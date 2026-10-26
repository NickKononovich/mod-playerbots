/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MgTShared.h"
#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "AttackersValue.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DungeonInteract.h"
#include "EncounterHelpers.h"
#include "Group.h"
#include "Map.h"
#include "PathGenerator.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "RtiTargetValue.h"
#include "RtiValue.h"  // [b34_mgt] IsCrowdControlBackedOff
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TargetValue.h"
#include "Timer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

using namespace EncounterHelpers;

namespace MagistersTerrace
{
namespace
{
enum MgTNpcs : uint32
{
    NPC_SELIN_FIREHEART         = 24723,
    NPC_FEL_CRYSTAL             = 24722,

    NPC_SUNBLADE_MAGE_GUARD     = 24683,
    NPC_SUNBLADE_BLOOD_KNIGHT   = 24684,
    NPC_SUNBLADE_MAGISTER       = 24685,
    NPC_SUNBLADE_WARLOCK        = 24686,
    NPC_SUNBLADE_PHYSICIAN      = 24687,
    NPC_SUNBLADE_IMP            = 24815,

    NPC_WRETCHED_SKULKER        = 24688,
    NPC_WRETCHED_BRUISER        = 24689,
    NPC_WRETCHED_HUSK           = 24690,

    NPC_COILSKAR_WITCH          = 24696,
    NPC_SISTER_OF_TORMENT       = 24697,
    NPC_ETHEREUM_SMUGGLER       = 24698,
};

enum MgTSpells : uint32
{
    SPELL_MAGIC_DAMPENING_FIELD = 44475,

    SPELL_HOLY_LIGHT            = 44479,
    SPELL_HOLY_LIGHT_H          = 46029,

    SPELL_ARCANE_NOVA           = 44644,
    SPELL_ARCANE_NOVA_H         = 46036,

    SPELL_INCINERATE            = 44519,
    SPELL_INCINERATE_H          = 46043,
    SPELL_FROSTBOLT             = 44606,
    SPELL_FROSTBOLT_H           = 46035,

    SPELL_WRETCHED_FIREBALL     = 44503,
    SPELL_WRETCHED_FROSTBOLT    = 44504,

    SPELL_DEADLY_EMBRACE        = 44547,

    SPELL_FORKED_LIGHTNING      = 20299,
    SPELL_FORKED_LIGHTNING_H    = 46150,

    SPELL_DRINK_FEL_INFUSION    = 44505,

    // Selin Fireheart (boss_selin_fireheart.cpp): channelled, interruptible (Spell.dbc InterruptFlags 0xd / 0xf).
    // Drain Life on a random target every 10 s heals her; Drain Mana (heroic) on a mana user fills her for the
    // crystal-less Fel Explosion phase.
    SPELL_SELIN_DRAIN_LIFE      = 44294,
    SPELL_SELIN_DRAIN_MANA      = 46153,
};

enum MgTDelrissaNpcs : uint32
{
    NPC_DELRISSA                = 24560,

    NPC_APOKO                   = 24553,
    NPC_ELLRYS_DUSKHALLOW       = 24558,
    NPC_YAZZAI                  = 24561,
    NPC_ZELFAN                  = 24556,
    NPC_GARAXXAS                = 24555,
    NPC_KAGANI_NIGHTSTRIKE      = 24557,
    NPC_WARLORD_SALARIS         = 24559,
    NPC_ERAMAS_BRIGHTBLAZE      = 24554,

    NPC_HIGH_EXPLOSIVE_SHEEP    = 24715,

    NPC_FIZZLE                  = 24656,
    NPC_SLIVER                  = 24552,
};

enum MgTDelrissaSpells : uint32
{
    SPELL_FLASH_HEAL            = 17843,
    SPELL_LESSER_HEALING_WAVE   = 44256,
    SPELL_SUMMON_IMP            = 44163,
    SPELL_DELRISSA_FEAR         = 38595,
    SPELL_POLYMORPH             = 13323,

    SPELL_TREMOR_TOTEM          = 8143,
};

enum MgTKaelNpcs : uint32
{
    NPC_KAEL_THAS               = 24664,
    NPC_FLAMESTRIKE_TRIGGER     = 24666,
    NPC_PHOENIX                 = 24674,
    NPC_PHOENIX_EGG             = 24675,
    NPC_ARCANE_SPHERE           = 24708,
};

enum MgTKaelSpells : uint32
{
    SPELL_FLAME_STRIKE          = 44190,
    SPELL_FLAME_STRIKE_H        = 46163,

    SPELL_PHOENIX_BURN          = 44197,

    SPELL_SHOCK_BARRIER         = 46165,
    SPELL_PYROBLAST             = 36819,

    SPELL_KAEL_FIREBALL         = 44189,

    SPELL_GRAVITY_LAPSE_INITIAL = 44224,
    SPELL_GRAVITY_LAPSE_DOT     = 44226,
    SPELL_GRAVITY_LAPSE_FLY     = 44227,
    SPELL_POWER_FEEDBACK        = 44233,
};

constexpr char const* SELIN_NAME = "selin fireheart";
constexpr char const* KAEL_NAME  = "kael'thas sunstrider";

constexpr float ROOM_X_MIN = 218.0f;
constexpr float ROOM_X_MAX = 260.0f;
constexpr float ROOM_Y_MIN = -40.0f;
constexpr float ROOM_Y_MAX = 40.0f;

constexpr float CRYSTAL_SCAN_RANGE = 60.0f;

constexpr float DAMPENING_RADIUS = 5.0f;
constexpr float DAMPENING_STAND  = DAMPENING_RADIUS + 1.5f;
constexpr float DAMPENING_CLEAR  = DAMPENING_RADIUS + 4.0f;
constexpr float DAMPENING_SCAN   = 30.0f;

constexpr float TANK_REJOIN_GAP = 5.0f;

constexpr float GLAIVE_MIN_RANGE = 12.0f;
constexpr float GLAIVE_MAX_RANGE = 60.0f;

constexpr float NOVA_CLEAR = 16.0f;

constexpr float INTERRUPT_SCAN = 30.0f;
constexpr float WRETCHED_SCAN  = 40.0f;
constexpr float FOCUS_SCAN     = 40.0f;

constexpr float ESCAPE_LOS_HEIGHT = 2.0f;

constexpr float PARTY_CENTROID_RANGE = 60.0f;

constexpr float ESCAPE_RADII[] = { 7.0f, 10.0f, 13.0f, 16.0f };
constexpr uint8 ESCAPE_BEARINGS = 12;
constexpr float ESCAPE_TRAVEL_WEIGHT = 0.25f;
constexpr uint8 ESCAPE_KEEP = 5;

constexpr float FLAMESTRIKE_STAND = 7.5f;
constexpr float FLAMESTRIKE_CLEAR = 10.0f;
constexpr float FLAMESTRIKE_SCAN  = 40.0f;

constexpr float BURN_DAMAGE  = 8.0f;
constexpr float BURN_STAND   = 9.0f;
constexpr float BURN_CLEAR   = 12.0f;
constexpr float PHOENIX_SCAN = 40.0f;

constexpr float BURN_MELEE_STAND = BURN_DAMAGE + 0.5f;
constexpr float BURN_MELEE_CLEAR = BURN_DAMAGE + 2.5f;
constexpr float BURN_MELEE_RADII[] = { 3.0f, 5.0f, 7.0f, 9.0f, 12.0f };
constexpr uint8 BURN_MELEE_BEARINGS = 16;

constexpr float KAEL_MELEE_RING = 5.0f;

constexpr LapseSpot KAEL_P2      = { 148.54f, 181.13f };

constexpr LapseSpot LAPSE_TANK   = { 148.54f, 177.53f };
constexpr LapseSpot LAPSE_MELEE[] = {
    { 151.30f, 178.82f },
    { 145.78f, 178.82f },
};

constexpr LapseSpot LAPSE_HEALER = { 148.54f, 167.13f };
constexpr LapseSpot LAPSE_RANGED[] = {
    { 137.07f, 164.75f },
    { 148.54f, 155.13f },
    { 164.92f, 169.66f },
};

constexpr float SPHERE_DAMAGE    = 5.0f;
constexpr float SPHERE_CLEAR     = 8.0f;
constexpr float SPHERE_SCAN      = 40.0f;

constexpr float LAPSE_PUSH_MAX   = 8.0f;

constexpr float SPHERE_SPEED     = 3.0f;
constexpr float SPHERE_LEAD      = 2.5f;

constexpr float SPHERE_ALERT     = 9.0f;
constexpr float SPHERE_TRANSIT   = 6.0f;
constexpr float SPHERE_ROUTE_SLACK = 0.5f;

constexpr float SPHERE_DODGE_NEAR_RADIUS = 4.0f;
constexpr float SPHERE_DODGE_RADII[]     = { SPHERE_DODGE_NEAR_RADIUS, 7.0f, 11.0f, 15.0f };
constexpr uint8 SPHERE_DODGE_BEARINGS    = 16;
constexpr float SPHERE_DODGE_TRAVEL_COST = 0.25f;
constexpr float SPHERE_DODGE_MARGIN_CAP    = 6.0f;
constexpr float SPHERE_DODGE_MARGIN_WEIGHT = 0.5f;
constexpr float SPHERE_DODGE_MIN_GAIN      = 2.0f;

constexpr float KITE_LEG          = 12.0f;
constexpr float KITE_TURN_MAGNITUDES[] = { 0.0f, 25.0f, 50.0f, 75.0f,
                                           100.0f, 125.0f, 150.0f, 175.0f };

constexpr float KITE_PROBE_STEP   = 2.0f;
constexpr float KITE_PROBE_MAX    = 24.0f;

constexpr float KITE_PASS_CLEAR   = SPHERE_DAMAGE;
constexpr float KITE_RUNWAY_TIEBREAK = 0.5f;
constexpr float KITE_COMMIT_BONUS = 3.0f;
constexpr float KITE_COMMIT_STRAIGHT = 10.0f;

constexpr uint32 KITE_COMMIT_BREAK_MS = 1500;

constexpr float KITE_NORTH_MIN    = 340.0f;
constexpr float KITE_NORTH_MAX    = 20.0f;
constexpr float KITE_BOWL_MIN     = 100.0f;
constexpr float KITE_BOWL_MAX     = 260.0f;
constexpr float KITE_BOWL_RADIUS  = 22.0f;
constexpr float KITE_SIDE_RADIUS  = 13.0f;

constexpr float LAPSE_TOLERANCE  = 3.0f;

constexpr float LAPSE_BURN_COST = 6.0f;

constexpr float LAPSE_LEG          = 10.0f;
constexpr float LAPSE_PROGRESS_MIN = 1.0f;
constexpr float LAPSE_DETOUR_TURNS[] = { 0.0f, 20.0f, 40.0f, 60.0f, 80.0f, 100.0f };

struct Hazard
{
    float x;
    float y;
    float stand;
    float clear;
};

struct ScoredSpot
{
    float score;
    float x;
    float y;
};

void CollectCreaturesByEntry(Player* bot, std::vector<uint32> const& entries, float radius,
                             std::vector<Unit*>& out)
{
    std::list<Creature*> found;
    bot->GetCreatureListWithEntryInGrid(found, entries, radius);

    out.reserve(out.size() + found.size());
    for (Creature* creature : found)
    {
        if (creature->IsAlive())
            out.push_back(creature);
    }
}

void CollectAttackersByEntry(Player* bot, std::vector<uint32> const& entries, float radius,
                             std::vector<Unit*>& out)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

    auto const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* attacker = botAI->GetUnit(guid);
        if (!attacker)
            continue;

        if (std::find(entries.begin(), entries.end(), attacker->GetEntry()) == entries.end())
            continue;

        if (bot->GetExactDist2d(attacker) > radius)
            continue;

        out.push_back(attacker);
    }
}

bool PartyCentroid(Player* bot, float& cx, float& cy)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    float sx = 0.0f, sy = 0.0f;
    int n = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
            continue;
        if (bot->GetExactDist2d(member) > PARTY_CENTROID_RANGE)
            continue;

        sx += member->GetPositionX();
        sy += member->GetPositionY();
        ++n;
    }

    if (n == 0)
        return false;

    cx = sx / n;
    cy = sy / n;
    return true;
}

bool SpotClearsHazards(std::vector<Hazard> const& hazards, float x, float y)
{
    for (Hazard const& hazard : hazards)
    {
        float const dx = x - hazard.x;
        float const dy = y - hazard.y;
        if (dx * dx + dy * dy < hazard.clear * hazard.clear)
            return false;
    }

    return true;
}

bool InsideAnyHazard(std::vector<Hazard> const& hazards, float x, float y)
{
    for (Hazard const& hazard : hazards)
    {
        float const dx = x - hazard.x;
        float const dy = y - hazard.y;
        if (dx * dx + dy * dy < hazard.stand * hazard.stand)
            return true;
    }

    return false;
}

void KeepBestReachable(Player* bot, std::vector<ScoredSpot>& scored, EscapeSpots& out)
{
    std::stable_sort(scored.begin(), scored.end(),
                     [](ScoredSpot const& a, ScoredSpot const& b) { return a.score < b.score; });

    float const bz = bot->GetPositionZ();

    uint8 kept = 0;
    for (ScoredSpot const& spot : scored)
    {
        float const z = bot->GetMapHeight(spot.x, spot.y, bz + GROUND_SEARCH_UP, true, GROUND_SEARCH_DOWN);
        if (!(std::fabs(z - bz) <= GROUND_TIER_STEP))
            continue;

        if (!bot->IsWithinLOS(spot.x, spot.y, z + ESCAPE_LOS_HEIGHT))
            continue;

        out.push_back({ spot.x, spot.y, z });

        if (++kept >= ESCAPE_KEEP)
            break;
    }
}

void CollectHazardEscapes(Player* bot, std::vector<Hazard> const& hazards, float anchorGap,
                          EscapeSpots& out)
{
    if (hazards.empty())
        return;

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();

    if (!InsideAnyHazard(hazards, bx, by))
        return;

    float cx = 0.0f, cy = 0.0f;
    bool const haveParty = PartyCentroid(bot, cx, cy);

    std::vector<ScoredSpot> scored;
    for (float radius : ESCAPE_RADII)
    {
        for (int i = 0; i < ESCAPE_BEARINGS; ++i)
        {
            float const angle = (2.0f * static_cast<float>(M_PI) * i) / ESCAPE_BEARINGS;
            float const x = bx + std::cos(angle) * radius;
            float const y = by + std::sin(angle) * radius;
            if (!SpotClearsHazards(hazards, x, y))
                continue;

            float score = radius * ESCAPE_TRAVEL_WEIGHT;
            if (haveParty)
                score += std::fabs(std::hypot(x - cx, y - cy) - anchorGap);

            scored.push_back({ score, x, y });
        }
    }

    KeepBestReachable(bot, scored, out);
}

void CollectRingEscapes(Player* bot, Unit* anchor, std::vector<Hazard> const& hazards, EscapeSpots& out)
{
    if (hazards.empty())
        return;

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();

    if (!InsideAnyHazard(hazards, bx, by))
        return;

    float const ax = anchor->GetPositionX();
    float const ay = anchor->GetPositionY();

    std::vector<ScoredSpot> scored;
    for (float radius : BURN_MELEE_RADII)
    {
        for (int i = 0; i < BURN_MELEE_BEARINGS; ++i)
        {
            float const angle = (2.0f * static_cast<float>(M_PI) * i) / BURN_MELEE_BEARINGS;
            float const x = bx + std::cos(angle) * radius;
            float const y = by + std::sin(angle) * radius;
            if (!SpotClearsHazards(hazards, x, y))
                continue;

            float const score = std::hypot(x - ax, y - ay) + radius * ESCAPE_TRAVEL_WEIGHT;
            scored.push_back({ score, x, y });
        }
    }

    KeepBestReachable(bot, scored, out);
}

uint8 InterruptPriority(uint32 spellId)
{
    switch (spellId)
    {
        case SPELL_DEADLY_EMBRACE:
            return 1;
        case SPELL_HOLY_LIGHT:
        case SPELL_HOLY_LIGHT_H:
            return 2;
        // A boss: kicks only, no stuns (CONTROL_INTERRUPT_URGENCY).
        case SPELL_SELIN_DRAIN_LIFE:
        case SPELL_SELIN_DRAIN_MANA:
        case SPELL_ARCANE_NOVA:
        case SPELL_ARCANE_NOVA_H:
            return 3;
        case SPELL_FORKED_LIGHTNING:
        case SPELL_FORKED_LIGHTNING_H:
            return 4;
        case SPELL_INCINERATE:
        case SPELL_INCINERATE_H:
            return 5;
        case SPELL_FROSTBOLT:
        case SPELL_FROSTBOLT_H:
            return 6;
        case SPELL_WRETCHED_FIREBALL:
        case SPELL_WRETCHED_FROSTBOLT:
            return 7;
        default:
            return 0;
    }
}

