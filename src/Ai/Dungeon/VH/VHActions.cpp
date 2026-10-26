/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "VHActions.h"
#include "Creature.h"
#include "DungeonInteract.h"
#include "HealthTriggers.h"  // [b33_vh] RestState
#include "InstanceScript.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "TerrainHazard.h"  // [b35_z0] SafeFloorZ
#include "VHTriggers.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <list>
#include <unordered_map>

namespace
{
constexpr float SINCLARI_GATHER_RADIUS = 30.0f;
constexpr float SINCLARI_TALK_DISTANCE = INTERACTION_DISTANCE - 1.0f;
constexpr float APPROACH_DISTANCE = 5.0f;
constexpr float PORTAL_STANDBY_DISTANCE = 15.0f;
constexpr float RALLY_SPREAD = 3.0f;
// A released prisoner: melee walks to this far from it, ranged to its spell range less a margin.
constexpr float ENGAGE_MELEE_DISTANCE = 1.5f;
constexpr float ENGAGE_RANGED_DISTANCE = 25.0f;
// The tank opens on a prisoner it sees from this far with a ranged pull: Moragg stops in the mouth of his cell
// (violet_hold.h BossStartMove1) and the tank stood 6-7 yd off with him as its victim for 2 minutes, no swing landed
// (vh-n build 12, vh-h build 13). The pull brings him out.
constexpr float ENGAGE_OPENER_RANGE = 30.0f;
// Where the trash ends its waypoints and starts channelling on the seal (violet_hold.h, last waypoints)
constexpr float DOOR_X = 1845.0f;
constexpr float DOOR_Y = 803.0f;
constexpr float DOOR_GUARD_RADIUS = 15.0f;
constexpr float XEVOZZ_KITE_RADIUS = 14.0f;
constexpr float XEVOZZ_KITE_MAX_STEP = 25.0f;
constexpr float XEVOZZ_KITE_MIN_STEP = 3.0f;
constexpr float XEVOZZ_SPHERE_SEARCH = 60.0f;
constexpr float XEVOZZ_FALLBACK_STEP = 10.0f;
constexpr uint8 XEVOZZ_KITE_POINTS = 12;

bool AllMembersAlive(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return bot->IsAlive();

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (!member->IsAlive())
            return false;
    }
    return true;
}

// [b33_vh] vh-h 20261026-001930 pull #17: the shaman drank until ~810 s 15-22 yd behind, the bear walked up to the
// guardian portal (z 47.7, the healer on the floor at z 38.6) and opened on it at 814 s with the healer 31 yd off and
// out of sight; 5 mobs, the first heal (Riptide) at 821 s with the bear at 8 %, dead at 822 s. The tank's move to a
// keeper portal waits while a healer of the group (alive, not stuck far off) is drinking / eating, farther than
// HEALER_GATE_REACH or out of the tank's sight, at most HEALER_GATE_MAX_MS per portal; nothing on the group yet.
constexpr float HEALER_GATE_REACH = 20.0f;
constexpr float HEALER_GATE_FAR = 60.0f;
constexpr uint32 HEALER_GATE_MAX_MS = 12 * IN_MILLISECONDS;

bool TankWaitsForHealer(PlayerbotAI* botAI, Player* bot, Creature* portal, Unit* keeper)
{
    struct Hold
    {
        ObjectGuid portal;
        uint32 start = 0;
    };
    thread_local std::unordered_map<ObjectGuid, Hold> holds;  // by tank

    Group* group = bot->GetGroup();
    if (!group || !PlayerbotAI::IsTank(bot) || (keeper && keeper->IsInCombat()) ||
        !botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get().empty())
    {
        holds.erase(bot->GetGUID());
        return false;
    }

    Player* away = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref && !away; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() ||
            member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId() ||
            !PlayerbotAI::IsHeal(member) || PlayerbotAI::IsTank(member))
            continue;

        float const distance = bot->GetExactDist(member);
        if (distance > HEALER_GATE_FAR)
            continue;

        if (RestState::IsDrinking(member) || RestState::IsEating(member) || distance > HEALER_GATE_REACH ||
            !bot->IsWithinLOSInMap(member))
            away = member;
    }

    if (!away)
    {
        holds.erase(bot->GetGUID());
        return false;
    }

    uint32 const now = getMSTime();
    Hold& hold = holds[bot->GetGUID()];
    if (hold.portal != portal->GetGUID() || !hold.start)
    {
        hold.portal = portal->GetGUID();
        hold.start = now ? now : 1;
    }

    if (getMSTimeDiff(hold.start, now) >= HEALER_GATE_MAX_MS)
        return false;

    if (bot->isMoving())
        bot->StopMoving();
    return true;
}
}  // namespace

