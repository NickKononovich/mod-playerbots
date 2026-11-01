/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ShamanTriggers.h"
#include "AiFactory.h"
#include "Creature.h"
#include "FearCounterValues.h"
#include "InstanceScript.h"
#include "LeaderOrders.h"  // [lead151] AtCommandedSpot
#include "ItemTemplate.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "TargetValue.h"
#include "Timer.h"
#include "Totem.h"
#include "TotemsShamanStrategy.h"
#include "Unit.h"
#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace
{
// Out of combat in a dungeon a totem is a unit the idle mobs around it see: scholo-h 2026-10-25 pull 3, the pack the
// careful planner had refused (9 mobs) came for a Healing Stream Totem dropped near it, 26 mobs in 114 s.
bool HoldsTotemsOutOfCombat(Player* bot)
{
    Map* map = bot->GetMap();
    // [lead151] b171 20261101-144503 Oggleflint: the totem triggers fired while lead149's actions refused (a led
    // member's totems only at its commanded spot) - true-and-impossible every tick. The trigger holds too
    return map && map->IsDungeon() && (!bot->IsInCombat() || !Leader::Events::AtCommandedSpot(bot));
}

// [rot_b31_heal] The reach every totem trigger of this file already uses (bot to totem).
constexpr float TOTEM_KEEP_RANGE = 30.0f;

bool CreatedBy(uint32 spellId, uint32 const* spells, size_t count)
{
    return spellId && std::find(spells, spells + count, spellId) != spells + count;
}

// The totem summoning slots in the order Call of the Elements drops them.
constexpr uint8 TOTEM_SLOTS[] = {SUMMON_SLOT_TOTEM_EARTH, SUMMON_SLOT_TOTEM_FIRE, SUMMON_SLOT_TOTEM_WATER,
                                 SUMMON_SLOT_TOTEM_AIR};

// Slots Call of the Elements would fill: empty, or the totem out of TOTEM_KEEP_RANGE, for a slot the bot has a totem
// for. Four slot lookups, no scan.
int CountTotemSlotsToDrop(Player* bot)
{
    int count = 0;
    for (uint8 slot : TOTEM_SLOTS)
    {
        ObjectGuid const guid = bot->m_SummonSlot[slot];
        if (!guid.IsEmpty())
        {
            Creature* totem = bot->GetMap()->GetCreature(guid);
            if (totem && totem->GetDistance(bot) <= TOTEM_KEEP_RANGE)
                continue;
        }

        if ((slot == SUMMON_SLOT_TOTEM_EARTH && bot->HasSpell(SPELL_STONESKIN_TOTEM_RANK_1)) ||
            (slot == SUMMON_SLOT_TOTEM_FIRE && bot->HasSpell(SPELL_SEARING_TOTEM_RANK_1)) ||
            (slot == SUMMON_SLOT_TOTEM_WATER && bot->HasSpell(SPELL_HEALING_STREAM_TOTEM_RANK_1)) ||
            (slot == SUMMON_SLOT_TOTEM_AIR && bot->HasSpell(SPELL_NATURE_RESISTANCE_TOTEM_RANK_1)))
            ++count;
    }

    return count;
}

// [rot_b31_heal] Two or more slots to fill and Call of the Elements ready: the single "no <element> totem" drops
// wait for it (one GCD for the set instead of one per totem). lbrs-n 20261025-184229-14: on 5 of the first 40 pulls
// the enhancement shaman dropped Windfury, Strength of Earth, Healing Stream and Searing one by one on the run-in,
// before "call of the elements and enemy within melee" could fire. Not while the [fear_counter] multiplier holds
// Call of the Elements back for a Tremor Totem: the other elements are then dropped one by one as before.
bool DefersToCallOfTheElements(PlayerbotAI* botAI, Player* bot)
{
    if (!bot->IsInCombat() || !bot->HasSpell(SPELL_CALL_OF_THE_ELEMENTS) || CountTotemSlotsToDrop(bot) < 2)
        return false;

    if (botAI->HasStrategy("fear counter", BOT_STATE_COMBAT) && bot->HasSpell(FearCounter::SPELL_TREMOR_TOTEM))
    {
        FearCounterState const state =
            botAI->GetAiObjectContext()->GetValue<FearCounterState>("fear counter state")->Get();
        if (!state.victim.IsEmpty() || (state.threat & FearCounter::THREAT_TREMOR))
            return false;
    }

    return botAI->CanCastSpell(SPELL_CALL_OF_THE_ELEMENTS, bot, true);
}

// [rot_b31_heal] A totem that acts on enemies around it: Searing shoots the nearest attackable unit (TotemAI), Magma
// and Earthbind pulse on enemies, Stoneclaw taunts. Left standing out of combat it opens the next pack by itself;
// the friendly auras (Strength of Earth, Windfury, Mana Spring, Healing Stream, Tremor, ...) do not. The dungeon
// Fire Elemental is the [drift_fe_recall] case (scholo-h 20261025-150737-4 pull 6).
bool IsHostileActingTotem(Creature* totem, bool dungeon)
{
    if (!totem)
        return false;

    uint32 const spell = totem->GetUInt32Value(UNIT_CREATED_BY_SPELL);
    return CreatedBy(spell, SEARING_TOTEM, SEARING_TOTEM_COUNT) || CreatedBy(spell, MAGMA_TOTEM, MAGMA_TOTEM_COUNT) ||
           CreatedBy(spell, EARTHBIND_TOTEM, EARTHBIND_TOTEM_COUNT) ||
           CreatedBy(spell, STONECLAW_TOTEM, STONECLAW_TOTEM_COUNT) ||
           (dungeon && CreatedBy(spell, FIRE_ELEMENTAL_TOTEM, FIRE_ELEMENTAL_TOTEM_COUNT));
}
}  // namespace

