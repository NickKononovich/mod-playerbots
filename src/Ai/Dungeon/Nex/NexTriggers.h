/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEXTRIGGERS_H
#define PLAYERBOTS_NEXTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

#include <array>

enum NexusIDs
{
    // Faction Commander
    NPC_ALLIANCE_COMMANDER          = 27949,
    NPC_HORDE_COMMANDER             = 27947,
    NPC_COMMANDER_STOUTBEARD        = 26796,
    NPC_COMMANDER_KOLURG            = 26798,
    // SPELL_FRIGHTENING_SHOUT         = 19134,
    SPELL_WHIRLWIND                 = 38618,

    // Grand Magus Telestra
    NPC_TELESTRA                    = 26731,
    NPC_FIRE_MAGUS                  = 26928,
    NPC_FROST_MAGUS                 = 26930,
    NPC_ARCANE_MAGUS                = 26929,

    // Anomalus
    BUFF_RIFT_SHIELD                = 47748,

    // Ormorok the Tree Shaper
    // NPC_CRYSTAL_SPIKE               = 27099,
    GO_CRYSTAL_SPIKE                = 188537,
};

namespace NexIds
{
// nexus.h
constexpr uint32 DATA_MAGUS_TELESTRA_EVENT = 0;
constexpr uint32 DATA_ANOMALUS_EVENT = 1;
constexpr uint32 DATA_ORMOROK_EVENT = 2;
constexpr uint32 DATA_KERISTRASZA_EVENT = 3;
constexpr uint32 DATA_TELESTRA_ORB = 5;
constexpr uint32 DATA_ANOMALUS_ORB = 6;
constexpr uint32 DATA_ORMOROK_ORB = 7;
constexpr uint32 GO_TELESTRA_SPHERE = 188526;
constexpr uint32 GO_ANOMALUS_SPHERE = 188527;
constexpr uint32 GO_ORMOROK_SPHERE = 188528;
// boss_anomalus.cpp: each rift keeps summoning Crazed Mana-Wraiths until it dies
constexpr uint32 NPC_CHAOTIC_RIFT = 26918;

// Walk to the spawn first, then look the sphere up nearby (far grids may not hold it yet).
constexpr float SPHERE_SEARCH_RANGE = 15.0f;
constexpr float SPHERE_USE_DISTANCE = 2.0f;

struct ContainmentSphere
{
    uint32 bossData;
    uint32 orbData;
    uint32 goEntry;
    float x;
    float y;
    float z;
};

// gameobject spawns 65547-65549 (acore_world)
constexpr std::array<ContainmentSphere, 3> CONTAINMENT_SPHERES = {{
    { DATA_MAGUS_TELESTRA_EVENT, DATA_TELESTRA_ORB, GO_TELESTRA_SPHERE, 281.923f, -25.4521f, -16.9398f },
    { DATA_ANOMALUS_EVENT, DATA_ANOMALUS_ORB, GO_ANOMALUS_SPHERE, 322.153f, 14.6832f, -16.8019f },
    { DATA_ORMOROK_EVENT, DATA_ORMOROK_ORB, GO_ORMOROK_SPHERE, 281.783f, 15.2074f, -16.621f },
}};

// Nearest sphere whose boss is dead and whose orb is not used yet (instance_nexus.cpp makes it selectable only
// then); nullptr when none is left or Keristrasza is already dead.
ContainmentSphere const* FindReadySphere(Player* bot);

// A living Chaotic Rift among the bot's possible targets, nullptr when none.
Unit* FindChaoticRift(PlayerbotAI* botAI);
}  // namespace NexIds

class FactionCommanderWhirlwindTrigger : public Trigger
{
public:
    FactionCommanderWhirlwindTrigger(PlayerbotAI* botAI) : Trigger(botAI, "faction commander whirlwind") {}
    bool IsActive() override;
};

class TelestraFirebombTrigger : public Trigger
{
public:
    TelestraFirebombTrigger(PlayerbotAI* botAI) : Trigger(botAI, "telestra firebomb spread") {}
    bool IsActive() override;
};

class TelestraSplitPhaseTrigger : public Trigger
{
public:
    TelestraSplitPhaseTrigger(PlayerbotAI* botAI) : Trigger(botAI, "telestra split phase") {}
    bool IsActive() override;
};

class ChaoticRiftTrigger : public Trigger
{
public:
    ChaoticRiftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "chaotic rift") {}
    bool IsActive() override;
};

class OrmorokSpikesTrigger : public Trigger
{
public:
    OrmorokSpikesTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ormorok spikes") {}
    bool IsActive() override;
};

class OrmorokStackTrigger : public Trigger
{
public:
    OrmorokStackTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ormorok stack") {}
    bool IsActive() override;
};

class IntenseColdTrigger : public Trigger
{
public:
    IntenseColdTrigger(PlayerbotAI* botAI) : Trigger(botAI, "intense cold") {}
    bool IsActive() override;
};

class NexusContainmentSphereTrigger : public Trigger
{
public:
    NexusContainmentSphereTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nexus containment sphere") {}
    bool IsActive() override;
};

class KeristraszaPositioningTrigger : public Trigger
{
public:
    KeristraszaPositioningTrigger(PlayerbotAI* botAI) : Trigger(botAI, "keristrasza positioning") {}
    bool IsActive() override;
};

#endif
