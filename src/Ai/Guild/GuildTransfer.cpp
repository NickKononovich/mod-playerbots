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
#include "BuyAction.h"  // [share92] RESTOCK_FOOD_MIN
#include "NewRpgProfession.h"  // [roles62] NeedsRespec
#include "NewRpgQuestRoute.h"  // [inv372] the route a pickup's mailbox is measured against
#include "QuestStats.h"  // [log28]

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
#include "Group.h"  // [econ2] a party follower takes no letter
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
#include "PlayerbotAIConfig.h"  // [craft386] sight distance
#include "Playerbots.h"
#include "SpellInfo.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"  // [craft386]
#include "Trainer.h"  // [econ2] the profession trainers
#include "TradeData.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GuildLedger.h"
#include "GuildRiding.h"  // [mount4]

namespace
{
    constexpr float CAMP_REACH = 10.0f;      // NewRpgStatusUpdateAction's GO_CAMP arrival distance
    constexpr uint32 MAIL_POSTAGE = 30;      // core HandleSendMail: 30 copper per item, 30 without items
    constexpr float NONE = FLT_MAX;
    constexpr uint8 FOOD = 1, DRINK = 2;

    uint32 NowSec() { return uint32(GameTime::GetGameTime().count()); }

    // [dun194] a member's line inside the run: the drinks the run needs it to carry (priced at the five's forming),
    // else the restock line
    uint32 RunLine(GuildFive const& five, uint32 low, uint8 kind)
    {
        if (kind == DRINK)
            if (auto const priced = five.drinkNeed.find(low); priced != five.drinkNeed.end())
                return priced->second;
        return RESTOCK_FOOD_MIN;
    }

