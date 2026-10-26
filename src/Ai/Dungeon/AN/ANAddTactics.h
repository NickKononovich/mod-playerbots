/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ANADDTACTICS_H
#define PLAYERBOTS_ANADDTACTICS_H

#include "ANTriggers.h"
#include "AttackAction.h"
#include "InstanceScript.h"
#include "Multiplier.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "TbcHazard.h"
#include "ThreatManager.h"
#include "MotionMaster.h"
#include "Trigger.h"
#include "WlkTacticLog.h"
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Header-only on purpose (no new translation unit, so no CMake reconfigure): the trash-pack tactics of Azjol-Nerub.
namespace ANAdds
{
// smart_scripts 28734: every 5-10 s the Anub'ar Skirmisher has a random enemy cast Fixate 52537 (taunt aura on the
// Skirmisher, 10 s), charges it and Backstabs it (52540, 300 % weapon, "on behind target" every 7 s). Wave 8: 5-5.6k
// Backstabs on the hunter and the healer, who turned their backs on it while stepping away from a melee mob.
inline constexpr uint32 SPELL_SKIRMISHER_FIXATE = 52537;
// Backstab needs the bot's back; a Skirmisher kept in the front 120 degrees leaves room for its own steps around.
inline constexpr float FIXATE_FACING_ARC = static_cast<float>(M_PI) * 2.0f / 3.0f;

// boss_hadronox.cpp: only the three Anub'ar Crushers report ACTION_CRUSHER_DIED. With all of them dead Hadronox
// climbs to the platform (EVENT_HADRONOX_MOVE3) and webs the doors (SPELL_WEB_FRONT_DOORS): that alone stops the
// endless door stream that killed the tank in wave 7 (16 adds, 99.7 % boss health).
inline constexpr uint32 NPC_ANUBAR_CRUSHER = 28922;
inline constexpr float CRUSHER_SEARCH_RANGE = 40.0f;

// The Skirmisher in combat with the group that is fixated on this bot.
inline Unit* FindFixatingSkirmisher(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != NPC_WATCHER_SKIRMISHER)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && unit->HasAura(SPELL_SKIRMISHER_FIXATE, bot->GetGUID()))
            return unit;
    }
    return nullptr;
}

// Hands off Hadronox until she has webbed the doors (boss_hadronox.cpp): EVENT_HADRONOX_MOVE3 (70 s, re-checked every
// 2 s while _crushersLeft > 0) sends her, passive, to hadronoxSteps[2] (530.4, 560.0, 733.2); MovementInform there
// casts SPELL_WEB_FRONT_DOORS 53177 and sets _doorsWebbed. EVENT_CHECK_EVADE (every 1 s) clears her motion the moment
// she first gets a player on her threat list (IsInCombatWithPlayer flips to true, UpdateAI 345-358): a hit or an area
// spell on her while she climbs stops her short of the spot, the doors stay open and the swarm never ends
// (an-h 20261025-063826: "boss attempt loop: Hadronox engaged 5 min, 218 trash kills", fought at ~545,530).
inline constexpr uint32 SPELL_WEB_FRONT_DOORS = 53177;
// She counts as arrived this close to the spot once she stands still.
inline constexpr float WEB_SPOT_ARRIVED = 4.0f;
// No area spell while she is this close to the bot or its target before the doors are webbed.
inline constexpr float HADRONOX_AOE_SAFE = 15.0f;
inline constexpr float HADRONOX_SCAN_RANGE = 120.0f;
inline constexpr uint32 HADRONOX_CACHE_MS = 500;
// an-h 20261025-170425: engaged at ~480 s while she still had her walk to do, the group fought her 318 s with the door
// stream open (224 adds, 2.7M damage: adds dying under her Leech Poison heal her), the healer ran dry, wipe at 740 s.
// Her climb is only lost when she stops short of the spot after the Crushers died: this long standing still proves it.
inline constexpr uint32 HADRONOX_STALL_MS = 10 * IN_MILLISECONDS;
inline constexpr float CRUSHER_ALIVE_SCAN = 150.0f;

