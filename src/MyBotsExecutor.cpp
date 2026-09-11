#include "MyBotsExecutor.h"
#include "MyBotsConfig.h"
#include "MyBotsJob.h"
#include "MyBotsSelfbot.h"
#include "MyBotsUtil.h"

#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"

#include <cmath>
#include <cstdlib>

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

    float const d = player->GetDistance(x, y, z);
    if (d <= dist)
    {
        player->StopMoving();
        job.stuckSince = 0;
        o.result = MyBotsStepResult::Done;
        o.detail = "arrived";
        return o;
    }

    if (player->IsInCombat())
    {
        o.result = MyBotsStepResult::Running;
        o.detail = "in_combat";
        return o;
    }

    float const moved = std::fabs(player->GetPositionX() - job.lastX)
        + std::fabs(player->GetPositionY() - job.lastY)
        + std::fabs(player->GetPositionZ() - job.lastZ);
    uint32 const now = MyBotsNow();
    if (moved < 0.4f)
    {
        if (!job.stuckSince)
            job.stuckSince = now;
        else if (now - job.stuckSince >= sMyBotsConfig.StuckTimeoutSec())
        {
            o.result = MyBotsStepResult::Failed;
            o.detail = "stuck";
            return o;
        }
    }
    else
        job.stuckSince = 0;

    job.lastX = player->GetPositionX();
    job.lastY = player->GetPositionY();
    job.lastZ = player->GetPositionZ();

    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        TryDoAction(ai, "move to position");

    player->GetMotionMaster()->Clear();
    player->GetMotionMaster()->MovePoint(1, x, y, z);
    o.result = MyBotsStepResult::Running;
    o.detail = "moving";
    return o;
}

MyBotsStepOutcome MyBotsExecutor::MoveToCreature(Player* player, MyBotsJob& job, uint32 entry, float dist)
{
    Creature* c = FindNearestCreature(player, entry, 120.f);
    if (!c)
    {
        MyBotsStepOutcome o;
        o.result = MyBotsStepResult::Failed;
        o.detail = "creature_not_found";
        return o;
    }
    return MoveTo(player, job, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), dist);
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

MyBotsStepOutcome MyBotsExecutor::UntilQuestComplete(Player* player, uint32 questId)
{
    MyBotsStepOutcome o;
    auto st = player->GetQuestStatus(questId);
    if (st == QUEST_STATUS_COMPLETE || st == QUEST_STATUS_REWARDED)
    {
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
    // Let combat/grind strategies from playerbots handle kills while we wait;
    // for directed jobs we only poll status.
    o.result = MyBotsStepResult::Running;
    o.detail = "waiting_objectives";
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
        return UntilQuestComplete(player, q);
    }
    if (op == "revive")
        return Revive(player);

    MyBotsStepOutcome o;
    o.result = MyBotsStepResult::Failed;
    o.detail = "unknown_op:" + op;
    return o;
}
