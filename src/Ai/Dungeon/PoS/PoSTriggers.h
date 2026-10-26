/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_POSTRIGGERS_H
#define PLAYERBOTS_POSTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Position.h"  // [b33_pos]
#include "Trigger.h"

class GameObject;
class InstanceScript;

enum PitOfSaronIDs
{
    //NPCs
    NPC_GARFROST                        = 36494,
    NPC_KRICK                           = 36477,
    NPC_ICK                             = 36476,
    NPC_TYRANNUS                        = 36658,
    NPC_RIMEFANG                        = 36661,

    //Spells
    SPELL_PURSUIT                       = 68987,
    SPELL_POISON_NOVA_POS               = 68989,
    SPELL_POISON_NOVA_POS_HC            = 70434,
    SPELL_EXPLOSIVE_BARRAGE_KRICK       = 69012,
    SPELL_EXPLOSIVE_BARRAGE_ICK         = 69263,
    SPELL_EXPLOSIVE_BARRAGE_SUMMON      = 69015,
    SPELL_EXPLODING_ORB_VISUAL          = 69017,
    SPELL_MARK_OF_RIMEFANG              = 69275,
    RIMEFANG_SPELL_HOARFROST            = 69246,
    RIMEFANG_SPELL_HOARFROST_HC         = 69245,
    RIMEFANG_SPELL_HOARFROST_HC2        = 69645,
};

namespace PoSIds
{
constexpr uint32 MAP_PIT_OF_SARON = 658;

// pit_of_saron.h DataTypes / InstanceProgress
constexpr uint32 DATA_GARFROST = 0;
constexpr uint32 DATA_ICK = 1;
constexpr uint32 DATA_TYRANNUS = 2;
constexpr uint32 DATA_INSTANCE_PROGRESS = 4;
constexpr uint32 DATA_GARFROST_GUID = 12;
constexpr uint32 DATA_RIMEFANG_GUID = 14;
constexpr uint32 PROGRESS_FINISHED_KRICK_SCENE = 2;
constexpr uint32 PROGRESS_AFTER_WARN_1 = 3;
constexpr uint32 PROGRESS_AFTER_WARN_2 = 4;
constexpr uint32 PROGRESS_AFTER_TUNNEL_WARN = 5;

// areatrigger / areatrigger_scripts: SmartTrigger SetData 1/2/3 on the Tyrannus event NPC, at_tyrannus_event_starter
constexpr uint32 AT_AMBUSH_1 = 5578;
constexpr uint32 AT_AMBUSH_2 = 5579;
constexpr uint32 AT_TUNNEL = 5580;
constexpr uint32 AT_TYRANNUS_INTRO = 5633;

// boss_forgemaster_garfrost.cpp
constexpr uint32 GO_SARONITE_ROCK = 196485;
constexpr uint32 SPELL_PERMAFROST_STACK = 68786;
constexpr uint32 SPELL_PERMAFROST_STACK_H = 70336;
constexpr uint32 SPELL_THROW_SARONITE = 68788;
constexpr float PERMAFROST_ROCK_SIZE = 4.0f;  // spell_garfrost_permafrost: IsInBetween(caster, target, 4.0f)

// pit_of_saron.cpp npc_pos_collapsing_icicle: lands 2.5 s after the summon (69428 cast + 69426 aura), Ice Shards 7 yd
constexpr uint32 NPC_COLLAPSING_ICICLE = 36847;
constexpr uint32 SPELL_ICICLE_FALL = 69428;
constexpr uint32 SPELL_ICICLE_IMPACT = 69426;
constexpr float ICICLE_RADIUS = 7.0f;

// boss_scourgelord_tyrannus.cpp. The Mark aura 69275 is never applied by this script, only Hoarfrost is cast at
// the marked player (5 s cast, 400 yd/s missile): the bearer is known from the cast target of Rimefang.
constexpr uint32 SPELL_OVERLORDS_BRAND = 69172;
constexpr float HOARFROST_RADIUS = 8.0f;  // SpellRadius index 14 of 69245
constexpr float HOARFROST_SPREAD = HOARFROST_RADIUS + 4.0f;

// [b33_pos] pit_of_saron.h DATA_TYRANNUS_GUID. npc_pos_martin_or_gorkun_second (pit_of_saron.cpp) keeps up to 3
// Fallen Warriors 36841 coming at the freed slaves from 15 s after the intro until Tyrannus dies: they spawn at
// (1060.9, 102.8), walk to (1047-1057, 118-128) and fight the slaves lined up at TSData (1044-1059, 118-131). They
// are no part of the boss fight. Both wipes (pos-h 20261026-000434-5B-39, pos-n 5H-54) fought Tyrannus 26-29 yd
// from that line (group centre ~1031,140): 6-10 aggro losses to Fallen Warriors, which then killed the casters and
// the healer. Every clean kill (14 runs) fought at (1008-1029, 146-177), 35-79 yd away, with 0-4 of them.
constexpr uint32 DATA_TYRANNUS_GUID = 15;
constexpr uint32 NPC_FALLEN_WARRIOR = 36841;
const Position SLAVE_LINE_CENTER = Position(1052.0f, 124.0f, 628.16f);
// Tyrannus lands at (1023.5, 159.1); this spot is 5 yd from his leash centre TSDistCheckPos (100 yd) and 58 yd from
// the slave line.
const Position TYRANNUS_TANK_SPOT = Position(1010.0f, 168.0f, 628.16f);
constexpr float SLAVE_LINE_KEEP_OUT = 38.0f;  // a Fallen Warrior's aggro range plus their 10 yd walk-in spread
}  // namespace PoSIds

