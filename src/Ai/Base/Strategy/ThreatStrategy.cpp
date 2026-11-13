/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ThreatStrategy.h"
#include "AttackerCountValues.h"
#include "AttackersValue.h"  // [kara4] IsCharmedAlly
#include "Creature.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "LeaderCommand.h"  // [rot2]
#include "LeaderCommandState.h"  // [rot3]
#include "LeaderFightChecks.h"   // [lead1100] FieldWalkerEnters
#include "LeaderOrders.h"        // [lead282] the leader's Attack
#include "Log.h"
#include "Map.h"
#include "PartyStrengthValue.h"
#include "Playerbots.h"
#include "RtiValue.h"  // [rot1] CrowdControlWouldBreak
#include "Spell.h"
#include "SpellAuraEffects.h"  // [lead601]
#include "SpellAuras.h"        // [rot47]
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "ThreatValues.h"
#include "Timer.h"
#include "EstimatedLifetimeValue.h"  // [aoe1] ExpectedFightLifetime
#include "HealthTriggers.h"         // [aoe1] RestState::OwnManaLine
#include "StringFormat.h"

#include <algorithm>
#include <functional>
#include <numeric>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

namespace
{
    // Share of the tank's threat (percent) at which a dps stops a threat-generating action. [aoe1] The area spells
    // take the game's line per mob instead (AreaHolds).
    constexpr uint8 SINGLE_THREAT_LIMIT = 80;
    // A melee bot pulls aggro only at 110 % of the victim's threat. 80 % held a Ret idle on 14 % of the Gruul
    // ticks (DPS audit 2026-10-24); 95 % keeps a 15-point margin under the melee pull point. Ranged keep 80 %.
    constexpr uint8 MELEE_SINGLE_THREAT_LIMIT = 95;
    // A tank soft for the content or outgeared by its dps (party strength): keep a wider margin, and hold AoE
    // until the tank has a solid lead on every mob of the pack.
    constexpr uint8 CAREFUL_SINGLE_THREAT_LIMIT = 70;
    constexpr uint8 CAREFUL_MELEE_SINGLE_THREAT_LIMIT = 85;

    // [ranged_audit_b30] An interrupt or silence (Counterspell, Wind Shear, Kick, Silence, Silencing Shot), or a
    // crowd control that deals no damage (Fear, Polymorph, Hex, Hammer of Justice, Shackle Undead).
    // [rot1] Split: an interrupt / silence or a damage-free stun (Hammer of Justice, the stun interrupts) is still
    // never held; a damage-free fear or confuse (Polymorph, Fear, Hex, Blind, Scare Beast) takes the crowd-control
    // threat gate (CrowdControlHolds) instead of skipping every threat rule.
    enum class StopperKind
    {
        None,
        Stopper,  // interrupt, silence, damage-free stun
        Control   // damage-free fear / confuse
    };

    StopperKind ClassifyStopper(SpellInfo const* info)
    {
        if (!info)
            return StopperKind::None;

        bool stun = false;
        bool control = false;
        bool damage = false;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            // Silencing Shot carries weapon damage as well: the stop of the cast decides
            if (effect.Effect == SPELL_EFFECT_INTERRUPT_CAST)
                return StopperKind::Stopper;

            if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE || effect.Effect == SPELL_EFFECT_WEAPON_DAMAGE ||
                effect.Effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE ||
                effect.Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG ||
                effect.Effect == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL)
                damage = true;