struct HadronoxGate
{
    uint32 ms = 0;
    ObjectGuid boss;      // Hadronox, alive, while the encounter runs
    bool webbed = false;  // she reached the web spot (latched per instance until the encounter ends)
    bool engaged = false; // a player (or a player's pet) is on her threat list
    bool stalled = false; // engaged, every Crusher dead, standing short of her spot for HADRONOX_STALL_MS
    uint32 stillSinceMs = 0;  // since when she stands engaged off her spot with the Crushers dead (0: not)
    bool crushersAlive = false;  // [b33_hadronox2] a Crusher lives: MOVE3 (her climb) has not started
};

// [b33_hadronox2] Encounter resets seen per instance (IN_PROGRESS -> neither IN_PROGRESS nor DONE: her evade, a
// Crusher pack member's evade - SummonedCreatureEvade - or a wipe). an-h 20261026-000434-an-h-5E-35: 3 resets, the
// runner logged boss_failed, then the group stood 120 s at the hole: the drop waited for DONE.
inline constexpr uint32 HADRONOX_FAILED_ATTEMPTS_DROP = 3;

inline uint32 HadronoxFailedAttempts(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || bot->GetMapId() != ANWatcherRoom::MAP_AZJOL_NERUB)
        return 0;

    uint32 const state = instance->GetBossState(ANIds::DATA_HADRONOX);
    static std::mutex lock;
    static std::unordered_map<uint32, std::pair<bool, uint32>> seen;  // instance -> (was in progress, resets)
    std::lock_guard<std::mutex> guard(lock);
    std::pair<bool, uint32>& entry = seen[bot->GetInstanceId()];
    bool const inProgress = state == IN_PROGRESS;
    if (entry.first && !inProgress && state != DONE)
    {
        ++entry.second;
        LOG_INFO("playerbots", "[an hadronox reset] {}: the Hadronox encounter reset ({} so far in this instance)",
                 bot->GetName(), entry.second);
    }
    entry.first = inProgress;
    return entry.second;
}

// boss_hadronox.cpp IsInCombatWithPlayer: her climb is cancelled only when this FLIPS to true (EVENT_CHECK_EVADE
// clears her point motion). Once it is true the harm is done (or, before MOVE3, cannot happen any more: MOVE3 then
// starts with the flag already set and she walks on), so from then on the only way to end the door stream is to kill
// her (her death despawns the summoning world triggers). an-h 20261025-150737: she aggroed the healer by herself at
// 552 s, 28-46 yd short of her spot; the hands-off gate kept refusing every taunt and hit on her ("righteous
// defense on Hadronox refused") for 225 s while the group farmed 227 door adds, and the 5 min attempt loop ended
// the run.
inline bool HadronoxInPlayerCombat(Creature* hadronox)
{
    for (ThreatReference const* ref : hadronox->GetThreatMgr().GetUnsortedThreatList())
        if (ref->GetVictim() && ref->GetVictim()->IsControlledByPlayer())
            return true;
    return false;
}

// Latched per instance: once she has webbed the doors she may chase the tank off the spot. Shared by every bot of the
// instance, which map worker threads may update on different ticks: guarded by a mutex.
inline bool WebbedLatch(uint32 instanceId, bool set, bool clear)
{
    static std::mutex lock;
    static std::unordered_set<uint32> webbed;
    std::lock_guard<std::mutex> guard(lock);
    if (clear)
    {
        webbed.erase(instanceId);
        return false;
    }
    if (set)
        webbed.insert(instanceId);
    return webbed.count(instanceId) != 0;
}

