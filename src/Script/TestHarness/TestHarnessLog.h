/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTHARNESSLOG_H
#define PLAYERBOTS_TESTHARNESSLOG_H

#include "Define.h"
#include "LeaderLog.h"  // [lead107]
#include "ObjectGuid.h"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class Spell;
class SpellInfo;
class Unit;

namespace TestHarness
{
    // [lead107] the JSON builder and the decision run live in the leader (Ai/Leader/LeaderLog.h)
    using Leader::DecisionRun;
    using Leader::JsonArray;
    using Leader::JsonEscape;
    using Leader::JsonObject;
    std::string WallClock();

    // <LogsDir>/tests/<fileName>, one line per call.
    void AppendLine(std::string const& fileName, std::string const& line);
    std::string TestsDir();

    // Per spell of one member: successful in-combat casts, and the damage / healing the spell's hits and ticks
    // carried at the script hooks (before absorbs and resists, so an upper bound; heals include overheal).
    struct SpellCounters
    {
        uint32 casts = 0;
        uint64 damage = 0;
        uint64 healing = 0;
        uint32 hits = 0;  // [rot531] damage events (direct hits and periodic ticks, pets' included)
    };

    struct MemberCounters
    {
        uint64 damageDone = 0;
        uint64 healingDone = 0;
        uint64 damageTaken = 0;
        uint32 deaths = 0;
        uint32 interrupts = 0;
        uint32 dispels = 0;
        std::map<uint32, uint32> itemUses;  // item entry -> uses (potions, flasks, food, healthstones)
        std::map<uint32, SpellCounters> spells;  // spell id -> casts / amounts
        uint64 busyMs = 0;  // in-combat casts x max(cast time, GCD, channel duration): GCD-utilisation estimate
        // [harness20] power the member took (UnitScript::OnPowerGain: what the bar took, capped at its max), keyed by
        // PowerGainKey(power, source, spell id): rage from damage dealt / taken, every energize spell and tick
        std::map<uint32, uint64> powerGains;
    };

    // [harness20] power (4 bits) | PowerGainSource (4 bits) | spell id (24 bits)
    inline uint32 PowerGainKey(uint8 power, uint8 source, uint32 spellId)
    {
        return (uint32(power & 0xF) << 28) | (uint32(source & 0xF) << 24) | (spellId & 0xFFFFFF);
    }

    struct DeathNote
    {
        ObjectGuid guid;
        std::string name;
        std::string killer;
        uint32 killerEntry = 0;
        uint32 ms = 0;
    };

    // Who started a fight (pull_start / pull_end, "who aggroed first"): the order the members entered combat, the
    // first mob that engaged and its first victim, and the first hit between a member and a mob. Filled by the
    // combat / damage hooks from the moment the harness resets it (the end of the previous fight).
    struct OpenerMember
    {
        ObjectGuid guid;
        std::string name;
        std::string enemy;  // the unit the hook named as its combat partner
        uint32 enemyEntry = 0;
        uint32 ms = 0;
    };

    struct OpenerNote
    {
        std::vector<OpenerMember> members;  // combat entries, first first
        std::string mob;
        uint32 mobEntry = 0;
        uint32 mobSpawnId = 0;
        std::string mobVictim;
        ObjectGuid mobVictimGuid;
        uint32 mobMs = 0;
        std::string hitBy;
        ObjectGuid hitByGuid;
        std::string hitVictim;
        bool hitByMember = false;
        uint32 hitMs = 0;
        std::vector<std::pair<ObjectGuid, uint32>> memberFirstHits;  // each member's first hit on a mob (engageDelay)
        // [opener_zero_hit] each member's first hit that dealt no damage (an evading mob, a full absorb): engageDelay's
        // firstZeroHitSec tells "attacked, nothing landed" from "did not attack" (st-n-92 P19: firstHitSec 10-12 s for
        // three damage dealers that cast and swung from +2 s on)
        std::vector<std::pair<ObjectGuid, uint32>> memberFirstZeroHits;
    };

    // [lead1] Leader::DecisionLog (C:/WoW/LEADER_REFACTOR_PLAN.md 2.7, step 1): one JSONL stream per run,
    // <runId>.decisions.jsonl next to the run's events. Every line names `who` decided or reported: "leader" (the
    // harness) or "tank:<name>" (the executor). A line where an executor decides (`decides` true on a tank line) is
    // what the refactor removes; today the tank's planner still decides, so those lines are the baseline.
    // Conflicts: two different owners deciding about one mob within Leader::DECISION_CONFLICT_MS ("two_owners"), and
    // the kinds the harness notes itself (NoteConflict: "waypoint_in_order", ...). Each is a line what="conflict" and
    // a count in the run's summary (SummaryJson, the run's "leader_decisions" event). World thread only.
    class DecisionLog : public Leader::DecisionSink
    {
    public:
        static DecisionLog& Instance();

