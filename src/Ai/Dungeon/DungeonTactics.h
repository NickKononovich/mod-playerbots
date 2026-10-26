/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONTACTICS_H
#define PLAYERBOTS_DUNGEONTACTICS_H

#include "AttackAction.h"
#include "Multiplier.h"
#include "ObjectGuid.h"
#include "TbcHazard.h"
#include "Trigger.h"
#include "Value.h"

class Creature;
class Player;
class PlayerbotAI;
class Unit;

// Table-driven dungeon tactics shared by the dungeon strategies (the tables are in DungeonTactics.cpp; the triggers
// and actions are registered with the dungeon hazard contexts, so a strategy only adds the trigger nodes):
// - boss room: the trash of an idle boss's room is fought outside the room, at a rally spot the boss cannot see
//   (Magisters' Terrace: Selin Fireheart's Wretched);
// - melee hold: melee damage dealers leave a mob alone while hitting it is what kills them (Arcatraz Sentinel
//   Energy Discharge while ranged can kill it, Gundrak Drakkari Lancer Retaliation);
// - kill first: damage dealers switch to a mob that makes the fight grow (Blackrock Spire Rookery Hatcher);
// - frontal cleave: everybody but the tank keeps out of the cone in front of a cleaving mob and out of its body
//   (a cone also hits every unit within 2 yd of the caster's centre: Gnomeregan Caverndeep Reaver Arcing Smash).
namespace DungeonTactics
{
struct BossRoom
{
    uint32 mapId;
    uint32 bossEntry;
    float bossX;  // spawn point: the room is only looked at near it
    float bossY;
    float trashRadius;   // an attacker this close to the boss is its room's trash
    float safeDistance;  // nobody stands or chases a target closer to the idle boss than this
    float rallyX;
    float rallyY;
    float rallyZ;
};

// The idle boss whose room trash the bot fights (grid search; read the cached "dungeon boss room" value).
ObjectGuid FindRoomTrashFight(PlayerbotAI* botAI);
// The room and its idle boss from the cached value, or nullptr.
BossRoom const* RoomTrashFight(PlayerbotAI* botAI, Unit*& boss);

// Melee damage dealers must not hit this target now.
bool MeleeShouldHold(Player* bot, Unit* target);
// The attackers a melee damage dealer must not pick now (for the strategies' target exclusions).
void AppendMeleeHoldExclusions(PlayerbotAI* botAI, GuidSet& exclusions);

// The closest attacker the damage dealers kill first, or nullptr.
Unit* KillFirstTarget(PlayerbotAI* botAI);
// While a damage dealer has a kill-first target, every other attacker (for the strategies' "dps target"
// exclusions). Without it "dps assist" (50) picks the generic choice right after "dungeon kill first" (62) switched,
// and the two swap the target every tick: Ahn'kahet batch 6, a mage, a hunter and a shaman cast nothing for 45 s
// ("[idle diag] ... cast 0 ... actions dps assist:21 dungeon kill first:21").
void AppendKillFirstExclusions(PlayerbotAI* botAI, GuidSet& exclusions);

// The bot stands in reach of a cleaving mob's cone (or inside its body) and the mob is on somebody else.
bool InFrontalCleave(Player* bot, PlayerbotAI* botAI, float distMargin, float angleMargin);
bool SpotInFrontalCleave(Player* bot, PlayerbotAI* botAI, float x, float y, float distMargin, float angleMargin);
// A cleaving mob is in the fight near the bot (the generic "set behind" must not place the bot).
bool CleaverNearby(Player* bot, PlayerbotAI* botAI);

// A ranged damage dealer of the bot's group is alive and near.
bool RangedDpsAlive(Player* bot);
}

class DungeonBossRoomValue : public ObjectGuidCalculatedValue
{
public:
    DungeonBossRoomValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "dungeon boss room", 500) {}

protected:
    ObjectGuid Calculate() override { return DungeonTactics::FindRoomTrashFight(botAI); }
};

class DungeonBossRoomTrashTrigger : public Trigger
{
public:
    DungeonBossRoomTrashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon boss room trash") {}
    bool IsActive() override;
};

// Walks out of the idle boss's room to the rally spot; the room trash follows the tank there.
class DungeonBossRoomPullOutAction : public TbcHazardMoveAction
{
public:
    DungeonBossRoomPullOutAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "dungeon boss room pull out") {}
    bool Execute(Event event) override;
};

// While the room trash is fought, nobody chases a target that stands close to the idle boss.
class DungeonBossRoomHoldMultiplier : public Multiplier
{
public:
    DungeonBossRoomHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "dungeon boss room hold") {}
    float GetValue(Action* action) override;
};

class DungeonMeleeHoldTrigger : public Trigger
{
public:
    DungeonMeleeHoldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon melee hold") {}
    bool IsActive() override;
};

// Switches to another attacker, or stops attacking until the held target may be hit again.
class DungeonMeleeHoldAction : public AttackAction
{
public:
    DungeonMeleeHoldAction(PlayerbotAI* botAI) : AttackAction(botAI, "dungeon melee hold") {}
    bool Execute(Event event) override;
};

class DungeonKillFirstTrigger : public Trigger
{
public:
    DungeonKillFirstTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon kill first") {}
    bool IsActive() override;
};

class DungeonKillFirstAction : public AttackAction
{
public:
    DungeonKillFirstAction(PlayerbotAI* botAI) : AttackAction(botAI, "dungeon kill first") {}
    bool Execute(Event event) override;
};

class DungeonFrontalCleaveTrigger : public Trigger
{
public:
    DungeonFrontalCleaveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon frontal cleave") {}
    bool IsActive() override;
};

