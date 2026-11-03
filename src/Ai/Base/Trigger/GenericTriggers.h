/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GENERICTRIGGERS_H
#define PLAYERBOTS_GENERICTRIGGERS_H

#include "HealthTriggers.h"
#include "Player.h"
#include "RangeTriggers.h"
#include "Trigger.h"
#include <utility>

class PlayerbotAI;
class Unit;

class StatAvailable : public Trigger
{
public:
    StatAvailable(PlayerbotAI* botAI, int32 amount, std::string const name = "stat available")
        : Trigger(botAI, name), amount(amount) {}

protected:
    int32 amount;
};

class HighManaTrigger : public Trigger
{
public:
    HighManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "high mana") {}

    bool IsActive() override;
};

class EnoughManaTrigger : public Trigger
{
public:
    EnoughManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "enough mana") {}

    bool IsActive() override;
};

class AlmostFullManaTrigger : public Trigger
{
public:
    AlmostFullManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "almost full mana") {}

    bool IsActive() override;
};

class RageAvailable : public StatAvailable
{
public:
    RageAvailable(PlayerbotAI* botAI, int32 amount) : StatAvailable(botAI, amount, "rage available") {}

    bool IsActive() override;
};

class LightRageAvailableTrigger : public RageAvailable
{
public:
    LightRageAvailableTrigger(PlayerbotAI* botAI) : RageAvailable(botAI, 20) {}
};

class MediumRageAvailableTrigger : public RageAvailable
{
public:
    MediumRageAvailableTrigger(PlayerbotAI* botAI) : RageAvailable(botAI, 40) {}
};

class HighRageAvailableTrigger : public RageAvailable
{
public:
    HighRageAvailableTrigger(PlayerbotAI* botAI) : RageAvailable(botAI, 60) {}
};

class EnergyAvailable : public StatAvailable
{
public:
    EnergyAvailable(PlayerbotAI* botAI, int32 amount) : StatAvailable(botAI, amount, "energy available") {}

    bool IsActive() override;
};

class LightEnergyAvailableTrigger : public EnergyAvailable
{
public:
    LightEnergyAvailableTrigger(PlayerbotAI* botAI) : EnergyAvailable(botAI, 20) {}
};

class MediumEnergyAvailableTrigger : public EnergyAvailable
{
public:
    MediumEnergyAvailableTrigger(PlayerbotAI* botAI) : EnergyAvailable(botAI, 40) {}
};

class HighEnergyAvailableTrigger : public EnergyAvailable
{
public:
    HighEnergyAvailableTrigger(PlayerbotAI* botAI) : EnergyAvailable(botAI, 60) {}
};

class ComboPointsAvailableTrigger : public StatAvailable
{
public:
    ComboPointsAvailableTrigger(PlayerbotAI* botAI, int32 amount = 5)
        : StatAvailable(botAI, amount, "combo points available")
    {
    }

    bool IsActive() override;
};

// [rot70] The seconds until `bot` could spend one more combo point: the builder once its energy is there (or the
// global cooldown passed), then the finisher one global cooldown later at the earliest and once the energy for both
// is there - the costs from spell data (CalcPowerCost: talents in), the regeneration as the core's Player::Regenerate
// (10 energy a second plus the unit's flat modifier, times the server's energy rate), the global cooldown the
// builder's StartRecoveryTime. -1 when the bot knows neither spell.
float ComboFinishHorizon(PlayerbotAI* botAI, Player* bot, std::string const& builder, std::string const& finisher);

// [rot70] A target with this bot's combo points on it dies before one more point could be spent
// (ExpectedLifetime < ComboFinishHorizon): the finisher now, with the points there are (1 or more), else they die with
// the target. b205 / b206 rfc-h: the Assassination rogue cast 163 Mutilates (~400 points) and 35 Envenoms, ~140 points
// lost on dying trash; the Combat rogue ~85. Was: 3 points and a 3.0 s lifetime by health / the group's dps.
class TargetWithComboPointsLowerHealTrigger : public ComboPointsAvailableTrigger
{
public:
    TargetWithComboPointsLowerHealTrigger(PlayerbotAI* botAI) : ComboPointsAvailableTrigger(botAI, 1) {}
    bool IsActive() override;
};

