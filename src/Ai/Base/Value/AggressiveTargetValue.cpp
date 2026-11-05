/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AggressiveTargetValue.h"
#include "ANAddTactics.h"
#include "AttackersValue.h"
#include "CreatureGroups.h"
#include "HealthTriggers.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "SLShared.h"
#include "UKTriggers.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // Path checks are the expensive part: only the nearest few candidates get one.
    constexpr size_t MAX_PATH_CHECKS = 3;
    // [hleft-never-pulled] halls_of_reflection.h NPC_LICH_KING_EVENT / NPC_LICH_KING_BOSS, Hadronox 28921
    constexpr uint32 HOR_LICH_KING_ENTRY = 36954;
    constexpr uint32 HOR_LICH_KING_ESCAPE_ENTRY = 37226;
    constexpr uint32 AN_HADRONOX_ENTRY = 28921;
    constexpr size_t MAX_TALL_PATH_CHECKS = 2;  // the same for candidates over the height rule
    // A pack the careful planner left is passed over this long, then planned afresh (its crowd control may be up).
    constexpr uint32 CAREFUL_SKIP_MS = 60 * IN_MILLISECONDS;
    constexpr size_t CAREFUL_SKIP_MAX_MOBS = 32;

    struct SkippedMob
    {
        ObjectGuid mob;
        uint32 ms = 0;
    };

    std::mutex skipLock;
    std::unordered_map<uint64, std::vector<SkippedMob>> skipped;  // by tank

    void Expire(std::vector<SkippedMob>& mobs, uint32 now)
    {
        mobs.erase(std::remove_if(mobs.begin(), mobs.end(),
                                  [now](SkippedMob const& entry)
                                  { return getMSTimeDiff(entry.ms, now) >= CAREFUL_SKIP_MS; }),
                   mobs.end());
    }
}

namespace
{
    constexpr uint32 TANK_REJECTS_TTL_MS = 10 * IN_MILLISECONDS;
    struct TankRejectEntry
    {
        std::vector<ObjectGuid> mobs;
        uint32 ms = 0;
    };
    constexpr uint32 TANK_UNSEEN_TTL_MS = 3 * MINUTE * IN_MILLISECONDS;
    std::mutex rejectLock;
    std::unordered_map<uint64, TankRejectEntry> rejects;
    std::unordered_map<uint64, std::unordered_map<uint64, uint32>> unseen;  // tank -> mob -> marked at
}

namespace TankRejects
{
    void Set(Player* tank, std::vector<ObjectGuid> const& mobs)
    {
        std::lock_guard<std::mutex> guard(rejectLock);
        TankRejectEntry& entry = rejects[tank->GetGUID().GetRawValue()];
        entry.mobs = mobs;
        entry.ms = getMSTime();
    }

    void MarkUnseen(ObjectGuid tank, ObjectGuid mob)
    {
        std::lock_guard<std::mutex> guard(rejectLock);
        unseen[tank.GetRawValue()][mob.GetRawValue()] = getMSTime();
    }

    bool IsRejected(ObjectGuid tank, ObjectGuid mob)
    {
        std::lock_guard<std::mutex> guard(rejectLock);
        if (auto const marks = unseen.find(tank.GetRawValue()); marks != unseen.end())
        {
            auto const mark = marks->second.find(mob.GetRawValue());
            if (mark != marks->second.end())
            {
                if (getMSTimeDiff(mark->second, getMSTime()) <= TANK_UNSEEN_TTL_MS)
                    return true;
                marks->second.erase(mark);
            }
        }

        auto const found = rejects.find(tank.GetRawValue());
        if (found == rejects.end() || getMSTimeDiff(found->second.ms, getMSTime()) > TANK_REJECTS_TTL_MS)
            return false;

        return std::find(found->second.mobs.begin(), found->second.mobs.end(), mob) != found->second.mobs.end();
    }
}

namespace
{
    constexpr uint32 GUARANTEE_REQUEST_TTL_MS = 5 * IN_MILLISECONDS;
    struct GuaranteeRequestEntry
    {
        ObjectGuid mob;
        bool walkIn = false;
        uint32 ms = 0;
    };
    std::mutex guaranteeRequestLock;
    std::unordered_map<uint64, GuaranteeRequestEntry> guaranteeRequests;  // by tank
}

namespace GuaranteeRequest
{
    void Set(ObjectGuid tank, ObjectGuid mob, bool walkIn)
    {
        std::lock_guard<std::mutex> guard(guaranteeRequestLock);
        GuaranteeRequestEntry& entry = guaranteeRequests[tank.GetRawValue()];
        entry.mob = mob;
        entry.walkIn = walkIn;
        entry.ms = getMSTime();
    }

