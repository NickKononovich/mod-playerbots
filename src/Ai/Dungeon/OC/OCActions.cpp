/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OCActions.h"
#include "DungeonInteract.h"
#include "GossipDef.h"
#include "InstanceScript.h"
#include "LastMovementValue.h"  // [b35_occ]
#include "LastSpellCastValue.h"
#include "MotionMaster.h"  // [b35_occ]
#include "MoveSpline.h"
#include "OCTriggers.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <limits>  // [b36_occ]
#include <string>
#include <unordered_map>

namespace
{
constexpr float OCULUS_DRAKE_GIVER_SEARCH_RANGE = 150.0f;
// npc_oculus_drakegiver gossip actions (GOSSIP_ACTION_INFO_DEF = 1000): +0 opens the next page, +1..+3 hand out
// (or swap for) the giver's essence, +4 is flavour text.
constexpr uint32 OCULUS_GOSSIP_ACTION_NEXT_PAGE = 1000;
constexpr uint32 OCULUS_GOSSIP_ACTION_ESSENCE_FIRST = 1001;
constexpr uint32 OCULUS_GOSSIP_ACTION_ESSENCE_LAST = 1003;
// A drake this close above ground or water has set down: the rider steps off without a fall worth the name (fall
// damage starts at 14.57 yd).
constexpr float OCULUS_DRAKE_LANDED_HEIGHT = 5.0f;
// [b38_occ] Drake spells reach 60 yd (OccFlyDrakeAction holds 55 off Eregos).
constexpr float OCC_DRAKE_SPELL_RANGE = 55.0f;
}  // namespace

bool MountDrakeAction::isPossible() { return bot->GetMapId() == OCULUS_MAP_ID; }
bool MountDrakeAction::Execute(Event /*event*/)
{
    // A human master picks his own drake first (upstream behaviour); a SelfBot leader is assigned like any bot.
    if (Player* human = RaidBotDuty::GetHumanMaster(botAI))
        if (!human->GetVehicleBase())
            return false;

    uint32 const essence = GetDrakeEssenceForRole();
    if (Item* item = bot->GetItemByEntry(essence))
    {
        // A player gets off his ground mount before calling his drake (a mounted cast of a spell without
        // SPELL_ATTR0_ALLOW_WHILE_MOUNTED fails, see [oc-portal-dismount]); MountingDrakeMultiplier keeps "check mount
        // state" from mounting again meanwhile.
        if (bot->IsMounted())
        {
            LOG_INFO("playerbots", "[oc-essence-dismount] {}: dismounts to call its drake", bot->GetName());
            bot->Dismount();
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }
        return UseItemAuto(item);
    }

    // The giver's gossip hands out its essence and takes back a different one, as for a player.
    return RequestDrakeEssence(essence);
}

// What a group does: the tank flies the Ruby drake (Searing Wrath, Evasive Maneuvers, Martyr takes the hits), the
// healer the Emerald one (Dream Funnel heals the other drakes), the DPS the Amber one (Shock Lance, Stop Time on
// Eregos' Enraged Assault). A group without a healer puts its last DPS on the Emerald drake.
uint32 MountDrakeAction::GetDrakeEssenceForRole()
{
    if (PlayerbotAI::IsTank(bot))
        return ITEM_RUBY_ESSENCE;

    if (PlayerbotAI::IsHeal(bot))
        return ITEM_EMERALD_ESSENCE;

    int32 lastDpsSlot = -1;
    for (Player* player : botAI->GetAllPlayersInGroup())
    {
        if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
            continue;

        if (PlayerbotAI::IsHeal(player))
            return ITEM_AMBER_ESSENCE;

        if (!PlayerbotAI::IsTank(player))
            lastDpsSlot = std::max(lastDpsSlot, botAI->GetGroupSlotIndex(player));
    }

    return lastDpsSlot >= 0 && botAI->GetGroupSlotIndex(bot) == lastDpsSlot ? ITEM_EMERALD_ESSENCE
                                                                             : ITEM_AMBER_ESSENCE;
}

bool MountDrakeAction::RequestDrakeEssence(uint32 itemId)
{
    uint32 giverEntry = 0;
    switch (itemId)
    {
        case ITEM_AMBER_ESSENCE:
            giverEntry = NPC_ETERNOS;
            break;
        case ITEM_EMERALD_ESSENCE:
            giverEntry = NPC_VERDISA;
            break;
        case ITEM_RUBY_ESSENCE:
            giverEntry = NPC_BELGARISTRASZ;
            break;
        default:
            return false;
    }

    Creature* giver = bot->FindNearestCreature(giverEntry, OCULUS_DRAKE_GIVER_SEARCH_RANGE);
    if (!giver || !giver->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "oc-drake-essence",
                                       "Oculus: drake giver not offering gossip yet, waiting for the essence");
        return false;
    }

    // Essences are bound to the Oculus (item Map 578): a member who released after a wipe lost his and asks again.
    if (OCRoute::ShouldLog(bot, "drake-essence", 10 * IN_MILLISECONDS))
        LOG_INFO("playerbots", "[oc drake essence] {}: asks {} for item {} ({:.1f} yd)", bot->GetName(),
                 giver->GetName(), itemId, bot->GetExactDist(giver));

    if (!bot->IsWithinDistInMap(giver, INTERACTION_DISTANCE))
    {
        MotionMaster* mm = bot->GetMotionMaster();
        if (mm->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            mm->MovePoint(0, giver->GetPositionX(), giver->GetPositionY(), giver->GetPositionZ());
        return true;
    }

    bot->StopMoving();
    bot->SetFacingToObject(giver);

    WorldPacket gossipHello(CMSG_GOSSIP_HELLO);
    gossipHello << giver->GetGUID();
    bot->GetSession()->HandleGossipHelloOpcode(gossipHello);

    // Verdisa and Eternos list the essence on the first page, Belgaristrasz one page deeper.
    constexpr uint8 MAX_GOSSIP_PAGES = 2;
    for (uint8 page = 0; page < MAX_GOSSIP_PAGES; ++page)
    {
        GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
        int32 essenceOption = -1;
        int32 nextPageOption = -1;
        for (auto const& [listId, menuItem] : menu.GetMenuItems())
        {
            if (menuItem.OptionType >= OCULUS_GOSSIP_ACTION_ESSENCE_FIRST &&
                menuItem.OptionType <= OCULUS_GOSSIP_ACTION_ESSENCE_LAST)
                essenceOption = static_cast<int32>(listId);
            else if (menuItem.OptionType == OCULUS_GOSSIP_ACTION_NEXT_PAGE)
                nextPageOption = static_cast<int32>(listId);
        }

        int32 const choice = essenceOption >= 0 ? essenceOption : nextPageOption;
        if (choice < 0)
            break;

        WorldPacket gossipSelect(CMSG_GOSSIP_SELECT_OPTION);
        gossipSelect << giver->GetGUID() << uint32(menu.GetMenuId()) << uint32(choice);
        bot->GetSession()->HandleGossipSelectOptionOpcode(gossipSelect);

        if (essenceOption >= 0)
            break;
    }

    bot->PlayerTalkClass->SendCloseGossip();
    return true;
}

