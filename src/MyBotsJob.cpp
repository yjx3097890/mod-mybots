#include "MyBotsJob.h"
#include "MyBotsDirector.h"
#include "MyBotsExecutor.h"
#include "MyBotsUtil.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "QueryResult.h"

#include <sstream>

namespace
{
std::string SqlEsc(std::string const& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in)
    {
        if (c == '\'')
            out += "''";
        else
            out += c;
    }
    return out;
}
}

MyBotsJobStore& MyBotsJobStore::Instance()
{
    static MyBotsJobStore instance;
    return instance;
}

std::shared_ptr<MyBotsJob> MyBotsJobStore::Create(uint32 charGuid, uint32 accountId, std::string type,
    std::string payload, std::vector<MyBotsJobStep> steps)
{
    auto job = std::make_shared<MyBotsJob>();
    job->id = MyBotsNewId("job");
    job->charGuid = charGuid;
    job->accountId = accountId;
    job->type = std::move(type);
    job->status = MyBotsJobStatus::Queued;
    job->payload = std::move(payload);
    job->createdAt = MyBotsNow();
    job->updatedAt = job->createdAt;
    job->steps = std::move(steps);
    for (size_t i = 0; i < job->steps.size(); ++i)
        job->steps[i].ordinal = static_cast<int>(i);

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _byId[job->id] = job;
        _activeByChar[charGuid] = job->id;
    }

    PersistInsert(*job);
    PersistSteps(*job);
    AppendEvent(charGuid, job->id, "job_created", job->type);
    return job;
}

std::shared_ptr<MyBotsJob> MyBotsJobStore::Get(std::string const& id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _byId.find(id);
    return it == _byId.end() ? nullptr : it->second;
}

std::shared_ptr<MyBotsJob> MyBotsJobStore::GetActiveForChar(uint32 charGuid)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _activeByChar.find(charGuid);
    if (it == _activeByChar.end())
        return nullptr;
    auto j = _byId.find(it->second);
    if (j == _byId.end() || !j->second)
        return nullptr;
    auto st = j->second->status;
    if (st == MyBotsJobStatus::Succeeded || st == MyBotsJobStatus::Failed || st == MyBotsJobStatus::Cancelled)
        return nullptr;
    return j->second;
}

std::vector<std::shared_ptr<MyBotsJob>> MyBotsJobStore::ListForChar(uint32 charGuid, uint32 limit)
{
    std::vector<std::shared_ptr<MyBotsJob>> out;
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto const& [id, job] : _byId)
    {
        if (job && job->charGuid == charGuid)
            out.push_back(job);
    }
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

void MyBotsJobStore::Save(MyBotsJob& job)
{
    job.updatedAt = MyBotsNow();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto st = job.status;
        if (st == MyBotsJobStatus::Succeeded || st == MyBotsJobStatus::Failed || st == MyBotsJobStatus::Cancelled)
        {
            auto it = _activeByChar.find(job.charGuid);
            if (it != _activeByChar.end() && it->second == job.id)
                _activeByChar.erase(it);
        }
        else
            _activeByChar[job.charGuid] = job.id;

        auto it = _byId.find(job.id);
        if (it == _byId.end())
            _byId[job.id] = std::make_shared<MyBotsJob>(job);
        else if (it->second.get() != &job)
            *it->second = job;
    }
    PersistUpdate(job);
    PersistSteps(job);
}

void MyBotsJobStore::CancelActive(uint32 charGuid, std::string const& reason)
{
    auto job = GetActiveForChar(charGuid);
    if (!job)
        return;
    job->status = MyBotsJobStatus::Cancelled;
    job->error = reason;

    if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid(HighGuid::Player, charGuid)))
        MyBotsExecutor::HaltControl(player, job.get());

    Save(*job);
    AppendEvent(charGuid, job->id, "job_cancelled", reason);
}

void MyBotsJobStore::AppendEvent(uint32 charGuid, std::string const& jobId, std::string const& kind,
    std::string const& message)
{
    CharacterDatabase.Execute(
        "INSERT INTO mybots_event (char_guid, job_id, kind, message, created_at) VALUES ({}, '{}', '{}', '{}', {})",
        charGuid, SqlEsc(jobId), SqlEsc(kind), SqlEsc(message), MyBotsNow());
}

