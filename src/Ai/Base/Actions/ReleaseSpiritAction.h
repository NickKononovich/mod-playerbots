/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RELEASESPIRITACTION_H
#define PLAYERBOTS_RELEASESPIRITACTION_H

#include "Action.h"
#include "Position.h"
#include "ReviveFromCorpseAction.h"

class PlayerbotAI;

class ReleaseSpiritAction : public Action
{
public:
    ReleaseSpiritAction(PlayerbotAI* botAI, std::string const& name = "release")
        : Action(botAI, name) {}

    bool Execute(Event event) override;
    void LogRelease(std::string const& releaseType) const;

protected:
    void IncrementDeathCount() const;
};

class AutoReleaseSpiritAction : public ReleaseSpiritAction
{
public:
    AutoReleaseSpiritAction(PlayerbotAI* botAI, std::string const& name = "auto release")
        : ReleaseSpiritAction(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool HandleBattlegroundSpiritHealer();
    bool ShouldAutoRelease() const;
    bool ShouldDelayBattlegroundRelease() const;
    bool WaitForGroupResurrect();
    bool FightIsOver();
    bool ReleaseDelayElapsed();

    static constexpr uint32 GROUP_RESURRECT_WAIT_MS = 60 * IN_MILLISECONDS;
    static constexpr uint32 INSTANCE_RESURRECT_WAIT_MS = 3 * MINUTE * IN_MILLISECONDS;
    // A Soulstone / Reincarnation not used this long after the fight (its trigger holds it while mobs stand on the
    // corpse) is given up: the bot releases and walks back like the others.
    static constexpr uint32 SELF_RESURRECT_WAIT_MS = 60 * IN_MILLISECONDS;
    static constexpr uint32 RELEASE_DELAY_MIN_MS = 1500;
    static constexpr uint32 RELEASE_DELAY_MAX_MS = 5000;

    time_t m_bgGossipTime = 0;
    uint32 _diedAt = 0;
    uint32 _releaseAt = 0;
    uint32 _deadSince = 0;  // the death itself, never moved by a fight (_diedAt restarts at each fight's end)
    // Dead this long in an instance with nobody near to resurrect it out of combat: release and run back, as a
    // player does (Upper Blackrock Spire wave 27: a member stayed in combat, the tank lay dead for 10 minutes).
    static constexpr uint32 INSTANCE_DEAD_MAX_MS = 2 * MINUTE * IN_MILLISECONDS;
    static constexpr float NEAR_RESURRECTER = 40.0f;
    // The resurrecters have found no spot out of the pack's reach to resurrect us from (CorpseRes) this long: release
    // at once instead of waiting INSTANCE_RESURRECT_WAIT_MS for a resurrection that cannot come.
    static constexpr uint32 CORPSE_RES_BLOCKED_MS = 6 * IN_MILLISECONDS;
    // [b47_plan] blocked this long: released even while a member is still "in combat"
    static constexpr uint32 CORPSE_RES_BLOCKED_LONG_MS = 30 * IN_MILLISECONDS;
    bool _noResSpot = false;
    // [b32_bfd] The death the clocks above belong to: the death count PlayerbotAI raises on each death, and where the
    // corpse lies (isUseful restarts them for a new death; the dead engine never asks this action while alive).
    static constexpr float NEW_DEATH_MOVED = 5.0f;
    uint32 _deathCountSeen = 0;
    uint32 _deathMapId = 0;
    Position _deathPos;
};

class RepopAction : public SpiritHealerAction
{
public:
    RepopAction(PlayerbotAI* botAI, std::string const& name = "repop")
        : SpiritHealerAction(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// SelfResurrectAction action registration
class SelfResurrectAction : public Action
{
public:
    SelfResurrectAction(PlayerbotAI* botAI) : Action(botAI, "self resurrect") {}
    virtual bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
