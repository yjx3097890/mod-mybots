#include "MyBotsExecutor.h"
#include "MyBotsConfig.h"
#include "MyBotsJob.h"
#include "MyBotsNav.h"
#include "MyBotsQuestPlan.h"
#include "MyBotsSelfbot.h"
#include "MyBotsUtil.h"

#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "SharedDefines.h"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace
{
bool TryDoAction(PlayerbotAI* ai, std::string const& name)
{
    if (!ai)
        return false;
    return ai->DoSpecificAction(name);
}

Creature* FindNearestCreature(Player* player, uint32 entry, float range)
{
    return player ? player->FindNearestCreature(entry, range, true) : nullptr;
}

// Grid search only sees loaded creatures. For anything further away we fall
// back to the static spawn table so a long trip can at least be started.
bool FindNearestSpawnPoint(Player* player, uint32 entry, float& x, float& y, float& z)
{
    if (!player || !entry)
        return false;

    uint16 const mapId = uint16(player->GetMapId());
    float best = -1.f;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != mapId || data.id != entry)
            continue;

        float const d = player->GetExactDist2d(data.posX, data.posY);
        if (best < 0.f || d < best)
        {
            best = d;
            x = data.posX;
            y = data.posY;
            z = data.posZ;
        }
    }

    return best >= 0.f;
}

bool ParseUIntArray(std::string const& detail, char const* key, std::vector<uint32>& out)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = detail.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = detail.find('[', pos);
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < detail.size() && detail[pos] != ']')
    {
        while (pos < detail.size() && (detail[pos] == ' ' || detail[pos] == ','))
            ++pos;
        if (pos >= detail.size() || detail[pos] == ']')
            break;
        char* end = nullptr;
        unsigned long v = std::strtoul(detail.c_str() + pos, &end, 10);
        if (end == detail.c_str() + pos)
            break;
        out.push_back(static_cast<uint32>(v));
        pos = static_cast<size_t>(end - detail.c_str());
    }
    return !out.empty();
}

std::vector<uint32> ParseUIntArray(std::string const& detail, char const* key)
{
    std::vector<uint32> out;
    ParseUIntArray(detail, key, out);
    return out;
}

void ResetNavState(MyBotsJob& job)
{
    job.stuckSince = 0;
    job.navAttempts = 0;
    job.detourUntil = 0;
    job.moveIssuedAt = 0;
    job.taxiInProgress = false;
}

// Clearing the motion master every tick restarts pathfinding and makes the
// character stutter, so only re-issue when the target moved, the generator
// dropped out, or the re-path interval elapsed.
void IssueMove(Player* player, MyBotsJob& job, float x, float y, float z, bool force)
{
    uint32 const now = MyBotsNow();

    // Pull the character out of the mesh if a previous bad destination sank them.
    MyBotsNav::CorrectIfUnderground(player);

    // Resolve walkable XYZ via mmap (Playerbots-style). Do NOT use Map::GetHeight
    // from the sky — that is what snapped us onto cave floors.
    if (!MyBotsNav::PrepareWalkTarget(player, x, y, z))
    {
        LOG_DEBUG("module.mybots", "MyBots: no walkable path for {} toward ({:.1f},{:.1f},{:.1f})",
            player->GetName(), x, y, z);
        return;
    }

    bool const sameTarget = std::fabs(job.moveTargetX - x) < 1.f
        && std::fabs(job.moveTargetY - y) < 1.f
        && std::fabs(job.moveTargetZ - z) < 1.f;
    bool const driving = player->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

    if (!force && sameTarget && driving && job.moveIssuedAt
        && now - job.moveIssuedAt < sMyBotsConfig.NavRepathSec())
        return;

    job.moveTargetX = x;
    job.moveTargetY = y;
    job.moveTargetZ = z;
    job.moveIssuedAt = now;

    // Same flags Playerbots DoMovePoint uses: generatePath, never forceDestination.
    player->GetMotionMaster()->Clear();
    player->GetMotionMaster()->MovePoint(1, x, y, z, FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
}
} // namespace

