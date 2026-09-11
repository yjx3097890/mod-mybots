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
    Cancelled
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
    return MyBotsJobStatus::Failed;
}

struct MyBotsJobStep
{
    int ordinal = 0;
    std::string op;
    MyBotsJobStatus status = MyBotsJobStatus::Queued;
    std::string detail;
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
