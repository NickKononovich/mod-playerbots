/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestStats.h"

#include "Creature.h"
#include "Log.h"
#include "NewRpgQuestRoute.h"  // [routeplan] the route stop's event time
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "Timer.h"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr uint32 QSTAT_LOG_MS = 5 * MINUTE * IN_MILLISECONDS;  // a log cadence, no decision reads it
    constexpr float MAX_TICK_STEP = 150.0f;                         // a longer step is a teleport or a flight

    struct Snapshot
    {
        uint8 status = 0;
        uint16 counts[QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT] = {};
    };

    struct Counters
    {
        uint64 xpQuest = 0, xpKill = 0, xpOther = 0;
        uint32 kills = 0, useful = 0, turnIns = 0;
        double walked = 0.0;
        // [pace23] the position samples: the moving ones (moving at both ends) against the run speed, and the
        // seconds standing in a walking state (IDLE, GO_*, DO_QUEST, WANDER_*, DO_GATHER, DO_PROFESSION)
        double movingYd = 0.0;
        double movingRunYd = 0.0;  // what its run speed covers in those samples' time
        uint64 movingMs = 0;
        uint64 standingMs = 0;
        uint64 walkFlagMs = 0;  // moving with the walk flag set
        uint32 sampleMs = 0;
        bool sampleMoving = false;
        uint32 mapId = 0;
        float x = 0.0f, y = 0.0f;
        bool hasPos = false;
        uint32 lastLog = 0;
        uint64 lastQuestXp = 0;
        bool hasLog = false;
        std::unordered_map<uint32, Snapshot> log;  // the quest log at the last rpg tick
        // [budget12] the time budget: milliseconds per bucket since login, the last tick's bucket and time
        std::map<std::string, uint64> budget;
        std::string lastBucket;
        uint32 lastTickMs = 0;
        uint32 combatMs = 0;  // [budget12] the combat time noted since the last tick (QuestStats::NoteCombat)
        uint32 deadMs = 0;
        uint32 diedAtMs = 0;  // [budget12] its death's time while it is dead
    };

    std::mutex lock;
    std::unordered_map<uint32, Counters> counters;  // bot guid counter

    bool Tracked(Player* player) { return player && GET_PLAYERBOT_AI(player); }

    void LogCounters(Player* bot, Counters const& c, char const* why)
    {
        std::string budget;
        for (auto const& [bucket, ms] : c.budget)
            budget += (budget.empty() ? "" : ",") + bucket + ":" + std::to_string(ms / IN_MILLISECONDS);
        LOG_INFO("playerbots",
                 "[qstat] {} L{} {} xp_quest={} xp_kill={} xp_other={} kills={} useful={} walked={} turnins={} "
                 "budget={}",
                 bot->GetName(), bot->GetLevel(), why, c.xpQuest, c.xpKill, c.xpOther, c.kills, c.useful,
                 uint64(c.walked), c.turnIns, budget.empty() ? "-" : budget);
        // [pace23] the samples' measure of its walk: straight progress per moving second against its run, the
        // seconds it stood in a walking state between the moves, the seconds it moved with the walk flag
        LOG_INFO("playerbots",
                 "[pace23] {} L{} moving {:.0f} s covered {:.0f} yd ({:.0f} % of its run); stood {:.0f} s in walking "
                 "states; walk flag {:.0f} s",
                 bot->GetName(), bot->GetLevel(), double(c.movingMs) / IN_MILLISECONDS, c.movingYd,
                 c.movingRunYd > 0.0 ? 100.0 * c.movingYd / c.movingRunYd : 0.0,
                 double(c.standingMs) / IN_MILLISECONDS, double(c.walkFlagMs) / IN_MILLISECONDS);
    }

    // the kill served a quest: its entry (or kill credit) is an objective of a quest in the log not yet past its
    // count before this kill, or it carries loot a quest in the log still needs
    bool ServedQuest(Player* bot, Creature* killed)
    {
        CreatureTemplate const* tmpl = killed->GetCreatureTemplate();
        if (!tmpl)
            return false;
        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE && status.Status != QUEST_STATUS_COMPLETE)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const entry = quest->RequiredNpcOrGo[i];
                if (entry <= 0 || !quest->RequiredNpcOrGoCount[i])
                    continue;
                bool const match = uint32(entry) == tmpl->Entry || uint32(entry) == tmpl->KillCredit[0] ||
                                   uint32(entry) == tmpl->KillCredit[1];
                // the credit already landed (the hook runs after it): a count still short, or just filled
                if (match && status.CreatureOrGOCount[i] <= quest->RequiredNpcOrGoCount[i] &&
                    status.CreatureOrGOCount[i] > 0)
                    return true;
            }
        }
        return tmpl->lootid && LootTemplates_Creature.HaveQuestLootForPlayer(tmpl->lootid, bot);
    }

    class QuestStatsPlayerScript : public PlayerScript
    {
    public:
        QuestStatsPlayerScript()
            : PlayerScript("QuestStatsPlayerScript",
                           {PLAYERHOOK_ON_GIVE_EXP, PLAYERHOOK_ON_CREATURE_KILL_CREDIT,
                            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LOGOUT,
                            PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_PLAYER_RESURRECT})
        {
        }

        // [budget12] the time dead: from the death to the rise (Player::KillPlayer / ResurrectPlayer)
        void OnPlayerJustDied(Player* player) override
        {
            if (!Tracked(player))
                return;
            std::lock_guard<std::mutex> guard(lock);
            counters[player->GetGUID().GetCounter()].diedAtMs = getMSTime();
        }

        void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
        {
            if (!Tracked(player))
                return;
            std::lock_guard<std::mutex> guard(lock);
            Counters& c = counters[player->GetGUID().GetCounter()];
            if (c.diedAtMs)
                c.deadMs += getMSTimeDiff(c.diedAtMs, getMSTime());
            c.diedAtMs = 0;
        }

        void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 xpSource) override
        {
            if (!Tracked(player))
                return;
            std::lock_guard<std::mutex> guard(lock);
            Counters& c = counters[player->GetGUID().GetCounter()];
            if (xpSource == XPSOURCE_KILL)
                c.xpKill += amount;
            else if (xpSource == XPSOURCE_QUEST || xpSource == XPSOURCE_QUEST_DF)
            {
                c.xpQuest += amount;
                c.lastQuestXp = amount;
            }
            else
                c.xpOther += amount;
        }

        void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
        {
            if (!Tracked(player) || !killed)
                return;
            bool const served = ServedQuest(player, killed);
            {
                std::lock_guard<std::mutex> guard(lock);
                Counters& c = counters[player->GetGUID().GetCounter()];
                ++c.kills;
                if (served)
                    ++c.useful;
            }
            QuestStats::LogEvent(player, "kill", 0,
                                 "entry=" + std::to_string(killed->GetEntry()) + " L" +
                                     std::to_string(killed->GetLevel()) + " useful=" + (served ? "1" : "0"));
        }

        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            if (!Tracked(player) || !quest)
                return;
            uint64 xp = 0;
            {
                std::lock_guard<std::mutex> guard(lock);
                Counters& c = counters[player->GetGUID().GetCounter()];
                ++c.turnIns;
                xp = c.lastQuestXp;  // GiveXP runs before this hook in Player::RewardQuest
                c.lastQuestXp = 0;
                c.log.erase(quest->GetQuestId());
            }
            QuestStats::LogEvent(player, "turnin", quest->GetQuestId(), "xp=" + std::to_string(xp));
            QuestRoute::NoteQuestEvent(player, quest->GetQuestId());  // [routeplan] metric only: the stop's end
        }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldlevel*/) override
        {
            if (!Tracked(player))
                return;
            std::lock_guard<std::mutex> guard(lock);
            LogCounters(player, counters[player->GetGUID().GetCounter()], "level");
        }

        void OnPlayerLogout(Player* player) override
        {
            if (!Tracked(player))
                return;
            std::lock_guard<std::mutex> guard(lock);
            auto const found = counters.find(player->GetGUID().GetCounter());
            if (found != counters.end())
                LogCounters(player, found->second, "logout");
        }
    };
}

