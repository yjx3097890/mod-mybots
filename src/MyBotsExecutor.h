#ifndef MYBOTS_EXECUTOR_H
#define MYBOTS_EXECUTOR_H

#include "Define.h"

#include <string>

class Player;
struct MyBotsJob;

enum class MyBotsStepResult : uint8
{
    Running = 0,
    Done,
    Failed,
    Waiting
};

struct MyBotsStepOutcome
{
    MyBotsStepResult result = MyBotsStepResult::Failed;
    std::string detail;
};

class MyBotsExecutor
{
public:
    static MyBotsStepOutcome RunStep(Player* player, MyBotsJob& job, std::string const& op, std::string const& detail);

    // targetMap != 0 pins the destination to a specific map. When the player is
    // on another map the move is routed cross-map (hearthstone/flight/boat) via
    // MyBotsTravel instead of walking a straight line off the current map.
    static MyBotsStepOutcome MoveTo(Player* player, MyBotsJob& job, float x, float y, float z, float dist = 2.5f,
        uint32 targetMap = 0);
    static MyBotsStepOutcome MoveToCreature(Player* player, MyBotsJob& job, uint32 entry, float dist = 3.f);
    static MyBotsStepOutcome Interact(Player* player, uint32 entry);
    static MyBotsStepOutcome UseItem(Player* player, MyBotsJob& job, uint32 itemId);
    static MyBotsStepOutcome GossipSelect(Player* player, uint32 entry, uint32 menu, uint32 option);
    static MyBotsStepOutcome AcceptQuest(Player* player, uint32 questId, uint32 giverEntry = 0);
    static MyBotsStepOutcome TurnInQuest(Player* player, uint32 questId, uint32 giverEntry = 0);
    static MyBotsStepOutcome WaitUntil(Player* player, MyBotsJob& job, uint32 seconds);
    static MyBotsStepOutcome UntilQuestComplete(Player* player, MyBotsJob& job, uint32 questId, std::string const& detail);
    static void ClearQuestCombat(Player* player, MyBotsJob& job);
    // Stop MovePoint / combat / grind left over from a job or Selfbot session.
    static void HaltControl(Player* player, MyBotsJob* job = nullptr);
    static MyBotsStepOutcome Revive(Player* player);
    static MyBotsStepOutcome EnsureSelfbot(Player* player);

    static bool ParseMoveXYZ(std::string const& detail, float& x, float& y, float& z);
    static bool ParseUInt(std::string const& detail, char const* key, uint32& out);
    static bool ParseFloat(std::string const& detail, char const* key, float& out);
};

#endif