            if (effect.Effect != SPELL_EFFECT_APPLY_AURA)
                continue;

            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_MOD_SILENCE:
                case SPELL_AURA_MOD_PACIFY_SILENCE:
                    return StopperKind::Stopper;
                case SPELL_AURA_MOD_STUN:
                    stun = true;
                    break;
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_CONFUSE:
                    control = true;
                    break;
                case SPELL_AURA_PERIODIC_DAMAGE:
                    damage = true;
                    break;
                default:
                    break;
            }
        }

        // a stun or fear that also hits (Shadowfury) is a damage spell and keeps the threat rule
        if (damage)
            return StopperKind::None;
        if (stun)
            return StopperKind::Stopper;
        return control ? StopperKind::Control : StopperKind::None;
    }

    // [rot1] Crowd control that the group's own damage never breaks: Cyclone makes the mob immune, Banish too.
    bool IsDamageProofCrowdControl(std::string const& spell) { return spell == "cyclone" || spell == "banish"; }

    // [rot1] The threat the bot commits before it decides again: a cast runs to its end (a channel to its last tick),
    // an instant holds the global cooldown, and the bot checks threat only at the next decision. NEXT_CAST_SECONDS
    // (2.5 s) was one size for all: a 3 s Starfire and a 1.5 s Moonfire read the same projection. Floor 1 s (the
    // shortest global cooldown); no spell (melee, wand) keeps the 2.5 s.
    constexpr uint32 COMMIT_FLOOR_MS = 1000;

    float CommitSeconds(Player* bot, SpellInfo const* info)
    {
        if (!info)
            return NextCastThreatValue::NEXT_CAST_SECONDS;

        uint32 ms = info->IsChanneled() ? static_cast<uint32>(std::max(info->GetDuration(), 0))
                                        : info->CalcCastTime(bot);
        ms = std::max({ ms, info->StartRecoveryTime, COMMIT_FLOOR_MS });
        return static_cast<float>(ms) / IN_MILLISECONDS;
    }

    // [rot1] Where an area spell lands and how far it reaches. A spell with no range of its own (Starfall, Blood Boil,
    // Arcane Explosion, Dragon's Breath, Typhoon) is centred on the caster; any other on its target (Blizzard,
    // Flamestrike, Hurricane, Death and Decay, Pestilence, Howling Blast). The radius is the largest effect radius;
    // Starfall's stars come from a triggered spell, the 30 yd of its tooltip. Nothing known: the configured AoeRadius.
    constexpr float STARFALL_RADIUS = 30.0f;
    // [lead1301] Seed of Corruption's detonation, first rank (core spell_warl_seed_of_corruption_dummy::Detonate)
    constexpr uint32 SPELL_SEED_OF_CORRUPTION_DAMAGE_R1 = 27285;

    // [lead1301] b252 rfc-h 5D-12 563-571 s: the seed passed the cast gate with no spared mob in the fallback
    // AoeRadius of its target; it burst seconds later on a Searing Blade Enforcer the leader spared, which then killed
    // the warlock. The seed is an area on its target until it bursts (its aura's duration): its radius is the
    // detonation's, and the spared mobs that come to it count as with every lasting area
    bool IsSeed(std::string const& spell) { return spell == "seed of corruption"; }

    float AreaRadius(Player* bot, SpellInfo const* info, std::string const& spell)
    {
        if (spell == "starfall")
            return STARFALL_RADIUS;
        if (IsSeed(spell) && info)
            if (SpellInfo const* burst = sSpellMgr->GetSpellInfo(
                    sSpellMgr->GetSpellWithRank(SPELL_SEED_OF_CORRUPTION_DAMAGE_R1, info->GetRank())))
                info = burst;

        float radius = 0.0f;
        if (info)
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect())
                {
                    radius = std::max(radius, effect.CalcRadius(bot));
                    // [lead472] a self aura's periodic area (Bladestorm's Whirlwind): the triggered spell's radius
                    if (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                        effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && effect.TriggerSpell)
                        if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                            for (SpellEffectInfo const& hit : triggered->GetEffects())
                                if (hit.IsEffect())
                                    radius = std::max(radius, hit.CalcRadius(bot));
                }

        return radius > 0.0f ? radius : sPlayerbotAIConfig.AoeRadius;
    }

    // [b35_lock] A wand (Shoot 5019), a bow / gun / crossbow Shoot (3018) or Throw (2764) repeats by itself once
    // started, and "shoot" is not even evaluated while it repeats (SpellCastUsefulValue): the hold that zeroes the
    // spells has to stop it. A hunter's Auto Shot (75) is not one of these.
    constexpr uint32 SPELL_WAND_SHOOT = 5019;
    constexpr uint32 SPELL_WEAPON_SHOOT = 3018;
    constexpr uint32 SPELL_THROW = 2764;

    void StopRepeatingShotOnHold(Player* bot, Unit* target)
    {
        if (!target || bot->getClass() == CLASS_HUNTER)
            return;

        Spell* shot = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        if (!shot || !shot->m_spellInfo || shot->m_targets.GetUnitTargetGUID() != target->GetGUID())
            return;

        uint32 const id = shot->m_spellInfo->Id;
        if (id == SPELL_WAND_SHOOT || id == SPELL_WEAPON_SHOOT || id == SPELL_THROW)
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
    }

    // [rot4] A heal or any friendly spell: never held for threat, never stopped by the leader's Stop (slabs-h: "Rtdruaa
    // holds nourish ... limit 30 %", "holds regrowth on party: the leader's Stop"; every heal action is Aoe-typed,
    // CastHealingSpellAction::getThreatType). Classified by the spell itself, for every class: a heal effect (direct,
    // percent, periodic, absorb), a positive spell that deals no damage, or a cast on a friendly unit.
    // [lead431] a spell whose effects trigger a damage spell (two deep, as LeaderMaxAreaRadius reads Starfall's stars)
    // [lead620] b218: lead601 logged 0 cancels in 8 runs while 5C's Starfall hit spared mobs under the leader's Stop
    // (cast ~886.4 s, Stop 888.6 s, "threat on a mob it was not told to hit" on a spared Searing Blade Warlock 889.2 s;
    // cast 983 s, the same at 985.1 s): neither lasting area passed this test. Spell.dbc: Starfall 53201's periodic
    // trigger is 53198, a SPELL_EFFECT_DUMMY with no TriggerSpell whose value (53194 + 1 die) is the star 53195 that
    // spell_dru_starfall_dummy casts (caster->CastSpell(hit, GetEffectValue())); Bladestorm 46924 triggers 50622
    // Whirlwind, SPELL_EFFECT_NORMALIZED_WEAPON_DMG (121) + a trigger of 44949 (121 too). Every weapon damage effect
    // counts, and a dummy whose value names a spell of the same name (the script's relay) is followed as a trigger
    bool IsDamageEffect(SpellEffectInfo const& hit)
    {
        switch (hit.Effect)
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
            case SPELL_EFFECT_HEALTH_LEECH:
                return true;
            case SPELL_EFFECT_APPLY_AURA:
                return hit.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                       hit.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH;
            default:
                return false;
        }
    }

    SpellInfo const* TriggeredBy(SpellInfo const* info, SpellEffectInfo const& effect)
    {
        if (effect.TriggerSpell)
            return sSpellMgr->GetSpellInfo(effect.TriggerSpell);
        if (effect.Effect != SPELL_EFFECT_DUMMY)
            return nullptr;
        int32 const value = effect.CalcValue();
        SpellInfo const* relayed = value > 0 ? sSpellMgr->GetSpellInfo(uint32(value)) : nullptr;
        if (!relayed || relayed == info || relayed->SpellFamilyName != info->SpellFamilyName ||
            !relayed->SpellName[0] || !info->SpellName[0] || strcmp(relayed->SpellName[0], info->SpellName[0]) != 0)
            return nullptr;
        return relayed;
    }

    bool TriggersDamage(SpellInfo const* info, uint8 depth)
    {
        if (!info || !depth)
            return false;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (!effect.IsEffect())
                continue;
            SpellInfo const* triggered = TriggeredBy(info, effect);
            if (!triggered)
                continue;
            for (SpellEffectInfo const& hit : triggered->GetEffects())
                if (IsDamageEffect(hit))
                    return true;
            if (TriggersDamage(triggered, depth - 1))
                return true;
        }
        return false;
    }

    // [lead1365] a channelled spell that deals damage: a damage effect, a persistent area aura of periodic damage
    // (Blizzard, Rain of Fire, Hurricane: SPELL_EFFECT_PERSISTENT_AREA_AURA with SPELL_AURA_PERIODIC_DAMAGE) or an
    // effect whose triggered spell deals it (Volley's periodic trigger), spell data
    bool ChannelDealsDamage(SpellInfo const* info)
    {
        if (!info)
            return false;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (!effect.IsEffect())
                continue;
            if (IsDamageEffect(effect))
                return true;
            if (effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA &&
                (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                 effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH))
                return true;
        }
        return TriggersDamage(info, 2);
    }

    bool IsFriendlySpell(Player* bot, SpellInfo const* info, Unit* target)
    {
        if (target && target != bot && bot->IsFriendlyTo(target))
            return true;

        if (!info)
            return false;

        // [lead431] b202 rfc-h 5C 304.9-311.8 s: the leader's burn let Rtdruab's Starfall go, its stars (30 yd round
        // the druid) took a Ragefire Shaman and Trogg off the ledge 18 yd below (unpulled, the leader's spared mobs):
        // 10 of the run's 17 aggro losses. Starfall is a positive self aura with no damage effect of its own, so it
        // passed here as a friendly spell before the AoE rule read it. Its stars are damage
        if (TriggersDamage(info, 2))
            return false;

        bool damage = false;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_HEAL:
                case SPELL_EFFECT_HEAL_PCT:
                case SPELL_EFFECT_HEAL_MAX_HEALTH:
                case SPELL_EFFECT_HEAL_MECHANICAL:
                    return true;
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_HEALTH_LEECH:
                    damage = true;
                    break;
                default:
                    break;
            }

            if (effect.Effect != SPELL_EFFECT_APPLY_AURA && effect.Effect != SPELL_EFFECT_APPLY_AREA_AURA_PARTY &&
                effect.Effect != SPELL_EFFECT_APPLY_AREA_AURA_RAID &&
                effect.Effect != SPELL_EFFECT_PERSISTENT_AREA_AURA)
                continue;

            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_PERIODIC_HEAL:
                case SPELL_AURA_OBS_MOD_HEALTH:
                case SPELL_AURA_SCHOOL_ABSORB:
                    return true;
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_LEECH:
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                    damage = true;
                    break;
                default:
                    break;
            }
        }

        return !damage && info->IsPositive();
    }

    // [lead247] a hostile spell that hits an area or chains (Divine Storm, Whirlwind, Cleave, Consecration) is area
    // damage by its own spell data
    // [lead472] and so is a self aura whose periodic trigger is one (Bladestorm's Whirlwind every second): b207 rfc-h
    // 5G the arms warrior's Bladestorm Whirlwind did 46-62 k a pull in the pulls of its 5 aggro losses, unread by
    // any area rule (its own effects hit only the warrior)
    bool HitsSeveral(SpellInfo const* info, uint8 depth = 2)
    {
        if (!info || !depth)
            return false;
        if (info->IsAffectingArea())
            return true;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = info->Effects[i];
            if (!effect.IsEffect())
                continue;
            if (effect.ChainTarget > 1)
                return true;
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL &&
                effect.TriggerSpell && HitsSeveral(sSpellMgr->GetSpellInfo(effect.TriggerSpell), depth - 1))
                return true;
        }
        return false;
    }
}

