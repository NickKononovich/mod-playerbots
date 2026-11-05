/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONHAZARD_H
#define PLAYERBOTS_DUNGEONHAZARD_H

#include "DungeonTactics.h"
#include "Multiplier.h"
#include "NamedObjectContext.h"
#include "ObjectGuid.h"
#include "TbcHazard.h"
#include "Trigger.h"
#include "Value.h"
#include <vector>

class Player;
class PlayerbotAI;

// Ground hazards of dungeon trash and bosses that are a circle around a creature or a persistent area aura and
// that nothing else handles: explosions on death or at low health, timed bombs, pulsing corpses, poison clouds
// under mobs that are tanked in them. The hazards of each map are listed in DungeonHazard.cpp; a dungeon strategy
// enables them with the "dungeon hazard" trigger and DungeonHazardHoldMultiplier.
namespace DungeonHazard
{
struct Circle
{
    float x;
    float y;
    float radius;  // spell radius plus the bot's own size: area spells hit a unit whose edge is inside
    // The hold multiplier keeps the bot from walking back into this circle. Off for hazards that are only passed by
    // (a dormant Arcatraz Sentinel next to the route): following the group past one must stay possible.
    bool hold = true;
    // The hazard is a mob chasing this bot (the bot has its aggro): it outruns a player, so stepping out is futile
    // and the bot runs to its tank instead, who takes the mob back.
    bool chasing = false;
    // [b44_st] A mob chasing this bot that nobody can take over or kill (the Atal'ai Deathwalker's Spirit): the circle
    // is grown by the mob's walk, and no reposition action leads the bot past it (DungeonHazardHoldMultiplier).
    bool kite = false;
    // [lead1483] what hits inside, for the leader's stay-or-leave verdict (LeaderHazardVerdict): the spell (the
    // caster's difficulty rank; 0: unknown), its caster, the ms its hits still come (-1: for good) and between them
    uint32 spellId = 0;
    ObjectGuid caster;
    int32 leftMs = -1;
    uint32 periodMs = 0;
};

// Hazards of the bot's map that apply to its role, within the scan range (grid search; read the cached value).
std::vector<Circle> Collect(Player* bot);

// Only the circles the bot can step out of (the chasing ones are left out).
bool IsInside(Player* bot, std::vector<Circle> const& circles, float extra);

// The bot has the aggro of a chasing hazard and its tank stands farther than the hand-over distance.
Player* ChasedTowardsTank(Player* bot, std::vector<Circle> const& circles);
}

class DungeonHazardCirclesValue : public CalculatedValue<std::vector<DungeonHazard::Circle>>
{
public:
    DungeonHazardCirclesValue(PlayerbotAI* botAI)
        : CalculatedValue<std::vector<DungeonHazard::Circle>>(botAI, "dungeon hazard circles", 300)
    {
    }

protected:
    std::vector<DungeonHazard::Circle> Calculate() override;
};

class DungeonHazardTrigger : public Trigger
{
public:
    DungeonHazardTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon hazard") {}
    bool IsActive() override;
};

// Steps to the closest free spot outside every hazard (melee prefer one near their target); a bot chased by a hazard
// mob runs to its tank instead.
class AvoidDungeonHazardAction : public TbcHazardMoveAction
{
public:
    AvoidDungeonHazardAction(PlayerbotAI* botAI) : TbcHazardMoveAction(botAI, "avoid dungeon hazard") {}
    bool Execute(Event event) override;
};

// Next to a hazard whose centre holds the bot's target, no reposition may carry the bot back in.
class DungeonHazardHoldMultiplier : public Multiplier
{
public:
    DungeonHazardHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "dungeon hazard hold") {}
    float GetValue(Action* action) override;
};

class DungeonHazardValueContext : public NamedObjectContext<UntypedValue>
{
public:
    DungeonHazardValueContext()
    {
        creators["dungeon hazard circles"] = &DungeonHazardValueContext::dungeon_hazard_circles;
        creators["dungeon boss room"] = &DungeonHazardValueContext::dungeon_boss_room;
        creators["dungeon wait for tank"] = &DungeonHazardValueContext::dungeon_wait_for_tank;
    }

private:
    static UntypedValue* dungeon_hazard_circles(PlayerbotAI* botAI) { return new DungeonHazardCirclesValue(botAI); }
    static UntypedValue* dungeon_boss_room(PlayerbotAI* botAI) { return new DungeonBossRoomValue(botAI); }
    static UntypedValue* dungeon_wait_for_tank(PlayerbotAI* botAI) { return new DungeonWaitForTankValue(botAI); }
};

