/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COSTRIGGERS_H
#define PLAYERBOTS_COSTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

class Creature;
class InstanceScript;

enum CullingOfStratholmeIDs
{
    // Salramm the Fleshcrafter
    NPC_GHOUL_MINION                   = 27733,
};

// The Culling of Stratholme (map 595). Ids from scripts/Kalimdor/CavernsOfTime/CullingOfStratholme/
// culling_of_stratholme.h / .cpp and the world DB.
namespace CoSIds
{
constexpr uint32 MAP_CULLING_OF_STRATHOLME = 595;

// Instance data (enum Data / Data64 / Encounters)
constexpr uint32 DATA_ARTHAS_EVENT = 0;
constexpr uint32 DATA_GUARDIANTIME_EVENT = 1;
constexpr uint32 DATA_ARTHAS = 0;
constexpr uint32 BOSS_INFINITE_CORRUPTOR = 4;

// enum ArthasPhase
constexpr uint32 COS_PROGRESS_NOT_STARTED = 0;
constexpr uint32 COS_PROGRESS_CRATES_FOUND = 1;
constexpr uint32 COS_PROGRESS_START_INTRO = 2;
constexpr uint32 COS_PROGRESS_FINISHED_INTRO = 3;
constexpr uint32 COS_PROGRESS_FINISHED_CITY_INTRO = 4;
constexpr uint32 COS_PROGRESS_KILLED_MEATHOOK = 5;
constexpr uint32 COS_PROGRESS_KILLED_SALRAMM = 6;
constexpr uint32 COS_PROGRESS_REACHED_TOWN_HALL = 7;
constexpr uint32 COS_PROGRESS_KILLED_EPOCH = 8;
constexpr uint32 COS_PROGRESS_LAST_CITY = 9;
constexpr uint32 COS_PROGRESS_BEFORE_MALGANIS = 10;
constexpr uint32 COS_PROGRESS_FINISHED = 11;

constexpr uint32 NPC_CHROMIE_START = 26527;    // npc_cos_chromie_start, at the instance entrance
constexpr uint32 NPC_CHROMIE_MIDDLE = 27915;   // npc_cos_chromie_middle, summoned 20 s after the 5th crate
constexpr uint32 NPC_ARTHAS_COS = 26499;
constexpr uint32 NPC_INFINITE_CORRUPTOR = 32273;
constexpr uint32 NPC_MEATHOOK = 26529;
constexpr uint32 NPC_SALRAMM = 26530;
constexpr uint32 NPC_EPOCH = 26532;
constexpr uint32 NPC_MAL_GANIS = 26533;
constexpr uint32 GO_SUSPICIOUS_CRATE = 190094;
constexpr uint32 ITEM_ARCANE_DISRUPTOR = 37888;

// DB gossip of Chromie (gossip_menu_option): 9586 -> 9594 -> 9595 (selecting there hands out the Disruptor),
// 9610 -> 9611 -> 9612 (selecting there starts Arthas' intro). Option id 0 is the story line on each page.
constexpr uint32 GOSSIP_MENU_CHROMIE_START = 9586;
constexpr uint32 GOSSIP_MENU_CHROMIE_DECISION = 9594;
constexpr uint32 GOSSIP_MENU_CHROMIE_INTERFERE = 9595;
constexpr uint32 GOSSIP_MENU_CHROMIE_MIDDLE = 9610;
constexpr uint32 GOSSIP_MENU_CHROMIE_MIDDLE_2 = 9611;
constexpr uint32 GOSSIP_MENU_CHROMIE_MIDDLE_3 = 9612;
constexpr uint32 GOSSIP_OPTION_STORY = 0;

// npc_arthas option actions (GOSSIP_ACTION_INFO_DEF + 1..6); +2 only opens the page holding +3.
constexpr uint32 GOSSIP_ACTION_ARTHAS_FIRST = 1001;
constexpr uint32 GOSSIP_ACTION_ARTHAS_TOWN_HALL_PAGE = 1002;
constexpr uint32 GOSSIP_ACTION_ARTHAS_LAST = 1006;

// Wave creatures summoned by Arthas (WavesLocations, culling_of_stratholme.cpp)
constexpr uint32 NPC_RISEN_ZOMBIE = 27737;
constexpr uint32 NPC_DEVOURING_GHOUL = 28249;
constexpr uint32 NPC_DARK_NECROMANCER = 28200;
constexpr uint32 NPC_TOMB_STALKER = 28199;
constexpr uint32 NPC_CRYPT_FIEND = 27734;
constexpr uint32 NPC_BILE_GOLEM = 28201;
constexpr uint32 NPC_ENRAGING_GHOUL = 27729;
constexpr uint32 NPC_PATCHWORK_CONSTRUCT = 27736;
}  // namespace CoSIds

