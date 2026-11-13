/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LEADERPACKMODEL_H
#define _PLAYERBOT_LEADERPACKMODEL_H

// [pack1] One pull-pack model, for the leader's planner and the guild's five gate.
//
// What joins a pull is counted here once: the target's formation and engage call, what the spot and the way to it
// wake, the scripted calls for help, the assistance chain with its hops, the fight a mob that keeps its range draws,
// and the flee-for-assistance chain. The two callers differ only in the world they read it from:
//
//  - the planner (Ai/Leader/LeaderPlanner.cpp) answers from the live units around the tank;
//  - the five gate (Ai/Guild/GuildFives.cpp) answers from the map's spawn rows (the `creature` table and its
//    formations, paths and templates), before any five is in the instance.
//
// The algorithm below never touches a world of its own: every question is a call on the `World` policy, so a term
// added here lands in both. A term that genuinely needs a live world (a patrol's walk forecast, a navmesh walk round
// a corner) is answered by the gate's closest spawn-data reading, and the gate says so on its own why line.
//
// The policy a `World` has to provide (see LivePackWorld in LeaderPlanner.cpp and SpawnPackWorld in GuildFives.cpp):
//
//   using Mob = ...;                                 a handle to one mob (Unit* live, a spawn index in the gate)
//   static Mob NoMob();                              the empty handle
//   std::vector<Mob> const& Possible() const;        the mobs this count may draw on
//   bool IsIdle(Mob) const;                          an idle enemy of the group, still standing
//   bool IsHeld(Mob) const;                          crowd control holds it out of the fight
//   bool IsSealed(Mob) const;                        banished or immune: it joins no fight
//   bool IsBoss(Mob) const;
//   bool Chains(Mob) const;                          it is a creature: it calls, draws and flees
//   Position At(Mob) const;                          where it stands (live) or its spawn point (the gate)
//   float Dist(Mob, Position const&) const;
//   float Dist(Mob, Mob) const;
//   bool WithinDist(Mob, Mob, float dist) const;     WorldObject::IsWithinDistInMap (both object sizes count)
//   float Size(Mob) const;
//   bool Sees(Mob, Position const&, float eye) const;
//   bool SeesMob(Mob from, Mob to) const;
//   bool CallsOnEngage(Mob) const;                   no CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE
//   bool CanAssist(Mob helper, Mob caller) const;    Creature::CanAssistTo
//   void SocialLink(Mob, Mob target, float radius, float& gap, bool& sees) const;
//   void Formation(Mob, std::vector<Mob>& out) const;
//   bool OnPullPath(Position const& to, Mob) const;
//   bool ReachesSpot(Mob, Position const& spot, float drift) const;
//   PackWhyKind SpotKind(Mob) const;
//   bool KeepsRange(Mob) const;
//   bool IsRooted(Mob) const;
//   bool Arriving(Mob, Position const& kill) const;  a patrol not at the fight yet: it engages where it arrives
//   bool WalksBy(Mob, Position const& spot) const;   split mode: left to the plan's patrol forecast
//   bool Crossing(Mob bringer, Position const& anchor, Mob, float radius);   split mode: a crossing group (noted)
//   bool WouldCross(Position const& from, Mob, float radius) const;
//   void NoteCross(Position const& from, Mob, float radius);
//   float AssistRadius() const;                      CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS
//   float FleeRadius() const;                        CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS
//   float CallRadius(Mob) const;                     its scripted call for help (0: none)
//   bool CalledForHelp(Mob caller, Position const& from, float radius, Mob) const;
//   void ScriptedAdds(Mob, std::vector<Mob>& out) const;
//   bool WalkLegs(Mob, Position const& kill, std::vector<std::pair<G3D::Vector3, G3D::Vector3>>& legs) const;
//   bool ScriptedCall(Position const& spot, std::vector<Mob> const& pack, Mob& lead, std::vector<Mob>& called) const;
//   bool FleesForHelp(Mob) const;
//   float MaxHealth(Mob) const;
//   float RunSpeed(Mob) const;
//   float GroupDps();                                read once, at the first flight
//   float MeleeEdge() const;                         how far the melee stand from the fight spot (FightHold)
//   bool RecordsWhy() const;
//   void NoteWhy(Mob, Mob source, PackWhyKind, float distance, float radius, uint8 step, float reachSec,
//                float killSec);
//   void NoteSkipped(Mob helper, Mob fleer, PackWhyKind, float distance, float radius, uint8 step, float reachSec,
//                    float killSec);
//   void Done(std::vector<Mob> const& pack, std::vector<uint8> const& hops);   on every way out

