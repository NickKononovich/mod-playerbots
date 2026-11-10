/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MageTriggers.h"

#include <algorithm>  // [rot251] std::max

#include "AiFactory.h"  // [rot250] spec tab
#include "EstimatedLifetimeValue.h"  // [rot251] ExpectedLifetime
#include "Creature.h"
#include "DpsEstimate.h"  // [rot603] CastPays
#include "DynamicObject.h"
#include "GameObject.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RaidConsumables.h"
#include "RaidUtilityAssignment.h"
#include "RtiValue.h"
#include "ServerFacade.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"  // [rot250]
#include "SpellPayoff.h"  // [rot564] Read
#include "TargetValue.h"
#include "Value.h"

namespace
{
    constexpr float FROST_NOVA_CC_CHECK_RANGE = 12.0f;  // the nova reaches 10 yd around the mage
    constexpr uint32 SPELL_RITUAL_OF_REFRESHMENT_RANK_1 = 43987;
    constexpr uint32 SPELL_RITUAL_OF_REFRESHMENT_RANK_2 = 58659;
    constexpr uint32 ITEM_ARCANE_POWDER = 17020;
    constexpr uint32 GO_REFRESHMENT_PORTAL_RANK_1 = 186811;
    constexpr uint32 GO_REFRESHMENT_TABLE_RANK_1 = 186812;
    constexpr uint32 GO_REFRESHMENT_TABLE_RANK_2 = 193061;
    constexpr uint32 GO_REFRESHMENT_PORTAL_RANK_2 = 193062;
    constexpr float REFRESHMENT_TABLE_SEARCH_DISTANCE = 30.0f;
    constexpr float RITUAL_OF_REFRESHMENT_BOSS_DISTANCE = 45.0f;
    // [ritual_needs_helpers] the ritual's portal needs two other members to click it (RaidConsumables IsSoulPortal):
    // a mage that starts it alone channels on (mgt-h 2026-10-25, 20261025-115805-mgt-h-5A-40, 653-706 s: alone at the
    // entrance after the corpse run, the group 190 yd on at Selin's room, "ritual of refreshment" channelled 50 s and
    // the harness regroup read it as "meet_stuck" three times, then sent the whole group out to the entrance).
    constexpr float RITUAL_HELPER_RANGE = 20.0f;
    constexpr uint32 RITUAL_HELPERS_NEEDED = 2;

    constexpr uint32 ARCANE_POWDER_RANK_1 = 2;  // Spell.dbc reagent counts
    constexpr uint32 ARCANE_POWDER_RANK_2 = 5;

    bool CanStartRitualOfRefreshment(Player* player)
    {
        if (player->HasSpell(SPELL_RITUAL_OF_REFRESHMENT_RANK_2))
            return !player->HasSpellCooldown(SPELL_RITUAL_OF_REFRESHMENT_RANK_2) &&
                   player->HasItemCount(ITEM_ARCANE_POWDER, ARCANE_POWDER_RANK_2);

        return player->HasSpell(SPELL_RITUAL_OF_REFRESHMENT_RANK_1) &&
               !player->HasSpellCooldown(SPELL_RITUAL_OF_REFRESHMENT_RANK_1) &&
               player->HasItemCount(ITEM_ARCANE_POWDER, ARCANE_POWDER_RANK_1);
    }
}

