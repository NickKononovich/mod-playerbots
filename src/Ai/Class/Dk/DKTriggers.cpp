/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DKTriggers.h"
#include "GenericTriggers.h"
#include "LeaderCommand.h"  // [rot2]
#include "LeaderCommandState.h"  // [rot3]
#include "Map.h"  // [b35_uh] IsGargoyleFight
#include "Pet.h"  // [b31_deep] GhoulFrenzyTrigger
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include <string>

// [rot2]
bool LeaderBurnRule(Player* bot, bool& burn)
{
    // [rot3] a led bot without a fresh command: no burn cooldown of its own (no fallback to the boss rule)
    Leader::MemberEvents command;
    LeaderCommandState::State const state = LeaderCommandState::Read(bot, command);
    if (state == LeaderCommandState::State::NotLed)
        return false;

    burn = state == LeaderCommandState::State::Fresh && command.intensity == Leader::Intensity::Burn &&
           command.attack != Leader::AttackEvent::StopDps;
    return true;
}

bool DKPresenceTrigger::IsActive()
{
    // [b31_dk] by aura id; a DK below level 55 cannot exist, so no "known" check is needed for Blood Presence
    return !bot->HasAura(SPELL_DK_BLOOD_PRESENCE) && !bot->HasAura(SPELL_DK_FROST_PRESENCE) &&
           !bot->HasAura(SPELL_DK_UNHOLY_PRESENCE);
}

bool KillingMachineTrigger::IsActive() { return bot->HasAura(SPELL_DK_KILLING_MACHINE_PROC); }  // [b31_dk]

// [b31_deep]
bool HasReadyRunes(Player* bot, uint8 blood, uint8 frost, uint8 unholy)
{
    uint8 ready[NUM_RUNE_TYPES] = {};
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (!bot->GetRuneCooldown(i))
            ++ready[bot->GetCurrentRune(i)];
    }

    uint8 missing = 0;
    missing += blood > ready[RUNE_BLOOD] ? blood - ready[RUNE_BLOOD] : 0;
    missing += frost > ready[RUNE_FROST] ? frost - ready[RUNE_FROST] : 0;
    missing += unholy > ready[RUNE_UNHOLY] ? unholy - ready[RUNE_UNHOLY] : 0;
    return missing <= ready[RUNE_DEATH];
}

bool RuneStrikeTrigger::IsActive()
{
    // [b31_deep] runic power and the dodge / parry aura state first (field reads), then the queued swing
    if (bot->GetPower(POWER_RUNIC_POWER) < RUNE_STRIKE_RUNIC_POWER || !bot->HasAuraState(AURA_STATE_DEFENSE))
        return false;

    Spell* queued = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
    if (queued && queued->GetSpellInfo()->Id == SPELL_DK_RUNE_STRIKE)
        return false;

    return bot->HasSpell(SPELL_DK_RUNE_STRIKE);
}

bool BloodRuneReadyTrigger::IsActive()
{
    // [b31_deep] a ready Blood rune in either slot (a Death rune does not count)
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (bot->GetCurrentRune(i) == RUNE_BLOOD && !bot->GetRuneCooldown(i))
            return true;
    }
    return false;
}

bool FrostStrikeRunicPowerTrigger::IsActive()
{
    return bot->GetPower(POWER_RUNIC_POWER) >= FROST_STRIKE_CAP_RUNIC_POWER;  // [b31_deep]
}

bool GhoulFrenzyTrigger::IsActive()
{
    // [b31_deep] our ghoul (the Master of Ghouls pet) without our Ghoul Frenzy, the spell off cooldown, and an Unholy
    // or Death rune for it or Blood Tap ready to make one
    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive() || !bot->IsInCombat() || pet->HasAura(SPELL_DK_GHOUL_FRENZY, bot->GetGUID()))
        return false;

    if (!bot->HasSpell(SPELL_DK_GHOUL_FRENZY) || bot->HasSpellCooldown(SPELL_DK_GHOUL_FRENZY))
        return false;

    if (HasReadyRunes(bot, 0, 0, 1))
        return true;

    return bot->HasSpell(SPELL_DK_BLOOD_TAP) && !bot->HasSpellCooldown(SPELL_DK_BLOOD_TAP);
}

