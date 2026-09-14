#include "MyBotsExecutor.h"
#include "MyBotsConfig.h"
#include "MyBotsJob.h"
#include "MyBotsNav.h"
#include "MyBotsQuestPlan.h"
#include "MyBotsTravel.h"
#include "MyBotsSelfbot.h"
#include "MyBotsUtil.h"

#include "Item.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellMgr.h"

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
// Prefer a spawn on the player's current map; otherwise pick any map and let
// MoveTo's cross-map router (hearthstone / boat / portal) get us there.
bool FindNearestSpawnPoint(Player* player, uint32 entry, float& x, float& y, float& z, uint32& outMap)
{
    if (!player || !entry)
        return false;

    uint16 const mapId = uint16(player->GetMapId());
    float bestSame = -1.f;
    float bestAny = -1.f;
    float ax = 0.f, ay = 0.f, az = 0.f;
    uint32 aMap = 0;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.id != entry)
            continue;

        if (data.mapid == mapId)
        {
            float const d = player->GetExactDist2d(data.posX, data.posY);
            if (bestSame < 0.f || d < bestSame)
            {
                bestSame = d;
                x = data.posX;
                y = data.posY;
                z = data.posZ;
                outMap = data.mapid;
            }
            continue;
        }

        // Cross-map: no meaningful 2d distance — keep the first alternate.
        if (bestAny < 0.f)
        {
            bestAny = 0.f;
            ax = data.posX;
            ay = data.posY;
            az = data.posZ;
            aMap = data.mapid;
        }
    }

    if (bestSame >= 0.f)
        return true;
    if (bestAny >= 0.f)
    {
        x = ax;
        y = ay;
        z = az;
        outMap = aMap;
        return true;
    }
    return false;
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
    job.taxiSawFlight = false;
    job.taxiBoardedAt = 0;
}

