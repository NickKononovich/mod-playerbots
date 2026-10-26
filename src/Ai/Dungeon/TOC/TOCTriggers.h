/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TOCTRIGGERS_H
#define PLAYERBOTS_TOCTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

class Creature;

enum TocC_IDs
{
    // Spells

    // Horse
    SPELL_DEFEND                    = 66482,
    SPELL_BOSS_DEFEND               = 62719,
    SPELL_MINIONS_DEFEND            = 64100,

    //Eadric
    SPELL_RADIANCE                  = 66935,

    // Objects
    OBJECT_LANCE_RACK               = 196398,

    // Items
    ITEM_LANCE                      = 46106,

    // Vehicles
    NPC_ARGENT_WARHORSE             = 35644,
    NPC_ARGENT_BATTLEWORG           = 36558,

    // Horde Champions
    NPC_MOKRA                       = 35572,
    NPC_ERESSEA                     = 35569,
    NPC_RUNOK                       = 35571,
    NPC_ZULTORE                     = 35570,
    NPC_VISCERI                     = 35617,

    // Alliance Champions
    NPC_JACOB                       = 34705,
    NPC_AMBROSE                     = 34702,
    NPC_COLOSOS                     = 34701,
    NPC_JAELYNE                     = 34657,
    NPC_LANA                        = 34703,

    // Grand Champion Minions
    NPC_IRONFORGE_MINION            = 35329,
    NPC_STORMWIND_MINION            = 35328,
    NPC_GNOMEREGAN_MINION           = 35331,
    NPC_EXODAR_MINION               = 35330,
    NPC_DARNASSUS_MINION            = 35332,
    NPC_ORGRIMMAR_MINION            = 35314,
    NPC_SILVERMOON_MINION           = 35326,
    NPC_THUNDER_BLUFF_MINION        = 35325,
    NPC_SENJIN_MINION               = 35323,
    NPC_UNDERCITY_MINION            = 35327,

    // Rest of the bosses and npcs
    NPC_EADRIC                      = 35119,
    NPC_EADRIC_H                    = 35518,
    NPC_PALETRESS                   = 34928,
    NPC_PALETRESS_H                 = 35517,

    NPC_ARGENT_LIGHTWIELDER         = 35309,
    NPC_ARGENT_MONK                 = 35305,
    NPC_PRIESTESS                   = 35307,

    NPC_BLACK_KNIGHT                = 35451,

    NPC_JAEREN                      = 35004,  // Horde
    NPC_ARELAS                      = 35005,  // Alliance
    NPC_RISEN_JAEREN                = 35545,
    NPC_RISEN_ARELAS                = 35564,
    NPC_TIRION                      = 33628,  // Possibly wrong npc here, 34996 had already populated creature_text (from ToC25) that
                                              // matches. Needs a sniff to confirm.

};

const std::vector<uint32> availableChampions = {
    NPC_MOKRA,   NPC_ERESSEA, NPC_RUNOK,   NPC_ZULTORE, NPC_VISCERI,
    NPC_AMBROSE, NPC_COLOSOS, NPC_JAELYNE, NPC_LANA,    NPC_JACOB,
};

const std::vector<uint32> availableTargets = {
    NPC_MOKRA,            NPC_ERESSEA,           NPC_RUNOK,                NPC_ZULTORE,          NPC_VISCERI,
    NPC_AMBROSE,          NPC_COLOSOS,           NPC_JAELYNE,              NPC_LANA,             NPC_JACOB,
    NPC_STORMWIND_MINION, NPC_GNOMEREGAN_MINION, NPC_EXODAR_MINION,        NPC_DARNASSUS_MINION, NPC_IRONFORGE_MINION,
    NPC_ORGRIMMAR_MINION, NPC_SILVERMOON_MINION, NPC_THUNDER_BLUFF_MINION, NPC_SENJIN_MINION,    NPC_UNDERCITY_MINION,
};

class ToCLanceTrigger : public Trigger
{
public:
    ToCLanceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc lance", 500) {}
    bool IsActive() override;
};

class ToCUELanceTrigger : public Trigger
{
public:
    ToCUELanceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc ue lance", 500) {}
    bool IsActive() override;
};

class ToCMountedTrigger : public Trigger
{
public:
    ToCMountedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc mounted") {}
    bool IsActive() override;
};

class ToCMountNearTrigger : public Trigger
{
public:
    ToCMountNearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc mount near", 500) {}
    bool IsActive() override;
};

class ToCEadricTrigger : public Trigger
{
public:
    ToCEadricTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc eadric") {}
    bool IsActive() override;
};

// The announcer offers the next stage and the group is ready for it (joust: everyone sits on a joust mount).
class ToCStartEventTrigger : public Trigger
{
public:
    ToCStartEventTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc start event", 1000) {}
    bool IsActive() override;

private:
    bool IsGroupReady(Creature* announcer, uint32 progress);

    uint32 readySince = 0;
    uint32 waitingSince = 0;
};

// Argent soldiers stand passive after the gossip (their DoZoneInCombat is commented out): the tank pulls each pack.
class ToCArgentSoldiersTrigger : public Trigger
{
public:
    ToCArgentSoldiersTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc argent soldiers", 1000) {}
    bool IsActive() override;
};

// Bot (or its pet) still damages Paletress through her Reflective Shield.
class ToCReflectiveShieldTrigger : public Trigger
{
public:
    ToCReflectiveShieldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc reflective shield") {}
    bool IsActive() override;
};

// Renew / Holy Fire / Holy Smite of Paletress, Waking Nightmare of the Memory, Death's Respite of the Black
// Knight, while the bot has an interrupt ready that the caster is not immune to.
class ToCInterruptTrigger : public Trigger
{
public:
    ToCInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc interrupt", 250) {}
    bool IsActive() override;
};

// A Black Knight ghoul within its Explode radius is casting Explode.
class ToCGhoulExplodeTrigger : public Trigger
{
public:
    ToCGhoulExplodeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc ghoul explode", 250) {}
    bool IsActive() override;
};

// Paletress or her Memory carries Paletress's Renew and the bot (not a healer) can purge it.
class ToCPaletressRenewTrigger : public Trigger
{
public:
    ToCPaletressRenewTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc paletress renew", 500) {}
    bool IsActive() override;
};

// The Black Knight marked the bot for death and a personal defensive is ready.
class ToCMarkedForDeathTrigger : public Trigger
{
public:
    ToCMarkedForDeathTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc marked for death", 250) {}
    bool IsActive() override;
};

// A healer sees a group member under Marked For Death losing health.
class ToCMarkedForDeathHealTrigger : public Trigger
{
public:
    ToCMarkedForDeathHealTrigger(PlayerbotAI* botAI) : Trigger(botAI, "toc marked for death heal", 500) {}
    bool IsActive() override;
};

#endif
