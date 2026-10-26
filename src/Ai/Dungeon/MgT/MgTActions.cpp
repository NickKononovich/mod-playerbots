/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MgTActions.h"
#include "AiObjectContext.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "EncounterHelpers.h"
#include "GameObject.h"  // [b31_dg]
#include "Group.h"
#include "InstanceScript.h"  // [b31_dg]
#include "KillOrder.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PetDefines.h"
#include "MgTShared.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "PullStrategy.h"
#include "RaidLeaderState.h"
#include "PlayerbotAIConfig.h"
#include "RaidMarkOwnership.h"
#include "RtiTargetValue.h"
#include "RtiValue.h"
#include "SpellInfo.h"  // [b36_mgt]
#include "SpellMgr.h"
#include "Timer.h"
#include <limits>
#include <algorithm>
#include <cmath>
#include <mutex>  // [b35_mgt]
#include <string>
#include <unordered_map>  // [b35_mgt]
#include <utility>
#include <vector>

using namespace EncounterHelpers;

bool MgTEscapeAction::Execute(Event)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
        return false;

    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, _value);
    for (MagistersTerrace::EscapeSpot const& spot : spots)
    {
        if (MoveTo(bot->GetMapId(), spot.x, spot.y, spot.z, false, false, false, false,
                   MovementPriority::MOVEMENT_COMBAT, true, false))
            return true;
    }

    return false;
}

bool MgTFocusAction::Execute(Event)
{
    Unit* target = botAI->GetUnit(AI_VALUE(ObjectGuid, _value));
    if (!target)
        return false;

    if (IsMechanicTrackerBot(bot, MagistersTerrace::MAP_MAGISTERS_TERRACE))
        MarkTargetWithSkull(bot, target);

    if (AI_VALUE(Unit*, "current target") == target)
        return false;

    return Attack(target);
}

bool MgTKillCrystalAction::Execute(Event)
{
    Unit* crystal = botAI->GetUnit(AI_VALUE(ObjectGuid, "mgt crystal target"));
    if (!crystal || AI_VALUE(Unit*, "current target") == crystal)
        return false;

    return Attack(crystal);
}

bool MgTReturnToRoomAction::SkipsIdleGuard()
{
    return MagistersTerrace::SelinRoom::FinalEntryActive(bot) ||
           MagistersTerrace::SelinRoom::Get(botAI).selinCommitted;
}

bool MgTReturnToRoomAction::Execute(Event)
{
    float x = MagistersTerrace::SELIN_REGROUP_X;
    float y = bot->GetPositionY();
    MagistersTerrace::ClampIntoRoom(x, y);
    // [selin_entry_spot] Her room clear and Selin idle (EntryHolds: nobody pulls her before every member stands at
    // x >= SELIN_SAFE_X): the walk in ends just past that line, off her axis, outside her aggro (21 yd + margin from
    // (242.1, 0.3)). Build 29 (20261025-163739-mgt-n-5A-37, 457-702 s): the walk to SELIN_REGROUP_X (232) ran into
    // her aggro, the idle-mob path guard cut it to a stop at x 212-218 every time, nobody reached x 220 and the
    // entry held for 4 minutes (stall).
    // [selin_final_entry] the group walks in together: to the entry's fight spot side, just past SELIN_SAFE_X
    if (float entryY = 0.0f; MagistersTerrace::SelinRoom::FinalEntryActive(bot, &entryY))
    {
        x = MagistersTerrace::SelinRoom::ENTRY_FIGHT_X;
        y = entryY;
    }
    else if (MagistersTerrace::SelinRoom::EntryHolds(bot))
    {
        constexpr float ENTRY_SPOT_INSIDE = 1.0f;  // past SELIN_SAFE_X
        constexpr float ENTRY_SPOT_SIDE_Y = 9.0f;  // off her axis: 21.9 + yd from her at x 221
        x = MagistersTerrace::SELIN_SAFE_X + ENTRY_SPOT_INSIDE;
        y = bot->GetPositionY() >= 0.0f ? ENTRY_SPOT_SIDE_Y : -ENTRY_SPOT_SIDE_Y;
    }

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    float const bz = bot->GetPositionZ();

    float const dist = bot->GetExactDist2d(x, y);
    if (dist < 1.0f)
        return false;

    float const step = std::min(8.0f, dist);
    float const moveX = bx + ((x - bx) / dist) * step;
    float const moveY = by + ((y - by) / dist) * step;

    float moveZ = bot->GetMapHeight(moveX, moveY, bz + MagistersTerrace::GROUND_SEARCH_UP, true,
                                    MagistersTerrace::GROUND_SEARCH_DOWN);
    if (std::fabs(moveZ - bz) > MagistersTerrace::GROUND_TIER_STEP)
        moveZ = bz;

    if (MoveTo(bot->GetMapId(), moveX, moveY, moveZ, false, false, false, false, MovementPriority::MOVEMENT_COMBAT,
               true, false))
        return true;

    // [b31_dg] never a silent false: mgt-n 20261025-184229-mgt-n-5B-20 (359-450 s) returned false every tick for 90 s
    // with no word why - her room door 188065 (DOOR_TYPE_ROOM) had shut on IN_PROGRESS with the healer outside, and
    // the closed-door guard cut every walk to "stay". Says the door state (and that a bot outside cannot get in).
    constexpr uint32 RETURN_FAIL_LOG_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 GO_SELIN_ENCOUNTER_DOOR = 188065;
    constexpr float DOOR_SEARCH = 60.0f;
    uint32 const now = getMSTime();
    if (!_failLogMs || getMSTimeDiff(_failLogMs, now) >= RETURN_FAIL_LOG_MS)
    {
        _failLogMs = now ? now : 1;
        GameObject* door = bot->FindNearestGameObject(GO_SELIN_ENCOUNTER_DOOR, DOOR_SEARCH);
        bool const shut = door && door->GetGoState() == GO_STATE_READY;
        bool const outside = door && bx < door->GetPositionX();
        InstanceScript* instance = bot->GetInstanceScript();
        LOG_INFO("playerbots",
                 "[mgt return to room] {} at ({:.1f}, {:.1f}): cannot walk in to ({:.1f}, {:.1f}): {}; Selin's "
                 "encounter state {}",
                 bot->GetName(), bx, by, moveX, moveY,
                 !door    ? std::string("no room door 188065 in sight")
                 : shut   ? (outside ? std::string("her room door 188065 is SHUT and the bot stands outside it - "
                                                   "locked out until the encounter ends")
                                     : std::string("her room door 188065 is shut"))
                          : std::string("the walk was refused (door open: path, ledge or closed-door guard)"),
                 instance ? int32(instance->GetBossState(0)) : -1);
    }
    return false;
}