class ComboPointsNotFullTrigger : public StatAvailable
{
public:
    ComboPointsNotFullTrigger(PlayerbotAI* botAI, int32 amount = 5, std::string const name = "combo points not full")
        : StatAvailable(botAI, amount, name)
    {
    }

    bool IsActive() override;
};

class LoseAggroTrigger : public Trigger
{
public:
    LoseAggroTrigger(PlayerbotAI* botAI) : Trigger(botAI, "lose aggro") {}

    bool IsActive() override;
};

// Tank only: a mob is hitting a healer or damage dealer of the group ("group member attacker" value), or a
// friend within Righteous Defense range is hit by a mob out of taunt range ("attacked group member").
// [rot151] "mob in reach attacking group member" (friendTarget false): the mob alone, for a tank with no spell cast on
// the friend (Dark Command, Death Grip, Growl, Feral Charge all need the mob in reach and sight). b212 rfc-h 5C
// 364-385 s: a Ragefire Shaman 53 yd off and then 24 yd off out of sight hit the balance druid, the Blood DK's peel
// fired on the friend alone and its four actions found no mob every tick ("dark command on group member attacker
// (isUseful false: no target)"), eating the tick's action budget.
class MobAttackingGroupMemberTrigger : public Trigger
{
public:
    MobAttackingGroupMemberTrigger(PlayerbotAI* botAI, std::string const name = "mob attacking group member",
                                   bool friendTarget = true)
        : Trigger(botAI, name), friendTarget(friendTarget)
    {
    }

    bool IsActive() override;

private:
    bool friendTarget;  // the friend alone ("attacked group member") is enough: Righteous Defense, Intervene
};

// Tank only: `minAttackers` (PEEL_AOE_MIN_ATTACKERS) or more mobs on non-tank members within `range` yards of the
// tank. One mob is taunted back; from two on the tank takes them with its area threat (Righteous Defense takes up to
// three). With one mob in reach of the area threat ("mob attacking group member nearby") it is what holds the mob
// while the taunts cool down.
class MobsAttackingGroupMembersTrigger : public Trigger
{
public:
    static constexpr uint8 PEEL_AOE_MIN_ATTACKERS = 2;

    MobsAttackingGroupMembersTrigger(PlayerbotAI* botAI, std::string const name, std::string const range,
                                     uint8 minAttackers = PEEL_AOE_MIN_ATTACKERS)
        : Trigger(botAI, name), range(range), minAttackers(minAttackers)
    {
    }

    bool IsActive() override;

private:
    std::string range;
    uint8 minAttackers;
};

// Tank only: the mob the peel would take ("group member attacker") stands out of melee within
// TankPeelWalkRange yards: the tank walks to it, as a player does while its taunts cool down (wave 10: dps and
// healers died to one mob with the tank 3-12 yd away).
class GroupMemberAttackerNearTrigger : public Trigger
{
public:
    GroupMemberAttackerNearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "group member attacker near") {}

    bool IsActive() override;
};

// Tank only: two or more mobs on non-tank members within taunt range and some of them out of reach of the tank's
// area threat, or any mob on the healer out of that reach ("far healer attacker"): the tank steps towards them
// ("move to group member attackers") and takes them with an AoE.
class GroupMemberAttackersFarTrigger : public Trigger
{
public:
    GroupMemberAttackersFarTrigger(PlayerbotAI* botAI) : Trigger(botAI, "group member attackers far") {}

    bool IsActive() override;
};

// [taunt_reaction] Tank in combat: a mob hits a non-tank member in taunt range and sight ("group member attacker"),
// or a friend in Righteous Defense range ("attacked group member"), and the class's taunt, ranged threat spell or
// area taunt is off cooldown (TauntReactionAction).
class TauntReactionTrigger : public Trigger
{
public:
    TauntReactionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "taunt reaction") {}

    bool IsActive() override;
};

// [flee_stop] Any damage dealer or tank in combat: a mob of the fight runs at low health ("fleeing mob" value) and
// nobody of the group stunned or snared it within FledMobs::STOP_SPREAD_MS.
class FleeingMobTrigger : public Trigger
{
public:
    FleeingMobTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fleeing mob") {}

    bool IsActive() override;
};

// [fled_survivor] Tank out of combat: a mob that fled from an earlier fight stands idle and damaged within 40 yd.
class FledSurvivorNearTrigger : public Trigger
{
public:
    FledSurvivorNearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fled survivor near") {}

    bool IsActive() override;
};

