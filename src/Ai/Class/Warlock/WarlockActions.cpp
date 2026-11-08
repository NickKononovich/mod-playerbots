/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WarlockActions.h"
#include "EstimatedLifetimeValue.h"  // [rot350]
#include "Event.h"
#include "GuildHonest.h"
#include "Item.h"
#include "KillOrder.h"  // [b44_hos]
#include "Log.h"  // [lead1310]
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"
#include "RtiValue.h"     // [b44_hos]
#include "ServerFacade.h"
#include "SpellAuras.h"  // [rot350]
#include "SpellInfo.h"   // [rot481] the aura's radius
#include "SpellMgr.h"    // [rot350]
#include "Strategy.h"     // [b44_hos] TargetValueExclusionType
#include "TargetValue.h"  // [b44_hos]
#include "Timer.h"
#include "Unit.h"
#include <algorithm>  // [rot481] std::max
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

const int ITEM_SOUL_SHARD = 6265;

// [b44_hos]
Unit* CastSeedOfCorruptionOnAttackerAction::GetTarget()
{
    Unit* pick = CastDebuffSpellOnAttackerAction::GetTarget();
    if (!botAI->HasStrategy("affli", BOT_STATE_COMBAT) || !pick || !botAI->HasAura("corruption", pick, false, true))
        return pick;

    GuidSet const exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::DebuffTarget);
    float const range = botAI->GetRange("spell");
    Unit* best = nullptr;
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || exclusions.count(guid) || IsCrowdControlled(unit) ||
            HasCrowdControlIcon(bot, unit) || !bot->IsWithinCombatRange(unit, range) ||
            !KillOrder::IsInGroupFight(bot, unit) || botAI->HasAura("seed of corruption", unit, false, true) ||
            botAI->HasAura("corruption", unit, false, true))
            continue;

        if (!best || unit->GetHealth() > best->GetHealth())
            best = unit;
    }
    return best;
}

// [b44_hos] The rule and its numbers: WarlockActions.h, above CastSeedOfCorruptionAction.
bool AfflictionSeedWaits(PlayerbotAI* botAI, Unit* seedTarget)
{
    if (!seedTarget || !botAI->HasStrategy("affli", BOT_STATE_COMBAT))
        return false;

    // the Seed would replace this warlock's Corruption on that unit
    if (botAI->HasAura("corruption", seedTarget, false, true))
        return true;

    Unit* main = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!main || !main->IsAlive() || !main->IsInWorld())
        return false;

    // Haunt first while it is ready and not on the target
    if (botAI->HasSpell("haunt") && !botAI->HasAura("haunt", main, false, true) && botAI->CanCastSpell("haunt", main))
        return true;

    // then Corruption on a target that lives long enough for it ("corruption", CORRUPTION_MIN_TARGET_LIFETIME); a
    // Seed of this warlock already on it stands (the two are exclusive)
    float const groupDps = botAI->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get();
    bool const lives = groupDps > 0.0f && main->GetHealth() / groupDps >= CORRUPTION_MIN_TARGET_LIFETIME;
    return lives && !botAI->HasAura("corruption", main, false, true) &&
           !botAI->HasAura("seed of corruption", main, false, true) && botAI->CanCastSpell("corruption", main);
}

// [rot350] wowsims affliction APL row 15: own Haunt debuff left < Haunt cast time + travel time + 2 s
int32 HauntRefreshInMs(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    uint32 const hauntId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "haunt")->Get();
    SpellInfo const* haunt = hauntId ? sSpellMgr->GetSpellInfo(hauntId) : nullptr;
    if (!haunt || !target)
        return INT32_MAX;

    int32 const castMs = int32(haunt->CalcCastTime(bot));
    int32 const travelMs = haunt->Speed > 0.0f ? int32(bot->GetDistance(target) / haunt->Speed * IN_MILLISECONDS) : 0;
    Aura* debuff = botAI->GetAura("haunt", target, true, false);
    int32 const left = debuff ? debuff->GetDuration() : 0;
    return left - castMs - travelMs - int32(HAUNT_REFRESH_SLACK_MS);
}

bool HauntDue(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    if (!target || !target->IsAlive() || HauntRefreshInMs(botAI, bot, target) >= 0)
        return false;

    return ExpectedLifetime(botAI, target) > HAUNT_MIN_FIGHT_LEFT;
}