uint8 CurrentCastPriority(Unit* caster)
{
    uint8 best = 0;
    for (CurrentSpellTypes slot : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
    {
        Spell* spell = caster->GetCurrentSpell(slot);
        if (!spell || !spell->m_spellInfo)
            continue;

        uint8 const prio = InterruptPriority(spell->m_spellInfo->Id);
        if (prio && (!best || prio < best))
            best = prio;
    }

    return best;
}

bool StopsACast(uint32 spellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return false;

    if ((info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) &&
        info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
        return true;

    for (uint8 i = EFFECT_0; i <= EFFECT_2; ++i)
    {
        if (info->Effects[i].Effect == SPELL_EFFECT_INTERRUPT_CAST)
            return true;

        if (info->Effects[i].Effect != SPELL_EFFECT_APPLY_AURA)
            continue;

        if (info->Effects[i].ApplyAuraName == SPELL_AURA_MOD_SILENCE ||
            info->Effects[i].ApplyAuraName == SPELL_AURA_MOD_STUN)
            return true;
    }

    return false;
}

enum class InterruptKind
{
    Dedicated,
    Control,
};

struct InterruptSpell
{
    char const* name;
    InterruptKind kind;
};

std::vector<InterruptSpell> const& InterruptSpells()
{
    static std::vector<InterruptSpell> const spells = {
        { "kick", InterruptKind::Dedicated },
        { "pummel", InterruptKind::Dedicated },
        { "shield bash", InterruptKind::Dedicated },
        { "wind shear", InterruptKind::Dedicated },
        { "mind freeze", InterruptKind::Dedicated },
        { "counterspell", InterruptKind::Dedicated },
        { "spell lock", InterruptKind::Dedicated },
        { "silencing shot", InterruptKind::Dedicated },
        { "shockwave", InterruptKind::Control },
        { "shadowfury", InterruptKind::Control },
        { "concussion blow", InterruptKind::Control },
        { "hammer of justice", InterruptKind::Control },
        { "maim", InterruptKind::Control },
        { "kidney shot", InterruptKind::Control },
        { "silence", InterruptKind::Control },
        { "bash", InterruptKind::Control },
        { "strangulate", InterruptKind::Control },
        { "arcane torrent", InterruptKind::Control },
    };

    return spells;
}

uint8 TrashFocusRank(uint32 entry)
{
    switch (entry)
    {
        case NPC_SUNBLADE_PHYSICIAN:
            return 1;
        case NPC_SUNBLADE_BLOOD_KNIGHT:
            return 2;
        case NPC_SISTER_OF_TORMENT:
            return 3;
        case NPC_SUNBLADE_WARLOCK:
            return 4;
        case NPC_SUNBLADE_MAGISTER:
            return 5;
        case NPC_ETHEREUM_SMUGGLER:
            return 6;
        case NPC_COILSKAR_WITCH:
            return 7;
        case NPC_SUNBLADE_IMP:
            return 8;
        case NPC_SUNBLADE_MAGE_GUARD:
            return 9;
        default:
            return 0;
    }
}

// The healers die first: Delrissa (Flash Heal, Renew, Power Word: Shield, and Dispel Magic on her own retinue,
// which strips the group's crowd control) and then Apoko. Her retinue is taunt immune and re-picks its victims
// every 8-10 s by distance, health and armour, so the free melee lackeys come next: in wave 8 Kagani, Eramas and
// Salaris killed the group one by one while the dps sat on Yazzai. Two of them are crowd controlled where the
// group can (see the CC order below), the casters come last.
// [b34_mgt] Kagani before Delrissa while he is free (a crowd-controlled lackey is never the focus, PickRankedFocus):
// the tank cannot hold him at all - RecalculateThreat every 8-10 s sets the retinue's threat to 1e7 x distance x
// armour, so he always kills the lowest-armour member within 5 yd, and Vanish every 30 s moves him to a random one.
// mgt-h 2026-10-26 (20261026-014521-mgt-h-5B-16): no CC landed, the dps sat on Delrissa (76 -> 16 % in 85 s) while
// Kagani (31 % from splash alone) killed the shaman, the rogue, the healer and the priest; he killed most of the
// members in every failed Delrissa attempt (5A-15: 6 of 8 deaths). Delrissa's heals are interrupted meanwhile.
// [b44_mgt] Delrissa last, the player kill order: the sheep, Apoko (the lackey healer), every free melee lackey,
// the casters (Ellrys' Fear, Zelfan's Recombobulate that strips our Polymorph, Yazzai, Garaxxas), then Delrissa.
// She is the slowest kill of the room (855k on heroic, Power Word: Shield every 10 s, Renew, Flash Heal) and hits
// for nothing (Shadow Word: Pain); her threat table is a normal one, so "[threat-hold]" paced the dps on her while
// the tank's threat was spread over five targets. mgt-h 20261026-075135-mgt-h-5A-14: the whole group sat on her for
// 55 s ("[boss focus] ... single target on the boss", "[threat-hold] ... on Priestess Delrissa 129 %", the dps opened
// 17.5 s into the pull) while Eramas Brightblaze and Warlord Salaris, both free, killed the shaman (1408 s) and the
// mage twice (1426, 1437). Normal 5B-13 (same build) killed her and the lackeys in 43 s with no CC: on heroic the
// melee lackeys' damage, not her heals, decides the fight.
uint8 RetinueFocusRank(uint32 entry)
{
    switch (entry)
    {
        case NPC_HIGH_EXPLOSIVE_SHEEP:
            return 1;
        case NPC_APOKO:
            return 2;
        case NPC_KAGANI_NIGHTSTRIKE:
            return 3;
        case NPC_ERAMAS_BRIGHTBLAZE:
            return 4;
        case NPC_WARLORD_SALARIS:
            return 5;
        case NPC_ELLRYS_DUSKHALLOW:
            return 6;
        case NPC_ZELFAN:
            return 7;
        case NPC_YAZZAI:
            return 8;
        case NPC_GARAXXAS:
            return 9;
        case NPC_DELRISSA:
            return 10;
        default:
            return 0;
    }
}

// Which retinue member a crowd control bot takes first (0 = never): the melee lackeys that kill a group, then the
// casters. Delrissa is immune to it, Apoko is a kill target.
// Danger order from the AC script and the deaths (deaths.jsonl 2026-10-23..25, Magisters' Terrace heroic: Kagani
// Nightstrike killed 14 members, Eramas Brightblaze 9, Warlord Salaris 6): Kagani's Vanish every 30 s resets his
// threat onto a random member, then Kidney Shot, Gouge, Backstab and Eviscerate; Eramas' Knockdown and Snap Kick;
// Salaris' Mortal Strike, Intercept and Frightening Shout; then Garaxxas' shots and the casters.
uint8 RetinueCcRank(uint32 entry)
{
    switch (entry)
    {
        case NPC_KAGANI_NIGHTSTRIKE:
            return 1;
        case NPC_ERAMAS_BRIGHTBLAZE:
            return 2;
        case NPC_WARLORD_SALARIS:
            return 3;
        case NPC_GARAXXAS:
            return 4;
        case NPC_YAZZAI:
            return 5;
        case NPC_ELLRYS_DUSKHALLOW:
            return 6;
        case NPC_ZELFAN:
            return 7;
        default:
            return 0;
    }
}

// Held by deliberate crowd control or carrying a crowd control icon (anything but Skull and Cross).
bool IsCrowdControlTarget(Player* bot, Unit* unit)
{
    if (IsCrowdControlled(unit))
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (uint8 icon = 0; icon < TARGETICONCOUNT; ++icon)
    {
        if (icon == RtiTargetValue::skullIndex || icon == RtiTargetValue::crossIndex)
            continue;

        if (group->GetTargetIcon(icon) == unit->GetGUID())
            return true;
    }

    return false;
}

Unit* PickRankedFocus(Player* bot, uint8 (*rank)(uint32), std::vector<Unit*> const& extra,
                      ObjectGuid& latched)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

    Unit* held = nullptr;
    uint8 heldRank = 0;
    Unit* best = nullptr;
    uint8 bestRank = 0;
    float bestDist = 0.0f;

    auto consider = [&](Unit* candidate)
    {
        uint8 const candidateRank = rank(candidate->GetEntry());
        if (!candidateRank)
            return;

        // Never the mob someone holds in crowd control: hitting it breaks the CC.
        if (IsCrowdControlTarget(bot, candidate))
            return;

        float const dist = bot->GetExactDist2d(candidate);
        if (dist > FOCUS_SCAN)
            return;

        if (!latched.IsEmpty() && candidate->GetGUID() == latched)
        {
            held = candidate;
            heldRank = candidateRank;
        }

        if (!best || candidateRank < bestRank || (candidateRank == bestRank && dist < bestDist))
        {
            best = candidate;
            bestRank = candidateRank;
            bestDist = dist;
        }
    };

    auto const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        if (Unit* attacker = botAI->GetUnit(guid))
            consider(attacker);
    }

    for (Unit* candidate : extra)
        consider(candidate);

    if (held && heldRank <= bestRank)
        return held;

    latched = best ? best->GetGUID() : ObjectGuid::Empty;
    return best;
}

bool RetinueEngaged(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

    auto const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* attacker = botAI->GetUnit(guid);
        if (attacker && RetinueFocusRank(attacker->GetEntry()))
            return true;
    }

    return false;
}

uint8 RetinueInterruptPriority(uint32 spellId)
{
    switch (spellId)
    {
        case SPELL_DELRISSA_FEAR:
        case SPELL_POLYMORPH:
            return 1;
        case SPELL_FLASH_HEAL:
        case SPELL_LESSER_HEALING_WAVE:
            return 2;
        case SPELL_SUMMON_IMP:
            return 3;
        default:
            return 0;
    }
}

uint8 CurrentRetinueCastPriority(Unit* caster)
{
    uint8 best = 0;
    for (CurrentSpellTypes slot : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
    {
        Spell* spell = caster->GetCurrentSpell(slot);
        if (!spell || !spell->m_spellInfo)
            continue;

        uint8 const prio = RetinueInterruptPriority(spell->m_spellInfo->Id);
        if (prio && (!best || prio < best))
            best = prio;
    }

    return best;
}

int InterruptSlot(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    int slot = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
            continue;

        if (!GET_PLAYERBOT_AI(member))
            continue;

        if (member->GetGUID() < bot->GetGUID())
            ++slot;
    }

    return slot;
}

constexpr FocusOrder FOCUS_ORDER_OWNERS[] = { FocusOrder::Kael, FocusOrder::Delrissa, FocusOrder::Trash };
constexpr char const* FOCUS_ORDER_KEYS[] = { "mgt kael focus target", "mgt delrissa focus target",
                                             "mgt focus target" };

bool LapseActive(Player* bot, Unit* kael)
{
    if (!kael || kael->HasAura(SPELL_POWER_FEEDBACK))
        return false;

    if (bot->HasAura(SPELL_GRAVITY_LAPSE_DOT) || bot->HasAura(SPELL_GRAVITY_LAPSE_FLY))
        return true;

    for (CurrentSpellTypes slot : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
    {
        Spell* spell = kael->GetCurrentSpell(slot);
        if (spell && spell->m_spellInfo && spell->m_spellInfo->Id == SPELL_GRAVITY_LAPSE_INITIAL)
            return true;
    }

    return false;
}

uint8 KaelInterruptPriority(uint32 spellId)
{
    switch (spellId)
    {
        case SPELL_PYROBLAST:
            return 1;
        case SPELL_KAEL_FIREBALL:
            return 2;
        default:
            return 0;
    }
}

uint8 CurrentKaelCastPriority(Unit* kael)
{
    uint8 best = 0;
    for (CurrentSpellTypes slot : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
    {
        Spell* spell = kael->GetCurrentSpell(slot);
        if (!spell || !spell->m_spellInfo)
            continue;

        uint8 const prio = KaelInterruptPriority(spell->m_spellInfo->Id);
        if (prio && (!best || prio < best))
            best = prio;
    }

    return best;
}

enum class LapseRole
{
    Tank,
    Healer,
    Melee,
    Ranged
};

LapseRole ClassifyLapseRole(Player* member)
{
    if (PlayerbotAI::IsTank(member))
        return LapseRole::Tank;

    if (PlayerbotAI::IsHeal(member))
        return LapseRole::Healer;

    if (PlayerbotAI::IsMelee(member))
        return LapseRole::Melee;

    return LapseRole::Ranged;
}

int LapseSlot(Player* bot, LapseRole role)
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    int slot = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
            continue;

        if (!GET_PLAYERBOT_AI(member) || ClassifyLapseRole(member) != role)
            continue;

        if (member->GetGUID() < bot->GetGUID())
            ++slot;
    }

    return slot;
}

bool LapseAnchor(Player* bot, LapseSpot& out)
{
    LapseRole const role = ClassifyLapseRole(bot);
    size_t const slot = static_cast<size_t>(LapseSlot(bot, role));

    switch (role)
    {
        case LapseRole::Tank:
            out = LAPSE_TANK;
            break;
        case LapseRole::Healer:
            out = LAPSE_HEALER;
            break;
        case LapseRole::Melee:
            out = LAPSE_MELEE[slot % std::size(LAPSE_MELEE)];
            break;
        default:
            out = LAPSE_RANGED[slot % std::size(LAPSE_RANGED)];
            break;
    }

    return true;
}

float BearingFromKael(float x, float y)
{
    float degrees = std::atan2(x - KAEL_P2.x, y - KAEL_P2.y) * 180.0f / static_cast<float>(M_PI);
    if (degrees < 0.0f)
        degrees += 360.0f;

    return degrees;
}

bool SphereHeading(Unit* sphere, float& hx, float& hy)
{
    if (Unit* victim = sphere->GetVictim())
    {
        float const dx = victim->GetPositionX() - sphere->GetPositionX();
        float const dy = victim->GetPositionY() - sphere->GetPositionY();
        float const len = std::hypot(dx, dy);
        if (len < 0.1f)
            return false;

        hx = dx / len;
        hy = dy / len;
        return true;
    }

    hx = std::cos(sphere->GetOrientation());
    hy = std::sin(sphere->GetOrientation());
    return true;
}

float DistanceToSphereSweep(Unit* sphere, float x, float y)
{
    float const sx = sphere->GetPositionX();
    float const sy = sphere->GetPositionY();

    float hx = 0.0f, hy = 0.0f;
    if (!SphereHeading(sphere, hx, hy))
        return std::hypot(x - sx, y - sy);

    float const reach = SPHERE_SPEED * SPHERE_LEAD;
    float t = (x - sx) * hx + (y - sy) * hy;
    t = std::clamp(t, 0.0f, reach);

    return std::hypot(x - (sx + hx * t), y - (sy + hy * t));
}

float PointToSegment(float px, float py, float ax, float ay, float bx, float by)
{
    float const dx = bx - ax;
    float const dy = by - ay;
    float const len2 = dx * dx + dy * dy;
    if (len2 < 0.0001f)
        return std::hypot(px - ax, py - ay);

    float t = ((px - ax) * dx + (py - ay) * dy) / len2;
    t = std::clamp(t, 0.0f, 1.0f);

    return std::hypot(px - (ax + dx * t), py - (ay + dy * t));
}

float SegmentToSegment(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy)
{
    auto side = [](float ox, float oy, float px, float py, float qx, float qy)
    { return (px - ox) * (qy - oy) - (py - oy) * (qx - ox); };

    float const d1 = side(cx, cy, dx, dy, ax, ay);
    float const d2 = side(cx, cy, dx, dy, bx, by);
    float const d3 = side(ax, ay, bx, by, cx, cy);
    float const d4 = side(ax, ay, bx, by, dx, dy);

    if (((d1 > 0.0f) != (d2 > 0.0f)) && ((d3 > 0.0f) != (d4 > 0.0f)))
        return 0.0f;

    return std::min({ PointToSegment(ax, ay, cx, cy, dx, dy), PointToSegment(bx, by, cx, cy, dx, dy),
                      PointToSegment(cx, cy, ax, ay, bx, by), PointToSegment(dx, dy, ax, ay, bx, by) });
}

