/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BuyAction.h"  // [gold41]
#include "GuildCoordinator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <tuple>

#include "AiFactory.h"  // [roster3] the talent tree in the gear key
#include "AiObjectContext.h"
#include "Bag.h"
#include "BudgetValues.h"  // [guild50] free money for gear
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DpsEstimate.h"  // [questrate] the kill time of the questing formula
#include "Engine.h"
#include "Formulas.h"
#include "GameObject.h"  // [questrate] chest places
#include "GameTime.h"
#include "GlyphActions.h"  // [insc143] the members' premade glyphs
#include "Group.h"
#include "Guild.h"
#include "GuildBankRun.h"
#include "GuildGatherPlan.h"  // [gather]
#include "GuildGear.h"        // [guild50]
#include "GuildHonest.h"
#include "GuildLedger.h"  // [eco190]
#include "GuildRiding.h"   // [mount1]
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LootMgr.h"          // [questrate] the quest items' creature sources
#include "LootObjectStack.h"  // [questrate] chest places
#include "Mail.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "NewRpgBaseAction.h"  // [guild14] TripRefused
#include "NewRpgCoop.h"        // [coop] the co-op hooks
#include "NewRpgCraft.h"       // [guild70] crafted products, the crafting wait
#include "NewRpgFishing.h"     // [gather] fishing places
#include "NewRpgInfo.h"
#include "NewRpgProfession.h"  // [guild50] BUY at a vendor
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Pet.h"  // [diag252]
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"  // [questrate] sight / loot distance
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "QuestAction.h"  // [inv380] the grey rule a quest-starter item gets (CanAcceptQuest)
#include "QuestDef.h"
#include "QuestPackets.h"  // [questvalue] the abandon
#include "QuestStats.h"    // [questvalue] t= stamps
#include "ReputationMgr.h"  // [craft402] the reputation ranks of the upgrade check
#include "ScriptMgr.h"
#include "SelfBotControl.h"
#include "SlowScope.h"  // [perf211]
#include "SellAction.h"
#include "SpellAuras.h"  // [diag253]
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "StringFormat.h"
#include "Timer.h"
#include "UpdateTime.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

namespace
{
    constexpr uint32 SPELL_DISENCHANT = 13262;
    constexpr size_t ADDON_LINE_MAX = 240;  // the client drops longer addon messages
    constexpr uint32 STUCK_SECONDS = 60;     // user rule: no movement beyond combat reach for 60 s
    constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;  // [diag253]
    constexpr uint32 SKILL_STEP_MAX = 75;    // Spell::EffectLearnSkill: a profession rank's cap = step x 75
    constexpr size_t TASKS_SENT_MAX = 8;     // OT lines per bot (the addon line budget)
    uint32 const CRAFT_LINES[] = {164, 165, 171, 197, 202, 333, 755, 773, 185, 129};

    char const* OrderName(GuildOrderType type)
    {
        static char const* const names[] = {
            "QUEST",      "PROFESSION",   "BANK_DEPOSIT", "BANK_WITHDRAW", "CRAFT_FOR", "GATHER",   "BUY", "DUNGEON",
            "DISENCHANT", "QUEST_REWARD", "FOUND_GUILD",  "SIGN_CHARTER",  "JOIN",      "BANK_TAB",  // [guild80]
            "TRADE_TO",   "MAIL_TO",      "DONATE_GOLD",                                             // [guild81]
            "SELL",                                                                                  // [tasks]
            "COOP",                                                                                  // [coop]
            "REPAIR"};                                                                               // [regress10]
        return uint8(type) < std::size(names) ? names[uint8(type)] : "?";
    }

    // [tasks] the task list's codes (logs, the addon's OT line) and Russian names
    char const* PriorityCode(GuildTaskPriority p)
    {
        return p == GuildTaskPriority::URGENT ? "U" : p == GuildTaskPriority::HELP ? "H" : "C";
    }
    char const* PriorityRu(GuildTaskPriority p)
    {
        return p == GuildTaskPriority::URGENT ? "urgent" : p == GuildTaskPriority::HELP ? "help" : "when convenient";
    }

    char const* PlaceCode(GuildTaskPlace p)
    {
        static char const* const codes[] = {"F", "T", "M", "B", "V", "R"};
        return uint8(p) < std::size(codes) ? codes[uint8(p)] : "?";
    }

    char const* PlaceRu(GuildTaskPlace p)
    {
        static char const* const names[] = {"field", "town", "mailbox", "bank", "vendor", "trainer"};
        return uint8(p) < std::size(names) ? names[uint8(p)] : "?";
    }

    char const* SourceCode(GuildTaskSource s)
    {
        return s == GuildTaskSource::GLOBAL ? "G" : s == GuildTaskSource::FIVE ? "5" : "S";
    }

    char const* ProfessionKindName(GuildProfessionKind k)
    {
        static char const* const names[] = {"none", "train", "gather", "buy", "craft"};
        return uint8(k) < std::size(names) ? names[uint8(k)] : "?";
    }

    // [regress10] the gear the global leader has repaired: items broken or with no more durability than one death
    // takes; urgent = a weapon among them (main / off hand, ranged) or a broken armour piece; the core's repair cost
    // of those items (Player::DurabilityRepair, no reputation discount)
    struct RepairNeed
    {
        bool any{false};
        bool urgent{false};
        bool weaponBroken{false};  // the main hand, a hunter's ranged weapon
        uint32 cost{0};            // all items in need
        uint32 urgentCost{0};      // the urgent ones
        std::string items;         // "Worn Dirk 0/20, ..." (the [gear] durability line, the task's details)
    };

    uint32 ItemRepairCost(Item* item)
    {
        uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        uint32 const curDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        ItemTemplate const* proto = item->GetTemplate();
        if (!maxDurability || curDurability >= maxDurability || !proto)
            return 0;
        DurabilityCostsEntry const* costs = sDurabilityCostsStore.LookupEntry(proto->ItemLevel);
        DurabilityQualityEntry const* quality = sDurabilityQualityStore.LookupEntry((proto->Quality + 1) * 2);
        if (!costs || !quality)
            return 0;
        uint32 const multiplier =
            costs->multiplier[ItemSubClassToDurabilityMultiplierId(proto->Class, proto->SubClass)];
        uint32 const cost = uint32(uint32(maxDurability - curDurability) * multiplier * double(quality->quality_mod) *
                                   sWorld->getRate(RATE_REPAIRCOST));
        return std::max<uint32>(cost, 1);
    }

    RepairNeed RepairNeedOf(Player* p)
    {
        RepairNeed need;
        float const deathLoss = NewRpgBaseAction::DeathDurabilityLoss(p);
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            uint32 const maxDurability = item ? item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) : 0;
            if (!maxDurability)
                continue;
            uint32 const cur = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            bool const broken = item->IsBroken();
            if (!broken && float(cur) > deathLoss * float(maxDurability))
                continue;
            bool const weapon = slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND ||
                                slot == EQUIPMENT_SLOT_RANGED;
            uint32 const cost = ItemRepairCost(item);
            need.any = true;
            need.cost += cost;
            if (weapon || broken)
            {
                need.urgent = true;
                need.urgentCost += cost;
            }
            if (broken && (slot == EQUIPMENT_SLOT_MAINHAND ||
                           (slot == EQUIPMENT_SLOT_RANGED && p->getClass() == CLASS_HUNTER)))
                need.weaponBroken = true;
            ItemTemplate const* proto = item->GetTemplate();
            need.items += Acore::StringFormat("{}{} {}/{}", need.items.empty() ? "" : ", ",
                                              proto ? proto->Name1 : std::to_string(item->GetEntry()), cur,
                                              maxDurability);
        }
        return need;
    }

    std::string TaskKey(GuildOrder const& o)
    {
        return std::to_string(uint32(o.type)) + ":" + std::to_string(o.itemId) + ":" + std::to_string(o.forLow) + ":" +
               std::to_string(o.transferId) + ":" + std::to_string(o.spellId) + ":" + std::to_string(o.skillLine) +
               ":" + std::to_string(uint32(o.profession));
    }

    // [tasks] one town: the bot's area and the spot's area share an area below their zone (Goldshire and the inn in
    // it are one town; two villages of one zone are not)
    bool SameTown(uint32 botArea, uint32 spotArea)
    {
        auto const chain = [](uint32 area)
        {
            std::vector<uint32> out;
            for (uint32 depth = 0; area && depth < 4; ++depth)
            {
                AreaTableEntry const* entry = sAreaTableStore.LookupEntry(area);
                if (!entry || !entry->zone)
                    break;  // the zone itself is no town
                out.push_back(area);
                area = entry->zone;
            }
            return out;
        };
        std::vector<uint32> const a = chain(botArea);
        std::vector<uint32> const b = chain(spotArea);
        if (botArea == spotArea)
            return true;
        return std::any_of(a.begin(), a.end(), [&](uint32 x) { return std::find(b.begin(), b.end(), x) != b.end(); });
    }

    // [tasks] the task the bot runs now (the first executable one not waiting for its town) and how many it holds
    std::string ActiveTask(std::vector<GuildOrder> const& tasks)
    {
        GuildOrder const* active = nullptr;
        uint32 others = 0;
        for (GuildOrder const& o : tasks)
        {
            if (o.type == GuildOrderType::QUEST)
                continue;
            ++others;
            if (!active && o.executable && !o.waitsTown)
                active = &o;
        }
        std::string const name = active ? OrderName(active->type) : "QUEST";
        return others ? name + " (tasks " + std::to_string(others) + ")" : name;
    }

    // [bank409] the withdraw purpose of a bank item nobody claims: its new holder gets the loot decision for it
    constexpr char const* UNCLAIMED_BANK = "[bank409] unclaimed";

    // [bank411] "" = the bank takes at least one of the held items now (room per item: its stacks, empty slots),
    // else why not
    template <class Held>
    std::string DepositStateOf(Player* p, Held const& items)
    {
        std::string why;
        for (auto const& h : items)
        {
            std::string const w = GuildBankRun::DepositState(p, h.itemId, h.count);
            if (w.empty())
                return w;
            if (why.empty())
                why = w;
        }
        return why;
    }

    uint32 FreeBagSlots(Player* p)
    {
        uint32 free = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            free += p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) ? 0 : 1;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                free += bag->GetFreeSlots();
        return free;
    }

    constexpr uint32 TOTEM_CATEGORY_RUNED_COPPER_ROD = 6;  // [craft400] TotemCategory.dbc: Runed Copper Rod

    // [econ2] the tool a profession's work needs (NewRpgProfession's list), 0 = none
    uint32 ProfessionTool(uint32 line)
    {
        switch (line)
        {
            case SKILL_MINING:        return 2901;   // Mining Pick
            case SKILL_SKINNING:      return 7005;   // Skinning Knife
            case SKILL_BLACKSMITHING: return 5956;   // Blacksmith Hammer
            case SKILL_ENGINEERING:   return 6219;   // Arclight Spanner
            case SKILL_JEWELCRAFTING: return 20815;  // Jeweler's Kit
            case SKILL_INSCRIPTION:   return 39505;  // Virtuoso Inking Set
            case SKILL_FISHING:       return 6256;   // Fishing Pole
            // [craft400] the Copper Rod (trade vendors) the enchanter makes its Runed Copper Rod of (Copper Rod,
            // Strange Dust, Lesser Magic Essence: [ench157]); every first enchant of the line needs that rod. b456:
            // none of the five enchanters held a rod, the bracer / chest enchants never castable
            case SKILL_ENCHANTING:    return 6217;
            default:                  return 0;
        }
    }

    bool HasProfessionTool(Player* p, uint32 line)
    {
        uint32 const tool = ProfessionTool(line);
        if (!tool)
            return true;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tool);
        if (proto && proto->TotemCategory && p->HasItemTotemCategory(proto->TotemCategory))
            return true;
        if (line == SKILL_ENCHANTING && p->HasItemTotemCategory(TOTEM_CATEGORY_RUNED_COPPER_ROD))  // [craft400]
            return true;
        return p->HasItemCount(tool, 1, false);
    }

    char const* RpgName(uint32 status)
    {
        static char const* const names[] = {"IDLE", "GO_GRIND", "GO_CAMP", "WANDER_RANDOM", "WANDER_NPC", "DO_QUEST",
                                             "TRAVEL_FLIGHT", "REST", "OUTDOOR_PVP", "DO_GATHER", "DO_PROFESSION"};
        return status < std::size(names) ? names[status] : "?";
    }

    char const* SkillShort(uint32 line)
    {
        switch (line)
        {
            case 164:
                return "BS";
            case 165:
                return "LW";
            case 171:
                return "Alch";
            case 197:
                return "Tail";
            case 202:
                return "Eng";
            case 333:
                return "Ench";
            case 755:
                return "JC";
            case 773:
                return "Insc";
            case 185:
                return "Cook";
            case 129:
                return "FA";
            case 186:
                return "Mine";
            case 182:
                return "Herb";
            case 393:
                return "Skin";
            case 356:  // [ench163]
                return "Fish";
            default: return "?";
        }
    }

    // [guild70] the crafters of a skill line, plural (the wanted list's breakdown)
    char const* SkillPlural(uint32 line)
    {
        switch (line)
        {
            case 164:
                return "blacksmiths";
            case 165:
                return "leatherworkers";
            case 171:
                return "alchemists";
            case 197:
                return "tailors";
            case 202:
                return "engineers";
            case 333:
                return "enchanters";
            case 755:
                return "jewelcrafters";
            case 773:
                return "scribes";
            case 185:
                return "cooks";
            case 129:
                return "first-aiders";
            default: return "?";
        }
    }

    std::string OrdersText(std::string text, size_t max)
    {
        std::replace(text.begin(), text.end(), ';', ',');
        std::replace(text.begin(), text.end(), '\t', ' ');
        if (text.size() <= max)
            return text;
        while (max && (uint8(text[max]) & 0xC0) == 0x80)  // [guild70] never cut a UTF-8 letter in two
            --max;
        return text.substr(0, max);
    }

    void SendLine(Player* player, std::string const& line)
    {
        std::string text = std::string(SelfBotControl::ADDON_PREFIX) + "\t" + line.substr(0, ADDON_LINE_MAX);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    uint32 Now() { return uint32(GameTime::GetGameTime().count()); }

    // the engine's last-action trail, newest part only
    std::string LastAction(PlayerbotAI* ai)
    {
        if (!ai || !ai->GetCurrentEngine())
            return "-";
        std::string const all = ai->GetCurrentEngine()->GetLastAction();
        return all.size() > 140 ? "..." + all.substr(all.size() - 140) : all;
    }

    std::string ItemName(uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return proto ? proto->Name1 : std::to_string(itemId);
    }

    // Player::UpdateCraftSkill -> CraftSkillGainChance (per mille), the same formula and config values
    int32 CraftChance(uint32 skillValue, uint32 grayLevel, uint32 yellowLevel)
    {
        int32 const orange = int32(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE) * 10);
        int32 const gray = int32(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREY) * 10);
        if (grayLevel <= yellowLevel)
            return skillValue < grayLevel ? orange : gray;
        if (skillValue <= yellowLevel)
            return orange;
        if (skillValue >= grayLevel)
            return gray;
        return gray + int32(int64(grayLevel - skillValue) * (orange - gray) / (grayLevel - yellowLevel));
    }

    // [ench165] Player::UpdateCraftSkill for a Disenchant: its skill-up chance at `skill` (its SkillLineAbility
    // trivial ranks, as for a recipe)
    int32 DisenchantSkillUpChance(uint32 skill)
    {
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(SPELL_DISENCHANT);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (itr->second->SkillLine == SKILL_ENCHANTING)
                return CraftChance(skill, itr->second->TrivialSkillLineRankHigh, itr->second->TrivialSkillLineRankLow);
        return 0;
    }

    // [guild70] a recipe's skill-up chance at `value` and its expected cost per skill point (gathered units, vendor
    // reagents free) when it is orange or yellow there; false when green / gray
    template <class R>
    bool SkillUpCost(R const& r, uint32 value, std::unordered_set<uint32> const& vendor, double& cost, double& units,
                     int32& chance)
    {
        if (value >= (r.yellow + r.gray) / 2)
            return false;
        chance = CraftChance(value, r.gray, r.yellow);
        if (chance <= 0)
            return false;
        double gathered = 0, all = 0;
        for (auto const& [item, n] : r.reagents)
        {
            all += n;
            gathered += vendor.count(item) ? 0 : n;
        }
        double const expected = 1000.0 / chance;
        cost = expected * gathered;
        units = expected * all;
        return true;
    }

    std::string ConText(Player* bot, Creature* killer)
    {
        int32 const diff = int32(killer->GetLevel()) - int32(bot->GetLevel());
        std::string con = diff >= 5 ? "red" : diff >= 3 ? "orange" : diff >= -2 ? "yellow" : "green";
        if (killer->GetLevel() <= Acore::XP::GetGrayLevel(bot->GetLevel()))
            con = "gray";
        std::string text = con + (diff >= 0 ? " +" : " ") + std::to_string(diff);
        if (killer->isElite())
            text += " elite";
        if (killer->IsDungeonBoss() || killer->isWorldBoss())
            text += " boss";
        uint32 const attackers = uint32(bot->getAttackers().size());
        if (attackers > 1)
            text += ", " + std::to_string(attackers) + " attackers";
        return text;
    }
}

// [guild50] gear planner helpers
namespace
{
    bool FriendlyNpc(Player* bot, uint32 entry)
    {
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        FactionTemplateEntry const* faction = tmpl ? sFactionTemplateStore.LookupEntry(tmpl->faction) : nullptr;
        return own && faction && !faction->IsHostileTo(*own) && !own->IsHostileTo(*faction);
    }

    // equal gain: the way that costs the least (already the guild's, on the way, made by a member, paid)
    uint32 KindRank(uint8 kind)
    {
        switch (kind)
        {
            case GuildGear::SRC_BANK: return 0;
            case GuildGear::SRC_QUEST: return 1;
            case GuildGear::SRC_CRAFT: return 2;
            case GuildGear::SRC_VENDOR: return 3;
            default: return 4;
        }
    }

    bool HasReagents(Player* crafter, GuildGear::CraftRecipe const& recipe)
    {
        for (auto const& [item, n] : recipe.reagents)
            if (crafter->GetItemCount(item, false) < n)
                return false;
        return true;
    }

    // [ui11] a pick's gain text: GuildGear::GainText (an empty slot, a near-empty one, "+N%")
    template <typename Pick>
    std::string GainText(Pick const& g)
    {
        return GuildGear::GainText(g.gain, g.score, g.current);
    }

    // [guild70] money as g / s / c
    std::string Gsc(uint64 copper)
    {
        std::string text;
        if (copper >= GOLD)
            text += std::to_string(copper / GOLD) + "g ";
        if (copper >= SILVER)
            text += std::to_string((copper % GOLD) / SILVER) + "s ";
        return text + std::to_string(copper % SILVER) + "c";
    }

    // the core's price of a guild bank tab (Guild::_GetGuildBankTabPrice: the GuildBankTabCost config values)
    uint32 GuildTabPrice(uint8 tab)
    {
        static ServerConfigs const costs[] = {CONFIG_GUILD_BANK_TAB_COST_0, CONFIG_GUILD_BANK_TAB_COST_1,
                                                CONFIG_GUILD_BANK_TAB_COST_2, CONFIG_GUILD_BANK_TAB_COST_3,
                                                CONFIG_GUILD_BANK_TAB_COST_4, CONFIG_GUILD_BANK_TAB_COST_5};
        return tab < std::size(costs) ? sWorld->getIntConfig(costs[tab]) : 0;
    }

    std::string MoneyText(uint32 copper)
    {
        std::string text;
        if (copper >= GOLD)
            text += std::to_string(copper / GOLD) + "g ";
        if (copper >= SILVER)
            text += std::to_string((copper % GOLD) / SILVER) + "s ";
        return text + std::to_string(copper % SILVER) + "c";
    }

    // [inv376] a generic bag (not a quiver, soul, herb or other special bag)
    bool GenericBagProto(ItemTemplate const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER;
    }

    // the smallest bag size in the member's bag slots (0 = a slot is empty); [inv376] of its generic bags only: a
    // quiver or a special bag is no place for a generic one
    uint32 SmallestBag(Player* p)
    {
        uint32 smallest = UINT32_MAX;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        {
            Bag* bag = p->GetBagByPos(b);
            if (!bag || GenericBagProto(bag->GetTemplate()))
                smallest = std::min<uint32>(smallest, bag ? bag->GetBagSize() : 0);
        }
        return smallest == UINT32_MAX ? 0 : smallest;
    }

    // [inv376] the bag slots a generic bag of `slots` gains `p` once its own spare bags and those coming to it are worn
    // (each into its smallest generic slot or a free one): 0 when it gains nothing
    uint32 BagGain(Player* p, uint32 slots, std::vector<uint32> spares)
    {
        std::vector<uint32> worn;  // free bag slots as 0
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        {
            Bag* bag = p->GetBagByPos(b);
            if (!bag || GenericBagProto(bag->GetTemplate()))
                worn.push_back(bag ? bag->GetBagSize() : 0);
        }
        if (worn.empty())
            return 0;
        std::sort(spares.rbegin(), spares.rend());
        for (uint32 spare : spares)
        {
            auto const least = std::min_element(worn.begin(), worn.end());
            if (spare > *least)
                *least = spare;
        }
        uint32 const least = *std::min_element(worn.begin(), worn.end());
        return slots > least ? slots - least : 0;
    }

    // the generic bags in the backpack and in the bags (not worn as a bag)
    void SpareBags(Player* p, std::vector<Item*>& out)
    {
        auto const add = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER)
                out.push_back(item);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    add(bag->GetItemByPos(uint8(j)));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// startup
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::Load()
{
    LoadPlan();
    GuildHonest::LogStartup(planIndex_.size());  // [honest] the handouts the guild-plan bots never get
    LoadRecipes();
    LoadGatherSources();
    LoadQuestValue();  // [questrate] the questing value formula's static part
    sGuildGatherPlan.Load();  // [gather] the gathering places of the wanted items
    GuildGear::Load(std::vector<uint32>(std::begin(CRAFT_LINES), std::end(CRAFT_LINES)));  // [guild50]
    GuildBankRun::LoadVaults();
    LoadDungeons();  // [guild60] the fives' dungeons and the completion table
    LoadRuns();
    LoadGuildMasters();  // [guild80] where the bots found their guild
    LoadMailboxes();     // [guild81] bot <-> bot transfers by mail
    // [coop] the global leader's side of co-op (lock order: co-op's board, then lock_)
    NewRpgCoop::SetCoordinatorHooks({[this](Player* b, std::string* w) { return UrgentTask(b, w); },
                                     [this](uint32 l, uint32 partner, uint32 q, bool lead, std::string const& w)
                                     { AddCoopTask(l, partner, q, lead, w); },
                                     [this](uint32 l, uint32 q, std::string const& w) { EndCoopTask(l, q, w); },
                                     [this](uint32 l, std::string* w) { return AssistWeight(l, w); },  // [assist2]
                                     [this](uint32 l, uint32 place, bool done) { OnEscortEnd(l, place, done); },
                                     [this](uint32 l) { return FiveWanted(l); }});  // [assist18]
    loaded_ = true;
}

void GuildCoordinator::LoadPlan()
{
    plan_.clear();
    planIndex_.clear();
    if (QueryResult result =
            PlayerbotsDatabase.Query("SELECT guid, num, name, grp, role, prof1, prof2, secondary FROM guild_project_plan "
                                     "ORDER BY num"))
    {
        do
        {
            Field* f = result->Fetch();
            PlanRow row;
            row.low = f[0].Get<uint32>();
            row.num = f[1].Get<uint8>();
            row.name = f[2].Get<std::string>();
            std::string const grp = f[3].Get<std::string>();
            row.grp = grp.empty() ? 'P' : grp[0];
            row.role = f[4].Get<std::string>();
            for (uint32 i : {5u, 6u})
                if (uint32 const prof = f[i].Get<uint16>())
                    row.profs.push_back(prof);
            std::stringstream sec(f[7].Get<std::string>());  // [guild70] "185,129"
            std::string part;
            while (std::getline(sec, part, ','))
                if (uint32 const skill = uint32(std::strtoul(part.c_str(), nullptr, 10)))
                    row.secondary.push_back(skill);
            if (planIndex_.emplace(row.low, plan_.size()).second)
                plan_.push_back(row);
        } while (result->NextRow());
    }
    // the user's pack characters not in the plan (his own character among them)
    std::stringstream in(sConfigMgr->GetOption<std::string>("Playerbots.UserPack.Guids", "", false));
    std::string token;
    while (std::getline(in, token, ','))
    {
        uint32 const low = uint32(std::strtoul(token.c_str(), nullptr, 10));
        if (low && planIndex_.emplace(low, plan_.size()).second)
            plan_.push_back({low, 0, "", 'P', "pack", {}});
    }
    aiCount_ = plan_.size();
    aiTimeUs_ = std::make_unique<std::atomic<uint64>[]>(aiCount_ ? aiCount_ : 1);
    for (size_t i = 0; i < aiCount_; ++i)
        aiTimeUs_[i] = 0;
    LOG_INFO("playerbots", "[guild30] coordinator: {} planned characters", plan_.size());
}

void GuildCoordinator::LoadRecipes()
{
    trainerRanks_.clear();
    recipesBySkill_.clear();
    smeltFrom_.clear();
    vendorItems_.clear();
    vendorMaps_.clear();
    reagentVendors_.clear();
    craftedFrom_.clear();
    skinned_.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT Item FROM skinning_loot_template WHERE Reference = 0"))
    {
        do
            skinned_.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }

    // trainer recipes: spell -> (skill rank, level); a trainer spell that teaches another spell stands for that one
    std::unordered_map<uint32, std::pair<uint32, uint32>> trainerReq;
    std::unordered_map<uint32, uint32> trainerFee;  // [prof272] taught spell -> the lowest MoneyCost
    if (QueryResult result = WorldDatabase.Query(
            "SELECT SpellId, ReqSkillLine, ReqSkillRank, ReqLevel, MoneyCost FROM trainer_spell"))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 const trainerSpell = f[0].Get<uint32>();
            uint32 spellId = trainerSpell;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                    if (info->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL && info->Effects[e].TriggerSpell)
                        spellId = info->Effects[e].TriggerSpell;
            // [tasks] a profession rank (Apprentice ... Grand Master): its level, the skill it needs, its new cap.
            // [econ1] b350 "trainer ranks: 0 skill lines": the trainer's spell (2020 Apprentice Blacksmith) carries
            // the SKILL_STEP effect itself and teaches 2018, whose skill effect is SPELL_EFFECT_SKILL (the step in
            // its value); the check looked only at 2018's SKILL_STEP, so every TrainerCap was 0 and every craft lag
            // "need 0" (b350: Enchanting max 2, Inscription 1, ... while the plans asked for 75 / 150). Both
            // spells are looked at, both effect kinds count (Spell::EffectLearnSkill / EffectSkill: step x 75)
            bool rankFound = false;
            for (uint32 const id : {trainerSpell, spellId})
            {
                SpellInfo const* rank = rankFound ? nullptr : sSpellMgr->GetSpellInfo(id);
                if (!rank)
                    continue;
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS && !rankFound; ++e)
                    if ((rank->Effects[e].Effect == SPELL_EFFECT_SKILL_STEP ||
                         rank->Effects[e].Effect == SPELL_EFFECT_SKILL) &&
                        rank->Effects[e].MiscValue > 0)
                    {
                        uint32 const maxSkill = uint32(std::max(0, rank->Effects[e].CalcValue())) * SKILL_STEP_MAX;
                        if (!maxSkill)
                            continue;
                        rankFound = true;
                        auto& ranks = trainerRanks_[uint32(rank->Effects[e].MiscValue)];
                        auto const same = std::find_if(ranks.begin(), ranks.end(),
                                                       [&](TrainerRank const& r) { return r.maxSkill == maxSkill; });
                        uint32 const level = f[3].Get<uint8>(), req = f[2].Get<uint32>();
                        if (same == ranks.end())
                            ranks.push_back({level, req, maxSkill, f[4].Get<uint32>()});
                        else if (level < same->reqLevel)
                            *same = {level, req, maxSkill, f[4].Get<uint32>()};
                    }
            }
            if (!f[1].Get<uint32>())
                continue;
            auto& req = trainerReq[spellId];
            uint32 const rank = f[2].Get<uint32>(), level = f[3].Get<uint8>();
            if (!req.first || rank < req.first)
                req = {rank, level};
            // [prof272] the recipe's fee: the lowest of its trainers
            if (auto const fee = trainerFee.find(spellId); fee == trainerFee.end() || f[4].Get<uint32>() < fee->second)
                trainerFee[spellId] = f[4].Get<uint32>();
        } while (result->NextRow());
    }

    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE maxcount = 0"))
    {
        do
            vendorItems_.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }
    // [lag329] where each unlimited vendor item is sold: the maps of its vendors' spawns
    // [lag335] ... and the spawns themselves (ReagentVendors)
    if (QueryResult result = WorldDatabase.Query("SELECT v.item, c.map, c.id, c.position_x, c.position_y, "
                                                 "c.position_z, v.ExtendedCost FROM npc_vendor v JOIN creature c ON "
                                                 "c.id = v.entry WHERE v.maxcount = 0"))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 const item = f[0].Get<uint32>();
            vendorMaps_[item].insert(f[1].Get<uint32>());
            reagentVendors_[item].push_back({f[2].Get<uint32>(), f[1].Get<uint32>(), f[3].Get<float>(),
                                             f[4].Get<float>(), f[5].Get<float>(), f[6].Get<uint32>()});
        } while (result->NextRow());
    }

    std::unordered_set<uint32> lines(std::begin(CRAFT_LINES), std::end(CRAFT_LINES));
    uint32 count = 0;
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
        if (!ability || (!lines.count(ability->SkillLine) && ability->SkillLine != SKILL_MINING))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell);
        if (!info)
            continue;
        Recipe recipe;
        recipe.spellId = ability->Spell;
        recipe.skillLine = ability->SkillLine;
        recipe.yellow = ability->TrivialSkillLineRankLow;
        recipe.gray = ability->TrivialSkillLineRankHigh;
        recipe.autoLearned = ability->AcquireMethod == 1;
        recipe.reqSkill = ability->MinSkillLineRank;
        auto const trainer = trainerReq.find(ability->Spell);
        if (trainer != trainerReq.end())
        {
            recipe.trainer = true;
            recipe.reqSkill = trainer->second.first;
            recipe.reqLevel = trainer->second.second;
            if (auto const fee = trainerFee.find(ability->Spell); fee != trainerFee.end())
                recipe.price = fee->second;
        }
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (info->Reagent[r] > 0 && info->ReagentCount[r] > 0)
                recipe.reagents.emplace_back(uint32(info->Reagent[r]), info->ReagentCount[r]);
        if (recipe.reagents.empty())
            continue;
        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)  // [guild70]
            if ((info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM ||
                 info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM_2) &&
                info->Effects[e].ItemType && !recipe.product)
            {
                recipe.product = info->Effects[e].ItemType;
                recipe.made = float(std::max(1, info->Effects[e].CalcValue()));
            }

        if (ability->SkillLine == SKILL_MINING)
        {
            // smelting: the bar stands for its ores in the wanted list (gatherers mine ore, not bars)
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                if (info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[e].ItemType)
                {
                    float const made = float(std::max(1, info->Effects[e].CalcValue()));
                    auto& from = smeltFrom_[info->Effects[e].ItemType];
                    if (from.empty())
                        for (auto const& [item, n] : recipe.reagents)
                            from.emplace_back(item, float(n) / made);
                }
            continue;
        }
        if (!recipe.gray)
            continue;
        // [guild70] a trainer / auto-learned recipe makes the intermediate (bolt, potion used as reagent…)
        if (recipe.product && (recipe.trainer || recipe.autoLearned) && !craftedFrom_.count(recipe.product))
        {
            auto& from = craftedFrom_[recipe.product];
            for (auto const& [item, n] : recipe.reagents)
                from.emplace_back(item, float(n) / recipe.made);
        }
        recipesBySkill_[ability->SkillLine].push_back(recipe);
        ++count;
    }
    // [guild70] two-way conversions (essences: lesser <-> greater) are no intermediates
    std::vector<uint32> cyclic;
    for (auto const& [product, from] : craftedFrom_)
        for (auto const& [item, n] : from)
        {
            auto const back = craftedFrom_.find(item);
            if (back != craftedFrom_.end() &&
                std::any_of(back->second.begin(), back->second.end(), [&](auto const& x) { return x.first == product; }))
                cyclic.push_back(product);
        }
    for (uint32 product : cyclic)
        craftedFrom_.erase(product);
    size_t rankCount = 0;
    for (auto& [line, ranks] : trainerRanks_)
    {
        std::sort(ranks.begin(), ranks.end(),
                  [](TrainerRank const& a, TrainerRank const& b) { return a.maxSkill < b.maxSkill; });
        rankCount += ranks.size();
    }
    LOG_INFO("playerbots", "[tasks] trainer ranks: {} skill lines, {} ranks (level, skill -> cap)",
             trainerRanks_.size(), rankCount);
    BuildSkillPlans();
    LOG_INFO("playerbots", "[guild30] wanted list: {} craft recipes, {} smelts, {} vendor items", count,
             smeltFrom_.size(), vendorItems_.size());
}

void GuildCoordinator::LoadGatherSources()
{
    gatherSource_.clear();
    // herb / ore nodes: chest gameobjects whose lock needs the gathering skill
    // [guild15] ... and whose spawns of the world yield at least one of it per round of them (spawns x drop chance
    // >= 1): b325 Tangy Clam Meat, 0.035 % of an Iron Coral, was a "gatherable" wanted item - 2600 of it called every
    // gatherer of level 10+ off its quests (GATHER orders above QUEST) to nodes that never give it
    if (QueryResult result = WorldDatabase.Query(
            "SELECT glt.Item, gt.Data0, CAST(MAX(IF(glt.Chance > 0, glt.Chance, 100.0 / g0.c) * s.n) AS DOUBLE) "
            "FROM gameobject_loot_template glt JOIN gameobject_template gt ON gt.type = 3 AND gt.Data1 = glt.Entry "
            "JOIN (SELECT id, COUNT(*) n FROM gameobject GROUP BY id) s ON s.id = gt.entry "
            "JOIN (SELECT Entry, GroupId, SUM(Chance = 0) c FROM gameobject_loot_template GROUP BY Entry, GroupId) g0 "
            "ON g0.Entry = glt.Entry AND g0.GroupId = glt.GroupId "
            "WHERE glt.Reference = 0 AND gt.Data0 <> 0 GROUP BY glt.Item, gt.Data0"))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 const item = f[0].Get<uint32>();
            if (f[2].Get<double>() < 100.0)  // chance in percent x spawns: below one per round of the spawns
                continue;
            LockEntry const* lock = sLockStore.LookupEntry(f[1].Get<uint32>());
            if (!lock)
                continue;
            for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            {
                if (lock->Type[i] != LOCK_KEY_SKILL)
                    continue;
                uint32 const skill = lock->Index[i] == LOCKTYPE_MINING      ? SKILL_MINING
                                     : lock->Index[i] == LOCKTYPE_HERBALISM ? SKILL_HERBALISM
                                                                            : 0;
                if (!skill)
                    continue;
                auto itr = gatherSource_.find(item);
                if (itr == gatherSource_.end() || lock->Skill[i] < itr->second.second)
                    gatherSource_[item] = {skill, lock->Skill[i]};
            }
        } while (result->NextRow());
    }
    LOG_INFO("playerbots", "[guild30] wanted list: {} items from herb/ore nodes", gatherSource_.size());
}

// ---------------------------------------------------------------------------------------------------------------------
// helpers (lock_ held where they touch the state)
// ---------------------------------------------------------------------------------------------------------------------
GuildCoordinator::PlanRow const* GuildCoordinator::Row(uint32 low) const
{
    auto const itr = planIndex_.find(low);
    return itr == planIndex_.end() ? nullptr : &plan_[itr->second];
}

std::string GuildCoordinator::Who(uint32 low) const
{
    PlanRow const* row = Row(low);
    Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
    std::string name = p ? p->GetName() : (row ? row->name : std::to_string(low));
    return "#" + std::to_string(row ? row->num : 0) + " " + name;
}

void GuildCoordinator::Decision(std::string text)
{
    LOG_INFO("playerbots", "[guild] decision: {}", text);
    decisions_.emplace_front(Now(), std::move(text));
    while (decisions_.size() > 10)
        decisions_.pop_back();
}

void GuildCoordinator::Alert(std::string text)
{
    LOG_INFO("playerbots", "[alert] {}", text);
    alerts_.emplace_front(Now(), std::move(text));
    while (alerts_.size() > 20)
        alerts_.pop_back();
}

std::string GuildCoordinator::RaidLeaderText(char grp) const
{
    uint32 const low = grp >= 'A' && grp <= 'C' ? raidLeader_[grp - 'A'] : 0;
    return low ? Who(low) : "-";
}

std::string GuildCoordinator::Chain(uint32 low) const
{
    PlanRow const* row = Row(low);
    std::string chain = "global";
    if (!row || row->grp < 'A' || row->grp > 'C')
        return chain;
    uint32 const leader = raidLeader_[row->grp - 'A'];
    if (leader == low)
        return chain + " (raid leader " + row->grp + ")";
    if (leader)
        chain += std::string(" → raid ") + row->grp + " " + Who(leader);
    auto const st = bots_.find(low);
    if (st != bots_.end() && st->second.fiveId)
    {
        auto const five = fives_.find(st->second.fiveId);
        if (five != fives_.end())
            chain += " → five " + std::to_string(five->first) +
                     (five->second.leaderLow == low ? " (leader)" : " " + Who(five->second.leaderLow));
    }
    return chain;
}

uint64 GuildCoordinator::XpTotal(Player* bot)
{
    uint64 xp = bot->GetUInt32Value(PLAYER_XP);
    for (uint8 level = 1; level < bot->GetLevel(); ++level)
        xp += sObjectMgr->GetXPForLevel(level);
    return xp;
}

void GuildCoordinator::XpUnits(Player* bot, uint8 fromLevel, uint32 fromXp, double& levels, double& kills)
{
    // [diag260] user 2026-11-11: XP per hour grows with the level by itself, no build compares on it. A gain counted in
    // levels (its XP over the level's XP to the next, player_xp_for_level) and [diag261] in same-level kills (its XP
    // over Acore::XP::BaseGain(level, level, content), the base XP of one kill of a mob of the bot's level - quest and
    // kill XP both scale with it, the ratio is about level-neutral). A level-up between two samples splits the gain
    levels = kills = 0.0;
    uint8 const toLevel = bot->GetLevel();
    uint32 const toXp = bot->GetUInt32Value(PLAYER_XP);
    if (!fromLevel || toLevel < fromLevel || (toLevel == fromLevel && toXp <= fromXp))
        return;
    ContentLevels const content = GetContentLevelsForMapAndZone(bot->GetMapId(), bot->GetZoneId());
    for (uint8 level = fromLevel; level <= toLevel; ++level)
    {
        uint32 const need = sObjectMgr->GetXPForLevel(level);
        uint32 const start = level == fromLevel ? fromXp : 0;
        uint32 const end = level == toLevel ? toXp : need;
        if (end <= start)
            continue;
        if (need)
            levels += double(end - start) / need;
        if (uint32 const kill = Acore::XP::BaseGain(level, level, content))
            kills += double(end - start) / kill;
    }
}

uint32 GuildCoordinator::QuestProgress(Player* bot)
{
    uint32 progress = uint32(bot->getRewardedQuests().size()) * 1000;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        progress += 1;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            progress += bot->GetQuestSlotCounter(slot, i);
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                progress += std::min<uint32>(bot->GetItemCount(quest->RequiredItemId[i], false),
                                             quest->RequiredItemCount[i]);
        progress += bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE ? 100 : 0;
    }
    return progress;
}

// lock_ held. [stat1] what the bot does now: its owner's label (UpdateHealth's, BotState::ownerLabel) over the New
// RPG status it keeps under a five / party / task (b461 Emdyl: 6400 s in two Wailing Caverns fives read as its quest)
std::string GuildCoordinator::StatusName(Player* bot, PlayerbotAI* botAI) const
{
    if (!botAI)
        return "human playing";
    auto const st = bots_.find(bot->GetGUID().GetCounter());
    std::string const owner = st != bots_.end() ? st->second.ownerLabel : "";
    if (!bot->IsAlive())
        return std::string(bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? "ghost" : "dead") +
               (owner.empty() ? "" : " / " + owner);
    std::string text = owner.empty() ? RpgName(uint32(botAI->rpgInfo.GetStatus())) : owner;
    if (bot->IsInCombat())
        text = "combat/" + text;
    if (!owner.empty())
        return text;
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
        text += " q" + std::to_string(quest->questId) + (quest->quest ? " " + quest->quest->GetTitle() : "");
    if (botAI->rpgInfo.vendorRunEntry)
        text += " vendor: " + botAI->rpgInfo.vendorRunReason;
    return text;
}

// lock_ held. [stat1] the owner that runs the bot now where its New RPG status would not say it: a five, a help /
// co-op party, a followed group (each with its time bucket), a guild task its last NextStep ran, a trip, a rest (the
// status label only); "" = its New RPG status says it
std::string GuildCoordinator::OwnerLabel(Player* bot, PlayerbotAI* botAI, BotState const& st,
                                         std::string& bucket) const
{
    bucket.clear();
    uint32 const low = bot->GetGUID().GetCounter();
    if (std::string const five = FiveLabel(st, low); !five.empty())
    {
        bucket = "DUNGEON";
        return five;
    }
    Group* group = bot->GetGroup();
    Player* leader = group ? ObjectAccessor::FindPlayer(group->GetLeaderGUID()) : nullptr;
    if (NewRpgCoop::InParty(low))
    {
        bucket = "HELP";
        for (GuildOrder const& e : st.external)
            if (e.type == GuildOrderType::COOP)
                return "PARTY " + e.details;
        return "PARTY help party" + (leader && leader != bot ? " of " + leader->GetName() : std::string());
    }
    if (leader && leader != bot && !bot->InBattleground())
    {
        bucket = "FOLLOW";
        return "FOLLOW " + leader->GetName() + (IsRealPlayer(leader) ? " (player)" : "");
    }
    if (bot->IsInFlight())
        return "TRAVEL flight";
    NewRpgInfo& info = botAI->rpgInfo;  // GetStatus() is not const
    if (bot->GetTransport() || info.trip.phase == NewRpgInfo::TripPhase::Wait ||
        info.trip.phase == NewRpgInfo::TripPhase::Ride || info.trip.phase == NewRpgInfo::TripPhase::Board)
        return "TRAVEL boat / zeppelin / tram";
    NewRpgStatus const status = info.GetStatus();
    bool const ownPlan = status == RPG_DO_QUEST || status == RPG_GO_GRIND || status == RPG_WANDER_RANDOM;
    if (!st.stepTask.empty() && !ownPlan)
        return "TASK " + st.stepTask + (st.run ? " (walks there)" : "");
    if (bot->getStandState() == UNIT_STAND_STATE_SIT && status != RPG_REST)
        return std::string("REST eats / drinks (") + RpgName(uint32(status)) + ")";
    return "";
}

// the legitimate waits a bot is in by its own state
std::string GuildCoordinator::WaitReason(Player* bot, PlayerbotAI* botAI, BotState const& state) const
{
    if (!state.waitReason.empty())
        return state.waitReason;
    if (botAI)  // [guild70] crafting at a forge / in town: standing is the work
        if (auto const* prof = std::get_if<NewRpgInfo::DoProfession>(&botAI->rpgInfo.data))
            if (prof->kind == NewRpgCraft::TASK_CRAFT && !prof->npcEntry)
                return "craft " + NewRpgCraft::RecipeName(prof->spellId);
    if (std::string const five = FiveWait(state); !five.empty())  // [guild60]
        return five;
    if (std::string const found = FoundWait(bot->GetGUID().GetCounter(), state); !found.empty())  // [guild80]
        return found;
    if (std::string const transfer = TransferWait(bot->GetGUID().GetCounter(), state); !transfer.empty())  // [guild81]
        return transfer;
    if (!botAI || IsRealPlayer(bot))
        return "human player";
    if (SelfBotControl::IsSuspended(bot))
        return "player controls";
    if (bot->IsInFlight())
        return "flight";
    if (bot->GetTransport())
        return "transport";
    if (bot->IsNonMeleeSpellCast(false))
        return "casting / crafting";
    if (bot->getStandState() == UNIT_STAND_STATE_SIT)
        return "resting (sits)";
    // [diag253] the New RPG holds a sick bot where it rose until the aura ends ([deaths14] "waits out Resurrection
    // Sickness", its RECOVER line): a game mechanic's end
    if (Aura const* sickness = bot->GetAura(SPELL_RESURRECTION_SICKNESS))
        return "waits out Resurrection Sickness (" +
               std::to_string(std::max(0, sickness->GetDuration()) / IN_MILLISECONDS) + " s left)";
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus const status = info.GetStatus();
    if (status == RPG_REST)
        return "resting";
    if (status == RPG_WANDER_NPC && info.vendorRunEntry)
        return "at vendor";
    if (info.groupWaiting)  // [stuck8]
        return "group leader waits for a member (its GROUP_WAIT line)";
    if (info.trip.phase == NewRpgInfo::TripPhase::Wait || info.trip.phase == NewRpgInfo::TripPhase::Ride ||
        info.trip.phase == NewRpgInfo::TripPhase::Board)
        return "waiting for boat / zeppelin / tram";
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
        if (quest->lastReachPOI)
            return "waiting for quest mob respawn q" + std::to_string(quest->questId);
    if (auto const* gather = std::get_if<NewRpgInfo::DoGather>(&info.data))
        if (gather->lastReach)
            return "at gather node";
    if (Group* group = bot->GetGroup())
        if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()))
            if (leader != bot && IsRealPlayer(leader))
                return "with the human group leader";
    return "";
}

// ---------------------------------------------------------------------------------------------------------------------
// world thread
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::Update(uint32 diff)
{
    if (!loaded_)
        return;

    DrainDeferredLoot();  // [lock23]

    uint32 const ms = getMSTime();
    ticks_.emplace_back(ms, diff);
    aiTicks_.emplace_back(ms, uint32(std::min<uint64>(aiTimeTotalUs_.exchange(0), UINT32_MAX)));
    while (!ticks_.empty() && getMSTimeDiff(ticks_.front().first, ms) > MINUTE * IN_MILLISECONDS)
        ticks_.pop_front();
    while (!aiTicks_.empty() && getMSTimeDiff(aiTicks_.front().first, ms) > MINUTE * IN_MILLISECONDS)
        aiTicks_.pop_front();

    PMON_SCOPE(coordScope, PERF_MON_TOTAL, "guild coordinator");  // [guild15] world-thread cost, pmon
    // [perf211] each world-thread step names itself when one run of it is slow (b436 / b437: ticks 250-650 ms over
    // the bots' AI, only "help update" named)
    {
        PMON_SCOPE(lootScope, PERF_MON_VALUE, "guild deaths + loot");
        SlowScope slow("coordinator deaths + loot");
        ProcessDeaths();
        ProcessLoot();
    }

    wantedTimer_ += diff;
    if (wantedTimer_ >= 15 * IN_MILLISECONDS)
    {
        wantedTimer_ = 0;
        PMON_SCOPE(wantedScope, PERF_MON_VALUE, "guild wanted list");
        {
            SlowScope slow("coordinator wanted list");
            UpdateWanted();
        }
        SlowScope slow("coordinator professions");
        UpdateProfessions();  // [tasks] profession vs questing: the global leader's call per member
        UpdateEnchantOrders();  // [craft386] the enchanters' orders on the members' gear
    }
    updateTimer_ += diff;
    if (updateTimer_ >= 5 * IN_MILLISECONDS)
    {
        updateTimer_ = 0;
        {
            SlowScope slow("coordinator leaders");
            UpdateLeaders();
        }
        {
            PMON_SCOPE(gearScope, PERF_MON_VALUE, "guild gear plan");
            SlowScope slow("coordinator gear plan");
            UpdateGear();  // [guild50]
        }
        {
            SlowScope slow("coordinator founding");
            UpdateFounding();  // [guild80] the bots' own guild: plan + charter / signatures / invites / bank tab
        }
        {
            SlowScope slow("coordinator riding");
            GuildRiding::Update();  // [mount1] one member's riding plan, the mount items to use, the guild line
        }
        {
            PMON_SCOPE(transferScope, PERF_MON_VALUE, "guild transfers plan");
            SlowScope slow("coordinator transfers plan");
            UpdateTransfers();  // [guild81] trade / mail / bank between members, gold for the bank tab
        }
        {
            PMON_SCOPE(ordersScope, PERF_MON_VALUE, "guild orders");
            SlowScope slow("coordinator orders");
            UpdateOrders();
        }
        {
            SlowScope slow("coordinator health");
            UpdateHealth();
        }
        {
            SlowScope slow("coordinator fives");
            UpdateFives();  // [guild60]
        }
        {
            SlowScope slow("coordinator assist");
            UpdateAssist();  // [assist1] the guild's blockers, the weights the help pricing reads, the escorts
        }
        NewRpgCoop::UpdateHelp();  // [help] the guild's help requests: helpers assigned, human parties ended
    }
    SlowScope slow("coordinator run transfers");
    RunTransfers(diff);  // [guild81] trades next to each other, mails at mailboxes (once a second)
    RunEnchantOrders(diff);  // [craft386] an enchanter and a member side by side: the trade's enchant
}

void GuildCoordinator::UpdateLeaders()
{
    CoordLock guard(lock_, __func__);
    for (uint8 g = 0; g < 3; ++g)
    {
        char const grp = char('A' + g);
        uint32& current = raidLeader_[g];
        auto const usable = [&](uint32 low) -> Player*
        {
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
            if (!p || !p->IsInWorld() || !GET_PLAYERBOT_AI(p) || IsRealPlayer(p) || IsSelfBot(p))
                return nullptr;
            return p;
        };
        if (current && usable(current))
            continue;
        uint32 best = 0;
        Player* bestPlayer = nullptr;
        PlanRow const* bestRow = nullptr;
        for (PlanRow const& row : plan_)
        {
            if (row.grp != grp)
                continue;
            Player* p = usable(row.low);
            if (!p)
                continue;
            bool const tank = row.role == "tank";
            bool const bestTank = bestRow && bestRow->role == "tank";
            if (!bestRow || (tank && !bestTank) ||
                (tank == bestTank && (p->GetLevel() > bestPlayer->GetLevel() ||
                                      (p->GetLevel() == bestPlayer->GetLevel() && row.num < bestRow->num))))
            {
                best = row.low;
                bestPlayer = p;
                bestRow = &row;
            }
        }
        if (best != current)
        {
            std::string const why = current ? "the previous one is offline" : "first choice";
            current = best;
            if (best)
                Decision(std::string("raid ") + grp + ": leader " + Who(best) + " (" + bestRow->role + ", L" +
                         std::to_string(bestPlayer->GetLevel()) + ") — " +
                         (bestRow->role == "tank" ? "the tank of the highest level" : "no tanks, highest level") +
                         "; " + why);
        }
    }
    for (PlanRow const& row : plan_)
        if (row.grp >= 'A' && row.grp <= 'C')
            bots_[row.low].raidLeaderLow = raidLeader_[row.grp - 'A'];
}

// [guild70] per skill line and value: the trainer / auto-learned recipe whose skill-up costs the fewest gathered
// units (the plan every crafter follows; recipes learned elsewhere are compared per crafter)
void GuildCoordinator::BuildSkillPlans()
{
    skillPlan_.clear();
    extraRecipes_.clear();
    size_t steps = 0;
    for (auto const& [line, list] : recipesBySkill_)
    {
        uint32 top = 0;
        for (size_t i = 0; i < list.size(); ++i)
        {
            top = std::max(top, (list[i].yellow + list[i].gray) / 2);
            if (!list[i].trainer && !list[i].autoLearned)
                extraRecipes_[line].push_back(i);
        }
        std::vector<int32>& plan = skillPlan_[line];
        plan.assign(top + 1, -1);
        for (uint32 value = 1; value <= top; ++value)
        {
            double bestCost = 0, bestUnits = 0;
            for (size_t i = 0; i < list.size(); ++i)
            {
                Recipe const& r = list[i];
                double cost = 0, units = 0;
                int32 chance = 0;
                if ((!r.trainer && !r.autoLearned) || r.reqSkill > value ||
                    !SkillUpCost(r, value, vendorItems_, cost, units, chance))
                    continue;
                if (plan[value] < 0 || cost < bestCost || (cost == bestCost && units < bestUnits))
                {
                    plan[value] = int32(i);
                    bestCost = cost;
                    bestUnits = units;
                }
            }
            steps += plan[value] >= 0;
        }
    }
    LOG_INFO("playerbots", "[guild70] skill-up plans: {} skill lines, {} skill values with a skill-up recipe",
             skillPlan_.size(), steps);
}

void GuildCoordinator::SkillUpCrafts(Player* crafter, uint32 skillLine, uint32 skill, uint32 upTo,
                                     std::map<size_t, double>& crafts, std::map<size_t, uint32>* points) const
{
    auto const list = recipesBySkill_.find(skillLine);
    auto const plan = skillPlan_.find(skillLine);
    if (list == recipesBySkill_.end() || plan == skillPlan_.end())
        return;
    std::vector<size_t> known;  // recipes from drops / vendors this crafter has learned
    if (crafter)
        if (auto const extra = extraRecipes_.find(skillLine); extra != extraRecipes_.end())
            for (size_t i : extra->second)
                if (crafter->HasSpell(list->second[i].spellId))
                    known.push_back(i);
    uint32 const top = std::min<uint32>(upTo, uint32(plan->second.size()));
    for (uint32 value = std::max<uint32>(1, skill); value < top; ++value)
    {
        int32 best = plan->second[value];
        double bestCost = 0, bestUnits = 0;
        int32 bestChance = 0;
        if (best >= 0)
            SkillUpCost(list->second[size_t(best)], value, vendorItems_, bestCost, bestUnits, bestChance);
        for (size_t i : known)
        {
            double cost = 0, units = 0;
            int32 chance = 0;
            if (!SkillUpCost(list->second[i], value, vendorItems_, cost, units, chance))
                continue;
            if (best < 0 || cost < bestCost || (cost == bestCost && units < bestUnits))
            {
                best = int32(i);
                bestCost = cost;
                bestUnits = units;
                bestChance = chance;
            }
        }
        if (best >= 0 && bestChance > 0)
        {
            crafts[size_t(best)] += 1000.0 / bestChance;  // a gap: that point comes from a green recipe
            if (points)
                ++(*points)[size_t(best)];
        }
    }
}

// [lag329] a vendor item is a source for a crafter only when a vendor of it stands on the crafter's map (a buy on
// another continent is no plan of its: "the vendor is on another map ... no route of its own goes there"). b440:
// Ivory Ink is sold without limit only by Jessica Sellers in Dalaran (map 571): every scribe's Inscription walk from 1
// counted its ink as bought there at 0c, so no herb was wanted, gathered or milled for it ("gather 0.0")
std::vector<GuildCoordinator::ReagentVendor> const* GuildCoordinator::ReagentVendors(uint32 itemId) const
{
    auto const itr = reagentVendors_.find(itemId);
    return itr != reagentVendors_.end() ? &itr->second : nullptr;
}

bool GuildCoordinator::VendorSource(uint32 item, uint32 mapId) const
{
    if (!vendorItems_.count(item))
        return false;
    if (mapId == ANY_MAP)
        return true;
    // [insc146] a bot inside a dungeon stands on the instance's map, where no vendor is: its continent is the map of
    // the instance's entrance (MapEntry entrance_map). b442: Dekorhan in RFC (map 389) - "Light Parchment: neither
    // held nor gathered in the guild", the Inscription call dropped with its gold letter and parchment buy
    for (uint8 hop = 0; hop < 2; ++hop)
    {
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        if (!entry || !entry->Instanceable() || entry->entrance_map < 0)
            break;
        static std::mutex seenLock;
        static std::set<uint32> seen;
        {
            std::lock_guard<std::mutex> guard(seenLock);
            if (seen.insert(mapId).second)
                LOG_INFO("playerbots", "[insc146] vendor reagents seen from instance map {} are bought on map {}, "
                         "its entrance's continent", mapId, entry->entrance_map);
        }
        mapId = uint32(entry->entrance_map);
    }
    // [insc147] a map that is neither a continent nor an instance with an entrance (the Deeprun Tram, map 369) has no
    // continent of its own: the vendors of any continent count, as before [lag329] (b443: Rollothan in the tram -
    // "Light Parchment: neither held nor gathered in the guild")
    if (MapEntry const* entry = sMapStore.LookupEntry(mapId); entry && !entry->IsContinent())
        return true;
    auto const maps = vendorMaps_.find(item);
    return maps == vendorMaps_.end() || maps->second.count(mapId);  // no spawn row: as before
}

void GuildCoordinator::ExpandReagent(uint32 item, double count, std::map<uint32, double>& out,
                                     std::map<uint32, std::map<uint32, float>>* via, float perUnit, uint32 depth,
                                     uint32 top, uint32 mapId) const
{
    if (depth < 3 && !VendorSource(item, mapId) && !gatherSource_.count(item) && !skinned_.count(item))
    {
        std::vector<std::pair<uint32, float>> const* parts = nullptr;
        if (auto const smelt = smeltFrom_.find(item); smelt != smeltFrom_.end())
            parts = &smelt->second;
        else if (auto const made = craftedFrom_.find(item); made != craftedFrom_.end())
            parts = &made->second;
        else if (auto const* converted = sGuildGatherPlan.ConvertedFrom(item))  // [gather] milling / prospecting
            parts = converted;
        if (parts)
        {
            for (auto const& [raw, per] : *parts)
                ExpandReagent(raw, count * per, out, via, perUnit * per, depth + 1, depth ? top : item, mapId);
            return;
        }
    }
    out[item] += count;
    if (via && depth)
        (*via)[item][top] = perUnit;
}

// [guild70] The wanted list. REQUIRED items drive the orders (GATHER, the shortfall sort): the raw reagents of every
// planned crafter's skill-up recipes from its skill up to what its level reaches now ([gather] the trainer rank cap;
// learned or not yet, offline at its last seen skill and cap), the reagents of upgrade crafts and bags, the members'
// quest items. Intermediates (bars, bolts, potions used as reagents) stand for their raw materials and count as them
// when held; they are OPTIONAL themselves: banked when someone holds them, never ordered.
void GuildCoordinator::UpdateWanted()
{
    enum : uint8
    {
        WHY_SKILL,
        WHY_QUEST,
        WHY_UPGRADE,
        WHY_BAG
    };
    struct Part
    {
        uint8 kind{0};  // WHY_*
        std::set<uint32> who;
        double amount{0};
        std::map<uint32, double> products;  // skill-ups: the recipe's product -> amount of this item for it
    };
    std::map<uint32, double> need;                      // raw item -> amount
    std::map<uint32, std::map<uint32, double>> crafterNeed;  // [econ1] member -> raw item -> its skill-up need
    std::map<uint32, std::map<std::string, Part>> why;  // raw item -> reason -> part
    std::map<uint32, std::map<uint32, float>> via;      // raw item -> intermediate -> raw per intermediate
    std::map<uint32, double> worth;                     // [gather] raw item -> copper it is worth to the guild
    std::unordered_map<uint64, float> pointValue;
    std::unordered_map<uint32, float> copperPerXp;  // member -> the core's quest XP -> money rate (RecordPace)
    std::set<std::pair<uint32, uint32>> called;     // [econ2] (member, line) the leader calls to craft / gather / buy
    std::unordered_map<uint64, uint32> walkTarget;  // [craft430] (low << 16 | line) -> its walk's judged skill
    std::vector<LinePriority> priorities;           // [craft436]
    {
        CoordLock guard(lock_, __func__);
        pointValue = pointValue_;
        priorities = linePriorities_;
        for (auto const& [low, picks] : professions_)
            for (ProfessionPick const& pick : picks)
                if (pick.kind != GuildProfessionKind::NONE && pick.kind != GuildProfessionKind::TRAIN)
                    called.insert({low, pick.skillLine});
        for (auto const& [low, st] : bots_)
        {
            copperPerXp[low] = st.copperPerXp;
            for (auto const& [line, target] : st.walkTarget)
                walkTarget[(uint64(low) << 16) | line] = target;
        }
    }
    // a value shared over the raw materials by their units
    auto const shareWorth = [&](std::map<uint32, double> const& raw, double value)
    {
        double units = 0;
        for (auto const& [item, amount] : raw)
            units += amount;
        if (units > 0 && value > 0)
            for (auto const& [item, amount] : raw)
                worth[item] += value * amount / units;
    };
    std::vector<Player*> members;
    Guild* guild = nullptr;
    std::unordered_set<uint32> const craftLines(std::begin(CRAFT_LINES), std::end(CRAFT_LINES));
    auto const addPart = [&](uint32 item, double amount, std::string const& label, uint8 kind, uint32 whoLow,
                             uint32 product)
    {
        need[item] += amount;
        Part& part = why[item][label];
        part.kind = kind;
        if (whoLow)
            part.who.insert(whoLow);
        part.amount += amount;
        if (product)
            part.products[product] += amount;
    };

    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        bool const online = p && p->IsInWorld();
        if (online)
        {
            members.push_back(p);
            if (!guild)
                guild = GuildBankRun::BotGuild(p);
        }

        std::vector<uint32> lines;
        auto const addLine = [&](uint32 line)
        {
            if (craftLines.count(line) && std::find(lines.begin(), lines.end(), line) == lines.end())
                lines.push_back(line);
        };
        for (uint32 line : row.profs)
            addLine(line);
        for (uint32 line : row.secondary)
            addLine(line);
        if (online)
            for (uint32 line : CRAFT_LINES)
                if (p->HasSkill(line))
                    addLine(line);
        for (uint32 line : lines)
        {
            auto const list = recipesBySkill_.find(line);
            if (list == recipesBySkill_.end())
                continue;
            // [econ2] b353 wanted Linen Cloth 9392 (have 1377): the skill-ups of every member's every line to its
            // rank cap, First Aid of 72 members first - the cloth was held "until the guild bank" in full bags and
            // mailed one by one for skill-ups nobody was called to make. A skill-up is wanted only on a line the
            // leader calls this member to (its value beat questing: UpdateProfessions); the upgrade / bag crafts and
            // the quests below stay wanted
            if (!called.count({row.low, line}))
                continue;
            uint64 const key = (uint64(row.low) << 16) | line;
            uint32 skill = 0;
            // [gather] b337: the skill-ups were summed over the whole range to 450 for every member (Linen Cloth
            // 10423: First Aid of 75 members x 124), far past what anyone can learn now. The need runs up to the
            // skill its level reaches now: the highest trainer rank of its level (TrainerCap), or its rank's cap
            uint32 cap = 0;
            if (online)
            {
                skill = p->HasSkill(line) ? p->GetPureSkillValue(line) : 0;
                // [econ1] b350 Linen Cloth need 9782 / have 1485, the reagents filled the bags: once the trainer
                // ranks load (TrainerCap > 0) the need ran to the rank its level allows, ranks it has not learned.
                // The need runs to the cap of the rank it holds now (Player max skill); a planned line it has not
                // learned wants nothing yet (its TRAIN call comes first)
                cap = p->HasSkill(line) ? p->GetPureMaxSkillValue(line) : 0;
                // [craft430] ... or on to the skill its called walk is judged at (the trainer ranks its level allows
                // on the way): b463 Orenem Tailoring 25/75 was called to 150, Wool Cloth (from 75) stayed unwanted and
                // 11 stacks of it were sold
                if (auto const target = walkTarget.find(key); cap && target != walkTarget.end())
                    cap = std::max(cap, std::min(target->second, TrainerCap(line, p->GetLevel())));
                lastSkill_[key] = skill;
                lastCap_[key] = cap;
            }
            else
            {
                if (auto const seen = lastSkill_.find(key); seen != lastSkill_.end())
                    skill = seen->second;
                if (auto const seen = lastCap_.find(key); seen != lastCap_.end())
                    cap = seen->second;
            }
            std::map<size_t, double> crafts;
            std::map<size_t, uint32> points;
            SkillUpCrafts(online ? p : nullptr, line, skill, cap, crafts, &points);
            auto const pv = pointValue.find(key);
            for (auto const& [index, expected] : crafts)
            {
                Recipe const& r = list->second[index];
                double const n = std::ceil(expected - 0.001);
                std::map<uint32, double> raw;
                for (auto const& [item, count] : r.reagents)  // [lag329] its vendors: on its map
                    ExpandReagent(item, n * count, raw, &via, 1.0f, 0, 0, online ? p->GetMapId() : ANY_MAP);
                for (auto const& [item, amount] : raw)
                {
                    addPart(item, amount, SkillPlural(line), WHY_SKILL, row.low, r.product);
                    if (online)
                        crafterNeed[row.low][item] += amount;  // [econ1] its own skill-ups to its rank cap
                }
                // [gather] the skill points this recipe gives, at the crafter's point value
                if (pv != pointValue.end())
                    shareWorth(raw, double(points[index]) * pv->second);
            }
        }
        if (!online)
            continue;
        // items of the members' active quests that can change hands (no quest-bound items)
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = p->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest || p->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                uint32 const item = quest->RequiredItemId[i];
                ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item) : nullptr;
                if (!proto || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_WHEN_PICKED_UP ||
                    proto->Class == ITEM_CLASS_QUEST)
                    continue;
                uint32 const have = p->GetItemCount(item, false);
                if (have >= quest->RequiredItemCount[i])
                    continue;
                addPart(item, quest->RequiredItemCount[i], "quest q" + std::to_string(questId), WHY_QUEST, row.low, 0);
                // [gather] the quest's value (the core's quest XP at the member's copper per XP + its money reward),
                // shared over the units of its item objectives
                uint32 units = 0;
                for (uint8 j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
                    units += quest->RequiredItemId[j] ? quest->RequiredItemCount[j] : 0;
                auto const rate = copperPerXp.find(row.low);
                double const value = double(quest->XPValue(p->GetLevel())) * sWorld->getRate(RATE_XP_QUEST) *
                                         (rate != copperPerXp.end() ? rate->second : 0.0f) +
                                     double(std::max<int32>(0, quest->GetRewOrReqMoney(p->GetLevel())));
                if (units)
                    worth[item] += value * quest->RequiredItemCount[i] / units;
            }
        }
    }

    // [guild50] the reagents of the craft queue (upgrade crafts, bags), intermediates as their raw materials
    {
        std::map<uint32, std::map<std::string, GearNeed>> gear;
        {
            CoordLock guard(lock_, __func__);
            gear = gearWanted_;
        }
        for (auto const& [item, reasons] : gear)
            for (auto const& [label, g] : reasons)
            {
                std::map<uint32, double> raw;
                ExpandReagent(item, g.count, raw, &via);
                shareWorth(raw, g.value);  // [gather] the crafted item's price, this reagent's part of it
                bool const bag = label.rfind("bag", 0) == 0;
                for (auto const& [id, amount] : raw)
                    for (uint32 forLow : g.forLows)
                        addPart(id, amount / std::max<size_t>(1, g.forLows.size()), label,
                                bag ? WHY_BAG : WHY_UPGRADE, forLow, 0);
            }
    }

    // [supply112] the guild cook's supply jobs: their reagents are wanted for it (the fishers' catch goes to it), worth
    // what the cooked units save against the vendor
    {
        struct SupplyPart
        {
            uint32 cook;
            uint32 item;
            double amount;
            double worth;
            std::string label;
        };
        std::vector<SupplyPart> parts;
        {
            CoordLock guard(lock_, __func__);
            // [eco194] ... and the alchemists' potion jobs: their herbs are wanted for them
            for (auto const* crafts : {&supplyCrafts_, &potionCrafts_})
                for (auto const& [cookLow, jobs] : *crafts)
                    if (Player* c = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(cookLow)))
                        for (SupplyCraft const& job : jobs)
                        {
                            uint32 const owed = c->HasSpell(job.spellId) ? SupplyOwed(c, job) : 0;
                            uint32 units = 0;
                            for (auto const& [item, n] : job.reagents)
                                units += n;
                            for (auto const& [item, n] : job.reagents)
                                if (owed && units)
                                    parts.push_back({cookLow, item, double(owed) * n,
                                                     double(owed) * job.made * job.savedPerUnit * n / units,
                                                     "supply " + ItemName(job.product) + " (band " + job.band + ")"});
                        }
        }
        for (SupplyPart const& part : parts)
        {
            std::map<uint32, double> raw;
            ExpandReagent(part.item, part.amount, raw, &via);
            shareWorth(raw, part.worth);
            for (auto const& [id, amount] : raw)
            {
                addPart(id, amount, part.label, WHY_UPGRADE, part.cook, 0);
                crafterNeed[part.cook][id] += amount;
            }
        }
    }

    // [craft436] a guild priority line ([craft435]: its gap pays more per bot-minute than questing): the raw materials
    // of its whole chain are wanted, each unit worth the gap's value over the chain's gathered units - the worth the
    // gathering plan weighs a gatherer's pace by. Its orders' and skill-ups' own worth above stays when higher (the
    // larger of the two, never their sum: one gap valued once)
    for (LinePriority const& r : priorities)
    {
        double units = 0.0;
        for (auto const& [item, amount] : r.raw)
            if (!VendorSource(item, ANY_MAP))
                units += amount;
        if (units <= 0.0)
            continue;
        double const perUnit = r.value / units;
        for (auto const& [item, amount] : r.raw)
        {
            if (VendorSource(item, ANY_MAP))
                continue;
            if (need[item] < amount)
                addPart(item, amount - need[item], std::string("guild priority ") + SkillShort(r.line) + " [craft436]",
                        WHY_UPGRADE, 0, 0);
            worth[item] = std::max(worth[item], perUnit * need[item]);
        }
    }

    std::map<uint32, uint32> bank;
    GuildBankRun::CountBank(guild, bank);
    // [econ1] what lies in the members' mailboxes counts as stock too (it is theirs, taken on the next visit)
    std::map<uint32, uint32> inMail;
    for (Player* m : members)
        for (Mail const* mail : m->GetMails())
            if (mail && mail->state != MAIL_STATE_DELETED)
                for (MailItemInfo const& info : mail->items)
                    if (Item* it = m->GetMItem(info.item_guid))
                        inMail[it->GetEntry()] += it->GetCount();
    auto const held = [&](uint32 item)
    {
        uint32 n = bank[item] + inMail[item];
        for (Player* m : members)
            n += m->GetItemCount(item, false);
        return n;
    };
    auto const num = [&](uint32 low)
    {
        PlanRow const* r = Row(low);
        return "#" + std::to_string(r ? r->num : 0);
    };
    auto const whyText = [&](std::map<std::string, Part> const& parts)
    {
        std::vector<std::pair<std::string, Part const*>> sorted;
        for (auto const& [label, part] : parts)
            sorted.emplace_back(label, &part);
        std::sort(sorted.begin(), sorted.end(),
                  [](auto const& a, auto const& b) { return a.second->amount > b.second->amount; });
        std::string text;
        for (auto const& [label, part] : sorted)
        {
            uint32 const amount = uint32(std::ceil(part->amount - 0.001));
            std::string piece;
            if (part->kind == WHY_SKILL)
            {
                std::vector<std::pair<double, uint32>> products;
                for (auto const& [product, a] : part->products)
                    products.emplace_back(a, product);
                std::sort(products.rbegin(), products.rend());
                piece = "reagent: ";
                for (size_t i = 0; i < products.size() && i < 2; ++i)
                    piece += (i ? ", " : "") + ItemName(products[i].second);
                if (products.size() > 2)
                    piece += " etc.";
                size_t const crafters = std::max<size_t>(1, part->who.size());
                piece += " (" + label + " " +
                         (crafters > 1 ? std::to_string(crafters) + "×≈" + std::to_string(amount / crafters)
                                       : num(*part->who.begin()) + " ×" + std::to_string(amount)) +
                         ")";
            }
            else if (part->kind == WHY_QUEST)
                piece = label + " for " + (part->who.empty() ? "?" : num(*part->who.begin())) + " ×" +
                        std::to_string(amount);
            else if (part->kind == WHY_BAG)
                piece = label + " for " + std::to_string(part->who.size()) + " bots ×" + std::to_string(amount);
            else
            {
                piece = label + " for";
                for (uint32 low : part->who)
                    piece += " " + num(low);
                piece += " ×" + std::to_string(amount);
            }
            if (text.size() + piece.size() > 200)
            {
                text += " + …";
                break;
            }
            text += (text.empty() ? "" : " + ") + piece;
        }
        return text;
    };

    std::vector<WantedItem> list;
    for (auto const& [item, amount] : need)
    {
        WantedItem w;
        w.itemId = item;
        w.need = uint32(std::ceil(amount - 0.001));
        w.have = held(item);
        // intermediates in the bags and the bank count as the raw material they were made of
        if (auto const v = via.find(item); v != via.end())
            for (auto const& [inter, per] : v->second)
                w.have += uint32(float(held(inter)) * per);
        w.vendor = vendorItems_.count(item) != 0;
        auto const src = gatherSource_.find(item);
        if (src != gatherSource_.end())
        {
            w.gatherSkill = src->second.first;
            w.gatherReq = src->second.second;
        }
        w.forWhom = whyText(why[item]);
        w.unitValue = w.need ? float(worth[item] / w.need) : 0.0f;
        list.push_back(w);
    }
    // OPTIONAL: the intermediates themselves, kept in the bank when held (they stand for raw materials above)
    std::map<uint32, std::string> optional;
    for (auto const& [raw, inters] : via)
        for (auto const& [inter, per] : inters)
            if (!need.count(inter))
            {
                std::string& text = optional[inter];
                char ratio[16];
                std::snprintf(ratio, sizeof(ratio), "%.2f", per);
                if (text.size() < 120)
                    text += (text.empty() ? "intermediate, counted as " : ", ") + ItemName(raw) + " ×" + ratio;
            }
    for (auto const& [inter, text] : optional)
    {
        WantedItem w;
        w.itemId = inter;
        w.optional = true;
        w.have = held(inter);
        w.forWhom = text + " (not ordered)";
        list.push_back(w);
    }
    std::sort(list.begin(), list.end(), [](WantedItem const& a, WantedItem const& b)
              {
                  if (a.optional != b.optional)
                      return !a.optional;
                  return a.Short() != b.Short() ? a.Short() > b.Short() : a.itemId < b.itemId;
              });

    // [econ1] the top of the list: need vs have (bags + bank + mail), one line per item when it changes
    {
        std::string top;
        size_t shown = 0;
        for (WantedItem const& w : list)
        {
            if (w.optional || shown >= 10)
                break;
            top += (shown++ ? "; " : "") + ItemName(w.itemId) + " need " + std::to_string(w.need) + " have " +
                   std::to_string(w.have);
        }
        if (top != wantedTop_)
        {
            wantedTop_ = top;
            LOG_INFO("playerbots", "[econ1] wanted top 10 (need to the held rank caps, have = bags + bank + mail): {}",
                     top);
        }
    }
    // [econ1] each crafter's own skill-up need minus its bags: where the bag relief sends a full holder's reagents
    std::map<uint32, std::map<uint32, uint32>> crafterLack;
    for (auto const& [low, items] : crafterNeed)
    {
        Player* c = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        if (!c || !c->IsInWorld())
            continue;
        for (auto const& [item, amount] : items)
        {
            uint32 const n = uint32(std::ceil(amount - 0.001));
            uint32 const have = c->GetItemCount(item, false);
            if (n > have)
                crafterLack[low][item] = n - have;
        }
    }

    std::map<uint32, std::map<uint32, uint32>> crafterNeedUnits;
    for (auto const& [low, items] : crafterNeed)
        for (auto const& [item, amount] : items)
            crafterNeedUnits[low][item] = uint32(std::ceil(amount - 0.001));

    CoordLock guard(lock_, __func__);
    wanted_ = std::move(list);
    crafterLack_ = std::move(crafterLack);
    crafterNeed_ = std::move(crafterNeedUnits);  // [econ2]
    guildState_ = guild ? "guild «" + guild->GetName() + "», bank tabs " + std::to_string(guild->GetPurchasedTabsSize())
                        : "no guild: the bank sleeps";
}

bool GuildCoordinator::GlyphNeeded(uint32 itemId) const
{
    CoordLock guard(lock_, __func__);
    auto const need = glyphFor_.find(itemId);
    return need != glyphFor_.end() && !need->second.empty();
}

bool GuildCoordinator::IsWanted(uint32 itemId) const
{
    CoordLock guard(lock_, __func__);
    for (WantedItem const& w : wanted_)
        if (w.itemId == itemId)
            return w.Short() > 0 || w.optional;
    return false;
}

void GuildCoordinator::RequestWithdraw(Player* bot, uint32 itemId, std::string const& why)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    CoordLock guard(lock_, __func__);
    auto& list = withdraw_[bot->GetGUID().GetCounter()];
    for (auto const& [item, text] : list)
        if (item == itemId)
            return;
    list.emplace_back(itemId, why);
}

// [supply112] the nearest vendor spawn on the bot's map that sells the item for gold (npc_vendor, no extended cost)
static bool NearestVendorOf(Player* p, uint32 itemId, uint32& npcEntry, WorldPosition& pos)
{
    static std::once_flag once;
    static std::unordered_map<uint32, std::vector<std::pair<uint32, WorldPosition>>> spawns;  // item -> vendors
    std::call_once(once, []()
    {
        // [supply113] and the cook's own fire: Basic Campfire's reagents and tools
        std::unordered_set<uint32> fireItems;
        if (SpellInfo const* fire = sSpellMgr->GetSpellInfo(NewRpgCraft::SPELL_BASIC_CAMPFIRE))
        {
            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (fire->Reagent[i] > 0)
                    fireItems.insert(uint32(fire->Reagent[i]));
            for (uint32 totem : fire->Totem)
                if (totem)
                    fireItems.insert(totem);
            std::unordered_set<uint32> categories(fire->TotemCategory.begin(), fire->TotemCategory.end());
            categories.erase(0);
            for (auto const& [id, proto] : *sObjectMgr->GetItemTemplateStore())
                if (proto.TotemCategory && categories.count(proto.TotemCategory))
                    fireItems.insert(id);
        }
        std::unordered_map<uint32, std::vector<uint32>> byEntry;  // vendor entry -> its recipe items
        for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
            if (tmpl.npcflag & UNIT_NPC_FLAG_VENDOR)
                if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry))
                    for (VendorItem const* item : items->m_items)
                        if (ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
                            proto && (proto->Class == ITEM_CLASS_RECIPE || fireItems.count(item->item)) &&
                            !item->ExtendedCost)
                            byEntry[entry].push_back(item->item);
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            if (auto const found = byEntry.find(data.id); found != byEntry.end())
                for (uint32 item : found->second)
                    spawns[item].emplace_back(data.id,
                                              WorldPosition(data.mapid, data.posX, data.posY, data.posZ));
    });
    auto const list = spawns.find(itemId);
    if (list == spawns.end())
        return false;
    float best = FLT_MAX;
    for (auto const& [entry, at] : list->second)
        if (at.GetMapId() == p->GetMapId())
            if (float const d = p->GetExactDist(at); d < best)
            {
                best = d;
                npcEntry = entry;
                pos = at;
            }
    return best < FLT_MAX;
}

void GuildCoordinator::UpdateOrders()
{
    // [gather] the gathering plan first, without lock_ (Assign takes its own lock): the required gatherable shortfall
    // of the wanted list over the online gatherers (groups B, C); [questrate] gathering and questing both by formula
    {
        std::map<uint32, uint32> shortfall;
        std::map<uint32, std::pair<uint32, uint32>> stock;
        std::vector<Player*> gatherers;
        std::map<uint32, float> questPace;  // gatherer -> its questing copper per minute (the leader's comparison)
        std::map<uint32, float> unitValue;  // item -> copper a unit is worth to the guild (UpdateWanted)
        // [questrate] gatherer -> the questing formula's numbers (the plan's log line)
        std::map<uint32, std::string> questWhy;
        {
            CoordLock guard(lock_, __func__);
            for (WantedItem const& w : wanted_)
                if (!w.optional && w.Short() && sGuildGatherPlan.Gatherable(w.itemId))
                {
                    shortfall[w.itemId] = w.Short();
                    stock[w.itemId] = {w.need, w.have};
                    unitValue[w.itemId] = w.unitValue;
                }
            // [fish110] the fish the fives' cooked supplies need (UpdateProfessions): the guild's fishers bring it
            for (auto const& [item, n] : fishShort_)
                if (n && sGuildGatherPlan.Gatherable(item))
                {
                    shortfall[item] = std::max(shortfall[item], n);
                    stock.try_emplace(item, n, 0);
                }
            // [lag333] the guild's median level (its planned members online): a gatherer below it levels first
            uint8 medianLevel = 0;
            {
                std::vector<uint8> levels;
                for (PlanRow const& row : plan_)
                    if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                        p && p->IsInWorld() && !HumanCharacter(p))
                        levels.push_back(p->GetLevel());
                if (!levels.empty())
                {
                    std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
                    medianLevel = levels[levels.size() / 2];
                }
            }
            for (PlanRow const& row : plan_)
                if (row.grp == 'B' || row.grp == 'C' || fishers_.count(row.low))
                    if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                        p && p->IsInWorld() && !HumanCharacter(p))
                    {
                        gatherers.push_back(p);
                        // [lag333] coordinator decision 2026-11-11 ([lag325]'s rule for the gathering): a gatherer
                        // below the guild's median level gets no URGENT gathering over its quests - no quest pace for
                        // the plan to beat, so it gathers only on its way (a convenient task). b442 Sylaenas L13 (the
                        // lowest member, median 19) was sent URGENT to Teldrassil's Silverleaf for the scribes' inks
                        // ("gathering 60.1 c/min > quests 55.3 c/min")
                        if (p->GetLevel() < medianLevel)
                        {
                            questWhy[row.low] = Acore::StringFormat("L{} below the guild's median L{}: it levels first "
                                                                    "[lag333]",
                                                                    uint32(p->GetLevel()), uint32(medianLevel));
                            continue;
                        }
                        // [questrate] its questing by formula (QuestPace), not by measured history
                        float copper = 0.0f;
                        std::string why;
                        if (auto const st = bots_.find(row.low);
                            st != bots_.end() && QuestPace(st->second, p, copper, &why))
                        {
                            questPace[row.low] = copper;
                            questWhy[row.low] = why;
                        }
                    }
        }
        std::set<uint32> fishers;
        std::vector<GatherClimb> climbs;
        {
            CoordLock guard(lock_, __func__);
            fishers = fishers_;
            // [craft432] the tiers no member online reaches (one that does: its walk waits for a place or a safe
            // level, not for a climb)
            for (auto const& [tier, need] : climbNeeds_)
            {
                bool reached = false;
                for (PlanRow const& row : plan_)
                    if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                        p && p->IsInWorld() && p->HasSkill(tier.first) &&
                        p->GetPureSkillValue(tier.first) >= tier.second)
                    {
                        reached = true;
                        break;
                    }
                if (!reached)
                    climbs.push_back({tier.first, int32(tier.second), need.itemId, float(need.value), need.forWhat});
            }
        }
        sGuildGatherPlan.Assign(shortfall, gatherers, questPace, unitValue, questWhy, fishers, climbs);
        if (gatherStockCalls_++ % GATHER_STOCK_LOG_EVERY == 0)  // a log cadence, no decision
            sGuildGatherPlan.LogStock(stock);
    }
    CoordLock guard(lock_, __func__);
    uint32 const now = Now();
    // [guild50] expected items that appeared become held for their member; one that did not come (cast broken, no
    // reward) is dropped once the bot no longer casts
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        auto const st = bots_.find(row.low);
        if (!p || !p->IsInWorld() || st == bots_.end() || st->second.expect.empty())
            continue;
        std::vector<Expect> keep;
        for (Expect const& e : st->second.expect)
        {
            Item* item = nullptr;
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END && !item; ++slot)
                if (Item* i = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (i->GetEntry() == e.itemId)
                        item = i;
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END && !item; ++b)
                if (Bag* bag = p->GetBagByPos(b))
                    for (uint32 j = 0; j < bag->GetBagSize() && !item; ++j)
                        if (Item* i = bag->GetItemByPos(uint8(j)))
                            if (i->GetEntry() == e.itemId)
                                item = i;
            bool heldAlready = false;
            for (HeldItem const& h : st->second.held)
                heldAlready |= item && h.itemGuidLow == item->GetGUID().GetCounter();
            if (item && !heldAlready)
            {
                st->second.held.push_back({item->GetGUID().GetCounter(), e.itemId, item->GetCount(), e.forLow,
                                           e.purpose});
                Decision(Who(row.low) + ": " + ItemName(e.itemId) + " — " + e.purpose +
                         (GuildBankRun::DepositState(p, e.itemId, item->GetCount()).empty()
                              ? ", to the bank"
                              : ", holds until the guild bank"));
            }
            else if (!item && p->IsNonMeleeSpellCast(false))
                keep.push_back(e);
        }
        st->second.expect = std::move(keep);
    }
    UpdateCrafts(now);  // [guild50]
    ClaimBankItems();                                     // [bank409]
    RouteSpareGreens();                                   // [ench161]
    std::map<std::pair<uint32, uint32>, bool> withdrawn;  // (member, item) already on its withdraw list
    for (auto const& [low, list] : withdraw_)
        for (auto const& [item, why] : list)
            withdrawn[{low, item}] = true;
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        BotState& st = bots_[row.low];
        if (!p || !p->IsInWorld())
            continue;
        // [guild50] an upgrade lying in the guild bank: the member takes it out itself
        if (!HumanCharacter(p))
            for (GearPick const& g : st.gearNow)
                if (g.kind == GuildGear::SRC_BANK && !p->GetItemCount(g.itemId, true) &&
                    !withdrawn.count({row.low, g.itemId}))
                {
                    withdraw_[row.low].emplace_back(
                        g.itemId, std::string("upgrade ") + GuildGear::GroupName(g.group) + " " + GainText(g));
                    withdrawn[{row.low, g.itemId}] = true;
                }
        // [guild50] old bags: once all four bag slots hold bags, a spare bag no bigger than the smallest worn one is
        // sold at the next vendor
        if (!HumanCharacter(p))
        {
            uint32 const smallest = SmallestBag(p);
            std::vector<Item*> spare;
            if (smallest)
                SpareBags(p, spare);
            auto const online = [](uint32 low) -> Player*
            {
                Player* m = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                return m && m->IsInWorld() ? m : nullptr;
            };
            // [inv376] the bags a member will wear anyway: its own spare ones and those held for it by others
            auto const sparesOf = [&](Player* m, uint32 skipGuid)
            {
                std::vector<uint32> sizes;
                std::vector<Item*> own;
                SpareBags(m, own);
                for (Item* b : own)
                    sizes.push_back(b->GetTemplate()->ContainerSlots);
                uint32 const mLow = m->GetGUID().GetCounter();
                for (auto const& [holder, hs] : bots_)
                    for (HeldItem const& h : hs.held)
                        if (h.forLow == mLow && h.itemGuidLow != skipGuid)
                            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(h.itemId);
                                GenericBagProto(proto))
                                sizes.push_back(proto->ContainerSlots);
                return sizes;
            };
            for (Item* bag : spare)
            {
                uint32 const guidLow = bag->GetGUID().GetCounter();
                uint32 const slots = bag->GetTemplate()->ContainerSlots;
                auto heldIt = std::find_if(st.held.begin(), st.held.end(),
                                           [&](HeldItem const& h) { return h.itemGuidLow == guidLow; });
                bool held = heldIt != st.held.end();
                Bag* asBag = bag->ToBag();
                if (slots > smallest || (asBag && !asBag->IsEmpty()))
                    continue;
                // [inv376] a bag freed by a bigger one (EquipAction::EquipBag) or one it never wore: to the member it
                // gains most bag slots (an empty bag slot, a smaller generic bag), by the transfer flow (trade /
                // mail); sold only when nobody gains from it. Its holder's earlier pick is dropped once that member
                // gains nothing from it any more
                if (held && heldIt->purpose.rfind("upgrade bag", 0) == 0)
                {
                    Player* to = online(heldIt->forLow);
                    if (to && BagGain(to, slots, sparesOf(to, guidLow)))
                        continue;
                    LOG_INFO("playerbots", "[inv376] {}: {} ({} slots) no longer gains {} a bag slot - its pick "
                             "is dropped", Who(row.low), ItemName(bag->GetEntry()), slots, Who(heldIt->forLow));
                    st.held.erase(heldIt);
                    held = false;
                }
                if (held)
                    continue;
                if (!bag->IsSoulBound())
                {
                    uint32 bestLow = 0, bestGain = 0;
                    float bestDist = 0.0f;
                    for (PlanRow const& other : plan_)
                    {
                        Player* m = other.low == row.low ? nullptr : online(other.low);
                        if (!m || !GET_PLAYERBOT_AI(m) || HumanCharacter(m))
                            continue;
                        uint32 const gain = BagGain(m, slots, sparesOf(m, guidLow));
                        float const dist = m->GetMapId() == p->GetMapId() ? p->GetExactDist(m)
                                                                         : std::numeric_limits<float>::max();
                        if (gain > bestGain || (gain && gain == bestGain && dist < bestDist))
                        {
                            bestLow = other.low;
                            bestGain = gain;
                            bestDist = dist;
                        }
                    }
                    if (bestLow)
                    {
                        st.sell.erase(std::remove(st.sell.begin(), st.sell.end(), guidLow), st.sell.end());
                        st.held.push_back({guidLow, bag->GetEntry(), 1, bestLow, "upgrade bag " + Who(bestLow)});
                        Decision(Who(row.low) + ": spare bag " + ItemName(bag->GetEntry()) + " → " + Who(bestLow) +
                                 " (+" + std::to_string(bestGain) + " bag slots)");
                        LOG_INFO("playerbots", "[inv376] {}: spare bag {} ({} slots) goes to {}: +{} bag slots there",
                                 Who(row.low), ItemName(bag->GetEntry()), slots, Who(bestLow), bestGain);
                        continue;
                    }
                }
                if (std::find(st.sell.begin(), st.sell.end(), guidLow) != st.sell.end())
                    continue;
                st.sell.push_back(guidLow);
                LOG_INFO("playerbots", "[inv376] {}: spare bag {} ({} slots) is sold: {}", Who(row.low),
                         ItemName(bag->GetEntry()), slots,
                         bag->IsSoulBound() ? "soulbound" : "no member gains a bag slot from it");
                Decision(Who(row.low) + ": old bag " + ItemName(bag->GetEntry()) + " → sell (" +
                         std::to_string(bag->GetTemplate()->ContainerSlots) + " slots, smallest worn " +
                         std::to_string(smallest) + ")");
            }
        }

        // items that left the bags (used, sold, traded, deposited) are not held any more
        auto const gone = [p](uint32 guidLow)
        { return !p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guidLow)); };
        st.held.erase(std::remove_if(st.held.begin(), st.held.end(),
                                     [&](HeldItem const& h) { return gone(h.itemGuidLow); }),
                      st.held.end());
        st.sell.erase(std::remove_if(st.sell.begin(), st.sell.end(), gone), st.sell.end());
        st.disenchant.erase(std::remove_if(st.disenchant.begin(), st.disenchant.end(), gone), st.disenchant.end());

        // [questvalue] a level-up or a login: the green / grey quests of its log are valued again
        // [quest120] b475 Eraelor L24 held 13 quests, 3 of them 7-17 levels below it and 4 untouched for days: a
        // log valued only on a level-up keeps every quest taken since (5321, taken 30 min after the last valuation,
        // was never priced). The log's own print joins the key, so an accept, a turn-in or a completion values it
        uint64 print = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (uint32 const logged = p->GetQuestSlotQuestId(slot))
                print += uint64(logged) * 31 + uint64(p->GetQuestStatus(logged));
        if (uint64 const key = uint64(p->GetLevel()) ^ (uint64(p->m_logintime) * 1000003ULL) ^ (print * 1000033ULL);
            key != st.revalueKey)
        {
            st.revalueKey = key;
            RevalueQuests(st, p);
        }
        std::vector<GuildOrder> orders;
        std::vector<GuildOrder> trainCalls;  // [prof272] the runnable TRAIN calls, put after the others
        FiveOrder(st, row.low, p, orders);  // [guild60] a five's dungeon comes first
        // [regress10] REPAIR: the leader tracks every member's durability (b345 Ginde L5 fought on with both
        // weapons at 0: the repair was only a side effect of a vendor run for bags or ammo, dropped by other states).
        // URGENT for a weapon broken or at one death's loss and for a broken armour piece, CONVENIENT for other armour
        // at one death's loss (in the repairer's town); its money against the core's cost, the shortfall sent by a
        // member (GuildTransfer)
        RepairNeed const repairNeed = GET_PLAYERBOT_AI(p) && !HumanCharacter(p) ? RepairNeedOf(p) : RepairNeed();
        if (repairNeed.items != st.durabilityKey)
        {
            st.durabilityKey = repairNeed.items;
            LOG_INFO("playerbots", "[gear] {} L{} durability: {}{}", Who(row.low), p->GetLevel(),
                     repairNeed.any ? repairNeed.items : std::string("nothing at one death's loss or broken"),
                     repairNeed.any ? Acore::StringFormat("; repair {} ({} urgent), money {}",
                                                          MoneyText(repairNeed.cost), MoneyText(repairNeed.urgentCost),
                                                          MoneyText(p->GetMoney()))
                                    : std::string());
        }
        // [gold41] user 2026-11-09: essential supplies it cannot pay - out of food (out of drink, using mana; a hunter
        // under its low-ammo line): a stack of the best of its level at the vendor price, less its money, asked from
        // the guild (the transfers plan picks the member whose spare money covers it, the richest first)
        st.supplyGoldShort = 0;
        st.supplyWhat.clear();
        if (p->IsAlive() && GET_PLAYERBOT_AI(p) && !HumanCharacter(p))
        {
            // the vendor price of `count` of the best item its level uses (the highest RequiredLevel at or below its
            // level, the cheapest of those): food / drink by their spell category and real Food / Drink aura, ammo by
            // its projectile subclass
            // the price per item kind and level, looked up once (the item store is static)
            static std::mutex priceLock;
            static std::map<std::pair<uint32, uint32>, float> priceCache;  // (kind, level) -> unit copper
            auto const cachedPrice = [p](uint32 kind, auto const& compute) -> float
            {
                std::pair<uint32, uint32> const key{kind, p->GetLevel()};
                {
                    std::lock_guard<std::mutex> guard(priceLock);
                    if (auto const found = priceCache.find(key); found != priceCache.end())
                        return found->second;
                }
                float const price = compute();
                std::lock_guard<std::mutex> guard(priceLock);
                priceCache[key] = price;
                return price;
            };
            // [gold43] only what a vendor sells for gold (npc_vendor, no extended cost): the item store's best
            // food of a level was often a quest or event item never sold - b390 Peny "short of its food and drink
            // 21 s" with 81 s in its purse
            static std::once_flag soldOnce;
            static std::unordered_set<uint32> sold;
            std::call_once(soldOnce, []()
            {
                for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
                {
                    if (!(tmpl.npcflag & UNIT_NPC_FLAG_VENDOR))
                        continue;
                    if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry))
                        for (VendorItem const* item : items->m_items)
                            if (item && !item->ExtendedCost)
                                sold.insert(item->item);
                }
            });
            auto const unitPrice = [p](auto const& matches) -> float
            {
                int32 bestLevel = -1;
                float best = 0.0f;
                for (auto const& [id, proto] : *sObjectMgr->GetItemTemplateStore())
                {
                    if (!proto.BuyPrice || int32(proto.RequiredLevel) > int32(p->GetLevel()) || !sold.count(id) ||
                        !matches(proto))
                        continue;
                    float const unit = float(proto.BuyPrice) / float(std::max<uint32>(1, proto.BuyCount));
                    if (int32(proto.RequiredLevel) > bestLevel || (int32(proto.RequiredLevel) == bestLevel && unit < best))
                    {
                        bestLevel = int32(proto.RequiredLevel);
                        best = unit;
                    }
                }
                return best;
            };
            auto const consumable = [](uint32 category, AuraType a, AuraType b)
            {
                return [category, a, b](ItemTemplate const& proto)
                {
                    if (proto.Class != ITEM_CLASS_CONSUMABLE || proto.Spells[0].SpellCategory != category)
                        return false;
                    SpellInfo const* spell = sSpellMgr->GetSpellInfo(proto.Spells[0].SpellId);
                    for (uint8 i = 0; spell && i < MAX_SPELL_EFFECTS; ++i)
                        if (spell->Effects[i].ApplyAuraName == a || spell->Effects[i].ApplyAuraName == b)
                            return true;
                    return false;
                };
            };
            constexpr uint32 SUPPLY_STACK = 20;    // a stack of food / drink (the restock's buy)
            constexpr uint32 AMMO_ROUNDS = 1000;   // a hunter's quiver load
            constexpr uint32 LOW_AMMO = 100;       // the vendor run's low-ammo line
            float need = 0.0f;
            // [supply76] a five's member before it goes in: the restock its supply check asks (below RESTOCK_FOOD_MIN:
            // up to RESTOCK_FOOD_TARGET), not only an empty bag. b406 rfc-n five 4: Feette (BM hunter, 11 drinks, 35 s)
            // reached Jazzik for "buy drink", bought ammo only (its consumable budget kept its money for its repair and
            // ranks), went in "short of drink 11", ran dry at pull 20 and held the Taragaman pull 18 times
            bool const fiveRestock = st.fiveId && !st.supplyShort.empty();
            uint32 const foodHave = CountBagFood(p, RESTOCK_SPELL_CATEGORY_FOOD);
            uint32 const drinkHave = CountBagFood(p, RESTOCK_SPELL_CATEGORY_DRINK);
            auto const units = [&](uint32 have) -> uint32
            {
                if (fiveRestock && have < RESTOCK_FOOD_MIN)
                    return RESTOCK_FOOD_TARGET - have;
                return have == 0 ? SUPPLY_STACK : 0;
            };
            if (uint32 const n = units(foodHave))
            {
                need += n * cachedPrice(0, [&]() {
                            return unitPrice(consumable(RESTOCK_SPELL_CATEGORY_FOOD, SPELL_AURA_MOD_REGEN,
                                                        SPELL_AURA_OBS_MOD_HEALTH)); });
                st.supplyWhat += "food";
            }
            if (uint32 const n = p->GetMaxPower(POWER_MANA) > 0 ? units(drinkHave) : 0)
            {
                need += n * cachedPrice(1, [&]() {
                            return unitPrice(consumable(RESTOCK_SPELL_CATEGORY_DRINK, SPELL_AURA_MOD_POWER_REGEN,
                                                        SPELL_AURA_OBS_MOD_POWER)); });
                st.supplyWhat += st.supplyWhat.empty() ? "drink" : " and drink";
            }
            // [ammo84] any class whose ranged weapon fires ammo (a warrior / rogue's gun is its pull)
            if (true)
                if (Item* ranged = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
                {
                    uint32 const sub = ranged->GetTemplate()->SubClass;
                    uint32 const ammoSub = sub == ITEM_SUBCLASS_WEAPON_BOW || sub == ITEM_SUBCLASS_WEAPON_CROSSBOW
                                               ? ITEM_SUBCLASS_ARROW
                                           : sub == ITEM_SUBCLASS_WEAPON_GUN ? ITEM_SUBCLASS_BULLET
                                                                             : 0;
                    uint32 const rounds =
                        GET_PLAYERBOT_AI(p)->GetAiObjectContext()->GetValue<uint32>("item count", "ammo")->Get();
                    if (ammoSub && rounds < LOW_AMMO)
                    {
                        need += AMMO_ROUNDS * cachedPrice(2 + ammoSub, [&]() {
                                    return unitPrice([ammoSub](ItemTemplate const& proto)
                                                     { return proto.Class == ITEM_CLASS_PROJECTILE &&
                                                              proto.SubClass == ammoSub; }); });
                        st.supplyWhat += st.supplyWhat.empty() ? "ammo" : " and ammo";
                    }
                }
            uint32 const needCopper = uint32(std::ceil(need));
            // [supply76] against the money it may spend on consumables (its budget keeps its repair and ranks back)
            uint32 const spendable =
                GET_PLAYERBOT_AI(p)->GetAiObjectContext()
                    ->GetValue<uint32>("free money for", std::to_string(uint32(NeedMoneyFor::consumables)))
                    ->Get();
            if (needCopper && spendable < needCopper)
                st.supplyGoldShort = needCopper - spendable;
        }
        st.repairShort = 0;
        if (repairNeed.any)
        {
            GuildOrder o;
            o.type = GuildOrderType::REPAIR;
            o.repairUrgent = repairNeed.urgent;
            o.cost = repairNeed.urgent ? repairNeed.urgentCost : repairNeed.cost;
            o.pos = NewRpgBaseAction::NearestRepairSpawn(p);
            o.details = repairNeed.items + "; repair " + MoneyText(o.cost);
            o.reason = repairNeed.weaponBroken ? "weapon broken: no new pulls until repaired"
                       : repairNeed.urgent     ? "weapon / armor: breaks at the next death or broken"
                                               : "armor: breaks at the next death";
            if (o.pos == WorldPosition())
                o.status = "no repairer on this continent";
            else if (p->GetMoney() < o.cost)
            {
                st.repairShort = o.cost - p->GetMoney();
                o.status = "short " + MoneyText(st.repairShort) + ": gold from the guild";
            }
            else
            {
                o.executable = true;
                o.status = "to the repairer";
            }
            if (repairNeed.urgent)
                orders.insert(orders.begin(), o);  // [regress10] the first executable order decides (interrupts)
            else
                orders.push_back(o);
        }
        FoundingOrders(st, row.low, p, orders);  // [guild80] found / sign / join the guild, buy the bank tab
        TransferOrders(row.low, p, orders);       // [guild81] trade / mail to a member, gold for the bank tab
        if (!HumanCharacter(p) && p->HasSkill(SKILL_ENCHANTING) && p->HasSpell(SPELL_DISENCHANT))
            AddOwnDisenchants(st, p);  // [ench155]
        // [inv380] the junk it carries for nothing onto its SELL task, whatever its bag pressure is: a vendor pays
        // for it and nothing in the guild wants it, so a slot spent on it is a slot lost for loot
        if (!HumanCharacter(p))
            AddDeadWeight(st, row.low, p);
        if (!st.disenchant.empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::DISENCHANT;
            o.count = uint32(st.disenchant.size());
            o.details = std::to_string(o.count) + " items";
            o.reason = "an upgrade for nobody, the enchanter itself";
            o.status = "disenchants";
            o.executable = true;
            orders.push_back(o);
        }
        std::vector<HeldItem> bankHeld;
        for (HeldItem const& h : st.held)
            if (h.purpose != "pending" && h.forLow != row.low &&
                !TransferHolds(h.itemGuidLow))  // [guild81] trade / mail moves it; [tasks] its own plan stays
                bankHeld.push_back(h);
        // [guild16] no bank order while the guild or its bank tab does not exist: the bot just holds the items
        std::string const bankWhy = bankHeld.empty() ? std::string() : DepositStateOf(p, bankHeld);
        if (bankWhy != st.bankBlocked)
        {
            // [bank408] e.g. a full bank: no vault visit, the bot quests on; full bags release the held items
            if (!bankWhy.empty())
                LOG_INFO("playerbots", "[bank408] {}: no BANK_DEPOSIT for {} items ({}), quests on", Who(row.low),
                         bankHeld.size(), bankWhy);
            st.bankBlocked = bankWhy;
        }
        if (!bankHeld.empty() && bankWhy.empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::BANK_DEPOSIT;
            o.itemId = bankHeld.front().itemId;
            o.count = uint32(bankHeld.size());
            o.forLow = bankHeld.front().forLow;
            o.details =
                std::to_string(o.count) + " items: " + ItemName(o.itemId) + " (" + bankHeld.front().purpose + ")";
            o.reason = "the global leader's loot decision";
            WorldPosition pos;
            float dist = 0;
            if (!st.vaultFail.empty())
                o.status = st.vaultFail;
            else if (!GuildBankRun::NearestVault(p, pos, dist))
                o.status = "no vault on this continent";
            else
            {
                o.executable = true;
                o.pos = pos;  // [tasks] the vault's town
                o.status = "to the vault " + std::to_string(uint32(dist)) + " yd";
            }
            orders.push_back(o);
        }
        auto const wd = withdraw_.find(row.low);
        if (wd != withdraw_.end() && !wd->second.empty() && GuildBankRun::BankState(p, false).empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::BANK_WITHDRAW;
            o.itemId = wd->second.front().first;
            o.count = uint32(wd->second.size());
            o.details = ItemName(o.itemId) + " (" + wd->second.front().second + ")";
            o.reason = "an upgrade for it lies in the bank";
            WorldPosition pos;
            float dist = 0;
            if (!GuildBankRun::NearestVault(p, pos, dist))
                o.status = "no vault on this continent";
            else
            {
                o.executable = true;
                o.pos = pos;  // [tasks] the vault's town
                o.status = "to the vault " + std::to_string(uint32(dist)) + " yd";
            }
            orders.push_back(o);
        }
        // [gather] gatherers (groups B, C): the place and the share the gathering plan assigned them (Assign above)
        if (GatherAssignment a; (row.grp == 'B' || row.grp == 'C') && GET_PLAYERBOT_AI(p) &&
                                sGuildGatherPlan.Assigned(row.low, a))
        {
            GuildOrder o;
            o.type = GuildOrderType::GATHER;
            o.itemId = a.itemId;
            o.count = a.share;
            o.placeId = a.placeId;
            o.pos = a.spot;
            o.details = ItemName(a.itemId) + " ×" + std::to_string(a.share) + " @ " + a.placeName;
            if (a.climbSkill)  // [craft432] its own task key: a climb is not the gather of its item
            {
                o.skillLine = a.climbSkill;
                o.details = std::string(SkillShort(a.climbSkill)) + " climb to " + std::to_string(a.climbTo) + ": " +
                            o.details;
            }
            o.reason = a.why;
            // [gather] b337: every GATHER was URGENT and took 11 bots off their quests for 0.0/h chests. The leader's
            // value test decides: a gather that pays more per minute than the bot's questing runs now (URGENT,
            // ClassifyTask); else the task waits and the bot quests (what it picks on the way counts)
            o.executable = a.worth;
            o.status = a.worth ? "gathers (in bags " + std::to_string(p->GetItemCount(a.itemId, false)) + ")"
                               : "quests: gathering does not beat questing";
            orders.push_back(o);
        }
        // [tasks] PROFESSION: the global leader's calls (UpdateProfessions: the profession beats questing by value
        // per minute, or a rank / the planned profession waits at the trainer)
        if (auto const calls = professions_.find(row.low); calls != professions_.end() && !HumanCharacter(p))
            for (ProfessionPick const& pick : calls->second)
            {
                GuildOrder o;
                o.type = GuildOrderType::PROFESSION;
                o.profession = pick.kind;
                o.skillLine = pick.skillLine;
                o.itemId = pick.itemId;
                o.count = pick.count;
                o.details = pick.details;
                o.reason = pick.reason;
                o.pos = pick.pos;  // [econ2] TRAIN: its trainer's town (ClassifyTask), the call waits for the visit
                o.spellId = pick.spellId;  // [prof273] a recipe call's taught spell (its own task key)
                // GATHER runs as DO_GATHER here; TRAIN / BUY / CRAFT through the profession executor in town
                o.executable = pick.kind == GuildProfessionKind::GATHER;
                o.status = pick.kind == GuildProfessionKind::GATHER ? "gathers itself"
                                                                    : "on its next town visit (profession executor)";
                // [econ1] a smelt: at the town forge on its next visit there (a convenient town task, NextStep)
                if (pick.kind == GuildProfessionKind::CRAFT && pick.skillLine == SKILL_MINING &&
                    pick.pos != WorldPosition())
                {
                    o.pos = pick.pos;
                    o.executable = true;
                    o.status = "smelts at the town forge on its next visit";
                }
                // [lag342] a craft call: a convenient town task at its inn's town (the route's stop), run there by
                // the profession executor as in any safe place
                else if (pick.kind == GuildProfessionKind::CRAFT && pick.pos != WorldPosition())
                {
                    o.pos = pick.pos;
                    o.cost = pick.cost;
                    o.executable = true;
                    o.status = "crafts in the town of its route's stop [lag342]";
                }
                // [prof272] a TRAIN call (a rank, a recipe, a tool) the bot's money pays: a convenient town task as
                // the smelt - its trainer's / vendor's town is a stop of its route (TownStops), the executor learns /
                // buys there (ProfessionCall due). Before, it stayed "on its next town visit" (executable false: no
                // town stop) and ran only when a walk happened to end in that town: b426-b429 160 tool / rank /
                // recipe calls, 2 learned at the planned trainer, 104 Mining Pick / Skinning Knife calls dropped
                // unrun, no Cooking recipe learned for the cook
                // [lag326] ... and a BUY call of a vendor-only reagent: the vendor's town is a stop of its route
                else if ((pick.kind == GuildProfessionKind::TRAIN || pick.kind == GuildProfessionKind::BUY) &&
                         pick.pos != WorldPosition())
                {
                    // [craft381] its game price weighs its town stop ([lag337] / [lag340], TownStopCopper sums the
                    // orders' cost there): b446 the TRAIN / BUY orders carried none, so Dekorhan's Virtuoso Inking
                    // Set in Darnassus weighed 0 c against his questing and the other-landmass stop was never taken
                    o.cost = std::max(pick.cost, pick.worth);
                    std::string const taskKey = TaskKey(o);
                    auto const tried = st.trainDue.find(taskKey);
                    if (tried != st.trainDue.end() && tried->second != p->GetLevel())
                        st.trainDue.erase(tried);
                    // it stood in that town with the call runnable and left it with the call still standing: the
                    // executor had its chance there (the trainer / vendor did not do it at this money and level)
                    auto const before = std::find_if(st.orders.begin(), st.orders.end(), [&](GuildOrder const& x)
                                                     { return x.type == o.type && TaskKey(x) == taskKey; });
                    // [craft439] b468 Colian logged in a ghost ("no New RPG strategy"), ran from the Blood Watch
                    // graveyard to his corpse, and "left Blood Watch with its TRAIN call Jeweler's Kit still
                    // standing": a ghost's pass through the town was counted as the executor's chance, his kit call
                    // parked until L24. A visit counts only while the bot stood there alive, out of combat, its New
                    // RPG executor on (the strategy that runs NewRpgProfession::StartTask)
                    if (before != st.orders.end() && before->townArea && TownReady(p, *before) && p->IsAlive() &&
                        !p->IsInCombat() && GET_PLAYERBOT_AI(p) &&
                        GET_PLAYERBOT_AI(p)->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
                        st.trainChance.insert(taskKey);
                    // [craft438] the tool a guild priority line ([craft435]) needs never waits for a level: its stop
                    // is priced by the gap (craft436) and taken again
                    bool priorityTool = false;
                    for (LinePriority const& r : linePriorities_)
                        priorityTool |= r.line == pick.skillLine && pick.itemId &&
                                        pick.itemId == ProfessionTool(pick.skillLine);
                    if (priorityTool)
                        st.trainDue.erase(taskKey);
                    if (before != st.orders.end() && before->executable && !before->waitsTown &&
                        before->townArea && !TownReady(p, *before))
                    {
                        if (st.trainChance.erase(taskKey) && !priorityTool && !st.trainDue.count(taskKey))
                        {
                            st.trainDue[taskKey] = p->GetLevel();
                            LOG_INFO("playerbots",
                                     "[prof272] {} L{} left {} with its TRAIN call {} still standing: no town stop for "
                                     "it until its next level",
                                     Who(row.low), p->GetLevel(), before->townName, o.details);
                        }
                        else if (priorityTool)
                            LOG_INFO("playerbots",
                                     "[craft438] {} L{} left {} with the guild priority tool {} unbought: its stop "
                                     "stays on the route, weighed by the gap",
                                     Who(row.low), p->GetLevel(), before->townName, o.details);
                    }
                    if (st.trainDue.count(taskKey))
                        o.status = "its trainer / vendor visit at this level did not do it: waits for its next level";
                    else if (p->GetMoney() < pick.cost)
                        o.status = "waits for its money: " + Gsc(pick.cost) + " (has " + Gsc(p->GetMoney()) + ")";
                    else if (pick.itemId && !FreeBagSlots(p))
                        o.status = "waits for a free bag slot for the tool";
                    else
                    {
                        trainCalls.push_back(o);
                        continue;
                    }
                }
                orders.push_back(o);
            }
        // [craft393] ... but never a reagent of a recipe it knows (marked before that rule, or learned since)
        st.sell.erase(std::remove_if(st.sell.begin(), st.sell.end(),
                                     [&](uint32 guid)
                                     {
                                         Item* item = p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid));
                                         if (!item || (item->GetTemplate()->Class != ITEM_CLASS_TRADE_GOODS &&
                                                       item->GetTemplate()->Class != ITEM_CLASS_REAGENT))
                                             return false;
                                         if (CookNeeds(item->GetEntry()))  // [craft398]
                                             return true;
                                         for (auto const& [line, recipes] : recipesBySkill_)
                                             if (p->HasSkill(line))
                                                 for (Recipe const& r : recipes)
                                                     if (p->HasSpell(r.spellId))
                                                         for (auto const& [reagent, n] : r.reagents)
                                                             if (reagent == item->GetEntry())
                                                                 return true;
                                         return false;
                                     }),
                      st.sell.end());
        // [craft399] user decision 2026-11-12: outgrown low food is sold, kept only while it is really needed - a food
        // whose one item gives a smaller share of a full rest than another food it holds goes once the better foods
        // alone cover its food line in full rests (RESTOCK_FOOD_MIN, BagFoodRests); below that line it stays as the
        // filler. A five's member keeps its food for the run
        if (!st.fiveId)
        {
            struct FoodStack
            {
                uint32 guid{0};
                float share{0.0f};
                uint32 count{0};
            };
            std::vector<FoodStack> foods;
            auto const look = [&](Item* item)
            {
                if (float const share = item ? FoodItemRests(p, item->GetTemplate()) : 0.0f; share > 0.0f)
                    foods.push_back({item->GetGUID().GetCounter(), share, item->GetCount()});
            };
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                look(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
                if (Bag* bag = p->GetBagByPos(b))
                    for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                        look(bag->GetItemByPos(uint8(j)));
            std::string sold;
            for (FoodStack const& f : foods)
            {
                float better = 0.0f;
                for (FoodStack const& g : foods)
                    if (g.share > f.share)
                        better += g.share * float(g.count);
                if (better < float(RESTOCK_FOOD_MIN) ||
                    std::find(st.sell.begin(), st.sell.end(), f.guid) != st.sell.end())
                    continue;
                st.sell.push_back(f.guid);
                if (Item* item = p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(f.guid)))
                    sold += (sold.empty() ? "" : ", ") + ItemName(item->GetEntry()) + " ×" + std::to_string(f.count) +
                            Acore::StringFormat(" ({:.2f} of a rest each)", f.share);
            }
            if (!sold.empty())
                LOG_INFO("playerbots", "[craft399] {} L{} sells outgrown food: {} - its better food covers its {} "
                         "rests", Who(row.low), p->GetLevel(), sold, RESTOCK_FOOD_MIN);
        }
        // [tasks] SELL: what the global leader gave up goes at the next vendor visit (SellMarked), never a trip
        if (!st.sell.empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::SELL;
            o.count = uint32(st.sell.size());
            // [tasks] what it sells, by name (the first few)
            std::string names;
            for (uint32 guid : st.sell)
                if (Item* item = p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid)); item && names.size() < 80)
                    names += (names.empty() ? "" : ", ") + ItemName(item->GetEntry());
            o.details =
                std::to_string(o.count) + " items" + (names.empty() ? "" : ": " + names) + " — sell to any vendor";
            o.reason = "nobody in the guild needs it (global leader's decision), selling frees the bags";
            o.status = "at the nearest vendor on the way";
            orders.push_back(o);
        }
        if (!HumanCharacter(p))
        {
            // [guild50] CRAFT_FOR: this crafter's jobs, the first it holds the reagents for
            CraftJob const* job = nullptr;
            uint32 jobs = 0;
            for (CraftJob const& j : crafts_)
                if (j.crafterLow == row.low)
                {
                    ++jobs;
                    if (!job || (j.ready && !job->ready))
                        job = &j;
                }
            if (job)
            {
                GuildOrder o;
                o.type = GuildOrderType::CRAFT_FOR;
                o.itemId = job->itemId;
                o.spellId = job->spellId;
                o.forLow = job->forLow;
                o.count = jobs;
                o.details = ItemName(job->itemId) + " for " + (job->forLow == row.low ? "self" : Who(job->forLow)) +
                            (jobs > 1 ? " (orders in total " + std::to_string(jobs) + ")" : "");
                o.reason = job->why;
                o.executable = job->ready;
                o.status = job->ready ? "materials there: creates" : "waits for materials (wanted list)";
                orders.push_back(o);
            }
            // BUY: the biggest upgrade it can pay for at a vendor
            for (GearPick const& g : st.gearNow)
            {
                if (g.kind != GuildGear::SRC_VENDOR || p->GetItemCount(g.itemId, true) || p->GetMoney() < g.aux ||
                    (g.extendedCost && !GuildGear::ExtendedCostMissing(p, g.extendedCost).empty()))
                    continue;
                // [tasks] user 2026-11-08 (b349: 26 BUY tasks «waits town», most never visited): a vendor on another
                // map is on no route of the bot; a quest reward it is about to take for the same slot supersedes
                // the buy. Both are dropped with a line (once per item)
                std::string drop;
                ItemTemplate const* buyProto = sObjectMgr->GetItemTemplate(g.itemId);
                if (g.pos.GetMapId() != p->GetMapId())
                    drop = "the vendor is on another map (" + GuildGear::MapName(g.pos.GetMapId()) +
                           "): no route of its own goes there";
                else if (buyProto)
                    for (auto const& [questId, pick] : st.rewards)
                        if (ItemTemplate const* reward = sObjectMgr->GetItemTemplate(pick.itemId);
                            !pick.forLow && reward && reward->InventoryType == buyProto->InventoryType &&
                            p->GetQuestStatus(questId) != QUEST_STATUS_NONE)
                        {
                            drop = "superseded by the reward of q" + std::to_string(questId) + ": " +
                                   ItemName(pick.itemId);
                            break;
                        }
                if (!drop.empty())
                {
                    if (st.buyDropped.insert(g.itemId).second)
                        LOG_INFO("playerbots", "[tasks] {} BUY {} not planned: {}", Who(row.low), ItemName(g.itemId),
                                 drop);
                    continue;
                }
                GuildOrder o;
                o.type = GuildOrderType::BUY;
                o.itemId = g.itemId;
                o.count = 1;
                o.npcEntry = g.srcId;
                o.pos = g.pos;
                o.cost = g.aux;
                o.extendedCost = g.extendedCost;
                o.details = ItemName(g.itemId) + " for itself — " + g.text + ", price " + Gsc(g.aux);
                o.reason = std::string("upgrade ") + GuildGear::GroupName(g.group) + " " + GainText(g) +
                           ", with its own money";
                o.status = "to the vendor";
                o.executable = true;
                orders.push_back(o);
                break;
            }
            GuildRiding::AddOrders(p, FoundingFreeMoney(p), orders);  // [mount1] riding, then its mount
            // [supply112] BUY: a supply cook's recipe it has not learned, at the nearest vendor of its map that sells
            // it, with its own money (the [recipes] use teaches it from the bags)
            if (auto const jobs = supplyCrafts_.find(row.low); jobs != supplyCrafts_.end())
                for (SupplyCraft const& job : jobs->second)
                {
                    if (!job.recipeItem || p->HasSpell(job.spellId) || p->GetItemCount(job.recipeItem, true) ||
                        p->GetMoney() < job.recipePrice)
                        continue;
                    uint32 npcEntry = 0;
                    WorldPosition pos;
                    if (!NearestVendorOf(p, job.recipeItem, npcEntry, pos))
                        continue;
                    GuildOrder o;
                    o.type = GuildOrderType::BUY;
                    o.itemId = job.recipeItem;
                    o.count = 1;
                    o.npcEntry = npcEntry;
                    o.pos = pos;
                    o.cost = job.recipePrice;
                    o.details = ItemName(job.recipeItem) + " for itself — the cook of band " + job.band +
                                ", price " + Gsc(job.recipePrice);
                    o.reason = "[supply112] the guild cook's recipe for the fives' " + ItemName(job.product) +
                               ", with its own money";
                    o.status = "to the vendor";
                    o.executable = true;
                    orders.push_back(o);
                    break;
                }
            // [supply113] BUY: the supply cook's own fire - Basic Campfire's reagent and tool it lacks (its spell's
            // SpellInfo), one lot at the nearest vendor of its map, with its own money
            if (auto const jobs = supplyCrafts_.find(row.low); jobs != supplyCrafts_.end() && !jobs->second.empty() &&
                p->HasSpell(NewRpgCraft::SPELL_BASIC_CAMPFIRE))
                if (SpellInfo const* fire = sSpellMgr->GetSpellInfo(NewRpgCraft::SPELL_BASIC_CAMPFIRE))
                {
                    std::vector<uint32> lack;
                    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                        if (fire->Reagent[i] > 0 && !p->HasItemCount(uint32(fire->Reagent[i]), fire->ReagentCount[i]))
                            lack.push_back(uint32(fire->Reagent[i]));
                    for (uint32 totem : fire->Totem)
                        if (totem && !p->HasItemCount(totem, 1))
                            lack.push_back(totem);
                    // a tool by its kind: the cheapest item of that kind a vendor sells
                    for (uint32 category : fire->TotemCategory)
                        if (category && !p->HasItemTotemCategory(category))
                        {
                            uint32 best = 0;
                            for (auto const& [id, proto] : *sObjectMgr->GetItemTemplateStore())
                                if (proto.TotemCategory == category && proto.BuyPrice &&
                                    (!best || proto.BuyPrice < sObjectMgr->GetItemTemplate(best)->BuyPrice))
                                {
                                    uint32 npc = 0;
                                    WorldPosition at;
                                    if (NearestVendorOf(p, id, npc, at))
                                        best = id;
                                }
                            if (best)
                                lack.push_back(best);
                        }
                    for (uint32 item : lack)
                    {
                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                        uint32 npcEntry = 0;
                        WorldPosition pos;
                        if (!proto || p->GetMoney() < proto->BuyPrice || !NearestVendorOf(p, item, npcEntry, pos))
                            continue;
                        GuildOrder o;
                        o.type = GuildOrderType::BUY;
                        o.itemId = item;
                        o.count = 1;
                        o.npcEntry = npcEntry;
                        o.pos = pos;
                        o.cost = proto->BuyPrice;
                        o.details = ItemName(item) + " for itself — the guild cook's Basic Campfire, price " +
                                    Gsc(proto->BuyPrice);
                        o.reason = "[supply113] the cook lights its own fire where no cooking fire stands, with its "
                                   "own money";
                        o.status = "to the vendor";
                        o.executable = true;
                        orders.push_back(o);
                        break;
                    }
                }
            // QUEST_REWARD: the choices the turn-ins will take
            if (!st.rewards.empty())
            {
                GuildOrder o;
                o.type = GuildOrderType::QUEST_REWARD;
                auto const& [questId, pick] = *st.rewards.begin();
                o.itemId = pick.itemId;
                o.forLow = pick.forLow;
                o.count = uint32(st.rewards.size());
                o.details = "q" + std::to_string(questId) + ": " + ItemName(pick.itemId) +
                            (st.rewards.size() > 1 ? " (+" + std::to_string(st.rewards.size() - 1) + " quests)" : "");
                o.reason = pick.why;
                o.status = StatusName(p, GET_PLAYERBOT_AI(p));
                orders.push_back(o);
            }
            // DUNGEON (plan, stage 4 forms the fives): the dungeon with most of its best planned upgrades
            std::map<uint32, std::pair<uint32, std::string>> byDungeon;
            for (GearPick const& g : st.gearPlan)
                if (g.kind == GuildGear::SRC_DROP)
                {
                    auto& d = byDungeon[g.mapId];
                    ++d.first;
                    if (d.second.size() < 50)
                        d.second += (d.second.empty() ? "" : ", ") + ItemName(g.itemId);
                }
            uint32 bestMap = 0;
            for (auto const& [mapId, d] : byDungeon)
                if (!bestMap || d.first > byDungeon[bestMap].first)
                    bestMap = mapId;
            if (bestMap && !st.fiveId)  // in a five: the five's own DUNGEON order ([guild60])
            {
                GuildOrder o;
                o.type = GuildOrderType::DUNGEON;
                o.count = byDungeon[bestMap].first;
                o.details = GuildGear::MapName(bestMap) + ": " + std::to_string(o.count) + " upgr. (" +
                            byDungeon[bestMap].second + ")";
                o.reason = "the best available slot items — this dungeon's loot";
                o.status = "plan: the global leader forms the five";
                orders.push_back(o);
            }
        }
        for (GuildOrder const& ext : st.external)
            orders.push_back(ext);
        // [prof272] the runnable TRAIN calls last: the first executable order decides an URGENT interrupt (below)
        for (GuildOrder& t : trainCalls)
        {
            t.executable = true;
            t.status = t.profession == GuildProfessionKind::BUY
                           ? "buys on its route's stop in the vendor's town ([lag326])"
                           : "trains on its route's stop in the trainer's town ([prof272])";
            orders.push_back(std::move(t));
        }
        GuildOrder quest;
        quest.details = "its own leader";
        quest.reason = "nothing more useful to do";
        orders.push_back(quest);

        // [tasks] urgency, place, origin and town of every task; a convenient town task waits for that town
        uint32 const freeSlots = FreeBagSlots(p);
        for (GuildOrder& o : orders)
        {
            ClassifyTask(o);
            if (o.type == GuildOrderType::BANK_DEPOSIT && !freeSlots)
            {
                o.priority = GuildTaskPriority::URGENT;  // bags full: the held items block its looting
                o.reason += "; bags full";
            }
            o.waitsTown = o.executable && !TownReady(p, o);
            if (o.waitsTown)
                o.status = "waits for town: " + (o.townName.empty() ? std::string("?") : o.townName) + "; " + o.status;
        }
        // [human] a human's character: no task of the leader touches its items, money or professions (the selfbot
        // that drives it while he is away included); each one is a suggestion he acts on himself
        if (HumanCharacter(p))
            for (GuildOrder& o : orders)
            {
                bool const touches = o.type != GuildOrderType::QUEST && o.type != GuildOrderType::COOP &&
                                     o.type != GuildOrderType::DUNGEON && o.type != GuildOrderType::QUEST_REWARD &&
                                     o.type != GuildOrderType::JOIN && o.type != GuildOrderType::SIGN_CHARTER;
                if (!touches)
                    continue;
                bool const wasRun = o.executable;
                o.executable = false;
                o.waitsTown = false;
                o.status = "advice to a human (he decides himself)";
                if (wasRun)
                    Suggest(row.low, TaskKey(o),
                            std::string("advice: ") + OrderName(o.type) + " — " + o.details + " (" + o.reason + ")");
            }
        // [tasks] the bag pressure (logged when its class changes): full bags block looting; with no bank tab the
        // held items for the bank stay in the bags, so what nobody in the guild uses is released to the vendor
        {
            uint32 total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
                if (Bag* bag = p->GetBagByPos(b))
                    if (!bag->GetTemplate()->BagFamily)
                        total += bag->GetBagSize();
            int8 const cls = freeSlots == 0 ? 0 : (freeSlots * 5 <= total ? 1 : 2);
            // [bank411] a full bank (no empty slot) relieves nothing either
            bool const noBank = !GuildBankRun::DepositState(p, 0, 1).empty();
            uint32 heldBank = 0;
            for (HeldItem const& h : st.held)
                if (!h.forLow && h.purpose != "pending")
                    ++heldBank;
            // [inv380] ... and what the bags carry for nothing, per bot, for the triage's bag metric: the grey junk
            // slots marked for sale, the quest starters held unopened, the quest items of no quest of its log
            uint32 const deadKey = st.junkSlots * 10000 + st.startersOpen * 100 + st.questDeadSlots;
            if (cls != st.bagClass || deadKey != st.bagDeadKey)
            {
                st.bagClass = cls;
                st.bagDeadKey = deadKey;
                LOG_INFO("playerbots", "[tasks] {} bags free={}/{} pressure={} held_for_bank={} sell_marked={} "
                         "bank={} dead_junk={} starters_open={} quest_dead={} [inv380]",
                         Who(row.low), freeSlots, total, cls == 0 ? "full" : (cls == 1 ? "low" : "ok"), heldBank,
                         st.sell.size(), noBank ? "no tab" : "yes", st.junkSlots, st.startersOpen, st.questDeadSlots);
            }
            // full bags and no bank: a held item goes to the vendor (sold, never destroyed) unless a transfer moves
            // it. [econ1] b350: a wanted reagent stayed in full bags (Thamesh 100 % -> 100 % at the vendor) because
            // the guild was short of it somewhere; now the transfers' bag relief sends it to a crafter whose own
            // skill-ups lack it and who has room, and only what nobody can take is sold
            // [econ2] b353: 18 vendor visits at >= 90 % bags, 13 still full after it (Blahadrin 10 visits at 92-95 %,
            // Septimon / Maronic 100 % -> 100 %). The leader decided only for the loot it saw (OnLoot): the bags held
            // what came before or by other ways - raw meat (262 slots over 74 members), cloth, ore kept as "needed
            // for its skill" by the vendor sale (ItemUsageValue), gear it neither wears nor plans (Maronic: three
            // Frayed Robes, two Ragged Leather Vests with its chest slot empty: BAD_EQUIP, never sold). At full bags
            // with no bank tab the leader takes every bag item over: a material beyond its own called skill-ups and
            // its quests goes to the bank list (the bag relief to a crafter with room, else the vendor, next pass),
            // gear no member's plan wants and it could wear now is sold. Food, potions, ammo, tools, quest and class
            // items stay
            std::set<uint32> adopted;
            if (cls == 0 && noBank && !HumanCharacter(p))
            {
                std::map<uint32, uint32> keep;  // item -> units its own work still uses
                if (auto const own = crafterNeed_.find(row.low); own != crafterNeed_.end())
                    keep = own->second;
                for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(p->GetQuestSlotQuestId(slot)))
                        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                            if (quest->RequiredItemId[i])
                                keep[quest->RequiredItemId[i]] += quest->RequiredItemCount[i];
                std::set<uint32> known;
                for (HeldItem const& h : st.held)
                    known.insert(h.itemGuidLow);
                for (uint32 guid : st.sell)
                    known.insert(guid);
                std::vector<Item*> items;
                for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                    if (Item* i = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        items.push_back(i);
                for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
                    if (Bag* bag = p->GetBagByPos(b))
                        for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                            if (Item* i = bag->GetItemByPos(uint8(j)))
                                items.push_back(i);
                std::map<uint32, uint32> kept;  // item -> units kept so far
                for (Item* i : items)
                {
                    ItemTemplate const* proto = i->GetTemplate();
                    uint32 const guid = i->GetGUID().GetCounter();
                    if (!proto || !proto->SellPrice || known.count(guid) || TransferHolds(guid) ||
                        proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM ||
                        std::any_of(recipeLearn_.begin(), recipeLearn_.end(),
                                    [&](auto const& r) { return r.second == guid; }))
                        continue;
                    bool const material = proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_GEM;
                    bool const gear = (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
                                      !proto->TotemCategory && proto->SubClass != ITEM_SUBCLASS_WEAPON_MISC &&
                                      proto->SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE &&
                                      proto->InventoryType != INVTYPE_AMMO && proto->InventoryType != INVTYPE_QUIVER;
                    if (material)
                    {
                        uint32& n = kept[proto->ItemId];
                        auto const want = keep.find(proto->ItemId);
                        if (want != keep.end() && n < want->second)
                        {
                            n += i->GetCount();
                            continue;  // its own called skill-ups / quest use it
                        }
                        st.held.push_back({guid, proto->ItemId, i->GetCount(), 0, "bag material"});
                        adopted.insert(guid);
                        LOG_INFO("playerbots", "[econ2] {} full bags: {} ×{} goes to the guild's list (relief or "
                                 "sale): not for its own called skill-ups or quests", Who(row.low),
                                 ItemName(proto->ItemId), i->GetCount());
                    }
                    else if (gear && proto->RequiredLevel <= p->GetLevel() && !PlannedFor(proto->ItemId))
                    {
                        st.sell.push_back(guid);
                        LOG_INFO("playerbots", "[econ2] {} full bags: {} sold at the next vendor: not worn, no "
                                 "member's gear plan wants it", Who(row.low), ItemName(proto->ItemId));
                    }
                }
            }
            if (cls == 0 && noBank && !HumanCharacter(p))
                for (auto it = st.held.begin(); it != st.held.end();)
                {
                    if (it->forLow || it->purpose == "pending" || TransferHolds(it->itemGuidLow) ||
                        PlannedFor(it->itemId) || adopted.count(it->itemGuidLow) ||
                        GuildBankRun::DepositState(p, it->itemId, it->count).empty())  // [bank411] onto its stack
                    {
                        ++it;
                        continue;
                    }
                    bool const wanted = std::any_of(wanted_.begin(), wanted_.end(), [&](WantedItem const& w)
                                                    { return w.itemId == it->itemId && !w.optional && w.Short() > 0; });
                    if (wanted)
                        LOG_INFO("playerbots",
                                 "[econ1] wanted {} sold from full bags of {}: no crafter with room "
                                 "lacks it, no bank tab",
                                 ItemName(it->itemId), Who(row.low));
                    LOG_INFO("playerbots", "[tasks] {} bags full, no bank tab: {} released to the vendor ({})",
                             Who(row.low), ItemName(it->itemId),
                             wanted ? "no crafter with room takes it" : "nobody wants it");
                    st.sell.push_back(it->itemGuidLow);
                    it = st.held.erase(it);
                }
        }

        // keep the since-time of an order that stays the same; log a decision when the first order changes
        for (GuildOrder& o : orders)
        {
            o.since = now;
            for (GuildOrder const& old : st.orders)
                if (old.type == o.type && old.itemId == o.itemId && old.forLow == o.forLow)
                {
                    o.since = old.since;
                    if (o.type == GuildOrderType::GATHER &&
                        (old.status.rfind("no ", 0) == 0 || old.status.rfind("нет ", 0) == 0))  // [econ1] World text
                        o.status = old.status;  // set by New RPG (no nodes here)
                }
        }
        bool const changed = st.orders.empty() || st.orders.front().type != orders.front().type ||
                             st.orders.front().itemId != orders.front().itemId;
        if (changed && orders.front().type != GuildOrderType::QUEST)
            Decision(Who(row.low) + " → " + OrderName(orders.front().type) + ": " + orders.front().details + " — " +
                     orders.front().reason);
        // [guild15] gear history: a new gear order of the leader (a vendor buy, a bank upgrade, a craft for a member)
        // is the decision behind the equip it ends in (QUEST_REWARD picks are noted where they are made, UpdateGear)
        for (GuildOrder const& o : orders)
        {
            bool const gear =
                o.type == GuildOrderType::BUY || o.type == GuildOrderType::CRAFT_FOR ||
                (o.type == GuildOrderType::BANK_WITHDRAW && o.details.find("upgrade") != std::string::npos);
            if (!gear || !o.itemId ||
                std::any_of(st.orders.begin(), st.orders.end(),
                            [&](GuildOrder const& old) { return TaskKey(old) == TaskKey(o); }))
                continue;
            GuildGear::HistoryNoteDecision(o.forLow ? o.forLow : row.low, o.itemId, "leader",
                                           Who(row.low) + " → " + OrderName(o.type) + ": " + o.details + " — " +
                                               o.reason);
        }
        // [coop] a new URGENT task outranks the bot's co-op task: it leaves the party (co-op exits with "the global
        // leader moved it")
        // [timers] ... and the bot's own questing / grinding (no New RPG status duration decides that): the global
        // leader's task priority is the computed exit; IDLE then runs the task (NextStep)
        bool const coopTask = std::any_of(st.external.begin(), st.external.end(),
                                          [](GuildOrder const& e) { return e.type == GuildOrderType::COOP; });
        bool const ownPlan = st.rpgStatus == RPG_DO_QUEST || st.rpgStatus == RPG_GO_GRIND ||
                             st.rpgStatus == RPG_WANDER_RANDOM || st.rpgStatus == RPG_REST;
        if (coopTask || ownPlan)
            for (GuildOrder const& o : orders)
            {
                if (!o.executable)
                    continue;
                bool const fresh = std::none_of(st.orders.begin(), st.orders.end(), [&](GuildOrder const& old)
                                                { return TaskKey(old) == TaskKey(o); });
                if (o.priority == GuildTaskPriority::URGENT && !o.waitsTown && fresh)
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                    {
                        LOG_INFO("playerbots", "[tasks] {} {} interrupted by URGENT {}: {}", Who(row.low),
                                 coopTask ? "COOP" : RpgName(st.rpgStatus), OrderName(o.type), o.details);
                        ai->rpgInfo.ChangeToIdle();
                    }
                break;  // the first executable order decides
            }
        // [timers] a gathering run the global leader's GATHER task started ends when that task is no longer its first
        // executable one (done, given back, or outranked) - no DO_GATHER duration
        if (st.rpgStatus == RPG_DO_GATHER)
        {
            auto const firstExecutable = [](std::vector<GuildOrder> const& list) -> GuildOrder const*
            {
                for (GuildOrder const& o : list)
                    if (o.executable && !o.waitsTown)
                        return &o;
                return nullptr;
            };
            GuildOrder const* before = firstExecutable(st.orders);
            GuildOrder const* first = firstExecutable(orders);
            if (before && before->type == GuildOrderType::GATHER &&
                (!first || first->type != GuildOrderType::GATHER || TaskKey(*first) != TaskKey(*before)))
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                {
                    LOG_INFO("playerbots", "[tasks] {} DO_GATHER ends: its gather task {} is no longer first ({})",
                             Who(row.low), before->details, first ? OrderName(first->type) : "none");
                    ai->rpgInfo.ChangeToIdle();
                }
        }
        // [regress10] the REPAIR call the map threads read (StartVendorRun, the grind target)
        {
            RepairCall call;
            for (GuildOrder const& o : orders)
                if (o.type == GuildOrderType::REPAIR)
                {
                    call.need = true;
                    call.urgent = o.priority == GuildTaskPriority::URGENT;
                    call.ready = o.executable;
                    call.inTown = !o.waitsTown;
                    call.cost = o.cost;
                    call.at = o.pos;
                    call.weaponBroken = repairNeed.weaponBroken;
                }
            std::lock_guard<std::mutex> repairGuard(repairLock_);
            if (call.need)
                repairCalls_[row.low] = call;
            else
                repairCalls_.erase(row.low);
        }
        LogTaskChanges(row.low, st, orders);
        st.orders = std::move(orders);
    }
}

void GuildCoordinator::SetOrder(uint32 low, GuildOrder order, std::string const& decision)
{
    CoordLock guard(lock_, __func__);
    BotState& st = bots_[low];
    order.since = Now();
    if (order.status.empty())
        order.status = "stage 2: not executed yet";
    st.external.erase(std::remove_if(st.external.begin(), st.external.end(),
                                     [&](GuildOrder const& o) { return o.type == order.type; }),
                      st.external.end());
    if (order.type != GuildOrderType::QUEST)
        st.external.push_back(order);
    Decision(Who(low) + " → " + OrderName(order.type) + ": " + decision);
}

// ---------------------------------------------------------------------------------------------------------------------
// [tasks] the bot's task list: every order is a task with its urgency, place, origin, creation time and town
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::ClassifyTask(GuildOrder& o)
{
    o.source = GuildTaskSource::GLOBAL;
    o.priority = GuildTaskPriority::CONVENIENT;
    o.place = GuildTaskPlace::FIELD;
    switch (o.type)
    {
        case GuildOrderType::QUEST:
            o.source = GuildTaskSource::SYSTEM;
            break;
        case GuildOrderType::COOP:  // [coop] a party's quest in the field
            o.priority = GuildTaskPriority::HELP;
            break;
        case GuildOrderType::PROFESSION:
            if (o.profession == GuildProfessionKind::TRAIN)
                o.place = GuildTaskPlace::TRAINER;
            else if (o.profession == GuildProfessionKind::BUY)
                o.place = GuildTaskPlace::VENDOR;
            else if (o.profession == GuildProfessionKind::CRAFT)
                o.place = GuildTaskPlace::TOWN;  // the craft executor crafts in a safe place (town)
            else
                o.priority = GuildTaskPriority::URGENT;  // gathering beat questing by value: it runs now
            break;
        case GuildOrderType::BANK_DEPOSIT:
        case GuildOrderType::BANK_WITHDRAW:
            o.place = GuildTaskPlace::BANK;
            break;
        case GuildOrderType::BUY:
        case GuildOrderType::SELL:
            o.place = GuildTaskPlace::VENDOR;
            break;
        case GuildOrderType::REPAIR:  // [regress10]
            o.place = GuildTaskPlace::VENDOR;
            if (o.repairUrgent)
                o.priority = GuildTaskPriority::URGENT;
            break;
        case GuildOrderType::GATHER:  // the gatherer's own work in the field: urgent only when it beats questing
            o.priority = o.executable ? GuildTaskPriority::URGENT : GuildTaskPriority::CONVENIENT;
            break;
        case GuildOrderType::DUNGEON:  // executable: the five called it (its walk to the entrance)
            if (o.executable)
            {
                o.priority = GuildTaskPriority::URGENT;
                o.source = GuildTaskSource::FIVE;
            }
            break;
        case GuildOrderType::FOUND_GUILD:  // the founding plan weighs every walk itself ([guild80])
        case GuildOrderType::SIGN_CHARTER:
        case GuildOrderType::BANK_TAB:
            o.priority = GuildTaskPriority::URGENT;
            o.place = GuildTaskPlace::TOWN;
            break;
        case GuildOrderType::JOIN:
            o.priority = GuildTaskPriority::URGENT;
            break;
        case GuildOrderType::MAIL_TO:
            o.place = GuildTaskPlace::MAILBOX;
            if (!o.transferId)
                o.source = GuildTaskSource::SYSTEM;  // mail the core delivered: taken at a mailbox in town
            break;
        case GuildOrderType::DONATE_GOLD:
            if (auto const t = transfers_.find(o.transferId);
                t != transfers_.end() && t->second.channel == Transfer::MAIL)
                o.place = GuildTaskPlace::MAILBOX;
            // [supply91] the supply gold of a five's member is the five's wait: the sender goes to its mailbox now
            // (URGENT), not at its next town visit. b411: Hobliir's 31 s 50 c for Thokhen (five 1) stood "waits town
            // Astranaar" 40 min, five 1's members idle until logout
            if (auto const t = transfers_.find(o.transferId);
                t != transfers_.end() && t->second.gold && t->second.purpose == "supplies")
                if (auto const to = bots_.find(t->second.toLow); to != bots_.end() && to->second.fiveId)
                    o.priority = GuildTaskPriority::URGENT;
            break;
        default:
            break;
    }
    o.townArea = 0;
    o.townZone = 0;
    o.townCapital = false;
    o.townName.clear();
    if (o.place == GuildTaskPlace::FIELD || o.pos == WorldPosition())
        return;
    uint64 const key = (uint64(o.pos.GetMapId()) << 48) |
                       (uint64(uint32(int32(o.pos.GetPositionX())) & 0xFFFFFF) << 24) |
                       uint64(uint32(int32(o.pos.GetPositionY())) & 0xFFFFFF);
    auto cached = townCache_.find(key);
    if (cached == townCache_.end())
        cached = townCache_
                     .emplace(key, sMapMgr->GetAreaId(PHASEMASK_NORMAL, o.pos.GetMapId(), o.pos.GetPositionX(),
                                                      o.pos.GetPositionY(), o.pos.GetPositionZ()))
                     .first;
    uint32 const area = cached->second;
    AreaTableEntry const* entry = sAreaTableStore.LookupEntry(area);
    uint32 zone = area;  // the top of the area chain
    AreaTableEntry const* zoneEntry = entry;
    for (uint32 depth = 0; zoneEntry && zoneEntry->zone && depth < 4; ++depth)
    {
        zone = zoneEntry->zone;
        zoneEntry = sAreaTableStore.LookupEntry(zone);
    }
    o.townArea = area;
    o.townZone = zone;
    o.townCapital = zoneEntry && (zoneEntry->flags & AREA_FLAG_CAPITAL);
    if (o.townCapital)
        o.townName = zoneEntry->area_name[0];
    else if (entry)
        o.townName = entry->area_name[0];
}

bool GuildCoordinator::TownReady(Player* bot, GuildOrder const& o)
{
    if (o.priority == GuildTaskPriority::URGENT || o.place == GuildTaskPlace::FIELD || !o.townArea)
        return true;
    if (o.townCapital)  // a capital: anywhere inside the city ([guild15] the city's mailbox nearest its way)
        return bot->GetZoneId() == o.townZone;
    return SameTown(bot->GetAreaId(), o.townArea);
}

void GuildCoordinator::LogTaskChanges(uint32 low, BotState& st, std::vector<GuildOrder> const& now)
{
    std::set<std::string> keys;
    for (GuildOrder const& o : now)
        if (o.type != GuildOrderType::QUEST)
            keys.insert(TaskKey(o));
    std::set<std::string> old;
    for (GuildOrder const& o : st.orders)
    {
        if (o.type == GuildOrderType::QUEST)
            continue;
        std::string const key = TaskKey(o);
        old.insert(key);
        if (keys.count(key))
            continue;
        bool const ran = st.ranTasks.erase(key) != 0;
        LOG_INFO("playerbots", "[tasks] {} {} {}: {} ({})", Who(low), ran ? "done" : "dropped", OrderName(o.type),
                 o.details, ran ? "ended after its start" : "ended before its start: no longer needed");
    }
    for (GuildOrder const& o : now)
        if (o.type != GuildOrderType::QUEST && !old.count(TaskKey(o)))
            LOG_INFO("playerbots", "[tasks] {} add {} [{}, {}{}, from {}]: {} — {}", Who(low), OrderName(o.type),
                     PriorityRu(o.priority), PlaceRu(o.place), o.townName.empty() ? "" : " " + o.townName,
                     o.source == GuildTaskSource::GLOBAL ? "global"
                     : o.source == GuildTaskSource::FIVE ? "five"
                                                         : "system",
                     o.details, o.reason);
}

std::string GuildCoordinator::TasksText(BotState const& st) const
{
    std::string text;
    for (GuildOrder const& o : st.orders)
    {
        if (o.type == GuildOrderType::QUEST)
            continue;
        std::string const state = o.waitsTown    ? "waits town " + o.townName
                                  : o.executable ? std::string("runnable")
                                                 : OrdersText(o.status, 40);
        text += std::string(text.empty() ? "" : " | ") + OrderName(o.type) +
                (o.type == GuildOrderType::PROFESSION ? std::string("/") + ProfessionKindName(o.profession) : "") +
                "[" + PriorityCode(o.priority) + "," + PlaceCode(o.place) + "," + state + "] " +
                OrdersText(o.details, 110) + (o.reason.empty() ? "" : " — " + OrdersText(o.reason, 70));
    }
    return text;
}

uint32 GuildCoordinator::PlannedFor(uint32 itemId) const
{
    for (auto const& [low, st] : bots_)
        for (auto const* list : {&st.gearNow, &st.gearPlan})
            for (GearPick const& g : *list)
                if (g.itemId == itemId)
                    return low;
    return 0;
}

// [ench155] lock_ held. The enchanter's own soulbound greens / blues in its bags (a quest reward, a BoP drop, the
// piece an upgrade replaced): no member can take them (soulbound), so its own Disenchant makes them the guild's mats
// instead of a vendor sale. Not one it wears better (GuildGear::Evaluate), not one a gear plan holds, not one its
// skill cannot disenchant yet (RequiredDisenchantSkill). OnLoot skips soulbound items, so this bag look is their path.
void GuildCoordinator::AddOwnDisenchants(BotState& st, Player* p)
{
    uint32 const skill = p->GetPureSkillValue(SKILL_ENCHANTING);
    std::map<uint32, double> demand;  // [craft409] built at the first candidate
    bool demandReady = false;
    auto const look = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || !item->IsSoulBound() || !proto->DisenchantID || proto->Quality < ITEM_QUALITY_UNCOMMON ||
            proto->Quality > ITEM_QUALITY_RARE ||
            (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON) ||
            proto->RequiredDisenchantSkill < 0 || uint32(proto->RequiredDisenchantSkill) > skill)
            return;
        uint32 const guid = item->GetGUID().GetCounter();
        if (std::find(st.disenchant.begin(), st.disenchant.end(), guid) != st.disenchant.end() ||
            std::any_of(st.held.begin(), st.held.end(), [&](HeldItem const& h) { return h.itemGuidLow == guid; }) ||
            PlannedFor(proto->ItemId))
            return;
        StatsWeightCalculator calc(p);
        calc.SetItemSetBonus(false);
        calc.SetOverflowPenalty(false);
        GuildGear::Eval ev;
        if (GuildGear::Evaluate(p, calc, proto, ev) && ev.gain > 0.0f)
            return;  // it wears it (the bot's own equip takes it)
        // [craft409] user rule 2026-11-12: disenchanted only for a needed result, else sold
        if (!demandReady)
        {
            demand = DisenchantDemand();
            demandReady = true;
        }
        std::string why;
        if (!DisenchantWorth(proto, skill, demand, why))
        {
            if (std::find(st.sell.begin(), st.sell.end(), guid) == st.sell.end())
            {
                st.sell.push_back(guid);
                LOG_INFO("playerbots", "[craft409] {} L{} sells its own {}: {}", p->GetName(), p->GetLevel(),
                         proto->Name1, why);
            }
            return;
        }
        st.disenchant.push_back(guid);
        LOG_INFO("playerbots", "[ench155] {} L{} will disenchant its own {} (soulbound, skill {}, needs {})",
                 p->GetName(), p->GetLevel(), proto->Name1, skill, proto->RequiredDisenchantSkill);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        if (Bag* bag = p->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                look(bag->GetItemByPos(uint8(j)));
}

// [ench160] lock_ held. LootTemplate::Process of one Disenchant: each group rolls one entry (VisitEntries gives the
// equal-chance share), an entry gives (min + max) / 2 on average
std::map<uint32, float> const& GuildCoordinator::DisenchantYield(uint32 disenchantId)
{
    auto const cached = disenchantYield_.find(disenchantId);
    if (cached != disenchantYield_.end())
        return cached->second;
    std::map<uint32, float>& out = disenchantYield_[disenchantId];
    if (LootTemplate const* tmpl = LootTemplates_Disenchant.GetLootFor(disenchantId))
        tmpl->VisitEntries(
            [&](LootStoreItem const& e, float chance)
            {
                if (e.reference || e.needs_quest || !(e.lootmode & LOOT_MODE_DEFAULT))
                    return;
                out[e.itemid] += std::min(100.0f, chance) / 100.0f * (float(e.mincount) + float(e.maxcount)) / 2.0f;
            });
    return out;
}

void GuildCoordinator::MarkBagsChanged(uint32 low)
{
    if (!IsPlanned(low))
        return;
    std::lock_guard<std::mutex> guard(bagsChangedLock_);
    bagsChanged_.insert(low);
}

// [ench161] lock_ held. User rules: greens nobody wears are disenchanted, not sold; an upgrade a member needs is never
// disenchanted; honest play (the holder mails / trades its own item, GuildTransfer). [ench153] routed only a green
// looted from then on to the enchanter: b442 the members held 179 disenchantable greens / blues in their bags (160 of
// skill 1) from earlier loot, quest rewards and plan picks that moved on, none went to the 5 enchanters (Enchanting
// 8-10), and every enchanter's walk stopped at "Strange Dust x1: neither held nor gathered in the guild".
// [ench162] incremental: only the bags of members whose bags changed (MarkBagsChanged), who came online, or all of
// them when what the routing depends on changed (an enchanter's skill, the gear plans' picks); the copies a plan
// takes that other members keep stay counted from their last look (greensPlanned_)
uint32 GuildCoordinator::EnchanterInbound(uint32 low, Player* enchanter) const
{
    uint32 n = 0;
    for (Mail const* m : enchanter->GetMails())
        if (m && m->state != MAIL_STATE_DELETED)
            n += uint32(m->items.size());
    for (auto const& [holder, hs] : bots_)
        for (HeldItem const& h : hs.held)
            n += h.forLow == low && holder != low ? 1 : 0;
    if (auto const wd = withdraw_.find(low); wd != withdraw_.end())
        n += uint32(wd->second.size());
    return n;
}

std::map<uint32, double> GuildCoordinator::DisenchantDemand()
{
    std::map<uint32, double> demand;
    for (WantedItem const& w : wanted_)
        demand[w.itemId] += w.Short();
    for (auto const& [low, needs] : crafterNeed_)
        for (auto const& [item, n] : needs)
            demand[item] += n;
    // [craft407] a rodless enchanter's Runed Copper Rod: its reagents beyond what it holds
    constexpr uint32 SPELL_RUNED_COPPER_ROD = 7421;
    SpellInfo const* rod = sSpellMgr->GetSpellInfo(SPELL_RUNED_COPPER_ROD);
    std::vector<uint32> yieldsQueued;  // DisenchantIDs on their way to a Disenchant
    for (auto const& [low, st] : bots_)
    {
        Player* e = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        if (rod && e && e->HasSkill(SKILL_ENCHANTING) && !HasProfessionTool(e, SKILL_ENCHANTING))
            for (uint8 k = 0; k < MAX_SPELL_REAGENTS; ++k)
                if (rod->Reagent[k] > 0 && rod->ReagentCount[k] > 0)
                {
                    uint32 const item = uint32(rod->Reagent[k]);
                    uint32 const have = e->GetItemCount(item, false);
                    if (have < rod->ReagentCount[k])
                        demand[item] += rod->ReagentCount[k] - have;
                }
        if (e)
            for (uint32 guid : st.disenchant)
                if (Item* item = e->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid)))
                    yieldsQueued.push_back(item->GetTemplate()->DisenchantID);
        for (HeldItem const& h : st.held)
            if (h.purpose.rfind("disenchant", 0) == 0)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(h.itemId))
                    yieldsQueued.push_back(proto->DisenchantID);
    }
    for (auto const& [low, list] : withdraw_)
        for (auto const& [item, why] : list)
            if (why.find(": disenchant") != std::string::npos)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item))
                    yieldsQueued.push_back(proto->DisenchantID);
    for (uint32 id : yieldsQueued)
        if (id)
            for (auto const& [item, units] : DisenchantYield(id))
                if (auto it = demand.find(item); it != demand.end())
                    it->second -= units;
    return demand;
}

bool GuildCoordinator::DisenchantWorth(ItemTemplate const* proto, uint32 skill, std::map<uint32, double>& demand,
                                       std::string& why)
{
    if (proto && proto->DisenchantID)
        for (auto const& [item, units] : DisenchantYield(proto->DisenchantID))
            if (auto it = demand.find(item); it != demand.end() && it->second > 0.0)
            {
                it->second -= units;
                why = "to disenchant: needs " + ItemName(item);
                return true;
            }
    if (int32 const chance = DisenchantSkillUpChance(skill); chance > 0)
    {
        why = "to disenchant: skill-up chance " + std::to_string((chance + 5) / 10) + " %";
        return true;
    }
    why = "sell: no demand for its disenchant";
    return false;
}

// [bank409] b460: the only tab held 98/98, 72 of them greens for #22 Idunk's Disenchant, and none came out: the
// claims (withdraw_) live in memory only, a restart lost them, and every deposit since failed. Each plan the claims
// are rebuilt from the bank's items as the global leader's loot decision would make them (lock_ held)
void GuildCoordinator::ClaimBankItems()
{
    struct Taker
    {
        uint32 low{0};
        uint32 room{0};   // free bag slots less its queued withdrawals
        float vault{0};   // yd to its nearest vault
        uint32 skill{0};  // Enchanting with Disenchant, else 0
    };
    Guild* guild = nullptr;
    std::vector<Taker> takers;
    std::set<std::pair<uint32, uint32>> claims;  // (member, item)
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || !p->IsInWorld() || !GET_PLAYERBOT_AI(p) || HumanCharacter(p))
            continue;
        if (!guild)
            guild = GuildBankRun::BotGuild(p);
        for (GearPick const& g : bots_[row.low].gearNow)  // the gear loop's withdrawals, made after this pass
            if (g.kind == GuildGear::SRC_BANK && !p->GetItemCount(g.itemId, true))
                claims.insert({row.low, g.itemId});
        WorldPosition pos;
        float dist = 0;
        uint32 const queued = EnchanterInbound(row.low, p);  // [craft409] letters, held items, claims: bag slots
        uint32 const free = FreeBagSlots(p);
        if (!GuildBankRun::BankState(p, false).empty() || free <= queued || !GuildBankRun::NearestVault(p, pos, dist))
            continue;  // no rights / bag room / withdraw slots, or in an instance
        bool const enchanter = p->HasSkill(SKILL_ENCHANTING) && p->HasSpell(SPELL_DISENCHANT);
        takers.push_back({row.low, free - queued, dist, enchanter ? uint32(p->GetPureSkillValue(SKILL_ENCHANTING)) : 0u});
    }
    if (!guild || takers.empty())
        return;
    for (auto const& [low, list] : withdraw_)
        for (auto const& [item, why] : list)
            claims.insert({low, item});
    std::map<uint32, uint32> claimed;  // item -> copies claimed
    std::map<uint32, double> demand;   // [bank412] built at the first green
    bool demandReady = false;
    for (auto const& [low, item] : claims)
        ++claimed[item];
    for (uint8 tab = 0; tab < guild->GetPurchasedTabsSize(); ++tab)
        for (uint8 slot = 0; slot < GUILD_BANK_MAX_SLOTS; ++slot)
        {
            Item* item = guild->GetBankItem(tab, slot);
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto)
                continue;
            uint32 const entry = proto->ItemId;
            if (uint32& n = claimed[entry]; n)
            {
                --n;
                continue;
            }
            // the bank's own job: a crafter's wanted material, a cook's ingredient, a later gear plan's piece
            if (CookNeeds(entry) || PlannedFor(entry) ||
                std::any_of(wanted_.begin(), wanted_.end(),
                            [&](WantedItem const& w) { return w.itemId == entry && (w.need || w.optional); }))
                continue;
            bool const greens = (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
                                proto->DisenchantID && proto->Quality >= ITEM_QUALITY_UNCOMMON &&
                                proto->Quality <= ITEM_QUALITY_RARE && proto->RequiredDisenchantSkill >= 0;
            Taker* to = nullptr;
            if (greens)  // as [ench165]: the highest Disenchant skill-up chance, then the most room
                for (Taker& t : takers)
                    if (t.room && t.skill && t.skill >= uint32(proto->RequiredDisenchantSkill) &&
                        (!to || DisenchantSkillUpChance(t.skill) > DisenchantSkillUpChance(to->skill) ||
                         (DisenchantSkillUpChance(t.skill) == DisenchantSkillUpChance(to->skill) &&
                          t.room > to->room)))
                        to = &t;
            // [bank412] user rule 2026-11-12: to an enchanter only when its Disenchant's result is needed
            std::string worth = "sell: no enchanter's skill or bag room";
            bool disenchant = false;
            if (to)
            {
                if (!demandReady)
                {
                    demand = DisenchantDemand();
                    demandReady = true;
                }
                disenchant = DisenchantWorth(proto, to->skill, demand, worth);
                if (!disenchant)
                    to = nullptr;
            }
            if (!to)
                for (Taker& t : takers)
                    if (t.room && (!to || t.vault < to->vault))
                        to = &t;
            if (!to)
                return;  // nobody has bag room left
            --to->room;
            withdraw_[to->low].emplace_back(entry,
                                            std::string(UNCLAIMED_BANK) + (disenchant ? ": disenchant" : ": sell"));
            LOG_INFO("playerbots",
                     "[bank409] {} ×{} in the guild bank (tab {} slot {}): nobody claims it → {} takes it: {} "
                     "[bank412]",
                     ItemName(entry), item->GetCount(), uint32(tab) + 1, uint32(slot), Who(to->low),
                     disenchant ? worth : worth + " (the loot decision then)");
        }
}

void GuildCoordinator::RouteSpareGreens()
{
    auto const started = std::chrono::steady_clock::now();
    uint32 const now = Now();
    if (!greensCost_.since)
        greensCost_.since = now;
    uint32 scannedMembers = 0, scannedItems = 0, routed = 0;
    bool full = false;
    auto const account = [&]()
    {
        uint64 const us = uint64(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        GreensCost& c = greensCost_;
        ++c.sweeps;
        c.full += full ? 1 : 0;
        c.members += scannedMembers;
        c.items += scannedItems;
        c.routed += routed;
        c.us += us;
        c.maxUs = std::max(c.maxUs, us);
        if (now - c.since >= MINUTE)  // a log cadence, no decision
        {
            LOG_INFO("playerbots",
                     "[ench162] spare-green routing, last {} s: {} sweeps ({} full), {} members / {} bag items looked "
                     "at, {} routed, {} us ({} us max)",
                     now - c.since, c.sweeps, c.full, c.members, c.items, c.routed, c.us, c.maxUs);
            c = GreensCost();
            c.since = now;
        }
    };

    struct Enchanter
    {
        uint32 low{0};
        uint32 skill{0};
        uint32 queued{0};
        int32 chance{0};  // [ench165] its Disenchant's skill-up chance (per mille)
        uint32 inbound{0};  // [craft409] letters, items held for it, bank claims: each needs a free bag slot
        uint32 room{0};     // [craft409] its free bag slots
    };
    auto const mix = [](uint64 a, uint64 b)
    {
        uint64 x = (a << 32) ^ b;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return x;
    };
    uint64 signature = 0;  // a sum: the same whatever the maps' order
    std::vector<Enchanter> enchanters;
    std::unordered_set<uint32> online;
    for (PlanRow const& row : plan_)
        if (Player* m = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low)))
        {
            if (!m->IsInWorld() || !GET_PLAYERBOT_AI(m) || HumanCharacter(m))
                continue;  // a human's bags are his own
            online.insert(row.low);
            if (m->HasSkill(SKILL_ENCHANTING) && m->HasSpell(SPELL_DISENCHANT))
            {
                uint32 const skill = m->GetPureSkillValue(SKILL_ENCHANTING);
                enchanters.push_back(
                    {row.low, skill, uint32(bots_[row.low].disenchant.size()), DisenchantSkillUpChance(skill),
                     EnchanterInbound(row.low, m), FreeBagSlots(m)});
                enchanters.back().queued += enchanters.back().inbound;  // [craft409] its whole queue
                signature += mix(row.low, enchanters.back().skill);
            }
        }
    // the copies the members' gear plans take (one per member and item), the ones held for a member first
    std::map<uint32, uint32> planned;
    for (auto const& [low, st] : bots_)
    {
        std::set<uint32> ids;
        for (auto const* list : {&st.gearNow, &st.gearPlan})
            for (GearPick const& g : *list)
                ids.insert(g.itemId);
        for (uint32 id : ids)
        {
            ++planned[id];
            signature += mix(low, id) ^ 1;
        }
    }

    std::unordered_set<uint32> dirty;
    {
        std::lock_guard<std::mutex> guard(bagsChangedLock_);
        dirty.swap(bagsChanged_);
    }
    if (signature != greensSignature_)
    {
        greensSignature_ = signature;
        full = true;
        dirty = online;
    }
    for (uint32 low : online)
        if (!greensOnline_.count(low))
            dirty.insert(low);  // it came online: its bags may have changed meanwhile
    greensOnline_ = online;
    for (auto it = dirty.begin(); it != dirty.end();)
        it = online.count(*it) ? std::next(it) : dirty.erase(it);
    if (enchanters.empty() || dirty.empty())
    {
        account();
        return;
    }

    for (auto const& [low, st] : bots_)
        for (HeldItem const& h : st.held)
        {
            if (h.purpose.rfind("disenchant", 0) == 0)
                continue;  // in its enchanter's inbound [craft409]
            if (auto const p = planned.find(h.itemId); p != planned.end() && p->second)
                --p->second;
        }
    for (auto const& [low, kept] : greensPlanned_)
        if (!dirty.count(low))
            for (uint32 id : kept)
                if (auto const p = planned.find(id); p != planned.end() && p->second)
                    --p->second;

    std::vector<uint32> again;  // a green still waiting for its loot decision: looked at once it is decided
    std::map<uint32, double> demand;  // [craft409] built at the first routed green of the sweep
    bool demandReady = false;
    for (PlanRow const& row : plan_)
    {
        if (!dirty.count(row.low))
            continue;
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p)
            continue;
        ++scannedMembers;
        BotState& st = bots_[row.low];
        std::vector<uint32>& kept = greensPlanned_[row.low];
        kept.clear();
        std::optional<StatsWeightCalculator> calc;
        auto const look = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto)
                return;
            ++scannedItems;
            if (item->IsSoulBound() || !proto->DisenchantID || proto->Quality < ITEM_QUALITY_UNCOMMON ||
                proto->Quality > ITEM_QUALITY_RARE ||
                (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON) ||
                proto->RequiredDisenchantSkill < 0)
                return;
            uint32 const guid = item->GetGUID().GetCounter();
            auto const held =
                std::find_if(st.held.begin(), st.held.end(), [&](HeldItem const& h) { return h.itemGuidLow == guid; });
            if (held != st.held.end() && held->purpose == "pending")
                again.push_back(row.low);
            if (held != st.held.end() ||
                std::find(st.disenchant.begin(), st.disenchant.end(), guid) != st.disenchant.end() ||
                TransferHolds(guid))
                return;
            if (auto const pl = planned.find(proto->ItemId); pl != planned.end() && pl->second)
            {
                --pl->second;  // this copy is a member's upgrade: kept
                kept.push_back(proto->ItemId);
                return;
            }
            uint32 const req = uint32(proto->RequiredDisenchantSkill);
            // its own Disenchant first (no transfer), else the highest skill-up chance of the Disenchant, the shortest
            // list among equal chances, then the lowest skill. [ench165] b443: by the lowest skill alone the three
            // enchanters at 8-9 took all 123 greens while Emdyl and Manue (11, the same orange chance) got none and
            // stayed "cannot level now: Strange Dust x1"
            Enchanter* to = nullptr;
            for (Enchanter& e : enchanters)
                if (e.skill >= req)
                {
                    if (e.low == row.low)
                    {
                        to = &e;
                        break;
                    }
                    // [craft409] b461 #6 Emdyl: 21 letters waiting, full bags, 1 Disenchant in the log while the
                    // router counted only its held items. Its whole queue counts now, and one more needs a bag slot
                    if (e.room <= e.inbound)
                        continue;
                    if (!to || e.chance > to->chance ||
                        (e.chance == to->chance &&
                         (e.queued < to->queued || (e.queued == to->queued && e.skill < to->skill))))
                        to = &e;
                }
            if (!to)
                return;  // no enchanter's skill disenchants it yet (a skill-up changes the signature: all again)
            if (!calc)
            {
                calc.emplace(p);
                calc->SetItemSetBonus(false);
                calc->SetOverflowPenalty(false);
            }
            GuildGear::Eval ev;
            if (GuildGear::Evaluate(p, *calc, proto, ev) && ev.gain > 0.0f)
                return;  // the holder wears it (its own equip takes it; the equip marks its bags changed)
            // [craft409] user rule 2026-11-12: disenchanted only for a needed result, else sold by its holder
            if (!demandReady)
            {
                demand = DisenchantDemand();
                demandReady = true;
            }
            std::string why;
            if (!DisenchantWorth(proto, to->skill, demand, why))
            {
                if (std::find(st.sell.begin(), st.sell.end(), guid) == st.sell.end())
                {
                    st.sell.push_back(guid);
                    LOG_INFO("playerbots", "[craft409] spare {} (at {}): {} (Enchanting {})", proto->Name1,
                             Who(row.low), why, to->skill);
                }
                return;
            }
            st.sell.erase(std::remove(st.sell.begin(), st.sell.end(), guid), st.sell.end());
            ++to->queued;
            to->inbound += to->low == row.low ? 0 : 1;
            ++routed;
            if (to->low == row.low)
                st.disenchant.push_back(guid);
            else
                st.held.push_back({guid, proto->ItemId, item->GetCount(), to->low, "disenchant " + Who(to->low)});
            LOG_INFO("playerbots",
                     "[ench161] spare {} (at {}) -> {} (Enchanting {}, needs {}): no member's gear plan takes this "
                     "copy, {} [craft409]",
                     proto->Name1, Who(row.low), to->low == row.low ? std::string("its own Disenchant") : Who(to->low),
                     to->skill, req, why);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            look(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    look(bag->GetItemByPos(uint8(j)));
    }
    if (!again.empty())
    {
        std::lock_guard<std::mutex> guard(bagsChangedLock_);
        bagsChanged_.insert(again.begin(), again.end());
    }
    account();
}

// ---------------------------------------------------------------------------------------------------------------------
// [tasks] profession vs questing: only the global leader decides. Lag = the skill the plan needs at the member's level
// (the trainer rank cap its level allows; the upgrade / bag crafts nobody in the guild can make yet) minus its skill.
// The leader walks the skill-up plan up from the skill: reagents from its bags and the guild bank, vendor reagents for
// copper, its own gathering at the measured gathering pace; it stops where a reagent only comes from others. Value of
// the reached part (copper): the products' vendor price + the unlocked items' price - the vendor reagents; time: the
// casts + the gathering. Questing value: [questrate] the formula over its quests (QuestPace). The higher one is what
// the member does.
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::RecordPace(BotState& st, Player* p) const
{
    double money = 0.0, xp = 0.0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 const questId = p->GetQuestSlotQuestId(slot))
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            {
                xp += quest->XPValue(p->GetLevel());
                money += quest->GetRewMoneyMaxLevel();
            }
    float const rate = sWorld->getRate(RATE_XP_QUEST);
    if (xp > 0.0 && rate > 0.0f)
        st.copperPerXp = float(money / (xp * rate));
}

// [questrate] world thread, startup: the static quest table (C:/WoW/tools/quest_audit.py writes it; the SQL update
// 2026_11_08_02 applies it) and, per quest objective, the health per unit of its cheapest source creature (the core's
// base health at the creature's middle level; an item drop over its chance in creature_loot_template)
void GuildCoordinator::LoadQuestValue()
{
    questStatic_.clear();
    questsByMap_.clear();
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT quest, doable, map, area_x, area_y, giver_x, giver_y, ender_x, ender_y, tour_yd, ideal_yd, flags "
        "FROM playerbots_quest_value_static");
    if (!result)
    {
        LOG_ERROR("playerbots", "[questrate] playerbots_quest_value_static is empty or missing: questing has no value "
                                "formula, the leader keeps every member on its quests");
        return;
    }
    std::vector<std::pair<uint32, uint32>> byLevel;  // (min level, quest) of the takeable ones
    do
    {
        Field* f = result->Fetch();
        uint32 const questId = f[0].Get<uint32>();
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        int16 const map = f[2].Get<int16>();
        if (!quest || map < 0)  // no spawn of its giver, objective or ender anywhere
            continue;
        std::string const doable = f[1].IsNull() ? std::string() : f[1].Get<std::string>();
        std::string const flags = f[11].IsNull() ? std::string() : f[11].Get<std::string>();
        QuestStatic q;
        q.map = uint16(map);
        q.areaX = float(f[3].Get<int32>());
        q.areaY = float(f[4].Get<int32>());
        q.giver = !f[5].IsNull() && !f[6].IsNull();
        q.ender = !f[7].IsNull() && !f[8].IsNull();
        if (q.giver)
        {
            q.giverX = float(f[5].Get<int32>());
            q.giverY = float(f[6].Get<int32>());
        }
        if (q.ender)
        {
            q.enderX = float(f[7].Get<int32>());
            q.enderY = float(f[8].Get<int32>());
        }
        q.tourYd = float(f[9].Get<uint32>());
        q.idealYd = float(f[10].Get<uint32>());
        // a quest it may pick up: the audit found the data to do it alone (class / profession quests: the core's
        // CanTakeQuest checks the class and the skill), no world event, a giver and an ender
        bool const flagged = flags.find("event") != std::string::npos ||
                             flags.find("seasonal") != std::string::npos ||
                             flags.find("no_giver") != std::string::npos ||
                             flags.find("no_ender") != std::string::npos;
        q.takeable = (doable == "yes" || doable == "class" || doable == "profession") && !flagged && q.giver && q.ender;
        questStatic_[questId] = q;
        if (q.takeable)
            byLevel.emplace_back(quest->GetMinLevel(), questId);
    } while (result->NextRow());
    std::sort(byLevel.begin(), byLevel.end());
    for (auto const& [minLevel, questId] : byLevel)
        questsByMap_[questStatic_[questId].map].push_back(questId);

    // the objectives' sources: kill credit -> creatures, quest item -> creatures that drop it (with the chance)
    std::unordered_map<uint32, float> creditHp;  // credit entry -> the lowest health of a creature crediting it
    std::unordered_map<uint32, float> itemHp;    // quest item -> the lowest health / chance of a creature dropping it
    for (auto const& [questId, q] : questStatic_)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
        {
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] > 0)
                    creditHp.emplace(uint32(quest->RequiredNpcOrGo[i]), 0.0f);
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i])
                    itemHp.emplace(quest->RequiredItemId[i], 0.0f);
        }
    auto const lower = [](float& slot, float value)
    {
        if (value > 0.0f && (slot <= 0.0f || value < slot))
            slot = value;
    };
    for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
    {
        uint8 const level = uint8((uint32(tmpl.minlevel) + uint32(tmpl.maxlevel)) / 2);
        CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, tmpl.unit_class);
        float const hp = stats ? float(stats->GenerateHealth(&tmpl)) : 0.0f;
        if (hp <= 0.0f)
            continue;
        for (uint32 const credit : {tmpl.Entry, tmpl.KillCredit[0], tmpl.KillCredit[1]})
            if (auto const c = credit ? creditHp.find(credit) : creditHp.end(); c != creditHp.end())
                lower(c->second, hp);
        LootTemplate const* loot = tmpl.lootid ? LootTemplates_Creature.GetLootFor(tmpl.lootid) : nullptr;
        if (!loot)
            continue;
        loot->VisitEntries(
            [&](LootStoreItem const& item, float chance)
            {
                auto const i = item.reference ? itemHp.end() : itemHp.find(item.itemid);
                float const p = std::min(std::abs(chance) / 100.0f, 1.0f);
                if (i != itemHp.end() && p > 0.0f)
                    lower(i->second, hp / p);
            });
    }
    for (auto& [questId, q] : questStatic_)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
        {
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] > 0)
                    q.npcHp[i] = creditHp[uint32(quest->RequiredNpcOrGo[i])];
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i])
                    q.itemHp[i] = itemHp[quest->RequiredItemId[i]];
        }
    LOG_INFO("playerbots",
             "[questrate] quest value: {} quests ({} takeable on {} maps), {} kill credits, {} quest items with a "
             "creature source",
             questStatic_.size(), byLevel.size(), questsByMap_.size(),
             std::count_if(creditHp.begin(), creditHp.end(), [](auto const& c) { return c.second > 0.0f; }),
             std::count_if(itemHp.begin(), itemHp.end(), [](auto const& c) { return c.second > 0.0f; }));
}

// [questrate] the questing value by formula (user 2026-11-08: no measurements). Per quest:
//   value (copper) = Quest::XPValue(level) x Rate.XP.Quest x its copper per XP (the core's max-level quest money,
//                    RecordPace) + GetRewOrReqMoney(level) + the reward items' sell price (+ the best choice's)
//   time (s)       = the walk / its run speed + the kills left x the source's health / its EstimateDps (solo)
//   walk           = in its log, incomplete: to the objective area + the tour's part still to do + area -> ender;
//                    complete: to the ender; one it may take: to the giver + giver -> tour -> ender (ideal_yd)
// Its log quests together (the work it is on: their value over their time); an empty or unpriced log: the best quest
// it may take now on its map.
// [questvalue] user 2026-11-08: a quest in the log that turned green or grey for the bot's level is valued again
// (on a level-up and on login): its value by the questing formula (quest XP x the realm's rate at its level at the
// bot's copper per XP + money + the reward items' price) over the time it still takes (the walk to its area, the
// tour share left, the kills, the walk to its ender) against the route's own rate (the yellow / orange quests of its
// log, else the best quest it may take). Kept when worth it, or when a reward is an upgrade in some member's gear
// plan, or when it leads to a follow-up that is not grey; else abandoned (the core's CMSG_QUESTLOG_REMOVE_QUEST)
void GuildCoordinator::RevalueQuests(BotState& st, Player* p)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
    if (!ai || HumanCharacter(p) || questStatic_.empty() || NewRpgCoop::InParty(p->GetGUID().GetCounter()))
        return;
    // [quest120] inside a dungeon or a battleground every open-world quest sits on another map: the off-map rule
    // below would read the whole log as unreachable, so the log is valued in the open world only
    if (!p->GetMap() || p->GetMap()->Instanceable())
        return;
    uint8 const level = p->GetLevel();
    float const rate = sWorld->getRate(RATE_XP_QUEST);
    float const speed = std::max(p->GetSpeed(MOVE_RUN), 1.0f);
    float const dps = DpsEstimate::EstimateDps(p, nullptr, DpsEstimate::BuffContext::Solo);
    float const x = p->GetPositionX(), y = p->GetPositionY();
    struct Row
    {
        uint16 slot;
        Quest const* quest;
        XPColorChar color;
        double copper;
        double seconds;
        bool priced;
        bool offMap;  // [quest120] its ender sits on a map the bot is not on: no route from here reaches it
        uint16 onMap;  // the map its ender sits on, QuestStatic::map's own type (0 when it is not known)
        QuestStatus status;
    };
    std::vector<Row> rows;
    double routeCopper = 0.0, routeSeconds = 0.0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = p->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        QuestStatus const status = p->GetQuestStatus(questId);
        XPColorChar const color = Acore::XP::GetColorCode(level, uint8(std::max<int32>(1, p->GetQuestLevel(quest))));
        // [econ1] user rule 2026-11-08: a quest is worth its XP (the core's quest XP at its level x the realm's
        // rate) per minute, plus a reward that is an upgrade (kept below); no copper (b350: Rumanatol's route rate
        // 21527 c/min from a complete quest next to its ender, 93 green quests abandoned)
        double const copper = double(quest->XPValue(level)) * rate;  // XP (the field keeps its name)
        // the time it still takes, by the static table (no row: not priced, kept)
        bool priced = false;
        double seconds = 0.0;
        auto const q = questStatic_.find(questId);
        // [quest120] b475 Eraelor held q9683 (Bloodmyst Isle, map 530) and q384 (map 0) for days on map 1: a quest
        // whose ender is on another map was never priced at all, so it never met the rate and never left the log
        bool const offMap = q != questStatic_.end() && q->second.ender && q->second.map != p->GetMapId();
        if (q != questStatic_.end() && q->second.map == p->GetMapId() && q->second.ender)
        {
            QuestStatic const& s = q->second;
            if (status == QUEST_STATUS_COMPLETE)
            {
                seconds = std::hypot(x - s.enderX, y - s.enderY) / speed;
                priced = true;
            }
            else if (status == QUEST_STATUS_INCOMPLETE && dps > 0.0f)
            {
                auto const data = p->getQuestStatusMap().find(questId);
                QuestStatusData const* st2 = data == p->getQuestStatusMap().end() ? nullptr : &data->second;
                uint32 left = 0, total = 0;
                double kills = 0.0;
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    if (uint32 const need = quest->RequiredNpcOrGoCount[i]; need && quest->RequiredNpcOrGo[i])
                    {
                        uint32 const done = st2 ? std::min<uint32>(st2->CreatureOrGOCount[i], need) : 0;
                        total += need;
                        left += need - done;
                        kills += double(need - done) * s.npcHp[i] / dps;
                    }
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (uint32 const need = quest->RequiredItemCount[i]; need && quest->RequiredItemId[i])
                    {
                        uint32 const done = st2 ? std::min<uint32>(st2->ItemCount[i], need) : 0;
                        total += need;
                        left += need - done;
                        kills += double(need - done) * s.itemHp[i] / dps;
                    }
                double const part = total ? double(left) / double(total) : 1.0;
                double const walk = std::hypot(x - s.areaX, y - s.areaY) + s.tourYd * part +
                                    std::hypot(s.areaX - s.enderX, s.areaY - s.enderY);
                seconds = walk / speed + kills;
                priced = true;
            }
        }
        rows.push_back({slot, quest, color, copper, seconds, priced, offMap,
                        uint16(q == questStatic_.end() ? 0 : q->second.map), status});
        // [econ1] the route's rate: the yellow / orange quests it still works; a complete one is handed in either
        // way (its XP comes with or without the green quest) and is no measure of what the time is worth
        if (priced && color != XP_GRAY && color != XP_GREEN && status == QUEST_STATUS_INCOMPLETE)
        {
            routeCopper += copper;
            routeSeconds += seconds;
        }
    }
    float routeRate = 0.0f;  // XP per minute
    std::string routeWhy;
    float unusedCopper = 0.0f;
    if (routeSeconds > 0.0)
    {
        routeRate = float(routeCopper * MINUTE / routeSeconds);
        routeWhy = "incomplete yellow / orange quests of its log";
    }
    else if (QuestPace(st, p, unusedCopper, &routeWhy, &routeRate))
        routeWhy = "the best quest it may take (" + routeWhy + ")";
    else
        routeWhy.clear();  // nothing to compare with: only the off-map rule below may still speak
    bool const haveRate = !routeWhy.empty();
    for (Row const& r : rows)
    {
        // [quest14] a grey quest is no XP/min question: the route's grey rule (map thread, GreyQuestNeed) keeps or
        // abandons it
        if (r.color == XP_GRAY)
            continue;
        if (r.offMap)
        {
            // [quest120] no route leaves the bot's map for it, so its worth here is 0 XP/min: it stays only for the
            // same needs a grey quest stays for (a chain step, an upgrade, a recipe, a skill, an attunement, a key,
            // a wanted item) - the global leader's call, GreyQuestNeed
            uint32 const offId = r.quest->GetQuestId();
            if (r.status == QUEST_STATUS_COMPLETE)
                continue;  // its work is done: the hand-in waits for the map, dropping it would throw the XP away
            std::string const need = GreyQuestNeed(p, r.quest);
            if (!need.empty())
            {
                LOG_INFO("playerbots",
                         "[quest] keep {} L{} q{} t={} at=({:.0f},{:.0f}) reason: its ender is on map {}, the bot "
                         "quests on map {}, but {} [quest120]",
                         p->GetName(), level, offId, QuestStats::NowMs(), x, y, r.onMap, p->GetMapId(), need);
                continue;
            }
            std::string const why = Acore::StringFormat(
                "its ender is on map {}, the bot quests on map {}: no route from here works it (0 XP/min)", r.onMap,
                p->GetMapId());
            LOG_INFO("playerbots", "[quest] abandon {} L{} q{} t={} at=({:.0f},{:.0f}) reason: {} [quest120]",
                     p->GetName(), level, offId, QuestStats::NowMs(), x, y, why);
            st.questDropped[offId] = why;
            auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
            if (doing && doing->questId == offId)
                ai->rpgInfo.ChangeToIdle();
            WorldPacket offPacket(CMSG_QUESTLOG_REMOVE_QUEST);
            offPacket << uint8(r.slot);
            WorldPackets::Quest::QuestLogRemoveQuest offRemove(std::move(offPacket));
            offRemove.Read();
            p->GetSession()->HandleQuestLogRemoveQuest(offRemove);
            continue;
        }
        if (r.color != XP_GREEN || !r.priced || !haveRate)
            continue;
        uint32 const questId = r.quest->GetQuestId();
        float const value = float(r.copper * MINUTE / std::max(r.seconds, 1.0));
        std::string keep;
        for (uint8 i = 0; i < QUEST_REWARDS_COUNT + QUEST_REWARD_CHOICES_COUNT && keep.empty(); ++i)
        {
            uint32 const item = i < QUEST_REWARDS_COUNT ? r.quest->RewardItemId[i]
                                                        : r.quest->RewardChoiceItemId[i - QUEST_REWARDS_COUNT];
            if (uint32 const forLow = item ? PlannedFor(item) : 0)
                keep = "reward " + ItemName(item) + " is an upgrade in " + Who(forLow) + "'s gear plan";
        }
        for (uint32 next : {r.quest->GetNextQuestInChain(), r.quest->GetNextQuestId()})
            if (Quest const* follow = keep.empty() && next ? sObjectMgr->GetQuestTemplate(next) : nullptr)
                if (Acore::XP::GetColorCode(level, uint8(std::max<int32>(1, p->GetQuestLevel(follow)))) != XP_GRAY)
                    keep = "it leads to q" + std::to_string(next) + " (not grey)";
        if (keep.empty() && value >= routeRate)
            keep = Acore::StringFormat("{:.0f} XP/min >= route rate {:.0f} XP/min", value, routeRate);
        char const* colorName = r.color == XP_GRAY ? "grey" : "green";
        if (!keep.empty())
        {
            LOG_INFO("playerbots", "[quest] keep {} L{} q{} t={} at=({:.0f},{:.0f}) reason: {}, {}", p->GetName(),
                     level, questId, QuestStats::NowMs(), x, y, colorName, keep);
            continue;
        }
        LOG_INFO("playerbots",
                 "[quest] abandon {} L{} q{} t={} at=({:.0f},{:.0f}) reason: {}, {:.0f} XP/min < route rate {:.0f} "
                 "XP/min (route: {}) [econ1]",
                 p->GetName(), level, questId, QuestStats::NowMs(), x, y, colorName, value, routeRate, routeWhy);
        // [quest120] b475 took q1010 back at a giver 25 min after this abandon (8 such retakes in half an hour of
        // log): the verdict is remembered, and a quest's XP only falls as the bot levels, so it holds
        st.questDropped[questId] = Acore::StringFormat("{:.0f} XP/min < route rate {:.0f} XP/min when it was dropped",
                                                       value, routeRate);
        if (auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data); doing && doing->questId == questId)
            ai->rpgInfo.ChangeToIdle();
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << uint8(r.slot);
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        p->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
    }
    LogQuestLog(st, p);
}

// [quest120] the quest log as the client shows it, after the valuation: one [qlog] line per bot, logged again only
// when it changes (C:/WoW/tools/world_triage.py, batch_triage.py read it). "since" is the time the quest's own
// objective counts last moved - the core keeps no such time, so it is counted from the first census that saw it, and
// a quest that never moves carries the age of the bot's session
void GuildCoordinator::LogQuestLog(BotState& st, Player* p)
{
    if (!p)
        return;
    uint8 const level = p->GetLevel();
    uint64 const now = QuestStats::NowMs();
    uint32 held = 0, grey = 0, green = 0, high = 0, complete = 0, offMap = 0, idle = 0, wasted = 0;
    uint32 earnedAway = 0;  // [quest121] complete, its ender on another map: earned XP only a crossing collects
    uint64 oldest = 0;
    uint32 oldestQuest = 0;
    std::string quests;
    std::set<uint32> inLog;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = p->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        inLog.insert(questId);
        ++held;
        int32 const questLevel = std::max<int32>(1, p->GetQuestLevel(quest));
        XPColorChar const color = Acore::XP::GetColorCode(level, uint8(questLevel));
        QuestStatus const status = p->GetQuestStatus(questId);
        uint64 print = uint64(status);
        uint32 done = 0;
        if (auto const data = p->getQuestStatusMap().find(questId); data != p->getQuestStatusMap().end())
        {
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                print = print * 101 + data->second.CreatureOrGOCount[i];
                done += data->second.CreatureOrGOCount[i];
            }
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                print = print * 101 + data->second.ItemCount[i];
                done += data->second.ItemCount[i];
            }
        }
        auto& seen = st.questSeen[questId];
        if (seen.second == 0 || seen.first != print)
            seen = {print, now};
        uint64 const sinceMs = now - seen.second;
        if (color == XP_GRAY)
            ++grey;
        else if (color == XP_GREEN)
            ++green;
        else
            ++high;
        if (status == QUEST_STATUS_COMPLETE)
            ++complete;
        bool const away = [&]
        {
            auto const q = questStatic_.find(questId);
            return q != questStatic_.end() && q->second.ender && q->second.map != p->GetMapId();
        }();
        if (away)
            ++offMap;
        // [quest121] complete and off its map: XP already earned that only this bot's crossing can collect. The
        // [quest120] guard keeps the slot on purpose, so this count must fall when the bot reaches that map - one
        // that never falls is a hand-in the route never places
        if (away && status == QUEST_STATUS_COMPLETE)
            ++earnedAway;
        // nothing done on it since it was taken, and it is one no route of this map works: a slot held for nothing
        bool const untouched = status != QUEST_STATUS_COMPLETE && done == 0;
        if (untouched)
            ++idle;
        if (untouched && (color == XP_GRAY || away))
            ++wasted;
        if (sinceMs > oldest)
        {
            oldest = sinceMs;
            oldestQuest = questId;
        }
        quests += Acore::StringFormat(" | q{} L{} {} {} since={}s", questId, questLevel,
                                      color == XP_GRAY     ? "grey"
                                      : color == XP_GREEN  ? "green"
                                      : color == XP_YELLOW ? "yellow"
                                                           : "orange/red",
                                      status == QUEST_STATUS_COMPLETE ? "complete" : "incomplete", sinceMs / 1000);
    }
    for (auto itr = st.questSeen.begin(); itr != st.questSeen.end();)
        itr = inLog.count(itr->first) ? std::next(itr) : st.questSeen.erase(itr);
    std::string const line = Acore::StringFormat(
        "[qlog] {} L{} held={}/{} grey={} green={} yellow+={} complete={} earned-off-map={} off-map={} "
        "no-progress={} wasted={} free={} longest-idle={}s q{}{}",
        p->GetName(), level, held, uint32(MAX_QUEST_LOG_SIZE), grey, green, high, complete, earnedAway, offMap,
        idle, wasted, uint32(MAX_QUEST_LOG_SIZE) - held, oldest / 1000, oldestQuest, quests);
    if (line == st.qlogLine)
        return;
    st.qlogLine = line;
    LOG_INFO("playerbots", "{}", line);
}

bool GuildCoordinator::QuestPace(BotState& st, Player* p, float& copperPerMinute, std::string* why,
                                 float* xpPerMinute) const
{
    if (!p || questStatic_.empty())
        return false;
    uint8 const level = p->GetLevel();
    float const rate = sWorld->getRate(RATE_XP_QUEST);
    float const speed = std::max(p->GetSpeed(MOVE_RUN), 1.0f);
    float const dps = DpsEstimate::EstimateDps(p, nullptr, DpsEstimate::BuffContext::Solo);
    uint32 const map = p->GetMapId();
    float const x = p->GetPositionX(), y = p->GetPositionY();
    auto const dist = [](float ax, float ay, float bx, float by) { return double(std::hypot(ax - bx, ay - by)); };
    // [econ1] user rule 2026-11-08: questing is worth its XP (the guild's levelling), not its copper. The pace is
    // XP per minute; the copper figure the profession / gathering comparisons use is that XP at the bot's own quest
    // money per XP (RecordPace): the same XP, in the unit of the material values
    auto const xpOf = [&](Quest const* quest) { return double(quest->XPValue(level)) * rate; };
    auto const perXpOf = [&](Quest const* quest)
    {
        double const xp = xpOf(quest);
        if (st.copperPerXp > 0.0f || xp <= 0.0)
            return double(st.copperPerXp);
        return double(quest->GetRewMoneyMaxLevel()) / xp;  // nothing measured yet: its own max-level money
    };
    auto const worth = [&](Quest const* quest) { return xpOf(quest) * perXpOf(quest); };
    // the kill seconds of the units left (all of them for a quest it has not taken) and the units left / in all;
    // false: kills it cannot price (no damage)
    auto const killSeconds = [&](Quest const* quest, QuestStatic const& q, QuestStatusData const* status,
                                 double& seconds, uint32& left, uint32& total)
    {
        seconds = 0.0;
        left = total = 0;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            uint32 const need = quest->RequiredNpcOrGoCount[i];
            if (!need || !quest->RequiredNpcOrGo[i])
                continue;
            uint32 const done = status ? std::min<uint32>(status->CreatureOrGOCount[i], need) : 0;
            total += need;
            left += need - done;
            if (q.npcHp[i] > 0.0f && need > done)
            {
                if (dps <= 0.0f)
                    return false;
                seconds += double(need - done) * q.npcHp[i] / dps;
            }
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const need = quest->RequiredItemCount[i];
            if (!need || !quest->RequiredItemId[i])
                continue;
            uint32 const done = status ? std::min<uint32>(status->ItemCount[i], need) : 0;
            total += need;
            left += need - done;
            if (q.itemHp[i] > 0.0f && need > done)
            {
                if (dps <= 0.0f)
                    return false;
                seconds += double(need - done) * q.itemHp[i] / dps;
            }
        }
        return true;
    };

    // its log
    double logCopper = 0.0, logSeconds = 0.0, logXp = 0.0;
    uint32 logQuests = 0, inLog = 0;
    uint32 handIns = 0;  // [quest121] the complete quests of the log: earned XP that only a trip to the ender costs
    double handInXp = 0.0, handInSeconds = 0.0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = p->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        ++inLog;
        QuestStatus const status = p->GetQuestStatus(questId);
        auto const q = questStatic_.find(questId);
        // [quest121] b475: 323 of the 897 quests the guild held were complete and never handed in, 103 of them with
        // their ender on the bot's own map, waiting 37.8 h on average. The old rule here dropped a complete quest
        // ("it is handed in on the way whatever the bot does", b350 Rumanatol's 21527 c/min), so the earned XP sat
        // in no rate and the trip to collect it never competed with a gathering run, a craft or a dungeon order.
        // It is priced like any other stop instead: its XP over the travel seconds to its ender, one term of the
        // same average (the money unit that blew up in b350 is gone - the pace is XP now, and the quest's own
        // seconds go into the denominator with it)
        if (q == questStatic_.end() || q->second.map != map || !q->second.ender ||
            (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE))
            continue;
        // [quest14] a grey quest counts toward no rate
        if (IsGreyQuest(p, quest))
            continue;
        // [flit] a log quest the bot does not work (only a tool's use or a spell credits it) is no questing value
        if (status == QUEST_STATUS_INCOMPLETE &&
            (NewRpgBaseAction::NeedsItemUseCredit(quest) || NewRpgBaseAction::NeedsSpellCredit(p, quest, true)))
            continue;
        QuestStatic const& s = q->second;
        double walk = 0.0, kills = 0.0;
        if (status == QUEST_STATUS_COMPLETE)
            walk = dist(x, y, s.enderX, s.enderY);
        else
        {
            auto const data = p->getQuestStatusMap().find(questId);
            uint32 left = 0, total = 0;
            if (!killSeconds(quest, s, data == p->getQuestStatusMap().end() ? nullptr : &data->second, kills, left,
                             total))
                continue;
            double const part = total ? double(left) / double(total) : 1.0;
            walk = dist(x, y, s.areaX, s.areaY) + s.tourYd * part + dist(s.areaX, s.areaY, s.enderX, s.enderY);
        }
        logCopper += worth(quest);
        logXp += xpOf(quest);
        logSeconds += walk / speed + kills;
        ++logQuests;
        if (status == QUEST_STATUS_COMPLETE)  // [quest121] XP already earned, the trip to its ender is all it costs
        {
            ++handIns;
            handInXp += xpOf(quest);
            handInSeconds += walk / speed;
        }
    }
    if (logQuests)
    {
        double const seconds = std::max(logSeconds, 1.0);
        copperPerMinute = float(logCopper * MINUTE / seconds);
        if (xpPerMinute)
            *xpPerMinute = float(logXp * MINUTE / seconds);
        if (why)
        {
            *why = Acore::StringFormat("formula: {} log quests, {:.0f} XP in {:.0f} s ({:.0f} XP/min)", logQuests,
                                       logXp, logSeconds, logXp * MINUTE / seconds);
            if (handIns)  // [quest121] the earned part of that rate: what a hand-in trip alone would collect
                *why += Acore::StringFormat("; {} of them complete, {:.0f} XP earned for {:.0f} s of travel ({:.0f} "
                                            "XP/min)",
                                            handIns, handInXp, handInSeconds,
                                            handInXp * MINUTE / std::max(handInSeconds, 1.0));
        }
        return true;
    }

    // the quests of its map it may take now (CanTakeQuest, kept until its level, map or quest counts change)
    uint64 const key = (uint64(level) << 56) | (uint64(map & 0xFFFF) << 40) |
                       (uint64(p->getRewardedQuests().size() & 0xFFFFFF) << 16) | uint64(inLog & 0xFFFF);
    if (st.questAvailKey != key)
    {
        st.questAvailKey = key;
        st.questAvail.clear();
        if (auto const list = questsByMap_.find(map); list != questsByMap_.end())
            for (uint32 const questId : list->second)
            {
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest || quest->GetMinLevel() > level)
                    break;  // sorted by min level
                // [quest14] a grey quest counts toward no rate
                if (p->GetQuestStatus(questId) == QUEST_STATUS_NONE && !IsGreyQuest(p, quest) &&
                    p->CanTakeQuest(quest, false))
                    st.questAvail.push_back(questId);
            }
    }
    float best = -1.0f;
    uint32 bestQuest = 0;
    double bestCopper = 0.0, bestSeconds = 0.0, bestXp = 0.0;
    for (uint32 const questId : st.questAvail)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        auto const q = questStatic_.find(questId);
        double kills = 0.0;
        uint32 left = 0, total = 0;
        if (!quest || q == questStatic_.end() || !killSeconds(quest, q->second, nullptr, kills, left, total))
            continue;
        double const seconds = (dist(x, y, q->second.giverX, q->second.giverY) + q->second.idealYd) / speed + kills;
        double const copper = worth(quest);
        float const perMinute = float(xpOf(quest) * MINUTE / std::max(seconds, 1.0));  // [econ1] the best by XP
        if (perMinute > best)
        {
            best = perMinute;
            bestQuest = questId;
            bestCopper = copper;
            bestSeconds = seconds;
            bestXp = xpOf(quest);
        }
    }
    if (!bestQuest)
        return false;
    copperPerMinute = float(bestCopper * MINUTE / std::max(bestSeconds, 1.0));
    if (xpPerMinute)
        *xpPerMinute = best;
    if (why)
        *why = Acore::StringFormat("formula: best available q{}, {:.0f} XP in {:.0f} s ({:.0f} XP/min)", bestQuest,
                                   bestXp, bestSeconds, best);
    return true;
}

float GuildCoordinator::GatherPace(uint32 skill) const
{
    uint64 units = 0, seconds = 0;
    if (auto const u = gatherUnits_.find(skill); u != gatherUnits_.end())
        for (auto const& [at, n] : u->second)
            units += n;
    if (auto const t = gatherTime_.find(skill); t != gatherTime_.end())
        for (auto const& [at, s] : t->second)
            seconds += s;
    return seconds ? float(double(units) * MINUTE / double(seconds)) : -1.0f;
}

uint32 GuildCoordinator::TrainerCap(uint32 skillLine, uint8 level) const
{
    uint32 cap = 0;
    if (auto const ranks = trainerRanks_.find(skillLine); ranks != trainerRanks_.end())
        for (TrainerRank const& r : ranks->second)
            if (r.reqLevel <= level)
                cap = std::max(cap, r.maxSkill);
    return cap;
}

// [craft435] the slots a craft fills for a member: both empty slots of a ring / trinket pair when the piece is not
// unique, else one. b469 one ring pick per member (the group's best) counted 75 of the guild's 150 finger slots
static uint32 PairSlotsFilled(Player* m, ItemTemplate const* proto, uint8 group)
{
    uint8 first = 0;
    if (group == GuildGear::EquipGroup(INVTYPE_FINGER))
        first = EQUIPMENT_SLOT_FINGER1;
    else if (group == GuildGear::EquipGroup(INVTYPE_TRINKET))
        first = EQUIPMENT_SLOT_TRINKET1;
    else
        return 1;
    if (proto->MaxCount == 1)
        return 1;
    uint32 empty = 0;
    for (uint8 slot = first; slot <= first + 1; ++slot)
        empty += m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) ? 0 : 1;
    return std::max<uint32>(1, empty);
}

void GuildCoordinator::UpdateProfessions()
{
    if (plan_.empty())
        return;
    std::vector<std::pair<PlanRow const*, Player*>> members;
    Guild* guild = nullptr;
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || !p->IsInWorld())
            continue;
        members.emplace_back(&row, p);
        if (!guild)
            guild = GuildBankRun::BotGuild(p);
    }
    std::map<uint32, uint32> bank;
    GuildBankRun::CountBank(guild, bank);
    std::unordered_set<uint32> const craftLines(std::begin(CRAFT_LINES), std::end(CRAFT_LINES));
    auto const learnable = [&](uint32 line, uint32 spellId)
    {
        auto const list = recipesBySkill_.find(line);
        if (list == recipesBySkill_.end())
            return false;
        return std::any_of(list->second.begin(), list->second.end(),
                           [&](Recipe const& r) { return r.spellId == spellId && (r.trainer || r.autoLearned); });
    };
    auto const plansLine = [&](PlanRow const& row, Player* p, uint32 line)
    {
        return p->HasSkill(line) || std::find(row.profs.begin(), row.profs.end(), line) != row.profs.end() ||
               std::find(row.secondary.begin(), row.secondary.end(), line) != row.secondary.end();
    };

    CoordLock guard(lock_, __func__);
    climbNeeds_.clear();  // [craft432] filled again by the walks below
    // [fish110] user 2026-11-10: the guild needs two fishers. Before, every member with Fishing in its plan (50 of 75)
    // got the trainer and the pole calls, and none fished: the gathering plan fishes only for a wanted shortfall, and
    // no fish was ever wanted (cooking, the fish's only use, beat questing for nobody - it unlocks no guild need).
    // Two members hold Fishing as their guild role: the ones online with the most Fishing skill, then the highest
    // level (a fisher stays one while online and in the plan); only they get the Fishing calls (the trainer, the
    // pole: their own gold), and the gathering plan sends them to fish what the fives' cooked supplies need
    {
        constexpr size_t GUILD_FISHERS = 2;  // the user's number
        std::set<uint32> next;
        for (uint32 low : fishers_)
            for (auto const& [row, m] : members)
                if (row->low == low && !HumanCharacter(m) && plansLine(*row, m, SKILL_FISHING))
                    next.insert(low);
        std::vector<std::pair<PlanRow const*, Player*>> candidates;
        for (auto const& [row, m] : members)
            if (!next.count(row->low) && !HumanCharacter(m) && plansLine(*row, m, SKILL_FISHING))
                candidates.emplace_back(row, m);
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](auto const& a, auto const& b)
                         {
                             auto const fishing = [](Player* p)
                             { return p->HasSkill(SKILL_FISHING) ? p->GetPureSkillValue(SKILL_FISHING) : 0u; };
                             uint32 const sa = fishing(a.second);
                             uint32 const sb = fishing(b.second);
                             if (sa != sb)
                                 return sa > sb;
                             return a.second->GetLevel() > b.second->GetLevel();
                         });
        for (auto const& [row, m] : candidates)
        {
            if (next.size() >= GUILD_FISHERS)
                break;
            next.insert(row->low);
            LOG_INFO("playerbots", "[fish110] {} L{} is a guild fisher: fishing {}/{} (the most skill, then level, of "
                     "{} members with Fishing planned)",
                     Who(row->low), m->GetLevel(), m->HasSkill(SKILL_FISHING) ? m->GetPureSkillValue(SKILL_FISHING) : 0,
                     m->HasSkill(SKILL_FISHING) ? m->GetPureMaxSkillValue(SKILL_FISHING) : 0, candidates.size());
        }
        fishers_ = std::move(next);
    }
    JudgeSupplySource(members);
    JudgePotionSupply(members);  // [eco194]
    // [prof275] the raw fish the guild cooks' supply jobs take (their owed crafts, through the recipe's yield), less
    // what the members hold of it: the fishers fish for the cook. Before, the need was every member's best fish food
    // of its level whoever cooked it - b428 "Raw Bristle Whisker Catfish ×1440, Raw Sagefish ×560" while every
    // band's job was Goblin Deviled Clams: 6.4 h of fishing for a fish no cook used
    {
        std::map<uint32, uint32> shortBy;
        std::map<uint32, uint32> owedFish;
        for (auto const& [cookLow, jobs] : supplyCrafts_)
        {
            Player* c = nullptr;
            for (auto const& [row, m] : members)
                if (row->low == cookLow)
                    c = m;
            if (!c)
                continue;
            for (SupplyCraft const& job : jobs)
            {
                uint32 const crafts = SupplyOwed(c, job);
                for (auto const& [item, n] : job.reagents)
                {
                    uint32 skill = 0;
                    if (sGuildGatherPlan.Gatherable(item, &skill) && skill == SKILL_FISHING)
                        owedFish[item] += crafts * n;
                }
            }
        }
        for (auto const& [fish, n] : owedFish)
        {
            uint32 held = 0;
            for (auto const& [row, m] : members)
                held += m->GetItemCount(fish, true);
            if (n > held)
                shortBy[fish] = n - held;
        }
        if (shortBy != fishShort_)
        {
            std::string text;
            for (auto const& [fish, n] : shortBy)
                text += (text.empty() ? "" : ", ") + ItemName(fish) + " ×" + std::to_string(n);
            LOG_INFO("playerbots", "[fish110] the guild cooks' supply jobs need: {} [prof275]",
                     text.empty() ? "no fish" : text);
        }
        fishShort_ = std::move(shortBy);
    }
    // what the plan wants crafted that no guild crafter can make yet (nobody knows the recipe)
    struct Need
    {
        uint32 line{0};
        uint32 reqSkill{0};
        uint32 spellId{0};
        uint32 itemId{0};
        uint32 count{0};
        uint32 forLow{0};  // BoP: only this member can craft it for itself
        bool bop{false};
        std::string label;
        double value{0.0};  // [craft428] copper: what the members gain by it (stat weight at the guild's vendor rate)
    };
    std::vector<Need> needs;
    // [craft428] the copper a stat-weight point is worth to the guild: what its members pay for weight at the vendors
    // (the vendor picks of their plans: price over the weight each one adds). b462 an unlocked craft was valued at its
    // vendor tag times the members it serves - a ring that fills an empty slot for 0 c of BuyPrice weighed nothing
    double copperPerWeight = 0.0;
    {
        double paid = 0.0, weight = 0.0;
        for (auto const& [low, state] : bots_)
            for (auto const* list : {&state.gearNow, &state.gearPlan})
                for (GearPick const& g : *list)
                    if (g.kind == GuildGear::SRC_VENDOR && !g.extendedCost && g.aux && g.score > g.current)
                    {
                        paid += double(g.aux);
                        weight += double(g.score - g.current);
                    }
        if (weight > 0.0)
            copperPerWeight = paid / weight;
        copperPerWeight_ = copperPerWeight;  // [craft431] the craft queue's reagent values
        if (uint32(copperPerWeight) != copperPerWeightLogged_)
        {
            copperPerWeightLogged_ = uint32(copperPerWeight);
            LOG_INFO("playerbots", "[craft428] a stat-weight point is worth {} c to the guild ({} c paid for {:.1f} "
                     "weight in its members' vendor picks)",
                     copperPerWeightLogged_, uint64(paid), weight);
        }
    }
    auto const priceOf = [](uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return proto ? double(proto->BuyPrice) : 0.0;
    };
    for (auto const& [row, m] : members)
    {
        auto const st = bots_.find(row->low);
        if (st == bots_.end())
            continue;
        std::unordered_set<uint32> seen;
        for (auto const* list : {&st->second.gearNow, &st->second.gearPlan})
            for (GearPick const& g : *list)
            {
                if (g.kind != GuildGear::SRC_CRAFT || g.crafterLow || !seen.insert(g.itemId).second ||
                    m->GetItemCount(g.itemId, true))
                    continue;
                GuildGear::CraftRecipe const* recipe = GuildGear::Recipe(g.srcId);
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(g.itemId);
                if (!recipe || !proto || !learnable(recipe->skillLine, recipe->spellId))
                    continue;  // a recipe found elsewhere: the skill alone does not unlock it
                uint32 const slots = PairSlotsFilled(m, proto, g.group);  // [craft435]
                needs.push_back({recipe->skillLine, recipe->reqSkill, recipe->spellId, g.itemId, slots, row->low,
                                 proto->Bonding == BIND_WHEN_PICKED_UP,
                                 std::string("upgrade ") + GuildGear::GroupName(g.group) + " for " + Who(row->low),
                                 (copperPerWeight > 0.0 && g.score > g.current
                                      ? double(g.score - g.current) * copperPerWeight
                                      : priceOf(g.itemId)) *
                                     slots});
            }
    }
    // bags: the next bag size above the biggest one a guild crafter knows, for every smaller bag slot
    {
        uint32 known = 0;
        for (uint32 spell : GuildGear::BagRecipes())
            if (GuildGear::CraftRecipe const* r = GuildGear::Recipe(spell))
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(r->itemId))
                    for (auto const& [row, m] : members)
                        if (m->HasSpell(spell))
                            known = std::max<uint32>(known, proto->ContainerSlots);
        // [craft423] the smallest bag size above it that a member still lacks: b462 every bag slot held an 8-slot
        // vendor bag, the next size (Linen Bag, 6) was needed by nobody, so no tailor ever worked toward a 10-slot one
        std::map<uint32, GuildGear::CraftRecipe const*> bySize;  // container slots -> the recipe of the lowest skill
        for (uint32 spell : GuildGear::BagRecipes())
        {
            GuildGear::CraftRecipe const* r = GuildGear::Recipe(spell);
            ItemTemplate const* proto = r ? sObjectMgr->GetItemTemplate(r->itemId) : nullptr;
            if (!proto || proto->ContainerSlots <= known || !learnable(r->skillLine, r->spellId))
                continue;
            auto& best = bySize[proto->ContainerSlots];
            if (!best || r->reqSkill < best->reqSkill)
                best = r;
        }
        for (auto const& [nextSlots, next] : bySize)
        {
            uint32 count = 0;
            for (auto const& [row, m] : members)
                for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
                {
                    Bag* bag = m->GetBagByPos(b);
                    count += (!bag || (GenericBagProto(bag->GetTemplate()) && bag->GetBagSize() < nextSlots)) ? 1 : 0;
                }
            if (!count)
                continue;
            needs.push_back({next->skillLine, next->reqSkill, next->spellId, next->itemId, count, 0, false,
                             "bag " + std::to_string(nextSlots) + " slots ×" + std::to_string(count),
                             priceOf(next->itemId) * count});
            break;
        }
    }
    // [insc143] glyphs: b426-b429 the 7 scribes stayed at Inscription 1/75 all run - the line unlocked nothing (the
    // needs were gear crafts and bags only), so its skill-ups never beat questing ("unlocks 0c", value = -costs) and
    // no scribe was called to them. A glyph of a member's premade list that it can use now and has a slot for
    // (GlyphUtil::NeedsGlyphItem: class, required level, slot type, not in its bags) is a need of the Inscription
    // recipe that makes it, the trainer's or the profession's own
    {
        std::map<uint32, uint32> glyphs;  // glyph item -> members who need one
        std::map<uint32, std::vector<uint32>> glyphFor;  // [insc145] glyph item -> those members
        for (auto const& [row, m] : members)
            if (!HumanCharacter(m))
                for (uint32 glyph : GlyphUtil::GetWantedGlyphItems(m))
                    if (GlyphUtil::NeedsGlyphItem(m, glyph))
                    {
                        ++glyphs[glyph];
                        glyphFor[glyph].push_back(row->low);
                    }
        glyphFor_ = std::move(glyphFor);
        auto const insc = glyphs.empty() ? recipesBySkill_.end() : recipesBySkill_.find(SKILL_INSCRIPTION);
        if (insc != recipesBySkill_.end())
            for (Recipe const& r : insc->second)
                if (auto const g = glyphs.find(r.product); g != glyphs.end() && (r.trainer || r.autoLearned))
                    needs.push_back({SKILL_INSCRIPTION, r.reqSkill, r.spellId, r.product, g->second, 0, false,
                                     "glyph for " + std::to_string(g->second) + " member" +
                                         (g->second > 1 ? "s" : ""),
                                     priceOf(r.product) * g->second});
        if (glyphs != glyphNeeds_)
        {
            std::string text;
            for (auto const& [glyph, n] : glyphs)
                if (text.size() < 300)
                    text += (text.empty() ? "" : ", ") + ItemName(glyph) + " ×" + std::to_string(n);
            LOG_INFO("playerbots", "[insc143] the members' glyph needs ({} glyphs): {}", glyphs.size(),
                     text.empty() ? "none" : text);
            glyphNeeds_ = std::move(glyphs);
        }
    }
    // [lag325] coordinator decision 2026-11-11 (the user's laggard focus): a member below the guild's median level
    // levels first - its profession calls (craft / buy / gather for a skill-up) wait until it reaches the median; the
    // [econ1] price of its quest XP (its own copper per XP, a few copper a minute at L13) never weighed its lag.
    // b435 Rollothan L13 (median 18): "profession 837 c/min > quests 89 c/min" -> 6.9 min crafting Light Parchment
    uint8 medianLevel = 0;
    {
        std::vector<uint8> levels;
        for (auto const& entry : members)
            if (!HumanCharacter(entry.second))
                levels.push_back(entry.second->GetLevel());
        if (!levels.empty())
        {
            std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
            medianLevel = levels[levels.size() / 2];
        }
    }
    // [lag326] the yards from a member to the nearest vendor of one of a recipe's vendor-sold reagents (npc_vendor,
    // its own map; FLT_MAX: none there)
    // [lag335] the reagents' vendors from npc_vendor itself: GuildGear::VendorsOf indexes only gear and bags, so no
    // reagent vendor was ever found here (b443 / b444: every scribe's distance FLT_MAX)
    auto const vendorYards = [this](Player* m, uint32 spellId)
    {
        float best = std::numeric_limits<float>::max();
        if (GuildGear::CraftRecipe const* r = GuildGear::Recipe(spellId))
            for (auto const& [reagent, count] : r->reagents)
                if (std::vector<ReagentVendor> const* spawns = ReagentVendors(reagent))
                    for (ReagentVendor const& v : *spawns)
                        if (!v.extendedCost && v.mapId == m->GetMapId() && FriendlyNpc(m, v.entry))
                            best = std::min(best, m->GetExactDist(v.x, v.y, v.z));
        return best;
    };
    // each need goes to the member of that profession closest to it (the highest skill; BoP: the member itself).
    // [lag326] coordinator 2026-11-11: a member [lag325] holds back (below the guild's median level) holds no need
    // another member of the line can take - the free ones first, then the highest skill, then the nearest to a vendor
    // of the recipe's reagents. b437: all 7 scribes at Inscription 1/75 (a tie) and every glyph need went to the first
    // of them, Rollothan L13 below the median: no scribe was ever called, Inscription could not start
    std::map<std::pair<uint32, uint32>, std::vector<Need const*>> assigned;  // (crafter, line) -> needs
    // [craft426] fight by numbers: a need goes to the crafter that delivers it first - the minutes a held member
    // still quests to the median level (its XP left at its own XP per minute) plus the skill points it lacks at the
    // line's measured minutes per point (its walks' last figure). b462 the Leatherworking needs went to Eraelor
    // (LW 13, free) over Bierya (LW 33, L20 below the median L21 with ~2100 XP left, ~55 min at 38 XP/min): 20 more
    // points at ~5.5 min each. No pace measured for the line yet: the free member, then the highest skill, as before
    std::map<uint32, double> waitMinutes;  // held member -> minutes to the median level
    auto const waitFor = [&](PlanRow const& row, Player* m) -> double
    {
        if (m->GetLevel() >= medianLevel)
            return 0.0;
        auto const known = waitMinutes.find(row.low);
        if (known != waitMinutes.end())
            return known->second;
        double minutes = std::numeric_limits<double>::infinity();
        float copper = 0.0f, xpPerMinute = 0.0f;
        if (QuestPace(bots_[row.low], m, copper, nullptr, &xpPerMinute) && xpPerMinute > 0.0f)
        {
            double xp = 0.0;
            for (uint8 l = m->GetLevel(); l < medianLevel; ++l)
                xp += double(sObjectMgr->GetXPForLevel(l));
            xp -= double(m->GetUInt32Value(PLAYER_XP));
            minutes = std::max(0.0, xp) / xpPerMinute;
        }
        waitMinutes[row.low] = minutes;
        return minutes;
    };
    for (Need const& n : needs)
    {
        uint32 best = 0, bestSkill = 0;
        bool bestHeld = true;
        float bestYards = std::numeric_limits<float>::max();
        double bestMinutes = std::numeric_limits<double>::infinity();
        auto const pace = lineMinPerPoint_.find(n.line);
        bool const priced = pace != lineMinPerPoint_.end() && pace->second > 0.0;
        for (auto const& [row, m] : members)
        {
            if (HumanCharacter(m) || (n.bop && row->low != n.forLow) || !plansLine(*row, m, n.line))
                continue;
            uint32 const skill = m->HasSkill(n.line) ? m->GetPureSkillValue(n.line) : 0;
            bool const held = m->GetLevel() < medianLevel;
            float const yards = vendorYards(m, n.spellId);
            double const minutes =
                priced ? waitFor(*row, m) + double(n.reqSkill > skill ? n.reqSkill - skill : 0) * pace->second
                       : 0.0;
            if (best)
            {
                if (priced && minutes != bestMinutes)
                {
                    if (minutes > bestMinutes)
                        continue;
                }
                else if (!priced && held != bestHeld)
                {
                    if (held)
                        continue;  // a free member beats a held one
                }
                else if (skill != bestSkill)
                {
                    if (skill < bestSkill)
                        continue;
                }
                else if (yards >= bestYards)
                    continue;
            }
            best = row->low;
            bestSkill = skill;
            bestHeld = held;
            bestYards = yards;
            bestMinutes = minutes;
        }
        if (best)
            assigned[{best, n.line}].push_back(&n);
    }

    // [insc144] a raw material the guild's gatherers bring for a crafter: units per minute, the measured pace of the
    // gathering skill (DO_GATHER) or, none measured yet, the best formula rate (GuildGatherPlan::BestRate, per hour)
    // of a member whose skill reaches it. b426-b429: Herbalism had no measured pace - no herb was ever wanted, so
    // nobody was sent to gather - and the walks stopped at the first herb ("no gathering pace measured")
    std::map<uint32, float> guildPace;
    auto const gatherPace = [&](uint32 rawItem, uint32 gatherSkill, uint32 reqSkill, Player* only)
    {
        float const measured = gatherSkill ? GatherPace(gatherSkill) : 0.0f;
        if (measured > 0.0f)
            return measured;
        if (!only)
            if (auto const known = guildPace.find(rawItem); known != guildPace.end())
                return known->second;
        float best = 0.0f;
        for (auto const& [gatherRow, g] : members)
        {
            if (only && g != only)
                continue;
            // [ench164] a mob's own drop (no skill): the members the gathering plan sends (UpdateOrders' gatherers)
            bool const can = gatherSkill
                                 ? g->HasSkill(gatherSkill) && g->GetPureSkillValue(gatherSkill) >= reqSkill
                                 : (gatherRow->grp == 'B' || gatherRow->grp == 'C' || fishers_.count(gatherRow->low)) &&
                                       !HumanCharacter(g);
            if (can)
                best = std::max(best, sGuildGatherPlan.BestRate(rawItem, g) / 60.0f);
        }
        if (!only)
            guildPace[rawItem] = best;
        return best;
    };

    // [prof272] the trainer's fees of a TRAIN call: the first rank of a line (Apprentice), a recipe it teaches
    auto const firstRankFee = [&](uint32 line) -> uint32
    {
        if (auto const ranks = trainerRanks_.find(line); ranks != trainerRanks_.end())
            for (TrainerRank const& r : ranks->second)
                if (r.maxSkill == SKILL_STEP_MAX)
                    return r.price;
        return 0;
    };
    auto const recipeFee = [&](uint32 line, uint32 spellId) -> uint32
    {
        if (auto const list = recipesBySkill_.find(line); list != recipesBySkill_.end())
            for (Recipe const& r : list->second)
                if (r.spellId == spellId)
                    return r.price;
        return 0;
    };
    for (auto const& [row, p] : members)
    {
        if (HumanCharacter(p))
        {
            professions_.erase(row->low);
            continue;
        }
        BotState& st = bots_[row->low];
        uint8 const level = p->GetLevel();
        bool const levelsFirst = level < medianLevel;  // [lag325]
        st.profGoldShort = 0;  // [lag330] summed below over its lines with members' orders
        st.ordersCall = false;  // [lag336]
        st.profGoldWhat.clear();
        std::vector<ProfessionPick> picks;
        std::vector<uint32> lines;
        for (auto const* list : {&row->profs, &row->secondary})
            for (uint32 line : *list)
                if (craftLines.count(line) && std::find(lines.begin(), lines.end(), line) == lines.end())
                    lines.push_back(line);
        for (uint32 line : lines)
        {
            bool const has = p->HasSkill(line);
            uint32 const skill = has ? p->GetPureSkillValue(line) : 0;
            uint32 const maxSkill = has ? p->GetPureMaxSkillValue(line) : 0;
            uint32 const cap = TrainerCap(line, level);
            static std::vector<Need const*> const none;
            auto const mine = assigned.find({row->low, line});
            std::vector<Need const*> const& lineNeeds = mine != assigned.end() ? mine->second : none;
            uint32 needPlan = 0;
            for (Need const* n : lineNeeds)
                if (n->reqSkill <= std::max(cap, maxSkill))
                    needPlan = std::max(needPlan, n->reqSkill);
            uint32 const need = std::max(cap, needPlan);
            std::string const head = Who(row->low) + ": " + SkillShort(line) + " " + std::to_string(skill) + "/" +
                                     std::to_string(maxSkill) + ", needs " + std::to_string(need) +
                                     " (trainer rank at L" + std::to_string(level) + ": " + std::to_string(cap) +
                                     (needPlan ? ", plan: " + std::to_string(needPlan) : "") + ")";
            // [gather] the value of one skill point of this crafter on this line, what its wanted materials are worth
            // (UpdateWanted): over the planned skill-up walk from its skill to what its level reaches, whatever the
            // reagents' availability (a crafter blocked on a material is worth what that material unblocks), the
            // products' vendor price (they are sold) plus the price of the planned crafts the walk unlocks, per point
            {
                uint32 const reach = std::max(cap, maxSkill);
                auto const list = recipesBySkill_.find(line);
                auto const skillPlan = skillPlan_.find(line);
                double sales = 0, unlocked = 0;
                uint32 points = 0;
                if (list != recipesBySkill_.end() && skillPlan != skillPlan_.end())
                    for (uint32 v = std::max<uint32>(1, skill); v < reach && v < skillPlan->second.size(); ++v)
                    {
                        if (skillPlan->second[v] < 0)
                            break;
                        Recipe const& r = list->second[size_t(skillPlan->second[v])];
                        int32 const chance = CraftChance(v, r.gray, r.yellow);
                        if (r.reqLevel > level || chance <= 0)
                            break;
                        if (ItemTemplate const* product = r.product ? sObjectMgr->GetItemTemplate(r.product) : nullptr)
                            sales += 1000.0 / chance * r.made * product->SellPrice;
                        ++points;
                    }
                for (Need const* n : lineNeeds)
                    if (n->reqSkill > skill && n->reqSkill <= skill + points)
                        unlocked += n->value;  // [craft428]
                uint64 const key = (uint64(row->low) << 16) | line;
                // [econ1] user rule 2026-11-08: gathering is weighed against the guild's levelling, so a material is
                // worth what it unlocks for the members (the planned upgrade / bag crafts the walk reaches), not the
                // vendor price of the skill-up products (b350: Copper Ore 99 c/unit beat questing 3-5x, 16.4 of 23.2
                // bot-hours went to gathering). The products' sales stay out of the gathering value
                (void)sales;
                if (points && unlocked > 0.0)
                    pointValue_[key] = float(unlocked / points);
                else
                    pointValue_.erase(key);
            }
            std::string key;
            std::string decision;
            auto const train = [&](std::string details, std::string reason, uint32 cost = 0, uint32 spellId = 0)
            {
                ProfessionPick pick;
                pick.kind = GuildProfessionKind::TRAIN;
                pick.skillLine = line;
                pick.cost = cost;
                pick.spellId = spellId;
                // [econ2] its nearest trainer's town: learned on its next visit there (ProfessionCall)
                if (auto const spots = trainerSpots_.find(line); spots != trainerSpots_.end())
                    if (NpcSpot const* n = NearestSpot(p, row->low, spots->second))
                        pick.pos = n->spot.pos;
                pick.details = std::move(details);
                pick.reason = std::move(reason);
                picks.push_back(pick);
            };
            if (!has)
            {
                train(std::string("learn ") + SkillShort(line),
                      "in the plan, not learned; trainer rank at L" + std::to_string(level) + " to " +
                          std::to_string(cap),
                      firstRankFee(line));
                key = "learn";
                decision = head + " — not learned: TRAIN at the trainer on the next town visit";
            }
            else if (skill >= need)
            {
                key = "ok";
                decision = head + " — no lag: quests";
            }
            else
            {
                // the next rank at the trainer, when its level and skill allow it
                TrainerRank const* rank = nullptr;
                if (auto const ranks = trainerRanks_.find(line); ranks != trainerRanks_.end())
                    for (TrainerRank const& r : ranks->second)
                        if (r.maxSkill > maxSkill && r.reqLevel <= level && (!rank || r.maxSkill < rank->maxSkill))
                            rank = &r;
                if (rank && skill >= rank->reqSkill)
                    train("rank to " + std::to_string(rank->maxSkill),
                          "skill " + std::to_string(skill) + " ≥ " + std::to_string(rank->reqSkill) + ", level ≥ " +
                              std::to_string(rank->reqLevel),
                          rank->price);
                // a planned recipe its skill already allows: learned at the trainer
                for (Need const* n : lineNeeds)
                    if (n->reqSkill <= skill && !p->HasSpell(n->spellId))
                    {
                        train("recipe " + ItemName(n->itemId), n->label, recipeFee(line, n->spellId), n->spellId);
                        break;
                    }
                // [craft392] the skill-up walk's own trainer recipes its skill and level reach now, up to its held
                // rank's cap: learned at the trainer at their fee (the walk below values them; the craft executor
                // makes only what it knows). b453 Dekorhan crafted Inscription 1 -> 25 from the recipes learned with
                // the skill, then "crafting done here: nothing here still gives skill" - the walk's next recipes
                // (Feyden Darkin, Darnassus) were never called
                auto const walkPlan = skillPlan_.find(line);  // [build154] its own type (C3538)
                if (auto const walkList = recipesBySkill_.find(line);
                    walkList != recipesBySkill_.end() && walkPlan != skillPlan_.end())
                {
                    std::set<uint32> called;
                    for (uint32 v = std::max<uint32>(1, skill); v < maxSkill && v < walkPlan->second.size(); ++v)
                    {
                        if (walkPlan->second[v] < 0)
                            break;
                        Recipe const& r = walkList->second[size_t(walkPlan->second[v])];
                        if (!r.trainer || r.autoLearned || p->HasSpell(r.spellId) || r.reqSkill > skill ||
                            r.reqLevel > level || !called.insert(r.spellId).second)
                            continue;
                        train("recipe " + NewRpgCraft::RecipeName(r.spellId),
                              "its skill-up walk's next recipe at " + std::to_string(v), r.price, r.spellId);
                        if (std::string& logged = st.craftLogged["craft392:" + std::to_string(r.spellId)];
                            logged.empty())
                        {
                            logged = "called";
                            LOG_INFO("playerbots", "[craft392] {} L{} {} {}: TRAIN {} ({}), the walk's recipe at {}",
                                     p->GetName(), uint32(level), SkillShort(line), skill,
                                     NewRpgCraft::RecipeName(r.spellId), Gsc(r.price), v);
                        }
                    }
                }
                // [craft427] the trainer recipes the walk took instead of a blocked plan recipe (its last update)
                if (auto const alts = st.walkRecipes.find(line); alts != st.walkRecipes.end())
                    if (auto const altList = recipesBySkill_.find(line); altList != recipesBySkill_.end())
                        for (Recipe const& r : altList->second)
                            if (alts->second.count(r.spellId) && r.trainer && !r.autoLearned &&
                                !p->HasSpell(r.spellId) && r.reqSkill <= skill && r.reqLevel <= level)
                            {
                                train("recipe " + NewRpgCraft::RecipeName(r.spellId),
                                      "its skill-up walk's recipe past a blocked one [craft427]", r.price, r.spellId);
                                break;
                            }
                // [prof274] the guild cook's supply recipe a trainer teaches (no recipe item to buy): learned there
                if (line == SKILL_COOKING)
                    if (auto const jobs = supplyCrafts_.find(row->low); jobs != supplyCrafts_.end())
                        for (SupplyCraft const& job : jobs->second)
                            if (!job.recipeItem && !p->HasSpell(job.spellId))
                            {
                                train("recipe " + ItemName(job.product),
                                      "[prof274] the guild cook's supply recipe for band " + job.band,
                                      recipeFee(line, job.spellId), job.spellId);
                                break;
                            }

                // the skill-up walk from its skill up to the need (within its rank's cap)
                auto const list = recipesBySkill_.find(line);
                auto const skillPlan = skillPlan_.find(line);
                std::map<uint32, double> pool;  // item -> units at hand (its bags + the guild bank) minus the used
                auto const avail = [&](uint32 item) -> double&
                {
                    auto it = pool.find(item);
                    if (it == pool.end())
                    {
                        auto const b = bank.find(item);
                        it = pool.emplace(item, double(p->GetItemCount(item, false)) +
                                                    (b != bank.end() ? double(b->second) : 0.0))
                                 .first;
                    }
                    return it->second;
                };
                double craftMin = 0, gatherMin = 0, vendorCost = 0, sales = 0, gatherUnits = 0;
                uint32 gatherItem = 0, buyItem = 0;
                // [prof277] what it buys / gathers now: the skill-ups below its held rank's cap, per item (a rank
                // past it is the trainer's first; the walk still values the whole way to the need). b430 Rollothan
                // Insc 1/75 "Common Parchment x316": the units of the walk to 150 through the next rank, every
                // vendor reagent of the walk summed under the last one's name
                std::map<uint32, double> buyNow, gatherNow;
                std::map<uint32, double> gatherItemMin;  // [ench169] the walk's gathering minutes per item
                bool held = false;
                std::string block;
                uint32 blockSkill = 0, blockReq = 0, blockItem = 0;  // [craft432] a block on a gathering tier
                uint32 reach = skill;
                // [insc144] what other members hold of a material: theirs by the transfer once the line is called
                std::map<uint32, double> others;
                auto const othersAvail = [&](uint32 item) -> double&
                {
                    auto it = others.find(item);
                    if (it == others.end())
                    {
                        double n = 0;
                        for (auto const& [otherRow, other] : members)
                            if (other != p)
                                n += other->GetItemCount(item, false);
                        it = others.emplace(item, n).first;
                    }
                    return it->second;
                };
                bool fromGuild = false;
                std::string guildText;
                // [ench160] an enchanter's dust / essences / shards from the greens on its way to its Disenchant
                // ([ench153] / [ench161] / [ench155]): their expected yield. b442 every enchanter's walk stopped at
                // "Strange Dust x1: neither held nor gathered in the guild" (Enchanting 8-10, best 10) - disenchanting
                // is the only source of Strange Dust / Lesser Magic Essence and the walk did not count it
                std::map<uint32, double> disenchanted, takeDisenchanted;
                uint32 greens = 0;
                if (line == SKILL_ENCHANTING && p->HasSpell(SPELL_DISENCHANT))
                {
                    auto const addYield = [&](ItemTemplate const* proto)
                    {
                        if (!proto || !proto->DisenchantID)
                            return;
                        ++greens;
                        for (auto const& [item, units] : DisenchantYield(proto->DisenchantID))
                            disenchanted[item] += units;
                    };
                    for (uint32 guid : st.disenchant)
                        if (Item* item = p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid)))
                            addYield(item->GetTemplate());
                    for (auto const& [holder, hs] : bots_)
                        for (HeldItem const& h : hs.held)
                            if (h.forLow == row->low && h.purpose.rfind("disenchant", 0) == 0)
                                addYield(sObjectMgr->GetItemTemplate(h.itemId));
                }
                // [insc144] the walk runs to the need through the trainer ranks its level allows (their fees are
                // costs): b426-b429 it stopped at the held rank's cap (Inscription 75) while the needs it is valued
                // by lie past it (the glyphs from 80), so the line never valued what it unlocks
                uint32 const top = need;
                st.walkRecipes[line].clear();  // [craft427] the alternatives this walk takes (TRAIN reads them)
                // [ench170] the walk's stopping points: a recipe's yellow or grey edge, a trainer rank's cap, its end -
                // the sums up to there (the rank fees added below)
                struct Stop
                {
                    uint32 reach{0};
                    double craftMin{0}, gatherMin{0}, vendorCost{0};
                    std::map<uint32, double> buyNow, gatherNow;  // [ench171] what the calls buy / gather up to it
                };
                std::vector<Stop> stops;
                std::set<uint32> rankCaps;
                if (auto const ranks = trainerRanks_.find(line); ranks != trainerRanks_.end())
                    for (TrainerRank const& tr : ranks->second)
                        rankCaps.insert(tr.maxSkill);
                for (uint32 v = std::max<uint32>(1, skill); v < top; ++v)
                {
                    if (list == recipesBySkill_.end() || skillPlan == skillPlan_.end() ||
                        v >= skillPlan->second.size() || skillPlan->second[v] < 0)
                    {
                        block = "no skill-up recipe at the trainer at " + std::to_string(v);
                        break;
                    }
                    // [craft427] the plan's recipe at this value, else the next cheapest trainer / auto-learned one (or
                    // one it knows) whose materials the guild reaches: b462 Thelengyl's Blacksmithing walk stopped at
                    // 125 on "Heavy Stone: no member gatherer reaches it (Mine 125)" while bronze recipes of the same
                    // values take none
                    std::vector<size_t> tries{size_t(skillPlan->second[v])};
                    bool alternatives = false;
                    Recipe const* chosen = nullptr;
                    double crafts = 0, cost = 0, gmin = 0;
                    std::map<uint32, double> take, takeOthers;
                    std::string firstWhy;
                    uint32 tierSkill = 0, tierReq = 0, tierItem = 0;  // [craft432] this value's first tier block
                    static std::vector<std::pair<uint32, uint32>> const noReagents;
                    for (size_t t = 0; t < tries.size() && !chosen; ++t)
                    {
                        Recipe const& r = list->second[tries[t]];
                        int32 const chance = CraftChance(v, r.gray, r.yellow);
                        std::string why;
                        if (r.reqLevel > level || chance <= 0)
                            why = NewRpgCraft::RecipeName(r.spellId) + " from level " + std::to_string(r.reqLevel);
                        auto const savedBuy = buyNow, savedGather = gatherNow, savedGatherMin = gatherItemMin,
                                   savedDisenchanted = takeDisenchanted;
                        uint32 const savedGatherItem = gatherItem, savedBuyItem = buyItem;
                        double const savedGatherUnits = gatherUnits;
                        crafts = why.empty() ? 1000.0 / chance : 0.0;
                        take.clear();
                        takeOthers.clear();
                        cost = gmin = 0;
                        for (auto const& [item, n] : why.empty() ? r.reagents : noReagents)
                        {
                            double want = crafts * n;
                            double const fromPool = std::clamp(avail(item) - take[item], 0.0, want);
                            take[item] += fromPool;
                            want -= fromPool;
                            held |= fromPool > 0;
                            if (want <= 0)
                                continue;
                            std::map<uint32, double> raw;
                            ExpandReagent(item, want, raw, nullptr, 1.0f, 0, 0, p->GetMapId());  // [lag329]
                            for (auto const& [rawItem, rawUnits] : raw)
                            {
                                double units = rawUnits;
                                if (rawItem != item)
                                {
                                    double const fromRaw = std::clamp(avail(rawItem) - take[rawItem], 0.0, units);
                                    take[rawItem] += fromRaw;
                                    units -= fromRaw;
                                    held |= fromRaw > 0;
                                }
                                if (units <= 0)
                                    continue;
                                if (VendorSource(rawItem, p->GetMapId()))  // [lag329]
                                {
                                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rawItem);
                                    cost += units * (proto ? double(proto->BuyPrice) /
                                                                 std::max<uint32>(1, proto->BuyCount)
                                                           : 0.0);
                                    buyItem = rawItem;
                                    if (v < maxSkill)
                                        buyNow[rawItem] += units;
                                    continue;
                                }
                                auto const src = gatherSource_.find(rawItem);
                                if (src != gatherSource_.end() && p->HasSkill(src->second.first) &&
                                    p->GetPureSkillValue(src->second.first) >= src->second.second)
                                {
                                    float const pace = gatherPace(rawItem, src->second.first, src->second.second, p);
                                    if (pace > 0.0f)
                                    {
                                        gmin += units / pace;
                                        gatherItemMin[rawItem] += units / pace;
                                        if (units > gatherUnits)
                                        {
                                            gatherItem = rawItem;
                                            gatherUnits = units;
                                        }
                                        if (v < maxSkill)
                                            gatherNow[rawItem] += units;
                                        continue;
                                    }
                                    why = ItemName(rawItem) + ": no gathering pace measured (" +
                                          SkillShort(src->second.first) + ")";
                                    break;
                                }
                                // [insc144] b426-b429 a scribe without Herbalism stopped here ("waits for the transfer"
                                // / "others gather it") - but a material is wanted and sent only for a line the leader
                                // calls, and the line was called only past this point: nobody ever sent or gathered the
                                // herbs (sold as "not in the wanted list"). What other members hold comes by the
                                // transfer, the rest the guild's gatherers bring at their pace
                                double const fromOthers =
                                    std::clamp(othersAvail(rawItem) - takeOthers[rawItem], 0.0, units);
                                takeOthers[rawItem] += fromOthers;
                                units -= fromOthers;
                                fromGuild = true;
                                // [ench160] then what its Disenchant makes of the greens on their way to it
                                auto const yield = disenchanted.find(rawItem);
                                if (double const fromGreens =
                                        yield == disenchanted.end()
                                            ? 0.0
                                            : std::clamp(yield->second - takeDisenchanted[rawItem], 0.0, units);
                                    fromGreens > 0)
                                {
                                    takeDisenchanted[rawItem] += fromGreens;
                                    units -= fromGreens;
                                    if (guildText.empty() && fromOthers <= 0)
                                        guildText = ItemName(rawItem) + " (Disenchant of " + std::to_string(greens) +
                                                    " guild greens [ench160])";
                                }
                                if (guildText.empty())
                                    guildText = ItemName(rawItem);
                                if (units <= 0)
                                    continue;
                                if (src != gatherSource_.end())
                                    if (float const pace = gatherPace(rawItem, src->second.first, src->second.second,
                                                                      nullptr);
                                        pace > 0.0f)
                                    {
                                        gmin += units / pace;
                                        gatherItemMin[rawItem] += units / pace;
                                        continue;
                                    }
                                // [ench163] skinning and fishing (GuildGatherPlan's SKIN / FISH places), [ench164]
                                // a mob's own drop (DROP, no skill): the guild's gatherers at the plan's rate by
                                // formula - b442 Light Hide, Oily Blackmouth, Coyote Meat, Wool Cloth "neither held
                                // nor gathered" (gatherSource_ holds only herb / ore nodes). The crafter's own
                                // gathering stays the nodes (its GATHER profession call runs DO_GATHER, no place)
                                uint32 planSkill = 0;
                                int32 planReq = 0;
                                bool const planned = src == gatherSource_.end() &&
                                                     sGuildGatherPlan.Gatherable(rawItem, &planSkill, &planReq);
                                if (planned)
                                    if (float const pace = gatherPace(rawItem, planSkill,
                                                                      uint32(std::max<int32>(0, planReq)), nullptr);
                                        pace > 0.0f)
                                    {
                                        gmin += units / pace;
                                        gatherItemMin[rawItem] += units / pace;
                                        continue;
                                    }
                                std::string const unreached =
                                    src != gatherSource_.end()
                                        ? std::string("no member gatherer reaches it (") +
                                              SkillShort(src->second.first) + " " +
                                              std::to_string(src->second.second) + ")"
                                    : planned && planSkill
                                        ? std::string("no member gatherer reaches it (") + SkillShort(planSkill) + " " +
                                              std::to_string(std::max<int32>(0, planReq)) + ") [ench163]"
                                    : planned ? std::string("no gatherer of the guild kills its mobs safely [ench164]")
                                              : std::string("neither held nor gathered in the guild");
                                why = ItemName(rawItem) + " ×" + std::to_string(uint32(std::ceil(units))) + ": " +
                                      unreached;
                                if (!tierItem && (src != gatherSource_.end() || (planned && planSkill)))
                                {
                                    tierItem = rawItem;
                                    tierSkill = src != gatherSource_.end() ? src->second.first : planSkill;
                                    tierReq = src != gatherSource_.end() ? src->second.second
                                                                         : uint32(std::max<int32>(0, planReq));
                                }
                                if (yield != disenchanted.end())  // [ench160] what the greens on their way gave
                                    why += " (beyond the Disenchant of " + std::to_string(greens) +
                                           " guild greens [ench160])";
                                break;
                            }
                            if (!why.empty())
                                break;
                        }
                        if (!why.empty())
                        {
                            buyNow = savedBuy;
                            gatherNow = savedGather;
                            gatherItemMin = savedGatherMin;
                            takeDisenchanted = savedDisenchanted;
                            gatherItem = savedGatherItem;
                            buyItem = savedBuyItem;
                            gatherUnits = savedGatherUnits;
                        }
                        if (why.empty())
                        {
                            chosen = &r;
                            break;
                        }
                        if (firstWhy.empty())
                            firstWhy = why;
                        if (alternatives)
                            continue;
                        alternatives = true;
                        std::vector<std::pair<double, size_t>> altOrder;
                        for (size_t i = 0; i < list->second.size(); ++i)
                        {
                            Recipe const& alt = list->second[i];
                            double altCost = 0, altUnits = 0;
                            int32 altChance = 0;
                            if (i == tries.front() || alt.reqSkill > v || alt.reqLevel > level ||
                                !((alt.trainer || alt.autoLearned) || p->HasSpell(alt.spellId)) ||
                                !SkillUpCost(alt, v, vendorItems_, altCost, altUnits, altChance) || altChance <= 0)
                                continue;
                            altOrder.emplace_back(altCost, i);
                        }
                        std::sort(altOrder.begin(), altOrder.end());
                        for (auto const& [altCost, i] : altOrder)
                            tries.push_back(i);
                    }
                    if (!chosen)
                    {
                        block = firstWhy;
                        blockSkill = tierSkill;
                        blockReq = tierReq;
                        blockItem = tierItem;
                        break;
                    }
                    Recipe const& r = *chosen;
                    if (chosen != &list->second[tries.front()])
                        st.walkRecipes[line].insert(r.spellId);  // [craft427] trained as the walk's own recipe
                    for (auto const& [item, n] : take)
                        pool[item] -= n;
                    for (auto const& [item, n] : takeOthers)
                        others[item] -= n;
                    for (auto const& [item, n] : takeDisenchanted)
                        disenchanted[item] -= n;
                    takeDisenchanted.clear();
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(r.spellId))
                        craftMin += crafts * info->CalcCastTime() / double(MINUTE * IN_MILLISECONDS);
                    if (ItemTemplate const* product = r.product ? sObjectMgr->GetItemTemplate(r.product) : nullptr)
                        sales += crafts * r.made * product->SellPrice;
                    vendorCost += cost;
                    gatherMin += gmin;
                    reach = v + 1;
                    if (reach == r.yellow || reach == r.gray || rankCaps.count(reach))
                        stops.push_back({reach, craftMin, gatherMin, vendorCost, buyNow, gatherNow});
                }
                if (reach > skill && (stops.empty() || stops.back().reach != reach))
                    stops.push_back({reach, craftMin, gatherMin, vendorCost, buyNow, gatherNow});
                // [insc144] the fees of the ranks the walk passes (the rank's cap less one step is the cap it lifts)
                if (auto const ranks = trainerRanks_.find(line); ranks != trainerRanks_.end())
                    for (TrainerRank const& tr : ranks->second)
                        if (tr.reqLevel <= level && tr.maxSkill > maxSkill && tr.maxSkill > SKILL_STEP_MAX)
                        {
                            if (reach > tr.maxSkill - SKILL_STEP_MAX)
                                vendorCost += tr.price;
                            for (Stop& stop : stops)  // [ench170] and those each stopping point passes
                                if (stop.reach > tr.maxSkill - SKILL_STEP_MAX)
                                    stop.vendorCost += tr.price;
                        }
                // [craft432] a walk stopped on a gathering tier: what the members' needs past the stop are worth is
                // what that tier is worth to the guild (the gathering plan sends a gatherer to climb toward it)
                if (blockItem && (blockSkill == SKILL_MINING || blockSkill == SKILL_HERBALISM ||
                                  blockSkill == SKILL_SKINNING))
                {
                    double past = 0.0;
                    for (Need const* n : lineNeeds)
                        if (n->reqSkill > reach && n->reqSkill <= top)
                            past += n->value;
                    if (past > 0.0)
                    {
                        ClimbNeed& c = climbNeeds_[{blockSkill, blockReq}];
                        c.itemId = blockItem;
                        c.value += past;
                        if (c.forWhat.size() < 80)
                            c.forWhat += (c.forWhat.empty() ? "" : ", ") + std::string(SkillShort(line)) + " " +
                                         std::to_string(reach) + " of " + Who(row->low);
                    }
                }
                std::string const lag = ", lag " + std::to_string(need - skill);
                // [supply112] what the line produces for the guild's needs: the cook's supply jobs
                double supplyValue = 0.0, supplyMin = 0.0;
                std::string supplyText;
                if (line == SKILL_COOKING)
                    SupplyWork(row->low, p, supplyValue, supplyMin, supplyText);
                else if (line == SKILL_ALCHEMY)  // [eco194] the potions the members drank
                    PotionWork(row->low, p, supplyValue, supplyMin, supplyText);
                if (reach <= skill && supplyValue <= 0.0)
                {
                    key = std::string("blocked:") + (skill >= maxSkill ? "cap" : block.substr(0, 24));
                    decision = head + lag + " — cannot level now: " +
                               (skill >= maxSkill ? "rank cap " + std::to_string(maxSkill) +
                                                        (rank ? ", rank at the trainer" : ", rank waits for the level")
                                                  : block);
                }
                else
                {
                    double unlocked = 0;
                    std::string unlockedText;
                    uint32 unlockedUnits = 0;  // [lag328]
                    for (Need const* n : lineNeeds)
                        if (n->reqSkill > skill && n->reqSkill <= reach)
                        {
                            unlockedUnits += n->count;
                            unlocked += n->value;  // [craft428]
                            if (unlockedText.size() < 80)
                                unlockedText +=
                                    (unlockedText.empty() ? "" : ", ") + ItemName(n->itemId) + " (" + n->label + ")";
                        }
                    // [econ2] user rule 2026-11-08 (econ1 applied it to gathering): a profession is worth the
                    // upgrades and skill-ups it unlocks for the guild, not the vendor price of its skill-up
                    // products. b353: First Aid 320 c/min "beat" questing from Linen Bandage sales alone (bandages
                    // stay with the crafter, NewRpgCraft::OnCreateItem) - 108 of the run's 189 craft points went to
                    // bandages, 56 to Roasted Boar Meat, and the call opened the profession errands (trainers, tool
                    // vendors 1000-3400 yd away): PROFESSION 13 % of all bot time. The sales stay out of the value
                    (void)sales;
                    // [ench170] coordinator 2026-11-11: the walk is judged at its stopping point of the best value per
                    // minute, not only at its end - b443 every cook's walk to 175 read "150616 min" from Tangy Clam
                    // Meat past 160 (ench167), which hid the cheap part below it. Each point counts the members' needs
                    // it unlocks, its own costs and minutes; none of positive value: the whole walk as before
                    uint32 judged = reach;
                    {
                        double bestRate = 0.0;
                        for (Stop const& stop : stops)
                        {
                            double stopUnlocked = 0;
                            for (Need const* n : lineNeeds)
                                if (n->reqSkill > skill && n->reqSkill <= stop.reach)
                                    stopUnlocked += n->value;  // [craft428]
                            double const stopValue = stopUnlocked + supplyValue - stop.vendorCost;
                            double const stopMinutes = stop.craftMin + stop.gatherMin + supplyMin;
                            if (stopValue <= 0 || stopMinutes <= 0)
                                continue;
                            if (double const rate = stopValue / stopMinutes; rate >= bestRate)
                            {
                                bestRate = rate;
                                judged = stop.reach;
                                craftMin = stop.craftMin;
                                gatherMin = stop.gatherMin;
                                vendorCost = stop.vendorCost;
                                unlocked = stopUnlocked;
                                // [ench171] the calls buy and gather for the judged point only (judged again from
                                // there once it is reached: the walk runs every update from the skill it holds)
                                buyNow = stop.buyNow;
                                gatherNow = stop.gatherNow;
                            }
                        }
                        if (judged != reach)  // the needs the judged point unlocks
                        {
                            unlockedText.clear();
                            for (Need const* n : lineNeeds)
                                if (n->reqSkill > skill && n->reqSkill <= judged && unlockedText.size() < 80)
                                    unlockedText += (unlockedText.empty() ? "" : ", ") + ItemName(n->itemId) + " (" +
                                                    n->label + ")";
                        }
                    }
                    double const value = unlocked + supplyValue - vendorCost;
                    double const minutes = craftMin + gatherMin + supplyMin;
                    if (judged > skill && craftMin + gatherMin > 0.0)  // [craft426]
                        lineMinPerPoint_[line] = (craftMin + gatherMin) / double(judged - skill);
                    st.walkTarget[line] = judged;  // [craft430] the wanted list runs to it on a called line
                    float questPace = 0.0f;
                    std::string questWhy;
                    char numbers[320];
                    std::snprintf(numbers, sizeof(numbers),
                                  "to %u in %.1f min (craft %.1f, gather %.1f), value %s%s (unlocks %s + supplies %s "
                                  "- costs %s) [econ2]",
                                  judged, minutes, craftMin, gatherMin, value < 0 ? "-" : "",
                                  Gsc(uint64(std::fabs(value))).c_str(), Gsc(uint64(unlocked)).c_str(),
                                  Gsc(uint64(supplyValue)).c_str(), Gsc(uint64(vendorCost)).c_str());
                    std::string const stopText = judged == reach
                                                     ? std::string()
                                                     : Acore::StringFormat(
                                                           "; stops at {} of the walk to {}, the best value per "
                                                           "minute [ench170]",
                                                           judged, reach);
                    // [ench169] the item most of the gathering minutes go to (b443: the cooks' "to 175 in 150616 min"
                    // named none)
                    std::string slowestText;
                    if (auto const slowest =
                            std::max_element(gatherItemMin.begin(), gatherItemMin.end(),
                                             [](auto const& x, auto const& y) { return x.second < y.second; });
                        slowest != gatherItemMin.end() && gatherMin > 0)
                        slowestText = Acore::StringFormat("; slowest gathered: {} {:.1f} min [ench169]",
                                                          ItemName(slowest->first), slowest->second);
                    std::string const reached =
                        std::string(numbers) + (unlockedText.empty() ? "" : "; unlocks " + unlockedText) +
                        (supplyText.empty() ? "" : "; supplies [supply112]: " + supplyText) +
                        (guildText.empty() ? "" : "; from the guild: " + guildText + " [insc144]") +
                        (block.empty() ? "" : "; next: " + block) + slowestText + stopText;
                    // [lag328] coordinator 2026-11-11: members' needs this crafter holds (lag326: the free crafter of
                    // the line) that its skill-up walk unlocks are its work for the guild - the copper comparison
                    // weighs them at the items' vendor tags, and a glyph's is a token (BuyPrice 4s, no vendor sells
                    // one): b440 Dekorhan L21 held Glyph of Maul x5 and Serpent Sting x10, "value -2g 96s 78c
                    // (unlocks 1g 44s 0c - costs 4g 40s 78c)", all 7 scribes stayed at Inscription 1/75 with 39
                    // glyph needs listed. A crafter lag325 does not hold back takes the call; each step pays its
                    // game price when it runs (the reagent buy in lots its budget pays [lag327], a trainer rank waits
                    // for its money [prof272]) - b440 Dekorhan held 1g 64s against the whole walk's 4g 40s to 225.
                    // The copper comparison decides the rest
                    bool const memberNeeds = !levelsFirst && unlockedUnits;
                    // [craft397] questing without a value weighs 0 c/min: a profession of positive value (the cook's
                    // supply jobs) takes the call. b456: "Bami: Cook 103/150 ... quests without a value (no quest with
                    // data on the map): quests (... supplies 5s 76c ...)" - his Coyote Steak x29 job and Gnetlibozz's
                    // Crab Cake x24 never had a CRAFT call, 0 [guild70] cook lines, the guild's food came from members'
                    // surplus only
                    bool const questPriced = QuestPace(st, p, questPace, &questWhy);
                    if (!questPriced && value > 0)
                    {
                        questPace = 0.0f;
                        questWhy = "no quest with data on the map: questing weighs 0 c/min [craft397]";
                    }
                    if (!questPriced && !memberNeeds && value <= 0)
                    {
                        key = "noquestpace";
                        decision = head + lag + " — quests without a value (no quest with data on the map): quests (" +
                                   reached + ")";
                    }
                    else if (levelsFirst && value > 0 && value > double(questPace) * minutes)
                    {
                        // [lag325] below the guild's median level: quests first, the profession waits
                        key = "quest:levels-first";
                        decision = head + lag + Acore::StringFormat(" — L{} below the guild's median L{}: it levels "
                                                                    "first, quests [lag325] (",
                                                                    uint32(level), uint32(medianLevel)) +
                                   reached + ")";
                    }
                    else if (memberNeeds || (value > 0 && value > double(questPace) * minutes))
                    {
                        // [lag330] user rule 2026-11-11: guild orders mean the guild helps its crafter - the walk's
                        // costs its spare money does not cover come from the members' gold ([gold41] transfers)
                        st.ordersCall |= memberNeeds;  // [lag336]
                        // [craft380] its spare: the purse less its upkeep (repair, ammo, travel, food - the module
                        // budget's reserves before gear), what its walk may spend. b446: FoundingFreeMoney ("free
                        // money for anything", which also keeps back gear and tradeskill at L^3 - 1 g 6 s each at
                        // L22 - and a guild tabard's 1 g) read 0 for Dekorhan holding 49 s, so every letter he took
                        // was followed by another of the walk's whole vendor cost (4 s 64 c, 30 c postage each)
                        uint32 spare = p->GetMoney();
                        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                            spare = ai->GetAiObjectContext()
                                        ->GetValue<uint32>("free money for", std::to_string(uint32(NeedMoneyFor::gear)))
                                        ->Get();
                        if (memberNeeds)
                            if (vendorCost > double(spare))
                            {
                                st.profGoldShort += uint32(std::ceil(vendorCost)) - spare;
                                st.profGoldWhat += (st.profGoldWhat.empty() ? "" : ", ") +
                                                   std::string(SkillShort(line)) + " to " + std::to_string(judged) +
                                                   " for " + std::to_string(unlockedUnits) + " members' orders";
                            }
                        double const perMinute = minutes > 0 ? value / minutes : value;
                        char pace[120];
                        if (value > 0 && value > double(questPace) * minutes)
                            std::snprintf(pace, sizeof(pace), "profession %.0f c/min > quests %.0f c/min", perMinute,
                                          questPace);
                        else
                            std::snprintf(pace, sizeof(pace), "members' needs: %u unlocked on the way [lag328]",
                                          unlockedUnits);
                        auto const add = [&](GuildProfessionKind kind, uint32 itemId, double count, std::string details)
                        {
                            ProfessionPick pick;
                            pick.kind = kind;
                            pick.skillLine = line;
                            pick.itemId = itemId;
                            pick.count = uint32(std::ceil(count));
                            pick.details = std::move(details);
                            pick.reason = std::string(pace) + "; " + reached;
                            picks.push_back(pick);
                        };
                        std::string kinds;
                        // [insc144] at its rank cap the TRAIN call comes first: nothing to craft or buy before it
                        bool const below = skill < maxSkill;
                        if (below && (held || fromGuild || (!gatherItem && !buyItem)))
                        {
                            add(GuildProfessionKind::CRAFT, 0, 0,
                                std::string(SkillShort(line)) + " " + std::to_string(skill) + "→" +
                                    std::to_string(judged) + ": crafts from what it has");
                            kinds += "craft";
                            // [lag342] coordinator 2026-11-11: the craft runs in a safe place (a city or an inn's
                            // rest area, NewRpgCraft::InSafePlace) - its town is the nearest inn on its map, a stop of
                            // its route, weighed by the call's value ([lag337] / [lag340]: the walk's worth). Before,
                            // it waited for a town the bot happened to stand in: b445 Dekorhan's "Insc 1→105: crafts
                            // from what it has (a town)" after his parchment buy, then SFK five 13 took him. A smelt
                            // keeps its forge ([econ1])
                            if (line != SKILL_MINING)
                            {
                                // [lag343] picked once when the call is set, kept until it ends (or its map changes):
                                // the nearest inn of each look changed as the bot walked past inns, and with it the
                                // route's town list (a build each time)
                                WorldPosition& inn = st.craftInn[line];
                                // [craft383] ... or its walk there is given up at this level (the route drops the
                                // stop, [stuck12]) or refused: the nearest inn whose walk is not. b446 Dekorhan's
                                // Astranaar inn: "every way there passes mobs it does not pull", the stop closed
                                // unvisited and the call kept it - the craft never had a town to run in
                                auto const off = [&](WorldPosition const& at)
                                {
                                    return at.GetMapId() == p->GetMapId() &&
                                           (NewRpgBaseAction::WalkGivenUp(p, at.GetPositionX(), at.GetPositionY()) ||
                                            TownRefused(row->low, at));
                                };
                                // [craft384] ... or the bot quests in another zone now and an inn stands in it: b446
                                // Bami's "Cook 103→115 waits town Ratchet", picked in the Barrens, kept while he
                                // quested in Ashenvale (Astranaar's inn in his zone). Inns it walks past inside one
                                // zone keep the pick ([lag343])
                                auto const zoneOf = [](WorldPosition const& at)
                                {
                                    return sMapMgr->GetZoneId(PHASEMASK_NORMAL, at.GetMapId(), at.GetPositionX(),
                                                              at.GetPositionY(), at.GetPositionZ());
                                };
                                if (inn != WorldPosition() && inn.GetMapId() == p->GetMapId() &&
                                    zoneOf(inn) != p->GetZoneId())
                                    if (WorldPosition const here =
                                            sTravelRouteMgr.GetNearestInn(p, std::numeric_limits<float>::max(), off);
                                        here != WorldPosition() && here != inn && zoneOf(here) == p->GetZoneId() &&
                                        // [craft391] ... on the landmass it stands on: b452 Dekorhan's pick flipped
                                        // between the Darnassus and Dolanaar inns 8 times as the Rut'theran portal
                                        // took him between the zones, each flip a town stop across the water
                                        TravelRouteMgr::Landmass(here) == TravelRouteMgr::Landmass(WorldPosition(p)))
                                    {
                                        std::string const text = Acore::StringFormat(
                                            "{} L{} {} craft inn ({:.0f},{:.0f}) in another zone: the inn of its zone "
                                            "({:.0f},{:.0f})",
                                            p->GetName(), uint32(level), SkillShort(line), inn.GetPositionX(),
                                            inn.GetPositionY(), here.GetPositionX(), here.GetPositionY());
                                        if (std::string& logged = st.craftLogged["craft384"]; logged != text)
                                        {
                                            logged = text;
                                            LOG_INFO("playerbots", "[craft384] {}", text);
                                        }
                                        inn = here;
                                    }
                                if (inn != WorldPosition() && off(inn))
                                {
                                    WorldPosition const next =
                                        sTravelRouteMgr.GetNearestInn(p, std::numeric_limits<float>::max(), off);
                                    std::string const text = Acore::StringFormat(
                                        "{} L{} {} craft inn ({:.0f},{:.0f}): its walk there is given up or refused - "
                                        "the next inn ({:.0f},{:.0f})",
                                        p->GetName(), uint32(level), SkillShort(line), inn.GetPositionX(),
                                        inn.GetPositionY(), next.GetPositionX(), next.GetPositionY());
                                    if (std::string& logged = st.craftLogged["craft383"]; logged != text)
                                    {
                                        logged = text;
                                        LOG_INFO("playerbots", "[craft383] {}", text);
                                    }
                                    inn = next;
                                }
                                if (inn == WorldPosition() || inn.GetMapId() != p->GetMapId())
                                    inn = sTravelRouteMgr.GetNearestInn(p, std::numeric_limits<float>::max(), off);
                                picks.back().pos = inn;
                                picks.back().cost = value > 0.0 ? uint32(value) : 0;  // its worth, not a price
                            }
                        }
                        // [prof277] the item most units of the crafts below its rank cap take, its own count
                        auto const most = [](std::map<uint32, double> const& units)
                        {
                            std::pair<uint32, double> best{0, 0.0};
                            for (auto const& [item, n] : units)
                                if (n > best.second)
                                    best = {item, n};
                            return best;
                        };
                        auto const [buyNowItem, buyNowUnits] = most(buyNow);
                        auto const [gatherNowItem, gatherNowUnits] = most(gatherNow);
                        if (below && buyNowItem)
                        {
                            add(GuildProfessionKind::BUY, buyNowItem, buyNowUnits,
                                ItemName(buyNowItem) + " ×" + std::to_string(uint32(std::ceil(buyNowUnits))) + " for " +
                                    SkillShort(line) + " to " + std::to_string(std::min(judged, maxSkill)) +
                                    " [prof277][ench171]");
                            kinds += kinds.empty() ? "buy" : "+buy";
                            // [lag326] the nearest vendor that sells it (npc_vendor, its own map, no token cost) is
                            // the call's town: a stop of its route, as a TRAIN call's trainer ([prof272]). Before, the
                            // call ran "on its next town visit" in any town: b435-b437 Rollothan's Light Parchment
                            // x85 stood unbought (only inscription suppliers sell it, none in Westfall)
                            // [lag335] npc_vendor's own spawns (GuildGear::VendorsOf holds no reagent: b443 / b444
                            // Dekorhan's "Light Parchment x85" had no vendor town, [lag327] 0 buys)
                            float yards = std::numeric_limits<float>::max();
                            if (std::vector<ReagentVendor> const* spawns = ReagentVendors(buyNowItem))
                                for (ReagentVendor const& v : *spawns)
                                    if (!v.extendedCost && v.mapId == p->GetMapId() && FriendlyNpc(p, v.entry) &&
                                        !NewRpgProfession::BuyFailed(p, v.entry, buyNowItem) &&
                                        p->GetExactDist(v.x, v.y, v.z) < yards)
                                    {
                                        yards = p->GetExactDist(v.x, v.y, v.z);
                                        picks.back().pos = WorldPosition(v.mapId, v.x, v.y, v.z);
                                    }
                            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(buyNowItem))
                                picks.back().cost = proto->BuyPrice * uint32(std::ceil(buyNowUnits)) /
                                                    std::max<uint32>(proto->BuyCount, 1);
                        }
                        if (gatherNowItem)
                        {
                            add(GuildProfessionKind::GATHER, gatherNowItem, gatherNowUnits,
                                ItemName(gatherNowItem) + " ×" + std::to_string(uint32(std::ceil(gatherNowUnits))) +
                                    " for " + SkillShort(line) + " to " + std::to_string(std::min(judged, maxSkill)) +
                                    " [prof277][ench171]");
                            kinds += kinds.empty() ? "gather" : "+gather";
                        }
                        // [craft381] the trainer's rank / recipe of this line weighs what the walk past it is worth
                        // (at its rank cap no craft runs before it): b446 Journeyman Inscription would have weighed
                        // its 9 s 50 c fee alone against the boat to Darnassus
                        for (ProfessionPick& train : picks)
                            if (train.kind == GuildProfessionKind::TRAIN && train.skillLine == line && !train.itemId)
                                train.worth = value > 0.0 ? uint32(value) : 0;
                        key = "profession:" + kinds;
                        decision = head + lag + " — " + pace + " (" + questWhy + ") → PROFESSION " + kinds + " (" +
                                   reached + ")";
                    }
                    else
                    {
                        char pace[120];
                        std::snprintf(pace, sizeof(pace), "quests %.0f c/min ≥ profession %.0f c/min", questPace,
                                      minutes > 0 ? value / minutes : value);
                        key = "quest";
                        decision = head + lag + " — " + pace + " (" + questWhy + "): quests (" + reached + ")";
                    }
                }
            }
            if (st.profDecision[line] != key)
            {
                st.profDecision[line] = key;
                Decision(decision);
            }
        }
        // [gather] b337: the planned gathering lines (Herbalism, Mining, Skinning, Fishing) had no call at all - the
        // loop above takes the craft lines only - so the gatherers never learned Fishing nor bought a pole, and the
        // gathering plan found no fish place they could work ("fishing never started"). The trainer call: the line
        // when the trainer of its level teaches it and it is not learned, the next rank at its cap when its level
        // and skill allow it, the Fishing Pole when it fishes without one (NewRpgProfession's own trainer / tool
        // errands, with their own trip judge)
        for (auto const* list : {&row->profs, &row->secondary})
            for (uint32 line : *list)
            {
                if (line != SKILL_HERBALISM && line != SKILL_MINING && line != SKILL_SKINNING && line != SKILL_FISHING)
                    continue;
                bool const has = p->HasSkill(line);
                uint32 const skill = has ? p->GetPureSkillValue(line) : 0;
                uint32 const maxSkill = has ? p->GetPureMaxSkillValue(line) : 0;
                uint32 const cap = TrainerCap(line, level);
                std::string const name = line == SKILL_HERBALISM  ? "Herb"
                                         : line == SKILL_MINING   ? "Mine"
                                         : line == SKILL_SKINNING ? "Skin"
                                                                  : "fishing";
                std::string const head = Who(row->low) + ": " + name + " " + std::to_string(skill) + "/" +
                                         std::to_string(maxSkill) + " (trainer rank at L" + std::to_string(level) +
                                         ": " + std::to_string(cap) + ")";
                // [smelt] user 2026-11-08: received / mined ore gets used - a miner holding the ore of a smelt that
                // still gives it Mining skill smelts it (NewRpgCraft's forge walk), the global leader's CRAFT call
                if (line == SKILL_MINING && has)
                {
                    std::vector<std::pair<uint32, uint32>> ores;
                    uint32 bar = 0;
                    uint32 smelt = NextSmelt(p, &ores, &bar);
                    bool forGuild = false;
                    {
                        bool held = smelt && !ores.empty();
                        for (auto const& [ore, n] : ores)
                            held = held && p->GetItemCount(ore, false) >= n;
                        if (!held)  // [craft437] else a bar the guild's non-smelting crafters lack
                            if (uint32 const guildSmelt = GuildSmelt(p, &ores, &bar))
                            {
                                smelt = guildSmelt;
                                forGuild = true;
                            }
                    }
                    if (smelt)
                    {
                        bool enough = !ores.empty();
                        for (auto const& [ore, n] : ores)
                            enough = enough && p->GetItemCount(ore, false) >= n;
                        if (enough)
                        {
                            ProfessionPick pick;
                            pick.kind = GuildProfessionKind::CRAFT;
                            pick.skillLine = SKILL_MINING;
                            pick.itemId = bar;
                            pick.details = "smelt: " + ItemName(ores.front().first) + " → " + ItemName(bar) +
                                           " (mining " + std::to_string(skill) + "/" + std::to_string(maxSkill) + ")";
                            pick.reason = forGuild
                                              ? "ore in bags; the guild's crafters lack " + ItemName(bar) +
                                                    " and cannot smelt it [craft437]"
                                              : "ore in bags; smelting (spell " + std::to_string(smelt) +
                                                    ") gives Mining skill, bars go to blacksmiths / engineers";
                            // [econ1] the nearest town forge of its side on its map (its walk there not refused):
                            // smelted on its next visit to that town
                            float bestDist = FLT_MAX;
                            for (Mailbox const& forge : forges_)
                            {
                                if (forge.pos.GetMapId() != p->GetMapId() ||
                                    (forge.areaTeam == AREATEAM_ALLY && p->GetTeamId() != TEAM_ALLIANCE) ||
                                    (forge.areaTeam == AREATEAM_HORDE && p->GetTeamId() != TEAM_HORDE) ||
                                    TownRefused(row->low, forge.pos))
                                    continue;
                                float const d = p->GetExactDist(forge.pos.GetPositionX(), forge.pos.GetPositionY(),
                                                                forge.pos.GetPositionZ());
                                if (d < bestDist)
                                {
                                    bestDist = d;
                                    pick.pos = forge.pos;
                                }
                            }
                            picks.push_back(pick);
                        }
                    }
                }
                std::string key;
                std::string decision;
                auto const call = [&](std::string details, std::string reason, uint32 tool = 0, uint32 fee = 0)
                {
                    ProfessionPick pick;
                    pick.kind = GuildProfessionKind::TRAIN;
                    pick.skillLine = line;
                    pick.itemId = tool;  // [econ2] a tool buy: the tool's item (ProfessionErrand)
                    // [prof272] the fee: the tool's price, else the rank's
                    ItemTemplate const* toolProto = tool ? sObjectMgr->GetItemTemplate(tool) : nullptr;
                    pick.cost = toolProto ? toolProto->BuyPrice : fee;
                    // [econ2] the town of its nearest trainer (of the nearest tool vendor): its next visit there
                    auto const& lists = tool ? toolVendors_ : trainerSpots_;
                    if (auto const spots = lists.find(tool ? tool : line); spots != lists.end())
                        if (NpcSpot const* n = NearestSpot(p, row->low, spots->second))
                            pick.pos = n->spot.pos;
                    pick.details = std::move(details);
                    pick.reason = std::move(reason);
                    picks.push_back(pick);
                };
                if (line == SKILL_FISHING && !fishers_.count(row->low))
                {
                    // [fish110] the guild's fishing is its two fishers' role: no trainer or pole call for the others
                    key = "notfisher";
                    decision = head + " — not one of the guild's 2 fishers: no call [fish110]";
                }
                else if (!has && cap)
                {
                    call(std::string("learn ") + name,
                         "in the gatherer plan, the trainer teaches at L" + std::to_string(level), 0,
                         firstRankFee(line));
                    key = "learn";
                    decision = head + " — not learned: TRAIN at the trainer";
                }
                // [ench166] the pole before the rank: without one the fisher fishes nothing, at its rank cap too (the
                // guild needs its fish, not its skill). b443: both guild fishers stood at Fishing 225/225 with no pole
                // in their bags, the rank call ("TRAIN rank to 300", Feathermoon Stronghold) took the branch the pole
                // call came after, and every fish's walk read "no member gatherer reaches it (Fish 0) [ench163]"
                else if (has && line == SKILL_FISHING && !NewRpgFishing::HasPole(p))
                {
                    call("Fishing Pole", "fishing learned, no pole [ench166]", ProfessionTool(line));
                    key = "tool";
                    decision = head + " — no pole: buy at a vendor [ench166]";
                }
                else if (has && skill >= maxSkill && cap > maxSkill)
                {
                    TrainerRank const* rank = nullptr;
                    if (auto const ranks = trainerRanks_.find(line); ranks != trainerRanks_.end())
                        for (TrainerRank const& r : ranks->second)
                            if (r.maxSkill > maxSkill && r.reqLevel <= level && (!rank || r.maxSkill < rank->maxSkill))
                                rank = &r;
                    if (rank && skill >= rank->reqSkill)
                    {
                        call("rank to " + std::to_string(rank->maxSkill),
                             "skill " + std::to_string(skill) + " at the rank cap, level ≥ " +
                                 std::to_string(rank->reqLevel),
                             0, rank->price);
                        key = "rank";
                        decision = head + " — rank cap: TRAIN rank to " + std::to_string(rank->maxSkill);
                    }
                }
                // [econ2] the executor buys a tool only on the leader's call: a learned Mining / Skinning without
                // its tool (the node / the corpse needs it)
                else if (has && (line == SKILL_MINING || line == SKILL_SKINNING) && !HasProfessionTool(p, line))
                {
                    uint32 const tool = ProfessionTool(line);
                    call(ItemName(tool), std::string(name) + " learned, no tool", tool);
                    key = "tool";
                    decision = head + " — no " + ItemName(tool) + ": buy at a vendor";
                }
                if (key.empty())
                {
                    key = "ok";
                    decision = head + " — learned, no call";
                }
                if (st.profDecision[line] != key)
                {
                    st.profDecision[line] = key;
                    Decision(decision);
                }
            }
        // [econ2] a crafting line the leader calls (its value beat questing) whose crafts need a tool it lacks
        // (Blacksmith Hammer, Arclight Spanner, Jeweler's Kit, Virtuoso Inking Set): the nearest vendor's town
        {
            std::set<uint32> toolLines;
            for (ProfessionPick const& pick : picks)
                if (pick.kind == GuildProfessionKind::CRAFT && ProfessionTool(pick.skillLine) &&
                    !HasProfessionTool(p, pick.skillLine))
                    toolLines.insert(pick.skillLine);
            // [craft407] the enchanter's rod is its trade's tool, not only its skill-up's: the members' enchants
            // ([craft386]) and every enchant recipe need it. b460: the five enchanters' Enchanting calls lost to
            // questing (value -1s 14c), so no Copper Rod was ever called and none bought
            if (p->HasSkill(SKILL_ENCHANTING) && !HasProfessionTool(p, SKILL_ENCHANTING))
                toolLines.insert(SKILL_ENCHANTING);
            for (uint32 line : toolLines)
            {
                ProfessionPick pick;
                pick.kind = GuildProfessionKind::TRAIN;
                pick.skillLine = line;
                pick.itemId = ProfessionTool(line);
                if (auto const spots = toolVendors_.find(pick.itemId); spots != toolVendors_.end())
                    if (NpcSpot const* n = NearestSpot(p, row->low, spots->second))
                        pick.pos = n->spot.pos;
                pick.details = ItemName(pick.itemId);
                pick.reason = std::string("its called ") + SkillShort(line) + " crafts need it";
                if (ItemTemplate const* toolProto = sObjectMgr->GetItemTemplate(pick.itemId))
                    pick.cost = toolProto->BuyPrice;  // [prof272]
                // [craft381] the craft call runs where its tool is bought: without the tool no craft of the line runs
                // at its inn, and the tool vendor's town (an Inscription supplier stands in a capital, a safe place
                // for the craft executor) is one stop for both - weighed by the call's worth with the tool's price.
                // b446 Dekorhan: "Insc 1→105 waits town Astranaar" (an inn, nothing to craft there without the set)
                // beside "Virtuoso Inking Set waits town Darnassus" that weighed 0 c, never on his route
                if (pick.pos != WorldPosition())
                    for (ProfessionPick& craft : picks)
                        if (craft.kind == GuildProfessionKind::CRAFT && craft.skillLine == line &&
                            craft.pos != pick.pos)
                        {
                            craft.pos = pick.pos;
                            std::string const text = Acore::StringFormat(
                                "{} L{} {} craft call runs at its tool vendor ({:.0f},{:.0f}) with its {} buy ({}): "
                                "one town stop weighed by the call's worth and the tool's price",
                                p->GetName(), uint32(level), SkillShort(line), pick.pos.GetPositionX(),
                                pick.pos.GetPositionY(), ItemName(pick.itemId), Gsc(pick.cost));
                            if (std::string& logged = st.craftLogged["craft381"]; logged != text)
                            {
                                logged = text;
                                LOG_INFO("playerbots", "[craft381] {}", text);
                            }
                        }
                picks.push_back(pick);
            }
        }
        std::stable_sort(picks.begin(), picks.end(), [](ProfessionPick const& a, ProfessionPick const& b)
                         { return uint8(a.kind) > uint8(b.kind); });  // craft, buy, gather, then the trainer
        // [lag343] a line whose craft call is gone (done, or no longer called) lets its inn go
        for (auto itr = st.craftInn.begin(); itr != st.craftInn.end();)
            if (std::none_of(picks.begin(), picks.end(),
                             [line = itr->first](ProfessionPick const& pick)
                             { return pick.kind == GuildProfessionKind::CRAFT && pick.skillLine == line; }))
                itr = st.craftInn.erase(itr);
            else
                ++itr;
        professions_[row->low] = std::move(picks);
    }
    RankGuildCrafts(members, needs.size(), [&](size_t i) -> GuildNeedView
                    {
                        Need const& n = needs[i];
                        return {n.line, n.reqSkill, n.spellId, n.itemId, n.count, n.forLow, n.value};
                    },
                    [&](uint32 low, uint32 line) { return assigned.count({low, line}) != 0; },
                    gatherPace, copperPerWeight, bank);
}

// [craft435] user 2026-11-13 (b469: 5 rings worn of 150 finger slots, 0 necks of 75, the only jeweler at JC 14
// questing): "where we lag, there we craft". Each crafter's call was judged alone against its own questing and its
// chain ran as town tasks of the same weight as a turn-in (Colian's JC call read 32985 c/min > quests 324 c/min and
// still "waits town Blood Watch"). The guild's own ranking: each crafting line's gap - the members' needs its crafts
// fill that no crafter knows yet ([craft428] values, both slots of a ring / trinket pair) and the orders its crafters
// hold ([craft431]) - by its total worth, the biggest first; the bot-minutes of the whole chain (the crafter's
// skill-ups to the highest recipe, the crafts, the gathering of what the members do not hold, at the guild's pace)
// against the members' median questing. Logged on a change of the ranking
void GuildCoordinator::RankGuildCrafts(std::vector<std::pair<PlanRow const*, Player*>> const& members, size_t needCount,
                                       std::function<GuildNeedView(size_t)> const& needAt,
                                       std::function<bool(uint32, uint32)> const& crafterOf,
                                       std::function<float(uint32, uint32, uint32, Player*)> const& gatherPace,
                                       double copperPerWeight, std::map<uint32, uint32> const& bank)
{
    struct Gap
    {
        LinePriority out;
        std::set<uint32> members;
        std::set<uint32> crafters;
        std::map<std::string, uint32> kinds;  // what it fills -> slots
        uint32 maxReq{0};
        double craftMin{0.0};
    };
    std::map<uint32, Gap> gaps;
    auto const castMinutes = [](uint32 spellId, double crafts)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        return info ? crafts * info->CalcCastTime() / double(MINUTE * IN_MILLISECONDS) : 0.0;
    };
    auto const kindOf = [](uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            return std::string("?");
        if (proto->Class == ITEM_CLASS_CONTAINER)
            return std::string("bags");
        if (proto->Class == ITEM_CLASS_GLYPH)
            return std::string("glyphs");
        uint8 const group = GuildGear::EquipGroup(proto->InventoryType);
        return group < GuildGear::GROUP_COUNT ? std::string(GuildGear::GroupName(group)) + "s" : std::string("items");
    };
    auto const addCrafts = [&](Gap& gap, uint32 spellId, double crafts)
    {
        if (GuildGear::CraftRecipe const* r = GuildGear::Recipe(spellId))
            for (auto const& [item, n] : r->reagents)
                ExpandReagent(item, crafts * n, gap.out.raw, nullptr, 1.0f, 0, 0, ANY_MAP);
        gap.craftMin += castMinutes(spellId, crafts);
    };
    for (size_t i = 0; i < needCount; ++i)
    {
        GuildNeedView const n = needAt(i);
        if (n.value <= 0.0)
            continue;
        Gap& gap = gaps[n.line];
        gap.out.value += n.value;
        gap.out.slots += n.count;
        gap.kinds[kindOf(n.itemId)] += n.count;
        if (n.forLow)
            gap.members.insert(n.forLow);
        gap.maxReq = std::max(gap.maxReq, n.reqSkill);
        addCrafts(gap, n.spellId, double(n.count));
    }
    for (CraftJob const& job : crafts_)
    {
        GuildGear::CraftRecipe const* r = GuildGear::Recipe(job.spellId);
        if (!r)
            continue;
        Gap& gap = gaps[r->skillLine];
        ItemTemplate const* product = sObjectMgr->GetItemTemplate(job.itemId);
        gap.out.value += job.weight > 0.0 && copperPerWeight > 0.0 ? job.weight * copperPerWeight
                         : product                                ? double(product->BuyPrice)
                                                                  : 0.0;
        ++gap.out.slots;
        gap.kinds[kindOf(job.itemId)] += 1;
        gap.members.insert(job.forLow);
        gap.crafters.insert(job.crafterLow);
        if (!job.ready)
            addCrafts(gap, job.spellId, 1.0);
    }
    // the members' questing: the median copper per minute of the bots (the unit every call is weighed in)
    double questPerMinute = 0.0;
    {
        std::vector<float> paces;
        for (auto const& [row, m] : members)
            if (float copper = 0.0f; !HumanCharacter(m) && QuestPace(bots_[row->low], m, copper) && copper > 0.0f)
                paces.push_back(copper);
        if (!paces.empty())
        {
            std::nth_element(paces.begin(), paces.begin() + paces.size() / 2, paces.end());
            questPerMinute = paces[paces.size() / 2];
        }
    }
    std::map<uint32, double> held;  // what the members and the bank hold of a raw material
    auto const heldOf = [&](uint32 item)
    {
        auto it = held.find(item);
        if (it == held.end())
        {
            double n = 0.0;
            if (auto const b = bank.find(item); b != bank.end())
                n += b->second;
            for (auto const& [row, m] : members)
                n += m->GetItemCount(item, false);
            it = held.emplace(item, n).first;
        }
        return it->second;
    };
    std::vector<LinePriority> ranked;
    std::map<uint32, std::string> texts;
    for (auto& [line, gap] : gaps)
    {
        gap.out.line = line;
        gap.out.members = uint32(gap.members.size());
        // its crafter: the one the needs went to ([craft426]), else the highest skill of the line
        Player* crafter = nullptr;
        uint32 crafterSkill = 0;
        bool crafterChosen = false;
        for (auto const& [row, m] : members)
        {
            if (HumanCharacter(m) || !m->HasSkill(line))
                continue;
            uint32 const skill = m->GetPureSkillValue(line);
            bool const chosen = crafterOf(row->low, line) || gap.crafters.count(row->low);
            if (chosen)
                gap.crafters.insert(row->low);
            if (crafter && (crafterChosen != chosen ? crafterChosen : skill <= crafterSkill))
                continue;
            crafter = m;
            crafterSkill = skill;
            crafterChosen = chosen;
        }
        // its skill-ups to the highest recipe of the gap, from the skill it holds
        double skillMin = 0.0;
        if (crafter && gap.maxReq > crafterSkill)
        {
            std::map<size_t, double> crafts;
            SkillUpCrafts(crafter, line, crafterSkill, gap.maxReq, crafts);
            if (auto const list = recipesBySkill_.find(line); list != recipesBySkill_.end())
                for (auto const& [index, n] : crafts)
                {
                    Recipe const& r = list->second[index];
                    for (auto const& [item, count] : r.reagents)
                        ExpandReagent(item, std::ceil(n) * count, gap.out.raw, nullptr, 1.0f, 0, 0, ANY_MAP);
                    skillMin += castMinutes(r.spellId, std::ceil(n));
                }
        }
        // the gathering of what the members do not hold, at the guild's pace; the gatherers who reach it
        double gatherMin = 0.0;
        std::set<uint32> gatherers;
        std::string unreached;
        for (auto const& [item, units] : gap.out.raw)
        {
            if (VendorSource(item, crafter ? crafter->GetMapId() : ANY_MAP))
                continue;
            double const left = units - std::min(units, heldOf(item));
            uint32 skill = 0, req = 0;
            if (auto const src = gatherSource_.find(item); src != gatherSource_.end())
            {
                skill = src->second.first;
                req = src->second.second;
            }
            else if (int32 planReq = 0; sGuildGatherPlan.Gatherable(item, &skill, &planReq))
                req = uint32(std::max<int32>(0, planReq));
            for (auto const& [row, m] : members)
                if (skill && !HumanCharacter(m) && m->HasSkill(skill) && m->GetPureSkillValue(skill) >= req)
                    gatherers.insert(row->low);
            if (left <= 0.0)
                continue;
            float const pace = gatherPace(item, skill, req, nullptr);
            if (pace > 0.0f)
                gatherMin += left / pace;
            else if (unreached.size() < 60)
                unreached += (unreached.empty() ? "" : ", ") + ItemName(item) + " ×" +
                             std::to_string(uint32(std::ceil(left)));
        }
        gap.out.botMinutes = gap.craftMin + skillMin + gatherMin;
        double const perHour = gap.out.botMinutes > 0.0 ? gap.out.value / gap.out.botMinutes * 60.0 : 0.0;
        std::string kinds;
        for (auto const& [kind, slots] : gap.kinds)
            kinds += (kinds.empty() ? "" : "/") + kind;
        std::string crafters;
        for (uint32 low : gap.crafters)
            if (crafters.size() < 60)
                crafters += (crafters.empty() ? "" : ", ") + Who(low);
        texts[line] = Acore::StringFormat(
            "{} {}: {} slots of {} members, value {} ({:.0f} min of bot work: skill-ups {:.0f}, crafts {:.0f}, "
            "gathering {:.0f}) = {}/h vs quests {}/h ({} + {} gatherers){}",
            SkillShort(line), kinds, gap.out.slots, gap.out.members, Gsc(uint64(gap.out.value)), gap.out.botMinutes,
            skillMin, gap.craftMin, gatherMin, Gsc(uint64(perHour)), Gsc(uint64(questPerMinute * 60.0)),
            crafters.empty() ? std::string("no crafter") : crafters, gatherers.size(),
            unreached.empty() ? std::string() : "; no gatherer reaches " + unreached);
        ranked.push_back(gap.out);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](LinePriority const& a, LinePriority const& b) { return a.value > b.value; });
    std::string text, key;
    for (size_t i = 0; i < ranked.size(); ++i)
    {
        LinePriority const& r = ranked[i];
        bool const beats = r.botMinutes > 0.0 && r.value / r.botMinutes > questPerMinute;
        text += Acore::StringFormat("{}{}. {}{}", i ? "; " : "", i + 1, texts[r.line],
                                    beats ? " - beats questing" : " - quests pay more");
        key += Acore::StringFormat("{}:{}:{}:{};", r.line, r.slots, r.members, beats ? 1 : 0);
    }
    // [craft436] the ranking runs the chain: a line whose gap pays more per bot-minute than the members' questing
    // weighs its crafters' town stops (the craft, the tool / recipe / rank, the reagent buy: TownStopCopper) by its
    // whole gap, so the route takes the biggest gap's stops first; its raw materials are wanted at the gap's worth per
    // unit (UpdateWanted), which sends the gatherers who reach them
    linePriorities_.clear();
    for (LinePriority const& r : ranked)
    {
        if (r.botMinutes <= 0.0 || r.value / r.botMinutes <= questPerMinute)
            continue;
        linePriorities_.push_back(r);
        uint32 const worth = uint32(std::min(r.value, double(UINT32_MAX)));
        for (auto& [low, picks] : professions_)
            for (ProfessionPick& pick : picks)
                if (pick.skillLine == r.line)
                {
                    if (pick.kind == GuildProfessionKind::CRAFT)
                        pick.cost = std::max(pick.cost, worth);
                    else if (pick.kind == GuildProfessionKind::TRAIN || pick.kind == GuildProfessionKind::BUY)
                        pick.worth = std::max(pick.worth, worth);
                }
    }
    // [craft437] the bars the called crafters that cannot smelt lack (their walk's raw ore, UpdateWanted's lack),
    // less the bars other members hold: the miners who hold the ore smelt them for the guild (GuildSmelt)
    guildBars_.clear();
    for (auto const& [low, items] : crafterLack_)
    {
        Player* c = nullptr;
        for (auto const& [row, m] : members)
            if (row->low == low)
                c = m;
        if (!c || c->HasSkill(SKILL_MINING))
            continue;
        for (auto const& [ore, units] : items)
            for (auto const& [bar, parts] : smeltFrom_)
                for (auto const& [from, per] : parts)
                    if (from == ore && per > 0.0f)
                        guildBars_[bar] += double(units) / per;
    }
    for (auto itr = guildBars_.begin(); itr != guildBars_.end();)
    {
        itr->second -= heldOf(itr->first);
        itr = itr->second > 0.0 ? std::next(itr) : guildBars_.erase(itr);
    }
    if (key != linePrioritiesLogged_)
    {
        linePrioritiesLogged_ = key;
        LOG_INFO("playerbots", "[craft435] guild priorities: {}", text.empty() ? "no crafting gap" : text);
    }
}

float GuildCoordinator::PlannedGain(uint32 low, uint8 group) const
{
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(low);
    if (itr == bots_.end())
        return 0.0f;
    float best = 0.0f;
    for (GearPick const& g : itr->second.gearNow)
        if (g.group == group)
            best = std::max(best, g.gain);
    return best;
}

GuildCoordinator::NpcSpot const* GuildCoordinator::NearestSpot(Player* p, uint32 low,
                                                              std::vector<NpcSpot> const& list) const
{
    NpcSpot const* best = nullptr;
    float bestDist = 0.0f;
    for (NpcSpot const& n : list)
    {
        Mailbox const& s = n.spot;
        if (s.pos.GetMapId() != p->GetMapId() || !FriendlyNpc(p, n.entry) ||
            (s.areaTeam == AREATEAM_ALLY && p->GetTeamId() != TEAM_ALLIANCE) ||
            (s.areaTeam == AREATEAM_HORDE && p->GetTeamId() != TEAM_HORDE) || TownRefused(low, s.pos))
            continue;
        float const d = p->GetExactDist(s.pos.GetPositionX(), s.pos.GetPositionY(), s.pos.GetPositionZ());
        if (!best || d < bestDist)
        {
            best = &n;
            bestDist = d;
        }
    }
    return best;
}

// [econ2] b353: any profession call of the leader (most of them First Aid crafts "from what it has") opened the
// profession executor wherever the bot stood, and the executor ran its own errands: Ryarii L7 3378 yd + a boat to
// learn Mining, Shalaeth L5 2755 yd for a Mining Pick, Mutil 2257-2969 yd for Smelt Tin - 61 % of the 10326 s of
// PROFESSION time were such trips. A call is due only where it runs: a TRAIN call in the town of its trainer (its
// next visit there, TownReady), a CRAFT / BUY call in a town (the craft executor works in a rest area); the smelt
// runs through NextStep and gathering through its own task, neither opens the executor
// [lag327] the due BUY call's item for the profession executor (NewRpgProfession::StartTask buys it at a vendor of
// it in reach); the same "due" test as ProfessionCall
bool GuildCoordinator::ProfessionBuyDue(Player* bot, uint32& itemId, uint32& count, uint32& skillLine)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return false;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return false;
    for (GuildOrder const& o : itr->second.orders)
    {
        if (o.type != GuildOrderType::PROFESSION || o.profession != GuildProfessionKind::BUY || !o.itemId)
            continue;
        bool const ready = o.pos != WorldPosition() ? o.townArea && TownReady(bot, o) : NewRpgCraft::InSafePlace(bot);
        if (!ready)
            continue;
        itemId = o.itemId;
        count = o.count;
        skillLine = o.skillLine;
        return true;
    }
    return false;
}

// [craft441] the craft executor's own gate beside InSafePlace: the bot stands in the town of a CRAFT call's stop
// ([lag342], the inn the route walked it to). The smelt keeps its forge ([econ1]) and is not a place of its own
bool GuildCoordinator::CraftTownReady(Player* bot)
{
    if (!bot || !bot->IsAlive() || bot->IsInCombat() || !bot->GetMap() || bot->GetMap()->Instanceable())
        return false;
    if (!IsPlanned(bot->GetGUID().GetCounter()))
        return false;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return false;
    for (GuildOrder const& o : itr->second.orders)
        if (o.type == GuildOrderType::PROFESSION && o.profession == GuildProfessionKind::CRAFT &&
            o.skillLine != SKILL_MINING && o.pos != WorldPosition() && o.townArea && TownReady(bot, o))
            return true;
    return false;
}

GuildProfessionKind GuildCoordinator::ProfessionCall(Player* bot, uint32* skillLine)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return GuildProfessionKind::NONE;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return GuildProfessionKind::NONE;
    BotState& st = itr->second;
    GuildOrder const* due = nullptr;
    GuildOrder const* waiting = nullptr;
    for (GuildOrder const& o : st.orders)
    {
        if (o.type != GuildOrderType::PROFESSION || o.profession == GuildProfessionKind::NONE ||
            o.profession == GuildProfessionKind::GATHER ||
            (o.profession == GuildProfessionKind::CRAFT && o.skillLine == SKILL_MINING && o.pos != WorldPosition()))
            continue;
        // [lag326] a BUY call with its vendor's town: due in that town only
        // [craft441] ... and a CRAFT call with its inn's town ([lag342]): due where the leader sent it, as in any
        // safe place. NewRpgCraft::InSafePlace is the core's rest flag, and a bot holds REST_FLAG_IN_TAVERN only
        // when the client sends the inn's areatrigger, which no bot does - so for a bot it reads "in a capital".
        // b473: Orenem stood 2x ~300 s at his Astranaar craft stop with "PROFESSION/craft runnable" while the gate
        // read "waits: craft Tail 29→150 (Astranaar)", Tailoring 25→29 in 14 h and not one craft cast, while
        // Dekorhan (Insc) and Eraelor (LW) crafted in Darnassus
        bool const craftTown = o.profession == GuildProfessionKind::CRAFT && o.pos != WorldPosition() &&
                               o.townArea && TownReady(bot, o);
        bool const ready = o.profession == GuildProfessionKind::TRAIN ||
                                   (o.profession == GuildProfessionKind::BUY && o.pos != WorldPosition())
                               ? o.townArea && TownReady(bot, o)
                               : craftTown || NewRpgCraft::InSafePlace(bot);
        if (ready)
        {
            due = &o;
            break;
        }
        if (!waiting)
            waiting = &o;
    }
    std::string const gate = due ? std::string("due: ") + ProfessionKindName(due->profession) + " " + due->details
                             : waiting ? std::string("waits: ") + ProfessionKindName(waiting->profession) + " " +
                                             waiting->details +
                                             (waiting->townName.empty() ? " (a town)" : " (" + waiting->townName + ")")
                                       : std::string();
    if (gate != st.profGate)
    {
        st.profGate = gate;
        if (!gate.empty())
            LOG_INFO("playerbots", "[econ2] {} L{} profession call {}", bot->GetName(), bot->GetLevel(), gate);
    }
    if (!due)
        return GuildProfessionKind::NONE;
    if (skillLine)
        *skillLine = due->skillLine;
    return due->profession;
}

std::vector<uint32> GuildCoordinator::CalledRecipes(Player* bot, uint32 skillLine) const
{
    std::vector<uint32> out;
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return out;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return out;
    for (GuildOrder const& o : itr->second.orders)
        if (o.type == GuildOrderType::PROFESSION && o.profession == GuildProfessionKind::TRAIN &&
            o.skillLine == skillLine && o.spellId && !bot->HasSpell(o.spellId))
            out.push_back(o.spellId);
    return out;
}

bool GuildCoordinator::ProfessionErrand(Player* bot, uint32 skillLine, bool tool, WorldPosition const& npc)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return true;  // a bot outside the guild plan leads itself
    if (tool && !FreeBagSlots(bot))
        return false;  // the core refuses a buy into full bags (b353: Ryarii / Shalaeth 4 vendors each, 0 bought)
    // the npc stands in the town the bot is in now (a capital: anywhere inside the city)
    if (npc.GetMapId() != bot->GetMapId())
        return false;
    uint32 const npcArea = sMapMgr->GetAreaId(PHASEMASK_NORMAL, npc.GetMapId(), npc.GetPositionX(),
                                              npc.GetPositionY(), npc.GetPositionZ());
    uint32 const npcZone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, npc.GetMapId(), npc.GetPositionX(),
                                              npc.GetPositionY(), npc.GetPositionZ());
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(npcZone);
    bool const here = zone && (zone->flags & AREA_FLAG_CAPITAL) ? bot->GetZoneId() == npcZone
                                                                 : SameTown(bot->GetAreaId(), npcArea);
    if (!here)
        return false;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return false;
    for (GuildOrder const& o : itr->second.orders)
    {
        if (o.type != GuildOrderType::PROFESSION || o.skillLine != skillLine)
            continue;
        // its called tool buy; its training, or a recipe trip on a line it is called to train / craft
        if (tool ? o.profession == GuildProfessionKind::TRAIN && o.itemId && o.itemId == ProfessionTool(skillLine)
                 : o.profession == GuildProfessionKind::TRAIN || o.profession == GuildProfessionKind::CRAFT)
            return true;
    }
    return false;
}

bool GuildCoordinator::NeedsForOwnCalls(Player* bot, uint32 itemId) const
{
    if (!bot || !itemId || !IsPlanned(bot->GetGUID().GetCounter()))
        return false;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 const questId = bot->GetQuestSlotQuestId(slot))
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                quest && bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i] == itemId)
                        return true;
    CoordLock guard(lock_, __func__);
    auto const own = crafterNeed_.find(bot->GetGUID().GetCounter());
    if (own == crafterNeed_.end())
        return false;
    auto const need = own->second.find(itemId);
    return need != own->second.end() && need->second > 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// [quest14] user rule 2026-11-08: grey quests are not done unless needed (a chain step to a quest it would do, an
// upgrade, a recipe / skill / attunement / key / wanted item)
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
    struct GreyStatic
    {
        std::unordered_map<uint32, std::vector<uint32>> followers;  // quest -> the quests it opens or leads to
        std::unordered_map<uint32, TeamId> attuneQuests;            // dungeon_access_requirements quests
        std::unordered_set<uint32> attuneItems;                     // dungeon_access_requirements items
        std::unordered_set<uint32> keyItems;                        // Lock.dbc item keys
        // [inv380] item -> the quests that use it (an objective item, a source drop, the item the quest hands out):
        // a quest item in the bags belongs to one of them or to none at all
        std::unordered_map<uint32, std::vector<uint32>> itemQuests;
    };

    // [inv380] the items one quest ties to the bags: its objective items, its source drops and the item it handed out
    void QuestItems(Quest const* quest, std::set<uint32>& out)
    {
        if (!quest)
            return;
        if (uint32 const src = quest->GetSrcItemId())
            out.insert(src);
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                out.insert(quest->RequiredItemId[i]);
        for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
            if (quest->ItemDrop[i])
                out.insert(quest->ItemDrop[i]);
    }

    GreyStatic const& GreyTables()
    {
        static GreyStatic const tables = []()
        {
            GreyStatic t;
            auto const link = [&t](uint32 from, uint32 to)
            {
                if (from && to && from != to)
                    t.followers[from].push_back(to);
            };
            for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
            {
                if (!quest)
                    continue;
                // PrevQuestId (> 0 rewarded, < 0 active) and the NextQuestId / ExclusiveGroup links the core folds
                // into prevQuests; RewardNextQuest into prevChainQuests (ObjectMgr::LoadQuests)
                for (int32 const prev : quest->prevQuests)
                    link(uint32(std::abs(prev)), questId);
                for (uint32 const prev : quest->prevChainQuests)
                    link(prev, questId);
                link(questId, quest->GetNextQuestId());
                link(questId, quest->GetNextQuestInChain());
                link(questId, quest->GetBreadcrumbForQuestId());  // a breadcrumb leads to its target
                std::set<uint32> items;  // [inv380] the item -> quest index
                QuestItems(quest, items);
                for (uint32 const item : items)
                    t.itemQuests[item].push_back(questId);
            }
            for (auto& [questId, list] : t.followers)
            {
                std::sort(list.begin(), list.end());
                list.erase(std::unique(list.begin(), list.end()), list.end());
            }
            for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
            {
                if (!sMapStore.LookupEntry(i))
                    continue;
                for (uint8 d = 0; d < MAX_DIFFICULTY; ++d)
                    if (DungeonProgressionRequirements const* req = sObjectMgr->GetAccessRequirement(i, Difficulty(d)))
                    {
                        for (ProgressionRequirement const* q : req->quests)
                            if (q)
                                t.attuneQuests.emplace(q->id, q->faction);
                        for (ProgressionRequirement const* item : req->items)
                            if (item)
                                t.attuneItems.insert(item->id);
                    }
            }
            for (uint32 i = 0; i < sLockStore.GetNumRows(); ++i)
                if (LockEntry const* lock = sLockStore.LookupEntry(i))
                    for (uint8 j = 0; j < MAX_LOCK_CASE; ++j)
                        if (lock->Type[j] == LOCK_KEY_ITEM && lock->Index[j])
                            t.keyItems.insert(lock->Index[j]);
            LOG_INFO("playerbots",
                     "[quest14] grey tables: {} chain links, {} attunement quests, {} attunement items, {} key items, "
                     "{} quest items [inv380]",
                     t.followers.size(), t.attuneQuests.size(), t.attuneItems.size(), t.keyItems.size(),
                     t.itemQuests.size());
            return t;
        }();
        return tables;
    }

    // the profession skill line a spell teaches or ranks up (0: none)
    uint32 ProfessionOf(uint32 spellId)
    {
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info)
            return 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if ((info->Effects[i].Effect == SPELL_EFFECT_SKILL_STEP || info->Effects[i].Effect == SPELL_EFFECT_SKILL) &&
                IsProfessionSkill(uint32(info->Effects[i].MiscValue)))
                return uint32(info->Effects[i].MiscValue);
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (IsProfessionSkill(itr->second->SkillLine))
                return itr->second->SkillLine;
        return 0;
    }
}

std::string GuildCoordinator::QuestDroppedForValue(Player* bot, uint32 questId) const
{
    if (!bot || !questId)
        return std::string();
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return std::string();
    auto const dropped = itr->second.questDropped.find(questId);
    return dropped == itr->second.questDropped.end() ? std::string() : dropped->second;
}

bool GuildCoordinator::IsGreyQuest(Player* bot, Quest const* quest)
{
    return bot && quest && bot->GetQuestLevel(quest) <= int32(Acore::XP::GetGrayLevel(bot->GetLevel()));
}

void GuildCoordinator::LogGrey(Player* bot, char const* decision, Quest const* quest, std::string const& reason)
{
    if (!bot || !quest)
        return;
    static std::mutex logLock;
    static std::set<std::tuple<uint32, uint32, std::string>> logged;
    {
        std::lock_guard<std::mutex> guard(logLock);
        if (!logged.emplace(bot->GetGUID().GetCounter(), quest->GetQuestId(), decision).second)
            return;
    }
    LOG_INFO("playerbots", "[quest14] {} {} grey q{} {}: {}", bot->GetName(), decision, quest->GetQuestId(),
             quest->GetTitle(), reason);
}

std::string GuildCoordinator::GreyQuestNeed(Player* bot, Quest const* quest) const
{
    if (!bot || !quest)
        return std::string();
    GreyStatic const& tables = GreyTables();
    uint32 const low = bot->GetGUID().GetCounter();
    // (b) and (c) of one quest: itself as an attunement, the spell it teaches, its rewards
    auto const ownNeed = [&](Quest const* q) -> std::string
    {
        if (auto const att = tables.attuneQuests.find(q->GetQuestId());
            att != tables.attuneQuests.end() && (att->second == TEAM_NEUTRAL || att->second == bot->GetTeamId()) &&
            !bot->GetQuestRewardStatus(q->GetQuestId()))
            return "an attunement (a dungeon's access requirement)";
        for (int32 const spell : {q->GetRewSpellCast(), int32(q->GetRewSpell())})
        {
            SpellInfo const* info = spell > 0 ? sSpellMgr->GetSpellInfo(uint32(spell)) : nullptr;
            if (!info)
                continue;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (uint32 const learned =
                        info->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL ? info->Effects[i].TriggerSpell : 0)
                    if (uint32 const skill = ProfessionOf(learned); skill && !bot->HasSpell(learned))
                        return "it teaches profession spell " + std::to_string(learned) + " (skill " +
                               std::to_string(skill) + ")";
        }
        std::optional<StatsWeightCalculator> calc;  // built for the first equippable reward only
        for (uint8 i = 0; i < QUEST_REWARDS_COUNT + QUEST_REWARD_CHOICES_COUNT; ++i)
        {
            uint32 const item =
                i < QUEST_REWARDS_COUNT ? q->RewardItemId[i] : q->RewardChoiceItemId[i - QUEST_REWARDS_COUNT];
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item) : nullptr;
            if (!proto)
                continue;
            bool const lacks = !bot->HasItemCount(item, 1, true);
            if (lacks && tables.attuneItems.count(item))
                return "reward " + proto->Name1 + " is an attunement item";
            if (lacks && (proto->Class == ITEM_CLASS_KEY || tables.keyItems.count(item)))
                return "reward " + proto->Name1 + " is a key";
            if (proto->Class == ITEM_CLASS_RECIPE && proto->RequiredSkill && bot->HasSkill(proto->RequiredSkill))
                for (auto const& s : proto->Spells)
                    if (s.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID && s.SpellId > 0 &&
                        !bot->HasSpell(uint32(s.SpellId)))
                        return "reward " + proto->Name1 + " is a recipe of its profession it does not know";
            if (IsWanted(item))
                return "reward " + proto->Name1 + " is on the guild's wanted list";
            if (NeedsForOwnCalls(bot, item))
                return "reward " + proto->Name1 + " is needed for its own calls";
            if (proto->InventoryType != INVTYPE_NON_EQUIP)
            {
                if (!calc)
                {
                    calc.emplace(bot);
                    calc->SetItemSetBonus(false);
                    calc->SetOverflowPenalty(false);
                }
                GuildGear::Eval ev;
                if (GuildGear::Evaluate(bot, *calc, proto, ev) && ev.gain > 0.0f)
                    return "reward " + proto->Name1 + " is an upgrade for it (" +
                           GuildGear::GainText(ev.gain, ev.score, ev.current) + ")";
            }
            uint32 forLow = 0;
            if (CoordLock::Held())  // [quest120] the log's re-valuation asks under the hold it already has
                forLow = PlannedFor(item);
            else
            {
                CoordLock guard(lock_, __func__);
                forLow = PlannedFor(item);
            }
            if (forLow)
                return "reward " + proto->Name1 + " is an upgrade in " +
                       (forLow == low ? std::string("its") : Who(forLow) + "'s") + " gear plan";
        }
        return std::string();
    };
    if (std::string own = ownNeed(quest); !own.empty())
        return own;
    // (a) the chain: every quest it opens or leads to, onward; one the bot would do that is not grey or is needed
    uint8 const grayLevel = Acore::XP::GetGrayLevel(bot->GetLevel());
    std::unordered_set<uint32> seen{quest->GetQuestId()};
    std::vector<uint32> open{quest->GetQuestId()};
    while (!open.empty())
    {
        uint32 const from = open.back();
        open.pop_back();
        auto const next = tables.followers.find(from);
        if (next == tables.followers.end())
            continue;
        for (uint32 const id : next->second)
        {
            if (!seen.insert(id).second)
                continue;
            Quest const* follow = sObjectMgr->GetQuestTemplate(id);
            if (!follow || bot->GetQuestRewardStatus(id) || !bot->SatisfyQuestRace(follow, false) ||
                !bot->SatisfyQuestClass(follow, false) || !bot->SatisfyQuestExclusiveGroup(follow, false))
                continue;
            open.push_back(id);
            // a quest it would do: a normal or elite one, not repeatable or seasonal (NewRpgBaseAction's filters)
            if (follow->IsRepeatable() || follow->IsSeasonal() ||
                (follow->GetType() != 0 && follow->GetType() != QUEST_TYPE_ELITE))
                continue;
            if (bot->GetQuestLevel(follow) > int32(grayLevel))
                return "it leads to q" + std::to_string(id) + " " + follow->GetTitle() + " (not grey)";
            if (std::string own = ownNeed(follow); !own.empty())
                return "it leads to q" + std::to_string(id) + " " + follow->GetTitle() + ": " + own;
        }
    }
    return std::string();
}

// ---------------------------------------------------------------------------------------------------------------------
// [inv380] user 2026-11-13: a member carries nothing it holds for nothing. Three kinds filled its bags -
//  1 grey junk with a vendor price: OnLoot never saw it (it returns for ITEM_QUALITY_POOR), so the leader marked
//    none of it, no SELL task named it and the bags only emptied when a sell run crossed its own line
//    ([vendor15]: free slots down to the working quest's item objectives). AddDeadWeight marks it: sold at the next
//    vendor on the way, never a trip of its own;
//  2 a quest-starter item the bot refuses to open: its usage falls to ITEM_USAGE_QUEST while it can take the quest
//    and to ITEM_USAGE_NONE when the grey rule refuses it ([quest14]), and nothing removed it below 90 % bags
//    (SmartDestroyItemAction). Held open for days;
//  3 a quest item of no quest of its log (abandoned, handed in, or a branch it never took): no vendor buys it
//    (SellPrice 0, BIND_QUEST_ITEM), so only the same 90 % destroy could free the slot.
// 2 and 3 need the grey rule, which reads lock_ itself, so they are decided on the bot's own thread here and the
// bot destroys them (DropDeadWeightAction); 1 is the leader's SELL task (AddDeadWeight, lock_ held).
// ---------------------------------------------------------------------------------------------------------------------
std::vector<GuildCoordinator::DeadItem> GuildCoordinator::QuestDeadWeight(Player* bot) const
{
    std::vector<DeadItem> dead;
    // a full quest log refuses every accept, so a starter it would open looks refused: no verdict this pass
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()) || HumanCharacter(bot) || !bot->SatisfyQuestLog(false))
        return dead;
    // what its log ties to its bags: an objective item, a source drop, the item a quest handed it (StartItem - the
    // hand-in item of q968 The Powers Below, of the Learn to Ride quests). Those never go
    std::set<uint32> questNeeds;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        QuestItems(sObjectMgr->GetQuestTemplate(bot->GetQuestSlotQuestId(slot)), questNeeds);
    GreyStatic const& tables = GreyTables();
    // a quest the bot will never take: it is done with it, its class or race is wrong, or the grey rule refuses it
    auto const refused = [&](Quest const* quest) -> std::string
    {
        if (!quest)
            return "it is no quest of this server";
        uint32 const id = quest->GetQuestId();
        if (bot->GetQuestRewardStatus(id))
            return "q" + std::to_string(id) + " " + quest->GetTitle() + " is handed in";
        if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false))
            return "q" + std::to_string(id) + " " + quest->GetTitle() + " is for another class or race";
        if (IsGreyQuest(bot, quest))
            if (std::string const need = GreyQuestNeed(bot, quest); need.empty())
                return "it refuses grey q" + std::to_string(id) + " " + quest->GetTitle() +
                       " (no chain step, upgrade, recipe, skill, attunement, key or wanted item)";
        return std::string();
    };
    auto const look = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || questNeeds.count(proto->ItemId))
            return;
        uint32 const guid = item->GetGUID().GetCounter();
        if (uint32 const startId = proto->StartQuest)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(startId);
            if (quest && bot->GetQuestStatus(startId) != QUEST_STATUS_NONE)
                return;  // it is on the quest; the hand-in item case went out above
            if (quest && QuestAction::CanAcceptQuest(bot, quest))
                return;  // it opens the quest from the bags (UseStartQuestItem)
            // not at this level yet: the quest still comes
            if (quest && bot->GetLevel() < quest->GetMinLevel() && !bot->GetQuestRewardStatus(startId))
                return;
            if (std::string why = refused(quest); !why.empty())
                dead.push_back({guid, proto->ItemId, why});
            return;
        }
        if (proto->Class != ITEM_CLASS_QUEST && proto->Bonding != BIND_QUEST_ITEM)
            return;
        auto const uses = tables.itemQuests.find(proto->ItemId);
        if (uses == tables.itemQuests.end())
        {
            dead.push_back({guid, proto->ItemId, "no quest of this server uses it"});
            return;
        }
        std::string why;
        for (uint32 const id : uses->second)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            std::string const no = refused(quest);
            if (no.empty())
                return;  // it may still take this one: the item waits for it
            if (why.empty())
                why = no;
        }
        dead.push_back({guid, proto->ItemId, why + ", and no other quest of its wants it"});
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        if (Bag* bag = bot->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                look(bag->GetItemByPos(uint8(j)));
    return dead;
}

// [inv380] map thread: the bot drops what the leader's rule found it carries for nothing. Between its steps, as the
// disenchant queue runs: no walk, no cast, and no vendor could take these items anyway.
bool GuildCoordinator::DropDeadWeightNow(Player* bot)
{
    if (!bot || !bot->IsAlive() || bot->IsInCombat())
        return false;
    std::vector<DeadItem> const dead = QuestDeadWeight(bot);
    bool dropped = false;
    for (DeadItem const& item : dead)
    {
        Item* held = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(item.itemGuidLow));
        ItemTemplate const* proto = held ? held->GetTemplate() : nullptr;
        if (!proto)
            continue;
        LOG_INFO("playerbots", "[inv380] {} L{} drops {}{}: {}", bot->GetName(), bot->GetLevel(), proto->Name1,
                 held->GetCount() > 1 ? " ×" + std::to_string(held->GetCount()) : "", item.why);
        bot->DestroyItem(held->GetBagSlot(), held->GetSlot(), true);
        dropped = true;
    }
    return dropped;
}

// [inv380] lock_ held. The grey junk a member carries onto its SELL task, and the bag counts for the bags line: the
// quest-starter items it holds unopened and the quest items no quest of its log needs (their verdict is the bot's
// own thread, QuestDeadWeight).
void GuildCoordinator::AddDeadWeight(BotState& st, uint32 low, Player* p)
{
    std::set<uint32> known;
    for (HeldItem const& h : st.held)
        known.insert(h.itemGuidLow);
    for (uint32 guid : st.sell)
        known.insert(guid);
    for (uint32 guid : st.disenchant)
        known.insert(guid);
    std::set<uint32> questNeeds;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        QuestItems(sObjectMgr->GetQuestTemplate(p->GetQuestSlotQuestId(slot)), questNeeds);
    auto const own = crafterNeed_.find(low);
    uint32 junk = 0, starters = 0, questDead = 0;
    auto const look = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || questNeeds.count(proto->ItemId))
            return;
        if (proto->StartQuest)
        {
            uint32 const startId = proto->StartQuest;
            if (p->GetQuestStatus(startId) == QUEST_STATUS_NONE && !p->GetQuestRewardStatus(startId))
                ++starters;
            return;  // the bot's own thread decides it (the grey rule)
        }
        if (proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM)
        {
            ++questDead;
            return;
        }
        uint32 const guid = item->GetGUID().GetCounter();
        if (proto->Quality != ITEM_QUALITY_POOR || !proto->SellPrice || TransferHolds(guid))
            return;
        // what the guild still does with it: the wanted list, this member's own called skill-ups, a guild cook
        if (CookNeeds(proto->ItemId) || (own != crafterNeed_.end() && own->second.count(proto->ItemId)) ||
            std::any_of(wanted_.begin(), wanted_.end(),
                        [&](WantedItem const& w) { return w.itemId == proto->ItemId && (w.need || w.optional); }))
            return;
        ++junk;  // the slot it costs, whether this pass marked it or an earlier one did
        if (known.count(guid))
            return;
        st.sell.push_back(guid);
        LOG_INFO("playerbots", "[inv380] {} L{} junk {}{} → sell: grey, a vendor pays {}, no quest of its log, no "
                 "crafter and no cook wants it", p->GetName(), p->GetLevel(), proto->Name1,
                 item->GetCount() > 1 ? " ×" + std::to_string(item->GetCount()) : "",
                 MoneyText(proto->SellPrice * item->GetCount()));
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        if (Bag* bag = p->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                look(bag->GetItemByPos(uint8(j)));
    st.junkSlots = junk;
    st.startersOpen = starters;
    st.questDeadSlots = questDead;
}

GuildCoordinator::RepairCall GuildCoordinator::Repair(Player* bot) const
{
    if (!bot)
        return RepairCall();
    std::lock_guard<std::mutex> guard(repairLock_);
    auto const found = repairCalls_.find(bot->GetGUID().GetCounter());
    return found == repairCalls_.end() ? RepairCall() : found->second;
}

bool GuildCoordinator::UrgentTask(Player* bot, std::string* why) const
{
    if (!bot)
        return false;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return false;
    BotState const& st = itr->second;
    auto const said = [why](std::string const& text)
    {
        if (why)
            *why = text;
        return true;
    };
    if (st.fiveId)
        return said("five #" + std::to_string(st.fiveId));
    for (GuildOrder const& o : st.orders)
    {
        if (!o.executable)
            continue;
        if (st.run && o.type == st.runType && o.priority == GuildTaskPriority::URGENT)
            return said(std::string(OrderName(o.type)) + " (walks to the vault)");
        if (o.priority == GuildTaskPriority::URGENT && !o.waitsTown)
            return said(std::string(OrderName(o.type)) + ": " + o.details);
        break;  // the first executable order decides
    }
    return false;
}

// [lag337] b444 [health] "waiting for town" 14 -> 29 -> 77 -> 79 in 1.5 h (40 MAIL_TO, 24 BUY, 4 TRAIN, 4 CRAFT, 3
// REPAIR waiting): a town stop carried no weight in the route (only its party members' hand-ins, [quest16]), so
// HandInsEarlier never moved it forward and the shortest order put it where it adds the least walk - often last of
// a 60-70 min route that is built again before it is reached (182 builds, 73 with a town stop, 26 town stops walked).
// Its tasks' game prices give it a weight: the price a buy, repair, rank or reagent costs (what the bot pays for it is
// what it is worth to it at least), the vendor price of an item a mail / trade carries to a member, the gold a
// pickup's letters hold - over the bot's copper per quest experience, the unit questing is weighed in ([econ1])
float GuildCoordinator::TownStopXp(Player* bot, WorldPosition const& pos) const
{
    if (!bot)
        return 0.0f;
    float perXp = 0.0f;
    {
        CoordLock guard(lock_, __func__);
        auto const itr = bots_.find(bot->GetGUID().GetCounter());
        if (itr == bots_.end() || itr->second.copperPerXp <= 0.0f)
            return 0.0f;
        perXp = itr->second.copperPerXp;
    }
    return float(TownStopCopper(bot, pos) / double(perXp));
}

float GuildCoordinator::QuestCopperPerMinute(Player* bot)
{
    if (!bot)
        return 0.0f;
    float copper = 0.0f;
    {
        CoordLock guard(lock_, __func__);
        auto const itr = bots_.find(bot->GetGUID().GetCounter());
        if (itr == bots_.end() || !QuestPace(itr->second, bot, copper))
            return 0.0f;
    }
    // [assist22] the unlock's critical member: its questing is worth its guild weight (the time it trades for a fare,
    // a town stop on another landmass)
    return copper * UnlockWeight(bot->GetGUID().GetCounter());
}

double GuildCoordinator::TownStopCopper(Player* bot, WorldPosition const& pos) const
{
    if (!bot)
        return 0.0;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return 0.0;
    double copper = 0.0;
    bool pickup = false;
    for (GuildOrder const& o : itr->second.orders)
    {
        if (!o.executable || o.pos == WorldPosition() || o.pos.GetMapId() != pos.GetMapId() ||
            o.pos.GetExactDist2d(pos.GetPositionX(), pos.GetPositionY()) > 1.0f)  // the same spot (TownStops)
            continue;
        // [lag344] gold the bot gives away (DONATE_GOLD: the sender's mail / trade of a member's gold) is no gain to
        // it at its own price, and to the guild only money moving between members: it weighs nothing here. b445:
        // "[lag337] Brelir L22 town stop DONATE_GOLD Blood Watch weighs 14565 quest xp" - his share of the guild's
        // first bank tab (100 g, the members' spare gold to the guild master) counted as his own gain
        if (o.type != GuildOrderType::DONATE_GOLD)
            copper += double(o.cost);
        if ((o.type == GuildOrderType::MAIL_TO || o.type == GuildOrderType::TRADE_TO) && o.itemId)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(o.itemId))
                copper += double(proto->BuyPrice) * std::max<uint32>(1, o.count);
        pickup |= o.type == GuildOrderType::MAIL_TO && !o.transferId;
    }
    // [inv374] the pickup by what its delivered letters carry: their gold, the vendor price of a member's item (as
    // the sender's mail above), what a game letter's item sells for or the quest it opens is worth at the bot's
    // copper per quest XP, and the worth of the craft call a reagent / supply or a profession orders' gold waits for
    // (b446: the pickup weighed the members' gold only - 4 upgrades, 7 greens to disenchant and 43 riding
    // pamphlets weighed nothing, a town stop the route put last)
    if (pickup)
    {
        uint32 const low = bot->GetGUID().GetCounter();
        uint32 const now = Now();
        double gold = 0.0, items = 0.0, quests = 0.0, call = 0.0;
        for (Mail const* m : bot->GetMails())
        {
            if (!m || m->state == MAIL_STATE_DELETED || m->COD || uint32(m->deliver_time) > now)
                continue;
            bool const game = GameLetter(m);
            if (!game && (m->messageType != MAIL_NORMAL || !IsPlanned(m->sender)))
                continue;
            gold += double(m->money);
            for (MailItemInfo const& info : m->items)
            {
                ItemTemplate const* proto = game ? sObjectMgr->GetItemTemplate(info.item_template) : nullptr;
                if (!proto || !LetterItemWorth(bot, proto))
                    continue;
                Item const* it = bot->GetMItem(info.item_guid);
                items += double(proto->SellPrice) * (it ? it->GetCount() : 1);
                if (Quest const* quest = proto->StartQuest ? sObjectMgr->GetQuestTemplate(proto->StartQuest) : nullptr)
                    if (bot->GetQuestStatus(proto->StartQuest) == QUEST_STATUS_NONE)
                        quests += double(quest->XPValue(bot->GetLevel())) * itr->second.copperPerXp;
            }
        }
        bool fundsCall = false;
        for (auto const& [id, t] : transfers_)
        {
            if (t.toLow != low || !t.mailed || t.deliverAt > now)
                continue;
            if (!t.gold)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(t.itemId))
                    items += double(proto->BuyPrice) * std::max<uint32>(1, t.count);
            fundsCall |= (!t.gold && (t.purpose == "reagent" || t.purpose == "supply")) ||
                         (t.gold && t.purpose == "profession orders");
        }
        if (fundsCall)
            for (GuildOrder const& o : itr->second.orders)
                if (o.type == GuildOrderType::PROFESSION && o.profession == GuildProfessionKind::CRAFT)
                    call = std::max(call, double(o.cost));
        double const worth = gold + items + quests + call;
        copper += worth;
        static std::map<uint32, uint32> logged;  // [inv374] member -> the pickup's worth last logged (under lock_)
        if (uint32 const w = uint32(std::min(worth, double(UINT32_MAX))); logged[low] != w)
        {
            logged[low] = w;
            LOG_INFO("playerbots", "[inv374] {} L{} mail pickup worth {}c: gold {:.0f}c, items {:.0f}c, quests "
                     "opened {:.0f}c, the craft call it waits for {:.0f}c", bot->GetName(), bot->GetLevel(), w, gold,
                     items, quests, call);
        }
    }
    return copper;
}

void GuildCoordinator::TownStops(Player* bot, std::vector<std::pair<WorldPosition, std::string>>& out) const
{
    out.clear();
    if (!bot)
        return;
    uint32 const low = bot->GetGUID().GetCounter();
    std::vector<std::pair<WorldPosition, std::string>> stops;
    {
        CoordLock guard(lock_, __func__);
        auto const itr = bots_.find(low);
        if (itr == bots_.end())
            return;
        // [eco199] a five member's route takes no town stop - but while its five gathers (not in the dungeon), the
        // pickup of the members' gold its planned need waits for
        bool const inFive = itr->second.fiveId != 0;
        if (inFive)
        {
            auto const five = fives_.find(itr->second.fiveId);
            if (five == fives_.end() || five->second.stage != GuildFive::Stage::Gather)
                return;
        }
        // [econ2] world2 [town12]: the mail pickup (MAIL_TO without a transfer, GuildTransfer TransferOrders) is a
        // town stop until the bot stands in its mailbox's town - whatever its waitsTown flag says - so a party
        // leader's route takes its followers there (b353: party followers held most of the ~100 letters)
        for (GuildOrder const& o : itr->second.orders)
            if (o.executable && o.priority == GuildTaskPriority::CONVENIENT && o.pos != WorldPosition() &&
                o.pos.GetMapId() == bot->GetMapId() &&
                (!inFive || (o.type == GuildOrderType::MAIL_TO && o.needGold)) &&
                (o.waitsTown || (o.type == GuildOrderType::MAIL_TO && !o.transferId &&
                                 (!o.townArea || !TownReady(bot, o)))))
                stops.emplace_back(o.pos,
                                   std::string(OrderName(o.type)) + (o.townName.empty() ? "" : " " + o.townName));
    }
    // [econ1] b349/b350 Geron: MAIL_TO waited for Kharanos 1083 yd away, its walk there was given up and the route
    // put the stop back again (182 MAIL_TO lines, ~70 min). A stop its own trip judge refuses (a danger on the way,
    // or the walk given up: NewRpgBaseAction::TripRefused) is left out and remembered: the planner then picks
    // another mailbox / channel for it (UpdateTransfers), until its next level. Judged outside lock_ (map thread)
    std::vector<std::pair<int32, int32>> refused;
    for (auto& stop : stops)
    {
        WorldPosition const& pos = stop.first;
        if (NewRpgBaseAction::TripRefused(bot, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                                          "town task " + stop.second))
            refused.emplace_back(int32(pos.GetPositionX()), int32(pos.GetPositionY()));
        else
            out.push_back(std::move(stop));
    }
    if (refused.empty())
        return;
    CoordLock guard(lock_, __func__);
    auto& entry = townRefused_[low];
    if (entry.first != bot->GetLevel())
        entry = {bot->GetLevel(), {}};
    for (auto const& xy : refused)
        if (entry.second.insert_or_assign(xy, bot->GetZoneId()).second)
            LOG_INFO("playerbots", "[econ1] {} L{} town stop refused at ({},{}): its walk there is refused or "
                     "given up - another mailbox / forge or channel is planned", bot->GetName(), bot->GetLevel(),
                     xy.first, xy.second);
}

bool GuildCoordinator::CookNeeds(uint32 itemId) const
{
    auto const cooking = recipesBySkill_.find(SKILL_COOKING);
    for (auto const& [cookLow, jobs] : supplyCrafts_)
    {
        for (SupplyCraft const& job : jobs)
            for (auto const& [item, n] : job.reagents)
                if (item == itemId)
                    return true;
        Player* cook = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(cookLow));
        if (!cook || !cook->IsInWorld() || !cook->HasSkill(SKILL_COOKING) || cooking == recipesBySkill_.end())
            continue;
        uint32 const skill = cook->GetPureSkillValue(SKILL_COOKING);
        for (Recipe const& r : cooking->second)
            if (skill < r.gray && cook->HasSpell(r.spellId))
                for (auto const& [item, n] : r.reagents)
                    if (item == itemId)
                        return true;
    }
    return false;
}

bool GuildCoordinator::TownRefused(uint32 low, WorldPosition const& pos) const
{
    auto const itr = townRefused_.find(low);
    if (itr == townRefused_.end())
        return false;
    Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
    if (!p || p->GetLevel() != itr->second.first)
        return false;
    // [craft387] a refusal judged in another zone than the one it stands in now: the way there is another way
    auto const spot = itr->second.second.find({int32(pos.GetPositionX()), int32(pos.GetPositionY())});
    return spot != itr->second.second.end() && spot->second == p->GetZoneId();
}

bool GuildCoordinator::QuestWorkOf(Player* bot, uint32 questId, float dps, QuestWork& out) const
{
    out = QuestWork();
    auto const q = questStatic_.find(questId);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!bot || !quest || q == questStatic_.end())
        return false;
    QuestStatic const& s = q->second;
    auto const data = bot->getQuestStatusMap().find(questId);
    QuestStatusData const* status = data == bot->getQuestStatusMap().end() ? nullptr : &data->second;
    uint32 left = 0, total = 0;
    double seconds = 0.0;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        uint32 const need = quest->RequiredNpcOrGoCount[i];
        if (!need || !quest->RequiredNpcOrGo[i])
            continue;
        uint32 const done = status ? std::min<uint32>(status->CreatureOrGOCount[i], need) : 0;
        total += need;
        left += need - done;
        if (s.npcHp[i] > 0.0f && need > done)
        {
            if (dps <= 0.0f)
                return false;
            seconds += double(need - done) * s.npcHp[i] / dps;
        }
    }
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const need = quest->RequiredItemCount[i];
        if (!need || !quest->RequiredItemId[i])
            continue;
        uint32 const done = status ? std::min<uint32>(status->ItemCount[i], need) : 0;
        total += need;
        left += need - done;
        if (s.itemHp[i] > 0.0f && need > done)
        {
            if (dps <= 0.0f)
                return false;
            seconds += double(need - done) * s.itemHp[i] / dps;
        }
    }
    out.map = s.map;
    out.killSeconds = float(seconds);
    out.tourYd = s.tourYd * (total ? float(left) / float(total) : 1.0f);
    out.areaX = s.areaX;
    out.areaY = s.areaY;
    out.ender = s.ender;
    out.enderX = s.enderX;
    out.enderY = s.enderY;
    return true;
}

void GuildCoordinator::AddCoopTask(uint32 low, uint32 partnerLow, uint32 questId, bool leads, std::string const& why)
{
    GuildOrder o;
    o.type = GuildOrderType::COOP;
    o.itemId = questId;  // the task's key: one co-op task per quest
    o.forLow = partnerLow;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    o.details =
        std::string(leads ? "leads " : "helps ") + Who(partnerLow) + ": " +
        (questId ? "q" + std::to_string(questId) + (quest ? " " + quest->GetTitle() : "") : CampText(partnerLow, low));
    o.reason = why;
    o.status = "co-op party (New RPG)";
    o.executable = false;  // New RPG co-op runs it, never NextStep
    CoordLock guard(lock_, __func__);
    ClassifyTask(o);
    BotState& st = bots_[low];
    o.since = Now();
    st.external.erase(std::remove_if(st.external.begin(), st.external.end(),
                                     [&](GuildOrder const& e)
                                     { return e.type == GuildOrderType::COOP && e.itemId == questId; }),
                      st.external.end());
    st.external.push_back(o);
    st.ranTasks.insert(TaskKey(o));  // it runs from now on: its end is "done"
}

void GuildCoordinator::EndCoopTask(uint32 low, uint32 questId, std::string const& why)
{
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(low);
    if (itr == bots_.end())
        return;
    auto& external = itr->second.external;
    size_t const before = external.size();
    external.erase(std::remove_if(external.begin(), external.end(),
                                  [&](GuildOrder const& e)
                                  { return e.type == GuildOrderType::COOP && e.itemId == questId; }),
                   external.end());
    if (external.size() != before)
        LOG_INFO("playerbots", "[tasks] {} ends COOP {}: {}", Who(low),
                 questId ? "q" + std::to_string(questId) : std::string("camp"), why);
}

// [econ1] a camp help request has no quest (b350 task lines "q0"): the camp by the area / zone of the member who asked
// (it stands at its refused camp), else of the helper
std::string GuildCoordinator::CampText(uint32 requesterLow, uint32 helperLow) const
{
    for (uint32 low : {requesterLow, helperLow})
    {
        Player* p = low ? ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low)) : nullptr;
        if (!p || !p->IsInWorld())
            continue;
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(p->GetAreaId());
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(p->GetZoneId());
        std::string text = "camp";
        if (area && area != zone)
            text += std::string(" ") + area->area_name[0];
        if (zone)
            text += std::string(area && area != zone ? ", " : " ") + zone->area_name[0];
        return text;
    }
    return "camp";
}

// [guild60] FormFive / DisbandFive and the fives' dungeon runs: GuildFives.cpp

// ---------------------------------------------------------------------------------------------------------------------
// [guild50] gear through the global leader: every member's best reachable upgrade per slot (upgrade index x
// StatsWeightCalculator), the quest reward to take, crafts for members and bags, purchases, bank withdrawals
// ---------------------------------------------------------------------------------------------------------------------
bool GuildCoordinator::HumanControls(Player* p)
{
    // user 2026-11-07: a character counts as the human's only while he controls it (no AI, or a SelfBot just taken
    // over by input); handed to its bot (SelfBot on) it is a normal member, geared like the others
    return !GET_PLAYERBOT_AI(p) || SelfBotControl::IsSuspended(p);
}

bool GuildCoordinator::HumanCharacter(Player* p)
{
    // a bot's session has no socket (WorldSession: address "headless"); a SelfBot the human plays with keeps his
    return p && (HumanControls(p) || (p->GetSession() && p->GetSession()->GetRemoteAddress() != "headless"));
}

uint32 GuildCoordinator::NextSmelt(Player* p, std::vector<std::pair<uint32, uint32>>* reagents, uint32* product)
{
    if (!p || !p->HasSkill(SKILL_MINING))
        return 0;
    uint32 const skill = p->GetPureSkillValue(SKILL_MINING);
    if (skill >= p->GetPureMaxSkillValue(SKILL_MINING))
        return 0;
    uint32 best = 0, bestGray = 0;
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* a = sSkillLineAbilityStore.LookupEntry(i);
        if (!a || a->SkillLine != SKILL_MINING || skill >= a->TrivialSkillLineRankHigh || !p->HasSpell(a->Spell))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(a->Spell);
        if (!info || info->Reagent[0] <= 0)
            continue;
        if (!best || a->TrivialSkillLineRankHigh < bestGray)
        {
            best = a->Spell;
            bestGray = a->TrivialSkillLineRankHigh;
        }
    }
    if (SpellInfo const* info = best ? sSpellMgr->GetSpellInfo(best) : nullptr)
    {
        if (reagents)
        {
            reagents->clear();
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
                if (info->Reagent[r] > 0 && info->ReagentCount[r] > 0)
                    reagents->emplace_back(uint32(info->Reagent[r]), info->ReagentCount[r]);
        }
        if (product)
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                if (info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[e].ItemType)
                    *product = info->Effects[e].ItemType;
    }
    return best;
}

// [craft437] b469: the jeweler Colian (no Mining) was mailed Copper Ore for his Copper Bars by 20+ members - he
// cannot smelt it - while no miner smelted copper: NextSmelt smelts only while it gives skill (Smelt Copper grey at
// 75 for the 105 miners). A smelt of a bar the guild's non-smelting crafters lack is the miner's work for the guild
uint32 GuildCoordinator::GuildSmelt(Player* p, std::vector<std::pair<uint32, uint32>>* reagents,
                                    uint32* product) const
{
    if (!p || !p->HasSkill(SKILL_MINING) || guildBars_.empty())
        return 0;
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* a = sSkillLineAbilityStore.LookupEntry(i);
        if (!a || a->SkillLine != SKILL_MINING || !p->HasSpell(a->Spell))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(a->Spell);
        if (!info || info->Reagent[0] <= 0)
            continue;
        uint32 bar = 0;
        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            if (info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[e].ItemType)
                bar = info->Effects[e].ItemType;
        if (!bar || !guildBars_.count(bar))
            continue;
        std::vector<std::pair<uint32, uint32>> ores;
        bool enough = true;
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (info->Reagent[r] > 0 && info->ReagentCount[r] > 0)
            {
                ores.emplace_back(uint32(info->Reagent[r]), info->ReagentCount[r]);
                enough = enough && p->GetItemCount(uint32(info->Reagent[r]), false) >= info->ReagentCount[r];
            }
        if (!enough)
            continue;
        if (reagents)
            *reagents = std::move(ores);
        if (product)
            *product = bar;
        return a->Spell;
    }
    return 0;
}

void GuildCoordinator::Suggest(uint32 low, std::string const& key, std::string const& text)
{
    BotState& st = bots_[low];
    auto const old = st.suggested.find(key);
    if (old != st.suggested.end() && old->second == text)
        return;
    st.suggested[key] = text;
    LOG_INFO("playerbots", "[human] suggest {} key={}: {}", Who(low), key, text);
    Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
    if (p && p->IsInWorld() && p->GetSession())
        ChatHandler(p->GetSession()).SendSysMessage("[Guild] " + text);
}

void GuildCoordinator::UpdateGear()
{
    if (plan_.empty())
        return;
    std::vector<Player*> bots;
    Guild* guild = nullptr;
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || !p->IsInWorld())
            continue;
        if (!guild)
            guild = GuildBankRun::BotGuild(p);
        if (!HumanControls(p))
            bots.push_back(p);
    }
    // [craft401] user standing rule 2026-11-12 «the guild always checks the upgrade potential»: a member's upgrade
    // check (the slot groups by its stat weights over the crafts its members know or can learn, the vendors, the
    // quest rewards and the drops) runs when what it depends on changes - its level, its worn pieces (a gear change,
    // the grey / white piece it wears), its quest log, its bags (new loot), the guild's crafting skills and bank (a
    // skill-up that opens recipes, a new piece in the bank) - not on a timer. One member per call, in plan order,
    // the first whose key changed; none changed: no work. Before, every member in turn, every 5 s
    std::map<uint32, uint32> bank;
    GuildBankRun::CountBank(guild, bank);
    // [craft402] the guild's signals (docs/GUILD_UPGRADE_SIGNALS.md): its members (joined, left, online), each
    // crafter's skills, known spells (a recipe learned) and bags (materials reaching it), the bank, the dungeon order
    // (the open dungeon, the guild's clears: a new drop table)
    uint64 guildKey = 0;
    {
        uint64 skills = uint64(bots.size()) * 0x100000001B3ull;
        for (Player* c : bots)
        {
            bool crafter = false;
            for (uint32 line : CRAFT_LINES)
                if (c->HasSkill(line))
                {
                    skills = skills * 1000003ull + c->GetPureSkillValue(line);
                    crafter = crafter || (line != SKILL_COOKING && line != SKILL_FIRST_AID);
                }
            if (!crafter)
                continue;
            size_t const spells = c->GetSpellMap().size();
            skills = skills * 1000033ull + spells;
            // [craft403] its holding of the reagents of the gear recipes it knows (GuildGear::Recipe), not every loot:
            // the reagent set is kept per crafter while its known spells stay (world thread)
            static std::unordered_map<uint32, std::pair<size_t, std::vector<uint32>>> gearReagents;
            auto& known = gearReagents[c->GetGUID().GetCounter()];
            if (known.first != spells)
            {
                std::set<uint32> items;
                for (auto const& [spellId, spell] : c->GetSpellMap())
                    if (GuildGear::CraftRecipe const* recipe = spell && spell->State != PLAYERSPELL_REMOVED
                                                                   ? GuildGear::Recipe(spellId)
                                                                   : nullptr)
                        for (auto const& [item, n] : recipe->reagents)
                            items.insert(item);
                known = {spells, std::vector<uint32>(items.begin(), items.end())};
            }
            uint64 mats = 0;
            for (uint32 item : known.second)
                mats = mats * 31ull + uint64(item) * c->GetItemCount(item, false);
            skills ^= mats * 0x2545F4914F6CDD1Dull;
        }
        uint64 banked = 0;
        for (auto const& [item, n] : bank)
            banked = banked * 31ull + item * 7ull + n;
        uint64 order = 0;
        {
            CoordLock guard(lock_, __func__);
            order = std::hash<std::string>{}(orderOpenKey_) ^ (uint64(guildDone_.size()) << 32) ^ plan_.size();
        }
        guildKey = skills ^ (banked * 0x9E3779B97F4A7C15ull) ^ (order * 0xFF51AFD7ED558CCDull);
    }
    auto const memberKey = [&](Player* p)
    {
        // its level, and its zone (the vendors and the quest givers it reaches)
        uint64 key = guildKey ^ (uint64(p->GetLevel()) << 56) ^ (uint64(p->GetZoneId()) << 40);
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                key = key * 131ull + item->GetEntry() * 17ull + slot;
        for (uint16 q = 0; q < MAX_QUEST_LOG_SIZE; ++q)
            key = key * 37ull + p->GetQuestSlotQuestId(q);
        uint64 bags = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bags += item->GetEntry();
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* item = bag->GetItemByPos(uint8(j)))
                        bags += item->GetEntry();
        // [craft402] ... its stat weights (spec, talents spent, known spells), broken pieces, guild, letters,
        // reputation ranks (the reputation vendors) and the vendor picks its gold now pays for
        key = key * 131ull + p->GetActiveSpec() * 7ull + p->GetFreeTalentPoints() + (p->GetSpellMap().size() << 8);
        // [roster3] its talent tree: a respec re-spends every point (free points and spell count can stay the same),
        // and its stat weights change with the tree
        key = key * 131ull + AiFactory::GetPlayerSpecTab(p);
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                item && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) && !item->GetUInt32Value(ITEM_FIELD_DURABILITY))
                key = key * 13ull + slot + 1;
        key = key * 131ull + p->GetGuildId();
        key = key * 131ull + p->GetMails().size();
        for (auto const& [listId, state] : p->GetReputationMgr().GetStateList())
            if (FactionEntry const* faction = sFactionStore.LookupEntry(state.ID))
                key = key * 7ull + uint64(p->GetReputationMgr().GetRank(faction));
        uint32 affordable = 0;  // lock_ held by the caller
        if (auto const st = bots_.find(p->GetGUID().GetCounter()); st != bots_.end())
            for (auto const* list : {&st->second.gearNow, &st->second.gearPlan})
                for (GearPick const& g : *list)
                    affordable += g.kind == GuildGear::SRC_VENDOR && g.aux <= p->GetMoney() ? 1 : 0;
        key = key * 131ull + affordable;
        return key ^ (bags * 0xC2B2AE3D27D4EB4Full);
    };
    Player* member = nullptr;
    uint64 memberKeyNow = 0;
    {
        CoordLock guard(lock_, __func__);
        for (size_t n = 0; n < plan_.size() && !member; ++n)
        {
            gearCursor_ = (gearCursor_ + 1) % plan_.size();
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(plan_[gearCursor_].low));
            if (!p || !p->IsInWorld() || HumanControls(p))
                continue;
            uint64 const key = memberKey(p);
            if (bots_[plan_[gearCursor_].low].gearKey == key)
                continue;
            member = p;
            memberKeyNow = key;
        }
    }
    if (!member)
        return;

    std::vector<WantedItem> wanted;
    {
        CoordLock guard(lock_, __func__);
        wanted = wanted_;
    }
    std::vector<GearPick> now, plan, future;
    std::map<uint32, RewardPick> rewards;
    PlanGear(member, now, plan, future, rewards, bots, bank, wanted);

    std::ostringstream log;
    for (GearPick const& g : now)
        log << " [" << GuildGear::GroupName(g.group) << "] " << ItemName(g.itemId) << " " << GainText(g) << " — "
            << g.text << ";";
    LOG_INFO("playerbots", "[guild50] gear plan {} L{}: now {}, plan {}, rewards {};{}", member->GetName(),
             member->GetLevel(), now.size(), plan.size(), rewards.size(), log.str());

    CoordLock guard(lock_, __func__);
    BotState& st = bots_[member->GetGUID().GetCounter()];
    st.gearNow = std::move(now);
    st.gearPlan = std::move(plan);
    st.gearFuture = std::move(future);
    // [guild15] gear history: the leader's reward pick is the decision behind the equip it ends in
    uint32 const low = member->GetGUID().GetCounter();
    for (auto const& [questId, pick] : rewards)
    {
        auto const old = st.rewards.find(questId);
        if (!pick.itemId || (old != st.rewards.end() && old->second.itemId == pick.itemId))
            continue;
        GuildGear::HistoryNoteDecision(pick.forLow ? pick.forLow : low, pick.itemId, "leader",
                                       Who(low) + " → QUEST_REWARD: q" + std::to_string(questId) + ": " +
                                           ItemName(pick.itemId) + " — " + pick.why);
    }
    st.rewards = std::move(rewards);
    st.gearAt = Now();
    st.gearKey = memberKeyNow;
    // [craft401] its upgrades (now and planned), logged on a change, and the guild's line on a change
    std::set<std::pair<uint8, uint32>> upgrades;
    std::string upgradeText;
    for (auto const* list : {&st.gearNow, &st.gearPlan})
        for (GearPick const& g : *list)
            if (upgrades.emplace(g.group, g.itemId).second && upgradeText.size() < 400)
                upgradeText += std::string(upgradeText.empty() ? "" : "; ") + GuildGear::GroupName(g.group) + " " +
                               ItemName(g.itemId) + " " + GainText(g) + " (" + g.text + ")";
    if (upgrades != st.upgrades)
    {
        st.upgrades = upgrades;
        LOG_INFO("playerbots", "[craft401] {} L{} upgrades: {}", Who(low), member->GetLevel(),
                 upgradeText.empty() ? "none found" : upgradeText);
    }
    // [craft422] the orders the guild can carry out (a craft with its crafter, a vendor buy it pays now) apart from
    // the crafts no member knows yet (the crafters' skill-up targets): b462 "165 orders" counted both, 6 crafts ran
    uint32 members = 0, slots = 0, orders = 0, crafts = 0, buys = 0, unmade = 0;
    for (auto const& [botLow, state] : bots_)
    {
        members += state.upgrades.empty() ? 0 : 1;
        slots += uint32(state.upgrades.size());
        for (auto const* list : {&state.gearNow, &state.gearPlan})
            for (GearPick const& g : *list)
            {
                crafts += g.kind == GuildGear::SRC_CRAFT && g.crafterLow ? 1 : 0;
                unmade += g.kind == GuildGear::SRC_CRAFT && !g.crafterLow ? 1 : 0;
                buys += g.kind == GuildGear::SRC_VENDOR && list == &state.gearNow ? 1 : 0;
            }
    }
    orders = crafts + buys;
    // the grey / white pieces the members wear (the [health] line)
    wornLow_ = wornGrey_ = 0;
    for (Player* p : bots)
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                item && slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD &&
                item->GetTemplate()->Quality <= ITEM_QUALITY_NORMAL)
            {
                ++wornLow_;
                wornGrey_ += item->GetTemplate()->Quality == ITEM_QUALITY_POOR ? 1 : 0;
            }
    std::string const summary =
        Acore::StringFormat("guild upgrades: {} members, {} slots, {} orders ({} crafts with a crafter, {} vendor "
                            "buys), {} crafts no member knows yet [craft422]; worn grey {} / white {}",
                            members, slots, orders, crafts, buys, unmade, wornGrey_, wornLow_ - wornGrey_);
    if (summary != upgradeSummary_)
    {
        upgradeSummary_ = summary;
        LOG_INFO("playerbots", "[craft401] {}", summary);
    }
}

void GuildCoordinator::PlanGear(Player* m, std::vector<GearPick>& now, std::vector<GearPick>& plan,
                                std::vector<GearPick>& future, std::map<uint32, RewardPick>& rewards, std::vector<Player*> const& bots,
                                std::map<uint32, uint32> const& bank, std::vector<WantedItem> const& wanted)
{
    StatsWeightCalculator calc(m);
    calc.SetItemSetBonus(false);  // as ItemUsageValue
    calc.SetOverflowPenalty(false);
    uint32 const level = m->GetLevel();
    // now: every item it may wear at its level; ahead (plan only): up to the level of the quests it holds
    uint32 ahead = level;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(m->GetQuestSlotQuestId(slot)))
            ahead = std::max<uint32>(ahead, uint32(m->GetQuestLevel(quest)));
    uint32 const classMask = m->getClassMask();

    // its own money: what the module's budget leaves for gear (repair, ammo, spells, travel kept back) minus the next
    // profession ranks it still has to pay
    uint32 money = m->GetMoney();
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(m))
        money = ai->GetAiObjectContext()
                    ->GetValue<uint32>("free money for", std::to_string(uint32(NeedMoneyFor::gear)))
                    ->Get();
    uint32 const ranks = NewRpgProfession::NextRankCost(m);
    money = money > ranks ? money - ranks : 0;

    // [ui11] bestFuture: a plan pick whose level (the item's, else its source's: quest, mob) is above `ahead` - an
    // L9 bot's plan listed L74-78 pieces (the DB has RequiredLevel 0 on 834 WotLK pieces of item level 150+) and they
    // hid the reachable ones of the same slot group
    std::map<uint8, GearPick> bestNow, bestPlan, bestFuture;
    std::map<uint8, GearPick> bestCraft;  // [craft421]
    bool aheadItem = false;  // the member still lacks the level / reputation to wear it: a plan, not an order
    std::string lockText;
    auto const consider = [&](GearPick const& pick, bool isNow)
    {
        if (isNow && aheadItem)
            return;
        if (isNow && pick.needLevel > level)
            return;
        GearPick use = pick;
        if (aheadItem)
            use.text += " — " + lockText;
        auto& best = isNow ? bestNow : (pick.needLevel > ahead ? bestFuture : bestPlan);
        // [craft421] the best guild craft of each group is kept beside the group's best pick: b462 the plans named a
        // 0.0 % drop (Sparkleshell Mantle, Gnomeregan) over every shoulder craft, so no crafter was asked for one while
        // 17 members wore grey shoulders
        if (!isNow && use.kind == GuildGear::SRC_CRAFT && pick.needLevel <= ahead)
            if (auto const craft = bestCraft.find(use.group); craft == bestCraft.end() || use.gain > craft->second.gain)
                bestCraft[use.group] = use;
        auto const itr = best.find(use.group);
        if (itr == best.end() || use.gain > itr->second.gain + 1e-6f ||
            (std::fabs(use.gain - itr->second.gain) <= 1e-6f && KindRank(use.kind) < KindRank(itr->second.kind)))
            best[use.group] = use;
    };
    auto const chanceText = [](float chance)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), chance < 1.0f ? "%.1f%%" : "%.0f%%", chance);
        return std::string(buf);
    };
    auto const levelText = [](uint32 lo, uint32 hi)
    { return "L" + std::to_string(lo) + (hi > lo ? "-" + std::to_string(hi) : std::string()); };
    auto const creatureName = [](uint32 entry)
    {
        CreatureTemplate const* c = sObjectMgr->GetCreatureTemplate(entry);
        return c ? c->Name : std::string("?");
    };
    // where a token / emblem an extended cost asks for comes from (its first boss / mob / quest source)
    auto const tokenFrom = [&](uint32 extendedCost)
    {
        ItemExtendedCostEntry const* cost = sItemExtendedCostStore.LookupEntry(extendedCost);
        if (!cost)
            return std::string();
        for (uint32 r = 0; r < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++r)
        {
            std::vector<GuildGear::Source> const* list = cost->reqitem[r] ? GuildGear::SourcesOf(cost->reqitem[r])
                                                                          : nullptr;
            if (!list)
                continue;
            for (GuildGear::Source const& s : *list)
            {
                if (s.kind == GuildGear::SRC_DROP || s.kind == GuildGear::SRC_WORLD)
                    return ItemName(cost->reqitem[r]) + ": " + creatureName(s.id) + " (" +
                           GuildGear::MapName(s.mapId) + ")";
                if (s.kind == GuildGear::SRC_QUEST)
                    return ItemName(cost->reqitem[r]) + ": quest q" + std::to_string(s.id);
            }
        }
        return std::string();
    };

    auto const addSources = [&](ItemTemplate const* proto, GuildGear::Eval const& ev)
    {
        GearPick base;
        base.group = GuildGear::EquipGroup(proto->InventoryType);
        base.itemId = proto->ItemId;
        base.gain = ev.gain;
        base.score = ev.score;  // [ui11]
        base.current = ev.current;
        base.needLevel = proto->RequiredLevel;
        auto const inBank = bank.find(proto->ItemId);
        if (inBank != bank.end() && inBank->second)
        {
            GearPick p = base;
            p.kind = GuildGear::SRC_BANK;
            p.text = "in the guild bank";
            consider(p, true);
            consider(p, false);
        }
        std::vector<GuildGear::Source> const* list = GuildGear::SourcesOf(proto->ItemId);
        if (!list)
            return;
        bool vendorDone = false;
        for (GuildGear::Source const& src : *list)
        {
            GearPick p = base;
            p.kind = src.kind;
            p.srcId = src.id;
            p.aux = src.aux;
            p.mapId = src.mapId;
            p.chance = src.chance;
            // [ui11] the level the source opens at: a quest's minimum level (else its own), a boss's entrance, a
            // world mob's lowest level, a shared reference's lowest user
            uint32 srcLevel = 0;
            if (src.kind == GuildGear::SRC_QUEST)
                srcLevel = src.levelMax ? src.levelMax : src.level;
            else if (src.kind == GuildGear::SRC_DROP || src.kind == GuildGear::SRC_WORLD)
                srcLevel = src.level;
            else if (src.kind == GuildGear::SRC_REFDROP)
                if (GuildGear::RefInfo const* ref = GuildGear::Reference(src.id))
                    srcLevel = ref->minLevel;
            p.needLevel = std::max(p.needLevel, srcLevel);
            switch (src.kind)
            {
                case GuildGear::SRC_QUEST:
                {
                    Quest const* quest = sObjectMgr->GetQuestTemplate(src.id);
                    if (!quest || m->GetQuestRewardStatus(src.id))
                        break;
                    // the quest's own race / class limits: never this member's
                    if (!m->SatisfyQuestRace(quest, false) || !m->SatisfyQuestClass(quest, false))
                        break;
                    std::string const title = "q" + std::to_string(src.id) + " «" + quest->GetTitle() + "»";
                    if (m->FindQuestSlot(src.id) < MAX_QUEST_LOG_SIZE)
                    {
                        if (src.aux >= 100)
                            break;  // a fixed reward comes with the turn-in anyway
                        p.text = "reward " + title + " (in the log)";
                        consider(p, true);
                        consider(p, false);
                    }
                    else if (m->CanTakeQuest(quest, false))
                    {
                        p.text = "quest " + title + " L" + std::to_string(src.level);
                        consider(p, false);
                    }
                    else
                    {
                        // a chain step (prerequisites, exclusive group) or a level still ahead
                        p.text = "quest " + title + " L" + std::to_string(src.level) + " — " +
                                 (m->SatisfyQuestLevel(quest, false) ? std::string("after the chain")
                                                                     : "from L" + std::to_string(src.levelMax));
                        consider(p, false);
                    }
                    break;
                }
                case GuildGear::SRC_DROP:
                {
                    if (src.level > ahead)
                        break;  // the instance's entry level (dungeon_access_template) is still far ahead
                    MapEntry const* map = sMapStore.LookupEntry(src.mapId);
                    p.text = std::string(map && map->IsRaid() ? "raid " : "dungeon ") + GuildGear::MapName(src.mapId) +
                             (src.difficulty ? " (diff. " + std::to_string(src.difficulty) + ")" : std::string()) +
                             ": " + creatureName(src.id) + ", " + chanceText(src.chance) +
                             (src.level > level ? ", entry from L" + std::to_string(src.level) : std::string());
                    consider(p, false);
                    break;
                }
                case GuildGear::SRC_WORLD:
                    p.text = "mob " + creatureName(src.id) + " " + levelText(src.level, src.levelMax) + " (" +
                             GuildGear::MapName(src.mapId) + "), " + chanceText(src.chance);
                    consider(p, false);
                    break;
                case GuildGear::SRC_REFDROP:
                {
                    GuildGear::RefInfo const* ref = GuildGear::Reference(src.id);
                    if (!ref || ref->minLevel > ahead)
                        break;
                    p.text = "world drop " + levelText(ref->minLevel, ref->maxLevel) + " (" +
                             std::to_string(ref->users) + " sources, " + GuildGear::MapName(ref->mapId) + "), " +
                             chanceText(src.chance);
                    consider(p, false);
                    break;
                }
                case GuildGear::SRC_OBJECT:
                {
                    GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(src.id);
                    p.text = std::string("object ") + (go ? go->name : std::string("?")) + " (" +
                             GuildGear::MapName(src.mapId) + "), " + chanceText(src.chance);
                    consider(p, false);
                    break;
                }
                case GuildGear::SRC_CONTAINER:
                    p.text = "from " + ItemName(src.id) + ", " + chanceText(src.chance);
                    consider(p, false);
                    break;
                case GuildGear::SRC_OTHER:
                {
                    std::string what;
                    if (src.aux == GuildGear::OTHER_PICKPOCKET)
                        what = "pickpocket from " + creatureName(src.id);
                    else if (src.aux == GuildGear::OTHER_SKINNING)
                        what = "skin from " + creatureName(src.id);
                    else if (src.aux == GuildGear::OTHER_FISHING)
                        what = "fishing, zone " + std::to_string(src.id);
                    else
                        what = "mail " + std::to_string(src.id);
                    p.text = what + ", " + chanceText(src.chance);
                    consider(p, false);
                    break;
                }
                case GuildGear::SRC_CRAFT:
                {
                    GuildGear::CraftRecipe const* recipe = GuildGear::Recipe(src.id);
                    if (!recipe)
                        break;
                    bool const bop = proto->Bonding == BIND_WHEN_PICKED_UP;
                    Player* crafter = nullptr;
                    bool ready = false;
                    for (Player* c : bots)
                    {
                        if ((bop && c != m) || !c->HasSpell(recipe->spellId))
                            continue;
                        bool const has = HasReagents(c, *recipe);
                        if (!crafter || (has && !ready))
                        {
                            crafter = c;
                            ready = has;
                        }
                    }
                    if (!crafter)
                    {
                        p.text = std::string("craft (") + SkillShort(recipe->skillLine) + " " +
                                 std::to_string(recipe->reqSkill) + "): nobody in the guild has the recipe";
                        consider(p, false);
                        break;
                    }
                    p.crafterLow = crafter->GetGUID().GetCounter();
                    p.text = "craft " + Who(p.crafterLow) + " (" + SkillShort(recipe->skillLine) + ")" +
                             (ready ? "" : ", materials in the wanted list");
                    if (ready)
                        consider(p, true);
                    consider(p, false);
                    break;
                }
                case GuildGear::SRC_VENDOR:
                {
                    if (vendorDone)
                        break;
                    vendorDone = true;
                    // the nearest friendly spawn on its map
                    std::vector<GuildGear::VendorSpawn> const* spawns = GuildGear::VendorsOf(proto->ItemId);
                    GuildGear::VendorSpawn const* nearest = nullptr;
                    GuildGear::VendorSpawn const* elsewhere = nullptr;
                    float nearDist = 0.0f;
                    static std::vector<GuildGear::VendorSpawn> const none;
                    // [craft404] a vendor's item for the level it has (not the quests' ahead), at a vendor it can
                    // reach (another continent: the game's continent rule, Outland from 58, Northrend from 68) and
                    // for a currency it holds. b458: 57 [craft401] lines named emblem items for L15-23 members -
                    // "#49 Nelaeleas L22 two-hand Repurposed Lava Dredger ... vendor Enchanter Isian (Northrend), for
                    // 65x Emblem of Heroism"
                    if (proto->RequiredLevel > level)
                        break;
                    for (GuildGear::VendorSpawn const& s : spawns ? *spawns : none)
                    {
                        if (!FriendlyNpc(m, s.entry) || NewRpgProfession::BuyFailed(m, s.entry, proto->ItemId) ||
                            (s.extendedCost && !GuildGear::HoldsExtendedCost(m, s.extendedCost)) ||
                            (s.mapId != m->GetMapId() && (s.mapId == MAP_OUTLAND || s.mapId == MAP_NORTHREND) &&
                             !GuildGatherPlan::ContinentAllowed(m, s.mapId, WorldPosition(s.mapId, s.x, s.y, s.z))))
                            continue;
                        if (s.mapId != m->GetMapId())
                        {
                            if (!elsewhere)
                                elsewhere = &s;
                            continue;
                        }
                        float const d = m->GetExactDist(s.x, s.y, s.z);
                        if (!nearest || d < nearDist)
                        {
                            nearest = &s;
                            nearDist = d;
                        }
                    }
                    if (!nearest)
                    {
                        if (!elsewhere)
                            break;
                        // a vendor on another continent: a plan only
                        p.srcId = elsewhere->entry;
                        p.mapId = elsewhere->mapId;
                        p.text =
                            "vendor " + creatureName(elsewhere->entry) + " (" + GuildGear::MapName(elsewhere->mapId) +
                            ")" +
                            (elsewhere->extendedCost ? ", for " + GuildGear::ExtendedCostText(elsewhere->extendedCost)
                                                     : std::string());
                        consider(p, false);
                        break;
                    }
                    p.srcId = nearest->entry;
                    p.mapId = nearest->mapId;
                    p.pos = WorldPosition(nearest->mapId, nearest->x, nearest->y, nearest->z);
                    p.aux = uint32(proto->BuyPrice);
                    p.extendedCost = nearest->extendedCost;
                    std::string const missing =
                        nearest->extendedCost ? GuildGear::ExtendedCostMissing(m, nearest->extendedCost) : "";
                    bool const canPay = money >= uint32(proto->BuyPrice) && missing.empty();
                    std::string price;
                    if (nearest->extendedCost)
                        price = GuildGear::ExtendedCostText(nearest->extendedCost);
                    if (proto->BuyPrice || !nearest->extendedCost)
                        price += (price.empty() ? "" : " + ") + MoneyText(uint32(proto->BuyPrice));
                    p.text = "vendor " + creatureName(nearest->entry) + " " + std::to_string(uint32(nearDist)) +
                             " yd, " + price +
                             (money >= uint32(proto->BuyPrice) ? std::string()
                                                               : " (saving: free " + MoneyText(money) + ")") +
                             (missing.empty() ? std::string() : " (short: " + missing + ")");
                    if (!missing.empty())
                        if (std::string const from = tokenFrom(nearest->extendedCost); !from.empty())
                            p.text += "; " + from;
                    if (canPay)
                        consider(p, true);
                    consider(p, false);
                    break;
                }
                default:
                    break;
            }
        }
    };

    for (uint8 group = 0; group < GuildGear::GROUP_COUNT; ++group)
    {
        std::vector<uint32> const& items = GuildGear::ItemsOfGroup(group);
        for (uint32 item : items)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (proto->RequiredLevel > ahead)
                break;  // sorted by RequiredLevel
            if (!(uint32(proto->AllowableClass) & classMask) || !(uint32(proto->AllowableRace) & m->getRaceMask()))
                continue;
            GuildGear::Eval ev;
            if (!GuildGear::Evaluate(m, calc, proto, ev, true))
                continue;
            aheadItem = !ev.locked.empty();
            lockText = ev.locked;
            addSources(proto, ev);
        }
        aheadItem = false;
    }
    // equipment in the guild bank the index does not know (world drops deposited by members)
    for (auto const& [entry, count] : bank)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!count || !proto || GuildGear::SourcesOf(entry) || GuildGear::EquipGroup(proto->InventoryType) == 0xFF ||
            (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
            continue;
        GuildGear::Eval ev;
        if (GuildGear::Evaluate(m, calc, proto, ev))
            addSources(proto, ev);
    }
    // one main hand: the better of the one-hander and the two-hander
    uint8 const oneHand = GuildGear::EquipGroup(INVTYPE_WEAPON), twoHand = GuildGear::EquipGroup(INVTYPE_2HWEAPON);
    for (auto* best : {&bestNow, &bestPlan, &bestFuture, &bestCraft})
    {
        auto const a = best->find(oneHand), b = best->find(twoHand);
        if (a != best->end() && b != best->end())
            best->erase(a->second.gain >= b->second.gain ? b : a);
    }
    auto const byGain = [](GearPick const& a, GearPick const& b) { return a.gain > b.gain; };
    for (auto const& [group, pick] : bestNow)
        now.push_back(pick);
    for (auto const& [group, pick] : bestPlan)
        plan.push_back(pick);
    for (auto const& [group, pick] : bestCraft)  // [craft421] unless the group's pick or what it gets now is as good
    {
        auto const planned = bestPlan.find(group);
        auto const ready = bestNow.find(group);
        if ((planned == bestPlan.end() || planned->second.itemId != pick.itemId) &&
            (ready == bestNow.end() || ready->second.gain < pick.gain))
            plan.push_back(pick);
    }
    for (auto const& [group, pick] : bestFuture)  // [ui11] by level: the nearest future first
        future.push_back(pick);
    std::sort(now.begin(), now.end(), byGain);
    std::sort(plan.begin(), plan.end(), byGain);
    std::sort(future.begin(), future.end(),
              [](GearPick const& a, GearPick const& b)
              { return a.needLevel != b.needLevel ? a.needLevel < b.needLevel : a.gain > b.gain; });

    // the choice rewards of the quests in its log: itself first, else a BoE for the member it upgrades most, else a
    // wanted-list item, else the one that sells for the most
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = m->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest || quest->GetRewChoiceItemsCount() <= 1)
            continue;
        RewardPick pick;
        float best = -1.0f;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
            GuildGear::Eval ev;
            if (proto && GuildGear::Evaluate(m, calc, proto, ev) && ev.gain > best)
            {
                best = ev.gain;
                pick.index = i;
                pick.itemId = proto->ItemId;
                pick.why = std::string("for itself: ") +
                           GuildGear::GroupName(GuildGear::EquipGroup(proto->InventoryType)) + " " + GainText(ev);
            }
        }
        if (best < 0.0f)
        {
            for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
                if (!proto || proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM ||
                    GuildGear::EquipGroup(proto->InventoryType) == 0xFF)
                    continue;
                for (Player* other : bots)
                {
                    if (other == m)
                        continue;
                    StatsWeightCalculator otherCalc(other);
                    otherCalc.SetItemSetBonus(false);
                    otherCalc.SetOverflowPenalty(false);
                    GuildGear::Eval ev;
                    if (GuildGear::Evaluate(other, otherCalc, proto, ev) && ev.gain > best)
                    {
                        best = ev.gain;
                        pick.index = i;
                        pick.itemId = proto->ItemId;
                        pick.forLow = other->GetGUID().GetCounter();
                        pick.why = "BoE for " + Who(pick.forLow) + ": " +
                                   GuildGear::GroupName(GuildGear::EquipGroup(proto->InventoryType)) + " " +
                                   GainText(ev) + ", via the bank";
                    }
                }
            }
        }
        if (best < 0.0f)
            for (uint8 i = 0; i < quest->GetRewChoiceItemsCount() && best < 0.0f; ++i)
                for (WantedItem const& w : wanted)
                    if (w.itemId == quest->RewardChoiceItemId[i] && w.Short())
                    {
                        best = 0.0f;
                        pick.index = i;
                        pick.itemId = w.itemId;
                        pick.why = "in the wanted list (" + w.forWhom + ")";
                        break;
                    }
        // [ench154] an enchanter quester: a green it can disenchant itself (the reward is soulbound, only its own
        // Disenchant turns it into the guild's enchanting mats - user rule: greens nobody wears are disenchanted, not
        // sold); among them the one the core makes the most mats of (RequiredDisenchantSkill: the higher band)
        if (best < 0.0f && m->HasSkill(SKILL_ENCHANTING) && m->HasSpell(SPELL_DISENCHANT))
        {
            int32 bestReq = -1;
            for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
                    proto && proto->DisenchantID && proto->Quality >= ITEM_QUALITY_UNCOMMON &&
                    (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
                    proto->RequiredDisenchantSkill <= int32(m->GetPureSkillValue(SKILL_ENCHANTING)) &&
                    proto->RequiredDisenchantSkill > bestReq)
                {
                    best = 0.0f;
                    bestReq = proto->RequiredDisenchantSkill;
                    pick.index = i;
                    pick.itemId = proto->ItemId;
                    pick.why = "an upgrade for nobody: the enchanter disenchants it (skill " +
                               std::to_string(m->GetPureSkillValue(SKILL_ENCHANTING)) + ") [ench154]";
                }
        }
        if (best < 0.0f)
        {
            uint32 price = 0;
            for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]))
                    if (best < 0.0f || proto->SellPrice > price)
                    {
                        best = 0.0f;
                        price = proto->SellPrice;
                        pick.index = i;
                        pick.itemId = proto->ItemId;
                        pick.why = "an upgrade for nobody: worth more sold (" + MoneyText(price) + ")";
                    }
        }
        if (best >= 0.0f)
            rewards[questId] = pick;
    }
}

// lock_ held. The craft queue from the members' plans (upgrade crafts) and the bag plan; each job's reagents go on
// the wanted list.
void GuildCoordinator::UpdateCrafts(uint32 /*now*/)
{
    crafts_.clear();
    gearWanted_.clear();
    std::vector<Player*> bots;
    for (PlanRow const& row : plan_)
        if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low)))
            if (p->IsInWorld() && !HumanControls(p))
                bots.push_back(p);

    // items already on the way to a member: held for it in someone's bags, or about to be made / rewarded for it
    std::map<std::pair<uint32, uint32>, uint32> pending;  // (member, item) -> count
    for (auto const& [low, st] : bots_)
    {
        for (HeldItem const& h : st.held)
            if (h.forLow)
                ++pending[{h.forLow, h.itemId}];
        for (Expect const& e : st.expect)
            ++pending[{e.forLow, e.itemId}];
    }

    auto const addJob = [&](uint32 crafterLow, uint32 spellId, uint32 forLow, bool bag, std::string why,
                            double weight = 0.0)
    {
        GuildGear::CraftRecipe const* recipe = GuildGear::Recipe(spellId);
        if (!recipe)
            return;
        CraftJob job;
        job.crafterLow = crafterLow;
        job.spellId = spellId;
        job.itemId = recipe->itemId;
        job.forLow = forLow;
        job.bag = bag;
        job.why = std::move(why);
        job.weight = weight;
        crafts_.push_back(job);
    };

    // upgrade crafts: a member's best way to a slot (now or planned) that is a craft
    for (Player* m : bots)
    {
        uint32 const low = m->GetGUID().GetCounter();
        auto const st = bots_.find(low);
        if (st == bots_.end())
            continue;
        std::unordered_set<uint32> done;
        for (auto const* list : {&st->second.gearNow, &st->second.gearPlan})
            for (GearPick const& g : *list)
            {
                if (g.kind != GuildGear::SRC_CRAFT || !g.crafterLow || !done.insert(g.itemId).second ||
                    m->GetItemCount(g.itemId, true) || pending.count({low, g.itemId}))
                    continue;
                addJob(g.crafterLow, g.srcId, low, false,
                       std::string("upgrade ") + GuildGear::GroupName(g.group) + " " + GainText(g),
                       g.score > g.current ? double(g.score - g.current) : 0.0);
            }
    }

    // bags: the biggest bag a guild crafter knows now (lower tiers are skipped), for every bag slot smaller than it
    bagState_.clear();
    uint32 bagSpell = 0, bagSlots = 0;
    Player* tailor = nullptr;
    for (uint32 spell : GuildGear::BagRecipes())
    {
        GuildGear::CraftRecipe const* recipe = GuildGear::Recipe(spell);
        ItemTemplate const* proto = recipe ? sObjectMgr->GetItemTemplate(recipe->itemId) : nullptr;
        if (!proto || (bagSlots && proto->ContainerSlots < bagSlots))
            break;  // sorted by size: a smaller one only when no crafter knows any bigger
        for (Player* c : bots)
            if (c->HasSpell(spell) && (!tailor || (!HasReagents(tailor, *GuildGear::Recipe(bagSpell)) &&
                                                   HasReagents(c, *recipe))))
            {
                tailor = c;
                bagSpell = spell;
                bagSlots = proto->ContainerSlots;
            }
    }
    if (bagSpell)
    {
        uint32 const bagItem = GuildGear::Recipe(bagSpell)->itemId;
        uint32 total = 0;
        for (Player* m : bots)
        {
            uint32 const low = m->GetGUID().GetCounter();
            uint32 need = 0;
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                Bag* bag = m->GetBagByPos(b);
                // [inv376] a quiver / special bag slot needs no generic bag
                need += (!bag || (GenericBagProto(bag->GetTemplate()) && bag->GetBagSize() < bagSlots)) ? 1 : 0;
            }
            std::vector<Item*> spare;
            SpareBags(m, spare);
            for (Item* item : spare)
                if (item->GetTemplate()->ContainerSlots >= bagSlots && need)
                    --need;
            uint32 const coming = pending.count({low, bagItem}) ? pending[{low, bagItem}] : 0;
            need = need > coming ? need - coming : 0;
            for (uint32 i = 0; i < need; ++i)
                addJob(tailor->GetGUID().GetCounter(), bagSpell, low, true,
                       "bag " + std::to_string(bagSlots) + " slots instead of " + std::to_string(SmallestBag(m)));
            total += need;
        }
        bagState_ = "bags: best " + ItemName(bagItem) + " (" + std::to_string(bagSlots) + " slots) at " +
                    Who(tailor->GetGUID().GetCounter()) + ", needs " + std::to_string(total);
    }

    // ready: the crafter holds the reagents, taken job by job from its bags; every job's reagents are wanted
    std::map<uint32, std::map<uint32, uint32>> stock;  // crafter -> reagent -> count left
    for (CraftJob& job : crafts_)
    {
        GuildGear::CraftRecipe const* recipe = GuildGear::Recipe(job.spellId);
        Player* crafter = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(job.crafterLow));
        if (!recipe || !crafter)
            continue;
        auto& left = stock[job.crafterLow];
        job.ready = true;
        for (auto const& [item, n] : recipe->reagents)
        {
            if (!left.count(item))
                left[item] = crafter->GetItemCount(item, false);
            job.ready &= left[item] >= n;
        }
        if (job.ready)
            for (auto const& [item, n] : recipe->reagents)
                left[item] -= n;
        for (auto const& [item, n] : recipe->reagents)
        {
            // [guild70] the reason per product: "улучшение: <item>" / "сумка <item>", for whom
            GearNeed& g = gearWanted_[item][(job.bag ? "bag " : "upgrade: ") + ItemName(job.itemId)];
            g.count += n;
            g.forLows.insert(job.forLow);
            // [gather] the crafted item's price (the leader values unlocked upgrades by it), shared by reagent units
            uint32 units = 0;
            for (auto const& [reagent, count] : recipe->reagents)
                units += count;
            // [craft431] an upgrade's worth is its stat-weight gain at the guild's vendor rate ([craft428]): b463
            // Malachite for the Malachite Pendant was gathered at 3.2 c/unit (the pendant's vendor tag) - "gathering
            // 0.1 c/min" lost to questing everywhere, while the copper for the same rings ran 26-523 c/unit
            if (job.weight > 0.0 && copperPerWeight_ > 0.0 && units)
                g.value += job.weight * copperPerWeight_ * n / units;
            else if (ItemTemplate const* product = sObjectMgr->GetItemTemplate(job.itemId); product && units)
                g.value += double(product->BuyPrice) * n / units;
        }
    }
}

int32 GuildCoordinator::RewardChoice(Player* bot, Quest const* quest)
{
    if (!bot || !quest || !IsPlanned(bot->GetGUID().GetCounter()) || quest->GetRewChoiceItemsCount() <= 1)
        return -1;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return -1;
    auto const pick = itr->second.rewards.find(quest->GetQuestId());
    if (pick == itr->second.rewards.end() || pick->second.index >= quest->GetRewChoiceItemsCount() ||
        quest->RewardChoiceItemId[pick->second.index] != pick->second.itemId)
        return -1;
    if (pick->second.forLow && pick->second.forLow != bot->GetGUID().GetCounter())
        itr->second.expect.push_back(
            {pick->second.itemId, pick->second.forLow, "reward for " + Who(pick->second.forLow)});
    LOG_INFO("playerbots", "[guild50] {} takes reward {} of q{}: {}", bot->GetName(), ItemName(pick->second.itemId),
             quest->GetQuestId(), pick->second.why);
    return int32(pick->second.index);
}

// ---------------------------------------------------------------------------------------------------------------------
// loot flow
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
    thread_local uint32 coordLockDepth = 0;  // [lock23] lock_ held on this thread (its CoordLock depth)
}

CoordLock::CoordLock(std::mutex& m, char const* where)
{
    if (coordLockDepth++)
    {
        // one line per place (a log line, no decision)
        static std::mutex logLock;
        static std::set<std::string> logged;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> guard(logLock);
            fresh = logged.insert(where ? where : "?").second;
        }
        if (fresh)
            LOG_ERROR("playerbots", "[lock23] the global leader's lock taken again on its own thread in {}: runs "
                      "under the hold it has", where ? where : "?");
        return;
    }
    m.lock();
    m_ = &m;
}

CoordLock::~CoordLock()
{
    if (m_)
        m_->unlock();
    --coordLockDepth;
}

bool CoordLock::Held() { return coordLockDepth != 0; }

void GuildCoordinator::DrainDeferredLoot()
{
    std::vector<std::tuple<ObjectGuid, ObjectGuid, uint32>> loot;
    {
        std::lock_guard<std::mutex> guard(deferredLock_);
        loot.swap(deferredLoot_);
    }
    for (auto const& [playerGuid, itemGuid, count] : loot)
        if (Player* player = ObjectAccessor::FindPlayer(playerGuid))
            if (Item* item = player->GetItemByGuid(itemGuid))
                OnLoot(player, item, count);
}

void GuildCoordinator::OnLoot(Player* player, Item* item, uint32 count)
{
    if (!player || !item || !IsPlanned(player->GetGUID().GetCounter()))
        return;
    // [lock23] a loot hook fired by a core call made under lock_ on this thread (a cast, an item moved by a step):
    // taken at the next Update, out of the lock - its decisions walk the same state the holder is working on
    if (CoordLock::Held())
    {
        LOG_INFO("playerbots", "[lock23] {} loot of {} x{} deferred: the global leader's lock is held on this thread",
                 player->GetName(), item->GetEntry(), count);
        std::lock_guard<std::mutex> guard(deferredLock_);
        deferredLoot_.emplace_back(player->GetGUID(), item->GetGUID(), count);
        return;
    }
    FiveLoot(player, item, count);  // [guild60] a five's drop: its report, the fate of spare BoP
    sGuildGatherPlan.OnGathered(player, item->GetEntry(), count);  // [gather] the plan's measured yield
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->Quality == ITEM_QUALITY_POOR || proto->Class == ITEM_CLASS_QUEST ||
        proto->Bonding == BIND_QUEST_ITEM || item->IsSoulBound())
        return;
    bool const equipment = (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
                           proto->Bonding == BIND_WHEN_EQUIPPED;
    bool const material = proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_REAGENT ||
                          proto->Class == ITEM_CLASS_GEM;
    if (!equipment && !material)
        return;
    uint32 const low = player->GetGUID().GetCounter();
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    bool const gathering = ai && ai->rpgInfo.GetStatus() == RPG_DO_GATHER;
    CoordLock guard(lock_, __func__);
    BotState& st = bots_[low];
    // [tasks] the gathering pace: node units looted while gathering
    if (auto const src = gatherSource_.find(item->GetEntry()); gathering && src != gatherSource_.end())
    {
        uint32 const now = Now();
        auto& units = gatherUnits_[src->second.first];
        units.emplace_back(now, count);
        while (!units.empty() && now - units.front().first > HOUR)
            units.pop_front();
    }
    // held until the global leader decides: no auto-equip, no vendor sale meanwhile
    bool known = false;
    for (HeldItem const& h : st.held)
        known |= h.itemGuidLow == item->GetGUID().GetCounter();
    if (!known)
        st.held.push_back({item->GetGUID().GetCounter(), item->GetEntry(), count, 0, "pending"});
    lootQueue_.push_back({low, item->GetGUID().GetCounter(), item->GetEntry(), count});
}

void GuildCoordinator::ProcessLoot()
{
    std::vector<LootReport> reports;
    {
        CoordLock guard(lock_, __func__);
        reports.swap(lootQueue_);
    }
    for (LootReport const& report : reports)
    {
        Player* looter = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(report.playerLow));
        Item* item = looter ? looter->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(report.itemGuidLow)) : nullptr;
        if (!item)
            continue;
        DecideLoot(looter, item, report);
    }
}

void GuildCoordinator::DecideLoot(Player* looter, Item* item, LootReport const& report)
{
    ItemTemplate const* proto = item->GetTemplate();
    uint32 const looterLow = looter->GetGUID().GetCounter();
    std::string const what = ItemName(report.itemId) + (report.count > 1 ? " ×" + std::to_string(report.count) : "") +
                             " (at " + Who(looterLow) + ")";
    bool const equipment = proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON;

    // 1. BoE: whom of the members it upgrades most (ItemUsage of the bot; score gain vs its slot)
    uint32 bestLow = 0;
    float bestGain = 0.0f;
    std::string bestUsage;
    if (equipment)
    {
        for (PlanRow const& row : plan_)
        {
            Player* m = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
            if (!m || !m->IsInWorld() || m->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (!GET_PLAYERBOT_AI(m))
                continue;  // a human picks his own gear
            // [loot12] the guild's one gear judge (the plan's, the five roll's and the equip's): no copy owned
            GuildGear::Eval ev;
            if (!GuildGear::EvaluateDrop(m, proto, item->GetItemRandomPropertyId(), ev))
                continue;
            if (ev.gain > bestGain)
            {
                bestGain = ev.gain;
                bestLow = row.low;
                bestUsage = ev.worn ? "replacement" : "new slot";
            }
        }
    }

    // [b367] the looter's own use of the item, read before lock_: ItemUsageValue asks IsHeld, which takes lock_ too
    // (std::mutex is not recursive - the b366 crash: resource_deadlock_would_occur from IsHeld under DecideLoot)
    ItemUsage looterUsage = ITEM_USAGE_NONE;
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(looter))
        looterUsage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", std::to_string(report.itemId))->Get();

    CoordLock guard(lock_, __func__);
    BotState& st = bots_[looterLow];
    auto held = std::find_if(st.held.begin(), st.held.end(),
                             [&](HeldItem const& h) { return h.itemGuidLow == report.itemGuidLow; });
    auto const release = [&]()
    {
        if (held != st.held.end())
            st.held.erase(held);
    };
    std::string const gainText = "+" + std::to_string(int32(bestGain * 100.0f + 0.5f)) + "% to the slot";

    if (bestLow == looterLow)
    {
        release();
        Decision("BoE " + what + " → equips it itself: " + gainText + " (" + bestUsage + ")");
        return;
    }
    if (bestLow)
    {
        if (held != st.held.end())
        {
            held->forLow = bestLow;
            held->purpose = "upgrade " + Who(bestLow);
        }
        Decision("BoE " + what + " → " + Who(bestLow) + ": " + gainText + " (" + bestUsage + "), via the guild bank");
        return;
    }

    // 2. wanted-list material: to the guild bank (held until it exists)
    for (WantedItem const& w : wanted_)
        if (w.itemId == report.itemId && (w.need || w.optional))
        {
            if (held != st.held.end())
                held->purpose = "wanted";
            Decision(what +
                     (GuildBankRun::DepositState(looter, report.itemId, report.count).empty()
                          ? " → bank"
                          : " → holds until the guild bank") +
                     ": in the wanted list" + (w.optional ? " (optional)" : "") + " (" + std::to_string(w.have) + "/" +
                     std::to_string(w.need) + ": " + OrdersText(w.forWhom, 90) + ")");
            return;
        }

    // [craft393] a reagent of a recipe the looter knows is its own to craft with, kept: b454 Dekorhan's milling loot
    // ("[insc142] ... takes the milling loot of Silverleaf: Alabaster Pigment") came here as loot, was in no wanted
    // list (the walk wants the herb it is milled from) and was marked "sell: no upgrade and not in the wanted list" -
    // "SELL 4 items: Alabaster Pigment x4", his Ivory Ink never made, Inscription stuck at 25
    for (auto const& [line, recipes] : recipesBySkill_)
    {
        if (!looter->HasSkill(line))
            continue;
        for (Recipe const& r : recipes)
            if (looter->HasSpell(r.spellId) &&
                std::any_of(r.reagents.begin(), r.reagents.end(),
                            [&](auto const& reagent) { return reagent.first == report.itemId; }))
            {
                release();
                Decision(what + " → keeps: a reagent of its own " + NewRpgCraft::RecipeName(r.spellId) +
                         " [craft393]");
                return;
            }
    }
    // [craft398] ... and an ingredient a guild cook cooks with stays in the guild (the transfer brings it to the
    // cook, [craft388]): b456 Bear Meat, Big Bear Meat, Crocolisk Meat, Crunchy Spider Leg on the members' SELL
    // tasks while "Bami cooks Coyote Steak ... lack 517"
    if (CookNeeds(report.itemId))
    {
        release();
        Decision(what + " → keeps: a guild cook's ingredient [craft398]");
        return;
    }
    // the looter uses it itself (its profession, a spell, a quest): no decision needed
    if (GET_PLAYERBOT_AI(looter))
    {
        ItemUsage const usage = looterUsage;
        if (usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_USE || usage == ITEM_USAGE_QUEST ||
            usage == ITEM_USAGE_KEEP || usage == ITEM_USAGE_AMMO || usage == ITEM_USAGE_GUILD_TASK ||
            usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
        {
            release();
            return;
        }
    }
    else
    {
        release();
        return;  // a human decides himself
    }

    // [quest18] an item a quest in its log still needs for its hand-in (incomplete or complete, not rewarded) is
    // never sold: the loot past the needed count stacks with the quest's own, and the sale of that stack took the
    // quest back to incomplete. b359 Ryarii L8 q4161: complete three times, 7 Small Spider Legs sold with the 2
    // looted after them each time ("sell: no upgrade and not in the wanted list") - 1071 s, no hand-in
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 const questId = looter->GetQuestSlotQuestId(slot))
        {
            QuestStatus const status = looter->GetQuestStatus(questId);
            Quest const* quest = (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
                                     ? sObjectMgr->GetQuestTemplate(questId)
                                     : nullptr;
            for (uint8 i = 0; quest && i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == report.itemId && quest->RequiredItemCount[i])
                {
                    release();
                    Decision(what + " → keeps: q" + std::to_string(questId) + " needs it for its hand-in");
                    return;
                }
        }

    // [tasks] a member's gear plan holds it (a later level, an offline member): kept for that member, never
    // disenchanted or sold
    if (uint32 const planned = equipment ? PlannedFor(report.itemId) : 0)

    {
        if (planned == looterLow)
        {
            if (held != st.held.end())
            {
                held->forLow = looterLow;  // kept in its bags (no bank run, no sale)
                held->purpose = "its own plan";
            }
            Decision("BoE " + what + " → holds for itself: in its gear plan");
            return;
        }
        if (held != st.held.end())
        {
            held->forLow = planned;
            held->purpose = "upgrade by plan of " + Who(planned);
        }
        Decision("BoE " + what + " → " + Who(planned) + ": in its gear plan, holds until the transfer");
        return;
    }

    // 3. nobody's: an enchanter disenchants it, else it is sold
    if (equipment && proto->DisenchantID && proto->Quality >= ITEM_QUALITY_UNCOMMON)
    {
        uint32 const req = proto->RequiredDisenchantSkill;
        // [craft409] user rule 2026-11-12: disenchanted only for a needed result, else sold
        std::map<uint32, double> demand = DisenchantDemand();
        std::string worth;
        if (looter->HasSkill(SKILL_ENCHANTING) && looter->GetPureSkillValue(SKILL_ENCHANTING) >= req &&
            looter->HasSpell(SPELL_DISENCHANT))
        {
            release();
            if (!DisenchantWorth(proto, looter->GetPureSkillValue(SKILL_ENCHANTING), demand, worth))
            {
                st.sell.push_back(report.itemGuidLow);
                Decision(what + " → " + worth + " [craft409]");
                return;
            }
            st.disenchant.push_back(report.itemGuidLow);
            Decision(what + " → disenchants itself (enchanter, skill " +
                     std::to_string(looter->GetPureSkillValue(SKILL_ENCHANTING)) + "): " + worth + " [craft409]");
            return;
        }
        // [ench153] to the guild's enchanter by the transfer flow (trade / mail / bank, GuildTransfer: whichever costs
        // least), a guild bank or not - b426-b428: 5 greens sold "enchanter #6 Emdyl exists, no bank" while all five
        // enchanters stood at skill 1-2. The enchanter the skill-up is worth most to: the lowest skill (Disenchant's
        // skill-up chance falls from its yellow rank to its gray one, SkillLineAbility 13262: 20 / 60)
        // [ench165] by the Disenchant's skill-up chance, then the shortest list, then the lowest skill (as [ench161])
        uint32 enchanter = 0;
        uint32 enchanterSkill = 0;
        int32 enchanterChance = 0;
        size_t enchanterQueued = 0;
        for (PlanRow const& row : plan_)
            if (Player* m = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low)))
                if (GET_PLAYERBOT_AI(m) && m->HasSkill(SKILL_ENCHANTING) && m->HasSpell(SPELL_DISENCHANT) &&
                    m->GetPureSkillValue(SKILL_ENCHANTING) >= req)
                {
                    uint32 const skill = m->GetPureSkillValue(SKILL_ENCHANTING);
                    int32 const chance = DisenchantSkillUpChance(skill);
                    // [craft409] its whole queue (letters, held items, bank claims); one more needs a bag slot
                    uint32 const inbound = EnchanterInbound(row.low, m);
                    if (FreeBagSlots(m) <= inbound)
                        continue;
                    size_t const queued = bots_[row.low].disenchant.size() + inbound;
                    if (enchanter &&
                        (chance < enchanterChance ||
                         (chance == enchanterChance &&
                          (queued > enchanterQueued || (queued == enchanterQueued && skill >= enchanterSkill)))))
                        continue;
                    enchanter = row.low;
                    enchanterSkill = skill;
                    enchanterChance = chance;
                    enchanterQueued = queued;
                }
        if (enchanter && DisenchantWorth(proto, enchanterSkill, demand, worth))
        {
            if (held == st.held.end())
            {
                st.held.push_back({report.itemGuidLow, report.itemId, report.count, 0, "pending"});
                held = std::prev(st.held.end());
            }
            held->forLow = enchanter;
            held->purpose = "disenchant " + Who(enchanter);
            Decision(what + " → to the enchanter " + Who(enchanter) + " (skill " + std::to_string(enchanterSkill) +
                     ") " + worth + " [ench153][craft409]");
            return;
        }
        release();
        st.sell.push_back(report.itemGuidLow);
        Decision(what + (enchanter ? " → " + worth + " [craft409]"
                                   : " → sell: an upgrade for nobody, no enchanter of skill " + std::to_string(req) +
                                         " with bag room [ench153][craft409]"));
        return;
    }
    release();
    st.sell.push_back(report.itemGuidLow);
    Decision(what + " → sell: no upgrade and not in the wanted list");
}

// ---------------------------------------------------------------------------------------------------------------------
// map thread: New RPG runs the order
// ---------------------------------------------------------------------------------------------------------------------
bool GuildCoordinator::DisenchantStep(Player* bot, PlayerbotAI* botAI, BotState& st, GuildRunStep& step)
{
    // [ench151] Disenchant has a cast time and breaks on movement: a walking or fighting bot keeps its list
    // (CastSpell refused the cast while it moved and the item was dropped from the list: b428 Emdyl's
    // Warrior's Tunic "dropped DISENCHANT ... ended before its start", never disenchanted)
    // (a sitting bot eats or drinks: it disenchants once it stands up)
    if (!st.disenchant.empty() && (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false) ||
                                   bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->IsFlying() ||
                                   !bot->IsStandState()))
        return false;
    if (!st.disenchant.empty() && (bot->isMoving() || bot->IsMounted()))
    {
        bot->StopMoving();
        if (bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        step.kind = GuildRunStep::BUSY;
        step.reason = "stops to disenchant";
        LOG_INFO("playerbots", "[ench151] {} L{} stops to disenchant {} items", bot->GetName(),
                 bot->GetLevel(), st.disenchant.size());
        return true;
    }
    while (!st.disenchant.empty())
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(st.disenchant.back()));
        st.disenchant.pop_back();
        // [tasks] only what no member needs: a gear plan that took it since the decision keeps it
        if (uint32 const planned = item ? PlannedFor(item->GetEntry()) : 0)
        {
            st.held.push_back({item->GetGUID().GetCounter(), item->GetEntry(), item->GetCount(), planned,
                               "upgrade by plan of " + Who(planned)});
            LOG_INFO("playerbots", "[tasks] {} keeps {} for {}: in its gear plan, not disenchanted",
                     bot->GetName(), item->GetTemplate()->Name1, Who(planned));
            continue;
        }
        if (!item)
            continue;
        uint32 const skill = bot->GetPureSkillValue(SKILL_ENCHANTING);
        if (botAI->CastSpell(SPELL_DISENCHANT, bot, item))
        {
            LOG_INFO("playerbots", "[ench151] {} L{} disenchants {} (skill {}/{})", bot->GetName(),
                     bot->GetLevel(), item->GetTemplate()->Name1, skill,
                     bot->GetPureMaxSkillValue(SKILL_ENCHANTING));
            step.kind = GuildRunStep::BUSY;
            step.reason = "disenchant " + item->GetTemplate()->Name1;
            return true;
        }
        LOG_INFO("playerbots", "[ench151] {} L{} could not disenchant {} (skill {}, needs {}): dropped",
                 bot->GetName(), bot->GetLevel(), item->GetTemplate()->Name1, skill,
                 item->GetTemplate()->RequiredDisenchantSkill);
    }
    return false;
}

// [craft410] user decision 2026-11-12: a Disenchant needs no travel and takes a few seconds, so an enchanter with a
// queue casts it between fights, as it loots. b461 #6 Emdyl's DISENCHANT (6 items) waited 6,400 s in one DO_QUEST:
// the guild's steps run only when the status changes
bool GuildCoordinator::DisenchantNow(Player* bot, PlayerbotAI* botAI)
{
    if (!bot || !botAI || !IsPlanned(bot->GetGUID().GetCounter()) || bot->IsInCombat() ||
        !bot->getAttackers().empty() || !bot->GetMap() || bot->GetMap()->Instanceable() ||
        !bot->HasSpell(SPELL_DISENCHANT))  // only the enchanters reach the global leader's lock
        return false;  // a five's run keeps its pace: its enchanter casts at the next town or field stop
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end() || itr->second.disenchant.empty())
        return false;
    uint32 const queued = uint32(itr->second.disenchant.size());
    GuildRunStep step;
    if (!DisenchantStep(bot, botAI, itr->second, step))
        return false;
    LOG_INFO("playerbots", "[craft410] {} L{} {} between fights ({} queued)", bot->GetName(), bot->GetLevel(),
             step.reason, queued);
    return true;
}

GuildRunStep GuildCoordinator::NextStep(Player* bot, PlayerbotAI* botAI)
{
    GuildRunStep step;
    if (!bot || !botAI || !IsPlanned(bot->GetGUID().GetCounter()))
        return step;
    // [craft396] the loot window of its last Disenchant first (before lock_: the loot hook takes it): b456 the
    // enchanters' disenchants ([ench151] 9x) opened their loot and nothing ever took it - no Strange Dust or Lesser
    // Magic Essence anywhere in the guild, every enchanter's walk "Strange Dust x1: neither held nor gathered",
    // [craft386] 0 plans, and the disenchanted green stayed in the bags ("spare Sturdy Quarterstaff (at #22 Idunk)
    // -> its own Disenchant" again)
    if (!bot->IsNonMeleeSpellCast(false) && NewRpgCraft::TakeItemLoot(bot))
    {
        step.kind = GuildRunStep::BUSY;
        step.reason = "takes its disenchant loot";
        return step;
    }
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return step;
    BotState& st = itr->second;
    uint32 const low = bot->GetGUID().GetCounter();
    // [gather] a fishing bot whose fish order is gone: the pole off, the bobber strategy off
    if (NewRpgFishing::IsFishing(bot) &&
        std::none_of(st.orders.begin(), st.orders.end(),
                     [](GuildOrder const& o)
                     {
                         GatherPlace const* place = o.type == GuildOrderType::GATHER && o.executable
                                                        ? sGuildGatherPlan.Place(o.placeId)
                                                        : nullptr;
                         return place && place->kind == GatherKind::FISH;
                     }))
        NewRpgFishing::Stop(botAI, bot);
    // [tasks] the first step of a task: the start log, the town metrics (wait since creation, tasks per visit)
    st.stepTask.clear();  // [stat1] set again by the task this step runs
    auto const started = [&](GuildOrder const& o)
    {
        st.stepTask = std::string(OrderName(o.type)) +
                      (o.type == GuildOrderType::PROFESSION ? std::string("/") + ProfessionKindName(o.profession)
                                                            : "") +
                      (o.townName.empty() ? "" : " " + o.townName) + ": " + OrdersText(o.details, 60);
        if (!st.ranTasks.insert(TaskKey(o)).second)
            return;
        uint32 const now = Now();
        LOG_INFO("playerbots", "[tasks] {} start {} ({}, {}): {}", bot->GetName(), OrderName(o.type),
                 PriorityRu(o.priority), PlaceRu(o.place), o.details);
        if (o.place == GuildTaskPlace::FIELD || !o.townArea)
            return;
        townWaitSum_ += now > o.since ? now - o.since : 0;
        ++townStarted_;
        if (st.visitTown != o.townArea)
        {
            st.visitTown = o.townArea;
            st.visitTasks = 0;
            ++townVisits_;
        }
        ++st.visitTasks;
    };
    for (GuildOrder& order : st.orders)
    {
        if (!order.executable)
            continue;
        // [tasks] a convenient town task never takes the bot off its quests: it runs once the bot is in that town
        order.waitsTown = !TownReady(bot, order);
        if (order.waitsTown)
            continue;
        switch (order.type)
        {
            case GuildOrderType::DISENCHANT:
            {
                if (DisenchantStep(bot, botAI, st, step))
                {
                    if (step.reason.rfind("disenchant ", 0) == 0)
                        started(order);
                    return step;
                }
                continue;
            }
            case GuildOrderType::BANK_DEPOSIT:
            case GuildOrderType::BANK_WITHDRAW:
            {
                // [bank407] the order lives until the next plan: once its items are in (or taken out) there is
                // nothing left for the vault, so no empty visit (b453 Brelir: 4 visits "put 0 from 0 items")
                uint32 const selfLow = bot->GetGUID().GetCounter();
                bool const work =
                    order.type == GuildOrderType::BANK_DEPOSIT
                        ? std::any_of(st.held.begin(), st.held.end(),
                                      [&](HeldItem const& h)
                                      {
                                          return h.purpose != "pending" && h.forLow != selfLow &&
                                                 !TransferHolds(h.itemGuidLow);
                                      })
                        : !withdraw_[selfLow].empty();
                if (!work)
                {
                    LOG_INFO("playerbots", "[bank407] {}: {} has nothing left to move, no vault visit",
                             bot->GetName(),
                             order.type == GuildOrderType::BANK_DEPOSIT ? "BANK_DEPOSIT" : "BANK_WITHDRAW");
                    continue;
                }
                // [bank408] the bank changed since the plan (filled up, rights gone): the order is dropped until the
                // next plan instead of a vault visit that moves nothing
                std::string why;
                if (order.type == GuildOrderType::BANK_DEPOSIT)
                {
                    std::vector<HeldItem> items;
                    for (HeldItem const& h : st.held)
                        if (h.purpose != "pending" && h.forLow != selfLow && !TransferHolds(h.itemGuidLow))
                            items.push_back(h);
                    why = DepositStateOf(bot, items);  // [bank411] room for one of them at least
                }
                else
                    why = GuildBankRun::BankState(bot, false);
                if (!why.empty())
                {
                    LOG_INFO("playerbots", "[bank408] {}: {} dropped ({}), quests on", bot->GetName(),
                             OrderName(order.type), why);
                    order.executable = false;
                    order.status = why;
                    continue;
                }
                WorldPosition pos;
                float dist = 0;
                // [guild14] a solo bot walks to the vault only on a safe walk (b303 Laaral L10 died twice on its bank
                // run across Bloodmyst): else the order waits
                if (!GuildBankRun::NearestVault(bot, pos, dist) ||
                    NewRpgBaseAction::TripRefused(bot, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                                                  "guild vault"))
                    continue;
                st.run = true;
                st.runType = order.type;
                st.runPos = pos;
                st.runApproached = false;
                step.kind = GuildRunStep::WALK;
                step.pos = pos;
                step.reason = std::string(OrderName(order.type)) + " " + order.details + ", vault " +
                              std::to_string(uint32(dist)) + " yd";
                started(order);
                return step;
            }
            case GuildOrderType::DUNGEON:  // [guild60] to the five's entrance
                if (FiveStep(bot, st, step))
                {
                    started(order);
                    return step;
                }
                continue;
            case GuildOrderType::FOUND_GUILD:  // [guild80] to the Guild Master / the guild vault
            case GuildOrderType::SIGN_CHARTER:
            case GuildOrderType::BANK_TAB:
                if (FoundStep(bot, st, order, step))
                {
                    started(order);
                    return step;
                }
                continue;
            case GuildOrderType::TRADE_TO:  // [guild81] to the other member / to a mailbox
            case GuildOrderType::MAIL_TO:
            case GuildOrderType::DONATE_GOLD:
                if (TransferStep(bot, st, order, step))
                {
                    started(order);
                    return step;
                }
                continue;
            case GuildOrderType::PROFESSION:  // [tasks] the leader's call: gathering its own reagents
                // [econ1] a smelt at the town forge: the walk to it in town, then the smelt cast there (the core's
                // spell, its ore, the forge in reach: SpellInfo RequiresSpellFocus), one bar per cast while it
                // still gives skill (NextSmelt); the craft executor's look is not needed for it
                if (order.profession == GuildProfessionKind::CRAFT && order.skillLine == SKILL_MINING &&
                    order.pos != WorldPosition())
                {
                    if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false))
                        continue;
                    std::vector<std::pair<uint32, uint32>> ores;
                    uint32 smelt = NextSmelt(bot, &ores);
                    bool enough = smelt && !ores.empty();
                    for (auto const& [ore, n] : ores)
                        enough = enough && bot->GetItemCount(ore, false) >= n;
                    if (!enough)  // [craft437] the bars the guild's crafters lack
                        smelt = GuildSmelt(bot, &ores);
                    enough = smelt && !ores.empty();
                    for (auto const& [ore, n] : ores)
                        enough = enough && bot->GetItemCount(ore, false) >= n;
                    if (!enough)
                        continue;
                    if (NewRpgCraft::FocusNear(bot, NewRpgCraft::RecipeFocus(smelt)))
                    {
                        if (bot->isMoving())
                            bot->StopMoving();
                        uint32 const before = bot->GetPureSkillValue(SKILL_MINING);
                        if (botAI->CastSpell(smelt, bot))
                        {
                            LOG_INFO("playerbots", "[econ1] smelt {} L{} casts {} at the forge (mining {}/{})",
                                     bot->GetName(), bot->GetLevel(), NewRpgCraft::RecipeName(smelt), before,
                                     bot->GetPureMaxSkillValue(SKILL_MINING));
                            ++st.casts;  // [econ2] progress for the stuck watch, a skill-up or not
                            step.kind = GuildRunStep::BUSY;
                            step.reason = "smelt " + NewRpgCraft::RecipeName(smelt);
                            started(order);
                            return step;
                        }
                        continue;
                    }
                    float const toForge = bot->GetExactDist(order.pos.GetPositionX(), order.pos.GetPositionY(),
                                                            order.pos.GetPositionZ());
                    if (toForge <= 2.0f)
                        continue;  // at the forge and still not in its reach: nothing to walk to
                    if (toForge <= 12.0f)
                    {
                        // the camp walk ends within its arrival distance: the last steps up to the forge
                        if (!bot->isMoving())
                            bot->GetMotionMaster()->MovePoint(0, order.pos.GetPositionX(), order.pos.GetPositionY(),
                                                              order.pos.GetPositionZ());
                        step.kind = GuildRunStep::BUSY;
                        step.reason = "steps up to the forge";
                        return step;
                    }
                    step.kind = GuildRunStep::WALK;
                    step.pos = order.pos;
                    step.reason = "PROFESSION smelt at the forge, " +
                                  std::to_string(uint32(bot->GetExactDist(order.pos.GetPositionX(),
                                                                          order.pos.GetPositionY(),
                                                                          order.pos.GetPositionZ()))) +
                                  " yd";
                    started(order);
                    return step;
                }
                if (order.profession != GuildProfessionKind::GATHER)
                    continue;
                [[fallthrough]];
            case GuildOrderType::GATHER:
                // [gather] the plan's place: fish there, walk to another zone's place, grind a skinning place's
                // mobs; nodes and chests are the DO_GATHER run below
                if (GatherPlace const* place =
                        order.type == GuildOrderType::GATHER ? sGuildGatherPlan.Place(order.placeId) : nullptr)
                {
                    if (place->kind == GatherKind::FISH)
                    {
                        WorldPosition to;
                        std::string why;
                        NewRpgFishing::Step const fish = NewRpgFishing::Tick(botAI, bot, place->spot, to, why);
                        // [gather] the walk to the water passes the solo judge as every gather walk
                        if (fish == NewRpgFishing::Step::WALK && to.GetMapId() == bot->GetMapId() &&
                            NewRpgBaseAction::TripRefused(bot, to.GetPositionX(), to.GetPositionY(), to.GetPositionZ(),
                                                          "fishing water"))
                        {
                            sGuildGatherPlan.Refuse(bot, order.placeId, "the solo judge refused the walk to the water");
                            order.executable = false;
                            order.status = "spot left to the leader: the way is dangerous";
                            continue;
                        }
                        if (fish == NewRpgFishing::Step::WALK || fish == NewRpgFishing::Step::BUSY)
                        {
                            step.kind = fish == NewRpgFishing::Step::WALK ? GuildRunStep::WALK : GuildRunStep::BUSY;
                            step.pos = to;
                            step.reason = "GATHER fish " + order.details + (why.empty() ? "" : ": " + why);
                            started(order);
                            return step;
                        }
                        if (fish == NewRpgFishing::Step::DONE)
                        {
                            NewRpgFishing::Stop(botAI, bot);
                            order.executable = false;
                            order.status = why;
                            LOG_INFO("playerbots", "[gather] {} gives the fishing task back: {}", bot->GetName(), why);
                            sGuildGatherPlan.Refuse(bot, order.placeId, why);  // else the next update sends it again
                        }
                        continue;
                    }
                    // [questrate] b338: a chest place in the bot's own zone ran as DO_GATHER, which works herb / ore
                    // nodes only (chests: 0 of 6402 Linen Cloth, Ryarii's Fel Rock chest): it walks to the chest
                    // place's spot as to another zone's place, and opens the chests in sight there (below)
                    // [econ1] b350 Aevmae: a silent GO_CAMP -> IDLE loop (52 rounds) under an URGENT GATHER - a
                    // place whose spot lies across a zone border was walked to again on every look although the bot
                    // stood at it (each walk a step nearer, so "no nearer" never held). At its spot (the GO_CAMP
                    // arrival distance) the bot has arrived: it works the place here
                    constexpr float GO_CAMP_ARRIVAL = 10.0f;  // NewRpgStatusUpdateAction: GO_CAMP reached
                    bool const atSpot =
                        place->spot.GetMapId() == bot->GetMapId() && bot->GetExactDist(place->spot) <= GO_CAMP_ARRIVAL;
                    bool const otherZone = !atSpot && (place->spot.GetMapId() != bot->GetMapId() ||
                                                       (place->zoneId && place->zoneId != bot->GetZoneId()));
                    bool const chestAway = place->kind == GatherKind::CHEST && !otherZone &&
                                           bot->GetExactDist(place->spot) > sPlayerbotAIConfig.SightDistance;
                    if (otherZone || chestAway)
                    {
                        // [gather] b337: no judge on the gather walk - 16 of 21 deaths walked a GATHER order (L1 bots
                        // through L5-8 areas), and walks the core gave up (MOVE_FAR_LOOP at the Darnassus / Exodar
                        // portals) were issued again at once. The solo judge first (TripRefused, the bot's own map);
                        // a walk issued again from no nearer than the last one proves the last gave the place up.
                        // Either way the place goes back to the leader for this level and the bot quests
                        std::string refusal;
                        float left = 0.0f;
                        if (place->spot.GetMapId() == bot->GetMapId())
                        {
                            left = bot->GetExactDist(place->spot);
                            if (NewRpgBaseAction::TripRefused(bot, place->spot.GetPositionX(),
                                                              place->spot.GetPositionY(), place->spot.GetPositionZ(),
                                                              "gather place"))
                                refusal = "the solo judge refused the walk (TRIP_REFUSED)";
                        }
                        else
                        {
                            TravelRouteMgr::Leg leg;
                            if (!sTravelRouteMgr.FindRoute(bot, place->spot, leg, left))
                                refusal = "no route to its map";
                        }
                        if (refusal.empty() && st.gatherWalkPlace == order.placeId && left >= st.gatherWalkLeft)
                            refusal = Acore::StringFormat("its last walk ended no nearer ({:.0f} -> {:.0f})",
                                                          st.gatherWalkLeft, left);
                        if (!refusal.empty())
                        {
                            if (refusal.rfind("the solo judge", 0) == 0)  // [assist5] a guard, not the route
                                NoteGatherBlocked(bot, order.placeId, refusal);
                            sGuildGatherPlan.Refuse(bot, order.placeId, refusal);
                            st.gatherWalkPlace = 0;
                            order.executable = false;
                            order.status = "spot left to the leader: " + refusal;
                            continue;
                        }
                        st.gatherWalkPlace = order.placeId;
                        st.gatherWalkLeft = left;
                        step.kind = GuildRunStep::WALK;
                        step.pos = place->spot;
                        step.reason = "GATHER to " + order.details;
                        started(order);
                        return step;
                    }
                    if (place->kind != GatherKind::CHEST)
                        st.gatherWalkPlace = 0;  // arrived (a chest place: its walks to the chests are checked below)
                    // [ench164] a drop place is ground as a skinning place, its loot is the mobs' own
                    bool const drop = place->kind == GatherKind::DROP;
                    if (place->kind == GatherKind::SKIN || drop)
                    {
                        // [questrate] b338 Ainne: the skinning place of its own zone lay in its danger zone of Large
                        // Crag Boar; GO_GRIND refused the walk and the order sent it again (455 s, 36 alerts). The
                        // solo judge first, as for a place in another zone: refused -> back to the leader
                        if (NewRpgBaseAction::TripRefused(bot, place->spot.GetPositionX(), place->spot.GetPositionY(),
                                                          place->spot.GetPositionZ(), "skinning place"))
                        {
                            NoteGatherBlocked(bot, order.placeId, "the solo judge refused the walk (TRIP_REFUSED)");
                            sGuildGatherPlan.Refuse(bot, order.placeId,
                                                    "the solo judge refused the walk (TRIP_REFUSED)");
                            order.executable = false;
                            order.status = "spot left to the leader: the way is dangerous";
                            continue;
                        }
                        // [econ1] b350: GO_GRIND was issued again and again at a skinning place whose mobs were dead
                        // (respawning). At the place (its spot in sight, the grid loaded): a live mob of its entries,
                        // or a corpse still to skin, in sight - else the place goes back to the plan (Assign picks
                        // another) and the bot quests
                        float const sight = sPlayerbotAIConfig.SightDistance;
                        if (bot->GetExactDist(place->spot) <= sight)
                        {
                            bool live = false;
                            for (uint32 const entry : place->entries)
                            {
                                if (bot->FindNearestCreature(entry, sight, true))
                                {
                                    live = true;
                                    break;
                                }
                                if (Creature* corpse = drop ? nullptr : bot->FindNearestCreature(entry, sight, false);
                                    corpse && !corpse->IsAlive() && corpse->HasUnitFlag(UNIT_FLAG_SKINNABLE))
                                {
                                    live = true;
                                    break;
                                }
                            }
                            if (!live)
                            {
                                LOG_INFO("playerbots",
                                         "[econ1] {} L{} skinning place #{} empty: no live mob of it in "
                                         "sight (respawning) - back to the plan",
                                         bot->GetName(), bot->GetLevel(), order.placeId);
                                sGuildGatherPlan.Refuse(bot, order.placeId, "no live mob of the place in sight");
                                order.executable = false;
                                order.status = "spot left to the leader: its mobs are respawning";
                                continue;
                            }
                        }
                        step.kind = GuildRunStep::GRIND;
                        step.pos = place->spot;
                        step.entries = place->entries;  // [econ1] the mobs it grinds (New RPG may aim at them)
                        step.reason = std::string(drop ? "GATHER drop " : "GATHER skin ") + order.details;
                        started(order);
                        return step;
                    }
                    if (place->kind == GatherKind::CHEST)
                    {
                        // [questrate] at the chest place: the nearest spawned chest of it in sight that it can open;
                        // walk to it, then hold while the loot strategy opens it. None left: the place goes back to
                        // the leader (Assign picks again)
                        GameObject* chest = nullptr;
                        float const sight = sPlayerbotAIConfig.SightDistance;
                        for (uint32 const entry : place->entries)
                        {
                            GameObject* go = bot->FindNearestGameObject(entry, sight, true);
                            if (!go || go->getLootState() != GO_READY ||
                                (chest && bot->GetExactDist(go) >= bot->GetExactDist(chest)))
                                continue;
                            if (LootObject loot(bot, go->GetGUID()); !loot.IsEmpty() && loot.IsLootPossible(bot))
                                chest = go;
                        }
                        if (!chest)
                        {
                            sGuildGatherPlan.Refuse(bot, order.placeId, "no chest of the place to open in sight");
                            order.executable = false;
                            order.status = "spot left to the leader: no chests";
                            continue;
                        }
                        float const left = bot->GetExactDist(chest);
                        if (left > sPlayerbotAIConfig.LootDistance)
                        {
                            // the same checks as the walk to the place: the solo judge, and a walk issued again from
                            // no nearer proves the last one gave the chest up (under water, no path)
                            std::string refusal;
                            if (NewRpgBaseAction::TripRefused(bot, chest->GetPositionX(), chest->GetPositionY(),
                                                              chest->GetPositionZ(), "chest"))
                                refusal = "the solo judge refused the walk to its chest (TRIP_REFUSED)";
                            else if (st.gatherWalkPlace == order.placeId && left >= st.gatherWalkLeft)
                                refusal = Acore::StringFormat(
                                    "its last walk to a chest ended no nearer ({:.0f} -> {:.0f})", st.gatherWalkLeft,
                                    left);
                            if (!refusal.empty())
                            {
                                sGuildGatherPlan.Refuse(bot, order.placeId, refusal);
                                st.gatherWalkPlace = 0;
                                order.executable = false;
                                order.status = "spot left to the leader: " + refusal;
                                continue;
                            }
                            st.gatherWalkPlace = order.placeId;
                            st.gatherWalkLeft = left;
                            step.kind = GuildRunStep::WALK;
                            step.pos = WorldPosition(chest);
                            step.reason = "GATHER chest " + order.details;
                            started(order);
                            return step;
                        }
                        st.gatherWalkPlace = 0;
                        step.kind = GuildRunStep::BUSY;  // the loot strategy opens it (AddAllLoot: nearest objects)
                        step.reason = "GATHER chest " + order.details;
                        started(order);
                        return step;
                    }
                }
                step.kind = GuildRunStep::GATHER;
                step.reason = "GATHER " + order.details;
                started(order);
                return step;
            case GuildOrderType::CRAFT_FOR:  // [guild50]
            {
                GuildGear::CraftRecipe const* recipe = GuildGear::Recipe(order.spellId);
                if (!recipe || bot->IsNonMeleeSpellCast(false) || bot->isMoving() || bot->IsInCombat() ||
                    !bot->HasSpell(recipe->spellId) || !HasReagents(bot, *recipe))
                    continue;
                order.executable = false;  // once per order list: the next one comes with the next update
                if (!botAI->CastSpell(recipe->spellId, bot))
                {
                    order.status = "not made here (forge / anvil / tool?) — quests";
                    continue;
                }
                if (order.forLow && order.forLow != low)
                    st.expect.push_back({recipe->itemId, order.forLow, "craft for " + Who(order.forLow)});
                order.status = "creates";
                step.kind = GuildRunStep::BUSY;
                step.reason = "craft " + ItemName(recipe->itemId) + " for " + Who(order.forLow);
                LOG_INFO("playerbots", "[guild50] {} crafts {} for {}: {}", bot->GetName(), ItemName(recipe->itemId),
                         Who(order.forLow), order.reason);
                started(order);
                return step;
            }
            case GuildOrderType::BUY:  // [guild50]
            {
                if (GuildRiding::IsTrainOrder(order))  // [mount1] Apprentice Riding at its trainer
                {
                    if (bot->HasSkill(SKILL_RIDING) || bot->GetMoney() < order.cost || bot->IsInCombat() ||
                        order.pos.GetMapId() != bot->GetMapId() ||
                        NewRpgProfession::BuyFailed(bot, order.npcEntry, order.spellId))
                        continue;
                    order.executable = false;
                    if (NewRpgBaseAction::TripRefused(bot, order.pos.GetPositionX(), order.pos.GetPositionY(),
                                                      order.pos.GetPositionZ(), "riding trainer"))
                    {
                        order.status = "the way to the trainer is dangerous — waits for a level";
                        continue;
                    }
                    NewRpgProfession::StartTrain(botAI, bot, order.spellId, SKILL_RIDING, order.npcEntry, order.pos,
                                                 order.cost);
                    order.status = "walks to the riding trainer";
                    step.kind = GuildRunStep::BUSY;
                    step.reason = "train Apprentice Riding";
                    started(order);
                    return step;
                }
                if (bot->GetItemCount(order.itemId, true) || bot->GetMoney() < order.cost || bot->IsInCombat() ||
                    order.pos.GetMapId() != bot->GetMapId() ||
                    (order.extendedCost && !GuildGear::ExtendedCostMissing(bot, order.extendedCost).empty()) ||
                    NewRpgProfession::BuyFailed(bot, order.npcEntry, order.itemId))
                    continue;
                order.executable = false;
                if (NewRpgBaseAction::TripRefused(bot, order.pos.GetPositionX(), order.pos.GetPositionY(),
                                                  order.pos.GetPositionZ(), "vendor for " + ItemName(order.itemId)))
                {
                    order.status = "the way to the vendor is dangerous — waits for a level";
                    continue;
                }
                NewRpgProfession::StartBuy(botAI, bot, order.itemId, order.npcEntry, order.pos, order.cost);
                order.status = "walks to the vendor";
                step.kind = GuildRunStep::BUSY;
                step.reason = "buy " + ItemName(order.itemId);
                started(order);
                return step;
            }
            case GuildOrderType::REPAIR:  // [regress10] New RPG's vendor run runs it (StartVendorRun reads Repair)
                started(order);
                continue;
            default:
                continue;
        }
    }
    return step;
}

uint8 GuildCoordinator::OnCampReached(Player* bot)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return 0;
    uint32 const low = bot->GetGUID().GetCounter();
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(low);
    if (itr == bots_.end() || !itr->second.run)
        return 0;
    BotState& st = itr->second;
    if (st.runType == GuildOrderType::DUNGEON)  // [guild60] at the five's entrance
        return FiveCampReached(bot, st);
    if (st.runType == GuildOrderType::FOUND_GUILD || st.runType == GuildOrderType::SIGN_CHARTER ||
        st.runType == GuildOrderType::BANK_TAB)  // [guild80] at the Guild Master / the vault
        return FoundCampReached(bot, st);
    if (st.runType == GuildOrderType::TRADE_TO || st.runType == GuildOrderType::MAIL_TO ||
        st.runType == GuildOrderType::DONATE_GOLD)  // [guild81] next to the member / at the mailbox
        return TransferCampReached(bot, st);
    if (!GuildBankRun::AtVault(bot, st.runPos))
    {
        if (!st.runApproached)
        {
            st.runApproached = true;
            bot->GetMotionMaster()->MovePoint(0, st.runPos.GetPositionX(), st.runPos.GetPositionY(),
                                              st.runPos.GetPositionZ());
            return 2;
        }
        if (bot->isMoving())
            return 2;
        st.run = false;
        st.vaultFail = "cannot reach the vault";
        Decision(Who(low) + ": " + st.vaultFail + " — quests on");
        return 1;
    }
    st.run = false;
    if (st.runType == GuildOrderType::BANK_DEPOSIT)
    {
        std::vector<uint32> guids;
        std::vector<HeldItem> moving;
        for (HeldItem const& h : st.held)
            if (h.purpose != "pending" && h.forLow != low && !TransferHolds(h.itemGuidLow))  // [guild81]
            {
                guids.push_back(h.itemGuidLow);
                moving.push_back(h);
            }
        uint32 const moved = GuildBankRun::Deposit(bot, guids);
        // [bank408] a full bank keeps the order off (DepositState); any other refusal releases the item, so the next
        // plan sends the bot to no vault visit that moves nothing again
        for (HeldItem const& h : moving)
        {
            if (Item* left = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(h.itemGuidLow)))
            {
                if (!GuildBankRun::DepositState(bot, h.itemId, left->GetCount()).empty())
                    continue;  // still in the bags: not deposited, waits for room in the bank [bank411]
                LOG_INFO("playerbots", "[bank408] {}: the vault refused {} ({}), released from the bank order",
                         bot->GetName(), ItemName(h.itemId), h.purpose);
                st.held.erase(std::remove_if(st.held.begin(), st.held.end(),
                                             [&](HeldItem const& x) { return x.itemGuidLow == h.itemGuidLow; }),
                              st.held.end());
                continue;
            }
            if (h.forLow)
                withdraw_[h.forLow].emplace_back(h.itemId, "from " + Who(low) + ": " + h.purpose);
            st.held.erase(std::remove_if(st.held.begin(), st.held.end(),
                                         [&](HeldItem const& x) { return x.itemGuidLow == h.itemGuidLow; }),
                          st.held.end());
        }
        Decision(Who(low) + " put into the guild bank " + std::to_string(moved) + " from " +
                 std::to_string(guids.size()) + " items");
    }
    // [bank410] one vault visit takes all its withdrawals, after a deposit too: b460 a member's BANK_DEPOSIT went
    // first, and a withdraw run took one item per visit
    auto& list = withdraw_[low];
    while (!list.empty())
    {
        auto const [itemId, purpose] = list.front();
        list.erase(list.begin());
        // [bank401] a refusal is the game's answer: BankState(withdraw) keeps the order off until it changes
        uint32 taken = 0;
        std::string const why = GuildBankRun::Withdraw(bot, itemId, &taken);
        Decision(Who(low) + (why.empty() ? " took from the bank " : " could not take from the bank ") +
                 ItemName(itemId) + (why.empty() ? "" : " (" + why + ")"));
        // [bank410] an unclaimed bank item gets the global leader's loot decision in its new bags (disenchant
        // itself, sell, ...), as fresh loot
        if (why.empty() && taken && purpose.rfind(UNCLAIMED_BANK, 0) == 0)
            if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(taken)))
            {
                st.held.push_back({taken, itemId, item->GetCount(), 0, "pending"});
                lootQueue_.push_back({low, taken, itemId, item->GetCount()});
            }
        if (!why.empty() && why != "not in the bank")
            break;  // no bag room, no withdraw slots left today: the rest waits
    }
    return 1;
}

bool GuildCoordinator::IsHeld(Player* bot, Item* item)
{
    if (!bot || !item || !IsPlanned(bot->GetGUID().GetCounter()))
        return false;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return false;
    for (HeldItem const& h : itr->second.held)
        if (h.itemGuidLow == item->GetGUID().GetCounter())
            return true;
    // [ench152] an item on the enchanter's disenchant list: no vendor sale before the cast (an unbound green is
    // ITEM_USAGE_VENDOR / AH for the enchanter's own usage value)
    if (std::find(itr->second.disenchant.begin(), itr->second.disenchant.end(), item->GetGUID().GetCounter()) !=
        itr->second.disenchant.end())
        return true;
    // [recipes] a recipe item on its way to the member who learns it, or one it learns itself: no sale
    if (TransferHolds(item->GetGUID().GetCounter()) ||
        std::any_of(recipeLearn_.begin(), recipeLearn_.end(),
                    [&](auto const& r) { return r.second == item->GetGUID().GetCounter(); }))
        return true;
    // [guild50] an item just made / rewarded for another member: held from its first sight (no auto-equip, no sale)
    auto& expect = itr->second.expect;
    for (auto e = expect.begin(); e != expect.end(); ++e)
        if (e->itemId == item->GetEntry())
        {
            itr->second.held.push_back({item->GetGUID().GetCounter(), e->itemId, item->GetCount(), e->forLow,
                                        e->purpose});
            expect.erase(e);
            return true;
        }
    return false;
}

void GuildCoordinator::SellMarked(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    // [human] a human's character sells what he decides himself (the SELL task is a suggestion)
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()) || HumanCharacter(bot))
        return;
    std::vector<uint32> sell;
    {
        CoordLock guard(lock_, __func__);
        auto const itr = bots_.find(bot->GetGUID().GetCounter());
        if (itr == bots_.end())
            return;
        sell.swap(itr->second.sell);
    }
    if (sell.empty())
        return;
    SellAction action(botAI);
    uint32 sold = 0, gone = 0, kept = 0;
    uint32 const before = bot->GetMoney();
    // [vendor20] coordinator 2026-11-09: one decider - what the global leader marked for sale is sold (forced past
    // the sale's own "needed" keeps: class reagents, a skill's materials), only an item a quest in the log still needs
    // for its hand-in stays ([quest18]). b367 Thamesh L11: "kept by SellAction 5" at every visit, the bags 100 % ->
    // 100 %, its q419 reward never had a slot
    std::set<uint32> questItems;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 const questId = bot->GetQuestSlotQuestId(slot))
            if (QuestStatus const status = bot->GetQuestStatus(questId);
                status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                        if (quest->RequiredItemId[i])
                            questItems.insert(quest->RequiredItemId[i]);
    for (uint32 low : sell)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low));
        if (!item)
        {
            ++gone;  // the vendor sale of the bot itself or a use came first
            continue;
        }
        if (questItems.count(item->GetEntry()))
        {
            ++kept;
            continue;
        }
        action.Sell(item, true);

        if (bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low)))
            ++kept;  // SellAction keeps what a quest or a spell still needs
        else
            ++sold;
    }
    LOG_INFO("playerbots", "[guild30] {} coordinator sales: sold {}, already gone {}, kept by SellAction {} (+{}c)",
             bot->GetName(), sold, gone, kept, bot->GetMoney() - before);
}

bool GuildCoordinator::HasMarkedSale(Player* bot) const
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return false;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    return itr != bots_.end() && !itr->second.sell.empty();
}

void GuildCoordinator::SetOrderStatus(Player* bot, std::string const& status)

{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return;
    for (GuildOrder& order : itr->second.orders)
        if (order.executable && !order.waitsTown)
        {
            order.status = status;
            // [econ1] New RPG gave the walk of a gathering place up ("no safe walk there (given up)"): the place goes
            // back to the gathering plan for this level (Refuse) and the next Assign picks another for the bot
            bool const gather =
                order.type == GuildOrderType::GATHER ||
                (order.type == GuildOrderType::PROFESSION && order.profession == GuildProfessionKind::GATHER);
            if (gather && order.placeId &&
                (status.rfind("no safe walk there", 0) == 0 || status.rfind("no nodes for its skill", 0) == 0 ||
                 status.rfind("нет узлов", 0) == 0 || status.rfind("no mob of the place", 0) == 0))  // [craft434]
            {
                sGuildGatherPlan.Refuse(bot, order.placeId, status);
                order.executable = false;
                LOG_INFO("playerbots", "[econ1] {} L{} gives gathering place #{} back: {}", bot->GetName(),
                         bot->GetLevel(), order.placeId, status);
            }
            return;
        }
}

void GuildCoordinator::SetWait(Player* bot, std::string const& reason)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    CoordLock guard(lock_, __func__);
    bots_[bot->GetGUID().GetCounter()].waitReason = reason;
}

void GuildCoordinator::AddAiTime(Player* bot, uint64 microseconds)
{
    aiTimeTotalUs_ += microseconds;
    auto const itr = planIndex_.find(bot->GetGUID().GetCounter());
    if (itr != planIndex_.end() && itr->second < aiCount_)
        aiTimeUs_[itr->second] += microseconds;
}

// ---------------------------------------------------------------------------------------------------------------------
// health monitor
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::OnKilledByCreature(Player* killed, Creature* killer)
{
    if (!killed || !killer || !IsPlanned(killed->GetGUID().GetCounter()))
        return;
    DeathReport report;
    report.playerLow = killed->GetGUID().GetCounter();
    report.creature = true;
    report.killer = killer->GetName() + " L" + std::to_string(killer->GetLevel()) + " (entry " +
                    std::to_string(killer->GetEntry()) + ")";
    report.cause = ConText(killed, killer);
    CoordLock guard(lock_, __func__);
    deathQueue_.push_back(report);
}

void GuildCoordinator::OnJustDied(Player* killed)
{
    if (!killed || !IsPlanned(killed->GetGUID().GetCounter()))
        return;
    DeathReport report;
    report.playerLow = killed->GetGUID().GetCounter();
    report.cause = killed->IsInWater() ? "water / environment" : "environment / not a creature";
    report.killer = "-";
    CoordLock guard(lock_, __func__);
    deathQueue_.push_back(report);
}

void GuildCoordinator::ProcessDeaths()
{
    // One death = one alert. The creature kill and the death itself come from the same Unit::Kill, possibly in two
    // world updates: a death without a creature report waits one update for it; a bot's death stays open (no second
    // alert) until it is seen alive again.
    ++deathCall_;
    std::vector<DeathReport> reports;
    {
        CoordLock guard(lock_, __func__);
        if (deathQueue_.empty() && envPending_.empty())
            return;
        reports.swap(deathQueue_);
    }
    std::map<uint32, DeathReport> byBot;
    {
        CoordLock guard(lock_, __func__);
        for (DeathReport const& r : reports)
        {
            if (r.creature)
            {
                byBot[r.playerLow] = r;
                envPending_.erase(std::remove_if(envPending_.begin(), envPending_.end(),
                                                 [&](auto const& e) { return e.second.playerLow == r.playerLow; }),
                                  envPending_.end());
            }
            else if (!byBot.count(r.playerLow))
                envPending_.emplace_back(deathCall_, r);
        }
        for (auto itr = envPending_.begin(); itr != envPending_.end();)
        {
            if (itr->first < deathCall_)
            {
                if (!byBot.count(itr->second.playerLow))
                    byBot[itr->second.playerLow] = itr->second;
                itr = envPending_.erase(itr);
            }
            else
                ++itr;
        }
        for (auto itr = byBot.begin(); itr != byBot.end();)
        {
            BotState& st = bots_[itr->first];
            if (st.deathOpen)
                itr = byBot.erase(itr);
            else
            {
                st.deathOpen = true;
                ++itr;
            }
        }
    }
    if (byBot.empty())
        return;
    uint32 const now = Now();
    CoordLock guard(lock_, __func__);
    for (auto const& [low, r] : byBot)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        BotState& st = bots_[low];
        st.deaths.push_back(now);
        while (!st.deaths.empty() && now - st.deaths.front() > HOUR)
            st.deaths.pop_front();
        ++deathsTotal_;
        PlanRow const* row = Row(low);
        AreaTableEntry const* zone = p ? sAreaTableStore.LookupEntry(p->GetZoneId()) : nullptr;
        std::ostringstream out;
        out << "DEATH #" << (row ? row->num : 0) << " " << (p ? p->GetName() : row ? row->name : "?") << " L"
            << (p ? p->GetLevel() : 0) << " killed by " << r.killer << " cause=" << r.cause;
        if (p)
            out << " at map " << p->GetMapId() << " (" << int32(p->GetPositionX()) << "," << int32(p->GetPositionY())
                << ") " << (zone ? zone->area_name[0] : "?") << " status=" << StatusName(p, GET_PLAYERBOT_AI(p))
                << " order=" << ActiveTask(st.orders);
        Alert(out.str());
        if (st.deaths.size() >= 2)
            Alert("DEATHS #" + std::to_string(row ? row->num : 0) + " " + (p ? p->GetName() : "?") + ": " +
                  std::to_string(st.deaths.size()) + " deaths in the last hour, last killer " + r.killer);
    }
}

void GuildCoordinator::OnRpgStatus(Player* bot, uint8 status)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    uint64 const xp = XpTotal(bot);
    uint32 const progress = QuestProgress(bot);
    // [econ2] world2 b353: money changing hands (a vendor sale, a BUY, a letter) and profession work (a skill-up of
    // any profession, gathering ones included, a smelt cast) are progress, as UpdateHealth counts them
    uint64 work = uint64(bot->GetMoney());
    for (uint32 const line : CRAFT_LINES)
        work += bot->HasSkill(line) ? uint64(bot->GetPureSkillValue(line)) << 32 : 0;
    for (uint32 const line : {uint32(SKILL_MINING), uint32(SKILL_HERBALISM), uint32(SKILL_SKINNING),
                              uint32(SKILL_FISHING)})
        work += bot->HasSkill(line) ? uint64(bot->GetPureSkillValue(line)) << 32 : 0;
    CoordLock guard(lock_, __func__);
    BotState& st = bots_[bot->GetGUID().GetCounter()];
    work += uint64(st.casts) << 48;
    // [tasks] the gathering pace: seconds spent in DO_GATHER, per gathering skill the bot has
    uint32 const now = Now();
    if (st.rpgStatus == RPG_DO_GATHER && st.rpgStatusAt && now > st.rpgStatusAt)
        for (uint32 skill : {uint32(SKILL_MINING), uint32(SKILL_HERBALISM)})
            if (bot->HasSkill(skill))
            {
                auto& spent = gatherTime_[skill];
                spent.emplace_back(now, now - st.rpgStatusAt);
                while (!spent.empty() && now - spent.front().first > HOUR)
                    spent.pop_front();
            }
    st.rpgStatus = status;
    st.rpgStatusAt = now;
    auto const itr = st.statusEntry.find(status);
    bool noProgress = false;
    if (itr != st.statusEntry.end())
    {
        auto const& [oldXp, oldProgress, x, y, z, map, oldWork] = itr->second;
        noProgress = oldXp == xp && oldProgress == progress && oldWork == work && map == bot->GetMapId() &&
                     bot->GetExactDist(x, y, z) <= std::max(bot->GetCombatReach(), 1.5f);
    }
    if (noProgress && !st.loopAlerted.count(status))
    {
        st.loopAlerted.insert(status);
        PlanRow const* row = Row(bot->GetGUID().GetCounter());
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        Alert("LOOP #" + std::to_string(row ? row->num : 0) + " " + bot->GetName() + " L" +
              std::to_string(bot->GetLevel()) + ": " + RpgName(status) +
              " re-entered without progress (no XP, no quest progress, same place) at map " +
              std::to_string(bot->GetMapId()) + " (" + std::to_string(int32(bot->GetPositionX())) + "," +
              std::to_string(int32(bot->GetPositionY())) + "), last action=" +
              LastAction(ai));
    }
    else if (!noProgress)
        st.loopAlerted.erase(status);
    st.statusEntry[status] = {xp, progress, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                              bot->GetMapId(), work};
}

void GuildCoordinator::UpdateHealth()
{
    uint32 const now = Now();
    // tick time over the last minute
    std::vector<uint32> diffs;
    diffs.reserve(ticks_.size());
    uint64 sum = 0;
    for (auto const& t : ticks_)
    {
        diffs.push_back(t.second);
        sum += t.second;
    }
    std::sort(diffs.begin(), diffs.end());
    uint32 const avg = diffs.empty() ? 0 : uint32(sum / diffs.size());
    uint32 const p95 = diffs.empty() ? 0 : diffs[std::min(diffs.size() - 1, diffs.size() * 95 / 100)];
    uint32 const maxDiff = diffs.empty() ? 0 : diffs.back();
    uint64 aiSum = 0, aiMax = 0;
    for (auto const& t : aiTicks_)
    {
        aiSum += t.second;
        aiMax = std::max<uint64>(aiMax, t.second);
    }
    float const aiAvgMs = aiTicks_.empty() ? 0.0f : float(aiSum) / aiTicks_.size() / 1000.0f;
    int32 const target = sConfigMgr->GetOption<int32>("MinRecordUpdateTimeDiff", 100);

    // per-bot AI time of the last interval
    std::vector<std::pair<uint64, uint32>> aiBots;
    for (size_t i = 0; i < aiCount_; ++i)
        if (uint64 const us = aiTimeUs_[i].exchange(0))
            aiBots.emplace_back(us, plan_[i].low);
    std::sort(aiBots.begin(), aiBots.end(), std::greater<>());

    CoordLock guard(lock_, __func__);
    uint32 online = 0, dead = 0, stuck = 0;
    RateSample sample;
    sample.at = now;
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        BotState& st = bots_[row.low];
        if (!p || !p->IsInWorld())
        {
            st.seen = false;
            st.anchorAt = 0;
            continue;
        }
        ++online;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(p);

        // [stat1] its owner, cached for the status label (StatusName) and the stuck check below; its time goes to the
        // owner's bucket ([qstat] budget DUNGEON / HELP / FOLLOW); a [status] line when the owner changes
        {
            std::string bucket;
            std::string label = ai && !IsRealPlayer(p) ? OwnerLabel(p, ai, st, bucket) : "";
            std::replace(label.begin(), label.end(), ',', ';');  // the alerts' status= field ends at a comma
            size_t const role = label.find("): ");  // a five's head ends at its role (a dungeon name may hold ':')
            std::string const head = bucket.empty() && label.rfind("TASK ", 0) != 0 ? ""
                                     : role != std::string::npos           ? label.substr(0, role + 1)
                                                                           : label.substr(0, label.find(':'));
            if (head != st.ownerLogged)
            {
                st.ownerLogged = head;
                LOG_INFO("playerbots", "[status] {} L{} {} t={}", p->GetName(), uint32(p->GetLevel()),
                         head.empty() ? std::string("RPG ") + RpgName(ai ? uint32(ai->rpgInfo.GetStatus()) : 0)
                                      : label,
                         QuestStats::NowMs());
            }
            st.ownerLabel = std::move(label);
            st.ownerBucket = bucket;
            QuestStats::NoteOwner(p, bucket);
        }

        // per-hour totals from the deltas of characters online at both samples
        uint64 const xp = XpTotal(p);
        uint32 const quests = uint32(p->getRewardedQuests().size());
        uint32 const money = p->GetMoney();
        // [eco200] coordinator 2026-11-11: the members' gold in its mailbox (sent, not taken yet) is still the guild's:
        // the [health] gold counts it (b430: -8.5 g/h while 14.7 g lay unread in the letters)
        uint32 mailGold = 0;
        for (Mail const* m : p->GetMails())
            if (m && m->state != MAIL_STATE_DELETED && m->messageType == MAIL_NORMAL && m->money &&
                IsPlanned(m->sender))
                mailGold += m->money;
        // [questrate] b338: Dandolar / Brelir / Lyleria bought 4-5 vendor upgrades in a row at one vendor (each BUY
        // order its own DO_PROFESSION trip, the next one issued when the last was equipped), standing still with no
        // XP: 20 STUCK alerts under BUY. Money changing hands (bought, sold, mailed, traded) is progress, as a skill
        // that rose is for a crafter
        bool const moneyMoved = st.seen && money != st.money;
        if (st.seen)
        {
            sample.xp += xp > st.xpTotal ? xp - st.xpTotal : 0;
            double levels = 0.0, kills = 0.0;
            XpUnits(p, st.xpLevel, st.xpInLevel, levels, kills);  // [diag260] [diag261]
            sample.levels += levels;
            sample.kills += kills;
            sample.quests += quests > st.questsDone ? quests - st.questsDone : 0;
            sample.money += int64(money) + int64(mailGold) - int64(st.money) - int64(st.mailGold);
        }
        st.seen = true;
        st.xpTotal = xp;
        st.xpLevel = p->GetLevel();
        st.xpInLevel = p->GetUInt32Value(PLAYER_XP);
        st.questsDone = quests;
        st.money = money;
        st.mailGold = mailGold;
        RecordPace(st, p);  // [questrate] its copper per quest XP (the questing formula, UpdateWanted)

        if (p->IsAlive())
            st.deathOpen = false;
        if (!p->IsAlive())
        {
            ++dead;
            st.anchorAt = 0;
            st.stuckAlerted = false;
            st.stillWhy.clear();  // [ui11]
            continue;
        }
        // [guild15] crafting in place: a profession skill that rose since the last sample is progress, as a walk is
        // (the craft casts between the samples are short; the status may be IDLE / DO_QUEST between two crafts)
        uint32 skillSum = 0;
        for (uint32 const line : CRAFT_LINES)
            skillSum += p->HasSkill(line) ? p->GetPureSkillValue(line) : 0;
        // [econ2] the gathering skills too (a smelt or a node raises Mining, a catch Fishing)
        for (uint32 const line : {uint32(SKILL_MINING), uint32(SKILL_HERBALISM), uint32(SKILL_SKINNING),
                                  uint32(SKILL_FISHING)})
            skillSum += p->HasSkill(line) ? p->GetPureSkillValue(line) : 0;
        bool const skillRose = st.skillSum && skillSum > st.skillSum;
        st.skillSum = skillSum;
        // [econ2] b353: 8 of the 15 profession STUCK alerts were miners smelting at a town forge (Galiver, Brelir,
        // Peny, Pherua, Gnetlibozz, Keenonk, Namwe: IDLE, order PROFESSION, a cast every few seconds) - Mining is no
        // craft line and a green smelt raises the skill one cast in several. A smelt cast since the last sample is
        // progress
        // [econ2] world2: a fishing bot's cast (the bobber channel) is progress as a smelt cast is
        bool const casted = st.casts != st.castsSeen ||
                            (NewRpgFishing::IsFishing(p) && p->GetCurrentSpell(CURRENT_CHANNELED_SPELL));
        st.castsSeen = st.casts;
        // [diag252] a fight that makes no progress: b424 Peny L16 (solo holy paladin, Redridge) logged nothing for
        // 1.96 h after "[deaths14] engages Redridge Mongrel" - no kill, no NewRpg tick (its [qstat] logout: QUEST 13 s,
        // COMBAT 0, kills 0), no STUCK alert: a bot in combat was never checked. A solo bot outside an instance whose
        // foes' (attackers', its own and its pet's victims') summed health has not fallen for STUCK_SECONDS is alerted
        // once, with its foes, as STUCK_COMBAT. A diagnostic only, no decision reads it
        if (ai && !IsRealPlayer(p) && p->IsInCombat() && !p->GetGroup() && !p->GetMap()->Instanceable())
        {
            std::vector<Unit*> foes(p->getAttackers().begin(), p->getAttackers().end());
            for (Unit* victim : {p->GetVictim(), p->GetPet() ? p->GetPet()->GetVictim() : nullptr})
                if (victim && std::find(foes.begin(), foes.end(), victim) == foes.end())
                    foes.push_back(victim);
            uint64 foesHp = 0;
            for (Unit* foe : foes)
                foesHp += foe->GetHealth();
            if (!st.combatAnchorAt || foesHp < st.combatFoesHp)
            {
                st.combatAnchorAt = now;
                st.combatAlerted = false;
            }
            st.combatFoesHp = foesHp;
            if (!st.combatAlerted && now - st.combatAnchorAt >= STUCK_SECONDS)
            {
                st.combatAlerted = true;
                std::ostringstream foesText;
                for (Unit* foe : foes)
                    foesText << (foe == foes.front() ? "" : "; ") << foe->GetName() << " L" << uint32(foe->GetLevel())
                             << " " << uint32(foe->GetHealthPct()) << " % hp " << int32(p->GetExactDist(foe))
                             << " yd" << (foe->GetVictim() == p ? " on it" : "")
                             << (foe->IsCreature() && foe->ToCreature()->IsInEvadeMode() ? " evading" : "");
                AreaTableEntry const* zone = sAreaTableStore.LookupEntry(p->GetZoneId());
                std::ostringstream out;
                out << "STUCK_COMBAT #" << row.num << " " << p->GetName() << " L" << uint32(p->GetLevel())
                    << " at map " << p->GetMapId() << " (" << int32(p->GetPositionX()) << ","
                    << int32(p->GetPositionY()) << ") " << (zone ? zone->area_name[0] : "?") << ": "
                    << (now - st.combatAnchorAt) << " s in combat, its foes' health not falling; hp "
                    << uint32(p->GetHealthPct()) << " %, mana " << uint32(p->GetPowerPct(POWER_MANA)) << " %; foes "
                    << foes.size() << " (" << (foes.empty() ? "none: in combat with no attacker or target"
                                                            : foesText.str())
                    << "), engine " << (ai && ai->GetState() == BOT_STATE_COMBAT ? "combat" : "non-combat")
                    << ", status=" << StatusName(p, ai) << ", last action=" << LastAction(ai);
                Alert(out.str());
            }
        }
        else
        {
            st.combatAnchorAt = 0;
            st.combatAlerted = false;
        }

        // stuck: out of combat, alive, not moved beyond its combat reach for 60 s, no legitimate wait
        float const reach = std::max(p->GetCombatReach(), 1.5f);
        if (p->IsInCombat() || !st.anchorAt || st.mapId != p->GetMapId() || skillRose || casted || moneyMoved ||
            p->GetExactDist(st.ax, st.ay, st.az) > reach)
        {
            st.mapId = p->GetMapId();
            st.ax = p->GetPositionX();
            st.ay = p->GetPositionY();
            st.az = p->GetPositionZ();
            st.anchorAt = now;
            st.stuckAlerted = false;
            st.stillWhy.clear();  // [ui11]
            continue;
        }
        // [diag253] a legitimate wait (Resurrection Sickness, a flight, a boat / zeppelin / tram, a rest, ...) is no
        // stuck time: the anchor moves on with it, the 60 s count from the wait's end. b424 Dekorhan L17: "STUCK 440 s"
        // right after its 416 s sickness wait ([deaths14] RECOVER) and its rest to full - the waits were counted
        // [stat1] a member of a help / co-op party or of a followed group stands where its leader stands: it is
        // judged by its leader's own watch (a five member by its dungeon leader's: FiveWait below)
        if (st.ownerBucket == "HELP" || st.ownerBucket == "FOLLOW")
            if (Group* group = p->GetGroup(); group && group->GetLeaderGUID() != p->GetGUID())
            {
                auto const lead = bots_.find(group->GetLeaderGUID().GetCounter());
                bool const leaderStands = lead != bots_.end() && lead->second.anchorAt &&
                                          now - lead->second.anchorAt >= STUCK_SECONDS;
                st.anchorAt = now;
                st.stillWhy = (leaderStands ? "its leader stands (judged there): " : "waits: ") + st.ownerLabel;
                continue;
            }
        std::string const wait = WaitReason(p, ai, st);
        if (!wait.empty())
        {
            st.anchorAt = now;
            st.stillWhy = "waits: " + wait;  // [ui11] BotControl: why it stands
            continue;
        }
        if (now - st.anchorAt < STUCK_SECONDS)
            continue;
        ++stuck;
        std::string const status = StatusName(p, ai);
        std::string const order = ActiveTask(st.orders);
        // [ui11] BotControl's detail pane: the STUCK cause (status, task, the newest actions)
        st.stillWhy = "STUCK " + std::to_string((now - st.anchorAt) / MINUTE) + " min: " + status + ", task " + order +
                      ", actions " + LastAction(ai);
        std::string const key = status + "|" + order.substr(0, order.find(" ("));
        if (st.stuckAlerted && st.stuckKey == key)
            continue;
        st.stuckAlerted = true;
        st.stuckKey = key;
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(p->GetZoneId());
        std::ostringstream out;
        out << "STUCK #" << row.num << " " << p->GetName() << " L" << uint32(p->GetLevel()) << " at map "
            << p->GetMapId() << " (" << int32(p->GetPositionX()) << "," << int32(p->GetPositionY()) << ") "
            << (zone ? zone->area_name[0] : "?") << ": " << (now - st.anchorAt) << " s, status=" << status
            << ", order=" << order << ", last action="
            << LastAction(ai)
            << ", reason=none";
        Alert(out.str());
    }
    sample.deaths = deathsTotal_;
    deathsTotal_ = 0;
    rates_.push_back(sample);
    while (!rates_.empty() && now - rates_.front().at > HOUR)
        rates_.pop_front();
    uint64 xpH = 0;
    double killsH = 0.0, levelsH = 0.0;  // [diag260] [diag261]
    uint32 questsH = 0, deathsH = 0;
    int64 moneyH = 0;
    for (RateSample const& r : rates_)
    {
        xpH += r.xp;
        killsH += r.kills;
        levelsH += r.levels;
        questsH += r.quests;
        deathsH += r.deaths;
        moneyH += r.money;
    }
    uint32 const window = rates_.size() > 1 ? now - rates_.front().at : 0;
    auto const perHour = [window](double v) { return window ? v * HOUR / window : v; };

    // [diag261] the primary rate: same-level kills per hour (guild, and the mean per online bot); [diag260] levels
    double const perBot = online ? 1.0 / online : 0.0;
    char buf[520];
    snprintf(buf, sizeof(buf),
             "tick avg %u / p95 %u / max %u ms (target %d); AI avg %.2f / max %.1f ms/tick; online %u/%u, dead %u, "
             "stuck %u; per hour: quests %.0f, xp %.0f, same-level kills %.0f (%.1f per bot), levels %.2f (%.3f per "
             "bot), deaths %.0f, gold %.1f (window %u min)",
             avg, p95, maxDiff, target, aiAvgMs, aiMax / 1000.0f, online, uint32(plan_.size()), dead, stuck,
             perHour(questsH), perHour(double(xpH)), perHour(killsH), perHour(killsH) * perBot, perHour(levelsH),
             perHour(levelsH) * perBot, perHour(deathsH), perHour(double(moneyH)) / GOLD, window / MINUTE);
    healthRow_ = buf;
    // [tasks] the task lists: waiting town tasks, their wait until started, town tasks per visit
    uint32 waiting = 0, waitingTown = 0;
    for (auto const& [low, st] : bots_)
        for (GuildOrder const& o : st.orders)
        {
            waiting += o.type != GuildOrderType::QUEST ? 1 : 0;
            waitingTown += o.waitsTown ? 1 : 0;
        }
    char tasks[160];
    snprintf(tasks, sizeof(tasks),
             "; tasks: %u, waiting for town %u, town tasks started %u (avg wait %u min, %.1f per visit)", waiting,
             waitingTown, townStarted_, townStarted_ ? uint32(townWaitSum_ / townStarted_ / MINUTE) : 0,
             townVisits_ ? float(townStarted_) / townVisits_ : 0.0f);
    healthRow_ += tasks;
    // [craft401] the grey / white pieces the members wear (the upgrade check's count)
    healthRow_ += Acore::StringFormat("; worn grey {}, white {}", wornGrey_, wornLow_ - wornGrey_);
    if (now - lastHealthLog_ >= MINUTE)
    {
        lastHealthLog_ = now;
        LOG_INFO("playerbots", "[health] {}", healthRow_);
        for (PlanRow const& row : plan_)
        {
            auto const st = bots_.find(row.low);
            if (st == bots_.end() || st->second.orders.size() < 2 || !st->second.seen)
                continue;
            LOG_INFO("playerbots", "[tasks] {}{}: {}", Who(row.low),  // [stat1] what owns it now, if not its New RPG
                     st->second.ownerLabel.empty() ? "" : " (now " + st->second.ownerLabel + ")",
                     TasksText(st->second));
        }
    }

    // efficiency budget: the server's own slow-tick threshold (MinRecordUpdateTimeDiff)
    if (target > 0 && p95 > uint32(target))
    {
        if (!perfAlerted_)
        {
            perfAlerted_ = true;
            std::ostringstream out;
            out << "PERF p95 tick " << p95 << " ms > " << target << " ms (MinRecordUpdateTimeDiff); avg " << avg
                << " max " << maxDiff << "; bot AI avg " << int32(aiAvgMs * 10) / 10.0f << " ms/tick; top AI 5 s:";
            for (size_t i = 0; i < aiBots.size() && i < 5; ++i)
                out << " " << Who(aiBots[i].second) << " " << aiBots[i].first / 1000 << " ms";
            if (PerfMonitor::IsEnabled())
            {
                out << "; pmon:";
                for (auto const& [name, us] : sPerfMonitor.TopTotals(5))
                    out << " " << name.substr(0, 40) << " " << us / 1000 << " ms";
            }
            else
                out << "; pmon off";
            Alert(out.str());
        }
    }
    else
        perfAlerted_ = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// the BotControl «Приказы» tab
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::SendOrders(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;
    uint32 const now = Now();
    CoordLock guard(lock_, __func__);
    SendLine(player, "OS;n=" + std::to_string(plan_.size()));
    SendLine(player, "OR;" + OrdersText(RaidLeaderText('A'), 40) + ";" + OrdersText(RaidLeaderText('B'), 40) + ";" +
                         OrdersText(!found_.state.empty() ? found_.state  // [guild80] founding / bank tab state
                                    : guildState_.empty() ? "no guild: the bank sleeps"
                                                          : guildState_,
                                    80));
    for (auto const& [at, text] : decisions_)
        SendLine(player, "OD;" + std::to_string(now - at) + ";" + OrdersText(text, 200));
    size_t shown = 0;
    for (WantedItem const& w : wanted_)
    {
        if (w.optional || !w.Short() || ++shown > 10)
            continue;
        std::string const source = w.gatherSkill
                                       ? std::string(SkillShort(w.gatherSkill)) + " " + std::to_string(w.gatherReq)
                                   : w.vendor ? "vendor"
                                              : "loot";
        SendLine(player, "OW;" + OrdersText(ItemName(w.itemId), 30) + ";" + std::to_string(w.Short()) + ";" +
                             std::to_string(w.have) + "/" + std::to_string(w.need) + ";" + source + ";" +
                             OrdersText(w.forWhom, 130) + ";R");
    }
    // [guild70] the optional ones (intermediates held somewhere): greyed at the bottom, never ordered
    size_t optionalShown = 0;
    for (WantedItem const& w : wanted_)
        if (w.optional && w.have && ++optionalShown <= 4)
            SendLine(player, "OW;" + OrdersText(ItemName(w.itemId), 30) + ";0;" + std::to_string(w.have) + "/0;bank;" +
                                 OrdersText(w.forWhom, 130) + ";O");
    SendLine(player, "OH;" + OrdersText(healthRow_, 220));
    for (auto const& [at, text] : alerts_)
        SendLine(player, "OA;" + std::to_string(now - at) + ";" + OrdersText(text, 200));
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        auto const st = bots_.find(row.low);
        GuildOrder order;
        if (st != bots_.end() && !st->second.orders.empty())
        {
            order = st->second.orders.front();
            for (GuildOrder const& o : st->second.orders)  // [tasks] the task it runs now, else the first one
                if (o.type != GuildOrderType::QUEST && o.executable && !o.waitsTown)
                {
                    order = o;
                    break;
                }
        }
        std::string status = order.status;
        std::string details = order.details;
        if (!p || !p->IsInWorld())
            status = "offline";
        else if (order.type == GuildOrderType::QUEST)
            status = StatusName(p, GET_PLAYERBOT_AI(p));
        if (st != bots_.end() && order.type != GuildOrderType::BANK_DEPOSIT)
        {
            uint32 hold = 0;
            for (HeldItem const& h : st->second.held)
                hold += h.purpose != "pending" && !TransferHolds(h.itemGuidLow) ? 1 : 0;  // [guild81]
            if (hold)
                details += "; holds for the bank: " + std::to_string(hold);
        }
        uint32 const sinceMin = order.since ? (now - order.since) / MINUTE : 0;
        SendLine(player, "O;" + std::to_string(row.num) + ";" + OrdersText(p ? p->GetName() : row.name, 12) + ";" +
                             OrdersText(Chain(row.low), 60) + ";" + OrderName(order.type) + ";" + OrdersText(details, 70) + ";" +
                             std::to_string(sinceMin) + ";" + OrdersText(status, 60));
        // [guild50] every order with its reason, then the best planned upgrades:
        // OX;num;ORDER;details;reason;status (ORDER = PLAN: details = slot item gain, reason = where from)
        if (st != bots_.end() && p && p->IsInWorld())
        {
            size_t shownX = 0;
            for (GuildOrder const& o : st->second.orders)
            {
                if (o.type == GuildOrderType::QUEST || ++shownX > 5)
                    continue;
                SendLine(player, "OX;" + std::to_string(row.num) + ";" + OrderName(o.type) + ";" +
                                     OrdersText(o.details, 70) + ";" + OrdersText(o.reason, 80) + ";" +
                                     OrdersText(o.status, 50));
            }
            // [tasks] the whole task list: OT;num;TYPE;U|C;place F/T/M/B/V/R;source G/5/S;sinceMin;state;details;
            // reason (state: run = runs now, town:<name> = waits for that town, else the task's status)
            size_t shownT = 0;
            bool runFound = false;
            for (GuildOrder const& o : st->second.orders)
            {
                if (++shownT > TASKS_SENT_MAX)
                    break;
                std::string state = o.status;
                if (o.type == GuildOrderType::QUEST)
                    state = runFound ? "waits" : "run";
                else if (o.waitsTown)
                    state = "town:" + o.townName;
                else if (o.executable && !runFound)
                    state = "run";
                runFound |= state == "run";
                SendLine(player, "OT;" + std::to_string(row.num) + ";" + OrderName(o.type) + ";" +
                                     PriorityCode(o.priority) + ";" + PlaceCode(o.place) + ";" + SourceCode(o.source) +
                                     ";" + std::to_string(o.since ? (now - o.since) / MINUTE : 0) + ";" +
                                     OrdersText(state, 40) + ";" + OrdersText(o.details, 56) + ";" +
                                     OrdersText(o.reason, 70));
            }
            size_t shownP = 0;
            for (GearPick const& g : st->second.gearPlan)
            {
                if (++shownP > 4)
                    break;
                SendLine(player, "OX;" + std::to_string(row.num) + ";PLAN;" +
                                     OrdersText(std::string(GuildGear::GroupName(g.group)) + ": " + ItemName(g.itemId) +
                                                    " " + GainText(g),
                                                90) +
                                     ";" + OrdersText(g.text, 100) + ";");
            }
        }
    }
    if (!bagState_.empty())
        SendLine(player, "OB;" + OrdersText(bagState_, 200));
    SendFives(player);  // [guild60] the fives and the completion table
    SendLine(player, "OE;n=" + std::to_string(plan_.size()));
}

// ---------------------------------------------------------------------------------------------------------------------
// [ui11] the BotControl detail pane of one member (asked for the selected bot only, not for all 75 every 5 s):
//   TS;num;name;level
//   T;i;TYPE;U|H|C;place F/T/M/B/V/R;source G/5/S;sinceMin;state;town;for whom;item   (state as the OT line)
//   TD;i;details   TR;i;reason
//   TG;N|P|F;slot group;item;gain;needLevel;where from   (N = now, P = plan in its level band, F = future)
//   TW;why it stands still ("" = it moves)
//   TE;num
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::SendTasks(Player* player, uint32 num)
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;
    uint32 const now = Now();
    CoordLock guard(lock_, __func__);
    PlanRow const* row = nullptr;
    for (PlanRow const& r : plan_)
        if (r.num == num)
            row = &r;
    if (!row)
    {
        SendLine(player, "TE;" + std::to_string(num));
        return;
    }
    Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row->low));
    bool const online = p && p->IsInWorld();
    SendLine(player, "TS;" + std::to_string(num) + ";" + OrdersText(online ? p->GetName() : row->name, 12) + ";" +
                         std::to_string(online ? p->GetLevel() : 0));
    auto const st = bots_.find(row->low);
    if (online && st != bots_.end())
    {
        uint32 i = 0;
        bool runFound = false;
        for (GuildOrder const& o : st->second.orders)
        {
            ++i;
            std::string state = o.status;  // as the OT line
            if (o.type == GuildOrderType::QUEST)
                state = runFound ? "waits" : "run";
            else if (o.waitsTown)
                state = "town:" + o.townName;
            else if (o.executable && !runFound)
                state = "run";
            runFound |= state == "run";
            std::string const forWhom =
                o.forLow ? Who(o.forLow)
                : o.type == GuildOrderType::BANK_DEPOSIT || o.type == GuildOrderType::DONATE_GOLD
                    ? std::string("guild bank")
                    : std::string("self");
            std::string const item = o.itemId ? ItemName(o.itemId) + (o.count > 1 ? " x" + std::to_string(o.count) : "")
                                              : std::string();
            std::string const n = std::to_string(i);
            SendLine(player, "T;" + n + ";" + OrderName(o.type) + ";" + PriorityCode(o.priority) + ";" +
                                 PlaceCode(o.place) + ";" + SourceCode(o.source) + ";" +
                                 std::to_string(o.since ? (now - o.since) / MINUTE : 0) + ";" + OrdersText(state, 40) +
                                 ";" + OrdersText(o.townName, 30) + ";" + OrdersText(forWhom, 24) + ";" +
                                 OrdersText(item, 50));
            if (!o.details.empty())
                SendLine(player, "TD;" + n + ";" + OrdersText(o.details, 200));
            if (!o.reason.empty())
                SendLine(player, "TR;" + n + ";" + OrdersText(o.reason, 200));
        }
        auto const gear = [&](char const* kind, std::vector<GearPick> const& list)
        {
            for (GearPick const& g : list)
                SendLine(player, std::string("TG;") + kind + ";" + GuildGear::GroupName(g.group) + ";" +
                                     OrdersText(ItemName(g.itemId), 40) + ";" + OrdersText(GainText(g), 40) + ";" +
                                     std::to_string(g.needLevel) + ";" + OrdersText(g.text, 110));
        };
        gear("N", st->second.gearNow);
        gear("P", st->second.gearPlan);
        gear("F", st->second.gearFuture);
        SendLine(player, "TW;" + OrdersText(st->second.stillWhy, 220));
    }
    SendLine(player, "TE;" + std::to_string(num));
}

// ---------------------------------------------------------------------------------------------------------------------
// the BotControl «Ресурсы» tab: the virtual guild bank (all planned characters' bags + bank slots + the guild bank),
// built on request from the players in memory
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::SendResources(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;

    std::unordered_map<uint32, uint32> wantedNeed;
    std::unordered_map<uint32, std::pair<std::string, bool>> wantedWhy;  // [guild70] item -> (why, optional)
    {
        CoordLock guard(lock_, __func__);
        for (WantedItem const& w : wanted_)
        {
            if (w.need)
                wantedNeed[w.itemId] = w.need;
            if (w.need || w.optional)
                wantedWhy[w.itemId] = {w.forWhom, w.optional};
        }
    }
    auto const useful = [&](ItemTemplate const* proto)
    {
        if (!proto || proto->Quality == ITEM_QUALITY_POOR || proto->Class == ITEM_CLASS_QUEST ||
            proto->Bonding == BIND_QUEST_ITEM)
            return false;
        if (wantedNeed.count(proto->ItemId))
            return true;
        return proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_REAGENT ||
               proto->Class == ITEM_CLASS_RECIPE || proto->Class == ITEM_CLASS_GEM;
    };

    struct Stock
    {
        uint32 total{0};
        std::vector<std::pair<uint32, uint32>> holders;  // (num, count), num 0 = the guild bank
    };
    std::map<uint32, Stock> stock;
    auto const add = [&](Item* item, std::map<uint32, uint32>& mine)
    {
        if (item && useful(item->GetTemplate()))
            mine[item->GetEntry()] += item->GetCount();
    };
    Guild* guild = nullptr;
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || !p->IsInWorld())
            continue;
        if (!guild)
            guild = GuildBankRun::BotGuild(p);
        std::map<uint32, uint32> mine;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), mine);
        for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
            add(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), mine);
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    add(bag->GetItemByPos(uint8(j)), mine);
        for (uint8 b = BANK_SLOT_BAG_START; b < BANK_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    add(bag->GetItemByPos(uint8(j)), mine);
        for (auto const& [entry, count] : mine)
        {
            Stock& s = stock[entry];
            s.total += count;
            s.holders.emplace_back(row.num, count);
        }
    }
    std::map<uint32, uint32> bank;
    GuildBankRun::CountBank(guild, bank);
    for (auto const& [entry, count] : bank)
        if (useful(sObjectMgr->GetItemTemplate(entry)))
        {
            Stock& s = stock[entry];
            s.total += count;
            s.holders.emplace_back(0, count);
        }
    for (auto const& [entry, need] : wantedNeed)  // [guild70] needed, nobody holds any yet
        stock[entry];

    SendLine(player, "RS;n=" + std::to_string(stock.size()) + ";bank=" + (guild ? "1" : "0"));
    // [guild70] money: RG;total;guild bank (-1 = no guild);bags   RB;num;name;copper per bot
    {
        uint64 bags = 0;
        std::vector<std::pair<PlanRow const*, uint32>> purses;
        for (PlanRow const& row : plan_)
            if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low)))
                if (p->IsInWorld())
                {
                    bags += p->GetMoney();
                    purses.emplace_back(&row, p->GetMoney());
                }
        int64 const bankMoney = guild ? int64(guild->GetTotalBankMoney()) : -1;
        SendLine(player, "RG;" + std::to_string(bags + uint64(std::max<int64>(0, bankMoney))) + ";" +
                             std::to_string(bankMoney) + ";" + std::to_string(bags));
        std::sort(purses.begin(), purses.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        for (auto const& [row, copper] : purses)
            SendLine(player, "RB;" + std::to_string(row->num) + ";" +
                                 OrdersText(Who(row->low).substr(Who(row->low).find(' ') + 1), 24) + ";" +
                                 std::to_string(copper));
    }
    for (auto& [entry, s] : stock)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        std::sort(s.holders.begin(), s.holders.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        std::string holders;
        for (auto const& [num, count] : s.holders)
        {
            std::string const part =
                (num ? "#" + std::to_string(num) : std::string("bank")) + " ×" + std::to_string(count);
            if (holders.size() + part.size() > 90)
            {
                holders += ", …";
                break;
            }
            holders += (holders.empty() ? "" : ", ") + part;
        }
        auto const need = wantedNeed.find(entry);
        auto const why = wantedWhy.find(entry);
        // R;itemId;quality;name;total;need (0 = not on the wanted list);holders;[guild70] tier R/O/-;why
        SendLine(player, "R;" + std::to_string(entry) + ";" + std::to_string(proto ? proto->Quality : 1) + ";" +
                             OrdersText(proto ? proto->Name1 : std::to_string(entry), 40) + ";" +
                             std::to_string(s.total) + ";" +
                             std::to_string(need != wantedNeed.end() ? need->second : 0) + ";" +
                             OrdersText(holders, 60) + ";" +
                             (why == wantedWhy.end() ? "-" : why->second.second ? "O" : "R") + ";" +
                             (why == wantedWhy.end() ? std::string() : OrdersText(why->second.first, 110)));
    }
    SendLine(player, "RE;n=" + std::to_string(stock.size()));
}

// [guild70] the «План» tab: what the global leader intends. Crafts (skill-ups, upgrade crafts, bags; waiting,
// ready, in progress, done), transfers (bot -> bank -> bot) and the sources of every missing piece.
// PS;n=   P;kind;state;who;what;for;how   PW;why   PE;n=
void GuildCoordinator::OnCrafted(Player* bot, uint32 spellId, uint32 before, uint32 after)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    CoordLock guard(lock_, __func__);
    craftsDone_.push_back({Now(), bot->GetGUID().GetCounter(), spellId, before, after});
    while (craftsDone_.size() > 40)
        craftsDone_.pop_front();
}

// [insc145] a glyph the scribe made goes to a member whose [insc143] need it was: held for it, so the transfer flow
// (UpdateTransfers, a.: trade / mail / bank, whichever costs least) moves it and the member's LearnGlyphAction puts
// it into its free slot. A member another made glyph of the entry is already held for is skipped; the scribe's own
// need keeps it (its own LearnGlyphAction uses it); no member: the scribe keeps it as any crafted product
void GuildCoordinator::OnGlyphCrafted(Player* scribe, Item* item, uint32 count)
{
    if (!scribe || !item || !IsPlanned(scribe->GetGUID().GetCounter()))
        return;
    uint32 const low = scribe->GetGUID().GetCounter();
    uint32 const entry = item->GetEntry();
    CoordLock guard(lock_, __func__);
    auto const need = glyphFor_.find(entry);
    bool const own = need != glyphFor_.end() &&
                     std::find(need->second.begin(), need->second.end(), low) != need->second.end();
    uint32 forLow = 0;
    if (need != glyphFor_.end() && !own)
        for (uint32 member : need->second)
        {
            bool taken = false;
            for (auto const& [holder, st] : bots_)
                for (HeldItem const& h : st.held)
                    taken |= h.itemId == entry && h.forLow == member;
            if (!taken)
            {
                forLow = member;
                break;
            }
        }
    if (!forLow)
    {
        LOG_INFO("playerbots", "[insc145] {} made {}: {}", Who(low), ItemName(entry),
                 own ? "its own need, it uses it" : "no member waits for it, kept");
        return;
    }
    BotState& st = bots_[low];
    st.held.push_back({item->GetGUID().GetCounter(), entry, count, forLow, "glyph for " + Who(forLow)});
    LOG_INFO("playerbots", "[insc145] {} made {}: held for {} (its premade glyph, a free slot), the transfer brings it",
             Who(low), ItemName(entry), Who(forLow));
}

void GuildCoordinator::SendPlan(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;
    struct PlanLine
    {
        std::string kind, state, who, what, forWhom, how, why;
    };
    std::vector<PlanLine> rows;
    auto const kindName = [](uint8 kind)
    {
        switch (kind)
        {
            case GuildGear::SRC_QUEST:
                return "quest";
            case GuildGear::SRC_DROP:
                return "dungeon boss";
            case GuildGear::SRC_CRAFT:
                return "craft";
            case GuildGear::SRC_VENDOR:
                return "vendor";
            case GuildGear::SRC_BANK:
                return "bank";
            case GuildGear::SRC_WORLD:
                return "mobs in the world";
            case GuildGear::SRC_REFDROP:
                return "world drop";
            case GuildGear::SRC_OBJECT:
                return "chest / object";
            case GuildGear::SRC_CONTAINER:
                return "from an item";
            default:
                return "other";
        }
    };
    uint32 const now = Now();

    // skill-ups: what each crafter makes next (the plan's recipe at its skill), or makes now
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || !p->IsInWorld())
            continue;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
        auto const* task = ai ? std::get_if<NewRpgInfo::DoProfession>(&ai->rpgInfo.data) : nullptr;
        for (uint32 line : CRAFT_LINES)
        {
            if (!p->HasSkill(line))
                continue;
            uint32 const value = p->GetPureSkillValue(line), cap = p->GetPureMaxSkillValue(line);
            std::string const skill = std::string(SkillShort(line)) + " " + std::to_string(value) + "/" +
                                      std::to_string(cap);
            if (task && task->kind == NewRpgCraft::TASK_CRAFT && task->skill == line)
            {
                rows.push_back({"craft", "in progress", Who(row.low), NewRpgCraft::RecipeName(task->spellId), "self",
                                task->npcEntry ? "buys a reagent" : "craft", "skill-up " + skill});
                continue;
            }
            auto const list = recipesBySkill_.find(line);
            auto const plan = skillPlan_.find(line);
            if (list == recipesBySkill_.end() || plan == skillPlan_.end())
                continue;
            if (value >= cap)
            {
                rows.push_back({"craft", "waits for the rank", Who(row.low), "—", "self", "trainer",
                                "skill at the rank cap: " + skill});
                continue;
            }
            int32 const index = value < plan->second.size() ? plan->second[value] : -1;
            if (index < 0)
                continue;
            Recipe const& r = list->second[size_t(index)];
            bool const known = p->HasSpell(r.spellId);
            bool mats = true;
            std::string missing;
            for (auto const& [item, n] : r.reagents)
                if (p->GetItemCount(item, false) < n)
                {
                    mats = false;
                    missing += (missing.empty() ? "" : ", ") + ItemName(item) + " ×" + std::to_string(n);
                }
            rows.push_back({"craft",
                            !known ? "learn the recipe"
                            : mats ? "materials there"
                                   : "waits for materials",
                            Who(row.low), ItemName(r.product), "self / bank", "craft",
                            "skill-up " + skill + " to " + std::to_string((r.yellow + r.gray) / 2) +
                                (missing.empty() ? "" : "; missing: " + missing)});
        }
    }

    // money goals: the guild bank tab (the core's price), the members' trainer ranks, tools, errands under way
    {
        Guild* guild = nullptr;
        for (PlanRow const& row : plan_)
            if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low)))
                if (p->IsInWorld() && (guild = GuildBankRun::BotGuild(p)))
                    break;
        uint8 const tabs = guild ? guild->GetPurchasedTabsSize() : 0;
        if (tabs < GUILD_BANK_MAX_TABS)
        {
            uint32 const price = GuildTabPrice(tabs);
            uint64 const have = guild ? guild->GetTotalBankMoney() : 0;
            rows.push_back({"money", guild ? "collected " + Gsc(have) + " from " + Gsc(price) : std::string("no guild"),
                            "guild bank", "bank tab " + std::to_string(tabs + 1) + ": " + Gsc(price), "guild",
                            "guild bank gold",
                            "core price (GuildBankTabCost" + std::to_string(tabs) + ")" +
                                (guild ? "" : "; the bots found the guild by charter, 0 until then")});
        }
        for (PlanRow const& row : plan_)
        {
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
            if (!p || !p->IsInWorld())
                continue;
            uint32 const money = p->GetMoney();
            auto const state = [&](uint32 cost)
            { return money >= cost ? "enough (has " + Gsc(money) + ")" : "saving: has " + Gsc(money); };
            if (uint32 const ranks = NewRpgProfession::NextRankCost(p))
                rows.push_back({"money", state(ranks), Who(row.low), "profession ranks: " + Gsc(ranks), "self",
                                "trainer, own money", "the next rank the trainer teaches now"});
            std::string tools;
            if (uint32 const cost = NewRpgProfession::MissingTools(p, tools))
                rows.push_back(
                    {"money", state(cost), Who(row.low), "tools: " + Gsc(cost), "self", "vendor, own money", tools});
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                if (auto const* task = std::get_if<NewRpgInfo::DoProfession>(&ai->rpgInfo.data))
                    if (task->cost)
                    {
                        char const* const what = task->kind == 1                           ? "rank at the trainer"
                                                 : task->kind == NewRpgCraft::TASK_RECIPES ? "recipes at the trainer"
                                                 : task->kind == NewRpgCraft::TASK_CRAFT   ? "reagents at a vendor"
                                                                                           : "purchase at a vendor";
                        rows.push_back({"money", "on the way: " + state(task->cost), Who(row.low),
                                        std::string(what) + ": " + Gsc(task->cost), "self", "own money",
                                        task->kind == NewRpgCraft::TASK_CRAFT ? NewRpgCraft::RecipeName(task->spellId)
                                                                              : std::string("profession errand")});
                    }
        }
    }

    CoordLock guard(lock_, __func__);
    for (auto const& [low, st] : bots_)  // [guild70] the leader's BUY orders: who pays what
        for (GuildOrder const& o : st.orders)
            if (o.type == GuildOrderType::BUY)
                rows.push_back({"money", o.status, Who(low), ItemName(o.itemId) + ": " + Gsc(o.cost), "self",
                                "vendor, own money", o.reason});
    for (CraftJob const& job : crafts_)
    {
        std::string state = job.ready ? "materials there" : "waits for materials";
        auto const st = bots_.find(job.crafterLow);
        if (st != bots_.end())
            for (GuildOrder const& o : st->second.orders)
                if (o.type == GuildOrderType::CRAFT_FOR && o.spellId == job.spellId && o.status == "creates")
                    state = "in progress";
        rows.push_back({"craft", state, Who(job.crafterLow), ItemName(job.itemId),
                        job.forLow == job.crafterLow ? "self" : Who(job.forLow), job.bag ? "craft (bag)" : "craft",
                        job.why});
    }
    for (CraftDone const& d : craftsDone_)
        rows.push_back({"craft", "done " + std::to_string((now - d.at) / 60) + " min ago", Who(d.low),
                        NewRpgCraft::RecipeName(d.spellId), "self / bank", "craft",
                        "skill " + std::to_string(d.before) + "→" + std::to_string(d.after)});

    // transfers: [guild81] trade / mail / bank between members, then what waits for the guild bank
    {
        std::vector<std::vector<std::string>> transfers;
        TransferPlanRows(transfers);
        for (auto const& t : transfers)
            rows.push_back({t[0], t[1], t[2], t[3], t[4], t[5], t[6]});
    }
    for (auto const& [low, list] : withdraw_)
        for (auto const& [item, why] : list)
            rows.push_back({"transfer", "waits for a bank trip", "bank", ItemName(item), Who(low), "bank", why});
    for (auto const& [low, st] : bots_)
        for (HeldItem const& h : st.held)
        {
            if (h.purpose == "pending" || TransferHolds(h.itemGuidLow))  // [guild81] listed above
                continue;
            std::string const what = ItemName(h.itemId) + (h.count > 1 ? " ×" + std::to_string(h.count) : "");
            rows.push_back({"transfer", st.vaultFail.empty() ? "holds until the bank" : st.vaultFail, Who(low), what,
                            h.forLow ? "bank → " + Who(h.forLow) : std::string("bank"), "bank", h.purpose});
        }

    // sources of what is missing: the wanted list's shortfalls and the members' gear plan
    for (WantedItem const& w : wanted_)
    {
        if (w.optional || !w.Short())
            continue;
        std::string gatherers;
        for (auto const& [low, st] : bots_)
            for (GuildOrder const& o : st.orders)
                if (o.type == GuildOrderType::GATHER && o.itemId == w.itemId)
                    gatherers += (gatherers.empty() ? "" : ", ") + Who(low);
        std::string how = !gatherers.empty() ? "gathering: " + gatherers
                          : w.vendor         ? "vendor (own money)"
                          : w.gatherSkill    ? std::string("gathering ") + SkillShort(w.gatherSkill) + " " +
                                                std::to_string(w.gatherReq) + ": no gatherer with the skill"
                                          : "loot from mobs";
        rows.push_back({"source", "short " + std::to_string(w.Short()), gatherers.empty() ? "—" : gatherers,
                        ItemName(w.itemId), "wanted list", how, w.forWhom});
    }
    for (auto const& [low, st] : bots_)
    {
        std::unordered_set<uint32> seen;
        for (auto const* list : {&st.gearNow, &st.gearPlan})
            for (GearPick const& g : *list)
            {
                if (g.kind == GuildGear::SRC_BANK || !seen.insert(g.itemId).second)
                    continue;
                rows.push_back({"source", list == &st.gearNow ? "available now" : "in the plan",
                                g.crafterLow ? Who(g.crafterLow) : std::string("—"), ItemName(g.itemId), Who(low),
                                kindName(g.kind),
                                std::string("upgrade ") + GuildGear::GroupName(g.group) + " " + GainText(g) +
                                    (g.text.empty() ? "" : ": " + g.text)});
            }
    }

    size_t const sent = std::min<size_t>(rows.size(), 400);
    SendLine(player, "PS;n=" + std::to_string(sent));
    for (size_t i = 0; i < sent; ++i)
    {
        PlanLine const& r = rows[i];
        // bytes (UTF-8): the row, then its reason on a line of its own (the client drops lines over 255 bytes)
        SendLine(player, "P;" + OrdersText(r.kind, 16) + ";" + OrdersText(r.state, 40) + ";" + OrdersText(r.who, 40) +
                             ";" + OrdersText(r.what, 50) + ";" + OrdersText(r.forWhom, 40) + ";" +
                             OrdersText(r.how, 50));
        SendLine(player, "PW;" + OrdersText(r.why, 230));
    }
    SendLine(player, "PE;n=" + std::to_string(sent));
}

// ---------------------------------------------------------------------------------------------------------------------
// scripts
// ---------------------------------------------------------------------------------------------------------------------
class GuildCoordinatorPlayerScript : public PlayerScript
{
public:
    GuildCoordinatorPlayerScript()
        : PlayerScript("GuildCoordinatorPlayerScript",
                       {PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM, PLAYERHOOK_ON_PLAYER_JUST_DIED,
                        PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE, PLAYERHOOK_ON_CREATE_ITEM,
                        PLAYERHOOK_ON_STORE_NEW_ITEM, PLAYERHOOK_ON_EQUIP})
    {
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootguid*/) override
    {
        sGuildCoordinator.OnLoot(player, item, count);
    }

    // [loot159] an item won by a group roll is stored by Group::CountTheRoll, not by the loot window: no
    // OnPlayerLootItem. Every green / blue of a five went past the coordinator (the five's report "loot: none" on all
    // 36 b428 RFC runs while the members held 20+ RFC boss blues; spare BoP never disenchanted, BoE never routed)
    void OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 count, RollVote vote, Roll* /*roll*/) override
    {
        if (player && item)
            LOG_INFO("playerbots", "[loot159] {} wins {} x{} by roll ({})", player->GetName(),
                     item->GetTemplate()->Name1, count, vote == NEED ? "need" : "greed");
        sGuildCoordinator.OnLoot(player, item, count);
    }

    // [guild70] a skill-up craft's product goes the loot way (NewRpgCraft decides which ones)
    void OnPlayerCreateItem(Player* player, Item* item, uint32 count) override
    {
        NewRpgCraft::OnCreateItem(player, item, count);
    }

    // [ench162] a new item in its bags (loot, quest reward, purchase, craft) or a piece equipped (the one it replaced
    // goes to the bags): its spare greens are looked at again
    void OnPlayerStoreNewItem(Player* player, Item* /*item*/, uint32 /*count*/) override
    {
        if (player)
            sGuildCoordinator.MarkBagsChanged(player->GetGUID().GetCounter());
    }

    void OnPlayerEquip(Player* player, Item* /*it*/, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        if (player)
            sGuildCoordinator.MarkBagsChanged(player->GetGUID().GetCounter());
    }

    void OnPlayerJustDied(Player* player) override { sGuildCoordinator.OnJustDied(player); }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        sGuildCoordinator.OnKilledByCreature(killed, killer);
    }
};

class GuildCoordinatorWorldScript : public WorldScript
{
public:
    GuildCoordinatorWorldScript()
        : WorldScript("GuildCoordinatorWorldScript", {WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_STARTUP})
    {
    }

    void OnStartup() override { sGuildCoordinator.Load(); }
    void OnUpdate(uint32 diff) override { sGuildCoordinator.Update(diff); }
};

// [tram26] a tram car of a travel line runs its loop server-side only while its grid is updated: a grid-spawned
// StaticTransport moves in GameObject::Update, which the map runs in active cells only - a car that rolled into the
// unwatched tunnel stood there, the platforms' bots waited a full loop in vain (b375: TRIP_ABANDON "a full loop
// (143333 ms) passed without it docking here" 26 times, Irorm / Idunk / Blahadrin / Namwe stood 850-1205 s in
// DO_QUEST). The car is an active object, as a player watching it keeps it moving
class TramCarActiveScript : public AllGameObjectScript
{
public:
    TramCarActiveScript() : AllGameObjectScript("TramCarActiveScript") {}

    void OnGameObjectAddWorld(GameObject* go) override
    {
        if (!go || go->GetGoType() != GAMEOBJECT_TYPE_TRANSPORT || go->isActiveObject())
            return;
        TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(go->GetEntry());
        if (!line || !line->isStatic)
            return;
        go->setActive(true);
        LOG_INFO("playerbots", "[tram26] tram car {} on map {} kept running (an active object)", go->GetEntry(),
                 go->GetMapId());
    }
};

// [supply111] coordinator 2026-11-10: the guild's supply source by game math, not a fixed choice. Per dungeon band
// (its fives' members), the food and drink a restock buys at a vendor against the cooked food / drink of the band's
// level, each priced per restored point:
// - restored: the item's spell, as the core restores it - SPELL_AURA_MOD_REGEN / MOD_POWER_REGEN give their amount
//   per 5 s (Player::RegenerateHealth: amount x 2 s / 5 s per 2 s tick) over the aura's duration, OBS_MOD_HEALTH /
//   OBS_MOD_POWER their amount per amplitude;
// - vendor: BuyPrice over BuyCount of the best item of the level a vendor sells for gold;
// - cooked: the recipe item's price shared over the band's restocks, the reagents a vendor sells at their price, a
//   fished one at the fisher's time per catch (its best rate by the gathering plan's formula) and a looted one free
//   (it drops while questing), the cast at the cook's time; time at the quest pace (copper per minute by formula,
//   QuestPace) of the guild's best cook and best fisher.
// The cheaper source per point is the band's; logged on change.
void GuildCoordinator::JudgeSupplySource(std::vector<std::pair<PlanRow const*, Player*>> const& members)
{
    static std::once_flag once;
    static std::unordered_set<uint32> sold;               // what a vendor sells for gold
    // recipe spell -> its recipe item and that item's price (vendor sold)
    static std::unordered_map<uint32, std::pair<uint32, uint32>> recipeItem;
    std::call_once(once, []()
    {
        for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
            if (tmpl.npcflag & UNIT_NPC_FLAG_VENDOR)
                if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry))
                    for (VendorItem const* item : items->m_items)
                        if (item && !item->ExtendedCost)
                            sold.insert(item->item);
        for (auto const& [id, proto] : *sObjectMgr->GetItemTemplateStore())
            if (proto.Class == ITEM_CLASS_RECIPE && sold.count(id))
                for (auto const& spell : proto.Spells)
                    if (SpellInfo const* learn = spell.SpellId > 0 ? sSpellMgr->GetSpellInfo(spell.SpellId) : nullptr)
                        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                            if (learn->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL &&
                                learn->Effects[e].TriggerSpell)
                                recipeItem[learn->Effects[e].TriggerSpell] = {id, proto.BuyPrice};
    });
    auto const restores = [](ItemTemplate const* proto, float& hp, float& mana)
    {
        hp = mana = 0.0f;
        SpellInfo const* spell = proto ? sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId) : nullptr;
        if (!spell)
            return;
        float const duration = float(std::max(spell->GetMaxDuration(), 0));
        // [eco204] the core's drink: its MOD_POWER_REGEN holds 0 until its PERIODIC_DUMMY's amount is moved onto it
        // (AuraEffect::UpdatePeriodic) - the plain drinks restored "0" here and only a few newer ones were judged
        float dummy = 0.0f;
        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            if (spell->Effects[e].ApplyAuraName == SPELL_AURA_PERIODIC_DUMMY)
                dummy = float(spell->Effects[e].CalcValue());
        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
        {
            SpellEffectInfo const& effect = spell->Effects[e];
            float const amount = effect.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN && effect.CalcValue() <= 0
                                     ? dummy
                                     : float(effect.CalcValue());
            float const amplitude = float(effect.Amplitude);
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_MOD_REGEN:
                    hp += amount * duration / 5000.0f;
                    break;
                case SPELL_AURA_MOD_POWER_REGEN:
                    if (effect.MiscValue == POWER_MANA)
                        mana += amount * duration / 5000.0f;
                    break;
                case SPELL_AURA_OBS_MOD_HEALTH:
                    if (amplitude > 0.0f)
                        hp += amount * duration / amplitude;
                    break;
                case SPELL_AURA_OBS_MOD_POWER:
                    if (amplitude > 0.0f && effect.MiscValue == POWER_MANA)
                        mana += amount * duration / amplitude;
                    break;
                default:
                    break;
            }
        }
    };
    auto const supplyItem = [](ItemTemplate const& proto, uint32 category)
    {
        return proto.Class == ITEM_CLASS_CONSUMABLE && proto.Spells[0].SpellCategory == category;
    };

    // the guild's best cook and best fisher (the most skill), their quest pace
    Player* cook = nullptr;
    Player* fisher = nullptr;
    float cookPace = -1.0f, fisherPace = -1.0f;
    for (auto const& [row, m] : members)
    {
        if (HumanCharacter(m))
            continue;
        auto const st = bots_.find(row->low);
        if (st == bots_.end())
            continue;
        float pace = 0.0f;
        if (m->HasSkill(SKILL_COOKING) &&
            (!cook || m->GetPureSkillValue(SKILL_COOKING) > cook->GetPureSkillValue(SKILL_COOKING)) &&
            QuestPace(st->second, m, pace))
        {
            cook = m;
            cookPace = pace;
        }
        if (fishers_.count(row->low) && m->HasSkill(SKILL_FISHING) &&
            (!fisher || m->GetPureSkillValue(SKILL_FISHING) > fisher->GetPureSkillValue(SKILL_FISHING)) &&
            QuestPace(st->second, m, pace))
        {
            fisher = m;
            fisherPace = pace;
        }
    }
    auto const cooking = recipesBySkill_.find(SKILL_COOKING);

    std::unordered_map<uint32, std::vector<SupplyCraft>> jobs;  // [supply112] cook -> its jobs
    std::set<std::pair<uint32, uint32>> bands;
    for (DungeonOption const& d : dungeons_)
        bands.emplace(d.walkMinLevel, d.maxLevel);
    // [prof276] the guild's real supply need: the dungeon bands overlap (b430: 12 bands 8-21 ... 20-45, every one with
    // its own cook), and each band counted every member in it - Coyote Meat "need 3185-4951 have 13" for 12 Coyote
    // Steak cooks. A member's restock of a kind is one band's job (the first band holding it), and a job is what its
    // cook can make: the units its looted reagents' stock in the members' bags gives (each unit claimed by one job),
    // plus what it holds already
    std::set<std::pair<uint32, uint32>> counted;  // (member, restock category) in a job already
    std::map<uint32, uint32> guildStock;          // looted reagent -> what the members hold (bags + bank)
    std::map<uint32, uint32> claimed;             // looted reagent -> the units the jobs before took
    auto const stock = [&](uint32 item) -> uint32
    {
        auto it = guildStock.find(item);
        if (it == guildStock.end())
        {
            uint32 n = 0;
            for (auto const& [row, m] : members)
                n += m->GetItemCount(item, true);
            it = guildStock.emplace(item, n).first;
        }
        uint32 const taken = claimed[item];
        return it->second > taken ? it->second - taken : 0u;
    };
    for (auto const& [lo, hi] : bands)
    {
        uint32 inBand = 0;
        uint8 level = 0;
        for (auto const& [row, m] : members)
            if (m->GetLevel() >= lo && m->GetLevel() <= hi && row->grp >= 'A' && row->grp <= 'C')
            {
                ++inBand;
                level = std::max<uint8>(level, m->GetLevel());
            }
        if (!inBand)
            continue;
        // the vendor's best food and drink of the band's top level, per restored point
        auto const vendorBest = [&](uint32 category, bool health, std::string& name) -> float
        {
            float bestPer = -1.0f;
            uint32 bestLevel = 0;
            for (auto const& [id, proto] : *sObjectMgr->GetItemTemplateStore())
            {
                if (!proto.BuyPrice || proto.RequiredLevel > level || !sold.count(id) || !supplyItem(proto, category))
                    continue;
                float hp = 0.0f, mana = 0.0f;
                restores(&proto, hp, mana);
                float const points = health ? hp : mana;
                if (points <= 0.0f)
                    continue;
                float const per = float(proto.BuyPrice) / float(std::max<uint32>(1, proto.BuyCount)) / points;
                if (bestPer < 0.0f || proto.RequiredLevel > bestLevel ||
                    (proto.RequiredLevel == bestLevel && per < bestPer))
                {
                    bestPer = per;
                    bestLevel = proto.RequiredLevel;
                    name = proto.Name1;
                }
            }
            return bestPer;
        };
        std::string foodName, drinkName;
        float const foodPer = vendorBest(RESTOCK_SPELL_CATEGORY_FOOD, true, foodName);
        float const drinkPer = vendorBest(RESTOCK_SPELL_CATEGORY_DRINK, false, drinkName);

        std::string const key = std::to_string(lo) + "-" + std::to_string(hi);
        // [prof274] the band's cook first - its cook before while it still cooks, else the most Cooking skill of the
        // members no other band holds (the guild's best cook when every one is taken) - and only a recipe that cook
        // can learn (its skill reaches the recipe's: trainer_spell ReqSkillRank / the recipe item's
        // RequiredSkillRank) with looted reagents the guild's members hold: b428 every band's job was Goblin Deviled
        // Clams (Cooking 125, Tangy Clam Meat) for cooks of 65 / 74 / 93 while no member held a Tangy Clam Meat -
        // never learned, never cooked, the fives ate vendor food
        Player* bandCook = nullptr;
        if (auto const was = supplyCook_.find(key); was != supplyCook_.end())
            for (auto const& [row, m] : members)
                if (row->low == was->second && m->HasSkill(SKILL_COOKING) && !HumanCharacter(m))
                    bandCook = m;
        if (!bandCook)
        {
            for (auto const& [row, m] : members)
            {
                if (HumanCharacter(m) || !m->HasSkill(SKILL_COOKING))
                    continue;
                bool taken = false;
                for (auto const& [band, low] : supplyCook_)
                    taken = taken || (band != key && low == row->low);
                if (taken)
                    continue;
                if (!bandCook || m->GetPureSkillValue(SKILL_COOKING) > bandCook->GetPureSkillValue(SKILL_COOKING))
                    bandCook = m;
            }
            if (!bandCook)
                bandCook = cook;
        }
        uint32 const cookSkill = bandCook ? bandCook->GetPureSkillValue(SKILL_COOKING) : 0;
        // the cooked food / drink of the band's level: the cheapest per point of those with the highest level
        std::string cookedText = "no cooking recipe of the level";
        float cookedPer = -1.0f;
        float cookedHp = 0.0f, cookedMana = 0.0f;
        SupplyCraft job;
        float jobUnitCost = 0.0f;
        if (cooking != recipesBySkill_.end())
        {
            uint32 bestLevel = 0;
            for (Recipe const& r : cooking->second)
            {
                ItemTemplate const* proto = r.product ? sObjectMgr->GetItemTemplate(r.product) : nullptr;
                if (!proto || proto->RequiredLevel > level ||
                    !(supplyItem(*proto, RESTOCK_SPELL_CATEGORY_FOOD) ||
                      supplyItem(*proto, RESTOCK_SPELL_CATEGORY_DRINK)))
                    continue;
                float hp = 0.0f, mana = 0.0f;
                restores(proto, hp, mana);
                if (hp + mana <= 0.0f)
                    continue;
                // its recipe: learned at the trainer / by itself (no item price), else a vendor's recipe item, else
                // a drop (no price known: not judged)
                float recipePrice = 0.0f;
                uint32 recipeId = 0;
                uint32 reqSkill = r.reqSkill;
                if (!r.trainer && !r.autoLearned)
                {
                    auto const item = recipeItem.find(r.spellId);
                    if (item == recipeItem.end())
                        continue;
                    recipeId = item->second.first;
                    recipePrice = float(item->second.second);
                    if (ItemTemplate const* recipeProto = sObjectMgr->GetItemTemplate(recipeId))
                        reqSkill = std::max(reqSkill, recipeProto->RequiredSkillRank);
                }
                // [prof274] a recipe the band's cook knows or its skill can learn
                if (!bandCook || (!bandCook->HasSpell(r.spellId) && reqSkill > cookSkill))
                    continue;
                float coin = 0.0f, seconds = 0.0f;
                std::string reagents;
                bool priced = true;
                for (auto const& [item, n] : r.reagents)
                {
                    ItemTemplate const* reagent = sObjectMgr->GetItemTemplate(item);
                    uint32 skill = 0;
                    if (reagent && sold.count(item) && reagent->BuyPrice)
                    {
                        coin += float(n) * float(reagent->BuyPrice) / float(std::max<uint32>(1, reagent->BuyCount));
                        reagents += Acore::StringFormat("{} x{} bought, ", reagent->Name1, n);
                    }
                    else if (sGuildGatherPlan.Gatherable(item, &skill) && skill == SKILL_FISHING)
                    {
                        float const rate = fisher ? sGuildGatherPlan.BestRate(item, fisher) : 0.0f;
                        if (rate <= 0.0f || fisherPace < 0.0f)
                        {
                            priced = false;
                            break;
                        }
                        seconds += float(n) * 3600.0f / rate;
                        coin += float(n) * 60.0f / rate * fisherPace;  // the fisher's minutes per catch at its pace
                        reagents += Acore::StringFormat("{} x{} fished ({:.0f}/h), ", reagent ? reagent->Name1 : "?",
                                                        n, rate);
                    }
                    else
                    {
                        // [prof274] a looted reagent the members hold (the transfer brings it to the cook)
                        uint32 const held = stock(item);
                        if (held < n)
                        {
                            priced = false;
                            break;
                        }
                        reagents += Acore::StringFormat("{} x{} looted (members hold {}), ",
                                                        reagent ? reagent->Name1 : "?", n, held);
                    }
                }
                if (!priced || cookPace < 0.0f)
                    continue;
                SpellInfo const* craft = sSpellMgr->GetSpellInfo(r.spellId);
                float const castMin = craft ? float(craft->CalcCastTime()) / 60000.0f : 0.0f;
                coin += castMin * cookPace;
                float const made = std::max(r.made, 1.0f);
                float const units = float(inBand * RESTOCK_FOOD_TARGET);
                float const unit = coin / made + recipePrice / units;
                float const per = unit / (hp + mana);
                if (cookedPer < 0.0f || proto->RequiredLevel > bestLevel ||
                    (proto->RequiredLevel == bestLevel && per < cookedPer))
                {
                    cookedPer = per;
                    bestLevel = proto->RequiredLevel;
                    cookedHp = hp;
                    cookedMana = mana;
                    // [supply112] the job a 'cook' verdict gives the band's cook
                    job = SupplyCraft();
                    job.spellId = r.spellId;
                    job.product = r.product;
                    job.category = proto->Spells[0].SpellCategory;
                    job.made = made;
                    job.castMin = castMin;
                    job.recipeItem = recipeId;
                    job.recipePrice = uint32(recipePrice);
                    job.reagents = r.reagents;
                    jobUnitCost = coin / made;
                    if (reagents.size() >= 2)
                        reagents.resize(reagents.size() - 2);
                    cookedText = Acore::StringFormat("{} {:.3f} c/point ({:.0f} hp + {:.0f} mana; {:.1f} c a unit: "
                                                     "recipe {:.0f} c over {:.0f} restocks, {}, cast {:.1f} s at the "
                                                     "cook's {:.0f} c/min)",
                                                     proto->Name1, per, hp, mana, unit, recipePrice, units, reagents,
                                                     castMin * 60.0f, cookPace);
                }
            }
        }
        // the vendor's price for the same points: food for its health, drink for its mana (a cooked item that restores
        // both replaces both)
        float vendorPer = -1.0f;
        if (cookedPer >= 0.0f && foodPer > 0.0f && (cookedMana <= 0.0f || drinkPer > 0.0f))
            vendorPer = (cookedHp * foodPer + cookedMana * std::max(drinkPer, 0.0f)) / (cookedHp + cookedMana);
        std::string const choice = cookedPer >= 0.0f && vendorPer > 0.0f && cookedPer < vendorPer ? "cook" : "vendor";
        std::string const text = Acore::StringFormat(
            "band {} ({} members, L{}): vendor {} {:.3f} c/hp, {} {:.3f} c/mana; cooked {} -> {} (cook {}, fisher {})",
            key, inBand, uint32(level), foodName.empty() ? "-" : foodName, foodPer, drinkName.empty() ? "-" : drinkName,
            drinkPer, cookedText, choice, cook ? cook->GetName() : "none", fisher ? fisher->GetName() : "none");
        if (supplyJudged_[key] != text)
        {
            supplyJudged_[key] = text;
            LOG_INFO("playerbots", "[supply111] {}", text);
        }

        // [supply112] coordinator 2026-11-10 (the user: the fishers fish for the cook, the guild cook cooks for the
        // guild): a 'cook' band gets one cook - its cook before while it still cooks, else the most Cooking skill of
        // the members no other band holds (the guild's best cook when every one is taken). Its job: the units the
        // band's members lack of a restock of that kind (RESTOCK_FOOD_TARGET each, a drink only for mana users)
        if (choice != "cook")
        {
            if (auto const was = supplyCook_.find(key); was != supplyCook_.end())
            {
                LOG_INFO("playerbots", "[supply112] band {}: vendor again, {} cooks no more for it", key,
                         Who(was->second));
                supplyCook_.erase(was);
            }
            continue;
        }
        if (!bandCook)
            continue;
        uint32 const cookLow = bandCook->GetGUID().GetCounter();
        // [prof276] the members' lack counted once (the first band holding a member), capped by what the cook can
        // make: what it holds of the product (bags, bank, its letters on the way) plus the crafts its looted
        // reagents' unclaimed stock gives (a vendor reagent is bought, a fish is the fishers' catch: no stock bound)
        uint32 lack = 0;
        std::vector<uint32> lackers;
        for (auto const& [row, m] : members)
            if (m->GetLevel() >= lo && m->GetLevel() <= hi && row->grp >= 'A' && row->grp <= 'C' &&
                (job.category != RESTOCK_SPELL_CATEGORY_DRINK || m->GetMaxPower(POWER_MANA) > 0) &&
                !counted.count({row->low, job.category}))
                if (uint32 const have = CountBagFood(m, job.category); have < RESTOCK_FOOD_TARGET)
                {
                    lack += RESTOCK_FOOD_TARGET - have;
                    lackers.push_back(row->low);
                }
        uint32 heldProduct = bandCook->GetItemCount(job.product, true);
        for (auto const& [id, t] : transfers_)
            if (t.fromLow == cookLow && t.itemId == job.product && t.mailed)
                heldProduct += t.count;
        uint32 crafts = UINT32_MAX;
        std::string limit;
        std::vector<uint32> bounded;  // its looted reagents
        for (auto const& [item, n] : job.reagents)
        {
            ItemTemplate const* reagent = sObjectMgr->GetItemTemplate(item);
            uint32 skill = 0;
            if ((reagent && sold.count(item) && reagent->BuyPrice) ||
                (sGuildGatherPlan.Gatherable(item, &skill) && skill == SKILL_FISHING))
                continue;
            bounded.push_back(item);
            if (uint32 const left = stock(item); left / n < crafts)
            {
                crafts = left / n;
                limit = ItemName(item) + " " + std::to_string(left);
            }
        }
        uint32 const canMake = crafts == UINT32_MAX ? lack : heldProduct + uint32(float(crafts) * job.made);
        job.target = std::min(lack, canMake);
        uint32 const owedCrafts =
            job.target > heldProduct ? uint32(std::ceil(float(job.target - heldProduct) / job.made)) : 0;
        std::string const sized =
            Acore::StringFormat("{} units: lack {} of {} members not in an earlier band's job, it holds {}{}",
                                job.target, lack, lackers.size(), heldProduct,
                                limit.empty() ? std::string() : ", the members' unclaimed " + limit);
        if (!job.target)
        {
            if (auto const was = supplyCook_.find(key); was != supplyCook_.end())
            {
                LOG_INFO("playerbots", "[prof276] band {}: {} cooks nothing for it ({})", key, Who(was->second),
                         sized);
                supplyCook_.erase(was);
                supplySized_.erase(key);
            }
            continue;
        }
        for (uint32 low : lackers)
            counted.insert({low, job.category});
        for (uint32 item : bounded)
            for (auto const& [reagentItem, n] : job.reagents)
                if (reagentItem == item)
                    claimed[item] += owedCrafts * n;
        if (auto const was = supplyCook_.find(key); was == supplyCook_.end() || was->second != cookLow)
            LOG_INFO("playerbots", "[supply112] band {}: {} (cooking {}) is its cook: {} [prof274]", key, Who(cookLow),
                     cookSkill, ItemName(job.product));
        if (supplySized_[key] != sized)
        {
            supplySized_[key] = sized;
            LOG_INFO("playerbots", "[prof276] band {}: {} cooks {} {}", key, Who(cookLow), ItemName(job.product),
                     sized);
        }
        supplyCook_[key] = cookLow;
        job.band = key;
        job.lo = lo;
        job.hi = hi;
        job.savedPerUnit = std::max(0.0f, vendorPer * (cookedHp + cookedMana) - jobUnitCost);
        jobs[cookLow].push_back(job);
    }
    supplyCrafts_ = std::move(jobs);
}

// [supply112] the crafts still owed: the job's units less what the cook holds of the product (bags and bank), less
// what it has sent and the members have not taken yet (its letters on the way), through the recipe's yield
uint32 GuildCoordinator::SupplyOwed(Player* cook, SupplyCraft const& job) const
{
    if (!cook || !job.target)
        return 0;
    uint32 held = cook->GetItemCount(job.product, true);
    for (auto const& [id, t] : transfers_)
        if (t.fromLow == cook->GetGUID().GetCounter() && t.itemId == job.product && t.mailed)
            held += t.count;
    if (held >= job.target)
        return 0;
    return uint32(std::ceil(float(job.target - held) / std::max(job.made, 1.0f)));
}

uint32 GuildCoordinator::SupplyCrafts(Player* cook, uint32 spellId) const
{
    if (!cook || !cook->HasSpell(spellId))
        return 0;
    CoordLock guard(lock_, __func__);
    uint32 owed = 0;
    // [eco194] ... and the alchemist's potion jobs
    for (auto const* crafts : {&supplyCrafts_, &potionCrafts_})
        if (auto const jobs = crafts->find(cook->GetGUID().GetCounter()); jobs != crafts->end())
            for (SupplyCraft const& job : jobs->second)
                if (job.spellId == spellId)
                    owed += SupplyOwed(cook, job);
    return owed;
}

// [eco206] coordinator 2026-11-11: a member of a forming / gathering five restocks to the drinks its run needs
// ([dun194]: the five gate's mana over the dungeon plus one refill, over one drink's mana), not the fixed 20 / 40 - it
// went in short of the five's own line
uint32 GuildCoordinator::FiveDrinkTarget(Player* bot) const
{
    if (!bot)
        return 0;
    CoordLock guard(lock_, __func__);
    auto const st = bots_.find(bot->GetGUID().GetCounter());
    if (st == bots_.end() || !st->second.fiveId)
        return 0;
    auto const five = fives_.find(st->second.fiveId);
    if (five == fives_.end() || five->second.stage != GuildFive::Stage::Gather)
        return 0;
    auto const need = five->second.drinkNeed.find(bot->GetGUID().GetCounter());
    return need != five->second.drinkNeed.end() ? need->second : 0;
}

// [eco192] the rest's time price for the restock's choice (RestockFoodAction: price and rest time per point)
float GuildCoordinator::RestCopperPerSecond(Player* bot)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return -1.0f;
    float perMinute = 0.0f;
    {
        CoordLock guard(lock_, __func__);
        auto const st = bots_.find(bot->GetGUID().GetCounter());
        if (st == bots_.end() || !QuestPace(st->second, bot, perMinute) || perMinute <= 0.0f)
            return -1.0f;
    }
    // the group rests with it (RestState::GroupResting): each member on its map waits out the same seconds
    uint32 waiting = 1;
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource();
                member && member != bot && member->IsInWorld() && member->GetMapId() == bot->GetMapId())
                ++waiting;
    return perMinute / float(MINUTE) * float(waiting);
}

// [supply112] coordinator 2026-11-10: a profession is worth what it produces for the guild's needs too, not only the
// gear and bag crafts its skill unlocks - the cook's supply jobs at what each unit saves against the vendor, over the
// casts they take (a recipe it has not learned yet: its BUY order comes first, no value until it knows it)
void GuildCoordinator::SupplyWork(uint32 low, Player* p, double& value, double& minutes, std::string& text) const
{
    value = minutes = 0.0;
    auto const jobs = supplyCrafts_.find(low);
    if (jobs == supplyCrafts_.end())
        return;
    for (SupplyCraft const& job : jobs->second)
    {
        if (!p->HasSpell(job.spellId))
        {
            text += Acore::StringFormat("{}{}: recipe not known yet ({})", text.empty() ? "" : "; ",
                                        ItemName(job.product),
                                        job.recipeItem ? "buys " + ItemName(job.recipeItem)
                                                       : std::string("learns it at a trainer [prof274]"));
            continue;
        }
        uint32 const owed = SupplyOwed(p, job);
        if (!owed)
            continue;
        value += double(owed) * job.made * job.savedPerUnit;
        minutes += double(owed) * job.castMin;
        text += Acore::StringFormat("{}{} x{} for band {} ({:.1f} c saved a unit)", text.empty() ? "" : "; ",
                                    ItemName(job.product), uint32(owed * job.made), job.band, job.savedPerUnit);
    }
}

// [eco194] the potion kind of an item: a potion whose spell heals (SPELL_EFFECT_HEAL) or gives mana
// (SPELL_EFFECT_ENERGIZE, POWER_MANA); points: the effect's mean amount (BasePoints + the mean of its die)
uint8 GuildCoordinator::PotionKindOf(ItemTemplate const* proto, float* points)
{
    if (points)
        *points = 0.0f;
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_POTION)
        return POTION_KINDS;
    for (auto const& spell : proto->Spells)
    {
        SpellInfo const* info = spell.SpellId > 0 ? sSpellMgr->GetSpellInfo(spell.SpellId) : nullptr;
        for (uint8 e = 0; info && e < MAX_SPELL_EFFECTS; ++e)
        {
            SpellEffectInfo const& effect = info->Effects[e];
            uint8 const kind = effect.Effect == SPELL_EFFECT_HEAL ? POTION_HEAL
                               : effect.Effect == SPELL_EFFECT_ENERGIZE && effect.MiscValue == POWER_MANA
                                   ? POTION_MANA
                                   : POTION_KINDS;
            if (kind == POTION_KINDS)
                continue;
            if (points)
                *points = float(effect.BasePoints) + float(std::max(effect.DieSides, 1) + 1) / 2.0f;
            return kind;
        }
    }
    return POTION_KINDS;
}

void GuildCoordinator::NotePotionUse(Player* bot, uint32 itemId)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()) || HumanCharacter(bot))
        return;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    uint8 const kind = PotionKindOf(proto);
    if (kind >= POTION_KINDS)
        return;
    uint32 used = 0, owed = 0;
    {
        CoordLock guard(lock_, __func__);
        PotionBook& book = potions_[bot->GetGUID().GetCounter()];
        used = ++book.used[kind];
        owed = book.Owed(kind);
    }
    LOG_INFO("playerbots",
             "[eco194] {} L{} drinks {} ({} potion {} this session, {} not replaced yet): the guild's alchemists "
             "make its replacement",
             bot->GetName(), bot->GetLevel(), proto->Name1, kind == POTION_HEAL ? "healing" : "mana", used, owed);
}

void GuildCoordinator::JudgePotionSupply(std::vector<std::pair<PlanRow const*, Player*>> const& members)
{
    // the potions a vendor sells for gold: the cheapest per point of each kind prices the alchemists' potions
    static std::once_flag once;
    static float vendorPer[POTION_KINDS] = {-1.0f, -1.0f};
    static std::string vendorName[POTION_KINDS];
    static std::unordered_set<uint32> sold;
    std::call_once(once, []()
    {
        for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
            if (tmpl.npcflag & UNIT_NPC_FLAG_VENDOR)
                if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry))
                    for (VendorItem const* item : items->m_items)
                        if (item && !item->ExtendedCost)
                            sold.insert(item->item);
        for (uint32 id : sold)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id);
            float points = 0.0f;
            uint8 const kind = PotionKindOf(proto, &points);
            if (kind >= POTION_KINDS || points <= 0.0f || !proto->BuyPrice)
                continue;
            float const per = float(proto->BuyPrice) / float(std::max<uint32>(1, proto->BuyCount)) / points;
            if (vendorPer[kind] < 0.0f || per < vendorPer[kind])
            {
                vendorPer[kind] = per;
                vendorName[kind] = proto->Name1;
            }
        }
    });
    auto const kindName = [](uint8 kind) { return kind == POTION_HEAL ? "healing" : "mana"; };

    // the members' books: what their bags hold now against the last look, net of the potions drunk in between
    for (auto const& [row, m] : members)
    {
        if (HumanCharacter(m))
            continue;
        uint32 held[POTION_KINDS]{};
        auto const count = [&](Item* item)
        {
            if (uint8 const kind = item ? PotionKindOf(item->GetTemplate()) : uint8(POTION_KINDS);
                kind < POTION_KINDS)
                held[kind] += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            count(m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = m->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    count(bag->GetItemByPos(uint8(slot)));
        PotionBook& book = potions_[row->low];
        for (uint8 kind = 0; kind < POTION_KINDS; ++kind)
        {
            if (book.looked)
            {
                int64 const rise = int64(held[kind]) - int64(book.held[kind]) + int64(book.used[kind]) -
                                   int64(book.usedAtLook[kind]);
                if (rise > 0)
                    book.got[kind] += uint32(rise);
            }
            book.held[kind] = held[kind];
            book.usedAtLook[kind] = book.used[kind];
        }
        book.looked = true;
    }

    // the alchemists and the potion recipes they know; the guild's best herbalist prices the herbs
    struct Maker
    {
        uint32 low{0};
        Player* p{nullptr};
        float pace{-2.0f};  // its quest pace (< 0: none), read for its first job (-2: not yet)
        uint32 units{0};
        std::vector<Recipe const*> recipes[POTION_KINDS];
    };
    std::vector<Maker> makers;
    Player* herbalist = nullptr;
    float herbalistPace = -1.0f;
    auto const alchemy = recipesBySkill_.find(SKILL_ALCHEMY);
    for (auto const& [row, m] : members)
    {
        if (HumanCharacter(m))
            continue;
        bool const herbs = m->HasSkill(SKILL_HERBALISM) &&
                           (!herbalist ||
                            m->GetPureSkillValue(SKILL_HERBALISM) > herbalist->GetPureSkillValue(SKILL_HERBALISM));
        bool const brews = m->HasSkill(SKILL_ALCHEMY) && alchemy != recipesBySkill_.end();
        if (herbs)
            herbalist = m;
        if (!brews)
            continue;
        Maker maker;
        maker.low = row->low;
        maker.p = m;
        for (Recipe const& r : alchemy->second)
            if (r.product && m->HasSpell(r.spellId))
                if (uint8 const kind = PotionKindOf(sObjectMgr->GetItemTemplate(r.product)); kind < POTION_KINDS)
                    maker.recipes[kind].push_back(&r);
        makers.push_back(std::move(maker));
    }
    // the paces (QuestPace: a walk over its quest log) only of those that price a job
    auto const paceOf = [&](Player* p)
    {
        float pace = -1.0f;
        if (auto const st = bots_.find(p->GetGUID().GetCounter()); st == bots_.end() || !QuestPace(st->second, p, pace))
            pace = -1.0f;
        return pace;
    };
    bool herbalistRead = false;

    // each member's owed potions of a kind go to the alchemist with the fewest units that knows a recipe of the kind
    // whose potion the member can use (its best such potion)
    std::map<std::pair<uint32, uint32>, SupplyCraft> jobs;  // (alchemist, recipe spell) -> its job
    uint32 drunk[POTION_KINDS]{}, owedAll[POTION_KINDS]{}, owedBy[POTION_KINDS]{}, unmade[POTION_KINDS]{};
    for (auto const& [row, m] : members)
    {
        auto const book = potions_.find(row->low);
        if (book == potions_.end())
            continue;
        for (uint8 kind = 0; kind < POTION_KINDS; ++kind)
        {
            drunk[kind] += book->second.used[kind];
            uint32 const owed = book->second.Owed(kind);
            if (!owed)
                continue;
            owedAll[kind] += owed;
            ++owedBy[kind];
            Maker* best = nullptr;
            Recipe const* bestRecipe = nullptr;
            for (Maker& maker : makers)
            {
                Recipe const* pick = nullptr;
                uint32 pickLevel = 0;
                for (Recipe const* r : maker.recipes[kind])
                {
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(r->product);
                    if (!proto || proto->RequiredLevel > m->GetLevel() || (pick && proto->RequiredLevel < pickLevel))
                        continue;
                    pick = r;
                    pickLevel = proto->RequiredLevel;
                }
                if (pick && (!best || maker.units < best->units))
                {
                    best = &maker;
                    bestRecipe = pick;
                }
            }
            if (!best)
            {
                unmade[kind] += owed;
                continue;
            }
            best->units += owed;
            SupplyCraft& job = jobs[{best->low, bestRecipe->spellId}];
            job.target += owed;
            if (job.spellId)
                continue;
            // the job's worth: the vendor's price of the same points less its reagents and the cast
            float points = 0.0f;
            PotionKindOf(sObjectMgr->GetItemTemplate(bestRecipe->product), &points);
            float coin = 0.0f;
            for (auto const& [item, n] : bestRecipe->reagents)
            {
                ItemTemplate const* reagent = sObjectMgr->GetItemTemplate(item);
                uint32 skill = 0;
                if (reagent && sold.count(item) && reagent->BuyPrice)
                    coin += float(n) * float(reagent->BuyPrice) / float(std::max<uint32>(1, reagent->BuyCount));
                else if (sGuildGatherPlan.Gatherable(item, &skill) && skill == SKILL_HERBALISM && herbalist)
                {
                    if (!herbalistRead)
                    {
                        herbalistRead = true;
                        herbalistPace = paceOf(herbalist);
                    }
                    float const rate = sGuildGatherPlan.BestRate(item, herbalist);
                    if (rate > 0.0f && herbalistPace > 0.0f)
                        coin += float(n) * 60.0f / rate * herbalistPace;  // its minutes per herb at its pace
                }
            }
            SpellInfo const* craft = sSpellMgr->GetSpellInfo(bestRecipe->spellId);
            float const castMin = craft ? float(craft->CalcCastTime()) / 60000.0f : 0.0f;
            if (best->pace < -1.5f)
                best->pace = paceOf(best->p);
            if (best->pace > 0.0f)
                coin += castMin * best->pace;
            float const made = std::max(bestRecipe->made, 1.0f);
            job.band = "potions";
            job.lo = 0;
            job.hi = 255;
            job.spellId = bestRecipe->spellId;
            job.product = bestRecipe->product;
            job.made = made;
            job.castMin = castMin;
            job.reagents = bestRecipe->reagents;
            job.potionKind = kind + 1;
            job.savedPerUnit = vendorPer[kind] > 0.0f ? std::max(0.0f, vendorPer[kind] * points - coin / made) : 0.0f;
        }
    }

    std::unordered_map<uint32, std::vector<SupplyCraft>> crafts;
    std::string jobText;
    for (auto const& [key, job] : jobs)
    {
        jobText += Acore::StringFormat("{}{} {} x{} ({:.1f} c saved a unit)", jobText.empty() ? "" : ", ",
                                       Who(key.first), ItemName(job.product), job.target, job.savedPerUnit);
        crafts[key.first].push_back(job);
    }
    potionCrafts_ = std::move(crafts);
    std::string text;
    for (uint8 kind = 0; kind < POTION_KINDS; ++kind)
        text += Acore::StringFormat("{}{} drunk {}, owed {} to {} members (vendor {} {:.3f} c/point){}",
                                    kind ? "; " : "", kindName(kind), drunk[kind], owedAll[kind], owedBy[kind],
                                    vendorName[kind].empty() ? "-" : vendorName[kind], vendorPer[kind],
                                    unmade[kind] ? Acore::StringFormat(", {} no alchemist makes", unmade[kind])
                                                 : std::string());
    text += "; jobs: " + (jobText.empty() ? std::string("none") : jobText) + "; alchemists " +
            std::to_string(makers.size());
    if (text != potionJudged_)
    {
        potionJudged_ = text;
        LOG_INFO("playerbots", "[eco194] potions: {}", text);
    }
}

void GuildCoordinator::PotionWork(uint32 low, Player* p, double& value, double& minutes, std::string& text) const
{
    value = minutes = 0.0;
    auto const jobs = potionCrafts_.find(low);
    if (jobs == potionCrafts_.end())
        return;
    for (SupplyCraft const& job : jobs->second)
    {
        uint32 const owed = SupplyOwed(p, job);
        if (!owed)
            continue;
        value += double(owed) * job.made * job.savedPerUnit;
        minutes += double(owed) * job.castMin;
        text += Acore::StringFormat("{}{} x{} for the members who drank theirs ({:.1f} c saved a unit) [eco194]",
                                    text.empty() ? "" : "; ", ItemName(job.product), uint32(owed * job.made),
                                    job.savedPerUnit);
    }
}

void AddGuildCoordinatorScripts()
{
    new TramCarActiveScript();
    new GuildCoordinatorPlayerScript();
    new GuildCoordinatorWorldScript();
    AddGuildLedgerScripts();  // [eco190]
}
