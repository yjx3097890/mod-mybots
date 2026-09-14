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

    // Resolve a walkable destination the same way Playerbots does: GetMapHeight
    // near the requested Z + PathGenerator. Never search from the sky — that
    // snaps onto cave floors and sinks the character.
    static bool PrepareWalkTarget(Player* player, float& x, float& y, float& z);

    // If a previous bad move already put the character under the mesh, yank
    // them back onto the floor under their feet before issuing another move.
    static void CorrectIfUnderground(Player* player);

    // True when (x,y,z) sits in a deep water column (Stormwind canals, etc.).
    // Includes targeting the water *surface* over a deep bed (boat nodes at z≈0).
    static bool IsDeepWaterAt(Player* player, float x, float y, float z);

    // When swimming but the logical destination is on land, pick a nearby dry
    // bank toward the goal so we climb out instead of swimming the canals.
    // Returns true and fills outX/Y/Z when an exit point was found.
    static bool TryExitWaterToward(Player* player, float destX, float destY, float destZ,
        float& outX, float& outY, float& outZ);

    // TravelMgr / travelnode ship points sit in the water. Snap to the nearest
    // dry pier/dock so transfer_approach walks the harbor street, not the canal.
    // Portals that are already on land only get a Z snap. Returns false if no
    // dry ground was found nearby (caller may keep the raw point).
    static bool ResolveBoardingDock(Player* player, float& x, float& y, float& z);

    // Side offset used when the straight line keeps failing.
    static bool ComputeDetour(Player* player, float destX, float destY, float destZ, uint32 attempt,
        float& outX, float& outY, float& outZ);

    static void MarkBadPoint(Player* player, float x, float y, float z);
    static bool IsBadPoint(Player* player, float x, float y, float z);
    static void ClearBadPoints();
    static uint32 BadPointCount();
};

#endif