bool CastHauntAction::isUseful()
{
    return CastSpellAction::isUseful() && HauntDue(botAI, bot, GetTarget());
}

// [rot352] the APL's row 18: Unstable Affliction's hasted cast time
uint32 UnstableAfflictionRefreshMs(PlayerbotAI* botAI, Player* bot)
{
    uint32 const id = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "unstable affliction")->Get();
    SpellInfo const* ua = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
    return ua ? ua->CalcCastTime(bot) : UNSTABLE_AFFLICTION_REFRESH_MS;
}

bool CastUnstableAfflictionAction::isUseful()
{
    beforeDuration = UnstableAfflictionRefreshMs(botAI, bot);
    return CastDebuffSpellAction::isUseful();
}

// [rot453]
uint32 ImmolateRefreshMs(PlayerbotAI* botAI, Player* bot)
{
    uint32 const id = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "immolate")->Get();
    SpellInfo const* immolate = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
    return immolate ? immolate->CalcCastTime(bot) : IMMOLATE_REFRESH_MS;
}

bool CastImmolateAction::isUseful()
{
    beforeDuration = ImmolateRefreshMs(botAI, bot);
    return CastDebuffSpellAction::isUseful();
}

// Checks if the bot has less than 26 soul shards, and if so, allows casting Drain Soul
bool CastDrainSoulAction::isUseful()
{
    // In the execute range Drain Soul is the damage filler, whatever the soul shard count
    Unit* target = GetTarget();
    if (target && target->IsAlive() && !target->HealthAbovePct(DRAIN_SOUL_EXECUTE_HEALTH_PCT))
        return true;

    return AI_VALUE2(uint32, "item count", "soul shard") < 26;
}

bool LifeTapRefusedByAttackers(Player* bot, bool& refusedLogged)
{
    size_t const attackers = bot ? bot->getAttackers().size() : 0;
    bool const refused = attackers > 0;
    if (refused && !refusedLogged)
        LOG_INFO("playerbots", "[lead1310] {}: life_tap_refused: {} mobs attacking", bot->GetName(), attackers);
    refusedLogged = refused;
    return refused;
}

// Checks if the bot's health is above a certain threshold and no mob attacks it ([lead1310]), and if so, allows
// casting Life Tap
bool CastLifeTapAction::isUseful()
{
    if (AI_VALUE2(uint8, "health", "self target") <= sPlayerbotAIConfig.LowHealth)
        return false;
    return !LifeTapRefusedByAttackers(bot, refusedLogged);
}

Value<Unit*>* CastBanishOnCcAction::GetTargetValue() { return context->GetValue<Unit*>("rti cc target"); }
Value<Unit*>* CastFearOnCcAction::GetTargetValue() { return context->GetValue<Unit*>("rti cc target"); }

// Checks if the target marked with the moon icon can be banished
bool CastBanishOnCcAction::isPossible()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // Only possible on elementals or demons
    uint32 creatureType = target->GetCreatureType();
    if (creatureType != CREATURE_TYPE_DEMON && creatureType != CREATURE_TYPE_ELEMENTAL)
        return false;

    // Use base class to check spell available, range, etc
    return CastCrowdControlSpellAction::isPossible();
}

// Checks if the target marked with the moon icon can be feared
bool CastFearOnCcAction::isPossible()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // Fear cannot be cast on mechanical or undead creatures
    uint32 creatureType = target->GetCreatureType();
    if (creatureType == CREATURE_TYPE_MECHANICAL || creatureType == CREATURE_TYPE_UNDEAD)
        return false;

    // Use base class to check spell available, range, etc
    return CastCrowdControlSpellAction::isPossible();
}

// Checks if the enemies are close enough to use Shadowflame
bool CastShadowflameAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;
    bool facingTarget = AI_VALUE2(bool, "facing", "current target");
    bool targetClose = bot->IsWithinCombatRange(target, 7.0f);  // 7 yard cone
    return facingTarget && targetClose;
}

// Checks if the bot knows Seed of Corruption, and prevents the use of Rain of Fire if it does
bool CastRainOfFireAction::isUseful()
{
    Unit* target = GetTarget();
    return target && !botAI->HasSpell("seed of corruption");
}

// Checks if the enemies are close enough to use Hellfire
bool CastHellfireAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    return bot->IsWithinCombatRange(target, 5.0f);  // 5 yard AoE radius
}

