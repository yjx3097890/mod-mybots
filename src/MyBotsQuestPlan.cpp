#include "MyBotsQuestPlan.h"
#include "MyBotsConfig.h"
#include "MyBotsExecutor.h"

#include "DatabaseEnv.h"
// GameObjectData uses G3D::Quat; include Quat before that header when pulling it
// in isolation (ObjectMgr alone is not always enough for module TUs).
#include "G3D/Quat.h"
#include "GameObjectData.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#ifdef MYBOTS_HAVE_TRAVELMGR
#include "Playerbots.h"
#include "TravelMgr.h"
#endif

#include <algorithm>
#include <cmath>
#include <unordered_set>

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

// Gossip / script NPCs that fire AreaExploredOrEventHappens(questId), e.g.
// Great Bear Spirit (11956) for quest 5929.
void CollectEventCreditCreatures(uint32 questId, std::vector<uint32>& out)
{
    if (!questId)
        return;
    if (QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT entryorguid FROM smart_scripts WHERE source_type = 0 AND "
            "entryorguid > 0 AND action_type = 15 AND action_param1 = {}",
            questId))
    {
        do
        {
            AddUnique(out, result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());
    }
}

bool QuestHasKillOrItemObjectives(Quest const* quest)
{
    if (!quest)
        return false;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredNpcOrGo[i] > 0 && quest->RequiredNpcOrGoCount[i] > 0)
            return true;
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
            return true;
    for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
        if (quest->ItemDrop[i] && quest->ItemDropQuantity[i])
            return true;
    return false;
}

bool CreatureHasWorldSpawn(uint32 entry)
{
    if (!entry)
        return false;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        if (data.id == entry)
            return true;
    return false;
}

bool FindCreatureSpawnNear(uint32 entry, float& x, float& y, float& z, uint16& mapId)
{
    if (!entry)
        return false;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.id != entry)
            continue;
        // Prefer the first spawn; callers only need a reference point near the quest hub.
        x = data.posX;
        y = data.posY;
        z = data.posZ;
        mapId = data.mapid;
        return true;
    }
    return false;
}

uint32 SpellFocusForItem(uint32 itemId)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        uint32 const spellId = proto->Spells[i].SpellId;
        if (!spellId)
            continue;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            if (info->RequiresSpellFocus)
                return info->RequiresSpellFocus;
    }
    return 0;
}

