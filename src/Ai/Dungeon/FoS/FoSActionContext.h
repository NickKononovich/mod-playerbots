/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOSACTIONCONTEXT_H
#define PLAYERBOTS_FOSACTIONCONTEXT_H

#include "Action.h"
#include "FoSActions.h"
#include "NamedObjectContext.h"

class WotlkDungeonFoSActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonFoSActionContext()
        {
            creators["move from bronjahm"] = &WotlkDungeonFoSActionContext::move_from_bronjahm;
            creators["attack corrupted soul fragment"] = &WotlkDungeonFoSActionContext::attack_corrupted_soul_fragment;
            creators["bronjahm group position"] = &WotlkDungeonFoSActionContext::bronjahm_group_position;
            creators["devourer of souls"] = &WotlkDungeonFoSActionContext::devourer_of_souls;
            creators["fos avoid wailing souls"] = &WotlkDungeonFoSActionContext::fos_avoid_wailing_souls;
            creators["fos interrupt"] = &WotlkDungeonFoSActionContext::fos_interrupt;
        }
    private:
        static Action* move_from_bronjahm(PlayerbotAI* botAI) { return new MoveFromBronjahmAction(botAI); }
        static Action* attack_corrupted_soul_fragment(PlayerbotAI* botAI) { return new AttackCorruptedSoulFragmentAction(botAI); }
        static Action* bronjahm_group_position(PlayerbotAI* botAI) { return new BronjahmGroupPositionAction(botAI); }
        static Action* devourer_of_souls(PlayerbotAI* botAI) { return new DevourerOfSoulsAction(botAI); }
        static Action* fos_avoid_wailing_souls(PlayerbotAI* botAI) { return new AvoidWailingSoulsAction(botAI); }
        static Action* fos_interrupt(PlayerbotAI* botAI) { return new FoSInterruptAction(botAI); }
};

#endif
