/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STRATSHARED_H
#define PLAYERBOTS_STRATSHARED_H

#include <vector>

#include "Define.h"

class Creature;
class Player;
class PlayerbotAI;
class Unit;

// Stratholme (map 329), the Scarlet side streets (RESEARCH_TACTICS.md section 11). Every Ravaged Cadaver and almost
// every Skeletal Berserker wanders 5 yd at random, eight Plague Ghoul groups patrol: the edge of a pack's aggro moves
// all the time. The group waits at that edge while the tank plans its pull (the test harness stops the leader
// there, ApproachStop), and a member a few yards ahead of the tank is the one a wanderer finds first (waves 16-34:
// the Enhancement shaman, 2-3 yd in front of the tank, took a whole street pack for 4-7 s and died before anyone
// had a target).
namespace Stratholme
{
constexpr uint32 MAP_STRATHOLME = 329;

// The yards an idle mob is given on top of its aggro range: its random wander (capped, Creature::GetWanderDistance)
// and a margin for the step it takes while the bot's position is sampled.
constexpr float IDLE_WANDER_MAX = 5.0f;
constexpr float IDLE_AGGRO_MARGIN = 2.0f;
// The tank stays the first one a mob meets: a spot is safe inside an idle mob's reach only when it is this much
// farther from the mob than the tank.
constexpr float TANK_LEAD = 1.5f;
// A member this far from the tank catches up with it whatever the mobs around (a leader that walks on).
constexpr float HOLD_MAX_TANK_DISTANCE = 25.0f;
// The tank defends a member out of its own fight within this distance.
constexpr float DEFEND_RANGE = 40.0f;

struct IdleMob
{
    Creature* mob;
    float reach;
};

bool InStratholme(Player* bot);

// The group's tank (the main tank first), alive and on the bot's map; nullptr when the bot is it or has none.
Player* GroupTank(Player* bot);

// The hostile creatures around the bot ("possible targets no los", a cached value) that are not fighting and could
// aggro on a player who walks into their reach.
std::vector<IdleMob> CollectIdleMobs(PlayerbotAI* botAI);

// True when a player standing at (x, y, z) is inside the reach of one of `mobs` and in its sight, while the tank is
// not nearer to that mob by TANK_LEAD: the mob would aggro on that player first.
bool SpotDrawsAggro(Player* bot, Player* tank, std::vector<IdleMob> const& mobs, float x, float y, float z);

// A mob hitting a member of the tank's group (not a tank) within DEFEND_RANGE of it; `victim` is that member.
Unit* FindSwarmer(Player* tank, Player*& victim);
}  // namespace Stratholme

#endif
