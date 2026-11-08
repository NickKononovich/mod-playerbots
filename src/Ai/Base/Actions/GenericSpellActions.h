/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GENERICSPELLACTIONS_H
#define PLAYERBOTS_GENERICSPELLACTIONS_H

#include "Action.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "UseItemAction.h"
#include "Value.h"

class PlayerbotAI;
class Unit;
class WorldObject;

class CastSpellAction : public Action
{
public:
    CastSpellAction(PlayerbotAI* botAI, std::string const spell);

    std::string const GetTargetName() override { return "current target"; };
    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
    ActionThreatType getThreatType() override { return ActionThreatType::Single; }

    std::vector<NextAction> getPrerequisites() override
    {
        return {};
    }

    std::string const getSpell() { return spell; }

protected:
    std::string spell;
    float range;
};

class CastAuraSpellAction : public CastSpellAction
{
public:
    CastAuraSpellAction(PlayerbotAI* botAI, std::string const spell, bool isOwner = false, bool checkDuration = false, uint32 beforeDuration = 0)
        : CastSpellAction(botAI, spell)
    {
        this->isOwner = isOwner;
        this->beforeDuration = beforeDuration;
        this->checkDuration = checkDuration;
    }

    bool isUseful() override;

protected:
    bool isOwner;
    bool checkDuration;
    uint32 beforeDuration;
};

class CastMeleeSpellAction : public CastSpellAction
{
public:
    CastMeleeSpellAction(PlayerbotAI* botAI, std::string const spell);
    bool isUseful() override;
};

class CastDebuffSpellAction : public CastAuraSpellAction
{
public:
    // [rot610] the lifetime argument is no longer read: DpsEstimate::DebuffPays weighs the debuff by the formula
    CastDebuffSpellAction(PlayerbotAI* botAI, std::string const spell, bool isOwner = false,
                          float /*needLifeTime*/ = 8.0f)
        : CastAuraSpellAction(botAI, spell, isOwner) {}
    bool isUseful() override;
};

class CastMeleeDebuffSpellAction : public CastDebuffSpellAction
{
public:
    CastMeleeDebuffSpellAction(PlayerbotAI* botAI, std::string const spell, bool isOwner = false, float needLifeTime = 8.0f);
    bool isUseful() override;
};

class CastDebuffSpellOnAttackerAction : public CastDebuffSpellAction
{
public:
    CastDebuffSpellOnAttackerAction(PlayerbotAI* botAI, std::string const spell, bool isOwner = true,
                                    float needLifeTime = 8.0f)
        : CastDebuffSpellAction(botAI, spell, isOwner, needLifeTime) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return spell + " on attacker"; }
    // ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
};

class CastDebuffSpellOnMeleeAttackerAction : public CastDebuffSpellAction
{
public:
    CastDebuffSpellOnMeleeAttackerAction(PlayerbotAI* botAI, std::string const spell, bool isOwner = true,
                                         float needLifeTime = 8.0f)
        : CastDebuffSpellAction(botAI, spell, isOwner, needLifeTime) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return spell + " on attacker"; }
    // ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
};

class CastBuffSpellAction : public CastAuraSpellAction
{
public:
    CastBuffSpellAction(PlayerbotAI* botAI, std::string const spell, bool checkIsOwner = false, uint32 beforeDuration = 0);

    std::string const GetTargetName() override { return "self target"; }
    bool isUseful() override;
    bool Execute(Event event) override;
    // [b31_shared] A buff on the bot or a friend makes no threat: the multiplier must not veto it. A buff action
    // aimed at an enemy (Turn Undead) keeps the Single rule.
    ActionThreatType getThreatType() override;
};

