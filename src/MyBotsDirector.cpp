#include "MyBotsDirector.h"
#include "MyBotsExecutor.h"
#include "MyBotsJob.h"
#include "MyBotsNav.h"
#include "MyBotsQuestPlan.h"
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
std::string ExtractJsonArray(std::string const& payload, char const* key)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = payload.find(needle);
    if (pos == std::string::npos)
        return {};
    pos = payload.find('[', pos);
    if (pos == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = pos; i < payload.size(); ++i)
    {
        if (payload[i] == '[')
            ++depth;
        else if (payload[i] == ']')
        {
            --depth;
            if (depth == 0)
                return payload.substr(pos, i - pos + 1);
        }
    }
    return {};
}

void ParseStepsArray(std::string const& arr, std::vector<MyBotsJobStep>& out)
{
    // Very small JSON array parser for objects: [{"op":"move_to","detail":"{...}"}, ...]
    size_t i = 0;
    while (i < arr.size())
    {
        auto opPos = arr.find("\"op\"", i);
        if (opPos == std::string::npos)
            break;
        auto colon = arr.find(':', opPos);
        auto q1 = arr.find('"', colon + 1);
        auto q2 = arr.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos)
            break;
        MyBotsJobStep step;
        step.op = arr.substr(q1 + 1, q2 - q1 - 1);
        auto dPos = arr.find("\"detail\"", q2);
        if (dPos != std::string::npos && dPos < arr.find('}', q2))
        {
            auto dc = arr.find(':', dPos);
            auto dq1 = arr.find('"', dc + 1);
            if (dq1 != std::string::npos)
            {
                // detail may be a string or raw object; prefer string
                if (arr[dq1] == '"' )
                {
                    std::string detail;
                    for (size_t k = dq1 + 1; k < arr.size(); ++k)
                    {
                        if (arr[k] == '\\' && k + 1 < arr.size())
                        {
                            detail.push_back(arr[k + 1]);
                            ++k;
                            continue;
                        }
                        if (arr[k] == '"')
                        {
                            step.detail = detail;
                            i = k + 1;
                            break;
                        }
                        detail.push_back(arr[k]);
                    }
                }
            }
        }
        else
        {
            // use whole object as detail for key extraction
            auto objStart = arr.rfind('{', opPos);
            auto objEnd = arr.find('}', q2);
            if (objStart != std::string::npos && objEnd != std::string::npos)
                step.detail = arr.substr(objStart, objEnd - objStart + 1);
            i = objEnd == std::string::npos ? arr.size() : objEnd + 1;
        }
        out.push_back(step);
        if (i <= opPos)
            i = q2 + 1;
    }
}
} // namespace

std::vector<MyBotsJobStep> MyBotsDirector::BuildMoveTo(std::string const& payload)
{
    std::vector<MyBotsJobStep> steps;
    MyBotsJobStep ensure;
    ensure.op = "ensure_selfbot";
    ensure.detail = "{}";
    steps.push_back(ensure);
    MyBotsJobStep move;
    move.op = "move_to";
    move.detail = payload;
    steps.push_back(move);
    return steps;
}

std::vector<MyBotsJobStep> MyBotsDirector::BuildCompleteQuest(uint32 questId, std::string const& payload)
{
    std::vector<MyBotsJobStep> steps;
    MyBotsJobStep ensure;
    ensure.op = "ensure_selfbot";
    ensure.detail = "{}";
    steps.push_back(ensure);

    // Explicit steps in the request always win.
    std::string arr = ExtractJsonArray(payload, "steps");
    if (!arr.empty())
    {
        ParseStepsArray(arr, steps);
        return steps;
    }

    // Optional hand-authored override in DB.
    {
        auto result = CharacterDatabase.Query(
            "SELECT script FROM mybots_quest_script WHERE quest_id = {}", questId);
        if (result)
        {
            std::string script = result->Fetch()[0].Get<std::string>();
            arr = ExtractJsonArray(script, "steps");
            if (arr.empty() && !script.empty() && script.front() == '[')
                arr = script;
            if (!arr.empty())
            {
                ParseStepsArray(arr, steps);
                return steps;
            }
        }
    }

    MyBotsQuestPlan const plan = MyBotsQuestPlanner::Resolve(questId, payload);
    if (!plan.error.empty())
    {
        // Keep a visible accept step so the job fails with a clear reason at runtime.
        MyBotsJobStep a;
        a.op = "accept_quest";
        a.detail = "{\"questId\":" + std::to_string(questId) + "}";
        steps.push_back(a);
        return steps;
    }

    // Accept
    if (plan.giverEntry)
    {
        MyBotsJobStep m;
        m.op = "move_to";
        m.detail = "{\"entry\":" + std::to_string(plan.giverEntry) + "}";
        steps.push_back(m);
        MyBotsJobStep a;
        a.op = "accept_quest";
        a.detail = "{\"questId\":" + std::to_string(questId) + ",\"entry\":" + std::to_string(plan.giverEntry) + "}";
        steps.push_back(a);
    }
    else
    {
        // Board/item starters: accept if possible, otherwise already_have is fine.
        MyBotsJobStep a;
        a.op = "accept_quest";
        a.detail = "{\"questId\":" + std::to_string(questId) + "}";
        steps.push_back(a);
    }

    // Hunt each objective creature, then keep grinding until the quest flips complete.
    // until itself also re-homes onto incomplete objectives every tick.
    for (uint32 entry : plan.objectiveEntries)
    {
        MyBotsJobStep m;
        m.op = "move_to";
        m.detail = "{\"entry\":" + std::to_string(entry) + ",\"dist\":25}";
        steps.push_back(m);
    }

    {
        MyBotsJobStep until;
        until.op = "until";
        std::ostringstream detail;
        detail << "{\"questId\":" << questId;
        if (!plan.objectiveEntries.empty())
        {
            detail << ",\"entries\":[";
            for (size_t i = 0; i < plan.objectiveEntries.size(); ++i)
            {
                if (i)
                    detail << ",";
                detail << plan.objectiveEntries[i];
            }
            detail << "]";
        }
        detail << "}";
        until.detail = detail.str();
        steps.push_back(until);
    }

    if (plan.turninEntry)
    {
        MyBotsJobStep m;
        m.op = "move_to";
        m.detail = "{\"entry\":" + std::to_string(plan.turninEntry) + "}";
        steps.push_back(m);
    }
    MyBotsJobStep t;
    t.op = "turnin_quest";
    t.detail = "{\"questId\":" + std::to_string(questId) + ",\"entry\":" + std::to_string(plan.turninEntry) + "}";
    steps.push_back(t);
    return steps;
}