// [rot481] see ImmolationAuraReaches (WarlockActions.h)
bool ImmolationAuraReaches(Player* bot, Unit* target)
{
    constexpr uint32 SPELL_METAMORPHOSIS_FORM = 47241;
    constexpr uint32 SPELL_IMMOLATION_AURA = 50589;
    if (!bot || !target || !target->IsAlive() || !bot->HasAura(SPELL_METAMORPHOSIS_FORM))
        return false;

    SpellInfo const* aura = sSpellMgr->GetSpellInfo(SPELL_IMMOLATION_AURA);
    if (!aura)
        return false;

    float radius = 0.0f;
    for (SpellEffectInfo const& effect : aura->GetEffects())
    {
        if (effect.HasRadius())
            radius = std::max(radius, effect.CalcRadius(bot));
        if (effect.ApplyAuraName != SPELL_AURA_PERIODIC_TRIGGER_SPELL || !effect.TriggerSpell)
            continue;
        if (SpellInfo const* tick = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
            for (SpellEffectInfo const& tickEffect : tick->GetEffects())
                if (tickEffect.HasRadius())
                    radius = std::max(radius, tickEffect.CalcRadius(bot));
    }
    return radius > 0.0f && bot->IsWithinDist(target, radius);
}

// [rot481] Rawr 2.3.23 Rawr.Warlock (the Demonology rotation's Immolation Aura, Spells.cs ImmolationAura) and the
// wowsims demo APL (castSpell 50589 right after the cooldowns) burn it in Metamorphosis on the target, so it is cast
// only where its area reaches the target. It used to pass on the "meta melee" strategy alone, at any range
bool CastImmolationAuraAction::isUseful()
{
    return ImmolationAuraReaches(bot, AI_VALUE(Unit*, "current target"));
}

// Checks if the "warlock tank" strategy is active, and if so, prevents the use of Soulshatter
bool CastSoulshatterAction::isUseful()
{
    if (botAI->HasStrategy("tank", BOT_STATE_COMBAT))
        return false;
    Unit* target = AI_VALUE(Unit*, "current target");
    return !(target && target->IsPlayer());  // players have no threat
}

// Checks if the bot has enough bag space to create a soul shard, then does so
bool CreateSoulShardAction::Execute(Event /*event*/)
{
    Player* bot = botAI->GetBot();
    if (!bot)
        return false;

    ItemPosCountVec dest;
    uint32 count = 1;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ITEM_SOUL_SHARD, count) == EQUIP_ERR_OK)
    {
        bot->StoreNewItem(dest, ITEM_SOUL_SHARD, true, Item::GenerateItemRandomPropertyId(ITEM_SOUL_SHARD));
        SQLTransaction<CharacterDatabaseConnection> trans = CharacterDatabase.BeginTransaction();
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        return true;
    }
    return false;
}

// Checks if the bot has less than 6 soul shards, allowing the creation of a new one
bool CreateSoulShardAction::isUseful()
{
    Player* bot = botAI->GetBot();
    if (!bot)
        return false;
    // [honest] a soul shard out of nothing: a guild-plan warlock gets its shards from Drain Soul
    if (GuildHonest::IsHonestBot(bot))
        return false;

    uint32 currentShards = bot->GetItemCount(ITEM_SOUL_SHARD, false);  // false = only bags
    const uint32 SHARD_CAP = 6;                                    // adjust as needed

    // Only allow if under cap AND there is space for a new shard
    ItemPosCountVec dest;
    uint32 count = 1;
    bool hasSpace = (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ITEM_SOUL_SHARD, count) == EQUIP_ERR_OK);

    return (currentShards < SHARD_CAP) && hasSpace;
}

bool CastCreateSoulstoneAction::isUseful()
{
    Player* bot = botAI->GetBot();
    if (!bot)
        return false;

    // List of all Soulstone item IDs
    static const std::vector<uint32> soulstoneIds = {
        5232,   // Minor Soulstone
        16892,  // Lesser Soulstone
        16893,  // Soulstone
        16895,  // Greater Soulstone
        16896,  // Major Soulstone
        22116,  // Master Soulstone
        36895   // Demonic Soulstone
    };

    // Check if the bot already has any soulstone
    for (uint32 id : soulstoneIds)
    {
        if (bot->GetItemCount(id, false) > 0)
            return false;                      // Already has a soulstone
    }

    // Only need to check one soulstone type for bag space (usually the highest-tier)
    ItemPosCountVec dest;
    uint32 count = 1;
    // Use the last in the list (highest tier)
    uint32 soulstoneToCreate = soulstoneIds.back();

    bool hasSpace = (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, soulstoneToCreate, count) == EQUIP_ERR_OK);

    return hasSpace;
}