class GroupBuffSpellAction : public CastBuffSpellAction
{
public:
    GroupBuffSpellAction(PlayerbotAI* botAI, std::string const spell, bool checkIsOwner = false,
                         uint32 beforeDuration = 0)
        : CastBuffSpellAction(botAI, spell, checkIsOwner, beforeDuration) {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

class CastEnchantItemMainHandAction : public CastSpellAction
{
public:
    CastEnchantItemMainHandAction(PlayerbotAI* botAI, std::string const spell);
    std::string const GetTargetName() override { return "self target"; }
    bool Execute(Event event) override;
    bool isPossible() override;
};

class CastEnchantItemOffHandAction : public CastSpellAction
{
public:
    CastEnchantItemOffHandAction(PlayerbotAI* botAI, std::string const spell);
    std::string const GetTargetName() override { return "self target"; }
    bool Execute(Event event) override;
    bool isPossible() override;
};

class CastHealingSpellAction : public CastAuraSpellAction
{
public:
    CastHealingSpellAction(PlayerbotAI* botAI, std::string const spell, uint8 estAmount = 15.0f,
                           HealingManaEfficiency manaEfficiency = HealingManaEfficiency::MEDIUM, bool isOwner = true);

    std::string const GetTargetName() override { return "self target"; }
    bool isUseful() override;
    // Healing threat spreads over every mob in combat (Aoe), but only a top-off waits for the tank's threat.
    ActionThreatType getThreatType() override;
    // Yunfan: Mana efficiency tell the bot how to save mana. The higher the better.
    HealingManaEfficiency manaEfficiency;
    uint8 estAmount;
};

class CastAoeHealSpellAction : public CastHealingSpellAction
{
public:
    CastAoeHealSpellAction(PlayerbotAI* botAI, std::string const spell, uint8 estAmount = 15.0f,
                           HealingManaEfficiency manaEfficiency = HealingManaEfficiency::MEDIUM)
        : CastHealingSpellAction(botAI, spell, estAmount, manaEfficiency)
    {
    }

    std::string const GetTargetName() override { return "party member to heal"; }
    bool isUseful() override;
};

class CastCureSpellAction : public CastSpellAction
{
public:
    CastCureSpellAction(PlayerbotAI* botAI, std::string const spell);

    std::string const GetTargetName() override { return "self target"; }
};

class PartyMemberActionNameSupport
{
public:
    PartyMemberActionNameSupport(std::string const spell) { name = std::string(spell + " on party"); }

    std::string const getName() { return name; }

private:
    std::string name;
};

class HealPartyMemberAction : public CastHealingSpellAction, public PartyMemberActionNameSupport
{
public:
    HealPartyMemberAction(PlayerbotAI* botAI, std::string const spell, uint8 estAmount = 15.0f,
                          HealingManaEfficiency manaEfficiency = HealingManaEfficiency::MEDIUM, bool isOwner = true)
        : CastHealingSpellAction(botAI, spell, estAmount, manaEfficiency, isOwner), PartyMemberActionNameSupport(spell) {}

    std::string const GetTargetName() override { return "party member to heal"; }
    std::string const getName() override { return PartyMemberActionNameSupport::getName(); }
};

class ResurrectPartyMemberAction : public CastSpellAction
{
public:
    ResurrectPartyMemberAction(PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

    std::string const GetTargetName() override { return "party member to resurrect"; }
    bool isPossible() override;
    std::vector<NextAction> getPrerequisites() override
    {
        return NextAction::merge(
            { NextAction("reach party member to resurrect") },
            Action::getPrerequisites()
        );
    }
};

class CurePartyMemberAction : public CastSpellAction, public PartyMemberActionNameSupport
{
public:
    CurePartyMemberAction(PlayerbotAI* botAI, std::string const spell, uint32 dispelType)
        : CastSpellAction(botAI, spell), PartyMemberActionNameSupport(spell), dispelType(dispelType) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return PartyMemberActionNameSupport::getName(); }

protected:
    uint32 dispelType;
};

// A taunt or threat spell on the mob hitting a non-tank group member ("group member attacker" value). A spell of
// SHORT_REACH yards or less goes to the best such mob within its reach when the pick stands further away.
class CastSpellOnGroupMemberAttackerAction : public CastSpellAction
{
public:
    static constexpr float SHORT_REACH = 15.0f;  // melee spells, Judgement, Hammer of Justice; not the 30 yd taunts

    // onlyForHealer: kept for a mob on the healer (a stun with a long cooldown, not a taunt)
    CastSpellOnGroupMemberAttackerAction(PlayerbotAI* botAI, std::string const spell, bool onlyForHealer = false)
        : CastSpellAction(botAI, spell), name(spell + " on group member attacker"), healerOnly(onlyForHealer)
    {
    }

    Value<Unit*>* GetTargetValue() override;
    Unit* GetTarget() override;
    std::string const getName() override { return name; }
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    std::string name;
    bool healerOnly;
    ObjectGuid _reachTarget;  // the short-reach target, looked up once per GroupMemberAttackerValue::CHECK_INTERVAL_MS
    uint32 _reachCheckedMs = 0;
};

// A spell cast on a group member a mob is hitting ("attacked group member" value: Righteous Defense).
class CastSpellOnAttackedGroupMemberAction : public CastSpellAction
{
public:
    // minMobs: useful only while the friend has that many attackers the peel has not just taken (the "swarmed" variant
    // goes before the single-target taunt: Righteous Defense takes up to three at once)
    CastSpellOnAttackedGroupMemberAction(PlayerbotAI* botAI, std::string const spell, uint32 minMobs = 0)
        : CastSpellAction(botAI, spell),
          name(spell + (minMobs > 1 ? " on swarmed group member" : " on attacked group member")),
          minFreshMobs(minMobs)
    {
    }

    Unit* GetTarget() override;
    std::string const getName() override { return name; }
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    std::string name;
    uint32 minFreshMobs;
};

// [taunt_reaction] Tank in combat: a mob that turns to a non-tank member is taken back from where the tank stands,
// within about one GCD, before any rotation filler, a fight-spot hold or an encounter walk (ACTION_RAID + 6 in
// TankAssistStrategy; a taunt needs no walking). hor-h build 29: the Shadowy Mercenary on the mage 5 s and the
// Ghostly Priest on the shaman 6 s while the tank walked to its wave spot (ACTION_RAID + 5) and its taunt went to the
// mob beside it. In order:
//   1. two or more fresh mobs on one member: Righteous Defense on the member; Challenging Shout / Challenging Roar
//      with two of them within 10 yd of the tank, for the healer's mobs or with the single taunt cooling;
//   2. the class's single taunt (Hand of Reckoning, Taunt, Dark Command, Growl) on the mob out of the tank's reach
//      first (GroupMemberAttackerValue::FindForTaunt);
//   3. with the taunt cooling, a ranged threat spell on a mob out of melee: Avenger's Shield, Heroic Throw, Faerie
//      Fire (Feral), Death Grip (on a caster: a mob casting or not in melee with its victim).
class TauntReactionAction : public Action
{
public:
    struct Spells
    {
        char const* taunt;
        char const* ranged;
        char const* swarm;   // the area taunt, nullptr when the class has none
        bool swarmOnFriend;  // Righteous Defense is cast on the friend, the shouts on the tank
    };

    static constexpr uint32 MIN_SWARM_MOBS = 2;
    static constexpr uint32 SWARM_SPREAD_MOBS = 3;  // [bear_aoe_threat] mobs on several members, for the shouts
    static constexpr float AOE_TAUNT_RADIUS = 10.0f;  // Challenging Shout, Challenging Roar
    static constexpr float DEFAULT_TAUNT_RANGE = 30.0f;
    static constexpr uint32 PLAN_MS = 300;  // the choice is made at most this often (line of sight per mob)

    TauntReactionAction(PlayerbotAI* botAI) : Action(botAI, "taunt reaction") {}

    bool isUseful() override;
    bool Execute(Event event) override;

    // The reaction spells of the bot's class, nullptr for a class without a tank taunt.
    static Spells const* ClassSpells(Player* bot);
    // Known and off cooldown (the global cooldown is the cast's own check).
    static bool SpellReady(PlayerbotAI* botAI, char const* spell);
    // The spell that takes `mob` from where the tank stands now: the taunt, else the ranged threat spell (out of
    // melee only); ready, in range, in sight. Empty when none: then a walk is the only way.
    static std::string ReadyFor(PlayerbotAI* botAI, Unit* mob);
    // Ready, the target in range (a friend by distance, a mob by combat reach) and in sight, and the cast's own
    // checks (form, stance, global cooldown) pass. The bot itself as target: no range check.
    static bool Usable(PlayerbotAI* botAI, char const* spell, Unit* target);
    // [lead526] a backup taunt / gap closer of the class is off cooldown (the trigger's test with the main spells)
    static bool AnyBackupReady(PlayerbotAI* botAI);

private:
    struct Plan
    {
        std::string spell;
        ObjectGuid target;   // the mob, the friend (Righteous Defense) or the tank itself (the shouts)
        ObjectGuid victim;   // the member the mobs hit
        GuidVector mobs;     // the mobs it takes, for the peel record and the reaction log
        bool holds = false;  // a taunt: the mob is marked peeled (not after a ranged threat spell)
        uint32 madeMs = 0;
    };

    bool MakePlan(Plan& plan);
    // [lead520] the class's backup taunts / gap closers while its taunt is on cooldown; true: `plan` holds one
    static bool BackupTaunt(PlayerbotAI* botAI, Unit* mob, Unit* victim, Plan& plan);
    static float SpellRange(PlayerbotAI* botAI, char const* spell, float fallback);

    Plan _plan;
};

// [flee_stop] Any damage dealer or tank: a mob of the fight running at low health ("fleeing mob") is stopped with
// the class's stun, root or snare before it reaches the next pack; the ranged then burst it (DpsTargetValue picks the
// runner, ThreatMultiplier does not hold on it). One stop per runner per FledMobs::STOP_SPREAD_MS for the group.
// Spells round the caster (Frost Nova, Piercing Howl, Earthbind Totem) only with the runner within SELF_REACH.
class FleeStopAction : public Action
{
public:
    static constexpr float SELF_REACH = 10.0f;
    // [lead1210] what a stop does to the runner, by its spell's auras (HoldOf): the higher holds more
    static constexpr uint8 HOLD_NONE = 0;  // a pull (Death Grip) or no aura of its own
    static constexpr uint8 HOLD_SNARE = 1;
    static constexpr uint8 HOLD_ROOT = 2;
    static constexpr uint8 HOLD_STUN = 3;

    FleeStopAction(PlayerbotAI* botAI) : Action(botAI, "flee stop") {}

    bool isUseful() override;
    bool Execute(Event event) override;
    // [lead104] the leader's question: this member has a stop ready that takes `mob` now (in range and sight)
    bool CanStop(Unit* mob) { return mob && Pick(mob) != nullptr; }
    // [lead1210] the hold of the stop this member would cast on `mob` now (HOLD_NONE also when it has none)
    uint8 BestHold(Unit* mob);
    static uint8 HoldOf(uint32 spellId);
    // the hold already on `mob` (a stun, a root or a snare aura)
    static uint8 HeldBy(Unit* mob);
    // [lead1345] the leader's question before a flee: the stop at range this member has ready (known, off cooldown)
    // for `mob` wherever it stands, the one that holds most (`hold`), its reach by spell data (`range`); spells round
    // the caster and pulls without a hold left out. nullptr: none
    char const* ReadyRangedStop(Unit* mob, uint8& hold, float& range);
    // [lead1345] the stop this member would cast on `mob` now (CanStop's pick) and its reach by spell data; nullptr:
    // none
    char const* StopNow(Unit* mob, float& range);

private:
    struct Stop
    {
        char const* spell;
        bool aroundSelf;
    };

    // the stops of the member's class (its list's order breaks a hold tie); false for a class without any
    static bool ClassStops(Player* bot, Stop const*& begin, Stop const*& end);

    // The class's stop that takes `mob` now and holds it most, nullptr when none; `holdOut`: its hold.
    Stop const* Pick(Unit* mob, uint8* holdOut = nullptr);
};

// The tank's area threat spell around itself (Consecration, Thunder Clap, Swipe, Blood Boil) for the peel: useful
// while a mob hitting a non-tank group member stands within `radius` yards, whatever the current target is. The
// plain melee actions want the current target in melee, so with the tank's target elsewhere the area threat was
// useless and the adds stayed on the casters.
class CastAreaSpellOnGroupMemberAttackersAction : public CastSpellAction
{
public:
    CastAreaSpellOnGroupMemberAttackersAction(PlayerbotAI* botAI, std::string const spell, float radius)
        : CastSpellAction(botAI, spell), name(spell + " on group member attackers"), radius(radius)
    {
    }

    std::string const GetTargetName() override { return "self target"; }
    std::string const getName() override { return name; }
    bool isUseful() override;

private:
    std::string name;
    float radius;
};

class BuffOnPartyAction : public CastBuffSpellAction, public PartyMemberActionNameSupport
{
public:
    BuffOnPartyAction(PlayerbotAI* botAI, std::string const spell)
        : CastBuffSpellAction(botAI, spell), PartyMemberActionNameSupport(spell) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return PartyMemberActionNameSupport::getName(); }
};

class GroupBuffOnPartyAction : public GroupBuffSpellAction, public PartyMemberActionNameSupport
{
public:
    GroupBuffOnPartyAction(PlayerbotAI* botAI, std::string const spell)
        : GroupBuffSpellAction(botAI, spell), PartyMemberActionNameSupport(spell) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return PartyMemberActionNameSupport::getName(); }
};