bool AttackErekemAction::Execute(Event /*event*/)
{
    // Focus boss first, adds after
    Unit* boss = AI_VALUE2(Unit*, "find target", "erekem");
    if (!boss) { return false; }

    if (AI_VALUE(Unit*, "current target") != boss)
        {
            return Attack(boss);
        }
    return false;
}

bool AttackIchorGlobuleAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ichoron");
    if (!boss) { return false; }

    Unit* currentTarget = AI_VALUE(Unit*, "current target");

    // Tank prioritise boss if it's up
    if (PlayerbotAI::IsTank(bot) && !boss->HasAura(SPELL_DRAINED))
    {
        if (AI_VALUE(Unit*, "current target") != boss)
        {
            return Attack(boss);
        }
        return false;
    }

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_ICHOR_GLOBULE)
        {
            // Check IDs here, NOT Unit* pointers:
            // Don't keep swapping between sentries.
            // If we're already attacking one, don't retarget another
            if (currentTarget && currentTarget->GetEntry() == NPC_ICHOR_GLOBULE)
            {
                return false;
            }
            return Attack(unit);
        }
    }
    // No ichor globules left alive, fall back to targeting boss
    if (currentTarget != boss)
    {
        return Attack(boss);
    }

    return false;
}

bool AttackVoidSentryAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "zuramat the obliterator");
    if (!boss) { return false; }

    Unit* currentTarget = AI_VALUE(Unit*, "current target");

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    // GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_VOID_SENTRY)
        {
            // Check IDs here, NOT Unit* pointers:
            // Don't keep swapping between sentries.
            // If we're already attacking one, don't retarget another
            if (currentTarget && currentTarget->GetEntry() == NPC_VOID_SENTRY)
            {
                return false;
            }
            return Attack(unit);
        }
    }
    // No void sentries left alive, fall back to targeting boss
    if (currentTarget != boss)
    {
        return Attack(boss);
    }

    return false;
}

bool VHStartEventAction::Execute(Event /*event*/)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetData(VHIds::DATA_ENCOUNTER_STATUS) != NOT_STARTED)
        return false;

    Creature* sinclari = instance->GetCreature(VHIds::DATA_SINCLARI);
    if (!sinclari || !sinclari->IsAlive() || !sinclari->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return false;

    if (!bot->IsWithinDistInMap(sinclari, SINCLARI_TALK_DISTANCE))
        return MoveNear(sinclari, 2.0f);

    if (!AllMembersAlive(bot) || !DungeonInteract::IsGroupGathered(bot, sinclari, SINCLARI_GATHER_RADIUS))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "vh-start-wait",
                                       "Violet Hold: waiting for the whole group alive and out of combat at Sinclari");
        return false;
    }

    if (!DungeonInteract::OpenGossip(bot, sinclari))
        return false;

    bool started = false;
    GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
    if (menu.GetMenuId() == VHIds::GOSSIP_MENU_SINCLARI)
    {
        // The option that opens the start confirmation; the late-join teleport option has no follow-up menu.
        int32 option = -1;
        for (auto const& entry : menu.GetMenuItems())
        {
            GossipMenuItemData const* data = menu.GetItemData(entry.first);
            if (data && data->GossipActionMenuId == VHIds::GOSSIP_MENU_SINCLARI_START)
            {
                option = static_cast<int32>(entry.first);
                break;
            }
        }

        if (DungeonInteract::SelectGossipOption(bot, sinclari, option) &&
            bot->PlayerTalkClass->GetGossipMenu().GetMenuId() == VHIds::GOSSIP_MENU_SINCLARI_START)
        {
            int32 const confirm = DungeonInteract::FindGossipOption(bot, [](GossipMenuItem const&) { return true; });
            started = DungeonInteract::SelectGossipOption(bot, sinclari, confirm);
        }
    }
    bot->PlayerTalkClass->SendCloseGossip();

    if (started)
        LOG_INFO("playerbots", "Violet Hold: {} started the event through the Sinclari gossip", bot->GetName());

    return started;
}

