/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoSTriggers.h"
#include "AiObjectContext.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "MotionMaster.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <cmath>  // [b44_cos]
#include <list>
#include <unordered_map>
#include <vector>

using namespace CoSIds;

namespace
{
constexpr float WAVE_SEARCH_RANGE = 300.0f;  // wave spots lie up to ~265 yd from Arthas' stand (WavesLocations)
constexpr uint32 WAVE_CACHE_MS = IN_MILLISECONDS;
// Arthas' LAST_CITY stand (LeaderIntroPos5 2540,1129) lies ~258 yd from the Corruptor (EventPos 2329,1277).
constexpr float CORRUPTOR_SEARCH_RANGE = 400.0f;
constexpr uint32 CORRUPTOR_SEARCH_INTERVAL_MS = 5 * IN_MILLISECONDS;
// instance_culling_of_stratholme Update: the Corruptor despawns (FAIL) once the whole minutes left reach 0, so the
// last 60 s of DATA_GUARDIANTIME_EVENT are never usable.
constexpr uint32 GUARDIAN_EXPIRY_MARGIN_MS = MINUTE * IN_MILLISECONDS;
// Time estimate for the detour: the street route is longer than the straight line, the group runs at ~7 yd/s and
// burns the heroic Corruptor down in about half a minute. Kept optimistic: Mal'Ganis waits, a failed try costs little.
constexpr float CORRUPTOR_ROUTE_FACTOR = 1.3f;
constexpr float GROUP_RUN_SPEED = 7.0f;
constexpr uint32 CORRUPTOR_KILL_MS = 30 * IN_MILLISECONDS;
constexpr uint32 CORRUPTOR_LOG_INTERVAL_MS = 30 * IN_MILLISECONDS;
constexpr float EVENT_TARGET_APPROACH = 20.0f;
// An idle mob this close to the group (or its wave) is in the way: Risen Zombie aggro reach 17-21 yd plus a margin
// so the harness' approach stop (which halts the walk short of that reach) always finds it pullable.
constexpr float EVENT_TRASH_IN_THE_WAY = 30.0f;
// [b34_cos3] During the Corruptor detour the tank's own walk ("cos engage event target", the navmesh path to the
// Corruptor) carries the group down the burning street, so an idle mob is in the way only inside its own real aggro
// range plus this slack: what the walk passes closer aggroes and is fought, the rest is run past. cos-h
// 20261026-014521-cos-h-5B-21: the detour ran from 02:13:46 (273 s usable) and the harness took one idle Risen
// Zombie after another at 23-30 yd ("blocks route", approach_stop at 28-29 yd, pulls 51-66, 165 s of fights); the
// group stood still 190 of the 330 s after Epoch and the Corruptor failed ~200 yd short at 02:18:19.
constexpr float DETOUR_TRASH_SLACK = 3.0f;
// Arthas' attackers the group goes for (he fights at his stand or a few yards ahead of the group).
constexpr float ARTHAS_ATTACKER_RANGE = 60.0f;
constexpr uint32 ARTHAS_IDLE_LOG_AFTER_MS = 30 * IN_MILLISECONDS;
constexpr float FOLLOW_ARTHAS_RANGE = 15.0f;
constexpr float ARTHAS_GOSSIP_GATHER = 30.0f;
// [b33_cos] the longest wait for a walking / returning Arthas before his gossip goes anyway (CoSArthasGossipTrigger)
constexpr uint32 ARTHAS_GOSSIP_MOVING_WAIT_MS = 30 * IN_MILLISECONDS;
constexpr float GHOUL_EXPLOSION_RANGE = 10.0f + 2.0f;  // Explode Ghoul 52480/58825: 10 yd around the living ghoul

struct WaveCache
{
    uint32 time = 0;
    ObjectGuid target;
    ObjectGuid combatTarget;  // [b44_cos] the nearest wave creature already in combat
};

struct CorruptorCache
{
    uint32 searchMs = 0;
    ObjectGuid guid;
    uint32 goLogMs = 0;
    uint32 skipLogMs = 0;
};

thread_local std::unordered_map<uint32, CorruptorCache> corruptorCache;

// The summoned Corruptor (instance->_infiniteGUID is not exposed by GetGuidData): resolved by GUID, searched for at
// most every CORRUPTOR_SEARCH_INTERVAL_MS per instance.
Creature* FindCorruptor(Player* bot, CorruptorCache& entry)
{
    if (entry.guid)
    {
        Creature* cached = bot->GetMap()->GetCreature(entry.guid);
        if (cached && cached->IsAlive())
            return cached;
        entry.guid.Clear();
    }

    uint32 const now = getMSTime();
    if (entry.searchMs && getMSTimeDiff(entry.searchMs, now) < CORRUPTOR_SEARCH_INTERVAL_MS)
        return nullptr;

    entry.searchMs = now;
    Creature* corruptor = bot->FindNearestCreature(NPC_INFINITE_CORRUPTOR, CORRUPTOR_SEARCH_RANGE);
    if (corruptor)
        entry.guid = corruptor->GetGUID();
    return corruptor;
}

bool IsWaveProgress(uint32 progress)
{
    return progress == COS_PROGRESS_FINISHED_CITY_INTRO || progress == COS_PROGRESS_KILLED_MEATHOOK;
}

bool IsArthasGossipProgress(uint32 progress)
{
    switch (progress)
    {
        case COS_PROGRESS_FINISHED_INTRO:
        case COS_PROGRESS_REACHED_TOWN_HALL:
        case COS_PROGRESS_KILLED_EPOCH:
        case COS_PROGRESS_LAST_CITY:
        case COS_PROGRESS_BEFORE_MALGANIS:
            return true;
        default:
            return false;
    }
}

bool IsOutOfCombatInInstance(Player* bot)
{
    return bot->GetMapId() == MAP_CULLING_OF_STRATHOLME && bot->IsAlive() && !bot->IsInCombat();
}

// [b33_cos] In combat by the flag only: nothing attacks the bot or a member close by, and its victim (if any) is out
// of its sight. cos-h 20261026-000434-cos-h-5H-22 00:29:55 -> 00:31:55: an Infinite Hunter fought Arthas on the Town
// Hall's upper floor (2436,1187,148), the tank stood one floor below (2439,1206,135) "in combat, 0 attackers" with
// "tank view: no line of sight" for 119 s while the Guardian of Time ran: every CoS move trigger asked for out of
// combat, the tank's own pull could not see the mob, nothing walked it up the stairs.
constexpr float NOMINAL_COMBAT_MEMBER_RANGE = 40.0f;
bool IsNominalCombat(Player* bot)
{
    if (bot->GetMapId() != MAP_CULLING_OF_STRATHOLME || !bot->IsAlive() || !bot->IsInCombat() ||
        !bot->getAttackers().empty())
        return false;

    Unit* victim = bot->GetVictim();
    if (victim && victim->IsAlive() && bot->IsWithinLOSInMap(victim))
        return false;

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                !member->getAttackers().empty() && bot->IsWithinDistInMap(member, NOMINAL_COMBAT_MEMBER_RANGE))
                return false;
        }
    }
    return true;
}
}  // namespace

