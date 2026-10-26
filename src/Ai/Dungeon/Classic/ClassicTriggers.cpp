/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ClassicTriggers.h"
#include "ClassicShared.h"
#include "GameObject.h"
#include "MovementActions.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "TargetValue.h"
#include "TbcHazard.h"
#include "TestRunnerStrategy.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

namespace ScarletMonastery
{
namespace
{
constexpr float LOS_EYE_HEIGHT = 2.0f;
constexpr uint32 MOGRAINE_CACHE_MS = 1000;
constexpr float MOGRAINE_SCAN_RANGE = 150.0f;  // the chapel and its door
constexpr float MOGRAINE_SPAWN_X = 1153.9f;    // the other wings share map 189: no grid search there
constexpr float MOGRAINE_SPAWN_Y = 1398.4f;
}

Unit* FindDoan(PlayerbotAI* botAI) { return TbcHazard::FindBoss(botAI, DOAN_NAME); }

bool IsDetonationPending(Unit* doan)
{
    return doan && doan->IsAlive() &&
           (doan->FindCurrentSpellBySpellId(SPELL_DOAN_DETONATION) || doan->HasAura(SPELL_DOAN_ARCANE_BUBBLE));
}

bool IsSafeFromDetonation(Player* bot, Unit* doan, float x, float y, float z)
{
    constexpr float margin = 2.0f;
    if (doan->GetExactDist2d(x, y) > DETONATION_RADIUS + bot->GetCombatReach() + margin)
        return true;

    return !doan->IsWithinLOS(x, y, z + LOS_EYE_HEIGHT);
}

bool IsMograineCallEntry(uint32 entry)
{
    // NPC_SCARLET_MONK, _ABBOT, _CHAMPION, _CENTURION, _WIZARD, _CHAPLAIN (scarletmonastery.h)
    switch (entry)
    {
        case 4540:
        case 4303:
        case 4302:
        case 4301:
        case 4300:
        case 4299:
            return true;
        default:
            return false;
    }
}

void CollectMograineCall(Unit* mograine, std::vector<Unit*>& out)
{
    out.clear();
    if (!mograine)
        return;

    std::list<Creature*> found;
    mograine->GetCreatureListWithEntryInGrid(found, {4540, 4303, 4302, 4301, 4300, 4299}, MOGRAINE_CALL_RADIUS);
    for (Creature* creature : found)
    {
        if (creature && creature->IsAlive() && !creature->IsInCombat() && !creature->IsInEvadeMode())
            out.push_back(creature);
    }
}

Unit* IdleMograineWithChapel(Player* bot, uint32* idleCalled)
{
    if (idleCalled)
        *idleCalled = 0;

    if (bot->GetMapId() != SCARLET_MONASTERY_MAP_ID ||
        bot->GetExactDist2d(MOGRAINE_SPAWN_X, MOGRAINE_SPAWN_Y) > MOGRAINE_SCAN_RANGE)
        return nullptr;

    // a grid search over the chapel: once a second per bot (bots of one map run on its thread)
    struct Cached
    {
        uint32 ms = 0;
        ObjectGuid mograine;
        uint32 called = 0;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    uint32 const now = getMSTime();
    Cached& entry = cache[bot->GetGUID().GetRawValue()];
    if (!entry.ms || getMSTimeDiff(entry.ms, now) >= MOGRAINE_CACHE_MS)
    {
        entry.ms = now;
        entry.mograine.Clear();
        entry.called = 0;
        Creature* mograine = bot->FindNearestCreature(NPC_COMMANDER_MOGRAINE, MOGRAINE_SCAN_RANGE, true);
        if (mograine && !mograine->IsInCombat() && !mograine->IsInEvadeMode())
        {
            std::vector<Unit*> called;
            CollectMograineCall(mograine, called);
            if (!called.empty())
            {
                entry.mograine = mograine->GetGUID();
                entry.called = uint32(called.size());
            }
        }
    }

    if (entry.mograine.IsEmpty())
        return nullptr;

    Creature* mograine = ObjectAccessor::GetCreature(*bot, entry.mograine);
    if (!mograine || !mograine->IsAlive() || mograine->IsInCombat())
        return nullptr;

    if (idleCalled)
        *idleCalled = entry.called;

    return mograine;
}
}  // namespace ScarletMonastery

using namespace ScarletMonastery;

bool DoanDetonationTrigger::IsActive()
{
    Unit* doan = FindDoan(botAI);
    if (!IsDetonationPending(doan))
        return false;

    return !IsSafeFromDetonation(bot, doan, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
}

bool DoanArcaneExplosionTrigger::IsActive()
{
    if (PlayerbotAI::IsMelee(bot) || PlayerbotAI::IsTank(bot))
        return false;

    Unit* doan = FindDoan(botAI);
    if (!doan || !doan->IsAlive() || IsDetonationPending(doan))
        return false;

    return TbcHazard::IsInside(bot, doan->GetPosition(), ARCANE_EXPLOSION_RADIUS + bot->GetCombatReach());
}

namespace Scholomance
{
namespace
{
constexpr uint32 HALL_CACHE_MS = 1000;
}

bool OnHallFloor(float x, float y, float z)
{
    return z < HALL_FLOOR_MAX_Z && x > HALL_MIN_X && x < HALL_MAX_X && y > HALL_MIN_Y && y < HALL_MAX_Y;
}

Unit* IdleHallMobNear(Player* bot)
{
    // a grid search: once a second per bot
    struct Cached
    {
        uint32 ms = 0;
        ObjectGuid mob;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    uint32 const now = getMSTime();
    Cached& entry = cache[bot->GetGUID().GetRawValue()];
    if (!entry.ms || getMSTimeDiff(entry.ms, now) >= HALL_CACHE_MS)
    {
        entry.ms = now;
        entry.mob.Clear();
        std::list<Creature*> found;
        // `false`: every creature in range, alive or dead (AllDeadCreaturesInRange filters only with true)
        bot->GetDeadCreatureListInGrid(found, HALL_RALLY_REACH, false);
        float nearest = HALL_RALLY_REACH;
        for (Creature* creature : found)
        {
            if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->IsInEvadeMode() ||
                !creature->IsHostileTo(bot) || creature->IsCritter() ||
                !OnHallFloor(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()))
                continue;

            float const distance = bot->GetExactDist(creature);
            if (distance < nearest)
            {
                nearest = distance;
                entry.mob = creature->GetGUID();
            }
        }
    }

    if (entry.mob.IsEmpty())
        return nullptr;

    Creature* mob = ObjectAccessor::GetCreature(*bot, entry.mob);
    return mob && mob->IsAlive() && !mob->IsInCombat() ? mob : nullptr;
}

// [b32_brs] Vectus / Marduk after Dawn's Gambit (ClassicShared.h). The map check first, then the grid search at most
// every HALL_CACHE_MS per bot (the trigger runs every tick).
Unit* ViewingRoomBossToAttack(Player* bot)
{
    if (!bot || bot->GetMapId() != MAP_SCHOLOMANCE || !bot->IsAlive() || bot->IsInCombat() ||
        !PlayerbotAI::IsTank(bot))
        return nullptr;
    struct Cached
    {
        uint32 ms = 0;
        ObjectGuid boss;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    uint32 const now = getMSTime();
    Cached& entry = cache[bot->GetGUID().GetRawValue()];
    if (!entry.ms || getMSTimeDiff(entry.ms, now) >= HALL_CACHE_MS)
    {
        entry.ms = now ? now : 1;
        entry.boss.Clear();
        for (uint32 const bossEntry : {NPC_VECTUS, NPC_MARDUK_BLACKPOOL})
        {
            Creature* boss = bot->FindNearestCreature(bossEntry, VIEWING_ROOM_ATTACK_RANGE, true);
            if (!boss || boss->IsInCombat() || boss->IsInEvadeMode() || bot->IsFriendlyTo(boss) ||
                !bot->IsValidAttackTarget(boss) || !bot->IsWithinLOSInMap(boss))
                continue;
            // the Gambit has run: no Student is still a (neutral, faction 15) student
            std::list<Creature*> students;
            boss->GetCreatureListWithEntryInGrid(students, NPC_SCHOLOMANCE_STUDENT, VIEWING_ROOM_STUDENT_RANGE);
            bool const gambitRan = std::none_of(students.begin(), students.end(), [](Creature* student)
            { return student->IsAlive() && student->GetFaction() == STUDENT_FACTION_BEFORE_GAMBIT; });
            if (!gambitRan)
                continue;
            entry.boss = boss->GetGUID();
            break;
        }
    }
    if (entry.boss.IsEmpty())
        return nullptr;
    Creature* boss = ObjectAccessor::GetCreature(*bot, entry.boss);
    return boss && boss->IsAlive() && !boss->IsInCombat() ? boss : nullptr;
}

bool KeepsOutOfManaBurn(Player* bot)
{
    return bot->getPowerType() == POWER_MANA && !PlayerbotAI::IsTank(bot) &&
           (PlayerbotAI::IsHeal(bot) || PlayerbotAI::IsRanged(bot));
}

Unit* ManaBurnThreat(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Unit* best = nullptr;
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        if (guid.GetEntry() != NPC_SCHOLOMANCE_ACOLYTE)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->GetVictim() == bot || IsCrowdControlled(unit) ||
            !unit->IsWithinCombatRange(bot, ACOLYTE_MANA_BURN_REACH))
            continue;

        if (!best || bot->GetExactDist2d(unit) < bot->GetExactDist2d(best))
            best = unit;
    }

    return best;
}
}  // namespace Scholomance

bool ScholoManaBurnTrigger::IsActive()
{
    if (bot->GetMapId() != Scholomance::MAP_SCHOLOMANCE || !bot->IsAlive() || !bot->IsInCombat() ||
        !Scholomance::KeepsOutOfManaBurn(bot))
        return false;

    return Scholomance::ManaBurnThreat(botAI) != nullptr;
}

bool MograineTooCloseTrigger::IsActive()
{
    if (bot->GetMapId() != ScarletMonastery::SCARLET_MONASTERY_MAP_ID || !bot->IsAlive())
        return false;

    // [b36_sm] Active across the whole hold band, not only inside the keep-away radius: MograineHoldMultiplier
    // (ShouldHoldOutside) vetoes every reposition within KEEP_AWAY + 1 + HOLD_BUFFER (33 yd) unless the current
    // target stands outside it, so a bot that came to rest between 30 and 33 yd by a move the hold exempts (the
    // tank's "aggressive target" walk) could never move again. sm-h 20261026-024524-sm-h-5A-13: the tank walked to
    // (1127.6,1418.2), 31.3 yd from Mograine, with no current target, and stood there 3316-3558 s ("mograine hold
    // made action test runner move useless", "own walk made no progress" x3); the healer stood at 31.2 yd. Every
    // waypoint was then skipped (Mograine and his chapel "route end"), Mograine was never pulled, his fake death
    // never opened the High Inquisitor's Door and the run stalled at 6/7. Out now steps to the exit past the band.
    constexpr float holdEdge = ScarletMonastery::MOGRAINE_KEEP_AWAY + 1.0f + TbcHazard::HOLD_BUFFER;
    Unit* mograine = ScarletMonastery::IdleMograineWithChapel(bot);
    return mograine && bot->GetExactDist2d(mograine) < holdEdge;
}

namespace
{
// [rally_when_needed] the last harness waypoint each tank was given (the harness clears it while the group drinks)
struct LastLead
{
    uint32 ms = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// a copy in and out under the lock: a map is not always updated by the same worker thread
std::mutex& HallLeadLock()
{
    static std::mutex lock;
    return lock;
}

std::unordered_map<uint64, LastLead>& HallLastLead()
{
    static std::unordered_map<uint64, LastLead> leads;
    return leads;
}

LastLead GetHallLead(Player* leader)
{
    std::lock_guard<std::mutex> guard(HallLeadLock());
    auto const found = HallLastLead().find(leader->GetGUID().GetRawValue());
    return found == HallLastLead().end() ? LastLead() : found->second;
}

void SetHallLead(Player* leader, uint32 now, float x, float y, float z)
{
    std::lock_guard<std::mutex> guard(HallLeadLock());
    LastLead& seen = HallLastLead()[leader->GetGUID().GetRawValue()];
    seen.ms = now ? now : 1;
    seen.x = x;
    seen.y = y;
    seen.z = z;
}

void HallRallyLog(Player* bot, char const* what, float x, float y, float z)
{
    thread_local std::unordered_map<uint64, uint32> lastMs;
    uint32 const now = getMSTime();
    uint32& last = lastMs[bot->GetGUID().GetRawValue()];
    if (last && getMSTimeDiff(last, now) < 10 * IN_MILLISECONDS)
        return;

    last = now ? now : 1;
    LOG_INFO("playerbots", "[scholo hall] {}: rally {} ({:.1f},{:.1f},{:.1f})", bot->GetName(), what, x, y, z);
}
}  // namespace

bool ScholoHallRallyTrigger::IsActive()
{
    namespace S = Scholomance;
    if (bot->GetMapId() != S::MAP_SCHOLOMANCE || !bot->IsAlive())
        return false;

    // [rally_when_needed] the tank (it leads the group's walk) and the harness waypoint it was last given, seen on
    // every tick: the harness clears the waypoint while the group drinks after a fight, the rally ran in that gap
    uint32 const now = getMSTime();
    Group* group = bot->GetGroup();
    Player* tank = nullptr;
    if (group)
    {
        ObjectGuid const mainTank = PlayerbotAI::GetMainTankGuid(group);
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                (member->GetGUID() == mainTank || (!tank && PlayerbotAI::IsTank(member))))
                tank = member;
        }
    }

