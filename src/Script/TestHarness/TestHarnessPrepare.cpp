/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestHarnessPrepare.h"

#include "Bag.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Pet.h"  // [rot110] PetFoodFor
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace TestHarness
{
    namespace
    {
        constexpr uint16 PROFESSION_SKILL = 450;  // the perks below need the profession at Grand Master
        constexpr uint16 SKILL_PER_STEP = 75;      // Apprentice 75, Journeyman 150, ... Grand Master 450
        constexpr uint8 PROFESSION_STEPS = 6;
        constexpr uint32 MAX_GEAR_PASSES = 6;
        // Stop once the average lands within this of the target (3.0 let whole groups stop 3 below it). Above the
        // target counts double: the gear stays below the content (run_start warns from target + 0.5).
        constexpr float GEAR_TOLERANCE = 1.0f;

        float GearMiss(float delta) { return delta > 0.0f ? 2.0f * delta : -delta; }

        // Spell ids verified in C:\WoW\data\dbc\Spell.dbc.
        constexpr uint32 SPELL_COLD_WEATHER_FLYING = 54197;
        constexpr uint32 SPELL_MIXOLOGY = 53042;
        constexpr uint32 SPELL_TOUGHNESS_RANK_6 = 53040;
        constexpr uint32 SPELL_MASTER_OF_ANATOMY_RANK_6 = 53666;
        constexpr uint32 SPELL_LIFEBLOOD_RANK_6 = 55503;

        // SpellItemEnchantment ids, verified in SpellItemEnchantment.dbc (each requires its profession at 400).
        constexpr uint32 ENCHANT_SOCKET_BRACER = 3717;
        constexpr uint32 ENCHANT_SOCKET_GLOVES = 3723;
        constexpr uint32 ENCHANT_HYPERSPEED_ACCELERATORS = 3604;
        constexpr uint32 ENCHANT_LIGHTWEAVE = 3722;
        constexpr uint32 ENCHANT_DARKGLOW = 3728;
        constexpr uint32 ENCHANT_SWORDGUARD = 3730;
        constexpr uint32 ENCHANT_MASTERS_AXE = 3835;
        constexpr uint32 ENCHANT_MASTERS_CRAG = 3836;
        constexpr uint32 ENCHANT_MASTERS_PINNACLE = 3837;
        constexpr uint32 ENCHANT_MASTERS_STORM = 3838;
        constexpr uint32 ENCHANT_FUR_ATTACK_POWER = 3756;
        constexpr uint32 ENCHANT_FUR_STAMINA = 3757;
        constexpr uint32 ENCHANT_FUR_SPELL_POWER = 3758;
        constexpr uint32 ENCHANT_RING_ASSAULT = 3839;
        constexpr uint32 ENCHANT_RING_SPELLPOWER = 3840;
        constexpr uint32 ENCHANT_RING_STAMINA = 3791;

        constexpr uint32 MAX_DRAGONS_EYES = 3;

        constexpr uint32 ITEM_SYMBOL_OF_KINGS = 21177;
        constexpr uint32 ITEM_DEVOUT_CANDLE = 44615;
        constexpr uint32 ITEM_WILD_SPINELEAF = 44605;
        constexpr uint32 ITEM_STARLEAF_SEED = 44614;
        constexpr uint32 ITEM_ARCANE_POWDER = 17020;
        constexpr uint32 ITEM_CORPSE_DUST = 37201;
        constexpr uint32 ITEM_SOUL_SHARD = 6265;
        constexpr uint32 ITEM_INFERNAL_STONE = 5565;  // [b35_lock] Inferno's reagent (one per cast, 10 min cooldown)
        constexpr uint32 INFERNO_MIN_LEVEL = 50;
        constexpr uint32 ITEM_ANKH = 17030;

        struct ProfessionSpells
        {
            uint16 skill;
            uint32 ranks[PROFESSION_STEPS];  // Apprentice .. Grand Master, verified in Spell.dbc (name + rank)
        };

        std::vector<ProfessionSpells> const professionSpells = {
            {SKILL_ALCHEMY, {2259, 3101, 3464, 11611, 28596, 51304}},
            {SKILL_BLACKSMITHING, {2018, 3100, 3538, 9785, 29844, 51300}},
            {SKILL_ENCHANTING, {7411, 7412, 7413, 13920, 28029, 51313}},
            {SKILL_ENGINEERING, {4036, 4037, 4038, 12656, 30350, 51306}},
            {SKILL_HERBALISM, {2366, 2368, 3570, 11993, 28695, 50300}},
            {SKILL_INSCRIPTION, {45357, 45358, 45359, 45360, 45361, 45363}},
            {SKILL_JEWELCRAFTING, {25229, 25230, 28894, 28895, 28897, 51311}},
            {SKILL_LEATHERWORKING, {2108, 3104, 3811, 10662, 32549, 51302}},
            {SKILL_MINING, {2575, 2576, 3564, 10248, 29354, 50310}},
            {SKILL_SKINNING, {8613, 8617, 8618, 10768, 32678, 50305}},
            {SKILL_TAILORING, {3908, 3909, 3910, 12180, 26790, 51309}},
        };

        // Slots InitEquipment() leaves empty below these levels (PlayerbotFactory::InitEquipment).
        bool SlotFilledAtLevel(uint8 slot, uint8 level)
        {
            if (slot == EQUIPMENT_SLOT_TRINKET1 || slot == EQUIPMENT_SLOT_TRINKET2)
                return level >= 50;
            if (slot == EQUIPMENT_SLOT_NECK || slot == EQUIPMENT_SLOT_HEAD)
                return level >= 30;
            if (slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_FINGER2)
                return level >= 20;
            return true;
        }

        // The relic slot (EQUIPMENT_SLOT_RANGED of paladins, druids, shamans and death knights): only counted once a
        // relic of the class's type exists at that level (green or better, RequiredLevel from item_template -
        // librams 10, idols and totems 52, sigils 74 in acore_world). Below it the slot is always empty and pulled
        // the average down (a level 60 death knight showed 58.4 for 62.0, wave 10 gear investigation).
        bool RelicSlotFilledAtLevel(uint8 cls, uint8 level)
        {
            uint32 subclass = 0;
            switch (cls)
            {
                case CLASS_PALADIN:
                    subclass = ITEM_SUBCLASS_ARMOR_LIBRAM;
                    break;
                case CLASS_DRUID:
                    subclass = ITEM_SUBCLASS_ARMOR_IDOL;
                    break;
                case CLASS_SHAMAN:
                    subclass = ITEM_SUBCLASS_ARMOR_TOTEM;
                    break;
                case CLASS_DEATH_KNIGHT:
                    subclass = ITEM_SUBCLASS_ARMOR_SIGIL;
                    break;
                default:
                    return true;  // a real ranged slot (bows, wands, thrown)
            }
            static std::map<uint32, uint8> const minLevel = []
            {
                std::map<uint32, uint8> result;
                for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                {
                    if (proto.Class != ITEM_CLASS_ARMOR || proto.Quality < ITEM_QUALITY_UNCOMMON ||
                        !proto.RequiredLevel)
                        continue;
                    auto const itr = result.find(proto.SubClass);
                    if (itr == result.end() || proto.RequiredLevel < itr->second)
                        result[proto.SubClass] = uint8(proto.RequiredLevel);
                }
                return result;
            }();
            auto const itr = minLevel.find(subclass);
            return itr != minLevel.end() && level >= itr->second;
        }

        // The loot of the content's bosses (its bossEntries, else the rank-3 and instance_encounters creatures spawned
        // on the map): every item listed in the boss loot itself, and the bind-on-pickup items of the reference
        // tables it uses (the bind-on-equip ones there are shared world drops a player has from elsewhere). The test
        // group never wears what the dungeon it is about to run drops (Scholomance showed its own Deathbone set).
        // World thread, once per content.
        std::unordered_set<uint32> const& ContentOwnLoot(ContentDef const& content)
        {
            static std::map<std::string, std::unordered_set<uint32>> cache;
            auto const cached = cache.find(content.key);
            if (cached != cache.end())
                return cached->second;

            std::unordered_set<uint32>& items = cache[content.key];
            std::set<uint32> bosses(content.bossEntries.begin(), content.bossEntries.end());
            if (bosses.empty())
            {
                if (QueryResult result = WorldDatabase.Query(
                        "SELECT DISTINCT c.id FROM creature c JOIN creature_template t ON t.entry = c.id "
                        "WHERE c.map = {} AND t.`rank` = 3 "
                        "UNION SELECT DISTINCT e.creditEntry FROM instance_encounters e "
                        "JOIN creature c ON c.id = e.creditEntry WHERE e.creditType = 0 AND c.map = {}",
                        content.mapId, content.mapId))
                {
                    do
                    {
                        bosses.insert(result->Fetch()[0].Get<uint32>());
                    } while (result->NextRow());
                }
            }

            std::set<uint32> pending;
            for (uint32 entry : bosses)
            {
                CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
                if (!creature || !creature->lootid)
                    continue;
                if (QueryResult result = WorldDatabase.Query(
                        "SELECT Item, Reference FROM creature_loot_template WHERE Entry = {}", creature->lootid))
                {
                    do
                    {
                        Field* fields = result->Fetch();
                        if (uint32 const reference = fields[1].Get<uint32>())
                            pending.insert(reference);
                        else
                            items.insert(fields[0].Get<uint32>());
                    } while (result->NextRow());
                }
            }

            std::set<uint32> visited;
            while (!pending.empty())
            {
                uint32 const reference = *pending.begin();
                pending.erase(pending.begin());
                if (!visited.insert(reference).second)
                    continue;
                if (QueryResult result = WorldDatabase.Query(
                        "SELECT Item, Reference FROM reference_loot_template WHERE Entry = {}", reference))
                {
                    do
                    {
                        Field* fields = result->Fetch();
                        if (uint32 const next = fields[1].Get<uint32>())
                        {
                            pending.insert(next);
                            continue;
                        }
                        uint32 const itemId = fields[0].Get<uint32>();
                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                        if (proto && proto->Bonding == BIND_WHEN_PICKED_UP)
                            items.insert(itemId);
                    } while (result->NextRow());
                }
            }

            LOG_INFO("playerbots", "Test harness: {} - {} boss creatures, {} own loot items kept out of the gear",
                     content.key, bosses.size(), items.size());
            return items;
        }

        std::vector<uint32> const dragonsEyeItems = {36766, 36767, 42142, 42143, 42144, 42145, 42146, 42148, 42149,
                                                     42150, 42151, 42152, 42153, 42154, 42155, 42156, 42157, 42158};

        uint32 GemEnchant(uint32 gemItem)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(gemItem);
            if (!proto)
                return 0;
            GemPropertiesEntry const* gem = sGemPropertiesStore.LookupEntry(proto->GemProperties);
            return gem ? gem->spellitemenchantement : 0;
        }

        void SetEnchant(Player* bot, Item* item, EnchantmentSlot slot, uint32 enchantId)
        {
            if (!item || !enchantId)
                return;
            bot->ApplyEnchantment(item, slot, false);
            item->SetEnchantment(slot, enchantId, 0, 0, bot->GetGUID());
            bot->ApplyEnchantment(item, slot, true);
        }

        void ClearBagContents(Player* bot)
        {
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            {
                Bag* bag = bot->GetBagByPos(bagSlot);
                if (!bag)
                    continue;
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (bag->GetItemByPos(slot))
                        bot->DestroyItem(bagSlot, slot, true);
            }
        }

        void Give(Player* bot, uint32 itemId, uint32 count, PrepareResult& result)
        {
            if (!itemId || !count || !sObjectMgr->GetItemTemplate(itemId))
                return;

            uint32 const have = bot->GetItemCount(itemId);
            if (have >= count)
                return;

            if (!bot->StoreNewItemInBestSlots(itemId, count - have))
                result.perks.push_back("bags full: item " + std::to_string(itemId));
        }

        // The skill a levelling player has at that level (ProfessionSkillFor) and the trainer ranks up to it; the
        // value never goes down for a character that already had more.
        void ApplyProfessions(Player* bot, std::pair<uint16, uint16> professions, PrepareResult& result)
        {
            uint16 const wanted = ProfessionSkillFor(bot->GetLevel());
            for (ProfessionSpells const& profession : professionSpells)
            {
                if (profession.skill == professions.first || profession.skill == professions.second)
                    continue;
                if (bot->HasSkill(profession.skill))
                    bot->SetSkill(profession.skill, 0, 0, 0);
            }

            for (ProfessionSpells const& profession : professionSpells)
            {
                if (profession.skill != professions.first && profession.skill != professions.second)
                    continue;

                uint16 const value = std::max<uint16>(wanted, bot->GetSkillValue(profession.skill));
                uint8 const step =
                    uint8(std::clamp<uint32>((value + SKILL_PER_STEP - 1) / SKILL_PER_STEP, 1, PROFESSION_STEPS));
                for (uint8 rank = 0; rank < step; ++rank)
                    if (!bot->HasSpell(profession.ranks[rank]))
                        bot->learnSpell(profession.ranks[rank], false);
                bot->SetSkill(profession.skill, step, value, uint16(step * SKILL_PER_STEP));
                result.professionSkill = std::max(result.professionSkill, value);
            }

            bot->SetFreePrimaryProfessions(0);

            auto learnPassive = [bot, &result](uint16 skill, uint32 spell, char const* name)
            {
                if (bot->GetSkillValue(skill) < PROFESSION_SKILL)
                    return;
                if (!bot->HasSpell(spell))
                    bot->learnSpell(spell, false);
                result.perks.push_back(name);
            };

            learnPassive(SKILL_ALCHEMY, SPELL_MIXOLOGY, "Mixology");
            learnPassive(SKILL_MINING, SPELL_TOUGHNESS_RANK_6, "Toughness");
            learnPassive(SKILL_SKINNING, SPELL_MASTER_OF_ANATOMY_RANK_6, "Master of Anatomy");
            learnPassive(SKILL_HERBALISM, SPELL_LIFEBLOOD_RANK_6, "Lifeblood");
        }

        bool HasProfession(Player* bot, uint16 skill) { return bot->GetSkillValue(skill) >= PROFESSION_SKILL; }

        void ApplySockets(Player* bot, PrepareResult& result)
        {
            if (!HasProfession(bot, SKILL_BLACKSMITHING))
                return;

            Item* wrists = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_WRISTS);
            if (wrists && !wrists->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT))
            {
                SetEnchant(bot, wrists, PRISMATIC_ENCHANTMENT_SLOT, ENCHANT_SOCKET_BRACER);
                result.perks.push_back("Socket Bracer");
            }

            Item* hands = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_HANDS);
            if (hands && !hands->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT))
            {
                SetEnchant(bot, hands, PRISMATIC_ENCHANTMENT_SLOT, ENCHANT_SOCKET_GLOVES);
                result.perks.push_back("Socket Gloves");
            }
        }

        // Profession enchants the stat calculator does not pick (use / proc effects) or might skip.
        void ApplyPerkEnchants(Player* bot, SpecDef const& spec, PrepareResult& result)
        {
            bool const caster = spec.role == Role::Caster;
            bool const healer = spec.role == Role::Healer;
            bool const tank = spec.role == Role::Tank;

            auto forcePerm = [bot, &result](uint8 slot, std::set<uint32> const& accepted, uint32 wanted,
                                            char const* name)
            {
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!item)
                    return;
                if (!accepted.count(item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)))
                    SetEnchant(bot, item, PERM_ENCHANTMENT_SLOT, wanted);
                result.perks.push_back(name);
            };

            if (HasProfession(bot, SKILL_ENGINEERING))
                forcePerm(EQUIPMENT_SLOT_HANDS, {ENCHANT_HYPERSPEED_ACCELERATORS}, ENCHANT_HYPERSPEED_ACCELERATORS,
                          "Hyperspeed Accelerators");

            if (HasProfession(bot, SKILL_TAILORING) && !tank)
            {
                uint32 const embroidery = caster ? ENCHANT_LIGHTWEAVE : healer ? ENCHANT_DARKGLOW : ENCHANT_SWORDGUARD;
                forcePerm(EQUIPMENT_SLOT_BACK, {embroidery}, embroidery,
                          caster ? "Lightweave Embroidery" : healer ? "Darkglow Embroidery" : "Swordguard Embroidery");
            }

            if (HasProfession(bot, SKILL_ENCHANTING))
            {
                uint32 const ring = tank ? ENCHANT_RING_STAMINA
                                         : (caster || healer) ? ENCHANT_RING_SPELLPOWER : ENCHANT_RING_ASSAULT;
                std::set<uint32> const rings = {ENCHANT_RING_ASSAULT, ENCHANT_RING_SPELLPOWER, ENCHANT_RING_STAMINA};
                forcePerm(EQUIPMENT_SLOT_FINGER1, rings, ring, "ring enchant 1");
                forcePerm(EQUIPMENT_SLOT_FINGER2, rings, ring, "ring enchant 2");
            }

            if (HasProfession(bot, SKILL_LEATHERWORKING))
            {
                uint32 const fur = tank ? ENCHANT_FUR_STAMINA
                                        : (caster || healer) ? ENCHANT_FUR_SPELL_POWER : ENCHANT_FUR_ATTACK_POWER;
                forcePerm(EQUIPMENT_SLOT_WRISTS,
                          {ENCHANT_FUR_ATTACK_POWER, ENCHANT_FUR_STAMINA, ENCHANT_FUR_SPELL_POWER}, fur, "Fur Lining");
            }

            if (HasProfession(bot, SKILL_INSCRIPTION))
            {
                uint32 const inscription = tank     ? ENCHANT_MASTERS_PINNACLE
                                           : caster ? ENCHANT_MASTERS_STORM
                                           : healer ? ENCHANT_MASTERS_CRAG
                                                    : ENCHANT_MASTERS_AXE;
                forcePerm(EQUIPMENT_SLOT_SHOULDERS,
                          {ENCHANT_MASTERS_AXE, ENCHANT_MASTERS_CRAG, ENCHANT_MASTERS_PINNACLE, ENCHANT_MASTERS_STORM},
                          inscription, "Master's Inscription");
            }
        }

        // ApplyEnchantAndGemsNew() already honours the Jewelcrafting requirement of Dragon's Eye; this tops the
        // count up to three when its colour scoring kept a cheaper gem.
        uint32 ApplyDragonsEyes(Player* bot, PrepareResult& result)
        {
            std::set<uint32> eyeEnchants;
            for (uint32 itemId : dragonsEyeItems)
                if (uint32 enchant = GemEnchant(itemId))
                    eyeEnchants.insert(enchant);

            struct SocketRef
            {
                Item* item;
                EnchantmentSlot slot;
                float score;
            };

            StatsWeightCalculator calculator(bot);
            std::vector<SocketRef> sockets;
            uint32 count = 0;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!item || !item->GetTemplate())
                    continue;

                uint8 firstPrismatic = 0;
                while (firstPrismatic < MAX_GEM_SOCKETS && item->GetTemplate()->Socket[firstPrismatic].Color)
                    ++firstPrismatic;
                bool const hasPrismatic = item->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT) != 0;

                for (uint8 index = 0; index < MAX_GEM_SOCKETS; ++index)
                {
                    uint32 const color = item->GetTemplate()->Socket[index].Color;
                    bool const prismatic = !color && hasPrismatic && index == firstPrismatic;
                    if ((!color && !prismatic) || color == SOCKET_COLOR_META)
                        continue;

                    EnchantmentSlot const enchantSlot = EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + index);
                    uint32 const current = item->GetEnchantmentId(enchantSlot);
                    if (eyeEnchants.count(current))
                    {
                        ++count;
                        continue;
                    }
                    sockets.push_back({item, enchantSlot, current ? calculator.CalculateEnchant(current) : 0.0f});
                }
            }

            if (!HasProfession(bot, SKILL_JEWELCRAFTING) || count >= MAX_DRAGONS_EYES || sockets.empty())
                return count;

            uint32 bestEye = 0;
            float bestScore = 0.0f;
            for (uint32 enchant : eyeEnchants)
            {
                float const score = calculator.CalculateEnchant(enchant);
                if (score > bestScore)
                {
                    bestScore = score;
                    bestEye = enchant;
                }
            }
            if (!bestEye)
                return count;

            std::sort(sockets.begin(), sockets.end(),
                      [](SocketRef const& left, SocketRef const& right) { return left.score < right.score; });
            for (SocketRef const& socket : sockets)
            {
                if (count >= MAX_DRAGONS_EYES || socket.score >= bestScore)
                    break;
                SetEnchant(bot, socket.item, socket.slot, bestEye);
                ++count;
            }

            result.perks.push_back("Dragon's Eye x" + std::to_string(count));
            return count;
        }

        uint32 GlyphOfItem(ItemTemplate const* proto)
        {
            for (uint32 index = 0; index < MAX_ITEM_PROTO_SPELLS; ++index)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(proto->Spells[index].SpellId));
                if (!info)
                    continue;
                for (uint8 effect = EFFECT_0; effect <= EFFECT_2; ++effect)
                    if (info->Effects[effect].Effect == SPELL_EFFECT_APPLY_GLYPH)
                        return uint32(info->Effects[effect].MiscValue);
            }
            return 0;
        }

        // The premade glyphs of the spec (Playerbots.PremadeSpecGlyph.<class>.<specNo>); the factory only glyphs
        // random bots, test characters get the same set here.
        uint32 ApplyPremadeGlyphs(Player* bot, int32 specNo)
        {
            uint8 const cls = bot->getClass();
            if (specNo < 0 || cls >= MAX_CLASSES)
                return 0;

            static uint8 const glyphOrder[MAX_GLYPH_SLOT_INDEX] = {0, 1, 3, 2, 4, 5};
            std::vector<uint32> const& items = sPlayerbotAIConfig.ParsedSpecGlyph[cls][specNo];
            uint32 applied = 0;
            for (uint32 index = 0; index < MAX_GLYPH_SLOT_INDEX && index < items.size(); ++index)
            {
                ItemTemplate const* proto = items[index] ? sObjectMgr->GetItemTemplate(items[index]) : nullptr;
                if (!proto || proto->Class != ITEM_CLASS_GLYPH || !(proto->AllowableClass & bot->getClassMask()) ||
                    proto->RequiredLevel > bot->GetLevel())
                    continue;

                uint32 const glyph = GlyphOfItem(proto);
                GlyphPropertiesEntry const* glyphEntry = glyph ? sGlyphPropertiesStore.LookupEntry(glyph) : nullptr;
                if (!glyphEntry)
                    continue;

                // Glyph slots open at 15 / 30 / 50 / 70 / 80 (Player::InitGlyphsForLevel).
                uint8 const slot = glyphOrder[index];
                if (!(bot->GetUInt32Value(PLAYER_GLYPHS_ENABLED) & (1u << slot)))
                    continue;
                if (GlyphPropertiesEntry const* old = sGlyphPropertiesStore.LookupEntry(bot->GetGlyph(slot)))
                    bot->RemoveAurasDueToSpell(old->SpellId);

                bot->CastSpell(bot, glyphEntry->SpellId,
                               TriggerCastFlags(TRIGGERED_FULL_MASK &
                                                ~(TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE)));
                bot->SetGlyph(slot, glyph, true);
                ++applied;
            }
            bot->SendTalentsInfoData(false);
            return applied;
        }

        // [rot110] A hunter's pet food: the vendor food of its diet that feeds it fully at its level (the core's
        // Pet::HaveInDiet / GetCurrentFoodBenefitLevel), lowest item level then lowest entry. b211 rfc-h: the harness
        // gave fish and Dalaran Clam Chowder only, so "feed pet" found nothing for a meat eater - Rthunac's Scorpid
        // ended the run Content (442k happiness, x1.00 damage), Spider / Bat / Ravager pets Unhappy (x0.75); wowsims'
        // pet is Happy (x1.25, sim/hunter/pet.go).
        uint32 PetFoodFor(Pet const* pet, uint8 level)
        {
            uint32 best = 0;
            uint32 bestLevel = 0;
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
            {
                if (proto.Class != ITEM_CLASS_CONSUMABLE || !proto.FoodType || proto.RequiredLevel > level ||
                    proto.Bonding != NO_BIND || !proto.BuyPrice || proto.HasFlag(ITEM_FLAG_CONJURED) ||
                    !pet->HaveInDiet(&proto))
                    continue;
                if (pet->GetCurrentFoodBenefitLevel(proto.ItemLevel) < PET_FOOD_FULL_BENEFIT)
                    continue;
                if (!best || proto.ItemLevel < bestLevel || (proto.ItemLevel == bestLevel && entry < best))
                {
                    best = entry;
                    bestLevel = proto.ItemLevel;
                }
            }
            return best;
        }

        void GiveConsumables(Player* bot, SpecDef const& spec, bool groupHasWarlock, PrepareResult& result)
        {
            uint8 const level = bot->GetLevel();
            RoleConsumables const role = ConsumablesFor(spec, level);
            LevelSupplies const supplies = SuppliesFor(level);
            Give(bot, role.flask, FLASK_COUNT, result);
            Give(bot, role.food, FISH_COUNT, result);
            if (role.prePot != supplies.manaPotion)
                Give(bot, role.prePot, PREPOT_COUNT, result);
            Give(bot, supplies.healingPotion, HEALING_POTION_COUNT, result);
            Give(bot, supplies.regenFood, REGEN_FOOD_COUNT, result);
            if (spec.cls == CLASS_HUNTER)
            {
                if (Pet* pet = bot->GetPet(); pet && pet->getPetType() == HUNTER_PET)
                {
                    if (uint32 const food = PetFoodFor(pet, level))
                        Give(bot, food, PET_FOOD_COUNT, result);
                    else
                        result.perks.push_back("no pet food for " + pet->GetName());
                }
            }

            if (UsesMana(spec.role, spec.cls))
            {
                // The pre-pot of a healer (and of a caster below 60) is its mana potion: one stack for both uses.
                uint32 const potions =
                    role.prePot == supplies.manaPotion ? MANA_POTION_COUNT + PREPOT_COUNT : MANA_POTION_COUNT;
                Give(bot, supplies.manaPotion, potions, result);
                Give(bot, supplies.drink, DRINK_COUNT, result);
            }
            else if (spec.cls == CLASS_DRUID)
            {
                // [b33_pos] a feral druid (bear tank or cat) is not a "mana user" for the harness's regen waits, but
                // shifts out to drink for its heals, Rebirth and buffs (scholo-n 5H: bear tank had no water at all)
                Give(bot, supplies.drink, DRINK_COUNT, result);
            }

            // A warlock of the group would hand them out; one stone each, it is unique (Fel Healthstone: level 69).
            if (groupHasWarlock && level >= 69)
                Give(bot, ITEM_FEL_HEALTHSTONE, 1, result);

            // Class reagents for a whole raid night (group buffs, battle res, soulstones, Reincarnation) at 80;
            // below it PlayerbotFactory::InitReagents() gives the reagents of the level.
            if (level < MAX_TEST_LEVEL)
            {
                if (spec.cls == CLASS_WARLOCK)
                {
                    Give(bot, ITEM_SOUL_SHARD, 20, result);
                    if (level >= INFERNO_MIN_LEVEL)  // [b35_lock] instead of farming the stones
                        Give(bot, ITEM_INFERNAL_STONE, 5, result);
                }
                return;
            }

            switch (spec.cls)
            {
                case CLASS_PALADIN:
                    Give(bot, ITEM_SYMBOL_OF_KINGS, 200, result);
                    break;
                case CLASS_PRIEST:
                    Give(bot, ITEM_DEVOUT_CANDLE, 100, result);
                    break;
                case CLASS_DRUID:
                    Give(bot, ITEM_WILD_SPINELEAF, 60, result);
                    Give(bot, ITEM_STARLEAF_SEED, 20, result);
                    break;
                case CLASS_MAGE:
                    Give(bot, ITEM_ARCANE_POWDER, 100, result);
                    Give(bot, 33312 /* Mana Sapphire: conjured before the run, never inside (user 2026-10-25) */, 1, result);
                    break;
                case CLASS_DEATH_KNIGHT:
                    Give(bot, ITEM_CORPSE_DUST, 40, result);
                    break;
                case CLASS_WARLOCK:
                    Give(bot, ITEM_SOUL_SHARD, 20, result);
                    Give(bot, ITEM_INFERNAL_STONE, 5, result);  // [b35_lock] Inferno ("inferno boss" trigger)
                    break;
                case CLASS_SHAMAN:
                    Give(bot, ITEM_ANKH, 10, result);
                    break;
                default:
                    break;
            }
        }

        // Gear top-up (TopUpGear): the average may end at most this much above the target (run_start warns from
        // target + 0.5).
        constexpr float TOP_UP_OVERSHOOT = 0.5f;

        struct GearPiece
        {
            uint32 entry = 0;
            int32 randomPropertyId = 0;
        };

        using GearSet = std::array<GearPiece, EQUIPMENT_SLOT_END>;

        GearSet SnapshotGear(Player* bot)
        {
            GearSet set{};
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    set[slot] = {item->GetEntry(), item->GetItemRandomPropertyId()};
            return set;
        }

        void UnequipSlot(Player* bot, uint8 slot)
        {
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        }

        // As the factory equips (PlayerbotFactory::AutoGear): the item, then its random suffix with the mods redone.
        bool EquipPiece(Player* bot, uint8 slot, GearPiece const& piece)
        {
            uint16 dest = 0;
            if (!piece.entry || bot->CanEquipNewItem(slot, dest, piece.entry, false) != EQUIP_ERR_OK)
                return false;
            Item* item = bot->EquipNewItem(dest, piece.entry, true);
            if (item && piece.randomPropertyId)
            {
                bot->_ApplyItemMods(item, item->GetSlot(), false);
                item->SetItemRandomProperties(piece.randomPropertyId);
                bot->_ApplyItemMods(item, item->GetSlot(), true);
            }
            return item != nullptr;
        }

        uint32 PieceLevel(GearPiece const& piece)
        {
            ItemTemplate const* proto = piece.entry ? sObjectMgr->GetItemTemplate(piece.entry) : nullptr;
            return proto ? proto->ItemLevel : 0;
        }

        // Epic item levels at 80 come in tiers (226, 232/233, 239, 245/246, 251/252, 258: acore_world has no epic of
        // required level 71-80 at 234-238 or 247-250), so the average is a step of the cap and the targets 235 / 248
        // sit in a gap: a spec with the next tier in every slot jumped from ~231.5 (cap 235) to ~238.5 (cap 239) and
        // the doubled miss kept the low set (Gruul 25A 2026-10-24: 17 of 25 at cap 235, 3-4 below; hard: Balance,
        // Arcane, priests, Holy paladin). The worn set (the best pass) takes items of the set of highCap unit by unit
        // (main and off hand together, so a two-hander never meets a one-hand pair), the biggest item level gain
        // first, while the average stays at most TOP_UP_OVERSHOOT above the target; a unit that cannot be worn
        // (unique) keeps its low items. Returns the factory passes it made.
        uint32 TopUpGear(Player* bot, uint32 quality, uint32 highCap, uint32 targetIlvl,
                         std::unordered_set<uint32> const* ownLoot, bool targetedPicker)
        {
            GearSet const low = SnapshotGear(bot);
            PlayerbotFactory::DestroyEquippedGear(bot);
            PlayerbotFactory::AutoGear(bot, quality, highCap, false, false, false, targetedPicker, ownLoot);
            GearSet const high = SnapshotGear(bot);
            PlayerbotFactory::DestroyEquippedGear(bot);
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                EquipPiece(bot, slot, low[slot]);

            std::vector<std::pair<int32, std::vector<uint8>>> units;  // item level gain, its slots
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                if (slot == EQUIPMENT_SLOT_OFFHAND || slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
                    continue;
                std::vector<uint8> slots = {slot};
                if (slot == EQUIPMENT_SLOT_MAINHAND)
                    slots.push_back(EQUIPMENT_SLOT_OFFHAND);
                int32 gain = 0;
                for (uint8 part : slots)
                    gain += int32(PieceLevel(high[part])) - int32(PieceLevel(low[part]));
                if (gain > 0)
                    units.emplace_back(gain, std::move(slots));
            }
            std::stable_sort(units.begin(), units.end(),
                             [](auto const& left, auto const& right) { return left.first > right.first; });

            float const ceiling = float(targetIlvl) + TOP_UP_OVERSHOOT;
            for (auto const& [gain, slots] : units)
            {
                if (AverageItemLevel(bot) >= float(targetIlvl))
                    break;
                for (uint8 part : slots)
                    UnequipSlot(bot, part);
                bool worn = true;
                for (uint8 part : slots)
                    if (high[part].entry && !EquipPiece(bot, part, high[part]))
                        worn = false;
                if (worn && AverageItemLevel(bot) <= ceiling)
                    continue;
                for (uint8 part : slots)
                    UnequipSlot(bot, part);
                for (uint8 part : slots)
                    EquipPiece(bot, part, low[part]);
            }
            return 2;
        }
    }

    int32 FindPremadeSpec(uint8 cls, std::string const& name)
    {
        if (cls >= MAX_CLASSES)
            return -1;
        for (int32 specNo = 0; specNo < MAX_SPECNO; ++specNo)
            if (sPlayerbotAIConfig.PremadeSpecName[cls][specNo] == name)
                return specNo;
        return -1;
    }

    float AverageItemLevel(Player* bot)
    {
        uint32 total = 0;
        uint32 slots = 0;
        uint8 const level = bot->GetLevel();
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD || !SlotFilledAtLevel(slot, level))
                continue;
            if (slot == EQUIPMENT_SLOT_RANGED && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) &&
                !RelicSlotFilledAtLevel(bot->getClass(), level))
                continue;

            ++slots;

            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item && item->GetTemplate())
            {
                total += item->GetTemplate()->ItemLevel;
                continue;
            }

            if (slot == EQUIPMENT_SLOT_OFFHAND)
            {
                Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                if (mainHand && mainHand->GetTemplate() &&
                    mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON && !bot->CanTitanGrip())
                    total += mainHand->GetTemplate()->ItemLevel;
            }
        }

        return slots ? float(total) / float(slots) : 0.0f;
    }

    // [b31_harness] Player::InitStatsForLevel (and GiveLevel, which calls it) zeroes UNIT_FIELD_AURASTATE while
    // every aura stays on the character, so the states the auras grant are gone: a seal saved by the previous run
    // (back at login) keeps its aura without AURA_STATE_JUDGEMENT, every Judgement then fails CheckCast
    // (CASTER_AURASTATE, Spell.dbc CasterAuraState 5 on 20271 / 53407 / 53408) and the "seal" trigger never fires
    // because the seal is there (build 30: Ret Judgement 0 in 8 of 9 boss attempts; the paladin AI's "drop stale
    // seal" [rot_b31_pdd] worked around it in combat). Seals are dropped here, the AI casts a fresh one (a fresh
    // application sets the state), and the state of every other aura is set again as Unit::_ApplyAura sets it.
    static uint32 ResetAuraStatesAfterStatInit(Player* bot)
    {
        uint32 seals = 0;
        bot->RemoveAppliedAuras([&seals](AuraApplication const* aurApp)
        {
            bool const seal = aurApp->GetBase()->GetSpellInfo()->GetSpellSpecific() == SPELL_SPECIFIC_SEAL;
            seals += seal ? 1 : 0;
            return seal;
        });

        for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
        {
            if (aurApp->GetBase()->IsRemoved())
                continue;
            if (AuraStateType const state = aurApp->GetBase()->GetSpellInfo()->GetAuraState())
                bot->ModifyAuraState(state, true);
        }

        return seals;
    }

    // [b31_dk_talents] Spent talent points of the active spec, counted as Player::ActivateSpec counts them.
    static uint32 SpentTalentPoints(Player* bot)
    {
        uint32 spent = 0;
        for (auto const& [spellId, talent] : bot->GetTalentMap())
        {
            if (talent->State == PLAYERSPELL_REMOVED || !(talent->specMask & bot->GetActiveSpecMask()))
                continue;
            if (TalentSpellPos const* pos = GetTalentSpellPos(spellId))
                spent += pos->rank + 1;
        }
        return spent;
    }

    // [b31_dk_talents] The talent points of the level as Player::CalculateTalentsPoints grants them anywhere but
    // Ebon Hold (a Death Knight there gets 55's points plus the starting-zone quest rewards only).
    static uint32 FullLevelTalentPoints(Player* bot)
    {
        uint32 const base = bot->GetLevel() < 10 ? 0 : bot->GetLevel() - 9;
        return uint32(float(base + bot->GetBonusTalentCount()) * sWorld->getRate(RATE_TALENT));
    }

    // [b31_dk_talents] A Death Knight of the prepared level has long left Ebon Hold: the starting-zone chain
    // (quest_template.RewardTalents, AllowableClasses = DK; the race variant of "A Special Surprise" by race) is marked
    // rewarded, so the next login on map 609 loads m_questRewardTalentCount = 46 (_LoadQuestStatusRewarded runs
    // before LoadFromDB's InitTalentForLevel) and keeps the full tree instead of resetting it to 25 points.
    // Kanrethad's Quest (id 1, 100 talents, no class) is no part of the chain: classless quests are skipped.
    static uint32 MarkDeathKnightChainRewarded(Player* bot)
    {
        constexpr uint32 DK_CLASS_MASK = 1 << (CLASS_DEATH_KNIGHT - 1);
        uint32 marked = 0;
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            if (!quest || !quest->GetBonusTalents() || quest->GetRequiredClasses() != DK_CLASS_MASK)
                continue;
            if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false) ||
                quest->GetMinLevel() > bot->GetLevel() || bot->IsQuestRewarded(questId))
                continue;
            // Not in the log any more (a fresh DK carries its first chain quests).
            uint16 const slot = bot->FindQuestSlot(questId);
            if (slot < MAX_QUEST_LOG_SIZE)
                bot->SetQuestSlot(slot, 0);
            if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE)
                bot->RemoveActiveQuest(questId, false);
            bot->SetRewardedQuest(questId);
            ++marked;
        }
        return marked;
    }

    PrepareResult PrepareCharacter(Player* bot, SpecDef const& spec, uint8 level, uint32 targetIlvl,
                                   std::pair<uint16, uint16> professions, bool groupHasWarlock,
                                   ContentDef const* content)
    {
        PrepareResult result;
        result.professions = professions;

        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!bot || !botAI || !bot->IsInWorld())
        {
            result.error = "not online as a bot";
            return result;
        }

        if (bot->getClass() != spec.cls)
        {
            result.error = "class mismatch";
            return result;
        }

        result.specNo = FindPremadeSpec(spec.cls, spec.premade);
        if (result.specNo < 0)
        {
            result.error = std::string("premade spec '") + spec.premade + "' not found in playerbots.conf";
            return result;
        }

        // Characters are only raised: one prepared for a higher level never runs lower content.
        if (bot->GetLevel() > level)
        {
            result.error = "level " + std::to_string(bot->GetLevel()) + " is above the content level " +
                           std::to_string(level);
            return result;
        }

        if (bot->isDead())
            bot->ResurrectPlayer(1.0f, false);
        // A run starts fresh: Resurrection Sickness from a death of an earlier run (saved with the character, up to
        // 10 minutes) is no part of it. Gruul 25A 2026-10-24: Rtpalaa and Rtdruaa logged in with it, the tank's own
        // pull gate (ChooseTargetActions, "[pull gate] waiting: resurrection sickness on Rtpalaa") held every pull of
        // its own - "group not ready" in front of the Gronn-Priests while a warlock's pet opened the fight.
        constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;
        bot->RemoveAurasDueToSpell(SPELL_RESURRECTION_SICKNESS);
        bot->CombatStop(true);
        // [event_boss_classic] Pocket money, as any player of the level carries: vendor starters buy with it (BRD
        // [brd_bar_buy]: 6 Dark Iron Ale Mugs from Plugger Spazzring for the Rocknot chain). brd-n 20261025-163739
        // brd-n-5A-21: moneyBefore 0, bought 0, the bar chain never started; brd-h had 112 g. Only raised to the floor.
        constexpr uint32 PREPARE_MONEY_FLOOR = 50 * GOLD;
        if (bot->GetMoney() < PREPARE_MONEY_FLOOR)
            bot->SetMoney(PREPARE_MONEY_FLOOR);

        // Levelling gear is green / blue (quest rewards, dungeon drops); epics only at the level cap.
        uint32 const quality = level >= MAX_TEST_LEVEL ? ITEM_QUALITY_EPIC : ITEM_QUALITY_RARE;
        PlayerbotFactory factory(bot, level, quality, 0);

        ClearBagContents(bot);
        if (bot->GetLevel() < level)
            bot->GiveLevel(level);
        result.level = level;
        bot->InitStatsForLevel(true);
        bot->SetUInt32Value(PLAYER_XP, 0);
        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
        factory.UnbindInstance();

        bot->LearnDefaultSkills();
        // Riding of the level (InitSkills: Apprentice 20, Journeyman 40, Expert 60, Artisan 70 by the
        // Playerbots.Use*MountAtMinLevel settings), Cold Weather Flying from 77 as the trainer sells it, and the mounts
        // of that riding (InitMounts). A group revived outside a floating portal (Tempest Keep) flies back with them.
        factory.InitSkills();
        if (level >= 77 && !bot->HasSpell(SPELL_COLD_WEATHER_FLYING))
            bot->learnSpell(SPELL_COLD_WEATHER_FLYING, false);
        factory.InitMounts();
        factory.InitClassSpells();
        factory.InitAvailableSpells();
        // [b31_dk_talents] Player::CalculateTalentsPoints gives a Death Knight on map 609 (Ebon Hold) only 55's points
        // plus m_questRewardTalentCount: fresh DK bots log in there, and the template step got 25 of 71 points at 80
        // (Rtdekam..Rtdekat 8-9 talent rows: no Heart Strike, no Dancing Rune Weapon; Rtdekau..Rtdekcd still level 1
        // there, the same on their first prepare). The chain is
        // marked rewarded (persists: the next login there loads the full count); m_questRewardTalentCount of this
        // session only grows by RewardQuest, so the missing points are lent as bonus talents for the template step
        // and taken back after it (the spent tree stays; nothing on the way recomputes it before the bot leaves
        // Ebon Hold: teleports never call InitTalentForLevel, only login / GiveLevel / ActivateSpec / RewardQuest do).
        // With reset = true a DK prepared earlier with a partial tree gets the whole template here.
        bool const deathKnight = bot->getClass() == CLASS_DEATH_KNIGHT;
        if (deathKnight)
            result.dkChainQuestsMarked = MarkDeathKnightChainRewarded(bot);
        uint32 const fullPoints = FullLevelTalentPoints(bot);
        uint32 const bonusBefore = bot->GetBonusTalentCount();
        uint32 lentPoints = 0;
        while (bot->CalculateTalentsPoints() < fullPoints && lentPoints < 100)
        {
            ++lentPoints;
            bot->SetBonusTalentCount(bonusBefore + lentPoints);
        }
        // Player::resetTalents returns before SetFreeTalentPoints when no talent is spent (the 0-talent DKs), so the
        // free-points field is refreshed here; spent <= points, so this never resets anything.
        if (lentPoints)
            bot->InitTalentForLevel();
        PlayerbotFactory::InitTalentsBySpecNo(bot, result.specNo, true);
        if (lentPoints)
            bot->SetBonusTalentCount(bonusBefore);
        result.talentsSpent = SpentTalentPoints(bot);
        result.talentsExpected = fullPoints;
        result.talentsCore = bot->CalculateTalentsPoints();
        result.talentsFree = bot->GetFreeTalentPoints();
        result.talentsMap = bot->GetMapId();
        if (deathKnight)
            LOG_INFO("playerbots", "[b31_dk_talents] {}: talent points {} (map {}) - expected {}, core {}, lent {}, "
                     "chain quests marked {}", bot->GetName(), result.talentsSpent, result.talentsMap, fullPoints,
                     result.talentsCore, lentPoints, result.dkChainQuestsMarked);
        LOG_INFO("playerbots", "[b31_talents] {} {} spec {} level {}: spent {} of expected {} (free {}, map {})",
                 bot->GetName(), ClassName(bot->getClass()), result.specNo, uint32(bot->GetLevel()),
                 result.talentsSpent, fullPoints, result.talentsFree, result.talentsMap);
        factory.InitAvailableSpells();
        factory.InitSpecialSpells();

        ApplyProfessions(bot, professions, result);
        factory.InitBags(true);

        // The factory limits items by a mixed gear score; the realized average decides the next limit.
        uint32 cap = targetIlvl;
        uint32 bestCap = cap;
        float bestDelta = 1000.0f;
        uint32 overCap = 0;  // the lowest cap whose pass landed above the target: the tier the top-up takes from
        // The ilvl-targeted picker (PlayerbotFactory::AutoGear ilvlTargetedMode, gear agent 2026-10-24): no random
        // skips, so the same cap gives the same set and the final pass on bestCap repeats the best pass instead of
        // drawing a new random one (the old re-roll could end below the best pass, wave 10 gear investigation).
        constexpr bool ILVL_TARGETED_PICKER = true;
        std::unordered_set<uint32> const* ownLoot = content ? &ContentOwnLoot(*content) : nullptr;
        for (uint32 pass = 0; pass < MAX_GEAR_PASSES; ++pass)
        {
            PlayerbotFactory::DestroyEquippedGear(bot);
            PlayerbotFactory::AutoGear(bot, quality, cap, false, false, false, ILVL_TARGETED_PICKER, ownLoot);
            ++result.gearPasses;

            float const avg = AverageItemLevel(bot);
            float const delta = avg - float(targetIlvl);
            if (delta > 0.0f && (!overCap || cap < overCap))
                overCap = cap;
            if (GearMiss(delta) < GearMiss(bestDelta))
            {
                bestDelta = delta;
                bestCap = cap;
            }

            if (GearMiss(delta) <= GEAR_TOLERANCE)
                break;

            int32 const next = int32(cap) - int32(std::lround(delta));
            // A cap of 0 means "no limit" to the factory: keep it at 1 or more for low levels.
            int32 const lowest = std::max<int32>(1, int32(targetIlvl) - 40);
            cap = uint32(std::clamp<int32>(next, lowest, int32(targetIlvl) + 40));
        }

        if (bestCap != cap || GearMiss(AverageItemLevel(bot) - float(targetIlvl)) > GearMiss(bestDelta))
        {
            // The best pass again (deterministic picker: the same cap, the same set, random suffixes included).
            PlayerbotFactory::DestroyEquippedGear(bot);
            PlayerbotFactory::AutoGear(bot, quality, bestCap, false, false, false, ILVL_TARGETED_PICKER, ownLoot);
            ++result.gearPasses;
            cap = bestCap;
        }
        // Below the target with a cap above it that overshot (an item level tier gap, TopUpGear): the best set takes
        // the next tier's items until the average reaches the target.
        if (bestDelta < -GEAR_TOLERANCE && overCap > cap)
            result.gearPasses += TopUpGear(bot, quality, overCap, targetIlvl, ownLoot, ILVL_TARGETED_PICKER);
        result.ilvlCap = cap;

        // Gems and enchants as the factory does for its own bots (Playerbots.MinEnchantingBotLevel); the profession
        // perks need Grand Master, so they only show up at 80.
        if (level >= sPlayerbotAIConfig.MinEnchantingBotLevel)
        {
            ApplySockets(bot, result);
            factory.ApplyEnchantAndGemsNew();
            ApplyPerkEnchants(bot, spec, result);
            result.dragonsEyes = ApplyDragonsEyes(bot, result);
        }

        factory.InitAmmo();
        bot->InitGlyphsForLevel();
        if (uint32 glyphs = ApplyPremadeGlyphs(bot, result.specNo))
            result.perks.push_back("glyphs x" + std::to_string(glyphs));
        if (spec.cls == CLASS_HUNTER || spec.cls == CLASS_WARLOCK)
        {
            factory.InitPet();
            factory.InitPetTalents();
        }
        factory.InitReagents();
        factory.InitPotions();
        factory.InitConsumables();
        GiveConsumables(bot, spec, groupHasWarlock, result);

        // [b31_harness] After the last stat reset of the preparation (GiveLevel / InitStatsForLevel above; nothing
        // below resets UNIT_FIELD_AURASTATE): no seal without its AURA_STATE_JUDGEMENT.
        if (uint32 const seals = ResetAuraStatesAfterStatInit(bot))
            LOG_INFO("playerbots", "[b31_harness] {}: {} seal(s) removed after InitStatsForLevel", bot->GetName(),
                     seals);

        bot->DurabilityRepairAll(false, 1.0f, false);
        bot->SetFullHealth();
        if (bot->GetMaxPower(POWER_MANA))
            bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
        bot->SaveToDB(false, false);

        result.avgIlvl = AverageItemLevel(bot);
        result.coreAvgIlvl = bot->GetAverageItemLevel();
        result.gearScore = PlayerbotAI::GetMixedGearScore(bot, false, false);
        result.ok = true;
        return result;
    }
}
