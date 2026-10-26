/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MGTTRIGGERS_H
#define PLAYERBOTS_MGTTRIGGERS_H

#include "MgTShared.h"
#include "Trigger.h"

class MgTCrystalActiveTrigger : public Trigger
{
public:
    MgTCrystalActiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt crystal active") {}
    bool IsActive() override;
};

class MgTOutOfRoomTrigger : public Trigger
{
public:
    MgTOutOfRoomTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt out of room") {}
    bool IsActive() override;
};

class MgTInDampeningFieldTrigger : public Trigger
{
public:
    MgTInDampeningFieldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt in dampening field") {}
    bool IsActive() override;
};

class MgTMageGuardAtRangeTrigger : public Trigger
{
public:
    MgTMageGuardAtRangeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt mage guard at range") {}
    bool IsActive() override;
};

class MgTArcaneNovaRangeTrigger : public Trigger
{
public:
    MgTArcaneNovaRangeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt arcane nova range") {}
    bool IsActive() override;
};

class MgTPriorityInterruptTrigger : public Trigger
{
public:
    MgTPriorityInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt priority interrupt") {}
    bool IsActive() override;
};

class MgTFocusTargetTrigger : public Trigger
{
public:
    MgTFocusTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt focus target") {}
    bool IsActive() override;
};

class MgTEnragedWretchedTrigger : public Trigger
{
public:
    MgTEnragedWretchedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt enraged wretched") {}
    bool IsActive() override;
};

class MgTDelrissaInterruptTrigger : public Trigger
{
public:
    MgTDelrissaInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt delrissa interrupt") {}
    bool IsActive() override;
};

class MgTDelrissaFocusTargetTrigger : public Trigger
{
public:
    MgTDelrissaFocusTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt delrissa focus target") {}
    bool IsActive() override;
};

class MgTDelrissaHealerSpotTrigger : public Trigger
{
public:
    MgTDelrissaHealerSpotTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt delrissa healer spot") {}
    bool IsActive() override;
};

// A crowd control bot (mage, hunter, warlock) whose CC icon is free while Delrissa's retinue is engaged and fewer
// than two of it are held.
// [delrissa_retinue_left] the tank, out of combat and the group ready: a lackey outlived Delrissa (MgTShared.h)
class MgTDelrissaRetinueLeftTrigger : public Trigger
{
public:
    MgTDelrissaRetinueLeftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt delrissa retinue left", 2000) {}
    bool IsActive() override;
};

class MgTDelrissaCcMarkTrigger : public Trigger
{
public:
    MgTDelrissaCcMarkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt delrissa cc mark", 1000) {}
    bool IsActive() override;
};

class MgTDelrissaTremorTotemTrigger : public Trigger
{
public:
    MgTDelrissaTremorTotemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt delrissa tremor totem") {}
    bool IsActive() override;
};

class MgTFlameStrikeTrigger : public Trigger
{
public:
    MgTFlameStrikeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt flame strike") {}
    bool IsActive() override;
};

class MgTPhoenixBurnTrigger : public Trigger
{
public:
    MgTPhoenixBurnTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt phoenix burn") {}
    bool IsActive() override;
};

class MgTKaelFocusTargetTrigger : public Trigger
{
public:
    MgTKaelFocusTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt kael focus target") {}
    bool IsActive() override;
};

class MgTKaelInterruptTrigger : public Trigger
{
public:
    MgTKaelInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt kael interrupt") {}
    bool IsActive() override;
};

// Selin's room (MagistersTerrace::SelinRoom, MgTShared.h): the pulling tank out of combat, the room holding the
// group, a pull spot found and no pull of its own under way.
class MgTSelinRoomPullTrigger : public Trigger
{
public:
    MgTSelinRoomPullTrigger(PlayerbotAI* botAI, std::string const name = "mgt selin room pull") : Trigger(botAI, name)
    {
    }
    bool IsActive() override;
};

// The same pull with Selin inert and Wretched of hers alive: every member stays in her zone-wide combat, where the
// pull's base relevance (4.5) loses to the combat engine's defaults (mgt-h/mgt-n 2026-10-25 11:20: no room pull for
// 160 s after the pair died).
class MgTSelinRoomPullInertTrigger : public MgTSelinRoomPullTrigger
{
public:
    MgTSelinRoomPullInertTrigger(PlayerbotAI* botAI) : MgTSelinRoomPullTrigger(botAI, "mgt selin room pull inert") {}
    bool IsActive() override;
};

// [selin pet] A pet of the bot stands in Selin's room or is her victim while her room is cleared (MgTShared.h).
class MgTSelinPetOutTrigger : public Trigger
{
public:
    MgTSelinPetOutTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt selin pet out") {}
    bool IsActive() override;
};

// Another member out of combat away from the rally while the room waits for its next pull.
class MgTSelinRoomGatherTrigger : public Trigger
{
public:
    MgTSelinRoomGatherTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt selin room gather") {}
    bool IsActive() override;
};

// A member stands in an idle room pack's aggro reach (following, chasing, positioning).
class MgTSelinRoomDriftTrigger : public Trigger
{
public:
    MgTSelinRoomDriftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt selin room drift") {}
    bool IsActive() override;
};

// The tank fights a room pack away from the rally while another is idle, or a member stands in her room while she is
// engaged and a pack of hers is idle.
class MgTSelinRoomRallyTrigger : public Trigger
{
public:
    MgTSelinRoomRallyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt selin room rally") {}
    bool IsActive() override;
};

// Selin's room entry order (MgTShared.h, SelinRoom::LEASH_EDGE_X): while a Wretched of hers is alive the bot stands
// past the leash edge, her fight not committed (SelinRoom::ClearingRoom).
class MgTSelinRoomLeashTrigger : public Trigger
{
public:
    MgTSelinRoomLeashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt selin room leash") {}
    bool IsActive() override;
};

// The tank with a drinking Wretched on it, or a shaman in a fight with four or more Wretched (MgTShared.h).
class MgTWretchedInfusionTrigger : public Trigger
{
public:
    MgTWretchedInfusionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt wretched infusion") {}
    bool IsActive() override;
};

class MgTGravityLapseTrigger : public Trigger
{
public:
    MgTGravityLapseTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mgt gravity lapse") {}
    bool IsActive() override;
};

#endif