class CastShootAction : public CastSpellAction
{
public:
    CastShootAction(PlayerbotAI* botAI);

    bool isPossible() override;
    bool Execute(Event event) override;
    // [b35_lock] Single, as the spells: with None the threat hold (ThreatMultiplier) zeroed every damage spell at its
    // limit but left the wand, which then shot alone (destro audit: 6.8 % of the fight, Lethtendris 23-35 s at
    // "multiplier threat x0.00"). The hold also stops a wand that is already repeating (ThreatStrategy.cpp).
    ActionThreatType getThreatType() override { return ActionThreatType::Single; }

private:
    uint32 shootSpellId;
};

class CastLifeBloodAction : public CastHealingSpellAction
{
public:
    CastLifeBloodAction(PlayerbotAI* botAI) : CastHealingSpellAction(botAI, "lifeblood") {}
};

class CastGiftOfTheNaaruAction : public CastHealingSpellAction
{
public:
    CastGiftOfTheNaaruAction(PlayerbotAI* botAI) : CastHealingSpellAction(botAI, "gift of the naaru") {}
};

class CastArcaneTorrentAction : public CastBuffSpellAction
{
public:
    CastArcaneTorrentAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "arcane torrent") {}
};

class CastWarStompAction : public CastMeleeSpellAction
{
public:
    CastWarStompAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "war stomp") {}
};

