/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DKTriggers.h"
#include "GenericTriggers.h"
#include "Group.h"  // [rot10]
#include "LeaderCommand.h"  // [rot2]
#include "LeaderCommandState.h"  // [rot3]
#include "Map.h"  // [b35_uh] IsGargoyleFight
#include "Pet.h"  // [b31_deep] GhoulFrenzyTrigger
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuras.h"  // [rot21]
#include <algorithm>     // [rot21]
#include "SpellInfo.h"
#include "SpellMgr.h"  // [rot10]
#include "StringFormat.h"  // [rot51]
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

namespace
{
constexpr uint32 SPELL_DK_FREEZING_FOG = 59052;
constexpr uint32 SPELL_DK_UNBREAKABLE_ARMOR = 51271;

// [rot21] the bot's own Frost Fever on its current target (ms left, 0 when none)
int32 OwnFrostFeverMs(PlayerbotAI* botAI)
{
    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    Aura* fever = target ? botAI->GetAura("frost fever", target, true, true) : nullptr;
    return fever ? fever->GetDuration() : 0;
}

// [rot21] the longest rune grace of a ready rune of this type (how long it has stood ready)
uint32 RuneGraceMs(Player* bot, RuneType type)
{
    uint32 grace = 0;
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (bot->GetCurrentRune(i) == type && !bot->GetRuneCooldown(i))
            grace = std::max(grace, bot->GetGracePeriod(i));
    }
    return grace;
}
}  // namespace

// [rot21]
bool KillingMachineFrostStrikeTrigger::IsActive()
{
    return bot->HasAura(SPELL_DK_KILLING_MACHINE_PROC) && RuneGraceMs(bot, RUNE_FROST) <= FROST_KM_RUNE_GRACE_MS &&
           RuneGraceMs(bot, RUNE_UNHOLY) <= FROST_KM_RUNE_GRACE_MS && OwnFrostFeverMs(botAI) >= FROST_FF_KEEP_MS;
}

// [rot21]
bool FreezingFogFrostFeverTrigger::IsActive()
{
    return bot->HasAura(SPELL_DK_FREEZING_FOG) && OwnFrostFeverMs(botAI) >= FROST_FF_KEEP_MS;
}

// [rot21]
bool FrostBloodStrikeTrigger::IsActive()
{
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (bot->GetCurrentRune(i) == RUNE_BLOOD && !bot->GetRuneCooldown(i))
            return OwnFrostFeverMs(botAI) > FROST_BLOOD_STRIKE_FF_MS;
    }
    return false;
}

// [rot21] rune slot 0 is the left Blood slot
bool FrostPestilenceTrigger::IsActive()
{
    if (!bot->HasAura(SPELL_DK_GLYPH_OF_DISEASE) || bot->GetCurrentRune(0) != RUNE_BLOOD || bot->GetRuneCooldown(0))
        return false;

    uint32 const pestilence = AI_VALUE2(uint32, "spell id", "pestilence");
    if (!pestilence || bot->HasSpellCooldown(pestilence))
        return false;

    if (bot->HasSpell(SPELL_DK_UNBREAKABLE_ARMOR) &&
        (!bot->HasSpellCooldown(SPELL_DK_UNBREAKABLE_ARMOR) ||
         bot->GetSpellCooldownDelay(SPELL_DK_UNBREAKABLE_ARMOR) < uint32(FROST_PESTILENCE_FF_MS)))
        return false;

    int32 const fever = OwnFrostFeverMs(botAI);
    return fever > 0 && fever <= FROST_PESTILENCE_FF_MS;
}

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