namespace ai::shaman
{
// [b31_deep] See the header: known (cached "spell id") and off cooldown.
bool IsSpellReady(PlayerbotAI* botAI, std::string const& spell)
{
    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    return spellId && !botAI->GetBot()->HasSpellCooldown(spellId);
}

bool HasIdleHostileTotem(Player* bot)
{
    if (bot->IsInCombat())
        return false;

    Map* map = bot->GetMap();
    bool const dungeon = map && map->IsDungeon();
    for (uint8 slot : {SUMMON_SLOT_TOTEM_FIRE, SUMMON_SLOT_TOTEM_EARTH})
    {
        ObjectGuid const guid = bot->m_SummonSlot[slot];
        if (!guid.IsEmpty() && IsHostileActingTotem(bot->GetMap()->GetCreature(guid), dungeon))
            return true;
    }

    return false;
}

uint8 DismissIdleHostileTotems(Player* bot)
{
    if (bot->IsInCombat())
        return 0;

    Map* map = bot->GetMap();
    bool const dungeon = map && map->IsDungeon();
    uint8 dismissed = 0;
    for (uint8 slot : {SUMMON_SLOT_TOTEM_FIRE, SUMMON_SLOT_TOTEM_EARTH})
    {
        ObjectGuid const guid = bot->m_SummonSlot[slot];
        Creature* totem = guid.IsEmpty() ? nullptr : bot->GetMap()->GetCreature(guid);
        // The same unsummon a player's right click on the totem sends (WorldSession::HandleTotemDestroyed).
        if (totem && totem->IsTotem() && IsHostileActingTotem(totem, dungeon))
        {
            totem->ToTotem()->UnSummon();
            ++dismissed;
        }
    }

    return dismissed;
}
}  // namespace ai::shaman

bool MainHandWeaponNoImbueTrigger::IsActive()
{
    Item* const itemForSpell = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!itemForSpell || itemForSpell->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
        return false;

    return true;
}

bool OffHandWeaponNoImbueTrigger::IsActive()
{
    Item* const itemForSpell = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!itemForSpell)
        return false;

    uint32 invType = itemForSpell->GetTemplate()->InventoryType;
    bool allowedType = (invType == INVTYPE_WEAPON) || (invType == INVTYPE_WEAPONOFFHAND);
    if (itemForSpell->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) || !allowedType)
        return false;

    return true;
}

bool ShockTrigger::IsActive()
{
    return SpellTrigger::IsActive() &&
        !botAI->HasAura("flame shock", GetTarget(), false, true) &&
        !botAI->HasAura("frost shock", GetTarget(), false, true);
}

// Checks if the target's health is above 25%/1500 hp. Returns false if either are true.
// This logic exists to prevent the use of Earth Shock on bosses as an Elemental Shaman.
bool EarthShockExecuteTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    float percent = 100.0f * target->GetHealth() / target->GetMaxHealth();
    if (percent >= 25.0f)
        return false;

    if (target->GetHealth() >= 1500)
        return false;

    return true;
}

bool TotemTrigger::IsActive()
{
    if (HoldsTotemsOutOfCombat(bot))
        return false;

    return AI_VALUE(uint8, "attacker count") >= attackerCount &&
        !AI_VALUE2(bool, "has totem", name) &&
        !botAI->HasAura(name, bot);
}

bool ShamanisticRageForManaTrigger::IsActive()
{
    constexpr uint8 SHAMANISTIC_RAGE_MANA_PCT = 85;  // below this a minute of melee hits refills what it misses

    if (!bot->IsInCombat() || !bot->HasSpell(SPELL_SHAMANISTIC_RAGE) || !AI_VALUE2(bool, "has mana", "self target") ||
        AI_VALUE2(uint8, "mana", "self target") >= SHAMANISTIC_RAGE_MANA_PCT)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return target && target->IsAlive() && bot->IsWithinMeleeRange(target);
}

bool WaterWalkingTrigger::IsActive()
{
    return BuffTrigger::IsActive() && AI_VALUE2(bool, "swimming", "self target");
}

bool WaterBreathingTrigger::IsActive()
{
    return BuffTrigger::IsActive() && AI_VALUE2(bool, "swimming", "self target");
}

bool WaterWalkingOnPartyTrigger::IsActive()
{
    return BuffOnPartyTrigger::IsActive() && AI_VALUE2(bool, "swimming", "self target");
}

bool WaterBreathingOnPartyTrigger::IsActive()
{
    return BuffOnPartyTrigger::IsActive() && AI_VALUE2(bool, "swimming", "self target");
}

// [b31_sd] The old check, bot->HasSpellCooldown(421), read Chain Lightning RANK 1: a max-rank shaman never puts rank 1
// on cooldown, so Elemental Mastery never fired. Now: the spell itself is ready (spell id of the known rank), the
// buff is not up and the burst gate holds (BoostTrigger), and Lava Burst, when known, is off cooldown so the instant
// charge lands on it. Two cooldown lookups.
bool ElementalMasteryTrigger::IsActive()
{
    uint32 const masteryId = AI_VALUE2(uint32, "spell id", "elemental mastery");
    if (!masteryId || bot->HasSpellCooldown(masteryId))
        return false;

    uint32 const lavaBurstId = AI_VALUE2(uint32, "spell id", "lava burst");
    if (lavaBurstId && bot->HasSpellCooldown(lavaBurstId))
        return false;

    return BoostTrigger::IsActive();
}