float ThreatMultiplier::GetValue(Action* action)
{
    if (AI_VALUE(bool, "neglect threat"))
    {
        return 1.0f;
    }

    if (!action)
        return 1.0f;

    // [rot1] The crowd control actions (Cyclone, Entangling Roots, Hibernate, Polymorph, Fear, Banish on the cc mark)
    // are ActionThreatType::None and skipped every rule below: Cyclone on a mob the tank held (strat-h pull 3, 6 casts,
    // 3 aggro losses), Roots from 30.8 yd on a fresh Plague Ghoul (pull 6, a 15 s loss). They get their own gate.
    CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action);
    bool const ccAction = dynamic_cast<CastCrowdControlSpellAction*>(action) != nullptr;
    if (!ccAction && action->getThreatType() == Action::ActionThreatType::None)
        return 1.0f;

    if (!AI_VALUE(bool, "group"))
        return 1.0f;

    // Holding threat is the tank's job: compared with an off-tank's lower threat it would read far over 100.
    if (PlayerbotAI::IsTank(bot))
        return 1.0f;

    bool const careful = AI_VALUE(PartyStrength, "party strength").CarefulThreat();
    SpellInfo const* spellInfo = nullptr;

    // [ranged_audit_b30] A kick or a crowd control is never held for threat: it stops the cast that kills the group,
    // and its own threat is small. Build 30 logged the hold on counterspell 84, fear 82, silencing shot 78,
    // kick 59 and wind shear 57 times (one line per bot per 10 s); brs-h: a Mana Burn killed the elemental shaman.
    // [rot1] Still so for an interrupt, a silence and a damage-free stun. A damage-free fear / confuse takes the
    // crowd-control gate.
    if (spellAction)
    {
        uint32 const spellId = AI_VALUE2(uint32, "spell id", spellAction->getSpell());
        spellInfo = sSpellMgr->GetSpellInfo(spellId);
        // [rot4] a heal or a friendly spell is never held for threat (healing threat in a 5-man is the tank's job)
        if (!ccAction && IsFriendlySpell(bot, spellInfo, action->GetTarget()))
            return 1.0f;

        StopperKind const kind = ccAction ? StopperKind::Control : ClassifyStopper(spellInfo);
        if (kind == StopperKind::Stopper)
            return 1.0f;

        if (kind == StopperKind::Control)
            return CrowdControlHolds(spellAction, action->GetTarget()) ? 0.0f : 1.0f;

        // [b31_shared] The Art of War makes Exorcism instant for 15 s: a held proc is lost. 199 of 203 L80 Exorcism
        // refusals were this multiplier.
        if (spellAction->getSpell() == "exorcism" && botAI->HasAura("the art of war", bot))
            return 1.0f;
    }

    if (action->getThreatType() == Action::ActionThreatType::Aoe)
    {
        if (spellAction)
        {
            if (AreaHolds(spellAction, spellInfo))
                return 0.0f;
        }
        else
        {
            // [aoe1] an area action without spell data: the game's melee line on the area's highest share
            uint8 threat = AI_VALUE2(uint8, "threat", "aoe");
            if (threat >= uint8(ThreatLead::MELEE_PULL_OVER * 100.0f))
                return 0.0f;
        }
    }

    // A mob already hitting this bot is not held off by its silence: the bot fights it until the tank takes it.
    // Stockade 2026-10-24, a rogue under three mobs the tank had no threat on read 100 % and stood 25 s idle.
    Unit* target = AI_VALUE(Unit*, "current target");
    // [rot46] a cast on another mob - a multi-DoT "on attacker" (balance Moonfire / Insect Swarm), a spell at a group
    // member's attacker - is judged on that mob: the gate read the current target only, so the DoTs went onto loose
    // casters the tank had no threat on (b180 5C: the balance druid took 10 of the bear's 17 lost mobs)
    if (Unit* actionTarget = action->GetTarget(); actionTarget && actionTarget != target && actionTarget != bot &&
                                                  !actionTarget->GetGUID().IsPlayer() && bot->IsHostileTo(actionTarget))
        target = actionTarget;
    if (target && target->GetVictim() == bot)
        return 1.0f;

    // [lead282] b193 / b195 rfc-h (16 runs): 590 s of damage dealers' fight time held here on the leader's own Attack
    // target, 270 s of it under the leader's Normal / Burn. The leader's headroom rule (the game's overtake after the
    // next GCD, lead243) is the one threat verdict on that target: its Stop holds the casts, nothing else does
    if (target && Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        if (Leader::Events::Get(bot->GetGUID(), events) && events.attack == Leader::AttackEvent::Attack &&
            events.intensity != Leader::Intensity::Stop &&
            (events.target == target->GetGUID() ||
             // [lead520] ... and on every mob the leader named for it (its kill order: the mobs it has headroom
             // on, its multi-DoT / cleave targets)
             std::find(events.killOrder.begin(), events.killOrder.end(), target->GetGUID()) !=
                 events.killOrder.end()))
            return 1.0f;
    }

    // [flee_stop] A mob running at low health is burst before it brings the next pack: its threat table no longer
    // matters (it hits nobody). strat-h build 29: "[threat-hold] Rtmagae holds frostbolt on Crimson Initiate 10420:
    // 125 %" while it ran off and survived.
    if (target && FledMobs::IsRunning(target))
        return 1.0f;

    if (target && HoldsForFreshAdd(target))
    {
        StopRepeatingShotOnHold(bot, target);  // [b35_lock]
        return 0.0f;
    }

    // [threat-hold] User 2026-10-25: «Дать танку время забрать аггро — но боты не считают аггро. Пусть считают».
    // The bot reads the mob's threat table the way a player reads Omen: it keeps casting while its threat after the
    // next cast ("next cast threat") stays under HOLD_SHARE of its pull-over point (110 % of the holder's threat in
    // melee range of the mob, 130 % out of it), and stops only close to it. The flat 80 % (ranged) of the old rule
    // held a ranged dps idle with 50 points of room left before the mob would turn.
    // [rot1] Soft throttle: "the next cast" is this action's own commitment (CommitSeconds), not a flat 2.5 s. Near
    // the limit a long cast (Starfire 3 s, Fireball 3 s, a channel) stops first while the instants and short casts
    // (Moonfire, Fire Blast, Living Bomb, Wrath) still fit: the bot hits softer before it stops dead, and the threat it
    // reaches at its next decision stays the same projection the old rule used.
    if (target && target->IsAlive() && !target->GetGUID().IsPlayer())
    {
        float const holder = ThreatLead::HolderThreat(bot, target);
        if (holder > 0.0f)
        {
            float const commit = spellAction ? CommitSeconds(bot, spellInfo) : NextCastThreatValue::NEXT_CAST_SECONDS;
            float const next =
                AI_VALUE(float, "next cast threat") * commit / NextCastThreatValue::NEXT_CAST_SECONDS;
            float const projected = (target->GetThreatMgr().GetThreat(bot) + next) / holder;
            float const limit = ThreatLead::PullOverPoint(bot, target) *
                                (careful ? ThreatLead::CAREFUL_HOLD_SHARE : ThreatLead::HOLD_SHARE);
            if (projected < limit)
                return 1.0f;

            uint32 const now = getMSTime();
            if (!_loggedMs || getMSTimeDiff(_loggedMs, now) >= HOLD_LOG_MS)
            {
                _loggedMs = now;
                LOG_INFO("playerbots", "[threat-hold] {} holds {} on {} {}: {:.0f} % of the holder's threat after the "
                         "next cast, limit {:.0f} % [rot1] commit {:.1f} s", bot->GetName(), action->getName(),
                         target->GetName(), target->GetEntry(), projected * 100.0f, limit * 100.0f, commit);
            }

            StopRepeatingShotOnHold(bot, target);  // [b35_lock]
            return 0.0f;
        }
    }

    uint8 singleLimit = careful ? CAREFUL_SINGLE_THREAT_LIMIT : SINGLE_THREAT_LIMIT;
    if (PlayerbotAI::IsMelee(bot))
        singleLimit = careful ? CAREFUL_MELEE_SINGLE_THREAT_LIMIT : MELEE_SINGLE_THREAT_LIMIT;

    uint8 threat = AI_VALUE2(uint8, "threat", "current target");
    if (threat >= singleLimit)
    {
        StopRepeatingShotOnHold(bot, target);  // [b35_lock]
        return 0.0f;
    }

    return 1.0f;
}

bool ThreatMultiplier::HoldsForFreshAdd(Unit* target)
{
    if (!PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot) || !target->IsAlive() || !target->IsInCombat() ||
        target->GetGUID().IsPlayer() || target->IsControlledByPlayer())
        return false;

    // already on a tank: nothing to wait for
    Unit* victim = target->GetVictim();
    Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
    if (victimPlayer && PlayerbotAI::IsTank(victimPlayer))
        return false;

    // [b44_hos] on a friendly NPC that keeps it whatever threat the bot builds (Brann's Tribunal taunt): the tank's
    // pickup changes nothing for this bot
    if (ThreatLead::NpcHoldsRegardless(bot, target))
        return false;

    // Misdirection hands this bot's threat to the tank
    if (botAI->HasAura("misdirection", bot))
        return false;

    uint32 const now = getMSTime();
    auto seen = _freshSeen.find(target->GetGUID());
    if (seen == _freshSeen.end())
    {
        if (_freshSeen.size() >= FRESH_ADD_MEMO)
        {
            auto oldest = _freshSeen.begin();
            for (auto itr = _freshSeen.begin(); itr != _freshSeen.end(); ++itr)
            {
                if (getMSTimeDiff(itr->second, now) > getMSTimeDiff(oldest->second, now))
                    oldest = itr;
            }
            _freshSeen.erase(oldest);
        }

        seen = _freshSeen.emplace(target->GetGUID(), now ? now : 1).first;
    }

    uint32 const age = getMSTimeDiff(seen->second, now);
    if (age >= FRESH_ADD_HOLD_MS)
        return false;

    // a tank of the group in reach of the mob to take it: else there is nobody to wait for
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    Player* tank = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref && !tank; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && member->IsInWorld() && member->GetMap() == bot->GetMap() &&
            PlayerbotAI::IsTank(member) && member->IsWithinDistInMap(target, FRESH_ADD_TANK_RANGE))
            tank = member;
    }

    if (!tank)
        return false;

    if (_freshLogged != target->GetGUID())
    {
        _freshLogged = target->GetGUID();
        LOG_INFO("playerbots", "[fresh add hold] {} waits on {} {}: {} ms in the fight, on {}, not on a tank yet "
                 "({} {:.1f} yd from it), up to {} ms", bot->GetName(), target->GetName(), target->GetEntry(), age,
                 victim ? victim->GetName() : "nobody", tank->GetName(), tank->GetExactDist(target),
                 FRESH_ADD_HOLD_MS);
    }

    return true;
}

// [rot1] A crowd control in a fight is a threat decision, as a player's sheep or Cyclone is (Omen, the tank's
// pickup): before the pull it is the pull itself (pre-pull cc), on a mob beating the caster it is self-defence; else
// it is never cast on a mob a tank holds (it takes the mob off the tank: Cyclone makes it immune, a break sends it to
// the highest threat, the caster's), never where the group's own damage breaks it at once (CrowdControlWouldBreak,
// the rule "cast auto cc" already keeps), and never past the single-target hold point.
bool ThreatMultiplier::CrowdControlHolds(CastSpellAction* action, Unit* target)
{
    if (!target || !target->IsAlive() || target->GetGUID().IsPlayer() || !target->IsInCombat())
        return false;

    if (target->GetVictim() == bot)
        return false;

    std::string const& spell = action->getSpell();
    char const* why = nullptr;
    char const* breaks = nullptr;
    float share = 0.0f;

    Unit* victim = target->GetVictim();
    Player* holder = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (holder && holder != bot && holder->GetGroup() == bot->GetGroup() && PlayerbotAI::IsTank(holder))
        why = "a tank holds it";
    else if (!IsDamageProofCrowdControl(spell) && CrowdControlWouldBreak(botAI, target, &breaks))
        why = breaks ? breaks : "the group's damage breaks it";
    else
    {
        bool const careful = AI_VALUE(PartyStrength, "party strength").CarefulThreat();
        share = ThreatLead::ProjectedShare(botAI, target);
        float const limit = ThreatLead::PullOverPoint(bot, target) *
                            (careful ? ThreatLead::CAREFUL_HOLD_SHARE : ThreatLead::HOLD_SHARE);
        if (share >= limit)
            why = "over the hold point";
    }

    if (!why)
        return false;

    uint32 const now = getMSTime();
    if (!_ccLoggedMs || getMSTimeDiff(_ccLoggedMs, now) >= HOLD_LOG_MS)
    {
        _ccLoggedMs = now ? now : 1;
        LOG_INFO("playerbots", "[rot1] [cc-threat] {} holds {} on {} {} in the fight: {} (on {}, share {:.0f} %)",
                 bot->GetName(), spell, target->GetName(), target->GetEntry(), why,
                 victim ? victim->GetName() : "nobody", share * 100.0f);
    }

    return true;
}

