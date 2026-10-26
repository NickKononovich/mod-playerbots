/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LootRollAction.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "DBCStores.h"
#include "Event.h"
#include "Group.h"
#include "ItemUsageValue.h"
#include "LootAction.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"

namespace
{
    constexpr uint32 LOOT_CLAIM_DELAY_MS = 1500;  // time for every bot in the group to publish its claim
    constexpr uint32 LOOT_CLAIM_EXPIRE_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 LOOT_FAIRNESS_WINDOW_MS = 2 * HOUR * IN_MILLISECONDS;
    constexpr float LOOT_FAIRNESS_WEIGHT = 0.5f;  // each recent need win shrinks the next claim by this much
    constexpr float MAX_UPGRADE_GAIN = 1.0f;       // an empty slot or an unscored old item
    constexpr float TOKEN_GAIN = 0.2f;
    constexpr float SET_BONUS_GAIN = 0.1f;

    // Main-spec needs of every bot per roll, so only the bot that gains most (after fairness) rolls need.
    // Bots of one group share a map thread, but different maps run in parallel: guarded by a mutex.
    struct RollClaims
    {
        uint32 firstSeenMs = 0;
        std::unordered_map<ObjectGuid, LootRollDecision> decisions;
    };

    struct LootBoard
    {
        std::mutex lock;
        std::unordered_map<ObjectGuid, RollClaims> rolls;
        std::unordered_map<ObjectGuid, std::deque<uint32>> needWins;
    };

    LootBoard& GetLootBoard()
    {
        static LootBoard board;
        return board;
    }

    uint32 CountRecentNeedWins(LootBoard& board, ObjectGuid botGuid, uint32 now)
    {
        auto const itr = board.needWins.find(botGuid);
        if (itr == board.needWins.end())
            return 0;

        std::deque<uint32>& wins = itr->second;
        while (!wins.empty() && getMSTimeDiff(wins.front(), now) > LOOT_FAIRNESS_WINDOW_MS)
            wins.pop_front();

        return wins.size();
    }

    // Returns the decision already published for this bot and roll, or publishes the one built by decide().
    template <class Decide>
    LootRollDecision GetOrPublishDecision(ObjectGuid rollGuid, ObjectGuid botGuid, uint32& firstSeenMs,
                                          Decide decide)
    {
        LootBoard& board = GetLootBoard();
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(board.lock);
            auto const roll = board.rolls.find(rollGuid);
            if (roll != board.rolls.end())
            {
                auto const decision = roll->second.decisions.find(botGuid);
                if (decision != roll->second.decisions.end())
                {
                    firstSeenMs = roll->second.firstSeenMs;
                    return decision->second;
                }
            }
        }

        // Item scoring stays outside the lock.
        LootRollDecision decision = decide();

        std::lock_guard<std::mutex> guard(board.lock);
        for (auto itr = board.rolls.begin(); itr != board.rolls.end();)
        {
            if (getMSTimeDiff(itr->second.firstSeenMs, now) > LOOT_CLAIM_EXPIRE_MS)
                itr = board.rolls.erase(itr);
            else
                ++itr;
        }

        if (decision.mainSpec)
            decision.priority /= 1.0f + LOOT_FAIRNESS_WEIGHT * CountRecentNeedWins(board, botGuid, now);

        RollClaims& claims = board.rolls[rollGuid];
        if (!claims.firstSeenMs)
            claims.firstSeenMs = now;

        claims.decisions[botGuid] = decision;
        firstSeenMs = claims.firstSeenMs;
        return decision;
    }

    // True when no other bot holds a stronger main-spec need on this roll (ties go to the lower guid).
    bool IsBestClaim(ObjectGuid rollGuid, ObjectGuid botGuid)
    {
        LootBoard& board = GetLootBoard();
        std::lock_guard<std::mutex> guard(board.lock);
        auto const roll = board.rolls.find(rollGuid);
        if (roll == board.rolls.end())
            return true;

        auto const own = roll->second.decisions.find(botGuid);
        if (own == roll->second.decisions.end())
            return true;

        for (auto const& [otherGuid, other] : roll->second.decisions)
        {
            if (otherGuid == botGuid || !other.mainSpec || other.vote != NEED)
                continue;

            if (other.priority > own->second.priority ||
                (other.priority == own->second.priority && otherGuid < botGuid))
                return false;
        }

        return true;
    }

    void RecordNeedWin(ObjectGuid botGuid)
    {
        LootBoard& board = GetLootBoard();
        std::lock_guard<std::mutex> guard(board.lock);
        board.needWins[botGuid].push_back(getMSTime());
    }

    bool IsArmorToken(ItemTemplate const* proto)
    {
        // Armor tokens are MISC JUNK (class 15, subclass 0); no other epic junk carries class bits.
        return proto->Class == ITEM_CLASS_MISC && proto->SubClass == ITEM_SUBCLASS_JUNK &&
               proto->Quality == ITEM_QUALITY_EPIC;
    }

    // The roll-level config has the last word (#2546: rolls used to bypass it on the packet path).
    RollVote ApplyRollConfig(RollVote vote, ItemTemplate const* proto, Player* bot)
    {
        if (vote == NEED)
        {
            if (sPlayerbotAIConfig.LootNeedRollLevel == 0 || RollUniqueCheck(proto, bot))
                return PASS;

            if (sPlayerbotAIConfig.LootNeedRollLevel == 1)
                return GREED;
        }
        else if (vote == GREED && !sPlayerbotAIConfig.LootGreedRollLevel)
            return PASS;

        return vote;
    }
}

