/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_CHAINPULLJUDGE_H
#define PLAYERBOTS_LEADER_CHAINPULLJUDGE_H

// [test1] The chain pull judge's numeric gates, as a seam: the facts go in as a structure, the verdict comes out,
// and nothing here reads the world, the clock or the database. Leader::Fight::JudgeChainPull (PullFacts.cpp) gathers
// the facts from the map as it always did and calls these predicates at the points where it used to spell the
// arithmetic out; the expressions are the same ones, character for character, so no bot decision changes.
//
// Why a seam at all: a recorded decision (pb_journal_option, the twenty chain pull numbers) cannot be replayed
// through JudgeChainPull - its pack is a list of ObjectGuid resolved through the map, and the map is gone once the
// run ended. The facts below are exactly what the journal records, so a recorded situation can be put back in and
// the same gate run over it offline, with no server.
//
// The clock is a FACT HERE, not a system call: `nowMs` comes in from the fixture like every other number. Nothing
// in this header calls getMSTime or GameTime, which is what makes a replay repeatable.

#include <cstdint>

namespace Leader::Judge
{
    // A world fact the judge read, as a fixture carries it. A fixture that does not carry it is Unknown, which is
    // NOT false: "the emitter did not record it" and "the gate did not trip" are different answers (the journal's
    // rule: absence is not zero).
    enum class Tri : uint8_t
    {
        Unknown = 0,
        No,
        Yes
    };

    // The timing the judge measured for one chain pull: the fight it is already in, and the three legs of taking the
    // next pack (the tank's walk with the fight, the pack's run in, the opener's cast and flight).
    struct ChainTiming
    {
        float fightSecLeft = 0.0f;
        float walkSec = 0.0f;
        float runInSec = 0.0f;
        float openSec = 0.0f;
    };

    // [lead1170] the trigger: the pull is due when the fight it is in has less left than taking the next pack costs.
    // The expression JudgeChainPull used inline; nothing else about it changed.
    inline bool DueNow(ChainTiming const& t)
    {
        return t.fightSecLeft < t.walkSec + t.runInSec + t.openSec;
    }

    // The survival model of the joined fight: the tank's health now (share of its own) against the deepest drop the
    // judge modelled over that fight.
    struct ChainSurvival
    {
        float tankHealth = 0.0f;
        float drop = 0.0f;
    };

    // [lead1170] the health gate: the tank must stand above the drop its own model predicts. The judge taking a
    // fight whose modelled margin is not positive is the defect a death test looks for - a death counts here as an
    // error of this calculation, not as bad luck.
    inline bool TankOutlastsDrop(ChainSurvival const& s)
    {
        return s.tankHealth > s.drop;
    }

    // Everything one recorded chain pull judgement carries. Numbers the emitter did not record are negative, flags
    // it did not record are Unknown; a replay says "undecided" rather than guessing them.
    struct ChainPullFacts
    {
        // the clock as the judge read it (0: the fixture carries none). An input, never read from the machine.
        uint32_t nowMs = 0;

        ChainTiming timing;
        ChainSurvival survival{-1.0f, -1.0f};

        float distance = -1.0f;   // tank to the pack's anchor
        float walkYd = -1.0f;     // the tank's walk with the fight
        float overload = -1.0f;   // the overload gate's load of the joined fight
        float unionSec = -1.0f;   // the joined fight's seconds
        float groupDps = -1.0f;
        float healerSec = -1.0f;

        int32_t packMobs = -1;    // the next pack
        int32_t fightMobs = -1;   // the fight it is in
        int32_t unionMobs = -1;   // the two joined, as the judge counted them
        int32_t stop = -1;        // the stop's index in the route plan

        bool walk = false;        // taken as a walk of the fight to the opener's reach
        bool due = false;         // the trigger, as the judge recorded it
        bool taken = false;       // the verdict, as the judge recorded it
        bool healerWalks = false;
        bool hasHealer = false;   // a healer that keeps the landing was named

        // the world facts, in the order JudgeChainPull reads them
        Tri bossInPack = Tri::Unknown;
        Tri walkerStop = Tri::Unknown;
        Tri openerReaches = Tri::Unknown;   // from where the tank stands, or by the walk's end
        Tri packReached = Tri::Unknown;
        Tri groupBuffed = Tri::Unknown;
        Tri everyoneAlive = Tri::Unknown;
        Tri tankHolds = Tri::Unknown;
        Tri overloadFits = Tri::Unknown;
        Tri healerKeeps = Tri::Unknown;
        Tri healerHasMana = Tri::Unknown;
        Tri wayInQuiet = Tri::Unknown;      // nothing idle woken on the way in
    };

    // The gate a replay stopped at. Named after the kinds JudgeChainPull writes into the journal, so a replayed
    // verdict and a recorded one compare as themselves.
    enum class Gate : uint8_t
    {
        Undecided = 0,  // the fixture does not carry what this gate reads
        Boss,
        Walker,
        Opener,
        Waits,
        PackReach,
        Buff,
        Dead,
        Holds,
        Overload,
        Health,
        Healer,
        Mana,
        Wakes,
        Taken
    };
    char const* GateName(Gate gate);

    struct ChainPullReplay
    {
        Gate gate = Gate::Undecided;
        bool due = false;      // recomputed from the facts, not copied from the fixture
        bool taken = false;
    };

    // Runs the chain pull's gates over recorded facts in the order JudgeChainPull runs them, through the very same
    // DueNow and TankOutlastsDrop the production path calls (they cannot drift: there is one copy of each). A gate
    // whose fact the fixture does not carry ends the replay as Undecided - the replay reports what it could not
    // decide instead of inventing it.
    ChainPullReplay ReplayChainPull(ChainPullFacts const& facts);
}

#endif