namespace CoSHelpers
{
InstanceScript* GetInstance(Player* bot)
{
    return bot->GetMapId() == MAP_CULLING_OF_STRATHOLME ? bot->GetInstanceScript() : nullptr;
}

uint32 GetProgress(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    return instance ? instance->GetData(DATA_ARTHAS_EVENT) : COS_PROGRESS_NOT_STARTED;
}

Creature* GetArthas(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return nullptr;

    ObjectGuid const guid = instance->GetGuidData(DATA_ARTHAS);
    return guid ? bot->GetMap()->GetCreature(guid) : nullptr;
}

Creature* GetWaveTarget(Player* bot, bool preferInCombat)
{
    if (!IsWaveProgress(GetProgress(bot)))
        return nullptr;

    Creature* arthas = GetArthas(bot);
    if (!arthas)
        return nullptr;

    // One answer per instance and second, measured from the interactor, so the whole group heads the same way.
    thread_local std::unordered_map<uint32, WaveCache> cache;
    WaveCache& entry = cache[bot->GetInstanceId()];
    uint32 const now = getMSTime();
    if (entry.time && getMSTimeDiff(entry.time, now) < WAVE_CACHE_MS)
    {
        // [b44_cos] A non-tank goes for the wave creature that fights: the nearest one may stand idle, and the
        // event-target trigger (which waits for an idle wave) then left it following while another wave mob fought.
        if (preferInCombat && entry.combatTarget)
        {
            Creature* fighting = bot->GetMap()->GetCreature(entry.combatTarget);
            if (fighting && fighting->IsAlive() && fighting->IsInCombat())
                return fighting;
        }
        Creature* cached = entry.target ? bot->GetMap()->GetCreature(entry.target) : nullptr;
        return cached && cached->IsAlive() ? cached : nullptr;
    }

    entry.time = now;
    entry.target.Clear();
    entry.combatTarget.Clear();

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    Player* origin = botAI ? DungeonInteract::GetInteractor(botAI) : nullptr;
    if (!origin)
        origin = bot;

    static std::vector<uint32> const waveEntries = {NPC_RISEN_ZOMBIE, NPC_DEVOURING_GHOUL, NPC_DARK_NECROMANCER,
                                                    NPC_TOMB_STALKER, NPC_CRYPT_FIEND, NPC_BILE_GOLEM,
                                                    NPC_ENRAGING_GHOUL, NPC_PATCHWORK_CONSTRUCT, NPC_MEATHOOK,
                                                    NPC_SALRAMM};
    std::list<Creature*> candidates;
    origin->GetCreatureListWithEntryInGrid(candidates, waveEntries, WAVE_SEARCH_RANGE);

    Creature* best = nullptr;
    Creature* bestFighting = nullptr;  // [b44_cos]
    for (Creature* creature : candidates)
    {
        // Infected citizens share the zombie entry; only Arthas' summons belong to the wave.
        if (!creature || !creature->IsAlive() || !creature->IsSummon() ||
            creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
            continue;

        TempSummon const* summon = creature->ToTempSummon();
        if (!summon || summon->GetSummonerGUID() != arthas->GetGUID())
            continue;

        if (!best || origin->GetExactDist(creature) < origin->GetExactDist(best))
            best = creature;
        if (creature->IsInCombat() &&
            (!bestFighting || origin->GetExactDist(creature) < origin->GetExactDist(bestFighting)))
            bestFighting = creature;
    }

    if (best)
        entry.target = best->GetGUID();
    if (bestFighting)
        entry.combatTarget = bestFighting->GetGUID();
    return preferInCombat && bestFighting ? bestFighting : best;
}

// [b44_cos] The "ranged in melee danger" edge (RangedPositionActions: MeleeGapTo = gap to the boss' hitbox - the
// bot's reach - 4/3 yd, danger under 2 yd) plus a margin, never under 8 yd; the band is a few yards deep so a bot
// standing in it is not sent on for a yard.
float EpochStackMinDistance(Player* bot, Unit const* boss)
{
    constexpr float DANGER_GAP = 2.0f;     // RangedPositionActions MELEE_DANGER_GAP
    constexpr float STACK_MARGIN = 1.5f;
    constexpr float STACK_FLOOR = 8.0f;    // hunters' dead zone and the boss' melee stay well behind
    float const edge = boss->GetCombatReach() + bot->GetCombatReach() + 4.0f / 3.0f + DANGER_GAP;
    return std::max(STACK_FLOOR, edge + STACK_MARGIN);
}

float EpochStackMaxDistance(Player* bot, Unit const* boss)
{
    constexpr float STACK_DEPTH = 4.0f;
    return EpochStackMinDistance(bot, boss) + STACK_DEPTH;
}

bool IsEpochStacked(Player* bot, Unit const* boss)
{
    constexpr float STACK_MAX_HEIGHT = 4.0f;
    if (!bot || !boss || bot->GetMap() != boss->GetMap())
        return false;
    float const distance = bot->GetExactDist2d(boss);
    return distance >= EpochStackMinDistance(bot, boss) && distance <= EpochStackMaxDistance(bot, boss) &&
           std::fabs(bot->GetPositionZ() - boss->GetPositionZ()) <= STACK_MAX_HEIGHT && bot->IsWithinLOSInMap(boss);
}

Creature* GetCorruptorDetourTarget(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance || !bot->GetMap()->IsHeroic())
        return nullptr;

    uint32 const progress = GetProgress(bot);
    if (progress < COS_PROGRESS_KILLED_EPOCH)
    {
        // A new instance under a reused id starts over (the cache is only filled from the last city on).
        if (!corruptorCache.empty())
            corruptorCache.erase(bot->GetInstanceId());
        return nullptr;
    }

    // The Corruptor stands in the burning streets behind the Town Hall's secret passage (Shkaf gate, opened by Arthas
    // at waypoint 36), off his route to Mal'Ganis. The detour starts once the LAST_CITY gossip sent Arthas running
    // (npc_arthas::OnGossipSelect drops his gossip flag): leaving him at his waypoint-45 stand would mean walking the
    // whole street back for that gossip. At BEFORE_MALGANIS he waits by the square, ~135 yd from the Corruptor, and
    // the Mal'Ganis gossip waits for the detour (CoSArthasGossipTrigger). Arthas is an escort without a player
    // (ReorderInstance: Start(true)), he never despawns for a group out of range.
    bool eligible = progress == COS_PROGRESS_BEFORE_MALGANIS;
    if (progress == COS_PROGRESS_LAST_CITY)
    {
        Creature* arthas = GetArthas(bot);
        eligible = arthas && arthas->IsAlive() && !arthas->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
    }
    if (!eligible)
        return nullptr;

    uint32 const state = instance->GetBossState(BOSS_INFINITE_CORRUPTOR);
    if (state == DONE || state == FAIL)
        return nullptr;

    uint32 const timeLeft = instance->GetData(DATA_GUARDIANTIME_EVENT);
    if (timeLeft <= GUARDIAN_EXPIRY_MARGIN_MS)
        return nullptr;

    CorruptorCache& entry = corruptorCache[bot->GetInstanceId()];
    Creature* corruptor = FindCorruptor(bot, entry);
    if (!corruptor)
        return nullptr;

    // Measured from the group's interactor so the whole group takes the same decision.
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    Player* origin = botAI ? DungeonInteract::GetInteractor(botAI) : nullptr;
    if (!origin || origin->GetMap() != bot->GetMap())
        origin = bot;

    uint32 const usableMs = timeLeft - GUARDIAN_EXPIRY_MARGIN_MS;
    float const distance = origin->GetExactDist2d(corruptor);
    uint32 const neededMs =
        static_cast<uint32>(distance * CORRUPTOR_ROUTE_FACTOR / GROUP_RUN_SPEED * IN_MILLISECONDS) + CORRUPTOR_KILL_MS;
    uint32 const now = getMSTime();
    if (usableMs < neededMs)
    {
        if (!entry.skipLogMs || getMSTimeDiff(entry.skipLogMs, now) >= CORRUPTOR_LOG_INTERVAL_MS)
        {
            entry.skipLogMs = now;
            LOG_INFO("playerbots",
                     "[cos-corruptor-skip] {}: Infinite Corruptor out of reach, {} s of Guardian of Time usable, ~{} s "
                     "needed for {:.0f} yd (progress {})",
                     bot->GetName(), usableMs / IN_MILLISECONDS, neededMs / IN_MILLISECONDS, distance, progress);
        }
        RaidBotDuty::LogWorkaroundOnce(bot, "cos-corruptor",
                                       "Culling: too little Guardian of Time left for the Infinite Corruptor");
        return nullptr;
    }

    if (!entry.goLogMs || getMSTimeDiff(entry.goLogMs, now) >= CORRUPTOR_LOG_INTERVAL_MS)
    {
        entry.goLogMs = now;
        LOG_INFO("playerbots",
                 "[cos-corruptor-detour] {}: group heads for the Infinite Corruptor, {} s of Guardian of Time usable, "
                 "~{} s needed for {:.0f} yd (progress {})",
                 bot->GetName(), usableMs / IN_MILLISECONDS, neededMs / IN_MILLISECONDS, distance, progress);
    }
    return corruptor;
}

bool IsGuardianTimerRunning(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance || !bot->GetMap()->IsHeroic())
        return false;