// Checks if Sprit Wolves are out/if Spirit Walk buff is not on the bot/if the cooldown for Spirit Walk is ready.
// There is custom code for the Spirit Walk cooldown (32 seconds), since no working
// code exists in the AC/Playerbots repo for checking if a guardian's spell is on cooldown.
bool SpiritWalkTrigger::IsActive()
{
    constexpr uint32 SPIRIT_WOLF = 29264u;
    constexpr uint32 SPIRIT_WALK_SPELL_ID = 58875u;
    constexpr int COOLDOWN_IN_SECONDS = 32;

    time_t now = time(nullptr);

    if ((now - lastSpiritWalkTime) < COOLDOWN_IN_SECONDS)
        return false;

    // [b31_deep] Nothing for it to remove: no root or snare on the shaman.
    if (!botAI->IsMovementImpaired(bot))
        return false;

    for (Unit* unit : bot->m_Controlled)
    {
        Creature* wolf = dynamic_cast<Creature*>(unit);
        if (wolf && wolf->GetEntry() == SPIRIT_WOLF && wolf->IsAlive())
        {
            if (!bot->HasAura(SPIRIT_WALK_SPELL_ID))
            {
                lastSpiritWalkTime = now;
                return true;
            }
        }
    }

    return false;
}

// Checks if the bot knows Call of the Elements, and prevents the trigger firing if it doesn't.
// Fires the trigger if at least 2 of the totem slots are empty or out of range.
bool CallOfTheElementsTrigger::IsActive()
{
    // [rot_b31_heal] The slot count first (four lookups), the spell check (cost, cooldown) only when it is met; the
    // count is shared with the single totem drops that wait for this spell (DefersToCallOfTheElements).
    if (bot->IsInCombat() && !Leader::Events::AtCommandedSpot(bot))  // [lead151] as its action (lead149)
        return false;
    return CountTotemSlotsToDrop(bot) >= 2 && botAI->CanCastSpell(SPELL_CALL_OF_THE_ELEMENTS, bot, true);
}

// Totemic Recall - Prevents the trigger firing under the following circumstances:
// 1. The bot does not have Totemic Recall (level 30).
// 2. The bot is in an instance and any boss encounter is in progress.
// 3. Any group member or their pet is in combat.
// 4. If Mana Tide Totem or Fire Elemental Totem is active and within 30 yards of the bot.
// 5. Finally, if any totem summon slot is not empty, the trigger will fire.
bool TotemicRecallTrigger::IsActive()
{
    if (!bot->HasSpell(SPELL_TOTEMIC_RECALL))
        return false;

    Map* map = bot->GetMap();
    if (map && map->IsDungeon())
    {
        InstanceScript* instance = ((InstanceMap*)map)->GetInstanceScript();
        if (instance)
        {
            for (uint32 i = 0; i < instance->GetEncounterCount(); ++i)
            {
                if (instance->GetBossState(i) == IN_PROGRESS)
                    return false;
            }
        }
    }

    Group* group = bot->GetGroup();
    if (group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;

            if (member->IsInCombat())
                return false;

            Pet* pet = member->GetPet();
            if (pet && pet->IsInCombat())
                return false;
        }
    }

    ObjectGuid guid = bot->m_SummonSlot[SUMMON_SLOT_TOTEM_WATER];
    if (!guid.IsEmpty())
    {
        Creature* totem = bot->GetMap()->GetCreature(guid);
        uint32 currentSpell = 0;
        if (totem)
        {
            currentSpell = totem->GetUInt32Value(UNIT_CREATED_BY_SPELL);
        }

        for (size_t i = 0; i < MANA_TIDE_TOTEM_COUNT; ++i)
        {
            if (currentSpell == MANA_TIDE_TOTEM[i] && totem && totem->GetDistance(bot) <= 30.0f)
                return false;
        }
    }

    guid = bot->m_SummonSlot[SUMMON_SLOT_TOTEM_FIRE];
    if (!guid.IsEmpty())
    {
        Creature* totem = bot->GetMap()->GetCreature(guid);
        uint32 currentSpell = 0;
        if (totem)
        {
            currentSpell = totem->GetUInt32Value(UNIT_CREATED_BY_SPELL);
        }

        // [drift_fe_recall] In a dungeon the Greater Fire Elemental (a guardian, aggressive by the core: it attacks
        // whatever hostile comes into its sight) is recalled with its totem once the fight is over: left out, it
        // opened the next pack on its own before the tank (scholo-h 20261025-150737-4 pull 6: "victim=Greater Fire
        // Elemental", a 12-mob pack the planner had refused). Outside dungeons it keeps fighting as before.
        Map* const totemMap = bot->GetMap();
        bool const dungeon = totemMap && totemMap->IsDungeon();
        for (size_t i = 0; i < FIRE_ELEMENTAL_TOTEM_COUNT && !dungeon; ++i)
        {
            if (currentSpell == FIRE_ELEMENTAL_TOTEM[i] && totem && totem->GetDistance(bot) <= 30.0f)
                return false;
        }
    }

    // [rot_b31_heal] Totems the next pull can still use stay: lbrs-n 20261025-184229-14, the enhancement shaman
    // recalled after every pull ("totemic recall" 74 ticks) and re-dropped the set with Call of the Elements on the
    // next one (~1 per pull, part of ~9 totem GCDs a minute). A totem within TOTEM_KEEP_RANGE of the shaman is in reach
    // for every totem trigger here (they re-drop only beyond it), so it is kept until it expires or the group walks
    // out of that range; then the recall returns the mana as before. The totems acting on enemies leave on their own
    // ("idle hostile totem"), a dungeon Fire Elemental still recalls the set ([drift_fe_recall]).
    if (!ai::shaman::HasIdleHostileTotem(bot))
    {
        for (uint8 slot : TOTEM_SLOTS)
        {
            ObjectGuid const kept = bot->m_SummonSlot[slot];
            if (kept.IsEmpty())
                continue;

            Creature* totem = bot->GetMap()->GetCreature(kept);
            if (totem && totem->IsAlive() && totem->GetDistance(bot) <= TOTEM_KEEP_RANGE)
                return false;
        }
    }

    return !bot->m_SummonSlot[SUMMON_SLOT_TOTEM_EARTH].IsEmpty() ||
           !bot->m_SummonSlot[SUMMON_SLOT_TOTEM_FIRE].IsEmpty() ||
           !bot->m_SummonSlot[SUMMON_SLOT_TOTEM_WATER].IsEmpty() ||
           !bot->m_SummonSlot[SUMMON_SLOT_TOTEM_AIR].IsEmpty();
}

