/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [guild81] Honest transfers between the guild bots, by the game's own flow: no GM command, no handout, no teleport.
//   - Trade: two members within the core's TRADE_DISTANCE open a trade, the sender puts the item / gold in, both
//     accept (CMSG_INITIATE_TRADE / BEGIN / SET_TRADE_ITEM / SET_TRADE_GOLD / ACCEPT_TRADE handlers).
//   - Mail: the sender at a mailbox mails the item / gold (CMSG_SEND_MAIL: the core takes its postage, 30 copper per
//     item, and delays an item between accounts by Mail.DeliveryDelay); the receiver takes it at a mailbox once the
//     core delivered it (CMSG_MAIL_TAKE_ITEM / TAKE_MONEY / MAIL_DELETE).
//   - Bank: the existing BANK_DEPOSIT / BANK_WITHDRAW flow (GuildBankRun), only when the guild and its tab exist.
// The global leader picks the channel by what it costs: the seconds both members walk off their own way (a detour
// from the walk each is already on, the whole way there and back when it stands), at their run speed. Trade costs
// nothing when they stand next to each other, and little when one walks where the other goes anyway; mail costs the
// sender's way to its cheapest mailbox plus the receiver's; the bank the two ways to the vaults. A chosen channel is
// kept while it stays possible (same map, a safe walk, the money for the postage).
// What moves: an item a member holds for another (upgrade from loot, a crafted product, an item for the enchanter),
// a crafting reagent the guild holds for its material stock to the crafter whose next skill-up or ordered craft
// lacks it, consumables inside a five (the quartermaster shares its food / drink equally with the members who have
// none left, by trade when they meet - user rule 2026-11-06), SUPPLY chains (a gatherer's ore / bars / herbs /
// leather / raw fish to the crafter that lacks it; a cook's food to the members who have none left, shared by each
// one's measured food use), and gold for the first bank tab (members' free surplus to the guild master, when the
// bank's savings pay for the walks; never below the module's money budget).
// All game actions run on the world thread (the core runs these opcodes there: PROCESS_THREADUNSAFE).

#include "GuildCoordinator.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <tuple>

#include "AiObjectContext.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildBankRun.h"
#include "GuildGear.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemPackets.h"
#include "Log.h"
#include "Mail.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TradeData.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    constexpr float CAMP_REACH = 10.0f;      // NewRpgStatusUpdateAction's GO_CAMP arrival distance
    constexpr uint32 MAIL_POSTAGE = 30;      // core HandleSendMail: 30 copper per item, 30 without items
    constexpr float NONE = FLT_MAX;
    constexpr uint8 FOOD = 1, DRINK = 2;

    uint32 NowSec() { return uint32(GameTime::GetGameTime().count()); }

    std::string Gold(uint32 copper)
    {
        uint32 const g = copper / 10000, s = (copper / 100) % 100, c = copper % 100;
        std::string out;
        if (g)
            out += std::to_string(g) + " з ";
        if (g || s)
            out += std::to_string(s) + " с ";
        out += std::to_string(c) + " м";
        return out;
    }

    std::string ItemText(uint32 itemId, uint32 count)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return (proto ? proto->Name1 : std::to_string(itemId)) + (count > 1 ? " ×" + std::to_string(count) : "");
    }

    Player* Online(uint32 low)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        return p && p->IsInWorld() && p->GetSession() ? p : nullptr;
    }

    bool Valid(WorldPosition const& p) { return p != WorldPosition(); }

    WorldPosition Pos(Player* p)
    {
        return WorldPosition(p->GetMapId(), p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(),
                             p->GetOrientation());
    }

    float Dist(WorldPosition const& a, WorldPosition const& b)
    {
        float const dx = a.GetPositionX() - b.GetPositionX(), dy = a.GetPositionY() - b.GetPositionY(),
                    dz = a.GetPositionZ() - b.GetPositionZ();
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // the walk the bot is on (a camp: vendor, vault, trainer; a profession errand), on its map
    WorldPosition RouteTarget(Player* p)
    {
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
        {
            WorldPosition target;
            if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&ai->rpgInfo.data))
                target = camp->pos;
            else if (auto const* prof = std::get_if<NewRpgInfo::DoProfession>(&ai->rpgInfo.data))
                target = prof->pos;
            if (Valid(target) && target.GetMapId() == p->GetMapId())
                return target;
        }
        return WorldPosition();
    }

    // yards the bot walks off its own way to stand at x: a detour from its walk, else there and back
    float Detour(Player* p, WorldPosition const& x)
    {
        if (!Valid(x) || x.GetMapId() != p->GetMapId())
            return NONE;
        WorldPosition const at = Pos(p);
        WorldPosition const target = RouteTarget(p);
        if (Valid(target))
            return std::max(0.0f, Dist(at, x) + Dist(x, target) - Dist(at, target));
        return 2.0f * Dist(at, x);
    }

    float WalkSeconds(Player* p, float yards)
    {
        if (yards == NONE)
            return NONE;
        float const speed = p->GetSpeed(MOVE_RUN);
        return yards / (speed > 0.0f ? speed : 1.0f);
    }

    std::string Sec(float s) { return s == NONE ? "—" : std::to_string(uint32(s + 0.5f)) + " с"; }

    // food / drink by the item's spell (the core's eat / drink auras)
    uint8 ConsumableKind(ItemTemplate const* proto)
    {
        if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_FOOD)
            return 0;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
        if (!spell)
            return 0;
        if (spell->HasAura(SPELL_AURA_MOD_POWER_REGEN))
            return DRINK;
        if (spell->HasAura(SPELL_AURA_MOD_REGEN))
            return FOOD;
        return 0;
    }

    template <class F>
    void ForEachBagItem(Player* p, F&& f)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* i = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                f(i);
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* i = bag->GetItemByPos(uint8(j)))
                        f(i);
    }

    // [recipes] the spell a recipe item teaches: the generic "Learning" layout (Spells[0] = 483 / 55884, Spells[1] =
    // the recipe) or a spell whose SPELL_EFFECT_LEARN_SPELL teaches it; 0 when none
    uint32 RecipeSpell(ItemTemplate const* proto)
    {
        constexpr int32 SPELL_LEARNING = 483;
        constexpr int32 SPELL_LEARNING_NORTHREND = 55884;
        int32 const first = proto->Spells[0].SpellId;
        if (first == SPELL_LEARNING || first == SPELL_LEARNING_NORTHREND)
            return proto->Spells[1].SpellId > 0 ? uint32(proto->Spells[1].SpellId) : 0;
        if (SpellInfo const* info = first > 0 ? sSpellMgr->GetSpellInfo(uint32(first)) : nullptr)
            for (SpellEffectInfo const& effect : info->Effects)
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    return effect.TriggerSpell;
        return 0;
    }

    // [recipes] the bot uses its recipe item as a player does (CMSG_USE_ITEM on itself); "" when it learned the spell
    std::string LearnRecipe(Player* p, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 const taught = RecipeSpell(proto);
        if (p->IsInCombat() || p->IsNonMeleeSpellCast(false) || p->IsInFlight() || p->CanUseItem(item) != EQUIP_ERR_OK)
            return "cannot use it now";
        uint32 const spellId = uint32(std::max<int32>(proto->Spells[0].SpellId, 0));
        // [econ1] b350: 22 "the core did not teach it" for Guinudrond - the use was sent while the global cooldown
        // ran (the core refuses the Learning cast, SPELL_FAILED_NOT_READY) and retried every second; a use waits
        // for the cooldown like a player's click does
        if (SpellInfo const* use = sSpellMgr->GetSpellInfo(spellId))
            if (p->GetGlobalCooldownMgr().HasGlobalCooldown(use))
                return "cannot use it now";
        WorldPacket packet(CMSG_USE_ITEM);
        packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0)
               << uint8(0) << uint32(TARGET_FLAG_NONE);
        p->GetSession()->HandleUseItemOpcode(packet);
        return !taught || p->HasSpell(taught) || p->IsNonMeleeSpellCast(false) ? "" : "the core did not teach it";
    }

    uint32 CountKind(Player* p, uint8 kind, uint8 level)
    {
        uint32 n = 0;
        ForEachBagItem(p, [&](Item* i)
                       {
                           ItemTemplate const* proto = i->GetTemplate();
                           if (ConsumableKind(proto) == kind && proto->RequiredLevel <= level)
                               n += i->GetCount();
                       });
        return n;
    }

    bool FreeSlot(Player* p, uint8& bagOut, uint8& slotOut)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                bagOut = INVENTORY_SLOT_BAG_0;
                slotOut = slot;
                return true;
            }
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                if (!bag->GetTemplate()->BagFamily)  // a profession bag takes only its own kind
                    for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                        if (!bag->GetItemByPos(uint8(j)))
                        {
                            bagOut = b;
                            slotOut = uint8(j);
                            return true;
                        }
        return false;
    }

    // CMSG_SPLIT_ITEM: `count` off the stack into a free slot; the new stack, or null
    Item* Split(Player* p, Item* item, uint32 count)
    {
        uint8 bag = 0, slot = 0;
        if (!FreeSlot(p, bag, slot))
            return nullptr;
        WorldPackets::Item::SplitItem packet{WorldPacket(CMSG_SPLIT_ITEM)};
        packet.SourceBag = item->GetBagSlot();
        packet.SourceSlot = item->GetSlot();
        packet.DestinationBag = bag;
        packet.DestinationSlot = slot;
        packet.Count = count;
        p->GetSession()->HandleSplitItemOpcode(packet);
        Item* split = p->GetItemByPos(bag, slot);
        return split && split->GetEntry() == item->GetEntry() && split != item ? split : nullptr;
    }

    // the core's trade, both sides by their opcode handlers; "" when the item / gold changed hands
    std::string DoTrade(Player* s, Player* r, Item* item, uint32 money)
    {
        WorldPacket init(CMSG_INITIATE_TRADE);
        init << r->GetGUID();
        s->GetSession()->HandleInitiateTradeOpcode(init);
        if (!s->GetTradeData() || s->GetTrader() != r)
            return "the core did not open the trade";
        WorldPacket begin(CMSG_BEGIN_TRADE);
        r->GetSession()->HandleBeginTradeOpcode(begin);
        ObjectGuid const itemGuid = item ? item->GetGUID() : ObjectGuid::Empty;
        uint32 const before = r->GetMoney();
        if (item)
        {
            WorldPacket set(CMSG_SET_TRADE_ITEM);
            set << uint8(0) << item->GetBagSlot() << item->GetSlot();  // trade slot 1
            s->GetSession()->HandleSetTradeItemOpcode(set);
        }
        if (money && s->GetTradeData())
        {
            WorldPacket gold(CMSG_SET_TRADE_GOLD);
            gold << money;
            s->GetSession()->HandleSetTradeGoldOpcode(gold);
        }
        if (s->GetTradeData() && (!item || s->GetTradeData()->GetItem(TradeSlots(0)) == item))
        {
            WorldPacket accept(CMSG_ACCEPT_TRADE);
            s->GetSession()->HandleAcceptTradeOpcode(accept);
            if (r->GetTradeData())
            {
                WorldPacket accept2(CMSG_ACCEPT_TRADE);
                r->GetSession()->HandleAcceptTradeOpcode(accept2);
            }
        }
        if (s->GetTradeData())
        {
            WorldPacket cancel(CMSG_CANCEL_TRADE);
            s->GetSession()->HandleCancelTradeOpcode(cancel);
            return "the core did not complete the trade (bags full / item not tradable)";
        }
        if (item && s->GetItemByGuid(itemGuid))  // the receiver may stack it onto its own: the sender's bags tell
            return "the item did not leave the sender";
        if (money && r->GetMoney() < before + money)
            return "the gold did not reach the receiver";
        return "";
    }

    // the core's CMSG_SEND_MAIL at the mailbox the sender stands at; "" when sent
    std::string DoMail(Player* s, GameObject* box, std::string const& to, Item* item, uint32 money,
                       std::string const& subject)
    {
        WorldPacket data(CMSG_SEND_MAIL);
        data << box->GetGUID();
        data << to << subject << std::string("") << uint32(0) << uint32(0);
        data << uint8(item ? 1 : 0);
        if (item)
            data << uint8(0) << item->GetGUID();
        data << money << uint32(0) << uint64(0) << uint8(0);
        ObjectGuid const itemGuid = item ? item->GetGUID() : ObjectGuid::Empty;
        uint32 const before = s->GetMoney();
        s->GetSession()->HandleSendMail(data);
        if (item ? s->GetItemByGuid(itemGuid) != nullptr : s->GetMoney() + money > before)
            return "the core refused the mail";
        return "";
    }

    GameObject* MailboxAt(Player* p)
    {
        GameObject* box = p->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, CAMP_REACH);
        return box && p->GetGameObjectIfCanInteractWith(box->GetGUID(), GAMEOBJECT_TYPE_MAILBOX) ? box : nullptr;
    }

    char const* ChannelName(uint8 channel)
    {
        return channel == 0 ? "обмен" : channel == 1 ? "почта" : "банк";
    }

    char const* ChannelLog(uint8 channel) { return channel == 0 ? "trade" : channel == 1 ? "mail" : "bank"; }
}

