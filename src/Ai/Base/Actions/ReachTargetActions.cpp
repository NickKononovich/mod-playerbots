/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReachTargetActions.h"
#include "CellImpl.h"  // [b33_move]
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "Event.h"
#include "GridNotifiers.h"      // [b33_move]
#include "GridNotifiersImpl.h"  // [b33_move]
#include "GameObject.h"  // [b47_plan]
#include "GenericSpellActions.h"
#include "Group.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"  // [lead792]
#include "MoveSpline.h"    // [lead792]
#include "BotPath.h"  // [lead109]
#include "PathGenerator.h"
#include "PlayerbotAIConfig.h"
#include "LeaderOrders.h"  // [lead29] the planned pull's mobs
#include "PullFacts.h"  // [lead146] MobComesToTank
#include "Playerbots.h"
#include "PullActions.h"  // [b46_ohb] FightHold
#include "RangedPositionActions.h"
#include "ServerFacade.h"
#include "SpellInfo.h"  // [b32_arc]
#include "SpellMgr.h"   // [b32_arc]
#include "TankAoeThreat.h"  // [b32_peel]
#include "TerrainHazard.h"  // [b47_plan] PathThroughClosedDoor for the corpse res spot
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <list>  // [b33_move]
#include <mutex>
#include <set>  // [lead1041]
#include <unordered_map>
#include <vector>

bool ReachTargetAction::Execute(Event /*event*/) { return ReachCombatTo(AI_VALUE(Unit*, GetTargetName()), distance); }

bool ReachTargetAction::isUseful()
{
    // do not move while staying
    if (botAI->HasStrategy("stay", botAI->GetState()))
    {
        return false;
    }

    // do not move while casting
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
    {
        return false;
    }
    Unit* target = GetTarget();
    // float dis = distance + CONTACT_DISTANCE;
    return target &&
           !bot->IsWithinCombatRange(target, distance);  // ServerFacade::instance().IsDistanceGreaterThan(AI_VALUE2(float,
                                                         // "distance", GetTargetName()), distance);
}

std::string const ReachTargetAction::GetTargetName() { return "current target"; }

namespace
{
    constexpr float TANK_LEASH_RANGE = 30.0f;       // a held mob farther than this from the healer is not chased
    constexpr float PEEL_WALK_IDLE_MARGIN = 3.0f;   // the fight drifts a few yards around the mob walked to

    // Wave 8 Slave Pens: the tank ran 30 yd after a mob while the healer, 40-54 yd behind, died to another pack.
    // A mob on the tank comes by itself; a caster that stays away is left to the ranged.
    bool TankLeashHolds(PlayerbotAI* botAI, Player* bot, Unit* target)
    {
        if (!PlayerbotAI::IsTank(bot) || !bot->IsInCombat() || !botAI->IsInNonRaidDungeon())
            return false;

        Creature* creature = target->ToCreature();
        if (!creature || creature->IsDungeonBoss())
            return false;

        // on a group member it is the tank's to take back, however far it is
        Unit* victim = target->GetVictim();
        Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
        if (victimPlayer && victimPlayer != bot && bot->IsInSameRaidWith(victimPlayer))
            return false;

        // The last fight of the group is no fight to leave to the ranged: a caster that stays out (stuck, leashed)
        // was hit by nobody (arc-n build 19: an Eredar Deathbringer in combat 41 yd away, every member without a
        // target for 180 s). With no other mob on the group the tank walks to it.
        bool otherFight = false;
        for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            Unit* other = botAI->GetUnit(guid);
            // [b33_arc] a mob that fights nobody (no victim: a phantom combat reference, [b30_stalls]) is no other
            // fight. arc-h 20261026-000434-arc-h-5E-33: two Eredar Deathbringers (spawns 138943, 138942) in combat on
            // nobody, the healer 50 yd from both; each was the other's "other fight", the leash held both, "reach
            // melee" never reached the [b32_arc] ranged pull ("10 mobs in combat", trash fight idle 180 s).
            if (other && other != target && other->IsAlive() && other->IsInCombat() && other->GetVictim())
            {
                otherFight = true;
                break;
            }
        }

        if (!otherFight)
            return false;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || !member->IsInCombat() ||
                member->GetMapId() != bot->GetMapId() || !PlayerbotAI::IsHeal(member))
                continue;

            float const targetToHealer = target->GetExactDist2d(member);
            if (targetToHealer > TANK_LEASH_RANGE && targetToHealer > bot->GetExactDist2d(member))
                return true;
        }

        return false;
    }

    bool SpotWakesIdlePack(PlayerbotAI* botAI, Player* bot, Unit* target)
    {
        return IdleMobReachingSpot(botAI, bot, target) != nullptr;
    }

    constexpr uint32 TANK_REACH_LOG_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 TANK_REACH_DROP_MS = 10 * IN_MILLISECONDS;  // [tank_reach_drop_idle_target]

    struct TankReachRefusal
    {
        ObjectGuid target;
        uint32 sinceMs = 0;
    };

    // per map thread: the bot's refused reach target and since when it fights nobody of the group
    std::unordered_map<ObjectGuid, TankReachRefusal>& TankReachRefusals()
    {
        thread_local std::unordered_map<ObjectGuid, TankReachRefusal> refusals;
        return refusals;
    }
}

Creature* IdleMobReachingSpot(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    // [idle_pack_no_los] Every idle mob in sight range, line of sight or not: "possible targets" holds only the mobs
    // the bot's eyes see, and in the Scholomance Necromancer room the log repeated "possible targets: no line of
    // sight from the bot's eyes" while the tank walked 30-45 yd into the next packs (wipe analysis w24-25, cause B:
    // 3 attackers grew to 19-26). The fight drifts round the corner the eyes cannot see past.
    for (ObjectGuid const guid :
         botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
    {
        Creature* mob = botAI->GetCreature(guid);
        if (mob && mob != target && !Leader::PullPack::Contains(bot, guid) &&  // [lead29] the planned pull's own mobs
            mob->IsAlive() && !mob->IsInCombat() && mob->HasReactState(REACT_AGGRESSIVE) &&
            bot->IsHostileTo(mob) &&
            !MovementAction::IdleAggroBlocker(mob, bot, target->GetPositionX(), target->GetPositionY(),
                                              target->GetPositionZ(), PEEL_WALK_IDLE_MARGIN, false))
            return mob;
    }

    return nullptr;
}

// Scholomance heroic 2026-10-24 22:32:47: in a fight of 7 the tank's "reach melee" ran 14.5 yd after a Neophyte that
// cast on the healer from beside the hall's next pack (the taunts cooling down) and the fight grew to 11, then 15. An
// idle mob (22:31:27) woke on the hunter: the tank's Righteous Defense took it, then "reach melee" ran 19 yd into its
// pack's room. A mob that fights already comes to the taunt; an idle one is the pull planner's.
Creature* TankWalkIntoIdlePack(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    if (!target || !PlayerbotAI::IsTank(bot) || !botAI->IsInNonRaidDungeon() || !target->IsInCombat() ||
        bot->IsWithinDistInMap(target, ReachMeleeAction::TANK_FAR_MOB))
        return nullptr;

    Creature* creature = target->ToCreature();
    if (!creature || creature->IsDungeonBoss())
        return nullptr;

    // [b39_hos] A mob that cannot path to its victim evades every attack ("Evade": Creature::IsEvadingAttacks ->
    // CanNotReachTarget, no damage, health regenerates) and no taunt brings it, so refusing the walk is a stalemate.
    // hos-h 20261026-054314-hos-h-5G-19: a Crystalline Shardling (27973, no script) stood 15.6 yd from the DK tank,
    // its victim, unable to reach it; "[tank reach] no walk ... aggro reach of idle Unrelenting Construct" x300, every
    // hit evaded, 90.3 % -> 99 %, "trash stalemate ... 180 s". The tank walks to it (into its reach, so it stops
    // evading) and takes the idle pack with it, as a player does who sees the Evade text.
    if (creature->CanNotReachTarget())
    {
        thread_local std::unordered_map<ObjectGuid, uint32> evadeLog;
        uint32 const now = getMSTime();
        uint32& last = evadeLog[bot->GetGUID()];
        if (!last || getMSTimeDiff(last, now) >= TANK_REACH_LOG_MS)
        {
            last = now ? now : 1;
            LOG_INFO("playerbots", "[b39_hos unreachable mob] {}: walks to {} ({:.1f} yd): it cannot reach its victim "
                     "and evades every attack, the idle-pack guard does not hold", bot->GetName(), target->GetName(),
                     bot->GetExactDist(target));
        }
        return nullptr;
    }

    return IdleMobReachingSpot(botAI, bot, target);
}

namespace
{
    // [b32_arc] A mob that holds the bot in combat (a PvE combat reference) and fights nobody: no victim, not
    // evading, not held by crowd control, not running. Such a reference (combat inherited without threat) never
    // brings the mob: the core engages a creature only on threat, and a PvE reference never times out.
    bool IsPhantomCombatTarget(Player* bot, Unit* target)
    {
        Creature* mob = target ? target->ToCreature() : nullptr;
        return mob && mob->IsAlive() && !mob->GetVictim() && !mob->IsInEvadeMode() && mob->IsInCombatWith(bot) &&
               !mob->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) &&
               !mob->HasBreakableByDamageCrowdControlAura();
    }

    constexpr uint32 SHOOTER_SIGHT_BREAK_MS = 8 * IN_MILLISECONDS;   // [b46_ohb] refused this long, standing
    constexpr uint32 SHOOTER_SIGHT_RETRY_MS = 5 * IN_MILLISECONDS;   // [b46_ohb] after a search found no spot
    constexpr float SHOOTER_GROUP_SIGHT_RANGE = 36.0f;               // [b46_ohb] a ranged member shoots it from here
    constexpr float SHOOTER_SIGHT_BREAK_WALK = 30.0f;                // [b46_ohb] the farthest member spot walked to
    constexpr int SHOOTER_SIGHT_BREAK_PATHS = 4;
    constexpr float SHOOTER_SIGHT_PATH_END = 2.0f;

    // [b46_ohb] A mob that shoots the bot from where it stands: the bot is its victim, it stands still in the bot's
    // sight, not fleeing, not held, not evading; and no ranged member or healer of the group sees it within
    // SHOOTER_GROUP_SIGHT_RANGE (nobody hurts it from where the group stands).
    bool ShooterOutOfGroupSight(Player* bot, Unit* target)
    {
        Creature* mob = target ? target->ToCreature() : nullptr;
        if (!mob || !mob->IsAlive() || mob->IsDungeonBoss() || mob->GetVictim() != bot || mob->isMoving() ||
            mob->IsInEvadeMode() || mob->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) ||
            mob->HasBreakableByDamageCrowdControlAura() || !mob->IsWithinLOSInMap(bot))
            return false;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
                (!PlayerbotAI::IsRanged(member) && !PlayerbotAI::IsHeal(member)))
                continue;

            if (member->GetExactDist(mob) <= SHOOTER_GROUP_SIGHT_RANGE && member->IsWithinLOSInMap(mob))
                return false;
        }

        return true;
    }

    constexpr float PHANTOM_PULL_RANGE_SLACK = 3.0f;  // [b32_arc] the cast spot stands this far inside the spell range
    constexpr uint32 PHANTOM_TAUNT_WAIT_MS = 6 * IN_MILLISECONDS;     // [b33_arc] a taunt's time to turn the mob
    constexpr uint32 PHANTOM_TAUNT_FAILED_MS = 60 * IN_MILLISECONDS;  // [b33_arc] then the damage spell, this long

    float PullSpellRange(PlayerbotAI* botAI, char const* spell)
    {
        if (!spell || !TauntReactionAction::SpellReady(botAI, spell))
            return 0.0f;

        uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
        SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        float const range = spellInfo ? spellInfo->GetMaxRange(false, botAI->GetBot()) : 0.0f;
        return range > 0.0f ? range : TauntReactionAction::DEFAULT_TAUNT_RANGE;
    }
}

