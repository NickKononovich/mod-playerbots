/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UPTRIGGERS_H
#define PLAYERBOTS_UPTRIGGERS_H

#include "Creature.h"
#include "DungeonStrategyUtils.h"
#include "GameObject.h"
#include "GenericTriggers.h"
#include "InstanceScript.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

enum UtgardePinnacleIDs
{
    // Skadi the Ruthless
    SPELL_FREEZING_CLOUD_N          = 47579,
    SPELL_FREEZING_CLOUD_H          = 60020,
    SPELL_FREEZING_CLOUD_BREATH     = 47592,
    NPC_BREATH_TRIGGER              = 28351,
    SPELL_SKADI_WHIRLWIND_N         = 50228,
    SPELL_SKADI_WHIRLWIND_H         = 59322,

    // King Ymiron
    SPELL_BANE_N                    = 48294,
    SPELL_BANE_H                    = 59301,
};

#define SPELL_FREEZING_CLOUD        DUNGEON_MODE(bot, SPELL_FREEZING_CLOUD_N, SPELL_FREEZING_CLOUD_H)
#define SPELL_SKADI_WHIRLWIND       DUNGEON_MODE(bot, SPELL_SKADI_WHIRLWIND_N, SPELL_SKADI_WHIRLWIND_H)
#define SPELL_BANE                  DUNGEON_MODE(bot, SPELL_BANE_N, SPELL_BANE_H)

// const float SKADI_BREATH_CENTRELINE = -512.46875f;

namespace UPIds
{
// utgarde_pinnacle.h
constexpr uint32 MAP_ID = 575;
constexpr uint32 DATA_SVALA_SORROWGRAVE = 0;
constexpr uint32 DATA_GORTOK_PALEHOOF = 1;
constexpr uint32 DATA_SKADI_THE_RUTHLESS = 2;
constexpr uint32 DATA_GRAUF = 4;
constexpr uint32 DATA_STATIS_GENERATOR = 21;
// boss_svala.cpp: Svala before the intro turns her into Svala Sorrowgrave
constexpr uint32 NPC_SVALA = 29281;
constexpr uint32 NPC_SVALA_SORROWGRAVE = 26668;
constexpr uint32 NPC_RITUAL_CHANNELER = 27281;
constexpr uint32 SPELL_PARALYZE = 48278;
// areatrigger_scripts (SmartTrigger): 5140 starts the Svala intro, 4991 the Skadi gauntlet
constexpr uint32 AT_SVALA_INTRO = 5140;
constexpr uint32 AT_SKADI_GAUNTLET = 4991;
// boss_skadi.cpp / gameobject_template: Harpooners drop a Harpoon (192539, Create Harpoon 56790 -> item 37372), the
// launchers (lock 1777 = key item 37372) cast 48641 -> Launch Harpoon 48642 from the World Trigger next to them
constexpr uint32 GO_HARPOON = 192539;
constexpr uint32 NPC_YMIRJAR_HARPOONER = 26692;
// boss_skadi.cpp: Grauf's breath pass carries Freezing Cloud Periodic Left 47590 / Right 47592; its 10 yd pulses
// (47574 / 47594) make the Flame Breath Triggers 28351 with y >= -511 (left) / y <= -511 (right) cast Freezing
// Cloud 47579 / 60020 (3 yd, 10 s) on themselves. The triggers stand between y -521 and -492, so the other half of
// the lane beyond 3 yd plus a body from the -511 line is safe.
constexpr uint32 SPELL_FREEZING_CLOUD_LEFT_PERIODIC = 47590;
constexpr uint32 SPELL_FREEZING_CLOUD_RIGHT_PERIODIC = 47592;
constexpr float BREATH_SPLIT_Y = -511.0f;
constexpr float BREATH_SAFE_NORTH_Y = -502.0f;  // during a right (south) pass
constexpr float BREATH_SAFE_SOUTH_Y = -520.0f;  // during a left (north) pass
constexpr float BREATH_SIDE_MARGIN = 5.5f;      // cloud radius 3 + body + slack from the split line
constexpr float HALL_X_MIN = 315.0f;
constexpr float HALL_X_MAX = 500.0f;
constexpr float HALL_Y_MIN = -535.0f;
constexpr float HALL_Y_MAX = -485.0f;
constexpr float HALL_Z_MIN = 95.0f;
constexpr float HALL_Z_MAX = 115.0f;
constexpr uint32 GO_HARPOON_LAUNCHER_A = 192175;
constexpr uint32 GO_HARPOON_LAUNCHER_B = 192176;
constexpr uint32 GO_HARPOON_LAUNCHER_C = 192177;
constexpr uint32 NPC_HARPOON_LAUNCHER_TRIGGER = 19871;
constexpr uint32 ITEM_HARPOON = 37372;

// boss_svala.cpp: areatrigger 5140 -> SetData(1, 1) sets DATA_SVALA_SORROWGRAVE IN_PROGRESS and keeps Svala
// ImmuneToAll through the Arthas talk (EVENT_SVALA_START 5 s, TALK1..TALK9 8 + 9 + 3 + 9 + 2 + 12 + 9 + 13 + 2 s: 72 s,
// UpdateEntry 29281 -> 26668 at TALK4). TALK9 clears the immunity and she AttackStarts a player within 100 yd
// (SelectTargetFromPlayerList(100)). A group waits that out near her hall instead of pulling elsewhere. Shared with
// the test harness runner.
inline bool IsSvalaIntroRunning(Creature const* svala, InstanceScript const* instance)
{
    return svala && instance &&
           (svala->GetEntry() == NPC_SVALA || svala->GetEntry() == NPC_SVALA_SORROWGRAVE) && svala->IsAlive() &&
           !svala->IsInCombat() && svala->IsImmuneToPC() &&
           instance->GetData(DATA_SVALA_SORROWGRAVE) == IN_PROGRESS;
}
}  // namespace UPIds

