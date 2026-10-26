/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MECHSHARED_H
#define PLAYERBOTS_MECHSHARED_H

#include "Define.h"
#include "ObjectGuid.h"
#include <utility>
#include <vector>

class Player;
class Unit;
class Creature;

namespace MechanarFlames
{
    constexpr float INFERNO_RADIUS = 10.0f;
    // Fixated kiter keeps a wide running berth (INFERNO_SAFE_DIST). A non-fixated bot only
    // needs to clear the flame's actual fire footprint: its self-cast area aura (spell 35281,
    // radius 0 = self) drops trail patches (spell 35278, ~5yd) under it, so it is fine to fight
    // the boss from the front and only sidestep the patches (handled by the trail avoidance) plus
    // a small buffer for the instant a fresh patch lands.
    constexpr float INFERNO_AVOID_RANGE = 10.0f;
    constexpr float INFERNO_AVOID_CLEAR = 19.0f;
    constexpr float INFERNO_SAFE_DIST = 14.0f;
    constexpr float MELEE_FLAME_CLEAR = 12.0f;
    constexpr float KITE_THRESHOLD = 16.0f;
    constexpr float KITE_STEP = 14.0f;

    constexpr float KITE_TURN_RUNWAY = 16.0f;
    constexpr float KITE_TURN_THRESHOLD = 24.0f;
    constexpr float KITE_TURN_PASS_BEHIND = 20.0f;
    constexpr float KITE_TURN_WALL_INSET = 4.0f;
    constexpr float KITE_TURN_EXIT_PAST = 3.0f;
    constexpr float KITE_TURN_CLEAR_WEIGHT = 8.0f;
    constexpr float KITE_TURN_ABORT_CLEAR = -2.0f;
    constexpr uint32 KITE_TURN_FAILSAFE_MS = 12000;
    constexpr float KITE_GROUP_CLEARANCE = 16.0f;
    constexpr float HEALER_FIRE_BAIL_PCT = 50.0f;

    constexpr float HEALER_KITE_LEASH = 34.0f;
    constexpr float HEALER_KITE_LEASH_MAX = 38.0f;
    constexpr float HEALER_KITE_LEASH_WEIGHT = 3.0f;
    constexpr float HEALER_KITE_FLAME_PATH_WEIGHT = 2.5f;

    // A fixated tank kites like everyone else (standing still lets the flame sit on it and lay its
    // trail under the tank's feet), but while the boss is on someone else it stays close to her so
    // its taunts reach and she ends up back on the tank. While the tank holds her she simply follows.
    constexpr float TANK_KITE_LEASH = 10.0f;
    constexpr float TANK_KITE_LEASH_MAX = 14.0f;
    // Extra berth a tank holding the boss keeps from a flame: she stands a few yards in front of it,
    // and the melee around her must stay out of the 10yd Inferno too.
    constexpr float TANK_DRAG_EXTRA = 3.0f;

    constexpr uint32 TRAIL_PATCH_SPELL = 35278;
    constexpr float TRAIL_PATCH_RADIUS = 5.0f;
    constexpr float TRAIL_DANGER_MARGIN = 2.5f;
    constexpr float TRAIL_SCAN = 18.0f;
    constexpr float TRAIL_CLEAR_STEP = 10.0f;

    struct TrailPatch
    {
        float x;
        float y;
        float radius;
    };

    void CollectTrailPatches(Player* bot, float scanRadius, std::vector<TrailPatch>& out);

    bool InTrailDanger(Player* bot);

    bool InTrailDangerCached(Player* bot);

    bool IsFlameNearCached(Player* bot);

    bool BossFlameNearCached(Player* bot);

