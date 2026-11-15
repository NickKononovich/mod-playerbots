/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_JOURNAL_ACTORFACTS_H
#define _PLAYERBOT_JOURNAL_ACTORFACTS_H

// [jrn50] THE PARTICIPANTS OF A DECISION, out of the judge's own `fields` and into rows that can be filtered.
//
// This is a seam on purpose: it depends on nothing but <string>, <vector> and <optional>, so the translation from
// what the emitter wrote to what lands in pb_journal_actor is testable without a world, a server or a database.
// The gap it closes was measured, not guessed: pb_journal_actor held no bot row at all and every mob's level,
// health and mana was NULL, which is exactly what made the two DeathReplay tests skip - "put the same conditions
// back and run the judge again" had nothing to put back.
//
// THE ONE RULE THIS FILE EXISTS TO ENFORCE: an absent key is an EMPTY optional, never a zero. A mob at 0 % health
// is a corpse; a mob whose health was never written is a gap. Both answer a query, and only one of them answers it
// truthfully, so every reader here is presence-aware and nothing is defaulted to 0.
//
// It also NEVER looks a unit up. Everything comes from the line the judge already wrote, in the judge's own pass
// under the judge's own lock; asking the world again here would be a second reading at a second moment - a second
// judge of one quantity.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Journal
{
    namespace Facts
    {
        // One participant as the emitter described it. Every measured number is optional, and empty means "the
        // emitter did not supply it".
        struct ActorFact
        {
            bool mob = false;  // true: a creature the judge weighed; false: a bot of the five

            std::string name;
            uint32_t low = 0;      // GUID counter: the identity (a name is not one)
            uint32_t entry = 0;    // creature entry; 0 for a bot
            uint32_t spawnId = 0;  // creature spawn id, when the judge named the spawn and not a live unit

            std::optional<uint32_t> level;
            std::optional<std::string> spec;
            std::optional<std::string> role;  // "tank" | "healer" | "dps" in THIS five
            std::optional<uint32_t> health;
            std::optional<uint32_t> healthMax;
            std::optional<float> healthPct;
            std::optional<uint32_t> mana;
            std::optional<uint32_t> manaMax;
            std::optional<float> manaPct;
            std::optional<float> x;
            std::optional<float> y;
            std::optional<float> z;
            std::optional<float> tankYd;
            std::optional<float> targetYd;
            std::optional<bool> inCombat;
            std::optional<bool> wanders;
            std::optional<bool> openerReady;
            std::optional<std::string> doing;

            // [jrn50] the supply facts of a member, which is what the wc-n failure turned on: a paladin tank ran out
            // of water and the five spent 58 % of the run resting, and the only way to establish that was a LIKE over
            // the text. `drinks` / `foods` are what it CARRIES, `drinksNeed` what its run was priced at.
            std::optional<uint32_t> drinks;
            std::optional<uint32_t> foods;
            std::optional<uint32_t> drinksNeed;

            // the checks a mob passed, as the judge's text AND as fields: a spawn that FAILED a check is a rejected
            // candidate, and a comma-separated string cannot be filtered
            std::optional<std::string> checks;
            std::optional<bool> chkReachesSpot;
            std::optional<bool> chkPullPath;
            std::optional<bool> chkWalk;
            std::optional<bool> chkWalksIntoFight;

            std::string note;  // why this participant is in the record ("pack member", "wander area", "five", ...)
        };

        // Every participant the emitter put in the decision's `fields`:
        //   "five":      [{name, low, level, role, spec, hp, hpMax, mana, manaMax, x, y, z, tankYd, targetYd,
        //                  combat, openerReady, drinks, foods, drinksNeed, doing}]   - the bots of the five
        //   "packAt":    [{low, entry, x, y, z, tankYd, wanders, level, hp, hpMax, combat}]  - the pack aimed at
        //   "wanderArea":[{spawn, checks}]                                  - every wandering spawn judged
        //   "lookAt":     {low, entry, ...}                                 - the single mob the answer is about
        // Keys the emitter left out come back as empty optionals.
        std::vector<ActorFact> ActorsFromFields(std::string const& json);

        // A share, and only ever beside its own denominator: empty when either side is absent, and empty when the
        // denominator is zero (a bot with no mana pool has no mana percentage - it does not have 0 %).
        std::optional<float> Share(std::optional<uint32_t> const& part, std::optional<uint32_t> const& whole);

        // The flat readers, exposed because the same presence rule has to hold for the callers that read one key.
        std::optional<std::string> Text(std::string const& json, char const* key);
        std::optional<uint32_t> UInt(std::string const& json, char const* key);
        std::optional<float> Float(std::string const& json, char const* key);
        std::optional<bool> Bool(std::string const& json, char const* key);
    }  // namespace Facts
}  // namespace Journal

#endif
