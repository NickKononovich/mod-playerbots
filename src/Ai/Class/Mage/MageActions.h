/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MAGEACTIONS_H
#define PLAYERBOTS_MAGEACTIONS_H

#include "GenericSpellActions.h"
#include "SharedDefines.h"
#include "UseItemAction.h"

class PlayerbotAI;

// Buff and Out of Combat Actions

class CastMoltenArmorAction : public CastBuffSpellAction
{
public:
    CastMoltenArmorAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "molten armor") {}
    std::vector<NextAction> getAlternatives() override;
};

class CastMageArmorAction : public CastBuffSpellAction
{
public:
    CastMageArmorAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "mage armor") {}
    std::vector<NextAction> getAlternatives() override;
};

class CastIceArmorAction : public CastBuffSpellAction
{
public:
    CastIceArmorAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "ice armor") {}
    std::vector<NextAction> getAlternatives() override;  // [b31_deep] Frost Armor below level 30
};

class CastFrostArmorAction : public CastBuffSpellAction
{
public:
    CastFrostArmorAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "frost armor") {}
};

class CastArcaneIntellectAction : public GroupBuffSpellAction
{
public:
    CastArcaneIntellectAction(PlayerbotAI* botAI) : GroupBuffSpellAction(botAI, "arcane intellect") {}
};

class CastArcaneIntellectOnPartyAction : public GroupBuffOnPartyAction
{
public:
    CastArcaneIntellectOnPartyAction(PlayerbotAI* botAI) : GroupBuffOnPartyAction(botAI, "arcane intellect") {}
};

class CastFocusMagicOnPartyAction : public CastSpellAction
{
public:
    CastFocusMagicOnPartyAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "focus magic") {}
    Unit* GetTarget() override;
};

class CastSummonWaterElementalAction : public CastBuffSpellAction
{
public:
    CastSummonWaterElementalAction(PlayerbotAI* botAI)
        : CastBuffSpellAction(botAI, "summon water elemental") {}
};

// Boost Actions

class CastCombustionAction : public CastBuffSpellAction
{
public:
    CastCombustionAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "combustion") {}
};

class CastArcanePowerAction : public CastBuffSpellAction
{
public:
    CastArcanePowerAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "arcane power") {}
};

class CastPresenceOfMindAction : public CastBuffSpellAction
{
public:
    CastPresenceOfMindAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "presence of mind") {}
};

class CastIcyVeinsAction : public CastBuffSpellAction
{
public:
    CastIcyVeinsAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "icy veins") {}
};

class CastColdSnapAction : public CastBuffSpellAction
{
public:
    CastColdSnapAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "cold snap") {}
};

// Defensive Actions

class CastFireWardAction : public CastBuffSpellAction
{
public:
    CastFireWardAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "fire ward") {}
};

class CastFrostWardAction : public CastBuffSpellAction
{
public:
    CastFrostWardAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "frost ward") {}
};

class CastIceBarrierAction : public CastBuffSpellAction
{
public:
    CastIceBarrierAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "ice barrier") {}
};

class CastInvisibilityAction : public CastBuffSpellAction
{
public:
    CastInvisibilityAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "invisibility") {}
};
class CastIceBlockAction : public CastBuffSpellAction
{
public:
    CastIceBlockAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "ice block") {}
};

class CastMirrorImageAction : public CastBuffSpellAction
{
public:
    CastMirrorImageAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "mirror image") {}
};

class CastBlinkBackAction : public CastSpellAction
{
public:
    CastBlinkBackAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "blink") {}
    bool Execute(Event event) override;
};

class CastManaShieldAction : public CastBuffSpellAction
{
public:
    CastManaShieldAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "mana shield") {}
};

// Utility Actions

class CastEvocationAction : public CastSpellAction
{
public:
    CastEvocationAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "evocation") {}
    std::string const GetTargetName() override { return "self target"; }
};

