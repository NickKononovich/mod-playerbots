/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDRIDING_H
#define PLAYERBOTS_GUILDRIDING_H

#include <string>
#include <vector>

#include "Define.h"

class Player;
struct GuildOrder;
struct ItemTemplate;

// [mount1] user 2026-11-13: the guild's members ride and fly when they have the money, honestly. The global leader
// plans each member's Apprentice Riding (its riding trainer's price) and its racial mount (a vendor whose gossip sells
// to it: the race / reputation conditions) against what riding saves it: its walking share on foot in the open world
// (QuestStats::FootTravel) times the mount's speed gain, less a mount cast per walk begun, priced at its quest pace
// (GuildCoordinator::QuestCopperPerMinute). A member buys with its spare money (FoundingFreeMoney: above its repairs,
// supplies, spells, travel and next profession ranks); the gold it lacks comes from the members whose own riding is
// worth less or who already ride, the highest value first (UpdateTransfers' mail / trade, after every other gold
// want). The bought mount item is used at once (the core's Learning cast); outdoors a walk longer than the mount's
// cast pays for mounts first (NewRpgBaseAction::MountForWalk).
namespace GuildRiding
{
constexpr uint32 SPELL_APPRENTICE_RIDING = 33388;

// the fastest ground mount spell the bot knows and may ride (its riding skill), its speed gain (0.6 = +60 %) and its
// cast seconds; spellId 0: none. Any thread (a per-thread cache, renewed when the spell book or the skill changes)
struct GroundMount
{
    uint32 spellId{0};
    float bonus{0.0f};
    float castSec{0.0f};
};
GroundMount BestGroundMount(Player* bot);

// the bot's travel speed over open ground: mounted its speed, else with a ground mount its run speed with the
// mount's gain (outside instances), else its run speed (yd/s)
float TravelSpeed(Player* bot);

// the straight walk length from which the mount's cast pays: the cast seconds against the seconds the speed gain saves
float MountPaysFromYd(Player* bot, GroundMount const& mount);

// an item that teaches a mount (class Misc, subclass Mount)
bool IsMountItem(ItemTemplate const* proto);

std::string Money(uint32 copper);

// the member's riding value from its last plan (0: not planned yet)
float SavedMinutesPerHour(uint32 low);

// the guild coordinator's 5 s step (world thread): one member's plan, the mount items to use, the guild line
void Update();

// UpdateOrders (world thread, the coordinator's lock held): the member's riding orders, by its spare money
void AddOrders(Player* p, uint32 spare, std::vector<GuildOrder>& orders);

// the order is the riding trainer visit (a BUY order with the riding spell, NextStep's train branch)
bool IsTrainOrder(GuildOrder const& o);

// the members whose riding waits for gold, the highest value first, and the gold each lacks (UpdateTransfers)
std::vector<uint32> FundingOrder();
uint32 Shortfall(uint32 low);
// `from` may give its spare to `to`'s riding: it rides already, or its own riding is worth less now
bool MayFund(uint32 fromLow, uint32 toLow);

// [mount7] a code path takes the bot off its mount: the reason its walk may mount again (MountForWalk)
void NoteDismount(Player* bot, char const* why);

// [taxi1] a flight taken: its seconds saved and fare, for the guild line
void NoteFlight(float savedSec, uint32 fare);
}  // namespace GuildRiding

#endif