// Tank only, in and out of combat: a mob hits the healer out of the tank's area threat ("far healer attacker").
class HealerAttackedFarTrigger : public Trigger
{
public:
    HealerAttackedFarTrigger(PlayerbotAI* botAI) : Trigger(botAI, "healer attacked far") {}

    bool IsActive() override;
};

// Tank out of combat: a mob hits a non-tank member ("defend member target") and the tank is not attacking it yet.
class GroupMemberAttackedTrigger : public Trigger
{
public:
    GroupMemberAttackedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "group member attacked") {}

    bool IsActive() override;
};

// Tank out of combat: its current target is a dungeon boss within IDLE_BOSS_RANGE that is not in combat, and has been
// for IDLE_BOSS_MS (a boss that does not aggro on proximity: the Violet Hold prisoners, Oculus Drakos / Varos /
// Urom). The target was set by someone else (the harness' body pull, a target pick) and nothing swung at him.
class IdleBossAtTankTrigger : public Trigger
{
public:
    IdleBossAtTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "idle boss at tank") {}

    bool IsActive() override;

private:
    ObjectGuid _boss;
    uint32 _sinceMs = 0;
};

class HasAggroTrigger : public Trigger
{
public:
    HasAggroTrigger(PlayerbotAI* botAI) : Trigger(botAI, "have aggro") {}

    bool IsActive() override;
};

class SpellTrigger : public Trigger
{
public:
    SpellTrigger(PlayerbotAI* botAI, std::string const spell, int32 checkInterval = 1)
        : Trigger(botAI, spell, checkInterval), spell(spell)
    {
    }

    std::string const GetTargetName() override { return "current target"; }
    std::string const getName() override { return spell; }
    bool IsActive() override;

protected:
    std::string spell;
};

class SpellCanBeCastTrigger : public SpellTrigger
{
public:
    SpellCanBeCastTrigger(PlayerbotAI* botAI, std::string const spell) : SpellTrigger(botAI, spell) {}

    bool IsActive() override;
};

class SpellNoCooldownTrigger : public SpellTrigger
{
public:
    SpellNoCooldownTrigger(PlayerbotAI* botAI, std::string const spell) : SpellTrigger(botAI, spell) {}

    bool IsActive() override;
};

class SpellCooldownTrigger : public SpellTrigger
{
public:
    SpellCooldownTrigger(PlayerbotAI* botAI, std::string const spell) : SpellTrigger(botAI, spell) {}

    std::string const GetTargetName() override { return "self target"; }
    bool IsActive() override;
};

class InterruptSpellTrigger : public SpellTrigger
{
public:
    InterruptSpellTrigger(PlayerbotAI* botAI, std::string const spell) : SpellTrigger(botAI, spell) {}

    bool IsActive() override;
};

class DeflectSpellTrigger : public SpellTrigger
{
public:
    DeflectSpellTrigger(PlayerbotAI* botAI, std::string const spell) : SpellTrigger(botAI, spell) {}

    bool IsActive() override;
};

class AttackerCountTrigger : public Trigger
{
public:
    AttackerCountTrigger(PlayerbotAI* botAI, int32 amount, float distance = sPlayerbotAIConfig.SightDistance)
        : Trigger(botAI), amount(amount), distance(distance) {}

    bool IsActive() override;
    std::string const getName() override { return "attacker count"; }

protected:
    int32 amount;
    float distance;
};

class HasAttackersTrigger : public AttackerCountTrigger
{
public:
    HasAttackersTrigger(PlayerbotAI* botAI) : AttackerCountTrigger(botAI, 1) {}
};

class MyAttackerCountTrigger : public AttackerCountTrigger
{
public:
    MyAttackerCountTrigger(PlayerbotAI* botAI, int32 amount) : AttackerCountTrigger(botAI, amount) {}

    bool IsActive() override;
    std::string const getName() override { return "my attacker count"; }
};

class BeingAttackedTrigger : public MyAttackerCountTrigger
{
public:
    BeingAttackedTrigger(PlayerbotAI* botAI) : MyAttackerCountTrigger(botAI, 1) {}
    std::string const getName() override { return "being attacked"; }
};

class MediumThreatTrigger : public MyAttackerCountTrigger
{
public:
    MediumThreatTrigger(PlayerbotAI* botAI) : MyAttackerCountTrigger(botAI, 2) {}
    bool IsActive() override;
};

