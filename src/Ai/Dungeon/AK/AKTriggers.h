/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AKTRIGGERS_H
#define PLAYERBOTS_AKTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "ObjectGuid.h"
#include "Trigger.h"

#include <array>

enum OldKingdomIDs
{
    // Elder Nadox
    BUFF_GUARDIAN_AURA                 = 56153,

    // Jedoga Shadowseeker
    NPC_TWILIGHT_VOLUNTEER             = 30385,

    // Forgotten One(s)
    SPELL_SHADOW_CRASH_N               = 60833,
    SPELL_SHADOW_CRASH_H               = 60848,
};

#define SPELL_SHADOW_CRASH             DUNGEON_MODE(bot, SPELL_SHADOW_CRASH_N, SPELL_SHADOW_CRASH_H)

namespace AKIds
{
// ahnkahet.h
constexpr uint32 DATA_ELDER_NADOX = 0;
constexpr uint32 DATA_PRINCE_TALDARAM = 1;
constexpr uint32 DATA_JEDOGA_SHADOWSEEKER = 2;
constexpr uint32 PERSISTENT_DATA_SPHERE1 = 0;
constexpr uint32 PERSISTENT_DATA_SPHERE2 = 1;
constexpr uint32 GO_TELDARAM_SPHERE1 = 193093;
constexpr uint32 GO_TELDARAM_SPHERE2 = 193094;
// boss_jedoga_shadowseeker.cpp
constexpr uint32 NPC_TWILIGHT_INITIATE = 30114;
constexpr float JEDOGA_CENTER_X = 372.331f;
constexpr float JEDOGA_CENTER_Y = -705.278f;
constexpr float JEDOGA_INITIATE_RANGE = 50.0f;

// boss_herald_volazj.cpp Insanity: every player is phased alone (57508-57512) with Twisted Visages of the others;
// Volazj stays stunned and unselectable until every visage of every phase is dead.
constexpr uint32 NPC_TWISTED_VISAGE = 30625;
constexpr std::array<uint32, 5> SPELL_INSANITY_PHASING = {{ 57508, 57509, 57510, 57511, 57512 }};
constexpr float INSANITY_HEALER_SELF_PCT = 60.0f;  // below this a healer heals itself instead of fighting

constexpr float DEVICE_SEARCH_RANGE = 15.0f;
constexpr float DEVICE_USE_DISTANCE = 2.0f;
constexpr float DEVICE_GATHER_RADIUS = 80.0f;

struct NerubianDevice
{
    uint32 persistentIndex;
    uint32 goEntry;
    float x;
    float y;
    float z;
};

// gameobject spawns 67331 / 67332 (acore_world)
constexpr std::array<NerubianDevice, 2> NERUBIAN_DEVICES = {{
    { PERSISTENT_DATA_SPHERE1, GO_TELDARAM_SPHERE1, 655.729f, -719.049f, 18.0264f },
    { PERSISTENT_DATA_SPHERE2, GO_TELDARAM_SPHERE2, 692.465f, -783.911f, 18.0264f },
}};

// Nearest Ancient Nerubian Device not activated yet while Taldaram is alive (instance_ahnkahet.cpp SetData);
// nullptr when both are done.
NerubianDevice const* FindPendingDevice(Player* bot);
}  // namespace AKIds

// Jedoga's 15 kneeling Twilight Initiates are an encounter gate, not a trash pack (boss_jedoga_shadowseeker.cpp:
// Reset() 165-216 summons them kneeling and attackable; SummonedCreatureDies 226-266 brings Jedoga down only when
// the last of them dies). The careful planner counted them as one 15-mob pack over its cap and walked off
// (ok-n 20261025-062726: "guarantee_hold ... Twilight Initiate (15 mobs ...)", then "skip_target route end"), so
// the room owns the group's pulls while they live: the tank pulls them one at a time, the nearest one it can see.
namespace AKJedogaRoom
{
constexpr uint32 MAP_AHNKAHET = 619;
// Jedoga's ritual spot on the altar (JedogaPosition[2], POINT_RITUAL), where the initiates kneel around.
constexpr float ALTAR_X = 373.48f;
constexpr float ALTAR_Y = -706.0f;
constexpr float ALTAR_Z = -16.18f;
constexpr float ROOM_RADIUS = 60.0f;
// The altar floor is at z -16.2; the Volunteers come from the lower ring at z -31.6.
constexpr float ROOM_MIN_Z = -26.0f;
constexpr float ROOM_MAX_Z = 0.0f;
constexpr float APPROACH_DISTANCE = 4.0f;
// The tank pulls once the group stands with it; after this long it pulls with the healer alone at its side.
constexpr uint32 GATHER_WAIT_MS = 30 * IN_MILLISECONDS;
constexpr float HEALER_NEAR = 30.0f;
constexpr uint32 CACHE_MS = 500;

struct Snapshot
{
    uint32 ms = 0;
    bool active = false;         // the bot is at the altar, Jedoga not engaged, an initiate alive
    uint32 initiatesAlive = 0;
    ObjectGuid visibleTarget;    // the nearest initiate in the bot's line of sight
    ObjectGuid nearestTarget;    // the nearest initiate, seen or not
};

Snapshot const& Get(PlayerbotAI* botAI);
// The room decides the group's next pull while initiates live (the careful planner's hook, like
// ANWatcherRoom::OwnsPull).
bool OwnsPull(PlayerbotAI* botAI);
}  // namespace AKJedogaRoom

