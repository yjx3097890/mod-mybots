#include "MyBotsIntentQueue.h"
#include "MyBotsConfig.h"
#include "MyBotsDirector.h"
#include "MyBotsExecutor.h"
#include "MyBotsJob.h"
#include "MyBotsLlm.h"
#include "MyBotsQuests.h"
#include "MyBotsSelfbot.h"
#include "MyBotsUtil.h"

#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "WorldSession.h"

#include <algorithm>
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

            if (intent.jobType == "complete_quest")
            {
                uint32 questId = 0;
                MyBotsExecutor::ParseUInt(intent.payload, "questId", questId);
                if (!questId)
                    MyBotsExecutor::ParseUInt(intent.payload, "quest_id", questId);

                // Hybrid planning: start with rule steps immediately so the bot
                // begins moving (including cross-map travel_to). If LLM is
                // enabled, also queue an async plan; when it returns successfully
                // ApplyLlmPlan replaces the remaining steps. On LLM failure the
                // rules keep running — no stall in Planning status.
                auto steps = MyBotsDirector::BuildCompleteQuest(questId, intent.payload, player);
                if (steps.size() <= 1)
                {
                    finish(400, "{\"ok\":false,\"code\":\"bad_job\",\"message\":\"could not build steps for job type\"}");
                    return;
                }
                auto job = sMyBotsJobStore.Create(guid, accountId, intent.jobType, intent.payload,
                    std::move(steps));
                // Snapshot the rule plan so /events shows whether travel_to was prepended.
                {
                    std::ostringstream msg;
                    msg << "{\"steps\":[";
                    for (size_t i = 0; i < job->steps.size(); ++i)
                    {
                        if (i)
                            msg << ",";
                        msg << "{\"op\":\"" << MyBotsJsonEscapeCopy(job->steps[i].op)
                            << "\",\"detail\":" << (job->steps[i].detail.empty() ? "{}" : job->steps[i].detail)
                            << "}";
                    }
                    msg << "]}";
                    std::string m = msg.str();
                    if (m.size() > 3500)
                    {
                        m.resize(3500);
                        m += "...(truncated)";
                    }
                    sMyBotsJobStore.AppendEvent(guid, job->id, "rules_plan", m);
                }
                if (MyBotsLlm::ShouldPlanCompleteQuest(questId, intent.payload))
                {
                    std::string context = MyBotsLlm::BuildPlanContext(player, questId, intent.payload);
                    MyBotsLlm::EnqueuePlan(job->id, guid, questId, std::move(context));
                    sMyBotsJobStore.AppendEvent(guid, job->id, "llm_plan_queued",
                        "quest:" + std::to_string(questId) + ";rules_first=1");
                    finish(202, "{\"ok\":true,\"code\":\"accepted\",\"jobId\":\""
                        + MyBotsJsonEscapeCopy(job->id) + "\",\"job\":"
                        + MyBotsDirector::JobToJson(*job)
                        + ",\"note\":\"rules_running_llm_pending\"}");
                    return;
                }
                // Make it visible in /events when LLM did not run (disabled, no key, custom steps).
                {
                    std::string why = "skipped";
                    if (!MyBotsLlm::IsReady())
                        why = sMyBotsConfig.LlmEnable() ? "no_api_key" : "llm_disabled";
                    else if (intent.payload.find("\"steps\"") != std::string::npos)
                        why = "payload_has_steps";
                    else
                        why = "db_script_or_filtered";
                    sMyBotsJobStore.AppendEvent(guid, job->id, "llm_skipped", why);
                }
                finish(202, "{\"ok\":true,\"code\":\"accepted\",\"jobId\":\"" + MyBotsJsonEscapeCopy(job->id)
                    + "\",\"job\":" + MyBotsDirector::JobToJson(*job) + "}");
                return;
            }

            auto steps = MyBotsDirector::BuildStepsForAssign(intent.jobType, intent.payload, player);
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
        case MyBotsIntentOp::ApplyLlmPlan:
        {
            auto job = sMyBotsJobStore.Get(intent.jobId);
            // Accept Planning (legacy) OR Queued/Running (hybrid rules-first).
            if (!job || (job->status != MyBotsJobStatus::Planning
                    && job->status != MyBotsJobStatus::Queued
                    && job->status != MyBotsJobStatus::Running
                    && job->status != MyBotsJobStatus::Paused))
            {
                finish(200, "{\"ok\":false,\"code\":\"ignored\"}");
                return;
            }
            uint32 questId = 0;
            MyBotsExecutor::ParseUInt(job->payload, "questId", questId);
            if (!questId)
                MyBotsExecutor::ParseUInt(job->payload, "quest_id", questId);

            bool const replan = intent.payload.find("\"replan\":1") != std::string::npos
                || intent.payload.find("\"replan\":true") != std::string::npos;
            bool const hybridLive = job->status != MyBotsJobStatus::Planning;

            std::string validateCtx = replan || hybridLive
                ? "{\"allowedEntries\":[],\"replan\":true}"
                : "{\"allowedEntries\":[]}";
            auto parsed = MyBotsLlm::ValidateAndParseSteps(questId, validateCtx, intent.payload);
            bool const tooThin = (replan || hybridLive) ? parsed.steps.empty() : (parsed.steps.size() <= 1);
            if (!parsed.ok || tooThin)
            {
                // Hybrid: rules are already running — ignore a failed LLM and keep them.
                if (hybridLive && !replan)
                {
                    sMyBotsJobStore.AppendEvent(job->charGuid, job->id, "llm_plan_ignored",
                        std::string("keep_rules;")
                            + (parsed.error.empty() ? "parse_failed" : parsed.error)
                            + (parsed.rawContent.empty()
                                ? ""
                                : ";raw=" + parsed.rawContent.substr(0,
                                    std::min<size_t>(parsed.rawContent.size(), 1500))));
                    finish(200, "{\"ok\":true,\"code\":\"keep_rules\"}");
                    return;
                }
                if (!replan && sMyBotsConfig.LlmFallbackRules())
                {
                    auto steps = MyBotsDirector::BuildCompleteQuest(questId, job->payload, player);
                    if (steps.size() > 1)
                    {
                        job->steps = std::move(steps);
                        for (size_t i = 0; i < job->steps.size(); ++i)
                            job->steps[i].ordinal = static_cast<int>(i);
                        job->stepIndex = 0;
                        job->status = MyBotsJobStatus::Queued;
                        job->error.clear();
                        sMyBotsJobStore.Save(*job);
                        sMyBotsJobStore.AppendEvent(job->charGuid, job->id, "llm_plan_fallback",
                            parsed.error.empty() ? "apply_parse_failed" : parsed.error);
                        finish(200, "{\"ok\":true,\"code\":\"fallback\"}");
                        return;
                    }
                }
                job->status = MyBotsJobStatus::Failed;
                job->error = replan ? "replan_failed" : "plan_failed";
                sMyBotsJobStore.Save(*job);
                sMyBotsJobStore.AppendEvent(job->charGuid, job->id,
                    replan ? "llm_replan_failed" : "llm_plan_failed", parsed.error);
                finish(200, "{\"ok\":false,\"code\":\"plan_failed\"}");
                return;
            }

            // Models often invent travel_to toward the giver (Lakeshire) even when
            // the quest is already held and the turn-in is elsewhere (Westfall).
            // Drop LLM travel_to; EnsureHubTravel re-injects only for real cross-map
            // using the status-aware hub (turn-in when incomplete/complete).
            {
                std::vector<MyBotsJobStep> kept;
                kept.reserve(parsed.steps.size());
                for (auto& s : parsed.steps)
                {
                    if (s.op == "travel_to")
                        continue;
                    kept.push_back(std::move(s));
                }
                parsed.steps = std::move(kept);
            }
            MyBotsDirector::EnsureHubTravel(player, questId, job->payload, parsed.steps);

            // Hybrid live OR explicit replan: keep finished prefix, replace remaining.
            if (replan || hybridLive)
            {
                int const from = std::max(0, job->stepIndex);
                std::vector<MyBotsJobStep> merged;
                merged.reserve(static_cast<size_t>(from) + parsed.steps.size());
                for (int i = 0; i < from && i < static_cast<int>(job->steps.size()); ++i)
                {
                    auto s = job->steps[static_cast<size_t>(i)];
                    if (s.status != MyBotsJobStatus::Succeeded)
                        s.status = MyBotsJobStatus::Succeeded;
                    merged.push_back(std::move(s));
                }
                for (auto& s : parsed.steps)
                {
                    s.status = MyBotsJobStatus::Queued;
                    s.result.clear();
                    merged.push_back(std::move(s));
                }
                job->steps = std::move(merged);
                for (size_t i = 0; i < job->steps.size(); ++i)
                    job->steps[i].ordinal = static_cast<int>(i);
                job->stepIndex = from;
                job->navAttempts = 0;
                job->detourUntil = 0;
                job->stuckSince = 0;
                job->moveIssuedAt = 0;
                if (job->status == MyBotsJobStatus::Planning)
                    job->status = MyBotsJobStatus::Queued;
                job->error.clear();
                sMyBotsJobStore.Save(*job);
                {
                    // Log remaining work from the job itself — parsed.steps was
                    // moved-from and would show empty op/detail.
                    std::ostringstream msg;
                    size_t const remain = job->steps.size() > static_cast<size_t>(from)
                        ? job->steps.size() - static_cast<size_t>(from)
                        : 0;
                    msg << "steps:" << remain
                        << (hybridLive ? ";switched_from_rules=1" : "")
                        << ";plan={\"steps\":[";
                    for (size_t i = static_cast<size_t>(from); i < job->steps.size(); ++i)
                    {
                        if (i > static_cast<size_t>(from))
                            msg << ",";
                        auto const& st = job->steps[i];
                        msg << "{\"op\":\"" << MyBotsJsonEscapeCopy(st.op)
                            << "\",\"detail\":" << (st.detail.empty() ? "{}" : st.detail)
                            << "}";
                    }
                    msg << "]}";
                    std::string m = msg.str();
                    if (m.size() > 3500)
                    {
                        m.resize(3500);
                        m += "...(truncated)";
                    }
                    sMyBotsJobStore.AppendEvent(job->charGuid, job->id,
                        replan ? "llm_replan_ok" : "llm_plan_ok", m);
                }
                finish(200, "{\"ok\":true,\"code\":\"" + std::string(replan ? "replanned" : "switched") + "\"}");
                return;
            }

            job->steps = std::move(parsed.steps);
            MyBotsDirector::EnsureHubTravel(player, questId, job->payload, job->steps);
            for (size_t i = 0; i < job->steps.size(); ++i)
                job->steps[i].ordinal = static_cast<int>(i);
            job->stepIndex = 0;
            job->status = MyBotsJobStatus::Queued;
            job->error.clear();
            sMyBotsJobStore.Save(*job);
            {
                std::ostringstream msg;
                msg << "steps:" << job->steps.size() << ";plan={\"steps\":[";
                for (size_t i = 0; i < job->steps.size(); ++i)
                {
                    if (i)
                        msg << ",";
                    msg << "{\"op\":\"" << MyBotsJsonEscapeCopy(job->steps[i].op)
                        << "\",\"detail\":" << (job->steps[i].detail.empty() ? "{}" : job->steps[i].detail)
                        << "}";
                }
                msg << "]}";
                std::string m = msg.str();
                if (m.size() > 3500)
                {
                    m.resize(3500);
                    m += "...(truncated)";
                }
                sMyBotsJobStore.AppendEvent(job->charGuid, job->id, "llm_plan_ok", m);
            }
            finish(200, "{\"ok\":true}");
            return;
        }
        case MyBotsIntentOp::ApplyRuleFallback:
        {
            auto job = sMyBotsJobStore.Get(intent.jobId);
            // Hybrid rules-first: job is already Queued/Running with rule steps —
            // a late LLM failure must not rewrite or fail it.
            if (!job)
            {
                finish(200, "{\"ok\":false,\"code\":\"ignored\"}");
                return;
            }
            if (job->status != MyBotsJobStatus::Planning)
            {
                sMyBotsJobStore.AppendEvent(job->charGuid, job->id, "llm_plan_ignored",
                    "fallback_while_rules_running");
                finish(200, "{\"ok\":true,\"code\":\"keep_rules\"}");
                return;
            }
            uint32 questId = 0;
            MyBotsExecutor::ParseUInt(job->payload, "questId", questId);
            if (!questId)
                MyBotsExecutor::ParseUInt(job->payload, "quest_id", questId);
            auto steps = MyBotsDirector::BuildCompleteQuest(questId, job->payload, player);
            if (steps.size() <= 1)
            {
                job->status = MyBotsJobStatus::Failed;
                job->error = "plan_failed";
                sMyBotsJobStore.Save(*job);
                sMyBotsJobStore.AppendEvent(job->charGuid, job->id, "llm_plan_failed", "fallback_empty");
                finish(200, "{\"ok\":false}");
                return;
            }
            job->steps = std::move(steps);
            for (size_t i = 0; i < job->steps.size(); ++i)
                job->steps[i].ordinal = static_cast<int>(i);
            job->stepIndex = 0;
            job->status = MyBotsJobStatus::Queued;
            job->error.clear();
            sMyBotsJobStore.Save(*job);
            std::string reason = "llm_failed";
            auto errPos = intent.payload.find("\"error\"");
            if (errPos != std::string::npos)
            {
                auto q1 = intent.payload.find('"', intent.payload.find(':', errPos) + 1);
                auto q2 = intent.payload.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                    reason = intent.payload.substr(q1 + 1, q2 - q1 - 1);
            }
            sMyBotsJobStore.AppendEvent(job->charGuid, job->id, "llm_plan_fallback", reason);
            finish(200, "{\"ok\":true,\"code\":\"fallback\"}");
            return;
        }
        case MyBotsIntentOp::FailPlan:
        {
            auto job = sMyBotsJobStore.Get(intent.jobId);
            if (!job)
            {
                finish(200, "{\"ok\":false,\"code\":\"ignored\"}");
                return;
            }
            if (job->status != MyBotsJobStatus::Planning)
            {
                sMyBotsJobStore.AppendEvent(job->charGuid, job->id, "llm_plan_ignored",
                    "fail_while_rules_running");
                finish(200, "{\"ok\":true,\"code\":\"keep_rules\"}");
                return;
            }
            job->status = MyBotsJobStatus::Failed;
            job->error = "plan_failed";
            sMyBotsJobStore.Save(*job);
            std::string reason = "plan_failed";
            auto errPos = intent.payload.find("\"error\"");
            if (errPos != std::string::npos)
            {
                auto q1 = intent.payload.find('"', intent.payload.find(':', errPos) + 1);
                auto q2 = intent.payload.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                    reason = intent.payload.substr(q1 + 1, q2 - q1 - 1);
            }
            sMyBotsJobStore.AppendEvent(job->charGuid, job->id, "llm_plan_failed", reason);
            finish(200, "{\"ok\":true}");
            return;
        }
        case MyBotsIntentOp::PlanDryRun:
        {
            uint32 questId = 0;
            MyBotsExecutor::ParseUInt(intent.payload, "questId", questId);
            if (!questId)
                MyBotsExecutor::ParseUInt(intent.payload, "quest_id", questId);
            if (!questId)
            {
                finish(400, "{\"ok\":false,\"code\":\"bad_request\",\"message\":\"questId required\"}");
                return;
            }
            std::string context = MyBotsLlm::BuildPlanContext(player, questId, intent.payload);
            // Return context immediately; HTTP thread runs PlanSync with long timeout.
            finish(200, "{\"ok\":true,\"code\":\"context\",\"questId\":" + std::to_string(questId)
                + ",\"context\":" + context + "}");
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
                MyBotsExecutor::HaltControl(player, job.get());
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
            MyBotsExecutor::HaltControl(player, job.get());
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
