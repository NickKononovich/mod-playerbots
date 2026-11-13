/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_USERPACK_H
#define PLAYERBOTS_USERPACK_H

#include <string>
#include <vector>

class Player;

// The user's own characters (Playerbots.UserPack.*) played by bots while the user is offline: logged in at world
// start without a master, grouped under one leader, playing with the open-world strategies ("new rpg", "grind").
// While the user plays one of them, the others are his bots (alt bots). Their XP is scaled, and they never enter a
// dungeon the test harness has not proven (streaks.json). Opt-in, off by default.
namespace UserPack
{
    // O(1). The character is one of the pack and the feature is on.
    bool IsPackCharacter(Player* player);

    // O(1). A pack bot that plays on its own (no game client master) and leads its group.
    bool IsAutonomousLeader(Player* bot);

    // World thread, at a bot's login after its saved strategies loaded. [rpgs1] a pack bot plays the pack's
    // open-world strategies ("new rpg", "grind") from its first tick.
    void OnBotLogin(Player* bot);

    // World thread. One line per pack character (".playerbots pack status").
    std::vector<std::string> StatusLines();

    // World thread. A pack order from the console (".playerbots pack cmd dungeon list"); the replies.
    std::vector<std::string> ConsoleCommand(std::string const& text);
}

void AddPlayerbotsUserPack();

#endif
