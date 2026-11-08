/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WORLDBOSSTACTICS_H
#define PLAYERBOTS_WORLDBOSSTACTICS_H

#include <string>

#include "AttackAction.h"
#include "MovementActions.h"
#include "Multiplier.h"
#include "Trigger.h"

class Player;
class PlayerbotAI;
class Unit;

// [world_boss] Tactics for the world boss ladder (C:/WoW/docs/WORLD_BOSS_PLAN.md §4): Lord Kazzak, Azuregos,
// Doomwalker, Doom Lord Kazzak, the four Emerald Dragons, Highlord Kruul (mod-world-bosses, ScriptName npc_world_boss).
// The fights are open-world raids: no instance, so no raid-leader coordinator (RaidLeader::FindCoordinator is nullptr
// outside instances) - every member reads the fight itself from game state (auras, stacks, casts, positions). No
// elapsed-time rule decides anything here (no-forcing-timers); the only timers are the boss's own.
namespace WorldBoss
{
// boss entries (acore_world creature_template, world_boss table)
constexpr uint32 NPC_LORD_KAZZAK = 12397;
constexpr uint32 NPC_AZUREGOS = 6109;
constexpr uint32 NPC_DOOMWALKER = 17711;
constexpr uint32 NPC_DOOM_LORD_KAZZAK = 18728;
constexpr uint32 NPC_TAERAR = 14890;
constexpr uint32 NPC_EMERISS = 14889;
constexpr uint32 NPC_LETHON = 14888;
constexpr uint32 NPC_YSONDRE = 14887;
constexpr uint32 NPC_KRUUL = 18338;
// native adds
constexpr uint32 NPC_DREAM_FOG = 15224;
constexpr uint32 NPC_SPIRIT_SHADE = 15261;  // walks to Lethon and heals him (Dark Offering)
// add clones of gen_world_bosses.js: 990000 + 10 * (boss index + 1) + k; the healers' SmartAI casts Heal on the boss
constexpr uint32 NPC_ADD_FIRST = 990000;
constexpr uint32 NPC_ADD_LAST = 990999;

// spells the tactics read (3.3.5a ids, raids/world_bosses.json)
constexpr uint32 SPELL_ADD_HEAL = 22883;             // Heal of the healer adds: interrupt
constexpr uint32 SPELL_LIVING_BOMB = 20475;          // Kruul: the target runs out
constexpr uint32 SPELL_MARK_OF_KAZZAK_TBC = 32960;   // Doom Lord Kazzak: mana drain, explodes at 0 mana
constexpr uint32 SPELL_VOLATILE_INFECTION = 24928;   // Emeriss: disease, the carrier moves out
constexpr uint32 SPELL_MANASTORM = 21097;            // Azuregos: persistent area, move out
constexpr uint32 SPELL_EARTHQUAKE = 32686;           // Doomwalker: raid damage for 8 s
constexpr uint32 SPELL_CORRUPTION_OF_EARTH = 24910;  // Emeriss: raid DoT at 75 / 50 / 25 %
constexpr uint32 SPELL_DIVINE_HYMN = 64843;
constexpr uint32 SPELL_TRANQUILITY_FIRST = 740;  // any rank: by name

struct BossTactic
{
    uint32 entry;
    uint32 swapAura;    // the stacking tank debuff; 0: none
    uint8 swapStacks;   // the other tank taunts at this many stacks on the current one
    bool sides;         // non-tanks stand at the boss's sides (frontal breath + tail sweep)
    bool frontShare;    // melee damage dealers stand in the front cone with the tanks (Meteor Slash is split)
    bool spreadRanged;  // chain spells: ranged and healers keep 9 yd from each other
    std::initializer_list<uint8> raidPhases;  // boss HP % where a raid-damage phase starts (healer cooldowns)
};

BossTactic const* Tactic(uint32 entry);
// the world boss the bot fights now (its victim, an attacker, or one within 60 yd in combat); nullptr: none
Unit* FightBoss(Player* bot, PlayerbotAI* botAI);
// the healer add (Heal on the boss) / Spirit Shade the damage dealers kill first, nearest; nullptr: none
Unit* PriorityAdd(Player* bot, PlayerbotAI* botAI, Unit* boss);
uint8 Stacks(Unit* unit, uint32 spellId);
bool IsTauntAction(std::string const& name);
bool CastClassTaunt(Player* bot, PlayerbotAI* botAI, Unit* target);
// the same with what happened: the spell cast, or why not (range, not known, cooldown, cannot be cast now)
bool CastClassTaunt(Player* bot, PlayerbotAI* botAI, Unit* target, std::string& why);
// [world_boss] a decision line in Playerbots.log, at most one per bot and kind every 5 s; true: written
bool Note(Player* bot, char const* key, std::string const& text);
}  // namespace WorldBoss