    std::string Gold(uint32 copper)
    {
        uint32 const g = copper / 10000, s = (copper / 100) % 100, c = copper % 100;
        std::string out;
        if (g)
            out += std::to_string(g) + " g ";
        if (g || s)
            out += std::to_string(s) + " s ";
        out += std::to_string(c) + " c";
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

    std::string Sec(float s) { return s == NONE ? "—" : std::to_string(uint32(s + 0.5f)) + " s"; }

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

    // [recipes] the bot uses its recipe item as a player does (CMSG_USE_ITEM on itself); "" when the Learning cast
    // began (or the spell is known), "-" when its own Learning cast still runs (nothing to log)
    std::string LearnRecipe(Player* p, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 const taught = RecipeSpell(proto);
        uint32 const spellId = uint32(std::max<int32>(proto->Spells[0].SpellId, 0));
        if (Spell const* current = p->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            current && current->GetSpellInfo() && current->GetSpellInfo()->Id == spellId)
            return "-";  // the 3 s Learning cast of the last use still runs
        if (taught && p->HasSpell(taught))
            return "";
        if (p->IsInCombat() || p->IsNonMeleeSpellCast(false) || p->IsInFlight())
            return "cannot use it now: combat / a cast / a flight";
        // [econ2] the core's own refusal, named: b353 22 "the core did not teach it" were the item check passing
        // and the cast refused afterwards. The reasons the core gives (Player::CanUseItem)
        switch (p->CanUseItem(item))
        {
            case EQUIP_ERR_OK:
                break;
            case EQUIP_ERR_NO_REQUIRED_PROFICIENCY:
                return "cannot use it now: the profession is not learned";
            case EQUIP_ERR_CANT_EQUIP_SKILL:
                return "cannot use it now: skill " + std::to_string(p->GetSkillValue(proto->RequiredSkill)) +
                       " below " + std::to_string(proto->RequiredSkillRank);
            case EQUIP_ERR_CANT_EQUIP_LEVEL_I:
                return "cannot use it now: level below " + std::to_string(proto->RequiredLevel);
            default:
                return "cannot use it now: the item check refuses it";
        }
        // [econ2] b353 Geron 16 / Geefeelli 7 "the core did not teach it", then "used" once it stood: the Learning
        // spell (483) has a 3 s cast with SPELL_INTERRUPT_FLAG_MOVEMENT, and the core refuses a cast-time spell of a
        // moving player (Spell::CheckCast SPELL_FAILED_MOVING) - the use came while it walked its quest route. A
        // player stops to use it: the bot stops, then uses it (its walk waits for the cast: the motion generators
        // hold while a movement-interrupted cast prepares, Unit::IsMovementPreventedByCasting)
        if (p->isMoving())
            p->StopMoving();
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
        if (!taught || p->HasSpell(taught))
            return "";
        if (Spell const* current = p->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            current && current->GetSpellInfo() && current->GetSpellInfo()->Id == spellId)
            return "";
        return std::string("the core did not teach it") + (p->isMoving() ? " (still moving)" : "");
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

    // [craft382] Spell::CheckCast (SPELL_EFFECT_MILLING / PROSPECTING): one cast takes a bag stack of five
    constexpr uint32 CONVERSION_STACK = 5;
    // [craft382] the free slots a crafter keeps for its own loot when the guild fills its bags with materials
    constexpr uint32 CRAFTER_ROOM = 2;

    // [econ1] the free slots of the general bags (backpack + bags without a profession family)
    uint32 FreeSlots(Player* p)
    {
        uint32 n = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            n += p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) ? 0 : 1;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                if (!bag->GetTemplate()->BagFamily)
                    for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                        n += bag->GetItemByPos(uint8(j)) ? 0 : 1;
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
            GuildLedger::Cause const ledger("trade (gold between members)");  // [eco190]
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

    char const* ChannelName(uint8 channel) { return channel == 0 ? "trade" : channel == 1 ? "mail" : "bank"; }

    char const* ChannelLog(uint8 channel) { return channel == 0 ? "trade" : channel == 1 ? "mail" : "bank"; }

    // [inv370] who sent a game letter: the creature / game object by its name, the auction house
    std::string GameSender(Mail const* m)
    {
        if (m->messageType == MAIL_CREATURE)
            if (CreatureTemplate const* c = sObjectMgr->GetCreatureTemplate(m->sender))
                return c->Name;
        if (m->messageType == MAIL_GAMEOBJECT)
            if (GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(m->sender))
                return go->name;
        return m->messageType == MAIL_AUCTION ? "the auction house" : "entry " + std::to_string(m->sender);
    }

    // [inv372] the yards a visit to x adds to the bot's way: from where it stands along its route's stops (the
    // cheapest leg to put it in, or after the last one); no route: there from where it stands
    float RouteDetour(Player* p, std::vector<QuestRoute::Point> const& stops, WorldPosition const& x)
    {
        WorldPosition a = Pos(p);
        float best = Dist(a, x);
        for (QuestRoute::Point const& s : stops)
        {
            WorldPosition const b(p->GetMapId(), s.x, s.y, s.z);
            best = std::min(best, std::max(0.0f, Dist(a, x) + Dist(x, b) - Dist(a, b)));
            a = b;
        }
        return std::min(best, Dist(a, x));
    }
}

bool GuildCoordinator::GameLetter(Mail const* m)
{
    return m && (m->messageType == MAIL_CREATURE || m->messageType == MAIL_GAMEOBJECT ||
                 m->messageType == MAIL_AUCTION);
}

// [inv370] b446: Gubber Blump's Shiny Bauble 43239 (no price, no quest, no use) is not worth a slot; the race
// trainer's Riding Training Pamphlet (no price) opens "Learn to Ride"
bool GuildCoordinator::LetterItemWorth(Player* p, ItemTemplate const* proto)
{
    if (!p || !proto)
        return false;
    if (proto->SellPrice || proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER ||
        proto->Class == ITEM_CLASS_RECIPE)
        return true;
    if (proto->StartQuest)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(proto->StartQuest))
            if (p->GetQuestStatus(proto->StartQuest) == QUEST_STATUS_NONE && p->CanTakeQuest(quest, false))
                return true;
    if (p->HasQuestForItem(proto->ItemId))
        return true;
    return proto->InventoryType != INVTYPE_NON_EQUIP && p->CanUseItem(proto) == EQUIP_ERR_OK;
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
    // [inv373] a town of a contested zone has no side in AreaTable (Splintertree Post, Hammerfall, Stonard,
    // Bloodvenom Post: team 0 in a team-0 zone), so its mailbox counted as friendly for both: b446 Alliance Bami's 4
    // letters waited "MAIL_TO waits town Splintertree Post". Its side: the service NPCs spawned in its area
    // (innkeeper, vendor, flight master, repairer) hostile to one side's players and to none of the other's
    std::map<uint32, std::pair<uint32, uint32>> hostile;  // area -> NPCs hostile to (the Alliance, the Horde)
    for (Mailbox const& box : mailboxes_)
        if (box.areaId && !box.areaTeam)
            hostile[box.areaId];
    ChrRacesEntry const* human = sChrRacesStore.LookupEntry(RACE_HUMAN);
    ChrRacesEntry const* orc = sChrRacesStore.LookupEntry(RACE_ORC);
    FactionTemplateEntry const* ally = human ? sFactionTemplateStore.LookupEntry(human->FactionID) : nullptr;
    FactionTemplateEntry const* horde = orc ? sFactionTemplateStore.LookupEntry(orc->FactionID) : nullptr;
    constexpr uint32 SERVICE = UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_FLIGHTMASTER |
                               UNIT_NPC_FLAG_REPAIR;
    if (ally && horde && !hostile.empty())
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            CreatureTemplate const* c = sObjectMgr->GetCreatureTemplate(data.id);
            if (!c || !(c->npcflag & SERVICE))
                continue;
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            if (!map || map->Instanceable())
                continue;
            auto const town = hostile.find(
                sMapMgr->GetAreaId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ));
            FactionTemplateEntry const* f = sFactionTemplateStore.LookupEntry(c->faction);
            if (town == hostile.end() || !f)
                continue;
            town->second.first += f->IsHostileTo(*ally) ? 1 : 0;
            town->second.second += f->IsHostileTo(*horde) ? 1 : 0;
        }
    uint32 sided = 0;
    for (std::vector<Mailbox>* list : {&mailboxes_, &forges_})
        for (Mailbox& box : *list)
        {
            auto const town = hostile.find(box.areaId);
            if (box.areaTeam || town == hostile.end() || (town->second.first > 0) == (town->second.second > 0))
                continue;
            box.areaTeam = town->second.first ? AREATEAM_HORDE : AREATEAM_ALLY;
            ++sided;
            if (list == &mailboxes_)
                if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(box.areaId))
                    LOG_INFO("playerbots", "[inv373] mailbox at {} ({:.0f},{:.0f}): a {} town by its NPCs ({} hostile "
                             "to the Alliance, {} to the Horde)", area->area_name[0], box.pos.GetPositionX(),
                             box.pos.GetPositionY(), box.areaTeam == AREATEAM_HORDE ? "Horde" : "Alliance",
                             town->second.first, town->second.second);
        }
    LOG_INFO("playerbots", "[inv373] {} mailboxes / forges of contested-zone towns given their side", sided);
    LOG_INFO("playerbots", "[econ1] smelting: {} town forges on the continents", forges_.size());
    // [econ2] b353 Ryarii L7 walked 3378 yd (and took the boat) to learn Mining, Namwe 1556 yd for Herbalism: the
    // TRAIN call had no place, so it ran wherever the bot stood. The trainers of every skill line (a tradeskill
    // trainer teaching one of its ranks or recipes) and the Fishing Pole vendors: the call waits for the bot's visit
    // to the town of the nearest one
    trainerSpots_.clear();
    toolVendors_.clear();
    // the professions' tools (NewRpgProfession's list): Mining Pick, Skinning Knife, Blacksmith Hammer, Arclight
    // Spanner, Jeweler's Kit, Virtuoso Inking Set, Fishing Pole
    std::set<uint32> const tools{2901, 7005, 5956, 6219, 20815, 39505, 6256, 6217};  // [craft400] Copper Rod
    auto const spotOf = [](CreatureData const& data)
    {
        NpcSpot n;
        n.entry = data.id;
        n.spot.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
        n.spot.areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        n.spot.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(n.spot.zoneId))
            n.spot.capital = zone->flags & AREA_FLAG_CAPITAL;
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(n.spot.areaId))
        {
            n.spot.areaTeam = area->team;
            if (!n.spot.areaTeam && area->zone)
                if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(area->zone))
                    n.spot.areaTeam = zone->team;
        }
        return n;
    };
    std::unordered_map<uint32, std::set<uint32>> linesOf;  // trainer entry -> skill lines it teaches
    auto const trainerLines = [&](uint32 entry) -> std::set<uint32> const&
    {
        auto itr = linesOf.find(entry);
        if (itr != linesOf.end())
            return itr->second;
        std::set<uint32>& lines = linesOf[entry];
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
        if (!trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill)
            return lines;
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            if (spell.ReqSkillLine)
                lines.insert(spell.ReqSkillLine);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId);
            if (!info)
                continue;
            std::vector<uint32> ids{spell.SpellId};
            for (SpellEffectInfo const& effect : info->Effects)
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    ids.push_back(effect.TriggerSpell);
            for (uint32 id : ids)
                if (SpellInfo const* rank = sSpellMgr->GetSpellInfo(id))
                    for (SpellEffectInfo const& effect : rank->Effects)
                        if ((effect.Effect == SPELL_EFFECT_SKILL_STEP || effect.Effect == SPELL_EFFECT_SKILL) &&
                            effect.MiscValue > 0)
                            lines.insert(uint32(effect.MiscValue));
        }
        return lines;
    };
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
        MapEntry const* map = sMapStore.LookupEntry(data.mapid);
        if (!tmpl || !map || map->Instanceable())
            continue;
        if (tmpl->npcflag & UNIT_NPC_FLAG_TRAINER)
            for (uint32 line : trainerLines(data.id))
                trainerSpots_[line].push_back(spotOf(data));
        if (tmpl->npcflag & UNIT_NPC_FLAG_VENDOR)
            if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id))
                for (VendorItem const* item : items->m_items)
                    if (item && tools.count(item->item) && !item->ExtendedCost)
                        toolVendors_[item->item].push_back(spotOf(data));
    }
    size_t trainers = 0;
    for (auto const& [line, list] : trainerSpots_)
        trainers += list.size();
    size_t vendors = 0;
    for (auto const& [item, list] : toolVendors_)
        vendors += list.size();
    LOG_INFO("playerbots", "[econ2] profession towns: {} trainer spawns over {} skill lines, {} tool vendor spawns",
             trainers, trainerSpots_.size(), vendors);
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
    CoordLock guard(lock_, __func__);
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
        std::string mailNo;  // [econ2] why no letter goes (the receiver cannot take it, it is worth less than postage)
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
    // [inv372] the friendly mailbox nearest the bot's route (RouteDetour: the yards a visit adds to its quest stops as
    // last built), not the straight-line nearest from where it stands: [lag344] b445 that one was kept as the pickup's
    // after the bot quested away from it, a town stop the route put last and rebuilt before it was reached. Once per
    // bot and plan (memo)
    struct RouteBox
    {
        std::vector<QuestRoute::Point> stops;
        WorldPosition box{};
        float yd{NONE};
    };
    std::map<uint32, RouteBox> routeBoxes;
    auto const RouteBoxOf = [&](Player* p) -> RouteBox const&
    {
        uint32 const low = p->GetGUID().GetCounter();
        if (auto const found = routeBoxes.find(low); found != routeBoxes.end())
            return found->second;
        RouteBox& rb = routeBoxes[low];
        rb.stops = QuestRoute::RouteStops(low);
        for (Mailbox const& box : mailboxes_)
        {
            if (box.pos.GetMapId() != p->GetMapId())
                continue;
            if ((box.areaTeam == AREATEAM_ALLY && p->GetTeamId() != TEAM_ALLIANCE) ||
                (box.areaTeam == AREATEAM_HORDE && p->GetTeamId() != TEAM_HORDE))
                continue;
            if (TownRefused(low, box.pos))
                continue;  // [econ1] its walk there was refused / given up at this level
            float const d = RouteDetour(p, rb.stops, box.pos);
            if (d < rb.yd)
            {
                rb.yd = d;
                rb.box = box.pos;
            }
        }
        return rb;
    };
    auto const NearestFriendlyMailbox = [&](Player* p) { return RouteBoxOf(p).box; };
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
    needGoldMail_.clear();
    needGoldOnWay_.clear();
    for (PlanRow const& row : plan_)
    {
        Player* p = Online(row.low);
        if (!p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p))
            continue;
        uint32 delivered = 0;
        uint32 goldIn = 0;  // [eco199] the members' gold delivered to its mailbox
        for (Mail const* m : p->GetMails())
        {
            if (!m || m->state == MAIL_STATE_DELETED || m->COD || (m->items.empty() && !m->money))
                continue;
            // [inv370] b446: 59 of the members' 73 unread letters were the game's own (43 Riding Training Pamphlets
            // of the level-20 mail, 18 Shiny Baubles, 2 of the Postmaster's recovered quest rewards - Peny's Old
            // Blanchy's Feed Pouch 56 h) and the pickup counted members' letters only: no task, never taken. A game
            // letter counts when it holds gold or an item worth its bag slot
            bool const game = GameLetter(m);
            if (!game && (m->messageType != MAIL_NORMAL || !IsPlanned(m->sender)))
                continue;
            bool worthItems = !game && !m->items.empty();
            for (MailItemInfo const& info : m->items)
                worthItems |= game && LetterItemWorth(p, sObjectMgr->GetItemTemplate(info.item_template));
            if (game && !m->money && !worthItems)
                continue;  // nothing in it worth a stop: cleared at the next mailbox it stands at (RunTransfers)
            std::string const from = game ? GameSender(m) : Who(m->sender);
            uint8 bag = 0, slot = 0;
            if (uint32(m->deliver_time) <= now)
                goldIn += m->money;
            if (uint32(m->deliver_time) <= now && (m->money || !worthItems || FreeSlot(p, bag, slot)))
                ++delivered;  // bags full: an item mail waits (no stand at the mailbox it cannot empty)
            else if (uint32(m->deliver_time) <= now)
                mailTransit_[row.low].push_back(from + ": delivered, bags full");
            else
                mailTransit_[row.low].push_back(from + ": in " +
                                                std::to_string((uint32(m->deliver_time) - now + 59) / 60) + " min");
        }
        WorldPosition box;
        uint32 keepArea = 0;
        if (auto const last = lastPickup.find(row.low); last != lastPickup.end())
            keepArea = sMapMgr->GetAreaId(PHASEMASK_NORMAL, last->second.second.GetMapId(),
                                          last->second.second.GetPositionX(), last->second.second.GetPositionY(),
                                          last->second.second.GetPositionZ());
        // [eco199] coordinator 2026-11-11 (b430: 27.3 g in Farret's 19 letters, a five leader that never reached a
        // mailbox): gold waiting there for its planned need (its repair, its supplies, its role spec's unlearn) is
        // taken at its next mailbox chance - a town stop on its route, a five member's too while its five gathers,
        // and the five's leader before its five goes in when a mailbox is on its way (FiveStep, the five's check)
        bool needGold = false;
        if (auto const st = bots_.find(row.low); goldIn && st != bots_.end())
        {
            needGold = st->second.repairShort || st->second.supplyGoldShort;
            if (!needGold)
                if (uint32 const roleSpec = RoleSpec(row.low, 0);
                    roleSpec && NewRpgProfession::NeedsRespec(p, roleSpec) && p->GetMoney() < p->resetTalentsCost())
                    needGold = true;
            if (needGold)
                needGoldMail_.insert(row.low);
        }
        if (delivered && nearestMailbox(p, box, keepArea) < NONE)
        {
            mailPickup_[row.low] = {delivered, box};
            if (needGold)
                needGoldOnWay_.insert(row.low);
        }
        // [econ1] b350: 143 letters lay in the members' mailboxes (Lanbuzz 19, Emdyl 11) and 15 were ever taken -
        // the pickup was planned only while a mailbox lay on the bot's way right then, so a bot whose walks never
        // passed one never got the task. No mailbox on its way now: its nearest friendly town mailbox, taken on
        // its next visit there (a convenient town task: the route's town stop, no special trip)
        // [town14] ... and the mailbox once picked stays the pickup's while its walk is not refused: the nearest by the
        // straight line changes as the bot walks (b354 Rollothan L10: "mailbox Goldshire" became Lakeshire, then
        // Darkshire as it quested east - each trip refused through Redridge Mongrels / Venom Web Spiders, 7 letters
        // never taken)
        // [inv372] ... the mailbox nearest its route now (RouteBoxOf), the last one kept only while it lies as near
        // (a tie): the straight-line nearest of [town14] was kept after the bot quested away from it
        else if (delivered)
        {
            auto const last = lastPickup.find(row.low);
            RouteBox const& rb = RouteBoxOf(p);
            WorldPosition pick = rb.box;
            float pickYd = rb.yd;
            if (last != lastPickup.end() && Valid(last->second.second) &&
                last->second.second.GetMapId() == p->GetMapId() && !TownRefused(row.low, last->second.second))
                if (float const lastYd = RouteDetour(p, rb.stops, last->second.second); lastYd <= pickYd)
                {
                    pick = last->second.second;
                    pickYd = lastYd;
                }
            if (Valid(pick))
            {
                if (last == lastPickup.end() || last->second.second != pick)
                {
                    uint32 const area = sMapMgr->GetAreaId(PHASEMASK_NORMAL, pick.GetMapId(), pick.GetPositionX(),
                                                           pick.GetPositionY(), pick.GetPositionZ());
                    AreaTableEntry const* entry = sAreaTableStore.LookupEntry(area);
                    LOG_INFO("playerbots", "[inv372] {} L{} mail pickup ({} letters) at the mailbox of {} "
                             "({:.0f},{:.0f}): {:.0f} yd off its route of {} quest stops{}", p->GetName(),
                             p->GetLevel(), delivered, entry ? entry->area_name[0] : "?", pick.GetPositionX(),
                             pick.GetPositionY(), pickYd, rb.stops.size(),
                             last != lastPickup.end() ? " (the last one lies farther off it)" : "");
                }
                mailPickup_[row.low] = {delivered, pick};
            }
        }
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
            w.purpose = h.purpose.rfind("upgrade", 0) == 0      ? "upgrade"
                        : h.purpose.rfind("craft", 0) == 0      ? "product"
                        : h.purpose.rfind("disenchant", 0) == 0 ? "to disenchant"
                                                                : "item";
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
        auto const calls = professions_.find(row.low);
        for (auto const& [line, plan] : skillPlan_)
        {
            // [econ2] only a line the leader calls it to craft (b353: Linen Cloth letters for First Aid skill-ups
            // of members questing on)
            if (!c->HasSkill(line) || calls == professions_.end() ||
                std::none_of(calls->second.begin(), calls->second.end(),
                             [&](ProfessionPick const& pick)
                             { return pick.skillLine == line && pick.kind == GuildProfessionKind::CRAFT; }))
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
                {
                    lack[{row.low, item}] = {n - have,
                                             "skill-up " + std::to_string(value) + ": " + ItemText(r.product, 1)};
                    // [lag341] ... and what it is made of (an ink of the pigment milled from herbs: ExpandReagent, the
                    // vendors on its map, [lag329]): nobody holds the intermediate, the members hold the herbs. b445:
                    // 73-80 Silverleaf in members' bags "held until the guild bank" (no tab), 0 sent to Dekorhan, the
                    // scribe called for 16 glyph orders - his Ivory Ink's Alabaster Pigment lacked, not the herb.
                    // A mill takes a whole stack: what the members hold goes, the transfer one holding at a time
                    std::map<uint32, double> raw;
                    ExpandReagent(item, double(n - have), raw, nullptr, 1.0f, 0, 0, c->GetMapId());
                    for (auto const& [rawItem, units] : raw)
                    {
                        if (rawItem == item)
                            continue;
                        // [craft437] an ore goes only to a crafter that smelts it: one without Mining lacks the bar
                        // (a miner holding the ore smelts it for the guild, GuildSmelt). b469 20+ letters of Copper
                        // Ore to the jeweler Colian (no Mining), none of it ever a bar
                        if (!c->HasSkill(SKILL_MINING))
                        {
                            uint32 bar = 0;
                            float per = 0.0f;
                            for (auto const& [smelted, parts] : smeltFrom_)
                                for (auto const& [ore, n] : parts)
                                    if (ore == rawItem && n > 0.0f && !bar)
                                    {
                                        bar = smelted;
                                        per = n;
                                    }
                            if (bar)
                            {
                                uint32 const bars = uint32(std::ceil(units / per));
                                uint32 const have = c->GetItemCount(bar, false);
                                if (bar != item && bars > have)
                                    lack.try_emplace({row.low, bar}, bars - have,
                                                     "skill-up " + std::to_string(value) + ": " +
                                                         ItemText(r.product, 1) + " via " + ItemText(item, 1) +
                                                         " [craft437]");
                                continue;
                            }
                        }
                        uint32 const own = c->GetItemCount(rawItem, false);
                        uint32 want = double(own) < units ? uint32(std::ceil(units - double(own))) : 0;
                        // [craft382] the walk's whole lack to its held rank cap (UpdateWanted's crafterLack_), not
                        // one craft's, in whole conversion stacks (Spell::CheckCast: a mill / prospect takes five of
                        // one raw), within its bag room. b446: one Scroll of Stamina wanted "Silverleaf x2" - Ainne's
                        // 2 and Thyllyn's 1 could never be milled, and nothing more went while they were on the way,
                        // with 155 Silverleaf, 140 Peacebloom and 54 Earthroot in the members' bags
                        if (auto const walk = crafterLack_.find(row.low); walk != crafterLack_.end())
                            if (auto const walkUnits = walk->second.find(rawItem); walkUnits != walk->second.end())
                                want = std::max(want, walkUnits->second);
                        ItemTemplate const* rawProto = sObjectMgr->GetItemTemplate(rawItem);
                        if (!want || !rawProto)
                            continue;
                        if (rawProto->HasFlag(ITEM_FLAG_IS_MILLABLE) || rawProto->HasFlag(ITEM_FLAG_IS_PROSPECTABLE))
                            want = (own + want + CONVERSION_STACK - 1) / CONVERSION_STACK * CONVERSION_STACK - own;
                        uint32 const stack = std::max<uint32>(1, rawProto->GetMaxStackSize());
                        uint32 const slots = FreeSlots(c);
                        uint32 const room = (slots > CRAFTER_ROOM ? slots - CRAFTER_ROOM : 0) * stack +
                                            (own % stack ? stack - own % stack : 0);
                        want = std::min(want, room);
                        if (want)
                            lack.try_emplace({row.low, rawItem}, want,
                                             "skill-up " + std::to_string(value) + ": " + ItemText(r.product, 1) +
                                                 " via " + ItemText(item, 1) + " [lag341][craft382]");
                    }
                }
        }
        // [smelt] a miner's next smelt that still gives it skill: the ore goes to it (it smelts and levels Mining)
        std::vector<std::pair<uint32, uint32>> ores;
        uint32 bar = 0;
        if (NextSmelt(c, &ores, &bar))
            for (auto const& [ore, n] : ores)
                if (uint32 const have = c->GetItemCount(ore, false); have < n)
                    lack[{row.low, ore}] = {n - have, "smelt (mining " +
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
                lack[{job.crafterLow, item}] = {n - have,
                                                "craft " + ItemText(job.itemId, 1) + " for " + Who(job.forLow)};
    }
    // [supply112] the guild cook's supply crafts: their reagents go to it (the guild fishers' fish, the looted meat)
    // [eco194] ... and the alchemists' potion jobs: the herbs other members hold go to them
    for (auto const* crafts : {&supplyCrafts_, &potionCrafts_})
        for (auto const& [cookLow, jobs] : *crafts)
        {
            Player* c = Online(cookLow);
            if (!c)
                continue;
            for (SupplyCraft const& job : jobs)
            {
                uint32 const owed = c->HasSpell(job.spellId) ? SupplyOwed(c, job) : 0;
                for (auto const& [item, n] : job.reagents)
                    if (uint32 const have = c->GetItemCount(item, false); owed && have < owed * n)
                        lack[{cookLow, item}] = {owed * n - have, "supply craft " + ItemText(job.product, owed) +
                                                                    " for band " + job.band};
            }
        }
    std::set<std::pair<uint32, uint32>> ownNeed;
    std::map<std::pair<uint32, uint32>, uint32> incoming;  // [craft382] (crafter, item) -> units in its mailbox
    for (auto const& [key, need] : lack)
        ownNeed.insert(key);
    for (auto const& [id, t] : transfers_)
        if (t.mailed && !t.gold)
            incoming[{t.toLow, t.itemId}] += std::max<uint32>(1, t.count);  // already in its mailbox
    // [craft388] what the members hold of a lacked material: every stack in their bags not held for a member, not
    // pending the leader's decision and not sold off - not only the stacks the loot hook registered (st.held) since
    // the server started, and each at its count now. b447: 155 Silverleaf in 24 members' bags, three stacks of 3, 1
    // and 2 ever offered to Dekorhan ("short 140"), the rest looted before the restart and never held
    std::set<uint32> lackItems;
    for (auto const& [key, need] : lack)
        lackItems.insert(key.second);
    std::set<uint32> heldFor;
    for (auto const& [low, st] : bots_)
        for (HeldItem const& h : st.held)
            if (h.forLow || h.purpose == "pending")
                heldFor.insert(h.itemGuidLow);
    struct MemberStack
    {
        uint32 from{0};
        uint32 guid{0};
        uint32 count{0};
    };
    std::map<uint32, std::vector<MemberStack>> stacksOf;
    if (!lackItems.empty())
        for (auto const& [low, st] : bots_)
        {
            Player* h = Online(low);
            if (!h || !GET_PLAYERBOT_AI(h) || HumanCharacter(h))
                continue;
            uint32 const holder = low;
            std::vector<uint32> const& sold = st.sell;
            ForEachBagItem(h,
                           [&](Item* i)
                           {
                               uint32 const guid = i->GetGUID().GetCounter();
                               if (lackItems.count(i->GetEntry()) && !heldFor.count(guid) &&
                                   !h->HasQuestForItem(i->GetEntry()) &&
                                   std::find(sold.begin(), sold.end(), guid) == sold.end())
                                   stacksOf[i->GetEntry()].push_back({holder, guid, i->GetCount()});
                           });
        }
    for (auto const& [key, need] : lack)
    {
        auto const& [crafter, item] = key;
        // [craft382] the holdings nearest it first until they cover the lack, less what its mailbox holds already.
        // Before, one holding at a time and none while one lay in its mailbox: 2-3 herbs a trip
        auto const inMail = incoming.find(key);
        uint32 left = need.first;
        left -= std::min(left, inMail != incoming.end() ? inMail->second : 0u);
        if (!left)
            continue;
        Player* c = Online(crafter);
        struct Holding
        {
            float dist{NONE};
            uint32 from{0};
            MemberStack const* held{nullptr};
        };
        std::vector<Holding> holdings;
        if (auto const stacks = stacksOf.find(item); stacks != stacksOf.end())
            for (MemberStack const& held : stacks->second)
            {
                if (held.from == crafter || ownNeed.count({held.from, item}) || used.count(held.guid))
                    continue;
                Player* h = Online(held.from);
                holdings.push_back(
                    {h && c && h->GetMapId() == c->GetMapId() ? h->GetExactDist(c) : NONE / 2, held.from, &held});
            }
        std::stable_sort(holdings.begin(), holdings.end(),
                         [](Holding const& a, Holding const& b) { return a.dist < b.dist; });
        std::map<uint32, uint32> perHolder;  // a holder's holdings taken: their want keys
        for (Holding const& holding : holdings)
        {
            if (!left)
                break;
            MemberStack const& held = *holding.held;
            Want best;
            best.key = "r" + std::to_string(crafter) + ":" + std::to_string(item) + ":" +
                       std::to_string(holding.from) + ":" + std::to_string(perHolder[holding.from]++);
            best.from = holding.from;
            best.to = crafter;
            best.guid = held.guid;
            best.itemId = item;
            best.count = held.count;
            best.purpose = "reagent";
            best.why = "short " + std::to_string(left) + " — " + need.second;
            left -= std::min(left, held.count);
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
                    best.purpose = "supply";
                    best.why = "gatherer → crafter; " + best.why;
                }
            }
            used.insert(best.guid);
            // [lag330] each supply step of a crafter's chain, logged once per want and source
            static std::map<std::string, uint32> supplyLogged;  // want key -> its source member (a log cadence)
            if (supplyLogged[best.key] != best.from)
            {
                supplyLogged[best.key] = best.from;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(best.itemId);
                LOG_INFO("playerbots", "[lag330] supply: {} -> {} {} x{} ({}): {}", Who(best.from), Who(best.to),
                         proto ? proto->Name1 : std::to_string(best.itemId), best.count, best.purpose, best.why);
            }
            wants.push_back(best);
        }
    }

    // f. [econ1] bag relief: a member with full bags holding reagents for the guild (held for the bank, forLow 0)
    // sends them to a crafter whose own skill-ups to its held rank cap lack that item and whose bags have room
    // (b350: Thamesh at the vendor 100 % -> 100 %, its bags full of wanted reagents). Nobody to take it: the
    // global leader sells it at the next vendor (UpdateOrders, bags full); with a bank tab the deposit takes it
    constexpr uint32 RELIEF_ROOM = 2;                        // the receiver keeps a slot for its own loot
    std::map<std::pair<uint32, uint32>, uint32> reliefLack;  // (crafter, item) -> still lacking after the plan
    for (auto const& [low, items] : crafterLack_)
    {
        // [craft440] a crafter without Mining takes no ore (the craft437 rule): its lack of an ore is the bars smelted
        // of it, which a full holder's bars relieve
        Player* c = Online(low);
        bool const smelts = c && c->HasSkill(SKILL_MINING);
        for (auto const& [item, n] : items)
        {
            uint32 bar = 0;
            float per = 0.0f;
            if (!smelts)
                for (auto const& [smelted, parts] : smeltFrom_)
                    for (auto const& [ore, units] : parts)
                        if (ore == item && units > 0.0f && !bar)
                        {
                            bar = smelted;
                            per = units;
                        }
            if (bar)
                reliefLack[{low, bar}] += uint32(std::ceil(double(n) / per));
            else
                reliefLack[{low, item}] = n;
        }
    }
    for (auto const& [low, st] : bots_)
    {
        Player* h = Online(low);
        if (!h || !GET_PLAYERBOT_AI(h) || HumanCharacter(h) || FreeSlots(h))
            continue;
        for (HeldItem const& held : st.held)
        {
            if (held.forLow || held.purpose == "pending" || used.count(held.itemGuidLow))
                continue;
            Player* best = nullptr;
            float bestDist = NONE;
            uint32 bestLack = 0;
            for (auto& [key, left] : reliefLack)
            {
                if (key.second != held.itemId || key.first == low || !left)
                    continue;
                Player* c = Online(key.first);
                if (!c || !GET_PLAYERBOT_AI(c) || HumanCharacter(c) || FreeSlots(c) < RELIEF_ROOM)
                    continue;
                float const d = c->GetMapId() == h->GetMapId() ? h->GetExactDist(c) : NONE / 2;
                if (d < bestDist)
                {
                    bestDist = d;
                    best = c;
                    bestLack = left;
                }
            }
            if (!best)
                continue;
            uint32 const toLow = best->GetGUID().GetCounter();
            uint32& left = reliefLack[{toLow, held.itemId}];
            left -= std::min(left, held.count);
            Want w;
            w.key = "b" + std::to_string(held.itemGuidLow);
            w.from = low;
            w.to = toLow;
            w.guid = held.itemGuidLow;
            w.itemId = held.itemId;
            w.count = held.count;
            w.purpose = "reagent";
            w.why = "bags full at " + Who(low) + "; " + Who(toLow) + "'s skill-ups to its rank cap lack " +
                    std::to_string(bestLack);
            used.insert(w.guid);
            wants.push_back(w);
        }
    }

    // e. SUPPLY of food: a cook shares the food it holds with the members who have none left, each by its measured
    // use (food eaten over the last hour, from the bags between updates; no use measured yet: equal shares); the cook
    // keeps its own share. Members of a five get theirs from the five's quartermaster (c).
    // [craft385] user 2026-11-11 «cooks feed the guild»: water-filling by shortfall as the five's drinks ([dun209]),
    // over the members outside a five. A member's fill is the food it holds and can eat (CountKind: its level) over
    // its line (RESTOCK_FOOD_MIN, the restock line of its own vendor buy); a giver keeps its own line. The lowest fill
    // takes first, its whole shortfall, from the giver with the most above its line - the guild's supply cooks
    // (supplyCrafts_: the band food they cooked) before the other members who hold food. Before, a member got food
    // only at 0 left, and every holder of the Cooking skill shared its whole stock by the use weights
    // [craft399] the fills in full rests (BagFoodRests: each item's restored health over the member's max health),
    // the giver's best food first - an outgrown food is a filler only, never a full share
    struct FoodFill
    {
        Player* p{nullptr};
        float held{0.0f};
    };
    std::vector<FoodFill> fills;
    for (PlanRow const& m : plan_)
    {
        Player* p = Online(m.low);
        if (!p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p))
            continue;
        if (auto const st = bots_.find(m.low); st != bots_.end() && st->second.fiveId)
            continue;
        fills.push_back({p, BagFoodRests(p)});
    }
    std::stable_sort(fills.begin(), fills.end(), [](FoodFill const& a, FoodFill const& b) { return a.held < b.held; });
    struct FoodGiver
    {
        Player* p{nullptr};
        bool cook{false};
        float surplus{0.0f};                           // rests above its line
        std::vector<std::pair<Item*, uint32>> stacks;  // its food stacks and what is left of each to give
    };
    std::vector<FoodGiver> givers;
    for (FoodFill const& f : fills)
    {
        if (f.held <= float(RESTOCK_FOOD_MIN))
            continue;
        uint32 const low = f.p->GetGUID().GetCounter();
        FoodGiver g;
        g.p = f.p;
        g.cook = supplyCrafts_.count(low) != 0;
        g.surplus = f.held - float(RESTOCK_FOOD_MIN);
        auto const giverState = bots_.find(low);
        ForEachBagItem(f.p,
                       [&](Item* i)
                       {
                           uint32 const guid = i->GetGUID().GetCounter();
                           bool const forSale = giverState != bots_.end() &&
                                                std::find(giverState->second.sell.begin(),
                                                          giverState->second.sell.end(),
                                                          guid) != giverState->second.sell.end();
                           if (ConsumableKind(i->GetTemplate()) == FOOD &&
                               i->GetTemplate()->RequiredLevel <= f.p->GetLevel() && !used.count(guid) && !forSale)
                               g.stacks.emplace_back(i, i->GetCount());
                       });
        if (!g.stacks.empty())
            givers.push_back(std::move(g));
    }
    std::stable_sort(givers.begin(), givers.end(),
                     [](FoodGiver const& a, FoodGiver const& b)
                     { return a.cook != b.cook ? a.cook : a.surplus > b.surplus; });
    for (FoodFill const& f : fills)
    {
        if (f.held >= float(RESTOCK_FOOD_MIN) || givers.empty())
            break;  // sorted by fill: the rest stand at their line
        float const shortfall = float(RESTOCK_FOOD_MIN) - f.held;
        FoodGiver* from = nullptr;
        std::pair<Item*, uint32>* stack = nullptr;
        float stackShare = 0.0f;  // what one item of it gives the receiver, of a rest
        for (FoodGiver& g : givers)
        {
            if (g.p == f.p || g.surplus <= 0.0f)
                continue;
            for (auto& entry : g.stacks)
                if (float const share = entry.second ? FoodItemRests(f.p, entry.first->GetTemplate()) : 0.0f;
                    share > stackShare)
                {
                    stack = &entry;
                    stackShare = share;
                }
            if (stack)
            {
                from = &g;
                break;
            }
        }
        if (!from)
            continue;
        float const giverShare = std::max(FoodItemRests(from->p, stack->first->GetTemplate()), 0.0001f);
        uint32 const give = std::min({uint32(std::ceil(shortfall / stackShare)),
                                      uint32(from->surplus / giverShare), stack->second});
        if (!give)
            continue;
        from->surplus -= float(give) * giverShare;
        stack->second -= give;
        uint32 const to = f.p->GetGUID().GetCounter();
        Want w;
        w.key = "s" + std::to_string(to) + ":food";
        w.from = from->p->GetGUID().GetCounter();
        w.to = to;
        w.guid = stack->first->GetGUID().GetCounter();
        w.itemId = stack->first->GetEntry();
        w.count = give;
        w.purpose = "supply";
        w.why = Acore::StringFormat("food {:.1f} of its line {} rests; {} with {:.1f} rests above its own line "
                                    "fills it [craft385][craft399]",
                                    f.held, RESTOCK_FOOD_MIN, from->cook ? "the guild cook" : "a member",
                                    from->surplus + float(give) * giverShare);
        wants.push_back(w);
        // [craft385] each fill once per change of its giver or food ([craft389]: b447 logged 192 lines in 13 min - the
        // counts in the line changed with every meal and every other fill from the same giver)
        static std::map<uint32, std::pair<uint32, uint32>> fillLogged;  // receiver -> (giver, food) (world thread)
        if (std::pair<uint32, uint32>& logged = fillLogged[to]; logged != std::make_pair(w.from, w.itemId))
        {
            logged = {w.from, w.itemId};
            LOG_INFO("playerbots", "[craft385] food fill: {} -> {} {}: {}", Who(w.from), Who(to),
                     ItemText(w.itemId, give), w.why);
        }
    }

    // [dun208] user correction 2026-11-11 (b439 dm-n five 11: Bhogagnuth, Jurdogs and Febineysh at 0 drinks for the
    // last third of the run beside Relara's 24-64; before that drinks bounced between members - Relara ->
    // Bhogagnuth 14, Bhogagnuth -> Relara 9 and 2): inside the run the five's share is water-filling over the members
    // that use the kind (a drink: its mana users), the giver included. A member's fill is what it holds (the bag
    // count of its own food / drink use and of the run's supply check, CountBagFood: spell category 59 / 11, items
    // it can use) over its [dun194] line (food: RESTOCK_FOOD_MIN each). The pool covering every line: each member
    // below its line is filled to it from the members above theirs, who keep their line. Else every fill is evened
    // to the pool's common fill (the pool over the lines): nobody receives above it, nobody gives below it. Food or
    // drink moves only from a higher fill to a lower one, so none comes back. The fills before and after: [dun208]
    auto const waterFill = [&](uint32 id, GuildFive const& five, uint8 kind)
    {
        uint32 const category = kind == FOOD ? RESTOCK_SPELL_CATEGORY_FOOD : RESTOCK_SPELL_CATEGORY_DRINK;
        struct Fill
        {
            Player* p;
            uint32 held;
            uint32 line;
            uint32 after;
        };
        std::vector<Fill> fills;
        uint64 pool = 0, lines = 0;
        for (uint32 m : five.members)
        {
            Player* p = Online(m);
            if (!p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p) || !p->IsAlive() ||
                (kind == DRINK && !p->GetMaxPower(POWER_MANA)))
                continue;
            uint32 const held = CountBagFood(p, category);
            uint32 const line = std::max<uint32>(1, RunLine(five, m, kind));
            fills.push_back({p, held, line, held});
            pool += held;
            lines += line;
        }
        if (fills.size() < 2 || !lines)
            return;
        bool const covered = pool >= lines;
        // [dun209b] the short pool's shares in whole drinks: pool x line / lines, the remainders handed out largest
        // first (they sum to the pool); then, the pool holding one a member, a member whose share is 0 takes 1 from
        // the largest share (b439 five 11: Febineysh 0/2 rounded to 0 beside 44 drinks). A member's share is both
        // the most it receives to and the least it gives down to
        std::vector<uint32> target(fills.size(), 0);
        // [dun230] a short pool of drink fills the five's healer to its line first: its mana gates every pull (the
        // five gate and the planner judge the tank's healing on it), a dps short of mana still deals its white
        // damage. b444 wc-n five 12: "pool 1 of the lines' 78, short: Farret 0/1, Feette 1/20, ..." - the healer
        // stood at 38-41 % with no drink while the planner held Verdan's pull for his mana
        size_t healerFill = fills.size();
        if (kind == DRINK)
            for (size_t n = 0; n < fills.size(); ++n)
                if (auto const role = five.roles.find(fills[n].p->GetGUID().GetCounter());
                    role != five.roles.end() && role->second == "healer")
                    healerFill = n;
        if (!covered)
        {
            uint64 given = 0, sharedPool = pool, sharedLines = lines;
            if (healerFill < fills.size())
            {
                target[healerFill] = uint32(std::min<uint64>(pool, fills[healerFill].line));
                given = target[healerFill];
                sharedPool -= given;
                sharedLines -= fills[healerFill].line;
            }
            std::vector<std::pair<uint64, size_t>> rest;  // (remainder, member)
            for (size_t n = 0; n < fills.size(); ++n)
            {
                if (n == healerFill || !sharedLines)
                    continue;
                uint64 const exact = sharedPool * fills[n].line;
                target[n] = uint32(exact / sharedLines);
                given += target[n];
                rest.emplace_back(exact % sharedLines, n);
            }
            std::stable_sort(rest.begin(), rest.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
            for (size_t k = 0; given < pool && k < rest.size(); ++k, ++given)
                ++target[rest[k].second];
            if (pool >= fills.size())
                for (size_t n = 0; n < fills.size(); ++n)
                    if (!target[n] && n != healerFill)
                    {
                        size_t most = n;  // [dun230] never from the healer's share
                        for (size_t m = 0; m < target.size(); ++m)
                            if (m != healerFill && target[m] > target[most])
                                most = m;
                        if (target[most] < 2)
                            break;
                        --target[most];
                        target[n] = 1;
                    }
        }
        // the most a member may hold after (a receiver's cap) and the least a giver keeps
        auto const capOf = [&](Fill const& f) { return covered ? f.line : target[size_t(&f - fills.data())]; };
        auto const keepOf = capOf;
        auto const fillText = [](Fill const& f, uint32 n)
        { return f.p->GetName() + " " + std::to_string(n) + "/" + std::to_string(f.line); };
        // the giver's biggest stack the receiver can use (what is left of each this update)
        std::map<Item*, uint32> left;
        auto const stackFor = [&](Player* giver, Player* receiver) -> Item*
        {
            Item* best = nullptr;
            ForEachBagItem(giver, [&](Item* i)
                           {
                               ItemTemplate const* proto = i->GetTemplate();
                               if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE ||
                                   (proto->SubClass != ITEM_SUBCLASS_CONSUMABLE &&
                                    proto->SubClass != ITEM_SUBCLASS_FOOD) ||
                                   proto->Spells[0].SpellCategory != category ||
                                   used.count(i->GetGUID().GetCounter()) || receiver->CanUseItem(proto) != EQUIP_ERR_OK)
                                   return;
                               auto const known = left.emplace(i, i->GetCount()).first;
                               if (known->second && (!best || known->second > left[best]))
                                   best = i;
                           });
            return best;
        };
        // the lowest fill first: each receiver takes from the giver with the most above its keep (one want a member)
        std::vector<size_t> order(fills.size());
        for (size_t n = 0; n < order.size(); ++n)
            order[n] = n;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
                  { return uint64(fills[a].held) * fills[b].line < uint64(fills[b].held) * fills[a].line; });
        if (!covered && healerFill < fills.size())  // [dun230] the healer of a short pool takes first
            std::stable_partition(order.begin(), order.end(), [&](size_t n) { return n == healerFill; });
        std::string planned;
        // [dun210] b441: Lanbuzz -> Almored 22 trades of one Ice Cold Milk each, Almored at 7/8 drinking them as they
        // came; Ainne <-> Geefeelli 4 / 2, Aevmae <-> Zharet 2 / 2, Aevmae <-> Galiver 2 / 1 back and forth. A member
        // eating / drinking now (the food / drink aura on it: SPELL_AURA_MOD_REGEN / SPELL_AURA_MOD_POWER_REGEN) takes
        // nothing until its rest ends, then its whole deficit at once. And a pair that traded one way trades back only
        // once the two hold less together than right after it (they ate / drank since): the order of the fills of
        // one update holds across the updates until consumption changes it
        static std::map<std::tuple<std::string, uint8, uint32, uint32>, std::pair<uint32, uint32>> lastMove;
        if (lastMove.size() > 4096)
            lastMove.clear();
        auto const pairKey = [&](Player* a, Player* b)
        {
            uint32 const x = a->GetGUID().GetCounter(), y = b->GetGUID().GetCounter();
            return std::make_tuple(five.runId, kind, std::min(x, y), std::max(x, y));
        };
        AuraType const resting = kind == FOOD ? SPELL_AURA_MOD_REGEN : SPELL_AURA_MOD_POWER_REGEN;
        // [dun211] b442: Relara -> Mytaeleas 9 trades, Almored -> Ainne 9, Geefeelli -> Rumanatol 9, Idunk ->
        // Jurdogs 7, mostly one drink each: a member a drink or two under the common fill after each rest took
        // them at once. A member takes its share only when what it holds no longer covers one full rest - its bar
        // from empty over what its best item restores (its max mana / health over BestBagRestore, the drink action's
        // own amount) - then its whole deficit in one trade; until then it rests from its own
        auto const oneRest = [&](Player* p) -> uint32
        {
            float const per = BestBagRestore(p, category);
            uint32 const bar = kind == FOOD ? p->GetMaxHealth() : p->GetMaxPower(POWER_MANA);
            return per > 0.0f ? uint32(std::ceil(float(bar) / per)) : 1;
        };
        std::string waiting;  // [dun210] / [dun211] the members left out this update, for the [dun208] line
        for (size_t r : order)
        {
            Fill& to = fills[r];
            uint32 const cap = capOf(to);
            if (to.after >= cap)
                continue;
            if (to.p->HasAuraType(resting))
            {
                waiting += (waiting.empty() ? "" : ", ") + to.p->GetName() + " eats / drinks [dun210]";
                continue;
            }
            if (uint32 const rest = oneRest(to.p); to.held >= rest)
            {
                waiting += (waiting.empty() ? "" : ", ") + to.p->GetName() + " holds a rest (" +
                           std::to_string(to.held) + " of " + std::to_string(rest) + ") [dun211]";
                continue;
            }
            size_t giver = fills.size();
            uint32 spare = 0;
            for (size_t g = 0; g < fills.size(); ++g)
            {
                uint32 const keep = keepOf(fills[g]);
                uint32 const above = fills[g].after > keep ? fills[g].after - keep : 0;
                // only a member at a higher fill gives (its fill before this update); [dun230] any member above its
                // share gives to the healer of a short pool (the healer gives none back: its share is all it holds)
                if (g != r && above > spare &&
                    ((!covered && r == healerFill) ||
                     uint64(fills[g].held) * to.line > uint64(to.held) * fills[g].line))
                {
                    // [dun210] no trade back while the pair holds what it held after its last trade
                    if (auto const last = lastMove.find(pairKey(fills[g].p, to.p));
                        last != lastMove.end() && last->second.first == to.p->GetGUID().GetCounter() &&
                        fills[g].held + to.held >= last->second.second)
                    {
                        waiting += (waiting.empty() ? "" : ", ") + fills[g].p->GetName() + " -> " + to.p->GetName() +
                                   " refused: the trade back [dun210]";
                        continue;
                    }
                    giver = g;
                    spare = above;
                }
            }
            if (giver == fills.size())
                continue;
            Item* from = stackFor(fills[giver].p, to.p);
            if (!from)
                continue;
            uint32 const give = std::min({cap - to.after, spare, left[from]});
            if (!give)
                continue;
            left[from] -= give;
            fills[giver].after -= give;
            to.after += give;
            lastMove[pairKey(fills[giver].p, to.p)] = {fills[giver].p->GetGUID().GetCounter(),
                                                       fills[giver].held + to.held};
            Want w;
            w.key = "c" + std::to_string(to.p->GetGUID().GetCounter()) + ":" + std::to_string(kind);
            w.tradeOnly = true;
            w.from = fills[giver].p->GetGUID().GetCounter();
            w.to = to.p->GetGUID().GetCounter();
            w.guid = from->GetGUID().GetCounter();
            w.itemId = from->GetEntry();
            w.count = give;
            w.purpose = "consumable";
            w.why = std::string(kind == FOOD ? "food" : "drink") + " below the five's fill; five " +
                    std::to_string(id) + "'s member " + Who(w.from) + " evens the fills: " +
                    fillText(fills[giver], fills[giver].held) + " -> " + fillText(to, to.held) + " [dun208]";
            wants.push_back(w);
            planned += (planned.empty() ? "" : ", ") + fills[giver].p->GetName() + " -> " + to.p->GetName() + " " +
                       std::to_string(give);
        }
        // the fills before and after, logged when the share's verdict changes (its text without the counts)
        std::string before, after;
        for (Fill const& f : fills)
        {
            before += (before.empty() ? "" : ", ") + fillText(f, f.held);
            after += (after.empty() ? "" : ", ") + fillText(f, f.after);
        }
        std::string const text = "five " + std::to_string(id) + " " + (kind == FOOD ? "food" : "drink") +
                                 " (pool " + std::to_string(pool) + " of the lines' " + std::to_string(lines) +
                                 (covered ? ", covers them" : ", short") +
                                 (!covered && healerFill < fills.size()
                                      ? ", [dun230] the healer " + fills[healerFill].p->GetName() + " first"
                                      : std::string()) +
                                 "): " + before + " -> " + after + "; " +
                                 (planned.empty() ? std::string("nothing moves") : planned) +
                                 (waiting.empty() ? std::string() : "; waits: " + waiting);
        std::string verdict = planned.empty() ? std::string("even") : text;
        verdict.erase(std::remove_if(verdict.begin(), verdict.end(), [](char ch) { return ch >= '0' && ch <= '9'; }),
                      verdict.end());
        static std::map<uint64, std::string> judged;  // five and kind -> its last verdict (lock_ held)
        if (std::string& last = judged[uint64(id) << 8 | kind]; last != verdict)
        {
            last = verdict;
            LOG_INFO("playerbots", "[dun208] {}", text);
        }
    };

    // c. consumables in a five: the quartermaster shares its food / drink equally with the members who have none
    // left (trade when they meet: they walk together)
    for (auto const& [id, five] : fives_)
    {
        if (five.stage == GuildFive::Stage::Done)
            continue;
        for (uint8 kind : {FOOD, DRINK})
        {
            if (five.stage == GuildFive::Stage::Run)
            {
                waterFill(id, five, kind);  // [dun208]
                continue;
            }
            // [share83] the giver is the member of the five holding the most of it (the quartermaster first on a tie),
            // not the quartermaster alone: b406 the warrior quartermasters held no drink, and the hunters ran dry
            // beside mages and warlocks with spare stacks (rfc-n five 4: Feette 0 drinks, 18 Taragaman holds)
            std::vector<Player*> needy;
            uint8 level = 255;
            for (uint32 m : five.members)
            {
                Player* p = Online(m);
                // [dun199] while the five gathers outside a member below its priced line is topped up (the mage's
                // conjured water goes to the members short of their priced line before the run); inside the run
                // the fills are evened (waterFill, [dun208])
                bool const lined = p && five.stage == GuildFive::Stage::Gather && kind == DRINK;
                uint32 const line = lined ? RunLine(five, m, kind) : 1;
                if (!p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p) ||
                    (kind == DRINK && p->getPowerType() != POWER_MANA) || CountKind(p, kind, p->GetLevel()) >= line)
                    continue;
                needy.push_back(p);
                level = std::min<uint8>(level, p->GetLevel());
            }
            if (needy.empty())
                continue;
            auto const pickGiver = [&](uint32& most) -> Player*
            {
                Player* giver = nullptr;
                most = 0;
                for (uint32 m : five.members)
                {
                    Player* p = Online(m);
                    if (!p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p) ||
                        std::find(needy.begin(), needy.end(), p) != needy.end())
                        continue;
                    uint32 const has = CountKind(p, kind, level);
                    if (has > most || (has == most && has && m == five.quartermasterLow))
                    {
                        giver = p;
                        most = has;
                    }
                }
                return giver;
            };
            uint32 qHas = 0;
            Player* const q = pickGiver(qHas);
            if (!q)
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
            // the giver keeps an equal share; [share92] inside the dungeon it keeps its own restock line and shares
            // only what lies above it (no member is pushed below the line that made another needy: no ping-pong)
            bool const inRun = five.stage == GuildFive::Stage::Gather && kind == DRINK;
            uint32 const keep = RunLine(five, q->GetGUID().GetCounter(), kind);  // [dun194] its own line
            uint32 const share = inRun ? (total > keep ? (total - keep) / uint32(needy.size()) : 0)
                                       : total / uint32(needy.size() + 1);
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
                w.from = q->GetGUID().GetCounter();
                w.to = p->GetGUID().GetCounter();
                w.guid = from->GetGUID().GetCounter();
                w.itemId = from->GetEntry();
                w.count = give;
                w.purpose = "consumable";
                w.why = std::string(kind == FOOD ? "food" : "drink") + " ran out; five " + std::to_string(id) + "'s " +
                        (q->GetGUID().GetCounter() == five.quartermasterLow ? "quartermaster" : "member") + " " +
                        Who(q->GetGUID().GetCounter()) + " shares equally: " + std::to_string(total) + " among " +
                        std::to_string(needy.size() + 1) + " [share83]";
                wants.push_back(w);
            }
        }
    }

    // d. [supply112] coordinator 2026-11-10: the band's guild cook sends its cooked supply to the members of a five
    // of its band before their run: from the five's forming on (Stage::Gather - [supply113] a finder five stays in it
    // while it quests until the port, it never stands at the entrance; mail reaches it anywhere), each member up to a
    // full restock (RESTOCK_FOOD_TARGET) of the kind, a drink only to a mana user; the cook keeps nothing back
    for (auto const& [id, five] : fives_)
    {
        if (five.stage != GuildFive::Stage::Gather)
            continue;
        for (auto const& [cookLow, jobs] : supplyCrafts_)
        {
            Player* c = Online(cookLow);
            if (!c || !GET_PLAYERBOT_AI(c) || HumanCharacter(c))
                continue;
            for (SupplyCraft const& job : jobs)
            {
                std::vector<Item*> stacks;
                ForEachBagItem(c, [&](Item* i)
                               {
                                   if (i->GetEntry() == job.product && !used.count(i->GetGUID().GetCounter()))
                                       stacks.push_back(i);
                               });
                if (stacks.empty())
                    continue;
                std::map<Item*, uint32> left;
                for (Item* i : stacks)
                    left[i] = i->GetCount();
                for (uint32 m : five.members)
                {
                    Player* p = Online(m);
                    if (!p || m == cookLow || !GET_PLAYERBOT_AI(p) || HumanCharacter(p) || p->GetLevel() < job.lo ||
                        p->GetLevel() > job.hi ||
                        (job.category == RESTOCK_SPELL_CATEGORY_DRINK && p->getPowerType() != POWER_MANA))
                        continue;
                    uint32 have = CountBagFood(p, job.category);
                    for (auto const& [tid, t] : transfers_)  // its letters on the way count as its own
                        if (t.toLow == m && t.itemId == job.product && t.mailed)
                            have += t.count;
                    if (have >= RESTOCK_FOOD_TARGET)
                        continue;
                    Item* from = nullptr;
                    for (Item* i : stacks)
                        if (!from || left[i] > left[from])
                            from = i;
                    if (!from || !left[from])
                        break;
                    uint32 const give = std::min(RESTOCK_FOOD_TARGET - have, left[from]);
                    left[from] -= give;
                    Want w;
                    w.key = "s" + std::to_string(m) + ":" + std::to_string(job.product);
                    w.from = cookLow;
                    w.to = m;
                    w.guid = from->GetGUID().GetCounter();
                    w.itemId = job.product;
                    w.count = give;
                    w.purpose = "consumable";
                    w.why = Acore::StringFormat("[supply112] the guild cook of band {} sends five {}'s {} {} before "
                                                "its run ({} of {})",
                                                job.band, id, Who(m), ItemText(job.product, give), have,
                                                RESTOCK_FOOD_TARGET);
                    wants.push_back(w);
                }
            }
        }
    }

    // d2. [eco194] the alchemists' potions go to the members whose drunk potions are not replaced yet: each member up
    // to its owed potions of the kind, a potion it can use (its level), its letters on the way counting as its own
    for (auto const& [makerLow, jobs] : potionCrafts_)
    {
        Player* c = Online(makerLow);
        if (!c || !GET_PLAYERBOT_AI(c) || HumanCharacter(c))
            continue;
        for (SupplyCraft const& job : jobs)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(job.product);
            if (!proto || !job.potionKind)
                continue;
            uint8 const kind = job.potionKind - 1;
            std::vector<Item*> stacks;
            ForEachBagItem(c, [&](Item* i)
                           {
                               if (i->GetEntry() == job.product && !used.count(i->GetGUID().GetCounter()))
                                   stacks.push_back(i);
                           });
            std::map<Item*, uint32> left;
            for (Item* i : stacks)
                left[i] = i->GetCount();
            for (auto const& [m, book] : potions_)
            {
                uint32 owed = book.Owed(kind);
                Player* p = Online(m);
                if (!owed || m == makerLow || !p || !GET_PLAYERBOT_AI(p) || HumanCharacter(p) ||
                    p->GetLevel() < proto->RequiredLevel)
                    continue;
                for (auto const& [tid, t] : transfers_)
                    if (t.toLow == m && t.mailed && t.itemId &&
                        PotionKindOf(sObjectMgr->GetItemTemplate(t.itemId)) == kind)
                        owed -= std::min(owed, t.count);
                if (!owed)
                    continue;
                Item* from = nullptr;
                for (Item* i : stacks)
                    if (!from || left[i] > left[from])
                        from = i;
                if (!from || !left[from])
                    break;
                uint32 const give = std::min(owed, left[from]);
                left[from] -= give;
                Want w;
                w.key = "p" + std::to_string(m) + ":" + std::to_string(job.product);
                w.from = makerLow;
                w.to = m;
                w.guid = from->GetGUID().GetCounter();
                w.itemId = job.product;
                w.count = give;
                w.purpose = "consumable";
                w.why = Acore::StringFormat("[eco194] the alchemist replaces the {} potions {} drank ({} of {} not "
                                            "replaced yet)",
                                            kind == POTION_HEAL ? "healing" : "mana", Who(m), give, book.Owed(kind));
                wants.push_back(w);
            }
        }
    }

    // [econ2] what one unit is worth to the guild (the wanted list's value), against the postage of a letter
    std::unordered_map<uint32, float> unitWorth;
    for (WantedItem const& w : wanted_)
        if (!w.optional)
            unitWorth[w.itemId] = w.unitValue;
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
            // [econ2] b353: 101 letters lay in the members' mailboxes - Lanbuzz 20, Emdyl 12, Ravelandy 10, Orton 6:
            // party followers (GROUP_WAIT the whole run, the party's leader walks, their own town stops never
            // come) and full bags ("mail waits", 7). A letter goes only to a bot that can take it: no follower of
            // a party (its letters wait for its party's town stop), bag room for what already waits in its
            // mailbox plus this one
            if (r && !HumanCharacter(r) && GET_PLAYERBOT_AI(r))
            {
                uint32 waiting = 0;
                for (Mail const* m : r->GetMails())
                    if (m && m->state != MAIL_STATE_DELETED)
                        waiting += uint32(m->items.size());
                if (Group const* group = r->GetGroup(); group && group->GetLeaderGUID() != r->GetGUID())
                    c.mailNo = "the receiver follows its party";
                else if (FreeSlots(r) <= waiting)
                    c.mailNo = "the receiver's bags hold no room for its " + std::to_string(waiting) + " waiting";
            }
            // [econ2] a reagent / supply letter costs the postage: b353 single Copper Ore (5 c) and Linen Cloth
            // (13 c) letters at 30 c each. It goes when the units are worth the postage to the guild (the wanted
            // list's value per unit, else the vendor price); an upgrade, a product, a recipe, gold always go
            if (c.mailNo.empty() && !w.gold && (w.purpose == "reagent" || w.purpose == "supply"))
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(w.itemId))
                {
                    auto const worth = unitWorth.find(w.itemId);
                    float const unit = std::max({worth != unitWorth.end() ? worth->second : 0.0f,
                                                 vendorItems_.count(w.itemId)
                                                     ? float(proto->BuyPrice) / std::max<uint32>(1, proto->BuyCount)
                                                     : 0.0f,
                                                 float(proto->SellPrice)});
                    if (unit * float(std::max<uint32>(1, w.count)) < float(postage))
                        c.mailNo = "worth " + Gold(uint32(unit * float(std::max<uint32>(1, w.count)))) +
                                   " < postage " + Gold(postage);
                }
            if (s->GetLevel() >= mailLevel && canWalk(s) && FoundingFreeMoney(s) >= postage + w.money &&
                !(old && old->mailRefused) && receiverBox && c.mailNo.empty())
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
                bool const needed = w.purpose == "upgrade" || w.purpose == "product" || w.purpose == "reagent" ||
                                    (w.purpose == "supply" && w.key[0] == 'r');
                if (needed && c.mail < NONE)
                    c.mail += float(c.delay);
            }
            // bank: both walks to the vaults (only with the guild's tab, the rights and room for it [bank411])
            if (!w.gold && r && GuildBankRun::DepositState(s, w.itemId, w.count).empty() &&
                GuildBankRun::BankState(r, false).empty() && canWalk(s) && canWalk(r))
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
            c.text = "trade at a meeting in the five";
            return c;
        }
        c.text = "trade " + Sec(c.trade) + (c.walkerLow && c.trade < NONE ? " (walks " + Who(c.walkerLow) + ")" : "") +
                 ", mail " + Sec(c.mail) +
                 (c.mail < NONE && c.delay ? " (delivery " + std::to_string(c.delay / 60) + " min)" : "") +
                 (c.mail < NONE ? ", " + Gold(MAIL_POSTAGE) : "") +
                 (c.mailNo.empty() ? "" : " (no letter: " + c.mailNo + " [econ2])") + ", bank " + Sec(c.bank);
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
            donation = "donations for the tab: the guild master is not a bot online";
        else
        {
            uint32 const gmFree = FoundingFreeMoney(gm);
            uint32 inMail = 0;
            for (Mail const* m : gm->GetMails())
                if (m && m->state != MAIL_STATE_DELETED && IsPlanned(m->sender))
                    inMail += m->money;
            uint32 const need = tabCost > gmFree + inMail ? tabCost - gmFree - inMail : 0;
            if (!need)
                donation = "tab " + Gold(tabCost) + ": the guild master has free " + Gold(gmFree) +
                           (inMail ? " + in mail " + Gold(inMail) : "") + " — enough, no donations needed";
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
                    w.purpose = "donation";
                    Choice const c = choose(w, nullptr);
                    if (c.cost == NONE)
                        continue;
                    if (c.channel == Transfer::TRADE)
                        w.money = free;
                    donors.push_back({w, c.cost});
                    pool += w.money;
                }
                if (pool < need)
                    donation = "tab " + Gold(tabCost) + ": the guild master has " + Gold(gmFree) +
                               (inMail ? " + in mail " + Gold(inMail) : "") + ", members have free " +
                               Gold(uint32(std::min<uint64>(pool, UINT32_MAX))) +
                               " — not enough, no donations (they save themselves)";
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
                        d.w.why = "bank tab " + Gold(tabCost) + ": the guild master has " + Gold(gmFree) + ", short " +
                                  Gold(need);
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
                    donation = "tab " + Gold(tabCost) + ": the guild master has " + Gold(gmFree) +
                               (inMail ? " + in mail " + Gold(inMail) : "") + ", donations " +
                               std::to_string(picked.size()) + " members (" + Sec(walk) +
                               " of walking); the bank saves " + Sec(saving) + " on transfers and frees " +
                               std::to_string(stuck) + " bag slots — " +
                               (pays ? "pays off: collecting" : "does not pay off: the guild master saves himself");
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
    // [gold41] user 2026-11-09: ... and for its essential supplies (food, drink, ammo of its level) it cannot pay. The
    // leader's choice: the richest member whose spare money (above its own needs: the module's budget keeps its
    // repairs, consumables, spells and travel back, less its next profession ranks) covers the need and the postage;
    // of equal spare the cheapest channel. Mailed or traded honestly (the core's mail / trade), one line per decision
    static std::map<std::string, std::string> goldLogged;  // want key -> decision last logged (a log cadence)
    std::unordered_map<uint32, uint32> inMailbox;  // [eco195] member -> the members' gold in its mailbox, not used yet
    for (auto const& [low, st] : bots_)
        for (int kind = 0; kind < 4; ++kind)
        {
            // [roles62] kind 2: the unlearn its planned role spec needs (Player::resetTalentsCost) it cannot pay -
            // b401 Shalaeth L11 "unlearning its talents costs 10000c, it has 7922c"
            // [lag330] kind 3: a crafter's skill-up costs for the members' orders it holds (UpdateProfessions)
            uint32 shortBy = kind == 0   ? st.repairShort
                             : kind == 1 ? st.supplyGoldShort
                             : kind == 3 ? st.profGoldShort
                                         : 0;
            if (kind == 2)
                if (Player* r = Online(low); r && r->IsAlive())
                    if (uint32 const roleSpec = RoleSpec(low, 0);
                        roleSpec && NewRpgProfession::NeedsRespec(r, roleSpec) && r->GetMoney() < r->resetTalentsCost())
                        shortBy = r->resetTalentsCost() - r->GetMoney();
            // [eco195] coordinator 2026-11-11: the gold the members already mailed it (in its mailbox, not taken yet)
            // is its money too - it rises only when it takes the letter, so each look planned the whole shortfall
            // again from the next richest member. b430: 135 supply letters (13.4 g) for 24 shortfalls, Manue 21 x
            // "short of its drink 15 s 50 c", Farret's 3 g 55 s 80 c unlearn sent in full by Bierya and Ellosera;
            // 21 g mailed in 1.1 h, 5.9 g taken, the [health] gold -8.5 g/h; DB after it: 30.5 g in 57 letters, 27.3 g
            // of it Farret's 19. The mailbox (the core's mail, across restarts) covers its needs in turn: repair,
            // supplies, role spec
            char const* const purpose = kind == 0   ? "repair"
                                        : kind == 1 ? "supplies"
                                        : kind == 2 ? "role spec"
                                                    : "profession orders";
            if (shortBy)
            {
                auto mail = inMailbox.find(low);
                if (mail == inMailbox.end())
                {
                    uint32 gold = 0;
                    if (Player* r = Online(low))
                        for (Mail const* m : r->GetMails())
                            if (m && m->state != MAIL_STATE_DELETED && m->money && IsPlanned(m->sender))
                                gold += m->money;
                    mail = inMailbox.emplace(low, gold).first;
                }
                uint32 const covered = std::min(shortBy, mail->second);
                shortBy -= covered;
                mail->second -= covered;
            }
            if (!shortBy || !IsPlanned(low))
                continue;
            Player* r = Online(low);
            if (!r || !GET_PLAYERBOT_AI(r) || HumanCharacter(r))
                continue;
            Want best;
            float bestCost = NONE;
            uint32 bestSpare = 0;
            for (PlanRow const& row : plan_)
            {
                Player* s = row.low == low ? nullptr : Online(row.low);
                if (!s || !GET_PLAYERBOT_AI(s) || HumanCharacter(s) || !canWalk(s))
                    continue;
                uint32 const spare = FoundingFreeMoney(s);
                if (spare < shortBy + MAIL_POSTAGE || spare < bestSpare)
                    continue;
                Want w;
                w.key = (kind == 0 ? "p" : kind == 1 ? "q" : kind == 2 ? "t" : "f") + std::to_string(low);
                w.gold = true;
                w.from = row.low;
                w.to = low;
                w.money = shortBy;
                w.purpose = purpose;
                w.why = kind == 0   ? "short of the repair " + Gold(shortBy)
                        : kind == 1 ? "short of its " + st.supplyWhat + " " + Gold(shortBy)
                        : kind == 2 ? "short of its role spec's unlearn " + Gold(shortBy)
                                    : "short of its " + st.profGoldWhat + " " + Gold(shortBy) + " [lag330]";
                Choice const c = choose(w, nullptr);
                if (c.cost >= NONE)
                    continue;
                if (spare > bestSpare || c.cost < bestCost)
                {
                    bestSpare = spare;
                    bestCost = c.cost;
                    best = w;
                }
            }
            if (bestCost < NONE)
            {
                std::string const decision = Acore::StringFormat(
                    "{} -> {} {} for {} (its spare {} the richest that covers it)", Who(best.from), Who(best.to),
                    Gold(best.money),
                    kind == 0   ? std::string("its repair")
                    : kind == 1 ? "its " + st.supplyWhat
                    : kind == 2 ? std::string("its role spec's unlearn at its trainer [roles62]")
                                : "its " + st.profGoldWhat,
                    Gold(bestSpare));
                if (goldLogged[best.key] != decision)
                {
                    goldLogged[best.key] = decision;
                    LOG_INFO("playerbots", "[{}] the global leader: {}", kind == 3 ? "lag330" : "gold41", decision);
                }
                wants.push_back(best);
            }
            // [respec116] b421: Farret (L18, planned healer) "unlearning its talents costs 50000c, it has 5652c" -
            // its earlier resets raised the core's price (Player::resetTalentsCost) to 5 g and no member's spare
            // covered the 4 g 43 s alone, so no gold went and it stayed a tank all run (314 roster lines "respec at
            // its trainer"). The user's priority is the healer roster: a role spec's unlearn no single member covers
            // is shared by the richest members, each its spare less the postage, when together they cover it
            else if (kind == 2 || kind == 3)  // [lag330] a crafter's orders are shared the same way
            {
                std::vector<std::pair<uint32, uint32>> donors;  // (spare, member)
                for (PlanRow const& row : plan_)
                {
                    Player* s = row.low == low ? nullptr : Online(row.low);
                    if (!s || !GET_PLAYERBOT_AI(s) || HumanCharacter(s) || !canWalk(s))
                        continue;
                    if (uint32 const spare = FoundingFreeMoney(s); spare > MAIL_POSTAGE)
                        donors.emplace_back(spare, row.low);
                }
                std::sort(donors.rbegin(), donors.rend());
                std::vector<Want> shares;
                uint32 left = shortBy;  // the shares already sent (in its mailbox) are out of it [eco195]
                for (auto const& [spare, from] : donors)
                {
                    if (!left)
                        break;
                    Want w;
                    w.key = (kind == 2 ? "t" : "f") + std::to_string(low) + ":" + std::to_string(from);
                    w.gold = true;
                    w.from = from;
                    w.to = low;
                    w.money = std::min(left, spare - MAIL_POSTAGE);
                    w.purpose = purpose;
                    w.why = kind == 2 ? "a share of its role spec's unlearn " + Gold(shortBy) + " [respec116]"
                                      : "a share of its " + st.profGoldWhat + " " + Gold(shortBy) + " [lag330]";
                    if (choose(w, nullptr).cost >= NONE)
                        continue;
                    left -= w.money;
                    shares.push_back(std::move(w));
                }
                std::string decision;
                std::string const what = kind == 2 ? std::string("its role spec's unlearn") : "its " + st.profGoldWhat;
                if (left)
                    decision = Acore::StringFormat("{} short of {} {}: the members' spare money together covers {} - "
                                                   "no gold goes",
                                                   Who(low), what, Gold(shortBy), Gold(shortBy - left));
                else
                {
                    decision = Acore::StringFormat("{} gets {} {} from {} members:", Who(low), what, Gold(shortBy),
                                                   shares.size());
                    for (Want const& w : shares)
                        decision += " " + Who(w.from) + " " + Gold(w.money) + ",";
                    decision.pop_back();
                    for (Want& w : shares)
                        wants.push_back(std::move(w));
                }
                std::string const logKey = (kind == 2 ? "t" : "f") + std::to_string(low);
                if (goldLogged[logKey] != decision)
                {
                    goldLogged[logKey] = decision;
                    LOG_INFO("playerbots", "[{}] the global leader: {}", kind == 2 ? "respec116" : "lag330", decision);
                }
            }
        }

    // [mount4] the riding plan's gold (GuildRiding::Shortfall: its Apprentice Riding and mount over its spare money),
    // the members whose riding saves the most copper an hour first, after every gold want above: from the member
    // whose spare, less what this plan already sends, covers it, else shared by the members that together cover it;
    // only members that ride already or whose own riding is worth less give (GuildRiding::MayFund)
    {
        std::unordered_map<uint32, uint32> promised;  // member -> the gold the wants above already send
        for (Want const& w : wants)
            if (w.gold)
                promised[w.from] += w.money;
        auto const spareOf = [&](uint32 from, Player* s)
        {
            uint32 const spare = FoundingFreeMoney(s);
            uint32 const sent = promised[from];
            return spare > sent ? spare - sent : 0;
        };
        for (uint32 const low : GuildRiding::FundingOrder())
        {
            uint32 shortBy = GuildRiding::Shortfall(low);
            Player* r = Online(low);
            if (!shortBy || !r || !GET_PLAYERBOT_AI(r) || HumanCharacter(r) || !IsPlanned(low))
                continue;
            for (Mail const* m : r->GetMails())  // [eco195] the members' gold already in its mailbox
                if (m && m->state != MAIL_STATE_DELETED && m->money && IsPlanned(m->sender))
                    shortBy -= std::min(shortBy, m->money);
            if (!shortBy)
                continue;
            std::vector<std::pair<uint32, uint32>> donors;  // (spare, member)
            for (PlanRow const& row : plan_)
            {
                Player* s = row.low == low ? nullptr : Online(row.low);
                if (!s || !GET_PLAYERBOT_AI(s) || HumanCharacter(s) || !canWalk(s) ||
                    !GuildRiding::MayFund(row.low, low))
                    continue;
                if (uint32 const spare = spareOf(row.low, s); spare > MAIL_POSTAGE)
                    donors.emplace_back(spare, row.low);
            }
            std::sort(donors.rbegin(), donors.rend());
            std::vector<Want> shares;
            uint32 left = shortBy;
            for (auto const& [spare, from] : donors)
            {
                if (!left)
                    break;
                Want w;
                w.key = "m" + std::to_string(low) + ":" + std::to_string(from);
                w.gold = true;
                w.from = from;
                w.to = low;
                w.money = std::min(left, spare - MAIL_POSTAGE);
                w.purpose = "riding";
                w.why = "a share of its riding and mount " + Gold(shortBy) + " [mount4]";
                if (choose(w, nullptr).cost >= NONE)
                    continue;
                left -= w.money;
                shares.push_back(std::move(w));
            }
            std::string decision;
            if (left)
                decision = Acore::StringFormat("{} short of its riding and mount {}: the spare money of the members "
                                               "that may give covers {} - no gold goes",
                                               Who(low), Gold(shortBy), Gold(shortBy - left));
            else
            {
                decision = Acore::StringFormat("{} gets its riding and mount {} ({:.1f} min/h saved) from {} members:",
                                               Who(low), Gold(shortBy), GuildRiding::SavedMinutesPerHour(low),
                                               shares.size());
                for (Want& w : shares)
                {
                    decision += " " + Who(w.from) + " " + Gold(w.money) + ",";
                    promised[w.from] += w.money;
                    wants.push_back(std::move(w));
                }
                decision.pop_back();
            }
            std::string const logKey = "m" + std::to_string(low);
            std::string const state = left ? std::string("short") : decision;  // a waiting member: once
            if (goldLogged[logKey] != state)
            {
                goldLogged[logKey] = state;
                LOG_INFO("playerbots", "[mount4] the global leader: {}", decision);
            }
        }
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
                                "learn " + name + " (skill " + skillText + ") — in the guild you are the best for it.");
                    else
                        recipeLearn_.emplace_back(holderRow->low, it->GetGUID().GetCounter());
                    continue;
                }
                // [econ2] mailed only to a member that learns it at once (its profession learned, its skill at the
                // recipe's rank): b353 Pattern: Stylish Green Shirt went to Orenem at Tailoring 11/120 and lay in
                // its bags; the best learner later is chosen again once someone can learn it
                if (!now)
                    continue;
                if (taught)
                    claimed.insert({bestLow, taught});
                Want w;
                w.key = "l" + std::to_string(it->GetGUID().GetCounter());
                w.from = holderRow->low;
                w.to = bestLow;
                w.guid = it->GetGUID().GetCounter();
                w.itemId = proto->ItemId;
                w.count = 1;
                w.purpose = "recipe";
                w.why = "learned by " + Who(bestLow) + " (skill " + skillText +
                        (now ? ", learns at once)" : ", learns once its skill is there)");
                if (HumanCharacter(best) && !HumanCharacter(h))
                    Suggest(bestLow, "r:" + std::to_string(proto->ItemId),
                            name + " comes to you by mail from " + Who(holderRow->low) + " — learn it (skill " +
                                skillText + ").");
                wants.push_back(w);
                used.insert(w.guid);
            }
        }
    }

    if (donation != donationState_)
    {
        // [econ1] b350: 189 "donation plan" lines - the members' free copper changed every update; a new line
        // only when the verdict changes (the text without its amounts), the plan row keeps the numbers
        auto const verdict = [](std::string text)
        {
            text.erase(std::remove_if(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; }),
                       text.end());
            return text;
        };
        bool const changed = verdict(donation) != verdict(donationState_);
        donationState_ = donation;
        if (!donation.empty() && changed)
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
                                           "you have " + what + " — needed " + Who(w.to) + " (" + w.purpose + ": " +
                                               w.why + "). Give it by trade or mail if you agree.");
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
                if (!t.gold)  // [ench162] the item reached the receiver, or went back to the sender
                {
                    MarkBagsChanged(t.toLow);
                    MarkBagsChanged(t.fromLow);
                }
                if (arrived && t.purpose == "to disenchant")
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
            c.text += "; no channel";
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
        if (!t.gold && t.purpose == "upgrade")
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
        if (t.key[0] == 'b')
            LOG_INFO("playerbots", "[econ1] bag relief {} -> {}: {} by {}", Who(t.fromLow), Who(t.toLow), what,
                     ChannelLog(t.channel));
        if (t.purpose != "consumable")
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
        CoordLock guard(lock_, __func__);
        learn.swap(recipeLearn_);
        for (auto const& [id, t] : transfers_)
            if (t.channel != Transfer::BANK && !t.mailed)
                list.push_back(t);
        // [inv370] every member at any mailbox it stands at takes its letters (not only one with a pickup task:
        // a bot passing a mailbox or standing at one in a town empties it)
        for (PlanRow const& row : plan_)
            pickups.push_back(row.low);
    }
    // [econ2] member -> (taught spell, recipe name) of a Learning cast begun (world thread only)
    static std::map<uint32, std::pair<uint32, std::string>> recipeCast;
    for (auto const& [low, guid] : learn)
    {
        Player* p = Online(low);
        Item* item = p && p->IsAlive() && !HumanCharacter(p)
                         ? p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid))
                         : nullptr;
        if (!item)
            continue;
        std::string const name = ItemText(item->GetEntry(), 1);
        uint32 const taught = RecipeSpell(item->GetTemplate());
        bool const known = taught && p->HasSpell(taught);
        std::string const err = LearnRecipe(p, item);
        if (err == "-")
            continue;
        LOG_INFO("playerbots", "[recipes] {} learns {}: {}", p->GetName(), name,
                 err.empty() ? (known ? "used (already known)" : "used") : err);
        if (err.empty() && taught && !known)
            recipeCast[low] = {taught, name};
    }
    // [econ2] the Learning cast ended: learned, or broken off (the item stays and is used again)
    for (auto it = recipeCast.begin(); it != recipeCast.end();)
    {
        Player* p = Online(it->first);
        if (p && p->IsNonMeleeSpellCast(false) && !p->HasSpell(it->second.first))
        {
            ++it;
            continue;
        }
        if (p)
            LOG_INFO("playerbots", "[econ2] recipe {} {}: {}", p->GetName(), it->second.second,
                     p->HasSpell(it->second.first) ? "learned" : "the cast was broken off, used again later");
        it = recipeCast.erase(it);
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
            uint32 handBack = 0;  // [dun254] what the receiver gives back of a whole stack it took
            uint32 const entry = item ? item->GetEntry() : 0;
            if (item && t.count && t.count < item->GetCount())
            {
                Item* const whole = item;
                item = Split(s, item, t.count);
                // [share103] no free slot to split the stack (the giver's bags full): a consumable share goes as the
                // whole stack when the giver keeps at least as much of that kind as it hands over - a player trades
                // the stack it cannot split. b417 rfc-n five 2: "trade #32 Ainne -> #50 Thyllyn refused: no free bag
                // slot to split the stack" - the druid healer went in with 0 drinks, healed 116 in the wipe (5 dead)
                if (!item && t.purpose == "consumable")
                {
                    uint8 const kind = ConsumableKind(whole->GetTemplate());
                    uint32 const giverHas = CountKind(s, kind, s->GetLevel());
                    if (kind && giverHas >= 2 * whole->GetCount())
                        item = whole;
                    // [dun254] b461 wc-n five4-4: Idunk's bags full, its one stack of 19 Ice Cold Milk under the
                    // rule above - "no free bag slot to split the stack" on every update, the healer Pherua at 0-2
                    // drinks for an hour (537 regen stalls on spirit regen), the run timed out at 7/8. The whole
                    // stack goes and the receiver hands the rest back in a second trade: the given stack's slot
                    // is free in the giver's bags, the split is the receiver's
                    else if (kind && r->GetFreeInventorySpace() > 0)
                    {
                        item = whole;
                        handBack = whole->GetCount() - t.count;
                    }
                }
                if (!item)
                {
                    outcomes.push_back({t.id, false, false, 0, 0, "no free bag slot to split the stack", 0, -1});
                    continue;
                }
            }
            uint32 const guid = item ? item->GetGUID().GetCounter() : 0;
            std::string const err = DoTrade(s, r, item, money);
            if (err.empty() && handBack)
            {
                // [dun254] the rest of the whole stack back to the giver (the receiver's own split)
                Item* stack = nullptr;
                ForEachBagItem(r, [&](Item* i)
                               {
                                   if (i->GetEntry() == entry && i->GetCount() > handBack &&
                                       (!stack || i->GetCount() > stack->GetCount()))
                                       stack = i;
                               });
                Item* back = stack ? Split(r, stack, handBack) : nullptr;
                std::string const backErr = back ? DoTrade(r, s, back, 0) : "no stack to split";
                logs.push_back("trade " + Who(t.toLow) + " -> " + Who(t.fromLow) + ": " + ItemText(entry, handBack) +
                               " back of the whole stack [dun254]" +
                               (backErr.empty() ? std::string() : " refused: " + backErr + " (the receiver keeps it)"));
            }
            if (err.empty())
            {
                logs.push_back("trade " + Who(t.fromLow) + " -> " + Who(t.toLow) + ": " + what + " (" + t.purpose +
                               ": " + t.why + "; " + t.costs + ")");
                outcomes.push_back({t.id, true, false, 0, guid, "", money, -1});
            }
            else
            {
                // [dun254] a refusal of the core's trade opening (a stun, a trade of the other's) passes: tried again
                // at once; b461 five4-4 waited the rest of the run on the receiver's free slots changing
                bool const passing = err == "the core did not open the trade";
                outcomes.push_back(
                    {t.id, false, false, 0, 0, err, 0, passing ? -1 : int32(r->GetFreeInventorySpace())});
            }
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
            // [supply112] a share of a stack (the cook's supply to each member): split off first, as the trade does;
            // no free slot to split it - the whole stack goes when the sender keeps at least as much again
            if (item && t.count && t.count < item->GetCount())
            {
                Item* const whole = item;
                item = Split(s, item, t.count);
                if (!item && t.purpose == "consumable" &&
                    s->GetItemCount(whole->GetEntry(), false) >= 2 * whole->GetCount())
                    item = whole;
                if (!item)
                {
                    outcomes.push_back({t.id, false, false, 0, 0, "no free bag slot to split the stack", 0, -1});
                    continue;
                }
            }
            uint32 const before = s->GetMoney();
            std::string const err =
                DoMail(s, box, name, item, money, t.gold ? "For the guild bank tab" : "For you: " + t.purpose);
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
    // [inv370] ... and the game's own letters: what is worth a bag slot taken, a letter left with nothing of worth
    // deleted as a player clears it (b446: 18 Shiny Baubles 43239, no price, no quest, no use)
    static std::map<uint32, size_t> waitLogged;  // [inv370] member -> its letters when "mail waits" was last logged
    for (uint32 low : pickups)
    {
        Player* r = Online(low);
        if (!r || !r->IsAlive() || HumanCharacter(r) || r->IsInFlight())
            continue;  // [human] a human takes his mail himself
        std::vector<uint32> ids;
        for (Mail const* m : r->GetMails())
            if (m && m->state != MAIL_STATE_DELETED && !m->COD && uint32(m->deliver_time) <= now &&
                (GameLetter(m) || (m->messageType == MAIL_NORMAL && IsPlanned(m->sender))))
                ids.push_back(m->messageID);
        GameObject* box = ids.empty() ? nullptr : MailboxAt(r);
        if (!box)
            continue;
        bool stuck = false;
        for (uint32 mailId : ids)
        {
            Mail* m = r->GetMail(mailId);
            if (!m)
                continue;
            uint32 const sender = m->sender;
            bool const game = GameLetter(m);
            uint32 const deliveredAt = std::min<uint32>(now, uint32(m->deliver_time));
            std::string const from = game ? GameSender(m) : Who(sender);
            std::vector<MailItemInfo> const items = m->items;
            uint32 taken = 0;
            std::string got, junk;
            bool full = false, kept = false;
            for (MailItemInfo const& info : items)
            {
                Item* it = r->GetMItem(info.item_guid);
                std::string const name =
                    it ? ItemText(it->GetEntry(), it->GetCount()) : ItemText(info.item_template, 1);
                if (game && !LetterItemWorth(r, sObjectMgr->GetItemTemplate(info.item_template)))
                {
                    junk += (junk.empty() ? "" : ", ") + name;
                    continue;  // [inv370] not worth its slot: goes with the letter
                }
                // [econ1] b350: Idunk "could not take ... (bags full?)" 11 times a letter, every second at the box -
                // the core's own check first (HandleMailTakeItem: CanStoreItem); full bags end the take here, the
                // rest stays in the mail for the next visit (its vendor visit makes room)
                ItemPosCountVec dest;
                InventoryResult const res =
                    it ? r->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) : EQUIP_ERR_OK;
                if (res != EQUIP_ERR_OK && game && res != EQUIP_ERR_INVENTORY_FULL)
                {
                    kept = true;  // [inv370] a game letter's item it cannot carry (a unique one it holds): left there
                    continue;
                }
                if (res != EQUIP_ERR_OK)
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
                    logs.push_back(r->GetName() + " could not take " + name + " from the mail of " + from +
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
                GuildLedger::Cause const ledger(game ? "mail taken (gold from the game)"
                                                     : "mail taken (gold from members)");  // [eco190]
                r->GetSession()->HandleMailTakeMoney(take);
                m = r->GetMail(mailId);
                if (!m || !m->money)
                    got += (got.empty() ? "" : ", ") + Gold(money);
            }
            m = r->GetMail(mailId);
            // [inv370] a game letter whose rest is worth nothing to it is deleted with that rest
            bool const onlyJunk = m && game && !full && !kept && !m->money && !junk.empty();
            if (m && ((m->items.empty() && !m->money) || onlyJunk))
            {
                WorldPacket del(CMSG_MAIL_DELETE);
                del << box->GetGUID() << mailId << uint32(0);
                r->GetSession()->HandleMailDelete(del);
                if (onlyJunk)
                    LOG_INFO("playerbots", "[inv370] {} L{} deletes the letter of {}: {} - no price, no quest, no use "
                             "to it", r->GetName(), r->GetLevel(), from, junk);
            }
            if (!got.empty())
            {
                logs.push_back(Who(low) + " takes the mail of " + from + " at a mailbox: " + got);
                if (game)
                    LOG_INFO("playerbots", "[inv370] {} L{} takes the game's letter of {} at a mailbox: {} ({} min "
                             "after its delivery)", r->GetName(), r->GetLevel(), from, got,
                             (now - deliveredAt) / 60);
            }
            if (taken)
            {
                LOG_INFO("playerbots", "[econ1] mail collected: {} took {} item(s) from {}", Who(low), taken, from);
                if (Player* taker = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low)))
                    QuestStats::TownDone(low, taker->GetName(), "mail");  // [log28]
            }
            if (full)
            {
                if (waitLogged[low] != ids.size())
                    LOG_INFO("playerbots", "[econ1] mail waits: {} has no bag room for the rest of its mail", Who(low));
                waitLogged[low] = ids.size();
                stuck = true;
                break;
            }
        }
        if (!stuck)
            waitLogged.erase(low);
    }

    CoordLock guard(lock_, __func__);
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
            if (!t.gold)  // [ench162] the traded item is in the receiver's bags
                MarkBagsChanged(t.toLow);
            if (t.purpose == "to disenchant" && o.newGuid)
                bots_[t.toLow].disenchant.push_back(o.newGuid);
            Decision(Who(t.fromLow) + " gave " + Who(t.toLow) +
                     " by trade: " + (t.gold ? Gold(t.money) : ItemText(t.itemId, t.count)) + " (" + t.purpose + ")");
            transfers_.erase(it);
        }
        else if (o.mailed)
        {
            t.mailed = true;
            t.deliverAt = o.deliverAt;
            Decision(Who(t.fromLow) + " mailed " + Who(t.toLow) + ": " +
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
                           : "receives " + what + " from " + Who(t.fromLow) + ", " + ChannelName(t.channel);
        o.reason = (t.purpose == "supply" ? "SUPPLY " : "") + t.purpose + ": " + t.why + " (" + t.costs + ")";
        if (t.mailed)
        {
            if (sender)
                continue;  // its part is done
            o.status = t.deliverAt > NowSec()
                           ? "in transit, delivery in " + std::to_string((t.deliverAt - NowSec() + 59) / 60) + " min"
                           : "delivered: collect at a mailbox";
        }
        else if (t.channel == Transfer::MAIL)
        {
            o.executable = sender && !t.stalled;
            o.pos = t.mailbox;
            o.status = sender
                           ? "to the mailbox " +
                                 std::to_string(uint32(p->GetExactDist(
                                     t.mailbox.GetPositionX(), t.mailbox.GetPositionY(), t.mailbox.GetPositionZ()))) +
                                 " yd"
                           : "waits for the letter";
        }
        else
        {
            o.executable = t.walkerLow == low;
            o.pos = t.meet;
            if (!t.walkerLow)
                o.status = "trade at a meeting (≤" + std::to_string(uint32(TRADE_DISTANCE)) + " yd)";
            else if (t.walkerLow == low)
                o.status = Valid(t.meet) ? "walks to the meeting point" : "walks to " + Who(o.forLow);
            else
                o.status = "waits: " + Who(t.walkerLow) + " walks to it";
        }
        if (!t.failed.empty())
            o.status += "; refused: " + t.failed;
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
        o.details = "collect mail: " + std::to_string(o.count) + " letters from the guild";
        o.reason = "delivered by the core";
        // [eco199] gold its planned need waits for: a town stop even while its five gathers
        o.needGold = needGoldMail_.count(low) != 0;
        if (o.needGold)
            o.reason += "; the members' gold its repair / supplies / role spec waits for [eco199]";
        o.status =
            "to the mailbox " +
            std::to_string(uint32(p->GetExactDist(o.pos.GetPositionX(), o.pos.GetPositionY(), o.pos.GetPositionZ()))) +
            " yd";
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
        order.status = "no safe way — quests";
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
        std::string state =
            t.mailed ? (left ? "in the mail, delivery in " + std::to_string(left) + " min" : "in the mail: collect")
            : t.channel == Transfer::BANK ? "via the bank"
            : t.channel == Transfer::MAIL ? "to the mailbox"
            : t.walkerLow                 ? "walks " + Who(t.walkerLow)
                                          : "at a meeting";
        if (!t.failed.empty())
            state += "; refused";
        rows.push_back({"transfer", state, Who(t.fromLow), what, Who(t.toLow),
                        std::string(t.gold ? "donation, " : "") + ChannelName(t.channel),
                        t.purpose + ": " + t.why + " (" + t.costs + ")"});
    }
    if (!donationState_.empty())
        rows.push_back(
            {"money", "donations", "guild members", "bank tab 1", "guild master", "mail / trade", donationState_});
}

