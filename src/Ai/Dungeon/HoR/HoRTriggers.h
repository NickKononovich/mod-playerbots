/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HORTRIGGERS_H
#define PLAYERBOTS_HORTRIGGERS_H

#include "Position.h"
#include "Trigger.h"

#include <string>
#include <vector>

class Creature;
class InstanceScript;
class Player;
class PlayerbotAI;
class Unit;

// Halls of Reflection (map 668). Ids from scripts/Northrend/FrozenHalls/HallsOfReflection/halls_of_reflection.h.
namespace HoRIds
{
constexpr uint32 HOR_MAP_ID = 668;

// Boss indices / data / persistent data (enum Data, HoRPersistentData)
constexpr uint32 DATA_FALRIC = 0;
constexpr uint32 DATA_MARWYN = 1;
constexpr uint32 DATA_LICH_KING = 2;
constexpr uint32 DATA_BATTERED_HILT = 6;
constexpr uint32 DATA_WAVE_NUMBER = 8;
constexpr uint32 PERSISTENT_DATA_INTRO = 0;
constexpr uint32 PERSISTENT_DATA_FROSTSWORN_GENERAL = 1;
constexpr uint32 PERSISTENT_DATA_LK_INTRO = 2;
constexpr uint32 BATTERED_HILT_FINISHED = 4;

// Creatures; the instance registers both leaders under the Horde entry (instance_halls_of_reflection.cpp:124-139)
constexpr uint32 NPC_FALRIC = 38112;
constexpr uint32 NPC_MARWYN = 38113;
constexpr uint32 NPC_LICH_KING_EVENT = 37226;
constexpr uint32 NPC_LICH_KING_BOSS = 36954;
constexpr uint32 NPC_JAINA_PART1 = 37221;
constexpr uint32 NPC_SYLVANAS_PART1 = 37223;
constexpr uint32 NPC_SYLVANAS_PART2 = 37554;
constexpr uint32 NPC_FROSTSWORN_GENERAL = 36723;
// boss_frostsworn_general: copies of the players (ACTION_SPIRITUAL_REFLECTIONS_COPY on engage), sent at him 8 s later.
constexpr uint32 NPC_SPIRITUAL_REFLECTION = 37068;
// boss_frostsworn_general::UpdateAI: beyond 30 yd from his home he evades (EVADE_REASON_OTHER) and hides the copies.
constexpr float GENERAL_LEASH = 30.0f;
constexpr float GENERAL_TANK_HOLD = 18.0f;   // the tank keeps him this close to his home
constexpr float GENERAL_TANK_RETURN = 8.0f;  // and walks back to this far from it
constexpr float GENERAL_GATHER_RANGE = 30.0f;  // nobody opens on him with a member farther than this from the tank
constexpr float REFLECTION_SEARCH = 40.0f;
// Escape: every wall wave is met at the leader; nobody but the tank walks out to a summon farther than this from her.
constexpr float ESCAPE_MEET_RANGE = 12.0f;
// Each summon takes 1000 threat on the player nearest to it (npc_hor_lich_king JustSummoned, SelectNearestPlayer):
// they rise behind the group, so the tank is the rearmost player on the leader-Lich King line, this much behind the
// rearmost other member, and everybody else stays at least ESCAPE_BEHIND_TANK_MARGIN ahead of him.
constexpr float ESCAPE_TANK_BEHIND_GROUP = 3.0f;
constexpr float ESCAPE_BEHIND_TANK_MARGIN = 2.0f;
constexpr float ESCAPE_GROUP_SCAN = 40.0f;
constexpr uint32 NPC_WAVE_PRIEST = 38175;
constexpr uint32 NPC_WAVE_PRIEST_H = 38563;
constexpr uint32 NPC_WAVE_MAGE = 38172;
constexpr uint32 NPC_WAVE_MAGE_H = 38524;
constexpr uint32 NPC_WAVE_MERCENARY = 38177;
constexpr uint32 NPC_WAVE_MERCENARY_H = 38564;
constexpr uint32 NPC_WAVE_RIFLEMAN = 38176;
constexpr uint32 NPC_WAVE_RIFLEMAN_H = 38544;
constexpr uint32 NPC_RISEN_WITCH_DOCTOR = 36941;
constexpr uint32 NPC_RISEN_WITCH_DOCTOR_H = 37551;
// The other escape summons (creature_template 36940/37550, 37069/37549; halls_of_reflection.cpp:1963-2093)
constexpr uint32 NPC_RAGING_GHOUL = 36940;
constexpr uint32 NPC_RAGING_GHOUL_H = 37550;
constexpr uint32 NPC_LUMBERING_ABOMINATION = 37069;
constexpr uint32 NPC_LUMBERING_ABOMINATION_H = 37549;

// Quests that unlock the start gossip of npc_hor_leader (halls_of_reflection.cpp:148-163)
constexpr uint32 QUEST_DELIVERANCE_FROM_THE_PIT_A = 24710;
constexpr uint32 QUEST_DELIVERANCE_FROM_THE_PIT_H = 24712;
constexpr uint32 QUEST_WRATH_OF_THE_LICH_KING_A = 24500;
constexpr uint32 QUEST_WRATH_OF_THE_LICH_KING_H = 24802;

// npc_hor_leader option actions: GOSSIP_ACTION_INFO_DEF + 1 (full intro) / + 2 (short intro)
constexpr uint32 GOSSIP_ACTION_START_INTRO = 1001;
constexpr uint32 GOSSIP_ACTION_SKIP_INTRO = 1002;

constexpr uint32 AREATRIGGER_SHADOW_THRONE = 5605;  // at_hor_shadow_throne: starts the Lich King intro

constexpr uint32 SPELL_SHARED_SUFFERING = 72368;
// Falric (boss_falric.cpp:32-35, 94-106): Quivering Strike 72422 on the victim every 5 s (heroic 72453,
// SpellDifficulty 2175; -20% dodge), Impending Despair 72426 on a random player within 45 yd every 12 s (same id in
// both modes; every 6 s it triggers the 72428 Despair Stricken stun). All three are Magic (Spell.dbc Dispel 1).
constexpr uint32 SPELL_QUIVERING_STRIKE = 72422;
constexpr uint32 SPELL_QUIVERING_STRIKE_H = 72453;
constexpr uint32 SPELL_IMPENDING_DESPAIR = 72426;
// halls_of_reflection.h: the front door closes at the intro's end and on every new wave (StartNextWave); only a wave
// wipe (HandleWaveWipe) or Marwyn's death opens it again.
constexpr uint32 GO_FRONT_DOOR = 201976;
// Falric's Hopelessness (72395/72396/72397, heroic 72390/72391/72393, Spell.dbc) at 67 / 34 / 11 %: -20/-40/-60 %
// (heroic -25/-50/-75 %) damage AND healing done by the players. Heroism / Bloodlust is held until the second one.
constexpr float FALRIC_HEROISM_HEALTH_PCT = 34.0f;

// Waves wipe when any player is farther than 70.5 yd (2d) from the centre and restart only when every player is
// alive within 40 yd (instance_halls_of_reflection.cpp:824-870). The front door (GO 201976, 65 yd from the centre)
// is shut while a wave runs (instance...:724), so the antechamber in front of it cannot be overrun.
constexpr float WAVE_LEASH_RANGE = 60.0f;
// instance_halls_of_reflection.cpp: a player farther than MAX_DIST_FROM_CENTER_IN_COMBAT (70.5) from the centre
// wipes the waves; the last living member walks out this far (along the corner's side) so they reset.
constexpr float WAVE_RESET_LEAVE_RANGE = 78.0f;
// With a member down this few alive inside cannot win the waves (or none of them heals): all leave.
constexpr uint32 WAVE_LOST_MAX_ALIVE = 2;
constexpr float WAVE_STACK_RANGE = 8.0f;
Position const WAVE_CENTER = {5309.459473f, 2006.478516f, 711.595459f, 0.0f};
// Wiped event waiting to resume (wave number 0): everyone alive within 40 yd. LoralenFollowLk1, 16.6 yd out.
Position const WAVE_GATHER_SPOT = {5292.94921f, 2008.25451f, 707.695801f, 0.0f};

// Corner tactic: the group tucks into the entrance antechamber against its west (high-y) wall, between the wall and
// the door-side brazier (5264.3, 1971.7), so the spirits must come round the room/corridor corner (5275, 1988.5) to
// the tank instead of casting from the ring of 34 trash spawns (creature 1971985-1972018, 28-36 yd from the centre,
// all around the room). Coordinates from an offline LoS sweep over the 668 vmaps (Icecrown_Halls_Of_Reflection.wmo +
// braziers, eye height +2 yd) checked against the 668 navmesh (same connected floor as the altar): from the corner
// spot 2 of the 18 caster spawns (Phantom Mage / Ghostly Priest / Tortured Rifleman) are in sight within 40 yd, 9 at
// any range; from the old altar stack spot 14 and 17. Needs an in-game check.
Position const WAVE_CORNER_SPOT = {5267.5f, 1975.0f, 707.70f, 0.0f};  // healers + ranged, 52.5 yd from the centre
Position const WAVE_TANK_SPOT = {5271.0f, 1978.5f, 707.70f, 0.0f};    // tank + melee, 5 yd towards the room
constexpr float WAVE_SETTLE_RANGE = 3.0f;       // out of combat: stand on the spot
constexpr float WAVE_CORNER_HOLD_RANGE = 5.0f;  // in combat, healers + ranged never leave the corner
constexpr float WAVE_TANK_HOLD_RANGE = 10.0f;   // in combat, tank + melee pick the spirits up at the corner edge

// Escape: under Remorseless Winter the Lich King zaps any player with (p.x - lk.x) + (p.y - lk.y) > 20, i.e. one
// that fell behind him on the corridor diagonal (halls_of_reflection.cpp:1637-1646).
constexpr float ESCAPE_BEHIND_LIMIT = 8.0f;
constexpr float ESCAPE_LICH_KING_MIN_RANGE = 15.0f;
constexpr float ESCAPE_LEADER_MAX_RANGE = 25.0f;
constexpr float ESCAPE_LEADER_SETTLE_MARGIN = 7.0f;
constexpr float ESCAPE_GATHER_RANGE = 10.0f;
// Each summon of the escape adds 1000 threat on the player nearest to it, it appears by the Lich King
// (npc_hor_lich_king::JustSummoned, halls_of_reflection.cpp:1571-1584): the tank waits this far from the leader
// towards him, at least ESCAPE_LICH_KING_MIN_RANGE + ESCAPE_TANK_LICH_KING_MARGIN away from him.
constexpr float ESCAPE_TANK_FORWARD = 10.0f;
constexpr float ESCAPE_TANK_LICH_KING_MARGIN = 2.0f;
constexpr float ESCAPE_TANK_SPOT_HOLD = 4.0f;
// The healer keeps to the leader, behind the tank, while summons are up.
constexpr float ESCAPE_HEALER_LEADER_RANGE = 8.0f;
constexpr float ESCAPE_HEALER_LEADER_SPREAD = 3.0f;
// The leader's spot counts as inside a cleave with the margins "dungeon avoid frontal cleave" leaves when it exits.
constexpr float ESCAPE_HEALER_CLEAVE_DIST_MARGIN = 1.0f;
constexpr float ESCAPE_HEALER_CLEAVE_ANGLE_MARGIN = 20.0f * static_cast<float>(M_PI) / 180.0f;
// Falric dispel order: Impending Despair on the healer, on anybody else, then Quivering Strike.
constexpr uint32 FALRIC_DISPEL_DESPAIR_HEALER = 3;
constexpr uint32 FALRIC_DISPEL_DESPAIR = 2;
constexpr uint32 FALRIC_DISPEL_QUIVERING_STRIKE = 1;
constexpr float EVENT_GATHER_RANGE = 25.0f;
constexpr float READY_MANA_PCT = 70.0f;
}  // namespace HoRIds