namespace
{
    // [aoe1] the taunts a tank takes mobs back with, and how many one cast takes: the single taunts one, Righteous
    // Defense three (the attackers of the friend it is cast on), Challenging Shout / Roar every mob in reach
    struct AreaTaunt
    {
        char const* name;
        uint8 takes;
    };

    std::vector<AreaTaunt> const& AreaTaunts(Player* tank)
    {
        static std::vector<AreaTaunt> const warrior = {{"taunt", 1}, {"mocking blow", 1}, {"challenging shout", 255}};
        static std::vector<AreaTaunt> const paladin = {{"hand of reckoning", 1}, {"righteous defense", 3}};
        static std::vector<AreaTaunt> const knight = {{"dark command", 1}, {"death grip", 1}};
        static std::vector<AreaTaunt> const druid = {{"growl", 1}, {"challenging roar", 255}};
        static std::vector<AreaTaunt> const none;
        switch (tank->getClass())
        {
            case CLASS_WARRIOR:
                return warrior;
            case CLASS_PALADIN:
                return paladin;
            case CLASS_DEATH_KNIGHT:
                return knight;
            case CLASS_DRUID:
                return druid;
            default:
                return none;
        }
    }

    constexpr float AREA_GCD_SEC = 1.5f;  // the game's global cooldown: one taunt per step

    // [aoe1] user 2026-11-03 (aggro-loss-if-survives) for an area that takes mobs over the game's line: the caster
    // survives them until a tank of the group has them back (its ready taunts in reach and sight, one per global
    // cooldown, the hardest hitter first) or they die (their expected fight lifetime), with the mobs already on it,
    // and a healer reaches it. The same terms as the leader's lead985 / lead1010 survivesSet, for every bot
    bool SurvivesTaking(Player* bot, PlayerbotAI* botAI, std::vector<Unit*> const& mobs, std::string& why)
    {
        Group* group = bot->GetGroup();
        if (!group || mobs.empty())
        {
            why = "no group";
            return false;
        }
        Player* healer = nullptr;
        Player* taker = nullptr;
        std::vector<uint8> steps;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap())
                continue;
            // [judge19] the leader's own test (HealerCovers), which this wrote out again with a literal 40 yd
            if (!healer && PlayerbotAI::IsHeal(member) && Leader::Fight::HealerCovers(bot, member))
                healer = member;
            if (!PlayerbotAI::IsTank(member))
                continue;
            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            if (!memberAI)
                continue;
            std::vector<uint8> ready;
            for (AreaTaunt const& taunt : AreaTaunts(member))
            {
                uint32 const id = memberAI->GetAiObjectContext()->GetValue<uint32>("spell id", taunt.name)->Get();
                SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
                if (!info || member->HasSpellCooldown(id))
                    continue;
                float const range = std::max(info->GetMaxRange(false), NOMINAL_MELEE_RANGE);
                bool const reaches = std::all_of(mobs.begin(), mobs.end(), [&](Unit* mob)
                    { return member->IsWithinCombatRange(mob, range) && member->IsWithinLOSInMap(mob); });
                if (reaches)
                    ready.push_back(taunt.takes);
            }
            uint32 const total = std::accumulate(ready.begin(), ready.end(), 0u);
            if (total > std::accumulate(steps.begin(), steps.end(), 0u))
            {
                taker = member;
                steps = ready;
            }
        }
        if (!healer)
        {
            why = "no healer reaches it";
            return false;
        }
        std::sort(steps.begin(), steps.end(), std::greater<uint8>());

        std::vector<std::pair<float, Unit*>> hits;
        for (Unit* mob : mobs)
        {
            float part = 0.0f;
            hits.emplace_back(Leader::Fight::LandedOnDps(mob, bot, part), mob);  // [judge18]
        }
        std::sort(hits.begin(), hits.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        float damage = 0.0f;
        float longest = 0.0f;
        float incoming = 0.0f;  // [judge19] the rate the heals term is judged against, as the leader judges it
        size_t next = 0;
        for (size_t step = 0; step < hits.size(); ++step)
        {
            // the taunt steps first, then the mobs no taunt is left for live out their lifetime on the caster
            float const back = step < steps.size() ? AREA_GCD_SEC * float(step + 1) : -1.0f;
            uint8 const takes = step < steps.size() ? steps[step] : uint8(1);
            for (uint8 n = 0; n < takes && next < hits.size(); ++n, ++next)
            {
                float const life = ExpectedFightLifetime(botAI, hits[next].second);
                float const until = back < 0.0f ? life : std::min(back, life);
                damage += hits[next].first * until;
                incoming += hits[next].first;  // [judge19]
                longest = std::max(longest, until);
            }
            if (next >= hits.size())
                break;
        }
        for (Unit* attacker : bot->getAttackers())
            if (attacker && attacker->IsAlive() &&
                std::none_of(mobs.begin(), mobs.end(), [attacker](Unit* m) { return m == attacker; }))
            {
                float part = 0.0f;
                float const dps = Leader::Fight::LandedOnDps(attacker, bot, part);  // [judge18]
                damage += dps * longest;
                incoming += dps;  // [judge19]
            }
        float const health = float(bot->GetHealth()) + float(bot->GetTotalAuraModifier(SPELL_AURA_SCHOOL_ABSORB));
        // [judge19] the leader's own heals term (Leader::Fight::HealerCover, [dun235]). This read
        // `damage < health` - no heals at all - while its comment above claimed the leader's terms, so the leader
        // logged "the area may take them" and the member refused the cast (b475: 22 clashes on 4 bots)
        float const heals = Leader::Fight::HealerCover(bot, healer, taker, longest, incoming, health);
        bool const survives = damage < health + heals;
        why = Acore::StringFormat("{:.0f} damage until {} ({:.1f} s) vs {:.0f} health + {:.0f} heals [dun235], "
                                  "healer {}",
                                  damage, taker ? "a tank has them back" : "they die", longest, health, heals,
                                  healer->GetName());
        return survives;
    }
}  // namespace

// [rot1] An area spell hits every mob in its radius, not only the bot's target: the old gate read the "aoe" value
// (the "attackers" within 12 yd of the bot or its target) and Starfall, a Single spell, only its target. strat-h pull
// 10 (8 of the druid's 16 aggro losses): Starfall and Hurricane on adds of the boss pack the tank had no lead on yet.
// Every mob the group fights (on a member's threat list) inside the spell's area is read; the current target is left
// to the single-target hold below (the tank's focus is on it).
// [aoe1] One computed rule for every content (dungeon, raid, world boss, open world): each mob the area reaches is
// compared with ITS holder at the game's line (ThreatLead::PullOverPoint: 110 % in its melee range, 130 % beyond),
// not with a fixed share of the tank's threat. The old 50 % (30 % "careful") limit held 36824 logged casts in the
// kept logs (b185-b351), 15290 of them under the game's line (41 %), and the leader-less fights (raids, world bosses,
// the open world) had no survival rule at all. Over the line the aggro-loss-if-survives rule decides (SurvivesTaking);
// a mob on nobody or not yet pulled is never taken by an area the leader did not plan.
// [kara4] a charmed member this far outside an area's radius still counts (Pestilence spreads round a target that
// moves, the member walks)
constexpr float CHARMED_AREA_SLACK = 2.0f;

