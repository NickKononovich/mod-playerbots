/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HUNTERACTIONS_H
#define PLAYERBOTS_HUNTERACTIONS_H

#include "AiObjectContext.h"
#include "AiObject.h"
#include "Event.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"  // [guild14]
#include "Unit.h"

class Item;
class Pet;
class PlayerbotAI;
class Unit;

// Buff and Out of Combat Spells

class CastTrueshotAuraAction : public CastBuffSpellAction
{
public:
    CastTrueshotAuraAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "trueshot aura") {}
};

class CastAspectOfTheDragonhawkAction : public CastBuffSpellAction
{
public:
    CastAspectOfTheDragonhawkAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "aspect of the dragonhawk") {}
};

class CastAspectOfTheHawkAction : public CastBuffSpellAction
{
public:
    CastAspectOfTheHawkAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "aspect of the hawk") {}
    bool isUseful() override;
};

class CastAspectOfTheMonkeyAction : public CastBuffSpellAction
{
public:
    CastAspectOfTheMonkeyAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "aspect of the monkey") {}
};

class CastAspectOfTheWildAction : public CastBuffSpellAction
{
public:
    CastAspectOfTheWildAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "aspect of the wild") {}
};

class CastAspectOfTheCheetahAction : public CastBuffSpellAction
{
public:
    CastAspectOfTheCheetahAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "aspect of the cheetah") {}
};

class CastAspectOfThePackAction : public CastBuffSpellAction
{
public:
    CastAspectOfThePackAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "aspect of the pack") {}
};

class CastAspectOfTheViperAction : public CastBuffSpellAction
{
public:
    CastAspectOfTheViperAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "aspect of the viper") {}
};

// Cooldown Spells

class CastRapidFireAction : public CastBuffSpellAction
{
public:
    CastRapidFireAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "rapid fire") {}
};

class CastDeterrenceAction : public CastBuffSpellAction
{
public:
    CastDeterrenceAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "deterrence") {}
};

class CastReadinessAction : public CastBuffSpellAction
{
public:
    CastReadinessAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "readiness") {}
};

class CastDisengageAction : public CastSpellAction
{
public:
    CastDisengageAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "disengage") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

// CC Spells

class CastScareBeastAction : public CastSpellAction
{
public:
    CastScareBeastAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "scare beast") {}
};

class CastScareBeastCcAction : public CastSpellAction
{
public:
    CastScareBeastCcAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "scare beast on cc") {}
    Value<Unit*>* GetTargetValue() override;
    bool Execute(Event event) override;
};

class CastFreezingTrap : public CastDebuffSpellAction
{
public:
    CastFreezingTrap(PlayerbotAI* botAI) :
        CastDebuffSpellAction(botAI, "freezing trap") {}
    Value<Unit*>* GetTargetValue() override;
};

class CastWyvernStingAction : public CastDebuffSpellAction
{
public:
    CastWyvernStingAction(PlayerbotAI* botAI) :
        CastDebuffSpellAction(botAI, "wyvern sting", true) {}
};

class CastSilencingShotAction : public CastSpellAction
{
public:
    CastSilencingShotAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "silencing shot") {}
};

// [b31_deep] wowsims MM fires Silencing Shot on cooldown as a damage shot. The bot holds it for interrupts, which a
// boss that its silence and interrupt cannot affect makes pointless: there it is spent on cooldown. [rot135] A led
// hunter spends it on cooldown on every target: its interrupts are the leader's orders on the ready interrupts.
class CastSilencingShotOnCooldownAction : public CastSilencingShotAction
{
public:
    CastSilencingShotOnCooldownAction(PlayerbotAI* botAI) : CastSilencingShotAction(botAI) {}
    std::string const getName() override { return "silencing shot on cooldown"; }
    bool isUseful() override;
};

class CastConcussiveShotAction : public CastSnareSpellAction
{
public:
    CastConcussiveShotAction(PlayerbotAI* botAI) :
        CastSnareSpellAction(botAI, "concussive shot") {}
};

class CastIntimidationAction : public CastBuffSpellAction
{
public:
    CastIntimidationAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "intimidation", false, 5000) {}
    std::string const GetTargetName() override
    {
        return "pet target";
    }
};

// Threat Spells

class CastDistractingShotAction : public CastSpellAction
{
public:
    CastDistractingShotAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "distracting shot") {}
};