// [b32_arc] See the header. True when it cast or walked.
bool ReachMeleeAction::PullPhantomFromRange(Unit* target)
{
    uint32 const now = getMSTime();
    bool const logDue = !_phantomLogMs || getMSTimeDiff(_phantomLogMs, now) >= TANK_REACH_LOG_MS;
    float const distance = bot->GetExactDist(target);
    TauntReactionAction::Spells const* spells = TauntReactionAction::ClassSpells(bot);

    // [b33_arc] A taunt that landed and brought nothing: arc-n 20261026-000434-arc-n-5B-24 (00:16-00:19), a Defender
    // Corpse (rooted trap) in combat on nobody, "pulls Defender Corpse 21303 from range with taunt" 16 times in
    // 150 s, never a victim. The taunt only turns a mob that can attack; within PHANTOM_TAUNT_FAILED_MS of a taunt on
    // the same mob the ranged threat spell (damage: Avenger's Shield, Heroic Throw, ...) is cast instead, and no
    // spell is cast for PHANTOM_TAUNT_WAIT_MS after the taunt (it gets time to turn the mob).
    // [b34_arc] A mob in combat whose AI never engaged (no threat list entry: the Arcatraz Warder / Defender Corpse
    // traps, put in combat by a shot that made no threat ref) is turned by nothing: Spell::EffectTaunt does nothing on
    // an empty threat list, and the damage spell made none. arc-h 20261026-014521-arc-h-5F-31: "pulls Warder Corpse
    // 21304 from range with taunt" every ~40 s for 20 min, each cast re-flagging the group. No spell; the combat
    // reference is left to end (the test harness ends an unengaged reference after 20 s).
    if (Creature const* mob = target->ToCreature(); mob && mob->IsInCombat() && !mob->IsEngaged())
    {
        if (logDue)
        {
            _phantomLogMs = now ? now : 1;
            LOG_INFO("playerbots", "[b34_arc] {}: no pull spell on {} {} ({:.1f} yd): in combat, never engaged by its "
                     "AI (no threat list entry), a taunt cannot turn it", bot->GetName(), target->GetName(),
                     target->GetEntry(), distance);
        }
        return false;
    }

    bool const taunted = _phantomTaunted == target->GetGUID() && _phantomTauntedMs &&
                         getMSTimeDiff(_phantomTauntedMs, now) < PHANTOM_TAUNT_FAILED_MS;
    if (taunted && getMSTimeDiff(_phantomTauntedMs, now) < PHANTOM_TAUNT_WAIT_MS)
        return false;

    std::string ready;
    if (!taunted)
        ready = TauntReactionAction::ReadyFor(botAI, target);
    else if (spells && spells->ranged && TauntReactionAction::Usable(botAI, spells->ranged, target))
        ready = spells->ranged;

    if (!ready.empty() && botAI->CastSpell(ready, target))
    {
        _phantomLogMs = now ? now : 1;
        if (spells && spells->taunt && ready == spells->taunt)
        {
            _phantomTaunted = target->GetGUID();
            _phantomTauntedMs = now ? now : 1;
        }
        LOG_INFO("playerbots", "[b32_arc] {}: pulls {} {} from range with {} ({:.1f} yd, in combat on nobody){}",
                 bot->GetName(), target->GetName(), target->GetEntry(), ready, distance,
                 taunted ? " [b33_arc] the taunt brought nothing" : "");
        return true;
    }

    float range = spells ? PullSpellRange(botAI, spells->ranged) : 0.0f;
    if (spells && !taunted)
        range = std::max(range, PullSpellRange(botAI, spells->taunt));
    if (range <= PHANTOM_PULL_RANGE_SLACK || distance <= range - 1.0f)
        return false;  // nothing ready yet (cooldowns): the next tick tries again

    // Step into the spell's range on the line to the mob, on a spot no idle mob reaches (the walk would pull them).
    float const want = range - PHANTOM_PULL_RANGE_SLACK;
    float const dx = bot->GetPositionX() - target->GetPositionX();
    float const dy = bot->GetPositionY() - target->GetPositionY();
    float const flat = std::sqrt(dx * dx + dy * dy);
    if (flat < 0.1f)
        return false;

    float const x = target->GetPositionX() + dx / flat * want;
    float const y = target->GetPositionY() + dy / flat * want;
    float z = bot->GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);

    for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || mob == target || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
            !bot->IsHostileTo(mob) ||
            MovementAction::IdleAggroBlocker(mob, bot, x, y, z, PEEL_WALK_IDLE_MARGIN, false))
            continue;

        if (logDue)
        {
            _phantomLogMs = now ? now : 1;
            LOG_INFO("playerbots", "[b32_arc] {}: no step into pull range of {} ({:.1f} yd, range {:.0f}): idle {} "
                     "reaches the spot", bot->GetName(), target->GetName(), distance, range, mob->GetName());
        }
        return false;
    }

    if (logDue)
    {
        _phantomLogMs = now ? now : 1;
        LOG_INFO("playerbots", "[b32_arc] {}: steps into pull range of {} {} ({:.1f} -> {:.1f} yd, in combat on "
                 "nobody)", bot->GetName(), target->GetName(), target->GetEntry(), distance, want);
    }
    return MoveTo(bot->GetMapId(), x, y, z);
}

// [b46_ohb] See the header. True when it walked.
bool ReachMeleeAction::BreakShooterSight(Unit* target)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    uint32 const now = getMSTime();
    float const eye = bot->GetCollisionHeight();
    Player* healer = nullptr;
    std::vector<std::pair<float, Player*>> spots;  // the healer's spot first, then the nearest
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap())
            continue;

        bool const heal = PlayerbotAI::IsHeal(member);
        if (heal && (!healer || bot->GetExactDist(member) < bot->GetExactDist(healer)))
            healer = member;

        float const walk = bot->GetExactDist(member);
        if (walk > SHOOTER_SIGHT_BREAK_WALK ||
            target->IsWithinLOS(member->GetPositionX(), member->GetPositionY(), member->GetPositionZ() + eye))
            continue;

        spots.push_back({heal ? walk - 100.0f : walk, member});
    }
    std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    std::string refused = spots.empty() ? "no member within 30 yd stands out of its sight" : "";
    int paths = 0;
    for (auto const& [score, member] : spots)
    {
        float const x = member->GetPositionX();
        float const y = member->GetPositionY();
        float const z = member->GetPositionZ();
        if (healer && healer != member && !healer->IsWithinLOS(x, y, z + eye))
        {
            refused = member->GetName() + "'s spot: out of the healer's sight";
            continue;
        }

        Creature* waker = nullptr;
        for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets no los"))
        {
            Creature* mob = botAI->GetCreature(guid);
            if (mob && mob != target && mob->IsAlive() && !mob->IsInCombat() &&
                mob->HasReactState(REACT_AGGRESSIVE) && bot->IsHostileTo(mob) &&
                !MovementAction::IdleAggroBlocker(mob, bot, x, y, z, PEEL_WALK_IDLE_MARGIN, false))
            {
                waker = mob;
                break;
            }
        }
        if (waker)
        {
            refused = member->GetName() + "'s spot: in the aggro reach of idle " + waker->GetName();
            continue;
        }

        if (paths++ >= SHOOTER_SIGHT_BREAK_PATHS)
            break;

        BotPath const path = FindBotPath(bot, x, y, z);  // [lead109]
        PathType const type = path.type;
        if ((type & PATHFIND_NOPATH) || path.points.empty() ||
            (!(type & PATHFIND_NORMAL) &&
             (path.actualEnd - G3D::Vector3(x, y, z)).length() > SHOOTER_SIGHT_PATH_END))
        {
            refused = member->GetName() + "'s spot: no path";
            continue;
        }

        // the fight spot moves with the walk (HoldFightSpot would end it at the old spot's edge); back if refused
        Position oldAnchor;
        bool const hadAnchor = FightHold::Get(bot, oldAnchor);
        FightHold::Move(bot, Position(x, y, z, bot->GetOrientation()));
        float const walk = bot->GetExactDist(x, y, z);
        if (MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true))
        {
            _sightBreakLogMs = now ? now : 1;
            Creature* idle = IdleMobReachingSpot(botAI, bot, target);
            LOG_INFO("playerbots", "[b46_ohb] shooter_sight_break: {}: {} {} shoots it from {:.1f} yd and no ranged "
                     "member sees it (the walk to it wakes idle {}): steps {:.1f} yd out of its sight to {}'s spot "
                     "({:.1f},{:.1f},{:.1f}), the fight spot moves there", bot->GetName(), target->GetName(),
                     target->GetEntry(), bot->GetExactDist(target), idle ? idle->GetName() : std::string("?"), walk,
                     member->GetName(), x, y, z);
            return true;
        }

        if (hadAnchor)
            FightHold::Move(bot, oldAnchor);
        refused = member->GetName() + "'s spot: the walk was refused (path guard)";
    }

    _sightBreakFailMs = now ? now : 1;
    if (!_sightBreakLogMs || getMSTimeDiff(_sightBreakLogMs, now) >= TANK_REACH_LOG_MS)
    {
        _sightBreakLogMs = now ? now : 1;
        LOG_INFO("playerbots", "[b46_ohb] shooter_sight_break: {}: {} {} shoots it from {:.1f} yd, no spot out of "
                 "its sight ({})", bot->GetName(), target->GetName(), target->GetEntry(), bot->GetExactDist(target),
                 refused.empty() ? "path budget spent" : refused);
    }
    return false;
}

// [lead792] b221 rfc-h (8 runs): the led tank's "reach melee" walked on after its refusal held - a walk issued while
// its mob stood or fought nobody went on once the mob came at the tank (MobComesToTank) or once the plan put the fight
// elsewhere (TankWalkMovesFight): 27 s / 213 yd with the walk under way at a sample where the mob already came, the
// mob and the tank meeting halfway (5A 12 s: the walk issued at 9.2 s with Earthborer on nobody, 10.7 yd on to it
// after it turned at the tank). The refusal ends the tank's own walk to that mob too: its spline is the bot's last
// own MoveTo (not a forced escape), it ends nearer the mob than the tank stands, and no leader position command
// for the tank ends there. Returns the yards left of the stopped walk (0: none stopped)
static float StopOwnWalkToMob(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    // a mob in the fight only: the walk to the open order's mob not yet in combat is the pull (the engage walk)
    if (!target->IsInCombat() || !bot->isMoving() || bot->movespline->Finalized())
        return 0.0f;
    G3D::Vector3 const end = bot->movespline->FinalDestination();
    LastMovement& last = botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
    if (last.priority == MovementPriority::MOVEMENT_FORCED ||
        std::hypot(last.lastMoveToX - end.x, last.lastMoveToY - end.y) > 1.0f)
        return 0.0f;
    if (target->GetExactDist(end.x, end.y, end.z) >= target->GetExactDist(bot))
        return 0.0f;
    Leader::MemberEvents events;
    if (Leader::Events::Get(bot->GetGUID(), events) && events.move != Leader::MoveEvent::None &&
        events.move != Leader::MoveEvent::Follow &&
        events.spot.GetExactDist2d(end.x, end.y) <= std::max(events.tolerance, Leader::LEADER_SPOT_TOLERANCE) + 1.0f)
        return 0.0f;
    float const left = bot->GetExactDist(end.x, end.y, end.z);
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    return left;
}

