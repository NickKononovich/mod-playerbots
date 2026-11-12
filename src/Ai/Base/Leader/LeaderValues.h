/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADERVALUES_H
#define PLAYERBOTS_LEADERVALUES_H

#include "Value.h"

#include <string>
#include <unordered_map>

class Player;
class PlayerbotAI;

enum class LeaderEventType : uint8
{
    None,
    Engage,
    BossDown,
    Reset,
    Wipe,
    HpMark,
    Bloodlust,
    BattleRes,
    LostFight  // the fight outlook says the enemies outlast the group: survivors drop out, the rest fight on
};

struct LeaderEvent
{
    LeaderEventType type = LeaderEventType::None;
    ObjectGuid boss;
    ObjectGuid target;
    uint8 mark = 0;
    std::string reason;
};

// Coordinator: the fight in progress as an experienced leader reads it - how long the enemies take to die at the
// group's damage, against how long the group lasts at the enemies' damage and the healers' mana.
struct FightOutlook
{
    uint32 enemies = 0;
    float enemyHp = 0.0f;     // current health of every enemy fighting the group (no shields or heals counted)
    float groupDps = 0.0f;    // the living members' damage by the formula (DpsEstimate on their victims)
    float killSec = 0.0f;     // enemyHp / groupDps
    float surviveSec = 0.0f;  // until the group's health is gone: healed while the healers have mana, then not
    bool lost = false;        // killSec beyond what the group survives, on two evaluations in a row
};

// "(enemies N, eHP X, groupDps Y, ttk Z s vs survive W s)" for the [wipe] log lines.
std::string FormatOutlook(FightOutlook const& outlook);

// A living resurrecter of the paladin's group to save with Divine Intervention, a healer first; nullptr = none.
Player* FindDivineInterventionTarget(PlayerbotAI* botAI, Player* bot);

class LeaderFightOutlookValue : public CalculatedValue<FightOutlook>
{
public:
    LeaderFightOutlookValue(PlayerbotAI* botAI)
        : CalculatedValue<FightOutlook>(botAI, "leader fight outlook", 1 * IN_MILLISECONDS)
    {
    }

    FightOutlook Calculate() override;

private:
    void Clear();

    uint32 _lastMs = 0;
    uint8 _lostStreak = 0;
};

struct TankAssignment
{
    ObjectGuid mainTank;
    ObjectGuid mainAssist;
    bool changed = false;
};

// Guid of the group's coordinator on this instance map (see RaidLeader::FindCoordinator).
class RaidCoordinatorValue : public ObjectGuidCalculatedValue
{
public:
    RaidCoordinatorValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "raid coordinator", 2) {}

    ObjectGuid Calculate() override;
};

// Coordinator only: a living boss out of combat close to the coordinator or the main tank.
class LeaderNearbyBossValue : public ObjectGuidCalculatedValue
{
public:
    LeaderNearbyBossValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "leader nearby boss", 2) {}

    ObjectGuid Calculate() override;
};

// Coordinator only: the next thing to announce or decide during a boss fight.
class LeaderEncounterEventValue : public CalculatedValue<LeaderEvent>
{
public:
    LeaderEncounterEventValue(PlayerbotAI* botAI)
        : CalculatedValue<LeaderEvent>(botAI, "leader encounter event", 1 * IN_MILLISECONDS)
    {
    }

    LeaderEvent Calculate() override;

private:
    Unit* FindEngagedBoss();
    // A reason the fight may be lost, weighed against the fight outlook: true = LostFight event.
    bool JudgeFight(std::string const& reason, LeaderEvent& event);

    uint32 _lastJudgeLogMs = 0;
};

// Coordinator of a raid group only: who should carry the main tank and main assist flags.
class LeaderTankAssignmentValue : public CalculatedValue<TankAssignment>
{
public:
    LeaderTankAssignmentValue(PlayerbotAI* botAI)
        : CalculatedValue<TankAssignment>(botAI, "leader tank assignment", 3)
    {
    }

    TankAssignment Calculate() override;
};

