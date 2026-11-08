/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildProfPath.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <sstream>

#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildGatherPlan.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"

namespace
{
    uint32 const PATH_LINES[] = {SKILL_BLACKSMITHING, SKILL_LEATHERWORKING, SKILL_TAILORING, SKILL_ENGINEERING,
                                 SKILL_ALCHEMY,       SKILL_ENCHANTING,     SKILL_JEWELCRAFTING, SKILL_INSCRIPTION,
                                 SKILL_COOKING,       SKILL_FIRST_AID};
    constexpr uint32 SPELL_LEARNING = 483;           // recipe items: spell 1 "Learning", spell 2 the recipe
    constexpr uint32 SPELL_LEARNING_NORTHREND = 55884;
    constexpr uint32 SKILL_STEP_MAX = 75;            // Spell::EffectLearnSkill: a rank's cap = step x 75
    constexpr uint32 ACQUIRE_ON_SKILL_VALUE = 1;     // SkillLineAbility AcquireMethod: learned with the skill
    constexpr uint32 ACQUIRE_ON_SKILL_LEARN = 2;
    constexpr uint32 MAX_PRICE_DEPTH = 4;            // reagent chains (ore -> bar -> part -> item)
    constexpr uint32 MAX_REFERENCE_DEPTH = 4;
    constexpr uint32 MAX_SKILL_VALUE = 450;
    constexpr uint8 FRIEND_ALLIANCE = 1;
    constexpr uint8 FRIEND_HORDE = 2;
    constexpr double NO_RECIPE = 1e12;  // a skill value no recipe in the set covers: any recipe there saves "all"
    constexpr double INF = std::numeric_limits<double>::infinity();

    constexpr uint64 HASH_GOLDEN = 0x9e3779b97f4a7c15ULL;
    constexpr uint64 HASH_PRIME = 1099511628211ULL;

    uint64 Mix(uint64 h, uint64 v) { return (h ^ (v + HASH_GOLDEN + (h << 6) + (h >> 2))) * HASH_PRIME; }

    std::string ItemName(uint32 itemId)
    {
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
            return proto->Name1;
        return std::to_string(itemId);
    }

    std::string SpellName(uint32 spellId)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            if (info->SpellName[LOCALE_enUS])
                return info->SpellName[LOCALE_enUS];
        return std::to_string(spellId);
    }

    std::string SkillLineName(uint32 skillLine)
    {
        if (SkillLineEntry const* entry = sSkillLineStore.LookupEntry(skillLine))
            if (entry->name[LOCALE_enUS])
                return entry->name[LOCALE_enUS];
        return std::to_string(skillLine);
    }

    std::string Money(double copper)
    {
        if (!std::isfinite(copper))
            return "inf";
        int64 c = int64(std::llround(copper));
        std::string const sign = c < 0 ? "-" : "";
        c = std::abs(c);
        return Acore::StringFormat("{}{}g{:02}s{:02}c", sign, c / 10000, c / 100 % 100, c % 100);
    }

    char const* RankName(uint32 rank)
    {
        static char const* const names[] = {"Hated",  "Hostile", "Unfriendly", "Neutral",
                                            "Friendly", "Honored", "Revered",    "Exalted"};
        return rank < std::size(names) ? names[rank] : "?";
    }

    // the spell a trainer spell / recipe item spell teaches (SPELL_EFFECT_LEARN_SPELL), else itself
    uint32 Taught(uint32 spellId)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                if (info->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL && info->Effects[e].TriggerSpell)
                    return info->Effects[e].TriggerSpell;
        return spellId;
    }

    // expected items of one roll of a loot template (references unrolled, quest-only entries skipped)
    void Expected(LootTemplate const* tmpl, std::map<uint32, float>& out, float scale, uint32 depth = 0)
    {
        if (!tmpl || depth > MAX_REFERENCE_DEPTH)
            return;
        tmpl->VisitEntries(
            [&](LootStoreItem const& e, float chance)
            {
                if (e.needs_quest || !(e.lootmode & LOOT_MODE_DEFAULT))
                    return;
                float const p = std::min(100.0f, chance) / 100.0f;
                if (e.reference)
                {
                    // LootTemplate::Process: a reference entry is processed maxcount times
                    Expected(LootTemplates_Reference.GetLootFor(uint32(std::abs(e.reference))), out,
                             scale * p * float(std::max<uint8>(1, e.maxcount)), depth + 1);
                    return;
                }
                out[e.itemid] += scale * p * (float(e.mincount) + float(e.maxcount)) / 2.0f;
            });
    }

    // the expected (copper, raw units, crafts) of one skill point at `value`
    struct PointCost
    {
        double copper{INF};
        double units{INF};
        double crafts{0.0};
        bool operator<(PointCost const& o) const
        {
            return copper < o.copper || (copper == o.copper && units < o.units);
        }
    };

    PointCost PerPoint(ProfRecipe const& r, uint32 value, double craftCopper, double craftUnits)
    {
        PointCost out;
        if (!r.gray || r.reqSkill > value || !std::isfinite(craftCopper))
            return out;
        int32 const chance = GuildProfPath::Chance(value, r.gray, r.yellow);
        if (chance <= 0)
            return out;
        uint32 const gain = std::max<uint32>(1, sWorld->getIntConfig(CONFIG_SKILL_GAIN_CRAFTING));
        out.crafts = 1000.0 / double(chance) / double(gain);
        out.copper = craftCopper * out.crafts;
        out.units = craftUnits * out.crafts;
        return out;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// member
// ---------------------------------------------------------------------------------------------------------------------
uint64 ProfMember::Fingerprint(uint32 skillLine) const
{
    uint64 h = Mix(Mix(low, skillLine), level);
    auto const value = skill.find(skillLine);
    auto const cap = maxSkill.find(skillLine);
    h = Mix(h, value == skill.end() ? 0 : value->second);
    h = Mix(h, cap == maxSkill.end() ? 0 : cap->second);
    if (std::vector<ProfRecipe> const* list = sGuildProfPath.Recipes(skillLine))
        for (ProfRecipe const& r : *list)
            if (known.count(r.spellId))
                h = Mix(h, r.spellId);
    for (auto const& [faction, rank] : reputation)
        h = Mix(Mix(h, faction), rank);
    for (uint32 c : toolCategories)
        h = Mix(h, c);
    return h;
}

ProfMember ProfMember::FromPlayer(Player* player, bool human)
{
    ProfMember m;
    if (!player)
        return m;
    m.low = player->GetGUID().GetCounter();
    m.level = player->GetLevel();
    m.factionTemplate = player->GetFaction();
    m.raceMask = player->getRaceMask();
    m.classMask = player->getClassMask();
    m.human = human;
    for (uint32 line : PATH_LINES)
        if (player->HasSkill(line))
        {
            m.skill[line] = player->GetPureSkillValue(line);
            m.maxSkill[line] = player->GetPureMaxSkillValue(line);
        }
    for (auto const& [spellId, spell] : player->GetSpellMap())
        if (spell && spell->State != PLAYERSPELL_REMOVED && sGuildProfPath.Recipe(spellId))
            m.known.insert(spellId);
    for (TotemCategoryEntry const* entry : sTotemCategoryStore)
        if (entry && player->HasItemTotemCategory(entry->ID))
            m.toolCategories.insert(entry->ID);
    for (uint32 faction : sGuildProfPath.RepFactions())
        if (sFactionStore.LookupEntry(faction))
            m.reputation[faction] = uint32(player->GetReputationRank(faction));
    return m;
}

// ---------------------------------------------------------------------------------------------------------------------
// load (world thread, startup)
// ---------------------------------------------------------------------------------------------------------------------
int32 GuildProfPath::Chance(uint32 skillValue, uint32 gray, uint32 yellow)
{
    // Player::UpdateCraftSkill -> CraftSkillGainChance, the same formula and config values
    int32 const orange = int32(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE) * 10);
    int32 const grey = int32(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREY) * 10);
    if (gray <= yellow)
        return skillValue < gray ? orange : grey;
    if (skillValue <= yellow)
        return orange;
    if (skillValue >= gray)
        return grey;
    return grey + int32(int64(gray - skillValue) * (orange - grey) / (gray - yellow));
}

