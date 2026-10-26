/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ANACTIONCONTEXT_H
#define PLAYERBOTS_ANACTIONCONTEXT_H

#include "ANActions.h"
#include "ANAddTactics.h"
#include "Action.h"
#include "NamedObjectContext.h"

class WotlkDungeonANActionContext : public NamedObjectContext<Action>
{
    public:
        WotlkDungeonANActionContext() {
            creators["attack web wrap"] = &WotlkDungeonANActionContext::attack_web_wrap;
            creators["krik'thir priority"] = &WotlkDungeonANActionContext::krikthir_priority;
            creators["dodge pound"] = &WotlkDungeonANActionContext::dodge_pound;
            creators["anub'arak flank"] = &WotlkDungeonANActionContext::anubarak_flank;
            creators["avoid anub'arak impale"] = &WotlkDungeonANActionContext::avoid_anubarak_impale;
            creators["hadronox return to platform"] = &WotlkDungeonANActionContext::hadronox_return_to_platform;
            creators["drop to lower kingdom"] = &WotlkDungeonANActionContext::drop_to_lower_kingdom;
            creators["face fixating skirmisher"] = &WotlkDungeonANActionContext::face_fixating_skirmisher;
            creators["hadronox attack crusher"] = &WotlkDungeonANActionContext::hadronox_attack_crusher;
            creators["hadronox attack boss"] = &WotlkDungeonANActionContext::hadronox_attack_boss;
            creators["hadronox hold back"] = &WotlkDungeonANActionContext::hadronox_hold_back;  // [b33_hadronox2]
            creators["hadronox leech standoff"] =
                &WotlkDungeonANActionContext::hadronox_leech_standoff;  // [b33_hadronox2]
            creators["krik'thir watcher pull"] = &WotlkDungeonANActionContext::krikthir_watcher_pull;
            creators["krik'thir back to rally"] = &WotlkDungeonANActionContext::krikthir_back_to_rally;
            creators["krik'thir event rally"] = &WotlkDungeonANActionContext::krikthir_event_rally;  // [b34_an2]
        }
    private:
        static Action* attack_web_wrap(PlayerbotAI* botAI) { return new AttackWebWrapAction(botAI); }
        static Action* krikthir_priority(PlayerbotAI* botAI) { return new WatchersTargetAction(botAI); }
        static Action* dodge_pound(PlayerbotAI* botAI) { return new AnubarakDodgePoundAction(botAI); }
        static Action* anubarak_flank(PlayerbotAI* botAI) { return new AnubarakFlankAction(botAI); }
        static Action* avoid_anubarak_impale(PlayerbotAI* botAI) { return new AvoidAnubarakImpaleAction(botAI); }
        static Action* hadronox_return_to_platform(PlayerbotAI* botAI)
        {
            return new HadronoxReturnToPlatformAction(botAI);
        }
        static Action* drop_to_lower_kingdom(PlayerbotAI* botAI) { return new DropToLowerKingdomAction(botAI); }
        static Action* face_fixating_skirmisher(PlayerbotAI* botAI)
        {
            return new FaceFixatingSkirmisherAction(botAI);
        }
        static Action* hadronox_attack_crusher(PlayerbotAI* botAI)
        {
            return new HadronoxAttackCrusherAction(botAI);
        }
        static Action* hadronox_attack_boss(PlayerbotAI* botAI) { return new HadronoxAttackBossAction(botAI); }
        // [b33_hadronox2]
        static Action* hadronox_hold_back(PlayerbotAI* botAI) { return new HadronoxHoldBackAction(botAI); }
        static Action* hadronox_leech_standoff(PlayerbotAI* botAI)
        {
            return new HadronoxLeechStandoffAction(botAI);
        }
        static Action* krikthir_watcher_pull(PlayerbotAI* botAI) { return new KrikthirWatcherPullAction(botAI); }
        static Action* krikthir_back_to_rally(PlayerbotAI* botAI) { return new KrikthirBackToRallyAction(botAI); }
        // [b34_an2]
        static Action* krikthir_event_rally(PlayerbotAI* botAI)
        {
            return new KrikthirBackToRallyAction(botAI, "krik'thir event rally");
        }
};

#endif
