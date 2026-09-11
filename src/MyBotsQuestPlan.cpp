#include "MyBotsQuestPlan.h"
#include "MyBotsConfig.h"
#include "MyBotsExecutor.h"

#include "Log.h"
#include "ObjectMgr.h"
#include "QuestDef.h"

#ifdef MYBOTS_HAVE_TRAVELMGR
#include "Playerbots.h"
#include "TravelMgr.h"
#endif

#include <algorithm>

namespace
{
uint32 FindCreatureForQuest(QuestRelations const* map, uint32 questId)
{
    if (!map)
        return 0;
    for (auto const& [entry, qid] : *map)
        if (qid == questId)
            return entry;
    return 0;
}

void AddUnique(std::vector<uint32>& out, uint32 entry)
{
    if (!entry)
        return;
    if (std::find(out.begin(), out.end(), entry) == out.end())
        out.push_back(entry);
}

void CollectCreaturesDroppingItem(uint32 itemId, std::vector<uint32>& out)
{
    if (!itemId)
        return;
    CreatureQuestItemMap const* map = sObjectMgr->GetCreatureQuestItemMap();
    if (!map)
        return;
    for (auto const& [creatureEntry, items] : *map)
        for (uint32 id : items)
            if (id == itemId)
            {
                AddUnique(out, creatureEntry);
                break;
            }
}
} // namespace

MyBotsQuestPlan MyBotsQuestPlanner::Resolve(uint32 questId, std::string const& payload)
{
    MyBotsQuestPlan plan;
    plan.questId = questId;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
    {
        plan.error = "quest_missing";
        return plan;
    }

    MyBotsExecutor::ParseUInt(payload, "giverEntry", plan.giverEntry);
    MyBotsExecutor::ParseUInt(payload, "turninEntry", plan.turninEntry);

    if (!plan.giverEntry)
        plan.giverEntry = FindCreatureForQuest(sObjectMgr->GetCreatureQuestRelationMap(), questId);
    // GameObject starters (wanted boards etc.) are skipped for move_to — accept
    // without a giver NPC, or the character already has the quest.

    if (!plan.turninEntry)
        plan.turninEntry = FindCreatureForQuest(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), questId);
    if (!plan.turninEntry)
        plan.turninEntry = plan.giverEntry;

    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const req = quest->RequiredNpcOrGo[i];
        if (quest->RequiredNpcOrGoCount[i] == 0)
            continue;
        if (req > 0)
            AddUnique(plan.objectiveEntries, uint32(req));
        else if (req < 0)
            plan.hasObjectives = true; // GO objective — until still needed
    }

    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const itemId = quest->RequiredItemId[i];
        if (!itemId || !quest->RequiredItemCount[i])
            continue;
        CollectCreaturesDroppingItem(itemId, plan.objectiveEntries);
    }

#ifdef MYBOTS_HAVE_TRAVELMGR
    // Playerbots already resolved loot → creature for quest items; reuse that.
    if (sMyBotsConfig.NavUseTravelMgr())
    {
        auto it = sTravelMgr.quests.find(questId);
        if (it != sTravelMgr.quests.end() && it->second)
        {
            if (!plan.turninEntry)
            {
                for (QuestTravelDestination* loc : it->second->questTakers)
                {
                    if (!loc || loc->getEntry() <= 0)
                        continue;
                    plan.turninEntry = uint32(loc->getEntry());
                    break;
                }
            }
            if (plan.objectiveEntries.empty())
            {
                for (QuestTravelDestination* loc : it->second->questObjectives)
                {
                    if (!loc || loc->getEntry() <= 0)
                        continue;
                    AddUnique(plan.objectiveEntries, uint32(loc->getEntry()));
                }
            }
        }
    }
#endif

    if (!plan.objectiveEntries.empty())
        plan.hasObjectives = true;

    LOG_INFO("module.mybots",
        "MyBots quest plan {}: giver={} turnin={} objectives={} hasObj={}",
        questId, plan.giverEntry, plan.turninEntry, plan.objectiveEntries.size(),
        plan.hasObjectives ? 1 : 0);

    return plan;
}