#include "G3D/Vector3.h"
#include "LeaderPlannerShared.h"
#include "Position.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace PullPlanning
{
    constexpr float PACK_SOCIAL_RADIUS = 12.0f;  // idle mobs this close to the target join the fight
    constexpr float PACK_SOCIAL_DRIFT = 2.0f;    // CollectPack: the assistance radius plus the fight's drift
    constexpr float CAREFUL_PATROL_MARGIN = 10.0f;  // a patrol this far beyond its aggro range walks in
    constexpr float CAREFUL_PATROL_RANGE = 40.0f;   // the timeless test, for a patrol whose walk cannot be forecast
    constexpr float PULL_PATH_HEIGHT = 5.0f;        // a mob this far above/below the way in is on another floor
    // [b47_plan] A pull back is fought round its kill spot, not on it: the tank's fight hold keeps 15 yd round it, the
    // ranged step out of melee and the healer walks for sight. An idle static mob whose aggro reach comes within this
    // much more of the kill spot joins (the measured fight spread). brd-h 20261026-093620-brd-h-5B-6 pull 54: "one
    // ... Anvilrage Soldier, 6 mobs, cost 1.75" fought at (612.7,46.4), 25.6 yd from the idle Anvilrage Officer
    // 45845 (reach 24 = aggro 21 + 3): its pack (2 Medics, 2 Soldiers) joined, 11 engaged, the wipe at 2196-2210 s.
    constexpr float KILL_SPOT_DRIFT = 5.0f;
    constexpr float CAREFUL_EYE_HEIGHT = 2.0f;
    constexpr size_t FLEE_CHAIN_MAX_MOBS = 16;  // CollectPack stops following a flee-for-assistance chain here
    // User, 2026-10-24 («на склоне стояло 3 моба ... почему они считаются одним паком?»): a mob that flees for
    // assistance (SMART_ACTION_FLEE_FOR_ASSIST, mostly at 15 % health) brings its helper only if it runs there before
    // the group kills the health it has left: the run (distance / its run speed) under FLEE_REACH_MARGIN times the
    // kill (FLEE_HEALTH_SHARE of its maximum health at the group's estimated dps). A Ragefire Shaman (759-903 hp) has
    // 1.1-1.4 s left at 100 dps, so a helper counts within 14-16 yd (run 8 yd/s), a Defias Miner (386-417 hp) within
    // 7 yd; the 30 yd flee radius alone brought every pack within 30 yd of the fight (RFC: 12 mobs for 3 on a slope).
    constexpr float FLEE_HEALTH_SHARE = 0.15f;
    constexpr float FLEE_REACH_MARGIN = 1.5f;
    // [plan29_audit] 24 -> 16 (PULL_ESTIMATE_AUDIT 2026-10-25: estimates of 23-29 ran into the bound, 1-10 came)
    constexpr size_t PACK_CHAIN_MAX_MOBS = 16;  // and an assistance chain here (far over any cap: refused anyway)
    // A pack mob that stands and casts or shoots this far from the spot draws the fight to it: the tank and the
    // melee go to it, and what sees them there joins (wave 13 Shattered Halls, Magisters' Terrace, Razorfen Kraul).
    constexpr float CAREFUL_DRIFT_MIN_DISTANCE = 8.0f;
    // RFC 5B-1 2026-10-24 20:24:37 (wave 44): a Voidwalker Minion was counted as 22 mobs, the whole room and Jergosh
    // 79 yd away, through a chain "reaches #N, which keeps its range" -> "assist of #N" -> ...; 1 came. A mob that
    // keeps its range stands at most CASTER_KEEP_RANGE from the real fight spot (the kill spot) - one spawned
    // further walks in to that range - and draws the fight to where it stands then, not to its spawn (with the
    // core's Z gate, ReachesSpot). Every mob that really enters the fight calls its assistance in turn, as the core
    // does (Creature::CallAssistance on engage: Deadmines foundry, 12 counted, 20 came), so the chain stays
    // transitive; PACK_MAX_ASSIST_HOPS only bounds it (assist / draws-fight links from a mob that came by itself).
    constexpr float CASTER_KEEP_RANGE = 30.0f;
    constexpr uint8 PACK_MAX_ASSIST_HOPS = 6;

    // User, 2026-10-24: «как считается количество мобов в паке? ... почему они считаются одним паком?» Why each mob
    // is in a pack ("[pack why]" for the planner, "[pack1] gate why" for the five gate): diagnostics only, one
    // record per mob, parallel to the pack.
    enum class PackWhyKind : uint8
    {
        Start,        // the candidate
        Social,       // within PACK_SOCIAL_RADIUS of the candidate
        PullPath,     // on the tank's way to the spot
        SpotAggro,    // its aggro range reaches the spot
        SpotWander,   // its wander radius brings it to the spot
        SpotPatrol,   // a patrol that walks into the spot
        KillPath,     // on the tank's way back to the kill spot
        KillSpot,     // reaches the kill spot
        Formation,    // a formation member of the source
        Assist,       // the source's assistance call (in its radius, in its sight)
        CallForHelp,  // the source's scripted call for help
        DrawsFight,   // reaches the source, a mob that keeps its range
        ScriptedAdd,  // an add the source's script calls
        Scripted,     // a scripted call (Scarlet Monastery Cathedral)
        FleeHelper,   // the source flees to it at low health
        FleeTooFar,   // the source would flee to it, but dies before it gets there (not in the pack)
        PatrolAdded   // counted by the plan's patrol forecast after the pack
    };

    // [lead8] LEADER_REFACTOR_PLAN.md step 8 / 2.5: where the pack's fight is, a required argument of every count.
    // FightHere: the fight is at `spot` itself (an engage at the mob, a stand where the tank pulls and fights);
    // FightAt: a separate fight spot (a pull back's kill spot, a line-of-sight corner, a sight fight spot), from which
    // the calls for help, the social / formation links and the "draws the fight" chains are counted (KILL_SPOT_DRIFT
    // round it), the walk's own aggro along the way to `spot`.
    struct PackFight
    {
        Position at;
        bool separate = false;
    };

    inline PackFight FightHere(Position const& spot) { return PackFight{spot, false}; }
    inline PackFight FightAt(Position const& kill) { return PackFight{kill, true}; }

    // What a pull of `target`, fought at `fight`, brings of `world.Possible()`. `assistChain`: the measured careful
    // count, which follows every pack mob's own calls (`spread` below); otherwise only the scripted calls for help.
    // `losPull`: the pull round a corner - the mobs walk to the tank, so only a rooted mob keeps calling from its
    // spawn and the walk itself calls along the way.
    template <class World>
    void CollectPackOn(World& world, typename World::Mob target, Position const& spot,
                       std::vector<typename World::Mob>& pack, PackFight const& fight, bool assistChain = false,
                       bool losPull = false)
    {
        using Mob = typename World::Mob;
        Mob const none = World::NoMob();
        Position const kill = fight.separate ? fight.at : spot;
        bool const killApart = fight.separate && kill.GetExactDist(spot) > CAREFUL_AGGRO_MARGIN;
        // [b47_plan] the fight's spread round a pull back's kill spot (the spot itself when they coincide)
        float const killDrift = fight.separate ? KILL_SPOT_DRIFT : 0.0f;
        pack.clear();
        pack.push_back(target);
        // parallel to `pack`: how a mob joins - 0 by itself (the target, its neighbours, what reaches the spot, a
        // flee helper), n a mob n assist / draws-fight links away from those
        std::vector<uint8> hops = {0};
        // [chain_fight_position] parallel to `pack`: 1 for a mob an assistance call brought (AssistDelayEvent: it
        // engages with its own call switched off and calls next from where it fights, not from its spawn)
        std::vector<uint8> assisted = {0};
        // parallel to `pack`: the flee-for-assistance steps from the candidate's own pack (0: no flight)
        std::vector<uint8> steps = {0};
        bool const recording = world.RecordsWhy();
        // [chain_step_weight] on every way out: the hops of this pack, and the crossing groups that merged after all
        struct OnExit
        {
            World& world;
            std::vector<Mob> const& pack;
            std::vector<uint8> const& hops;

            ~OnExit() { world.Done(pack, hops); }
        } const onExit{world, pack, hops};
        auto const indexOf = [&pack](Mob mob) -> size_t
        {
            for (size_t i = 0; i < pack.size(); ++i)
            {
                if (pack[i] == mob)
                    return i;
            }

            return pack.size();
        };
        auto const inPack = [&](Mob mob) { return indexOf(mob) < pack.size(); };
        auto const hopOf = [&](Mob mob) -> uint8
        {
            size_t const at = indexOf(mob);
            return at < hops.size() ? hops[at] : uint8(0);
        };
        auto const stepOf = [&](Mob mob) -> uint8
        {
            size_t const at = indexOf(mob);
            return at < steps.size() ? steps[at] : uint8(0);
        };

        if (recording)
            world.NoteWhy(target, none, PackWhyKind::Start, 0.0f, 0.0f, 0, 0.0f, 0.0f);

        // Mobs that engage by themselves (the target, its neighbours, what sees the spot): they call for assistance
        // where they stand the moment they engage (Unit::Attack -> CallAssistance).
        std::vector<Mob> direct = {target};
        auto const isDirect = [&direct](Mob mob)
        { return std::find(direct.begin(), direct.end(), mob) != direct.end(); };
        // A mob brings its formation (a linked pack, the rest of a patrol group) with it, and each member its own.
        std::function<void(Mob)> addFormation;
        std::vector<Mob> scratch;
        // A mob held by crowd control (a Sap before the pull) stays where it is while the rest fights.
        // `kind`, `source`, `distance` (below 0: to `source`) and `radius`: why it comes (the why line only).
        auto const add = [&](Mob mob, PackWhyKind kind, Mob source, float distance = -1.0f, float radius = 0.0f,
                             float reachSec = 0.0f, float killSec = 0.0f)
        {
            if (world.IsHeld(mob) || inPack(mob))
                return;

            // [pack_skip_sealed_mob] a banished / immune idle mob joins no fight
            if (!(mob == target) && world.IsSealed(mob))
                return;

            // an assistant, or a mob a ranged mob's fight draws, is one link further than its source; a formation
            // member and a called or scripted add come as their source does
            bool const hasSource = !(source == none);
            uint8 const sourceHop = hasSource ? hopOf(source) : uint8(0);
            uint8 hop = 0;
            if (kind == PackWhyKind::Assist || kind == PackWhyKind::DrawsFight)
                hop = uint8(std::min<uint32>(uint32(sourceHop) + 1u, 255u));
            else if (kind == PackWhyKind::Formation || kind == PackWhyKind::CallForHelp ||
                     kind == PackWhyKind::ScriptedAdd)
                hop = sourceHop;

            uint8 step = hasSource ? stepOf(source) : uint8(0);
            if (kind == PackWhyKind::FleeHelper && step < 255)
                ++step;

            pack.push_back(mob);
            hops.push_back(hop);
            // [b35_lbrs2] round a corner a social neighbour came by the target's engage call (AssistDelayEvent:
            // SetNoCallAssistance), so it makes no engage call of its own at its spawn: only the periodic one, on its
            // walk and at the kill spot (brs-h 20261026-024524-brs-h-5D-8: a Firebrand Invoker's phantom spawn call
            // chained the Firebrand hall pair to pair)
            assisted.push_back(kind == PackWhyKind::Assist || (losPull && kind == PackWhyKind::Social) ? 1 : 0);
            steps.push_back(step);
            if (recording)
            {
                if (distance < 0.0f)
                    distance = hasSource ? world.Dist(mob, source) : 0.0f;

                world.NoteWhy(mob, source, kind, distance, radius, step, reachSec, killSec);
            }

            // [b51_strat] A formation member engages by itself where it stands: CreatureGroup::MemberEngagingTarget
            // -> EngageWithTarget -> AtEngage -> AttackStart -> Unit::Attack -> CallAssistance, with no
            // SetNoCallAssistance (only AssistDelayEvent sets that). Round a corner its spawn call was dropped (not in
            // `direct`), so a split of one linked formation counted 7 while its engage calls woke the next one:
            // strat-h 20261026-130307-5H-2 pull 2 (Mangled Cadaver of formation 52157, "7 come", 19 engaged: 52157 +
            // formation 52158 + Mangled Cadaver 52189), strat-n 122322-5B-3 (7 -> 17), strat-h 093620-5E-2 (7 -> 32).
            if (kind == PackWhyKind::Formation && !isDirect(mob))
                direct.push_back(mob);

            addFormation(mob);
        };

        addFormation = [&](Mob mob)
        {
            std::vector<Mob> members;
            world.Formation(mob, members);
            for (Mob member : members)
            {
                if (!(member == mob) && world.IsIdle(member))
                    add(member, PackWhyKind::Formation, mob);
            }
        };

        addFormation(target);
        // The target's engage call (Creature::CallAssistance, AnyAssistCreatureInRangeCheck): an idle mob that may
        // assist it (CanAssistTo: its faction, it answers calls, aggressive, sees the enemy) within the assistance
        // radius, plus the drift of the fight, that the target sees. A mob that only stands near is no neighbour:
        // it comes if it sees the spot (ReachesSpot) or another member calls it.
        float const socialAssistRadius = world.AssistRadius();
        float const socialRadius =
            socialAssistRadius > 0.0f ? std::min(PACK_SOCIAL_RADIUS, socialAssistRadius + PACK_SOCIAL_DRIFT) : 0.0f;
        Position const targetAt = world.At(target);
        {
            std::vector<Mob> const& possible = world.Possible();
            for (size_t p = 0; p < possible.size(); ++p)
            {
                Mob const mob = possible[p];
                if (mob == target || !world.IsIdle(mob))
                    continue;

                // [lead1331] user 2026-11-05 (Q69): with a wanderer on either side, the neighbours by their wander
                // areas (the gap between the areas, the sight between the spawn points), not by where they stand
                float gap = 0.0f;
                bool sees = false;
                world.SocialLink(mob, target, socialRadius, gap, sees);
                bool const social = socialRadius > 0.0f && world.CallsOnEngage(target) &&  // [b33_brs]
                                    gap <= socialRadius && world.CanAssist(mob, target) && sees &&
                                    !world.Crossing(target, targetAt, mob, socialRadius);
                // the same tests in the same order as one condition, the first that holds named for the why line
                bool joins = true;
                PackWhyKind kind = PackWhyKind::Social;
                Position from = targetAt;
                if (social)
                    kind = PackWhyKind::Social;
                else if (world.OnPullPath(spot, mob))
                {
                    kind = PackWhyKind::PullPath;
                    from = spot;
                }
                else if (world.ReachesSpot(mob, spot, killApart ? 0.0f : killDrift) && !world.WalksBy(mob, spot))
                {
                    kind = recording ? world.SpotKind(mob) : PackWhyKind::SpotAggro;
                    from = spot;
                }
                else if (killApart && world.OnPullPath(kill, mob))
                {
                    kind = PackWhyKind::KillPath;
                    from = kill;
                }
                else if (killApart && world.ReachesSpot(mob, kill, killDrift) && !world.WalksBy(mob, spot))
                {
                    kind = PackWhyKind::KillSpot;
                    from = kill;
                }
                else
                    joins = false;

                if (joins)
                {
                    add(mob, kind, kind == PackWhyKind::Social ? target : none,
                        recording ? world.Dist(mob, from) : 0.0f);
                    direct.push_back(mob);
                }
            }
        }

        float const assistRadius = world.AssistRadius();
        // Where a mob that keeps its range fights from: where it stands, if that is within CASTER_KEEP_RANGE of the
        // real fight spot (the kill spot), else at that range from it on the line to where it stands (it walks in).
        auto const standPoint = [&](Mob caller) -> Position
        {
            Position const at = world.At(caller);
            float const toKill = at.GetExactDist(kill);
            if (toKill <= CASTER_KEEP_RANGE || toKill <= 0.0f)
                return at;

            float const share = CASTER_KEEP_RANGE / toKill;
            return Position(kill.GetPositionX() + (at.GetPositionX() - kill.GetPositionX()) * share,
                            kill.GetPositionY() + (at.GetPositionY() - kill.GetPositionY()) * share,
                            kill.GetPositionZ() + (at.GetPositionZ() - kill.GetPositionZ()) * share);
        };
        // What each pack mob from `from` on brings (and what that brings in turn), in measured careful pulls.
        auto const spread = [&](size_t from)
        {
            if (!assistChain)
                return;

            for (size_t i = from; i < pack.size() && pack.size() < PACK_CHAIN_MAX_MOBS; ++i)
            {
                Mob const caller = pack[i];
                // the safety bound of the chain (PACK_MAX_ASSIST_HOPS links from a mob that came by itself)
                if (!world.Chains(caller) || hops[i] >= PACK_MAX_ASSIST_HOPS)
                    continue;

                // Round a corner a caster comes too: only a rooted mob stays and keeps calling from its spawn,
                // and a mob that walks to the tank calls only once it engages by itself.
                bool const rooted = world.IsRooted(caller);
                bool const stays = rooted || (!losPull && world.KeepsRange(caller));
                // Stratholme wave 6 (Rlpalbh, 15 mobs for a 7-mob formation): a Plague Ghoul patrol forecast to walk
                // into the fight was counted with the assistance of the pack it passed 60 yd away. A patrol that is
                // not at the fight yet engages (and calls) where it arrives, which the spot tests count; not here.
                bool const arriving = world.Arriving(caller, kill);
                bool const callsHere = !arriving && (!losPull || stays || isDirect(caller));
                // where it fights: a rooted mob where it stands, a ranged one within its range of the fight spot
                Position const stand = rooted ? world.At(caller) : standPoint(caller);
                // apart from where the fight is (the kill spot; default: the spot), the tank and the melee go to it
                // Only a mob that came by itself draws the fight (hop 0): the group fights at the kill spot and goes
                // to the casters there, not on from caster to caster across a hall. Build 20 counted chains of them
                // (Scholomance upper hall 20, Sethekk 13, a Stratholme Cadaver formation 26) where the passing waves
                // pulled 2-16 there without a death; the DB groups are 2-7.
                bool const drawsFight =
                    !arriving && stays && hops[i] == 0 && stand.GetExactDist(kill) > CAREFUL_DRIFT_MIN_DISTANCE;
                // [chain_melee_edge] the melee that go to it stand no further from the fight spot than the fight
                // hold lets them (FightHold: RADIUS + MELEE_SLACK, PATCH_pets_rally 7), so what sees them there is
                // looked for at that edge, on the line to the caster.
                Position drawAt = stand;
                if (float const toStand = stand.GetExactDist(kill); toStand > world.MeleeEdge() && toStand > 0.0f)
                {
                    float const share = world.MeleeEdge() / toStand;
                    drawAt.Relocate(kill.GetPositionX() + (stand.GetPositionX() - kill.GetPositionX()) * share,
                                    kill.GetPositionY() + (stand.GetPositionY() - kill.GetPositionY()) * share,
                                    kill.GetPositionZ() + (stand.GetPositionZ() - kill.GetPositionZ()) * share);
                }
                // [chain_fight_position] User 2026-10-25 (PATCH_pets_rally 5): the engage call (Unit::Attack ->
                // Creature::CallAssistance) comes from where the mob stands when it engages, but only from a mob
                // that engaged by itself; an assistant engages with its call off (AssistDelayEvent:
                // SetNoCallAssistance) and calls next by the periodic call (Creature::Update, every
                // CreatureFamilyAssistancePeriod, CanPeriodicallyCallForAssistance) from where it FIGHTS then: a
                // melee mob by the tank (the kill spot), a mob that keeps its range where it stands and casts. The
                // chain counted every assistant's call from its spawn, pack to pack (scholo-h 153805: "16 mobs,
                // hopeless" -> 1 came, "lone Adept brings 10").
                bool const engageCall = callsHere && !assisted[i] && world.CallsOnEngage(caller);  // [b33_brs]
                Position const fightAt = stays ? stand : kill;
                Position const callerAt = world.At(caller);
                float const callerSize = world.Size(caller);
                float const callRadius = world.CallRadius(caller);
                // It calls at low health where it fights then: the kill spot, or where it stands and casts.
                Position const callFrom = stays ? stand : kill;
                std::vector<Mob> const& possible = world.Possible();
                for (size_t p = 0; p < possible.size(); ++p)
                {
                    Mob const mob = possible[p];
                    if (mob == caller || !world.Chains(mob) || !world.IsIdle(mob) || inPack(mob))
                        continue;

                    // its engage call reaches from where it stands when it engages (CallAssistance), its periodic
                    // call from where it fights ([chain_fight_position])
                    bool const engageAssists = engageCall && assistRadius > 0.0f &&
                                               world.WithinDist(caller, mob, assistRadius) &&
                                               world.CanAssist(mob, caller) && world.SeesMob(caller, mob) &&
                                               !world.Crossing(caller, callerAt, mob, assistRadius);
                    bool const fightAssists =
                        !engageAssists && !arriving && assistRadius > 0.0f &&
                        world.Dist(mob, fightAt) <= assistRadius + callerSize + world.Size(mob) &&
                        world.CanAssist(mob, caller) && world.Sees(mob, fightAt, CAREFUL_EYE_HEIGHT) &&
                        !world.Crossing(none, fightAt, mob, assistRadius);
                    bool const assists = engageAssists || fightAssists;
                    // Creature::CallForHelp: any friendly idle mob in the radius that sees the fight
                    bool const called = world.CalledForHelp(caller, callFrom, callRadius, mob) &&
                                        !world.Crossing(none, callFrom, mob, callRadius);
                    // the core's aggro of the mob on the tank and melee where the ranged mob fights (ReachesSpot)
                    // a patrol of another spawn group that walks by is left to the plan's patrol forecast, as at the
                    // spot (WalksBy)
                    if (assists || called ||
                        (drawsFight && world.ReachesSpot(mob, drawAt, 0.0f) && !world.WalksBy(mob, spot)))
                    {
                        if (assists)
                            add(mob, PackWhyKind::Assist, caller, -1.0f, assistRadius);
                        else if (called)
                            add(mob, PackWhyKind::CallForHelp, caller,
                                recording ? world.Dist(mob, callFrom) : 0.0f, callRadius);
                        else
                            add(mob, PackWhyKind::DrawsFight, caller);

                        addFormation(mob);
                    }
                }

                // [b34_est] The periodic call (Creature::Update, every CreatureFamilyAssistancePeriod) comes from
                // where the mob is then, so also on its walk to the fight spot round the corner. ohb-h
                // 20261026-014521-ohb-h-5F-19 pull 25 (wipe): a los pull counted at 3 brought 13, three Durnholde
                // formations (4 + 5 + 4); the tank's spot stood 11.6 yd up a ramp, the walk to it ran 5.7 yd past
                // the next formation's Sentry (it engaged at once), and that one's walk past the third. A mob that
                // walks round the corner calls along its navmesh walk there, at the assistance radius (an assist).
                std::vector<std::pair<G3D::Vector3, G3D::Vector3>> legs;
                if (losPull && !stays && !arriving && assistRadius > 0.0f &&
                    callerAt.GetExactDist(kill) > assistRadius && world.WalkLegs(caller, kill, legs))
                {
                    std::vector<Mob> const& walkers = world.Possible();
                    for (size_t p = 0; p < walkers.size(); ++p)
                    {
                        if (legs.empty())
                            break;  // it reaches the kill spot before its first call: the kill spot's call only

                        Mob const mob = walkers[p];
                        if (mob == caller || !world.Chains(mob) || !world.IsIdle(mob) || inPack(mob) ||
                            !world.CanAssist(mob, caller))
                            continue;

                        float const reach = assistRadius + callerSize + world.Size(mob);
                        Position const at = world.At(mob);
                        G3D::Vector3 const mobAt(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ());
                        for (auto const& [legFrom, legTo] : legs)
                        {
                            // the leg's point nearest the mob
                            G3D::Vector3 const leg = legTo - legFrom;
                            float const squared = leg.dot(leg);
                            float const share =
                                squared > 0.0f ? std::clamp((mobAt - legFrom).dot(leg) / squared, 0.0f, 1.0f) : 0.0f;
                            G3D::Vector3 const point = legFrom + leg * share;
                            Position const closest(point.x, point.y, point.z);
                            if (world.Dist(mob, closest) <= reach && world.Sees(mob, closest, CAREFUL_EYE_HEIGHT) &&
                                !world.Crossing(none, closest, mob, assistRadius))
                            {
                                add(mob, PackWhyKind::Assist, caller, -1.0f, assistRadius);
                                break;
                            }
                        }
                    }
                }

                scratch.clear();
                world.ScriptedAdds(caller, scratch);
                for (Mob called : scratch)
                    add(called, PackWhyKind::ScriptedAdd, caller);
            }
        };

        spread(0);

        // Call for help in every careful mode (measured: in the chain above). A pack mob whose script calls at low
        // health brings every mob CalledForHelp finds round the spot where it fights then (the kill spot; a rooted
        // mob: where it stands), not round its spawn, each with its formation and the neighbours its engage calls
        // (CallAssistance); a called mob that calls in turn does so from there too.
        auto const callForHelp = [&](size_t from)
        {
            if (assistChain)
                return;

            for (size_t i = from; i < pack.size() && pack.size() < PACK_CHAIN_MAX_MOBS; ++i)
            {
                Mob const caller = pack[i];
                float const callRadius = world.Chains(caller) ? world.CallRadius(caller) : 0.0f;
                if (callRadius <= 0.0f)
                    continue;

                Position const callFrom = world.IsRooted(caller) ? world.At(caller) : kill;
                std::vector<Mob> const& possible = world.Possible();
                for (size_t p = 0; p < possible.size(); ++p)
                {
                    Mob const mob = possible[p];
                    if (mob == caller || !world.Chains(mob) || !world.IsIdle(mob) || inPack(mob) ||
                        !world.CalledForHelp(caller, callFrom, callRadius, mob) ||
                        world.Crossing(none, callFrom, mob, callRadius))
                        continue;

                    add(mob, PackWhyKind::CallForHelp, caller, recording ? world.Dist(mob, callFrom) : 0.0f,
                        callRadius);
                    if (assistRadius <= 0.0f || !world.CallsOnEngage(mob))  // [b33_brs]
                        continue;

                    Position const mobAt = world.At(mob);
                    std::vector<Mob> const& neighbours = world.Possible();
                    for (size_t q = 0; q < neighbours.size(); ++q)
                    {
                        Mob const neighbour = neighbours[q];
                        if (!(neighbour == mob) && world.Chains(neighbour) && world.IsIdle(neighbour) &&
                            world.WithinDist(mob, neighbour, assistRadius) && world.CanAssist(neighbour, mob) &&
                            world.SeesMob(mob, neighbour) && !world.Crossing(mob, mobAt, neighbour, assistRadius))
                            add(neighbour, PackWhyKind::Assist, mob, -1.0f, assistRadius);
                    }
                }
            }
        };

        callForHelp(0);

        // A scripted call (Scarlet Monastery Cathedral, ClassicShared.h): Commander Mograine's engage sends his
        // whole chapel at his victim, wherever its mobs stand and whatever sees what. Wave 34: a fight 34 yd from him
        // drew a hunter to 12 yd of him, and the rest of the chapel came at once.
        {
            Mob lead = none;
            scratch.clear();
            if (world.ScriptedCall(spot, pack, lead, scratch))
            {
                add(lead, PackWhyKind::Scripted, none, recording ? world.Dist(lead, spot) : 0.0f);
                for (Mob mob : scratch)
                {
                    if (world.IsIdle(mob))
                        add(mob, PackWhyKind::Scripted, lead);
                }
            }
        }

        // Flee for assistance (Creature::DoFleeToGetAssistance), in every careful mode: a pack mob that flees at low
        // health runs to the nearest idle mob within the flee radius that sees it, which comes with its formation and
        // calls its neighbours (CallAssistance); a helper that flees fetches the next. Counted from the spot, where
        // the fight stays, or (measured) from a mob that keeps its range, where the tank goes to fight it. Wave 11b
        // Scarlet Monastery Library: a 2-mob plan, the next pack 27 yd away, 13 came.
        float const fleeRadius = world.FleeRadius();
        if (fleeRadius <= 0.0f)
            return;

        float fleeDps = -1.0f;  // the group's estimated dps, read at the first flight
        for (size_t i = 0; i < pack.size() && pack.size() < FLEE_CHAIN_MAX_MOBS; ++i)
        {
            Mob const fleer = pack[i];
            if (!world.Chains(fleer) || world.IsBoss(fleer) || !world.FleesForHelp(fleer))
                continue;

            Position const from = assistChain && !losPull && world.KeepsRange(fleer) ? standPoint(fleer) : kill;
            Mob helper = none;
            float nearest = fleeRadius;
            // Split mode: a crossing group nearer than the helper is where the fleer would run now (noted); once it
            // has walked on, the fleer runs to the helper.
            Mob passing = none;
            float passingAt = fleeRadius;
            std::vector<Mob> const& possible = world.Possible();
            for (size_t p = 0; p < possible.size(); ++p)
            {
                Mob const mob = possible[p];
                if (!world.Chains(mob) || !world.IsIdle(mob) || world.IsHeld(mob) || inPack(mob))
                    continue;

                float const distance = world.Dist(mob, from);
                if (distance > nearest || !world.CanAssist(mob, fleer) ||
                    !world.Sees(mob, from, CAREFUL_EYE_HEIGHT))
                    continue;

                if (world.WouldCross(from, mob, fleeRadius))
                {
                    if (passing == none || distance < passingAt)
                    {
                        passing = mob;
                        passingAt = distance;
                    }

                    continue;
                }

                helper = mob;
                nearest = distance;
            }

            if (!(passing == none) && (helper == none || passingAt < nearest))
                world.NoteCross(from, passing, fleeRadius);

            if (helper == none)
                continue;

            // It dies on the way: the helper never hears of the fight.
            if (fleeDps < 0.0f)
                fleeDps = world.GroupDps();

            float const reachSec = nearest / std::max(world.RunSpeed(fleer), 0.1f);
            float const killSec = world.MaxHealth(fleer) * FLEE_HEALTH_SHARE / fleeDps;
            if (reachSec >= killSec * FLEE_REACH_MARGIN)
            {
                if (recording)
                    world.NoteSkipped(helper, fleer, PackWhyKind::FleeTooFar, nearest, fleeRadius, steps[i], reachSec,
                                      killSec);

                continue;
            }

            size_t const before = pack.size();
            add(helper, PackWhyKind::FleeHelper, fleer, nearest, fleeRadius, reachSec, killSec);
            addFormation(helper);
            direct.push_back(helper);  // the fleeing mob's call brings it, and it calls its neighbours where it stands
            if (assistChain)
            {
                spread(before);
                continue;
            }

            if (assistRadius <= 0.0f || !world.CallsOnEngage(helper))  // [b33_brs]
                continue;

            Position const helperAt = world.At(helper);
            std::vector<Mob> const& neighbours = world.Possible();
            for (size_t q = 0; q < neighbours.size(); ++q)
            {
                Mob const mob = neighbours[q];
                if (!(mob == helper) && world.Chains(mob) && world.IsIdle(mob) &&
                    world.WithinDist(helper, mob, assistRadius) && world.CanAssist(mob, helper) &&
                    world.SeesMob(helper, mob) && !world.Crossing(helper, helperAt, mob, assistRadius))
                    add(mob, PackWhyKind::Assist, helper, -1.0f, assistRadius);
            }

            callForHelp(before);
        }
    }
}

#endif
