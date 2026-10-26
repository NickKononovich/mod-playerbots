/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPTRIGGERCONTEXT_H
#define PLAYERBOTS_SPTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "SPTriggers.h"

class TbcDungeonSlavePensTriggerContext : public NamedObjectContext<Trigger>
{
public:
    TbcDungeonSlavePensTriggerContext()
    {
        creators["mennu nova totem"] = &TbcDungeonSlavePensTriggerContext::mennu_nova_totem;
        creators["mennu totems up"] = &TbcDungeonSlavePensTriggerContext::mennu_totems_up;
    }

private:
    static Trigger* mennu_nova_totem(PlayerbotAI* botAI) { return new MennuNovaTotemTrigger(botAI); }
    static Trigger* mennu_totems_up(PlayerbotAI* botAI) { return new MennuTotemsUpTrigger(botAI); }
};

#endif
