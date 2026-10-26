/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoSMultipliers.h"
#include "Action.h"
#include "ChooseTargetActions.h"
#include "GenericSpellActions.h"
#include "HoSActions.h"
#include "HoSTriggers.h"
#include "LeaderActions.h"
#include "MovementActions.h"
#include "ReachTargetActions.h"
#include "ShamanActions.h"
#include "WlkTacticLog.h"

float KrystallusMultiplier::GetValue(Action* action)
{
    // Spirit Wolves are two more units that get Stoned and Shatter on their owner (wave 8): a shaman fighting
    // Krystallus keeps Feral Spirit for later, the way a player delays a cooldown that hurts the group here.
    if (dynamic_cast<CastFeralSpiritAction*>(action))
    {
        Unit* boss = AI_VALUE2(Unit*, "find target", "krystallus");
        if (boss && boss->IsAlive() && boss->IsInCombat())
            return 0.0f;
    }

    // A hunter's pet is dismissed for this fight (krystallus dismiss pet): it is not called or revived back before
    // Krystallus is dead. Class and name first, so other actions never pay for the instance lookup.
    if (bot->getClass() == CLASS_HUNTER)
    {
        std::string const name = action->getName();
        if ((name == "call pet" || name == "revive pet") && HoSShared::IsKrystallusAhead(bot))
            return 0.0f;
    }

    // The other short-lived summons (HoSIds::NPC_GREATER_FIRE_ELEMENTAL ...) are not summoned near a living
    // Krystallus either: krystallus dismiss summon would only put them down again. Name first, so other actions never
    // pay for the instance lookup.
    {
        std::string const name = action->getName();
        if ((name == "fire elemental totem" || name == "fire elemental totem melee" ||
             name == "summon water elemental" || name == "shadowfiend" || name == "force of nature" ||
             name == "mirror image" || name == "army of the dead" || name == "summon gargoyle" ||
             name == "raise dead") &&
            HoSShared::IsKrystallusAhead(bot))
            return 0.0f;
    }

    // Both Ground Slam auras exist only in this encounter, so no boss lookup is needed.
    if (HoSShared::IsGroundSlamActive(bot) && dynamic_cast<MovementAction*>(action) &&
        !dynamic_cast<ShatterSpreadAction*>(action))
        return 0.0f;

    return 1.0f;
}

float SjonnirMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "sjonnir the ironshaper");
    if (!boss) { return 1.0f; }

    if (HoSShared::IsLightningRing(bot, boss))
        {
            // Problematic since there's a lot of movement on this boss, will prevent players from positioning
            // well to deal with adds etc. during the channel period. Takes a bit of work to improve this though
            if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<AvoidLightningRingAction*>(action))
            {
                return 0.0f;
            }
        }
    return 1.0f;
}

// hos-h 2026-10-25: the hunter's Feign Death (wipe escape) lay in a Searing Gaze for 69 s and died in it; the escape
// hold keeps every action off, the step out of the gaze included.
float TribunalMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<WipeEscapeAction*>(action))
        return 1.0f;

    return HoSShared::NearestTribunalHazard(bot) ? 0.0f : 1.0f;
}

// Tribunal of Ages: the tank's own fight stays at its hold spot. Beyond TRIBUNAL_TANK_LEASH from it every movement
// except the tactic's (hold point, hazards, Brann's protection, the loose-add pickup near the spot) and the generic
// "avoid" steps is cut, so "reach melee" / follow cannot drag it after an add across the room (hos-h 20261025-062726).
float TribunalHoldMultiplier::GetValue(Action* action)
{
    if (!PlayerbotAI::IsTank(bot) || !dynamic_cast<MovementAction*>(action))
        return 1.0f;

    if (dynamic_cast<HoSTribunalHoldPointAction*>(action) || dynamic_cast<HoSAvoidTribunalHazardAction*>(action) ||
        dynamic_cast<HoSProtectBrannAction*>(action) || dynamic_cast<HoSTankTribunalAddAction*>(action))
        return 1.0f;

    // [b34_hos] around the tank's forward spot
    if (!HoSShared::IsTribunalActive(bot) ||
        bot->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y) <= HoSIds::TRIBUNAL_TANK_LEASH)
        return 1.0f;

    std::string const name = action->getName();
    if (name.find("avoid") != std::string::npos || name.find("flee") != std::string::npos)
        return 1.0f;

    if (WlkTacticLog::Allow(bot, "hos tribunal leash"))
        LOG_INFO("playerbots", "[hos tribunal leash] {}: {} held, {:.1f} yd from the hold spot at Brann",
                 bot->GetName(), name, bot->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y));
    return 0.0f;
}