bool MgTCloseOnMageGuardAction::Execute(Event)
{
    Unit* guard = botAI->GetUnit(AI_VALUE(ObjectGuid, "mgt mage guard target"));
    if (!guard)
        return false;

    return ReachCombatTo(guard, 0.0f);
}

bool MgTPriorityInterruptAction::Execute(Event)
{
    Unit* target = botAI->GetUnit(AI_VALUE(ObjectGuid, "mgt interrupt target"));
    if (!target)
        return false;

    uint8 const urgency = MagistersTerrace::GetInterruptUrgency(target);
    if (!urgency)
        return false;

    return !MagistersTerrace::CastInterrupt(botAI, target, urgency).empty();
}

bool MgTTauntEnragedWretchedAction::Execute(Event)
{
    Unit* wretched = botAI->GetUnit(AI_VALUE(ObjectGuid, "mgt enraged wretched"));
    if (!wretched)
        return false;

    for (std::string const& spell : MagistersTerrace::TauntSpellNames())
    {
        if (botAI->CanCastSpell(spell, wretched) && botAI->CastSpell(spell, wretched))
            return true;
    }

    return false;
}

bool MgTDelrissaInterruptAction::Execute(Event)
{
    auto const& order = AI_VALUE_REF(GuidVector, "mgt delrissa interrupt order");
    for (ObjectGuid const guid : order)
    {
        Unit* target = botAI->GetUnit(guid);
        if (!target)
            continue;

        uint8 const urgency = MagistersTerrace::GetRetinueInterruptUrgency(target);
        if (!urgency)
            continue;

        if (!MagistersTerrace::CastInterrupt(botAI, target, urgency).empty())
            return true;
    }

    return false;
}

// [b36_mgt] Hex and Polymorph on this bot's own mark, by this action itself, as long as the mark is unheld:
// "cast auto cc" holds any crowd control within 10 yd of a tank or a melee (CrowdControlWouldBreak), and in this
// fight every lackey is on somebody in that brawl or chases the healer with the tank's peels behind it - but no
// member uses an area spell next to a held lackey (MgTFocusBurstMultiplier, [delrissa_cc_keep]). Out of range or
// sight the caster walks there (the lackey is in the group's fight, the room's packs are dead by then).
// mgt-h 20261026-034959-mgt-h-5B-15: the [b34_mgt] single try at the marking found Warlord Salaris 36-45 yd away on
// the healer, out of sight; no Hex was cast all fight.
bool MgTDelrissaCcMarkAction::CastOnMark(Unit* target, std::string const& spell)
{
    if (spell != "hex" && spell != "polymorph")
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo || bot->HasSpellCooldown(spellId) || bot->IsNonMeleeSpellCast(false))
        return false;

    constexpr float rangeMargin = 3.0f;
    float const range = std::max(spellInfo->GetMaxRange(false, bot) - rangeMargin, 5.0f);
    if (bot->GetExactDist(target) > range)
        return MoveTo(target, range - 1.0f, MovementPriority::MOVEMENT_COMBAT);

    if (!bot->IsWithinLOSInMap(target))
        return MoveToLOS(target, true);

    if (!botAI->CanCastSpell(spell, target) || !botAI->CastSpell(spell, target))
        return false;

    uint32 const now = getMSTime();
    if (!_castLogMs || getMSTimeDiff(_castLogMs, now) >= 2 * IN_MILLISECONDS)
    {
        _castLogMs = now ? now : 1;
        Unit* victim = target->GetVictim();
        LOG_INFO("playerbots", "[delrissa cc] {} casts {} on {} ({:.1f} yd, on {})", bot->GetName(), spell,
                 target->GetName(), bot->GetExactDist(target), victim ? victim->GetName() : std::string("nobody"));
    }
    return true;
}