// [b31_deep] Misdirection puts its 30 s aura (34477) and the 4 s threat-transfer buff (35079) on the HUNTER; the
// tank never carries one, so a check on the tank was always true.
constexpr uint32 SPELL_MISDIRECTION = 34477;
constexpr uint32 SPELL_MISDIRECTION_TRANSFER = 35079;

// Raid utility coordinator: the tank holding the current pull, else the main tank.
class CastMisdirectionOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastMisdirectionOnMainTankAction(PlayerbotAI* botAI) :
        BuffOnMainTankAction(botAI, "misdirection", true) {}
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
    bool isUseful() override;  // [b31_deep]

    std::string const GetTargetName() override { return "threat redirect target"; }
    Value<Unit*>* GetTargetValue() override { return context->GetValue<Unit*>("threat redirect target"); }
};

class CastFeignDeathAction : public CastBuffSpellAction
{
public:
    CastFeignDeathAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "feign death") {}
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

// Pet Spells

// Feeds the hunter pet with real food from the bags through the Feed Pet spell (no free happiness).
class FeedPetAction : public Action
{
public:
    FeedPetAction(PlayerbotAI* botAI) : Action(botAI, "feed pet") {}
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    Item* FindPetFood(Pet* pet);

    uint32 _noFoodUntil = 0;
};

class CastCallPetAction : public CastBuffSpellAction
{
public:
    CastCallPetAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "call pet") {}
};

class CastMendPetAction : public CastAuraSpellAction
{
public:
    CastMendPetAction(PlayerbotAI* botAI) : CastAuraSpellAction(botAI, "mend pet") {}
    std::string const GetTargetName() override
    {
        return "pet target";
    }
};

class CastRevivePetAction : public CastBuffSpellAction
{
public:
    CastRevivePetAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "revive pet") {}
};

class CastKillCommandAction : public CastBuffSpellAction
{
public:
    CastKillCommandAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "kill command", false, 5000) {}
    std::string const GetTargetName() override
    {
        return "pet target";
    }
};

class CastBestialWrathAction : public CastBuffSpellAction
{
public:
    CastBestialWrathAction(PlayerbotAI* botAI) :
        CastBuffSpellAction(botAI, "bestial wrath", false, 5000) {}
    std::string const GetTargetName() override
    {
        return "pet target";
    }
};

// Direct Damage Spells

class CastAutoShotAction : public CastSpellAction
{
public:
    CastAutoShotAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "auto shot") {}
    ActionThreatType getThreatType() override
    {
        return ActionThreatType::None;
    }
    bool isUseful() override;
};

// [rot608] the fitting ammo from its bags into the ammo slot (HunterAmmoMismatch)
class HunterEquipMatchingAmmoAction : public Action
{
public:
    HunterEquipMatchingAmmoAction(PlayerbotAI* botAI) : Action(botAI, "equip matching ammo") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class CastArcaneShotAction : public CastSpellAction
{
public:
    CastArcaneShotAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "arcane shot") {}
    bool isUseful() override;
};

class CastAimedShotAction : public CastSpellAction
{
public:
    CastAimedShotAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "aimed shot") {}
};

class CastChimeraShotAction : public CastSpellAction
{
public:
    CastChimeraShotAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "chimera shot") {}
};

class CastSteadyShotAction : public CastSpellAction
{
public:
    CastSteadyShotAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "steady shot") {}
};

class CastKillShotAction : public CastSpellAction
{
public:
    CastKillShotAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "kill shot") {}
};

// DoT/Debuff Spells

class CastHuntersMarkAction : public CastDebuffSpellAction
{
public:
    CastHuntersMarkAction(PlayerbotAI* botAI) :
        CastDebuffSpellAction(botAI, "hunter's mark") {}
    bool isUseful() override
    {
        // Bypass TTL check
        return CastAuraSpellAction::isUseful();
    }
};

class CastTranquilizingShotAction : public CastSpellAction
{
public:
    CastTranquilizingShotAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "tranquilizing shot") {}
};

class CastViperStingAction : public CastDebuffSpellAction
{
public:
    CastViperStingAction(PlayerbotAI* botAI) :
        CastDebuffSpellAction(botAI, "viper sting", true) {}
    bool isUseful() override;
};

// [guide_b31_dead] Marksmanship talent; its shot refreshes the hunter's own Serpent Sting
constexpr uint32 SPELL_CHIMERA_SHOT = 53209;

