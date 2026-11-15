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

    uint32_t TravelTicks(Counts const& counts)
    {
        // The parts of the two biggest buckets in which the bot was walking. NOT added to MechanicTicks: a walk to
        // the next pack is the dungeon flow doing its job and a walk in circles is a defect, and 15 s of counters
        // cannot tell them apart - whether a walk had a purpose is the route domain's question. Naming the number
        // is honest; calling it legitimate would be inventing.
        return counts.noTargetMoving + counts.rangeLosMoving;
    }

    uint32_t UnaskedTicks(Counts const& counts)
    {
        // [jrn55] The buckets the chain decides BEFORE the engine's own reason is read. The refusal and the name
        // of the action behind it are recorded on every tick and counted only on a tick that reaches `other`, so
        // on these the answer existed and was discarded. Three names, in the chain's own order, and nothing else:
        // `gcd`, `threat` and `leaderHold` are asked after it, and `other` is where it is read.
        return counts.noTarget + counts.rangeLos + counts.moving;
    }

    bool NamesNoCause(Counts const& counts)
    {
        // Equality against this window's own total: no threshold, nothing to tune. Either some idle tick reached
        // the point where the engine says why, or none did.
        uint32_t const idle = IdleTicks(counts);
        return idle > 0 && UnaskedTicks(counts) == idle;
    }

    Tick Classify(TickFacts const& facts)
    {
        if (facts.casting)
            return Tick::CastWait;
        if (facts.cast)
            return Tick::Cast;
        if (facts.actionRan)
            return Tick::Action;
        // [jrn52] AND HERE THE MOVEMENT QUESTION COMES FIRST, inside each of the two branches that used to
        // swallow it. Walking with nothing to attack yet and standing with nothing to attack are not the same
        // tick; neither are running at a mob and stopping short of one.
        if (!facts.hasLiveTarget)
            return facts.moving ? Tick::NoTargetMoving : Tick::NoTargetStill;
        if (facts.outOfReachOrLos)
            return facts.moving ? Tick::RangeLosMoving : Tick::RangeLosStill;
        if (facts.moving)
            return Tick::Moving;
        return Tick::AskGcd;
    }

    Tick ClassifyAfterGcd(bool gcdRunning, bool leaderHold)
    {
        if (gcdRunning)
            return Tick::Gcd;
        if (leaderHold)
            return Tick::LeaderHold;
        return Tick::AskThreat;
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
        std::string why = "the engine itself counted " + number(idle) + " of " + number(counts.ticks) +
                          " ticks that produced nothing and says in which bucket, and " + number(idle - mechanic) +
                          " of them are named by neither the global cooldown nor the leader's command nor the "
                          "threat hold";
        // [jrn55] ... and the sentence stops implying that the bucket it names IS the cause when the engine was
        // never asked. On every tick the engine records WHAT it would have done and WHY it did not, and that pair
        // is counted only on a tick that reaches `other`; `noTarget`, `rangeLos` and `moving` are decided first.
        // A window whose whole idle sits in those three therefore names a bucket and no cause at all - measured:
        // every one of the 38 090 idle ticks of a healer in a dungeon in combat (1 028 windows, three builds).
        if (NamesNoCause(counts))
            why += "; and NOT ONE of them reached the point where the engine says why, because all " +
                   number(UnaskedTicks(counts)) +
                   " sit in the buckets decided before that question (" + number(counts.noTarget) +
                   " with no live target, " + number(counts.rangeLos) + " with one out of reach or sight, " +
                   number(counts.moving) +
                   " still moving to one in reach) - so this window names a bucket and no cause, the engine's own "
                   "answer existed on each of those ticks and was not counted, and the verdict stays yes because "
                   "the idle is real even though its reason was never recorded";
        if (uint32_t const travel = TravelTicks(counts))
            why += "; of those, " + number(travel) +
                   " are ticks in which the bot was WALKING (" + number(counts.noTargetMoving) +
                   " with nothing to attack yet, " + number(counts.rangeLosMoving) +
                   " on its way to a target out of reach), which is the dungeon flow when the walk has a purpose "
                   "and a defect when it has none - a question for the route domain and not for this window, so "
                   "the verdict stays yes rather than invent an answer";
        return why;
    }

    char const* RealWhen()
    {
        return "at least one idle bucket is non-zero, the window does not contradict itself (a global cooldown "
               "with no cast, no action and no cast in flight) and the idle ticks are not all the game's global "
               "cooldown, the leader's command and the threat hold";
    }
}  // namespace Journal::IdleWindow