bool MgTDelrissaCcMarkAction::Execute(Event)
{
    Group* group = bot->GetGroup();
    std::string const spell = MagistersTerrace::DelrissaCcSpell(bot);
    if (!group || spell.empty())
        return false;

    // [b36_mgt] its own mark, not held: cast it (again)
    if (Unit* marked = MagistersTerrace::DelrissaOwnUnheldMark(bot))
        return CastOnMark(marked, spell);

    if (MagistersTerrace::CountControlledRetinue(bot) >= MagistersTerrace::DELRISSA_MAX_CROWD_CONTROL)
        return false;

    int32 const index = RtiTargetValue::GetRtiIndex(AI_VALUE(std::string, "rti cc"));
    if (index < 0 || index == RtiTargetValue::skullIndex || index == RtiTargetValue::crossIndex)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", spell));
    if (!spellInfo)
        return false;

    Unit* target = MagistersTerrace::PickDelrissaCcTarget(bot, spellInfo);
    if (!target)
        return false;

    RaidMarkOwnership::SetIcon(bot, group, static_cast<uint8>(index), target->GetGUID());

    Unit* victim = target->GetVictim();
    LOG_INFO("playerbots", "[delrissa cc] {} marks {} for {} (priority {}, on {}{})", bot->GetName(),
             target->GetName(), spell, MagistersTerrace::DelrissaCcPriority(target),
             victim ? victim->GetName() : std::string("nobody"),
             victim && victim->ToPlayer() && PlayerbotAI::IsHeal(victim->ToPlayer()) ? ", a healer" : "");

    // The generic "stale auto cc mark" takes the icon back once the mob dies or stays uncontrolled for 8 s.
    AutoCcMark mark;
    mark.target = target->GetGUID();
    mark.markedAtMs = getMSTime();
    context->GetValue<AutoCcMark>("auto cc mark")->Set(mark);

    // [b34_mgt] / [b36_mgt] the first cast at once (see CastOnMark)
    CastOnMark(target, spell);
    return true;
}

bool MgTDelrissaRetinueLeftAction::Execute(Event)
{
    constexpr float pullRange = 25.0f;
    Unit* lackey = MagistersTerrace::DelrissaRetinueLeft(bot);
    if (!lackey)
        return false;

    uint32 const now = getMSTime();
    if (!_logMs || getMSTimeDiff(_logMs, now) >= 10 * IN_MILLISECONDS)
    {
        _logMs = now ? now : 1;
        LOG_INFO("playerbots",
                 "[delrissa retinue left] {}: Delrissa is dead, {} lives ({:.1f} yd): the encounter is done only with "
                 "every lackey dead - pulling it",
                 bot->GetName(), lackey->GetName(), bot->GetExactDist(lackey));
    }

    if (bot->GetExactDist(lackey) > pullRange || !bot->IsWithinLOSInMap(lackey))
        return MoveTo(lackey->GetMapId(), lackey->GetPositionX(), lackey->GetPositionY(), lackey->GetPositionZ(),
                      false, false, false, false, MovementPriority::MOVEMENT_NORMAL, true);

    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy || strategy->HasTarget() || strategy->IsPullPendingToStart() || strategy->HasPullStarted())
        return false;

    strategy->RequestPull(lackey, true, false);
    bot->SetSelection(lackey->GetGUID());
    context->GetValue<Unit*>("current target")->Set(lackey);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    return true;
}

bool MgTDelrissaTremorTotemAction::Execute(Event)
{
    return botAI->CanCastSpell("tremor totem", bot) && botAI->CastSpell("tremor totem", bot);
}

bool MgTDelrissaHealerSpotAction::Execute(Event)
{
    auto const& spots = AI_VALUE_REF(MagistersTerrace::EscapeSpots, "mgt delrissa healer spot");
    if (spots.empty())
        return false;

    MagistersTerrace::EscapeSpot const spot = spots.front();
    MovementPriority const priority =
        bot->IsInCombat() ? MovementPriority::MOVEMENT_COMBAT : MovementPriority::MOVEMENT_NORMAL;
    return MoveTo(bot->GetMapId(), spot.x, spot.y, spot.z, false, false, false, false, priority, true, false);
}

bool MgTKaelInterruptAction::Execute(Event)
{
    Unit* target = MagistersTerrace::GetKaelInterruptTarget(bot);
    if (!target)
        return false;

    return !MagistersTerrace::CastInterrupt(botAI, target, MagistersTerrace::CONTROL_INTERRUPT_URGENCY).empty();
}

