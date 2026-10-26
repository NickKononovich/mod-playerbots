/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MgTMultipliers.h"
#include "AiObjectContext.h"
#include "AttackersValue.h"
#include "ChooseTargetActions.h"
#include "DungeonHoldFire.h"
#include "FollowActions.h"
#include "GenericSpellActions.h"
#include "MgTActions.h"
#include "MgTShared.h"
#include "MovementActions.h"
#include "Playerbots.h"
#include "PullActions.h"
#include "PullStrategy.h"
#include "ReachTargetActions.h"
#include "ShamanActions.h"
#include "TbcHazard.h"
#include "Timer.h"

float MgTCrystalFocusMultiplier::GetValue(Action* action)
{
    if (dynamic_cast<MgTKillCrystalAction*>(action))
        return 1.0f;

    bool const suppressed =
        dynamic_cast<DpsAssistAction*>(action) ||
        dynamic_cast<TankAssistAction*>(action) ||
        dynamic_cast<CastDebuffSpellOnAttackerAction*>(action) ||
        (action->getThreatType() == Action::ActionThreatType::Aoe && PlayerbotAI::IsDps(bot) &&
         !dynamic_cast<CastHealingSpellAction*>(action));

    if (!suppressed)
        return 1.0f;

    if (PlayerbotAI::IsHeal(bot))
        return 1.0f;

    if (AI_VALUE(ObjectGuid, "mgt crystal target").IsEmpty())
        return 1.0f;

    return 0.0f;
}

float MgTFocusOrderMultiplier::GetValue(Action* action)
{
    bool const picker =
        dynamic_cast<DpsAssistAction*>(action) ||
        dynamic_cast<DpsAoeAction*>(action) ||
        dynamic_cast<TankAssistAction*>(action);

    bool const drop = !picker && dynamic_cast<DropTargetAction*>(action) != nullptr;

    if (!picker && !drop)
        return 1.0f;

    if (!bot->IsInCombat())
        return 1.0f;

    // The tank keeps Delrissa as its target once "mgt delrissa focus target" gave it to it (no tank assist back
    // onto a taunt-immune lackey); every other focus order is for the damage dealers only.
    bool const delrissaTank = PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot) &&
                              MagistersTerrace::ResolveFocusOrderOwner(botAI) == MagistersTerrace::FocusOrder::Delrissa;
    if (!delrissaTank && (PlayerbotAI::IsHeal(bot) || PlayerbotAI::IsTank(bot) || !PlayerbotAI::IsDps(bot)))
        return 1.0f;

    if (!AI_VALUE(ObjectGuid, "mgt crystal target").IsEmpty())
        return 1.0f;

    ObjectGuid const order = MagistersTerrace::ResolveFocusOrder(botAI);
    if (order.IsEmpty())
        return 1.0f;

    if (!drop)
        return 0.0f;

    Unit* current = AI_VALUE(Unit*, "current target");
    if (!current || current->GetGUID() != order)
        return 1.0f;
    if (!current->IsAlive() || !AttackersValue::IsValidTarget(current, bot))
        return 1.0f;

    return 0.0f;
}

namespace
{
// [delrissa_cc_keep] The area spells that hit a crowd-controlled retinue member next to the fight, whatever the role
// or the action's threat type says (mgt-h 2026-10-25 20261025-163739-mgt-h-5A-15, Delrissa wipe: Polymorph cast 12
// times in 95 s, broken by the tank's Consecration x10 / Hammer of the Righteous x8, the shaman's Fire Nova x3 and
// Magma Totem x2, the hunter's Multi-Shot x4 and Explosive Trap; the two free melee lackeys killed six members).
bool BreaksCrowdControl(Action* action)
{
    if (action->getThreatType() == Action::ActionThreatType::Aoe)
        return true;

    static char const* const names[] = {
        "consecration", "hammer of the righteous", "avenger's shield", "holy wrath", "multi-shot", "volley",
        "explosive trap", "fire nova", "magma totem", "chain lightning", "arcane explosion", "blizzard",
        "flamestrike", "cone of cold", "frost nova", "blast wave", "dragon's breath", "thunder clap", "whirlwind",
        "cleave", "swipe", "hurricane", "rain of fire", "hellfire", "seed of corruption", "fan of knives",
        "holy nova", "typhoon", "thunderstorm", "living bomb", "starfall", "explosive shot", "divine storm"};
    std::string const& name = action->getName();
    for (char const* aoe : names)
        if (name == aoe)
            return true;
    return false;
}
}  // namespace

