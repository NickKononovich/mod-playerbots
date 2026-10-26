/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MgTTriggers.h"
#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "Group.h"
#include "MgTShared.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "RaidLeaderState.h"
#include "RtiTargetValue.h"
#include "SelfTravelActions.h"

bool MgTCrystalActiveTrigger::IsActive()
{
    // Healers keep healing through the drain: Fel Explosion keeps pulsing while the damage dealers kill the crystal.
    if (PlayerbotAI::IsHeal(bot))
        return false;

    return !AI_VALUE(ObjectGuid, "mgt crystal target").IsEmpty();
}

bool MgTDelrissaRetinueLeftTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || !PlayerbotAI::IsTank(bot) ||
        bot->GetMapId() != MagistersTerrace::MAP_MAGISTERS_TERRACE)
        return false;

    // the main tank pulls (or a tank when the group has no main tank alive)
    if (!MagistersTerrace::SelinRoom::IsPullingTank(bot))
        return false;

    if (!MagistersTerrace::DelrissaRetinueLeft(bot))
        return false;

    // the group alive, topped up and with the tank (a lackey is a hard single pull)
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
                continue;
            if (!member->IsAlive() || member->GetHealthPct() < 80.0f || member->IsInCombat() ||
                bot->GetExactDist(member) > 40.0f)
                return false;
            if (PlayerbotAI::IsHeal(member) && member->getPowerType() == POWER_MANA &&
                member->GetPowerPct(POWER_MANA) < 70.0f)
                return false;
        }
    return true;
}

bool MgTOutOfRoomTrigger::IsActive()
{
    if (bot->GetPositionX() >= MagistersTerrace::SELIN_SAFE_X)
        return false;

    // [selin_final_entry] the final pull: the whole group walks in together, now
    if (bot->IsAlive() && MagistersTerrace::SelinRoom::FinalEntryActive(bot))
        return true;

    // Her room trash pulled her in (same faction, within the 10 yd call for assistance): the Wretched are fought
    // outside her reach first (wave 10/11: the walk into the room with the trash alive wiped every attempt). With
    // nobody in the room she cannot attack and evades; she is pulled again once the trash is dead.
    // [selin_ac_final_pull] Her fight committed (the final pull: no Wretched idle): in now, before the door shuts -
    // the pair is fought inside.
    if (MagistersTerrace::WretchedInFight(botAI) && !MagistersTerrace::SelinRoom::Get(botAI).selinCommitted)
        return false;

    // The walk into her room with a pack of hers still idle brings that pack (the door is 17-18 yd from the pair, the
    // room's middle 16-18 yd from both side packs): the group stays out, she evades, and the room tactic pulls the
    // rest first (MagistersTerrace::SelinRoom).
    if (!MagistersTerrace::SelinRoom::Get(botAI).idle.empty())
        return false;

    // A Wretched of hers still alive anywhere on her floor (fought at the rally, chasing somebody): the room is not
    // clear and nobody walks in yet ("mgt selin room leash").
    if (MagistersTerrace::SelinRoom::ClearingRoom(botAI))
        return false;

    // [b30_stalls] Her room clear and Selin idle with a member still outside (EntryHolds: nobody pulls her until every
    // member stands at x >= SELIN_SAFE_X): the walk in to the entry spot (MgTReturnToRoomAction). GetSelin below is
    // the threat list ("find target"), empty out of combat, so after a wipe (her room already cleared, no final entry
    // left to start) nobody ever walked in; the harness yields the steer to this walk ([selin_room_owns_steer]) and the
    // tank held its pull: mgt-n build 30 (20261025-184229-mgt-n-5B-20, 618-855 s) stood at x 187-189 until the stall
    // watchdog skipped her and the route behind gate 187979 stayed shut.
    if (bot->IsAlive() && MagistersTerrace::SelinRoom::EntryHolds(bot))
        return true;

    return MagistersTerrace::GetSelin(bot) != nullptr;
}

