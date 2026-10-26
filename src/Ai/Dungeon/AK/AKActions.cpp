/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AKActions.h"
#include "DungeonInteract.h"
#include "GameObject.h"
#include "LastMovementValue.h"  // [b35_ok]
#include "MovementActions.h"    // [b35_ok] IdleAggroBlocker
#include "MoveSpline.h"         // [b35_ok]
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "WlkTacticLog.h"

bool AttackNadoxGuardianAction::Execute(Event /*event*/)
{
    Unit* target = AI_VALUE2(Unit*, "find target", "ahn'kahar guardian");
    if (!target || AI_VALUE(Unit*, "current target") == target)
    {
        return false;
    }
    return Attack(target);
}

bool AttackJedogaVolunteerAction::Execute(Event /*event*/)
{
    Unit* target = nullptr;
    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit) // Null check for safety
        {
            continue; // Skip null or invalid units
        }
        if (unit && unit->GetEntry() == NPC_TWILIGHT_VOLUNTEER)
        {
            target = unit;
            break;
        }
    }

    if (!target || AI_VALUE(Unit*, "current target") == target)
    {
        return false;
    }
    return Attack(target);
}

bool AvoidShadowCrashAction::Execute(Event /*event*/)
{
    // Could check all enemy units in range as it's possible to pull multiple of these mobs.
    // They should really be killed 1 by 1, multipulls are messy so we just handle singles for now
    Unit* unit = AI_VALUE2(Unit*, "find target", "forgotten one");
    if (!unit) { return false; }

    Unit* victim = nullptr;
    float radius = 10.0f;
    float targetDist = radius + 2.0f;

    // Actively move if targeted by a shadow crash.
    // Spell check not needed, they don't have any other non-instant casts
    if (unit->HasUnitState(UNIT_STATE_CASTING)) // && unit->FindCurrentSpellBySpellId(SPELL_SHADOW_CRASH))
    {
        // This doesn't seem to avoid casts very well, perhaps because this isn't checked while allies are casting.
        // TODO: Revisit if this is an issue in heroics, otherwise ignore shadow crashes for the most part.
        // Forgotten One can cast with no target (Psychic Scream, target swap): GetTarget() is then empty.
        victim = botAI->GetUnit(unit->GetTarget());
        if (victim)
        {
            float const distance = bot->GetExactDist2d(victim->GetPosition());
            if (distance < radius)
                return MoveAway(victim, targetDist - distance);
        }
    }

    // Otherwise ranged members passively spread, to avoid AoE overlap
    if (PlayerbotAI::IsMelee(bot)) { return false; }

    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Unit* memberUnit = botAI->GetUnit(member);
        if (!memberUnit || bot->GetGUID() == member)
        {
            continue;
        }
        float currentDist = bot->GetExactDist2d(memberUnit);
        if (currentDist < radius)
        {
            return MoveAway(memberUnit, targetDist - currentDist);
        }
    }
    return false;
}

// [b35_ok] ok-h 20261026-024524-ok-h-5H-34 (686-721 s, death burst, run stopped): after the east device the bear tank
// walked ~75 yd to the west device (655.7,-719.0) at once, the disc priest still drinking 17 yd back; "[pull gate]
// waiting: healer Rtpriak not with the group (52 yd)" and "[pack why] refused Eye of Taldaram ... 4 mobs" held the
// planner, but this walk (ACTION_MOVE + 2, its only gate IsGroupGathered at 80 yd) went on into the device's own pack
// (2 Eyes of Taldaram + 2 Frostbringers, 7 yd from it): pull 14 "unplanned (tank's use nerubian device)",
// "pull_without_healer" at 52.2 yd, the tank 100 -> 31 % in 3 s (Shadow Shock 17289 x2 + Frostbolt 61461, all
// spell damage), the healer's sight walk refused, then tank, priest, tank (battle res) and cat died to the Eyes.
// Pull 13 (602 s, east device, Frostbringer) was the same kind; every ok-h run since 25.10 shows 1-4 such pulls.
// A player leads the group to the device and clears its pack first: the walk waits for the laggers
// (DungeonInteract::GroupLaggers, as [b32_pull] / [b35_occ]) and stops short of an idle pack that reaches the device,
// where the planner (pull candidates within 40 yd) takes it. Neither hold lasts past its cap (no stall).
namespace
{
constexpr float DEVICE_GROUP_LEAD_RADIUS = 20.0f;
constexpr uint32 DEVICE_GROUP_WAIT_MAX_MS = 60 * IN_MILLISECONDS;
constexpr float DEVICE_PACK_RADIUS = 40.0f;        // idle mobs this near the device are looked at
constexpr float DEVICE_PACK_MARGIN = 3.0f;         // over the core's aggro reach at the device spot
constexpr float DEVICE_PACK_STOP_DISTANCE = 32.0f; // the walk stops this far from such a mob (Eye reach ~20-23 yd)
constexpr uint32 DEVICE_PACK_WAIT_MAX_MS = 120 * IN_MILLISECONDS;
constexpr uint32 DEVICE_HOLD_LOG_MS = 10 * IN_MILLISECONDS;
}  // namespace