    bool Get(ObjectGuid tank, ObjectGuid& mob, bool& walkIn)
    {
        std::lock_guard<std::mutex> guard(guaranteeRequestLock);
        auto const found = guaranteeRequests.find(tank.GetRawValue());
        if (found == guaranteeRequests.end() || getMSTimeDiff(found->second.ms, getMSTime()) > GUARANTEE_REQUEST_TTL_MS)
            return false;

        mob = found->second.mob;
        walkIn = found->second.walkIn;
        return true;
    }
}

// [b32_sh_ooze] RespawnIgnored (AggressiveTargetValue.h)
namespace
{
    std::mutex respawnIgnoredLock;
    std::unordered_map<uint32, std::set<uint32>> respawnIgnored;  // instance id -> entries
}

namespace RespawnIgnored
{
    bool Mark(uint32 instanceId, uint32 entry)
    {
        std::lock_guard<std::mutex> guard(respawnIgnoredLock);
        return respawnIgnored[instanceId].insert(entry).second;
    }

    bool IsIgnored(Unit const* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->GetSpawnId() || !creature->GetInstanceId() || creature->IsDungeonBoss() ||
            creature->isWorldBoss() || creature->GetRespawnDelay() < 1 ||
            creature->GetRespawnDelay() > RESPAWN_IGNORE_MAX_S)
            return false;
        std::lock_guard<std::mutex> guard(respawnIgnoredLock);
        auto const found = respawnIgnored.find(creature->GetInstanceId());
        return found != respawnIgnored.end() && found->second.count(creature->GetEntry());
    }

    bool Any(uint32 instanceId)
    {
        std::lock_guard<std::mutex> guard(respawnIgnoredLock);
        auto const found = respawnIgnored.find(instanceId);
        return found != respawnIgnored.end() && !found->second.empty();
    }
}

namespace
{
    // The harness refreshes its target on every steer tick: an entry not refreshed this long is gone (a run ended,
    // the party left the instance).
    constexpr uint32 ROUTE_TARGET_TTL_MS = 10 * IN_MILLISECONDS;
    struct RouteTargetEntry
    {
        ObjectGuid mob;
        uint32 ms = 0;
    };
    std::mutex routeTargetLock;
    std::unordered_map<uint64, RouteTargetEntry> routeTargets;  // by tank
}

namespace RouteTarget
{
    void Set(ObjectGuid tank, ObjectGuid mob)
    {
        std::lock_guard<std::mutex> guard(routeTargetLock);
        RouteTargetEntry& entry = routeTargets[tank.GetRawValue()];
        entry.mob = mob;
        entry.ms = getMSTime();
    }

    void Clear(ObjectGuid tank)
    {
        std::lock_guard<std::mutex> guard(routeTargetLock);
        routeTargets.erase(tank.GetRawValue());
    }

    ObjectGuid Get(ObjectGuid tank)
    {
        std::lock_guard<std::mutex> guard(routeTargetLock);
        auto const found = routeTargets.find(tank.GetRawValue());
        if (found == routeTargets.end())
            return ObjectGuid::Empty;

        if (getMSTimeDiff(found->second.ms, getMSTime()) > ROUTE_TARGET_TTL_MS)
        {
            routeTargets.erase(found);
            return ObjectGuid::Empty;
        }

        return found->second.mob;
    }
}

namespace
{
    constexpr uint32 CAREFUL_VERDICT_TTL_MS = 10 * IN_MILLISECONDS;
    // the last verdict outlives the tank's fight this long, for the rest after it (GetNextFight)
    constexpr uint32 CAREFUL_VERDICT_AFTER_COMBAT_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 CAREFUL_VERDICT_COMBAT_NOTE_MS = IN_MILLISECONDS;  // NoteCombat writes at most this often
    struct CarefulVerdictEntry
    {
        ObjectGuid target;
        std::vector<ObjectGuid> mobs;
        uint32 count = 0;
        uint32 ms = 0;
        uint32 combatMs = 0;  // the tank's last noted fight (0: none since the verdict)
        bool accepted = false;
        bool hopeless = false;
        std::string why;
        CarefulPlanVerdict::Fight fight;
    };
    struct AbortEntry
    {
        ObjectGuid mob;
        uint32 ms = 0;
    };
    std::unordered_map<uint64, AbortEntry> aborts;  // by tank, under verdictLock
    // [b32_smpull] pulls given up for good from where the tank stands (NotePullOutOfReach): tank -> mob -> until
    struct OutOfReachEntry  // [lead1290] until when, and the stand it was noted from
    {
        uint32 until = 0;
        float x = 0.0f;
        float y = 0.0f;
    };
    std::unordered_map<uint64, std::unordered_map<uint64, OutOfReachEntry>> outOfReach;  // under verdictLock
    // [b37_sv] ranged pulls given up out of sight for the healer's sake (NoteEngageOnly): tank -> mob -> until
    std::unordered_map<uint64, std::unordered_map<uint64, uint32>> engageOnly;  // under verdictLock
    std::mutex verdictLock;
    std::unordered_map<uint64, CarefulVerdictEntry> verdicts;  // by tank