inline HadronoxGate const& GetHadronoxGate(PlayerbotAI* botAI)
{
    thread_local std::unordered_map<uint64, HadronoxGate> cache;
    Player* bot = botAI->GetBot();
    HadronoxGate& gate = cache[bot->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    if (gate.ms && getMSTimeDiff(gate.ms, now) < HADRONOX_CACHE_MS)
        return gate;

    gate.ms = now;
    gate.boss.Clear();
    gate.webbed = false;
    gate.engaged = false;
    gate.stalled = false;
    gate.crushersAlive = false;  // [b33_hadronox2]
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || bot->GetMapId() != ANWatcherRoom::MAP_AZJOL_NERUB)
        return gate;

    HadronoxFailedAttempts(bot);  // [b33_hadronox2] sees the reset transitions while the fight runs

    if (instance->GetBossState(ANIds::DATA_HADRONOX) != IN_PROGRESS)
    {
        WebbedLatch(bot->GetInstanceId(), false, true);
        gate.stillSinceMs = 0;
        return gate;
    }

    Creature* hadronox = bot->FindNearestCreature(ANIds::NPC_HADRONOX, HADRONOX_SCAN_RANGE, true);
    if (!hadronox)
    {
        gate.stillSinceMs = 0;
        return gate;
    }

    gate.boss = hadronox->GetGUID();
    gate.engaged = HadronoxInPlayerCombat(hadronox);
    // MOVE3 waits for the Crushers (re-checked every 2 s) and then walks her up even with players on her list: only
    // the FLIP to player combat clears her motion. So while a Crusher lives or she walks, the climb is still to come.
    bool const climbing = hadronox->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
    bool const crushersAlive = hadronox->FindNearestCreature(NPC_ANUBAR_CRUSHER, CRUSHER_ALIVE_SCAN, true) != nullptr;
    gate.crushersAlive = crushersAlive;  // [b33_hadronox2]
    if (!gate.engaged || climbing || crushersAlive)
        gate.stillSinceMs = 0;
    else if (!gate.stillSinceMs)
        gate.stillSinceMs = now ? now : 1;
    gate.stalled = gate.stillSinceMs && getMSTimeDiff(gate.stillSinceMs, now) >= HADRONOX_STALL_MS;
    bool const arrived =
        hadronox->GetExactDist(ANIds::HADRONOX_PLATFORM_X, ANIds::HADRONOX_PLATFORM_Y, ANIds::HADRONOX_PLATFORM_Z) <=
            WEB_SPOT_ARRIVED &&
        hadronox->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE;
    bool const wasWebbed = WebbedLatch(bot->GetInstanceId(), false, false);
    gate.webbed = WebbedLatch(bot->GetInstanceId(), arrived, false);
    if (gate.webbed && !wasWebbed)
        LOG_INFO("playerbots", "[an hadronox webbed] {}: Hadronox reached her web spot ({:.1f} yd from it), the doors "
                 "are webbed, the group may engage her", bot->GetName(),
                 hadronox->GetExactDist(ANIds::HADRONOX_PLATFORM_X, ANIds::HADRONOX_PLATFORM_Y,
                                        ANIds::HADRONOX_PLATFORM_Z));
    return gate;
}

// Hadronox is alive in a running encounter and has not webbed the doors yet: nobody touches her, so the group's first
// contact cannot cancel her climb, and no fight on her is spent against the open door stream. Only when her climb is
// provably lost (engaged by the group, every Crusher dead, standing short of her spot for HADRONOX_STALL_MS: an-h
// 20261025-150737) is she killed before webbing.
inline bool HadronoxHandsOff(PlayerbotAI* botAI)
{
    HadronoxGate const& gate = GetHadronoxGate(botAI);
    if (gate.boss.IsEmpty() || gate.webbed)
        return false;

    if (gate.stalled)
    {
        if (WlkTacticLog::Allow(botAI->GetBot(), "an hadronox engaged"))
            LOG_INFO("playerbots", "[an hadronox engaged] {}: Hadronox fights the group short of her web spot with the "
                     "Crushers dead, her climb is over: kill her (her death ends the door stream)",
                     botAI->GetBot()->GetName());
        return false;
    }
    return true;
}

