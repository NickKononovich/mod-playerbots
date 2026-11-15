/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MoveStep.h"

namespace Move
{
    bool StepStarted(StepStart step) { return step == StepStart::Started; }

    bool StepMoves(StepStart step) { return step == StepStart::Started || step == StepStart::AlreadyUnderWay; }

    char const* StepRefusal(StepStart step)
    {
        switch (step)
        {
            case StepStart::NoMover:
                return "no mover (the unit or its motion master is gone)";
            case StepStart::NoCompletePath:
                return "[nav24] no complete path to the spot (no leader route, the core's path is not complete)";
            case StepStart::DungeonPathRefused:
                return "[sfk243] the dungeon path was refused (its points climb a wall, no straight move)";
            case StepStart::Started:
            case StepStart::AlreadyUnderWay:
                break;
        }
        return "";
    }

    bool TransientRefusal(std::string const& guard)
    {
        return guard.empty() || guard.rfind("IsDuplicateMove", 0) == 0 || guard.rfind("IsWaitingForLastMove", 0) == 0 ||
               guard.rfind("IsMovingAllowed", 0) == 0 || guard.rfind("the walk ends where", 0) == 0;
    }

    bool LeaderDecidesAgain(StepStart step) { return !StepMoves(step) && !TransientRefusal(StepRefusal(step)); }
}  // namespace Move