    // Fresh, or kept after the tank's fight; under verdictLock.
    bool IsKept(CarefulVerdictEntry const& entry, uint32 now)
    {
        return getMSTimeDiff(entry.ms, now) <= CAREFUL_VERDICT_TTL_MS ||
               (entry.combatMs && getMSTimeDiff(entry.combatMs, now) <= CAREFUL_VERDICT_AFTER_COMBAT_MS);
    }
}

namespace CarefulPlanVerdict
{
    void Set(Player* tank, ObjectGuid target, bool accepted, uint32 mobs, std::vector<ObjectGuid> const& packMobs,
             Fight const& fight, bool hopeless, std::string const& why)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        CarefulVerdictEntry& entry = verdicts[tank->GetGUID().GetRawValue()];
        entry.target = target;
        entry.mobs = packMobs;
        entry.count = mobs;
        entry.accepted = accepted;
        entry.ms = getMSTime();
        entry.combatMs = 0;
        entry.fight = fight;
        entry.hopeless = !accepted && hopeless;
        entry.why = accepted ? std::string() : why;
    }

    bool GetRefusal(ObjectGuid tank, ObjectGuid mob, bool& hopeless, uint32& waitMs, std::string& why)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        auto const found = verdicts.find(tank.GetRawValue());
        if (found == verdicts.end())
            return false;

        CarefulVerdictEntry const& entry = found->second;
        if (entry.accepted || getMSTimeDiff(entry.ms, getMSTime()) > CAREFUL_VERDICT_TTL_MS ||
            (entry.target != mob && std::find(entry.mobs.begin(), entry.mobs.end(), mob) == entry.mobs.end()))
            return false;

        hopeless = entry.hopeless;
        // the wait as published, less the time since (the verdict is refreshed about once a second)
        uint32 const age = getMSTimeDiff(entry.ms, getMSTime());
        waitMs = entry.fight.guaranteeWaitMs > age ? entry.fight.guaranteeWaitMs - age : 0;
        why = entry.why;
        return true;
    }

    void NoteAborted(Player* tank, ObjectGuid mob)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        AbortEntry& entry = aborts[tank->GetGUID().GetRawValue()];
        entry.mob = mob;
        entry.ms = getMSTime();
    }

    // [b32_smpull]
    void NotePullOutOfReach(Player* tank, ObjectGuid mob, uint32 forMs)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        uint32 const now = getMSTime();
        auto& memo = outOfReach[tank->GetGUID().GetRawValue()];
        for (auto it = memo.begin(); it != memo.end();)
        {
            if (int32(it->second.until - now) <= 0)  // expired (the difference survives getMSTime's wrap)
                it = memo.erase(it);
            else
                ++it;
        }
        OutOfReachEntry& entry = memo[mob.GetRawValue()];
        entry.until = (now + forMs) ? now + forMs : 1;
        entry.x = tank->GetPositionX();
        entry.y = tank->GetPositionY();
    }

    bool PullOutOfReach(Player* tank, ObjectGuid mob)
    {
        if (!tank)
            return false;
        std::lock_guard<std::mutex> guard(verdictLock);
        auto const found = outOfReach.find(tank->GetGUID().GetRawValue());
        if (found == outOfReach.end())
            return false;
        auto const entry = found->second.find(mob.GetRawValue());
        if (entry == found->second.end())
            return false;
        // `until` is ahead of now while the memo runs (getMSTime wraps after 49 days: the difference decides)
        uint32 const now = getMSTime();
        // [lead1290] a give-up from another stand is no fact about this one: the tank moved, the pull is planned again
        if (int32(entry->second.until - now) <= 0 ||
            tank->GetExactDist2d(entry->second.x, entry->second.y) > PULL_OUT_OF_REACH_STAND)
        {
            found->second.erase(entry);
            return false;
        }
        return true;
    }

    // [b37_sv]
    void NoteEngageOnly(Player* tank, ObjectGuid mob, uint32 forMs)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        uint32 const now = getMSTime();
        auto& memo = engageOnly[tank->GetGUID().GetRawValue()];
        for (auto it = memo.begin(); it != memo.end();)
        {
            if (int32(it->second - now) <= 0)
                it = memo.erase(it);
            else
                ++it;
        }
        memo[mob.GetRawValue()] = (now + forMs) ? now + forMs : 1;
    }

    bool EngageOnly(ObjectGuid tank, ObjectGuid mob)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        auto const found = engageOnly.find(tank.GetRawValue());
        if (found == engageOnly.end())
            return false;
        auto const entry = found->second.find(mob.GetRawValue());
        if (entry == found->second.end())
            return false;
        uint32 const now = getMSTime();
        if (int32(entry->second - now) <= 0)
        {
            found->second.erase(entry);
            return false;
        }
        return true;
    }

    bool RecentlyAborted(ObjectGuid tank, uint32 withinMs, ObjectGuid* mob)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        auto const found = aborts.find(tank.GetRawValue());
        if (found == aborts.end() || getMSTimeDiff(found->second.ms, getMSTime()) > withinMs)
            return false;

        if (mob)
            *mob = found->second.mob;
        return true;
    }

    bool Get(ObjectGuid tank, ObjectGuid mob, bool& accepted, uint32& mobs)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        auto const found = verdicts.find(tank.GetRawValue());
        if (found == verdicts.end())
            return false;

        CarefulVerdictEntry const& entry = found->second;
        uint32 const now = getMSTime();
        if (getMSTimeDiff(entry.ms, now) > CAREFUL_VERDICT_TTL_MS)
        {
            // kept for GetNextFight while the tank fights and rests, but no verdict on a pack any more
            if (!IsKept(entry, now))
                verdicts.erase(found);

            return false;
        }

        if (entry.target != mob && std::find(entry.mobs.begin(), entry.mobs.end(), mob) == entry.mobs.end())
            return false;

        accepted = entry.accepted;
        mobs = entry.count;
        return true;
    }

    void NoteCombat(Player* tank)
    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(verdictLock);
        auto const found = verdicts.find(tank->GetGUID().GetRawValue());
        if (found == verdicts.end() || !IsKept(found->second, now))
            return;

        uint32& combatMs = found->second.combatMs;
        if (!combatMs || getMSTimeDiff(combatMs, now) >= CAREFUL_VERDICT_COMBAT_NOTE_MS)
            combatMs = now;
    }

    // [lead182b] the planner's look held before any verdict (a readiness gate): the pack it looks at
    struct LookEntry
    {
        uint32 mobs = 0;
        uint32 ms = 0;
    };
    std::unordered_map<uint64, LookEntry> looks;  // by tank, under verdictLock

    void NoteLook(Player* tank, uint32 mobs)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        uint32 const now = getMSTime();
        looks[tank->GetGUID().GetRawValue()] = {mobs, now ? now : 1};
    }

    bool GetNextFight(ObjectGuid tank, Fight& out)
    {
        std::lock_guard<std::mutex> guard(verdictLock);
        auto const found = verdicts.find(tank.GetRawValue());
        if (found == verdicts.end())
        {
            // [lead182b] no verdict yet: the pack the planner's held look is about, while that look is fresh
            auto const look = looks.find(tank.GetRawValue());
            if (look == looks.end() || !look->second.mobs ||
                getMSTimeDiff(look->second.ms, getMSTime()) > CAREFUL_VERDICT_TTL_MS)
                return false;
            out = Fight();
            out.mobs = look->second.mobs;
            return true;
        }

        if (!IsKept(found->second, getMSTime()))
        {
            verdicts.erase(found);
            return false;
        }

        out = found->second.fight;
        out.refused = !found->second.accepted;
        if (found->second.accepted && found->second.combatMs)
        {
            out.boss = false;
            out.realBoss = false;
        }
        return true;
    }

    bool GetNextFight(ObjectGuid tank, RestState::NextFight& out)
    {
        Fight fight;
        if (!GetNextFight(tank, fight))
            return false;

        out.mobs = fight.mobs;
        out.fightSec = fight.fightSec;
        out.cost = fight.cost;
        out.boss = fight.boss;
        out.realBoss = fight.boss && fight.realBoss;
        out.topUp = fight.refused;
        return true;
    }
}