// Find the active totem strategy for this slot, and return the highest-rank spellId the bot knows for it
static uint32 GetRequiredTotemSpellId(PlayerbotAI* botAI, char const* strategies[],
    uint32 const* spellList[], const size_t spellCounts[], size_t numStrategies)
{
    Player* bot = botAI->GetBot();
    for (size_t i = 0; i < numStrategies; ++i)
    {
        if (botAI->HasStrategy(strategies[i], BOT_STATE_COMBAT))
        {
            // Find the highest-rank spell the bot knows
            for (size_t j = 0; j < spellCounts[i]; ++j)
            {
                if (bot->HasSpell(spellList[i][j]))
                {
                    return spellList[i][j];
                }
            }
        }
    }

    return 0;  // No relevant strategy active, or bot doesn't know any rank
}

bool NoEarthTotemTrigger::IsActive()
{
    if (HoldsTotemsOutOfCombat(bot))
        return false;
    // [rot_b31_heal] Two or more slots to fill: one Call of the Elements instead of a GCD per totem.
    if (DefersToCallOfTheElements(botAI, bot))
        return false;

    // Check if the bot has Stoneskin Totem (required level 4) and prevents the trigger firing if it doesn't
    if (!bot->HasSpell(SPELL_STONESKIN_TOTEM_RANK_1))
        return false;

    ObjectGuid guid = bot->m_SummonSlot[SUMMON_SLOT_TOTEM_EARTH];
    Creature* totem = nullptr;
    uint32 currentSpell = 0;
    if (!guid.IsEmpty())
    {
        totem = bot->GetMap()->GetCreature(guid);
        if (totem)
        {
            currentSpell = totem->GetUInt32Value(UNIT_CREATED_BY_SPELL);
        }
    }

    // Define supported earth totem strategies for this slot:
    static char const* names[] = {"strength of earth", "stoneskin", "tremor", "earthbind"};
    static uint32 const* spells[] = {STRENGTH_OF_EARTH_TOTEM, STONESKIN_TOTEM, TREMOR_TOTEM, EARTHBIND_TOTEM};
    static const size_t counts[] = {STRENGTH_OF_EARTH_TOTEM_COUNT, STONESKIN_TOTEM_COUNT, TREMOR_TOTEM_COUNT,
                                    EARTHBIND_TOTEM_COUNT};

    uint32 requiredSpell = GetRequiredTotemSpellId(botAI, names, spells, counts, 4);

    // EXCEPTION: If Stoneclaw Totem is out and in range, consider the slot "occupied" (do not fire the trigger)
    for (size_t i = 0; i < STONECLAW_TOTEM_COUNT; ++i)
    {
        if (currentSpell == STONECLAW_TOTEM[i] && totem && totem->GetDistance(bot) <= 30.0f)
            return false;
    }

    // If no relevant strategy, only care if the slot is empty or totem is too far away
    if (!requiredSpell)
        return guid.IsEmpty() || !totem || totem->GetDistance(bot) > 30.0f;

    // Fire if slot is empty or wrong totem or totem is too far away
    return !currentSpell || currentSpell != requiredSpell || !totem || totem->GetDistance(bot) > 30.0f;
}

bool NoFireTotemTrigger::IsActive()
{
    if (HoldsTotemsOutOfCombat(bot))
        return false;
    // [rot_b31_heal] Two or more slots to fill: one Call of the Elements instead of a GCD per totem.
    if (DefersToCallOfTheElements(botAI, bot))
        return false;

    // Check if the bot has Searing Totem (required level 10) and prevents the trigger firing if it doesn't
    if (!bot->HasSpell(SPELL_SEARING_TOTEM_RANK_1))
        return false;

    ObjectGuid guid = bot->m_SummonSlot[SUMMON_SLOT_TOTEM_FIRE];
    Creature* totem = nullptr;
    uint32 currentSpell = 0;
    if (!guid.IsEmpty())
    {
        totem = bot->GetMap()->GetCreature(guid);
        if (totem)
        {
            currentSpell = totem->GetUInt32Value(UNIT_CREATED_BY_SPELL);
        }
    }

    // Define supported fire totem strategies for this slot:
    static char const* names[] = {"searing", "magma", "flametongue", "wrath", "frost resistance"};
    static uint32 const* spells[] = {SEARING_TOTEM, MAGMA_TOTEM, FLAMETONGUE_TOTEM, TOTEM_OF_WRATH,
                                     FROST_RESISTANCE_TOTEM};
    static const size_t counts[] = {SEARING_TOTEM_COUNT, MAGMA_TOTEM_COUNT, FLAMETONGUE_TOTEM_COUNT, TOTEM_OF_WRATH_COUNT,
                                    FROST_RESISTANCE_TOTEM_COUNT};

    uint32 requiredSpell = GetRequiredTotemSpellId(botAI, names, spells, counts, 5);

    // EXCEPTION: If Fire Elemental is out and in range, consider the slot "occupied" (do not fire the trigger)
    for (size_t i = 0; i < FIRE_ELEMENTAL_TOTEM_COUNT; ++i)
    {
        if (currentSpell == FIRE_ELEMENTAL_TOTEM[i] && totem && totem->GetDistance(bot) <= 30.0f)
            return false;
    }

    // If no relevant strategy, only care if the slot is empty or totem is too far away
    if (!requiredSpell)
        return guid.IsEmpty() || !totem || totem->GetDistance(bot) > 30.0f;

    // [b31_deep] "magma" with a Searing Totem standing that still reaches the target, and Magma not paying for itself
    // (CastMagmaTotemAction::isUseful): the Searing Totem is the right totem, not a wrong one. It fired every tick
    // into a refused Magma (28 896 "magma totem" isUseful refusals on L80 Enhancement, DEEP_AUDIT_SHAMAN.md).
    if (totem && totem->IsAlive() && CreatedBy(requiredSpell, MAGMA_TOTEM, MAGMA_TOTEM_COUNT) &&
        CreatedBy(currentSpell, SEARING_TOTEM, SEARING_TOTEM_COUNT) && totem->GetDistance(bot) <= 30.0f)
    {
        constexpr float SEARING_TOTEM_RANGE = 20.0f;  // as CastSearingTotemAction
        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && totem->GetDistance(target) <= SEARING_TOTEM_RANGE && !ai::shaman::MagmaTotemPays(botAI, target))
            return false;
    }

    // Fire if slot is empty or wrong totem or totem is too far away
    return !currentSpell || currentSpell != requiredSpell || !totem || totem->GetDistance(bot) > 30.0f;
}