    uint32 const state = instance->GetBossState(BOSS_INFINITE_CORRUPTOR);
    return state != DONE && state != FAIL && instance->GetData(DATA_GUARDIANTIME_EVENT) > GUARDIAN_EXPIRY_MARGIN_MS;
}

bool IsSkippableEventTrash(Player* bot, Unit const* unit)
{
    if (!bot || !unit || unit->IsInCombat() || !unit->IsAlive())
        return false;

    Creature const* creature = unit->ToCreature();
    if (!creature || creature->IsDungeonBoss())
        return false;

    switch (creature->GetEntry())
    {
        case NPC_INFINITE_CORRUPTOR:
        case NPC_MEATHOOK:
        case NPC_SALRAMM:
        case NPC_EPOCH:
        case NPC_MAL_GANIS:
        case NPC_ARTHAS_COS:
            return false;
        default:
            break;
    }

    // cos-h 2026-10-25 121320: the ten waves took 20 min (12:21:08 -> 12:41:01) while the group pulled 40-odd idle
    // Risen Zombies one by one ("blocks route" / "event chain"; Meathook spawned 12:25:03, engaged 12:28:07), and the
    // Guardian of Time ran out at 12:46:08 with Epoch still alive.
    uint32 const progress = GetProgress(bot);
    if (progress < COS_PROGRESS_FINISHED_CITY_INTRO || progress > COS_PROGRESS_BEFORE_MALGANIS)
        return false;

    if (!IsGuardianTimerRunning(bot))
        return false;

    // Arthas' summons are the event itself (waves, Town Hall adversaries, rift spawns, Epoch, Mal'Ganis).
    if (creature->IsSummon())
    {
        TempSummon const* summon = creature->ToTempSummon();
        Creature* arthas = GetArthas(bot);
        if (summon && (!arthas || summon->GetSummonerGUID() == arthas->GetGUID()))
            return false;
    }

    // Walked past, never through: an idle citizen this close to the group or to the wave it heads for stands in the
    // way and is fought (cos-h 20261025-163739: an idle Risen Zombie 21 yd from the leader stopped every approach to
    // wave 7 - "approach_stop" - while the harness' blocker gate and the idle-pull veto refused to pull it as skippable
    // trash; the waves stalled after Meathook for 6 minutes).
    // [b34_cos3] The Corruptor detour: only inside the mob's real aggro range plus DETOUR_TRASH_SLACK (the tank's own
    // walk goes on there; the 30 yd margin is for the waves' approach stop).
    float inTheWay = EVENT_TRASH_IN_THE_WAY;
    if (progress >= COS_PROGRESS_LAST_CITY && GetCorruptorDetourTarget(bot))
        inTheWay = std::min(inTheWay,
                            creature->GetAggroRange(bot) + creature->m_CombatDistance + DETOUR_TRASH_SLACK);
    if (bot->GetExactDist(creature) <= inTheWay)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    Player* interactor = botAI ? DungeonInteract::GetInteractor(botAI) : nullptr;
    if (interactor && interactor != bot && interactor->GetMap() == bot->GetMap() &&
        interactor->GetExactDist(creature) <= inTheWay)
        return false;

    Creature* wave = GetWaveTarget(bot);
    return !wave || wave->GetExactDist(creature) > EVENT_TRASH_IN_THE_WAY;
}