// [lead960] b227 5C-11 (the wipe): the engage order on Ragefire Shaman (75.0 s, "engage: the tank walks its route in",
// the pack 45 yd off, its route 86 path points round the ledge) went to Fighting at 84.1 s with a Ragefire Trogg of
// the way on the tank; the walk to the Shaman went on - 83-95 s the blood DK slid ~80 yd down the route with the
// Trogg in its melee, woke the next pack (11 mobs, overload 1.57) and died. "reach melee" never ran (the Trogg stood
// in reach) and lead792 judges only a walk to the reach-melee target. The walk in of an engage / body pull is the
// pull's execution: once the leader's order reads Fighting (the tank in a fight with a mob on it or its victim) its
// purpose has ended and the fight is where the tank took it. It stops if it is the tank's own last MoveTo (not a
// forced escape), issued before the order went Fighting, and it does not end on the order's fight spot or a leader
// position command's spot. b227 (16 runs): 15 such walks in 11 runs, 230 yd in fights (92 yd with the mob in
// its melee, 7 woke more mobs); 5C-11 84 s 67 yd and the wipe.
bool StopPullWalkInFight(PlayerbotAI* botAI, Player* bot)
{
    if (!bot->isMoving() || bot->movespline->Finalized() || !bot->IsAlive() || !bot->IsInCombat() ||
        !PlayerbotAI::IsTank(bot) || !Leader::LedBot(bot))
        return false;
    if (!bot->GetVictim() && bot->getAttackers().empty())
        return false;

    Leader::PullOrder order;
    if (!Leader::Orders::GetOpen(bot->GetGUID(), order) || order.state != Leader::OrderState::Fighting ||
        (order.method != Leader::PullMethod::Engage && order.method != Leader::PullMethod::Body))
        return false;

    G3D::Vector3 const end = bot->movespline->FinalDestination();
    LastMovement& last = botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
    if (!last.msTime || last.priority == MovementPriority::MOVEMENT_FORCED ||
        std::hypot(last.lastMoveToX - end.x, last.lastMoveToY - end.y) > 1.0f)
        return false;
    // issued before the order went Fighting (the clocks are both getMSTime, wrap-safe)
    if (static_cast<int32>(order.changedMs - last.msTime) <= 0)
        return false;
    if (order.fight.fightSpot.GetExactDist2d(end.x, end.y) <= Leader::LEADER_SPOT_TOLERANCE)
        return false;

    Leader::MemberEvents events;
    if (Leader::Events::Get(bot->GetGUID(), events) && events.move != Leader::MoveEvent::None &&
        events.move != Leader::MoveEvent::Follow &&
        events.spot.GetExactDist2d(end.x, end.y) <= std::max(events.tolerance, Leader::LEADER_SPOT_TOLERANCE) + 1.0f)
        return false;

    Unit* mob = bot->GetVictim();
    if (!mob)
        mob = *bot->getAttackers().begin();
    float const left = bot->GetExactDist(end.x, end.y, end.z);
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    LOG_INFO("playerbots", "[lead960] {}: stops its {} walk to {} ({:.1f} yd of it left, issued {:.1f} s before the "
             "fight): the order is Fighting, {} on it {:.1f} yd off{}", bot->GetName(),
             Leader::MethodName(order.method), order.targetName, left,
             getMSTimeDiff(last.msTime, order.changedMs) / 1000.0f, mob->GetName(), bot->GetExactDist(mob),
             bot->IsWithinMeleeRange(mob) ? " (in melee)" : "");
    return true;
}

bool ReachMeleeAction::Execute(Event event)
{
    // [lead1171] the leader's meeting verdict (isUseful): the gap closer onto / the grip of the incoming mob
    if (!_meetSpell.empty())
    {
        std::string const spell = _meetSpell;
        _meetSpell.clear();
        Unit* target = GetTarget();
        if (!target || !botAI->CastSpell(spell, target))
            return false;
        LOG_INFO("playerbots", "[lead1171] {}: meets {} ({:.1f} yd) with {}", bot->GetName(), target->GetName(),
                 bot->GetExactDist(target), spell);
        return true;
    }

    // [b46_ohb] a standing shooter nobody else sees (isUseful): the tank steps out of its sight, it comes
    if (!_sightBreak.IsEmpty())
    {
        Unit* target = GetTarget();
        bool const same = target && target->GetGUID() == _sightBreak;
        _sightBreak.Clear();
        return same && BreakShooterSight(target);
    }

    // [b32_arc] a phantom-combat target (isUseful): pulled from range, never walked into the idle pack's reach
    if (!_phantomPull.IsEmpty())
    {
        Unit* target = GetTarget();
        bool const same = target && target->GetGUID() == _phantomPull;
        _phantomPull.Clear();
        return same && PullPhantomFromRange(target);
    }

    return ReachTargetAction::Execute(event);
}

bool ReachMeleeAction::isUseful()
{
    _phantomPull.Clear();  // [b32_arc] set again below only for a phantom-combat target
    _sightBreak.Clear();   // [b46_ohb] likewise, for a standing shooter nobody else sees
    _meetSpell.clear();    // [lead1171] likewise, for the leader's meeting verdict
    if (!ReachTargetAction::isUseful())
        return false;

    Unit* target = GetTarget();
    if (!target || TankLeashHolds(botAI, bot, target))
        return false;

    // [lead146] b162 20261101-121409: after the opener at range the led tank walked "reach melee" to meet its
    // incoming mob (17 of 42 pulls >5 yd in the first 12 s, 10 of them a plain "engage at its spot"), and every
    // ranged / healer spot followed. The mob runs to the tank: the fight is where the tank took it
    // [lead1171] user 2026-11-04 (Q66): the leader's meeting verdict for that mob (PlanTankMeet) - the tank walks in
    // to meet it (the walk below), or casts its gap closer / grip (Execute); a stand verdict or none keeps the wait
    Leader::Fight::TankMeet meet = Leader::Fight::TankMeet::None;
    if (PlayerbotAI::IsTank(bot) && Leader::LedBot(bot) && Leader::Fight::MobComesToTank(bot, target))
    {
        std::string meetSpell;
        meet = Leader::Fight::TankMeetFor(bot, target, &meetSpell);
        if ((meet == Leader::Fight::TankMeet::Closer || meet == Leader::Fight::TankMeet::Grip) &&
            !meetSpell.empty() && botAI->CanCastSpell(meetSpell, target) &&
            // [lead1591] the gap closer's own ground and door guard (a charge through a shut gate)
            (meet != Leader::Fight::TankMeet::Closer || !GapCloserLeavesGround(bot, target, meetSpell.c_str())))
        {
            _meetSpell = meetSpell;
            return true;
        }
    }
    if (meet != Leader::Fight::TankMeet::Walk && PlayerbotAI::IsTank(bot) && Leader::LedBot(bot) &&
        Leader::Fight::MobComesToTank(bot, target))
    {
        if (float const left = StopOwnWalkToMob(botAI, bot, target); left > 0.0f)
            LOG_INFO("playerbots", "[lead792] {}: stops its walk to {} ({:.1f} yd, low {}, {:.1f} yd of it left): it "
                     "comes to the tank", bot->GetName(), target->GetName(), bot->GetExactDist(target),
                     target->GetGUID().GetCounter(), left);
        thread_local std::set<std::pair<uint64, uint64>> waitLogged;  // [lead1041] one line per tank and mob
        if (!target->isMoving() && waitLogged.insert({bot->GetGUID().GetRawValue(), target->GetGUID().GetRawValue()})
                                       .second)
            LOG_INFO("playerbots", "[lead1041] {}: waits for {} ({:.1f} yd): on the tank, its chase not begun yet",
                     bot->GetName(), target->GetName(), bot->GetExactDist(target));
        return false;
    }
    // [lead760] the fight stands where the leader planned it after the tank's opener: no walk to a mob away from the
    // plan's fight spot back, to a mob a ready taunt reaches from here, to a caster on the tank at range while it
    // holds other mobs in its melee (TankWalkMovesFight)
    if (PlayerbotAI::IsTank(bot) && bot->IsInCombat() && Leader::LedBot(bot))
        if (std::string const moves = Leader::Fight::TankWalkMovesFight(bot, target); !moves.empty())
        {
            // [lead792] ... and its walk to the mob under way ends
            if (float const left = StopOwnWalkToMob(botAI, bot, target); left > 0.0f)
                LOG_INFO("playerbots", "[lead792] {}: stops its walk to {} ({:.1f} yd, low {}, {:.1f} yd of it left): "
                         "{}", bot->GetName(), target->GetName(), bot->GetExactDist(target),
                         target->GetGUID().GetCounter(), left, moves);
            thread_local std::unordered_map<ObjectGuid, uint32> stillLog;
            uint32 const now = getMSTime();
            uint32& last = stillLog[bot->GetGUID()];
            if (!last || getMSTimeDiff(last, now) >= TANK_REACH_LOG_MS)
            {
                last = now ? now : 1;
                LOG_INFO("playerbots", "[lead760] {}: no walk to {} ({:.1f} yd, low {}): {}", bot->GetName(),
                         target->GetName(), bot->GetExactDist(target), target->GetGUID().GetCounter(), moves);
            }
            return false;
        }

    // [reach_idle_waits_ready] The tank's walk in to a mob that fights nobody yet is a pull: it waits while the group
    // still gets ready (a member buffing, a sitter below its line) and, for a boss, until the group is ready for
    // him (PullReadiness::BossNotReady). sh-h w24 (20261025-115805-sh-h-5A-37, 1640 s): "reach melee" walked onto
    // Warchief Kargath Bladefist 9 s into "buffing: before Warchief Kargath Bladefist", the group at 69 % mana.
    // [lead531] b212 rfc-h 5F 621.6-691 s: the leader's engage order on Taragaman (its boss gate passed) was refused
    // here 70 s ("Rthunac at 69 % mana (needs 74 %)"), order_orphan: a led tank's walk to the mob of the leader's
    // open order is the leader's pull - its readiness is the leader's check before it issues, never the tank's veto
    Leader::PullOrder ordered;
    bool const leaderPull = Leader::LedBot(bot) && Leader::Orders::GetOpen(bot->GetGUID(), ordered) &&
                            ordered.target == target->GetGUID();
    if (PlayerbotAI::IsTank(bot) && botAI->IsInNonRaidDungeon() && !bot->IsInCombat() && !target->IsInCombat() &&
        !leaderPull)
    {
        std::string why;
        Creature* creature = target->ToCreature();
        bool const boss = creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
        bool held = PullReadiness::GroupPreparing(bot, &why);
        if (!held && boss)
        {
            why = PullReadiness::BossNotReady(bot, target);
            held = !why.empty();
        }

        if (held)
        {
            thread_local std::unordered_map<ObjectGuid, uint32> heldLog;
            uint32 const now = getMSTime();
            uint32& last = heldLog[bot->GetGUID()];
            if (!last || getMSTimeDiff(last, now) >= TANK_REACH_LOG_MS)
            {
                last = now ? now : 1;
                LOG_INFO("playerbots", "[reach_idle_waits_ready] {}: no walk to idle {} ({:.1f} yd): {}; target "
                         "dropped, back to the non-combat engine", bot->GetName(), target->GetName(),
                         bot->GetExactDist(target), why);
            }

            // [reach_idle_release] build 27 P0 (ac-n-5A-55 Shirrak, ub-n-5A-29 Hungarfen, strat-h-5A-10
            // Hearthsinger): refusing the walk alone left the tank in the combat engine out of combat with the idle
            // boss as its swing and current target ("melee (5.00) -> boss" every tick). The combat engine neither
            // drinks nor buffs nor runs the test runner's move, so the group never got ready: a deadlock (Rlpalba at
            // 0 % mana for 219 s). The idle mob is let go as DropTargetAction does: the non-combat engine rests and
            // buffs, and the tank's pick ("aggressive target", "tank target") takes it again once the group is ready.
            bot->AttackStop();
            bot->SetTarget(ObjectGuid::Empty);
            bot->SetSelection(ObjectGuid::Empty);
            context->GetValue<Unit*>("current target")->Set(nullptr);
            botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
            return false;
        }
    }

    // [idle_pack_no_los] A melee damage dealer does not run 20-34 yd after a mob standing in the aggro reach of an
    // idle pack either (wipe analysis w24-25, Scholomance): the tank's taunts bring it, the melee waits for it.
    if (!PlayerbotAI::IsTank(bot) && botAI->IsInNonRaidDungeon() && target->IsInCombat() &&
        !bot->IsWithinDistInMap(target, TANK_FAR_MOB))
    {
        Creature* creature = target->ToCreature();
        if (creature && !creature->IsDungeonBoss() && IdleMobReachingSpot(botAI, bot, target))
            return false;
    }

    Creature* idle = TankWalkIntoIdlePack(botAI, bot, target);
    if (!idle)
    {
        TankReachRefusals().erase(bot->GetGUID());
        return true;
    }

    uint32 const now = getMSTime();
    // [tank_reach_drop_idle_target] A refused target that fights nobody of the group never comes to the tank: the
    // bot stood in its combat engine on it for good. arc-n 2026-10-25 (20261025-115805-arc-n-5A-14, 735-894 s): a
    // Defender Corpse (21303, a rooted trap that bursts when touched) stayed "in combat" on nobody after its neighbour
    // burst; "no walk to Defender Corpse ... aggro reach of idle Eredar Soul-Eater" every 5 s for 150 s, the planner
    // (non-combat) never looked at the Soul-Eater that blocked Dalliah. After TANK_REACH_DROP_MS the target is
    // dropped and the pull planner takes the way on.
    {
        Unit* victim = target->GetVictim();
        Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        bool const onGroup = victimPlayer && (victimPlayer == bot || victimPlayer->IsInSameGroupWith(bot));

        // [b46_ohb] ohb-h 20261026-093620-ohb-h-5E-16 (1381-2035 s, stopped as a hung fight): a Durnholde Rifleman
        // 17820 (SmartAI Shoot with SMARTCAST_COMBAT_MOVE: it stands while it sees its victim) shot the Prot Paladin
        // from 22 yd, 22.3 yd from an idle Durnholde Veteran; "[tank reach] no walk ... aggro reach of idle
        // Durnholde Veteran" x433, the priest, warlock and hunter had no line of sight from their spot ("sight-fail
        // ... runs into the melee zone or past unsafe spots"), so the fight went on for 11 min. A mob on the tank
        // resets the drop clock below every tick, so nothing ended it. A player steps back round the corner, out of
        // the shooter's sight: it walks after him, away from the idle pack, and the group kills it there.
        if (ShooterOutOfGroupSight(bot, target))
        {
            if (_shooter != target->GetGUID())
            {
                _shooter = target->GetGUID();
                _shooterSinceMs = now ? now : 1;
            }
            else if (getMSTimeDiff(_shooterSinceMs, now) >= SHOOTER_SIGHT_BREAK_MS &&
                     (!_sightBreakFailMs || getMSTimeDiff(_sightBreakFailMs, now) >= SHOOTER_SIGHT_RETRY_MS))
            {
                _shooterSinceMs = now ? now : 1;  // the next try only if it stands again for as long
                _sightBreak = target->GetGUID();
                return true;
            }
        }
        else
            _shooter.Clear();

        auto& refusal = TankReachRefusals()[bot->GetGUID()];
        if (refusal.target != target->GetGUID() || onGroup)
        {
            refusal.target = target->GetGUID();
            refusal.sinceMs = now ? now : 1;
        }
        else if (getMSTimeDiff(refusal.sinceMs, now) >= TANK_REACH_DROP_MS && IsPhantomCombatTarget(bot, target))
        {
            // [b32_arc] The drop is no way on for a mob that holds the bot in combat on nobody: [b30_stalls]
            // (AttackersValue::AddCombatOnlyRefs) keeps it an attacker, so the bot stays on its combat engine, the
            // planner (non-combat) never runs and "tank assist" takes it again at once. arc-h 20261025-221216-arc-h-
            // 5E-39 (619-801 s): an Eredar Deathbringer (spawn 138944, 301.8,127.4) in combat with all five members
            // at 26-34 yd, 100 % health, no victim; "no walk ... aggro reach of idle Eredar Soul-Eater (21.1 yd)",
            // "drops Eredar Deathbringer ... left to the pull planner" and "tank assist -> Eredar Deathbringer" in a
            // loop for 180 s, "trash fight idle". Same spot: arc-n-5A-56, arc-h-5F-53, arc-n-5D-46 (today). A player
            // pulls it to himself from where he stands: the taunt / ranged threat spell gives it a victim (it walks
            // to the tank, away from the Soul-Eater), or it cannot reach him and evades, which ends the phantom combat.
            _phantomPull = target->GetGUID();
            return true;
        }
        else if (getMSTimeDiff(refusal.sinceMs, now) >= TANK_REACH_DROP_MS)
        {
            LOG_INFO("playerbots",
                     "[tank_reach_drop_idle_target] {}: drops {} ({:.1f} yd, fights nobody of the group): refused "
                     "{} s (aggro reach of idle {}), left to the pull planner",
                     bot->GetName(), target->GetName(), bot->GetExactDist(target),
                     getMSTimeDiff(refusal.sinceMs, now) / IN_MILLISECONDS, idle->GetName());
            TankReachRefusals().erase(bot->GetGUID());
            bot->AttackStop();
            bot->SetSelection(ObjectGuid::Empty);
            context->GetValue<Unit*>("current target")->Set(nullptr);
            return false;
        }
    }

    thread_local std::unordered_map<ObjectGuid, uint32> logged;
    uint32& last = logged[bot->GetGUID()];
    if (!last || getMSTimeDiff(last, now) >= TANK_REACH_LOG_MS)
    {
        last = now ? now : 1;
        Unit* victim = target->GetVictim();
        LOG_INFO("playerbots",
                 "[tank reach] {}: no walk to {} ({:.1f} yd, on {}): its spot is in the aggro reach of idle {} "
                 "({:.1f} yd from it)",
                 bot->GetName(), target->GetName(), bot->GetExactDist(target), victim ? victim->GetName() : "nobody",
                 idle->GetName(), idle->GetExactDist(target));
    }

    return false;
}