float WalkClearanceOfSphere(Unit* sphere, float fx, float fy, float tx, float ty)
{
    float const sx = sphere->GetPositionX();
    float const sy = sphere->GetPositionY();

    float hx = 0.0f, hy = 0.0f;
    if (!SphereHeading(sphere, hx, hy))
        return PointToSegment(sx, sy, fx, fy, tx, ty);

    float const reach = SPHERE_SPEED * SPHERE_LEAD;

    return SegmentToSegment(fx, fy, tx, ty, sx, sy, sx + hx * reach, sy + hy * reach);
}

bool InsideKiteArena(float x, float y, bool healer)
{
    float const bearing = BearingFromKael(x, y);

    if (bearing >= KITE_NORTH_MIN || bearing <= KITE_NORTH_MAX)
        return false;

    bool const bowl = bearing >= KITE_BOWL_MIN && bearing <= KITE_BOWL_MAX;
    if (healer && !bowl)
        return false;

    float const limit = bowl ? KITE_BOWL_RADIUS : KITE_SIDE_RADIUS;

    return std::hypot(x - KAEL_P2.x, y - KAEL_P2.y) <= limit;
}

float KiteRunway(float fx, float fy, float hx, float hy, bool healer, bool outside)
{
    int const steps = static_cast<int>(KITE_PROBE_MAX / KITE_PROBE_STEP);

    bool started = !outside;
    float runway = 0.0f;
    for (int i = 1; i <= steps; ++i)
    {
        float const step = KITE_PROBE_STEP * i;
        bool const clear = InsideKiteArena(fx + hx * step, fy + hy * step, healer);

        if (!started)
        {
            if (clear)
            {
                started = true;
                runway = step;
            }

            continue;
        }

        if (!clear)
            break;

        runway = step;
    }

    return started ? runway : 0.0f;
}

bool IsLapseHealer(Player* bot)
{
    return PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsTank(bot);
}

bool InsideAnyBurn(std::vector<LapseSpot> const& burns, float x, float y)
{
    for (LapseSpot const& burn : burns)
    {
        if (std::hypot(x - burn.x, y - burn.y) < BURN_DAMAGE)
            return true;
    }

    return false;
}

Unit* GetChasingSphere(Player* bot, LapseWorld const& world)
{
    for (Unit* sphere : world.spheres)
    {
        if (sphere->GetVictim() == bot)
            return sphere;
    }

    return nullptr;
}

float BurningPhoenixKaelGap(Player* bot, Unit* kael)
{
    std::vector<Unit*> phoenixes;
    CollectCreaturesByEntry(bot, { NPC_PHOENIX }, PHOENIX_SCAN, phoenixes);

    float gap = std::numeric_limits<float>::max();
    for (Unit* phoenix : phoenixes)
    {
        if (phoenix->HasAura(SPELL_PHOENIX_BURN))
            gap = std::min(gap, phoenix->GetExactDist2d(kael));
    }

    return gap;
}
}

Unit* GetSelin(Player* bot)
{
    return GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<Unit*>("find target", SELIN_NAME)->Get();
}

Unit* GetKaelthas(Player* bot)
{
    return GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<Unit*>("find target", KAEL_NAME)->Get();
}

Unit* GetActiveFelCrystal(Player* bot)
{
    if (!GetSelin(bot))
        return nullptr;

    std::vector<Unit*> crystals;
    CollectCreaturesByEntry(bot, { NPC_FEL_CRYSTAL }, CRYSTAL_SCAN_RANGE, crystals);

    Unit* best = nullptr;
    float bestDist = CRYSTAL_SCAN_RANGE;
    for (Unit* crystal : crystals)
    {
        if (!AttackersValue::IsValidTarget(crystal, bot))
            continue;

        float const dist = bot->GetExactDist2d(crystal);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = crystal;
        }
    }

    return best;
}

void CollectDampeningEscapes(Player* bot, EscapeSpots& out)
{
    if (!bot->IsInCombat())
        return;

    std::vector<Hazard> fields;
    for (Position const& pos : GetDynamicObjectPositions(bot, DAMPENING_SCAN, SPELL_MAGIC_DAMPENING_FIELD))
        fields.push_back({ pos.GetPositionX(), pos.GetPositionY(), DAMPENING_STAND, DAMPENING_CLEAR });

    CollectHazardEscapes(bot, fields, PlayerbotAI::IsTank(bot) ? TANK_REJOIN_GAP : 0.0f, out);
}

Unit* GetGlaiveThrowingMageGuard(Player* bot)
{
    std::vector<Unit*> guards;
    CollectAttackersByEntry(bot, { NPC_SUNBLADE_MAGE_GUARD }, GLAIVE_MAX_RANGE, guards);

    for (Unit* guard : guards)
    {
        if (guard->GetVictim() != bot)
            continue;

        if (bot->GetExactDist2d(guard) > GLAIVE_MIN_RANGE)
            return guard;
    }

    return nullptr;
}

void CollectNovaEscapes(Player* bot, EscapeSpots& out)
{
    if (PlayerbotAI::IsMelee(bot))
        return;

    std::vector<Unit*> magisters;
    CollectAttackersByEntry(bot, { NPC_SUNBLADE_MAGISTER }, NOVA_CLEAR, magisters);

    std::vector<Hazard> hazards;
    hazards.reserve(magisters.size());
    for (Unit* magister : magisters)
    {
        hazards.push_back({ magister->GetPositionX(), magister->GetPositionY(), NOVA_CLEAR,
                            NOVA_CLEAR + 2.0f });
    }

    CollectHazardEscapes(bot, hazards, 0.0f, out);
}

uint8 GetInterruptUrgency(Unit* caster)
{
    return caster ? CurrentCastPriority(caster) : 0;
}

Unit* GetInterruptTarget(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

    Unit* best = nullptr;
    uint8 bestPrio = 0;
    float bestDist = 0.0f;
    auto const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* caster = botAI->GetUnit(guid);
        if (!caster)
            continue;

        float const dist = bot->GetExactDist2d(caster);
        if (dist > INTERRUPT_SCAN)
            continue;

        uint8 const prio = CurrentCastPriority(caster);
        if (!prio)
            continue;

        if (!best || prio < bestPrio || (prio == bestPrio && dist < bestDist))
        {
            best = caster;
            bestPrio = prio;
            bestDist = dist;
        }
    }

    return best;
}

std::string CastInterrupt(PlayerbotAI* botAI, Unit* target, uint8 urgency)
{
    if (!target->IsNonMeleeSpellCast(true))
        return {};

    bool const allowControl = urgency && urgency <= CONTROL_INTERRUPT_URGENCY;

    for (InterruptSpell const& entry : InterruptSpells())
    {
        if (entry.kind == InterruptKind::Control && !allowControl)
            continue;

        std::string const name = entry.name;

        uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get();
        if (!spellId || !StopsACast(spellId))
            continue;

        if (!botAI->CanCastSpell(name, target))
            continue;

        if (botAI->CastSpell(name, target))
            return name;
    }

    return {};
}

uint8 GetRetinueInterruptUrgency(Unit* caster)
{
    return caster ? CurrentRetinueCastPriority(caster) : 0;
}

Unit* GetFocusTarget(Player* bot, ObjectGuid& latched)
{
    if (Unit* wretched = GetWretchedFocus(bot, latched))
        return wretched;

    return PickRankedFocus(bot, &TrashFocusRank, {}, latched);
}

ObjectGuid ResolveFocusOrder(PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();

    for (char const* key : FOCUS_ORDER_KEYS)
    {
        ObjectGuid const order = context->GetValue<ObjectGuid>(key)->Get();
        if (!order.IsEmpty())
            return order;
    }

    return ObjectGuid::Empty;
}

FocusOrder ResolveFocusOrderOwner(PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();

    for (size_t i = 0; i < std::size(FOCUS_ORDER_KEYS); ++i)
    {
        if (!context->GetValue<ObjectGuid>(FOCUS_ORDER_KEYS[i])->Get().IsEmpty())
            return FOCUS_ORDER_OWNERS[i];
    }

    return FocusOrder::None;
}

void CollectFocusExclusions(Player* bot, std::vector<ObjectGuid>& out)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

    ObjectGuid const order = ResolveFocusOrder(botAI);
    if (order.IsEmpty())
        return;

    Unit* focus = botAI->GetUnit(order);
    bool const transient = !focus || focus->GetEntry() == NPC_HIGH_EXPLOSIVE_SHEEP ||
                           focus->GetEntry() == NPC_PHOENIX_EGG;

    auto const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    out.reserve(out.size() + attackers.size() + 1);
    for (ObjectGuid const guid : attackers)
    {
        if (guid != order)
            out.push_back(guid);
    }

    if (transient)
        out.push_back(order);
}

bool WretchedInFight(PlayerbotAI* botAI)
{
    GuidVector const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->RefGet();
    return std::any_of(attackers.begin(), attackers.end(),
                       [](ObjectGuid const& guid)
                       {
                           uint32 const entry = guid.GetEntry();
                           return entry == NPC_WRETCHED_SKULKER || entry == NPC_WRETCHED_BRUISER ||
                                  entry == NPC_WRETCHED_HUSK;
                       });
}

Unit* GetEnragedWretched(Player* bot)
{
    std::vector<Unit*> wretched;
    CollectAttackersByEntry(bot, { NPC_WRETCHED_SKULKER, NPC_WRETCHED_BRUISER, NPC_WRETCHED_HUSK },
                            WRETCHED_SCAN, wretched);

    Unit* best = nullptr;
    float bestDist = WRETCHED_SCAN;
    for (Unit* mob : wretched)
    {
        if (!mob->HasAura(SPELL_DRINK_FEL_INFUSION))
            continue;

        Unit* victim = mob->GetVictim();
        if (!victim || victim == bot)
            continue;

        float const dist = bot->GetExactDist2d(mob);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = mob;
        }
    }

    return best;
}

std::vector<std::string> const& TauntSpellNames()
{
    static std::vector<std::string> const names = { "taunt", "growl", "dark command", "hand of reckoning" };

    return names;
}

void ClampIntoRoom(float& x, float& y)
{
    x = std::clamp(x, ROOM_X_MIN, ROOM_X_MAX);
    y = std::clamp(y, ROOM_Y_MIN, ROOM_Y_MAX);
}

bool ShouldHoldTremorTotem(Player* bot)
{
    if (bot->getClass() != CLASS_SHAMAN || !bot->HasSpell(SPELL_TREMOR_TOTEM))
        return false;

    return RetinueEngaged(bot);
}

Unit* GetDelrissaFocusTarget(Player* bot, ObjectGuid& latched)
{
    std::vector<Unit*> sheep;
    if (RetinueEngaged(bot))
    {
        std::vector<Unit*> found;
        CollectCreaturesByEntry(bot, { NPC_HIGH_EXPLOSIVE_SHEEP }, FOCUS_SCAN, found);

        for (Unit* mob : found)
        {
            if (AttackersValue::IsValidTarget(mob, bot))
                sheep.push_back(mob);
        }
    }

    return PickRankedFocus(bot, &RetinueFocusRank, sheep, latched);
}

void CollectDelrissaInterruptPreference(Player* bot, std::vector<Unit*>& out)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

    std::vector<std::pair<uint8, Unit*>> ranked;
    auto const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* caster = botAI->GetUnit(guid);
        if (!caster || bot->GetExactDist2d(caster) > INTERRUPT_SCAN)
            continue;

        if (!RetinueFocusRank(caster->GetEntry()))
            continue;

        if (uint8 const prio = CurrentRetinueCastPriority(caster))
            ranked.push_back({ prio, caster });
    }

    if (ranked.empty())
        return;

    std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) {
        if (a.first != b.first)
            return a.first < b.first;

        return a.second->GetGUID() < b.second->GetGUID();
    });

    size_t const slot = static_cast<size_t>(InterruptSlot(bot));
    size_t const index = slot < ranked.size() ? slot : 0;

    out.reserve(out.size() + ranked.size());
    out.push_back(ranked[index].second);

    for (size_t i = 0; i < ranked.size(); ++i)
    {
        if (i != index)
            out.push_back(ranked[i].second);
    }
}

void CollectDelrissaHealerSpot(Player* bot, EscapeSpots& out)
{
    if (bot->GetMapId() != MAP_MAGISTERS_TERRACE || !PlayerbotAI::IsHeal(bot) || !bot->IsAlive())
        return;

    // Before the pull: stay beyond the retinue's reach so the opening pick lands on the tank and melee.
    // During the fight: stay beyond 25 yd of the casters (their distance weight drops past that) and out of
    // the melee lackeys' 5 yd, without kiting a mob that already chases the healer.
    constexpr float prePullKeep = 32.0f;
    constexpr float combatKeep = 27.0f;
    constexpr float holdSlack = 3.0f;
    constexpr float scan = 45.0f;
    constexpr float minStep = 3.0f;
    constexpr float minGain = 2.0f;
    constexpr float healSlack = 3.0f;

    static std::vector<uint32> const party = { NPC_DELRISSA, NPC_APOKO, NPC_ELLRYS_DUSKHALLOW, NPC_YAZZAI,
                                               NPC_ZELFAN, NPC_GARAXXAS, NPC_KAGANI_NIGHTSTRIKE,
                                               NPC_WARLORD_SALARIS, NPC_ERAMAS_BRIGHTBLAZE };

    std::vector<Unit*> found;
    CollectCreaturesByEntry(bot, party, scan, found);
    if (found.empty())
        return;

    bool const engaged = bot->IsInCombat() && RetinueEngaged(bot);
    float const keep = engaged ? combatKeep : prePullKeep;

    // [b36_mgt] A melee lackey on the healer: no spacing from the casters - the healer runs to the tank, where the
    // group's crowd control and damage reach the lackey. mgt-h 20261026-034959-mgt-h-5B-15: the healer spot walked
    // the paladin to (79.0,-36.6), 54-55 yd from the shaman and the priest, out of their sight, with Warlord Salaris
    // on it (97 %, nobody near enough to Hex or hit him); it died there, then Salaris killed the rogue and the shaman.
    if (engaged)
    {
        constexpr float toTank = 6.0f;
        Player* mainTank = GetGroupMainTank(bot);
        bool meleeOnMe = false;
        for (Unit* mob : found)
        {
            uint32 const entry = mob->GetEntry();
            if (mob->IsAlive() && mob->GetVictim() == bot && !IsCrowdControlled(mob) &&
                (entry == NPC_KAGANI_NIGHTSTRIKE || entry == NPC_WARLORD_SALARIS || entry == NPC_ERAMAS_BRIGHTBLAZE))
            {
                meleeOnMe = true;
                break;
            }
        }

        if (meleeOnMe && mainTank && mainTank != bot && mainTank->IsAlive() && mainTank->GetMap() == bot->GetMap())
        {
            if (bot->GetExactDist2d(mainTank) <= toTank + 2.0f && bot->IsWithinLOSInMap(mainTank))
                out.push_back({ bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() });
            else
            {
                float const angle = mainTank->GetAngle(bot);
                float x = mainTank->GetPositionX() + std::cos(angle) * toTank;
                float y = mainTank->GetPositionY() + std::sin(angle) * toTank;
                float z = mainTank->GetPositionZ();
                if (!bot->GetMap()->CheckCollisionAndGetValidCoords(mainTank, mainTank->GetPositionX(),
                                                                    mainTank->GetPositionY(),
                                                                    mainTank->GetPositionZ(), x, y, z))
                {
                    x = mainTank->GetPositionX();
                    y = mainTank->GetPositionY();
                    z = mainTank->GetPositionZ();
                }
                out.push_back({ x, y, z });
            }
            return;
        }
    }

    // The gallery to Selin runs over Delrissa's hall (17 yd higher, wave 18): a retinue on another floor is no
    // threat, and holding for it froze the healer before the first boss.
    constexpr float otherFloor = 8.0f;
    std::vector<Unit*> threats;
    for (Unit* mob : found)
    {
        if (engaged ? !mob->IsInCombat() : mob->IsInCombat())
            continue;

        if (!engaged && std::fabs(mob->GetPositionZ() - bot->GetPositionZ()) > otherFloor)
            continue;

        if (engaged && mob->GetVictim() == bot)
            continue;

        threats.push_back(mob);
    }

    if (threats.empty())
        return;

    auto const nearestThreat = [&threats](float x, float y)
    {
        float nearest = std::numeric_limits<float>::max();
        for (Unit* mob : threats)
            nearest = std::min(nearest, mob->GetExactDist2d(x, y));

        return nearest;
    };

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const current = nearestThreat(botX, botY);
    if (current >= keep + holdSlack)
        return;

    if (current >= keep)
    {
        out.push_back({ botX, botY, bot->GetPositionZ() });
        return;
    }

    float cx = 0.0f;
    float cy = 0.0f;
    uint32 closeCount = 0;
    for (Unit* mob : threats)
    {
        if (mob->GetExactDist2d(botX, botY) >= keep)
            continue;

        cx += mob->GetPositionX();
        cy += mob->GetPositionY();
        ++closeCount;
    }

    if (!closeCount)
        return;

    cx /= closeCount;
    cy /= closeCount;

    Player* tank = GetGroupMainTank(bot);
    float const baseAngle = std::atan2(botY - cy, botX - cx);
    float const step = std::max(minStep, keep - current + 2.0f);
    constexpr float turns[] = { 0.0f, 30.0f, -30.0f, 60.0f, -60.0f, 90.0f, -90.0f };

    for (float turn : turns)
    {
        float const angle = baseAngle + turn * static_cast<float>(M_PI) / 180.0f;
        float x = botX + std::cos(angle) * step;
        float y = botY + std::sin(angle) * step;
        float z = bot->GetPositionZ();

        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, botX, botY, bot->GetPositionZ(), x, y, z))
            continue;

        if (nearestThreat(x, y) < std::min(keep, current + minGain))
            continue;

        if (tank && tank != bot && tank->GetExactDist2d(x, y) > sPlayerbotAIConfig.HealDistance - healSlack)
            continue;

        out.push_back({ x, y, z });
        return;
    }

    // Nowhere better within heal range of the tank: at least stop walking in.
    out.push_back({ botX, botY, bot->GetPositionZ() });
}

