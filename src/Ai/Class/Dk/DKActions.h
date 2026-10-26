/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DKACTIONS_H
#define PLAYERBOTS_DKACTIONS_H

#include "Event.h"
#include "GenericSpellActions.h"

class PlayerbotAI;

class CastBloodPresenceAction : public CastBuffSpellAction
{
public:
    CastBloodPresenceAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "blood presence") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastFrostPresenceAction : public CastBuffSpellAction
{
public:
    CastFrostPresenceAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "frost presence") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastUnholyPresenceAction : public CastBuffSpellAction
{
public:
    CastUnholyPresenceAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "unholy presence") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastDeathchillAction : public CastBuffSpellAction
{
public:
    CastDeathchillAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "deathchill") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }

    std::vector<NextAction> getPrerequisites() override;
};

class CastDarkCommandAction : public CastSpellAction
{
public:
    CastDarkCommandAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "dark command") {}
};

BEGIN_RANGED_SPELL_ACTION(CastDeathGripAction, "death grip")
END_SPELL_ACTION()

// Unholy presence
class CastUnholyMeleeSpellAction : public CastMeleeSpellAction
{
public:
    CastUnholyMeleeSpellAction(PlayerbotAI* botAI, std::string const spell) : CastMeleeSpellAction(botAI, spell) {}

    std::vector<NextAction> getPrerequisites() override;
};

// Frost presence
class CastFrostMeleeSpellAction : public CastMeleeSpellAction
{
public:
    CastFrostMeleeSpellAction(PlayerbotAI* botAI, std::string const spell) : CastMeleeSpellAction(botAI, spell) {}

    std::vector<NextAction> getPrerequisites() override;
};

// Blood presence
class CastBloodMeleeSpellAction : public CastMeleeSpellAction
{
public:
    CastBloodMeleeSpellAction(PlayerbotAI* botAI, std::string const spell) : CastMeleeSpellAction(botAI, spell) {}

    std::vector<NextAction> getPrerequisites() override;
};

class CastRuneStrikeAction : public CastMeleeSpellAction
{
public:
    CastRuneStrikeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "rune strike") {}
};

class CastPestilenceAction : public CastSpellAction
{
public:
    CastPestilenceAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "pestilence") {}
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
    bool isUseful() override;
};

class CastHowlingBlastAction : public CastSpellAction
{
public:
    CastHowlingBlastAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "howling blast") {}
};

class CastIcyTouchAction : public CastSpellAction
{
public:
    CastIcyTouchAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "icy touch") {}
};

class CastIcyTouchOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastIcyTouchOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "icy touch", true, .0f)
    {
    }
};

// debuff ps

class CastPlagueStrikeAction : public CastSpellAction
{
public:
    CastPlagueStrikeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "plague strike") {}
};

class CastPlagueStrikeOnAttackerAction : public CastDebuffSpellOnMeleeAttackerAction
{
public:
    CastPlagueStrikeOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnMeleeAttackerAction(botAI, "plague strike", true, .0f)
    {
    }
};

// debuff
// [b31_deep] Mark of Blood (49005: 1 Blood rune, 3 min; the target's hits heal their victim 4 % of its maximum health,
// 20 charges): the L80 Blood template takes the talent and no strategy cast it. On a boss under tank pressure
// (BloodDKStrategy), with a Blood or Death rune ready (PlayerbotAI::CanCastSpell does not see rune costs).
class CastMarkOfBloodAction : public CastDebuffSpellAction
{
public:
    CastMarkOfBloodAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "mark of blood") {}

    bool isUseful() override;
};

class CastMarkOfBloodOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastMarkOfBloodOnAttackerAction(PlayerbotAI* botAI) : CastDebuffSpellOnAttackerAction(botAI, "mark of blood", true)
    {
    }
};

class CastUnholyBlightAction : public CastBuffSpellAction
{
public:
    CastUnholyBlightAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "unholy blight") {}
};

class CastSummonGargoyleAction : public CastSpellAction
{
public:
    CastSummonGargoyleAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "summon gargoyle") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastGhoulFrenzyAction : public CastBuffSpellAction
{
public:
    CastGhoulFrenzyAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "ghoul frenzy", false, 5000) {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
    std::string const GetTargetName() override { return "pet target"; }
};

BEGIN_MELEE_SPELL_ACTION(CastCorpseExplosionAction, "corpse explosion")
END_SPELL_ACTION()

class CastAntiMagicShellAction : public CastBuffSpellAction
{
public:
    CastAntiMagicShellAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "anti-magic shell") {}
};

class CastAntiMagicZoneAction : public CastBuffSpellAction
{
public:
    CastAntiMagicZoneAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "anti-magic zone") {}
};

class CastChainsOfIceAction : public CastSpellAction
{
public:
    CastChainsOfIceAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "chains of ice") {}
};

class CastHungeringColdAction : public CastMeleeSpellAction
{
public:
    CastHungeringColdAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "hungering cold") {}
};

class CastHeartStrikeAction : public CastMeleeSpellAction
{
public:
    CastHeartStrikeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "heart strike") {}
};

class CastBloodStrikeAction : public CastMeleeSpellAction
{
public:
    CastBloodStrikeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "blood strike") {}
};

class CastFrostStrikeAction : public CastMeleeSpellAction
{
public:
    CastFrostStrikeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "frost strike") {}
};

class CastObliterateAction : public CastMeleeSpellAction
{
public:
    CastObliterateAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "obliterate") {}
};

class CastDeathStrikeAction : public CastMeleeSpellAction
{
public:
    CastDeathStrikeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "death strike") {}
};

