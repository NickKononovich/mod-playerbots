/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_PULLFACTS_H
#define PLAYERBOTS_LEADER_PULLFACTS_H

// [lead104] user: production logic does not live in the test harness. The leader's pull and fight facts: what the
// leader judges before it issues an engage and in a fight; the harness only calls them and logs.

#include "ChainPullJudge.h"  // [test1] Leader::Judge::Tri - the world gates as the judge computed them
#include "ObjectGuid.h"
#include "Position.h"
#include <functional>
#include <string>
#include <vector>

class Creature;
class Player;
class Unit;

namespace Leader
{
    class LeaderGroup;
}

namespace Leader::Fight
{
    // [lead101] [lead103] the idle mob the tank's walk in to `target` wakes (the leader's route search against every
    // idle pack's reach), the plan's own mobs and the target's formation not judged; null: the walk is clear
    Unit* WalkInWakes(Player* tank, Unit* target, std::vector<ObjectGuid> const& planMobs);
    // [lead101] an order's own mob that stays the leader's next pick: alive, in no fight and [lead108] one the tank
    // has a walk to (TankHasWalk)
    bool OrderMobStaysPick(Player* tank, Creature* own);
    // [lead108] the leader's route search finds the tank a walk to `mob`: a complete path, or a partial one that ends
    // nearer the mob than the tank stands (the reachability part of the leader's walk verdict)
    bool TankHasWalk(Player* tank, Unit* mob);
    // [lead104] a fight mob the leader has stopped from fleeing: at its SmartAI flee-for-assist health or running,
    // not held (stunned, rooted, crowd-controlled)
    bool NeedsFleeStop(Unit* mob);
    // [lead200] a fight mob at its SmartAI flee-for-assist health or running, held or not: the group kills it
    bool IsFleer(Unit* mob);
    // [lead104] the member (not a healer) with a stop ready that takes `mob` now; null: none
    // [lead1230] `afterHold`: the hold on it now is not judged (the chain's next stop, cast once that hold ends)
    Player* FleeStopper(std::vector<Player*> const& members, Unit* mob, bool afterHold = false);
    // [lead1230] the creature the core's flee would run `mob` to for help (Creature::DoFleeToGetAssistance's search)
    // and its run's seconds there (`reachSec`, -1 with none); null: the flee calls nobody
    Creature* FleeHelp(Unit* mob, float& reachSec);
    // [lead146] the mob runs to the tank: alive, in combat, its victim the tank, moving, not rooted and not out of
    // its reach - the fight is where the tank took it, the tank does not walk to meet it
    bool MobComesToTank(Player* tank, Unit* mob);
    // [lead760] user 2026-11-03 (top priority: in-fight walking): after the tank's opener the fight stands where the
    // leader planned it. Why a led tank's walk or gap closer to `target` would move the fight off that place (empty:
    // it does not): the plan's fight spot back is not where the mob is (the mob comes there), the mob comes to the
    // tank, a ready taunt reaches it from where the tank stands, a caster on the tank at range while the tank holds
    // other fight mobs in its melee (the ranged and the caster corner take it there)
    // [lead925] `gapCloser`: the move is a gap closer (Charge, Intercept, Feral Charge): a mob that comes to the tank
    // is charged (user 2026-11-03 q50: landing earlier starts its threat earlier), judged by TankGapCloserRisk instead
    std::string TankWalkMovesFight(Player* tank, Unit* target, bool gapCloser = false);
    // [lead925] user 2026-11-03 (q50, item 5): one check for every led tank's gap closer onto `target` - it lands the
    // tank at the target. Allowed (empty) when a living healer's heals reach that landing (HealReaches), or the tank
    // outlives the nearest healer's way there - its health over the expected damage per second it takes there
    // (AttackerDps of its attackers and the target) times the healer's walk into heal reach (Leader::WalkYd at its
    // run speed) plus its gap between two heals (HealerThroughput) -, or a defensive cooldown of its own is up
    // (Survival Instincts / Barkskin, Shield Wall / Last Stand). Else why not; `save`: its ready defensive the leader
    // may tell first (0: none - no gap closer)
    std::string TankGapCloserRisk(Player* tank, Unit* target, uint32* save = nullptr);
    // [lead925] a led tank's gap closer the risk held: the tank's request for its save (the leader's fight-save
    // External tells it while TankGapCloserRisk still names one). One request per tank: the latest target
    void AskGapCloserSave(Player* tank, Unit* target);
    bool GapCloserSaveAsked(Player* tank, ObjectGuid& target);
    void ClearGapCloserSave(Player* tank);
    // [lead925] `spell` is one of the defensives a gap closer's landing waits for
    bool IsGapCloserSave(uint32 spell);
    // [lead925] the tank's defensive `spell` is up (its own aura, Last Stand's 12976)
    bool GapCloserSaveUp(Player* tank, uint32 spell);
    // [lead925] a led tank's gap closer passes TankGapCloserRisk; when it does not, its save is asked for. `what`: the
    // spell's name, logged once per tank and target
    bool TankGapCloserAllowed(Player* tank, Unit* target, char const* what);
    // [lead760] the tank's taunt that is ready and reaches `mob` (or Righteous Defense its victim) from where the tank
    // stands, in sight (spell data, the game's combat range); 0: none
    uint32 TauntReachesFromHere(Player* tank, Unit* mob);