bool NoWaterTotemTrigger::IsActive()
{
    if (HoldsTotemsOutOfCombat(bot))
        return false;
    // [rot_b31_heal] Two or more slots to fill: one Call of the Elements instead of a GCD per totem.
    if (DefersToCallOfTheElements(botAI, bot))
        return false;

    // Check if the bot has Healing Stream Totem (required level 20) and prevents the trigger firing if it doesn't
    if (!bot->HasSpell(SPELL_HEALING_STREAM_TOTEM_RANK_1))
        return false;

    ObjectGuid guid = bot->m_SummonSlot[SUMMON_SLOT_TOTEM_WATER];
    Creature* totem = nullptr;
    uint32 currentSpell = 0;
    if (!guid.IsEmpty())
    {
        totem = bot->GetMap()->GetCreature(guid);
        if (totem)
        {
            currentSpell = totem->GetUInt32Value(UNIT_CREATED_BY_SPELL);
        }
    }

    // Define supported water totem strategies for this slot:
    static char const* names[] = {"healing stream", "mana spring", "cleansing", "fire resistance"};
    static uint32 const* spells[] = {HEALING_STREAM_TOTEM, MANA_SPRING_TOTEM, CLEANSING_TOTEM, FIRE_RESISTANCE_TOTEM};
    static const size_t counts[] = {HEALING_STREAM_TOTEM_COUNT, MANA_SPRING_TOTEM_COUNT, CLEANSING_TOTEM_COUNT,
                                    FIRE_RESISTANCE_TOTEM_COUNT};

    uint32 requiredSpell = GetRequiredTotemSpellId(botAI, names, spells, counts, 4);

    // EXCEPTION: If Mana Tide is out and in range, consider the slot "occupied" (do not fire the trigger)
    for (size_t i = 0; i < MANA_TIDE_TOTEM_COUNT; ++i)
    {
        if (currentSpell == MANA_TIDE_TOTEM[i] && totem && totem->GetDistance(bot) <= 30.0f)
            return false;
    }

    // If no relevant strategy, only care if the slot is empty or totem is too far away
    if (!requiredSpell)
    {
        return guid.IsEmpty() || !totem || totem->GetDistance(bot) > 30.0f;
    }

    // Fire if slot is empty or wrong totem or totem is too far away
    return !currentSpell || currentSpell != requiredSpell || !totem || totem->GetDistance(bot) > 30.0f;
}

bool NoAirTotemTrigger::IsActive()
{
    if (HoldsTotemsOutOfCombat(bot))
        return false;
    // [rot_b31_heal] Two or more slots to fill: one Call of the Elements instead of a GCD per totem.
    if (DefersToCallOfTheElements(botAI, bot))
        return false;

    // Check if the bot has Nature Resistance Totem (required level 30) and prevents the trigger firing if it doesn't
    if (!bot->HasSpell(SPELL_NATURE_RESISTANCE_TOTEM_RANK_1))
        return false;

    ObjectGuid guid = bot->m_SummonSlot[SUMMON_SLOT_TOTEM_AIR];
    Creature* totem = nullptr;
    uint32 currentSpell = 0;
    if (!guid.IsEmpty())
    {
        totem = bot->GetMap()->GetCreature(guid);
        if (totem)
        {
            currentSpell = totem->GetUInt32Value(UNIT_CREATED_BY_SPELL);
        }
    }

    // Define supported air totem strategies for this slot:
    // [b31_deep] The strategy is "grounding" (ShamanAiObjectContext), not "grounding totem", and all 4 entries are
    // read (the count was 3): a chosen Grounding never replaced the Windfury / Wrath of Air standing in the slot.
    static char const* names[] = {"wrath of air", "windfury", "nature resistance", "grounding"};
    static uint32 const* spells[] = {WRATH_OF_AIR_TOTEM, WINDFURY_TOTEM, NATURE_RESISTANCE_TOTEM, GROUNDING_TOTEM};
    static const size_t counts[] = {WRATH_OF_AIR_TOTEM_COUNT, WINDFURY_TOTEM_COUNT, NATURE_RESISTANCE_TOTEM_COUNT,
                                    GROUNDING_TOTEM_COUNT};

    uint32 requiredSpell = GetRequiredTotemSpellId(botAI, names, spells, counts, 4);

    // If no relevant strategy, only care if the slot is empty or totem is too far away
    if (!requiredSpell)
    {
        return guid.IsEmpty() || !totem || totem->GetDistance(bot) > 30.0f;
    }

    // Fire if slot is empty or wrong totem or totem is too far away
    return !currentSpell || currentSpell != requiredSpell || !totem || totem->GetDistance(bot) > 30.0f;
}