// [rot550] Serpent Sting by wowsims' hunter APLs (bm / mm / sv: "not dotIsActive(49001)"): re-applied only once our own
// DoT is over, since a recast restarts its tick timer (core/dot.go Apply: Cancel, TickCount 0) and the tick in progress
// is lost (b275 rfc-h 5F-6 Rthunac: recast ~0.3 s before the end on Oggleflint / Taragaman, 67 % of the sim's damage per
// cast). True when our sting is absent or every tick of it has landed.
class Aura;
bool SerpentStingDue(Aura* own);
// [rot550] The ticks that land before the target dies (ExpectedFightLifetime) beat Steady Shot, the filler every
// wowsims hunter APL ends with, in the sting's own time (SpellPayoff::DotPays); no fixed lifetime (b275 5F-6: a third
// sting on Taragaman at 8.5 % health landed 1-2 ticks).
// [hunt20] Steady Shot is learned at level 50 (Spell.dbc baseLevel 50), so below it there is no filler to weigh the
// sting against, and this test used to answer "it pays" on that alone - a check that cannot fail is not a check. Over
// an 84 min guild window (wc-n, five hunters L24-26) it refused exactly 0 times: not one `[rot550]` line in the whole
// log, while Serpent Sting took 39 % of every mana unit those hunters spent - 9 % of base mana a cast
// (Spell.dbc field 204 ManaCostPercentage), the dearest shot they own, equal to Multi-Shot and 1.8x Arcane Shot.
// With no filler the gate is wowsims' own gate for this very cast, read from the local source:
//     C:/WoW/tools/simpct_data/vendor/src/wotlk/ui/hunter/apls/sv.apl.json
//     {"not": {"dotIsActive": {"spellId": 49001}}} AND
//     {"cmp": {"op": "OpGt", "lhs": {"remainingTime": {}}, "rhs": {"const": {"val": "6s"}}}}
// wowsims' `remainingTime` is the fight's time left; on trash the fight ends when the mob dies, so the bot's
// ExpectedFightLifetime of the target is that same quantity. The number is wowsims', on the same footing as the
// 10 / 30 % Viper band in HealthTriggers.h - not a line of ours.
// `loggedMs`: the caller's log clock; a held sting is logged at most every SERPENT_STING_PAYOFF_LOG_MS (none: no log)
constexpr uint32 SERPENT_STING_PAYOFF_LOG_MS = 5000;
constexpr float SERPENT_STING_WOWSIMS_MIN_FIGHT_SEC = 6.0f;
bool SerpentStingPays(PlayerbotAI* botAI, Player* bot, Unit* target, uint32* loggedMs = nullptr);

class CastSerpentStingAction : public CastDebuffSpellAction
{
public:
    CastSerpentStingAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "serpent sting", true, 0.0f) {}

    // [guide_b31_dead] Chimera Shot refreshes our Serpent Sting, so with it the sting is only re-applied when missing
    // [rot550] without it once its last tick has landed, and only where its ticks pay
    bool isUseful() override;
};

class CastScorpidStingAction : public CastDebuffSpellAction
{
public:
    CastScorpidStingAction(PlayerbotAI* botAI) :
        CastDebuffSpellAction(botAI, "scorpid sting", true) {}
    bool isUseful() override
    {
        // Bypass TTL check
        return CastAuraSpellAction::isUseful();
    }
};

class CastSerpentStingOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastSerpentStingOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "serpent sting", true, 0.0f) {}
    bool isUseful() override;  // [rot550] SerpentStingPays on the attacker
};

class CastImmolationTrapAction : public CastSpellAction
{
public:
    CastImmolationTrapAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "immolation trap") {}
    bool isUseful() override;
};

class CastExplosiveTrapAction : public CastSpellAction
{
public:
    CastExplosiveTrapAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "explosive trap") {}
    bool isUseful() override;  // [b31_deep] yields the shared fire-trap cooldown to a ready Black Arrow
};

class CastBlackArrowAction : public CastDebuffSpellAction
{
public:
    CastBlackArrowAction(PlayerbotAI* botAI) :
        CastDebuffSpellAction(botAI, "black arrow", true) {}
    bool isUseful() override
    {
        if (botAI->HasStrategy("trap weave", BOT_STATE_COMBAT))
            return false;
        // Bypass TTL check
        return CastAuraSpellAction::isUseful();
    }
};

class CastExplosiveShotBaseAction : public CastDebuffSpellAction
{
public:
    CastExplosiveShotBaseAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "explosive shot", true, 0.0f) {}
};

