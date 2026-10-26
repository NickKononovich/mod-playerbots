/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COSMULTIPLIERS_H
#define PLAYERBOTS_COSMULTIPLIERS_H

#include "Multiplier.h"

class EpochMultiplier : public Multiplier
{
    public:
        EpochMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "chrono-lord epoch") {}

    public:
        float GetValue(Action* action) override;
};

// The crates (CoSDispelCrateAction): the Disruptor's carrier does not mount for the short walks between them.
class CoSCrateMountMultiplier : public Multiplier
{
    public:
        CoSCrateMountMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "cos crate mount") {}

    public:
        float GetValue(Action* action) override;
};

// Heroic city waves (Guardian of Time running): idle mobs outside Arthas' event (CoSHelpers::IsSkippableEventTrash)
// are left standing. From the Town Hall to Mal'Ganis the group keeps pace with Arthas (heroic: the Guardian of Time
// timer): the planner's "aggressive target" leaves idle townsfolk, Risen Zombies and street packs alone and only
// takes a mob that already fights (the group or Arthas); Arthas walks into the street packs himself.
class CoSLastCityIdlePullMultiplier : public Multiplier
{
    public:
        CoSLastCityIdlePullMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "cos last city idle pull") {}

    public:
        float GetValue(Action* action) override;

    private:
        uint32 _logMs = 0;
};

// [b39_cos] Heroic, Guardian of Time running: no crowd control on Arthas' event (the waves, the last city): no
// pre-pull claims, no auto cc marks, no casts of a held mark. A held wave mob is a wave that is not done.
class CoSTimedRunCcMultiplier : public Multiplier
{
    public:
        CoSTimedRunCcMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "cos timed run cc") {}

    public:
        float GetValue(Action* action) override;
};

#endif
