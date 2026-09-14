#include "MyBotsTravel.h"

#include "MyBotsJob.h"
#include "MyBotsNav.h"
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
        bool portal = false; // type 2 / name — same-map AreaTrigger (Darnassus tree)
        std::string name;
    };

    std::mutex g_transferMu;
    bool g_transfersAttempted = false;
    std::vector<ContinentTransfer> g_transfers;

    // playerbots TravelNodePathType: portal=2, transport=3. Skip walk(1) and
    // flightPath(4) — taxi is handled by MyBotsNav, not by standing on a dock.
    // Include same-map boats (Moonspray Auberdine↔Rut'theran) — filtered by span.
    char const* kLoadSql =
        "SELECT a.map_id, a.x, a.y, a.z, a.name, b.map_id, b.x, b.y, b.z, b.name, l.type "
        "FROM playerbots_travelnode_link l "
        "JOIN playerbots_travelnode a ON a.id = l.node_id "
        "JOIN playerbots_travelnode b ON b.id = l.to_node_id "
        "WHERE l.type IN (2, 3) "
        "AND a.map_id IN (0, 1, 530, 571) AND b.map_id IN (0, 1, 530, 571)";

    // Minimum 2d span for same-map boats/portals (skip tiny dock links).
    constexpr float kMinSameMapSpan = 200.f;
    // Teldrassil / similar: prefer portal when destination is far above/below us.
    constexpr float kPortalZGap = 200.f;

    bool NameLooksPortal(std::string const& name)
    {
        std::string const n = LowerCopy(name);
        return n.find("portal") != std::string::npos;
    }

    bool FindBestTransfer(Player* player, uint32 destMap, float destX, float destY, float destZ,
        float& fromX, float& fromY, float& fromZ, float& toX, float& toY, float& toZ,
        std::string& viaName, bool* outPortal = nullptr);

    // Rut'theran → Darnassus: huge Z gap, walk into the pink portal under the tree
    // instead of pathfinding up the trunk. Only when we are already near the pad
    // (taxi/boat must land us first — do not walk across the Darkshore sea to it).
    bool PortalClimbHelps(Player* player, float destX, float destY, float destZ)
    {
        if (!player)
            return false;
        float const zGap = std::fabs(destZ - player->GetPositionZ());
        if (zGap < kPortalZGap)
            return false;

        float fx = 0.f, fy = 0.f, fz = 0.f, tx = 0.f, ty = 0.f, tz = 0.f;
        std::string via;
        bool portal = false;
        if (!FindBestTransfer(player, player->GetMapId(), destX, destY, destZ,
                fx, fy, fz, tx, ty, tz, via, &portal))
            return false;
        if (!portal)
            return false;
        float const toEntry = Dist2d(player->GetPositionX(), player->GetPositionY(), fx, fy);
        if (toEntry > 600.f)
            return false;
        // Entry should be near our elevation; exit near the destination floor.
        if (std::fabs(fz - player->GetPositionZ()) > 120.f)
            return false;
        if (std::fabs(tz - destZ) > 250.f
            && Dist2d(tx, ty, destX, destY) >= Dist2d(player->GetPositionX(), player->GetPositionY(), destX, destY) * 0.6f)
            return false;
        return true;
    }

    bool LocalTransferHelps(Player* player, float destX, float destY, float destZ)
    {
        if (!player)
            return false;
        if (PortalClimbHelps(player, destX, destY, destZ))
            return true;

        float const direct = Dist2d(player->GetPositionX(), player->GetPositionY(), destX, destY);
        if (direct < 350.f)
            return false;

        float fx = 0.f, fy = 0.f, fz = 0.f, tx = 0.f, ty = 0.f, tz = 0.f;
        std::string via;
        if (!FindBestTransfer(player, player->GetMapId(), destX, destY, destZ,
                fx, fy, fz, tx, ty, tz, via))
            return false;

        float const toEntry = Dist2d(player->GetPositionX(), player->GetPositionY(), fx, fy);
        float const exitToDest = Dist2d(tx, ty, destX, destY);
        float const viaCost = toEntry + exitToDest;
        if (exitToDest >= direct * 0.7f)
            return false;
        if (viaCost >= direct * 0.9f)
            return false;
        if (toEntry > direct * 0.55f)
            return false;
        return true;
    }

    bool FindBestTransfer(Player* player, uint32 destMap, float destX, float destY, float destZ,
        float& fromX, float& fromY, float& fromZ, float& toX, float& toY, float& toZ,
        std::string& viaName, bool* outPortal)
    {
        MyBotsTravel::EnsureTransfersLoaded();
        if (outPortal)
            *outPortal = false;

        uint32 const curMap = player->GetMapId();
        float const px = player->GetPositionX();
        float const py = player->GetPositionY();
        float const pz = player->GetPositionZ();
        float const zGap = std::fabs(destZ - pz);
        bool const preferPortal = (curMap == destMap && zGap >= kPortalZGap);

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
                if (preferPortal && !t.portal)
                    continue;
                if (preferPortal)
                {
                    // Must bridge our floor to the destination floor.
                    if (std::fabs(t.fromZ - pz) > 120.f)
                        continue;
                    if (std::fabs(t.toZ - destZ) > 250.f)
                        continue;
                }
                float d = Dist3(px, py, pz, t.fromX, t.fromY, t.fromZ) + 0.1f
                    + Dist3(t.toX, t.toY, t.toZ, destX, destY, destZ);
                if (t.portal && preferPortal)
                    d *= 0.35f; // strongly prefer the tree portal over any boat leftover
                if (d < best)
                {
                    best = d;
                    pick = &t;
                }
            }
            // If Z-gap portal filter found nothing, fall back to any transfer.
            if (!pick && preferPortal)
            {
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
                if (outPortal)
                    *outPortal = pick->portal;
                return true;
            }
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
        std::string const toName = fields[9].Get<std::string>();
        uint8 const linkType = fields[10].Get<uint8>();
        t.portal = (linkType == 2) || NameLooksPortal(t.name) || NameLooksPortal(toName);
        // Keep portal in the via label so logs show why we walked under the tree.
        if (t.portal && t.name.find("portal") == std::string::npos
            && toName.find("portal") != std::string::npos)
            t.name = toName;
        if (fromMap == toMap)
        {
            float const span = Dist2d(t.fromX, t.fromY, t.toX, t.toY);
            // Portals often have large Z and moderate XY (Teldrassil trunk);
            // boats need a long 2d hop or they are dock fluff.
            float const minSpan = t.portal ? 80.f : kMinSameMapSpan;
            if (span < minSpan)
                continue;
        }
        t.faction = InferFaction(t.name);
        g_transfers.push_back(std::move(t));
    } while (result->NextRow());

    LOG_INFO("module.mybots",
        "MyBots: loaded {} continent transfers from playerbots_travelnode (boats/portals, no walk paths)",
        g_transfers.size());
}

