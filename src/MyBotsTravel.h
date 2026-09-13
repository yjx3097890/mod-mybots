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
    // map. Rule order (ported from playerbots' travel decisions):
    //   1. Hearthstone home when the bind point is on the destination map.
    //   2. (TODO) Flight-master hops within a continent.
    //   3. (TODO) Known inter-continent transfers (boat / zeppelin / portal).
    // Returns Arrived once the player is on destMap.
    static MyBotsTravelResult AdvanceCrossMap(Player* player, MyBotsJob& job,
        uint32 destMap, float x, float y, float z, std::string& detail);

    // Clear cross-map bookkeeping once a leg finishes or the job resets.
    static void Reset(MyBotsJob& job);
};

#endif