// ---------------------------------------------------------------------------------------------------------------------
// [craft386] user 2026-11-11 «enchanters enchant the guild's gear»: the guild's Enchanting (about 49 from members'
// disenchanted greens, ench160-171) went into the enchanters' own pieces only ([ench156], the craft executor's skill-up
// casts). An enchanter now enchants the worn pieces of the members it meets with what its skill and its bags allow,
// as players do: the member takes the piece off, puts it in the trade's "will not be traded" slot, the enchanter casts
// on it, both accept (the core casts at the accept, WorldSession::HandleAcceptTradeOpcode), the member wears it again.
// The enchant is the one the member's stat weights value most; a piece that already has a permanent enchantment is
// never overwritten. The reagents are the enchanter's own (the core's triggered trade cast takes them but does not
// check them: they are checked here first - no free enchant)
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
    struct EnchantRecipe
    {
        uint32 spellId{0};
        uint32 enchantId{0};
        uint32 yellow{0};
        uint32 gray{0};
        std::vector<std::pair<uint32, uint32>> reagents;
    };

    // the Enchanting recipes that enchant an item (SkillLineAbility of the line, SPELL_EFFECT_ENCHANT_ITEM), world
    // thread, built once
    std::vector<EnchantRecipe> const& EnchantRecipes()
    {
        static std::vector<EnchantRecipe> list;
        static bool loaded = false;
        if (loaded)
            return list;
        loaded = true;
        std::set<uint32> seen;
        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
            if (!ability || ability->SkillLine != SKILL_ENCHANTING || !seen.insert(ability->Spell).second)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell);
            if (!info)
                continue;
            EnchantRecipe r;
            r.spellId = ability->Spell;
            r.yellow = ability->TrivialSkillLineRankLow;
            r.gray = ability->TrivialSkillLineRankHigh;
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS && !r.enchantId; ++e)
                if (info->Effects[e].Effect == SPELL_EFFECT_ENCHANT_ITEM && info->Effects[e].MiscValue > 0)
                    r.enchantId = uint32(info->Effects[e].MiscValue);
            for (uint8 k = 0; k < MAX_SPELL_REAGENTS; ++k)
                if (info->Reagent[k] > 0 && info->ReagentCount[k] > 0)
                    r.reagents.emplace_back(uint32(info->Reagent[k]), info->ReagentCount[k]);
            if (r.enchantId && !r.reagents.empty())
                list.push_back(std::move(r));
        }
        LOG_INFO("playerbots", "[craft386] {} Enchanting recipes enchant an item", list.size());
        return list;
    }

    // the recipe's tools in the caster's bags (Runed Copper Rod: TotemCategory): the triggered trade cast does not
    // check them either
    bool EnchantToolsHeld(Player* e, SpellInfo const* info)
    {
        for (uint32 totem : info->Totem)
            if (totem && !e->HasItemCount(totem, 1))
                return false;
        for (uint32 category : info->TotemCategory)
            if (category && !e->HasItemTotemCategory(category))
                return false;
        return true;
    }

    std::string SpellNameOf(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        return info ? std::string(info->SpellName[0]) : std::to_string(spellId);
    }

    // Spell::CheckItems (SPELL_EFFECT_ENCHANT_ITEM): the item's required level (else its item level) reaches the
    // spell's base level
    bool EnchantLevelOk(SpellInfo const* info, ItemTemplate const* proto)
    {
        if (info->HasAttribute(SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF))
            return true;
        uint32 const required = proto->RequiredLevel ? proto->RequiredLevel : proto->ItemLevel;
        return required >= info->BaseLevel;
    }

    // the enchanter's trade with the member: the piece in the member's "will not be traded" slot, the enchant spell on
    // the enchanter's side (TradeData::SetSpell, what the client's cast on a trade item does), both accept
    std::string EnchantInTrade(Player* e, Player* m, Item* item, uint32 spellId)
    {
        auto const cancel = [&](std::string why)
        {
            if (e->GetTradeData())
            {
                WorldPacket packet(CMSG_CANCEL_TRADE);
                e->GetSession()->HandleCancelTradeOpcode(packet);
            }
            return why;
        };
        WorldPacket init(CMSG_INITIATE_TRADE);
        init << m->GetGUID();
        e->GetSession()->HandleInitiateTradeOpcode(init);
        if (!e->GetTradeData() || e->GetTrader() != m)
            return cancel("the core did not open the trade");
        WorldPacket begin(CMSG_BEGIN_TRADE);
        m->GetSession()->HandleBeginTradeOpcode(begin);
        WorldPacket set(CMSG_SET_TRADE_ITEM);
        set << uint8(TRADE_SLOT_NONTRADED) << item->GetBagSlot() << item->GetSlot();
        m->GetSession()->HandleSetTradeItemOpcode(set);
        if (!m->GetTradeData() || m->GetTradeData()->GetItem(TRADE_SLOT_NONTRADED) != item || !e->GetTradeData())
            return cancel("the piece did not go into the will-not-be-traded slot");
        e->GetTradeData()->SetSpell(spellId);
        WorldPacket accept(CMSG_ACCEPT_TRADE);
        e->GetSession()->HandleAcceptTradeOpcode(accept);
        if (m->GetTradeData())
        {
            WorldPacket accept2(CMSG_ACCEPT_TRADE);
            m->GetSession()->HandleAcceptTradeOpcode(accept2);
        }
        if (e->GetTradeData())
            return cancel("the core did not complete the trade (the cast was refused)");
        return "";
    }
}