class LowTankThreatTrigger : public Trigger
{
public:
    LowTankThreatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "low tank threat") {}
    bool IsActive() override;
};

// A non-tank close to pulling aggro: its threat on its target is near the tank's. Fires before the mob turns,
// unlike "medium threat" (mobs already on the bot); class strategies answer with their threat drop. With
// `raidOnly` ("high threat in raid") only inside a raid, for drops that cost too much damage in a 5-man.
class HighThreatTrigger : public Trigger
{
public:
    HighThreatTrigger(PlayerbotAI* botAI, bool raidOnly = false)
        : Trigger(botAI, raidOnly ? "high threat in raid" : "high threat"), _raidOnly(raidOnly)
    {
    }

    bool IsActive() override;

private:
    bool _raidOnly;
};

class AoeTrigger : public AttackerCountTrigger
{
public:
    AoeTrigger(PlayerbotAI* botAI, int32 amount = 3, float range = 15.0f)
        : AttackerCountTrigger(botAI, amount), range(range) {}

    bool IsActive() override;
    std::string const getName() override { return "aoe"; }

private:
    float range;
};

class NoFoodTrigger : public Trigger
{
public:
    NoFoodTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no food trigger") {}

    bool IsActive() override;
};

class NoDrinkTrigger : public Trigger
{
public:
    NoDrinkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no drink trigger") {}

    bool IsActive() override;
};

// Low on real food/drink out of combat; checked every 5 s (bounded bag walk), the action looks for a vendor.
class NeedFoodRestockTrigger : public Trigger
{
public:
    NeedFoodRestockTrigger(PlayerbotAI* botAI) : Trigger(botAI, "need food restock", 5) {}

    bool IsActive() override;
};

class LightAoeTrigger : public AoeTrigger
{
public:
    LightAoeTrigger(PlayerbotAI* botAI) : AoeTrigger(botAI, 2, 8.0f) {}
};

class MediumAoeTrigger : public AoeTrigger
{
public:
    MediumAoeTrigger(PlayerbotAI* botAI) : AoeTrigger(botAI, 3, 8.0f) {}
};

class HighAoeTrigger : public AoeTrigger
{
public:
    HighAoeTrigger(PlayerbotAI* botAI) : AoeTrigger(botAI, 4, 8.0f) {}
};

// [rot401] An area spell where it pays: at least SpellPayoff::AoeBreakEvenTargets mobs (the fewest on which one cast
// deals the spec's filler damage in the same time, from the spells; never fewer than 2, an area on one mob is the
// single-target rotation's call) within the spell's own radius (its damage effects) of the current target, or of the
// bot for an area around the caster (Arcane Explosion, Blast Wave, Dragon's Breath). `excludesTarget`: the area
// never hits its target (Mind Sear), which is not counted. Replaces the fixed 3 mobs in 8 yd ("medium aoe") for the
// casters' area spells; the leader's AoE rule and the threat gate still decide the cast (multipliers).
class AoePaysTrigger : public Trigger
{
public:
    AoePaysTrigger(PlayerbotAI* botAI, std::string const aoeSpell, std::string const filler, bool selfCentred = false,
                   bool excludesTarget = false)
        : Trigger(botAI, aoeSpell + " pays"), aoeSpell(aoeSpell), filler(filler), selfCentred(selfCentred),
          excludesTarget(excludesTarget) {}

    bool IsActive() override;

protected:
    // the filler the area is weighed against; a class with a spec-dependent filler overrides it
    virtual std::string const Filler() { return filler; }

    std::string aoeSpell;
    std::string filler;
    bool selfCentred;
    bool excludesTarget;

private:
    static constexpr uint32 AOE_MIN_TARGETS = 2;
};

class BuffTrigger : public SpellTrigger
{
public:
    BuffTrigger(PlayerbotAI* botAI, std::string const spell, int32 checkInterval = 1,
        bool checkIsOwner = false, bool checkDuration = false, uint32 beforeDuration = 0)
        : SpellTrigger(botAI, spell, checkInterval)
    {
        this->checkIsOwner = checkIsOwner;
        this->checkDuration = checkDuration;
        this->beforeDuration = beforeDuration;
    }

public:
    std::string const GetTargetName() override { return "self target"; }
    bool IsBuffTrigger() override { return true; }
    bool IsActive() override;

protected:
    bool checkIsOwner;
    bool checkDuration;
    uint32 beforeDuration;
};

