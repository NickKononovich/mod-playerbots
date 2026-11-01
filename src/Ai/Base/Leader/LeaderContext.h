/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADERCONTEXT_H
#define PLAYERBOTS_LEADERCONTEXT_H

#include "ChatCommandTrigger.h"
#include "ConsumableActions.h"
#include "LeaderActions.h"
#include "LeaderTriggers.h"
#include "LeaderValues.h"
#include "NamedObjectContext.h"
#include "RaidLeaderStrategy.h"

class LeaderStrategyContext : public NamedObjectContext<Strategy>
{
public:
    LeaderStrategyContext()
    {
        creators["raid leader"] = &LeaderStrategyContext::raid_leader;
        creators["raid consumables"] = &LeaderStrategyContext::raid_consumables;
    }

private:
    static Strategy* raid_leader(PlayerbotAI* botAI) { return new RaidLeaderStrategy(botAI); }
    static Strategy* raid_consumables(PlayerbotAI* botAI) { return new RaidConsumablesStrategy(botAI); }
};

class LeaderActionContext : public NamedObjectContext<Action>
{
public:
    LeaderActionContext()
    {
        creators["leader ready check"] = &LeaderActionContext::leader_ready_check;
        creators["leader ready check result"] = &LeaderActionContext::leader_ready_check_result;
        creators["leader encounter event"] = &LeaderActionContext::leader_encounter_event;
        creators["leader assign tanks"] = &LeaderActionContext::leader_assign_tanks;
        creators["use raid flask"] = &LeaderActionContext::use_raid_flask;
        creators["use food buff"] = &LeaderActionContext::use_food_buff;
        creators["use pre pull potion"] = &LeaderActionContext::use_pre_pull_potion;
        creators["use soulwell"] = &LeaderActionContext::use_soulwell;
        creators["click soul portal"] = &LeaderActionContext::click_soul_portal;
        creators["group pause"] = &LeaderActionContext::group_pause;
        creators["group resume"] = &LeaderActionContext::group_resume;
        creators["rest"] = &LeaderActionContext::rest;
        creators["interrupt order"] = &LeaderActionContext::interrupt_order;
        creators["resurrect first"] = &LeaderActionContext::resurrect_first;
        creators["burst order"] = &LeaderActionContext::burst_order;
        creators["burst end"] = &LeaderActionContext::burst_end;
        creators["wipe escape"] = &LeaderActionContext::wipe_escape;
        creators["wipe escape end"] = &LeaderActionContext::wipe_escape_end;
        creators["leader move order"] = &LeaderActionContext::leader_move_order;      // [lead15]
        creators["leader attack order"] = &LeaderActionContext::leader_attack_order;  // [lead15]
        creators["leader heal target"] = &LeaderActionContext::leader_heal_target;    // [lead76]
    }

private:
    static Action* leader_ready_check(PlayerbotAI* botAI) { return new LeaderReadyCheckAction(botAI); }
    static Action* leader_ready_check_result(PlayerbotAI* botAI) { return new LeaderReadyCheckResultAction(botAI); }
    static Action* leader_encounter_event(PlayerbotAI* botAI) { return new LeaderEncounterEventAction(botAI); }
    static Action* leader_assign_tanks(PlayerbotAI* botAI) { return new LeaderAssignTanksAction(botAI); }
    static Action* use_raid_flask(PlayerbotAI* botAI) { return new UseRaidFlaskAction(botAI); }
    static Action* use_food_buff(PlayerbotAI* botAI) { return new UseFoodBuffAction(botAI); }
    static Action* use_pre_pull_potion(PlayerbotAI* botAI) { return new UsePrePullPotionAction(botAI); }
    static Action* use_soulwell(PlayerbotAI* botAI) { return new UseSoulwellAction(botAI); }
    static Action* click_soul_portal(PlayerbotAI* botAI) { return new ClickSoulPortalAction(botAI); }
    static Action* group_pause(PlayerbotAI* botAI) { return new GroupPauseAction(botAI); }
    static Action* group_resume(PlayerbotAI* botAI) { return new GroupResumeAction(botAI); }
    static Action* rest(PlayerbotAI* botAI) { return new RestChatAction(botAI); }
    static Action* interrupt_order(PlayerbotAI* botAI) { return new InterruptOrderAction(botAI); }
    static Action* resurrect_first(PlayerbotAI* botAI) { return new ResurrectFirstAction(botAI); }
    static Action* burst_order(PlayerbotAI* botAI) { return new BurstOrderAction(botAI); }
    static Action* burst_end(PlayerbotAI* botAI) { return new BurstEndAction(botAI); }
    static Action* wipe_escape(PlayerbotAI* botAI) { return new WipeEscapeAction(botAI); }
    static Action* wipe_escape_end(PlayerbotAI* botAI) { return new WipeEscapeEndAction(botAI); }
    static Action* leader_move_order(PlayerbotAI* botAI) { return new LeaderMoveOrderAction(botAI); }
    static Action* leader_attack_order(PlayerbotAI* botAI) { return new LeaderAttackOrderAction(botAI); }
    static Action* leader_heal_target(PlayerbotAI* botAI) { return new LeaderHealTargetAction(botAI); }
};

