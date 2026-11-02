/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderPrepull.h"

#include <algorithm>

#include "AiFactory.h"
#include "Creature.h"
#include "DBCStores.h"
#include "HealthTriggers.h"  // RestState
#include "LeaderFightChecks.h"
#include "LeaderRest.h"  // CountRestItems
#include "LeaderOrders.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"

namespace Leader::Prepull
{
namespace
{
using Leader::Fight::SPELL_ARMY_OF_THE_DEAD;

// a death knight that casts Army before a pull in wowsims: Blood (tank and dps) and Frost, not Unholy
bool ArmyCaster(Player* bot)
{
    return bot && bot->IsAlive() && bot->getClass() == CLASS_DEATH_KNIGHT && bot->HasSpell(SPELL_ARMY_OF_THE_DEAD) &&
           AiFactory::GetPlayerSpecTab(bot) != DEATH_KNIGHT_TAB_UNHOLY;
}

Spell* ArmyChannel(Player* dk)
{
    Spell* channel = dk->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    return channel && channel->GetSpellInfo() && channel->GetSpellInfo()->Id == SPELL_ARMY_OF_THE_DEAD ? channel
                                                                                                       : nullptr;
}

// the channel's length (spell data)
float ChannelSec()
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_ARMY_OF_THE_DEAD);
    return info ? std::max(info->GetDuration(), 0) / 1000.0f : 0.0f;
}

// the DK can start the channel now: out of combat, standing, the cooldown off, the spell's runes ready
// (SpellRuneCost.dbc by RuneType, as Spell::CheckRuneCost reads it; a Death rune stands in for any); empty: yes
std::string ArmyBlocked(Player* dk)
{
    if (!dk->IsAlive())
        return "dead";
    if (dk->IsInCombat())
        return "in combat";
    if (dk->HasSpellCooldown(SPELL_ARMY_OF_THE_DEAD))
        return "on cooldown";
    if (dk->isMoving())
        return "walking";
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_ARMY_OF_THE_DEAD);
    SpellRuneCostEntry const* cost = info ? sSpellRuneCostStore.LookupEntry(info->RuneCostID) : nullptr;
    if (!cost)
        return std::string();
    uint32 ready[NUM_RUNE_TYPES] = {};
    for (uint8 i = 0; i < MAX_RUNES; ++i)
        if (!dk->GetRuneCooldown(i))
            ++ready[dk->GetCurrentRune(i)];
    uint32 death = ready[RUNE_DEATH];
    for (uint8 i = 0; i < RUNE_DEATH; ++i)
    {
        uint32 const have = ready[i];
        if (have >= cost->RuneCost[i])
            continue;
        uint32 const missing = cost->RuneCost[i] - have;
        if (death < missing)
            return "its runes are not ready";
        death -= missing;
    }
    return std::string();
}

// the boss whose pull the group prepares for: the route's boss target
Creature* PreparedBoss(LeaderGroup& group, Map* map)
{
    ObjectGuid const guid = group.RouteTargetIsBoss() ? group.RouteTarget() : ObjectGuid::Empty;
    Creature* boss = guid.IsEmpty() || !map ? nullptr : map->GetCreature(guid);
    if (!boss || !boss->IsAlive() || boss->IsInCombat() || !(boss->IsDungeonBoss() || boss->isWorldBoss()))
        return nullptr;
    return boss;
}

// the member's mana regen per second now (the game's own: UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER, the drink's
// SPELL_AURA_MOD_POWER_REGEN included, times the server's mana rate)
float ManaPerSec(Player* bot)
{
    return bot->GetFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER + AsUnderlyingType(POWER_MANA)) *
           sWorld->getRate(RATE_POWER_MANA);
}

// the member's food heal per second (its SPELL_AURA_OBS_MOD_HEALTH auras: amount per amplitude)
float FoodHealthPerSec(Player* bot)
{
    float perSec = 0.0f;
    for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_OBS_MOD_HEALTH))
        if (effect->GetAmplitude() > 0)
            perSec += effect->GetAmount() * 1000.0f / effect->GetAmplitude();
    return perSec;
}

// ms until an eater's food gives Well Fed: the food aura's periodic trigger; -1: no such timer
int32 WellFedMs(Player* bot)
{
    int32 left = -1;
    for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL))
        if (effect->GetBase() && effect->GetBase()->GetSpellInfo()->HasAura(SPELL_AURA_OBS_MOD_HEALTH))
            left = std::max(left, effect->GetPeriodicTimer());
    return left;
}

