#include "MyBotsTravel.h"

#include "MyBotsJob.h"
#include "MyBotsUtil.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "Player.h"
#include "QueryResult.h"
#include "SharedDefines.h"

#include "Playerbots.h"

#ifdef MYBOTS_HAVE_TRAVELMGR
#include "TravelMgr.h"
#endif

#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr uint32 HEARTHSTONE_ITEM = 6948;
    constexpr uint32 HEARTHSTONE_SPELL = 8690;
    // How close we must get to a transfer boarding point before we wait for it.
    constexpr float TRANSFER_BOARD_DIST = 12.f;
    // Give AreaTrigger / boat loading this many seconds after arrival before
    // declaring the transfer stuck and trying another rule.
    constexpr uint32 TRANSFER_WAIT_SEC = 90;

    // Overworld continents only. Skip instances, battlegrounds, and Deeprun Tram
    // so a "shortcut" never walks a level-23 into Maraudon on the way to Darnassus.
    bool IsContinentMap(uint32 mapId)
    {
        return mapId == 0 || mapId == 1 || mapId == 530 || mapId == 571;
    }

    float Dist2d(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    float Dist3(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        float const dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    std::string LowerCopy(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // 0 = both factions, 1 = Alliance, 2 = Horde.
    uint8 InferFaction(std::string const& name)
    {
        std::string const n = LowerCopy(name);
        if (n.find("horde") != std::string::npos || n.find("zeppelin") != std::string::npos)
            return 2;
        if (n.find("alliance") != std::string::npos || n.find("night elf") != std::string::npos
            || n.find("icebreaker") != std::string::npos || n.find("bravery") != std::string::npos
            || n.find("mehley") != std::string::npos || n.find("stormwind") != std::string::npos)
            return 1;
        return 0;
    }

    bool FactionOk(uint8 faction, Player const* player)
    {
        if (!faction)
            return true;
        TeamId const team = player->GetTeamId();
        if (faction == 1)
            return team == TEAM_ALLIANCE;
        return team == TEAM_HORDE;
    }

    struct ContinentTransfer
    {
        uint32 fromMap = 0;
        uint32 toMap = 0;
        float fromX = 0.f;
        float fromY = 0.f;
        float fromZ = 0.f;
        float toX = 0.f;
        float toY = 0.f;
        float toZ = 0.f;
        uint8 faction = 0;
        std::string name;
    };

    std::mutex g_transferMu;
    bool g_transfersAttempted = false;
    std::vector<ContinentTransfer> g_transfers;

    // playerbots TravelNodePathType: portal=2, transport=3. Skip walk(1) and
    // flightPath(4) — taxi is handled by MyBotsNav, not by standing on a dock.
    char const* kLoadSql =
        "SELECT a.map_id, a.x, a.y, a.z, a.name, b.map_id, b.x, b.y, b.z, b.name "
        "FROM playerbots_travelnode_link l "
        "JOIN playerbots_travelnode a ON a.id = l.node_id "
        "JOIN playerbots_travelnode b ON b.id = l.to_node_id "
        "WHERE a.map_id <> b.map_id AND l.type IN (2, 3)";

    bool FindBestTransfer(Player* player, uint32 destMap, float destX, float destY, float destZ,
        float& fromX, float& fromY, float& fromZ, float& toX, float& toY, float& toZ,
        std::string& viaName)
    {
        MyBotsTravel::EnsureTransfersLoaded();

        uint32 const curMap = player->GetMapId();
        float const px = player->GetPositionX();
        float const py = player->GetPositionY();
        float const pz = player->GetPositionZ();

        float best = std::numeric_limits<float>::max();
        ContinentTransfer const* pick = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_transferMu);
            for (ContinentTransfer const& t : g_transfers)
            {
                if (t.fromMap != curMap || t.toMap != destMap)
                    continue;
                if (!FactionOk(t.faction, player))
                    continue;
                float const d = Dist3(px, py, pz, t.fromX, t.fromY, t.fromZ) + 0.1f
                    + Dist3(t.toX, t.toY, t.toZ, destX, destY, destZ);
                if (d < best)
                {
                    best = d;
                    pick = &t;
                }
            }
            if (pick)
            {
                fromX = pick->fromX;
                fromY = pick->fromY;
                fromZ = pick->fromZ;
                toX = pick->toX;
                toY = pick->toY;
                toZ = pick->toZ;
                viaName = pick->name;
                return true;
            }
            // Playerbots no longer loads mapTransfers at startup. Only fall back
            // if our SQL load found nothing (older playerbots that still fills it).
            if (!g_transfers.empty())
                return false;
        }

#ifdef MYBOTS_HAVE_TRAVELMGR
        auto it = sTravelMgr.mapTransfersMap.find({curMap, destMap});
        if (it == sTravelMgr.mapTransfersMap.end() || it->second.empty())
            return false;

        WorldPosition const start(player);
        WorldPosition const end(destMap, destX, destY, destZ);

        float tmBest = std::numeric_limits<float>::max();
        mapTransfer* tmPick = nullptr;
        for (mapTransfer& t : it->second)
        {
            float const d = t.distance(start, end);
            if (d >= 199999.f)
                continue;
            if (d < tmBest)
            {
                tmBest = d;
                tmPick = &t;
            }
        }
        if (!tmPick)
            return false;

        WorldPosition* from = tmPick->getPointFrom();
        WorldPosition* to = tmPick->getPointTo();
        if (!from || !to)
            return false;

        fromX = from->GetPositionX();
        fromY = from->GetPositionY();
        fromZ = from->GetPositionZ();
        toX = to->GetPositionX();
        toY = to->GetPositionY();
        toZ = to->GetPositionZ();
        viaName.clear();
        return true;
#else
        return false;
#endif
    }

    void CountPairTransfers(uint32 fromMap, uint32 toMap, Player const* player,
        uint32& all, uint32& allowed)
    {
        all = 0;
        allowed = 0;
        std::lock_guard<std::mutex> lock(g_transferMu);
        for (ContinentTransfer const& t : g_transfers)
        {
            if (t.fromMap != fromMap || t.toMap != toMap)
                continue;
            ++all;
            if (FactionOk(t.faction, player))
                ++allowed;
        }
    }
}