namespace CarefulPullSkip
{
    void Mark(Player* tank, ObjectGuid mob)
    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(skipLock);
        std::vector<SkippedMob>& mobs = skipped[tank->GetGUID().GetRawValue()];
        Expire(mobs, now);
        for (SkippedMob& entry : mobs)
        {
            if (entry.mob == mob)
            {
                entry.ms = now;
                return;
            }
        }

        if (mobs.size() < CAREFUL_SKIP_MAX_MOBS)
            mobs.push_back({mob, now});
    }

    std::vector<ObjectGuid> Get(Player* tank)
    {
        std::vector<ObjectGuid> result;
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(skipLock);
        auto const found = skipped.find(tank->GetGUID().GetRawValue());
        if (found == skipped.end())
            return result;

        Expire(found->second, now);
        for (SkippedMob const& entry : found->second)
            result.push_back(entry.mob);

        if (found->second.empty())
            skipped.erase(found);

        return result;
    }
}

namespace
{
    // MAP_UTGARDE_KEEP (574) comes from AreaDefines.h (via SLShared.h); a local copy here was ambiguous.

    // The dungeon's own order rule for one creature (no formation).
    bool IsOrderLocked(Creature* creature, InstanceScript const* instance)
    {
        switch (creature->GetMapId())
        {
            case MAP_UTGARDE_KEEP:
                // forge 1 -> 2 -> 3 (utgarde_keep.cpp npc_dragonflayer_forge_master::JustEngagedWith)
                return UKIds::IsForgeMasterLocked(creature, instance);
            default:
                return false;
        }
    }
}

