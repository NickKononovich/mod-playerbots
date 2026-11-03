/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERSUPPORT_H
#define PLAYERBOTS_LEADER_LEADERSUPPORT_H

// [lead400] .. [lead405] The leader's support decisions the ownership audit (C:/WoW/LEADER_OWNERSHIP_AUDIT.md, its
// LISTED items) left without a decider once the members' own were turned off: who removes which debuff (Dispel),
// who raises a dead member (a battle resurrection in the fight, the plain one out of it), the members' potions
// (the pre-pull potion at the boss pull, mana / health potions when the leader's estimate needs them), and the buff
// pass's soulstone, Ritual of Souls, ritual portal clicks and Soulwell. Each is one command per fact through the
// per-domain arbiter (Leader::Events::Set*); every decision is logged. Numbers come from the game: spell data
// (dispel types, ranges, costs, cooldowns, restore amounts), the members' measured damage taken and the leader's
// own fight estimates (FightManaLine, the healer's gap between heals).

#include "Common.h"
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "LeaderGroup.h"
#include "LeaderLog.h"

class Player;

namespace Leader::Support
{
using LogFn = std::function<void(Player*, char const*, JsonObject)>;

// Every leader tick (TickLeaderEvents, between BeginTick and EndTick): the dispels (in and out of a fight), the
// resurrections out of a fight, the pre-pull and in-fight potions, the buff pass's soulstone / ritual / Soulwell.
void Tick(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, Player* healer, bool fighting,
          LogFn const& logEvent);

// [lead401] The fight's battle resurrection: a dead group member whose body lies on the map and a member (not the
// tank, not in `told`) with a ready battle resurrection (Rebirth, Raise Ally) that reaches the body (range, sight,
// its power). `needed`: the fight cannot go on without the dead one (the only healer). False: none.
struct BattleRes
{
    Player* caster = nullptr;
    uint32 spell = 0;
    Player* dead = nullptr;
    bool needed = false;
    std::string why;
};
bool PickBattleRes(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, Player* healer,
                   std::set<ObjectGuid> const& told, BattleRes& out);

// [lead405] What the buff pass's support orders still do (empty: nothing); the pass's buff step waits for it.
std::string BuffPassWork(LeaderGroup& group);

// [lead780] the member's measured damage taken per second in the fight under way (DamageTaken since its start); 0:
// none measured (out of a fight, nothing taken yet)
float MemberTakenPerSec(LeaderGroup& group, Player* member);

// [lead903] user 2026-11-03 (36): a healer out of mana counts as critical for the cooldown rule: a living healer of
// `members` whose mana is under the line the fight's `fightSecLeft` need (FightManaLine, its own mana cooldowns and the
// potion credited) - it cannot heal this fight to its end. Empty: none; else the reason
std::string HealerOutOfMana(std::vector<Player*> const& members, float fightSecLeft);
}  // namespace Leader::Support

#endif
