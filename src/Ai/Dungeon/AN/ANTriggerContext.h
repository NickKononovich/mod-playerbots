/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ANTRIGGERCONTEXT_H
#define PLAYERBOTS_ANTRIGGERCONTEXT_H

#include "ANAddTactics.h"
#include "ANTriggers.h"
#include "NamedObjectContext.h"

class WotlkDungeonANTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonANTriggerContext()
        {
            creators["krik'thir web wrap"] = &WotlkDungeonANTriggerContext::krikthir_web_wrap;
            creators["krik'thir watchers"] = &WotlkDungeonANTriggerContext::krikthir_watchers;
            creators["anub'arak impale"] = &WotlkDungeonANTriggerContext::anubarak_impale;
            creators["hadronox platform"] = &WotlkDungeonANTriggerContext::hadronox_platform;
            creators["azjol-nerub lower kingdom"] = &WotlkDungeonANTriggerContext::azjol_nerub_lower_kingdom;
            creators["anub'arak pound"] = &WotlkDungeonANTriggerContext::anubarak_pound;
            creators["anub'arak frontal"] = &WotlkDungeonANTriggerContext::anubarak_frontal;
            creators["anub'ar skirmisher fixate"] = &WotlkDungeonANTriggerContext::skirmisher_fixate;
            creators["hadronox crusher"] = &WotlkDungeonANTriggerContext::hadronox_crusher;
            creators["hadronox boss"] = &WotlkDungeonANTriggerContext::hadronox_boss;
            creators["hadronox hold back"] = &WotlkDungeonANTriggerContext::hadronox_hold_back;  // [b33_hadronox2]
            creators["hadronox leech standoff"] =
                &WotlkDungeonANTriggerContext::hadronox_leech_standoff;  // [b33_hadronox2]
            creators["krik'thir watcher pull"] = &WotlkDungeonANTriggerContext::krikthir_watcher_pull;
            creators["krik'thir watcher drift"] = &WotlkDungeonANTriggerContext::krikthir_watcher_drift;
            creators["krik'thir watcher rally"] = &WotlkDungeonANTriggerContext::krikthir_watcher_rally;  // [b34_an2]
        }
    private:
        static Trigger* krikthir_web_wrap(PlayerbotAI* botAI) { return new KrikthirWebWrapTrigger(botAI); }
        static Trigger* krikthir_watchers(PlayerbotAI* botAI) { return new KrikthirWatchersTrigger(botAI); }
        static Trigger* anubarak_impale(PlayerbotAI* botAI) { return new AnubarakImpaleTrigger(botAI); }
        static Trigger* hadronox_platform(PlayerbotAI* botAI) { return new HadronoxPlatformTrigger(botAI); }
        static Trigger* azjol_nerub_lower_kingdom(PlayerbotAI* botAI)
        {
            return new AzjolNerubLowerKingdomTrigger(botAI);
        }
        static Trigger* anubarak_pound(PlayerbotAI* botAI) { return new AnubarakPoundTrigger(botAI); }
        static Trigger* anubarak_frontal(PlayerbotAI* botAI) { return new AnubarakFrontalTrigger(botAI); }
        static Trigger* skirmisher_fixate(PlayerbotAI* botAI) { return new SkirmisherFixateTrigger(botAI); }
        static Trigger* hadronox_crusher(PlayerbotAI* botAI) { return new HadronoxCrusherTrigger(botAI); }
        static Trigger* hadronox_boss(PlayerbotAI* botAI) { return new HadronoxBossTrigger(botAI); }
        // [b33_hadronox2]
        static Trigger* hadronox_hold_back(PlayerbotAI* botAI) { return new HadronoxHoldBackTrigger(botAI); }
        static Trigger* hadronox_leech_standoff(PlayerbotAI* botAI)
        {
            return new HadronoxLeechStandoffTrigger(botAI);
        }
        static Trigger* krikthir_watcher_pull(PlayerbotAI* botAI) { return new KrikthirWatcherPullTrigger(botAI); }
        static Trigger* krikthir_watcher_drift(PlayerbotAI* botAI)
        {
            return new KrikthirWatcherDriftTrigger(botAI);
        }
        // [b34_an2]
        static Trigger* krikthir_watcher_rally(PlayerbotAI* botAI)
        {
            return new KrikthirWatcherRallyTrigger(botAI);
        }
};

#endif
