/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildCoordinator.h"
#include "GuildHonest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <sstream>
#include <tuple>

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
#include "Group.h"
#include "Guild.h"
#include "GuildBankRun.h"
#include "GuildGatherPlan.h"  // [gather]
#include "GuildGear.h"        // [guild50]
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LootMgr.h"          // [questrate] the quest items' creature sources
#include "LootObjectStack.h"  // [questrate] chest places
#include "MapMgr.h"
#include "MotionMaster.h"
#include "NewRpgBaseAction.h"  // [guild14] TripRefused
#include "NewRpgCoop.h"        // [coop] the co-op hooks
#include "NewRpgFishing.h"     // [gather] fishing places
#include "NewRpgCraft.h"       // [guild70] crafted products, the crafting wait
#include "NewRpgInfo.h"
#include "NewRpgProfession.h"  // [guild50] BUY at a vendor
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"  // [questrate] sight / loot distance
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SelfBotControl.h"
#include "SellAction.h"
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
        return p == GuildTaskPriority::URGENT ? "срочно"
               : p == GuildTaskPriority::HELP ? "помощь"
                                              : "при случае";
    }

    char const* PlaceCode(GuildTaskPlace p)
    {
        static char const* const codes[] = {"F", "T", "M", "B", "V", "R"};
        return uint8(p) < std::size(codes) ? codes[uint8(p)] : "?";
    }

    char const* PlaceRu(GuildTaskPlace p)
    {
        static char const* const names[] = {"поле", "город", "ящик", "банк", "торговец", "тренер"};
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
            case 164: return "кузн";
            case 165: return "кожев";
            case 171: return "алх";
            case 197: return "портн";
            case 202: return "инж";
            case 333: return "чар";
            case 755: return "ювел";
            case 773: return "начерт";
            case 185: return "кулин";
            case 129: return "перев";
            case 186: return "горн";
            case 182: return "трав";
            case 393: return "шкуры";
            default: return "?";
        }
    }

    // [guild70] the crafters of a skill line, plural (the wanted list's breakdown)
    char const* SkillPlural(uint32 line)
    {
        switch (line)
        {
            case 164: return "кузнецы";
            case 165: return "кожевники";
            case 171: return "алхимики";
            case 197: return "портные";
            case 202: return "инженеры";
            case 333: return "зачарователи";
            case 755: return "ювелиры";
            case 773: return "начертатели";
            case 185: return "повара";
            case 129: return "лекари";
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

    std::string GainText(float gain)
    {
        if (gain < 0.005f)
            return "вес равный, больше брони / ур. предмета";
        return "+" + std::to_string(int32(gain * 100.0f + 0.5f)) + "%";
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
            text += std::to_string(copper / GOLD) + "з ";
        if (copper >= SILVER)
            text += std::to_string((copper % GOLD) / SILVER) + "с ";
        return text + std::to_string(copper % SILVER) + "м";
    }

    // the smallest bag size in the member's bag slots (0 = a slot is empty)
    uint32 SmallestBag(Player* p)
    {
        uint32 smallest = UINT32_MAX;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        {
            Bag* bag = p->GetBagByPos(b);
            smallest = std::min<uint32>(smallest, bag ? bag->GetBagSize() : 0);
        }
        return smallest == UINT32_MAX ? 0 : smallest;
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
                                     [this](uint32 l, uint32 q, std::string const& w) { EndCoopTask(l, q, w); }});
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
    if (QueryResult result = WorldDatabase.Query("SELECT SpellId, ReqSkillLine, ReqSkillRank, ReqLevel FROM trainer_spell"))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 spellId = f[0].Get<uint32>();
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                    if (info->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL && info->Effects[e].TriggerSpell)
                        spellId = info->Effects[e].TriggerSpell;
            // [tasks] a profession rank (Apprentice ... Grand Master): its level, the skill it needs, its new cap
            if (SpellInfo const* rank = sSpellMgr->GetSpellInfo(spellId))
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                    if (rank->Effects[e].Effect == SPELL_EFFECT_SKILL_STEP && rank->Effects[e].MiscValue > 0)
                    {
                        // Spell::EffectLearnSkill: the new max skill is the step x 75
                        uint32 const maxSkill = uint32(std::max(0, rank->Effects[e].CalcValue())) * SKILL_STEP_MAX;
                        auto& ranks = trainerRanks_[uint32(rank->Effects[e].MiscValue)];
                        auto const same = std::find_if(ranks.begin(), ranks.end(),
                                                       [&](TrainerRank const& r) { return r.maxSkill == maxSkill; });
                        uint32 const level = f[3].Get<uint8>(), req = f[2].Get<uint32>();
                        if (same == ranks.end())
                            ranks.push_back({level, req, maxSkill});
                        else if (level < same->reqLevel)
                            *same = {level, req, maxSkill};
                    }
            if (!f[1].Get<uint32>())
                continue;
            auto& req = trainerReq[spellId];
            uint32 const rank = f[2].Get<uint32>(), level = f[3].Get<uint8>();
            if (!req.first || rank < req.first)
                req = {rank, level};
        } while (result->NextRow());
    }

    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE maxcount = 0"))
    {
        do
            vendorItems_.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
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
    std::string chain = "глоб.";
    if (!row || row->grp < 'A' || row->grp > 'C')
        return chain;
    uint32 const leader = raidLeader_[row->grp - 'A'];
    if (leader == low)
        return chain + " (рейд-лидер " + row->grp + ")";
    if (leader)
        chain += std::string(" → рейд ") + row->grp + " " + Who(leader);
    auto const st = bots_.find(low);
    if (st != bots_.end() && st->second.fiveId)
    {
        auto const five = fives_.find(st->second.fiveId);
        if (five != fives_.end())
            chain += " → пятёрка " + std::to_string(five->first) +
                     (five->second.leaderLow == low ? " (лидер)" : " " + Who(five->second.leaderLow));
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

std::string GuildCoordinator::StatusName(Player* bot, PlayerbotAI* botAI) const
{
    if (!botAI)
        return "играет человек";
    if (!bot->IsAlive())
        return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? "призрак" : "мёртв";
    std::string text = RpgName(uint32(botAI->rpgInfo.GetStatus()));
    if (bot->IsInCombat())
        text = "бой/" + text;
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
        text += " q" + std::to_string(quest->questId) + (quest->quest ? " " + quest->quest->GetTitle() : "");
    if (botAI->rpgInfo.vendorRunEntry)
        text += " торговец: " + botAI->rpgInfo.vendorRunReason;
    return text;
}

// the legitimate waits a bot is in by its own state
std::string GuildCoordinator::WaitReason(Player* bot, PlayerbotAI* botAI, BotState const& state) const
{
    if (!state.waitReason.empty())
        return state.waitReason;
    if (botAI)  // [guild70] crafting at a forge / in town: standing is the work
        if (auto const* prof = std::get_if<NewRpgInfo::DoProfession>(&botAI->rpgInfo.data))
            if (prof->kind == NewRpgCraft::TASK_CRAFT && !prof->npcEntry)
                return "крафт " + NewRpgCraft::RecipeName(prof->spellId);
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
        return "waiting for boat / zeppelin";
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

    uint32 const ms = getMSTime();
    ticks_.emplace_back(ms, diff);
    aiTicks_.emplace_back(ms, uint32(std::min<uint64>(aiTimeTotalUs_.exchange(0), UINT32_MAX)));
    while (!ticks_.empty() && getMSTimeDiff(ticks_.front().first, ms) > MINUTE * IN_MILLISECONDS)
        ticks_.pop_front();
    while (!aiTicks_.empty() && getMSTimeDiff(aiTicks_.front().first, ms) > MINUTE * IN_MILLISECONDS)
        aiTicks_.pop_front();

    PMON_SCOPE(coordScope, PERF_MON_TOTAL, "guild coordinator");  // [guild15] world-thread cost, pmon
    {
        PMON_SCOPE(lootScope, PERF_MON_VALUE, "guild deaths + loot");
        ProcessDeaths();
        ProcessLoot();
    }

    wantedTimer_ += diff;
    if (wantedTimer_ >= 15 * IN_MILLISECONDS)
    {
        wantedTimer_ = 0;
        PMON_SCOPE(wantedScope, PERF_MON_VALUE, "guild wanted list");
        UpdateWanted();
        UpdateProfessions();  // [tasks] profession vs questing: the global leader's call per member
    }
    updateTimer_ += diff;
    if (updateTimer_ >= 5 * IN_MILLISECONDS)
    {
        updateTimer_ = 0;
        UpdateLeaders();
        {
            PMON_SCOPE(gearScope, PERF_MON_VALUE, "guild gear plan");
            UpdateGear();  // [guild50]
        }
        UpdateFounding();  // [guild80] the bots' own guild: plan + charter / signatures / invites / bank tab
        {
            PMON_SCOPE(transferScope, PERF_MON_VALUE, "guild transfers plan");
            UpdateTransfers();  // [guild81] trade / mail / bank between members, gold for the bank tab
        }
        {
            PMON_SCOPE(ordersScope, PERF_MON_VALUE, "guild orders");
            UpdateOrders();
        }
        UpdateHealth();
        UpdateFives();  // [guild60]
    }
    RunTransfers(diff);  // [guild81] trades next to each other, mails at mailboxes (once a second)
}

void GuildCoordinator::UpdateLeaders()
{
    std::lock_guard<std::mutex> guard(lock_);
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
            std::string const why = current ? "прежний не в сети" : "первый выбор";
            current = best;
            if (best)
                Decision(std::string("рейд ") + grp + ": лидер " + Who(best) + " (" + bestRow->role + ", L" +
                         std::to_string(bestPlayer->GetLevel()) + ") — " +
                         (bestRow->role == "tank" ? "танк с наибольшим уровнем" : "танков нет, наибольший уровень") +
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

void GuildCoordinator::ExpandReagent(uint32 item, double count, std::map<uint32, double>& out,
                                     std::map<uint32, std::map<uint32, float>>* via, float perUnit, uint32 depth,
                                     uint32 top) const
{
    if (depth < 3 && !vendorItems_.count(item) && !gatherSource_.count(item) && !skinned_.count(item))
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
                ExpandReagent(raw, count * per, out, via, perUnit * per, depth + 1, depth ? top : item);
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
    std::map<uint32, std::map<std::string, Part>> why;  // raw item -> reason -> part
    std::map<uint32, std::map<uint32, float>> via;      // raw item -> intermediate -> raw per intermediate
    std::map<uint32, double> worth;                     // [gather] raw item -> copper it is worth to the guild
    std::unordered_map<uint64, float> pointValue;
    std::unordered_map<uint32, float> copperPerXp;  // member -> the core's quest XP -> money rate (RecordPace)
    {
        std::lock_guard<std::mutex> guard(lock_);
        pointValue = pointValue_;
        for (auto const& [low, st] : bots_)
            copperPerXp[low] = st.copperPerXp;
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
            uint64 const key = (uint64(row.low) << 16) | line;
            uint32 skill = 0;
            // [gather] b337: the skill-ups were summed over the whole range to 450 for every member (Linen Cloth
            // 10423: First Aid of 75 members x 124), far past what anyone can learn now. The need runs up to the
            // skill its level reaches now: the highest trainer rank of its level (TrainerCap), or its rank's cap
            uint32 cap = 0;
            if (online)
            {
                skill = p->HasSkill(line) ? p->GetPureSkillValue(line) : 0;
                cap = std::max<uint32>(TrainerCap(line, p->GetLevel()),
                                       p->HasSkill(line) ? p->GetPureMaxSkillValue(line) : 0);
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
                for (auto const& [item, count] : r.reagents)
                    ExpandReagent(item, n * count, raw, &via);
                for (auto const& [item, amount] : raw)
                    addPart(item, amount, SkillPlural(line), WHY_SKILL, row.low, r.product);
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
                addPart(item, quest->RequiredItemCount[i], "квест q" + std::to_string(questId), WHY_QUEST, row.low,
                        0);
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
            std::lock_guard<std::mutex> guard(lock_);
            gear = gearWanted_;
        }
        for (auto const& [item, reasons] : gear)
            for (auto const& [label, g] : reasons)
            {
                std::map<uint32, double> raw;
                ExpandReagent(item, g.count, raw, &via);
                shareWorth(raw, g.value);  // [gather] the crafted item's price, this reagent's part of it
                bool const bag = label.rfind("сумка", 0) == 0;
                for (auto const& [id, amount] : raw)
                    for (uint32 forLow : g.forLows)
                        addPart(id, amount / std::max<size_t>(1, g.forLows.size()), label,
                                bag ? WHY_BAG : WHY_UPGRADE, forLow, 0);
            }
    }

    std::map<uint32, uint32> bank;
    GuildBankRun::CountBank(guild, bank);
    auto const held = [&](uint32 item)
    {
        uint32 n = bank[item];
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
                piece = "реагент: ";
                for (size_t i = 0; i < products.size() && i < 2; ++i)
                    piece += (i ? ", " : "") + ItemName(products[i].second);
                if (products.size() > 2)
                    piece += " и др.";
                size_t const crafters = std::max<size_t>(1, part->who.size());
                piece += " (" + label + " " +
                         (crafters > 1 ? std::to_string(crafters) + "×≈" + std::to_string(amount / crafters)
                                       : num(*part->who.begin()) + " ×" + std::to_string(amount)) +
                         ")";
            }
            else if (part->kind == WHY_QUEST)
                piece = label + " для " + (part->who.empty() ? "?" : num(*part->who.begin())) + " ×" +
                        std::to_string(amount);
            else if (part->kind == WHY_BAG)
                piece = label + " для " + std::to_string(part->who.size()) + " ботов ×" + std::to_string(amount);
            else
            {
                piece = label + " для";
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
                    text += (text.empty() ? "промежуточный, засчитан как " : ", ") + ItemName(raw) + " ×" + ratio;
            }
    for (auto const& [inter, text] : optional)
    {
        WantedItem w;
        w.itemId = inter;
        w.optional = true;
        w.have = held(inter);
        w.forWhom = text + " (не заказывается)";
        list.push_back(w);
    }
    std::sort(list.begin(), list.end(), [](WantedItem const& a, WantedItem const& b)
              {
                  if (a.optional != b.optional)
                      return !a.optional;
                  return a.Short() != b.Short() ? a.Short() > b.Short() : a.itemId < b.itemId;
              });

    std::lock_guard<std::mutex> guard(lock_);
    wanted_ = std::move(list);
    guildState_ = guild ? "гильдия «" + guild->GetName() + "», вкладок банка " +
                              std::to_string(guild->GetPurchasedTabsSize())
                        : "гильдии нет: банк спит";
}

bool GuildCoordinator::IsWanted(uint32 itemId) const
{
    std::lock_guard<std::mutex> guard(lock_);
    for (WantedItem const& w : wanted_)
        if (w.itemId == itemId)
            return w.Short() > 0 || w.optional;
    return false;
}

void GuildCoordinator::RequestWithdraw(Player* bot, uint32 itemId, std::string const& why)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    std::lock_guard<std::mutex> guard(lock_);
    auto& list = withdraw_[bot->GetGUID().GetCounter()];
    for (auto const& [item, text] : list)
        if (item == itemId)
            return;
    list.emplace_back(itemId, why);
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
            std::lock_guard<std::mutex> guard(lock_);
            for (WantedItem const& w : wanted_)
                if (!w.optional && w.Short() && sGuildGatherPlan.Gatherable(w.itemId))
                {
                    shortfall[w.itemId] = w.Short();
                    stock[w.itemId] = {w.need, w.have};
                    unitValue[w.itemId] = w.unitValue;
                }
            for (PlanRow const& row : plan_)
                if (row.grp == 'B' || row.grp == 'C')
                    if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                        p && p->IsInWorld() && !HumanCharacter(p))
                    {
                        gatherers.push_back(p);
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
        sGuildGatherPlan.Assign(shortfall, gatherers, questPace, unitValue, questWhy);
        if (gatherStockCalls_++ % GATHER_STOCK_LOG_EVERY == 0)  // a log cadence, no decision
            sGuildGatherPlan.LogStock(stock);
    }
    std::lock_guard<std::mutex> guard(lock_);
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
                         (GuildBankRun::BankState(p, true).empty() ? ", в банк" : ", держит до банка гильдии"));
            }
            else if (!item && p->IsNonMeleeSpellCast(false))
                keep.push_back(e);
        }
        st->second.expect = std::move(keep);
    }
    UpdateCrafts(now);  // [guild50]
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
                    withdraw_[row.low].emplace_back(g.itemId, std::string("улучшение ") +
                                                                  GuildGear::GroupName(g.group) + " " +
                                                                  GainText(g.gain));
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
            for (Item* bag : spare)
            {
                uint32 const guidLow = bag->GetGUID().GetCounter();
                bool const held = std::any_of(st.held.begin(), st.held.end(),
                                              [&](HeldItem const& h) { return h.itemGuidLow == guidLow; });
                Bag* asBag = bag->ToBag();
                if (held || bag->GetTemplate()->ContainerSlots > smallest || (asBag && !asBag->IsEmpty()) ||
                    std::find(st.sell.begin(), st.sell.end(), guidLow) != st.sell.end())
                    continue;
                st.sell.push_back(guidLow);
                Decision(Who(row.low) + ": старая сумка " + ItemName(bag->GetEntry()) + " → продать (" +
                         std::to_string(bag->GetTemplate()->ContainerSlots) + " яч., самая малая надетая " +
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

        std::vector<GuildOrder> orders;
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
        st.repairShort = 0;
        if (repairNeed.any)
        {
            GuildOrder o;
            o.type = GuildOrderType::REPAIR;
            o.repairUrgent = repairNeed.urgent;
            o.cost = repairNeed.urgent ? repairNeed.urgentCost : repairNeed.cost;
            o.pos = NewRpgBaseAction::NearestRepairSpawn(p);
            o.details = repairNeed.items + "; ремонт " + MoneyText(o.cost);
            o.reason = repairNeed.weaponBroken ? "оружие сломано: новых пулов нет до ремонта"
                       : repairNeed.urgent     ? "оружие / броня: сломается при следующей смерти или сломано"
                                               : "броня: сломается при следующей смерти";
            if (o.pos == WorldPosition())
                o.status = "нет ремонтника на этом континенте";
            else if (p->GetMoney() < o.cost)
            {
                st.repairShort = o.cost - p->GetMoney();
                o.status = "не хватает " + MoneyText(st.repairShort) + ": золото от гильдии";
            }
            else
            {
                o.executable = true;
                o.status = "к ремонтнику";
            }
            if (repairNeed.urgent)
                orders.insert(orders.begin(), o);  // [regress10] the first executable order decides (interrupts)
            else
                orders.push_back(o);
        }
        FoundingOrders(st, row.low, p, orders);  // [guild80] found / sign / join the guild, buy the bank tab
        TransferOrders(row.low, p, orders);       // [guild81] trade / mail to a member, gold for the bank tab
        if (!st.disenchant.empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::DISENCHANT;
            o.count = uint32(st.disenchant.size());
            o.details = std::to_string(o.count) + " предм.";
            o.reason = "никому не улучшение, зачарователь — сам";
            o.status = "распыляет";
            o.executable = true;
            orders.push_back(o);
        }
        std::vector<HeldItem> bankHeld;
        for (HeldItem const& h : st.held)
            if (h.purpose != "pending" && h.forLow != row.low &&
                !TransferHolds(h.itemGuidLow))  // [guild81] trade / mail moves it; [tasks] its own plan stays
                bankHeld.push_back(h);
        // [guild16] no bank order while the guild or its bank tab does not exist: the bot just holds the items
        if (!bankHeld.empty() && GuildBankRun::BankState(p, true).empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::BANK_DEPOSIT;
            o.itemId = bankHeld.front().itemId;
            o.count = uint32(bankHeld.size());
            o.forLow = bankHeld.front().forLow;
            o.details = std::to_string(o.count) + " предм.: " + ItemName(o.itemId) + " (" + bankHeld.front().purpose + ")";
            o.reason = "решение глоб. лидера по добыче";
            WorldPosition pos;
            float dist = 0;
            if (!st.vaultFail.empty())
                o.status = st.vaultFail;
            else if (!GuildBankRun::NearestVault(p, pos, dist))
                o.status = "нет хранилища на этом континенте";
            else
            {
                o.executable = true;
                o.pos = pos;  // [tasks] the vault's town
                o.status = "к хранилищу " + std::to_string(uint32(dist)) + " ярд";
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
            o.reason = "в банке лежит улучшение для него";
            WorldPosition pos;
            float dist = 0;
            if (!GuildBankRun::NearestVault(p, pos, dist))
                o.status = "нет хранилища на этом континенте";
            else
            {
                o.executable = true;
                o.pos = pos;  // [tasks] the vault's town
                o.status = "к хранилищу " + std::to_string(uint32(dist)) + " ярд";
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
            o.reason = a.why;
            // [gather] b337: every GATHER was URGENT and took 11 bots off their quests for 0.0/h chests. The leader's
            // value test decides: a gather that pays more per minute than the bot's questing runs now (URGENT,
            // ClassifyTask); else the task waits and the bot quests (what it picks on the way counts)
            o.executable = a.worth;
            o.status = a.worth ? "собирает (в сумках " + std::to_string(p->GetItemCount(a.itemId, false)) + ")"
                               : "квестует: сбор не выгоднее квестов";
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
                // GATHER runs as DO_GATHER here; TRAIN / BUY / CRAFT through the profession executor in town
                o.executable = pick.kind == GuildProfessionKind::GATHER;
                o.status = pick.kind == GuildProfessionKind::GATHER
                               ? "собирает сам"
                               : "при следующем визите в город (исполнитель профессий)";
                orders.push_back(o);
            }
        // [tasks] SELL: what the global leader gave up goes at the next vendor visit (SellMarked), never a trip
        if (!st.sell.empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::SELL;
            o.count = uint32(st.sell.size());
            o.details = std::to_string(o.count) + " предм.";
            o.reason = "никому не нужно (решение глоб. лидера)";
            o.status = "у ближайшего торговца по пути";
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
                o.details = ItemName(job->itemId) + " для " + (job->forLow == row.low ? "себя" : Who(job->forLow)) +
                            (jobs > 1 ? " (всего заказов " + std::to_string(jobs) + ")" : "");
                o.reason = job->why;
                o.executable = job->ready;
                o.status = job->ready ? "материалы есть: создаёт" : "ждёт материалы (wanted list)";
                orders.push_back(o);
            }
            // BUY: the biggest upgrade it can pay for at a vendor
            for (GearPick const& g : st.gearNow)
            {
                if (g.kind != GuildGear::SRC_VENDOR || p->GetItemCount(g.itemId, true) || p->GetMoney() < g.aux ||
                    (g.extendedCost && !GuildGear::ExtendedCostMissing(p, g.extendedCost).empty()))
                    continue;
                GuildOrder o;
                o.type = GuildOrderType::BUY;
                o.itemId = g.itemId;
                o.count = 1;
                o.npcEntry = g.srcId;
                o.pos = g.pos;
                o.cost = g.aux;
                o.extendedCost = g.extendedCost;
                o.details = ItemName(g.itemId) + " — " + g.text;
                o.reason = std::string("улучшение ") + GuildGear::GroupName(g.group) + " " + GainText(g.gain) +
                           ", на свои деньги";
                o.status = "к торговцу";
                o.executable = true;
                orders.push_back(o);
                break;
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
                            (st.rewards.size() > 1 ? " (+" + std::to_string(st.rewards.size() - 1) + " кв.)" : "");
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
                o.details = GuildGear::MapName(bestMap) + ": " + std::to_string(o.count) + " улучш. (" +
                            byDungeon[bestMap].second + ")";
                o.reason = "лучшие доступные вещи слотов — добыча этого данжа";
                o.status = "план: пятёрку соберёт глоб. лидер";
                orders.push_back(o);
            }
        }
        for (GuildOrder const& ext : st.external)
            orders.push_back(ext);
        GuildOrder quest;
        quest.details = "сам себе лидер";
        quest.reason = "нет более полезного дела";
        orders.push_back(quest);

        // [tasks] urgency, place, origin and town of every task; a convenient town task waits for that town
        uint32 const freeSlots = FreeBagSlots(p);
        for (GuildOrder& o : orders)
        {
            ClassifyTask(o);
            if (o.type == GuildOrderType::BANK_DEPOSIT && !freeSlots)
            {
                o.priority = GuildTaskPriority::URGENT;  // bags full: the held items block its looting
                o.reason += "; сумки полны";
            }
            o.waitsTown = o.executable && !TownReady(p, o);
            if (o.waitsTown)
                o.status = "ждёт города: " + (o.townName.empty() ? std::string("?") : o.townName) + "; " + o.status;
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
                o.status = "совет человеку (решает сам)";
                if (wasRun)
                    Suggest(row.low, TaskKey(o),
                            std::string("совет: ") + OrderName(o.type) + " — " + o.details + " (" + o.reason + ")");
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
            bool const noBank = !GuildBankRun::BankState(p, true).empty();
            uint32 heldBank = 0;
            for (HeldItem const& h : st.held)
                if (!h.forLow && h.purpose != "pending")
                    ++heldBank;
            if (cls != st.bagClass)
            {
                st.bagClass = cls;
                LOG_INFO("playerbots", "[tasks] {} bags free={}/{} pressure={} held_for_bank={} sell_marked={} bank={}",
                         Who(row.low), freeSlots, total, cls == 0 ? "full" : (cls == 1 ? "low" : "ok"), heldBank,
                         st.sell.size(), noBank ? "no tab" : "yes");
            }
            // full bags and no bank: a held item no member's craft or quest wants (IsWanted) goes to the vendor
            // (sold, never destroyed); a wanted one stays and the transfers move it to the member who lacks it
            if (cls == 0 && noBank && !HumanCharacter(p))
                for (auto it = st.held.begin(); it != st.held.end();)
                {
                    if (it->forLow || it->purpose == "pending" || TransferHolds(it->itemGuidLow) ||
                        std::any_of(wanted_.begin(), wanted_.end(), [&](WantedItem const& w)
                                    { return w.itemId == it->itemId && !w.optional; }) || PlannedFor(it->itemId))
                    {
                        ++it;
                        continue;
                    }
                    LOG_INFO("playerbots", "[tasks] {} bags full, no bank tab: {} released to the vendor (nobody "
                             "wants it)", Who(row.low), ItemName(it->itemId));
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
                    if (o.type == GuildOrderType::GATHER && old.status.rfind("нет ", 0) == 0)
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
                (o.type == GuildOrderType::BANK_WITHDRAW && o.details.find("улучшение") != std::string::npos);
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
    std::lock_guard<std::mutex> guard(lock_);
    BotState& st = bots_[low];
    order.since = Now();
    if (order.status.empty())
        order.status = "этап 2: пока не выполняется";
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
                OrdersText(o.details, 60);
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
bool GuildCoordinator::QuestPace(BotState& st, Player* p, float& copperPerMinute, std::string* why) const
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
    auto const worth = [&](Quest const* quest)
    {
        double const xp = double(quest->XPValue(level)) * rate;
        double perXp = st.copperPerXp;
        if (perXp <= 0.0 && xp > 0.0)  // nothing in its log yet: this quest's own max-level money for its XP
            perXp = double(quest->GetRewMoneyMaxLevel()) / xp;
        double copper = xp * perXp + double(std::max<int32>(0, quest->GetRewOrReqMoney(level)));
        for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
            if (ItemTemplate const* proto =
                    quest->RewardItemId[i] ? sObjectMgr->GetItemTemplate(quest->RewardItemId[i]) : nullptr)
                copper += double(proto->SellPrice) * quest->RewardItemIdCount[i];
        double choice = 0.0;
        for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
            if (ItemTemplate const* proto =
                    quest->RewardChoiceItemId[i] ? sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]) : nullptr)
                choice = std::max(choice, double(proto->SellPrice) * quest->RewardChoiceItemCount[i]);
        return copper + choice;
    };
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
    double logCopper = 0.0, logSeconds = 0.0;
    uint32 logQuests = 0, inLog = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = p->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        ++inLog;
        QuestStatus const status = p->GetQuestStatus(questId);
        auto const q = questStatic_.find(questId);
        if (q == questStatic_.end() || q->second.map != map || !q->second.ender ||
            (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE))
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
        logSeconds += walk / speed + kills;
        ++logQuests;
    }
    if (logQuests)
    {
        copperPerMinute = float(logCopper * MINUTE / std::max(logSeconds, 1.0));  // 1 s: a hand-in where it stands
        if (why)
            *why = Acore::StringFormat("формула: {} кв. в журнале, {:.0f} м за {:.0f} с", logQuests, logCopper,
                                       logSeconds);
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
                if (p->GetQuestStatus(questId) == QUEST_STATUS_NONE && p->CanTakeQuest(quest, false))
                    st.questAvail.push_back(questId);
            }
    }
    float best = -1.0f;
    uint32 bestQuest = 0;
    double bestCopper = 0.0, bestSeconds = 0.0;
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
        float const perMinute = float(copper * MINUTE / std::max(seconds, 1.0));
        if (perMinute > best)
        {
            best = perMinute;
            bestQuest = questId;
            bestCopper = copper;
            bestSeconds = seconds;
        }
    }
    if (!bestQuest)
        return false;
    copperPerMinute = best;
    if (why)
        *why = Acore::StringFormat("формула: лучший доступный q{}, {:.0f} м за {:.0f} с", bestQuest, bestCopper,
                                   bestSeconds);
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

    std::lock_guard<std::mutex> guard(lock_);
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
    };
    std::vector<Need> needs;
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
                needs.push_back({recipe->skillLine, recipe->reqSkill, recipe->spellId, g.itemId, 1, row->low,
                                 proto->Bonding == BIND_WHEN_PICKED_UP,
                                 std::string("улучшение ") + GuildGear::GroupName(g.group) + " для " + Who(row->low)});
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
        GuildGear::CraftRecipe const* next = nullptr;
        uint32 nextSlots = 0;
        for (uint32 spell : GuildGear::BagRecipes())
        {
            GuildGear::CraftRecipe const* r = GuildGear::Recipe(spell);
            ItemTemplate const* proto = r ? sObjectMgr->GetItemTemplate(r->itemId) : nullptr;
            if (!proto || proto->ContainerSlots <= known || !learnable(r->skillLine, r->spellId))
                continue;
            if (!next || proto->ContainerSlots < nextSlots ||
                (proto->ContainerSlots == nextSlots && r->reqSkill < next->reqSkill))
            {
                next = r;
                nextSlots = proto->ContainerSlots;
            }
        }
        if (next)
        {
            uint32 count = 0;
            for (auto const& [row, m] : members)
                for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
                {
                    Bag* bag = m->GetBagByPos(b);
                    count += (!bag || bag->GetBagSize() < nextSlots) ? 1 : 0;
                }
            if (count)
                needs.push_back({next->skillLine, next->reqSkill, next->spellId, next->itemId, count, 0, false,
                                 "сумка " + std::to_string(nextSlots) + " яч. ×" + std::to_string(count)});
        }
    }
    // each need goes to the member of that profession closest to it (the highest skill; BoP: the member itself)
    std::map<std::pair<uint32, uint32>, std::vector<Need const*>> assigned;  // (crafter, line) -> needs
    for (Need const& n : needs)
    {
        uint32 best = 0, bestSkill = 0;
        for (auto const& [row, m] : members)
        {
            if (HumanCharacter(m) || (n.bop && row->low != n.forLow) || !plansLine(*row, m, n.line))
                continue;
            uint32 const skill = m->HasSkill(n.line) ? m->GetPureSkillValue(n.line) : 0;
            if (!best || skill > bestSkill)
            {
                best = row->low;
                bestSkill = skill;
            }
        }
        if (best)
            assigned[{best, n.line}].push_back(&n);
    }

    for (auto const& [row, p] : members)
    {
        if (HumanCharacter(p))
        {
            professions_.erase(row->low);
            continue;
        }
        BotState& st = bots_[row->low];
        uint8 const level = p->GetLevel();
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
                                     std::to_string(maxSkill) + ", нужно " + std::to_string(need) +
                                     " (ранг тренера на L" + std::to_string(level) + ": " + std::to_string(cap) +
                                     (needPlan ? ", план: " + std::to_string(needPlan) : "") + ")";
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
                        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(n->itemId))
                            unlocked += double(proto->BuyPrice) * n->count;
                uint64 const key = (uint64(row->low) << 16) | line;
                if (points)
                    pointValue_[key] = float((sales + unlocked) / points);
                else
                    pointValue_.erase(key);
            }
            std::string key;
            std::string decision;
            auto const train = [&](std::string details, std::string reason)
            {
                ProfessionPick pick;
                pick.kind = GuildProfessionKind::TRAIN;
                pick.skillLine = line;
                pick.details = std::move(details);
                pick.reason = std::move(reason);
                picks.push_back(pick);
            };
            if (!has)
            {
                train(std::string("выучить ") + SkillShort(line),
                      "в плане, не выучена; ранг тренера на L" + std::to_string(level) + " до " + std::to_string(cap));
                key = "learn";
                decision = head + " — не выучена: TRAIN у тренера при следующем визите в город";
            }
            else if (skill >= need)
            {
                key = "ok";
                decision = head + " — отставания нет: квестует";
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
                    train("ранг до " + std::to_string(rank->maxSkill),
                          "навык " + std::to_string(skill) + " ≥ " + std::to_string(rank->reqSkill) + ", уровень ≥ " +
                              std::to_string(rank->reqLevel));
                // a planned recipe its skill already allows: learned at the trainer
                for (Need const* n : lineNeeds)
                    if (n->reqSkill <= skill && !p->HasSpell(n->spellId))
                    {
                        train("рецепт " + ItemName(n->itemId), n->label);
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
                double craftMin = 0, gatherMin = 0, vendorCost = 0, sales = 0, gatherUnits = 0, buyUnits = 0;
                uint32 gatherItem = 0, buyItem = 0;
                bool held = false;
                std::string block;
                uint32 reach = skill;
                uint32 const top = std::min(need, maxSkill);
                for (uint32 v = std::max<uint32>(1, skill); v < top; ++v)
                {
                    if (list == recipesBySkill_.end() || skillPlan == skillPlan_.end() ||
                        v >= skillPlan->second.size() || skillPlan->second[v] < 0)
                    {
                        block = "нет рецепта прокачки у тренера на " + std::to_string(v);
                        break;
                    }
                    Recipe const& r = list->second[size_t(skillPlan->second[v])];
                    int32 const chance = CraftChance(v, r.gray, r.yellow);
                    if (r.reqLevel > level || chance <= 0)
                    {
                        block = NewRpgCraft::RecipeName(r.spellId) + " с уровня " + std::to_string(r.reqLevel);
                        break;
                    }
                    double const crafts = 1000.0 / chance;
                    std::map<uint32, double> take;
                    double cost = 0, gmin = 0;
                    std::string why;
                    for (auto const& [item, n] : r.reagents)
                    {
                        double want = crafts * n;
                        double const fromPool = std::clamp(avail(item) - take[item], 0.0, want);
                        take[item] += fromPool;
                        want -= fromPool;
                        held |= fromPool > 0;
                        if (want <= 0)
                            continue;
                        std::map<uint32, double> raw;
                        ExpandReagent(item, want, raw, nullptr);
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
                            if (vendorItems_.count(rawItem))
                            {
                                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rawItem);
                                cost += units *
                                        (proto ? double(proto->BuyPrice) / std::max<uint32>(1, proto->BuyCount) : 0.0);
                                buyItem = rawItem;
                                buyUnits += units;
                                continue;
                            }
                            auto const src = gatherSource_.find(rawItem);
                            if (src != gatherSource_.end() && p->HasSkill(src->second.first) &&
                                p->GetPureSkillValue(src->second.first) >= src->second.second)
                            {
                                float const pace = GatherPace(src->second.first);
                                if (pace > 0.0f)
                                {
                                    gmin += units / pace;
                                    if (units > gatherUnits)
                                    {
                                        gatherItem = rawItem;
                                        gatherUnits = units;
                                    }
                                    continue;
                                }
                                why = ItemName(rawItem) + ": нет замеров темпа сбора (" +
                                      SkillShort(src->second.first) + ")";
                                break;
                            }
                            uint32 others = 0;
                            for (auto const& [otherRow, other] : members)
                                if (other != p)
                                    others += other->GetItemCount(rawItem, false);
                            why = ItemName(rawItem) + " ×" + std::to_string(uint32(std::ceil(units))) + ": " +
                                  (others ? "у членов гильдии " + std::to_string(others) + " — ждёт передачи"
                                          : std::string("собирают другие (wanted list)"));
                            break;
                        }
                        if (!why.empty())
                            break;
                    }
                    if (!why.empty())
                    {
                        block = why;
                        break;
                    }
                    for (auto const& [item, n] : take)
                        pool[item] -= n;
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(r.spellId))
                        craftMin += crafts * info->CalcCastTime() / double(MINUTE * IN_MILLISECONDS);
                    if (ItemTemplate const* product = r.product ? sObjectMgr->GetItemTemplate(r.product) : nullptr)
                        sales += crafts * r.made * product->SellPrice;
                    vendorCost += cost;
                    gatherMin += gmin;
                    reach = v + 1;
                }
                std::string const lag = ", отставание " + std::to_string(need - skill);
                if (reach <= skill)
                {
                    key = std::string("blocked:") + (skill >= maxSkill ? "cap" : block.substr(0, 24));
                    decision = head + lag + " — прокачать сейчас нельзя: " +
                               (skill >= maxSkill ? "потолок ранга " + std::to_string(maxSkill) +
                                                        (rank ? ", ранг у тренера" : ", ранг ждёт уровня")
                                                  : block);
                }
                else
                {
                    double unlocked = 0;
                    std::string unlockedText;
                    for (Need const* n : lineNeeds)
                        if (n->reqSkill > skill && n->reqSkill <= reach)
                        {
                            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(n->itemId);
                            unlocked += double(proto ? proto->BuyPrice : 0) * n->count;
                            if (unlockedText.size() < 80)
                                unlockedText +=
                                    (unlockedText.empty() ? "" : ", ") + ItemName(n->itemId) + " (" + n->label + ")";
                        }
                    double const value = sales + unlocked - vendorCost;
                    double const minutes = craftMin + gatherMin;
                    float questPace = 0.0f;
                    std::string questWhy;
                    char numbers[200];
                    std::snprintf(numbers, sizeof(numbers), "до %u за %.1f мин (крафт %.1f, сбор %.1f), ценность %s",
                                  reach, minutes, craftMin, gatherMin, Gsc(uint64(std::max(0.0, value))).c_str());
                    std::string const reached = std::string(numbers) +
                                                (unlockedText.empty() ? "" : "; открывает " + unlockedText) +
                                                (block.empty() ? "" : "; дальше: " + block);
                    if (!QuestPace(st, p, questPace, &questWhy))
                    {
                        key = "noquestpace";
                        decision = head + lag + " — квесты без цены (нет квеста с данными на карте): квестует (" +
                                   reached + ")";
                    }
                    else if (value > 0 && value > double(questPace) * minutes)
                    {
                        double const perMinute = minutes > 0 ? value / minutes : value;
                        char pace[120];
                        std::snprintf(pace, sizeof(pace), "профессия %.0f м/мин > квесты %.0f м/мин", perMinute,
                                      questPace);
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
                        if (held || (!gatherItem && !buyItem))
                        {
                            add(GuildProfessionKind::CRAFT, 0, 0,
                                std::string(SkillShort(line)) + " " + std::to_string(skill) + "→" +
                                    std::to_string(reach) + ": крафт из того, что есть");
                            kinds += "craft";
                        }
                        if (buyItem)
                        {
                            add(GuildProfessionKind::BUY, buyItem, buyUnits,
                                ItemName(buyItem) + " ×" + std::to_string(uint32(std::ceil(buyUnits))) + " для " +
                                    SkillShort(line));
                            kinds += kinds.empty() ? "buy" : "+buy";
                        }
                        if (gatherItem)
                        {
                            add(GuildProfessionKind::GATHER, gatherItem, gatherUnits,
                                ItemName(gatherItem) + " ×" + std::to_string(uint32(std::ceil(gatherUnits))) + " для " +
                                    SkillShort(line));
                            kinds += kinds.empty() ? "gather" : "+gather";
                        }
                        key = "profession:" + kinds;
                        decision = head + lag + " — " + pace + " (" + questWhy + ") → PROFESSION " + kinds + " (" +
                                   reached + ")";
                    }
                    else
                    {
                        char pace[120];
                        std::snprintf(pace, sizeof(pace), "квесты %.0f м/мин ≥ профессия %.0f м/мин", questPace,
                                      minutes > 0 ? value / minutes : value);
                        key = "quest";
                        decision = head + lag + " — " + pace + " (" + questWhy + "): квестует (" + reached + ")";
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
                std::string const name = line == SKILL_HERBALISM ? "трав"
                                         : line == SKILL_MINING  ? "горн"
                                         : line == SKILL_SKINNING ? "шкуры"
                                                                  : "рыбалка";
                std::string const head = Who(row->low) + ": " + name + " " + std::to_string(skill) + "/" +
                                         std::to_string(maxSkill) + " (ранг тренера на L" + std::to_string(level) +
                                         ": " + std::to_string(cap) + ")";
                std::string key;
                std::string decision;
                auto const call = [&](std::string details, std::string reason)
                {
                    ProfessionPick pick;
                    pick.kind = GuildProfessionKind::TRAIN;
                    pick.skillLine = line;
                    pick.details = std::move(details);
                    pick.reason = std::move(reason);
                    picks.push_back(pick);
                };
                if (!has && cap)
                {
                    call(std::string("выучить ") + name, "в плане сборщика, тренер учит на L" + std::to_string(level));
                    key = "learn";
                    decision = head + " — не выучена: TRAIN у тренера";
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
                        call("ранг до " + std::to_string(rank->maxSkill),
                             "навык " + std::to_string(skill) + " на потолке ранга, уровень ≥ " +
                                 std::to_string(rank->reqLevel));
                        key = "rank";
                        decision = head + " — потолок ранга: TRAIN ранг до " + std::to_string(rank->maxSkill);
                    }
                }
                else if (has && line == SKILL_FISHING && !NewRpgFishing::HasPole(p))
                {
                    call("удочка (Fishing Pole)", "рыбалка выучена, удочки нет");
                    key = "tool";
                    decision = head + " — нет удочки: купить у торговца";
                }
                if (key.empty())
                {
                    key = "ok";
                    decision = head + " — выучена, звонка нет";
                }
                if (st.profDecision[line] != key)
                {
                    st.profDecision[line] = key;
                    Decision(decision);
                }
            }
        std::stable_sort(picks.begin(), picks.end(), [](ProfessionPick const& a, ProfessionPick const& b)
                         { return uint8(a.kind) > uint8(b.kind); });  // craft, buy, gather, then the trainer
        professions_[row->low] = std::move(picks);
    }
}

