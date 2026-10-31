/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StatsWeightCalculator.h"
#include "AiFactory.h"
#include "DBCStores.h"
#include "Group.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "RandomItemMgr.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsCollector.h"
#include "Timer.h"
#include "Unit.h"
#include <algorithm>
#include <memory>
#include <unordered_map>

namespace
{
constexpr uint32 SPELL_MISERY_RANKS[] = { 33191, 33192, 33193 };
constexpr uint32 SPELL_IMPROVED_FAERIE_FIRE_RANKS[] = { 33600, 33601, 33602 };
constexpr uint32 SPELL_HEROIC_PRESENCE_MELEE = 6562;
constexpr uint32 SPELL_HEROIC_PRESENCE_CASTER = 28878;
constexpr uint32 SPELL_WRATH_OF_AIR_TOTEM = 3738;
constexpr uint32 SPELL_IMPROVED_MOONKIN_FORM_RANKS[] = { 48384, 48395, 48396 };
constexpr uint32 SPELL_SWIFT_RETRIBUTION_RANKS[] = { 53379, 53484, 53648 };
constexpr uint32 HIT_COMPOSITION_CACHE_MS = 30 * IN_MILLISECONDS;
constexpr float DUAL_WIELD_HIT_PAST_CAP_FACTOR = 0.5f;  // hit past the yellow cap only helps white swings
constexpr float TANK_EXPERTISE_PAST_DODGE_CAP_FACTOR = 0.5f;  // only removes parries (and boss parry-haste)

template <size_t Size>
uint8 GetHighestRank(Player* player, uint32 const (&rankSpellIds)[Size])
{
    for (size_t rank = Size; rank > 0; --rank)
    {
        if (player->HasSpell(rankSpellIds[rank - 1]))
            return uint8(rank);
    }

    return 0;
}

// Misery and Improved Faerie Fire are target debuffs (they do not stack), Heroic Presence is a party aura that is
// missing whenever the Draenei is out of range; neither shows up in the bot's own aura totals.
HitComposition GetHitComposition(Player* player)
{
    thread_local std::unordered_map<uint64, HitComposition> cache;

    uint32 const now = getMSTime();
    uint64 const key = player->GetGUID().GetRawValue();
    auto const it = cache.find(key);
    if (it != cache.end() && getMSTimeDiff(it->second.computedAt, now) < HIT_COMPOSITION_CACHE_MS)
        return it->second;

    HitComposition composition;
    composition.computedAt = now;

    auto const consider = [&composition](Player* member, bool sameParty)
    {
        composition.spellHitDebuff = std::max(composition.spellHitDebuff, GetHighestRank(member, SPELL_MISERY_RANKS));
        composition.spellHitDebuff =
            std::max(composition.spellHitDebuff, GetHighestRank(member, SPELL_IMPROVED_FAERIE_FIRE_RANKS));
        if (sameParty && member->getRace() == RACE_DRAENEI)
            composition.heroicPresence = true;
        // Totems and the 3% spell haste auras are party-wide only.
        if (sameParty && member->getClass() == CLASS_SHAMAN && member->HasSpell(SPELL_WRATH_OF_AIR_TOTEM))
            composition.wrathOfAir = true;
        if (sameParty)
        {
            composition.spellHasteAura =
                std::max(composition.spellHasteAura, GetHighestRank(member, SPELL_IMPROVED_MOONKIN_FORM_RANKS));
            composition.spellHasteAura =
                std::max(composition.spellHasteAura, GetHighestRank(member, SPELL_SWIFT_RETRIBUTION_RANKS));
        }
    };

    consider(player, true);

    Map* map = player->FindMap();
    if (Group* group = player->GetGroup(); group && map)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            // Members on another map run on another map thread; their spell maps are not safe to read here.
            if (!member || member == player || !member->IsInWorld() || member->FindMap() != map)
                continue;

            consider(member, group->SameSubGroup(player, member));
        }
    }

    cache[key] = composition;
    return composition;
}

constexpr uint32 SPELL_MOLTEN_ARMOR_RANKS[] = { 30482, 43045, 43046 };
constexpr uint32 SPELL_FEL_ARMOR_RANKS[] = { 28176, 28189, 47892, 47893 };
constexpr uint32 SPELL_CAREFUL_AIM = 34484;
constexpr uint32 SPELL_HUNTER_VS_WILD = 56341;
constexpr uint32 SPELL_ARMORED_TO_THE_TEETH = 61222;
constexpr uint32 SPELL_MENTAL_DEXTERITY = 51885;
constexpr uint32 SPELL_ROGUE_SWORD_SPECIALIZATION = 13964;
constexpr uint32 SPELL_POLEAXE_SPECIALIZATION = 12785;
constexpr uint32 SPELL_NERVES_OF_COLD_STEEL = 50138;
constexpr uint32 SPELL_SHADOW_FOCUS = 15835;
constexpr uint32 SPELL_ARCANE_FOCUS = 12840;
constexpr uint32 SPELL_NATURES_GRACE_RANKS[] = { 16880, 61345, 61346 };
constexpr uint32 SPELL_CELESTIAL_FOCUS_RANKS[] = { 16850, 16923, 16924 };
constexpr uint32 SPELL_GIFT_OF_THE_EARTHMOTHER_RANKS[] = { 51179, 51180, 51181, 51182, 51183 };

// Armor penetration, as Unit::CalcArmorReducedDamage applies it. A level 83 boss has 10643 armor, 8088.7 under
// Sunder Armor (-20%) and Faerie Fire (-5%), which AzerothCore multiplies. A level 80 attacker's armor constant is
// 10 * (8.5 * (80 + 4.5 * 21) + 40) = 15232.5, and (armor + 16635) / 3 > armor, so 100% penetration ignores all of
// it. Damage scales with K / (K + A * (1 - p)): one more point of penetration is worth 1 / (K + A * (1 - p)), which
// grows by 53% from 0 to the cap. That rising value, against a main stat whose value stays flat, is the "stack
// Strength first, then switch to ArP" transition of the 3.3 guides.
constexpr float ARP_BOSS_ARMOR_DEBUFFED = 8088.7f;
constexpr float ARP_ARMOR_CONSTANT = 15232.5f;
constexpr float ARP_RATING_PER_PCT_80 = 14.0f;  // 1400 rating = 100% at level 80
constexpr float ARP_PROC_UPTIME = 0.2f;  // Mjolnir Runestone / Grim Toll / Needle-Encrusted Scorpion: 10 s, ~45 s ICD

// Gear ArP rating at which ArP overtakes the spec's main stat (docs/DPS_BENCHMARKS.md, "ArP"); 0 = never stacks.
constexpr float ARP_CROSSOVER_FURY = 650.0f;  // Icy Veins: 1400 - 751 (ilvl 239 Mjolnir) = 649; +20% Str talent
constexpr float ARP_CROSSOVER_ARMS = 500.0f;  // no +20% Strength talent, Str is worth ~15% less than for Fury
// Combat: Warcraft Tavern / Warmane ICC gem Fractured over Agility / AP (Aldriana T8 EP: ArP 2.02 vs Agi 1.96).
constexpr float ARP_CROSSOVER_COMBAT = 400.0f;
constexpr float ARP_CROSSOVER_FERAL = 400.0f;  // Warmane feral guide: Agility below 400 ArP, ArP from 400 to 1400
// Marksmanship: legacy-wow 3.3.5 "Agility > ArP until 700 ArP, then ArP > Agility" (Serpent Sting / Chimera ignore
// armor).
constexpr float ARP_CROSSOVER_MARKSMANSHIP = 700.0f;
// Frost DK: ~45% of the damage is Frost; Warmane frost 3.3.5a "Strength / ArP once you get 72%+ from gear and gems",
// gem Bold until ~1100 ArP from gear, red gems and food.
constexpr float ARP_CROSSOVER_FROST_DK = 1000.0f;

// Hit between the melee special cap (8%) and the spell cap (17%) only helps spells (poisons, Lightning Bolt,
// Flame Shock, Death Coil) and, while dual-wielding, white swings.
constexpr float MELEE_SPELL_HIT_BAND_ROGUE = 0.6f;
constexpr float MELEE_SPELL_HIT_BAND_ENHANCEMENT = 0.7f;
constexpr float MELEE_SPELL_HIT_BAND_UNHOLY = 0.35f;

// Spell haste past the GCD soft cap only speeds up the casts that are not GCD-bound yet.
constexpr float GCD_FLOOR = 1.0f;
constexpr float GCD_BASE = 1.5f;
constexpr float NATURES_GRACE_HASTE = 0.2f;
constexpr float WRATH_OF_AIR_HASTE = 0.05f;
constexpr float HASTE_PAST_CAP_BALANCE = 0.5f;  // Starfire and Wrath outside Nature's Grace still gain
constexpr float HASTE_PAST_CAP_RESTO_DRUID = 0.35f;  // only Nourish / Regrowth / Healing Touch still gain
// Discipline: Warcraft Tavern "haste to soft cap > SP > crit > haste above cap"; the cap is 434 rating unbuffed,
// 325 with a 3% aura, 257 with Wrath of Air, 154 with both = a fixed 13.24% total haste multiplier.
constexpr float DISC_HASTE_TARGET = 1.1324f;
constexpr float HASTE_PAST_CAP_DISCIPLINE = 0.6f;

