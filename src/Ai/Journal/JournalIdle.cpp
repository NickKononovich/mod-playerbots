/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn40] THE STANDING WATCH: a bot that has not left a small radius, and the reason, as a row.
//
// The user asked for it twice and in two numbers: "if it stands in one place for more than a minute, STANDING
// exactly, then for what reason", and later "right now it stands in one place doing nothing for more than 30
// seconds. For what reason?" - and then "the 30 seconds was figurative, not a hard requirement; what matters is
// that we can find the cause afterwards".
//
// SO NO THRESHOLD IS COMPILED IN. The row carries the DURATION (`stood_sec`, and `still_ms` in the issue's value
// range), and "longer than 30 seconds" is a WHERE clause. That is the same move as writing a window instead of
// implying one: the boundary comes out of the data, it can be changed without a build, and the user's rule that no
// elapsed-time rule may change a bot decision is not even approached - nothing here writes to a bot, an engine or
// an order.
//
// THE ONE NUMBER THAT IS UNAVOIDABLE, named rather than hidden: the instrument cannot see an episode shorter than
// its own reporting cadence, which is the journal's single statistics window (AiPlayerbot.Journal.StatsSeconds,
// 10 s by default). An episode is reported once per window while it is open and once when it ends, so a 10-minute
// hang is ONE issue row whose value range tops out at 600 s - not sixty rows. That cadence is a property of the
// sampler and is reused, not invented: the journal has one instrument window, not two.
//
// THE RADIUS IS NOT A NEW NUMBER EITHER. It is max(GetCombatReach(), 1.5f), which is exactly the expression the
// loop detector in GuildCoordinator already uses for "same place". A second radius for the same idea would be two
// judges of one quantity, which is a class of defect this project counts (five confirmed cases).
//
// WHAT MAKES IT A REAL PROBLEM, stated because an instrument that reports its own clean results poisons the count:
// 406 of 3 900 idle-diagnostics rows had every bucket at zero - a clean result, not a problem - and 20 502 lines
// tagged as deaths were deaths PREVENTED against 17 real ones. So standing is NOT reported when the bot is
// legitimately held still: dead or a ghost, sitting (eating, drinking, resting), casting, being teleported, or in
// flight. Those are facts about this instant, read here and nowhere else. Standing IN COMBAT is reported, with
// `in_combat` in the row: a bot that stands through a fight is exactly the case being looked for.
//
// THE REASON COMES FROM WHAT IS ALREADY KNOWN AT THIS INSTANT - the engine's own last action and its state, read
// through public accessors. The world is NOT read a second time: a reading at another moment would belong to a
// different instant than the standing it claims to explain, which is the same second-judge defect again.

#include "Journal.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Config.h"
#include "Engine.h"
#include "LeaderOrders.h"  // [jrn40] Leader::Runs::OfBot
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "Timer.h"

namespace
{
    // The cadence is read where every other journal setting is read - at use time, never during static
    // initialisation (asking ConfigMgr from a file-scope initialiser reads unconstructed containers: an access
    // violation, not an exception).
    uint32 CadenceMs()
    {
        static uint32 const ms = []
        {
            int32 const seconds = sConfigMgr->GetOption<int32>("AiPlayerbot.Journal.StatsSeconds", 10, false);
            return uint32(seconds > 0 ? seconds : 10) * 1000u;
        }();
        return ms;
    }

    bool Enabled()
    {
        static bool const on = sConfigMgr->GetOption<bool>("AiPlayerbot.Journal.Stats", true, false);
        return on;
    }

    struct Episode
    {
        uint32 sinceMs = 0;      // when the bot last moved out of its radius
        uint32 reportedMs = 0;   // when this episode was last written
        float x = 0.f, y = 0.f, z = 0.f;
    };

    // One lock for every map thread would be contended on a per-player-per-tick path, so the state is sharded by
    // the bot's GUID counter. A bot stays on one map thread, so its shard is touched by one thread at a time in
    // practice; the lock is there for the rare migration and for the shard's neighbours.
    constexpr size_t SHARDS = 32;

    struct Shard
    {
        std::mutex lock;
        std::unordered_map<uint32, Episode> episodes;
    };

    std::array<Shard, SHARDS>& Shards()
    {
        static std::array<Shard, SHARDS> shards;
        return shards;
    }

    std::string RoleOf(Player* bot)
    {
        if (PlayerbotAI::IsTank(bot))
            return "tank";
        if (PlayerbotAI::IsHeal(bot))
            return "healer";
        return "dps";
    }