bool DelrissaRetinueEngaged(Player* bot) { return RetinueEngaged(bot); }

bool IsDelrissaScriptedThreat(Unit* unit)
{
    return unit && unit->IsCreature() && unit->GetEntry() != NPC_DELRISSA &&
           RetinueFocusRank(unit->GetEntry());
}

uint8 DelrissaCcPriority(Unit* unit)
{
    // A lackey that is not on a healer ranks behind every melee lackey that is (RetinueCcRank stays below 10).
    constexpr uint8 onHealerBonus = 10;

    uint8 const rank = unit ? RetinueCcRank(unit->GetEntry()) : 0;
    if (!rank)
        return 0;

    bool const melee = unit->GetEntry() == NPC_KAGANI_NIGHTSTRIKE || unit->GetEntry() == NPC_ERAMAS_BRIGHTBLAZE ||
                       unit->GetEntry() == NPC_WARLORD_SALARIS;
    Player* victim = unit->GetVictim() ? unit->GetVictim()->ToPlayer() : nullptr;
    if (melee && victim && PlayerbotAI::IsHeal(victim))
        return rank;

    return rank + onHealerBonus;
}

std::string DelrissaCcSpell(Player* bot)
{
    // Polymorph and Freezing Trap hold a humanoid; the warlock's Banish role spell does not, Fear does.
    switch (bot->getClass())
    {
        case CLASS_MAGE:
            return "polymorph";
        case CLASS_HUNTER:
        {
            // [delrissa_cc_keep] Freezing Arrow (WotLK, ranged, lands where the lackey stands) over the trap: the
            // retinue re-picks its victims by distance every 8-10 s and Kagani's Vanish moves him, so a trap laid
            // by the hunter was never walked into (build 29 wipe: 1 Freezing Arrow, Kagani free all fight).
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (botAI && botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "freezing arrow")->Get())
                return "freezing arrow";
            return "freezing trap";
        }
        case CLASS_WARLOCK:
            return "fear";
        case CLASS_SHAMAN:
            return "hex";  // a humanoid too; the group's shaman was left out (mgt-h 2026-10-25: no CC landed)
        default:
            return {};
    }
}

Unit* DelrissaRetinueLeft(Player* bot)
{
    constexpr float scan = 200.0f;  // her hall and the way back from the entrance
    if (bot->GetMapId() != MAP_MAGISTERS_TERRACE)
        return nullptr;

    std::list<Creature*> delrissa;
    bot->GetCreatureListWithEntryInGrid(delrissa, std::vector<uint32>{ NPC_DELRISSA }, scan);
    bool dead = false;
    for (Creature* creature : delrissa)
    {
        if (creature->IsAlive())
            return nullptr;
        dead = true;
    }
    if (!dead)
        return nullptr;

    static std::vector<uint32> const lackeys = { NPC_APOKO,  NPC_ELLRYS_DUSKHALLOW, NPC_YAZZAI,
                                                 NPC_ZELFAN, NPC_GARAXXAS,          NPC_KAGANI_NIGHTSTRIKE,
                                                 NPC_WARLORD_SALARIS, NPC_ERAMAS_BRIGHTBLAZE };
    std::vector<Unit*> alive;
    CollectCreaturesByEntry(bot, lackeys, scan, alive);
    Unit* nearest = nullptr;
    for (Unit* lackey : alive)
    {
        if (lackey->IsInCombat() || !bot->IsValidAttackTarget(lackey))
            continue;
        if (!nearest || bot->GetExactDist(lackey) < bot->GetExactDist(nearest))
            nearest = lackey;
    }
    return nearest;
}

uint32 CountControlledRetinue(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    uint32 count = 0;
    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (!RetinueCcRank(guid.GetEntry()))
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && IsCrowdControlTarget(bot, unit))
            ++count;
    }
    return count;
}

namespace
{
// [b44_mgt] Heroic Medallion of Immunity (boss_priestess_delrissa.cpp: every lackey checks UNIT_STATE_LOST_CONTROL
// once a second from 2 s into its fight, casts 46227 and waits 1 min): the first crowd control on each lackey is
// thrown off within a second, and the next one holds for the whole minute. The generic back-off ([rot_b31_cc],
// 15 s after an early break) read that first break as the group's area damage and kept the lackey free for 15 s.
// mgt-h 20261026-075135-mgt-h-5A-14: "[cc backoff] Eramas Brightblaze (207): crowd control broke after 0 ms" and
// "Warlord Salaris (208): ... after 2000 ms", then no crowd control on either again; Eramas killed the shaman 22 s
// after his Polymorph, Salaris the mage. The first back-off of a lackey whose medallion is due (none seen in the last
// minute) is the medallion: the lackey may be controlled again at once, for the length of that back-off.
constexpr uint32 MEDALLION_COOLDOWN_MS = 60 * IN_MILLISECONDS;
constexpr uint32 MEDALLION_FORGIVE_MS = 15 * IN_MILLISECONDS;  // RtiValue.cpp CC_BREAK_BACKOFF_MS
constexpr size_t MEDALLION_PRUNE_SIZE = 64;

struct MedallionLedger
{
    std::mutex lock;  // bots of different maps run on different threads
    std::unordered_map<ObjectGuid, uint32> spentMs;
};

MedallionLedger& GetMedallionLedger()
{
    static MedallionLedger ledger;
    return ledger;
}

bool RetinueMedallionDue(Player* bot, Unit* unit)
{
    Map* map = bot->GetMap();
    return map && map->IsHeroic() && unit && RetinueCcRank(unit->GetEntry());
}

// The medallion of this lackey was thrown within the last minute (it holds the next crowd control).
bool RetinueMedallionSpent(Player* bot, Unit* unit)
{
    if (!RetinueMedallionDue(bot, unit))
        return true;  // normal mode: no medallion

    MedallionLedger& ledger = GetMedallionLedger();
    std::lock_guard<std::mutex> guard(ledger.lock);
    auto const itr = ledger.spentMs.find(unit->GetGUID());
    return itr != ledger.spentMs.end() && getMSTimeDiff(itr->second, getMSTime()) < MEDALLION_COOLDOWN_MS;
}

// IsCrowdControlBackedOff, except for the back-off that the heroic medallion caused.
bool RetinueCcBackedOff(Player* bot, Unit* unit)
{
    if (!IsCrowdControlBackedOff(unit))
        return false;

    if (!RetinueMedallionDue(bot, unit))
        return true;

    uint32 const now = getMSTime();
    MedallionLedger& ledger = GetMedallionLedger();
    std::lock_guard<std::mutex> guard(ledger.lock);
    if (ledger.spentMs.size() >= MEDALLION_PRUNE_SIZE)
    {
        for (auto itr = ledger.spentMs.begin(); itr != ledger.spentMs.end();)
        {
            if (getMSTimeDiff(itr->second, now) >= 10 * MEDALLION_COOLDOWN_MS)
                itr = ledger.spentMs.erase(itr);
            else
                ++itr;
        }
    }

    auto const itr = ledger.spentMs.find(unit->GetGUID());
    if (itr == ledger.spentMs.end() || getMSTimeDiff(itr->second, now) >= MEDALLION_COOLDOWN_MS)
    {
        ledger.spentMs[unit->GetGUID()] = now ? now : 1;
        LOG_INFO("playerbots",
                 "[b44_mgt] delrissa medallion: {} ({}) threw off the crowd control with its Medallion of Immunity "
                 "(1 min cooldown) - {} may control it again at once",
                 unit->GetName(), unit->GetGUID().GetCounter(), bot->GetName());
        return false;
    }

    return getMSTimeDiff(itr->second, now) >= MEDALLION_FORGIVE_MS;
}
}  // namespace

Unit* PickDelrissaCcTarget(Player* bot, SpellInfo const* spellInfo)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    AiObjectContext* context = botAI->GetAiObjectContext();
    ObjectGuid const focus = context->GetValue<ObjectGuid>("mgt delrissa focus target")->Get();
    // [b44_mgt] a crowd control with a cooldown (Hex 45 s) goes to a lackey whose medallion is spent first: on one
    // whose medallion is due it is thrown off at once and the cooldown leaves that lackey free (5A-14: Hex on
    // Warlord Salaris broke after 2 s, Salaris then killed the mage). Polymorph, no cooldown, can bait it.
    bool const cooldownSpell = spellInfo->RecoveryTime > 0 || spellInfo->CategoryRecoveryTime > 0;
    constexpr uint8 medallionDuePenalty = 20;

    // Only the lackeys actually in the fight (her four of eight are random): the most dangerous of them as they stand
    // now, a melee lackey on a healer first.
    Unit* best = nullptr;
    uint8 bestRank = 0;
    for (ObjectGuid const guid : context->GetValue<GuidVector>("attackers")->Get())
    {
        if (!RetinueCcRank(guid.GetEntry()) || guid == focus)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || bot->GetExactDist2d(unit) > FOCUS_SCAN || IsCrowdControlTarget(bot, unit))
            continue;

        // [b34_mgt] a lackey whose hold broke early (heroic Medallion of Immunity, once a minute) is refused by
        // "cast auto cc" for 15 s: marking it again only cleared the mark again, and the next lackey stayed free
        // [b44_mgt] except the back-off its heroic medallion caused (RetinueCcBackedOff)
        if (RetinueCcBackedOff(bot, unit))
            continue;

        uint8 rank = DelrissaCcPriority(unit);
        if (rank && cooldownSpell && !RetinueMedallionSpent(bot, unit))
            rank = static_cast<uint8>(rank + medallionDuePenalty);
        if (!rank || (best && rank >= bestRank))
            continue;

        if (!spellInfo->CheckTargetCreatureType(unit) || unit->IsImmunedToSpell(spellInfo))
            continue;

        best = unit;
        bestRank = rank;
    }
    return best;
}

Unit* DelrissaOwnUnheldMark(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    Group* group = bot->GetGroup();
    if (!botAI || !group)
        return nullptr;

    std::string const spell = DelrissaCcSpell(bot);
    if (spell != "hex" && spell != "polymorph")
        return nullptr;

    AiObjectContext* context = botAI->GetAiObjectContext();
    AutoCcMark const& mark = context->GetValue<AutoCcMark>("auto cc mark")->RefGet();
    if (mark.target.IsEmpty() || mark.broken || !RetinueCcRank(mark.target.GetEntry()))
        return nullptr;

    // Hex has a 45 s cooldown: nothing to do until it is ready (the mark ages out unheld meanwhile)
    uint32 const spellId = context->GetValue<uint32>("spell id", spell)->Get();
    if (!spellId || bot->HasSpellCooldown(spellId) || bot->IsNonMeleeSpellCast(false))
        return nullptr;

    int32 const index = RtiTargetValue::GetRtiIndex(context->GetValue<std::string>("rti cc")->Get());
    if (index < 0 || group->GetTargetIcon(static_cast<uint8>(index)) != mark.target)
        return nullptr;

    Unit* unit = botAI->GetUnit(mark.target);
    if (!unit || !unit->IsAlive() || IsCrowdControlled(unit) || RetinueCcBackedOff(bot, unit))  // [b44_mgt]
        return nullptr;

    return unit;
}

void CollectDelrissaPets(Player* bot, std::vector<ObjectGuid>& out)
{
    std::vector<Unit*> pets;
    CollectAttackersByEntry(bot, { NPC_FIZZLE, NPC_SLIVER }, FOCUS_SCAN, pets);

    out.reserve(out.size() + pets.size());
    for (Unit* pet : pets)
        out.push_back(pet->GetGUID());
}

Unit* GetKaelInterruptTarget(Player* bot)
{
    Unit* kael = GetKaelthas(bot);
    if (!kael || kael->HasAura(SPELL_SHOCK_BARRIER))
        return nullptr;

    return CurrentKaelCastPriority(kael) ? kael : nullptr;
}

bool IsKaelUnattackable(Player* bot)
{
    Unit* kael = GetKaelthas(bot);

    return kael && kael->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);
}

void CollectFlameStrikeEscapes(Player* bot, EscapeSpots& out)
{
    if (!GetKaelthas(bot))
        return;

    std::vector<Hazard> hazards;

    std::vector<Unit*> triggers;
    CollectCreaturesByEntry(bot, { NPC_FLAMESTRIKE_TRIGGER }, FLAMESTRIKE_SCAN, triggers);
    for (Unit* trigger : triggers)
    {
        hazards.push_back({ trigger->GetPositionX(), trigger->GetPositionY(), FLAMESTRIKE_STAND,
                            FLAMESTRIKE_CLEAR });
    }

    for (uint32 spellId : { SPELL_FLAME_STRIKE, SPELL_FLAME_STRIKE_H })
    {
        for (Position const& pos : GetDynamicObjectPositions(bot, FLAMESTRIKE_SCAN, spellId))
        {
            hazards.push_back({ pos.GetPositionX(), pos.GetPositionY(), FLAMESTRIKE_STAND,
                                FLAMESTRIKE_CLEAR });
        }
    }

    CollectHazardEscapes(bot, hazards, 0.0f, out);
}

void CollectPhoenixEscapes(Player* bot, EscapeSpots& out)
{
    Unit* kael = GetKaelthas(bot);
    if (!kael || LapseActive(bot, kael))
        return;

    bool const melee = PlayerbotAI::IsMelee(bot);
    float const stand = melee ? BURN_MELEE_STAND : BURN_STAND;
    float const clear = melee ? BURN_MELEE_CLEAR : BURN_CLEAR;

    std::vector<Unit*> phoenixes;
    CollectCreaturesByEntry(bot, { NPC_PHOENIX }, PHOENIX_SCAN, phoenixes);

    std::vector<Hazard> hazards;
    for (Unit* phoenix : phoenixes)
    {
        if (!phoenix->HasAura(SPELL_PHOENIX_BURN))
            continue;

        hazards.push_back({ phoenix->GetPositionX(), phoenix->GetPositionY(), stand, clear });
    }

    if (melee)
        CollectRingEscapes(bot, kael, hazards, out);
    else
        CollectHazardEscapes(bot, hazards, 0.0f, out);
}

PhoenixRing GetPhoenixRing(Player* bot)
{
    if (!PlayerbotAI::IsMelee(bot))
        return PhoenixRing::None;

    Unit* kael = GetKaelthas(bot);
    if (!kael || LapseActive(bot, kael))
        return PhoenixRing::None;

    float const gap = BurningPhoenixKaelGap(bot, kael);
    if (gap >= KAEL_MELEE_RING + BURN_MELEE_CLEAR)
        return PhoenixRing::None;

    if (gap < BURN_MELEE_CLEAR - KAEL_MELEE_RING &&
        bot->GetExactDist2d(kael) <= KAEL_MELEE_RING + BURN_MELEE_CLEAR)
        return PhoenixRing::Covers;

    return PhoenixRing::Threatens;
}

