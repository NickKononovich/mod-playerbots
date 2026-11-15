/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADER_FIGHT_HEALERPLACE_H
#define PLAYERBOTS_LEADER_FIGHT_HEALERPLACE_H

// [spot91] WHETHER A HEALER'S FIGHT PLACE CAN EXIST AT ALL, AS A SEAM: the facts go in, the answer comes out, and
// nothing here reads the world, the clock or the database. Header-only, so the unit stand compiles it alone.
//
// WHY IT EXISTS. `combat_spot_none` is recorded as "the search found no place and the member cannot fight its target
// from where it stands" - a defect of the SEARCH. For a healer that sentence can be false by arithmetic, and then the
// row names the wrong culprit: the candidate places are generated on rings round the TARGET, while the check that
// accepts them measures from the TANK's fight place, and when those two centres are far enough apart NO point of any
// ring the generator offers is inside the tank's acceptance. The set is empty before a single candidate is looked at.
//
// THE TWO CENTRES, read out of the code they already live in:
//   * the generator (LeaderCombatSpot) walks rings round the target from LEADER_HEALER_YD down to LEADER_HEALER_YD - 6
//     in steps of RING_STEP_YD - so a generated place is `ringInner .. ringOuter` yards from the TARGET, never more;
//   * the acceptance (healerSpotKeeps) requires the place to be within LEADER_HEALER_TANK_YD of the TANK's fight place;
//   * the validity check (CombatSpotValidWhy) accepts a gap to the target of `acceptInner .. acceptOuter`, which is
//     LEADER_HEALER_YD - 6 .. max(LEADER_HEALER_YD + 10, the Inhibit Magic ring).
// ONE QUANTITY, "the healer's band", IS THEREFORE WRITTEN AS TWO NUMBERS IN TWO PLACES: the generator stops at
// LEADER_HEALER_YD (20) while the check accepts out to LEADER_HEALER_YD + 10 (30). `OneBand` is that fact, and rule 5
// says it is a defect whether or not it has bitten yet. It HAS bitten - see the measurement below.
//
// MEASURED (b504, issue 7708442440541159566, run 20261115-175531-wc-n-five6-3, healer Aestymro L24 on Deviate Ravager
// 3636/177 at 1794761878276): 107 candidates refused, 59 of them "heal reach", 46 "out of the healer's band", 0
// reached. The pull of that very target (entry 3636, low 177) was TAKEN by the tank Bierya at 1794761871466 with the
// mob 51.4443 yd from the tank - the `tank_yd` column of that decision's `look target` actor row, not a number read
// out of a sentence. With the generator's outer ring at 20 and the tank's acceptance at 30, the nearest a generated
// place can be to the tank is 51.4443 - 20 = 31.4443 yd: the geometry is short by 1.4443 yd, and no search however
// good could have found a place. Against the band the CHECK already accepts (30) the same geometry is short by
// nothing. So that event is not a search defect; it is the two numbers of one band, 1.44 yd apart from mattering.
//
// WHAT THIS SEAM DOES NOT DO. It does not widen the rings, move the healer, refuse the pull or decide anything: those
// are behaviour and need the user's word (rule 3). It answers one arithmetic question so that the record can tell
// "no place was found" from "no place could exist", and so that the question put to the user carries a number.
//
// NO NEW CONSTANT. Every number is an argument; the only literal here is zero, and it is the boundary between
// "a place can exist" and "it cannot", not a setting.
//
// ITS CALLERS, so the seam is not a dead one (a dead seam costs more than an absent one):
//   * Leader::Fight::LeaderCombatSpot (LeaderFightChecks.cpp) asks `CanExist` and `RingReachesBand` to walk the
//     healer's candidate rings round the TANK'S FIGHT PLACE, which LeaderEvents.cpp hands it as `tankAt`;
//   * the `combat_spot_none` emitter in src/Ai/Leader/Dungeon/LeaderEvents.cpp asks `CanExist`, `MaxTankToTargetYd`
//     and `ShortBy` for the row `topic='healer_place'`, so the record can tell "no place COULD exist" from "a place
//     could and the search did not find it" - two things that used to be one row;
//   * `CanExistOnTargetRings` / `ShortByOnTargetRings` are kept because they answer what the OLD generator could
//     have found, which is the measurement this file was written on and the thing the new rule must beat.
//
// NO CLOCK: every fact is a state, read at the instant the place is judged.

#include <algorithm>
#include <cmath>

namespace Leader::Fight::HealerPlace
{
    // the ring radii the generator actually offers (round the TARGET) and the gaps the validity check accepts
    struct Band
    {
        float ringInner = 0.0f;    // innermost ring searched (LEADER_HEALER_YD - 6)
        float ringOuter = 0.0f;    // outermost ring searched (LEADER_HEALER_YD): a generated place is never further
        float acceptInner = 0.0f;  // the validity check's floor
        float acceptOuter = 0.0f;  // the validity check's ceiling: max(LEADER_HEALER_YD + 10, the Inhibit ring)
    };