// Melee: a spot behind the current target, in melee range and out of every cleave; anybody else (or with no such
// spot): the nearest spot out of every cleave.
class DungeonAvoidFrontalCleaveAction : public TbcHazardMoveAction
{
public:
    DungeonAvoidFrontalCleaveAction(PlayerbotAI* botAI)
        : TbcHazardMoveAction(botAI, "dungeon avoid frontal cleave")
    {
    }
    bool Execute(Event event) override;
};

// Near a cleaving mob the generic "set behind" (1 yd from the target's centre, 108 degrees off its facing: inside
// the always-hit body radius) does not run; DungeonAvoidFrontalCleaveAction places the melee instead.
class DungeonFrontalCleaveMultiplier : public Multiplier
{
public:
    DungeonFrontalCleaveMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "dungeon frontal cleave") {}
    float GetValue(Action* action) override;
};

// Encounter support shared by the dungeon strategies (tables in DungeonTactics.cpp, registered with the dungeon hazard
// contexts like the tactics above):
// - priority interrupt: casts the group interrupts whenever someone can (Halls of Reflection Phantom Mage);
// - top up: healers keep everyone near full health in fights whose group-wide hits cannot be avoided or interrupted
//   (Ingvar's roars, Falric's Defiling Horror, Blackheart after Incite Chaos), and a member marked for a delayed burst
//   at full (Garfrost's Deep Freeze);
// - tank cooldown: the tank uses a defensive cooldown when a tank-buster cast starts (Ingvar's Smash / Dark Smash);
// - fear guard: before a fear that cannot be interrupted (Bronjahm in Soulstorm), Tremor Totem, Fear Ward and
//   Berserker Rage;
// - wait for tank: after a boss wipes its threat list (Talon King Ikiss) damage dealers hold until the tank has it.
namespace DungeonTactics
{
// The nearest attacker in reach casting a spell the group always interrupts, or nullptr.
Unit* PriorityInterruptTarget(PlayerbotAI* botAI);

// The member a healer tops up now (the lowest under the fight's threshold), or nullptr.
Player* TopUpTarget(PlayerbotAI* botAI, bool& protect);

// A tank-buster aimed at the bot has started; returns the defensive spell to use, or nullptr.
char const* TankCooldown(PlayerbotAI* botAI);

// The boss of a fight in its fear phase, or nullptr (attackers only).
Unit* FearGuardBoss(PlayerbotAI* botAI);
// The member a priest Fear Wards now, or nullptr.
Player* FearWardTarget(PlayerbotAI* botAI);

// The boss whose threat was just wiped and is not on a tank yet, or nullptr.
Unit* WaitForTankBoss(PlayerbotAI* botAI);
}

class DungeonPriorityInterruptTrigger : public Trigger
{
public:
    DungeonPriorityInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon priority interrupt") {}
    bool IsActive() override;
};

class DungeonPriorityInterruptAction : public Action
{
public:
    DungeonPriorityInterruptAction(PlayerbotAI* botAI) : Action(botAI, "dungeon priority interrupt") {}
    bool Execute(Event event) override;
};

class DungeonTopUpTrigger : public Trigger
{
public:
    DungeonTopUpTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon top up", 500) {}
    bool IsActive() override;
};

// Shield / HoT first on a member under a delayed burst, then the efficient direct heal of the class.
class DungeonTopUpAction : public Action
{
public:
    DungeonTopUpAction(PlayerbotAI* botAI) : Action(botAI, "dungeon top up") {}
    bool Execute(Event event) override;
};

class DungeonTankCooldownTrigger : public Trigger
{
public:
    DungeonTankCooldownTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon tank cooldown") {}
    bool IsActive() override;
};

class DungeonTankCooldownAction : public Action
{
public:
    DungeonTankCooldownAction(PlayerbotAI* botAI) : Action(botAI, "dungeon tank cooldown") {}
    bool Execute(Event event) override;
};

// Shaman: Tremor Totem down; priest: Fear Ward on the member most worth it; warrior: Berserker Rage when the fear
// cast starts in reach.
class DungeonFearGuardTrigger : public Trigger
{
public:
    DungeonFearGuardTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon fear guard", 500) {}
    bool IsActive() override;
};

class DungeonFearGuardAction : public Action
{
public:
    DungeonFearGuardAction(PlayerbotAI* botAI) : Action(botAI, "dungeon fear guard") {}
    bool Execute(Event event) override;
};

// While Tremor Totem is wanted the shaman's other earth totems would replace it.
class DungeonFearGuardMultiplier : public Multiplier
{
public:
    DungeonFearGuardMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "dungeon fear guard") {}
    float GetValue(Action* action) override;
};

class DungeonWaitForTankValue : public BoolCalculatedValue
{
public:
    DungeonWaitForTankValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "dungeon wait for tank", 250) {}

protected:
    bool Calculate() override;

private:
    uint32 _offTankSinceMs = 0;  // when the boss last left the tank; the hold ends after a few seconds
};

class DungeonWaitForTankTrigger : public Trigger
{
public:
    DungeonWaitForTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon wait for tank") {}
    bool IsActive() override;
};

// Stops the bot's own damage on the boss (auto attack, casts, pet) until the tank has it back.
class DungeonWaitForTankAction : public Action
{
public:
    DungeonWaitForTankAction(PlayerbotAI* botAI) : Action(botAI, "dungeon wait for tank") {}
    bool Execute(Event event) override;
};

class DungeonWaitForTankMultiplier : public Multiplier
{
public:
    DungeonWaitForTankMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "dungeon wait for tank") {}
    float GetValue(Action* action) override;
};

#endif
