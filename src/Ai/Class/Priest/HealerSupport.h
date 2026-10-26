/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HEALERSUPPORT_H
#define PLAYERBOTS_HEALERSUPPORT_H

#include "GenericSpellActions.h"
#include "Playerbots.h"
#include "Trigger.h"
#include "Value.h"
#include <unordered_map>

// Shared by the priest and shaman healers (registered in both class contexts; implemented in PriestTriggers.cpp).
namespace ai::healer
{
static constexpr uint32 BATTLE_REZ_WINDOW_MS = 20 * IN_MILLISECONDS;  // "just resurrected": died this recently
static constexpr float BATTLE_REZ_SAFE_HEALTH_PCT = 70.0f;
static constexpr uint32 PEEL_HOLD_MS = 4 * IN_MILLISECONDS;  // the picked-up mobs' first swings land after the taunt
static constexpr float PEEL_TANK_TOP_UP_PCT = 80.0f;
static constexpr uint32 HEALER_VALUE_INTERVAL_MS = 500;
}

// The group member brought back during the fight (battle res, Soulstone, Reincarnation) who still needs care:
// alive again, seen dead within the last BATTLE_REZ_WINDOW_MS. The lowest health one when there are several.
class BattleResurrectedMemberValue : public UnitCalculatedValue
{
public:
    BattleResurrectedMemberValue(PlayerbotAI* botAI)
        : UnitCalculatedValue(botAI, "battle resurrected member", ai::healer::HEALER_VALUE_INTERVAL_MS)
    {
    }

    Unit* Calculate() override;

private:
    std::unordered_map<ObjectGuid, uint32> _deadSeenAt;
};

// A tank picking mobs off a healer or dps: it targets a mob that hits a non-tank group member, or it has just
// taunted its target. Held for PEEL_HOLD_MS so the healer covers the incoming swings.
class PeelingTankValue : public UnitCalculatedValue
{
public:
    PeelingTankValue(PlayerbotAI* botAI)
        : UnitCalculatedValue(botAI, "peeling tank", ai::healer::HEALER_VALUE_INTERVAL_MS)
    {
    }

    Unit* Calculate() override;

private:
    std::unordered_map<ObjectGuid, uint32> _peelUntil;
};

class BattleResurrectedMemberTrigger : public Trigger
{
public:
    BattleResurrectedMemberTrigger(PlayerbotAI* botAI) : Trigger(botAI, "battle resurrected member") {}

    bool IsActive() override { return AI_VALUE(Unit*, "battle resurrected member"); }
};

class BattleResurrectedMemberLowHealthTrigger : public Trigger
{
public:
    BattleResurrectedMemberLowHealthTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "battle resurrected member low health")
    {
    }

    bool IsActive() override
    {
        Unit* member = AI_VALUE(Unit*, "battle resurrected member");
        return member && member->GetHealthPct() < ai::healer::BATTLE_REZ_SAFE_HEALTH_PCT;
    }
};

class TankPeelTrigger : public Trigger
{
public:
    TankPeelTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tank peel") {}

    bool IsActive() override { return AI_VALUE(Unit*, "peeling tank"); }
};

// A heal on the unit a value names, e.g. "flash heal on battle resurrected"; only below maxHealthPct when given.
class CastHealOnValueTargetAction : public CastHealingSpellAction
{
public:
    CastHealOnValueTargetAction(PlayerbotAI* botAI, std::string const spell, std::string const targetValue,
                                std::string const suffix, uint8 estAmount, HealingManaEfficiency manaEfficiency,
                                bool isOwner = true, float maxHealthPct = 100.0f)
        : CastHealingSpellAction(botAI, spell, estAmount, manaEfficiency, isOwner),
          _targetValue(targetValue),
          _name(spell + " on " + suffix),
          _maxHealthPct(maxHealthPct)
    {
    }

    std::string const GetTargetName() override { return _targetValue; }
    std::string const getName() override { return _name; }

    bool isUseful() override
    {
        Unit* target = GetTarget();
        return target && target->GetHealthPct() < _maxHealthPct && CastHealingSpellAction::isUseful();
    }

private:
    std::string const _targetValue;
    std::string const _name;
    float const _maxHealthPct;
};

// A buff on the unit a value names, e.g. "power word: fortitude on battle resurrected".
class CastBuffOnValueTargetAction : public CastBuffSpellAction
{
public:
    CastBuffOnValueTargetAction(PlayerbotAI* botAI, std::string const spell, std::string const targetValue,
                                std::string const suffix, bool checkIsOwner = false)
        : CastBuffSpellAction(botAI, spell, checkIsOwner), _targetValue(targetValue), _name(spell + " on " + suffix)
    {
    }

    std::string const GetTargetName() override { return _targetValue; }
    std::string const getName() override { return _name; }

private:
    std::string const _targetValue;
    std::string const _name;
};

#endif
