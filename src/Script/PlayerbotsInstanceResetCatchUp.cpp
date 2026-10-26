/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "World.h"

#include <ctime>
#include <set>
#include <vector>

// Applies raid and heroic resets that fell while the worldserver was down (AC #26741).
// InstanceSaveMgr::LoadResetTimes only moves an expired instance_reset time forward and assumes the saves were
// cleaned; nothing cleans them, so a lock survives the reset it missed. This runs before LoadInstances and does
// what InstanceSaveMgr::_ResetSave does at runtime: non-extended binds go, extended binds lose the extension.
namespace
{
    struct ExtendedBind
    {
        uint32 guidLow;
        uint32 instanceId;
        uint16 mapId;
        uint8 difficulty;
        time_t missedReset;
        std::string name;
    };

    std::vector<ExtendedBind> extendedBinds;

    void ApplyMissedResets()
    {
        time_t const now = std::time(nullptr);

        // Only raid/heroic saves follow instance_reset; normal instances keep their own resettime and the core
        // already deletes the expired ones on startup.
        QueryResult result = CharacterDatabase.Query(
            "SELECT ci.guid, ci.instance, ci.permanent, ci.extended, i.map, i.difficulty, r.resettime, "
            "IFNULL(c.name, '') FROM character_instance ci "
            "JOIN instance i ON i.id = ci.instance "
            "JOIN instance_reset r ON r.mapid = i.map AND r.difficulty = i.difficulty "
            "LEFT JOIN characters c ON c.guid = ci.guid "
            "WHERE i.resettime = 0 AND r.resettime > 0 AND r.resettime < {}",
            uint32(now));
        if (!result)
        {
            LOG_INFO("playerbots", "Instance reset catch-up: no lock outlived a reset missed while offline");
            return;
        }

        std::set<uint32> instanceIds;
        uint32 unbound = 0;
        do
        {
            Field* fields = result->Fetch();
            uint32 const guidLow = fields[0].Get<uint32>();
            uint32 const instanceId = fields[1].Get<uint32>();
            bool const permanent = fields[2].Get<bool>();
            bool const extended = fields[3].Get<bool>();
            uint16 const mapId = fields[4].Get<uint16>();
            uint8 const difficulty = fields[5].Get<uint8>();
            time_t const missedReset = time_t(fields[6].Get<uint32>());
            std::string const name = fields[7].Get<std::string>();

            instanceIds.insert(instanceId);
            std::string const due = Acore::Time::TimeToTimestampStr(Seconds(missedReset));
            if (extended)
            {
                extendedBinds.push_back({guidLow, instanceId, mapId, difficulty, missedReset, name});
                LOG_INFO("playerbots",
                         "Instance reset catch-up: {} (guid {}) keeps extended lock on map {} difficulty {} "
                         "instance {} (reset was due {}), extension cleared",
                         name, guidLow, mapId, difficulty, instanceId, due);
                continue;
            }

            ++unbound;
            LOG_INFO("playerbots",
                     "Instance reset catch-up: unbound {} (guid {}) from map {} difficulty {} instance {} "
                     "(permanent {}, reset was due {})",
                     name, guidLow, mapId, difficulty, instanceId, permanent ? 1 : 0, due);
        } while (result->NextRow());

        // Synchronous on purpose: InstanceSaveMgr::LoadInstances reads these tables right after this hook and
        // removes the instances left without binds, their respawn rows and stale characters.instance_id.
        for (uint32 instanceId : instanceIds)
        {
            CharacterDatabase.DirectExecute("DELETE FROM character_instance WHERE instance = {} AND extended = 0",
                                            instanceId);
            CharacterDatabase.DirectExecute("UPDATE character_instance SET extended = 0 WHERE instance = {}",
                                            instanceId);
        }

        LOG_INFO("playerbots", "Instance reset catch-up: {} binds unbound, {} extended locks shortened, {} instances",
                 unbound, uint32(extendedBinds.size()), uint32(instanceIds.size()));
    }

    // An extended lock only survives one reset; drop the ones whose following reset was missed too.
    void ExpireDoublyMissedExtensions()
    {
        time_t const now = std::time(nullptr);
        for (ExtendedBind const& bind : extendedBinds)
        {
            Difficulty const difficulty = Difficulty(bind.difficulty);
            MapDifficulty const* mapDiff = GetMapDifficultyData(bind.mapId, difficulty);
            if (!mapDiff || !mapDiff->resetTime)
                continue;

            // Same period as InstanceSaveMgr::LoadResetTimes.
            uint32 period = uint32(((mapDiff->resetTime * sWorld->getRate(RATE_INSTANCE_RESET_TIME)) / DAY) * DAY);
            if (period < DAY)
                period = DAY;

            if (bind.missedReset + time_t(period) > now)
                continue;

            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(bind.guidLow);
            InstancePlayerBind* playerBind = sInstanceSaveMgr->PlayerGetBoundInstance(guid, bind.mapId, difficulty);
            if (!playerBind || !playerBind->save || playerBind->save->GetInstanceId() != bind.instanceId)
                continue;

            sInstanceSaveMgr->PlayerUnbindInstance(guid, bind.mapId, difficulty, true);
            LOG_INFO("playerbots",
                     "Instance reset catch-up: unbound {} (guid {}) from extended lock on map {} difficulty {} "
                     "instance {} (two resets missed)",
                     bind.name, bind.guidLow, bind.mapId, bind.difficulty, bind.instanceId);
        }
        extendedBinds.clear();
    }
}

class PlayerbotsInstanceResetCatchUpWorldScript : public WorldScript
{
public:
    PlayerbotsInstanceResetCatchUpWorldScript()
        : WorldScript("PlayerbotsInstanceResetCatchUpWorldScript",
                      {WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE, WORLDHOOK_ON_STARTUP})
    {
    }

    // Runs in World::SetInitialWorldSettings before sInstanceSaveMgr->LoadInstances(), while the old reset times are
    // still in instance_reset (LoadResetTimes overwrites them).
    void OnLoadCustomDatabaseTable() override { ApplyMissedResets(); }

    void OnStartup() override { ExpireDoublyMissedExtensions(); }
};

void AddPlayerbotsInstanceResetCatchUp()
{
    new PlayerbotsInstanceResetCatchUpWorldScript();
}