Unit* GetArthasAttacker(Player* bot)
{
    Creature* arthas = GetArthas(bot);
    if (!arthas || !arthas->IsAlive() || !arthas->IsInCombat())
        return nullptr;

    // His escort stands still while he fights (npc_escortAI): what attacks him holds the whole event (cos-n
    // 20261025-163739: after the Town Hall gossip at 17:02:50 the group stood 3 minutes next to an Arthas that
    // did not walk on). A player kills it.
    Unit* best = nullptr;
    auto const consider = [&](Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !unit->IsInWorld() || unit->GetMap() != bot->GetMap() ||
            !bot->IsValidAttackTarget(unit) || !bot->IsWithinDistInMap(unit, ARTHAS_ATTACKER_RANGE))
            return;
        if (!best || bot->GetExactDist(unit) < bot->GetExactDist(best))
            best = unit;
    };
    consider(arthas->GetVictim());
    for (Unit* attacker : arthas->getAttackers())
        consider(attacker);
    return best;
}

void LogArthasIdle(Player* bot)
{
    struct IdleWatch
    {
        uint32 sinceMs = 0;
        uint32 logMs = 0;
    };
    thread_local std::unordered_map<uint32, IdleWatch> watches;

    IdleWatch& entry = watches[bot->GetInstanceId()];
    Creature* arthas = GetArthas(bot);
    uint32 const progress = GetProgress(bot);
    bool const idle = arthas && arthas->IsAlive() && !arthas->isMoving() &&
                      !arthas->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) && progress >= COS_PROGRESS_START_INTRO &&
                      progress < COS_PROGRESS_FINISHED;
    uint32 const now = getMSTime();
    if (!idle)
    {
        entry = IdleWatch();
        return;
    }

    if (!entry.sinceMs)
        entry.sinceMs = now ? now : 1;
    if (getMSTimeDiff(entry.sinceMs, now) < ARTHAS_IDLE_LOG_AFTER_MS ||
        (entry.logMs && getMSTimeDiff(entry.logMs, now) < ARTHAS_IDLE_LOG_AFTER_MS))
        return;

    entry.logMs = now ? now : 1;
    Unit* victim = arthas->GetVictim();
    LOG_INFO("playerbots",
             "[cos-arthas-idle] {}: Arthas stands {} s without gossip at ({:.1f},{:.1f},{:.1f}), progress {}, "
             "in combat {}, {} attackers, victim {} ({:.0f} yd from him), {:.0f} yd from the bot",
             bot->GetName(), getMSTimeDiff(entry.sinceMs, now) / IN_MILLISECONDS, arthas->GetPositionX(),
             arthas->GetPositionY(), arthas->GetPositionZ(), progress, arthas->IsInCombat() ? "yes" : "no",
             arthas->getAttackers().size(), victim ? victim->GetName() : std::string("none"),
             victim ? arthas->GetExactDist(victim) : 0.0f, bot->GetExactDist(arthas));
}