// [conjure_outside_instances] Mages conjure food / water / Mana Gems / Refreshment Tables before the run or in the
// open world, never in a dungeon or raid (user rule 2026-10-25; hor-h-5A-1: the mage conjured and handed out food
// for 50 s at the entrance while the group walked on). In an instance one conjure is allowed only when the mage's
// own stock of that kind is completely empty and the group rests anyway. Outside, the mage still waits while its
// group leader walks away from it. what: the spell for the once-per-run skip log line.
constexpr bool CONJURE_OUTSIDE_INSTANCES = true;
bool MageConjureAllowed(PlayerbotAI* botAI, Player* bot, bool ownStockEmpty, char const* what);

class CastConjureManaGemAction : public CastBuffSpellAction
{
public:
    CastConjureManaGemAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "conjure mana gem") {}
    bool isUseful() override;
};

class CastConjureFoodAction : public CastBuffSpellAction
{
public:
    CastConjureFoodAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "conjure food") {}
    bool isUseful() override;
};

class CastConjureWaterAction : public CastBuffSpellAction
{
public:
    CastConjureWaterAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "conjure water") {}
    bool isUseful() override;
};

// Level 75+: Conjured Mana Pie / Strudel, food that also restores mana (counted as conjured food by the bots)
class CastConjureRefreshmentAction : public CastBuffSpellAction
{
public:
    CastConjureRefreshmentAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "conjure refreshment") {}
    bool isUseful() override;
};

// Reagent: Arcane Powder; creates the Refreshment Portal that two more group members click
class CastRitualOfRefreshmentAction : public CastSpellAction
{
public:
    CastRitualOfRefreshmentAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "ritual of refreshment") {}
    std::string const GetTargetName() override { return "self target"; }
    bool isUseful() override;
};

class UseManaSapphireAction : public UseItemAction
{
public:
    UseManaSapphireAction(PlayerbotAI* botAI) : UseItemAction(botAI, "mana sapphire") {}
    bool isUseful() override;
};
class UseManaEmeraldAction : public UseItemAction
{
public:
    UseManaEmeraldAction(PlayerbotAI* botAI) : UseItemAction(botAI, "mana emerald") {}
    bool isUseful() override;
};

class UseManaRubyAction : public UseItemAction
{
public:
    UseManaRubyAction(PlayerbotAI* botAI) : UseItemAction(botAI, "mana ruby") {}
    bool isUseful() override;
};

class UseManaCitrineAction : public UseItemAction
{
public:
    UseManaCitrineAction(PlayerbotAI* botAI) : UseItemAction(botAI, "mana citrine") {}
    bool isUseful() override;
};

class UseManaJadeAction : public UseItemAction
{
public:
    UseManaJadeAction(PlayerbotAI* botAI) : UseItemAction(botAI, "mana jade") {}
    bool isUseful() override;
};

class UseManaAgateAction : public UseItemAction
{
public:
    UseManaAgateAction(PlayerbotAI* botAI) : UseItemAction(botAI, "mana agate") {}
    bool isUseful() override;
};

// CC, Interrupt, and Dispel Actions

class CastPolymorphAction : public CastCrowdControlSpellAction
{
public:
    CastPolymorphAction(PlayerbotAI* botAI) : CastCrowdControlSpellAction(botAI, "polymorph") {}
};

class CastSpellstealAction : public CastSpellAction
{
public:
    CastSpellstealAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "spellsteal") {}
};

class CastCounterspellAction : public CastSpellAction
{
public:
    CastCounterspellAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "counterspell") {}
};

class CastCounterspellOnEnemyHealerAction : public CastSpellOnEnemyHealerAction
{
public:
    CastCounterspellOnEnemyHealerAction(PlayerbotAI* botAI)
        : CastSpellOnEnemyHealerAction(botAI, "counterspell") {}
};

class CastFrostNovaAction : public CastSpellAction
{
public:
    CastFrostNovaAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "frost nova") {}
    bool isUseful() override;
};