void GuildCoordinator::UpdateEnchantOrders()
{
    std::vector<uint32> lows;
    {
        CoordLock guard(lock_, __func__);
        for (PlanRow const& row : plan_)
            lows.push_back(row.low);
    }
    std::vector<EnchantRecipe> const& recipes = EnchantRecipes();
    std::vector<EnchantOrder> orders;
    std::map<uint32, std::unique_ptr<StatsWeightCalculator>> weights;  // member -> its stat weights (this look)
    std::set<uint32> taken;                                             // member pieces already given an order
    for (uint32 enchanterLow : lows)
    {
        Player* e = Online(enchanterLow);
        if (!e || !GET_PLAYERBOT_AI(e) || HumanCharacter(e) || !e->IsAlive() || !e->HasSkill(SKILL_ENCHANTING))
            continue;
        // what it can cast now: a recipe it knows, the reagents in its bags
        std::map<uint32, uint32> pool;
        std::vector<EnchantRecipe const*> castable;
        for (EnchantRecipe const& r : recipes)
        {
            if (!e->HasSpell(r.spellId))
                continue;
            bool held = true;
            for (auto const& [item, n] : r.reagents)
            {
                uint32& have = pool.try_emplace(item, e->GetItemCount(item, false)).first->second;
                held = held && have >= n;
            }
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(r.spellId); held && info && EnchantToolsHeld(e, info))
                castable.push_back(&r);
        }
        if (castable.empty())
            continue;
        uint32 const skill = e->GetPureSkillValue(SKILL_ENCHANTING);
        struct Candidate
        {
            float gain{0.0f};
            bool skillUp{false};
            Player* m{nullptr};
            Item* item{nullptr};
            EnchantRecipe const* recipe{nullptr};
        };
        std::vector<Candidate> candidates;
        for (uint32 memberLow : lows)
        {
            Player* m = memberLow == enchanterLow ? nullptr : Online(memberLow);
            if (!m || !GET_PLAYERBOT_AI(m) || HumanCharacter(m) || !m->IsAlive() || m->GetMap() != e->GetMap())
                continue;
            // the members it meets: its group, or in its sight (the trade waits until they stand side by side)
            bool const meets = (e->GetGroup() && e->GetGroup() == m->GetGroup()) ||
                               e->IsWithinDistInMap(m, sPlayerbotAIConfig.SightDistance);
            if (!meets)
                continue;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item* item = m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!item || item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) || taken.count(item->GetGUID().GetCounter()))
                    continue;
                ItemTemplate const* proto = item->GetTemplate();
                for (EnchantRecipe const* r : castable)
                {
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(r->spellId);
                    if (!info || !item->IsFitToSpellRequirements(info) || !EnchantLevelOk(info, proto))
                        continue;
                    if (auto const refused = enchantRefused_.find({item->GetGUID().GetCounter(), r->spellId});
                        refused != enchantRefused_.end() && refused->second == m->GetLevel())
                        continue;
                    std::unique_ptr<StatsWeightCalculator>& calc = weights[memberLow];
                    if (!calc)
                        calc = std::make_unique<StatsWeightCalculator>(m);
                    float const gain =
                        calc->CalculateEnchant(r->enchantId, proto->Class == ITEM_CLASS_WEAPON ? proto->Delay : 0,
                                               slot == EQUIPMENT_SLOT_RANGED);
                    if (gain <= 0.0f)
                        continue;
                    candidates.push_back({gain, skill < r->gray, m, item, r});
                }
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](Candidate const& a, Candidate const& b) { return a.gain > b.gain; });
        for (Candidate const& c : candidates)
        {
            uint32 const guid = c.item->GetGUID().GetCounter();
            if (taken.count(guid))
                continue;
            bool held = true;
            for (auto const& [item, n] : c.recipe->reagents)
                held = held && pool[item] >= n;
            if (!held)
                continue;
            for (auto const& [item, n] : c.recipe->reagents)
                pool[item] -= n;
            taken.insert(guid);
            EnchantOrder o;
            o.enchanterLow = enchanterLow;
            o.memberLow = c.m->GetGUID().GetCounter();
            o.itemGuidLow = guid;
            o.spellId = c.recipe->spellId;
            o.enchantId = c.recipe->enchantId;
            o.gain = c.gain;
            o.text = SpellNameOf(c.recipe->spellId) + " on " + c.m->GetName() + "'s " +
                     c.item->GetTemplate()->Name1 + " (stat weight " + Acore::StringFormat("{:.1f}", c.gain) +
                     (c.skillUp ? ", a skill-up chance at " + std::to_string(skill) : "") + ")";
            orders.push_back(std::move(o));
        }
    }
    // each order once when it is planned
    std::set<std::tuple<uint32, uint32, uint32>> before;
    for (EnchantOrder const& o : enchantOrders_)
        before.emplace(o.enchanterLow, o.itemGuidLow, o.spellId);
    for (EnchantOrder const& o : orders)
        if (!before.count({o.enchanterLow, o.itemGuidLow, o.spellId}))
            LOG_INFO("playerbots", "[craft386] plan: {} enchants {} - when they stand side by side",
                     Who(o.enchanterLow), o.text);
    enchantOrders_ = std::move(orders);
}