void LogTimerMilestone(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance || !bot->GetMap()->IsHeroic())
        return;

    struct Milestone
    {
        uint32 progress = 0;
        uint32 corruptorState = 0;
        bool seen = false;
    };
    thread_local std::unordered_map<uint32, Milestone> milestones;

    uint32 const progress = GetProgress(bot);
    uint32 const state = instance->GetBossState(BOSS_INFINITE_CORRUPTOR);
    Milestone& entry = milestones[bot->GetInstanceId()];
    if (entry.seen && entry.progress == progress && entry.corruptorState == state)
        return;

    // A new instance under a reused id starts over.
    if (entry.seen && progress < entry.progress)
        entry = Milestone();

    entry.seen = true;
    entry.progress = progress;
    entry.corruptorState = state;
    LOG_INFO("playerbots", "[cos-timer] {}: progress {}, Infinite Corruptor state {}, Guardian of Time left {} s",
             bot->GetName(), progress, state, instance->GetData(DATA_GUARDIANTIME_EVENT) / IN_MILLISECONDS);
}

// Everyone alive stands around the anchor out of combat and nobody lies dead waiting for a resurrection.
bool IsGroupReady(Player* bot, WorldObject const* anchor, float radius)
{
    if (!DungeonInteract::IsGroupGathered(bot, anchor, radius))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->GetMapId() == bot->GetMapId() && member->GetInstanceId() == bot->GetInstanceId() &&
            !member->IsAlive())
            return false;
    }
    return true;
}
}  // namespace CoSHelpers

