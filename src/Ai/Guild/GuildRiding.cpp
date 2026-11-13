/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildRiding.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "Bag.h"
#include "ConditionMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "GossipDef.h"
#include "GuildCoordinator.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "QuestStats.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    constexpr uint8 APPRENTICE_RIDING_LEVEL = 20;  // trainer_spell ReqLevel of Apprentice Riding and the mounts'

    // a riding trainer spawn that teaches Apprentice Riding, or a vendor spawn that sells an apprentice mount
    struct Spawn
    {
        uint32 entry{0};
        WorldPosition pos{};
        uint32 faction{0};
        uint32 price{0};  // the trainer's riding price (a vendor: per item, ItemTemplate::BuyPrice)
        std::vector<uint32> items;  // a vendor's apprentice mounts
    };

    struct Index
    {
        std::vector<Spawn> trainers;
        std::vector<Spawn> vendors;
    };

    Index const& GetIndex()
    {
        static Index index;
        static std::once_flag built;
        std::call_once(built, []()
        {
            // [mount7] an event's spawns stand only while it runs (b470: Thokhen's plan took the Brewfest Ram, 10 g,
            // from a Brewfest vendor that is not there - 13.6 g asked of 12 members)
            std::unordered_set<ObjectGuid::LowType> eventSpawns;
            for (auto const& guids : sGameEventMgr->GameEventCreatureGuids)
                eventSpawns.insert(guids.begin(), guids.end());
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
                MapEntry const* map = sMapStore.LookupEntry(data.mapid);
                if (!tmpl || !map || map->Instanceable() || eventSpawns.count(spawnId))
                    continue;
                Spawn spawn;
                spawn.entry = data.id;
                spawn.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
                spawn.faction = tmpl->faction;
                if (Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(data.id))
                    if (trainer->GetTrainerType() == Trainer::Type::Mount)
                        if (Trainer::Spell const* riding = trainer->GetSpell(GuildRiding::SPELL_APPRENTICE_RIDING))
                        {
                            Spawn t = spawn;
                            t.price = riding->MoneyCost;
                            index.trainers.push_back(t);
                        }
                if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id))
                {
                    Spawn v = spawn;
                    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
                    {
                        VendorItem const* item = items->GetItem(slot);
                        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
                        // the apprentice mounts: the first riding rank, sold for gold
                        if (proto && !item->ExtendedCost && GuildRiding::IsMountItem(proto) &&
                            proto->RequiredSkill == SKILL_RIDING && proto->RequiredSkillRank <= 75 && proto->BuyPrice)
                            v.items.push_back(proto->ItemId);
                    }
                    if (!v.items.empty())
                        index.vendors.push_back(v);
                }
            }
            LOG_INFO("playerbots", "[mount1] riding index: {} riding trainer spawns, {} apprentice mount vendor spawns",
                     index.trainers.size(), index.vendors.size());
        });
        return index;
    }

    bool Hostile(Player* bot, uint32 faction)
    {
        FactionTemplateEntry const* npc = sFactionTemplateStore.LookupEntry(faction);
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        return !npc || !own || npc->IsHostileTo(*own);
    }

    // the vendor's gossip sells to the bot: its vendor option's conditions (the breeders' race or exalted rule)
    bool SellsTo(Player* bot, uint32 entry)
    {
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        if (!tmpl)
            return false;
        bool option = false;
        auto const bounds = sObjectMgr->GetGossipMenuItemsMapBounds(tmpl->GossipMenuId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            if (itr->second.OptionType != GOSSIP_OPTION_VENDOR)
                continue;
            option = true;
            if (sConditionMgr->IsObjectMeetToConditions(bot, itr->second.Conditions))
                return true;
        }
        return !option;
    }

    // the speed gain and cast seconds of a mount spell; false: no ground mount
    bool MountSpellFacts(SpellInfo const* info, Player* caster, float& bonus, float& castSec)
    {
        if (!info || info->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
            return false;
        bonus = 0.0f;
        for (SpellEffectInfo const& effect : info->Effects)
        {
            if (effect.ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
                return false;
            if (effect.ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
                bonus = std::max(bonus, float(effect.CalcValue()) / 100.0f);
        }
        castSec = float(info->CalcCastTime(caster)) / float(IN_MILLISECONDS);
        return bonus > 0.0f;
    }

    // the mount item's taught spell (the "Learning" layout: Spells[1])
    uint32 TaughtSpell(ItemTemplate const* proto)
    {
        return proto && proto->Spells[1].SpellId > 0 ? uint32(proto->Spells[1].SpellId) : 0;
    }

    Item* HeldMountItem(Player* p)
    {
        auto const fits = [p](Item* i)
        {
            ItemTemplate const* proto = i ? i->GetTemplate() : nullptr;
            uint32 const taught = TaughtSpell(proto);
            return GuildRiding::IsMountItem(proto) && taught && !p->HasSpell(taught);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* i = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); fits(i))
                return i;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* i = bag->GetItemByPos(uint8(j)); fits(i))
                        return i;
        return nullptr;
    }

    struct Plan
    {
        bool due{false};       // L20+, no ground mount yet, a trainer and a vendor for it on its map
        bool rides{false};     // a ground mount it can ride
        bool riding{false};    // Apprentice Riding learned
        bool heldMount{false};  // a mount item in its bags to use
        uint32 trainerEntry{0};
        WorldPosition trainerPos{};
        uint32 trainCost{0};
        uint32 mountItem{0};
        uint32 vendorEntry{0};
        WorldPosition vendorPos{};
        uint32 mountCost{0};
        float savedMinPerHour{0.0f};
        float copperPerHour{0.0f};
        uint32 spare{0};  // its spare money at the last order update
        std::string why;  // not due: why
        uint32 Need() const { return (riding ? 0 : trainCost) + (heldMount ? 0 : mountCost); }
    };

    std::mutex lock;
    std::unordered_map<uint32, Plan> plans;
    std::unordered_map<uint32, std::string> logged;  // member -> the last [mount4] state
    std::unordered_map<uint32, uint32> learning;     // member -> the mount spell its item's use is to teach
    size_t next = 0;
    std::atomic<uint32> flights{0};
    std::atomic<uint64> flightFares{0};
    std::atomic<uint64> flightSaved{0};
    std::string lastGuildLine;

    uint32 KnownNodes(Player* p)
    {
        uint32 n = 0;
        for (uint32 i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
            if (sTaxiNodesStore.LookupEntry(i) && p->m_taxi.IsTaximaskNodeKnown(i))
                ++n;
        return n;
    }

    Player* Member(uint32 low)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        return p && p->IsInWorld() ? p : nullptr;
    }

    // one member's plan: its trainer and vendor, its value (world thread, no coordinator lock held)
    Plan Compute(Player* p)
    {
        Plan plan;
        GuildRiding::GroundMount const own = GuildRiding::BestGroundMount(p);
        plan.rides = own.spellId != 0;
        plan.riding = p->HasSkill(SKILL_RIDING);
        Item const* held = HeldMountItem(p);
        plan.heldMount = held != nullptr;
        if (plan.rides)
        {
            plan.why = "rides already";
            return plan;
        }
        Index const& index = GetIndex();
        // the nearest trainer that teaches it now (level, the trainer's requirement), its price after the discount
        float bestTrainer = FLT_MAX;
        for (Spawn const& t : index.trainers)
        {
            if (plan.riding)
                break;
            if (t.pos.GetMapId() != p->GetMapId() || Hostile(p, t.faction))
                continue;
            Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(t.entry);
            Trainer::Spell const* spell = trainer ? trainer->GetSpell(GuildRiding::SPELL_APPRENTICE_RIDING) : nullptr;
            if (!spell || !trainer->IsTrainerValidForPlayer(p) || !trainer->CanTeachSpell(p, spell))
                continue;
            float const d = p->GetExactDist(t.pos);
            if (d < bestTrainer)
            {
                bestTrainer = d;
                plan.trainerEntry = t.entry;
                plan.trainerPos = t.pos;
                plan.trainCost =
                    uint32(float(spell->MoneyCost) *
                           p->GetReputationPriceDiscount(sFactionTemplateStore.LookupEntry(t.faction)));
            }
        }
        // the cheapest mount a vendor on its map sells it (its race / reputation), the nearest of equal price
        ItemTemplate const* mountProto = held ? held->GetTemplate() : nullptr;
        float bestVendor = FLT_MAX;
        for (Spawn const& v : index.vendors)
        {
            if (plan.heldMount)
                break;
            if (v.pos.GetMapId() != p->GetMapId() || Hostile(p, v.faction) || !SellsTo(p, v.entry))
                continue;
            float const discount = p->GetReputationPriceDiscount(sFactionTemplateStore.LookupEntry(v.faction));
            float const d = p->GetExactDist(v.pos);
            for (uint32 itemId : v.items)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto || !(proto->AllowableRace & p->getRaceMask()) ||
                    !(proto->AllowableClass & p->getClassMask()))
                    continue;
                uint32 const price = uint32(float(proto->BuyPrice) * discount);
                if (!plan.mountItem || price < plan.mountCost || (price == plan.mountCost && d < bestVendor))
                {
                    bestVendor = d;
                    plan.mountItem = itemId;
                    plan.vendorEntry = v.entry;
                    plan.vendorPos = v.pos;
                    plan.mountCost = price;
                    mountProto = proto;
                }
            }
        }
        if (p->GetLevel() < APPRENTICE_RIDING_LEVEL && !plan.riding)
            plan.why = "below the riding level";
        else if (!plan.riding && !plan.trainerEntry)
            plan.why = "no riding trainer on its map that teaches it now";
        else if (!plan.heldMount && !plan.mountItem)
            plan.why = "no mount vendor on its map that sells to it (its race's breeder stands elsewhere)";
        // the value: its walking on foot sped up by the mount, a mount cast per walk begun, at its quest pace
        float bonus = 0.0f, castSec = 0.0f;
        if (!MountSpellFacts(sSpellMgr->GetSpellInfo(TaughtSpell(mountProto)), p, bonus, castSec))
        {
            if (plan.why.empty())
                plan.why = "its mount's speed is unknown";
            return plan;
        }
        uint64 footMs = 0, trackedMs = 0;
        uint32 starts = 0;
        if (!QuestStats::FootTravel(p->GetGUID().GetCounter(), footMs, trackedMs, starts))
        {
            if (plan.why.empty())
                plan.why = "no travel measured yet";
            return plan;
        }
        float const hours = float(trackedMs) / float(HOUR * IN_MILLISECONDS);
        float const savedSec =
            float(footMs) / float(IN_MILLISECONDS) * bonus / (1.0f + bonus) - float(starts) * castSec;
        plan.savedMinPerHour = hours > 0.0f ? std::max(0.0f, savedSec / hours / 60.0f) : 0.0f;
        plan.copperPerHour = plan.savedMinPerHour * sGuildCoordinator.QuestCopperPerMinute(p);
        if (plan.why.empty() && plan.savedMinPerHour <= 0.0f)
            plan.why = "riding saves it no time (it walks too little)";
        plan.due = plan.why.empty();
        return plan;
    }

    // the bought mount item used as a player does (CMSG_USE_ITEM: the core's Learning cast)
    void UseMountItem(Player* p, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 const spellId = uint32(std::max<int32>(proto->Spells[0].SpellId, 0));
        if (p->IsInCombat() || p->IsNonMeleeSpellCast(false) || p->IsInFlight() || p->IsMounted() ||
            p->CanUseItem(item) != EQUIP_ERR_OK)
            return;
        if (SpellInfo const* use = sSpellMgr->GetSpellInfo(spellId))
            if (p->GetGlobalCooldownMgr().HasGlobalCooldown(use))
                return;
        if (p->isMoving())
            p->StopMoving();  // the Learning cast breaks off on movement
        WorldPacket packet(CMSG_USE_ITEM);
        packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0)
               << uint8(0) << uint32(TARGET_FLAG_NONE);
        p->GetSession()->HandleUseItemOpcode(packet);
        std::lock_guard<std::mutex> guard(lock);
        if (learning.emplace(p->GetGUID().GetCounter(), TaughtSpell(proto)).second)
            LOG_INFO("playerbots", "[mount3] {} uses {} to learn its mount", p->GetName(), proto->Name1);
    }
}  // namespace

