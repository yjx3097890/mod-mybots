#ifndef MYBOTS_TRAVEL_H
#define MYBOTS_TRAVEL_H

#include "Define.h"

#include <string>

class Player;
struct MyBotsJob;

// Result of advancing a cross-map travel leg. The executor uses this to decide
// whether to keep waiting, hand back to normal same-map navmesh movement, or
// fail the job safely instead of walking a straight line off the current map.
enum class MyBotsTravelResult : uint8
{
    Advancing = 0, // still routing toward destMap; keep ticking move_to
    Arrived,       // now on destMap; caller resumes same-map pathing
    Unreachable    // no rule could route there; fail (never straight-line)
};

class MyBotsTravel
{
public:
    // Route the player toward (x,y,z) on destMap when they are on a different
    // map. Rule order:
    //   1. Hearthstone home when the bind point is on the destination map.
    //   2. Walk to the nearest continent boat / zeppelin / portal boarding
    //      point on the current map, then wait for the transfer.
    //   3. Fail cleanly — never walk a straight line across the wrong map.
    // When a transfer approach leg is active, job.travelLeg* is set and the
    // executor issues same-map navmesh movement toward that point.
    static MyBotsTravelResult AdvanceCrossMap(Player* player, MyBotsJob& job,
        uint32 destMap, float x, float y, float z, std::string& detail);

    // Load continent boat/portal edges from playerbots_travelnode(+_link)
    // without pulling the 1.4M walk-path rows or writing TravelMgr. Idempotent.
    static void EnsureTransfersLoaded();

    // Clear cross-map bookkeeping once a leg finishes or the job resets.
    static void Reset(MyBotsJob& job);

    // Fire the hearthstone (playerbots "hearthstone" action, with a direct-cast
    // fallback). Used by both AdvanceCrossMap and the use_hearthstone op.
    static bool TriggerHearthstone(Player* player);
};

#endif