bool MgTTakeLapseSpotAction::Execute(Event)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
        return false;

    MagistersTerrace::LapseWorld world;
    MagistersTerrace::CollectLapseWorld(bot, world);

    // [b44_mgt] no sphere near a healer or a ranged member: it heals and casts where it stands
    if (MagistersTerrace::LapseCasterMayStand(bot, world))
    {
        _kite = MagistersTerrace::KiteState{};
        return false;
    }

    MagistersTerrace::LapseSpot spot = { bot->GetPositionX(), bot->GetPositionY() };
    MagistersTerrace::LapseSpot station = spot;
    Unit* kael = MagistersTerrace::GetKaelthas(bot);
    bool move = false;

    if (MagistersTerrace::GetGravityLapseKite(bot, world, spot, _kite) ||
        MagistersTerrace::GetGravityLapseDodge(bot, world, spot))
    {
        move = true;
    }
    else if (MagistersTerrace::GetGravityLapseSpot(bot, world, station) &&
             !MagistersTerrace::OnLapseStation(bot, kael, station))
    {
        move = MagistersTerrace::GetLapseApproach(bot, world, station,
                                                  MagistersTerrace::LapseThreatened(bot, world), spot);
    }

    if (!move)
        return false;

    float z = bot->GetPositionZ();
    if (kael && MagistersTerrace::IsLapseMeleeSlot(bot))
        z = kael->GetPositionZ();

    return MoveTo(bot->GetMapId(), spot.x, spot.y, z, false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT, true, false);
}

namespace
{
// Before the first pull the group tops up; after this long the tank pulls with whoever stands gathered, after
// twice this long wherever they stand (user rule: nobody stands still for good).
constexpr uint32 SELIN_ROOM_READY_WAIT_MS = 60000;
// A wait is broken off only after this long without a check (the pull alternates with other actions: the old 5 s
// reset the wait every time, "waited 0 s" for 3 minutes in mgt-n build 21).
constexpr uint32 SELIN_ROOM_WAIT_BREAK_MS = 30000;
// After this long a member short of the rally counts when the tank sees it this close (a healer that never reaches
// the rally spot itself, mgt-n build 21).
constexpr uint32 SELIN_ROOM_RALLY_WAIT_CAP_MS = 15000;
constexpr float SELIN_ROOM_RALLY_NEAR_TANK = 40.0f;
// [b35_mgt] After this long a member parked this close to the gather point counts as gathered, sight or not: its walk
// guard refuses the last yards (mgt-h 20261026-024524-mgt-h-5E-15, 337-498 s: the rogue stood at (199.2,-5.9), 10.5 yd
// from the rally (208.5,-1.0), "mgt selin room to rally ... would wake Wretched Skulker (17.5 yd of reach 22.5) ->
// stay", out of the tank's sight; the tank held "Rtrogab: not at the rally" for 160 s and the run stalled).
constexpr uint32 SELIN_ROOM_RALLY_PARKED_MS = 45000;
constexpr float SELIN_ROOM_RALLY_PARKED_RADIUS = 15.0f;
constexpr float SELIN_ROOM_RALLY_PARKED_DZ = 3.0f;
// [b35_mgt] the final entry's wait for laggers ends after this long (a lagger its walk guard parks would hold it)
constexpr uint32 SELIN_FINAL_LAG_MAX_MS = 20 * IN_MILLISECONDS;
constexpr float SELIN_ROOM_READY_HEALTH_PCT = 85.0f;
constexpr float SELIN_ROOM_READY_HEALER_MANA_PCT = 80.0f;
constexpr float SELIN_ROOM_READY_TANK_MANA_PCT = 50.0f;
// Once the group was ready, the walk to the cast spot does not wait for it again.
constexpr uint32 SELIN_ROOM_READY_LATCH_MS = 30000;
// [selin_ac_final_pull] the pull that brings Selin: the group full (user: "the group ready, full hp/mana")
constexpr float SELIN_FINAL_READY_HEALTH_PCT = 95.0f;
constexpr float SELIN_FINAL_READY_HEALER_MANA_PCT = 90.0f;
constexpr float SELIN_FINAL_READY_TANK_MANA_PCT = 60.0f;
// Where the tank takes the pair after the final pull: just inside her room, so the door shuts with it inside.
constexpr float SELIN_FINAL_FIGHT_X = MagistersTerrace::SELIN_SAFE_X + 2.0f;
constexpr float SELIN_FINAL_FIGHT_Y = 0.5f;
constexpr float SELIN_FINAL_FIGHT_Z = -2.8f;

// [b31_dg] The final entry's "wait for laggers": the tank leaves the stage spot only when no alive member stands more
// than this behind it (x, the walk-in axis). mgt-n 20261025-184229-mgt-n-5B-20 (349-354 s): the tank walked alone
// to the pair's cast spot (214.0, 1.0), 17.4 yd from the Skulker (aggro 19 yd), the pair and 2 s later Selin came,
// her room door 188065 (DOOR_TYPE_ROOM) shut on IN_PROGRESS with the healer at (212.4, 2.3) and the shadow priest at
// (211.7, -0.2) outside: they stood there 90 s ("[closed door] ... -> stay") and the tank died unhealed.
constexpr float SELIN_FINAL_LAG = 4.0f;
// The tank holds only while it still stands outside the pair's aggro (heroic 21 yd: x ~210.6 on her axis).
constexpr float SELIN_FINAL_HOLD_MAX_X = MagistersTerrace::SelinRoom::STAGE_X + 1.5f;
constexpr uint32 SELIN_FINAL_LAG_LOG_MS = 5 * IN_MILLISECONDS;

// The first alive group member standing more than SELIN_FINAL_LAG behind the tank on the walk in (nullptr: none).
Player* SelinEntryLagger(Player* tank)
{
    Group* group = tank->GetGroup();
    if (!group)
        return nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == tank || !member->IsAlive() || member->GetMapId() != tank->GetMapId() ||
            member->GetInstanceId() != tank->GetInstanceId())
            continue;