uint8 GuildProfPath::FriendMask(uint32 factionTemplate) const
{
    FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(factionTemplate);
    if (!ft || ft->IsHostileToPlayers())
        return 0;
    uint8 mask = 0;
    if (!(ft->hostileMask & FACTION_MASK_ALLIANCE))
        mask |= FRIEND_ALLIANCE;
    if (!(ft->hostileMask & FACTION_MASK_HORDE))
        mask |= FRIEND_HORDE;
    return mask;
}

void GuildProfPath::Load()
{
    uint32 const start = getMSTime();
    bySkill_.clear();
    bySpell_.clear();
    byProduct_.clear();
    byEnchant_.clear();
    ranks_.clear();
    vendors_.clear();
    trainerOf_.clear();
    recipeItems_.clear();
    bestDrop_.clear();
    worldDrop_.clear();
    questReward_.clear();
    questSpell_.clear();
    discovery_.clear();
    deMatCopper_.clear();
    totemItems_.clear();
    repFactions_.clear();
    {
        std::lock_guard<std::mutex> guard(lock_);
        cache_.clear();
    }

    LoadTrainers();
    LoadVendors();
    LoadRecipeItems();
    LoadDrops();
    LoadDisenchant();
    LoadRecipes();
    SortSources();
    loaded_ = true;

    size_t recipes = 0;
    for (auto const& [line, list] : bySkill_)
        recipes += list.size();
    LOG_INFO("playerbots", "[prof] skill-up index: {} lines, {} recipes, {} vendor items, {} disenchant mats in {} ms",
             bySkill_.size(), recipes, vendors_.size(), deMatCopper_.size(), GetMSTimeDiffToNow(start));
}

void GuildProfPath::LoadTrainers()
{
    // creature_default_trainer x trainer_spell: the recipe's trainer, fee, skill and level; profession ranks
    QueryResult result = WorldDatabase.Query(
        "SELECT d.CreatureId, s.TrainerId, s.SpellId, s.MoneyCost, s.ReqSkillRank, s.ReqLevel "
        "FROM trainer_spell s JOIN creature_default_trainer d ON d.TrainerId = s.TrainerId");
    if (!result)
        return;
    do
    {
        Field* f = result->Fetch();
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(f[0].Get<uint32>());
        if (!tmpl)
            continue;
        uint8 const friendly = FriendMask(tmpl->faction);
        if (!friendly)
            continue;
        uint32 const spellId = Taught(f[2].Get<uint32>());
        uint32 const price = f[3].Get<uint32>(), req = f[4].Get<uint32>();
        uint8 const level = f[5].Get<uint8>();
        if (SpellInfo const* rank = sSpellMgr->GetSpellInfo(spellId))
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                if ((rank->Effects[e].Effect == SPELL_EFFECT_SKILL_STEP ||
                     rank->Effects[e].Effect == SPELL_EFFECT_SKILL) &&
                    rank->Effects[e].MiscValue > 0)
                {
                    uint32 const cap = uint32(std::max(0, rank->Effects[e].CalcValue())) * SKILL_STEP_MAX;
                    auto& list = ranks_[uint32(rank->Effects[e].MiscValue)];
                    auto same =
                        std::find_if(list.begin(), list.end(), [&](Rank const& r) { return r.maxSkill == cap; });
                    if (same == list.end())
                        list.push_back({level, req, cap, price});
                    else if (level < same->reqLevel)
                        *same = {level, req, cap, price};
                }
        ProfRecipeSource src;
        src.kind = ProfSourceKind::TRAINER;
        src.entry = f[1].Get<uint32>();
        src.price = price;
        src.reqLevel = level;
        src.rank = req;  // TRAINER: the ReqSkillRank (the recipe's skill requirement)
        src.friendly = friendly;
        auto& list = trainerOf_[spellId];
        auto same =
            std::find_if(list.begin(), list.end(), [&](ProfRecipeSource const& s) { return s.entry == src.entry; });
        if (same == list.end())
            list.push_back(src);
    } while (result->NextRow());
    for (auto& [line, list] : ranks_)
        std::sort(list.begin(), list.end(), [](Rank const& a, Rank const& b) { return a.maxSkill < b.maxSkill; });
}

void GuildProfPath::LoadVendors()
{
    std::unordered_set<uint32> seen;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!seen.insert(data.id).second)
            continue;
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
        if (!tmpl || !(tmpl->npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;
        uint8 const friendly = FriendMask(tmpl->faction);
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id);
        if (!friendly || !items)
            continue;
        for (VendorItem const* v : items->m_items)
        {
            ItemTemplate const* proto = v ? sObjectMgr->GetItemTemplate(v->item) : nullptr;
            if (!proto)
                continue;
            VendorOffer offer;
            offer.vendor = data.id;
            offer.price = uint32(std::max(0, proto->BuyPrice)) / std::max<uint32>(1, proto->BuyCount);
            offer.maxCount = v->maxcount;
            offer.incrTime = v->incrtime;
            offer.extendedCost = v->ExtendedCost;
            offer.friendly = friendly;
            vendors_[v->item].push_back(offer);
        }
    }
    for (auto& [item, list] : vendors_)
        std::sort(list.begin(), list.end(),
                  [](VendorOffer const& a, VendorOffer const& b)
                  {
                      if ((a.extendedCost != 0) != (b.extendedCost != 0))
                          return !a.extendedCost;
                      if ((a.maxCount != 0) != (b.maxCount != 0))
                          return !a.maxCount;
                      return a.price < b.price;
                  });
}