std::vector<MyBotsJobStep> MyBotsDirector::BuildPatrol(std::string const& patrolId, std::string const& payload)
{
    std::vector<MyBotsJobStep> steps;
    MyBotsJobStep ensure;
    ensure.op = "ensure_selfbot";
    ensure.detail = "{}";
    steps.push_back(ensure);

    std::string waypoints = ExtractJsonArray(payload, "waypoints");
    if (waypoints.empty())
    {
        if (auto p = sMyBotsJobStore.GetPatrol(patrolId))
            waypoints = ExtractJsonArray("{\"waypoints\":" + p->waypointsJson + "}", "waypoints");
        if (waypoints.empty())
        {
            if (auto p = sMyBotsJobStore.GetPatrol(patrolId))
                waypoints = p->waypointsJson;
        }
    }

    // Expand each {x,y,z} into move_to + optional wait
    if (!waypoints.empty())
    {
        size_t pos = 0;
        while ((pos = waypoints.find('{', pos)) != std::string::npos)
        {
            auto end = waypoints.find('}', pos);
            if (end == std::string::npos)
                break;
            std::string obj = waypoints.substr(pos, end - pos + 1);
            float x = 0, y = 0, z = 0;
            if (MyBotsExecutor::ParseMoveXYZ(obj, x, y, z))
            {
                MyBotsJobStep m;
                m.op = "move_to";
                m.detail = obj;
                steps.push_back(m);
                uint32 waitSec = 0;
                if (MyBotsExecutor::ParseUInt(obj, "wait", waitSec) && waitSec)
                {
                    MyBotsJobStep w;
                    w.op = "wait";
                    w.detail = "{\"seconds\":" + std::to_string(waitSec) + "}";
                    steps.push_back(w);
                }
            }
            pos = end + 1;
        }
    }

    // Loop marker: director rewinds when finished if type==patrol
    return steps;
}

std::vector<MyBotsJobStep> MyBotsDirector::BuildStepsForAssign(std::string const& type, std::string const& payload)
{
    if (type == "move_to")
        return BuildMoveTo(payload);
    if (type == "complete_quest")
    {
        uint32 questId = 0;
        MyBotsExecutor::ParseUInt(payload, "questId", questId);
        if (!questId)
            MyBotsExecutor::ParseUInt(payload, "quest_id", questId);
        return BuildCompleteQuest(questId, payload);
    }
    if (type == "patrol")
    {
        // patrolId in payload
        std::string id;
        auto pos = payload.find("\"patrolId\"");
        if (pos != std::string::npos)
        {
            auto q1 = payload.find('"', payload.find(':', pos) + 1);
            auto q2 = payload.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                id = payload.substr(q1 + 1, q2 - q1 - 1);
        }
        return BuildPatrol(id, payload);
    }
    if (type == "script")
    {
        std::vector<MyBotsJobStep> steps;
        MyBotsJobStep ensure;
        ensure.op = "ensure_selfbot";
        ensure.detail = "{}";
        steps.push_back(ensure);
        std::string arr = ExtractJsonArray(payload, "steps");
        ParseStepsArray(arr, steps);
        return steps;
    }
    return BuildMoveTo(payload);
}

