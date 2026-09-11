#include "Chat.h"
#include "MyBotsConfig.h"
#include "MyBotsDirector.h"
#include "MyBotsJob.h"
#include "MyBotsNav.h"
#include "MyBotsSelfbot.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <string>

using namespace Acore::ChatCommands;

namespace
{
Player* ResolveTarget(ChatHandler* handler, Optional<std::string> const& name)
{
    if (name && !name->empty())
    {
        if (!handler->IsConsole() && sMyBotsConfig.SelfbotSelfOnlyInGame())
        {
            Player* self = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!self || self->GetSession()->GetSecurity() < SEC_GAMEMASTER)
            {
                handler->SendSysMessage("In-game players may only target themselves. Use the HTTP API from your backend.");
                return nullptr;
            }
        }

        Player* player = ObjectAccessor::FindPlayerByName(*name);
        if (!player)
            handler->SendSysMessage("Player not found or offline.");
        return player;
    }

    if (handler->IsConsole())
    {
        handler->SendSysMessage("Console usage requires a character name.");
        return nullptr;
    }

    if (!handler->GetSession())
        return nullptr;

    return handler->GetSession()->GetPlayer();
}

void Report(ChatHandler* handler, MyBotsResult const& result)
{
    handler->SendSysMessage(result.message);
}
} // namespace

class mybots_commandscript : public CommandScript
{
public:
    mybots_commandscript() : CommandScript("mybots_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable selfbotTable = {
            { "on", HandleSelfbotOn, SEC_PLAYER, Console::Yes },
            { "off", HandleSelfbotOff, SEC_PLAYER, Console::Yes },
        };

        static ChatCommandTable navTable = {
            { "status", HandleNavStatus, SEC_PLAYER, Console::Yes },
            { "clear", HandleNavClear, SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable mybotsTable = {
            { "status", HandleStatus, SEC_PLAYER, Console::Yes },
            { "selfbot", selfbotTable },
            { "nav", navTable },
            { "goto", HandleGoto, SEC_PLAYER, Console::No },
            { "cancel", HandleCancel, SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable = {
            { "mybots", mybotsTable },
        };

        return commandTable;
    }

    static bool HandleSelfbotOn(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = ResolveTarget(handler, name);
        if (!player)
            return false;

        Report(handler, MyBotsSelfbot::Enable(player));
        return true;
    }

    static bool HandleSelfbotOff(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = ResolveTarget(handler, name);
        if (!player)
            return false;

        Report(handler, MyBotsSelfbot::Disable(player));
        return true;
    }

    static bool HandleStatus(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = ResolveTarget(handler, name);
        if (!player)
            return false;

        MyBotsResult const result = MyBotsSelfbot::Status(player);
        handler->SendSysMessage(result.message);
        handler->SendSysMessage(MyBotsSelfbot::SnapshotJson(player));
        return true;
    }

    // Drive a move_to job from the client, so pathfinding can be tested
    // without going through the management backend.
    static bool HandleGoto(ChatHandler* handler, float x, float y, float z, Optional<float> dist)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        uint32 const guid = player->GetGUID().GetCounter();
        if (sMyBotsConfig.JobReplace())
            sMyBotsJobStore.CancelActive(guid, "replaced_by_goto");

        std::string payload = "{\"type\":\"move_to\",\"x\":" + std::to_string(x)
            + ",\"y\":" + std::to_string(y)
            + ",\"z\":" + std::to_string(z)
            + ",\"dist\":" + std::to_string(dist ? *dist : 2.5f) + "}";

        auto job = sMyBotsJobStore.Create(guid, player->GetSession()->GetAccountId(), "move_to", payload,
            MyBotsDirector::BuildStepsForAssign("move_to", payload));
        if (!job)
        {
            handler->SendSysMessage("MyBots: could not queue job.");
            return false;
        }

        handler->PSendSysMessage("MyBots: job {} queued.", job->id);
        return true;
    }

    static bool HandleCancel(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        sMyBotsJobStore.CancelActive(player->GetGUID().GetCounter(), "cancelled_in_game");
        handler->SendSysMessage("MyBots: active job cancelled.");
        return true;
    }

    static bool HandleNavStatus(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = ResolveTarget(handler, name);
        if (!player)
            return false;

        handler->PSendSysMessage("MyBots nav: {} bad points remembered.", MyBotsNav::BadPointCount());

        auto job = sMyBotsJobStore.GetActiveForChar(player->GetGUID().GetCounter());
        if (!job)
        {
            handler->SendSysMessage("MyBots nav: no active job.");
            return true;
        }

        if (job->stepIndex >= 0 && job->stepIndex < static_cast<int>(job->steps.size()))
        {
            auto const& step = job->steps[static_cast<size_t>(job->stepIndex)];
            handler->PSendSysMessage("MyBots nav: job {} step {} {} -> {}", job->id, job->stepIndex, step.op,
                step.result.empty() ? "pending" : step.result);
        }
        else
            handler->PSendSysMessage("MyBots nav: job {} has no current step.", job->id);

        return true;
    }

    static bool HandleNavClear(ChatHandler* handler)
    {
        MyBotsNav::ClearBadPoints();
        handler->SendSysMessage("MyBots nav: bad points cleared.");
        return true;
    }
};

void AddMyBotsCommandScripts()
{
    new mybots_commandscript();
}
