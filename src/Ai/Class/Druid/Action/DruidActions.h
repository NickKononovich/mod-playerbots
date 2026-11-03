/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DRUIDACTIONS_H
#define PLAYERBOTS_DRUIDACTIONS_H

#include "DruidTriggers.h"  // [b31_sd] DRUID_MULTI_DOT_MIN_LIFETIME
#include "GenericSpellActions.h"
#include "SharedDefines.h"
#include "Value.h"

class PlayerbotAI;
class Unit;

class CastFaerieFireAction : public CastDebuffSpellAction
{
public:
    CastFaerieFireAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "faerie fire") {}
};

class CastFaerieFireFeralAction : public CastSpellAction
{
public:
    CastFaerieFireFeralAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "faerie fire (feral)") {}
};

class CastRejuvenationAction : public CastHealingSpellAction
{
public:
    CastRejuvenationAction(PlayerbotAI* botAI) : CastHealingSpellAction(botAI, "rejuvenation") {}
};

class CastRegrowthAction : public CastHealingSpellAction
{
public:
    CastRegrowthAction(PlayerbotAI* botAI) : CastHealingSpellAction(botAI, "regrowth") {}
};

class CastHealingTouchAction : public CastHealingSpellAction
{
public:
    CastHealingTouchAction(PlayerbotAI* botAI) : CastHealingSpellAction(botAI, "healing touch") {}
};

class CastRejuvenationOnPartyAction : public HealPartyMemberAction
{
public:
    CastRejuvenationOnPartyAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "rejuvenation", 15.0f, HealingManaEfficiency::VERY_HIGH)
    {
    }
};

class CastRegrowthOnPartyAction : public HealPartyMemberAction
{
public:
    CastRegrowthOnPartyAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "regrowth", 35.0f, HealingManaEfficiency::HIGH)
    {
    }
};

class CastHealingTouchOnPartyAction : public HealPartyMemberAction
{
public:
    CastHealingTouchOnPartyAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "healing touch", 50.0f, HealingManaEfficiency::MEDIUM)
    {
    }
};

// [b31_sd] Healing Touch in the resto health bands. The guides cast it only under Nature's Swiftness (that node uses
// "healing touch on party"); this filler stays only for a druid without Nourish (level 80), whose direct heals are
// otherwise Regrowth alone.
inline constexpr uint32 SPELL_NOURISH_RANK_1 = 50464;

class CastHealingTouchFillerOnPartyAction : public CastHealingTouchOnPartyAction
{
public:
    CastHealingTouchFillerOnPartyAction(PlayerbotAI* botAI) : CastHealingTouchOnPartyAction(botAI) {}

    std::string const getName() override { return "healing touch filler on party"; }
    bool isUseful() override
    {
        return !bot->HasSpell(SPELL_NOURISH_RANK_1) && CastHealingTouchOnPartyAction::isUseful();
    }
};

class CastReviveAction : public ResurrectPartyMemberAction
{
public:
    CastReviveAction(PlayerbotAI* botAI) : ResurrectPartyMemberAction(botAI, "revive") {}

    std::vector<NextAction> getPrerequisites() override;
};

class CastRebirthAction : public ResurrectPartyMemberAction
{
public:
    CastRebirthAction(PlayerbotAI* botAI) : ResurrectPartyMemberAction(botAI, "rebirth") {}

    // Battle res queue of the raid utility coordinator: two ressers never pick the same corpse.
    std::string const GetTargetName() override { return "battle res target"; }
    std::vector<NextAction> getPrerequisites() override;
    bool isUseful() override;
    bool Execute(Event event) override;  // remembers the ally for the follow-up heals (RebirthFollowUp)
};