    Player* leader = tank ? tank : bot;
    TestRunner::Waypoint waypoint;
    bool harnessLeads = false;
    for (Player* walker : {bot, tank})
        if (walker && TestRunner::GetWaypoint(walker->GetGUID(), waypoint) && waypoint.mapId == bot->GetMapId())
        {
            harnessLeads = true;
            SetHallLead(leader, now, waypoint.x, waypoint.y, waypoint.z);
        }

    if (bot->IsInCombat())
    {
        _lastCombatMs = now;
        return false;
    }

    if (!_lastCombatMs || getMSTimeDiff(_lastCombatMs, now) > S::HALL_RALLY_AFTER_FIGHT_MS ||
        !S::OnHallFloor(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
        return false;

    // the tank's own pull (it walks to its cast spot) and a LoS pull's hold own the movement
    if (LosPull::IsHolding(bot))
        return false;

    PullStrategy* pull = PullStrategy::Get(botAI);
    if (pull && (pull->HasTarget() || pull->IsPullPendingToStart() || pull->HasPullStarted()))
        return false;

    Unit* mob = S::IdleHallMobNear(bot);
    if (!mob)
        return false;

    // [rally_when_needed] scholo-h 2026-10-25 15:41:32-46 (build 28): the fight at the ramp foot was over, the harness
    // led the tank to the Iron Gate (next target a Risen Guard past it, off the hall floor), the group stood ready
    // (mana 75-83 %). 4 s later the rally walked the tank back up the stairs to the ramp top, 10 s later the harness
    // walked him down again. The rally is a regroup, not a reflex: it never runs against a harness waypoint (of the
    // bot or of its tank), nor right after a fight while the tank's last waypoint leads on, away from the ramp top. It
    // runs when it is needed: a member is low or dead, the group stands scattered, or an idle hall mob is about to
    // wake on the bot.
    if (harnessLeads)
    {
        HallRallyLog(bot, "skipped: the harness waypoint leads", waypoint.x, waypoint.y, waypoint.z);
        return false;  // the harness walk owns the movement (it stops short of idle mobs itself)
    }

    bool low = false;
    bool scattered = false;
    for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMap() != bot->GetMap())
            continue;

        if (!member->IsAlive())
        {
            low = true;
            continue;
        }

        if (member->GetHealthPct() < S::HALL_RALLY_LOW_HEALTH_PCT ||
            (member->getPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA) > 0 &&
             member->GetPowerPct(POWER_MANA) < S::HALL_RALLY_LOW_MANA_PCT))
            low = true;

