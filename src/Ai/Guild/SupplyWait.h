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

// [supply132] WHETHER A CONJURER IS STILL FILLING ITS OWN LINE - A FACT, NOT AN ABILITY.
//
// The second waiter of the same shape, in the same function (GuildFives.cpp, `shortfallInFlight`), and the
// one [supply130] removed from `drinkLine` and left standing in its twin:
//     (p->getClass() == CLASS_MAGE && p->HasSpell(5504))   // Conjure Water
// "it is a mage and it knows how to make water" is an ABILITY, constant by definition, so it is one more
// assertion that cannot come out false (trap 5) - and it holds `holdForPool` for ever: a five whose pool is
// short never enters while one member of it happens to be a mage, whatever that mage does or cannot do.
//
// MEASURED, and the bite is NOT measured - said plainly (rule 5 names a defect before it bites). b504 wrote
// [dun199] three times in the whole build and only for five 2, so this branch can bite exactly that five; and
// five 2 was NOT held by it in b504, because `st.supplyDone` was false there and `holdForPool` was never
// reached (that was [supply131], the shoulders run). What five 2 DID carry is the state this test needs:
// #42 Cassand, mage L25, pb_journal_actor at the gate "mana 178 of 1611 = 11.049 %, drinks 0, drinks_need 27",
// its [dun199] order "conjures water up to 29 before the run", and over 176 windows of b504 - 1 297 ticks -
// casts 0 and non-cast actions 0. An ability that says "water is on its way" while no water is on its way.
//
// THE REPLACEMENT IS STATE, AND IT CAN COME OUT FALSE: a conjurer is in motion while it is BELOW the line it
// was told to reach AND the core would take the cast NOW. No new threshold: the line is [dun199]'s own
// ConjureGoal (never under the member's priced line) and the castability is RestState::CanConjureDrink - the
// highest rank it knows, its own mana against that spell's own cost from spell data, and not in combat. No
// clock: there is no duration, age or timestamp here either, so no assertion below can be met by waiting.
struct ConjurerFacts
{
    bool knowsConjure = false;  // the class ability. Necessary and never sufficient: this is the old whole test
    uint32_t held = 0;          // drinks in its bags NOW (CountBagFood, the counter the pool itself is counted with)
    uint32_t goal = 0;          // the line [dun199] told it to reach; 0 = nobody has ordered it to conjure yet
    bool canCastNow = false;    // RestState::CanConjureDrink: mana against the spell's own cost, and not in combat
};

// Is this member's own water actually on its way? One place, so the gate's reading and a reader's cannot drift.
inline bool ConjurerFilling(ConjurerFacts const& facts)
{
    if (!facts.knowsConjure)
        return false;
    // at or over the line there is nothing left for it to make: its bags already answer the five's requirement,
    // and goal 0 (nobody has ordered it to conjure yet) is "at the line" by the same arithmetic
    if (facts.held >= facts.goal)
        return false;
    // below the line it is only "in motion" while the game would take the cast now. A mage that cannot cast is
    // standing, and a five must never wait on standing - that is the bound [dun282] asked for in its own words,
    // "enters short never becomes never enters"
    return facts.canCastNow;
}

}  // namespace GuildSupplyWait

#endif