namespace EncounterOrder
{
    bool IsLocked(Unit* mob)
    {
        Creature* creature = mob ? mob->ToCreature() : nullptr;
        if (!creature)
            return false;

        // Shadow Labyrinth: Blackheart's engage calls every idle mob within 100 yd (wave 6: 18 came); his room and
        // the hall before it are cleared first (SLShared.cpp).
        if (creature->GetMapId() == ShadowLabyrinth::MAP_SHADOW_LABYRINTH)
            return ShadowLabyrinth::IsBlackheartRoomLocked(creature);

        if (creature->GetMapId() != MAP_UTGARDE_KEEP)
            return false;

        InstanceScript const* instance = creature->GetInstanceScript();
        if (!instance)
            return false;

        if (IsOrderLocked(creature, instance))
            return true;

        CreatureGroup const* formation = creature->GetFormation();
        if (!formation)
            return false;

        for (auto const& member : formation->GetMembers())
        {
            if (member.first != creature && member.first->IsAlive() && IsOrderLocked(member.first, instance))
                return true;
        }

        return false;
    }
}

constexpr float MAX_PULL_HEIGHT_DIFF = 18.0f;  // Sunken Temple ramp: dz 12.3 to the first pack; the path check keeps other floors out
constexpr float DUNGEON_TANK_PULL_RANGE = 40.0f;
// The test harness's route target (RouteTarget) is picked this far: the harness stops its walk at the aggro edge of
// the pack, which may lie past DUNGEON_TANK_PULL_RANGE (BRS heroic 22:41: Scarshield Legionnaire / Warlock "blocks
// route" out of the pick, "[pull hold] no aggressive target"; Utgarde Keep 22:25: the next mob over 40 yd, a 15 s
// approach to sight). Up to the harness's approach scan around the target, within "possible targets" sight.
constexpr float ROUTE_TARGET_PICK_RANGE = 70.0f;

namespace
{
    // The harness's live route target of this dungeon tank out of combat, empty when none.
    ObjectGuid DungeonRouteTarget(Player* bot)
    {
        if (!bot->GetMap()->IsDungeon() || !PlayerbotAI::IsTank(bot) || bot->IsInCombat())
            return ObjectGuid::Empty;

        return RouteTarget::Get(bot->GetGUID());
    }

    // "[pull goal] <tank>: picks route target <name> at <yd> (past <range>)", once per tank per target per 10 s.
    void LogRouteTargetPick(Player* bot, Unit* unit, float pickRange)
    {
        constexpr uint32 ROUTE_TARGET_LOG_MS = 10 * IN_MILLISECONDS;
        static std::mutex mutex;
        static std::unordered_map<uint64, std::pair<uint64, uint32>> last;  // tank -> mob, when
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::pair<uint64, uint32>& entry = last[bot->GetGUID().GetRawValue()];
            if (entry.first == unit->GetGUID().GetRawValue() && getMSTimeDiff(entry.second, now) < ROUTE_TARGET_LOG_MS)
                return;

            entry = {unit->GetGUID().GetRawValue(), now};
        }

        LOG_INFO("playerbots", "[pull goal] {}: picks route target {} at {:.1f} yd (past {:.0f})", bot->GetName(),
                 unit->GetName(), bot->GetDistance(unit), pickRange);
    }

    // [b39_scholo] The route target goes before nearer idle mobs whose pack the planner refused: scholo-h
    // 20261026-054314-scholo-h-5A-3 (200-514 s). The harness retargeted the lone Spectral Researcher (Low 363,
    // 25.5 yd, packMobs 1: behind_root_pull) and held "the tank's pull decides". The pick took the nearest mob, a
    // Scholomance Neophyte at 21.8 yd whose chain the planner calls hopeless ("brings 20 mobs, over 18"), so the
    // Researcher never got a plan ("no verdict (unknown)"). After 120 s it was skipped as "careful pull refused",
    // then every target behind it was skipped, twice. At 514 s the body pull took it: 1 attacker, 9 s, no damage of
    // note. The preference lasts ROUTE_PREFER_MS for that route target: once the planner looks at the route target,
    // the refusal of the nearer pack goes stale, and the pick would flip back to it.
    constexpr uint32 ROUTE_PREFER_MS = 60 * IN_MILLISECONDS;
    std::mutex routePreferLock;
    std::unordered_map<uint64, std::pair<uint64, uint32>> routePrefer;  // tank -> route target, since

    bool RoutePreferred(Player* bot, ObjectGuid routeTarget)
    {
        std::lock_guard<std::mutex> lock(routePreferLock);
        auto const found = routePrefer.find(bot->GetGUID().GetRawValue());
        return found != routePrefer.end() && found->second.first == routeTarget.GetRawValue() &&
               getMSTimeDiff(found->second.second, getMSTime()) < ROUTE_PREFER_MS;
    }

    void NoteRoutePreferred(Player* bot, ObjectGuid routeTarget)
    {
        std::lock_guard<std::mutex> lock(routePreferLock);
        uint32 const now = getMSTime();
        routePrefer[bot->GetGUID().GetRawValue()] = {routeTarget.GetRawValue(), now ? now : 1};
    }
}