// The boss preparation still ahead, seconds, from the members' facts; -1: not computable yet (the group is
// not in the preparation, the buff pass still gives buffs, a member that must rest has not sat down)
float PrepLeftSec(LeaderGroup& group, Creature* boss, std::vector<Player*> const& members, std::string& why)
{
    GroupRouteState const& route = group.Route();
    // the buff pass is the boss's preparation: the group stands at the boss's buff distance, the pull follows it
    if (!route.needBuff || !route.buffStep || route.target != boss->GetGUID())
    {
        why = "the group is not in the boss's preparation";
        return -1.0f;
    }
    if (route.buffStep < 3)
    {
        why = "the buff pass gives its buffs";
        return -1.0f;
    }
    float left = 0.0f;
    why = "nothing left";
    auto const note = [&](float sec, std::string const& what)
    {
        if (sec > left)
        {
            left = sec;
            why = what;
        }
    };
    for (Player* bot : members)
    {
        if (!bot->IsAlive())
            continue;
        // the buff pass's food step: an eater without Well Fed waits for its food's trigger
        if (route.buffStep == 3 && RestState::IsEating(bot))
        {
            int32 const fed = WellFedMs(bot);
            if (fed < 0)
            {
                why = bot->GetName() + " eats (no Well Fed timer)";
                return -1.0f;
            }
            note(fed / 1000.0f, bot->GetName() + " until Well Fed");
        }
        // the pass's ready step waits for a member below its own rest line that can rest (TickBuffing's rule)
        bool const canRest = bot->IsSitState() || RestState::CanRest(bot);
        float const manaLine = RestState::OwnManaLine(bot);
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        float const mana = maxMana ? bot->GetPowerPct(POWER_MANA) : 100.0f;
        if (canRest && RestState::WantsDrink(bot) && Leader::Dungeon::CountRestItems(bot, 59) > 0 && maxMana &&
            mana < manaLine)
        {
            float const perSec = ManaPerSec(bot);
            if (!RestState::IsDrinking(bot) || perSec <= 0.0f)
            {
                why = bot->GetName() + " must drink and does not yet";
                return -1.0f;
            }
            note((manaLine - mana) * maxMana / 100.0f / perSec, bot->GetName() + " drinks to its line");
        }
        float const healthLine = RestState::HealthLine(bot);
        if (canRest && RestState::WantsFood(bot) && Leader::Dungeon::CountRestItems(bot, 11) > 0 &&
            bot->GetHealthPct() < healthLine)
        {
            float const perSec = FoodHealthPerSec(bot);
            if (!RestState::IsEating(bot) || perSec <= 0.0f)
            {
                why = bot->GetName() + " must eat and does not yet";
                return -1.0f;
            }
            note((healthLine - bot->GetHealthPct()) * bot->GetMaxHealth() / 100.0f / perSec,
                 bot->GetName() + " eats to its line");
        }
    }
    return left;
}

std::vector<Player*> GroupPlayers(LeaderGroup& group, Map* map)
{
    std::vector<Player*> players;
    for (ObjectGuid const& guid : group.MemberGuids())
        if (Player* bot = ObjectAccessor::FindPlayer(guid); bot && bot->GetMap() == map)
            players.push_back(bot);
    return players;
}

float Sec(uint32 fromMs, uint32 toMs) { return int32(getMSTimeDiff(fromMs, toMs)) / 1000.0f; }
}  // namespace

