#include "MyBotsIntentQueue.h"
#include "MyBotsConfig.h"
#include "MyBotsDirector.h"
#include "MyBotsExecutor.h"
#include "MyBotsJob.h"
#include "MyBotsQuests.h"
#include "MyBotsSelfbot.h"
#include "MyBotsUtil.h"

#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "WorldSession.h"

#include <chrono>
#include <sstream>

MyBotsIntentQueue& MyBotsIntentQueue::Instance()
{
    static MyBotsIntentQueue instance;
    return instance;
}

std::shared_ptr<MyBotsIntent> MyBotsIntentQueue::Submit(MyBotsIntent intent)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_pending.size() >= sMyBotsConfig.ApiQueueMax())
        return nullptr;
    auto ptr = std::make_shared<MyBotsIntent>(std::move(intent));
    _pending.push_back(ptr);
    return ptr;
}

bool MyBotsIntentQueue::Wait(std::shared_ptr<MyBotsIntent> const& intent, uint32 timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);
    return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return intent->done; });
}

void MyBotsIntentQueue::DrainOnWorldThread()
{
    std::vector<std::shared_ptr<MyBotsIntent>> batch;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        batch.swap(_pending);
    }

    for (auto const& intent : batch)
        Execute(*intent);

    if (!batch.empty())
        _cv.notify_all();
}