bool LootRollAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    std::vector<Roll const*> rolls = group->GetRolls();
    bool voted = false;
    for (Roll const* roll : rolls)
    {
        auto voteItr = roll->playerVote.find(bot->GetGUID());
        if (voteItr == roll->playerVote.end() || voteItr->second != NOT_EMITED_YET)
            continue;

        ObjectGuid guid = roll->itemGUID;
        uint32 itemId = roll->itemid;
        int32 randomProperty = 0;
        if (roll->itemRandomPropId)
            randomProperty = roll->itemRandomPropId;
        else if (roll->itemRandomSuffix)
            randomProperty = -((int)roll->itemRandomSuffix);

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        uint32 firstSeenMs = 0;
        LootRollDecision const decision = GetOrPublishDecision(
            guid, bot->GetGUID(), firstSeenMs,
            [&]() { return DecideRoll(proto, randomProperty, roll->rollVoteMask); });

        // Wait until the other bots had the chance to publish their own claim on this roll.
        if (getMSTimeDiff(firstSeenMs, getMSTime()) < LOOT_CLAIM_DELAY_MS)
            continue;

        RollVote vote = decision.vote;
        if (vote == NEED && decision.mainSpec && !IsBestClaim(guid, bot->GetGUID()))
            vote = GREED;  // another bot gains more from it, or got less loot lately

        vote = ApplyRollConfig(vote, proto, bot);

        switch (group->GetLootMethod())
        {
            case MASTER_LOOT:
            case FREE_FOR_ALL:
                group->CountRollVote(bot->GetGUID(), guid, PASS);
                break;
            default:
                group->CountRollVote(bot->GetGUID(), guid, vote);
                break;
        }
        voted = true;
    }

    return voted;
}

LootRollDecision LootRollAction::DecideRoll(ItemTemplate const* proto, int32 randomProperty, uint8 voteMask)
{
    LootRollDecision decision;

    std::string itemUsageParam = std::to_string(proto->ItemId);
    if (randomProperty != 0)
        itemUsageParam += "," + std::to_string(randomProperty);

    ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", itemUsageParam);
    Map* map = bot->GetMap();
    bool const instance = map && map->IsDungeon();

    if (IsArmorToken(proto))
    {
        if (CanBotUseToken(proto, bot))
        {
            decision.vote = NEED;
            decision.mainSpec = true;
            decision.priority = TOKEN_GAIN;
        }
        else
            decision.vote = GREED;
    }
    else if (usage == ITEM_USAGE_DISENCHANT)
    {
        bool const canDisenchant = voteMask & ROLL_FLAG_TYPE_DISENCHANT;
        decision.vote = sPlayerbotAIConfig.LootRollDisenchant && canDisenchant ? DISENCHANT : GREED;
    }
    else
    {
        switch (proto->Class)
        {
            case ITEM_CLASS_WEAPON:
            case ITEM_CLASS_ARMOR:
                // EQUIP/REPLACE already passed the spec weights, the armor type and the upgrade threshold.
                if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                {
                    decision.vote = NEED;
                    decision.mainSpec = true;
                    decision.priority = CalculateUpgradeGain(proto, randomProperty);
                }
                else if (usage == ITEM_USAGE_BAD_EQUIP && CompletesSetBonus(proto))
                {
                    decision.vote = NEED;
                    decision.mainSpec = true;
                    decision.priority = CalculateUpgradeGain(proto, randomProperty) + SET_BONUS_GAIN;
                }
                // Off-spec, wrong armor type, a side-grade or a fix for a broken piece is greed, never need.
                else if (usage != ITEM_USAGE_NONE)
                    decision.vote = GREED;
                break;
            case ITEM_CLASS_RECIPE:
                if (!sPlayerbotAIConfig.LootRollRecipe)
                    decision.vote = PASS;
                else if (usage == ITEM_USAGE_SKILL)
                    decision.vote = NEED;  // Bot can learn this recipe
                else if (proto->Bonding != BIND_WHEN_PICKED_UP)
                    decision.vote = GREED;  // BoE recipe bot can't learn - GREED for AH/trade
                break;
            default:
                if (StoreLootAction::IsLootAllowed(proto->ItemId, botAI))
                    decision.vote = CalculateRollVote(proto, usage);
                break;
        }
    }

    // In dungeons and raids need is only for the bot's own spec.
    if (instance && decision.vote == NEED && !decision.mainSpec)
        decision.vote = GREED;

    if (decision.vote == NEED && !(voteMask & ROLL_FLAG_TYPE_NEED))
        decision.vote = GREED;

    return decision;
}

