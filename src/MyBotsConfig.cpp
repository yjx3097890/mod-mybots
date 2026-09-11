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
    _apiEnable = sConfigMgr->GetOption<bool>("MyBots.Api.Enable", true);
    _apiBind = sConfigMgr->GetOption<std::string>("MyBots.Api.Bind", "127.0.0.1");
    _apiPort = static_cast<uint16>(sConfigMgr->GetOption<uint32>("MyBots.Api.Port", 9100));
    _apiToken = sConfigMgr->GetOption<std::string>("MyBots.Api.Token", "change-me");
    _apiTimeoutMs = sConfigMgr->GetOption<uint32>("MyBots.Api.TimeoutMs", 3000);

    LOG_INFO("module.mybots", "mod-mybots config loaded (reload={}): enable={} selfbotAllow={} disableRpgQuest={} api={}:{}",
        reload ? 1 : 0,
        _enable ? 1 : 0,
        _selfbotAllow ? 1 : 0,
        _selfbotDisableRpgQuest ? 1 : 0,
        _apiBind,
        _apiPort);
}