float ArpMarginal(float pct)
{
    float const p = std::clamp(pct, 0.0f, 100.0f) / 100.0f;
    return 1.0f / (ARP_ARMOR_CONSTANT + ARP_BOSS_ARMOR_DEBUFFED * (1.0f - p));
}

// Rating from gear, gems and enchants: the combat rating field minus rating auras (trinket procs, buffs).
float GearRatingPct(Player* player, CombatRating cr)
{
    int32 const total = int32(player->GetUInt32Value(static_cast<uint16>(PLAYER_FIELD_COMBAT_RATING_1) + cr));
    int32 const fromAuras = player->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_RATING, 1 << cr);
    return float(std::max(0, total - fromAuras)) * player->GetRatingMultiplier(cr);
}

// Largest armor penetration rating proc of the equipped trinkets (on-equip proc aura -> MOD_RATING ArP), in %.
// Deathbringer's Will picks its proc in a script and is not seen here.
float ArmorPenetrationProcPct(Player* player)
{
    float best = 0.0f;
    for (uint8 const slot : { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 })
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            continue;

        for (_Spell const& itemSpell : proto->Spells)
        {
            if (itemSpell.SpellId <= 0 || itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_EQUIP)
                continue;

            SpellInfo const* equip = sSpellMgr->GetSpellInfo(itemSpell.SpellId);
            if (!equip)
                continue;

            for (SpellEffectInfo const& effect : equip->GetEffects())
            {
                if (effect.ApplyAuraName != SPELL_AURA_PROC_TRIGGER_SPELL || !effect.TriggerSpell)
                    continue;

                SpellInfo const* proc = sSpellMgr->GetSpellInfo(effect.TriggerSpell);
                if (!proc)
                    continue;

                for (SpellEffectInfo const& procEffect : proc->GetEffects())
                {
                    if (procEffect.ApplyAuraName == SPELL_AURA_MOD_RATING &&
                        (procEffect.MiscValue & (1 << CR_ARMOR_PENETRATION)))
                        best = std::max(best, float(procEffect.CalcValue()) *
                                                  player->GetRatingMultiplier(CR_ARMOR_PENETRATION));
                }
            }
        }
    }

    return best;
}

// Threat reduction per percent and magic resistance per point, by role
constexpr float HEALER_THREAT_REDUCTION_WEIGHT = 2.0f;
constexpr float HEALER_RESISTANCE_WEIGHT = 1.0f;
constexpr float CASTER_THREAT_REDUCTION_WEIGHT = 4.0f;
constexpr float CASTER_RESISTANCE_WEIGHT = 0.5f;
constexpr float TANK_THREAT_REDUCTION_WEIGHT = -4.0f;
}

template <size_t Size>
bool HasAnySpell(Player* player, uint32 const (&spellIds)[Size])
{
    for (uint32 const spellId : spellIds)
    {
        if (player->HasSpell(spellId))
            return true;
    }

    return false;
}

StatsWeightCalculator::StatsWeightCalculator(Player* player) : player_(player)
{
    if (PlayerbotAI::IsHeal(player))
        type_ = CollectorType::SPELL_HEAL;
    else if (PlayerbotAI::IsCaster(player))
        type_ = CollectorType::SPELL_DMG;
    else if (PlayerbotAI::IsTank(player))
        type_ = CollectorType::MELEE_TANK;
    else if (PlayerbotAI::IsMelee(player))
        type_ = CollectorType::MELEE_DMG;
    else
        type_ = CollectorType::RANGED;
    cls = player->getClass();
    lvl = player->GetLevel();
    tab = AiFactory::GetPlayerSpecTab(player);
    collector_ = std::make_unique<StatsCollector>(type_, cls, lvl);
    // By talent spec, like the weights in GenerateBasicWeights(); type_ follows the strategies
    healSpec_ = PlayerbotAI::IsHeal(player, true);
    casterSpec_ = !healSpec_ && PlayerbotAI::IsCaster(player, true);

    if (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_UNHOLY)
        hitOverflowType_ = CollectorType::SPELL;
    else if (cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT)
        hitOverflowType_ = CollectorType::SPELL;
    else if (cls == CLASS_ROGUE)
        hitOverflowType_ = CollectorType::SPELL;
    else
        hitOverflowType_ = type_;

    if (type_ == CollectorType::MELEE_DMG && player->CanDualWield())
    {
        Item* offhand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        dualWield_ = (offhand && offhand->GetTemplate()->Class == ITEM_CLASS_WEAPON) ||
                     (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY);
    }

    hitComposition_ = GetHitComposition(player);
    InitGearBreakpoints(player);

    enable_overflow_penalty_ = true;
    enable_item_set_bonus_ = true;
}

void StatsWeightCalculator::InitGearBreakpoints(Player* player)
{
    // Armor penetration from gear plus passive talents (Mace Specialization only with a mace equipped).
    gearArpPct_ = GearRatingPct(player, CR_ARMOR_PENETRATION);
    gearArpPct_ += float(player->GetTotalAuraModifier(
        SPELL_AURA_MOD_ARMOR_PENETRATION_PCT,
        [player](AuraEffect const* aurEff)
        {
            SpellInfo const* info = aurEff->GetSpellInfo();
            return info->EquippedItemClass == -1 || player->HasItemFitToSpellRequirements(info);
        }));
    procArpPct_ = ArmorPenetrationProcPct(player);

    float crossoverRating = 0.0f;
    arpMainStat_ = STATS_TYPE_STRENGTH;
    if (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY)
        crossoverRating = ARP_CROSSOVER_FURY;
    else if (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_ARMS)
        crossoverRating = ARP_CROSSOVER_ARMS;
    else if (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_FROST)
        crossoverRating = ARP_CROSSOVER_FROST_DK;
    else if (cls == CLASS_ROGUE && tab == ROGUE_TAB_COMBAT)
    {
        crossoverRating = ARP_CROSSOVER_COMBAT;
        arpMainStat_ = STATS_TYPE_AGILITY;
    }
    else if (cls == CLASS_DRUID && tab == DRUID_TAB_FERAL && type_ == CollectorType::MELEE_DMG)
    {
        crossoverRating = ARP_CROSSOVER_FERAL;
        arpMainStat_ = STATS_TYPE_AGILITY;
    }
    else if (cls == CLASS_HUNTER && tab == HUNTER_TAB_MARKSMANSHIP)
    {
        crossoverRating = ARP_CROSSOVER_MARKSMANSHIP;
        arpMainStat_ = STATS_TYPE_AGILITY;
    }
    arpCrossoverPct_ = crossoverRating / ARP_RATING_PER_PCT_80;

    if (type_ == CollectorType::MELEE_DMG && (hitOverflowType_ & CollectorType::SPELL))
    {
        if (cls == CLASS_ROGUE)
            meleeSpellHitBandFactor_ = MELEE_SPELL_HIT_BAND_ROGUE;
        else if (cls == CLASS_SHAMAN)
            meleeSpellHitBandFactor_ = MELEE_SPELL_HIT_BAND_ENHANCEMENT;
        else if (cls == CLASS_DEATH_KNIGHT)
            meleeSpellHitBandFactor_ = MELEE_SPELL_HIT_BAND_UNHOLY;
    }

    // GCD soft caps: haste that pushes a 1.5 s cast under the 1.0 s GCD floor is lost. Spell haste multiplies:
    // cap = GCD_BASE / (GCD_FLOOR * talents * Wrath of Air * 3% aura) - 1.
    gearHastePct_ = GearRatingPct(player, CR_HASTE_SPELL);
    float const partyHaste = (1.0f + (hitComposition_.wrathOfAir ? WRATH_OF_AIR_HASTE : 0.0f)) *
                             (1.0f + 0.01f * float(hitComposition_.spellHasteAura));
    if (cls == CLASS_DRUID && tab == DRUID_TAB_BALANCE && HasAnySpell(player, SPELL_NATURES_GRACE_RANKS))
    {
        // Starlight Wrath makes Wrath a 1.5 s cast; under Nature's Grace (+20%) it hits the GCD floor at 401 haste
        // rating with Wrath of Air and Improved Moonkin Form, 585 without the totem (Wowhead / Icy Veins 3.3).
        float const talentHaste =
            (1.0f + NATURES_GRACE_HASTE) * (1.0f + 0.01f * float(GetHighestRank(player, SPELL_CELESTIAL_FOCUS_RANKS)));
        hasteSoftCapPct_ = (GCD_BASE / (GCD_FLOOR * talentHaste * partyHaste) - 1.0f) * 100.0f;
        hastePastCapFactor_ = HASTE_PAST_CAP_BALANCE;
    }
    else if (cls == CLASS_DRUID && tab == DRUID_TAB_RESTORATION)
    {
        // Rejuvenation / Lifebloom / Wild Growth / Swiftmend are GCD-bound: 856 rating with Gift of the Earthmother
        // 5/5, Wrath of Air and a 3% aura.
        float const talentHaste = 1.0f + 0.02f * float(GetHighestRank(player, SPELL_GIFT_OF_THE_EARTHMOTHER_RANKS));
        hasteSoftCapPct_ = (GCD_BASE / (GCD_FLOOR * talentHaste * partyHaste) - 1.0f) * 100.0f;
        hastePastCapFactor_ = HASTE_PAST_CAP_RESTO_DRUID;
    }
    else if (cls == CLASS_PRIEST && tab == PRIEST_TAB_DISCIPLINE)
    {
        hasteSoftCapPct_ = std::max(0.0f, (DISC_HASTE_TARGET / partyHaste - 1.0f) * 100.0f);
        hastePastCapFactor_ = HASTE_PAST_CAP_DISCIPLINE;
    }
}

