/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_LEADERFIGHTCHECKS_H
#define PLAYERBOTS_LEADER_LEADERFIGHTCHECKS_H

// [lead107] user: production code does not live in the test harness. The leader's own checks, moved out of
// TestHarnessRunner.cpp unchanged: the idle-mob test, the AoE rule, the taunt reach, the unpulled-mob reach of a
// spot and of a walk, the spot searches (follow, healer, cleave exit, hazard, combat spots), the fight-mob, patrol,
// room-edge, walk and reachability facts, the attackers' expected damage.

#include "Creature.h"
#include "DungeonLeaderState.h"  // [lead1011] Unreachable
#include "HazardZone.h"
#include "LeaderLog.h"
#include "LeaderOrders.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Position.h"
#include <functional>
#include <initializer_list>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class PlayerbotAI;
class SpellInfo;
class Unit;

namespace PullPlanner
{
    struct Decision;  // ChooseTargetActions.h
}

namespace Leader::Fight
{
    // The skip_target detour metric: a walk longer than DETOUR_RATIO x the straight distance and longer than that
    // + DETOUR_EXTRA is a detour.
    constexpr float DETOUR_RATIO = 3.0f;
    constexpr float DETOUR_EXTRA = 60.0f;

    // [lead18] the spot guard's idle-mob scan round the tank (the event tick, LeaderAssignSpots); [lead1121] the
    // guard scans round a spot or walk point this scan does not cover (LeaderAggroAt)
    constexpr float LEADER_IDLE_SCAN_YD = 80.0f;

