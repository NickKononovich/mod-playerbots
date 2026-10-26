/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HOLTRIGGERS_H
#define PLAYERBOTS_HOLTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

enum HallsOfLightningIDs
{
    // General Bjarngrim
    NPC_STORMFORGED_LIEUTENANT      = 29240,
    SPELL_WHIRLWIND_BJARNGRIM       = 52027,

    // Ionar
    SPELL_STATIC_OVERLOAD_N         = 52658,
    SPELL_STATIC_OVERLOAD_H         = 59795,
    SPELL_BALL_LIGHTNING_N          = 52780,
    SPELL_BALL_LIGHTNING_H          = 59800,
    SPELL_DISPERSE                  = 52770,
    NPC_SPARK_OF_IONAR              = 28926,

    // Loken
    SPELL_LIGHTNING_NOVA_N          = 52960,
    SPELL_LIGHTNING_NOVA_H          = 59835,
};

#define SPELL_STATIC_OVERLOAD       DUNGEON_MODE(bot, SPELL_STATIC_OVERLOAD_N, SPELL_STATIC_OVERLOAD_H)
#define SPELL_BALL_LIGHTNING        DUNGEON_MODE(bot, SPELL_BALL_LIGHTNING_N, SPELL_BALL_LIGHTNING_H)
#define SPELL_LIGHTNING_NOVA        DUNGEON_MODE(bot, SPELL_LIGHTNING_NOVA_N, SPELL_LIGHTNING_NOVA_H)

namespace HoLIds
{
// boss_volkhan.cpp: Shattering Stomp is cast once below 25 % and makes every Brittle Golem cast Shatter
// (52429/59527, 10 yd) at the start of the 3 s cast, so the bots must already be clear when the boss nears 25 %.
constexpr uint32 NPC_BRITTLE_GOLEM = 28681;
constexpr uint32 SPELL_SHATTERING_STOMP_N = 52237;
constexpr uint32 SPELL_SHATTERING_STOMP_H = 59529;
constexpr float STOMP_WINDOW_HIGH_PCT = 40.0f;
constexpr float STOMP_WINDOW_LOW_PCT = 20.0f;
constexpr float SHATTER_SAFE_DISTANCE = 13.0f;
}

namespace HoLShared
{
Unit* BrittleGolemThreat(Player* bot, Unit* volkhan);
bool IsLightningNova(Player* bot, Unit* loken);
}

class StormforgedLieutenantTrigger : public Trigger
{
public:
    StormforgedLieutenantTrigger(PlayerbotAI* botAI) : Trigger(botAI, "stormforged lieutenant") {}
    bool IsActive() override;
};

class BjarngrimWhirlwindTrigger : public Trigger
{
public:
    BjarngrimWhirlwindTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bjarngrim whirlwind") {}
    bool IsActive() override;
};

class VolkhanTrigger : public Trigger
{
public:
    VolkhanTrigger(PlayerbotAI* botAI) : Trigger(botAI, "volkhan") {}
    bool IsActive() override;
};

class VolkhanBrittleGolemTrigger : public Trigger
{
public:
    VolkhanBrittleGolemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "volkhan brittle golem") {}
    bool IsActive() override;
};

class IonarStaticOverloadTrigger : public Trigger
{
public:
    IonarStaticOverloadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ionar static overload") {}
    bool IsActive() override;
};

class IonarBallLightningTrigger : public Trigger
{
public:
    IonarBallLightningTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ionar ball lightning spread") {}
    bool IsActive() override;
};

class IonarTankAggroTrigger : public Trigger
{
public:
    IonarTankAggroTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ionar tank aggro") {}
    bool IsActive() override;
};

class LokenRangedTrigger : public Trigger
{
public:
    LokenRangedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "loken ranged") {}
    bool IsActive() override;
};

class LokenLightningNovaTrigger : public Trigger
{
public:
    LokenLightningNovaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "lightning nova") {}
    bool IsActive() override;
};

#endif