bool MgTSelinRoomLeashTrigger::IsActive()
{
    using namespace MagistersTerrace::SelinRoom;

    if (!bot->IsAlive() || bot->GetMapId() != MagistersTerrace::MAP_MAGISTERS_TERRACE)
        return false;

    // Once her fight is on the leash lets go (ClearingRoom: Snapshot::selinCommitted).
    if (bot->GetPositionX() <= LEASH_EDGE_X)
        return false;

    // [selin_final_entry] the group walks in for the final pull: no leash
    if (FinalEntryActive(bot))
        return false;

    // [selin_ac_final_pull] The pulling tank on its way to / at a side pack's cast spot inside the room (Selin idle on
    // her Z-gated dais): the spot is the room tactic's own, out of every other pack's reach.
    if (IsPullingTank(bot) && !Get(botAI).selinEngaged && !bot->IsInCombat())
    {
        PullPlan plan;
        if (GetPullPlan(botAI, plan) && !plan.linked && plan.x > LEASH_EDGE_X)
            return false;
    }

    return ClearingRoom(botAI);
}

bool MgTInDampeningFieldTrigger::IsActive()
{
    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt dampening escape");
    return !spots.empty();
}

bool MgTMageGuardAtRangeTrigger::IsActive()
{
    return !AI_VALUE(ObjectGuid, "mgt mage guard target").IsEmpty();
}

bool MgTArcaneNovaRangeTrigger::IsActive()
{
    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt nova escape");
    return !spots.empty();
}

bool MgTPriorityInterruptTrigger::IsActive()
{
    return !AI_VALUE(ObjectGuid, "mgt interrupt target").IsEmpty();
}

bool MgTFocusTargetTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    if (!PlayerbotAI::IsDps(bot) || PlayerbotAI::IsTank(bot))
        return false;

    if (!AI_VALUE(ObjectGuid, "mgt crystal target").IsEmpty())
        return false;

    ObjectGuid const order = AI_VALUE(ObjectGuid, "mgt focus target");
    return !order.IsEmpty() && MagistersTerrace::ResolveFocusOrder(botAI) == order;
}

bool MgTEnragedWretchedTrigger::IsActive()
{
    return !AI_VALUE(ObjectGuid, "mgt enraged wretched").IsEmpty();
}

bool MgTDelrissaInterruptTrigger::IsActive()
{
    auto const& order = AI_VALUE_REF(GuidVector, "mgt delrissa interrupt order");
    return !order.empty();
}

bool MgTDelrissaFocusTargetTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    // The tank too: the retinue is taunt immune, so the tank holds nothing by hitting a lackey, and its target is
    // the "main tank target" the group's crowd control refuses (mgt-h 2026-10-25 wipe: Freezing Trap refused on
    // Kagani, the tank's target; Delrissa still at 65 % when the last dps died).
    if (PlayerbotAI::IsHeal(bot) || (!PlayerbotAI::IsDps(bot) && !PlayerbotAI::IsTank(bot)))
        return false;

    ObjectGuid const order = AI_VALUE(ObjectGuid, "mgt delrissa focus target");
    return !order.IsEmpty() && MagistersTerrace::ResolveFocusOrder(botAI) == order;
}

bool MgTDelrissaHealerSpotTrigger::IsActive()
{
    if (!PlayerbotAI::IsHeal(bot))
        return false;

    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt delrissa healer spot");
    constexpr float arrived = 1.5f;
    return !spots.empty() && bot->GetExactDist2d(spots.front().x, spots.front().y) > arrived;
}

bool MgTDelrissaCcMarkTrigger::IsActive()
{
    if (bot->GetMapId() != MagistersTerrace::MAP_MAGISTERS_TERRACE || !bot->IsInCombat() || !bot->GetGroup())
        return false;

    // A crowd control icon hides its mob from the dps, so only a bot that will cast the CC may place one.
    // [b34_mgt] The cast is "cast auto cc" (the "auto cc marks" strategy, every class with a crowd control role), not
    // the class "cc" strategy: no shaman has one, so the Hex added for the shaman never marked (mgt-h 2026-10-26
    // 20261026-014521-mgt-h-5B-16: no [delrissa cc] line, Kagani free all fight, four deaths).
    if (MagistersTerrace::DelrissaCcSpell(bot).empty() || !botAI->HasStrategy("auto cc marks", BOT_STATE_COMBAT))
        return false;

    int32 const index = RtiTargetValue::GetRtiIndex(AI_VALUE(std::string, "rti cc"));
    if (index < 0 || index == RtiTargetValue::skullIndex || index == RtiTargetValue::crossIndex)
        return false;

    if (ObjectGuid const current = bot->GetGroup()->GetTargetIcon(static_cast<uint8>(index)))
    {
        Unit* unit = botAI->GetUnit(current);
        // [b36_mgt] its own Hex / Polymorph mark not held yet (or freed): the mark action casts it again itself.
        // mgt-h 20261026-034959-mgt-h-5B-15: the one cast try at the marking found Warlord Salaris 36-45 yd
        // away on the healer, out of sight; "cast auto cc" then held the Hex for good ("within a tank's or a
        // melee's area damage": the tank's futile peels on him), and Salaris killed the rogue and the shaman.
        if (unit && unit->IsAlive())
            return MagistersTerrace::DelrissaOwnUnheldMark(bot) == unit;
    }

    return MagistersTerrace::DelrissaRetinueEngaged(bot) &&
           MagistersTerrace::CountControlledRetinue(bot) < MagistersTerrace::DELRISSA_MAX_CROWD_CONTROL;
}