bool DismountDrakeAction::Execute(Event /*event*/)
{
    Unit* drake = bot->GetVehicleBase();
    if (!drake)
        return false;

    // The rider is put down where the drake is: a player who leaves it in mid-air falls (Oculus islands float
    // hundreds of yards above the instance floor). Set the drake down first, then step off.
    float const groundZ = drake->GetMapWaterOrGroundLevel(drake->GetPositionX(), drake->GetPositionY(),
                                                          drake->GetPositionZ());
    if (groundZ > INVALID_HEIGHT && drake->GetPositionZ() - groundZ <= OCULUS_DRAKE_LANDED_HEIGHT)
    {
        bot->ExitVehicle();
        return true;
    }

    // A person who left his drake stands on an island: land next to him (bot-led groups land through the route).
    Player* human = RaidBotDuty::GetHumanMaster(botAI);
    if (human && human->IsAlive() && !human->GetVehicleBase() && human->GetMapId() == bot->GetMapId() &&
        PlayerbotAI::HasGroundSupport(human, human->GetPositionX(), human->GetPositionY(), human->GetPositionZ()))
    {
        constexpr float LANDING_SPREAD = 5.0f;
        constexpr float LANDING_RESTEER = 3.0f;
        int32 const slot = std::max(0, botAI->GetGroupSlotIndex(bot));
        float const angle = slot * 2.0f * static_cast<float>(M_PI) / 5.0f;
        float x = human->GetPositionX() + LANDING_SPREAD * std::cos(angle);
        float y = human->GetPositionY() + LANDING_SPREAD * std::sin(angle);
        float z = human->GetPositionZ();
        if (!PlayerbotAI::HasGroundSupport(human, x, y, z))
        {
            x = human->GetPositionX();
            y = human->GetPositionY();
        }

        MotionMaster* mm = drake->GetMotionMaster();
        if (mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE && drake->movespline &&
            !drake->movespline->Finalized())
        {
            G3D::Vector3 const destination = drake->movespline->FinalDestination();
            if ((destination - G3D::Vector3(x, y, z)).length() < LANDING_RESTEER)
                return false;
        }

        drake->SetCanFly(true);  // a ridden Oculus drake flies freely for a player too (client flight state)
        mm->Clear(false);
        mm->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
        drake->SendMovementFlagUpdate();
        return true;
    }

    RaidBotDuty::LogWorkaroundOnce(bot, "oc-drake-airborne",
                                   "Oculus: drake is still in the air, staying mounted until it has landed");
    return false;
}