void GuildProfPath::LoadRecipeItems()
{
    for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (proto.TotemCategory)
            totemItems_[proto.TotemCategory].push_back(entry);
        if (proto.Class != ITEM_CLASS_RECIPE)
            continue;
        uint32 const first = proto.Spells[0].SpellId > 0 ? uint32(proto.Spells[0].SpellId) : 0;
        uint32 const second = proto.Spells[1].SpellId > 0 ? uint32(proto.Spells[1].SpellId) : 0;
        uint32 const spellId = (first == SPELL_LEARNING || first == SPELL_LEARNING_NORTHREND) ? second : Taught(first);
        if (spellId)
            recipeItems_[spellId].push_back(entry);
        if (proto.RequiredReputationFaction)
            repFactions_.insert(proto.RequiredReputationFaction);
    }
    for (auto& [category, items] : totemItems_)
        std::sort(items.begin(), items.end(),
                  [](uint32 a, uint32 b)
                  {
                      ItemTemplate const* pa = sObjectMgr->GetItemTemplate(a);
                      ItemTemplate const* pb = sObjectMgr->GetItemTemplate(b);
                      return (pa ? pa->BuyPrice : 0) < (pb ? pb->BuyPrice : 0);
                  });

    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
    {
        if (!quest)
            continue;
        for (uint32 item : quest->RewardItemId)
            if (item)
                questReward_.emplace(item, questId);
        for (uint32 item : quest->RewardChoiceItemId)
            if (item)
                questReward_.emplace(item, questId);
        if (quest->GetRewSpellCast() > 0)
            questSpell_.emplace(Taught(uint32(quest->GetRewSpellCast())), questId);
    }
    if (QueryResult result = WorldDatabase.Query("SELECT spellId, reqSkillValue FROM skill_discovery_template"))
    {
        do
        {
            Field* f = result->Fetch();
            discovery_[f[0].Get<uint32>()] = f[1].Get<uint32>();
        } while (result->NextRow());
    }
}

void GuildProfPath::LoadDrops()
{
    // creature loot: the best per-kill chance of each item (loot id), shared references: world drops
    for (auto const& [lootId, tmpl] : LootTemplates_Creature.GetTemplates())
        tmpl->VisitEntries(
            [&](LootStoreItem const& e, float chance)
            {
                if (e.reference || e.needs_quest)
                    return;
                auto& best = bestDrop_[e.itemid];
                if (chance > best.second)
                    best = {lootId, chance};
            });
    for (auto const& [refId, tmpl] : LootTemplates_Reference.GetTemplates())
        tmpl->VisitEntries(
            [&](LootStoreItem const& e, float /*chance*/)
            {
                if (!e.reference && !e.needs_quest)
                    worldDrop_.insert(e.itemid);
            });
}

void GuildProfPath::LoadDisenchant()
{
    // a disenchant mat's opportunity cost: the median over the spare items of its bands of the item's sell price,
    // shared by the expected count of all the mats the item gives
    std::unordered_map<uint32, std::map<uint32, float>> yield;
    for (auto const& [deId, tmpl] : LootTemplates_Disenchant.GetTemplates())
        Expected(tmpl, yield[deId], 1.0f);
    std::unordered_map<uint32, std::vector<double>> perMat;
    for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (!proto.DisenchantID || !proto.SellPrice || proto.Quality < ITEM_QUALITY_UNCOMMON ||
            proto.Quality > ITEM_QUALITY_EPIC)
            continue;
        auto const y = yield.find(proto.DisenchantID);
        if (y == yield.end())
            continue;
        double total = 0.0;
        for (auto const& [mat, n] : y->second)
            total += n;
        if (total <= 0.0)
            continue;
        for (auto const& [mat, n] : y->second)
            if (n > 0.0f)
                perMat[mat].push_back(double(proto.SellPrice) / total);
    }
    for (auto& [mat, list] : perMat)
    {
        std::nth_element(list.begin(), list.begin() + list.size() / 2, list.end());
        deMatCopper_[mat] = std::max(1.0, list[list.size() / 2]);
    }
}

void GuildProfPath::LoadRecipes()
{
    std::unordered_set<uint32> const lines(std::begin(PATH_LINES), std::end(PATH_LINES));
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
        if (!ability || (!lines.count(ability->SkillLine) && ability->SkillLine != SKILL_MINING) ||
            bySpell_.count(ability->Spell))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell);
        if (!info)
            continue;
        ProfRecipe r;
        r.spellId = ability->Spell;
        r.skillLine = ability->SkillLine;
        r.yellow = ability->TrivialSkillLineRankLow;
        r.gray = ability->TrivialSkillLineRankHigh;
        r.raceMask = ability->RaceMask;
        r.classMask = ability->ClassMask;
        r.reqSkill = ability->MinSkillLineRank;
        for (uint8 k = 0; k < MAX_SPELL_REAGENTS; ++k)
            if (info->Reagent[k] > 0 && info->ReagentCount[k] > 0)
                r.reagents.emplace_back(uint32(info->Reagent[k]), info->ReagentCount[k]);
        if (r.reagents.empty())
            continue;  // Disenchant, Prospecting, Milling: conversions, priced by GuildGatherPlan
        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
        {
            SpellEffectInfo const& eff = info->Effects[e];
            if ((eff.Effect == SPELL_EFFECT_CREATE_ITEM || eff.Effect == SPELL_EFFECT_CREATE_ITEM_2) && eff.ItemType &&
                !r.product)
            {
                r.product = eff.ItemType;
                // SpellEffectInfo::CalcValue: BasePoints + 1 .. BasePoints + DieSides
                r.made = eff.DieSides > 1 ? float(2 * eff.BasePoints + 1 + eff.DieSides) / 2.0f
                                          : float(std::max(1, eff.BasePoints + 1));
            }
            if (eff.Effect == SPELL_EFFECT_ENCHANT_ITEM && eff.MiscValue > 0 && !r.enchantId)
                r.enchantId = uint32(eff.MiscValue);
        }
        uint32 const castMs = info->CastTimeEntry ? uint32(std::max(0, info->CastTimeEntry->CastTime)) : 0;
        r.castSeconds = float(std::max(castMs, info->StartRecoveryTime)) / 1000.0f;
        r.focus = info->RequiresSpellFocus;
        for (uint32 t : info->Totem)
            if (t)
                r.totems.push_back(t);
        for (uint32 c : info->TotemCategory)
            if (c)
                r.totemCategories.push_back(c);

        // sources
        if (ability->AcquireMethod == ACQUIRE_ON_SKILL_VALUE || ability->AcquireMethod == ACQUIRE_ON_SKILL_LEARN)
            r.sources.push_back({ProfSourceKind::AUTO});
        if (auto const t = trainerOf_.find(r.spellId); t != trainerOf_.end())
        {
            uint32 req = std::numeric_limits<uint32>::max();
            for (ProfRecipeSource const& s : t->second)
            {
                r.sources.push_back(s);
                req = std::min(req, s.rank);
            }
            r.reqSkill = req;
        }
        if (auto const d = discovery_.find(r.spellId); d != discovery_.end())
        {
            r.discovery = true;
            r.reqSkill = std::max(r.reqSkill, d->second);
            r.sources.push_back({ProfSourceKind::DISCOVERY});
        }
        if (auto const items = recipeItems_.find(r.spellId); items != recipeItems_.end())
        {
            uint32 itemReq = std::numeric_limits<uint32>::max();
            for (uint32 item : items->second)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                if (!proto)
                    continue;
                itemReq = std::min(itemReq, proto->RequiredSkillRank);
                if (auto const v = vendors_.find(item); v != vendors_.end())
                    for (VendorOffer const& o : v->second)
                    {
                        ProfRecipeSource s;
                        s.kind = o.extendedCost             ? ProfSourceKind::VENDOR_TOKENS
                                 : proto->RequiredReputationFaction ? ProfSourceKind::REPUTATION
                                 : o.maxCount                ? ProfSourceKind::VENDOR_LIMITED
                                                             : ProfSourceKind::VENDOR;
                        s.entry = o.vendor;
                        s.item = item;
                        s.price = o.price;
                        s.maxCount = o.maxCount;
                        s.incrTime = o.incrTime;
                        s.faction = proto->RequiredReputationFaction;
                        s.rank = proto->RequiredReputationRank;
                        s.friendly = o.friendly;
                        r.sources.push_back(s);
                    }
                if (auto const drop = bestDrop_.find(item); drop != bestDrop_.end())
                {
                    ProfRecipeSource s;
                    s.kind = ProfSourceKind::DROP;
                    s.entry = drop->second.first;
                    s.item = item;
                    s.chance = drop->second.second;
                    r.sources.push_back(s);
                }
                else if (worldDrop_.count(item))
                {
                    ProfRecipeSource s;
                    s.kind = ProfSourceKind::WORLD_DROP;
                    s.item = item;
                    r.sources.push_back(s);
                }
                if (auto const q = questReward_.find(item); q != questReward_.end())
                {
                    ProfRecipeSource s;
                    s.kind = ProfSourceKind::QUEST;
                    s.entry = q->second;
                    s.item = item;
                    r.sources.push_back(s);
                }
            }
            if (itemReq != std::numeric_limits<uint32>::max() && !trainerOf_.count(r.spellId))
                r.reqSkill = std::max(r.reqSkill, itemReq);
        }
        if (auto const q = questSpell_.find(r.spellId); q != questSpell_.end())
        {
            ProfRecipeSource s;
            s.kind = ProfSourceKind::QUEST;
            s.entry = q->second;
            r.sources.push_back(s);
        }

        auto& list = bySkill_[r.skillLine];
        bySpell_[r.spellId] = {r.skillLine, list.size()};
        if (r.product)
            byProduct_[r.product].push_back(r.spellId);
        if (r.enchantId)
            byEnchant_[r.enchantId].push_back(r.spellId);
        list.push_back(std::move(r));
    }
}