// Out of combat: the group waits for a ready check before it may pull.
class PullHoldValue : public BoolCalculatedValue
{
public:
    PullHoldValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "pull hold", 250) {}

    bool Calculate() override;
};

// "pause": the group is on a break (RaidLeader::IsPaused); out of combat nobody pulls or leads on.
class GroupPausedValue : public BoolCalculatedValue
{
public:
    GroupPausedValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "group paused", 500) {}

    bool Calculate() override;
};

// "burst [sec]": end of a timed burst (getMSTime), 0 = none. Set by the burst action, read by its expiry trigger.
class BurstUntilValue : public ManualSetValue<uint32>
{
public:
    BurstUntilValue(PlayerbotAI* botAI) : ManualSetValue<uint32>(botAI, 0, "burst until") {}

    bool addedBoost = false;  // the burst switched "boost" on and switches it off again
};

// In combat: offensive actions of DPS and healers wait for the pull countdown.
class PullCountdownValue : public BoolCalculatedValue
{
public:
    PullCountdownValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "pull countdown", 250) {}

    bool Calculate() override;

private:
    ObjectGuid _openedOn;  // the boss this bot last opened on early ([threat-open], logged once)
};

// Member: the survival spell to use now that the leader called a lost fight ("" = none or no order). Divine
// Intervention goes on a living resurrecter (FindDivineInterventionTarget).
class WipeEscapeSpellValue : public CalculatedValue<std::string>
{
public:
    WipeEscapeSpellValue(PlayerbotAI* botAI) : CalculatedValue<std::string>(botAI, "wipe escape spell", 1000) {}

    std::string Calculate() override;
};

// getMSTime of the member's own escape (Vanish, Feign Death ...), 0 = none. Set by the "wipe escape" action.
class WipeEscapeAtValue : public ManualSetValue<uint32>
{
public:
    WipeEscapeAtValue(PlayerbotAI* botAI) : ManualSetValue<uint32>(botAI, 0, "wipe escape at") {}
};

// Member out of a lost fight (own escape or Divine Intervention): it stays still and does nothing until the
// group's fight is over and the mobs went home; then it resurrects the others (corpse resurrection flow).
class WipeEscapeHoldValue : public BoolCalculatedValue
{
public:
    WipeEscapeHoldValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "wipe escape hold", 500) {}

    bool Calculate() override;

private:
    uint32 _quietSinceMs = 0;
    float _holdHealthPct = 0.0f;  // the bot's health when the hold began (0: no hold)
};

class ReadyCheckInProgressValue : public BoolCalculatedValue
{
public:
    ReadyCheckInProgressValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "ready check in progress", 1000) {}

    bool Calculate() override;
};

// Time stamp of the passed ready check while its pre-pull window is open, else 0.
class PrePullWindowValue : public Uint32CalculatedValue
{
public:
    PrePullWindowValue(PlayerbotAI* botAI) : Uint32CalculatedValue(botAI, "pre pull window", 500) {}

    uint32 Calculate() override;
};

class RaidFlaskValue : public ObjectGuidCalculatedValue
{
public:
    RaidFlaskValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "raid flask", 10) {}

    ObjectGuid Calculate() override;
};

class RaidFoodBuffValue : public ObjectGuidCalculatedValue
{
public:
    RaidFoodBuffValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "raid food buff", 10) {}

    ObjectGuid Calculate() override;
};

class PrePullPotionValue : public ObjectGuidCalculatedValue
{
public:
    PrePullPotionValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "pre pull potion", 5) {}

    ObjectGuid Calculate() override;
};

class NearbySoulwellValue : public ObjectGuidCalculatedValue
{
public:
    NearbySoulwellValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "nearby soulwell", 3) {}

    ObjectGuid Calculate() override;
};

class NearbySoulPortalValue : public ObjectGuidCalculatedValue
{
public:
    NearbySoulPortalValue(PlayerbotAI* botAI) : ObjectGuidCalculatedValue(botAI, "nearby soul portal", 2) {}

    ObjectGuid Calculate() override;
};

#endif
