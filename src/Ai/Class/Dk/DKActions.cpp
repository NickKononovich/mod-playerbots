/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DKActions.h"
#include "DKTriggers.h"
#include "Duration.h"
#include "GenericSpellActions.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TargetValue.h"  // [b36_tankaoe] IsCrowdControlled

// [b36_tankaoe] the tank's pile: a mob this close to the tank stands in Death and Decay's 10 yd round it
constexpr float DK_TANK_DND_PILE_RANGE = 5.0f;

Unit* CastDeathAndDecayAction::GetTarget()
{
    Unit* target = CastSpellAction::GetTarget();
    if (!bot->IsInCombat() || !PlayerbotAI::IsTank(bot))
        return target;

    if (target && target->IsAlive() && bot->IsWithinDistInMap(target, DK_TANK_DND_PILE_RANGE))
        return target;

    // the closest mob on the tank's spot (cached "attackers", no scan); none: the target as before
    Unit* closest = nullptr;
    float closestDistance = DK_TANK_DND_PILE_RANGE;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* mob = botAI->GetUnit(guid);
        if (!mob || !mob->IsAlive() || mob->GetMap() != bot->GetMap() || IsCrowdControlled(mob))
            continue;

        float const distance = bot->GetDistance(mob);
        if (distance <= closestDistance)
        {
            closest = mob;
            closestDistance = distance;
        }
    }

    return closest ? closest : target;
}

std::vector<NextAction> CastDeathchillAction::getPrerequisites()
{
    return NextAction::merge({ NextAction("frost presence") },
                             CastSpellAction::getPrerequisites());
}

std::vector<NextAction> CastUnholyMeleeSpellAction::getPrerequisites()
{
    return NextAction::merge({ NextAction("unholy presence") },
                             CastMeleeSpellAction::getPrerequisites());
}

std::vector<NextAction> CastFrostMeleeSpellAction::getPrerequisites()
{
    return NextAction::merge({ NextAction("frost presence") },
                             CastMeleeSpellAction::getPrerequisites());
}

std::vector<NextAction> CastBloodMeleeSpellAction::getPrerequisites()
{
    return NextAction::merge({ NextAction("blood presence") },
                             CastMeleeSpellAction::getPrerequisites());
}

bool CastPestilenceAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    // Pestilence spreads the diseases on the target; without both of ours it wastes the Blood rune.
    Unit* target = GetTarget();
    return target && botAI->HasAura("blood plague", target, false, true) &&
           botAI->HasAura("frost fever", target, false, true);
}

bool CastRaiseDeadAction::Execute(Event event)
{
    const bool result = CastBuffSpellAction::Execute(event);

    if (!result)
        return false;

    // [b31_deep] Raise Dead (46584) starts its cooldown when the ghoul is gone (SPELL_ATTR0_COOLDOWN_ON_EVENT) and
    // spell_dk_raise_dead clears it at the summon. With Master of Ghouls the "no pet" trigger waits for the pet, so the
    // core's cooldown (Night of the Dead: 90 s instead of 180 s) is kept: the forced 3 min cost the Unholy ghoul 90 s.
    // The temporary ghoul (a guardian, no pet) is not seen by "no pet": the cooldown is set now, with the talent mods.
    if (bot->HasAura(SPELL_DK_MASTER_OF_GHOULS))
        return true;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return true;

    int32 cooldown = int32(spellInfo->CategoryRecoveryTime ? spellInfo->CategoryRecoveryTime : spellInfo->RecoveryTime);
    bot->ApplySpellMod(spellId, SPELLMOD_COOLDOWN, cooldown);
    if (cooldown > 0)
        bot->AddSpellCooldown(spellId, 0, uint32(cooldown));

    return true;
}

bool CastMarkOfBloodAction::isUseful()
{
    // [b31_deep] the rune first, then the boss (a 3 min cooldown), then the debuff checks
    if (!HasReadyRunes(bot, 1, 0, 0))
        return false;

    Unit* target = GetTarget();
    Creature* creature = target ? target->ToCreature() : nullptr;
    if (!creature || !(creature->IsDungeonBoss() || creature->isWorldBoss() ||
                       creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS))
        return false;

    return CastDebuffSpellAction::isUseful();
}