bool ThreatMultiplier::AreaHolds(CastSpellAction* action, SpellInfo const* info)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    std::string const& spell = action->getSpell();
    Unit* mainTarget = AI_VALUE(Unit*, "current target");
    Unit* actionTarget = action->GetTarget();
    bool const selfCentred = !info || info->GetMaxRange(false, bot) <= 0.0f;
    WorldObject const* center = selfCentred ? static_cast<WorldObject const*>(bot)
                                            : static_cast<WorldObject const*>(actionTarget ? actionTarget : mainTarget);
    if (!center)
        return false;

    float const radius = AreaRadius(bot, info, spell);

    // [kara4] a group member an enemy charms stands in the area: the area would hit it (a charmed player is hostile
    // to its group, an area spell's targets are picked by the core, not by the bot's target lists). b358 kara-n
    // 20261109-003901-10A-1 t=339: a Spectral Retainer's Mind Control took the fury warrior; the death knight's
    // diseases (Pestilence spreads them) and the group's areas took it to 1 %, Blood Plague's tick killed it. The
    // charm ends, the member is the group's again: no area until then.
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* member = ref->GetSource(); member && member != bot && member->IsAlive() &&
                                                member->GetMap() == bot->GetMap() &&
                                                AttackersValue::IsCharmedAlly(member, bot) &&
                                                center->IsWithinDist(member, radius + CHARMED_AREA_SLACK, false))
        {
            std::string const kind = "held charmed " + std::string(member->GetName());
            std::string& last = _aoeVerdicts[spell];
            if (last != kind)
            {
                last = kind;
                LOG_INFO("playerbots", "[kara4] [aoe] {} {}: held, {} is charmed inside the area ({:.0f} yd)",
                         bot->GetName(), spell, member->GetName(), center->GetExactDist(member));
            }
            return true;
        }

    // [rot47] The threat the spell adds to each mob it hits: the single-target projection's "next cast threat" over
    // the time the spell keeps hitting - a lasting area (Death and Decay, Consecration, Rain of Fire: a persistent
    // area aura; Starfall: a periodic trigger on the caster) over its duration, a channel (Hurricane, Blizzard,
    // Volley) over its channel, Pestilence over the diseases it spreads (the own disease's full duration on the
    // target). A lasting area also reads the mobs walking into it (their victim stands inside); the diseases spread
    // only to the mobs already in Pestilence's radius.
    float commit = CommitSeconds(bot, info);
    bool lasting = false;
    if (info && info->GetDuration() > 0 &&
        (info->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA) || info->HasAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL)))
    {
        commit = std::max(commit, static_cast<float>(info->GetDuration()) / IN_MILLISECONDS);
        lasting = true;
    }
    if (spell == "pestilence" && mainTarget)
    {
        for (char const* disease : { "frost fever", "blood plague" })
            if (Aura const* aura = botAI->GetAura(disease, mainTarget, true))
                commit = std::max(commit, static_cast<float>(aura->GetMaxDuration()) / IN_MILLISECONDS);
    }
    float const added = AI_VALUE(float, "next cast threat") * commit / NextCastThreatValue::NEXT_CAST_SECONDS;

    // [lead1010] the leader's threat verdict is final (lead282 for the single target): the mobs its Attack names for
    // this member - the target, the mobs it has headroom on, the mobs without headroom it survives taking all at once
    // (lead985's survival math for an area, the same rule as below with the leader's measured threat rates) - are not
    // held here again. The leader's spared mobs are held by LeaderCommandMultiplier
    std::vector<ObjectGuid> leaderNamed;
    if (Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        if (Leader::Events::Get(bot->GetGUID(), events) && events.attack == Leader::AttackEvent::Attack &&
            events.intensity != Leader::Intensity::Stop && events.aoe != Leader::AoeRule::Forbidden)
            leaderNamed = events.killOrder;
    }
    auto const named = [&leaderNamed](Unit* mob)
    { return std::find(leaderNamed.begin(), leaderNamed.end(), mob->GetGUID()) != leaderNamed.end(); };

    uint32 reached = 0;     // the mobs the area reaches (the line's N)
    Unit* worst = nullptr;  // the highest share of its holder among them
    float worstShare = 0.0f;
    std::vector<Unit*> over;  // the mobs the area takes off their holder (over the game's line)
    std::string held;         // a reason no survival can lift: a loose mob or an idle one
    std::vector<ObjectGuid> seen;
    for (GroupReference* ref = group->GetFirstMember(); ref && held.empty(); ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;

        for (auto const& entry : member->GetThreatMgr().GetThreatenedByMeList())
        {
            Creature* mob = entry.second ? entry.second->GetOwner() : nullptr;
            if (!mob || mob == mainTarget || !mob->IsAlive() || !mob->IsInWorld() || mob->GetMap() != bot->GetMap())
                continue;

            if (std::find(seen.begin(), seen.end(), mob->GetGUID()) != seen.end())
                continue;
            seen.push_back(mob->GetGUID());
            if (named(mob))
                continue;  // [lead1010]

            if (mob->IsControlledByPlayer() || mob->IsInEvadeMode() || mob->GetVictim() == bot ||
                FledMobs::IsRunning(mob))
                continue;

            Unit* const mobVictim = mob->GetVictim();
            bool const inArea = center->IsWithinDist(mob, radius, false);
            if (!inArea && !(lasting && mobVictim && center->IsWithinDist(mobVictim, radius, false)))
                continue;
            ++reached;

            // [rot47b] the holder's threat over the same seconds: the bot's side was projected over the spell's whole
            // duration (Death and Decay 10 s) against the holder's threat of now - the tank's own threat over those
            // 10 s at the rate measured on that mob (its threat since this bot first read it) is in the comparison
            // too. A holder whose threat fell (a taunt, a threat drop, another holder) starts its measure again
            float holder = ThreatLead::HolderThreat(bot, mob);
            if (holder <= 0.0f)
            {
                // [aoe1] a mob on nobody of the group yet: the area would be its first threat
                held = Acore::StringFormat("{} is on nobody", mob->GetName());
                worst = mob;
                break;
            }
            if (holder >= ThreatLead::HOLD_UNBOUNDED)
                continue;  // a friendly NPC keeps it whatever the bot does
            {
                struct Seen
                {
                    float threat;
                    uint32 ms;
                };
                thread_local std::unordered_map<uint64, Seen> holderSeen;
                if (holderSeen.size() > 4096)
                    holderSeen.clear();
                uint32 const nowMs = getMSTime();
                uint64 const key = mob->GetGUID().GetRawValue() ^ (bot->GetGUID().GetRawValue() << 1);
                auto const [it, fresh] = holderSeen.try_emplace(key, Seen{holder, nowMs});
                if (!fresh && holder < it->second.threat)
                    it->second = Seen{holder, nowMs};
                uint32 const sinceMs = getMSTimeDiff(it->second.ms, nowMs);
                if (sinceMs >= IN_MILLISECONDS)
                    holder += (holder - it->second.threat) * commit * IN_MILLISECONDS / float(sinceMs);
            }
            float const share = (mob->GetThreatMgr().GetThreat(bot) + added) / holder;
            if (!worst || share > worstShare)
            {
                worst = mob;
                worstShare = share;
            }
            if (share >= ThreatLead::PullOverPoint(bot, mob))
                over.push_back(mob);
        }
    }

    // [aoe1] a mob not pulled yet inside the area: the area pulls it (the leader's own AoE rule spares those it did
    // not plan: LeaderAoeRule "is not pulled")
    if (held.empty())
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest hostile npcs"))
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || unit->IsInCombat() || unit == mainTarget ||
                !bot->IsValidAttackTarget(unit) || !center->IsWithinDist(unit, radius, false) || named(unit))
                continue;
            held = Acore::StringFormat("{} is not pulled", unit->GetName());
            worst = unit;
            break;
        }

    if (!reached && held.empty())
        return false;

    bool casts = held.empty();
    std::string reason;
    if (casts && !over.empty())
    {
        std::string why;
        casts = SurvivesTaking(bot, botAI, over, why);
        reason = Acore::StringFormat("{} over the line, {}", over.size(), why);
    }
    else if (!casts)
        reason = held;

    // one line per decision kind and spell (a change of the verdict, no time throttle)
    std::string const kind = casts         ? (over.empty() ? "casts" : "casts survives")
                             : held.empty() ? "held survival"
                                            : "held " + held;
    std::string& last = _aoeVerdicts[spell];
    if (last != kind)
    {
        last = kind;
        Unit* victim = worst ? worst->GetVictim() : nullptr;
        LOG_INFO("playerbots", "[aoe] {} {}: {} mobs, worst {} at {:.0f} % of its holder {} after the cast -> {}{}{}{}",
                 bot->GetName(), spell, reached, worst ? worst->GetName() : std::string("-"), worstShare * 100.0f,
                 victim ? victim->GetName() : std::string("nobody"), casts ? "casts" : "held",
                 reason.empty() ? "" : " (", reason, reason.empty() ? "" : ")");
    }
    return !casts;
}