bool ExplodeGhoulTrigger::IsActive()
{
    // Salramm detonates a LIVING Ghoul Minion (boss_salramm.cpp ExplodeGhoul); the tank keeps the ghouls on him.
    if (bot->GetMapId() != MAP_CULLING_OF_STRATHOLME || PlayerbotAI::IsTank(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "salramm the fleshcrafter");
    if (!boss)
        return false;

    GuidVector npcs = AI_VALUE(GuidVector, "nearest hostile npcs");
    for (ObjectGuid const& guid : npcs)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && unit->GetEntry() == NPC_GHOUL_MINION &&
            bot->GetExactDist2d(unit) < GHOUL_EXPLOSION_RANGE)
            return true;
    }
    return false;
}

bool EpochRangedTrigger::IsActive()
{
    // [b44_cos] Only in the fight and only off the stack band (or out of the boss' sight): it fired for every ranged
    // member whenever Epoch was found, and the walk onto his centre fought "ranged in melee danger" (cos-h, 25
    // move_conflicts in 12 runs).
    if (PlayerbotAI::IsMelee(bot))
        return false;
    Unit* boss = AI_VALUE2(Unit*, "find target", "chrono-lord epoch");
    if (!boss || !boss->IsAlive() || !boss->IsInCombat() || bot->GetMap() != boss->GetMap())
        return false;
    return !CoSHelpers::IsEpochStacked(bot, boss);
}

bool CoSChromieStartTrigger::IsActive()
{
    return IsOutOfCombatInInstance(bot) && CoSHelpers::GetProgress(bot) == COS_PROGRESS_NOT_STARTED &&
           DungeonInteract::IsInteractor(botAI) && !bot->HasItemCount(ITEM_ARCANE_DISRUPTOR, 1);
}

bool CoSSuspiciousCrateTrigger::IsActive()
{
    return IsOutOfCombatInInstance(bot) && CoSHelpers::GetProgress(bot) == COS_PROGRESS_NOT_STARTED &&
           DungeonInteract::IsInteractor(botAI) && bot->HasItemCount(ITEM_ARCANE_DISRUPTOR, 1);
}

