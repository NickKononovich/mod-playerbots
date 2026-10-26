/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DTKTRIGGERCONTEXT_H
#define PLAYERBOTS_DTKTRIGGERCONTEXT_H

#include "DTKTriggers.h"
#include "GenericTriggers.h"
#include "NamedObjectContext.h"

class WotlkDungeonDTKTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        WotlkDungeonDTKTriggerContext()
        {
            creators["corpse explode"] = &WotlkDungeonDTKTriggerContext::corpse_explode;
            creators["trollgore out of room"] = &WotlkDungeonDTKTriggerContext::trollgore_out_of_room;
            creators["arcane field"] = &WotlkDungeonDTKTriggerContext::arcane_field;
            creators["in arcane field"] = &WotlkDungeonDTKTriggerContext::in_arcane_field;
            // creators["crystal handler"] = &WotlkDungeonDTKTriggerContext::crystal_handler;
            creators["gift of tharon'ja"] = &WotlkDungeonDTKTriggerContext::gift_of_tharonja;
            creators["tharon'ja out of melee"] = &WotlkDungeonDTKTriggerContext::tharonja_out_of_melee;
            creators["novos summon target targeted"] = &WotlkDungeonDTKTriggerContext::novos_summon_target_targeted;
            creators["novos pull"] = &WotlkDungeonDTKTriggerContext::novos_pull;
            creators["king dred fear guard"] = &WotlkDungeonDTKTriggerContext::king_dred_fear_guard;
            creators["king dred grievous bite"] = &WotlkDungeonDTKTriggerContext::king_dred_grievous_bite;
        }
    private:
        static Trigger* corpse_explode(PlayerbotAI* botAI) { return new CorpseExplodeTrigger(botAI); }
        static Trigger* trollgore_out_of_room(PlayerbotAI* botAI) { return new TrollgoreOutOfRoomTrigger(botAI); }
        static Trigger* arcane_field(PlayerbotAI* botAI) { return new ArcaneFieldTrigger(botAI); }
        static Trigger* in_arcane_field(PlayerbotAI* botAI) { return new InArcaneFieldTrigger(botAI); }
        // static Trigger* crystal_handler(PlayerbotAI* botAI) { return new CrystalHandlerTrigger(botAI); }
        static Trigger* gift_of_tharonja(PlayerbotAI* botAI) { return new GiftOfTharonjaTrigger(botAI); }
        static Trigger* novos_pull(PlayerbotAI* botAI) { return new NovosPullTrigger(botAI); }
        static Trigger* king_dred_fear_guard(PlayerbotAI* botAI) { return new KingDredFearGuardTrigger(botAI); }
        static Trigger* king_dred_grievous_bite(PlayerbotAI* botAI) { return new KingDredGrievousBiteTrigger(botAI); }
        static Trigger* novos_summon_target_targeted(PlayerbotAI* botAI)
        {
            return new NovosSummonTargetTargetedTrigger(botAI);
        }
        static Trigger* tharonja_out_of_melee(PlayerbotAI* botAI)
        {
            return new TwoTriggers(botAI, "gift of tharon'ja", "enemy out of melee");
        }
};

#endif