        if (tank->GetPositionX() - member->GetPositionX() > SELIN_FINAL_LAG)
            return member;
    }
    return nullptr;
}
}  // namespace

bool MgTSelinRoomPullAction::IsGroupReady(bool finalPull)
{
    using namespace MagistersTerrace::SelinRoom;

    uint32 const now = getMSTime();
    // A wait broken off (a fight, the tactic off) starts again.
    if (!_waitSinceMs || getMSTimeDiff(_waitCheckMs, now) >= SELIN_ROOM_WAIT_BREAK_MS)
        _waitSinceMs = now;
    _waitCheckMs = now;

    uint32 const waited = getMSTimeDiff(_waitSinceMs, now);
    Snapshot const& snapshot = Get(botAI);
    bool const inert = snapshot.selinInert;
    bool const roomFight = snapshot.fighting;
    // In combat nobody eats, drinks or regenerates: with Selin inert only gathering counts.
    bool const relaxed = !finalPull && (waited >= SELIN_ROOM_READY_WAIT_MS || inert);
    // The final pull's group must stand at the stage spot: the door shuts on whoever is not with it.
    bool const anyPlace = !finalPull && waited >= 2 * SELIN_ROOM_READY_WAIT_MS;
    float gatherX = RALLY_X;
    float gatherY = RALLY_Y;
    float gatherZ = RALLY_Z;
    GatherPoint(botAI, gatherX, gatherY, gatherZ);
    float const healthPct = finalPull ? SELIN_FINAL_READY_HEALTH_PCT : SELIN_ROOM_READY_HEALTH_PCT;
    float const healerManaPct = finalPull ? SELIN_FINAL_READY_HEALER_MANA_PCT : SELIN_ROOM_READY_HEALER_MANA_PCT;
    float const tankManaPct = finalPull ? SELIN_FINAL_READY_TANK_MANA_PCT : SELIN_ROOM_READY_TANK_MANA_PCT;
    char const* missing = nullptr;
    std::string who;
    auto const check = [&](Player* member)
    {
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            return;

        if (!member->IsAlive())
        {
            if (!relaxed)
                missing = "dead";
        }
        // With Selin inert everybody stays in her zone-wide combat; only a room pack's fight holds the pull.
        else if (member->IsInCombat() && !(inert && !roomFight))
            missing = "in combat";
        else if (member != bot && !anyPlace && member->GetExactDist2d(gatherX, gatherY) > GATHER_RADIUS &&
                 !(waited >= SELIN_ROOM_RALLY_WAIT_CAP_MS && bot->GetExactDist(member) <= SELIN_ROOM_RALLY_NEAR_TANK &&
                   bot->IsWithinLOSInMap(member)) &&
                 !(waited >= SELIN_ROOM_RALLY_PARKED_MS &&
                   member->GetExactDist2d(gatherX, gatherY) <= SELIN_ROOM_RALLY_PARKED_RADIUS &&
                   std::fabs(member->GetPositionZ() - gatherZ) <= SELIN_ROOM_RALLY_PARKED_DZ))
            missing = "not at the rally";
        else if (!relaxed)
        {
            bool drinking = false;
            if (RaidLeader::IsEatingOrDrinking(member, drinking))
                missing = "eating or drinking";
            else if (member->GetHealthPct() < healthPct)
                missing = "health";
            else if (member->getPowerType() == POWER_MANA && PlayerbotAI::IsHeal(member) &&
                     member->GetPowerPct(POWER_MANA) < healerManaPct)
                missing = "healer mana";
            else if (member == bot && member->getPowerType() == POWER_MANA &&
                     member->GetPowerPct(POWER_MANA) < tankManaPct)
                missing = "tank mana";
        }

        if (missing && who.empty())
            who = member->GetName();
    };

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref && !missing; ref = ref->next())
            check(ref->GetSource());
    }
    else
        check(bot);

    if (!missing)
        return true;

    if (!_waitLogMs || getMSTimeDiff(_waitLogMs, now) >= 10 * IN_MILLISECONDS)
    {
        _waitLogMs = now;
        LOG_INFO("playerbots", "[selin room] {} holds the pull at the rally ({}: {}, waited {} s)", bot->GetName(),
                 who, missing, waited / IN_MILLISECONDS);
    }
    return false;
}