bool SetTotemTrigger::IsActive()
{
   uint32 highestKnownSpell = 0;
   for (size_t i = 0; i < totemSpellIdsCount; ++i)
   {
       const uint32 spellId = totemSpellIds[i];
       if (bot->HasSpell(spellId))
       {
           highestKnownSpell = spellId;
           break;
       }
   }

   if (!highestKnownSpell)
       return false;

   ActionButton const* button = bot->GetActionButton(actionButtonId);
   if (!button || button->GetType() != ACTION_BUTTON_SPELL || button->GetAction() == 0)
       return true;

   if (button->GetAction() != highestKnownSpell)
       return true;

   return false;
}

// On one target Chain Lightning out-damages Lightning Bolt and goes on cooldown; mana comes from Water Shield and
// Thunderstorm. Only a shaman nearly dry saves it: the medium-mana gate turned it off for the rest of a long fight.
// [b31_deep] Before Lava Burst (level 75) the floor is CHAIN_LIGHTNING_FILLER_PRE_LAVA_BURST_MANA_PCT: without Lava
// Burst, the level 80 glyphs and its gear the single-target Chain Lightning spam ran the 60-74 shaman dry
// (DEEP_AUDIT_SHAMAN.md; the advanced wowsims APL casts single-target Chain Lightning only with spare mana).
bool ChainLightningFillerTrigger::IsActive()
{
    constexpr uint8 CHAIN_LIGHTNING_FILLER_MANA_PCT = 20;
    constexpr uint8 CHAIN_LIGHTNING_FILLER_PRE_LAVA_BURST_MANA_PCT = 50;
    if (!ChainLightningNoCdTrigger::IsActive() || !AI_VALUE2(bool, "has mana", "self target"))
        return false;

    uint8 const manaFloor = AI_VALUE2(uint32, "spell id", "lava burst")
                                ? CHAIN_LIGHTNING_FILLER_MANA_PCT
                                : CHAIN_LIGHTNING_FILLER_PRE_LAVA_BURST_MANA_PCT;
    return AI_VALUE2(uint8, "mana", "self target") >= manaFloor;
}

// [b31_sd] It used to fire on 2 mobs at the filler's own relevance (5.1), i.e. a duplicate of "chain lightning filler"
// without the mana floor. It now ranks above Lava Burst (GenericShamanStrategy, ShamanAoeStrategy), so it needs a
// pack where the jumps beat a guaranteed-crit Lava Burst: 3+ mobs within the jump range of the target.
// [b31_deep] 2, was 3: the wowsims elemental APLs put Chain Lightning above Lava Burst on more than one target
// (numberTargets > 1); the target counts itself, so 2 is the target plus one mob within the jump range.
bool ChainLightningAoeTrigger::IsActive()
{
    constexpr uint32 CHAIN_LIGHTNING_AOE_MIN_TARGETS = 2;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !ChainLightningNoCdTrigger::IsActive())
        return false;

    return HasEnoughAoeTargets(botAI, target->GetPosition(), CHAIN_LIGHTNING_JUMP_RANGE,
                               CHAIN_LIGHTNING_AOE_MIN_TARGETS);
}

// Thunderstorm hits and knocks back everything within its radius, so it is only a free mana tap
// when nothing is in range.
bool ThunderstormForManaTrigger::IsActive()
{
    if (!AI_VALUE2(bool, "has mana", "self target") ||
        AI_VALUE2(uint8, "mana", "self target") >= sPlayerbotAIConfig.HighMana)
        return false;

    for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && bot->IsWithinDist(unit, THUNDERSTORM_RADIUS))
            return false;
    }

    return true;
}

// Knocking mobs away from the tank wrecks a group pull, so the knockback is solo only.
bool ThunderstormKnockbackTrigger::IsActive()
{
    if (bot->GetGroup())
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return target && bot->IsWithinDist(target, THUNDERSTORM_RADIUS);
}

// [b31_sd] Riptide on cooldown (GUIDE_AUDIT_HEALERS.md: 4.0/min on bosses against ~10 possible): the tank below
// RIPTIDE_TANK_HEALTH_PCT instead of the almost-full band (85 %), where the tank of a five-man spends most fights.
bool RiptideOnMainTankTrigger::IsActive()
{
    Unit* target = GetTarget();
    return target && target->IsAlive() && target->GetHealthPct() < RIPTIDE_TANK_HEALTH_PCT &&
           BuffOnMainTankTrigger::IsActive();
}

bool EarthShieldOnMainTankTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive())
        return false;

    if (BuffOnMainTankTrigger::IsActive())
        return true;

    Aura* own = botAI->GetAura(spell, target, true);
    return own && own->GetCharges() <= EARTH_SHIELD_RECAST_CHARGES;
}

// [b31_deep] Known and off cooldown first (the header).
bool HeroismTrigger::IsActive()
{
    return ai::shaman::IsSpellReady(botAI, spell) && BuffTrigger::IsActive() && ai::raidutil::ShouldCastLust(botAI);
}

bool BloodlustTrigger::IsActive()
{
    return ai::shaman::IsSpellReady(botAI, spell) && BuffTrigger::IsActive() && ai::raidutil::ShouldCastLust(botAI);
}

// [b31_deep] Elemental Flame Shock multidot, see the header.
bool FlameShockOnAttackerTrigger::IsActive()
{
    return AI_VALUE(uint8, "attacker count") >= FLAME_SHOCK_MULTIDOT_MIN_ATTACKERS && DebuffTrigger::IsActive();
}