bool ReachMeleeAction::GuardsTankWalk()
{
    Unit* target = GetTarget();
    // [b39_hos] not the walk to a mob that cannot reach its victim (TankWalkIntoIdlePack lets it through): the path
    // guard would refuse the same idle pack and the evading mob would stay unkillable.
    Creature* creature = target ? target->ToCreature() : nullptr;
    if (creature && creature->CanNotReachTarget() && !creature->IsDungeonBoss())
        return false;

    return target && !bot->IsWithinDistInMap(target, TANK_FAR_MOB);
}

bool ReachHealerAttackerAction::isUseful()
{
    if (!PlayerbotAI::IsTank(bot) || !ReachTargetAction::isUseful())
        return false;

    // Scholomance heroic: the Neophytes cast on the healer from 20-35 yd, among the hall's next packs; walking to
    // them took 11 mobs to 21 in three walks.
    Unit* target = GetTarget();
    if (!target || SpotWakesIdlePack(botAI, bot, target))
        return false;

    // [b32_peel] only to a mob at the healer (the walk ends at the healer); a caster hitting it from range is taunted
    // or pulled in, not chased across the room
    if (!TankPeel::MayCloseGap(bot, target))
        return false;

    // [reach_healer_taunt_first] A taunt or a ranged threat spell takes the mob from here ("taunt reaction" in combat,
    // the opener of "tank defend on attacked group member" out of it): no walk while one is ready. The walk itself is
    // path-guarded, in and out of combat (GuardsIdleTankWalk).
    std::string const ready = TauntReactionAction::ReadyFor(botAI, target);
    if (!ready.empty())
    {
        uint32 const now = getMSTime();
        if (!_tauntLogMs || getMSTimeDiff(_tauntLogMs, now) >= TAUNT_LOG_MS)
        {
            _tauntLogMs = now ? now : 1;
            LOG_DEBUG("playerbots", "[reach healer] {}: no walk to {} ({:.1f} yd), {} is ready", bot->GetName(),
                      target->GetName(), bot->GetExactDist(target), ready);
        }
        return false;
    }

    return true;
}

bool ReachGroupMemberAttackerAction::isUseful()
{
    if (!PlayerbotAI::IsTank(bot) || sPlayerbotAIConfig.TankPeelWalkRange <= 0.0f || !ReachTargetAction::isUseful())
        return false;

    Unit* target = GetTarget();
    if (!target || !bot->IsWithinDistInMap(target, sPlayerbotAIConfig.TankPeelWalkRange))
        return false;

    // [b32_peel] only to a mob next to the member it hits, not to a caster hitting from range
    if (!TankPeel::MayCloseGap(bot, target))
        return false;

    // a boss the tank holds stays where its encounter wants it: the taunts still reach the mob
    Unit* current = AI_VALUE(Unit*, "current target");
    Creature* currentCreature = current ? current->ToCreature() : nullptr;
    if (currentCreature && currentCreature->IsDungeonBoss() && current->GetVictim() == bot)
        return false;

    // nor into the aggro range of an idle pack next to the mob: the peel must not pull the next pack
    return !SpotWakesIdlePack(botAI, bot, target);
}

Unit* MoveToTankAction::GetTarget()
{
    RangedPositionInfo const info = AI_VALUE(RangedPositionInfo, "group ranged position");
    return info.chased ? botAI->GetUnit(info.tank) : nullptr;
}

bool MoveToTankAction::Execute(Event /*event*/)
{
    Unit* tank = GetTarget();
    bool const wasMoving = bot->isMoving();
    if (!tank || !ReachCombatTo(tank, distance))
        return false;

    // [run_to_tank] the run into the tank's AoE (Consecration / Thunder Clap / Death and Decay) with mobs on the bot
    if (!wasMoving)
    {
        uint32 onBot = 0;
        for (Unit* attacker : bot->getAttackers())
            if (attacker && attacker->IsAlive() && attacker->GetVictim() == bot)
                ++onBot;
        LOG_INFO("playerbots", "[run to tank] {}: {} mobs on it, to {} {:.1f} yd away", bot->GetName(), onBot,
                 tank->GetName(), bot->GetExactDist(tank));
    }

    NoteHealWalk(botAI);
    return true;
}

bool MoveToTankAction::isUseful()
{
    if (!sPlayerbotAIConfig.RangedFleeToTank || PlayerbotAI::IsTank(bot) || !ReachTargetAction::isUseful())
        return false;

    Unit* tank = GetTarget();
    if (!tank || tank == bot || !tank->IsAlive() || tank->GetMapId() != bot->GetMapId() || botAI->IsInVehicle())
        return false;

    // Round a corner on the same floor the tank is a refuge too, and the more so as its taunts cannot see the mob:
    // Stockade 2026-10-24, the rogue's three mobs stayed "no line of sight" for the tank 23 yd away until it died.
    constexpr float UNSEEN_REFUGE_MAX = 30.0f;
    constexpr float UNSEEN_REFUGE_MAX_HEIGHT = 5.0f;
    return bot->IsWithinLOSInMap(tank) ||
           (bot->GetExactDist(tank) <= UNSEEN_REFUGE_MAX &&
            std::fabs(bot->GetPositionZ() - tank->GetPositionZ()) <= UNSEEN_REFUGE_MAX_HEIGHT);
}

bool CastReachTargetSpellAction::isUseful()
{
    // do not move while staying
    if (botAI->HasStrategy("stay", botAI->GetState()))
    {
        return false;
    }

    return ServerFacade::instance().IsDistanceGreaterThan(AI_VALUE2(float, "distance", "current target"),
                                                (distance + sPlayerbotAIConfig.ContactDistance));
}

namespace
{
    // In range of the target but a wall in between: the reach walks round the corner instead of standing there.
    // Only for a target already fighting (a step that sees an idle mob can pull it) or a group member.
    bool InRangeOutOfSight(Player* bot, Unit* target, float distance)
    {
        if (!target || target == bot || target->GetMap() != bot->GetMap() ||
            !bot->IsWithinCombatRange(target, distance))
            return false;

        if (!target->IsPlayer() && !target->IsInCombat())
            return false;

        return !bot->IsWithinLOSInMap(target);
    }