bool MgTSelinRoomPullAction::Execute(Event /*event*/)
{
    using namespace MagistersTerrace::SelinRoom;

    PullPlan plan;
    if (!GetPullPlan(botAI, plan))
        return false;

    Unit* target = botAI->GetUnit(plan.target);
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!target || !target->IsAlive() || target->IsInCombat() || !strategy)
        return false;

    uint32 const now = getMSTime();
    // [selin_final_entry] the entry runs: straight on into the room, no new gather or ready check
    if (plan.linked && FinalEntryActive(bot))
    {
        bot->SetSelection(target->GetGUID());
        context->GetValue<Unit*>("current target")->Set(target);
        // [b31_dg] wait for laggers: the tank does not step into the pair's aggro (the door shuts 2 s later) while a
        // member is still behind it; holding here keeps it at the stage spot, outside every idle pack's reach.
        // [b35_mgt] at most SELIN_FINAL_LAG_MAX_MS of it per entry
        static std::mutex lagLock;
        static std::unordered_map<uint64, std::pair<uint32, uint32>> lagWaits;  // tank -> (since, last seen)
        uint32 lagWaited = 0;
        {
            std::lock_guard<std::mutex> guard(lagLock);
            std::pair<uint32, uint32>& wait = lagWaits[bot->GetGUID().GetRawValue()];
            if (!wait.first || getMSTimeDiff(wait.second, now) > 5 * IN_MILLISECONDS)  // not refreshed: a new entry
                wait.first = now ? now : 1;
            wait.second = now ? now : 1;
            lagWaited = getMSTimeDiff(wait.first, now);
        }
        if (bot->GetPositionX() <= SELIN_FINAL_HOLD_MAX_X && lagWaited < SELIN_FINAL_LAG_MAX_MS)
            if (Player* lagger = SelinEntryLagger(bot))
            {
                if (!_lagLogMs || getMSTimeDiff(_lagLogMs, now) >= SELIN_FINAL_LAG_LOG_MS)
                {
                    _lagLogMs = now ? now : 1;
                    LOG_INFO("playerbots",
                             "[selin final entry] {} waits at ({:.1f}, {:.1f}): {} at ({:.1f}, {:.1f}) is {:.1f} yd "
                             "behind, the door shuts behind the pair",
                             bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), lagger->GetName(),
                             lagger->GetPositionX(), lagger->GetPositionY(),
                             bot->GetPositionX() - lagger->GetPositionX());
                }
                return true;
            }
        return MoveTo(bot->GetMapId(), SELIN_FINAL_FIGHT_X, SELIN_FINAL_FIGHT_Y, SELIN_FINAL_FIGHT_Z, false, false,
                      false, false, MovementPriority::MOVEMENT_COMBAT, true);
    }
    // [selin_heroic_entry_pull] heroic, the pair idle: this pull brings the pair and Selin (see MgTShared.h)
    bool const entryPull = !plan.linked && HeroicEntryPull(botAI);

    if (!_readyMs || getMSTimeDiff(_readyMs, now) >= SELIN_ROOM_READY_LATCH_MS)
    {
        _readyMs = 0;
        // The group gathers at the rally (the stage spot for the final pull), the tank with it, and tops up there.
        float gatherX = RALLY_X;
        float gatherY = RALLY_Y;
        float gatherZ = RALLY_Z;
        GatherPoint(botAI, gatherX, gatherY, gatherZ);
        if (bot->GetExactDist2d(gatherX, gatherY) > RALLY_ARRIVED)
            return MoveTo(bot->GetMapId(), gatherX, gatherY, gatherZ);

        if (!IsGroupReady(plan.linked || entryPull))
            return false;

        _readyMs = now;
        // No other idle pack left: the margin is FLT_MAX, there is no reach to stay out of.
        std::string const outside = plan.margin >= std::numeric_limits<float>::max() / 2.0f
                                        ? std::string("no other pack idle")
                                        : Acore::StringFormat("{:.1f} yd outside the other packs' reach",
                                                              plan.margin);
        LOG_INFO("playerbots", "[selin room] {} pulls {} ({} pack{}) from ({:.1f}, {:.1f}), {:.1f} yd from it, {}",
                 bot->GetName(), target->GetName(), plan.linked ? "Selin's" : "a side",
                 plan.linked ? ": she comes, evades with nobody in her room or else is fought ([selin engaged])" : "",
                 plan.x, plan.y, target->GetExactDist(plan.x, plan.y, plan.z), outside);
    }

    // A side pack of four or more: the group's crowd control holds one or two of it (the "pre pull cc" strategy:
    // a Freezing Trap is laid where the hunter waits, at the rally the pack comes to; a Polymorph lands as it comes),
    // as the planner does for any hard pack (AggressiveTargetAction::HoldForPrePullCc). Wave 29 11:46: the whole
    // 6-pack on the tank, wipe with every Wretched at 1-17 %. The pair next to Selin comes alone and fast: no hold.
    bool const controlled = HoldsPackInControl(plan, target);
    if (controlled && PrePullCc::IsHolding(bot))
        return false;

    // [b31_dg] Not for the centre pair: its pull is the group's walk in from the stage spot (below), and its cast spot
    // stands inside the pair's aggro (mgt-n 20261025-184229-5B-20: (214.0, 1.0), 17.4 yd from the Skulker, aggro
    // 19 yd) - the tank walking there alone pulled the pair and Selin, and the door shut on the healer outside.
    if (!plan.linked && bot->GetExactDist2d(plan.x, plan.y) > SPOT_ARRIVED)
        return MoveTo(bot->GetMapId(), plan.x, plan.y, plan.z);

    // The crowd control stops holding the pull; without it nothing of it stays on the pack.
    if (controlled)
        PrePullCc::MarkPulled(bot);
    else
        PrePullCc::Cancel(bot);

    // The class's ranged pull, then back to the rally ("return to pull position"). The pair as a single-mob pull
    // (Avenger's Shield jumps 12.5 yd + reaches: from the pair it could reach a side pack); once the pair is dead a
    // side pack stands 35+ yd from the other one, and the paladin's choice may jump inside the pack.
    // [selin_ac_final_pull] The pair (it brings Selin, who never evades on AzerothCore) is taken into her room: the
    // group follows in from the stage spot ("mgt out of room", her fight committed once no Wretched is idle) and
    // the door shuts with everybody inside; the pair dies first, then Selin.
    // [selin_heroic_entry_pull] the group walks in as the tank casts; the tank follows to the fight spot on the pulled
    // pack's side (away from the other side pack, 26 yd from it), where the pack, the pair and Selin are fought.
    float const entryY = target->GetPositionY() >= 0.0f ? ENTRY_FIGHT_SIDE_Y : -ENTRY_FIGHT_SIDE_Y;
    if (entryPull && !FinalEntryActive(bot))
    {
        StartFinalEntry(bot, entryY);
        LOG_INFO("playerbots",
                 "[selin heroic entry pull] {}: {} (a side pack) brings the centre pair and Selin on heroic: the group "
                 "walks into the room now, the fight spot ({:.1f}, {:.1f})",
                 bot->GetName(), target->GetName(), ENTRY_FIGHT_X, entryY);
    }

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo pullPosition = posMap["pull"];
    if (entryPull)
        pullPosition.Set(ENTRY_FIGHT_X, entryY, SELIN_FINAL_FIGHT_Z, bot->GetMapId());
    else if (plan.linked)
        pullPosition.Set(SELIN_FINAL_FIGHT_X, SELIN_FINAL_FIGHT_Y, SELIN_FINAL_FIGHT_Z, bot->GetMapId());
    else
        pullPosition.Set(RALLY_X, RALLY_Y, RALLY_Z, bot->GetMapId());
    posMap["pull"] = pullPosition;

    // [selin_final_entry] The centre pair (it brings Selin 2 s later and the door shuts on IN_PROGRESS): no ranged pull
    // from outside - the whole group walks in together from the stage spot (every member's "mgt out of room" while
    // the entry runs), the tank first into the room's spot; the pair aggroes on the way in and is taken there.
    if (plan.linked)
    {
        if (!FinalEntryActive(bot))
        {
            StartFinalEntry(bot, SELIN_FINAL_FIGHT_Y);
            LOG_INFO("playerbots",
                     "[selin final entry] {}: the group is ready at the stage spot: everybody walks into the room "
                     "together, the centre pair ({}) comes on the way in, Selin 2 s later with the door shut behind "
                     "the group",
                     bot->GetName(), target->GetName());
        }
        bot->SetSelection(target->GetGUID());
        context->GetValue<Unit*>("current target")->Set(target);
        _readyMs = 0;
        _waitSinceMs = 0;
        // [b31_dg] the walk itself (and its wait for laggers) is the entry branch at the top, from the next tick on
        if (SelinEntryLagger(bot))
            return true;
        return MoveTo(bot->GetMapId(), SELIN_FINAL_FIGHT_X, SELIN_FINAL_FIGHT_Y, SELIN_FINAL_FIGHT_Z, false, false,
                      false, false, MovementPriority::MOVEMENT_COMBAT, true);
    }

    strategy->RequestPull(target, true, plan.linked);
    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>("current target")->Set(target);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    _readyMs = 0;
    _waitSinceMs = 0;
    return true;
}