namespace
{
// [rot_b31_heal] A magic buff on an enemy worth a Purge: it makes the mob hit harder or faster, soak or heal damage,
// ignore it or send spells back. Armor-type self buffs and the short ones are already left out by
// PlayerbotAI::canDispel (dispel whitelist) and Playerbots.DispelAuraDuration.
bool IsDangerousEnemyBuff(Aura const* aura)
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        AuraEffect const* effect = aura->GetEffect(i);
        if (!effect)
            continue;

        int32 const amount = effect->GetAmount();
        switch (effect->GetAuraType())
        {
            case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
            case SPELL_AURA_MOD_DAMAGE_DONE:
            case SPELL_AURA_MOD_ATTACK_POWER:
            case SPELL_AURA_MOD_MELEE_HASTE:
            case SPELL_AURA_MOD_MELEE_RANGED_HASTE:
            case SPELL_AURA_HASTE_SPELLS:
            case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
            case SPELL_AURA_MOD_HEALING_DONE_PERCENT:
                if (amount > 0)
                    return true;
                break;
            case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                if (amount < 0)
                    return true;
                break;
            case SPELL_AURA_PERIODIC_HEAL:
            case SPELL_AURA_SCHOOL_ABSORB:
            case SPELL_AURA_MANA_SHIELD:
            case SPELL_AURA_DAMAGE_SHIELD:
            case SPELL_AURA_SCHOOL_IMMUNITY:
            case SPELL_AURA_DAMAGE_IMMUNITY:
            case SPELL_AURA_REFLECT_SPELLS:
            case SPELL_AURA_REFLECT_SPELLS_SCHOOL:
                return true;
            default:
                break;
        }
    }

    return false;
}
}  // namespace

bool PurgeTrigger::IsActive()
{
    // The mana floor first: no aura is read below it.
    float const floor = PlayerbotAI::IsHeal(bot) ? HEALER_MANA_FLOOR : MANA_FLOOR;
    if (bot->getPowerType() == POWER_MANA && bot->GetPowerPct(POWER_MANA) < floor)
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld() || !target->IsAlive() || bot->IsFriendlyTo(target))
        return false;

    // The same visible-aura walk PlayerbotAI::HasAuraToDispel does (1 s trigger interval), plus the danger test.
    Unit::VisibleAuraMap const* visibleAuras = target->GetVisibleAuras();
    if (!visibleAuras)
        return false;

    for (auto const& [slot, aurApp] : *visibleAuras)
    {
        Aura const* aura = aurApp ? aurApp->GetBase() : nullptr;
        if (!aura || aura->IsPassive() || aura->IsRemoved())
            continue;

        if (sPlayerbotAIConfig.DispelAuraDuration && aura->GetDuration() &&
            aura->GetDuration() < static_cast<int32>(sPlayerbotAIConfig.DispelAuraDuration))
            continue;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo || !spellInfo->IsPositive() || !botAI->canDispel(spellInfo, DISPEL_MAGIC))
            continue;

        if (IsDangerousEnemyBuff(aura))
            return true;
    }

    return false;
}

bool PartyHealerLowManaTrigger::IsActive()
{
    // [rot_b31_heal] The fight clock and the mana at its start, kept by the trigger itself (no per-tick scan). The
    // trigger is not evaluated out of combat, so a gap since its last evaluation also starts a new fight.
    uint32 const now = getMSTime();
    if (!bot->IsInCombat())
    {
        combatStartMs = 0;
        return false;
    }

    float const ownMana = bot->GetPowerPct(POWER_MANA);
    if (!combatStartMs || getMSTimeDiff(lastCheckMs, now) > FIGHT_GAP_MS)
    {
        combatStartMs = now ? now : 1;
        combatStartMana = ownMana;
    }
    lastCheckMs = now;

    // Talent not taken, or the 5 min cooldown running: the early condition below holds for most of a boss fight.
    if (!bot->GetGroup() || !bot->HasSpell(MANA_TIDE_TOTEM[0]) || bot->HasSpellCooldown(MANA_TIDE_TOTEM[0]))
        return false;

    std::string const lowQualifier = ai::raidutil::LowManaHealerQualifier(true, sPlayerbotAIConfig.LowMana);
    if (AI_VALUE2(uint8, "low mana healer count", lowQualifier) >= 1)
        return true;

    std::string const mediumQualifier = ai::raidutil::LowManaHealerQualifier(true, sPlayerbotAIConfig.MediumMana);
    if (AI_VALUE2(uint8, "low mana healer count", mediumQualifier) >= 2)
        return true;

    // [rot_b31_heal] Early Mana Tide: two party healers, or the shaman in a fight that keeps draining, below
    // EARLY_MANA_PCT. The boss lookup is a cached value read only on this last, rare path.
    std::string const earlyQualifier =
        ai::raidutil::LowManaHealerQualifier(true, static_cast<uint32>(EARLY_MANA_PCT));
    if (AI_VALUE2(uint8, "low mana healer count", earlyQualifier) >= 2)
        return true;  // the party's healer mana is falling as a whole

    if (ownMana >= EARLY_MANA_PCT)
        return false;

    if (combatStartMana - ownMana >= FALLING_MANA_PCT)
        return true;  // spent a third of the bar in this one fight

    return AI_VALUE(Unit*, "boss target") != nullptr;
}

namespace ai::shaman
{
namespace
{
    struct ElementTotemStrategies
    {
        std::vector<std::string> managed;  // buff totems the coordinator may swap
        std::vector<std::string> all;      // every strategy of the element (special picks are left alone)
    };

