/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgCraft.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AiObjectContext.h"
#include "Bag.h"
#include "BudgetValues.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GuildBankRun.h"
#include "GuildCoordinator.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgProfession.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "World.h"

namespace
{
    // the crafting skill lines, and Mining for smelting
    uint32 const CRAFT_SKILLS[] = {SKILL_BLACKSMITHING, SKILL_LEATHERWORKING, SKILL_ALCHEMY,     SKILL_TAILORING,
                                   SKILL_ENGINEERING,   SKILL_ENCHANTING,    SKILL_JEWELCRAFTING, SKILL_INSCRIPTION,
                                   SKILL_COOKING,       SKILL_FIRST_AID,     SKILL_MINING};

    struct CraftRecipe
    {
        uint32 spellId{0};
        uint32 skill{0};
        uint32 yellow{0};  // SkillLineAbility TrivialSkillLineRankLow
        uint32 gray{0};    // TrivialSkillLineRankHigh
        uint32 product{0};
        uint32 focus{0};   // SpellFocusObject (forge, anvil, cooking fire)
        std::array<uint32, 2> totem{};
        std::array<uint32, 2> totemCategory{};
        std::vector<std::pair<uint32, uint32>> reagents;
    };

    struct Spawn
    {
        uint32 entry{0};
        uint16 mapId{0};
        float x{0}, y{0}, z{0};
        uint32 zoneId{0};
    };

    struct Index
    {
        std::unordered_map<uint32, CraftRecipe> recipes;            // spell -> recipe
        std::unordered_map<uint32, std::vector<uint32>> bySkill;    // skill line -> recipe spells
        std::unordered_map<uint32, std::vector<Spawn>> vendors;     // reagent -> vendor spawns (gold, unlimited)
        std::unordered_map<uint32, std::vector<Spawn>> foci;        // focus id -> gameobject spawns
    };

    Index index;
    bool loaded = false;

