#ifndef MYBOTS_QUEST_PLAN_H
#define MYBOTS_QUEST_PLAN_H

#include "Define.h"

#include <string>
#include <vector>

struct MyBotsQuestPlan
{
    uint32 questId = 0;
    uint32 giverEntry = 0;
    uint32 turninEntry = 0;
    // Creature entries the character should hunt / speak with while incomplete.
    std::vector<uint32> objectiveEntries;
    // Kill targets with no creature table spawn (must be summoned, e.g. 5676).
    std::vector<uint32> summonedEntries;
    // Quest StartItem used at a spell-focus / summoning circle (Bloodstone Choker).
    uint32 useItemId = 0;
    // Nearest summoning-circle GO near the turn-in / giver.
    bool hasSummonSite = false;
    float summonX = 0.f;
    float summonY = 0.f;
    float summonZ = 0.f;
    // Hub location of the quest. Prefer turn-in when the character already has
    // the quest; otherwise prefer giver. Used for travel_to / LLM hints.
    // hubMap 0 (Eastern Kingdoms) is valid — never treat 0 as "missing".
    bool hasHub = false;
    uint32 hubMap = 0;
    float hubX = 0.f;
    float hubY = 0.f;
    float hubZ = 0.f;
    uint32 hubEntry = 0; // which NPC the hub coords came from
    bool hubIsTurnin = false;
    // Summoning-circle map (usually same as hub). Raw xyz move_to must carry this
    // or the bot walks those coordinates on the wrong continent.
    uint32 summonMap = 0;
    // False for speak/deliver quests that only need accept → turn-in.
    bool hasObjectives = false;
    // True when the objective is gossip/event credit (do not grind-attack NPCs).
    bool speakObjective = false;
    std::string error;

    bool HasSummonedObjective() const { return !summonedEntries.empty(); }
};

class Player;

class MyBotsQuestPlanner
{
public:
    // Payload may override giverEntry / turninEntry. Everything else is resolved
    // from ObjectMgr quest relations and the quest template objectives.
    // When player already has the quest (incomplete/complete), hub prefers the
    // turn-in NPC so we do not fly back to the giver (e.g. Lakeshire Wiley while
    // turning in to Westfall Gryan).
    static MyBotsQuestPlan Resolve(uint32 questId, std::string const& payload, Player* player = nullptr);
};

#endif