void MyBotsTravel::EnsureTransfersLoaded()
{
    std::lock_guard<std::mutex> lock(g_transferMu);
    if (g_transfersAttempted)
        return;
    g_transfersAttempted = true;
    g_transfers.clear();

    QueryResult result = PlayerbotsDatabase.Query(kLoadSql);
    if (!result)
    {
        LOG_ERROR("module.mybots",
            "MyBots: no continent transfers (playerbots_travelnode query empty/failed)");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint32 const fromMap = fields[0].Get<uint32>();
        uint32 const toMap = fields[5].Get<uint32>();
        if (!IsContinentMap(fromMap) || !IsContinentMap(toMap))
            continue;

        ContinentTransfer t;
        t.fromMap = fromMap;
        t.fromX = fields[1].Get<float>();
        t.fromY = fields[2].Get<float>();
        t.fromZ = fields[3].Get<float>();
        t.name = fields[4].Get<std::string>();
        t.toMap = toMap;
        t.toX = fields[6].Get<float>();
        t.toY = fields[7].Get<float>();
        t.toZ = fields[8].Get<float>();
        t.faction = InferFaction(t.name);
        g_transfers.push_back(std::move(t));
    } while (result->NextRow());

    LOG_INFO("module.mybots",
        "MyBots: loaded {} continent transfers from playerbots_travelnode (boats/portals, no walk paths)",
        g_transfers.size());
}

bool MyBotsTravel::TriggerHearthstone(Player* player)
{
    if (!player)
        return false;
    if (player->GetItemCount(HEARTHSTONE_ITEM, false) == 0)
        return false;
    if (player->HasSpellCooldown(HEARTHSTONE_SPELL))
        return false;

    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        if (ai->DoSpecificAction("hearthstone"))
            return true;

    player->CastSpell(player, HEARTHSTONE_SPELL, false);
    return true;
}

void MyBotsTravel::Reset(MyBotsJob& job)
{
    job.travelDestMap = 0xFFFFFFFFu;
    job.travelStage = 0;
    job.travelActionAt = 0;
    job.travelStuckSince = 0;
    job.hearthCastAt = 0;
    job.travelLegSet = false;
    job.travelLegX = 0.f;
    job.travelLegY = 0.f;
    job.travelLegZ = 0.f;
}

