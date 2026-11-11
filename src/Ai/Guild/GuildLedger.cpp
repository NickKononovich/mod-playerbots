/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildLedger.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "GameTime.h"
#include "GuildCoordinator.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "StringFormat.h"

namespace
{
constexpr uint32 SPELL_CATEGORY_FOOD = 11;   // the core's spell category of food (BuyAction's restock items)
constexpr uint32 SPELL_CATEGORY_DRINK = 59;  // ... and of drink
constexpr uint32 LEDGER_LOG_SECONDS = 60;    // the [health] line's minute: the totals log with it, on change

// a cause the core's own hook named for this player's next money change in the same world tick
struct Pending
{
    uint32 low{0};
    uint64 ms{0};
    char const* cause{nullptr};
};
thread_local Pending pending;
thread_local char const* scoped = nullptr;

struct Book
{
    int64 in{0};
    int64 out{0};
};
std::mutex ledgerLock;
std::map<std::string, Book> books;  // cause -> the guild's money in / out by it
uint32 firstAt = 0;
uint32 lastLogAt = 0;
std::string lastLogged;

void Expect(Player* player, char const* cause)
{
    if (!player || !sGuildCoordinator.IsPlanned(player->GetGUID().GetCounter()))
        return;
    pending.low = player->GetGUID().GetCounter();
    pending.ms = GameTime::GetGameTimeMS().count();
    pending.cause = cause;
}

std::string Gsc(int64 copper)
{
    uint64 const c = uint64(copper < 0 ? -copper : copper);
    std::string out = copper < 0 ? "-" : "";
    if (c >= 10000)
        out += std::to_string(c / 10000) + "g";
    if (c >= 100)
        out += std::to_string(c / 100 % 100) + "s";
    out += std::to_string(c % 100) + "c";
    return out;
}

std::string TextLocked();

void BookChange(Player* player, int32 amount)
{
    if (!amount || !player || !sGuildCoordinator.IsPlanned(player->GetGUID().GetCounter()))
        return;
    char const* cause = scoped;
    if (pending.cause && pending.low == player->GetGUID().GetCounter() &&
        pending.ms == uint64(GameTime::GetGameTimeMS().count()))
    {
        cause = pending.cause;
        pending.cause = nullptr;
    }
    if (!cause)
        cause = amount > 0 ? "other in" : "other out";
    uint32 const now = uint32(GameTime::GetGameTime().count());
    std::string line;
    {
        std::lock_guard<std::mutex> guard(ledgerLock);
        if (!firstAt)
            firstAt = now;
        Book& b = books[cause];
        (amount > 0 ? b.in : b.out) += amount;
        if (now - lastLogAt >= LEDGER_LOG_SECONDS)
        {
            lastLogAt = now;
            line = TextLocked();
            if (line == lastLogged)
                line.clear();
            else
                lastLogged = line;
        }
    }
    if (!line.empty())
        LOG_INFO("playerbots", "[eco190] {}", line);
}
}  // namespace

namespace GuildLedger
{
Cause::Cause(char const* what) : prev_(scoped) { scoped = what; }

Cause::~Cause() { scoped = prev_; }

char const* BuyCause(ItemTemplate const* proto)
{
    if (!proto)
        return "buy: other";
    switch (proto->Class)
    {
        case ITEM_CLASS_CONSUMABLE:
            if (proto->Spells[0].SpellCategory == SPELL_CATEGORY_FOOD ||
                proto->Spells[0].SpellCategory == SPELL_CATEGORY_DRINK)
                return "buy: food / drink";
            if (proto->SubClass == ITEM_SUBCLASS_POTION || proto->SubClass == ITEM_SUBCLASS_ELIXIR ||
                proto->SubClass == ITEM_SUBCLASS_FLASK)
                return "buy: potion";
            return "buy: other consumable";
        case ITEM_CLASS_PROJECTILE:
        case ITEM_CLASS_QUIVER:
            return "buy: ammo";
        case ITEM_CLASS_WEAPON:
        case ITEM_CLASS_ARMOR:
            return proto->TotemCategory ? "buy: tool" : "buy: gear";
        case ITEM_CLASS_CONTAINER:
            return "buy: bag";
        case ITEM_CLASS_RECIPE:
            return "buy: recipe";
        case ITEM_CLASS_TRADE_GOODS:
        case ITEM_CLASS_REAGENT:
            return "buy: reagent";
        default:
            return proto->TotemCategory ? "buy: tool" : "buy: other";
    }
}

std::string Text()
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    return TextLocked();
}
}  // namespace GuildLedger