bool CoSChromieMiddleTrigger::IsActive()
{
    return IsOutOfCombatInInstance(bot) && CoSHelpers::GetProgress(bot) == COS_PROGRESS_CRATES_FOUND &&
           DungeonInteract::IsInteractor(botAI);
}

bool CoSArthasGossipTrigger::IsActive()
{
    if (!IsOutOfCombatInInstance(bot) || !IsArthasGossipProgress(CoSHelpers::GetProgress(bot)) ||
        !DungeonInteract::IsInteractor(botAI))
        return false;

    // Arthas only offers the gossip while he waits for the group (flag set on the event waypoints).
    Creature* arthas = CoSHelpers::GetArthas(bot);
    if (!arthas || !arthas->IsAlive() || !arthas->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return false;

    // [b33_cos] Not while he still walks: CMSG_GOSSIP_HELLO pauses a creature's point movement for
    // Creature.MovingStopTimeForPlayer (180 s; Creature::PauseMovementForInteraction). cos-h
    // 20261026-000434-cos-h-5H-22: the KILLED_EPOCH gossip went 2 s after Epoch died, while Arthas most likely still
    // returned to his combat start (escort RETURNING = a POINT movement); the escort then stood 3 min 15 s at
    // (2428.5,1121.5) after the secret-passage gossip (00:34:56 -> 00:38:10; every other cos run: 18-30 s) and the
    // Guardian of Time ran out at 00:37:38. A player lets him arrive first; after ARTHAS_GOSSIP_MOVING_WAIT_MS the
    // gossip goes anyway.
    uint32 const now = getMSTime();
    if (arthas->isMoving() || arthas->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) == POINT_MOTION_TYPE)
    {
        if (!_movingSinceMs)
            _movingSinceMs = now ? now : 1;
        if (getMSTimeDiff(_movingSinceMs, now) < ARTHAS_GOSSIP_MOVING_WAIT_MS)
            return false;
    }
    else
        _movingSinceMs = 0;

    // Heroic: the Infinite Corruptor detour comes before Mal'Ganis while the timer allows it (the LAST_CITY gossip
    // itself is never held: GetCorruptorDetourTarget waits for it).
    if (CoSHelpers::GetCorruptorDetourTarget(bot))
        return false;

    return CoSHelpers::IsGroupReady(bot, arthas, ARTHAS_GOSSIP_GATHER);
}

