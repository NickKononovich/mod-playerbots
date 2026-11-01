/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ThreatStrategy.h"
#include "AttackerCountValues.h"
#include "Creature.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "LeaderCommand.h"  // [rot2]
#include "LeaderCommandState.h"  // [rot3]
#include "Log.h"
#include "Map.h"
#include "PartyStrengthValue.h"
#include "Playerbots.h"
#include "RtiValue.h"  // [rot1] CrowdControlWouldBreak
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "ThreatValues.h"
#include "Timer.h"

#include <algorithm>
#include <vector>

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

    float AreaRadius(Player* bot, SpellInfo const* info, std::string const& spell)
    {
        if (spell == "starfall")
            return STARFALL_RADIUS;

        float radius = 0.0f;
        if (info)
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect())
                    radius = std::max(radius, effect.CalcRadius(bot));

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
    bool IsFriendlySpell(Player* bot, SpellInfo const* info, Unit* target)
    {
        if (target && target != bot && bot->IsFriendlyTo(target))
            return true;

        if (!info)
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
            if (AreaHolds(spellAction, spellInfo, careful))
                return 0.0f;
        }
        else
        {
            uint8 threat = AI_VALUE2(uint8, "threat", "aoe");
            if (threat >= (careful ? CAREFUL_AOE_THREAT_LIMIT : AOE_THREAT_LIMIT))
                return 0.0f;
        }
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

// [rot1] An area spell hits every mob in its radius, not only the bot's target: the old gate read the "aoe" value
// (the "attackers" within 12 yd of the bot or its target) and Starfall, a Single spell, only its target. strat-h pull
// 10 (8 of the druid's 16 aggro losses): Starfall and Hurricane on adds of the boss pack the tank had no lead on yet.
// Every mob the group fights (on a member's threat list) inside the spell's area is read; the current target is left
// to the single-target hold below (the tank's focus is on it), every other mob takes the area limit.
bool ThreatMultiplier::AreaHolds(CastSpellAction* action, SpellInfo const* info, bool careful)
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
    uint8 const limit = careful ? CAREFUL_AOE_THREAT_LIMIT : AOE_THREAT_LIMIT;

    std::vector<ObjectGuid> seen;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
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

            if (mob->IsControlledByPlayer() || mob->IsInEvadeMode() || mob->GetVictim() == bot ||
                FledMobs::IsRunning(mob) || !center->IsWithinDist(mob, radius, false))
                continue;

            uint8 const share = ThreatLead::TankShare(bot, mob);
            if (share < limit)
                continue;

            uint32 const now = getMSTime();
            if (!_aoeLoggedMs || getMSTimeDiff(_aoeLoggedMs, now) >= HOLD_LOG_MS)
            {
                _aoeLoggedMs = now ? now : 1;
                Unit* victim = mob->GetVictim();
                LOG_INFO("playerbots", "[rot1] [aoe-threat] {} holds {}: {} {} ({:.1f} yd from the {} centre, on {}) "
                         "at {} % of the tank's threat, limit {} % in {:.0f} yd", bot->GetName(), spell, mob->GetName(),
                         mob->GetEntry(), center->GetExactDist2d(mob), selfCentred ? "own" : "target",
                         victim ? victim->GetName() : "nobody", share, limit, radius);
            }

            return true;
        }
    }

    return false;
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

    // [rot2] Where an area action lands and how far it reaches (the rules of ThreatMultiplier::AreaHolds).
    bool AreaFootprint(Player* bot, CastSpellAction* action, SpellInfo const* info, Unit* mainTarget, Position& center,
                       float& radius)
    {
        bool const selfCentred = !info || info->GetMaxRange(false, bot) <= 0.0f;
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

void LeaderCommandMultiplier::Refresh()
{
    uint32 const now = getMSTime();
    if (_readMs && getMSTimeDiff(_readMs, now) < COMMAND_CACHE_MS)
        return;

    _readMs = now ? now : 1;
    LeaderCommandState::State const state = LeaderCommandState::Read(bot, _command);
    _led = state != LeaderCommandState::State::NotLed;
    _hasCommand = state == LeaderCommandState::State::Fresh;

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
        name == "interrupt assigned cast")
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
        bool const manaDispersion =
            name == "dispersion" && bot->GetPowerPct(POWER_MANA) < static_cast<float>(sPlayerbotAIConfig.LowMana);
        if (Emergency() || manaDispersion || LeaderCommandState::PreSaveAssigned(bot))
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
        (PlayerbotAI::IsHeal(bot) && (name == "avenging wrath" || name == "shadowfiend")) ||
        (name == "shadowfiend" && bot->GetPowerPct(POWER_MANA) < static_cast<float>(sPlayerbotAIConfig.MediumMana)) ||
        (name == "mirror image" && AI_VALUE(uint8, "my attacker count") > 0);
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
    if (IsFriendlySpell(bot, info, target))
        return 1.0f;

    bool const aoe = action->getThreatType() == Action::ActionThreatType::Aoe;
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
    if (!AreaFootprint(bot, spellAction, info, AI_VALUE(Unit*, "current target"), center, radius))
        return 1.0f;

    if (!LeaderCommand::AoeAllowed(bot, center))
    {
        Log(action,
            _command.aoe == Leader::AoeRule::Forbidden ? "the leader forbids AoE" : "AoE outside the leader's pack");
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