class IckAndKrickTrigger : public Trigger
{
public:
    IckAndKrickTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ick and krick") {}

    bool IsActive() override;
};

class TyrannusTrigger : public Trigger
{
public:
    TyrannusTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tyrannus") {}

    bool IsActive() override;
};

// Sends the route area triggers of the Tyrannus event when the bot is inside them (no client sends them).
class PoSAreaTriggerTrigger : public Trigger
{
public:
    PoSAreaTriggerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pos area trigger", 2000) {}
    bool IsActive() override;
};

// Permafrost stacks are high (or still up while hidden): stand behind a Saronite Rock until they drop.
class PoSPermafrostTrigger : public Trigger
{
public:
    PoSPermafrostTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pos permafrost") {}
    bool IsActive() override;
};

// Garfrost throws a boulder at this bot: keep moving through the cast end and the missile flight.
class PoSThrowSaroniteTrigger : public Trigger
{
public:
    PoSThrowSaroniteTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pos throw saronite") {}
    bool IsActive() override;

private:
    uint32 dodgeUntil = 0;
};

class PoSIcicleTrigger : public Trigger
{
public:
    PoSIcicleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pos icicle", 200) {}
    bool IsActive() override;
};

// Overlord's Brand copies the bearer's damage onto the tank and its heals onto Tyrannus.
class PoSOverlordsBrandTrigger : public Trigger
{
public:
    PoSOverlordsBrandTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pos overlords brand") {}
    bool IsActive() override;
};

// [b33_pos] Tyrannus encounter in progress and the bot stands near the freed slaves' line (the Fallen Warriors' way
// in): it walks over to the Tyrannus tank spot. The tank also drags Tyrannus there.
class PoSTyrannusPositionTrigger : public Trigger
{
public:
    PoSTyrannusPositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pos tyrannus position", 500) {}
    bool IsActive() override;
};

// The Mark of Rimefang bearer is too close to someone else: Hoarfrost lands on the bearer's spot.
class PoSMarkOfRimefangTrigger : public Trigger
{
public:
    PoSMarkOfRimefangTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pos mark of rimefang", 250) {}
    bool IsActive() override;
};

namespace PoSShared
{
InstanceScript* GetInstance(Player* bot);
Unit* GetGarfrost(Player* bot);
uint8 PermafrostStacks(Unit* unit);
// Saronite Rock shielding the bot from Garfrost right now (the script's own LoS test), else nullptr.
GameObject* GetCoveringRock(Player* bot, Unit* garfrost);
// The Saronite Rock with the nearest hiding spot (the rock between Garfrost and the spot), and the spot.
GameObject* FindHidingSpot(Player* bot, Unit* garfrost, Position& spot);
// Non-tank with high Permafrost stacks, or with stacks while already covered by a rock.
bool ShouldHideFromPermafrost(Player* bot);
// Area trigger of the Tyrannus event route the instance waits for now, or 0.
uint32 PendingAreaTrigger(Player* bot);
// Nearest Collapsing Icicle that still has to land within reach of the bot, else nullptr.
Unit* FindFallingIcicle(Player* bot);
// The group member carrying the Mark of Rimefang within range of the bot (the bot itself first), else nullptr.
Player* FindMarkedMember(Player* bot, float range);
// Ick is casting Poison Nova (68989/70434: 5 s, 15 yd). No UNIT_STATE_CASTING test: a lingering channel keeps it set.
bool IsPoisonNova(Unit* ick);
// [b33_pos] Tyrannus while his fight is in progress and he is alive, else nullptr.
Unit* GetTyrannus(Player* bot);
// [b33_pos] A Fallen Warrior fighting the freed slaves, not the group (nobody targets it, no AoE counts it).
bool IsSlaveFightWarrior(Unit* unit);
// [b33_pos] Where the bot should stand instead of near the slave line, or false when it already stands clear.
bool TyrannusSpotFor(Player* bot, Position& spot);
// [b33_pos] Overlord's Brand: the bearer's damage would be copied onto an ally (anyone but a tank Tyrannus is
// hitting: spell_gen_overlords_brand casts 69189 at Tyrannus's victim).
bool BrandDamageHitsAlly(Player* bot);
float PoisonNovaDangerRadius(Player* bot);
}  // namespace PoSShared

#endif