void GuildCoordinator::LoadMailboxes()
{
    mailboxes_.clear();
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        GameObjectTemplate const* proto = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!proto || proto->type != GAMEOBJECT_TYPE_MAILBOX)
            continue;
        MapEntry const* map = sMapStore.LookupEntry(data.mapid);
        if (!map || map->Instanceable())
            continue;
        Mailbox box;
        box.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
        // the town's side: its area (Crossroads in the Barrens), else its zone
        uint32 const areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        box.areaId = areaId;
        box.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(box.zoneId))
            box.capital = zone->flags & AREA_FLAG_CAPITAL;
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
        {
            box.areaTeam = area->team;
            if (!box.areaTeam && area->zone)
                if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(area->zone))
                    box.areaTeam = zone->team;
        }
        mailboxes_.push_back(box);
    }
    // [econ1] b350: 1 smelt in the whole run - the craft executor smelts only with a forge in sight, and the smelt
    // call had no place. The forges of the towns (a forge spell focus in an area with a mailbox): the smelt call
    // waits for the miner's visit to the nearest one
    forges_.clear();
    std::set<uint32> towns;
    for (Mailbox const& box : mailboxes_)
        if (box.areaId)
            towns.insert(box.areaId);
    constexpr uint32 FOCUS_FORGE = 3;  // SpellFocusObject.dbc 3: Forge (the smelting spells' RequiresSpellFocus)
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        GameObjectTemplate const* proto = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!proto || proto->type != GAMEOBJECT_TYPE_SPELL_FOCUS || proto->spellFocus.focusId != FOCUS_FORGE)
            continue;
        MapEntry const* map = sMapStore.LookupEntry(data.mapid);
        if (!map || map->Instanceable())
            continue;
        uint32 const areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        if (!towns.count(areaId))
            continue;
        Mailbox forge;
        forge.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
        forge.areaId = areaId;
        forge.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
        {
            forge.areaTeam = area->team;
            if (!forge.areaTeam && area->zone)
                if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(area->zone))
                    forge.areaTeam = zone->team;
        }
        forges_.push_back(forge);
    }
    LOG_INFO("playerbots", "[econ1] smelting: {} town forges on the continents", forges_.size());
    LOG_INFO("playerbots", "[guild81] transfers: {} mailboxes on the continents, postage {} copper per item, item "
             "delivery between accounts {} s, trade distance {} yd", mailboxes_.size(), MAIL_POSTAGE,
             sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY), TRADE_DISTANCE);
}