namespace
{
// ledgerLock held
std::string TextLocked()
{
    if (books.empty())
        return "";
    uint32 const now = uint32(GameTime::GetGameTime().count());
    float const hours = std::max(float(now - firstAt) / 3600.0f, 1.0f / 60.0f);
    std::string in, out;
    int64 net = 0;
    for (auto const& [cause, b] : books)
    {
        if (b.in)
            in += Acore::StringFormat("{}{} +{} ({}/h)", in.empty() ? "" : ", ", cause, Gsc(b.in),
                                      Gsc(int64(float(b.in) / hours)));
        if (b.out)
            out += Acore::StringFormat("{}{} {} ({}/h)", out.empty() ? "" : ", ", cause, Gsc(b.out),
                                       Gsc(int64(float(b.out) / hours)));
        net += b.in + b.out;
    }
    return Acore::StringFormat("guild money by cause over {:.1f} h: in {}; out {}; net {} ({}/h)", hours,
                               in.empty() ? "-" : in, out.empty() ? "-" : out, Gsc(net),
                               Gsc(int64(float(net) / hours)));
}
}  // namespace

class GuildLedgerPlayerScript : public PlayerScript
{
public:
    GuildLedgerPlayerScript()
        : PlayerScript("GuildLedgerPlayerScript",
                       {PLAYERHOOK_ON_MONEY_CHANGED, PLAYERHOOK_ON_BEFORE_LOOT_MONEY, PLAYERHOOK_ON_QUEST_COMPUTE_EXP,
                        PLAYERHOOK_ON_BEFORE_DURABILITY_REPAIR, PLAYERHOOK_ON_BEFORE_BUY_ITEM_FROM_VENDOR,
                        PLAYERHOOK_CAN_SELL_ITEM, PLAYERHOOK_CAN_SEND_MAIL})
    {
    }

    void OnPlayerMoneyChanged(Player* player, int32& amount) override { BookChange(player, amount); }

    void OnPlayerBeforeLootMoney(Player* player, Loot* /*loot*/) override { Expect(player, "loot"); }

    // Player::RewardQuest computes the XP right before it pays the quest's money
    void OnPlayerQuestComputeXP(Player* player, Quest const* /*quest*/, uint32& /*xpValue*/) override
    {
        Expect(player, "quest reward");
    }

    void OnPlayerBeforeDurabilityRepair(Player* player, ObjectGuid /*npcGUID*/, ObjectGuid /*itemGUID*/,
                                        float& /*discountMod*/, uint8 /*guildBank*/) override
    {
        Expect(player, "repair");
    }

    void OnPlayerBeforeBuyItemFromVendor(Player* player, ObjectGuid /*vendorguid*/, uint32 /*vendorslot*/,
                                         uint32& item, uint8 /*count*/, uint8 /*bag*/, uint8 /*slot*/) override
    {
        Expect(player, GuildLedger::BuyCause(sObjectMgr->GetItemTemplate(item)));
    }

    bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        Expect(player, proto && proto->Quality == ITEM_QUALITY_POOR      ? "sale: junk"
                       : proto && proto->Quality >= ITEM_QUALITY_UNCOMMON ? "sale: green and better"
                                                                          : "sale: white items");
        return true;
    }

    bool OnPlayerCanSendMail(Player* player, ObjectGuid /*receiverGuid*/, ObjectGuid /*mailbox*/,
                             std::string& /*subject*/, std::string& /*body*/, uint32 /*money*/, uint32 /*COD*/,
                             Item* /*item*/) override
    {
        Expect(player, "mail sent (postage, gold to members)");
        return true;
    }
};

void AddGuildLedgerScripts() { new GuildLedgerPlayerScript(); }
