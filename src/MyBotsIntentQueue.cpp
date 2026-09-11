#include "MyBotsIntentQueue.h"
#include "MyBotsSelfbot.h"

#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"

#include <chrono>
#include <sstream>

MyBotsIntentQueue& MyBotsIntentQueue::Instance()
{
    static MyBotsIntentQueue instance;
    return instance;
}

std::shared_ptr<MyBotsIntent> MyBotsIntentQueue::Submit(MyBotsIntentOp op, std::string playerName, uint32 guidLow)
{
    auto intent = std::make_shared<MyBotsIntent>();
    intent->op = op;
    intent->playerName = std::move(playerName);
    intent->guidLow = guidLow;

    std::lock_guard<std::mutex> lock(_mutex);
    _pending.push_back(intent);
    return intent;
}

bool MyBotsIntentQueue::Wait(std::shared_ptr<MyBotsIntent> const& intent, uint32 timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);
    return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return intent->done; });
}

void MyBotsIntentQueue::DrainOnWorldThread()
{
    std::vector<std::shared_ptr<MyBotsIntent>> batch;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        batch.swap(_pending);
    }

    for (auto const& intent : batch)
        Execute(*intent);

    if (!batch.empty())
        _cv.notify_all();
}

void MyBotsIntentQueue::Execute(MyBotsIntent& intent)
{
    Player* player = nullptr;
    if (intent.guidLow)
        player = ObjectAccessor::FindPlayer(ObjectGuid(HighGuid::Player, intent.guidLow));
    else if (!intent.playerName.empty())
        player = ObjectAccessor::FindPlayerByName(intent.playerName);

    MyBotsResult result;
    if (!player || !player->IsInWorld())
    {
        result = MyBotsResult{};
        result.ok = false;
        result.httpStatus = 409;
        result.code = "offline";
        result.message = "Character is not online";
        intent.httpStatus = result.httpStatus;
        intent.response = "{\"ok\":false,\"code\":\"offline\",\"message\":\"Character is not online\"}";
        intent.done = true;
        return;
    }

    switch (intent.op)
    {
        case MyBotsIntentOp::Status:
            intent.httpStatus = 200;
            intent.response = MyBotsSelfbot::SnapshotJson(player);
            intent.done = true;
            return;
        case MyBotsIntentOp::SelfbotOn:
            result = MyBotsSelfbot::Enable(player);
            break;
        case MyBotsIntentOp::SelfbotOff:
            result = MyBotsSelfbot::Disable(player);
            break;
    }

    intent.httpStatus = result.httpStatus;
    std::ostringstream ss;
    ss << "{\"ok\":" << (result.ok ? "true" : "false")
       << ",\"code\":\"" << result.code << "\""
       << ",\"message\":\"" << result.message << "\"";
    if (result.ok)
        ss << ",\"character\":" << MyBotsSelfbot::SnapshotJson(player);
    ss << "}";
    intent.response = ss.str();
    intent.done = true;
}