float GuildCoordinator::PlannedGain(uint32 low, uint8 group) const
{
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(low);
    if (itr == bots_.end())
        return 0.0f;
    float best = 0.0f;
    for (GearPick const& g : itr->second.gearNow)
        if (g.group == group)
            best = std::max(best, g.gain);
    return best;
}

GuildProfessionKind GuildCoordinator::ProfessionCall(Player* bot, uint32* skillLine)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return GuildProfessionKind::NONE;
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = professions_.find(bot->GetGUID().GetCounter());
    if (itr == professions_.end() || itr->second.empty())
        return GuildProfessionKind::NONE;
    if (skillLine)
        *skillLine = itr->second.front().skillLine;
    return itr->second.front().kind;
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
    std::lock_guard<std::mutex> guard(lock_);
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
        return said("пятёрка #" + std::to_string(st.fiveId));
    for (GuildOrder const& o : st.orders)
    {
        if (!o.executable)
            continue;
        if (st.run && o.type == st.runType && o.priority == GuildTaskPriority::URGENT)
            return said(std::string(OrderName(o.type)) + " (идёт к хранилищу)");
        if (o.priority == GuildTaskPriority::URGENT && !o.waitsTown)
            return said(std::string(OrderName(o.type)) + ": " + o.details);
        break;  // the first executable order decides
    }
    return false;
}