    bool MayReach(PlayerbotAI* botAI, Player* bot)
    {
        return !botAI->HasStrategy("stay", botAI->GetState()) && !bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    }
}

ReachSpellAction::ReachSpellAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach spell", botAI->GetRange("spell"))
{
}

bool ReachSpellAction::isUseful()
{
    // [b32_engage] Read again: a hunter's range is its weapon's (PlayerbotAI::GetHunterWeaponRange), which the
    // constructor may have read before the bot had one.
    distance = botAI->GetRange("spell");
    if (ReachTargetAction::isUseful())
    {
        // [b32_engage] A mob on its way to the group comes into range by itself: a ranged dps stays and shoots
        // instead of walking out to meet it (engage_delay.md: the hunters' stop-start crawl to a running mob).
        return PlayerbotAI::IsHeal(bot) || PlayerbotAI::IsTank(bot) || !PlayerbotAI::IsRanged(bot) ||
               !TargetComesIntoRange(bot, GetTarget(), distance);
    }

    // Melee and tanks come into melee anyway; a healer's target is a side job (EnemyOutOfSpellRangeTrigger).
    if (!PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot) || !MayReach(botAI, bot))
        return false;

    return InRangeOutOfSight(bot, GetTarget(), distance);
}

bool ReachSpellAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, GetTargetName());
    if (InRangeOutOfSight(bot, target, distance))
        return MoveToSight(target, distance, "sight: spell target round the corner");

    return ReachTargetAction::Execute(event);
}

ReachPartyMemberToHealAction::ReachPartyMemberToHealAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach party member to heal", botAI->GetRange("heal"))
{
}

std::string const ReachPartyMemberToHealAction::GetTargetName() { return "party member to heal"; }

// "party member to heal out of spell range" fires on a lost line of sight too; the plain reach did nothing inside
// heal range (ReachCombatTo returns false there), so the healer stood behind the corner with a heal it could not
// cast (Stockade 2026-10-24).
bool ReachPartyMemberToHealAction::isUseful()
{
    if (ReachTargetAction::isUseful())
        return true;

    return MayReach(botAI, bot) && InRangeOutOfSight(bot, GetTarget(), distance);
}

bool ReachPartyMemberToHealAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, GetTargetName());
    if (InRangeOutOfSight(bot, target, distance))
    {
        if (!MoveToSight(target, distance, "sight: heal target round the corner"))
            return false;

        NoteHealWalk(botAI);
        return true;
    }

    return ReachTargetAction::Execute(event);
}

ReachPartyMemberToResurrectAction::ReachPartyMemberToResurrectAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach party member to resurrect", botAI->GetRange("spell"))
{
}

std::string const ReachPartyMemberToResurrectAction::GetTargetName() { return "party member to resurrect"; }

namespace
{
    // RFC 2026-10-24 18:28: the tank died round a corner among a Searing Blade pack; the paladin stood 12-19 yd from
    // the corpse out of its sight ("redemption ... no line of sight") and the follow guard stopped every walk toward
    // it ("would wake Searing Blade Enforcer"): the tank lay there 3.5 minutes. User: «Почему хил не ресает танка, не
    // выйдет в зону видимости, не попав в агр, и не реснув танка?»
    constexpr float RES_REACH_MARGIN = 5.0f;   // beyond the guard's 2 yd: the walk keeps clear of every reach
    constexpr float RES_RANGE_INSIDE = 2.0f;   // the spot stands this far inside the spell's range
    constexpr float RES_PATH_STEP = 2.0f;      // walks are sampled this often
    constexpr float RES_MAX_WALK = 80.0f;      // the path toward the corpse is sampled this far
    constexpr float RES_RING_INSIDE = 8.0f;    // the inner ring of candidate spots
    constexpr float RES_RING_STEP = static_cast<float>(M_PI) / 6.0f;
    constexpr uint32 RES_RING_TURNS = 3;       // ring spots up to 90 degrees either side of the group's direction
    constexpr uint32 RES_RING_PATHS = 6;       // ring spots given a path, nearest first
    constexpr float RES_SPOT_SNAP_UP = 5.0f;
    constexpr float RES_SPOT_SNAP_RANGE = 15.0f;
    constexpr float RES_SPOT_MAX_DZ = 8.0f;
    constexpr float RES_PATH_END = 3.0f;       // a path ending farther from its spot does not get there
    constexpr float RES_AT_SPOT = 1.5f;
    constexpr uint32 RES_SPOT_RECHECK_MS = 2 * IN_MILLISECONDS;
    constexpr uint32 RES_LOG_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 CORPSE_RES_FRESH_MS = 5 * IN_MILLISECONDS;

    struct ResReach
    {
        Creature* mob;
        Unit* walker;  // the resurrecter, for the core's Z gate per point
        float reach;
    };

    // The idle aggressive hostiles a resurrecter must not come near: the path guard's set and reach
    // (MovementAction::GuardIdleMobPath) with a wider margin and no line of sight exemption, so the guard lets the
    // chosen walk pass. A mob the bot stands in the reach of already blocks only coming nearer, as in the guard.
    // [b33_move] ... and the corpse's own surroundings: OHB heroic 2026-10-26 (20261026-000434-ohb-h-5H-20) the tank,
    // ~106 yd off, found "ring spot on the group's side (106 yd walk)" with no blocker at all - the healer lay inside
    // unpulled Durnholde territory past the tank's sight distance ("possible targets" is the bot's surroundings
    // only), while the nearer cat saw the same walk enter a Rifleman's reach. The tank walked in alone. The mobs round
    // the corpse (RES_CORPSE_SCAN, its spell range plus the widest reach) are judged too, so a rez spot and its walk
    // keep RES_REACH_MARGIN clear of every idle pack there, or the search is BLOCKED (wait / the planner clears it).
    constexpr float RES_CORPSE_SCAN = 70.0f;

    std::vector<ResReach> CollectResReaches(PlayerbotAI* botAI, Player* bot, Unit* corpse)
    {
        std::vector<ResReach> reaches;
        GuidVector const possible =
            botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
        std::vector<Creature*> mobs;
        for (ObjectGuid const& guid : possible)
            if (Creature* mob = botAI->GetCreature(guid))
                mobs.push_back(mob);

        if (corpse && corpse->IsInWorld() && corpse->GetMap() == bot->GetMap())
        {
            std::list<Unit*> nearby;
            Acore::AnyUnitInObjectRangeCheck check(corpse, RES_CORPSE_SCAN);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(corpse, nearby, check);
            Cell::VisitObjects(corpse, searcher, RES_CORPSE_SCAN);
            for (Unit* unit : nearby)
                if (Creature* mob = unit ? unit->ToCreature() : nullptr)
                    if (std::find(mobs.begin(), mobs.end(), mob) == mobs.end())
                        mobs.push_back(mob);
        }

        for (Creature* mob : mobs)
        {
            if (!mob || !mob->IsInWorld() || !mob->IsAlive() || mob->IsInCombat() || mob->GetMap() != bot->GetMap() ||
                !bot->IsHostileTo(mob) || !mob->HasReactState(REACT_AGGRESSIVE) || mob->IsCivilian() ||
                mob->IsNeutralToAll())
                continue;

            float reach = MovementAction::IdleAggroReach(mob, bot, RES_REACH_MARGIN);
            reach = std::min(reach, mob->GetExactDist(bot) - 1.0f);
            if (reach > 0.0f)
                reaches.push_back({ mob, bot, reach });
        }

        return reaches;
    }

    Creature* InResReach(std::vector<ResReach> const& reaches, G3D::Vector3 const& point)
    {
        for (ResReach const& idle : reaches)
            if (idle.mob->GetExactDist(point.x, point.y, point.z) < idle.reach &&
                MovementAction::PassesAggroZGate(idle.mob, idle.walker, point.z))
                return idle.mob;

        return nullptr;
    }

    // The first mob whose reach the walk along `points` enters (sampled every RES_PATH_STEP), and the walk's length.
    Creature* WalkWakes(Movement::PointsArray const& points, std::vector<ResReach> const& reaches, float& length)
    {
        length = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const from = points[i - 1];
            G3D::Vector3 const to = points[i];
            float const segment = (to - from).length();
            for (float walked = 0.0f; walked < segment; walked += RES_PATH_STEP)
                if (Creature* mob = InResReach(reaches, from + (to - from) * (walked / segment)))
                    return mob;

            length += segment;
        }

        return points.empty() ? nullptr : InResReach(reaches, points.back());
    }

    struct CorpseResNote
    {
        uint32 firstBlockedMs = 0;
        uint32 lastBlockedMs = 0;
        uint32 lastFoundMs = 0;
    };

    std::mutex corpseResMutex;
    std::unordered_map<uint64, CorpseResNote> corpseResNotes;
}