// ---------------------------------------------------------------------------------------------------------------------
// world thread: the plan
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::UpdateTransfers()
{
    uint32 const now = NowSec();
    std::lock_guard<std::mutex> guard(lock_);
    std::vector<std::string> logs;
    struct Choice  // the channel picked and what each one costs
    {
        Transfer::Channel channel{Transfer::TRADE};
        float cost{NONE};
        float trade{NONE}, mail{NONE}, bank{NONE};
        uint32 walkerLow{0};
        WorldPosition meet{};
        WorldPosition mailbox{};
        uint32 delay{0};
        std::string text;
    };

    auto const canWalk = [&](Player* p)
    {
        if (!p || !p->IsAlive() || !GET_PLAYERBOT_AI(p) || HumanCharacter(p))
            return false;
        auto const st = bots_.find(p->GetGUID().GetCounter());
        return st == bots_.end() || !st->second.fiveId;  // a five's dungeon first
    };
    // [guild15] user rule 2026-11-08: no special trip to a mailbox (b325 Colian walked 400-1463 yd to hand over
    // bananas / Rough Stone). A mailbox counts only on the bot's way: the one it passes (CAMP_REACH), one in the town
    // (area) its own walk goes to (vendor, trainer, inn, vault), or in the town it visits now (at a vendor)
    // (keepArea: the town of the mailbox its mail walk already goes to, while it is still in that town)
    auto const nearestMailbox = [&](Player* p, WorldPosition& out, uint32 keepArea = 0)
    {
        float best = NONE;
        uint32 townNow = 0;
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
            if (ai->rpgInfo.GetStatus() == RPG_WANDER_NPC || (keepArea && p->GetAreaId() == keepArea))
                townNow = p->GetAreaId();
        uint32 townAhead = 0;
        WorldPosition const target = RouteTarget(p);
        if (Valid(target))
            townAhead = sMapMgr->GetAreaId(PHASEMASK_NORMAL, target.GetMapId(), target.GetPositionX(),
                                           target.GetPositionY(), target.GetPositionZ());
        // [guild15] user 2026-11-08: in a capital (Stormwind, Ironforge, Darnassus, the Exodar) the bot stops at the
        // city's mailbox nearest its way as soon as it is inside the city's zone - not every mailbox of the area it
        // goes to (b326-b333: 270-850 yd inside Ironforge / the Exodar)
        uint32 const zoneNow = p->GetZoneId();
        for (Mailbox const& box : mailboxes_)
        {
            if (box.pos.GetMapId() != p->GetMapId())
                continue;
            bool const passing = Dist(Pos(p), box.pos) <= CAMP_REACH;
            if (box.capital)
            {
                if (!passing && box.zoneId != zoneNow)
                    continue;  // a city's mailbox: once inside the city
            }
            else if (!passing && (!box.areaId || (box.areaId != townNow && box.areaId != townAhead)))
                continue;  // not on its way
            if ((box.areaTeam == AREATEAM_ALLY && p->GetTeamId() != TEAM_ALLIANCE) ||
                (box.areaTeam == AREATEAM_HORDE && p->GetTeamId() != TEAM_HORDE))
                continue;  // a hostile town's mailbox
            if (TownRefused(p->GetGUID().GetCounter(), box.pos))
                continue;  // [econ1] its walk there was refused / given up at this level
            float const d = Detour(p, box.pos);
            if (d < best)
            {
                best = d;
                out = box.pos;
            }
        }
        return best;
    };
    auto const NearestFriendlyMailbox = [&](Player* p)
    {
        WorldPosition best{};
        float bestDist = NONE;
        for (Mailbox const& box : mailboxes_)
        {
            if (box.pos.GetMapId() != p->GetMapId())
                continue;
            if ((box.areaTeam == AREATEAM_ALLY && p->GetTeamId() != TEAM_ALLIANCE) ||
                (box.areaTeam == AREATEAM_HORDE && p->GetTeamId() != TEAM_HORDE))
                continue;
            if (TownRefused(p->GetGUID().GetCounter(), box.pos))
                continue;  // [econ1] its walk there was refused / given up at this level
            float const d = Dist(Pos(p), box.pos);
            if (d < bestDist)
            {
                bestDist = d;
                best = box.pos;
            }
        }
        return best;
    };
    auto const vaultWalk = [&](Player* p)
    {
        WorldPosition pos;
        float dist = 0;
        return GuildBankRun::NearestVault(p, pos, dist) ? Detour(p, pos) : NONE;
    };

    // 0. food use: what left the bags since the last update (eaten; sold or traded food counts too), last hour
    for (PlanRow const& row : plan_)
    {
        Player* p = Online(row.low);
        if (!p || !GET_PLAYERBOT_AI(p))
            continue;
        uint32 const n = CountKind(p, FOOD, p->GetLevel());
        auto& [last, eaten] = foodUse_[row.low];
        if (last > n)
            eaten.emplace_back(now, last - n);
        last = n;
        while (!eaten.empty() && now - eaten.front().first > HOUR)
            eaten.pop_front();
    }

    // 1. the members' mail: delivered (taken at a mailbox) / still in transit
    auto const lastPickup = mailPickup_;  // [guild15] a pickup walk keeps its town's mailbox while there
    mailPickup_.clear();
    mailTransit_.clear();
    for (PlanRow const& row : plan_)
    {
        Player* p = Online(row.low);
        if (!p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p))
            continue;
        uint32 delivered = 0;
        for (Mail const* m : p->GetMails())
        {
            if (!m || m->state == MAIL_STATE_DELETED || m->messageType != MAIL_NORMAL || m->COD ||
                !IsPlanned(m->sender) || (m->items.empty() && !m->money))
                continue;
            uint8 bag = 0, slot = 0;
            if (uint32(m->deliver_time) <= now && (m->money || FreeSlot(p, bag, slot)))
                ++delivered;  // bags full: an item mail waits (no stand at the mailbox it cannot empty)
            else if (uint32(m->deliver_time) <= now)
                mailTransit_[row.low].push_back(Who(m->sender) + ": delivered, bags full");
            else
                mailTransit_[row.low].push_back(Who(m->sender) + ": in " +
                                                std::to_string((uint32(m->deliver_time) - now + 59) / 60) + " min");
        }
        WorldPosition box;
        uint32 keepArea = 0;
        if (auto const last = lastPickup.find(row.low); last != lastPickup.end())
            keepArea = sMapMgr->GetAreaId(PHASEMASK_NORMAL, last->second.second.GetMapId(),
                                          last->second.second.GetPositionX(), last->second.second.GetPositionY(),
                                          last->second.second.GetPositionZ());
        if (delivered && nearestMailbox(p, box, keepArea) < NONE)
            mailPickup_[row.low] = {delivered, box};
        // [econ1] b350: 143 letters lay in the members' mailboxes (Lanbuzz 19, Emdyl 11) and 15 were ever taken -
        // the pickup was planned only while a mailbox lay on the bot's way right then, so a bot whose walks never
        // passed one never got the task. No mailbox on its way now: its nearest friendly town mailbox, taken on
        // its next visit there (a convenient town task: the route's town stop, no special trip)
        else if (delivered)
            if (WorldPosition const any = NearestFriendlyMailbox(p); Valid(any))
                mailPickup_[row.low] = {delivered, any};
    }

    // 2. what should move
    struct Want
    {
        std::string key;
        bool gold{false};
        bool tradeOnly{false};
        uint32 from{0}, to{0}, guid{0}, itemId{0}, count{0}, money{0};
        std::string purpose, why;
    };
    std::vector<Want> wants;
    std::set<uint32> used;  // item guids already moving

    // a. items held for a member: an upgrade from loot, a crafted product, an item for the enchanter
    for (auto const& [low, st] : bots_)
        for (HeldItem const& h : st.held)
        {
            if (!h.forLow || h.forLow == low || h.purpose == "pending" || !IsPlanned(h.forLow))
                continue;
            Want w;
            w.key = "i" + std::to_string(h.itemGuidLow);
            w.from = low;
            w.to = h.forLow;
            w.guid = h.itemGuidLow;
            w.itemId = h.itemId;
            w.count = h.count;
            w.purpose = h.purpose.rfind("улучшение", 0) == 0 ? "улучшение"
                        : h.purpose.rfind("крафт", 0) == 0   ? "изделие"
                        : h.purpose.rfind("распылить", 0) == 0 ? "на распыление"
                                                               : "предмет";
            w.why = h.purpose;
            wants.push_back(w);
            used.insert(h.itemGuidLow);
        }

    // b. reagents: what a crafter's next skill-up recipe or ordered craft lacks, from the guild's material stock
    // (items held for the bank, forLow 0) in another member's bags; a holder that lacks the item itself keeps it
    std::map<std::pair<uint32, uint32>, std::pair<uint32, std::string>> lack;  // (crafter, item) -> (missing, why)
    for (PlanRow const& row : plan_)
    {
        Player* c = Online(row.low);
        if (!c || !GET_PLAYERBOT_AI(c) || HumanCharacter(c))
            continue;
        for (auto const& [line, plan] : skillPlan_)
        {
            if (!c->HasSkill(line))
                continue;
            uint32 const value = c->GetPureSkillValue(line);
            if (value >= c->GetPureMaxSkillValue(line) || value >= plan.size() || plan[value] < 0)
                continue;
            auto const list = recipesBySkill_.find(line);
            if (list == recipesBySkill_.end() || size_t(plan[value]) >= list->second.size())
                continue;
            Recipe const& r = list->second[size_t(plan[value])];
            if (!c->HasSpell(r.spellId))
                continue;
            for (auto const& [item, n] : r.reagents)
                if (uint32 const have = c->GetItemCount(item, false); have < n)
                    lack[{row.low, item}] = {n - have, "прокачка навыка " + std::to_string(value) + ": " +
                                                           ItemText(r.product, 1)};
        }
        // [smelt] a miner's next smelt that still gives it skill: the ore goes to it (it smelts and levels Mining)
        std::vector<std::pair<uint32, uint32>> ores;
        uint32 bar = 0;
        if (NextSmelt(c, &ores, &bar))
            for (auto const& [ore, n] : ores)
                if (uint32 const have = c->GetItemCount(ore, false); have < n)
                    lack[{row.low, ore}] = {n - have, "плавка (горное дело " +
                                                          std::to_string(c->GetPureSkillValue(SKILL_MINING)) +
                                                          "): " + ItemText(bar, 1)};
    }
    for (CraftJob const& job : crafts_)
    {
        GuildGear::CraftRecipe const* recipe = GuildGear::Recipe(job.spellId);
        Player* c = Online(job.crafterLow);
        if (!recipe || !c || job.ready)
            continue;
        for (auto const& [item, n] : recipe->reagents)
            if (uint32 const have = c->GetItemCount(item, false); have < n)
                lack[{job.crafterLow, item}] = {n - have, "крафт " + ItemText(job.itemId, 1) + " для " +
                                                              Who(job.forLow)};
    }
    std::set<std::pair<uint32, uint32>> ownNeed, incoming;
    for (auto const& [key, need] : lack)
        ownNeed.insert(key);
    for (auto const& [id, t] : transfers_)
        if (t.mailed && !t.gold)
            incoming.insert({t.toLow, t.itemId});  // already in its mailbox
    for (auto const& [key, need] : lack)
    {
        auto const& [crafter, item] = key;
        if (incoming.count(key))
            continue;
        Player* c = Online(crafter);
        Want best;
        float bestDist = NONE;
        for (auto const& [low, st] : bots_)
        {
            if (low == crafter || ownNeed.count({low, item}))
                continue;
            Player* h = Online(low);
            for (HeldItem const& held : st.held)
            {
                if (held.itemId != item || held.forLow || held.purpose == "pending" || used.count(held.itemGuidLow))
                    continue;
                float const d = h && c && h->GetMapId() == c->GetMapId() ? h->GetExactDist(c) : NONE / 2;
                if (d < bestDist)
                {
                    bestDist = d;
                    best.key = "r" + std::to_string(crafter) + ":" + std::to_string(item);
                    best.from = low;
                    best.to = crafter;
                    best.guid = held.itemGuidLow;
                    best.itemId = item;
                    best.count = held.count;
                    best.purpose = "реагент";
                    best.why = "не хватает " + std::to_string(need.first) + " — " + need.second;
                }
            }
        }
        if (best.from)
        {
            // [guild81] SUPPLY: the holder gathered it (miner / herbalist / skinner / fisher -> its crafter)
            if (Player* h = Online(best.from))
            {
                auto const src = gatherSource_.find(item);
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                uint32 const skill = src != gatherSource_.end() ? src->second.first
                                     : skinned_.count(item)     ? uint32(SKILL_SKINNING)
                                     : proto && proto->Class == ITEM_CLASS_TRADE_GOODS &&
                                             proto->SubClass == ITEM_SUBCLASS_MEAT
                                         ? uint32(SKILL_FISHING)
                                         : 0;
                if (skill && h->HasSkill(skill))
                {
                    best.purpose = "снабжение";
                    best.why = "сборщик → ремесленник; " + best.why;
                }
            }
            used.insert(best.guid);
            wants.push_back(best);
        }
    }

    // e. SUPPLY of food: a cook shares the food it holds with the members who have none left, each by its measured
    // use (food eaten over the last hour, from the bags between updates; no use measured yet: equal shares); the cook
    // keeps its own share. Members of a five get theirs from the five's quartermaster (c).
    std::set<uint32> fed;
    for (PlanRow const& row : plan_)
    {
        Player* cook = Online(row.low);
        if (!cook || !GET_PLAYERBOT_AI(cook) || HumanCharacter(cook) || !cook->HasSkill(SKILL_COOKING))
            continue;
        auto const useOf = [&](uint32 low)
        {
            uint32 n = 0;
            auto const u = foodUse_.find(low);
            if (u != foodUse_.end())
                for (auto const& [at, eaten] : u->second.second)
                    n += eaten;
            return n;
        };
        std::vector<std::pair<Player*, uint32>> needy;  // member, food eaten last hour
        uint8 level = cook->GetLevel();
        for (PlanRow const& m : plan_)
        {
            Player* p = m.low == row.low || fed.count(m.low) ? nullptr : Online(m.low);
            if (!p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p) || CountKind(p, FOOD, p->GetLevel()))
                continue;
            if (auto const st = bots_.find(m.low); st != bots_.end() && st->second.fiveId)
                continue;
            needy.emplace_back(p, useOf(m.low));
            level = std::min<uint8>(level, p->GetLevel());
        }
        if (needy.empty())
            continue;
        std::vector<Item*> stacks;
        uint32 total = 0;
        auto const cookState = bots_.find(row.low);
        auto const forSale = [&](Item* i)  // the global leader already sold it off
        {
            return cookState != bots_.end() &&
                   std::find(cookState->second.sell.begin(), cookState->second.sell.end(),
                             i->GetGUID().GetCounter()) != cookState->second.sell.end();
        };
        ForEachBagItem(cook, [&](Item* i)
                       {
                           if (ConsumableKind(i->GetTemplate()) == FOOD && i->GetTemplate()->RequiredLevel <= level &&
                               !used.count(i->GetGUID().GetCounter()) && !forSale(i))
                           {
                               stacks.push_back(i);
                               total += i->GetCount();
                           }
                       });
        uint64 weights = 0;
        bool const measured = std::any_of(needy.begin(), needy.end(), [](auto const& n) { return n.second > 0; });
        uint32 const cookUse = useOf(row.low);
        for (auto const& [p, use] : needy)
            weights += measured ? use : 1;
        weights += measured ? cookUse : 1;
        if (!total || !weights)
            continue;
        std::map<Item*, uint32> left;
        for (Item* i : stacks)
            left[i] = i->GetCount();
        for (auto const& [p, use] : needy)
        {
            uint32 const share = uint32(uint64(total) * (measured ? use : 1) / weights);
            Item* from = nullptr;
            for (Item* i : stacks)
                if (!from || left[i] > left[from])
                    from = i;
            if (!share || !from || !left[from])
                continue;
            uint32 const give = std::min(share, left[from]);
            left[from] -= give;
            Want w;
            w.key = "s" + std::to_string(p->GetGUID().GetCounter()) + ":food";
            w.from = row.low;
            w.to = p->GetGUID().GetCounter();
            w.guid = from->GetGUID().GetCounter();
            w.itemId = from->GetEntry();
            w.count = give;
            w.purpose = "снабжение";
            w.why = "еда кончилась; съел за час " + std::to_string(use) + "; повар делит " + std::to_string(total) +
                    (measured ? " по расходу" : " поровну");
            wants.push_back(w);
            fed.insert(w.to);
        }
    }

    // c. consumables in a five: the quartermaster shares its food / drink equally with the members who have none
    // left (trade when they meet: they walk together)
    for (auto const& [id, five] : fives_)
    {
        if (five.stage == GuildFive::Stage::Done)
            continue;
        Player* q = Online(five.quartermasterLow);
        if (!q || !GET_PLAYERBOT_AI(q))
            continue;
        for (uint8 kind : {FOOD, DRINK})
        {
            std::vector<Player*> needy;
            uint8 level = q->GetLevel();
            for (uint32 m : five.members)
            {
                Player* p = m == five.quartermasterLow ? nullptr : Online(m);
                if (!p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p) ||
                    (kind == DRINK && p->getPowerType() != POWER_MANA) || CountKind(p, kind, p->GetLevel()))
                    continue;
                needy.push_back(p);
                level = std::min<uint8>(level, p->GetLevel());
            }
            if (needy.empty())
                continue;
            std::vector<Item*> stacks;
            uint32 total = 0;
            ForEachBagItem(q, [&](Item* i)
                           {
                               ItemTemplate const* proto = i->GetTemplate();
                               if (ConsumableKind(proto) == kind && proto->RequiredLevel <= level &&
                                   !used.count(i->GetGUID().GetCounter()))
                               {
                                   stacks.push_back(i);
                                   total += i->GetCount();
                               }
                           });
            uint32 const share = total / uint32(needy.size() + 1);  // the quartermaster keeps an equal share
            if (!share)
                continue;
            std::sort(stacks.begin(), stacks.end(), [](Item* a, Item* b) { return a->GetCount() > b->GetCount(); });
            std::map<Item*, uint32> left;
            for (Item* i : stacks)
                left[i] = i->GetCount();
            for (Player* p : needy)
            {
                Item* from = nullptr;
                for (Item* i : stacks)
                    if (!from || left[i] > left[from])
                        from = i;
                if (!from || !left[from])
                    break;
                uint32 const give = std::min(share, left[from]);
                left[from] -= give;
                Want w;
                w.key = "c" + std::to_string(p->GetGUID().GetCounter()) + ":" + std::to_string(kind);
                w.tradeOnly = true;
                w.from = five.quartermasterLow;
                w.to = p->GetGUID().GetCounter();
                w.guid = from->GetGUID().GetCounter();
                w.itemId = from->GetEntry();
                w.count = give;
                w.purpose = "расходник";
                w.why = std::string(kind == FOOD ? "еда" : "питьё") + " кончилась; интендант пятёрки " +
                        std::to_string(id) + " делит поровну: " + std::to_string(total) + " на " +
                        std::to_string(needy.size() + 1);
                wants.push_back(w);
            }
        }
    }

    // the channel each costs: seconds both walk off their way (see the file comment)
    uint32 const tradeLevel = sWorld->getIntConfig(CONFIG_TRADE_LEVEL_REQ);
    uint32 const mailLevel = sWorld->getIntConfig(CONFIG_MAIL_LEVEL_REQ);
    auto const choose = [&](Want const& w, Transfer const* old) -> Choice
    {
        Choice c;
        Player* s = Online(w.from);
        Player* r = Online(w.to);
        if (!s)
            return c;
        // trade: next to each other, or one walks to the other (to where it stands, or where it walks anyway)
        // [human] the trade below runs both sides' opcodes: never with a human receiver (he gets it by mail and
        // takes it himself)
        if (r && !HumanCharacter(r) && s->GetMap() == r->GetMap() && s->GetLevel() >= tradeLevel &&
            r->GetLevel() >= tradeLevel)
        {
            if (s->IsWithinDistInMap(r, TRADE_DISTANCE))
                c.trade = 0.0f;
            else if (!w.tradeOnly && !(old && old->tradeRefused))  // a refused walk: only when they meet
                for (Player* walker : {s, r})
                {
                    Player* other = walker == s ? r : s;
                    if (!canWalk(walker))
                        continue;
                    for (bool const fixed : {false, true})
                    {
                        // where the other stands now, or where it walks anyway (it comes there itself)
                        WorldPosition const x = fixed ? RouteTarget(other) : Pos(other);
                        // [guild15] user rule 2026-11-08: only when they meet anyway - the other stands (or walks
                        // to) within trade distance of the walker's own way; nobody walks off it to hand over
                        float const detour = Detour(walker, x);
                        if (detour > 2.0f * TRADE_DISTANCE)
                            continue;
                        float const sec = WalkSeconds(walker, detour);
                        if (sec < c.trade)
                        {
                            c.trade = sec;
                            c.walkerLow = walker->GetGUID().GetCounter();
                            c.meet = fixed ? x : WorldPosition();
                        }
                    }
                }
        }
        if (!w.tradeOnly)
        {
            // mail: the sender's cheapest mailbox (it pays the postage out of its free money), the receiver's
            uint32 const postage = MAIL_POSTAGE;
            // [econ1] mail only to a receiver that passes a mailbox: a bot with a friendly town mailbox on its map
            // its walks are not refused to (it takes the letter on its next visit there); a human takes his own
            bool const receiverBox = !r || HumanCharacter(r) || !GET_PLAYERBOT_AI(r) ||
                                     Valid(NearestFriendlyMailbox(r));
            if (s->GetLevel() >= mailLevel && canWalk(s) && FoundingFreeMoney(s) >= postage + w.money &&
                !(old && old->mailRefused) && receiverBox)
            {
                WorldPosition box;
                uint32 const keepArea =
                    old && old->channel == Transfer::MAIL && Valid(old->mailbox)
                        ? sMapMgr->GetAreaId(PHASEMASK_NORMAL, old->mailbox.GetMapId(), old->mailbox.GetPositionX(),
                                             old->mailbox.GetPositionY(), old->mailbox.GetPositionZ())
                        : 0;
                float const ys = nearestMailbox(s, box, keepArea);
                // [guild15] the receiver takes it at a mailbox on its own way later (no walk of its own counted)
                if (ys < NONE)
                {
                    c.mail = WalkSeconds(s, ys);
                    c.mailbox = box;
                }
                // [guild15] no mailbox on its way right now: its nearest friendly mailbox in any town, mailed on its
                // next visit there (MAIL_TO waits for that town, no special trip: no walk counted). Before, the
                // channel was offered only while a box lay on the way, and the plan flipped once the sender left town.
                else if (WorldPosition const any = NearestFriendlyMailbox(s); Valid(any))
                {
                    c.mail = 0.0f;
                    c.mailbox = any;
                }
                uint32 const sAccount = s->GetSession()->GetAccountId();
                uint32 const rAccount = r ? r->GetSession()->GetAccountId()
                                          : sCharacterCache->GetCharacterAccountIdByGuid(
                                                ObjectGuid::Create<HighGuid::Player>(w.to));
                c.delay = !w.gold && sAccount != rAccount ? sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY) : 0;
                // an upgrade / product / needed reagent: the core's delivery time is time until it is usable
                bool const needed = w.purpose == "улучшение" || w.purpose == "изделие" || w.purpose == "реагент" ||
                                    (w.purpose == "снабжение" && w.key[0] == 'r');
                if (needed && c.mail < NONE)
                    c.mail += float(c.delay);
            }
            // bank: both walks to the vaults (only with the guild's tab and the rights)
            if (!w.gold && r && GuildBankRun::BankState(s, true).empty() && GuildBankRun::BankState(r, false).empty() &&
                canWalk(s) && canWalk(r))
            {
                float const ys = vaultWalk(s), yr = vaultWalk(r);
                if (ys < NONE && yr < NONE)
                    c.bank = WalkSeconds(s, ys) + WalkSeconds(r, yr);
            }
        }
        // the cheapest; equal: trade, bank (no delay, no postage), mail. The old channel stays while possible.
        c.cost = c.trade;
        c.channel = Transfer::TRADE;
        if (c.bank < c.cost)
        {
            c.cost = c.bank;
            c.channel = Transfer::BANK;
        }
        if (c.mail < c.cost)
        {
            c.cost = c.mail;
            c.channel = Transfer::MAIL;
        }
        if (old)
        {
            float const kept = old->channel == Transfer::TRADE  ? c.trade
                               : old->channel == Transfer::MAIL ? c.mail
                                                                : c.bank;
            if (kept < NONE)
            {
                c.cost = kept;
                c.channel = old->channel;
            }
        }
        if (w.tradeOnly && c.trade == NONE)
        {
            c.channel = Transfer::TRADE;  // a five's share: handed over when they meet
            c.cost = 0.0f;
            c.text = "обмен при встрече в пятёрке";
            return c;
        }
        c.text = "обмен " + Sec(c.trade) +
                 (c.walkerLow && c.trade < NONE ? " (идёт " + Who(c.walkerLow) + ")" : "") + ", почта " + Sec(c.mail) +
                 (c.mail < NONE && c.delay ? " (доставка " + std::to_string(c.delay / 60) + " мин)" : "") +
                 (c.mail < NONE ? ", " + Gold(MAIL_POSTAGE) : "") + ", банк " + Sec(c.bank);
        return c;
    };

    // d. gold for the first bank tab: pooling to the guild master when the bank's savings pay for the walks
    std::vector<Want> donations;
    std::string donation;
    Guild* guild = found_.guildId ? sGuildMgr->GetGuildById(found_.guildId) : nullptr;
    uint32 const tabCost = sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_0);
    if (guild && !guild->GetPurchasedTabsSize() && tabCost)
    {
        uint32 const gmLow = guild->GetLeaderGUID().GetCounter();
        Player* gm = Online(gmLow);
        if (!gm || !GET_PLAYERBOT_AI(gm) || HumanCharacter(gm))
            donation = "взносы на вкладку: глава не бот в сети";
        else
        {
            uint32 const gmFree = FoundingFreeMoney(gm);
            uint32 inMail = 0;
            for (Mail const* m : gm->GetMails())
                if (m && m->state != MAIL_STATE_DELETED && IsPlanned(m->sender))
                    inMail += m->money;
            uint32 const need = tabCost > gmFree + inMail ? tabCost - gmFree - inMail : 0;
            if (!need)
                donation = "вкладка " + Gold(tabCost) + ": у главы свободно " + Gold(gmFree) +
                           (inMail ? " + в почте " + Gold(inMail) : "") + " — хватает, взносов не нужно";
            else
            {
                struct Donor
                {
                    Want w;
                    float cost;
                };
                std::vector<Donor> donors;
                uint64 pool = 0;
                for (PlanRow const& row : plan_)
                {
                    Player* p = row.low == gmLow ? nullptr : Online(row.low);
                    if (!p || p->GetGuildId() != guild->GetId() || !canWalk(p))
                        continue;
                    uint32 const free = FoundingFreeMoney(p);
                    if (free <= MAIL_POSTAGE)
                        continue;
                    Want w;
                    w.key = "g" + std::to_string(row.low);
                    w.gold = true;
                    w.from = row.low;
                    w.to = gmLow;
                    w.money = free - MAIL_POSTAGE;  // what it can give whichever channel (the postage kept back)
                    w.purpose = "взнос";
                    Choice const c = choose(w, nullptr);
                    if (c.cost == NONE)
                        continue;
                    if (c.channel == Transfer::TRADE)
                        w.money = free;
                    donors.push_back({w, c.cost});
                    pool += w.money;
                }
                if (pool < need)
                    donation = "вкладка " + Gold(tabCost) + ": у главы " + Gold(gmFree) +
                               (inMail ? " + в почте " + Gold(inMail) : "") + ", у членов свободно " +
                               Gold(uint32(std::min<uint64>(pool, UINT32_MAX))) +
                               " — не хватает, взносов нет (копят сами)";
                else
                {
                    // the cheapest walks first, each up to its free surplus, until the tab is paid
                    std::sort(donors.begin(), donors.end(), [](Donor const& a, Donor const& b)
                              { return a.cost != b.cost ? a.cost < b.cost : a.w.money > b.w.money; });
                    uint32 left = need;
                    float walk = 0.0f;
                    std::vector<Want> picked;
                    for (Donor& d : donors)
                    {
                        if (!left)
                            break;
                        d.w.money = std::min(d.w.money, left);
                        left -= d.w.money;
                        walk += d.cost;
                        d.w.why = "вкладка банка " + Gold(tabCost) + ": у главы " + Gold(gmFree) +
                                  ", не хватает " + Gold(need);
                        picked.push_back(d.w);
                    }
                    float const tabWalk = WalkSeconds(gm, vaultWalk(gm));
                    if (tabWalk < NONE)
                        walk += tabWalk;
                    // the bank's savings: the transfers that would go cheaper through it, and the items held for it
                    float saving = 0.0f;
                    uint32 stuck = 0;
                    for (Want const& w : wants)
                    {
                        Player* s = Online(w.from);
                        Player* r = Online(w.to);
                        if (w.tradeOnly || !s || !r)
                            continue;
                        float const viaBank = WalkSeconds(s, vaultWalk(s)) + WalkSeconds(r, vaultWalk(r));
                        Choice const c = choose(w, nullptr);
                        if (viaBank < NONE && c.cost == NONE)
                            ++stuck;
                        else if (viaBank < c.cost)
                            saving += c.cost - viaBank;
                    }
                    for (auto const& [low, st] : bots_)
                        for (HeldItem const& h : st.held)
                            stuck += !h.forLow && h.purpose != "pending" ? 1 : 0;
                    bool const pays = saving > walk || stuck > 0;
                    donation = "вкладка " + Gold(tabCost) + ": у главы " + Gold(gmFree) +
                               (inMail ? " + в почте " + Gold(inMail) : "") + ", взносы " +
                               std::to_string(picked.size()) + " членов (" + Sec(walk) + " ходьбы); банк сбережёт " +
                               Sec(saving) + " на передачах и освободит " + std::to_string(stuck) + " яч. сумок — " +
                               (pays ? "окупается: собирают" : "не окупается: копит глава сам");
                    if (pays)
                        donations = std::move(picked);
                }
            }
        }
    }
    for (Want& w : donations)
        wants.push_back(std::move(w));

    // e. [regress10] gold for a repair the member cannot pay (its REPAIR task's shortfall, the core's repair cost):
    // from the member whose free money covers it at the cheapest channel
    for (auto const& [low, st] : bots_)
    {
        if (!st.repairShort || !IsPlanned(low))
            continue;
        Player* r = Online(low);
        if (!r || !GET_PLAYERBOT_AI(r) || HumanCharacter(r))
            continue;
        Want best;
        float bestCost = NONE;
        for (PlanRow const& row : plan_)
        {
            Player* s = row.low == low ? nullptr : Online(row.low);
            if (!s || !GET_PLAYERBOT_AI(s) || HumanCharacter(s) || !canWalk(s) ||
                FoundingFreeMoney(s) < st.repairShort + MAIL_POSTAGE)
                continue;
            Want w;
            w.key = "p" + std::to_string(low);
            w.gold = true;
            w.from = row.low;
            w.to = low;
            w.money = st.repairShort;
            w.purpose = "ремонт";
            w.why = "не хватает на ремонт " + Gold(st.repairShort);
            Choice const c = choose(w, nullptr);
            if (c.cost < bestCost)
            {
                bestCost = c.cost;
                best = w;
            }
        }
        if (bestCost < NONE)
            wants.push_back(best);
    }

    // e. [recipes] user 2026-11-08 (Eraelor held Recipe: Brilliant Smallfish): every recipe item in a member's bags
    // goes to the member best placed to learn it - one with that profession (its own or planned, GuildProfPath's
    // lines) that can learn it now, the highest skill first, the holder itself on a tie; the learner uses the item
    // itself (RunTransfers: the core's CMSG_USE_ITEM). A human holder or learner gets a suggestion
    recipeLearn_.clear();
    {
        std::vector<std::pair<PlanRow const*, Player*>> members;
        for (PlanRow const& row : plan_)
            if (Player* p = Online(row.low); p && p->IsAlive())
                members.emplace_back(&row, p);
        using RecipeScore = std::tuple<bool, uint32, bool, bool>;  // learns now, skill, the holder, a bot
        // [econ1] b350: 10 members held Recipe: Brilliant Smallfish and every copy was mailed to #23 Lanbuzz (19
        // letters, one recipe learned). One copy per (learner, taught spell): the one it holds itself, the one in
        // its mailbox, the one already on its way; a further copy goes to the next best learner or stays
        std::set<std::pair<uint32, uint32>> claimed;
        for (auto const& [row, m] : members)
        {
            ForEachBagItem(m,
                           [&](Item* it)
                           {
                               if (it->GetTemplate()->Class == ITEM_CLASS_RECIPE)
                                   if (uint32 const spell = RecipeSpell(it->GetTemplate()))
                                       claimed.insert({row->low, spell});
                           });
            for (Mail const* mail : m->GetMails())
                if (mail && mail->state != MAIL_STATE_DELETED)
                    for (MailItemInfo const& info : mail->items)
                        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(info.item_template))
                            if (proto->Class == ITEM_CLASS_RECIPE)
                                if (uint32 const spell = RecipeSpell(proto))
                                    claimed.insert({row->low, spell});
        }
        for (auto const& [id, t] : transfers_)
            if (!t.gold && t.mailed)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(t.itemId);
                    proto && proto->Class == ITEM_CLASS_RECIPE)
                    if (uint32 const spell = RecipeSpell(proto))
                        claimed.insert({t.toLow, spell});
        for (auto const& [holderRow, h] : members)
        {
            std::vector<Item*> recipes;
            ForEachBagItem(h,
                           [&](Item* it)
                           {
                               ItemTemplate const* proto = it->GetTemplate();
                               if (proto->Class == ITEM_CLASS_RECIPE && proto->RequiredSkill &&
                                   !used.count(it->GetGUID().GetCounter()))
                                   recipes.push_back(it);
                           });
            for (Item* it : recipes)
            {
                ItemTemplate const* proto = it->GetTemplate();
                uint32 const taught = RecipeSpell(proto);
                uint32 const line = proto->RequiredSkill;
                Player* best = nullptr;
                RecipeScore bestScore{};
                for (auto const& [row, m] : members)
                {
                    bool const planned =
                        std::find(row->profs.begin(), row->profs.end(), line) != row->profs.end() ||
                        std::find(row->secondary.begin(), row->secondary.end(), line) != row->secondary.end();
                    if ((taught && m->HasSpell(taught)) || (!m->HasSkill(line) && !planned) ||
                        m->GetLevel() < proto->RequiredLevel ||
                        (m != h && taught && claimed.count({row->low, taught})))
                        continue;
                    uint32 const skill = m->HasSkill(line) ? m->GetPureSkillValue(line) : 0;
                    RecipeScore const score{m->HasSkill(line) && skill >= proto->RequiredSkillRank, skill, m == h,
                                            !HumanCharacter(m)};
                    if (!best || score > bestScore)
                    {
                        best = m;
                        bestScore = score;
                    }
                }
                if (!best)
                    continue;
                bool const now = std::get<0>(bestScore);
                uint32 const bestLow = best->GetGUID().GetCounter();
                std::string const name = ItemText(proto->ItemId, 1);
                std::string const skillText =
                    std::to_string(std::get<1>(bestScore)) + "/" + std::to_string(proto->RequiredSkillRank);
                if (best == h)
                {
                    if (!now)
                        continue;  // its own, learnable once its skill reaches the rank: kept
                    if (HumanCharacter(h))
                        Suggest(holderRow->low, "r:" + std::to_string(proto->ItemId),
                                "выучи " + name + " (навык " + skillText + ") — в гильдии ты лучший для него.");
                    else
                        recipeLearn_.emplace_back(holderRow->low, it->GetGUID().GetCounter());
                    continue;
                }
                if (taught)
                    claimed.insert({bestLow, taught});
                Want w;
                w.key = "l" + std::to_string(it->GetGUID().GetCounter());
                w.from = holderRow->low;
                w.to = bestLow;
                w.guid = it->GetGUID().GetCounter();
                w.itemId = proto->ItemId;
                w.count = 1;
                w.purpose = "рецепт";
                w.why = "выучит " + Who(bestLow) + " (навык " + skillText +
                        (now ? ", учит сразу)" : ", учит, когда дорастёт)");
                if (HumanCharacter(best) && !HumanCharacter(h))
                    Suggest(bestLow, "r:" + std::to_string(proto->ItemId),
                            name + " придёт тебе почтой от " + Who(holderRow->low) + " — выучи (навык " + skillText +
                                ").");
                wants.push_back(w);
                used.insert(w.guid);
            }
        }
    }

    if (donation != donationState_)
    {
        donationState_ = donation;
        if (!donation.empty())
        {
            logs.push_back("donation plan: " + donation);
            Decision(donation);
        }
    }

    // [human] user rule 2026-11-08 (b349: TRADE_TO Eraelor -> Thamesh Linen Cloth opened trades from the human's
    // character every second, "target does not have space"): nothing moves out of a human's bags by the leader's
    // hand - he gets the suggestion and gives it himself
    wants.erase(std::remove_if(wants.begin(), wants.end(),
                               [&](Want const& w)
                               {
                                   Player* s = Online(w.from);
                                   if (!s || !HumanCharacter(s))
                                       return false;
                                   std::string const what = w.gold ? Gold(w.money) : ItemText(w.itemId, w.count);
                                   Suggest(w.from, "t:" + w.key,
                                           "у тебя " + what + " — нужно " + Who(w.to) + " (" + w.purpose + ": " +
                                               w.why + "). Передай обменом или почтой, если согласен.");
                                   return true;
                               }),
                wants.end());

    // 3. reconcile: keep what still moves (its channel while possible), drop the rest, plan the new ones
    std::map<std::string, Want const*> desired;
    for (Want const& w : wants)
        desired.emplace(w.key, &w);
    for (auto it = transfers_.begin(); it != transfers_.end();)
    {
        Transfer& t = it->second;
        if (t.mailed)
        {
            // in the receiver's mailbox: done once it took it, dropped when the mail is gone (returned / expired)
            Player* r = Online(t.toLow);
            bool arrived = false, pending = !r;  // offline: the mail waits
            if (r)
            {
                arrived = !t.gold && r->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(t.itemGuidLow));
                for (Mail const* m : r->GetMails())
                    if (m && m->state != MAIL_STATE_DELETED && m->sender == t.fromLow &&
                        (t.gold ? m->money == t.money
                                : std::any_of(m->items.begin(), m->items.end(), [&](MailItemInfo const& i)
                                              { return i.item_guid == t.itemGuidLow; })))
                        pending = true;
            }
            if (arrived || !pending)
            {
                if (arrived && t.purpose == "на распыление")
                    bots_[t.toLow].disenchant.push_back(t.itemGuidLow);
                if (!arrived && !t.gold)
                    logs.push_back("mail " + Who(t.fromLow) + " -> " + Who(t.toLow) + ": " +
                                   ItemText(t.itemId, t.count) + " no longer in its mailbox (taken onto a stack, "
                                   "returned or expired)");
                it = transfers_.erase(it);
            }
            else
                ++it;
            continue;
        }
        auto const d = desired.find(t.key);
        // a consumable share may come from another stack of the quartermaster: the same transfer
        // [guild15] a reagent / supply transfer ("r<to>:<item>") is the same while the same member sends the same item:
        // its stack's guid changes when the gatherer loots more onto it (b333: Bami -> Guinudrond Silverleaf, Baridrol
        // -> Dekorhan Linen planned and dropped 4 times, one trade done)
        bool const same = d != desired.end() && d->second->from == t.fromLow && d->second->to == t.toLow &&
                          (t.gold || d->second->tradeOnly || t.key[0] == 's' ||
                           (t.key[0] == 'r' && d->second->itemId == t.itemId) || d->second->guid == t.itemGuidLow);
        if (!same)
        {
            logs.push_back("drop " + std::string(ChannelLog(t.channel)) + " " + Who(t.fromLow) + " -> " +
                           Who(t.toLow) + ": " + (t.gold ? Gold(t.money) : ItemText(t.itemId, t.count)) +
                           " (no longer to move: moved, used, sold or another plan)");
            it = transfers_.erase(it);
            continue;
        }
        // a refused walk is tried again after a level-up of either member (as the other refusal rules)
        if (t.tradeRefused || t.mailRefused)
        {
            Player* s = Online(t.fromLow);
            Player* r = Online(t.toLow);
            uint32 const levels = (s ? s->GetLevel() : 0) << 8 | (r ? r->GetLevel() : 0);
            if (!t.refusedLevels)
                t.refusedLevels = levels;
            else if (s && r && levels != t.refusedLevels)
            {
                t.tradeRefused = t.mailRefused = false;
                t.refusedLevels = 0;
                logs.push_back("retry " + Who(t.fromLow) + " -> " + Who(t.toLow) + ": a level-up, refused walks "
                               "are tried again");
            }
        }
        Choice c = choose(*d->second, &t);
        t.stalled = c.cost == NONE;  // no channel now (refused walks): kept with its refusals, traded if they meet
        if (t.stalled)
        {
            c.channel = t.channel;
            c.walkerLow = 0;
            c.meet = t.meet;
            c.mailbox = t.mailbox;
            c.text += "; нет канала";
        }
        t.itemGuidLow = d->second->guid;
        t.itemId = d->second->itemId;
        if (c.channel != t.channel)
            logs.push_back("switch " + Who(t.fromLow) + " -> " + Who(t.toLow) + " " +
                           ItemText(t.itemId, t.count) + ": " + ChannelLog(t.channel) + " -> " +
                           ChannelLog(c.channel) + " (" + c.text + ")");
        t.channel = c.channel;
        t.walkerLow = c.walkerLow;
        t.meet = c.meet;
        t.mailbox = c.mailbox;
        t.costs = c.text;
        t.money = d->second->money;
        t.count = d->second->count;
        t.why = d->second->why;
        desired.erase(d);
        ++it;
    }
    for (auto const& [key, w] : desired)
    {
        Choice const c = choose(*w, nullptr);
        if (c.cost == NONE)
            continue;  // no channel now: it stays where it is (held until the bank / a meeting)
        Transfer t;
        t.id = nextTransferId_++;
        t.key = key;
        t.gold = w->gold;
        t.channel = c.channel;
        t.fromLow = w->from;
        t.toLow = w->to;
        t.itemGuidLow = w->guid;
        t.itemId = w->itemId;
        t.count = w->count;
        t.money = w->money;
        t.purpose = w->purpose;
        t.why = w->why;
        // [guild15] gear history: the receiver's equip of an upgrade is the global leader's decision
        if (!t.gold && t.purpose == "улучшение")
            GuildGear::HistoryNoteDecision(t.toLow, t.itemId, "leader",
                                           "guild transfer from " + Who(t.fromLow) + ": " + t.why);
        t.costs = c.text;
        t.walkerLow = c.walkerLow;
        t.meet = c.meet;
        t.mailbox = c.mailbox;
        t.since = now;
        std::string const what = t.gold ? Gold(t.money) : ItemText(t.itemId, t.count);
        logs.push_back("plan " + Who(t.fromLow) + " -> " + Who(t.toLow) + ": " + what + " by " +
                       ChannelLog(t.channel) + " (" + t.costs + ") — " + t.purpose + ": " + t.why);
        if (t.purpose != "расходник")
            Decision(Who(t.fromLow) + " → " + Who(t.toLow) + ": " + what + ", " + ChannelName(t.channel) + " (" +
                     t.purpose + "; " + t.costs + ")");
        transfers_.emplace(t.id, std::move(t));
    }
    for (std::string const& text : logs)
        LOG_INFO("playerbots", "[guild81] {}", text);
}