void GuildCoordinator::TownStops(Player* bot, std::vector<std::pair<WorldPosition, std::string>>& out) const
{
    out.clear();
    if (!bot)
        return;
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end() || itr->second.fiveId)
        return;
    for (GuildOrder const& o : itr->second.orders)
        if (o.executable && o.waitsTown && o.priority == GuildTaskPriority::CONVENIENT &&
            o.pos != WorldPosition() && o.pos.GetMapId() == bot->GetMapId())
            out.emplace_back(o.pos, std::string(OrderName(o.type)) + (o.townName.empty() ? "" : " " + o.townName));
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
    o.details = std::string(leads ? "ведёт " : "помогает ") + Who(partnerLow) + ": q" +
                std::to_string(questId) + (quest ? " " + quest->GetTitle() : "");
    o.reason = why;
    o.status = "пати co-op (New RPG)";
    o.executable = false;  // New RPG co-op runs it, never NextStep
    std::lock_guard<std::mutex> guard(lock_);
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
    std::lock_guard<std::mutex> guard(lock_);
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
        LOG_INFO("playerbots", "[tasks] {} ends COOP q{}: {}", Who(low), questId, why);
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
        ChatHandler(p->GetSession()).SendSysMessage("[Гильдия] " + text);
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
    // one member per call, in plan order: the score work of 50 members spread over minutes, not one tick
    Player* member = nullptr;
    for (size_t n = 0; n < plan_.size() && !member; ++n)
    {
        gearCursor_ = (gearCursor_ + 1) % plan_.size();
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(plan_[gearCursor_].low));
        if (p && p->IsInWorld() && !HumanControls(p))
            member = p;
    }
    if (!member)
        return;

    std::map<uint32, uint32> bank;
    GuildBankRun::CountBank(guild, bank);
    std::vector<WantedItem> wanted;
    {
        std::lock_guard<std::mutex> guard(lock_);
        wanted = wanted_;
    }
    std::vector<GearPick> now, plan;
    std::map<uint32, RewardPick> rewards;
    PlanGear(member, now, plan, rewards, bots, bank, wanted);

    std::ostringstream log;
    for (GearPick const& g : now)
        log << " [" << GuildGear::GroupName(g.group) << "] " << ItemName(g.itemId) << " " << GainText(g.gain) << " — "
            << g.text << ";";
    LOG_INFO("playerbots", "[guild50] gear plan {} L{}: now {}, plan {}, rewards {};{}", member->GetName(),
             member->GetLevel(), now.size(), plan.size(), rewards.size(), log.str());

    std::lock_guard<std::mutex> guard(lock_);
    BotState& st = bots_[member->GetGUID().GetCounter()];
    st.gearNow = std::move(now);
    st.gearPlan = std::move(plan);
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
}

