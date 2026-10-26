/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RTITRIGGERS_H
#define PLAYERBOTS_RTITRIGGERS_H

#include "GenericTriggers.h"
#include "Trigger.h"

class PlayerbotAI;

class NoRtiTrigger : public Trigger
{
public:
    NoRtiTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no rti target") {}

    bool IsActive() override;
};

class RtiCcTrigger : public HasCcTargetTrigger
{
public:
    RtiCcTrigger(PlayerbotAI* botAI, std::string const name) : HasCcTargetTrigger(botAI, name) {}

    bool IsActive() override;
};

// Early in a trash pull, this crowd-control bot's icon is free and the pack is big enough to spare a mob.
class NeedAutoCcMarkTrigger : public Trigger
{
public:
    NeedAutoCcMarkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "need auto cc mark") {}

    bool IsActive() override;
};

// The main tank's Skull or Cross on a trash pull is free, dead, or one of its own left on a mob out of the fight.
class NeedTankKillMarksTrigger : public Trigger
{
public:
    NeedTankKillMarksTrigger(PlayerbotAI* botAI) : Trigger(botAI, "need tank kill marks", 1000) {}

    bool IsActive() override;
};

// The unit this bot marked for crowd control should lose the mark: it died, the CC never landed or broke
// for good, or only held mobs are left and this one is the next to kill.
class StaleAutoCcMarkTrigger : public Trigger
{
public:
    StaleAutoCcMarkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "stale auto cc mark") {}

    bool IsActive() override;

private:
    bool IsLastHeldToRelease(ObjectGuid target);
};

// The mob this bot marked for crowd control is not held by anything yet, or broke free.
class AutoCcTargetFreeTrigger : public Trigger
{
public:
    AutoCcTargetFreeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "auto cc target free") {}

    bool IsActive() override;
};

// The tank announced a hard pull (PrePullCc) and this crowd control bot has not answered it yet, or still has a Sap
// or a trap to place. Out of combat only.
class PrePullCcTrigger : public Trigger
{
public:
    PrePullCcTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pre pull cc") {}

    bool IsActive() override;
};

// Out of combat, this bot's crowd control mark sits on an idle mob no pre-pull plan holds any more.
class StalePrePullCcMarkTrigger : public Trigger
{
public:
    StalePrePullCcMarkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "stale pre pull cc mark") {}

    bool IsActive() override;
};

// A mob is loose on a healer and nobody fights it: Blind (rogue) or Psychic Scream (a priest it beats on).
class EmergencyCcTrigger : public Trigger
{
public:
    EmergencyCcTrigger(PlayerbotAI* botAI) : Trigger(botAI, "emergency cc") {}

    bool IsActive() override;
};

// [b44_st] A member of the bot's group is charmed by an enemy (Hex of Jammal'an, Mind Control) and nothing holds it
// yet (AttackersValue::FindCharmedAlly): "control charmed member" holds it without damage.
class CharmedGroupMemberTrigger : public Trigger
{
public:
    CharmedGroupMemberTrigger(PlayerbotAI* botAI) : Trigger(botAI, "charmed group member") {}

    bool IsActive() override;
};

#endif