namespace GuildRiding
{
GroundMount BestGroundMount(Player* bot)
{
    struct Cached
    {
        size_t spells{SIZE_MAX};
        uint16 skill{0};
        GroundMount mount;
    };
    thread_local std::unordered_map<uint32, Cached> cache;
    Cached& c = cache[bot->GetGUID().GetCounter()];
    uint16 const skill = bot->GetPureSkillValue(SKILL_RIDING);
    if (c.spells == bot->GetSpellMap().size() && c.skill == skill)
        return c.mount;
    c.spells = bot->GetSpellMap().size();
    c.skill = skill;
    c.mount = GroundMount();
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        float bonus = 0.0f, castSec = 0.0f;
        if (!info || info->IsPassive() || !MountSpellFacts(info, bot, bonus, castSec) ||
            PlayerbotSpellRepository::Instance().GetRequiredRidingSkill(info) > skill)
            continue;
        if (bonus > c.mount.bonus)
            c.mount = {spellId, bonus, castSec};
    }
    return c.mount;
}

float TravelSpeed(Player* bot)
{
    float const run = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    if (bot->IsMounted() || bot->GetMap()->Instanceable())
        return run;
    GroundMount const mount = BestGroundMount(bot);
    return mount.spellId ? run * (1.0f + mount.bonus) : run;
}

