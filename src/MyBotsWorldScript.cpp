#include "MyBotsConfig.h"
#include "MyBotsHttpServer.h"
#include "MyBotsIntentQueue.h"
#include "MyBotsJob.h"
#include "MyBotsLlm.h"

#include "Log.h"
#include "ScriptMgr.h"

class MyBotsWorldScript : public WorldScript
{
public:
    MyBotsWorldScript() : WorldScript("MyBotsWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_SHUTDOWN,
        WORLDHOOK_ON_UPDATE
    }), _accum(0)
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        sMyBotsConfig.Load(reload);
    }

    void OnStartup() override
    {
        if (!sMyBotsConfig.Enable())
        {
            LOG_INFO("module.mybots", "mod-mybots is disabled");
            return;
        }

        LOG_INFO("module.mybots", "mod-mybots started (Selfbot + Job director)");
        MyBotsHttpServer::Start();
        MyBotsLlm::Start();
    }

    void OnShutdown() override
    {
        MyBotsLlm::Stop();
        MyBotsHttpServer::Stop();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!sMyBotsConfig.Enable())
            return;

        sMyBotsIntentQueue.DrainOnWorldThread();

        _accum += diff;
        if (_accum >= sMyBotsConfig.DirectorTickMs())
        {
            _accum = 0;
            sMyBotsJobStore.TickAll(diff);
        }
    }

private:
    uint32 _accum;
};

void AddMyBotsWorldScripts()
{
    new MyBotsWorldScript();
}