    // [lead1170] user 2026-11-04 (Q65): the chain pull. While a trash fight finishes, the leader opens the next pack
    // of the route's plan (GroupRouteState::routePlan, its order: never an off-route mob) so that it comes in as the
    // fight ends. Due when the fight's seconds left (its mobs' health over the group's measured dps) are fewer than the
    // pack's run in to the tank (its walk over its run speed) plus the tank's opening (its ranged opener's cast and
    // flight, spell data). Taken only when the tank opens every mob of the pack from where it stands (a ranged opener
    // ready, in range and sight: the fight stays where it is), the union of the fight's mobs and the pack passes the
    // overload gate (PullPlanner::JudgePack: tank health drop, healers' rate and mana), the tank's health now is above
    // the union's modelled drop, a healer reaches the tank, no healer runs out of mana over the union's fight
    // (Support::HealerOutOfMana), no member is dead, the tank holds the fight, and nothing more joins: no boss, no
    // walker stop or walker meeting there, no idle mob woken on its way in (WalkInWakes)
    // [lead1190] user 2026-11-04 (third round): the healer that keeps a landing of the tank (a meeting point, a
    // chain pull's walk end) in its heal: one that reaches it from where it stands, else one that walks into its
    // reach no later than the tank gets there (its run speed and walk against the tank's `inSec`) - the point on its
    // line to the landing at the leader's healer band (LEADER_HEALER_TANK_YD, the acquire margin), else its spot
    // kept at its offset to the tank - in heal reach and sight of the landing, its spot and walk past no unpulled
    // mob's reach. "" when one does (or the group has no healer), else why none
    struct HealerAlong
    {
        Player* healer = nullptr;  // null: no healer in the group
        Position spot;             // where it heals from
        bool walks = false;        // it walks there (false: it reaches the landing where it stands)
        float walkSec = 0.0f;
    };
    std::string HealerKeepsLanding(Player* tank, Position const& landing, float inSec,
                                   std::vector<Player*> const& members, HealerAlong& along);

    // [lead1190] user 2026-11-04 (third round, full freedom): a chain pull whose pack no ranged opener of the tank
    // reaches from where it stands walks the fight there - the tank goes (its mobs follow, they are on it) to the
    // nearest point on its line to the pack's anchor from which its longest ready ranged opener reaches every pack
    // mob in sight (`walkArriveYd`: the fight move's arrival tolerance off the range); due when the fight left is
    // shorter than that walk plus the opening and the pack's run in from there. Every check of the chain stays, and
    // the walk's: no unpulled mob's reach at its end or on the way (the pack's own included), a healer that keeps the
    // walk's end by its own walk in time (HealerKeepsLanding: it is ordered along). At the end the chain is judged
    // again from there (the opener reaches: taken as before)
    struct ChainPull
    {
        Creature* anchor = nullptr;    // the pack's mob nearest the tank (null: no idle on-route pack left)
        std::vector<ObjectGuid> mobs;  // the pack: the stop's idle mobs and their pack mates (`packOf`)
        bool due = false;              // the trigger holds (fight left < run in + opening)
        bool waiting = false;          // only the trigger holds it back: not yet due (no verdict to log)
        bool taken = false;
        std::string kind;              // the verdict's class: taken / boss / walker / opener / pack_reach / waits /
                                       // buff / dead / holds / overload / health / healer / mana / wakes
        std::string why;               // the verdict's reason (taken: the numbers' summary)
        std::string opener;
        size_t stop = 0;               // the stop's index in the route's plan
        float fightSecLeft = 0.0f;
        float runInSec = 0.0f;
        float openSec = 0.0f;
        float distance = 0.0f;
        float overload = 0.0f;
        float drop = 0.0f;
        float unionSec = 0.0f;
        float tankHealth = 0.0f;  // share of its health now
        uint32 unionMobs = 0;
        bool walk = false;        // [lead1190] taken as a walk of the fight to `walkTo` (kind "walk")
        Position walkTo;          // [lead1190]
        float walkSec = 0.0f;     // [lead1190] the tank's walk there
        HealerAlong healer;       // [lead1190] the healer that keeps the tank (its walk along)
        // [test1] THE WORLD GATES AS THIS JUDGEMENT COMPUTED THEM, one flag per gate that reads the world. Until
        // now only the resulting `kind` was recorded, and it names the gate that rejected while saying nothing
        // about the gates before it, so a replay of a recorded verdict could only answer Undecided.
        // Tri::Unknown means THE GATE WAS NEVER REACHED (an earlier one rejected first) - it does NOT mean "it
        // passed": the journal's rule, absence is not zero. A replay that cannot tell those apart would answer,
        // and lie. The names mirror ChainPullFacts' fields, which is what a replay reads.
        Leader::Judge::Tri bossInPack = Leader::Judge::Tri::Unknown;
        Leader::Judge::Tri walkerStop = Leader::Judge::Tri::Unknown;
        Leader::Judge::Tri groupBuffed = Leader::Judge::Tri::Unknown;
        Leader::Judge::Tri everyoneAlive = Leader::Judge::Tri::Unknown;
        Leader::Judge::Tri tankHolds = Leader::Judge::Tri::Unknown;
        Leader::Judge::Tri wayInQuiet = Leader::Judge::Tri::Unknown;
        // [jrn50] WHETHER A RANGED ENGAGE OPENER OF THE TANK WAS READY, as THIS judgement read it - the same fact
        // the opener gate's refusal is made of ("no ranged opener of the tank is ready"), which turned 121 pulls
        // down while pb_journal_actor.opener_ready stood empty beside them. It travels on the verdict instead of
        // being asked of the cooldowns again by whoever writes the row: a second reading at a second moment is a
        // second judge of one quantity.
        // Tri::No covers both "every one of them is cooling down" and "the tank knows none at all", because that
        // is one fact to the gate; Tri::Unknown is the usual "the judgement did not get this far".
        Leader::Judge::Tri openerReady = Leader::Judge::Tri::Unknown;
    };
    ChainPull JudgeChainPull(LeaderGroup& group, Player* tank, std::vector<Unit*> const& fightMobs, float fightSecLeft,
                             std::function<std::vector<ObjectGuid>(Creature*)> const& packOf, float walkArriveYd);