uint64 QuestStats::NowMs()
{
    return uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count());
}

void QuestStats::LogEvent(Player* bot, char const* event, uint32 questId, std::string const& extra)
{
    LOG_INFO("playerbots", "[quest] {} {} L{} q{} t={} at=({:.0f},{:.0f}){}{}", event, bot->GetName(),
             bot->GetLevel(), questId, NowMs(), bot->GetPositionX(), bot->GetPositionY(), extra.empty() ? "" : " ",
             extra);
}

void QuestStats::NoteCombat(Player* bot, uint32 combatMs, bool died)
{
    if (!Tracked(bot))
        return;
    std::lock_guard<std::mutex> guard(lock);
    Counters& c = counters[bot->GetGUID().GetCounter()];
    (died ? c.deadMs : c.combatMs) += combatMs;
}

void QuestStats::OnRpgTick(Player* bot, uint32 /*activeQuest*/, char const* bucket)
{
    // the quest log against the one of the last tick: accepted, progressed (per objective), completed, abandoned
    std::unordered_map<uint32, Snapshot> now;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        auto const itr = bot->getQuestStatusMap().find(questId);
        if (itr == bot->getQuestStatusMap().end())
            continue;
        Snapshot& snap = now[questId];
        snap.status = uint8(itr->second.Status);
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            snap.counts[i] = itr->second.CreatureOrGOCount[i];
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            snap.counts[QUEST_OBJECTIVES_COUNT + i] = uint16(itr->second.ItemCount[i]);
    }

    std::vector<std::pair<char const*, std::pair<uint32, std::string>>> events;
    {
        std::lock_guard<std::mutex> guard(lock);
        Counters& c = counters[bot->GetGUID().GetCounter()];
        uint32 const sampleNow = getMSTime();
        bool const movingNow = bot->isMoving();
        if (c.hasPos && c.mapId == bot->GetMapId())
        {
            float const step = std::hypot(bot->GetPositionX() - c.x, bot->GetPositionY() - c.y);
            if (step < MAX_TICK_STEP)
                c.walked += step;
            // [pace23]
            uint32 const dt = c.sampleMs ? getMSTimeDiff(c.sampleMs, sampleNow) : 0;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            NewRpgStatus const status = ai ? ai->rpgInfo.GetStatus() : RPG_IDLE;
            bool const walkingState = status != RPG_REST && status != RPG_TRAVEL_FLIGHT &&
                                      status != RPG_OUTDOOR_PVP && !bot->IsInCombat() && !bot->IsInFlight();
            if (dt && dt < 30 * IN_MILLISECONDS && step < MAX_TICK_STEP)
            {
                if (c.sampleMoving && movingNow)
                {
                    c.movingMs += dt;
                    c.movingYd += step;
                    c.movingRunYd += bot->GetSpeed(MOVE_RUN) * float(dt) / float(IN_MILLISECONDS);
                    if (bot->IsWalking())
                        c.walkFlagMs += dt;
                }
                else if (!c.sampleMoving && !movingNow && walkingState && !bot->IsSitState() &&
                         !bot->IsNonMeleeSpellCast(false))
                    c.standingMs += dt;
            }
        }
        c.sampleMs = sampleNow;
        c.sampleMoving = movingNow;
        c.hasPos = true;
        c.mapId = bot->GetMapId();
        c.x = bot->GetPositionX();
        c.y = bot->GetPositionY();

        if (c.hasLog)
        {
            for (auto const& [questId, snap] : now)
            {
                auto const before = c.log.find(questId);
                if (before == c.log.end())
                {
                    events.push_back({"accept", {questId, ""}});
                    continue;
                }
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                for (uint8 i = 0; quest && i < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (snap.counts[i] > before->second.counts[i])
                    {
                        uint32 const required = i < QUEST_OBJECTIVES_COUNT
                                                    ? quest->RequiredNpcOrGoCount[i]
                                                    : quest->RequiredItemCount[i - QUEST_OBJECTIVES_COUNT];
                        events.push_back({"progress", {questId, "obj=" + std::to_string(i) + " " +
                                                                    std::to_string(snap.counts[i]) + "/" +
                                                                    std::to_string(required)}});
                    }
                if (snap.status == QUEST_STATUS_COMPLETE && before->second.status != QUEST_STATUS_COMPLETE)
                    events.push_back({"complete", {questId, ""}});
            }
            for (auto const& [questId, snap] : c.log)
                if (!now.count(questId) && !bot->GetQuestRewardStatus(questId))
                    events.push_back({"abandon", {questId, ""}});
        }
        c.log = std::move(now);
        c.hasLog = true;

        uint32 const ms = getMSTime();
        // [budget12] the gap since the last tick: its fights and its time dead as noted, the rest the last bucket's
        if (!c.diedAtMs && !bot->IsAlive())  // logged in dead: its time dead from here
            c.diedAtMs = ms;
        if (c.diedAtMs)  // still dead (a ghost's tick): its time so far
        {
            c.deadMs += getMSTimeDiff(c.diedAtMs, ms);
            c.diedAtMs = ms;
        }
        if (c.lastTickMs)
        {
            uint32 const gap = getMSTimeDiff(c.lastTickMs, ms);
            uint32 const fight = std::min(gap, c.combatMs);
            uint32 const dead = std::min(gap - fight, c.deadMs);
            c.budget["COMBAT"] += fight;
            c.budget["DEAD"] += dead;
            c.budget[c.lastBucket.empty() ? std::string("IDLE") : c.lastBucket] += gap - fight - dead;
        }
        c.combatMs = 0;
        c.deadMs = 0;
        c.lastTickMs = ms;
        c.lastBucket = bucket ? bucket : "IDLE";
        if (!c.lastLog || getMSTimeDiff(c.lastLog, ms) >= QSTAT_LOG_MS)
        {
            c.lastLog = ms;
            LogCounters(bot, c, "tick");
        }
    }
    for (auto const& [event, data] : events)
    {
        LogEvent(bot, event, data.first, data.second);
        QuestRoute::NoteQuestEvent(bot, data.first);  // [routeplan] metric only: the route stop's end
    }
}

void AddQuestStatsScripts() { new QuestStatsPlayerScript(); }
