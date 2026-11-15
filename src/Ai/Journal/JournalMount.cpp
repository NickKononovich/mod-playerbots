/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [mount9] THE MOUNT STATE'S WATCH: every change of it, with the facts that could justify the change, and - when
// the same justification comes round twice without the bot leaving its own radius - a row in the problems table.
//
// THE USER SAW IT IN THE GAME AND NAMED IT: "they look stuck, they just stand there, propped up against
// something, and they keep getting off the mount and back on. That is a plain bug. I need such cases caught in
// issues."
//
// WHY IT WAS NOT CAUGHT, measured on b504 and worth keeping as a trap of its own: getting onto a mount is a CAST,
// so the standing watch's criterion answers honestly that a casting bot is not idle - and worse, a cast ENDS the
// standing episode, so the duration restarts with every mount. Emdyl held ONE point in Stormwind
// (-8777.3105, 665.5, 103.1), `stood_yd` 0.000 in all 352 of its rows, for 95.4 MINUTES, and the longest episode
// the standing watch ever reported there was 69.9 s; 154 of its 462 rows restart at 10.0 s. Over the whole table,
// standing rows whose trace shows the mount action executing average 17.8 s against 72.6 s for the rest.
// THE GENERAL FORM: LEGITIMACY ESTABLISHED INSIDE ONE WINDOW DOES NOT CARRY OVER TO A SEQUENCE OF WINDOWS.
//
// AND WHY THE STATE IS READ ONCE A TICK RATHER THAN REPORTED BY THE PLACES THAT CHANGE IT. The mount is taken
// away in a dozen actions of this module, by GuildRiding's own paths, and by the CORE itself (a cast that may not
// be made while mounted, walking indoors). The silent ones are exactly the half that matters: a change nobody
// claims is the one no reader can explain. Instrumenting the callers would have measured the half that already
// speaks, and it would have put twenty judges on one quantity (rule 5). One reading of `IsMounted()` on the
// bot's own tick measures all of it, and `why` stays empty when nobody claimed the change - which is a finding.
//
// NOTHING HERE DECIDES ANYTHING. No bot, engine or order reads this file, and no elapsed time appears in it: the
// criterion is "the same justification twice over unchanged inputs", which is arithmetic over recorded facts.

#include "Journal.h"

#include "MountEpisode.h"  // [mount9] the episode's arithmetic and the flip rule, as a seam a test asserts
#include "IssueKey.h"      // [mount9] the deduplication key, likewise

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>

#include "CheckMountStateAction.h"  // the dismount band and the mount leader: ONE judge of each, read not redone
#include "Config.h"
#include "LeaderOrders.h"  // Leader::Runs::OfBot
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "Timer.h"

namespace
{
    constexpr std::size_t SHARDS = 16;

    struct Shard
    {
        std::mutex lock;
        std::unordered_map<uint32, Journal::MountState::Episode> episodes;
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

