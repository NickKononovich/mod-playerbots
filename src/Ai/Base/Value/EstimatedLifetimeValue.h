/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ESTIMATEDLIFETIMEVALUE_H
#define PLAYERBOTS_ESTIMATEDLIFETIMEVALUE_H

#include "NamedObjectContext.h"
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

// [lifetime-calibrated] The group's damage per second: the rolling average its recent dungeon fights actually dealt
// (GroupDamage below) once one is measured, else the gear-table model. Re-read every 2 s so the next pull already
// uses the fight that just ended; the model itself (a gear score per member) is rebuilt every 20 s only.
class EstimatedGroupDpsValue : public FloatCalculatedValue
{
public:
    EstimatedGroupDpsValue(PlayerbotAI* botAI)
        : FloatCalculatedValue(botAI, "estimated group dps", 2 * IN_MILLISECONDS) {}

public:
    float Calculate() override;

    // The gear-table model alone (each member's basic dps by level and role, scaled by its gear score).
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
// up to GROUP_HIT_GAP_CAP_MS each (a pull back round a corner is no fighting time). A fight of at least
// GROUP_FIGHT_MIN_MS goes into the group's rolling average (the newest weighs GROUP_DPS_WEIGHT). Kept per group,
// shared between map threads behind a lock; forgotten after GROUP_DPS_TTL_MS or when the group's size changes.
namespace GroupDamage
{
    void NoteDealt(Player* owner, uint32 amount);
    // The group's measured damage per second, 0 when no fight of it is known (or `bot` is in no group / dungeon).
    float CalibratedDps(Player* bot, uint32* fights = nullptr);
    // [lead86] the group's measured damage over its measured fighting seconds in this run (every fight weighed by its
    // length: a long fight - a boss - is not pulled down by short trash pulls), 0 when none is known
    float SustainedDps(Player* bot);
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
float ObservedLifetime(Unit* target);
float ExpectedLifetime(PlayerbotAI* botAI, Unit* target, std::string* source = nullptr);

#endif