ReachPartyMemberToResurrectAction::SpotSearch ReachPartyMemberToResurrectAction::FindSpot(Unit* corpse)
{
    uint32 const now = getMSTime();
    if (_spotMs && _spotFor == corpse->GetGUID() && getMSTimeDiff(_spotMs, now) < RES_SPOT_RECHECK_MS)
        return _search;

    _spotMs = now ? now : 1;
    _spotFor = corpse->GetGUID();
    _search = SpotSearch::NONE;
    _spotReason.clear();

    std::vector<ResReach> const reaches = CollectResReaches(botAI, bot, corpse);  // [b33_move]
    float const range = std::max(1.0f, distance - RES_RANGE_INSIDE);
    G3D::Vector3 const corpsePos(corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
    Creature* blocker = nullptr;

    // The path toward the corpse: its first point that sees the corpse in range, the farthest from it.
    BotPath const path = FindBotPath(bot, corpsePos.x, corpsePos.y, corpsePos.z);  // [lead109]
    Movement::PointsArray const& points = path.points;
    // [b47_plan] A spot past a closed door is no spot: the bot's own walk stops 2 yd before the door ([closed door]
    // -> stay) and the cast then fails on the door's model. strat-h 20261026-104908-strat-h-5B-2: the tank died on
    // the far side of the Service Entrance Gate 175368 (161 s); paladin, priest and shaman "resurrect Rtwaraa from
    // ... 1.4 yd: first spot of the walk that sees the corpse" every few seconds for 170 s, each "redemption on
    // Rtwaraa 0.4 yd - no line of sight", and the corpse waited for them for good. The walk is cut at the door; with
    // no spot left the search is BLOCKED (named for the door), so the dead releases and comes back honestly.
    GameObject* door = nullptr;
    float doorWalk = 0.0f;
    if (points.size() >= 2)
    {
        ClosedDoorHit const hit = PathThroughClosedDoor(bot, std::vector<G3D::Vector3>(points.begin(), points.end()));
        if (hit.Found())
        {
            door = hit.door;
            doorWalk = hit.walked;
        }
    }
    if ((path.type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) && points.size() >= 2)
    {
        float walkedTotal = 0.0f;
        bool done = false;
        for (size_t i = 1; i < points.size() && !done && walkedTotal <= RES_MAX_WALK; ++i)
        {
            G3D::Vector3 const from = points[i - 1];
            G3D::Vector3 const to = points[i];
            float const segment = (to - from).length();
            if (segment < 0.1f)
                continue;

            float walked = 0.0f;
            while (walked < segment)
            {
                walked = std::min(walked + RES_PATH_STEP, segment);
                G3D::Vector3 const point = from + (to - from) * (walked / segment);
                if (door && walkedTotal + walked >= doorWalk - 1.0f)  // [b47_plan] the door ends the walk
                {
                    done = true;
                    break;
                }

                if (Creature* mob = InResReach(reaches, point))
                {
                    blocker = mob;
                    done = true;
                    break;
                }

                if ((point - corpsePos).length() <= range && corpse->IsWithinLOS(point.x, point.y, point.z))
                {
                    _search = SpotSearch::FOUND;
                    _spot = point;
                    _spotWalk = walkedTotal + walked;
                    _spotReason = "first spot of the walk that sees the corpse";
                    return _search;
                }
            }

            walkedTotal += segment;
        }
    }

    // A spot on the range ring round the corpse on the group's side (toward the resurrecter) whose whole walk wakes
    // nothing: the outer ring and the spots nearest to the bot first, the shortest walk kept.
    struct RingSpot
    {
        G3D::Vector3 point;
        float fromBot;
    };
    std::vector<RingSpot> ring;
    float const toGroup = corpse->GetAngle(bot);
    for (float const radius : { range, std::max(1.0f, range - RES_RING_INSIDE) })
    {
        for (uint32 turn = 0; turn <= RES_RING_TURNS; ++turn)
        {
            for (float const side : { 1.0f, -1.0f })
            {
                if (!turn && side < 0.0f)
                    continue;

                float const angle = toGroup + side * turn * RES_RING_STEP;
                float const x = corpsePos.x + std::cos(angle) * radius;
                float const y = corpsePos.y + std::sin(angle) * radius;
                float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, corpsePos.z + RES_SPOT_SNAP_UP,
                                                         true, RES_SPOT_SNAP_RANGE);
                if (z <= INVALID_HEIGHT || std::fabs(z - corpsePos.z) > RES_SPOT_MAX_DZ)
                    continue;

                G3D::Vector3 const point(x, y, z);
                if (Creature* mob = InResReach(reaches, point))
                {
                    blocker = blocker ? blocker : mob;
                    continue;
                }

                if (!corpse->IsWithinLOS(x, y, z))
                    continue;

                ring.push_back({ point, bot->GetExactDist(x, y, z) });
            }
        }
    }

    std::sort(ring.begin(), ring.end(),
              [](RingSpot const& left, RingSpot const& right) { return left.fromBot < right.fromBot; });

    float bestWalk = 0.0f;
    uint32 paths = 0;
    for (RingSpot const& candidate : ring)
    {
        if (++paths > RES_RING_PATHS)
            break;

        BotPath const ringPath =  // [lead109]
            FindBotPath(bot, candidate.point.x, candidate.point.y, candidate.point.z);
        Movement::PointsArray const& walk = ringPath.points;
        if (ringPath.type != PATHFIND_NORMAL || walk.size() < 2 ||
            (walk.back() - candidate.point).length() > RES_PATH_END)
            continue;

        float length = 0.0f;
        if (Creature* mob = WalkWakes(walk, reaches, length))
        {
            blocker = blocker ? blocker : mob;
            continue;
        }

        // [b47_plan] nor a ring spot whose walk runs through a closed door
        if (ClosedDoorHit const hit = PathThroughClosedDoor(bot, std::vector<G3D::Vector3>(walk.begin(), walk.end()));
            hit.Found())
        {
            door = door ? door : hit.door;
            continue;
        }

        if (_search != SpotSearch::FOUND || length < bestWalk)
        {
            _search = SpotSearch::FOUND;
            _spot = walk.back();
            _spotWalk = length;
            bestWalk = length;
        }
    }

    if (_search == SpotSearch::FOUND)
    {
        _spotReason = blocker ? Acore::StringFormat("ring spot on the group's side, the walk round the corner enters "
                                                    "{}'s reach", blocker->GetName())
                              : "ring spot on the group's side";
        return _search;
    }

    if (!reaches.empty())
    {
        _search = SpotSearch::BLOCKED;
        _spotReason = blocker ? Acore::StringFormat("every spot in sight of the corpse or its walk is in {}'s reach",
                                                    blocker->GetName())
                              : "no spot in sight of the corpse within range";
    }
    // [b47_plan] the corpse lies behind a closed door the bot cannot walk through: no spot either
    if (door && _search != SpotSearch::FOUND && !bot->IsWithinLOSInMap(corpse))
    {
        _search = SpotSearch::BLOCKED;
        _spotReason = Acore::StringFormat("[b47_plan] the corpse lies behind the closed door {} {}", door->GetName(),
                                          door->GetEntry());
    }

    return _search;
}

bool ReachPartyMemberToResurrectAction::isUseful()
{
    if (ReachTargetAction::isUseful())
        return true;

    // In range but round a corner: the cast fails on the line of sight (ResurrectPartyMemberAction::isPossible lets
    // this prerequisite run for it).
    if (bot->IsInCombat() || !MayReach(botAI, bot))
        return false;

    Unit* target = GetTarget();
    return target && target != bot && target->GetMap() == bot->GetMap() && !bot->IsWithinLOSInMap(target);
}

bool ReachPartyMemberToResurrectAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, GetTargetName());
    if (!target || target->GetMap() != bot->GetMap())
        return false;

    // Battle resurrection: the plain reach.
    if (bot->IsInCombat())
        return ReachTargetAction::Execute(event);

    Player* dead = target->ToPlayer();
    SpotSearch const search = FindSpot(target);
    uint32 const now = getMSTime();
    bool const logDue = !_loggedMs || getMSTimeDiff(_loggedMs, now) >= RES_LOG_MS;
    if (search == SpotSearch::FOUND)
    {
        if (dead)
            CorpseRes::Note(dead, false);

        if (logDue || (_spot - _loggedSpot).length() > RES_PATH_STEP)
        {
            LOG_INFO("playerbots",
                     "[corpse res] {} resurrects {} from ({:.1f},{:.1f},{:.1f}) {:.1f} yd: {} ({:.0f} yd walk)",
                     bot->GetName(), target->GetName(), _spot.x, _spot.y, _spot.z,
                     target->GetExactDist(_spot.x, _spot.y, _spot.z), _spotReason, _spotWalk);
            _loggedMs = now ? now : 1;
            _loggedSpot = _spot;
        }

        if (bot->GetExactDist(_spot.x, _spot.y, _spot.z) <= RES_AT_SPOT)
            return false;

        // Already on the way there: the walk is kept.
        LastMovement const& last = *context->GetValue<LastMovement&>("last movement");
        if (bot->isMoving() &&
            G3D::Vector3(last.lastMoveToX - _spot.x, last.lastMoveToY - _spot.y, last.lastMoveToZ - _spot.z).length() <=
                RES_AT_SPOT)
            return true;

        if (!MoveTo(target->GetMapId(), _spot.x, _spot.y, _spot.z, false, false, false, false,
                    MovementPriority::MOVEMENT_NORMAL, true))
            return false;

        NoteMoveReason("corpse res: in sight of the corpse, out of the pack's reach");
        return true;
    }

    if (search == SpotSearch::BLOCKED)
    {
        if (dead)
            CorpseRes::Note(dead, true);

        if (logDue)
        {
            LOG_INFO("playerbots", "[corpse res] {} finds no safe spot to resurrect {} ({:.1f} yd): {} -> {} releases",
                     bot->GetName(), target->GetName(), bot->GetExactDist(target), _spotReason, target->GetName());
            _loggedMs = now ? now : 1;
            _loggedSpot = G3D::Vector3();
        }

        return false;
    }

    if (InRangeOutOfSight(bot, target, distance))
        return MoveToSight(target, distance, "sight: corpse round the corner");

    return ReachTargetAction::Execute(event);
}

namespace CorpseRes
{
    void Note(Player* dead, bool blocked)
    {
        uint32 const now = getMSTime() ? getMSTime() : 1;
        std::lock_guard<std::mutex> lock(corpseResMutex);
        CorpseResNote& note = corpseResNotes[dead->GetGUID().GetRawValue()];
        if (!blocked)
        {
            note.lastFoundMs = now;
            note.firstBlockedMs = 0;
            return;
        }

        if (!note.firstBlockedMs || getMSTimeDiff(note.lastBlockedMs, now) > CORPSE_RES_FRESH_MS)
            note.firstBlockedMs = now;

        note.lastBlockedMs = now;
    }

    bool Blocked(Player* dead, uint32 forMs)
    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> lock(corpseResMutex);
        auto const found = corpseResNotes.find(dead->GetGUID().GetRawValue());
        if (found == corpseResNotes.end())
            return false;

        CorpseResNote const& note = found->second;
        if (!note.firstBlockedMs || getMSTimeDiff(note.lastBlockedMs, now) > CORPSE_RES_FRESH_MS)
            return false;

        // A resurrecter that found a spot since is on its way.
        if (note.lastFoundMs && getMSTimeDiff(note.lastFoundMs, now) <= CORPSE_RES_FRESH_MS)
            return false;

        return getMSTimeDiff(note.firstBlockedMs, now) >= forMs;
    }
}

namespace
{
    // The spot MoveToSight last refused as unsafe on this thread (LastSightRejection).
    thread_local std::string sightRejection;
}

std::string const& LastSightRejection() { return sightRejection; }

// MovementAction helpers of the sight walks (declared in MovementActions.h; kept here, next to their users).
void MovementAction::NoteMoveReason(std::string const& reason)
{
    LastMovement& last = *context->GetValue<LastMovement&>("last movement");
    last.reason = reason;
    last.reasonMs = last.msTime;
}

// A healer in heal range of a tank that ran round a corner, a caster whose target stands behind a door frame: the
// distance checks are fine, only the wall is in the way, and ReachCombatTo does not move inside its distance. The
// nearest spot that sees the target is where the path to it comes round the corner: walk to there, not to the target
// (into the pack). User, Stockade 2026-10-24: «танк любит забегать за повороты и становится вне видимости хила».
// [b32_engage] A mob running to a group member (the tank picking the pack up) that will be within `range` of the bot
// and in its sight within COMES_INTO_RANGE_MS, if it keeps running at its speed towards its victim. O(1): one line
// of sight test.
bool MovementAction::TargetComesIntoRange(Player* bot, Unit* target, float range)
{
    constexpr float COMES_INTO_RANGE_SEC = 2.0f;
    Creature* creature = target ? target->ToCreature() : nullptr;
    Unit* victim = creature && creature->IsAlive() && creature->isMoving() ? creature->GetVictim() : nullptr;
    Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!victimPlayer || !victimPlayer->IsInSameRaidWith(bot) || creature->GetMap() != bot->GetMap())
        return false;

    float const toVictim = creature->GetExactDist(victim);
    float const step = std::min(toVictim, creature->GetSpeed(MOVE_RUN) * COMES_INTO_RANGE_SEC);
    float const k = toVictim > 0.1f ? step / toVictim : 0.0f;
    float const x = creature->GetPositionX() + (victim->GetPositionX() - creature->GetPositionX()) * k;
    float const y = creature->GetPositionY() + (victim->GetPositionY() - creature->GetPositionY()) * k;
    float const z = creature->GetPositionZ() + (victim->GetPositionZ() - creature->GetPositionZ()) * k;
    if (bot->GetExactDist(x, y, z) > range + creature->GetCombatReach())
        return false;

    return bot->IsWithinLOS(x, y, z + creature->GetCollisionHeight() * 0.5f);
}