namespace HoRHelpers
{
InstanceScript* GetInstance(Player* bot);
Creature* GetInstanceCreature(Player* bot, uint32 dataId);
bool IsLichKingFightActive(Player* bot);
bool IsWaveEventActive(Player* bot);
bool IsFrontDoorOpen(Player* bot);
// Falric or Marwyn engaged, or called by their wave and walking in (the 8 s before SetInCombatWithZone).
bool IsShadowBossFight(Player* bot);
bool IsShadowAuraPaladin(PlayerbotAI* botAI);

// Where a bot stands during the spirit waves and how far it may stray from there.
struct WaveHold
{
    Position spot;
    float range = 0.0f;   // stay within this radius of spot; 0 = no hold (a boss is engaged, only the leash applies)
    float spread = 1.0f;  // MoveNear distance around spot
};
WaveHold GetWaveHold(Player* bot);
// The hold is active and the unit has not entered the hold area yet (keep waiting, do not chase it there).
bool IsOutsideWaveHold(Player* bot, Unit* unit);
// The waves run and no other member of the bot's group is alive inside them: the bot leaves so they reset.
bool IsWaveLost(Player* bot);

float EscapeBehindOffset(Player* bot, Creature* lichKing);
// The escape leader (Jaina 36955 / Sylvanas 37554) waiting with the gossip that starts the run, or nullptr.
Creature* GetWaitingEscapeLeader(Player* bot);

// The kill orders of "hor attack wave priority" (spirit waves) and "hor attack escape priority" (Lich King run).
enum class PriorityList : uint8
{
    Wave,
    Escape
};
// The kill order applies to the bot now (the trigger condition of its attack action).
bool IsPriorityListActive(Player* bot, PriorityList list);
// The alive, engageable attackers of the best rank of the kill order that is in the fight; empty: normal targeting.
// The attack action keeps any of them as its target, and the "dps target" exclusions keep only them.
std::vector<Unit*> GetPriorityRank(PlayerbotAI* botAI, PriorityList list);

bool IsEscapeSummon(uint32 entry);
// A Raging Ghoul, Risen Witch Doctor or Lumbering Abomination is in the fight.
bool EscapeSummonsUp(PlayerbotAI* botAI);
// The tank's pick-up spot between the leader and the Lich King; false outside the escape.
bool GetEscapeTankSpot(Player* bot, Position& spot);
// Not the tank, and as far back towards the Lich King as the escape tank (or farther): the summons would pick it.
bool IsBehindEscapeTank(Player* bot);
// An escape summon on a player who is not a tank (one on a healer first, then the nearest to the bot), or nullptr.
Unit* GetLooseEscapeMob(PlayerbotAI* botAI);
// Falric is engaged and a member carries Impending Despair (first) or Quivering Strike the bot can dispel.
Player* GetFalricDispelTarget(PlayerbotAI* botAI, std::string& spell);
Creature* GetFrostswornGeneral(Player* bot);  // alive and visible
// A chase from behind (the escape: the Lich King walks after the group): an escape step goes sideways or forward, to
// this anchor (the leader), never back along the trail towards him. False outside such a chase.
bool GetChaseForwardAnchor(Player* bot, Position& anchor, Unit*& chaser);
Unit* GetAttackableReflection(PlayerbotAI* botAI);
bool IsGroupGathered(Player* bot, float range);
}  // namespace HoRHelpers