class CastScourgeStrikeAction : public CastMeleeSpellAction
{
public:
    CastScourgeStrikeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "scourge strike") {}
};

class CastDeathCoilAction : public CastSpellAction
{
public:
    CastDeathCoilAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "death coil") {}
};

class CastBloodBoilAction : public CastMeleeSpellAction
{
public:
    CastBloodBoilAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "blood boil") {}
};

class CastDeathAndDecayAction : public CastSpellAction
{
public:
    CastDeathAndDecayAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "death and decay") {}
    // ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    // [b36_tankaoe] a tank lays it on its own pile: the ground spell lands on the target's spot, and the target can be
    // a caster at range or empty while the pack stands on the tank
    Unit* GetTarget() override;
};

class CastHornOfWinterAction : public CastSpellAction
{
public:
    CastHornOfWinterAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "horn of winter") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }

    // [rot_b31_pdd] a party buff around the caster: on "current target" it never ran out of combat (build 30, 1787
    // times "horn of winter ... isUseful false: no target (current target is empty)" from the nc trigger) and the
    // threat multiplier held it in combat
    std::string const GetTargetName() override { return "self target"; }
};

class CastImprovedIcyTalonsAction : public CastBuffSpellAction
{
public:
    CastImprovedIcyTalonsAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "improved icy talons") {}
};

class CastBoneShieldAction : public CastBuffSpellAction
{
public:
    CastBoneShieldAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "bone shield") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastDeathPactAction : public CastBuffSpellAction
{
public:
    CastDeathPactAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "death pact") {}

    // [b31_dk] Death Pact eats the ghoul: with Master of Ghouls it is the permanent pet (a 3 min Raise Dead and the
    // Unholy pet damage), so only when the DK is about to die and no healer can save it.
    bool isUseful() override;
};

class CastDeathRuneMasteryAction : public CastBuffSpellAction
{
public:
    CastDeathRuneMasteryAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "death rune mastery") {}
};

class CastDancingRuneWeaponAction : public CastSpellAction
{
public:
    CastDancingRuneWeaponAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "dancing rune weapon") {}
};

class CastEmpowerRuneWeaponAction : public CastBuffSpellAction
{
public:
    CastEmpowerRuneWeaponAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "empower rune weapon") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastArmyOfTheDeadAction : public CastBuffSpellAction
{
public:
    CastArmyOfTheDeadAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "army of the dead") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastRaiseDeadAction : public CastBuffSpellAction
{
public:
    CastRaiseDeadAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "raise dead") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
    virtual bool Execute(Event event) override;
};

class CastIceboundFortitudeAction : public CastBuffSpellAction
{
public:
    CastIceboundFortitudeAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "icebound fortitude") {}

    // [b31_dk] not on top of a running Vampiric Blood unless health is below DK_ICEBOUND_FORTITUDE_HEALTH_PCT
    bool isUseful() override;
};

class CastUnbreakableArmorAction : public CastBuffSpellAction
{
public:
    CastUnbreakableArmorAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "unbreakable armor") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastVampiricBloodAction : public CastBuffSpellAction
{
public:
    CastVampiricBloodAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "vampiric blood") {}

    // [b31_dk] not on top of a running Icebound Fortitude unless health is below DK_ICEBOUND_FORTITUDE_HEALTH_PCT
    bool isUseful() override;
};

class CastMindFreezeAction : public CastMeleeSpellAction
{
public:
    CastMindFreezeAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "mind freeze") {}
};

class CastStrangulateAction : public CastMeleeSpellAction
{
public:
    CastStrangulateAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "strangulate") {}
};

class CastMindFreezeOnEnemyHealerAction : public CastSpellOnEnemyHealerAction
{
public:
    CastMindFreezeOnEnemyHealerAction(PlayerbotAI* botAI) : CastSpellOnEnemyHealerAction(botAI, "mind freeze") {}
};

class CastRuneTapAction : public CastSpellAction
{
public:
    CastRuneTapAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "rune tap") {}
    std::string const GetTargetName() override { return "self target"; }
};

class CastBloodTapAction : public CastMeleeSpellAction
{
public:
    CastBloodTapAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "blood tap") {}
    // [b31_deep] a self-buff / summon: no threat of its own, the ThreatMultiplier hold does not apply
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

// [bear_aoe_threat] Rune Tap and Vampiric Blood cost a Blood rune. Build 30 Blood DK wipes (strat-h, ok-h, up-h):
// at 26-49 % health both failed in Execute for 4-6 s ("CheckCast ok": the harness check ignores rune costs) while
// Heart Strike / Blood Boil / Death and Decay held the Blood runes, and the tank died with them unused. With no Blood
// or Death rune ready and one of the two off cooldown, Blood Tap (off the global cooldown) activates one for them.
class CastBloodTapForDefenseAction : public CastBuffSpellAction
{
public:
    CastBloodTapForDefenseAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "blood tap") {}

    std::string const getName() override { return "blood tap for defense"; }
    bool isUseful() override;
};

class CastHysteriaAction : public CastSpellAction
{
public:
    CastHysteriaAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "hysteria") {}
    Unit* GetTarget() override;
};

// Raise Ally in 3.3.5 gives the dead player a ghoul to control, so the coordinator's battle res queue only hands
// a death knight dead players driven by a game client (bots cannot play the ghoul).
class CastRaiseAllyAction : public CastSpellAction
{
public:
    CastRaiseAllyAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "raise ally") {}

    std::string const GetTargetName() override { return "battle res target"; }
    bool isUseful() override;
};

#endif