float MountPaysFromYd(Player* bot, GroundMount const& mount)
{
    if (mount.bonus <= 0.0f)
        return FLT_MAX;
    float const run = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    // d / run - d / (run * (1 + b)) = cast  =>  d = cast * run * (1 + b) / b
    return mount.castSec * run * (1.0f + mount.bonus) / mount.bonus;
}

bool IsMountItem(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_MISC && proto->SubClass == ITEM_SUBCLASS_JUNK_MOUNT;
}

std::string Money(uint32 copper)
{
    uint32 const g = copper / GOLD, s = (copper % GOLD) / SILVER, c = copper % SILVER;
    std::string text;
    if (g)
        text += std::to_string(g) + "g";
    if (s || (g && c))
        text += std::to_string(s) + "s";
    if (c || text.empty())
        text += std::to_string(c) + "c";
    return text;
}

float SavedMinutesPerHour(uint32 low)
{
    std::lock_guard<std::mutex> guard(lock);
    auto const itr = plans.find(low);
    return itr != plans.end() ? itr->second.savedMinPerHour : 0.0f;
}

void Update()
{
    std::vector<uint32> const lows = sGuildCoordinator.PlannedLows();
    if (lows.empty())
        return;
    // a. one member's plan per step (its trainer, vendor and value)
    if (next >= lows.size())
        next = 0;
    uint32 const low = lows[next++];
    Player* p = Member(low);
    if (p && GET_PLAYERBOT_AI(p) && !GuildCoordinator::HumanCharacter(p))
    {
        Plan plan = Compute(p);
        std::lock_guard<std::mutex> guard(lock);
        plan.spare = plans[low].spare;
        plans[low] = plan;
    }
    else
    {
        std::lock_guard<std::mutex> guard(lock);
        plans.erase(low);
    }
    // b. a mount item in a member's bags: used now; c. the guild line
    uint32 online = 0, riding = 0, mounts = 0, nodes = 0, minNodes = UINT32_MAX, maxNodes = 0, eligible = 0;
    for (uint32 member : lows)
    {
        Player* m = Member(member);
        if (!m)
            continue;
        ++online;
        eligible += m->GetLevel() >= APPRENTICE_RIDING_LEVEL ? 1 : 0;
        riding += m->HasSkill(SKILL_RIDING) ? 1 : 0;
        mounts += BestGroundMount(m).spellId ? 1 : 0;
        uint32 const known = KnownNodes(m);
        nodes += known;
        minNodes = std::min(minNodes, known);
        maxNodes = std::max(maxNodes, known);
        if (GET_PLAYERBOT_AI(m) && !GuildCoordinator::HumanCharacter(m) && m->HasSkill(SKILL_RIDING) && m->IsAlive())
            if (Item* item = HeldMountItem(m))
                UseMountItem(m, item);
        std::lock_guard<std::mutex> guard(lock);
        if (auto const used = learning.find(member); used != learning.end() && m->HasSpell(used->second))
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(used->second);
            LOG_INFO("playerbots", "[mount3] {} learned {}: rides from now on", m->GetName(),
                     info ? info->SpellName[0] : "?");
            learning.erase(used);
        }
    }
    uint32 due = 0, affordable = 0;
    uint64 shortGold = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto const& [member, plan] : plans)
            if (plan.due)
            {
                ++due;
                if (plan.spare >= plan.Need())
                    ++affordable;
                else
                    shortGold += plan.Need() - plan.spare;
            }
    }
    if (!online)
        return;
    std::string const counts = Acore::StringFormat("{}/{}/{}/{}/{}/{}/{}", online, riding, mounts, nodes, due,
                                                   affordable, flights.load());
    if (counts == lastGuildLine)
        return;
    lastGuildLine = counts;
    LOG_INFO("playerbots",
             "[mount6] guild travel: {} members online ({} at L20+): {} with Apprentice Riding, {} with a ground "
             "mount; flight paths known {:.1f} on average ({}-{}); riding planned for {} members ({} can pay now, "
             "the rest short {} together); {} purposeful flights, fares {}, {:.0f} min saved",
             online, eligible, riding, mounts, float(nodes) / float(online), minNodes, maxNodes, due, affordable,
             Money(uint32(std::min<uint64>(shortGold, UINT32_MAX))), flights.load(),
             Money(uint32(std::min<uint64>(flightFares.load(), UINT32_MAX))), float(flightSaved.load()) / 60.0f);
}