class HoRStartIntroTrigger : public Trigger
{
public:
    HoRStartIntroTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor start intro", 2) {}
    bool IsActive() override;
};

class HoRWavePositionTrigger : public Trigger
{
public:
    HoRWavePositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor wave position") {}
    bool IsActive() override;
};

class HoRWaveLostTrigger : public Trigger
{
public:
    HoRWaveLostTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor wave lost") {}
    bool IsActive() override;
};

class HoRWavePriorityTrigger : public Trigger
{
public:
    HoRWavePriorityTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor wave priority") {}
    bool IsActive() override;
};

class HoRLichKingIntroTrigger : public Trigger
{
public:
    HoRLichKingIntroTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor lich king intro", 2) {}
    bool IsActive() override;
};

class HoREscapeGatherTrigger : public Trigger
{
public:
    HoREscapeGatherTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor escape gather", 2) {}
    bool IsActive() override;
};

class HoRStartEscapeTrigger : public Trigger
{
public:
    HoRStartEscapeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor start escape", 2) {}
    bool IsActive() override;
};

class HoREscapePositionTrigger : public Trigger
{
public:
    HoREscapePositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor escape position") {}
    bool IsActive() override;
};

class HoREscapePriorityTrigger : public Trigger
{
public:
    HoREscapePriorityTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor escape priority") {}
    bool IsActive() override;
};