class CastBloodFuryAction : public CastBuffSpellAction
{
public:
    CastBloodFuryAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "blood fury") {}
};

class CastBerserkingAction : public CastBuffSpellAction
{
public:
    CastBerserkingAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "berserking") {}
};

class CastEveryManForHimselfAction : public CastSpellAction
{
public:
    CastEveryManForHimselfAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "every man for himself") {}

    std::string const GetTargetName() override { return "self target"; }
    bool isPossible() override;
    bool Execute(Event event) override;  // [fear_counter] logs a fear / charm / sleep break
};

class CastWillOfTheForsakenAction : public CastSpellAction
{
public:
    CastWillOfTheForsakenAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "will of the forsaken") {}

    std::string const GetTargetName() override { return "self target"; }
    bool isPossible() override;
    bool Execute(Event event) override;  // [fear_counter] logs a fear / charm / sleep break
};

class CastStoneformAction : public CastSpellAction
{
public:
    CastStoneformAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "stoneform") {}

    std::string const GetTargetName() override { return "self target"; }
};

class CastEscapeArtistAction : public CastSpellAction
{
public:
    CastEscapeArtistAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "escape artist") {}

    std::string const GetTargetName() override { return "self target"; }
};

class UseTrinketAction : public Action
{
public:
    UseTrinketAction(PlayerbotAI* botAI) : Action(botAI, "use trinket") {}
    bool Execute(Event event) override;

protected:
    bool UseTrinket(Item* trinket);

private:
    std::unordered_map<uint64, uint32> trinketItemCooldownExpiries;
    std::unordered_map<uint32, uint32> trinketCategoryCooldownExpiries;
};