        // One decision / report line; `ms` is its getMSTime() stamp (0: now). Returns the line's id.
        uint32 Write(DecisionRun const& run, std::string const& who, std::string const& what, ObjectGuid mob,
                     std::string const& mobName, bool decides, JsonObject fields, uint32 ms = 0) override;
        // A conflict the caller detected (`about`: the decision line ids or a short description).
        void NoteConflict(DecisionRun const& run, std::string const& kind, ObjectGuid mob, std::string const& about,
                          JsonObject fields) override;
        // [lead15] counts a conflict of `kind` without a line of its own (own_decision: the line is the decision)
        void CountConflict(DecisionRun const& run, std::string const& kind) override;
        // The run's counts: lines by `what`, conflicts by kind, decisions by owner.
        std::string SummaryJson(uint32 runSeq);
        void EndRun(uint32 runSeq);
        // [speech] the lines the leader said since the last call (oldest first); the caller sends them to the GM
        std::vector<std::string> DrainSpeech(uint32 runSeq);

    private:
        struct MobLast
        {
            std::string owner;
            std::string what;
            uint32 id = 0;
            uint32 ms = 0;
        };
        struct RunState
        {
            std::map<std::string, uint32> lines;      // what -> count
            std::map<std::string, uint32> conflicts;  // kind -> count
            std::map<std::string, uint32> owners;     // owner -> decisions
            std::map<std::string, uint32> nonLeader;  // [lead17] "<who kind>|<what>" -> decisions not by the leader
            std::unordered_map<ObjectGuid, MobLast> lastByMob;
            std::deque<std::string> speech;                    // [speech] lines waiting for the GM
            std::unordered_map<std::string, uint32> speechAt;  // [speech] topic -> last said (getMSTime)
        };
        void Say(RunState& state, uint32 elapsedMs, uint32 stamp, std::string const& who,
                 std::string const& what, std::string const& mobName, bool decides, std::string const& json);

        std::mutex _lock;
        uint32 _nextId = 0;
        std::unordered_map<uint32, RunState> _runs;
    };

    // Counters fed by unit / player hooks on the map threads; read by the harness on the world thread.
    class StatsRegistry
    {
    public:
        static StatsRegistry& Instance();

        void TrackMember(ObjectGuid guid, uint32 runSeq);
        void TrackInstance(uint32 instanceId, uint32 runSeq);
        void UntrackRun(uint32 runSeq);

        MemberCounters Snapshot(ObjectGuid guid);
        uint32 TakeKills(uint32 runSeq);
        std::vector<DeathNote> TakeDeaths(uint32 runSeq);
        // Entries of the creatures killed in the run's instance since the last call (boss kill credit).
        std::vector<uint32> TakeKilledEntries(uint32 runSeq);

        void OnDamage(Unit* attacker, Unit* victim, uint32 damage);
        void OnHeal(Unit* healer, Unit* target, uint32 gain);
        void OnPowerGain(Unit* unit, uint8 power, int32 gain, uint8 source, uint32 spellId);  // [harness20]
        // [b31_harness] Called from the end of Spell::_cast (AllSpellScript OnSpellCast, after SMSG_SPELL_GO), so
        // only casts that went off count. castItemEntry: the item the cast started from (Spell::m_CastItem is
        // already cleared there for a consumed item, the script stashes it at the start of _cast).
        void OnSpellCast(Player* player, Spell* spell, uint32 castItemEntry);
        void OnSpellAmount(Unit* source, Unit* other, SpellInfo const* spell, uint32 amount, bool heal);
        void OnUnitDeath(Unit* victim, Unit* killer);
        // [b30_topped_up] [b30_reenter] a member re-entered the instance (stranded with no path back, or a route
        // choice): counted as its death, `killer` names which ("stranded_reenter: ..." / "shortcut_reenter: ...")
        void RecordReentry(Player* player, std::string const& killer);
        void OnPlayerEnterCombat(Player* player, Unit* enemy);
        void OnCreatureEngage(Unit* creature, Unit* victim);

        OpenerNote Opener(uint32 runSeq);
        void ResetOpener(uint32 runSeq);

        bool Active() const { return _active.load(std::memory_order_relaxed); }

    private:
        uint32 RunOfMember(ObjectGuid guid) const;  // caller holds _lock

        std::atomic<bool> _active{false};
        std::mutex _lock;
        std::unordered_map<ObjectGuid, uint32> _memberRun;
        std::unordered_map<ObjectGuid, MemberCounters> _counters;
        std::unordered_map<uint32, uint32> _instanceRun;
        std::unordered_map<uint32, uint32> _kills;
        std::unordered_map<uint32, std::vector<DeathNote>> _deaths;
        std::unordered_map<uint32, std::vector<uint32>> _killedEntries;
        std::unordered_map<uint32, OpenerNote> _openers;
    };
}

#endif