bool RaiseDeadBoostTrigger::IsActive()
{
    // [b31_deep] the glyph and the cooldown first (aura and cooldown map reads), the fight test last
    if (!bot->HasAura(SPELL_DK_GLYPH_OF_RAISE_DEAD) || bot->GetPet())
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "raise dead");
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    return IsGargoyleFight(botAI);
}

bool UnholyPresenceAfterGargoyleTrigger::IsActive()
{
    if (!bot->HasAura(SPELL_DK_UNHOLY_PRESENCE))
        return false;

    // [b31_deep] the Gargoyle out: its cooldown started less than GARGOYLE_DURATION_MS ago
    if (bot->HasSpell(SPELL_DK_SUMMON_GARGOYLE) &&
        bot->GetSpellCooldownDelay(SPELL_DK_SUMMON_GARGOYLE) > GARGOYLE_COOLDOWN_MS - GARGOYLE_DURATION_MS)
        return false;

    // waiting for its fight with the runic power for it: Unholy Presence was its prerequisite
    return !(bot->GetPower(POWER_RUNIC_POWER) >= GARGOYLE_RUNIC_POWER && IsGargoyleWaiting(botAI));
}

bool ArmyOfTheDeadTrigger::IsActive()
{
    // [b31_deep] the cooldown and the runes first, the boss test last
    uint32 const spellId = AI_VALUE2(uint32, "spell id", "army of the dead");
    if (!spellId || bot->HasSpellCooldown(spellId) || !bot->IsInCombat() || !HasReadyRunes(bot, 1, 1, 1))
        return false;

    // [rot2] a led bot: under the leader's Burn only
    bool burn = false;
    if (LeaderBurnRule(bot, burn))
        return burn;

    Unit* target = AI_VALUE(Unit*, "current target");
    Creature* creature = target && target->IsAlive() ? target->ToCreature() : nullptr;
    return creature && (creature->IsDungeonBoss() || creature->isWorldBoss() ||
                        creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS);
}

bool PestilenceGlyphTrigger::IsActive()
{
    if (!SpellTrigger::IsActive())
    {
        return false;
    }
    if (!bot->HasAura(SPELL_DK_GLYPH_OF_DISEASE))
    {
        return false;
    }
    Aura* blood_plague = botAI->GetAura("blood plague", GetTarget(), true, true);
    Aura* frost_fever = botAI->GetAura("frost fever", GetTarget(), true, true);
    // [b31_dk] the same window as DKDiseaseRefreshMultiplier, which holds Icy Touch / Plague Strike for this refresh
    if ((blood_plague && blood_plague->GetDuration() <= DK_DISEASE_REFRESH_MS) ||
        (frost_fever && frost_fever->GetDuration() <= DK_DISEASE_REFRESH_MS))
    {
        return true;
    }
    return false;
}

// Based on runeSlotTypes
bool HighBloodRuneTrigger::IsActive()
{
    return bot->GetRuneCooldown(0) <= 2000 && bot->GetRuneCooldown(1) <= 2000;
}

bool HighFrostRuneTrigger::IsActive()
{
    return bot->GetRuneCooldown(4) <= 2000 && bot->GetRuneCooldown(5) <= 2000;
}

bool HighUnholyRuneTrigger::IsActive()
{
    return bot->GetRuneCooldown(2) <= 2000 && bot->GetRuneCooldown(3) <= 2000;
}

bool NoRuneTrigger::IsActive()
{
    for (uint32 i = 0; i < MAX_RUNES; ++i)
    {
        if (!bot->GetRuneCooldown(i))
            return false;
    }
    return true;
}

bool DesolationTrigger::IsActive()
{
    return bot->HasAura(66817) && BuffTrigger::IsActive();
}

bool DeathGripFarTargetTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || bot->GetExactDist(target) < MIN_GRIP_DISTANCE)
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "death grip");
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    if (target->IsNonMeleeSpellCast(false) || target->HasUnitState(UNIT_STATE_FLEEING))
        return true;

    return target->IsInCombat() && !target->isMoving();
}

bool AntiMagicShellTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || target->GetTarget() != bot->GetGUID() || !target->IsNonMeleeSpellCast(true))
        return false;

    Spell* spell = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell)
        spell = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

    SpellInfo const* spellInfo = spell ? spell->GetSpellInfo() : nullptr;
    return spellInfo && (spellInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_MAGIC);
}