    // The fight area: the north part of the floor in front of her dais (x 285-308), from the north wall by the
    // Nethermancer door down to y 18. [enc-mech-south] The south end of the room holds trash the route leaves for after
    // her (creature.sql map 554): Tempest-Forge Destroyer 138862 at 293.9,-14.9 (random movement 6 yd, so up to
    // y -8.9, aggro reach 24 yd: y 15) and the Centurion/Astromage formation 138894-138897 at 272-274,-18..-29.
    // The old floor reached y -26: every kite / drag of the boss went into them (mech-n 2026-10-25 115805 12:18:32
    // tank at 288,-10.5 with 8 attackers - wipe, reset at 12:19:10; 132016 13:39:41 tank at 285,-7.6 with 7 attackers,
    // tank, healer and Rlshaag dead). Kiting stays north of their reach.
    constexpr float ROOM_ANCHOR_X = 296.5f;
    constexpr float ROOM_ANCHOR_Y = 36.0f;
    constexpr float ROOM_X_MIN = 285.0f;
    constexpr float ROOM_X_MAX = 308.0f;
    constexpr float ROOM_Y_MIN = 18.0f;
    constexpr float ROOM_Y_MAX = 54.0f;
    constexpr float KITE_LONG_AXIS_WEIGHT = 7.0f;

    // [b33_mech] Her Arcane Blast (35314, instant, on her victim every 15-25 s) is a knockback: Spell.dbc effect 98,
    // 35.0 horizontal / 7.5 vertical speed, about 27 yd flat, straight away from her. A tank holding her near the south
    // end of the room with her north of it lands in the trash the route leaves for after her (mech-n 2026-10-26 000434
    // 00:25:04-06: Rldrual 305.7,21.4 -> 303.5,-3.5 with her at 306,26, then Dragon's Breath, the Tempest-Forge
    // Destroyer joined and killed the tank and the DK - wipe; the same throw at 2026-10-25 115805 12:18:35 and 132016
    // 13:39:39). Only a throw to the south reaches trash (north is the wall by the door, east her dais), so the tank
    // picks spots whose throw lands north of the Destroyer's reach: her to the south or the side of it.
    constexpr float ARCANE_BLAST_THROW = 27.0f;
    constexpr float THROW_SAFE_Y = 15.0f;
    constexpr float THROW_WEIGHT = 3.0f;
    // A tank holding her turns its kite this far north of the room's south end, so the drag never brings her north of
    // it at the south end.
    constexpr float TANK_TURN_SOUTH_Y = 32.0f;

    // How many yards south of THROW_SAFE_Y her Arcane Blast would throw a tank standing at x,y (0 = safe).
    float ThrowOvershoot(Unit* boss, float x, float y);

    bool InRoom(float x, float y);

    bool SegmentInRoom(float ax, float ay, float bx, float by);

    void ClampIntoRoom(float& x, float& y);

    bool HealerHoldsFire(Player* bot);

    bool TankMustGrabBoss(Player* bot);

    bool TankHoldsBoss(Player* bot);

    bool BossNearFlame(Player* bot, float range);

    bool IsFlame(Unit* unit);

    Creature* GetNearestFlame(Player* bot, float radius);

    void CollectAvoidFlames(Player* bot, std::vector<std::pair<float, float>>& out);

    void CollectChasingFlames(Player* bot, std::vector<Creature*>& out);

    ObjectGuid FindFixatingFlameGuid(Player* bot);

    Unit* GetFixatingFlame(Player* bot);

    Unit* GetSepethrea(Player* bot);
}

class Action;
class PlayerbotAI;

// Mechano-Lord Capacitus (boss_mechano_lord_capacitus.cpp, Spell.dbc):
// - normal only: from 15 s every 20 s Reflective Damage Shield 35159 (10 s, SPELL_AURA_DAMAGE_SHIELD: 750 arcane to
//   every melee hit on him) and Reflective Magic Shield 35158 (10 s, SPELL_AURA_REFLECT_SPELLS 100 %), alternating,
//   the damage shield first;
// - heroic only: from 15 s every 30 s Polarity Shift 39096 puts Positive Charge 39088 or Negative Charge 39091 (60 s)
//   on every player; each charge pulses 39090 / 39093 every 5 s, 2000 nature to every other player within 10 yd
//   (radius index 13) who does not carry the same charge (spell_capacitus_polarity_charge), and stacks 39089 /
//   39092 (+damage) per same-charged player within it: the group splits into one stack per charge.
namespace MechanarCapacitus
{
    constexpr uint32 NPC_CAPACITUS = 19219;
    constexpr uint32 SPELL_REFLECTIVE_MAGIC_SHIELD = 35158;
    constexpr uint32 SPELL_REFLECTIVE_DAMAGE_SHIELD = 35159;
    constexpr uint32 SPELL_POSITIVE_CHARGE = 39088;
    constexpr uint32 SPELL_POSITIVE_CHARGE_PULSE = 39090;
    constexpr uint32 SPELL_NEGATIVE_CHARGE = 39091;

