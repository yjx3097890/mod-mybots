#include "MyBotsNav.h"

#include "MyBotsConfig.h"
#include "MyBotsUtil.h"

#include "Creature.h"
#include "DBCStores.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SharedDefines.h"

#ifdef MYBOTS_HAVE_TRAVELMGR
// Playerbots.h first: TravelMgr.h relies on its AiObject/config headers.
#include "Playerbots.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#endif

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    struct MyBotsBadPoint
    {
        uint32 mapId = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        uint32 expiresAt = 0;
    };

    // Only touched from the world thread (executor/director), so no lock needed.
    std::vector<MyBotsBadPoint> _badPoints;

    void PruneBadPoints(uint32 now)
    {
        for (size_t i = 0; i < _badPoints.size();)
        {
            if (_badPoints[i].expiresAt <= now)
            {
                _badPoints[i] = _badPoints.back();
                _badPoints.pop_back();
            }
            else
                ++i;
        }
    }

    float Dist2d(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    // Path type of a short hop from where the character stands. Tells us whether
    // the navmesh is usable here at all, and whether we are standing on it.
    uint32 ProbeNearbyPathType(Player* player)
    {
        float const probeX = player->GetPositionX() + std::cos(player->GetOrientation()) * 6.f;
        float const probeY = player->GetPositionY() + std::sin(player->GetOrientation()) * 6.f;

        PathGenerator gen(player);
        if (!gen.CalculatePath(probeX, probeY, player->GetPositionZ(), /*forceDest=*/false))
            return PATHFIND_NOPATH;

        return gen.GetPathType();
    }

    // True when the character has no navmesh polygon under them, i.e. they fell
    // through a floor. Maps without mmaps report PATHFIND_NOT_USING_PATH and are
    // deliberately not treated as off-mesh.
    bool IsOffMesh(Player* player)
    {
        uint32 const type = ProbeNearbyPathType(player);
        if (type & PATHFIND_NOT_USING_PATH)
            return false;

        return (type & (PATHFIND_NOPATH | PATHFIND_FARFROMPOLY_START)) != 0;
    }

    // Whether we must insist on real navmesh routes. False while flying/swimming
    // or on a map that simply has no mmaps, where straight lines are expected.
    bool RequireMeshRoute(Player* player)
    {
        if (player->IsFlying() || player->isSwimming())
            return false;

        return (ProbeNearbyPathType(player) & PATHFIND_NOT_USING_PATH) == 0;
    }

    // Reject fake "paths": NOT_USING_PATH straight lines, SHORTCUT, or a 2-point
    // segment that is basically crow-flies over a long distance (walks through
    // walls/terrain and then circles when the generator gives up near the NPC).
    bool IsCredibleMeshPath(PathGenerator const& gen, Player* player, float destX, float destY, bool requireMesh)
    {
        uint32 const type = gen.GetPathType();
        if (requireMesh && (type & PATHFIND_NOT_USING_PATH))
            return false;
        if (requireMesh && (type & PATHFIND_SHORTCUT))
            return false;
        if (requireMesh && (type & PATHFIND_SHORT))
            return false;
        if (!(type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)))
            return false;

        Movement::PointsArray const& path = gen.GetPath();
        if (requireMesh && path.size() <= 2)
        {
            float const crow = Dist2d(player->GetPositionX(), player->GetPositionY(), destX, destY);
            if (crow > 20.f)
                return false;
        }
        return true;
    }

    // The destination sits outside the loaded navmesh. Instead of cutting a
    // straight line across the world, aim at the farthest point along the way
    // that still has a real route; mmap tiles load as we travel and later ticks
    // pick up the true destination.
    bool ApproachWaypoint(Player* player, float destX, float destY, float& outX, float& outY, float& outZ)
    {
        float const dist = player->GetExactDist2d(destX, destY);
        if (dist < 40.f)
            return false;

        float const dirX = (destX - player->GetPositionX()) / dist;
        float const dirY = (destY - player->GetPositionY()) / dist;

        static float const kSteps[] = { 160.f, 120.f, 80.f, 50.f, 30.f };
        for (float step : kSteps)
        {
            if (step >= dist)
                continue;

            float const cx = player->GetPositionX() + dirX * step;
            float const cy = player->GetPositionY() + dirY * step;

            float cz = player->GetMapHeight(cx, cy, player->GetPositionZ() + 20.f, true, 60.f);
            if (cz <= INVALID_HEIGHT)
                cz = player->GetMapHeight(cx, cy, player->GetPositionZ());
            if (cz <= INVALID_HEIGHT)
                continue;

            PathGenerator gen(player);
            if (!gen.CalculatePath(cx, cy, cz, /*forceDest=*/false))
                continue;

            if (!IsCredibleMeshPath(gen, player, cx, cy, true))
                continue;

            outX = cx;
            outY = cy;
            outZ = cz;
            return true;
        }

        return false;
    }

    Creature* FindNearbyFlightMaster(Player* player, float range)
    {
        if (!player)
            return nullptr;

#ifdef MYBOTS_HAVE_TRAVELMGR
        if (TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(player))
        {
            if (player->GetDistance(info->pos) <= range)
                if (Creature* c = player->FindNearestCreature(info->templateEntry, range, true))
                    if (c->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER))
                        return c;
        }