void MyBotsIntentQueue::Execute(MyBotsIntent& intent)
{
    auto finish = [&](int status, std::string body) {
        intent.httpStatus = status;
        intent.response = std::move(body);
        intent.done = true;
    };

    if (intent.op == MyBotsIntentOp::ListPatrols)
    {
        finish(200, "{\"ok\":true,\"patrols\":" + sMyBotsJobStore.ListPatrolsJson(0) + "}");
        return;
    }

    if (intent.op == MyBotsIntentOp::UpsertPatrol)
    {
        MyBotsPatrol p;
        // payload: {"id":"...","name":"...","waypoints":[...],"accountId":0}
        auto idPos = intent.payload.find("\"id\"");
        if (idPos != std::string::npos)
        {
            auto q1 = intent.payload.find('"', intent.payload.find(':', idPos) + 1);
            auto q2 = intent.payload.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                p.id = intent.payload.substr(q1 + 1, q2 - q1 - 1);
        }
        auto namePos = intent.payload.find("\"name\"");
        if (namePos != std::string::npos)
        {
            auto q1 = intent.payload.find('"', intent.payload.find(':', namePos) + 1);
            auto q2 = intent.payload.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                p.name = intent.payload.substr(q1 + 1, q2 - q1 - 1);
        }
        auto wp = intent.payload.find("\"waypoints\"");
        if (wp != std::string::npos)
        {
            auto lb = intent.payload.find('[', wp);
            int depth = 0;
            for (size_t i = lb; i < intent.payload.size(); ++i)
            {
                if (intent.payload[i] == '[')
                    ++depth;
                else if (intent.payload[i] == ']')
                {
                    --depth;
                    if (depth == 0)
                    {
                        p.waypointsJson = intent.payload.substr(lb, i - lb + 1);
                        break;
                    }
                }
            }
        }
        MyBotsExecutor::ParseUInt(intent.payload, "accountId", p.accountId);
        if (p.id.empty())
        {
            finish(400, "{\"ok\":false,\"code\":\"bad_request\",\"message\":\"patrol id required\"}");
            return;
        }
        if (p.name.empty())
            p.name = p.id;
        if (p.waypointsJson.empty())
            p.waypointsJson = "[]";
        sMyBotsJobStore.UpsertPatrol(p);
        finish(200, "{\"ok\":true,\"patrol\":{\"id\":\"" + MyBotsJsonEscapeCopy(p.id) + "\"}}");
        return;
    }

    Player* player = nullptr;
    if (intent.guidLow)
        player = ObjectAccessor::FindPlayer(ObjectGuid(HighGuid::Player, intent.guidLow));
    else if (!intent.playerName.empty())
        player = ObjectAccessor::FindPlayerByName(intent.playerName);

    if (intent.op == MyBotsIntentOp::GetJob)
    {
        auto job = sMyBotsJobStore.Get(intent.jobId);
        if (!job)
        {
            finish(404, "{\"ok\":false,\"code\":\"not_found\",\"message\":\"job not found\"}");
            return;
        }
        finish(200, "{\"ok\":true,\"job\":" + MyBotsDirector::JobToJson(*job) + "}");
        return;
    }

    if (!player || !player->IsInWorld())
    {
        // List/cancel by guid may still work for DB listing when offline for list events
        if (intent.op == MyBotsIntentOp::ListEvents && intent.guidLow)
        {
            finish(200, "{\"ok\":true,\"events\":" + sMyBotsJobStore.EventsJson(intent.guidLow) + "}");
            return;
        }
        finish(409, "{\"ok\":false,\"code\":\"offline\",\"message\":\"Character is not online\"}");
        return;
    }

    uint32 const guid = player->GetGUID().GetCounter();
    uint32 const accountId = player->GetSession() ? player->GetSession()->GetAccountId() : 0;

    switch (intent.op)
    {
        case MyBotsIntentOp::Status:
            finish(200, MyBotsSelfbot::SnapshotJson(player));
            return;
        case MyBotsIntentOp::SelfbotOn:
        {
            auto result = MyBotsSelfbot::Enable(player);
            std::ostringstream ss;
            ss << "{\"ok\":" << (result.ok ? "true" : "false")
               << ",\"code\":\"" << result.code << "\",\"message\":\"" << MyBotsJsonEscapeCopy(result.message) << "\"";
            if (result.ok)
                ss << ",\"character\":" << MyBotsSelfbot::SnapshotJson(player);
            ss << "}";
            finish(result.httpStatus, ss.str());
            return;
        }
        case MyBotsIntentOp::SelfbotOff:
        {
            auto result = MyBotsSelfbot::Disable(player);
            std::ostringstream ss;
            ss << "{\"ok\":" << (result.ok ? "true" : "false")
               << ",\"code\":\"" << result.code << "\",\"message\":\"" << MyBotsJsonEscapeCopy(result.message) << "\"}";
            finish(result.httpStatus, ss.str());
            return;
        }
        case MyBotsIntentOp::AssignJob:
        {
            if (!intent.replace && !sMyBotsConfig.JobReplace())
            {
                if (auto active = sMyBotsJobStore.GetActiveForChar(guid))
                {
                    finish(409, "{\"ok\":false,\"code\":\"busy\",\"message\":\"character already has a running job\",\"jobId\":\""
                        + MyBotsJsonEscapeCopy(active->id) + "\"}");
                    return;
                }
            }
            else if (intent.replace || sMyBotsConfig.JobReplace())
                sMyBotsJobStore.CancelActive(guid, "replaced");

            auto steps = MyBotsDirector::BuildStepsForAssign(intent.jobType, intent.payload);
            if (steps.size() <= 1)
            {
                finish(400, "{\"ok\":false,\"code\":\"bad_job\",\"message\":\"could not build steps for job type\"}");
                return;
            }
            auto job = sMyBotsJobStore.Create(guid, accountId, intent.jobType, intent.payload, std::move(steps));
            finish(202, "{\"ok\":true,\"code\":\"accepted\",\"jobId\":\"" + MyBotsJsonEscapeCopy(job->id)
                + "\",\"job\":" + MyBotsDirector::JobToJson(*job) + "}");
            return;
        }
        case MyBotsIntentOp::CancelJob:
        {
            if (!intent.jobId.empty())
            {
                auto job = sMyBotsJobStore.Get(intent.jobId);
                if (!job || job->charGuid != guid)
                {
                    finish(404, "{\"ok\":false,\"code\":\"not_found\"}");
                    return;
                }
                job->status = MyBotsJobStatus::Cancelled;
                job->error = "cancelled";
                sMyBotsJobStore.Save(*job);
                sMyBotsJobStore.AppendEvent(guid, job->id, "job_cancelled", "api");
                finish(200, "{\"ok\":true,\"job\":" + MyBotsDirector::JobToJson(*job) + "}");
                return;
            }
            sMyBotsJobStore.CancelActive(guid, "cancelled");
            finish(200, "{\"ok\":true,\"code\":\"cancelled\"}");
            return;
        }
        case MyBotsIntentOp::PauseJob:
        {
            auto job = intent.jobId.empty() ? sMyBotsJobStore.GetActiveForChar(guid) : sMyBotsJobStore.Get(intent.jobId);
            if (!job)
            {
                finish(404, "{\"ok\":false,\"code\":\"not_found\"}");
                return;
            }
            job->status = MyBotsJobStatus::Paused;
            sMyBotsJobStore.Save(*job);
            finish(200, "{\"ok\":true,\"job\":" + MyBotsDirector::JobToJson(*job) + "}");
            return;
        }
        case MyBotsIntentOp::ResumeJob:
        {
            std::shared_ptr<MyBotsJob> job;
            if (!intent.jobId.empty())
                job = sMyBotsJobStore.Get(intent.jobId);
            else
            {
                auto list = sMyBotsJobStore.ListForChar(guid);
                for (auto& j : list)
                    if (j && j->status == MyBotsJobStatus::Paused)
                    {
                        job = j;
                        break;
                    }
            }
            if (!job)
            {
                finish(404, "{\"ok\":false,\"code\":\"not_found\"}");
                return;
            }
            job->status = MyBotsJobStatus::Running;
            sMyBotsJobStore.Save(*job);
            finish(200, "{\"ok\":true,\"job\":" + MyBotsDirector::JobToJson(*job) + "}");
            return;
        }
        case MyBotsIntentOp::ListJobs:
        {
            auto list = sMyBotsJobStore.ListForChar(guid);
            std::ostringstream ss;
            ss << "{\"ok\":true,\"jobs\":[";
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (i)
                    ss << ",";
                ss << MyBotsDirector::JobToJson(*list[i]);
            }
            ss << "]}";
            finish(200, ss.str());
            return;
        }
        case MyBotsIntentOp::ListEvents:
            finish(200, "{\"ok\":true,\"events\":" + sMyBotsJobStore.EventsJson(guid) + "}");
            return;
        case MyBotsIntentOp::QuestLog:
            finish(200, MyBotsQuests::BuildQuestLogJson(player));
            return;
        case MyBotsIntentOp::QuestsAvailable:
            finish(200, MyBotsQuests::BuildNearbyAvailableJson(player));
            return;
        default:
            finish(404, "{\"ok\":false,\"code\":\"not_found\"}");
            return;
    }
}