bool RitualOfRefreshmentTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || !CanStartRitualOfRefreshment(bot))
        return false;

    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    if (!group || !map || !map->IsDungeon())
        return false;

    // [ritual_needs_helpers] two other living members close enough to click the portal
    uint32 helpers = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && member->GetMap() == map &&
            bot->GetExactDist(member) <= RITUAL_HELPER_RANGE)
            ++helpers;
    }
    if (helpers < RITUAL_HELPERS_NEEDED)
        return false;

    // One caster per group: the bot mage with the lowest GUID among those who can start the ritual now
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || member->getClass() != CLASS_MAGE || !member->IsAlive() ||
            member->IsInCombat() || member->GetMap() != map || !GET_PLAYERBOT_AI(member) ||
            !CanStartRitualOfRefreshment(member))
            continue;

        if (member->GetGUID() < bot->GetGUID())
            return false;
    }

    if (!AI_VALUE(bool, "ready check in progress"))
    {
        bool bossNear = false;
        for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets"))
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && unit->IsAlive() && !unit->IsInCombat() && ai::raidutil::IsBossUnit(unit) &&
                bot->GetDistance(unit) <= RITUAL_OF_REFRESHMENT_BOSS_DISTANCE)
            {
                bossNear = true;
                break;
            }
        }

        if (!bossNear)
            return false;
    }

    // Not while a Refreshment Table, or the portal of a ritual in progress, is already up nearby
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "nearest game objects"))
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go || !go->isSpawned() || bot->GetDistance(go) > REFRESHMENT_TABLE_SEARCH_DISTANCE)
            continue;

        switch (go->GetEntry())
        {
            case GO_REFRESHMENT_PORTAL_RANK_1:
            case GO_REFRESHMENT_TABLE_RANK_1:
            case GO_REFRESHMENT_TABLE_RANK_2:
            case GO_REFRESHMENT_PORTAL_RANK_2:
                return false;
            default:
                break;
        }
    }

    // [b43_class] not at "pull imminent" or next to a pack: only at a real rest or the run start
    std::string const blocked = RaidConsumables::RitualBlockedReason(botAI);
    if (!blocked.empty())
    {
        LOG_DEBUG("playerbots", "[b43_class] {}: ritual of refreshment waits: {}", bot->GetName(), blocked);
        return false;
    }

    return true;
}

bool NoManaGemTrigger::IsActive()
{
    static const std::vector<uint32> gemIds = {
        33312,  // Mana Sapphire
        22044,  // Mana Emerald
        8008,   // Mana Ruby
        8007,   // Mana Citrine
        5513,   // Mana Jade
        5514    // Mana Agate
    };

    for (uint32 gemId : gemIds)
    {
        if (bot->GetItemCount(gemId, false) > 0)  // false = only in bags
            return false;
    }
    return true;
}

bool ArcaneIntellectTrigger::IsActive()
{
    return BuffTrigger::IsActive() && !botAI->HasAura("arcane brilliance", GetTarget());
}

namespace
{
    // [b31_deep] any mage armor self-buff up
    bool HasAnyMageArmor(PlayerbotAI* botAI, Unit* target)
    {
        return botAI->HasAura("molten armor", target) || botAI->HasAura("mage armor", target) ||
               botAI->HasAura("ice armor", target) || botAI->HasAura("frost armor", target);
    }
}

bool MageArmorTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // [b31_deep] below level 34 (Mage Armor r1) the bot had no armor at all: the action falls back through its
    // alternatives (Ice Armor, then Frost Armor)
    if (!botAI->HasSpell("mage armor"))
        return !HasAnyMageArmor(botAI, target);

    if (botAI->HasAura("mage armor", target) || botAI->HasAura("ice armor", target) ||
        botAI->HasAura("frost armor", target))
        return false;

    if (!botAI->HasAura("molten armor", target))
        return true;

    // Molten stays up in raids; elsewhere it gives way to Mage Armor once mana runs low
    return !bot->GetMap()->IsRaid() && AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.LowMana;
}

bool MoltenArmorTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // [b31_deep] below level 62 (Molten Armor r1) "bdps" mages had no armor at all (0 Ice / Frost Armor casts in
    // two days): the action falls back through its alternatives (Mage Armor, Ice Armor, Frost Armor)
    if (!botAI->HasSpell("molten armor"))
        return !HasAnyMageArmor(botAI, target);

    if (botAI->HasAura("molten armor", target) || botAI->HasAura("ice armor", target) ||
        botAI->HasAura("frost armor", target))
        return false;

    uint8 const mana = AI_VALUE2(uint8, "mana", "self target");
    if (!botAI->HasAura("mage armor", target))
        return bot->GetMap()->IsRaid() || mana >= sPlayerbotAIConfig.LowMana;

    return bot->GetMap()->IsRaid() || mana >= sPlayerbotAIConfig.HighMana;
}

