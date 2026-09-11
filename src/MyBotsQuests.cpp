#include "MyBotsQuests.h"
#include "MyBotsUtil.h"

#include "CellImpl.h"
#include "Common.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "GossipDef.h"

#include <list>
#include <set>
#include <sstream>

namespace
{
char const* StatusLabel(QuestStatus st)
{
    switch (st)
    {
        case QUEST_STATUS_COMPLETE:   return "complete";
        case QUEST_STATUS_INCOMPLETE: return "incomplete";
        case QUEST_STATUS_FAILED:     return "failed";
        case QUEST_STATUS_NONE:       return "none";
        case QUEST_STATUS_REWARDED:   return "rewarded";
        default:                      return "unknown";
    }
}

uint32 FirstCreatureForQuest(QuestRelations const* map, uint32 questId)
{
    if (!map)
        return 0;
    for (auto const& [entry, qid] : *map)
        if (qid == questId)
            return entry;
    return 0;
}

std::string QuestTitle(Quest const* quest)
{
    if (!quest)
        return {};
    std::string title = quest->GetTitle();
    if (QuestLocale const* loc = sObjectMgr->GetQuestLocale(quest->GetQuestId()))
    {
        if (loc->Title.size() > LOCALE_zhCN && !loc->Title[LOCALE_zhCN].empty())
            title = loc->Title[LOCALE_zhCN];
    }
    return title;
}

void AppendQuestItem(std::ostringstream& ss, bool& first, Quest const* quest, QuestStatus st,
    uint32 giverEntry, uint32 turninEntry, bool available)
{
    if (!quest)
        return;
    if (!first)
        ss << ',';
    first = false;

    std::string titleEsc;
    MyBotsJsonEscape(QuestTitle(quest), titleEsc);

    ss << "{\"questId\":" << quest->GetQuestId()
       << ",\"title\":\"" << titleEsc << "\""
       << ",\"status\":" << static_cast<uint32>(st)
       << ",\"status_label\":\"" << StatusLabel(st) << "\""
       << ",\"questLevel\":" << quest->GetQuestLevel()
       << ",\"minLevel\":" << quest->GetMinLevel()
       << ",\"completable\":" << (st == QUEST_STATUS_COMPLETE ? "true" : "false");
    if (available)
        ss << ",\"heuristic\":false,\"source\":\"nearby\"";
    else
        ss << ",\"source\":\"live\"";
    if (giverEntry)
        ss << ",\"giverEntry\":" << giverEntry;
    if (turninEntry)
        ss << ",\"turninEntry\":" << turninEntry;
    ss << '}';
}

struct AnyCreatureInRange
{
    WorldObject const* obj;
    float range;
    AnyCreatureInRange(WorldObject const* o, float r) : obj(o), range(r) {}
    bool operator()(Creature* c) const
    {
        return c && c->IsInWorld() && c->IsAlive() && obj->IsWithinDistInMap(c, range);
    }
};
} // namespace

std::string MyBotsQuests::BuildQuestLogJson(Player* player)
{
    if (!player)
        return "{\"ok\":false,\"code\":\"not_found\"}";

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"online\":true"
       << ",\"source\":\"live\""
       << ",\"guid\":" << player->GetGUID().GetCounter()
       << ",\"name\":\"" << MyBotsJsonEscapeCopy(player->GetName()) << "\""
       << ",\"items\":[";

    bool first = true;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus const st = player->GetQuestStatus(questId);
        uint32 giver = FirstCreatureForQuest(sObjectMgr->GetCreatureQuestRelationMap(), questId);
        uint32 turnin = FirstCreatureForQuest(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), questId);
        if (!turnin)
            turnin = giver;
        AppendQuestItem(ss, first, quest, st, giver, turnin, false);
    }

    ss << "]}";
    return ss.str();
}

std::string MyBotsQuests::BuildNearbyAvailableJson(Player* player, float range, uint32 limit)
{
    if (!player)
        return "{\"ok\":false,\"code\":\"not_found\"}";
    if (limit == 0 || limit > 200)
        limit = 80;
    if (range <= 0.f)
        range = 80.f;

    std::list<Creature*> creatures;
    AnyCreatureInRange check(player, range);
    Acore::CreatureListSearcher<AnyCreatureInRange> searcher(player, creatures, check);
    Cell::VisitObjects(player, searcher, range);

    std::set<uint32> seen;
    std::ostringstream items;
    bool first = true;
    uint32 count = 0;

    for (Creature* creature : creatures)
    {
        if (!creature || count >= limit)
            break;
        if (!player->CanInteractWithQuestGiver(creature))
            continue;

        player->PrepareQuestMenu(creature->GetGUID());
        QuestMenu& menu = player->PlayerTalkClass->GetQuestMenu();
        if (menu.Empty())
            continue;

        uint32 const entry = creature->GetEntry();
        for (uint8 idx = 0; idx < menu.GetMenuItemCount() && count < limit; ++idx)
        {
            QuestMenuItem const& mi = menu.GetItem(idx);
            if (seen.count(mi.QuestId))
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(mi.QuestId);
            if (!quest)
                continue;
            QuestStatus const st = player->GetQuestStatus(mi.QuestId);
            if (st != QUEST_STATUS_NONE)
                continue;
            if (!player->CanTakeQuest(quest, false) || !player->CanAddQuest(quest, false))
                continue;

            seen.insert(mi.QuestId);
            uint32 turnin = FirstCreatureForQuest(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), mi.QuestId);
            if (!turnin)
                turnin = entry;
            AppendQuestItem(items, first, quest, QUEST_STATUS_NONE, entry, turnin, true);
            ++count;
        }
    }

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"online\":true"
       << ",\"source\":\"nearby\""
       << ",\"range\":" << range
       << ",\"guid\":" << player->GetGUID().GetCounter()
       << ",\"name\":\"" << MyBotsJsonEscapeCopy(player->GetName()) << "\""
       << ",\"level\":" << static_cast<uint32>(player->GetLevel())
       << ",\"note\":\"nearby_questgivers\""
       << ",\"items\":[" << items.str() << "]}";
    return ss.str();
}