void StatsWeightCalculator::Reset()
{
    collector_->Reset();
    weight_ = 0;
    for (uint32 i = 0; i < STATS_TYPE_MAX; i++)
    {
        stats_weights_[i] = 0;
    }
}

float StatsWeightCalculator::CalculateItem(uint32 itemId, int32 randomPropertyIds, int32 slot)
{
    ItemTemplate const* proto = &sObjectMgr->GetItemTemplateStore()->at(itemId);

    if (!proto)
        return 0.0f;

    Reset();

    collector_->CollectItemStats(proto);

    if (randomPropertyIds != 0)
        CalculateRandomProperty(randomPropertyIds, itemId);

    bool const checkSetBonus = enable_item_set_bonus_;
    std::array<float, STATS_TYPE_MAX> setBonusStats{};
    std::array<float, STATS_TYPE_MAX> setBonusBaseline{};

    // Clamp the item's own stats first so the tier below is valued only on what the player can
    // still use (a tier's hit at the hit cap must not make the piece look valued).
    if (enable_overflow_penalty_)
        ApplyOverflowPenalty(player_);

    if (checkSetBonus)
    {
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            setBonusBaseline[i] = collector_->stats[i];
        CollectItemSetBonus(player_, proto, slot, setBonusStats);

        // Clamp the combined stats: the tier's contribution is capped the same way.
        if (enable_overflow_penalty_)
            ApplyOverflowPenalty(player_);
    }

    GenerateWeights(player_);
    for (uint32 i = 0; i < STATS_TYPE_MAX; i++)
    {
        weight_ += stats_weights_[i] * collector_->stats[i];
    }

    CalculateItemTypePenalty(proto);

    // Value the tier on the stats that survive the clamp, on top of the item's own clamped
    // stats. A tier whose weighted delta is ~zero (most raid tier procs only produce
    // STATS_TYPE_BONUS, capped hit) falls back to the legacy completeness multiplier.
    float setBonusValue = 0.0f;
    if (checkSetBonus)
    {
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            if (i != STATS_TYPE_BONUS)
                setBonusValue += stats_weights_[i] * (collector_->stats[i] - setBonusBaseline[i]);
    }
    bool const setBonusValued = setBonusValue > 0.0001f;

    if (enable_item_set_bonus_ && !setBonusValued)
        CalculateItemSetMod(player_, proto);

    CalculateSocketBonus(player_, proto);

    // Apply weapon speed governance if slot is provided and this is a weapon
    if (sPlayerbotAIConfig.PreferredSpecWeapons && slot >= 0 && proto->Class == ITEM_CLASS_WEAPON)
        weight_ *= ApplyPreferredSpecWeapons(proto, slot);

    return weight_;
}

float StatsWeightCalculator::CalculateEnchant(uint32 enchantId, uint32 weaponDelay, bool isRanged)
{
    SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);

    if (!enchant)
        return 0.0f;

    Reset();

    collector_->CollectEnchantStats(enchant, 0, weaponDelay, isRanged);

    if (enable_overflow_penalty_)
        ApplyOverflowPenalty(player_);

    GenerateWeights(player_);
    for (uint32 i = 0; i < STATS_TYPE_MAX; i++)
    {
        weight_ += stats_weights_[i] * collector_->stats[i];
    }

    return weight_;
}

void StatsWeightCalculator::CalculateRandomProperty(int32 randomPropertyId, uint32 itemId)
{
    if (randomPropertyId > 0)
    {
        ItemRandomPropertiesEntry const* item_rand = sItemRandomPropertiesStore.LookupEntry(randomPropertyId);
        if (!item_rand)
        {
            return;
        }

        for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < MAX_ENCHANTMENT_SLOT; ++i)
        {
            uint32 enchantId = item_rand->Enchantment[i - PROP_ENCHANTMENT_SLOT_0];
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
            if (enchant)
                collector_->CollectEnchantStats(enchant);
        }
    }
    else
    {
        ItemRandomSuffixEntry const* item_rand = sItemRandomSuffixStore.LookupEntry(-randomPropertyId);
        if (!item_rand)
        {
            return;
        }

        for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < MAX_ENCHANTMENT_SLOT; ++i)
        {
            uint32 enchantId = item_rand->Enchantment[i - PROP_ENCHANTMENT_SLOT_0];
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
            uint32 enchant_amount = 0;

            for (int k = 0; k < MAX_ITEM_ENCHANTMENT_EFFECTS; ++k)
            {
                if (item_rand->Enchantment[k] == enchantId)
                {
                    enchant_amount = uint32((item_rand->AllocationPct[k] * GenerateEnchSuffixFactor(itemId)) / 10000);
                    break;
                }
            }

            if (enchant)
                collector_->CollectEnchantStats(enchant, enchant_amount);
        }
    }
}

int32 StatsWeightCalculator::PickBestRandomPropertyId(uint32 itemId)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return 0;

    bool isSuffix = false;
    uint32 poolEntry = proto->RandomProperty;
    if (!poolEntry)
    {
        poolEntry = proto->RandomSuffix;
        isSuffix = true;
    }
    if (!poolEntry)
        return 0;

    std::vector<uint32> const& pool = sRandomItemMgr.GetEnchantmentPool(poolEntry);
    if (pool.empty())
        return 0;

    Reset();
    GenerateWeights(player_);

    int32 bestId = 0;
    float bestScore = 0.0f;
    for (uint32 enchId : pool)
    {
        int32 candidate = isSuffix ? -static_cast<int32>(enchId) : static_cast<int32>(enchId);

        collector_->Reset();
        CalculateRandomProperty(candidate, itemId);

        float score = 0.0f;
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            score += stats_weights_[i] * collector_->stats[i];

        if (bestId == 0 || score > bestScore)
        {
            bestId = candidate;
            bestScore = score;
        }
    }

    collector_->Reset();
    return bestId;
}

void StatsWeightCalculator::GenerateWeights(Player* player)
{
    GenerateBasicWeights(player);
    GenerateAdditionalWeights(player);
    ApplyWeightFinetune(player);
}

