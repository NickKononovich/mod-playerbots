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
class Unit;

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

// [raid3] A raid / the open world, where the leader's event tick does not run: the buff step issues its queued buff
// orders itself (every step-1 tick) and takes them back when the step ends (EndRaidBuffPass)
void TickRaidBuffPass(LeaderGroup& group, std::vector<Player*> const& members, LogFn const& logEvent);
void EndRaidBuffPass(LeaderGroup& group, std::vector<Player*> const& members);

// [lead780] the member's measured damage taken per second in the fight under way (DamageTaken since its start); 0:
// none measured (out of a fight, nothing taken yet)
float MemberTakenPerSec(LeaderGroup& group, Player* member);

// [lead903] user 2026-11-03 (36): a healer out of mana counts as critical for the cooldown rule: a living healer of
// `members` whose mana is under the line the fight's `fightSecLeft` need (FightManaLine, its own mana cooldowns and the
// potion credited) - it cannot heal this fight to its end. Empty: none; else the reason
// [lead1362] `boss`: the boss fight under way - the line is his (the boss rate, the boss line's heal cost)
std::string HealerOutOfMana(std::vector<Player*> const& members, float fightSecLeft, Unit* boss = nullptr);

// [lead931] an energize of `mana` on `receiver` cast by `caster` (Innervate): the mob where the threat the game
// forwards to the caster (half the mana, split over every mob listing the receiver) takes the caster past its share
// of the tank's threat (lead884's rule); empty: none
std::string EnergizeSplitOvertakes(Player* receiver, Player* caster, Player* tank, uint32 mana);

// [lead1181] user 2026-11-04 (Q63): before a member out of a fight sits to drink, the leader uses the group's mana
// sources that work out of a fight - a druid's Innervate (on another member or on itself), a priest's Hymn of Hope
// (the core gives it to the 3 members lowest on mana), a shaman's Mana Tide Totem (its tick's area), the member's own
// Evocation / Dispersion, its mana gem - each held by the leader's cooldown verdict (`verdict`: on trash only when back
// before the next boss pull). The way that brings `bot` the `needPct` (% of its max mana) in the fewest group seconds:
// its cast (a channel or a totem's life: the group stands in it; an aura over time: its duration's share) plus the
// drink at `drinkRate` %/s for what it leaves; sec < 0: no tool. A caster already told the same tool on another
// member is no tool now (one order per fact)
struct RestManaWay
{
    Player* caster = nullptr;
    uint32 spell = 0;      // the caster's spell (its External), 0 with `item`
    uint32 item = 0;       // a mana gem of `bot` itself (its Consume)
    Player* on = nullptr;  // the External's target (Innervate's receiver); nullptr: the caster's own / its area
    float sec = -1.0f;
    std::string name;
    std::string why;
};
// `recharge`: the seconds until the caster's spell is ready again once cast now (the leader's RechargeAfterCastSec)
using CooldownVerdictFn = std::function<bool(float rechargeSec, float effectSec, std::string& why)>;
using RechargeFn = std::function<float(Player* caster, uint32 spell)>;
RestManaWay BestRestManaTool(Player* bot, std::vector<Player*> const& members, float needPct, float drinkRate,
                             CooldownVerdictFn const& verdict, RechargeFn const& recharge);
}  // namespace Leader::Support

#endif