bool MovementAction::MoveToSight(Unit* target, float range, std::string const& reason)
{
    constexpr float SIGHT_STEP = 2.0f;         // the path is sampled this often
    constexpr float CORNER_CLEARANCE = 2.5f;   // the walk ends this far past the first point that sees the target
    constexpr size_t MAX_SIGHT_CHECKS = 25;    // ~50 yd of path at most
    constexpr uint32 SIGHT_REPLAN_MS = 1500;   // a walk to a sight spot this young is kept

    sightRejection.clear();
    uint32 const now = getMSTime();
    // [b31_dg] a sight walk that fails says why (one line per bot and SIGHT_FAIL_LOG_MS): mgt-n
    // 20261025-184229-mgt-n-5B-20 (359-450 s) - the healer's "reach party member to heal" returned false every tick
    // for 90 s outside Selin's shut room door with no word of the reason. Logging only: sightRejection keeps its
    // meaning for the callers (only unsafe spots on the way).
    constexpr uint32 SIGHT_FAIL_LOG_MS = 5 * IN_MILLISECONDS;
    auto const sightFail = [&](std::string const& why)
    {
        thread_local std::unordered_map<ObjectGuid, uint32> failLogged;
        uint32& logged = failLogged[bot->GetGUID()];
        if (!logged || getMSTimeDiff(logged, now) >= SIGHT_FAIL_LOG_MS)
        {
            logged = now ? now : 1;
            LOG_INFO("playerbots", "[sight-fail] {}: {} -> no walk to see {} ({:.1f} yd, range {:.1f}): {}",
                     bot->GetName(), reason, target ? target->GetName() : std::string("nobody"),
                     target ? bot->GetExactDist(target) : 0.0f, range, why);
        }
        return false;
    };
    if (!target)
        return false;
    if (!IsMovingAllowed(target))
        return sightFail("IsMovingAllowed refused the walk (see its own hold / root / cast state)");

    LastMovement const& last = *context->GetValue<LastMovement&>("last movement");
    if (bot->isMoving() && last.msTime && getMSTimeDiff(last.msTime, now) < SIGHT_REPLAN_MS &&
        last.reasonMs == last.msTime && last.reason.compare(0, reason.size(), reason) == 0 &&
        target->GetExactDist(last.lastMoveToX, last.lastMoveToY, last.lastMoveToZ) <= range)
        return true;

    // [lead109] a led bot's way is the leader's route (BotPath.h)
    BotPath const path = FindBotPath(bot, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    if (!(path.type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT)))
        return sightFail(Acore::StringFormat("no path to the target (path type {}{})", uint32(path.type),
                                             path.leaderRoute ? ", the leader's route" : ""));

    Movement::PointsArray const& points = path.points;
    if (points.size() < 2)
        return sightFail("the path to the target has no points");

    // User 2026-10-24: «хил не должен забегать в зону мили, где кливы и массовые спеллы — это зона танка; просто
    // вбежать туда, откуда видно танка, не обязательно рядом с ним». A healer / ranged sight spot keeps clear of the
    // melee zone of the fight: every hostile in combat with the group stays farther than its reach + this margin.
    constexpr float MELEE_ZONE_MARGIN = 6.0f;
    bool const keepsClear = PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot);
    std::vector<Unit*> fighters;
    if (keepsClear)
    {
        auto addAttackers = [&fighters](Unit* unit)
        {
            if (!unit)
                return;
            for (Unit* attacker : unit->getAttackers())
                if (attacker && attacker->IsAlive() &&
                    std::find(fighters.begin(), fighters.end(), attacker) == fighters.end())
                    fighters.push_back(attacker);
        };
        addAttackers(target);
        if (target->IsAlive() && !target->IsPlayer() && target->IsInCombat())
            fighters.push_back(target);
        if (Unit* victim = target->GetVictim())
            addAttackers(victim);
    }
    auto inMeleeZone = [&](G3D::Vector3 const& spot)
    {
        for (Unit* fighter : fighters)
            if (fighter->GetExactDist(spot.x, spot.y, spot.z) < fighter->GetCombatReach() + MELEE_ZONE_MARGIN)
                return true;
        return false;
    };

    // [b32_engage] Side step first (user 2026-10-25: «нормальный игрок просто пошел бы перпендикулярно, покуда не
    // станет в зону видимости танка … на безопасном расстоянии, не по дуге в опасную зону»). scholo-h-5A-3
    // 20261025-221216 ~2694 s: the resto druid's arc walk took it from 18 to 30.6 yd from the tank, towards the
    // next pack, and a knockback nearly threw it in. A ranged / healer bot first tries short steps across the line
    // to the target (both sides, then slightly towards it): the first spot it reaches straight (no ledge, no detour)
    // that sees the target within range, no farther from it than SIDE_STEP_MAX_FARTHER more than now, outside the
    // melee zone and outside every idle mob's aggro reach by SIDE_STEP_IDLE_MARGIN (knockbacks). None: the way
    // round the corner, which walks towards the target.
    if (keepsClear)
    {
        constexpr float SIDE_STEP = 2.0f;
        constexpr float SIDE_STEP_MAX = 12.0f;
        constexpr float SIDE_STEP_TOWARD = 0.5f;       // the second try at each step leans this much towards it
        constexpr float SIDE_STEP_MAX_FARTHER = 3.0f;  // a spot at most this much farther from the target than now
        constexpr float SIDE_STEP_MAX_RISE = 3.0f;
        constexpr float SIDE_STEP_DETOUR = 1.5f;       // a walk longer than this times the straight line: no step
        constexpr float SIDE_STEP_IDLE_MARGIN = SIGHT_IDLE_AGGRO_MARGIN + 5.0f;
        constexpr size_t SIDE_STEP_MAX_PATHS = 4;
        float const dx = target->GetPositionX() - bot->GetPositionX();
        float const dy = target->GetPositionY() - bot->GetPositionY();
        float const flat = std::sqrt(dx * dx + dy * dy);
        float const nowDist = bot->GetExactDist(target);
        Map* map = bot->GetMap();
        GuidVector const idle = context->GetValue<GuidVector>("possible targets no los")->Get();
        size_t paths = 0;
        bool stepped = false;
        G3D::Vector3 stepSpot;
        for (float s = SIDE_STEP; flat > 0.5f && s <= SIDE_STEP_MAX && !stepped && paths < SIDE_STEP_MAX_PATHS;
             s += SIDE_STEP)
        {
            for (int i = 0; i < 4 && !stepped && paths < SIDE_STEP_MAX_PATHS; ++i)
            {
                float const side = (i % 2) ? -1.0f : 1.0f;
                float const toward = i < 2 ? 0.0f : SIDE_STEP_TOWARD * s;
                float x = bot->GetPositionX() + (-dy / flat) * side * s + (dx / flat) * toward;
                float y = bot->GetPositionY() + (dx / flat) * side * s + (dy / flat) * toward;
                float z = bot->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                          bot->GetPositionZ(), x, y, z, false) ||
                    std::fabs(z - bot->GetPositionZ()) > SIDE_STEP_MAX_RISE)
                    continue;

                G3D::Vector3 const at(x, y, z);
                float const dist = target->GetExactDist(x, y, z);
                if (dist > range || dist > nowDist + SIDE_STEP_MAX_FARTHER || inMeleeZone(at) ||
                    !target->IsWithinLOS(x, y, z))
                    continue;

                bool idleReach = false;
                for (ObjectGuid const guid : idle)
                {
                    Creature* mob = botAI->GetCreature(guid);
                    if (mob && mob->IsAlive() && !mob->IsInCombat() && mob->HasReactState(REACT_AGGRESSIVE) &&
                        bot->IsHostileTo(mob) &&
                        !IdleAggroBlocker(mob, bot, x, y, z, SIDE_STEP_IDLE_MARGIN, false))
                    {
                        idleReach = true;
                        break;
                    }
                }

                if (idleReach)
                    continue;

                ++paths;
                BotPath const stepPath = FindBotPath(bot, x, y, z);  // [lead109]
                Movement::PointsArray const& stepPoints = stepPath.points;
                float const straight = bot->GetExactDist(x, y, z);
                if (!(stepPath.type & PATHFIND_NORMAL) || stepPoints.size() < 2 ||
                    (stepPoints.back() - at).length() > SIGHT_STEP ||
                    stepPath.length > SIDE_STEP_DETOUR * straight + SIGHT_STEP ||
                    !UnsafeSpotReason(botAI, x, y, z, target).empty() ||
                    !IdleWakeOnWalk(stepPoints, target->GetGUID()).empty())
                    continue;

                stepped = true;
                stepSpot = at;
            }
        }

        if (stepped && MoveTo(target->GetMapId(), stepSpot.x, stepSpot.y, stepSpot.z, false, false, false, false,
                              MovementPriority::MOVEMENT_COMBAT, true))
        {
            NoteMoveReason(Acore::StringFormat("{}, side step {:.1f} yd ({:.1f} -> {:.1f} yd)", reason,
                                               bot->GetExactDist(stepSpot.x, stepSpot.y, stepSpot.z), nowDist,
                                               target->GetExactDist(stepSpot.x, stepSpot.y, stepSpot.z)));
            return true;
        }
    }


    // RFC 2026-10-24 (user: «ДД встал на зубе (скальном выступе), чуть ли не свалится»): the first spot of the way
    // that saw an Earthborer round a corner was a rock tooth 9.6 yd above the tank, and the priest and the shaman
    // stood on it. A ranged / healer sight spot must be safe to stand on (UnsafeSpotReason: footing, idle mobs, a
    // walk back to the fight); an unsafe one is passed and the walk goes on along the way, towards the target.
    constexpr size_t MAX_UNSAFE_SPOTS = 6;  // the footing and walk-back tests are dear: this many, then give up
    std::string rejected;
    size_t unsafeSpots = 0;
    auto unsafe = [&](G3D::Vector3 const& spot)
    {
        if (!keepsClear)
            return false;

        ++unsafeSpots;
        std::string const why = UnsafeSpotReason(botAI, spot.x, spot.y, spot.z, target);
        if (why.empty())
            return false;

        if (rejected.empty())
            rejected = Acore::StringFormat("rejected spot {:.1f} yd away, {:+.1f} yd high: {}",
                                           bot->GetExactDist(spot.x, spot.y, spot.z),
                                           spot.z - bot->GetPositionZ(), why);
        return true;
    };

    // The next pack, for the view of a spot (a ranged / healer bot only).
    constexpr size_t SIGHT_VIEW_PACK_MOBS = 4;
    std::vector<Creature*> const pack =
        keepsClear ? IdleMobsNear(botAI, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                  SIGHT_VIEW_PACK_MOBS)
                   : std::vector<Creature*>();

    // The spot of the way round the corner (the first that sees the target, and its neighbours), and how far it is
    // walked: the walk ends there unless the arc below finds a much shorter one.
    // [lead136] b162 20261101-121409: 22 of 42 pulls "planned (body)" - the tank's walk to its opener's range
    // ended in the idle mob's own aggro reach, the mob came before the opener. A tank's sight spot for an idle mob
    // lies outside that mob's aggro reach (Creature::GetAggroRange + 1 yd) when one exists.
    // [lead139] b164 20261101-125734 1270-1320 s: no such spot for a Searing Blade Cultist by Bazzalan - no walk at
    // all, the tank stood 55 yd off its pull for 50 s. Then the nearest in-range spot along its route (a second pass)
    Creature const* idleTarget = PlayerbotAI::IsTank(bot) && !target->IsInCombat() ? target->ToCreature() : nullptr;
    float const idleReach = idleTarget ? idleTarget->GetAggroRange(bot) + 1.0f : 0.0f;
    bool pathFound = false;
    bool pathGiveUp = false;  // the way round the corner gets into the melee zone or only past unsafe spots
    G3D::Vector3 pathSpot;
    float pathWalk = 0.0f;
    size_t checks = 0;
    float walkedBefore = 0.0f;  // the length of the path before segment i
    for (bool outOfReach : {true, false})
    {
    if (pathFound || (!outOfReach && !idleTarget))
        break;
    pathGiveUp = false;
    checks = 0;
    walkedBefore = 0.0f;
    for (size_t i = 1; i < points.size() && checks < MAX_SIGHT_CHECKS && !pathFound && !pathGiveUp; ++i)
    {
        G3D::Vector3 const from = points[i - 1];
        G3D::Vector3 const to = points[i];
        float const length = (to - from).length();
        if (length < 0.1f)
            continue;

        for (float walked = std::min(SIGHT_STEP, length); checks < MAX_SIGHT_CHECKS;
             walked = std::min(walked + SIGHT_STEP, length))
        {
            ++checks;
            G3D::Vector3 const spot = from + (to - from) * (walked / length);
            if (target->GetExactDist(spot.x, spot.y, spot.z) <= range && target->IsWithinLOS(spot.x, spot.y, spot.z) &&
                (!idleTarget || !outOfReach || target->GetExactDist(spot.x, spot.y, spot.z) > idleReach))
            {
                // The choices: a step past the corner, so a small turn of the tank does not hide it again, the first
                // spot that sees it, and for a ranged / healer bot a second step on. A ranged / healer bot takes the
                // one that sees the target best (RateSpotView: round no edge, not walled in, the next pack in view;
                // RFC 2026-10-24, user: «можно же немножко отбежать левее, чтобы видеть танка с большего угла»).
                struct Choice
                {
                    G3D::Vector3 at;
                    float score;
                    float walk;
                };
                std::array<float, 3> const steps = {CORNER_CLEARANCE, 0.0f, 2.0f * CORNER_CLEARANCE};
                std::vector<Choice> choices;
                for (float const step : steps)
                {
                    if (step > CORNER_CLEARANCE && !keepsClear)
                        continue;

                    float const further = std::min(walked + step, length);
                    if (step > 0.0f && further <= walked)
                        continue;

                    G3D::Vector3 const at = from + (to - from) * (further / length);
                    if (step > 0.0f && (target->GetExactDist(at.x, at.y, at.z) > range ||
                                        !target->IsWithinLOS(at.x, at.y, at.z)))
                        continue;

                    if (inMeleeZone(at) || std::any_of(choices.begin(), choices.end(), [&at](Choice const& c)
                                                       { return (c.at - at).length() < 0.5f; }))
                        continue;

                    choices.push_back({at, keepsClear ? RateSpotView(target, at.x, at.y, at.z, pack).Score() : 0.0f,
                                       walkedBefore + further});
                }

                // The first spot that sees the target is inside the melee zone (a tight corner right at the fight):
                // walking on only gets closer - leave it to the arc below, else to the ranged position choice.
                if (choices.empty())
                {
                    pathGiveUp = true;
                    break;
                }

                // Best view first, less SIGHT_VIEW_WALK_COST yd of walk per point ([b32_engage]: the shortest spot
                // of a similar view); of equal ones the order above (the step past the corner first).
                constexpr float SIGHT_VIEW_WALK_COST = 3.0f;
                std::stable_sort(choices.begin(), choices.end(),
                                 [](Choice const& a, Choice const& b)
                                 {
                                     return a.score - a.walk / SIGHT_VIEW_WALK_COST >
                                            b.score - b.walk / SIGHT_VIEW_WALK_COST;
                                 });
                for (Choice const& choice : choices)
                {
                    if (!unsafe(choice.at))
                    {
                        // [sight-arc] The walk there (the path up to the spot) wakes no idle pack either.
                        std::string wakes;
                        if (keepsClear)
                        {
                            Movement::PointsArray walkThere(points.begin(), points.begin() + i);
                            walkThere.push_back(choice.at);
                            wakes = IdleWakeOnWalk(walkThere, target->GetGUID());
                        }

                        if (wakes.empty())
                        {
                            pathFound = true;
                            pathSpot = choice.at;
                            pathWalk = choice.walk;
                            break;
                        }

                        if (rejected.empty())
                            rejected = Acore::StringFormat("rejected spot {:.1f} yd away: the walk there wakes idle {}",
                                                           bot->GetExactDist(choice.at.x, choice.at.y, choice.at.z),
                                                           wakes);
                    }

                    if (unsafeSpots >= MAX_UNSAFE_SPOTS)
                    {
                        pathGiveUp = true;
                        break;
                    }
                }

                if (pathFound || pathGiveUp || walked >= length)
                    break;
                continue;
            }

            if (walked >= length)
                break;
        }

        walkedBefore += length;
    }
    }

    // [sight-arc] Sunken Temple 2026-10-25 (st-n-5A-92, section C): the shortest way to a mob round a corner hugs the
    // inner wall, so its first spot that sees the mob is at the very corner - the mage and the hunter walked 15-20 yd
    // along it where a side step onto the outer arc would have shown the mob after 3-6 yd. A ranged / healer bot also
    // looks on the arc round the target at casting distance (the group's side, +-ARC_STEPS * ARC_STEP): spots that
    // see the target, out of the melee zone and safe to stand on (UnsafeSpotReason: no idle pack's aggro, footing, a
    // walk back), and takes the shortest walk of them when it is clearly shorter than the way round the corner.
    constexpr float ARC_RANGE_INSIDE = 3.0f;                      // the arc is this far inside the range
    constexpr float ARC_MAX_RADIUS = 28.0f;                       // and no farther out than this
    constexpr float ARC_MIN_RADIUS = 8.0f;                        // a shorter range leaves no arc worth the search
    // [sight-arc] rings from ARC_RING_DEPTH inside the arc radius out to it, all the way round the target
    // (MovementAction::SightArcCandidates, shared with the tank's pull spot): a side step that opens a corner.
    constexpr float ARC_RING_DEPTH = 9.0f;
    constexpr uint32 ARC_MAX_LOS = 48;    // line-of-sight tests of the candidates, nearest first
    constexpr float ARC_FLOOR = 6.0f;      // a spot farther above / below the target than this is another floor
    constexpr size_t ARC_MAX_PATHS = 3;    // the nearest candidates get a path, the others are not searched
    constexpr float ARC_PATH_END = 2.0f;   // a path that ends farther than this from its spot does not reach it
    constexpr float ARC_MIN_GAIN = 5.0f;   // the arc spot is taken when its walk is this much shorter
    bool arcFound = false;
    G3D::Vector3 arcSpot;
    float arcWalk = 0.0f;
    std::string arcRejected;  // the first arc spot refused, and why ([sight-arc] log)
    // [b32_engage] a hunter's arc is at its weapon's range (Auto Shot 35 yd), not the casters' 28
    float const arcRadius =
        std::min(range - ARC_RANGE_INSIDE, std::max(ARC_MAX_RADIUS, botAI->GetHunterWeaponRange() - ARC_RANGE_INSIDE));
    // [b32_engage] The arc is for a target out of range round a corner: a bot already in range does not walk the
    // arc (it would stand farther from the target than now, scholo-h-5A-3), and no arc spot is farther than the bot.
    float const arcNowDist = bot->GetExactDist(target);
    if (keepsClear && arcRadius >= ARC_MIN_RADIUS && arcNowDist > range && (!pathFound || pathWalk > ARC_MIN_GAIN))
    {
        struct ArcCandidate
        {
            float straight;  // the straight distance from the bot
            float angleOff;  // degrees off the group's direction from the target
            G3D::Vector3 at;
        };
        std::vector<ArcCandidate> candidates;
        for (SightArcCandidate const& spot :
             SightArcCandidates(bot, target, std::max(ARC_MIN_RADIUS, arcRadius - ARC_RING_DEPTH), arcRadius,
                                ARC_FLOOR, ARC_MAX_LOS))
        {
            G3D::Vector3 const at(spot.x, spot.y, spot.z);
            if (inMeleeZone(at) || target->GetExactDist(at.x, at.y, at.z) > arcNowDist)  // [b32_engage]
                continue;

            candidates.push_back({spot.straight, spot.angleOffDeg, at});
        }
        size_t searched = 0;
        size_t arcUnsafe = 0;
        for (ArcCandidate const& candidate : candidates)
        {
            if (searched >= ARC_MAX_PATHS || arcUnsafe >= MAX_UNSAFE_SPOTS)
                break;

            // A walk is never shorter than the straight line: nothing farther can beat the best one found.
            if ((arcFound && candidate.straight >= arcWalk) ||
                (pathFound && candidate.straight + ARC_MIN_GAIN >= pathWalk))
                break;

            ++searched;
            G3D::Vector3 const& at = candidate.at;
            BotPath const arcPath = FindBotPath(bot, at.x, at.y, at.z);  // [lead109]
            Movement::PointsArray const& arcPoints = arcPath.points;
            if (!(arcPath.type & PATHFIND_NORMAL) || arcPoints.size() < 2 ||
                (arcPoints.back() - at).length() > ARC_PATH_END)
                continue;

            float const walk = arcPath.length;
            if (arcFound && walk >= arcWalk)
                continue;

            // The spot itself (UnsafeSpotReason: an idle pack's aggro there, footing, the walk back) and the walk to
            // it (IdleWakeOnWalk: a walk round the outer arc can cut through the reach of the next pack while both
            // of its ends stand clear of it). The first refusal is logged; the others are counted in it.
            std::string why = UnsafeSpotReason(botAI, at.x, at.y, at.z, target);
            if (why.empty())
            {
                std::string const wakes = IdleWakeOnWalk(arcPoints, target->GetGUID());
                if (!wakes.empty())
                    why = "the walk there wakes idle " + wakes;
            }

            if (!why.empty())
            {
                ++arcUnsafe;
                if (arcRejected.empty())
                    arcRejected = Acore::StringFormat("{:.0f} deg off ({:.1f} yd walk): {}", candidate.angleOff,
                                                      walk, why);
                continue;
            }

            arcFound = true;
            arcSpot = at;
            arcWalk = walk;
        }
    }

    bool const takeArc = arcFound && (!pathFound || arcWalk + ARC_MIN_GAIN < pathWalk);
    // One line per bot and SIGHT_ARC_LOG_MS: the sight walk is replanned while it goes on.
    constexpr uint32 SIGHT_ARC_LOG_MS = 5 * IN_MILLISECONDS;
    thread_local std::unordered_map<ObjectGuid, uint32> arcLogged;
    auto arcLogDue = [&]()
    {
        uint32& logged = arcLogged[bot->GetGUID()];
        if (logged && getMSTimeDiff(logged, now) < SIGHT_ARC_LOG_MS)
            return false;

        logged = now ? now : 1;
        return true;
    };
    if (!takeArc && !arcRejected.empty() && arcLogDue())
        LOG_INFO("playerbots",
                 "[sight-arc] {}: {} -> no arc spot round {} (first refused: {}; the way round the corner: {})",
                 bot->GetName(), reason, target->GetName(), arcRejected,
                 pathFound ? Acore::StringFormat("{:.1f} yd", pathWalk)
                           : (rejected.empty() ? std::string("nothing") : rejected));

    if (!takeArc && !pathFound)
    {
        sightRejection = rejected;
        return sightFail(Acore::StringFormat(
            "no spot of the way ({} points sampled, {} path points{}) sees it{}{}", checks, points.size(),
            pathGiveUp ? ", the way round the corner runs into the melee zone or past unsafe spots" : "",
            rejected.empty() ? std::string() : "; " + rejected,
            arcRejected.empty() ? std::string() : "; arc: " + arcRejected));
    }

    G3D::Vector3 const chosen = takeArc ? arcSpot : pathSpot;
    if (!MoveTo(target->GetMapId(), chosen.x, chosen.y, chosen.z, false, false, false, false,
                MovementPriority::MOVEMENT_COMBAT, true))
        return sightFail(Acore::StringFormat("the walk to the sight spot ({:.1f},{:.1f},{:.1f}) was refused by {}",
                                             chosen.x, chosen.y, chosen.z, LastMoveRefusal()));

    std::string note = reason;
    if (takeArc)
    {
        note += Acore::StringFormat(", arc walk {:.1f} yd", arcWalk);
        if (arcLogDue())
            LOG_INFO("playerbots",
                     "[sight-arc] {}: {} -> arc spot {:.1f} yd from {} (walk {:.1f} yd; the way round the corner: "
                     "{}{})",
                     bot->GetName(), reason, arcRadius, target->GetName(), arcWalk,
                     pathFound ? Acore::StringFormat("{:.1f} yd", pathWalk) : std::string("nothing"),
                     arcRejected.empty() ? std::string() : "; refused first: " + arcRejected);
    }

    if (keepsClear)
        note += ", " + RateSpotView(target, chosen.x, chosen.y, chosen.z, pack).Describe();
    if (!rejected.empty())
        note += " (" + rejected + ")";
    NoteMoveReason(note);
    return true;
}
