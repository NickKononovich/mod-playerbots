/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHACTIONS_H
#define PLAYERBOTS_SHACTIONS_H

#include "TbcHazard.h"

class Player;

// The ring a healer (KARGATH_HEALER_RING) or a ranged damage dealer (KARGATH_RANGED_RING) keeps round Kargath.
float KargathRingFor(Player* bot);

// Blade Dance: out of 15 yd of where Kargath stands now (he charges on from there; the trigger fires again for the
// next spot). The cast under way is dropped: no cast is worth 5.7k a hit.
class KargathLeaveBladeDanceAction : public TbcHazardMoveAction
{
public:
    KargathLeaveBladeDanceAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "kargath leave blade dance") {}
    bool Execute(Event event) override;
};

// The healer and the ranged step back out to their ring round Kargath between the dances.
class KargathKeepRingAction : public TbcHazardMoveAction
{
public:
    KargathKeepRingAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "kargath keep ring") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