// [rot51] b187 5C/5G: Blood DK tank damage 60-77 % of the sim - white remainder 95 DPS against 788, Rune Strike 7.6 a
// minute against 17.4, rune weapon / ghoul 38 DPS against 420. One line per change of the swing and cooldown state.
void RuneStrikeTrigger::TraceTankBoss()
{
    constexpr uint32 RUNIC_POWER_BUCKET = 100;  // internal units (x10): 10 runic power per step, not every point
    constexpr uint32 RUNE_WEAPON_CD_BUCKET_S = 10;
    constexpr uint32 RAISE_DEAD_CD_BUCKET_S = 30;
    if (!PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return;

    Unit* target = AI_VALUE(Unit*, "current target");
    Creature* boss = target ? target->ToCreature() : nullptr;
    if (!boss || !boss->IsDungeonBoss())
        return;

    Spell const* queued = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
    uint32 const raiseDead = AI_VALUE2(uint32, "spell id", "raise dead");
    uint32 const runeWeaponCd = bot->HasSpellCooldown(SPELL_DK_DANCING_RUNE_WEAPON)
                                    ? bot->GetSpellCooldownDelay(SPELL_DK_DANCING_RUNE_WEAPON) / IN_MILLISECONDS
                                    : 0;
    uint32 const raiseDeadCd = raiseDead && bot->HasSpellCooldown(raiseDead)
                                   ? bot->GetSpellCooldownDelay(raiseDead) / IN_MILLISECONDS
                                   : 0;
    bool burn = false;
    bool const led = LeaderBurnRule(bot, burn);
    std::string const trace = Acore::StringFormat(
        "rp {}+ defense state {} queued {} victim is target {} melee attacking {} in melee {}, dancing "
        "rune weapon known {} cd {}+ s, raise dead cd {}+ s glyph {} pet {}, gargoyle fight {} cooldown fight {} led "
        "burn {}/{}",
        bot->GetPower(POWER_RUNIC_POWER) / RUNIC_POWER_BUCKET * 10, bot->HasAuraState(AURA_STATE_DEFENSE),
        queued && queued->GetSpellInfo() ? queued->GetSpellInfo()->Id : 0, bot->GetVictim() == target,
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING), bot->IsWithinMeleeRange(target),
        bot->HasSpell(SPELL_DK_DANCING_RUNE_WEAPON), runeWeaponCd / RUNE_WEAPON_CD_BUCKET_S * RUNE_WEAPON_CD_BUCKET_S,
        raiseDeadCd / RAISE_DEAD_CD_BUCKET_S * RAISE_DEAD_CD_BUCKET_S, bot->HasAura(SPELL_DK_GLYPH_OF_RAISE_DEAD),
        bot->GetPet() != nullptr, IsGargoyleFight(botAI), IsDKCooldownFight(botAI), led, burn);
    if (trace == lastTrace)
        return;

    lastTrace = trace;
    LOG_INFO("playerbots", "[rot51] {} blood tank on {}: {}", bot->GetName(), boss->GetName(), trace);
}

bool RuneStrikeHeldForRuneWeapon(Player* bot)
{
    return bot->GetPower(POWER_RUNIC_POWER) < RUNE_STRIKE_RUNE_WEAPON_POOL_RUNIC_POWER &&
           bot->HasSpell(SPELL_DK_DANCING_RUNE_WEAPON) && !bot->HasSpellCooldown(SPELL_DK_DANCING_RUNE_WEAPON);
}

bool RuneStrikeTrigger::IsActive()
{
    TraceTankBoss();
    // [b31_deep] runic power and the dodge / parry aura state first (field reads), then the queued swing
    uint32 const runicPower = bot->GetPower(POWER_RUNIC_POWER);
    if (runicPower < RUNE_STRIKE_RUNIC_POWER || !bot->HasAuraState(AURA_STATE_DEFENSE))
        return false;

    // [rot52] wowsims blood_aggro: Rune Strike only while Dancing Rune Weapon is not ready or at 80+ runic power.
    // b188 rot51 trace: Dancing Rune Weapon ready for whole stretches while each Rune Strike took the 20 runic power
    // its 60 needed (rune weapon / ghoul damage 38 DPS against the sim's 420)
    if (RuneStrikeHeldForRuneWeapon(bot))
        return false;

    Spell* queued = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
    if (queued && queued->GetSpellInfo()->Id == SPELL_DK_RUNE_STRIKE)
        return false;

    return bot->HasSpell(SPELL_DK_RUNE_STRIKE);
}

bool FrostStrikeRunicPowerTrigger::IsActive()
{
    return bot->GetPower(POWER_RUNIC_POWER) >= FROST_STRIKE_CAP_RUNIC_POWER;  // [b31_deep]
}

bool GhoulFrenzyTrigger::IsActive()
{
    // [b31_deep] our ghoul (the Master of Ghouls pet) without our Ghoul Frenzy, the spell off cooldown, and Blood Tap
    // ready to make the Death rune for it
    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive() || !bot->IsInCombat() || pet->HasAura(SPELL_DK_GHOUL_FRENZY, bot->GetGUID()))
        return false;

    if (!bot->HasSpell(SPELL_DK_GHOUL_FRENZY) || bot->HasSpellCooldown(SPELL_DK_GHOUL_FRENZY))
        return false;

    // [rot22] only through Blood Tap (wowsims unholy_dw_ss: the Blood Tap -> Ghoul Frenzy sequence): a real Unholy rune
    // stays for Scourge Strike
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

    // a boss pull waits while the group's shaman can cast Bloodlust now (the Gargoyle keeps its haste)
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

// [rot10]
bool HornOfWinterOnPartyTrigger::IsActive()
{
    Group* group = bot->GetGroup();
    if (!group || bot->IsInCombat() || bot->IsMounted())
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "horn of winter");
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo || bot->HasSpellCooldown(spellId))
        return false;

    float const radius = spellInfo->Effects[EFFECT_0].CalcRadius(bot);
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMap() != bot->GetMap() || !bot->IsWithinDistInMap(member, radius))
            continue;

        if (!botAI->HasAura("horn of winter", member) && !botAI->HasAura("strength of earth", member))
            return true;
    }

    return false;
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