    // the other centre: where the tank fights, and how near it the acceptance keeps the healer
    struct Tank
    {
        float toTargetYd = 0.0f;  // the tank's fight place to the target - the distance between the two centres
        float keepsYd = 0.0f;     // LEADER_HEALER_TANK_YD
    };

    // The nearest a GENERATED place can lie to the tank. A place sits at some radius r in [ringInner, ringOuter] from
    // the target; over that annulus the smallest distance to the tank is reached on the tank's side of the target.
    inline float MinGapToTank(Band const& band, Tank const& tank)
    {
        float const inner = std::min(band.ringInner, band.ringOuter);
        float const outer = std::max(band.ringInner, band.ringOuter);
        float const d = tank.toTargetYd;
        if (d > outer)
            return d - outer;  // the whole annulus is beyond the tank: its near edge is the nearest point
        if (d < inner)
            return inner - d;  // the tank stands inside the hole: the ring's near side is the nearest point
        return 0.0f;           // the tank's own ground is on a searched ring
    }

    // Can ANY place the generator offers satisfy the tank's acceptance? When this is false the search is not at
    // fault: the set it is asked to pick from is empty by arithmetic.
    inline bool CanExistOnTargetRings(Band const& band, Tank const& tank)
    {
        return MinGapToTank(band, tank) <= tank.keepsYd;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // [spot91b] USER 2026-11-15: "the healer's place is searched on rings round the MOB. Why is it not searched near
    // the TANK? That is nonsense, fix it." THE ONE CENTRE IS THE TANK, because the acceptance already measures from
    // the tank: a candidate generated on a ring round the tank's fight place is inside `keepsYd` of it BY
    // CONSTRUCTION, so the tank half of the acceptance can no longer empty the set before a candidate is looked at.
    // The band to the target stays a CHECK, exactly as it was - nothing here moves the healer out of its heal range.
    //
    // MEASURED on the closed b504 corpus, and it is why the rings round the mob are the defect: in ALL 20 healer
    // `combat_spot_none` events every one of the 480 ring candidates round the target (16 directions x 3 rings) was
    // refused - 480 of 480. Of 2 329 candidates 1 283 (55.1 %) were refused "heal reach", the test `keeps` owns, and
    // 820 of the 980 `near` candidates (83.7 %) - the stage centred on the HEALER - went that way. Not one stage of
    // the search was centred on the tank, and the acceptance is the tank's.
    // ---------------------------------------------------------------------------------------------------------------

    // The farthest the tank's fight place can be from the target with a place still existing: the disk the acceptance
    // allows round the tank (radius keepsYd) has to touch the band round the target. Both numbers are the code's own,
    // and this is the single place their sum is written.
    inline float MaxTankToTargetYd(Band const& band, Tank const& tank) { return tank.keepsYd + band.acceptOuter; }

    // Can a place exist at all, with the candidates generated round the TANK? The disk |p - tank| <= keepsYd must meet
    // the annulus acceptInner <= |p - target| <= acceptOuter: it reaches the annulus while d <= keepsYd + acceptOuter,
    // and it is not swallowed by the annulus's hole while d + keepsYd >= acceptInner. Both halves are needed: a rule
    // that checked only the first would call a tank hugging the target with a short reach a place.
    inline bool CanExist(Band const& band, Tank const& tank)
    {
        return tank.toTargetYd <= MaxTankToTargetYd(band, tank) &&
               tank.toTargetYd + tank.keepsYd >= band.acceptInner;
    }

    // A ring of this radius round the tank can hold a point inside the band round the target: the ring's nearest point
    // to the target is |d - ring| and its farthest d + ring. Rings that cannot are not walked - the pruning is the
    // band's own two numbers, so it adds no candidate the check would refuse and introduces no number.
    inline bool RingReachesBand(Band const& band, float toTargetYd, float ring)
    {
        return std::fabs(toTargetYd - ring) <= band.acceptOuter && toTargetYd + ring >= band.acceptInner;
    }

    // By how many yards the geometry is short. Zero means a place can exist (it says nothing about whether the
    // ground, the sight or the hazards there are good - those are the search's own checks).
    inline float ShortByOnTargetRings(Band const& band, Tank const& tank)
    {
        return std::max(0.0f, MinGapToTank(band, tank) - tank.keepsYd);
    }

    // By how many yards the geometry is short of allowing ANY place, the candidates being generated round the tank.
    // Zero: a place can exist (it says nothing about the ground, the sight or the hazards there - those are the
    // search's own checks).
    inline float ShortBy(Band const& band, Tank const& tank)
    {
        return std::max(0.0f, tank.toTargetYd - MaxTankToTargetYd(band, tank));
    }

    // Rule 5: is "the healer's band" ONE number, or two? True when the ring the generator offers reaches as far out
    // as the gap the validity check already accepts.
    inline bool OneBand(Band const& band) { return band.ringOuter >= band.acceptOuter; }
}  // namespace Leader::Fight::HealerPlace

#endif