void GuildProfPath::SortSources()
{
    auto const order = [](ProfSourceKind k) { return uint8(k); };
    for (auto& [line, list] : bySkill_)
        for (ProfRecipe& r : list)
            std::stable_sort(r.sources.begin(), r.sources.end(),
                             [&](ProfRecipeSource const& a, ProfRecipeSource const& b)
                             {
                                 if (a.kind != b.kind)
                                     return order(a.kind) < order(b.kind);
                                 return a.price < b.price;
                             });
}

// ---------------------------------------------------------------------------------------------------------------------
// lookups (read-only after Load)
// ---------------------------------------------------------------------------------------------------------------------
ProfRecipe const* GuildProfPath::Recipe(uint32 spellId) const
{
    auto const itr = bySpell_.find(spellId);
    if (itr == bySpell_.end())
        return nullptr;
    auto const list = bySkill_.find(itr->second.first);
    return list == bySkill_.end() ? nullptr : &list->second[itr->second.second];
}

std::vector<ProfRecipe> const* GuildProfPath::Recipes(uint32 skillLine) const
{
    auto const itr = bySkill_.find(skillLine);
    return itr == bySkill_.end() ? nullptr : &itr->second;
}

std::vector<ProfRecipe const*> GuildProfPath::RecipesFor(uint32 product) const
{
    std::vector<ProfRecipe const*> out;
    if (auto const itr = byProduct_.find(product); itr != byProduct_.end())
        for (uint32 spellId : itr->second)
            if (ProfRecipe const* r = Recipe(spellId))
                out.push_back(r);
    return out;
}

std::vector<ProfRecipe const*> GuildProfPath::RecipesForEnchant(uint32 enchantId) const
{
    std::vector<ProfRecipe const*> out;
    if (auto const itr = byEnchant_.find(enchantId); itr != byEnchant_.end())
        for (uint32 spellId : itr->second)
            if (ProfRecipe const* r = Recipe(spellId))
                out.push_back(r);
    return out;
}

uint32 GuildProfPath::TrainerCap(uint32 skillLine, uint8 level) const
{
    uint32 cap = 0;
    if (auto const itr = ranks_.find(skillLine); itr != ranks_.end())
        for (Rank const& r : itr->second)
            if (r.reqLevel <= level)
                cap = std::max(cap, r.maxSkill);
    return cap;
}