class DungeonHazardTriggerContext : public NamedObjectContext<Trigger>
{
public:
    DungeonHazardTriggerContext()
    {
        creators["dungeon hazard"] = &DungeonHazardTriggerContext::dungeon_hazard;
        creators["dungeon boss room trash"] = &DungeonHazardTriggerContext::dungeon_boss_room_trash;
        creators["dungeon melee hold"] = &DungeonHazardTriggerContext::dungeon_melee_hold;
        creators["dungeon kill first"] = &DungeonHazardTriggerContext::dungeon_kill_first;
        creators["dungeon frontal cleave"] = &DungeonHazardTriggerContext::dungeon_frontal_cleave;
        creators["dungeon priority interrupt"] = &DungeonHazardTriggerContext::dungeon_priority_interrupt;
        creators["dungeon top up"] = &DungeonHazardTriggerContext::dungeon_top_up;
        creators["dungeon tank cooldown"] = &DungeonHazardTriggerContext::dungeon_tank_cooldown;
        creators["dungeon fear guard"] = &DungeonHazardTriggerContext::dungeon_fear_guard;
        creators["dungeon wait for tank"] = &DungeonHazardTriggerContext::dungeon_wait_for_tank;
    }

private:
    static Trigger* dungeon_hazard(PlayerbotAI* botAI) { return new DungeonHazardTrigger(botAI); }
    static Trigger* dungeon_boss_room_trash(PlayerbotAI* botAI) { return new DungeonBossRoomTrashTrigger(botAI); }
    static Trigger* dungeon_melee_hold(PlayerbotAI* botAI) { return new DungeonMeleeHoldTrigger(botAI); }
    static Trigger* dungeon_kill_first(PlayerbotAI* botAI) { return new DungeonKillFirstTrigger(botAI); }
    static Trigger* dungeon_frontal_cleave(PlayerbotAI* botAI) { return new DungeonFrontalCleaveTrigger(botAI); }
    static Trigger* dungeon_priority_interrupt(PlayerbotAI* botAI)
    {
        return new DungeonPriorityInterruptTrigger(botAI);
    }
    static Trigger* dungeon_top_up(PlayerbotAI* botAI) { return new DungeonTopUpTrigger(botAI); }
    static Trigger* dungeon_tank_cooldown(PlayerbotAI* botAI) { return new DungeonTankCooldownTrigger(botAI); }
    static Trigger* dungeon_fear_guard(PlayerbotAI* botAI) { return new DungeonFearGuardTrigger(botAI); }
    static Trigger* dungeon_wait_for_tank(PlayerbotAI* botAI) { return new DungeonWaitForTankTrigger(botAI); }
};

class DungeonHazardActionContext : public NamedObjectContext<Action>
{
public:
    DungeonHazardActionContext()
    {
        creators["avoid dungeon hazard"] = &DungeonHazardActionContext::avoid_dungeon_hazard;
        creators["dungeon boss room pull out"] = &DungeonHazardActionContext::dungeon_boss_room_pull_out;
        creators["dungeon melee hold"] = &DungeonHazardActionContext::dungeon_melee_hold;
        creators["dungeon kill first"] = &DungeonHazardActionContext::dungeon_kill_first;
        creators["dungeon avoid frontal cleave"] = &DungeonHazardActionContext::dungeon_avoid_frontal_cleave;
        creators["dungeon priority interrupt"] = &DungeonHazardActionContext::dungeon_priority_interrupt;
        creators["dungeon top up"] = &DungeonHazardActionContext::dungeon_top_up;
        creators["dungeon tank cooldown"] = &DungeonHazardActionContext::dungeon_tank_cooldown;
        creators["dungeon fear guard"] = &DungeonHazardActionContext::dungeon_fear_guard;
        creators["dungeon wait for tank"] = &DungeonHazardActionContext::dungeon_wait_for_tank;
    }

private:
    static Action* avoid_dungeon_hazard(PlayerbotAI* botAI) { return new AvoidDungeonHazardAction(botAI); }
    static Action* dungeon_boss_room_pull_out(PlayerbotAI* botAI) { return new DungeonBossRoomPullOutAction(botAI); }
    static Action* dungeon_melee_hold(PlayerbotAI* botAI) { return new DungeonMeleeHoldAction(botAI); }
    static Action* dungeon_kill_first(PlayerbotAI* botAI) { return new DungeonKillFirstAction(botAI); }
    static Action* dungeon_avoid_frontal_cleave(PlayerbotAI* botAI)
    {
        return new DungeonAvoidFrontalCleaveAction(botAI);
    }
    static Action* dungeon_priority_interrupt(PlayerbotAI* botAI) { return new DungeonPriorityInterruptAction(botAI); }
    static Action* dungeon_top_up(PlayerbotAI* botAI) { return new DungeonTopUpAction(botAI); }
    static Action* dungeon_tank_cooldown(PlayerbotAI* botAI) { return new DungeonTankCooldownAction(botAI); }
    static Action* dungeon_fear_guard(PlayerbotAI* botAI) { return new DungeonFearGuardAction(botAI); }
    static Action* dungeon_wait_for_tank(PlayerbotAI* botAI) { return new DungeonWaitForTankAction(botAI); }
};

#endif
