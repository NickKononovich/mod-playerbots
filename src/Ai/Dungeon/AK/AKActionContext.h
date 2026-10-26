/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AKACTIONCONTEXT_H
#define PLAYERBOTS_AKACTIONCONTEXT_H

#include "AKActions.h"
#include "Action.h"
#include "NamedObjectContext.h"

class WotlkDungeonOKActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonOKActionContext() {
            creators["attack nadox guardian"] = &WotlkDungeonOKActionContext::attack_nadox_guardian;
            creators["attack jedoga volunteer"] = &WotlkDungeonOKActionContext::attack_jedoga_volunteer;
            creators["avoid shadow crash"] = &WotlkDungeonOKActionContext::avoid_shadow_crash;
            creators["attack twisted visage"] = &WotlkDungeonOKActionContext::attack_twisted_visage;
            creators["use nerubian device"] = &WotlkDungeonOKActionContext::use_nerubian_device;
            creators["attack jedoga initiate"] = &WotlkDungeonOKActionContext::attack_jedoga_initiate;
            creators["amanitar potent fungus"] = &WotlkDungeonOKActionContext::amanitar_potent_fungus;  // [b33_ok]
        }
    private:
        static Action* attack_nadox_guardian(PlayerbotAI* botAI) { return new AttackNadoxGuardianAction(botAI); }
        static Action* attack_jedoga_volunteer(PlayerbotAI* botAI) { return new AttackJedogaVolunteerAction(botAI); }
        static Action* avoid_shadow_crash(PlayerbotAI* botAI) { return new AvoidShadowCrashAction(botAI); }
        static Action* attack_twisted_visage(PlayerbotAI* botAI) { return new VolazjAttackVisageAction(botAI); }
        static Action* use_nerubian_device(PlayerbotAI* botAI) { return new UseNerubianDeviceAction(botAI); }
        static Action* attack_jedoga_initiate(PlayerbotAI* botAI) { return new AttackJedogaInitiateAction(botAI); }
        static Action* amanitar_potent_fungus(PlayerbotAI* botAI) { return new AmanitarPotentFungusAction(botAI); }
};

#endif
