#include "MyBotsQuests.h"
#include "MyBotsUtil.h"

#include "Common.h"
#include "DBCStores.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        ss << ",\"heuristic\":false,\"source\":\"map\"";
    else
        ss << ",\"source\":\"live\"";
    if (giverEntry)
        ss << ",\"giverEntry\":" << giverEntry;
    if (turninEntry)
        ss << ",\"turninEntry\":" << turninEntry;
    ss << '}';
}

bool QuestAllowsPlayerFaction(Player* player, Quest const* quest)
{
    if (!player || !quest)
        return false;
    uint32 const races = quest->GetAllowableRaces();
    if (!races)
        return true; // 0 = all races
    return (races & player->getRaceMask()) != 0;
}

bool CreatureTemplateFriendlyToPlayer(Player* player, uint32 entry)
{
    CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(entry);
    if (!cInfo)
        return false;

    FactionTemplateEntry const* npcFaction = sFactionTemplateStore.LookupEntry(cInfo->faction);
    FactionTemplateEntry const* playerFaction = player->GetFactionTemplateEntry();
    if (!npcFaction || !playerFaction)
        return true; // unknown — let CanTakeQuest decide

    // Skip opposite-faction / hostile quest givers (only list own side + neutrals).
    if (playerFaction->IsHostileTo(*npcFaction))
        return false;
    return true;
}

struct MapQuestCand
{
    uint32 questId = 0;
    uint32 giverEntry = 0;
    float dist = 0.f;
    int questLevel = 0;
};

struct StarterSpawn
{
    uint16 mapId = 0;
    float x = 0.f;
    float y = 0.f;
};

// entry -> spawns (quest starters only). Built once — avoids full creature scans every SSE poll.
std::once_flag gStarterSpawnOnce;
std::unordered_map<uint32, std::vector<StarterSpawn>> gStarterSpawns;

void EnsureStarterSpawnIndex()
{
    std::call_once(gStarterSpawnOnce, [] {
        QuestRelations const* starters = sObjectMgr->GetCreatureQuestRelationMap();
        std::unordered_set<uint32> starterEntries;
        if (starters)
        {
            for (auto const& [entry, questId] : *starters)
            {
                (void)questId;
                starterEntries.insert(entry);
            }
        }
        if (starterEntries.empty())
            return;

        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            (void)spawnId;
            if (!starterEntries.count(data.id))
                continue;
            gStarterSpawns[data.id].push_back(StarterSpawn{
                uint16(data.mapid), data.posX, data.posY });
        }
    });
}
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

std::string MyBotsQuests::BuildNearbyAvailableJson(Player* player, float /*range*/, uint32 limit)
{
    if (!player)
        return "{\"ok\":false,\"code\":\"not_found\"}";
    if (limit == 0 || limit > 200)
        limit = 80;

    uint16 const mapId = uint16(player->GetMapId());
    uint32 const zoneId = player->GetZoneId();
    float const px = player->GetPositionX();
    float const py = player->GetPositionY();

    EnsureStarterSpawnIndex();

    QuestRelations const* starters = sObjectMgr->GetCreatureQuestRelationMap();
    std::vector<MapQuestCand> cands;
    std::unordered_map<uint32, size_t> bestByQuest; // questId -> index in cands

    if (starters)
    {
        for (auto const& [entry, questId] : *starters)
        {
            auto sit = gStarterSpawns.find(entry);
            if (sit == gStarterSpawns.end())
                continue;

            float bestDist = -1.f;
            for (StarterSpawn const& sp : sit->second)
            {
                if (sp.mapId != mapId)
                    continue;
                float const dx = sp.x - px;
                float const dy = sp.y - py;
                float const d = std::sqrt(dx * dx + dy * dy);
                if (bestDist < 0.f || d < bestDist)
                    bestDist = d;
            }
            if (bestDist < 0.f)
                continue;

            if (!CreatureTemplateFriendlyToPlayer(player, entry))
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            if (!QuestAllowsPlayerFaction(player, quest))
                continue;

            QuestStatus const st = player->GetQuestStatus(questId);
            if (st != QUEST_STATUS_NONE)
                continue;
            if (!player->CanTakeQuest(quest, false) || !player->CanAddQuest(quest, false))
                continue;

            MapQuestCand cand;
            cand.questId = questId;
            cand.giverEntry = entry;
            cand.dist = bestDist;
            cand.questLevel = quest->GetQuestLevel();

            auto bit = bestByQuest.find(questId);
            if (bit == bestByQuest.end())
            {
                bestByQuest[questId] = cands.size();
                cands.push_back(cand);
            }
            else if (cand.dist < cands[bit->second].dist)
            {
                cands[bit->second] = cand;
            }
        }
    }

    int const level = static_cast<int>(player->GetLevel());
    std::sort(cands.begin(), cands.end(), [level](MapQuestCand const& a, MapQuestCand const& b) {
        if (std::fabs(a.dist - b.dist) > 1.f)
            return a.dist < b.dist;
        int const da = std::abs((a.questLevel > 0 ? a.questLevel : level) - level);
        int const db = std::abs((b.questLevel > 0 ? b.questLevel : level) - level);
        if (da != db)
            return da < db;
        return a.questId < b.questId;
    });

    std::ostringstream items;
    bool first = true;
    uint32 count = 0;
    for (MapQuestCand const& cand : cands)
    {
        if (count >= limit)
            break;
        Quest const* quest = sObjectMgr->GetQuestTemplate(cand.questId);
        if (!quest)
            continue;
        uint32 turnin = FirstCreatureForQuest(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), cand.questId);
        if (!turnin)
            turnin = cand.giverEntry;
        AppendQuestItem(items, first, quest, QUEST_STATUS_NONE, cand.giverEntry, turnin, true);
        ++count;
    }

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"online\":true"
       << ",\"source\":\"map\""
       << ",\"map\":" << mapId
       << ",\"zone\":" << zoneId
       << ",\"x\":" << px
       << ",\"y\":" << py
       << ",\"guid\":" << player->GetGUID().GetCounter()
       << ",\"name\":\"" << MyBotsJsonEscapeCopy(player->GetName()) << "\""
       << ",\"level\":" << static_cast<uint32>(player->GetLevel())
       << ",\"note\":\"map_questgivers\""
       << ",\"items\":[" << items.str() << "]}";
    return ss.str();
}