    // Player::UpdateCraftSkill -> CraftSkillGainChance (per mille): orange up to the yellow rank, then linear to gray
    int32 Chance(uint32 value, uint32 gray, uint32 yellow)
    {
        int32 const orange = int32(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE) * 10);
        int32 const grey = int32(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREY) * 10);
        if (gray <= yellow)
            return value < gray ? orange : grey;
        if (value <= yellow)
            return orange;
        if (value >= gray)
            return grey;
        return grey + int32(int64(gray - value) * (orange - grey) / (gray - yellow));
    }

    // 2 orange, 1 yellow, 0 green / gray (not crafted for skill)
    uint8 Tier(CraftRecipe const& r, uint32 value)
    {
        if (!r.gray || value >= (r.yellow + r.gray) / 2)
            return 0;
        return value <= r.yellow ? 2 : 1;
    }

    // the crafts this recipe takes from `value` until it turns green (or the rank cap)
    uint32 ExpectedCrafts(CraftRecipe const& r, uint32 value, uint32 cap)
    {
        uint32 const end = std::min(cap, (r.yellow + r.gray) / 2);
        uint32 const gain = std::max<uint32>(1, sWorld->getIntConfig(CONFIG_SKILL_GAIN_CRAFTING));
        double crafts = 0;
        for (uint32 v = value; v < end; v += gain)
            crafts += 1000.0 / std::max(1, Chance(v, r.gray, r.yellow));
        return std::max<uint32>(1, uint32(std::ceil(crafts - 0.001)));
    }

    uint32 UnitPrice(uint32 item)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        if (!proto)
            return 1;
        if (index.vendors.count(item))
            return std::max<uint32>(1, proto->BuyPrice / std::max<uint32>(1, proto->BuyCount));
        return std::max<uint32>(1, proto->SellPrice);
    }

    bool ToolsOk(Player* bot, CraftRecipe const& r)
    {
        for (uint32 t : r.totem)
            if (t && !bot->HasItemCount(t, 1))
                return false;
        for (uint32 c : r.totemCategory)
            if (c && !bot->HasItemTotemCategory(c))
                return false;
        return true;
    }

    bool Friendly(Player* bot, uint32 entry)
    {
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        FactionTemplateEntry const* faction = tmpl ? sFactionTemplateStore.LookupEntry(tmpl->faction) : nullptr;
        return own && faction && !faction->IsHostileTo(*own) && !own->IsHostileTo(*faction);
    }

    // 2: a consumable, a bag or a wanted item (reagent, upgrade); 1: a trade good; 0: else
    uint8 Useful(uint32 product)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(product);
        if (!proto)
            return 0;
        if (proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_CONTAINER ||
            sGuildCoordinator.IsWanted(product))
            return 2;
        return proto->Class == ITEM_CLASS_TRADE_GOODS ? 1 : 0;
    }

    Spawn const* ZoneVendor(Player* bot, uint32 item)
    {
        auto const spawns = index.vendors.find(item);
        if (spawns == index.vendors.end())
            return nullptr;
        Spawn const* best = nullptr;
        float bestDist = FLT_MAX;
        for (Spawn const& s : spawns->second)
        {
            if (s.mapId != bot->GetMapId() || s.zoneId != bot->GetZoneId() || !Friendly(bot, s.entry))
                continue;
            float const d = bot->GetExactDist(s.x, s.y, s.z);
            if (d < bestDist)
            {
                bestDist = d;
                best = &s;
            }
        }
        return best;
    }

    // the nearest spawn of the focus within the bot's sight (the town it stands in)
    Spawn const* FocusInSight(Player* bot, uint32 focusId)
    {
        auto const spawns = index.foci.find(focusId);
        if (spawns == index.foci.end())
            return nullptr;
        Spawn const* best = nullptr;
        float bestDist = bot->GetVisibilityRange();
        for (Spawn const& s : spawns->second)
        {
            if (s.mapId != bot->GetMapId())
                continue;
            float const d = bot->GetExactDist(s.x, s.y, s.z);
            if (d < bestDist)
            {
                bestDist = d;
                best = &s;
            }
        }
        return best;
    }

    // what the bot's craft state looks like: the same state gives the same answer, so a "nothing to craft" is not
    // looked at again until a skill, the money or the bags change
    uint64 Fingerprint(Player* bot)
    {
        uint64 skills = 0;
        for (uint32 skill : CRAFT_SKILLS)
            if (bot->HasSkill(skill))
                skills = skills * 31 + bot->GetPureSkillValue(skill) * 7 + bot->GetPureMaxSkillValue(skill);
        uint64 items = 0;
        auto const add = [&](Item* item)
        {
            if (item)
                items = items * 131 + item->GetEntry() * 17 + item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = bot->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    add(bag->GetItemByPos(uint8(j)));
        return skills ^ (items * 1000003) ^ (uint64(bot->GetMoney()) << 20) ^ bot->GetZoneId();
    }

    std::mutex lookLock;
    std::unordered_map<uint32, uint64> lastEmptyLook;                          // bot -> fingerprint of "nothing"
    std::unordered_map<uint32, std::pair<uint64, std::set<uint32>>> refused;  // bot -> (fingerprint, failed casts)

    bool RefusedBefore(Player* bot, uint32 spellId, uint64 fp)
    {
        std::lock_guard<std::mutex> guard(lookLock);
        auto const itr = refused.find(bot->GetGUID().GetCounter());
        return itr != refused.end() && itr->second.first == fp && itr->second.second.count(spellId);
    }

    struct Option
    {
        CraftRecipe const* recipe{nullptr};
        uint8 useful{0};
        uint8 tier{0};
        double perPoint{0};  // copper per expected skill point
        uint32 crafts{0};
        uint32 value{0};
        uint32 cap{0};
        Spawn const* vendor{nullptr};
        uint32 buyItem{0};
        uint32 buyLots{0};
        uint32 buyCost{0};
        Spawn const* focus{nullptr};  // a walk to it first
        std::vector<uint32> bank;     // reagents to take out of the guild bank

        bool BetterThan(Option const& o) const
        {
            if (!o.recipe)
                return true;
            if (useful != o.useful)
                return useful > o.useful;
            if (tier != o.tier)
                return tier > o.tier;
            return perPoint < o.perPoint;
        }
    };

    // where = 0: only what the bot can cast where it stands; 1: also vendor reagents and a forge in sight;
    // 2: also the guild bank
    bool Evaluate(Player* bot, CraftRecipe const& r, uint8 where, uint32 budget, std::map<uint32, uint32>& bank,
                  Option& out)
    {
        uint32 const value = bot->GetPureSkillValue(r.skill);
        uint32 const cap = bot->GetPureMaxSkillValue(r.skill);
        uint8 const tier = Tier(r, value);
        if (!tier || value >= cap || !ToolsOk(bot, r))
            return false;
        Option o;
        o.recipe = &r;
        o.tier = tier;
        o.value = value;
        o.cap = cap;
        if (r.focus && !NewRpgCraft::FocusNear(bot, r.focus))
        {
            if (!where || !(o.focus = FocusInSight(bot, r.focus)))
                return false;
        }
        // the bags (and the bank) limit the crafts; vendor reagents are bought for them with the budget
        uint32 crafts = ExpectedCrafts(r, value, cap);
        double perCraft = 0;
        for (auto const& [item, n] : r.reagents)
        {
            perCraft += double(n) * UnitPrice(item);
            if (where && ZoneVendor(bot, item))
                continue;
            uint32 const fromBank = where >= 2 ? bank[item] : 0;
            crafts = std::min(crafts, (bot->GetItemCount(item, false) + fromBank) / n);
        }
        if (!crafts)
            return false;
        uint32 money = budget;
        for (auto const& [item, n] : r.reagents)
        {
            uint32 const have = bot->GetItemCount(item, false);
            Spawn const* vendor = where ? ZoneVendor(bot, item) : nullptr;
            if (!vendor || have >= crafts * n)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            uint32 const lot = std::max<uint32>(1, proto ? proto->BuyCount : 1);
            uint32 const lotPrice = proto ? proto->BuyPrice : 0;
            uint32 lots = (crafts * n - have + lot - 1) / lot;
            if (lotPrice && lots * lotPrice > money)
                lots = money / lotPrice;
            if (o.buyItem || !lots)
            {
                // a second short vendor reagent, or no money: the crafts it has them for now (the next look buys more)
                crafts = std::min(crafts, have / n);
                continue;
            }
            crafts = std::min(crafts, (have + lots * lot) / n);
            o.vendor = vendor;
            o.buyItem = item;
            o.buyLots = lots;
            o.buyCost = lots * lotPrice;
            money -= o.buyCost;
        }
        for (auto const& [item, n] : r.reagents)
            if (where >= 2 && bank[item] && bot->GetItemCount(item, false) < std::max<uint32>(1, crafts) * n &&
                !(where && ZoneVendor(bot, item)))
                o.bank.push_back(item);
        if (!crafts && !o.buyItem)
            return false;
        o.crafts = std::max<uint32>(1, crafts);
        o.useful = Useful(r.product);
        o.perPoint = perCraft * 1000.0 / std::max(1, Chance(value, r.gray, r.yellow));
        if (!o.bank.empty() && where < 2)
            return false;
        out = o;
        return true;
    }

    uint32 TaughtSpell(uint32 trainerSpell)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(trainerSpell))
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                if (info->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL && info->Effects[e].TriggerSpell)
                    return info->Effects[e].TriggerSpell;
        return trainerSpell;
    }
}

