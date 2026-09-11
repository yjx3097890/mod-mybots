#ifndef MYBOTS_NAV_H
#define MYBOTS_NAV_H

#include "Define.h"

#include <string>

class Player;

enum class MyBotsTaxiResult : uint8
{
    // No taxi leg is useful (short hop, disabled, no route, no money).
    Unavailable = 0,
    // Walking to the boarding node; caller should keep ticking move_to.
    Approaching,
    // Flight started; caller should wait until landing.
    Boarded
};

class MyBotsNav
{
public:
    // Long hops try a flight path before grinding the navmesh across a continent.
    static bool ShouldUseTaxi(Player* player, float x, float y, float z);
    // On Approaching, boardX/Y/Z receive the node to walk to; the caller issues the movement.
    static MyBotsTaxiResult TryTaxi(Player* player, float x, float y, float z,
        float& boardX, float& boardY, float& boardZ, std::string& detail);

    // Project a destination onto walkable ground so MovePoint does not aim at
    // floating spawn Z / DBC taxi Z / bad detour heights.
    static bool SnapToGround(Player* player, float& x, float& y, float& z);

    // Side offset used when the straight line keeps failing.
    static bool ComputeDetour(Player* player, float destX, float destY, float destZ, uint32 attempt,
        float& outX, float& outY, float& outZ);

    static void MarkBadPoint(Player* player, float x, float y, float z);
    static bool IsBadPoint(Player* player, float x, float y, float z);
    static void ClearBadPoints();
    static uint32 BadPointCount();
};

#endif
