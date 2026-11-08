/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOT_NEWRPGFISHING_H
#define PLAYERBOT_NEWRPGFISHING_H

#include <string>

#include "TravelMgr.h"

class Player;
class PlayerbotAI;

// [gather] A fishing task of the global leader (GATHER of a fish place, or PROFESSION GATHER of Fishing): the bot walks
// to fishable water of the place (GuildGatherPlan FISH: the area's fishing_loot_template, the zone's fishing skill),
// stands on the shore (MoveNearWaterAction), equips its own Fishing Pole, casts Fishing 7620 (FishingAction) and
// clicks its bobber when it bites (UseBobberAction); the loot pipeline stores the catch. Everything is the core's own
// fishing; nothing is handed out. Map thread, one call per New RPG IDLE tick while the task lasts.
namespace NewRpgFishing
{
    enum class Step : uint8
    {
        NONE,  // nothing to do this tick (in combat, dead): New RPG goes on
        WALK,  // GO_CAMP to `walkTo`, then call again
        BUSY,  // acted or waits for the bite at the water: the tick is taken
        DONE   // cannot fish here (`why`): the task is given back to the leader
    };

    // `water`: the place's water marker (a pool or a swimming mob's spawn, GuildGatherPlace::spot)
    Step Tick(PlayerbotAI* botAI, Player* bot, WorldPosition const& water, WorldPosition& walkTo, std::string& why);
    // the task ended or was taken away: the bobber strategy off, the weapons it had before the pole back in hand
    void Stop(PlayerbotAI* botAI, Player* bot);
    // map thread: the bot is on a fishing task (for the [gather] lines and the coordinator's stuck check)
    bool IsFishing(Player* bot);
    // the bot owns a fishing pole (equipped or in its bags)
    bool HasPole(Player* bot);
    // [fish14] out of a fishing task with the pole in hand: its best main-hand weapon from the bags back (true: done)
    bool PutPoleAway(Player* bot);
}

#endif
