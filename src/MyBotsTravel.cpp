#include "MyBotsTravel.h"

#include "MyBotsJob.h"
#include "MyBotsUtil.h"

#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"

#include "Playerbots.h"

#ifdef MYBOTS_HAVE_TRAVELMGR
#include "TravelMgr.h"
#endif

#include <cmath>
#include <limits>

namespace
{
    constexpr uint32 HEARTHSTONE_ITEM = 6948;
    constexpr uint32 HEARTHSTONE_SPELL = 8690;
    // How close we must get to a transfer boarding point before we wait for it.
    constexpr float TRANSFER_BOARD_DIST = 12.f;
    // Give AreaTrigger / boat loading this many seconds after arrival before
    // declaring the transfer stuck and trying another rule.
    constexpr uint32 TRANSFER_WAIT_SEC = 90;

    float Dist2d(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

#ifdef MYBOTS_HAVE_TRAVELMGR
    // Pick the cheapest TravelMgr mapTransfer between curMap and destMap for
    // the player's current position and the final destination. Returns false
    // when playerbots has no known boat/zeppelin/portal edge for this pair.
    bool FindBestTransfer(Player* player, uint32 destMap, float destX, float destY, float destZ,
        float& fromX, float& fromY, float& fromZ, float& toX, float& toY, float& toZ)
    {
        uint32 const curMap = player->GetMapId();
        auto it = sTravelMgr.mapTransfersMap.find({curMap, destMap});
        if (it == sTravelMgr.mapTransfersMap.end() || it->second.empty())
            return false;

        WorldPosition const start(player);
        WorldPosition const end(destMap, destX, destY, destZ);

        float best = std::numeric_limits<float>::max();
        mapTransfer* pick = nullptr;
        for (mapTransfer& t : it->second)
        {
            float const d = t.distance(start, end);
            if (d >= 199999.f)
                continue;
            if (d < best)
            {
                best = d;
                pick = &t;
            }
        }
        if (!pick)
            return false;

        WorldPosition* from = pick->getPointFrom();
        WorldPosition* to = pick->getPointTo();
        if (!from || !to)
            return false;

        fromX = from->GetPositionX();
        fromY = from->GetPositionY();
        fromZ = from->GetPositionZ();
        toX = to->GetPositionX();
        toY = to->GetPositionY();
        toZ = to->GetPositionZ();
        return true;
    }
#endif
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

    // Rule 2: walk to a TravelMgr mapTransfer boarding point (boat / zeppelin /
    // AreaTrigger portal), then wait for the transfer to fire.
#ifdef MYBOTS_HAVE_TRAVELMGR
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
        if (FindBestTransfer(player, destMap, x, y, z, fx, fy, fz, tx, ty, tz))
        {
            job.travelLegX = fx;
            job.travelLegY = fy;
            job.travelLegZ = fz;
            job.travelLegSet = true;
            job.travelStuckSince = 0;
            job.travelActionAt = now;
            detail = "transfer_approach";
            LOG_DEBUG("module.mybots",
                "MyBots: {} routes via transfer ({:.1f},{:.1f},{:.1f}) map {} -> {} ({:.1f},{:.1f})",
                player->GetName(), fx, fy, fz, player->GetMapId(), destMap, tx, ty);
            return MyBotsTravelResult::Advancing;
        }
    }
#else
    (void)x;
    (void)y;
    (void)z;
#endif

    // No hearth, no known transfer. Fail cleanly — never walk a straight line
    // across the wrong map (that was the "walks off the map forever" bug).
    detail = "cross_map_unreachable";
    return MyBotsTravelResult::Unreachable;
}
