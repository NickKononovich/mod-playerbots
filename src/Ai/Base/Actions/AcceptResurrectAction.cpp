/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptResurrectAction.h"
#include "Event.h"
#include "Playerbots.h"

bool AcceptResurrectAction::Execute(Event event)
{
    if (bot->IsAlive())
        return false;

    WorldPacket p(event.getPacket());
    p.rpos(0);
    ObjectGuid guid;
    p >> guid;

    bool const requested = bot->isResurrectRequested();
    WorldPacket packet(CMSG_RESURRECT_RESPONSE, 8 + 1);
    packet << guid;
    packet << uint8(1);  // accept
    bot->GetSession()->HandleResurrectResponseOpcode(packet);

    // [lead590] Player::ResurectUsingRequestData teleports the body to the resurrecter (a near teleport) and raises
    // it only on the teleport's MSG_MOVE_TELEPORT_ACK (DELAYED_RESURRECT_PLAYER). A game client sends that ack at
    // once; the bot's comes from PlayerbotHolder::UpdateSessions on the world thread. b216 rfc-h 5D-4 225 s: Rtlocaa
    // accepted Ancestral Spirit, stayed dead at its corpse with its engine frozen (UpdateAI skips a teleporting bot)
    // and the leader waited 'Rtlocaa is dead' to the stall. The bot acks here like the client, in its own tick, and
    // the line names every gate, so a still-dead body after accept shows which one held.
    bool const nearBefore = bot->IsBeingTeleportedNear();
    if (nearBefore)
        botAI->HandleTeleportAck();

    Player* const master = botAI->GetMaster();
    LOG_INFO("playerbots",
             "[lead590] {} accepts the resurrection by {}: requested {} by it {}, prevent-res aura {}, near teleport "
             "{} -> {}, far {}, alive {}, mover self {}, master {} (self {}, teleporting {})",
             bot->GetName(), guid.ToString(), requested, bot->isResurrectRequestedBy(guid),
             bot->HasPreventResurectionAura(), nearBefore, bot->IsBeingTeleportedNear(),
             bot->IsBeingTeleportedFar(), bot->IsAlive(), bot->m_mover == bot,
             master ? master->GetName() : std::string("none"), master == bot,
             master ? master->IsBeingTeleported() : false);

    return true;
}
