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

    uint32 path = 0;
    uint32 cost = 0;
    sObjectMgr->GetTaxiPath(srcNode, dstNode, path, cost);
    if (!path)
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

    std::vector<uint32> nodes;
    nodes.push_back(srcNode);
    nodes.push_back(dstNode);

    player->GetMotionMaster()->Clear();
    if (!player->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        detail = "taxi_refused";
        return MyBotsTaxiResult::Unavailable;
    }

    LOG_DEBUG("module.mybots", "MyBots: taxi {} -> {} for {} (cost {})", srcNode, dstNode, player->GetName(), cost);
    detail = "taxi_boarded";
    return MyBotsTaxiResult::Boarded;
}

bool MyBotsNav::PrepareWalkTarget(Player* player, float& x, float& y, float& z)
{
    if (!player)
        return false;

    // Match Playerbots SearchForBestPath closely:
    // - keep the requested X/Y (do NOT replace with PathGenerator's actual end —
    //   incomplete paths often end inside terrain / under the mesh)
    // - only search for a walkable Z near the requested Z
    // - prefer PATHFIND_NORMAL over INCOMPLETE
    float const reqX = x;
    float const reqY = y;
    float const reqZ = z;

    float bestLen = 0.f;
    float bestZ = INVALID_HEIGHT;
    bool foundNormal = false;
    bool foundAny = false;

    auto consider = [&](float candidateZ, bool requireCloseToReq)
    {
        if (candidateZ <= INVALID_HEIGHT)
            return;

        // Re-sample floor at this candidate so we never keep a floating/sunk Z.
        float floorZ = player->GetMapHeight(reqX, reqY, candidateZ);
        if (floorZ <= INVALID_HEIGHT)
            return;
        if (std::fabs(floorZ - candidateZ) > 2.f)
            candidateZ = floorZ;

        if (requireCloseToReq && std::fabs(candidateZ - reqZ) > 0.5f)
            return;

        PathGenerator gen(player);
        if (!gen.CalculatePath(reqX, reqY, candidateZ, /*forceDest=*/false))
            return;

        uint32 const type = gen.GetPathType();
        bool const isNormal = (type & PATHFIND_NORMAL) != 0;
        bool const isIncomplete = (type & PATHFIND_INCOMPLETE) != 0;
        if (!isNormal && !isIncomplete)
            return;

        // Incomplete paths that dive far below the player are the usual "run
        // underground for a few seconds" symptom at quest start.
        G3D::Vector3 const& end = gen.GetActualEndPosition();
        if (isIncomplete && end.z < player->GetPositionZ() - 6.f)
            return;
        if (candidateZ < player->GetPositionZ() - 25.f
            && std::fabs(candidateZ - reqZ) > 8.f)
            return;

        float const len = gen.getPathLength();
        if (isNormal)
        {
            if (!foundNormal || len < bestLen)
            {
                foundNormal = true;
                foundAny = true;
                bestLen = len;
                bestZ = candidateZ;
            }
            return;
        }

        // Incomplete only if we still have no normal path.
        if (!foundNormal && (!foundAny || len < bestLen))
        {
            foundAny = true;
            bestLen = len;
            bestZ = candidateZ;
        }
    };

    // Exact hit near requested Z first (playerbots early-out).
    consider(player->GetMapHeight(reqX, reqY, reqZ), true);
    if (foundNormal)
    {
        x = reqX;
        y = reqY;
        z = bestZ;
        return true;
    }

    static float const kStep = 2.f;
    for (int i = 0; i <= 8; ++i)
        consider(player->GetMapHeight(reqX, reqY, reqZ + kStep * float(i)), false);
    for (int i = 1; i <= 8; ++i)
        consider(player->GetMapHeight(reqX, reqY, reqZ - kStep * float(i)), false);

    // Also try the player's own floor height at the destination XY — useful when
    // spawn Z is stale but the surface the character is already on continues there.
    consider(player->GetMapHeight(reqX, reqY, player->GetPositionZ()), false);

    if (!foundAny)
    {
        float fallback = reqZ;
        player->UpdateAllowedPositionZ(reqX, reqY, fallback);
        if (fallback <= INVALID_HEIGHT)
            return false;
        // Still refuse a fallback that would aim deep under the character.
        if (fallback < player->GetPositionZ() - 15.f)
            fallback = player->GetMapHeight(reqX, reqY, player->GetPositionZ());
        if (fallback <= INVALID_HEIGHT)
            return false;
        x = reqX;
        y = reqY;
        z = fallback;
        return true;
    }

    x = reqX;
    y = reqY;
    z = bestZ;
    return true;
}

void MyBotsNav::CorrectIfUnderground(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsInFlight() || player->IsFlying())
        return;

    float const x = player->GetPositionX();
    float const y = player->GetPositionY();
    float const z = player->GetPositionZ();
    float ground = player->GetMapHeight(x, y, z + 5.f);
    if (ground <= INVALID_HEIGHT)
        ground = player->GetMapHeight(x, y, z);
    if (ground <= INVALID_HEIGHT)
        return;

    // Already under the walkable mesh by a noticeable amount.
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