void StatsWeightCalculator::GenerateBasicWeights(Player* player)
{
    // Per-spec stat weights, ordered as the 3.3.5 / ICC guides rank the stats (docs/STAT_PRIORITIES.md has the
    // per-spec table and sources). A weight is the value of one point of the stat as items carry it; where a guide
    // compares "AP (x2)" with Agility or Strength (2 AP per point of item budget), AP is weighted just above half
    // the stat it ranks against. Hit / expertise / defense / ArP / haste caps are applied in ApplyOverflowPenalty,
    // the ArP crossover of the stacking specs in ApplyWeightFinetune.
    stats_weights_[STATS_TYPE_STAMINA] += 0.1f;
    stats_weights_[STATS_TYPE_ARMOR] += 0.001f;
    stats_weights_[STATS_TYPE_BONUS] += 1.0f;
    stats_weights_[STATS_TYPE_MELEE_DPS] += 0.01f;
    stats_weights_[STATS_TYPE_RANGED_DPS] += 0.01f;

    if (cls == CLASS_HUNTER && tab == HUNTER_TAB_BEAST_MASTERY)
    {
        // Hit > AP (x2) > Agility > ArP / Crit > Haste: the pet scales off AP only (Icy Veins, Warcraft Tavern).
        stats_weights_[STATS_TYPE_AGILITY] += 1.8f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 1.1f;
        stats_weights_[STATS_TYPE_HIT] += 2.0f;
        stats_weights_[STATS_TYPE_CRIT] += 1.2f;
        stats_weights_[STATS_TYPE_HASTE] += 0.8f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 7.5f;
    }
    else if (cls == CLASS_HUNTER && tab == HUNTER_TAB_SURVIVAL)
    {
        // Hit > Agility > Crit > AP (x2) > ArP > Haste; Delicate in red and yellow (Warcraft Tavern, Icy Veins).
        stats_weights_[STATS_TYPE_AGILITY] += 2.5f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 0.9f;
        stats_weights_[STATS_TYPE_HIT] += 2.7f;
        stats_weights_[STATS_TYPE_CRIT] += 2.1f;
        stats_weights_[STATS_TYPE_HASTE] += 0.7f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 7.5f;
    }
    else if (cls == CLASS_HUNTER && tab == HUNTER_TAB_MARKSMANSHIP)
    {
        // Hit > Agility (ArP past the crossover) > Crit > AP > Haste (Icy Veins, Warcraft Tavern, legacy-wow).
        stats_weights_[STATS_TYPE_AGILITY] += 2.4f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 2.25f;
        stats_weights_[STATS_TYPE_HIT] += 2.6f;
        stats_weights_[STATS_TYPE_CRIT] += 1.6f;
        stats_weights_[STATS_TYPE_HASTE] += 0.9f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 10.0f;
    }
    else if (cls == CLASS_ROGUE && tab == ROGUE_TAB_COMBAT)
    {
        // Hit > Expertise > ArP (past the crossover) > AP (x2) > Agility > Haste > Crit > Strength (Warcraft Tavern,
        // Warmane ICC; Aldriana T8 EP Agility 1.96 AP).
        stats_weights_[STATS_TYPE_AGILITY] += 1.9f;
        stats_weights_[STATS_TYPE_STRENGTH] += 0.95f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 1.8f;
        stats_weights_[STATS_TYPE_HIT] += 2.2f;
        stats_weights_[STATS_TYPE_CRIT] += 1.4f;
        stats_weights_[STATS_TYPE_HASTE] += 1.7f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.1f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 7.0f;
    }
    else if (cls == CLASS_DRUID && tab == DRUID_TAB_FERAL && !PlayerbotAI::IsTank(player))
    {
        // Warcraft Tavern P3 DPS per point: ArP 2.35 > Hit 2.19 = Expertise 2.19 > Agility 1.91 > Strength 1.85 >
        // Crit 1.50 > Haste 1.42 > Feral AP 0.93 (scaled here to AP = 1.0; ArP from the crossover).
        stats_weights_[STATS_TYPE_AGILITY] += 2.05f;
        stats_weights_[STATS_TYPE_STRENGTH] += 2.0f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 2.3f;
        stats_weights_[STATS_TYPE_HIT] += 2.4f;
        stats_weights_[STATS_TYPE_CRIT] += 1.6f;
        stats_weights_[STATS_TYPE_HASTE] += 1.5f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.35f;
        // Weapon DPS is taken into account as part of Attack Power for Cat Druids.
    }
    else if (cls == CLASS_ROGUE && tab == ROGUE_TAB_ASSASSINATION)
    {
        // Hit > Expertise > AP (x2) > Haste > Agility > Crit > ArP > Strength (Warcraft Tavern; Bright in red).
        stats_weights_[STATS_TYPE_AGILITY] += 1.5f;
        stats_weights_[STATS_TYPE_STRENGTH] += 0.85f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 0.9f;
        stats_weights_[STATS_TYPE_HIT] += 2.2f;
        stats_weights_[STATS_TYPE_CRIT] += 1.1f;
        stats_weights_[STATS_TYPE_HASTE] += 1.8f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.1f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 5.0f;
    }
    else if (cls == CLASS_ROGUE && tab == ROGUE_TAB_SUBTLETY)
    {
        // Hit > Expertise > Agility > Str / AP > ArP > Haste > Crit; Delicate in red (Icy Veins).
        stats_weights_[STATS_TYPE_AGILITY] += 2.1f;
        stats_weights_[STATS_TYPE_STRENGTH] += 1.0f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 1.0f;
        stats_weights_[STATS_TYPE_HIT] += 2.2f;
        stats_weights_[STATS_TYPE_CRIT] += 0.8f;
        stats_weights_[STATS_TYPE_HASTE] += 0.9f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.15f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 5.0f;
    }
    else if (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY)
    {
        // Hit > Expertise > ArP (past the crossover) > Strength > AP (x2) > Crit > Agility > Haste (Icy Veins,
        // Warcraft Tavern; wowsims Fury EP Str 2.72, Crit 2.12, Agi 1.82, Haste 1.72, AP 1.0).
        stats_weights_[STATS_TYPE_AGILITY] += 1.5f;
        stats_weights_[STATS_TYPE_STRENGTH] += 2.5f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.05f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 2.1f;
        stats_weights_[STATS_TYPE_HIT] += 2.8f;
        stats_weights_[STATS_TYPE_CRIT] += 2.0f;
        stats_weights_[STATS_TYPE_HASTE] += 1.3f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 2.0f;
        stats_weights_[STATS_TYPE_DEFENSE] -= 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.6f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 7.0f;
    }
    else if (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_ARMS)
    {
        // Hit > ArP (past the crossover) > Strength > Crit > Expertise > Agility > Haste; expertise sits low because
        // dodges feed Overpower (Icy Veins phase 5).
        stats_weights_[STATS_TYPE_AGILITY] += 1.4f;
        stats_weights_[STATS_TYPE_STRENGTH] += 2.5f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 1.7f;
        stats_weights_[STATS_TYPE_HIT] += 2.7f;
        stats_weights_[STATS_TYPE_CRIT] += 1.9f;
        stats_weights_[STATS_TYPE_HASTE] += 1.1f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 2.0f;
        stats_weights_[STATS_TYPE_DEFENSE] -= 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 1.7f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 7.0f;
    }
    else if (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_FROST)
    {
        // Hit > Expertise > Strength (ArP past the crossover) > AP (x2) > Crit > Haste > Agility (Icy Veins, Warcraft
        // Tavern, Warmane frost 3.3.5a).
        stats_weights_[STATS_TYPE_AGILITY] += 0.6f;
        stats_weights_[STATS_TYPE_STRENGTH] += 2.5f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 0.9f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 2.0f;
        stats_weights_[STATS_TYPE_HIT] += 2.7f;
        stats_weights_[STATS_TYPE_CRIT] += 1.6f;
        stats_weights_[STATS_TYPE_HASTE] += 1.4f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.55f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 7.0f;
    }
    else if (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_UNHOLY)
    {
        // Hit > Strength > Haste > ArP > Crit > Expertise > AP > Agility (Icy Veins, legacy-wow; Warmane: Strength
        // is the best stat per point even at end game, so ArP never overtakes it).
        stats_weights_[STATS_TYPE_AGILITY] += 0.5f;
        stats_weights_[STATS_TYPE_STRENGTH] += 2.5f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 0.6f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 1.5f;
        stats_weights_[STATS_TYPE_HIT] += 2.6f;
        stats_weights_[STATS_TYPE_CRIT] += 1.45f;
        stats_weights_[STATS_TYPE_HASTE] += 1.8f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 1.3f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 5.0f;
    }
    else if (cls == CLASS_PALADIN && tab == PALADIN_TAB_RETRIBUTION)
    {
        // Hit > Expertise > Strength > Haste > Agility > Crit > AP > ArP > SP (Warcraft Tavern, Icy Veins; wowsims
        // Ret EP Str 2.53, Hit 1.96, Exp 1.80, Haste 1.44, Crit 1.16, Agi 1.13, AP 1.0, ArP 0.76). An EP is the value
        // of one point, so AP is 1.0 and not halved for "AP (x2)".
        stats_weights_[STATS_TYPE_AGILITY] += 1.25f;
        stats_weights_[STATS_TYPE_STRENGTH] += 2.5f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 0.7f;
        stats_weights_[STATS_TYPE_HIT] += 2.6f;
        stats_weights_[STATS_TYPE_CRIT] += 1.2f;
        stats_weights_[STATS_TYPE_HASTE] += 1.4f;
        stats_weights_[STATS_TYPE_SPELL_POWER] += 0.3f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.3f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 9.0f;
    }
    else if ((cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT))
    {
        // Hit > Expertise > Haste / AP (x2) > Agility > Intellect (Mental Dexterity, see below) > SP > Crit >
        // Strength > ArP; Bright in red, Quick in yellow (Icy Veins, Warcraft Tavern, legacy-wow; wowsims Enh EP).
        stats_weights_[STATS_TYPE_AGILITY] += 1.6f;
        stats_weights_[STATS_TYPE_STRENGTH] += 0.9f;
        stats_weights_[STATS_TYPE_INTELLECT] += 0.25f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_SPELL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 0.5f;
        stats_weights_[STATS_TYPE_HIT] += 2.1f;
        stats_weights_[STATS_TYPE_CRIT] += 0.95f;
        stats_weights_[STATS_TYPE_HASTE] += 1.8f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.0f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 8.5f;
    }
    else if (cls == CLASS_PRIEST && tab == PRIEST_TAB_SHADOW)
    {
        // Hit > SP > Haste > Crit > Spirit (Twisted Faith) > Intellect (Warcraft Tavern, legacy-wow; wowsims P4 EP
        // Spi 0.47, Int 0.11).
        stats_weights_[STATS_TYPE_INTELLECT] += 0.15f;
        stats_weights_[STATS_TYPE_SPIRIT] += 0.4f;
        stats_weights_[STATS_TYPE_SPELL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_HIT] += 1.2f;
        stats_weights_[STATS_TYPE_CRIT] += 0.75f;
        stats_weights_[STATS_TYPE_HASTE] += 0.95f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 1.0f;
    }
    else if (cls == CLASS_DRUID && tab == DRUID_TAB_BALANCE)
    {
        // Hit > Haste to the soft cap > SP > Crit > Intellect > Spirit (Warcraft Tavern; Icy Veins: Hit > SP > Haste).
        // Haste past the GCD soft cap is scaled down in ApplyOverflowPenalty.
        stats_weights_[STATS_TYPE_INTELLECT] += 0.4f;
        stats_weights_[STATS_TYPE_SPIRIT] += 0.3f;
        stats_weights_[STATS_TYPE_SPELL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_HIT] += 1.2f;
        stats_weights_[STATS_TYPE_CRIT] += 0.8f;
        stats_weights_[STATS_TYPE_HASTE] += 1.05f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 1.0f;
    }
    else if (cls == CLASS_MAGE)
    {
        // Arcane / Frost: Hit > SP > Haste > Crit > Spirit (Molten Armor) > Intellect; Fire: Hit > SP > Crit > Haste >
        // Spirit > Intellect (Warcraft Tavern, pvemage335, legacy-wow; wowsims mage EP Haste 0.94, Crit 0.58).
        bool const fire = tab == MAGE_TAB_FIRE;
        stats_weights_[STATS_TYPE_INTELLECT] += 0.3f;
        stats_weights_[STATS_TYPE_SPIRIT] += 0.4f;
        stats_weights_[STATS_TYPE_SPELL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_HIT] += 1.2f;
        stats_weights_[STATS_TYPE_CRIT] += fire ? 0.9f : 0.7f;
        stats_weights_[STATS_TYPE_HASTE] += fire ? 0.8f : 0.9f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 1.0f;
    }
    else if (cls == CLASS_WARLOCK)
    {
        // Affliction: Hit > SP > Haste > Crit >= Spirit (Fel Armor) > Int; Demonology: Hit > SP > Spirit (1 Spirit =
        // 0.59-0.65 SP) > Haste > Crit > Int; Destruction: Hit > SP > Haste >= Crit > Spirit > Int (Warcraft Tavern,
        // Icy Veins, legacy-wow; wowsims warlock EP Hit 0.93, Haste 0.81, Spi 0.54, Crit 0.53, Int 0.18).
        stats_weights_[STATS_TYPE_INTELLECT] += 0.15f;
        stats_weights_[STATS_TYPE_SPELL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_HIT] += 1.2f;
        if (tab == WARLOCK_TAB_DEMONOLOGY)
        {
            stats_weights_[STATS_TYPE_SPIRIT] += 0.65f;
            stats_weights_[STATS_TYPE_HASTE] += 0.6f;
            stats_weights_[STATS_TYPE_CRIT] += 0.5f;
        }
        else if (tab == WARLOCK_TAB_DESTRUCTION)
        {
            stats_weights_[STATS_TYPE_SPIRIT] += 0.5f;
            stats_weights_[STATS_TYPE_HASTE] += 0.75f;
            stats_weights_[STATS_TYPE_CRIT] += 0.7f;
        }
        else
        {
            stats_weights_[STATS_TYPE_SPIRIT] += 0.5f;
            stats_weights_[STATS_TYPE_HASTE] += 0.9f;
            stats_weights_[STATS_TYPE_CRIT] += 0.55f;
        }
        stats_weights_[STATS_TYPE_ATTACK_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 1.0f;
    }
    else if (cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ELEMENTAL)
    {
        // Hit > SP > Haste > Crit > Intellect > MP5 (Warcraft Tavern, Icy Veins, legacy-wow; wowsims Ele EP Haste
        // 1.29, Crit 0.67, Int 0.22, MP5 0.08).
        stats_weights_[STATS_TYPE_INTELLECT] += 0.25f;
        stats_weights_[STATS_TYPE_SPELL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_HIT] += 1.2f;
        stats_weights_[STATS_TYPE_CRIT] += 0.7f;
        stats_weights_[STATS_TYPE_HASTE] += 1.29f;
        stats_weights_[STATS_TYPE_MANA_REGENERATION] += 0.15f;
    }
    else if (cls == CLASS_PALADIN && tab == PALADIN_TAB_HOLY)
    {
        // Intellect > Haste > Crit > SP > MP5, no Spirit (Warmane ICC holy guide; Icy Veins / Warcraft Tavern put MP5
        // above crit for early gear). Brilliant / Quick gems, never SP or MP5 gems.
        stats_weights_[STATS_TYPE_INTELLECT] += 1.2f;
        stats_weights_[STATS_TYPE_HEAL_POWER] += 0.7f;
        stats_weights_[STATS_TYPE_MANA_REGENERATION] += 0.6f;
        stats_weights_[STATS_TYPE_CRIT] += 0.75f;
        stats_weights_[STATS_TYPE_HASTE] += 1.0f;
    }
    else if (cls == CLASS_SHAMAN && tab == SHAMAN_TAB_RESTORATION)
    {
        // Haste > Intellect > SP > Crit > MP5 > Spirit (Icy Veins EP vs SP 1: Haste 1.2, Int 1.1, Crit 0.9; Warcraft
        // Tavern: Int > SP > Haste > Crit > MP5; both gem haste: Reckless red, Quick yellow).
        stats_weights_[STATS_TYPE_INTELLECT] += 1.05f;
        stats_weights_[STATS_TYPE_SPIRIT] += 0.05f;
        stats_weights_[STATS_TYPE_HEAL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_MANA_REGENERATION] += 0.7f;
        stats_weights_[STATS_TYPE_CRIT] += 0.75f;
        stats_weights_[STATS_TYPE_HASTE] += 1.15f;
    }
    else if (cls == CLASS_PRIEST && tab == PRIEST_TAB_DISCIPLINE)
    {
        // Haste to the soft cap > SP > Crit > Haste above it > Int > Spirit > MP5 (Warcraft Tavern, Icy Veins).
        stats_weights_[STATS_TYPE_INTELLECT] += 0.55f;
        stats_weights_[STATS_TYPE_SPIRIT] += 0.45f;
        stats_weights_[STATS_TYPE_HEAL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_MANA_REGENERATION] += 0.4f;
        stats_weights_[STATS_TYPE_CRIT] += 0.8f;
        stats_weights_[STATS_TYPE_HASTE] += 1.05f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 1.0f;
    }
    else if (cls == CLASS_PRIEST)
    {
        // Holy: SP > Haste > Crit > Int > Spirit > MP5 (Warcraft Tavern).
        stats_weights_[STATS_TYPE_INTELLECT] += 0.6f;
        stats_weights_[STATS_TYPE_SPIRIT] += 0.55f;
        stats_weights_[STATS_TYPE_HEAL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_MANA_REGENERATION] += 0.5f;
        stats_weights_[STATS_TYPE_CRIT] += 0.7f;
        stats_weights_[STATS_TYPE_HASTE] += 0.85f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 1.0f;
    }
    else if (cls == CLASS_DRUID && tab == DRUID_TAB_RESTORATION)
    {
        // SP > Haste (to the GCD soft cap) > MP5 > Int > Spirit > Crit, HoTs do not crit (Icy Veins EP vs SP 1: Haste
        // ~1, Spirit ~0.9, Crit ~0.1; Warcraft Tavern: Haste > Spirit > SP > Crit).
        stats_weights_[STATS_TYPE_INTELLECT] += 0.85f;
        stats_weights_[STATS_TYPE_SPIRIT] += 0.8f;
        stats_weights_[STATS_TYPE_HEAL_POWER] += 1.0f;
        stats_weights_[STATS_TYPE_MANA_REGENERATION] += 0.9f;
        stats_weights_[STATS_TYPE_CRIT] += 0.15f;
        stats_weights_[STATS_TYPE_HASTE] += 0.98f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_RANGED_DPS] += 1.0f;
    }
    else if (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_PROTECTION)
    {
        // Defense to 540 > Stamina > Armor > Hit > Expertise > Block rating > Block value > Dodge > Parry > Strength >
        // Agility (Icy Veins phase 5). Defense beats Stamina per point of item budget (1 rating = 1.5 Stamina) until
        // the cap, where ApplyOverflowPenalty drops it to 0.
        stats_weights_[STATS_TYPE_AGILITY] += 0.8f;
        stats_weights_[STATS_TYPE_STRENGTH] += 0.9f;
        stats_weights_[STATS_TYPE_STAMINA] += 3.0f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 0.2f;
        stats_weights_[STATS_TYPE_DEFENSE] += 4.8f;
        stats_weights_[STATS_TYPE_PARRY] += 1.0f;
        stats_weights_[STATS_TYPE_DODGE] += 1.1f;
        stats_weights_[STATS_TYPE_BLOCK_RATING] += 1.4f;
        stats_weights_[STATS_TYPE_BLOCK_VALUE] += 1.2f;
        stats_weights_[STATS_TYPE_ARMOR] += 0.15f;
        stats_weights_[STATS_TYPE_HIT] += 2.8f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 2.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.6f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 2.0f;
    }
    else if (cls == CLASS_PALADIN && tab == PALADIN_TAB_PROTECTION)
    {
        // Defense to 540 > Stamina > Block rating > Expertise > Hit > Dodge > Parry > Strength > Block value >
        // Agility (Icy Veins; Warmane: Block value is situational, Strength adds block value and SP).
        stats_weights_[STATS_TYPE_AGILITY] += 0.7f;
        stats_weights_[STATS_TYPE_STRENGTH] += 0.9f;
        stats_weights_[STATS_TYPE_STAMINA] += 3.0f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 0.2f;
        stats_weights_[STATS_TYPE_DEFENSE] += 4.8f;
        stats_weights_[STATS_TYPE_PARRY] += 1.0f;
        stats_weights_[STATS_TYPE_DODGE] += 1.1f;
        stats_weights_[STATS_TYPE_BLOCK_RATING] += 2.6f;
        stats_weights_[STATS_TYPE_BLOCK_VALUE] += 0.7f;
        stats_weights_[STATS_TYPE_ARMOR] += 0.15f;
        stats_weights_[STATS_TYPE_HIT] += 2.4f;
        stats_weights_[STATS_TYPE_SPELL_POWER] += 0.1f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.5f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 2.0f;
    }
    else if (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_BLOOD)
    {
        // Defense to 540 > Hit > Stamina > Expertise > Dodge > Parry > Strength > Agility (Icy Veins, Warcraft
        // Tavern; Warmane: cap hit / expertise at the cost of minor health).
        stats_weights_[STATS_TYPE_AGILITY] += 0.8f;
        stats_weights_[STATS_TYPE_STRENGTH] += 1.0f;
        stats_weights_[STATS_TYPE_STAMINA] += 3.0f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 0.2f;
        stats_weights_[STATS_TYPE_DEFENSE] += 4.8f;
        stats_weights_[STATS_TYPE_PARRY] += 1.15f;
        stats_weights_[STATS_TYPE_DODGE] += 1.2f;
        stats_weights_[STATS_TYPE_BLOCK_RATING] -= 2.0f;
        stats_weights_[STATS_TYPE_BLOCK_VALUE] -= 2.0f;
        stats_weights_[STATS_TYPE_ARMOR] += 0.15f;
        stats_weights_[STATS_TYPE_HIT] += 3.2f;
        stats_weights_[STATS_TYPE_SPELL_POWER] -= 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.6f;
        stats_weights_[STATS_TYPE_MELEE_DPS] += 2.0f;
    }
    else
    {
        // BEAR DRUID TANK: Stamina > Agility > Expertise > Hit > Strength > Defense > Dodge > Haste > ArP > Crit;
        // crit-immune through Survival of the Fittest, so Defense is avoidance only (Warcraft Tavern equivalency
        // points Sta 3 : Agi 2 : Dodge 1 = Defense 1; Icy Veins; stack Solid Majestic Zircon).
        stats_weights_[STATS_TYPE_AGILITY] += 2.7f;
        stats_weights_[STATS_TYPE_STRENGTH] += 1.8f;
        stats_weights_[STATS_TYPE_STAMINA] += 4.0f;
        stats_weights_[STATS_TYPE_ATTACK_POWER] += 0.6f;
        stats_weights_[STATS_TYPE_DEFENSE] += 1.45f;
        stats_weights_[STATS_TYPE_DODGE] += 1.4f;
        stats_weights_[STATS_TYPE_ARMOR] += 0.15f;
        stats_weights_[STATS_TYPE_ARMOR_PENETRATION] += 0.9f;
        stats_weights_[STATS_TYPE_HIT] += 2.3f;
        stats_weights_[STATS_TYPE_CRIT] += 0.8f;
        stats_weights_[STATS_TYPE_HASTE] += 1.0f;
        stats_weights_[STATS_TYPE_EXPERTISE] += 2.6f;
        // Weapon DPS is taken into account as part of Attack Power for Bear Druids.
    }
}

