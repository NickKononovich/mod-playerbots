/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFTRAVELCONTEXT_H
#define PLAYERBOTS_SELFTRAVELCONTEXT_H

#include "NamedObjectContext.h"
#include "SelfTravelActions.h"
#include "SelfTravelStrategy.h"

// The dead strategy runs its actions at 100 (PassThroughStrategy): a ghost's way to the portal comes before
// "find corpse" (100) and below "repop" (101).
constexpr float GHOST_PORTAL_RELEVANCE = 100.5f;

class SelfTravelStrategyContext : public NamedObjectContext<Strategy>
{
public:
    // No siblings: "instance portal" and "breathe" are added together (with siblings the second removed the first).
    SelfTravelStrategyContext() : NamedObjectContext<Strategy>(false, false)
    {
        creators["instance portal"] = &SelfTravelStrategyContext::instance_portal;
        creators["ghost portal"] = &SelfTravelStrategyContext::ghost_portal;
        creators["breathe"] = &SelfTravelStrategyContext::breathe;
    }

private:
    static Strategy* instance_portal(PlayerbotAI* botAI)
    {
        return new InstancePortalStrategy(botAI, "instance portal", ACTION_MOVE + 4.0f);
    }
    static Strategy* ghost_portal(PlayerbotAI* botAI)
    {
        return new InstancePortalStrategy(botAI, "ghost portal", GHOST_PORTAL_RELEVANCE);
    }
    static Strategy* breathe(PlayerbotAI* botAI) { return new BreatheStrategy(botAI); }
};

class SelfTravelTriggerContext : public NamedObjectContext<Trigger>
{
public:
    SelfTravelTriggerContext()
    {
        creators["instance portal"] = &SelfTravelTriggerContext::instance_portal;
        creators["low breath"] = &SelfTravelTriggerContext::low_breath;
    }

private:
    static Trigger* instance_portal(PlayerbotAI* botAI) { return new InstancePortalTrigger(botAI); }
    static Trigger* low_breath(PlayerbotAI* botAI) { return new LowBreathTrigger(botAI); }
};

class SelfTravelActionContext : public NamedObjectContext<Action>
{
public:
    SelfTravelActionContext()
    {
        creators["move to instance portal"] = &SelfTravelActionContext::move_to_instance_portal;
        creators["surface for air"] = &SelfTravelActionContext::surface_for_air;
    }

private:
    static Action* move_to_instance_portal(PlayerbotAI* botAI) { return new MoveToInstancePortalAction(botAI); }
    static Action* surface_for_air(PlayerbotAI* botAI) { return new SurfaceForAirAction(botAI); }
};

#endif