bool FrostNovaOnTargetTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;

    return botAI->HasAura(spell, target);
}

bool HealerAttackerInFrostNovaRangeTrigger::IsActive()
{
    // the qualifier stays inside the nova's 10 yd so the combat reach of a big mob does not decide it
    Unit* mob = AI_VALUE2(Unit*, "healer attacker near", "9");
    Creature* creature = mob ? mob->ToCreature() : nullptr;
    // creature_immunities is indexed by mechanic (Creature::IsImmunedToSpell); the nova's damage breaks any
    // crowd control within its 10 yd
    if (!creature || creature->IsDungeonBoss() || creature->isWorldBoss() || mob->isFrozen() ||
        mob->HasUnitState(UNIT_STATE_ROOT) ||
        creature->HasMechanicTemplateImmunity((1ULL << MECHANIC_FREEZE) | (1ULL << MECHANIC_ROOT)) ||
        IsCrowdControlledMobNear(botAI, bot, FROST_NOVA_CC_CHECK_RANGE))
        return false;

    Unit* victim = mob->GetVictim();
    return victim && mob->IsWithinMeleeRange(victim);
}

bool FrostbiteOnTargetTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;

    return botAI->HasAura(spell, target);
}

bool NoFocusMagicTrigger::IsActive()
{
    constexpr uint32 SPELL_FOCUS_MAGIC = 54646;
    if (!bot->HasSpell(SPELL_FOCUS_MAGIC))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive())
            continue;

        if (member->HasAura(SPELL_FOCUS_MAGIC, bot->GetGUID()))
            return false;
    }
    return true;
}

// [b31_deep] Cold Snap right after the first Icy Veins has run out (wowsims: haste cooldowns back to back)
bool ColdSnapTrigger::IsActive()
{
    constexpr uint32 SPELL_ICY_VEINS = 12472;
    constexpr uint32 SPELL_COLD_SNAP = 11958;
    if (!bot->HasSpell(SPELL_COLD_SNAP) || bot->HasSpellCooldown(SPELL_COLD_SNAP) ||
        !bot->HasSpell(SPELL_ICY_VEINS) || !bot->HasSpellCooldown(SPELL_ICY_VEINS) || bot->HasAura(SPELL_ICY_VEINS))
        return false;

    // [rot26] wowsims mage__frost: Cold Snap whenever Icy Veins is on cooldown (no boss-only rule)
    return BoostTrigger::IsActive();
}

const std::unordered_set<uint32> FlamestrikeNearbyTrigger::FLAMESTRIKE_SPELL_IDS = {
    2120, 2121, 8422, 8423, 10215, 10216, 27086, 42925, 42926
};

bool FlamestrikeNearbyTrigger::IsActive()
{
    // [b31_deep] the own Flamestrike is a dynamic object of the caster, never an aura on it (Unit::GetAura reads
    // m_appliedAuras only): the old lookup never fired, "flamestrike active and medium aoe" was a dead node
    for (uint32 spellId : FLAMESTRIKE_SPELL_IDS)
    {
        DynamicObject* dynObj = bot->GetDynObject(spellId);
        if (!dynObj)
            continue;

        float dist = bot->GetDistance2d(dynObj->GetPositionX(), dynObj->GetPositionY());
        if (dist <= radius)
            return true;
    }
    return false;
}