void StatsWeightCalculator::GenerateAdditionalWeights(Player* player)
{
    uint8 cls = player->getClass();
    // int tab = AiFactory::GetPlayerSpecTab(player);
    if (cls == CLASS_HUNTER)
    {
        if (player->HasAura(SPELL_CAREFUL_AIM))
            stats_weights_[STATS_TYPE_INTELLECT] += 1.1f;
        if (player->HasAura(SPELL_HUNTER_VS_WILD))
            stats_weights_[STATS_TYPE_STAMINA] += 0.3f;
    }
    else if (cls == CLASS_WARRIOR)
    {
        if (player->HasAura(SPELL_ARMORED_TO_THE_TEETH))
            stats_weights_[STATS_TYPE_ARMOR] += 0.03f;
    }
    else if (cls == CLASS_SHAMAN)
    {
        if (player->HasAura(SPELL_MENTAL_DEXTERITY))
            stats_weights_[STATS_TYPE_INTELLECT] += 1.1f;
    }
    else if (cls == CLASS_MAGE)
    {
        // Without Molten Armor (Spirit -> crit) Spirit is only out-of-combat regen.
        if (!HasAnySpell(player, SPELL_MOLTEN_ARMOR_RANKS))
            stats_weights_[STATS_TYPE_SPIRIT] = std::min(stats_weights_[STATS_TYPE_SPIRIT], 0.05f);
    }
    else if (cls == CLASS_WARLOCK)
    {
        // Without Fel Armor (Spirit -> SP) Spirit only feeds Life Tap / regen.
        if (!HasAnySpell(player, SPELL_FEL_ARMOR_RANKS))
            stats_weights_[STATS_TYPE_SPIRIT] = std::min(stats_weights_[STATS_TYPE_SPIRIT], 0.1f);
    }

    // Healers would rather survive the hits (Greater Resistance), casters shed threat (Enchant Cloak - Subtlety),
    // tanks want more threat (Enchant Gloves - Threat). Melee and hunters weigh neither.
    if (healSpec_)
    {
        stats_weights_[STATS_TYPE_THREAT_REDUCTION] += HEALER_THREAT_REDUCTION_WEIGHT;
        stats_weights_[STATS_TYPE_RESISTANCE] += HEALER_RESISTANCE_WEIGHT;
    }
    else if (casterSpec_)
    {
        stats_weights_[STATS_TYPE_THREAT_REDUCTION] += CASTER_THREAT_REDUCTION_WEIGHT;
        stats_weights_[STATS_TYPE_RESISTANCE] += CASTER_RESISTANCE_WEIGHT;
    }
    else if (type_ == CollectorType::MELEE_TANK)
        stats_weights_[STATS_TYPE_THREAT_REDUCTION] += TANK_THREAT_REDUCTION_WEIGHT;

    if (pvpSpec_ && !exclude_resilience_)
        stats_weights_[STATS_TYPE_RESILIENCE] += 7.0f;
}