namespace CoSHelpers
{
InstanceScript* GetInstance(Player* bot);
uint32 GetProgress(Player* bot);
Creature* GetArthas(Player* bot);
// Current wave creature (or Meathook / Salramm) nearest to the group's interactor, or nullptr. [b44_cos]
// preferInCombat (the non-tanks): the nearest wave creature already fighting, when one is, before an idle nearer one.
Creature* GetWaveTarget(Player* bot, bool preferInCombat = false);
// [b44_cos] Chrono-Lord Epoch's ranged stack band, measured from the boss: from just outside "ranged in melee
// danger" (RangedPositionActions MeleeGapTo < 2 yd) to a few yards past it, beside the tank, where Time Step's
// charge to a ranged member is a short one.
float EpochStackMinDistance(Player* bot, Unit const* boss);
float EpochStackMaxDistance(Player* bot, Unit const* boss);
// In the band, level with the boss and in its sight.
bool IsEpochStacked(Player* bot, Unit const* boss);
// Heroic Infinite Corruptor from the last city on (Arthas sent running, or waiting before Mal'Ganis) while the
// Guardian of Time timer still leaves time for the detour, or nullptr.
Creature* GetCorruptorDetourTarget(Player* bot);
// Heroic with the Guardian of Time timer running (Infinite Corruptor slot neither DONE nor FAIL, more than the last
// unusable minute left).
bool IsGuardianTimerRunning(Player* bot);
// Heroic, Guardian of Time running, from the city waves to Mal'Ganis: an idle mob that is no part of Arthas' event
// (not his summon - waves, Town Hall adversaries, Epoch, Mal'Ganis -, no boss, not the Corruptor). Expert groups
// leave the infected citizens (Risen Zombies) and the street packs standing and spend the 25 minutes on the event;
// whatever attacks is fought anyway. Also read by the test harness' target filter.
bool IsSkippableEventTrash(Player* bot, Unit const* unit);
// A live, attackable unit fighting Arthas within reach of the bot (his victim or an attacker), nearest first; his
// escort stops while he fights, so the group kills it like a player would.
Unit* GetArthasAttacker(Player* bot);
// Logs ([cos-arthas-idle], every 30 s) an Arthas that stands without his gossip flag for 30 s: combat, attackers.
void LogArthasIdle(Player* bot);
// Logs the Guardian of Time left at every event step once per instance ([cos-timer]): the segment budget of a run.
void LogTimerMilestone(Player* bot);
bool IsGroupReady(Player* bot, WorldObject const* anchor, float radius);
}  // namespace CoSHelpers

class ExplodeGhoulTrigger : public Trigger
{
public:
    ExplodeGhoulTrigger(PlayerbotAI* botAI) : Trigger(botAI, "explode ghoul") {}
    bool IsActive() override;
};

class EpochRangedTrigger : public Trigger
{
public:
    EpochRangedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "chrono-lord epoch ranged") {}
    bool IsActive() override;
};

class CoSChromieStartTrigger : public Trigger
{
public:
    CoSChromieStartTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cos chromie start") {}
    bool IsActive() override;
};

class CoSSuspiciousCrateTrigger : public Trigger
{
public:
    CoSSuspiciousCrateTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cos suspicious crate") {}
    bool IsActive() override;
};

class CoSChromieMiddleTrigger : public Trigger
{
public:
    CoSChromieMiddleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cos chromie middle") {}
    bool IsActive() override;
};

class CoSArthasGossipTrigger : public Trigger
{
public:
    CoSArthasGossipTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cos arthas gossip") {}
    bool IsActive() override;

private:
    uint32 _movingSinceMs = 0;  // [b33_cos] Arthas offers the gossip while he still walks / returns
};

class CoSEventTargetTrigger : public Trigger
{
public:
    CoSEventTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cos event target") {}
    bool IsActive() override;
};

class CoSFollowArthasTrigger : public Trigger
{
public:
    CoSFollowArthasTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cos follow arthas") {}
    bool IsActive() override;
};

#endif