    // The journal has ONE instrument window and it is reused, not invented: a second cadence for the same idea
    // would be a second judge of "how often do we look". Read at use time, never during static initialisation.
    uint32 CadenceMs()
    {
        static uint32 const ms = []
        {
            int32 const seconds = sConfigMgr->GetOption<int32>("AiPlayerbot.Journal.StatsSeconds", 10, false);
            return uint32(seconds > 0 ? seconds : 10) * 1000u;
        }();
        return ms;
    }
}  // namespace

namespace Journal
{
    void NoteMount(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;  // a real player's riding is not this instrument's subject

        uint32 const low = bot->GetGUID().GetCounter();
        Shard& shard = Shards()[low % SHARDS];
        bool const mountedNow = bot->IsMounted();
        uint32 const nowMs = getMSTime();
        uint32 const cadence = CadenceMs();

        // THE CHEAP PEEK FIRST, because this runs for every bot on every tick: a change of one bool, a map
        // change, or a report falling due. Everything below - the target, the leader, the walk - is read ONLY at
        // the instant it is needed, which is also the only instant those facts belong to.
        {
            std::lock_guard<std::mutex> peek(shard.lock);
            auto const it = shard.episodes.find(low);
            if (it != shard.episodes.end() && it->second.known && it->second.mounted == mountedNow &&
                it->second.mapId == bot->GetMapId() &&
                (it->second.mounts + it->second.dismounts == 0 ||
                 getMSTimeDiff(it->second.reportedMs, nowMs) < cadence))
            {
                // nothing changed and nothing is due. The position still has to be checked, because leaving the
                // radius is what ENDS an episode - and that is two float compares, done below without the facts.
                float const dx = it->second.x - bot->GetPositionX();
                float const dy = it->second.y - bot->GetPositionY();
                float const radius = std::max(bot->GetCombatReach(), 1.5f);
                if (dx * dx + dy * dy <= radius * radius)
                    return;
            }
        }

        AiObjectContext* context = botAI->GetAiObjectContext();

        Journal::MountState::Facts facts;
        facts.indoors = !bot->IsOutdoors();
        facts.inCombat = bot->IsInCombat() || bot->GetVictim();
        facts.inWater = bot->IsInWater();
        facts.onTransport = bot->GetTransport() || bot->GetVehicle() || bot->IsInFlight();
        facts.moving = bot->isMoving();
        // THE DISMOUNT BAND IS NOT RECOMPUTED HERE. It is CheckMountStateAction's own quantity and that action is
        // its judge; this watch asks it (DismountBandYd) instead of keeping a second copy of the expression,
        // which is rule 5 applied to an instrument rather than to behaviour.
        Unit* const target = AI_VALUE(Unit*, "current target");
        float targetYd = 0.f;
        if (target)
        {
            targetYd = bot->GetExactDist(target);
            facts.targetInReach = targetYd <= CheckMountStateAction::DismountBandYd(bot) +
                                                  bot->GetCombatReach() + target->GetCombatReach();
        }
        Player* const leader = CheckMountStateAction::GetMountLeader(botAI);
        facts.hasLeader = leader && leader != bot;
        if (facts.hasLeader)
            facts.leaderRides = CheckMountStateAction::IsMountedOrMounting(leader);
        NewRpgInfo const& rpg = botAI->rpgInfo;
        facts.hasWalk = rpg.mountWalkT != 0;
        float walkLeftYd = 0.f;
        if (facts.hasWalk)
        {
            facts.walkToX = int32_t(rpg.moveFarPos.GetPositionX());
            facts.walkToY = int32_t(rpg.moveFarPos.GetPositionY());
            walkLeftYd = bot->GetExactDist2d(rpg.moveFarPos.GetPositionX(), rpg.moveFarPos.GetPositionY());
        }

        Journal::MountState::Sample sample;
        sample.nowMs = nowMs;
        sample.mapId = bot->GetMapId();
        sample.x = bot->GetPositionX();
        sample.y = bot->GetPositionY();
        sample.z = bot->GetPositionZ();
        sample.mounted = mountedNow;
        sample.facts = facts;
        // The radius is NOT a new number: max(combat reach, 1.5 yd), the expression the standing watch and
        // GuildCoordinator's loop watch already use for "the same place".
        sample.radius = std::max(bot->GetCombatReach(), 1.5f);
        sample.cadenceMs = cadence;

        Journal::MountState::Look look;
        bool real = false;
        {
            std::lock_guard<std::mutex> observe(shard.lock);
            Journal::MountState::Episode& episode = shard.episodes[low];
            look = Journal::MountState::Observe(episode, sample);
            real = Journal::MountState::IsReal(episode);
        }
        if (!look.report)
            return;

        Journal::Mount row;
        row.event = look.changed ? (look.mountedNow ? "mount" : "dismount") : "sample";
        row.botName = bot->GetName();
        row.botLow = low;
        row.botLevel = bot->GetLevel();
        row.role = RoleOf(bot);
        row.mapId = sample.mapId;
        row.zoneId = bot->GetZoneId();
        row.x = sample.x;
        row.y = sample.y;
        row.z = sample.z;
        if (SpellInfo const* spell = CheckMountStateAction::GetMountSpell(bot))
        {
            row.mountSpell = spell->Id;
            row.mountName = spell->SpellName[0] ? spell->SpellName[0] : "";
        }
        row.indoors = facts.indoors;
        row.inCombat = facts.inCombat;
        // NULL and not false when there is no target at all: "no target to be in reach of" and "a target that is
        // not in reach" are different facts, and a query that merges them answers and lies.
        if (target)
            row.targetInReach = facts.targetInReach;
        row.inWater = facts.inWater;
        row.onTransport = facts.onTransport;
        row.moving = facts.moving;
        row.hasLeader = facts.hasLeader;
        if (facts.hasLeader)
        {
            row.leaderRides = facts.leaderRides;
            row.leaderLow = leader->GetGUID().GetCounter();
        }
        row.hasWalk = facts.hasWalk;
        if (facts.hasWalk)
        {
            row.walkToX = rpg.moveFarPos.GetPositionX();
            row.walkToY = rpg.moveFarPos.GetPositionY();
            row.walkLeftYd = walkLeftYd;
        }
        // The reason the code that took it off named, in its own words. Empty means nobody claimed the change -
        // the core took the mount away silently - and that is the finding, not a gap in this row.
        row.why = rpg.dismountReason == "-" ? std::string() : rpg.dismountReason;
        row.episodeMounts = look.mounts;
        row.episodeDismounts = look.dismounts;
        row.episodeFlips = look.flips;
        row.heldSec = look.heldSec;
        row.heldYd = look.heldYd;
        row.justification = look.changed ? look.justification : look.flipUnder;
        row.runId = Leader::Runs::OfBot(bot->GetGUID());
        uint64 const sourceId = Journal::RecordMount(row);

        Journal::Issue issue;
        issue.kind = "mount_flap";
        // A bot that cannot walk because it keeps casting its mount is not idle by any criterion the other
        // detectors hold, and it is standing in the user's own sight: `high` when the circle is proven.
        issue.severity = real ? "high" : "low";
        // THE INPUTS EXIST AND THE JUDGE IS DETERMINISTIC: the facts of every change are in pb_journal_mount, the
        // arithmetic is in the seam, and tests/unit/MountEpisodeTest.cpp replays b504's own rows through it. So
        // this is `decision` and a test can be the proof - unlike the standing watch beside it, whose subject is
        // a property of the world over time.
        issue.reproClass = "decision";
        issue.status = "open";
        issue.dedupKey = Journal::Key::MountFlap(*row.role, facts.hasWalk, facts.hasLeader,
                                                 real ? look.flipUnder : look.justification);
        issue.why = Acore::StringFormat(
            "{} got onto its mount {} time(s) and off it {} time(s) without leaving {:.1f} yd of one spot; {} of "
            "those changes repeated a justification that had not changed ({})",
            row.botName, look.mounts, look.dismounts, look.heldYd, look.flips,
            real ? look.flipUnder : look.justification);
        issue.realWhen = "a change of the mount state in the same direction as the previous one, under an "
                         "identical justification (the facts of pb_journal_mount), with the bot still inside "
                         "max(combat reach, 1.5 yd) of one anchor on one map - one such repetition is the whole "
                         "proof, because nothing that justified the first change had changed";
        issue.real = real ? "yes" : "no";
        issue.realWhy =
            real ? std::string("the same justification came round twice without the bot leaving its own radius, "
                               "so one of the two changes was taken against unchanged inputs")
                 : std::string("every change of the state had its own justification, or the bot had covered "
                               "ground between them: getting off for a fight on a long walk and back on "
                               "afterwards is right, and the counts stay in the row");
        issue.realSource = "emitter";
        issue.value = double(look.flips);
        issue.valueUnit = "flip";
        issue.botName = row.botName;
        issue.botLow = low;
        issue.mapId = row.mapId;
        issue.zoneId = row.zoneId;
        issue.x = row.x;
        issue.y = row.y;
        issue.z = row.z;
        issue.runId = row.runId;
        issue.sourceTable = "pb_journal_mount";
        issue.sourceId = sourceId;
        // THE WINDOW OF THE SLICE, FROM THE DATA: the last time this bot really made progress through space is
        // the moment its episode began, which is when it last left its own radius. 0 would be a gap; this is a
        // fact the seam already holds.
        uint64 const heldMs = uint64(look.heldSec * IN_MILLISECONDS);
        issue.lastProgressMs = Journal::ClockMs() > heldMs ? Journal::ClockMs() - heldMs : 0;
        // The copy that survives the sweep of pb_journal_mount: read by eye and by the exporter, never filtered.
        issue.fixture = Acore::StringFormat(
            R"({{"mounts":{},"dismounts":{},"flips":{},"heldSec":{:.1f},"heldYd":{:.2f},"justification":"{}",)"
            R"("x":{:.4f},"y":{:.4f},"z":{:.4f},"map":{},"zone":{},"walkLeftYd":{:.0f},"why":"{}"}})",
            look.mounts, look.dismounts, look.flips, look.heldSec, look.heldYd,
            real ? look.flipUnder : look.justification, row.x, row.y, row.z, row.mapId, row.zoneId, walkLeftYd,
            row.why);
        Journal::RecordIssue(issue);
    }
}  // namespace Journal
