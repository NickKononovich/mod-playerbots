/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILD_SUPPLYWAIT_H
#define PLAYERBOTS_GUILD_SUPPLYWAIT_H

#include <cstdint>
#include <string>

// [supply131] THE FIVE'S WAIT FOR A MEMBER'S SUPPLY RUN, AS A FACT THAT CAN COME OUT FALSE.
//
// Measured on b504 (the certification batch of wc-n, 5 fives at once, 45 min, not one run finished):
// three of five fives never left "gathering (0/5 at the entrance)", and in five 2 the two members
// #42 Cassand (mage) and #44 Blahadrin (warlock) stood at 0.0 yd for 2601 s each - the whole build -
// with pb_journal_idle.held_by EMPTY, 1201 of 1202 ticks "noTarget", 0 casts and a trail that ends
// "|A:check mount state - USELESS|A:follow - USELESS|no actions executed".
//
// Their task label never changed once, in all 45 min, down to the last line written:
//   [tasks] #42 Cassand (now DUNGEON Wailing Caverns five 2 (dps): waits for its supply run before
//   the entrance)
// That label has exactly one source, `st.supplyAsked && !st.supplyDone`, and FiveStep - the only
// writer of supplyDone - gives the member NO STEP while it stands, so [town20] leaves it no errand
// either: it is held by its five and the five is held by it.
//
// WHY THE WAIT COULD NOT END. FiveStep read the bare flag `rpgInfo.vendorRunEntry`, which is set for
// ANY vendor run, and answered "a run is live, keep waiting" forever. Cassand's live vendor run was
// not its restock at all - its own task line names it:
//   BUY[C,V,waits town Cragpool Lake] Glorious Shoulders for itself - vendor Nizzik 3357 yd, 86s 58c
// A run for a pair of shoulders 3357 yd away held a certification batch at the gate. The conclusion
// line FiveStep writes when the wait ends ("[supply27] ... supply run made / could not be made") was
// written 4 times in the whole build (Salleas, Irrua, Relara, Almored) and NEVER for these two: the
// wait truly never reached its end, it was not merely slow.
//
// This is the project's rule 5, one quantity and two judges: "is this member's live vendor run its
// supply run" is already answered by OnRestockRun (GuildFives.cpp) and by RestockRunPending
// (SoloSafety.cpp, [eco40]) through the run's own reason - and the place that decides the five's wait
// read neither. The weaker reading could not come out false, which is trap 5 of this project
// ("an assertion that cannot turn out false is not a check").
//
// NO TIMER ANYWHERE (rule 2). There is not one duration, age or timestamp in Facts, so no assertion
// below can be satisfied by waiting; the replacement for the unending flag is the run's own reason,
// a state fact. No threshold is introduced either: the bound this restores is the one the code
// already states in its own words ([dun282] "enters short never becomes never enters").
namespace GuildSupplyWait
{

// What a live vendor run is for, read from the run's own reason (the string NewRpgBaseAction
// ::StartVendorRun stored). `None` = this run is not a restock: a repair, a sale, an item purchase.
enum class Kind : uint8_t
{
    None = 0,
    Drink,
    Food,
};

// One place for "does this run's reason name a restock", so OnRestockRun's drink test and the five's
// wait cannot drift apart. Reads nothing but the string it is given.
// header-only on purpose: nothing but <string>, so the stand compiles it in seconds and no entry in
// the stand's source list is needed (tests/CMakeLists.txt is held by another agent right now)
inline Kind ReasonFor(std::string const& reason)
{
    // the very strings StartVendorRun stores for a restock ("buy drink", "buy food"); everything else
    // - "repair", "sell junk", an item purchase - is not this member's supplies
    if (reason.find("buy drink") != std::string::npos)
        return Kind::Drink;
    if (reason.find("buy food") != std::string::npos)
        return Kind::Food;
    return Kind::None;
}

inline bool IsRestock(std::string const& reason) { return ReasonFor(reason) != Kind::None; }

struct Facts
{
    bool asked = false;    // the five's gate asked this member to restock (st.supplyAsked)
    bool done = false;     // the wait already concluded (st.supplyDone)
    bool started = false;  // a restock run of its own was seen live at least once in this wait
    bool yielded = false;  // this wait already gave the member one pass to start a run

    // its vendor run is live AND the run's own reason names a restock (ReasonFor != None). A live
    // vendor run for anything else is NOT this fact: it says nothing about the member's supplies
    bool restockRunLive = false;
};

enum class Verdict : uint8_t
{
    NoWait = 0,   // nobody asked, or the wait is over: the member gets its five step
    Running,      // its own restock run is live: the wait stands, and it is marked as started
    Yield,        // nothing live yet: one pass for the run to start, then the wait ends
    Concluded,    // the run was made, or none started after a yield: the wait ends
};

inline Verdict Of(Facts const& facts)
{
    if (!facts.asked || facts.done)
        return Verdict::NoWait;
    // the member's OWN restock is under way: this is the only fact that keeps a five at the gate for
    // one member's supplies, and it stops being true the moment that run ends or turns into another
    if (facts.restockRunLive)
        return Verdict::Running;
    // "a run seen started and ended, or none started after a yield, ends the wait" - the rule the code
    // already stated in FiveStep; it is reachable again now that its premise can come out false
    if (facts.started || facts.yielded)
        return Verdict::Concluded;
    return Verdict::Yield;
}

}  // namespace GuildSupplyWait

#endif