class BuffOnPartyTrigger : public BuffTrigger
{
public:
    BuffOnPartyTrigger(PlayerbotAI* botAI, std::string const spell, int32 checkInterval = 1)
        : BuffTrigger(botAI, spell, checkInterval) {}

    Value<Unit*>* GetTargetValue() override;
    bool IsActive() override;
    std::string const getName() override { return spell + " on party"; }
};

class ProtectPartyMemberTrigger : public Trigger
{
public:
    ProtectPartyMemberTrigger(PlayerbotAI* botAI) : Trigger(botAI, "protect party member") {}

    std::string const GetTargetName() override { return "party member to protect"; }
    bool IsActive() override;
};

class NoAttackersTrigger : public Trigger
{
public:
    NoAttackersTrigger(PlayerbotAI* botAI, std::string const name = "no attackers") : Trigger(botAI, name) {}

    bool IsActive() override;
};

// [lead95] b136 rfc-h: 94 s between the leader's issued pull order and the tank taking it (the tank's "aggressive
// target" ran at 4.0, after its blessings, buffs and the ready reply) - a led tank with an order issued and not taken
// yet takes it first (the order is the leader's decision; the readiness was checked before it was issued)
// [lead96] a mob comes at the group that the tank has no threat on yet, in an opener's range (TankIncomingMob)
class TankIncomingMobTrigger : public Trigger
{
public:
    TankIncomingMobTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tank incoming mob") {}

    bool IsActive() override;
};

class LeaderPullIssuedTrigger : public Trigger
{
public:
    LeaderPullIssuedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader pull issued") {}

    bool IsActive() override;
};

class NoTargetTrigger : public Trigger
{
public:
    NoTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no target") {}

    bool IsActive() override;
};

class InvalidTargetTrigger : public Trigger
{
public:
    InvalidTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "invalid target") {}

    bool IsActive() override;
};

class TargetInSightTrigger : public Trigger
{
public:
    TargetInSightTrigger(PlayerbotAI* botAI) : Trigger(botAI, "target in sight") {}

    bool IsActive() override;
};

class DebuffTrigger : public BuffTrigger
{
public:
    DebuffTrigger(PlayerbotAI* botAI, std::string const spell, int32 checkInterval = 1, bool checkIsOwner = false,
                  float needLifeTime = 8.0f, uint32 beforeDuration = 0)
        : BuffTrigger(botAI, spell, checkInterval, checkIsOwner, false, beforeDuration), needLifeTime(needLifeTime) {}

    std::string const GetTargetName() override { return "current target"; }
    bool IsDebuffTrigger() override { return true; }
    bool IsActive() override;

protected:
    // [rot400] With a filler set (the spec's single-target filler): the DoT only when its ticks that land before the
    // target dies (ExpectedFightLifetime) beat that filler cast in the DoT's own time (SpellPayoff::DotPays). b228
    // rfc-h: Shadow 476k / Affliction 246k DoT damage per run left on mobs that died within 6-8 s.
    bool DotPaysOff(Unit* target);
    // [rot400] the share of the DoT's full periodic damage that lands at once on top (Improved Devouring Plague)
    virtual float DotUpfrontShare() { return 0.0f; }
    // [rot400] the filler's spell name; empty: no payoff check (a class with a spec-dependent filler overrides it)
    virtual std::string const DotFiller() { return dotFiller; }

    float needLifeTime;
    std::string dotFiller;

private:
    static constexpr uint32 DOT_PAYOFF_LOG_MS = 5000;
    uint32 _payoffLoggedMs = 0;
};

class DebuffOnBossTrigger : public DebuffTrigger
{
public:
    DebuffOnBossTrigger(PlayerbotAI* botAI, std::string const spell, int32 checkInterval = 1, bool checkIsOwner = false)
        : DebuffTrigger(botAI, spell, checkInterval, checkIsOwner) {}
    bool IsActive() override;
};

class DebuffOnAttackerTrigger : public DebuffTrigger
{
public:
    DebuffOnAttackerTrigger(PlayerbotAI* botAI, std::string const spell, bool checkIsOwner = true,
                            float needLifeTime = 8.0f)
        : DebuffTrigger(botAI, spell, 1, checkIsOwner, needLifeTime) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return spell + " on attacker"; }
};

