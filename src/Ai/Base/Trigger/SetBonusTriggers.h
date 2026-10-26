/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SETBONUSTRIGGERS_H
#define PLAYERBOTS_SETBONUSTRIGGERS_H

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common.h"
#include "Trigger.h"

class PlayerbotAI;
class Player;

// Rotation reactions to the custom tier set bonuses (hard T4-H / T5-H / T6-H, E1 Titanforged, E2 Ascendant), see
// C:/WoW/SET_BONUS_SPELLS.md. A bonus is a passive aura the item set puts on the player; a proc bonus casts a charge
// aura on him (a native one such as Slam! or Divine Favor, or a clone such as Lunar Surge). Every check here is an
// aura lookup by spell id (Unit::HasAura, a map lookup), never a scan of the aura list by name. Whether the set is
// worn is cached per trigger for WORN_RECHECK_MS: gear does not change in a fight.
namespace ai::setbonus
{
// "party member to heal" health gate, resolved against the PlayerbotAIConfig thresholds at run time.
enum class HealGate : uint8
{
    NONE,
    ALMOST_FULL,
    MEDIUM,
    LOW
};

struct TriggerSpec
{
    std::vector<uint32> auras;   // any of these on the bot (the proc / charge aura); empty: no check
    std::vector<uint32> worn;    // any of these set bonus passives on the bot; empty: no check
    std::vector<uint32> absent;  // none of these on the bot
    uint32 stackAura = 0;        // this aura on the bot with at least minStacks stacks
    uint8 minStacks = 0;
    std::string readySpell;      // learned and not on cooldown
    HealGate healGate = HealGate::NONE;
    uint8 aoeHealCount = 0;      // at least this many members below almost full health ("aoe heal")
    uint8 selfHealthBelow = 0;   // bot health pct below this (0: no check)
    uint8 selfHealthAbove = 0;   // bot health pct above this (0: no check)
    uint8 maxManaPct = 0;        // bot mana pct below this (0: no check)
    uint8 minRage = 0;           // rage window (0: no check)
    uint8 maxRage = 0;
    uint8 minComboPoints = 0;    // on the current target
    bool tankingBoss = false;    // in combat, the current target is a boss and attacks the bot
    uint32 icdAura = 0;          // proc aura of an internal cooldown: fire only icdMs after it was last seen
    uint32 icdMs = 0;
    std::string note;            // log text: what the bonus makes the bot do
};

using TriggerCreatorMap = std::unordered_map<std::string, std::function<Trigger*(PlayerbotAI*)>>;

// Registers every "set bonus ..." trigger in a trigger context (TriggerContext, shared by all classes).
void RegisterTriggers(TriggerCreatorMap& creators);

// [setbonus-rot] log line, at most once per LOG_INTERVAL_MS per trigger and bot.
constexpr uint32 LOG_INTERVAL_MS = 30 * IN_MILLISECONDS;
constexpr uint32 WORN_RECHECK_MS = 10 * IN_MILLISECONDS;
}  // namespace ai::setbonus

class SetBonusTrigger : public Trigger
{
public:
    SetBonusTrigger(PlayerbotAI* botAI, std::string const name, ai::setbonus::TriggerSpec spec)
        : Trigger(botAI, name), _spec(std::move(spec))
    {
    }

    std::string const GetTargetName() override { return "self target"; }
    bool IsActive() override;

private:
    bool IsWorn();
    bool Conditions();

    ai::setbonus::TriggerSpec const _spec;
    bool _worn = false;
    uint32 _wornCheckMs = 0;
    uint32 _icdSeenMs = 0;
    uint32 _lastLogMs = 0;
};

#endif
