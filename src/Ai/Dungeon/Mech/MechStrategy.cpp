/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MechStrategy.h"
#include "DungeonHazard.h"
#include "MechMultipliers.h"
#include "MechTriggers.h"

void TbcDungeonMechanarStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // The lift between the levels: above everything else, a rider must not be walked off the deck.
    triggers.push_back(new TriggerNode("mechanar lift", {
        NextAction("mechanar ride lift", ACTION_EMERGENCY + 8) }));

    triggers.push_back(new TriggerNode("sepethrea kite flame", {
        NextAction("sepethrea kite flame", ACTION_EMERGENCY + 6) }));

    triggers.push_back(new TriggerNode("sepethrea avoid flame", {
        NextAction("sepethrea avoid flame", ACTION_EMERGENCY + 5) }));

    triggers.push_back(new TriggerNode("sepethrea trail", {
        NextAction("sepethrea avoid trail", ACTION_EMERGENCY + 4) }));

    triggers.push_back(new TriggerNode("sepethrea focus boss", {
        NextAction("sepethrea focus boss", ACTION_RAID + 2) }));

    // Mechano-Lord Capacitus (MechShared.h MechanarCapacitus): heroic Polarity Shift, one stack per charge; normal
    // Reflective Magic / Damage Shield, no magic spell into the one, no melee damage dealer into the other.
    triggers.push_back(new TriggerNode("capacitus polarity", {
        NextAction("capacitus polarity", ACTION_EMERGENCY + 3) }));
    triggers.push_back(new TriggerNode("capacitus reflective shield", {
        NextAction("capacitus hold fire", ACTION_EMERGENCY + 2) }));

    // Pathaleon is invisible and unattackable until the four bridge mobs before him die (MechanarBridge): the tank
    // pulls that pack once the way to it is clear and the group is gathered and ready.
    triggers.push_back(new TriggerNode("mechanar bridge pull", {
        NextAction("mechanar bridge pull", 4.5f) }));

    // Mechano-Lord Capacitus: Nether Charges once they stop wandering and pulse towards Nether Detonation
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void TbcDungeonMechanarStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new MechanarLiftHoldMultiplier(botAI));
    multipliers.push_back(new SepethreaKiteFlameMultiplier(botAI));
    multipliers.push_back(new SepethreaFocusBossMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new CapacitusMultiplier(botAI));
}