// ---------------------------------------------------------------------------------------------------------------------
// prices (plan time)
// ---------------------------------------------------------------------------------------------------------------------
GuildProfPath::UnitPrice GuildProfPath::Price(PriceCtx& ctx, uint32 item, uint32 depth) const
{
    if (auto const memo = ctx.memo.find(item); memo != ctx.memo.end())
        return memo->second;
    UnitPrice best;
    best.copper = INF;
    if (depth > MAX_PRICE_DEPTH)
        return best;
    ctx.memo[item] = best;  // a cycle (essence <-> essence) sees no price

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    double const sell = proto ? double(proto->SellPrice) : 0.0;
    auto const consider = [&](UnitPrice const& p)
    {
        if (p.copper < best.copper)
            best = p;
    };
    if (auto const offers = vendors_.find(item); offers != vendors_.end())
        for (VendorOffer const& o : offers->second)
            if (!o.extendedCost && (o.friendly & ctx.friendMask))
            {
                UnitPrice p;
                p.copper = double(o.price);
                p.way = UnitWay::BUY;
                consider(p);
                break;
            }
    uint32 skill = 0;
    if (sGuildGatherPlan.Gatherable(item, &skill))
    {
        UnitPrice p;
        p.way = UnitWay::GATHER;
        p.gatherSkill = skill;
        auto const pace = ctx.model->gatherPace.find(skill);
        float const cpm =
            ctx.model->gatherCopperPerMinute > 0.0f ? ctx.model->gatherCopperPerMinute : ctx.model->copperPerMinute;
        // the gatherers' time at the measured pace; unmeasured: the sell price the guild gives up
        p.copper = pace != ctx.model->gatherPace.end() && pace->second > 0.0f && cpm > 0.0f
                       ? double(cpm) / double(pace->second)
                       : std::max(1.0, sell);
        consider(p);
    }
    if (auto const de = deMatCopper_.find(item); de != deMatCopper_.end())
    {
        UnitPrice p;
        p.way = UnitWay::DISENCHANT;
        p.copper = de->second;
        consider(p);
    }
    if (std::vector<std::pair<uint32, float>> const* from = sGuildGatherPlan.ConvertedFrom(item))
        for (auto const& [raw, per] : *from)
        {
            UnitPrice const rawPrice = Price(ctx, raw, depth + 1);
            UnitPrice p;
            p.way = UnitWay::CONVERT;
            p.raw = raw;
            p.rawPerUnit = per;
            p.copper = double(per) * rawPrice.copper;
            consider(p);
        }
    if (auto const made = byProduct_.find(item); made != byProduct_.end())
        for (uint32 spellId : made->second)
        {
            ProfRecipe const* r = Recipe(spellId);
            if (!r || r->discovery || r->sources.empty() ||
                (r->sources.front().kind != ProfSourceKind::AUTO && r->sources.front().kind != ProfSourceKind::TRAINER))
                continue;  // an intermediate is made by a recipe every crafter of its line can learn
            double copper = double(r->castSeconds) * ctx.model->copperPerMinute / 60.0;
            for (auto const& [reagent, n] : r->reagents)
                copper += double(n) * Price(ctx, reagent, depth + 1).copper;
            UnitPrice p;
            p.way = UnitWay::CRAFT;
            p.recipe = r;
            p.copper = copper / double(std::max(0.01f, r->made));
            consider(p);
        }
    if (!std::isfinite(best.copper) && sell > 0.0 && (bestDrop_.count(item) || worldDrop_.count(item)))
    {
        best.way = UnitWay::DROP;  // only a mob drop (cloth, venom sacs): what the guild gives up by not selling it
        best.copper = sell;
    }
    // held by the guild: reserved at the sell price for the share of the need the stock covers
    if (auto const share = ctx.stockShare.find(item); share != ctx.stockShare.end() && share->second > 0.0)
    {
        double const other = std::isfinite(best.copper) ? best.copper : sell;
        best.copper = share->second * sell + (1.0 - share->second) * other;
        if (share->second >= 1.0)
            best.way = UnitWay::STOCK;
    }
    ctx.memo[item] = best;
    return best;
}

double GuildProfPath::CraftCopper(PriceCtx& ctx, ProfRecipe const& recipe, double& rawUnits) const
{
    double copper = 0.0;
    std::map<uint32, double> raw;
    for (auto const& [reagent, n] : recipe.reagents)
    {
        copper += double(n) * Price(ctx, reagent).copper;
        Expand(ctx, reagent, double(n), raw, nullptr);
    }
    rawUnits = 0.0;
    for (auto const& [item, n] : raw)
        rawUnits += n;
    double value = 0.0;
    if (recipe.product)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(recipe.product);
        double const sell = proto ? double(proto->SellPrice) : 0.0;
        double unit = sell;
        if (auto const need = ctx.model->needed.find(recipe.product); need != ctx.model->needed.end())
        {
            auto const share = ctx.neededShare.find(recipe.product);
            double const s = share == ctx.neededShare.end() ? 1.0 : share->second;
            unit = s * double(need->second.second) + (1.0 - s) * sell;
        }
        value = unit * double(recipe.made);
    }
    // a profitable craft is a free skill point, never a reason to craft more: the net is floored at 0
    return std::max(0.0, copper - value) + double(recipe.castSeconds) * ctx.model->copperPerMinute / 60.0;
}

void GuildProfPath::Expand(PriceCtx& ctx, uint32 item, double count, std::map<uint32, double>& raw,
                           std::map<uint32, double>* intermediates, uint32 depth) const
{
    UnitPrice const p = Price(ctx, item);
    if (depth < MAX_PRICE_DEPTH && p.way == UnitWay::CRAFT && p.recipe)
    {
        if (intermediates)
            (*intermediates)[item] += count;
        for (auto const& [reagent, n] : p.recipe->reagents)
            Expand(ctx, reagent, count * double(n) / double(std::max(0.01f, p.recipe->made)), raw, intermediates,
                   depth + 1);
        return;
    }
    if (depth < MAX_PRICE_DEPTH && p.way == UnitWay::CONVERT && p.raw)
    {
        if (intermediates)
            (*intermediates)[item] += count;
        Expand(ctx, p.raw, count * double(p.rawPerUnit), raw, intermediates, depth + 1);
        return;
    }
    raw[item] += count;
}

bool GuildProfPath::Acquire(ProfMember const& member, uint8 friendMask, ProfRecipe const& recipe,
                            ProfCostModel const& model, ProfRecipeSource& out) const
{
    if ((recipe.raceMask && member.raceMask && !(recipe.raceMask & member.raceMask)) ||
        (recipe.classMask && member.classMask && !(recipe.classMask & member.classMask)))
        return false;
    if (member.known.count(recipe.spellId))
    {
        out = {ProfSourceKind::KNOWN};
        return true;
    }
    bool found = false;
    for (ProfRecipeSource const& s : recipe.sources)
    {
        ProfRecipeSource cand = s;
        bool usable = false;
        switch (s.kind)
        {
            case ProfSourceKind::AUTO:
                usable = true;
                cand.price = 0;
                break;
            case ProfSourceKind::TRAINER:
                usable = (s.friendly & friendMask) && s.reqLevel <= member.level;
                break;
            case ProfSourceKind::VENDOR:
            case ProfSourceKind::VENDOR_LIMITED:
                usable = (s.friendly & friendMask) != 0;
                break;
            case ProfSourceKind::REPUTATION:
            {
                auto const rep = member.reputation.find(s.faction);
                usable = (s.friendly & friendMask) && rep != member.reputation.end() && rep->second >= s.rank;
                break;
            }
            default:
                break;
        }
        // a recipe item the guild already holds is used instead of buying or farming one
        if (s.item && model.stock.count(s.item) && model.stock.at(s.item) > 0)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(s.item);
            cand.kind = ProfSourceKind::GUILD_HELD;
            cand.price = proto ? proto->SellPrice : 0;
            usable = !proto || !proto->RequiredReputationFaction ||
                     (member.reputation.count(proto->RequiredReputationFaction) &&
                      member.reputation.at(proto->RequiredReputationFaction) >= proto->RequiredReputationRank);
        }
        if (usable && (!found || cand.price < out.price))
        {
            out = cand;
            found = true;
        }
    }
    if (!found && !recipe.sources.empty())
        out = recipe.sources.front();  // the best source it cannot use now: listed, never taken
    return found;
}