void GuildCoordinator::PlanGear(Player* m, std::vector<GearPick>& now, std::vector<GearPick>& plan,
                                std::map<uint32, RewardPick>& rewards, std::vector<Player*> const& bots,
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

    std::map<uint8, GearPick> bestNow, bestPlan;
    bool aheadItem = false;  // the member still lacks the level / reputation to wear it: a plan, not an order
    std::string lockText;
    auto const consider = [&](GearPick const& pick, bool isNow)
    {
        if (isNow && aheadItem)
            return;
        GearPick use = pick;
        if (aheadItem)
            use.text += " — " + lockText;
        auto& best = isNow ? bestNow : bestPlan;
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
                    return ItemName(cost->reqitem[r]) + ": квест q" + std::to_string(s.id);
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
        auto const inBank = bank.find(proto->ItemId);
        if (inBank != bank.end() && inBank->second)
        {
            GearPick p = base;
            p.kind = GuildGear::SRC_BANK;
            p.text = "в банке гильдии";
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
                        p.text = "награда " + title + " (в журнале)";
                        consider(p, true);
                        consider(p, false);
                    }
                    else if (m->CanTakeQuest(quest, false))
                    {
                        p.text = "квест " + title + " L" + std::to_string(src.level);
                        consider(p, false);
                    }
                    else
                    {
                        // a chain step (prerequisites, exclusive group) or a level still ahead
                        p.text = "квест " + title + " L" + std::to_string(src.level) + " — " +
                                 (m->SatisfyQuestLevel(quest, false) ? std::string("после цепочки")
                                                                     : "с L" + std::to_string(src.levelMax));
                        consider(p, false);
                    }
                    break;
                }
                case GuildGear::SRC_DROP:
                {
                    if (src.level > ahead)
                        break;  // the instance's entry level (dungeon_access_template) is still far ahead
                    MapEntry const* map = sMapStore.LookupEntry(src.mapId);
                    p.text = std::string(map && map->IsRaid() ? "рейд " : "данж ") + GuildGear::MapName(src.mapId) +
                             (src.difficulty ? " (сложн. " + std::to_string(src.difficulty) + ")" : std::string()) +
                             ": " + creatureName(src.id) + ", " + chanceText(src.chance) +
                             (src.level > level ? ", вход с L" + std::to_string(src.level) : std::string());
                    consider(p, false);
                    break;
                }
                case GuildGear::SRC_WORLD:
                    p.text = "моб " + creatureName(src.id) + " " + levelText(src.level, src.levelMax) + " (" +
                             GuildGear::MapName(src.mapId) + "), " + chanceText(src.chance);
                    consider(p, false);
                    break;
                case GuildGear::SRC_REFDROP:
                {
                    GuildGear::RefInfo const* ref = GuildGear::Reference(src.id);
                    if (!ref || ref->minLevel > ahead)
                        break;
                    p.text = "мировой дроп " + levelText(ref->minLevel, ref->maxLevel) + " (" +
                             std::to_string(ref->users) + " источн., " + GuildGear::MapName(ref->mapId) + "), " +
                             chanceText(src.chance);
                    consider(p, false);
                    break;
                }
                case GuildGear::SRC_OBJECT:
                {
                    GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(src.id);
                    p.text = std::string("объект ") + (go ? go->name : std::string("?")) + " (" +
                             GuildGear::MapName(src.mapId) + "), " + chanceText(src.chance);
                    consider(p, false);
                    break;
                }
                case GuildGear::SRC_CONTAINER:
                    p.text = "из " + ItemName(src.id) + ", " + chanceText(src.chance);
                    consider(p, false);
                    break;
                case GuildGear::SRC_OTHER:
                {
                    std::string what;
                    if (src.aux == GuildGear::OTHER_PICKPOCKET)
                        what = "кража у " + creatureName(src.id);
                    else if (src.aux == GuildGear::OTHER_SKINNING)
                        what = "шкура с " + creatureName(src.id);
                    else if (src.aux == GuildGear::OTHER_FISHING)
                        what = "рыбалка, зона " + std::to_string(src.id);
                    else
                        what = "почта " + std::to_string(src.id);
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
                        p.text = std::string("крафт (") + SkillShort(recipe->skillLine) + " " +
                                 std::to_string(recipe->reqSkill) + "): рецепта нет ни у кого в гильдии";
                        consider(p, false);
                        break;
                    }
                    p.crafterLow = crafter->GetGUID().GetCounter();
                    p.text = "крафт " + Who(p.crafterLow) + " (" + SkillShort(recipe->skillLine) + ")" +
                             (ready ? "" : ", материалы в wanted list");
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
                    for (GuildGear::VendorSpawn const& s : spawns ? *spawns : none)
                    {
                        if (!FriendlyNpc(m, s.entry) || NewRpgProfession::BuyFailed(m, s.entry, proto->ItemId))
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
                        p.text = "торговец " + creatureName(elsewhere->entry) + " (" +
                                 GuildGear::MapName(elsewhere->mapId) + ")" +
                                 (elsewhere->extendedCost ? ", за " + GuildGear::ExtendedCostText(elsewhere->extendedCost)
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
                    p.text = "торговец " + creatureName(nearest->entry) + " " + std::to_string(uint32(nearDist)) +
                             " ярд, " + price +
                             (money >= uint32(proto->BuyPrice) ? std::string()
                                                               : " (копит: свободно " + MoneyText(money) + ")") +
                             (missing.empty() ? std::string() : " (не хватает: " + missing + ")");
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
    for (auto* best : {&bestNow, &bestPlan})
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
    std::sort(now.begin(), now.end(), byGain);
    std::sort(plan.begin(), plan.end(), byGain);

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
                pick.why = std::string("себе: ") + GuildGear::GroupName(GuildGear::EquipGroup(proto->InventoryType)) +
                           " " + GainText(ev.gain);
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
                        pick.why = "BoE для " + Who(pick.forLow) + ": " +
                                   GuildGear::GroupName(GuildGear::EquipGroup(proto->InventoryType)) + " " +
                                   GainText(ev.gain) + ", через банк";
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
                        pick.why = "в wanted list (" + w.forWhom + ")";
                        break;
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
                        pick.why = "никому не улучшение: дороже продать (" + MoneyText(price) + ")";
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

    auto const addJob = [&](uint32 crafterLow, uint32 spellId, uint32 forLow, bool bag, std::string why)
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
                       std::string("улучшение ") + GuildGear::GroupName(g.group) + " " + GainText(g.gain));
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
                need += (!bag || bag->GetBagSize() < bagSlots) ? 1 : 0;
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
                       "сумка " + std::to_string(bagSlots) + " яч. вместо " + std::to_string(SmallestBag(m)));
            total += need;
        }
        bagState_ = "сумки: лучшая " + ItemName(bagItem) + " (" + std::to_string(bagSlots) + " яч.) у " +
                    Who(tailor->GetGUID().GetCounter()) + ", нужно " + std::to_string(total);
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
            GearNeed& g = gearWanted_[item][(job.bag ? "сумка " : "улучшение: ") + ItemName(job.itemId)];
            g.count += n;
            g.forLows.insert(job.forLow);
            // [gather] the crafted item's price (the leader values unlocked upgrades by it), shared by reagent units
            uint32 units = 0;
            for (auto const& [reagent, count] : recipe->reagents)
                units += count;
            if (ItemTemplate const* product = sObjectMgr->GetItemTemplate(job.itemId); product && units)
                g.value += double(product->BuyPrice) * n / units;
        }
    }
}