namespace
{
    // [rot2] Casts on another member that the leader assigns now (Leader::MemberEvents::externalSpell): a led bot no
    // longer picks them itself. Its personal saves (Barkskin, Ice Block, its own Pain Suppression) are not here.
    bool IsAutonomousExternal(std::string const& name)
    {
        static std::vector<std::string> const names = {
            "innervate on healer", "hymn of hope", "divine hymn", "power infusion", "pain suppression on party",
            "pain suppression on spiking tank", "hand of sacrifice on main tank", "hand of sacrifice on spiking tank",
            "hand of salvation on party", "hand of freedom on party", "blessing of protection on party",
            "misdirection on main tank", "tricks of the trade on main tank"};
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    // [rot2] The long offensive cooldowns (2 min and more) that wait for the leader's Burn. A cooldown of 90 s or less
    // (Dancing Rune Weapon, Starfall) stays on cooldown, as the sim APLs cast it: with the leader's burn windows on the
    // bosses (5+ min apart) holding it would lose uses, holding a 2-5 min one loses none. The death knight's cooldowns
    // (Hysteria, Raise Dead, Empower Rune Weapon, Army of the Dead, Summon Gargoyle) take the same rule in their
    // triggers (IsDKBossFight, IsGargoyleFight): their action names are shared with defensive uses.
    // [rot5] Added by the same rule (3.3.5 cooldowns, the bots' talents): Mirror Image 3 min, Feral Spirit 3 min,
    // Fire / Earth Elemental Totem 10 min, Elemental Mastery 3 min, Shadowfiend 5 min, Rapid Fire 3 min (Rapid
    // Killing), Readiness 3 min, Adrenaline Rush 5 min, Killing Spree 2 min, Blade Flurry 2 min, Avenging Wrath 2-3
    // min, Recklessness 5 min, Death Wish 2-3 min, Metamorphosis 2-3 min, Presence of Mind 2 min, Summon Infernal /
    // Doomguard 10 / 20 min. Kept on cooldown: Bestial Wrath (84 s with Longevity, BM's template talent), Shadow Dance
    // (1 min), Bladestorm (90 s), Starfall, Dancing Rune Weapon, Unbreakable Armor; Summon Water Elemental is the frost
    // template's permanent pet (Glyph of Eternal Water 50045, "no pet" upkeep), not a cooldown. Exceptions in
    // BurnCooldownException: a healer's Avenging Wrath / Shadowfiend (its heal and mana tools), Shadowfiend for mana,
    // Mirror Image as the mage's own threat drop with mobs on it.
    // [lead80] the one list (Leader::BURN_COOLDOWNS) - the "kept on cooldown" ones above now wait for the leader's
    // burn too: the leader burns them when they pay (the fight outlasts the effect, back before the next boss)
    bool IsBurnCooldown(std::string const& name) { return Leader::IsBurnCooldown(name); }

    // [lead247] b191 rfc-h 5G: 16 losses off the blood DK, 32 of the 47 aggro-loss lines on the ret paladin (Divine
    // Storm 22 % of its damage) and the arms warrior (Whirlwind 23 %, Sweeping Strikes 12 %), 27 of them while the
    // leader's AoE rule was Forbidden: those actions are no Aoe action type, the rule never held them. A hostile
    // spell that hits an area or chains (Divine Storm, Whirlwind, Cleave, Consecration) is area damage by its own
    // spell data; the self buffs that splash every melee hit onto a second mob are named (no spell data says so)
    bool IsSplashBuff(std::string const& name) { return name == "sweeping strikes" || name == "blade flurry"; }

    // [rot2] Where an area action lands and how far it reaches (the rules of ThreatMultiplier::AreaHolds).
    bool AreaFootprint(Player* bot, CastSpellAction* action, SpellInfo const* info, Unit* mainTarget, Position& center,
                       float& radius, bool& selfCentred)
    {
        selfCentred = !info || info->GetMaxRange(false, bot) <= 0.0f;
        Unit* target = action->GetTarget();
        WorldObject const* origin = selfCentred ? static_cast<WorldObject const*>(bot)
                                                : static_cast<WorldObject const*>(target ? target : mainTarget);
        if (!origin)
            return false;

        center.Relocate(origin->GetPositionX(), origin->GetPositionY(), origin->GetPositionZ());
        radius = AreaRadius(bot, info, action->getSpell());
        return true;
    }
}

namespace
{
    // [rot3] The personal saves a led bot uses on its own only in an emergency (LeaderCommandMultiplier::Emergency);
    // a pre-emptive save ("a big hit is coming") is the leader's PreSave ("leader pre save").
    bool IsPersonalSave(std::string const& name)
    {
        static std::vector<std::string> const names = {
            "barkskin", "survival instincts", "frenzied regeneration", "ice block", "divine shield",
            "divine protection", "icebound fortitude", "anti-magic shell", "vampiric blood", "shield wall",
            "last stand", "enraged regeneration", "evasion", "cloak of shadows", "deterrence", "dispersion",
            "pain suppression"};
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    // [rot3] The bot's own target choice: off for a led bot without a fresh command (no new decision).
    bool IsOwnTargetChoice(std::string const& name)
    {
        static std::vector<std::string> const names = {"dps assist", "dps aoe", "attack anything", "attack rti target",
                                                       "tank assist"};
        return std::find(names.begin(), names.end(), name) != names.end();
    }
}

// [lead813] b221 rfc-h 5C: Rtdruab's Starfall held 50 times "the area reaches the leader's spared <mob>" (Earthborer
// 23, Ragefire Trogg 12, Molten Elemental 9) - the test was the 30 yd sphere alone, through the cave's rock. The
// game's area hits only what the spell's centre sees (Spell::CheckEffectTarget: the line of sight unless
// SPELL_ATTR2_IGNORE_LINE_OF_SIGHT; Starfall's own filter, spell_dru_starfall_dummy, the same): a spared mob out of
// that sight is out of the area
bool LeaderCommandMultiplier::AreaSees(SpellInfo const* info, Unit* mob, Position const& center)
{
    if (!info || info->HasAttribute(SPELL_ATTR2_IGNORE_LINE_OF_SIGHT) ||
        mob->IsWithinLOS(center.GetPositionX(), center.GetPositionY(), center.GetPositionZ()))
        return true;
    // [lead1135] b238 rfc-h 5C-11 409.5-412.5 s: Starfall from the ledge (-152.6, 0.8, -40.1) woke a Ragefire Shaman
    // and a Trogg 13-18 yd below - "out of its Starfall's sight" by the line from the mob to the druid's feet, which
    // the ledge's edge cuts. A caster-centred area's targets are picked by the caster's sight from its eyes
    // (spell_dru_starfall_dummy::FilterTargets: caster->IsWithinLOS(target)); either line seeing it is in the area
    if (center.GetExactDist(bot) <= 0.1f &&
        bot->IsWithinLOS(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ()))
    {
        thread_local std::set<std::pair<ObjectGuid, ObjectGuid>> eyesLogged;
        if (eyesLogged.size() > 4096)
            eyesLogged.clear();
        if (eyesLogged.insert({bot->GetGUID(), mob->GetGUID()}).second)
            LOG_INFO("playerbots", "[lead1135] {}: the spared {} is in its {}'s sight from its eyes: in the area",
                     bot->GetName(), mob->GetName(), info->SpellName[0] ? info->SpellName[0] : "area");
        return true;
    }
    thread_local std::set<std::pair<ObjectGuid, ObjectGuid>> logged;
    if (logged.size() > 4096)
        logged.clear();
    if (logged.insert({bot->GetGUID(), mob->GetGUID()}).second)
        LOG_INFO("playerbots", "[lead813] {}: the spared {} is out of its {}'s sight: out of the area", bot->GetName(),
                 mob->GetName(), info->SpellName[0] ? info->SpellName[0] : "area");
    return false;
}

// [lead415] the first of the leader's spared mobs (Leader::MemberEvents::aoeSpared) the area reaches; nullptr: none
Unit* LeaderCommandMultiplier::SparedInArea(std::function<bool(Unit*)> const& reaches)
{
    for (ObjectGuid const& guid : _command.aoeSpared)
        if (Unit* mob = botAI->GetUnit(guid); mob && mob->IsAlive() && reaches(mob))
            return mob;
    return nullptr;
}

void LeaderCommandMultiplier::Refresh()
{
    uint32 const now = getMSTime();
    if (_readMs && getMSTimeDiff(_readMs, now) < COMMAND_CACHE_MS)
        return;

    _readMs = now ? now : 1;
    LeaderCommandState::State const state = LeaderCommandState::Read(bot, _command);
    _led = state != LeaderCommandState::State::NotLed;
    _hasCommand = state == LeaderCommandState::State::Fresh;

    // [lead601] b217 rfc-h 5C 541 s / 818 s: Rtdruab's Starfall passed the cast gate (no spared mob in its 30 yd then)
    // and its stars went on hitting for 10 s while the leader spared the pack's mobs it reached (no threat headroom:
    // "spares Searing Blade Cultist / Enforcer"): 7 of the run's 31 aggro losses "threat on a mob it was not told to
    // hit (area / splash / dot)" within 5 s of the two casts (542.5-546.1, 822.4, 826.3). A lasting area of the
    // member's own on itself (a periodic trigger that deals damage: Starfall's stars, Bladestorm's Whirlwind) is the
    // area spell the leader's AoE rule holds as long as it hits: the leader's Stop, its AoE rule forbidding the area,
    // or a spared mob in its reach (lead472's lasting rule: the mob or its victim inside) ends it, as the cast gate
    // would hold its next cast
    if (_hasCommand && bot->IsInCombat() && !PlayerbotAI::IsTank(bot))
    {
        bool const stop = _command.attack == Leader::AttackEvent::StopDps ||
                          _command.intensity == Leader::Intensity::Stop;
        for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL))
        {
            SpellInfo const* info = effect ? effect->GetSpellInfo() : nullptr;
            if (!info || effect->GetCasterGUID() != bot->GetGUID() || info->GetMaxRange(false, bot) > 0.0f ||
                !TriggersDamage(info, 2))
                continue;
            std::string spell = info->SpellName[0] ? info->SpellName[0] : "";
            std::transform(spell.begin(), spell.end(), spell.begin(), ::tolower);
            float const radius = AreaRadius(bot, info, spell);
            Position const center = bot->GetPosition();
            std::string why;
            // [lead1160] b240 rfc-h: 14 Starfalls cancelled 1-2 s after the cast ("the area reaches the leader's
            // spared Searing Blade Enforcer"; "AoE outside the leader's pack" 3), Taragaman's Starfall 0 damage in
            // 5C-11 against the sim's ~450 dps from it. A cancel gives no cooldown back (Starfall's 60 s recharge
            // runs from the cast): it is worth only the mob it keeps out of the fight. A running area is cancelled
            // only when its next star really hits a spared mob - the game's own target test, not the cast gate's
            // forecast: the mob within the area's radius plus its size (WorldObjectSpellAreaTargetCheck, a player's
            // spell on a creature), a valid target for the caster, and seen by the caster from its eyes both ways
            // the game reads it (spell_dru_starfall_dummy::FilterTargets: caster->IsWithinLOS(target); Spell::
            // CheckEffectTarget: caster->IsWithinLOSInMap(target, M2)) - or an unpulled walker that gets there
            // within the aura's next tick (FieldWalkerEnters over the periodic amplitude), or a pulled spared mob
            // whose victim stands where the caster's stars reach. The pack's centre moving off the caster is no
            // hit on anyone: that test stays the cast gate's (AreaHolds)
            float const tickSec = float(std::max<int32>(effect->GetAmplitude(), 1)) / 1000.0f;
            auto const hit = [&](Unit* mob)
            {
                return bot->IsValidAttackTarget(mob) && mob->GetExactDist(&center) <= radius + mob->GetCombatReach() &&
                       (info->HasAttribute(SPELL_ATTR2_IGNORE_LINE_OF_SIGHT) ||
                        (bot->IsWithinLOS(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ()) &&
                         bot->IsWithinLOSInMap(mob, VMAP::ModelIgnoreFlags::M2)));
            };
            if (stop)
                why = "the leader's Stop";
            else if (_command.aoe == Leader::AoeRule::Forbidden)
                why = "the leader forbids AoE";
            else if (Unit* spared = SparedInArea(
                         [&](Unit* mob)
                         {
                             if (hit(mob))
                                 return true;
                             if (!mob->IsInCombat())
                                 return mob->ToCreature() &&
                                        !Leader::Fight::FieldWalkerEnters(mob->ToCreature(), center, radius, tickSec)
                                             .empty();
                             Unit* const mobVictim = mob->GetVictim();
                             return mobVictim && mobVictim->GetExactDist(&center) <= radius &&
                                    bot->IsWithinLOSInMap(mobVictim, VMAP::ModelIgnoreFlags::M2);
                         }))
                why = "[lead1160] its next tick hits the leader's spared " + spared->GetName();
            if (why.empty())
                continue;
            uint32 const id = info->Id;
            bot->RemoveAurasDueToSpell(id);
            LOG_INFO("playerbots", "[lead601] {} cancels its {} ({}): {}", bot->GetName(), spell, id, why);
            break;
        }
        // [lead1365] b259 rfc-h 5H-8 564.5-569 s: the leader's Stop ("its splash threat on Searing Blade Enforcer
        // overtakes the tank") held the mage's next casts while its Blizzard channel went on ticking on four
        // Enforcers - lead901 even counted the channel's ticks left (11390 vs the tank's 8626); at 565.4 two
        // Enforcers and a Cultist were on it (victimCasts Blizzard), it died at 569. A channelled area that deals
        // damage (Blizzard, Rain of Fire, Hurricane, Volley, Mind Sear) is the member's own running area as lead601's
        // auras are: the Stop, the AoE rule forbidding it, a spared mob or the DontAttack mob in its reach (the
        // radius plus the mob's size round the channel's centre, in its sight: the game's area test) interrupts it -
        // its next tick adds the threat the Stop holds. Interrupting a channel removes its dynamic object
        // (Spell::cancel -> Unit::RemoveDynObject)
        if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            SpellInfo const* info = channel->GetSpellInfo();
            if (info && HitsSeveral(info) && ChannelDealsDamage(info))
            {
                Position center = bot->GetPosition();
                if (channel->m_targets.HasDst() && channel->m_targets.GetDstPos())
                    center = *channel->m_targets.GetDstPos();
                // [build149] the target by its guid: the cached pointer can outlive the unit
                else if (Unit* on = ObjectAccessor::GetUnit(*bot, channel->m_targets.GetUnitTargetGUID()))
                    center = on->GetPosition();
                std::string spell = info->SpellName[0] ? info->SpellName[0] : "";
                std::transform(spell.begin(), spell.end(), spell.begin(), ::tolower);
                float const radius = AreaRadius(bot, info, spell);
                auto const reaches = [&](Unit* mob)
                {
                    return mob && mob->IsAlive() && mob->GetExactDist(&center) <= radius + mob->GetCombatReach() &&
                           AreaSees(info, mob, center);
                };
                std::string why;
                if (stop)
                    why = "the leader's Stop";
                else if (_command.aoe == Leader::AoeRule::Forbidden)
                    why = "the leader forbids AoE";
                else if (Unit* spared = SparedInArea(reaches))
                    why = "its next tick hits the leader's spared " + spared->GetName();
                else if (Unit* loose = _command.dontAttack.IsEmpty() ? nullptr : botAI->GetUnit(_command.dontAttack);
                         loose && reaches(loose))
                    why = "its next tick hits the leader's DontAttack " + loose->GetName();
                if (!why.empty())
                {
                    bot->InterruptSpell(CURRENT_CHANNELED_SPELL);
                    LOG_INFO("playerbots", "[lead1365] {} interrupts its channelled {} ({}): {}", bot->GetName(),
                             spell, info->Id, why);
                }
            }
        }
    }

    // [rot3] the bot's health over the last EMERGENCY_WINDOW_MS, for the "big hit landing" test
    while (!_health.empty() && getMSTimeDiff(_health.front().first, _readMs) > EMERGENCY_WINDOW_MS)
        _health.pop_front();
    _health.emplace_back(_readMs, bot->GetHealthPct());
}