bool MyBotsTravel::LocalBoatHelps(Player* player, float x, float y, float z)
{
    // Tree portals (huge Z gap) beat climbing even when taxi is enabled — the
    // bird may drop you at Rut'theran, still below Darnassus.
    if (PortalClimbHelps(player, x, y, z))
        return true;
    // Prefer flight paths for long flat hops; boat only when taxi is not in play.
    if (MyBotsNav::ShouldUseTaxi(player, x, y, z))
        return false;
    return LocalTransferHelps(player, x, y, z);
}

namespace
{
    bool PortalExitReached(Player* player, MyBotsJob const& job)
    {
        if (!player || !job.travelDestRawSet)
            return false;
        float const d2 = Dist2d(player->GetPositionX(), player->GetPositionY(),
            job.travelDestRawX, job.travelDestRawY);
        float const dz = std::fabs(player->GetPositionZ() - job.travelDestRawZ);
        if (d2 < 55.f && dz < 120.f)
            return true;
        if (dz < 80.f && d2 < 120.f && job.travelRawSet
            && std::fabs(player->GetPositionZ() - job.travelDestRawZ)
                < std::fabs(job.travelRawZ - job.travelDestRawZ) * 0.35f)
            return true;
        return false;
    }
}

MyBotsTravelResult MyBotsTravel::AdvanceLocalTransfer(Player* player, MyBotsJob& job,
    float x, float y, float z, std::string& detail)
{
    if (!player || !player->IsInWorld())
    {
        detail = "no_player";
        return MyBotsTravelResult::Unreachable;
    }

    uint32 const now = MyBotsNow();
    uint32 const curMap = player->GetMapId();
    job.travelDestMap = curMap;

    bool const onTransport = player->GetTransport() != nullptr
        || player->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);

    if (job.travelIsPortal && job.travelDestRawSet && PortalExitReached(player, job))
    {
        detail = "local_portal_arrived";
        return MyBotsTravelResult::Arrived;
    }

    if (player->IsInFlight() || player->HasUnitFlag(UNIT_FLAG_TAXI_FLIGHT))
    {
        detail = "in_flight";
        return MyBotsTravelResult::Advancing;
    }

    if (onTransport)
    {
        job.travelSawTransport = true;
        player->StopMoving();
        job.travelStuckSince = 0;
        job.travelLegSet = false;
        job.travelRawSet = false;
        job.travelDockReady = false;
        detail = "transfer_aboard";
        return MyBotsTravelResult::Advancing;
    }

    if (job.travelSawTransport && !job.travelIsPortal && job.travelDestRawSet)
    {
        float const dExit = Dist2d(player->GetPositionX(), player->GetPositionY(),
            job.travelDestRawX, job.travelDestRawY);
        bool const inWater = player->isSwimming() || player->IsInWater()
            || MyBotsNav::IsDeepWaterAt(player, player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ());
        if (inWater || dExit < 80.f)
        {
            if (!job.travelDockResolveAt || now - job.travelDockResolveAt >= 2 || inWater
                || !job.travelLegSet)
            {
                job.travelDockResolveAt = now;
                float dx = job.travelDestRawX, dy = job.travelDestRawY, dz = job.travelDestRawZ;
                if (MyBotsNav::ResolveBoardingDock(player, dx, dy, dz))
                {
                    job.travelLegX = dx;
                    job.travelLegY = dy;
                    job.travelLegZ = dz;
                    job.travelLegSet = true;
                }
            }
            float const d = Dist2d(player->GetPositionX(), player->GetPositionY(),
                job.travelLegX, job.travelLegY);
            if (job.travelLegSet && (inWater || d > TRANSFER_BOARD_DIST))
            {
                detail = "transfer_disembark";
                return MyBotsTravelResult::Advancing;
            }
            if (!inWater && job.travelLegSet && d <= TRANSFER_BOARD_DIST)
            {
                detail = "local_boat_arrived";
                return MyBotsTravelResult::Arrived;
            }
            detail = "transfer_disembark";
            return MyBotsTravelResult::Advancing;
        }
        detail = "local_boat_arrived";
        return MyBotsTravelResult::Arrived;
    }

    if (player->IsInCombat())
    {
        detail = "cross_map_combat";
        return MyBotsTravelResult::Advancing;
    }

    if (job.travelLegSet)
    {
        if (job.travelRawSet && !job.travelIsPortal)
        {
            float const dShip = Dist2d(player->GetPositionX(), player->GetPositionY(),
                job.travelRawX, job.travelRawY);
            float const dLegShip = Dist2d(job.travelLegX, job.travelLegY,
                job.travelRawX, job.travelRawY);
            bool const legOnShip = dLegShip < 16.f;
            bool const legWet = MyBotsNav::IsDeepWaterAt(player, job.travelLegX, job.travelLegY, job.travelLegZ)
                || MyBotsNav::IsDeepWaterAt(player, job.travelLegX, job.travelLegY, job.travelLegZ + 1.f);
            bool const underHull = dShip < 48.f
                && player->GetPositionZ() < job.travelLegZ - 2.5f;
            bool const tryResolve = underHull || legOnShip || legWet || !job.travelDockReady;
            bool const due = !job.travelDockResolveAt || now - job.travelDockResolveAt >= 3
                || underHull;

            if (tryResolve && due)
            {
                job.travelDockResolveAt = now;
                float dx = job.travelRawX, dy = job.travelRawY, dz = job.travelRawZ;
                if (MyBotsNav::ResolveBoardingDock(player, dx, dy, dz))
                {
                    job.travelLegX = dx;
                    job.travelLegY = dy;
                    job.travelLegZ = dz;
                    float const dNew = Dist2d(dx, dy, job.travelRawX, job.travelRawY);
                    if (dNew >= 16.f && dNew <= 65.f
                        && !MyBotsNav::IsDeepWaterAt(player, dx, dy, dz))
                        job.travelDockReady = true;
                }
            }

            if (underHull)
            {
                MyBotsNav::MarkBadPoint(player, player->GetPositionX(), player->GetPositionY(),
                    player->GetPositionZ());
                player->UpdatePosition(job.travelLegX, job.travelLegY, job.travelLegZ,
                    player->GetOrientation(), true);
                player->StopMoving();
            }
        }

        float const d = Dist2d(player->GetPositionX(), player->GetPositionY(),
            job.travelLegX, job.travelLegY);
        if (d > TRANSFER_BOARD_DIST)
        {
            detail = job.travelIsPortal ? "transfer_portal" : "transfer_approach";
            return MyBotsTravelResult::Advancing;
        }

        float const dRaw = job.travelRawSet
            ? Dist2d(player->GetPositionX(), player->GetPositionY(), job.travelRawX, job.travelRawY)
            : d;
        if (dRaw > 70.f)
        {
            detail = job.travelIsPortal ? "transfer_portal" : "transfer_approach";
            return MyBotsTravelResult::Advancing;
        }

        if (job.travelIsPortal)
        {
            job.travelLegX = job.travelRawX;
            job.travelLegY = job.travelRawY;
            job.travelLegZ = job.travelRawZ;
            if (!job.travelStuckSince)
                job.travelStuckSince = now;
            if (now - job.travelStuckSince < TRANSFER_WAIT_SEC)
            {
                detail = "transfer_portal";
                return MyBotsTravelResult::Advancing;
            }
            LOG_DEBUG("module.mybots", "MyBots: {} portal wait timed out at ({:.1f},{:.1f})",
                player->GetName(), job.travelLegX, job.travelLegY);
            job.travelLegSet = false;
            job.travelRawSet = false;
            job.travelDockReady = false;
            job.travelStuckSince = 0;
            job.travelStage = 1;
            job.travelIsPortal = false;
        }
        else
        {
            player->StopMoving();
            if (!job.travelStuckSince)
                job.travelStuckSince = now;
            if (now - job.travelStuckSince < TRANSFER_WAIT_SEC)
            {
                detail = "transfer_waiting";
                return MyBotsTravelResult::Advancing;
            }
            LOG_DEBUG("module.mybots", "MyBots: {} local boat wait timed out at ({:.1f},{:.1f})",
                player->GetName(), job.travelLegX, job.travelLegY);
            job.travelLegSet = false;
            job.travelRawSet = false;
            job.travelDockReady = false;
            job.travelStuckSince = 0;
            job.travelStage = 1;
        }
    }

    if (job.travelStage == 0)
    {
        float fx = 0.f, fy = 0.f, fz = 0.f, tx = 0.f, ty = 0.f, tz = 0.f;
        std::string via;
        bool portal = false;
        if (FindBestTransfer(player, curMap, x, y, z, fx, fy, fz, tx, ty, tz, via, &portal))
        {
            job.travelIsPortal = portal;
            job.travelRawX = fx;
            job.travelRawY = fy;
            job.travelRawZ = fz;
            job.travelRawSet = true;
            job.travelDestRawX = tx;
            job.travelDestRawY = ty;
            job.travelDestRawZ = tz;
            job.travelDestRawSet = true;
            job.travelDockReady = portal;
            job.travelDockResolveAt = now;
            job.travelLegX = fx;
            job.travelLegY = fy;
            job.travelLegZ = fz;
            if (!portal)
            {
                MyBotsNav::ResolveBoardingDock(player, fx, fy, fz);
                job.travelLegX = fx;
                job.travelLegY = fy;
                job.travelLegZ = fz;
                float const dNew = Dist2d(fx, fy, job.travelRawX, job.travelRawY);
                if (dNew >= 16.f && dNew <= 65.f
                    && !MyBotsNav::IsDeepWaterAt(player, fx, fy, fz))
                    job.travelDockReady = true;
            }
            job.travelLegSet = true;
            job.travelStuckSince = 0;
            job.travelActionAt = now;
            job.travelSawTransport = false;
            if (portal)
                detail = via.empty() ? "transfer_portal" : ("transfer_portal;via=" + via);
            else
                detail = via.empty() ? "transfer_approach" : ("transfer_approach;via=" + via);
            LOG_DEBUG("module.mybots",
                "MyBots: {} local {} via {} ({:.1f},{:.1f},{:.1f}) -> ({:.1f},{:.1f},{:.1f})",
                player->GetName(), portal ? "portal" : "boat",
                via.empty() ? "?" : via.c_str(),
                job.travelRawX, job.travelRawY, job.travelRawZ, tx, ty, tz);
            return MyBotsTravelResult::Advancing;
        }
    }

    detail = "local_transfer_unreachable";
    return MyBotsTravelResult::Unreachable;
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
    job.travelRawSet = false;
    job.travelRawX = 0.f;
    job.travelRawY = 0.f;
    job.travelRawZ = 0.f;
    job.travelDockReady = false;
    job.travelDockResolveAt = 0;
    job.travelDestRawSet = false;
    job.travelDestRawX = 0.f;
    job.travelDestRawY = 0.f;
    job.travelDestRawZ = 0.f;
    job.travelSawTransport = false;
    job.travelIsPortal = false;
}