Unit* GetKaelFocusTarget(Player* bot)
{
    Unit* kael = GetKaelthas(bot);
    if (!kael)
        return nullptr;

    if (kael->HasAura(SPELL_SHOCK_BARRIER) && AttackersValue::IsValidTarget(kael, bot))
        return kael;

    std::vector<Unit*> kin;
    CollectCreaturesByEntry(bot, { NPC_PHOENIX_EGG, NPC_PHOENIX }, PHOENIX_SCAN, kin);

    Unit* best = nullptr;
    float bestDist = PHOENIX_SCAN;
    for (Unit* egg : kin)
    {
        if (egg->GetEntry() != NPC_PHOENIX_EGG || !AttackersValue::IsValidTarget(egg, bot))
            continue;

        float const dist = bot->GetExactDist2d(egg);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = egg;
        }
    }

    if (best)
        return best;

    if (PlayerbotAI::IsMelee(bot))
        return nullptr;

    bestDist = PHOENIX_SCAN;
    for (Unit* phoenix : kin)
    {
        if (phoenix->GetEntry() != NPC_PHOENIX || !phoenix->IsInCombat() ||
            !AttackersValue::IsValidTarget(phoenix, bot))
            continue;

        float const dist = bot->GetExactDist2d(phoenix);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = phoenix;
        }
    }

    return best;
}

bool IsGravityLapseActive(Player* bot)
{
    return LapseActive(bot, GetKaelthas(bot));
}

void CollectLapseWorld(Player* bot, LapseWorld& out)
{
    std::vector<Unit*> units;
    CollectCreaturesByEntry(bot, { NPC_ARCANE_SPHERE, NPC_PHOENIX }, std::max(SPHERE_SCAN, PHOENIX_SCAN), units);

    for (Unit* unit : units)
    {
        if (unit->GetEntry() == NPC_ARCANE_SPHERE && bot->GetExactDist2d(unit) <= SPHERE_SCAN)
            out.spheres.push_back(unit);
        else if (unit->GetEntry() == NPC_PHOENIX && unit->HasAura(SPELL_PHOENIX_BURN) &&
                 bot->GetExactDist2d(unit) <= PHOENIX_SCAN)
            out.burns.push_back({ unit->GetPositionX(), unit->GetPositionY() });
    }
}

bool GetGravityLapseKite(Player* bot, LapseWorld const& world, LapseSpot& out, KiteState& state)
{
    Unit* chaser = GetChasingSphere(bot, world);
    if (!chaser)
    {
        state = KiteState{};
        return false;
    }

    if (state.lastMs && GetMSTimeDiffToNow(state.lastMs) > KITE_COMMIT_BREAK_MS)
        state = KiteState{};

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();

    float ax = bx - chaser->GetPositionX();
    float ay = by - chaser->GetPositionY();
    float away = std::hypot(ax, ay);

    if (away < 0.1f)
    {
        ax = bx - KAEL_P2.x;
        ay = by - KAEL_P2.y;
        away = std::hypot(ax, ay);
        if (away < 0.1f)
            return false;
    }

    ax /= away;
    ay /= away;

    bool const healer = IsLapseHealer(bot);
    bool const outside = !InsideKiteArena(bx, by, healer);

    float here = std::numeric_limits<float>::max();
    for (Unit* other : world.spheres)
        here = std::min(here, DistanceToSphereSweep(other, bx, by));

    float const passBar = std::min(KITE_PASS_CLEAR, here - SPHERE_ROUTE_SLACK);

    bool haveFallback = false;
    float fallbackClear = -1.0f;
    float fallbackTurn = 0.0f;
    LapseSpot fallback = { bx, by };

    for (int pass = 0; pass < 2; ++pass)
    {
        bool const requireFullLeg = pass == 0;

        for (float magnitude : KITE_TURN_MAGNITUDES)
        {
            bool found = false;
            float bestPreference = 0.0f;
            float bestTurn = 0.0f;
            LapseSpot best = { bx, by };

            for (int side = 0; side < 2; ++side)
            {
                float const turn = side ? -magnitude : magnitude;
                if (side && magnitude <= 0.0f)
                    continue;

                float const angle = turn * static_cast<float>(M_PI) / 180.0f;
                float const cosT = std::cos(angle);
                float const sinT = std::sin(angle);

                float const hx = ax * cosT - ay * sinT;
                float const hy = ax * sinT + ay * cosT;

                float const x = bx + hx * KITE_LEG;
                float const y = by + hy * KITE_LEG;

                float const runway = KiteRunway(bx, by, hx, hy, healer, outside);

                if (requireFullLeg ? runway < KITE_LEG : !InsideKiteArena(x, y, healer))
                    continue;

                float clearance = std::numeric_limits<float>::max();
                for (Unit* other : world.spheres)
                    clearance = std::min(clearance, WalkClearanceOfSphere(other, bx, by, x, y));

                if (clearance < passBar)
                {
                    if (clearance > fallbackClear)
                    {
                        haveFallback = true;
                        fallbackClear = clearance;
                        fallbackTurn = turn;
                        fallback = { x, y };
                    }

                    continue;
                }

                float preference = clearance + runway * KITE_RUNWAY_TIEBREAK;

                if (std::fabs(turn) > KITE_COMMIT_STRAIGHT && std::fabs(state.turn) > KITE_COMMIT_STRAIGHT &&
                    (turn > 0.0f) == (state.turn > 0.0f))
                    preference += KITE_COMMIT_BONUS;

                if (InsideAnyBurn(world.burns, x, y))
                    preference -= LAPSE_BURN_COST;

                if (!found || preference > bestPreference)
                {
                    found = true;
                    bestPreference = preference;
                    bestTurn = turn;
                    best = { x, y };
                }
            }

            if (found)
            {
                out = best;
                state.turn = bestTurn;
                state.lastMs = getMSTime();

                return true;
            }
        }
    }

    if (!haveFallback)
        return false;

    out = fallback;
    state.turn = fallbackTurn;
    state.lastMs = getMSTime();

    return true;
}

bool IsLapseMeleeSlot(Player* bot)
{
    LapseRole const role = ClassifyLapseRole(bot);

    return role == LapseRole::Tank || role == LapseRole::Melee;
}

bool GetGravityLapseDodge(Player* bot, LapseWorld const& world, LapseSpot& out)
{
    if (world.spheres.empty())
        return false;

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();

    float here = std::numeric_limits<float>::max();
    for (Unit* sphere : world.spheres)
        here = std::min(here, DistanceToSphereSweep(sphere, bx, by));

    if (here >= SPHERE_ALERT)
        return false;

    LapseSpot anchor = { bx, by };
    LapseAnchor(bot, anchor);

    bool const healer = IsLapseHealer(bot);

    float const routeBar = std::min(SPHERE_TRANSIT, here - SPHERE_ROUTE_SLACK);

    bool found = false;
    float best = 0.0f;

    bool haveFallback = false;
    float fallbackClearance = 0.0f;
    LapseSpot fallback = { bx, by };

    for (float radius : SPHERE_DODGE_RADII)
    {
        for (int i = 0; i < SPHERE_DODGE_BEARINGS; ++i)
        {
            float const angle = (2.0f * static_cast<float>(M_PI) * i) / SPHERE_DODGE_BEARINGS;
            float const x = bx + std::cos(angle) * radius;
            float const y = by + std::sin(angle) * radius;

            if (!InsideKiteArena(x, y, healer))
                continue;

            float clearance = std::numeric_limits<float>::max();
            float route = std::numeric_limits<float>::max();
            for (Unit* sphere : world.spheres)
            {
                clearance = std::min(clearance, DistanceToSphereSweep(sphere, x, y));
                route = std::min(route, WalkClearanceOfSphere(sphere, bx, by, x, y));
            }

            if (route < routeBar)
                continue;

            if (clearance < SPHERE_CLEAR)
            {
                if (clearance > here + SPHERE_DODGE_MIN_GAIN && clearance > fallbackClearance)
                {
                    haveFallback = true;
                    fallbackClearance = clearance;
                    fallback = { x, y };
                }

                continue;
            }

            float const margin = std::min(clearance - SPHERE_CLEAR, SPHERE_DODGE_MARGIN_CAP);
            float const score = std::hypot(x - anchor.x, y - anchor.y) +
                                radius * SPHERE_DODGE_TRAVEL_COST - margin * SPHERE_DODGE_MARGIN_WEIGHT +
                                (InsideAnyBurn(world.burns, x, y) ? LAPSE_BURN_COST : 0.0f);

            if (!found || score < best)
            {
                best = score;
                out = { x, y };
                found = true;
            }
        }
    }

    if (found)
        return true;

    if (haveFallback)
    {
        out = fallback;
        return true;
    }

    return false;
}

bool GetGravityLapseSpot(Player* bot, LapseWorld const& world, LapseSpot& out)
{
    if (!LapseAnchor(bot, out))
        return false;

    LapseSpot const anchor = out;

    for (Unit* sphere : world.spheres)
    {
        float dx = out.x - sphere->GetPositionX();
        float dy = out.y - sphere->GetPositionY();
        float dist = std::hypot(dx, dy);
        if (dist >= SPHERE_CLEAR)
            continue;

        if (dist < 0.1f)
        {
            dx = out.x - KAEL_P2.x;
            dy = out.y - KAEL_P2.y;
            dist = std::hypot(dx, dy);
            if (dist < 0.1f)
                break;
        }

        out.x = sphere->GetPositionX() + (dx / dist) * SPHERE_CLEAR;
        out.y = sphere->GetPositionY() + (dy / dist) * SPHERE_CLEAR;
    }

    float const pushX = out.x - anchor.x;
    float const pushY = out.y - anchor.y;
    float const push = std::hypot(pushX, pushY);
    if (push > LAPSE_PUSH_MAX)
    {
        out.x = anchor.x + (pushX / push) * LAPSE_PUSH_MAX;
        out.y = anchor.y + (pushY / push) * LAPSE_PUSH_MAX;
    }

    bool const healer = IsLapseHealer(bot);
    if (InsideKiteArena(anchor.x, anchor.y, healer) && !InsideKiteArena(out.x, out.y, healer))
        out = anchor;

    return true;
}

bool OnLapseStation(Player* bot, Unit* kael, LapseSpot const& spot)
{
    if (!IsLapseMeleeSlot(bot) || !kael)
        return bot->GetExactDist2d(spot.x, spot.y) <= LAPSE_TOLERANCE;

    return bot->IsWithinCombatRange(kael, sPlayerbotAIConfig.MeleeDistance + CONTACT_DISTANCE);
}

bool LapseThreatened(Player* bot, LapseWorld const& world)
{
    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();

    for (Unit* sphere : world.spheres)
    {
        if (DistanceToSphereSweep(sphere, bx, by) < SPHERE_ALERT)
            return true;
    }

    return false;
}

// [b44_mgt] A healer or a ranged member with no sphere near stands and casts. mgt-n 20261026-075135-mgt-n-5B-13
// (Kael 50 % at 1724 s): "mgt take lapse spot" ran every tick for 50 s - kite legs from a sphere that chased the bot
// from across the room, station walks of a few yards - and pre-empted every cast: "[idle diag] Rlpriak 15s ... cast
// 1", the healer at 16 % "moving true" with only the lapse move possible; the Gravity Lapse damage drained the
// group unhealed (healer 1735 s, rogue 1749 s, shaman 1777 s, tank 1778 s). A sphere moves 3 yd/s
// (SPHERE_SPEED): this far from every sphere's sweep a 2.5 s cast ends with the bot still out of its alert range.
bool LapseCasterMayStand(Player* bot, LapseWorld const& world)
{
    constexpr float standClear = SPHERE_ALERT + SPHERE_SPEED * 2.5f;
    if (IsLapseMeleeSlot(bot))
        return false;

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    if (!InsideKiteArena(bx, by, IsLapseHealer(bot)) || InsideAnyBurn(world.burns, bx, by))
        return false;

    for (Unit* sphere : world.spheres)
    {
        if (DistanceToSphereSweep(sphere, bx, by) < standClear)
            return false;
    }

    return true;
}

bool GetLapseApproach(Player* bot, LapseWorld const& world, LapseSpot const& station, bool allowBlocked,
                      LapseSpot& out)
{
    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();

    float const gap = std::hypot(station.x - bx, station.y - by);
    if (gap < 0.1f)
        return false;

    float const leg = std::min(LAPSE_LEG, gap);
    float const progressBar = std::min(LAPSE_PROGRESS_MIN, leg * 0.5f);
    float const dx = (station.x - bx) / gap;
    float const dy = (station.y - by) / gap;

    float here = std::numeric_limits<float>::max();
    for (Unit* sphere : world.spheres)
        here = std::min(here, DistanceToSphereSweep(sphere, bx, by));

    float const routeBar = std::min(SPHERE_TRANSIT, here - SPHERE_ROUTE_SLACK);

    bool haveFallback = false;
    float fallbackRoute = -1.0f;
    LapseSpot fallback = { bx, by };

    for (float magnitude : LAPSE_DETOUR_TURNS)
    {
        bool found = false;
        float bestRoute = 0.0f;
        LapseSpot best = { bx, by };

        for (int side = 0; side < 2; ++side)
        {
            if (side && magnitude <= 0.0f)
                continue;

            float const angle = (side ? -magnitude : magnitude) * static_cast<float>(M_PI) / 180.0f;
            float const cosT = std::cos(angle);
            float const sinT = std::sin(angle);

            float const x = bx + (dx * cosT - dy * sinT) * leg;
            float const y = by + (dx * sinT + dy * cosT) * leg;

            if (gap - std::hypot(station.x - x, station.y - y) < progressBar)
                continue;

            float route = std::numeric_limits<float>::max();
            for (Unit* sphere : world.spheres)
                route = std::min(route, WalkClearanceOfSphere(sphere, bx, by, x, y));

            if (route < routeBar)
            {
                if (route > fallbackRoute)
                {
                    haveFallback = true;
                    fallbackRoute = route;
                    fallback = { x, y };
                }

                continue;
            }

            if (!found || route > bestRoute)
            {
                found = true;
                bestRoute = route;
                best = { x, y };
            }
        }

        if (found)
        {
            out = best;
            return true;
        }
    }

    if (!allowBlocked || !haveFallback)
        return false;

    out = fallback;

    return true;
}

void CollectMeleePhoenixExclusions(Player* bot, std::vector<ObjectGuid>& out)
{
    if (!PlayerbotAI::IsMelee(bot) || !GetKaelthas(bot))
        return;

    std::vector<Unit*> phoenixes;
    CollectCreaturesByEntry(bot, { NPC_PHOENIX }, PHOENIX_SCAN, phoenixes);

    for (Unit* phoenix : phoenixes)
        out.push_back(phoenix->GetGUID());
}

