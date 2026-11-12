/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SOLOSAFETY_H
#define PLAYERBOTS_SOLOSAFETY_H

// [guild13] user 2026-11-07: the levelling guild bots stop dying (b294 night: 150 deaths in 2.5 h). A solo questing
// bot plays as a careful player:
// - it pulls only what it wins: the pack its pull wakes (the careful planner's CollectPack: the engage call, the
//   assistance chain, the mobs whose aggro reach covers the fight), their damage on it (the server's numbers,
//   Leader::Fight::AttackerDps, or what that mob was measured to deal, the higher) over the fight its measured damage
//   gives, against its health, its pet's and its heals' (spell data, its mana). The user's rule on con: one mob above
//   its level at a time, and only at full health and mana; two or more above its level at once never.
// - it rests to full before a pull the judge would win only at full;
// - its walks go round the idle mobs (a route search penalty: the reach of each costs the fight it starts; a mob
//   it must never fight is refused while a complete walk round exists);
// - after a death it rests to full and goes to its quest hub before it plans again.
// Applies to player bots under the New RPG that play alone (no living group member within the group reward
// distance), outside instances.

#include "Define.h"
#include "G3D/Vector3.h"
#include "G3D/Vector4.h"
#include "ObjectGuid.h"  // [build153] Verdict / EngageNote::counted ([death392])
#include "Position.h"

#include <string>
#include <vector>

class Creature;
class GameObject;  // [death407]
struct CreatureTemplate;
class Player;
class Unit;
class WorldObject;  // [objective23]

namespace Leader::Travel
{
    struct RoutePenalty;
}

namespace SoloSafety
{
    bool Applies(Player* bot);
    // [deaths146] a bot in the open world with no group member near that fights with it (SoloPlayerBot), whatever
    // its strategies: a dungeon five's member walking alone to its instance is one
    bool WalksAlone(Player* bot);
    // [deaths23] the group member fights with the bot: both in the co-op / help party, or neither and not both bots
    // on their own New RPG plans (a human, a follower and its leader); two own-plan bots walk their own routes
    bool FightsWith(Player* bot, Player* member);
    // [deaths12] the largest wander distance of the map's spawns (a wanderer's reach is its home's plus it)
    float MapWanderReach(uint32 mapId);
    // [party59] the pack a fight against `target` at `fight` brings, as the solo judge's pull model counts it (the
    // linked pack, engage and periodic assistance calls with the wanderers' areas and patrol beats, the casters'
    // stands, script calls, flee for help), the idle mobs whose reach covers a member's spot in `stands` too; the
    // target itself is not in it. Judged with `bot`'s level and reach
    std::vector<Creature*> PackAt(Player* bot, Creature* target, G3D::Vector3 const& fight,
                                  std::vector<G3D::Vector3> const& stands);
    // [death396] the spread of `mob`'s damage on `victim` per second of the fight (its swings' rolls and its casts'):
    // the variance the judge's high end (two standard deviations) sums over the seconds it lives
    float DamageVariancePerSec(Unit* mob, Player* victim);

    // DeathRecorder's DealDamage hook (map thread): the bot's dealt and taken damage, per fight and per attacker
    void NoteDamage(Unit* attacker, Unit* victim, uint32 damage);
    // DeathRecorder's heal hook: the bot's own heals on itself (its measured self-heal per fight second)
    void NoteHeal(Unit* healer, Unit* target, uint32 heal);

