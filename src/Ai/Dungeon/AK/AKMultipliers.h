/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AKMULTIPLIERS_H
#define PLAYERBOTS_AKMULTIPLIERS_H

#include "Multiplier.h"

class ElderNadoxMultiplier : public Multiplier
{
    public:
        ElderNadoxMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "elder nadox") {}

    public:
        float GetValue(Action* action) override;
};

class JedogaShadowseekerMultiplier : public Multiplier
{
    public:
        JedogaShadowseekerMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "jedoga shadowseeker") {}

    public:
        float GetValue(Action* action) override;
};

// Volazj Insanity: no attack on the unselectable boss or on a visage of another phase.
class VolazjInsanityMultiplier : public Multiplier
{
    public:
        VolazjInsanityMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "volazj insanity") {}

    public:
        float GetValue(Action* action) override;
};

// Jedoga's initiates are pulled by the room tactic ("jedoga initiates"), not by the careful planner, which counts
// them as one pack over its cap and walks away from the altar.
class JedogaRoomPullMultiplier : public Multiplier
{
    public:
        JedogaRoomPullMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "jedoga room pull") {}

    public:
        float GetValue(Action* action) override;
};

// [b33_ok] Amanitar: no AoE while the mushrooms stand round the boss (a hit Poisonous Mushroom clouds everyone within
// 5 yd, a Healthy one killed away from the group wastes its fungus), and a bot that walked to a Healthy Mushroom for
// Potent Fungus keeps it as its target until it dies.
class AmanitarMushroomMultiplier : public Multiplier
{
    public:
        AmanitarMushroomMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "amanitar mushroom") {}

    public:
        float GetValue(Action* action) override;
};

class ForgottenOneMultiplier : public Multiplier
{
    public:
        ForgottenOneMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "forgotten one") {}

    public:
        float GetValue(Action* action) override;
};

#endif