// ---------------------------------------------------------------------------------------------------------------------
// the path
// ---------------------------------------------------------------------------------------------------------------------
void GuildProfPath::Compute(ProfMember const& member, uint32 skillLine, uint32 target, ProfCostModel const& model,
                            ProfPath& path) const
{
    path = ProfPath();
    path.low = member.low;
    path.skillLine = skillLine;
    auto const own = member.skill.find(skillLine);
    uint32 const from = std::max<uint32>(1, own == member.skill.end() ? 0 : own->second);
    uint32 const levelCap = TrainerCap(skillLine, member.level);
    uint32 const wanted = target ? std::min(target, MAX_SKILL_VALUE) : (levelCap ? levelCap : MAX_SKILL_VALUE);
    uint32 const to = levelCap ? std::min(wanted, levelCap) : wanted;
    path.fromSkill = from;
    path.targetSkill = wanted;
    path.reached = from;
    if (to < wanted)
        if (auto const ranks = ranks_.find(skillLine); ranks != ranks_.end())
            for (Rank const& r : ranks->second)
                if (r.maxSkill > to)
                {
                    path.blocked = Acore::StringFormat("level {} for rank {}", uint32(r.reqLevel), r.maxSkill);
                    break;
                }
    std::vector<ProfRecipe> const* list = Recipes(skillLine);
    if (!list || from >= to)
    {
        if (!list)
            path.blocked = "no recipes";
        return;
    }

    PriceCtx ctx;
    ctx.member = &member;
    ctx.model = &model;
    ctx.friendMask = FriendMask(member.factionTemplate);
    if (!ctx.friendMask)
        ctx.friendMask = FRIEND_ALLIANCE;  // an offline member without a faction: the guild is Alliance

    size_t const n = list->size();
    std::vector<ProfRecipeSource> source(n);
    std::vector<uint8> state(n, 0);  // 0 unusable, 1 in the set, 2 candidate (must be acquired)
    for (size_t i = 0; i < n; ++i)
    {
        ProfRecipe const& r = (*list)[i];
        if (!r.gray || r.reqSkill >= to || r.gray <= from)
            continue;
        if (Acquire(member, ctx.friendMask, r, model, source[i]))
            state[i] = (source[i].kind == ProfSourceKind::KNOWN || source[i].kind == ProfSourceKind::AUTO) ? 1 : 2;
    }

    // two passes: the second prices held mats and needed products by the share of the first path they cover
    std::vector<PointCost> best;
    std::vector<int32> pick;
    std::vector<uint8> taken;
    for (uint32 pass = 0; pass < 2; ++pass)
    {
        ctx.memo.clear();
        std::vector<double> craftCopper(n, INF), craftUnits(n, INF);
        for (size_t i = 0; i < n; ++i)
            if (state[i])
                craftCopper[i] = CraftCopper(ctx, (*list)[i], craftUnits[i]);
        best.assign(to - from, PointCost());
        pick.assign(to - from, -1);
        taken = state;
        auto const apply = [&](size_t i)
        {
            ProfRecipe const& r = (*list)[i];
            for (uint32 v = std::max(from, r.reqSkill); v < std::min(to, r.gray); ++v)
            {
                PointCost const c = PerPoint(r, v, craftCopper[i], craftUnits[i]);
                if (c < best[v - from])
                {
                    best[v - from] = c;
                    pick[v - from] = int32(i);
                }
            }
        };
        for (size_t i = 0; i < n; ++i)
            if (state[i] == 1)
                apply(i);
        // greedy by net saving: a recipe to acquire joins when it saves more than its price
        double const cps = model.copperPerMinute / 60.0;
        while (true)
        {
            double bestNet = 0.0, bestUnits = 0.0;
            int32 chosen = -1;
            for (size_t i = 0; i < n; ++i)
            {
                if (taken[i] != 2)
                    continue;
                ProfRecipe const& r = (*list)[i];
                double copper = 0.0, units = 0.0;
                for (uint32 v = std::max(from, r.reqSkill); v < std::min(to, r.gray); ++v)
                {
                    PointCost const c = PerPoint(r, v, craftCopper[i], craftUnits[i]);
                    PointCost const& cur = best[v - from];
                    if (!std::isfinite(c.copper))
                        continue;
                    if (!std::isfinite(cur.copper))
                        copper += NO_RECIPE;
                    else if (c < cur)
                    {
                        copper += cur.copper - c.copper;
                        units += cur.units - c.units;
                    }
                }
                double net = copper - double(source[i].price);
                // time unpriced: a trainer recipe that only saves crafts and gathering is worth its fee
                if (net <= 0.0 && cps <= 0.0 && source[i].kind == ProfSourceKind::TRAINER && units > 0.0 &&
                    copper >= 0.0)
                    net = units;
                if (net > bestNet || (net == bestNet && net > 0.0 && units > bestUnits))
                {
                    bestNet = net;
                    bestUnits = units;
                    chosen = int32(i);
                }
            }
            if (chosen < 0)
                break;
            taken[size_t(chosen)] = 3;
            apply(size_t(chosen));
        }
        if (pass)
            break;
        // first-pass needs -> shares
        std::map<uint32, double> raw, made;
        for (uint32 v = from; v < to; ++v)
            if (pick[v - from] >= 0)
            {
                ProfRecipe const& r = (*list)[size_t(pick[v - from])];
                double const crafts = best[v - from].crafts;
                for (auto const& [reagent, k] : r.reagents)
                    Expand(ctx, reagent, crafts * double(k), raw, &made);
                if (r.product)
                    made[r.product] += crafts * double(r.made);
            }
        for (auto const& [item, need] : raw)
            if (auto const s = model.stock.find(item); s != model.stock.end() && need > 0.0)
                ctx.stockShare[item] = std::min(1.0, double(s->second) / need);
        for (auto const& [item, count] : made)
            if (auto const s = model.stock.find(item); s != model.stock.end() && count > 0.0)
                ctx.stockShare[item] = std::min(1.0, double(s->second) / count);
        for (auto const& [product, need] : model.needed)
            if (auto const m = made.find(product); m != made.end() && m->second > 0.0)
                ctx.neededShare[product] = std::min(1.0, double(need.first) / m->second);
    }

    // steps
    for (uint32 v = from; v < to; ++v)
    {
        int32 const i = pick[v - from];
        if (i < 0)
        {
            if (path.blocked.empty())
                path.blocked = Acore::StringFormat("no recipe at {}", v);
            break;
        }
        ProfRecipe const& r = (*list)[size_t(i)];
        PointCost const& c = best[v - from];
        if (path.steps.empty() || path.steps.back().spellId != r.spellId || path.steps.back().toSkill != v)
        {
            ProfStep step;
            step.spellId = r.spellId;
            step.fromSkill = v;
            path.steps.push_back(step);
        }
        ProfStep& step = path.steps.back();
        step.toSkill = v + 1;
        step.crafts += c.crafts;
        step.copper += c.copper;
        step.castSeconds += c.crafts * double(r.castSeconds);
        for (auto const& [reagent, k] : r.reagents)
            step.reagents[reagent] += c.crafts * double(k);
        path.reached = v + 1;
    }

    // totals, mats, products, tools, recipes
    std::map<uint32, double> raw;
    for (ProfStep const& step : path.steps)
    {
        ProfRecipe const* r = Recipe(step.spellId);
        path.copper += step.copper;
        path.castSeconds += step.castSeconds;
        path.crafts += step.crafts;
        for (auto const& [reagent, count] : step.reagents)
            Expand(ctx, reagent, count, raw, &path.intermediates);
        if (r && r->product)
            path.leftovers[r->product] += step.crafts * double(r->made);
        if (r)
        {
            for (uint32 t : r->totems)
                if (!member.toolItems.count(t))
                    path.tools[t] = 1.0;
            for (uint32 cat : r->totemCategories)
                if (!member.toolCategories.count(cat))
                    if (auto const items = totemItems_.find(cat); items != totemItems_.end() && !items->second.empty())
                    {
                        bool held = false;
                        for (uint32 t : items->second)
                            held = held || path.tools.count(t);
                        if (!held)
                            path.tools[items->second.front()] = 1.0;
                    }
        }
    }
    // products used as reagents later in the path are no leftovers
    for (auto const& [item, count] : path.intermediates)
        if (auto const left = path.leftovers.find(item); left != path.leftovers.end())
            left->second = std::max(0.0, left->second - count);
    for (auto const& [item, count] : raw)
    {
        ProfMatSource& m = path.mats[item];
        UnitPrice const p = Price(ctx, item);
        m.count = count;
        m.unitCopper = p.copper;
        m.gatherSkill = p.gatherSkill;
        auto const s = model.stock.find(item);
        m.fromStock = s == model.stock.end() ? 0.0 : std::min(count, double(s->second));
        double const rest = count - m.fromStock;
        switch (p.way)
        {
            case UnitWay::BUY: m.toBuy = rest; break;
            case UnitWay::GATHER: m.toGather = rest; break;
            case UnitWay::DISENCHANT: m.toDisenchant = rest; break;
            default: m.fromDrops = rest; break;
        }
        m.why = Acore::StringFormat("{} #{} {}-{}", SkillLineName(skillLine), member.low, path.fromSkill,
                                      path.reached);
    }
    for (size_t i = 0; i < n; ++i)
    {
        if (taken[i] == 3)
        {
            ProfMissingRecipe m;
            m.spellId = (*list)[i].spellId;
            m.source = source[i];
            m.taken = true;
            path.fees += double(source[i].price);
            path.recipes.push_back(m);
        }
    }
    // rare recipes it cannot use now that would save copper or fill a gap: buy / farm / reputation tasks
    for (size_t i = 0; i < n; ++i)
    {
        ProfRecipe const& r = (*list)[i];
        if (state[i] || !r.gray || r.reqSkill >= to || r.gray <= from || r.discovery)
            continue;
        double units = 0.0;
        double const copper = CraftCopper(ctx, r, units);
        if (!std::isfinite(copper))
            continue;
        double saving = 0.0;
        bool gap = false;
        for (uint32 v = std::max(from, r.reqSkill); v < std::min(to, r.gray); ++v)
        {
            PointCost const c = PerPoint(r, v, copper, units);
            if (!std::isfinite(c.copper))
                continue;
            if (pick[v - from] < 0)
                gap = true;
            else if (c < best[v - from])
                saving += best[v - from].copper - c.copper;
        }
        if (!gap && saving <= 0.0)
            continue;
        ProfMissingRecipe m;
        m.spellId = r.spellId;
        Acquire(member, ctx.friendMask, r, model, m.source);
        m.saving = saving - double(m.source.price);
        m.fillsGap = gap;
        path.recipes.push_back(m);
    }
    // the trainer ranks on the way
    if (auto const ranks = ranks_.find(skillLine); ranks != ranks_.end())
    {
        auto const cap = member.maxSkill.find(skillLine);
        uint32 const have = cap == member.maxSkill.end() ? 0 : cap->second;
        for (Rank const& r : ranks->second)
            if (r.maxSkill > have && r.maxSkill < path.reached + SKILL_STEP_MAX)  // the previous cap was passed
            {
                path.ranks.push_back({r.reqSkill, r.maxSkill, r.reqLevel, r.price});
                path.fees += double(r.price);
            }
    }
}