void AddOrders(Player* p, uint32 spare, std::vector<GuildOrder>& orders)
{
    uint32 const low = p->GetGUID().GetCounter();
    Plan plan;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const itr = plans.find(low);
        if (itr == plans.end())
            return;
        itr->second.spare = spare;
        plan = itr->second;
    }
    // the live facts: what it learned or bought since its plan
    bool const riding = p->HasSkill(SKILL_RIDING);
    bool const held = HeldMountItem(p) != nullptr;
    if (BestGroundMount(p).spellId)
        return;
    if (!plan.due)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (logged[low] != plan.why)
        {
            logged[low] = plan.why;
            LOG_INFO("playerbots", "[mount4] the global leader: {} L{} no riding yet: {}", p->GetName(), p->GetLevel(),
                     plan.why);
        }
        return;
    }
    plan.riding = riding;
    plan.heldMount = held;
    uint32 const need = plan.Need();
    float const payback = plan.copperPerHour > 0.0f ? float(need) / plan.copperPerHour : 0.0f;
    std::string const value =
        Acore::StringFormat("saves {:.1f} min/h ({:.0f}c/h at its quest pace{})", plan.savedMinPerHour,
                            plan.copperPerHour,
                            payback > 0.0f ? Acore::StringFormat(", paid back in {:.1f} h", payback) : "");
    std::string decision;
    char const* key = "waits";
    if (!riding && spare >= need)
    {
        CreatureTemplate const* trainer = sObjectMgr->GetCreatureTemplate(plan.trainerEntry);
        GuildOrder o;
        o.type = GuildOrderType::BUY;
        o.spellId = SPELL_APPRENTICE_RIDING;
        o.count = 1;
        o.npcEntry = plan.trainerEntry;
        o.pos = plan.trainerPos;
        o.cost = plan.trainCost;
        o.details = std::string("Apprentice Riding at ") + (trainer ? trainer->Name : "?") + ", price " +
                    Money(plan.trainCost) + " — " + value;
        o.reason = "[mount1] riding and its mount (" + Money(need) + ") from its spare money " + Money(spare);
        o.status = "to the riding trainer";
        o.executable = true;
        orders.push_back(o);
        key = "trains";
        decision = "trains Apprentice Riding at " + std::string(trainer ? trainer->Name : "?") + " (" +
                   Money(plan.trainCost) + "), then its mount (" + Money(plan.mountCost) + ")";
    }
    else if (riding && !held && plan.mountItem && spare >= plan.mountCost)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(plan.mountItem);
        CreatureTemplate const* vendor = sObjectMgr->GetCreatureTemplate(plan.vendorEntry);
        GuildOrder o;
        o.type = GuildOrderType::BUY;
        o.itemId = plan.mountItem;
        o.count = 1;
        o.npcEntry = plan.vendorEntry;
        o.pos = plan.vendorPos;
        o.cost = plan.mountCost;
        o.details = std::string(proto ? proto->Name1 : "?") + " for itself — " + (vendor ? vendor->Name : "?") +
                    ", price " + Money(plan.mountCost) + " — " + value;
        o.reason = "[mount2] its mount, with its own money";
        o.status = "to the vendor";
        o.executable = true;
        orders.push_back(o);
        key = "buys";
        decision = "buys " + std::string(proto ? proto->Name1 : "?") + " at " + (vendor ? vendor->Name : "?") +
                   " (" + Money(plan.mountCost) + ")";
    }
    else if (riding && held)
    {
        key = "uses";
        decision = "uses its mount item";
    }
    else
        decision = "waits for gold: needs " + Money(need) + ", spare " + Money(spare);
    std::string const line = Acore::StringFormat("{} L{} {}; {}", p->GetName(), p->GetLevel(), decision, value);
    std::lock_guard<std::mutex> guard(lock);
    if (logged[low] != key)
    {
        logged[low] = key;
        LOG_INFO("playerbots", "[mount4] the global leader: {}", line);
    }
}