    struct IdleAggroCheck
    {
        Player* leader;
        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsInCombat() && !creature->IsCritter() &&
                   !creature->IsCivilian() && !creature->IsTotem() &&
                   creature->GetCreatureTemplate()->type != CREATURE_TYPE_TOTEM && !creature->IsTrigger() &&
                   creature->HasReactState(REACT_AGGRESSIVE) && !creature->IsImmuneToPC() &&
                   !creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) &&
                   !creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) && creature->IsHostileTo(leader) &&
                   // A stealthed mob the leader cannot detect (Laughing Skull Rogue, Blood Furnace wave 24) is
                   // no pull the tank can make: holding at its edge stalled the run. It comes when it notices.
                   leader->CanSeeOrDetect(creature);
        }
    };

    constexpr uint32 LEADER_WAKE_LOG_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 LEADER_TANK_STILL_MS = 3 * IN_MILLISECONDS;   // the tank stands this long: Prepare
    constexpr uint32 LEADER_CALM_TICK_MS = 250;                    // the calm cadence (out of combat)
    constexpr uint32 LEADER_FIGHT_TICK_MS = 300;                   // [lead72] the user's 0.3 s in a fight
    constexpr uint32 LEADER_AOE_RECHECK_MS = 500;
    constexpr float LEADER_LOOSE_TANK_YD = 2.5f;                   // the victim's spot next to the tank
    constexpr float LEADER_PATROL_GROUP_YD = 8.0f;                 // [lead32] a patrol's walking group
    constexpr uint32 SPELL_ARMY_OF_THE_DEAD = 42650;
    // the tank's taunts and AoE threat spells (3.3.5a; every rank the leader may assign, highest last)
    constexpr uint32 TANK_TAUNTS[] = {355, 6795, 62124, 56222, 31789};
    constexpr uint32 TANK_AOE_THREAT[] = {43265, 49936, 49937, 49938, 48721, 49939, 49940, 49941, 26573, 20116,
                                          20922, 20923, 20924, 27173, 48818, 48819, 6343, 8198, 8204, 8205,
                                          11580, 11581, 25264, 47501, 47502, 779, 780, 769, 9754, 9908, 26997,
                                          48561, 48562};
    constexpr float LEADER_FOLLOW_START_YD = 6.0f;                 // farther from the tank: walked along
    constexpr float LEADER_FOLLOW_TOLERANCE = 2.5f;
    constexpr uint32 LEADER_HEAL_REACH_CHECK_MS = 1000;
    constexpr uint32 LEADER_HEAL_REACH_LOG_MS = 5 * IN_MILLISECONDS;
    constexpr float LEADER_AOE_RADIUS = 10.0f;     // the AoE rule's radius around the target
    constexpr float LEADER_AOE_GUARD_YD = 3.0f;    // ... and the margin for a mob the AoE must not touch
    constexpr uint32 LEADER_OPENING_MS = 6 * IN_MILLISECONDS;
    constexpr float LEADER_SAVE_HP_PCT = 30.0f;
    constexpr float LEADER_SAVE_MANA_PCT = 30.0f;
    // the external saves / buffs the leader assigns (3.3.5a ids; any known rank of Hand of Protection)
    constexpr uint32 SPELL_MISDIRECTION = 34477;
    constexpr uint32 SPELL_TRICKS_OF_THE_TRADE = 57934;
    constexpr uint32 SPELL_PAIN_SUPPRESSION = 33206;
    constexpr uint32 SPELL_HAND_OF_SACRIFICE = 6940;
    constexpr uint32 SPELL_HAND_OF_PROTECTION_1 = 1022;
    constexpr uint32 SPELL_HAND_OF_PROTECTION_2 = 5599;
    constexpr uint32 SPELL_HAND_OF_PROTECTION_3 = 10278;
    constexpr uint32 SPELL_INNERVATE = 29166;
    constexpr uint32 SPELL_HYMN_OF_HOPE = 64901;
    constexpr uint32 SPELL_HEROISM = 32182;    // [lead105]
    constexpr uint32 SPELL_BLOODLUST = 2825;
    constexpr uint32 SPELL_DIVINE_HYMN = 64843;
    constexpr uint32 SPELL_POWER_INFUSION = 10060;
    constexpr uint32 SPELL_GUARDIAN_SPIRIT = 47788;  // [lead300]
    constexpr uint32 SPELL_LAY_ON_HANDS_1 = 633;
    constexpr uint32 SPELL_LAY_ON_HANDS_2 = 2800;
    constexpr uint32 SPELL_LAY_ON_HANDS_3 = 10310;
    constexpr uint32 SPELL_LAY_ON_HANDS_4 = 27154;
    constexpr uint32 SPELL_LAY_ON_HANDS_5 = 48788;
    constexpr uint32 SPELL_MANA_TIDE_TOTEM = 16190;

    // the first of `spells` the bot knows and has off cooldown (the highest rank listed last); 0: none
    uint32 LeaderReadySpell(Player* bot, std::initializer_list<uint32> spells);

    // the bot's interrupt with no cooldown running (Kick, Pummel, Shield Bash, Counterspell, Wind Shear, Mind
    // Freeze, Silence); 0: none
    uint32 LeaderReadyInterrupt(Player* bot);

    // [lead840] the bot channels a mana tool now (Evocation, Hymn of Hope: the core's channelled spell): the mana the
    // boss line counted comes only while nothing breaks the channel - a move, an item's or a spell's cast
    bool LeaderChannelsManaTool(Player* bot);

    // [lead460] the bot's ready interrupt reaches `caster` from where the bot stands (the spell's own range: melee
    // reach for Kick / Pummel / Shield Bash / Mind Freeze, its yards for Counterspell / Wind Shear / Silence)
    bool LeaderInterruptReaches(Player* bot, Unit* caster);

    // [lead776] what the bot's ready interrupt costs its own damage, as a rank (lower: cheaper): 0 off the global
    // cooldown with a mana cost or none, 1 off it with energy / rage / runic power (the resources its attacks are
    // bought with), 2 on the global cooldown (a cast of its own lost)
    uint8 LeaderInterruptCostRank(Player* bot);

    // the healer at `from` heals `to`: in range (LEADER_HEAL_REACH_YD) and in sight
    // [lead36] LEADER_SPOT_ACQUIRE_MARGIN while a new spot is picked (SpotAcquire), 0 for a held spot's check
    extern float g_spotAcquireMargin;
    struct SpotAcquire
    {
        SpotAcquire() { g_spotAcquireMargin = Leader::LEADER_SPOT_ACQUIRE_MARGIN; }
        ~SpotAcquire() { g_spotAcquireMargin = 0.0f; }
    };

    // [lead950] `healer`: whose eyes stand at `from` (the game's line of sight runs from each body's collision
    // height); none: the tank's own height at both ends
    bool HealReaches(Player* tank, Position const& from, Position const& to, Unit const* healer = nullptr);

    // [lead124] the member's own attack reach: the range its rotation positions by (PlayerbotAI::GetRange "spell":
    // the configured spell distance, a hunter's weapon range); 0 for no bot AI
    float LeaderShootReach(Player* bot);
    // [lead157] the nearest a ranged member stands to `target` (centre to centre): out of the target's melee reach,
    // and a hunter out of Auto Shot's dead zone (its minimum range) as well
    float LeaderRangedMinGap(Player* bot, Unit* target);
    // [lead127] the member standing at `at` sees `target` (the game's IsWithinLOSInMap from there)
    bool LeaderSeesFrom(Player* bot, Unit* target, Position const& at);
    // [lead580] the member standing at `at` hits `target` in melee: in the game's melee range of it from there
    // (Unit::GetMeleeRange, centre to centre in 3D) and in the game's line of sight of it (LeaderSeesFrom)
    bool LeaderMeleeReachesFrom(Player* bot, Unit* target, Position const& at);
    // [lead580] a melee place round `target`: on the ground the core lets the member stand on next to it
    // (CheckCollisionAndGetValidCoords from the target), one LeaderMeleeReachesFrom passes. Behind the target first
    // (out of its front half), `side` (+1 / -1) the side tried first; in front of it only when no place behind fits.
    // `behind`: the place found is out of its front half. False when no place round it fits
    bool LeaderMeleePlace(Player* bot, Unit* target, float side, Position& out, bool* behind = nullptr);
    // [lead203] a ranged member standing at `at` casts on `target`: within its spells' range (the larger of its
    // positioning reach and LeaderSpellReach), out of Auto Shot's dead zone (a hunter), in sight. The one fact both
    // combat spot rules leave a spot by
    // [lead210] `fightMobs`: and out of the melee reach of every one of them (the game takes a mob off its victim for
    // a member in its melee reach at 110 % of the victim's threat, at range only at 130 %: ThreatManager)
    // [lead380] `whereTheyStand`: every mob judged where it stands now (the fact for a member standing at `at`), not
    // where a moving one runs to
    bool LeaderCastsFrom(Player* bot, Unit* target, Position const& at,
                         std::vector<ObjectGuid> const* fightMobs = nullptr, bool whereTheyStand = false);
    // [lead210] `at` is out of the melee reach (LeaderRangedMinGap) of every living mob of `fightMobs` ([lead214] a
    // mob on `bot` itself left out, a moving one judged at its victim - [lead380] or where it stands now when
    // `whereTheyStand`: the spot under the member's feet is judged by the facts, a spot it walks to by where the
    // mobs will be)
    bool LeaderOutOfMobsMelee(Player* bot, Position const& at, std::vector<ObjectGuid> const& fightMobs,
                              bool whereTheyStand = false);

    // the AoE rule around the target: AllowedOnPack, with `spared` the mobs no area spell may reach - a crowd-
    // controlled mob, a mob not in the fight (an unpulled pack) or one without the tank's threat - within `range` of it
    // ([lead415] each member's AoE checks its own spell's footprint against them)
    Leader::AoeRule LeaderAoeRule(Player* tank, Unit* target, float range, std::string& why,
                                  std::vector<ObjectGuid>& spared);

    // [lead420] the largest area radius among the bot's known offensive spells (spell data, cached per spell set)
    float LeaderMaxAreaRadius(Player* bot);
    // [lead1096] the longest lasting ground field among the bot's spells (a persistent area aura's duration, spell
    // data), seconds; 0: none
    float LeaderMaxFieldSec(Player* bot);
    // [lead1100] the bot's longest ground field: its spell (0: none) and its radius (spell data)
    uint32 LeaderFieldSpell(Player* bot);
    float LeaderMaxFieldRadius(Player* bot);
    // [lead1100] user 2026-11-04 (Q9): an unpulled mob walks into a ground field of `radius` round `center` before
    // it ends (`fieldSec`): a patrol only when its waypoint walk (its walker's forecast) enters the radius within the
    // field's seconds, a wanderer only when its wander radius round its home reaches the field and its walk gets
    // there in time, any other walk under way only when it ends inside. Returns how ("patrol" / "wanderer" /
    // "walk"); empty: it does not come.
    std::string FieldWalkerEnters(Creature* mob, Position const& center, float radius, float fieldSec);
    // [lead1100] the damage one cast of the field `info` deals on one target over its duration (the core's
    // damage bonus with the caster's spell power / attack power coefficients, spell data ticks)
    float FieldDamagePerTarget(Player* bot, Unit* target, SpellInfo const* info);

    // [lead53] the tank's taunt reach: its known taunt spell's range (0: none known)
    float LeaderTauntReach(Player* tank);

    // [lead1011] an unreachable verdict with the facts it was judged under (the tank's and the mob's positions, the
    // tank's own state and fight state, the last fight's end, the mob's fight state)
    Leader::DungeonLeaderState::Unreachable LeaderUnreachable(Player* tank, Unit* mob, std::string const& why,
                                                              uint32 peaceSinceMs);
    // [lead1011] "" while the verdict's facts hold - else the fact that changed (the verdict ends)
    std::string LeaderUnreachableChanged(Leader::DungeonLeaderState::Unreachable const& verdict, Player* tank,
                                         Unit const* mob, uint32 peaceSinceMs);

    // the member's walk-along spot behind the tank (two rows of two), on reachable ground
    Position LeaderFollowSpot(Player* tank, uint32 index);

    // [lead18] the spot guard of every leader spot: the idle mobs around the group (refreshed by the event tick),
    // the group's walked ground (cleared, LEADER_PREFER_CLEARED). World thread only.
    extern std::vector<Creature*> const* g_leaderIdle;
    // [lead1121] where g_leaderIdle was scanned from (LEADER_IDLE_SCAN_YD round it); nullptr: unknown, every point
    // is scanned round itself too
    extern Position const* g_leaderIdleAt;
    extern std::vector<Position> const* g_leaderCleared;
    extern std::vector<ObjectGuid> const* g_leaderExempt;  // [lead24] the committed pull's mobs
    // [lead600] the idle packs the leader decided into the fight (lead76 pack_joins): no member keeps away from them
    extern std::set<ObjectGuid> const* g_leaderJoined;

    // [lead24] the committed pull's own mobs (target, pack, expected pack)
    std::vector<ObjectGuid> LeaderPullMobs(std::shared_ptr<PullPlanner::Decision> const& commit, ObjectGuid mob);

    bool LeaderOnCleared(Position const& spot);

    // the unpulled mob whose reach (the planner's IdleMobReachesSpot, plus LEADER_SPOT_AGGRO_MARGIN towards it)
    // covers the spot; nullptr: none
    Creature* LeaderAggroAt(Player* bot, Position const& spot);
    // [lead490] b209 rfc-h: 16 of 16 "at target" melee candidates rejected "an idle mob's aggro reach" for the enh
    // shaman, warriors, rogues, DKs, ret, cat - the pulled target stood inside an idle pack's reach, and the tank
    // fought it there without waking it. [lead492] ranged too: "ring: an idle mob's aggro reach" x64-112 on
    // Bazzalan, Taragaman, Oggleflint, Searing Blade Warlocks. For a fight spot on `target`: the unpulled mob whose
    // reach covers the spot AND to which the spot lies nearer than the fight already stands in its reach (the
    // target's victim where it stands; [lead500] not the target: a mob beside its idle pack wakes none); nullptr: none
    Creature* LeaderFightAggroAt(Player* bot, Position const& spot, Unit* target);

    // the walk to the spot passes no unpulled mob's reach (and there is a walk)
    // [lead490] `fightTarget`: the walk of a member to its fight spot on that target (LeaderFightAggroAt per point)
    Creature* LeaderAggroOnWalk(Player* bot, Position const& spot, bool& noPath, Unit* fightTarget = nullptr);

    // a candidate of a spot search: 0 rejected (an unpulled mob reaches it), 1 valid on new ground (kept as the
    // fallback), 2 valid on cleared ground (taken at once); `fightTarget`: a fight spot on it (LeaderFightAggroAt)
    int LeaderCandidate(Player* bot, Position const& spot, std::optional<Position>& fallback,
                        Unit* fightTarget = nullptr);

    // a free spot near the healer, out of every cleave cone, from which it reaches everyone (`keeps`)
    bool LeaderHealerRespot(Player* healer, PlayerbotAI* healerAI,
                            std::function<bool(Position const&)> const& keeps, Position& out);

    char const* MoveEventName(Leader::MoveEvent move);

    // A free spot out of every cleave cone near the member, nearest first (DungeonTactics's exit steps), that
    // `keeps` (the heal reach).
    bool LeaderCleaveExit(Player* bot, PlayerbotAI* botAI, Position& out,
                          std::function<bool(Position const&)> const& keeps);

    // [lead16] the leader's hazard view: per member, the circles "avoid aoe" would flee (AvoidAoeAction::
    // CollectZones), the hostile area-spell dynamic objects near it, its DungeonHazard circles (role-specific)
    constexpr float LEADER_HAZARD_SCAN_YD = 25.0f;
    constexpr uint32 LEADER_HAZARD_SCAN_MS = 200;   // the slow layer's rescan; membership is checked every tick
    constexpr float LEADER_HAZARD_EDGE = 2.0f;      // a safe spot stands this far outside every circle
    constexpr float LEADER_HAZARD_DZ = 5.0f;        // a circle on another floor does not count
    constexpr float LEADER_TERRAIN_EDGE = 2.0f;     // and this far from lava / slime / a drop-off
    constexpr float LEADER_HEALER_YD = 20.0f;
    constexpr uint32 LEADER_SPOT_LOS_MS = 500;      // a combat spot's sight re-check

    void LeaderCollectHazards(Player* bot, std::vector<HazardZone>& out);

    // the circle of `zones` that holds (x, y, z) (with `extra` around it), nullptr: none
    HazardZone const* LeaderZoneAt(std::vector<HazardZone> const& zones, float x, float y, float z, float extra);

    // the spot is fit to stand on in a fight: out of every hazard and cleave cone, off lava / drop-offs
    bool LeaderSpotSafe(Player* bot, PlayerbotAI* botAI, std::vector<HazardZone> const& zones, float x, float y,
                        float z);

    // the nearest safe spot around the member that `keeps` (the heal reach), on its floor
    bool LeaderSafeSpot(Player* bot, PlayerbotAI* botAI, std::vector<HazardZone> const& zones,
                        std::function<bool(Position const&)> const& keeps, Position& out);

    // a combat spot still fits: in its window from the target (melee: behind it), safe, in heal reach; `sight`:
    // also the line of sight to the target (ranged / healer)
    // [lead455] coordinator 2026-11-02 (Q4): combat_spot_none names why each candidate failed. While one lives on
    // the thread, the spot searches (LeaderCombatSpot, LeaderNearestCombatSpot, the tank-trail fallback) count every
    // rejected candidate by its search and reason; Summary() lists them ("ring: off the target's floor x16; ...")
    struct SpotRejects
    {
        SpotRejects();
        ~SpotRejects();
        SpotRejects(SpotRejects const&) = delete;
        SpotRejects& operator=(SpotRejects const&) = delete;
        std::string Summary() const;
        std::map<std::string, uint32> counts;
        SpotRejects* previous = nullptr;
    };
    void NoteSpotReject(char const* search, std::string const& why);

    bool LeaderCombatSpotValid(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& spot, bool melee,
                               std::vector<HazardZone> const& zones,
                               std::function<bool(Position const&)> const& keeps, bool sight);

    // a new combat spot: melee behind the target, ranged at its own reach (LeaderShootReach) / the healer at LEADER_HEALER_YD
    // nearest the member's own side, each safe, in heal reach and (ranged) in sight of the target
    bool LeaderCombatSpot(Player* bot, PlayerbotAI* botAI, Unit* target, bool melee,
                          std::vector<HazardZone> const& zones, std::function<bool(Position const&)> const& keeps,
                          Position& out, std::vector<ObjectGuid> const* fightMobs = nullptr);

    // [lead21] a mob of the fight: a hostile creature the tank may attack, never a player's summon or guardian
    // (slabs-h 20261026-195203: the tank was told to take back the mage's Mirror Image 1187 times)
    bool LeaderFightMob(Player* tank, Unit* unit);

    // [lead18] the nearest walking patrol (idle, a waypoint walker or on the move, not of the committed pull)
    // around the pull spot or the tank; `gap`: its distance to the nearer of the two, `reach`: its aggro reach
    Creature* LeaderPatrolNear(Player* tank, Position const& spot, PullPlanner::Decision const& d, float& gap,
                               float& reach);

    // [lead18] LEADER_EDGE_FIRST: the room around `picked` (idle mobs within 40 yd of it, patrols out) as packs
    // (linked within 8 yd); the edge packs have the fewest other packs within 20 yd. The pick moves to the edge
    // pack nearest the tank when `picked` is not on the edge. `order`: the room, edge first (room_order).
    Unit* LeaderEdgePick(Player* tank, Unit* picked, std::string& why, JsonArray& order, std::string& sig);

    // [lead18] a spot the member walks to: a path from where it stands, no idle mob (outside the pull) reaching it
    bool LeaderMemberWalks(Player* bot, Position const& spot, std::list<Creature*> const& idle,
                           std::vector<ObjectGuid> const& pack);

    // [lead45] a boss is always walked to: never a refused pick (its hold is a readiness wait, not a refusal)
    bool IsBossLook(Player* tank, ObjectGuid guid);

    // [lead63] user: «берите физ. атаку / маг. атаку того, кто бьёт» - an attacker's expected damage per second on
    // `victim`, from the server's numbers: its melee damage range (the unit's final min / max damage) over its
    // attack time, after the victim's armor (Unit::CalcArmorReducedDamage at the attacker's level), times the
    // swings that land (1 - the victim's dodge, parry and miss chance), minus the blocked share (block chance x
    // shield block value); plus the spell it is casting now (its school damage effects over its cast time, at
    // least the global cooldown). `spell`: that spell part
    float AttackerDps(Unit* attacker, Unit* victim, float& spell);

    // [lead64] where a melee member waits at a pull: short of the meeting point on its own side by
    // LEADER_LOOSE_TANK_YD (next to the tank), never into the mob before the tank has hit it
    Position LeaderMeleeWait(Player* bot, Position const& meet);

    // [lead42] how many members refused their Attack on `unit` this fight
    int64 RefusedCount(std::map<ObjectGuid, std::set<ObjectGuid>> const& refused, Unit const* unit);

    // [lead40] the leader's reachability verdict of a mob for `walker`: the route walk to it (EvaluatePath) and the
    // skip_target detour metric (DETOUR_RATIO / DETOUR_EXTRA). Empty: reachable on a fair walk; else why not
    // [lead79] `onRoute`: the mob lies on the route's path (Retarget's route rules): the route IS that walk, so
    // only "no walk at all" refuses it - a winding route is no detour (b122: "detour (534 yd for 102 yd)" on the
    // way to Taragaman, which b100 walked fine)
    std::string LeaderWalkVerdict(Player* walker, Creature* mob, bool onRoute = false);

    // [lead39] a walk on the walker's floor: a full path, no step down or up over 2.5 yd (no ledge drop)
    bool LeaderWalkStaysOnFloor(Player* bot, Position const& to);

    // [lead39] fight_drag: the tank's fight moved by the leader (from, to, the distance, off the meeting point)
    void LogFightDrag(DecisionRun const& run, Player* tank, Position const& to, Position const& meet,
                      char const* why);

    // [lead18] the member's spell reach to its target (a hunter 35 yd, a caster 30 yd)
    float LeaderSpellReach(Player* bot);
    // [lead660] the 3D centre gap within which the member's spells reach `target`: LeaderShootReach /
    // LeaderSpellReach plus the member's and the target's combat reach (Unit::IsWithinCombatRange)
    float LeaderCastReach(Player* bot, Unit* target);
    // [lead991] the spec's rotation spells by name (LeaderSpellReach's); the mob is immune to the school of every one
    // the member knows (Unit::IsImmunedToDamageOrSchool: a Molten Elemental to the fire mage's)
    std::vector<char const*> LeaderRotationSpellNames(Player* bot);
    bool LeaderRotationImmune(Player* bot, Unit* mob);
    float LeaderImmuneReach(Player* bot, Unit* mob);

    // [lead18] why a held combat spot no longer fits ("" = it does): out of the member's reach of the target
    // (melee: of its melee reach), out of sight (ranged), a hazard, a cleave, lava / a drop-off, the heal reach
    // [lead380] `whereTheyStand`: the fight mobs' melee reach judged where they stand now (LeaderOutOfMobsMelee)
    std::string LeaderCombatSpotBreaks(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& spot,
                                       bool melee, std::vector<HazardZone> const& zones,
                                       std::function<bool(Position const&)> const& keeps, bool sight,
                                       std::vector<ObjectGuid> const* fightMobs = nullptr,
                                       bool whereTheyStand = false);

    // [lead454] coordinator 2026-11-02 (Q2): a melee member in reach in front of a trash mob stays there when the
    // damage its front loses over the mob's remaining life costs less than the walk round it. The loss: the core's
    // parry chance (Unit::GetUnitParryChance) plus its block chance times the share of the member's average hit the
    // mob's block value takes (mobs neither parry nor block from behind) over ExpectedLifetime; the walk: the arc at
    // its distance into the mob's back half (out of HasInArc(pi)) at its run speed. Bosses keep behind, and so do the
    // members whose rotation needs the back (a cat's Shred, a Subtlety rogue's Backstab). `why`: the terms
    // [lead680] the core's roll (expertise, weapon skill vs the mob's level, no-parry / no-block flags) and the walk
    // the chase really takes (to the first point in reach out of the front half) against the walk to the nearest
    // place in reach (none where the member stands in reach, else the place on the line to the mob once the fight
    // stands); `standAt`: that place when the front costs less
    bool LeaderFrontCostsLess(Player* bot, Unit* target, std::string* why = nullptr, Position* standAt = nullptr);

    // [lead690] user 2026-11-03 ("the mage also has Blink to move faster"): the member's forward leap (the mage's
    // Blink: SPELL_EFFECT_LEAP, its radius from spell data) and its length (0: none)
    uint32 LeaderLeapSpell(Player* bot, float* leap = nullptr);
    // [lead690] the leap saves time on the member's walk to `to`: ready, the mana there, the walk (the leader's route)
    // at least the leap long and straight for the leap (its point the leap's length along lies the leap off, within
    // the member's combat reach), in sight, the leap's line and landing clear of hazard zones, lava / drop-offs and
    // idle mobs' aggro reach; the walk seconds it saves at the run speed beat its cast time plus the global cooldown
    // it starts where the walk left after it is shorter. `why`: the terms
    // [lead691] `manaLine` (% of max mana, < 0: none): the member's mana after the leap still covers it
    bool LeaderLeapSaves(Player* bot, Position const& to, std::vector<HazardZone> const& zones,
                         std::string* why = nullptr, float manaLine = -1.0f);

    // [lead691] user 2026-11-03: every class's movement skills on the walks the leader orders, used whenever they
    // save walk seconds (never kept for a boss: no boss here needs a run). The leader decides; the member casts the
    // External (LeaderExternalAction). Spell data (Spell.dbc 3.3.5a):
    //   Blink 1953 (SPELL_EFFECT_LEAP 20 yd, GCD 1.5 s, 21 % base mana);
    //   Sprint 2983 / 8696 / 11305 (+50 / 70 / 70 % run speed, 15 s, off the GCD, no energy);
    //   Shadowstep 36554 (talent; to an enemy 25 yd, behind it, off the GCD, 10 energy);
    //   Dash 1850 / 9821 / 33357 (+50 / 60 / 70 %, 15 s, off the GCD; spell_dru_dash: cat form only, and its speed
    //     is recalculated to 0 on leaving it) after Cat Form 768 (GCD 1.5 s, 35 % base mana);
    //   Feral Charge - Cat 49376 (taught by the Feral Charge talent 49377; cat form, enemy 8-25 yd, off the GCD);
    //   Ghost Wolf 2645 (+40 %, 2 s cast less Improved Ghost Wolf 16262 / 16287, GCD 1.5 s, 6 % base mana; a
    //     shapeshift no spell is cast in, so the leader also tells the member to leave it);
    //   Aspect of the Cheetah 5118 (+30 %, the caster) / Aspect of the Pack 13159 (+30 %, the party in 40 yd):
    //     no GCD, 1 s aspect category cooldown, Dazed (15571) on a hit - out of a fight only;
    //   Power Word: Shield 17 ... 48066 (GCD 1.5 s, 23 % base mana, Weakened Soul 6788 15 s) with Body and Soul
    //     64127 / 64129 (its proc 64128 / 65081: +30 / 60 % for 4 s on the shielded member);
    //   Disengage 781 (SPELL_EFFECT_LEAP_BACK: MiscValue / 10 yd/s across, BasePoints / 10 yd/s up -> a leap of
    //     2 x vz x vxy / gravity backwards, off the GCD, 5 % base mana; spell_hun_disengage: in combat only).
    // Not wired: Unholy Presence (Improved Unholy Presence 50391 / 50392 already gives 8 / 15 % in Blood and Frost
    // through 49772: rank 2 is the swap's whole 15 %), Travel Form indoors, Demonic Circle.
    struct LeaderMoveTick
    {
        Player* tank = nullptr;
        std::vector<Player*> members;  // the seats (the tank apart)
        bool fighting = false;
        bool pullOrdered = false;  // the leader issued the pull order
        float fightSecLeft = 0.0f;
        bool bossFight = false;
        Unit* waitBoss = nullptr;  // the boss the next pull waits for (its boss mana line)
        std::map<ObjectGuid, std::vector<HazardZone>>* hazardZones = nullptr;
        std::set<ObjectGuid> const* trailing = nullptr;  // members the group waits for (LEADER_TRAIL_YD)
        std::vector<G3D::Vector3> const* routePath = nullptr;  // the tank's route leg, walked by index
        size_t routePathIndex = 0;
        // [lead905] the route plan's walk legs ahead (stop to stop to the boss): the seconds until each begins and
        // its walk (yd)
        std::vector<std::pair<float, float>> routeLegs;
    };
    struct LeaderMoveTold
    {
        Player* member = nullptr;
        uint32 spell = 0;
        std::string on;
        std::string why;
    };
    // [lead691] one leader tick of the movement skills: each member's best skill for its walk (the most seconds
    // saved over its cost), told as its External; a told one no longer saving withdrawn; a movement form left
    // when it no longer serves. `told`: the new orders to log. Returns the hunters held in a movement aspect
    std::set<ObjectGuid> LeaderTickMoveSkills(LeaderMoveTick const& facts, std::vector<LeaderMoveTold>& told);
    // [lead691] the member's mana line the leader keeps for its fight now (FightManaLine for the seconds left) or
    // its next (the boss gate's MemberBossManaLine for the boss waited for, else OwnManaLine), % of max mana
    float LeaderMoveManaLine(Player* member, bool fighting, float fightSecLeft, bool bossFight, Unit* waitBoss);
    // [lead691] the hunter is in its own Aspect of the Cheetah / Pack
    bool LeaderInMoveAspect(Player* hunter);
    // [lead691] the member's External: `spell` is a movement form it is in and the leader told it on itself by its
    // guid - leave it (Ghost Wolf)
    bool LeaderMoveFormLeave(Player* bot, uint32 spell, ObjectGuid target);
    // [lead691] the member's External: a movement aura of its own already up (nothing to cast)
    bool LeaderMoveSkillUp(Player* bot, uint32 spell, ObjectGuid target);
    // [lead691] the form the member must take before the External's cast (Cat Form for Dash), 0: none needed
    uint32 LeaderMoveSkillForm(Player* bot, uint32 spell);

    // [lead18] the nearest spot around `from` that fits (ranged: at least 8 yd off the target)
    bool LeaderNearestCombatSpot(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& from, bool melee,
                                 std::vector<HazardZone> const& zones,
                                 std::function<bool(Position const&)> const& keeps, Position& out,
                                 std::vector<ObjectGuid> const* fightMobs = nullptr);

    // [lead15] the assigned spot's checks: ground, sight to the fight spot, no idle mob outside the pulled pack
    // reaching it, a walk to it.
    bool LeaderSpotOk(Player* tank, Position const& fight, Position& spot, std::vector<ObjectGuid> const& pack);
}

#endif
