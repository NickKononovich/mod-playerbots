/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDLEADERSTATE_H
#define PLAYERBOTS_RAIDLEADERSTATE_H

#include "Common.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class Action;
class Group;
class Player;
class PlayerbotAI;
class Unit;

// The raid leader layer: one bot of a group (the coordinator) runs ready checks, sets the tank flags, calls
// the pull and the wipe, and every member reads the shared state below to answer and to hold its attacks.
namespace RaidLeader
{
    // [ready_reply_now] members answer on receipt and rebuff after (ReadyCheckAction): 2 s for the answers to come
    // in (was 6 s: members rebuffed before they answered), the retry after 8 s (was 15 s).
    constexpr uint32 READY_CHECK_MIN_WAIT_MS = 2 * IN_MILLISECONDS;
    constexpr uint32 READY_CHECK_ANSWER_MS = 25 * IN_MILLISECONDS;
    constexpr uint32 READY_CHECK_RETRY_MS = 8 * IN_MILLISECONDS;
    // Then the pull goes on with who is not ready ("ready_check_pull_anyway"). Wave 21 (*.bak_w21idle): 4 attempts of
    // up to 25 s answers + 15 s retry held the pull up to PULL_HOLD_MAX_MS (150 s) for a mage at 77-84 % mana who
    // never drank (hol-h, Loken): the harness stall watchdog skipped the boss first. 2 attempts: at most ~65 s.
    constexpr uint8 READY_CHECK_MAX_ATTEMPTS = 2;
    constexpr uint32 READY_VALID_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 PULL_DELAY_MS = 3 * IN_MILLISECONDS;  // "pull in 3" after the group is ready
    constexpr uint32 PRE_POT_WINDOW_MS = 6 * IN_MILLISECONDS;
    constexpr uint32 PULL_HOLD_MAX_MS = 150 * IN_MILLISECONDS;
    // [ready_hold_30s] User rule 2026-10-25: held this long with every member alive, here and above its own rest
    // lines (RestState::WantsDrink / WantsFood, the 95 % boss top-up included), the pull goes on whatever the answers
    // said (DOWNTIME_AUDIT: pull holds of 120-153 s with everyone at 100 % health, 3,587 s a wave).
    constexpr uint32 PULL_HOLD_READY_MAX_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 PULL_HOLD_PERSON_MAX_MS = 10 * MINUTE * IN_MILLISECONDS;  // a person leads: until he pulls
    constexpr uint32 WIPE_RECOVERY_MAX_MS = 4 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 TANK_FLAGS_RETRY_MS = 5 * IN_MILLISECONDS;
    constexpr float BOSS_NEAR_DISTANCE = 45.0f;
    constexpr float READY_MAX_LEADER_DISTANCE = 50.0f;
    constexpr float PRE_POT_BOSS_DISTANCE = 60.0f;
    constexpr uint8 HP_MARK_HALF = 50;
    constexpr uint8 LAST_PHASE_HP_PCT = 20;  // below this the fight is treated as its last phase

    constexpr uint32 SPELL_SATED = 57724;
    constexpr uint32 SPELL_EXHAUSTION = 57723;
    constexpr uint32 SPELL_REBIRTH_RANK_1 = 20484;
    constexpr uint32 SPELL_RAISE_ALLY = 61999;
    constexpr uint32 SPELL_DIVINE_INTERVENTION_AURA = 19753;  // on the saved member: unattackable, stunned, 3 min

    // Smart wipe: a lost fight orders the members who can drop out of it (Vanish, Feign Death, Invisibility,
    // Shadowmeld, a paladin's Divine Intervention on a resurrecter) to do so; the rest fight on.
    constexpr uint32 WIPE_ESCAPE_ORDER_MS = 60 * IN_MILLISECONDS;  // the order is answered within this window

    enum class ReadyCheckPhase : uint8
    {
        Idle,
        Pending,
        RetryWait
    };

    struct ReadyAnswer
    {
        bool ready = true;
        std::string reason;
    };

    struct GroupState
    {
        ReadyCheckPhase phase = ReadyCheckPhase::Idle;
        uint32 checkStartMs = 0;
        uint32 retryAtMs = 0;
        uint8 attempts = 0;
        std::unordered_map<ObjectGuid, ReadyAnswer> answers;

