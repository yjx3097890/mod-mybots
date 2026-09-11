#include "MyBotsSelfbot.h"
#include "MyBotsConfig.h"

#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "WorldSession.h"

#include <sstream>

namespace
{
MyBotsResult Fail(int status, std::string const& code, std::string const& message)
{
    MyBotsResult r;
    r.ok = false;
    r.httpStatus = status;
    r.code = code;
    r.message = message;
    return r;
}

MyBotsResult Ok(std::string const& code, std::string const& message)
{
    MyBotsResult r;
    r.ok = true;
    r.httpStatus = 200;
    r.code = code;
    r.message = message;
    return r;
}

void JsonEscape(std::string const& in, std::string& out)
{
    out.clear();
    out.reserve(in.size() + 8);
    for (char c : in)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
}

void DisableAutonomousQuesting(PlayerbotAI* ai)
{
    if (!ai || !sMyBotsConfig.SelfbotDisableRpgQuest())
        return;

    // Drop Playerbots' own quest/travel brain so this module can direct later.
    ai->ChangeStrategy("-rpg quest,-travel,-rpg", BOT_STATE_NON_COMBAT);
}
} // namespace

MyBotsResult MyBotsSelfbot::Enable(Player* player)
{
    if (!sMyBotsConfig.Enable())
        return Fail(503, "disabled", "mod-mybots is disabled");

    if (!sMyBotsConfig.SelfbotAllow())
        return Fail(403, "selfbot_disabled", "Selfbot wrapping is disabled in mybots.conf");

    if (!player || !player->IsInWorld())
        return Fail(409, "offline", "Character is not in the world");

    if (!sPlayerbotAIConfig.enabled)
        return Fail(503, "playerbots_disabled", "AiPlayerbot.Enabled is 0");

    if (PlayerbotAI* existing = GET_PLAYERBOT_AI(player))
    {
        existing->SetMaster(player);
        DisableAutonomousQuesting(existing);
        LOG_INFO("module.mybots", "Selfbot already active for {}", player->GetName());
        return Ok("already_on", "Selfbot already enabled");
    }

    PlayerbotsMgr::instance().AddPlayerbotData(player, true);
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai)
        return Fail(500, "ai_create_failed", "PlayerbotsMgr did not attach PlayerbotAI");

    ai->SetMaster(player);
    DisableAutonomousQuesting(ai);

    LOG_INFO("module.mybots", "Selfbot enabled for {} guid={}", player->GetName(), player->GetGUID().GetCounter());
    return Ok("enabled", "Selfbot enabled");
}

MyBotsResult MyBotsSelfbot::Disable(Player* player)
{
    if (!player)
        return Fail(404, "not_found", "Character not found");

    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai)
        return Ok("already_off", "Selfbot already disabled");

    ObjectGuid guid = player->GetGUID();
    delete ai;
    PlayerbotsMgr::instance().RemovePlayerBotData(guid, true);

    LOG_INFO("module.mybots", "Selfbot disabled for {} guid={}", player->GetName(), guid.GetCounter());
    return Ok("disabled", "Selfbot disabled");
}

MyBotsResult MyBotsSelfbot::Status(Player* player)
{
    if (!player || !player->IsInWorld())
        return Fail(409, "offline", "Character is not in the world");

    bool const on = GET_PLAYERBOT_AI(player) != nullptr;
    return Ok(on ? "on" : "off", on ? "Selfbot is enabled" : "Selfbot is disabled");
}

std::string MyBotsSelfbot::SnapshotJson(Player* player)
{
    if (!player)
        return "{\"ok\":false,\"code\":\"not_found\",\"message\":\"Character not found\"}";

    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    std::string nameEsc;
    JsonEscape(player->GetName(), nameEsc);

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"online\":" << (player->IsInWorld() ? "true" : "false")
       << ",\"guid\":" << player->GetGUID().GetCounter()
       << ",\"accountId\":" << (player->GetSession() ? player->GetSession()->GetAccountId() : 0)
       << ",\"name\":\"" << nameEsc << "\""
       << ",\"selfbot\":" << (ai ? "true" : "false")
       << ",\"map\":" << player->GetMapId()
       << ",\"zone\":" << player->GetZoneId()
       << ",\"x\":" << player->GetPositionX()
       << ",\"y\":" << player->GetPositionY()
       << ",\"z\":" << player->GetPositionZ()
       << ",\"o\":" << player->GetOrientation()
       << ",\"level\":" << static_cast<uint32>(player->GetLevel())
       << ",\"class\":" << static_cast<uint32>(player->getClass())
       << ",\"hp\":[" << player->GetHealth() << "," << player->GetMaxHealth() << "]";

    if (player->GetSession())
        ss << ",\"latencyMs\":" << player->GetSession()->GetLatency();

    ss << "}";
    return ss.str();
}