bool MgTSelinRoomPullAction::HoldsPackInControl(MagistersTerrace::SelinRoom::PullPlan const& plan, Unit* target)
{
    using namespace MagistersTerrace::SelinRoom;

    if (plan.linked || !sPlayerbotAIConfig.AutoCc || !sPlayerbotAIConfig.AutoCcPrePull)
        return false;

    if (PrePullCc::HasPlan(bot, target->GetGUID()))
        return true;

    Snapshot const& snapshot = Get(botAI);
    int16 const index = PackOf(snapshot, target->GetGUID());
    if (index < 0)
        return false;

    std::vector<Unit*> rest;
    for (ObjectGuid const& guid : snapshot.packs[index].members)
    {
        Unit* mob = botAI->GetUnit(guid);
        if (mob && mob != target && mob->IsAlive() && !mob->IsInCombat())
            rest.push_back(mob);
    }

    if (rest.size() + 1 < sPlayerbotAIConfig.AutoCcMarkMinPackSize)
        return false;

    // Who may answer: the crowd control dps, or the healer of a group that has none (as the planner asks them).
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    GuidVector dps;
    GuidVector healers;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            PlayerbotAI::IsTank(member) || !GET_PLAYERBOT_AI(member) || !GetCrowdControlRole(member->getClass()))
            continue;

        (PlayerbotAI::IsHeal(member) ? healers : dps).push_back(member->GetGUID());
    }

    GuidVector const& controllers = dps.empty() ? healers : dps;
    if (controllers.empty())
        return false;

    // The kill order the tank opens with: the pull target, then the rest as the tank's kill marks will rank them;
    // the claims come from its tail.
    KillOrder::SortKillOrder(rest, target, nullptr);
    GuidVector order;
    order.push_back(target->GetGUID());
    for (Unit* mob : rest)
        order.push_back(mob->GetGUID());

    PrePullCc::Register(bot, target, order, controllers, false);
    LOG_INFO("playerbots", "[selin room] {} asks {} crowd control member(s) to hold part of the {} pack of {}",
             bot->GetName(), controllers.size(), order.size(), target->GetName());
    return true;
}