bool OccFlyDrakeAction::Execute(Event /*event*/)
{
    Unit* vehicleBase = bot->GetVehicleBase();
    if (!vehicleBase) { return false; }

    // [b36_occ] While Planar Anomalies are out, "avoid planar anomaly" steers the drake: the hold below (MoveIdle at the
    // boss) and the formation follow would undo the dodge.
    if (!OCRoute::GetPlanarAnomalies(vehicleBase, OCIds::PLANAR_ANOMALY_SCAN_RANGE).empty())
        return false;

    // [b38_occ] A dying drake stays low over the floor it fled to (OccDrakeAttackAction), not back at the boss.
    Position refuge;
    if (OCRoute::GetLowDrakeRefuge(bot, vehicleBase, refuge))
        return FlyTo(vehicleBase, refuge);

    MotionMaster* mm = vehicleBase->GetMotionMaster();
    Unit* boss = AI_VALUE2(Unit*, "find target", "ley-guardian eregos");

    if (boss && !boss->HasAura(SPELL_PLANAR_SHIFT))
    {
        // Handle as boss encounter instead of formation flight
        float distance = vehicleBase->GetExactDist(boss);
        float range = 55.0f;    // Drake range is 60yd

        // [b44_occ] In range is not in sight. occ-h 20261026-075135-occ-h-5G-16: the four drakes following the pilot up
        // from Urom's platform were frozen here (MoveIdle) at z 597, 5 yd under the Eregos floor (z 602), the moment
        // Eregos came within 55 yd; every drake spell failed on line of sight ("occ drake attack" returned false for
        // 30-75 s, the Emerald Drake never funnelled), the Ruby Drake tanked him alone and died at ~25 s, then the
        // others one by one. Out of sight, the drake flies up to the boss' height, 20-40 yd off him on its own side.
        if (distance <= range && !vehicleBase->IsWithinLOSInMap(boss))
        {
            constexpr float SIGHT_MIN_2D = 20.0f;
            constexpr float SIGHT_MAX_2D = 40.0f;
            constexpr float SIGHT_ABOVE_BOSS = 3.0f;
            float const angle = boss->GetAngle(vehicleBase);
            float const reach = std::min(std::max(boss->GetExactDist2d(vehicleBase), SIGHT_MIN_2D), SIGHT_MAX_2D);
            Position const spot(boss->GetPositionX() + reach * std::cos(angle),
                                boss->GetPositionY() + reach * std::sin(angle),
                                boss->GetPositionZ() + SIGHT_ABOVE_BOSS);
            if (OCRoute::ShouldLog(bot, "eregos-no-sight", 10 * IN_MILLISECONDS))
                LOG_INFO("playerbots", "[b44_occ] [oc eregos sight] {}: drake at ({:.1f},{:.1f},{:.1f}) {:.1f} yd from "
                         "Eregos without line of sight, flies to ({:.1f},{:.1f},{:.1f})", bot->GetName(),
                         vehicleBase->GetPositionX(), vehicleBase->GetPositionY(), vehicleBase->GetPositionZ(),
                         distance, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
            return FlyTo(vehicleBase, spot);
        }

        mm->Clear(false);
        if (distance > range)
        {
            mm->MoveForwards(boss, range - distance);
            vehicleBase->SendMovementFlagUpdate();
            return true;
        }

        vehicleBase->SetFacingToObject(boss);
        mm->MoveIdle();
        vehicleBase->SendMovementFlagUpdate();
        return false;
    }

    // A person on a drake leads the formation (upstream behaviour).
    if (RaidBotDuty::GetHumanMaster(botAI))
        return FollowFormationLeader(vehicleBase, RaidBotDuty::GetVehicleFormationLeader(botAI));

    Player* pilot = OCRoute::GetPilot(botAI);
    if (!pilot)
        return false;

    // Nobody steers this drake but the bot: fly the group to the next objective.
    if (pilot == bot)
        return SteerTowardsGoal(vehicleBase);

    // The pilot reached the island: wait above our own spot next to it while it waits for the group, then set down.
    OCRoute::LegTarget const leg = OCRoute::GetLegTarget(pilot);
    if (leg.valid && leg.land &&
        (OCRoute::IsAtLegTarget(pilot, leg) || (pilot->GetVehicleBase() && OCRoute::IsNearLegTarget2d(pilot, leg))))
    {
        Position spot = OCRoute::GetLandingSpot(botAI, leg);
        if (pilot->GetVehicleBase() && !OCRoute::IsGroupGatheredForLanding(pilot, leg))
            spot.Relocate(spot.GetPositionX(), spot.GetPositionY(),
                          spot.GetPositionZ() + OCRoute::LANDING_HOVER_HEIGHT);
        return FlyTo(vehicleBase, spot);
    }

    return FollowFormationLeader(vehicleBase, pilot);
}

bool OccFlyDrakeAction::FollowFormationLeader(Unit* vehicleBase, Player* leader)
{
    if (!leader || leader == bot)
        return false;

    Unit* masterVehicle = leader->GetVehicleBase();
    if (!masterVehicle)
        return false;

    if (vehicleBase->GetExactDist(masterVehicle) > 20.0f)
    {
        // 3/4 of a circle, with frontal cone 90 deg unobstructed
        float angle = botAI->GetGroupSlotIndex(bot) * (2*M_PI - M_PI_2)/5 + M_PI_2;
        vehicleBase->SetCanFly(true);  // a ridden Oculus drake flies freely for a player too (client flight state)
        vehicleBase->GetMotionMaster()->MoveFollow(masterVehicle, 15.0f, angle);
        vehicleBase->SendMovementFlagUpdate();
        _hasSteerDestination = false;
        return true;
    }
    return false;
}

bool OccFlyDrakeAction::FlyTo(Unit* vehicleBase, Position const& destination)
{
    constexpr float ARRIVED_DISTANCE = 1.5f;
    constexpr float RESTEER_DISTANCE = 5.0f;

    if (vehicleBase->GetExactDist(&destination) < ARRIVED_DISTANCE)
        return false;

    MotionMaster* mm = vehicleBase->GetMotionMaster();
    if (_hasSteerDestination && mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
        _lastSteerDestination.GetExactDist(&destination) < RESTEER_DISTANCE)
        return false;

    vehicleBase->SetCanFly(true);  // a ridden Oculus drake flies freely for a player too (client flight state)
    mm->Clear(false);
    mm->MovePoint(0, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(),
                  FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
    vehicleBase->SendMovementFlagUpdate();
    _lastSteerDestination = destination;
    _hasSteerDestination = true;
    return true;
}

bool OccFlyDrakeAction::SteerTowardsGoal(Unit* vehicleBase)
{
    constexpr float DRAKE_STAND_OFF = 40.0f;  // drake spells reach 60y
    constexpr float DRAKE_HOVER_HEIGHT = 15.0f;

    OCRoute::LegTarget const leg = OCRoute::GetLegTarget(bot);
    if (!leg.valid)
        return false;

    if (leg.land)
    {
        // Fly in above the spot and set down only with every drake of the group over the island: the others land
        // next to the pilot right after it, nobody stands alone in reach of the island's packs.
        Position const spot = OCRoute::GetLandingSpot(botAI, leg);
        Position const hover(spot.GetPositionX(), spot.GetPositionY(),
                             spot.GetPositionZ() + OCRoute::LANDING_HOVER_HEIGHT);
        if (!OCRoute::IsNearLegTarget2d(vehicleBase, leg))
            return FlyTo(vehicleBase, hover);

        std::string missing;
        if (OCRoute::IsGroupGatheredForLanding(bot, leg, &missing))
            return FlyTo(vehicleBase, spot);

        if (OCRoute::ShouldLog(bot, "landing-hover", 10 * IN_MILLISECONDS))
            LOG_INFO("playerbots", "[oc landing hover] {}: holds above ({:.1f},{:.1f},{:.1f}) for the group, away: {}",
                     bot->GetName(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), missing);
        return FlyTo(vehicleBase, hover);
    }

    Position const& target = leg.pos;
    float const distance = vehicleBase->GetExactDist2d(target.GetPositionX(), target.GetPositionY());
    if (distance <= DRAKE_STAND_OFF)
    {
        vehicleBase->SetFacingTo(vehicleBase->GetAngle(&target));
        return false;
    }

    float const ratio = (distance - DRAKE_STAND_OFF) / distance;
    Position const destination(
        vehicleBase->GetPositionX() + (target.GetPositionX() - vehicleBase->GetPositionX()) * ratio,
        vehicleBase->GetPositionY() + (target.GetPositionY() - vehicleBase->GetPositionY()) * ratio,
        target.GetPositionZ() + DRAKE_HOVER_HEIGHT);
    return FlyTo(vehicleBase, destination);
}

bool OccDrakeAttackAction::Execute(Event /*event*/)
{
    vehicleBase = bot->GetVehicleBase();
    if (!vehicleBase) { return false; }

    // [b38_occ] A drake dying in mid-air drops its rider where it is: occ-h 20261026-050341-occ-h-5H-17, the priest's
    // Emerald Drake died at z 612 over open air and he fell 113 yd ("environment: fall", 1660 s); both druids' drakes
    // died over the Eregos floor (z 602) and they lived through the fall. Below 25 % the drake first flies low over
    // the nearest floor (GetLowDrakeRefuge), then fights on from there.
    constexpr float REFUGE_ARRIVED = 4.0f;
    constexpr float REFUGE_RESTEER = 3.0f;
    Position refuge;
    if (OCRoute::GetLowDrakeRefuge(bot, vehicleBase, refuge) && vehicleBase->GetExactDist(&refuge) > REFUGE_ARRIVED)
    {
        MotionMaster* mm = vehicleBase->GetMotionMaster();
        bool const onTheWay = mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE && vehicleBase->movespline &&
                              !vehicleBase->movespline->Finalized() &&
                              (vehicleBase->movespline->FinalDestination() -
                               G3D::Vector3(refuge.GetPositionX(), refuge.GetPositionY(), refuge.GetPositionZ()))
                                      .length() < REFUGE_RESTEER;
        if (!onTheWay)
        {
            vehicleBase->SetCanFly(true);
            mm->Clear(false);
            mm->MovePoint(0, refuge.GetPositionX(), refuge.GetPositionY(), refuge.GetPositionZ(), FORCED_MOVEMENT_NONE,
                          0.0f, 0.0f, false, true);
            vehicleBase->SendMovementFlagUpdate();
        }
        return true;
    }

    // A drake spell that hits a hostile unit which cannot fly kills its rider (npc_oculus_drake::SpellHitTarget,
    // single-target spells): from the drake only the fliers are fought (Azure Ring Guardians, Eregos); a pack that
    // wakes under a landing drake is fought on foot after the dismount (occ-h 2026-10-25 115805: the tank died to
    // "environment" at 601 s, its Ruby Drake setting down on the north island with the pack awake under it).
    auto const drakeTarget = [](Unit* unit) { return unit && unit->IsAlive() && unit->CanFly(); };

    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && !drakeTarget(target))
    {
        if (OCRoute::ShouldLog(bot, "drake-grounded-target", 10 * IN_MILLISECONDS))
            LOG_INFO("playerbots", "[oc drake grounded target] {}: no drake spell at {} (cannot fly, would kill the "
                     "rider)", bot->GetName(), target->GetName());
        target = nullptr;
    }

    if (!target)
    {
        GuidVector npcs = AI_VALUE(GuidVector, "possible targets");
        for (auto& npc : npcs)
        {
            Unit* unit = botAI->GetUnit(npc);
            if (!unit || !unit->IsInCombat() || !drakeTarget(unit)) { continue; }

            target = unit;
            break;
        }
    }
    // Eregos never aggroes on sight: open on him from the drake (his spell conditions allow it).
    if (!target)
        target = OCRoute::GetPullableEregos(bot, vehicleBase);
    // Check this again to see if a target was assigned
    if (!target) { return false; }

    switch (vehicleBase->GetEntry())
    {
        case NPC_AMBER_DRAKE:
            return AmberDrakeAction(target);
        case NPC_EMERALD_DRAKE:
            return EmeraldDrakeAction(target);
        case NPC_RUBY_DRAKE:
            return RubyDrakeAction(target);
        default:
            break;
    }
    return false;
}

bool OccDrakeAttackAction::CastDrakeSpellAction(Unit* target, uint32 spellId, uint32 cooldown)
{
    if (botAI->CanCastVehicleSpell(spellId, target))
        if (botAI->CastVehicleSpell(spellId, target))
        {
            vehicleBase->AddSpellCooldown(spellId, 0, cooldown);
            return true;
        }
    return false;
}

bool OccDrakeAttackAction::AmberDrakeAction(Unit* target)
{
    Aura* shockCharges = target->GetAura(SPELL_SHOCK_CHARGE, vehicleBase->GetGUID());
    if (shockCharges && shockCharges->GetStackAmount() > 8)
    {
        // At 9 charges, better to detonate and re-channel rather than stacking the last charge due to gcd
        // If stacking Amber drakes, may need to drop this even lower as the charges stack so fast
        return CastDrakeSpellAction(target, SPELL_SHOCK_LANCE, 0);
    }

    // [b38_occ] Greater Ley-Whelps on a group drake are lanced first (occ-h 5H-17: every drake fought Eregos only, the
    // whelps killed the Ruby, Emerald and Amber Drakes at 1619-1670 s). Whelps fly: the hit never kills the rider.
    if (Creature* whelp = OCRoute::GetWhelpOnGroup(bot, vehicleBase, OCC_DRAKE_SPELL_RANGE))
        if (CastDrakeSpellAction(whelp, SPELL_SHOCK_LANCE, 0))
            return true;

    // Deal with enrage after shock charges, as Stop Time adds 5 charges and they may get wasted
    if (target->HasAura(SPELL_ENRAGED_ASSAULT) &&
        !target->HasAura(SPELL_STOP_TIME) &&
        !vehicleBase->HasSpellCooldown(SPELL_STOP_TIME))
    {
        return CastDrakeSpellAction(target, SPELL_STOP_TIME, 60000);
    }

    if (!vehicleBase->FindCurrentSpellBySpellId(SPELL_TEMPORAL_RIFT))
    {
        return CastDrakeSpellAction(target, SPELL_TEMPORAL_RIFT, 0);
    }

    return false;
}

bool OccDrakeAttackAction::EmeraldDrakeAction(Unit* target)
{
    Aura* poisonStacks = target->GetAura(SPELL_LEECHING_POISON, vehicleBase->GetGUID());
    if (!poisonStacks || (poisonStacks->GetStackAmount() < 3 ||
                         poisonStacks->GetDuration() < 4000))
    {
        return CastDrakeSpellAction(target, SPELL_LEECHING_POISON, 0);
    }

    // [b36_occ] Touch the Nightmare and Dream Funnel are paid with the Emerald Drake's own health (power type health;
    // Dream Funnel also bleeds the caster every second): the old rule cast Touch whenever the debuff was off,
    // whatever the drake had left. occ-h 20261026-034959-occ-h-5A-18: the Emerald Drake was the first to die, 26 s
    // into Eregos, and nobody healed the drakes after it. A drake below the floor feeds on Leeching Poison instead.
    constexpr float EMERALD_TOUCH_MIN_HEALTH_PCT = 70.0f;   // Touch costs 30 %
    constexpr float EMERALD_FUNNEL_MIN_HEALTH_PCT = 45.0f;
    constexpr float EMERALD_FUNNEL_TARGET_PCT = 85.0f;      // no funnel for a scratch
    if (!vehicleBase->HasSpellCooldown(SPELL_TOUCH_THE_NIGHTMARE) &&
        vehicleBase->HealthAbovePct(EMERALD_TOUCH_MIN_HEALTH_PCT) &&
        (!target->HasAura(SPELL_TOUCH_THE_NIGHTMARE) || vehicleBase->HealthAbovePct(90)))
    {
        return CastDrakeSpellAction(target, SPELL_TOUCH_THE_NIGHTMARE, 10000);
    }

    Unit* healingTarget = nullptr;
    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Unit* unit = botAI->GetUnit(member);
        if (!unit || bot->GetGUID() == member)
        {
            continue;
        }

        Unit* drake = unit->GetVehicleBase();
        if (!drake || !drake->IsAlive() || drake->GetHealthPct() >= EMERALD_FUNNEL_TARGET_PCT) { continue; }  // [b36_occ]

        if (!healingTarget || drake->GetHealthPct() < healingTarget->GetHealthPct() - 15.0f)
        {
            healingTarget = drake;
        }
    }

    Spell* currentSpell = vehicleBase->FindCurrentSpellBySpellId(SPELL_DREAM_FUNNEL);
    // [b36_occ] Below its floor the funnel would kill the healer's own drake: stop it, poison feeds the drake back.
    if (!vehicleBase->HealthAbovePct(EMERALD_FUNNEL_MIN_HEALTH_PCT))
    {
        if (currentSpell)
            vehicleBase->InterruptSpell(CURRENT_CHANNELED_SPELL);
        return CastDrakeSpellAction(target, SPELL_LEECHING_POISON, 0);
    }

    if (healingTarget)
    {
        if (!currentSpell || currentSpell->m_targets.GetUnitTarget() != healingTarget)
        {
            float distance = vehicleBase->GetExactDist(healingTarget);
            float range = 55.0f;
            if (distance > range)
            {
                MotionMaster* mm = vehicleBase->GetMotionMaster();
                mm->Clear(false);
                mm->MoveForwards(healingTarget, distance - range - 10.0f);
                vehicleBase->SendMovementFlagUpdate();
                return false;
            }
            return CastDrakeSpellAction(healingTarget, SPELL_DREAM_FUNNEL, 0);
        }
    }
    // Fill GCDs with Leeching Poison to refresh timer, rather than idling
    if (!currentSpell)
    {
        return CastDrakeSpellAction(target, SPELL_LEECHING_POISON, 0);
    }

    return false;
}

