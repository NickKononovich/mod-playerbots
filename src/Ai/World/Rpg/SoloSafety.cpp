/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SoloSafety.h"
#include "PerfMonitor.h"  // [guild15]

#include "BuyAction.h"
#include "CellImpl.h"
#include "AttackersValue.h"  // [deaths] NO_ATTACK
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DpsEstimate.h"  // [dps1] its own damage from the formula
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
#include "SpellInfo.h"
#include "SurvivalKit.h"  // [deaths3] its heals now
#include "QuestStats.h"   // [budget12] the time budget
#include "SpellMgr.h"
#include "SmartScriptMgr.h"  // [guild14] creature casts
#include "StringFormat.h"
#include <sstream>
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <mutex>
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
    };

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
        // [guild15] the time it attacked (a victim, or a cast / auto shot under way, at the damage event): only that
        // time measures its damage (b332 Colian L12: "melee 38.1 s dealt 0" - an attacker dropped by a sight bug, no
        // victim, no swing - measured him at 4.7 dps at that gap)
        uint32 actMs = 0;
        bool sampleAct = false;
        // [dps1] what invalidates the fight as a formula check: it swam or looted at a damage event
        bool swam = false;
        bool looted = false;
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
        std::vector<RouteWall> routeWalls;  // [pull9]
        std::string runPastLogged;          // [runpast11] the last run-past line (one line per change)
        // [runpast11] the run-past set of the last route search from where the bot stood (mob -> its chase damage) and
        // the committed leg's: its set, its points, the mobs its RUN_PAST lines named
        std::unordered_map<uint64, float> pendingRunPast;
        std::unordered_map<uint64, float> legRunPast;
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
    };

    struct EntryDamage
    {
        double damage = 0.0;
        double sec = 0.0;
    };

    std::mutex lock;
    std::unordered_map<uint32, SoloBotState> states;
    std::unordered_map<uint32, EntryDamage> entryDamage;  // creature entry -> what it dealt to solo bots, per second

    bool SoloPlayerBot(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
            return false;
        if (bot->GetMap() && bot->GetMap()->Instanceable())
            return false;
        // [deaths3] b341: 10 of 12 deaths were grouped bots questing away from every member (the user pack's fives
        // 120-1000 yd apart, offline members): no judge, no walk round the camps, no survival kit. A bot fights alone
        // when no living member stands within the core's group reward distance (Player::IsAtGroupRewardDistance)
        Group* group = bot->GetGroup();
        if (!group || group->GetMembersCount() <= 1)
            return true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
                member->IsAtGroupRewardDistance(bot))
                return false;
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

    // a closed fight goes into the bot's and the entries' measurements (caller holds the lock)
    void CloseFight(Player* bot, SoloBotState& state, bool died)
    {
        if (!state.open)
            return;
        state.open = false;
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
        std::string invalid;
        {
            float const acted = float(state.actMs) / float(IN_MILLISECONDS) + SwingSec(bot);
            if (!state.actMs && !foughtDealt)
            {
                invalid = "never attacked";
                measured = false;
            }
            else if (acted < fought)
                fought = acted;
        }
        if (invalid.empty() && !measured)
            invalid = "ran before two swings";
        if (invalid.empty() && state.swam)
            invalid = "swimming";
        if (invalid.empty() && state.looted)
            invalid = "looting";
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
            DpsEstimate::Result const& f = state.formula;
            sample.formulaDps = f.oomSec < fought && fought > 0.0f
                                    ? (f.burstDps * f.oomSec + std::min(f.whiteDps, f.burstDps) * (fought - f.oomSec)) /
                                          fought
                                    : f.burstDps;
            sample.cycleSec = f.cycleSec;
            sample.dealt = foughtDealt;
            sample.fightSec = seconds;
            sample.activeSec = fought;
            sample.taken = state.taken;
            sample.outcome = died ? "died" : state.fleeing ? "fled" : "won";
            sample.invalidWhy = invalid;
            sample.ctx = state.formulaCtx;
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
        state.swam = false;
        state.looted = false;
        state.formula = DpsEstimate::Result();
        state.formulaEntry = 0;
        state.formulaLevel = 0;
        state.loggedNoAttack = false;
    }

    // [guild15] the melee-phase clock: the time since the last sample counts as melee when the bot stood in reach then
    void SampleMelee(Player* bot, Unit* mob, SoloBotState& state, uint32 now)
    {
        if (state.sampleMs && state.sampleMelee)
            state.meleeMs += getMSTimeDiff(state.sampleMs, now);
        if (state.sampleMs && state.sampleAct)
            state.actMs += getMSTimeDiff(state.sampleMs, now);  // [guild15]
        state.sampleMs = now;
        state.sampleMelee = bot->IsWithinMeleeRange(mob);
        state.sampleAct = bot->GetVictim() || bot->IsNonMeleeSpellCast(false);
        state.swam = state.swam || bot->IsInWater();  // [dps1]
        state.looted = state.looted || !bot->GetLootGUID().IsEmpty();
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

    // [guild14] a creature's spell damage per second from its SmartAI: every in-combat repeating cast (SMART_EVENT_
    // UPDATE_IC + SMART_ACTION_CAST) of a damaging spell - its direct school damage and its periodic damage over the
    // aura's duration - once per repeat interval (the mean of repeatMin / repeatMax), at least its cast time and the
    // global cooldown. b303: casters were counted by their current cast only (Kobold Geomancers, Defias Rogue Wizards,
    // Gnarlpine and Bristlelimb Shamans killed bots judged to win)
    // [deaths] a hostile spell's damage on its target as a creature casts it: its direct school damage and its periodic
    // damage over the aura's duration (0: none, a positive spell)
    float CastDamage(SpellInfo const* info, Creature* creature)
    {
        if (!info || info->IsPositive())
            return 0.0f;
        float damage = 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                damage += float(std::max(effect.CalcValue(creature), 0));
            else if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE &&
                     effect.Amplitude > 0)
                damage += float(std::max(effect.CalcValue(creature), 0)) *
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
                        heal.amount += float(std::max(effect.CalcValue(creature), 0));
                    else if (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                             effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL && effect.Amplitude > 0)
                        heal.amount += float(std::max(effect.CalcValue(creature), 0)) *
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
    float MeleeDps(Unit* unit, Player* bot)
    {
        float const raw = std::max(
            0.0f, (unit->GetFloatValue(UNIT_FIELD_MINDAMAGE) + unit->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f);
        float const hit =
            float(Unit::CalcArmorReducedDamage(unit, bot, uint32(raw), nullptr, unit->GetLevel(), BASE_ATTACK));
        int32 const skillDiff =
            int32(unit->GetWeaponSkillValue(BASE_ATTACK, bot)) - int32(bot->Unit::GetMaxSkillValueForLevel(unit));
        float const skillBonus = 0.04f * float(skillDiff);
        float left = 100.0f;
        auto const take = [&left](float chance)
        {
            chance = std::clamp(chance, 0.0f, left);
            left -= chance;
            return chance;
        };
        take(unit->MeleeSpellMissChance(bot, BASE_ATTACK, skillDiff, 0));
        float block = 0.0f;
        if (!PlayerbotAI::IsCaster(bot))
        {
            take(bot->GetUnitDodgeChance() - skillBonus);
            take(bot->GetUnitParryChance() - skillBonus);
            block = take(bot->GetUnitBlockChance() - skillBonus);
        }
        float const crit = take(unit->GetUnitCriticalChance(BASE_ATTACK, bot));
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
        return std::max(perSwing, 0.0f) / SwingSec(unit);
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
        // [deaths14] a caster that stands to cast (SMART_ACTION_CAST on its victim with SMARTCAST_COMBAT_MOVE: "prevents
        // combat movement if cast successful"): it stops at that spell's range from the bot, not at the fight spot -
        // its periodic assistance calls (Creature::Update -> CallAssistance) are made from there
        float standRange = 0.0f;
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
                    help.flees = true;
                else if (holder.action.type == SMART_ACTION_CAST && holder.target.type == SMART_TARGET_VICTIM &&
                         (holder.action.cast.castFlags & SMARTCAST_COMBAT_MOVE))
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell))
                        help.standRange = std::max(help.standRange, info->GetMaxRange(false));
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
        float const botDps = std::max(own.burstDps, 1.0f);
        float const whiteDps = std::clamp(own.whiteDps, 1.0f, botDps);
        float const maxMana = float(bot->GetMaxPower(POWER_MANA));
        bool const spends = maxMana > 0.0f && own.manaPerSec > 0.0f;
        float const spellSec = spends ? maxMana / own.manaPerSec : std::numeric_limits<float>::max();
        float seconds = 0.0f;
        float damage = 0.0f;
        float targetDamage = 0.0f;
        for (RouteFoe const& foe : foes)
        {
            float hp = foe.hp + HealToCome(foe.mob, foe.hp / botDps);
            if (seconds < spellSec)
            {
                float const spellPart = (spellSec - seconds) * botDps;
                float const part = std::min(hp, spellPart);
                seconds += part / botDps;
                hp -= part;
            }
            seconds += hp / whiteDps;
            damage += foe.dps * seconds;
            if (&foe == &foes.front())
                targetDamage = foe.dps * seconds;
        }
        float const health = float(bot->GetMaxHealth());
        if (damage < health)
            return false;  // no heal needed (the heal kit is read only for a close call)
        float const spare = spends ? std::max(0.0f, maxMana - own.manaPerSec * seconds) : maxMana;
        float const heals = SurvivalKit::HealPoints(bot, seconds, spare, true);
        float const ehp = health + std::min(petHealth, targetDamage) + heals;
        return damage >= ehp;
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

    // [pullplan] an idle mob's aggro reach on the bot as a disc (x, y, z, radius): a wanderer's round its home with
    // its wander area on top
    G3D::Vector4 AggroDisc(Creature* mob, Player* bot)
    {
        float radius = mob->GetAggroRange(bot) + bot->GetCombatReach();
        Position centre = mob->GetPosition();
        if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
        {
            centre = mob->GetHomePosition();
            radius += mob->GetWanderDistance();
        }
        return G3D::Vector4(centre.GetPositionX(), centre.GetPositionY(), centre.GetPositionZ(), radius);
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
                    if (nearest)
                    {
                        pack.push_back({nearest, true});
                        changed = true;
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

    // [pullplan] the bot's pull from range: its rotation's reach for a ranged bot; else Throw / Shoot with the ranged
    // weapon (and ammunition) that spell needs; 0 when it has none (a body pull at the target's aggro edge)
    float PullReach(Player* bot, uint32* spellOut = nullptr)
    {
        if (spellOut)
            *spellOut = 0;
        if (PlayerbotAI::IsRanged(bot))
            return Leader::Fight::LeaderSpellReach(bot);
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
    namespace
    {
        // [wall17] a judge asked by the route search for a mob on the walk (RouteCosts): it reads and writes none of
        // the bot's engage state (the pull spot search kept for its target, the verdict line), and the route searches
        // of its own pull spot search price their mobs without asking it again (no recursion)
        thread_local bool wallJudge = false;
    }

    float MapWanderReach(uint32 mapId) { return MaxWanderDistance(mapId); }

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

    float BotDps(Player* bot, Unit* target)
    {
        if (!bot)
            return 0.0f;
        return OwnDamage(bot, target).dps;
    }

    Verdict Judge(Player* bot, Unit* target)
    {
        Verdict verdict;
        if (!target || !Applies(bot))
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
        for (ObjectGuid const& guid :
             GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
            if (Creature* mob = ObjectAccessor::GetCreature(*bot, guid))
                if (mob != target && mob->IsAlive() && !mob->IsInCombat() && mob->IsHostileTo(bot) &&
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
            if (range > sight)
            {
                std::list<Unit*> units;
                Acore::AnyUnitInObjectRangeCheck check(bot, range);
                Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
                Cell::VisitObjects(bot, searcher, range);
                for (Unit* unit : units)
                {
                    Creature* mob = unit->ToCreature();
                    if (!mob || mob == target || bot->GetExactDist2d(mob) <= sight || !mob->IsAlive() ||
                        mob->IsInCombat() || !mob->IsHostileTo(bot) || !mob->HasReactState(REACT_AGGRESSIVE) ||
                        mob->GetCharmerOrOwnerGUID() ||
                        std::find(idle.begin(), idle.end(), mob) != idle.end())
                        continue;
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
        }
        else
            pack.push_back(target);
        for (Unit* attacker : bot->getAttackers())
            if (attacker && attacker->IsAlive() && std::find(pack.begin(), pack.end(), attacker) == pack.end())
                pack.push_back(attacker);

        struct Mob
        {
            Unit* unit;
            float hp;
            float dps;
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
                    verdict.radius = std::max(verdict.radius,
                                              unit->GetExactDist2d(target) + creature->GetAggroRange(bot));
                }
                if (unit->GetLevel() > bot->GetLevel())
                    ++verdict.above;
                verdict.topLevel = std::max<uint8>(verdict.topLevel, unit->GetLevel());
                mobs.push_back({unit, float(unit->GetHealth()), dps});
            }
        }
        verdict.mobs = mobs.size();

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
        float const botDps = std::max(own.burstDps, 1.0f);
        // [deaths] each mob's health with the heals it casts on itself over the time the bot needs for it
        for (Mob& mob : mobs)
            if (Creature* creature = mob.unit->ToCreature())
                mob.hp += HealToCome(creature, mob.hp / botDps);

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
        float const whiteDps = std::clamp(own.whiteDps, 1.0f, botDps);
        struct Projection
        {
            float seconds = 0.0f;
            float damage = 0.0f;
            float targetDamage = 0.0f;  // the target's part (a taunting pet's to take)
        };
        auto const project = [&](float mana) -> Projection
        {
            float const spellSec = bot->GetMaxPower(POWER_MANA) && manaPerSec > 0.0f
                                       ? mana / manaPerSec
                                       : std::numeric_limits<float>::max();
            Projection out;
            for (Mob const& mob : mobs)
            {
                float hp = mob.hp;
                if (out.seconds < spellSec)
                {
                    float const spellPart = (spellSec - out.seconds) * botDps;
                    if (hp <= spellPart)
                    {
                        out.seconds += hp / botDps;
                        hp = 0.0f;
                    }
                    else
                    {
                        out.seconds = spellSec;
                        hp -= spellPart;
                    }
                }
                out.seconds += hp / whiteDps;
                out.damage += mob.dps * out.seconds;
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
        auto const effective = [&](Projection const& fight, float health, float mana, bool& wins) -> float
        {
            float spare = mana;
            if (bot->GetMaxPower(POWER_MANA) && manaPerSec > 0.0f)
                spare = std::max(0.0f, mana - manaPerSec * fight.seconds);
            float const heals = SurvivalKit::HealPoints(bot, fight.seconds, spare, true);
            float const ehp = health + std::min(petHealth, fight.targetDamage) + heals;
            wins = fight.damage < ehp;
            return ehp;
        };
        bool winNow = false;
        bool winFull = false;
        verdict.ehpNow = effective(now, float(bot->GetHealth()), float(bot->GetPower(POWER_MANA)), winNow);
        verdict.ehpFull =
            effective(atFull, float(bot->GetMaxHealth()), float(bot->GetMaxPower(POWER_MANA)), winFull);
        bool const full = bot->GetHealth() >= bot->GetMaxHealth() &&
                          (!bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA));

        if (elite)
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = "an elite in the pack";
        }
        else if (verdict.above >= 2)
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = Acore::StringFormat("{} mobs above its level at once", verdict.above);
        }
        else if (winNow && (!verdict.above || full))
            verdict.kind = Verdict::Kind::Win;
        else if (winFull)
        {
            verdict.kind = Verdict::Kind::RestFirst;
            verdict.why = winNow ? "a mob above its level: at full health and mana only" : "wins only at full";
        }
        else
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = "loses even at full";
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

    void NoteEngage(Player* bot, Unit* target, Verdict const& verdict)
    {
        if (!bot || !target)
            return;
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
                 "ehp {:.0f}, fought {} ({:.0f},{:.0f}), {:.0f} yd from the target",
                 bot->GetName(), bot->GetLevel(), target->GetName(), target->GetLevel(), verdict.mobs, verdict.above,
                 verdict.damage, verdict.fightSec, verdict.ehpNow,
                 verdict.pullSpot ? "at its pull spot" : (PlayerbotAI::IsRanged(bot) ? "at its reach" : "at the target"),
                 verdict.pullSpot ? verdict.pull.x : target->GetPositionX(),
                 verdict.pullSpot ? verdict.pull.y : target->GetPositionY(),
                 verdict.pullSpot ? target->GetExactDist2d(verdict.pull.x, verdict.pull.y) : 0.0f);
        state.engaged = true;
        state.engage.target = target->GetGUID();
        state.engage.targetName = target->GetName();
        state.engage.predicted = verdict.damage;
        state.engage.health = float(bot->GetHealth());
        state.engage.healthPct = bot->GetHealthPct();
        state.engage.manaPct = bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f;
        state.engage.mobs = verdict.mobs;
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
            if (bot->IsInCombat())
                state.rest = false;
            return false;
        }
        bool const healthFull = bot->GetHealth() >= bot->GetMaxHealth();
        bool const manaFull =
            !bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA);
        if (healthFull && manaFull)
        {
            state.rest = false;
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

    bool WaitsToRegen(Player* bot)
    {
        if (!bot || bot->IsInCombat())
            return false;
        bool const health = WantsRest(bot, false);
        bool const mana = WantsRest(bot, true);
        // something to eat or drink for what it lacks: the rest actions sit it down with that
        bool const eats = health && CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD);
        bool const drinks = mana && CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK);
        return (health || mana) && !eats && !drinks;
    }

    bool RestBeforeWalk(Player* bot, Position const& dest)
    {
        if (!bot || bot->IsInCombat() || !bot->IsAlive() || !Applies(bot))
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
        float const fightSec = float(stats->BaseHealth[expansion]) / std::max(OwnDamage(bot, nullptr).dps, 1.0f);
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
        state.restWhy = "after a death";
    }

    // [guild80] the open solo fight projected (the attackers' damage still to come against its health and heals),
    // computed as the b304 disengage judged it; SurvivalKit picks the escape from it
    Projection Project(Player* bot)
    {
        Projection out;
        if (!bot || !bot->IsAlive() || !bot->IsInCombat() || !Applies(bot))
            return out;
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.open)
            return out;
        SoloBotState& state = found->second;
        out.open = true;
        out.fleeing = state.fleeing;

        // the attackers still on it (a mob on its pet hits the pet), their damage: measured this fight (each from its
        // first hit, a swing added), the model before their first hit - the higher
        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != bot)
                continue;
            float dps = MobDps(attacker, bot);
            auto const track = state.attackers.find(attacker->GetGUID().GetRawValue());
            // its measurement this fight once it has swung twice (a single hit, a crit, is no rate), else the model
            if (track != state.attackers.end() && track->second.damage &&
                getMSTimeDiff(track->second.firstMs, track->second.lastMs) >=
                    uint32(track->second.swingSec * IN_MILLISECONDS))
                dps = float(track->second.damage) /
                      (float(getMSTimeDiff(track->second.firstMs, now)) / IN_MILLISECONDS + track->second.swingSec);
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

        // its damage: the higher of the formula (against its victim) and this fight's once it has swung (b304 run 1:
        // this fight's first seconds alone - a caster's first bolt still in flight - sent 9 bots running in 10 min)
        float const elapsed = float(getMSTimeDiff(state.firstMs, now)) / IN_MILLISECONDS;
        float botDps = OwnDamage(bot, bot->GetVictim()).dps;  // [dps1] no fight history
        if (state.dealt && elapsed >= SwingSec(bot))
            botDps = std::max(botDps, float(state.dealt) / (elapsed + SwingSec(bot)));
        // [guild80] ... and this fight's alone once it covers two of its swings: b319 Rumanatol L11 dealt 5.0 dps to a
        // Blue Flutterer (10.2 measured at that gap), Rollothan L8 5.7 (8.5): the fight showed lost only at hp 43 / 4
        // [guild15] once it has dealt damage: a bot hit on its walk before it turned on the mob had dealt 0 after two
        // swings, the projection read 1.0 dps and it ran from L8 mobs it kills in 10 s (b321 Colian L11, Nightstalkers)
        if (elapsed >= 2.0f * SwingSec(bot) && state.dealt)
            botDps = float(state.dealt) / (elapsed + SwingSec(bot));
        // [deaths] ... and while its attackers stand in its melee reach, what it deals there once that part covers two
        // of its swings: the mob is on it, the rest of the fight is fought there. b337: Rumanatol L11 at The Lost Fold
        // stood "melee 38.1 s dealt 0" under a Bloodmyst Hatchling, the formula (18.9 dps) read the fight won until his
        // health was gone; Eraelor L7 (hunter, no pet) dealt 10.2 dps at range for 6 s, then 2.4 dps in melee for
        // 40 s - the whole fight's average read won until the run cost more than his health, and he died
        if (float const meleeSec = float(state.meleeMs) / IN_MILLISECONDS;
            state.sampleMelee && meleeSec >= 2.0f * SwingSec(bot))
            botDps = float(state.dealtMelee) / (meleeSec + SwingSec(bot));
        out.botDps = std::max(botDps, 1.0f);
        // [deaths] each attacker's health with the heals it still casts on itself over the time the bot needs for it
        for (Projection::Foe& foe : out.foes)
            if (Creature* creature = foe.unit->ToCreature())
                foe.hp += HealToCome(creature, foe.hp / out.botDps);
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
            out.seconds += foe.hp / out.botDps;
            out.damage += foe.dps * out.seconds;
        }
        // [deaths3] no measured heal rate (no fight history): the projection is the fight without its kit; the kit's
        // heals are the pieces that turn it won
        out.heals = 0.0f;
        out.health = float(bot->GetHealth());
        out.lost = out.damage >= out.health + out.heals;
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

    bool StandsInReach(Player* bot, std::string& who, std::vector<G3D::Vector3>& spots)
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
        float out = 0.0f;  // the yards out of the covering reaches
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID())
                continue;
            G3D::Vector4 const disc = AggroDisc(mob, bot);
            discs.push_back(disc);
            if (!InDisc(disc, here))
                continue;
            out = std::max(out, disc.w - std::hypot(disc.x - here.x, disc.y - here.y));
            who += Acore::StringFormat("{}{} L{}", who.empty() ? "" : ", ", mob->GetName(), mob->GetLevel());
        }
        if (who.empty())
            return false;
        constexpr uint32 DIRECTIONS = 8;  // the direction search's eighths all round (as the flee's maze search)
        for (float const ring : {out + bot->GetCombatReach(), sight})
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = here.x + std::cos(angle) * ring;
                float y = here.y + std::sin(angle) * ring;
                float z = here.z;
                bot->UpdateAllowedPositionZ(x, y, z);
                G3D::Vector3 const at(x, y, z);
                if (std::none_of(discs.begin(), discs.end(), [&at](G3D::Vector4 const& d) { return InDisc(d, at); }))
                    spots.push_back(at);
            }
        return true;
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
        std::unordered_map<uint32, std::pair<uint64, std::string>> partyPull;

        // the group's judged pull among the mobs that hit the bot this fight (`state`: the bot's, lock held)
        bool PartyPullHit(uint32 group, SoloBotState const& state, std::string& pulled)
        {
            std::lock_guard<std::mutex> guard(partyPullLock);
            auto const found = partyPull.find(group);
            if (found == partyPull.end() || !state.attackers.count(found->second.first))
                return false;
            pulled = found->second.second;
            return true;
        }
    }

    void NotePartyPull(Player* leader, Unit* target)
    {
        if (!leader || !target || !leader->GetGroup())
            return;
        std::lock_guard<std::mutex> guard(partyPullLock);
        partyPull[leader->GetGroup()->GetGUID().GetCounter()] = {target->GetGUID().GetRawValue(), target->GetName()};
    }

    std::string DeathContext(Player* bot)

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
        if (found != states.end() && found->second.engaged)
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
        else
            out += "; no judged pull (attacked)";

        // [deaths] how its last route search saw the mobs that hit it: refused (a walk round them was searched),
        // priced (the walk may pass their reach), unseen (out of the search's range or not idle then)
        if (found != states.end() && found->second.routeMs && !found->second.attackers.empty())
        {
            SoloBotState const& state = found->second;
            std::string seen;
            for (auto const& [guid, track] : state.attackers)
            {
                auto const mob = state.routeMobs.find(guid);
                seen += (seen.empty() ? "" : ", ") + track.name + " " +
                        (mob == state.routeMobs.end() ? "unseen" : mob->second ? "refused" : "priced");
            }
            out += Acore::StringFormat("; last route search {:.0f} s before ({}{}): {}",
                                       float(getMSTimeDiff(state.routeMs, getMSTime())) / IN_MILLISECONDS,
                                       state.routeComplete ? "complete" : "partial",
                                       state.routeThroughRefused ? ", through a refused reach" : "", seen);
        }
        return out;
    }

    namespace
    {
        // the share of its speed a daze (spell 1604, Unit::AttackerStateUpdate from behind) takes off, by its data
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
        void ChaseTimes(Player* bot, float vm, float leashSec, float melee, float reach, float& until, float& exposure)
        {
            float const leashRadius = sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS);
            float const vb = bot->GetSpeed(bot->IsInWater() ? MOVE_SWIM : MOVE_RUN);
            float const dazed = DazedShare();
            bool const canDaze = melee > 0.0f && vm > vb && vb * (1.0f + dazed) <= vm;
            float const follow = std::max(std::min(vm, canDaze ? vb * (1.0f + dazed) : vb), 0.1f);
            until = std::max(leashSec, leashRadius / follow) + 1.0f;  // + ThreatManager's 1 s, as the flee
            exposure = until;
            if (vm < vb && !canDaze)
                exposure = std::min(until, reach / (vb - vm));
        }
    }

    RunPast RunPastChase(Player* bot, Creature* mob)
    {
        RunPast out;
        if (!bot || !mob || sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS) <= 0.0f || mob->isWorldBoss())
        {
            out.damage = std::numeric_limits<float>::max();
            return out;
        }
        float const melee = MeleeDps(mob, bot);
        float exposure = 0.0f;
        ChaseTimes(bot, mob->GetSpeed(MOVE_RUN), float(mob->GetLeashTimer()), melee, mob->GetMeleeRange(bot),
                   out.seconds, exposure);
        float const casts = ChaseSpellDamage(mob, out.seconds);
        out.damage = melee * BehindFactor(bot) * exposure + (casts > 0.0f ? casts : SpellDpsOf(mob) * out.seconds);
        return out;
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
        float const melee = hit / swing;
        // Creature::GetLeashTimer of a mob of this template (the core's own numbers)
        constexpr int32 LEASH_TIMER_OFFSET = 11;
        float const leashSec =
            float(std::max<int32>(LEASH_TIMER_OFFSET, LEASH_TIMER_OFFSET + int32(tmpl->minlevel / 10) - 2));
        float exposure = 0.0f;
        ChaseTimes(bot, tmpl->speed_run * baseMoveSpeed[MOVE_RUN], leashSec, melee, NOMINAL_MELEE_RANGE, out.seconds,
                   exposure);
        out.damage = melee * BehindFactor(bot) * exposure;
        return out;
    }

    bool SurvivesRunPast(Player* bot, float damage, float seconds)
    {
        if (!bot || damage == std::numeric_limits<float>::max())
            return false;
        float const mana = float(bot->GetPower(POWER_MANA));
        return damage < float(bot->GetHealth()) + SurvivalKit::HealPoints(bot, seconds, mana, true, true);
    }

    void CommitRunPast(Player* bot, std::vector<G3D::Vector3> const& points,
                       std::vector<G3D::Vector3> const& searched)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        state.legRunPast = state.pendingRunPast;
        state.legPoints = state.legRunPast.empty() ? std::vector<G3D::Vector3>() : points;
        state.legSearched = state.legRunPast.empty() ? std::vector<G3D::Vector3>() : searched;  // [runpast15]
        state.legNamed.clear();
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

    bool HasRunPastLeg(Player* bot)
    {
        if (!bot)
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        return found != states.end() && !found->second.legRunPast.empty();
    }

    bool RunPastStep(Player* bot, std::vector<G3D::Vector3>& rest, std::string& line)
    {
        rest.clear();
        line.clear();
        if (!bot || !bot->IsAlive() || !bot->IsInCombat() || !Applies(bot) || Disengaging(bot))
            return false;
        std::unordered_map<uint64, float> set;
        std::vector<G3D::Vector3> points;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            if (found == states.end() || found->second.legRunPast.empty() || found->second.legPoints.size() < 2)
                return false;
            set = found->second.legRunPast;
            points = found->second.legPoints;
            // [deaths13] the bot's own judged pull is among its attackers: it fights that pull where it stands, no
            // walk on past the camp with it in tow. b353 Eraelor L9 (Gnarlpine Defender) and Nurlian L7 (Wrathscale
            // Siren): "PULL ... brings 1", then "RUN_PAST keeps walking past" the very mob it pulled (it was priced on
            // the leg), the walk dragged the fight into the next camp - judged 1, came 3 / 2, both died
            if (found->second.engaged)
                for (Unit* attacker : bot->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker->GetGUID() == found->second.engage.target)
                    {
                        LOG_INFO("playerbots", "[deaths13] {} L{} ends its run past: it fights its own pull of {} L{} "
                                 "(the leg's chase set holds {} mobs)",
                                 bot->GetName(), bot->GetLevel(), attacker->GetName(), attacker->GetLevel(),
                                 found->second.legRunPast.size());
                        found->second.legRunPast.clear();
                        found->second.legPoints.clear();
                        found->second.legNamed.clear();
                        return false;
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
            state.legPoints.clear();
            state.legNamed.clear();
            return false;
        }
        // every attacker one the leg priced; their chases still to come survived
        std::vector<Creature*> chasers;
        RunPast left;
        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker || !attacker->IsAlive())
                continue;
            Creature* mob = attacker->ToCreature();
            if (!mob || !set.count(mob->GetGUID().GetRawValue()))
                return false;  // a mob it did not price: the fight as before
            RunPast const chase = RunPastChase(bot, mob);
            left.damage = chase.damage == std::numeric_limits<float>::max() ? chase.damage : left.damage + chase.damage;
            left.seconds = std::max(left.seconds, chase.seconds);
            chasers.push_back(mob);
        }
        if (chasers.empty() || !SurvivesRunPast(bot, left.damage, left.seconds))
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
        float const mana = float(bot->GetPower(POWER_MANA));
        float const hp = float(bot->GetHealth()) + SurvivalKit::HealPoints(bot, left.seconds, mana, true, true);
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        for (Creature* mob : chasers)
            if (state.legNamed.insert(mob->GetGUID().GetRawValue()).second)
                line += Acore::StringFormat("{}{} keeps walking past {} L{} (chase priced {:.0f}, hp {:.0f})",
                                            line.empty() ? "" : "; ", bot->GetName(), mob->GetName(), mob->GetLevel(),
                                            set[mob->GetGUID().GetRawValue()], hp);
        return true;
    }

    void RouteCosts(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& to, std::vector<G3D::Vector4>& refuse,
                    std::vector<Leader::Travel::RoutePenalty>& penalties)
    {
        PMON_SCOPE(costScope, PERF_MON_VALUE, "solo route costs");  // [guild15]
        Player* bot = walker ? walker->ToPlayer() : nullptr;
        if (!bot || bot->IsInCombat() || !bot->IsAlive())
            return;
        // [deaths3] a co-op party's walk keeps the con rule too (b341 Ryarii L1 and Salleas L1: their walk to the boar
        // spawns went through Nightsabers, Webwood Lurkers and Shadow Sprites L5-6, five fights off the objective, a
        // Lurker L6 killed Ryarii): orange / red, elite and danger-zone reaches are refused; the pack it loses alone is
        // the party's judgement (NewRpgCoop::JudgePull), not refused here
        bool const alone = Applies(bot);
        if (!alone && !(NewRpgCoop::InParty(bot->GetGUID().GetCounter()) && !bot->GetMap()->Instanceable() &&
                        GET_PLAYERBOT_AI(bot) && GET_PLAYERBOT_AI(bot)->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)))
            return;
        // the walk's first leg as far as the bot sees: the rest is planned again from there. [deaths] The leg ends up
        // to SightDistance from the bot, so a mob whose aggro reach (Creature::GetAggroRange, at most the core's
        // MAX_AGGRO_RADIUS) covers its end stands up to that much farther: those are seen too
        // [deaths3] ... and a wanderer's reach is its home's with its wander area (b338 Keenonk L11: a Black Slime L20
        // "unseen" by the search, its home in reach of the walk): the search looks that much farther, at most as far
        // as the map shows the bot units (Map::GetVisibilityRange)
        float const wander = std::min(MaxWanderDistance(bot->GetMapId()), bot->GetMap()->GetVisibilityRange());
        float const range = sPlayerbotAIConfig.SightDistance + MAX_AGGRO_RADIUS + wander;
        std::unordered_map<uint64, bool> seen;  // [deaths] mob -> refused (DeathContext)
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);
        float const dps = std::max(BotDps(bot), 1.0f);
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
            auto const chaseCovers = [&](Creature* chaser, float seconds, Creature* c)
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
                return std::hypot(c->GetPositionX() - px, c->GetPositionY() - py) <= assistRadius;
            };
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
                    if (chase.seconds * speed > walkOn)
                    {
                        price = std::numeric_limits<float>::max();
                        hp = float(bot->GetHealth());
                        static std::mutex stopLogLock;
                        static std::unordered_set<uint64> stopLogged;
                        bool fresh = false;
                        {
                            std::lock_guard<std::mutex> guard(stopLogLock);
                            if (stopLogged.size() > 65536)
                                stopLogged.clear();
                            fresh = stopLogged.insert(chaser->GetGUID().GetCounter() * 1000003ull +
                                                      bot->GetGUID().GetCounter()).second;
                        }
                        if (fresh)
                            LOG_INFO("playerbots", "[deaths14] {} L{} no run past {} L{}: the walk ends {:.0f} yd past "
                                     "where it meets the walk, the chase runs {:.0f} s ({:.0f} yd) - a wall",
                                     bot->GetName(), bot->GetLevel(), chaser->GetName(), chaser->GetLevel(),
                                     walkOn, chase.seconds, chase.seconds * speed);
                        return false;
                    }
                }
                with.damage = chase.damage == std::numeric_limits<float>::max() ? chase.damage
                                                                                 : with.damage + chase.damage;
                with.seconds = std::max(with.seconds, chase.seconds);
                added.push_back({key, chase.damage});
                if (assistRadius <= 0.0f || chaser->HasFlagsExtra(CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE) ||
                    chase.damage == std::numeric_limits<float>::max())
                    continue;
                for (Creature* c : around)
                    if (c != chaser && std::find(chasers.begin(), chasers.end(), c) == chasers.end() &&
                        !chasing.count(c->GetGUID().GetRawValue()) && c->CanAssistTo(chaser, bot) &&
                        chaseCovers(chaser, chase.seconds, c))
                        chasers.push_back(c);  // its own chase and calls are counted in turn
            }
            price = with.damage;
            hp = float(bot->GetHealth()) +
                 SurvivalKit::HealPoints(bot, with.seconds, float(bot->GetPower(POWER_MANA)), true, true);
            if (!SurvivesRunPast(bot, with.damage, with.seconds))
                return false;
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
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->IsCivilian() || mob->IsTotem() ||
                mob->GetGUID() == target || !mob->IsHostileTo(bot) || !mob->HasReactState(REACT_AGGRESSIVE) ||
                mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) ||
                mob->GetCharmerOrOwnerGUID())
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
            // the leg ends within sight of the bot: a reach beyond it is priced when the bot gets there
            if (bot->GetExactDist2d(centre.GetPositionX(), centre.GetPositionY()) >
                sPlayerbotAIConfig.SightDistance + radius)
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
            if (why == RouteWall::Why::None &&
                NewRpgBaseAction::InDeathZone(bot, mob->GetPositionX(), mob->GetPositionY()) &&
                !NewRpgBaseAction::SameDangerZone(bot, bot->GetPositionX(), bot->GetPositionY(), mob->GetPositionX(),
                                                  mob->GetPositionY()))
                why = RouteWall::Why::Zone;
            uint32 wallPack = 1;
            // [regress10] user 2026-11-08 "you can just run past": an orange / red or elite mob is no wall by itself -
            // walked past, it and its linked pack chase the bot until the core's leash lets them go; the leg takes
            // that chase (added to the others it takes) while the bot lives through it from its health now with its
            // heal kit, else it stays a wall. Priced as a mob it must never fight: a complete way round is walked
            bool ranPast = false;
            float runPastDamage = 0.0f;
            float runPastHp = 0.0f;
            bool const mayRunPast =
                alone && !NewRpgBaseAction::InDeathZone(bot, mob->GetPositionX(), mob->GetPositionY());
            if ((why == RouteWall::Why::Elite || why == RouteWall::Why::Con) && mayRunPast)
            {
                buildAround();
                PullModel model(bot, mob, around);
                model.Links();
                if (tryRunPast(mob, model, runPastDamage, runPastHp))
                {
                    ranPast = true;
                    why = RouteWall::Why::None;
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
                std::vector<RouteFoe> foes;
                uint32 above = 0;
                for (PullModel::Member const& m : model.pack)
                {
                    foes.push_back({m.mob, MobDps(m.mob, bot), float(m.mob->GetHealth())});  // [dps1] no measurement
                    above += m.mob->GetLevel() > level ? 1 : 0;
                }
                // the judge's kill order: the mob met first, then the most damage per health
                std::sort(foes.begin() + 1, foes.end(), [](RouteFoe const& x, RouteFoe const& y)
                          { return x.dps / std::max(x.hp, 1.0f) > y.dps / std::max(y.hp, 1.0f); });
                if (above >= 2)
                    why = RouteWall::Why::Above;
                else if (LosesAtFull(bot, own, foes, petHealth))
                    why = RouteWall::Why::Loses;
                wallPack = uint32(foes.size());
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
                        if (pull.kind == Verdict::Kind::Win && pull.mobs == 1 && pull.pullSpot)

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
                if (why != RouteWall::Why::None && mayRunPast && tryRunPast(mob, model, runPastDamage, runPastHp))
                {
                    ranPast = true;
                    why = RouteWall::Why::None;
                }
            }
            bool const never = why != RouteWall::Why::None;
            if (never)
                walls.push_back({mob->GetEntry(), mob->GetLevel(), why, wallPack,
                                 bot->GetExactDist2d(centre.GetPositionX(), centre.GetPositionY()), runPastDamage,
                                 runPastHp});
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
            penalty.cost = float(mob->GetHealth()) / dps * speed;
            // a mob it must never fight, when no walk round it is complete: any detour up to the whole trip again is
            // the better walk (b301: Thokhen L3 walked 4 times through the same L9 Nightstalkers on a 1943 yd walk)
            if (never || ranPast)
                penalty.cost = std::max(penalty.cost, (to - from).length() + 2.0f * radius);
            penalties.push_back(penalty);
            seen[mob->GetGUID().GetRawValue()] = never;
        }
        // [deaths] its danger zones (where it died, a pack it refused) are refused whether or not their mobs are in
        // sight now (b338 Keenonk L11 died twice at the same Black Slime L20 and twice among the same Dragonmaw Scouts:
        // "unseen" in the route search before each death)
        NewRpgBaseAction::DangerDiscs(bot, bot->GetPositionX(), bot->GetPositionY(), range, refuse);
        // [runpast11] the packs this leg runs past, one line per change of them
        std::string runPastLine;
        if (!ranPastNames.empty())
        {
            float const kit = float(bot->GetHealth()) +
                              SurvivalKit::HealPoints(bot, runPast.seconds, float(bot->GetPower(POWER_MANA)), true,
                                                      true);
            runPastLine = Acore::StringFormat("run-past priced {:.0f} hp vs hp {:.0f} ({:.0f} s of chase): ",
                                              runPast.damage, kit, runPast.seconds);
            for (size_t i = 0; i < ranPastNames.size(); ++i)
                runPastLine += (i ? ", " : "") + ranPastNames[i];
        }
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        if (!runPastLine.empty() && state.runPastLogged != runPastLine)
            LOG_INFO("playerbots", "[solo] {} L{} {}", bot->GetName(), bot->GetLevel(), runPastLine);
        state.runPastLogged = runPastLine;
        // [runpast11] a search from where the bot stands: its run-past set waits for the leg's commit
        if (bot->GetExactDist(from.x, from.y, from.z) <= bot->GetCombatReach())
            state.pendingRunPast = std::move(chaseOf);
        state.routeMs = getMSTime();
        state.routeMobs = std::move(seen);
        state.routeWalls = std::move(walls);
        state.routeComplete = false;
        state.routeThroughRefused = false;
    }

    std::string ConWalls(Player* bot)
    {
        if (!bot)
            return std::string();
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end())
            return std::string();
        std::string out;
        uint32 n = 0;
        for (RouteWall const& w : found->second.routeWalls)
            if (w.why == RouteWall::Why::Con || w.why == RouteWall::Why::Elite)
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(w.entry);
                if (++n <= 3)  // a log line's length, no decision
                    out += Acore::StringFormat("{}{} L{} ({})", out.empty() ? "" : ", ", tmpl ? tmpl->Name : "?",
                                               w.level, w.why == RouteWall::Why::Elite ? "elite" : "orange / red");
            }
        return out;
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
        return out.empty() ? std::string("no mob wall (a danger zone's disc or the pull spot's walk)") : out;
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
}