    struct Verdict
    {
        enum class Kind : uint8
        {
            Win,
            RestFirst,
            Refuse
        };
        Kind kind = Kind::Win;
        std::string why;
        uint32 mobs = 0;
        uint32 above = 0;      // mobs above the bot's level
        float damage = 0.0f;   // expected damage on the bot
        float ehpNow = 0.0f;   // health + pet + heals now
        float ehpFull = 0.0f;  // the same at full health and mana
        float fightSec = 0.0f;
        Position at;           // the pack's centre and reach (a refused pack is remembered there)
        float radius = 0.0f;
        uint8 topLevel = 0;
        // [deaths] user 2026-11-08: two mobs whose aggro reaches overlap are no pack unless the core links them
        // (formation, assistance, the script's call): the pull is made from a spot outside the other's reach, walked
        // to round it (`pullAvoid`: x, y, z, radius of those reaches). [pullplan] The fight is fought there: the
        // judge's pack is what the core brings to that spot
        bool pullSpot = false;
        G3D::Vector3 pull;
        std::vector<G3D::Vector4> pullAvoid;
        std::vector<ObjectGuid> counted;  // [death392] the mobs the projection counted (its kill order)
    };
    // the bot's pull of `target` now
    Verdict Judge(Player* bot, Unit* target);
    // the bot engages `target` on the verdict (the fight record's prediction; its pull spot)
    void NoteEngage(Player* bot, Unit* target, Verdict const& verdict);
    // [fight49] the judged fight's safe area: its spot and the yards round it no idle mob's reach the judge left out
    // covers. True when `to` (a chase's end, the bot's own spot) lies outside it; `why` names the reach it would enter.
    // A position past it is judged again there: the joiners it would wake (`why`), the area moved to that spot
    bool FightPastJudged(Player* bot, Unit* target, G3D::Vector3 const& to, std::string& why);
    // [fight50] the judged fight spot of its engage of `target`; false when none
    bool JudgedFightSpot(Player* bot, Unit* target, G3D::Vector3& at);
    // [death395] an attacker of the judged fight stands within the assistance radius of an idle mob that assists it
    // and the judge did not count (Creature::Update's periodic CallAssistance from where each one fights): true with
    // the two named in `why`
    bool AttackerWakes(Player* bot, Unit* target, std::string& why);
    // [death399] a solo bot's loot at `at` lies in an idle hostile's reach (its wander area, a patrol's forecast beat)
    // the bot does not stand in now: the walk there is a pull nobody judged (`who`: the nearest such mob)
    bool LootInReach(Player* bot, G3D::Vector3 const& at, std::string& who);
    // [deaths18] a party judge's engaged pull (the leader's grind target, NewRpgCoop::JudgePull): the death line of a
    // member names a party fight only when this target hit it
    void NotePartyPull(Player* leader, Unit* target);

    // [deaths] the walk of a solo bot that approaches the target it engages from a pull spot (Verdict::pullSpot): its
    // walk to `to` near that target ends at `spot`, round the reaches in `avoid` (Leader::Route); false: no pull spot
    // applies (the target fights it already, another target, another walk)
    bool PullWalk(Player* bot, G3D::Vector3 const& to, G3D::Vector3& spot, std::vector<G3D::Vector4>& avoid);
    // [lag370] the engage's pull spot has a walk from where the bot stands (Leader::Route: found, through no refused
    // reach); none: the pull spot is dropped (logged) and the engage is judged again from here
    void CheckPullSpotWalk(Player* bot, Unit* target);
    // [pullplan] the pull itself, in the bot's combat walk to `target` (MovementAction::ReachCombatTo): at its pull
    // spot a melee bot with Throw / Shoot casts it (Hold); once the target comes for it the bot waits for it at the
    // spot (Hold) or walks back there after a body pull's step in (ToSpot, `spot`); None: the walk goes on as before
    enum class PullMove : uint8
    {
        None,
        Hold,
        ToSpot
    };
    PullMove PullStep(Player* bot, Unit* target, G3D::Vector3& spot);

    // the bot wants to rest to full (a pull that needs it, a revive); `mana`: the drink side
    bool WantsRest(Player* bot, bool mana, std::string* why = nullptr);
    void RequestRest(Player* bot, std::string const& why);
    // it wants to rest and has nothing to eat / drink for it: it sits and regenerates
    bool WaitsToRegen(Player* bot);

