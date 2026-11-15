/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOVE_STEP_H
#define PLAYERBOTS_MOVE_STEP_H

// [move51] DID THE STEP START, AS A SEAM. `MovementAction::DoMovePoint` was `void`: it had three exits that put no
// order into the motion master at all, and the caller could not tell them from the exit that did. `MoveTo` returned
// `true` after every one of them and set `LastMovement` as if a walk were on, `LeaderMoveOrderAction::Execute`
// acked the leader "walks to the spot", and on the next tick `IsDuplicateMove` / `IsWaitingForLastMove` refused the
// re-ask of that very walk - so the member stood still and the leader held an order nobody executed. Measured on
// the live journal: 73 `A:leader move order - OK` rows in the largest key of the problems table, standing up to
// 51 s.
//
// Nothing here reads the world, the clock or the database: the outcome of the move order goes in as a value, and
// "did the step start", "may this be reported as success", "what does the refusal say" and "does the leader hear a
// refusal it must decide on" come out. One judge of "is the bot walking by this order": this enum. The guards that
// find the outcome stay where they were (DoMovePoint reads the navmesh, as it always did) and nothing recomputes
// their answer.
//
// WHAT THE THREE SILENT EXITS MEANT, in the words of the code that returns them:
//   * `NoMover` - there is no unit to move, or it has no motion master. No order was possible.
//   * `NoCompletePath` - [nav24] the core's path to the destination is not complete (a shortcut with no polygon at
//     the end, or an INCOMPLETE path past the 74 / 148 polygon limit whose straight tail climbs through rock), and
//     the leader's route search found none either. The walk gives the spot up; it left two LOG_DEBUG lines an hour.
//   * `DungeonPathRefused` - [sfk243] in a dungeon a path WAS found, but `WalkBotPoints` refused to walk its points
//     (its path climbs a wall), and a straight move to the destination in a dungeon is the very thing that walked
//     22 tanks from the sfk floor up onto the ramparts. So no move at all - and this exit left no line anywhere.
//
// `AlreadyUnderWay` is the fourth exit, the one in `MoveTo` itself: `JudgeWaterDrop` answered `Under`, meaning the
// group's jump off the brink is in the air already (the effect motion generator runs, or the bot carries
// MOVEMENTFLAG_FALLING). It is not a refusal and not a step this call started: the bot really is moving, by an
// earlier order. It is kept reportable as success on purpose - treating a jump in flight as a refusal would let
// the leader pick another spot in the middle of a group water drop, and that is the leader's decision to change,
// not this seam's.

#include <cstdint>
#include <string>

namespace Move
{
    // the outcome of one move order, as the movement itself saw it
    enum class StepStart : uint8_t
    {
        Started = 0,             // an order went into the motion master: the bot walks by THIS call
        AlreadyUnderWay = 1,     // no order now, the bot moves by an earlier one (the water drop in flight)
        NoMover = 2,             // no unit, or no motion master: no order was possible
        NoCompletePath = 3,      // [nav24] no complete core path and no leader route: the walk gives the spot up
        DungeonPathRefused = 4,  // [sfk243] a dungeon path WalkBotPoints refused: no straight move either
    };

    // did this very call start the step
    bool StepStarted(StepStart step);

    // may the caller report success - the bot moves, by this order or by the one already in the air
    bool StepMoves(StepStart step);

    // the refusal in the words of the exit that refused; "" when nothing was refused
    char const* StepRefusal(StepStart step);

    // [lead372] a MoveTo "refusal" that refuses no X: the same walk is already on, or the game holds the bot (a
    // root, a stun, a cast). The walk goes once that ends, so the leader walks the plain path and keeps its order.
    // A navmesh or dungeon-path refusal is NOT one of these: it stands until the destination or the ground changes.
    bool TransientRefusal(std::string const& guard);

    // does the leader hear a refusal it has to decide on again (as against a walk that is simply under way)
    bool LeaderDecidesAgain(StepStart step);
}  // namespace Move

#endif
