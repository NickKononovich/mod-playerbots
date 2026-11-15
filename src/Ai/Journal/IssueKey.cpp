/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IssueKey.h"

namespace Journal::Key
{
    std::string Standing(std::string const& role, bool inCombat, uint32_t engineState, std::string const& heldBy,
                         std::string const& actionTrace)
    {
        // THE TRACE COMES IN AND DOES NOT GO INTO THE KEY, and that is this function's whole reason to exist:
        // Engine::GetLastAction() is the trace of an instant, it changes on every sample of one episode, and the
        // five rows it made of Kylantino's one episode are in the test beside this file. What is left is what this
        // detector can honestly call a cause: standing through a fight is a different problem from standing in
        // peace, and so is each state of the bot's AI. The trace stays in `why` and in pb_journal_idle.doing.
        //
        // THE ROLE IS A CAUSE and it is in the key, as it is in both sibling kinds (`idle|<role>|<buckets>` and
        // `stuck_target|<role>|...`): a healer standing still is not the same problem as a tank standing still,
        // and it is constant through an episode, so it narrows the key without splitting one episode. Measured on
        // b501's rows: 42 dps, 18 tank and 3 healer episodes out of combat, 3 tank and 2 dps in combat.
        //
        // [jrn42] THE FIRST CAUSE FIELD THIS KEY EVER HAD, and the request for it stood in this very comment:
        // "narrowing it further needs CAUSE FIELDS from the emitter ... and not a string that happens to differ".
        // `heldBy` is such a field - a closed set of names read as facts at the sample's instant, not a text that
        // varies - so it narrows without splitting one episode, and it is what separates a bot standing with no
        // order from a bot standing exactly where its leader told it to. The empty name is kept out of the key
        // instead of being spelled, so the keys already measured on b501 and b502 go on meaning what they meant.
        (void)actionTrace;
        return "standing|" + role + "|" + std::string(inCombat ? "combat" : "peace") + "|state " +
               std::to_string(engineState) + (heldBy.empty() ? std::string() : "|held: " + heldBy);
    }

    std::string StuckTarget(std::string const& role, std::string const& actionRunning)
    {
        // Whether the engine had anything at all to run is the cause ("no action of its combat engine ran on it");
        // WHICH actions it listed is the trace of that instant and belongs in pb_journal_stuck.action_running.
        return "stuck_target|" + role + "|" + (actionRunning.empty() ? "no action" : "an action ran");
    }
}
