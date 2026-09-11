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
    bool SelfbotIgnoreClientMovement() const { return _selfbotIgnoreClientMovement; }

    bool ApiEnable() const { return _apiEnable; }
    std::string const& ApiBind() const { return _apiBind; }
    uint16 ApiPort() const { return _apiPort; }
    std::string const& ApiToken() const { return _apiToken; }
    uint32 ApiTimeoutMs() const { return _apiTimeoutMs; }
    uint32 ApiQueueMax() const { return _apiQueueMax; }

    bool JobReplace() const { return _jobReplace; }
    uint32 StuckTimeoutSec() const { return _stuckTimeoutSec; }
    uint32 DirectorTickMs() const { return _directorTickMs; }

    bool NavUseTaxi() const { return _navUseTaxi; }
    float NavTaxiMinDistance() const { return _navTaxiMinDistance; }
    float NavTaxiBoardDistance() const { return _navTaxiBoardDistance; }
    uint32 NavTaxiRetrySec() const { return _navTaxiRetrySec; }
    uint32 NavRepathSec() const { return _navRepathSec; }
    uint32 NavMaxStuckRetries() const { return _navMaxStuckRetries; }
    float NavDetourRadius() const { return _navDetourRadius; }
    uint32 NavDetourSec() const { return _navDetourSec; }
    float NavBadPointRadius() const { return _navBadPointRadius; }
    uint32 NavBadPointTtlSec() const { return _navBadPointTtlSec; }
    bool NavUseTravelMgr() const { return _navUseTravelMgr; }

private:
    MyBotsConfig() = default;

    bool _enable = true;
    bool _selfbotAllow = true;
    bool _selfbotSelfOnlyInGame = true;
    bool _selfbotDisableRpgQuest = true;
    bool _selfbotIgnoreClientMovement = true;
    bool _apiEnable = true;
    std::string _apiBind = "127.0.0.1";
    uint16 _apiPort = 9100;
    std::string _apiToken = "change-me";
    uint32 _apiTimeoutMs = 3000;
    uint32 _apiQueueMax = 64;
    bool _jobReplace = true;
    uint32 _stuckTimeoutSec = 45;
    uint32 _directorTickMs = 1000;
    bool _navUseTaxi = true;
    float _navTaxiMinDistance = 600.f;
    float _navTaxiBoardDistance = 12.f;
    uint32 _navTaxiRetrySec = 120;
    uint32 _navRepathSec = 5;
    uint32 _navMaxStuckRetries = 4;
    float _navDetourRadius = 10.f;
    uint32 _navDetourSec = 12;
    float _navBadPointRadius = 6.f;
    uint32 _navBadPointTtlSec = 900;
    bool _navUseTravelMgr = true;
};

#define sMyBotsConfig MyBotsConfig::Instance()

#endif