RollVote LootRollAction::CalculateRollVote(ItemTemplate const* proto, ItemUsage usage)
{
    RollVote needVote = PASS;
    switch (usage)
    {
        case ITEM_USAGE_EQUIP:
        case ITEM_USAGE_REPLACE:
        case ITEM_USAGE_GUILD_TASK:
            needVote = NEED;
            break;
        case ITEM_USAGE_BAD_EQUIP:  // equippable but not an upgrade for this spec
        case ITEM_USAGE_SKILL:
        case ITEM_USAGE_USE:
        case ITEM_USAGE_AH:
        case ITEM_USAGE_VENDOR:
            needVote = GREED;
            break;
        case ITEM_USAGE_DISENCHANT:
            needVote = sPlayerbotAIConfig.LootRollDisenchant ? DISENCHANT : GREED;
            break;
        default:
            break;
    }

    return StoreLootAction::IsLootAllowed(proto->ItemId, botAI) ? needVote : PASS;
}

float LootRollAction::CalculateUpgradeGain(ItemTemplate const* proto, int32 randomProperty)
{
    // Same scoring as "item usage", so the gain matches the decision that made the item an upgrade.
    StatsWeightCalculator calculator(bot);
    calculator.SetItemSetBonus(false);
    calculator.SetOverflowPenalty(false);
    if (sRandomPlayerbotMgr.IsSpecPvp(bot->GetGUID().GetCounter(), bot->getClass()))
        calculator.SetPvpSpec(true);

    float const newScore = calculator.CalculateItem(proto->ItemId, randomProperty);
    if (newScore <= 0.0f)
        return 0.0f;

    uint8 const slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
    if (slot >= EQUIPMENT_SLOT_END)
        return MAX_UPGRADE_GAIN;

    // Rings, trinkets and one-handers for a dual wielder replace the weaker of their two slots.
    uint8 secondSlot = NULL_SLOT;
    if (slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_TRINKET1)
        secondSlot = static_cast<uint8>(slot + 1);
    else if (slot == EQUIPMENT_SLOT_MAINHAND && bot->CanDualWield() && proto->InventoryType != INVTYPE_2HWEAPON)
        secondSlot = EQUIPMENT_SLOT_OFFHAND;

    float oldScore = -1.0f;
    for (uint8 candidate : {slot, secondSlot})
    {
        if (candidate == NULL_SLOT)
            continue;

        Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, candidate);
        if (!oldItem)
            return MAX_UPGRADE_GAIN;

        float const score = calculator.CalculateItem(oldItem->GetTemplate()->ItemId,
                                                     oldItem->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));
        if (oldScore < 0.0f || score < oldScore)
            oldScore = score;
    }

    if (oldScore <= 0.0f)
        return MAX_UPGRADE_GAIN;

    return std::clamp(newScore / oldScore - 1.0f, 0.0f, MAX_UPGRADE_GAIN);
}

bool LootRollAction::CompletesSetBonus(ItemTemplate const* proto)
{
    if (!proto->ItemSet)
        return false;

    // Tier pieces are class-restricted; a set piece for another class gives this bot nothing.
    uint32 const classMask = 1 << (bot->getClass() - 1);
    if (!(proto->AllowableClass & classMask))
        return false;

    ItemSetEntry const* itemSet = sItemSetStore.LookupEntry(proto->ItemSet);
    if (!itemSet)
        return false;

    uint32 equippedPieces = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || item->GetTemplate()->ItemSet != proto->ItemSet)
            continue;

        // replacing a piece of the same set (or the same piece) does not add to the count
        if (item->GetTemplate()->InventoryType == proto->InventoryType)
            return false;

        ++equippedPieces;
    }

    for (uint32 threshold : itemSet->items_to_triggerspell)
    {
        if (threshold && threshold == equippedPieces + 1)
            return true;
    }

    return false;
}

