#include "MyBotsConfig.h"

#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
bool IsClientMovementOpcode(uint16 opcode)
{
    // WotLK client movement / spline ack range commonly used by MSG_MOVE_* and CMSG_MOVE_*.
    // Drop these while Selfbot AI is attached to reduce rubber-banding.
    if (opcode >= 0x00B5 && opcode <= 0x00EE)
        return true;
    return false;
}
} // namespace

class MyBotsServerScript : public ServerScript
{
public:
    MyBotsServerScript() : ServerScript("MyBotsServerScript", {
        SERVERHOOK_CAN_PACKET_RECEIVE
    })
    {
    }

    bool CanPacketReceive(WorldSession* session, WorldPacket& packet) override
    {
        if (!sMyBotsConfig.Enable() || !sMyBotsConfig.SelfbotIgnoreClientMovement())
            return true;
        if (!session)
            return true;

        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld())
            return true;

        if (!GET_PLAYERBOT_AI(player))
            return true;

        if (IsClientMovementOpcode(static_cast<uint16>(packet.GetOpcode())))
            return false;

        return true;
    }
};

void AddMyBotsServerScripts()
{
    new MyBotsServerScript();
}