bool MgTWretchedInfusionAction::Execute(Event /*event*/)
{
    // A drinking Wretched hits its victim with +2000 % damage for 10 s: the tank's damage cooldown.
    static std::vector<std::string> const defensives = {"divine protection", "shield wall", "survival instincts",
                                                        "icebound fortitude", "barkskin"};
    // Four or more Wretched in the fight: the shaman's haste for the whole group shortens it.
    static std::vector<std::string> const hastes = {"heroism", "bloodlust"};

    bool const tank = PlayerbotAI::IsTank(bot);
    for (std::string const& spell : tank ? defensives : hastes)
    {
        if (botAI->CanCastSpell(spell, bot) && botAI->CastSpell(spell, bot))
            return true;
    }
    return false;
}

bool MgTSelinRoomToRallyAction::Execute(Event /*event*/)
{
    using namespace MagistersTerrace::SelinRoom;

    // the stage spot before the final pull ([selin_ac_final_pull]), else the rally
    float x = RALLY_X;
    float y = RALLY_Y;
    float z = RALLY_Z;
    if (!bot->IsInCombat())
        GatherPoint(botAI, x, y, z);
    if (bot->GetExactDist2d(x, y) <= RALLY_ARRIVED)
        return false;

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true);
}

bool MgTSelinPetOutAction::Execute(Event /*event*/)
{
    using namespace MagistersTerrace::SelinRoom;

    Creature* pet = PetToCallBack(botAI);
    CharmInfo* charmInfo = pet ? pet->GetCharmInfo() : nullptr;
    if (!charmInfo)
        return false;

    // Already on its way back, off every mob: nothing to order again.
    if (!pet->GetVictim() && charmInfo->HasCommandState(COMMAND_FOLLOW) && charmInfo->IsCommandFollow())
        return false;

    uint32 const now = getMSTime();
    if (!_logMs || getMSTimeDiff(_logMs, now) >= 5 * IN_MILLISECONDS)
    {
        _logMs = now;
        Snapshot const& snapshot = Get(botAI);
        LOG_INFO("playerbots",
                 "[selin pet] {}: {} at ({:.1f}, {:.1f}) called back from Selin's room ({}; {} Wretched of her floor "
                 "alive): a pet in her room is her victim and the group must not follow it in",
                 bot->GetName(), pet->GetName(), pet->GetPositionX(), pet->GetPositionY(),
                 pet->GetGUID() == snapshot.selinPetVictim ? "her victim" : "past her door", snapshot.wretchedAlive);
    }

    // the client's Follow button (COMMAND_FOLLOW), as DungeonPetControl gives it
    pet->AttackStop();
    pet->InterruptNonMeleeSpells(false);
    pet->ClearInPetCombat();
    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    charmInfo->SetCommandState(COMMAND_FOLLOW);
    charmInfo->SetIsCommandAttack(false);
    charmInfo->SetIsAtStay(false);
    charmInfo->SetIsReturning(true);
    charmInfo->SetIsCommandFollow(true);
    charmInfo->SetIsFollowing(false);
    charmInfo->RemoveStayPosition();
    return true;
}

bool MgTSelinRoomLeashAction::Execute(Event /*event*/)
{
    using namespace MagistersTerrace::SelinRoom;

    constexpr uint32 logMs = 5 * IN_MILLISECONDS;

    uint32 const now = getMSTime();
    if (!_logMs || getMSTimeDiff(_logMs, now) >= logMs)
    {
        _logMs = now;
        LOG_INFO("playerbots",
                 "[selin leash] {} at ({:.1f}, {:.1f}) walks back to the rally: {} Wretched of her floor alive, "
                 "nobody enters her room before they are dead",
                 bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), Get(botAI).wretchedAlive);
    }

    if (bot->GetExactDist2d(RALLY_X, RALLY_Y) <= RALLY_ARRIVED)
        return false;

    return MoveTo(bot->GetMapId(), RALLY_X, RALLY_Y, RALLY_Z, false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT, true);
}