bool IsTrainOrder(GuildOrder const& o) { return o.type == GuildOrderType::BUY && o.spellId == SPELL_APPRENTICE_RIDING; }

std::vector<uint32> FundingOrder()
{
    std::vector<std::pair<float, uint32>> due;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto const& [low, plan] : plans)
            if (plan.due && plan.copperPerHour > 0.0f && plan.spare < plan.Need())
                due.emplace_back(plan.copperPerHour, low);
    }
    std::sort(due.rbegin(), due.rend());
    std::vector<uint32> lows;
    for (auto const& [value, low] : due)
        lows.push_back(low);
    return lows;
}

uint32 Shortfall(uint32 low)
{
    std::lock_guard<std::mutex> guard(lock);
    auto const itr = plans.find(low);
    if (itr == plans.end() || !itr->second.due || itr->second.copperPerHour <= 0.0f)
        return 0;
    uint32 const need = itr->second.Need();
    return need > itr->second.spare ? need - itr->second.spare : 0;
}

bool MayFund(uint32 fromLow, uint32 toLow)
{
    std::lock_guard<std::mutex> guard(lock);
    auto const to = plans.find(toLow);
    if (to == plans.end())
        return false;
    auto const from = plans.find(fromLow);
    if (from == plans.end())
        return false;  // its plan is not made yet: its own riding may come first
    if (from->second.rides)
        return true;
    return !from->second.due || from->second.copperPerHour < to->second.copperPerHour;
}

void NoteDismount(Player* bot, char const* why)
{
    if (!bot || !bot->IsMounted())
        return;
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot); ai && ai->rpgInfo.mountWalkT)
        ai->rpgInfo.dismountReason = why;
}

void NoteFlight(float savedSec, uint32 fare)
{
    ++flights;
    flightFares += fare;
    flightSaved += uint64(std::max(0.0f, savedSec));
}
}  // namespace GuildRiding
