/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTCONTROL_H
#define PLAYERBOTS_SELFBOTCONTROL_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;
class WorldPacket;
class WorldSession;

// Who drives a player's own character: the SelfBot AI or the person at the game client.
// Implemented in PlayerbotsSelfBotAfk.cpp (next to the other SelfBot session hooks).
namespace SelfBotControl
{
    constexpr char const* ADDON_PREFIX = "BOTCTL";

    enum class Reason : uint8
    {
        None,
        Command,      // ".playerbots bot self [on|off]"
        ManualInput,  // the client moved, cast or changed target
        IdleReturn,   // SelfBotReturnAfterIdleSecs
    };

    // World thread (chat command handler, world update). Each returns the lines to show the player.
    // mode: "" toggles, "on", "off", "status" ("?").
    std::vector<std::string> HandleCommand(Player* player, std::string const& mode);
    std::vector<std::string> Activate(Player* player, Reason reason);
    std::vector<std::string> Deactivate(Player* player, Reason reason);
    std::string StatusLine(Player* player);

    // Any thread: a packet from a real game client is about to be handled.
    void OnClientPacket(WorldSession* session, WorldPacket const& packet);

    // getMSTime() of the last real input (movement start, jump, cast, target change) of that client, 0 = none.
    uint32 GetLastInputMs(ObjectGuid guid);

    // True from the input that takes control until the SelfBot AI is removed on the world thread:
    // the AI must not tick in between.
    bool IsSuspended(Player* player);

    // World thread, every world update: idle return of SelfBots taken over by input.
    void UpdateWorld(uint32 diff);

    // Machine-readable status for the BotControl addon: whispered to the player himself with LANG_ADDON,
    // prefix "BOTCTL" (client event CHAT_MSG_ADDON). Lines: "S;key=value;..." then "B;..." per bot, "E;n=<bots>".
    void SendAddonStatus(Player* player);
}

#endif