void UseNerubianDeviceAction::StopOwnWalk()
{
    if (!_walking)
        return;

    _walking = false;
    if (bot->isMoving() || !bot->movespline->Finalized())
    {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        AI_VALUE(LastMovement&, "last movement").clear();
    }
}

bool UseNerubianDeviceAction::HoldWalk(AKIds::NerubianDevice const* device)
{
    uint32 const nowMs = getMSTime() | 1u;
    // At the device already: it is used, nothing walks.
    if (bot->GetExactDist(device->x, device->y, device->z) <= AKIds::DEVICE_USE_DISTANCE + 3.0f)
    {
        _laggerWaitMs = 0;
        _packWaitMs = 0;
        return false;
    }

    bool const logNow = !_holdLogMs || getMSTimeDiff(_holdLogMs, nowMs) >= DEVICE_HOLD_LOG_MS;

    std::string const laggers =
        DungeonInteract::GroupLaggers(bot, device->x, device->y, device->z, DEVICE_GROUP_LEAD_RADIUS);
    if (laggers.empty())
        _laggerWaitMs = 0;
    else
    {
        if (!_laggerWaitMs)
            _laggerWaitMs = nowMs;
        if (getMSTimeDiff(_laggerWaitMs, nowMs) < DEVICE_GROUP_WAIT_MAX_MS)
        {
            StopOwnWalk();
            if (logNow)
            {
                _holdLogMs = nowMs;
                LOG_INFO("playerbots", "[b35_ok] {}: waits for {} before the walk to Ancient Nerubian Device {} "
                         "({:.1f} yd)", bot->GetName(), laggers, device->goEntry,
                         bot->GetExactDist(device->x, device->y, device->z));
            }
            return true;
        }
    }

    // The nearest idle aggressive mob that wakes on the bot at the device: the device's pack, the planner's pull.
    Creature* blocker = nullptr;
    float blockerDistance = 0.0f;
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
            !bot->IsHostileTo(mob) || mob->IsDungeonBoss() ||
            mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) ||
            mob->GetExactDist(device->x, device->y, device->z) > DEVICE_PACK_RADIUS ||
            MovementAction::IdleAggroBlocker(mob, bot, device->x, device->y, device->z, DEVICE_PACK_MARGIN, true))
            continue;

        float const distance = bot->GetExactDist(mob);
        if (!blocker || distance < blockerDistance)
        {
            blocker = mob;
            blockerDistance = distance;
        }
    }

    if (!blocker)
    {
        _packWaitMs = 0;
        return false;
    }

    if (!_packWaitMs)
        _packWaitMs = nowMs;
    if (getMSTimeDiff(_packWaitMs, nowMs) >= DEVICE_PACK_WAIT_MAX_MS)
    {
        if (logNow)
        {
            _holdLogMs = nowMs;
            LOG_INFO("playerbots", "[b35_ok] {}: {} still idle at Ancient Nerubian Device {} after {} s: walks on",
                     bot->GetName(), blocker->GetName(), device->goEntry, DEVICE_PACK_WAIT_MAX_MS / IN_MILLISECONDS);
        }
        return false;
    }

    // Far from the pack the walk goes on (the planner takes it from DEVICE_PACK_STOP_DISTANCE); there it stops.
    if (blockerDistance > DEVICE_PACK_STOP_DISTANCE)
        return false;

    StopOwnWalk();
    if (logNow)
    {
        _holdLogMs = nowMs;
        LOG_INFO("playerbots", "[b35_ok] {}: Ancient Nerubian Device {} waits: {} ({}) at {:.1f} yd from it wakes at "
                 "the device, the planner pulls it first ({:.1f} yd from the bot, {} s)", bot->GetName(),
                 device->goEntry, blocker->GetName(), blocker->GetEntry(),
                 blocker->GetExactDist(device->x, device->y, device->z), blockerDistance,
                 getMSTimeDiff(_packWaitMs, nowMs) / IN_MILLISECONDS);
    }
    return true;
}