int32 GuildCoordinator::RewardChoice(Player* bot, Quest const* quest)
{
    if (!bot || !quest || !IsPlanned(bot->GetGUID().GetCounter()) || quest->GetRewChoiceItemsCount() <= 1)
        return -1;
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return -1;
    auto const pick = itr->second.rewards.find(quest->GetQuestId());
    if (pick == itr->second.rewards.end() || pick->second.index >= quest->GetRewChoiceItemsCount() ||
        quest->RewardChoiceItemId[pick->second.index] != pick->second.itemId)
        return -1;
    if (pick->second.forLow && pick->second.forLow != bot->GetGUID().GetCounter())
        itr->second.expect.push_back({pick->second.itemId, pick->second.forLow, "награда для " + Who(pick->second.forLow)});
    LOG_INFO("playerbots", "[guild50] {} takes reward {} of q{}: {}", bot->GetName(), ItemName(pick->second.itemId),
             quest->GetQuestId(), pick->second.why);
    return int32(pick->second.index);
}

// ---------------------------------------------------------------------------------------------------------------------
// loot flow
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::OnLoot(Player* player, Item* item, uint32 count)
{
    if (!player || !item || !IsPlanned(player->GetGUID().GetCounter()))
        return;
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
    std::lock_guard<std::mutex> guard(lock_);
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
        std::lock_guard<std::mutex> guard(lock_);
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
                             " (у " + Who(looterLow) + ")";
    bool const equipment = proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON;

    // 1. BoE: whom of the members it upgrades most (ItemUsage of the bot; score gain vs its slot)
    uint32 bestLow = 0;
    float bestGain = 0.0f;
    std::string bestUsage;
    if (equipment)
    {
        std::string const qualifier =
            std::to_string(report.itemId) +
            (item->GetItemRandomPropertyId() ? "," + std::to_string(item->GetItemRandomPropertyId()) : "");
        for (PlanRow const& row : plan_)
        {
            Player* m = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
            if (!m || !m->IsInWorld() || m->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(m);
            if (!ai)
                continue;  // a human picks his own gear
            ItemUsage const usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", qualifier)->Get();
            if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
                continue;
            StatsWeightCalculator calc(m);
            float const score = calc.CalculateItem(report.itemId, item->GetItemRandomPropertyId());
            float current = 0.0f;
            uint8 const slot = m->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot != NULL_SLOT)
                if (Item* worn = m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    calc.Reset();
                    current = calc.CalculateItem(worn->GetEntry(), worn->GetItemRandomPropertyId());
                }
            float const gain = current > 0.0f ? (score - current) / current : 1.0f;
            if (gain > bestGain)
            {
                bestGain = gain;
                bestLow = row.low;
                bestUsage = usage == ITEM_USAGE_EQUIP ? "пустой слот" : "замена";
            }
        }
    }

    std::lock_guard<std::mutex> guard(lock_);
    BotState& st = bots_[looterLow];
    auto held = std::find_if(st.held.begin(), st.held.end(),
                             [&](HeldItem const& h) { return h.itemGuidLow == report.itemGuidLow; });
    auto const release = [&]()
    {
        if (held != st.held.end())
            st.held.erase(held);
    };
    std::string const gainText = "+" + std::to_string(int32(bestGain * 100.0f + 0.5f)) + "% к слоту";

    if (bestLow == looterLow)
    {
        release();
        Decision("BoE " + what + " → надевает сам: " + gainText + " (" + bestUsage + ")");
        return;
    }
    if (bestLow)
    {
        if (held != st.held.end())
        {
            held->forLow = bestLow;
            held->purpose = "улучшение " + Who(bestLow);
        }
        Decision("BoE " + what + " → " + Who(bestLow) + ": " + gainText + " (" + bestUsage +
                 "), через банк гильдии");
        return;
    }

    // 2. wanted-list material: to the guild bank (held until it exists)
    for (WantedItem const& w : wanted_)
        if (w.itemId == report.itemId && (w.need || w.optional))
        {
            if (held != st.held.end())
                held->purpose = "wanted";
            Decision(what + (GuildBankRun::BankState(looter, true).empty() ? " → банк" : " → держит до банка гильдии") +
                     ": в wanted list" + (w.optional ? " (необязательный)" : "") + " (" +
                     std::to_string(w.have) + "/" + std::to_string(w.need) + ": " + OrdersText(w.forWhom, 90) + ")");
            return;
        }

    // the looter uses it itself (its profession, a spell, a quest): no decision needed
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(looter))
    {
        ItemUsage const usage =
            ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", std::to_string(report.itemId))->Get();
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

    // [tasks] a member's gear plan holds it (a later level, an offline member): kept for that member, never
    // disenchanted or sold
    if (uint32 const planned = equipment ? PlannedFor(report.itemId) : 0)
    {
        if (planned == looterLow)
        {
            if (held != st.held.end())
            {
                held->forLow = looterLow;  // kept in its bags (no bank run, no sale)
                held->purpose = "свой план";
            }
            Decision("BoE " + what + " → держит для себя: в его плане экипировки");
            return;
        }
        if (held != st.held.end())
        {
            held->forLow = planned;
            held->purpose = "улучшение по плану " + Who(planned);
        }
        Decision("BoE " + what + " → " + Who(planned) + ": в его плане экипировки, держит до передачи");
        return;
    }

    // 3. nobody's: an enchanter disenchants it, else it is sold
    if (equipment && proto->DisenchantID && proto->Quality >= ITEM_QUALITY_UNCOMMON)
    {
        uint32 const req = proto->RequiredDisenchantSkill;
        if (looter->HasSkill(SKILL_ENCHANTING) && looter->GetPureSkillValue(SKILL_ENCHANTING) >= req &&
            looter->HasSpell(SPELL_DISENCHANT))
        {
            release();
            st.disenchant.push_back(report.itemGuidLow);
            Decision(what + " → распылит сам (зачарователь, навык " +
                     std::to_string(looter->GetPureSkillValue(SKILL_ENCHANTING)) + ")");
            return;
        }
        uint32 enchanter = 0;
        for (PlanRow const& row : plan_)
            if (Player* m = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low)))
                if (m->HasSkill(SKILL_ENCHANTING) && m->GetPureSkillValue(SKILL_ENCHANTING) >= req)
                {
                    enchanter = row.low;
                    break;
                }
        if (enchanter && GuildBankRun::BankState(looter, true).empty())
        {
            if (held != st.held.end())
            {
                held->forLow = enchanter;
                held->purpose = "распылить " + Who(enchanter);
            }
            Decision(what + " → банк для зачарователя " + Who(enchanter));
            return;
        }
        release();
        st.sell.push_back(report.itemGuidLow);
        Decision(what + " → продать: никому не улучшение, " +
                 (enchanter ? "зачарователь " + Who(enchanter) + " есть, банка нет" : std::string("зачарователя нет")));
        return;
    }
    release();
    st.sell.push_back(report.itemGuidLow);
    Decision(what + " → продать: не улучшение и не в wanted list");
}