class LeaderTriggerContext : public NamedObjectContext<Trigger>
{
public:
    LeaderTriggerContext()
    {
        creators["leader ready check needed"] = &LeaderTriggerContext::leader_ready_check_needed;
        creators["leader ready check done"] = &LeaderTriggerContext::leader_ready_check_done;
        creators["leader encounter event"] = &LeaderTriggerContext::leader_encounter_event;
        creators["leader tank flags"] = &LeaderTriggerContext::leader_tank_flags;
        creators["ready check low mana"] = &LeaderTriggerContext::ready_check_low_mana;
        creators["need raid flask"] = &LeaderTriggerContext::need_raid_flask;
        creators["need food buff"] = &LeaderTriggerContext::need_food_buff;
        creators["pre pull potion"] = &LeaderTriggerContext::pre_pull_potion;
        creators["soulwell available"] = &LeaderTriggerContext::soulwell_available;
        creators["soul portal available"] = &LeaderTriggerContext::soul_portal_available;
        creators["burst over"] = &LeaderTriggerContext::burst_over;
        creators["leader move order"] = &LeaderTriggerContext::leader_move_order;      // [lead15]
        creators["leader attack order"] = &LeaderTriggerContext::leader_attack_order;  // [lead15]
        creators["leader heal target"] = &LeaderTriggerContext::leader_heal_target;    // [lead76]
        creators["wipe escape"] = &LeaderTriggerContext::wipe_escape;
        creators["wipe escape over"] = &LeaderTriggerContext::wipe_escape_over;

        // Group orders in chat (ChatCommandHandlerStrategy maps them to the actions above)
        creators["pause"] = &LeaderTriggerContext::pause;
        creators["resume"] = &LeaderTriggerContext::resume;
        creators["rest"] = &LeaderTriggerContext::rest;
        creators["interrupt"] = &LeaderTriggerContext::interrupt;
        creators["res"] = &LeaderTriggerContext::res;
        creators["burst"] = &LeaderTriggerContext::burst;
    }

private:
    static Trigger* leader_ready_check_needed(PlayerbotAI* botAI) { return new LeaderReadyCheckNeededTrigger(botAI); }
    static Trigger* leader_ready_check_done(PlayerbotAI* botAI) { return new LeaderReadyCheckDoneTrigger(botAI); }
    static Trigger* leader_encounter_event(PlayerbotAI* botAI) { return new LeaderEncounterEventTrigger(botAI); }
    static Trigger* leader_tank_flags(PlayerbotAI* botAI) { return new LeaderTankFlagsTrigger(botAI); }
    static Trigger* ready_check_low_mana(PlayerbotAI* botAI) { return new ReadyCheckLowManaTrigger(botAI); }
    static Trigger* need_raid_flask(PlayerbotAI* botAI) { return new NeedRaidFlaskTrigger(botAI); }
    static Trigger* need_food_buff(PlayerbotAI* botAI) { return new NeedFoodBuffTrigger(botAI); }
    static Trigger* pre_pull_potion(PlayerbotAI* botAI) { return new PrePullPotionTrigger(botAI); }
    static Trigger* soulwell_available(PlayerbotAI* botAI) { return new SoulwellAvailableTrigger(botAI); }
    static Trigger* soul_portal_available(PlayerbotAI* botAI) { return new SoulPortalAvailableTrigger(botAI); }
    static Trigger* burst_over(PlayerbotAI* botAI) { return new BurstOverTrigger(botAI); }
    static Trigger* leader_move_order(PlayerbotAI* botAI) { return new LeaderMoveOrderTrigger(botAI); }
    static Trigger* leader_attack_order(PlayerbotAI* botAI) { return new LeaderAttackOrderTrigger(botAI); }
    static Trigger* leader_heal_target(PlayerbotAI* botAI) { return new LeaderHealTargetTrigger(botAI); }
    static Trigger* wipe_escape(PlayerbotAI* botAI) { return new WipeEscapeTrigger(botAI); }
    static Trigger* wipe_escape_over(PlayerbotAI* botAI) { return new WipeEscapeOverTrigger(botAI); }
    static Trigger* pause(PlayerbotAI* botAI) { return new ChatCommandTrigger(botAI, "pause"); }
    static Trigger* resume(PlayerbotAI* botAI) { return new ChatCommandTrigger(botAI, "resume"); }
    static Trigger* rest(PlayerbotAI* botAI) { return new ChatCommandTrigger(botAI, "rest"); }
    static Trigger* interrupt(PlayerbotAI* botAI) { return new ChatCommandTrigger(botAI, "interrupt"); }
    static Trigger* res(PlayerbotAI* botAI) { return new ChatCommandTrigger(botAI, "res"); }
    static Trigger* burst(PlayerbotAI* botAI) { return new ChatCommandTrigger(botAI, "burst"); }
};

