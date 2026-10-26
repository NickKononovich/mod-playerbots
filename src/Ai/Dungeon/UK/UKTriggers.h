/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UKTRIGGERS_H
#define PLAYERBOTS_UKTRIGGERS_H

#include "Creature.h"
#include "DungeonStrategyUtils.h"
#include "InstanceScript.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

enum UtgardeKeepIDs
{
    // Prince Keleseth
    SPELL_FROST_TOMB               = 48400,
    NPC_FROST_TOMB                 = 23965,

    // Ingvar the Plunderer
    SPELL_STAGGERING_ROAR_N         = 42708,
    SPELL_STAGGERING_ROAR_H         = 59708,
    SPELL_SMASH_N                   = 42669,
    SPELL_SMASH_H                   = 59706,
    SPELL_DREADFUL_ROAR_N           = 42729,
    SPELL_DREADFUL_ROAR_H           = 59734,
    SPELL_WOE_STRIKE_N              = 42730,
    SPELL_WOE_STRIKE_H              = 59735,
    SPELL_DARK_SMASH_N              = 42723,
    SPELL_DARK_SMASH_H              = 59709,
};

#define SPELL_STAGGERING_ROAR       DUNGEON_MODE(bot, SPELL_STAGGERING_ROAR_N, SPELL_STAGGERING_ROAR_H)
#define SPELL_DREADFUL_ROAR         DUNGEON_MODE(bot, SPELL_DREADFUL_ROAR_N, SPELL_DREADFUL_ROAR_H)
#define SPELL_SMASH                 DUNGEON_MODE(bot, SPELL_SMASH_N, SPELL_SMASH_H)
#define SPELL_DARK_SMASH            DUNGEON_MODE(bot, SPELL_DARK_SMASH_N, SPELL_DARK_SMASH_H)
// [b33_uk] Woe Strike 42730 / 59735 (phase 2, on Ingvar's victim, 10 s, recast every 3-7 s): its proc aura (procFlags
// 0x8800 = any positive magic spell or periodic taken, no spell_proc filter) fires 42739 / 59736 (1750-2250 Shadow,
// raised by the Dreadful Roar stacks) at whoever cast the positive spell, buffs included, not only heals.
#define SPELL_WOE_STRIKE            DUNGEON_MODE(bot, SPELL_WOE_STRIKE_N, SPELL_WOE_STRIKE_H)

