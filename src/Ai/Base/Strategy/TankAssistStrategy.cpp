/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TankAssistStrategy.h"
#include "Playerbots.h"

void TankAssistStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Out of combat a mob on a member is taken before anything else, pull holds and plans included.
    triggers.push_back(new TriggerNode("group member attacked",
                                       { NextAction("tank defend on attacked group member", ACTION_EMERGENCY + 2) }));
    // A boss beside the tank that does not aggro on proximity is opened on (below the defence of a member).
    triggers.push_back(
        new TriggerNode("idle boss at tank", { NextAction("tank open idle boss", ACTION_EMERGENCY + 1) }));
    // [taunt_reaction] A mob turning to a non-tank member is taken back from where the tank stands, within about one
    // GCD: the area taunt for two or more on one member, the class taunt, else a ranged threat spell. Above every
    // rotation filler, the fight-spot hold and the encounter walks (HoR "hor move to wave position" at ACTION_RAID +
    // 5 held the taunt 5-6 s at the wave start); below the emergencies (ACTION_EMERGENCY: avoid aoe, defensives).
    triggers.push_back(new TriggerNode("taunt reaction", { NextAction("taunt reaction", ACTION_RAID + 6) }));
    // [flee_stop] A mob running at low health is stunned or snared before it brings the next pack (instant, above
    // the rotation and the interrupts, below the dungeon priority interrupt).
    triggers.push_back(new TriggerNode("fleeing mob", { NextAction("flee stop", ACTION_INTERRUPT + 4) }));
    // [fled_survivor] Out of combat: a damaged mob that fled from an earlier fight is finished before the group walks
    // on (above the harness walk "test runner move", ACTION_NORMAL + 1, and "follow").
    triggers.push_back(
        new TriggerNode("fled survivor near", { NextAction("finish fled survivor", ACTION_HIGH + 5) }));
    triggers.push_back(
        // Below the peel taunts ("mob attacking group member", ACTION_INTERRUPT + 0.2..0.6), above the rotation.
        new TriggerNode("tank assist", { NextAction("tank assist", ACTION_INTERRUPT) }));
    // Wave 9 Steamvault: a patrol killed the healer three times 40-51 yd from an idle tank out of line of sight; the
    // out-of-combat attack failed every tick and nothing walked the tank there. In combat the peel step
    // ("move to group member attackers", ACTION_MOVE + 1) goes first; this path follows if the step is refused.
    triggers.push_back(
        new TriggerNode("healer attacked far", { NextAction("reach healer attacker", ACTION_MOVE + 0.8f) }));
    // User, Ragefire Chasm 2026-10-24: a fight that a mob walking about can walk into is walked back by the tank, once
    // ("[fight spot]"); as high as "return to pull position", the walk of a pull back.
    triggers.push_back(
        new TriggerNode("fight spot back", { NextAction("move fight spot back", ACTION_MOVE + 5.0f) }));
    // User, Ragefire Chasm 2026-10-24: «обязательно танковать мобов на краю лавы?» A fight within a few yards of lava,
    // slime or a drop-off is dragged to open floor ("[hazard]"); below the walk back (which it waits for), above the
    // healer-sight step (ACTION_MOVE + 2: the spot is in the healer's sight), "reach melee" and "tank face".
    triggers.push_back(
        new TriggerNode("fight near hazard", { NextAction("move fight off hazard", ACTION_MOVE + 4.0f) }));
    // [chain_pull_in_combat] The last mob of the fight nearly down: the next small pack close by is pulled now, a
    // cast from where the tank fights (above the rotation, below the peels and the moves).
    triggers.push_back(new TriggerNode("chain pull", { NextAction("chain pull", ACTION_HIGH + 1.0f) }));
}
