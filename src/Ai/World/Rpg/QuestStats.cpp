/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestStats.h"

#include "Creature.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "Timer.h"

#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr uint32 QSTAT_LOG_MS = 5 * MINUTE * IN_MILLISECONDS;  // a log cadence, no decision reads it
    constexpr float MAX_TICK_STEP = 150.0f;                         // a longer step is a teleport or a flight

    struct Counters
    {
        uint64 xpQuest = 0, xpKill = 0, xpOther = 0;
        uint32 kills = 0, useful = 0, turnIns = 0;
        double walked = 0.0;
        uint32 mapId = 0;
        float x = 0.0f, y = 0.0f;
        bool hasPos = false;
        uint32 lastLog = 0;
        uint32 progressQuest = 0, progressDone = 0;
    };

    std::mutex lock;
    std::unordered_map<uint32, Counters> counters;  // bot guid counter

    bool Tracked(Player* player) { return player && GET_PLAYERBOT_AI(player); }

    void LogCounters(Player* bot, Counters const& c, char const* why)
    {
        LOG_INFO("playerbots",
                 "[qstat] {} L{} {} xp_quest={} xp_kill={} xp_other={} kills={} useful={} walked={} turnins={}",
                 bot->GetName(), bot->GetLevel(), why, c.xpQuest, c.xpKill, c.xpOther, c.kills, c.useful,
                 uint64(c.walked), c.turnIns);
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
                            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LOGOUT})
        {
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
                c.xpQuest += amount;
            else
                c.xpOther += amount;
        }

        void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
        {
            if (!Tracked(player) || !killed)
                return;
            bool const served = ServedQuest(player, killed);
            std::lock_guard<std::mutex> guard(lock);
            Counters& c = counters[player->GetGUID().GetCounter()];
            ++c.kills;
            if (served)
                ++c.useful;
        }

        void OnPlayerCompleteQuest(Player* player, Quest const* /*quest*/) override
        {
            if (!Tracked(player))
                return;
            std::lock_guard<std::mutex> guard(lock);
            ++counters[player->GetGUID().GetCounter()].turnIns;
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

void QuestStats::OnRpgTick(Player* bot, uint32 activeQuest)
{
    // the active quest's progress (its objective counts) - one line when it changes
    uint32 done = 0, required = 0;
    if (activeQuest)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(activeQuest);
        auto const itr = bot->getQuestStatusMap().find(activeQuest);
        if (quest && itr != bot->getQuestStatusMap().end())
        {
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i])
                {
                    required += quest->RequiredNpcOrGoCount[i];
                    done += std::min<uint32>(itr->second.CreatureOrGOCount[i], quest->RequiredNpcOrGoCount[i]);
                }
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
                {
                    required += quest->RequiredItemCount[i];
                    done += std::min<uint32>(itr->second.ItemCount[i], quest->RequiredItemCount[i]);
                }
        }
    }

    std::lock_guard<std::mutex> guard(lock);
    Counters& c = counters[bot->GetGUID().GetCounter()];
    if (c.hasPos && c.mapId == bot->GetMapId())
    {
        float const step = std::hypot(bot->GetPositionX() - c.x, bot->GetPositionY() - c.y);
        if (step < MAX_TICK_STEP)
            c.walked += step;
    }
    c.hasPos = true;
    c.mapId = bot->GetMapId();
    c.x = bot->GetPositionX();
    c.y = bot->GetPositionY();

    if (activeQuest && required && (activeQuest != c.progressQuest || done != c.progressDone))
    {
        if (activeQuest == c.progressQuest && done > c.progressDone)
            LOG_INFO("playerbots", "[qprog] {} L{} q{} {}/{}", bot->GetName(), bot->GetLevel(), activeQuest, done,
                     required);
        c.progressQuest = activeQuest;
        c.progressDone = done;
    }

    uint32 const now = getMSTime();
    if (!c.lastLog || getMSTimeDiff(c.lastLog, now) >= QSTAT_LOG_MS)
    {
        c.lastLog = now;
        LogCounters(bot, c, "tick");
    }
}

void AddQuestStatsScripts() { new QuestStatsPlayerScript(); }