std::string MyBotsDirector::JobToJson(MyBotsJob const& job)
{
    std::ostringstream ss;
    ss << "{\"id\":\"" << MyBotsJsonEscapeCopy(job.id) << "\""
       << ",\"charGuid\":" << job.charGuid
       << ",\"accountId\":" << job.accountId
       << ",\"type\":\"" << MyBotsJsonEscapeCopy(job.type) << "\""
       << ",\"status\":\"" << MyBotsJobStatusName(job.status) << "\""
       << ",\"payload\":" << (job.payload.empty() ? "null" : job.payload)
       << ",\"error\":\"" << MyBotsJsonEscapeCopy(job.error) << "\""
       << ",\"stepIndex\":" << job.stepIndex
       << ",\"steps\":[";
    for (size_t i = 0; i < job.steps.size(); ++i)
    {
        if (i)
            ss << ",";
        auto const& s = job.steps[i];
        ss << "{\"ordinal\":" << s.ordinal
           << ",\"op\":\"" << MyBotsJsonEscapeCopy(s.op) << "\""
           << ",\"status\":\"" << MyBotsJobStatusName(s.status) << "\""
           << ",\"detail\":\"" << MyBotsJsonEscapeCopy(s.detail) << "\""
           << ",\"result\":\"" << MyBotsJsonEscapeCopy(s.result) << "\"}";
    }
    ss << "]}";
    return ss.str();
}

void MyBotsDirector::TickJob(MyBotsJob& job)
{
    if (job.status == MyBotsJobStatus::Paused)
        return;
    if (job.status == MyBotsJobStatus::Succeeded || job.status == MyBotsJobStatus::Failed
        || job.status == MyBotsJobStatus::Cancelled)
        return;

    Player* player = ObjectAccessor::FindPlayer(ObjectGuid(HighGuid::Player, job.charGuid));
    if (!player || !player->IsInWorld())
        return;

    if (job.status == MyBotsJobStatus::Queued)
    {
        job.status = MyBotsJobStatus::Running;
        sMyBotsJobStore.Save(job);
        sMyBotsJobStore.AppendEvent(job.charGuid, job.id, "job_running", job.type);
    }

    // Patrol combat suspend/resume
    if (job.type == "patrol")
    {
        if (player->IsInCombat())
        {
            job.combatSuspended = true;
            return;
        }
        if (job.combatSuspended)
        {
            job.combatSuspended = false;
            sMyBotsJobStore.AppendEvent(job.charGuid, job.id, "patrol_resume", "left_combat");
        }
    }

    if (player->isDead() || !player->IsAlive())
    {
        auto r = MyBotsExecutor::Revive(player);
        if (r.result != MyBotsStepResult::Done)
            return;
    }

    // Recover from a bad first MovePoint before the next step runs.
    MyBotsNav::CorrectIfUnderground(player);

    if (job.stepIndex < 0 || job.stepIndex >= static_cast<int>(job.steps.size()))
    {
        if (job.type == "patrol" && !job.steps.empty())
        {
            // rewind to first move after ensure_selfbot
            job.stepIndex = 1;
            for (size_t i = 1; i < job.steps.size(); ++i)
                job.steps[i].status = MyBotsJobStatus::Queued;
            sMyBotsJobStore.AppendEvent(job.charGuid, job.id, "patrol_loop", "rewind");
        }
        else
        {
            job.status = MyBotsJobStatus::Succeeded;
            MyBotsExecutor::HaltControl(player, &job);
            sMyBotsJobStore.Save(job);
            sMyBotsJobStore.AppendEvent(job.charGuid, job.id, "job_succeeded", "done");
            return;
        }
    }

    MyBotsJobStep& step = job.steps[static_cast<size_t>(job.stepIndex)];
    step.status = MyBotsJobStatus::Running;
    auto outcome = MyBotsExecutor::RunStep(player, job, step.op, step.detail);

    // Surface navigation escalations once each, so the management side can see
    // why a move is taking long instead of only seeing "running".
    if (outcome.detail != step.result
        && (outcome.detail == "detour_retry" || outcome.detail == "repath"
            || outcome.detail.rfind("taxi_", 0) == 0))
        sMyBotsJobStore.AppendEvent(job.charGuid, job.id, "nav", outcome.detail);

    step.result = outcome.detail;

    if (outcome.result == MyBotsStepResult::Done)
    {
        step.status = MyBotsJobStatus::Succeeded;
        job.stepIndex++;
        job.stuckSince = 0;
        job.waitUntil = 0;
        job.navAttempts = 0;
        job.detourUntil = 0;
        job.moveIssuedAt = 0;
        job.taxiRetryAt = 0;
        job.questHuntEntry = 0;
        // until clears grind itself on success; keep flag consistent across steps
        if (step.op != "until")
            job.questGrindEnabled = false;
        sMyBotsJobStore.Save(job);
    }
    else if (outcome.result == MyBotsStepResult::Failed)
    {
        step.status = MyBotsJobStatus::Failed;
        job.status = MyBotsJobStatus::Failed;
        job.error = outcome.detail;
        MyBotsExecutor::HaltControl(player, &job);
        sMyBotsJobStore.Save(job);
        sMyBotsJobStore.AppendEvent(job.charGuid, job.id, "job_failed", outcome.detail);
    }
    else
    {
        // Running / Waiting — persist lightly every so often via updated_at
        static uint32 lastPersist = 0;
        uint32 now = MyBotsNow();
        if (now != lastPersist)
        {
            lastPersist = now;
            // cheap in-memory only; skip DB spam
        }
    }
}
