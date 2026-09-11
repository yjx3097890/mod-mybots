#include "Chat.h"
#include "MyBotsConfig.h"
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

        static ChatCommandTable mybotsTable = {
            { "status", HandleStatus, SEC_PLAYER, Console::Yes },
            { "selfbot", selfbotTable },
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
};

void AddMyBotsCommandScripts()
{
    new mybots_commandscript();
}