void GuildCoordinator::RunEnchantOrders(uint32 diff)
{
    enchantTimer_ += diff;
    if (enchantTimer_ < IN_MILLISECONDS || enchantOrders_.empty())
        return;
    enchantTimer_ = 0;
    std::set<uint32> busy;  // one trade a member a run
    for (auto it = enchantOrders_.begin(); it != enchantOrders_.end();)
    {
        EnchantOrder const& o = *it;
        Player* e = Online(o.enchanterLow);
        Player* m = Online(o.memberLow);
        Item* item = m ? m->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(o.itemGuidLow)) : nullptr;
        if (!e || !m || !item || item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) || !e->HasSpell(o.spellId))
        {
            it = enchantOrders_.erase(it);  // gone, enchanted or no longer known: the next look plans again
            continue;
        }
        bool const ready = !busy.count(o.enchanterLow) && !busy.count(o.memberLow) && e->IsAlive() && m->IsAlive() &&
                           !e->IsInCombat() && !m->IsInCombat() && !e->IsInFlight() && !m->IsInFlight() &&
                           !e->GetTradeData() && !m->GetTradeData() && !e->IsNonMeleeSpellCast(false) &&
                           !m->IsNonMeleeSpellCast(false) && e->GetMap() == m->GetMap() &&
                           e->IsWithinDistInMap(m, TRADE_DISTANCE) && item->IsEquipped();
        SpellInfo const* info = sSpellMgr->GetSpellInfo(o.spellId);
        bool reagents = info && EnchantToolsHeld(e, info);
        for (uint8 k = 0; info && k < MAX_SPELL_REAGENTS; ++k)
            if (info->Reagent[k] > 0 && info->ReagentCount[k] > 0)
                reagents = reagents && e->HasItemCount(uint32(info->Reagent[k]), info->ReagentCount[k]);
        if (!reagents)
        {
            it = enchantOrders_.erase(it);
            continue;
        }
        if (!ready)
        {
            ++it;
            continue;
        }
        busy.insert(o.enchanterLow);
        busy.insert(o.memberLow);
        // the member takes the piece off into a free bag slot, as a player puts it in the trade window
        uint8 const wornSlot = item->GetSlot();
        uint8 bag = 0, slot = 0;
        std::string err;
        if (!FreeSlot(m, bag, slot))
            err = "no free bag slot to take the piece off";
        else
        {
            m->SwapItem(uint16((INVENTORY_SLOT_BAG_0 << 8) | wornSlot), uint16((bag << 8) | slot));
            if (m->GetItemByPos(bag, slot) != item)
                err = "the piece did not come off";
        }
        if (err.empty())
            err = EnchantInTrade(e, m, item, o.spellId);
        bool const enchanted = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) == o.enchantId;
        // and wears it again
        if (!item->IsEquipped() && m->GetItemByPos(bag, slot) == item)
            m->SwapItem(uint16((bag << 8) | slot), uint16((INVENTORY_SLOT_BAG_0 << 8) | wornSlot));
        if (enchanted)
            LOG_INFO("playerbots", "[craft386] done: {} enchanted {}; Enchanting {}, worn again: {}",
                     Who(o.enchanterLow), o.text, e->GetPureSkillValue(SKILL_ENCHANTING),
                     item->IsEquipped() ? "yes" : "no (its own equip picks it)");
        else
        {
            enchantRefused_[{o.itemGuidLow, o.spellId}] = m->GetLevel();
            LOG_INFO("playerbots", "[craft386] refused: {} could not enchant {}: {} - not again until {}'s next level",
                     Who(o.enchanterLow), o.text, err.empty() ? "the enchantment is not on the piece" : err,
                     m->GetName());
        }
        it = enchantOrders_.erase(it);
    }
}