class NadoxGuardianTrigger : public Trigger
{
public:
    NadoxGuardianTrigger(PlayerbotAI* botAI) : Trigger(botAI, "elder nadox guardian") {}
    bool IsActive() override;
};

class JedogaVolunteerTrigger : public Trigger
{
public:
    JedogaVolunteerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "jedoga volunteer") {}
    bool IsActive() override;
};

class TaldaramNerubianDeviceTrigger : public Trigger
{
public:
    TaldaramNerubianDeviceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "taldaram nerubian device") {}
    bool IsActive() override;
};

class JedogaInitiatesTrigger : public Trigger
{
public:
    JedogaInitiatesTrigger(PlayerbotAI* botAI) : Trigger(botAI, "jedoga initiates") {}
    bool IsActive() override;
};

namespace AKShared
{
bool IsInInsanity(Player* bot);
// The weakest Twisted Visage in the bot's own phase (only those can be seen and attacked), else nullptr.
Unit* FindVisibleVisage(PlayerbotAI* botAI);
}

// Volazj Insanity: a visage of the bot's own phase is up and the bot is not fighting it.
class VolazjInsanityTrigger : public Trigger
{
public:
    VolazjInsanityTrigger(PlayerbotAI* botAI) : Trigger(botAI, "volazj insanity") {}
    bool IsActive() override;
};

class ShadowCrashTrigger : public Trigger
{
public:
    ShadowCrashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shadow crash") {}
    bool IsActive() override;
};

// [b33_ok] Amanitar (heroic only; boss_amanitar.cpp, SmartAI 30391 / 30435, Spell.dbc):
// - on engage 32 mushrooms spawn over the bog (40 % Healthy 30391, 60 % Poisonous 30435), respawning every 40-60 s;
// - Mini 57055 (first at 25-32 s, then every 30-45 s) on every player: -75 % damage done, -500 Defense, no dispel;
// - a dying Healthy Mushroom casts Potent Fungus 56648 on players within 4 yd: +100 % damage, removes Mini
//   (spell_linked_spell 56648 -> -57055); Remove Mushroom Power 57283 strips it again 29 s after each Mini;
// - a Poisonous Mushroom that is damaged, or sees a player within 3 yd, casts Poison Cloud 57061 (3237 nature a tick
//   for 8 s on everyone within 5 yd) and despawns.
// ok-h 20261026-000434-ok-h-5C-36: nothing handled it; AoE and the tank's "tank assist" killed 113 mushrooms in
// 212 s, nobody stood at a Healthy one, the group did 2.6k dps of the expected 7.3k (Mini), the healer went OOM.
class Creature;

namespace AKAmanitar
{
constexpr uint32 MAP_AHNKAHET = 619;
constexpr uint32 NPC_AMANITAR = 30258;
constexpr uint32 NPC_HEALTHY_MUSHROOM = 30391;
constexpr uint32 NPC_POISONOUS_MUSHROOM = 30435;
constexpr uint32 SPELL_MINI = 57055;
constexpr uint32 SPELL_POTENT_FUNGUS = 56648;
constexpr float SCAN_RANGE = 60.0f;
constexpr float FUNGUS_KILL_DISTANCE = 3.0f;  // Potent Fungus reaches 4 yd round the dying mushroom
constexpr float FUNGUS_APPROACH = 1.5f;
constexpr float POISON_CLEARANCE = 8.0f;      // no Poisonous Mushroom this close to the Healthy one picked
constexpr float MAX_DZ = 5.0f;
constexpr float TANK_REACH = 12.0f;           // the tank drags the boss along: only a mushroom next to it
constexpr float MELEE_REACH = 20.0f;
constexpr float RANGED_REACH = 30.0f;
constexpr uint32 CACHE_MS = 500;

struct Snapshot
{
    uint32 ms = 0;
    bool active = false;  // Amanitar is alive and fighting near the bot
    GuidVector healthy;
    GuidVector poisonous;
    ObjectGuid fungusTarget;  // FindFungusMushroom's pick, set only while the bot needs the fungus
};

Snapshot const& Get(PlayerbotAI* botAI);
bool IsMushroom(Unit const* unit);
// Mini is on the bot, Potent Fungus is not, and the bot deals damage (a healer's heals are not reduced).
bool NeedsFungus(Player* bot);
// The nearest Healthy Mushroom in reach of the bot's role, in sight, with no Poisonous one next to it (cached).
Creature* FindFungusMushroom(PlayerbotAI* botAI);
}  // namespace AKAmanitar

class AmanitarMiniTrigger : public Trigger
{
public:
    AmanitarMiniTrigger(PlayerbotAI* botAI) : Trigger(botAI, "amanitar mini") {}
    bool IsActive() override;
};

#endif