bool OccDrakeAttackAction::RubyDrakeAction(Unit* target)
{
    // [b38_occ] The old rules never fired in occ-h 5H-17 (no Martyr, no Evasive Maneuvers before the Ruby Drake died):
    // Martyr waited for Evasive Maneuvers already running with > 10 s left and 5 charges, and Evasive Maneuvers for 10
    // charges. Now: Martyr (draws the harmful spells aimed at the group's drakes onto this one, 10 s) whenever a
    // whelp is on another drake of the group and this drake can take it; Evasive Maneuvers with 5 charges while this
    // drake is attacked; Searing Wrath (5 yd splash) at the whelps on the group before Eregos.
    constexpr float RUBY_MARTYR_MIN_HEALTH_PCT = 40.0f;
    constexpr uint32 RUBY_EVASIVE_MIN_CHARGES = 5;
    Creature* whelp = OCRoute::GetWhelpOnGroup(bot, vehicleBase, OCC_DRAKE_SPELL_RANGE);
    if (whelp && whelp->GetVictim() != vehicleBase && whelp->GetVictim() != bot &&
        vehicleBase->HealthAbovePct(RUBY_MARTYR_MIN_HEALTH_PCT) && !vehicleBase->HasSpellCooldown(SPELL_MARTYR) &&
        !vehicleBase->HasAura(SPELL_MARTYR) && CastDrakeSpellAction(vehicleBase, SPELL_MARTYR, 10000))
        return true;

    Aura* evasiveCharges = vehicleBase->GetAura(SPELL_EVASIVE_CHARGES);
    if (evasiveCharges && evasiveCharges->GetStackAmount() >= RUBY_EVASIVE_MIN_CHARGES &&
        !vehicleBase->getAttackers().empty() && !vehicleBase->HasAura(SPELL_EVASIVE_MANEUVERS) &&
        !vehicleBase->HasSpellCooldown(SPELL_EVASIVE_MANEUVERS) &&
        CastDrakeSpellAction(vehicleBase, SPELL_EVASIVE_MANEUVERS, 5000))
        return true;

    return CastDrakeSpellAction(whelp ? static_cast<Unit*>(whelp) : target, SPELL_SEARING_WRATH, 0);
}

