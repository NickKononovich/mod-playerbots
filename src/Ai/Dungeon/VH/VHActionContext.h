/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_VHACTIONCONTEXT_H
#define PLAYERBOTS_VHACTIONCONTEXT_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "VHActions.h"

class WotlkDungeonVHActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonVHActionContext() {
            creators["attack erekem"] = &WotlkDungeonVHActionContext::attack_erekem;
            creators["attack ichor globule"] = &WotlkDungeonVHActionContext::attack_ichor_globule;
            creators["attack void sentry"] = &WotlkDungeonVHActionContext::attack_void_sentry;
            creators["vh start event"] = &WotlkDungeonVHActionContext::vh_start_event;
            creators["vh attack seal attacker"] = &WotlkDungeonVHActionContext::vh_attack_seal_attacker;
            creators["vh attack portal keeper"] = &WotlkDungeonVHActionContext::vh_attack_portal_keeper;
            creators["vh engage released boss"] = &WotlkDungeonVHActionContext::vh_engage_released_boss;
            creators["vh rally"] = &WotlkDungeonVHActionContext::vh_rally;
            creators["xevozz kite"] = &WotlkDungeonVHActionContext::xevozz_kite;
            creators["vh late join"] = &WotlkDungeonVHActionContext::vh_late_join;
        }
    private:
        static Action* attack_erekem(PlayerbotAI* botAI) { return new AttackErekemAction(botAI); }
        static Action* attack_ichor_globule(PlayerbotAI* botAI) { return new AttackIchorGlobuleAction(botAI); }
        static Action* attack_void_sentry(PlayerbotAI* botAI) { return new AttackVoidSentryAction(botAI); }
        static Action* vh_start_event(PlayerbotAI* botAI) { return new VHStartEventAction(botAI); }
        static Action* vh_attack_seal_attacker(PlayerbotAI* botAI) { return new VHAttackSealAttackerAction(botAI); }
        static Action* vh_attack_portal_keeper(PlayerbotAI* botAI) { return new VHAttackPortalKeeperAction(botAI); }
        static Action* vh_engage_released_boss(PlayerbotAI* botAI) { return new VHEngageReleasedBossAction(botAI); }
        static Action* vh_rally(PlayerbotAI* botAI) { return new VHRallyAction(botAI); }
        static Action* xevozz_kite(PlayerbotAI* botAI) { return new XevozzKiteAction(botAI); }
        static Action* vh_late_join(PlayerbotAI* botAI) { return new VHLateJoinAction(botAI); }
};

#endif