bool UseNerubianDeviceAction::Execute(Event /*event*/)
{
    AKIds::NerubianDevice const* device = AKIds::FindPendingDevice(bot);
    if (!device)
        return false;

    // Walking off alone would pull the trash of the side platforms into a split group.
    if (!DungeonInteract::IsGroupGathered(bot, bot, AKIds::DEVICE_GATHER_RADIUS))
    {
        StopOwnWalk();  // [b35_ok]
        return false;
    }

    // [b35_ok] the group with the bot, the device's pack pulled by the planner first
    if (HoldWalk(device))
        return false;

    if (bot->GetExactDist(device->x, device->y, device->z) > AKIds::DEVICE_SEARCH_RANGE)
    {
        _walking = true;  // [b35_ok]
        return MoveTo(bot->GetMapId(), device->x, device->y, device->z);
    }

    GameObject* go = bot->FindNearestGameObject(device->goEntry, AKIds::DEVICE_SEARCH_RANGE);
    if (!go || !go->isSpawned() || go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "ahnkahet-nerubian-device",
                                       "needs honest tactic: Ancient Nerubian Device " +
                                           std::to_string(device->goEntry) +
                                           " is missing or not selectable although it is not activated");
        return false;
    }

    if (!go->IsWithinDistInMap(bot, go->GetInteractionDistance()))
    {
        if (MoveNear(go, AKIds::DEVICE_USE_DISTANCE))
            return true;

        return MoveTo(go->GetMapId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
    }

    bot->StopMoving();
    _walking = false;  // [b35_ok]
    if (!DungeonInteract::UseGameObject(bot, go))
        return false;

    LOG_INFO("playerbots", "Ahn'kahet: {} used Ancient Nerubian Device {}", bot->GetName(), device->goEntry);
    return true;
}

bool AttackJedogaInitiateAction::GroupReady()
{
    uint32 const now = getMSTime();
    if (DungeonInteract::IsGroupGathered(bot, bot, AKJedogaRoom::ROOM_RADIUS))
    {
        _gatherWaitMs = 0;
        return true;
    }

    if (!_gatherWaitMs)
        _gatherWaitMs = now;

    if (getMSTimeDiff(_gatherWaitMs, now) < AKJedogaRoom::GATHER_WAIT_MS)
    {
        if (WlkTacticLog::Allow(bot, "ak jedoga gather"))
            LOG_INFO("playerbots", "[ak jedoga gather] {} waits for the group at the altar before the next initiate "
                     "({} s)", bot->GetName(), getMSTimeDiff(_gatherWaitMs, now) / IN_MILLISECONDS);
        return false;
    }

    // A straggler (dead, stuck on the stairs) does not hold the gate for ever: the healer at the tank's side is
    // enough for one kneeling initiate at a time.
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (DungeonInteract::IsActiveMember(bot, member) && PlayerbotAI::IsHeal(member) && !member->IsInCombat() &&
            member->IsWithinDistInMap(bot, AKJedogaRoom::HEALER_NEAR))
            return true;
    }
    return false;
}

