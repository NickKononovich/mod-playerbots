/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_THREATSTRATEGY_H
#define PLAYERBOTS_THREATSTRATEGY_H

#include "LeaderOrders.h"  // [rot2] Leader::MemberEvents
#include "ObjectGuid.h"
#include "Strategy.h"

#include <string>
#include <unordered_map>

class CastSpellAction;
class PlayerbotAI;
class SpellInfo;
class Unit;

class ThreatMultiplier : public Multiplier
{
public:
    ThreatMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "threat") {}

    float GetValue(Action* action) override;

private:
    static constexpr uint32 HOLD_LOG_MS = 5000;
    // [fresh_add_hold] A ranged damage dealer gives the tank this long on a mob new to the fight (an add, a wave, the
    // pack at the pull start) that is not on a tank yet, as a player waits for the tank's lead on Omen.
    static constexpr uint32 FRESH_ADD_HOLD_MS = 1500;
    static constexpr float FRESH_ADD_TANK_RANGE = 40.0f;  // a tank farther from the mob than this takes it by no taunt
    static constexpr size_t FRESH_ADD_MEMO = 32;          // mobs remembered; the oldest go first

    bool HoldsForFreshAdd(Unit* target);
    // [rot1] the crowd-control gate (ThreatStrategy.cpp)
    bool CrowdControlHolds(CastSpellAction* action, Unit* target);
    // [rot1] the area gate: every mob of the fight inside the spell's area
    bool AreaHolds(CastSpellAction* action, SpellInfo const* info, bool careful);

    uint32 _loggedMs = 0;
    uint32 _ccLoggedMs = 0;   // [rot1]
    uint32 _aoeLoggedMs = 0;  // [rot1]
    std::unordered_map<ObjectGuid, uint32> _freshSeen;  // mob, when this bot first wanted to hit it in the fight
    ObjectGuid _freshLogged;
};

// [rot2] The rotation side of the dungeon leader's commands (LeaderCommand.h) for a led bot; inert (1.0) for any
// bot the leader does not lead. Registered by InterruptStrategy (every combat engine outside battlegrounds).
// - AoE: an Aoe action only where LeaderCommand::AoeAllowed says so (the per-mob threat gate still applies);
// - Stop: no offensive cast; Normal: the long offensive cooldowns (2 min and more) wait for Burn;
// - DontAttack: no cast on that mob, no area spell whose area holds it;
// - the autonomous externals on others (Innervate on a healer, Hymns, Power Infusion, Pain Suppression on others,
//   the Hand / Blessing of Protection spells, Misdirection, Tricks, Bloodlust / Heroism) only by the leader's
//   assignment ("leader external");
// - crowd control: with a leader assignment, only on the assigned mob.
class LeaderCommandMultiplier : public Multiplier
{
public:
    LeaderCommandMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "leader command") {}

    float GetValue(Action* action) override;

private:
    static constexpr uint32 COMMAND_CACHE_MS = 100;  // one command read per bot this often (a lock)
    static constexpr uint32 LOG_MS = 5000;

    bool Refresh();
    void Log(Action* action, char const* why);

    uint32 _readMs = 0;
    bool _led = false;
    bool _hasCommand = false;
    Leader::MemberEvents _command;
    uint32 _loggedMs = 0;
    std::string _loggedAction;
};

class ThreatStrategy : public Strategy
{
public:
    ThreatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "threat"; }
};

class FocusMultiplier : public Multiplier
{
public:
    FocusMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "focus") {}

    float GetValue(Action* action) override;
};

class FocusStrategy : public Strategy
{
public:
    FocusStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "focus"; }
};

#endif
