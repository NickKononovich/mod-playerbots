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
// tagged as deaths were deaths PREVENTED against 17 real ones. So standing is NOT reported when the bot is not
// standing at all: dead or a ghost, sitting (eating, drinking, resting), casting, being teleported, or in flight.
// Those are facts about this instant, read here and nowhere else. Standing IN COMBAT is reported, with
// `in_combat` in the row: a bot that stands through a fight is exactly the case being looked for.
//
// [jrn42] AND WHEN THE BOT REALLY DOES STAND, BUT LEGITIMATELY, THE ROW SAYS SO AND STAYS. Its leader ordered it
// to stand, it waits on the pier for a transport, its far walk waits for the group: `held_by` names which, the
// issue row is marked `is_real='no'` with that name, and the duration goes on being measured. 85.5 % of the
// out-of-combat standing rows inside Wailing Caverns on b502 lay under a `pull` verdict of `held` by the leader of
// their own run, so without the name one leader hold was counted again as a problem of each of five members.
//
// THE REASON COMES FROM WHAT IS ALREADY KNOWN AT THIS INSTANT - the engine's own last action and its state, read
// through public accessors. The world is NOT read a second time: a reading at another moment would belong to a
// different instant than the standing it claims to explain, which is the same second-judge defect again.

#include "Journal.h"