        ObjectGuid holdBoss;  // boss the group must not pull before the ready check passes
        uint32 holdSinceMs = 0;
        ObjectGuid readyBoss;
        uint32 readyMs = 0;

        ObjectGuid engagedBoss;
        uint32 engageMs = 0;
        bool pullAnnounced = false;
        uint8 hpMarks = 0;
        bool lustAnnounced = false;
        std::unordered_set<ObjectGuid> battleResAnnounced;

        bool wipeCalled = false;
        bool recovering = false;
        uint32 wipeMs = 0;
        uint32 escapeOrderMs = 0;  // a lost fight: survivors drop out (OrderWipeEscape), 0 = no order

        uint32 tankFlagsQueuedMs = 0;
        uint32 lastTouchMs = 0;
    };

    // Shared by the members of a group; they may tick on different map threads.
    class Registry
    {
    public:
        static Registry& Instance();

        template <typename F>
        auto With(ObjectGuid groupGuid, F&& fn)
        {
            std::lock_guard<std::mutex> lock(_lock);
            return fn(Touch(groupGuid));
        }

    private:
        GroupState& Touch(ObjectGuid groupGuid);

        std::mutex _lock;
        std::unordered_map<ObjectGuid, GroupState> _groups;
    };

    bool IsEnabled();

    // The group's coordinator on the member's instance map: the leader with an AI (bot or SelfBot); when a person
    // leads, a tank bot (RaidLeaderForHumanLeader); nullptr outside instances or with no bot on the map.
    Player* FindCoordinator(Player* member);
    bool IsBossCreature(Unit* unit);
    bool CanResurrect(Player* player);
    bool HasBattleResurrection(Player* player);
    bool IsEatingOrDrinking(Player* player, bool& drinking);

    // Damage or attack actions; heals, buffs and movement are not offensive.
    bool IsOffensiveAction(Action* action, Player* bot);

    // A living tank bot in combat, or out of combat while a group member fights an attacker of the group. Holds
    // only stop new pulls: they never take tank assist, threat, taunts or defensives from such a tank (tank assist
    // is also the only way its non-combat engine joins a fight).
    bool IsTankDefendingGroup(PlayerbotAI* botAI);

    void Announce(PlayerbotAI* botAI, std::string const& textKey, std::string const& defaultText,
                  std::map<std::string, std::string> placeholders = {});

    void RecordReadyAnswer(Player* member, bool ready, std::string const& reason);
    bool IsAutomaticReadyCheckPending(Player* member);
    void NoteEngage(Player* member, Unit* boss);
    void ClearEngage(Player* member);
    bool CallWipe(Player* caller, Unit* boss, std::string const& reason);
    bool OrderWipeEscape(Player* caller);       // true when the order is new
    bool IsWipeEscapeOrdered(Player* member);  // an order of the last WIPE_ESCAPE_ORDER_MS
    void QueueTankFlags(Player* coordinator, ObjectGuid mainTank, ObjectGuid mainAssist);

    // Group pause, as a raid leader calls a break: the fight in progress is finished, then nobody pulls or leads
    // on until "resume" - or until the person who paused is back at the client (any movement, cast or target
    // change after PAUSE_INPUT_GRACE_MS, see SelfBotControl::GetLastInputMs). The client AFK flag is not used:
    // a person watching the SelfBot play is AFK too.
    constexpr uint32 PAUSE_INPUT_GRACE_MS = 5 * IN_MILLISECONDS;

    bool SetPaused(Player* member, bool paused, Player* by);  // true when the state changed
    bool IsPaused(Player* member);
    uint32 GetPausedMs(Player* member, ObjectGuid* by = nullptr);  // how long the group is paused, 0 = not

    // The group leader is a person at the game client (no PlayerbotAI).
    bool IsLedByPerson(Player* member);

    // "res <name>": the member the resurrecters take first, cleared once it is alive.
    void SetResurrectFirst(Player* member, ObjectGuid target);
    ObjectGuid GetResurrectFirst(Player* member);
}

#endif
