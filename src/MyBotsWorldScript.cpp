#include "MyBotsConfig.h"
#include "MyBotsHttpServer.h"
#include "MyBotsIntentQueue.h"

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
    })
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

        LOG_INFO("module.mybots", "mod-mybots P0 started (director on top of mod-playerbots)");
        MyBotsHttpServer::Start();
    }

    void OnShutdown() override
    {
        MyBotsHttpServer::Stop();
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        if (sMyBotsConfig.Enable())
            sMyBotsIntentQueue.DrainOnWorldThread();
    }
};

void AddMyBotsWorldScripts()
{
    new MyBotsWorldScript();
}