bool MgTDelrissaTremorTotemTrigger::IsActive()
{
    if (!AI_VALUE(bool, "mgt delrissa tremor totem"))
        return false;

    return !AI_VALUE2(bool, "has totem", "tremor totem");
}

bool MgTFlameStrikeTrigger::IsActive()
{
    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt flame strike escape");
    return !spots.empty();
}

bool MgTPhoenixBurnTrigger::IsActive()
{
    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt phoenix escape");
    return !spots.empty();
}

bool MgTKaelFocusTargetTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    if (!PlayerbotAI::IsDps(bot) || PlayerbotAI::IsTank(bot))
        return false;

    return !AI_VALUE(ObjectGuid, "mgt kael focus target").IsEmpty();
}

bool MgTKaelInterruptTrigger::IsActive()
{
    return !AI_VALUE(ObjectGuid, "mgt kael interrupt target").IsEmpty();
}

bool MgTGravityLapseTrigger::IsActive()
{
    return AI_VALUE(bool, "mgt gravity lapse");
}

bool MgTSelinRoomPullTrigger::IsActive()
{
    if (MagistersTerrace::SelinRoom::InRoomFight(botAI) || !MagistersTerrace::SelinRoom::IsPullingTank(bot) ||
        !MagistersTerrace::SelinRoom::OwnsPull(botAI))
        return false;

    // The group's coordinator holds every new pull (ready check, buffs, a break): so does this one.
    if (RaidLeader::IsEnabled() && AI_VALUE(bool, "pull hold"))
        return false;

    // The pull goes through the tank's pull strategy; not while a pull of its own is already under way.
    PullStrategy* strategy = PullStrategy::Get(botAI);
    return strategy && !strategy->HasTarget() && !strategy->IsPullPendingToStart() && !strategy->HasPullStarted();
}

bool MgTSelinRoomPullInertTrigger::IsActive()
{
    return bot->IsInCombat() && MagistersTerrace::SelinRoom::Get(botAI).selinInert &&
           MgTSelinRoomPullTrigger::IsActive();
}

bool MgTSelinPetOutTrigger::IsActive()
{
    return bot->IsAlive() && !bot->m_Controlled.empty() &&
           MagistersTerrace::SelinRoom::PetToCallBack(botAI) != nullptr;
}

namespace
{
// The group's pulling tank has a planner guarantee pull under way (SelinRoom::OwnsPull asked for it): the room's
// gather, drift and rally keep off, or they walk the tank and the group back from the pull.
bool SelinGuaranteeActive(Player* bot)
{
    ObjectGuid mob;
    bool walkIn = false;
    if (MagistersTerrace::SelinRoom::IsPullingTank(bot))
        return GuaranteeRequest::Get(bot->GetGUID(), mob, walkIn);

    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource();
                member && member != bot && MagistersTerrace::SelinRoom::IsPullingTank(member))
                return GuaranteeRequest::Get(member->GetGUID(), mob, walkIn);
    return false;
}
}  // namespace