bool ImprovedScorchTrigger::IsActive()
{
    // [guide_b31_dead] maintain the debuff only with the Improved Scorch talent (ranks 1-3)
    if (!bot->HasAura(11095) && !bot->HasAura(12872) && !bot->HasAura(12873))
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;

    static const uint32 ImprovedScorchExclusiveDebuffs[] = {// Shadow Mastery
                                                            17794, 17797, 17798, 17799, 17800,
                                                            // Winter's Chill
                                                            12579};

    for (uint32 spellId : ImprovedScorchExclusiveDebuffs)
    {
        if (target->HasAura(spellId))
            return false;
    }

    // [b31_deep] wowsims fire APL: Scorch when the own Improved Scorch has <= 4 s left (maxOverlap 4 s). It used to
    // wait for the debuff to fall off completely, a gap of cast + travel every 30 s. Another caster's debuff: false.
    constexpr uint32 SPELL_IMPROVED_SCORCH_DEBUFF = 22959;
    constexpr int32 IMPROVED_SCORCH_REFRESH_MS = 4 * IN_MILLISECONDS;
    Aura* own = nullptr;
    auto const range = target->GetAppliedAuras().equal_range(SPELL_IMPROVED_SCORCH_DEBUFF);
    for (auto itr = range.first; itr != range.second; ++itr)
    {
        Aura* aura = itr->second->GetBase();
        if (aura->GetCasterGUID() != bot->GetGUID())
            return false;
        own = aura;
    }

    if (!own)
        return DebuffTrigger::IsActive();

    if (own->GetDuration() < 0 || own->GetDuration() > IMPROVED_SCORCH_REFRESH_MS)
        return false;

    // [rot452] a boss: the APL's refresh row has no lifetime condition (its fight-end rows spam Scorch). b238 rfc-h 5C
    // Rtmagab: the health / group-dps lifetime fell under the 12 s first and the debuff dropped 2-8 s before the end
    // of 4 of 8 boss fights (re-applied by the "estimated lifetime" path in Taragaman, 4 s down); boss uptime 71 %
    Creature const* creature = target->ToCreature();
    if (creature && (creature->IsDungeonBoss() || creature->isWorldBoss()))
    {
        LOG_DEBUG("playerbots", "[rot452] {} refreshes Improved Scorch on {} ({} ms left)", bot->GetName(),
                  target->GetName(), own->GetDuration());
        return true;
    }

    return DpsEstimate::DebuffPays(bot, target, AI_VALUE2(uint32, "spell id", "scorch"));  // [rot610]
}

const std::unordered_set<uint32> BlizzardChannelCheckTrigger::BLIZZARD_SPELL_IDS = {
    10,     // Blizzard Rank 1
    6141,   // Blizzard Rank 2
    8427,   // Blizzard Rank 3
    10185,  // Blizzard Rank 4
    10186,  // Blizzard Rank 5
    10187,  // Blizzard Rank 6
    27085,  // Blizzard Rank 7
    42938,  // Blizzard Rank 8
    42939   // Blizzard Rank 9
};

bool BlizzardChannelCheckTrigger::IsActive()
{
    if (Spell* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        spell && BLIZZARD_SPELL_IDS.count(spell->m_spellInfo->Id))
    {
        // [b31_deep] count the mobs inside the Blizzard (8 yd round its dynamic object), not within sight of the
        // mage: the old count cancelled the channel only once everything around was dead
        constexpr float BLIZZARD_RADIUS = 8.0f;
        DynamicObject* dynObj = bot->GetDynObject(spell->m_spellInfo->Id);
        if (!dynObj)
            return false;

        return !HasEnoughAoeTargets(botAI, dynObj->GetPosition(), BLIZZARD_RADIUS, minEnemies);
    }

    return false;
}