#endif
        // Fallback: scan a few common distances with any creature that has the flag.
        // WorldObject has no generic "all creatures in range" helper without an entry,
        // so without TravelMgr we refuse to board (approach-only still works).
        (void)range;
        return nullptr;
    }
}

bool MyBotsNav::ShouldUseTaxi(Player* player, float x, float y, float z)
{
    if (!player || !sMyBotsConfig.NavUseTaxi())
        return false;

    if (!player->IsAlive() || player->IsInCombat() || player->IsInFlight())
        return false;

    if (player->HasUnitState(UNIT_STATE_STUNNED) || player->HasUnitState(UNIT_STATE_ROOT))
        return false;

    return player->GetDistance(x, y, z) >= sMyBotsConfig.NavTaxiMinDistance();
}

MyBotsTaxiResult MyBotsNav::TryTaxi(Player* player, float x, float y, float z,
    float& boardX, float& boardY, float& boardZ, std::string& detail)
{
    if (!player)
        return MyBotsTaxiResult::Unavailable;

    uint32 const mapId = player->GetMapId();
    uint32 const teamId = player->GetTeamId();

    uint32 const srcNode = sObjectMgr->GetNearestTaxiNode(player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ(), mapId, teamId);
    uint32 const dstNode = sObjectMgr->GetNearestTaxiNode(x, y, z, mapId, teamId);
    if (!srcNode || !dstNode || srcNode == dstNode)
        return MyBotsTaxiResult::Unavailable;

    TaxiNodesEntry const* srcEntry = sTaxiNodesStore.LookupEntry(srcNode);
    TaxiNodesEntry const* dstEntry = sTaxiNodesStore.LookupEntry(dstNode);
    if (!srcEntry || !dstEntry || srcEntry->map_id != mapId || dstEntry->map_id != mapId)
        return MyBotsTaxiResult::Unavailable;

    // Flying is only worth it when the landing node is much closer to the goal than we are.
    float const selfToGoal = Dist2d(player->GetPositionX(), player->GetPositionY(), x, y);
    float const nodeToGoal = Dist2d(dstEntry->x, dstEntry->y, x, y);
    if (nodeToGoal >= selfToGoal * 0.6f)
        return MyBotsTaxiResult::Unavailable;

    // Prefer a direct DBC hop; otherwise use playerbots' BFS taxi graph for
    // multi-stop routes (Stormwind -> Menethil via Ironforge, etc.).
    std::vector<uint32> nodes;
    uint32 cost = 0;
    {
        uint32 path = 0;
        uint32 hopCost = 0;
        sObjectMgr->GetTaxiPath(srcNode, dstNode, path, hopCost);
        if (path)
        {
            nodes.push_back(srcNode);
            nodes.push_back(dstNode);
            cost = hopCost;
        }
    }
#ifdef MYBOTS_HAVE_TRAVELMGR
    if (nodes.empty())
    {
        std::vector<uint32> multi = sTravelNodeMap.FindTaxiPath(srcNode, dstNode);
        if (multi.size() >= 2)
        {
            uint32 total = 0;
            bool ok = true;
            for (size_t i = 1; i < multi.size(); ++i)
            {
                uint32 path = 0;
                uint32 hopCost = 0;
                sObjectMgr->GetTaxiPath(multi[i - 1], multi[i], path, hopCost);
                if (!path)
                {
                    ok = false;
                    break;
                }
                total += hopCost;
            }
            if (ok)
            {
                nodes = std::move(multi);
                cost = total;
            }
        }
    }
#endif
    if (nodes.size() < 2)
        return MyBotsTaxiResult::Unavailable;

    if (player->GetMoney() < cost)
    {
        detail = "taxi_no_money";
        return MyBotsTaxiResult::Unavailable;
    }

    float const toBoard = player->GetDistance(srcEntry->x, srcEntry->y, srcEntry->z);
    if (toBoard > sMyBotsConfig.NavTaxiBoardDistance())
    {
        boardX = srcEntry->x;
        boardY = srcEntry->y;
        boardZ = srcEntry->z;
        detail = "taxi_approach";
        return MyBotsTaxiResult::Approaching;
    }

    // ActivateTaxiPathTo(nullptr) is a script cheat that starts a flight from
    // anywhere near the node coords — that is the "suddenly on a gryphon in
    // the middle of Goldshire" bug. Only board through a real flight master.
    Creature* flightMaster = FindNearbyFlightMaster(player, sMyBotsConfig.NavTaxiBoardDistance() + 5.f);
    if (!flightMaster)
    {
        detail = "taxi_no_flightmaster";
        return MyBotsTaxiResult::Unavailable;
    }

    player->GetMotionMaster()->Clear();
    if (!player->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        detail = "taxi_refused";
        return MyBotsTaxiResult::Unavailable;
    }

    LOG_DEBUG("module.mybots", "MyBots: taxi {} -> {} ({} hops) for {} (cost {})",
        srcNode, dstNode, nodes.size() - 1, player->GetName(), cost);
    detail = nodes.size() > 2 ? "taxi_boarded_multi" : "taxi_boarded";
    return MyBotsTaxiResult::Boarded;
}

