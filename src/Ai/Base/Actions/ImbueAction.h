/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_IMBUEACTION_H
#define PLAYERBOTS_IMBUEACTION_H

#include "Action.h"

class PlayerbotAI;

// A temporary weapon enhancement (poison, sharpening stone, weight stone, oil), put on between fights only
// ([rjoin-imbue], ImbueAction.cpp): Execute() is true only when something was applied.
class ImbueAction : public Action
{
public:
    ImbueAction(PlayerbotAI* botAI, std::string const name, bool offhand)
        : Action(botAI, name), _offhand(offhand)
    {
    }

    bool isUseful() override;

protected:
    void PrepareImbue();
    void Tried(uint8 slot);  // [b34_gnomer] an apply was cast on this equipment slot; isUseful() verifies it took
    bool Done(bool applied);

private:
    bool _offhand;
    uint32 _retryAfterMs = 0;
    uint8 _triedSlotMask = 0;  // [b34_gnomer] bit 0 main hand, bit 1 off hand
    uint8 _failures = 0;       // [b34_gnomer] applies in a row that left the slot without an enhancement
};

class ImbueWithPoisonAction : public ImbueAction
{
public:
    ImbueWithPoisonAction(PlayerbotAI* botAI);

    bool Execute(Event event) override;
};

class ImbueWithStoneAction : public ImbueAction
{
public:
    ImbueWithStoneAction(PlayerbotAI* botAI);

    bool Execute(Event event) override;
};

class ImbueWithOilAction : public ImbueAction
{
public:
    ImbueWithOilAction(PlayerbotAI* botAI);

    bool Execute(Event event) override;
};

class TryEmergencyAction : public Action
{
public:
    TryEmergencyAction(PlayerbotAI* botAI);

    bool Execute(Event event) override;
};

#endif