    // [lead1171] user 2026-11-04 (Q66, full freedom): the tank meets the mob. A mob coming at the led tank after its
    // ranged opener no longer waits the mob's whole run (lead146): the leader compares, from the walk times and the
    // spell data, the tank standing (the mob runs the gap), walking in (both run: the gap closes at both speeds), a
    // gap closer (Charge / Feral Charge: the core's charge speed, SPEED_CHARGE) and a pull to the tank (Death Grip:
    // the core's grip jump speed) by the group's time to its first hits at the landing (the tank's contact, each
    // melee member's walk to it, each ranged member's walk into its range of it) and takes the fastest. A landing off
    // the tank's spot needs: no other mob in the tank's melee (no drag), no unpulled mob's reach at the landing or on
    // the walk there (the leader's spot guard), a healer that reaches the landing from where it stands, and for a gap
    // closer the leader's landing check (TankGapCloserAllowed). Once per mob; the melee places follow the landing
    // [lead1190] the healer: one that keeps the landing where it stands or by its walk there before the tank's contact
    // (HealerKeepsLanding; it is ordered there)
    enum class TankMeet : uint8
    {
        None,
        Stand,   // the mob comes to the tank
        Walk,    // the tank walks in to meet it
        Closer,  // a gap closer onto it
        Grip     // pulled to the tank (Death Grip)
    };
    char const* TankMeetName(TankMeet meet);
    struct TankMeetPlan
    {
        TankMeet meet = TankMeet::None;
        std::string spell;  // Closer / Grip: the spell the tank casts
        Position landing;   // where the fight lands
        float gap = 0.0f;   // the gap to close (yd, the combat reaches off)
        // each option's seconds to the tank's contact and the group's summed seconds to its first hits (< 0: not
        // judged - the option was out)
        float standSec = -1.0f;
        float walkSec = -1.0f;
        float closerSec = -1.0f;
        float gripSec = -1.0f;
        float standGroup = -1.0f;
        float walkGroup = -1.0f;
        float closerGroup = -1.0f;
        float gripGroup = -1.0f;
        std::string why;    // why the moving options were out (empty: none was)
        HealerAlong healer;  // [lead1190] Walk / Closer: the healer that keeps the landing (its walk there)
    };
    TankMeetPlan PlanTankMeet(Player* tank, Unit* mob, std::vector<Player*> const& members);
    // the leader's verdict for `mob`, published for the tank's own actions (under a lock: the leader's thread writes,
    // the tank's map thread reads); TankMeet::None: none decided
    void SetTankMeet(Player* tank, Unit* mob, TankMeetPlan const& plan);
    TankMeet TankMeetFor(Player* tank, Unit* mob, std::string* spell = nullptr, Position* landing = nullptr);
}

#endif