namespace SelinRoom
{
namespace
{
// Selin's spawn (creature 96875) on her dais.
constexpr float SELIN_X = 242.1f;
constexpr float SELIN_Y = 0.3f;
// The tactic runs within this of her: her room, the corridor before her door and the end of the gallery (x 167+),
// on her floor (z -2.9); Delrissa's hall under the gallery (z -21) is reached only after Vexallus.
constexpr float AREA_RADIUS = 75.0f;
constexpr float AREA_MIN_Z = -8.0f;
constexpr float AREA_MAX_Z = 6.0f;
// Her Wretched stand 11-28 yd from her.
constexpr float ROOM_RADIUS = 40.0f;
constexpr float SCAN_RANGE = 120.0f;
// worldserver.conf CreatureFamilyAssistanceRadius; Creature::CallAssistance adds both combat reaches.
constexpr float ASSIST_RADIUS = 10.0f;
constexpr float LINK_MARGIN = 1.5f;
// Idle trash outside her room this close to the rally, on the same floor, is the planner's pull first.
constexpr float OTHER_TRASH_RADIUS = 35.0f;
constexpr float OTHER_TRASH_DZ = 5.0f;
// A cast spot stays this far outside every other idle pack's aggro range, the walk to it WALK_MARGIN.
constexpr float SPOT_MARGIN = 1.5f;  // MovementAction::SIGHT_IDLE_AGGRO_MARGIN (was 2.5, [sight-arc])
constexpr float WALK_MARGIN = 1.5f;
constexpr float WALK_STEP = 1.0f;
// The spots looked at: the corridor before her door (x 197-213, navmesh 585) and her floor up to the side packs.
constexpr float SPOT_MIN_X = 196.0f;
constexpr float SPOT_MAX_X = 226.0f;
constexpr float SPOT_MIN_Y = -22.0f;
constexpr float SPOT_MAX_Y = 22.0f;
constexpr float SPOT_STEP = 1.0f;
constexpr int32 SPOT_COLUMNS = int32((SPOT_MAX_X - SPOT_MIN_X) / SPOT_STEP) + 1;
constexpr int32 SPOT_ROWS = int32((SPOT_MAX_Y - SPOT_MIN_Y) / SPOT_STEP) + 1;
// The floor under a spot: corridor and room at -2.9 .. -1.5 (steps); the balcony over the corridor is at 8.8.
constexpr float FLOOR_PROBE_Z = 0.5f;
constexpr float FLOOR_SEARCH = 6.0f;
constexpr float FLOOR_MIN_Z = -4.5f;
constexpr float FLOOR_MAX_Z = -0.8f;
constexpr float EYE_HEIGHT = 2.0f;
// The spot stands this much inside the pull spell's range (PullAction casts within range - 0.5, exact distance).
constexpr float RANGE_SLACK = 1.5f;
// A margin above this does not make a spot better: the one nearer the rally wins.
constexpr float MARGIN_CAP = 5.0f;
// New line-of-sight probes and paths per plan (one bot, every PLAN_MS); what was probed stays cached, so the next
// plans go on where this one stopped.
constexpr uint32 MAX_NEW_RAYS = 800;
constexpr uint32 MAX_NEW_PATHS = 12;
// Spots are only looked at around her door (x 215, y -4..5): from the corridor the room is seen through it alone.
constexpr float DOOR_X = 214.5f;
constexpr float DOOR_Y = 0.5f;
constexpr float DOOR_SEARCH_RADIUS = 10.0f;
// The members of a pack nearest to the door are the pull targets looked at.
constexpr size_t TARGETS_PER_PACK = 3;
constexpr float PATH_END_TOLERANCE = 1.5f;
// Where a bot ends up when it walks to its target: ranged stop at about their range, melee at the target.
constexpr float RANGED_STAND = 28.0f;
constexpr float MELEE_STAND = 3.0f;
constexpr uint32 CACHE_MS = 500;
constexpr uint32 PLAN_MS = 3000;
// Engaged with nobody to hit this long, Selin is taken as inert (a normal evade comes within a second or two).
// [selin_ac_final_pull] AzerothCore: she never evades (mgt-h 2026-10-25, 4+ runs): 3 s, not 8 s, is enough to tell.
constexpr uint32 INERT_MS = 3000;
// Inert with her room clear, her fight is committed (Snapshot::selinCommitted); inert with Wretched alive the room
// tactic goes on clearing them ("mgt selin room pull inert"). The former 45 s give-up waited for no idle pack to stand
// within reach of her fight, which the side packs always do: it never let go (mgt-h/mgt-n 2026-10-25 11:20, build 24).
// A Wretched this close to her door (or inside) is no pet's target while the room is cleared: the pet's run to the
// mob's far side (DungeonPetControl, [pet-quick-engage]) ends inside her room.
constexpr float PET_DOOR_MARGIN = 4.0f;
constexpr uint32 LOG_MS = 10000;

bool IsRoomWretched(uint32 entry)
{
    return entry == NPC_WRETCHED_SKULKER || entry == NPC_WRETCHED_BRUISER || entry == NPC_WRETCHED_HUSK;
}

bool InArea(Player* bot)
{
    return bot->GetMapId() == MAP_MAGISTERS_TERRACE && bot->GetPositionZ() > AREA_MIN_Z &&
           bot->GetPositionZ() < AREA_MAX_Z && bot->GetExactDist2d(SELIN_X, SELIN_Y) < AREA_RADIUS;
}

bool GroupInCombat(Player* bot)
{
    if (bot->IsInCombat())
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (DungeonInteract::IsActiveMember(bot, member) && member->IsInCombat())
            return true;
    }
    return false;
}

void Compute(PlayerbotAI* botAI, Snapshot& snapshot)
{
    snapshot.active = false;
    snapshot.selinEngaged = false;
    snapshot.selinInert = false;
    snapshot.fighting = false;
    snapshot.otherTrash = false;
    snapshot.selin.Clear();
    snapshot.packs.clear();
    snapshot.idle.clear();
    snapshot.reachPacks = 0;
    snapshot.wretchedAlive = 0;
    snapshot.selinCommitted = false;
    snapshot.selinHasVictim = false;
    snapshot.selinPetVictim.Clear();

    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || !InArea(bot))
        return;

    Creature* selin = bot->FindNearestCreature(NPC_SELIN_FIREHEART, SCAN_RANGE, true);
    if (!selin)
        return;

    snapshot.selin = selin->GetGUID();
    snapshot.selinEngaged = selin->IsInCombat();
    // How long she has been engaged with nobody to hit (the snapshot object lives on in the per-bot cache).
    uint32 const now = getMSTime();
    // [b42_mgt] one stamp per Selin, shared by every map thread (the per-thread snapshot copies each kept their own:
    // stale ones read 559 s in a 493 s run, fresh ones on a thread that rarely updates the bot never reached INERT_MS)
    {
        static std::mutex targetlessLock;
        static std::unordered_map<uint64, uint32> targetlessSince;  // Selin's guid -> engaged with no victim since
        std::lock_guard<std::mutex> guard(targetlessLock);
        uint64 const key = (uint64(selin->GetInstanceId()) << 32) | uint64(selin->GetGUID().GetCounter());
        if (!snapshot.selinEngaged || selin->GetVictim())
            targetlessSince.erase(key);
        else if (!targetlessSince.count(key))
            targetlessSince[key] = now ? now : 1;
        auto const found = targetlessSince.find(key);
        snapshot.selinTargetlessMs = found != targetlessSince.end() ? found->second : 0;
    }
    snapshot.selinInert = snapshot.selinTargetlessMs && getMSTimeDiff(snapshot.selinTargetlessMs, now) >= INERT_MS;
    // Her victim counts only as a player of the group: a pet or guardian in her room (mgt-h build 25: a hunter's
    // Crab) is called back ("mgt selin pet out"), and with nobody to hit she evades.
    Unit* const victim = snapshot.selinEngaged ? selin->GetVictim() : nullptr;
    Player* const victimPlayer = victim ? victim->ToPlayer() : nullptr;
    snapshot.selinHasVictim = victimPlayer && (victimPlayer == bot || victimPlayer->IsInSameGroupWith(bot));
    if (victim && !victimPlayer)
        if (Player* owner = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
            owner && (owner == bot || owner->IsInSameGroupWith(bot)))
            snapshot.selinPetVictim = victim->GetGUID();

    std::vector<Unit*> wretched;
    CollectCreaturesByEntry(bot, {NPC_WRETCHED_SKULKER, NPC_WRETCHED_BRUISER, NPC_WRETCHED_HUSK}, SCAN_RANGE,
                            wretched);

    // The room is clear only once every Wretched of her floor is dead, the ones fought at the rally included.
    for (Unit* unit : wretched)
        if (unit->GetExactDist2d(SELIN_X, SELIN_Y) < AREA_RADIUS && unit->GetPositionZ() > AREA_MIN_Z &&
            unit->GetPositionZ() < AREA_MAX_Z)
            ++snapshot.wretchedAlive;

    // [selin engaged] Her fight is on once she fights a player of the group, or stays engaged with nobody to hit and
    // does not evade (inert) with her room clear: the room rules let go (no leash, no hold, no room pull, no rally,
    // `idle` stays empty), the members outside walk in ("mgt out of room") and the group fights her and the crystal
    // she channels. Inert with Wretched alive the room tactic goes on clearing them first (mgt-h build 25: the group
    // walked in on a pet victim with 6 Wretched alive and wiped).
    // [selin_ac_final_pull] No idle Wretched left and one of hers fighting (the centre pair pulled last, from the
    // stage spot), or Selin engaged with none idle: her fight is on now - the group steps in before the door shuts
    // (she joins 2 s after the pair), kills the pair, then her. Never waits for an evade (AzerothCore: none comes).
    uint32 idleWretched = 0;
    uint32 fightingWretched = 0;
    for (Unit* unit : wretched)
        if (unit->GetExactDist2d(SELIN_X, SELIN_Y) < AREA_RADIUS && unit->GetPositionZ() > AREA_MIN_Z &&
            unit->GetPositionZ() < AREA_MAX_Z)
            ++(unit->IsInCombat() ? fightingWretched : idleWretched);
    // build 29 (20261025-163739-mgt-n-5A-37, ~434 s): "no Wretched idle, one fighting" alone also held while the
    // LAST SIDE PACK was fought in the corridor (Selin idle): the fight was committed, the members walked off the
    // fight into the doorway. Only the deliberate final entry, or Selin herself engaged, commits it.
    bool const finalEngage = !idleWretched && fightingWretched &&
                             (FinalEntryActive(bot) || (snapshot.selinEngaged && !selin->IsInEvadeMode()));
    if (finalEngage ||
        (snapshot.selinEngaged && !selin->IsInEvadeMode() &&
         (snapshot.selinHasVictim || (snapshot.selinInert && !snapshot.wretchedAlive))))
    {
        snapshot.selinCommitted = true;
        if (!snapshot.committedLogMs || getMSTimeDiff(snapshot.committedLogMs, now) >= LOG_MS)
        {
            snapshot.committedLogMs = now;
            LOG_INFO("playerbots",
                     "[selin engaged] {} at ({:.1f}, {:.1f}): Selin fights {} with {} Wretched of her floor alive: "
                     "the room rules let go, everybody goes in and fights her",
                     bot->GetName(), bot->GetPositionX(), bot->GetPositionY(),
                     victimPlayer ? victimPlayer->GetName()
                                  : Acore::StringFormat("nobody for {} s (inert, not evading)",
                                                        getMSTimeDiff(snapshot.selinTargetlessMs, now) /
                                                            IN_MILLISECONDS),
                     snapshot.wretchedAlive);
        }
        return;
    }
    snapshot.committedLogMs = 0;

    // One pack per formation (a lone Wretched is its own pack).
    std::vector<void const*> keys;
    std::vector<std::pair<Creature*, size_t>> members;
    for (Unit* unit : wretched)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || creature->GetExactDist2d(SELIN_X, SELIN_Y) > ROOM_RADIUS)
            continue;

        CreatureGroup* formation = creature->GetFormation();
        void const* key = formation ? static_cast<void const*>(formation) : static_cast<void const*>(creature);
        auto const found = std::find(keys.begin(), keys.end(), key);
        size_t const index = static_cast<size_t>(std::distance(keys.begin(), found));
        if (found == keys.end())
        {
            keys.push_back(key);
            snapshot.packs.emplace_back();
        }

        Pack& pack = snapshot.packs[index];
        pack.members.push_back(creature->GetGUID());
        if (creature->IsInCombat())
            pack.fighting = true;

        if (creature->GetExactDist(selin) <
            ASSIST_RADIUS + creature->GetObjectSize() + selin->GetObjectSize() + LINK_MARGIN)
            pack.linked = true;

        members.emplace_back(creature, index);
    }

    for (auto const& [creature, index] : members)
    {
        if (snapshot.packs[index].fighting)
        {
            snapshot.fighting = true;
            continue;
        }

        IdleMob mob;
        mob.guid = creature->GetGUID();
        mob.x = creature->GetPositionX();
        mob.y = creature->GetPositionY();
        mob.z = creature->GetPositionZ();
        mob.aggro = creature->GetAggroRange(bot);
        mob.pack = static_cast<uint8>(index);
        snapshot.idle.push_back(mob);

        // A wall corner is no cover this close (hiddenMinDist in SeenByIdle).
        constexpr float hiddenMinDist = 8.0f;
        float const dist = bot->GetExactDist2d(mob.x, mob.y);
        if (index < 32 && dist < mob.aggro + DRIFT_MARGIN &&
            (dist < hiddenMinDist || creature->IsWithinLOSInMap(bot, VMAP::ModelIgnoreFlags::M2)))
            snapshot.reachPacks |= 1u << index;
    }

    snapshot.active = (!snapshot.selinEngaged || snapshot.selinInert) && !snapshot.packs.empty();
    if (!snapshot.active)
        return;

    // [selin_ac_final_pull] every idle Wretched left is in a pack linked to her, nothing of the room fights
    snapshot.finalPull = !snapshot.selinEngaged && !snapshot.idle.empty() && !snapshot.fighting;
    for (IdleMob const& mob : snapshot.idle)
        if (!snapshot.packs[mob.pack].linked)
            snapshot.finalPull = false;

    // With Selin inert every member stays in her zone-wide combat: only a room pack's fight counts then.
    if (!snapshot.fighting && !snapshot.selinEngaged)
        snapshot.fighting = GroupInCombat(bot);

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get())
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature == selin ||
            IsRoomWretched(creature->GetEntry()) || creature->GetEntry() == NPC_FEL_CRYSTAL)
            continue;

        if (creature->GetExactDist2d(RALLY_X, RALLY_Y) < OTHER_TRASH_RADIUS &&
            std::fabs(creature->GetPositionZ() - RALLY_Z) < OTHER_TRASH_DZ)
        {
            snapshot.otherTrash = true;
            break;
        }
    }
}

// How far (x, y) stands outside the aggro range of the nearest idle room mob (the pack `ignorePack` left out).
float IdleMargin(Snapshot const& snapshot, float x, float y, int16 ignorePack)
{
    float margin = std::numeric_limits<float>::max();
    for (IdleMob const& mob : snapshot.idle)
    {
        if (mob.pack == ignorePack)
            continue;

        margin = std::min(margin, std::hypot(mob.x - x, mob.y - y) - mob.aggro);
    }
    return margin;
}

// The pull spell's reach: Hand of Reckoning for a paladin (PaladinPullStrategy casts it for a single-mob pull, the
// one this tactic asks for: Avenger's Shield jumps 12.5 yd + reaches and could bounce out of the pack), else the
// class's pull spell.
float PullRange(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    std::string spell;
    if (bot->getClass() == CLASS_PALADIN)
        spell = "hand of reckoning";
    else if (PullStrategy* strategy = PullStrategy::Get(botAI))
        spell = strategy->GetSpellName();

    uint32 const id =
        spell.empty() ? 0 : botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    if (SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr)
        return info->GetMaxRange(false, bot);

    return botAI->GetRange("spell");
}

// The floor height of every grid spot (static geometry: computed once per map thread). NaN = no floor there.
std::vector<float> const& FloorGrid(Player* bot)
{
    thread_local std::vector<float> grid;
    if (!grid.empty())
        return grid;

    std::vector<float> heights(static_cast<size_t>(SPOT_COLUMNS * SPOT_ROWS), std::numeric_limits<float>::quiet_NaN());
    bool anyFloor = false;
    Map* map = bot->GetMap();
    for (int32 column = 0; column < SPOT_COLUMNS; ++column)
    {
        for (int32 row = 0; row < SPOT_ROWS; ++row)
        {
            float const x = SPOT_MIN_X + column * SPOT_STEP;
            float const y = SPOT_MIN_Y + row * SPOT_STEP;
            float const z = map->GetHeight(bot->GetPhaseMask(), x, y, FLOOR_PROBE_Z, true, FLOOR_SEARCH);
            if (z >= FLOOR_MIN_Z && z <= FLOOR_MAX_Z)
            {
                heights[static_cast<size_t>(column * SPOT_ROWS + row)] = z;
                anyFloor = true;
            }
        }
    }

    // No floor at all: the terrain was not loaded yet; the next plan probes again.
    if (!anyFloor)
    {
        thread_local std::vector<float> none;
        none = std::move(heights);
        return none;
    }

    grid = std::move(heights);
    return grid;
}

// Why the last pull plan of this thread found no spot (logged with "no pull spot").
struct SpotStats
{
    uint32 candidates = 0;  // floor spots near the door within the pull range
    uint32 targetLos = 0;   // the target not in sight
    uint32 seen = 0;        // an idle mob of another pack sees the spot within its aggro range + SPOT_MARGIN
    uint32 path = 0;        // no full path from the rally
    uint32 walk = 0;        // an idle mob of another pack sees the walk within its aggro range + WALK_MARGIN
    uint32 budget = 0;      // new line-of-sight probes or paths used up (the next plan goes on from the cache)
    uint32 rays = 0;        // new line-of-sight probes cast by this plan
    float range = 0.0f;
};