Unit* AggressiveTargetValue::Calculate() { return PickFor(botAI, DungeonRouteTarget(bot), _reachability); }

// [lead5] The pick itself, for the tank's own value (its route target from the RouteTarget registry) and for the
// dungeon leader (LEADER_ONE_PICKER: the harness's own route target, its own reachability cache, on the world thread
// while no map thread runs): one choice, two callers.
Unit* AggressiveTargetValue::PickFor(PlayerbotAI* botAI, ObjectGuid pickRoute, TargetReachability& reachability)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return nullptr;
    AiObjectContext* context = botAI->GetAiObjectContext();
    // the route target only for a dungeon tank out of combat (the registry's own rule)
    ObjectGuid const routeTarget = bot->GetMap()->IsDungeon() && PlayerbotAI::IsTank(bot) && !bot->IsInCombat()
                                       ? pickRoute
                                       : ObjectGuid::Empty;
    Player* master = botAI->GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() ||
                   !GET_PLAYERBOT_AI(master)))
        master = nullptr;

    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    if (targets.empty() && routeTarget.IsEmpty())
        return nullptr;

    float aggroRange = sPlayerbotAIConfig.AggroDistance;
    // In a dungeon the tank chooses the next pack from outside its aggro reach (the leader stops at the aggro edge
    // plus a margin, wave 11) and pulls it from range, as a player does.
    if (bot->GetMap()->IsDungeon() && PlayerbotAI::IsTank(bot))
        aggroRange = std::max(aggroRange, DUNGEON_TANK_PULL_RANGE);
    std::vector<std::pair<float, Unit*>> candidates;
    // The careful pull planner left these packs a moment ago: another pack first.
    std::vector<ObjectGuid> const skip =
        bot->GetMap()->IsDungeon() && PlayerbotAI::IsTank(bot) ? CarefulPullSkip::Get(bot) : std::vector<ObjectGuid>();
    // The harness walks this tank to this mob: it is a candidate up to ROUTE_TARGET_PICK_RANGE, and always gets its
    // path check (the planner plans the harness's pull at once instead of waiting for the walk into pull range).
    // The route target round a corner is a candidate too ("possible targets" keeps the tank's sight): the planner
    // plans it from the list that ignores sight (its cast spot walks in until it sees the mob, PullBackSpot) and an
    // engage walks into sight first (AggressiveTargetAction::Execute). hos-n wave 7: a Crystalline Shardling "not
    // pulled, tank does not see it", the group stood 186 s.
    if (!routeTarget.IsEmpty() && std::find(targets.begin(), targets.end(), routeTarget) == targets.end())
    {
        GuidVector const unseen = AI_VALUE(GuidVector, "possible targets no los");
        if (std::find(unseen.begin(), unseen.end(), routeTarget) != unseen.end())
            targets.push_back(routeTarget);
    }

    uint32 dropHeight = 0, dropLos = 0, dropRange = 0, dropSkip = 0, dropOrder = 0, dropOther = 0;  // diagnostics
    std::vector<ObjectGuid> rejected;  // another floor or no path: TankRejects
    std::vector<ObjectGuid> tallCandidates;  // over the height rule: kept for the path check (walkers only)
    Unit* heightDropped = nullptr;     // the first mob the height rule left out (diagnostics)

    for (ObjectGuid const guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
        {
            ++dropOther;
            continue;
        }

        if (unit->ToCreature() && !unit->ToCreature()->GetCreatureTemplate()->lootid &&
            bot->GetReactionTo(unit) >= REP_NEUTRAL)
        {
            ++dropOther;
            continue;
        }

        if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
        {
            ++dropOther;
            continue;
        }

        // [hleft-never-pulled] Encounter creatures no tank pulls: the Halls of Reflection Lich King 36954 / 37226 (the
        // escape event drives him: Sylvanas / Jaina gossip, the walls) and Hadronox before she webbed the doors
        // (ANAdds::HadronoxHandsOff: her first player contact cancels her climb).
        if (unit->GetEntry() == HOR_LICH_KING_ENTRY || unit->GetEntry() == HOR_LICH_KING_ESCAPE_ENTRY ||
            (unit->GetEntry() == AN_HADRONOX_ENTRY && ANAdds::HadronoxHandsOff(botAI)))
        {
            ++dropOther;
            continue;
        }

        // [b32_sh_ooze] An idle fast respawner (RespawnIgnored, DB respawn delay) is no pick: it comes when it
        // aggroes. The harness's route target is exempt (a required objective entry).
        if (!unit->IsInCombat() && unit->GetGUID() != routeTarget && RespawnIgnored::IsIgnored(unit))
        {
            ++dropOther;
            continue;
        }

        // A mob down a ramp or a few steps below is still the next pull (Stockade wave 11: the tank stopped at the
        // aggro edge 7 yd above the first pack and never saw it); another floor is left out by the path check below.
        // The harness's route target is exempt: it walks the group to it over the navmesh, and its pick always gets
        // the full path check below (a complete path, no detour). Wailing Caverns heroic wave 6 (Rtpalad): "no
        // aggressive target of 1 possible (1 height)", the route's only mob up a ramp more than 18 yd above, the
        // group stood. Not for a swimmer or a flier: the path check passes them without a path.
        // Up-h build 18: the Skadi gauntlet runs up a ramp, "aggressive target: height" on mobs a full navmesh path
        // reaches. A walker over the height rule is still a candidate: the path check below decides (a complete
        // path without a detour, TargetReachability), so another floor stays out. Not a swimmer or a flier: the path
        // check passes them without a path.
        bool const walkers = !unit->IsInWater() && !unit->IsFlying() && !bot->IsInWater() && !bot->IsFlying();
        bool const tall = std::fabs(bot->GetPositionZ() - unit->GetPositionZ()) > MAX_PULL_HEIGHT_DIFF;
        if (tall && !walkers)
        {
            ++dropHeight;
            if (!heightDropped)
                heightDropped = unit;
            rejected.push_back(guid);
            continue;
        }

        if (!bot->InBattleground() && master && botAI->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) &&
            ServerFacade::instance().GetDistance2d(master, unit) > aggroRange)
        {
            ++dropOther;
            continue;
        }

        if (guid != routeTarget && !bot->IsWithinLOSInMap(unit))
        {
            ++dropLos;
            continue;
        }

        float const distance = bot->GetDistance(unit);
        if (distance > aggroRange && (guid != routeTarget || distance > ROUTE_TARGET_PICK_RANGE))
        {
            ++dropRange;
            continue;
        }

        if (!unit->IsInCombat() && std::find(skip.begin(), skip.end(), guid) != skip.end())
        {
            ++dropSkip;
            continue;
        }

        // its encounter's predecessor has not started: engaged now it evades at once
        if (!unit->IsInCombat() && EncounterOrder::IsLocked(unit))
        {
            ++dropOrder;
            continue;
        }

        candidates.emplace_back(distance, unit);
        if (tall && guid != routeTarget)
            tallCandidates.push_back(guid);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](auto const& left, auto const& right) { return left.first < right.first; });

    // [b39_scholo] The route target goes before nearer idle mobs whose pack the planner refused (see RoutePreferred).
    // It does not go before a mob in combat.
    if (!routeTarget.IsEmpty() && bot->GetMap()->IsDungeon() && PlayerbotAI::IsTank(bot) && !bot->IsInCombat())
    {
        auto const routeAt = std::find_if(candidates.begin(), candidates.end(), [&routeTarget](auto const& candidate)
                                          { return candidate.second->GetGUID() == routeTarget; });
        if (routeAt != candidates.end() && routeAt != candidates.begin() && !routeAt->second->IsInCombat())
        {
            bool nearerFight = false;
            bool allRefused = true;
            std::string nearerWhy;
            for (auto nearer = candidates.begin(); nearer != routeAt; ++nearer)
            {
                if (nearer->second->IsInCombat())
                {
                    nearerFight = true;
                    break;
                }
                bool hopeless = false;
                uint32 waitMs = 0;
                std::string why;
                if (!CarefulPlanVerdict::GetRefusal(bot->GetGUID(), nearer->second->GetGUID(), hopeless, waitMs, why))
                    allRefused = false;
                else if (nearerWhy.empty())
                    nearerWhy = nearer->second->GetName() + ": " + why;
            }
            bool const sticky = RoutePreferred(bot, routeTarget);
            if (!nearerFight && (sticky || allRefused))
            {
                if (!sticky)
                {
                    NoteRoutePreferred(bot, routeTarget);
                    LOG_INFO("playerbots", "[b39_scholo] route_target_first: {}: route target {} ({:.1f} yd) goes "
                             "before {} nearer refused mob(s) ({}) for {} s", bot->GetName(),
                             routeAt->second->GetName(), routeAt->first, size_t(routeAt - candidates.begin()),
                             nearerWhy, ROUTE_PREFER_MS / IN_MILLISECONDS);
                }
                std::rotate(candidates.begin(), routeAt, routeAt + 1);
            }
        }
    }

    // The nearest mob the bot can walk to; one on another floor would run to the healer instead.
    size_t checks = 0;
    size_t tallChecks = 0;
    for (auto const& candidate : candidates)
    {
        bool const isRouteTarget = candidate.second->GetGUID() == routeTarget;
        // a candidate over the height rule has a budget of its own: other floors do not starve the near ones
        bool const tall = std::find(tallCandidates.begin(), tallCandidates.end(), candidate.second->GetGUID()) !=
                          tallCandidates.end();
        if (!isRouteTarget && (tall ? tallChecks++ >= MAX_TALL_PATH_CHECKS : checks++ >= MAX_PATH_CHECKS))
            continue;

        if (reachability.IsReachable(bot, candidate.second))
        {
            if (PlayerbotAI::IsTank(bot))
                TankRejects::Set(bot, rejected);
            if (isRouteTarget && candidate.first > aggroRange)
                LogRouteTargetPick(bot, candidate.second, aggroRange);
            return candidate.second;
        }

        rejected.push_back(candidate.second->GetGUID());
    }

    if (PlayerbotAI::IsTank(bot) && bot->GetMap()->IsDungeon())
        TankRejects::Set(bot, rejected);

    if (bot->GetMap()->IsDungeon() && PlayerbotAI::IsTank(bot) && !bot->IsInCombat())
        LOG_DEBUG("playerbots", "[pull hold] {}: no aggressive target of {} possible ({} candidates unreachable, "
                  "{} height, {} los, {} range {:.0f}, {} careful skip, {} encounter order, {} other)", bot->GetName(),
                  targets.size(), candidates.size(), dropHeight, dropLos, dropRange, aggroRange, dropSkip, dropOrder,
                  dropOther);
    if (heightDropped && bot->GetMap()->IsDungeon() && PlayerbotAI::IsTank(bot) && !bot->IsInCombat())
        LOG_DEBUG("playerbots", "[pull hold] {}: height rule left out {} (dz {:.1f}, {:.1f} yd{})", bot->GetName(),
                  heightDropped->GetName(), heightDropped->GetPositionZ() - bot->GetPositionZ(),
                  bot->GetDistance(heightDropped), heightDropped->GetGUID() == routeTarget ? ", the route target" : "");
    return nullptr;
}

