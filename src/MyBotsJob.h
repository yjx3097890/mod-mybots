#ifndef MYBOTS_JOB_H
#define MYBOTS_JOB_H

#include "Define.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class MyBotsJobStatus : uint8
{
    Queued = 0,
    Running,
    Paused,
    Succeeded,
    Failed,
    Cancelled,
    Planning
};

inline char const* MyBotsJobStatusName(MyBotsJobStatus s)
{
    switch (s)
    {
        case MyBotsJobStatus::Queued: return "queued";
        case MyBotsJobStatus::Running: return "running";
        case MyBotsJobStatus::Paused: return "paused";
        case MyBotsJobStatus::Succeeded: return "succeeded";
        case MyBotsJobStatus::Failed: return "failed";
        case MyBotsJobStatus::Cancelled: return "cancelled";
        case MyBotsJobStatus::Planning: return "planning";
    }
    return "unknown";
}

inline MyBotsJobStatus MyBotsJobStatusFromName(std::string const& s)
{
    if (s == "queued") return MyBotsJobStatus::Queued;
    if (s == "running") return MyBotsJobStatus::Running;
    if (s == "paused") return MyBotsJobStatus::Paused;
    if (s == "succeeded") return MyBotsJobStatus::Succeeded;
    if (s == "failed") return MyBotsJobStatus::Failed;
    if (s == "cancelled") return MyBotsJobStatus::Cancelled;
    if (s == "planning") return MyBotsJobStatus::Planning;
    return MyBotsJobStatus::Failed;
}

struct MyBotsJobStep
{
    int ordinal = 0;
    std::string op;
    MyBotsJobStatus status = MyBotsJobStatus::Queued;
    // Arguments the step was created with; must survive re-ticking.
    std::string detail;
    // Last outcome reported by the executor, runtime only.
    std::string result;
    uint32 retries = 0;
};

struct MyBotsJob
{
    std::string id;
    uint32 charGuid = 0;
    uint32 accountId = 0;
    std::string type;
    MyBotsJobStatus status = MyBotsJobStatus::Queued;
    std::string payload;
    std::string error;
    uint32 createdAt = 0;
    uint32 updatedAt = 0;
    std::vector<MyBotsJobStep> steps;
    int stepIndex = 0;
    // Runtime (not always persisted)
    float lastX = 0.f;
    float lastY = 0.f;
    float lastZ = 0.f;
    uint32 stuckSince = 0;
    uint32 waitUntil = 0;
    bool combatSuspended = false;
    int patrolIndex = 0;
    // Navigation runtime: issued destination, detour and taxi bookkeeping.
    float moveTargetX = 0.f;
    float moveTargetY = 0.f;
    float moveTargetZ = 0.f;
    uint32 moveIssuedAt = 0;
    uint32 navAttempts = 0;
    uint32 detourUntil = 0;
    float detourX = 0.f;
    float detourY = 0.f;
    float detourZ = 0.f;
    uint32 taxiRetryAt = 0;
    bool taxiInProgress = false;
    bool taxiSawFlight = false;
    uint32 taxiBoardedAt = 0;
    // Cached spawn position for move_to by creature entry.
    uint32 navSpawnEntry = 0;
    float navSpawnX = 0.f;
    float navSpawnY = 0.f;
    float navSpawnZ = 0.f;
    // Logical move request (before PrepareWalkTarget) — used to avoid repath churn.
    float moveReqX = 0.f;
    float moveReqY = 0.f;
    float moveReqZ = 0.f;
    uint32 lastLiftAt = 0;
    // until step temporarily enables grind; cleared when the step finishes.
    bool questGrindEnabled = false;
    uint32 questHuntEntry = 0;
    // LLM high-level replan after nav stuck (runtime).
    uint32 llmReplanCount = 0;
    uint32 lastLlmReplanAt = 0;
    // use_item cast bookkeeping (runtime).
    uint32 useItemPendingId = 0;
    uint32 useItemPendingAt = 0;
    // Cross-map travel runtime: when a move target lives on another map we route
    // there via rules (hearthstone / flight / boat) instead of walking a straight
    // line off the current map. 0xFFFFFFFF means "no cross-map leg active".
    uint32 travelDestMap = 0xFFFFFFFFu;
    uint32 travelStage = 0;        // rule cursor: 0=hearth, 1=flight, 2=transfer, ...
    uint32 travelActionAt = 0;     // last time we issued a travel action (sec)
    uint32 travelStuckSince = 0;   // cross-map progress watchdog (sec)
    uint32 hearthCastAt = 0;       // when we last triggered the hearthstone (sec)
    float travelLegX = 0.f;        // sub-destination for the current leg (e.g. dock)
    float travelLegY = 0.f;
    float travelLegZ = 0.f;
    bool travelLegSet = false;
};

struct MyBotsPatrol
{
    std::string id;
    std::string name;
    std::string waypointsJson;
    uint32 accountId = 0;
};

class MyBotsJobStore
{
public:
    static MyBotsJobStore& Instance();

    std::shared_ptr<MyBotsJob> Create(uint32 charGuid, uint32 accountId, std::string type, std::string payload,
        std::vector<MyBotsJobStep> steps);
    std::shared_ptr<MyBotsJob> Get(std::string const& id);
    std::shared_ptr<MyBotsJob> GetActiveForChar(uint32 charGuid);
    std::vector<std::shared_ptr<MyBotsJob>> ListForChar(uint32 charGuid, uint32 limit = 20);
    void Save(MyBotsJob& job);
    void CancelActive(uint32 charGuid, std::string const& reason);
    void AppendEvent(uint32 charGuid, std::string const& jobId, std::string const& kind, std::string const& message);
    std::string EventsJson(uint32 charGuid, uint32 limit = 50);

    bool UpsertPatrol(MyBotsPatrol const& patrol);
    std::optional<MyBotsPatrol> GetPatrol(std::string const& id);
    std::string ListPatrolsJson(uint32 accountId = 0);

    void TickAll(uint32 nowMs);

private:
    MyBotsJobStore() = default;
    void PersistInsert(MyBotsJob const& job);
    void PersistUpdate(MyBotsJob const& job);
    void PersistSteps(MyBotsJob const& job);

    std::mutex _mutex;
    std::unordered_map<std::string, std::shared_ptr<MyBotsJob>> _byId;
    std::unordered_map<uint32, std::string> _activeByChar;
};

#define sMyBotsJobStore MyBotsJobStore::Instance()

#endif