void TickArmy(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, bool fighting,
              std::function<void(Player*, char const*, JsonObject)> const& logEvent)
{
    auto& plans = group.State().armyPrepull;
    uint32 const now = getMSTime() ? getMSTime() : 1;
    Map* map = tank->GetMap();
    Creature* boss = fighting ? nullptr : PreparedBoss(group, map);
    // an order given stands for its boss while that one is still unpulled (the route may look elsewhere)
    for (auto const& [dkGuid, told] : plans)
        if (!boss && !fighting && told.orderedMs && !told.done && !told.startedMs)
            if (Creature* toldBoss = map->GetCreature(told.boss);
                toldBoss && toldBoss->IsAlive() && !toldBoss->IsInCombat())
                boss = toldBoss;
    std::vector<Player*> members = seats;
    members.push_back(tank);

    // the channel's course: started, ended (the added delay: its end after the preparation's)
    for (Player* dk : members)
    {
        auto found = plans.find(dk->GetGUID());
        if (found == plans.end() || found->second.done || !found->second.orderedMs)
            continue;
        DungeonLeaderState::ArmyPrepull& plan = found->second;
        bool const channeling = ArmyChannel(dk) != nullptr;
        if (channeling && !plan.startedMs)
        {
            plan.startedMs = now;
            group.LogEvent("army_prepull_started", JsonObject()
                                                       .Str("dk", dk->GetName())
                                                       .Real("afterOrderSec", Sec(plan.orderedMs, now))
                                                       .Real("channelSec", ChannelSec()));
        }
        else if (!channeling && plan.startedMs)
        {
            plan.done = true;
            group.LogEvent("army_prepull_end",
                           JsonObject()
                               .Str("dk", dk->GetName())
                               .Real("channelSec", Sec(plan.startedMs, now))
                               .Real("predictedPrepEndSec", Sec(plan.orderedMs, plan.predictedEndMs))
                               .Real("prepEndSec", plan.prepEndMs ? Sec(plan.orderedMs, plan.prepEndMs) : -1.0f)
                               .Real("addedDelaySec", plan.prepEndMs ? Sec(plan.prepEndMs, now) : 0.0f)
                               .Bool("prepStillRuns", !plan.prepEndMs));
        }
        else if (!channeling && !boss)
        {
            plan.done = true;
            group.LogEvent("army_prepull_dropped",
                           JsonObject()
                               .Str("dk", dk->GetName())
                               .Str("why", fighting ? "the fight started before the channel" : "no boss pull next"));
        }
    }
    if (!boss)
        return;

    float const channelSec = ChannelSec();
    std::vector<Player*> const players = GroupPlayers(group, map);
    for (Player* dk : members)
    {
        if (!ArmyCaster(dk))
            continue;
        DungeonLeaderState::ArmyPrepull& plan = plans[dk->GetGUID()];
        if (plan.boss != boss->GetGUID())
            plan = DungeonLeaderState::ArmyPrepull{boss->GetGUID()};
        if (plan.done || plan.startedMs)
            continue;
        std::string const blocked = ArmyBlocked(dk);
        if (!plan.orderedMs)
        {
            if (!blocked.empty())
                continue;
            std::string why;
            float const left = PrepLeftSec(group, boss, players, why);
            if (left < 0.0f || left > channelSec)
                continue;
            plan.orderedMs = now;
            plan.orderedRemainSec = left;
            plan.predictedEndMs = now + uint32(left * IN_MILLISECONDS);
            group.LogEvent("army_prepull_ordered", JsonObject()
                                                       .Str("boss", boss->GetName())
                                                       .Str("dk", dk->GetName())
                                                       .Real("prepLeftSec", left)
                                                       .Str("prepLeftWhy", why)
                                                       .Real("channelSec", channelSec)
                                                       .Real("plannedDelaySec", channelSec - left));
        }
        else if (!blocked.empty() && blocked != "walking")
        {
            plan.done = true;
            group.LogEvent("army_prepull_dropped", JsonObject().Str("dk", dk->GetName()).Str("why", blocked));
            continue;
        }
        // the order stands until the channel runs (each tick asserts it; a buff order of the same tick keeps
        // the member's one external slot)
        Leader::MemberEvents events;
        if (Leader::Events::Get(dk->GetGUID(), events) && events.externalSpell &&
            events.externalSpell != SPELL_ARMY_OF_THE_DEAD)
            continue;
        if (Leader::Events::SetExternal(dk->GetGUID(), SPELL_ARMY_OF_THE_DEAD, dk->GetGUID()))
            logEvent(dk, "External",
                     JsonObject()
                         .Num("spell", SPELL_ARMY_OF_THE_DEAD)
                         .Str("on", dk->GetName())
                         .Str("why", "Army of the Dead before the boss pull"));
    }
}

std::string ArmyHolds(LeaderGroup& group, Unit* boss, bool orderNow)
{
    if (!boss)
        return std::string();
    auto& plans = group.State().armyPrepull;
    uint32 const now = getMSTime() ? getMSTime() : 1;
    std::string holds;
    for (Player* dk : GroupPlayers(group, boss->GetMap()))
    {
        if (!ArmyCaster(dk))
            continue;
        DungeonLeaderState::ArmyPrepull& plan = plans[dk->GetGUID()];
        if (plan.boss != boss->GetGUID())
            plan = DungeonLeaderState::ArmyPrepull{boss->GetGUID()};
        if (plan.done)
            continue;
        Spell* channel = ArmyChannel(dk);
        if (!plan.orderedMs)
        {
            // the preparation ended before the leader could time the channel into it: the pull waits it whole
            if (!orderNow || !ArmyBlocked(dk).empty())
                continue;
            plan.orderedMs = now;
            plan.predictedEndMs = now;
            group.LogEvent("army_prepull_ordered", JsonObject()
                                                       .Str("boss", boss->GetName())
                                                       .Str("dk", dk->GetName())
                                                       .Real("prepLeftSec", 0.0f)
                                                       .Str("prepLeftWhy", "the preparation is over")
                                                       .Real("channelSec", ChannelSec())
                                                       .Real("plannedDelaySec", ChannelSec()));
        }
        // told, cannot start now (TickArmy drops it on a lasting reason): nothing to wait for
        else if (!channel && !plan.startedMs && !ArmyBlocked(dk).empty())
            continue;
        if (!plan.prepEndMs)
        {
            plan.prepEndMs = now;
            group.LogEvent(
                "army_prepull_prep_end",
                JsonObject()
                    .Str("dk", dk->GetName())
                    .Str("at", orderNow ? "the buff pass end" : "the pull")
                    .Real("orderedBeforeSec", Sec(plan.orderedMs, now))
                    .Real("predictedPrepEndSec", Sec(plan.orderedMs, plan.predictedEndMs))
                    .Real("channelLeftSec", channel ? channel->GetCastTimeRemaining() / 1000.0f : ChannelSec()));
        }
        if (channel || !plan.startedMs)
            holds = Acore::StringFormat(
                "Army of the Dead: {} {}", dk->GetName(),
                channel ? Acore::StringFormat("channels, {:.1f} s left", channel->GetCastTimeRemaining() / 1000.0f)
                        : std::string("starts the channel"));
    }
    return holds;
}
}  // namespace Leader::Prepull