bool DestroySoulShardAction::Execute(Event /*event*/)
{
    // Look for the first soul shard in any bag and destroy it
    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    if (pItem->GetTemplate()->ItemId == ITEM_SOUL_SHARD)
                    {
                        bot->DestroyItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
                        return true;  // Only destroy one!
                    }
                }
            }
        }
    }
    // Also check main inventory slots (not in bags)
    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetTemplate()->ItemId == ITEM_SOUL_SHARD)
            {
                bot->DestroyItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
                return true;
            }
        }
    }
    return false;
}

// Checks if the target has a soulstone aura
static bool HasSoulstoneAura(Unit* unit)
{
    static const std::vector<uint32> soulstoneAuraIds = {20707, 20762, 20763, 20764, 20765, 27239, 47883};
    for (uint32 spellId : soulstoneAuraIds)
        if (unit->HasAura(spellId))
            return true;
    return false;
}

// Use the soulstone item on the bot itself with nc strategy "ss self"
bool UseSoulstoneSelfAction::Execute(Event /*event*/)
{
    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    if (HasSoulstoneAura(bot))
        return false;

    bot->SetSelection(bot->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, bot);
}

// Reservation map for soulstone targets (GUID -> reservation expiry in ms)
static std::unordered_map<ObjectGuid, uint32> soulstoneReservations;
static std::mutex soulstoneReservationsMutex;

// Helper to clean up expired reservations
void CleanupSoulstoneReservations()
{
    uint32 now = getMSTime();
    std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
    for (auto it = soulstoneReservations.begin(); it != soulstoneReservations.end();)
    {
        if (it->second <= now)
            it = soulstoneReservations.erase(it);
        else
            ++it;
    }
}

// Use the soulstone item on the bot's master with nc strategy "ss master"
bool UseSoulstoneMasterAction::Execute(Event /*event*/)
{
    CleanupSoulstoneReservations();

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    Player* master = botAI->GetMaster();
    if (!master || HasSoulstoneAura(master))
        return false;

    uint32 now = getMSTime();

    {
        std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
        if (soulstoneReservations.count(master->GetGUID()) && soulstoneReservations[master->GetGUID()] > now)
            return false;  // Already being soulstoned

        soulstoneReservations[master->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
    }

    float distance = ServerFacade::instance().GetDistance2d(bot, master);
    if (distance >= 30.0f)
        return false;

    if (!bot->IsWithinLOSInMap(master))
        return false;

    bot->SetSelection(master->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, master);
}

// Use the soulstone item on a tank in the group with nc strategy "ss tank"
bool UseSoulstoneTankAction::Execute(Event /*event*/)
{
    CleanupSoulstoneReservations();

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    Player* chosenTank = nullptr;
    Group* group = bot->GetGroup();
    uint32 now = getMSTime();

    // First: Try to soulstone the main tank
    if (group)
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (member && member->IsAlive() && PlayerbotAI::IsTank(member) && PlayerbotAI::IsMainTank(member) &&
                !HasSoulstoneAura(member))
            {
                std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
                if (soulstoneReservations.count(member->GetGUID()) && soulstoneReservations[member->GetGUID()] > now)
                    continue;  // Already being soulstoned

                float distance = ServerFacade::instance().GetDistance2d(bot, member);
                if (distance < 30.0f && bot->IsWithinLOSInMap(member))
                {
                    chosenTank = member;
                    soulstoneReservations[chosenTank->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
                    break;
                }
            }
        }

        // If no main tank found, soulstone another tank
        if (!chosenTank)
        {
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->GetSource();
                if (member && member->IsAlive() && PlayerbotAI::IsTank(member) && !HasSoulstoneAura(member))
                {
                    std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
                    if (soulstoneReservations.count(member->GetGUID()) &&
                        soulstoneReservations[member->GetGUID()] > now)
                        continue;  // Already being soulstoned

                    float distance = ServerFacade::instance().GetDistance2d(bot, member);
                    if (distance < 30.0f && bot->IsWithinLOSInMap(member))
                    {
                        chosenTank = member;
                        soulstoneReservations[chosenTank->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
                        break;
                    }
                }
            }
        }
    }

    if (!chosenTank)
        return false;

    bot->SetSelection(chosenTank->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, chosenTank);
}

