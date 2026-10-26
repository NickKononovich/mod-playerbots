/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GDStrategy.h"
#include "GDMultipliers.h"
#include "GDTriggers.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"

void WotlkDungeonGDStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Altars of Slad'ran, Moorabi and the Colossus open the bridge to Gal'darah
    triggers.push_back(new TriggerNode("gundrak altar ready",
        { NextAction("gundrak use altar", ACTION_MOVE + 4) }));
    triggers.push_back(new TriggerNode("gundrak bridge formed",
        { NextAction("gundrak cross bridge", ACTION_MOVE + 5) }));
    // Out of the central circle's pool: its bottom is a navmesh part of its own (GundrakCircle)
    triggers.push_back(new TriggerNode("gundrak circle route",
        { NextAction("gundrak circle route", ACTION_MOVE + 6) }));
    // [b34_gd] Mobs that fell off the deck into the moat fight the group from under the east landing, out of
    // sight and reach: the members jump in after them (above the bridge walk, which would pull them back onto
    // the landing), and walk out to the bridge start once the fight is over.
    triggers.push_back(new TriggerNode("gundrak moat fight",
        { NextAction("gundrak moat drop", ACTION_MOVE + 7) }));
    triggers.push_back(new TriggerNode("gundrak moat exit",
        { NextAction("gundrak moat exit", ACTION_MOVE + 4) }));

    // Moorabi

    // Drakkari Colossus: dormant until a player engages one of its Living Mojos
    triggers.push_back(new TriggerNode("drakkari colossus dormant",
        { NextAction("engage living mojo", ACTION_MOVE + 3) }));

    // Slad'ran: a Viper or Constrictor on the healer is killed first (DungeonTactics.cpp kill first; wave 10 heroic:
    // the snakes killed the healer while the dps stayed on the boss), by the snake killer only: the snakes never stop,
    // the others burn the boss (GundrakSladran::IsSnakeKiller, SladranMultiplier; wave 6 wiped chasing them all).
    // Freeing a Snake Wrap victim (64) outranks it.
    triggers.push_back(new TriggerNode("dungeon kill first",
        { NextAction("dungeon kill first", ACTION_RAID + 2) }));
    // [b35_gd] above the healer's emergency heals: gd-h 20261026-024524 "avoid poison nova (not evaluated: pain
    // suppression (91.00) ran first)" while the nova's 3.5 s cast ran out
    triggers.push_back(new TriggerNode("poison nova",
        { NextAction("avoid poison nova", ACTION_EMERGENCY + 4) }));
    triggers.push_back(new TriggerNode("snake wrap",
        { NextAction("attack snake wrap", ACTION_RAID + 4) }));
    triggers.push_back(new TriggerNode("slad'ran stack on tank",
        { NextAction("slad'ran stack on tank", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("slad'ran tank hold",
        { NextAction("slad'ran tank hold", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("slad'ran tank park",
        { NextAction("slad'ran tank park", ACTION_RAID + 2) }));

    // Gal'darah
    triggers.push_back(new TriggerNode("whirling slash",
        { NextAction("avoid whirling slash", ACTION_RAID + 5) }));

    // Eck the Ferocious (Heroic only)

    // Living Mojo trash: Mojo Puddle (DungeonHazard.cpp)
    triggers.push_back(new TriggerNode("dungeon hazard",
        { NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));

    // Drakkari Lancer trash: melee damage dealers stop hitting it while Retaliation is up
    triggers.push_back(new TriggerNode("dungeon melee hold",
        { NextAction("dungeon melee hold", ACTION_EMERGENCY + 1) }));
}

void WotlkDungeonGDStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new SladranMultiplier(botAI));
    multipliers.push_back(new GaldarahMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    // The snake killer keeps its snake until dead and the others stay on the boss: SladranMultiplier.
}

void WotlkDungeonGDStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget || type == TargetValueExclusionType::DebuffTarget)
        DungeonTactics::AppendMeleeHoldExclusions(botAI, exclusions);

    if (type != TargetValueExclusionType::DpsTarget)
        return;

    // While Slad'ran fights, every damage dealer but the snake killer burns him: the snakes leave their "dps target".
    if (GundrakSladran::GetEngagedBoss(botAI) && !GundrakSladran::IsSnakeKiller(botAI))
    {
        GundrakSladran::AppendSnakeExclusions(botAI, exclusions);
        return;
    }

    // The snake killer's "dps target" is the kill-first snake while there is one, so "dps assist" keeps the target
    // that "dungeon kill first" chose instead of switching it back every tick.
    DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);
}