class LeaderValueContext : public NamedObjectContext<UntypedValue>
{
public:
    LeaderValueContext()
    {
        creators["raid coordinator"] = &LeaderValueContext::raid_coordinator;
        creators["leader nearby boss"] = &LeaderValueContext::leader_nearby_boss;
        creators["leader encounter event"] = &LeaderValueContext::leader_encounter_event;
        creators["leader tank assignment"] = &LeaderValueContext::leader_tank_assignment;
        creators["pull hold"] = &LeaderValueContext::pull_hold;
        creators["pull countdown"] = &LeaderValueContext::pull_countdown;
        creators["ready check in progress"] = &LeaderValueContext::ready_check_in_progress;
        creators["pre pull window"] = &LeaderValueContext::pre_pull_window;
        creators["raid flask"] = &LeaderValueContext::raid_flask;
        creators["raid food buff"] = &LeaderValueContext::raid_food_buff;
        creators["pre pull potion"] = &LeaderValueContext::pre_pull_potion;
        creators["nearby soulwell"] = &LeaderValueContext::nearby_soulwell;
        creators["nearby soul portal"] = &LeaderValueContext::nearby_soul_portal;
        creators["group paused"] = &LeaderValueContext::group_paused;
        creators["burst until"] = &LeaderValueContext::burst_until;
        creators["leader fight outlook"] = &LeaderValueContext::leader_fight_outlook;
        creators["wipe escape spell"] = &LeaderValueContext::wipe_escape_spell;
        creators["wipe escape at"] = &LeaderValueContext::wipe_escape_at;
        creators["wipe escape hold"] = &LeaderValueContext::wipe_escape_hold;
    }

private:
    static UntypedValue* raid_coordinator(PlayerbotAI* botAI) { return new RaidCoordinatorValue(botAI); }
    static UntypedValue* leader_nearby_boss(PlayerbotAI* botAI) { return new LeaderNearbyBossValue(botAI); }
    static UntypedValue* leader_encounter_event(PlayerbotAI* botAI) { return new LeaderEncounterEventValue(botAI); }
    static UntypedValue* leader_tank_assignment(PlayerbotAI* botAI) { return new LeaderTankAssignmentValue(botAI); }
    static UntypedValue* pull_hold(PlayerbotAI* botAI) { return new PullHoldValue(botAI); }
    static UntypedValue* pull_countdown(PlayerbotAI* botAI) { return new PullCountdownValue(botAI); }
    static UntypedValue* ready_check_in_progress(PlayerbotAI* botAI) { return new ReadyCheckInProgressValue(botAI); }
    static UntypedValue* pre_pull_window(PlayerbotAI* botAI) { return new PrePullWindowValue(botAI); }
    static UntypedValue* raid_flask(PlayerbotAI* botAI) { return new RaidFlaskValue(botAI); }
    static UntypedValue* raid_food_buff(PlayerbotAI* botAI) { return new RaidFoodBuffValue(botAI); }
    static UntypedValue* pre_pull_potion(PlayerbotAI* botAI) { return new PrePullPotionValue(botAI); }
    static UntypedValue* nearby_soulwell(PlayerbotAI* botAI) { return new NearbySoulwellValue(botAI); }
    static UntypedValue* nearby_soul_portal(PlayerbotAI* botAI) { return new NearbySoulPortalValue(botAI); }
    static UntypedValue* group_paused(PlayerbotAI* botAI) { return new GroupPausedValue(botAI); }
    static UntypedValue* burst_until(PlayerbotAI* botAI) { return new BurstUntilValue(botAI); }
    static UntypedValue* leader_fight_outlook(PlayerbotAI* botAI) { return new LeaderFightOutlookValue(botAI); }
    static UntypedValue* wipe_escape_spell(PlayerbotAI* botAI) { return new WipeEscapeSpellValue(botAI); }
    static UntypedValue* wipe_escape_at(PlayerbotAI* botAI) { return new WipeEscapeAtValue(botAI); }
    static UntypedValue* wipe_escape_hold(PlayerbotAI* botAI) { return new WipeEscapeHoldValue(botAI); }
};

#endif