// ---------------------------------------------------------------------------------------------------------------------
// world thread: the game actions (trade next to each other, mail at a mailbox, take the delivered mail)
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::RunTransfers(uint32 diff)
{
    transferTimer_ += diff;
    if (transferTimer_ < IN_MILLISECONDS)
        return;
    transferTimer_ = 0;
    std::vector<Transfer> list;
    std::vector<uint32> pickups;
    std::vector<std::pair<uint32, uint32>> learn;  // [recipes] (member, recipe item guid) it learns now
    {
        std::lock_guard<std::mutex> guard(lock_);
        learn.swap(recipeLearn_);
        for (auto const& [id, t] : transfers_)
            if (t.channel != Transfer::BANK && !t.mailed)
                list.push_back(t);
        for (auto const& [low, pick] : mailPickup_)
            pickups.push_back(low);
    }
    for (auto const& [low, guid] : learn)
    {
        Player* p = Online(low);
        Item* item = p && p->IsAlive() && !HumanCharacter(p)
                         ? p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid))
                         : nullptr;
        if (!item)
            continue;
        std::string const name = ItemText(item->GetEntry(), 1);
        std::string const err = LearnRecipe(p, item);
        LOG_INFO("playerbots", "[recipes] {} learns {}: {}", p->GetName(), name, err.empty() ? "used" : err);
    }
    if (list.empty() && pickups.empty())
        return;

    struct Outcome
    {
        uint32 id;
        bool done;
        bool mailed;
        uint32 deliverAt;
        uint32 newGuid;
        std::string failed;
        uint32 money;  // gold handed over (what the budget left at that moment)
        int32 refusedFree = -1;  // [human] a refused trade: the receiver's free bag slots then
    };
    std::vector<Outcome> outcomes;
    std::vector<std::string> logs;
    uint32 const now = NowSec();
    for (Transfer const& t : list)
    {
        Player* s = Online(t.fromLow);
        if (!s || !s->IsAlive() || s->IsInFlight() || s->GetTradeData() || HumanCharacter(s))
            continue;  // [human] never a human's items (the planner suggests instead)
        Player* r = Online(t.toLow);
        Item* item = t.gold ? nullptr : s->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(t.itemGuidLow));
        if (!t.gold && !item)
            continue;  // the planner drops it
        uint32 money = 0;
        if (t.gold)
        {
            // never below the member's reserve: what the budget leaves now (the postage kept back for mail)
            uint32 const free = FoundingFreeMoney(s);
            uint32 const keep = t.channel == Transfer::MAIL ? MAIL_POSTAGE : 0;
            money = std::min(t.money, free > keep ? free - keep : 0);
            if (!money)
                continue;
        }
        std::string const what = t.gold ? Gold(money) : ItemText(t.itemId, t.count);
        if (t.channel == Transfer::TRADE)
        {
            if (!r || !r->IsAlive() || r->GetTradeData() || s->GetMap() != r->GetMap() ||
                !s->IsWithinDistInMap(r, TRADE_DISTANCE) || HumanCharacter(r))
                continue;
            // [human] refused before with the receiver's bags as they are now: no new try until its free slots change
            int32 const free = int32(r->GetFreeInventorySpace());
            if (t.refusedFree >= 0 && free == t.refusedFree)
                continue;
            if (item)
            {
                ItemPosCountVec dest;
                if (r->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                {
                    outcomes.push_back(
                        {t.id, false, false, 0, 0, "the receiver's bags have no room for it", 0, free});
                    continue;
                }
            }
            if (item && t.count && t.count < item->GetCount())
            {
                item = Split(s, item, t.count);
                if (!item)
                {
                    outcomes.push_back({t.id, false, false, 0, 0, "no free bag slot to split the stack", 0, -1});
                    continue;
                }
            }
            uint32 const guid = item ? item->GetGUID().GetCounter() : 0;
            std::string const err = DoTrade(s, r, item, money);
            if (err.empty())
            {
                logs.push_back("trade " + Who(t.fromLow) + " -> " + Who(t.toLow) + ": " + what + " (" + t.purpose +
                               ": " + t.why + "; " + t.costs + ")");
                outcomes.push_back({t.id, true, false, 0, guid, "", money, -1});
            }
            else
                outcomes.push_back({t.id, false, false, 0, 0, err, 0, int32(r->GetFreeInventorySpace())});
        }
        else if (t.channel == Transfer::MAIL)
        {
            GameObject* box = MailboxAt(s);  // at a mailbox: on its mail walk, or passing by
            if (!box)
                continue;
            std::string name;
            if (r)
                name = r->GetName();
            else if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(t.toLow), name))
                continue;
            uint32 const before = s->GetMoney();
            std::string const err =
                DoMail(s, box, name, item, money, t.gold ? "На вкладку банка гильдии" : "Для тебя: " + t.purpose);
            if (err.empty())
            {
                uint32 deliverAt = now;
                if (r)
                    for (Mail const* m : r->GetMails())
                        if (m && m->sender == t.fromLow)
                            deliverAt = std::max<uint32>(deliverAt, uint32(m->deliver_time));
                logs.push_back("mail " + Who(t.fromLow) + " -> " + Who(t.toLow) + ": " + what + ", postage " +
                               Gold(before - s->GetMoney() - money) + ", delivery in " +
                               std::to_string(deliverAt - now) + " s (" + t.purpose + ": " + t.why + "; " + t.costs +
                               ")");
                outcomes.push_back({t.id, false, true, deliverAt, 0, "", money, -1});
            }
            else
                outcomes.push_back({t.id, false, false, 0, 0, err, 0, -1});
        }
    }
    // the delivered mails of members, taken at the mailbox the receiver stands at
    for (uint32 low : pickups)
    {
        Player* r = Online(low);
        // [human] a human takes his mail himself
        GameObject* box = r && r->IsAlive() && !HumanCharacter(r) ? MailboxAt(r) : nullptr;
        if (!box)
            continue;
        std::vector<uint32> ids;
        for (Mail const* m : r->GetMails())
            if (m && m->state != MAIL_STATE_DELETED && m->messageType == MAIL_NORMAL && !m->COD &&
                IsPlanned(m->sender) && uint32(m->deliver_time) <= now)
                ids.push_back(m->messageID);
        for (uint32 mailId : ids)
        {
            Mail* m = r->GetMail(mailId);
            if (!m)
                continue;
            uint32 const sender = m->sender;
            std::vector<MailItemInfo> const items = m->items;
            uint32 taken = 0;
            std::string got;
            bool full = false;
            for (MailItemInfo const& info : items)
            {
                Item* it = r->GetMItem(info.item_guid);
                std::string const name =
                    it ? ItemText(it->GetEntry(), it->GetCount()) : ItemText(info.item_template, 1);
                // [econ1] b350: Idunk "could not take ... (bags full?)" 11 times a letter, every second at the box -
                // the core's own check first (HandleMailTakeItem: CanStoreItem); full bags end the take here, the
                // rest stays in the mail for the next visit (its vendor visit makes room)
                ItemPosCountVec dest;
                if (it && r->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) != EQUIP_ERR_OK)
                {
                    full = true;
                    break;
                }
                WorldPacket take(CMSG_MAIL_TAKE_ITEM);
                take << box->GetGUID() << mailId << info.item_guid;
                r->GetSession()->HandleMailTakeItem(take);
                // taken = gone from the letter (a stack merged into the bags has no item of that guid any more)
                Mail const* after = r->GetMail(mailId);
                bool const still = after && std::any_of(after->items.begin(), after->items.end(),
                                                        [&](MailItemInfo const& i)
                                                        { return i.item_guid == info.item_guid; });
                if (!still)
                {
                    ++taken;
                    got += (got.empty() ? "" : ", ") + name;
                }
                else
                {
                    logs.push_back(r->GetName() + " could not take " + name + " from the mail of " + Who(sender) +
                                   " (the core refused it)");
                    full = true;
                    break;
                }
            }
            m = r->GetMail(mailId);
            if (m && m->money)
            {
                uint32 const money = m->money;
                WorldPacket take(CMSG_MAIL_TAKE_MONEY);
                take << box->GetGUID() << mailId;
                r->GetSession()->HandleMailTakeMoney(take);
                m = r->GetMail(mailId);
                if (!m || !m->money)
                    got += (got.empty() ? "" : ", ") + Gold(money);
            }
            m = r->GetMail(mailId);
            if (m && m->items.empty() && !m->money)
            {
                WorldPacket del(CMSG_MAIL_DELETE);
                del << box->GetGUID() << mailId << uint32(0);
                r->GetSession()->HandleMailDelete(del);
            }
            if (!got.empty())
                logs.push_back(Who(low) + " takes the mail of " + Who(sender) + " at a mailbox: " + got);
            if (taken)
                LOG_INFO("playerbots", "[econ1] mail collected: {} took {} item(s) from {}", Who(low), taken,
                         Who(sender));
            if (full)
            {
                LOG_INFO("playerbots", "[econ1] mail waits: {} has no bag room for the rest of its mail", Who(low));
                break;
            }
        }
    }

    std::lock_guard<std::mutex> guard(lock_);
    for (Outcome const& o : outcomes)
    {
        auto const it = transfers_.find(o.id);
        if (it == transfers_.end())
            continue;
        Transfer& t = it->second;
        if (t.gold && (o.done || o.mailed))
            t.money = o.money;
        if (o.done)
        {
            if (t.purpose == "на распыление" && o.newGuid)
                bots_[t.toLow].disenchant.push_back(o.newGuid);
            Decision(Who(t.fromLow) + " передал " + Who(t.toLow) + " обменом: " +
                     (t.gold ? Gold(t.money) : ItemText(t.itemId, t.count)) + " (" + t.purpose + ")");
            transfers_.erase(it);
        }
        else if (o.mailed)
        {
            t.mailed = true;
            t.deliverAt = o.deliverAt;
            Decision(Who(t.fromLow) + " отправил почтой " + Who(t.toLow) + ": " +
                     (t.gold ? Gold(t.money) : ItemText(t.itemId, t.count)) + " (" + t.purpose + ")");
        }
        else
        {
            // [human] the receiver's free slots at a refused trade (a new try only once they change)
            if (t.channel == Transfer::TRADE)
                t.refusedFree = o.refusedFree;
            if (o.failed == t.failed)
                continue;
            t.failed = o.failed;
            logs.push_back(std::string(ChannelLog(t.channel)) + " " + Who(t.fromLow) + " -> " + Who(t.toLow) +
                           " refused: " + o.failed);
            // the planner picks another channel next time
            if (t.channel == Transfer::TRADE)
                t.tradeRefused = true;
            else
                t.mailRefused = true;
        }
    }
    for (std::string const& text : logs)
        LOG_INFO("playerbots", "[guild81] {}", text);
}