bool AttackJedogaInitiateAction::Execute(Event /*event*/)
{
    AKJedogaRoom::Snapshot const& snapshot = AKJedogaRoom::Get(botAI);
    if (!snapshot.active || !GroupReady())
        return false;

    // Jedoga only descends once all 15 kneeling initiates are dead (boss_jedoga_shadowseeker.cpp
    // SummonedCreatureDies 226-266): one at a time, the nearest the tank can see.
    if (Unit* initiate = botAI->GetUnit(snapshot.visibleTarget))
    {
        if (!initiate->IsAlive() || !bot->IsValidAttackTarget(initiate))
            return false;

        if (WlkTacticLog::Allow(bot, "ak jedoga initiate pull"))
            LOG_INFO("playerbots", "[ak jedoga initiate pull] {} pulls {} at {:.1f} yd ({} initiates left)",
                     bot->GetName(), initiate->GetName(), bot->GetExactDist(initiate), snapshot.initiatesAlive);
        // [ak-jedoga-walk] Already its target and victim out of melee: walk in. Attack() again every tick stops
        // every move (AttackAction: StopMoving below MOVEMENT_COMBAT) and a kneeling initiate never enters combat,
        // so the "already attacking" early-out never comes (ok-n wave 23: 115 s at 38.6 yd, "reach melee" never
        // evaluated, the harness walk "made no progress").
        if (AI_VALUE(Unit*, "current target") == initiate && bot->GetVictim() == initiate &&
            !bot->IsWithinMeleeRange(initiate))
            return MoveNear(initiate, AKJedogaRoom::APPROACH_DISTANCE);
        return Attack(initiate);
    }

    // None in sight: walk to the nearest one until it is (they kneel around the altar, the pillars hide some).
    Unit* nearest = botAI->GetUnit(snapshot.nearestTarget);
    if (!nearest)
        return MoveTo(bot->GetMapId(), AKJedogaRoom::ALTAR_X, AKJedogaRoom::ALTAR_Y, AKJedogaRoom::ALTAR_Z);

    if (WlkTacticLog::Allow(bot, "ak jedoga initiate approach"))
        LOG_INFO("playerbots", "[ak jedoga initiate approach] {}: no initiate in sight, walks to {} ({:.1f} yd)",
                 bot->GetName(), nearest->GetName(), bot->GetExactDist(nearest));
    if (MoveNear(nearest, AKJedogaRoom::APPROACH_DISTANCE))
        return true;

    return MoveTo(bot->GetMapId(), AKJedogaRoom::ALTAR_X, AKJedogaRoom::ALTAR_Y, AKJedogaRoom::ALTAR_Z);
}

bool VolazjAttackVisageAction::Execute(Event /*event*/)
{
    Unit* visage = AKShared::FindVisibleVisage(botAI);
    if (!visage)
        return false;

    return Attack(visage);
}

// [b33_ok] The fungus only reaches 4 yd round the dying mushroom: a ranged bot that shot it from afar would waste it.
// It is attacked once the bot stands next to it; walking there goes through MoveNear (an Attack() every tick stops
// every move, see AttackJedogaInitiateAction).
bool AmanitarPotentFungusAction::Execute(Event /*event*/)
{
    Creature* mushroom = AKAmanitar::FindFungusMushroom(botAI);
    if (!mushroom)
        return false;

    float const distance = bot->GetExactDist2d(mushroom);
    if (distance > AKAmanitar::FUNGUS_KILL_DISTANCE)
    {
        if (WlkTacticLog::Allow(bot, "ak amanitar fungus walk"))
            LOG_INFO("playerbots", "[ak amanitar fungus walk] {} has Mini, walks to Healthy Mushroom at {:.1f} yd",
                     bot->GetName(), distance);
        if (MoveNear(mushroom, AKAmanitar::FUNGUS_APPROACH))
            return true;

        return MoveTo(mushroom->GetMapId(), mushroom->GetPositionX(), mushroom->GetPositionY(),
                      mushroom->GetPositionZ());
    }

    // Already its target: the bot's own melee swing or spell kills it (it has 1 % of a normal mob's health).
    if (AI_VALUE(Unit*, "current target") == mushroom)
        return false;

    if (WlkTacticLog::Allow(bot, "ak amanitar fungus kill"))
        LOG_INFO("playerbots", "[ak amanitar fungus kill] {} kills Healthy Mushroom at {:.1f} yd for Potent Fungus",
                 bot->GetName(), distance);
    return Attack(mushroom);
}