namespace UPEvent
{
constexpr uint32 EVENT_CHECK_INTERVAL = 1000;
constexpr uint32 HARPOON_CHECK_INTERVAL = 500;
constexpr uint32 RITUAL_CHECK_INTERVAL = 500;
constexpr float EVENT_APPROACH_YD = 70.0f;
constexpr float GATHER_YD = 30.0f;
constexpr float START_MIN_HEALTH_PCT = 90.0f;
constexpr float START_MIN_MANA_PCT = 70.0f;
constexpr float SVALA_SEARCH_YD = 120.0f;
// The sacrificed player is ported to the altar (296.6, -346.1, 90.6) wherever the group fights; wave 9: the group
// stood 87 yd away fighting trash and never saw the channelers with the old 60 yd search.
constexpr float CHANNELER_SEARCH_YD = 110.0f;
constexpr float HARPOON_SEARCH_YD = 30.0f;

InstanceScript* GetInstance(Player* bot);
bool IsReadyToStart(Player* bot);
Creature* GetGrauf(Player* bot);
bool IsGauntletRunning(Player* bot);
GameObject* GetStasisGenerator(Player* bot);
bool NeedsSvalaStart(PlayerbotAI* botAI);
bool NeedsGortokStart(PlayerbotAI* botAI);
bool NeedsSkadiStart(PlayerbotAI* botAI);
Creature* FindRitualChanneler(PlayerbotAI* botAI);
bool IsBaneUp(PlayerbotAI* botAI);
// [b36_ymiron] Ymiron is casting Bane (the 2 s cast before the reflect aura lands). Run 33: the aura reflected
// 3-4 hits of 4-6k into each melee inside 25 ms of landing, three DPS died before any tick saw the aura.
bool IsBaneCasting(PlayerbotAI* botAI);
// [b36_ymiron] cast or aura: damage on Ymiron must stop
bool IsBaneThreat(PlayerbotAI* botAI);
Unit* GetYmiron(PlayerbotAI* botAI);
// 1 = Grauf breathes along the north half (left pass), -1 = along the south half (right pass), 0 = no pass.
int GraufBreathSide(Player* bot);
bool InGauntletHall(Player* bot);
// The y the bot should cross to during the current pass, or 0 when it already stands on the safe half.
float BreathSafeY(Player* bot);
// The nearest living Harpooner fighting the group within 35 yd of the bot (cached attackers only).
Unit* FindStagingHarpooner(PlayerbotAI* botAI);
}  // namespace UPEvent

// Grauf's breath pass covers the bot's half of the gauntlet lane.
class SkadiBreathSideTrigger : public Trigger
{
public:
    SkadiBreathSideTrigger(PlayerbotAI* botAI) : Trigger(botAI, "skadi breath side") {}
    bool IsActive() override;
};

// A Harpooner (the only source of harpoons) fights the group nearby and the bot is not on it yet.
class SkadiHarpoonerTrigger : public Trigger
{
public:
    SkadiHarpoonerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "skadi harpooner") {}
    bool IsActive() override;
};

class SkadiFreezingCloudTrigger : public Trigger
{
public:
    SkadiFreezingCloudTrigger(PlayerbotAI* botAI) : Trigger(botAI, "skadi freezing cloud") {}
    bool IsActive() override;
};

class SkadiWhirlwindTrigger : public Trigger
{
public:
    SkadiWhirlwindTrigger(PlayerbotAI* botAI) : Trigger(botAI, "skadi whirlwind") {}
    bool IsActive() override;
};

class YmironBaneTrigger : public Trigger
{
public:
    YmironBaneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ymiron bane") {}
    bool IsActive() override;
};

class YmironBanePurgeTrigger : public Trigger
{
public:
    YmironBanePurgeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ymiron bane purge") {}
    bool IsActive() override;
};

class SvalaStartReadyTrigger : public Trigger
{
public:
    SvalaStartReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "svala start ready", UPEvent::EVENT_CHECK_INTERVAL) {}
    bool IsActive() override;
};

class SvalaRitualTrigger : public Trigger
{
public:
    SvalaRitualTrigger(PlayerbotAI* botAI) : Trigger(botAI, "svala ritual", UPEvent::RITUAL_CHECK_INTERVAL) {}
    bool IsActive() override;
};

class GortokStartReadyTrigger : public Trigger
{
public:
    GortokStartReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "gortok start ready", UPEvent::EVENT_CHECK_INTERVAL)
    {
    }
    bool IsActive() override;
};

class SkadiStartReadyTrigger : public Trigger
{
public:
    SkadiStartReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "skadi start ready", UPEvent::EVENT_CHECK_INTERVAL) {}
    bool IsActive() override;
};

class SkadiGauntletTrigger : public Trigger
{
public:
    SkadiGauntletTrigger(PlayerbotAI* botAI) : Trigger(botAI, "skadi gauntlet", UPEvent::HARPOON_CHECK_INTERVAL) {}
    bool IsActive() override;
};

class SkadiHarpoonOnGroundTrigger : public Trigger
{
public:
    SkadiHarpoonOnGroundTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "skadi harpoon on ground", UPEvent::HARPOON_CHECK_INTERVAL)
    {
    }
    bool IsActive() override;
};

class SkadiHarpoonInHandTrigger : public Trigger
{
public:
    SkadiHarpoonInHandTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "skadi harpoon in hand", UPEvent::HARPOON_CHECK_INTERVAL)
    {
    }
    bool IsActive() override;
};

#endif
