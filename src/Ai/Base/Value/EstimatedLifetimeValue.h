/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ESTIMATEDLIFETIMEVALUE_H
#define PLAYERBOTS_ESTIMATEDLIFETIMEVALUE_H

#include "NamedObjectContext.h"
#include "ObjectGuid.h"
#include "PossibleTargetsValue.h"
#include "TargetValue.h"
#include "Value.h"

#include <string>  // [b35_mvaway] ExpectedLifetime source

class Player;
class PlayerbotAI;
class Unit;

// [target health] / [expected group single target dps] = [expected lifetime]
class EstimatedLifetimeValue : public FloatCalculatedValue, public Qualified
{
public:
    EstimatedLifetimeValue(PlayerbotAI* botAI) : FloatCalculatedValue(botAI, "estimated lifetime") {}

public:
    float Calculate() override;
};

// [lifetime-calibrated] The group's damage per second: what this dungeon's fights actually dealt (GroupDamage below)
// once one is measured, else the members' own model ([lead265] their weapons and spells). Re-read every 2 s so the
// next pull already uses the fight that just ended; the model itself is rebuilt every 20 s only (a cache).
class EstimatedGroupDpsValue : public FloatCalculatedValue
{
public:
    EstimatedGroupDpsValue(PlayerbotAI* botAI)
        : FloatCalculatedValue(botAI, "estimated group dps", 2 * IN_MILLISECONDS) {}

public:
    float Calculate() override;

    // [lead265] The model alone: each member's weapon and best damage spell from its own stats and spells.
    float ModelDps();

    // A damage dealer of `level` in the gear expected at it (also read by "next cast threat", ThreatValues.cpp).
    static float GetBasicDps(uint32 level);
    static float GetBasicGs(uint32 level);

private:
    float _modelDps = 0.0f;
    uint32 _modelMs = 0;
};

// [lifetime-calibrated] The damage a group really deals, measured from DealDamage (the death recorder's hook, after
// armor, resistances and absorbs, overkill cut off): every hit of a member (or its pet / totem) on a creature in a
// dungeon. A fight is the run of hits without a gap of GROUP_FIGHT_GAP_MS; its length counts the gaps between hits
// up to GROUP_HIT_GAP_CAP_MS each (a pull back round a corner is no fighting time). [lead265] Every fight goes into
// the group's average by its seconds (no minimum length, no newest-fight weight, no clock). Kept per group, shared
// between map threads behind a lock; reset when the dungeon or the group's size changes.
namespace GroupDamage
{
    void NoteDealt(Player* owner, uint32 amount);
    // The group's measured damage per second, 0 when no fight of it is known (or `bot` is in no group / dungeon).
    float CalibratedDps(Player* bot, uint32* fights = nullptr);
    // [lead86] the group's measured damage over its measured fighting seconds in this run (every fight weighed by its
    // length: a long fight - a boss - is not pulled down by short trash pulls), 0 when none is known
    float SustainedDps(Player* bot);
    // [lead271] one member's part of it: the group's sustained dps times the member's share of the group's measured
    // hits (its pet / totem included); 0 when none is known
    float MemberSustainedDps(Player* member);
    // [combat_no_damage_release] True while the holds that keep a bot from fighting are lifted: the group of `bot`
    // (in a dungeon) is in combat - a member has a victim or attackers - and no member (nor its pet / totem) has dealt
    // damage for NO_DAMAGE_RELEASE_AFTER_MS; the release lasts NO_DAMAGE_RELEASE_MS, then the clock starts again.
    // Threat-open waits, approach holds, rest / pull holds and target vetoes ask it. Cheap: one lookup under a lock,
    // the group's combat state re-read at most every NO_DAMAGE_EVAL_MS.
    bool NoDamageRelease(Player* bot);
}

// [cast-short-life] How long `target` still lives, in seconds: the model above (its health over the expected group
// dps, [lifetime-calibrated]: the group's measured dps, slowed against a mob of a higher level) or, when it is
// lower, the rate its health actually fell at over the last seconds. Sunken Temple 2026-10-25
// (st-n-5A-92): the model put the group at ~500 dps where it did 848, so a Slime Maggot dying in 2 s still looked
// worth a 2.5 s Frostbolt to the mage (32 pulls without a hit). Cheap: a few samples per unit on the map thread.
// [b35_mvaway] Once its health is seen falling the observed rate alone counts (the damage really dealt to it); the
// group's dps only stands in before that, cut to the share of the group on `target`. gd-h 20261026-024524-gd-h-5H-27:
// the whole group's 7855 dps put a Snake Wrap, hit by one or two members, at "lives 0.1 s" with 13 % health, and the
// mage held Fireball on it. `source` (optional) names the estimate for the log.
// [guild13] a solo bot's own modelled damage per second (MemberModelDps, healers included)
float SoloModelDps(Player* member);
float ObservedLifetime(Unit* target);
float ExpectedLifetime(PlayerbotAI* botAI, Unit* target, std::string* source = nullptr);
// [rot341] The same by the rate over the target's whole observed health loss (since its track began: first seen,
// healed or forgotten), not the last 0.7-4 s: wowsims' simTimeRemain for a decision on a dot's full duration
// (feral rotation.go ripNow endThresh). The 4 s rate swung with bursts: b221 rfc-h 5H Rtdruaf Bit at 5 points with
// Rip down on Oggleflint at ~37 % health (it lost 2.1 %/s over the fight and lived 18 s more), no Rip that fight.
// Falls back to ExpectedLifetime while no loss was seen.
float ExpectedFightLifetime(PlayerbotAI* botAI, Unit* target);

// [lead451] A member's held cast (CastTimeMultiplier): the target it would not live to the cast's landing on and the
// seconds the cast needs to land (its cast time, flight and the next tick). The member reports the fact; the leader's
// target pick reads it (a mob that dies before this member's cast lands goes to the members whose damage lands).
namespace CastLand
{
    struct Held
    {
        ObjectGuid target;
        float landsSec = 0.0f;
    };
    void Note(ObjectGuid member, ObjectGuid target, float landsSec);
    // false when the member never held a cast
    bool Get(ObjectGuid member, Held& held);
}

#endif