SpotStats& LastSpotStats()
{
    thread_local SpotStats stats;
    return stats;
}

// The room is static and the idle Wretched stand still: line of sight and paths are cached across plans (shared by
// the map threads, which take turns on a map), so each plan only pays for what it has not looked at yet (wave 29:
// 8909 spots in range, the budget ran out after 500 probes far from the door).
int32 Quantize(float value) { return static_cast<int32>(std::lround(value * 4.0f)); }

using RayKey = std::tuple<uint64, int32, int32, int32>;

std::mutex& CacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<RayKey, bool>& RaySeen()
{
    static std::map<RayKey, bool> seen;
    return seen;
}

// New probes the running plan may still cast.
uint32& RayBudget()
{
    thread_local uint32 budget = 0;
    return budget;
}

// `from` (a unit's guid and position) sees (x, y, z), eye to eye; the spell's own test (Spell::CheckCast through
// WorldObject::IsWithinLOSInMap ignores M2 doodads). False with `known` false when the probe budget is used up.
bool CachedLos(Map* map, uint32 phase, ObjectGuid from, float fx, float fy, float fz, float x, float y, float z,
               bool& known)
{
    constexpr size_t maxEntries = 200000;

    RayKey const key(from.GetRawValue(), Quantize(x), Quantize(y), Quantize(z));
    {
        std::lock_guard<std::mutex> lock(CacheMutex());
        auto const found = RaySeen().find(key);
        if (found != RaySeen().end())
        {
            known = true;
            return found->second;
        }
    }

    uint32& budget = RayBudget();
    if (!budget)
    {
        known = false;
        return false;
    }

    --budget;
    ++LastSpotStats().rays;
    bool const visible = map->isInLineOfSight(fx, fy, fz + EYE_HEIGHT, x, y, z + EYE_HEIGHT, phase,
                                              LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2);
    {
        std::lock_guard<std::mutex> lock(CacheMutex());
        if (RaySeen().size() > maxEntries)
            RaySeen().clear();

        RaySeen().emplace(key, visible);
    }
    known = true;
    return visible;
}

// Creature::CanStartAttack needs line of sight: an idle mob within its aggro range of the spot only counts when it
// sees it. The corridor walk from the rally round the stairs passes 17 yd from the side packs behind the wall
// (navmesh 585 route rally -> door: (204.8,-18.6), (210.3,-16.1)); counting them regardless refused every path
// (wave 29: "no pull spot" with 14 idle). A mob closer than hiddenMinDist counts anyway (a wall corner is no cover a
// step later), so does one whose probe did not fit in the budget.
bool SeenByIdle(Map* map, uint32 phase, Snapshot const& snapshot, float x, float y, float z, float needed,
                int16 ignorePack, bool& outOfBudget)
{
    constexpr float hiddenMinDist = 8.0f;

    for (IdleMob const& mob : snapshot.idle)
    {
        if (mob.pack == ignorePack)
            continue;

        float const dist = std::hypot(mob.x - x, mob.y - y);
        if (dist - mob.aggro >= needed)
            continue;

        if (dist < hiddenMinDist)
            return true;

        bool known = false;
        bool const visible = CachedLos(map, phase, mob.guid, mob.x, mob.y, mob.z, x, y, z, known);
        if (!known)
        {
            outOfBudget = true;
            return true;
        }

        if (visible)
            return true;
    }
    return false;
}

// The walk keeps WALK_MARGIN outside the reach of every idle mob that sees it, the pulled pack left out.
bool WalkSafe(Map* map, uint32 phase, Movement::PointsArray const& points, Snapshot const& snapshot,
              int16 ignorePack, bool& outOfBudget)
{
    for (size_t i = 1; i < points.size(); ++i)
    {
        G3D::Vector3 const& from = points[i - 1];
        G3D::Vector3 const& to = points[i];
        float const length = std::hypot(to.x - from.x, to.y - from.y);
        uint32 const steps = std::max<uint32>(1, static_cast<uint32>(std::ceil(length / WALK_STEP)));
        for (uint32 step = 0; step <= steps; ++step)
        {
            float const share = static_cast<float>(step) / static_cast<float>(steps);
            float const x = from.x + (to.x - from.x) * share;
            float const y = from.y + (to.y - from.y) * share;
            float const z = from.z + (to.z - from.z) * share;
            if (IdleMargin(snapshot, x, y, ignorePack) < WALK_MARGIN &&
                SeenByIdle(map, phase, snapshot, x, y, z, WALK_MARGIN, ignorePack, outOfBudget))
                return false;
        }
    }
    return true;
}

// The path from the rally to a grid spot (static geometry: cached for good, a few hundred spots at most). Not ok
// when there is none.
struct CachedPath
{
    bool ok = false;
    Movement::PointsArray points;
};

CachedPath const* PathFromRally(Player* bot, float x, float y, float z, uint32& pathBudget)
{
    static std::map<std::pair<int32, int32>, CachedPath> cache;
    std::pair<int32, int32> const key(Quantize(x), Quantize(y));
    {
        std::lock_guard<std::mutex> lock(CacheMutex());
        auto const found = cache.find(key);
        if (found != cache.end())
            return &found->second;
    }

    if (!pathBudget)
        return nullptr;

    --pathBudget;
    CachedPath entry;
    PathGenerator path(bot);
    path.CalculatePath(RALLY_X, RALLY_Y, RALLY_Z, x, y, z, false);
    G3D::Vector3 const& end = path.GetActualEndPosition();
    // The stairs' top in the corridor's middle (x 203-207) is a navmesh island: no path ends there.
    entry.ok = !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) &&
               std::hypot(end.x - x, end.y - y) <= PATH_END_TOLERANCE;
    if (entry.ok)
        entry.points = path.GetPath();

    // std::map nodes stay put: the entry may be read after the lock (never erased).
    std::lock_guard<std::mutex> lock(CacheMutex());
    return &cache.emplace(key, std::move(entry)).first->second;
}

// The best cast spot on `target`: it sees the target within the pull range, no idle mob of another pack sees it
// within its aggro range + SPOT_MARGIN, and the walk to it from the rally stays out of their sight too. Only the
// spots around the door are looked at (a corridor spot sees the room through the door alone), the clearest of the
// others' reach first, then the nearest to the door.
bool FindSpot(Player* bot, Unit* target, Snapshot const& snapshot, uint8 packIndex, float range, uint32& pathBudget,
              PullPlan& out)
{
    struct Candidate
    {
        float capped;
        float margin;
        float door;
        float x;
        float y;
        float z;
    };

    SpotStats& stats = LastSpotStats();
    stats.range = range;
    std::vector<float> const& floor = FloorGrid(bot);
    float const reach = range - RANGE_SLACK;
    std::vector<Candidate> candidates;
    for (int32 column = 0; column < SPOT_COLUMNS; ++column)
    {
        for (int32 row = 0; row < SPOT_ROWS; ++row)
        {
            float const z = floor[static_cast<size_t>(column * SPOT_ROWS + row)];
            if (std::isnan(z))
                continue;

            float const x = SPOT_MIN_X + column * SPOT_STEP;
            float const y = SPOT_MIN_Y + row * SPOT_STEP;
            float const door = std::hypot(x - DOOR_X, y - DOOR_Y);
            if (target->GetExactDist(x, y, z) > reach)
                continue;

            // [selin_ac_final_pull] A side pack (it comes alone, Selin stays idle on her Z-gated dais) may be pulled
            // from any floor spot of the grid, inside the room too: user (GM, 2026-10-25) - from the doorway and the
            // room's middle the other pack is in plain sight. The pair (it brings her) only from the corridor side
            // of the door, near it: the group steps in from there as she comes.
            bool const linkedTarget = snapshot.packs[packIndex].linked;
            if (linkedTarget && (door > DOOR_SEARCH_RADIUS || x > PULL_SPOT_MAX_X))
                continue;

            // Geometrically clear spots first; one inside a reach only behind a wall ranks as just clear enough.
            float const margin = IdleMargin(snapshot, x, y, packIndex);
            float const capped = std::clamp(margin, SPOT_MARGIN, MARGIN_CAP);
            candidates.push_back({capped, margin, door, x, y, z});
        }
    }
    stats.candidates += static_cast<uint32>(candidates.size());

    std::sort(candidates.begin(), candidates.end(),
              [](Candidate const& a, Candidate const& b)
              { return a.capped != b.capped ? a.capped > b.capped : a.door < b.door; });

    Map* map = bot->GetMap();
    uint32 const phase = bot->GetPhaseMask();
    for (Candidate const& candidate : candidates)
    {
        bool known = false;
        bool const sees = CachedLos(map, phase, target->GetGUID(), target->GetPositionX(), target->GetPositionY(),
                                    target->GetPositionZ(), candidate.x, candidate.y, candidate.z, known);
        if (!known)
        {
            ++stats.budget;
            return false;
        }

        if (!sees)
        {
            ++stats.targetLos;
            continue;
        }

        bool outOfBudget = false;
        if (candidate.margin < SPOT_MARGIN && SeenByIdle(map, phase, snapshot, candidate.x, candidate.y,
                                                         candidate.z, SPOT_MARGIN, packIndex, outOfBudget))
        {
            if (outOfBudget)
            {
                ++stats.budget;
                return false;
            }

            ++stats.seen;
            continue;
        }

        CachedPath const* path = PathFromRally(bot, candidate.x, candidate.y, candidate.z, pathBudget);
        if (!path)
        {
            ++stats.budget;
            return false;
        }

        if (!path->ok)
        {
            ++stats.path;
            continue;
        }

        if (!WalkSafe(map, phase, path->points, snapshot, packIndex, outOfBudget))
        {
            if (outOfBudget)
            {
                ++stats.budget;
                return false;
            }

            ++stats.walk;
            continue;
        }

        out.target = target->GetGUID();
        out.x = candidate.x;
        out.y = candidate.y;
        out.z = candidate.z;
        out.margin = candidate.margin;
        return true;
    }
    return false;
}

// The pair next to Selin first, then the side packs, nearest to the rally first.
bool ComputePlan(PlayerbotAI* botAI, Snapshot const& snapshot, PullPlan& plan)
{
    Player* bot = botAI->GetBot();
    float const range = PullRange(botAI);
    LastSpotStats() = SpotStats();

    std::vector<std::pair<float, uint8>> order;
    bool sideIdle = false;
    for (IdleMob const& mob : snapshot.idle)
        if (!snapshot.packs[mob.pack].linked)
            sideIdle = true;
    for (size_t index = 0; index < snapshot.packs.size(); ++index)
    {
        Pack const& pack = snapshot.packs[index];
        if (pack.fighting)
            continue;

        float nearest = std::numeric_limits<float>::max();
        for (IdleMob const& mob : snapshot.idle)
        {
            if (mob.pack == index)
                nearest = std::min(nearest, std::hypot(mob.x - RALLY_X, mob.y - RALLY_Y));
        }

        // The pair next to Selin first: a side pack's way to the rally passes 12-14 yd from it, and the pair's call
        // for assistance (10 yd + reaches, every 3 s while it fights) brought it into the side pack's fight, and
        // Selin 2 s after it (wave 29 11:45:26-31; she evaded 2 s later, nobody being in her room).
        // [selin_ac_final_pull] AzerothCore: the pair brings Selin and she never evades - the side packs come first,
        // the pair last (from the stage spot, the group ready), never while a side pack is idle.
        if (pack.linked && sideIdle)
            continue;
        order.emplace_back(nearest, static_cast<uint8>(index));
    }
    std::sort(order.begin(), order.end());

    RayBudget() = MAX_NEW_RAYS;
    uint32 pathBudget = MAX_NEW_PATHS;
    for (auto const& [key, index] : order)
    {
        std::vector<std::pair<float, Unit*>> targets;
        for (ObjectGuid const& guid : snapshot.packs[index].members)
        {
            Unit* member = botAI->GetUnit(guid);
            if (member && member->IsAlive() && !member->IsInCombat())
                targets.emplace_back(member->GetExactDist2d(DOOR_X, DOOR_Y), member);
        }
        std::sort(targets.begin(), targets.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        if (targets.size() > TARGETS_PER_PACK)
            targets.resize(TARGETS_PER_PACK);

        bool found = false;
        for (auto const& [doorDist, member] : targets)
        {
            PullPlan candidate;
            if (FindSpot(bot, member, snapshot, index, range, pathBudget, candidate) &&
                (!found || candidate.margin > plan.margin))
            {
                plan = candidate;
                found = true;
                // No member gives a better spot than one this far out of the others' reach.
                if (candidate.margin >= MARGIN_CAP)
                    break;
            }
        }

        if (found)
        {
            plan.pack = index;
            plan.linked = snapshot.packs[index].linked;
            return true;
        }
    }
    return false;
}

bool ThrottledLog(uint64 key)
{
    static std::mutex mutex;
    static std::unordered_map<uint64, uint32> last;
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> lock(mutex);
    uint32& at = last[key];
    if (at && getMSTimeDiff(at, now) < LOG_MS)
        return false;

    at = now;
    return true;
}

// [b38_mgt] No pull spot found while the next pull brings Selin anyway (heroic with the centre pair idle, or the final
// pull): the room keeps the pull as the final entry of the pair (the whole group walks in from the stage spot together)
// instead of handing it to the careful planner. mgt-h 20261026-050341-mgt-h-5G-15 (build 38, 262-327 s): "[selin room]
// no pull spot ... (14 idle; seen by another pack 1244)" released the pull, the planner's [careful_ready_last_resort]
// "[pull one]" took the centre pair's Wretched Skulker (231.3, 2.8) from the corridor with the group at the rally
// (200, 10): all 14 Wretched and Selin came, her room door 188065 shut on IN_PROGRESS with the three melee at x 212
// for the whole 5-minute attempt, and the tank alone fought her (boss_attempt_loop).
bool EntryFallbackPlan(PlayerbotAI* botAI, Snapshot const& snapshot, PullPlan& plan)
{
    if (!snapshot.active || snapshot.selinEngaged || snapshot.fighting)
        return false;

    if (!snapshot.finalPull && !HeroicEntryPull(botAI))
        return false;

    Unit* best = nullptr;
    uint8 bestPack = 0;
    float bestDist = 0.0f;
    for (IdleMob const& mob : snapshot.idle)
    {
        if (!snapshot.packs[mob.pack].linked)
            continue;

        Unit* unit = botAI->GetUnit(mob.guid);
        if (!unit || !unit->IsAlive() || unit->IsInCombat())
            continue;

        float const dist = std::hypot(mob.x - DOOR_X, mob.y - DOOR_Y);
        if (!best || dist < bestDist)
        {
            best = unit;
            bestPack = mob.pack;
            bestDist = dist;
        }
    }
    if (!best)
        return false;

    plan = PullPlan();
    plan.target = best->GetGUID();
    plan.pack = bestPack;
    plan.linked = true;
    plan.x = STAGE_X;
    plan.y = STAGE_Y;
    plan.z = STAGE_Z;
    plan.margin = 0.0f;

    Player* bot = botAI->GetBot();
    if (ThrottledLog(bot->GetGUID().GetRawValue() ^ 0xb38b38b38ull))
        LOG_INFO("playerbots",
                 "[b38_mgt] {}: no pull spot, the next pull brings Selin ({}): the room keeps it as the final entry of "
                 "{} - the whole group walks in from the stage spot, nobody left outside her door",
                 bot->GetName(), snapshot.finalPull ? "the final pull" : "heroic, the centre pair idle",
                 best->GetName());
    return true;
}

// [b42_mgt] Selin engaged with nobody to hit, a pack of hers idle, and the room making no progress (no Wretched fought,
// none died) for INERT_STALL_MS with the whole group at the rally: the group walks into her room together (the final
// entry, StartFinalEntry) and fights her there, the door shutting with everybody inside. mgt-n
// 20261026-071042-mgt-n-5C-14 (501-683 s): the planner's [pull boss] brought the south pack, the pair and Selin; the
// pack died at the rally, the north pack (6) stayed idle, no room pull came, [b41_mgt] kept the leash through the
// no-damage release and the run failed "no damage on Selin Fireheart for 180 s" with all five at (200, -3).
constexpr uint32 INERT_STALL_MS = 30 * IN_MILLISECONDS;
constexpr float INERT_STALL_GATHER = 15.0f;
void InertStallEntry(PlayerbotAI* botAI, Snapshot const& snapshot)
{
    struct Progress
    {
        uint32 sinceMs = 0;
        uint32 alive = 0;
    };
    static std::mutex progressLock;
    static std::unordered_map<uint32, Progress> progress;  // instance id -> the room's last progress

    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || snapshot.selin.IsEmpty() || !IsPullingTank(bot))
        return;

    uint32 const now = getMSTime();
    bool fightingPack = WretchedInFight(botAI);
    for (Pack const& pack : snapshot.packs)
        fightingPack = fightingPack || pack.fighting;
    bool const stalled = snapshot.selinEngaged && snapshot.selinTargetlessMs && !snapshot.selinCommitted &&
                         snapshot.selinPetVictim.IsEmpty() && snapshot.wretchedAlive && !fightingPack;
    uint32 waited = 0;
    {
        std::lock_guard<std::mutex> guard(progressLock);
        Progress& entry = progress[bot->GetInstanceId()];
        if (!stalled || entry.alive != snapshot.wretchedAlive || !entry.sinceMs)
        {
            entry.sinceMs = now ? now : 1;
            entry.alive = snapshot.wretchedAlive;
        }
        waited = getMSTimeDiff(entry.sinceMs, now);
    }
    // diagnostics: which room flag keeps the room pull ("mgt selin room pull inert") from running
    if (stalled && waited >= INERT_STALL_MS / 3 && ThrottledLog(bot->GetGUID().GetRawValue() ^ 0xb42b42b42ull))
        LOG_INFO("playerbots",
                 "[b42_mgt] room stall {}: {} s, active {}, inert {}, fighting {}, otherTrash {}, idle {}, packs {}, "
                 "alive {}, finalPull {}, inRoomFight {}",
                 bot->GetName(), waited / IN_MILLISECONDS, snapshot.active, snapshot.selinInert, snapshot.fighting,
                 snapshot.otherTrash, snapshot.idle.size(), snapshot.packs.size(), snapshot.wretchedAlive,
                 snapshot.finalPull, InRoomFight(botAI));
    if (!stalled || waited < INERT_STALL_MS || FinalEntryActive(bot))
        return;

    // everybody alive at the rally (or the stage spot), so nobody is left behind the door
    float idleY = 0.0f;
    for (IdleMob const& mob : snapshot.idle)
        idleY += mob.y;
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!DungeonInteract::IsActiveMember(bot, member) || !member->IsAlive())
                continue;
            if (member->GetExactDist2d(RALLY_X, RALLY_Y) > INERT_STALL_GATHER &&
                member->GetExactDist2d(STAGE_X, STAGE_Y) > INERT_STALL_GATHER)
                return;
        }

    // the fight spot on the side away from the idle pack
    float const entryY = idleY >= 0.0f ? -ENTRY_FIGHT_SIDE_Y : ENTRY_FIGHT_SIDE_Y;
    StartFinalEntry(bot, entryY);
    LOG_INFO("playerbots",
             "[b42_mgt] {}: Selin engaged with nobody to hit, {} Wretched of hers alive and no room progress for {} s: "
             "the group walks into her room together now, the fight spot ({:.1f}, {:.1f})",
             bot->GetName(), snapshot.wretchedAlive, waited / IN_MILLISECONDS, ENTRY_FIGHT_X, entryY);
}
}  // namespace

