/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WaypointMgr.h"  // [spot35]
#include "SoloSafety.h"
#include "SlowScope.h"  // [perf26]
#include "PerfMonitor.h"  // [guild15]

#include "BuyAction.h"
#include "CellImpl.h"
#include "AttackersValue.h"  // [deaths] NO_ATTACK
#include "AggroReach.h"  // [death470] the ground one mob's aggro covers, as a seam without the world
#include "ChaseModel.h"  // [runpast88] the chase window, as a seam without the world
#include "ManaBudget.h"  // [mana90] the seconds of a fight the mana holds, as a seam without the world
#include "KillRate.h"  // [dpszero1] its own damage in three states - no estimate, deals nothing, deals
#include "MobStand.h"  // [stuck259] whether the mob left the stand its verdict was given at, as a seam
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DpsEstimate.h"  // [dps1] its own damage from the formula
#include "Journal.h"      // [runpast88] Journal::DeathFacts - the death's numbers as values
#include "Item.h"
#include "CreatureGroups.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"  // [deaths3] spawn wander distances
#include "World.h"
#include "EstimatedLifetimeValue.h"
#include "Formulas.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HealthTriggers.h"  // [mana90] ManaRegenPerSec: the in-combat regen, from the one place that has it
#include "LeaderFightChecks.h"
#include "LeaderPlannerShared.h"  // [deaths] PullPlanning::PatrolForecast
#include "NewRpgBaseAction.h"
#include "NewRpgCoop.h"  // [deaths3] a co-op party's walk
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"  // [pullplan] the target's chase
#include "ModelIgnoreFlags.h"  // [deaths] the pull spot's sight of its target
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RouteSearch.h"
#include "RouteTargets.h"  // [nav200] UNREACHABLE_RECHECK_DISTANCE / _HEIGHT, the dungeon side's own rule
#include "LeaderOrders.h"  // [lag370] Leader::Route
#include "SpellInfo.h"
#include "SurvivalKit.h"  // [deaths3] its heals now
#include "QuestStats.h"   // [budget12] the time budget
#include "GameObject.h"  // [death407] a quest object's linked trap
#include "SpellMgr.h"
#include "SmartScriptMgr.h"  // [guild14] creature casts
#include "StringFormat.h"
#include <sstream>
#include "Timer.h"
#include "GameTime.h"  // [death463] the map's respawn times against the game clock

#include <algorithm>
#include <cmath>
#include <mutex>
#include <atomic>  // [party75]
#include <chrono>  // [party75]
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // one attacker's part of an open fight
    struct AttackerTrack
    {
        uint32 entry = 0;
        uint32 firstMs = 0;
        uint32 lastMs = 0;
        uint64 damage = 0;
        float swingSec = 0.0f;
        std::string name;
        uint8 level = 0;
    };

    struct EngageNote
    {
        ObjectGuid target;
        std::string targetName;
        float predicted = 0.0f;
        float health = 0.0f;
        float healthPct = 0.0f;
        float manaPct = 0.0f;
        uint32 mobs = 0;
        G3D::Vector3 fightAt;     // [fight49] where the judge put the fight
        float safeRadius = -1.0f;  // [fight49] the yards round it out of every reach it left out (-1: not set)
        std::vector<ObjectGuid> counted;  // [death392] the mobs its judge counted: no reach "left out"
    };

    // the self heal the bot casts best (most healing per mana), from spell data
    struct HealSpell
    {
        size_t spells = size_t(-1);
        int32 power = -1;
        uint32 id = 0;
        float amount = 0.0f;
        float cost = 0.0f;
        float castSec = 0.0f;
    };

    // [pull9] a mob the last route search refused, and why (logged with a refused walk)
    struct RouteWall
    {
        enum class Why : uint8
        {
            None,
            Elite,
            Con,    // orange / red
            Zone,   // in a remembered danger zone (a death, a refused pull) the bot does not stand in
            Above,  // its linked pack holds two or more above the bot's level
            Loses   // it loses its linked pack one by one even at full
        };
        uint32 entry = 0;
        uint8 level = 0;
        Why why = Why::None;
        uint32 pack = 1;
        float yards = 0.0f;
        // [runpast11] the run past it priced (the leash chases of its pack with the ones the leg takes already) and
        // the bot's health with its heal kit over that chase: a wall because damage >= hp; 0 / 0 when not priced
        float runPastDamage = 0.0f;
        float runPastHp = 0.0f;
        // [dpszero2] the FIGHT judge called this wall's fight lost. `why` keeps the reason the wall was first
        // raised for (an elite stays an elite: ConWalls hands those quests to a party, and that must not change),
        // so the fight's verdict needs its own field - and it is the field the zone rule and its problem row read
        bool fightLost = false;
        // [death471] THE FACT THIS WALL WAS MADE FROM, so that it lives exactly as long as that fact does.
        // The refusal discs themselves are rebuilt by every search, but this list is READ BACK long after the search
        // that wrote it, by things that act on it: ConWalls hands a walled quest to a party, NearestLostFightWall
        // writes the problem row, RouteWalls prints the wall in a death line. b503 Duskwood: Mor'Ladim L30 is a
        // WAYPOINT patrol (creature 5086, wander_distance 0, detection_range 20) and its wall was read back at 103,
        // 107, 115, 117, 128, 132, 160 and 162 yd from the walker; Baridrol's death row carries route_age_ms 24 826,
        // so the stand it was refused at was 25 s old and the patrol had walked on. The replacement is NOT a shorter
        // age: it is the mob itself. `mob` says which creature, and `atX/atY/atRadius` is THE VERY DISC the refusal
        // was computed over - the one that went into `refuse`. A reader asks AggroReach::Covers of it, so the
        // quantity "the ground this mob's aggro covers" keeps exactly one judge (rule 5): the refusal stands while
        // the mob is still on the ground the refusal was made of, and a patrol that has walked off it walls nothing
        uint64 mob = 0;  // the creature's raw guid; 0: the emitter did not say (and then the wall is kept as before)
        float atX = 0.0f;
        float atY = 0.0f;
        float atRadius = 0.0f;
    };

    constexpr float RUN_PAST_ON_LEG = 8.0f;        // [death452] the farRoute tolerance (MoveFarToDirect)
    constexpr size_t RUN_PAST_SEARCHES_KEPT = 8;   // [death452] the searches of one walk's look (a stage, vias, legs)

    struct SoloBotState
    {
        // the open fight
        bool open = false;
        uint32 firstMs = 0;
        uint32 lastMs = 0;
        uint64 dealt = 0;
        uint64 taken = 0;
        uint64 healed = 0;  // its own heals on itself this fight
        // [guild15] the melee phase (the bot within melee reach of the mob in the damage event): what it dealt
        // there and how long it stood there (sampled at every damage event of the fight, either way)
        uint64 dealtMelee = 0;
        uint32 meleeMs = 0;
        uint32 sampleMs = 0;
        bool sampleMelee = false;
        // [guild15] the time it attacked (a victim, or a cast / auto shot under way, at the damage event): b332
        // Colian L12 "melee 38.1 s dealt 0" - an attacker dropped by a sight bug, no victim, no swing.
        // [dps20] this clock bounds the fight length SoloSafety's OWN numbers use (its heal rate, its NO_ATTACK
        // line, the part of a disengage it fought). The dps row's active_s is DpsEstimate's reading of the same
        // damage events and no longer this one: one judge per number, and that number is the measurement's.
        uint32 actMs = 0;
        bool sampleAct = false;
        // [dps20] the white-swing buckets, the lead swing and the "it swam / it looted" facts were here and are
        // DpsEstimate's now (LiveFight, SampleFight): they are read at the same damage events, and a fight in a
        // dungeon or in a five never reaches this state at all. One clock measures them for every fight.
        // [dps1] the formula at the fight's open against its first mob (for the [dps] line only)
        DpsEstimate::Result formula;
        DpsEstimate::BuffContext formulaCtx = DpsEstimate::BuffContext::Solo;
        uint32 formulaEntry = 0;
        uint8 formulaLevel = 0;
        uint32 manaAtOpen = 0;
        std::unordered_map<uint64, AttackerTrack> attackers;
        bool engaged = false;
        EngageNote engage;
        // its fights: their seconds and its own heals on itself (the heal rate; its damage is the formula's, [dps1])
        double fightSecSum = 0.0;
        double healedSum = 0.0;
        uint32 fights = 0;
        // rest and recovery
        bool rest = false;
        bool restAfterDeath = false;  // [death397] the rest after a death outlives a fight met on the way
        bool loggedDeathRest = false;
        std::string restWhy;
        bool recover = false;
        // one log line per target and verdict
        ObjectGuid loggedTarget;
        uint8 loggedKind = 255;
        HealSpell heal;
        // [guild14] (entry, level gap) of the mobs it lost a fight to
        std::set<std::pair<uint32, int32>> lostTo;
        // [guild14] the open fight turned lost: it disengages until the fight ends
        bool fleeing = false;
        // [guild15] when the run / reset began, and what it had dealt by then: its damage is measured over the part
        // it fought (b321 Colian / Farret L11: runs that began at the first hit measured 0 dps on Nightstalkers and
        // Root Threshers, the judge then counted 1.0 dps and refused / ran from every L8 mob, the next run 0 again)
        uint32 fleeMs = 0;
        uint64 dealtAtFlee = 0;
        // [deaths] NO_ATTACK logged this fight
        bool loggedNoAttack = false;
        // [deaths] the last route search round its walk: when, the idle mobs it saw (guid -> refused, else priced),
        // and what the search gave (DeathContext: how a killer met on a walk had been seen)
        uint32 routeMs = 0;
        std::unordered_map<uint64, bool> routeMobs;
        // [surv21] the mobs that search had in range and left out because their fight was not the bot's (RouteCosts
        // skips a mob in combat): they are priced at nothing, and the death line said "unseen" of them - the same
        // word it says of a mob 1340 yd away. b486 Geron L22's two Addled Lepers were the far kind, Bami L24's
        // killer the near one
        std::unordered_set<uint64> routeOthersFight;
        std::vector<RouteWall> routeWalls;  // [pull9]
        std::string runPastLogged;          // [runpast11] the last run-past line (one line per change)
        // [runpast11] the run-past set of the last route search from where the bot stood (mob -> its chase damage) and
        // the committed leg's: its set, its points, the mobs its RUN_PAST lines named
        std::unordered_map<uint64, float> pendingRunPast;
        std::unordered_map<uint64, float> legRunPast;
        // [death452] each route search's run-past set by where it started (a leg is committed from the search it was
        // cut from, wherever that started), its beyond-sight orange / red spawn reaches priced as run pasts, and the
        // health it was priced at; the committed leg's static reaches and health
        struct SearchRunPast
        {
            G3D::Vector3 from;
            std::unordered_map<uint64, float> chases;
            std::vector<G3D::Vector4> statics;
            float hp = 0.0f;
        };
        std::vector<SearchRunPast> searchRunPast;
        std::vector<G3D::Vector4> legStatic;
        float legHp = 0.0f;
        bool runPastHitLogged = false;  // [death384] its held-hit line logged
        bool runPastWalking = false;    // [death384] the run-past executor's last look: it walks the leg on
        // [runpast87] the keep-walking chase it took last: priced damage and seconds, its health and the fight's taken
        // damage then - the death line sets the measured beside the priced (a log for the chase model, no decision)
        float runPastPriced = 0.0f;
        // [runpast88] the HIGH end of the same chase ([death386]: the mean plus two standard deviations of the
        // chasers' swings). THIS is the number SurvivesRunPast was asked, and it was kept nowhere: the death line
        // printed the mean, so the journal held a price no decision had been taken on
        float runPastPricedHigh = 0.0f;
        float runPastSec = 0.0f;
        float runPastHp = 0.0f;
        uint64 runPastTakenAt = 0;
        // [surv20] the mobs that chase in it (the chasers it was priced on and the pack their fight brings): a
        // sample of the chase model is a sample only of the fight those mobs are in
        std::unordered_set<uint64> runPastChasers;
        std::vector<G3D::Vector3> legPoints;
        std::vector<G3D::Vector3> legSearched;  // [runpast15] the searched walk the leg was cut from
        std::unordered_set<uint64> legNamed;
        bool routeComplete = false;
        bool routeThroughRefused = false;
        // [deaths] the engage's pull spot (Verdict::pullSpot) and the reaches its walk goes round
        bool pullSpot = false;
        ObjectGuid pullTarget;
        G3D::Vector3 pull;
        std::vector<G3D::Vector4> pullAvoid;
        // [pullplan] the pack the judge counted there, and the PULL line logged at the fight's open
        uint32 pullMobs = 0;
        bool pullLogged = false;
        // [deaths] the last pull spot search: its target, where the bot stood, what it found (perf: one search per
        // target and spot of the bot, the judge looks every tick)
        ObjectGuid searchTarget;
        G3D::Vector3 searchFrom;
        bool searchFound = false;
        bool searchRouted = false;  // [pullplan] its walk there searched (not a far forecast)
        G3D::Vector3 searchSpot;
        std::vector<G3D::Vector4> searchAvoid;
        // [surv10] the fight no judge opened ("no judged pull (attacked)"): 28 of the 32 open-world deaths over b477,
        // b478, b480 and b482 closed such a fight, and 4241 of the 4245 fights a judge did open were won - so the
        // deaths carried no prediction to compare a fact against at all. What the first mob was and where the bot
        // stood against that mob's own aggro disc, held until the fight's first projection prices it (the line is the
        // judge's inputs for a fight it was never asked about; no decision reads it)
        bool unjudgedPending = false;
        std::string unjudgedWhy;
        // [surv10] the mobs the core will bring to a fight no judge opened, as the pull judge's own model counts them
        // (PackAt: the engage call, the formation, the periodic CallAssistance, a flee for help, and an idle mob whose
        // reach covers the fight). Built once per fight, outside the lock, and resolved per tick from these guids -
        // the projection counted only `bot->getAttackers()` and so priced 1 attacker in 17 of b484's 19 opens.
        std::vector<uint64> joiners;
        bool joinersBuilt = false;
    };

    struct EntryDamage
    {
        double damage = 0.0;
        double sec = 0.0;
    };

    std::mutex lock;
    std::unordered_map<uint32, SoloBotState> states;
    std::unordered_map<uint32, EntryDamage> entryDamage;  // creature entry -> what it dealt to solo bots, per second

    // [nav200] bot -> target -> (where the bot stood when the route found no walk, where the target stood then).
    // The open world's unreachableFrom: the dungeon leader keeps the same memo per group (GroupRouteState), a solo
    // bot keeps its own. No clock lives here - an entry ends on a fact, never on elapsed time.
    std::mutex unreachableLock;
    std::unordered_map<uint32, std::unordered_map<ObjectGuid, std::pair<Position, Position>>> unreachableFrom;

    bool PlaysOwnPlan(Player* member)
    {
        PlayerbotAI* memberAI = member ? GET_PLAYERBOT_AI(member) : nullptr;
        return memberAI && !memberAI->HasGameClientMaster() &&
               memberAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) &&
               !NewRpgCoop::InParty(member->GetGUID().GetCounter());
    }

    bool MemberSupports(Player* bot, Player* member)
    {
        if (!bot || !member)
            return false;
        // [deaths23] a co-op / help party fights together; one of the two in a party the other is not in does not
        bool const botParty = NewRpgCoop::InParty(bot->GetGUID().GetCounter());
        if (botParty != NewRpgCoop::InParty(member->GetGUID().GetCounter()))
            return false;
        // out of a party: two bots each on its own plan walk apart; a follower and its leader, a human, do not
        return botParty || !PlaysOwnPlan(member) || !PlaysOwnPlan(bot);
    }

    bool SoloPlayerBot(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
            return false;
        if (bot->GetMap() && bot->GetMap()->Instanceable())
            return false;
        // [deaths3] b341: 10 of 12 deaths were grouped bots questing away from every member (the user pack's fives
        // 120-1000 yd apart, offline members): no judge, no walk round the camps, no survival kit. A bot fights alone
        // when no living member stands within the core's group reward distance (Player::IsAtGroupRewardDistance)
        // [deaths23] ... that fights with it: a group member playing its own New RPG plan (no co-op / help party
        // holds it, the wait13 fact) walks its own route and is no support. b372: Laaral / Dekorhan and Thokhen /
        // Ellosera, restored groups each on its own plan within 74 yd of each other in the Cryo-Core camp (20 Sunhawk
        // Reclaimers L16-17): neither the solo judge nor the party judge judged a pull, 8 deaths "no judged pull"
        Group* group = bot->GetGroup();
        if (!group || group->GetMembersCount() <= 1)
            return true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
                !member->IsAtGroupRewardDistance(bot))
                continue;
            if (MemberSupports(bot, member))
                return false;
            // [deaths23] one line per bot and member (a log line, no decision)
            static std::mutex ownPlanLogLock;
            static std::unordered_set<uint64> ownPlanLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(ownPlanLogLock);
                fresh = ownPlanLogged.insert(uint64(bot->GetGUID().GetCounter()) << 32 |
                                             member->GetGUID().GetCounter())
                            .second;
            }
            if (fresh)
                LOG_INFO("playerbots", "[deaths23] {} L{} judged as a solo bot: its group member {} ({:.0f} yd) plays "
                         "its own plan, no co-op / help party holds both",
                         bot->GetName(), bot->GetLevel(), member->GetName(), bot->GetExactDist(member));
        }
        return true;
    }

    float SwingSec(Unit* unit, WeaponAttackType type = BASE_ATTACK)
    {
        return std::max(float(unit->GetAttackTime(type)) / float(IN_MILLISECONDS), 0.1f);
    }

    // a class spell, not a profession recipe (Playerbots.cpp drops the formula cache on a class spell learned)
    bool ProfessionSpell(uint32 spellId)
    {
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(itr->second->SkillLine))
                if (line->categoryId == SKILL_CATEGORY_PROFESSION || line->categoryId == SKILL_CATEGORY_SECONDARY)
                    return true;
        return false;
    }

    G3D::Vector4 AggroDisc(Creature* mob, Player* bot);  // [deaths138] below

    // a closed fight goes into the bot's and the entries' measurements (caller holds the lock)
    void CloseFight(Player* bot, SoloBotState& state, bool died)
    {
        if (!state.open)
            return;
        state.open = false;
        // [runpast87] the chase it kept walking under, priced against what it took (the chase model's calibration
        // data; no decision reads it)
        if (state.runPastPriced > 0.0f)
        {
            // [surv20] ... of the fight its own chasers are in, and of no other. b486 Bami L24 committed a run past
            // one Leper Gnome L10 ("priced 3 dmg (10 at the high end) over 12 s against hp 1137 - a margin of 1127",
            // honest for a L10) and was then killed by an Addled Leper L24 that was in Geron's fight (the killer's
            // threat list "Geron 3223", Bami on neither it nor its own attacker list). The sample read "priced 3 dmg:
            // took 0 (died)" and says the chase model is out by orders of magnitude; it is the Gnome's chase beside
            // another mob's fight. The chase is sampled only where one of its mobs hit the bot
            bool ofItsChase = false;
            for (auto const& [guid, track] : state.attackers)
                ofItsChase = ofItsChase || state.runPastChasers.count(guid) > 0;
            LOG_INFO("playerbots", "[runpast87] {} L{} run-past chase priced {:.0f} dmg over {:.0f} s against hp {:.0f}: "
                     "took {} ({}{})",
                     bot->GetName(), bot->GetLevel(), state.runPastPriced, state.runPastSec, state.runPastHp,
                     state.taken - std::min(state.taken, state.runPastTakenAt), died ? "died" : "lived",
                     ofItsChase ? "" : "; [surv20] no sample of the chase: none of its own chasers hit the bot");
            state.runPastPriced = 0.0f;
            state.runPastPricedHigh = 0.0f;  // [runpast88]
            state.runPastChasers.clear();
        }
        float const seconds =
            float(getMSTimeDiff(state.firstMs, state.lastMs)) / float(IN_MILLISECONDS) + SwingSec(bot);
        // [guild15] its damage over the part it fought: up to the run / reset; a run before two of its swings
        // measured nothing of its damage (it never fought), so the fight adds no damage measurement at all
        float fought = seconds;
        uint64 foughtDealt = state.dealt;
        if (state.fleeing && state.fleeMs)
        {
            fought = float(getMSTimeDiff(state.firstMs, state.fleeMs)) / float(IN_MILLISECONDS);
            foughtDealt = state.dealtAtFlee;
        }
        bool measured = !state.fleeing || fought >= 2.0f * SwingSec(bot);
        if (state.fleeing)
            fought += SwingSec(bot);
        // [guild15] the time it never attacked measures nothing of its damage (a sight bug, a swim, a chest opened
        // under a hit); [dps1] the reasons the fight is no check of the formula
        // [dps20] "never attacked" is not written here any more: the measurement reads it off the damage it
        // actually counted, for a fight on any map, and one reason has one author. This still drops the fight out
        // of SoloSafety's own heal-rate sum, which is its number and nobody else's.
        std::string invalid;
        {
            float const acted = float(state.actMs) / float(IN_MILLISECONDS) + SwingSec(bot);
            if (!state.actMs && !foughtDealt)
                measured = false;
            else if (acted < fought)
                fought = acted;
        }
        // [dps40] the part it fought is a part of the fight: a bot that ran after its last hit landed took `fleeMs`
        // past the `lastMs` that ends `seconds`, and logged an active_s longer than the fight_s holding it (b479
        // Bierya L21 feral 8.6 s of a 4.0 s fight). One judge of the fight's length, and this is its part.
        fought = std::min(fought, seconds);
        if (invalid.empty() && !measured)
            invalid = "ran before two swings";
        // [dps20] "swimming" and "looting" are DpsEstimate's own reasons now, read off the same damage events
        if (invalid.empty() && !state.formulaEntry)
            invalid = "no target at the open";
        // its heal rate over its fights (no decision reads its damage history: [dps1] the formula is its damage)
        if (measured)
            state.fightSecSum += fought;
        state.healedSum += double(state.healed);
        ++state.fights;
        for (auto const& [guid, track] : state.attackers)
        {
            if (!track.entry || !track.damage)
                continue;
            EntryDamage& entry = entryDamage[track.entry];
            double const trackSec =
                double(getMSTimeDiff(track.firstMs, track.lastMs)) / IN_MILLISECONDS + track.swingSec;
            entry.damage += double(track.damage);
            entry.sec += trackSec;
            // [dps1] what every solo bot measured of this entry: a log for the mob damage model, never read by a
            // decision (user 2026-11-08)
            LOG_INFO("playerbots", "[mobdps] {} L{} hit {} for {:.1f} dps this fight, {:.1f} dps over all solo bots",
                     track.name, track.level, bot->GetName(), float(double(track.damage) / trackSec),
                     float(entry.damage / entry.sec));
        }
        // [dps1] the formula at the open over this fight's length against what it dealt while it attacked
        {
            DpsEstimate::FightSample sample;
            sample.targetEntry = state.formulaEntry;
            sample.targetLevel = state.formulaLevel;
            sample.mobs = uint32(state.attackers.size());
            // [dps20] the formula is no longer priced here. It was `FormulaOver(state.formula, fought)` - the
            // formula over SoloSafety's clock - while a fight in a dungeon was priced over the measurement's own
            // window, so one column held two numbers. RecordFight prices it, once, over the window it measured.
            sample.cycleSec = state.formula.cycleSec;
            sample.dealt = foughtDealt;
            sample.fightSec = seconds;
            sample.taken = state.taken;
            sample.outcome = died ? "died" : state.fleeing ? "fled" : "won";
            sample.invalidWhy = invalid;
            sample.ctx = state.formulaCtx;
            // [dps20] the window, the time it attacked and the white-swing buckets are no longer passed: the clock
            // that sees the damage events reads them for this fight exactly as it reads a five's in a dungeon
            DpsEstimate::RecordFight(bot, sample);
        }
        QuestStats::NoteCombat(bot, uint32(std::max(seconds, 0.0f) * float(IN_MILLISECONDS)), false);  // [budget12]
        std::string came;
        for (auto const& [guid, track] : state.attackers)
            came += (came.empty() ? "" : ", ") + track.name + " L" + std::to_string(track.level);
        if (state.engaged)
            LOG_INFO("playerbots",
                     "[solo] {} L{} fight {}: {} judged {} mobs, {:.0f} dmg expected at hp {:.0f}; came {} ({}), took "
                     "{} in {:.1f} s, dealt {} ({:.1f} dps); melee {:.1f} s dealt {} ({:.1f} dps), range {:.1f} s dealt "
                     "{} ({:.1f} dps)",
                     bot->GetName(), bot->GetLevel(), died ? "LOST" : state.fleeing ? "DISENGAGED" : "won",
                     state.engage.targetName,
                     state.engage.mobs, state.engage.predicted, state.engage.health, state.attackers.size(), came,
                     state.taken, seconds, state.dealt, float(state.dealt) / seconds,
                     float(state.meleeMs) / 1000.0f, state.dealtMelee,
                     state.meleeMs ? float(state.dealtMelee) * 1000.0f / float(state.meleeMs) : 0.0f,
                     std::max(0.0f, seconds - float(state.meleeMs) / 1000.0f), state.dealt - state.dealtMelee,
                     seconds * 1000.0f > float(state.meleeMs) + 1.0f
                         ? float(state.dealt - state.dealtMelee) / (seconds - float(state.meleeMs) / 1000.0f)
                         : 0.0f);
        // [guild14] what it lost (a death, a disengage) at the fight's level gap (its top attacker's)
        {
            int32 gap = std::numeric_limits<int32>::min();
            for (auto const& [guid, track] : state.attackers)
                gap = std::max(gap, int32(track.level) - int32(bot->GetLevel()));
            if (died || state.fleeing)
                for (auto const& [guid, track] : state.attackers)
                    if (track.entry && track.damage && int32(track.level) - int32(bot->GetLevel()) == gap)
                    {
                        state.lostTo.insert({track.entry, int32(track.level) - int32(bot->GetLevel())});
                        LOG_INFO("playerbots", "[solo] {} L{} lost to {} L{} ({}): not pulled again at a gap of {} or "
                                 "more", bot->GetName(), bot->GetLevel(), track.name, track.level,
                                 died ? "died" : "disengaged", int32(track.level) - int32(bot->GetLevel()));
                    }
            // [deaths138] the reach of every mob that hit it in a fight it lost is a danger zone, as a refused pull's
            // (NewRpgBaseAction::RememberDanger): kept away from until it outlevels the mob or its gear / spells /
            // talents change. The lost-to record only stopped a pull of that entry - the walk back to its objective,
            // its rest spot and its sweep still crossed the same mob's reach. b428: 18 of 119 open-world deaths came
            // in the fight right after a lost one; Orton L16 fled the Dark Iron Sappers at Stonewrought Dam, rested
            // to full 31 yd off, walked back into the same Sapper's reach (idle, aggro 19) and died; Rumanatol L18 the
            // same with the Fouled Water Spirit at The Foul Pool. A patrol's reach moves along its path: no disc for it
            // [deaths143] user 2026-11-11: a loss that came from mana (it closed the fight unable to cast any mana
            // spell it knows) against a mob not orange / red for it is a zone its rest to full health and mana clears
            // (NewRpgBaseAction::RememberDanger restClears); a loss with mana left, or to an orange / red mob, keeps
            // the rule above
            bool outOfMana = false;
            if ((died || state.fleeing) && !bot->GetMap()->Instanceable() && bot->GetMaxPower(POWER_MANA) > 0)
            {
                uint32 const mana = bot->GetPower(POWER_MANA);
                uint32 cheapest = std::numeric_limits<uint32>::max();
                for (auto const& [spellId, known] : bot->GetSpellMap())
                {
                    if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                        continue;
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                    if (!info || info->IsPassive() || info->PowerType != POWER_MANA)
                        continue;
                    bool acts = false;  // a spell that fights or heals: damage, a heal, an aura on a foe or itself
                    for (SpellEffectInfo const& effect : info->GetEffects())
                        acts = acts || effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                               effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_WEAPON_DAMAGE ||
                               effect.Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG ||
                               effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                               effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL;
                    if (!acts)
                        continue;
                    int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
                    if (cost > 0)
                        cheapest = std::min(cheapest, uint32(cost));
                }
                outOfMana = cheapest != std::numeric_limits<uint32>::max() && mana < cheapest;
                // [death465] ... only a loss of the fight its judge counted: more attackers than judged (or none
                // judged) ran its mana dry on mobs the estimate left out, and the rest does not change them. b470
                // Rollothan L17 died twice at the Longshore murloc camp (5 attackers, "no judged pull"): the first
                // zone was "out of mana", cleared by its rest, and its grind walk went back in
                if (outOfMana && (!state.engaged || state.attackers.size() > state.engage.mobs))
                {
                    LOG_INFO("playerbots", "[death465] {} L{} lost out of mana to {} attackers ({} judged): the zone "
                             "stays past its rest", bot->GetName(), bot->GetLevel(), state.attackers.size(),
                             state.engaged ? state.engage.mobs : 0);
                    outOfMana = false;
                }
            }
            if ((died || state.fleeing) && !bot->GetMap()->Instanceable())
                for (auto const& [guid, track] : state.attackers)
                {
                    if (!track.entry || !track.damage)
                        continue;
                    Creature* mob = ObjectAccessor::GetCreature(*bot, ObjectGuid(guid));
                    if (!mob || !mob->IsAlive() || !mob->IsHostileTo(bot) ||
                        mob->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE || mob->GetCharmerOrOwnerGUID())
                        continue;
                    G3D::Vector4 disc = AggroDisc(mob, bot);
                    Position const& home = mob->GetHomePosition();  // where it stands again once it evades
                    disc.x = home.GetPositionX();
                    disc.y = home.GetPositionY();
                    bool const restClears =
                        outOfMana && Acore::XP::GetColorCode(bot->GetLevel(), track.level) > XP_ORANGE;
                    NewRpgBaseAction::RememberDanger(bot, disc.x, disc.y, disc.w, track.entry, track.level,
                                                     restClears);
                    LOG_INFO("playerbots",
                             "[deaths138] {} L{} lost a fight to {} L{}: its reach ({:.0f},{:.0f}) {:.0f} yd is a "
                             "danger zone until {}",
                             bot->GetName(), bot->GetLevel(), track.name, track.level, disc.x, disc.y, disc.w,
                             restClears ? std::string("its rest to full health and mana ([deaths143] out of mana, "
                                                      "the mob not orange / red)")
                                        : Acore::StringFormat("L{} or its gear / spells change",
                                                              std::max<uint32>(track.level, bot->GetLevel()) + 1));
                }
        }
        // [guild80] a run / reset closes with a rest to full (WantsRest drops a rest asked for in combat: b319
        // Ravelandy L8 went questing at hp 53 after a run and died to the same Prowler)
        if (state.fleeing && !died)
        {
            state.rest = true;
            state.restWhy = "after a survival run";
        }
        state.fleeing = false;
        state.fleeMs = 0;
        state.dealtAtFlee = 0;
        state.attackers.clear();
        state.engaged = false;
        state.dealt = 0;
        state.taken = 0;
        state.healed = 0;
        state.dealtMelee = 0;
        state.meleeMs = 0;
        state.sampleMs = 0;
        state.sampleMelee = false;
        state.actMs = 0;
        state.sampleAct = false;
        state.joiners.clear();  // [surv10] the next fight grows its own pack
        state.joinersBuilt = false;
        state.formula = DpsEstimate::Result();
        state.formulaEntry = 0;
        state.formulaLevel = 0;
        state.loggedNoAttack = false;
    }

    // [guild15] the melee-phase clock: the time since the last sample counts as melee when the bot stood in reach then
    // [dps20] the white-swing gate's buckets were read here too and are DpsEstimate::SampleFight's now, at the same
    // damage events: a fight in a dungeon or in a five reaches this function for no bot, and the buckets are what
    // say where a melee bot's missing swings went.
    void SampleMelee(Player* bot, Unit* mob, SoloBotState& state, uint32 now)
    {
        if (state.sampleMs && state.sampleMelee)
            state.meleeMs += getMSTimeDiff(state.sampleMs, now);
        if (state.sampleMs && state.sampleAct)
            state.actMs += getMSTimeDiff(state.sampleMs, now);  // [guild15]
        // [dps70] never 0: `sampleMs` 0 is what marks the fight's first sample. One tick of 1 ms is the whole cost.
        state.sampleMs = now ? now : 1;
        state.sampleMelee = bot->IsWithinMeleeRange(mob);
        state.sampleAct = bot->GetVictim() || bot->IsNonMeleeSpellCast(false);
    }

    void CloseIfOver(Player* bot, SoloBotState& state)
    {
        if (state.open && !bot->IsInCombat() && bot->IsAlive())
            CloseFight(bot, state, false);
    }

    void OpenIfNeeded(Player* bot, SoloBotState& state, uint32 now, Unit* mob)
    {
        if (state.open)
            return;
        state.open = true;
        state.firstMs = now;
        state.lastMs = now;
        state.manaAtOpen = bot->GetPower(POWER_MANA);
        // [dps1] the formula against the fight's first mob (a cached profile: no cost unless its power changed)
        state.formulaCtx = DpsEstimate::ContextOf(bot);
        state.formula = DpsEstimate::Estimate(bot, mob, state.formulaCtx);
        state.formulaEntry = mob ? mob->GetEntry() : 0;
        state.formulaLevel = mob ? mob->GetLevel() : 0;
        // [death398] an engage whose target is not in this fight (its pull was dropped, never made): the fight is not
        // that engage's. b453 Keenonk L22 died to 5 Redridge gnolls "pulled Blackrock Grunt judged 2 mobs", its fight
        // spot 650 yd off - the deaths139 lines and the LOST line read the stale engage's numbers
        if (state.engaged && mob && state.engage.target != mob->GetGUID())
        {
            Creature* engaged = ObjectAccessor::GetCreature(*bot, state.engage.target);
            if (!engaged || !engaged->IsAlive() || engaged->GetVictim() != bot)
            {
                LOG_INFO("playerbots", "[death398] {} L{} fight on {} L{} opens apart from its stale engage of {}",
                         bot->GetName(), bot->GetLevel(), mob->GetName(), mob->GetLevel(), state.engage.targetName);
                state.engaged = false;
            }
        }
        // [deaths12] a fight the judge did not open (b350 Geron L10 at Frostmane Hold, Lyleria L9 at Ban'ethil
        // Hollow: "no judged pull (attacked)", nothing in the log says what came): the first mob, how it moves
        // (wanderer: its home and area; patrol; idle), where it stood against its aggro reach, the bot's state
        if (!state.engaged && mob)
            if (Creature* c = mob->ToCreature())
            {
                PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
                char const* const moves = c->GetDefaultMovementType() == RANDOM_MOTION_TYPE     ? "wanderer"
                                          : c->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE ? "patrol"
                                                                                                 : "idle";
                LOG_INFO("playerbots",
                         "[solo] {} L{} UNJUDGED fight: {} L{} ({}, home {:.0f} yd off, wander {:.0f}, aggro {:.0f}) "
                         "at {:.0f} yd; the bot {} ({}), status {}, hp {:.0f} %",
                         bot->GetName(), bot->GetLevel(), c->GetName(), c->GetLevel(), moves,
                         c->GetHomePosition().GetExactDist2d(bot), c->GetWanderDistance(), c->GetAggroRange(bot),
                         bot->GetExactDist2d(c), bot->isMoving() ? "moving" : "standing",
                         bot->GetVictim() ? "attacking " + bot->GetVictim()->GetName() : std::string("no target"),
                         ai ? ai->rpgInfo.ToString() : std::string("-"), bot->GetHealthPct());
                // [surv10] the same mob's aggro disc as the bot's own avoidance builds it (AggroDisc: a wanderer's
                // disc sits on its home and carries its wander distance, every other mob's on where it stands), and
                // where the bot stood in it. 55 of the 57 unjudged opens over b477-b482 opened with the bot already
                // inside that disc, so the geometry is not the unknown: the inputs the line below adds are the
                // fight's own numbers, which nothing priced
                G3D::Vector4 const disc = AggroDisc(c, bot);
                float const into = std::hypot(disc.x - bot->GetPositionX(), disc.y - bot->GetPositionY());
                // [surv10] rpgInfo.ToString() is several lines (questId, objectiveIdx, poiPos, lastReachPOI), and
                // this record carries the fight's numbers AFTER it: b485 wrote 203 of its 213 price lines as five
                // physical lines each, with the projection - the whole point of the record - orphaned on the last of
                // them, where no line-based reader finds it. One record is one line, so the status is flattened.
                std::string status = ai ? ai->rpgInfo.ToString() : std::string("-");
                for (char& ch : status)
                    if (ch == '\n' || ch == '\r')
                        ch = ' ';
                state.unjudgedPending = true;
                state.unjudgedWhy = Acore::StringFormat(
                    "{} L{} ({}) met at {:.0f} yd, its aggro disc ({:.0f},{:.0f}) r {:.0f} and the bot {:.0f} yd from "
                    "its centre ({}), the bot {} ({}), hp {:.0f} %, status {}",
                    c->GetName(), c->GetLevel(), moves, bot->GetExactDist2d(c), disc.x, disc.y, disc.w, into,
                    into <= disc.w ? "inside it" : "outside it", bot->isMoving() ? "moving" : "standing",
                    bot->GetVictim() ? "attacking " + bot->GetVictim()->GetName() : std::string("no target"),
                    bot->GetHealthPct(), status);
            }
        // [pullplan] the pull it made: where it stood when the fight opened (its pull spot, else where it was)
        if (state.pullSpot && !state.pullLogged && mob && mob->GetGUID() == state.pullTarget)
        {
            state.pullLogged = true;
            LOG_INFO("playerbots",
                     "[solo] PULL {} target {} from ({:.0f},{:.0f}) brings {} ({:.1f} yd off its pull spot)",
                     bot->GetName(), mob->GetEntry(), bot->GetPositionX(), bot->GetPositionY(), state.pullMobs,
                     bot->GetExactDist2d(state.pull.x, state.pull.y));
        }
    }

    // the bot's self heal from its known spells (direct heal effects; spell power counted)
    HealSpell const& BestHeal(Player* bot, SoloBotState& state)
    {
        int32 const power = bot->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_ALL);
        if (state.heal.spells == bot->GetSpellMap().size() && state.heal.power == power)
            return state.heal;
        HealSpell best;
        best.spells = bot->GetSpellMap().size();
        best.power = power;
        float bestPerMana = 0.0f;
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || !info->IsPositive() || info->PowerType != POWER_MANA)
                continue;
            float amount = 0.0f;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_HEAL)
                    amount += float(bot->SpellHealingBonusDone(bot, info, uint32(std::max(effect.CalcValue(bot), 0)),
                                                               HEAL, effect.EffectIndex));
            float const cost = float(info->CalcPowerCost(bot, info->GetSchoolMask()));
            if (amount <= 0.0f || cost <= 0.0f)
                continue;
            if (amount / cost > bestPerMana)
            {
                bestPerMana = amount / cost;
                best.id = spellId;
                best.amount = amount;
                best.cost = cost;
                best.castSec = std::max(float(info->CalcCastTime(bot)), float(info->StartRecoveryTime)) /
                               float(IN_MILLISECONDS);
            }
        }
        state.heal = best;
        return state.heal;
    }

    // [dps1] its damage per second from the formula over its state now (user 2026-11-08: no fight history); the
    // target's level, armor and avoidance when known, else a mob of its level
    DpsEstimate::Result OwnDamage(Player* bot, Unit* target, float fightSec = 0.0f)
    {
        return DpsEstimate::Estimate(bot, target, DpsEstimate::ContextOf(bot), fightSec);
    }

    // [death382] an effect's points as a creature casts it, at the mean of the core's roll: SpellEffectInfo::CalcValue
    // adds irand(1, DieSides) each call, and the casts' caches kept that one roll per entry for the server's life
    // (Redridge Mystic's Lightning Bolt 74-98, Deepmoss Venomspitter's Toxic Spit 38-62). The midrange of a few
    // samples of the core's own value (level scaling and all): the roll is uniform, its midrange centred
    float MeanCalcValue(SpellInfo const* info, SpellEffectInfo const& effect, Creature* creature,
                        float* variance = nullptr)
    {
        if (variance)
            *variance = 0.0f;
        if (effect.DieSides <= 1 && effect.DieSides >= -1)
            return float(effect.CalcValue(creature));
        constexpr int ROLL_SAMPLES = 16;  // a cached look per entry: the midrange's spread is the range over 17
        int32 low = std::numeric_limits<int32>::max();
        int32 high = std::numeric_limits<int32>::min();
        for (int sample = 0; sample < ROLL_SAMPLES; ++sample)
        {
            int32 const value = effect.CalcValue(creature);
            low = std::min(low, value);
            high = std::max(high, value);
        }
        static std::mutex rollLogLock;  // a log cadence per spell and creature kind, no decision reads it
        static std::unordered_set<uint64> rollLogged;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> guard(rollLogLock);
            fresh = rollLogged.insert(uint64(info->Id) * 1000003ull + creature->GetEntry()).second;
        }
        if (fresh)
            LOG_INFO("playerbots", "[death382] {} L{} spell {} effect {}: priced at {:.1f}, the mean of its roll {}-{}",
                     creature->GetName(), creature->GetLevel(), info->Id, uint32(effect.EffectIndex),
                     (float(low) + float(high)) / 2.0f, low, high);
        if (variance)  // [death390] of a discrete uniform roll low..high
        {
            float const span = float(high - low) + 1.0f;
            *variance = (span * span - 1.0f) / 12.0f;
        }
        return (float(low) + float(high)) / 2.0f;
    }

    // [guild14] a creature's spell damage per second from its SmartAI: every in-combat repeating cast (SMART_EVENT_
    // UPDATE_IC + SMART_ACTION_CAST) of a damaging spell - its direct school damage and its periodic damage over the
    // aura's duration - once per repeat interval (the mean of repeatMin / repeatMax), at least its cast time and the
    // global cooldown. b303: casters were counted by their current cast only (Kobold Geomancers, Defias Rogue Wizards,
    // Gnarlpine and Bristlelimb Shamans killed bots judged to win)
    // [deaths] a hostile spell's damage on its target as a creature casts it: its direct school damage and its periodic
    // damage over the aura's duration (0: none, a positive spell)
    float CastDamage(SpellInfo const* info, Creature* creature)
    {
        if (!info)
            return 0.0f;
        // [death388] a self buff that pulses damage round its caster (SPELL_AURA_PERIODIC_TRIGGER_SPELL of a hostile
        // spell: a Blackrock Summoner's Fire Shield II triggers 3053 every 3 s) - its pulses over the aura's duration
        float pulses = 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL &&
                effect.TriggerSpell && effect.TriggerSpell != info->Id && effect.Amplitude > 0)
                if (SpellInfo const* pulse = sSpellMgr->GetSpellInfo(effect.TriggerSpell);
                    pulse && !pulse->IsPositive())
                    pulses += CastDamage(pulse, creature) *
                              float(std::max(info->GetMaxDuration(), 0) / effect.Amplitude);
        if (info->IsPositive())
            return pulses;
        float damage = pulses;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                damage += std::max(MeanCalcValue(info, effect, creature), 0.0f);
            else if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE &&
                     effect.Amplitude > 0)
                damage += std::max(MeanCalcValue(info, effect, creature), 0.0f) *
                          float(std::max(info->GetMaxDuration(), 0) / effect.Amplitude);
        }
        return damage;
    }

    // the shortest time between two casts of the spell: its cast time, the global cooldown
    float CastEverySec(SpellInfo const* info, Creature* creature)
    {
        float const castSec = float(info->CalcCastTime(creature)) / float(IN_MILLISECONDS);
        float const gcdSec = float(std::max<uint32>(info->StartRecoveryTime, 1500)) / float(IN_MILLISECONDS);
        return std::max(castSec, gcdSec);
    }

    float SpellDpsOf(Creature* creature)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, float> cache;  // entry -> dps at the creature's level
        uint32 const entry = creature->GetEntry();
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
                return found->second;
        }
        float dps = 0.0f;
        if (creature->GetAIName() == "SmartAI")
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.event.type != SMART_EVENT_UPDATE_IC || holder.action.type != SMART_ACTION_CAST)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
                float const damage = CastDamage(info, creature);
                if (damage <= 0.0f)
                    continue;
                float const repeatSec =
                    float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) / 2.0f /
                    float(IN_MILLISECONDS);
                // a cast with no repeat goes once a fight: counted over its cast and the cooldown only when it repeats
                if (repeatSec <= 0.0f)
                    continue;
                dps += damage / std::max(repeatSec, CastEverySec(info, creature));
            }
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[entry] = dps;
        return dps;
    }

    // [death390] the spread of a creature's SmartAI casts per second (the casts SpellDpsOf counts): each direct school
    // damage roll's variance (MeanCalcValue's uniform roll), once per its repeat
    float SpellVarianceOf(Creature* creature)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, float> cache;  // entry -> variance per second
        uint32 const entry = creature->GetEntry();
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
                return found->second;
        }
        float perSec = 0.0f;
        if (creature->GetAIName() == "SmartAI")
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.event.type != SMART_EVENT_UPDATE_IC || holder.action.type != SMART_ACTION_CAST)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
                if (!info || info->IsPositive())
                    continue;
                float const repeatSec =
                    float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) / 2.0f /
                    float(IN_MILLISECONDS);
                if (repeatSec <= 0.0f)
                    continue;
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                    {
                        float variance = 0.0f;
                        MeanCalcValue(info, effect, creature, &variance);
                        perSec += variance / std::max(repeatSec, CastEverySec(info, creature));
                    }
            }
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[entry] = perSec;
        return perSec;
    }

    // [deaths] the casts of a creature's SmartAI that reach a bot running from it (SoloSafety::ChaseSpellDamage)
    struct ChaseCast
    {
        float damage = 0.0f;
        float everySec = 0.0f;  // 0: once
    };
    std::vector<ChaseCast> ChaseCastsOf(Creature* creature)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, std::vector<ChaseCast>> cache;  // entry -> its casts at its level
        uint32 const entry = creature->GetEntry();
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
                return found->second;
        }
        std::vector<ChaseCast> casts;
        if (creature->GetAIName() == "SmartAI")
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.action.type != SMART_ACTION_CAST)
                    continue;
                // in combat, or at range from its victim (a minimum range: the cast waits for the bot to be away)
                bool const inCombat = holder.event.type == SMART_EVENT_UPDATE_IC;
                bool const atRange = holder.event.type == SMART_EVENT_RANGE && holder.event.minMaxRepeat.rangeMin > 0;
                if (!inCombat && !atRange)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
                ChaseCast cast;
                cast.damage = CastDamage(info, creature);
                if (cast.damage <= 0.0f)
                    continue;
                float const repeatSec =
                    float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) / 2.0f /
                    float(IN_MILLISECONDS);
                if (!(holder.event.event_flags & SMART_EVENT_FLAG_NOT_REPEATABLE) && repeatSec > 0.0f)
                    cast.everySec = std::max(repeatSec, CastEverySec(info, creature));
                casts.push_back(cast);
            }
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[entry] = casts;
        return casts;
    }

    // [death444] a creature's SmartAI interrupt of a casting victim: a cast of a spell with SPELL_EFFECT_INTERRUPT_CAST
    // on SMART_EVENT_VICTIM_CASTING (repeat min / max) or SMART_EVENT_AREA_CASTING (first min / max, repeat min / max):
    // the means of its rolls, a not repeatable event once (b462 Defias Pathstalker: Shield Bash 1.5-13.6 s, then 25-39 s)
    std::vector<SurvivalKit::Interrupter> InterruptsOf(Creature* creature)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, std::vector<SurvivalKit::Interrupter>> cache;
        uint32 const entry = creature->GetEntry();
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
                return found->second;
        }
        std::vector<SurvivalKit::Interrupter> out;
        if (creature->GetAIName() == "SmartAI")
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.action.type != SMART_ACTION_CAST || (holder.event.type != SMART_EVENT_VICTIM_CASTING &&
                                                                holder.event.type != SMART_EVENT_AREA_CASTING))
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
                if (!info || !info->HasEffect(SPELL_EFFECT_INTERRUPT_CAST))
                    continue;
                SurvivalKit::Interrupter in;
                float repeatMs = 0.0f;
                if (holder.event.type == SMART_EVENT_VICTIM_CASTING)
                    repeatMs = float(holder.event.targetCasting.repeatMin + holder.event.targetCasting.repeatMax) / 2.0f;
                else
                {
                    in.firstSec = float(holder.event.minMaxRepeat.min + holder.event.minMaxRepeat.max) / 2.0f /
                                  float(IN_MILLISECONDS);
                    repeatMs = float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) / 2.0f;
                }
                if (!(holder.event.event_flags & SMART_EVENT_FLAG_NOT_REPEATABLE))
                    in.repeatSec = std::max(repeatMs / float(IN_MILLISECONDS),
                                            float(info->RecoveryTime) / float(IN_MILLISECONDS));
                out.push_back(in);
            }
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[entry] = out;
        return out;
    }

    // [deaths] b338 Thokhen L9: a Bristlelimb Windcaller judged at its health healed itself (Rejuvenation at 0-50 %)
    // and outlasted the bot's potion. A creature's SmartAI heals on itself (SMART_TARGET_SELF casts of a healing
    // spell: its direct heal and its periodic heal over the aura's duration), in combat (SMART_EVENT_UPDATE_IC) or in
    // a health band (SMART_EVENT_HEALTH_PCT); a repeating one once per repeat (at least its cast time and the global
    // cooldown)
    struct SelfHeal
    {
        float amount = 0.0f;
        float everySec = 0.0f;  // 0: once
        bool band = false;      // SMART_EVENT_HEALTH_PCT: cast while its health is in [minPct, maxPct]
        float minPct = 0.0f;
    };
    std::vector<SelfHeal> SelfHealsOf(Creature* creature)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, std::vector<SelfHeal>> cache;  // entry -> its heals at its level
        uint32 const entry = creature->GetEntry();
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
                return found->second;
        }
        std::vector<SelfHeal> heals;
        if (creature->GetAIName() == "SmartAI")
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.action.type != SMART_ACTION_CAST || holder.target.type != SMART_TARGET_SELF ||
                    (holder.event.type != SMART_EVENT_UPDATE_IC && holder.event.type != SMART_EVENT_HEALTH_PCT))
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
                if (!info || !info->IsPositive())
                    continue;
                SelfHeal heal;
                for (SpellEffectInfo const& effect : info->GetEffects())
                {
                    if (effect.Effect == SPELL_EFFECT_HEAL)
                        heal.amount += std::max(MeanCalcValue(info, effect, creature), 0.0f);
                    else if (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                             effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL && effect.Amplitude > 0)
                        heal.amount += std::max(MeanCalcValue(info, effect, creature), 0.0f) *
                                       float(std::max(info->GetMaxDuration(), 0) / effect.Amplitude);
                }
                if (heal.amount <= 0.0f)
                    continue;
                float const repeatSec =
                    float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) / 2.0f /
                    float(IN_MILLISECONDS);
                if (!(holder.event.event_flags & SMART_EVENT_FLAG_NOT_REPEATABLE) && repeatSec > 0.0f)
                    heal.everySec = std::max(repeatSec, CastEverySec(info, creature));
                heal.band = holder.event.type == SMART_EVENT_HEALTH_PCT;
                heal.minPct = float(holder.event.minMaxRepeat.min);
                heals.push_back(heal);
            }
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[entry] = heals;
        return heals;
    }

    // [death388] the heals a creature's SmartAI casts on its hurt friends (SMART_EVENT_FRIENDLY_HEALTH + a heal cast)
    // over `fightSec`: its direct heal once per repeat (its cast time and the global cooldown at least)
    float FriendHealToCome(Creature* creature, float fightSec)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, std::pair<float, float>> cache;  // entry -> amount, every seconds
        uint32 const entry = creature->GetEntry();
        std::pair<float, float> heal{0.0f, 0.0f};
        bool cached = false;
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
            {
                heal = found->second;
                cached = true;
            }
        }
        if (!cached)
        {
            if (creature->GetAIName() == "SmartAI")
                for (SmartScriptHolder const& holder :
                     sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
                {
                    if (holder.event.type != SMART_EVENT_FRIENDLY_HEALTH || holder.action.type != SMART_ACTION_CAST)
                        continue;
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
                    if (!info || !info->IsPositive())
                        continue;
                    float amount = 0.0f;
                    for (SpellEffectInfo const& effect : info->GetEffects())
                        if (effect.Effect == SPELL_EFFECT_HEAL)
                            amount += std::max(MeanCalcValue(info, effect, creature), 0.0f);
                    if (amount <= 0.0f)
                        continue;
                    float const repeatSec = float(holder.event.friendlyHealth.repeatMin +
                                                  holder.event.friendlyHealth.repeatMax) /
                                            2.0f / float(IN_MILLISECONDS);
                    heal.first += amount;
                    heal.second = std::max(repeatSec, CastEverySec(info, creature));
                }
            std::lock_guard<std::mutex> guard(cacheLock);
            cache[entry] = heal;
        }
        if (heal.first <= 0.0f)
            return 0.0f;
        return heal.first * (1.0f + std::floor(std::max(fightSec, 0.0f) / std::max(heal.second, 0.1f)));
    }

    // the health a creature heals itself over the `fightSec` its fight lasts (a band heal only while its health has
    // the band still ahead or under way: above its lower bound)
    float HealToCome(Creature* creature, float fightSec)
    {
        float healed = 0.0f;
        for (SelfHeal const& heal : SelfHealsOf(creature))
        {
            if (heal.band && creature->GetHealthPct() <= heal.minPct)
                continue;
            healed += heal.everySec > 0.0f ? heal.amount * (1.0f + std::floor(std::max(fightSec, 0.0f) / heal.everySec))
                                           : heal.amount;
        }
        return healed;
    }

    // [deaths3] a mob's melee on the bot through the core's attack table (Unit::RollMeleeOutcomeAgainst): its hit after
    // the bot's armor, one roll per swing - miss (Unit::MeleeSpellMissChance), dodge / parry / block less 0.04 % per
    // weapon skill point of the mob over the bot's level (none while the victim casts: a caster bot fights casting),
    // crit (the core's chance, double damage), crushing blow (a mob 4+ levels above whose skill is 15+ over the bot's
    // defense: 2 % a point less 15 %, 150 % damage), the rest plain hits; a block takes off its block value
    float HandDps(Unit* unit, Player* bot, WeaponAttackType hand, float* variancePerSec);

    // [death434] each hand's swings: a creature with a weapon in its off-hand slot swings it too, on its own timer
    // (UnitAI::DoMeleeAttackIfReady, Creature::HasWeaponForAttack; UNIT_FIELD_MINOFFHANDDAMAGE carries the 50 % off-hand
    // cut). b462 Zharet L19 engaged 3 Defias Highwaymen (sword + sword) "929 dmg over 47 s" at ~10 dps each; they hit
    // for 16-19 each and killed it in 17 s
    float MeleeDps(Unit* unit, Player* bot, float* variancePerSec = nullptr)
    {
        float mainVariance = 0.0f;
        float dps = HandDps(unit, bot, BASE_ATTACK, variancePerSec ? &mainVariance : nullptr);
        float offVariance = 0.0f;
        if (unit->HasOffhandWeaponForAttack())
            dps += HandDps(unit, bot, OFF_ATTACK, variancePerSec ? &offVariance : nullptr);
        if (variancePerSec)
            *variancePerSec = mainVariance + offVariance;
        return dps;
    }

    float HandDps(Unit* unit, Player* bot, WeaponAttackType hand, float* variancePerSec)
    {
        bool const off = hand == OFF_ATTACK;
        float const minDamage = unit->GetFloatValue(off ? UNIT_FIELD_MINOFFHANDDAMAGE : UNIT_FIELD_MINDAMAGE);
        float const maxDamage = unit->GetFloatValue(off ? UNIT_FIELD_MAXOFFHANDDAMAGE : UNIT_FIELD_MAXDAMAGE);
        float const raw = std::max(0.0f, (minDamage + maxDamage) / 2.0f);
        if (raw <= 0.0f)
        {
            if (variancePerSec)
                *variancePerSec = 0.0f;
            return 0.0f;
        }
        float const hit =
            float(Unit::CalcArmorReducedDamage(unit, bot, uint32(raw), nullptr, unit->GetLevel(), hand));
        int32 const skillDiff =
            int32(unit->GetWeaponSkillValue(hand, bot)) - int32(bot->Unit::GetMaxSkillValueForLevel(unit));
        float const skillBonus = 0.04f * float(skillDiff);
        float left = 100.0f;
        auto const take = [&left](float chance)
        {
            chance = std::clamp(chance, 0.0f, left);
            left -= chance;
            return chance;
        };
        take(unit->MeleeSpellMissChance(bot, hand, skillDiff, 0));
        float block = 0.0f;
        if (!PlayerbotAI::IsCaster(bot))
        {
            take(bot->GetUnitDodgeChance() - skillBonus);
            take(bot->GetUnitParryChance() - skillBonus);
            block = take(bot->GetUnitBlockChance() - skillBonus);
        }
        float const crit = take(unit->GetUnitCriticalChance(hand, bot));
        float crush = 0.0f;
        Creature* creature = unit->ToCreature();
        if (unit->getLevelForTarget(bot) >= bot->getLevelForTarget(unit) + 4 && !unit->IsControlledByPlayer() &&
            !(creature && creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_CRUSHING_BLOWS)))
        {
            int32 const defense =
                std::min<int32>(int32(bot->GetDefenseSkillValue(unit)),
                                int32(bot->Unit::GetMaxSkillValueForLevel(unit)));
            int32 const lack = int32(unit->GetMaxSkillValueForLevel(bot)) - defense;
            if (lack >= 15)
                crush = take(float(lack) * 2.0f - 15.0f);
        }
        float const blocked = std::min(hit, float(bot->GetShieldBlockValue()));
        float const perSwing =
            (hit * (left + block) - blocked * block + 2.0f * hit * crit + 1.5f * hit * crush) / 100.0f;
        // [death386] the spread of a swing: its weapon roll (Unit::CalculateDamage: uniform min..max, after the bot's
        // armor) under the table's outcomes (none, plain, crit x2, crush x1.5); swings are independent, so the variance
        // of a fight adds up per second of swings
        if (variancePerSec)
        {
            float const reduce = raw > 0.0f ? hit / raw : 0.0f;
            float const low = minDamage * reduce;
            float const high = maxDamage * reduce;
            float const square = (low * low + low * high + high * high) / 3.0f;  // E[h^2] of the uniform roll
            float const second =
                (square * (left + block) + 4.0f * square * crit + 2.25f * square * crush) / 100.0f;
            *variancePerSec = std::max(second - perSwing * perSwing, 0.0f) / SwingSec(unit, hand);
        }
        return std::max(perSwing, 0.0f) / SwingSec(unit, hand);
    }

    // [guild14] what a mob deals the bot per second: its melee (the core's attack table, [deaths3]) and its spells (the
    // cast it is on, its SmartAI casts: the higher)
    float MobDps(Unit* unit, Player* bot)
    {
        float spell = 0.0f;
        Leader::Fight::AttackerDps(unit, bot, spell);  // the cast it is on
        float dps = MeleeDps(unit, bot);
        if (Creature* creature = unit->ToCreature())
            dps += std::max(spell, SpellDpsOf(creature));
        else
            dps += spell;
        return dps;
    }

    // [guild15] the help a creature's SmartAI brings in the fight: SMART_ACTION_CALL_FOR_HELP (its radius: every idle
    // mob that assists it) and SMART_ACTION_FLEE_FOR_ASSIST (Creature::DoFleeToGetAssistance: the nearest one that
    // assists it within CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS). b314: murloc camps - Streamrunners flee for
    // assist at 1-5 % health; judged 2, came 3-4, two deaths in 10 min
    struct ScriptHelp
    {
        float callRadius = 0.0f;
        bool flees = false;
        // [lag369] the health share its flee waits for (SMART_EVENT_HEALTH_PCT's HPMax of the flee's event; a flee on
        // another event: at any health, 1)
        float fleePct = 0.0f;
        // [deaths14] a caster that stands to cast (SMART_ACTION_CAST on its victim with SMARTCAST_COMBAT_MOVE: "prevents
        // combat movement if cast successful"): it stops at that spell's range from the bot, not at the fight spot -
        // its periodic assistance calls (Creature::Update -> CallAssistance) are made from there
        float standRange = 0.0f;
        // [death449] the creatures its script summons in a fight (SMART_ACTION_SUMMON_CREATURE, or a cast of a
        // SPELL_EFFECT_SUMMON spell, on an in-combat event): each comes once and fights the bot
        std::vector<uint32> summons;
    };
    ScriptHelp ScriptHelpOf(Creature* creature)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, ScriptHelp> cache;
        uint32 const entry = creature->GetEntry();
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
                return found->second;
        }
        ScriptHelp help;
        if (creature->GetAIName() == "SmartAI")
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.action.type == SMART_ACTION_CALL_FOR_HELP)
                    help.callRadius = std::max(help.callRadius, float(holder.action.callHelp.range));
                else if (holder.action.type == SMART_ACTION_FLEE_FOR_ASSIST)
                {
                    help.flees = true;
                    help.fleePct = std::max(help.fleePct,
                                            holder.event.type == SMART_EVENT_HEALTH_PCT
                                                ? std::min(float(holder.event.minMaxRepeat.max), 100.0f) / 100.0f
                                                : 1.0f);
                }
                else if (holder.action.type == SMART_ACTION_CAST && holder.target.type == SMART_TARGET_VICTIM &&
                         (holder.action.cast.castFlags & SMARTCAST_COMBAT_MOVE))
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell))
                        help.standRange = std::max(help.standRange, info->GetMaxRange(false));
                // [death449] b467 Salleas L20: Deepmoss Hatchling "At 30% HP - Summon Deepmoss Matriarch" (6536), two
                // Matriarchs L22 joined a fight judged as 2 hatchlings, dead
                uint32 const event = holder.event.type;
                if (event == SMART_EVENT_UPDATE_OOC || event == SMART_EVENT_RESET || event == SMART_EVENT_RESPAWN ||
                    event == SMART_EVENT_LINK || event == SMART_EVENT_JUST_SUMMONED ||
                    event == SMART_EVENT_JUST_CREATED || event == SMART_EVENT_EVADE || event == SMART_EVENT_OOC_LOS)
                    continue;
                uint32 summoned = 0;
                if (holder.action.type == SMART_ACTION_SUMMON_CREATURE)
                    summoned = holder.action.summonCreature.creature;
                else if (holder.action.type == SMART_ACTION_CAST)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell))
                        for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !summoned; ++i)
                            if (info->Effects[i].Effect == SPELL_EFFECT_SUMMON && info->Effects[i].MiscValue > 0)
                                summoned = uint32(info->Effects[i].MiscValue);
                if (summoned && sObjectMgr->GetCreatureTemplate(summoned))
                    help.summons.push_back(summoned);
            }
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[entry] = help;
        return help;
    }

    // [deaths3] the largest wander distance of a map's spawns (creature.wander_distance of RANDOM_MOTION_TYPE spawns),
    // from the spawn data once: a wanderer's reach is its home's, so the route search looks that much farther
    float MaxWanderDistance(uint32 mapId)
    {
        static std::once_flag built;
        static std::unordered_map<uint32, float> byMap;
        std::call_once(built,
                       []
                       {
                           for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                               if (data.movementType == RANDOM_MOTION_TYPE)
                                   byMap[data.mapid] = std::max(byMap[data.mapid], data.wander_distance);
                       });
        auto const found = byMap.find(mapId);
        return found == byMap.end() ? 0.0f : found->second;
    }

    // [deaths3] a pet that holds a mob on itself: it knows a taunt (SPELL_EFFECT_ATTACK_ME or SPELL_AURA_MOD_TAUNT)
    bool PetTaunts(Pet* pet)
    {
        for (auto const& [spellId, spell] : pet->m_spells)
        {
            if (spell.state == PETSPELL_REMOVED)
                continue;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.Effect == SPELL_EFFECT_ATTACK_ME ||
                        (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_MOD_TAUNT))
                        return true;
        }
        return false;
    }

    // [pull9] the judge's "loses even at full" for a pack it beats one at a time (its kill order: `foes` sorted, the
    // first held by a taunting pet): its rotation while its full mana lasts then its white damage, against its full
    // health, the pet's hold and its heals (Judge's projection at full; the route search's wall test)
    struct RouteFoe
    {
        Creature* mob;
        float dps, hp;
    };
    bool LosesAtFull(Player* bot, DpsEstimate::Result const& own, std::vector<RouteFoe> const& foes, float petHealth)
    {
        // [dpszero1] its own damage with no invented floor. NO ESTIMATE: no wall is built on a number nobody
        // computed (rule 7) - the walk is not refused here, it is simply not judged here, which is what this
        // function already answers when it cannot tell. A MEASURED ZERO: the pack is never killed, so it loses -
        // and that is an answer about the fight, not a small number standing in for one.
        KillRate::Own const burst{own.estimated, own.burstDps};
        KillRate::Own const white{own.estimated, std::min(own.whiteDps, own.burstDps)};
        if (!KillRate::Judgeable(burst))
            return false;
        if (KillRate::StateOf(burst) == KillRate::State::DealsNothing)
            return true;
        float const maxMana = float(bot->GetMaxPower(POWER_MANA));
        bool const spends = maxMana > 0.0f && own.manaPerSec > 0.0f;
        float const spellSec = spends ? maxMana / own.manaPerSec : std::numeric_limits<float>::max();
        float seconds = 0.0f;
        float damage = 0.0f;
        float targetDamage = 0.0f;
        float variance = 0.0f;
        for (RouteFoe const& foe : foes)
        {
            float hp = foe.hp + HealToCome(foe.mob, KillRate::KillSec(burst, foe.hp));
            if (seconds < spellSec)
            {
                float const spellPart = (spellSec - seconds) * burst.dps;
                float const part = std::min(hp, spellPart);
                seconds += KillRate::KillSec(burst, part);
                hp -= part;
            }
            if (hp > 0.0f)
            {
                // [dpszero1] nothing of its damage outlives its mana: the rest of the pack never falls
                if (KillRate::StateOf(white) != KillRate::State::Deals)
                    return true;
                seconds += KillRate::KillSec(white, hp);
            }
            damage += foe.dps * seconds;
            // [death396] the judge's high end ([death386]): the swings' and casts' spread over the seconds each one
            // lives. Before, the mean: b453 Maronic L21 "Redridge Brute: wins on the mean (2949 dmg) but not at the
            // high end (3610 dmg) at ehp 3370 full" - its walks were priced through the camps the judge refused
            float swings = 0.0f;
            MeleeDps(foe.mob, bot, &swings);
            variance += (swings + SpellVarianceOf(foe.mob)) * seconds;
            if (&foe == &foes.front())
                targetDamage = foe.dps * seconds;
        }
        constexpr float HIGH_END_DEVIATIONS = 2.0f;  // the judge's one-sided 97.7 % bound
        float const high = damage + HIGH_END_DEVIATIONS * std::sqrt(variance);
        float const health = float(bot->GetMaxHealth());
        if (high < health)
            return false;  // no heal needed (the heal kit is read only for a close call)
        float const heals = SurvivalKit::FightHeals(bot, seconds, health, maxMana, spends ? own.manaPerSec : 0.0f,
                                                    high / std::max(seconds, 1.0f), true);  // [death442]
        float const ehp = health + std::min(petHealth, targetDamage) + heals;
        if (damage < ehp && high >= ehp && !foes.empty())
        {
            static std::mutex highLogLock;  // a log cadence per bot and mob kind, no decision reads it
            static std::unordered_set<uint64> highLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(highLogLock);
                if (highLogged.size() > 65536)
                    highLogged.clear();
                fresh = highLogged.insert(uint64(foes.front().mob->GetEntry()) * 1000003ull +
                                          bot->GetGUID().GetCounter())
                            .second;
            }
            if (fresh)
                LOG_INFO("playerbots", "[death396] {} L{} walls its walk at {} L{} ({} mobs): wins on the mean ({:.0f} "
                         "dmg) but not at the high end ({:.0f} dmg, +2 sd) at ehp {:.0f} full",
                         bot->GetName(), bot->GetLevel(), foes.front().mob->GetName(), foes.front().mob->GetLevel(),
                         foes.size(), damage, high, ehp);
        }
        return high >= ehp;
    }

    char const* KindName(SoloSafety::Verdict::Kind kind)
    {
        switch (kind)
        {
            case SoloSafety::Verdict::Kind::Win:
                return "engage";
            case SoloSafety::Verdict::Kind::RestFirst:
                return "rest first";
            default:
                return "refuse";
        }
    }

    bool IsElite(Creature* creature)
    {
        uint32 const rank = creature->GetCreatureTemplate()->rank;
        return rank == CREATURE_ELITE_ELITE || rank == CREATURE_ELITE_RAREELITE || rank == CREATURE_ELITE_WORLDBOSS;
    }

    // [pullplan] an idle mob's aggro reach on the bot as a disc (x, y, z, radius)
    // [death470] the arithmetic itself is the seam's (AggroReach::Of, a pure function, world- and clock-free): one
    // judge of "what ground does this mob's aggro cover", and the only place that reads the world for it is here.
    // [surv10] lives on inside it: a FREE wanderer's disc sits on its home, widened by how far it has actually gone,
    // so it holds the area it returns to AND the circle round where it stands now (b484 Kylantino L22 met a
    // Boulderfist Ogre L33 at 5 yd and the home disc read "outside it"). What is new is the mob that is HELD away
    // from home by something of its own - a fight, the walk back after one: its ground is its home and its wander
    // area, and the yards it is being held in are ground it is leaving.
    G3D::Vector4 AggroDisc(Creature* mob, Player* bot)
    {
        AggroReach::Mob ask;
        Position const& home = mob->GetHomePosition();
        ask.homeX = home.GetPositionX();
        ask.homeY = home.GetPositionY();
        ask.homeZ = home.GetPositionZ();
        ask.x = mob->GetPositionX();
        ask.y = mob->GetPositionY();
        ask.z = mob->GetPositionZ();
        ask.aggroYd = mob->GetAggroRange(bot);
        ask.botReachYd = bot->GetCombatReach();
        ask.wanders = mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE;
        ask.wanderYd = mob->GetWanderDistance();
        ask.busy = mob->IsInCombat() || mob->IsInEvadeMode();
        AggroReach::Disc const disc = AggroReach::Of(ask);
        return G3D::Vector4(disc.x, disc.y, disc.z, disc.radius);
    }

    bool InDisc(G3D::Vector4 const& disc, G3D::Vector3 const& at)
    {
        return (disc.x - at.x) * (disc.x - at.x) + (disc.y - at.y) * (disc.y - at.y) <= disc.w * disc.w;
    }

    // [pullplan] CreatureGroup::MemberEngagingTarget: the engage of `engaged` brings its formation by its group flags
    bool EngagesFormation(Creature* engaged, CreatureGroup const* formation)
    {
        auto const& members = formation->GetMembers();
        auto const found = members.find(engaged);
        if (found == members.end())
            return false;
        GroupAIFlags const needed = engaged == formation->GetLeader() ? GroupAIFlags::GROUP_AI_FLAG_MEMBER_ASSIST_LEADER
                                                                     : GroupAIFlags::GROUP_AI_FLAG_LEADER_ASSIST_MEMBER;
        return found->second.HasGroupFlag(std::underlying_type_t<GroupAIFlags>(needed));
    }

    // [pullplan] user 2026-11-08: what the core brings with a solo pull, mechanic by mechanic:
    // - the engage call (Unit::Attack -> Creature::CallAssistance) of every mob that engages by itself (the target,
    //   its formation, a mob whose aggro reach covers the bot), where it stands then; an assistant engages with its
    //   call off (AssistDelayEvent: SetNoCallAssistance) - AnyAssistCreatureInRangeCheck: centre to centre within
    //   CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS, in the caller's sight, Creature::CanAssistTo;
    // - the formation (CreatureGroup::MemberEngagingTarget, its group flags);
    // - the periodic call (Creature::Update, every CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD from the engage on) of
    //   every engaged mob from where it fights: the fight spot, and the target's first one on its run there;
    // - its script's call for help (SMART_ACTION_CALL_FOR_HELP) and flee for assistance at low health, at the fight;
    // - an idle mob whose aggro reach (its wander area, its patrol beat) covers the fight spot.
    // Nothing else comes: an idle mob near the target's camp stays when the fight is away from it.
    struct PullModel
    {
        struct Member
        {
            Creature* mob;
            bool calls;  // makes an engage call of its own
        };
        Player* bot = nullptr;
        Creature* target = nullptr;
        std::vector<Creature*> idle;  // idle hostile aggressive mobs round it (not the target)
        std::vector<Member> pack;
        float assist = 0.0f;
        std::unordered_map<Creature*, std::vector<G3D::Vector3>> beats;  // patrols' forecast walks
        size_t linkedUpTo = 0;  // the members whose links are counted

        PullModel(Player* walker, Creature* mob, std::vector<Creature*> const& around) : bot(walker), target(mob)
        {
            assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
            for (Creature* c : around)
                if (c && c != mob)
                    idle.push_back(c);
            pack.push_back({mob, mob->IsAlive() && !mob->IsInCombat()});
        }

        bool Has(Creature* c) const
        {
            return std::any_of(pack.begin(), pack.end(), [c](Member const& m) { return m.mob == c; });
        }

        std::vector<G3D::Vector3> const& Beat(Creature* c)
        {
            auto found = beats.find(c);
            if (found != beats.end())
                return found->second;
            std::vector<G3D::Vector3> track;
            float sampleSec = 0.0f;
            if (c->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE ||
                !PullPlanning::PatrolForecast(c, track, sampleSec))
                track.clear();
            return beats.emplace(c, std::move(track)).first->second;
        }

        // the idle mob's reach covers `at` (where it stands or wanders, or anywhere on its patrol beat)
        bool Reaches(Creature* c, G3D::Vector3 const& at)
        {
            G3D::Vector4 const disc = AggroDisc(c, bot);
            if (InDisc(disc, at))
                return true;
            for (G3D::Vector3 const& step : Beat(c))
                if (InDisc(G3D::Vector4(step.x, step.y, step.z, disc.w), at))
                    return true;
            return false;
        }

        bool Sees(G3D::Vector3 const& from, Creature* c) const
        {
            return bot->GetMap()->isInLineOfSight(from.x, from.y, from.z + bot->GetCollisionHeight(),
                                                  c->GetPositionX(), c->GetPositionY(),
                                                  c->GetPositionZ() + c->GetCollisionHeight(), bot->GetPhaseMask(),
                                                  LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
        }

        // the formations and engage calls of the members not counted yet
        void Links()
        {
            for (size_t i = linkedUpTo; i < pack.size(); ++i)
            {
                Creature* m = pack[i].mob;
                if (CreatureGroup* formation = m->GetFormation(); formation && EngagesFormation(m, formation))
                    for (auto const& [mate, info] : formation->GetMembers())
                        if (mate && mate != m && mate->IsAlive() && !mate->GetVictim() && !Has(mate))
                            pack.push_back({mate, true});
                if (!pack[i].calls || assist <= 0.0f || m->HasFlagsExtra(CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE))
                    continue;
                for (Creature* c : idle)
                    if (!Has(c) && m->IsWithinDistInMap(c, assist, true, false, false) && c->CanAssistTo(m, bot) &&
                        m->IsWithinLOSInMap(c))
                        pack.push_back({c, false});
            }
            linkedUpTo = pack.size();
        }

        // the pack of a fight at `fight` (nullptr: what comes wherever it is fought - the linked pack); `firstCall`:
        // where the target makes its first periodic call when it has not reached the fight by then
        void Grow(G3D::Vector3 const* fight, G3D::Vector3 const* firstCall)
        {
            std::set<Creature*> fled;  // one flee per member
            for (bool changed = true; changed;)
            {
                changed = false;
                Links();
                if (!fight)
                    break;
                std::vector<float> callRadius;
                // [deaths14] where each standing caster of the pack casts from: on its way to the fight, at its spell's
                // range from the bot's fight spot (its own spot when nearer). b354 Kylantino L11 / Lanbuzz L9 (Frostmane
                // Seer), Thelengyl L10 (Greymist Seer), Baridrol L10 (Gnarlpine Augur): judged 1, came 3-4 - the seer
                // stood by its camp casting Lightning Bolt and its calls brought the camp; a melee bot closes in to it
                std::vector<std::pair<Creature*, G3D::Vector3>> stands;
                for (Member const& m : pack)
                {
                    ScriptHelp const help = ScriptHelpOf(m.mob);
                    callRadius.push_back(help.callRadius);
                    if (help.standRange <= 0.0f)
                        continue;
                    G3D::Vector3 const at(m.mob->GetPositionX(), m.mob->GetPositionY(), m.mob->GetPositionZ());
                    float const gap = (at - *fight).length();
                    stands.push_back(
                        {m.mob, gap <= help.standRange ? at : *fight + (at - *fight) * (help.standRange / gap)});
                }
                bool const closesIn = !PlayerbotAI::IsRanged(bot);
                for (Creature* c : idle)
                {
                    if (Has(c))
                        continue;
                    bool joins = Reaches(c, *fight);
                    bool calls = true;
                    for (size_t k = 0; !joins && k < stands.size(); ++k)
                    {
                        G3D::Vector3 const& stand = stands[k].second;
                        if ((assist > 0.0f && c->GetExactDist(stand.x, stand.y, stand.z) <= assist &&
                             c->CanAssistTo(stands[k].first, bot) && Sees(stand, c)) ||
                            (closesIn && Reaches(c, stand)))
                        {
                            joins = true;
                            calls = !(assist > 0.0f && c->GetExactDist(stand.x, stand.y, stand.z) <= assist);
                            static std::mutex standLogLock;
                            static std::unordered_set<uint64> standLogged;
                            bool fresh = false;
                            {
                                std::lock_guard<std::mutex> guard(standLogLock);
                                if (standLogged.size() > 65536)
                                    standLogged.clear();
                                fresh = standLogged.insert(c->GetGUID().GetCounter() * 1000003ull +
                                                           bot->GetGUID().GetCounter()).second;
                            }
                            if (fresh)
                                LOG_INFO("playerbots", "[deaths14] {} L{} judge: {} L{} joins from the spot a caster of "
                                         "the pack casts from ({:.0f},{:.0f}), {:.0f} yd from the fight",
                                         bot->GetName(), bot->GetLevel(), c->GetName(), c->GetLevel(), stand.x,
                                         stand.y, (stand - *fight).length());
                        }
                    }
                    float const gap = c->GetExactDist(fight->x, fight->y, fight->z);
                    // [regress10] the engaged members call every CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD while they
                    // fight: a wanderer anywhere in its wander area or a patrol anywhere on its beat may be within the
                    // assistance radius at one of those calls (b347 Idunk L10 x2: judged 1 Tunnel Rat, came 3-4 with
                    // the wanderers "assists a chaser"); the nearest it comes to the fight
                    float reachGap = gap;
                    if (c->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                        reachGap = std::min(reachGap,
                                            std::max(0.0f, c->GetHomePosition().GetExactDist(fight->x, fight->y,
                                                                                            fight->z) -
                                                               c->GetWanderDistance()));
                    for (G3D::Vector3 const& step : Beat(c))
                        reachGap = std::min(reachGap, (step - *fight).length());
                    bool const roams = reachGap < gap;  // its sight from there is not known now: counted
                    for (size_t i = 0; !joins && i < callRadius.size(); ++i)
                    {
                        Creature* m = pack[i].mob;
                        if (assist > 0.0f && reachGap <= assist && c->CanAssistTo(m, bot) && (roams || Sees(*fight, c)))
                        {
                            joins = true;
                            calls = false;
                        }
                        else if (callRadius[i] > 0.0f && gap <= callRadius[i] && c->CanAssistTo(m, bot))
                            joins = true;
                    }
                    if (!joins && firstCall && assist > 0.0f &&
                        c->GetExactDist(firstCall->x, firstCall->y, firstCall->z) <= assist &&
                        c->CanAssistTo(target, bot) && Sees(*firstCall, c))
                    {
                        joins = true;
                        calls = false;
                    }
                    if (joins)
                    {
                        pack.push_back({c, calls});
                        changed = true;
                    }
                }
                // a member that flees for assistance runs to the nearest mob that answers within the flee radius
                // [deaths13] the runner searches from where it stands when it flees (Creature::DoFleeToGetAssistance:
                // NearestAssistCreatureInCreatureRangeCheck round the mob), and the mob stands within the bot's own
                // fighting reach of the fight spot then - a ranged bot's spell reach (it kites a rooted mob there),
                // else its melee reach. b353 Thamesh L10 (mage): the Tunnel Rat Scout ran for help at 15 % from 31 yd,
                // judged 1, came 5, died; the search had looked 30 yd round the bot only
                float const flee = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS) +
                                   (PlayerbotAI::IsRanged(bot) ? Leader::Fight::LeaderSpellReach(bot)
                                                               : bot->GetCombatReach());
                for (size_t i = 0; i < pack.size(); ++i)
                {
                    if (!ScriptHelpOf(pack[i].mob).flees || !fled.insert(pack[i].mob).second)
                        continue;
                    Creature* nearest = nullptr;
                    float best = flee;
                    for (Creature* c : idle)
                        if (!Has(c) && c->CanAssistTo(pack[i].mob, bot))
                            if (float const gap = c->GetExactDist2d(fight->x, fight->y); gap <= best)
                            {
                                best = gap;
                                nearest = c;
                            }
                    // [lag369] coordinator 2026-11-12 (b449 Gilduck L14 mage in Westfall: every Defias pull "flees for
                    // help to Defias Trapper 33 yd from the fight", 0 xp from 07:18): the runner flees when its health
                    // falls to its script's share (SMART_EVENT_HEALTH_PCT) and the help comes only when it reaches its
                    // helper (MoveSeekAssistance at its run speed, the call at its arrival) - a bot whose damage takes
                    // the rest of its health before that kills it on the way and nobody comes
                    if (nearest)
                    {
                        ScriptHelp const help = ScriptHelpOf(pack[i].mob);
                        float const left = help.fleePct * float(pack[i].mob->GetMaxHealth());
                        // [death412] ... a melee bot hits it on its run only while it is faster than the runner (the
                        // runner leaves its melee reach at once at the same speed): b455 Ginde / Aestymro / Emdyl at
                        // the Redridge Canyons gnolls - "Redridge Mystic would flee at 15 % ...: killed first in 2.7 s
                        // at 20.0 dps", the Bashers and Mystics of the next camp joined 17-24 s in, 3 deaths
                        bool const keepsUp = PlayerbotAI::IsRanged(bot) ||
                                             bot->GetSpeed(MOVE_RUN) > pack[i].mob->GetSpeed(MOVE_RUN);
                        float const dps =
                            keepsUp ? DpsEstimate::EstimateDps(bot, pack[i].mob, DpsEstimate::BuffContext::Solo) : 0.0f;
                        float const run = best / std::max(pack[i].mob->GetSpeed(MOVE_RUN), 0.1f);
                        if (dps > 0.0f && left / dps < run)
                        {
                            static std::mutex killLogLock;  // one line per bot and runner kind: a log cadence
                            static std::unordered_set<uint64> killLogged;
                            bool fresh = false;
                            {
                                std::lock_guard<std::mutex> guard(killLogLock);
                                fresh = killLogged.insert(uint64(pack[i].mob->GetEntry()) * 1000003ull +
                                                          bot->GetGUID().GetCounter())
                                            .second;
                            }
                            if (fresh)
                                LOG_INFO("playerbots", "[lag369] {} L{} judge: {} would flee at {:.0f} % ({:.0f} hp) "
                                         "to {} {:.0f} yd off ({:.1f} s run): killed first in {:.1f} s at {:.1f} dps",
                                         bot->GetName(), bot->GetLevel(), pack[i].mob->GetName(),
                                         help.fleePct * 100.0f, left, nearest->GetName(), best, run, left / dps, dps);
                            continue;
                        }
                        pack.push_back({nearest, true});
                        changed = true;
                        // [death443] the runner's call at its helper (AssistanceMovementGenerator::Finalize ->
                        // Creature::CallAssistance): every idle mob within the assistance radius of where it arrives
                        // that assists it comes too, not the nearest alone. b462 Bami L22 at Stonewatch Tower: the
                        // Blackrock Scouts / Sentries flee at 15 %, 8 came to a camp walk priced as single pulls
                        G3D::Vector3 const arrival(nearest->GetPositionX(), nearest->GetPositionY(),
                                                   nearest->GetPositionZ());
                        size_t const hopFrom = pack.size() - 1;  // the helper, then its arrival callees
                        if (assist > 0.0f)
                            for (Creature* c : idle)
                                if (!Has(c) && c->GetExactDist(arrival.x, arrival.y, arrival.z) <= assist &&
                                    c->CanAssistTo(pack[i].mob, bot) && Sees(arrival, c))
                                    pack.push_back({c, true});
                        // [death454] ... and each one answering makes its own calls on its way to the fight: the
                        // core's periodic CallAssistance (Creature::Update every ASSISTANCE_PERIOD from where it is,
                        // AnyAssistCreatureInRangeCheck within the assistance radius; the AssistDelayEvent's
                        // NoCallAssistance is reset there), so the chain grows hop by hop from each caller's walk to
                        // the fight. b467 Irorm L23: a Mosshide runner's helper brought a second
                        // hop of Fenrunners / Mistweavers 12-20 yd past the [death443] radius - judged 2, came 7
                        float const period =
                            float(sWorld->getIntConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD)) / IN_MILLISECONDS;
                        for (size_t h = hopFrom; assist > 0.0f && period > 0.0f && h < pack.size(); ++h)
                        {
                            Creature* caller = pack[h].mob;
                            if (caller->HasFlagsExtra(CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE))
                                continue;
                            G3D::Vector3 const start(caller->GetPositionX(), caller->GetPositionY(),
                                                     caller->GetPositionZ());
                            float const walk = (*fight - start).length();
                            float const step = std::max(caller->GetSpeed(MOVE_RUN) * period, 1.0f);
                            for (Creature* c : idle)
                            {
                                if (Has(c) || !c->CanAssistTo(caller, bot))
                                    continue;
                                for (float d = 0.0f; d <= walk + step; d += step)
                                {
                                    G3D::Vector3 const at =
                                        walk > 0.0f ? start + (*fight - start) * (std::min(d, walk) / walk) : start;
                                    if (c->GetExactDist(at.x, at.y, at.z) <= assist && Sees(at, c))
                                    {
                                        pack.push_back({c, true});
                                        static std::mutex hopLogLock;  // one line per bot and mob: a log cadence
                                        static std::unordered_set<uint64> hopLogged;
                                        bool fresh = false;
                                        {
                                            std::lock_guard<std::mutex> guard(hopLogLock);
                                            if (hopLogged.size() > 65536)
                                                hopLogged.clear();
                                            fresh = hopLogged.insert(c->GetGUID().GetCounter() * 1000003ull +
                                                                     bot->GetGUID().GetCounter()).second;
                                        }
                                        if (fresh)
                                            LOG_INFO("playerbots", "[death454] {} L{} judge: {} L{} joins at the call "
                                                     "of {} L{} on its way to the fight ({:.0f} yd from the fight)",
                                                     bot->GetName(), bot->GetLevel(), c->GetName(), c->GetLevel(),
                                                     caller->GetName(), caller->GetLevel(), (at - *fight).length());
                                        break;
                                    }
                                }
                            }
                        }
                        // one line per runner and helper beyond the radius round the fight spot alone
                        if (best > sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS))
                        {
                            static std::mutex fleeLogLock;
                            static std::unordered_set<uint64> fleeLogged;
                            uint64 const key = pack[i].mob->GetGUID().GetCounter() * 1000003ull +
                                               nearest->GetGUID().GetCounter();
                            bool fresh = false;
                            {
                                std::lock_guard<std::mutex> guard(fleeLogLock);
                                fresh = fleeLogged.insert(key).second;
                            }
                            if (fresh)
                                LOG_INFO("playerbots", "[deaths13] {} L{} judge: {} flees for help to {} {:.0f} yd "
                                         "from the fight (its runner's reach {:.0f} yd)",
                                         bot->GetName(), bot->GetLevel(), pack[i].mob->GetName(), nearest->GetName(),
                                         best, flee);
                        }
                    }
                }
            }
        }
    };

    // [death427] a ranged-role bot without a rotation of ranged spells (a healer spec): the longest reach of a damage
    // spell it knows, 0 with none. LeaderSpellReach's fixed 30 yd for it is a heal's reach: b460 Rollothan L15 (holy
    // paladin, no ranged damage spell) was given pull spots 27 yd from the target, made no pull there and walked on
    // to melee - 8 of its 8 pulls 7-23 yd off the spot, the last one into a trogg camp (6 came, dead)
    float KnownDamageReach(Player* bot)
    {
        struct Known
        {
            uint64 key;
            float reach;
        };
        thread_local std::unordered_map<ObjectGuid, Known> known;
        PlayerSpellMap const& spells = bot->GetSpellMap();
        uint64 const key = uint64(bot->GetLevel()) << 48 | uint64(bot->GetActiveSpec()) << 40 | uint64(spells.size());
        if (auto const it = known.find(bot->GetGUID()); it != known.end() && it->second.key == key)
            return it->second.reach;
        float reach = 0.0f;
        for (auto const& [spellId, spell] : spells)
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active ||
                !spell->IsInSpec(bot->GetActiveSpec()))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || info->IsPositive())
                continue;
            bool damage = false;
            for (SpellEffectInfo const& effect : info->GetEffects())
                damage = damage || effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                         (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                          effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE);
            if (damage)
                reach = std::max(reach, info->GetMaxRange(false, bot));
        }
        if (reach <= NOMINAL_MELEE_RANGE)
            reach = 0.0f;
        if (known.size() > 4096)
            known.clear();
        known[bot->GetGUID()] = {key, reach};
        return reach;
    }

    // [pullplan] the bot's pull from range: its rotation's reach for a ranged bot; else Throw / Shoot with the ranged
    // weapon (and ammunition) that spell needs; 0 when it has none (a body pull at the target's aggro edge)
    float PullReach(Player* bot, uint32* spellOut = nullptr)
    {
        if (spellOut)
            *spellOut = 0;
        if (PlayerbotAI::IsRanged(bot))
            return Leader::Fight::LeaderRotationSpellNames(bot).empty() ? KnownDamageReach(bot)  // [death427]
                                                                        : Leader::Fight::LeaderSpellReach(bot);
        constexpr uint32 SPELL_THROW = 2764;
        constexpr uint32 SPELL_SHOOT = 3018;  // bows, guns, crossbows
        Item* weapon = bot->GetWeaponForAttack(RANGED_ATTACK, true);
        if (!weapon)
            return 0.0f;
        uint32 spell = 0;
        switch (weapon->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_THROWN:
                spell = SPELL_THROW;
                break;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_GUN:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            {
                uint32 const ammo = bot->GetUInt32Value(PLAYER_AMMO_ID);
                if (ammo && bot->HasItemCount(ammo, 1))
                    spell = SPELL_SHOOT;
                break;
            }
            default:
                break;
        }
        SpellInfo const* info = spell && bot->HasSpell(spell) ? sSpellMgr->GetSpellInfo(spell) : nullptr;
        if (!info)
            return 0.0f;
        if (spellOut)
            *spellOut = spell;
        return info->GetMaxRange(false, bot);
    }
}

namespace SoloSafety
{
    bool FightsWith(Player* bot, Player* member) { return MemberSupports(bot, member); }

    namespace
    {
        // [wall17] a judge asked by the route search for a mob on the walk (RouteCosts): it reads and writes none of
        // the bot's engage state (the pull spot search kept for its target, the verdict line), and the route searches
        // of its own pull spot search price their mobs without asking it again (no recursion)
        thread_local bool wallJudge = false;
    }

    float MapWanderReach(uint32 mapId) { return MaxWanderDistance(mapId); }

    float DamageVariancePerSec(Unit* mob, Player* victim)
    {
        if (!mob || !victim)
            return 0.0f;
        float variance = 0.0f;
        MeleeDps(mob, victim, &variance);
        if (Creature* caster = mob->ToCreature())
            variance += SpellVarianceOf(caster);
        return variance;
    }

    std::vector<Creature*> PackAt(Player* bot, Creature* target, G3D::Vector3 const& fight,
                                  std::vector<G3D::Vector3> const& stands)
    {
        std::vector<Creature*> out;
        if (!bot || !target || !target->IsInWorld())
            return out;
        // the idle mobs that may answer: round the fight as far as the largest reach of a call there goes (aggro or
        // the target's script call, the assistance radius, the map's widest wander area)
        float const assist = std::max(0.0f, sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS));
        float const range =
            std::min(std::max(MAX_AGGRO_RADIUS, ScriptHelpOf(target).callRadius) + assist +
                         sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS) +
                         MaxWanderDistance(bot->GetMapId()),
                     bot->GetMap()->GetVisibilityRange());
        std::vector<Creature*> idle;
        {
            std::unordered_set<uint64> taken;
            // [surv22] ... round every spot the pack is counted at, not round the target alone. A `stands` list that
            // is a run ([surv22] in RunPastStep: the leg from the bot on, the chase's seconds at its run speed) is
            // longer than this one look: the chase runs 134 yd and more, the look reaches the visibility range at
            // most, so the mobs on the far part of the run were never candidates at all and no number of stands
            // could bring them in. One look per spot no earlier look covers - the same `range`, nothing new
            std::vector<G3D::Vector3> centres;
            for (G3D::Vector3 const& at : stands)
            {
                bool covered = std::hypot(at.x - target->GetPositionX(), at.y - target->GetPositionY()) <= range;
                for (size_t i = 0; !covered && i < centres.size(); ++i)
                    covered = std::hypot(at.x - centres[i].x, at.y - centres[i].y) <= range;
                if (!covered)
                    centres.push_back(at);
            }
            // (the check measures from the target, so a look round a spot takes that spot's own distance on top and
            // the spot's own range is told apart here)
            auto const keep = [&](std::list<Unit*> const& units, G3D::Vector3 const* at)
            {
                for (Unit* unit : units)
                {
                    Creature* mob = unit->ToCreature();
                    if (!mob || mob == target || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                        !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID())
                        continue;
                    if (at && std::hypot(mob->GetPositionX() - at->x, mob->GetPositionY() - at->y) > range)
                        continue;
                    if (taken.insert(mob->GetGUID().GetRawValue()).second)
                        idle.push_back(mob);
                }
            };
            {
                std::list<Unit*> units;
                Acore::AnyUnitInObjectRangeCheck check(target, range);
                Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(target, units, check);
                Cell::VisitObjects(target, searcher, range);
                keep(units, nullptr);
            }
            for (G3D::Vector3 const& at : centres)
            {
                float const reach =
                    range + std::hypot(at.x - target->GetPositionX(), at.y - target->GetPositionY());
                std::list<Unit*> units;
                Acore::AnyUnitInObjectRangeCheck check(target, reach);
                Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(target, units, check);
                Cell::VisitObjects(at.x, at.y, bot->GetMap(), searcher, range);
                keep(units, &at);
            }
        }
        PullModel model(bot, target, idle);
        model.Grow(&fight, nullptr);
        // the members' own spots: an idle mob whose reach (its wander area, its patrol beat) covers where a member
        // stands in the fight joins as well, and the pack grows from it
        for (bool added = true; added;)
        {
            added = false;
            for (Creature* c : model.idle)
            {
                if (model.Has(c))
                    continue;
                for (G3D::Vector3 const& at : stands)
                    if (model.Reaches(c, at))
                    {
                        model.pack.push_back({c, true});
                        added = true;
                        break;
                    }
            }
            if (added)
                model.Grow(&fight, nullptr);
        }
        for (PullModel::Member const& m : model.pack)
            if (m.mob != target)
                out.push_back(m.mob);
        return out;
    }

    bool WalksAlone(Player* bot) { return SoloPlayerBot(bot); }

    bool Applies(Player* bot)
    {
        if (!SoloPlayerBot(bot))
            return false;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT);
    }

    void NoteDamage(Unit* attacker, Unit* victim, uint32 damage)
    {
        if (!attacker || !victim || !damage)
            return;
        uint32 const now = getMSTime();
        // the bot takes it
        if (Player* bot = victim->ToPlayer())
        {
            Unit* source = attacker->GetCharmerOrOwnerOrSelf();
            Creature* mob = source ? source->ToCreature() : nullptr;
            if (!mob || !SoloPlayerBot(bot))
                return;
            // [deaths] a dead mob's damage (its poison / bleed still ticking after it died: a DoT tick's attacker is
            // its caster) is no fight: it opened false ones (b338 Dandolar NO_ATTACK, 12 dmg in 12 s while eating;
            // Webwood Lurker "never attacked, fight_s=3.2, taken=1")
            if (!mob->IsAlive() || !source->IsAlive() || !attacker->IsAlive())
                return;
            std::lock_guard<std::mutex> guard(lock);
            SoloBotState& state = states[bot->GetGUID().GetCounter()];
            OpenIfNeeded(bot, state, now, mob);
            state.lastMs = now;
            uint32 const taken = std::min(damage, bot->GetHealth());
            state.taken += taken;
            SampleMelee(bot, mob, state, now);  // [guild15]
            AttackerTrack& track = state.attackers[mob->GetGUID().GetRawValue()];
            if (!track.entry)
            {
                track.entry = mob->GetEntry();
                track.firstMs = now;
                track.swingSec = SwingSec(mob);
                track.name = mob->GetName();
                track.level = mob->GetLevel();
                // [deaths139] an attacker past the judged count: how it came, for the triage's add classes (b426-b428:
                // "adds: judged 1, came 2-8" 38 deaths, 19 of them at the Cryo-Core's Sunhawk Reclaimers - wanderers
                // 8-20 yd apart; the log held nothing of where the add stood or what it was doing). A log, no decision
                if (state.engaged && state.attackers.size() > state.engage.mobs)
                {
                    G3D::Vector3 const& fight = state.engage.fightAt;
                    Position const& home = mob->GetHomePosition();
                    float nearestAttacker = -1.0f;
                    std::string nearestName = "-";
                    for (auto const& [raw, other] : state.attackers)
                        if (raw != mob->GetGUID().GetRawValue())
                            if (Creature* c = ObjectAccessor::GetCreature(*bot, ObjectGuid(raw)))
                                if (float const d = c->GetExactDist2d(mob);
                                    nearestAttacker < 0.0f || d < nearestAttacker)
                                {
                                    nearestAttacker = d;
                                    nearestName = c->GetName();
                                }
                    MovementGeneratorType const type = mob->GetDefaultMovementType();
                    float const reach = mob->GetAggroRange(bot) + bot->GetCombatReach() +
                                        (type == RANDOM_MOTION_TYPE ? mob->GetWanderDistance() : 0.0f);
                    float const homeGap = home.GetExactDist2d(fight.x, fight.y);
                    LOG_INFO("playerbots",
                             "[deaths139] {} L{} add {} L{} joins its judged fight on {} (attacker {} of {} judged, "
                             "{:.1f} s in): {}, home ({:.0f},{:.0f}) {:.0f} yd from the fight spot (reach {:.0f}: {}), "
                             "now {:.0f} yd from the bot, {:.0f} yd from {} (an attacker), its victim {}; [deaths148] "
                             "the bot {:.0f} yd from the fight spot, its home {:.0f} yd from the bot",
                             bot->GetName(), bot->GetLevel(), track.name, track.level, state.engage.targetName,
                             state.attackers.size(), state.engage.mobs,
                             float(getMSTimeDiff(state.firstMs, now)) / IN_MILLISECONDS,
                             type == RANDOM_MOTION_TYPE     ? "wanderer"
                             : type == WAYPOINT_MOTION_TYPE ? "patrol"
                                                            : "idle",
                             home.GetPositionX(), home.GetPositionY(), homeGap, reach,
                             homeGap <= reach ? "covers it" : "short of it", mob->GetExactDist2d(bot),
                             nearestAttacker, nearestName,
                             mob->GetVictim() ? mob->GetVictim()->GetName() : std::string("none"),
                             bot->GetExactDist2d(fight.x, fight.y), home.GetExactDist2d(bot));
                }
            }
            track.lastMs = now;
            track.damage += taken;
            // [deaths] hit in its melee reach for two of its swings without attacking anything (no victim, no cast):
            // the state of its target choice, once a fight (b337 Rumanatol L11 at The Lost Fold "melee 38.1 s dealt
            // 0", Laaral L13 five fights "never attacked" for 42-111 s; the site has no model between them)
            if (!state.loggedNoAttack && !state.actMs && !state.dealt && state.sampleMelee &&
                float(state.meleeMs) / IN_MILLISECONDS >= 2.0f * SwingSec(bot))
            {
                state.loggedNoAttack = true;
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                Unit* current = botAI ? botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get() : nullptr;
                char const* rejection = AttackersValue::PossibleTargetRejection(mob, bot);
                LOG_INFO("playerbots",
                         "[solo] {} L{} NO_ATTACK: {} L{} hits it in melee for {:.1f} s, no victim, no cast; engine "
                         "{}, current target {}, {} {:.1f} yd (z {:+.1f}), sight bot->mob {} mob->bot {}, valid "
                         "target {}{}{}, moving {}, at ({:.1f},{:.1f},{:.1f})",
                         bot->GetName(), bot->GetLevel(), mob->GetName(), mob->GetLevel(),
                         float(state.meleeMs) / IN_MILLISECONDS,
                         botAI ? (botAI->GetState() == BOT_STATE_COMBAT ? "combat" : "non-combat") : "-",
                         current ? current->GetName() : std::string("none"), mob->GetName(), bot->GetExactDist(mob),
                         mob->GetPositionZ() - bot->GetPositionZ(), bot->IsWithinLOSInMap(mob),
                         mob->IsWithinLOSInMap(bot), AttackersValue::IsValidTarget(mob, bot),
                         rejection ? ", rejected: " : "", rejection ? rejection : "", bot->isMoving(),
                         bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            }
            return;
        }
        // the bot (its pet) deals it
        Creature* mob = victim->ToCreature();
        Player* bot = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!mob || mob->GetCharmerOrOwnerPlayerOrPlayerItself() || !SoloPlayerBot(bot))
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        OpenIfNeeded(bot, state, now, mob);
        state.lastMs = now;
        state.dealt += std::min(damage, mob->GetHealth());
        SampleMelee(bot, mob, state, now);  // [guild15]
        if (state.sampleMelee)
            state.dealtMelee += std::min(damage, mob->GetHealth());
    }

    void NoteHeal(Unit* healer, Unit* target, uint32 heal)
    {
        Player* bot = target ? target->ToPlayer() : nullptr;
        if (!bot || healer != target || !heal || !SoloPlayerBot(bot))
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        if (state.open)
            state.healed += std::min<uint32>(heal, bot->GetMaxHealth() - std::min(bot->GetHealth(),
                                                                                   bot->GetMaxHealth()));
    }

    KillRate::Own BotDps(Player* bot, Unit* target)
    {
        // [dpszero1] no bot means NO ESTIMATE, not a zero: the old `return 0.0f;` handed a caller a number it
        // could not tell from "it deals nothing", and the one caller then floored it to 1 dps (rule 7)
        if (!bot)
            return {};
        DpsEstimate::Result const own = OwnDamage(bot, target);
        return {own.estimated, own.dps};
    }

    Verdict Judge(Player* bot, Unit* target)
    {
        if (!target || !Applies(bot))
            return Verdict();
        return JudgeAlone(bot, target);
    }

    Verdict JudgeAlone(Player* bot, Unit* target)
    {
        Verdict verdict;
        if (!bot || !target || !GET_PLAYERBOT_AI(bot))
            return verdict;

        // where the fight stands with no pull spot: at the target for melee, at the bot's reach on its side for a
        // ranged bot
        Position spot = target->GetPosition();
        float const reach = PlayerbotAI::IsRanged(bot) ? Leader::Fight::LeaderSpellReach(bot) : 0.0f;
        float const gap = bot->GetExactDist2d(target);
        if (reach > 0.0f)
        {
            if (gap > reach && gap > 0.0f)
            {
                float const share = reach / gap;
                spot.Relocate(target->GetPositionX() + (bot->GetPositionX() - target->GetPositionX()) * share,
                              target->GetPositionY() + (bot->GetPositionY() - target->GetPositionY()) * share,
                              target->GetPositionZ());
            }
            else
                spot = bot->GetPosition();
        }
        std::vector<Creature*> idle;
        // [death470] A MOB BUSY SOMEWHERE ELSE IS STILL PART OF THIS FIGHT'S SURROUNDINGS. `!IsInCombat()` dropped it
        // from the count outright, and `[death462]` measured what that cost: of the 31 mobs it named as "reaches over
        // the spot left out" of a judged engage, 18 were in a fight of their own (the victim another bot of the guild)
        // and 8 were evading home - 26 of 31. They are not gone: the fight ends, they walk home, their home reach
        // covers the fight spot and they join. `[deaths139]`'s 141 adds say it from the other end - 60 had a home
        // whose reach "covers it", median 9.2 s into the fight. Over the whole priced corpus 118 of 4 920 fights had
        // more mobs come than were judged (2.4 %) - and that 2.4 % holds ALL FIVE of the fights that were lost.
        // The ground such a mob covers is its HOME's, never where it is being held: AggroDisc / AggroReach::Of.
        for (ObjectGuid const& guid :
             GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
            if (Creature* mob = ObjectAccessor::GetCreature(*bot, guid))
                if (mob != target && mob->IsAlive() && mob->IsHostileTo(bot) &&
                    mob->HasReactState(REACT_AGGRESSIVE))
                    idle.push_back(mob);
        // [deaths12] a wanderer whose home lies beyond the bot's sight walks its wander area into the fight's
        // assistance calls all the same (PullModel::Grow prices it by its home and wander distance): the judge looks
        // as far as the route search does (sight + the map's largest wander distance, at most the map's view).
        // Before, only the mobs in sight were in the model: b350 Ganna L11 (judged 2 Tunnel Rats, came 4), Ginde L6
        // (judged 1 Kobold Geomancer, came 2 with a Kobold Miner), Thamesh L10 (judged 2 Rockjaws, came 5) died to
        // wanderers "assists a chaser"
        // [deaths16] ... and round the target: a camp judged from afar has members beyond the bot's sight that stand
        // by the target (its aggro reach, its friends' assistance calls, its script's call for help), fought there all
        // the same. b355 Emdyl L10 judged a Stonesplinter Trogg from 97 yd ("judged 1 mobs"), the troggs and the
        // scout 14-25 yd from it stood 105-115 yd from Emdyl - came 2; Kianey L12 the same trogg, came 4, both died
        {
            float const sight = sPlayerbotAIConfig.SightDistance;
            float const wander = std::min(MaxWanderDistance(bot->GetMapId()), bot->GetMap()->GetVisibilityRange());
            Creature* const targetCreature = target->ToCreature();
            float const around =
                std::max(MAX_AGGRO_RADIUS, targetCreature ? ScriptHelpOf(targetCreature).callRadius : 0.0f) +
                std::max(0.0f, sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS)) + wander;
            float const range =
                std::min(std::max(sight + wander, gap + around), bot->GetMap()->GetVisibilityRange());
            // [death391] a patrol walks its beat into the fight from wherever it is now: looked for as far as the map
            // shows (b449 Laaral L23: a Rabid Shadowhide Gnoll patrol, home 32 yd off the fight spot, came 47 s in)
            float const look = bot->GetMap()->GetVisibilityRange();
            if (look > sight)
            {
                std::list<Unit*> units;
                Acore::AnyUnitInObjectRangeCheck check(bot, look);
                Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
                Cell::VisitObjects(bot, searcher, look);
                for (Unit* unit : units)
                {
                    Creature* mob = unit->ToCreature();
                    // [death470] a mob in a fight of its own or evading home belongs here too (the count above)
                    if (!mob || mob == target || !mob->IsAlive() || !mob->IsHostileTo(bot) ||
                        !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID() ||
                        mob->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC) ||
                        std::find(idle.begin(), idle.end(), mob) != idle.end())
                        continue;
                    // [death403] a mob in sight that the cached "possible targets" left out (not selectable for the
                    // bot then, not seen: its stealth) aggroes all the same: counted. Before, every mob within sight
                    // was taken as read from that list - b453 Ginde L19 at Longshore: "[death392] fights Murloc
                    // Warrior L15 inside the reach of Murloc Hunter L16, not among its 2 counted mobs", came 4
                    if (bot->GetExactDist2d(mob) <= sight)
                    {
                        static std::mutex missLogLock;  // a log cadence per bot and mob kind, no decision reads it
                        static std::unordered_set<uint64> missLogged;
                        bool fresh = false;
                        {
                            std::lock_guard<std::mutex> guard(missLogLock);
                            if (missLogged.size() > 65536)
                                missLogged.clear();
                            fresh = missLogged.insert(uint64(mob->GetEntry()) * 1000003ull +
                                                      bot->GetGUID().GetCounter())
                                        .second;
                        }
                        if (fresh)
                            LOG_INFO("playerbots", "[death403] {} L{} judges {} L{} with {} L{} ({:.0f} yd off), "
                                     "not among its possible targets",
                                     bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel(),
                                     mob->GetName(), mob->GetLevel(), bot->GetExactDist2d(mob));
                        idle.push_back(mob);
                        continue;
                    }
                    // [death391] a patrol by its forecast beat (PullPlanning::PatrolForecast, the leader's waypoint
                    // forecast): it joins when a step of it comes within the bot's sight or the target's surroundings
                    if (mob->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE)
                    {
                        std::vector<G3D::Vector3> beat;
                        float sampleSec = 0.0f;
                        if (PullPlanning::PatrolForecast(mob, beat, sampleSec) &&
                            std::any_of(beat.begin(), beat.end(),
                                        [&](G3D::Vector3 const& step)
                                        {
                                            return bot->GetExactDist2d(step.x, step.y) <= sight ||
                                                   target->GetExactDist2d(step.x, step.y) <= around;
                                        }))
                        {
                            idle.push_back(mob);
                            static std::mutex patrolLogLock;  // a log cadence per patrol kind, no decision reads it
                            static std::unordered_set<uint32> patrolLogged;
                            bool fresh = false;
                            if (bot->GetExactDist2d(mob) > range)
                            {
                                std::lock_guard<std::mutex> guard(patrolLogLock);
                                fresh = patrolLogged.insert(mob->GetEntry()).second;
                            }
                            if (fresh)
                                LOG_INFO("playerbots", "[death391] {} L{} judges {} L{} with patrol {} L{} ({:.0f} yd "
                                         "off): its beat comes by", bot->GetName(), bot->GetLevel(), target->GetName(),
                                         target->GetLevel(), mob->GetName(), mob->GetLevel(), bot->GetExactDist2d(mob));
                            continue;
                        }
                    }
                    if (bot->GetExactDist2d(mob) > range)
                        continue;  // as far as the wanderers and the target's surroundings reach
                    bool const wanders = mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE;
                    Position const& home = wanders ? mob->GetHomePosition() : mob->GetPosition();
                    float const roam = wanders ? mob->GetWanderDistance() : 0.0f;
                    // its wander area comes within the bot's sight (the fight is fought within it), or it stands
                    // within the target's surroundings
                    if ((wanders && home.GetExactDist2d(bot) - roam <= sight) ||
                        home.GetExactDist2d(target) - roam <= around)
                        idle.push_back(mob);
                }
            }
        }

        // [pullplan] user 2026-11-08: a player pulls the target away from its camp - from range (its spells, Throw /
        // Shoot) or with its body at the edge of the target's aggro reach - and fights where it stands. The fight
        // spot: outside every other idle mob's aggro reach (wander area, patrol beat), and where the fewest of them
        // answer the fight's calls (the periodic assistance call there and on the target's run to it, its script's
        // call for help), in sight of the target, walked to round those reaches. The judge then counts exactly what
        // the core brings there (PullModel): the linked pack (formation, engage calls) and what that spot cannot avoid
        std::vector<Unit*> pack;
        Creature* targetMob = target->ToCreature();
        if (targetMob)
        {
            PMON_SCOPE(pullScope, PERF_MON_VALUE, "solo pull spot");
            PullModel model(bot, targetMob, idle);
            model.Links();
            size_t const linked = model.pack.size();
            float const pullReach = PullReach(bot);
            bool const comes = targetMob->IsHostileTo(bot) && targetMob->HasReactState(REACT_AGGRESSIVE);
            G3D::Vector3 const home(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            // a target beyond sight: the spot is a forecast (the judge of a camp ahead); its walk is searched near it
            bool const farTarget = gap > sPlayerbotAIConfig.SightDistance;
            G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            if (!target->IsInCombat() && (pullReach > 0.0f || comes))
            {
                bool searched = false;
                if (!wallJudge)  // [wall17]
                {
                    std::lock_guard<std::mutex> guard(lock);
                    SoloBotState const& state = states[bot->GetGUID().GetCounter()];
                    // the search holds while the bot stands where it searched, or walks on to the spot it found
                    if (state.searchTarget == target->GetGUID() && (state.searchRouted || farTarget) &&
                        ((state.searchFrom - from).length() <= bot->GetCombatReach() ||
                         (state.searchFound &&
                          (state.searchSpot - from).length() <= (state.searchSpot - state.searchFrom).length())))
                    {
                        searched = true;
                        verdict.pullSpot = state.searchFound;
                        verdict.pull = state.searchSpot;
                        verdict.pullAvoid = state.searchAvoid;
                    }
                }
                if (!searched)
                {
                    // the reaches the pull must not wake (the linked pack comes anyway)
                    std::vector<G3D::Vector4> reaches;
                    struct Answer
                    {
                        Creature* mob;
                        bool linked;   // answers a linked mob's periodic call
                        bool target;   // answers the target's
                        float script;  // the largest script call for help of a linked mob it answers
                    };
                    std::vector<Answer> answers;
                    for (Creature* c : idle)
                    {
                        if (model.Has(c))
                            continue;
                        G3D::Vector4 const disc = AggroDisc(c, bot);
                        reaches.push_back(disc);
                        G3D::Vector3 last(disc.x, disc.y, disc.z);
                        for (G3D::Vector3 const& step : model.Beat(c))
                            if ((step - last).length() >= disc.w)  // a disc per radius of its beat: the beat covered
                            {
                                reaches.emplace_back(step.x, step.y, step.z, disc.w);
                                last = step;
                            }
                        Answer answer{c, false, c->CanAssistTo(targetMob, bot), 0.0f};
                        for (size_t i = 0; i < linked; ++i)
                            if (c->CanAssistTo(model.pack[i].mob, bot))
                            {
                                answer.linked = true;
                                answer.script = std::max(answer.script, ScriptHelpOf(model.pack[i].mob).callRadius);
                            }
                        if (answer.linked || answer.target)
                            answers.push_back(answer);
                    }
                    auto const outside = [&reaches](G3D::Vector3 const& at)
                    {
                        return std::none_of(reaches.begin(), reaches.end(),
                                            [&at](G3D::Vector4 const& r) { return InDisc(r, at); });
                    };
                    float const period =
                        float(sWorld->getIntConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD)) / float(IN_MILLISECONDS);
                    float const run = target->GetSpeed(MOVE_RUN) * period;
                    auto const firstCallOf = [&](G3D::Vector3 const& at, G3D::Vector3& call)
                    {
                        G3D::Vector3 const way = at - home;
                        float const length = way.length();
                        if (length <= run || length <= 0.0f)
                            return false;
                        call = home + way * (run / length);
                        return true;
                    };
                    // how many idle mobs the fight at `at` calls
                    auto const answering = [&](G3D::Vector3 const& at)
                    {
                        G3D::Vector3 call;
                        bool const early = firstCallOf(at, call);
                        uint32 count = 0;
                        for (Answer const& a : answers)
                        {
                            // [regress10] the nearest its wander area / beat brings it during the fight's calls
                            float gap2d = a.mob->GetExactDist2d(at.x, at.y);
                            if (a.mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                                gap2d = std::min(gap2d, std::max(0.0f, a.mob->GetHomePosition().GetExactDist2d(
                                                                           at.x, at.y) -
                                                                           a.mob->GetWanderDistance()));
                            for (G3D::Vector3 const& step : model.Beat(a.mob))
                                gap2d = std::min(gap2d, std::hypot(step.x - at.x, step.y - at.y));
                            if ((a.linked && (gap2d <= model.assist || gap2d <= a.script)) ||
                                (early && a.target && a.mob->GetExactDist2d(call.x, call.y) <= model.assist))
                                ++count;
                        }
                        return count;
                    };
                    struct Candidate
                    {
                        uint32 calls;
                        float walk;
                        G3D::Vector3 at;
                    };
                    std::vector<Candidate> candidates;
                    // where it stands, when it pulls from range from there
                    if (pullReach > 0.0f && bot->IsWithinDistInMap(target, pullReach) && outside(from))
                        candidates.push_back({answering(from), 0.0f, from});
                    // the rings it pulls from: its pull reach, the target's aggro edge (a body pull)
                    std::vector<float> rings;
                    if (pullReach > bot->GetCombatReach())
                        rings.push_back(pullReach - bot->GetCombatReach());
                    if (comes)
                        if (float const edge = targetMob->GetAggroRange(bot) - bot->GetCombatReach();
                            edge > 0.0f && (rings.empty() || std::fabs(edge - rings.front()) > bot->GetCombatReach()))
                            rings.push_back(edge);
                    for (float const ring : rings)
                    {
                        // one spot per stretch of the ring as wide as the bot's reach
                        uint32 const arcs = std::max<uint32>(
                            8, uint32(std::ceil(2.0f * float(M_PI) * ring / std::max(bot->GetCombatReach(), 1.0f))));
                        for (uint32 i = 0; i < arcs; ++i)
                        {
                            float const angle = 2.0f * float(M_PI) * float(i) / float(arcs);
                            G3D::Vector3 const at(home.x + std::cos(angle) * ring, home.y + std::sin(angle) * ring,
                                                  home.z);
                            if (outside(at))
                                candidates.push_back({answering(at), (at - from).length(), at});
                        }
                    }
                    std::sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b)
                              { return a.calls != b.calls ? a.calls < b.calls : a.walk < b.walk; });
                    // the walk there round every such reach (a reach the bot stands in lies behind it), the target's
                    // own too for a pull from beyond it; the polygons' edges are tested against the discs as the solo
                    // route search tests them (penalties at no cost)
                    std::vector<G3D::Vector4> avoid;
                    std::vector<Leader::Travel::RoutePenalty> edges;
                    auto const avoidDisc = [&](G3D::Vector4 const& r)
                    {
                        if (InDisc(r, from))
                            return;
                        avoid.push_back(r);
                        Leader::Travel::RoutePenalty edge;
                        edge.x = r.x;
                        edge.y = r.y;
                        edge.z = r.z;
                        edge.r = r.w;
                        edges.push_back(edge);
                    };
                    for (G3D::Vector4 const& r : reaches)
                        avoidDisc(r);
                    G3D::Vector4 const targetDisc = AggroDisc(targetMob, bot);
                    float const height = bot->GetCollisionHeight();
                    constexpr size_t PULL_SPOT_ROUTE_TRIES = 3;  // search cost bound (the best spots), not a rule
                    size_t routed = 0;
                    for (Candidate& c : candidates)
                    {
                        if (routed >= PULL_SPOT_ROUTE_TRIES)
                            break;
                        if (c.walk > bot->GetCombatReach())
                            bot->UpdateAllowedPositionZ(c.at.x, c.at.y, c.at.z);
                        if (!bot->GetMap()->isInLineOfSight(c.at.x, c.at.y, c.at.z + height, home.x, home.y,
                                                            home.z + height, bot->GetPhaseMask(),
                                                            LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                            continue;
                        if (c.walk <= bot->GetCombatReach() || farTarget)
                        {
                            verdict.pullSpot = true;
                            verdict.pull = c.at;
                            break;
                        }
                        ++routed;
                        std::vector<G3D::Vector4> walkAvoid = avoid;
                        std::vector<Leader::Travel::RoutePenalty> walkEdges = edges;
                        if (!InDisc(targetDisc, c.at) && !InDisc(targetDisc, from))
                        {
                            walkAvoid.push_back(targetDisc);
                            Leader::Travel::RoutePenalty edge;
                            edge.x = targetDisc.x;
                            edge.y = targetDisc.y;
                            edge.z = targetDisc.z;
                            edge.r = targetDisc.w;
                            walkEdges.push_back(edge);
                        }
                        Movement::PointsArray corners;
                        bool complete = false;
                        if (!Leader::Travel::LongRoute(bot, from, c.at, false, corners, complete, nullptr, &walkAvoid,
                                                       0.0f, nullptr, &walkEdges) ||
                            !complete || corners.empty())
                            continue;
                        verdict.pullSpot = true;
                        verdict.pull = corners.back();
                        verdict.pullAvoid = std::move(walkAvoid);
                        break;
                    }
                    if (!wallJudge)  // [wall17] the engage's search stays the bot's own
                    {
                        std::lock_guard<std::mutex> guard(lock);
                        SoloBotState& state = states[bot->GetGUID().GetCounter()];
                        state.searchTarget = target->GetGUID();
                        state.searchFrom = from;
                        state.searchFound = verdict.pullSpot;
                        state.searchRouted = !farTarget;
                        state.searchSpot = verdict.pull;
                        state.searchAvoid = verdict.pullAvoid;
                    }
                }

            }
            G3D::Vector3 fight(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
            G3D::Vector3 firstCall;
            bool early = false;
            if (verdict.pullSpot)
            {
                fight = verdict.pull;
                spot.Relocate(fight.x, fight.y, fight.z);
                G3D::Vector3 const way = fight - home;
                float const run = target->GetSpeed(MOVE_RUN) *
                                  float(sWorld->getIntConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD)) /
                                  float(IN_MILLISECONDS);
                if (way.length() > run && way.length() > 0.0f)
                {
                    firstCall = home + way * (run / way.length());
                    early = true;
                }
            }
            model.Grow(&fight, early ? &firstCall : nullptr);
            for (PullModel::Member const& m : model.pack)
                pack.push_back(m.mob);
            verdict.fightAt = fight;  // [death462]
            verdict.fightAtSet = true;
        }
        else
            pack.push_back(target);
        for (Unit* attacker : bot->getAttackers())
            if (attacker && attacker->IsAlive() && std::find(pack.begin(), pack.end(), attacker) == pack.end())
                pack.push_back(attacker);
        // [death388] each one's summons fight with it (Unit::m_Controlled: a Blackrock Summoner's Imp Minion from its
        // out-of-combat Summon Imp) - b449 Laaral L23 died to an Imp Minion it had not counted
        for (size_t i = 0, owners = pack.size(); i < owners; ++i)
            for (Unit* summon : pack[i]->m_Controlled)
                if (summon && summon->IsAlive() && summon->IsHostileTo(bot) &&
                    std::find(pack.begin(), pack.end(), summon) == pack.end())
                {
                    pack.push_back(summon);
                    static std::mutex summonLogLock;  // a log cadence per owner kind, no decision reads it
                    static std::unordered_set<uint32> summonLogged;
                    bool fresh = false;
                    {
                        std::lock_guard<std::mutex> guard(summonLogLock);
                        fresh = summonLogged.insert(pack[i]->GetEntry()).second;
                    }
                    if (fresh)
                        LOG_INFO("playerbots", "[death388] {} L{}: {} L{} counts its summon {} L{} in the fight",
                                 bot->GetName(), bot->GetLevel(), pack[i]->GetName(), pack[i]->GetLevel(),
                                 summon->GetName(), summon->GetLevel());
                }

        struct Mob
        {
            Unit* unit;
            float hp;
            float dps;
            float variance = 0.0f;  // [death386] of its melee per second of the fight
            bool summoned = false;  // [death449] a summon to come: `unit` is its summoner
        };
        std::vector<Mob> mobs;
        bool elite = false;
        verdict.at = target->GetPosition();
        {
            std::lock_guard<std::mutex> guard(lock);
            for (Unit* unit : pack)
            {
                float dps = MobDps(unit, bot);  // [guild14] crits, SmartAI casts; [dps1] no measurement
                if (Creature* creature = unit->ToCreature())
                {
                    elite = elite || IsElite(creature);
                    // [death470] a member HELD away from home by a fight of its own is measured to the ground its
                    // aggro covers (its home), not to the tick it is being held on - otherwise admitting such a
                    // member to the count would stretch the pack's radius over every yard it was dragged. A free
                    // mob is measured exactly where it was before. The radius names the pack for
                    // NewRpgCoop::judgedPulls and for engage.safeRadius, so it must not grow by itself
                    bool const held = creature->IsInCombat() || creature->IsInEvadeMode();
                    G3D::Vector4 const ground = held ? AggroDisc(creature, bot)
                                                     : G3D::Vector4(unit->GetPositionX(), unit->GetPositionY(),
                                                                    unit->GetPositionZ(), 0.0f);
                    verdict.radius = std::max(verdict.radius, target->GetExactDist2d(ground.x, ground.y) +
                                                                  creature->GetAggroRange(bot));
                }
                if (unit->GetLevel() > bot->GetLevel())
                    ++verdict.above;
                verdict.topLevel = std::max<uint8>(verdict.topLevel, unit->GetLevel());
                float variance = 0.0f;
                MeleeDps(unit, bot, &variance);  // [death386] its swings' spread
                if (Creature* caster = unit->ToCreature())
                    variance += SpellVarianceOf(caster);  // [death390] its casts' rolls
                mobs.push_back({unit, float(unit->GetHealth()), dps, variance});
            }
        }
        verdict.mobs = mobs.size();
        for (Mob const& mob : mobs)
            verdict.counted.push_back(mob.unit->GetGUID());  // [death392]

        SoloBotState snapshot;
        {
            std::lock_guard<std::mutex> guard(lock);
            SoloBotState& state = states[bot->GetGUID().GetCounter()];
            CloseIfOver(bot, state);
            // [guild15] user 2026-11-07: a mob it lost to is no veto by itself (a player kills yellow +1 / +2 mobs
            // solo all the time): only the projection refuses
            BestHeal(bot, state);
            snapshot.heal = state.heal;
            snapshot.fightSecSum = state.fightSecSum;
            snapshot.healedSum = state.healedSum;
        }
        // [dps1] its damage from the formula against the target (user 2026-11-08: no fight history): its rotation
        // while its mana lasts, then what lasts without mana
        DpsEstimate::Result const own = OwnDamage(bot, target);
        // [dpszero1] its own damage with NO invented floor, in the three states a reader owes it (KillRate.h).
        // NO ESTIMATE: nobody ran the formula, so no verdict about this fight may rest on a number - the judge
        // answers nothing, which is what it already answers when it does not apply. A MEASURED ZERO: it deals
        // nothing, so no health it has makes the fight winnable; refused, and no fight is priced because there is
        // no fight that ends. Everything below is the honest reading, a hundredth of a dps included.
        KillRate::Own const burst{own.estimated, own.burstDps};
        switch (KillRate::StateOf(burst))
        {
            case KillRate::State::NoEstimate:
                return verdict;
            case KillRate::State::DealsNothing:
                verdict.kind = Verdict::Kind::Refuse;
                verdict.why = "its own damage is measured at zero: the fight has no end to price";
                return verdict;
            case KillRate::State::Deals:
                break;
        }
        float const botDps = own.burstDps;
        // [deaths] each mob's health with the heals it casts on itself over the time the bot needs for it
        for (Mob& mob : mobs)
            if (Creature* creature = mob.unit->ToCreature())
                mob.hp += HealToCome(creature, KillRate::KillSec(burst, mob.hp));
        // [death388] ... and the heals its friends cast on it (SMART_EVENT_FRIENDLY_HEALTH: a Greymist Oracle's Healing
        // Wave on a hurt friend), on the one the bot fights first, over the time the bot needs for the pack
        if (!mobs.empty())
        {
            float packHp = 0.0f;
            for (Mob const& mob : mobs)
                packHp += mob.hp;
            for (Mob const& mob : mobs)
                if (Creature* creature = mob.unit->ToCreature())
                    mobs.front().hp += FriendHealToCome(creature, KillRate::KillSec(burst, packHp));
        }

        // [death449] the creatures the pack's scripts summon in the fight: each its template's health at its level,
        // its melee against the bot as its summoner's scaled by the two templates' melee (CreatureBaseStats)
        {
            auto const templateMelee = [](CreatureTemplate const* tmpl, float& health) -> float
            {
                CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(tmpl->maxlevel, tmpl->unit_class);
                if (!stats)
                    return 0.0f;
                health = float(stats->GenerateHealth(tmpl));
                float const swing = std::max(float(tmpl->BaseAttackTime), 1.0f) / float(IN_MILLISECONDS);
                float const base = stats->GenerateBaseDamage(tmpl);
                return ((base + base * 1.5f) / 2.0f + float(stats->AttackPower) / 14.0f * swing) *
                       tmpl->DamageModifier / swing;
            };
            size_t const owners = mobs.size();
            for (size_t i = 0; i < owners; ++i)
            {
                Creature* summoner = mobs[i].unit->ToCreature();
                if (!summoner || summoner->IsSummon())
                    continue;
                float ownHealth = 0.0f;
                float const ownMelee = templateMelee(summoner->GetCreatureTemplate(), ownHealth);
                for (uint32 entry : ScriptHelpOf(summoner).summons)
                {
                    CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
                    float health = 0.0f;
                    float const melee = tmpl ? templateMelee(tmpl, health) : 0.0f;
                    if (health <= 0.0f)
                        continue;
                    Mob summon{mobs[i].unit, health, ownMelee > 0.0f ? mobs[i].dps * melee / ownMelee : mobs[i].dps,
                               mobs[i].variance, true};
                    mobs.push_back(summon);
                    ++verdict.mobs;
                    if (tmpl->maxlevel > bot->GetLevel())
                        ++verdict.above;
                    static std::mutex summonedLogLock;  // a log cadence per summoner kind, no decision reads it
                    static std::unordered_set<uint32> summonedLogged;
                    bool fresh = false;
                    {
                        std::lock_guard<std::mutex> guard(summonedLogLock);
                        fresh = summonedLogged.insert(summoner->GetEntry()).second;
                    }
                    if (fresh)
                        LOG_INFO("playerbots", "[death449] {} L{}: {} L{} summons {} L{} in its fight ({:.0f} hp, "
                                 "{:.1f} dps): counted",
                                 bot->GetName(), bot->GetLevel(), summoner->GetName(), summoner->GetLevel(),
                                 tmpl->Name, tmpl->maxlevel, health, summon.dps);
                }
            }
        }

        // [death463] a spawn killed earlier respawns at its home (Creature::Update at the map's respawn time of its DB
        // guid, Map::GetCreatureRespawnTime) - one whose home reach covers the fight spot and whose respawn falls
        // within the walk there and the fight joins it. Its template's health and melee as a [death449] summon's, scaled
        // by the target's. The respawn time is the map's state, no timer of the bot's
        if (verdict.fightAtSet && !mobs.empty() && target->ToCreature())
        {
            Creature* targetMob = target->ToCreature();
            Map* map = bot->GetMap();
            FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
            CreatureBaseStats const* targetStats = sObjectMgr->GetCreatureBaseStats(
                targetMob->GetCreatureTemplate()->maxlevel, targetMob->GetCreatureTemplate()->unit_class);
            auto const templateMelee = [](CreatureTemplate const* tmpl, CreatureBaseStats const* stats) -> float
            {
                if (!stats)
                    return 0.0f;
                float const swing = std::max(float(tmpl->BaseAttackTime), 1.0f) / float(IN_MILLISECONDS);
                float const base = stats->GenerateBaseDamage(tmpl);
                return ((base + base * 1.5f) / 2.0f + float(stats->AttackPower) / 14.0f * swing) *
                       tmpl->DamageModifier / swing;
            };
            float const ownMelee = templateMelee(targetMob->GetCreatureTemplate(), targetStats);
            float packHp = 0.0f;
            for (Mob const& mob : mobs)
                packHp += mob.hp;
            G3D::Vector3 const& at = verdict.fightAt;
            float const walk = std::hypot(bot->GetPositionX() - at.x, bot->GetPositionY() - at.y) /
                               std::max(bot->GetSpeed(MOVE_RUN), 0.1f);
            float const window = walk + KillRate::KillSec(burst, packHp);
            time_t const now = GameTime::GetGameTime().count();
            float const reachMax = MAX_AGGRO_RADIUS + MaxWanderDistance(bot->GetMapId());
            std::set<uint32> grids;
            for (float ox : {-reachMax, reachMax})
                for (float oy : {-reachMax, reachMax})
                    grids.insert(Acore::ComputeGridCoord(at.x + ox, at.y + oy).GetId());
            for (uint32 const gridId : grids)
            {
                CellObjectGuids const& cell =
                    sObjectMgr->GetGridObjectGuids(bot->GetMapId(), map->GetSpawnMode(), gridId);
                for (ObjectGuid::LowType const spawnId : cell.creatures)
                {
                    CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
                    if (!data || (data->phaseMask && !(data->phaseMask & bot->GetPhaseMask())))
                        continue;
                    float const gap = std::hypot(data->posX - at.x, data->posY - at.y);
                    if (gap > reachMax)
                        continue;
                    time_t const respawn = map->GetCreatureRespawnTime(spawnId);
                    if (respawn <= now || float(respawn - now) > window)
                        continue;
                    CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data->id);
                    if (!tmpl || tmpl->detection_range < 1.0f || (tmpl->unit_flags & UNIT_FLAG_IMMUNE_TO_PC) ||
                        (data->unit_flags & UNIT_FLAG_IMMUNE_TO_PC))
                        continue;
                    FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(tmpl->faction);
                    if (!botFaction || !faction || !faction->IsHostileTo(*botFaction))
                        continue;
                    // Creature::GetAggroRange from its template: the detection range a yard per level above the bot
                    float const reach = std::clamp(tmpl->detection_range + float(int32(tmpl->maxlevel) -
                                                                                 int32(bot->GetLevel())),
                                                   1.0f, MAX_AGGRO_RADIUS) +
                                        bot->GetCombatReach() +
                                        (data->movementType == RANDOM_MOTION_TYPE ? data->wander_distance : 0.0f);
                    if (gap > reach)
                        continue;
                    CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(tmpl->maxlevel, tmpl->unit_class);
                    if (!stats)
                        continue;
                    float const health = float(stats->GenerateHealth(tmpl));
                    float const melee = templateMelee(tmpl, stats);
                    Mob spawned{target, health,
                                ownMelee > 0.0f ? mobs.front().dps * melee / ownMelee : mobs.front().dps,
                                mobs.front().variance, true};
                    mobs.push_back(spawned);
                    ++verdict.mobs;
                    ++verdict.respawns;
                    if (tmpl->maxlevel > bot->GetLevel())
                        ++verdict.above;
                    elite = elite || tmpl->rank == CREATURE_ELITE_ELITE || tmpl->rank == CREATURE_ELITE_RAREELITE ||
                            tmpl->rank == CREATURE_ELITE_WORLDBOSS;
                    static std::mutex respawnLogLock;  // a log cadence per bot and spawn, no decision reads it
                    static std::unordered_set<uint64> respawnLogged;
                    bool fresh = false;
                    {
                        std::lock_guard<std::mutex> guard(respawnLogLock);
                        if (respawnLogged.size() > 65536)
                            respawnLogged.clear();
                        fresh = respawnLogged.insert(uint64(spawnId) * 1000003ull + bot->GetGUID().GetCounter())
                                    .second;
                    }
                    if (fresh)
                        LOG_INFO("playerbots", "[death463] {} L{} on {} L{}: {} L{} (spawn {}) respawns {} s from now "
                                 "{:.0f} yd from the fight spot (reach {:.0f}), within its walk and fight ({:.0f} s): "
                                 "counted",
                                 bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel(), tmpl->Name,
                                 tmpl->maxlevel, spawnId, int64(respawn - now), gap, reach, window);
                }
            }
        }

        // the kill order: the target first, then the most damage per health
        std::sort(mobs.begin() + 1, mobs.end(),
                  [](Mob const& a, Mob const& b)
                  { return a.dps / std::max(a.hp, 1.0f) > b.dps / std::max(b.hp, 1.0f); });

        // [deaths3] its pet's health takes hits only as far as the pet holds a mob: a pet with a taunt (Growl,
        // Torment: SPELL_EFFECT_ATTACK_ME / SPELL_AURA_MOD_TAUNT) holds its target, the first of the kill order; the
        // rest hit the bot (b341 Blahadrin L12: a Voidwalker's 390 health in the judge's ehp, "3 mobs, 497 dmg" engaged
        // at hp 126 - every mob hit the warlock, it died)
        float petHealth = 0.0f;
        if (Pet* pet = bot->GetPet())
            if (pet->IsAlive() && PetTaunts(pet))
                petHealth = float(pet->GetHealth());
        // [guild15] its damage lasts as long as its mana: the rotation's mana per second (the formula's spend less its
        // in-combat regeneration) runs its spells, its weapon's white damage (and its pet's) is what is left when the
        // mana is gone (b307/b308: priests pulled at 7-25 % mana judged on their full-mana damage and died)
        float const manaPerSec = own.manaPerSec;
        // [dpszero1] what lasts with no mana: never above the burst, and never floored either. A bot whose damage
        // does not outlive its mana is one whose fight does not end, and the projection says so by not ending it
        KillRate::Own const white{own.estimated, std::min(own.whiteDps, own.burstDps)};
        float const whiteDps = white.dps;
        struct Projection
        {
            float seconds = 0.0f;
            float damage = 0.0f;
            float targetDamage = 0.0f;  // the target's part (a taunting pet's to take)
            float variance = 0.0f;      // [death386] of the damage: the mobs' swings over their seconds alive
            // [dpszero1] the projection never reached the end of the pack: what lasts without mana kills nothing,
            // so a mob is left standing however long the fight runs. Not a number - the absence of one
            bool unending = false;
        };
        // [death390] the bot's own spread: the kill time of each mob varies with its hits' crit (x2 melee, x1.5 a
        // spell) and miss shares - a renewal of one hit per swing / global cooldown: the variance of a kill of T
        // seconds is T x relative variance of a hit x its period, and the mobs alive meanwhile hit on for it
        float killVariancePerSec = 0.0f;
        {
            bool const caster = PlayerbotAI::IsCaster(bot);
            float crit = 0.0f;
            if (caster)
                for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
                    crit = std::max(crit, bot->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + school));
            else
                crit = bot->GetFloatValue(PLAYER_CRIT_PERCENTAGE);
            int32 const above = std::max<int32>(0, int32(target->GetLevel()) - int32(bot->GetLevel()));
            // the 3.3.5 hit tables against a mob: spells 4 / 5 / 6 / 17 % at +0..+3; melee 5 % miss and 5 % dodge,
            // 0.5 % more each per level above
            float const miss = caster ? (above < 3 ? 4.0f + float(above) : 17.0f + 11.0f * float(above - 3))
                                      : 10.0f + 1.0f * float(above);
            float const hitShare = std::clamp(1.0f - miss / 100.0f, 0.01f, 1.0f);
            float const critShare = std::clamp(crit / 100.0f, 0.0f, 1.0f);
            float const multiplier = caster ? 1.5f : 2.0f;
            float const mean = hitShare * (1.0f + critShare * (multiplier - 1.0f));
            float const second = hitShare * (1.0f + critShare * (multiplier * multiplier - 1.0f));
            float const period =
                caster ? 1.5f : std::max(float(bot->GetAttackTime(BASE_ATTACK)) / float(IN_MILLISECONDS), 0.5f);
            killVariancePerSec = std::max(second / (mean * mean) - 1.0f, 0.0f) * period;
        }
        // [death457] the casts' pushback under every mob still alive (not the target's hits alone) and the mana the
        // rotation spends at that pace: each kill of the order at its own rate. b467 Sylaenas L17 balance judged 2
        // Axxarien mobs on the Hellcaller's hits alone ("321 dmg"), both hit her casts back, took 475, died
        std::vector<std::pair<float, float>> phases(mobs.size(), {botDps, manaPerSec});  // (dps, mana a second)
        if (mobs.size() > 1)
        {
            DpsEstimate::BuffContext const ctx = DpsEstimate::ContextOf(bot);
            float later = 0.0f;  // the hits of the mobs after the target, summed from the order's end
            for (size_t i = mobs.size(); i-- > 0;)
            {
                if (i > 0)
                    later += DpsEstimate::MeleeHitsPerSec(bot, mobs[i].unit);
                float const hits = (i == 0 ? own.pushHitsPerSec : 0.0f) + later;
                DpsEstimate::Result const at = DpsEstimate::Estimate(bot, target, ctx, 0.0f, hits);
                // [dpszero1] the pushed-back rotation's own reading, floor-free: a zero it MEASURED is kept as a
                // zero (the projection below then leaves the mob to what lasts without mana). Only an estimate
                // that did not run at all leaves the phase at the judge's own reading, which did run - never 1.0
                KillRate::Own const pushed{at.estimated, at.burstDps};
                if (KillRate::Judgeable(pushed))
                    phases[i] = {pushed.dps, at.manaPerSec};
            }
            thread_local std::unordered_set<uint64> pushLogged;  // a log cadence per bot, target kind and pack size
            if (pushLogged.size() > 65536)
                pushLogged.clear();
            if (pushLogged.insert((uint64(bot->GetGUID().GetCounter()) * 1000003ull + target->GetEntry()) * 31ull +
                                  mobs.size())
                    .second)
                LOG_INFO("playerbots", "[death457] {} L{} on {} L{} ({} mobs): its casts pushed back by every mob on "
                         "it - {:.1f} dps (mana {:.2f}/s) against {:.1f} on its target's hits alone",
                         bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel(), mobs.size(),
                         phases.front().first, phases.front().second, botDps);
        }
        auto const project = [&](float mana) -> Projection
        {
            bool const spends = bot->GetMaxPower(POWER_MANA) != 0;
            Projection out;
            float aliveDps = 0.0f;
            for (Mob const& mob : mobs)
                aliveDps += mob.dps;
            for (size_t i = 0; i < mobs.size(); ++i)
            {
                Mob const& mob = mobs[i];
                float const started = out.seconds;
                float hp = mob.hp;
                auto const [phaseDps, phaseMana] = phases[i];
                float const spellSec = spends && phaseMana > 0.0f ? mana / phaseMana
                                                                  : std::numeric_limits<float>::max();
                // [dpszero1] a phase that deals nothing spends no seconds and no mana: the mob is left to what
                // lasts without mana below, instead of to a floor that would have killed it
                if (spellSec > 0.0f && phaseDps > 0.0f)
                {
                    float const spellPart = spellSec * phaseDps;
                    float const spent = hp <= spellPart ? hp / phaseDps : spellSec;
                    out.seconds += spent;
                    hp = std::max(0.0f, hp - spellPart);
                    if (spends && phaseMana > 0.0f)
                        mana = std::max(0.0f, mana - spent * phaseMana);
                }
                if (hp > 0.0f)
                {
                    // [dpszero1] no damage of its outlives its mana: this mob never falls, and the fight that
                    // follows is unpriceable rather than long. Said once, here, instead of a floor on the dps
                    if (KillRate::StateOf(white) != KillRate::State::Deals)
                    {
                        out.unending = true;
                        return out;
                    }
                    out.seconds += KillRate::KillSec(white, hp);
                }
                out.damage += mob.dps * out.seconds;
                out.variance += mob.variance * out.seconds;
                // [death390] its kill time's spread, under every mob still alive
                out.variance += aliveDps * aliveDps * killVariancePerSec * (out.seconds - started);
                aliveDps -= mob.dps;
                if (&mob == &mobs.front())
                    out.targetDamage = mob.dps * out.seconds;
            }
            return out;
        };
        Projection const now = project(float(bot->GetPower(POWER_MANA)));
        Projection const atFull = project(float(bot->GetMaxPower(POWER_MANA)));
        float const seconds = now.seconds;
        float const damage = now.damage;
        verdict.damage = damage;
        verdict.fightSec = seconds;
        // [deaths3] user 2026-11-08 (no fight history in decisions): its heals are what its kit holds now - each heal
        // item once, its spell heals as far as the mana its rotation leaves pays for them (SurvivalKit::HealPoints),
        // not the heals measured over its past fights (b341 Ravelandy L9: past potions read as 1.4 hp/s)
        constexpr float HIGH_END_DEVIATIONS = 2.0f;  // a one-sided 97.7 % bound of a sum of independent swings
        std::vector<SurvivalKit::Interrupter> interrupters;  // [death444]
        for (Mob const& mob : mobs)
            if (Creature* creature = mob.summoned ? nullptr : mob.unit->ToCreature())
                for (SurvivalKit::Interrupter const& in : InterruptsOf(creature))
                    interrupters.push_back(in);
        auto const effective = [&](Projection const& fight, float health, float mana, bool& wins) -> float
        {
            // [death442] its heals as its play pays them: the rotation's mana first, the kit's heals as they fall due
            // under the high end of the hits (b462 Septimon L17: 990 heal points from the mana its rotation spent)
            float const high = fight.damage + HIGH_END_DEVIATIONS * std::sqrt(fight.variance);
            float const heals = SurvivalKit::FightHeals(bot, fight.seconds, health, mana,
                                                        bot->GetMaxPower(POWER_MANA) ? manaPerSec : 0.0f,
                                                        high / std::max(fight.seconds, 1.0f), true, &interrupters);
            float const ehp = health + std::min(petHealth, fight.targetDamage) + heals;
            // [death386] user 2026-11-12 "survives with a margin": the high end of its rolls, not their mean - two
            // standard deviations of the mobs' swings (weapon roll, miss / dodge / parry, crit, crush) over the fight
            // [dpszero1] a fight the projection could not end is not a fight it won
            wins = !fight.unending && high < ehp;
            return ehp;
        };
        bool winNow = false;
        bool winFull = false;
        verdict.ehpNow = effective(now, float(bot->GetHealth()), float(bot->GetPower(POWER_MANA)), winNow);
        verdict.ehpFull =
            effective(atFull, float(bot->GetMaxHealth()), float(bot->GetMaxPower(POWER_MANA)), winFull);
        if (!winFull && atFull.damage < verdict.ehpFull)
        {
            static std::mutex marginLogLock;  // a log cadence per bot and target kind, no decision reads it
            static std::unordered_set<uint64> marginLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(marginLogLock);
                if (marginLogged.size() > 65536)
                    marginLogged.clear();
                fresh = marginLogged.insert(uint64(target->GetEntry()) * 1000003ull + bot->GetGUID().GetCounter())
                            .second;
            }
            if (fresh)
                LOG_INFO("playerbots", "[death386] {} L{} {} L{}: wins on the mean ({:.0f} dmg) but not at the high "
                         "end of its rolls ({:.0f} dmg, +2 sd) at ehp {:.0f} full",
                         bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel(), atFull.damage,
                         atFull.damage + HIGH_END_DEVIATIONS * std::sqrt(atFull.variance), verdict.ehpFull);
        }
        bool const full = bot->GetHealth() >= bot->GetMaxHealth() &&
                          (!bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA));

        // [mana90] AND THE MANA OVER THE WHOLE FIGHT, which nothing asked before. The heal count above is right for
        // the seconds it is given, and those seconds are the episode's: b502 Selalleas L26 was judged
        // "3 attackers 37.6 dps, 24 s ... 683 dmg to come at hp 479 (+175 heals)" - correct - and then fought 167.7 s,
        // healed itself 2628 against a body of 774 and died "lost out of mana to 5 attackers". Four of the hour's
        // eight deaths are that. The budget is the game's own numbers (ManaBudget::Of: the mana it has, what its
        // rotation spends, the mana combat gives back, and the health its own heal buys per point of mana) over the
        // judge's OWN estimate of the fight's length - no constant, no clock.
        // [dun230] the in-combat regen from the one place that computes it (ManaRegenPerSec, the core's own rate
        // under the five-second rule, less a drink aura's MP5 that breaks on standing up): no second copy here
        float const combatRegen = RestState::ManaRegenPerSec(bot, true);
        auto const manaBudget = [&](Projection const& fight, float health, float mana)
        {
            ManaBudget::Fight ask;
            ask.fightSec = fight.seconds;
            ask.incomingDps = fight.damage / std::max(fight.seconds, 1.0f);
            ask.health = health;
            ask.mana = mana;
            ask.manaPerSec = bot->GetMaxPower(POWER_MANA) ? manaPerSec : 0.0f;
            ask.regenPerSec = combatRegen;
            ask.healPerMana = SurvivalKit::HealPerMana(bot);
            return ManaBudget::Of(ask);
        };
        ManaBudget::Answer const manaNow = manaBudget(now, float(bot->GetHealth()), float(bot->GetPower(POWER_MANA)));
        ManaBudget::Answer const manaFull =
            manaBudget(atFull, float(bot->GetMaxHealth()), float(bot->GetMaxPower(POWER_MANA)));
        verdict.manaHoldsSec = manaNow.holdsSec;
        verdict.manaNeeded = manaNow.needMana;

        if (elite)
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = "an elite in the pack";
        }
        // the mana is a bound on the fight like the health is: short of it at full too, the fight is refused; short
        // of it only now, the bot drinks first (the same answer the health's own margin gives through RestFirst)
        else if (!manaNow.holds && !manaFull.holds)
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = Acore::StringFormat(
                "the mana holds {:.0f} s of a {:.0f} s fight ({:.0f} mana asked of {:.0f} at full)",
                manaFull.holdsSec, atFull.seconds, manaFull.needMana, float(bot->GetMaxPower(POWER_MANA)));
        }
        else if (!manaNow.holds)
        {
            verdict.kind = Verdict::Kind::RestFirst;
            verdict.why = Acore::StringFormat("the mana holds {:.0f} s of a {:.0f} s fight ({:.0f} asked of {:.0f}): "
                                              "at full mana only",
                                              manaNow.holdsSec, now.seconds, manaNow.needMana,
                                              float(bot->GetPower(POWER_MANA)));
        }
        // [lag353] user 2026-11-11: two or more mobs above its level are judged by the fight's numbers as one is, no
        // fixed refusal ("never two above at once"): the judge's margin for a mob above its level holds - fought only
        // from full health and mana and won there (its heals, potions and bandages in the ehp); a death in such a fight
        // is a bug of the estimate, fixed there. b446 Rollothan L14 refused 2 Riverpaw Herbalists L15, "495 dmg
        // expected over 41 s ... ehp 1900", and stood walled in by them
        // [death394] a win now is one at full too: the high end it fails at full health and mana fails now as well (b451
        // Kianey L20: "[death386] ... not at the high end (631 dmg, +2 sd) at ehp 625 full", engaged at ehp 646 now -
        // a heal count of its mana now - and died to 5 Redridge mobs)
        else if (winNow && winFull && (!verdict.above || full))
            verdict.kind = Verdict::Kind::Win;
        else if (winFull)
        {
            verdict.kind = Verdict::Kind::RestFirst;
            verdict.why = winNow ? "a mob above its level: at full health and mana only" : "wins only at full";
        }
        else
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = winNow ? "loses at full at the high end of its rolls" : "loses even at full";
        }
        if (verdict.above >= 2 && verdict.kind != Verdict::Kind::Refuse)
        {
            static std::mutex aboveLogLock;  // a log cadence per bot, target and verdict, no decision reads it
            static std::unordered_set<uint64> aboveLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(aboveLogLock);
                if (aboveLogged.size() > 65536)
                    aboveLogged.clear();
                fresh = aboveLogged
                            .insert((target->GetGUID().GetCounter() * 1000003ull + bot->GetGUID().GetCounter()) * 3ull +
                                    uint64(verdict.kind))
                            .second;
            }
            if (fresh)
                LOG_INFO("playerbots", "[lag353] {} L{} {} {} L{}: {} mobs above its level at once, judged by the "
                         "fight's numbers ({:.0f} dmg expected over {:.0f} s, ehp {:.0f} now / {:.0f} full) - the old "
                         "rule refused it",
                         bot->GetName(), bot->GetLevel(),
                         verdict.kind == Verdict::Kind::Win ? "takes" : "rests first for",
                         target->GetName(), target->GetLevel(), verdict.above, damage, seconds, verdict.ehpNow,
                         verdict.ehpFull);
        }
        std::string const numbers = Acore::StringFormat(
            "{} mobs ({} above), {:.0f} dmg expected over {:.0f} s at {:.1f} dps ({:.1f} without mana; full: {:.0f} "
            "dmg over {:.0f} s), ehp {:.0f} now / {:.0f} full",
            verdict.mobs, verdict.above, damage, seconds, botDps, whiteDps, atFull.damage, atFull.seconds,
            verdict.ehpNow, verdict.ehpFull) +
            (verdict.pullSpot ? Acore::StringFormat(", fought at its pull spot ({:.0f},{:.0f})", verdict.pull.x,
                                                    verdict.pull.y)
                              : std::string(", fought where it stands"));
        verdict.why = verdict.why.empty() ? numbers : verdict.why + ": " + numbers;

        bool log = false;
        if (!wallJudge)  // [wall17]
        {
            std::lock_guard<std::mutex> guard(lock);
            SoloBotState& state = states[bot->GetGUID().GetCounter()];
            if (state.loggedTarget != target->GetGUID() || state.loggedKind != uint8(verdict.kind))

            {
                state.loggedTarget = target->GetGUID();
                state.loggedKind = uint8(verdict.kind);
                log = verdict.kind != Verdict::Kind::Win;
            }
        }
        if (log)
            LOG_INFO("playerbots", "[solo] {} L{} {} {} L{}: {}", bot->GetName(), bot->GetLevel(),
                     KindName(verdict.kind), target->GetName(), target->GetLevel(), verdict.why);
        return verdict;
    }

    // [death462] the mobs whose reach covers the judged fight spot that the judge's pack left out, each with why it was
    // not an idle mob then (in a fight, not aggressive, ...). A log, no decision: b470 had 13 adds "covers it" with no
    // word of what they were doing at the judge
    static std::string LeftOutAt(Player* bot, Unit* target, G3D::Vector3 const& at, std::vector<ObjectGuid> const& counted)
    {
        float const look = bot->GetMap()->GetVisibilityRange();
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, look);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, look);
        std::string out;
        uint32 listed = 0;
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || mob == target || !mob->IsHostileTo(bot) ||
                std::find(counted.begin(), counted.end(), mob->GetGUID()) != counted.end())
                continue;
            if (!InDisc(AggroDisc(mob, bot), at))
                continue;
            std::string why = "idle (a pull model gap)";
            if (mob->IsInEvadeMode())
                why = "evading home";
            else if (mob->IsInCombat())
                why = Acore::StringFormat("in a fight (victim {})",
                                          mob->GetVictim() ? mob->GetVictim()->GetName() : std::string("none"));
            else if (!mob->HasReactState(REACT_AGGRESSIVE))
                why = "not aggressive";
            else if (mob->GetCharmerOrOwnerGUID())
                why = "owned";
            else if (mob->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC))
                why = "immune to players";
            if (++listed > 4)
                break;
            out += Acore::StringFormat("{}{} L{} {:.0f} yd ({})", out.empty() ? "" : ", ", mob->GetName(),
                                       mob->GetLevel(), std::hypot(mob->GetPositionX() - at.x, mob->GetPositionY() - at.y),
                                       why);
        }
        return out;
    }

    void NoteEngage(Player* bot, Unit* target, Verdict const& verdict)
    {
        if (!bot || !target)
            return;
        // [death462] the fight spot the judge priced: its pull spot, else the spot its pack was grown from (a ranged
        // bot's reach on its side), else the target's. Before, a ranged engage with no pull spot was recorded at the
        // target: b470 Geron L20's fight-area step back ([death389]) took it to the Minor Tidecaller's own spot inside
        // the Murloc Flesheater's reach the judge had never priced - 4 of 13 "covers it" adds were such engages
        G3D::Vector3 const fightAt = verdict.pullSpot     ? verdict.pull
                                     : verdict.fightAtSet ? verdict.fightAt
                                                          : G3D::Vector3(target->GetPositionX(), target->GetPositionY(),
                                                                         target->GetPositionZ());
        bool const fresh = [&]()
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            return found == states.end() || !found->second.engaged ||
                   found->second.engage.target != target->GetGUID();
        }();
        std::string const leftOut = fresh ? LeftOutAt(bot, target, fightAt, verdict.counted) : std::string();
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        // [deaths] the pull spot of this engage (none: the walk goes to the target as before)
        if (state.pullTarget != target->GetGUID())
            state.pullLogged = false;
        state.pullSpot = verdict.pullSpot;
        state.pullMobs = verdict.mobs;
        state.pullTarget = target->GetGUID();
        state.pull = verdict.pull;
        state.pullAvoid = verdict.pullAvoid;
        if (state.engaged && state.engage.target == target->GetGUID())
            return;
        // [deaths14] the engaged Win verdict's numbers (a Win was never logged): b354 Tuihgrelfim L8 "judged 1 Murloc
        // Forager, came 3" and Kylantino / Lanbuzz (Frostmane Seer) had no line saying where the judge put the fight
        // or what it counted - the triage could not tell a wrong fight spot from an uncounted wanderer
        LOG_INFO("playerbots", "[deaths14] {} L{} engages {} L{}: judged {} mobs ({} above), {:.0f} dmg over {:.0f} s at "
                 "ehp {:.0f}, fought {} ({:.0f},{:.0f}), {:.0f} yd from the target{}{}",
                 bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel(), verdict.mobs, verdict.above,
                 verdict.damage, verdict.fightSec, verdict.ehpNow,
                 verdict.pullSpot ? "at its pull spot" : (PlayerbotAI::IsRanged(bot) ? "at its reach" : "at the target"),
                 fightAt.x, fightAt.y, target->GetExactDist2d(fightAt.x, fightAt.y),
                 verdict.respawns ? Acore::StringFormat("; [death463] {} respawn(s) counted", verdict.respawns)
                                  : std::string(),
                 leftOut.empty() ? std::string() : "; [death462] reaches over the spot left out: " + leftOut);
        state.engaged = true;
        state.engage.target = target->GetGUID();
        state.engage.targetName = target->GetName();
        state.engage.predicted = verdict.damage;
        state.engage.health = float(bot->GetHealth());
        state.engage.healthPct = bot->GetHealthPct();
        state.engage.manaPct = bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f;
        state.engage.mobs = verdict.mobs;
        // [fight49] the safe area: the fight spot and its margin to the nearest idle mob's reach that is not the
        // target's own (the judge counted those joining it)
        state.engage.fightAt = fightAt;  // [death462]
        state.engage.safeRadius = -1.0f;
        state.engage.counted = verdict.counted;  // [death392]
    }

    namespace
    {
        // the margin round `at` to the nearest idle hostile reach of a mob other than the fight's (in sight)
        // [death392] `counted`: the mobs the judge counted - their reaches are the fight's own, no reach it left out.
        // Before, only the target was: b449 Laaral L23's judged fight of 5 had "safe 0 yd: the reach of Shadowhide
        // Gnoll L22", one of the 5 - every step was "past the judged fight spot"
        float SafeMargin(Player* bot, Unit* target, G3D::Vector3 const& at, std::string& nearest,
                         std::vector<ObjectGuid> const& counted)
        {
            float const look = sPlayerbotAIConfig.SightDistance;
            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(bot, look);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
            Cell::VisitObjects(bot, searcher, look);
            float margin = look;
            for (Unit* unit : units)
            {
                Creature* mob = unit->ToCreature();
                if (!mob || mob == target || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                    !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID() ||
                    std::find(counted.begin(), counted.end(), mob->GetGUID()) != counted.end())
                    continue;
                G3D::Vector4 const disc = AggroDisc(mob, bot);
                float gap = std::hypot(disc.x - at.x, disc.y - at.y) - disc.w;
                // [death399] a patrol reaches the area from anywhere on its forecast beat ([death391]), not only where
                // it walks now. b453 Thyllyn L20 won its pulls at the Zoram Strand "safe 6 yd" by where Ruuzel L22
                // stood, ended the fight on Ruuzel's beat, stood there and died to him and two Wrathtails
                if (mob->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE)
                {
                    std::vector<G3D::Vector3> beat;
                    float sampleSec = 0.0f;
                    if (PullPlanning::PatrolForecast(mob, beat, sampleSec))
                        for (G3D::Vector3 const& step : beat)
                            gap = std::min(gap, std::hypot(step.x - at.x, step.y - at.y) - disc.w);
                }
                if (gap < margin)
                {
                    margin = gap;
                    nearest = Acore::StringFormat("{} L{}", mob->GetName(), mob->GetLevel());
                }
            }
            return margin;
        }
    }

    bool LootInReach(Player* bot, G3D::Vector3 const& at, std::string& who)
    {
        if (!bot || bot->IsInCombat() || !Applies(bot))
            return false;
        std::vector<ObjectGuid> const none;
        std::string here;
        G3D::Vector3 const stands(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        // a reach it stands in already is no step deeper: only one the walk to the loot enters
        return SafeMargin(bot, nullptr, at, who, none) < 0.0f && SafeMargin(bot, nullptr, stands, here, none) >= 0.0f;
    }

    // [death418] a mob of the judged fight: its engaged target, a mob its judge counted, or one on the bot now. The
    // judged area held only for the engaged target: b456 Maronic L21 (holy) judged 4 Shadowhide Gnolls at
    // (-9287,-3183); its target dead, it chased the other three 23-41 yd off the spot into the camp, 10 came, dead
    // [death420] ... the counted mobs and the ones on it only once that engage's fight is open (OpenIfNeeded drops a
    // stale engage, [death398]): b458 Rollothan L15, a Black Dragon Whelp on it while its engage of a Redridge Poacher
    // was stale, "steps back to (-9721,-2271)" 71 yd to that old spot, met a second whelp on the way and died
    static bool OfEngage(Player* bot, SoloBotState const& state, Unit* target)
    {
        ObjectGuid const guid = target->GetGUID();
        if (!state.engaged)
            return false;
        if (state.engage.target == guid)
            return true;
        return state.open && (target->GetVictim() == bot ||
                              std::find(state.engage.counted.begin(), state.engage.counted.end(), guid) !=
                                  state.engage.counted.end());
    }

    bool AttackerWakes(Player* bot, Unit* target, std::string& why)
    {
        if (!bot || !target)
            return false;
        std::vector<ObjectGuid> counted;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end() || !OfEngage(bot, found->second, target))
                return false;
            counted = found->second.engage.counted;
        }
        float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        if (assist <= 0.0f)
            return false;
        for (Unit* attacker : bot->getAttackers())
        {
            Creature* mob = attacker ? attacker->ToCreature() : nullptr;
            if (!mob || !mob->IsAlive() || mob->HasFlagsExtra(CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE))
                continue;
            std::list<Creature*> nearby;
            Acore::AnyAssistCreatureInRangeCheck check(mob, bot, assist);
            Acore::CreatureListSearcher<Acore::AnyAssistCreatureInRangeCheck> searcher(mob, nearby, check);
            Cell::VisitObjects(mob, searcher, assist);
            for (Creature* helper : nearby)
                if (helper && helper->IsAlive() && !helper->IsInCombat() &&
                    std::find(counted.begin(), counted.end(), helper->GetGUID()) == counted.end())
                {
                    why = Acore::StringFormat("{} L{} fights {:.0f} yd from {} L{} the judge did not count",
                                              mob->GetName(), mob->GetLevel(), mob->GetExactDist2d(helper),
                                              helper->GetName(), helper->GetLevel());
                    return true;
                }
        }
        return false;
    }

    bool JudgedFightSpot(Player* bot, Unit* target, G3D::Vector3& at)
    {
        if (!bot || !target)
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !OfEngage(bot, found->second, target))
            return false;
        at = found->second.engage.fightAt;
        return true;
    }

    bool FightPastJudged(Player* bot, Unit* target, G3D::Vector3 const& to, std::string& why)
    {
        if (!bot || !target)
            return false;
        G3D::Vector3 at;
        float radius = -1.0f;
        std::vector<ObjectGuid> counted;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end() || !OfEngage(bot, found->second, target))
                return false;
            at = found->second.engage.fightAt;
            radius = found->second.engage.safeRadius;
            counted = found->second.engage.counted;
        }
        std::string nearest;
        if (radius < 0.0f)
        {
            float const raw = SafeMargin(bot, target, at, nearest, counted);
            if (raw < 0.0f)  // [death392] a reach over the spot the judge did not count: a pull model gap, logged
                LOG_INFO("playerbots", "[death392] {} L{} fights {} L{} at ({:.0f},{:.0f}) inside the reach of {}, "
                         "not among its {} counted mobs", bot->GetName(), bot->GetLevel(), target->GetName(),
                         target->GetLevel(), at.x, at.y, nearest, counted.size());
            radius = std::max(0.0f, raw);
            std::lock_guard<std::mutex> guard(lock);
            states[bot->GetGUID().GetCounter()].engage.safeRadius = radius;
        }
        float const off = std::hypot(to.x - at.x, to.y - at.y);
        if (off <= radius)
            return false;
        // past the judged area: judged again at `to` - a reach it would enter there is its answer
        std::string entered;
        float const there = SafeMargin(bot, target, to, entered, counted);
        if (there >= 0.0f)
        {
            // no reach there either: the area grows to it (the fight goes on there)
            std::lock_guard<std::mutex> guard(lock);
            auto& engage = states[bot->GetGUID().GetCounter()].engage;
            engage.fightAt = to;
            engage.safeRadius = there;
            return false;
        }
        why = Acore::StringFormat("{:.0f} yd past the judged fight spot ({:.0f},{:.0f}, safe {:.0f} yd): the reach of {}",
                                  off - radius, at.x, at.y, radius, entered);
        // [death413] a melee bot whose target hits it from beyond its reach (a caster standing to cast) loses by
        // standing: the step past is judged as the fight it is - the target's health left and every idle mob whose
        // reach covers `to`, at the high end against its health now. b455 Ganna L22 (prot) stood 41 s "does not
        // chase Redridge Mystic L19 (3 % hp): the reach of Black Dragon Whelp L18" under its bolts and died
        if (!PlayerbotAI::IsRanged(bot) && target->GetVictim() == bot && !bot->IsWithinMeleeRange(target))
        {
            // [dpszero1] no floor: with no estimate there is nothing to price this step with, and with a measured
            // zero the fight has no end - either way the step is not refused on an invented number
            DpsEstimate::Result const step = OwnDamage(bot, target);
            KillRate::Own const burst{step.estimated, step.burstDps};
            if (KillRate::StateOf(burst) != KillRate::State::Deals)
                return true;
            struct Foe
            {
                float hp, dps, variance;
            };
            std::vector<Foe> foes{{float(target->GetHealth()), MobDps(target, bot), DamageVariancePerSec(target, bot)}};
            float const look = sPlayerbotAIConfig.SightDistance;
            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(bot, look);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
            Cell::VisitObjects(bot, searcher, look);
            for (Unit* unit : units)
                if (Creature* mob = unit->ToCreature())
                    if (mob != target && mob->IsAlive() && !mob->IsInCombat() && mob->IsHostileTo(bot) &&
                        mob->HasReactState(REACT_AGGRESSIVE) && !mob->GetCharmerOrOwnerGUID() &&
                        InDisc(AggroDisc(mob, bot), to))
                        foes.push_back({float(mob->GetHealth()), MobDps(mob, bot), DamageVariancePerSec(mob, bot)});
            float aliveDps = 0.0f;
            float aliveVariance = 0.0f;
            for (Foe const& foe : foes)
            {
                aliveDps += foe.dps;
                aliveVariance += foe.variance;
            }
            float seconds = 0.0f;
            float damage = 0.0f;
            float variance = 0.0f;
            for (Foe const& foe : foes)
            {
                float const kill = KillRate::KillSec(burst, foe.hp);
                seconds += kill;
                damage += aliveDps * kill;
                variance += aliveVariance * kill;
                aliveDps -= foe.dps;
                aliveVariance -= foe.variance;
            }
            float const high = damage + 2.0f * std::sqrt(std::max(variance, 0.0f));
            float const mana = float(bot->GetPower(POWER_MANA));
            float const ehp = float(bot->GetHealth()) +
                              SurvivalKit::HealPoints(bot, seconds, mana, true, false,
                                                      damage / std::max(seconds, 1.0f));
            if (high < ehp)
            {
                LOG_INFO("playerbots", "[death413] {} L{} steps past its judged fight spot after {} L{} out of its "
                         "reach: {} mob(s) there, {:.0f} dmg at the high end over {:.0f} s at ehp {:.0f}",
                         bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel(), foes.size(), high,
                         seconds, ehp);
                std::lock_guard<std::mutex> guard(lock);
                auto& engage = states[bot->GetGUID().GetCounter()].engage;
                engage.fightAt = to;
                engage.safeRadius = 0.0f;
                return false;
            }
        }
        return true;
    }

    bool PullWalk(Player* bot, G3D::Vector3 const& to, G3D::Vector3& spot, std::vector<G3D::Vector4>& avoid)
    {
        if (!bot || bot->IsInCombat())
            return false;
        ObjectGuid targetGuid;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end() || !found->second.pullSpot)
                return false;
            targetGuid = found->second.pullTarget;
            spot = found->second.pull;
            avoid = found->second.pullAvoid;
        }
        Creature* target = ObjectAccessor::GetCreature(*bot, targetGuid);
        if (!target || !target->IsAlive() || target->IsInCombat() || bot->GetTarget() != targetGuid)
            return false;
        // a walk that heads for the target (as near it as the pull spot), and the bot not at the spot yet (there the
        // pull is made: the walk on is its own)
        float const spotGap = target->GetExactDist2d(spot.x, spot.y);
        return target->GetExactDist2d(to.x, to.y) <= spotGap + bot->GetCombatReach() &&
               bot->GetExactDist2d(spot.x, spot.y) > bot->GetCombatReach();
    }

    // [lag370] coordinator 2026-11-12 (b449 Gilduck L14: from "engages Defias Smuggler ... fought at its pull spot
    // (-10535,1240), 28 yd from the target" at 07:00 no state change for 50 min - no attack, no hit taken, a route
    // search every 2-5 s priced to that spot): the combat walk goes to the pull spot (PullWalk ends the target's walk
    // there) and when no walk reaches it from where the bot stands, every tick asks the same search again and the bot
    // stands. The spot's walk is a fact of the moment: none, the spot is dropped and the engage released (its next
    // look judges the pull from here)
    void CheckPullSpotWalk(Player* bot, Unit* target)
    {
        if (!bot || !target || bot->IsInCombat() || !Applies(bot))
            return;
        G3D::Vector3 spot;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end() || !found->second.pullSpot || found->second.pullTarget != target->GetGUID())
                return;
            spot = found->second.pull;
        }
        if (bot->GetExactDist2d(spot.x, spot.y) <= bot->GetCombatReach() || bot->isMoving())
            return;
        Leader::RouteResult const route = Leader::Route(bot, Position(spot.x, spot.y, spot.z));
        if (route.found && !route.throughRefused)
            return;
        {
            std::lock_guard<std::mutex> guard(lock);
            SoloBotState& state = states[bot->GetGUID().GetCounter()];
            state.pullSpot = false;
            state.engaged = false;
            state.pullTarget = ObjectGuid::Empty;
        }
        LOG_INFO("playerbots", "[lag370] {} L{} drops its pull spot ({:.0f},{:.0f}) for {} L{}: {} from where it "
                 "stands",
                 bot->GetName(), bot->GetLevel(), spot.x, spot.y, target->GetName(), target->GetLevel(),
                 route.found ? "its only walk passes a refused reach" : "no walk reaches it");
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
        // [stuck40] the grind picker leaves it while the two stands that made it undroppable are unchanged. This was
        // one ObjectGuid on the AI, so a second such mob evicted the first and the picker took turns between them
        NoteUnreachable(bot, target, route.found ? "its only walk to its pull spot passes a refused reach"
                                                 : "no walk reaches its pull spot",
                        false);
        bot->SetTarget(ObjectGuid::Empty);
    }

    // [stuck259] THE MOB IS NO LONGER AT THE STAND ITS VERDICT WAS GIVEN AT - by the mob's OWN area, and not by
    // Leader::LEADER_COMMIT_MOVED_YD, which measures when a LEADER'S COMMITMENT to a pull stops being about the
    // same arrangement (one quantity, one judge). Measured: of 199 `stuck_target` cases 25 were the same bot and
    // the same mob again inside a minute, and in 20 of those 25 the bot had not moved a yard - only the mob had,
    // inside areas of 10 yd and 3 yd that the 12 yd measure cannot see at all. The rule itself is the seam
    // (MobStand.h), where the stand asserts it; the facts are read here.
    bool MobLeftItsStand(Unit* target, Position const& judgedAt)
    {
        Rpg::MobStand::Facts facts;
        facts.bodyYd = target->GetCombatReach();
        Creature* mob = target->ToCreature();
        // the wanderer's own area is the pull judge's own model of it ([lead1331]), read from the one place that
        // has it; a mob that does not roam holds the spot it was judged on, which is that area with no radius
        if (Leader::Fight::WanderArea area; mob && Leader::Fight::WanderAreaOf(mob, area))
        {
            facts.wanders = true;
            facts.areaRadius = area.radius;
            facts.fromCenterYd = target->GetExactDist2d(&area.center);
        }
        else
            facts.fromCenterYd = target->GetExactDist2d(&judgedAt);
        return Rpg::MobStand::LeftItsArea(facts);
    }

    void NoteUnreachable(Player* bot, Unit* target, std::string const& why, bool log)
    {
        if (!bot || !target)
            return;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> guard(unreachableLock);
            auto& mine = unreachableFrom[bot->GetGUID().GetCounter()];
            fresh = mine.find(target->GetGUID()) == mine.end();
            mine[target->GetGUID()] = {bot->GetPosition(), target->GetPosition()};
        }
        if (!fresh || !log)  // one line per bot and target while the memo holds (the [regress10] hearthNote cadence)
            return;
        LOG_INFO("playerbots",
                 "[nav200] {} L{} leaves {} L{} ({:.0f} yd) at ({:.1f},{:.1f},{:.1f}) map {}: {} from where it "
                 "stands ({:.1f},{:.1f},{:.1f}) - asked again only once the bot moves {:.0f} yd ({:.0f} yd up or "
                 "down) or the target leaves its own area ([stuck259]: its wander area, else the spot it holds)",
                 bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel(), bot->GetExactDist(target),
                 target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), bot->GetMapId(), why,
                 bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                 Leader::Travel::UNREACHABLE_RECHECK_DISTANCE, Leader::Travel::UNREACHABLE_RECHECK_HEIGHT);
    }

    bool Unreachable(Player* bot, Unit* target)
    {
        if (!bot)
            return false;
        bool held = false;
        std::lock_guard<std::mutex> guard(unreachableLock);
        auto const mineAt = unreachableFrom.find(bot->GetGUID().GetCounter());
        if (mineAt == unreachableFrom.end())
            return false;
        auto& mine = mineAt->second;
        // the bot is no longer where it searched from: every memo made there is an open question again (the dungeon
        // side's rule, and what keeps this map small - a bot that walks on drops all of them)
        for (auto itr = mine.begin(); itr != mine.end();)
        {
            Position const& from = itr->second.first;
            if (bot->GetExactDist2d(&from) > Leader::Travel::UNREACHABLE_RECHECK_DISTANCE ||
                std::fabs(bot->GetPositionZ() - from.GetPositionZ()) > Leader::Travel::UNREACHABLE_RECHECK_HEIGHT)
                itr = mine.erase(itr);
            else
                ++itr;
        }
        if (target)
        {
            auto const found = mine.find(target->GetGUID());
            if (found != mine.end())
            {
                // the target left the stand it was judged at: its walk is another question (the dungeon side's
                // skipFacts). [stuck259] the mob's own area says that, not the leader's commitment measure
                if (MobLeftItsStand(target, found->second.second))
                    mine.erase(found);
                else
                    held = true;
            }
        }
        if (mine.empty())
            unreachableFrom.erase(mineAt);
        return held;
    }

    PullMove PullStep(Player* bot, Unit* target, G3D::Vector3& spot)
    {
        Creature* mob = target ? target->ToCreature() : nullptr;
        if (!bot || !mob || !mob->IsAlive() || !Applies(bot))
            return PullMove::None;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end() || !found->second.pullSpot || found->second.pullTarget != mob->GetGUID())
                return PullMove::None;
            spot = found->second.pull;
        }
        bool const there = bot->GetExactDist2d(spot.x, spot.y) <= bot->GetCombatReach();
        if (!mob->IsInCombat())
        {
            if (!there)
                return PullMove::None;  // its walk goes to the spot (Leader::Route: PullWalk)
            // a melee bot with Throw / Shoot pulls from the spot; without one it steps in to the target's aggro edge
            uint32 spell = 0;
            float const pullReach = PullReach(bot, &spell);
            if (spell && !PlayerbotAI::IsRanged(bot) && bot->IsWithinDistInMap(mob, pullReach) &&
                !bot->HasSpellCooldown(spell) && !bot->IsNonMeleeSpellCast(true) && bot->IsWithinLOSInMap(mob))
            {
                bot->StopMoving();
                bot->SetFacingToObject(mob);
                if (bot->CastSpell(mob, spell, false) == SPELL_CAST_OK)
                    return PullMove::Hold;
            }
            return PullMove::None;
        }
        // the target comes for it: the bot meets it at the spot (back there after a body pull's step in); a target
        // that stands (a caster at its range, rooted) is gone to
        if (mob->GetVictim() != bot || !mob->isMoving() ||
            mob->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
            return PullMove::None;
        return there ? PullMove::Hold : PullMove::ToSpot;
    }

    float ChaseSpellDamage(Creature* mob, float seconds)
    {
        if (!mob || seconds <= 0.0f)
            return 0.0f;
        float damage = 0.0f;
        for (ChaseCast const& cast : ChaseCastsOf(mob))
            damage += cast.everySec > 0.0f ? cast.damage * std::ceil(seconds / cast.everySec) : cast.damage;
        return damage;
    }

    bool WantsRest(Player* bot, bool mana, std::string* why)
    {
        if (!bot)
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end())
            return false;
        // [guild14] a fight that ended (a disengage, a won fight with no next pull judged yet) is closed out of combat
        CloseIfOver(bot, found->second);
        if (!found->second.rest)
            return false;
        SoloBotState& state = found->second;
        if (bot->IsInCombat() || !bot->IsAlive())
        {
            // [death397] ... but the rest after a death: b453 Maronic L21 rose at half health, a 2.6 s fight cleared
            // that rest, its hub walk went on unrested into the Redridge Bashers' camp and it died again
            if (bot->IsInCombat() && !state.restAfterDeath)
                state.rest = false;
            else if (bot->IsInCombat() && !state.loggedDeathRest)
            {
                state.loggedDeathRest = true;
                LOG_INFO("playerbots", "[death397] {} L{} keeps its rest after a death through a fight (hp {:.0f} %)",
                         bot->GetName(), bot->GetLevel(), bot->GetHealthPct());
            }
            return false;
        }
        bool const healthFull = bot->GetHealth() >= bot->GetMaxHealth();
        bool const manaFull =
            !bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA);
        if (healthFull && manaFull)
        {
            state.rest = false;
            state.restAfterDeath = false;
            return false;
        }
        bool const wants = mana ? !manaFull : !healthFull;
        if (wants && why)
            *why = Acore::StringFormat("{} {:.0f} % < 100 %: {}", mana ? "mana" : "hp",
                                       mana ? bot->GetPowerPct(POWER_MANA) : bot->GetHealthPct(), state.restWhy);
        return wants;
    }

    void RequestRest(Player* bot, std::string const& why)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        if (!state.rest)
            LOG_INFO("playerbots", "[solo] {} L{} rests to full: {}", bot->GetName(), bot->GetLevel(), why);
        state.rest = true;
        state.restWhy = why;
    }

    // [eco40] coordinator 2026-11-13: the bot is walking to the vendor that sells the food / drink it has none of
    // (StartVendorRun's run: vendorRunEntry with "buy food" / "buy drink" in its reason). Such a rest ends at that
    // vendor and nowhere else - nothing in its bags shortens it - so it must not stand the walk still. b475
    // Bhogagnuth: "[New RPG] goes to vendor Gruham Rumdnul (397 yd): buy drink", then 278 "[rest] sits to regenerate:
    // nothing to eat or drink" 397 yd short of the water, mana 60 % -> 81 % on spirit alone, and the leader read it
    // as "STUCK 1 min: GO_CAMP vendor: buy drink". 1471 such sits in 11 h against 63 vendor runs that reached one
    bool RestockRunPending(Player* bot)
    {
        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!botAI || !botAI->rpgInfo.vendorRunEntry)
            return false;
        std::string const& reason = botAI->rpgInfo.vendorRunReason;
        return reason.find("buy food") != std::string::npos || reason.find("buy drink") != std::string::npos;
    }

    // what it lacks and carries nothing for: the rest actions have nothing to sit it down with
    bool NothingForItsRest(Player* bot)
    {
        bool const health = WantsRest(bot, false);
        bool const mana = WantsRest(bot, true);
        bool const eats = health && CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD);
        bool const drinks = mana && CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK);
        return (health || mana) && !eats && !drinks;
    }

    // [eco40] the rest it is told to take cannot be taken here and the vendor run it is on is the only thing that
    // ends it: neither the sit nor the rest-before-walk hold that walk
    bool WalksToItsRestock(Player* bot)
    {
        return bot && !bot->IsInCombat() && RestockRunPending(bot) && NothingForItsRest(bot);
    }

    bool WaitsToRegen(Player* bot)
    {
        if (!bot || bot->IsInCombat())
            return false;
        if (NothingForItsRest(bot) && RestockRunPending(bot))  // [eco40] it walks to the water instead
        {
            static std::mutex restockWalkLock;
            static std::set<std::pair<uint32, uint32>> restockWalkLogged;  // (bot, vendor): once per run
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            bool first = false;
            {
                std::lock_guard<std::mutex> guard(restockWalkLock);
                first = restockWalkLogged.insert({bot->GetGUID().GetCounter(), botAI->rpgInfo.vendorRunEntry}).second;
            }
            if (first)
                LOG_INFO("playerbots", "[eco40] {} L{} walks on to its vendor ({}) with nothing to eat or drink: the "
                         "sit here ends at that vendor (hp {:.0f} %, mana {:.0f} %)",
                         bot->GetName(), bot->GetLevel(), botAI->rpgInfo.vendorRunReason, bot->GetHealthPct(),
                         bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
            return false;
        }
        return NothingForItsRest(bot);
    }

    bool RestBeforeWalk(Player* bot, Position const& dest)
    {
        if (!bot || bot->IsInCombat() || !bot->IsAlive() || !Applies(bot))
            return false;
        // [eco40] ... and the same walk is not held by a rest asked for earlier, nor by the mob on its stretch: with
        // nothing to eat or drink the bot would stand there until its spirit alone filled the bar (b475: 1471 sits)
        if (WalksToItsRestock(bot))
            return false;
        bool const full = bot->GetHealth() >= bot->GetMaxHealth() &&
                          (!bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA));
        if (full)
            return false;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            // a rest asked for (a pull, a death, this walk): no walk on until full - a step stands it up from its food
            if (found != states.end() && found->second.rest)
                return true;
        }
        // the stretch it sees of the walk
        float const fromX = bot->GetPositionX();
        float const fromY = bot->GetPositionY();
        float const total = bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
        if (total <= 0.0f)
            return false;
        float const share = std::min(1.0f, sPlayerbotAIConfig.SightDistance / total);
        float const toX = fromX + (dest.GetPositionX() - fromX) * share;
        float const toY = fromY + (dest.GetPositionY() - fromY) * share;
        float const dx = toX - fromX;
        float const dy = toY - fromY;
        float const lengthSq = dx * dx + dy * dy;
        // the first idle mob on it whose reach (aggro range, its wander area) the stretch enters
        Creature* first = nullptr;
        float firstT = 2.0f;
        for (ObjectGuid const& guid :
             GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
        {
            Creature* mob = ObjectAccessor::GetCreature(*bot, guid);
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID())
                continue;
            float reach = mob->GetAggroRange(bot) + bot->GetCombatReach();
            Position centre = mob->GetPosition();
            if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
            {
                centre = mob->GetHomePosition();
                reach += mob->GetWanderDistance();
            }
            float t = lengthSq > 0.0f
                          ? ((centre.GetPositionX() - fromX) * dx + (centre.GetPositionY() - fromY) * dy) / lengthSq
                          : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            float const ex = fromX + dx * t - centre.GetPositionX();
            float const ey = fromY + dy * t - centre.GetPositionY();
            if (ex * ex + ey * ey > reach * reach || t >= firstT)
                continue;
            first = mob;
            firstT = t;
        }
        if (!first)
            return false;
        Verdict const verdict = Judge(bot, first);
        if (verdict.kind != Verdict::Kind::RestFirst)
            return false;
        RequestRest(bot, std::string("before walking past ") + first->GetName() + ": " + verdict.why);
        return true;
    }

    // [dps1] user 2026-11-08: no decision reads a stored damage measurement; the 'solo' rows in playerbots_db_store
    // are neither read nor written any more (left in the table)
    void LoadMeasured(Player* /*bot*/, std::string const& /*value*/) {}

    // [dps1] broken gear loses its stats in the core (Player::_ApplyItemMods), the formula reads them as they are
    void OnRepaired(Player* /*bot*/) {}

    // [dps1] the formula's cache is keyed by its power signature: a change is computed anew at its next read
    void OnPowerChanged(Player* bot, char const* why)
    {
        if (bot && bot->IsInWorld())
            LOG_DEBUG("playerbots", "[dps] {} L{} power changed ({}): its damage formula is computed anew",
                      bot->GetName(), bot->GetLevel(), why);
    }

    bool PowerSpell(uint32 spellId) { return !ProfessionSpell(spellId); }

    // [dps1] a hunter without its pet takes the mob in melee once the mob has run from Auto Shot's longest range to
    // its shortest (user 2026-11-08, computed, no measurement): a mob of its level, its health from
    // creature_classlevelstats over the formula's damage the fight, the creature default run speed
    // (ObjectMgr::CheckCreatureTemplate: speed_run 1.14286 x baseMoveSpeed)
    float MeleeShare(Player* bot)
    {
        constexpr uint32 SPELL_AUTO_SHOT = 75;
        constexpr float CREATURE_DEFAULT_SPEED_RUN = 1.14286f;
        constexpr uint8 CLASSIC_MAX_LEVEL = 60;
        constexpr uint8 OUTLAND_MAX_LEVEL = 70;
        if (!bot)
            return -1.0f;
        SpellInfo const* autoShot = sSpellMgr->GetSpellInfo(SPELL_AUTO_SHOT);
        if (!autoShot || !bot->GetWeaponForAttack(RANGED_ATTACK, true))
            return 1.0f;
        CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(bot->GetLevel(), CLASS_WARRIOR);
        if (!stats)
            return -1.0f;
        // the expansion of the content at its level (the creature_classlevelstats health column it uses)
        uint8 const expansion = bot->GetLevel() <= CLASSIC_MAX_LEVEL   ? EXPANSION_CLASSIC
                                : bot->GetLevel() <= OUTLAND_MAX_LEVEL ? EXPANSION_THE_BURNING_CRUSADE
                                                                       : EXPANSION_WRATH_OF_THE_LICH_KING;
        // [dpszero1] no floor: with no estimate this share is unknown, which this function already says with -1,
        // and a measured zero means a fight that never ends - the approach is then a vanishing part of it
        DpsEstimate::Result const own = OwnDamage(bot, nullptr);
        KillRate::Own const rate{own.estimated, own.dps};
        if (!KillRate::Judgeable(rate))
            return -1.0f;
        if (KillRate::StateOf(rate) == KillRate::State::DealsNothing)
            return 1.0f;
        float const fightSec = KillRate::KillSec(rate, float(stats->BaseHealth[expansion]));
        float const approachSec = std::max(0.0f, autoShot->GetMaxRange(false) - autoShot->GetMinRange(false)) /
                                  (baseMoveSpeed[MOVE_RUN] * CREATURE_DEFAULT_SPEED_RUN);
        if (fightSec <= 0.0f)
            return 0.0f;
        return std::clamp((fightSec - approachSec) / fightSec, 0.0f, 1.0f);
    }

    void OnDeath(Player* bot)
    {
        if (!SoloPlayerBot(bot))
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        CloseFight(bot, state, true);
        state.recover = true;
        state.rest = true;
        state.restAfterDeath = true;
        state.loggedDeathRest = false;
        state.restWhy = "after a death";
    }

    // [guild80] the open solo fight projected (the attackers' damage still to come against its health and heals),
    // computed as the b304 disengage judged it; SurvivalKit picks the escape from it
    Projection Project(Player* bot)
    {
        Projection out;
        if (!bot || !bot->IsAlive() || !bot->IsInCombat() || !Applies(bot))
            return out;
        std::unique_lock<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.open)
            return out;
        SoloBotState& state = found->second;
        out.open = true;
        out.fleeing = state.fleeing;

        // [death441] user rule 2026-11-08 (reminded 2026-11-12): the attackers' damage and its own come from the
        // formulas alone - the core's creature stats (MobDps) and DpsEstimate (its gear, spells, talents, buffs). What
        // this fight measured goes to the [dps] / [mobdps] lines only. Before, the fight's own rates replaced them
        // after two swings (death438: one rotation): b462 Septimon L17 ran on "198 s at its 2.2 dps" against its
        // formula's 20.9 from a fight judged won
        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != bot)
                continue;
            float const dps = MobDps(attacker, bot);
            Projection::Foe foe;
            foe.unit = attacker;
            foe.hp = float(attacker->GetHealth());
            foe.dps = dps;
            if (Creature* creature = attacker->ToCreature())
            {
                foe.spellDps = SpellDpsOf(creature);
                out.leashSec = std::max(out.leashSec, float(creature->GetLeashTimer()));
            }
            out.foes.push_back(foe);
            out.incoming += dps;
        }
        if (out.foes.empty() || out.incoming <= 0.0f)
            return out;

        // [surv10] the fight no judge opened is priced here, at its first projection: taken under the lock, logged
        // below it once the projection is whole
        std::string unjudged;
        if (state.unjudgedPending)
        {
            unjudged = state.unjudgedWhy;
            state.unjudgedPending = false;
            state.unjudgedWhy.clear();
        }
        // [surv10] coordinator 2026-11-14: one judge of one question. The pull judge's model already counts what the
        // core brings to a fight - the engage call, the formation, the periodic CallAssistance, a flee for help, and
        // an idle mob whose reach covers the fight spot ([pullplan]) - while this projection counted only the mobs
        // already swinging. That is why it priced 1 attacker in 17 of b484's 19 unjudged opens and called all three
        // fatal ones won with 29, 263 and 333 hp of margin while 4, 5 and 2 mobs came. The pack is grown once per
        // fight below (outside this lock) and resolved here, so the fight is priced on the mobs it will have and not
        // on the mobs it has this instant. No threshold: the same model, read in one more place.
        bool const growJoiners = !state.engaged && !state.joinersBuilt;
        for (uint64 const guid : state.joiners)
        {
            Unit* const joiner = ObjectAccessor::GetCreature(*bot, ObjectGuid(guid));
            if (!joiner || !joiner->IsAlive() || !joiner->IsHostileTo(bot))
                continue;
            if (std::any_of(out.foes.begin(), out.foes.end(),
                            [joiner](Projection::Foe const& f) { return f.unit == joiner; }))
                continue;  // already swinging: counted above, with what this fight measured of it
            float const dps = MobDps(joiner, bot);
            Projection::Foe foe;
            foe.unit = joiner;
            foe.hp = float(joiner->GetHealth());
            foe.dps = dps;
            if (Creature* creature = joiner->ToCreature())
            {
                foe.spellDps = SpellDpsOf(creature);
                out.leashSec = std::max(out.leashSec, float(creature->GetLeashTimer()));
            }
            out.foes.push_back(foe);
            out.incoming += dps;
        }

        DpsEstimate::Result const own = OwnDamage(bot, bot->GetVictim());
        // [dpszero1] no floor. NO ESTIMATE: nothing is projected, the same answer this function already gives when
        // there is nothing to project - a fight is never called lost on a number nobody computed. A MEASURED ZERO:
        // the attackers never fall, so the open fight IS lost, and it is said as that verdict rather than by
        // dividing their health by an invented 1 dps.
        KillRate::Own const rate{own.estimated, own.dps};
        switch (KillRate::StateOf(rate))
        {
            case KillRate::State::NoEstimate:
                return out;
            case KillRate::State::DealsNothing:
                out.lost = true;
                return out;
            case KillRate::State::Deals:
                break;
        }
        out.botDps = rate.dps;
        // [deaths] each attacker's health with the heals it still casts on itself over the time the bot needs for it
        for (Projection::Foe& foe : out.foes)
            if (Creature* creature = foe.unit->ToCreature())
                foe.hp += HealToCome(creature, KillRate::KillSec(rate, foe.hp));
        // the rest of the fight as the judge counts it: its current target first, then the most damage per health
        Unit* const victim = bot->GetVictim();
        std::sort(out.foes.begin(), out.foes.end(),
                  [victim](Projection::Foe const& a, Projection::Foe const& b)
                  {
                      if ((a.unit == victim) != (b.unit == victim))
                          return a.unit == victim;
                      return a.dps / std::max(a.hp, 1.0f) > b.dps / std::max(b.hp, 1.0f);
                  });
        for (Projection::Foe const& foe : out.foes)
        {
            out.seconds += KillRate::KillSec(rate, foe.hp);
            out.damage += foe.dps * out.seconds;
        }
        // [death415] its heals as the judge counts them (SurvivalKit::HealPoints on the mana its rotation leaves, each
        // cast net of its time under the attackers): b456 Jurdogs L17 (holy) engaged a Black Dragon Whelp "433 dmg at
        // ehp 1326"; this projection read "444 dmg to come at hp 442 (+0 heals)" lost, no heal castable mid-cast
        // turned it, it ran before two swings and the whelp killed it on the run. Outside the solo lock: HealPoints
        // takes the kit's lock, and the kit calls into this file under its own
        out.health = float(bot->GetHealth());
        guard.unlock();
        float const mana = float(bot->GetPower(POWER_MANA));
        std::vector<SurvivalKit::Interrupter> interrupters;  // [death444]
        for (Projection::Foe const& foe : out.foes)
            if (Creature* creature = foe.unit->ToCreature())
                for (SurvivalKit::Interrupter const& in : InterruptsOf(creature))
                    interrupters.push_back(in);
        out.heals = SurvivalKit::FightHeals(bot, out.seconds, out.health, mana, own.manaPerSec,
                                            out.damage / std::max(out.seconds, 1.0f), true,
                                            &interrupters);  // [death442]
        out.lost = out.damage >= out.health + out.heals;
        // [surv10] the verdict the fight never had: the damage to come on the bot over the seconds its own damage
        // needs for the mobs now on it, against its health and the heals the kit counts - the four numbers the pull
        // judge logs at an engage ([deaths14]), for the fight that opened without one. A log, no decision reads it:
        // without it a death from an unjudged fight carries no prediction to compare the fact against, and 28 of the
        // 32 deaths over b477-b482 were such deaths
        if (!unjudged.empty())
            LOG_INFO("playerbots",
                     "[surv10] {} L{} the fight no judge opened, priced at its first projection: {}; {} attacker(s) "
                     "{:.1f} dps, {:.0f} dmg to come over {:.0f} s at its {:.1f} dps, hp {:.0f} + {:.0f} heals => {}",
                     bot->GetName(), bot->GetLevel(), unjudged, out.foes.size(), out.incoming, out.damage,
                     out.seconds, out.botDps, out.health, out.heals,
                     out.lost ? Acore::StringFormat("LOST at the open, {:.0f} short",
                                                    out.damage - out.health - out.heals)
                              : Acore::StringFormat("won with {:.0f} hp of margin",
                                                    out.health + out.heals - out.damage));
        // [surv10] the pack of a fight no judge opened, grown once per fight with the judge's own model and kept as
        // guids for the ticks after this one. Outside the lock: PackAt visits the cells round the fight, which is far
        // too costly to repeat every tick for every bot in combat ([[performance-goal]]), and this is the one place in
        // Project that already runs unlocked. The first tick of such a fight therefore still prices the attackers
        // alone; from the next one it prices the pack. The mobs are named so the next window can read the joiners the
        // old projection was blind to beside the ones that actually came.
        if (growJoiners)
        {
            std::vector<uint64> grown;
            std::string named;
            // [surv10] the pack is grown round the mob this fight is against. The bot's victim names it once the bot
            // has struck back, but a fight nobody judged opens with the mob striking first, and then the bot has no
            // victim at all: 129 of b485's 220 priced opens read "no target", so the pack was grown for none of them
            // and the projection stayed at the one attacker it could see. Tuihgrelfim L22 died at Lake Everstill out
            // of exactly that case - met a wanderer at 2 yd with no target, priced "won with 164 hp of margin" on 2
            // attackers, and 4 mobs hit it. Without a victim the fight's own first attacker names the pack, which is
            // the same mob the judge would have been asked about.
            Unit* seed = bot->GetVictim();
            if (!seed)
                for (Projection::Foe const& foe : out.foes)
                    if (foe.unit && foe.unit->ToCreature())
                    {
                        seed = foe.unit;
                        break;
                    }
            if (Unit* const victim = seed)
                if (Creature* const target = victim->ToCreature())
                {
                    G3D::Vector3 const at(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                    for (Creature* mob : PackAt(bot, target, at, {at}))
                    {
                        if (!mob || mob == target)
                            continue;
                        grown.push_back(mob->GetGUID().GetRawValue());
                        if (named.size() < 160)
                            named += Acore::StringFormat("{}{} L{}", named.empty() ? "" : ", ", mob->GetName(),
                                                         mob->GetLevel());
                    }
                }
            std::lock_guard<std::mutex> guard2(lock);
            SoloBotState& again = states[bot->GetGUID().GetCounter()];
            if (again.open && !again.joinersBuilt)
            {
                again.joinersBuilt = true;
                again.joiners = std::move(grown);
                if (!again.joiners.empty())
                    LOG_INFO("playerbots",
                             "[surv10] {} L{} the fight no judge opened brings {} mob(s) its projection was not "
                             "counting: {} - priced with it from the next tick on",
                             bot->GetName(), bot->GetLevel(), again.joiners.size(), named);
            }
        }
        return out;
    }

    void BeginFlee(Player* bot, std::string const& why)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.open)
            return;
        if (!found->second.fleeing)
        {
            found->second.fleeMs = getMSTime();
            found->second.dealtAtFlee = found->second.dealt;
        }
        found->second.fleeing = true;
        // [death453] the walk it was on is over: its run past ends with the flee (the flee's legs are the kit's). b467
        // Septimon L17 fled the Longshore murlocs, its flee stopped, RunPastStep took the old leg up again - 45 yd on
        // into the murloc camp at its q152 spot, still in the fight
        found->second.legRunPast.clear();
        found->second.legStatic.clear();
        found->second.legPoints.clear();
        found->second.legSearched.clear();
        found->second.legNamed.clear();
        found->second.rest = true;
        found->second.restWhy = why;
    }

    void EndFlee(Player* bot)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.open)
            return;
        found->second.fleeing = false;
        found->second.fleeMs = 0;
        found->second.dealtAtFlee = 0;
    }

    void RequestRestAfter(Player* bot, std::string const& why)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        state.rest = true;
        state.restWhy = why;
    }

    // [guild14] the b304 disengage (out of CombatStrategy since b309); [guild80] SurvivalKit decides the flee now
    bool ShouldDisengage(Player* bot) { return Disengaging(bot); }

    bool Disengaging(Player* bot)
    {
        if (!bot || !bot->IsInCombat())
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        return found != states.end() && found->second.open && found->second.fleeing;
    }

    bool TakeRecovery(Player* bot)
    {
        if (!bot || !bot->IsAlive())
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.recover)
            return false;
        found->second.recover = false;
        return true;
    }

    bool InRefusedReach(Player* bot, std::string& who)
    {
        if (!bot || !bot->IsAlive())
            return false;
        ObjectGuid refused;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end() || found->second.loggedKind == uint8(Verdict::Kind::Win) ||
                found->second.loggedKind == 255)
                return false;
            refused = found->second.loggedTarget;
        }
        Creature* mob = refused.IsCreature() ? ObjectAccessor::GetCreature(*bot, refused) : nullptr;
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot))
            return false;
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (!InDisc(AggroDisc(mob, bot), here))
            return false;
        who = Acore::StringFormat("{} L{}", mob->GetName(), mob->GetLevel());
        return true;
    }

    bool StandsInReach(Player* bot, std::string& who, std::vector<G3D::Vector3>& spots, bool atRestart)
    {
        who.clear();
        spots.clear();
        if (!bot || !bot->IsAlive())
            return false;
        float const sight = sPlayerbotAIConfig.SightDistance;
        // [deaths13] a wanderer whose home lies beyond sight walks its wander area to within it: the look reaches as
        // far as the map's largest wander distance past sight (at most the map's view), as the judge's does
        float const range = std::min(sight + std::min(MaxWanderDistance(bot->GetMapId()),
                                                      bot->GetMap()->GetVisibilityRange()),
                                     bot->GetMap()->GetVisibilityRange());
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, std::max(range, sight));
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, std::max(range, sight));
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        std::vector<G3D::Vector4> discs;
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || (!atRestart && (!mob->IsAlive() || mob->IsInCombat())) || !mob->IsHostileTo(bot) ||
                (mob->IsAlive() && !mob->HasReactState(REACT_AGGRESSIVE)) || mob->GetCharmerOrOwnerGUID())
                continue;
            G3D::Vector4 disc = AggroDisc(mob, bot);
            if (atRestart)  // [login67] it respawns / is back at its home
            {
                Position const& home = mob->GetHomePosition();
                disc.x = home.GetPositionX();
                disc.y = home.GetPositionY();
                disc.z = home.GetPositionZ();
            }
            discs.push_back(disc);
            if (!InDisc(disc, here))
                continue;
            who += Acore::StringFormat("{}{} L{}", who.empty() ? "" : ", ", mob->GetName(), mob->GetLevel());
        }
        if (who.empty())
            return false;
        // [deaths140] the spots are where each direction leaves the union of the reaches (every disc's chord along
        // that ray solved exactly), its combat reach past the last rim, nearest first, out to its sight. Before: a
        // ring at `out` (the deepest disc's depth, right only straight away from that disc's centre) and one at sight,
        // eight spots each - in a camp of overlapping reaches none of them was clear or walkable and the bot sat
        // where it stood. b428: 255 "stays for its help request" and 184 "Hearthstone ... waits here" against one
        // "waits for its help out of the reach"; Manue L17 (Longshore), Januel L14 (Stonesplinter Valley), Ganna L17
        // (Stendel's Pond) held and rested inside the reaches until a wanderer / patrol walked in: dead
        float const margin = bot->GetCombatReach();
        constexpr uint32 DIRECTIONS = 16;  // the search's directions all round (WorkSpot's: a look's cost, no rule)
        std::vector<std::pair<float, G3D::Vector3>> found;
        for (uint32 i = 0; i < DIRECTIONS; ++i)
        {
            float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
            float const dx = std::cos(angle);
            float const dy = std::sin(angle);
            // the yards along the ray inside each disc: [t1, t2] of |here + t d - c| <= r (2d)
            std::vector<std::pair<float, float>> inside;
            for (G3D::Vector4 const& d : discs)
            {
                float const ox = here.x - d.x;
                float const oy = here.y - d.y;
                float const b = ox * dx + oy * dy;
                float const c = ox * ox + oy * oy - d.w * d.w;
                float const disc = b * b - c;
                if (disc <= 0.0f)
                    continue;
                float const root = std::sqrt(disc);
                if (-b + root > 0.0f)
                    inside.emplace_back(std::max(0.0f, -b - root), -b + root);
            }
            std::sort(inside.begin(), inside.end());
            float clearAt = 0.0f;  // the first yard of the ray past every disc it starts in or meets on the way
            for (auto const& [t1, t2] : inside)
                if (t1 <= clearAt + margin)
                    clearAt = std::max(clearAt, t2);
            float const t = clearAt + margin;
            if (t > sight)
                continue;  // no clear spot this way within its sight (the mobs beyond are not looked at)
            float x = here.x + dx * t;
            float y = here.y + dy * t;
            float z = here.z;
            bot->UpdateAllowedPositionZ(x, y, z);
            G3D::Vector3 const at(x, y, z);
            if (std::none_of(discs.begin(), discs.end(), [&at](G3D::Vector4 const& d) { return InDisc(d, at); }))
                found.emplace_back(t, at);
        }
        std::sort(found.begin(), found.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (auto const& [t, at] : found)
            spots.push_back(at);
        return true;
    }

    bool StandsInLostReach(Player* bot, std::string& who, std::vector<G3D::Vector3>& spots)
    {
        who.clear();
        spots.clear();
        std::string covering;
        if (!bot || !bot->IsAlive() || !GET_PLAYERBOT_AI(bot) || !StandsInReach(bot, covering, spots))
            return false;
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        for (ObjectGuid const& guid :
             GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
        {
            Creature* mob = ObjectAccessor::GetCreature(*bot, guid);
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID() ||
                !InDisc(AggroDisc(mob, bot), here))
                continue;
            Verdict const verdict = JudgeAlone(bot, mob);
            if (verdict.kind != Verdict::Kind::Refuse)
                continue;
            who += Acore::StringFormat("{}{} L{} ({} mobs, {:.0f} dmg at ehp {:.0f})", who.empty() ? "" : ", ",
                                       mob->GetName(), mob->GetLevel(), verdict.mobs, verdict.damage,
                                       verdict.ehpFull);
        }
        if (who.empty())
            spots.clear();
        return !who.empty();
    }

    void NoteNoWayOut(Player* bot, std::string const& who, size_t spots, char const* site)
    {
        if (!bot)
            return;
        static std::mutex noWayLock;
        static std::unordered_map<uint32, std::string> noWayLogged;
        {
            std::lock_guard<std::mutex> guard(noWayLock);
            std::string& last = noWayLogged[bot->GetGUID().GetCounter()];
            if (last == who)
                return;
            last = who;
        }
        LOG_INFO("playerbots",
                 "[deaths140] {} L{} {}: waits inside the reach of {} at ({:.0f},{:.0f}) - {} spot(s) out of every "
                 "reach within its sight, none with a clean walk",
                 bot->GetName(), bot->GetLevel(), site, who, bot->GetPositionX(), bot->GetPositionY(), spots);
    }

    bool WorkSpot(Player* bot, WorldObject* target, float range, bool sight, G3D::Vector3& at, std::string& who)
    {
        SlowScope slow("work spot", bot ? &bot->GetName() : nullptr);  // [perf26]
        who.clear();
        if (!bot || !target || !bot->IsAlive() || range <= 0.0f)
            return false;
        float const look = std::min(sPlayerbotAIConfig.SightDistance +
                                        std::min(MaxWanderDistance(bot->GetMapId()), bot->GetMap()->GetVisibilityRange()),
                                    bot->GetMap()->GetVisibilityRange());
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(target, look);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(target, units, check);
        Cell::VisitObjects(target, searcher, look);
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        std::vector<G3D::Vector4> discs;
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || mob == target || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID())
                continue;
            G3D::Vector4 const disc = AggroDisc(mob, bot);
            discs.push_back(disc);
            if (InDisc(disc, here))
                who += Acore::StringFormat("{}{} L{}", who.empty() ? "" : ", ", mob->GetName(), mob->GetLevel());
        }
        auto const clear = [&](G3D::Vector3 const& p)
        { return std::none_of(discs.begin(), discs.end(), [&p](G3D::Vector4 const& d) { return InDisc(d, p); }); };
        auto const works = [&](G3D::Vector3 const& p)
        {
            return target->GetExactDist(p.x, p.y, p.z) <= range &&
                   (!sight || target->IsWithinLOS(p.x, p.y, p.z + bot->GetCollisionHeight()));
        };
        if (clear(here) && works(here))
        {
            at = here;
            return true;
        }
        constexpr uint32 DIRECTIONS = 16;  // the search's directions all round the target (a look's cost, no rule)
        bool found = false;
        float best = 0.0f;
        for (float const share : {0.9f, 0.6f, 0.3f})
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = target->GetPositionX() + std::cos(angle) * range * share;
                float y = target->GetPositionY() + std::sin(angle) * range * share;
                float z = target->GetPositionZ();
                bot->UpdateAllowedPositionZ(x, y, z);
                G3D::Vector3 const p(x, y, z);
                if (!clear(p) || !works(p))
                    continue;
                float const d = (p - here).length();
                if (!found || d < best)
                {
                    found = true;
                    best = d;
                    at = p;
                }
            }
        return found;
    }

    bool DenseCamp(Player* bot, std::string& why)
    {
        why.clear();
        if (!bot || bot->IsInCombat() || !Applies(bot))
            return false;
        std::string who;
        std::vector<G3D::Vector3> spots;
        if (!StandsInReach(bot, who, spots))
            return false;
        // [deaths17] a spot out of every reach counts only where the bot can fight from it: within its pull reach of
        // where it stands (its ranged pull, else none). Before, any such spot within its sight cleared the camp - the
        // bot fights at its objective, not at that spot: b357 Gnetlibozz L11 (Silver Stream Mine, 5 attackers), Namwe
        // L9 (Gol'Bolar Quarry, 7), Kianey L12 (Stonesplinter Valley, 4) died among the roamers, the line never fired
        float const pullReach = PullReach(bot);
        G3D::Vector3 const at(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (std::any_of(spots.begin(), spots.end(),
                        [&](G3D::Vector3 const& s) { return (s - at).length() <= pullReach; }))
            return false;
        // the roamers among the mobs whose reach covers the bot

        float const sight = sPlayerbotAIConfig.SightDistance;
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, sight + MAX_AGGRO_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, sight + MAX_AGGRO_RADIUS);
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        uint32 roamers = 0;
        std::string names;
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID())
                continue;
            MovementGeneratorType const type = mob->GetDefaultMovementType();
            if (type != RANDOM_MOTION_TYPE && type != WAYPOINT_MOTION_TYPE)
                continue;
            if (!InDisc(AggroDisc(mob, bot), here))
                continue;
            ++roamers;
            if (roamers <= 4)
                names += Acore::StringFormat("{}{} L{} ({})", names.empty() ? "" : ", ", mob->GetName(), mob->GetLevel(),
                                             type == RANDOM_MOTION_TYPE ? "wanderer" : "patrol");
        }
        if (roamers < 2)
            return false;
        why = Acore::StringFormat("its objective lies where no spot within sight is out of every mob's reach and {} "
                                  "roamers reach it ({}): a camp for a party",
                                  roamers, names);
        return true;
    }

    bool RestSpotInReach(Player* bot)
    {
        // [deaths14] b354 Bhogagnuth L8 began its rest-first ("rests to full: before walking past Mangeclaw") on its
        // walk 19 yd from Mangeclaw L11 (idle, aggro 21): the rare walked in, it died. RestEscape steps out of every
        // reach only before a rest; the eat / drink sat it down where it stood first
        if (!bot || bot->IsInCombat() || !Applies(bot))
            return false;
        // [rest20] A member of a led five answers to its leader's Rest, not to this one: RestsInReach, the only
        // caller, has always thrown this verdict away for it (`!Leader::LedGroup(bot) && RestSpotInReach(bot)`).
        // The gate belongs here, with the judge, because the line below was printed BEFORE it and so claimed a
        // refusal that never bound. That is what it cost: of the 57 "rests not here" lines of the 2026-11-15
        // window, 21 (7 bots of 23 - Namwe, Geron, Bhogagnuth, Bierya, Geefeelli, Nelaeleas, Sylaenas) belong to
        // five members strung out on the walk to the Wailing Caverns entrance. SoloPlayerBot calls them solo
        // (their fellows are out of group-reward distance), nothing refused their rest, nothing had to replace
        // it - and read from the log those 21 lines looked like "refused with no replacement". One judge, one
        // answer, and the line can no longer disagree with it.
        if (Leader::LedGroup(bot))
            return false;
        std::string who;
        std::vector<G3D::Vector3> spots;
        if (!StandsInReach(bot, who, spots) || spots.empty())
            return false;
        static std::mutex restLogLock;
        static std::unordered_map<uint32, std::string> restLogged;
        {
            std::lock_guard<std::mutex> guard(restLogLock);
            std::string& last = restLogged[bot->GetGUID().GetCounter()];
            if (last == who)
                return true;
            last = who;
        }
        LOG_INFO("playerbots", "[deaths14] {} L{} rests not here: within the aggro reach of {} ({} spots out of every "
                 "reach round it)", bot->GetName(), bot->GetLevel(), who, spots.size());
        return true;
    }

    bool HoldsPulls(Player* bot)
    {
        if (!bot || !bot->IsAlive() || bot->IsInCombat())
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end())
            return false;
        if (found->second.recover)
            return true;
        bool const full = bot->GetHealth() >= bot->GetMaxHealth() &&
                          (!bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA));
        return found->second.rest && !full;
    }

    namespace
    {
        // [deaths18] group (its guid counter) -> the target its leader's party judge engaged last, and its name
        std::mutex partyPullLock;
        std::unordered_map<uint32, std::tuple<uint64, std::string, uint32>> partyPull;  // [party65] + noted at

        // the group's judged pull among the mobs that hit the bot this fight (`state`: the bot's, lock held)
        // [party65] a party fight also when the party judge's pull was noted while this fight of the bot was open (or
        // just before its first hit: the pull's walk in): the pack's other mobs killed the member, not the target
        // (b400 Rollothan L11 to a Riverpaw Brute of Kianey's judged pull, Geron L15 to a Stonesplinter Shaman of
        // Maronic's - both logged "no judged pull (attacked)"). The log's label only
        constexpr uint32 PARTY_PULL_LABEL_LEAD_MS = 15 * IN_MILLISECONDS;
        bool PartyPullHit(uint32 group, SoloBotState const& state, std::string& pulled)
        {
            std::lock_guard<std::mutex> guard(partyPullLock);
            auto const found = partyPull.find(group);
            if (found == partyPull.end())
                return false;
            bool const hit = state.attackers.count(std::get<0>(found->second)) > 0;
            uint32 const noted = std::get<2>(found->second);
            bool const during = state.firstMs && noted + PARTY_PULL_LABEL_LEAD_MS >= state.firstMs &&
                                (!state.lastMs || noted <= state.lastMs);
            if (!hit && !during)
                return false;
            pulled = std::get<1>(found->second) + (hit ? "" : ", its pack's other mobs hit it");
            return true;
        }
    }

    void NotePartyPull(Player* leader, Unit* target)
    {
        if (!leader || !target || !leader->GetGroup())
            return;
        std::lock_guard<std::mutex> guard(partyPullLock);
        partyPull[leader->GetGroup()->GetGUID().GetCounter()] = {target->GetGUID().GetRawValue(), target->GetName(),
                                                                 getMSTime()};
    }

    // [surv20] the killer, for the chase line's context. [runpast88] `facts`: the same numbers as values
    std::string DeathContext(Player* bot, Creature* killer, Journal::DeathFacts* facts)
    {
        std::string out = Acore::StringFormat("hp {:.0f}/{:.0f}", float(bot->GetHealth()), float(bot->GetMaxHealth()));
        std::string attackers;
        uint32 count = 0;
        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker)
                continue;
            ++count;
            attackers += (attackers.empty() ? "" : ", ") + attacker->GetName() + " L" +
                         std::to_string(attacker->GetLevel()) + " " +
                         std::to_string(uint32(attacker->GetHealthPct())) + "%";
        }
        out += Acore::StringFormat("; {} attackers: {}", count, attackers);
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found != states.end() && !found->second.attackers.empty())
        {
            std::string came;
            for (auto const& [guid, track] : found->second.attackers)
                came += (came.empty() ? "" : ", ") + track.name + " L" + std::to_string(track.level);
            out += Acore::StringFormat("; hit by {} this fight: {}", found->second.attackers.size(), came);
        }
        // [runpast88] the fight's own numbers as values, from the very same state the sentence above was built from.
        // `attackers` on the death row is the core's list at the hook and the core has already cleared it, so the
        // quantity that answers "more hitters than the judge counted" is THIS one - the creatures whose damage
        // events SoloSafety saw this fight. In the open world it is also the only measure of what the killer dealt:
        // DamageTaken::From only accumulates inside a dungeon (DeathRecorder::IsTracked), which is why all eight
        // b502 deaths - every one of them out in the world - carried damage_from_killer 0.
        if (facts && found != states.end())
        {
            SoloBotState const& state = found->second;
            if (!state.attackers.empty())
            {
                facts->hitBy = uint32(state.attackers.size());
                facts->takenThisFight = uint32(state.taken);
                if (state.lastMs && state.firstMs)
                    facts->fightSec = float(getMSTimeDiff(state.firstMs, state.lastMs)) / float(IN_MILLISECONDS);
                if (killer)
                    if (auto const track = state.attackers.find(killer->GetGUID().GetRawValue());
                        track != state.attackers.end())
                        facts->damageFromKiller = uint32(track->second.damage);
            }
        }
        // [death451] in a dungeon the fight is the five leader's pull ([dun188] / "careful pull" lines), never the
        // bot's last solo engage (b467 Shalaeth L21 in Wailing Caverns: "pulled Pridewing Consort judged 1 mobs",
        // an open-world pull of before its five, named on a death to Lord Cobrahn's 11-mob pull)
        bool const inDungeon = bot->GetMap() && bot->GetMap()->IsDungeon();
        if (found != states.end() && found->second.engaged && !inDungeon)
        {
            EngageNote const& engage = found->second.engage;
            out += Acore::StringFormat("; pulled {} judged {} mobs {:.0f} dmg at hp {:.0f} % mana {:.0f} %",
                                       engage.targetName, engage.mobs, engage.predicted, engage.healthPct,
                                       engage.manaPct);
        }
        // [deaths14] a co-op / help party's pulls are the party judge's (NewRpgCoop::JudgePull), not noted here: b354
        // Gilduck L11 led a help party into five Frostmane trolls and its line said "no judged pull (attacked)"
        // [deaths18] coordinator 2026-11-09: a party fight only when it was the party judge's pull - the target its
        // leader's judge engaged ([deaths17] party engages) is among the mobs that hit the bot this fight. Before,
        // membership alone named it (InParty, any group): b359's 10 "party fights" mostly had no judged pull of their
        // party (Guinudrond's solo walk past a Riverpaw camp among them)
        else if (std::string pulled; bot->GetGroup() && found != states.end() &&
                                     PartyPullHit(bot->GetGroup()->GetGUID().GetCounter(), found->second, pulled))
            out += "; [deaths14] a party fight (the party judge's pull of " + pulled + ")";
        // [deaths78] a led dungeon run's fight is its leader's pull (b406: 9 RFC deaths logged "no judged pull")
        else if (bot->GetMap() && bot->GetMap()->IsDungeon() && Leader::LedBot(bot))
            out += "; [deaths78] a led dungeon fight (the dungeon leader's pull)";
        else
            out += "; no judged pull (attacked)";

        // [runpast87] the chase it kept walking under: priced against what it took since
        // [surv20] ... named as this death's price only where the killer or a mob that hit the bot is one of the
        // chase's own chasers (CloseFight's note): Bami's b486 line put a Leper Gnome L10's 3 dmg beside a death to
        // an Addled Leper L24 in another bot's fight, and read as the killer's price it says the chase model is out
        // by orders of magnitude
        if (found != states.end() && found->second.runPastPriced > 0.0f)
        {
            SoloBotState const& state = found->second;
            bool ofItsChase = killer && state.runPastChasers.count(killer->GetGUID().GetRawValue()) > 0;
            for (auto const& [guid, track] : state.attackers)
                ofItsChase = ofItsChase || state.runPastChasers.count(guid) > 0;
            out += Acore::StringFormat("; [runpast87] its run-past chase priced {:.0f} dmg over {:.0f} s against hp {:.0f}, "
                                       "took {} since{}",
                                       state.runPastPriced, state.runPastSec, state.runPastHp,
                                       state.taken - state.runPastTakenAt,
                                       ofItsChase ? ""
                                                  : " - [surv20] of other mobs, none of its chasers in this fight");
            // [runpast88] ... and as values. `runPastPricedHigh` is the one that matters and was nowhere: the gate
            // judges on RunPast::High() ([death386], two standard deviations of the chasers' swings) while the line
            // prints the mean, so the journal held a number no decision was made on. b502 Lanbuzz: the sentence
            // says 577, the gate let the walk through on 677, and 911 arrived. One quantity, one judge - the
            // judged number goes in its own column beside the printed one.
            if (facts)
            {
                facts->runPastPriced = state.runPastPriced;
                facts->runPastPricedHigh = state.runPastPricedHigh;
                facts->runPastSec = state.runPastSec;
                facts->runPastHp = state.runPastHp;
                facts->runPastTaken = uint32(state.taken - std::min(state.taken, state.runPastTakenAt));
                facts->runPastOfItsChase = ofItsChase;
            }
        }
        // [deaths] how its last route search saw the mobs that hit it: refused (a walk round them was searched),
        // priced (the walk may pass their reach), unseen (out of the search's range), in a fight of its own
        // ([surv21]: a mob whose fight is not the bot's is left out of the pricing altogether)
        if (found != states.end() && found->second.routeMs && !found->second.attackers.empty())
        {
            SoloBotState const& state = found->second;
            std::string seen;
            // [runpast88] the four words of that sentence are four counters, because the question the user asks of
            // them - "the deaths where a mob the search never saw came" - is a WHERE and not a LIKE. b502 Emdyl:
            // three priced, one unseen (Slark, a patrol out of the search's range), and the search itself 233 s old
            uint32 priced = 0, refusedSeen = 0, unseen = 0, otherFight = 0;
            for (auto const& [guid, track] : state.attackers)
            {
                auto const mob = state.routeMobs.find(guid);
                if (mob != state.routeMobs.end() && mob->second)
                    ++refusedSeen;
                else if (mob != state.routeMobs.end())
                    ++priced;
                else if (state.routeOthersFight.count(guid))
                    ++otherFight;
                else
                    ++unseen;
                seen += (seen.empty() ? "" : ", ") + track.name + " " +
                        (mob != state.routeMobs.end() ? (mob->second ? "refused" : "priced")
                         : state.routeOthersFight.count(guid) ? "in a fight of its own"  // [surv21]
                                                              : "unseen");
            }
            out += Acore::StringFormat("; last route search {:.0f} s before ({}{}): {}",
                                       float(getMSTimeDiff(state.routeMs, getMSTime())) / IN_MILLISECONDS,
                                       state.routeComplete ? "complete" : "partial",
                                       state.routeThroughRefused ? ", through a refused reach" : "", seen);
            if (facts)
            {
                facts->routeAgeMs = getMSTimeDiff(state.routeMs, getMSTime());
                facts->routeComplete = state.routeComplete;
                facts->routeThroughRefused = state.routeThroughRefused;
                facts->routePriced = priced;
                facts->routeRefused = refusedSeen;
                facts->routeUnseen = unseen;
                facts->routeOtherFight = otherFight;
            }
        }
        return out;
    }

    namespace
    {
        // the share of its speed a daze (spell 1604, Unit::AttackerStateUpdate from behind) takes off, by its data.
        // [runpast88] the sign is the SPELL'S: MOD_DECREASE_SPEED carries -50, so this returns -0.5 and the dazed
        // speed is `vb * (1 + share)`. The std::min keeps the deepest slow and leaves 0 when daze is off, which
        // reads as "no daze" in that same multiplication - the convention Chase::Of documents and relies on.
        float DazedShare()
        {
            float dazed = 0.0f;
            if (sWorld->getBoolConfig(CONFIG_ENABLE_DAZE))
                if (SpellInfo const* daze = sSpellMgr->GetSpellInfo(1604))
                    for (SpellEffectInfo const& effect : daze->GetEffects())
                        if (effect.ApplyAuraName == SPELL_AURA_MOD_DECREASE_SPEED)
                            dazed = std::min(dazed, float(effect.CalcValue()) / 100.0f);
            return dazed;
        }

        // a hit from behind is never dodged or parried (Unit::RollMeleeOutcomeAgainst): the melee lands that much more
        float BehindFactor(Player* bot)
        {
            float const avoided =
                (bot->GetFloatValue(PLAYER_DODGE_PERCENTAGE) + bot->GetFloatValue(PLAYER_PARRY_PERCENTAGE)) / 100.0f;
            return avoided > 0.0f && avoided < 1.0f ? 1.0f / (1.0f - avoided) : 1.0f;
        }

        // the chase of a mob of run speed `vm`, leash timer `leashSec`, melee dps `melee` (its melee reach `reach`):
        // how long it lasts and how long its melee lands
        Chase::Window ChaseTimes(Player* bot, float vm, float leashSec, float melee, float reach,
                                 bool withSpeed = true, float botSpeed = 0.0f)
        {
            float const leashRadius = sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS);
            // [death460] `botSpeed` > 0: the bot's speed given (a ghost priced as the body it rises into)
            float vb = botSpeed > 0.0f ? botSpeed : bot->GetSpeed(bot->IsInWater() ? MOVE_SWIM : MOVE_RUN);
            // [lag360] the speed piece the run-past executor casts as the chase starts (SurvivalKit::RunPastSpeed:
            // instant, castable now, lasting the chase, no daze of its own on being struck)
            if (withSpeed)
            {
                float pct = 0.0f;
                if (SurvivalKit::RunPastSpeed(bot, std::max(leashSec, leashRadius / std::max(vb, 0.1f)) + 1.0f, pct))
                    vb *= 1.0f + pct;
            }
            // [runpast88] the arithmetic itself is a seam now (Chase::Of in ChaseModel.h): the same six numbers in
            // and the same two out, with no world in it, so a chase recorded in a real death can be put back on the
            // test stand. [runpast89] and the window now also answers whether the chase ENDS at all.
            return Chase::Of(vb, vm, leashSec, leashRadius, DazedShare(), melee, reach);
        }
    }

    float RunPast::High() const
    {
        if (damage == std::numeric_limits<float>::max())
            return damage;
        return damage + 2.0f * std::sqrt(std::max(variance, 0.0f));
    }

    void RunPast::Add(RunPast const& chase)
    {
        damage = chase.damage == std::numeric_limits<float>::max() || damage == std::numeric_limits<float>::max()
                     ? std::numeric_limits<float>::max()
                     : damage + chase.damage;
        variance += chase.variance;
        seconds = std::max(seconds, chase.seconds);
    }

    RunPast RunPastChase(Player* bot, Creature* mob, bool withSpeed, float botSpeed)
    {
        RunPast out;
        if (!bot || !mob || sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS) <= 0.0f || mob->isWorldBoss())
        {
            out.damage = std::numeric_limits<float>::max();
            return out;
        }
        float variance = 0.0f;
        float const melee = MeleeDps(mob, bot, &variance);
        // [death401] a chase in the water is at both swim speeds (MoveSplineInit::SelectSpeedType: MOVE_SWIM for a
        // swimming unit): b453 Mytaeleas / Enyssia / Salleas died to Greymist chases off Mist's Edge priced at run
        // speed
        float const chaseSpeed = mob->GetSpeed(bot->IsInWater() ? MOVE_SWIM : MOVE_RUN);
        Chase::Window const window = ChaseTimes(bot, chaseSpeed, float(mob->GetLeashTimer()), melee,
                                                mob->GetMeleeRange(bot), withSpeed, botSpeed);
        // [runpast89] a chase with no end has no price either: the same "no run past" answer a boss or a server
        // without a leash radius gets, and for the same reason - there is no number of seconds to multiply
        if (!window.ends)
        {
            out.damage = std::numeric_limits<float>::max();
            out.seconds = 0.0f;
            return out;
        }
        out.seconds = window.until;
        float const casts = ChaseSpellDamage(mob, out.seconds);
        out.damage =
            melee * BehindFactor(bot) * window.exposure + (casts > 0.0f ? casts : SpellDpsOf(mob) * out.seconds);
        out.variance = variance * BehindFactor(bot) * BehindFactor(bot) * window.exposure;
        return out;
    }

    bool QuestObjectSummonsLose(Player* bot, GameObject* go, std::string& why)
    {
        why.clear();
        if (!bot || !go || !Applies(bot))
            return false;
        uint32 const trapEntry = go->GetGOInfo()->GetLinkedGameObjectEntry();
        GameObjectTemplate const* trap = trapEntry ? sObjectMgr->GetGameObjectTemplate(trapEntry) : nullptr;
        if (!trap || trap->type != GAMEOBJECT_TYPE_TRAP)
            return false;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(trap->trap.spellId);
        if (!spell)
            return false;
        struct Foe
        {
            float hp, dps, variance;  // variance: of its damage per second of the fight
        };
        std::vector<Foe> foes;
        // [death410] the mobs already standing by the object (their aggro reach, wander area or patrol beat covers
        // it) are in that fight too, killed first
        G3D::Vector3 const at(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
        std::string standing;
        {
            float const look = sPlayerbotAIConfig.SightDistance;
            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(go, look);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(go, units, check);
            Cell::VisitObjects(go, searcher, look);
            for (Unit* unit : units)
            {
                Creature* mob = unit->ToCreature();
                if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                    !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID())
                    continue;
                bool covers = InDisc(AggroDisc(mob, bot), at);
                if (!covers && mob->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE)
                {
                    std::vector<G3D::Vector3> beat;
                    float sampleSec = 0.0f;
                    if (PullPlanning::PatrolForecast(mob, beat, sampleSec))
                        for (G3D::Vector3 const& step : beat)
                            covers = covers || std::hypot(step.x - at.x, step.y - at.y) <= AggroDisc(mob, bot).w;
                }
                if (!covers)
                    continue;
                foes.push_back({float(mob->GetHealth()), MobDps(mob, bot), DamageVariancePerSec(mob, bot)});
                if (standing.size() < 120)
                    standing += Acore::StringFormat("{}{} L{}", standing.empty() ? "" : ", ", mob->GetName(),
                                                    mob->GetLevel());
            }
        }
        // the trap's summons (SPELL_EFFECT_SUMMON: the creature in its misc value, its points the count): the
        // template's health and melee (RunPastTemplate's numbers), its swing's roll uniform base .. 1.5 x base
        uint32 count = 0;
        CreatureTemplate const* named = nullptr;
        for (SpellEffectInfo const& effect : spell->GetEffects())
        {
            if (effect.Effect != SPELL_EFFECT_SUMMON || effect.MiscValue <= 0)
                continue;
            CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(uint32(effect.MiscValue));
            CreatureBaseStats const* stats =
                tmpl ? sObjectMgr->GetCreatureBaseStats(tmpl->maxlevel, tmpl->unit_class) : nullptr;
            if (!tmpl || !stats)
                continue;
            int32 const n = std::max(1, effect.CalcValue());
            float const swing = std::max(float(tmpl->BaseAttackTime), 1.0f) / float(IN_MILLISECONDS);
            float const base = stats->GenerateBaseDamage(tmpl);
            float const power = float(stats->AttackPower) / 14.0f * swing;
            float const low = (base + power) * tmpl->DamageModifier;
            float const high = (base * 1.5f + power) * tmpl->DamageModifier;
            float const mean = (low + high) / 2.0f;
            float const square = (low * low + low * high + high * high) / 3.0f;  // E[h^2] of the uniform roll
            // [death434] an off-hand swing deals half: its spread a quarter of the main hand's
            float const offHand = TemplateHands(tmpl) - 1.0f;
            for (int32 i = 0; i < n; ++i)
                foes.push_back({float(stats->GenerateHealth(tmpl)), mean * (1.0f + offHand) / swing,
                                std::max(square - mean * mean, 0.0f) * (1.0f + offHand * offHand) / swing});
            count += uint32(n);
            named = tmpl;
        }
        if (!count)
            return false;
        // fought one after another at the bot's damage, the ones alive hitting on meanwhile; priced at the judge's
        // high end ([death386]: two standard deviations of the rolls) against its health now and the heals its mana
        // holds
        DpsEstimate::Result const own = OwnDamage(bot, nullptr);
        // [dpszero1] no floor: with no estimate the trap is not walled on a number nobody computed, and with a
        // measured zero the summons never fall, which walls it with no number needed
        KillRate::Own const burst{own.estimated, own.burstDps};
        if (!KillRate::Judgeable(burst))
            return false;
        if (KillRate::StateOf(burst) == KillRate::State::DealsNothing)
        {
            why = Acore::StringFormat("its trap summons {} x {} L{} and its own damage is measured at zero", count,
                                      named->Name, named->maxlevel);
            return true;
        }
        float aliveDps = 0.0f;
        float aliveVariance = 0.0f;
        for (Foe const& foe : foes)
        {
            aliveDps += foe.dps;
            aliveVariance += foe.variance;
        }
        float seconds = 0.0f;
        float damage = 0.0f;
        float variance = 0.0f;
        for (Foe const& foe : foes)
        {
            float const kill = KillRate::KillSec(burst, foe.hp);
            seconds += kill;
            damage += aliveDps * kill;
            variance += aliveVariance * kill;
            aliveDps -= foe.dps;
            aliveVariance -= foe.variance;
        }
        constexpr float HIGH_END_DEVIATIONS = 2.0f;
        float const highEnd = damage + HIGH_END_DEVIATIONS * std::sqrt(std::max(variance, 0.0f));
        float const mana = float(bot->GetPower(POWER_MANA));
        float const ehp = float(bot->GetHealth()) +
                          SurvivalKit::FightHeals(bot, seconds, float(bot->GetHealth()), mana, own.manaPerSec,
                                                  highEnd / std::max(seconds, 1.0f), true);  // [death442]
        why = Acore::StringFormat("its trap summons {} x {} L{}{}{}: {:.0f} dmg at the high end over {:.0f} s at ehp "
                                  "{:.0f}",
                                  count, named->Name, named->maxlevel, standing.empty() ? "" : " beside ", standing,
                                  highEnd, seconds, ehp);
        return highEnd >= ehp;
    }

    float TemplateHands(CreatureTemplate const* tmpl)
    {
        if (!tmpl)
            return 1.0f;
        static std::mutex cacheLock;
        static std::unordered_map<uint32, float> cache;  // entry -> its hands (the world data, read once)
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            if (auto const found = cache.find(tmpl->Entry); found != cache.end())
                return found->second;
        }
        int8 id = 1;  // the default equipment (Creature::LoadEquipment of a spawn with equipment_id 1)
        EquipmentInfo const* equipment = sObjectMgr->GetEquipmentInfo(tmpl->Entry, id);
        ItemEntry const* offHand = equipment ? sItemStore.LookupEntry(equipment->ItemEntry[1]) : nullptr;
        float const hands = offHand && offHand->ClassID == ITEM_CLASS_WEAPON ? 1.5f : 1.0f;
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[tmpl->Entry] = hands;
        return hands;
    }

    RunPast RunPastTemplate(Player* bot, CreatureTemplate const* tmpl)
    {
        RunPast out;
        CreatureBaseStats const* stats =
            tmpl ? sObjectMgr->GetCreatureBaseStats(tmpl->maxlevel, tmpl->unit_class) : nullptr;
        if (!bot || !stats || sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS) <= 0.0f ||
            (tmpl->type_flags & CREATURE_TYPE_FLAG_BOSS_MOB))
        {
            out.damage = std::numeric_limits<float>::max();
            return out;
        }
        // Creature::UpdateEntry / the stat system: weapon damage base .. 1.5 x base, plus attack power / 14 per second
        // of swing, times the template's damage modifier
        float const swing = std::max(float(tmpl->BaseAttackTime), 1.0f) / float(IN_MILLISECONDS);
        float const base = stats->GenerateBaseDamage(tmpl);
        float const hit =
            ((base + base * 1.5f) / 2.0f + float(stats->AttackPower) / 14.0f * swing) * tmpl->DamageModifier;
        float const melee = hit * TemplateHands(tmpl) / swing;  // [death434]
        // Creature::GetLeashTimer of a mob of this template (the core's own numbers)
        constexpr int32 LEASH_TIMER_OFFSET = 11;
        float const leashSec =
            float(std::max<int32>(LEASH_TIMER_OFFSET, LEASH_TIMER_OFFSET + int32(tmpl->minlevel / 10) - 2));
        float const chaseSpeed = bot->IsInWater() ? tmpl->speed_swim * baseMoveSpeed[MOVE_SWIM]  // [death401]
                                                  : tmpl->speed_run * baseMoveSpeed[MOVE_RUN];
        Chase::Window const window = ChaseTimes(bot, chaseSpeed, leashSec, melee, NOMINAL_MELEE_RANGE);
        if (!window.ends)  // [runpast89]
        {
            out.damage = std::numeric_limits<float>::max();
            out.seconds = 0.0f;
            return out;
        }
        out.seconds = window.until;
        out.damage = melee * BehindFactor(bot) * window.exposure;
        return out;
    }

    float RunPastPool(Player* bot, float damage, float seconds, Chase::Heal const& heal, bool atFull)
    {
        if (!bot)
            return 0.0f;
        float const health = atFull ? float(bot->GetMaxHealth()) : float(bot->GetHealth());
        float const mana = atFull ? float(bot->GetMaxPower(POWER_MANA)) : float(bot->GetPower(POWER_MANA));
        if (damage == std::numeric_limits<float>::max())
            return health;  // no chase to walk forward: nothing of the kit can be said to land
        return health + Chase::Under(heal, seconds, damage, health, float(bot->GetMaxHealth()), mana).healed;
    }

    float RunPastPool(Player* bot, float damage, float seconds, bool atFull)
    {
        Chase::Heal heal;
        if (bot)
            SurvivalKit::RunPastHealPiece(bot, heal);
        return RunPastPool(bot, damage, seconds, heal, atFull);
    }

    bool SurvivesRunPast(Player* bot, float damage, float seconds)
    {
        if (!bot || damage == std::numeric_limits<float>::max())
            return false;
        // [runpast89] the gate and the number the journal line prints are ONE quantity: the bot dies exactly when the
        // chase's damage is not below the pool Chase::Under walks out for it
        return damage < RunPastPool(bot, damage, seconds);
    }

    bool RunOutSurvives(Player* bot, std::vector<G3D::Vector3> const& points, std::string& why)
    {
        why.clear();
        if (!bot || points.empty())
            return false;
        float const sight = sPlayerbotAIConfig.SightDistance;
        float const range = std::min(sight + MaxWanderDistance(bot->GetMapId()) + MAX_AGGRO_RADIUS,
                                     std::max(bot->GetMap()->GetVisibilityRange(), sight));
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);
        std::vector<Creature*> idle;
        for (Unit* unit : units)
            if (Creature* mob = unit->ToCreature())
                if (mob->IsAlive() && !mob->IsInCombat() && mob->IsHostileTo(bot) &&
                    mob->HasReactState(REACT_AGGRESSIVE) && !mob->GetCharmerOrOwnerGUID() && !mob->IsCivilian())
                    idle.push_back(mob);
        // the walk sampled at the route search's step from the bot on
        constexpr float STEP = 4.0f;
        std::vector<G3D::Vector3> samples{
            G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ())};
        for (G3D::Vector3 const& p : points)
        {
            G3D::Vector3 const last = samples.back();
            float const gap = (p - last).length();
            for (float d = STEP; d < gap; d += STEP)
                samples.push_back(last + (p - last) * (d / gap));
            samples.push_back(p);
        }
        float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        std::vector<Creature*> chasers;
        for (Creature* mob : idle)
        {
            G3D::Vector4 const disc = AggroDisc(mob, bot);
            std::vector<G3D::Vector3> beat;
            float sampleSec = 0.0f;
            if (mob->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE)
                PullPlanning::PatrolForecast(mob, beat, sampleSec);
            bool reaches = false;
            for (size_t i = 0; !reaches && i < samples.size(); ++i)
            {
                reaches = InDisc(disc, samples[i]);
                for (size_t k = 0; !reaches && k < beat.size(); ++k)
                    reaches = InDisc(G3D::Vector4(beat[k].x, beat[k].y, beat[k].z, disc.w), samples[i]);
            }
            if (reaches)
                chasers.push_back(mob);
        }
        // their engage calls bring the idle mobs within the assistance radius of each (Creature::CallAssistance)
        for (size_t i = 0; assist > 0.0f && i < chasers.size(); ++i)
            for (Creature* c : idle)
                if (std::find(chasers.begin(), chasers.end(), c) == chasers.end() &&
                    chasers[i]->IsWithinDistInMap(c, assist) && c->CanAssistTo(chasers[i], bot))
                    chasers.push_back(c);
        RunPast total;
        for (Creature* mob : chasers)
        {
            total.Add(RunPastChase(bot, mob));
            if (why.size() < 160)
                why += Acore::StringFormat("{}{} L{}", why.empty() ? "" : ", ", mob->GetName(), mob->GetLevel());
        }
        bool const lives = SurvivesRunPast(bot, total.High(), total.seconds);
        why = Acore::StringFormat("{} chaser(s) ({}): {:.0f} dmg at the high end over {:.0f} s, hp {}", chasers.size(),
                                  why.empty() ? "-" : why, chasers.empty() ? 0.0f : total.High(), total.seconds,
                                  bot->GetHealth());
        return lives;
    }

    void CommitRunPast(Player* bot, std::vector<G3D::Vector3> const& points,
                       std::vector<G3D::Vector3> const& searched)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        // [death452] the set of the search this leg was cut from: the latest search that started at the searched
        // walk's start (or its end: a search from the destination back, walked the other way)
        std::vector<G3D::Vector3> const& walk = searched.empty() ? points : searched;
        SoloBotState::SearchRunPast const* match = nullptr;
        for (auto it = state.searchRunPast.rbegin(); !walk.empty() && it != state.searchRunPast.rend(); ++it)
        {
            auto const startsAt = [&](G3D::Vector3 const& at)
            { return std::hypot(at.x - it->from.x, at.y - it->from.y) <= RUN_PAST_ON_LEG; };
            if (startsAt(walk.front()) || startsAt(walk.back()))
            {
                match = &*it;
                break;
            }
        }
        size_t const before = state.pendingRunPast.size();
        state.legRunPast = match ? match->chases : state.pendingRunPast;
        state.legStatic = match ? match->statics : std::vector<G3D::Vector4>();
        state.legHp = match ? match->hp : float(bot->GetHealth());
        bool const runPastLeg = !state.legRunPast.empty() || !state.legStatic.empty();
        if (match && (state.legRunPast.size() != before || !state.legStatic.empty()))
            LOG_INFO("playerbots", "[death452] {} L{} commits its leg's run past from the search at ({:.0f},{:.0f}): {} "
                     "mob(s) in sight, {} reach(es) beyond it (the search from its spot had {})",
                     bot->GetName(), bot->GetLevel(), match->from.x, match->from.y, state.legRunPast.size(),
                     state.legStatic.size(), before);
        state.searchRunPast.clear();
        state.legPoints = runPastLeg ? points : std::vector<G3D::Vector3>();
        state.legSearched = runPastLeg ? searched : std::vector<G3D::Vector3>();  // [runpast15]
        state.legNamed.clear();
    }

    void NoteStaticRunPast(Player* bot, std::vector<G3D::Vector4> const& reaches)
    {
        if (!bot || reaches.empty())
            return;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || found->second.searchRunPast.empty())
            return;
        std::vector<G3D::Vector4>& statics = found->second.searchRunPast.back().statics;
        statics.insert(statics.end(), reaches.begin(), reaches.end());
    }

    bool LegPricedAbove(Player* bot)
    {
        if (!bot)
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        return found != states.end() && found->second.legHp > float(bot->GetHealth());
    }

    namespace
    {
        // [death452] the attacker came from a beyond-sight reach the committed leg priced as a run past
        bool InLegStatic(std::vector<G3D::Vector4> const& statics, Creature const* mob)
        {
            Position const home = mob->GetHomePosition();
            for (G3D::Vector4 const& disc : statics)
                if (std::hypot(home.GetPositionX() - disc.x, home.GetPositionY() - disc.y) <= disc.w)
                    return true;
            return false;
        }
    }

    namespace
    {
        // [runpast15] the stop of the walk whose searches run on this thread (WalkStopScope)
        thread_local bool walkStopSet = false;
        thread_local G3D::Vector3 walkStop;

        // [runpast15] the stretch `points` crosses no reach of an idle hostile mob outside `chasing` (its aggro range
        // with the bot's combat reach round the mob; a wanderer's round its home with its wander distance - the reach
        // RouteCosts walls / prices): a walk on past the leg's end wakes nobody new
        bool StretchClear(Player* bot, std::vector<G3D::Vector3> const& points,
                          std::unordered_map<uint64, float> const& chasing)
        {
            if (points.size() < 2)
                return false;
            float length = 0.0f;
            for (size_t i = 1; i < points.size(); ++i)
                length += (points[i] - points[i - 1]).length();
            float const wander = std::min(MaxWanderDistance(bot->GetMapId()), bot->GetMap()->GetVisibilityRange());
            float const range = length + MAX_AGGRO_RADIUS + wander;
            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(bot, range);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
            Cell::VisitObjects(bot, searcher, range);
            for (Unit* unit : units)
            {
                Creature* mob = unit->ToCreature();
                if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->IsCivilian() || mob->IsTotem() ||
                    !mob->IsHostileTo(bot) || !mob->HasReactState(REACT_AGGRESSIVE) ||
                    mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) ||
                    mob->GetCharmerOrOwnerGUID() || chasing.count(mob->GetGUID().GetRawValue()))
                    continue;
                float const aggro = mob->GetAggroRange(bot);
                if (aggro <= 0.0f)
                    continue;
                Position centre = mob->GetPosition();
                float radius = aggro + bot->GetCombatReach();
                if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                {
                    centre = mob->GetHomePosition();
                    radius += mob->GetWanderDistance();
                }
                G3D::Vector3 const c(centre.GetPositionX(), centre.GetPositionY(), 0.0f);
                for (size_t i = 1; i < points.size(); ++i)
                {
                    G3D::Vector3 const a(points[i - 1].x, points[i - 1].y, 0.0f);
                    G3D::Vector3 const ab = G3D::Vector3(points[i].x, points[i].y, 0.0f) - a;
                    float const t = std::clamp((c - a).dot(ab) / std::max(ab.squaredLength(), 0.01f), 0.0f, 1.0f);
                    if ((a + ab * t - c).squaredLength() <= radius * radius)
                        return false;
                }
            }
            return true;
        }

        // [runpast15] the walk on from `here` along `walk` (from its point nearest `here`), `yards` long; empty when
        // the walk ends sooner (its stop lies inside the chase) or `here` is off it
        std::vector<G3D::Vector3> WalkOn(G3D::Vector3 const& here, std::vector<G3D::Vector3> const& walk, float yards)
        {
            std::vector<G3D::Vector3> out;
            size_t nearest = walk.size();
            float best = 8.0f * 8.0f;  // the farRoute tolerance (NewRpgBaseAction::MoveFarToDirect)
            for (size_t i = 0; i < walk.size(); ++i)
                if (float const d = (walk[i] - here).squaredLength(); d < best)
                {
                    best = d;
                    nearest = i;
                }
            if (nearest == walk.size())
                return out;
            out.push_back(here);
            float left = yards;
            G3D::Vector3 at = here;
            for (size_t i = nearest + 1; i < walk.size(); ++i)
            {
                float const d = (walk[i] - at).length();
                if (d >= left)
                {
                    out.push_back(at + (walk[i] - at) * (left / std::max(d, 0.01f)));
                    return out;
                }
                left -= d;
                at = walk[i];
                out.push_back(at);
            }
            out.clear();  // the walk ends inside the chase
            return out;
        }
    }

    WalkStopScope::WalkStopScope(G3D::Vector3 const& stop) : had(walkStopSet), previous(walkStop)
    {
        walkStopSet = true;
        walkStop = stop;
    }

    WalkStopScope::~WalkStopScope()
    {
        walkStopSet = had;
        walkStop = previous;
    }

    // [spot10] user 2026-11-14: "a player must not walk into a mob's zone and hit it when he has no desire to". A
    // walk is walked in legs, and a leg ends where its LENGTH puts it - the bot's sight along the path, else the
    // path's own end (MoveFarToDirect / GraphStage / WalkAround: Leader::Travel::PointOnPath). Nobody asked whether
    // that spot lies inside an idle hostile's aggro disc. There the bot stands until its next look while the
    // wanderer whose area it is walks over and opens a fight no judge priced: of the 498 such opens of b491 (one per
    // bot and mob place, not per line), 487 had the bot already inside the disc, the median 10 yd into a median
    // reach of 22 (only 10 of those 487 within 2 yd of the rim, where a mis-measured radius would have put them),
    // the mob met at a median of 5 yd, 398 of them wanderers - and 367 of the 383
    // that printed a destination stood more than 40 yd from it. The stop of a leg, not the end of a walk.
    // The stop is moved back along the SAME walk to the point nearest the one its length picked that no such disc
    // covers. Nothing new is measured: the discs are AggroDisc's (the fight judge's own), the floor on the step is
    // the walker's MOVE_FAR_PROGRESS (`progress` - the margin its loop detector asks of a leg, so a moved stop still
    // closes the gap by as much as that detector needs), and no point past the leg's end is taken (the [guild13]
    // rule: a solo bot commits only as far as it sees). The mobs it means to meet are left out - the one it has
    // targeted, and any whose reach covers the walk's own stop (its objective, its camp: it goes there on purpose),
    // so a bot that wants the fight is not held back by this. Every point of the leg covered -> false, and the leg's
    // end stands as it was: the answer there is to walk on through, never to stand still.
    bool LegStop(Player* bot, std::vector<G3D::Vector3> const& points, float progress, G3D::Vector3& end,
                 std::string& who)
    {
        SlowScope slow("leg stop", bot ? &bot->GetName() : nullptr);  // [perf26] a unit scan, as WorkSpot's is
        who.clear();
        if (!bot || !bot->IsAlive() || bot->IsInCombat() || !Applies(bot) || points.size() < 2)
            return false;
        G3D::Vector3 const asked = end;
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        // the walk's own stop: the destination its searches are measured to (WalkStopScope), else this path's end
        G3D::Vector3 const stop = walkStopSet ? walkStop : points.back();
        float const look = std::min(sPlayerbotAIConfig.SightDistance +
                                        std::min(MaxWanderDistance(bot->GetMapId()),
                                                 bot->GetMap()->GetVisibilityRange()),
                                    bot->GetMap()->GetVisibilityRange());
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, look);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, look);
        ObjectGuid const target = bot->GetTarget();
        std::vector<G3D::Vector4> discs;
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID() || mob->GetGUID() == target)
                continue;
            G3D::Vector4 const disc = AggroDisc(mob, bot);
            if (InDisc(disc, stop))
                continue;  // the walk ends in that reach: the bot means to stand there
            discs.push_back(disc);
            if (InDisc(disc, asked))
                who += Acore::StringFormat("{}{} L{}", who.empty() ? "" : ", ", mob->GetName(), mob->GetLevel());
        }
        if (who.empty())
            return false;  // the stop its length picked is out of every reach already
        auto const clear = [&discs](G3D::Vector3 const& at)
        { return std::none_of(discs.begin(), discs.end(), [&at](G3D::Vector4 const& d) { return InDisc(d, at); }); };
        // the yards along the walk of every point of it, and of the leg's end on it (the stretch it lies on)
        std::vector<float> along(points.size(), 0.0f);
        for (size_t i = 1; i < points.size(); ++i)
            along[i] = along[i - 1] + (points[i] - points[i - 1]).length();
        float alongEnd = along.back();
        float nearest = std::numeric_limits<float>::max();
        for (size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const leg = points[i] - points[i - 1];
            float const len = leg.length();
            float const dot = (asked.x - points[i - 1].x) * leg.x + (asked.y - points[i - 1].y) * leg.y +
                              (asked.z - points[i - 1].z) * leg.z;
            float const t = len > 0.0f ? std::clamp(dot / (len * len), 0.0f, 1.0f) : 0.0f;
            G3D::Vector3 const on = points[i - 1] + leg * t;
            if (float const d = (on - asked).length(); d < nearest)
            {
                nearest = d;
                alongEnd = along[i - 1] + len * t;
            }
        }
        // the gap to the walk's stop, measured as the walker's own loop detector measures it (bot->GetDistance to
        // the destination, in three dimensions), so the margin below is the very one that detector asks of a leg
        float const gapNow = (here - stop).length();
        bool found = false;
        float best = 0.0f;
        for (size_t i = 0; i < points.size(); ++i)
        {
            if (along[i] > alongEnd || (points[i] - here).length() <= progress || !clear(points[i]))
                continue;
            // a moved stop still closes the gap to the walk's stop by the margin the loop detector asks of a leg
            if (gapNow - (points[i] - stop).length() <= progress)
                continue;
            if (float const off = alongEnd - along[i]; !found || off < best)
            {
                found = true;
                best = off;
                end = points[i];
            }
        }
        if (!found)
            who.clear();
        return found;
    }

    bool HasRunPastLeg(Player* bot)
    {
        if (!bot)
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        return found != states.end() && (!found->second.legRunPast.empty() || !found->second.legStatic.empty());
    }

    bool PendingRunPast(Player* bot)
    {
        if (!bot)
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        return found != states.end() && !found->second.pendingRunPast.empty();
    }

    bool HoldsPullSpot(Player* bot, Unit* target)
    {
        if (!bot || !target || target->IsInCombat() || !target->IsAlive())
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        return found != states.end() && found->second.pullSpot && found->second.pullTarget == target->GetGUID();
    }

    bool RunPastChaser(Player* bot, Unit* target)
    {
        // only while it walks the leg on (the executor's last look said so and it moves): a bot that stopped to fight
        // (the run past given up, a survival "flee stops ... it fights") hits as before
        if (!bot || !target || target == bot || !target->IsCreature() || !bot->IsInCombat() || !bot->isMoving())
            return false;
        bool chaser = false;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end())
                return false;
            bool const runPast = found->second.runPastWalking &&
                                 (found->second.legRunPast.count(target->GetGUID().GetRawValue()) ||
                                  InLegStatic(found->second.legStatic, target->ToCreature()));  // [death452]
            // [death409] ... and a survival flee on the move: its price is the leash from the flee's start, a stun or
            // hit on a chaser renews it (Unit::AtTargetAttacked; UNIT_STATE_LOST_CONTROL extends it while stunned,
            // shared with its assistants) - b453 Geron L19 took 1722 against a chase priced 486
            bool const flees = found->second.fleeing && target->GetVictim() == bot;
            if (!runPast && !flees)
                return false;
            chaser = true;
            fresh = !found->second.runPastHitLogged;
            found->second.runPastHitLogged = true;
        }
        if (fresh)
            LOG_INFO("playerbots", "[death384] {} L{} holds its hit at {} L{}: a chaser of its run past (a hit renews "
                     "the leash)", bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel());
        return chaser;
    }

    void SetRunPastWalking(Player* bot, bool walking)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        auto found = states.find(bot->GetGUID().GetCounter());
        if (found != states.end())
            found->second.runPastWalking = walking;
    }

    bool RunPastStep(Player* bot, std::vector<G3D::Vector3>& rest, std::string& line)
    {
        rest.clear();
        line.clear();
        if (!bot || !bot->IsAlive() || !bot->IsInCombat() || !Applies(bot) || Disengaging(bot))
            return false;
        std::unordered_map<uint64, float> set;
        std::vector<G3D::Vector3> points;
        std::vector<G3D::Vector4> statics;  // [death452]
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end() || (found->second.legRunPast.empty() && found->second.legStatic.empty()) ||
                found->second.legPoints.size() < 2)
                return false;
            set = found->second.legRunPast;
            statics = found->second.legStatic;
            points = found->second.legPoints;
            // [deaths13] the bot's own judged pull is among its attackers: it fights that pull where it stands, no
            // walk on past the camp with it in tow. b353 Eraelor L9 (Gnarlpine Defender) and Nurlian L7 (Wrathscale
            // Siren): "PULL ... brings 1", then "RUN_PAST keeps walking past" the very mob it pulled (it was priced on
            // the leg), the walk dragged the fight into the next camp - judged 1, came 3 / 2, both died
            // [death453] ... any pull of its own this fight: the leg was committed before the bot chose this fight
            // [death466] ... an attacker of that engage (its target, or a mob its judge counted once its fight is open:
            // OfEngage), not any attacker while an engage is recorded. b470 Gilduck L18: its engage of a Redridge
            // Mystic was stale, the run past's chaser Redridge Brute was taken for "its own pull", the run past ended
            // in the gnoll camp with 2 on it, unjudged ([death398]) - dead
            if (found->second.engaged)
                for (Unit* attacker : bot->getAttackers())
                    if (attacker && attacker->IsAlive() && OfEngage(bot, found->second, attacker))
                    {
                        LOG_INFO("playerbots", "[deaths13] {} L{} ends its run past: it fights its own pull of {} L{} "
                                 "(the leg's chase set holds {} mobs)",
                                 bot->GetName(), bot->GetLevel(), attacker->GetName(), attacker->GetLevel(),
                                 found->second.legRunPast.size());
                        found->second.legRunPast.clear();
                        found->second.legStatic.clear();
                        found->second.legPoints.clear();
                        found->second.legNamed.clear();
                        return false;
                    }
        }
        // [death452] an attacker from a beyond-sight reach the leg priced as a run past: in its set, its chase now
        if (!statics.empty())
        {
            std::vector<std::pair<uint64, float>> added;
            for (Unit* attacker : bot->getAttackers())
                if (Creature* mob = attacker ? attacker->ToCreature() : nullptr)
                    if (mob->IsAlive() && !set.count(mob->GetGUID().GetRawValue()) && InLegStatic(statics, mob))
                        added.push_back({mob->GetGUID().GetRawValue(), RunPastChase(bot, mob).damage});
            if (!added.empty())
            {
                std::lock_guard<std::mutex> guard(lock);
                SoloBotState& state = states[bot->GetGUID().GetCounter()];
                for (auto const& [key, damage] : added)
                {
                    set[key] = damage;
                    state.legRunPast[key] = damage;
                }
            }
        }
        // still on the leg: its nearest point within the far walk's own tolerance (MoveFarToDirect's farRoute, 8 yd);
        // at its end or off it the leg is over
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        constexpr float ON_LEG = 8.0f;  // the farRoute tolerance (NewRpgBaseAction::MoveFarToDirect)
        size_t nearest = points.size();
        float best = ON_LEG * ON_LEG;
        for (size_t i = 0; i < points.size(); ++i)
            if (float const d = (points[i] - here).squaredLength(); d < best)
            {
                best = d;
                nearest = i;
            }
        // between two points the nearest corner may be far: the segment's own distance counts too
        for (size_t i = 1; nearest == points.size() && i < points.size(); ++i)
        {
            G3D::Vector3 const ab = points[i] - points[i - 1];
            float const len2 = std::max(ab.squaredLength(), 0.01f);
            float const t = std::clamp((here - points[i - 1]).dot(ab) / len2, 0.0f, 1.0f);
            if ((points[i - 1] + ab * t - here).squaredLength() < ON_LEG * ON_LEG)
                nearest = i;
        }
        bool legOver = nearest == points.size() || (points.back() - here).length() <= ON_LEG;
        // [runpast15] the leg (cut at the bot's sight) is over but its walk is not: the chase goes on while the bot
        // walks, so it walks on along its searched walk, else its far route, as far as the chases still to come run
        // (the leash's seconds left at its run speed) - when that stretch exists (the walk's stop lies past it) and
        // wakes no idle mob. Before, the run past ended at the leg's end and the bot fought there the very mobs it had
        // priced as never to fight; [deaths14] then walled every such mob instead (b355: 953 walls, 0 run pasts)
        if (legOver && bot->GetExactDist(points.back().x, points.back().y, points.back().z) <= ON_LEG)
        {
            float seconds = 0.0f;
            bool priced = true;
            uint32 chasing = 0;
            for (Unit* attacker : bot->getAttackers())
            {
                if (!attacker || !attacker->IsAlive())
                    continue;
                Creature* mob = attacker->ToCreature();
                if (!mob || !set.count(mob->GetGUID().GetRawValue()))
                {
                    priced = false;
                    break;
                }
                seconds = std::max(seconds, RunPastChase(bot, mob).seconds);
                ++chasing;
            }
            if (priced && chasing && seconds > 0.0f)
            {
                float const yards = seconds * bot->GetSpeed(MOVE_RUN);
                std::vector<G3D::Vector3> searched;
                {
                    std::lock_guard<std::mutex> guard(lock);
                    searched = states[bot->GetGUID().GetCounter()].legSearched;
                }
                std::vector<G3D::Vector3> on = WalkOn(here, searched, yards);
                char const* along = "its searched walk";
                if (on.empty())
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                    {
                        on = WalkOn(here, ai->rpgInfo.farRoute, yards);
                        along = "its far route";
                    }
                if (!on.empty() && StretchClear(bot, on, set))
                {
                    LOG_INFO("playerbots", "[runpast15] {} L{} walks on past its leg's end: {:.0f} yd along {}, {} "
                             "chasing for {:.0f} s more",
                             bot->GetName(), bot->GetLevel(), yards, along, chasing, seconds);
                    std::lock_guard<std::mutex> guard(lock);
                    SoloBotState& state = states[bot->GetGUID().GetCounter()];
                    state.legPoints = on;
                    state.legSearched.clear();
                    points = on;
                    nearest = 0;
                    legOver = false;
                }
            }
        }
        if (legOver)
        {
            std::lock_guard<std::mutex> guard(lock);
            SoloBotState& state = states[bot->GetGUID().GetCounter()];
            state.legRunPast.clear();
            state.legStatic.clear();
            state.legPoints.clear();
            state.legNamed.clear();
            state.runPastPriced = 0.0f;  // [runpast87]
            state.runPastPricedHigh = 0.0f;  // [runpast88]
            state.runPastChasers.clear();  // [surv20]
            return false;
        }
        // every attacker one the leg priced; their chases still to come survived
        std::unordered_set<uint64> counted;  // [surv20] the chase's own mobs: its chasers and the pack they bring
        std::vector<Creature*> chasers;
        std::vector<Creature*> unpriced;
        RunPast left;
        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker || !attacker->IsAlive())
                continue;
            Creature* mob = attacker->ToCreature();
            if (!mob)
                return false;  // a player's: the fight as before
            // [death439] a mob the leg did not price (a wanderer that walked into the walk, a reach beyond the leg's
            // sight) joins the chase priced here, with the pack its fight brings below: the run past goes on when
            // the bot lives through it all, else the fight / the kit's escape as before. Before, any such mob ended
            // the run past where it stood: a walk priced as a run past turned into a fight nobody judged
            if (!set.count(mob->GetGUID().GetRawValue()))
                unpriced.push_back(mob);
            left.Add(RunPastChase(bot, mob));
            chasers.push_back(mob);
        }
        // [runpast81] user rule 2026-11-10: the chase is priced on the pack the judge counts - each chaser and every
        // mob its fight brings (PackAt: the linked pack, assistance calls with the wanderers' areas and patrol beats,
        // flee-for-help helpers, script calls), each mob's chase until it leashes, summed against its health now; a
        // pack chase it does not live through is no run-past (the fight from its spot, or the kit's escape). b406
        // Rollothan L11 kept walking past a Defias Smuggler priced alone ("chase priced 157, hp 496") that its judge had
        // counted as 5 (4 above); the patrol and its fleeing friends came, dead
        // [surv23] what the commit line could not say: it printed `chasers.size()` alone - the bot's attackers at
        // that tick - so "1 chaser(s)" read the same whether the chase's run met no other mob or whether no run was
        // looked at at all. b488 gave eleven commits, every one of them "1 chaser(s)", with the pack and the stretch
        // invisible. The pack the chase was priced on, the yards of run it was counted over and the leg points
        // sampled on them go on the same line now; no decision reads them
        uint32 joiners = 0;
        float stretch = 0.0f;
        size_t sampled = 0;
        if (!chasers.empty() && left.damage != std::numeric_limits<float>::max())
        {
            for (Creature* mob : chasers)  // [surv20] `counted` is declared above: the commit below keeps it
                counted.insert(mob->GetGUID().GetRawValue());
            G3D::Vector3 const fight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            // [surv22] the chase's mobs are the ones its RUN counts, not the ones standing where the bot is at this
            // tick. The route search counts every mob whose reach the stretch ahead crosses (RouteCosts' chaseCovers
            // walks it at the route's own sample spacing, "its own aggro on the running bot"); the executor counted
            // the spot alone, so the same leg was priced twice over, and the cheaper judge is the one that keeps the
            // bot walking. b486 Ellosera L25 past a Stonetalon Grunt L40: the search had both Grunts ("run-past
            // priced 623 hp vs hp 1182 (14 s of chase): Stonetalon Grunt L40 pack 1 +1 assisting on the chase,
            // Stonetalon Grunt L40 pack 1", and its death line "Stonetalon Grunt priced, Stonetalon Grunt priced"),
            // the commit had one ("1 chaser(s) ... priced 145 dmg"); the second stood 7 yd off the walk AHEAD with
            // an aggro reach of 33 (L40 against L25) and hit it: "hit by 2 this fight", took 879, dead.
            // The leg's own points are the walk's samples, so the stretch is the leg from here on as far as this
            // chase's seconds carry the bot at its run speed - the chase model's own seconds, no number of mine
            std::vector<G3D::Vector3> stands{fight};
            {
                float const runs = std::max(left.seconds, 0.0f) * bot->GetSpeed(MOVE_RUN);
                for (size_t i = nearest; i + 1 < points.size() && stretch <= runs; ++i)
                {
                    stretch += (points[i + 1] - points[i]).length();
                    stands.push_back(points[i + 1]);
                }
                sampled = stands.size();  // [surv23] the stretch and its spots go on the commit line
            }
            for (Creature* mob : std::vector<Creature*>(chasers))
                for (Creature* join : PackAt(bot, mob, fight, stands))
                {
                    if (!join || !join->IsAlive() || !counted.insert(join->GetGUID().GetRawValue()).second)
                        continue;
                    RunPast const chase = RunPastChase(bot, join);
                    if (chase.damage == std::numeric_limits<float>::max())
                    {
                        left.damage = chase.damage;
                        break;
                    }
                    left.Add(chase);
                    ++joiners;
                }
            if (joiners && !SurvivesRunPast(bot, left.High(), left.seconds))
            {
                thread_local std::unordered_map<uint64, uint64> refusedLogged;  // a log cadence only
                uint64 const key = chasers.front()->GetGUID().GetRawValue() * 31 + joiners;
                if (uint64& last = refusedLogged[bot->GetGUID().GetRawValue()]; last != key)
                {
                    last = key;
                    LOG_INFO("playerbots", "[runpast81] {} L{} no run-past: {} chaser(s) and {} mob(s) their fight brings, "
                         "{:.0f} dmg over {:.0f} s against hp {:.0f}",
                         bot->GetName(), bot->GetLevel(), chasers.size(), joiners, left.damage, left.seconds,
                         float(bot->GetHealth()));
                }
                return false;
            }
        }
        if (chasers.empty() || !SurvivesRunPast(bot, left.High(), left.seconds))
            return false;  // nothing chases, or the kit's turn (it does not live through the chase now)
        // one attacker the judge beats here for less damage than its chase: the fight is the cheaper way
        if (chasers.size() == 1)
        {
            Verdict const verdict = Judge(bot, chasers.front());
            if (verdict.kind == Verdict::Kind::Win && verdict.damage < left.damage)
                return false;
        }
        rest.push_back(here);
        rest.insert(rest.end(), points.begin() + std::min(nearest + 1, points.size() - 1), points.end());
        // [runpast89] the same number the gate above was asked, from the same judge
        float const hp = RunPastPool(bot, left.High(), left.seconds);
        for (Creature* mob : unpriced)  // [death439] its chase priced here: on the leg's set from now on
            set[mob->GetGUID().GetRawValue()] = RunPastChase(bot, mob).damage;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        for (Creature* mob : unpriced)
            state.legRunPast[mob->GetGUID().GetRawValue()] = set[mob->GetGUID().GetRawValue()];
        if (state.runPastPriced <= 0.0f)  // [runpast87] the chase's price at its start
        {
            state.runPastPriced = left.damage;
            state.runPastPricedHigh = left.High();  // [runpast88] the number the gate above was actually asked
            state.runPastSec = left.seconds;
            state.runPastHp = hp;
            state.runPastTakenAt = state.taken;
            // [surv20] the mobs this price is of: whatever hits the bot outside them is another fight, and the
            // sample of the chase model says so instead of pricing that fight at this chase's damage
            state.runPastChasers = counted;
            for (Creature* mob : chasers)
                state.runPastChasers.insert(mob->GetGUID().GetRawValue());
            // [surv10] the committed run-past, with the margin it was committed on and the widest level gap in the
            // chase. [runpast87] prices a chase against what it cost only at a fight's close, so a chase that
            // leashed cleanly is never sampled: 27 samples over b482-b484, one of them a death, far too few to say
            // the chase model is mis-priced. The committed decisions are the denominator that sample was missing.
            // b484 Kylantino L22 committed past 5 Boulderfist mobs on "310 dmg vs hp 403" - a 93 hp margin - and
            // took 403. One line per chase, at the same gate [runpast87] opens on; a log, no decision reads it.
            int32 gap = std::numeric_limits<int32>::min();
            for (Creature* mob : chasers)
                gap = std::max(gap, int32(mob->GetLevel()) - int32(bot->GetLevel()));
            // [surv23] ... and the pack behind that number: the mobs the chase's own run meets ([surv22]), the yards
            // of run they were counted over and the leg points sampled on them. "1 chaser(s), 0 met over 0 yd (1
            // spot)" is a run nobody looked at; "1 chaser(s), 0 met over 134 yd (34 spots)" is a clear run
            LOG_INFO("playerbots",
                     "[surv10] {} L{} commits a run-past: {} chaser(s), widest level gap {:+d}, priced {:.0f} dmg "
                     "({:.0f} at the high end) over {:.0f} s against hp {:.0f} - a margin of {:.0f}; [surv23] {} "
                     "mob(s) its run meets over {:.0f} yd ({} spot(s) of the leg sampled)",
                     bot->GetName(), bot->GetLevel(), chasers.size(), gap, left.damage, left.High(), left.seconds,
                     hp, hp - left.High(), joiners, stretch, sampled);
        }
        for (Creature* mob : chasers)
            if (state.legNamed.insert(mob->GetGUID().GetRawValue()).second)
                line += Acore::StringFormat(
                    "{}{} keeps walking past {} L{} (chase priced {:.0f}{}, hp {:.0f})", line.empty() ? "" : "; ",
                    bot->GetName(), mob->GetName(), mob->GetLevel(), set[mob->GetGUID().GetRawValue()],
                    std::find(unpriced.begin(), unpriced.end(), mob) != unpriced.end() ? " [death439] on the run" : "",
                    hp);
        return true;
    }

    namespace
    {
        // [deaths141] the pack the judge sees when a walk meets `mob`'s reach at `meet` (PullModel::Grow from that
        // spot), priced at full: lost or not, and its size. Cached per walker and level, spawn, the side the walk
        // meets it from (an eighth round its reach: a look's resolution) and the idle mobs that can take part (a
        // death or a respawn there is another pack); its cost measured
        std::pair<bool, uint32> GrownLoses(Player* bot, Creature* mob, PullModel& model,
                                           std::vector<Creature*> const& around, G3D::Vector4 const& reach,
                                           G3D::Vector3 const& meet, DpsEstimate::Result const& own, float petHealth)
        {
            float const side = std::atan2(meet.y - reach.y, meet.x - reach.x);
            uint64 const octant = uint64(int32(std::floor((side + float(M_PI)) / (float(M_PI) / 4.0f))) & 7);
            uint64 campKey = 0;  // the mobs that can take part
            for (Creature* c : around)
            {
                G3D::Vector4 const cd = AggroDisc(c, bot);
                if (std::hypot(cd.x - reach.x, cd.y - reach.y) <= reach.w + cd.w + model.assist)
                    campKey += uint64(c->GetSpawnId()) * 2654435761ull;  // order-free
            }
            uint64 const key = ((uint64(bot->GetGUID().GetCounter()) * 1000003ull + bot->GetLevel()) * 1000003ull +
                                mob->GetSpawnId()) * 1000003ull + octant * 4099ull + campKey;
            thread_local std::unordered_map<uint64, std::pair<bool, uint32>> grownCache;
            static std::atomic<uint64> growCalls{0}, growCached{0}, growUs{0};
            if (auto const found = grownCache.find(key); found != grownCache.end())
            {
                ++growCached;
                return found->second;
            }
            auto const start = std::chrono::steady_clock::now();
            model.Grow(&meet, nullptr);
            std::vector<RouteFoe> grown;
            for (PullModel::Member const& m : model.pack)
                grown.push_back({m.mob, MobDps(m.mob, bot), float(m.mob->GetHealth())});
            std::sort(grown.begin() + 1, grown.end(), [](RouteFoe const& x, RouteFoe const& y)
                      { return x.dps / std::max(x.hp, 1.0f) > y.dps / std::max(y.hp, 1.0f); });
            // [lag353] two above its level at once are priced by the fight's numbers too
            std::pair<bool, uint32> const verdict{LosesAtFull(bot, own, grown, petHealth), uint32(grown.size())};
            if (grownCache.size() > 65536)
                grownCache.clear();
            grownCache[key] = verdict;
            growUs += uint64(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                    .count());
            if (uint64 const n = ++growCalls; n % 200 == 0)
                LOG_INFO("playerbots", "[deaths141] route packs: {} grown, {} answered from the cache, {:.1f} us each "
                         "grown",
                         n, growCached.load(), double(growUs.load()) / double(n));
            return verdict;
        }

        // [death431] the mobs the last RouteCosts on this thread priced by their linked pack alone (the straight leg
        // missed their reach, the real walk may enter it) with what their Grow needs: read by GrowOnWalk once the walk
        // is found, in the same search (written at the call's end: a judge's search inside it does not clobber it)
        struct UngrownMob
        {
            ObjectGuid mob;
            G3D::Vector4 reach;
        };
        struct UngrownCosts
        {
            uint64 walker = 0;
            G3D::Vector3 from, to;
            std::vector<UngrownMob> mobs;
            std::vector<ObjectGuid> around;
            DpsEstimate::Result own;
            float petHealth = 0.0f;
        };
        thread_local UngrownCosts ungrownCosts;
    }

    void RouteCosts(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& to, std::vector<G3D::Vector4>& refuse,
                    std::vector<Leader::Travel::RoutePenalty>& penalties)
    {
        SlowScope slow("route costs");  // [perf26]
        PMON_SCOPE(costScope, PERF_MON_VALUE, "solo route costs");  // [guild15]
        Player* bot = walker ? walker->ToPlayer() : nullptr;
        ungrownCosts.mobs.clear();  // [death431] this call's, written at its end
        ungrownCosts.around.clear();
        std::vector<UngrownMob> ungrown;
        if (!bot || bot->IsInCombat() || !bot->IsAlive())
            return;
        // [deaths3] a co-op party's walk keeps the con rule too (b341 Ryarii L1 and Salleas L1: their walk to the boar
        // spawns went through Nightsabers, Webwood Lurkers and Shadow Sprites L5-6, five fights off the objective, a
        // Lurker L6 killed Ryarii): orange / red, elite and danger-zone reaches are refused; the pack it loses alone is
        // the party's judgement (NewRpgCoop::JudgePull), not refused here
        // [deaths146] ... and a bot that walks alone off its New RPG plans (a dungeon five's member on its way to the
        // instance, its group inside): b438b Aevmae L19 (dm-n five 2) rose at the Westfall graveyard 32 yd above the
        // Deadmines entrance, its "move to instance portal" walk went round by Moonbrook through the Defias
        // Pathstalkers at Demont's Place unpriced - 8 deaths there in a row under Resurrection Sickness (hp 192)
        bool const alone = Applies(bot) || WalksAlone(bot);
        // [group22] coordinator 2026-11-09: any bot leading an open-world group (co-op, help, user pack, a restored
        // group) prices its walk for the group as a co-op leader does - the reaches of mobs it never fights refused,
        // the packs the party judge's (JudgePull); a human leader decides himself (no bot walk). b370: the user-pack
        // leaders Kianey and Manue walked their groups unpriced into Stendel's Pond (Riverpaw L15-17) and the Westfall
        // Riverpaw camps - 3 + 2 + 1 deaths there, "a dense camp ... none judged"
        if (!alone && !((NewRpgCoop::InParty(bot->GetGUID().GetCounter()) || NewRpgCoop::JudgesPulls(bot)) &&
                        !bot->GetMap()->Instanceable() && GET_PLAYERBOT_AI(bot) &&
                        GET_PLAYERBOT_AI(bot)->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)))
            return;
        if (!alone)
        {
            // [group22] one line per bot: its walk priced for its group (a log line, no decision)
            static std::mutex groupLogLock;
            static std::unordered_set<uint32> groupLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(groupLogLock);
                fresh = groupLogged.insert(bot->GetGUID().GetCounter()).second;
            }
            if (fresh)
                LOG_INFO("playerbots", "[group22] {} L{} prices its walks for its group ({})", bot->GetName(),
                         bot->GetLevel(),
                         NewRpgCoop::InParty(bot->GetGUID().GetCounter()) ? "co-op / help party" : "open-world group");
        }

        // the walk's first leg as far as the bot sees: the rest is planned again from there. [deaths] The leg ends up
        // to SightDistance from the bot, so a mob whose aggro reach (Creature::GetAggroRange, at most the core's
        // MAX_AGGRO_RADIUS) covers its end stands up to that much farther: those are seen too
        // [deaths3] ... and a wanderer's reach is its home's with its wander area (b338 Keenonk L11: a Black Slime L20
        // "unseen" by the search, its home in reach of the walk): the search looks that much farther, at most as far
        // as the map shows the bot units (Map::GetVisibilityRange)
        float const wander = std::min(MaxWanderDistance(bot->GetMapId()), bot->GetMap()->GetVisibilityRange());
        float const range = sPlayerbotAIConfig.SightDistance + MAX_AGGRO_RADIUS + wander;
        std::unordered_map<uint64, bool> seen;  // [deaths] mob -> refused (DeathContext)
        std::unordered_set<uint64> othersFight;  // [surv21] in range, left out for being in a fight not the bot's
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);
        // [dpszero1] the walk's own damage as a state, with no floor. It prices the fight a mob on the way COSTS
        // (yards of walk below), and where there is no estimate or the bot deals nothing that cost has no bound -
        // which the search already has a value for: any detour is the better walk
        KillRate::Own const rate = BotDps(bot);
        float const speed = bot->GetSpeed(MOVE_RUN);
        ObjectGuid const target = bot->GetTarget();
        uint8 const level = bot->GetLevel();
        std::vector<Creature*> around;  // [pullplan] the idle mobs round the walk (built at the first priced mob)
        bool aroundBuilt = false;
        DpsEstimate::Result own;  // [pull9] its damage and the pet's hold, read with `around`
        float petHealth = 0.0f;
        std::vector<RouteWall> walls;  // [pull9] the mobs refused, for the MAZE / refused-walk lines
        // [regress10] the run-past chases this leg already takes (orange / red / elite mobs it walks past): their sum
        RunPast runPast;
        std::unordered_set<uint64> chasing;  // the mobs counted in it
        // [runpast11] the leg's chases are summed nearest first: the mobs the walk meets first are the ones it runs
        // past first (Cell::VisitObjects gives them in grid order: a far orange mob's chase could use up the sum and
        // wall the camp next to the bot)
        units.sort([bot](Unit const* a, Unit const* b) { return bot->GetExactDist2d(a) < bot->GetExactDist2d(b); });
        auto const buildAround = [&]()
        {
            if (aroundBuilt)
                return;
            aroundBuilt = true;
            for (Unit* other : units)
                if (Creature* c = other->ToCreature())
                    if (c->IsAlive() && !c->IsInCombat() && c->IsHostileTo(bot) && c->HasReactState(REACT_AGGRESSIVE))
                        around.push_back(c);
            own = OwnDamage(bot, nullptr);
            if (Pet* pet = bot->GetPet())
                if (pet->IsAlive() && PetTaunts(pet))
                    petHealth = float(pet->GetHealth());
        };
        // [runpast11] the run past a pack it does not fight (cycle 10, user 2026-11-08 "you can just run past"): its
        // linked pack's leash chases added to the ones the leg takes already, survived from its health now with its
        // heal kit -> no wall. Before, only an orange / red or elite mob was priced so: a pack walled by the fight
        // judge ("pack above its level", "loses at full") never was (b349: 1337 of the maze's wall tails, no run past
        // ever priced). `price` / `hp`: the chase and the bot's health with its kit (logged on the wall)
        std::vector<std::string> ranPastNames;
        std::unordered_map<uint64, float> chaseOf;  // [runpast11] each mob run past: its chase (the executor's set)
        // [runpast60] a patrol's forecast beat (PullPlanning::PatrolForecast), once per mob and search
        std::unordered_map<Creature*, std::vector<G3D::Vector3>> beats;
        auto const beatOf = [&](Creature* c) -> std::vector<G3D::Vector3> const&
        {
            auto found = beats.find(c);
            if (found == beats.end())
            {
                std::vector<G3D::Vector3> track;
                float sampleSec = 0.0f;
                if (c->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE ||
                    !PullPlanning::PatrolForecast(c, track, sampleSec))
                    track.clear();
                found = beats.emplace(c, std::move(track)).first;
            }
            return found->second;
        };
        // [runpast60] the mob's reach covers `at` from anywhere it roams: its aggro disc (a wanderer's round its home
        // with its wander area) where it stands, or anywhere on its patrol beat (PullModel::Reaches' reach data)
        auto const reachCovers = [&](Creature* c, G3D::Vector3 const& at, float extra)
        {
            G3D::Vector4 disc = AggroDisc(c, bot);
            disc.w += extra;
            if (InDisc(disc, at))
                return true;
            for (G3D::Vector3 const& step : beatOf(c))
                if (InDisc(G3D::Vector4(step.x, step.y, step.z, disc.w), at))
                    return true;
            return false;
        };
        // [lag356] the health a run past may cost: its health now with the heal kit it uses on the move over the
        // chase's seconds (SurvivesRunPast's test) - one kit look for the whole search, not one per mob
        // [runpast89] ... and the pool now depends on the chase's damage too (a heal only puts back what the chase
        // took off), so the number is no longer cacheable by the seconds alone - what IS looked up once is the heal
        // piece, which is the part that locks and walks the bags. The arithmetic after it is free.
        Chase::Heal runPastHeal;
        SurvivalKit::RunPastHealPiece(bot, runPastHeal);
        auto const budget = [&](float damage, float seconds)
        { return RunPastPool(bot, damage, seconds, runPastHeal, false); };
        // [surv10] coordinator 2026-11-14: the same budget at full health and mana. The fight judge already answers
        // "rest first" where a fight is lost now and won at full (Verdict::Kind::RestFirst, "wins only at full", and
        // RestBeforeWalk acts on it); the chase had no such answer, so a wall the bot's own rest would clear stayed a
        // wall for ever. b486 Kylantino L22 stood in Boulder'gor with all four of its exits priced 410, 412, 419 and
        // 429 against hp 403 - short by 7, 9, 16 and 26 - while its full health is 493, +90. It logged 106 MAZE lines
        // and never rested. Idunk L22 was short by 3 of 483, Jurdogs L19 by 39 of 582: 6 of the 11 distinct walls of
        // that window were within 40 hp of passing. The same comparison the judge makes, no number of mine.
        auto const budgetFull = [&](float damage, float seconds)
        { return RunPastPool(bot, damage, seconds, runPastHeal, true); };
        // [surv10] a chase this walk cannot take now and could take at full: the walk rests first instead of walling
        std::string restForChase;
        auto const tryRunPast = [&](Creature* mob, PullModel const& model, float& price, float& hp)
        {
            RunPast with = runPast;
            std::vector<std::pair<uint64, float>> added;
            // [deaths12] the chasers call for assistance on the way (Creature::Update's periodic CallAssistance every
            // CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD from where each chases, AnyAssistCreatureInRangeCheck within
            // CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS): they trail the bot along its leg, so an idle mob within that
            // radius of the stretch the chase covers (from the leg's point nearest the chaser on, the chase's seconds
            // at the bot's run speed) that can assist a chaser joins the chase. Before, only the linked pack was
            // priced: b350 Eraelor L9, Enyssia L6, Tuihgrelfim L8, Lyleria L9 ran past a camp's first pack and died
            // to the camp ("no judged pull (attacked)", 3-4 attackers)
            std::vector<Creature*> chasers;
            for (PullModel::Member const& m : model.pack)
                chasers.push_back(m.mob);
            float const assistRadius = model.assist;
            G3D::Vector3 const leg = to - from;
            float const legLen = std::max(std::hypot(leg.x, leg.y), 0.01f);
            auto const chaseCovers = [&](Creature* chaser, float seconds, Creature* c, bool calls)
            {
                float const along0 = std::clamp(((chaser->GetPositionX() - from.x) * leg.x +
                                                 (chaser->GetPositionY() - from.y) * leg.y) /
                                                    legLen,
                                                0.0f, legLen);
                float const along1 = std::min(legLen, along0 + seconds * speed);
                float const along = std::clamp(((c->GetPositionX() - from.x) * leg.x +
                                                (c->GetPositionY() - from.y) * leg.y) /
                                                   legLen,
                                               along0, along1);
                float const px = from.x + leg.x * (along / legLen), py = from.y + leg.y * (along / legLen);
                if (calls && c->CanAssistTo(chaser, bot) &&
                    std::hypot(c->GetPositionX() - px, c->GetPositionY() - py) <= assistRadius)
                    return true;
                // [runpast60] ... or a roamer that comes within the assistance radius of the chase's stretch from
                // anywhere on its round (a wanderer's area, a patrol's beat), or whose own aggro reach the stretch
                // crosses (the bot runs into it with the chase on): sampled along the stretch at the route's step
                constexpr float STRETCH_STEP = 4.0f;  // the route search's sample spacing, a look's cost
                bool const roams = c->GetDefaultMovementType() == RANDOM_MOTION_TYPE ||
                                   c->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE;
                for (float a = along0; a <= along1; a += STRETCH_STEP)
                {
                    G3D::Vector3 const p(from.x + leg.x * (a / legLen), from.y + leg.y * (a / legLen),
                                         from.z + leg.z * (a / legLen));
                    if (reachCovers(c, p, 0.0f))
                        return true;  // its own aggro on the running bot
                    if (calls && roams && c->CanAssistTo(chaser, bot))
                    {
                        G3D::Vector4 home = AggroDisc(c, bot);  // the wanderer's area centre and its wander
                        float const wanderOnly = home.w - c->GetAggroRange(bot) - bot->GetCombatReach();
                        if (std::hypot(home.x - p.x, home.y - p.y) <= assistRadius + std::max(0.0f, wanderOnly))
                            return true;
                        for (G3D::Vector3 const& step : beatOf(c))
                            if (std::hypot(step.x - p.x, step.y - p.y) <= assistRadius)
                                return true;
                    }
                }
                return false;
            };
            // [death405] ... and a stop inside the reach of any idle mob (not only a chaser's, [death393]): the bot
            // stands there with the chase on it and that mob joins - a fight nobody judged. b453 Sycina L21 "RUN_PAST
            // keeps walking past Thistlefur Shaman L24 (chase priced 22)" stopped at its objective in another
            // Thistlefur Shaman's wander area (reach 37 yd): UNJUDGED, 3 Shamans, dead
            G3D::Vector3 const anyStop = walkStopSet ? walkStop : to;
            for (Creature* c : around)
                if (c != mob && reachCovers(c, anyStop, 0.0f))
                {
                    static std::mutex anyStopLogLock;  // a log cadence per bot and mob kind, no decision reads it
                    static std::unordered_set<uint64> anyStopLogged;
                    bool fresh = false;
                    {
                        std::lock_guard<std::mutex> guard(anyStopLogLock);
                        if (anyStopLogged.size() > 65536)
                            anyStopLogged.clear();
                        fresh = anyStopLogged.insert(uint64(c->GetEntry()) * 1000003ull +
                                                     bot->GetGUID().GetCounter())
                                    .second;
                    }
                    if (fresh)
                        LOG_INFO("playerbots", "[death405] {} L{} no run past {} L{}: its walk stops at "
                                 "({:.0f},{:.0f}) inside the reach of {} L{}",
                                 bot->GetName(), bot->GetLevel(), mob->GetName(), mob->GetLevel(), anyStop.x, anyStop.y,
                                 c->GetName(), c->GetLevel());
                    price = std::numeric_limits<float>::max();
                    hp = budget(with.High(), with.seconds);
                    return false;
                }
            for (size_t i = 0; i < chasers.size(); ++i)
            {
                Creature* chaser = chasers[i];
                uint64 const key = chaser->GetGUID().GetRawValue();
                if (chasing.count(key))
                    continue;
                RunPast const chase = RunPastChase(bot, chaser);
                // [deaths14] a run past is a walk on while the chase lasts: the leash lets the chaser go only while the
                // bot keeps walking. A walk that ends inside the chase (its stop - the objective, the town stop - lies
                // nearer than the chase's seconds at the bot's run speed from where the chaser meets the walk) stops
                // with the chaser on it: a fight it never judged. b354 Thelengyl L10 "keeps walking past" a Rabid
                // Thistle Bear L13, stopped at its objective 26 yd from the bear's home, fought it there and died
                // [death393] the walk's stop inside its reach (the objective, the camp it walks to): the bot stops
                // and stands there with the chaser on it - a fight at the stop, the judge's, no run past. b451 Laaral
                // L23 (twice at Render's Camp): "refuse Kazon L27: loses even at full: 6 mobs", then "RUN_PAST keeps
                // walking past Kazon L27 (chase priced 205, hp 2418)" to its DO_QUEST spot 4 yd from Kazon's home
                if (G3D::Vector3 const stop = walkStopSet ? walkStop : to; reachCovers(chaser, stop, 0.0f))
                {
                    static std::mutex stopReachLogLock;  // a log cadence per bot and mob kind, no decision reads it
                    static std::unordered_set<uint64> stopReachLogged;
                    bool fresh = false;
                    {
                        std::lock_guard<std::mutex> guard(stopReachLogLock);
                        if (stopReachLogged.size() > 65536)
                            stopReachLogged.clear();
                        fresh = stopReachLogged.insert(uint64(chaser->GetEntry()) * 1000003ull +
                                                       bot->GetGUID().GetCounter())
                                    .second;
                    }
                    if (fresh)
                        LOG_INFO("playerbots", "[death393] {} L{} no run past {} L{}: its walk stops at "
                                 "({:.0f},{:.0f}) inside its reach", bot->GetName(), bot->GetLevel(), chaser->GetName(),
                                 chaser->GetLevel(), stop.x, stop.y);
                    price = std::numeric_limits<float>::max();
                    hp = budget(with.High(), with.seconds);
                    return false;
                }
                if (chase.damage != std::numeric_limits<float>::max())
                {
                    float const meets = std::clamp(((chaser->GetPositionX() - from.x) * leg.x +
                                                    (chaser->GetPositionY() - from.y) * leg.y) /
                                                       legLen,
                                                   0.0f, legLen);
                    // [runpast15] ... the walk's own stop: a search for a stage of a longer walk (WalkStopScope) goes
                    // on from its end to that stop (straight: no more than the walk) - the executor walks on past the
                    // leg's end while the chase lasts (RunPastStep)
                    float walkOn = legLen - meets;
                    // (only along a far route the executor can walk on: a complete one found to that stop)
                    if (walkStopSet)
                        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
                            ai && ai->rpgInfo.farRoute.size() >= 2 &&
                            (ai->rpgInfo.farRoute.back() - walkStop).length() <= 8.0f)  // the farRoute tolerance
                            walkOn += std::hypot(walkStop.x - to.x, walkStop.y - to.y);
                    // [lag354] user 2026-11-11 (nobody is walled in the open world: a mob chases until its leash lets
                    // it go): a chase that outlasts the walk is no wall - its whole damage is in the price already, and
                    // from the walk's end the bot runs on as from a fight going wrong (SurvivalKit's flee, priced by
                    // the same leash and daze) until it drops off; then the walk is searched again from there
                    if (chase.seconds * speed > walkOn)
                    {
                        static std::mutex stopLogLock;
                        static std::unordered_set<uint64> stopLogged;
                        bool fresh = false;
                        {
                            std::lock_guard<std::mutex> guard(stopLogLock);
                            if (stopLogged.size() > 65536)
                                stopLogged.clear();
                            // [lag356] once per bot and mob kind (b447: 934 lines in 13 min per spawn)
                            fresh = stopLogged.insert(uint64(chaser->GetEntry()) * 1000003ull +
                                                      bot->GetGUID().GetCounter()).second;
                        }
                        if (fresh)
                            LOG_INFO("playerbots", "[lag354] {} L{} run past {} L{}: the walk ends {:.0f} yd past "
                                     "where it meets the walk, the chase runs {:.0f} s ({:.0f} yd) - it runs on from "
                                     "the walk's end until the leash lets the chaser go",
                                     bot->GetName(), bot->GetLevel(), chaser->GetName(), chaser->GetLevel(), walkOn,
                                     chase.seconds, chase.seconds * speed);
                        // [death406] the run on from the walk's end is a run only where it is clear: an idle mob
                        // whose reach covers it (sampled on along the leg, the chase's yards left) stops the run -
                        // the bot stands and fights the chase there. b453 Geron L19 off Menethil: "RUN_PAST ...
                        // (chase priced 227 / 259, hp 1144)", then "no way out clear of other mobs" by a Mottled
                        // Raptor, fought 54 s, took 1722
                        G3D::Vector3 const end = walkStopSet ? walkStop : to;
                        float const left = chase.seconds * speed - walkOn;
                        G3D::Vector3 const ahead =
                            G3D::Vector3(leg.x, leg.y, 0.0f) / std::max(std::hypot(leg.x, leg.y), 0.01f);
                        constexpr float RUN_STEP = 4.0f;  // the route search's sample spacing
                        for (float d = RUN_STEP; d <= left; d += RUN_STEP)
                        {
                            G3D::Vector3 const p = end + ahead * d;
                            for (Creature* c : around)
                                if (std::find(chasers.begin(), chasers.end(), c) == chasers.end() &&
                                    reachCovers(c, p, 0.0f))
                                {
                                    static std::mutex runOnLogLock;  // a log cadence, no decision reads it
                                    static std::unordered_set<uint64> runOnLogged;
                                    bool runOnFresh = false;
                                    {
                                        std::lock_guard<std::mutex> guard(runOnLogLock);
                                        if (runOnLogged.size() > 65536)
                                            runOnLogged.clear();
                                        runOnFresh = runOnLogged.insert(uint64(c->GetEntry()) * 1000003ull +
                                                                        bot->GetGUID().GetCounter())
                                                         .second;
                                    }
                                    if (runOnFresh)
                                        LOG_INFO("playerbots", "[death406] {} L{} no run past {} L{}: its run on "
                                                 "from the walk's end meets the reach of {} L{} {:.0f} yd on",
                                                 bot->GetName(), bot->GetLevel(), chaser->GetName(),
                                                 chaser->GetLevel(), c->GetName(), c->GetLevel(), d);
                                    price = std::numeric_limits<float>::max();
                                    hp = budget(with.High(), with.seconds);
                                    return false;
                                }
                        }
                    }
                }
                with.Add(chase);
                added.push_back({key, chase.damage});
                // [lag356] the sum is past its health already: no more chasers to grow (each one's assistance look
                // samples the whole stretch round every idle mob - b447: route costs 71-76 % of the searches' time)
                if (with.High() >= budget(with.High(), with.seconds))
                {
                    price = with.High();
                    hp = budget(with.High(), with.seconds);
                    // [surv10] the same answer where the sum of the chasers passes its health only at full
                    if (restForChase.empty() && with.High() < budgetFull(with.High(), with.seconds))
                        restForChase = Acore::StringFormat(
                            "before running past {} L{}: the chase of its {} chaser(s) is priced {:.0f} against "
                            "{:.0f} now and {:.0f} at full health and mana - short by {:.0f}",
                            mob->GetName(), mob->GetLevel(), chasers.size(), with.High(), hp,
                            budgetFull(with.High(), with.seconds), with.High() - hp);
                    return false;
                }
                // [runpast61] the chase's assistance calls only when the chaser makes them; a mob whose aggro reach the
                // stretch crosses comes at the running bot by itself either way (its own aggro, no call)
                bool const calls =
                    assistRadius > 0.0f && !chaser->HasFlagsExtra(CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE);
                for (Creature* c : around)
                    if (c != chaser && std::find(chasers.begin(), chasers.end(), c) == chasers.end() &&
                        !chasing.count(c->GetGUID().GetRawValue()) &&
                        chaseCovers(chaser, chase.seconds, c, calls))
                        chasers.push_back(c);  // its own chase and calls are counted in turn
            }
            price = with.High();
            hp = budget(with.High(), with.seconds);
            if (with.High() >= hp)
            {
                // [surv10] short of the chase now, inside it at full: the answer is "rest first", not "never"
                if (restForChase.empty() && with.High() < budgetFull(with.High(), with.seconds))
                    restForChase = Acore::StringFormat(
                        "before running past {} L{}: its chase is priced {:.0f} against {:.0f} now and "
                        "{:.0f} at full health and mana - short by {:.0f}",
                        mob->GetName(), mob->GetLevel(), with.High(), hp, budgetFull(with.High(), with.seconds),
                        with.High() - hp);
                return false;
            }
            runPast = with;
            for (auto const& [key, damage] : added)
            {
                chasing.insert(key);
                chaseOf[key] = damage;
            }
            ranPastNames.push_back(Acore::StringFormat("{} L{} pack {}{}", mob->GetName(), mob->GetLevel(),
                                                       model.pack.size(),
                                                       chasers.size() > model.pack.size()
                                                           ? Acore::StringFormat(" +{} assisting on the chase",
                                                                                 chasers.size() - model.pack.size())
                                                           : std::string()));
            return true;
        };
        // [dpszero2] "it loses this pack even at full" over a mob's LINKED pack (PullModel::Links) in the judge's
        // own kill order - the SAME judge the walk already asks (LosesAtFull), in one place instead of two. The
        // zone rule reads this answer; it does not compute one of its own (rule 5).
        auto const losesLinkedPack = [&](PullModel const& model) -> bool
        {
            std::vector<RouteFoe> foes;
            for (PullModel::Member const& m : model.pack)
                foes.push_back({m.mob, MobDps(m.mob, bot), float(m.mob->GetHealth())});  // [dps1]
            if (foes.empty())
                return false;
            // the judge's kill order: the mob met first, then the most damage per health
            std::sort(foes.begin() + 1, foes.end(), [](RouteFoe const& x, RouteFoe const& y)
                      { return x.dps / std::max(x.hp, 1.0f) > y.dps / std::max(y.hp, 1.0f); });
            return LosesAtFull(bot, own, foes, petHealth);
        };
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsCivilian() || mob->IsTotem() ||
                mob->GetGUID() == target || !mob->IsHostileTo(bot) || !mob->HasReactState(REACT_AGGRESSIVE) ||
                mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) ||
                mob->GetCharmerOrOwnerGUID())
                continue;
            // [surv21] a mob in a fight already is left out as before - but noted, so the death line can tell it
            // from a mob the search never had in range (both read "unseen" before). Its reach is priced at nothing
            // either way, and the note says so: no decision reads it
            if (mob->IsInCombat())
            {
                othersFight.insert(mob->GetGUID().GetRawValue());
                continue;
            }
            float const aggro = mob->GetAggroRange(bot);
            if (aggro <= 0.0f)
                continue;
            Position centre = mob->GetPosition();
            float radius = aggro + bot->GetCombatReach();
            if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
            {
                centre = mob->GetHomePosition();
                radius += mob->GetWanderDistance();
            }
            // the leg ends within sight of the bot: a reach beyond it is priced when the bot gets there
            // [runpast60] ... a patrol's reach by the nearest step of its beat, not where it walks now: b397 Leprithus
            // L19 (74 yd off on its beat) was left out of the search Emdyl L14's sweep leg was walked by (run past two
            // Rotten Ghouls, priced 300 hp vs 520); he came along his beat and the three killed it
            float nearest = bot->GetExactDist2d(centre.GetPositionX(), centre.GetPositionY());
            for (G3D::Vector3 const& step : beatOf(mob))
                nearest = std::min(nearest, bot->GetExactDist2d(step.x, step.y));
            if (nearest > sPlayerbotAIConfig.SightDistance + radius)
                continue;
            // never walked through: orange / red con, an elite, a mob in a remembered danger zone (where it died, a
            // pack its pull judge refused). [pull9] A zone the bot stands in lies behind it, as a refused reach it
            // stands in does (Leader::Route): its mobs are judged one by one below (b345 Almored L5 in the Venture Co.
            // camp it had refused: every walk out walled by that zone's mobs, LOOP alerts until logout)
            XPColorChar const con = Acore::XP::GetColorCode(level, mob->GetLevel());
            RouteWall::Why why = IsElite(mob) ? RouteWall::Why::Elite
                                 : con <= XP_ORANGE ? RouteWall::Why::Con
                                                    : RouteWall::Why::None;
            // [regress10] ... a zone where it died; a refused pull's zone is refused as its disc (DangerDiscs below),
            // the mobs in it are judged one by one with their links as the pull judge does (b347: 8 of the 20 most
            // logged MAZE "no way" wall lists were single mobs of the bot's level walled for standing in a refused
            // camp's disc - Eraelor L8 by Timberling Tramplers L8 round Oakenscowl's camp, 71 MAZE lines, every walk
            // of its log refused; Pherua L6 by the Wrathscale camp, Enyssia L5 by a Gnarlpine Warrior L7)
            // [surv10] coordinator 2026-11-14: standing in a zone where the bot died is no longer a wall of its own.
            // A mob of the bot's own con was walled for where it stood, so the ground round a past death became
            // unreachable and the bot met MAZE / MOVE_FAR_LOOP instead of a route; the zone is now a cost on the
            // search (DeathZoneCosts below) and this mob is priced one at a time like any other on the way - a wall
            // only when the bot loses that single fight. [lag365] had already taken refused-pull zones out of the
            // walls for the same reason; this is where it died, the half that was left.
            uint32 wallPack = 1;
            // [regress10] user 2026-11-08 "you can just run past": an orange / red or elite mob is no wall by itself -
            // walked past, it and its linked pack chase the bot until the core's leash lets them go; the leg takes
            // that chase (added to the others it takes) while the bot lives through it from its health now with its
            // heal kit, else it stays a wall. Priced as a mob it must never fight: a complete way round is walked
            bool ranPast = false;
            float runPastDamage = 0.0f;
            float runPastHp = 0.0f;
            // [dpszero2] the fight judge's verdict on this mob, kept beside `why` and not inside it
            bool fightLost = false;
            // [surv10] the run past is no longer vetoed by the zone either: the veto was the other half of the block,
            // so an orange / red mob standing where the bot once died walled every walk past it with no way round
            // priced at all. The chase is priced here as it is anywhere else, and the zone's own cost is added to the
            // search below - a zone is never impassable at any value (coordinator 2026-11-14).
            bool const mayRunPast = alone;
            if ((why == RouteWall::Why::Elite || why == RouteWall::Why::Con) && mayRunPast)
            {
                buildAround();
                PullModel model(bot, mob, around);
                model.Links();
                // [dpszero2] user 2026-11-15: a bot does not enter the aggro zone of a mob WHOSE FIGHT IS LOST, and
                // running past is entering it. The fight's verdict therefore comes first, from the judge that is
                // already here - there is no level check and no second judge. The chase is priced only where the
                // fight is not a veto, so the judge costs nothing where the answer was already no.
                // Measured over the recorded hour: 19 588 run-pasts priced survivable (73 of 75 bots), 9 601 of them
                // against a mob 11+ levels above; Lyleria L24 priced a run past Horde Guard L40 8 358 times.
                // The verdict is read here ONLY to veto the run past: no pull is offered for an orange / red mob
                // ([wall17] is for the bot's own band and stays that way).
                KillRate::Zone zone;
                zone.judged = true;
                zone.fightLost = losesLinkedPack(model);
                fightLost = fightLost || zone.fightLost;
                zone.chaseSurvived = true;  // asked of the fight alone first
                if (KillRate::MayRunPast(zone))
                {
                    zone.chaseSurvived = tryRunPast(mob, model, runPastDamage, runPastHp);
                    if (KillRate::MayRunPast(zone))
                    {
                        ranPast = true;
                        why = RouteWall::Why::None;
                    }
                }
                wallPack = uint32(model.pack.size());
            }
            // [pullplan] user 2026-11-08: a mob on the way is cleared one at a time as a player clears a path (the
            // grind target's on-the-way pull from its pull spot, SoloSafety::Judge): it is priced by its kill time, a
            // wall only when the bot loses even that one fight - the mob with what comes wherever it is fought, its
            // formation and the engage calls (PullModel's linked pack, killed one by one, the most damage per
            // health first). Before, the call chained hop after hop from every assistant's home through the whole camp,
            // which the core never does (b343: 68 quests postponed for "every way there passes mobs it does not pull")
            // [pull9] ... judged as the judge judges it: "loses even at full" (full health, the pet's hold, heals and
            // mana; where it wins only at full the walk rests first, RestBeforeWalk), two above its level at once
            // never. Before, its health now was the test: after a fight the mobs round it were walls (b345: 66 maze
            // postpones)
            if (why == RouteWall::Why::None && alone && !ranPast)
            {
                buildAround();
                PullModel model(bot, mob, around);
                model.Links();
                // [deaths141] coordinator 2026-11-10: a mob whose reach the leg crosses is priced with the pack the
                // judge sees when the walk meets it (PullModel::Grow from that spot: the wanderers and patrols whose
                // reach covers it, the engage and periodic assistance calls, a caster's stand, a flee for help), not
                // its linked pack alone. b428: Ganna L17 walked "complete" into Stendel's Pond (the Riverpaws priced
                // as their linked mobs), the judge there saw 5-6 and refused, it stood inside the reaches until they
                // came; Febineysh L14 at The Loch, Relara / Salleas L14 at Bathran's Haunt the same. The leg's first
                // point in the reach is the fight spot (the mob aggroes there and comes to the walker)
                bool priced = false;
                {
                    G3D::Vector3 const ab = to - from;
                    float const ox = from.x - centre.GetPositionX();
                    float const oy = from.y - centre.GetPositionY();
                    float const qa = ab.x * ab.x + ab.y * ab.y;
                    float const qb = ox * ab.x + oy * ab.y;
                    float const qc = ox * ox + oy * oy - radius * radius;
                    float const disc = qb * qb - qa * qc;
                    float meetAt = -1.0f;  // the leg's share where it first stands in the reach
                    if (qc <= 0.0f)
                        meetAt = 0.0f;  // it starts inside
                    else if (qa > 0.0f && disc >= 0.0f)
                    {
                        float const s0 = (-qb - std::sqrt(disc)) / qa;
                        if (s0 >= 0.0f && s0 <= 1.0f)
                            meetAt = s0;
                    }
                    if (meetAt >= 0.0f)
                    {
                        G3D::Vector3 const meet = from + ab * meetAt;
                        auto const [loses, size] =
                            GrownLoses(bot, mob, model, around,
                                       G3D::Vector4(centre.GetPositionX(), centre.GetPositionY(),
                                                    centre.GetPositionZ(), radius),
                                       meet, own, petHealth);
                        if (loses)
                        {
                            why = RouteWall::Why::Loses;
                            fightLost = true;  // [dpszero2]
                        }
                        wallPack = size;
                        priced = true;
                    }
                }
                if (!priced)  // a mob off the leg: its linked pack, as before
                {
                    // [lag353] the fight's numbers decide, no fixed refusal of two above its level
                    // [dpszero2] through the one judge the zone rule reads as well (losesLinkedPack): this was the
                    // second copy of the same four lines, and an empty pack reached `foes.begin() + 1`
                    if (losesLinkedPack(model))
                    {
                        why = RouteWall::Why::Loses;
                        fightLost = true;  // [dpszero2]
                    }
                    wallPack = uint32(model.pack.size());
                    // [death431] the walk the search finds may enter its reach off the straight leg: grown there
                    // once the walk is known (GrowOnWalk)
                    if (why == RouteWall::Why::None)
                        ungrown.push_back({mob->GetGUID(), G3D::Vector4(centre.GetPositionX(), centre.GetPositionY(),
                                                                       centre.GetPositionZ(), radius)});
                }
                // [wall17] coordinator 2026-11-09: the linked pack priced where the mob stands is not the pull a
                // player makes - the pull judge draws it out from a pull spot (SoloSafety::Judge). A mob at the bot's
                // level band (not [guild12]'s orange / red) whose reach crosses the leg is judged so before it walls
                // the walk: a win (or a win at full, rested first) is a pull on the way, no wall. Only mobs on the
                // leg, nearest first (the units' order); the judge's own walks do not ask it again (wallJudge).
                // b357: about 500 of 703 walls were mobs at +2 levels or below
                if (why != RouteWall::Why::None && !wallJudge)
                {
                    G3D::Vector3 const a(from.x, from.y, 0.0f);
                    G3D::Vector3 const ab = G3D::Vector3(to.x, to.y, 0.0f) - a;
                    G3D::Vector3 const c(centre.GetPositionX(), centre.GetPositionY(), 0.0f);
                    float const t = std::clamp((c - a).dot(ab) / std::max(ab.squaredLength(), 0.01f), 0.0f, 1.0f);
                    if ((a + ab * t - c).squaredLength() <= radius * radius)
                    {
                        wallJudge = true;
                        Verdict const pull = Judge(bot, mob);
                        wallJudge = false;
                        // [wall18] only the pull the rule is for: one mob drawn out from a pull spot, won with its
                        // health now. b359: of 28 "pulls instead of walling", Manue's were wins only at full (ehp 113 of
                        // 243) and Gnetlibozz's a 2-mob pack at 213 of 225 in Silver Stream Mine - it engaged the next
                        // pack there, 4 came, it died
                        // [death464] ... and only when the walk's own meeting with the reach brings no more than that
                        // pull: the walk aggroes the mob where its leg enters the reach, not at the pull spot. b470
                        // Jurdogs L19 "pulls Murloc Flesheater instead of walling ... fought at its pull spot
                        // (-9378,-2522)", met it 19 yd off at (-9376,-2541) inside a Lake Thresher L25 elite's reach,
                        // which came
                        bool const meetsMore = priced && wallPack > pull.mobs;
                        if (meetsMore && pull.kind == Verdict::Kind::Win && pull.mobs == 1 && pull.pullSpot)
                        {
                            static std::mutex meetLogLock;  // a log cadence per bot and mob, no decision reads it
                            static std::unordered_set<uint64> meetLogged;
                            bool fresh = false;
                            {
                                std::lock_guard<std::mutex> guard(meetLogLock);
                                if (meetLogged.size() > 65536)
                                    meetLogged.clear();
                                fresh = meetLogged.insert(mob->GetGUID().GetCounter() * 1000003ull +
                                                          bot->GetGUID().GetCounter()).second;
                            }
                            if (fresh)
                                LOG_INFO("playerbots", "[death464] {} walls its walk at {} L{}: its pull spot wins "
                                         "1 mob, the walk meets its reach with {} - fought there",
                                         bot->GetName(), mob->GetName(), mob->GetLevel(), wallPack);
                        }
                        if (!meetsMore && pull.kind == Verdict::Kind::Win && pull.mobs == 1 && pull.pullSpot)
                        {
                            static std::mutex pullLogLock;
                            static std::unordered_set<uint64> pullLogged;
                            bool fresh = false;
                            {
                                std::lock_guard<std::mutex> guard(pullLogLock);
                                if (pullLogged.size() > 65536)
                                    pullLogged.clear();
                                fresh = pullLogged.insert(mob->GetGUID().GetCounter() * 1000003ull +
                                                          bot->GetGUID().GetCounter()).second;
                            }
                            if (fresh)
                                LOG_INFO("playerbots", "[wall17] {} pulls {} L{} instead of walling: judged {}",
                                         bot->GetName(), mob->GetName(), mob->GetLevel(), pull.why);
                            why = RouteWall::Why::None;
                        }
                    }
                }

                // [runpast11] a pack it does not fight is run past when it lives through the chase, as an orange
                // one is: the same leash model and the same sum
                // [lag359] ... from the mobs a walk past brings (its engage call and formation: Links; the chase's own
                // assistance calls are tryRunPast's), not the fight's grown pack: a run deals no damage, so no member
                // flees for help and no caster stands to cast at the fight spot. b447 Dekorhan L22 on the Zoram
                // Strand: the Wrathtail wanderers (30 yd apart, assistance radius 10) priced as one grown pack of 11,
                // "run-past priced 2271 hp vs hp 1152", the pier and Darnassus given up, hearthed
                // [dpszero2] ... but NOT where the judge above has just called the fight lost: that is the zone the
                // user's rule keeps the bot out of, and a run past is how it used to get in. The verdict the judge
                // already produced is read here; nothing is recomputed
                KillRate::Zone zone;
                zone.judged = true;
                zone.fightLost = why == RouteWall::Why::Loses;
                zone.chaseSurvived = true;  // asked of the fight alone first
                if (why != RouteWall::Why::None && mayRunPast && KillRate::MayRunPast(zone))
                {
                    buildAround();
                    PullModel walkPast(bot, mob, around);
                    walkPast.Links();
                    zone.chaseSurvived = tryRunPast(mob, walkPast, runPastDamage, runPastHp);
                    if (KillRate::MayRunPast(zone))
                    {
                        ranPast = true;
                        why = RouteWall::Why::None;
                    }
                }
            }
            // [party75] coordinator 2026-11-10: a co-op / help party leader's walk is priced as a solo walk is, with
            // the party's capacity: a mob of its band whose reach crosses the leg is judged by the party judge
            // (NewRpgCoop::JudgePull: its pack on each member, the party's damage and heals); a pack the party loses
            // walls the walk like a solo bot's lost pack (the search goes round it, else the maze), a pack it wins
            // stays a priced fight on the way. Before, a party walk refused only the mobs it never fights and walked
            // into every pack whose fight cost less than the detour (b415 Hobliir L18 with Selalleas: 33 pulls of the
            // Myst Spinners / Leechers on the 879 yd walk to q9669's single 17683)
            if (why == RouteWall::Why::None && !alone && !wallJudge && NewRpgCoop::JudgesPulls(bot))
            {
                G3D::Vector3 const a(from.x, from.y, 0.0f);
                G3D::Vector3 const ab = G3D::Vector3(to.x, to.y, 0.0f) - a;
                G3D::Vector3 const c(centre.GetPositionX(), centre.GetPositionY(), 0.0f);
                float const t = std::clamp((c - a).dot(ab) / std::max(ab.squaredLength(), 0.01f), 0.0f, 1.0f);
                if ((a + ab * t - c).squaredLength() <= radius * radius)
                {
                    // [party75] the verdict per mob spawn and party (its leader, members and their levels): the
                    // judge runs once for them, its cost measured (a line per 200 judged)
                    uint64 partyKey = bot->GetGUID().GetCounter();
                    if (Group* group = bot->GetGroup())
                        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                            if (Player* m = ref->GetSource())
                                partyKey = partyKey * 1000003ull + (uint64(m->GetGUID().GetCounter()) << 8) +
                                           m->GetLevel();
                    uint64 const judgeKey = partyKey * 1000003ull + mob->GetSpawnId();
                    thread_local std::unordered_map<uint64, std::pair<bool, uint32>> judged;  // key -> (lost, mobs)
                    static std::atomic<uint64> judgeCalls{0}, judgeCached{0}, judgeUs{0};
                    bool lostPack = false;
                    uint32 lostMobs = 0;
                    std::string lostWhy;
                    if (auto const found = judged.find(judgeKey); found != judged.end())
                    {
                        lostPack = found->second.first;
                        lostMobs = found->second.second;
                        ++judgeCached;
                    }
                    else
                    {
                        auto const start = std::chrono::steady_clock::now();
                        wallJudge = true;
                        Verdict const party = NewRpgCoop::JudgePull(bot, mob);
                        wallJudge = false;
                        judgeUs += uint64(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count());
                        lostPack = party.kind == Verdict::Kind::Refuse && party.mobs > 0;
                        lostMobs = party.mobs;
                        lostWhy = party.why;
                        if (judged.size() > 65536)
                            judged.clear();
                        judged[judgeKey] = {lostPack, lostMobs};
                        if (uint64 const n = ++judgeCalls; n % 200 == 0)
                            LOG_INFO("playerbots", "[party75] judge: {} judged, {} answered from the cache, {:.1f} us "
                                     "each judged",
                                     n, judgeCached.load(), double(judgeUs.load()) / double(n));
                    }
                    Verdict party;
                    party.mobs = lostMobs;
                    party.why = lostWhy.empty() ? std::string("the party loses its pack (judged before)") : lostWhy;
                    if (lostPack)
                    {
                        why = RouteWall::Why::Loses;
                        wallPack = party.mobs;
                        static std::mutex partyLogLock;  // one line per mob and leader: a log cadence
                        static std::unordered_set<uint64> partyLogged;
                        bool fresh = false;
                        {
                            std::lock_guard<std::mutex> guard(partyLogLock);
                            if (partyLogged.size() > 65536)
                                partyLogged.clear();
                            fresh = partyLogged.insert(mob->GetGUID().GetCounter() * 1000003ull +
                                                       bot->GetGUID().GetCounter()).second;
                        }
                        if (fresh)
                            LOG_INFO("playerbots", "[party75] {} L{} walls its party's walk at {} L{}: {}",
                                     bot->GetName(), bot->GetLevel(), mob->GetName(), mob->GetLevel(), party.why);
                    }
                }
            }
            bool const never = why != RouteWall::Why::None;
            // [death471] the wall carries the fact it was made from: which creature, and the very disc its refusal
            // was computed over - the one that goes into `refuse` on the next line
            if (never)
                walls.push_back({mob->GetEntry(), mob->GetLevel(), why, wallPack,
                                 bot->GetExactDist2d(centre.GetPositionX(), centre.GetPositionY()), runPastDamage,
                                 runPastHp, fightLost, mob->GetGUID().GetRawValue(), centre.GetPositionX(),
                                 centre.GetPositionY(), radius});
            if (never)
                refuse.emplace_back(centre.GetPositionX(), centre.GetPositionY(), centre.GetPositionZ(), radius);
            // [deaths] a patrol it must never fight is refused along its beat (its waypoint forecast), not only where
            // it stands now: the walk crosses its beat only where PatrolWait finds the window (b338 Murloc Lurker
            // patrols at Stone Cairn Lake on the way back of Ravelandy's runs)
            std::vector<G3D::Vector3> track;
            float sampleSec = 0.0f;
            if (never && mob->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE &&
                PullPlanning::PatrolForecast(mob, track, sampleSec))
            {
                G3D::Vector3 last(centre.GetPositionX(), centre.GetPositionY(), centre.GetPositionZ());
                for (G3D::Vector3 const& at : track)
                    if ((at - last).length() >= radius)  // a disc per radius of its beat: the beat covered
                    {
                        refuse.emplace_back(at.x, at.y, at.z, radius);
                        last = at;
                    }
            }
            // the fight it starts, in yards of walk: its health over the bot's damage, at the bot's run speed
            Leader::Travel::RoutePenalty penalty;
            penalty.x = centre.GetPositionX();
            penalty.y = centre.GetPositionY();
            penalty.z = centre.GetPositionZ();
            penalty.r = radius;
            // a mob it must never fight, when no walk round it is complete: any detour up to the whole trip again is
            // the better walk (b301: Thokhen L3 walked 4 times through the same L9 Nightstalkers on a 1943 yd walk)
            float const anyDetour = (to - from).length() + 2.0f * radius;
            // [dpszero1] an unbounded kill time (no estimate, or it deals nothing) is priced at that same value
            // instead of at a floored 1 dps: the number already in the code for "going round is better"
            float const killSec = KillRate::KillSec(rate, float(mob->GetHealth()));
            penalty.cost = std::isfinite(killSec) ? killSec * speed : anyDetour;
            if (never || ranPast)
                penalty.cost = std::max(penalty.cost, anyDetour);
            penalties.push_back(penalty);
            seen[mob->GetGUID().GetRawValue()] = never;
        }
        // [deaths] its danger zones (where it died, a pack it refused) are refused whether or not their mobs are in
        // sight now (b338 Keenonk L11 died twice at the same Black Slime L20 and twice among the same Dragonmaw Scouts:
        // "unseen" in the route search before each death)
        // [lag365] coordinator 2026-11-12 (b448 Dekorhan L22: "MAZE no way to (2252,-1356) ... walls: no mob wall"):
        // a zone of a pull it refused is no wall of its own - its mobs are the ones this search prices above, each
        // run past or judged as any mob on the way (the zone keeps its quest spots and pulls out, InDangerZone); only
        // where it died stays refused
        // [surv10] coordinator 2026-11-14: a zone where it died costs, it does not forbid. Refusing the ground traded
        // deaths for stuckness - Kylantino L22 logged "MAZE no way" and MOVE_FAR_LOOP in Arathi while every way out
        // was walled - and the block did not even stop the deaths it was there for: b484 Rollothan L18 died 10 yd
        // INSIDE a zone its own earlier death had recorded (Redridge Basher (-8955,-2081) r 20), so the zone was
        // remembered and influenced nothing. As a cost the search finds a way round by itself when one is cheaper,
        // and crosses the zone knowingly when none is, so no ground becomes unreachable at any value.
        // The price is the zone's own recorded data and the same formula the live mob above is priced by (its health
        // over the bot's damage at its run speed, yards of walk): the creature that killed the bot there, by its
        // template. Game numbers, no multiplier of mine.
        {
            std::vector<G3D::Vector4> zoneDiscs;
            std::vector<uint32> zoneEntries;
            NewRpgBaseAction::DeathZoneCosts(bot, bot->GetPositionX(), bot->GetPositionY(), range, zoneDiscs,
                                             zoneEntries);
            std::string priced;
            uint32 unpriceable = 0;
            for (size_t i = 0; i < zoneDiscs.size() && i < zoneEntries.size(); ++i)
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(zoneEntries[i]);
                CreatureBaseStats const* stats =
                    tmpl ? sObjectMgr->GetCreatureBaseStats(tmpl->maxlevel, tmpl->unit_class) : nullptr;
                if (!tmpl || !stats)
                {
                    // no template for a recorded entry: priced by nothing, so it is left out rather than turned back
                    // into a wall (the coordinator's condition - a zone is never impassable)
                    ++unpriceable;
                    continue;
                }
                Leader::Travel::RoutePenalty penalty;
                penalty.x = zoneDiscs[i].x;
                penalty.y = zoneDiscs[i].y;
                penalty.z = zoneDiscs[i].z;
                penalty.r = zoneDiscs[i].w;
                // [dpszero1] the same reading, no floor: an unbounded kill time leaves the zone unpriceable, which
                // this loop already has an answer for - it is left out rather than turned into a wall
                float const zoneKillSec = KillRate::KillSec(rate, float(stats->GenerateHealth(tmpl)));
                if (!std::isfinite(zoneKillSec))
                {
                    ++unpriceable;
                    continue;
                }
                penalty.cost = zoneKillSec * speed;
                penalties.push_back(penalty);
                if (priced.size() < 200)
                    priced += Acore::StringFormat("{}{} at ({:.0f},{:.0f}) r {:.0f} costs {:.0f} yd",
                                                  priced.empty() ? "" : ", ", tmpl->Name, penalty.x, penalty.y,
                                                  penalty.r, penalty.cost);
            }
            if (!priced.empty() || unpriceable)
                LOG_INFO("playerbots",
                         "[surv10] {} L{} prices its death zone(s) on this walk instead of refusing them: {}{}",
                         bot->GetName(), bot->GetLevel(), priced.empty() ? std::string("-") : priced,
                         unpriceable ? Acore::StringFormat("; {} zone(s) left unpriced (no creature template)",
                                                           unpriceable)
                                     : std::string());
        }
        // [surv10] a chase that walled this walk only because the bot is not at full: the walk rests first, the same
        // answer the fight judge gives through RestBeforeWalk. The mob stays a wall for THIS search - the bot is not
        // at full yet - and the search after the rest prices it against the full budget. Outside every lock here
        // (RequestRest takes the solo lock); one line per bot and reason, no decision reads the line.
        if (!restForChase.empty() && !bot->IsInCombat())
        {
            static std::mutex restLogLock;
            static std::unordered_map<uint32, std::string> restLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(restLogLock);
                if (restLogged.size() > 4096)
                    restLogged.clear();
                std::string& last = restLogged[bot->GetGUID().GetCounter()];
                fresh = last != restForChase;
                if (fresh)
                    last = restForChase;
            }
            if (fresh)
                LOG_INFO("playerbots", "[surv10] {} L{} rests before its walk instead of walling it: {}",
                         bot->GetName(), bot->GetLevel(), restForChase);
            RequestRest(bot, restForChase);
        }
        if (!ungrown.empty())  // [death431] what GrowOnWalk needs (`around` was built for them)
        {
            ungrownCosts.walker = bot->GetGUID().GetRawValue();
            ungrownCosts.from = from;
            ungrownCosts.to = to;
            ungrownCosts.mobs = std::move(ungrown);
            ungrownCosts.around.clear();
            for (Creature* c : around)
                ungrownCosts.around.push_back(c->GetGUID());
            ungrownCosts.own = own;
            ungrownCosts.petHealth = petHealth;
        }
        // [runpast11] the packs this leg runs past, one line per change of them
        std::string runPastLine;
        if (!ranPastNames.empty())
        {
            // [runpast88] the gate judges on High(), so the line prints the pool that gate was asked and the high
            // price beside it - the two numbers that decided, not an average nobody judged on
            float const kit = RunPastPool(bot, runPast.High(), runPast.seconds);
            runPastLine =
                runPast.damage == std::numeric_limits<float>::max()
                    // [runpast89] a boss, a server with no leash radius, or a chaser the bot cannot outrun: there is
                    // no end to that chase and so no seconds to price it over
                    ? Acore::StringFormat("run-past has no price at hp {:.0f} (no end to the chase): ", kit)
                    : Acore::StringFormat("run-past priced {:.0f} hp ({:.0f} at the high end) vs hp {:.0f} "
                                          "({:.0f} s of chase): ",
                                          runPast.damage, runPast.High(), kit, runPast.seconds);
            for (size_t i = 0; i < ranPastNames.size(); ++i)
                runPastLine += (i ? ", " : "") + ranPastNames[i];
        }
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        if (!runPastLine.empty() && state.runPastLogged != runPastLine)
            LOG_INFO("playerbots", "[solo] {} L{} {}", bot->GetName(), bot->GetLevel(), runPastLine);
        state.runPastLogged = runPastLine;
        // [runpast11] a search from where the bot stands: its run-past set waits for the leg's commit
        // [death452] ... and every search's set is kept by its start: the leg is committed from the search it was cut
        // from (a search whose start the navmesh snapped off the bot's spot, a stage's or a via's walk). Before, only
        // a start within the combat reach in 3D kept it: b467 Salleas L21 "run-past priced 316 hp vs hp 442: Scorched
        // Basilisk L27", its graph stage leg committed with no set, it stood and fought the Basilisk
        if (bot->GetExactDist2d(from.x, from.y) <= RUN_PAST_ON_LEG)
            state.pendingRunPast = chaseOf;
        state.searchRunPast.push_back({from, std::move(chaseOf), {}, float(bot->GetHealth())});
        if (state.searchRunPast.size() > RUN_PAST_SEARCHES_KEPT)
            state.searchRunPast.erase(state.searchRunPast.begin());
        state.routeMs = getMSTime();
        state.routeMobs = std::move(seen);
        state.routeOthersFight = std::move(othersFight);  // [surv21]
        state.routeWalls = std::move(walls);
        state.routeComplete = false;
        state.routeThroughRefused = false;
    }

    void GrowOnWalk(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& to,
                    std::vector<G3D::Vector3> const& walk, std::vector<G3D::Vector4>& lost)
    {
        UngrownCosts const& costs = ungrownCosts;
        Player* bot = walker ? walker->ToPlayer() : nullptr;
        if (!bot || costs.mobs.empty() || walk.size() < 2 || costs.walker != bot->GetGUID().GetRawValue() ||
            costs.from != from || costs.to != to)
            return;
        constexpr float PENALTY_Z = 10.0f;  // the route search's penalty disc height (LongRoute)
        std::vector<Creature*> around;
        bool aroundBuilt = false;
        for (UngrownMob const& entry : costs.mobs)
        {
            G3D::Vector4 const& reach = entry.reach;
            size_t enter = 0;
            while (enter < walk.size() &&
                   !(InDisc(reach, walk[enter]) && std::fabs(walk[enter].z - reach.z) <= PENALTY_Z))
                ++enter;
            if (enter == walk.size())
                continue;  // the walk keeps out of its reach
            // the walk's first point in the reach: on the segment into it, where it crosses the reach's edge
            G3D::Vector3 meet = walk[enter];
            if (enter > 0)
            {
                G3D::Vector3 const a = walk[enter - 1];
                G3D::Vector3 const ab = walk[enter] - a;
                float const ox = a.x - reach.x;
                float const oy = a.y - reach.y;
                float const qa = ab.x * ab.x + ab.y * ab.y;
                float const qb = ox * ab.x + oy * ab.y;
                float const qc = ox * ox + oy * oy - reach.w * reach.w;
                float const disc = qb * qb - qa * qc;
                if (qa > 0.0f && disc >= 0.0f)
                    meet = a + ab * std::clamp((-qb - std::sqrt(disc)) / qa, 0.0f, 1.0f);
            }
            Creature* mob = ObjectAccessor::GetCreature(*bot, entry.mob);
            if (!mob || !mob->IsAlive() || mob->IsInCombat())
                continue;
            if (!aroundBuilt)
            {
                aroundBuilt = true;
                for (ObjectGuid const& guid : costs.around)
                    if (Creature* c = ObjectAccessor::GetCreature(*bot, guid))
                        if (c->IsAlive() && !c->IsInCombat())
                            around.push_back(c);
            }
            PullModel model(bot, mob, around);
            model.Links();
            auto const [loses, size] = GrownLoses(bot, mob, model, around, reach, meet, costs.own, costs.petHealth);
            if (!loses)
                continue;
            lost.push_back(reach);
            static std::mutex lostLogLock;  // one line per walker and mob: a log cadence, no decision reads it
            static std::unordered_set<uint64> lostLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(lostLogLock);
                if (lostLogged.size() > 65536)
                    lostLogged.clear();
                fresh = lostLogged.insert(mob->GetSpawnId() * 1000003ull + bot->GetGUID().GetCounter()).second;
            }
            if (fresh)
                LOG_INFO("playerbots", "[death431] {} L{} walk to ({:.0f},{:.0f}) enters the reach of {} L{} off its "
                         "straight leg at ({:.1f},{:.1f},{:.1f}): the pack met there ({}) loses at full, refused",
                         bot->GetName(), bot->GetLevel(), to.x, to.y, mob->GetName(), mob->GetLevel(), meet.x, meet.y,
                         meet.z, size);
        }
    }

    // [death471] the wall's own fact still holds: the creature is still in the world and still ON THE GROUND its
    // refusal was computed over - the very disc that went into `refuse`, asked of the one judge of that quantity
    // (AggroReach::Covers). A free wanderer is inside its own home disc by construction and stays inside it while
    // it wanders, so its wall never flickers; a patrol's disc sits where it stood, and once it has walked off it
    // the wall describes nothing. A wall whose emitter did not record the fact (`mob` 0) is kept exactly as before:
    // absence of the fact is not "the fact is false". The state lock must NOT be held here - this reads the world
    // and `lock` only guards the state map, so every caller copies its walls out first
    bool WallStandsStill(Player* bot, RouteWall const& wall)
    {
        if (!wall.mob)
            return true;
        Creature* mob = ObjectAccessor::GetCreature(*bot, ObjectGuid(wall.mob));
        if (!mob || !mob->IsAlive())
            return false;  // the spawn that made the refusal is gone: so is the refusal
        AggroReach::Disc const made{wall.atX, wall.atY, 0.0f, wall.atRadius};
        return AggroReach::Covers(made, mob->GetPositionX(), mob->GetPositionY());
    }

    std::string ConWalls(Player* bot)
    {
        if (!bot)
            return std::string();
        std::vector<RouteWall> kept;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const it = states.find(bot->GetGUID().GetCounter());
            if (it == states.end())
                return std::string();
            kept = it->second.routeWalls;
        }
        kept.erase(std::remove_if(kept.begin(), kept.end(),
                                  [&](RouteWall const& w) { return !WallStandsStill(bot, w); }),
                   kept.end());
        std::string out;
        uint32 n = 0;
        for (RouteWall const& w : kept)
            if (w.why == RouteWall::Why::Con || w.why == RouteWall::Why::Elite)
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(w.entry);
                if (++n <= 3)  // a log line's length, no decision
                    out += Acore::StringFormat("{}{} L{} ({})", out.empty() ? "" : ", ", tmpl ? tmpl->Name : "?",
                                               w.level, w.why == RouteWall::Why::Elite ? "elite" : "orange / red");
            }
        return out;
    }

    // [dpszero2] read back, never recomputed: the search wrote these walls and their reasons, and judging the fight
    // again here would be a second judge of the same thing (rule 5)
    // [death471] the one thing asked of the world is whether the wall's own fact still holds - the mob still on the
    // ground its refusal was made of (WallStandsStill). That is not a second verdict on the fight; it is the end of the
    // first one, and without it the verdict outlives the fact it was made from
    bool NearestLostFightWall(Player* bot, LostWall& out)
    {
        if (!bot)
            return false;
        std::vector<RouteWall> walls;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end())
                return false;
            walls = found->second.routeWalls;
        }
        RouteWall const* nearest = nullptr;
        for (RouteWall const& w : walls)
            if (w.fightLost && WallStandsStill(bot, w) && (!nearest || w.yards < nearest->yards))
                nearest = &w;
        if (!nearest)
            return false;
        out = {nearest->entry, nearest->level, nearest->pack, nearest->yards};
        return true;
    }

    std::string RouteWalls(Player* bot)
    {

        if (!bot)
            return std::string();
        std::vector<RouteWall> walls;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end())
                return std::string();
            walls = found->second.routeWalls;
        }
        // [death471] a wall whose mob has left the ground its refusal was made of is no wall any more
        walls.erase(std::remove_if(walls.begin(), walls.end(),
                                   [&](RouteWall const& w) { return !WallStandsStill(bot, w); }),
                    walls.end());
        std::sort(walls.begin(), walls.end(), [](RouteWall const& a, RouteWall const& b) { return a.yards < b.yards; });
        constexpr size_t LOGGED_WALLS = 5;  // the nearest few: a log line's length, no decision
        std::string out;
        for (size_t i = 0; i < walls.size() && i < LOGGED_WALLS; ++i)
        {
            RouteWall const& w = walls[i];
            CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(w.entry);
            char const* why = w.why == RouteWall::Why::Elite   ? "elite"
                              : w.why == RouteWall::Why::Con   ? "orange / red"
                              : w.why == RouteWall::Why::Zone  ? "danger zone"
                              : w.why == RouteWall::Why::Above ? "pack above its level"
                                                               : "loses at full";
            out += Acore::StringFormat("{}{} L{} {:.0f} yd ({}, pack {}", out.empty() ? "" : ", ",
                                       tmpl ? tmpl->Name : std::to_string(w.entry), w.level, w.yards, why, w.pack);
            // [dpszero2] the fight's own verdict, which `why` no longer carries for an elite / orange wall - and
            // the reason no chase is priced below: a zone whose fight is lost is not entered, so there is nothing
            // to price a run through it with
            if (w.fightLost && w.why != RouteWall::Why::Loses)
                out += "; its fight is lost too: no run past priced";
            // [runpast11] the run past it priced and refused: its chase against the bot's health with its kit
            if (w.runPastHp > 0.0f)
                out += w.runPastDamage == std::numeric_limits<float>::max()
                           ? std::string("; no run past: a boss or no leash")
                           : Acore::StringFormat("; run-past priced {:.0f} hp vs hp {:.0f}", w.runPastDamage,
                                                 w.runPastHp);
            out += ")";
        }
        if (walls.size() > LOGGED_WALLS)
            out += Acore::StringFormat(" +{} more", walls.size() - LOGGED_WALLS);
        if (!out.empty())
            return out;
        // [lag365] no mob wall: the refused discs are its death zones or the pull spot's walk - named
        std::string const zones = bot ? NewRpgBaseAction::DangerNames(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                       bot->GetMap()->GetVisibilityRange())
                                      : std::string();
        return zones.empty() ? std::string("no mob wall: the pull spot's walk")
                             : "no mob wall: its danger zones " + zones;
    }

    bool PatrolWait(Player* bot, std::vector<G3D::Vector3> const& points, float& waitSec, std::string& who)
    {
        waitSec = 0.0f;
        who.clear();
        if (!bot || points.size() < 2)
            return false;
        std::vector<uint64> refused;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end())
                return false;
            for (auto const& [guid, never] : found->second.routeMobs)
                if (never)
                    refused.push_back(guid);
        }
        float const speed = std::max(bot->GetSpeed(MOVE_RUN), 0.1f);
        // the walk's points and the seconds the bot needs to each
        std::vector<float> at(points.size(), 0.0f);
        for (size_t i = 1; i < points.size(); ++i)
            at[i] = at[i - 1] + (points[i] - points[i - 1]).length() / speed;
        struct Beat
        {
            std::vector<G3D::Vector3> track;
            float sampleSec = 0.0f;
            float radius = 0.0f;
            std::vector<size_t> crossing;  // the walk's points its beat comes within reach of (no others met)
        };
        std::vector<Beat> beats;
        for (uint64 raw : refused)
        {
            Creature* mob = ObjectAccessor::GetCreature(*bot, ObjectGuid(raw));
            if (!mob || !mob->IsAlive() || mob->IsInCombat())
                continue;
            Beat beat;
            beat.radius = mob->GetAggroRange(bot) + bot->GetCombatReach();
            bool const patrols = mob->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE &&
                                 PullPlanning::PatrolForecast(mob, beat.track, beat.sampleSec) &&
                                 beat.sampleSec > 0.0f && !beat.track.empty();
            if (!patrols)
            {
                // a refused mob that stays where it is: no wait clears a walk through its reach
                Position centre = mob->GetPosition();
                float radius = beat.radius;
                if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                {
                    centre = mob->GetHomePosition();
                    radius += mob->GetWanderDistance();
                }
                for (G3D::Vector3 const& point : points)
                    if (centre.GetExactDist2d(point.x, point.y) <= radius)
                        return false;
                continue;
            }
            for (size_t i = 0; i < points.size(); ++i)
                for (G3D::Vector3 const& spot : beat.track)
                    if (std::hypot(spot.x - points[i].x, spot.y - points[i].y) <= beat.radius)
                    {
                        beat.crossing.push_back(i);
                        break;
                    }
            if (beat.crossing.empty())
                continue;
            who += (who.empty() ? "" : ", ") + Acore::StringFormat("{} L{}", mob->GetName(), mob->GetLevel());
            beats.push_back(std::move(beat));
        }
        if (beats.empty())
            return false;
        // the first start (a forecast sample) from which the walk meets no beat while the forecasts last
        size_t const samples = beats.front().track.size();
        float const step = beats.front().sampleSec;
        for (size_t k = 0; k < samples; ++k)
        {
            float const start = float(k) * step;
            bool clear = true;
            for (Beat const& beat : beats)
                for (size_t i : beat.crossing)
                {
                    if (!clear)
                        break;
                    size_t const index = size_t((start + at[i]) / beat.sampleSec);
                    if (index >= beat.track.size())
                        clear = false;  // past its forecast: unknown
                    else
                    {
                        G3D::Vector3 const& spot = beat.track[index];
                        clear = std::hypot(spot.x - points[i].x, spot.y - points[i].y) > beat.radius;
                    }
                }
            if (clear)
            {
                waitSec = start;
                return true;
            }
        }
        return false;
    }

    void NoteRoute(Unit* walker, bool complete, bool throughRefused)
    {
        Player* bot = walker ? walker->ToPlayer() : nullptr;
        if (!bot || !SoloPlayerBot(bot))
            return;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.routeMs)
            return;
        found->second.routeComplete = complete;
        found->second.routeThroughRefused = throughRefused;
    }

    bool RoamerReaches(Player* bot, G3D::Vector3 const& at, std::string& who)
    {
        who.clear();
        if (!bot || !bot->IsInWorld())
            return false;
        // the look round the spot: the widest wander area of the map with its aggro reach (a search radius)
        float const look = std::min(MaxWanderDistance(bot->GetMapId()) + sPlayerbotAIConfig.SightDistance,
                                    bot->GetMap()->GetVisibilityRange());
        struct NearSpot
        {
            float x, y, r;
            bool operator()(Unit* u) const { return u->GetExactDist2d(x, y) <= r; }
        } check{at.x, at.y, look};
        std::list<Unit*> units;
        Acore::UnitListSearcher<NearSpot> searcher(bot, units, check);
        Cell::VisitObjects(at.x, at.y, bot->GetMap(), searcher, look);
        uint8 const level = bot->GetLevel();
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID())
                continue;
            MovementGeneratorType const moves = mob->GetDefaultMovementType();
            if (moves != RANDOM_MOTION_TYPE && moves != WAYPOINT_MOTION_TYPE)
                continue;  // a mob that stays: the pull judge and the spot's own checks price it
            if (!IsElite(mob) && Acore::XP::GetColorCode(level, mob->GetLevel()) > XP_ORANGE)
                continue;
            float const reach = mob->GetAggroRange(bot) + bot->GetCombatReach();
            bool covers = false;
            if (moves == RANDOM_MOTION_TYPE)
                covers = mob->GetHomePosition().GetExactDist2d(at.x, at.y) <= reach + mob->GetWanderDistance();
            else if (WaypointPath const* path = sWaypointMgr->GetPath(mob->GetWaypointPath()))
            {
                for (WaypointNode const& node : path->Nodes)
                    covers = covers || std::hypot(node.X - at.x, node.Y - at.y) <= reach;
            }
            else
                covers = mob->GetExactDist2d(at.x, at.y) <= reach;
            if (!covers)
                continue;
            who = Acore::StringFormat("{} L{} ({})", mob->GetName(), mob->GetLevel(),
                                      moves == RANDOM_MOTION_TYPE ? "wanderer" : "patrol");
            return true;
        }
        return false;
    }
}
