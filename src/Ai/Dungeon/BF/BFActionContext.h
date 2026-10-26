/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BFACTIONCONTEXT_H
#define PLAYERBOTS_BFACTIONCONTEXT_H

#include "BFActions.h"
#include "BFTechnician.h"
#include "NamedObjectContext.h"

class TbcDungeonBloodFurnaceActionContext : public NamedObjectContext<Action>
{
public:
    TbcDungeonBloodFurnaceActionContext()
    {
        creators["kelidan leave burning nova"] = &TbcDungeonBloodFurnaceActionContext::kelidan_leave_burning_nova;
        creators["broggok back off"] = &TbcDungeonBloodFurnaceActionContext::broggok_back_off;
        creators["attack shadowmoon technician"] =
            &TbcDungeonBloodFurnaceActionContext::attack_shadowmoon_technician;
    }

private:
    static Action* kelidan_leave_burning_nova(PlayerbotAI* botAI) { return new KelidanLeaveBurningNovaAction(botAI); }
    static Action* broggok_back_off(PlayerbotAI* botAI) { return new BroggokBackOffAction(botAI); }
    static Action* attack_shadowmoon_technician(PlayerbotAI* botAI)
    {
        return new AttackShadowmoonTechnicianAction(botAI);
    }
};

#endif