        if (tank && member != tank && member->GetExactDist(tank) > S::HALL_RALLY_SCATTERED)
            scattered = true;
    }

    // an idle hall mob about to wake on the bot where it stands (a patrol with a wider margin: it walks on)
    Creature* creature = mob->ToCreature();
    float const margin = creature && creature->isMoving() ? S::HALL_RALLY_PATROL_MARGIN : S::HALL_RALLY_WAKE_MARGIN;
    bool const waking = creature && !MovementAction::IdleAggroBlocker(creature, bot, bot->GetPositionX(),
                                                                       bot->GetPositionY(), bot->GetPositionZ(),
                                                                       margin, false);

    // the next target lies ahead: the tank's last waypoint lies farther from the ramp top than the bot
    LastLead const lead = GetHallLead(leader);
    if (!waking && lead.ms && getMSTimeDiff(lead.ms, now) <= S::HALL_RALLY_LEAD_MEMORY_MS)
    {
        float const dx = lead.x - S::HALL_RALLY_X;
        float const dy = lead.y - S::HALL_RALLY_Y;
        float const dz = lead.z - S::HALL_RALLY_Z;
        float const botToRally = bot->GetExactDist(S::HALL_RALLY_X, S::HALL_RALLY_Y, S::HALL_RALLY_Z);
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > botToRally)
        {
            HallRallyLog(bot, "skipped: the next target lies ahead (the tank's last waypoint)", lead.x, lead.y, lead.z);
            return false;
        }
    }

    if (!low && !scattered && !waking)
        return false;  // a ready group out of reach of the idle floor stays where the harness / the tank led it

    HallRallyLog(bot, low ? "needed: a member is low" : scattered ? "needed: the group is scattered"
                                                                  : "needed: an idle hall mob wakes here",
                 mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
    return true;
}