void StatsWeightCalculator::CollectItemSetBonus(Player* player, ItemTemplate const* proto, int32 slot,
                                                std::array<float, STATS_TYPE_MAX>& setBonusStats)
{
    uint32 itemSet = proto->ItemSet;
    if (!itemSet)
        return;

    ItemSetEntry const* setEntry = sItemSetStore.LookupEntry(itemSet);
    if (!setEntry)
        return;

    // Pieces worn while the candidate is evaluated: the item currently in the candidate's
    // slot (if it is from the same set) is replaced, so it must not count. The candidate
    // itself adds one piece, so only the tier at (worn + 1) is newly activated.
    uint32 worn = 0;
    for (ItemSetEffect* eff : player->ItemSetEff)
    {
        if (eff && eff->setid == itemSet)
        {
            worn = eff->item_count;
            break;
        }
    }

    if (worn && slot >= 0 && slot < EQUIPMENT_SLOT_END)
    {
        if (Item* oldItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(slot)))
        {
            if (oldItem->GetTemplate()->ItemSet == itemSet)
                --worn;
        }
    }

    // Value only the tier this item switches on. Tiers that are already active stay
    // credited to the item providing them, so candidates are compared on their delta.
    // Keep the stats the tier adds and let the caller weigh them after GenerateWeights.
    for (uint32 j = 0; j < MAX_ITEM_SET_SPELLS; ++j)
    {
        uint32 threshold = setEntry->items_to_triggerspell[j];
        uint32 spellId = setEntry->spells[j];
        if (!threshold || !spellId || threshold != worn + 1)
            continue;

        std::array<float, STATS_TYPE_MAX> before{};
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            before[i] = collector_->stats[i];

        collector_->CollectSpellStats(spellId, 1.0f, Milliseconds(0));

        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            setBonusStats[i] += collector_->stats[i] - before[i];
    }
}

void StatsWeightCalculator::CalculateItemSetMod(Player* player, ItemTemplate const* proto)
{
    uint32 itemSet = proto->ItemSet;
    if (!itemSet)
        return;

    float multiplier = 1.0f;
    size_t i = 0;
    for (i = 0; i < player->ItemSetEff.size(); i++)
    {
        if (player->ItemSetEff[i])
        {
            ItemSetEffect* eff = player->ItemSetEff[i];

            uint32 setId = eff->setid;
            if (itemSet != setId)
                continue;

            ItemSetEntry const* setEntry = sItemSetStore.LookupEntry(setId);
            if (!setEntry)
                continue;

            uint32 itemCount = eff->item_count;
            uint32 max_items = 0;
            for (size_t j = 0; j < MAX_ITEM_SET_SPELLS; j++)
                max_items = std::max(max_items, setEntry->items_to_triggerspell[j]);
            if (itemCount < max_items)
            {
                multiplier += 0.1f * itemCount;  // 10% bonus for each item already equipped
            }
            else
            {
                multiplier = 1.0f;  // All item set effect has been triggered
            }
            break;
        }
    }

    if (i == player->ItemSetEff.size())
        multiplier = 1.05f;  // this is the first item in the item set

    weight_ *= multiplier;
}

void StatsWeightCalculator::CalculateSocketBonus(Player* /*player*/, ItemTemplate const* proto)
{
    uint32 socketNum = 0;
    for (uint32 enchant_slot = SOCK_ENCHANTMENT_SLOT; enchant_slot < SOCK_ENCHANTMENT_SLOT + MAX_GEM_SOCKETS;
         ++enchant_slot)
    {
        uint8 socketColor = proto->Socket[enchant_slot - SOCK_ENCHANTMENT_SLOT].Color;

        if (!socketColor)  // no socket slot
            continue;

        socketNum++;
    }

    float multiplier = 1.0f + socketNum * 0.03f;  // 3% bonus for socket

    weight_ *= multiplier;
}