// [b33_hadronox2] Her climb (MOVE3, boss_hadronox.cpp) ends the moment a player lands on her threat list: Web Grab
// 53406 (cast on herself every 25 s once she fights the door adds) takes everyone within 60 yd, Leech Poison 53030 /
// 59417 everyone within 25 yd. The platform the group held is her web spot itself (HADRONOX_PLATFORM), so every
// engage came short of it, the doors stayed open and the stream healed her (each poisoned death +10 %). From the
// Crushers' death until she has webbed the doors the group stands back where the spiral from Krik'thir enters the
// shaft: 78 yd from the web spot, 81 yd from the add ramp's upper landing (her way up), 141 / 160 yd from
// hadronoxSteps[0] / [1] (positions logged on that walkway, an-h 20261026-000434-an-h-5E-35 at 549 s).
inline constexpr float HADRONOX_HOLD_X = 530.0f;
inline constexpr float HADRONOX_HOLD_Y = 624.4f;
inline constexpr float HADRONOX_HOLD_Z = 778.0f;
// Web Grab's 60 yd and a margin: inside this of her, or of the web spot she walks to, the bot goes back
inline constexpr float HADRONOX_GRAB_SAFE = 65.0f;
// at the hold, a bot this close to it stays put (the adds come to the group; no chase back into her reach)
inline constexpr float HADRONOX_HOLD_LEASH = 12.0f;

// The climb phase: every Crusher dead, she has not webbed the doors and no player is on her list yet.
inline bool HadronoxHoldBack(PlayerbotAI* botAI)
{
    HadronoxGate const& gate = GetHadronoxGate(botAI);
    if (gate.boss.IsEmpty() || gate.webbed || gate.engaged || gate.crushersAlive)
        return false;

    Unit* boss = botAI->GetUnit(gate.boss);
    return boss && boss->IsAlive();
}

// The bot within Web Grab's reach of her, or of the spot she climbs to.
inline bool InHadronoxGrabReach(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Unit* boss = botAI->GetUnit(GetHadronoxGate(botAI).boss);
    return (boss && bot->GetExactDist(boss) <= HADRONOX_GRAB_SAFE) ||
           bot->GetExactDist(ANIds::HADRONOX_PLATFORM_X, ANIds::HADRONOX_PLATFORM_Y, ANIds::HADRONOX_PLATFORM_Z) <=
               HADRONOX_GRAB_SAFE;
}

// The Crusher with the least health that fights the group up on Hadronox's platform.
inline Unit* FindPlatformCrusher(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(ANIds::DATA_HADRONOX) != IN_PROGRESS)
        return nullptr;

    Unit* best = nullptr;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != NPC_ANUBAR_CRUSHER)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || ANIds::IsBelowHadronoxPlatform(bot, unit) ||
            bot->GetExactDist(unit) > CRUSHER_SEARCH_RANGE)
            continue;

        if (!best || unit->GetHealthPct() < best->GetHealthPct())
            best = unit;
    }
    return best;
}
// Hadronox herself on her platform once no Crusher fights there (the dps' next target), nullptr otherwise.
inline Unit* FindPlatformHadronox(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(ANIds::DATA_HADRONOX) != IN_PROGRESS || FindPlatformCrusher(botAI) ||
        HadronoxHandsOff(botAI))
        return nullptr;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != ANIds::NPC_HADRONOX)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && !ANIds::IsBelowHadronoxPlatform(bot, unit) &&
            bot->GetExactDist(unit) <= CRUSHER_SEARCH_RANGE)
            return unit;
    }

    // [b31_dg] Not on the bot's attackers: she stands on her web spot (doors webbed, or her climb provably lost) and
    // fights somebody the bot does not list - or nobody yet. an-n 20261025-184229-an-n-5G-32: 19 times "[an hadronox
    // webbed]", then she hit the group from her spot with Leech Poison / Acid Cloud (no melee, so on nobody's
    // attackers) while the four members stood "follow"-ing the stranded tank and dealt 0 damage for 26 s each time
    // (pull_end damage 0), until she evaded (HARD_RESET despawn) and the encounter reset: 14 "boss attempts". Once
    // the hands-off gate lets go she IS the fight: taken from the gate, on the platform, within reach - by the tank
    // at once (its pull), by the others once she fights.
    if (Unit* unit = botAI->GetUnit(GetHadronoxGate(botAI).boss))
        if (unit->IsAlive() && !ANIds::IsBelowHadronoxPlatform(bot, unit) &&
            bot->GetExactDist(unit) <= CRUSHER_SEARCH_RANGE && (PlayerbotAI::IsTank(bot) || unit->IsInCombat()))
            return unit;
    return nullptr;
}
}  // namespace ANAdds