MyBotsTravelResult MyBotsTravel::AdvanceCrossMap(Player* player, MyBotsJob& job,
    uint32 destMap, float x, float y, float z, std::string& detail)
{
    if (!player || !player->IsInWorld())
    {
        detail = "no_player";
        return MyBotsTravelResult::Unreachable;
    }

    if (player->GetMapId() == destMap)
    {
        detail = "cross_map_arrived";
        return MyBotsTravelResult::Arrived;
    }

    job.travelDestMap = destMap;
    uint32 const now = MyBotsNow();

    if (player->IsInFlight() || player->HasUnitFlag(UNIT_FLAG_TAXI_FLIGHT))
    {
        detail = "in_flight";
        return MyBotsTravelResult::Advancing;
    }
    if (player->IsInCombat())
    {
        detail = "cross_map_combat";
        return MyBotsTravelResult::Advancing;
    }
    if (player->HasUnitState(UNIT_STATE_CASTING))
    {
        detail = "hearth_casting";
        return MyBotsTravelResult::Advancing;
    }

    if (job.hearthCastAt && now - job.hearthCastAt < 15)
    {
        detail = "hearth_pending";
        return MyBotsTravelResult::Advancing;
    }

    // Rule 1: hearthstone home when the bind point is on the destination map.
    if (player->m_homebindMapId == destMap)
    {
        if (TriggerHearthstone(player))
        {
            job.hearthCastAt = now;
            job.travelLegSet = false;
            detail = "hearth_cast";
            LOG_DEBUG("module.mybots", "MyBots: {} hearthstones toward map {}",
                player->GetName(), destMap);
            return MyBotsTravelResult::Advancing;
        }
    }

    // Rule 2: walk to a boat / zeppelin / portal boarding point, then wait.
    if (job.travelLegSet)
    {
        float const d = Dist2d(player->GetPositionX(), player->GetPositionY(),
            job.travelLegX, job.travelLegY);
        if (d > TRANSFER_BOARD_DIST)
        {
            // Still walking to the dock / portal — executor will IssueMove.
            detail = "transfer_approach";
            return MyBotsTravelResult::Advancing;
        }

        // At the boarding point: AreaTriggers teleport on contact; boats need
        // the player to stand on the dock until the transport loads them. Wait
        // and let the map change; do not straight-line anywhere.
        if (!job.travelStuckSince)
            job.travelStuckSince = now;
        if (now - job.travelStuckSince < TRANSFER_WAIT_SEC)
        {
            detail = "transfer_waiting";
            return MyBotsTravelResult::Advancing;
        }

        // Timed out waiting for this transfer — clear and try picking again
        // (or fall through to unreachable). Mark stage so we do not immediately
        // re-pick the same dead dock forever.
        LOG_DEBUG("module.mybots", "MyBots: {} transfer wait timed out at ({:.1f},{:.1f})",
            player->GetName(), job.travelLegX, job.travelLegY);
        job.travelLegSet = false;
        job.travelStuckSince = 0;
        job.travelStage = 1; // "already tried transfer"
    }

    if (job.travelStage == 0)
    {
        float fx = 0.f, fy = 0.f, fz = 0.f, tx = 0.f, ty = 0.f, tz = 0.f;
        std::string via;
        if (FindBestTransfer(player, destMap, x, y, z, fx, fy, fz, tx, ty, tz, via))
        {
            job.travelLegX = fx;
            job.travelLegY = fy;
            job.travelLegZ = fz;
            job.travelLegSet = true;
            job.travelStuckSince = 0;
            job.travelActionAt = now;
            detail = via.empty() ? "transfer_approach" : ("transfer_approach;via=" + via);
            LOG_DEBUG("module.mybots",
                "MyBots: {} routes via {} ({:.1f},{:.1f},{:.1f}) map {} -> {} ({:.1f},{:.1f})",
                player->GetName(), via.empty() ? "transfer" : via.c_str(),
                fx, fy, fz, player->GetMapId(), destMap, tx, ty);
            return MyBotsTravelResult::Advancing;
        }
    }

    // No hearth, no known transfer. Fail cleanly — never walk a straight line
    // across the wrong map (that was the "walks off the map forever" bug).
    uint32 pairAll = 0, pairOk = 0;
    CountPairTransfers(player->GetMapId(), destMap, player, pairAll, pairOk);
    std::ostringstream oss;
    oss << "cross_map_unreachable;from=" << player->GetMapId()
        << ";to=" << destMap
        << ";hearth=" << player->m_homebindMapId
        << ";transfers=" << pairOk << "/" << pairAll;
    detail = oss.str();
    return MyBotsTravelResult::Unreachable;
}