    // [guild15] the walk's stretch in sight toward `dest` passes an idle mob whose fight the judge wins only at full:
    // the bot rests to full first (true: the rest is asked for)
    bool RestBeforeWalk(Player* bot, Position const& dest);
    // its gear was repaired (nothing to do: [dps1] its damage is the formula over the core's stats)
    void OnRepaired(Player* bot);
    // its gear, spells or talents changed (logged; [dps1] the formula's cache sees the change itself)
    void OnPowerChanged(Player* bot, char const* why);
    // a spell that changes its fighting power (not a profession recipe)
    bool PowerSpell(uint32 spellId);
    // [dps1] a hunter without its pet: the share of a fight against a mob of its level it stands in melee, computed
    // (the mob's run from Auto Shot's range over the fight the formula's damage gives); -1 when unknown
    float MeleeShare(Player* bot);
    // [dps1] the 'solo' row of playerbots_db_store is no longer read (kept as a no-op for the loader)
    void LoadMeasured(Player* bot, std::string const& value);
    // the bot died: rest to full and the walk to its hub after the revive
    void OnDeath(Player* bot);
    // once after the revive: true and the hub to walk to
    bool TakeRecovery(Player* bot);
    // [regress10] alive with that recovery not taken yet, or a rest asked for and not full yet: no new pull
    bool HoldsPulls(Player* bot);
    // [regress10] the idle mobs whose aggro reach (wander area, patrol beat) covers where the bot stands (`who`: their
    // names) and the spots that leave every idle reach round it, nearest first ([deaths140] where each of 16
    // directions leaves the union of the reaches, its combat reach past the last rim, within its sight); false when
    // none covers it
    // [login67] atRestart: the reaches its next login meets - every mob at its home (the restart respawns the dead
    // ones and the fighting ones are back home), dead and fighting mobs too
    bool StandsInReach(Player* bot, std::string& who, std::vector<G3D::Vector3>& spots, bool atRestart = false);
    // [deaths140] a wait (walled-in hold, Hearthstone cooldown) found no spot out of `who`'s reaches with a clean walk:
    // logged once per bot and set of reaches, the wait is taken where it stands. A log, no decision
    void NoteNoWayOut(Player* bot, std::string const& who, size_t spots, char const* site);
    // [reach66] the mob its judge last refused (or would fight only at full) is alive, idle and its aggro reach
    // covers the bot where it stands: `who` names it
    bool InRefusedReach(Player* bot, std::string& who);
    // [objective23] a spot within `range` of `target` (and in its sight when `sight`) out of every other idle mob's and
    // wanderer's reach, the nearest to the bot (its own spot when it is one); false: no such spot (the objective
    // leaves none). `who`: the mobs whose reach covers the bot's own spot ("" when none)
    bool WorkSpot(Player* bot, WorldObject* target, float range, bool sight, G3D::Vector3& at, std::string& who);
    // [spot35] user 2026-11-09: an orange / red or elite roamer (a wanderer's home + wander distance + aggro reach,
    // a patrol's path + aggro reach) idle and hostile whose round covers `at`: true, `who` its name and level
    bool RoamerReaches(Player* bot, G3D::Vector3 const& at, std::string& who);
    // [deaths14] a solo bot out of a fight stands in the aggro reach of an idle hostile (any: idle, wanderer, patrol)
    // and a spot out of every reach is there: its rest waits until it stands there (NewRpgBaseAction::RestEscape)
    bool RestSpotInReach(Player* bot);
    // [deaths14] the bot stands where no spot round it (out to its sight) is out of every idle hostile's reach and two
    // or more of those reaching it roam (a wander area, a patrol beat): between its pulls the roamers walk into it one
    // after another - a camp for a party, not a solo bot (`why`: the roamers)
    bool DenseCamp(Player* bot, std::string& why);
    // the death line's context: attackers now, the fight's start
    std::string DeathContext(Player* bot);

