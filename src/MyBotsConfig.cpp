#include "MyBotsConfig.h"
#include "Config.h"
#include "Log.h"

MyBotsConfig& MyBotsConfig::Instance()
{
    static MyBotsConfig instance;
    return instance;
}

void MyBotsConfig::Load(bool reload)
{
    _enable = sConfigMgr->GetOption<bool>("MyBots.Enable", true);
    _selfbotAllow = sConfigMgr->GetOption<bool>("MyBots.Selfbot.Allow", true);
    _selfbotSelfOnlyInGame = sConfigMgr->GetOption<bool>("MyBots.Selfbot.SelfOnlyInGame", true);
    _selfbotDisableRpgQuest = sConfigMgr->GetOption<bool>("MyBots.Selfbot.DisableRpgQuest", true);
    _selfbotIgnoreClientMovement = sConfigMgr->GetOption<bool>("MyBots.Selfbot.IgnoreClientMovement", true);
    _apiEnable = sConfigMgr->GetOption<bool>("MyBots.Api.Enable", true);
    _apiBind = sConfigMgr->GetOption<std::string>("MyBots.Api.Bind", "127.0.0.1");
    _apiPort = static_cast<uint16>(sConfigMgr->GetOption<uint32>("MyBots.Api.Port", 9100));
    _apiToken = sConfigMgr->GetOption<std::string>("MyBots.Api.Token", "change-me");
    _apiTimeoutMs = sConfigMgr->GetOption<uint32>("MyBots.Api.TimeoutMs", 3000);
    _apiQueueMax = sConfigMgr->GetOption<uint32>("MyBots.Api.QueueMax", 64);
    _jobReplace = sConfigMgr->GetOption<bool>("MyBots.Job.Replace", true);
    _stuckTimeoutSec = sConfigMgr->GetOption<uint32>("MyBots.Executor.StuckTimeoutSec", 45);
    _directorTickMs = sConfigMgr->GetOption<uint32>("MyBots.Director.TickMs", 1000);
    _navUseTaxi = sConfigMgr->GetOption<bool>("MyBots.Nav.UseTaxi", false);
    _navTaxiPartyFollow = sConfigMgr->GetOption<bool>("MyBots.Nav.TaxiPartyFollow", true);
    _navTaxiMinDistance = sConfigMgr->GetOption<float>("MyBots.Nav.TaxiMinDistance", 600.f);
    _navTaxiBoardDistance = sConfigMgr->GetOption<float>("MyBots.Nav.TaxiBoardDistance", 12.f);
    _navTaxiRetrySec = sConfigMgr->GetOption<uint32>("MyBots.Nav.TaxiRetrySec", 120);
    _navRepathSec = sConfigMgr->GetOption<uint32>("MyBots.Nav.RepathSec", 10);
    _navMaxStuckRetries = sConfigMgr->GetOption<uint32>("MyBots.Nav.MaxStuckRetries", 4);
    _navDetourRadius = sConfigMgr->GetOption<float>("MyBots.Nav.DetourRadius", 10.f);
    _navDetourSec = sConfigMgr->GetOption<uint32>("MyBots.Nav.DetourSec", 12);
    _navBadPointRadius = sConfigMgr->GetOption<float>("MyBots.Nav.BadPointRadius", 6.f);
    _navBadPointTtlSec = sConfigMgr->GetOption<uint32>("MyBots.Nav.BadPointTtlSec", 900);
    _navUseTravelMgr = sConfigMgr->GetOption<bool>("MyBots.Nav.UseTravelMgr", true);
    _llmEnable = sConfigMgr->GetOption<bool>("MyBots.Llm.Enable", false);
    _llmBaseUrl = sConfigMgr->GetOption<std::string>("MyBots.Llm.BaseUrl", "https://api.deepseek.com");
    _llmApiKey = sConfigMgr->GetOption<std::string>("MyBots.Llm.ApiKey", "");
    _llmModel = sConfigMgr->GetOption<std::string>("MyBots.Llm.Model", "deepseek-chat");
    _llmTimeoutMs = sConfigMgr->GetOption<uint32>("MyBots.Llm.TimeoutMs", 60000);
    _llmFallbackRules = sConfigMgr->GetOption<bool>("MyBots.Llm.FallbackRules", true);
    _llmMaxSteps = sConfigMgr->GetOption<uint32>("MyBots.Llm.MaxSteps", 32);
    if (_llmMaxSteps < 2)
        _llmMaxSteps = 2;
    if (_llmMaxSteps > 64)
        _llmMaxSteps = 64;
    _llmReplanOnStuck = sConfigMgr->GetOption<bool>("MyBots.Llm.ReplanOnStuck", true);
    _llmReplanCooldownSec = sConfigMgr->GetOption<uint32>("MyBots.Llm.ReplanCooldownSec", 90);
    _llmReplanMax = sConfigMgr->GetOption<uint32>("MyBots.Llm.ReplanMax", 2);

    LOG_INFO("module.mybots",
        "mod-mybots config loaded (reload={}): enable={} disableRpgQuest={} ignoreMove={} api={}:{} queueMax={} taxi={} taxiMinDist={} llm={} model={}",
        reload ? 1 : 0,
        _enable ? 1 : 0,
        _selfbotDisableRpgQuest ? 1 : 0,
        _selfbotIgnoreClientMovement ? 1 : 0,
        _apiBind,
        _apiPort,
        _apiQueueMax,
        _navUseTaxi ? 1 : 0,
        _navTaxiMinDistance,
        (_llmEnable && !_llmApiKey.empty()) ? 1 : 0,
        _llmModel);
}
