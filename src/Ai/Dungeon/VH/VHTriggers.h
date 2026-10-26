/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_VHTRIGGERS_H
#define PLAYERBOTS_VHTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

enum VioletHoldIDs
{
    // Ichoron
    SPELL_DRAINED                      = 59820,
    NPC_ICHOR_GLOBULE                  = 29321,

    // Zuramat the Obliterator
    SPELL_VOID_SHIFTED                 = 54343,
    SPELL_SHROUD_OF_DARKNESS_N         = 54524,
    SPELL_SHROUD_OF_DARKNESS_H         = 59745,
    NPC_VOID_SENTRY                    = 29364,
};

#define SPELL_SHROUD_OF_DARKNESS    DUNGEON_MODE(bot, SPELL_SHROUD_OF_DARKNESS_N, SPELL_SHROUD_OF_DARKNESS_H)

class Creature;
class InstanceScript;

namespace VHIds
{
// violet_hold.h VHData / VHBosses (ObjectData and GetData ids)
constexpr uint32 DATA_CYANIGOSA = 2;
constexpr uint32 BOSS_MORAGG = 3;
constexpr uint32 BOSS_ZURAMAT = 8;
constexpr uint32 DATA_SINCLARI = 10;
constexpr uint32 DATA_DOOR_SEAL = 11;
constexpr uint32 DATA_MAIN_DOOR = 20;
constexpr uint32 DATA_TELEPORTATION_PORTAL = 12;
constexpr uint32 DATA_ENCOUNTER_STATUS = 30;
constexpr uint32 DATA_WAVE_COUNT = 33;
constexpr uint32 WAVE_FIRST_BOSS = 6;
constexpr uint32 WAVE_SECOND_BOSS = 12;
constexpr uint32 WAVE_CYANIGOSA = 18;

// violet_hold.h VHCreatures
constexpr uint32 NPC_PORTAL_GUARDIAN = 30660;
constexpr uint32 NPC_PORTAL_KEEPER_1 = 30695;
constexpr uint32 NPC_PORTAL_KEEPER_2 = 30893;
constexpr uint32 NPC_ETHEREAL_SPHERE = 29271;  // boss_xevozz.cpp, summoned by 54102/54137/54138

// violet_hold.h VHSpells
constexpr uint32 SPELL_DESTROY_DOOR_SEAL = 58040;

// gossip_menu_option 9997 option 0 -> menu 9998 option 0 -> SAI ACTION_START_INSTANCE (smart_scripts 30658)
constexpr uint32 GOSSIP_MENU_SINCLARI = 9997;
constexpr uint32 GOSSIP_MENU_SINCLARI_START = 9998;
// gossip_menu_option 9997 option 1 "I'm not fighting, so send me in now!", shown only while the encounter is
// IN_PROGRESS (conditions 15/9997/1): SAI teleports the player inside the seal to (1830.53, 803.94, 44.34).
constexpr uint32 GOSSIP_OPTION_SINCLARI_LATE_JOIN = 1;

// Prison Seal (gameobject 191723) at x 1822.59, closed while the event runs. West of it lie the instance arrival
// (areatrigger_teleport 5209: 1808.82, 803.93) and Sinclari's spot during the event (sinclariOutsidePosition
// 1817.32, 804.06); the prison is east of it.
constexpr float SEAL_X = 1822.59f;

// Bottom of the stairs in front of the door: every trash waypoint path (violet_hold.h) passes within ~10 yd
constexpr float RALLY_X = 1868.0f;
constexpr float RALLY_Y = 805.0f;
constexpr float RALLY_Z = 38.7f;
constexpr float RALLY_RADIUS = 10.0f;

// violet_hold.h MiddleRoomLocation, Xevozz is kited on a circle around it
constexpr float ROOM_CENTER_X = 1892.29f;
constexpr float ROOM_CENTER_Y = 805.70f;
constexpr float ROOM_CENTER_Z = 38.44f;

constexpr float KEEPER_SEARCH_RADIUS = 10.0f;  // keepers are summoned 2 yd from the portal
constexpr float SPHERE_DANGER_RADIUS = 15.0f;  // Arcane Power at 3 yd, spheres creep at ~3 yd/s
}

namespace VHShared
{
uint32 EventStatus(Player* bot);
bool IsEventInProgress(Player* bot);
// The event runs, the seal is closed and the bot stands outside it (came back in through the portal after a
// release). Not in the seconds between the start and the closing: the group still walks in then ("vh rally").
bool IsOutsideSeal(Player* bot);
bool IsSealUnderAttack(Player* bot);
Unit* NearestSealAttacker(Player* bot);
Creature* ActiveKeeperPortal(Player* bot);
Unit* PortalKeeper(Creature* portal);
// The trigger condition of "vh attack portal keeper": a keeper portal is open and the seal is not under attack.
bool IsPortalKeeperPhase(Player* bot);
Creature* ReleasedBoss(Player* bot);
}

class ErekemTargetTrigger : public Trigger
{
public:
    ErekemTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "erekem target") {}
    bool IsActive() override;
};

class IchoronTargetTrigger : public Trigger
{
public:
    IchoronTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ichoron target") {}
    bool IsActive() override;
};

class VoidShiftTrigger : public Trigger
{
public:
    VoidShiftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "void shift") {}
    bool IsActive() override;
};

class ShroudOfDarknessTrigger : public Trigger
{
public:
    ShroudOfDarknessTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shroud of darkness") {}
    bool IsActive() override;
};

class CyanigosaPositioningTrigger : public Trigger
{
public:
    CyanigosaPositioningTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cyanigosa positioning") {}
    bool IsActive() override;
};

class VHSinclariStartTrigger : public Trigger
{
public:
    VHSinclariStartTrigger(PlayerbotAI* botAI) : Trigger(botAI, "vh sinclari start") {}
    bool IsActive() override;
};

class VHSealUnderAttackTrigger : public Trigger
{
public:
    VHSealUnderAttackTrigger(PlayerbotAI* botAI) : Trigger(botAI, "vh seal under attack") {}
    bool IsActive() override;
};

class VHPortalKeeperTrigger : public Trigger
{
public:
    VHPortalKeeperTrigger(PlayerbotAI* botAI) : Trigger(botAI, "vh portal keeper") {}
    bool IsActive() override;
};

class VHReleasedBossTrigger : public Trigger
{
public:
    VHReleasedBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "vh released boss") {}
    bool IsActive() override;
};

class VHRallyTrigger : public Trigger
{
public:
    VHRallyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "vh rally") {}
    bool IsActive() override;
};

// Alive outside the closed seal while the event runs: in again through Sinclari's late-join gossip, as a player does.
class VHLateJoinTrigger : public Trigger
{
public:
    VHLateJoinTrigger(PlayerbotAI* botAI) : Trigger(botAI, "vh late join") {}
    bool IsActive() override;
};

class XevozzEtherealSphereTrigger : public Trigger
{
public:
    XevozzEtherealSphereTrigger(PlayerbotAI* botAI) : Trigger(botAI, "xevozz ethereal sphere") {}
    bool IsActive() override;
};

#endif
