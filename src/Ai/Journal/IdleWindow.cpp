/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IdleWindow.h"

namespace Journal::IdleWindow
{
    uint32_t IdleTicks(Counts const& counts)
    {
        return counts.noTarget + counts.rangeLos + counts.moving + counts.gcd + counts.threat + counts.leaderHold +
               counts.other;
    }

    uint32_t MechanicTicks(Counts const& counts)
    {
        // The global cooldown is the game's own mechanic and the leader's command is an order nobody argues with;
        // the threat hold is this project's own rule about not overtaking the tank. A tick spent on any of the
        // three produced nothing AND was right to produce nothing, which is a different fact from a tick with no
        // target at all - so it is counted apart and never subtracted from the row, which keeps every number.
        return counts.gcd + counts.leaderHold + counts.threat;
    }

    bool ActivityRecorded(Counts const& counts)
    {
        // Three different things count, and leaving any of them out would read as "the bot did nothing" for a bot
        // that was in the middle of a cast: `castWait` is a cast ALREADY IN FLIGHT and explains a global cooldown
        // just as a finished cast does.
        return counts.casts || counts.nonCast || counts.castWait;
    }

    bool ContradictsItself(Counts const& counts)
    {
        // A global cooldown runs because a spell was cast. A window that counts one while recording nothing done
        // at all is not improbable, it is impossible - and until the recorder's own blind spot is closed, such a
        // window says more about the instrument than about the bot.
        return counts.gcd > 0 && !ActivityRecorded(counts);
    }

    char const* Real(Counts const& counts)
    {
        uint32_t const idle = IdleTicks(counts);
        if (!idle)
            return "no";  // the window is printed every cadence whether or not anything idled
        if (ContradictsItself(counts))
            return "no";
        if (idle == MechanicTicks(counts))
            return "no";
        return "yes";
    }

    std::string RealWhy(Counts const& counts)
    {
        uint32_t const idle = IdleTicks(counts);
        uint32_t const mechanic = MechanicTicks(counts);
        auto const number = [](uint32_t value) { return std::to_string(value); };
        if (!idle)
            return "every idle bucket of this window was zero: the window is printed on its own cadence whether or "
                   "not anything idled, so this is a clean instrument result and not a problem";
        if (ContradictsItself(counts))
            return "this window disagrees with itself and the instrument is the suspect, not the bot: it counts " +
                   number(counts.gcd) +
                   " ticks on the global cooldown, which only runs because a spell was cast, while recording no "
                   "cast, no executed action and no cast in flight over all " +
                   number(counts.ticks) +
                   " ticks - both cannot be true, so the bucket this window's key names may not be the cause at all";
        if (idle == mechanic)
            return "all " + number(idle) +
                   " ticks that produced nothing were right to: " + number(counts.gcd) +
                   " on the game's own global cooldown, " + number(counts.leaderHold) +
                   " under the leader's command, " + number(counts.threat) +
                   " held back by the bot's own threat on the target. The duration stays in the row; the problem, "
                   "if there is one, belongs to whatever issued that command and is recorded there";
        return "the engine itself counted " + number(idle) + " of " + number(counts.ticks) +
               " ticks that produced nothing and says in which bucket, and " + number(idle - mechanic) +
               " of them are named by neither the global cooldown nor the leader's command nor the threat hold";
    }

    char const* RealWhen()
    {
        return "at least one idle bucket is non-zero, the window does not contradict itself (a global cooldown "
               "with no cast, no action and no cast in flight) and the idle ticks are not all the game's global "
               "cooldown, the leader's command and the threat hold";
    }
}  // namespace Journal::IdleWindow