std::string AggressiveTargetValue::Explain(Player* bot, Unit* unit)
{
    return Explain(bot, unit, DungeonRouteTarget(bot));
}

// [lead5] `routeTarget`: the route target the pick sees (the leader's own with LEADER_ONE_PICKER).
std::string AggressiveTargetValue::Explain(Player* bot, Unit* unit, ObjectGuid routeTarget)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !unit)
        return "no unit";

    // FindUnits (AnyUnfriendlyUnitInObjectRangeCheck), AcceptUnit, the line of sight of NearestUnitsValue.
    if (!unit->IsAlive())
        return "possible targets: dead";
    if (unit->IsCritter())
        return "possible targets: critter";
    if (!bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.SightDistance))
        return "possible targets: beyond sight distance";
    if (bot->IsFriendlyTo(unit))
        return "possible targets: friendly";
    if (char const* reason = AttackersValue::PossibleTargetRejection(unit, bot))
        return std::string("possible targets: ") + reason;
    if (AttackersValue::IsIgnoredCritter(unit, bot))
        return "possible targets: ignored critter";
    if (!bot->IsWithinLOSInMap(unit) && unit->GetGUID() != routeTarget)
        return "possible targets: no line of sight from the bot's eyes";

    GuidVector const cached = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->LazyGet();
    bool const listed = std::find(cached.begin(), cached.end(), unit->GetGUID()) != cached.end();

    // AggressiveTargetValue::Calculate.
    std::string verdict;
    float aggroRange = sPlayerbotAIConfig.AggroDistance;
    if (bot->GetMap()->IsDungeon() && PlayerbotAI::IsTank(bot))
        aggroRange = std::max(aggroRange, DUNGEON_TANK_PULL_RANGE);
    if (unit->ToCreature() && !unit->ToCreature()->GetCreatureTemplate()->lootid &&
        bot->GetReactionTo(unit) >= REP_NEUTRAL)
        verdict = "aggressive target: not hostile, no loot";
    else if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
        verdict = "aggressive target: npc flags";
    else if (std::fabs(bot->GetPositionZ() - unit->GetPositionZ()) > MAX_PULL_HEIGHT_DIFF &&
             (unit->IsInWater() || unit->IsFlying() || bot->IsInWater() || bot->IsFlying()))
        verdict = "aggressive target: height (a swimmer or a flier)";
    else if (bot->GetDistance(unit) > aggroRange &&
             (unit->GetGUID() != routeTarget || bot->GetDistance(unit) > ROUTE_TARGET_PICK_RANGE))
        verdict = "aggressive target: beyond pull range";
    else
    {
        std::vector<ObjectGuid> const skip = CarefulPullSkip::Get(bot);
        if (!unit->IsInCombat() && std::find(skip.begin(), skip.end(), unit->GetGUID()) != skip.end())
            verdict = "aggressive target: careful pull skip";
        else if (!unit->IsInCombat() && EncounterOrder::IsLocked(unit))
            verdict = "aggressive target: encounter order (its predecessor has not started)";
        else
        {
            TargetReachability reachability;
            verdict = reachability.IsReachable(bot, unit) ? "accepted" : "aggressive target: unreachable";
        }
    }
    return verdict + (listed ? "" : " (not in the cached possible targets)");
}