namespace NewRpgCraft
{
    void Load()
    {
        if (loaded)
            return;
        std::unordered_set<uint32> const lines(std::begin(CRAFT_SKILLS), std::end(CRAFT_SKILLS));
        std::unordered_set<uint32> reagents, foci;
        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
            if (!ability || !lines.count(ability->SkillLine) || !ability->TrivialSkillLineRankHigh ||
                index.recipes.count(ability->Spell))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell);
            if (!info)
                continue;
            CraftRecipe r;
            r.spellId = ability->Spell;
            r.skill = ability->SkillLine;
            r.yellow = ability->TrivialSkillLineRankLow;
            r.gray = ability->TrivialSkillLineRankHigh;
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS && !r.product; ++e)
                if ((info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM ||
                     info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM_2) &&
                    info->Effects[e].ItemType)
                    r.product = info->Effects[e].ItemType;
            if (!r.product)
                continue;  // enchants on items, prospecting, milling: no product to make for skill here
            for (uint8 k = 0; k < MAX_SPELL_REAGENTS; ++k)
                if (info->Reagent[k] > 0 && info->ReagentCount[k] > 0)
                    r.reagents.emplace_back(uint32(info->Reagent[k]), info->ReagentCount[k]);
            if (r.reagents.empty())
                continue;
            r.focus = info->RequiresSpellFocus;
            r.totem = {info->Totem[0], info->Totem[1]};
            r.totemCategory = {info->TotemCategory[0], info->TotemCategory[1]};
            for (auto const& [item, n] : r.reagents)
                reagents.insert(item);
            if (r.focus)
                foci.insert(r.focus);
            index.bySkill[r.skill].push_back(r.spellId);
            index.recipes.emplace(r.spellId, std::move(r));
        }

        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
            if (!tmpl || !(tmpl->npcflag & UNIT_NPC_FLAG_VENDOR))
                continue;
            VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id);
            if (!items)
                continue;
            Spawn spawn{data.id, data.mapid, data.posX, data.posY, data.posZ, 0};
            for (VendorItem const* item : items->m_items)
            {
                if (!item || item->ExtendedCost || item->maxcount || !reagents.count(item->item))
                    continue;
                if (!spawn.zoneId)
                    spawn.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, spawn.mapId, spawn.x, spawn.y, spawn.z);
                index.vendors[item->item].push_back(spawn);
            }
        }
        size_t focusSpawns = 0;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        {
            GameObjectTemplate const* tmpl = sObjectMgr->GetGameObjectTemplate(data.id);
            if (!tmpl || tmpl->type != GAMEOBJECT_TYPE_SPELL_FOCUS || !foci.count(tmpl->spellFocus.focusId))
                continue;
            index.foci[tmpl->spellFocus.focusId].push_back({data.id, data.mapid, data.posX, data.posY, data.posZ, 0});
            ++focusSpawns;
        }
        loaded = true;
        LOG_INFO("playerbots",
                 "[guild70] craft index: {} recipes in {} skill lines, {} vendor reagents, {} focus spawns", index.recipes.size(), index.bySkill.size(), index.vendors.size(), focusSpawns);
    }

    std::string RecipeName(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        return info ? info->SpellName[0] : std::to_string(spellId);
    }

    uint32 RecipeSkill(uint32 spellId)
    {
        auto const itr = index.recipes.find(spellId);
        return itr == index.recipes.end() ? 0 : itr->second.skill;
    }

    uint32 RecipeFocus(uint32 spellId)
    {
        auto const itr = index.recipes.find(spellId);
        return itr == index.recipes.end() ? 0 : itr->second.focus;
    }

    bool FocusNear(Player* bot, uint32 focusId)
    {
        GameObject* go = nullptr;
        Acore::GameObjectFocusCheck check(bot, focusId);
        Acore::GameObjectSearcher<Acore::GameObjectFocusCheck> searcher(bot, go, check);
        Cell::VisitObjects(bot, searcher, bot->GetVisibilityRange());
        return go != nullptr;
    }

    uint32 Budget(PlayerbotAI* botAI, Player* bot)
    {
        uint32 const free = botAI->GetAiObjectContext()
                                ->GetValue<uint32>("free money for", std::to_string(uint32(NeedMoneyFor::tradeskill)))
                                ->Get();
        uint32 const ranks = NewRpgProfession::NextRankCost(bot);
        return std::min(free, bot->GetMoney()) > ranks ? std::min(free, bot->GetMoney()) - ranks : 0;
    }

    bool InSafePlace(Player* bot)
    {
        return bot->IsAlive() && !bot->IsInCombat() && bot->GetMap() && !bot->GetMap()->Instanceable() &&
               (bot->HasRestFlag(REST_FLAG_IN_CITY) || bot->HasRestFlag(REST_FLAG_IN_TAVERN));
    }

    bool HasSkillUpRecipe(Player* bot, uint32 skill)
    {
        auto const list = index.bySkill.find(skill);
        if (list == index.bySkill.end() || !bot->HasSkill(skill))
            return false;
        uint32 const value = bot->GetPureSkillValue(skill);
        for (uint32 spell : list->second)
            if (bot->HasSpell(spell) && Tier(index.recipes.at(spell), value))
                return true;
        return false;
    }

    bool TeachesSkillUpRecipe(Player* bot, uint32 trainerEntry, uint32 skill, uint32& cost)
    {
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(trainerEntry);
        if (!trainer || !bot->HasSkill(skill) || !trainer->IsTrainerValidForPlayer(bot))
            return false;
        uint32 const value = bot->GetPureSkillValue(skill);
        bool found = false;
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            if (spell.ReqSkillLine != skill || (found && spell.MoneyCost >= cost))
                continue;
            auto const recipe = index.recipes.find(TaughtSpell(spell.SpellId));
            if (recipe == index.recipes.end() || !Tier(recipe->second, value) || !trainer->CanTeachSpell(bot, &spell))
                continue;
            cost = spell.MoneyCost;
            found = true;
        }
        return found;
    }

    uint32 LearnRecipes(Player* bot, Creature* npc, uint32 skill, uint32 budget)
    {
        Trainer::Trainer* trainer = npc ? sObjectMgr->GetTrainer(npc->GetEntry()) : nullptr;
        if (!trainer || !bot->HasSkill(skill) || !trainer->IsTrainerValidForPlayer(bot))
            return 0;
        uint32 const value = bot->GetPureSkillValue(skill);
        std::vector<std::pair<uint32, uint32>> list;  // (cost, trainer spell)
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            if (spell.ReqSkillLine != skill)
                continue;
            auto const recipe = index.recipes.find(TaughtSpell(spell.SpellId));
            // a recipe that still gives skill now or later (green and gray ones give none any more)
            if (recipe == index.recipes.end() || value >= (recipe->second.yellow + recipe->second.gray) / 2 ||
                !trainer->CanTeachSpell(bot, &spell))
                continue;
            list.emplace_back(spell.MoneyCost, spell.SpellId);
        }
        std::sort(list.begin(), list.end());
        uint32 learned = 0;
        for (auto const& [cost, spellId] : list)
        {
            if (cost > budget)
                break;
            uint32 const before = bot->GetMoney();
            trainer->TeachSpell(npc, bot, spellId);  // the trainer window's path: checks, pays, learns
            if (!bot->HasSpell(TaughtSpell(spellId)))
                continue;
            uint32 const paid = before - bot->GetMoney();
            budget = budget > paid ? budget - paid : 0;
            ++learned;
            LOG_INFO("playerbots", "[guild70] {} L{} learned recipe {} at {} (skill {} {}), paid {}c ({}c left)",
                     bot->GetName(), bot->GetLevel(), RecipeName(TaughtSpell(spellId)), npc->GetName(), skill, value,
                     paid, bot->GetMoney());
        }
        return learned;
    }

    uint32 PickHere(Player* bot)
    {
        uint64 const fp = Fingerprint(bot);
        std::map<uint32, uint32> noBank;
        Option best;
        for (uint32 skill : CRAFT_SKILLS)
        {
            auto const list = index.bySkill.find(skill);
            if (list == index.bySkill.end() || !bot->HasSkill(skill))
                continue;
            for (uint32 spell : list->second)
            {
                Option o;
                if (!bot->HasSpell(spell) || RefusedBefore(bot, spell, fp) ||
                    !Evaluate(bot, index.recipes.at(spell), 0, 0, noBank, o))
                    continue;
                if (o.BetterThan(best))
                    best = o;
            }
        }
        return best.recipe ? best.recipe->spellId : 0;
    }

    bool StartCraft(PlayerbotAI* botAI, Player* bot)
    {
        if (!loaded || !InSafePlace(bot) || !sGuildCoordinator.IsPlanned(bot->GetGUID().GetCounter()))
            return false;
        uint32 const low = bot->GetGUID().GetCounter();
        uint64 const fp = Fingerprint(bot);
        {
            std::lock_guard<std::mutex> guard(lookLock);
            auto const itr = lastEmptyLook.find(low);
            if (itr != lastEmptyLook.end() && itr->second == fp)
                return false;
        }
        auto const nothing = [&]()
        {
            std::lock_guard<std::mutex> guard(lookLock);
            lastEmptyLook[low] = fp;
            return false;
        };

        uint32 const budget = Budget(botAI, bot);
        std::map<uint32, uint32> bank;
        Guild* guild = GuildBankRun::BotGuild(bot);
        bool const bankOk = guild && GuildBankRun::BankState(bot, false).empty();
        if (bankOk)
            GuildBankRun::CountBank(guild, bank);

        Option now, banked;
        for (uint32 skill : CRAFT_SKILLS)
        {
            auto const list = index.bySkill.find(skill);
            if (list == index.bySkill.end() || !bot->HasSkill(skill))
                continue;
            for (uint32 spell : list->second)
            {
                if (!bot->HasSpell(spell) || RefusedBefore(bot, spell, fp))
                    continue;
                CraftRecipe const& r = index.recipes.at(spell);
                Option o;
                if (Evaluate(bot, r, 1, budget, bank, o) && o.BetterThan(now))
                    now = o;
                if (bankOk && Evaluate(bot, r, 2, budget, bank, o) && !o.bank.empty() && o.BetterThan(banked))
                    banked = o;
            }
        }
        // the best one needs reagents from the guild bank: the coordinator's withdraw run fetches them
        if (banked.recipe && banked.BetterThan(now))
            for (uint32 item : banked.bank)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                sGuildCoordinator.RequestWithdraw(bot, item,
                                                  "реагент для " + RecipeName(banked.recipe->spellId) + " (" +
                                                      (proto ? proto->Name1 : std::to_string(item)) + ")");
                LOG_INFO("playerbots", "[guild70] {} L{} asks the guild bank for {} for {} (skill {})",
                         bot->GetName(), bot->GetLevel(), proto ? proto->Name1 : std::to_string(item),
                         RecipeName(banked.recipe->spellId), banked.value);
            }
        if (!now.recipe)
            return nothing();

        NewRpgInfo::DoProfession task;
        task.kind = TASK_CRAFT;
        task.skill = now.recipe->skill;
        task.spellId = now.recipe->spellId;
        task.pos = WorldPosition(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        std::string where = "here";
        if (now.buyItem)
        {
            task.npcEntry = now.vendor->entry;
            task.pos = WorldPosition(now.vendor->mapId, now.vendor->x, now.vendor->y, now.vendor->z);
            task.itemId = now.buyItem;
            task.count = now.buyLots;
            task.cost = now.buyCost;
            CreatureTemplate const* npc = sObjectMgr->GetCreatureTemplate(task.npcEntry);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(task.itemId);
            where = "buys " + std::to_string(task.count) + " lots of " + (proto ? proto->Name1 : "?") + " at " +
                    (npc ? npc->Name : "?") + " for " + std::to_string(task.cost) + "c";
        }
        else if (now.focus)
        {
            task.pos = WorldPosition(now.focus->mapId, now.focus->x, now.focus->y, now.focus->z);
            char const* const what = now.recipe->focus == 1 ? "anvil" : now.recipe->focus == 3 ? "forge" : "focus";
            where = std::string("at the ") + what + " " + std::to_string(uint32(bot->GetExactDist(task.pos))) + " yd";
        }
        if (bot->GetExactDist(task.pos) > 10.0f &&
            NewRpgBaseAction::TripRefused(bot, task.pos.GetPositionX(), task.pos.GetPositionY(),
                                          task.pos.GetPositionZ(), "craft " + RecipeName(task.spellId)))
            return nothing();
        LOG_INFO("playerbots",
                 "[guild70] {} L{} CRAFT reason: {} skill {}/{} ({}), ~{} crafts, product use {}, {:.0f}c per "
                 "point, {}",
                 bot->GetName(), bot->GetLevel(), RecipeName(task.spellId), now.value, now.cap,
                 now.tier == 2 ? "orange" : "yellow", now.crafts, now.useful, now.perPoint, where);
        botAI->rpgInfo.ChangeToDoProfession(task);
        return true;
    }

    void OnCreateItem(Player* player, Item* item, uint32 count)
    {
        PlayerbotAI* ai = player ? GET_PLAYERBOT_AI(player) : nullptr;
        if (!ai || !item)
            return;
        auto const* task = std::get_if<NewRpgInfo::DoProfession>(&ai->rpgInfo.data);
        if (!task || task->kind != TASK_CRAFT || !task->castSpell)
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_CONTAINER)
            return;  // potions, food, bandages, bags: the crafter keeps and uses them
        sGuildCoordinator.OnLoot(player, item, count);  // an upgrade for a member, the bank, sale or disenchant
    }
}