// [rot250] see EvocationTrigger (MageTriggers.h)
bool EvocationTrigger::IsActive()
{
    if (!AI_VALUE2(bool, "has mana", "self target") || AI_VALUE2(uint8, "mana", "self target") > MAGE_CONSERVE_MANA_PCT)
        return false;

    if (AiFactory::GetPlayerSpecTab(bot) != MAGE_TAB_ARCANE)
        return true;

    constexpr uint32 SPELL_MISSILE_BARRAGE_PROC = 44401;
    constexpr uint32 SPELL_ARCANE_BLAST_STACK = 36032;
    constexpr uint8 ARCANE_BLAST_MAX_STACKS = 4;
    if (bot->HasAura(SPELL_MISSILE_BARRAGE_PROC))
        return false;

    Aura const* stacks = bot->GetAura(SPELL_ARCANE_BLAST_STACK);
    if (stacks && stacks->GetStackAmount() >= ARCANE_BLAST_MAX_STACKS)
        return true;

    // Below 4 stacks the APL casts Arcane Blast first: Evocation only once it can no longer be paid.
    uint32 const arcaneBlast = AI_VALUE2(uint32, "spell id", "arcane blast");
    SpellInfo const* info = arcaneBlast ? sSpellMgr->GetSpellInfo(arcaneBlast) : nullptr;
    if (!info)
        return true;

    return int32(bot->GetPower(POWER_MANA)) < info->CalcPowerCost(bot, info->GetSchoolMask());
}

namespace
{
// [rot251] the APL's remainingTime: the longest ExpectedLifetime of the target and the mage's attackers
float FightRemainingSec(PlayerbotAI* botAI, Unit* target)
{
    float remaining = ExpectedLifetime(botAI, target);
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        if (Unit* mob = botAI->GetUnit(guid); mob && mob != target && mob->IsAlive())
            remaining = std::max(remaining, ExpectedLifetime(botAI, mob));
    return remaining;
}

bool FireFightEndTarget(Player* bot, Unit* target)
{
    return target && target->IsAlive() && target->IsInWorld() && bot->IsInCombat() &&
           AiFactory::GetPlayerSpecTab(bot) == MAGE_TAB_FIRE;
}
}  // namespace

namespace
{
// [rot604] the filler the mage casts when it stands: Fireball for Fire, else the stronger of Fireball / Frostbolt
uint32 MageFillerId(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint32 const fireball = AI_VALUE2(uint32, "spell id", "fireball");
    uint32 const frostbolt = AI_VALUE2(uint32, "spell id", "frostbolt");
    if (AiFactory::GetPlayerSpecTab(bot) == MAGE_TAB_FIRE || !frostbolt)
        return fireball ? fireball : frostbolt;
    if (!fireball)
        return frostbolt;
    return DpsEstimate::FillerDps(bot, target, fireball) > DpsEstimate::FillerDps(bot, target, frostbolt) ? fireball
                                                                                                           : frostbolt;
}
}  // namespace

bool FireballOverFrostboltTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive())
        return false;
    uint32 const fireball = AI_VALUE2(uint32, "spell id", "fireball");
    uint32 const frostbolt = AI_VALUE2(uint32, "spell id", "frostbolt");
    return fireball && frostbolt &&
           DpsEstimate::FillerDps(bot, target, fireball) > DpsEstimate::FillerDps(bot, target, frostbolt);
}

bool FillerOverArcaneMissilesTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || AI_VALUE2(uint32, "spell id", "arcane blast"))
        return false;
    uint32 const missiles = AI_VALUE2(uint32, "spell id", "arcane missiles");
    uint32 const id = AI_VALUE2(uint32, "spell id", filler);
    if (!missiles || !id || MageFillerId(botAI, bot, target) != id)
        return false;
    return DpsEstimate::FillerDps(bot, target, id) > DpsEstimate::FillerDps(bot, target, missiles);
}

