/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_EOETRIGGERCONTEXT_H
#define PLAYERBOTS_EOETRIGGERCONTEXT_H

#include "EoETriggers.h"
#include "NamedObjectContext.h"

class RaidEoETriggerContext : public NamedObjectContext<Trigger>
{
public:
    RaidEoETriggerContext()
    {
        creators["malygos"] = &RaidEoETriggerContext::malygos;
        creators["power spark"] = &RaidEoETriggerContext::power_spark;
        creators["eoe drake flying"] = &RaidEoETriggerContext::eoe_drake_flying;
    }

private:
    static Trigger* power_spark(PlayerbotAI* botAI) { return new PowerSparkTrigger(botAI); }
    static Trigger* malygos(PlayerbotAI* botAI) { return new MalygosTrigger(botAI); }
    static Trigger* eoe_drake_flying(PlayerbotAI* botAI) { return new EoEDrakeFlyingTrigger(botAI); }
};

#endif
