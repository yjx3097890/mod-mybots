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
    // False for speak/deliver quests that only need accept → turn-in.
    bool hasObjectives = false;
    // True when the objective is gossip/event credit (do not grind-attack NPCs).
    bool speakObjective = false;
    std::string error;
};

class MyBotsQuestPlanner
{
public:
    // Payload may override giverEntry / turninEntry. Everything else is resolved
    // from ObjectMgr quest relations and the quest template objectives.
    static MyBotsQuestPlan Resolve(uint32 questId, std::string const& payload);
};

#endif
