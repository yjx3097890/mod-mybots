#include "MyBotsSelfbot.h"
#include "MyBotsConfig.h"
#include "MyBotsDirector.h"
#include "MyBotsExecutor.h"
#include "MyBotsJob.h"
#include "MyBotsUtil.h"

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
    MyBotsJsonEscape(in, out);
}

void DisableAutonomousQuesting(PlayerbotAI* ai)
{
    if (!ai)
        return;

    // Alt/self bots do not auto-quest like random bots unless these strategies are on.
    // DisableRpgQuest=1: strip them so this module can direct later.
    // DisableRpgQuest=0: explicitly enable official autonomous play for P0 testing.
    if (sMyBotsConfig.SelfbotDisableRpgQuest())
    {
        ai->ChangeStrategy("-rpg quest,-travel,-rpg,-new rpg,-grind", BOT_STATE_NON_COMBAT);
        return;
    }

    ai->ChangeStrategy("+new rpg,+grind,-follow", BOT_STATE_NON_COMBAT);
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
        LOG_INFO("module.mybots", "Selfbot already active for {} disableRpgQuest={}",
            player->GetName(),
            sMyBotsConfig.SelfbotDisableRpgQuest() ? 1 : 0);
        return Ok("already_on", "Selfbot already enabled");
    }

    PlayerbotsMgr::instance().AddPlayerbotData(player, true);
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai)
        return Fail(500, "ai_create_failed", "PlayerbotsMgr did not attach PlayerbotAI");

    ai->SetMaster(player);
    DisableAutonomousQuesting(ai);

    LOG_INFO("module.mybots", "Selfbot enabled for {} guid={} disableRpgQuest={}",
        player->GetName(),
        player->GetGUID().GetCounter(),
        sMyBotsConfig.SelfbotDisableRpgQuest() ? 1 : 0);
    return Ok("enabled", "Selfbot enabled");
}

MyBotsResult MyBotsSelfbot::Disable(Player* player)
{
    if (!player)
        return Fail(404, "not_found", "Character not found");

    // Cancel any directed job and freeze motion before tearing down the AI —
    // otherwise MovePoint keeps skating after Selfbot is "off".
    uint32 const guid = player->GetGUID().GetCounter();
    if (auto job = sMyBotsJobStore.GetActiveForChar(guid))
    {
        job->status = MyBotsJobStatus::Cancelled;
        job->error = "selfbot_off";
        MyBotsExecutor::HaltControl(player, job.get());
        sMyBotsJobStore.Save(*job);
        sMyBotsJobStore.AppendEvent(guid, job->id, "job_cancelled", "selfbot_off");
    }
    else
        MyBotsExecutor::HaltControl(player, nullptr);

    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai)
        return Ok("already_off", "Selfbot already disabled");

    ObjectGuid playerGuid = player->GetGUID();
    delete ai;
    PlayerbotsMgr::instance().RemovePlayerBotData(playerGuid, true);

    LOG_INFO("module.mybots", "Selfbot disabled for {} guid={}", player->GetName(), playerGuid.GetCounter());
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
       << ",\"hp\":[" << player->GetHealth() << "," << player->GetMaxHealth() << "]"
       << ",\"power\":[" << player->GetPower(player->getPowerType()) << ","
       << player->GetMaxPower(player->getPowerType()) << "]";

    if (auto job = sMyBotsJobStore.GetActiveForChar(player->GetGUID().GetCounter()))
        ss << ",\"job\":" << MyBotsDirector::JobToJson(*job);
    else
        ss << ",\"job\":null";

    if (player->GetSession())
        ss << ",\"latencyMs\":" << player->GetSession()->GetLatency();

    ss << "}";
    return ss.str();
}