// [b32_brez] The bear tank's last-resort battle res (BearBattleRes, DruidTriggers.h): leaves its form and starts
// Rebirth in one step, so no tick passes in caster form before the cast (and the "caster form" prerequisite's
// medium-mana gate cannot leave the cast to fail in form). "bear battle res reshift" brings Dire Bear Form back.
class CastBearBattleResAction : public CastRebirthAction
{
public:
    CastBearBattleResAction(PlayerbotAI* botAI) : CastRebirthAction(botAI) {}

    std::string const getName() override { return "bear battle res"; }
    std::string const GetTargetName() override { return "bear battle res target"; }
    std::vector<NextAction> getPrerequisites() override { return Action::getPrerequisites(); }
    bool isUseful() override;
    bool Execute(Event event) override;
};

// An ally back from Rebirth stands in the fight low and unbuffed: the druid that brought it back heals it up and
// gives it Mark of the Wild before anything else, for a while after the cast.
namespace RebirthFollowUp
{
    constexpr uint32 WINDOW_MS = 30000;
    constexpr float SAFE_HEALTH_PCT = 70.0f;

    void Record(PlayerbotAI* botAI, Unit* target);
    // The ally within the window once it is alive again, else nullptr.
    Unit* GetTarget(PlayerbotAI* botAI);
}

class CastSpellOnRebirthTargetAction : public CastSpellAction
{
public:
    // hot: not cast again while this druid's own aura of it is on the target
    CastSpellOnRebirthTargetAction(PlayerbotAI* botAI, std::string const spell, bool hot = false)
        : CastSpellAction(botAI, spell), name(spell + " on rebirth target"), isHot(hot)
    {
        range = botAI->GetRange("heal");
    }

    std::string const getName() override { return name; }
    Unit* GetTarget() override { return RebirthFollowUp::GetTarget(botAI); }
    bool isUseful() override;

private:
    std::string const name;
    bool const isHot;
};

class CastBuffOnRebirthTargetAction : public CastSpellAction
{
public:
    // sameBuff: an aura that covers it (Gift of the Wild for the Mark); tankOnly: Thorns goes on a tank
    CastBuffOnRebirthTargetAction(PlayerbotAI* botAI, std::string const spell, std::string const sameBuff,
                                  bool tankOnly)
        : CastSpellAction(botAI, spell), name(spell + " on rebirth target"), coveringBuff(sameBuff),
          onlyTank(tankOnly)
    {
        range = botAI->GetRange("heal");
    }

    std::string const getName() override { return name; }
    Unit* GetTarget() override { return RebirthFollowUp::GetTarget(botAI); }
    bool isUseful() override;
    std::vector<NextAction> getPrerequisites() override;

private:
    std::string const name;
    std::string const coveringBuff;
    bool const onlyTank;
};

class CastMarkOfTheWildAction : public GroupBuffSpellAction
{
public:
    CastMarkOfTheWildAction(PlayerbotAI* botAI) : GroupBuffSpellAction(botAI, "mark of the wild") {}
};

class CastMarkOfTheWildOnPartyAction : public GroupBuffOnPartyAction
{
public:
    CastMarkOfTheWildOnPartyAction(PlayerbotAI* botAI) : GroupBuffOnPartyAction(botAI, "mark of the wild") {}
};

// [rot371] Survival Instincts and Frenzied Regeneration of a led bear tank are the leader's (user 2026-11-03: only as
// its health drops, the leader decides): its External ("leader external", the spell by id) or its PreSave ("leader pre
// save") casts them; the bear's own triggers (medium health, damage spike, tank pressure, the set bonus) do not.
// Out of a led group the bear's own health rules stay.
bool BearSaveLeaderOwned(Player* bot, char const* spell);

class CastSurvivalInstinctsAction : public CastBuffSpellAction
{
public:
    CastSurvivalInstinctsAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "survival instincts") {}

    bool isUseful() override
    {
        return !BearSaveLeaderOwned(bot, "Survival Instincts") && CastBuffSpellAction::isUseful();
    }
};

