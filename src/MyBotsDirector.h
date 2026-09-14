#ifndef MYBOTS_DIRECTOR_H
#define MYBOTS_DIRECTOR_H

#include "Define.h"

#include <string>
#include <vector>

class Player;
struct MyBotsJob;
struct MyBotsJobStep;

class MyBotsDirector
{
public:
    static void TickJob(MyBotsJob& job);

    static std::vector<MyBotsJobStep> BuildStepsForAssign(std::string const& type, std::string const& payload,
        Player* player = nullptr);
    static std::vector<MyBotsJobStep> BuildCompleteQuest(uint32 questId, std::string const& payload,
        Player* player = nullptr);
    // If the character is on another map than the quest hub, prepend travel_to
    // unless the remaining plan already starts with travel_to / hearthstone.
    static void EnsureHubTravel(Player* player, uint32 questId, std::string const& payload,
        std::vector<MyBotsJobStep>& steps);
    static std::vector<MyBotsJobStep> BuildMoveTo(std::string const& payload);
    static std::vector<MyBotsJobStep> BuildPatrol(std::string const& patrolId, std::string const& payload);

    static std::string JobToJson(MyBotsJob const& job);
};

#endif