class DebuffOnMeleeAttackerTrigger : public DebuffTrigger
{
public:
    DebuffOnMeleeAttackerTrigger(PlayerbotAI* botAI, std::string const spell, bool checkIsOwner = true,
                                 float needLifeTime = 8.0f)
        : DebuffTrigger(botAI, spell, 1, checkIsOwner, needLifeTime) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return spell + " on attacker"; }
};

class BoostTrigger : public BuffTrigger
{
public:
    BoostTrigger(PlayerbotAI* botAI, std::string const spell, float balance = 50.f)
        : BuffTrigger(botAI, spell, 1), balance(balance) {}

    bool IsActive() override;

protected:
    float balance;
};

class GenericBoostTrigger : public Trigger
{
public:
    GenericBoostTrigger(PlayerbotAI* botAI, float balance = 50.f)
        : Trigger(botAI, "generic boost", 1), balance(balance) {}

    bool IsActive() override;

protected:
    float balance;
};

class HealerShouldAttackTrigger : public Trigger
{
public:
    HealerShouldAttackTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "healer should attack", 1) {}

    bool IsActive() override;
};

class RandomTrigger : public Trigger
{
public:
    RandomTrigger(PlayerbotAI* botAI, std::string const name, int32 probability = 7);

    bool IsActive() override;

protected:
    int32 probability;
    uint32 lastCheck;
};

class AndTrigger : public Trigger
{
public:
    AndTrigger(PlayerbotAI* botAI, Trigger* ls, Trigger* rs) : Trigger(botAI), ls(ls), rs(rs) {}
    virtual ~AndTrigger()
    {
        delete ls;
        delete rs;
    }

    bool IsActive() override;
    std::string const getName() override;

protected:
    Trigger* ls;
    Trigger* rs;
};

class TwoTriggers : public Trigger
{
public:
    explicit TwoTriggers(PlayerbotAI* botAI, std::string name1 = "", std::string name2 = "") : Trigger(botAI)
    {
        this->name1 = std::move(name1);
        this->name2 = std::move(name2);
    }
    bool IsActive() override;
    std::string const getName() override;

protected:
    std::string name1;
    std::string name2;
};

class SnareTargetTrigger : public DebuffTrigger
{
public:
    SnareTargetTrigger(PlayerbotAI* botAI, std::string const spell) : DebuffTrigger(botAI, spell) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return spell + " on snare target"; }
};

class LowManaTrigger : public Trigger
{
public:
    LowManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "low mana") {}

    bool IsActive() override;
};

class MediumManaTrigger : public Trigger
{
public:
    MediumManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "medium mana") {}

    bool IsActive() override;
};

class LowEnergyTrigger : public Trigger
{
public:
    LowEnergyTrigger(PlayerbotAI* botAI, uint8 threshold = 30) : Trigger(botAI, "low energy"), threshold(threshold) {}

    bool IsActive() override;

private:
    uint8 threshold;
};

class NoPetTrigger : public Trigger
{
public:
    NoPetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no pet", 5 * IN_MILLISECONDS) {}

    virtual bool IsActive() override;
};

class HasPetTrigger : public Trigger
{
public:
    HasPetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "has pet", 5 * IN_MILLISECONDS) {}

    virtual bool IsActive() override;
};

class PetAttackTrigger : public Trigger
{
public:
    PetAttackTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pet attack") {}

    virtual bool IsActive() override;
};

class ItemCountTrigger : public Trigger
{
public:
    ItemCountTrigger(PlayerbotAI* botAI, std::string const item, int32 count, int32 interval = 30 * IN_MILLISECONDS)
        : Trigger(botAI, item, interval), item(item), count(count) {}

    bool IsActive() override;
    std::string const getName() override { return "item count"; }

protected:
    std::string const item;
    int32 count;
};

class AmmoCountTrigger : public ItemCountTrigger
{
public:
    AmmoCountTrigger(PlayerbotAI* botAI, std::string const item, uint32 count = 1,
                     int32 interval = 30 * IN_MILLISECONDS)
        : ItemCountTrigger(botAI, item, count, interval) {}
    bool IsActive() override;
};

class HasAuraTrigger : public Trigger
{
public:
    HasAuraTrigger(PlayerbotAI* botAI, std::string const spell, int32 checkInterval = 1)
        : Trigger(botAI, spell, checkInterval) {}