class CastFrenziedRegenerationAction : public CastBuffSpellAction
{
public:
    CastFrenziedRegenerationAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "frenzied regeneration") {}

    bool isUseful() override
    {
        return !BearSaveLeaderOwned(bot, "Frenzied Regeneration") && CastBuffSpellAction::isUseful();
    }
};

class CastThornsAction : public CastBuffSpellAction
{
public:
    CastThornsAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "thorns") {}

    bool Execute(Event event) override;
};

class CastThornsOnPartyAction : public BuffOnPartyAction
{
public:
    CastThornsOnPartyAction(PlayerbotAI* botAI) : BuffOnPartyAction(botAI, "thorns") {}

    bool Execute(Event event) override;
};

class CastThornsOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastThornsOnMainTankAction(PlayerbotAI* botAI) : BuffOnMainTankAction(botAI, "thorns", false) {}

    bool Execute(Event event) override;
};

// [b31_deep] The tank HoT upkeeps are heals, not single-target threat: heal threat is split over every mob, so the
// threat strategy holding them (CastSpellAction's default Single type, measured on the healer's current target)
// lowered nothing and broke the rolling Lifebloom stack (7076 "multiplier threat x0" on L80 bosses).
class CastRejuvenationOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastRejuvenationOnMainTankAction(PlayerbotAI* botAI) : BuffOnMainTankAction(botAI, "rejuvenation", true) {}

    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastLifebloomOnMainTankAction : public BuffOnMainTankAction
{
public:
    CastLifebloomOnMainTankAction(PlayerbotAI* botAI) : BuffOnMainTankAction(botAI, "lifebloom", true) {}

    bool isUseful() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastWrathAction : public CastSpellAction
{
public:
    CastWrathAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "wrath") {}
    bool isUseful() override;
};

class CastStarfallAction : public CastSpellAction
{
public:
    CastStarfallAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "starfall") {}

    bool isUseful() override;
    // [rot1] the stars hit every enemy within 30 yd of the druid: the area threat gate reads them all
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
};

class CastHurricaneAction : public CastSpellAction
{
public:
    CastHurricaneAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "hurricane") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
};

class CastTyphoonAction : public CastSpellAction
{
public:
    CastTyphoonAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "typhoon") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class CastMoonfireAction : public CastDebuffSpellAction
{
public:
    CastMoonfireAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "moonfire", true) {}
};

// Moving, Starfire and Wrath wait for the druid to stand: Moonfire is the instant nuke.
// [rot1] Not over its own running DoT: 3.3.5 has no pandemic, a recast throws away the remaining ticks. Loken: 20.9
// Moonfire/min against the sim's 4.0 (abilities_balance_druid.md). Recast only when the own DoT is missing or has
// less than one tick (MOONFIRE_TICK_MS) left: at most one tick lost against the new direct hit and a full DoT.
// With Moonfire up, Insect Swarm ("insect swarm missing") or nothing is the moving cast.
inline constexpr int32 MOONFIRE_TICK_MS = 3 * IN_MILLISECONDS;

class CastMoonfireOnTheMoveAction : public CastSpellAction
{
public:
    CastMoonfireOnTheMoveAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "moonfire") {}

    bool isUseful() override;
};

class CastInsectSwarmAction : public CastDebuffSpellAction
{
public:
    CastInsectSwarmAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "insect swarm", true) {}
};

class CastStarfireAction : public CastSpellAction
{
public:
    CastStarfireAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "starfire") {}
    bool isUseful() override;
};

class CastEntanglingRootsAction : public CastSpellAction
{
public:
    CastEntanglingRootsAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "entangling roots") {}
};

class CastEntanglingRootsCcAction : public CastCrowdControlSpellAction
{
public:
    CastEntanglingRootsCcAction(PlayerbotAI* botAI) : CastCrowdControlSpellAction(botAI, "entangling roots") {}
    Value<Unit*>* GetTargetValue() override;
};

class CastHibernateAction : public CastSpellAction
{
public:
    CastHibernateAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "hibernate") {}
};