bool VHAttackSealAttackerAction::Execute(Event /*event*/)
{
    Unit* attacker = VHShared::NearestSealAttacker(bot);
    if (!attacker)
        return false;

    RaidBotDuty::LogWorkaroundOnce(bot, "vh-activation-crystal",
                                   "Violet Hold: needs honest tactic: Activation Crystals are not used, "
                                   "the seal health is only sent to clients as a world state");

    // Already fighting something at the door: finish it instead of ping-ponging between door trash.
    Unit* current = AI_VALUE(Unit*, "current target");
    if (current && current->IsAlive() &&
        (current == attacker || current->GetExactDist2d(DOOR_X, DOOR_Y) <= DOOR_GUARD_RADIUS))
        return false;

    if (!bot->IsWithinLOSInMap(attacker))
        return MoveNear(attacker, APPROACH_DISTANCE, MovementPriority::MOVEMENT_COMBAT);

    return Attack(attacker);
}

bool VHAttackPortalKeeperAction::Execute(Event /*event*/)
{
    Creature* portal = VHShared::ActiveKeeperPortal(bot);
    if (!portal)
        return false;

    Unit* keeper = VHShared::PortalKeeper(portal);
    if (!keeper)
    {
        // The keeper appears 10 s after the portal: wait next to it, its trash spawns there too.
        if (bot->IsInCombat() || bot->GetExactDist2d(portal) <= PORTAL_STANDBY_DISTANCE)
            return false;

        if (TankWaitsForHealer(botAI, bot, portal, nullptr))  // [b33_vh]
            return true;

        return MoveNear(portal, PORTAL_STANDBY_DISTANCE - APPROACH_DISTANCE);
    }

    Unit* current = AI_VALUE(Unit*, "current target");
    if (current == keeper)
        return false;

    // The tank keeps the trash it holds and only takes the keeper when free.
    if (PlayerbotAI::IsTank(bot) && bot->IsInCombat() && current && current->IsAlive())
        return false;

    // [b33_vh] not onto an idle keeper (and the trash next to it) without the healer
    if (TankWaitsForHealer(botAI, bot, portal, keeper))
        return true;

    if (!bot->IsWithinLOSInMap(keeper))
        return MoveNear(keeper, APPROACH_DISTANCE, MovementPriority::MOVEMENT_COMBAT);

    return Attack(keeper);
}

bool VHEngageReleasedBossAction::Execute(Event /*event*/)
{
    Creature* boss = VHShared::ReleasedBoss(bot);
    if (!boss)
        return false;

    // The prisoner stands in front of its cell until somebody comes (vh-n/h 2026-10-25: the tank had it as its
    // target 26 yd off, out of sight behind the cell's frame, and nothing walked it closer: MoveNear tries only spots
    // in sight of the bot, and an Attack on the same target returns false). Target it, then walk into reach by the
    // navmesh until the fight starts.
    if (AI_VALUE(Unit*, "current target") != boss || bot->GetVictim() != boss)
        if (Attack(boss))
            return true;

    bool const melee = PlayerbotAI::IsTank(bot) || PlayerbotAI::IsMelee(bot);
    bool const inReach = bot->IsWithinLOSInMap(boss) &&
                         (melee ? bot->IsWithinMeleeRange(boss) : bot->GetDistance(boss) <= ENGAGE_RANGED_DISTANCE);
    if (inReach)
        return false;

    if (PlayerbotAI::IsTank(bot) && !boss->IsInCombat() && bot->IsWithinLOSInMap(boss) &&
        bot->GetDistance(boss) <= ENGAGE_OPENER_RANGE)
    {
        static constexpr char const* openers[] = {"avenger's shield", "hand of reckoning", "exorcism",
                                                  "judgement of wisdom", "judgement of light", "heroic throw",
                                                  "taunt", "death grip", "icy touch", "faerie fire (feral)",
                                                  "growl", "shoot", "throw"};
        for (char const* opener : openers)
            if (botAI->CanCastSpell(opener, boss) && botAI->CastSpell(opener, boss))
                return true;
    }

    float const distance = melee ? ENGAGE_MELEE_DISTANCE : ENGAGE_RANGED_DISTANCE - APPROACH_DISTANCE;
    if (MoveTo(boss, distance, MovementPriority::MOVEMENT_COMBAT))
        return true;
    return MoveNear(boss, APPROACH_DISTANCE, MovementPriority::MOVEMENT_COMBAT) || bot->isMoving();
}

