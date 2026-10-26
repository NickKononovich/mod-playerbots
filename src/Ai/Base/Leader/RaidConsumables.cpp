/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidConsumables.h"

#include "Bag.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    enum class ConsumableRole : uint8
    {
        Tank,
        Physical,
        Caster,
        Healer
    };

    constexpr int32 SCORE_BEST = 10;
    constexpr int32 SCORE_GOOD = 8;
    constexpr int32 SCORE_RATING = 6;
    constexpr int32 SCORE_MINOR = 4;

    ConsumableRole RoleOf(Player* bot)
    {
        if (PlayerbotAI::IsTank(bot))
            return ConsumableRole::Tank;
        if (PlayerbotAI::IsHeal(bot))
            return ConsumableRole::Healer;
        if (PlayerbotAI::IsCaster(bot))
            return ConsumableRole::Caster;
        return ConsumableRole::Physical;
    }

    template <typename F>
    void ForEachBagItem(Player* bot, F&& fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        fn(item);
    }

    bool HasRatingBit(int32 mask, CombatRating rating) { return mask & (1 << rating); }

    int32 RatingScore(int32 mask, ConsumableRole role)
    {
        bool const tank = HasRatingBit(mask, CR_DEFENSE_SKILL) || HasRatingBit(mask, CR_DODGE) ||
                          HasRatingBit(mask, CR_PARRY) || HasRatingBit(mask, CR_BLOCK);
        bool const physical = HasRatingBit(mask, CR_HIT_MELEE) || HasRatingBit(mask, CR_HIT_RANGED) ||
                              HasRatingBit(mask, CR_CRIT_MELEE) || HasRatingBit(mask, CR_CRIT_RANGED) ||
                              HasRatingBit(mask, CR_HASTE_MELEE) || HasRatingBit(mask, CR_HASTE_RANGED) ||
                              HasRatingBit(mask, CR_EXPERTISE) || HasRatingBit(mask, CR_ARMOR_PENETRATION);
        bool const spell = HasRatingBit(mask, CR_HIT_SPELL) || HasRatingBit(mask, CR_CRIT_SPELL) ||
                           HasRatingBit(mask, CR_HASTE_SPELL);

        switch (role)
        {
            case ConsumableRole::Tank:
                return tank ? SCORE_RATING : 0;
            case ConsumableRole::Physical:
                return physical ? SCORE_RATING : 0;
            case ConsumableRole::Caster:
            case ConsumableRole::Healer:
                return spell ? SCORE_RATING : 0;
        }
        return 0;
    }

    int32 StatScore(int32 stat, ConsumableRole role)
    {
        switch (role)
        {
            case ConsumableRole::Tank:
                return stat == STAT_STAMINA ? SCORE_GOOD : (stat == STAT_AGILITY ? SCORE_MINOR : 0);
            case ConsumableRole::Physical:
                return (stat == STAT_STRENGTH || stat == STAT_AGILITY) ? SCORE_GOOD : 0;
            case ConsumableRole::Caster:
            case ConsumableRole::Healer:
                return (stat == STAT_INTELLECT || stat == STAT_SPIRIT) ? SCORE_MINOR : 0;
        }
        return 0;
    }

    int32 AuraScore(SpellEffectInfo const& effect, ConsumableRole role)
    {
        if (!effect.IsAura())
            return 0;

        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_MOD_INCREASE_HEALTH:
            case SPELL_AURA_MOD_INCREASE_HEALTH_2:
                return role == ConsumableRole::Tank ? SCORE_BEST : 0;
            case SPELL_AURA_MOD_RESISTANCE:
                return (role == ConsumableRole::Tank && (effect.MiscValue & SPELL_SCHOOL_MASK_NORMAL)) ? SCORE_GOOD
                                                                                                    : 0;
            case SPELL_AURA_MOD_ATTACK_POWER:
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                return role == ConsumableRole::Physical ? SCORE_BEST : 0;
            case SPELL_AURA_MOD_DAMAGE_DONE:
                if (role == ConsumableRole::Caster)
                    return SCORE_BEST;
                return role == ConsumableRole::Healer ? SCORE_GOOD : 0;
            case SPELL_AURA_MOD_HEALING_DONE:
                return role == ConsumableRole::Healer ? SCORE_BEST : 0;
            case SPELL_AURA_MOD_POWER_REGEN:
                return (role == ConsumableRole::Healer && effect.MiscValue == POWER_MANA) ? SCORE_GOOD : 0;
            case SPELL_AURA_MOD_STAT:
                return StatScore(effect.MiscValue, role);
            case SPELL_AURA_MOD_RATING:
                return RatingScore(effect.MiscValue, role);
            default:
                return 0;
        }
    }

    int32 SpellScore(SpellInfo const* spellInfo, ConsumableRole role)
    {
        if (!spellInfo)
            return 0;

        int32 score = 0;
        for (SpellEffectInfo const& effect : spellInfo->Effects)
            score += AuraScore(effect, role);
        return score;
    }

    uint32 UseSpellOf(ItemTemplate const* proto)
    {
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (proto->Spells[i].SpellId > 0 && proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                return uint32(proto->Spells[i].SpellId);
        return 0;
    }

    // Buff food applies "Well Fed" through a periodic trigger after the bot has eaten for a while.
    SpellInfo const* WellFedSpellOf(SpellInfo const* foodSpell, uint8 depth = 0)
    {
        if (!foodSpell || depth > 1)
            return nullptr;

        for (SpellEffectInfo const& effect : foodSpell->Effects)
        {
            if (!effect.TriggerSpell)
                continue;

            if (effect.IsAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL))
                return sSpellMgr->GetSpellInfo(effect.TriggerSpell);

            if (effect.Effect == SPELL_EFFECT_TRIGGER_SPELL)
                if (SpellInfo const* wellFed = WellFedSpellOf(sSpellMgr->GetSpellInfo(effect.TriggerSpell), depth + 1))
                    return wellFed;
        }

        return nullptr;
    }

    bool CanUse(Player* bot, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        return proto && proto->Class == ITEM_CLASS_CONSUMABLE && bot->CanUseItem(proto) == EQUIP_ERR_OK;
    }
}