bool MyBotsExecutor::ParseMoveXYZ(std::string const& detail, float& x, float& y, float& z)
{
    return ParseFloat(detail, "x", x) && ParseFloat(detail, "y", y) && ParseFloat(detail, "z", z);
}

bool MyBotsExecutor::ParseUInt(std::string const& detail, char const* key, uint32& out)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = detail.find(needle);
    if (pos == std::string::npos)
    {
        needle = std::string(key) + "=";
        pos = detail.find(needle);
        if (pos == std::string::npos)
            return false;
        pos += needle.size();
    }
    else
    {
        pos = detail.find(':', pos);
        if (pos == std::string::npos)
            return false;
        ++pos;
    }
    while (pos < detail.size() && (detail[pos] == ' ' || detail[pos] == '\"'))
        ++pos;
    out = static_cast<uint32>(std::strtoul(detail.c_str() + pos, nullptr, 10));
    return true;
}

bool MyBotsExecutor::ParseFloat(std::string const& detail, char const* key, float& out)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = detail.find(needle);
    if (pos == std::string::npos)
    {
        needle = std::string(key) + "=";
        pos = detail.find(needle);
        if (pos == std::string::npos)
            return false;
        pos += needle.size();
    }
    else
    {
        pos = detail.find(':', pos);
        if (pos == std::string::npos)
            return false;
        ++pos;
    }
    while (pos < detail.size() && (detail[pos] == ' ' || detail[pos] == '\"'))
        ++pos;
    out = std::strtof(detail.c_str() + pos, nullptr);
    return true;
}

MyBotsStepOutcome MyBotsExecutor::EnsureSelfbot(Player* player)
{
    MyBotsStepOutcome o;
    if (GET_PLAYERBOT_AI(player))
    {
        o.result = MyBotsStepResult::Done;
        o.detail = "selfbot_ready";
        return o;
    }
    auto r = MyBotsSelfbot::Enable(player);
    o.result = r.ok ? MyBotsStepResult::Done : MyBotsStepResult::Failed;
    o.detail = r.message;
    // Jobs always suppress autonomous RPG so director owns control.
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        ai->ChangeStrategy("-rpg quest,-travel,-rpg,-new rpg,-grind,-follow", BOT_STATE_NON_COMBAT);
    return o;
}