class CastHibernateCcAction : public CastCrowdControlSpellAction
{
public:
    CastHibernateCcAction(PlayerbotAI* botAI) : CastCrowdControlSpellAction(botAI, "hibernate") {}
    Value<Unit*>* GetTargetValue() override;
};

class CastCycloneCcAction : public CastCrowdControlSpellAction
{
public:
    CastCycloneCcAction(PlayerbotAI* botAI) : CastCrowdControlSpellAction(botAI, "cyclone") {}
    Value<Unit*>* GetTargetValue() override;
    // [rot1] in a 5-man group only before the pull (a mob not in combat yet)
    bool isUseful() override;

private:
    static constexpr uint32 LOG_MS = 10 * IN_MILLISECONDS;
    uint32 _loggedMs = 0;
};

class CastNaturesGraspAction : public CastBuffSpellAction
{
public:
    CastNaturesGraspAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "nature's grasp") {}
};

class CastCurePoisonAction : public CastCureSpellAction
{
public:
    CastCurePoisonAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "cure poison") {}
};

class CastCurePoisonOnPartyAction : public CurePartyMemberAction
{
public:
    CastCurePoisonOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "cure poison", DISPEL_POISON) {}
};

class CastAbolishPoisonAction : public CastCureSpellAction
{
public:
    CastAbolishPoisonAction(PlayerbotAI* botAI) : CastCureSpellAction(botAI, "abolish poison") {}
    std::vector<NextAction> getAlternatives() override;
};

class CastAbolishPoisonOnPartyAction : public CurePartyMemberAction
{
public:
    CastAbolishPoisonOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "abolish poison", DISPEL_POISON)
    {
    }

    std::vector<NextAction> getAlternatives() override;
};

class CastBarkskinAction : public CastBuffSpellAction
{
public:
    CastBarkskinAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "barkskin") {}
};

class CastInnervateAction : public CastSpellAction
{
public:
    CastInnervateAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "innervate") {}

    std::string const GetTargetName() override { return "self target"; }
    bool isUseful() override;  // [rot38] a led non-healer: the leader owns Innervate (its "leader external")
};

class CastInnervateOnHealerAction : public CastSpellAction
{
public:
    CastInnervateOnHealerAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "innervate") {}

    // Raid utility coordinator: lowest-mana healers are paired with the druids that have Innervate ready.
    std::string const GetTargetName() override { return "innervate target"; }
    bool isPossible() override;
    std::vector<NextAction> getPrerequisites() override;
};

class CastTranquilityAction : public CastAoeHealSpellAction
{
public:
    CastTranquilityAction(PlayerbotAI* botAI)
        : CastAoeHealSpellAction(botAI, "tranquility", 15.0f, HealingManaEfficiency::MEDIUM)
    {
    }
};

class CastNaturesSwiftnessAction : public CastBuffSpellAction
{
public:
    CastNaturesSwiftnessAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "nature's swiftness") {}
};

class CastWildGrowthOnPartyAction : public HealPartyMemberAction
{
public:
    CastWildGrowthOnPartyAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "wild growth", 15.0f, HealingManaEfficiency::VERY_HIGH)
    {
    }

    bool isUseful() override;
    // [heal_triage] Cast on the member with the most hurt players within 30 yd, not only on the lowest one.
    Unit* GetTarget() override;

private:
    ObjectGuid _center;
    uint32 _centerMs = 0;
};

class CastPartySwiftmendAction : public HealPartyMemberAction
{
public:
    CastPartySwiftmendAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "swiftmend", 15.0f, HealingManaEfficiency::MEDIUM)
    {
    }

    bool isUseful() override;
};

class CastPartyNourishAction : public HealPartyMemberAction
{
public:
    // [b31_sd] HIGH, not LOW: Nourish is the druid's cheapest direct heal (+20 % on a target with a HoT); with LOW the
    // conserve-mana strategy dropped it first (GUIDE_AUDIT_HEALERS.md, Resto Druid: 1.16/min on bosses).
    CastPartyNourishAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "nourish", 25.0f, HealingManaEfficiency::HIGH) {}
};

