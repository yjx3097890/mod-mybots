#include "MyBotsConfig.h"

#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
// Only drop client-*driven* locomotion. ACK packets for teleports / speed /
// knockback must still reach the server or the client rubber-bands and looks
// like it is sprinting then snapping back.
bool IsClientDrivenMovementOpcode(uint16 opcode)
{
    switch (opcode)
    {
        case MSG_MOVE_START_FORWARD:
        case MSG_MOVE_START_BACKWARD:
        case MSG_MOVE_STOP:
        case MSG_MOVE_START_STRAFE_LEFT:
        case MSG_MOVE_START_STRAFE_RIGHT:
        case MSG_MOVE_STOP_STRAFE:
        case MSG_MOVE_JUMP:
        case MSG_MOVE_START_TURN_LEFT:
        case MSG_MOVE_START_TURN_RIGHT:
        case MSG_MOVE_STOP_TURN:
        case MSG_MOVE_START_PITCH_UP:
        case MSG_MOVE_START_PITCH_DOWN:
        case MSG_MOVE_STOP_PITCH:
        case MSG_MOVE_SET_RUN_MODE:
        case MSG_MOVE_SET_WALK_MODE:
        case MSG_MOVE_FALL_LAND:
        case MSG_MOVE_START_SWIM:
        case MSG_MOVE_STOP_SWIM:
        case MSG_MOVE_SET_FACING:
        case MSG_MOVE_SET_PITCH:
        case MSG_MOVE_HEARTBEAT:
        case CMSG_MOVE_SET_RAW_POSITION:
        case CMSG_MOVE_FALL_RESET:
        case CMSG_MOVE_TIME_SKIPPED:
        case MSG_MOVE_START_ASCEND:
        case MSG_MOVE_STOP_ASCEND:
        case MSG_MOVE_START_DESCEND:
        case CMSG_MOVE_SET_FLY:
        case CMSG_MOVE_CHNG_TRANSPORT:
            return true;
        default:
            return false;
    }
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

        if (IsClientDrivenMovementOpcode(static_cast<uint16>(packet.GetOpcode())))
            return false;

        return true;
    }
};

void AddMyBotsServerScripts()
{
    new MyBotsServerScript();
}