// [rot_b31_pdd]
bool IsGargoyleFight(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsInCombat())
        return false;

    // [rot2] a led bot: the leader's Burn is the cooldown window (LeaderBurnRule)
    bool burn = false;
    if (LeaderBurnRule(bot, burn))
        return burn;

    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!target || !target->IsAlive())
        return false;

    if (target->ToPlayer() || ai::raidutil::IsBurstWindow(botAI))
        return true;

    // a boss pull waits the few seconds a shaman of the group needs for Bloodlust (the Gargoyle keeps its haste)
    if (ai::raidutil::ShouldHoldBurstForLust(botAI))
        return false;

    Creature* creature = target->ToCreature();
    if (!creature)
        return false;

    if (creature->IsDungeonBoss() || creature->isWorldBoss() ||
        creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS)
        return true;

    if (!creature->isElite())
        return false;

    // [b35_uh] inside a dungeon or raid trash takes a pack lasting GARGOYLE_INSTANCE_PACK_LIFETIME_S (the next boss
    // gets the Gargoyle otherwise); the open world keeps the single elite's lifetime
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return botAI->GetAiObjectContext()->GetValue<float>("estimated lifetime", "current target")->Get() >=
               GARGOYLE_MIN_LIFETIME_S;

    float const dps = botAI->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get();
    if (dps <= 0.0f)
        return false;

    // the live attackers' health over the group's dps; stops counting once the pack is long enough
    float const needHealth = GARGOYLE_INSTANCE_PACK_LIFETIME_S * dps;
    float health = 0.0f;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* mob = botAI->GetUnit(guid);
        if (!mob || !mob->IsAlive() || mob->GetMap() != map)
            continue;

        health += static_cast<float>(mob->GetHealth());
        if (health >= needHealth)
            return true;
    }
    return false;
}

// [b35_uh]
uint8 CountReadyRunes(Player* bot)
{
    uint8 ready = 0;
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (!bot->GetRuneCooldown(i))
            ++ready;
    }
    return ready;
}

// [b35_uh]
bool IsDKBossFight(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsInCombat())
        return false;

    // [rot2] a led bot: the leader's Burn is the cooldown window (LeaderBurnRule)
    bool burn = false;
    if (LeaderBurnRule(bot, burn))
        return burn;

    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!target || !target->IsAlive())
        return false;

    if (target->ToPlayer())
        return true;

    Creature* creature = target->ToCreature();
    return creature && (creature->IsDungeonBoss() || creature->isWorldBoss() ||
                        creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS);
}

// [b35_uh]
bool IsGargoyleOut(Player* bot)
{
    return bot->HasSpell(SPELL_DK_SUMMON_GARGOYLE) &&
           bot->GetSpellCooldownDelay(SPELL_DK_SUMMON_GARGOYLE) > GARGOYLE_COOLDOWN_MS - GARGOYLE_DURATION_MS;
}

// [b35_uh]
bool IsGargoyleUnavailable(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->HasSpell(SPELL_DK_SUMMON_GARGOYLE))
        return true;

    uint32 const delay = bot->GetSpellCooldownDelay(SPELL_DK_SUMMON_GARGOYLE);
    if (!delay || delay > GARGOYLE_COOLDOWN_MS - GARGOYLE_DURATION_MS)
        return false;  // ready (waiting for its fight or its runic power) or out

    // back only after the current target is dead
    return static_cast<float>(delay) / IN_MILLISECONDS >
           botAI->GetAiObjectContext()->GetValue<float>("estimated lifetime", "current target")->Get();
}

bool EmpowerRuneWeaponUnderGargoyleTrigger::IsActive()
{
    // [b35_uh] the runes and the cooldown first (field and map reads), the fight tests last
    if (CountReadyRunes(bot) > ERW_MAX_READY_RUNES || !bot->HasSpell(SPELL_DK_EMPOWER_RUNE_WEAPON) ||
        bot->HasSpellCooldown(SPELL_DK_EMPOWER_RUNE_WEAPON))
        return false;

    if (!IsDKBossFight(botAI))
        return false;

    return IsGargoyleOut(bot) || IsGargoyleUnavailable(botAI);
}

bool NoRuneBossTrigger::IsActive()
{
    // [b35_uh]
    if (CountReadyRunes(bot) || !bot->HasSpell(SPELL_DK_EMPOWER_RUNE_WEAPON) ||
        bot->HasSpellCooldown(SPELL_DK_EMPOWER_RUNE_WEAPON))
        return false;

    return IsDKBossFight(botAI);
}