void StatsWeightCalculator::CalculateItemTypePenalty(ItemTemplate const* proto)
{
    // // penalty for different type armor
    // if (proto->Class == ITEM_CLASS_ARMOR && proto->SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH &&
    //     proto->SubClass <= ITEM_SUBCLASS_ARMOR_PLATE && NotBestArmorType(proto->SubClass))
    // {
    //     weight_ *= 1.0;
    // }
    if (proto->Class == ITEM_CLASS_WEAPON)
    {
        // double hand
        bool isDoubleHand = proto->Class == ITEM_CLASS_WEAPON &&
                            !(ITEM_SUBCLASS_MASK_SINGLE_HAND & (1 << proto->SubClass)) &&
                            !(ITEM_SUBCLASS_MASK_WEAPON_RANGED & (1 << proto->SubClass));

        if (isDoubleHand)
        {
            weight_ *= 0.5;
            // spec without double hand
            // enhancement, rogue, ice dk, unholy dk, shield tank, fury warrior without titan's grip but with duel wield
            if (((cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT && player_->CanDualWield()) ||
                 (cls == CLASS_ROGUE) || (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_FROST) ||
                 (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY && !player_->CanTitanGrip() &&
                  player_->CanDualWield()) ||
                 (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_PROTECTION) ||
                 (cls == CLASS_PALADIN && tab == PALADIN_TAB_PROTECTION) ||
                 (cls == CLASS_PALADIN && tab == PALADIN_TAB_HOLY)))
            {
                weight_ *= 0.1;
            }
        }
        // spec with double hand
        // fury without duel wield, arms, bear, retribution, blood dk
        if (!isDoubleHand)
        {
            if ((cls == CLASS_HUNTER && !player_->CanDualWield()) ||
                (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY && !player_->CanDualWield()) ||
                (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_ARMS) || (cls == CLASS_DRUID && tab == DRUID_TAB_FERAL) ||
                (cls == CLASS_PALADIN && tab == PALADIN_TAB_RETRIBUTION) ||
                (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_BLOOD) ||
                (cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT && !player_->CanDualWield()))
            {
                weight_ *= 0.1;
            }
            // caster's main hand (cannot duel weapon but can equip two-hands stuff)
            if ((cls == CLASS_MAGE || cls == CLASS_PRIEST || cls == CLASS_WARLOCK || cls == CLASS_DRUID ||
                (cls == CLASS_SHAMAN && !player_->CanDualWield())) &&
                !(cls == CLASS_PALADIN && tab == PALADIN_TAB_HOLY))
            {
                weight_ *= 0.65;
            }
            if (cls == CLASS_PALADIN && tab == PALADIN_TAB_HOLY)
            {
                weight_ *= 0.8;
            }
        }
        // fury with titan's grip
        if ((!isDoubleHand || proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM ||
             proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF) &&
            (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY && player_->CanTitanGrip()))
        {
            weight_ *= 0.1;
        }

        if (cls == CLASS_HUNTER && proto->SubClass == ITEM_SUBCLASS_WEAPON_THROWN)
        {
            weight_ *= 0.1;
        }

        if (lvl >= 10 && cls == CLASS_ROGUE && (tab == ROGUE_TAB_ASSASSINATION || tab == ROGUE_TAB_SUBTLETY) &&
            proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER)
        {
            weight_ *= 1.5;
        }

        if (cls == CLASS_ROGUE && player_->HasAura(SPELL_ROGUE_SWORD_SPECIALIZATION) &&
            (proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD || proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE))
        {
            weight_ *= 1.1;
        }
        if (cls == CLASS_WARRIOR && player_->HasAura(SPELL_POLEAXE_SPECIALIZATION) &&
            (proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM || proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2))
        {
            weight_ *= 1.1;
        }
        if (cls == CLASS_DEATH_KNIGHT && player_->HasAura(SPELL_NERVES_OF_COLD_STEEL) && !isDoubleHand)
        {
            weight_ *= 1.3;
        }
        bool slowDelay = proto->Delay > 2500;
        if (cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT && slowDelay)
            weight_ *= 1.1;
    }
}

bool StatsWeightCalculator::NotBestArmorType(uint32 item_subclass_armor)
{
    if (player_->HasSkill(SKILL_PLATE_MAIL))
    {
        return item_subclass_armor != ITEM_SUBCLASS_ARMOR_PLATE;
    }
    if (player_->HasSkill(SKILL_MAIL))
    {
        return item_subclass_armor != ITEM_SUBCLASS_ARMOR_MAIL;
    }
    if (player_->HasSkill(SKILL_LEATHER))
    {
        return item_subclass_armor != ITEM_SUBCLASS_ARMOR_LEATHER;
    }
    return false;
}

void StatsWeightCalculator::ApplyOverflowPenalty(Player* player)
{
    {
        // A Draenei party member out of aura range does not show in GetTotalAuraModifier.
        bool const heroicPresenceMissing = hitComposition_.heroicPresence &&
                                           !player->HasAura(SPELL_HEROIC_PRESENCE_MELEE) &&
                                           !player->HasAura(SPELL_HEROIC_PRESENCE_CASTER);
        float const heroicPresenceBonus = heroicPresenceMissing ? 1.0f : 0.0f;
        float hit_current, hit_overflow;
        float validPoints;
        if (hitOverflowType_ & CollectorType::SPELL)
        {
            hit_current = player->GetTotalAuraModifier(SPELL_AURA_MOD_SPELL_HIT_CHANCE);
            hit_current +=
                player->GetTotalAuraModifier(SPELL_AURA_MOD_INCREASES_SPELL_PCT_TO_HIT);  // suppression (18176)
            hit_current += player->GetRatingBonusValue(CR_HIT_SPELL);
            hit_current += heroicPresenceBonus;

            if (cls == CLASS_PRIEST && tab == PRIEST_TAB_SHADOW && player->HasAura(SPELL_SHADOW_FOCUS))
                hit_current += 3;
            if (cls == CLASS_MAGE && tab == MAGE_TAB_ARCANE && player->HasAura(SPELL_ARCANE_FOCUS))
                hit_current += 3;

            hit_overflow = float(SPELL_HIT_OVERFLOW) - float(hitComposition_.spellHitDebuff);
            if (hit_overflow > hit_current)
                validPoints = (hit_overflow - hit_current) / player->GetRatingMultiplier(CR_HIT_SPELL);
            else
                validPoints = 0;
        }
        else if (hitOverflowType_ & CollectorType::MELEE)
        {
            hit_current = player->GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE);
            hit_current += player->GetRatingBonusValue(CR_HIT_MELEE);
            hit_current += heroicPresenceBonus;
            hit_overflow = MELEE_HIT_OVERFLOW;
            if (hit_overflow > hit_current)
                validPoints = (hit_overflow - hit_current) / player->GetRatingMultiplier(CR_HIT_MELEE);
            else
                validPoints = 0;
        }
        else
        {
            hit_current = player->GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE);
            hit_current += player->GetRatingBonusValue(CR_HIT_RANGED);
            hit_current += heroicPresenceBonus;
            hit_overflow = RANGED_HIT_OVERFLOW;
            if (hit_overflow > hit_current)
                validPoints = (hit_overflow - hit_current) / player->GetRatingMultiplier(CR_HIT_RANGED);
            else
                validPoints = 0;
        }

        float const itemHit = collector_->stats[STATS_TYPE_HIT];
        float const meleeHit = player->GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE) +
                               player->GetRatingBonusValue(CR_HIT_MELEE) + heroicPresenceBonus;
        float fullPoints = validPoints;  // hit worth its full weight
        float capPoints = validPoints;  // end of the hit that still helps the spec's yellow attacks or spells
        if (meleeSpellHitBandFactor_ < 1.0f)
        {
            // Rogue / Enhancement / Unholy: full value to the 8% special cap, reduced value up to the spell cap.
            float const yellowPoints = std::max(0.0f, (float(MELEE_HIT_OVERFLOW) - meleeHit) /
                                                          player->GetRatingMultiplier(CR_HIT_MELEE));
            fullPoints = yellowPoints;
            capPoints = std::max(yellowPoints, validPoints);
        }
        float effectiveHit = std::min(itemHit, fullPoints) +
                             std::clamp(itemHit - fullPoints, 0.0f, capPoints - fullPoints) * meleeSpellHitBandFactor_;
        if (dualWield_ && itemHit > capPoints)
        {
            // Past the special-attack (or spell) cap, hit still removes white-swing misses up to the dual-wield cap.
            float const whitePoints =
                std::max(0.0f, (float(MELEE_DUAL_WIELD_HIT_OVERFLOW) - meleeHit) /
                                   player->GetRatingMultiplier(CR_HIT_MELEE));
            float const extraHit = std::clamp(itemHit - capPoints, 0.0f, std::max(0.0f, whitePoints - capPoints));
            effectiveHit += extraHit * DUAL_WIELD_HIT_PAST_CAP_FACTOR;
        }
        collector_->stats[STATS_TYPE_HIT] = effectiveHit;
    }

    {
        if (type_ & CollectorType::MELEE)
        {
            // PLAYER_EXPERTISE already includes the expertise rating (Player::UpdateExpertise).
            float const expertise_current = player->GetUInt32Value(PLAYER_EXPERTISE);
            float const expertiseMultiplier = player->GetRatingMultiplier(CR_EXPERTISE);

            float const dodgeCap = float(EXPERTISE_OVERFLOW);
            float validPoints;
            if (dodgeCap > expertise_current)
                validPoints = (dodgeCap - expertise_current) / expertiseMultiplier;
            else
                validPoints = 0;

            float const itemExpertise = collector_->stats[STATS_TYPE_EXPERTISE];
            float effectiveExpertise = std::min(itemExpertise, validPoints);
            if (type_ == CollectorType::MELEE_TANK && itemExpertise > validPoints)
            {
                float const parryPoints =
                    std::max(0.0f, (float(EXPERTISE_PARRY_OVERFLOW) - expertise_current) / expertiseMultiplier);
                float const extraExpertise =
                    std::clamp(itemExpertise - validPoints, 0.0f, std::max(0.0f, parryPoints - validPoints));
                effectiveExpertise += extraExpertise * TANK_EXPERTISE_PAST_DODGE_CAP_FACTOR;
            }
            collector_->stats[STATS_TYPE_EXPERTISE] = effectiveExpertise;
        }
    }

    {
        if (type_ & CollectorType::MELEE)
        {
            float defense_current, defense_overflow;
            defense_current = player->GetRatingBonusValue(CR_DEFENSE_SKILL);
            defense_overflow = DEFENSE_OVERFLOW;

            float validPoints;
            if (defense_overflow > defense_current)
                validPoints = (defense_overflow - defense_current) / player->GetRatingMultiplier(CR_DEFENSE_SKILL);
            else
                validPoints = 0;

            collector_->stats[STATS_TYPE_DEFENSE] = std::min(collector_->stats[STATS_TYPE_DEFENSE], validPoints);
        }
    }

    {
        if (type_ & (CollectorType::MELEE | CollectorType::RANGED))
        {
            // Gear ArP only: an active trinket proc must not make the cap look reached. Past the soft cap
            // (100% minus the trinket proc) a point is wasted whenever the proc is up.
            float const multiplier = player->GetRatingMultiplier(CR_ARMOR_PENETRATION);
            float const hardPoints = std::max(0.0f, (float(ARMOR_PENETRATION_OVERFLOW) - gearArpPct_) / multiplier);
            float const softCapPct = float(ARMOR_PENETRATION_OVERFLOW) - procArpPct_;
            float const softPoints = std::min(hardPoints, std::max(0.0f, (softCapPct - gearArpPct_) / multiplier));

            float const itemArp = collector_->stats[STATS_TYPE_ARMOR_PENETRATION];
            collector_->stats[STATS_TYPE_ARMOR_PENETRATION] =
                std::min(itemArp, softPoints) +
                std::clamp(itemArp - softPoints, 0.0f, hardPoints - softPoints) * (1.0f - ARP_PROC_UPTIME);
        }
    }

    if (hasteSoftCapPct_ > 0.0f)
    {
        float const capPoints =
            std::max(0.0f, (hasteSoftCapPct_ - gearHastePct_) / player->GetRatingMultiplier(CR_HASTE_SPELL));
        float const itemHaste = collector_->stats[STATS_TYPE_HASTE];
        if (itemHaste > capPoints)
            collector_->stats[STATS_TYPE_HASTE] = capPoints + (itemHaste - capPoints) * hastePastCapFactor_;
    }
}

