/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERPLANNERSHARED_H
#define _PLAYERBOT_LEADERPLANNERSHARED_H

// [lead31a] The pull planner's pieces the tank's execution (Ai/Base/Actions/ChooseTargetActions.cpp) uses. The
// planner itself is the leader's (Ai/Leader/LeaderPlanner.cpp; its internals in namespace PullPlanning).

#include "G3D/Vector3.h"
#include "LeaderOrders.h"
#include "Position.h"

#include <string>
#include <vector>

class Creature;
class Player;
class Unit;

namespace PullPlanning
{
    // the values the execution shares (the planner's own definitions, LeaderPlanner.cpp)
    constexpr float CAREFUL_AGGRO_MARGIN = 3.0f;
    constexpr float APPROACH_DISTANCE = 30.0f;
    constexpr float SIDE_PULL_ARRIVED = 2.0f;

    // set by Execute: the decision it runs was issued by the leader (the commit reports it as the leader's)
    extern thread_local bool executingLeaderDecision;

    void NoteLosCorner(Player* tank, Position const& spot, Position const& gather);
    bool NoteLosPullStart(Player* bot, Unit* target);
    // [lead199] the tank's engage openers on an idle mob, best first: the ones that land damage (threat) before a
    // taunt (no threat on a mob that fights nobody); `gapClosers`: Charge / Feral Charge may land the tank at the mob
    std::vector<char const*> TankEngageOpeners(Player* tank, bool gapClosers);
    // [lead204] the planner's pack rule: `mob` comes with a pull of `target` (one formation, or spawn points within
    // the social radius, each one's wander radius on top)
    bool SpawnsWith(Creature* mob, Unit* target);
    // [lead205] where the patrol `mob` walks from now (its walker's forecast, PatrolTrack), one sample every
    // `sampleSec`; false: no forecast (no patrol, or it does not walk its path now)
    bool PatrolForecast(Creature* mob, std::vector<G3D::Vector3>& track, float& sampleSec);
    // [dun172] a druid tank out of (Dire) Bear Form: the bear's stats last seen in form (the planner plans with them),
    // false when `tank` is in form, no druid tank, or no bear of it was seen
    bool BearStatsSeen(Player* tank, float& health, float& armor, float& dodge, float& parry);
    // [dun180] the damage per second `mob` lands on `victim` as the planner reads it (LandedMeleeDps: its melee
    // through the game's attack table and armor, its spells from its spell data)
    float MobLandedDps(Unit* mob, Player* victim);
    // [lead1303] the damage per second `victim` takes now by the formula: MobLandedDps of every living attacker whose
    // victim it is (who attacks whom is state; no measured intake)
    float FormulaTakenDps(Player* victim);
    // [dun191] the planner's pack grouping, read by the five gate: the radius of an engage call CollectPack counts
    // (the core's assistance radius plus the fight's drift), and an entry's scripted call for help radius (0: none)
    float PackSocialRadius();
    float ScriptedCallRadius(uint32 entry);
    // [dun205] a template that stands and casts once it fights (the planner's KeepsRange by entry)
    bool KeepsRangeEntry(uint32 entry);
    // [dun244] the planner's spell model of a mob by its template (GetMobSpells' sources and formula, no live
    // creature): its spells' damage per second, the share of the fight it casts (no swing meanwhile) and its weapon
    // specials per second (`weaponHit`: its average weapon hit)
    void TemplateSpellDps(uint32 entry, float weaponHit, float& spellDps, float& castShare, float& weaponDps);
    // [dun219] a waypoint patrol whose beat (its waypoint path, waypoint_data) never takes it beyond `range` of `at`:
    // no window opens there; true also for a walker without a known path. False for one that stands or wanders
    bool PatrolBeatStays(Creature* patrol, Position const& at, float range);
    // [dun226] the farthest its beat's waypoints come from `at` (0: no path known)
    float PatrolBeatFarthest(Creature* patrol, Position const& at);

    // [split250] the widest reach of the area spells (hostile, affecting an area) the member knows, from their spell
    // data: what a crowd-controlled mob must stand beyond, from the fight, not to be broken (0: none)
    float MemberAreaReach(Player* member);
    // [split250] the yards a fight drifts from where it is planned (CollectPack's)
    float FightDriftYd();
    // [split254] the tank has a ranged pull (its pull strategy's spell, known, reaching CAREFUL_MIN_PULL_RANGE, as
    // PullBackSpot reads it): its fight can be pulled back to it
    bool TankPullsAtRange(Player* tank);
}

// the tank's commit of a pull, posted to the leader's order registry
bool PostCommit(Player* bot, Unit* target, bool ok, Leader::PullMethod method, std::string const& why, uint32 mobs,
                float cost, bool single);

#endif
