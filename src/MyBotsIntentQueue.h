#ifndef MYBOTS_INTENT_QUEUE_H
#define MYBOTS_INTENT_QUEUE_H

#include "MyBotsSelfbot.h"

#include "Define.h"
#include "ObjectGuid.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class MyBotsIntentOp
{
    Status,
    SelfbotOn,
    SelfbotOff,
    AssignJob,
    CancelJob,
    PauseJob,
    ResumeJob,
    ListJobs,
    GetJob,
    ListEvents,
    UpsertPatrol,
    ListPatrols
};

struct MyBotsIntent
{
    MyBotsIntentOp op = MyBotsIntentOp::Status;
    std::string playerName;
    uint32 guidLow = 0;
    std::string jobId;
    std::string jobType;
    std::string payload;
    bool replace = true;
    std::string response;
    int httpStatus = 500;
    bool done = false;
};

class MyBotsIntentQueue
{
public:
    static MyBotsIntentQueue& Instance();

    // Returns nullptr if queue is full.
    std::shared_ptr<MyBotsIntent> Submit(MyBotsIntent intent);
    bool Wait(std::shared_ptr<MyBotsIntent> const& intent, uint32 timeoutMs);
    void DrainOnWorldThread();

private:
    MyBotsIntentQueue() = default;

    void Execute(MyBotsIntent& intent);

    std::mutex _mutex;
    std::condition_variable _cv;
    std::vector<std::shared_ptr<MyBotsIntent>> _pending;
};

#define sMyBotsIntentQueue MyBotsIntentQueue::Instance()

#endif