Unit* CastHysteriaAction::GetTarget()
{
    // [rot2] a led bot casts Hysteria on another member only by the leader's assignment ("leader external"): its own
    // pick is itself
    bool burn = false;
    if (LeaderBurnRule(bot, burn))
        return bot->HasAura(49016) ? nullptr : bot;

    Group* group = bot->GetGroup();
    if (!group)
    {
        if (!bot->HasAura(49016))
            return bot;
        return nullptr;
    }

    // Raid utility coordinator: the best physical DPS no other death knight is planned for.
    Unit* assigned = AI_VALUE(Unit*, "utility buff target");
    if (assigned && !assigned->HasAura(49016))
        return assigned;

    Unit* rangedDps = nullptr;
    Unit* tank = nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive())
            continue;

        if (member->GetMap() != bot->GetMap() || bot->GetDistance(member) > sPlayerbotAIConfig.SpellDistance)
            continue;

        // Skip if already has hysteria
        if (member->HasAura(49016))
            continue;

        // Priority 1: Melee DPS
        if (PlayerbotAI::IsMelee(member) && PlayerbotAI::IsDps(member))
            return member;

        // Priority 2: Ranged DPS (physical, not casters)
        if (!rangedDps && PlayerbotAI::IsRanged(member) && PlayerbotAI::IsDps(member) && !PlayerbotAI::IsCaster(member))
            rangedDps = member;

        // Priority 3: Tank
        if (!tank && PlayerbotAI::IsTank(member))
            tank = member;
    }

    if (rangedDps)
        return rangedDps;

    if (tank)
        return tank;

    // Fallback to self if no hysteria
    if (!bot->HasAura(49016))
        return bot;

    return nullptr;
}

bool CastRaiseAllyAction::isUseful()
{
    Unit* target = GetTarget();
    Player* player = target ? target->ToPlayer() : nullptr;
    return player && IsRealPlayer(player) && bot->GetDistance(player) <= sPlayerbotAIConfig.SpellDistance &&
           CastSpellAction::isUseful();
}

bool CastBloodTapForDefenseAction::isUseful()
{
    // [bear_aoe_threat] a Blood or Death rune ready: Rune Tap / Vampiric Blood can be cast without it
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        RuneType const type = bot->GetCurrentRune(i);
        if ((type == RUNE_BLOOD || type == RUNE_DEATH) && !bot->GetRuneCooldown(i))
            return false;
    }

    // only for a defensive that waits on the rune
    bool waiting = false;
    for (char const* defensive : {"rune tap", "vampiric blood"})
    {
        uint32 const spellId = AI_VALUE2(uint32, "spell id", defensive);
        if (spellId && bot->HasSpell(spellId) && !bot->HasSpellCooldown(spellId))
            waiting = true;
    }

    return waiting && CastBuffSpellAction::isUseful();
}

// [b31_dk] A living healer of the group on the bot's map (other than the bot). Only reached from
// CastDeathPactAction::isUseful below 15 % health with the permanent ghoul, so the group walk is rare.
static bool HasAliveGroupHealer(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap())
            continue;

        if (PlayerbotAI::IsHeal(member))
            return true;
    }

    return false;
}

bool CastDeathPactAction::isUseful()
{
    if (!CastBuffSpellAction::isUseful())
        return false;

    // [b31_dk] no Master of Ghouls: the ghoul is the temporary one Raise Dead summoned for this (critical health)
    if (!bot->HasAura(SPELL_DK_MASTER_OF_GHOULS))
        return true;

    // the permanent ghoul: only about to die with nobody to heal; Icebound Fortitude / Anti-Magic Shell go first
    // (above it on "critical health")
    return bot->GetHealthPct() < DK_DEATH_PACT_PERMANENT_GHOUL_HEALTH_PCT && !HasAliveGroupHealer(bot);
}

bool CastIceboundFortitudeAction::isUseful()
{
    // [b31_dk] Vampiric Blood already running: Icebound Fortitude waits for the next hit unless the tank is low
    if (bot->HasAura(SPELL_DK_VAMPIRIC_BLOOD) && bot->GetHealthPct() >= DK_ICEBOUND_FORTITUDE_HEALTH_PCT)
        return false;

    return CastBuffSpellAction::isUseful();
}

bool CastVampiricBloodAction::isUseful()
{
    // [b31_dk] Icebound Fortitude already running: Vampiric Blood is kept for after it unless the tank is low
    if (bot->HasAura(SPELL_DK_ICEBOUND_FORTITUDE) && bot->GetHealthPct() >= DK_ICEBOUND_FORTITUDE_HEALTH_PCT)
        return false;

    return CastBuffSpellAction::isUseful();
}