// [b31_ubrs_door] the drake pen behind the stadium's wave portcullis (ClassicShared.h)
namespace BlackrockSpire
{
bool InPenBand(float y, float z) { return y >= PEN_MIN_Y && y <= PEN_MAX_Y && z >= PEN_MIN_Z && z <= PEN_MAX_Z; }

bool InDrakePen(float x, float y, float z) { return x >= PEN_ENTERED_X && x <= PEN_DOOR_X + 40.0f && InPenBand(y, z); }

bool WavePortcullisOpen(Player* bot)
{
    GameObject* gate = bot->FindNearestGameObject(GO_WAVE_PORTCULLIS, 60.0f);
    return gate && gate->GetGoState() == GO_STATE_ACTIVE;
}
}  // namespace BlackrockSpire

bool BrsDrakePenTrigger::IsActive()
{
    using namespace BlackrockSpire;
    if (bot->GetMapId() != MAP_BLACKROCK_SPIRE || !bot->IsAlive() ||
        !InDrakePen(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
        return false;
    // Only while the gate is up: behind the closed bars there is no way out, the fight goes on where it is.
    return WavePortcullisOpen(bot);
}

bool DungeonEventStepTrigger::IsActive()
{
    WorldObject* target = nullptr;
    return DungeonEvents::NextStep(botAI, target) != nullptr;
}

// [b32_brs] Scholomance Viewing Room: Vectus / Marduk to attack (Scholomance::ViewingRoomBossToAttack)
bool ScholoViewingRoomBossTrigger::IsActive() { return Scholomance::ViewingRoomBossToAttack(bot) != nullptr; }

// [b32_st] Sunken Temple, the Shade of Hakkar event (ClassicShared.h SunkenTemple). st-h 20261025-221216-st-h-5G-8:
// the group fought Hakkari Minions and walked the Hakkari Blood to Eternal Flame 1/4 while the Suppressor spawned at
// 3671 s stood idle for 32 s - the Shade despawned before the second Bloodkeeper, one flame of four in every st run.
Unit* SunkenTemple::SuppressorToAttack(Player* bot)
{
    if (!bot || bot->GetMapId() != ST_MAP_ID || !bot->IsAlive() || PlayerbotAI::IsTank(bot) ||
        PlayerbotAI::IsHeal(bot))
        return nullptr;
    struct Cached
    {
        uint32 ms = 0;
        ObjectGuid mob;
    };
    thread_local std::unordered_map<uint64, Cached> cache;
    uint32 const now = getMSTime();
    Cached& entry = cache[bot->GetGUID().GetRawValue()];
    if (!entry.ms || getMSTimeDiff(entry.ms, now) >= SUPPRESSOR_CACHE_MS)
    {
        entry.ms = now ? now : 1;
        entry.mob.Clear();
        std::list<Creature*> suppressors;
        bot->GetCreatureListWithEntryInGrid(suppressors, NPC_NIGHTMARE_SUPPRESSOR, SUPPRESSOR_ATTACK_RANGE);
        float nearest = SUPPRESSOR_ATTACK_RANGE + 1.0f;
        for (Creature* mob : suppressors)
        {
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->IsInEvadeMode() ||
                !bot->IsValidAttackTarget(mob) || !bot->IsWithinLOSInMap(mob))
                continue;
            float const distance = bot->GetExactDist(mob);
            if (distance < nearest)
            {
                nearest = distance;
                entry.mob = mob->GetGUID();
            }
        }
    }
    if (entry.mob.IsEmpty())
        return nullptr;
    Creature* mob = ObjectAccessor::GetCreature(*bot, entry.mob);
    return mob && mob->IsAlive() && !mob->IsInCombat() ? mob : nullptr;
}

bool StSuppressorTrigger::IsActive() { return SunkenTemple::SuppressorToAttack(bot) != nullptr; }

// [b33_st] (ClassicShared.h SunkenTemple::FightsInMelee) the normal and the heroic template of the Deathwalker.
bool SunkenTemple::FightsInMelee(Unit const* unit)
{
    constexpr uint32 NPC_ATALAI_DEATHWALKER = 5271;
    constexpr uint32 NPC_ATALAI_DEATHWALKER_HEROIC = 705271;
    Creature const* creature = unit ? unit->ToCreature() : nullptr;
    if (!creature || creature->GetMapId() != ST_MAP_ID)
        return false;
    uint32 const entry = creature->GetEntry();
    uint32 const templateEntry = creature->GetCreatureTemplate() ? creature->GetCreatureTemplate()->Entry : entry;
    return entry == NPC_ATALAI_DEATHWALKER || entry == NPC_ATALAI_DEATHWALKER_HEROIC ||
           templateEntry == NPC_ATALAI_DEATHWALKER || templateEntry == NPC_ATALAI_DEATHWALKER_HEROIC;
}
