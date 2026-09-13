#include "MyBotsTravel.h"

#include "MyBotsJob.h"
#include "MyBotsUtil.h"

#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"

// PlayerbotAI so we can delegate the actual hearthstone cast to the engine
// instead of re-implementing the cast/cooldown handling.
#include "Playerbots.h"

namespace
{
    constexpr uint32 HEARTHSTONE_ITEM = 6948;
    constexpr uint32 HEARTHSTONE_SPELL = 8690;

    // Fire the hearthstone. Prefer the playerbots "hearthstone" action (handles
    // stand-up, cast, and messaging); fall back to a direct cast if the AI is
    // unavailable for some reason.
    bool TriggerHearthstone(Player* player)
    {
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
}

void MyBotsTravel::Reset(MyBotsJob& job)
{
    job.travelDestMap = 0xFFFFFFFFu;
    job.travelStage = 0;
    job.travelActionAt = 0;
    job.travelStuckSince = 0;
    job.hearthCastAt = 0;
    job.travelLegSet = false;
}

MyBotsTravelResult MyBotsTravel::AdvanceCrossMap(Player* player, MyBotsJob& job,
    uint32 destMap, float /*x*/, float /*y*/, float /*z*/, std::string& detail)
{
    if (!player || !player->IsInWorld())
    {
        detail = "no_player";
        return MyBotsTravelResult::Unreachable;
    }

    // Already there — hand control back to the normal same-map pathing.
    if (player->GetMapId() == destMap)
    {
        detail = "cross_map_arrived";
        return MyBotsTravelResult::Arrived;
    }

    job.travelDestMap = destMap;
    uint32 const now = MyBotsNow();

    // A taxi flight or hearthstone teleport owns movement until it resolves.
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

    // Give a just-issued hearthstone time to channel + teleport before retrying.
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
            detail = "hearth_cast";
            LOG_DEBUG("module.mybots", "MyBots: {} hearthstones toward map {}",
                player->GetName(), destMap);
            return MyBotsTravelResult::Advancing;
        }
    }

    // Rules 2/3 (flight-master hops, inter-continent boat/zeppelin/portal) are
    // not wired yet. Crucially we do NOT fall back to a straight line across the
    // wrong map — that was the "walks off the map forever" bug. Fail cleanly so
    // the director can surface it and (later) an LLM replan can pick a route.
    detail = "cross_map_unreachable";
    return MyBotsTravelResult::Unreachable;
}