// [b36_occ]
bool AvoidPlanarAnomalyAction::Execute(Event /*event*/)
{
    constexpr float FLEE_STEPS[2] = {22.0f, 38.0f};
    constexpr uint32 FLEE_DIRECTIONS = 16;
    constexpr float FIGHT_LEASH = 80.0f;      // 2D from Eregos: back in drake range (60 yd) in a few seconds
    constexpr float LEASH_SOFT = 50.0f;       // beyond this a candidate pays for the way back
    constexpr float GROUND_CLEARANCE = 6.0f;  // never set down on (or fly into) an island
    constexpr float RESTEER_MARGIN = 3.0f;

    Unit* drake = bot->GetVehicleBase();
    if (!drake || bot->GetMapId() != OCULUS_MAP_ID)
        return false;

    std::list<Creature*> const anomalies = OCRoute::GetPlanarAnomalies(drake, OCIds::PLANAR_ANOMALY_SCAN_RANGE);
    if (anomalies.empty())
    {
        _hasFleeDestination = false;
        return false;
    }

    auto const nearestAnomaly = [&anomalies](float x, float y, float z)
    {
        float nearest = std::numeric_limits<float>::max();
        for (Creature* anomaly : anomalies)
            nearest = std::min(nearest, anomaly->GetExactDist(x, y, z));
        return nearest;
    };

    float const here = nearestAnomaly(drake->GetPositionX(), drake->GetPositionY(), drake->GetPositionZ());
    MotionMaster* mm = drake->GetMotionMaster();
    bool const fleeing = _hasFleeDestination && mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
                         drake->movespline && !drake->movespline->Finalized();

    // Still on the way to a spot that stays clear: keep going. Clear and not moving: hold here (the boss is immune).
    if (fleeing && nearestAnomaly(_fleeDestination.GetPositionX(), _fleeDestination.GetPositionY(),
                                  _fleeDestination.GetPositionZ()) >= OCIds::PLANAR_ANOMALY_SAFE_DISTANCE)
        return true;
    if (!fleeing && here >= OCIds::PLANAR_ANOMALY_SAFE_DISTANCE + RESTEER_MARGIN)
        return true;

    Creature* eregos = OCRoute::GetBoss(bot, OCULUS_DATA_EREGOS);
    Position const center = eregos && eregos->IsAlive() ? eregos->GetPosition() : OCULUS_EREGOS_POSITION;

    bool found = false;
    float bestScore = -std::numeric_limits<float>::max();
    float bestClear = 0.0f;
    Position best;
    float const z = drake->GetPositionZ();
    for (float step : FLEE_STEPS)
    {
        for (uint32 i = 0; i < FLEE_DIRECTIONS; ++i)
        {
            float const angle = i * 2.0f * static_cast<float>(M_PI) / FLEE_DIRECTIONS;
            float const x = drake->GetPositionX() + step * std::cos(angle);
            float const y = drake->GetPositionY() + step * std::sin(angle);
            float const leash = center.GetExactDist2d(x, y);
            if (leash > FIGHT_LEASH)
                continue;

            float const ground = drake->GetMapHeight(x, y, z);
            if (ground > INVALID_HEIGHT && z - ground < GROUND_CLEARANCE)
                continue;

            if (!drake->IsWithinLOS(x, y, z))
                continue;

            float const clear = nearestAnomaly(x, y, z);
            float const score = clear - 0.5f * std::max(0.0f, leash - LEASH_SOFT);
            if (score > bestScore)
            {
                bestScore = score;
                bestClear = clear;
                best.Relocate(x, y, z);
                found = true;
            }
        }
    }

    // Nothing better than here: stay put rather than fly into the anomalies.
    if (!found || bestClear <= here)
    {
        if (OCRoute::ShouldLog(bot, "planar-anomaly-stuck", 5 * IN_MILLISECONDS))
            LOG_INFO("playerbots", "[b36_occ] {}: no spot clearer of the Planar Anomalies than here ({:.1f} yd)",
                     bot->GetName(), here);
        return true;
    }

    if (OCRoute::ShouldLog(bot, "planar-anomaly-flee", 3 * IN_MILLISECONDS))
        LOG_INFO("playerbots", "[b36_occ] {}: {} flies clear of {} Planar Anomalies ({:.1f} yd) to ({:.1f},{:.1f},{:.1f}), "
                 "{:.1f} yd clear, {:.1f} yd from Eregos", bot->GetName(), drake->GetName(), anomalies.size(), here,
                 best.GetPositionX(), best.GetPositionY(), best.GetPositionZ(), bestClear, center.GetExactDist2d(best));

    drake->SetCanFly(true);  // a ridden Oculus drake flies freely for a player too (client flight state)
    mm->Clear(false);
    mm->MovePoint(0, best.GetPositionX(), best.GetPositionY(), best.GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                  false, true);
    drake->SendMovementFlagUpdate();
    _fleeDestination = best;
    _hasFleeDestination = true;
    return true;
}

bool AvoidArcaneExplosionAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "mage-lord urom");
    if (!boss) { return false; }

    Position const* closestPos = nullptr;

    for (auto& position : uromSafePositions)
    {
        if (!closestPos || bot->GetExactDist(position) < bot->GetExactDist(closestPos))
            {
                closestPos = &position;
            }
    }

    if (!closestPos) { return false; }

    return MoveNear(bot->GetMapId(), closestPos->GetPositionX(), closestPos->GetPositionY(), closestPos->GetPositionZ(), 2.0f, MovementPriority::MOVEMENT_COMBAT);
}

bool TimeBombSpreadAction::Execute(Event /*event*/)
{
    float radius = 10.0f;
    float distanceExtra = 2.0f;

    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        if (bot->GetGUID() == member)
        {
            continue;
        }

        Unit* unit = botAI->GetUnit(member);
        if (unit && bot->GetExactDist2d(unit) < radius)
        {
            return MoveAway(unit, radius + distanceExtra - bot->GetExactDist2d(unit));
        }
    }
    return false;
}

bool OcUseNexusPortalAction::Execute(Event /*event*/)
{
    constexpr float PORTAL_SEARCH_RANGE = 200.0f;
    constexpr uint32 PORTAL_RETRY_MS = 2 * IN_MILLISECONDS;
    constexpr uint32 PORTAL_STUCK_MS = 4 * IN_MILLISECONDS;
    constexpr uint32 PORTAL_FORGET_MS = 60 * IN_MILLISECONDS;

    GameObject* portal = bot->FindNearestGameObject(OCIds::GO_NEXUS_PORTAL, PORTAL_SEARCH_RANGE, true);
    if (!portal)
    {
        if (OCRoute::ShouldLog(bot, "portal-missing", 10 * IN_MILLISECONDS))
            LOG_INFO("playerbots", "[oc nexus portal] {}: no spawned Nexus Portal within {:.0f} yd", bot->GetName(),
                     PORTAL_SEARCH_RANGE);
        return false;
    }

    if (!portal->IsWithinDistInMap(bot, portal->GetInteractionDistance() - 1.0f))
    {
        // [b35_occ] The pilot's walk leads the group (DungeonInteract::GroupLaggers, as [b32_pull]): it waits for the
        // laggers and a healer out of sight instead of walking ahead alone. occ-h 20261026-024524-occ-h-5D-18: the
        // bear tank walked (mounted) from the zone-in at once while the others answered the run-start ready check
        // and the warlock drank; at 9 s it stood 115 yd ahead at (1170.7,1012.6), an Azure Ley-Whelp took it with
        // the healer 108 yd behind out of sight ("pull_without_healer", "[sight-fail]"), and the whelps killed the
        // tank, the shaman and the DK at 52-60 s (death burst). The walk to the portal is one spline from the
        // zone-in, so it is stopped here. A member that never comes holds it at most PORTAL_GROUP_WAIT_MAX_MS.
        constexpr float PORTAL_GROUP_LEAD_RADIUS = 20.0f;
        constexpr uint32 PORTAL_GROUP_WAIT_MAX_MS = 60 * IN_MILLISECONDS;
        thread_local std::unordered_map<uint64, uint32> groupWaitSince;
        uint32& waitSince = groupWaitSince[bot->GetGUID().GetRawValue()];
        std::string const laggers = DungeonInteract::GroupLaggers(bot, portal->GetPositionX(),
                                                                  portal->GetPositionY(), portal->GetPositionZ(),
                                                                  PORTAL_GROUP_LEAD_RADIUS);
        if (laggers.empty())
            waitSince = 0;
        else
        {
            uint32 const nowMs = getMSTime();
            if (!waitSince)
                waitSince = nowMs ? nowMs : 1;
            if (getMSTimeDiff(waitSince, nowMs) < PORTAL_GROUP_WAIT_MAX_MS)
            {
                if (OCRoute::IsWalkingToNexusPortal(bot))
                {
                    bot->StopMoving();
                    bot->GetMotionMaster()->Clear();
                    AI_VALUE(LastMovement&, "last movement").clear();
                }
                if (OCRoute::ShouldLog(bot, "portal-group-wait", 10 * IN_MILLISECONDS))
                    LOG_INFO("playerbots", "[b35_occ] {}: waits for {} before the walk to the Nexus Portal ({:.1f} yd)",
                             bot->GetName(), laggers, bot->GetExactDist(portal));
                return false;
            }
        }

        // After a wipe the group revives at the zone-in, 120 yd off and out of the portal's sight: MoveNear only
        // takes points in line of sight (every run stood there until the harness walked the leader over), so the
        // island's path is walked to the portal itself.
        if (MoveNear(portal, 2.0f))
            return true;

        if (OCRoute::ShouldLog(bot, "portal-walk", 10 * IN_MILLISECONDS))
            LOG_INFO("playerbots", "[oc nexus portal walk] {}: walks to the portal from {:.1f} yd (out of sight)",
                     bot->GetName(), bot->GetExactDist(portal));
        return MoveTo(portal->GetMapId(), portal->GetPositionX(), portal->GetPositionY(), portal->GetPositionZ());
    }

    // A click answered by no teleport (seen after every wipe so far, cause not pinned down) is clicked again, like a
    // player would, and logged with the bot's state.
    thread_local std::unordered_map<uint64, uint32> lastUse;
    uint32& used = lastUse[bot->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    uint32 const sinceUse = used ? getMSTimeDiff(used, now) : PORTAL_FORGET_MS;
    if (sinceUse < PORTAL_RETRY_MS)
        return false;

    if (sinceUse >= PORTAL_STUCK_MS && sinceUse < PORTAL_FORGET_MS &&
        OCRoute::ShouldLog(bot, "portal-retry", 10 * IN_MILLISECONDS))
        LOG_INFO("playerbots",
                 "[oc nexus portal retry] {}: still on the entrance island {} ms after a click (casting {}, mover {}, "
                 "vehicle {}, mounted {}, go flags {}, spawned {})",
                 bot->GetName(), sinceUse, bot->IsNonMeleeSpellCast(false, false, true) ? "yes" : "no",
                 static_cast<Unit*>(bot->m_mover) == bot ? "self" : "other", bot->GetVehicleBase() ? "yes" : "no",
                 bot->IsMounted() ? "yes" : "no", static_cast<uint32>(portal->GetGameObjectFlags()),
                 portal->isSpawned() ? "yes" : "no");

    // [oc-portal-dismount] The portal casts Teleport to Boss 1 (49305) as the clicker, and that spell is no
    // mounted-allowed one (Spell::CheckCast: SPELL_FAILED_NOT_MOUNTED). After every wipe the tank walks the 120 yd
    // from the zone-in under "check mount state" (relevance 100 at the portal, every tick) and its clicks went
    // unanswered until the 4 min stall (occ-h 2026-10-25 132016 from 734 s, 115805 from 632 s), while the very first
    // click of a run, unmounted, always worked. A player gets off his mount to use it; done in the same tick, before
    // "check mount state" can mount again (OccPortalMountMultiplier also keeps it off the last 30 yd).
    if (bot->IsMounted())
    {
        LOG_INFO("playerbots", "[oc-portal-dismount] {}: dismounts to use the Nexus Portal", bot->GetName());
        bot->Dismount();
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }

    bot->StopMoving();
    // A sharpening stone or oil still being applied (the bots start them between pulls) is cancelled for the click.
    if (bot->IsNonMeleeSpellCast(false, false, true))
        bot->InterruptNonMeleeSpells(false);

    used = now ? now : 1;
    return DungeonInteract::UseGameObject(bot, portal);
}

bool OcPullBossAction::Execute(Event /*event*/)
{
    Creature* boss = OCRoute::GetPullableBoss(bot);
    if (!boss)
        return false;

    // [oc-pull-boss-dismount] A mounted player cannot attack (Unit::Attack returns false), and this action outranks
    // "check mount state" (54): occ-h wave 23 (112011) stood mounted 3.6 yd from Drakos for 174 s, his victim empty,
    // until the harness skipped him "not pulled".
    if (bot->IsMounted())
    {
        LOG_INFO("playerbots", "[oc-pull-boss-dismount] {}: dismounts to pull {}", bot->GetName(), boss->GetName());
        bot->Dismount();
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        return true;
    }

    // Open in melee: on Urom's outer platforms UromMultiplier blocks every other action aimed at him.
    if (!bot->IsWithinMeleeRange(boss))
        return MoveNear(boss, 1.0f);

    // A target set by somebody else (the harness' body pull, a tank target pick) is no attack yet: the tank stood on
    // Drakos with him as its target for 2 minutes (occ-h-5A-48 build 14). Swing unless already swinging at him.
    if (AI_VALUE(Unit*, "current target") == boss && bot->GetVictim() == boss)
        return false;
    return Attack(boss);
}

bool AvoidEnergizeCoresAction::Execute(Event /*event*/)
{
    constexpr float MIN_DISTANCE = 3.5f;  // inside Varos' boundary radius the cone does not matter
    Creature* varos = OCRoute::GetBoss(bot, OCULUS_DATA_VAROS);
    if (!varos || !varos->IsAlive())
        return false;

    float const angle = OCRoute::GetEnergizeCoresSafeAngle(varos, bot);
    float const distance = std::max(MIN_DISTANCE, bot->GetExactDist2d(varos));
    float const x = varos->GetPositionX() + distance * std::cos(angle);
    float const y = varos->GetPositionY() + distance * std::sin(angle);
    return MoveTo(bot->GetMapId(), x, y, varos->GetPositionZ(), false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT);
}

bool OcHoldLandingSpotAction::Execute(Event /*event*/)
{
    Position spot;
    if (!OculusShouldHoldLanding(botAI, &spot))
        return false;

    if (OCRoute::ShouldLog(bot, "landing-hold", 10 * IN_MILLISECONDS))
        LOG_INFO("playerbots", "[oc landing hold] {}: back to its landing spot ({:.1f},{:.1f},{:.1f}), {:.1f} yd, the "
                 "group is not down yet", bot->GetName(), spot.GetPositionX(), spot.GetPositionY(),
                 spot.GetPositionZ(), bot->GetExactDist2d(spot));
    return MoveTo(bot->GetMapId(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
}
