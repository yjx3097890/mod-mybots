#ifndef MYBOTS_SELFBOT_H
#define MYBOTS_SELFBOT_H

#include <string>

class Player;

struct MyBotsResult
{
    bool ok = false;
    int httpStatus = 500;
    std::string code;
    std::string message;
};

class MyBotsSelfbot
{
public:
    static MyBotsResult Enable(Player* player);
    static MyBotsResult Disable(Player* player);
    static MyBotsResult Status(Player* player);
    static std::string SnapshotJson(Player* player);
};

#endif