#include "IssueKey.h"         // [jrn41] the deduplication key, as a seam a test can assert
#include "StandingEpisode.h"  // [jrn41] the episode's arithmetic, as a seam a test can assert

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

    // [jrn41] the episode's own arithmetic lives in the seam (StandingEpisode.h): the anchor carries its MAP, and
    // the reported radius is the one the bot stayed inside - the two defects measured on b501's live rows
    using Episode = Journal::Standing::Anchor;

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
    void Report(Player* bot, PlayerbotAI* botAI, Journal::Standing::Look const& look)
    {
        float const stoodSec = look.stoodSec;
        bool const ended = look.ended;
        std::string const trail =
            botAI->GetCurrentEngine() ? botAI->GetCurrentEngine()->GetLastAction() : std::string();
        // [jrn42] THE NEWEST END OF THE TRAIL, because the column is read as "its engine's last action".
        // Engine::LogAction keeps the trail's newest 512 characters, so its END is the newest; both columns that
        // carried it were keeping the oldest. Measured on b502: `doing` (96) held the OLDEST 96 characters in 837
        // of 1032 standing rows, and `why` (512) had been filled to the brim in 170 of 1029, which pushed the
        // newest ~110 characters of the trail off the end of the column that names it. A truncation that keeps
        // the wrong end is worse than a missing value, because a reader of it answers.
        std::string const doing = Journal::Standing::TrailTail(trail, 96);
        std::string const heldBy = look.heldBy;

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
        // the radius it stayed inside: the pair is the fact, neither number alone is. [jrn41] it is the widest
        // distance seen WHILE INSIDE the radius, never the step that took it out - the seam holds that
        idle.stoodYd = look.stoodYd;
        idle.doing = doing;
        idle.heldBy = heldBy;  // [jrn42] "" = checked, nothing held it still; a name = this hold did
        idle.runId = Leader::Runs::OfBot(bot);  // [jrn40] by identity, not by nearness in time
        // [jrn42] the prose first and the trail with the room that is left, so that MySQL never does the cutting:
        // its cut takes the end of the string, which here is the NEWEST end of the trail - the one the sentence
        // promises. 512 is the column, and this is the one place that knows it.
        std::string const prose = Acore::StringFormat(
            "{} for {:.0f} s within {:.1f} yd (state {}, {}{})", ended ? "stood" : "has been standing", stoodSec,
            look.stoodYd, uint32(botAI->GetState()), bot->IsInCombat() ? "in combat" : "out of combat",
            heldBy.empty() ? std::string() : ", held: " + heldBy);
        std::string const tail = trail.empty() ? std::string(", and its engine had no action to name")
                                               : ", its engine's last action: ";
        idle.why = prose + tail;
        if (!trail.empty() && idle.why.size() < 512)
            idle.why += Journal::Standing::TrailTail(trail, 512 - idle.why.size());
        uint64 const sourceId = Journal::RecordIdle(idle);

        Journal::Issue issue;
        issue.kind = "standing";
        issue.severity = bot->IsInCombat() ? "high" : "mid";
        // A property of the world over time: there is nothing to replay offline, so a test cannot be the proof -
        // a run in which the sign is gone is (see Journal::Issue::reproClass).
        issue.reproClass = "not_offline";
        // THE KEY IS THE CAUSE, NOT THE LINE, and it is built in the seam (IssueKey.h) so that a test can assert
        // it in both directions. [jrn41] THE ENGINE'S ACTION TRACE WAS IN IT AND IS NOT ANY MORE: `doing` changes
        // on every sample of one episode, so Kylantino's single episode on map 369 - the same position to four
        // decimals, 10 s to 60 s - became five rows in this table. The trace stays in `why` and in
        // pb_journal_idle.doing, where it is read by eye in a row already found.
        // [jrn42] ... and the hold is in it, which is the first CAUSE field this key has had
        issue.dedupKey =
            Journal::Key::Standing(*idle.role, bot->IsInCombat(), uint32(botAI->GetState()), heldBy, doing);
        issue.why = idle.why;
        issue.realWhen = "alive, not sitting, not casting, not in flight, not being teleported, under no order to "
                         "stand and waiting for no transport or group - and still inside max(combat reach, "
                         "1.5 yd) for the whole episode";
        // the criterion above IS checked at runtime, so the emitter can answer rather than leaving "unknown"
        // [jrn42] AND A NAMED HOLD ANSWERS `no`, NOT A MISSING ROW. The user's rule for this table is that a row
        // recognised as not-a-problem is MARKED and STAYS (better to over-report than to miss; `is_real` is a
        // column and not a gate), so the duration of a legitimate wait keeps being measured while the count that is
        // driven to zero - is_real IN ('yes','unknown') - stops counting it. The problem a leader hold really is
        // belongs to the leader and is already recorded, with its own `why`, in pb_journal_decision.
        issue.real = heldBy.empty() ? "yes" : "no";
        issue.realWhy = heldBy.empty()
                            ? std::string("the bot held a position inside its own combat reach while nothing "
                                          "legitimately held it still, and the duration is in the row")
                            : "it stood in one place because " + heldBy +
                                  ": the duration is in the row, and the problem, if there is one, belongs to "
                                  "whatever issued that hold and is recorded there";
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
        uint64 const stoodMs = uint64(stoodSec * 1000.0f);
        issue.lastProgressMs = Journal::ClockMs() > stoodMs ? Journal::ClockMs() - stoodMs : 0;
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
        // [jrn42] AND EACH NOW HAS A NAME IN THE ROW. Folded into one anonymous bool they could only drop the
        // episode, so the row went on claiming "nothing legitimately held it still" for the three holds that were
        // not in the list at all, and nobody could ask which hold it was. Measured on b502: 931 of 1089
        // out-of-combat standing rows inside Wailing Caverns lay in a window in which the leader of that very run
        // had recorded a `pull` verdict of `held`, so ONE leader hold was being counted again as a problem of each
        // of the five members waiting under it (trap 1: count events, not rows); and Ganna's single 120 s wait
        // on the Deeprun Tram platform, fifteen seconds before it boarded car 176084, was twelve problems.
        Journal::Standing::Holds holds;
        holds.dead = !bot->IsAlive();
        holds.sitting = bot->IsSitState();
        holds.casting = bot->IsNonMeleeSpellCast(false);
        holds.inFlight = bot->IsInFlight();
        holds.beingTeleported = bot->IsBeingTeleported();
        // The leader's own command is the one judge of "it was told to be here": read from the member's events,
        // where the executor reads it (LeaderMoveOrderAction acks "on the spot"), never recomputed. A Hold is an
        // order not to move; a StandAt / MoveTo the member has already reached leaves it nothing more to do.
        // Splitting those two names apart is the next narrowing if the counts ask for it.
        if (Leader::MemberEvents events; Leader::Events::Get(bot->GetGUID(), events))
            holds.orderedToStand =
                events.move == Leader::MoveEvent::Hold ||
                ((events.move == Leader::MoveEvent::StandAt || events.move == Leader::MoveEvent::MoveTo) &&
                 bot->GetExactDist2d(events.spot.GetPositionX(), events.spot.GetPositionY()) <= events.tolerance);
        // On the pier / platform until the boat, zeppelin or tram docks, and the far walk that waits with the
        // group - both are states the travel domain already keeps, read here and not guessed from a duration
        holds.waitsForTransport = botAI->rpgInfo.trip.phase == NewRpgInfo::TripPhase::Wait ||
                                  botAI->rpgInfo.trip.phase == NewRpgInfo::TripPhase::Board;
        holds.waitsForGroup = botAI->rpgInfo.groupWaiting;

        // [jrn41] the sample is a set of facts; the episode's arithmetic is in the seam, where a test holds it.
        // What is read here is read ONCE and at this instant - the same rule as everywhere in this file.
        Journal::Standing::Sample sample;
        sample.nowMs = nowMs;
        sample.mapId = bot->GetMapId();
        sample.x = bot->GetPositionX();
        sample.y = bot->GetPositionY();
        sample.z = bot->GetPositionZ();
        sample.holds = holds;
        // The radius is NOT a new number: it is max(GetCombatReach(), 1.5f), the expression the loop detector in
        // GuildCoordinator already uses for "same place".
        sample.radius = std::max(bot->GetCombatReach(), 1.5f);
        sample.cadenceMs = CadenceMs();

        Journal::Standing::Look look;
        {
            std::lock_guard<std::mutex> lock(shard.lock);
            // The episode is over when the bot leaves the radius, when the bot is not standing at all, or when it
            // is on another map at all. [jrn42] "Not standing at all" is dead / sitting / casting / in flight /
            // being teleported (Journal::Standing::EndsTheEpisode); the three waits - its leader's order, a
            // transport to dock, its group - are a bot that really does stand, so they NAME the episode and do
            // not end it, and the duration stays the true one instead of one cadence-long row per sample.
            // Such a bot ENDS the episode rather than pausing it: the stretch before the
            // hold was real standing and is reported with its own duration, while a duration joined across a hold
            // would be a number about nothing. An ending episode is reported only if it lasted at least one
            // instrument window - the one floor this detector has, and it is the sampler's own cadence, not a rule
            // about bots. While it is open it is reported once per window, so an episode that never ends is
            // visible within one window instead of only at an end that never comes; the issue row deduplicates
            // them, so a ten-minute hang is ONE problem whose duration range tops out at 600 s.
            look = Journal::Standing::Observe(shard.episodes[low], sample);
        }
        if (look.report)
            Report(bot, botAI, look);
    }

    void ForgetStanding(uint32 botLow)
    {
        Shard& shard = Shards()[botLow % SHARDS];
        std::lock_guard<std::mutex> lock(shard.lock);
        shard.episodes.erase(botLow);
    }
}  // namespace Journal