MyBotsTravelResult MyBotsTravel::AdvanceCrossMap(Player* player, MyBotsJob& job,
    uint32 destMap, float x, float y, float z, std::string& detail)
{
    if (!player || !player->IsInWorld())
    {
        detail = "no_player";
        return MyBotsTravelResult::Unreachable;
    }

    job.travelDestMap = destMap;
    uint32 const now = MyBotsNow();
    bool const onTransport = player->GetTransport() != nullptr
        || player->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);

    // Already on the destination continent: stay put while the boat finishes,
    // then step onto a dry exit pier before handing control back to same-map nav.
    if (player->GetMapId() == destMap)
    {
        if (onTransport)
        {
            player->StopMoving();
            job.travelStuckSince = 0;
            // Departure pier XYZ is on the old map — do not keep walking to it.
            job.travelLegSet = false;
            job.travelRawSet = false;
            job.travelDockReady = false;
            detail = "transfer_aboard";
            return MyBotsTravelResult::Advancing;
        }

        if (job.travelDestRawSet)
        {
            float const dExitShip = Dist2d(player->GetPositionX(), player->GetPositionY(),
                job.travelDestRawX, job.travelDestRawY);
            bool const inWater = player->isSwimming() || player->IsInWater()
                || MyBotsNav::IsDeepWaterAt(player, player->GetPositionX(), player->GetPositionY(),
                    player->GetPositionZ());
            // Still near the arrival boat / in the harbor — get onto the pier first.
            if (inWater || dExitShip < 80.f)
            {
                bool resolved = false;
                if (!job.travelDockResolveAt || now - job.travelDockResolveAt >= 2 || inWater
                    || !job.travelLegSet)
                {
                    job.travelDockResolveAt = now;
                    float dx = job.travelDestRawX, dy = job.travelDestRawY, dz = job.travelDestRawZ;
                    if (MyBotsNav::ResolveBoardingDock(player, dx, dy, dz))
                    {
                        job.travelLegX = dx;
                        job.travelLegY = dy;
                        job.travelLegZ = dz;
                        job.travelLegSet = true;
                        resolved = true;
                    }
                }
                else
                    resolved = job.travelLegSet;

                float const d = Dist2d(player->GetPositionX(), player->GetPositionY(),
                    job.travelLegX, job.travelLegY);
                if (resolved && (inWater || d > TRANSFER_BOARD_DIST))
                {
                    detail = "transfer_disembark";
                    return MyBotsTravelResult::Advancing;
                }
                if (inWater)
                {
                    player->StopMoving();
                    detail = "transfer_disembark";
                    return MyBotsTravelResult::Advancing;
                }
            }
        }

        detail = "cross_map_arrived";
        return MyBotsTravelResult::Arrived;
    }

    if (player->IsInFlight() || player->HasUnitFlag(UNIT_FLAG_TAXI_FLIGHT))
    {
        detail = "in_flight";
        return MyBotsTravelResult::Advancing;
    }

    // Boat / zeppelin already moving (or player boarded): freeze pathing. Issuing
    // MovePoint toward the departure dock walks them back through the hull.
    if (onTransport)
    {
        player->StopMoving();
        job.travelStuckSince = 0;
        detail = "transfer_aboard";
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
            job.travelRawSet = false;
            job.travelDockReady = false;
            detail = "hearth_cast";
            LOG_DEBUG("module.mybots", "MyBots: {} hearthstones toward map {}",
                player->GetName(), destMap);
            return MyBotsTravelResult::Advancing;
        }
    }

    // Rule 2: walk to a boat / zeppelin / portal boarding point, then wait.
    if (job.travelLegSet)
    {
        if (job.travelRawSet)
        {
            float const dShip = Dist2d(player->GetPositionX(), player->GetPositionY(),
                job.travelRawX, job.travelRawY);
            float const dLegShip = Dist2d(job.travelLegX, job.travelLegY,
                job.travelRawX, job.travelRawY);
            bool const legOnShip = dLegShip < 16.f;
            bool const legWet = MyBotsNav::IsDeepWaterAt(player, job.travelLegX, job.travelLegY, job.travelLegZ)
                || MyBotsNav::IsDeepWaterAt(player, job.travelLegX, job.travelLegY, job.travelLegZ + 1.f);
            bool const underHull = dShip < 48.f
                && player->GetPositionZ() < job.travelLegZ - 2.5f;
            bool const tryResolve = underHull || legOnShip || legWet || !job.travelDockReady;
            bool const due = !job.travelDockResolveAt || now - job.travelDockResolveAt >= 3
                || underHull;

            if (tryResolve && due)
            {
                job.travelDockResolveAt = now;
                float dx = job.travelRawX, dy = job.travelRawY, dz = job.travelRawZ;
                if (MyBotsNav::ResolveBoardingDock(player, dx, dy, dz))
                {
                    job.travelLegX = dx;
                    job.travelLegY = dy;
                    job.travelLegZ = dz;
                    float const dNew = Dist2d(dx, dy, job.travelRawX, job.travelRawY);
                    if (dNew >= 16.f && dNew <= 65.f
                        && !MyBotsNav::IsDeepWaterAt(player, dx, dy, dz))
                        job.travelDockReady = true;
                }
            }

            if (underHull)
            {
                MyBotsNav::MarkBadPoint(player, player->GetPositionX(), player->GetPositionY(),
                    player->GetPositionZ());
                player->UpdatePosition(job.travelLegX, job.travelLegY, job.travelLegZ,
                    player->GetOrientation(), true);
                player->StopMoving();
                LOG_DEBUG("module.mybots",
                    "MyBots: {} lifted from under transport onto dock ({:.1f},{:.1f},{:.1f})",
                    player->GetName(), job.travelLegX, job.travelLegY, job.travelLegZ);
            }
        }

        float const d = Dist2d(player->GetPositionX(), player->GetPositionY(),
            job.travelLegX, job.travelLegY);
        if (d > TRANSFER_BOARD_DIST)
        {
            // Still walking to the dock / portal — executor will IssueMove.
            detail = "transfer_approach";
            return MyBotsTravelResult::Advancing;
        }

        // Only start the dock wait when we are near the *raw* boarding node
        // (or a dry pier next to it), not an inland holding point mid-city.
        float const dRaw = job.travelRawSet
            ? Dist2d(player->GetPositionX(), player->GetPositionY(), job.travelRawX, job.travelRawY)
            : d;
        if (dRaw > 70.f)
        {
            detail = "transfer_approach";
            return MyBotsTravelResult::Advancing;
        }

        // At the boarding point: AreaTriggers teleport on contact; boats need
        // the player to stand on the dock until the transport loads them. Wait
        // and let the map change; do not straight-line anywhere.
        player->StopMoving();
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
        job.travelRawSet = false;
        job.travelDockReady = false;
        job.travelStuckSince = 0;
        job.travelStage = 1; // "already tried transfer"
    }

    if (job.travelStage == 0)
    {
        float fx = 0.f, fy = 0.f, fz = 0.f, tx = 0.f, ty = 0.f, tz = 0.f;
        std::string via;
        bool portal = false;
        if (FindBestTransfer(player, destMap, x, y, z, fx, fy, fz, tx, ty, tz, via, &portal))
        {
            job.travelIsPortal = portal;
            job.travelRawX = fx;
            job.travelRawY = fy;
            job.travelRawZ = fz;
            job.travelRawSet = true;
            job.travelDestRawX = tx;
            job.travelDestRawY = ty;
            job.travelDestRawZ = tz;
            job.travelDestRawSet = true;
            job.travelDockReady = portal;
            job.travelDockResolveAt = now;
            job.travelLegX = fx;
            job.travelLegY = fy;
            job.travelLegZ = fz;
            if (!portal)
            {
                MyBotsNav::ResolveBoardingDock(player, fx, fy, fz);
                job.travelLegX = fx;
                job.travelLegY = fy;
                job.travelLegZ = fz;
                float const dNew = Dist2d(fx, fy, job.travelRawX, job.travelRawY);
                if (dNew >= 16.f && dNew <= 65.f
                    && !MyBotsNav::IsDeepWaterAt(player, fx, fy, fz))
                    job.travelDockReady = true;
            }
            job.travelLegSet = true;
            job.travelStuckSince = 0;
            job.travelActionAt = now;
            if (portal)
                detail = via.empty() ? "transfer_portal" : ("transfer_portal;via=" + via);
            else
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
