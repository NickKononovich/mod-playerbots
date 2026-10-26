/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SVTRIGGERCONTEXT_H
#define PLAYERBOTS_SVTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "SVTriggers.h"

class TbcDungeonSteamvaultTriggerContext : public NamedObjectContext<Trigger>
{
public:
    TbcDungeonSteamvaultTriggerContext()
    {
        creators["kalithresh distiller"] = &TbcDungeonSteamvaultTriggerContext::kalithresh_distiller;
        creators["kalithresh spell reflection"] = &TbcDungeonSteamvaultTriggerContext::kalithresh_spell_reflection;
        creators["sv bog overlord loose"] = &TbcDungeonSteamvaultTriggerContext::sv_bog_overlord_loose;
    }

private:
    static Trigger* kalithresh_distiller(PlayerbotAI* botAI) { return new KalithreshDistillerTrigger(botAI); }
    static Trigger* sv_bog_overlord_loose(PlayerbotAI* botAI) { return new SvBogOverlordLooseTrigger(botAI); }
    static Trigger* kalithresh_spell_reflection(PlayerbotAI* botAI)
    {
        return new KalithreshSpellReflectionTrigger(botAI);
    }
};

#endif