class CastSpellOnEnemyHealerAction : public CastSpellAction
{
public:
    CastSpellOnEnemyHealerAction(PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return spell + " on enemy healer"; }
};

class CastSnareSpellAction : public CastDebuffSpellAction
{
public:
    CastSnareSpellAction(PlayerbotAI* botAI, std::string const spell) : CastDebuffSpellAction(botAI, spell) {}

    Value<Unit*>* GetTargetValue() override;
    std::string const getName() override { return spell + " on snare target"; }
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastCrowdControlSpellAction : public CastBuffSpellAction
{
public:
    CastCrowdControlSpellAction(PlayerbotAI* botAI, std::string const spell) : CastBuffSpellAction(botAI, spell) {}

    Value<Unit*>* GetTargetValue() override;
    bool Execute(Event event) override;
    bool isPossible() override;
    bool isUseful() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastProtectSpellAction : public CastSpellAction
{
public:
    CastProtectSpellAction(PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

    std::string const GetTargetName() override;
    bool isUseful() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastVehicleSpellAction : public CastSpellAction
{
public:
    CastVehicleSpellAction(PlayerbotAI* botAI, std::string const& spell) : CastSpellAction(botAI, spell)
    {
        range = 120.0f;
    }

    std::string const GetTargetName() override { return "current target"; }
    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }

protected:
    WorldObject* spellTarget;
};

class CastHurlBoulderAction : public CastVehicleSpellAction
{
public:
    CastHurlBoulderAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "hurl boulder") {}
};

class CastSteamRushAction : public CastVehicleSpellAction
{
public:
    CastSteamRushAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "steam rush") {}
};

class CastRamAction : public CastVehicleSpellAction
{
public:
    CastRamAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "ram") {}
};

