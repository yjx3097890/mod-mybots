#ifndef MYBOTS_LLM_H
#define MYBOTS_LLM_H

#include "MyBotsJob.h"

#include "Define.h"

#include <string>
#include <vector>

class Player;

struct MyBotsLlmPlanResult
{
    bool ok = false;
    std::string error;
    std::vector<MyBotsJobStep> steps;
    std::string rawContent;
};

class MyBotsLlm
{
public:
    static void Start();
    static void Stop();

    // True when Enable + non-empty ApiKey.
    static bool IsReady();

    // World-thread: assemble read-only planning context JSON for a quest.
    static std::string BuildPlanContext(Player* player, uint32 questId, std::string const& payload);

    // World-thread: context for high-level replan after nav stuck/unreachable.
    static std::string BuildReplanContext(Player* player, MyBotsJob const& job, std::string const& reason);

    // True when complete_quest should use async LLM (no hand steps / DB script).
    static bool ShouldPlanCompleteQuest(uint32 questId, std::string const& payload);

    // Worker queue: after job is created with status=planning (initial or replan).
    static void EnqueuePlan(std::string jobId, uint32 charGuid, uint32 questId, std::string contextJson,
        bool replan = false);

    // If LLM replan is allowed, pause the job as planning and enqueue. Returns true
    // when a replan was queued (caller must not fail the job yet).
    static bool TryRequestNavReplan(Player* player, MyBotsJob& job, std::string const& reason);

    // Synchronous plan (API / dry-run thread). Does not touch the world.
    static MyBotsLlmPlanResult PlanSync(uint32 questId, std::string const& contextJson);

    // Parse + validate model output against context allowlists.
    static MyBotsLlmPlanResult ValidateAndParseSteps(uint32 questId, std::string const& contextJson,
        std::string const& modelContent);
};

#endif