bool ArmyOfTheDeadUnderGargoyleTrigger::IsActive()
{
    // [b35_uh] the Gargoyle test first (a cooldown map read), then the cooldown / runes / boss of the base trigger
    if (!IsGargoyleOut(bot) && !IsGargoyleUnavailable(botAI))
        return false;

    return ArmyOfTheDeadTrigger::IsActive();
}

bool ArmyOfTheDeadLastFrostRuneTrigger::IsActive()
{
    // [b35_uh] the Gargoyle out and Empower Rune Weapon spent (cooldown map reads)
    if (!IsGargoyleOut(bot) || !bot->HasSpell(SPELL_DK_EMPOWER_RUNE_WEAPON) ||
        !bot->HasSpellCooldown(SPELL_DK_EMPOWER_RUNE_WEAPON))
        return false;

    // exactly one ready Frost rune that is not a Death rune (wowsims currentNonDeathRuneCount RuneFrost == 1)
    uint8 frost = 0;
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (bot->GetCurrentRune(i) == RUNE_FROST && !bot->GetRuneCooldown(i))
            ++frost;
    }
    if (frost != 1)
        return false;

    if (!ArmyOfTheDeadTrigger::IsActive())
        return false;

    // below the disease refreshes: both of ours on the target with more than DK_DISEASE_REFRESH_MS left
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    Aura* frostFever = botAI->GetAura("frost fever", target, true, true);
    Aura* bloodPlague = botAI->GetAura("blood plague", target, true, true);
    return frostFever && bloodPlague && frostFever->GetDuration() > DK_DISEASE_REFRESH_MS &&
           bloodPlague->GetDuration() > DK_DISEASE_REFRESH_MS;
}

bool IsGargoyleWaiting(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "summon gargoyle")->Get();
    return spellId && bot->HasSpell(spellId) && !bot->HasSpellCooldown(spellId) && IsGargoyleFight(botAI);
}

bool SummonGargoyleTrigger::IsActive()
{
    // the runic power first (one field read), the fight test only with 60 in hand
    return bot->GetPower(POWER_RUNIC_POWER) >= GARGOYLE_RUNIC_POWER && IsGargoyleWaiting(botAI);
}

bool HornOfWinterReadyTrigger::IsActive()
{
    constexpr uint32 HORN_OF_WINTER_RUNIC_POWER = 100;  // internal units (x10)
    if (bot->IsInCombat() || bot->IsMounted() || !bot->GetGroup() || !PlayerbotAI::IsTank(bot) ||
        bot->getStandState() != UNIT_STAND_STATE_STAND)
        return false;

    if (bot->GetPower(POWER_RUNIC_POWER) + HORN_OF_WINTER_RUNIC_POWER > bot->GetMaxPower(POWER_RUNIC_POWER))
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "horn of winter");
    return spellId && !bot->HasSpellCooldown(spellId);
}

// [rot1]
bool IsDKCooldownFight(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsInCombat())
        return false;

    // [rot2] a led bot: the leader's Burn only (IsDKBossFight answers it), not the old burst window
    bool burn = false;
    if (LeaderBurnRule(bot, burn))
        return burn;

    return IsDKBossFight(botAI) || ai::raidutil::IsBurstWindow(botAI);
}

bool DancingRuneWeaponReadyTrigger::IsActive()
{
    // the spell, cooldown and power first (map reads), the target last
    if (!bot->IsInCombat() || !bot->HasSpell(SPELL_DK_DANCING_RUNE_WEAPON) ||
        bot->HasSpellCooldown(SPELL_DK_DANCING_RUNE_WEAPON) ||
        bot->GetPower(POWER_RUNIC_POWER) < DANCING_RUNE_WEAPON_RUNIC_POWER)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return target && target->IsAlive() && bot->IsWithinMeleeRange(target);
}

bool HysteriaCooldownFightTrigger::IsActive()
{
    return HysteriaNoCooldownTrigger::IsActive() && IsDKCooldownFight(botAI);
}

bool RaiseDeadBossTrigger::IsActive() { return RaiseDeadBoostTrigger::IsActive() && IsDKCooldownFight(botAI); }

bool NoRuneLowHealthTrigger::IsActive()
{
    return bot->IsInCombat() && bot->GetHealthPct() < sPlayerbotAIConfig.LowHealth && NoRuneTrigger::IsActive();
}
