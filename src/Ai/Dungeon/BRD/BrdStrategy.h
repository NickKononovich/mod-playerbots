/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRDSTRATEGY_H
#define PLAYERBOTS_BRDSTRATEGY_H

#include "AttackAction.h"
#include "MovementActions.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

// Blackrock Depths (map 230), the expert-group rules of TACTIC_AUDIT_brd.md section 6 F8/F9, checked against
// AzerothCore (blackrock_depths.cpp, smart_scripts):
// - kill first: Anvilrage Medic 8894 (Heal 15586 / Prayer of Healing 15585 on its pack), Princess Moira Bronzebeard
//   8929 while Emperor Dagran Thaurissan 9019 fights (she heals him), the golems 8906 / 8907 of Golem Lord
//   Argelmach's room while he fights (his aggro sets instance data 34 and wakes them), Verek 9042 before Warder
//   Stilgiss 9041;
// - Iron Hall: the six Ironhand Guardians 8982 (brd_ironhand_guardian: Gout of Flames 15529 every 13-19 s once the
//   hall is set off) stand in pairs across the hall at y -587 / -632 / -677 and breathe across it; nobody stays in a
//   breathing guardian's row;
// - Lord Incendius 9017 (smart_scripts 9017 id 3: Mighty Blow 14099 on his victim every 12-22 s, a knockback): the
//   tank stands with a wall behind it, so the blow does not throw it into the lava.
class ClassicDungeonBlackrockDepthsStrategy : public Strategy
{
public:
    ClassicDungeonBlackrockDepthsStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "classic-brd"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;

    // The damage dealers keep the kill-first target: without the exclusion "dps assist" switches back the next tick.
    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
};

namespace BlackrockDepths
{
// The closest attacker of the kill-first table the damage dealers take now, or nullptr.
Unit* KillFirstTarget(PlayerbotAI* botAI);
}  // namespace BlackrockDepths

// The Ironhand Guardians near the bot (grid search every 5 s, only inside the Iron Hall).
class BrdIronhandGuardiansValue : public ObjectGuidListCalculatedValue
{
public:
    BrdIronhandGuardiansValue(PlayerbotAI* botAI) : ObjectGuidListCalculatedValue(botAI, "brd ironhand guardians", 5000)
    {
    }

protected:
    GuidVector Calculate() override;
};

class BrdKillFirstTrigger : public Trigger
{
public:
    BrdKillFirstTrigger(PlayerbotAI* botAI) : Trigger(botAI, "brd kill first") {}
    bool IsActive() override;
};

class BrdKillFirstAction : public AttackAction
{
public:
    BrdKillFirstAction(PlayerbotAI* botAI) : AttackAction(botAI, "brd kill first") {}
    bool Execute(Event event) override;
};

class BrdGoutOfFlamesTrigger : public Trigger
{
public:
    BrdGoutOfFlamesTrigger(PlayerbotAI* botAI) : Trigger(botAI, "brd gout of flames") {}
    bool IsActive() override;
};

// Steps out of the breathing guardian's row to the middle between two rows (or past the end row), keeping x.
class BrdAvoidGoutOfFlamesAction : public MovementAction
{
public:
    BrdAvoidGoutOfFlamesAction(PlayerbotAI* botAI) : MovementAction(botAI, "brd avoid gout of flames") {}
    bool Execute(Event event) override;
};

class BrdIncendiusWallTrigger : public Trigger
{
public:
    BrdIncendiusWallTrigger(PlayerbotAI* botAI) : Trigger(botAI, "brd incendius wall") {}
    bool IsActive() override;
};

// The tank moves round Lord Incendius to a melee spot whose knockback line ends at a wall (no line of sight 14 yd
// behind the spot, measured away from him).
class BrdIncendiusTankAtWallAction : public MovementAction
{
public:
    BrdIncendiusTankAtWallAction(PlayerbotAI* botAI) : MovementAction(botAI, "brd incendius tank at wall") {}
    bool Execute(Event event) override;
};

class ClassicDungeonBrdValueContext : public NamedObjectContext<UntypedValue>
{
public:
    ClassicDungeonBrdValueContext()
    {
        creators["brd ironhand guardians"] = &ClassicDungeonBrdValueContext::brd_ironhand_guardians;
    }

private:
    static UntypedValue* brd_ironhand_guardians(PlayerbotAI* botAI) { return new BrdIronhandGuardiansValue(botAI); }
};

class ClassicDungeonBrdTriggerContext : public NamedObjectContext<Trigger>
{
public:
    ClassicDungeonBrdTriggerContext()
    {
        creators["brd kill first"] = &ClassicDungeonBrdTriggerContext::brd_kill_first;
        creators["brd gout of flames"] = &ClassicDungeonBrdTriggerContext::brd_gout_of_flames;
        creators["brd incendius wall"] = &ClassicDungeonBrdTriggerContext::brd_incendius_wall;
    }

private:
    static Trigger* brd_kill_first(PlayerbotAI* botAI) { return new BrdKillFirstTrigger(botAI); }
    static Trigger* brd_gout_of_flames(PlayerbotAI* botAI) { return new BrdGoutOfFlamesTrigger(botAI); }
    static Trigger* brd_incendius_wall(PlayerbotAI* botAI) { return new BrdIncendiusWallTrigger(botAI); }
};

class ClassicDungeonBrdActionContext : public NamedObjectContext<Action>
{
public:
    ClassicDungeonBrdActionContext()
    {
        creators["brd kill first"] = &ClassicDungeonBrdActionContext::brd_kill_first;
        creators["brd avoid gout of flames"] = &ClassicDungeonBrdActionContext::brd_avoid_gout_of_flames;
        creators["brd incendius tank at wall"] = &ClassicDungeonBrdActionContext::brd_incendius_tank_at_wall;
    }

private:
    static Action* brd_kill_first(PlayerbotAI* botAI) { return new BrdKillFirstAction(botAI); }
    static Action* brd_avoid_gout_of_flames(PlayerbotAI* botAI) { return new BrdAvoidGoutOfFlamesAction(botAI); }
    static Action* brd_incendius_tank_at_wall(PlayerbotAI* botAI) { return new BrdIncendiusTankAtWallAction(botAI); }
};

#endif
