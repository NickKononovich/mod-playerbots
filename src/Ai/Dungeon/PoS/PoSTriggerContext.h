/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_POSTRIGGERCONTEXT_H
#define PLAYERBOTS_POSTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "PoSTriggers.h"

class WotlkDungeonPoSTriggerContext : public NamedObjectContext<Trigger>
{
public:
    WotlkDungeonPoSTriggerContext()
    {
        creators["ick and krick"] = &WotlkDungeonPoSTriggerContext::ick_and_krick;
        creators["tyrannus"] = &WotlkDungeonPoSTriggerContext::tyrannus;
        creators["pos area trigger"] = &WotlkDungeonPoSTriggerContext::pos_area_trigger;
        creators["pos permafrost"] = &WotlkDungeonPoSTriggerContext::pos_permafrost;
        creators["pos throw saronite"] = &WotlkDungeonPoSTriggerContext::pos_throw_saronite;
        creators["pos icicle"] = &WotlkDungeonPoSTriggerContext::pos_icicle;
        creators["pos overlords brand"] = &WotlkDungeonPoSTriggerContext::pos_overlords_brand;
        creators["pos mark of rimefang"] = &WotlkDungeonPoSTriggerContext::pos_mark_of_rimefang;
        creators["pos tyrannus position"] = &WotlkDungeonPoSTriggerContext::pos_tyrannus_position;  // [b33_pos]
    }

private:
    static Trigger* ick_and_krick(PlayerbotAI* botAI) { return new IckAndKrickTrigger(botAI); }
    static Trigger* tyrannus(PlayerbotAI* botAI) { return new TyrannusTrigger(botAI); }
    static Trigger* pos_area_trigger(PlayerbotAI* botAI) { return new PoSAreaTriggerTrigger(botAI); }
    static Trigger* pos_permafrost(PlayerbotAI* botAI) { return new PoSPermafrostTrigger(botAI); }
    static Trigger* pos_throw_saronite(PlayerbotAI* botAI) { return new PoSThrowSaroniteTrigger(botAI); }
    static Trigger* pos_icicle(PlayerbotAI* botAI) { return new PoSIcicleTrigger(botAI); }
    static Trigger* pos_overlords_brand(PlayerbotAI* botAI) { return new PoSOverlordsBrandTrigger(botAI); }
    static Trigger* pos_mark_of_rimefang(PlayerbotAI* botAI) { return new PoSMarkOfRimefangTrigger(botAI); }
    static Trigger* pos_tyrannus_position(PlayerbotAI* botAI) { return new PoSTyrannusPositionTrigger(botAI); }
};

#endif