    // Leader::Route for a solo bot: the idle mobs round the walk, refused (`refuse`: x, y, z, radius) or priced
    void RouteCosts(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& to, std::vector<G3D::Vector4>& refuse,
                    std::vector<Leader::Travel::RoutePenalty>& penalties);
    // [pull9] the mobs its last route search refused, nearest first, and why (for the refused-walk lines)
    std::string RouteWalls(Player* bot);
    // [wall17] the orange / red and elite mobs among its last route search's walls (empty: none) - [guild12] never
    // fights them; a quest walled by them goes to a party (NewRpgCoop::OnSpotRefused)
    std::string ConWalls(Player* bot);

    // [deaths] what that route search gave (complete; through a refused reach): kept for the death line
    void NoteRoute(Unit* walker, bool complete, bool throughRefused);
    // [deaths] user 2026-11-08: the walk `points` (from the bot on) passes the reach of mobs its last route search
    // refused: when every one of them is a patrol, the seconds until a walk started then meets none of them anywhere
    // on it (their waypoint forecast, PullPlanning::PatrolForecast) - a computed ETA, no timer; false: a refused mob
    // that does not patrol covers the walk, or no start within the forecast clears it. `who`: the patrols
    bool PatrolWait(Player* bot, std::vector<G3D::Vector3> const& points, float& waitSec, std::string& who);

    // [deaths] user 2026-11-08 (b337 Dekorhan L7: a run priced 106 at hp 106 killed it): what `mob` casts on a bot
    // running from it over `seconds`, from its SmartAI - its in-combat casts (SMART_EVENT_UPDATE_IC) and its casts at
    // range (SMART_EVENT_RANGE with a minimum range: Shoot, Throw, cast only while the bot is away) - a repeating
    // cast once per repeat (at least its cast time and the global cooldown) from the run's start, one that does not
    // repeat (SMART_EVENT_FLAG_NOT_REPEATABLE) once
    float ChaseSpellDamage(Creature* mob, float seconds);
    // [regress10] user 2026-11-08 "you can just run past": a mob the bot walks past without fighting chases it until
    // the core's leash lets it go (Creature::CanCreatureAttack: past CONFIG_CREATURE_LEASH_RADIUS from its reset spot
    // and its leash timer since the engage), hitting all the way when it is faster (daze from behind); the damage of
    // that chase and how long it lasts - the flee's chase model. RunPastTemplate prices a spawn not loaded (its
    // template at its top level: the core's base stats)
    struct RunPast
    {
        float damage = 0.0f;
        float seconds = 0.0f;
        float variance = 0.0f;  // [death386] of the chasers' swings over their exposure
        // [death386] the high end of its rolls (two standard deviations): the price a run past is judged on
        float High() const;
        void Add(RunPast const& chase);  // one more chaser: damage and variance add, the longest chase
    };
    // [lag360] `withSpeed`: with the speed piece the run-past executor casts first (the price); false: without it
    RunPast RunPastChase(Player* bot, Creature* mob, bool withSpeed = true);
    RunPast RunPastTemplate(Player* bot, CreatureTemplate const* tmpl);
    // [death407] using `go` (a quest object) fires its linked trap (GameObjectTemplate::GetLinkedGameObjectEntry): the
    // creatures its trap spell summons are that objective's fight - true when the bot loses it from its health now
    // with the heals its mana holds (`why`: the summons and the price)
    bool QuestObjectSummonsLose(Player* bot, GameObject* go, std::string& why);
    // the bot lives through `damage` over `seconds` from its health now with its heal kit (the flee's test)
    bool SurvivesRunPast(Player* bot, float damage, float seconds);
    // [death400] the walk `points` (from the bot on) out of reaches it stands in: every idle mob whose reach (wander
    // area, patrol beat) it crosses chases it with the ones their engage calls bring - true when it lives through
    // their chases at the high end (`why`: the chasers and the price)
    bool RunOutSurvives(Player* bot, std::vector<G3D::Vector3> const& points, std::string& why);
    // [runpast11] the run-past executor. The route search of a walk from where the bot stands keeps the mobs it priced
    // as run past (their chase each); CommitRunPast makes that set the walked leg's (`points`: from the bot on).
    // RunPastStep, in a fight: every attacker is one of the leg's set, the bot is still on the leg, the kit does not
    // run (Disengaging), the chases still to come leave it alive (SurvivesRunPast over RunPastChase now) and no single
    // attacker is a fight the judge wins for less damage than its chase -> true with the rest of the leg (`rest`, from
    // the bot on) and the RUN_PAST line for a mob not named yet this leg (`line`, empty when none). False: it fights as
    // before. The leg ends at its end, off it, or at the next commit
    // [runpast15] `searched`: the whole searched walk the leg was cut from (to the search's end, past the leg's sight)
    void CommitRunPast(Player* bot, std::vector<G3D::Vector3> const& points,
                       std::vector<G3D::Vector3> const& searched = {});
    // [runpast15] the stop of the walk being searched (a far walk's destination) while its stage / leg searches run
    // on this thread: RouteCosts measures a run past's walk on to that stop, not to the search's own end (a stage two
    // sights ahead). b355: 953 "[deaths14] no run past" walls measured to the stage - MAZE 20 -> 57, MOVE_FAR_LOOP
    // 16 -> 57 against b354, no run past walked at all
    struct WalkStopScope
    {
        explicit WalkStopScope(G3D::Vector3 const& stop);
        ~WalkStopScope();
        WalkStopScope(WalkStopScope const&) = delete;
        WalkStopScope& operator=(WalkStopScope const&) = delete;
        bool had;
        G3D::Vector3 previous;
    };
    // O(1): a committed leg with a run-past set (the trigger's check; RunPastStep is the action's)
    bool HasRunPastLeg(Player* bot);
    // [death384] the target is a mob of the bot's run past under way: no hit or hostile cast at it (each one renews
    // its leash, Unit::DealDamage / Unit::AtTargetAttacked -> Creature::UpdateLeashExtensionTime); logs once per bot
    bool RunPastChaser(Player* bot, Unit* target);
    void SetRunPastWalking(Player* bot, bool walking);  // [death384] the executor's look
    bool RunPastStep(Player* bot, std::vector<G3D::Vector3>& rest, std::string& line);