// ---------------------------------------------------------------------------------------------------------------------
// lock_ held: the orders (UpdateOrders), the walks (NextStep / OnCampReached, map thread), the tabs
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::TransferOrders(uint32 low, Player* p, std::vector<GuildOrder>& orders) const
{
    for (auto const& [id, t] : transfers_)
    {
        if (t.channel == Transfer::BANK || (t.fromLow != low && t.toLow != low))
            continue;
        bool const sender = t.fromLow == low;
        GuildOrder o;
        o.type = t.gold ? GuildOrderType::DONATE_GOLD
                 : t.channel == Transfer::TRADE ? GuildOrderType::TRADE_TO
                                                : GuildOrderType::MAIL_TO;
        o.transferId = t.id;
        o.itemId = t.itemId;
        o.count = t.count;
        o.cost = t.money;
        o.forLow = sender ? t.toLow : t.fromLow;
        std::string const what = t.gold ? Gold(t.money) : ItemText(t.itemId, t.count);
        o.details = sender ? what + " → " + Who(t.toLow) + ", " + ChannelName(t.channel)
                           : "получает " + what + " от " + Who(t.fromLow) + ", " + ChannelName(t.channel);
        o.reason = (t.purpose == "снабжение" ? "SUPPLY " : "") + t.purpose + ": " + t.why + " (" + t.costs + ")";
        if (t.mailed)
        {
            if (sender)
                continue;  // its part is done
            o.status = t.deliverAt > NowSec() ? "в пути, доставка через " +
                                                    std::to_string((t.deliverAt - NowSec() + 59) / 60) + " мин"
                                              : "доставлено: забрать у почтового ящика";
        }
        else if (t.channel == Transfer::MAIL)
        {
            o.executable = sender && !t.stalled;
            o.pos = t.mailbox;
            o.status = sender ? "к почтовому ящику " +
                                    std::to_string(uint32(p->GetExactDist(t.mailbox.GetPositionX(),
                                                                          t.mailbox.GetPositionY(),
                                                                          t.mailbox.GetPositionZ()))) +
                                    " ярд"
                              : "ждёт письмо";
        }
        else
        {
            o.executable = t.walkerLow == low;
            o.pos = t.meet;
            if (!t.walkerLow)
                o.status = "обмен при встрече (≤" + std::to_string(uint32(TRADE_DISTANCE)) + " ярд)";
            else if (t.walkerLow == low)
                o.status = Valid(t.meet) ? "идёт к месту встречи" : "идёт к " + Who(o.forLow);
            else
                o.status = "ждёт: " + Who(t.walkerLow) + " идёт к нему";
        }
        if (!t.failed.empty())
            o.status += "; отказ: " + t.failed;
        orders.push_back(o);
    }
    // the receiver's delivered mails: taken at the cheapest mailbox
    auto const pick = mailPickup_.find(low);
    if (pick != mailPickup_.end())
    {
        GuildOrder o;
        o.type = GuildOrderType::MAIL_TO;
        o.count = pick->second.first;
        o.pos = pick->second.second;
        o.details = "забрать почту: " + std::to_string(o.count) + " писем от гильдии";
        o.reason = "доставлено ядром";
        o.status = "к почтовому ящику " +
                   std::to_string(uint32(p->GetExactDist(o.pos.GetPositionX(), o.pos.GetPositionY(),
                                                         o.pos.GetPositionZ()))) +
                   " ярд";
        o.executable = true;
        orders.push_back(o);
    }
}