// A fixated non-tank whose back is turned to its Skirmisher.
class SkirmisherFixateTrigger : public Trigger
{
public:
    SkirmisherFixateTrigger(PlayerbotAI* botAI) : Trigger(botAI, "anub'ar skirmisher fixate") {}

    bool IsActive() override
    {
        if (PlayerbotAI::IsTank(bot))
            return false;

        Unit* skirmisher = ANAdds::FindFixatingSkirmisher(botAI);
        return skirmisher && !bot->HasInArc(ANAdds::FIXATE_FACING_ARC, skirmisher);
    }
};

// Turns to the Skirmisher: a plain Action (no movement), so the fixate hold below does not block it.
class FaceFixatingSkirmisherAction : public Action
{
public:
    FaceFixatingSkirmisherAction(PlayerbotAI* botAI) : Action(botAI, "face fixating skirmisher") {}

    bool Execute(Event /*event*/) override
    {
        Unit* skirmisher = ANAdds::FindFixatingSkirmisher(botAI);
        if (!skirmisher)
            return false;

        ServerFacade::instance().SetFacingTo(bot, skirmisher, true);
        return true;
    }
};

// While a Skirmisher is fixated on it, a non-tank stands its ground: every step away (ranged leaving melee range,
// a healer repositioning) shows it the back. The tank taunts it off and the dps kill it first (krik'thir priority).
class SkirmisherFixateHoldMultiplier : public Multiplier
{
public:
    SkirmisherFixateHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "anub'ar skirmisher fixate hold") {}

    float GetValue(Action* action) override
    {
        if (!TbcHazard::IsRepositionAction(action) || PlayerbotAI::IsTank(bot))
            return 1.0f;

        return ANAdds::FindFixatingSkirmisher(botAI) ? 0.0f : 1.0f;
    }
};

// Damage dealers whose target is not a Crusher while one fights the group on the platform.
class HadronoxCrusherTrigger : public Trigger
{
public:
    HadronoxCrusherTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hadronox crusher") {}

    bool IsActive() override
    {
        if (!PlayerbotAI::IsDps(bot))
            return false;

        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->IsAlive() && target->GetEntry() == ANAdds::NPC_ANUBAR_CRUSHER)
            return false;

        return ANAdds::FindPlatformCrusher(botAI) != nullptr;
    }
};

// Damage dealers whose target is not Hadronox once the Crushers are dead: she is the fight, the swarm from the doors
// never ends (an-h 2026-10-25: 785 s, 2.8M group damage of which Hadronox took ~0.36M, the healer ran out of mana
// and she killed the tank at 17 %).
class HadronoxBossTrigger : public Trigger
{
public:
    HadronoxBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hadronox boss") {}

    bool IsActive() override
    {
        // [b31_dg] the tank too: it takes her once she may be fought (webbed / stalled), unless it already holds her.
        // an-n 20261025-184229-an-n-5G-32: every one of the 14 Hadronox engages was "unplanned", her own aggro on a
        // melee dps; no bot ever pulled or tanked her on purpose.
        bool const tank = PlayerbotAI::IsTank(bot);
        if (!tank && !PlayerbotAI::IsDps(bot))
            return false;

        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->IsAlive() && target->GetEntry() == ANIds::NPC_HADRONOX)
            return false;

        Unit* boss = ANAdds::FindPlatformHadronox(botAI);
        return boss && !(tank && boss->GetVictim() == bot);
    }
};

// Before the doors are webbed (ANAdds::HadronoxHandsOff): no action aimed at Hadronox, from anyone (the tank too),
// and no area spell while she is near the bot or its target, so her first player contact cannot cancel her climb.
// The Crushers right by her web spot (crusherWaypoints) are fought with single-target spells meanwhile.
class HadronoxHandsOffMultiplier : public Multiplier
{
public:
    HadronoxHandsOffMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hadronox hands off") {}