namespace RaidConsumables
{
    bool Allowed(Player* bot)
    {
        Map* map = bot->GetMap();
        // Raids and heroic dungeons: the fights where a real raider brings flasks, buff food and potions.
        return sPlayerbotAIConfig.RaidConsumables && map && (map->IsRaid() || (map->IsDungeon() && map->IsHeroic())) &&
               !bot->InBattleground();
    }

    ObjectGuid FindFlask(Player* bot)
    {
        bool hasBattle = false;
        bool hasGuardian = false;
        for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
        {
            hasBattle = hasBattle || sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_BATTLE);
            hasGuardian = hasGuardian || sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_GUARDIAN);
        }

        if (hasBattle && hasGuardian)
            return ObjectGuid::Empty;

        ConsumableRole const role = RoleOf(bot);
        Item* best = nullptr;
        int32 bestScore = 0;
        ForEachBagItem(bot,
                       [&](Item* item)
                       {
                           if (!CanUse(bot, item))
                               return;

                           ItemTemplate const* proto = item->GetTemplate();
                           if (proto->SubClass != ITEM_SUBCLASS_FLASK && proto->SubClass != ITEM_SUBCLASS_ELIXIR)
                               return;

                           uint32 const spellId = UseSpellOf(proto);
                           bool const battle = sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_BATTLE);
                           bool const guardian =
                               sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_GUARDIAN);
                           if ((!battle && !guardian) || (battle && hasBattle) || (guardian && hasGuardian))
                               return;

                           int32 score = SpellScore(sSpellMgr->GetSpellInfo(spellId), role);
                           if (score <= 0)
                               return;

                           if (battle && guardian)
                               score += SCORE_BEST;  // a flask fills both slots

                           if (score > bestScore || (score == bestScore && best &&
                                                     proto->ItemLevel > best->GetTemplate()->ItemLevel))
                           {
                               best = item;
                               bestScore = score;
                           }
                       });

        return best ? best->GetGUID() : ObjectGuid::Empty;
    }

    ObjectGuid FindFoodBuff(Player* bot)
    {
        ConsumableRole const role = RoleOf(bot);
        Item* best = nullptr;
        int32 bestScore = 0;
        uint32 wellFedIcon = 0;
        uint32 bestWellFed = 0;
        ForEachBagItem(bot,
                       [&](Item* item)
                       {
                           if (!CanUse(bot, item) || item->GetTemplate()->SubClass != ITEM_SUBCLASS_FOOD)
                               return;

                           SpellInfo const* wellFed =
                               WellFedSpellOf(sSpellMgr->GetSpellInfo(UseSpellOf(item->GetTemplate())));
                           if (!wellFed)
                               return;

                           wellFedIcon = wellFed->SpellIconID;
                           int32 const score = SpellScore(wellFed, role);
                           if (score > bestScore)
                           {
                               best = item;
                               bestScore = score;
                               bestWellFed = wellFed->Id;
                           }
                       });

        if (!best || bot->HasAura(bestWellFed))
            return ObjectGuid::Empty;

        // Any Well Fed counts: they share the icon, and another food would only replace it.
        for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
            if (aurApp->IsPositive() && aurApp->GetBase()->GetSpellInfo()->SpellIconID == wellFedIcon)
                return ObjectGuid::Empty;

        return best->GetGUID();
    }

    ObjectGuid FindPrePullPotion(Player* bot)
    {
        uint32 candidates[2] = {0, 0};
        switch (RoleOf(bot))
        {
            case ConsumableRole::Tank:
                candidates[0] = ITEM_INDESTRUCTIBLE_POTION;
                break;
            case ConsumableRole::Physical:
                candidates[0] = ITEM_POTION_OF_SPEED;
                break;
            case ConsumableRole::Caster:
                candidates[0] = ITEM_POTION_OF_WILD_MAGIC;
                candidates[1] = ITEM_POTION_OF_SPEED;
                break;
            case ConsumableRole::Healer:
                candidates[0] = ITEM_POTION_OF_WILD_MAGIC;
                break;
        }

        for (uint32 entry : candidates)
        {
            if (!entry)
                continue;

            Item* item = bot->GetItemByEntry(entry);
            if (item && CanUse(bot, item))
                return item->GetGUID();
        }

        return ObjectGuid::Empty;
    }

    Item* FindHealthstone(Player* bot)
    {
        Item* found = nullptr;
        ForEachBagItem(bot,
                       [&](Item* item)
                       {
                           if (found || !CanUse(bot, item))
                               return;

                           SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(UseSpellOf(item->GetTemplate()));
                           if (spellInfo && spellInfo->SpellFamilyName == SPELLFAMILY_WARLOCK &&
                               spellInfo->HasEffect(SPELL_EFFECT_HEAL))
                               found = item;
                       });

        return found;
    }

    bool IsSoulwell(GameObject* go)
    {
        if (!go)
            return false;

        switch (go->GetEntry())
        {
            case GO_SOULWELL_RANK_1:
            case GO_SOULWELL_RANK_1_TALENT_1:
            case GO_SOULWELL_RANK_1_TALENT_2:
            case GO_SOULWELL_RANK_2:
            case GO_SOULWELL_RANK_2_TALENT_1:
            case GO_SOULWELL_RANK_2_TALENT_2:
                return true;
            default:
                return false;
        }
    }

    bool IsSoulPortal(GameObject* go)
    {
        // Ritual of Refreshment uses the same "two other members click the portal" step as Ritual of Souls.
        return go && (go->GetEntry() == GO_SOUL_PORTAL_RANK_1 || go->GetEntry() == GO_SOUL_PORTAL_RANK_2 ||
                      go->GetEntry() == GO_REFRESHMENT_PORTAL_RANK_1 ||
                      go->GetEntry() == GO_REFRESHMENT_PORTAL_RANK_2);
    }

    bool IsRefreshmentTable(GameObject* go)
    {
        return go && (go->GetEntry() == GO_REFRESHMENT_TABLE_RANK_1 || go->GetEntry() == GO_REFRESHMENT_TABLE_RANK_2);
    }

    // [b43_class] brd-h 20261026-060218-brd-h-5F-5, 480-488 s: the warlock started Ritual of Souls 58887 (High
    // Interrogator Gerstahn within 45 yd counted as "before the boss"), and the tank and the rogue clicked its portal
    // (ACTION_NORMAL + 4) right at "pull start (106)". The tank channelled ~8 s instead of pulling, the druid's heals
    // took Gerstahn. A ritual is for a real rest or the run start, never next to a pack.
    std::string RitualBlockedReason(PlayerbotAI* botAI)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        if (!bot)
            return "no bot";
        if (bot->IsInCombat())
            return "in combat";

        Map* map = bot->GetMap();
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && member->IsAlive() && member->IsInCombat() && member->GetMap() == map &&
                    bot->GetDistance(member) <= RITUAL_GROUP_COMBAT_RANGE)
                    return member->GetName() + " is in combat";
            }
        }

        // The bot is the puller: its own pull is requested, running or targeted.
        if (PullStrategy* pull = PullStrategy::Get(botAI))
        {
            if (pull->IsPullPendingToStart() || pull->HasPullStarted() || pull->HasTarget())
                return "its own pull is on";
        }

        AiObjectContext* context = botAI->GetAiObjectContext();
        // The main tank has an elite / boss target within 40 yd (members only; false for the tank itself).
        if (context->GetValue<bool>("pull imminent")->Get())
            return "pull imminent";

        for (ObjectGuid const guid : context->GetValue<GuidVector>("possible targets")->Get())
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || unit->IsPlayer())
                continue;

            float const distance = bot->GetDistance(unit);
            if (distance <= RITUAL_HOSTILE_RANGE)
                return unit->GetName() + " at " + std::to_string(static_cast<int32>(distance)) + " yd";
        }

        return "";
    }
}