uint64 GuildProfPath::ModelKey(ProfCostModel const& model)
{
    // coarse: small stock changes keep the cached path (log2 buckets), a new pace or need recomputes it
    auto const bucket = [](double v) { return v <= 0.0 ? 0 : uint64(std::log2(v + 1.0) * 2.0); };
    uint64 h = Mix(0, bucket(model.copperPerMinute));
    h = Mix(h, bucket(model.gatherCopperPerMinute));
    for (auto const& [skill, pace] : model.gatherPace)
        h = Mix(Mix(h, skill), bucket(pace * 10.0));
    for (auto const& [item, count] : model.stock)
        h = Mix(Mix(h, item), bucket(count));
    for (auto const& [item, need] : model.needed)
        h = Mix(Mix(Mix(h, item), bucket(need.first)), bucket(need.second));
    return Mix(h, model.spareItems.size());
}

ProfPath GuildProfPath::PlanPath(ProfMember const& member, uint32 skillLine, uint32 targetSkill,
                                 ProfCostModel const& model)
{
    uint64 const key = Mix(Mix(member.Fingerprint(skillLine), ModelKey(model)), targetSkill);
    uint64 const slot = (uint64(member.low) << 16) | skillLine;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (auto const itr = cache_.find(slot); itr != cache_.end() && itr->second.key == key)
            return itr->second.path;
    }
    uint32 const start = getMSTime();
    ProfPath path;
    if (loaded_)
        Compute(member, skillLine, targetSkill, model, path);
    path.fingerprint = key;
    path.computedMs = GetMSTimeDiffToNow(start);
    LOG_DEBUG("playerbots", "[prof] {}", PathText(path, 4));
    std::lock_guard<std::mutex> guard(lock_);
    cache_[slot] = {path, key};
    return path;
}

bool GuildProfPath::CachedPath(uint32 low, uint32 skillLine, ProfPath& out) const
{
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = cache_.find((uint64(low) << 16) | skillLine);
    if (itr == cache_.end())
        return false;
    out = itr->second.path;
    return true;
}

uint32 GuildProfPath::NextRecipe(uint32 low, uint32 skillLine, uint32 skill) const
{
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = cache_.find((uint64(low) << 16) | skillLine);
    if (itr == cache_.end())
        return 0;
    for (ProfStep const& step : itr->second.path.steps)
        if (skill >= step.fromSkill && skill < step.toSkill)
            return step.spellId;
    return 0;
}

void GuildProfPath::Invalidate(uint32 low, uint32 skillLine)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (skillLine)
    {
        cache_.erase((uint64(low) << 16) | skillLine);
        return;
    }
    for (auto itr = cache_.begin(); itr != cache_.end();)
        itr = (itr->first >> 16) == low ? cache_.erase(itr) : std::next(itr);
}