    // [dps1] the bot's damage per second: DpsEstimate's formula over its state now (its buff context: solo / party /
    // raid), against `target` or a mob of its level; no fight history
    float BotDps(Player* bot, Unit* target = nullptr);

    // [guild80] the open solo fight projected: the attackers on the bot (their damage per second: this fight's
    // measurement, else the model; their SmartAI spell share), the time its damage needs for them, the damage still to
    // come against its health and its measured heals. lost: damage >= health + heals
    struct Projection
    {
        struct Foe
        {
            Unit* unit = nullptr;
            float hp = 0.0f;
            float dps = 0.0f;
            float spellDps = 0.0f;
        };
        bool open = false;
        bool lost = false;
        bool fleeing = false;
        std::vector<Foe> foes;
        float incoming = 0.0f;
        float seconds = 0.0f;
        float damage = 0.0f;
        float health = 0.0f;
        float heals = 0.0f;
        float botDps = 0.0f;
        float leashSec = 0.0f;
    };
    Projection Project(Player* bot);
    // [guild80] the survival kit chose to run until the mobs leash (the fight closes as DISENGAGED, rest after)
    void BeginFlee(Player* bot, std::string const& why);
    // [guild15] the run found no way out and stopped: the fight goes on as a fight (its close is won / lost, measured)
    void EndFlee(Player* bot);
    // [guild80] it rests to full once the fight is over (a reset by Vanish / Feign Death / Shadowmeld)
    void RequestRestAfter(Player* bot, std::string const& why);

    // [guild14] the open solo fight is projected lost (the attackers' damage still to come against its health, heals
    // over the time its damage needs to kill them) and running until they leash leaves it alive: it disengages (logged,
    // rests to full after) - only from mobs that cannot follow (rooted, stunned); true from then on until the fight
    // ends
    bool ShouldDisengage(Player* bot);
    // [guild14] it disengages now (cheap: the flag only)
    bool Disengaging(Player* bot);
}

#endif