MyBotsStepOutcome MyBotsExecutor::MoveTo(Player* player, MyBotsJob& job, float x, float y, float z, float dist)
{
    MyBotsStepOutcome o;
    if (!player)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "no_player";
        return o;
    }

    uint32 const now = MyBotsNow();

    // A taxi flight owns movement until the character lands.
    if (player->IsInFlight())
    {
        job.stuckSince = 0;
        job.taxiInProgress = true;
        o.result = MyBotsStepResult::Running;
        o.detail = "in_flight";
        return o;
    }

    if (job.taxiInProgress)
    {
        job.taxiInProgress = false;
        job.stuckSince = 0;
        job.moveIssuedAt = 0;
    }

    if (player->GetDistance(x, y, z) <= dist)
    {
        player->StopMoving();
        ResetNavState(job);
        o.result = MyBotsStepResult::Done;
        o.detail = "arrived";
        return o;
    }

    if (player->IsInCombat())
    {
        // Combat generators own movement here, so standing still is not "stuck".
        job.stuckSince = 0;
        job.moveIssuedAt = 0;
        job.lastX = player->GetPositionX();
        job.lastY = player->GetPositionY();
        job.lastZ = player->GetPositionZ();
        o.result = MyBotsStepResult::Running;
        o.detail = "in_combat";
        return o;
    }

    float const moved = std::fabs(player->GetPositionX() - job.lastX)
        + std::fabs(player->GetPositionY() - job.lastY)
        + std::fabs(player->GetPositionZ() - job.lastZ);
    if (moved < 0.4f)
    {
        if (!job.stuckSince)
            job.stuckSince = now;
    }
    else
        job.stuckSince = 0;

    job.lastX = player->GetPositionX();
    job.lastY = player->GetPositionY();
    job.lastZ = player->GetPositionZ();

    bool const stuck = job.stuckSince && now - job.stuckSince >= sMyBotsConfig.StuckTimeoutSec();

    // Finish the current side offset before aiming at the real target again.
    if (job.detourUntil)
    {
        if (now >= job.detourUntil || player->GetDistance(job.detourX, job.detourY, job.detourZ) <= 3.f)
        {
            job.detourUntil = 0;
            job.stuckSince = 0;
            job.moveIssuedAt = 0;
        }
        else if (!stuck)
        {
            IssueMove(player, job, job.detourX, job.detourY, job.detourZ, false);
            o.result = MyBotsStepResult::Running;
            o.detail = "detour";
            return o;
        }
    }

    if (stuck)
    {
        job.stuckSince = 0;
        job.detourUntil = 0;
        ++job.navAttempts;
        MyBotsNav::MarkBadPoint(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());

        if (job.navAttempts > sMyBotsConfig.NavMaxStuckRetries())
        {
            player->StopMoving();
            o.result = MyBotsStepResult::Failed;
            o.detail = "stuck";
            return o;
        }

        float dx = 0.f, dy = 0.f, dz = 0.f;
        if (MyBotsNav::ComputeDetour(player, x, y, z, job.navAttempts, dx, dy, dz))
        {
            job.detourX = dx;
            job.detourY = dy;
            job.detourZ = dz;
            job.detourUntil = now + sMyBotsConfig.NavDetourSec();
            IssueMove(player, job, dx, dy, dz, true);
            o.result = MyBotsStepResult::Running;
            o.detail = "detour_retry";
            return o;
        }

        IssueMove(player, job, x, y, z, true);
        o.result = MyBotsStepResult::Running;
        o.detail = "repath";
        return o;
    }

    // Long hops: let a flight path cover the continent instead of the navmesh.
    if (now >= job.taxiRetryAt && MyBotsNav::ShouldUseTaxi(player, x, y, z))
    {
        float bx = 0.f, by = 0.f, bz = 0.f;
        std::string taxiDetail;
        switch (MyBotsNav::TryTaxi(player, x, y, z, bx, by, bz, taxiDetail))
        {
            case MyBotsTaxiResult::Boarded:
                job.taxiInProgress = true;
                job.moveIssuedAt = 0;
                o.result = MyBotsStepResult::Running;
                o.detail = taxiDetail;
                return o;
            case MyBotsTaxiResult::Approaching:
                IssueMove(player, job, bx, by, bz, false);
                o.result = MyBotsStepResult::Running;
                o.detail = taxiDetail;
                return o;
            case MyBotsTaxiResult::Unavailable:
                job.taxiRetryAt = now + sMyBotsConfig.NavTaxiRetrySec();
                break;
        }
    }

    IssueMove(player, job, x, y, z, false);
    o.result = MyBotsStepResult::Running;
    o.detail = "moving";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::MoveToCreature(Player* player, MyBotsJob& job, uint32 entry, float dist)
{
    if (Creature* c = FindNearestCreature(player, entry, 120.f))
    {
        job.navSpawnEntry = 0;
        return MoveTo(player, job, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), dist);
    }

    // Out of grid range: head for the spawn point so the taxi/long-distance
    // layer can do its work instead of failing the step outright.
    if (job.navSpawnEntry != entry)
    {
        float sx = 0.f, sy = 0.f, sz = 0.f;
        if (!FindNearestSpawnPoint(player, entry, sx, sy, sz))
        {
            MyBotsStepOutcome o;
            o.result = MyBotsStepResult::Failed;
            o.detail = "creature_not_found";
            return o;
        }
        job.navSpawnEntry = entry;
        job.navSpawnX = sx;
        job.navSpawnY = sy;
        // Hint from the spawn's own Z first. Using the player's Z here used to
        // pick a cave floor at the destination when the character was sunk or
        // standing in another zone at a different elevation.
        float surface = player->GetMapHeight(sx, sy, sz);
        if (surface <= INVALID_HEIGHT)
            surface = player->GetMapHeight(sx, sy, sz + 5.f);
        if (surface > INVALID_HEIGHT)
            sz = surface;
        job.navSpawnZ = sz;
    }

    return MoveTo(player, job, job.navSpawnX, job.navSpawnY, job.navSpawnZ, dist);
}