float MgTFocusBurstMultiplier::GetValue(Action* action)
{
    // [b44_mgt] A lackey's threat table is the script's own (RecalculateThreat every 8-10 s puts every player at
    // 1e7 x distance x armour; taunts and the tank's threat count for nothing): "[threat-hold]" read the dps at
    // 129-214 % of the tank and held its casts on a target it could not pull any further. Gundrak's Slad'ran pattern:
    // ThreatMultiplier reads (and resets) "neglect threat" for every action. Delrissa herself keeps the rule.
    if (!PlayerbotAI::IsTank(bot) && MagistersTerrace::ResolveFocusOrderOwner(botAI) ==
                                         MagistersTerrace::FocusOrder::Delrissa)
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->IsAlive() && MagistersTerrace::IsDelrissaScriptedThreat(target))
            context->GetValue<bool>("neglect threat")->Set(true);
    }

    if (dynamic_cast<CastHealingSpellAction*>(action))
        return 1.0f;

    // [delrissa_cc_keep] a retinue member held in crowd control: no area spell from anybody, the tank included
    if (MagistersTerrace::ResolveFocusOrderOwner(botAI) == MagistersTerrace::FocusOrder::Delrissa &&
        BreaksCrowdControl(action) && MagistersTerrace::CountControlledRetinue(bot) > 0)
        return 0.0f;

    if (action->getThreatType() != Action::ActionThreatType::Aoe)
        return 1.0f;

    if (PlayerbotAI::IsHeal(bot) || !PlayerbotAI::IsDps(bot))
        return 1.0f;

    switch (MagistersTerrace::ResolveFocusOrderOwner(botAI))
    {
        case MagistersTerrace::FocusOrder::Kael:
        case MagistersTerrace::FocusOrder::Delrissa:
            return 0.0f;
        default:
            return 1.0f;
    }
}

float MgTRoomLeashMultiplier::GetValue(Action* action)
{
    bool const isFlight =
        dynamic_cast<FleeAction*>(action) ||
        dynamic_cast<FleeWithPetAction*>(action) ||
        dynamic_cast<RunAwayAction*>(action);

    if (!isFlight)
        return 1.0f;

    if (bot->GetPositionX() > MagistersTerrace::SELIN_SAFE_X + MagistersTerrace::SELIN_FLEE_SLACK)
        return 1.0f;

    if (!MagistersTerrace::GetSelin(bot))
        return 1.0f;

    return 0.0f;
}

float MgTDampeningFieldMultiplier::GetValue(Action* action)
{
    bool const isCompeting =
        dynamic_cast<CombatFormationMoveAction*>(action) ||
        dynamic_cast<FollowAction*>(action) ||
        dynamic_cast<AvoidAoeAction*>(action);

    if (!isCompeting)
        return 1.0f;

    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt dampening escape");
    if (spots.empty())
        return 1.0f;

    return 0.0f;
}

// The healer spacing spot wins over following the leader in and over formation moves; reaching a member to
// heal is left alone (the spot already keeps the main tank within heal range).
float MgTDelrissaHealerHoldMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<FollowAction*>(action) && !dynamic_cast<CombatFormationMoveAction*>(action))
        return 1.0f;

    if (!PlayerbotAI::IsHeal(bot))
        return 1.0f;

    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt delrissa healer spot");
    return spots.empty() ? 1.0f : 0.0f;
}

float MgTDelrissaTremorTotemMultiplier::GetValue(Action* action)
{
    bool const isCompeting =
        dynamic_cast<CastStrengthOfEarthTotemAction*>(action) ||
        dynamic_cast<CastStoneskinTotemAction*>(action) ||
        dynamic_cast<CastEarthbindTotemAction*>(action);

    if (!isCompeting)
        return 1.0f;

    if (!AI_VALUE(bool, "mgt delrissa tremor totem"))
        return 1.0f;

    return 0.0f;
}

float MgTFlameStrikeMultiplier::GetValue(Action* action)
{
    bool const isCompeting =
        dynamic_cast<CombatFormationMoveAction*>(action) ||
        dynamic_cast<FollowAction*>(action) ||
        dynamic_cast<AvoidAoeAction*>(action);

    if (!isCompeting)
        return 1.0f;

    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt flame strike escape");
    if (spots.empty())
        return 1.0f;

    return 0.0f;
}