void StatsWeightCalculator::ApplyWeightFinetune(Player* /*player*/)
{
    if (type_ & (CollectorType::MELEE | CollectorType::RANGED))
    {
        // ArP grows in value as it is stacked (see ArpMarginal; the old flat x1.2 past 50% was this curve's value
        // at 50%). A stacking spec values ArP at its main stat's weight exactly at the crossover: below it Strength
        // / Agility wins (gems, enchants, item choice), above it ArP wins up to the cap.
        float& arp = stats_weights_[STATS_TYPE_ARMOR_PENETRATION];
        float const marginal = ArpMarginal(gearArpPct_);
        if (arpCrossoverPct_ > 0.0f)
            arp = stats_weights_[arpMainStat_] * marginal / ArpMarginal(arpCrossoverPct_);
        else
            arp *= marginal / ArpMarginal(0.0f);
    }
}

float StatsWeightCalculator::ApplyPreferredSpecWeapons(ItemTemplate const* proto, int32 slot)
{
    // Multiply score by 3x when this weapon's delay matches the spec-ideal speed.
    float weight = 2.0f;

    // Applies to mainhand, offhand, and ranged slots only.
    if (slot != EQUIPMENT_SLOT_MAINHAND &&
        slot != EQUIPMENT_SLOT_OFFHAND  &&
        slot != EQUIPMENT_SLOT_RANGED)
        return 1.0f;

    uint32 delay = proto->Delay;  // milliseconds
    float boost = 1.0f + weight;  // applied on a match

    // Hunter: melee weapons are stat sticks — speed irrelevant.
    // Ranged weapons scale Aimed/Chimera/Explosive Shot from top-end damage,
    // so a slow ranged weapon (>=2600 ms) is strongly preferred.
    if (cls == CLASS_HUNTER)
    {
        if (slot == EQUIPMENT_SLOT_RANGED && delay >= 2600)
            return boost;
        return 1.0f;
    }

    // Feral Druid: forms normalise attack speed; raw weapon Delay is irrelevant.
    if (cls == CLASS_DRUID && tab == DRUID_TAB_FERAL)
        return 1.0f;

    switch (cls)
    {
        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_ARMS)
            {
                // Arms: slow 2H axes or polearms in mainhand only (Axe Specialization: +5% crit).
                bool isAxeOrPolearm = (proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                                       proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM);
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400 && isAxeOrPolearm)
                    return boost;
            }
            else if (tab == WARRIOR_TAB_FURY)
            {
                if (!player_->CanDualWield())
                {
                    // Pre-DW: treat like Arms — slow 2H in mainhand only.
                    if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                        return boost;
                }
                else if (player_->CanTitanGrip())
                {
                    // Titan's Grip: slow 2H (>=3400) in both hands.
                    if (delay >= 3400)
                        return boost;
                }
                else
                {
                    // 1H DW: slow 1H (>=2600) in both hands.
                    // 2H must be excluded — delay >= 2600 would otherwise pass
                    // for a 2H heirloom (~3600ms) just as it did for Enhancement.
                    if (proto->InventoryType == INVTYPE_2HWEAPON)
                        break;
                    if (delay >= 2600)
                        return boost;
                }
            }
            else if (tab == WARRIOR_TAB_PROTECTION)
            {
                // Prot: slow 1H (>=2600) in mainhand. Shield in offhand, no speed bonus.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 2600)
                    return boost;
            }
            break;

        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_RETRIBUTION)
            {
                // Ret: slow 2H in mainhand only.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                    return boost;
            }
            else if (tab == PALADIN_TAB_PROTECTION)
            {
                // Prot: slow 1H (>=2600) in mainhand. Shield in offhand.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 2600)
                    return boost;
            }
            break;

        case CLASS_DEATH_KNIGHT:
            if (tab == DEATH_KNIGHT_TAB_BLOOD || tab == DEATH_KNIGHT_TAB_UNHOLY)
            {
                // Blood / Unholy: slow 2H in mainhand only.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                    return boost;
            }
            else if (tab == DEATH_KNIGHT_TAB_FROST)
            {
                // Frost DK has Dual Wield innately — always dual-wields 1H.
                if (proto->InventoryType == INVTYPE_2HWEAPON)
                    break;
                if (delay >= 2600)
                    return boost;
            }
            break;

        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_ENHANCEMENT)
            {
                if (!player_->CanDualWield())
                {
                    // Pre-Dual Wield: Enhancement plays like a 2H spec.
                    if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                        return boost;
                }
                else
                {
                    // Post-Dual Wield: slow 1H (>=2600) in both hands.
                    if (proto->InventoryType == INVTYPE_2HWEAPON)
                        break;

                    if (delay >= 2600)
                    {
                        float mult = boost;
                        if (slot == EQUIPMENT_SLOT_OFFHAND)
                        {
                            Item* mh = player_->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                            if (mh && mh->GetTemplate() && mh->GetTemplate()->Delay == delay)
                                mult *= boost;  // synchronized: ×(1+weight)² total = ×9 for 2.0f weight
                        }
                        return mult;
                    }
                }
            }
            break;

        case CLASS_ROGUE:
            if (tab == ROGUE_TAB_COMBAT)
            {
                // Combat: slow MH (>=2600), fast OH (<=1500).
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 2600)
                    return boost;
                if (slot == EQUIPMENT_SLOT_OFFHAND && delay <= 1500)
                    return boost;
            }
            else  // Assassination or Subtlety: slow dagger MH, fast dagger OH.
            {
                bool isDagger = (proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER);
                if (slot == EQUIPMENT_SLOT_MAINHAND && isDagger && delay >= 1700)
                    return boost;
                if (slot == EQUIPMENT_SLOT_OFFHAND && isDagger && delay <= 1500)
                    return boost;
            }
            break;

        default:
            break;
    }

    return 1.0f;
}