namespace UKIds
{
// boss_skarvald_dalronn.cpp: whichever twin dies first comes back as an unselectable ghost (27390 / 27389) that
// keeps fighting until the other dies. Dalronn's ghost goes on casting Shadow Bolt 43649 (2 s, every 2 s) and,
// being unselectable, can be neither interrupted nor killed (wave 8: it killed the hunter after Dalronn died
// first). Skarvald's ghost only melees. So Skarvald dies first, and the living Dalronn's casts are interrupted.
constexpr uint32 NPC_SKARVALD = 24200;
constexpr uint32 NPC_DALRONN = 24201;
constexpr float DALRONN_INTERRUPT_RANGE = 30.0f;

// boss_ingvar_the_plunderer.cpp: Smash 42669 / Dark Smash 42723 (3 s cast, Ingvar rooted with rotation disabled):
// a 60 degree, 10 yd cone (spell_cone), which Spell.cpp also lands on every unit within the caster's boundary
// (MIN_MELEE_REACH 2 yd plus both bodies) whatever the angle. Every Smash death so far stood 0.5-1.8 yd from his
// centre, behind him included.
constexpr float SMASH_CONE_RADIUS = 10.0f;
constexpr float SMASH_ALWAYS_HIT_REACH = 2.0f;  // MIN_MELEE_REACH
constexpr float SMASH_SAFE_ARC = static_cast<float>(M_PI) / 2.0f;  // the 60 degree cone with 15 degrees each side
constexpr float SMASH_MARGIN = 1.5f;

// Ingvar is casting Smash or Dark Smash.
bool IsIngvarSmashing(Player* bot, Unit* boss);
// Radius around Ingvar's centre that every Smash hits regardless of facing, plus a margin.
float SmashBoundaryRadius(Player* bot, Unit* boss);
// The point (x, y) is inside the boundary or in the cone in front of Ingvar.
bool InSmashDanger(Player* bot, Unit* boss, float x, float y);

// utgarde_keep.cpp npc_dragonflayer_forge_master / utgarde_keep.h
constexpr uint32 NPC_DRAGONFLAYER_FORGE_MASTER = 24079;
constexpr uint32 DATA_FORGE_1 = 100;
constexpr uint32 DATA_FORGE_2 = 101;
// creature spawns 125904 / 125905 (acore_world); the third master (125903) needs master 2 first
constexpr float FORGE_1_X = 349.556f;
constexpr float FORGE_1_Y = -39.3328f;
constexpr float FORGE_2_X = 385.775f;
constexpr float FORGE_2_Y = -16.2345f;
constexpr float FORGE_HOME_MATCH_RADIUS = 5.0f;

// A forge master engaged before the previous one evades at once (utgarde_keep.cpp JustEngagedWith checks
// GetData(prevDataId)); the order follows its home position exactly like the script's constructor. Shared by the
// strategy's target exclusion and the test harness runner.
inline bool IsForgeMasterLocked(Creature const* master, InstanceScript const* instance)
{
    if (!master || !instance || master->GetEntry() != NPC_DRAGONFLAYER_FORGE_MASTER)
        return false;

    Position const& home = master->GetHomePosition();
    uint32 prevDataId = DATA_FORGE_2;
    if (home.GetExactDist2d(FORGE_1_X, FORGE_1_Y) < FORGE_HOME_MATCH_RADIUS)
        prevDataId = 0;
    else if (home.GetExactDist2d(FORGE_2_X, FORGE_2_Y) < FORGE_HOME_MATCH_RADIUS)
        prevDataId = DATA_FORGE_1;

    return prevDataId && !instance->GetData(prevDataId);
}
}  // namespace UKIds

class KelesethFrostTombTrigger : public Trigger
{
public:
    KelesethFrostTombTrigger(PlayerbotAI* botAI) : Trigger(botAI, "keleseth frost tomb") {}
    bool IsActive() override;
};

// Skarvald the Constructor is alive: the damage dealers kill him before Dalronn (see UKIds above).
class SkarvaldPriorityTrigger : public Trigger
{
public:
    SkarvaldPriorityTrigger(PlayerbotAI* botAI) : Trigger(botAI, "skarvald priority") {}
    bool IsActive() override;
};

// The living Dalronn the Controller is casting within interrupt reach.
class DalronnInterruptTrigger : public Trigger
{
public:
    DalronnInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dalronn interrupt") {}
    bool IsActive() override;
};

// Ingvar casts Smash / Dark Smash and the bot (not the tank) stands in its cone or inside his boundary.
class IngvarSmashDangerTrigger : public Trigger
{
public:
    IngvarSmashDangerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ingvar smash danger") {}
    bool IsActive() override;
};

class IngvarDreadfulRoarTrigger : public Trigger
{
public:
    IngvarDreadfulRoarTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ingvar dreadful roar") {}
    bool IsActive() override;
};

class IngvarSmashTankTrigger : public Trigger
{
public:
    IngvarSmashTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ingvar smash tank") {}
    bool IsActive() override;
};

class IngvarSmashTankReturnTrigger : public Trigger
{
public:
    IngvarSmashTankReturnTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ingvar smash tank return") {}
    bool IsActive() override;
};

class NotBehindIngvarTrigger : public Trigger
{
public:
    NotBehindIngvarTrigger(PlayerbotAI* botAI) : Trigger(botAI, "not behind ingvar") {}
    bool IsActive() override;
};

#endif