bool VHRallyAction::Execute(Event /*event*/)
{
    float const angle = GetFollowAngle();
    float const x = VHIds::RALLY_X + std::cos(angle) * RALLY_SPREAD;
    float const y = VHIds::RALLY_Y + std::sin(angle) * RALLY_SPREAD;
    float z = VHIds::RALLY_Z;
    // [b35_z0] the floor under the spread spot from the navmesh near RALLY_Z. UpdateAllowedPositionZ seeded at 38.7
    // sent the vmap ray from 39.2, under the floor (the navmesh polygon there is centred at z 40.6): no hit, the
    // grid's 0.0 of the WMO-only tile, and z clamped to it (vh-n 20261026-000434-vh-n-5E-46: "vh rally" to
    // (1865.0,805.0,0.0) from z 44.4, refused as a 44 yd ledge drop, 84 times on the night). No floor found: the
    // fixed RALLY_Z stands.
    SafeFloorZ(bot, x, y, z);
    return MoveTo(bot->GetMapId(), x, y, z);
}

bool VHLateJoinAction::Execute(Event /*event*/)
{
    // A member who released during the event walks back in through the portal as a ghost and rises at the
    // entrance (the corpse lies in the instance: WorldSession::HandleMoveWorldportAck), outside the closed seal.
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || !bot->IsAlive() || !VHShared::IsOutsideSeal(bot))
        return false;

    Creature* sinclari = instance->GetCreature(VHIds::DATA_SINCLARI);
    if (!sinclari || !sinclari->IsAlive() || !sinclari->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return false;

    if (!bot->IsWithinDistInMap(sinclari, SINCLARI_TALK_DISTANCE))
        return MoveNear(sinclari, 2.0f);

    if (bot->isMoving())
        bot->StopMoving();
    if (!DungeonInteract::OpenGossip(bot, sinclari))
        return false;

    // The option exists only while the encounter is IN_PROGRESS (its condition); the server checks it again.
    GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
    bool sent = false;
    if (menu.GetMenuId() == VHIds::GOSSIP_MENU_SINCLARI && menu.GetItem(VHIds::GOSSIP_OPTION_SINCLARI_LATE_JOIN))
        sent = DungeonInteract::SelectGossipOption(bot, sinclari,
                                                   static_cast<int32>(VHIds::GOSSIP_OPTION_SINCLARI_LATE_JOIN));
    bot->PlayerTalkClass->SendCloseGossip();
    if (sent)
        LOG_INFO("playerbots", "Violet Hold: {} asked Sinclari to be sent in (late join)", bot->GetName());
    return sent;
}

bool XevozzKiteAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "xevozz");
    if (!boss)
        return false;

    std::list<Creature*> spheres;
    boss->GetCreatureListWithEntryInGrid(spheres, VHIds::NPC_ETHEREAL_SPHERE, XEVOZZ_SPHERE_SEARCH);
    if (spheres.empty())
        return false;

    // Walk the boss around the room centre, always towards the reachable point farthest from every sphere.
    float bestScore = -1.0f;
    float bestX = 0.0f;
    float bestY = 0.0f;
    for (uint8 i = 0; i < XEVOZZ_KITE_POINTS; ++i)
    {
        float const angle = 2.0f * static_cast<float>(M_PI) * i / XEVOZZ_KITE_POINTS;
        float const x = VHIds::ROOM_CENTER_X + std::cos(angle) * XEVOZZ_KITE_RADIUS;
        float const y = VHIds::ROOM_CENTER_Y + std::sin(angle) * XEVOZZ_KITE_RADIUS;
        if (bot->GetExactDist2d(x, y) > XEVOZZ_KITE_MAX_STEP)
            continue;

        float nearest = FLT_MAX;
        for (Creature* sphere : spheres)
            if (sphere && sphere->IsAlive())
                nearest = std::min(nearest, sphere->GetExactDist2d(x, y));

        if (nearest > bestScore)
        {
            bestScore = nearest;
            bestX = x;
            bestY = y;
        }
    }

    if (bestScore < 0.0f)
    {
        Creature* closest = nullptr;
        for (Creature* sphere : spheres)
            if (sphere && sphere->IsAlive() && (!closest || bot->GetExactDist2d(sphere) < bot->GetExactDist2d(closest)))
                closest = sphere;

        return closest && MoveAway(closest, XEVOZZ_FALLBACK_STEP);
    }

    if (bot->GetExactDist2d(bestX, bestY) < XEVOZZ_KITE_MIN_STEP)
        return false;

    float z = VHIds::ROOM_CENTER_Z;
    bot->UpdateAllowedPositionZ(bestX, bestY, z);
    return MoveTo(bot->GetMapId(), bestX, bestY, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}