bool LeaderCommandMultiplier::Emergency()
{
    // [rot3] critical health, or low health with a big hit landing now (EMERGENCY_DROP_PCT of the bot's health lost
    // within EMERGENCY_WINDOW_MS)
    float const hp = bot->GetHealthPct();
    if (hp < EMERGENCY_HEALTH_PCT)
        return true;
    if (hp >= EMERGENCY_HIT_HEALTH_PCT)
        return false;

    float peak = hp;
    for (auto const& sample : _health)
        peak = std::max(peak, sample.second);
    return peak - hp >= EMERGENCY_DROP_PCT;
}

void LeaderCommandMultiplier::Log(Action* action, char const* why)
{
    uint32 const now = getMSTime();
    if (_loggedMs && getMSTimeDiff(_loggedMs, now) < LOG_MS && _loggedAction == action->getName())
        return;

    _loggedMs = now ? now : 1;
    _loggedAction = action->getName();
    LOG_INFO("playerbots", "[rot2] [leader command] {} holds {}: {}", bot->GetName(), action->getName(), why);
}

float LeaderCommandMultiplier::GetValue(Action* action)
{
    // the name tests first, the command read (cached) next, the spell and area lookups last
    if (!action)
        return 1.0f;

    std::string const& name = action->getName();
    if (name == "leader external" || name == "leader cc" || name == "leader threat" || name == "leader pre save" ||
        name == "interrupt assigned cast" || name == "leader dispel" || name == "leader consume" ||
        name == "leader use object")  // [lead400] [lead402] [lead405]
        return 1.0f;

    Refresh();
    // a group without our leader: the bot's own rules
    if (!_led)
        return 1.0f;
    // [lead56] user: in the fight the tank decides itself (taunt, AoE, threat, target switches): the led tank's own
    // fight rules, no command gate and no stale mode
    if (Leader::LEADER_TANK_OWNS_FIGHT && bot->IsInCombat() && PlayerbotAI::IsTank(bot) &&
        Leader::Orders::IsTracked(bot->GetGUID()))
        return 1.0f;

    // [rot3] a led bot: no fallback to its own rules; without a fresh command the safe minimum only
    bool const stale = !_hasCommand;

    // [rot3] its personal saves on its own only in an emergency (or the leader's PreSave)
    if (IsPersonalSave(name))
    {
        // [lead423] Dispersion for mana is the leader's mana tool (its External), no longer the bot's own
        if (Emergency() || LeaderCommandState::PreSaveAssigned(bot))
            return 1.0f;

        Log(action, "a personal save is for an emergency or the leader's PreSave");
        return 0.0f;
    }

    if (IsAutonomousExternal(name))
    {
        Log(action, "an external on another member waits for the leader's assignment");
        return 0.0f;
    }

    // [rot3] crowd control only on the leader's assigned mob ("pre pull cc" runs the leader's published plan)
    bool const ccAction = dynamic_cast<CastCrowdControlSpellAction*>(action) ||
                          (name.size() > 6 && name.compare(name.size() - 6, 6, " on cc") == 0);
    if (ccAction && name != "pre pull cc")
    {
        Unit* target = action->GetTarget();
        if (stale || _command.cc.IsEmpty() || !target || target->GetGUID() != _command.cc)
        {
            if (stale)
                LeaderCommandState::LogStale(bot, "holds its own crowd control");
            Log(action, "crowd control only on the leader's assigned mob");
            return 0.0f;
        }
    }

    bool const stop =
        !stale && (_command.attack == Leader::AttackEvent::StopDps || _command.intensity == Leader::Intensity::Stop);
    bool const burn = !stale && Leader::BurnAllows(_command, name);  // [lead178] the leader's verdict per cooldown
    // [rot5] the tool uses of a burn cooldown stay the bot's (IsBurnCooldown's comment)
    bool const burnException =
        // [lead423] a healer's Shadowfiend is the leader's mana tool too (its External, "leader external")
        (PlayerbotAI::IsHeal(bot) && name == "avenging wrath") ||
        // [lead422] user 2026-11-02: a damage dealer's own Shadowfiend below half mana is off - the leader's verdict
        // decides it (b201 5B: spent on trash at 38 % mana, on cooldown for Taragaman)
        (name == "mirror image" && AI_VALUE(uint8, "my attacker count") > 0) ||
        LeaderCommand::BurnToolUse(bot, name,
                                   bot->GetHealthPct() < static_cast<float>(sPlayerbotAIConfig.CriticalHealth),
                                   PlayerbotAI::IsHeal(bot));
    if (IsBurnCooldown(name) && !burn && !burnException)
    {
        if (stale)
            LeaderCommandState::LogStale(bot, "holds its burn cooldowns");
        Log(action, stop ? "the leader's Stop" : "a long cooldown waits for the leader's Burn");
        return 0.0f;
    }

    if (stale && IsOwnTargetChoice(name))
    {
        LeaderCommandState::LogStale(bot, "keeps its target");
        return 0.0f;
    }

    // [rot3] stale: no buff on another member
    if (stale && dynamic_cast<CastBuffSpellAction*>(action))
    {
        Unit* target = action->GetTarget();
        if (target && target != bot && !bot->IsHostileTo(target))
        {
            LeaderCommandState::LogStale(bot, "holds its buffs on others");
            return 0.0f;
        }
    }

    CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action);
    if (!spellAction)
        return 1.0f;

    // [rot4] Stop / StopDps, the AoE rule and the stale-command target rule are for offensive casts only: a heal or
    // any friendly spell passes (slabs-h: "holds regrowth / nourish / wild growth on party: the leader's Stop")
    Unit* target = action->GetTarget();
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spellAction->getSpell());
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    // [lead247] a splash buff is the area damage of the melee hits after it
    // [lead415] its second target is a mob within the core's NOMINAL_MELEE_RANGE of the bot (SelectNearbyTarget)
    if (IsSplashBuff(name) &&
        (stale || stop || !LeaderCommand::AoeAllowed(bot, bot->GetPosition()) ||
         SparedInArea([this](Unit* spared) { return bot->IsWithinDistInMap(spared, NOMINAL_MELEE_RANGE); })))
    {
        Log(action, stop ? "the leader's Stop" : "the leader forbids AoE");
        return 0.0f;
    }
    if (IsFriendlySpell(bot, info, target))
        return 1.0f;

    bool const aoe = action->getThreatType() == Action::ActionThreatType::Aoe || HitsSeveral(info);
    bool const hostile = target && target != bot && bot->IsHostileTo(target);
    if (!aoe && !hostile)
        return 1.0f;

    // [rot3] stale: the rotation on the current target while it is in the fight, no area spell
    if (stale)
    {
        Unit* current = AI_VALUE(Unit*, "current target");
        if (!aoe && current && target == current && current->IsAlive() && current->IsInCombat())
            return 1.0f;

        LeaderCommandState::LogStale(bot, "keeps to its current target, no area spell");
        return 0.0f;
    }

    if (stop)
    {
        Log(action, "the leader's Stop");
        return 0.0f;
    }

    if (hostile && !_command.dontAttack.IsEmpty() && target->GetGUID() == _command.dontAttack)
    {
        Log(action, "the leader's DontAttack mob");
        return 0.0f;
    }

    if (!aoe)
        return 1.0f;

    Position center;
    float radius = 0.0f;
    bool selfCentred = false;
    if (!AreaFootprint(bot, spellAction, info, AI_VALUE(Unit*, "current target"), center, radius, selfCentred))
        return 1.0f;

    // [rot310] a caster-centred area reaches the pack from its own radius away (Starfall from the ranged spot)
    if (!LeaderCommand::AoeAllowed(bot, center, selfCentred ? radius : 0.0f))
    {
        Log(action,
            _command.aoe == Leader::AoeRule::Forbidden ? "the leader forbids AoE" : "AoE outside the leader's pack");
        return 0.0f;
    }

    // [lead415] the leader's spared mobs: none within this spell's own radius of its centre (the game's area test,
    // the mob's size included)
    // [lead472] a lasting area (a persistent area aura: Consecration, Death and Decay; a periodic trigger: Bladestorm,
    // Starfall) also hits the spared mobs that walk into it while it lasts - the ones whose victim stands inside (the
    // rule ThreatMultiplier::AreaHolds reads for its own lasting areas). b207 rfc-h 5G / 5H / 5C: Bladestorm's
    // Whirlwind, Consecration and Starfall took pack mobs coming to the tank
    bool const lasting = info && info->GetDuration() > 0 &&
                         (info->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA) ||
                          info->HasAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL) || IsSeed(spellAction->getSpell()));
    // [lead1096] user 2026-11-04 (Q9): a ground field (a persistent area aura) is not placed where an unpulled walker
    // can walk into it before it ends (spell data duration)
    // [lead1100] b235 rfc-h: 191 holds on any-way walks at walk speed; a patrol counts only when its waypoint walk
    // enters the radius while the field lasts, a wanderer only when its wander radius reaches it
    // (Leader::Fight::FieldWalkerEnters). A walker the leader takes into the fight is no longer on its spared list
    // [lead1160] b240 rfc-h: 14 Starfalls passed this gate and were cancelled 1-2 s later when a walker came into
    // their 30 yd (Taragaman's room: a Searing Blade Enforcer patrol). Every lasting area - a ground field or a
    // periodic area on the caster (Starfall's stars, Bladestorm's Whirlwind) - is judged against the spared walkers
    // over its whole duration (spell data), as the field is
    bool const field = info && info->GetDuration() > 0 && info->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA);
    float const fieldSec = lasting ? float(info->GetMaxDuration()) / 1000.0f : 0.0f;
    std::string walkerKind;
    if (Unit* spared = SparedInArea(
            [&](Unit* mob)
            {
                if (mob->IsWithinDist3d(&center, radius) && AreaSees(info, mob, center))  // [lead813]
                    return true;
                Unit* const mobVictim = lasting ? mob->GetVictim() : nullptr;
                if (mobVictim && mobVictim->IsWithinDist3d(&center, radius))
                    return true;
                if (!lasting || !mob->ToCreature())
                    return false;
                walkerKind = Leader::Fight::FieldWalkerEnters(mob->ToCreature(), center, radius, fieldSec);
                return !walkerKind.empty();
            }))
    {
        Log(action, (!walkerKind.empty()
                         ? Acore::StringFormat("[{}] the {}'s {:.0f} s let the leader's spared {} {} (low {}, "
                                               "{:.1f} yd) walk into it",
                                               field ? "lead1100" : "lead1160", field ? "field" : "area", fieldSec,
                                               walkerKind, spared->GetName(),
                                               spared->GetGUID().GetCounter(), spared->GetExactDist(&center))
                         : "the area reaches the leader's spared " + spared->GetName())
                        .c_str());
        return 0.0f;
    }

    if (!_command.dontAttack.IsEmpty())
    {
        Unit* spared = botAI->GetUnit(_command.dontAttack);
        if (spared && spared->IsAlive() && spared->GetExactDist(&center) <= radius + spared->GetCombatReach())
        {
            Log(action, "the area holds the leader's DontAttack mob");
            return 0.0f;
        }
    }

    return 1.0f;
}

void ThreatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new ThreatMultiplier(botAI));
}

float FocusMultiplier::GetValue(Action* action)
{
    if (!action)
    {
        return 1.0f;
    }
    if (action->getThreatType() == Action::ActionThreatType::Aoe && !dynamic_cast<CastHealingSpellAction*>(action))
    {
        return 0.0f;
    }
    if (dynamic_cast<CastDebuffSpellOnAttackerAction*>(action))
    {
        return 0.0f;
    }
    return 1.0f;
}

void FocusStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new FocusMultiplier(botAI));
}