// ---------------------------------------------------------------------------------------------------------------------
// map thread: New RPG runs the order
// ---------------------------------------------------------------------------------------------------------------------
GuildRunStep GuildCoordinator::NextStep(Player* bot, PlayerbotAI* botAI)
{
    GuildRunStep step;
    if (!bot || !botAI || !IsPlanned(bot->GetGUID().GetCounter()))
        return step;
    std::lock_guard<std::mutex> guard(lock_);
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
    auto const started = [&](GuildOrder const& o)
    {
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
                while (!st.disenchant.empty())
                {
                    Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(st.disenchant.back()));
                    st.disenchant.pop_back();
                    // [tasks] only what no member needs: a gear plan that took it since the decision keeps it
                    if (uint32 const planned = item ? PlannedFor(item->GetEntry()) : 0)
                    {
                        st.held.push_back({item->GetGUID().GetCounter(), item->GetEntry(), item->GetCount(), planned,
                                           "улучшение по плану " + Who(planned)});
                        LOG_INFO("playerbots", "[tasks] {} keeps {} for {}: in its gear plan, not disenchanted",
                                 bot->GetName(), item->GetTemplate()->Name1, Who(planned));
                        continue;
                    }
                    if (item && !bot->IsNonMeleeSpellCast(false) && botAI->CastSpell(SPELL_DISENCHANT, bot, item))
                    {
                        step.kind = GuildRunStep::BUSY;
                        step.reason = "disenchant " + item->GetTemplate()->Name1;
                        started(order);
                        return step;
                    }
                }
                continue;
            }
            case GuildOrderType::BANK_DEPOSIT:
            case GuildOrderType::BANK_WITHDRAW:
            {
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
                            order.status = "место отдано лидеру: путь опасен";
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
                    bool const otherZone = place->spot.GetMapId() != bot->GetMapId() ||
                                           (place->zoneId && place->zoneId != bot->GetZoneId());
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
                            sGuildGatherPlan.Refuse(bot, order.placeId, refusal);
                            st.gatherWalkPlace = 0;
                            order.executable = false;
                            order.status = "место отдано лидеру: " + refusal;
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
                    if (place->kind == GatherKind::SKIN)
                    {
                        // [questrate] b338 Ainne: the skinning place of its own zone lay in its danger zone of Large
                        // Crag Boar; GO_GRIND refused the walk and the order sent it again (455 s, 36 alerts). The
                        // solo judge first, as for a place in another zone: refused -> back to the leader
                        if (NewRpgBaseAction::TripRefused(bot, place->spot.GetPositionX(), place->spot.GetPositionY(),
                                                          place->spot.GetPositionZ(), "skinning place"))
                        {
                            sGuildGatherPlan.Refuse(bot, order.placeId,
                                                    "the solo judge refused the walk (TRIP_REFUSED)");
                            order.executable = false;
                            order.status = "место отдано лидеру: путь опасен";
                            continue;
                        }
                        step.kind = GuildRunStep::GRIND;
                        step.pos = place->spot;
                        step.reason = "GATHER skin " + order.details;
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
                            order.status = "место отдано лидеру: сундуков нет";
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
                                order.status = "место отдано лидеру: " + refusal;
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
                    order.status = "не создаётся здесь (кузня / наковальня / инструмент?) — квестует";
                    continue;
                }
                if (order.forLow && order.forLow != low)
                    st.expect.push_back({recipe->itemId, order.forLow, "крафт для " + Who(order.forLow)});
                order.status = "создаёт";
                step.kind = GuildRunStep::BUSY;
                step.reason = "craft " + ItemName(recipe->itemId) + " for " + Who(order.forLow);
                LOG_INFO("playerbots", "[guild50] {} crafts {} for {}: {}", bot->GetName(), ItemName(recipe->itemId),
                         Who(order.forLow), order.reason);
                started(order);
                return step;
            }
            case GuildOrderType::BUY:  // [guild50]
            {
                if (bot->GetItemCount(order.itemId, true) || bot->GetMoney() < order.cost || bot->IsInCombat() ||
                    order.pos.GetMapId() != bot->GetMapId() ||
                    (order.extendedCost && !GuildGear::ExtendedCostMissing(bot, order.extendedCost).empty()) ||
                    NewRpgProfession::BuyFailed(bot, order.npcEntry, order.itemId))
                    continue;
                order.executable = false;
                if (NewRpgBaseAction::TripRefused(bot, order.pos.GetPositionX(), order.pos.GetPositionY(),
                                                  order.pos.GetPositionZ(), "vendor for " + ItemName(order.itemId)))
                {
                    order.status = "путь к торговцу опасен — ждёт уровня";
                    continue;
                }
                NewRpgProfession::StartBuy(botAI, bot, order.itemId, order.npcEntry, order.pos, order.cost);
                order.status = "идёт к торговцу";
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
    std::lock_guard<std::mutex> guard(lock_);
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
        st.vaultFail = "не дотянуться до хранилища";
        Decision(Who(low) + ": " + st.vaultFail + " — квестует дальше");
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
        for (HeldItem const& h : moving)
        {
            if (bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(h.itemGuidLow)))
                continue;  // still in the bags: not deposited
            if (h.forLow)
                withdraw_[h.forLow].emplace_back(h.itemId, "от " + Who(low) + ": " + h.purpose);
            st.held.erase(std::remove_if(st.held.begin(), st.held.end(),
                                         [&](HeldItem const& x) { return x.itemGuidLow == h.itemGuidLow; }),
                          st.held.end());
        }
        Decision(Who(low) + " положил в банк гильдии " + std::to_string(moved) + " из " +
                 std::to_string(guids.size()) + " предм.");
    }
    else if (st.runType == GuildOrderType::BANK_WITHDRAW)
    {
        auto& list = withdraw_[low];
        if (!list.empty())
        {
            uint32 const itemId = list.front().first;
            bool const ok = GuildBankRun::Withdraw(bot, itemId);
            list.erase(list.begin());
            Decision(Who(low) + (ok ? " взял из банка " : " не смог взять из банка ") + ItemName(itemId));
        }
    }
    return 1;
}