bool CoSEventTargetTrigger::IsActive()
{
    if (bot->GetMapId() == MAP_CULLING_OF_STRATHOLME && DungeonInteract::IsInteractor(botAI))
    {
        CoSHelpers::LogTimerMilestone(bot);
        CoSHelpers::LogArthasIdle(bot);
    }

    // [b33_cos] the tank also goes for an Arthas attacker out of its sight while only nominally in combat (see
    // IsNominalCombat): the action walks the navmesh path to it (stairs, another floor).
    bool const nominal = !IsOutOfCombatInInstance(bot) && PlayerbotAI::IsTank(bot) && IsNominalCombat(bot);
    if (!IsOutOfCombatInInstance(bot) && !nominal)
        return false;
    if (nominal)
    {
        Unit* attacker = CoSHelpers::GetArthasAttacker(bot);
        if (!attacker || bot->IsWithinLOSInMap(attacker))
            return false;
        static thread_local uint32 nominalLogMs = 0;
        uint32 const now = getMSTime();
        if (!nominalLogMs || getMSTimeDiff(nominalLogMs, now) >= 10 * IN_MILLISECONDS)
        {
            nominalLogMs = now ? now : 1;
            LOG_INFO("playerbots",
                     "[b33_cos-nominal-combat] {}: in combat with no attacker, walks to {} ({}) off Arthas out of "
                     "sight, {:.0f} yd (progress {})",
                     bot->GetName(), attacker->GetName(), attacker->GetEntry(), bot->GetExactDist(attacker),
                     CoSHelpers::GetProgress(bot));
        }
        return true;
    }

    Unit* target = CoSHelpers::GetCorruptorDetourTarget(bot);
    bool const detour = target != nullptr;
    if (!target)
        target = CoSHelpers::GetArthasAttacker(bot);
    if (!target)
        target = CoSHelpers::GetWaveTarget(bot, !PlayerbotAI::IsTank(bot));  // [b44_cos] non-tanks: a fighting one
    if (!target)
        return false;

    if (PlayerbotAI::IsTank(bot))
        return true;

    // The others close in behind the tank once it fights the wave; before that they follow the tank (cos-h
    // 20261025-163739: Rtmagar / Rthunar flipped between "follow" and this walk to 15 yd of the idle wave 7 every
    // second - move_conflict at 16:56:49 .. 16:57:46).
    // [b34_cos3] ...and on the Corruptor detour too, while the tank they follow (their master, alive, here) leads
    // it: each member's own navmesh path to the Corruptor split the group. cos-h 20261026-014521-cos-h-5B-21
    // (02:13:46 -> 02:15:22): Rtshaaf / Rtpriai, still on the secret passage's ramp above the 8.8 yd drop, walked
    // back into the Town Hall ("going point to (2389.9,1194.7,148.1)") against "follow" / "move to ranged position"
    // (move_conflict), the others held with "would wake Risen Zombie -> stay"; the harness' ping-pong hold kept the
    // tank 96 s for Rtshaaf 47-119 yd away (dz 9-22). The tank's own walk is the route; the members come in once it
    // fights the Corruptor.
    // [b35_cos4] The tank is looked up in the group, not as the bot's master: cos-h 20261026-024524-cos-h-5H-19
    // (03:13:15 on) every non-tank still walked its own path to the Corruptor ("doing cos engage event target ->
    // going point to (2389.9,1194.7,148.1)" for Rthunad, (2385.1,1217.9,134.0) for Rtpriag - back into the Town Hall),
    // the master test never held. Far from the tank (DETOUR_FOLLOW_TANK_RANGE, cut off) the member walks its own path
    // again, which CoSEngageEventTargetAction sends round the closed Festival Lane Gate.
    if (detour && !target->IsInCombat())
    {
        constexpr float DETOUR_FOLLOW_TANK_RANGE = 60.0f;
        Player* tank = nullptr;
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() ||
                    member->GetMap() != bot->GetMap() || !PlayerbotAI::IsTank(member))
                    continue;
                if (!tank || group->IsLeader(member->GetGUID()) || PlayerbotAI::IsMainTank(member))
                    tank = member;
            }
        if (tank && bot->IsWithinDistInMap(tank, DETOUR_FOLLOW_TANK_RANGE))
            return false;
    }
    if (!detour && !target->IsInCombat())
        return false;

    return bot->GetExactDist2d(target) > EVENT_TARGET_APPROACH;
}

bool CoSFollowArthasTrigger::IsActive()
{
    if (!IsOutOfCombatInInstance(bot))
        return false;

    uint32 const progress = CoSHelpers::GetProgress(bot);
    if (progress < COS_PROGRESS_START_INTRO || progress >= COS_PROGRESS_FINISHED)
        return false;

    Creature* arthas = CoSHelpers::GetArthas(bot);
    if (arthas && !arthas->IsAlive())
        RaidBotDuty::LogWorkaroundOnce(bot, "cos-arthas-death",
                                       "needs honest tactic: Arthas died (AC #26217), the event rolls back");

    if (!arthas || !arthas->IsAlive() || bot->IsWithinDistInMap(arthas, FOLLOW_ARTHAS_RANGE))
        return false;

    // Within the gather range only the interactor (the one the others follow) walks after Arthas; the others keep
    // with it by "follow" (cos-n 20261025-163739 17:06:06: Rtshaax flipped between this and "follow", 159 deg apart).
    if (!DungeonInteract::IsInteractor(botAI) && bot->IsWithinDistInMap(arthas, ARTHAS_GOSSIP_GATHER))
        return false;

    // Arthas idle, waiting for the group's gossip (or the intro not walking yet): nothing to follow, the gossip
    // steps lead (cos-n/h 2026-10-25 wave 8: the leader walked after an idle Arthas for 3 minutes while Chromie's
    // gossip 9610 -> 9612 waited 96 yd away). Only a member beyond the gossip gather range walks back to his gossip
    // stand: nothing else brings the group back after the heroic Corruptor detour, and the gossip waits for everyone.
    if (!arthas->isMoving())
    {
        if (IsArthasGossipProgress(progress))
        {
            if (bot->IsWithinDistInMap(arthas, ARTHAS_GOSSIP_GATHER))
                return false;
        }
        else if (progress == COS_PROGRESS_START_INTRO || arthas->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
            return false;
    }

    return !CoSHelpers::GetCorruptorDetourTarget(bot) && !CoSHelpers::GetWaveTarget(bot);
}