    ElementTotemStrategies const& GetElementStrategies(uint8 element)
    {
        static ElementTotemStrategies const strategies[ai::raidutil::TOTEM_ELEMENT_COUNT] = {
            { { "strength of earth", "stoneskin" }, { "strength of earth", "stoneskin", "tremor", "earthbind" } },
            { { "wrath", "flametongue", "searing", "magma" },
              { "wrath", "flametongue", "searing", "magma", "frost resistance" } },
            { { "mana spring", "healing stream" },
              { "mana spring", "healing stream", "cleansing", "fire resistance" } },
            { { "wrath of air", "windfury" }, { "wrath of air", "windfury", "nature resistance", "grounding" } },
        };

        return strategies[element];
    }
}

std::string GetTotemAssignmentChange(PlayerbotAI* botAI, uint8 element)
{
    if (element >= ai::raidutil::TOTEM_ELEMENT_COUNT)
        return "";

    ai::raidutil::RaidUtilityAssignment const& assignment = ai::raidutil::GetAssignment(botAI);
    if (!assignment.valid)
        return "";

    std::string const& planned = assignment.totems[element];
    if (planned.empty())
        return "";

    ElementTotemStrategies const& strategies = GetElementStrategies(element);
    std::string current;
    for (std::string const& name : strategies.all)
    {
        if (botAI->HasStrategy(name, BOT_STATE_COMBAT))
        {
            current = name;
            break;
        }
    }

    // A resistance / tremor / grounding / cleansing pick comes from the player or a boss strategy: keep it.
    if (!current.empty() &&
        std::find(strategies.managed.begin(), strategies.managed.end(), current) == strategies.managed.end())
    {
        return "";
    }

    if (planned == ai::raidutil::TOTEM_AVOID_FIRE_BUFF)
        return (current == "wrath" || current == "flametongue") ? "searing" : "";

    return planned != current ? planned : "";
}

// [b31_sd] Resto water totem without a raid plan for it (one shaman in the party): Mana Spring does not stack with
// Blessing of Wisdom, so with a paladin's Wisdom on the shaman the slot goes to Healing Stream, the guides' other
// main water totem (GUIDE_AUDIT_HEALERS.md: HST 0/min). Only the two managed water strategies are swapped; a
// cleansing / resistance / tremor pick from the player or a boss strategy stays. Two aura-name lookups.
std::string GetWaterTotemChoiceChange(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->IsInCombat() || !bot->GetGroup() || AiFactory::GetPlayerSpecTab(bot) != SHAMAN_TAB_RESTORATION)
        return "";

    ai::raidutil::RaidUtilityAssignment const& assignment = ai::raidutil::GetAssignment(botAI);
    if (assignment.valid && !assignment.totems[ai::raidutil::TOTEM_ELEMENT_WATER].empty())
        return "";  // two or more shamans: the raid plan owns the water slot

    bool const manaSpring = botAI->HasStrategy("mana spring", BOT_STATE_COMBAT);
    bool const healingStream = botAI->HasStrategy("healing stream", BOT_STATE_COMBAT);
    if (!manaSpring && !healingStream)
        return "";

    bool const wisdom = botAI->HasAnyAuraOf(bot, "blessing of wisdom", "greater blessing of wisdom", nullptr);
    if (wisdom && manaSpring)
        return "healing stream";
    if (!wisdom && healingStream)
        return "mana spring";

    return "";
}
}

bool WaterTotemChoiceTrigger::IsActive() { return !ai::shaman::GetWaterTotemChoiceChange(botAI).empty(); }

bool TotemAssignmentTrigger::IsActive()
{
    if (bot->IsInCombat() || !bot->GetGroup())
        return false;

    for (uint8 element = 0; element < ai::raidutil::TOTEM_ELEMENT_COUNT; ++element)
    {
        if (!ai::shaman::GetTotemAssignmentChange(botAI, element).empty())
            return true;
    }

    return false;
}

namespace
{
    constexpr float REINCARNATION_FIGHT_RADIUS = 40.0f;  // a mob still fighting this close kills the shaman again
    constexpr float REINCARNATION_AGGRO_MARGIN = 5.0f;
}

// True while a living hostile mob is still fighting, evading home over the bodies, or standing close enough to
// aggro on the shaman when he rises. onlyIdle stays true when every such mob is out of combat and at rest.
bool ShamanReincarnationTrigger::HostileNearCorpse(bool& onlyIdle)
{
    onlyIdle = true;
    bool found = false;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest hostile npcs"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        Creature* creature = unit->ToCreature();
        float const distance = bot->GetDistance(unit);
        if (creature && creature->IsInEvadeMode())
        {
            found = true;
            onlyIdle = false;
            continue;
        }

        if (unit->IsInCombat())
        {
            if (distance <= REINCARNATION_FIGHT_RADIUS)
            {
                found = true;
                onlyIdle = false;
            }
            continue;
        }

        if (creature && distance <= creature->GetAggroRange(bot) + REINCARNATION_AGGRO_MARGIN)
            found = true;
    }

    return found;
}

bool ShamanReincarnationTrigger::IsActive()
{
    if (bot->IsAlive() || !bot->GetUInt32Value(PLAYER_SELF_RES_SPELL))
        return false;

    // Any mob that would come at once holds the Ankh, a pack resting on the corpse too: Reincarnation rises on the
    // corpse, it cannot pick a spot (Stratholme wave 18: given up after 90 s on an idle pack, the shaman rose among
    // the Ravaged Cadavers and died again within 2 s). Kept too long, the release takes over (corpse run, safe spot).
    bool onlyIdle = true;
    return !HostileNearCorpse(onlyIdle);
}

bool TidalWavesHealingWaveTrigger::IsActive()
{
    if (!bot->HasAura(SPELL_TIDAL_WAVES_BUFF))
        return false;

    Unit* target = AI_VALUE(Unit*, "party member to heal");
    return target && target->IsAlive() && target->GetHealthPct() < TIDAL_WAVES_HEALING_WAVE_PCT;
}
