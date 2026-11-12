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

// The group's damage per second by the formula (DpsEstimate per member); [lead1300] what this dungeon's fights dealt
// (GroupDamage below) goes to the log only. The model is rebuilt every 20 s (a cache).
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
    // [lead1303] user rule 2026-11-08: the rates a decision reads instead of the two above (log lines only) - a
    // member's formula damage (DpsEstimate on its victim, its group context; a healer heals: 0) and the sum of its
    // group's members present on its map (`bot` included)
    float MemberFormulaDps(Player* member);
    float GroupFormulaDps(Player* bot);
    // [combat_no_damage_release] True while the holds that keep a bot from fighting are lifted: the group of `bot`
    // (in a dungeon) is in combat - a member has a victim or attackers - and no member (nor its pet / totem) has dealt
    // damage for NO_DAMAGE_RELEASE_AFTER_MS; the release lasts NO_DAMAGE_RELEASE_MS, then the clock starts again.
    // Threat-open waits, approach holds, rest / pull holds and target vetoes ask it. Cheap: one lookup under a lock,
    // the group's combat state re-read at most every NO_DAMAGE_EVAL_MS.
    bool NoDamageRelease(Player* bot);
}

// [cast-short-life] How long `target` still lives, in seconds. [lead1300] user rule 2026-11-08: its health now over
// the formula damage (DpsEstimate against it) of the members on it now, the bot's own included; no measured rate (its
// observed health loss, the group's measured dps) is read. `source` (optional) names the estimate for the log.
// [guild13] a solo bot's own modelled damage per second (MemberModelDps, healers included)
float SoloModelDps(Player* member);
float ExpectedLifetime(PlayerbotAI* botAI, Unit* target, std::string* source = nullptr);
// [rot341] the same for a decision on a dot's full duration (wowsims' simTimeRemain); [lead1300] by the formula too
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