// Use the soulstone item on a healer in the group with nc strategy "ss healer"
bool UseSoulstoneHealerAction::Execute(Event /*event*/)
{
    CleanupSoulstoneReservations();

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    Player* healer = nullptr;
    Group* group = bot->GetGroup();
    uint32 now = getMSTime();
    if (group)
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (member && member->IsAlive() && PlayerbotAI::IsHeal(member) && !HasSoulstoneAura(member))
            {
                {
                    std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
                    if (soulstoneReservations.count(member->GetGUID()) &&
                        soulstoneReservations[member->GetGUID()] > now)
                        continue;  // Already being soulstoned

                    float distance = ServerFacade::instance().GetDistance2d(bot, member);
                    if (distance < 30.0f && bot->IsWithinLOSInMap(member))
                    {
                        healer = member;
                        soulstoneReservations[healer->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
                        break;
                    }
                }
            }
        }
    }

    if (!healer)
        return false;

    bot->SetSelection(healer->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, healer);
}

// Use the soulstone item with nc strategy "ss auto" (default): a healer who can resurrect, else the main tank
bool UseSoulstoneAutoAction::Execute(Event /*event*/)
{
    CleanupSoulstoneReservations();

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    Group* group = bot->GetGroup();
    if (!group)
    {
        if (HasSoulstoneAura(bot))
            return false;

        bot->SetSelection(bot->GetGUID());
        return UseItem(items[0], ObjectGuid::Empty, nullptr, bot);
    }

    uint32 const now = getMSTime();
    Player* resHealer = nullptr;
    Player* mainTank = nullptr;
    bool preferredInGroup = false;
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* member = gref->GetSource();
        if (!member || member->GetMap() != bot->GetMap())
            continue;

        // Resurrection, Redemption, Ancestral Spirit, Revive (druids, level 12) or Rebirth, by the spells known
        bool const canResurrect = RaidLeader::CanResurrect(member);
        bool const isResHealer = canResurrect && PlayerbotAI::IsHeal(member);
        bool const isMainTank = PlayerbotAI::IsMainTank(member);
        preferredInGroup = preferredInGroup || isResHealer || isMainTank;

        if (!member->IsAlive() || !member->IsInWorld() || HasSoulstoneAura(member) ||
            ((!isResHealer || resHealer) && (!isMainTank || mainTank)))
            continue;

        {
            std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
            auto const reservation = soulstoneReservations.find(member->GetGUID());
            if (reservation != soulstoneReservations.end() && reservation->second > now)
                continue;  // Already being soulstoned
        }

        if (ServerFacade::instance().GetDistance2d(bot, member) >= 30.0f || !bot->IsWithinLOSInMap(member))
            continue;

        if (isResHealer && !resHealer)
            resHealer = member;
        else if (isMainTank && !mainTank)
            mainTank = member;
    }

    Player* target = resHealer ? resHealer : mainTank;
    if (!target && !preferredInGroup && !HasSoulstoneAura(bot))
        target = bot;  // nobody in the group can use it better
    if (!target)
        return false;

    {
        std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
        soulstoneReservations[target->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
    }

    bot->SetSelection(target->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, target);
}

const std::vector<uint32> CastCreateFirestoneAction::firestoneSpellIds = {
    60220,  // Create Firestone (Rank 7)
    27250,  // Rank 5
    17953,  // Rank 4
    17952,  // Rank 3
    17951,  // Rank 2
    6366    // Rank 1
};

CastCreateFirestoneAction::CastCreateFirestoneAction(PlayerbotAI* botAI)
    : CastBuffSpellAction(botAI, "create firestone")
{
}

bool CastCreateFirestoneAction::Execute(Event /*event*/)
{
    for (uint32 spellId : firestoneSpellIds)
    {
        if (bot->HasSpell(spellId))
            return botAI->CastSpell(spellId, bot);
    }
    return false;
}

bool CastCreateFirestoneAction::isUseful()
{
    for (uint32 spellId : firestoneSpellIds)
    {
        if (bot->HasSpell(spellId))
            return true;
    }
    return false;
}