// the TASK_CRAFT steps: buy the vendor reagent, walk to the forge / anvil / fire, then craft until nothing here
// still gives skill
bool NewRpgDoProfessionAction::ExecuteCraft(NewRpgInfo::DoProfession& data)
{
    NewRpgInfo& info = botAI->rpgInfo;
    if (bot->IsNonMeleeSpellCast(false))
        return true;  // the craft in progress
    if (data.castSpell)
    {
        uint32 const after = bot->HasSkill(data.skill) ? bot->GetPureSkillValue(data.skill) : 0;
        LOG_INFO("playerbots", "[guild70] {} craft {} skill {}→{}", bot->GetName(),
                 NewRpgCraft::RecipeName(data.castSpell), data.skillBefore, after);
        sGuildCoordinator.OnCrafted(bot, data.castSpell, data.skillBefore, after);
        data.castSpell = 0;
        ++data.crafted;
    }

    // the vendor reagent first
    if (data.npcEntry)
    {
        if (data.npc.IsEmpty())
        {
            if (bot->GetExactDist(data.pos) > 10.0f)
            {
                bool const moved = MoveFarTo(data.pos);
                if (!std::holds_alternative<NewRpgInfo::DoProfession>(info.data))
                    return true;  // the walk was given up
                if (moved)
                    return true;
                return MoveRandomNear(10.0f);
            }
            Creature* npc = bot->FindNearestCreature(data.npcEntry, 60.0f);
            if (!npc)
            {
                LOG_INFO("playerbots", "[guild70] {} finds no reagent vendor {} at its spawn", bot->GetName(),
                         data.npcEntry);
                info.ChangeToIdle();
                return true;
            }
            data.npc = npc->GetGUID();
        }
        Creature* npc = ObjectAccessor::GetCreature(*bot, data.npc);
        if (!npc || !npc->IsAlive())
        {
            info.ChangeToIdle();
            return true;
        }
        if (!IsWithinInteractionDist(npc))
        {
            if (MoveWorldObjectTo(data.npc))
                return true;
            return MoveRandomNear(15.0f);
        }
        bot->SetFacingToObject(npc);
        uint32 const moneyBefore = bot->GetMoney();
        uint32 const before = bot->GetItemCount(data.itemId, false);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(data.itemId);
        if (VendorItemData const* items = npc->GetVendorItems())
            for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
            {
                VendorItem const* item = items->GetItem(slot);
                if (!item || item->item != data.itemId || item->ExtendedCost)
                    continue;
                uint32 const stack = proto ? std::max<uint32>(1, proto->GetMaxStackSize() /
                                                                     std::max<uint32>(1, proto->BuyCount))
                                           : 1;
                for (uint32 left = data.count; left;)
                {
                    uint32 const lots = std::min<uint32>({left, stack, 255u});
                    if (!bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, data.itemId, uint8(lots), NULL_BAG,
                                                    NULL_SLOT))
                        break;
                    left -= lots;
                }
                break;
            }
        uint32 const got = bot->GetItemCount(data.itemId, false) - before;
        LOG_INFO("playerbots", "[guild70] {} L{} {} {} ×{} for {} at {}: paid {}c ({}c left)", bot->GetName(),
                 bot->GetLevel(), got ? "bought" : "could not buy", proto ? proto->Name1 : "?", got,
                 NewRpgCraft::RecipeName(data.spellId), npc->GetName(), moneyBefore - bot->GetMoney(), bot->GetMoney());
        data.npcEntry = 0;
        data.npc.Clear();
        // the forge / anvil the recipe needs, if not in reach here: walk to the one in sight
        data.pos = WorldPosition(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (uint32 const focus = NewRpgCraft::RecipeFocus(data.spellId))
            if (!NewRpgCraft::FocusNear(bot, focus))
            {
                info.ChangeToIdle();  // the next look plans the walk to it
                return true;
            }
        if (!got)
        {
            info.ChangeToIdle();
            return true;
        }
    }

    // to the forge / anvil / fire
    if (uint32 const focus = data.crafted ? 0 : NewRpgCraft::RecipeFocus(data.spellId))
        if (!NewRpgCraft::FocusNear(bot, focus) && bot->GetExactDist(data.pos) > 3.0f)
        {
            bool const moved = MoveFarTo(data.pos);
            if (!std::holds_alternative<NewRpgInfo::DoProfession>(info.data))
                return true;
            if (moved)
                return true;
        }

    if (bot->isMoving())
    {
        bot->StopMoving();
        return true;
    }
    uint32 const spell = NewRpgCraft::PickHere(bot);
    if (!spell)
    {
        LOG_INFO("playerbots", "[guild70] {} L{} crafting done here: {} crafts, nothing here still gives skill",
                 bot->GetName(), bot->GetLevel(), data.crafted);
        if (!data.crafted && data.spellId)
        {
            // the planned recipe could not be made here after all: not planned again in this state
            uint64 const fp = Fingerprint(bot);
            std::lock_guard<std::mutex> guard(lookLock);
            auto& entry = refused[bot->GetGUID().GetCounter()];
            if (entry.first != fp)
                entry = {fp, {}};
            entry.second.insert(data.spellId);
        }
        info.ChangeToIdle();
        return true;
    }
    uint32 const skill = NewRpgCraft::RecipeSkill(spell);
    data.skill = skill;
    data.skillBefore = bot->HasSkill(skill) ? bot->GetPureSkillValue(skill) : 0;
    data.spellId = spell;
    data.castSpell = spell;
    if (!botAI->CastSpell(spell, bot))
    {
        LOG_INFO("playerbots", "[guild70] {} cannot cast {} here (tool, focus, bags?)", bot->GetName(),
                 NewRpgCraft::RecipeName(spell));
        data.castSpell = 0;
        uint64 const fp = Fingerprint(bot);
        {
            std::lock_guard<std::mutex> guard(lookLock);
            auto& entry = refused[bot->GetGUID().GetCounter()];
            if (entry.first != fp)
                entry = {fp, {}};
            entry.second.insert(spell);
        }
        return true;  // the next pick takes another recipe
    }
    return true;
}
