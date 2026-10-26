/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOVECONFLICT_H
#define PLAYERBOTS_MOVECONFLICT_H

#include <string>

class Player;
class PlayerbotAI;

// [move_conflict] Two movement sources that pull one bot back and forth (scholo-h: the harness waypoint walked the
// tank down to the Iron Gate, "scholo hall rally" dragged him back up the stairs, the waypoint walked him down
// again).
namespace MoveConflict
{
    // (a) The detector. Every movement order (MovementAction::MoveTo) is noted with its source (the action's name;
    // "test runner move" is the harness waypoint) and its target. When two sources alternate with opposite
    // directions (the angle between their directions from the bot over FLIP_ANGLE) more than MAX_FLIPS times within
    // WINDOW_MS, "move_conflict" is logged with both sources and targets (once per WINDOW_MS per pair).
    void NoteOrder(Player* bot, std::string const& source, float x, float y, float z);

    // (b) The waypoint lane. While the bot has an active harness waypoint (TestRunner::GetWaypoint) and no fight is
    // on (not in combat, no encounter in progress), a dungeon or raid strategy's movement action (any action name
    // outside the shared contexts: base, chat, packets, leader, test runner, self travel, dungeon hazards) may not
    // move it more than LANE_WIDTH yd off the waypoint's direction (sideways, or backwards). Exempt: hazard escapes
    // (the hazard context, and names with avoid / flee / escape / away / dodge / hazard) and escorts of event NPCs
    // (names with follow / escort: the harness route does not model where the NPC leads). True: the move is vetoed
    // (logged as "waypoint_lane_veto", rate-limited).
    bool VetoesOffWaypoint(Player* bot, std::string const& source, float x, float y, float z,
                           bool encounterInProgress);
    // The cheap part of VetoesOffWaypoint (a harness waypoint on this map, no combat, a source the lane binds):
    // callers ask the encounter state only when this holds.
    bool LaneApplies(Player* bot, std::string const& source);

    // (c) [escort_anchor] The lane the other way round: an escort wins over the harness waypoint. While a leashed
    // escort NPC (EscortProtect::EscortAnchor, Old Hillsbrad's Thrall) is under way near the bot, the bot's
    // test-runner waypoint is suspended: Thrall walks his own path and the group goes with him (ohb-h 163739 build
    // 29: 102 move_conflict, "escort stay close" against "test runner move" / "follow" at ~165 degrees, idle 465 s).
    // While the NPC waits at a checkpoint (gossip up), only a waypoint within its return distance stands (the walk
    // to talk to it). True: the waypoint yields (logged "[escort_anchor]", event "escort_yield", rate-limited).
    bool EscortYieldsWaypoint(PlayerbotAI* botAI, float x, float y, float z);

    // (d) [b33_harn] The event hold. A dungeon strategy that keeps its group at a fixed spot while a scripted
    // event runs (Halls of Stone's Tribunal of Ages: the tank's spot at Brann's console) notes it here every
    // evaluation (NoteEventHold, from a trigger every member evaluates; the note lasts EVENT_HOLD_TTL_MS). While it
    // is fresh, nothing generic moves a member of that group out of `radius` of the spot: the harness waypoint walk
    // yields (EventHoldYieldsWaypoint, TestRunnerStrategy) and a LoS pull whose fight spot or gather spot lies
    // outside is dropped (EventHoldBlocksSpot, LosPull::Get). hos-h 20261026-000434-hos-h-5B-37: "test runner move"
    // walked the tank 31 yd off the hold spot at 1832 s (Tribunal IN_PROGRESS), and at 1946 s "move to los pull
    // spot" took the shaman and the rogue 15 yd off it mid-wave.
    void NoteEventHold(Player* bot, float x, float y, float z, float radius, char const* what);
    // The fresh hold of the bot's group: true with its spot and radius.
    bool GetEventHold(Player* bot, float& x, float& y, float& z, float& radius);
    // True: a fresh hold stands and (x, y) lies outside it (logged "event_hold_block", rate-limited per source).
    bool EventHoldBlocksSpot(Player* bot, float x, float y, char const* source);
    bool EventHoldYieldsWaypoint(Player* bot, float x, float y, float z);
}

#endif