bool MyBotsNav::PrepareWalkTarget(Player* player, float& x, float& y, float& z)
{
    if (!player)
        return false;

    float const reqX = x;
    float const reqY = y;
    float const reqZ = z;

    // A floor this far below the target is a different storey, not a height
    // rounding error. Accepting one is how the bot ended up under the Pig and
    // Whistle while Harry stood on the floor above. Relaxed only after the
    // strict pass finds nothing at all.
    float maxDrop = 5.f;

    // When the destination's mmap tile is not loaded, CalculatePath happily
    // reports PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH and hands back a straight
    // line through terrain and buildings. Accepting that is what made the
    // character walk in a dead straight line into the ground.
    bool const requireMesh = RequireMeshRoute(player);

    float bestZ = INVALID_HEIGHT;
    float bestDelta = 0.f;
    bool foundNormal = false;
    bool foundAny = false;

    auto consider = [&](float hintZ)
    {
        if (hintZ <= INVALID_HEIGHT)
            return;

        float const candidateZ = player->GetMapHeight(reqX, reqY, hintZ);
        if (candidateZ <= INVALID_HEIGHT)
            return;

        if (candidateZ < reqZ - maxDrop)
            return;

        PathGenerator gen(player);
        if (!gen.CalculatePath(reqX, reqY, candidateZ, /*forceDest=*/false))
            return;

        if (!IsCredibleMeshPath(gen, player, reqX, reqY, requireMesh))
            return;

        uint32 const type = gen.GetPathType();
        bool const isNormal = (type & PATHFIND_NORMAL) != 0 && (type & PATHFIND_INCOMPLETE) == 0;
        bool const isIncomplete = (type & PATHFIND_INCOMPLETE) != 0;
        if (!isNormal && !isIncomplete)
            return;

        if (isIncomplete && gen.GetActualEndPosition().z < reqZ - maxDrop)
            return;

        // Pick the floor nearest the requested Z, never the shortest path: a
        // shorter path normally means it tunnelled under the building.
        float const delta = std::fabs(candidateZ - reqZ);
        if (isNormal)
        {
            if (!foundNormal || delta < bestDelta)
            {
                foundNormal = true;
                foundAny = true;
                bestDelta = delta;
                bestZ = candidateZ;
            }
            return;
        }

        if (!foundNormal && (!foundAny || delta < bestDelta))
        {
            foundAny = true;
            bestDelta = delta;
            bestZ = candidateZ;
        }
    };

    consider(reqZ);
    if (foundNormal && bestDelta <= 1.f)
    {
        x = reqX;
        y = reqY;
        z = bestZ;
        return true;
    }

    // Widen mostly upward (upper floors, ramps); downward stays shallow so we
    // cannot fall through to a basement or the terrain under a city WMO.
    static float const kStep = 2.f;
    for (int i = 1; i <= 8; ++i)
    {
        consider(reqZ + kStep * float(i));
        if (i <= 2)
            consider(reqZ - kStep * float(i));
    }

    if (!foundAny)
    {
        // The requested Z is not near any floor at all — hand written quest
        // coordinates or stale spawn data. Widen downward so the step can still
        // run; the strict pass above already had first refusal, so a floor next
        // to the target always beats the storey below it.
        maxDrop = 500.f;
        for (int i = 1; i <= 20; ++i)
            consider(reqZ - 5.f * float(i));
        consider(player->GetPositionZ());
    }

    if (!foundAny)
        return requireMesh && ApproachWaypoint(player, reqX, reqY, x, y, z);

    // Incomplete-only result over a long hop usually means the mesh ends before
    // the NPC — aiming at the final XY every tick makes the character walk a
    // straight segment then orbit. Prefer a reachable mid waypoint instead.
    if (requireMesh && !foundNormal)
    {
        float const crow = Dist2d(player->GetPositionX(), player->GetPositionY(), reqX, reqY);
        if (crow > 40.f)
        {
            float ax = 0.f, ay = 0.f, az = 0.f;
            if (ApproachWaypoint(player, reqX, reqY, ax, ay, az))
            {
                x = ax;
                y = ay;
                z = az;
                return true;
            }
        }
    }

    x = reqX;
    y = reqY;
    z = bestZ;
    return true;
}