class CastDruidRemoveCurseOnPartyAction : public CurePartyMemberAction
{
public:
    CastDruidRemoveCurseOnPartyAction(PlayerbotAI* botAI) : CurePartyMemberAction(botAI, "remove curse", DISPEL_CURSE) {}
};

class CastInsectSwarmOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    // [b31_sd] The lifetime gate of the trigger (DRUID_MULTI_DOT_MIN_LIFETIME) holds for the queued cast too: the
    // isUseful() override that skipped CastDebuffSpellAction's gate is gone.
    CastInsectSwarmOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "insect swarm", true, DRUID_MULTI_DOT_MIN_LIFETIME) {}
};

class CastMoonfireOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastMoonfireOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "moonfire", true, DRUID_MULTI_DOT_MIN_LIFETIME) {}
};

class CastEnrageAction : public CastBuffSpellAction
{
public:
    CastEnrageAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "enrage") {}

    // [rot331] on its cooldown in combat, as wowsims' major cooldown (DruidActions.cpp)
    bool isUseful() override;
    bool Execute(Event event) override;  // [rot331] logs the tag
};

class CastRejuvenationOnNotFullAction : public HealPartyMemberAction
{
public:
    CastRejuvenationOnNotFullAction(PlayerbotAI* botAI)
        : HealPartyMemberAction(botAI, "rejuvenation", 5.0f, HealingManaEfficiency::VERY_HIGH)
    {
    }
    bool isUseful() override;
    Unit* GetTarget() override;
};

class CastForceOfNatureAction : public CastSpellAction
{
public:
    CastForceOfNatureAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "force of nature") {}
};

inline constexpr uint32 BLANKET_TARGET_CACHE_MS = 200;

class BlanketHotTargetValue : public CalculatedValue<ObjectGuid>, public Qualified
{
public:
    BlanketHotTargetValue(PlayerbotAI* botAI)
        : CalculatedValue<ObjectGuid>(botAI, "blanket hot target", BLANKET_TARGET_CACHE_MS) {}

protected:
    ObjectGuid Calculate() override;
};

class CastBlanketHotAction : public CastSpellAction
{
public:
    CastBlanketHotAction(PlayerbotAI* botAI, std::string const& spell) : CastSpellAction(botAI, spell)
    {
        range = botAI->GetRange("heal");
    }

    Unit* GetTarget() override;
    bool isUseful() override;
};

class CastRejuvenationBlanketAction : public CastBlanketHotAction
{
public:
    CastRejuvenationBlanketAction(PlayerbotAI* botAI) : CastBlanketHotAction(botAI, "rejuvenation") {}
    std::string const getName() override { return "rejuvenation blanket"; }
};

class CastWildGrowthBlanketAction : public CastBlanketHotAction
{
public:
    CastWildGrowthBlanketAction(PlayerbotAI* botAI) : CastBlanketHotAction(botAI, "wild growth") {}
    std::string const getName() override { return "wild growth blanket"; }
};

// [rot30] Whether the Lunar Eclipse proc cooldown is over (it records an active Lunar Eclipse) / the Eclipse talent
bool DruidLunarProcReady(AiObjectContext* context, Player* bot);
bool DruidHasEclipseTalent(Player* bot);

// [b31_deep] Start of the last Lunar Eclipse (time()), for its 30 s proc cooldown (CastWrathAction and
// CastStarfireAction). The Solar counterpart is gone: the fishing rule only needs the Lunar cooldown.
class EclipseLunarProcTimeValue : public ManualSetValue<time_t>
{
public:
    EclipseLunarProcTimeValue(PlayerbotAI* botAI) : ManualSetValue<time_t>(botAI, 0) {}
};

#endif