bool FireBlastPaysTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive())
        return false;
    uint32 const fireBlast = AI_VALUE2(uint32, "spell id", "fire blast");
    if (!fireBlast || bot->HasSpellCooldown(fireBlast))
        return false;
    // within its range from where it stands (20 yd, inside the 30-35 yd it casts the fillers from): no walk for it
    SpellInfo const* info = sSpellMgr->GetSpellInfo(fireBlast);
    if (!info || !bot->IsWithinCombatRange(target, info->GetMaxRange(false, bot)))
        return false;
    return DpsEstimate::CastPays(bot, target, fireBlast, MageFillerId(botAI, bot, target));
}

bool FireBlastFightEndTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!FireFightEndTarget(bot, target))
        return false;

    uint32 const scorch = AI_VALUE2(uint32, "spell id", "scorch");
    SpellInfo const* info = scorch ? sSpellMgr->GetSpellInfo(scorch) : nullptr;
    float const scorchCastSec = info ? info->CalcCastTime(bot) / 1000.0f : 1.5f;
    return FightRemainingSec(botAI, target) <= scorchCastSec;
}

bool ScorchFightEndTrigger::IsActive()
{
    Unit* target = GetTarget();
    return FireFightEndTarget(bot, target) && FightRemainingSec(botAI, target) <= FIRE_FIGHT_END_SCORCH_SEC;
}

namespace
{
// [rot564] talent first ranks (Spell.dbc) and the core's Ignite rule (spell_mage_ignite: 8 % per rank of the crit)
constexpr uint32 SPELL_MAGE_IGNITE_RANK_1 = 11119;
constexpr uint32 SPELL_MAGE_HOT_STREAK_RANK_1 = 44445;
constexpr uint32 SPELL_MAGE_MISSILE_BARRAGE_RANK_1 = 44404;
constexpr int32 IGNITE_PCT_PER_RANK = 8;
constexpr uint32 CRIT_BONUS_PROBE = 10000;  // the core's crit bonus is a function of the damage: read as a ratio

// a talent's proc chance in percent: its spell_proc entry where it sets one, else Spell.dbc ProcChance; 0 untaken
float TalentProcChance(Player* bot, uint32 firstRank)
{
    Aura const* talent = bot->GetAuraOfRankedSpell(firstRank);
    if (!talent)
        return 0.0f;
    SpellProcEntry const* entry = sSpellMgr->GetSpellProcEntry(talent->GetId());
    return entry && entry->Chance > 0.0f ? entry->Chance : static_cast<float>(talent->GetSpellInfo()->ProcChance);
}

float CritRate(Player* bot, Unit* target, SpellInfo const* info)
{
    return std::clamp(bot->SpellDoneCritChance(target, info, info->GetSchoolMask(), BASE_ATTACK, false), 0.0f,
                      100.0f) /
           100.0f;
}

// [rot564] Ignite's damage over a fire hit's expected value (Rawr Mage Spell.cs: the crit damage x the Ignite factor
// x the crit rate): the expected hit d = b (1 + c (m - 1)) carries an Ignite of p b m c, so p m c / (1 + c (m - 1));
// p = 8 % per Ignite rank (spell_mage_ignite). 0 for a spell of no fire school or without the talent.
float IgniteShare(Player* bot, Unit* target, SpellInfo const* info)
{
    if (!info || !(info->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE) || info->HasAttribute(SPELL_ATTR2_CANT_CRIT))
        return 0.0f;
    Aura const* ignite = bot->GetAuraOfRankedSpell(SPELL_MAGE_IGNITE_RANK_1);
    if (!ignite)
        return 0.0f;
    float const p = static_cast<float>(IGNITE_PCT_PER_RANK * ignite->GetSpellInfo()->GetRank()) / 100.0f;
    float const c = CritRate(bot, target, info);
    float const m = static_cast<float>(Unit::SpellCriticalDamageBonus(bot, info, CRIT_BONUS_PROBE, target)) /
                    static_cast<float>(CRIT_BONUS_PROBE);
    return p * m * c / (1.0f + c * std::max(0.0f, m - 1.0f));
}
}