class CastNapalmAction : public CastVehicleSpellAction
{
public:
    CastNapalmAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "napalm") {}
};

class CastFireCannonAction : public CastVehicleSpellAction
{
public:
    CastFireCannonAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "fire cannon") {}
};

class CastSteamBlastAction : public CastVehicleSpellAction
{
public:
    CastSteamBlastAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "steam blast") {}
};

class CastIncendiaryRocketAction : public CastVehicleSpellAction
{
public:
    CastIncendiaryRocketAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "incendiary rocket") {}
};

class CastRocketBlastAction : public CastVehicleSpellAction
{
public:
    CastRocketBlastAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "rocket blast") {}
};

class CastGlaiveThrowAction : public CastVehicleSpellAction
{
public:
    CastGlaiveThrowAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "glaive throw") {}
};

class CastBladeSalvoAction : public CastVehicleSpellAction
{
public:
    CastBladeSalvoAction(PlayerbotAI* botAI) : CastVehicleSpellAction(botAI, "blade salvo") {}
};

class MainTankActionNameSupport
{
public:
    MainTankActionNameSupport(std::string spell) { name = std::string(spell) + " on main tank"; }

    virtual std::string const getName() { return name; }

private:
    std::string name;
};

class BuffOnMainTankAction : public CastBuffSpellAction, public MainTankActionNameSupport
{
public:
    BuffOnMainTankAction(PlayerbotAI* botAI, std::string spell, bool checkIsOwner = false)
        : CastBuffSpellAction(botAI, spell, checkIsOwner), MainTankActionNameSupport(spell) {}

public:
    virtual Value<Unit*>* GetTargetValue();
    virtual std::string const getName() { return MainTankActionNameSupport::getName(); }
    // [b31_shared] HoTs and tank buffs (Lifebloom, Earth Shield, Beacon) make no threat on the dps' target
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
    // [heal_triage] A heal-over-time upkeep on a healthy tank yields to a member below medium health.
    bool isUseful() override;

protected:
    bool HotUpkeepYields();
};

#endif