Snapshot const& Get(PlayerbotAI* botAI)
{
    // Map threads update their bots in parallel: one cache per thread, keyed by the bot.
    thread_local std::unordered_map<uint64, Snapshot> cache;
    Player* bot = botAI->GetBot();
    Snapshot& snapshot = cache[bot->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    if (snapshot.ms && getMSTimeDiff(snapshot.ms, now) < CACHE_MS)
        return snapshot;

    // [b41_mgt] A copy this thread has not refreshed for a while (the bot updated on other map threads, or in an
    // earlier run) carries a stale "engaged with no victim since" stamp: mgt-n 20261026-063902-mgt-n-5A-14 (462 s)
    // read "Selin fights nobody for 559 s (inert)" 2 s after her reset, in a 493 s run, and committed her fight.
    constexpr uint32 STALE_MS = 5 * IN_MILLISECONDS;
    if (!snapshot.ms || getMSTimeDiff(snapshot.ms, now) >= STALE_MS)
    {
        snapshot.selinTargetlessMs = 0;
        snapshot.committedLogMs = 0;
    }

    snapshot.ms = now;
    Compute(botAI, snapshot);
    InertStallEntry(botAI, snapshot);  // [b42_mgt]
    return snapshot;
}

bool HoldsGroup(PlayerbotAI* botAI)
{
    Snapshot const& snapshot = Get(botAI);
    return snapshot.active && !snapshot.fighting && !snapshot.otherTrash;
}

namespace
{
std::mutex& FinalEntryMutex()
{
    static std::mutex mutex;
    return mutex;
}

struct FinalEntry
{
    uint32 startMs = 0;
    float y = 0.0f;
};

std::unordered_map<uint32, FinalEntry>& FinalEntries()  // instance id -> the running entry
{
    static std::unordered_map<uint32, FinalEntry> entries;
    return entries;
}
}  // namespace

void StartFinalEntry(Player* tank, float entryY)
{
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> lock(FinalEntryMutex());
    FinalEntry& entry = FinalEntries()[tank->GetInstanceId()];
    entry.startMs = now ? now : 1;
    entry.y = entryY;
}

bool FinalEntryActive(Player* bot, float* entryY)
{
    if (bot->GetMapId() != MAP_MAGISTERS_TERRACE)
        return false;

    std::lock_guard<std::mutex> lock(FinalEntryMutex());
    auto const found = FinalEntries().find(bot->GetInstanceId());
    if (found == FinalEntries().end() || getMSTimeDiff(found->second.startMs, getMSTime()) >= FINAL_ENTRY_MS)
        return false;
    if (entryY)
        *entryY = found->second.y;
    return true;
}

bool HeroicEntryPull(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Map* map = bot->GetMap();
    if (!map || !map->IsHeroic())
        return false;

    Snapshot const& snapshot = Get(botAI);
    if (!snapshot.active || snapshot.selinEngaged || snapshot.fighting || snapshot.finalPull)
        return false;

    bool linkedIdle = false;
    for (IdleMob const& mob : snapshot.idle)
        if (snapshot.packs[mob.pack].linked)
            linkedIdle = true;
    return linkedIdle;
}

void GatherPoint(PlayerbotAI* botAI, float& x, float& y, float& z)
{
    bool const stage = Get(botAI).finalPull || HeroicEntryPull(botAI);
    x = stage ? STAGE_X : RALLY_X;
    y = stage ? STAGE_Y : RALLY_Y;
    z = stage ? STAGE_Z : RALLY_Z;
}

bool OwnsPull(PlayerbotAI* botAI)
{
    if (!HoldsGroup(botAI))
        return false;

    Player* bot = botAI->GetBot();
    if (!IsPullingTank(bot))
        return true;

    PullPlan plan;
    if (GetPullPlan(botAI, plan))
        return true;

    if (ThrottledLog(bot->GetGUID().GetRawValue()))
    {
        SpotStats const& stats = LastSpotStats();
        LOG_INFO("playerbots",
                 "[selin room] {}: no pull spot sees an idle pack out of the others' reach ({} idle; range {:.1f}, "
                 "{} spots in range, target not in sight {}, seen by another pack {}, no path {}, walk seen {}, "
                 "budget {}, new rays {})",
                 bot->GetName(), Get(botAI).idle.size(), stats.range, stats.candidates, stats.targetLos, stats.seen,
                 stats.path, stats.walk, stats.budget, stats.rays);
    }

    // No spot for a careful pull: the planner's guarantee pulls the nearest idle Wretched as it stands, with the
    // tank walking in (the request lives 5 s, renewed every tick while this holds).
    // [selin_ac_final_pull] a side pack's mob first: the pair (it brings Selin) only once no side pack is idle
    Snapshot const& room = Get(botAI);
    IdleMob const* nearest = nullptr;
    float nearestDist = 0.0f;
    bool nearestLinked = true;
    for (IdleMob const& mob : room.idle)
    {
        float const dist = bot->GetExactDist(mob.x, mob.y, mob.z);
        bool const linked = room.packs[mob.pack].linked;
        if (!nearest || (nearestLinked && !linked) || (linked == nearestLinked && dist < nearestDist))
        {
            nearest = &mob;
            nearestDist = dist;
            nearestLinked = linked;
        }
    }
    if (nearest)
        GuaranteeRequest::Set(bot->GetGUID(), nearest->guid, true);

    return false;
}

bool IsPullingTank(Player* bot)
{
    if (!PlayerbotAI::IsTank(bot))
        return false;

    if (PlayerbotAI::IsMainTank(bot))
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member != bot && DungeonInteract::IsActiveMember(bot, member) && PlayerbotAI::IsMainTank(member))
            return false;
    }
    return true;
}

bool GetPullPlan(PlayerbotAI* botAI, PullPlan& plan)
{
    struct Cached
    {
        uint32 ms = 0;
        bool valid = false;
        PullPlan plan;
    };

    if (!HoldsGroup(botAI))
        return false;

    Player* bot = botAI->GetBot();
    if (!IsPullingTank(bot))
        return false;

    thread_local std::unordered_map<uint64, Cached> cache;
    Cached& cached = cache[bot->GetGUID().GetRawValue()];
    uint32 const now = getMSTime();
    if (!cached.ms || getMSTimeDiff(cached.ms, now) >= PLAN_MS)
    {
        cached.ms = now;
        cached.plan = PullPlan();
        cached.valid = ComputePlan(botAI, Get(botAI), cached.plan);
        if (!cached.valid)  // [b38_mgt] the pull that brings Selin stays the room's: the group walks in with it
        {
            cached.plan = PullPlan();
            cached.valid = EntryFallbackPlan(botAI, Get(botAI), cached.plan);
        }
    }

    if (!cached.valid)
        return false;

    // The planned mob was pulled or died in the meantime.
    Unit* target = botAI->GetUnit(cached.plan.target);
    if (!target || !target->IsAlive() || target->IsInCombat())
        return false;

    plan = cached.plan;
    return true;
}

bool InIdleReach(Snapshot const& snapshot, float x, float y, float margin, int16 ignorePack)
{
    return IdleMargin(snapshot, x, y, ignorePack) < margin;
}

bool ApproachUnsafe(PlayerbotAI* botAI, Unit* target)
{
    if (!target)
        return false;

    // Idle packs are kept out of the fight whether Selin is idle or not.
    Snapshot const& snapshot = Get(botAI);
    if (snapshot.idle.empty())
        return false;

    Player* bot = botAI->GetBot();
    float const reach = PlayerbotAI::IsRanged(bot) ? RANGED_STAND : MELEE_STAND;
    float const dist = bot->GetExactDist2d(target);
    if (dist <= reach)
        return false;

    // Where the walk to the target ends: on the line to it, `reach` short of it.
    float const share = reach / dist;
    float const x = target->GetPositionX() + (bot->GetPositionX() - target->GetPositionX()) * share;
    float const y = target->GetPositionY() + (bot->GetPositionY() - target->GetPositionY()) * share;
    return InIdleReach(snapshot, x, y, WALK_MARGIN);
}

bool InRoomFight(PlayerbotAI* botAI)
{
    if (!botAI->GetBot()->IsInCombat())
        return false;

    return !Get(botAI).selinInert || WretchedInFight(botAI);
}

int16 PackOf(Snapshot const& snapshot, ObjectGuid member)
{
    for (size_t index = 0; index < snapshot.packs.size(); ++index)
    {
        std::vector<ObjectGuid> const& members = snapshot.packs[index].members;
        if (std::find(members.begin(), members.end(), member) != members.end())
            return static_cast<int16>(index);
    }
    return -1;
}

uint32 CountLowWretched(PlayerbotAI* botAI, float healthPct)
{
    uint32 low = 0;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (!IsRoomWretched(guid.GetEntry()))
            continue;

        Unit* mob = botAI->GetUnit(guid);
        if (mob && mob->IsAlive() && mob->GetHealthPct() <= healthPct)
            ++low;
    }
    return low;
}

bool ClearingRoom(PlayerbotAI* botAI)
{
    Snapshot const& snapshot = Get(botAI);
    return !snapshot.selin.IsEmpty() && snapshot.wretchedAlive && !snapshot.selinCommitted;
}

bool CrossesLeash(PlayerbotAI* botAI, Unit* target)
{
    if (!target || !ClearingRoom(botAI))
        return false;

    Player* bot = botAI->GetBot();
    float const reach = PlayerbotAI::IsRanged(bot) ? RANGED_STAND : MELEE_STAND;
    float const dist = bot->GetExactDist2d(target);
    if (dist <= reach)
        return bot->GetPositionX() > LEASH_EDGE_X;

    // Where the walk to the target ends: on the line to it, `reach` short of it.
    float const share = reach / dist;
    float const x = target->GetPositionX() + (bot->GetPositionX() - target->GetPositionX()) * share;
    return x > LEASH_EDGE_X;
}

bool EntryHolds(Player* bot, Player** outside)
{
    if (outside)
        *outside = nullptr;

    if (bot->GetMapId() != MAP_MAGISTERS_TERRACE || !InArea(bot))
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    Snapshot const& snapshot = Get(botAI);
    if (snapshot.selin.IsEmpty() || snapshot.selinEngaged || snapshot.wretchedAlive)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!DungeonInteract::IsActiveMember(bot, member) || member->GetPositionX() >= SELIN_SAFE_X)
            continue;

        if (outside)
            *outside = member;
        return true;
    }
    return false;
}

bool KeepsPetsOut(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != MAP_MAGISTERS_TERRACE)
        return false;

    Snapshot const& snapshot = Get(botAI);
    return ClearingRoom(botAI) || (!snapshot.selinCommitted && !snapshot.selinPetVictim.IsEmpty());
}

void AppendPetExclusions(PlayerbotAI* botAI, GuidSet& exclusions)
{
    if (!KeepsPetsOut(botAI))
        return;

    Snapshot const& snapshot = Get(botAI);
    exclusions.insert(snapshot.selin);
    for (Pack const& pack : snapshot.packs)
    {
        for (ObjectGuid const& guid : pack.members)
        {
            Unit* mob = botAI->GetUnit(guid);
            if (mob && mob->GetPositionX() > LEASH_EDGE_X - PET_DOOR_MARGIN)
                exclusions.insert(guid);
        }
    }
}

Creature* PetToCallBack(PlayerbotAI* botAI)
{
    if (!KeepsPetsOut(botAI))
        return nullptr;

    Player* bot = botAI->GetBot();
    Snapshot const& snapshot = Get(botAI);
    for (Unit* controlled : bot->m_Controlled)
    {
        Creature* pet = controlled ? controlled->ToCreature() : nullptr;
        if (!pet || !pet->IsInWorld() || !pet->IsAlive() || pet->IsTotem() || !pet->GetCharmInfo() ||
            pet->GetMap() != bot->GetMap())
            continue;

        if (pet->GetGUID() == snapshot.selinPetVictim || pet->GetPositionX() > LEASH_EDGE_X)
            return pet;
    }
    return nullptr;
}
}  // namespace SelinRoom

bool IsDrinkingWretched(Unit* unit)
{
    return unit && SelinRoom::IsRoomWretched(unit->GetEntry()) && unit->HasAura(SPELL_DRINK_FEL_INFUSION);
}

Unit* GetWretchedFocus(Player* bot, ObjectGuid& latched)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

    Unit* drinking = nullptr;
    Unit* held = nullptr;
    Unit* lowest = nullptr;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (!SelinRoom::IsRoomWretched(guid.GetEntry()))
            continue;

        Unit* mob = botAI->GetUnit(guid);
        // Never the mob someone holds in crowd control: hitting it breaks the CC.
        if (!mob || !mob->IsAlive() || IsCrowdControlTarget(bot, mob) || bot->GetExactDist2d(mob) > FOCUS_SCAN)
            continue;

        if (mob->HasAura(SPELL_DRINK_FEL_INFUSION) &&
            (!drinking || mob->GetHealthPct() < drinking->GetHealthPct()))
            drinking = mob;

        if (mob->GetGUID() == latched)
            held = mob;

        if (!lowest || mob->GetHealthPct() < lowest->GetHealthPct())
            lowest = mob;
    }

    // A drinking one dies first (it is below 20 % already); else the one the group is on, else the lowest.
    Unit* best = drinking ? drinking : (held ? held : lowest);
    latched = best ? best->GetGUID() : ObjectGuid::Empty;
    return best;
}
}
