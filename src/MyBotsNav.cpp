#include "MyBotsNav.h"

#include "MyBotsConfig.h"
#include "MyBotsUtil.h"

#include "DBCStores.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"

#ifdef MYBOTS_HAVE_TRAVELMGR
// Playerbots.h first: TravelMgr.h relies on its AiObject/config headers.
#include "Playerbots.h"
#include "TravelMgr.h"
#endif

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

    std::vector<uint32> nodes;
    nodes.push_back(srcNode);
    nodes.push_back(dstNode);

    player->GetMotionMaster()->Clear();
    if (!player->ActivateTaxiPathTo(nodes, nullptr, 1))
    {
        detail = "taxi_refused";
        return MyBotsTaxiResult::Unavailable;
    }

    LOG_DEBUG("module.mybots", "MyBots: taxi {} -> {} for {} (cost {})", srcNode, dstNode, player->GetName(), cost);
    detail = "taxi_boarded";
    return MyBotsTaxiResult::Boarded;
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

    Map* map = player->GetMap();
    if (!map)
        return false;

    for (uint32 i = 0; i < kAngleCount; ++i)
    {
        float const angle = baseAngle + kAngles[(attempt + i) % kAngleCount];
        float const cx = player->GetPositionX() + std::cos(angle) * radius;
        float const cy = player->GetPositionY() + std::sin(angle) * radius;

        float cz = map->GetHeight(player->GetPhaseMask(), cx, cy, player->GetPositionZ() + 5.f);
        if (cz < -50000.f)
            cz = player->GetPositionZ();

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
