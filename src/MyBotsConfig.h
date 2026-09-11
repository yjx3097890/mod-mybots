#ifndef MYBOTS_CONFIG_H
#define MYBOTS_CONFIG_H

#include "Define.h"

#include <string>

class MyBotsConfig
{
public:
    static MyBotsConfig& Instance();

    void Load(bool reload);

    bool Enable() const { return _enable; }
    bool SelfbotAllow() const { return _selfbotAllow; }
    bool SelfbotSelfOnlyInGame() const { return _selfbotSelfOnlyInGame; }
    bool SelfbotDisableRpgQuest() const { return _selfbotDisableRpgQuest; }

    bool ApiEnable() const { return _apiEnable; }
    std::string const& ApiBind() const { return _apiBind; }
    uint16 ApiPort() const { return _apiPort; }
    std::string const& ApiToken() const { return _apiToken; }
    uint32 ApiTimeoutMs() const { return _apiTimeoutMs; }

private:
    MyBotsConfig() = default;

    bool _enable = true;
    bool _selfbotAllow = true;
    bool _selfbotSelfOnlyInGame = true;
    bool _selfbotDisableRpgQuest = true;
    bool _apiEnable = true;
    std::string _apiBind = "127.0.0.1";
    uint16 _apiPort = 9100;
    std::string _apiToken = "change-me";
    uint32 _apiTimeoutMs = 3000;
};

#define sMyBotsConfig MyBotsConfig::Instance()

#endif