    // Capacitus alive and in combat near the bot, or nullptr.
    Unit* GetCapacitus(Player* bot);

    // +1 positive, -1 negative, 0 no charge.
    int32 Charge(Unit* unit);

    // The bot carries a charge and a member of the other charge stands within the pulse radius (+ margin).
    bool PolarityDanger(Player* bot);

    // Where the bot stands with its own charge: by the tank when it shares the tank's charge, else the stack beyond
    // the boss, out of the pulse radius of the tank's stack. False: no spot needed.
    bool PolaritySpot(Player* bot, float& x, float& y, float& z);

    // The bot carries the charge the tank does not: its stack stands beyond the boss, so no chase walks it back.
    bool OppositeOfTank(Player* bot);

    // A harmful spell action that the magic shield reflects onto the caster (DmgClass magic), aimed at him or AoE.
    bool ReflectedSpell(PlayerbotAI* botAI, Action* action, Unit* capacitus);

    // The bot casts a magic harmful spell at him right now.
    bool CastingIntoMagicShield(PlayerbotAI* botAI, Unit* capacitus);

    // A melee damage dealer who would take the damage shield's 750 per hit (the tank keeps him).
    bool MeleeHoldsForDamageShield(Player* bot, Unit* capacitus);
}

// Pathaleon the Calculator (boss_pathaleon_the_calculator.cpp): Greater Invisibility 34426 and CanAIAttack false until
// DATA_BRIDGE_MOB_DEATH_COUNT reaches 4; four spawns on the bridge before him tell him on death (smart_scripts
// -138820/-138869/-138879/-138893, action 223 DoAction 1): Sunseeker Astromage, Netherbinder, Engineer and Bloodwarder
// Physician, one formation (leader 138820, groupAI 3) at 134-141, 100-112, z 26.5, 40 yd short of him. Then he
// teleports in and DoZoneInCombat 25 s later. Nothing else starts him: the bridge pack is a required pull.
namespace MechanarBridge
{
    constexpr uint32 NPC_PATHALEON = 19220;
    constexpr uint32 SPELL_GREATER_INVISIBILITY = 34426;
    constexpr float PACK_X = 138.0f;
    constexpr float PACK_Y = 106.0f;
    constexpr float PACK_Z = 26.5f;

    // The alive members of the bridge pack (by spawn id).
    void CollectBridgePack(Player* bot, std::vector<Creature*>& out);

    // Pathaleon is still out of reach: invisible, or the bridge pack is not dead yet.
    bool PathaleonDormant(Player* bot);

    // The pulling tank's next bridge pull: the pack idle, the group gathered and out of combat, the bot near it and
    // no other idle mob near the pull spot. The target to pull, or nullptr.
    Creature* BridgePullTarget(PlayerbotAI* botAI);

    bool IsPullingTank(Player* bot);
}

class Transport;

// The lift between the Mechanar's two levels (GO 183788 Doodad_FactoryElevator01, GAMEOBJECT_TYPE_TRANSPORT, a
// StaticTransport). The mmaps hold no game objects, so no path joins the lower part (Mo'arg doors, 245,51,1.8) and
// the upper one (Nethermancer door, 268,52,27): the bot waits at its level's landing until the deck stands there,
// steps onto it, rides it as a passenger without moving and steps off at the other landing.
namespace MechanarLift
{
    constexpr uint32 MAP_ID = 554;
    constexpr uint32 GO_LIFT = 183788;