// --- triggers ---------------------------------------------------------------------------------------------------
class WorldBossTankSwapTrigger : public Trigger
{
public:
    WorldBossTankSwapTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss tank swap") {}
    bool IsActive() override;
};

// the boss hits a non-tank (Arcane Vacuum / Overrun reset the threat list): the least stacked tank takes it back
class WorldBossThreatResetTrigger : public Trigger
{
public:
    WorldBossThreatResetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss threat reset") {}
    bool IsActive() override;
};

// an add hits a non-tank: the tank that does not hold the boss takes it
class WorldBossLooseAddTrigger : public Trigger
{
public:
    WorldBossLooseAddTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss loose add") {}
    bool IsActive() override;
};

class WorldBossHealerAddCastTrigger : public Trigger
{
public:
    WorldBossHealerAddCastTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss add heal cast") {}
    bool IsActive() override;
};

class WorldBossPriorityAddTrigger : public Trigger
{
public:
    WorldBossPriorityAddTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss priority add") {}
    bool IsActive() override;
};

class WorldBossDispelTrigger : public Trigger
{
public:
    WorldBossDispelTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss dispel") {}
    bool IsActive() override;
};

// the bot carries Living Bomb / Mark of Kazzak (TBC) / Volatile Infection: away from the group
class WorldBossCarrierTrigger : public Trigger
{
public:
    WorldBossCarrierTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss carrier") {}
    bool IsActive() override;
};

class WorldBossSpreadTrigger : public Trigger
{
public:
    WorldBossSpreadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss spread") {}
    bool IsActive() override;
};

// inside Manastorm or next to a Dream Fog
class WorldBossMoveOutTrigger : public Trigger
{
public:
    WorldBossMoveOutTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss move out") {}
    bool IsActive() override;
};

class WorldBossPositionTrigger : public Trigger
{
public:
    WorldBossPositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss position") {}
    bool IsActive() override;
};

class WorldBossRaidCooldownTrigger : public Trigger
{
public:
    WorldBossRaidCooldownTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world boss raid cooldown") {}
    bool IsActive() override;
};

// --- actions ----------------------------------------------------------------------------------------------------
class WorldBossTauntAction : public AttackAction
{
public:
    WorldBossTauntAction(PlayerbotAI* botAI) : AttackAction(botAI, "world boss taunt") {}
    bool Execute(Event event) override;
};

class WorldBossTauntAddAction : public AttackAction
{
public:
    WorldBossTauntAddAction(PlayerbotAI* botAI) : AttackAction(botAI, "world boss taunt add") {}
    bool Execute(Event event) override;
};

class WorldBossInterruptAddAction : public Action
{
public:
    WorldBossInterruptAddAction(PlayerbotAI* botAI) : Action(botAI, "world boss interrupt add") {}
    bool Execute(Event event) override;
};

class WorldBossAttackPriorityAddAction : public AttackAction
{
public:
    WorldBossAttackPriorityAddAction(PlayerbotAI* botAI) : AttackAction(botAI, "world boss attack priority add") {}
    bool Execute(Event event) override;
};

class WorldBossDispelAction : public Action
{
public:
    WorldBossDispelAction(PlayerbotAI* botAI) : Action(botAI, "world boss dispel") {}
    bool Execute(Event event) override;
};

class WorldBossCarrierMoveAction : public MovementAction
{
public:
    WorldBossCarrierMoveAction(PlayerbotAI* botAI) : MovementAction(botAI, "world boss carrier move") {}
    bool Execute(Event event) override;
};

class WorldBossSpreadAction : public MovementAction
{
public:
    WorldBossSpreadAction(PlayerbotAI* botAI) : MovementAction(botAI, "world boss spread out") {}
    bool Execute(Event event) override;
};

class WorldBossMoveOutAction : public MovementAction
{
public:
    WorldBossMoveOutAction(PlayerbotAI* botAI) : MovementAction(botAI, "world boss move out of area") {}
    bool Execute(Event event) override;
};

class WorldBossPositionAction : public MovementAction
{
public:
    WorldBossPositionAction(PlayerbotAI* botAI) : MovementAction(botAI, "world boss position") {}
    bool Execute(Event event) override;
};

class WorldBossRaidCooldownAction : public Action
{
public:
    WorldBossRaidCooldownAction(PlayerbotAI* botAI) : Action(botAI, "world boss raid cooldown") {}
    bool Execute(Event event) override;
};

// the tank carrying the swap stacks does not taunt the boss back (the generic "lose aggro" taunt would ping-pong)
class WorldBossStackedTankMultiplier : public Multiplier
{
public:
    WorldBossStackedTankMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "world boss stacked tank") {}
    float GetValue(Action* action) override;
};

#endif
