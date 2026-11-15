/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IssueKey.h"

namespace Journal::Key
{
    std::string Standing(std::string const& role, bool inCombat, uint32_t engineState,
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
        // THE KEY IS STILL WIDE, AND THAT IS STATED RATHER THAN HIDDEN: its largest group covers 42 rows over 17
        // bots and 4 maps, because this detector holds no field that tells one cause of standing from another.
        // Narrowing it further needs CAUSE FIELDS from the emitter - the way the idle diagnostics has buckets and
        // keys on the combination of the non-zero ones - and not a string that happens to differ.
        (void)actionTrace;
        return "standing|" + role + "|" + std::string(inCombat ? "combat" : "peace") + "|state " +
               std::to_string(engineState);
    }

    std::string StuckTarget(std::string const& role, std::string const& actionRunning)
    {
        // Whether the engine had anything at all to run is the cause ("no action of its combat engine ran on it");
        // WHICH actions it listed is the trace of that instant and belongs in pb_journal_stuck.action_running.
        return "stuck_target|" + role + "|" + (actionRunning.empty() ? "no action" : "an action ran");
    }
}
