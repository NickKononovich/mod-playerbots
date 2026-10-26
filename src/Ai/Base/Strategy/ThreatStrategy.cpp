/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ThreatStrategy.h"
#include "AttackerCountValues.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "PartyStrengthValue.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatValues.h"
#include "Timer.h"

namespace
{
    // Share of the tank's threat (percent) at which a dps stops a threat-generating action.
    constexpr uint8 AOE_THREAT_LIMIT = 50;
    constexpr uint8 SINGLE_THREAT_LIMIT = 80;
    // A melee bot pulls aggro only at 110 % of the victim's threat. 80 % held a Ret idle on 14 % of the Gruul
    // ticks (DPS audit 2026-10-24); 95 % keeps a 15-point margin under the melee pull point. Ranged keep 80 %.
    constexpr uint8 MELEE_SINGLE_THREAT_LIMIT = 95;
    // A tank soft for the content or outgeared by its dps (party strength): keep a wider margin, and hold AoE
    // until the tank has a solid lead on every mob of the pack.
    constexpr uint8 CAREFUL_AOE_THREAT_LIMIT = 30;
    constexpr uint8 CAREFUL_SINGLE_THREAT_LIMIT = 70;
    constexpr uint8 CAREFUL_MELEE_SINGLE_THREAT_LIMIT = 85;

    // [ranged_audit_b30] An interrupt or silence (Counterspell, Wind Shear, Kick, Silence, Silencing Shot), or a
    // crowd control that deals no damage (Fear, Polymorph, Hex, Hammer of Justice, Shackle Undead).
    bool IsCastStopperOrCrowdControl(SpellInfo const* info)
    {
        if (!info)
            return false;

        bool control = false;
        bool damage = false;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            // Silencing Shot carries weapon damage as well: the stop of the cast decides
            if (effect.Effect == SPELL_EFFECT_INTERRUPT_CAST)
                return true;

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
                    return true;
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_MOD_STUN:
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
        return control && !damage;
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
}

float ThreatMultiplier::GetValue(Action* action)
{
    if (AI_VALUE(bool, "neglect threat"))
    {
        return 1.0f;
    }

    if (!action || action->getThreatType() == Action::ActionThreatType::None)
        return 1.0f;

    if (!AI_VALUE(bool, "group"))
        return 1.0f;

    // Holding threat is the tank's job: compared with an off-tank's lower threat it would read far over 100.
    if (PlayerbotAI::IsTank(bot))
        return 1.0f;

    // [ranged_audit_b30] A kick or a crowd control is never held for threat: it stops the cast that kills the group,
    // and its own threat is small. Build 30 logged the hold on counterspell 84, fear 82, silencing shot 78,
    // kick 59 and wind shear 57 times (one line per bot per 10 s); brs-h: a Mana Burn killed the elemental shaman.
    if (CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action))
    {
        uint32 const spellId = AI_VALUE2(uint32, "spell id", spellAction->getSpell());
        if (IsCastStopperOrCrowdControl(sSpellMgr->GetSpellInfo(spellId)))
            return 1.0f;

        // [b31_shared] The Art of War makes Exorcism instant for 15 s: a held proc is lost. 199 of 203 L80 Exorcism
        // refusals were this multiplier.
        if (spellAction->getSpell() == "exorcism" && botAI->HasAura("the art of war", bot))
            return 1.0f;
    }

    bool const careful = AI_VALUE(PartyStrength, "party strength").CarefulThreat();
    if (action->getThreatType() == Action::ActionThreatType::Aoe)
    {
        uint8 threat = AI_VALUE2(uint8, "threat", "aoe");
        if (threat >= (careful ? CAREFUL_AOE_THREAT_LIMIT : AOE_THREAT_LIMIT))
            return 0.0f;
    }

    // A mob already hitting this bot is not held off by its silence: the bot fights it until the tank takes it.
    // Stockade 2026-10-24, a rogue under three mobs the tank had no threat on read 100 % and stood 25 s idle.
    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->GetVictim() == bot)
        return 1.0f;

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
    if (target && target->IsAlive() && !target->GetGUID().IsPlayer())
    {
        float const projected = ThreatLead::ProjectedShare(botAI, target);
        if (projected >= 0.0f)
        {
            float const limit = ThreatLead::PullOverPoint(bot, target) *
                                (careful ? ThreatLead::CAREFUL_HOLD_SHARE : ThreatLead::HOLD_SHARE);
            if (projected < limit)
                return 1.0f;

            uint32 const now = getMSTime();
            if (!_loggedMs || getMSTimeDiff(_loggedMs, now) >= HOLD_LOG_MS)
            {
                _loggedMs = now;
                LOG_INFO("playerbots", "[threat-hold] {} holds {} on {} {}: {:.0f} % of the holder's threat after the "
                         "next cast, limit {:.0f} %", bot->GetName(), action->getName(), target->GetName(),
                         target->GetEntry(), projected * 100.0f, limit * 100.0f);
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