bool GuildCoordinator::TransferHolds(uint32 itemGuidLow) const
{
    for (auto const& [id, t] : transfers_)
        if (!t.gold && t.channel != Transfer::BANK && t.itemGuidLow == itemGuidLow)
            return true;
    return false;
}

// map thread
bool GuildCoordinator::TransferStep(Player* bot, BotState& st, GuildOrder& order, GuildRunStep& step)
{
    uint32 const low = bot->GetGUID().GetCounter();
    Transfer* t = nullptr;
    if (order.transferId)
    {
        auto const it = transfers_.find(order.transferId);
        if (it == transfers_.end() || it->second.mailed)
            return false;
        t = &it->second;
    }
    WorldPosition dest = order.pos;
    std::string what = "mailbox";
    if (t && t->channel == Transfer::TRADE)
    {
        if (t->walkerLow != low)
            return false;
        Player* other = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(
            t->fromLow == low ? t->toLow : t->fromLow));
        if (!other || other->GetMap() != bot->GetMap())
            return false;
        if (!Valid(dest))
            dest = Pos(other);  // walks to where it stands (re-aimed after each arrival)
        what = "trade with " + other->GetName();
    }
    if (!Valid(dest) || dest.GetMapId() != bot->GetMapId())
        return false;
    float const dist = bot->GetExactDist(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    // a solo bot walks only a safe walk ([guild14]); none: this channel is off, the planner picks another
    if (dist > CAMP_REACH && NewRpgBaseAction::TripRefused(bot, dest.GetPositionX(), dest.GetPositionY(),
                                                           dest.GetPositionZ(), what))
    {
        order.executable = false;
        order.status = "нет безопасного пути — квестует";
        if (t)
        {
            if (t->channel == Transfer::TRADE)
                t->tradeRefused = true;
            else
                t->mailRefused = true;
        }
        LOG_INFO("playerbots", "[guild81] {} L{}: no safe walk for the {} ({} yd), the channel is off", bot->GetName(),
                 bot->GetLevel(), what, uint32(dist));
        return false;
    }
    st.run = true;
    st.runType = order.type;
    st.runPos = dest;
    st.runApproached = false;
    step.kind = GuildRunStep::WALK;
    step.pos = dest;
    char const* const name = order.type == GuildOrderType::TRADE_TO  ? "TRADE_TO"
                             : order.type == GuildOrderType::MAIL_TO ? "MAIL_TO"
                                                                     : "DONATE_GOLD";
    step.reason = std::string(name) + " " + order.details + ", " + std::to_string(uint32(dist)) + " yd";
    return true;
}