// Clearing the motion master every tick restarts pathfinding and makes the
// character stutter, so only re-issue when the target moved, the generator
// dropped out, or the re-path interval elapsed.
// Returns false when no navmesh route exists, so callers can escalate instead of
// letting the character walk a straight line through walls.
float Dist2dApprox(float ax, float ay, float bx, float by)
{
    float const dx = ax - bx;
    float const dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

bool IssueMove(Player* player, MyBotsJob& job, float x, float y, float z, bool force)
{
    uint32 const now = MyBotsNow();

    // Never interrupt an active taxi spline — Clear() here is what caused the
    // Goldshire board→drop→board loop.
    if (player->IsInFlight() || player->HasUnitFlag(UNIT_FLAG_TAXI_FLIGHT))
        return true;

    bool const inWater = player->isSwimming() || player->IsInWater();
    bool const sameRequest = std::fabs(job.moveReqX - x) < 2.f
        && std::fabs(job.moveReqY - y) < 2.f
        && std::fabs(job.moveReqZ - z) < 3.f;
    bool const driving = player->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
    // While in canals we must keep re-evaluating exit banks — skipping repath
    // leaves the bot swimming the same useless segment forever.
    if (!force && !inWater && sameRequest && driving && job.moveIssuedAt
        && now - job.moveIssuedAt < sMyBotsConfig.NavRepathSec())
        return true;

    // Already close to the last issued mesh target for this request — do not
    // retarget to a slightly different PrepareWalkTarget (common near NPCs and
    // the cause of endless circling).
    if (!force && !inWater && sameRequest && driving && job.moveIssuedAt)
    {
        float const toIssued = Dist2dApprox(player->GetPositionX(), player->GetPositionY(),
            job.moveTargetX, job.moveTargetY);
        if (toIssued < 8.f)
            return true;
    }

    // Rate-limit underground lifts — every-tick teleport=true is pure rubber-band.
    if (!job.lastLiftAt || now - job.lastLiftAt >= 5)
    {
        float const zBefore = player->GetPositionZ();
        MyBotsNav::CorrectIfUnderground(player);
        if (std::fabs(player->GetPositionZ() - zBefore) > 0.5f)
            job.lastLiftAt = now;
    }

    job.moveReqX = x;
    job.moveReqY = y;
    job.moveReqZ = z;

    // Stormwind canals: once swimming, pathing used to accept straight-line
    // water routes forever. Climb to a dry bank toward the land destination first.
    bool exitedWater = false;
    if (inWater)
    {
        float ex = 0.f, ey = 0.f, ez = 0.f;
        if (MyBotsNav::TryExitWaterToward(player, x, y, z, ex, ey, ez))
        {
            x = ex;
            y = ey;
            z = ez;
            exitedWater = true;
        }
    }

    // Resolve walkable XYZ via mmap (Playerbots-style). Do NOT use Map::GetHeight
    // from the sky — that is what snapped us onto cave floors.
    if (!MyBotsNav::PrepareWalkTarget(player, x, y, z))
    {
        LOG_DEBUG("module.mybots", "MyBots: no walkable path for {} toward ({:.1f},{:.1f},{:.1f})",
            player->GetName(), x, y, z);
        return false;
    }

    bool const sameTarget = std::fabs(job.moveTargetX - x) < 1.5f
        && std::fabs(job.moveTargetY - y) < 1.5f
        && std::fabs(job.moveTargetZ - z) < 2.f;
    if (!force && !exitedWater && sameTarget && driving && job.moveIssuedAt
        && now - job.moveIssuedAt < sMyBotsConfig.NavRepathSec())
        return true;

    job.moveTargetX = x;
    job.moveTargetY = y;
    job.moveTargetZ = z;
    job.moveIssuedAt = now;

    // Avoid Clear() when already on a point move — Mutate via MovePoint is enough
    // and prevents the client from restarting the run animation at full speed.
    MotionMaster* mm = player->GetMotionMaster();
    if (!driving)
        mm->Clear(false);
    mm->MovePoint(1, x, y, z, FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
    return true;
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

MyBotsStepOutcome MyBotsExecutor::MoveTo(Player* player, MyBotsJob& job, float x, float y, float z, float dist,
    uint32 targetMap)
{
    MyBotsStepOutcome o;
    if (!player)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "no_player";
        return o;
    }

    // Cross-map: the destination lives on another map. Route there by rules
    // (hearthstone / boat / portal) instead of walking a straight line across
    // the current map toward coordinates that mean nothing here. Once we land
    // on the target map, fall through to normal same-map navmesh movement.
    // NOTE: map 0 (Eastern Kingdoms) is a valid pin — do not treat 0 as "unset".
    if (targetMap != MAP_UNSPECIFIED && targetMap != player->GetMapId())
    {
        std::string td;
        switch (MyBotsTravel::AdvanceCrossMap(player, job, targetMap, x, y, z, td))
        {
            case MyBotsTravelResult::Advancing:
                // Walk to the transfer boarding point on the CURRENT map using
                // normal navmesh pathing — that is the whole point of the leg.
                if (job.travelLegSet && (td == "transfer_approach" || td.rfind("transfer_", 0) == 0))
                {
                    if (td == "transfer_approach")
                        IssueMove(player, job, job.travelLegX, job.travelLegY, job.travelLegZ, false);
                }
                o.result = MyBotsStepResult::Running;
                o.detail = td;
                return o;
            case MyBotsTravelResult::Unreachable:
                player->StopMoving();
                o.result = MyBotsStepResult::Failed;
                o.detail = td;
                return o;
            case MyBotsTravelResult::Arrived:
                MyBotsTravel::Reset(job);
                ResetNavState(job);
                break; // resume same-map pathing below
        }
    }

    uint32 const now = MyBotsNow();

    // A taxi flight owns movement until the character lands.
    if (player->IsInFlight() || player->HasUnitFlag(UNIT_FLAG_TAXI_FLIGHT))
    {
        job.stuckSince = 0;
        job.taxiInProgress = true;
        job.taxiSawFlight = true;
        o.result = MyBotsStepResult::Running;
        o.detail = "in_flight";
        return o;
    }

    if (job.taxiInProgress)
    {
        // ActivateTaxiPathTo sets the flag a tick or two later. Treating that
        // gap as "landed" made us Clear() the spline and re-board forever.
        if (!job.taxiSawFlight && job.taxiBoardedAt && now - job.taxiBoardedAt < 5)
        {
            o.result = MyBotsStepResult::Running;
            o.detail = "taxi_pending";
            return o;
        }
        job.taxiInProgress = false;
        job.taxiSawFlight = false;
        job.taxiBoardedAt = 0;
        job.stuckSince = 0;
        job.moveIssuedAt = 0;
        // Cool down so we do not immediately hop the same Goldshire bird again.
        job.taxiRetryAt = now + sMyBotsConfig.NavTaxiRetrySec();
    }

    if (player->GetDistance(x, y, z) <= dist
        || player->GetExactDist2d(x, y) <= dist)
    {
        player->StopMoving();
        ResetNavState(job);
        o.result = MyBotsStepResult::Done;
        o.detail = "arrived";
        return o;
    }

    // Near the goal but pathfinding keeps failing / detouring: soft-arrive so
    // turn-in / interact can run instead of orbiting forever.
    float const near2d = player->GetExactDist2d(x, y);
    if (near2d <= std::max(dist * 3.f, 12.f) && near2d > dist
        && job.stuckSince && now - job.stuckSince >= std::max(8u, sMyBotsConfig.StuckTimeoutSec() / 2))
    {
        player->StopMoving();
        ResetNavState(job);
        o.result = MyBotsStepResult::Done;
        o.detail = "arrived_soft";
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
                job.taxiSawFlight = false;
                job.taxiBoardedAt = now;
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

    bool const wasInWater = player->isSwimming() || player->IsInWater();
    bool const landDest = !MyBotsNav::IsDeepWaterAt(player, x, y, z);
    if (!IssueMove(player, job, x, y, z, false))
    {
        // No navmesh route: stay put and let the stuck timer escalate to a
        // detour. Issuing a move anyway is what produced straight lines through
        // walls and floors.
        o.result = MyBotsStepResult::Running;
        o.detail = wasInWater && landDest ? "canal_trapped" : "unreachable";
        return o;
    }

    o.result = MyBotsStepResult::Running;
    o.detail = wasInWater && landDest ? "canal_exit" : "moving";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::MoveToCreature(Player* player, MyBotsJob& job, uint32 entry, float dist)
{
    if (Creature* c = FindNearestCreature(player, entry, 120.f))
    {
        job.navSpawnEntry = 0;
        return MoveTo(player, job, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), dist,
            player->GetMapId());
    }

    // Out of grid range (or on another map): head for a spawn point so the
    // cross-map / taxi layer can do its work instead of failing outright.
    if (job.navSpawnEntry != entry)
    {
        float sx = 0.f, sy = 0.f, sz = 0.f;
        uint32 spawnMap = 0;
        if (!FindNearestSpawnPoint(player, entry, sx, sy, sz, spawnMap))
        {
            MyBotsStepOutcome o;
            o.result = MyBotsStepResult::Failed;
            o.detail = "creature_not_found";
            return o;
        }
        job.navSpawnEntry = entry;
        job.navSpawnX = sx;
        job.navSpawnY = sy;
        // Only nudge Z onto the surface when we are already on that map — a
        // height lookup on the wrong map is meaningless.
        if (spawnMap == player->GetMapId())
        {
            float const surface = player->GetMapHeight(sx, sy, sz);
            if (surface > INVALID_HEIGHT && std::fabs(surface - sz) <= 3.f)
                sz = surface;
        }
        job.navSpawnZ = sz;
        // Stash the spawn map in travelDestMap-ish via MoveTo's targetMap arg.
        return MoveTo(player, job, job.navSpawnX, job.navSpawnY, sz, dist, spawnMap);
    }

    // Reuse the cached spawn; we do not know its map from the cache alone, so
    // re-resolve once if the previous call stored coordinates only.
    uint32 spawnMap = player->GetMapId();
    {
        float sx = 0.f, sy = 0.f, sz = 0.f;
        uint32 m = 0;
        if (FindNearestSpawnPoint(player, entry, sx, sy, sz, m))
            spawnMap = m;
    }
    return MoveTo(player, job, job.navSpawnX, job.navSpawnY, job.navSpawnZ, dist, spawnMap);
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

MyBotsStepOutcome MyBotsExecutor::UseItem(Player* player, MyBotsJob& job, uint32 itemId)
{
    MyBotsStepOutcome o;
    if (!itemId)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "bad_item";
        return o;
    }

    uint32 const now = MyBotsNow();
    if (job.useItemPendingId == itemId && job.useItemPendingAt)
    {
        if (player->IsNonMeleeSpellCast(false))
        {
            o.result = MyBotsStepResult::Running;
            o.detail = "casting";
            return o;
        }
        // Give the cast a moment to start; then treat idle as finished/rejected.
        if (now - job.useItemPendingAt >= 1)
        {
            job.useItemPendingId = 0;
            job.useItemPendingAt = 0;
            o.result = MyBotsStepResult::Done;
            o.detail = "item_used";
            return o;
        }
        o.result = MyBotsStepResult::Running;
        o.detail = "casting_item";
        return o;
    }

    if (player->IsNonMeleeSpellCast(false))
    {
        o.result = MyBotsStepResult::Running;
        o.detail = "casting";
        return o;
    }

    Item* item = player->GetItemByEntry(itemId);
    if (!item)
    {
        o.result = MyBotsStepResult::Failed;
        o.detail = "item_missing";
        return o;
    }

    SpellCastTargets targets;
    targets.SetUnitTarget(player);
    player->CastItemUseSpell(item, targets, 1, 0);
    job.useItemPendingId = itemId;
    job.useItemPendingAt = now;
    o.result = MyBotsStepResult::Running;
    o.detail = "casting_item";
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
        job->moveReqX = job->moveReqY = job->moveReqZ = 0.f;
        job->moveTargetX = job->moveTargetY = job->moveTargetZ = 0.f;
        job->lastLiftAt = 0;
        job->taxiRetryAt = 0;
        job->taxiInProgress = false;
        job->taxiSawFlight = false;
        job->taxiBoardedAt = 0;
        job->navSpawnEntry = 0;
        job->questHuntEntry = 0;
        job->useItemPendingId = 0;
        job->useItemPendingAt = 0;
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

    // Prefer entries baked into the step; fall back to live plan from the template.
    std::vector<uint32> entries = ParseUIntArray(detail, "entries");
    if (entries.empty())
    {
        uint32 single = 0;
        if (ParseUInt(detail, "entry", single) && single)
            entries.push_back(single);
    }

    uint32 speakFlag = 0;
    ParseUInt(detail, "speak", speakFlag);
    bool speakObjective = speakFlag != 0;
    if (entries.empty() || !speakObjective)
    {
        MyBotsQuestPlan const plan = MyBotsQuestPlanner::Resolve(questId, detail);
        if (entries.empty())
            entries = plan.objectiveEntries;
        if (!speakObjective)
            speakObjective = plan.speakObjective;
    }

    // Temporarily let Playerbots fight / open quest containers, or only talk for
    // gossip/event objectives (Great Bear Spirit must not be grind-attacked).
    if (!job.questGrindEnabled)
    {
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        {
            if (speakObjective)
                ai->ChangeStrategy("+rpg quest,-grind,-follow", BOT_STATE_NON_COMBAT);
            else
                ai->ChangeStrategy("+grind,+rpg quest,-follow", BOT_STATE_NON_COMBAT);
        }
        job.questGrindEnabled = true;
    }

    if (player->IsInCombat() && !speakObjective)
    {
        o.result = MyBotsStepResult::Running;
        o.detail = "fighting";
        return o;
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

        // Item / source-item dropper: keep hunting only while the character still
        // needs an item this creature is known to provide (questitem map).
        if (CreatureQuestItemList const* items = sObjectMgr->GetCreatureQuestItemList(entry))
        {
            bool needsAny = false;
            for (uint32 itemId : *items)
            {
                if (!itemId)
                    continue;
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                {
                    if (quest->RequiredItemId[i] == itemId
                        && player->GetItemCount(itemId) < quest->RequiredItemCount[i])
                        return true;
                }
                for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
                {
                    if (quest->ItemDrop[i] == itemId
                        && player->GetItemCount(itemId) < quest->ItemDropQuantity[i])
                        return true;
                }
                needsAny = true;
            }
            // Mapped items are already satisfied (e.g. key looted) — stop chasing.
            if (needsAny)
                return false;
        }

        // Unknown drop mapping: keep hunting until the quest completes.
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
        // No creature left to chase (key looted, badge in a chest, speak/GO). Keep
        // grind/rpg quest on so Playerbots can use the key / loot the object.
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            TryDoAction(ai, "rpg");
        o.result = MyBotsStepResult::Running;
        o.detail = "waiting_objectives";
        return o;
    }

    job.questHuntEntry = hunt;

    if (Creature* target = FindNearestCreature(player, hunt, 40.f))
    {
        if (player->IsWithinDistInMap(target, speakObjective ? 8.f : 5.f))
        {
            player->SetFacingToObject(target);
            if (speakObjective)
            {
                // Gossip / event credit — open menu and poke options (e.g. 5929).
                Interact(player, hunt);
                for (uint32 opt = 0; opt < 6; ++opt)
                    GossipSelect(player, hunt, 0, opt);
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
                {
                    TryDoAction(ai, "talk to quest giver");
                    TryDoAction(ai, "rpg");
                }
                o.result = MyBotsStepResult::Running;
                o.detail = "speaking";
                return o;
            }
            if (!player->GetVictim())
                player->Attack(target, true);
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
                TryDoAction(ai, "attack");
            o.result = MyBotsStepResult::Running;
            o.detail = "attacking";
            return o;
        }
        MyBotsStepOutcome move = MoveTo(player, job, target->GetPositionX(), target->GetPositionY(),
            target->GetPositionZ(), speakObjective ? 6.f : 4.f);
        if (move.result == MyBotsStepResult::Failed)
            return move;
        o.result = MyBotsStepResult::Running;
        o.detail = speakObjective ? "approaching_speak" : "hunting";
        return o;
    }

    // Summoned kill targets (Binding voidwalker): no world spawn. Stand on the
    // summoning circle and use the quest StartItem until the creature appears.
    uint32 useItemId = 0;
    ParseUInt(detail, "useItemId", useItemId);
    if (!useItemId)
    {
        MyBotsQuestPlan const plan = MyBotsQuestPlanner::Resolve(questId, detail);
        useItemId = plan.useItemId;
        if (!useItemId && plan.HasSummonedObjective())
            if (Quest const* q = sObjectMgr->GetQuestTemplate(questId))
                useItemId = q->GetSrcItemId();
    }

    float sx = 0.f, sy = 0.f, sz = 0.f;
    bool haveSite = ParseMoveXYZ(detail, sx, sy, sz);
    if (!haveSite)
    {
        MyBotsQuestPlan const plan = MyBotsQuestPlanner::Resolve(questId, detail);
        if (plan.hasSummonSite)
        {
            sx = plan.summonX;
            sy = plan.summonY;
            sz = plan.summonZ;
            haveSite = true;
        }
    }
    if (haveSite && player->GetExactDist2d(sx, sy) > 4.f)
    {
        MyBotsStepOutcome move = MoveTo(player, job, sx, sy, sz, 2.5f);
        if (move.result == MyBotsStepResult::Failed)
            return move;
        o.result = MyBotsStepResult::Running;
        o.detail = "approaching_summon_site";
        return o;
    }
    if (useItemId)
    {
        MyBotsStepOutcome used = UseItem(player, job, useItemId);
        if (used.result == MyBotsStepResult::Failed && used.detail == "item_missing")
            return used;
        o.result = MyBotsStepResult::Running;
        o.detail = used.detail.empty() ? "summoning" : used.detail;
        return o;
    }

    // Out of grid range: walk/fly toward the nearest spawn of this entry.
    MyBotsStepOutcome move = MoveToCreature(player, job, hunt, speakObjective ? 8.f : 20.f);
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
    if (op == "move_to" || op == "travel_to")
    {
        float x = 0, y = 0, z = 0;
        uint32 entry = 0;
        uint32 map = MAP_UNSPECIFIED;
        ParseUInt(detail, "map", map); // leaves MAP_UNSPECIFIED when key absent
        // Entry-based moves: resolve spawn (any map) and route via MoveToCreature /
        // cross-map. travel_to may also use entry when the LLM names a hub NPC.
        if (ParseUInt(detail, "entry", entry) && entry)
        {
            float dist = 3.f;
            ParseFloat(detail, "dist", dist);
            // If map is pinned and differs, MoveToCreature still finds the spawn
            // on that map via FindNearestSpawnPoint's cross-map fallback.
            (void)map;
            return MoveToCreature(player, job, entry, dist);
        }
        if (!ParseMoveXYZ(detail, x, y, z))
        {
            MyBotsStepOutcome o;
            o.result = MyBotsStepResult::Failed;
            o.detail = "bad_move_args";
            return o;
        }
        float dist = 2.5f;
        ParseFloat(detail, "dist", dist);
        return MoveTo(player, job, x, y, z, dist, map);
    }
    if (op == "interact")
    {
        uint32 entry = 0;
        ParseUInt(detail, "entry", entry);
        return Interact(player, entry);
    }
    if (op == "use_item")
    {
        uint32 itemId = 0;
        ParseUInt(detail, "itemId", itemId);
        if (!itemId)
            ParseUInt(detail, "item", itemId);
        return UseItem(player, job, itemId);
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
    if (op == "use_hearthstone" || op == "hearthstone")
    {
        MyBotsStepOutcome o;
        if (!player)
        {
            o.result = MyBotsStepResult::Failed;
            o.detail = "no_player";
            return o;
        }
        if (player->HasUnitState(UNIT_STATE_CASTING))
        {
            o.result = MyBotsStepResult::Running;
            o.detail = "hearth_casting";
            return o;
        }
        // Optional: only succeed when we land on a requested map (0 is valid).
        uint32 wantMap = MAP_UNSPECIFIED;
        if (ParseUInt(detail, "map", wantMap) && player->GetMapId() == wantMap)
        {
            o.result = MyBotsStepResult::Done;
            o.detail = "already_on_map";
            return o;
        }
        if (job.hearthCastAt && MyBotsNow() - job.hearthCastAt < 15)
        {
            o.result = MyBotsStepResult::Running;
            o.detail = "hearth_pending";
            return o;
        }
        if (!MyBotsTravel::TriggerHearthstone(player))
        {
            o.result = MyBotsStepResult::Failed;
            o.detail = "hearth_unavailable";
            return o;
        }
        job.hearthCastAt = MyBotsNow();
        o.result = MyBotsStepResult::Running;
        o.detail = "hearth_cast";
        return o;
    }

    MyBotsStepOutcome o;
    o.result = MyBotsStepResult::Failed;
    o.detail = "unknown_op:" + op;
    return o;
}