std::map<uint32, ProfMatSource> GuildProfPath::NeededMats() const
{
    std::map<uint32, ProfMatSource> out;
    std::lock_guard<std::mutex> guard(lock_);
    for (auto const& [slot, entry] : cache_)
        for (auto const& [item, m] : entry.path.mats)
        {
            double const rest = m.count - m.fromStock;
            if (rest <= 0.0)
                continue;
            ProfMatSource& o = out[item];
            o.count += rest;
            o.toBuy += m.toBuy;
            o.toGather += m.toGather;
            o.toDisenchant += m.toDisenchant;
            o.fromDrops += m.fromDrops;
            o.unitCopper = m.unitCopper;
            o.gatherSkill = m.gatherSkill;
            o.why += (o.why.empty() ? "" : ", ") + m.why + Acore::StringFormat(" x{:.0f}", rest);
        }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// gear crafts, enchants, reputations (scope additions)
// ---------------------------------------------------------------------------------------------------------------------
bool GuildProfPath::PlanCraft(uint32 product, std::vector<ProfMember> const& crafters, ProfCostModel const& model,
                              ProfCraftPlan& out)
{
    bool found = false;
    for (ProfRecipe const* r : RecipesFor(product))
        for (ProfMember const& m : crafters)
        {
            ProfRecipeSource src;
            if (r->discovery && !m.known.count(r->spellId))
                continue;
            if (!Acquire(m, FriendMask(m.factionTemplate) ? FriendMask(m.factionTemplate) : FRIEND_ALLIANCE, *r, model,
                         src) &&
                src.kind != ProfSourceKind::DROP && src.kind != ProfSourceKind::WORLD_DROP &&
                src.kind != ProfSourceKind::REPUTATION && src.kind != ProfSourceKind::VENDOR_TOKENS)
                continue;  // drops / reputation / tokens stay possible: the leader makes them tasks
            auto const own = m.skill.find(r->skillLine);
            uint32 const skill = own == m.skill.end() ? 0 : own->second;
            double copper = double(src.price);
            if (skill < r->reqSkill)
            {
                if (TrainerCap(r->skillLine, m.level) < r->reqSkill)
                    continue;
                ProfPath const path = PlanPath(m, r->skillLine, r->reqSkill, model);
                if (path.reached < r->reqSkill)
                    continue;
                copper += path.copper + path.fees;
            }
            PriceCtx ctx;
            ctx.member = &m;
            ctx.model = &model;
            ctx.friendMask = FriendMask(m.factionTemplate) ? FriendMask(m.factionTemplate) : FRIEND_ALLIANCE;
            std::map<uint32, double> mats;
            for (auto const& [reagent, n] : r->reagents)
            {
                copper += double(n) * Price(ctx, reagent).copper / double(std::max(0.01f, r->made));
                Expand(ctx, reagent, double(n) / double(std::max(0.01f, r->made)), mats, nullptr);
            }
            if (!std::isfinite(copper) || (found && copper >= out.copper))
                continue;
            out = ProfCraftPlan();
            out.product = product;
            out.spellId = r->spellId;
            out.crafterLow = m.low;
            out.copper = copper;
            out.recipeSources = r->sources;
            out.mats = std::move(mats);
            out.skillNeeded = r->reqSkill;
            found = true;
        }
    return found;
}

void GuildProfPath::ReputationNeeds(std::vector<std::pair<uint32, uint32>> const& itemsOrSpells, uint32 forLow,
                                    std::vector<ProfReputationNeed>& out) const
{
    // pairs: (item, 0) for a bought item (arcanum, shoulder inscription, gem), (0, spell) for a recipe to learn
    auto const add = [&](uint32 faction, uint32 rank, uint32 item, uint32 spell)
    {
        FactionEntry const* entry = sFactionStore.LookupEntry(faction);
        ProfReputationNeed need;
        need.faction = faction;
        need.rank = rank;
        need.forItem = item;
        need.forSpell = spell;
        need.forLow = forLow;
        need.reason = Acore::StringFormat("{} {} -> {} for #{}", entry ? entry->name[LOCALE_enUS] : "?",
                                            RankName(rank), item ? ItemName(item) : SpellName(spell), forLow);
        out.push_back(need);
    };
    for (auto const& [item, spell] : itemsOrSpells)
    {
        if (item)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item))
                if (proto->RequiredReputationFaction)
                    add(proto->RequiredReputationFaction, proto->RequiredReputationRank, item, 0);
        if (spell)
            if (ProfRecipe const* r = Recipe(spell))
                for (ProfRecipeSource const& s : r->sources)
                    if (s.kind == ProfSourceKind::REPUTATION)
                    {
                        add(s.faction, s.rank, 0, spell);
                        break;
                    }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// text
// ---------------------------------------------------------------------------------------------------------------------
std::string GuildProfPath::SourceText(ProfRecipeSource const& s)
{
    switch (s.kind)
    {
        case ProfSourceKind::KNOWN: return "known";
        case ProfSourceKind::AUTO: return "auto";
        case ProfSourceKind::TRAINER: return Acore::StringFormat("trainer {} {}", s.entry, Money(s.price));
        case ProfSourceKind::VENDOR: return Acore::StringFormat("vendor {} {}", s.entry, Money(s.price));
        case ProfSourceKind::VENDOR_LIMITED:
            return Acore::StringFormat("vendor {} {} (limited {} / {} s)", s.entry, Money(s.price), s.maxCount,
                                         s.incrTime);
        case ProfSourceKind::VENDOR_TOKENS: return Acore::StringFormat("vendor {} (tokens)", s.entry);
        case ProfSourceKind::REPUTATION:
        {
            FactionEntry const* entry = sFactionStore.LookupEntry(s.faction);
            return Acore::StringFormat("vendor {} {} ({} {})", s.entry, Money(s.price),
                                         entry ? entry->name[LOCALE_enUS] : "?", RankName(s.rank));
        }
        case ProfSourceKind::DROP: return Acore::StringFormat("drop loot {} {:.2f}%", s.entry, s.chance);
        case ProfSourceKind::WORLD_DROP: return "world drop";
        case ProfSourceKind::QUEST: return Acore::StringFormat("quest {}", s.entry);
        case ProfSourceKind::DISCOVERY: return "discovery";
        case ProfSourceKind::GUILD_HELD: return Acore::StringFormat("guild holds item {}", s.item);
        default: return "none";
    }
}

std::string GuildProfPath::PathText(ProfPath const& path, size_t maxSteps) const
{
    std::ostringstream out;
    out << SkillLineName(path.skillLine) << " #" << path.low << " " << path.fromSkill << "->" << path.reached << "/"
        << path.targetSkill;
    if (!path.blocked.empty())
        out << " (" << path.blocked << ")";
    out << ": " << uint32(path.crafts + 0.5) << " crafts, " << Money(path.copper) << " + fees " << Money(path.fees)
        << ", cast " << uint32(path.castSeconds / 60.0 + 0.5) << " min;";
    for (size_t i = 0; i < path.steps.size() && i < maxSteps; ++i)
    {
        ProfStep const& s = path.steps[i];
        out << " " << s.fromSkill << "-" << s.toSkill << " " << SpellName(s.spellId) << " x" << uint32(s.crafts + 0.5)
            << ";";
    }
    if (path.steps.size() > maxSteps)
        out << " ...";
    return out.str();
}