// map thread
uint8 GuildCoordinator::TransferCampReached(Player* bot, BotState& st)
{
    uint32 const low = bot->GetGUID().GetCounter();
    GuildOrder const* order = nullptr;
    for (GuildOrder const& o : st.orders)
        if (o.type == st.runType && o.executable && !order)
            order = &o;
    if (!order)
    {
        st.run = false;
        return 1;  // handed over (or called off): back to its own work
    }
    auto const it = order->transferId ? transfers_.find(order->transferId) : transfers_.end();
    bool const trade = it != transfers_.end() && it->second.channel == Transfer::TRADE;
    if (!trade)
    {
        // at the mailbox: step up to it; the world thread mails / takes the mail
        if (!st.runApproached)
        {
            st.runApproached = true;
            bot->GetMotionMaster()->MovePoint(0, st.runPos.GetPositionX(), st.runPos.GetPositionY(),
                                              st.runPos.GetPositionZ());
            return 2;
        }
        if (bot->isMoving() || MailboxAt(bot))
            return 2;
        st.run = false;
        if (it != transfers_.end())
            it->second.mailRefused = true;
        LOG_INFO("playerbots", "[guild81] {} L{}: cannot reach the mailbox, the mail channel is off", bot->GetName(),
                 bot->GetLevel());
        return 1;
    }
    Transfer const& t = it->second;
    Player* other = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(t.fromLow == low ? t.toLow
                                                                                                     : t.fromLow));
    if (other && other->GetMap() == bot->GetMap())
    {
        if (bot->IsWithinDistInMap(other, TRADE_DISTANCE))
            return 2;  // next to it: the world thread trades within a second
        if (Valid(t.meet))
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(other))
                if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&ai->rpgInfo.data))
                    if (Dist(camp->pos, t.meet) <= CAMP_REACH)
                        return 2;  // the meeting point is where the other walks: it comes
    }
    st.run = false;  // it moved on: the next step aims at where it is now
    return 1;
}