void MyBotsNav::CorrectIfUnderground(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsInFlight() || player->IsFlying()
        || player->IsInWater())
        return;

    // Only relocate when the character actually left the navmesh. Standing under
    // a bridge or an overpass is legal and must not teleport anybody upstairs.
    if (!IsOffMesh(player))
        return;

    float const x = player->GetPositionX();
    float const y = player->GetPositionY();
    float const z = player->GetPositionZ();

    // Search from well above the character: GetMapHeight walks downward from the
    // hint, so a hint at the current Z can only ever find the terrain below a
    // city WMO, never the floor the character fell through.
    float ground = player->GetMapHeight(x, y, z + 40.f, true, 80.f);
    if (ground <= INVALID_HEIGHT || ground <= z + 1.5f)
        ground = player->GetMapHeight(x, y, z + 5.f);
    if (ground <= INVALID_HEIGHT)
        return;

    if (z + 1.5f >= ground)
        return;

    player->UpdateGroundPositionZ(x, y, ground);
    // UpdatePosition notifies the client; Relocate alone leaves them visually sunk.
    player->UpdatePosition(x, y, ground + 0.05f, player->GetOrientation(), true);
    LOG_DEBUG("module.mybots", "MyBots: lifted {} from underground ({:.1f} -> {:.1f})",
        player->GetName(), z, ground);
}

bool MyBotsNav::ComputeDetour(Player* player, float destX, float destY, float destZ, uint32 attempt,
    float& outX, float& outY, float& outZ)
{
    if (!player)
        return false;

    // Fan out left/right of the straight line, widening every full sweep.
    static float const kAngles[] = { 1.05f, -1.05f, 1.57f, -1.57f, 2.09f, -2.09f };
    static uint32 const kAngleCount = sizeof(kAngles) / sizeof(kAngles[0]);

    float const baseAngle = player->GetAngle(destX, destY);
    float const radius = sMyBotsConfig.NavDetourRadius() * float(1 + attempt / kAngleCount);

    for (uint32 i = 0; i < kAngleCount; ++i)
    {
        float const angle = baseAngle + kAngles[(attempt + i) % kAngleCount];
        float cx = player->GetPositionX() + std::cos(angle) * radius;
        float cy = player->GetPositionY() + std::sin(angle) * radius;
        float cz = player->GetPositionZ();

        if (!PrepareWalkTarget(player, cx, cy, cz))
            continue;

        if (std::fabs(cz - player->GetPositionZ()) > 12.f)
            continue;

        if (IsBadPoint(player, cx, cy, cz))
            continue;

        if (!player->IsWithinLOS(cx, cy, cz + 1.f))
            continue;

        outX = cx;
        outY = cy;
        outZ = cz;
        return true;
    }

    return false;
}

void MyBotsNav::MarkBadPoint(Player* player, float x, float y, float z)
{
    if (!player)
        return;

    uint32 const now = MyBotsNow();
    PruneBadPoints(now);

    if (IsBadPoint(player, x, y, z))
        return;

    MyBotsBadPoint p;
    p.mapId = player->GetMapId();
    p.x = x;
    p.y = y;
    p.z = z;
    p.expiresAt = now + sMyBotsConfig.NavBadPointTtlSec();
    _badPoints.push_back(p);
}

bool MyBotsNav::IsBadPoint(Player* player, float x, float y, float z)
{
    if (!player)
        return false;

    uint32 const now = MyBotsNow();
    float const radius = sMyBotsConfig.NavBadPointRadius();
    uint32 const mapId = player->GetMapId();

    for (MyBotsBadPoint const& p : _badPoints)
    {
        if (p.mapId != mapId || p.expiresAt <= now)
            continue;

        if (Dist2d(p.x, p.y, x, y) <= radius && std::fabs(p.z - z) <= radius)
            return true;
    }

#ifdef MYBOTS_HAVE_TRAVELMGR
    // Read-only reuse of what Playerbots already learned: grids whose navmesh
    // tile failed to load are not worth walking into.
    if (sMyBotsConfig.NavUseTravelMgr())
    {
        GridCoord const coord = Acore::ComputeGridCoord(x, y);
        if (sTravelMgr.isBadMmap(mapId, uint8(coord.x_coord), uint8(coord.y_coord)))
            return true;
    }
#endif

    return false;
}

void MyBotsNav::ClearBadPoints()
{
    _badPoints.clear();
}

uint32 MyBotsNav::BadPointCount()
{
    PruneBadPoints(MyBotsNow());
    return uint32(_badPoints.size());
}
