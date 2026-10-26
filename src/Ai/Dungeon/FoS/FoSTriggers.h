/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOSTRIGGERS_H
#define PLAYERBOTS_FOSTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Position.h"
#include "Trigger.h"

enum ForgeOfSoulsBronjahmIDs
{
    // Bronjahm
    NPC_CORRUPTED_SOUL_FRAGMENT = 36535,

    SPELL_CORRUPT_SOUL          = 68839,
    SPELL_SOULSTORM_VISUAL      = 68870,
    SPELL_SOULSTORM_VISUAL2     = 68904,
    SPELL_SOULSTORM             = 68872,

    // Devourer of Souls
    SPELL_WAILING_SOULS         = 68899,
};

namespace FoSIds
{
// boss_devourer_of_souls.cpp
constexpr uint32 SPELL_MIRRORED_SOUL_CHANNEL = 69051;  // dummy aura on the Devourer while it channels
constexpr uint32 SPELL_MIRRORED_SOUL_PROC = 69023;     // spell_gen_mirrored_soul: 45% of the damage taken is copied
constexpr uint32 SPELL_WAILING_SOULS_PERIODIC = 68875;
constexpr uint32 SPELL_WAILING_SOULS_PERIODIC_2 = 68876;
constexpr uint32 SPELL_PHANTOM_BLAST = 68982;
constexpr uint32 SPELL_PHANTOM_BLAST_H = 70322;
// Wailing Souls: 68912 picks a random player, the boss faces it, roots and casts 68899 (3 s); then the 68875
// channel ticks every 500 ms. Ticks 1..29 turn the boss 3 degrees in one random direction and fire the beam
// (68873 normal: 5 degree cone per spell_cone; 70324 heroic: no spell_cone row, so the core uses the 104 degree
// default of TARGET_UNIT_CONE_ENEMY_104). Ticks 30..33 only wait, tick 33 unroots the boss.
constexpr uint32 SPELL_WAILING_SOULS_DMG_N = 68873;
constexpr uint32 SPELL_WAILING_SOULS_DMG_H = 70324;
constexpr float WAILING_SOULS_DEFAULT_CONE_DEGREES = 104.0f;
constexpr uint32 WAILING_SOULS_LAST_BEAM_TICK = 29;
constexpr float WAILING_SOULS_TURN_PER_TICK = 0.05235988f;  // 2 * pi / 120
constexpr float WAILING_SOULS_SAFETY_MARGIN = 0.17f;        // ~10 degrees: one more tick plus the reaction delay
// Spell.cpp lands a cone on every unit within the caster's boundary whatever the angle: MIN_MELEE_REACH 2 yd plus
// both bodies (the Devourer's combat reach is 6.4, a player's 1.5), so within ~9.9 yd of his centre. Wave 9: a mage
// running round him at 1.5 yd took four beams in a row. This is the part beyond his own combat reach, with a yard of
// margin.
constexpr float WAILING_SOULS_BOUNDARY_EXTRA = 4.5f;
constexpr float CORRUPT_SOUL_FLEE_DISTANCE = 25.0f;
// boss_bronjahm.cpp: below 35% Bronjahm roots itself and casts Teleport (0.6 s) to the room centre, then Soulstorm
// (4 s cast, 10 min aura on itself). The aura fires 68921 (heroic 69049) every 500 ms at every enemy, and
// spell_bronjahm_soulstorm_targeting drops the targets within 10 yd (2D, centre to centre): ~1000 damage and a
// 50% slow per tick outside, nothing inside. Fear (3 s, +70% speed) keeps hitting one player within 10 yd.
constexpr uint32 SPELL_BRONJAHM_TELEPORT = 68988;
constexpr float SOULSTORM_CENTRE_X = 5297.33f;  // spell_target_position of 68988
constexpr float SOULSTORM_CENTRE_Y = 2506.3f;
constexpr float SOULSTORM_CENTRE_Z = 686.069f;
// Melee and the tank in reach of the boss; ranged and healers past the ~5.75 yd at which their "enemy too close"
// reactions start, all well inside the 10 yd eye.
constexpr float SOULSTORM_MELEE_STACK = 3.0f;
constexpr float SOULSTORM_MELEE_MAX = 4.5f;
constexpr float SOULSTORM_RANGED_STACK = 7.0f;
constexpr float SOULSTORM_RANGED_MIN = 6.0f;
constexpr float SOULSTORM_RANGED_MAX = 8.0f;
}  // namespace FoSIds

class MoveFromBronjahmTrigger : public Trigger
{
public:
    MoveFromBronjahmTrigger(PlayerbotAI* botAI) : Trigger(botAI, "move from bronjahm") {}

    bool IsActive() override;
};

class SwitchToSoulFragment : public Trigger
{
public:
    SwitchToSoulFragment(PlayerbotAI* botAI) : Trigger(botAI, "switch to soul fragment") {}

    bool IsActive() override;
};

class BronjahmPositionTrigger : public Trigger
{
public:
    BronjahmPositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bronjahm position") {}
    bool IsActive() override;
};

// Mirrored Soul is up and the bot (or its pet) still damages the Devourer.
class DevourerOfSoulsTrigger : public Trigger
{
public:
    DevourerOfSoulsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "devourer of souls") {}
    bool IsActive() override;
};

// Wailing Souls is being cast or channelled and the rest of the sweep can still reach the bot.
class WailingSoulsTrigger : public Trigger
{
public:
    WailingSoulsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fos wailing souls") {}
    bool IsActive() override;
};

class FoSInterruptTrigger : public Trigger
{
public:
    FoSInterruptTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fos interrupt", 250) {}
    bool IsActive() override;
};

namespace FoSShared
{
// The Devourer while it mirrors damage onto a player, else nullptr.
Unit* GetMirroringDevourer(PlayerbotAI* botAI);
// The Devourer while Wailing Souls is cast or its beam still has ticks to fire, else nullptr.
Unit* GetWailingDevourer(PlayerbotAI* botAI);
// Angle from the Devourer's current facing that the rest of the sweep can still hit, whichever way it turns.
float GetWailingDangerAngle(Unit* boss);
// The point (x, y) stays outside every remaining beam of the sweep.
bool IsSafeFromWailing(Unit* boss, float x, float y, float dangerAngle);
// The Devourer casting Phantom Blast, else nullptr.
Unit* GetPhantomBlastCaster(PlayerbotAI* botAI);
// Bronjahm teleports to the room centre for Soulstorm, casts it or storms (all on its cast bar / auras).
bool IsSoulstorm(Unit* boss);
// The eye of the storm: the teleport destination while Bronjahm still casts it, then Bronjahm itself.
Position GetSoulstormCentre(Unit* boss);
}  // namespace FoSShared

#endif