    // Writes the episode out: one domain row with the duration and the reason, and one issue row pointing at it.
    void Report(Player* bot, PlayerbotAI* botAI, Episode const& episode, uint32 nowMs, float movedYd, bool ended)
    {
        float const stoodSec = getMSTimeDiff(episode.sinceMs, nowMs) / 1000.0f;
        std::string const doing =
            botAI->GetCurrentEngine() ? botAI->GetCurrentEngine()->GetLastAction() : std::string();

        Journal::Idle idle;
        idle.event = "standing";
        idle.botName = bot->GetName();
        idle.botLow = bot->GetGUID().GetCounter();
        idle.botLevel = bot->GetLevel();
        idle.role = RoleOf(bot);
        idle.mapId = bot->GetMapId();
        idle.zoneId = bot->GetZoneId();
        idle.x = bot->GetPositionX();
        idle.y = bot->GetPositionY();
        idle.z = bot->GetPositionZ();
        idle.inDungeon = bot->GetMap() && bot->GetMap()->IsDungeon();
        idle.inCombat = bot->IsInCombat();
        idle.stoodSec = stoodSec;
        idle.stoodYd = movedYd;  // the radius it stayed inside: the pair is the fact, neither number alone is
        idle.doing = doing;
        idle.runId = Leader::Runs::OfBot(bot);  // [jrn40] by identity, not by nearness in time
        idle.why = Acore::StringFormat(
            "{} for {:.0f} s within {:.1f} yd (state {}, {}){}", ended ? "stood" : "has been standing", stoodSec,
            movedYd, uint32(botAI->GetState()), bot->IsInCombat() ? "in combat" : "out of combat",
            doing.empty() ? std::string(", and its engine had no action to name")
                          : ", its engine's last action: " + doing);
        uint64 const sourceId = Journal::RecordIdle(idle);

        Journal::Issue issue;
        issue.kind = "standing";
        issue.severity = bot->IsInCombat() ? "high" : "mid";
        // A property of the world over time: there is nothing to replay offline, so a test cannot be the proof -
        // a run in which the sign is gone is (see Journal::Issue::reproClass).
        issue.reproClass = "not_offline";
        // THE KEY IS THE CAUSE, NOT THE LINE. Measured on the idle diagnostics: keying on the whole line gave 757
        // "causes" over 3 900 rows and keying on the cause alone gave one - the 757 were a trailing list of action
        // names, which is trap #2 in new clothes. So the key here is the engine's last action, the engine state and
        // whether it was a fight: no name, no coordinates, no duration. The duration's spread is the value range.
        issue.dedupKey = "standing|" + std::string(bot->IsInCombat() ? "combat" : "peace") + "|state " +
                         std::to_string(uint32(botAI->GetState())) + "|" + (doing.empty() ? "no action" : doing);
        issue.why = idle.why;
        issue.realWhen = "alive, not sitting, not casting, not in flight, not being teleported - and still inside "
                         "max(combat reach, 1.5 yd) for the whole episode";
        // the criterion above IS checked at runtime, so the emitter can answer rather than leaving "unknown"
        issue.real = "yes";
        issue.realWhy = "the bot held a position inside its own combat reach while nothing legitimately held it "
                        "still, and the duration is in the row";
        issue.realSource = "emitter";
        issue.value = double(stoodSec);
        issue.valueUnit = "sec";
        issue.botName = idle.botName;
        issue.botLow = idle.botLow;
        issue.mapId = idle.mapId;
        issue.zoneId = idle.zoneId;
        issue.x = idle.x;
        issue.y = idle.y;
        issue.z = idle.z;
        issue.sourceTable = "pb_journal_idle";
        issue.sourceId = sourceId;
        issue.runId = idle.runId;
        // the window of the slice comes from the data: the episode's own start is the last moment this bot was
        // demonstrably making progress through space
        issue.lastProgressMs = Journal::ClockMs() > uint64(getMSTimeDiff(episode.sinceMs, nowMs))
                                   ? Journal::ClockMs() - uint64(getMSTimeDiff(episode.sinceMs, nowMs))
                                   : 0;
        Journal::RecordIssue(issue);
    }
}  // namespace

namespace Journal
{
    void NoteStanding(Player* bot)
    {
        if (!Enabled() || !bot || !bot->IsInWorld())
            return;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;  // a real player's standing is not this instrument's subject

        uint32 const nowMs = getMSTime();
        uint32 const low = bot->GetGUID().GetCounter();
        Shard& shard = Shards()[low % SHARDS];

        // WHAT IS NOT A PROBLEM, so that an empty result reads as "it did not happen" and not as "we did not look".
        // Each of these is a legitimate reason to be still, and each is a fact about this instant.
        bool const held = !bot->IsAlive() || bot->IsSitState() || bot->IsNonMeleeSpellCast(false) ||
                          bot->IsInFlight() || bot->IsBeingTeleported();

        float const radius = std::max(bot->GetCombatReach(), 1.5f);
        Episode snapshot;
        bool report = false;
        bool ended = false;
        float movedYd = 0.f;
        {
            std::lock_guard<std::mutex> lock(shard.lock);
            Episode& episode = shard.episodes[low];
            if (!episode.sinceMs)
            {
                episode = Episode{nowMs, nowMs, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
                return;
            }
            movedYd = bot->GetExactDist(episode.x, episode.y, episode.z);
            if (held || movedYd > radius)
            {
                // The episode is over - the bot either moved out of the radius or is now legitimately held still.
                // A held bot ENDS the episode rather than pausing it: the stretch before the hold was real standing
                // and is reported with its own duration, while a duration joined across a hold would be a number
                // about nothing. It is reported only if it lasted at least one instrument window, which is the one
                // floor this detector has and it is the sampler's own cadence, not a rule about bots.
                snapshot = episode;
                report = getMSTimeDiff(episode.sinceMs, nowMs) >= CadenceMs();
                ended = true;
                episode = Episode{nowMs, nowMs, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
            }
            else if (getMSTimeDiff(episode.reportedMs, nowMs) >= CadenceMs())
            {
                // Still standing. One row per instrument window, so an episode that never ends is visible within
                // one window instead of only at an end that never comes; the issue row deduplicates them, so a
                // ten-minute hang is ONE problem whose duration range tops out at 600 s, not sixty problems.
                episode.reportedMs = nowMs;
                snapshot = episode;
                report = true;
            }
        }
        if (report)
            Report(bot, botAI, snapshot, nowMs, movedYd, ended);
    }

    void ForgetStanding(uint32 botLow)
    {
        Shard& shard = Shards()[botLow % SHARDS];
        std::lock_guard<std::mutex> lock(shard.lock);
        shard.episodes.erase(botLow);
    }
}  // namespace Journal
