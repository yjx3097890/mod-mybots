#ifndef MYBOTS_QUESTS_H
#define MYBOTS_QUESTS_H

#include "Define.h"

#include <string>

class Player;

namespace MyBotsQuests
{
// Live quest log from PLAYER_QUEST_LOG slots (in-memory, not characters DB).
std::string BuildQuestLogJson(Player* player);

// Quests the player can take from NPC starters on the current map (own faction).
// `range` is kept for API compatibility but ignored (map-wide listing).
// Live available quests on the character's current map (starter NPC spawn + CanTakeQuest).
std::string BuildNearbyAvailableJson(Player* player, float range = 80.f, uint32 limit = 80);
}

#endif