bool MasterLootRollAction::isUseful() { return !IsRealPlayer(botAI->GetMaster()); }

bool MasterLootRollAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());  // SMSG_LOOT_START_ROLL (8+4+4+4+4+4+4+4+1)
    if (p.size() < 8 + 4 * 7 + 1)
        return false;

    ObjectGuid rollGuid;
    uint32 mapId;
    uint32 itemSlot;
    uint32 itemId;
    uint32 randomSuffix;
    uint32 randomPropertyId;
    uint32 count;
    uint32 timeout;
    uint8 voteMask;

    p.rpos(0);              // reset packet pointer
    p >> rollGuid;          // guid of the rolled item
    p >> mapId;             /// 3.3.3 mapid
    p >> itemSlot;          // the itemEntryId for the item that shall be rolled for
    p >> itemId;            // the itemEntryId for the item that shall be rolled for
    p >> randomSuffix;      // randomSuffix
    p >> randomPropertyId;  // item random property ID
    p >> count;             // items in stack
    p >> timeout;           // the countdown time to choose "need" or "greed"
    p >> voteMask;          // allowed roll types

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto || !bot->GetGroup())
        return false;

    int32 randomProperty = 0;
    if (randomPropertyId)
        randomProperty = randomPropertyId;
    else if (randomSuffix)
        randomProperty = -static_cast<int32>(randomSuffix);

    uint32 firstSeenMs = 0;
    GetOrPublishDecision(rollGuid, bot->GetGUID(), firstSeenMs,
                         [&]() { return DecideRoll(proto, randomProperty, voteMask); });
    return true;
}

bool RecordLootWinAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());  // SMSG_LOOT_ROLL_WON (8+4+4+4+4+8+1+1)
    if (p.size() < 8 + 4 * 4 + 8 + 1 + 1)
        return false;

    ObjectGuid rollGuid;
    uint32 itemSlot;
    uint32 itemId;
    uint32 randomSuffix;
    uint32 randomPropertyId;
    ObjectGuid winnerGuid;
    uint8 rollNumber;
    uint8 rollType;

    p.rpos(0);
    p >> rollGuid >> itemSlot >> itemId >> randomSuffix >> randomPropertyId >> winnerGuid >> rollNumber >> rollType;

    if (winnerGuid != bot->GetGUID() || rollType != ROLL_NEED)
        return false;

    RecordNeedWin(bot->GetGUID());
    return true;
}

bool CanBotUseToken(ItemTemplate const* proto, Player* bot)
{
    // Get the bitmask for the bot's class
    uint32 botClassMask = (1 << (bot->getClass() - 1));

    // Check if the bot's class is allowed to use the token
    if (proto->AllowableClass & botClassMask)
        return true; // Bot's class is eligible to use this token

    return false; // Bot's class cannot use this token
}

bool RollUniqueCheck(ItemTemplate const* proto, Player* bot)
{
    // Count the total number of the item (equipped + in bags)
    uint32 totalItemCount = bot->GetItemCount(proto->ItemId, true);

    // Count the number of the item in bags only
    uint32 bagItemCount = bot->GetItemCount(proto->ItemId, false);

    // Determine if the unique item is already equipped
    bool isEquipped = (totalItemCount > bagItemCount);
    if (isEquipped && proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE))
        return true;  // Unique Item is already equipped
    else if (proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE) && (bagItemCount > 1))
        return true; // Unique item already in bag, don't roll for it
    return false; // Item is not equipped or in bags, roll for it
}

bool RollAction::Execute(Event event)
{
    std::string link = event.getParam();

    if (link.empty())
    {
        bot->DoRandomRoll(1, 100);
        return false;
    }
    ItemIds itemIds = chat->parseItems(link);
    if (itemIds.empty())
        return false;
    uint32 itemId = *itemIds.begin();
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return false;

    std::string itemUsageParam;
    itemUsageParam = std::to_string(itemId);

    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemUsageParam);
    switch (proto->Class)
    {
        case ITEM_CLASS_WEAPON:
        case ITEM_CLASS_ARMOR:
        // a /roll for gear is a need claim: only for this spec, never for an off-spec piece
        if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
            bot->DoRandomRoll(1, 100);
    }
    return true;
}