bool GuildCoordinator::IsHeld(Player* bot, Item* item)
{
    if (!bot || !item || !IsPlanned(bot->GetGUID().GetCounter()))
        return false;
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return false;
    for (HeldItem const& h : itr->second.held)
        if (h.itemGuidLow == item->GetGUID().GetCounter())
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
        std::lock_guard<std::mutex> guard(lock_);
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
    for (uint32 low : sell)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low));
        if (!item)
        {
            ++gone;  // the vendor sale of the bot itself or a use came first
            continue;
        }
        action.Sell(item);
        if (bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low)))
            ++kept;  // SellAction keeps what a quest or a spell still needs
        else
            ++sold;
    }
    LOG_INFO("playerbots", "[guild30] {} coordinator sales: sold {}, already gone {}, kept by SellAction {} (+{}c)",
             bot->GetName(), sold, gone, kept, bot->GetMoney() - before);
}

void GuildCoordinator::SetOrderStatus(Player* bot, std::string const& status)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return;
    for (GuildOrder& order : itr->second.orders)
        if (order.executable && !order.waitsTown)
        {
            order.status = status;
            return;
        }
}

void GuildCoordinator::SetWait(Player* bot, std::string const& reason)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    std::lock_guard<std::mutex> guard(lock_);
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
    std::lock_guard<std::mutex> guard(lock_);
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
    std::lock_guard<std::mutex> guard(lock_);
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
        std::lock_guard<std::mutex> guard(lock_);
        if (deathQueue_.empty() && envPending_.empty())
            return;
        reports.swap(deathQueue_);
    }
    std::map<uint32, DeathReport> byBot;
    {
        std::lock_guard<std::mutex> guard(lock_);
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
    std::lock_guard<std::mutex> guard(lock_);
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
    std::lock_guard<std::mutex> guard(lock_);
    BotState& st = bots_[bot->GetGUID().GetCounter()];
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
        auto const& [oldXp, oldProgress, x, y, z, map] = itr->second;
        noProgress = oldXp == xp && oldProgress == progress && map == bot->GetMapId() &&
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
                              bot->GetMapId()};
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

    std::lock_guard<std::mutex> guard(lock_);
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

        // per-hour totals from the deltas of characters online at both samples
        uint64 const xp = XpTotal(p);
        uint32 const quests = uint32(p->getRewardedQuests().size());
        uint32 const money = p->GetMoney();
        // [questrate] b338: Dandolar / Brelir / Lyleria bought 4-5 vendor upgrades in a row at one vendor (each BUY
        // order its own DO_PROFESSION trip, the next one issued when the last was equipped), standing still with no
        // XP: 20 STUCK alerts under BUY. Money changing hands (bought, sold, mailed, traded) is progress, as a skill
        // that rose is for a crafter
        bool const moneyMoved = st.seen && money != st.money;
        if (st.seen)
        {
            sample.xp += xp > st.xpTotal ? xp - st.xpTotal : 0;
            sample.quests += quests > st.questsDone ? quests - st.questsDone : 0;
            sample.money += int64(money) - int64(st.money);
        }
        st.seen = true;
        st.xpTotal = xp;
        st.questsDone = quests;
        st.money = money;
        RecordPace(st, p);  // [questrate] its copper per quest XP (the questing formula, UpdateWanted)

        if (p->IsAlive())
            st.deathOpen = false;
        if (!p->IsAlive())
        {
            ++dead;
            st.anchorAt = 0;
            st.stuckAlerted = false;
            continue;
        }
        // [guild15] crafting in place: a profession skill that rose since the last sample is progress, as a walk is
        // (the craft casts between the samples are short; the status may be IDLE / DO_QUEST between two crafts)
        uint32 skillSum = 0;
        for (uint32 const line : CRAFT_LINES)
            skillSum += p->HasSkill(line) ? p->GetPureSkillValue(line) : 0;
        bool const skillRose = st.skillSum && skillSum > st.skillSum;
        st.skillSum = skillSum;
        // stuck: out of combat, alive, not moved beyond its combat reach for 60 s, no legitimate wait
        float const reach = std::max(p->GetCombatReach(), 1.5f);
        if (p->IsInCombat() || !st.anchorAt || st.mapId != p->GetMapId() || skillRose || moneyMoved ||
            p->GetExactDist(st.ax, st.ay, st.az) > reach)
        {
            st.mapId = p->GetMapId();
            st.ax = p->GetPositionX();
            st.ay = p->GetPositionY();
            st.az = p->GetPositionZ();
            st.anchorAt = now;
            st.stuckAlerted = false;
            continue;
        }
        if (now - st.anchorAt < STUCK_SECONDS)
            continue;
        std::string const wait = WaitReason(p, ai, st);
        if (!wait.empty())
            continue;
        ++stuck;
        std::string const status = StatusName(p, ai);
        std::string const order = ActiveTask(st.orders);
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
    uint32 questsH = 0, deathsH = 0;
    int64 moneyH = 0;
    for (RateSample const& r : rates_)
    {
        xpH += r.xp;
        questsH += r.quests;
        deathsH += r.deaths;
        moneyH += r.money;
    }
    uint32 const window = rates_.size() > 1 ? now - rates_.front().at : 0;
    auto const perHour = [window](double v) { return window ? v * HOUR / window : v; };

    char buf[400];
    snprintf(buf, sizeof(buf),
             "тик ср %u / p95 %u / макс %u мс (цель %d); AI ср %.2f / макс %.1f мс/тик; онлайн %u/%u, мёртвых %u, "
             "застряли %u; за час: квесты %.0f, опыт %.0f, смерти %.0f, золото %.1f (окно %u мин)",
             avg, p95, maxDiff, target, aiAvgMs, aiMax / 1000.0f, online, uint32(plan_.size()), dead, stuck,
             perHour(questsH), perHour(double(xpH)), perHour(deathsH), perHour(double(moneyH)) / GOLD, window / MINUTE);
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
             "; задачи: %u, ждут города %u, начато городских %u (ср. ожидание %u мин, %.1f за визит)", waiting,
             waitingTown, townStarted_, townStarted_ ? uint32(townWaitSum_ / townStarted_ / MINUTE) : 0,
             townVisits_ ? float(townStarted_) / townVisits_ : 0.0f);
    healthRow_ += tasks;
    if (now - lastHealthLog_ >= MINUTE)
    {
        lastHealthLog_ = now;
        LOG_INFO("playerbots", "[health] {}", healthRow_);
        for (PlanRow const& row : plan_)
        {
            auto const st = bots_.find(row.low);
            if (st == bots_.end() || st->second.orders.size() < 2 || !st->second.seen)
                continue;
            LOG_INFO("playerbots", "[tasks] {}: {}", Who(row.low), TasksText(st->second));
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
    std::lock_guard<std::mutex> guard(lock_);
    SendLine(player, "OS;n=" + std::to_string(plan_.size()));
    SendLine(player, "OR;" + OrdersText(RaidLeaderText('A'), 40) + ";" + OrdersText(RaidLeaderText('B'), 40) + ";" +
                         OrdersText(!found_.state.empty() ? found_.state  // [guild80] founding / bank tab state
                                    : guildState_.empty() ? "гильдии нет: банк спит"
                                                          : guildState_,
                                    80));
    for (auto const& [at, text] : decisions_)
        SendLine(player, "OD;" + std::to_string(now - at) + ";" + OrdersText(text, 200));
    size_t shown = 0;
    for (WantedItem const& w : wanted_)
    {
        if (w.optional || !w.Short() || ++shown > 10)
            continue;
        std::string const source = w.gatherSkill ? std::string(SkillShort(w.gatherSkill)) + " " +
                                                       std::to_string(w.gatherReq)
                                   : w.vendor ? "торговец"
                                              : "добыча";
        SendLine(player, "OW;" + OrdersText(ItemName(w.itemId), 30) + ";" + std::to_string(w.Short()) + ";" +
                             std::to_string(w.have) + "/" + std::to_string(w.need) + ";" + source + ";" +
                             OrdersText(w.forWhom, 130) + ";R");
    }
    // [guild70] the optional ones (intermediates held somewhere): greyed at the bottom, never ordered
    size_t optionalShown = 0;
    for (WantedItem const& w : wanted_)
        if (w.optional && w.have && ++optionalShown <= 4)
            SendLine(player, "OW;" + OrdersText(ItemName(w.itemId), 30) + ";0;" + std::to_string(w.have) +
                                 "/0;банк;" + OrdersText(w.forWhom, 130) + ";O");
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
            status = "не в сети";
        else if (order.type == GuildOrderType::QUEST)
            status = StatusName(p, GET_PLAYERBOT_AI(p));
        if (st != bots_.end() && order.type != GuildOrderType::BANK_DEPOSIT)
        {
            uint32 hold = 0;
            for (HeldItem const& h : st->second.held)
                hold += h.purpose != "pending" && !TransferHolds(h.itemGuidLow) ? 1 : 0;  // [guild81]
            if (hold)
                details += "; держит для банка: " + std::to_string(hold);
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
                    state = runFound ? "ждёт" : "run";
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
                                                    " " + GainText(g.gain),
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
        std::lock_guard<std::mutex> guard(lock_);
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
                (num ? "#" + std::to_string(num) : std::string("банк")) + " ×" + std::to_string(count);
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
    std::lock_guard<std::mutex> guard(lock_);
    craftsDone_.push_back({Now(), bot->GetGUID().GetCounter(), spellId, before, after});
    while (craftsDone_.size() > 40)
        craftsDone_.pop_front();
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
            case GuildGear::SRC_QUEST: return "квест";
            case GuildGear::SRC_DROP: return "босс подземелья";
            case GuildGear::SRC_CRAFT: return "крафт";
            case GuildGear::SRC_VENDOR: return "торговец";
            case GuildGear::SRC_BANK: return "банк";
            case GuildGear::SRC_WORLD: return "мобы в мире";
            case GuildGear::SRC_REFDROP: return "мировой дроп";
            case GuildGear::SRC_OBJECT: return "сундук / объект";
            case GuildGear::SRC_CONTAINER: return "из предмета";
            default: return "другое";
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
                rows.push_back({"крафт", "в работе", Who(row.low), NewRpgCraft::RecipeName(task->spellId), "себе",
                                task->npcEntry ? "покупает реагент" : "крафт", "прокачка " + skill});
                continue;
            }
            auto const list = recipesBySkill_.find(line);
            auto const plan = skillPlan_.find(line);
            if (list == recipesBySkill_.end() || plan == skillPlan_.end())
                continue;
            if (value >= cap)
            {
                rows.push_back({"крафт", "ждёт ранг", Who(row.low), "—", "себе", "тренер",
                                "навык на пределе ранга: " + skill});
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
            rows.push_back({"крафт",
                            !known ? "учить рецепт" : mats ? "материалы есть" : "ждёт материалы", Who(row.low),
                            ItemName(r.product), "себе / банк", "крафт",
                            "прокачка " + skill + " до " + std::to_string((r.yellow + r.gray) / 2) +
                                (missing.empty() ? "" : "; нет: " + missing)});
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
            rows.push_back({"деньги",
                            guild ? "собрано " + Gsc(have) + " из " + Gsc(price) : std::string("нет гильдии"),
                            "банк гильдии", "вкладка банка " + std::to_string(tabs + 1) + ": " + Gsc(price),
                            "гильдия", "золото банка гильдии",
                            "цена ядра (GuildBankTabCost" + std::to_string(tabs) + ")" +
                                (guild ? "" : "; гильдию основывают боты по уставу, до тех пор 0")});
        }
        for (PlanRow const& row : plan_)
        {
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
            if (!p || !p->IsInWorld())
                continue;
            uint32 const money = p->GetMoney();
            auto const state = [&](uint32 cost)
            { return money >= cost ? "хватает (есть " + Gsc(money) + ")" : "копит: есть " + Gsc(money); };
            if (uint32 const ranks = NewRpgProfession::NextRankCost(p))
                rows.push_back({"деньги", state(ranks), Who(row.low), "ранги профессий: " + Gsc(ranks), "себе",
                                "тренер, свои деньги", "следующий ранг, который тренер учит сейчас"});
            std::string tools;
            if (uint32 const cost = NewRpgProfession::MissingTools(p, tools))
                rows.push_back({"деньги", state(cost), Who(row.low), "инструменты: " + Gsc(cost), "себе",
                                "торговец, свои деньги", tools});
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                if (auto const* task = std::get_if<NewRpgInfo::DoProfession>(&ai->rpgInfo.data))
                    if (task->cost)
                    {
                        char const* const what = task->kind == 1                       ? "ранг у тренера"
                                                 : task->kind == NewRpgCraft::TASK_RECIPES ? "рецепты у тренера"
                                                 : task->kind == NewRpgCraft::TASK_CRAFT   ? "реагенты у торговца"
                                                                                           : "покупка у торговца";
                        rows.push_back({"деньги", "в пути: " + state(task->cost), Who(row.low),
                                        std::string(what) + ": " + Gsc(task->cost), "себе", "свои деньги",
                                        task->kind == NewRpgCraft::TASK_CRAFT ? NewRpgCraft::RecipeName(task->spellId)
                                                                              : std::string("поручение профессии")});
                    }
        }
    }

    std::lock_guard<std::mutex> guard(lock_);
    for (auto const& [low, st] : bots_)  // [guild70] the leader's BUY orders: who pays what
        for (GuildOrder const& o : st.orders)
            if (o.type == GuildOrderType::BUY)
                rows.push_back({"деньги", o.status, Who(low), ItemName(o.itemId) + ": " + Gsc(o.cost), "себе",
                                "торговец, свои деньги", o.reason});
    for (CraftJob const& job : crafts_)
    {
        std::string state = job.ready ? "материалы есть" : "ждёт материалы";
        auto const st = bots_.find(job.crafterLow);
        if (st != bots_.end())
            for (GuildOrder const& o : st->second.orders)
                if (o.type == GuildOrderType::CRAFT_FOR && o.spellId == job.spellId && o.status == "создаёт")
                    state = "в работе";
        rows.push_back({"крафт", state, Who(job.crafterLow), ItemName(job.itemId),
                        job.forLow == job.crafterLow ? "себе" : Who(job.forLow), job.bag ? "крафт (сумка)" : "крафт",
                        job.why});
    }
    for (CraftDone const& d : craftsDone_)
        rows.push_back({"крафт", "сделано " + std::to_string((now - d.at) / 60) + " мин назад", Who(d.low),
                        NewRpgCraft::RecipeName(d.spellId), "себе / банк", "крафт",
                        "навык " + std::to_string(d.before) + "→" + std::to_string(d.after)});

    // transfers: [guild81] trade / mail / bank between members, then what waits for the guild bank
    {
        std::vector<std::vector<std::string>> transfers;
        TransferPlanRows(transfers);
        for (auto const& t : transfers)
            rows.push_back({t[0], t[1], t[2], t[3], t[4], t[5], t[6]});
    }
    for (auto const& [low, list] : withdraw_)
        for (auto const& [item, why] : list)
            rows.push_back({"передача", "ждёт похода в банк", "банк", ItemName(item), Who(low), "банк", why});
    for (auto const& [low, st] : bots_)
        for (HeldItem const& h : st.held)
        {
            if (h.purpose == "pending" || TransferHolds(h.itemGuidLow))  // [guild81] listed above
                continue;
            std::string const what = ItemName(h.itemId) + (h.count > 1 ? " ×" + std::to_string(h.count) : "");
            rows.push_back({"передача", st.vaultFail.empty() ? "держит до банка" : st.vaultFail, Who(low), what,
                            h.forLow ? "банк → " + Who(h.forLow) : std::string("банк"), "банк", h.purpose});
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
        std::string how = !gatherers.empty() ? "сбор: " + gatherers
                          : w.vendor          ? "торговец (за свои деньги)"
                          : w.gatherSkill     ? std::string("сбор ") + SkillShort(w.gatherSkill) + " " +
                                                std::to_string(w.gatherReq) + ": нет сборщика с навыком"
                                              : "добыча с мобов";
        rows.push_back({"источник", "не хватает " + std::to_string(w.Short()),
                        gatherers.empty() ? "—" : gatherers, ItemName(w.itemId), "wanted list", how, w.forWhom});
    }
    for (auto const& [low, st] : bots_)
    {
        std::unordered_set<uint32> seen;
        for (auto const* list : {&st.gearNow, &st.gearPlan})
            for (GearPick const& g : *list)
            {
                if (g.kind == GuildGear::SRC_BANK || !seen.insert(g.itemId).second)
                    continue;
                rows.push_back({"источник", list == &st.gearNow ? "доступно сейчас" : "в плане",
                                g.crafterLow ? Who(g.crafterLow) : std::string("—"), ItemName(g.itemId), Who(low),
                                kindName(g.kind),
                                std::string("улучшение ") + GuildGear::GroupName(g.group) + " " + GainText(g.gain) +
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
        : PlayerScript("GuildCoordinatorPlayerScript", {PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_PLAYER_JUST_DIED,
                                                         PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
                                                         PLAYERHOOK_ON_CREATE_ITEM})
    {
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootguid*/) override
    {
        sGuildCoordinator.OnLoot(player, item, count);
    }

    // [guild70] a skill-up craft's product goes the loot way (NewRpgCraft decides which ones)
    void OnPlayerCreateItem(Player* player, Item* item, uint32 count) override
    {
        NewRpgCraft::OnCreateItem(player, item, count);
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

void AddGuildCoordinatorScripts()
{
    new GuildCoordinatorPlayerScript();
    new GuildCoordinatorWorldScript();
}