// Rank 4
class CastExplosiveShotRank4Action : public CastExplosiveShotBaseAction
{
public:
    CastExplosiveShotRank4Action(PlayerbotAI* botAI) :
        CastExplosiveShotBaseAction(botAI) {}
    bool Execute(Event /*event*/) override
    {
        return botAI->CastSpell(60053, GetTarget());
    }
    bool isUseful() override
    {
        Unit* target = GetTarget();
        if (!target)
            return false;

        return !target->HasAura(60053, bot->GetGUID());  // [b31_deep] our own DoT, not another hunter's
    }
};

// Rank 3
class CastExplosiveShotRank3Action : public CastExplosiveShotBaseAction
{
public:
    CastExplosiveShotRank3Action(PlayerbotAI* botAI) :
        CastExplosiveShotBaseAction(botAI) {}
    bool Execute(Event /*event*/) override
    {
        return botAI->CastSpell(60052, GetTarget());
    }
    bool isUseful() override
    {
        Unit* target = GetTarget();
        if (!target)
            return false;

        return !target->HasAura(60052, bot->GetGUID());  // [b31_deep] our own DoT, not another hunter's
    }
};

// Rank 2
class CastExplosiveShotRank2Action : public CastExplosiveShotBaseAction
{
public:
    CastExplosiveShotRank2Action(PlayerbotAI* botAI) :
        CastExplosiveShotBaseAction(botAI) {}
    bool Execute(Event /*event*/) override
    {
        return botAI->CastSpell(60051, GetTarget());
    }
    bool isUseful() override
    {
        Unit* target = GetTarget();
        if (!target)
            return false;

        return !target->HasAura(60051, bot->GetGUID());  // [b31_deep] our own DoT, not another hunter's
    }
};

// Rank 1
class CastExplosiveShotRank1Action : public CastExplosiveShotBaseAction
{
public:
    CastExplosiveShotRank1Action(PlayerbotAI* botAI) :
        CastExplosiveShotBaseAction(botAI) {}
    bool Execute(Event /*event*/) override
    {
        return botAI->CastSpell(53301, GetTarget());
    }
    bool isUseful() override
    {
        Unit* target = GetTarget();
        if (!target)
            return false;

        return !target->HasAura(53301, bot->GetGUID());  // [b31_deep] our own DoT, not another hunter's
    }
};

// [b31_deep] Lock and Load double tap (wowsims sv.apl, Warcraft Tavern SV): the proc's two free shots go to the
// highest known Explosive Shot rank, and to the next lower rank while our own highest-rank DoT still ticks (ranks
// of Explosive Shot stack on the target), so the second charge neither clips nor waits for the first DoT.
constexpr uint32 EXPLOSIVE_SHOT_RANKS[] = { 60053, 60052, 60051, 53301 };

class CastExplosiveShotLockAndLoadAction : public CastExplosiveShotBaseAction
{
public:
    CastExplosiveShotLockAndLoadAction(PlayerbotAI* botAI) : CastExplosiveShotBaseAction(botAI) {}
    std::string const getName() override { return "explosive shot lock and load"; }
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    uint32 PickRank();
};

// Melee Spells

class CastWingClipAction : public CastSpellAction
{
public:
    CastWingClipAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "wing clip") {}
    bool isUseful() override;
    std::vector<NextAction> getPrerequisites() override;
};

class CastRaptorStrikeAction : public CastSpellAction
{
public:
    CastRaptorStrikeAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "raptor strike") {}
};

class CastMongooseBiteAction : public CastSpellAction
{
public:
    CastMongooseBiteAction(PlayerbotAI* botAI) :
        CastSpellAction(botAI, "mongoose bite") {}
};

// AoE Spells

class CastMultiShotAction : public CastSpellAction
{
public:
    CastMultiShotAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "multi-shot") {}
    bool isUseful() override;  // [b33_aoe_class] not next to a crowd-controlled mob
};

class CastVolleyAction : public CastSpellAction
{
public:
    CastVolleyAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "volley") {}
    bool isUseful() override;  // [b33_aoe_class] not next to a crowd-controlled mob
    ActionThreatType getThreatType() override
    {
        return ActionThreatType::Aoe;
    }
};

// [guild14] steps out of Auto Shot's dead zone to the spot HunterStepOutSpot finds, then shoots
class HunterStepOutAction : public MovementAction
{
public:
    HunterStepOutAction(PlayerbotAI* botAI) : MovementAction(botAI, "hunter step out") {}

    bool Execute(Event event) override;
};

#endif