float MgTGravityLapseMultiplier::GetValue(Action* action)
{
    // [b44_mgt] Generic "avoid aoe" walked the members back along their trail into the spheres' spawn under Kael while
    // "mgt take lapse spot" walked them out (ping-pong; the lapse logic reads the spheres' sweep and the burns itself).
    // mgt-n 20261026-075135-mgt-n-5B-13: "[avoid aoe] Rlpalam: out of (141.9,178.0) r 5.0: back along trail to
    // (148.8,173.0)" x4 at the start of the lapse, the healer 90 -> 7 % in 3 s, dead at 1735 s.
    bool const isCompeting =
        dynamic_cast<CombatFormationMoveAction*>(action) ||
        dynamic_cast<FollowAction*>(action) ||
        dynamic_cast<AvoidAoeAction*>(action) ||
        dynamic_cast<ReachTargetAction*>(action) ||
        dynamic_cast<CastReachTargetSpellAction*>(action);

    if (!AI_VALUE(bool, "mgt gravity lapse"))
        return 1.0f;

    // [b44_mgt] Kael channels Gravity Lapse and hits nobody, the Phoenix chases whom it likes: no threat rule while it
    // lasts. 5B-13: "[threat-hold] Rlpalam holds holy shock on party on Phoenix 24674: 2779 %" - the healer's heals
    // were held by its own target's threat table while the lapse damage drained the group.
    context->GetValue<bool>("neglect threat")->Set(true);

    if (!isCompeting)
        return 1.0f;

    return 0.0f;
}

float MgTKaelUnattackableMultiplier::GetValue(Action* action)
{
    bool const suppressed =
        dynamic_cast<DpsAssistAction*>(action) ||
        dynamic_cast<TankAssistAction*>(action) ||
        dynamic_cast<CastDebuffSpellOnAttackerAction*>(action) ||
        (action->getThreatType() == Action::ActionThreatType::Aoe && PlayerbotAI::IsDps(bot) &&
         !dynamic_cast<CastHealingSpellAction*>(action));

    if (!suppressed)
        return 1.0f;

    if (PlayerbotAI::IsHeal(bot))
        return 1.0f;

    if (!AI_VALUE(bool, "mgt kael unattackable"))
        return 1.0f;

    return 0.0f;
}

float MgTPhoenixBurnMultiplier::GetValue(Action* action)
{
    bool const isCompeting =
        dynamic_cast<CombatFormationMoveAction*>(action) ||
        dynamic_cast<FollowAction*>(action) ||
        dynamic_cast<AvoidAoeAction*>(action) ||
        dynamic_cast<ReachTargetAction*>(action) ||
        dynamic_cast<CastReachTargetSpellAction*>(action);

    if (!isCompeting)
        return 1.0f;

    MagistersTerrace::PhoenixRing const ring = AI_VALUE(MagistersTerrace::PhoenixRing, "mgt phoenix ring");
    if (ring != MagistersTerrace::PhoenixRing::None && dynamic_cast<SetBehindTargetAction*>(action))
        return 0.0f;

    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt phoenix escape");
    if (!spots.empty())
        return 0.0f;

    if (ring == MagistersTerrace::PhoenixRing::Covers)
        return 0.0f;

    return 1.0f;
}

// [b41_mgt] Every walk this multiplier vetoes (the leash edge, the walk up to an engaged Selin with a pack of hers
// idle, a reach into an idle pack) and the entry order's holds stay vetoed through the group's no-damage release: a walk
// past x 216 gives an inert Selin her victim, her encounter goes IN_PROGRESS and door 188065 shuts on everybody
// outside (mgt-n 20261026-063902-mgt-n-5A-14: the tank alone inside from 365 s, hunter, tank and mage dead).
bool MgTSelinRoomMultiplier::KeepsVeto(Action* action)
{
    if (bot->GetMapId() != MagistersTerrace::MAP_MAGISTERS_TERRACE)
        return false;

    return TbcHazard::IsRepositionAction(action) || MagistersTerrace::SelinRoom::EntryHolds(bot);
}