    std::string const GetTargetName() override { return "self target"; }
    bool IsActive() override;
};

class HasAuraStackTrigger : public Trigger
{
public:
    HasAuraStackTrigger(PlayerbotAI* botAI, std::string spell, int stack, int checkInterval = 1)
        : Trigger(botAI, spell, checkInterval), stack(stack) {}

    std::string const GetTargetName() override { return "self target"; }
    bool IsActive() override;

private:
    int stack;
};

class HasNoAuraTrigger : public Trigger
{
public:
    HasNoAuraTrigger(PlayerbotAI* botAI, std::string const spell) : Trigger(botAI, spell) {}

    std::string const GetTargetName() override { return "self target"; }
    bool IsActive() override;
};

class TimerTrigger : public Trigger
{
public:
    TimerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "timer"), lastCheck(0) {}

    bool IsActive() override;

private:
    time_t lastCheck;
};

class TimerBGTrigger : public Trigger
{
public:
    TimerBGTrigger(PlayerbotAI* botAI) : Trigger(botAI, "timer bg"), lastCheck(0) {}

    bool IsActive() override;

private:
    time_t lastCheck;
};

class TankAssistTrigger : public NoAttackersTrigger
{
public:
    // its own name: as "no attackers" it was logged under the other trigger's name and looked inactive
    TankAssistTrigger(PlayerbotAI* botAI) : NoAttackersTrigger(botAI, "tank assist") {}

    bool IsActive() override;
};

class IsBehindTargetTrigger : public Trigger
{
public:
    IsBehindTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "behind target") {}

    bool IsActive() override;
};

class IsNotBehindTargetTrigger : public Trigger
{
public:
    IsNotBehindTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "is not behind target") {}

    bool IsActive() override;
};

class IsNotFacingTargetTrigger : public Trigger
{
public:
    IsNotFacingTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "not facing target") {}

    bool IsActive() override;
};

class HasCcTargetTrigger : public Trigger
{
public:
    HasCcTargetTrigger(PlayerbotAI* botAI, std::string const name) : Trigger(botAI, name) {}

    bool IsActive() override;

protected:
    bool IsCcTargetFree(Unit* ccTarget, Unit* rtiCcTarget);
};

class NoMovementTrigger : public Trigger
{
public:
    NoMovementTrigger(PlayerbotAI* botAI, std::string const name) : Trigger(botAI, name) {}

    bool IsActive() override;
};

class NoPossibleTargetsTrigger : public Trigger
{
public:
    NoPossibleTargetsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no possible targets") {}

    bool IsActive() override;
};

class NotDpsTargetActiveTrigger : public Trigger
{
public:
    NotDpsTargetActiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "not dps target active") {}

    bool IsActive() override;
};

class NotDpsAoeTargetActiveTrigger : public Trigger
{
public:
    NotDpsAoeTargetActiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "not dps aoe target active") {}

    bool IsActive() override;
};

class PossibleAddsTrigger : public Trigger
{
public:
    PossibleAddsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "possible adds") {}

    bool IsActive() override;
};

class LossOfControlTrigger : public Trigger
{
public:
    LossOfControlTrigger(PlayerbotAI* botAI) : Trigger(botAI, "loss of control", 1) {}

    bool IsActive() override;
};

class FearCharmSleepTrigger : public Trigger
{
public:
    FearCharmSleepTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fear charm sleep", 1) {}

    bool IsActive() override;
};

class FearSleepSapTrigger : public Trigger
{
public:
    FearSleepSapTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fear sleep sap", 1) {}

    bool IsActive() override;
};

class PoisonDiseaseBleedTrigger : public Trigger
{
public:
    PoisonDiseaseBleedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "poison disease bleed", 1) {}

    bool IsActive() override;
};

class MovementImpairedTrigger : public Trigger
{
public:
    MovementImpairedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "movement impaired", 1) {}

    bool IsActive() override;
};

class IsSwimmingTrigger : public Trigger
{
public:
    IsSwimmingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "swimming") {}

    bool IsActive() override;
};

class HasItemForSpellTrigger : public Trigger
{
public:
    HasItemForSpellTrigger(PlayerbotAI* botAI, std::string const spell) : Trigger(botAI, spell) {}

    bool IsActive() override;
};

class TargetChangedTrigger : public Trigger
{
public:
    TargetChangedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "target changed") {}

    bool IsActive() override;
};

