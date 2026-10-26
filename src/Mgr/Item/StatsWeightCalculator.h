/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STATSWEIGHTCALCULATOR_H
#define PLAYERBOTS_STATSWEIGHTCALCULATOR_H

#include "Player.h"
#include "StatsCollector.h"

#define ITEM_SUBCLASS_MASK_SINGLE_HAND                                                                        \
    ((1 << ITEM_SUBCLASS_WEAPON_AXE) | (1 << ITEM_SUBCLASS_WEAPON_MACE) | (1 << ITEM_SUBCLASS_WEAPON_SWORD) | \
     (1 << ITEM_SUBCLASS_WEAPON_DAGGER) | (1 << ITEM_SUBCLASS_WEAPON_FIST))

enum StatsOverflowThreshold
{
    SPELL_HIT_OVERFLOW = 17,  // vs a +3 boss; lowered by Misery / Improved Faerie Fire in the raid
    MELEE_HIT_OVERFLOW = 8,
    MELEE_DUAL_WIELD_HIT_OVERFLOW = 27,  // white-swing cap while dual-wielding
    RANGED_HIT_OVERFLOW = 8,
    EXPERTISE_OVERFLOW = 26,  // dodge cap
    EXPERTISE_PARRY_OVERFLOW = 56,  // parry cap, still worth expertise for a tank attacking from the front
    DEFENSE_OVERFLOW = 140,
    ARMOR_PENETRATION_OVERFLOW = 100
};

// Group buffs and debuffs that move the hit caps; cached per bot, see GetHitComposition().
struct HitComposition
{
    uint32 computedAt = 0;
    uint8 spellHitDebuff = 0;  // best Misery / Improved Faerie Fire rank in the raid (1% each)
    bool heroicPresence = false;  // a Draenei in the bot's party
    bool wrathOfAir = false;  // a Shaman in the bot's party (Wrath of Air Totem, 5% spell haste)
    uint8 spellHasteAura = 0;  // Improved Moonkin Form / Swift Retribution in the party (1% per rank)
};

class StatsWeightCalculator
{
public:
    StatsWeightCalculator(Player* player);
    void Reset();
    float CalculateItem(uint32 itemId, int32 randomPropertyId = 0, int32 slot = -1);
    float CalculateEnchant(uint32 enchantId);
    int32 PickBestRandomPropertyId(uint32 itemId);

    void SetOverflowPenalty(bool apply) { enable_overflow_penalty_ = apply; }
    void SetItemSetBonus(bool apply) { enable_item_set_bonus_ = apply; }
    void SetPvpSpec(bool isPvp) { pvpSpec_ = isPvp; }
    void SetExcludeResilience(bool exclude) { exclude_resilience_ = exclude; }

    private:
    void GenerateWeights(Player* player);
    void GenerateBasicWeights(Player* player);
    void GenerateAdditionalWeights(Player* player);

    void CalculateRandomProperty(int32 randomPropertyId, uint32 itemId);
    void CalculateItemSetMod(Player* player, ItemTemplate const* proto);
    void CalculateSocketBonus(Player* player, ItemTemplate const* proto);

    void CalculateItemTypePenalty(ItemTemplate const* proto);
    float ApplyPreferredSpecWeapons(ItemTemplate const* proto, int32 slot);

    bool NotBestArmorType(uint32 item_subclass_armor);

    void ApplyOverflowPenalty(Player* player);
    void ApplyWeightFinetune(Player* player);
    void InitGearBreakpoints(Player* player);

private:
    Player* player_;
    CollectorType type_;
    CollectorType hitOverflowType_;
    std::unique_ptr<StatsCollector> collector_;
    uint8 cls;
    uint8 lvl;
    int tab;
    bool enable_overflow_penalty_;
    bool enable_item_set_bonus_;

    float weight_;
    float stats_weights_[STATS_TYPE_MAX];
    bool pvpSpec_ = false;
    bool exclude_resilience_ = false;
    bool dualWield_ = false;
    HitComposition hitComposition_;

    // Gear-dependent breakpoints, computed once per calculator (InitGearBreakpoints); see docs/DPS_BENCHMARKS.md.
    float gearArpPct_ = 0.0f;  // armor penetration % from gear, gems, enchants and passive talents (no procs)
    float procArpPct_ = 0.0f;  // largest armor penetration proc of the equipped trinkets, in %
    float arpCrossoverPct_ = 0.0f;  // gear ArP % where ArP overtakes the main stat; 0: the spec never stacks ArP
    StatsType arpMainStat_ = STATS_TYPE_STRENGTH;
    float meleeSpellHitBandFactor_ = 1.0f;  // hit between the melee special cap and the spell cap
    float gearHastePct_ = 0.0f;  // spell haste % from gear, gems and enchants (no procs)
    float hasteSoftCapPct_ = 0.0f;  // spell haste % of the GCD soft cap; 0: no soft cap for the spec
    float hastePastCapFactor_ = 1.0f;
};

#endif