float MgTSelinRoomMultiplier::GetValue(Action* action)
{
    // Inside an idle mob's aggro range + this (about 20 yd) an area spell (Consecration, Blizzard, Arcane Explosion:
    // 8-10 yd) may reach it.
    constexpr float aoeMargin = 1.0f;

    if (bot->GetMapId() != MagistersTerrace::MAP_MAGISTERS_TERRACE)
        return 1.0f;

    if (dynamic_cast<MgTSelinRoomLeashAction*>(action))
        return 1.0f;

    // [selin_final_entry] the whole group walks in for the final pull: no room rule holds that walk
    if (dynamic_cast<MgTReturnToRoomAction*>(action) && MagistersTerrace::SelinRoom::FinalEntryActive(bot))
        return 1.0f;

    // The whole group inside before anybody touches her (MgTShared.h, room entry order): no pull, no hit, no walk up
    // to her while a member still stands outside her door.
    if (MagistersTerrace::SelinRoom::EntryHolds(bot))
    {
        Unit* selin = botAI->GetUnit(MagistersTerrace::SelinRoom::Get(botAI).selin);
        Unit* current = AI_VALUE(Unit*, "current target");
        bool const onSelin = selin && current == selin;
        if (dynamic_cast<AggressiveTargetAction*>(action) || dynamic_cast<PullStartAction*>(action) ||
            DungeonHoldFire::IsDamageAction(bot, action, selin) ||
            (onSelin && TbcHazard::IsRepositionAction(action)))
        {
            Player* outside = nullptr;
            MagistersTerrace::SelinRoom::EntryHolds(bot, &outside);
            uint32 const now = getMSTime();
            if (!_entryLogMs || getMSTimeDiff(_entryLogMs, now) >= 10 * IN_MILLISECONDS)
            {
                _entryLogMs = now;
                LOG_INFO("playerbots",
                         "[selin entry] {} holds {} on Selin: {} still outside her room (x {:.1f} < {:.1f})",
                         bot->GetName(), action->getName(), outside ? outside->GetName() : std::string("a member"),
                         outside ? outside->GetPositionX() : 0.0f, MagistersTerrace::SELIN_SAFE_X);
            }
            return 0.0f;
        }
    }

    // While a Wretched of hers lives no chase or reach crosses the leash edge, the healer's included.
    if (TbcHazard::IsRepositionAction(action) &&
        MagistersTerrace::SelinRoom::CrossesLeash(botAI, AI_VALUE(Unit*, "current target")))
        return 0.0f;

    // The planner's pull (careful plan, lone-mob minimum, approach) waits for the room tactic's pull.
    if (dynamic_cast<AggressiveTargetAction*>(action))
        return !MagistersTerrace::SelinRoom::InRoomFight(botAI) && MagistersTerrace::SelinRoom::OwnsPull(botAI)
                   ? 0.0f
                   : 1.0f;

    bool const follow = dynamic_cast<FollowAction*>(action) || dynamic_cast<CombatFormationMoveAction*>(action);
    if (follow)
    {
        // The rest of the group waits at the rally while the tank walks to its cast spot ("mgt selin room gather").
        return !MagistersTerrace::SelinRoom::InRoomFight(botAI) && !MagistersTerrace::SelinRoom::IsPullingTank(bot) &&
                       MagistersTerrace::SelinRoom::HoldsGroup(botAI)
                   ? 0.0f
                   : 1.0f;
    }

    // The room tactic's own pull walk is planned out of every idle pack's sight.
    if (dynamic_cast<MgTSelinRoomPullAction*>(action))
        return 1.0f;

    bool const aoe = action->getThreatType() == Action::ActionThreatType::Aoe;
    bool const reposition = TbcHazard::IsRepositionAction(action) && !PlayerbotAI::IsHeal(bot);
    if (!aoe && !reposition)
        return 1.0f;

    // Fel Infusion (MgTShared.h): no area spell from the damage dealers while two Wretched are low, or they reach
    // 20 % and drink together; the focus kills them one at a time.
    constexpr float wretchedLowPct = 40.0f;
    if (aoe && bot->IsInCombat() && !PlayerbotAI::IsHeal(bot) &&
        MagistersTerrace::SelinRoom::CountLowWretched(botAI, wretchedLowPct) >= 2)
        return 0.0f;

    MagistersTerrace::SelinRoom::Snapshot const& snapshot = MagistersTerrace::SelinRoom::Get(botAI);
    if (snapshot.idle.empty())
        return 1.0f;

    // The tank's own pull: the walk to its cast spot, the opener on the pack, the way back.
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (strategy && (strategy->HasTarget() || strategy->IsPullPendingToStart() || strategy->HasPullStarted()))
        return 1.0f;

    Unit* target = AI_VALUE(Unit*, "current target");
    // Selin engaged (inert or not) while a pack of hers is idle: nobody walks to her, the room's floor is in their
    // reach everywhere (16-19 yd from both side packs).
    if (reposition && target && target->GetGUID() == snapshot.selin)
        return 0.0f;

    if (aoe)
    {
        bool const nearBot = MagistersTerrace::SelinRoom::InIdleReach(snapshot, bot->GetPositionX(),
                                                                      bot->GetPositionY(), aoeMargin);
        bool const nearTarget = target && MagistersTerrace::SelinRoom::InIdleReach(
                                              snapshot, target->GetPositionX(), target->GetPositionY(), aoeMargin);
        return nearBot || nearTarget ? 0.0f : 1.0f;
    }

    // Chasing a mob that stands in an idle pack's reach (a caster Husk still in the room, a runner) is what draws
    // the other packs in; the rally stays put and the ranged kill it from there.
    return MagistersTerrace::SelinRoom::ApproachUnsafe(botAI, target) ? 0.0f : 1.0f;
}