class InterruptEnemyHealerTrigger : public SpellTrigger
{
public:
    InterruptEnemyHealerTrigger(PlayerbotAI* botAI, std::string const spell) : SpellTrigger(botAI, spell) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return spell + " on enemy healer"; }
};

class RandomBotUpdateTrigger : public RandomTrigger
{
public:
    RandomBotUpdateTrigger(PlayerbotAI* botAI) : RandomTrigger(botAI, "random bot update", 30 * 1000) {}

    bool IsActive() override;
};

class NoNonBotPlayersAroundTrigger : public Trigger
{
public:
    NoNonBotPlayersAroundTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "no non bot players around", 10 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class NewPlayerNearbyTrigger : public Trigger
{
public:
    NewPlayerNearbyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new player nearby", 10 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class CollisionTrigger : public Trigger
{
public:
    CollisionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "collision", 5 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class StayTimeTrigger : public Trigger
{
public:
    StayTimeTrigger(PlayerbotAI* botAI, uint32 delay, std::string const name)
        : Trigger(botAI, name, 5 * IN_MILLISECONDS), delay(delay) {}

    bool IsActive() override;

private:
    uint32 delay;
};

class SitTrigger : public StayTimeTrigger
{
public:
    SitTrigger(PlayerbotAI* botAI) : StayTimeTrigger(botAI, sPlayerbotAIConfig.SitDelay, "sit") {}
};

class ReturnToStayPositionTrigger : public Trigger
{
public:
    ReturnToStayPositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "return to stay position", 2) {}

    virtual bool IsActive() override;
};

class ReturnTrigger : public StayTimeTrigger
{
public:
    ReturnTrigger(PlayerbotAI* botAI) : StayTimeTrigger(botAI, sPlayerbotAIConfig.ReturnDelay, "return") {}
};

class GiveItemTrigger : public Trigger
{
public:
    GiveItemTrigger(PlayerbotAI* botAI, std::string const name, std::string const item)
        : Trigger(botAI, name, 2 * IN_MILLISECONDS), item(item) {}

    bool IsActive() override;

protected:
    std::string const item;
};

class GiveFoodTrigger : public GiveItemTrigger
{
public:
    GiveFoodTrigger(PlayerbotAI* botAI) : GiveItemTrigger(botAI, "give food", "conjured food") {}

    bool IsActive() override;
};

class GiveWaterTrigger : public GiveItemTrigger
{
public:
    GiveWaterTrigger(PlayerbotAI* botAI) : GiveItemTrigger(botAI, "give water", "conjured water") {}

    bool IsActive() override;
};

class IsMountedTrigger : public Trigger
{
public:
    IsMountedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mounted", 1) {}

    bool IsActive() override;
};

class CorpseNearTrigger : public Trigger
{
public:
    CorpseNearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "corpse near", 1 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class IsFallingTrigger : public Trigger
{
public:
    IsFallingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "falling", 10 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class IsFallingFarTrigger : public Trigger
{
public:
    IsFallingFarTrigger(PlayerbotAI* botAI) : Trigger(botAI, "falling far", 10 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class HasAreaDebuffTrigger : public Trigger
{
public:
    HasAreaDebuffTrigger(PlayerbotAI* botAI) : Trigger(botAI, "have area debuff") {}

    bool IsActive() override;
};

class BuffOnMainTankTrigger : public BuffTrigger
{
public:
    BuffOnMainTankTrigger(PlayerbotAI* botAI, std::string spell, bool checkIsOwner = false, int checkInterval = 1)
        : BuffTrigger(botAI, spell, checkInterval, checkIsOwner) {}

public:
    virtual Value<Unit*>* GetTargetValue();
};

class SelfResurrectTrigger : public Trigger
{
public:
    SelfResurrectTrigger(PlayerbotAI* botAI) : Trigger(botAI, "can self resurrect") {}

    bool IsActive() override { return !bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL); }
};

class NewPetTrigger : public Trigger
{
public:
    NewPetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "new pet"), lastPetGuid(ObjectGuid::Empty), triggered(false) {}

    bool IsActive() override;

private:
    ObjectGuid lastPetGuid;
    bool triggered;
};

class ForceRebuffPendingTrigger : public Trigger
{
public:
    ForceRebuffPendingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "force rebuff pending") {}

    bool IsActive() override;
};

#endif