    float GetValue(Action* action) override
    {
        if (bot->GetMapId() != ANWatcherRoom::MAP_AZJOL_NERUB || !bot->IsInCombat() ||
            !ANAdds::HadronoxHandsOff(botAI))
            return 1.0f;

        Unit* boss = botAI->GetUnit(ANAdds::GetHadronoxGate(botAI).boss);
        if (!boss)
            return 1.0f;

        Unit* target = action->GetTarget();
        if (target && target->GetGUID() == boss->GetGUID())
        {
            if (WlkTacticLog::Allow(bot, "an hadronox hold"))
                LOG_INFO("playerbots", "[an hadronox hold] {}: {} on Hadronox refused, she has not webbed the doors "
                         "yet ({:.1f} yd from her spot)", bot->GetName(), action->getName(),
                         boss->GetExactDist(ANIds::HADRONOX_PLATFORM_X, ANIds::HADRONOX_PLATFORM_Y,
                                            ANIds::HADRONOX_PLATFORM_Z));
            return 0.0f;
        }

        if (action->getThreatType() != Action::ActionThreatType::Aoe)
            return 1.0f;

        Unit* current = AI_VALUE(Unit*, "current target");
        bool const closeToBoss = bot->GetExactDist(boss) <= ANAdds::HADRONOX_AOE_SAFE ||
                          (current && current->GetExactDist(boss) <= ANAdds::HADRONOX_AOE_SAFE);
        if (!closeToBoss)
            return 1.0f;

        if (WlkTacticLog::Allow(bot, "an hadronox no aoe"))
            LOG_INFO("playerbots", "[an hadronox no aoe] {}: {} held, Hadronox is {:.1f} yd away and the doors are "
                     "not webbed", bot->GetName(), action->getName(), bot->GetExactDist(boss));
        return 0.0f;
    }
};

// [b33_hadronox] Once she is fought: no damage dealer's area spell on the adds around her. Spell.dbc / FactionTemplate:
// her Leech Poison 53030 (heroic 59417) is SRC_AREA_ENEMY, 25 yd, uncapped, 500 (1000) a second for 10 s, recast every
// 12 s (boss_hadronox.cpp :322-325), and the door adds (faction 1814) are her enemies (1965 lists 966 as an enemy), so
// every add within 25 yd of her carries it; spell_hadronox_leech_poison_aura casts Leech Poison 53800 (heal 10 % of
// her maximum health) for each poisoned unit that dies, KilledUnit adds another 10 % for one she kills herself. An area
// spell on the adds stacked on her kills them inside her poison: +15.4k health (normal) a dead add.
// an-n 20261026-000434: 245 s on her, 1.19M group damage, 191 adds dead, she never fell (154k health). The general
// "stack the adds on the boss and area them" (KillOrder::BossFocusTarget) is the wrong play for her alone; the single
// target stays on her and the adds die to the tank's area threat and single-target peels.
inline constexpr float HADRONOX_LEECH_POISON_RADIUS = 25.0f;
// an area spell lands around its target: the target this close to her puts the area in her poison
inline constexpr float HADRONOX_ADD_AOE_GUARD = HADRONOX_LEECH_POISON_RADIUS + 5.0f;

class HadronoxLeechAoeMultiplier : public Multiplier
{
public:
    HadronoxLeechAoeMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hadronox leech aoe") {}

    float GetValue(Action* action) override
    {
        if (action->getThreatType() != Action::ActionThreatType::Aoe || PlayerbotAI::IsTank(bot) ||
            bot->GetMapId() != ANWatcherRoom::MAP_AZJOL_NERUB || !bot->IsInCombat())
            return 1.0f;

        ANAdds::HadronoxGate const& gate = ANAdds::GetHadronoxGate(botAI);
        if (gate.boss.IsEmpty() || ANAdds::HadronoxHandsOff(botAI))
            return 1.0f;  // not fought yet: HadronoxHandsOffMultiplier rules

        Unit* boss = botAI->GetUnit(gate.boss);
        if (!boss || !boss->IsAlive())
            return 1.0f;

        Unit* target = action->GetTarget();
        if (!target)
            target = AI_VALUE(Unit*, "current target");
        bool const inPoison = bot->GetExactDist(boss) <= HADRONOX_LEECH_POISON_RADIUS ||
                              (target && target->GetExactDist(boss) <= HADRONOX_ADD_AOE_GUARD);
        if (!inPoison)
            return 1.0f;

        if (WlkTacticLog::Allow(bot, "an hadronox leech aoe"))
            LOG_INFO("playerbots", "[an hadronox leech aoe] {}: {} held, the adds around Hadronox ({:.1f} yd) carry "
                     "her Leech Poison: each one dying heals her 10 %", bot->GetName(), action->getName(),
                     target ? target->GetExactDist(boss) : bot->GetExactDist(boss));
        return 0.0f;
    }
};