bool MgTSelinRoomGatherTrigger::IsActive()
{
    if (!bot->IsAlive() || SelinGuaranteeActive(bot) || MagistersTerrace::SelinRoom::FinalEntryActive(bot) ||
        MagistersTerrace::SelinRoom::InRoomFight(botAI) ||
        MagistersTerrace::SelinRoom::IsPullingTank(bot) ||
        !MagistersTerrace::SelinRoom::HoldsGroup(botAI))
        return false;

    // [selin_rally_yields_portal_walk] A bot on its own portal walk (the harness regroup's "group comes to the
    // entrance", a wing change) is not called back to the rally: mgt-h 2026-10-25 (20261025-115805-mgt-h-5A-40,
    // 708-884 s) the healer flipped between "mgt selin room to rally" and "move to instance portal" (both 34) for
    // 3 minutes at x 171-190 and never reached the entrance.
    if (SelfTravel::PortalDestination(botAI) != 0)
        return false;

    // the stage spot before the final pull ([selin_ac_final_pull]), else the rally
    float x = MagistersTerrace::SelinRoom::RALLY_X;
    float y = MagistersTerrace::SelinRoom::RALLY_Y;
    float z = MagistersTerrace::SelinRoom::RALLY_Z;
    MagistersTerrace::SelinRoom::GatherPoint(botAI, x, y, z);
    return bot->GetExactDist2d(x, y) > MagistersTerrace::SelinRoom::GATHER_STAND;
}

bool MgTSelinRoomDriftTrigger::IsActive()
{
    if (!bot->IsAlive() || SelinGuaranteeActive(bot) || MagistersTerrace::SelinRoom::FinalEntryActive(bot))
        return false;

    // Whether Selin is idle or not (the pair brings her for a moment): an idle pack stays out of the fight.
    MagistersTerrace::SelinRoom::Snapshot const& snapshot = MagistersTerrace::SelinRoom::Get(botAI);
    if (snapshot.idle.empty())
        return false;

    // The tank walking to its cast spot may come into the reach of the pack it pulls (the spot keeps the others').
    int16 ignorePack = -1;
    MagistersTerrace::SelinRoom::PullPlan plan;
    if (!MagistersTerrace::SelinRoom::InRoomFight(botAI) && MagistersTerrace::SelinRoom::GetPullPlan(botAI, plan))
        ignorePack = MagistersTerrace::SelinRoom::PackOf(snapshot, plan.target);

    // Creature::CanStartAttack: an idle mob in sight within its aggro range + DRIFT_MARGIN (Snapshot::reachPacks).
    uint32 reach = snapshot.reachPacks;
    if (ignorePack >= 0 && ignorePack < 32)
        reach &= ~(1u << ignorePack);

    return reach != 0;
}

bool MgTSelinRoomRallyTrigger::IsActive()
{
    // The pack comes to the tank: a few yards of slack around the rally.
    constexpr float rallySlack = 5.0f;
    // The doorway counts as her room (the door is at x 215).
    constexpr float roomEdgeSlack = 2.0f;

    // [selin_final_entry] the group walks in together: no walk back to the rally
    if (!bot->IsAlive() || !bot->IsInCombat() || SelinGuaranteeActive(bot) ||
        MagistersTerrace::SelinRoom::FinalEntryActive(bot))
        return false;

    MagistersTerrace::SelinRoom::Snapshot const& snapshot = MagistersTerrace::SelinRoom::Get(botAI);
    if (snapshot.idle.empty())
        return false;

    // The tank holds the pulled pack at the rally while another pack of hers is idle.
    bool const holdPack = PlayerbotAI::IsTank(bot) && MagistersTerrace::WretchedInFight(botAI);
    // Selin engaged (the pair brings her) while a pack of hers is idle: nobody stays in her room, where she can hit
    // (x > 216); with nobody to hit she evades.
    bool const leaveRoom =
        snapshot.selinEngaged && bot->GetPositionX() > MagistersTerrace::SELIN_LEASH_X - roomEdgeSlack;
    if (!holdPack && !leaveRoom)
        return false;

    return bot->GetExactDist2d(MagistersTerrace::SelinRoom::RALLY_X, MagistersTerrace::SelinRoom::RALLY_Y) >
           rallySlack;
}

bool MgTWretchedInfusionTrigger::IsActive()
{
    // Four Wretched or more: a whole side pack is in the fight.
    constexpr uint32 hasteAt = 4;

    if (!bot->IsInCombat() || !MagistersTerrace::WretchedInFight(botAI))
        return false;

    if (bot->getClass() == CLASS_SHAMAN)
        return MagistersTerrace::SelinRoom::CountLowWretched(botAI, 100.0f) >= hasteAt;

    if (!PlayerbotAI::IsTank(bot))
        return false;

    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* mob = botAI->GetUnit(guid);
        if (mob && mob->IsAlive() && mob->GetVictim() == bot && MagistersTerrace::IsDrinkingWretched(mob))
            return true;
    }
    return false;
}