bool FindSummonSiteNear(uint32 focusId, uint16 mapId, float refX, float refY, float& x, float& y, float& z)
{
    std::unordered_set<uint32> goEntries;
    if (GameObjectTemplateContainer const* all = sObjectMgr->GetGameObjectTemplates())
    {
        for (auto const& [entry, go] : *all)
        {
            if (go.type != GAMEOBJECT_TYPE_SPELL_FOCUS)
                continue;
            if (focusId && go.spellFocus.focusId != focusId)
                continue;
            // Without a focus id, only take named summoning circles (warlock Binding etc.).
            if (!focusId)
            {
                std::string const& n = go.name;
                if (n.find("Summoning Circle") == std::string::npos
                    && n.find("Summoning Portal") == std::string::npos
                    && n.find("Rune of Summoning") == std::string::npos)
                    continue;
            }
            goEntries.insert(entry);
        }
    }
    if (goEntries.empty())
        return false;

    float best = -1.f;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (data.mapid != mapId || !goEntries.count(data.id))
            continue;
        float const dx = data.posX - refX;
        float const dy = data.posY - refY;
        float const d = dx * dx + dy * dy;
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

void ResolveSummonSite(MyBotsQuestPlan& plan)
{
    if (!plan.HasSummonedObjective())
        return;

    if (!plan.useItemId)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(plan.questId))
            plan.useItemId = quest->GetSrcItemId();

    uint32 const focusId = SpellFocusForItem(plan.useItemId);
    uint32 const anchor = plan.turninEntry ? plan.turninEntry : plan.giverEntry;
    float refX = 0.f, refY = 0.f, refZ = 0.f;
    uint16 mapId = 0;
    if (anchor && FindCreatureSpawnNear(anchor, refX, refY, refZ, mapId))
    {
        if (FindSummonSiteNear(focusId, mapId, refX, refY, plan.summonX, plan.summonY, plan.summonZ))
            plan.hasSummonSite = true;
    }

    // Fallback: any summoning circle on the same map as the character's hub NPC.
    if (!plan.hasSummonSite && mapId)
    {
        if (FindSummonSiteNear(0, mapId, refX, refY, plan.summonX, plan.summonY, plan.summonZ))
            plan.hasSummonSite = true;
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
        {
            uint32 const entry = uint32(req);
            AddUnique(plan.objectiveEntries, entry);
            // No creature table row ⇒ must be summoned (Binding voidwalker 5676, etc.).
            if (!CreatureHasWorldSpawn(entry))
                AddUnique(plan.summonedEntries, entry);
        }
        else if (req < 0)
            plan.hasObjectives = true; // GO objective — until still needed
    }

    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const itemId = quest->RequiredItemId[i];
        if (!itemId || !quest->RequiredItemCount[i])
            continue;
        // Item objectives always need an until step — even when the item comes
        // from a chest GO and creature_questitem has no row for it. Skipping
        // until here is what made quests like 1667 walk straight to turn-in
        // and fail with objectives_incomplete.
        plan.hasObjectives = true;
        CollectCreaturesDroppingItem(itemId, plan.objectiveEntries);
    }

    // Source / intermediate items (keys, etc.). Quest 1667 stores Dead-Tooth's
    // Key here while the badge objective is on a strongbox.
    for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
    {
        uint32 const itemId = quest->ItemDrop[i];
        if (!itemId || !quest->ItemDropQuantity[i])
            continue;
        plan.hasObjectives = true;
        CollectCreaturesDroppingItem(itemId, plan.objectiveEntries);
    }

    // Speak / explore / gossip-credit quests (Great Bear Spirit 5929, etc.): no
    // RequiredNpcOrGo/Item rows, but SpecialFlags mark an event objective.
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT)
        || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_SPEAKTO))
    {
        plan.hasObjectives = true;
        plan.speakObjective = !QuestHasKillOrItemObjectives(quest);
        CollectEventCreditCreatures(questId, plan.objectiveEntries);
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

    if (plan.HasSummonedObjective())
    {
        plan.useItemId = quest->GetSrcItemId();
        ResolveSummonSite(plan);
    }

    // Resolve the quest hub (giver preferred, else turn-in) so the director can
    // prepend a cross-map travel_to when the character is on another continent.
    {
        uint32 const hubEntry = plan.giverEntry ? plan.giverEntry : plan.turninEntry;
        uint16 mapId = 0;
        float hx = 0.f, hy = 0.f, hz = 0.f;
        if (hubEntry && FindCreatureSpawnNear(hubEntry, hx, hy, hz, mapId))
        {
            plan.hasHub = true;
            plan.hubMap = mapId;
            plan.hubX = hx;
            plan.hubY = hy;
            plan.hubZ = hz;
        }
        else if (plan.hasSummonSite)
        {
            // Binding-style quests: the summoning circle is the hub.
            plan.hasHub = true;
            // summon site already resolved on a known map via ResolveSummonSite —
            // reuse turn-in/giver map lookup as a fallback.
            uint16 m = 0;
            float rx = 0.f, ry = 0.f, rz = 0.f;
            uint32 const anchor = plan.turninEntry ? plan.turninEntry : plan.giverEntry;
            if (anchor && FindCreatureSpawnNear(anchor, rx, ry, rz, m))
                plan.hubMap = m;
            plan.hubX = plan.summonX;
            plan.hubY = plan.summonY;
            plan.hubZ = plan.summonZ;
            plan.hasHub = plan.hubMap != 0;
        }
    }

    LOG_INFO("module.mybots",
        "MyBots quest plan {}: giver={} turnin={} hubMap={} objectives={} summoned={} useItem={} "
        "summonSite={} hasObj={} speak={}",
        questId, plan.giverEntry, plan.turninEntry, plan.hubMap, plan.objectiveEntries.size(),
        plan.summonedEntries.size(), plan.useItemId, plan.hasSummonSite ? 1 : 0,
        plan.hasObjectives ? 1 : 0, plan.speakObjective ? 1 : 0);

    return plan;
}