std::string MyBotsJobStore::EventsJson(uint32 charGuid, uint32 limit)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT id, job_id, kind, message, created_at FROM mybots_event WHERE char_guid = {} ORDER BY id DESC LIMIT {}",
        charGuid, limit);
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            if (!first)
                ss << ",";
            first = false;
            ss << "{\"id\":" << f[0].Get<uint64>()
               << ",\"jobId\":\"" << MyBotsJsonEscapeCopy(f[1].Get<std::string>()) << "\""
               << ",\"kind\":\"" << MyBotsJsonEscapeCopy(f[2].Get<std::string>()) << "\""
               << ",\"message\":\"" << MyBotsJsonEscapeCopy(f[3].Get<std::string>()) << "\""
               << ",\"createdAt\":" << f[4].Get<uint32>() << "}";
        } while (result->NextRow());
    }
    ss << "]";
    return ss.str();
}

bool MyBotsJobStore::UpsertPatrol(MyBotsPatrol const& patrol)
{
    CharacterDatabase.Execute(
        "REPLACE INTO mybots_patrol (id, name, waypoints, account_id) VALUES ('{}', '{}', '{}', {})",
        SqlEsc(patrol.id), SqlEsc(patrol.name), SqlEsc(patrol.waypointsJson), patrol.accountId);
    return true;
}

std::optional<MyBotsPatrol> MyBotsJobStore::GetPatrol(std::string const& id)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT id, name, waypoints, account_id FROM mybots_patrol WHERE id = '{}'", SqlEsc(id));
    if (!result)
        return std::nullopt;
    Field* f = result->Fetch();
    MyBotsPatrol p;
    p.id = f[0].Get<std::string>();
    p.name = f[1].Get<std::string>();
    p.waypointsJson = f[2].Get<std::string>();
    p.accountId = f[3].Get<uint32>();
    return p;
}

std::string MyBotsJobStore::ListPatrolsJson(uint32 accountId)
{
    QueryResult result;
    if (accountId)
        result = CharacterDatabase.Query(
            "SELECT id, name, waypoints, account_id FROM mybots_patrol WHERE account_id = 0 OR account_id = {}",
            accountId);
    else
        result = CharacterDatabase.Query("SELECT id, name, waypoints, account_id FROM mybots_patrol");

    std::ostringstream ss;
    ss << "[";
    bool first = true;
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            if (!first)
                ss << ",";
            first = false;
            std::string wp = f[2].Get<std::string>();
            if (wp.empty())
                wp = "[]";
            ss << "{\"id\":\"" << MyBotsJsonEscapeCopy(f[0].Get<std::string>()) << "\""
               << ",\"name\":\"" << MyBotsJsonEscapeCopy(f[1].Get<std::string>()) << "\""
               << ",\"waypoints\":" << wp
               << ",\"accountId\":" << f[3].Get<uint32>() << "}";
        } while (result->NextRow());
    }
    ss << "]";
    return ss.str();
}

void MyBotsJobStore::PersistInsert(MyBotsJob const& job)
{
    CharacterDatabase.Execute(
        "INSERT INTO mybots_job (id, char_guid, account_id, type, status, payload, error, created_at, updated_at) "
        "VALUES ('{}', {}, {}, '{}', '{}', '{}', '{}', {}, {})",
        SqlEsc(job.id), job.charGuid, job.accountId, SqlEsc(job.type), MyBotsJobStatusName(job.status),
        SqlEsc(job.payload), SqlEsc(job.error), job.createdAt, job.updatedAt);
}

void MyBotsJobStore::PersistUpdate(MyBotsJob const& job)
{
    CharacterDatabase.Execute(
        "UPDATE mybots_job SET status='{}', payload='{}', error='{}', updated_at={} WHERE id='{}'",
        MyBotsJobStatusName(job.status), SqlEsc(job.payload), SqlEsc(job.error), job.updatedAt, SqlEsc(job.id));
}

void MyBotsJobStore::PersistSteps(MyBotsJob const& job)
{
    CharacterDatabase.Execute("DELETE FROM mybots_job_step WHERE job_id='{}'", SqlEsc(job.id));
    for (auto const& step : job.steps)
    {
        CharacterDatabase.Execute(
            "INSERT INTO mybots_job_step (job_id, ordinal, op, status, detail) VALUES ('{}', {}, '{}', '{}', '{}')",
            SqlEsc(job.id), step.ordinal, SqlEsc(step.op), MyBotsJobStatusName(step.status), SqlEsc(step.detail));
    }
}

void MyBotsJobStore::TickAll(uint32 /*nowMs*/)
{
    std::vector<std::shared_ptr<MyBotsJob>> active;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto const& [guid, id] : _activeByChar)
        {
            auto it = _byId.find(id);
            if (it != _byId.end() && it->second)
                active.push_back(it->second);
        }
    }
    for (auto& job : active)
        MyBotsDirector::TickJob(*job);
}