// Frost Nova for the healer ("healer attacker in frost nova range"), whatever the mage's own target is.
class CastFrostNovaOnHealerAttackerAction : public CastSpellAction
{
public:
    CastFrostNovaOnHealerAttackerAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "frost nova") {}

    std::string const GetTargetName() override { return "self target"; }
    std::string const getName() override { return "frost nova on healer attacker"; }
};

class CastDeepFreezeAction : public CastSpellAction
{
public:
    CastDeepFreezeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "deep freeze") {}
    // [b31_deep] was "return true": known / cooldown / range / mana were never checked and the cast failed 9056
    // times on cooldown. The frozen state (Fingers of Frost counts as frozen) is still left to the core.
    bool isPossible() override;
    // [ranged_audit_b30] In a group only on a stun-immune creature, the only one it damages.
    bool isUseful() override;
};

class CastRemoveCurseAction : public CastCureSpellAction
{
public:
    CastRemoveCurseAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "remove curse") {}
};

class CastRemoveLesserCurseAction : public CastCureSpellAction
{
public:
    CastRemoveLesserCurseAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "remove lesser curse") {}
};

class CastRemoveCurseOnPartyAction : public CurePartyMemberAction
{
public:
    CastRemoveCurseOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "remove curse", DISPEL_CURSE) {}
};

class CastRemoveLesserCurseOnPartyAction : public CurePartyMemberAction
{
public:
    CastRemoveLesserCurseOnPartyAction(PlayerbotAI* botAI)
        : CurePartyMemberAction(botAI, "remove lesser curse", DISPEL_CURSE) {}
};

// Damage and Debuff Actions

class CastFireballAction : public CastSpellAction
{
public:
    CastFireballAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "fireball") {}
};

class CastScorchAction : public CastSpellAction
{
public:
    CastScorchAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "scorch") {}
};

class CastFireBlastAction : public CastSpellAction
{
public:
    CastFireBlastAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "fire blast") {}
};

class CastArcaneBlastAction : public CastSpellAction
{
public:
    CastArcaneBlastAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "arcane blast") {}
};

class CastArcaneBarrageAction : public CastSpellAction
{
public:
    CastArcaneBarrageAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "arcane barrage") {}
};

class CastArcaneMissilesAction : public CastSpellAction
{
public:
    CastArcaneMissilesAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "arcane missiles") {}
};

class CastPyroblastAction : public CastSpellAction
{
public:
    CastPyroblastAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "pyroblast") {}
};

class CastLivingBombAction : public CastDebuffSpellAction
{
public:
    // [b31_deep] 12 s target lifetime, as the trigger (LIVING_BOMB_MIN_TARGET_LIFETIME, MageTriggers.h)
    CastLivingBombAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "living bomb", true, 12.0f) {}
    bool isUseful() override;
};

class CastLivingBombOnAttackersAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastLivingBombOnAttackersAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "living bomb", true, 12.0f) {}  // [b31_deep] 12 s lifetime
    bool isUseful() override;
};

class CastFrostboltAction : public CastSpellAction
{
public:
    CastFrostboltAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "frostbolt") {}
};

class CastFrostfireBoltAction : public CastSpellAction
{
public:
    CastFrostfireBoltAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "frostfire bolt") {}
};

class CastIceLanceAction : public CastSpellAction
{
public:
    CastIceLanceAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "ice lance") {}
};

class CastBlizzardAction : public CastSpellAction
{
public:
    CastBlizzardAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "blizzard") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class CastConeOfColdAction : public CastSpellAction
{
public:
    CastConeOfColdAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "cone of cold") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class CastFlamestrikeAction : public CastDebuffSpellAction
{
public:
    CastFlamestrikeAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "flamestrike", true, 0.0f) {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class CastDragonsBreathAction : public CastSpellAction
{
public:
    CastDragonsBreathAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "dragon's breath") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class CastBlastWaveAction : public CastSpellAction
{
public:
    CastBlastWaveAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "blast wave") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

#endif
