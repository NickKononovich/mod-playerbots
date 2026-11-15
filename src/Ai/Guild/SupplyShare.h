/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILD_SUPPLYSHARE_H
#define PLAYERBOTS_GUILD_SUPPLYSHARE_H

// [supply130] THE FIVE'S DRINK, AS A SEAM: the members' bags and their priced lines go in, the five's pool, its
// requirement and the share of a short pool come out, and nothing here reads the world, the clock or the database.
// Header-only on purpose - it depends on nothing of the server, so the unit stand compiles it in a second.
//
// WHY A SEAM AND NOT TWO PLACES COUNTING THE SAME DRINKS: the gate (GuildFives.cpp) and the in-run share
// (GuildTransfer.cpp waterFill) each counted the five's drink their own way, and they disagreed about the same
// five by exactly the conjuring mages' lines. Measured on b502, wc-n five 1 (the certification batch's run 681):
//
//   member      role    held  line
//   Bami        tank      6     7
//   Rumanatol   healer    3     2
//   Orton       mage      4    11
//   Thamesh     mage     10    10
//   Arrengyl    dps       6     6
//
// The gate excused both mages (they can conjure water) AND left their bags out of the pool, so it compared 15
// against 15 and let the five in. The in-run share counts every mana user with its full priced line and read the
// same five as "pool 29 of the lines' 36, short" eight seconds later. One quantity, two judges - the user's rule
// 5 - and the run that followed is what it cost: the share drained the pool from 29 to 2 in 13 minutes, the
// paladin tank held 0 of its 7 from 10:37:54 on, and the leader held the pull "the tank rests" for 1762 s of the
// 3033 s run (58 %) at 3 of 8 bosses, with no death and no wipe.
//
// THE LINE IS THE RUN'S LINE FOR EVERY MANA USER (GuildTransfer::RunLine, the leader's priced drinkNeed). A mage
// that conjures answers its line BY CONJURING, which puts the water in its bags where the pool can see it - the
// conjure order ([dun199]) is given at the gate for exactly that. Being able to make water is not the same fact
// as holding it, and only the second one carries a run.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

// ИМЯ: НЕ `Guild::Supply`. У ядра есть `class Guild`, и тогда компилятор ищет `Supply` ВНУТРИ
// класса, а не в пространстве имён: шов собирается отдельно и падает в сервере
// (error C2641 на `std::vector<Guild::Supply::Member>`). Поймано первой же серверной сборкой b503.
namespace GuildSupply
{
    enum class Role : uint8_t
    {
        Tank,
        Healer,
        Other
    };

    // One mana user of the five: what its bags hold now and the line the run priced for it.
    struct Member
    {
        uint32_t held{0};
        uint32_t line{0};
        Role role{Role::Other};
        // whether it can make its own water (a mage with Conjure Water). A FACT OF THE MEMBER AND OF NOTHING
        // ELSE: it changes neither the pool nor the requirement below, because conjuring answers the line by
        // putting the water in the bags, where the pool already counts it.
        bool conjures{false};
    };

    // The five's pool: every mana user's bags. No member is left out of it - a member whose drink is not in the
    // pool is a member the pool cannot hand to anybody.
    inline uint32_t PoolDrinks(std::vector<Member> const& members)
    {
        uint32_t drinks = 0;
        for (Member const& m : members)
            drinks += m.held;
        return drinks;
    }

    // What the run asks of the five: every mana user's line.
    inline uint32_t PoolNeed(std::vector<Member> const& members)
    {
        uint32_t need = 0;
        for (Member const& m : members)
            need += m.line;
        return need;
    }

    // Whether the five's pooled drink carries the run. `roomForEveryShortMember` is the hand-over's own fact
    // ([supply104]: a member below its line takes a stack only into a free bag slot) and stays the caller's to
    // read from the world.
    inline bool Pooled(std::vector<Member> const& members, bool roomForEveryShortMember)
    {
        return roomForEveryShortMember && PoolDrinks(members) >= PoolNeed(members);
    }

    // THE SHARE OF A SHORT POOL, in whole drinks, summing to the pool. The order is the order in which a dry bar
    // stops the five:
    //   1. the healer to its line ([dun230]: its mana gates every pull; a dps short of mana still deals its white
    //      damage),
    //   2. then the TANK to its line - the leader holds the pull on the tank's mana, which is what 58 % of run
    //      681 was spent on, so a share that leaves the tank dry contradicts the arithmetic the leader judges by,
    //   3. then the rest in proportion to their lines, the remainders handed out largest first,
    //   4. and, the pool holding at least one a member, a member whose share came out 0 takes one from the
    //      largest share - never from the healer's and never from the tank's.
    // A member's share is both the most it may receive and the least it must give down to.
    inline std::vector<uint32_t> ShortPoolTargets(std::vector<Member> const& members, uint32_t pool)
    {
        std::vector<uint32_t> target(members.size(), 0);
        uint64_t sharedPool = pool, sharedLines = PoolNeed(members), given = 0;
        size_t const none = members.size();
        size_t healer = none, tank = none;
        for (size_t n = 0; n < members.size(); ++n)
        {
            if (members[n].role == Role::Healer && healer == none)
                healer = n;
            else if (members[n].role == Role::Tank && tank == none)
                tank = n;
        }
        auto const first = [&](size_t n)
        {
            if (n == none)
                return;
            target[n] = uint32_t(sharedPool < members[n].line ? sharedPool : members[n].line);
            given += target[n];
            sharedPool -= target[n];
            sharedLines -= sharedLines < members[n].line ? sharedLines : members[n].line;
        };
        first(healer);
        first(tank);

        std::vector<std::pair<uint64_t, size_t>> rest;  // (remainder, member)
        for (size_t n = 0; n < members.size(); ++n)
        {
            if (n == healer || n == tank || !sharedLines)
                continue;
            uint64_t const exact = sharedPool * members[n].line;
            target[n] = uint32_t(exact / sharedLines);
            given += target[n];
            rest.emplace_back(exact % sharedLines, n);
        }
        std::stable_sort(rest.begin(), rest.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        for (size_t k = 0; given < pool && k < rest.size(); ++k, ++given)
            ++target[rest[k].second];
        if (pool >= members.size())
            for (size_t n = 0; n < members.size(); ++n)
            {
                if (target[n] || n == healer || n == tank)
                    continue;
                size_t most = n;  // never out of the healer's or the tank's share
                for (size_t m = 0; m < target.size(); ++m)
                    if (m != healer && m != tank && target[m] > target[most])
                        most = m;
                if (target[most] < 2)
                    break;
                --target[most];
                target[n] = 1;
            }
        return target;
    }
}

#endif