class HoRLichKingTargetedTrigger : public Trigger
{
public:
    HoRLichKingTargetedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor lich king targeted") {}
    bool IsActive() override;
};

// Escape, tank: away from the pick-up spot (HoRHelpers::GetEscapeTankSpot) and no summon loose on the group.
class HoREscapeTankPositionTrigger : public Trigger
{
public:
    HoREscapeTankPositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor escape tank position") {}
    bool IsActive() override;
};

// Escape, tank: a summon is on a player who is not a tank.
class HoREscapeLooseMobTrigger : public Trigger
{
public:
    HoREscapeLooseMobTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor escape loose mob") {}
    bool IsActive() override;
};

// Escape, healer: summons are up and the healer strayed from the leader (outside any frontal cleave).
class HoREscapeHealerHoldTrigger : public Trigger
{
public:
    HoREscapeHealerHoldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor escape healer hold") {}
    bool IsActive() override;
};

// Falric (Defiling Horror 72435/72452: 2500/4000 shadow a second for 4 s on everybody, every 20 s) and Marwyn
// (Corrupted Flesh, Well of Corruption, Shared Suffering: shadow): the group's first paladin holds Shadow Resistance
// Aura instead of Devotion Aura.
class HoRShadowAuraTrigger : public Trigger
{
public:
    HoRShadowAuraTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor shadow aura") {}
    bool IsActive() override;
};

// Frostsworn General: the tank holds him near his home (he evades past 30 yd), the damage dealers kill the copies first.
class HoRGeneralLeashTrigger : public Trigger
{
public:
    HoRGeneralLeashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor general leash") {}
    bool IsActive() override;
};

class HoRReflectionTrigger : public Trigger
{
public:
    HoRReflectionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor spiritual reflection") {}
    bool IsActive() override;
};

class HoRFalricDispelTrigger : public Trigger
{
public:
    HoRFalricDispelTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hor falric dispel") {}
    bool IsActive() override;
};

#endif