// [b33_hadronox2] The climb phase (ANAdds::HadronoxHoldBack): a bot in Web Grab's reach of her or of her web spot.
class HadronoxHoldBackTrigger : public Trigger
{
public:
    HadronoxHoldBackTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hadronox hold back") {}

    bool IsActive() override
    {
        return bot->IsAlive() && bot->GetMapId() == ANWatcherRoom::MAP_AZJOL_NERUB && ANAdds::HadronoxHoldBack(botAI) &&
               ANAdds::InHadronoxGrabReach(botAI);
    }
};

class HadronoxHoldBackAction : public MovementAction
{
public:
    HadronoxHoldBackAction(PlayerbotAI* botAI) : MovementAction(botAI, "hadronox hold back") {}

    bool Execute(Event /*event*/) override
    {
        if (WlkTacticLog::Allow(bot, "an hadronox hold back"))
            LOG_INFO("playerbots", "[an hadronox hold back] {}: the Crushers are dead and Hadronox climbs to her web "
                     "spot: back to ({:.1f}, {:.1f}, {:.1f}), {:.1f} yd from the spot, until she has webbed the doors",
                     bot->GetName(), ANAdds::HADRONOX_HOLD_X, ANAdds::HADRONOX_HOLD_Y, ANAdds::HADRONOX_HOLD_Z,
                     bot->GetExactDist(ANIds::HADRONOX_PLATFORM_X, ANIds::HADRONOX_PLATFORM_Y,
                                       ANIds::HADRONOX_PLATFORM_Z));
        return MoveTo(bot->GetMapId(), ANAdds::HADRONOX_HOLD_X, ANAdds::HADRONOX_HOLD_Y, ANAdds::HADRONOX_HOLD_Z, false,
                      false, false, false, MovementPriority::MOVEMENT_COMBAT, true);
    }
};

// [b33_hadronox2] At the hold during the climb phase no reposition walks back into her reach (a chase after an add,
// "reach spell", the follow / route step to the platform): the adds come to the group. The hold-back step itself,
// attacks and the hazard dodges stay.
class HadronoxHoldBackMultiplier : public Multiplier
{
public:
    HadronoxHoldBackMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hadronox hold back") {}

    float GetValue(Action* action) override
    {
        if (bot->GetMapId() != ANWatcherRoom::MAP_AZJOL_NERUB || dynamic_cast<HadronoxHoldBackAction*>(action) ||
            !TbcHazard::IsRepositionAction(action) || action->getName().rfind("avoid", 0) == 0)
            return 1.0f;

        if (bot->GetExactDist(ANAdds::HADRONOX_HOLD_X, ANAdds::HADRONOX_HOLD_Y, ANAdds::HADRONOX_HOLD_Z) >
                ANAdds::HADRONOX_HOLD_LEASH ||
            !ANAdds::HadronoxHoldBack(botAI))
            return 1.0f;

        return 0.0f;
    }
};

// [b33_hadronox2] Once she is fought: her Leech Poison (25 yd around her, ANAdds above) poisons every add near her, and
// an add dying poisoned heals her 10 %. The adds go for the ranged and the healer: those stand outside the poison so
// the adds on them die out of it (with HadronoxLeechAoeMultiplier's no area spell near her). Melee and the tank stay.
inline constexpr float HADRONOX_LEECH_STANDOFF_TRIGGER = HADRONOX_LEECH_POISON_RADIUS + 2.0f;
inline constexpr float HADRONOX_LEECH_STANDOFF = HADRONOX_LEECH_POISON_RADIUS + 5.0f;