// [rot420] the Arcane Blast stacks: its damage at full stacks over its damage now
float MageAoePaysTrigger::ArcaneStackScale()
{
    // Spell.dbc 36032: arcane damage done +15 % per stack (SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, school arcane; the core
    // multiplies the amount by the stacks, and SpellDamageBonusDone applies the aura as its own factor)
    constexpr uint32 SPELL_ARCANE_BLAST_STACK = 36032;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_ARCANE_BLAST_STACK);
    uint32 const fillerId = AI_VALUE2(uint32, "spell id", Filler());
    SpellInfo const* filler = fillerId ? sSpellMgr->GetSpellInfo(fillerId) : nullptr;
    if (!info || !filler || info->StackAmount <= 1)
        return 1.0f;

    int32 perStack = 0;
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_DONE &&
            (effect.MiscValue & static_cast<int32>(filler->GetSchoolMask())))
            perStack += effect.CalcValue(bot);
    if (perStack <= 0)
        return 1.0f;

    Aura const* stacks = bot->GetAura(SPELL_ARCANE_BLAST_STACK);
    uint32 const current = stacks ? stacks->GetStackAmount() : 0;
    float const full = 100.0f + static_cast<float>(perStack) * static_cast<float>(info->StackAmount);
    float const now = 100.0f + static_cast<float>(perStack) * static_cast<float>(current);
    return std::max(1.0f, full / now);
}

// [rot564] Arcane: the Missile Barrage share of an Arcane Blast, over its direct hit. spell_mage_missile_barrage procs
// on Arcane Blast at the talent's full chance (Rawr Mage CycleDataArcane: 0.08 x rank); the proc's Arcane Missiles
// land their whole damage in the channel the proc shortens (its SPELLMOD_DURATION), worth that damage over the
// filler at its steady stacks in the same time.
float MageAoePaysTrigger::MissileBarrageShare(Unit* target, float stackScale)
{
    float const chance = TalentProcChance(bot, SPELL_MAGE_MISSILE_BARRAGE_RANK_1) / 100.0f;
    Aura const* talent = bot->GetAuraOfRankedSpell(SPELL_MAGE_MISSILE_BARRAGE_RANK_1);
    uint32 const blastId = AI_VALUE2(uint32, "spell id", "arcane blast");
    uint32 const missilesId = AI_VALUE2(uint32, "spell id", "arcane missiles");
    SpellInfo const* missilesInfo = missilesId ? sSpellMgr->GetSpellInfo(missilesId) : nullptr;
    SpellPayoff::Cast blast;
    SpellPayoff::Cast missiles;
    if (chance <= 0.0f || !talent || !blastId || !missilesInfo || missilesInfo->GetDuration() <= 0 ||
        !SpellPayoff::Read(bot, target, blastId, blast) || blast.direct <= 0.0f ||
        !SpellPayoff::Read(bot, target, missilesId, missiles) || missiles.channelMs <= 0.0f)
        return 0.0f;

    // the proc the talent triggers and its channel cut, hasted as the channel is
    float channelMs = missiles.channelMs;
    for (SpellEffectInfo const& effect : talent->GetSpellInfo()->GetEffects())
    {
        SpellInfo const* proc = effect.TriggerSpell ? sSpellMgr->GetSpellInfo(effect.TriggerSpell) : nullptr;
        if (!proc)
            continue;
        for (SpellEffectInfo const& mod : proc->GetEffects())
            if (mod.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER && mod.MiscValue == SPELLMOD_DURATION)
                channelMs += static_cast<float>(mod.CalcValue(bot)) * missiles.channelMs /
                             static_cast<float>(missilesInfo->GetDuration());
    }
    channelMs = std::max(channelMs, missiles.gcdMs);

    float const fillerDps = blast.direct * stackScale * static_cast<float>(IN_MILLISECONDS) / blast.executeMs;
    float const gain = missiles.Total() - fillerDps * channelMs / static_cast<float>(IN_MILLISECONDS);
    return gain > 0.0f ? chance * gain / blast.direct : 0.0f;
}