std::string GuildCoordinator::TransferWait(uint32 /*low*/, BotState const& st) const
{
    if (!st.run)
        return "";
    if (st.runType == GuildOrderType::TRADE_TO)
        return "trade: waits for the other member";
    if (st.runType == GuildOrderType::MAIL_TO || st.runType == GuildOrderType::DONATE_GOLD)
        return "at the mailbox";
    return "";
}

void GuildCoordinator::TransferPlanRows(std::vector<std::vector<std::string>>& rows) const
{
    for (auto const& [id, t] : transfers_)
    {
        std::string const what = t.gold ? Gold(t.money) : ItemText(t.itemId, t.count);
        uint32 const left = t.deliverAt > NowSec() ? (t.deliverAt - NowSec() + 59) / 60 : 0;
        std::string state = t.mailed                      ? (left ? "в почте, доставка через " + std::to_string(left) +
                                                                       " мин"
                                                                  : "в почте: забрать")
                            : t.channel == Transfer::BANK ? "через банк"
                            : t.channel == Transfer::MAIL ? "к почтовому ящику"
                            : t.walkerLow                 ? "идёт " + Who(t.walkerLow)
                                                          : "при встрече";
        if (!t.failed.empty())
            state += "; отказ";
        rows.push_back({"передача", state, Who(t.fromLow), what, Who(t.toLow),
                        std::string(t.gold ? "взнос, " : "") + ChannelName(t.channel),
                        t.purpose + ": " + t.why + " (" + t.costs + ")"});
    }
    if (!donationState_.empty())
        rows.push_back({"деньги", "взносы", "члены гильдии", "вкладка банка 1", "глава гильдии", "почта / обмен",
                        donationState_});
}