    // TransportAnimation.dbc 147460-147523 (entry 183788): at the bottom 0-3.70 s, rising until 6.67 s (25.44 yd),
    // at the top until 11.37 s, down at 15.23 s, period 15.33 s; data0 pauseAtTime 0, so it never waits.
    constexpr uint32 BOTTOM_LEAVES_MS = 3700;
    constexpr uint32 TOP_ARRIVES_MS = 6667;
    constexpr uint32 TOP_LEAVES_MS = 11367;
    constexpr uint32 BOTTOM_ARRIVES_MS = 15233;
    constexpr float RISE = 25.44f;

    // Factoryelevator.m2 (vmaps Factoryelevator.m2.vmo, GameObjectModels.dtree): the deck spans local x -264.7..-249.7,
    // y -59.8..-44.8 around the spawn origin (0.54,-1.39,0.21), turned by the spawn's 3.147: world x 250.3-265.2,
    // y 43.4-58.4. Its top face is local z 0.2.
    constexpr float DECK_LOCAL_X = -257.22f;
    constexpr float DECK_LOCAL_Y = -52.31f;
    constexpr float DECK_HALF = 7.0f;  // 7.47 yd to the edge, a bot's half width inside
    constexpr float DECK_TOP = 0.2f;
    constexpr float ON_DECK_DZ = 1.5f;

    // Landings (mmaps polygons): east of Mo'arg door 2 at the bottom, in front of the Nethermancer door at the top.
    constexpr float BOTTOM_LANDING_X = 247.0f;
    constexpr float BOTTOM_LANDING_Y = 50.8f;
    constexpr float BOTTOM_LANDING_Z = 1.8f;
    constexpr float TOP_LANDING_X = 269.3f;
    constexpr float TOP_LANDING_Y = 52.3f;
    constexpr float TOP_LANDING_Z = 26.8f;

    // A goal (the harness waypoint, the group leader) this close to the shaft on the other level is reached by lift.
    constexpr float GOAL_RANGE = 40.0f;
    constexpr float LEADER_RANGE = 70.0f;
    constexpr float APPROACH_RANGE = 40.0f;
    // [w24b-mech-landing-walk] A bot whose own walk ends at the landing of its level stays on its leg out to this
    // range: the lower level's path to the bottom landing swings out of APPROACH_RANGE (mech-n 2026-10-25 115805:
    // Rldruaf from 244.3,21.3 out to 215.9,12.7, its follow then walked it back - lift / follow ping-pong, 2 minutes).
    constexpr float LANDING_WALK_RANGE = 120.0f;
    constexpr float LANDING_WALK_END_RANGE = 3.0f;
    // Onto the deck only while it stays long enough for the walk from the landing (up to 13 yd, under 2 s).
    constexpr uint32 BOARD_MIN_LEFT_MS = 2300;
    constexpr float BOARD_FROM_RANGE = 6.0f;
    constexpr float LANDING_WAIT_RANGE = 2.5f;

    enum class Level : uint8
    {
        Bottom,
        Top
    };

    struct Leg
    {
        Transport* lift = nullptr;
        Level here = Level::Bottom;  // the bot's level (for a rider: the deck's next stop)
        Level goal = Level::Bottom;  // where it goes
        bool riding = false;         // a passenger of the lift
        bool onDeck = false;         // stands on the deck (a passenger or not)
    };

    Transport* FindLift(Player* bot);

    void DeckCenter(Transport* lift, float& x, float& y);

    float DeckZ(Transport* lift);

    bool OnDeck(Player* bot, Transport* lift);

    // How long the deck stays at that level from now on (ms), 0 while it is not standing there.
    uint32 StopLeftMs(Transport* lift, Level level);

    void Landing(Level level, float& x, float& y, float& z);

    // The bot's lift leg: it rides or stands on the deck, or its goal is on the other level and it is near the shaft
    // (out of combat). False: the lift is none of its business now.
    bool ActiveLeg(Player* bot, Leg& leg);
}

#endif