MyBotsStepOutcome MyBotsExecutor::Interact(Player* player, uint32 entry)
{
    MyBotsStepOutcome o;
    Creature* c = FindNearestCreature(player, entry, 8.f);
    if (!c)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "creature_not_in_range";
        return o;
    }
    player->SetFacingToObject(c);
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        TryDoAction(ai, "talk to quest giver");
    player->PrepareGossipMenu(c, 0, true);
    player->SendPreparedGossip(c);
    o.result = MyBotsStepResult::Done;
    o.detail = "interacted";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::GossipSelect(Player* player, uint32 entry, uint32 /*menu*/, uint32 option)
{
    MyBotsStepOutcome o;
    Creature* c = FindNearestCreature(player, entry, 8.f);
    if (!c)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "creature_not_in_range";
        return o;
    }
    player->SetFacingToObject(c);
    player->PrepareGossipMenu(c, 0, true);
    player->OnGossipSelect(c, option, 0);
    o.result = MyBotsStepResult::Done;
    o.detail = "gossip_selected";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::AcceptQuest(Player* player, uint32 questId, uint32 giverEntry)
{
    MyBotsStepOutcome o;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "quest_missing";
        return o;
    }
    if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE)
    {
        o.result = MyBotsStepResult::Done;
        o.detail = "already_have_or_done";
        return o;
    }

    Object* giver = nullptr;
    Creature* c = giverEntry ? FindNearestCreature(player, giverEntry, 10.f) : nullptr;
    if (c)
        giver = c;
    else
        giver = player;

    if (!player->CanTakeQuest(quest, false))
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "cannot_take";
        return o;
    }
    player->AddQuestAndCheckCompletion(quest, giver);
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        TryDoAction(ai, "accept all quests");
    o.result = MyBotsStepResult::Done;
    o.detail = "accepted";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::TurnInQuest(Player* player, uint32 questId, uint32 giverEntry)
{
    MyBotsStepOutcome o;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "quest_missing";
        return o;
    }

    if (player->GetQuestStatus(questId) == QUEST_STATUS_REWARDED)
    {
        o.result = MyBotsStepResult::Done;
        o.detail = "already_rewarded";
        return o;
    }

    if (player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
    {
        if (player->CanCompleteQuest(questId))
            player->CompleteQuest(questId);
    }

    if (player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "objectives_incomplete";
        return o;
    }

    Object* giver = player;
    if (Creature* c = giverEntry ? FindNearestCreature(player, giverEntry, 10.f) : nullptr)
        giver = c;

    player->RewardQuest(quest, 0, giver, true);
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        TryDoAction(ai, "turn in all quests");
    o.result = MyBotsStepResult::Done;
    o.detail = "turned_in";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::WaitUntil(Player* /*player*/, MyBotsJob& job, uint32 seconds)
{
    MyBotsStepOutcome o;
    uint32 const now = MyBotsNow();
    if (!job.waitUntil)
        job.waitUntil = now + seconds;
    if (now >= job.waitUntil)
    {
        job.waitUntil = 0;
        o.result = MyBotsStepResult::Done;
        o.detail = "waited";
        return o;
    }
    o.result = MyBotsStepResult::Waiting;
    o.detail = "waiting";
    return o;
}

void MyBotsExecutor::ClearQuestCombat(Player* player, MyBotsJob& job)
{
    if (!job.questGrindEnabled)
        return;
    if (player)
    {
        player->AttackStop();
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            ai->ChangeStrategy("-grind,-rpg,-new rpg,-travel", BOT_STATE_NON_COMBAT);
    }
    job.questGrindEnabled = false;
    job.questHuntEntry = 0;
}

void MyBotsExecutor::HaltControl(Player* player, MyBotsJob* job)
{
    if (job)
        ClearQuestCombat(player, *job);

    if (!player)
        return;

    player->AttackStop();
    player->StopMoving();
    if (MotionMaster* mm = player->GetMotionMaster())
    {
        mm->Clear();
        mm->MoveIdle();
    }

    if (job)
    {
        job->stuckSince = 0;
        job->waitUntil = 0;
        job->navAttempts = 0;
        job->detourUntil = 0;
        job->moveIssuedAt = 0;
        job->taxiRetryAt = 0;
        job->taxiInProgress = false;
        job->navSpawnEntry = 0;
        job->questHuntEntry = 0;
    }
}

MyBotsStepOutcome MyBotsExecutor::UntilQuestComplete(Player* player, MyBotsJob& job, uint32 questId,
    std::string const& detail)
{
    MyBotsStepOutcome o;
    if (!player || !questId)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "bad_until_args";
        return o;
    }

    auto const st = player->GetQuestStatus(questId);
    if (st == QUEST_STATUS_COMPLETE || st == QUEST_STATUS_REWARDED)
    {
        ClearQuestCombat(player, job);
        o.result = MyBotsStepResult::Done;
        o.detail = "quest_complete";
        return o;
    }
    if (st == QUEST_STATUS_NONE)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "quest_not_taken";
        return o;
    }

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "quest_missing";
        return o;
    }

    // Temporarily let Playerbots fight while we shepherd movement onto objectives.
    if (!job.questGrindEnabled)
    {
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            ai->ChangeStrategy("+grind,-follow,-rpg quest", BOT_STATE_NON_COMBAT);
        job.questGrindEnabled = true;
    }

    if (player->IsInCombat())
    {
        o.result = MyBotsStepResult::Running;
        o.detail = "fighting";
        return o;
    }

    // Prefer entries baked into the step; fall back to live plan from the template.
    std::vector<uint32> entries = ParseUIntArray(detail, "entries");
    if (entries.empty())
    {
        uint32 single = 0;
        if (ParseUInt(detail, "entry", single) && single)
            entries.push_back(single);
    }
    if (entries.empty())
    {
        MyBotsQuestPlan const plan = MyBotsQuestPlanner::Resolve(questId, detail);
        entries = plan.objectiveEntries;
    }

    auto stillNeedsCreature = [&](uint32 entry) -> bool
    {
        bool listedAsKill = false;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            if (quest->RequiredNpcOrGo[i] != int32(entry))
                continue;
            listedAsKill = true;
            if (player->GetReqKillOrCastCurrentCount(questId, int32(entry)) < quest->RequiredNpcOrGoCount[i])
                return true;
        }
        if (listedAsKill)
            return false; // kill objective finished for this entry

        // Not a kill objective → treat as item-dropper; keep hunting until quest done.
        return true;
    };

    uint32 hunt = 0;
    float bestDist = 0.f;
    for (uint32 entry : entries)
    {
        if (!entry || !stillNeedsCreature(entry))
            continue;
        if (Creature* live = FindNearestCreature(player, entry, 80.f))
        {
            float const d = player->GetDistance(live);
            if (!hunt || d < bestDist)
            {
                hunt = entry;
                bestDist = d;
            }
        }
        else if (!hunt)
            hunt = entry; // fall back to spawn walk
    }
    if (!hunt)
    {
        for (uint32 entry : entries)
            if (entry && stillNeedsCreature(entry))
            {
                hunt = entry;
                break;
            }
    }
    if (!hunt && !entries.empty())
        hunt = entries.front();

    if (!hunt)
    {
        // No known creature to chase (GO / talk / explore quests). Keep grind on
        // and wait — operator can cancel, or a script override should be used.
        o.result = MyBotsStepResult::Running;
        o.detail = "waiting_objectives";
        return o;
    }

    job.questHuntEntry = hunt;

    if (Creature* target = FindNearestCreature(player, hunt, 40.f))
    {
        if (player->IsWithinDistInMap(target, 5.f))
        {
            player->SetFacingToObject(target);
            if (!player->GetVictim())
                player->Attack(target, true);
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
                TryDoAction(ai, "attack");
            o.result = MyBotsStepResult::Running;
            o.detail = "attacking";
            return o;
        }
        MyBotsStepOutcome move = MoveTo(player, job, target->GetPositionX(), target->GetPositionY(),
            target->GetPositionZ(), 4.f);
        if (move.result == MyBotsStepResult::Failed)
            return move;
        o.result = MyBotsStepResult::Running;
        o.detail = "hunting";
        return o;
    }

    // Out of grid range: walk/fly toward the nearest spawn of this entry.
    MyBotsStepOutcome move = MoveToCreature(player, job, hunt, 20.f);
    if (move.result == MyBotsStepResult::Failed)
    {
        o.result = MyBotsStepResult::Running;
        o.detail = "objective_spawn_missing";
        return o;
    }
    if (move.result == MyBotsStepResult::Done)
    {
        // Arrived at spawn but no live creature — keep looking next tick.
        o.result = MyBotsStepResult::Running;
        o.detail = "waiting_spawn";
        return o;
    }
    o.result = MyBotsStepResult::Running;
    o.detail = move.detail == "moving" || move.detail.rfind("taxi_", 0) == 0 || move.detail == "in_flight"
        || move.detail == "detour" || move.detail == "detour_retry" || move.detail == "repath"
        ? move.detail
        : "approaching_objective";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::Revive(Player* player)
{
    MyBotsStepOutcome o;
    if (player->IsAlive())
    {
        o.result = MyBotsStepResult::Done;
        o.detail = "alive";
        return o;
    }
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
    {
        TryDoAction(ai, "release");
        TryDoAction(ai, "revive");
        TryDoAction(ai, "spirit healer");
    }
    if (!player->IsAlive())
    {
        player->ResurrectPlayer(0.5f);
        player->SpawnCorpseBones();
    }
    o.result = player->IsAlive() ? MyBotsStepResult::Done : MyBotsStepResult::Failed;
    o.detail = player->IsAlive() ? "revived" : "revive_failed";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::RunStep(Player* player, MyBotsJob& job, std::string const& op, std::string const& detail)
{
    if (op == "ensure_selfbot")
        return EnsureSelfbot(player);
    if (op == "move_to")
    {
        float x = 0, y = 0, z = 0;
        uint32 entry = 0;
        if (ParseUInt(detail, "entry", entry) && entry)
            return MoveToCreature(player, job, entry);
        if (!ParseMoveXYZ(detail, x, y, z))
        {
            MyBotsStepOutcome o;
            o.result = MyBotsStepResult::Failed;
            o.detail = "bad_move_args";
            return o;
        }
        float dist = 2.5f;
        ParseFloat(detail, "dist", dist);
        return MoveTo(player, job, x, y, z, dist);
    }
    if (op == "interact")
    {
        uint32 entry = 0;
        ParseUInt(detail, "entry", entry);
        return Interact(player, entry);
    }
    if (op == "gossip_select")
    {
        uint32 entry = 0, menu = 0, option = 0;
        ParseUInt(detail, "entry", entry);
        ParseUInt(detail, "menu", menu);
        ParseUInt(detail, "option", option);
        return GossipSelect(player, entry, menu, option);
    }
    if (op == "accept_quest")
    {
        uint32 q = 0, entry = 0;
        ParseUInt(detail, "questId", q);
        if (!q)
            ParseUInt(detail, "quest_id", q);
        ParseUInt(detail, "entry", entry);
        return AcceptQuest(player, q, entry);
    }
    if (op == "turnin_quest" || op == "turn_in_quest")
    {
        uint32 q = 0, entry = 0;
        ParseUInt(detail, "questId", q);
        if (!q)
            ParseUInt(detail, "quest_id", q);
        ParseUInt(detail, "entry", entry);
        return TurnInQuest(player, q, entry);
    }
    if (op == "wait")
    {
        uint32 sec = 1;
        ParseUInt(detail, "seconds", sec);
        return WaitUntil(player, job, sec);
    }
    if (op == "until")
    {
        uint32 q = 0;
        ParseUInt(detail, "questId", q);
        if (!q)
            ParseUInt(detail, "quest_id", q);
        return UntilQuestComplete(player, job, q, detail);
    }
    if (op == "revive")
        return Revive(player);

    MyBotsStepOutcome o;
    o.result = MyBotsStepResult::Failed;
    o.detail = "unknown_op:" + op;
    return o;
}
