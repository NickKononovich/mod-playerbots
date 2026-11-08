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
#include "ModelIgnoreFlags.h"  // [deaths] the pull spot's sight of its target
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RouteSearch.h"
#include "SpellInfo.h"
#include "SurvivalKit.h"  // [deaths3] its heals now
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
        bool routeComplete = false;
        bool routeThroughRefused = false;
        // [deaths] the engage's pull spot (Verdict::pullSpot) and the reaches its walk goes round
        bool pullSpot = false;
        ObjectGuid pullTarget;
        G3D::Vector3 pull;
        std::vector<G3D::Vector4> pullAvoid;
        // [deaths] the last pull spot search: its target, where the bot stood, what it found (perf: one search per
        // target and spot of the bot, the judge looks every tick)
        ObjectGuid searchTarget;
        G3D::Vector3 searchFrom;
        bool searchFound = false;
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
}

namespace SoloSafety
{
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

        // where the fight stands: at the target for melee, at the bot's reach on its side for a ranged bot
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
        // [deaths] user 2026-11-08: two mobs whose aggro reaches overlap are no pack when the core does not link them
        // (the target's formation, its faction's assistance call, its script's call for help): the fault is a pull
        // spot where both see the bot. The pull is made from a spot outside every other idle mob's reach - in the
        // target's aggro reach for a melee bot (it comes), in its spell reach with sight of it for a ranged one -
        // walked to round those reaches; with no such spot the overlapping mob stays in the pack (the judge refuses,
        // the co-op seek asks for help)
        Creature* targetMob = target->ToCreature();
        // (a melee bot's pull is its body in the target's aggro reach: only a hostile, aggressive target comes)
        if (targetMob && !target->IsInCombat() &&
            (reach > 0.0f || (targetMob->IsHostileTo(bot) && targetMob->HasReactState(REACT_AGGRESSIVE))))
        {
            float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
            ScriptHelp const targetHelp = ScriptHelpOf(targetMob);
            CreatureGroup* const targetFormation = targetMob->GetFormation();
            auto const linked = [&](Creature* mob)
            {
                if (targetFormation && mob->GetFormation() == targetFormation)
                    return true;
                if (assist > 0.0f && mob->GetFaction() == targetMob->GetFaction() &&
                    mob->IsWithinDistInMap(targetMob, assist) && mob->CanAssistTo(targetMob, bot))
                    return true;
                return targetHelp.callRadius > 0.0f && mob->IsWithinDistInMap(targetMob, targetHelp.callRadius) &&
                       mob->CanAssistTo(targetMob, bot);
            };
            std::vector<G3D::Vector4> reaches;  // the idle mobs not linked to the target
            bool covered = false;
            for (ObjectGuid const& guid : GET_PLAYERBOT_AI(bot)->GetAiObjectContext()
                                              ->GetValue<GuidVector>("possible targets no los")->Get())
            {
                Creature* mob = ObjectAccessor::GetCreature(*bot, guid);
                if (!mob || mob == targetMob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                    !mob->HasReactState(REACT_AGGRESSIVE) || linked(mob))
                    continue;
                float radius = mob->GetAggroRange(bot) + bot->GetCombatReach();
                Position centre = mob->GetPosition();
                if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                {
                    centre = mob->GetHomePosition();
                    radius += mob->GetWanderDistance();
                }
                reaches.emplace_back(centre.GetPositionX(), centre.GetPositionY(), centre.GetPositionZ(), radius);
                covered = covered || centre.GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) <= radius;
            }
            G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            bool searched = false;
            if (covered)
            {
                std::lock_guard<std::mutex> guard(lock);
                SoloBotState const& state = states[bot->GetGUID().GetCounter()];
                if (state.searchTarget == target->GetGUID() &&
                    (state.searchFrom - from).length() <= bot->GetCombatReach())
                {
                    searched = true;
                    verdict.pullSpot = state.searchFound;
                    verdict.pull = state.searchSpot;
                    verdict.pullAvoid = state.searchAvoid;
                    if (verdict.pullSpot)
                        spot.Relocate(verdict.pull.x, verdict.pull.y, verdict.pull.z);
                }
            }
            if (covered && !searched)
            {
                auto const outside = [&reaches](float x, float y)
                {
                    for (G3D::Vector4 const& r : reaches)
                        if ((r.x - x) * (r.x - x) + (r.y - y) * (r.y - y) <= r.w * r.w)
                            return false;
                    return true;
                };
                float const pullReach =
                    (reach > 0.0f ? reach : targetMob->GetAggroRange(bot)) - bot->GetCombatReach();
                std::vector<std::pair<float, G3D::Vector3>> spots;
                if (pullReach > 0.0f)
                {
                    // one spot per stretch of the ring as wide as the bot's reach
                    uint32 const arcs = std::max<uint32>(
                        8, uint32(std::ceil(2.0f * float(M_PI) * pullReach / std::max(bot->GetCombatReach(), 1.0f))));
                    float const height = bot->GetCollisionHeight();
                    for (uint32 i = 0; i < arcs; ++i)
                    {
                        float const angle = 2.0f * float(M_PI) * float(i) / float(arcs);
                        G3D::Vector3 at(target->GetPositionX() + std::cos(angle) * pullReach,
                                        target->GetPositionY() + std::sin(angle) * pullReach, target->GetPositionZ());
                        if (!outside(at.x, at.y))
                            continue;
                        bot->UpdateAllowedPositionZ(at.x, at.y, at.z);
                        if (!bot->GetMap()->isInLineOfSight(at.x, at.y, at.z + height, target->GetPositionX(),
                                                            target->GetPositionY(), target->GetPositionZ() + height,
                                                            bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                            VMAP::ModelIgnoreFlags::Nothing))
                            continue;
                        spots.emplace_back(bot->GetExactDist2d(at.x, at.y), at);
                    }
                }
                std::sort(spots.begin(), spots.end(),
                          [](auto const& a, auto const& b) { return a.first < b.first; });
                // the walk there round every such reach (a reach the bot stands in lies behind it); the polygons'
                // edges are tested against the discs as the solo route search tests them (penalties at no cost)
                G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                std::vector<G3D::Vector4> avoid;
                std::vector<Leader::Travel::RoutePenalty> edges;
                for (G3D::Vector4 const& r : reaches)
                    if ((r.x - here.x) * (r.x - here.x) + (r.y - here.y) * (r.y - here.y) > r.w * r.w)
                    {
                        avoid.push_back(r);
                        Leader::Travel::RoutePenalty edge;
                        edge.x = r.x;
                        edge.y = r.y;
                        edge.z = r.z;
                        edge.r = r.w;
                        edges.push_back(edge);
                    }
                constexpr size_t PULL_SPOT_ROUTE_TRIES = 3;  // search cost bound (the nearest spots), not a rule
                for (size_t i = 0; i < spots.size() && i < PULL_SPOT_ROUTE_TRIES; ++i)
                {
                    Movement::PointsArray corners;
                    bool complete = false;
                    if (!Leader::Travel::LongRoute(bot, here, spots[i].second, false, corners, complete, nullptr,
                                                   &avoid, 0.0f, nullptr, &edges) ||
                        !complete || corners.empty())
                        continue;
                    verdict.pullSpot = true;
                    verdict.pull = corners.back();
                    verdict.pullAvoid = avoid;
                    spot.Relocate(verdict.pull.x, verdict.pull.y, verdict.pull.z);
                    break;
                }
                std::lock_guard<std::mutex> guard(lock);
                SoloBotState& state = states[bot->GetGUID().GetCounter()];
                state.searchTarget = target->GetGUID();
                state.searchFrom = from;
                state.searchFound = verdict.pullSpot;
                state.searchSpot = verdict.pull;
                state.searchAvoid = verdict.pullAvoid;
            }
        }

        std::vector<Unit*> pack;
        PullPlanner::SoloPack(bot, target, spot, pack);
        if (pack.empty())
            pack.push_back(target);
        for (Unit* attacker : bot->getAttackers())
            if (attacker && attacker->IsAlive() && std::find(pack.begin(), pack.end(), attacker) == pack.end())
                pack.push_back(attacker);
        // [guild13] b300: "judged 1 mob, came 2-3" in most lost fights (Princess and her Porcine Entourage, Prowlers
        // wandering in): what the core brings on top, until nothing more joins - a pack mob's formation
        // (CreatureGroup), an idle mob whose aggro reach (with its wander area) covers the fight spot, and one its
        // faction's assistance call reaches (CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS, Creature::CanAssistTo)
        {
            float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
            std::vector<Creature*> idle;
            for (ObjectGuid const& guid : GET_PLAYERBOT_AI(bot)->GetAiObjectContext()
                                              ->GetValue<GuidVector>("possible targets no los")->Get())
                if (Creature* mob = ObjectAccessor::GetCreature(*bot, guid))
                    if (mob->IsAlive() && !mob->IsInCombat() && mob->IsHostileTo(bot) &&
                        mob->HasReactState(REACT_AGGRESSIVE))
                        idle.push_back(mob);
            auto const inPack = [&pack](Unit* unit) { return std::find(pack.begin(), pack.end(), unit) != pack.end(); };
            for (size_t i = 0; i < pack.size(); ++i)
            {
                Creature* member = pack[i]->ToCreature();
                if (!member)
                    continue;
                if (CreatureGroup* formation = member->GetFormation())
                    for (auto const& [mate, info] : formation->GetMembers())
                        if (mate && mate->IsAlive() && !inPack(mate))
                            pack.push_back(mate);
                // [deaths3] the core's assistance (Creature::CallAssistance): the target calls once as it engages,
                // where it stands; its assistants do not call then (SetNoCallAssistance), and from then on every
                // engaged mob calls again each CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD from where it fights - the
                // fight spot. Before, the call chained from every assistant's home through the whole camp (b341
                // Thyllyn L12: "6 mobs (4 above)" for one Greymist Coastrunner, every quest there postponed)
                for (Creature* mob : idle)
                {
                    if (inPack(mob) || assist <= 0.0f || mob->GetFaction() != member->GetFaction() ||
                        !mob->CanAssistTo(member, bot))
                        continue;
                    bool const atEngage = member == targetMob && mob->IsWithinDistInMap(member, assist);
                    bool const atFight =
                        mob->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) <= assist;
                    if (atEngage || atFight)
                        pack.push_back(mob);
                }
                // [guild15] the help its script calls: all in the call's radius, the nearest one it flees to
                ScriptHelp const help = ScriptHelpOf(member);
                if (help.callRadius > 0.0f)
                    for (Creature* mob : idle)
                        if (!inPack(mob) && mob->IsWithinDistInMap(member, help.callRadius) &&
                            mob->CanAssistTo(member, bot))
                            pack.push_back(mob);
                if (help.flees)
                {
                    float const flee = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
                    Creature* nearest = nullptr;
                    for (Creature* mob : idle)
                        if (!inPack(mob) && mob->IsWithinDistInMap(member, flee) && mob->CanAssistTo(member, bot) &&
                            (!nearest || member->GetDistance(mob) < member->GetDistance(nearest)))
                            nearest = mob;
                    if (nearest)
                        pack.push_back(nearest);
                }
            }
            for (Creature* mob : idle)
            {
                if (inPack(mob))
                    continue;
                float reach = mob->GetAggroRange(bot) + bot->GetCombatReach();
                Position centre = mob->GetPosition();
                if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                {
                    centre = mob->GetHomePosition();
                    reach += mob->GetWanderDistance();
                }
                if (centre.GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) <= reach)
                    pack.push_back(mob);
            }
        }

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
            verdict.ehpNow, verdict.ehpFull);
        verdict.why = verdict.why.empty() ? numbers : verdict.why + ": " + numbers;

        bool log = false;
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
        state.pullSpot = verdict.pullSpot;
        state.pullTarget = target->GetGUID();
        state.pull = verdict.pull;
        state.pullAvoid = verdict.pullAvoid;
        if (state.engaged && state.engage.target == target->GetGUID())
            return;
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
            // pack its pull judge refused)
            bool never = IsElite(mob) || Acore::XP::GetColorCode(level, mob->GetLevel()) <= XP_ORANGE ||
                         NewRpgBaseAction::InDangerZone(bot, mob->GetPositionX(), mob->GetPositionY());
            // [guild15] ... and a mob whose fight alone the bot loses now: its damage (the model, every bot's and this
            // bot's measurement) over the time the bot's damage needs for its health, against the bot's health (b315:
            // 12 of 14 deaths were bots attacked on their way, most through Gnarlpine / Wendigo / murloc camps)
            // [guild15] ... with the mobs the core brings when it is attacked: its formation and the idle mobs of
            // its faction its assistance call reaches (CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS, Creature::CanAssistTo;
            // an engaged mob calls again every CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD, so the call spreads one more
            // hop per period of the fight), killed one by one, the most damage per health first (b321: 7 of 9 deaths
            // were bots attacked on their way through camps, 2-4 mobs came; b322 Cassand L8 by 3 Prowlers L9-10)
            if (!never && alone)
            {
                std::vector<Creature*> pack{mob};
                auto const inPack = [&pack](Creature* c) { return std::find(pack.begin(), pack.end(), c) != pack.end(); };
                if (CreatureGroup* formation = mob->GetFormation())
                    for (auto const& [mate, info] : formation->GetMembers())
                        if (mate && mate->IsAlive() && !mate->IsInCombat() && !inPack(mate))
                            pack.push_back(mate);
                float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
                float const period =
                    float(sWorld->getIntConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD)) / float(IN_MILLISECONDS);
                auto const packDamage = [&](float& seconds)
                {
                    struct Foe
                    {
                        float dps, hp;
                    };
                    std::vector<Foe> foes;
                    for (Creature* c : pack)
                        foes.push_back({MobDps(c, bot), float(c->GetHealth())});  // [dps1] no measurement
                    std::sort(foes.begin(), foes.end(), [](Foe const& x, Foe const& y)
                              { return x.dps / std::max(x.hp, 1.0f) > y.dps / std::max(y.hp, 1.0f); });
                    float incoming = 0.0f;
                    for (Foe const& f : foes)
                        incoming += f.dps;
                    float damage = 0.0f;
                    seconds = 0.0f;
                    for (Foe const& f : foes)
                    {
                        float const t = f.hp / dps;
                        damage += incoming * t;
                        seconds += t;
                        incoming -= f.dps;
                    }
                    return damage;
                };
                size_t hopsDone = 0;
                size_t frontier = 0;  // pack members from here on have not called yet
                float seconds = 0.0f;
                float damage = packDamage(seconds);
                // the first call at the pull, then one more hop per period while the fight lasts
                while (assist > 0.0f && frontier < pack.size() &&
                       (hopsDone == 0 || (period > 0.0f && float(hopsDone) * period < seconds)))
                {
                    size_t const end = pack.size();
                    for (size_t i = frontier; i < end; ++i)
                        for (Unit* other : units)
                        {
                            Creature* c = other->ToCreature();
                            if (!c || inPack(c) || !c->IsAlive() || c->IsInCombat() ||
                                !c->HasReactState(REACT_AGGRESSIVE) || c->GetFaction() != pack[i]->GetFaction() ||
                                !c->IsWithinDistInMap(pack[i], assist) || !c->CanAssistTo(pack[i], bot))
                                continue;
                            pack.push_back(c);
                        }
                    frontier = end;
                    ++hopsDone;
                    damage = packDamage(seconds);
                }
                never = damage >= float(bot->GetHealth());
            }
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
            if (never)
                penalty.cost = std::max(penalty.cost, (to - from).length() + 2.0f * radius);
            penalties.push_back(penalty);
            seen[mob->GetGUID().GetRawValue()] = never;
        }
        // [deaths] its danger zones (where it died, a pack it refused) are refused whether or not their mobs are in
        // sight now (b338 Keenonk L11 died twice at the same Black Slime L20 and twice among the same Dragonmaw Scouts:
        // "unseen" in the route search before each death)
        NewRpgBaseAction::DangerDiscs(bot, bot->GetPositionX(), bot->GetPositionY(), range, refuse);
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        state.routeMs = getMSTime();
        state.routeMobs = std::move(seen);
        state.routeComplete = false;
        state.routeThroughRefused = false;
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