// hos-h 20261025-115805 (Tribunal 1477-1794 s): a Searing Gaze landed at the tank's adds; "hos avoid tribunal hazard"
// stepped the tank, the healer and the hunter out to 10 yd, and the next tick "reach melee" (the tank's add) and
// "hos tribunal healer hold" (the tank) walked them back in. The gaps in the ticks show it (healer at -6.5, -6.0, -4.0,
// -3.5, -1.5 ... 0 s, tank at -9.9 ... -8.9, -7.9 ... -6.9, -3.9 s): the healer died to Searing Gaze at 1748 s, the
// hunter at 1754 s, the tank to the Protectors at 1749 s, a wipe 46 s before the Tribunal ended. A player stays out
// until the gaze is gone and lets the adds come to him. The walks cut here are the ones whose goal is a unit: when that
// unit stands in a hazard the walk ends in it.
float TribunalHazardKeepOutMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<MovementAction*>(action) || dynamic_cast<HoSAvoidTribunalHazardAction*>(action))
        return 1.0f;

    bool const healerHold = dynamic_cast<HoSTribunalHealerHoldAction*>(action) != nullptr;
    if (!healerHold && !dynamic_cast<ReachMeleeAction*>(action) && !dynamic_cast<ReachHealerAttackerAction*>(action) &&
        !dynamic_cast<ReachGroupMemberAttackerAction*>(action) && !dynamic_cast<MoveToTankAction*>(action))
        return 1.0f;

    if (!HoSShared::IsTribunalActive(bot))
        return 1.0f;

    // [b34_hos] The healer's hold walks to the hold spot itself (its trigger already refuses one with a hazard on it).
    if (healerHold)
        return 1.0f;

    Unit* goal = action->GetTarget();
    if (!goal || goal == bot ||
        !HoSShared::IsNearTribunalHazard(bot, goal->GetPositionX(), goal->GetPositionY(), goal->GetPositionZ(),
                                         HoSShared::TRIBUNAL_HAZARD_RADIUS))
        return 1.0f;

    if (WlkTacticLog::Allow(bot, "hos tribunal hazard keep out"))
        LOG_INFO("playerbots", "[hos tribunal hazard keep out] {}: {} held, {} stands in a Searing Gaze or Dark Matter",
                 bot->GetName(), action->getName(), goal->GetName());
    return 0.0f;
}

// [b33_hos] hos-h 20261026-000434 (Tribunal 1820-1970 s): the group held the spot through wave 1, then at wave 2
// (1899 s) the tank's "reach melee" to a Protector 60 yd out (it began inside the 12 yd leash, so the leash never
// cut it), the holy paladin's "reach spell" to the same Protector (59.8 yd) and the casters' "move to ranged position"
// took everybody 25-38 yd towards the spawn; "intervene on attacked group member" put the tank 26 yd out. The healer
// was Charged (5.0k) on the way and died 21 yd from the spot to 3 Protectors (12.2k melee, 1909 s), the wipe
// followed. The goal decides it, not where the bot stands: no walk to an add far from the spot, the adds come in.
float TribunalNoChaseMultiplier::GetValue(Action* action)
{
    bool const reach = dynamic_cast<ReachTargetAction*>(action) && !dynamic_cast<MoveToTankAction*>(action) &&
                       !dynamic_cast<ReachPartyMemberToHealAction*>(action);
    bool const tank = PlayerbotAI::IsTank(bot);
    bool gapCloser = false;
    if (!reach)
    {
        if (!tank || !dynamic_cast<CastSpellAction*>(action))
            return 1.0f;

        std::string const name = action->getName();
        gapCloser = name == "charge" || name == "intercept" || name.rfind("intervene", 0) == 0 ||
                    name.rfind("feral charge", 0) == 0;
        if (!gapCloser)
            return 1.0f;
    }

    if (!HoSShared::IsTribunalActive(bot))
        return 1.0f;

    Unit* goal = action->GetTarget();
    if (!goal || goal == bot)
        return 1.0f;

    // [b34_hos] The tank's leash is around its forward spot, everybody else's around the hold spot by Brann.
    float const fromSpot = (gapCloser || tank)
                               ? goal->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y)
                               : goal->GetExactDist2d(HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y);
    bool const healer = PlayerbotAI::IsHeal(bot) && !tank;
    bool held = false;
    if (gapCloser || tank)
        held = fromSpot > HoSIds::TRIBUNAL_TANK_LEASH;  // Intervene's goal is the member: it lands next to it
    else if (healer)
        held = bot->IsHostileTo(goal);  // the healer stays at the tank; a heal's own reach is not a ReachTarget here
    else
        // [b35_hos] Except a Stormcaller: it casts from range and never walks in. hos-h 20261026-024524-hos-h-5G-28:
        // they stood 45.8-46.1 yd from the spot, the three melee damage dealers' "reach melee" was held here for
        // ~50 s (1539-1592 s, "Dark Rune Stormcaller is 46.1 yd from the hold spot") while Lightning Bolts killed the
        // healer, the warrior and the paladin.
        // [b44_hos] A melee damage dealer's walk to a Stormcaller far from the spot is held while a ranged one takes
        // it (HoSShared::LeftToRanged): the walk out and back with every wave was the Enhancement shaman's 19 % of
        // "reach melee" samples (DPS_GAP_TRIBUNAL_COS.md 2.5).
        held = bot->IsHostileTo(goal) &&
               (goal->GetEntry() != HoSIds::NPC_DARK_RUNE_STORMCALLER
                    ? fromSpot > HoSIds::TRIBUNAL_DPS_REACH
                    : PlayerbotAI::IsMelee(bot) && HoSShared::LeftToRanged(bot, goal));

    if (!held)
        return 1.0f;

    if (WlkTacticLog::Allow(bot, "hos tribunal no chase walk"))
        LOG_INFO("playerbots", "[hos tribunal no chase walk] {}: {} held, {} is {:.1f} yd from the hold spot",
                 bot->GetName(), action->getName(), goal->GetName(), fromSpot);
    return 0.0f;
}
