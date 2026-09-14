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
    // Hub location of the quest (giver spawn preferred, else turn-in). Used to
    // prepend a cross-map travel_to when the character is on another continent.
    bool hasHub = false;
    uint32 hubMap = 0;
    float hubX = 0.f;
    float hubY = 0.f;
    float hubZ = 0.f;
    // False for speak/deliver quests that only need accept → turn-in.
    bool hasObjectives = false;
    // True when the objective is gossip/event credit (do not grind-attack NPCs).
    bool speakObjective = false;
    std::string error;

    bool HasSummonedObjective() const { return !summonedEntries.empty(); }
};

class MyBotsQuestPlanner
{
public:
    // Payload may override giverEntry / turninEntry. Everything else is resolved
    // from ObjectMgr quest relations and the quest template objectives.
    static MyBotsQuestPlan Resolve(uint32 questId, std::string const& payload);
};

#endif