// [rot564] Fire: Fireball's value over its direct hit (its Ignite + the Hot Streak Pyroblasts) over the area spell's
// value over its own damage (its Ignite on its direct part: Flamestrike / Dragon's Breath / Blast Wave, none on Arcane
// Explosion). Rawr Mage CycleDataFire FBPyro: Pyroblasts per Fireball K = c^2 / (1 + c) x the Hot Streak chance (the
// talent aura's amount, spell_mage_hot_streak); an instant Pyroblast is worth its damage and Ignite over the filler
// in its GCD. b276 rfc-h 5H Rtmagad: "flamestrike lands 7926 on 2 mobs vs fireball 7957" with Fireball's hit alone.
float MageAoePaysTrigger::FireScale(Unit* target)
{
    uint32 const fireballId = AI_VALUE2(uint32, "spell id", "fireball");
    uint32 const areaId = AI_VALUE2(uint32, "spell id", aoeSpell);
    SpellInfo const* fireball = fireballId ? sSpellMgr->GetSpellInfo(fireballId) : nullptr;
    SpellInfo const* areaInfo = areaId ? sSpellMgr->GetSpellInfo(areaId) : nullptr;
    SpellPayoff::Cast filler;
    SpellPayoff::Cast area;
    if (!fireball || !areaInfo || !SpellPayoff::Read(bot, target, fireballId, filler) || filler.direct <= 0.0f ||
        !SpellPayoff::Read(bot, target, areaId, area) || area.Total() <= 0.0f)
        return 1.0f;

    float fillerFactor = 1.0f + IgniteShare(bot, target, fireball);

    uint32 const pyroId = AI_VALUE2(uint32, "spell id", "pyroblast");
    SpellInfo const* pyroInfo = pyroId ? sSpellMgr->GetSpellInfo(pyroId) : nullptr;
    Aura const* hotStreak = bot->GetAuraOfRankedSpell(SPELL_MAGE_HOT_STREAK_RANK_1);
    AuraEffect const* hotStreakChance = hotStreak ? hotStreak->GetEffect(EFFECT_0) : nullptr;
    SpellPayoff::Cast pyro;
    if (pyroInfo && hotStreakChance && SpellPayoff::Read(bot, target, pyroId, pyro))
    {
        float const c = CritRate(bot, target, fireball);
        float const k = c * c / (1.0f + c) * static_cast<float>(hotStreakChance->GetAmount()) / 100.0f;
        float const fillerDps = filler.direct * static_cast<float>(IN_MILLISECONDS) / filler.executeMs;
        float const gain = pyro.Total() + IgniteShare(bot, target, pyroInfo) * pyro.direct -
                           fillerDps * pyro.gcdMs / static_cast<float>(IN_MILLISECONDS);
        if (gain > 0.0f)
            fillerFactor += k * gain / filler.direct;
    }

    float const areaFactor = 1.0f + IgniteShare(bot, target, areaInfo) * area.direct / area.Total();
    return fillerFactor / areaFactor;
}

float MageAoePaysTrigger::FillerScale()
{
    int const tab = AiFactory::GetPlayerSpecTab(bot);
    if (tab != MAGE_TAB_FIRE && tab != MAGE_TAB_ARCANE)
        return 1.0f;
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return 1.0f;
    if (tab == MAGE_TAB_FIRE)
        return FireScale(target);
    float const stackScale = ArcaneStackScale();
    return stackScale + MissileBarrageShare(target, stackScale);
}

std::string const MageAoePaysTrigger::Filler()
{
    switch (AiFactory::GetPlayerSpecTab(bot))
    {
        case MAGE_TAB_ARCANE:
            return "arcane blast";
        case MAGE_TAB_FIRE:
            return "fireball";
        default:
            return "frostbolt";
    }
}