class HadronoxLeechStandoffTrigger : public Trigger
{
public:
    HadronoxLeechStandoffTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hadronox leech standoff") {}

    bool IsActive() override
    {
        if (!bot->IsAlive() || !bot->IsInCombat() || bot->GetMapId() != ANWatcherRoom::MAP_AZJOL_NERUB ||
            PlayerbotAI::IsTank(bot) || !(PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot)))
            return false;

        ANAdds::HadronoxGate const& gate = ANAdds::GetHadronoxGate(botAI);
        if (gate.boss.IsEmpty() || ANAdds::HadronoxHandsOff(botAI))
            return false;

        Unit* boss = botAI->GetUnit(gate.boss);
        return boss && boss->IsAlive() && boss->IsInCombat() &&
               bot->GetExactDist(boss) < HADRONOX_LEECH_STANDOFF_TRIGGER;
    }
};

class HadronoxLeechStandoffAction : public MovementAction
{
public:
    HadronoxLeechStandoffAction(PlayerbotAI* botAI) : MovementAction(botAI, "hadronox leech standoff") {}

    bool Execute(Event /*event*/) override
    {
        Unit* boss = botAI->GetUnit(ANAdds::GetHadronoxGate(botAI).boss);
        if (!boss)
            return false;

        // A spot on her own floor (her platform hangs over the shaft) in her line of sight, the nearest to the bot.
        constexpr int STEPS = 16;
        constexpr float FLOOR_TOLERANCE = 4.0f;
        float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f, bestDist = 0.0f;
        bool found = false;
        for (int i = 0; i < STEPS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / STEPS;
            float const x = boss->GetPositionX() + std::cos(angle) * HADRONOX_LEECH_STANDOFF;
            float const y = boss->GetPositionY() + std::sin(angle) * HADRONOX_LEECH_STANDOFF;
            float const z = bot->GetMapWaterOrGroundLevel(x, y, boss->GetPositionZ() + FLOOR_TOLERANCE);
            if (z <= INVALID_HEIGHT || std::fabs(z - boss->GetPositionZ()) > FLOOR_TOLERANCE ||
                !boss->IsWithinLOS(x, y, z + 2.0f))
                continue;

            float const dist = bot->GetExactDist2d(x, y);
            if (!found || dist < bestDist)
            {
                found = true;
                bestX = x;
                bestY = y;
                bestZ = z;
                bestDist = dist;
            }
        }
        if (!found)
            return false;

        if (WlkTacticLog::Allow(bot, "an hadronox leech standoff"))
            LOG_INFO("playerbots", "[an hadronox leech standoff] {}: {:.1f} yd from Hadronox, out of her Leech Poison "
                     "to ({:.1f}, {:.1f}, {:.1f}) so the adds on it die outside it", bot->GetName(),
                     bot->GetExactDist(boss), bestX, bestY, bestZ);
        return MoveTo(bot->GetMapId(), bestX, bestY, bestZ, false, false, false, false,
                      MovementPriority::MOVEMENT_COMBAT, true);
    }
};

class HadronoxAttackBossAction : public AttackAction
{
public:
    HadronoxAttackBossAction(PlayerbotAI* botAI) : AttackAction(botAI, "hadronox attack boss") {}

    bool isUseful() override { return !PlayerbotAI::IsHeal(bot); }

    bool Execute(Event /*event*/) override
    {
        Unit* boss = ANAdds::FindPlatformHadronox(botAI);
        if (!boss || AI_VALUE(Unit*, "current target") == boss)
            return false;

        return Attack(boss);
    }
};

class HadronoxAttackCrusherAction : public AttackAction
{
public:
    HadronoxAttackCrusherAction(PlayerbotAI* botAI) : AttackAction(botAI, "hadronox attack crusher") {}

    bool isUseful() override { return !PlayerbotAI::IsHeal(bot); }

    bool Execute(Event /*event*/) override
    {
        Unit* crusher = ANAdds::FindPlatformCrusher(botAI);
        if (!crusher || AI_VALUE(Unit*, "current target") == crusher)
            return false;

        return Attack(crusher);
    }
};

#endif
