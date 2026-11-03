/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonTactics.h"
#include "AttackersValue.h"
#include "Creature.h"
#include "DungeonHoldFire.h"
#include "Group.h"
#include "KillOrder.h"
#include "Map.h"
#include "MgTShared.h"
#include "MovementActions.h"
#include "ObjectDefines.h"
#include "Playerbots.h"
#include "ShamanActions.h"
#include "TargetValue.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DungeonTactics
{
namespace
{
enum DungeonTacticsIds : uint32
{
    MAP_GNOMEREGAN = 90,
    MAP_BLACKROCK_SPIRE = 229,
    MAP_ARCATRAZ = 552,
    MAP_MAGISTERS_TERRACE = 585,
    MAP_GUNDRAK = 604,
    MAP_HALLS_OF_LIGHTNING = 602,
    MAP_SCHOLOMANCE = 289,
    MAP_STEAMVAULT = 545,
    MAP_OCULUS = 578,
    MAP_AHNKAHET = 619,
    MAP_STRATHOLME = 329,

    NPC_SELIN_FIREHEART = 24723,
    NPC_ARCATRAZ_SENTINEL = 20869,
    NPC_ARCATRAZ_SENTINEL_HEROIC = 21586,
    NPC_DRAKKARI_LANCER = 29819,
    NPC_DRAKKARI_LANCER_HEROIC = 30932,
    NPC_ROOKERY_HATCHER = 10683,
    NPC_ROOKERY_HATCHER_HEROIC = 710683,
    NPC_CAVERNDEEP_REAVER = 6211,
    NPC_CAVERNDEEP_REAVER_HEROIC = 706211,
    // Gundrak, Slad'ran (boss_slad_ran.cpp): Vipers from 90 % every 8 s, Constrictors from 50 / 75 % every 3-5 s.
    NPC_SLADRAN_VIPER = 29680,
    NPC_SLADRAN_CONSTRICTOR = 29713,
    // Halls of Lightning: Blistering Steamrager's hits carry Scald 52530 / 59140 (+925-1075 / 2775-3225 fire per
    // hit) and Steam Blast 52531 / 59141 is a 15 yd frontal cone (wave 10: it killed the healer at Volkhan).
    NPC_BLISTERING_STEAMRAGER = 28583,
    // Scholomance (SmartAI): the Acolyte's Mana Burn 17615 (instant, no kick) picks a random mana user within 20 yd
    // of it and it heals with Dark Mending 17613; the Spectral Researcher's Wail of Souls 17631 (instant, 30 yd
    // around it) and Mana Burn 17630 (random mana user within 30 yd). Wave 24: 951 + 766 burned the healer dry in
    // 15 s. Neither can be kicked: they die first.
    NPC_SCHOLOMANCE_ACOLYTE = 10471,
    NPC_SPECTRAL_RESEARCHER = 10499,
    // [b42_scholo] The Dark Summoner (SmartAI 11582) casts Summon Risen Lackey 17618 every 5-7 s in combat. It keeps
    // its range, so it stands out by the packs. scholo-h 20261026-071042-scholo-h-5E-3 pull 9 (guarantee: 7 mobs,
    // 21 with the summons): two Summoners at the Necrolyte balcony (266-268, 85-91, z 110) summoned for 80 s; a
    // Lackey came out on the Aberration room's floor below ("[far engage] Risen Lackey (0) at 257.0 101.1 95.9"),
    // the room came with it (Aberrations, a Ghoul, a Corpse: 26-40 attackers), the priest, the paladin and the
    // lock died. It dies first: each one alive adds a mob every few seconds.
    NPC_SCHOLOMANCE_DARK_SUMMONER = 11582,
    // [b48_scholo] Summon Risen Lackey 17618 (2 s cast, Spell.dbc InterruptFlags 9, PreventionType 1: kickable)
    // targets TARGET_DEST_NEARBY_ENTRY with no conditions row, so Spell::SelectImplicitNearbyTargets puts the Lackey
    // on the nearest unit in 2D within its range (WorldObjectSpellNearbyTargetCheck: GetDistance2d) - from the
    // Necrolyte gallery (z 110) that can be a Risen Aberration on the pit floor 14 yd below: Dark Summoner 48791
    // stands 0.8 yd (2D) over Aberration 48810. The Lackey's SmartAI attacks the nearest player within 30 yd out of
    // combat (no Z gate) and its CallAssistance wakes the pit. scholo-h 20261026-111724-scholo-h-5C-4 (pull 10):
    // "[far engage] Risen Lackey (0) at 252.9 98.7 95.8 engaged Rtwarab", then a Corpse, three Aberrations and a
    // Ghoul at dz 14.1; all five died to Risen Aberration (443-453 s). The cast is kicked.
    SPELL_DARK_SUMMONER_SUMMON_RISEN_LACKEY = 17618,
    // [b47_plan] Blackfathom Deeps, Twilight Shadowmage 4813 (SmartAI 4813): Dominate Mind 7645 (2 s cast every
    // 15-30 s, Spell.dbc InterruptFlags 13: kickable) turns a member on the group; Shadow Bolt 9613 stands still
    // (cast flag 64). bfd-n 20261026-093620-bfd-n-5B-19 (3493-3710 s): the tank, the paladin and the priest died to
    // it one by one over 60 s. It dies first in the group's fight, and its Dominate Mind is kicked.
    MAP_BLACKFATHOM_DEEPS = 48,
    NPC_TWILIGHT_SHADOWMAGE = 4813,
    SPELL_TWILIGHT_SHADOWMAGE_DOMINATE_MIND = 7645,
    // The Oculus, Ring-Lord packs on Varos' rings (SmartAI 27639 / 27640, RESEARCH_TACTICS.md section 13): the
    // Sorceress' Blizzard 50715 / 59278 (8 yd, picked on ranged and healers) and Flamestrike 16102 / 61402 deal the
    // group damage, the Conjurer's Charged Skin 50717 / 59276 pulses 760 arcane on whoever stands within 5 yd, the
    // Centrifuge Construct (formation leader) only hits its tank: Sorceress, then Conjurer, then Construct.
    NPC_RING_LORD_SORCERESS = 27639,
    NPC_RING_LORD_CONJURER = 27640,
    // Ahn'kahet (SmartAI 30278 / 30276, section 8): the Spell Flinger's Shadow Blast takes half of its target's
    // health (heroic 80 %), the Web Winder's Web Grab pulls anyone in 40 yd into the pack: Flinger, then Winder, then
    // the Swarmers (AoE). The Deep Crawler's Fatal Sting (59109, 4.7-5.2k + a DoT) only goes on a target below
    // 20 % health (SmartAI 30279): it dies third, before the Slasher.
    NPC_AHNKAHAR_SPELL_FLINGER = 30278,
    NPC_AHNKAHAR_WEB_WINDER = 30276,
    NPC_DEEP_CRAWLER = 30279,
    // [b33_ok] Ahn'kahet heroic, Amanitar's bog: a Savage Cave Beast (SmartAI 30329, wanders 5 yd at 370 / -847,
    // inside the mushroom field) joins her fight. It has a third of her health and hits the Mini-ed tank hard
    // (ok-h 20261026-000434-ok-h-5C-36: alive for the whole 212 s, it killed all five right after she died).
    NPC_SAVAGE_CAVE_BEAST = 30329,
    NPC_AMANITAR = 30258,
    // The Steamvault (SmartAI 17803 / 17801, section 12): the Coilfang Oracle heals an ally below 40 % (Heal 22883 /
    // 31730, 3.5 s), the Siren fears everyone in 10 yd (38660): Oracle, then Siren, then the rest.
    NPC_COILFANG_ORACLE = 17803,
    NPC_COILFANG_SIREN = 17801,
    // Stratholme, the Scarlet side streets (SmartAI 10405 / 10390 / 10391, RESEARCH_TACTICS.md section 11): the Plague
    // Ghoul (elite, 8 waypoint patrols) enrages at 30 % (8599) and cleaves, the Skeletal Guardian casts Shadow Bolt,
    // Frostbolt, Blizzard or Arcane Explosion by a random phase: Ghoul, then Guardian, then the Cadavers and the
    // Berserkers. The Berserker (Cleave 15496) and the Ghoul (Cleave 40505) cleave: a melee chain jumps 10 yd from the
    // victim to units in the caster's front 180 degrees (Spell::SearchChainTargets).
    NPC_PLAGUE_GHOUL = 10405,
    NPC_SKELETAL_GUARDIAN = 10390,
    NPC_SKELETAL_BERSERKER = 10391,
    // [b35_gnomer] Gnomeregan: the Mobile Alert System (SmartAI 7849, 5 spawns, 3 on patrols, HealthModifier 0.1) drops
    // its combat movement on reset and, in combat, summons a Leprous Assistant 7603 and a Leprous Defender 6223
    // every 10 s (smart_scripts id 3-5, TEMPSUMMON 4 / 30 s) for as long as it lives. gnomer-n 5B 20261026-024524,
    // 2209-2302 s: the patrol (guid 30141) walked into a pull at -790 / 418, stood 35-39 yd from the tank after the
    // fight moved back, every damage dealer skipped it ("[dps range] skips Mobile Alert System"), attackers 2 -> 27,
    // full wipe. It dies first, wherever it stands: one or two hits kill it.
    NPC_MOBILE_ALERT_SYSTEM = 7849,
    NPC_MOBILE_ALERT_SYSTEM_HEROIC = 707849,

    // Halls of Reflection escape, Lumbering Abomination 37069 / 37549 (npc_hor_lumbering_abomination,
    // halls_of_reflection.cpp:2052-2093): Vomit Spray 70176 / 70181 (10 yd TARGET_UNIT_CONE_ENEMY_24, spell_cone 60
    // degrees for 70176) and Cleave 40505 on every melee swing (chain 2: jumps 10 yd from the victim into the front
    // 180 degrees). Both reach melee distance + 10 yd in front.
    MAP_HALLS_OF_REFLECTION_ESCAPE = 668,
    NPC_LUMBERING_ABOMINATION = 37069,
    NPC_LUMBERING_ABOMINATION_HEROIC = 37549,

    // Drakkari Lancer (SmartAI): a proc aura, every melee hit it takes triggers Retaliation 22858 (wave 10: 10-12k on
    // the Enhancement shaman each time).
    SPELL_LANCER_RETALIATION = 40546,
};

// Selin Fireheart cannot attack anybody at x <= 216 (boss_selin_fireheart.cpp CanAIAttack); her 14 Wretched stand
// in three linked packs 11-28 yd from her. The rally spot is the corridor before her door.
constexpr std::array<BossRoom, 1> BOSS_ROOMS = {{
    {MAP_MAGISTERS_TERRACE, NPC_SELIN_FIREHEART, 242.1f, 0.3f, 40.0f, 30.0f, 200.0f, -3.0f, -3.0f},
}};

struct MeleeHoldSpec
{
    uint32 mapId;
    uint32 entry;
    uint32 aura;          // 0 = always
    bool whileRangedDps;  // only while a ranged damage dealer can kill it instead
};

// Arcatraz Sentinel: Energy Discharge pulses 563-938 arcane every second over 15 yd, on top of its melee; a melee
// leaving at 50 % health died anyway (waves 7-9).
constexpr std::array<MeleeHoldSpec, 4> MELEE_HOLDS = {{
    {MAP_ARCATRAZ, NPC_ARCATRAZ_SENTINEL, 0, true},
    {MAP_ARCATRAZ, NPC_ARCATRAZ_SENTINEL_HEROIC, 0, true},
    {MAP_GUNDRAK, NPC_DRAKKARI_LANCER, SPELL_LANCER_RETALIATION, false},
    {MAP_GUNDRAK, NPC_DRAKKARI_LANCER_HEROIC, SPELL_LANCER_RETALIATION, false},
}};

using KillFirstCondition = bool (*)(Unit* unit, Player* bot);

struct KillFirstSpec
{
    uint32 mapId;
    uint32 entry;
    KillFirstCondition condition = nullptr;  // nullptr = always
    // Killed wherever it is (a Hatcher walks to the eggs, away from the tank); otherwise only in the group's fight
    // (KillOrder::IsInGroupFight): Scholomance wave 29 shot a Researcher 50 yd from the tank and drew two packs.
    bool anywhere = false;
};

bool VictimIsHealer(Unit* unit, Player* /*bot*/)
{
    Player* victim = unit->GetVictim() ? unit->GetVictim()->ToPlayer() : nullptr;
    return victim && PlayerbotAI::IsHeal(victim);
}

bool VictimIsNotTank(Unit* unit, Player* /*bot*/)
{
    Player* victim = unit->GetVictim() ? unit->GetVictim()->ToPlayer() : nullptr;
    return victim && !PlayerbotAI::IsTank(victim);
}

// A healer of the bot's group stands within `reach` of the unit (the SmartAI target pick: IsWithinCombatRange).
bool HealerWithin(Unit* unit, Player* bot, float reach)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->IsAlive() && member->GetMap() == bot->GetMap() && PlayerbotAI::IsHeal(member) &&
            unit->IsWithinCombatRange(member, reach))
            return true;
    }

    return false;
}

// The tank holds it (it dies first among the pack), or it burns a healer's mana from where it stands.
bool ManaBurnerThreat(Unit* unit, Player* bot, float reach)
{
    return KillOrder::IsHeldByTank(unit, bot) || HealerWithin(unit, bot, reach);
}

bool AcolyteThreat(Unit* unit, Player* bot) { return ManaBurnerThreat(unit, bot, 20.0f); }
bool ResearcherThreat(Unit* unit, Player* bot) { return ManaBurnerThreat(unit, bot, 30.0f); }

// A living attacker of the bot with this entry is part of the group's fight (the earlier step of a kill order).
bool EntryInGroupFight(Player* bot, uint32 entry)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        if (guid.GetEntry() != entry)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && KillOrder::IsInGroupFight(bot, unit))
            return true;
    }
    return false;
}

// On a drake the bot fights with the vehicle's spells (oc drake attack), not with its own target.
bool OnFoot(Unit* /*unit*/, Player* bot) { return !bot->GetVehicle(); }

bool SorceressesDown(Unit* /*unit*/, Player* bot)
{
    return !bot->GetVehicle() && !EntryInGroupFight(bot, NPC_RING_LORD_SORCERESS);
}

bool SpellFlingersDown(Unit* /*unit*/, Player* bot) { return !EntryInGroupFight(bot, NPC_AHNKAHAR_SPELL_FLINGER); }

// [b33_ok] Amanitar is fighting next to the beast (it only goes first in her fight, not in its own packs).
bool AmanitarFighting(Unit* unit, Player* /*bot*/)
{
    Creature* amanitar = unit->FindNearestCreature(NPC_AMANITAR, 60.0f, true);
    return amanitar && amanitar->IsInCombat();
}

bool WebWindersDown(Unit* unit, Player* bot)
{
    return SpellFlingersDown(unit, bot) && !EntryInGroupFight(bot, NPC_AHNKAHAR_WEB_WINDER);
}

bool OraclesDown(Unit* /*unit*/, Player* bot) { return !EntryInGroupFight(bot, NPC_COILFANG_ORACLE); }

bool PlagueGhoulsDown(Unit* /*unit*/, Player* bot) { return !EntryInGroupFight(bot, NPC_PLAGUE_GHOUL); }

// Rookery Hatchers walk to the eggs and hatch them (npc_rookery_hatcher, 6-8 s): every one alive adds whelps. A
// Slad'ran snake or a Steamrager on a member the tank does not hold is killed before it kills that member. Kill
// orders of mixed packs are a chain: the second entry is only first once none of the first is left in the fight.
constexpr std::array<KillFirstSpec, 21> KILL_FIRST = {{  // [b42_scholo] 20, [b47_plan] 21
    {MAP_GNOMEREGAN, NPC_MOBILE_ALERT_SYSTEM, nullptr, true},          // [b35_gnomer]
    {MAP_GNOMEREGAN, NPC_MOBILE_ALERT_SYSTEM_HEROIC, nullptr, true},   // [b35_gnomer]
    {MAP_BLACKROCK_SPIRE, NPC_ROOKERY_HATCHER, nullptr, true},
    {MAP_BLACKROCK_SPIRE, NPC_ROOKERY_HATCHER_HEROIC, nullptr, true},
    {MAP_GUNDRAK, NPC_SLADRAN_VIPER, &VictimIsHealer},
    {MAP_GUNDRAK, NPC_SLADRAN_CONSTRICTOR, &VictimIsHealer},
    {MAP_HALLS_OF_LIGHTNING, NPC_BLISTERING_STEAMRAGER, &VictimIsNotTank},
    {MAP_SCHOLOMANCE, NPC_SCHOLOMANCE_ACOLYTE, &AcolyteThreat},
    {MAP_SCHOLOMANCE, NPC_SPECTRAL_RESEARCHER, &ResearcherThreat},
    {MAP_SCHOLOMANCE, NPC_SCHOLOMANCE_DARK_SUMMONER, nullptr},  // [b42_scholo]
    {MAP_BLACKFATHOM_DEEPS, NPC_TWILIGHT_SHADOWMAGE, nullptr},  // [b47_plan]
    {MAP_OCULUS, NPC_RING_LORD_SORCERESS, &OnFoot},
    {MAP_OCULUS, NPC_RING_LORD_CONJURER, &SorceressesDown},
    {MAP_AHNKAHET, NPC_AHNKAHAR_SPELL_FLINGER, nullptr},
    {MAP_AHNKAHET, NPC_AHNKAHAR_WEB_WINDER, &SpellFlingersDown},
    {MAP_AHNKAHET, NPC_DEEP_CRAWLER, &WebWindersDown},
    {MAP_AHNKAHET, NPC_SAVAGE_CAVE_BEAST, &AmanitarFighting},  // [b33_ok]
    {MAP_STEAMVAULT, NPC_COILFANG_ORACLE, nullptr},
    {MAP_STEAMVAULT, NPC_COILFANG_SIREN, &OraclesDown},
    {MAP_STRATHOLME, NPC_PLAGUE_GHOUL, nullptr},
    {MAP_STRATHOLME, NPC_SKELETAL_GUARDIAN, &PlagueGhoulsDown},
}};

struct FrontalCleaveSpec
{
    uint32 mapId;
    uint32 entry;
    float radius;
    float arc;  // radians
};

constexpr float DEGREE = static_cast<float>(M_PI) / 180.0f;

// Caverndeep Reaver Arcing Smash 38761: 10 yd, TARGET_UNIT_CONE_ENEMY_104, instant. Blistering Steamrager Steam
// Blast 52531 / 59141: 15 yd, TARGET_UNIT_CONE_ENEMY_24 (the default 60 degree cone), instant.
constexpr float ABOMINATION_CLEAVE_REACH = 13.0f;  // melee distance + Cleave's 10 yd chain jump
constexpr std::array<FrontalCleaveSpec, 7> FRONTAL_CLEAVES = {{
    {MAP_GNOMEREGAN, NPC_CAVERNDEEP_REAVER, 10.0f, 104.0f * DEGREE},
    {MAP_GNOMEREGAN, NPC_CAVERNDEEP_REAVER_HEROIC, 10.0f, 104.0f * DEGREE},
    {MAP_HALLS_OF_LIGHTNING, NPC_BLISTERING_STEAMRAGER, 15.0f, 60.0f * DEGREE},
    {MAP_STRATHOLME, NPC_SKELETAL_BERSERKER, 10.0f, 180.0f * DEGREE},
    {MAP_STRATHOLME, NPC_PLAGUE_GHOUL, 10.0f, 180.0f * DEGREE},
    {MAP_HALLS_OF_REFLECTION_ESCAPE, NPC_LUMBERING_ABOMINATION, ABOMINATION_CLEAVE_REACH, 180.0f * DEGREE},
    {MAP_HALLS_OF_REFLECTION_ESCAPE, NPC_LUMBERING_ABOMINATION_HEROIC, ABOMINATION_CLEAVE_REACH, 180.0f * DEGREE},
}};

constexpr float ROOM_SCAN_RANGE = 80.0f;
constexpr float RALLY_ARRIVED = 3.0f;
constexpr float RANGED_ALIVE_RANGE = 60.0f;
constexpr float CLEAVE_SCAN_RANGE = 20.0f;
constexpr float CLEAVE_TRIGGER_DIST_MARGIN = 0.3f;
constexpr float CLEAVE_TRIGGER_ANGLE_MARGIN = 10.0f * DEGREE;
// The spot chosen keeps a wider margin than the trigger, so the bot does not step straight back to its edge.
constexpr float CLEAVE_EXIT_DIST_MARGIN = 1.0f;
constexpr float CLEAVE_EXIT_ANGLE_MARGIN = 20.0f * DEGREE;
constexpr std::array<float, 9> BEHIND_OFFSETS = {
    0.0f,
    static_cast<float>(M_PI) / 8.0f, -static_cast<float>(M_PI) / 8.0f,
    static_cast<float>(M_PI) / 4.0f, -static_cast<float>(M_PI) / 4.0f,
    static_cast<float>(M_PI) * 3.0f / 8.0f, -static_cast<float>(M_PI) * 3.0f / 8.0f,
    static_cast<float>(M_PI) / 2.0f, -static_cast<float>(M_PI) / 2.0f,
};
constexpr std::array<float, 5> CLEAVE_EXIT_STEPS = {3.0f, 5.0f, 8.0f, 11.0f, 14.0f};
constexpr uint32 CLEAVE_EXIT_DIRECTIONS = 16;
constexpr uint32 CLEAVE_LOG_INTERVAL_MS = 10 * IN_MILLISECONDS;

bool IsMeleeDps(Player* bot)
{
    return PlayerbotAI::IsMelee(bot) && !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot);
}

BossRoom const* FindRoom(uint32 mapId, uint32 bossEntry)
{
    for (BossRoom const& room : BOSS_ROOMS)
        if (room.mapId == mapId && room.bossEntry == bossEntry)
            return &room;

    return nullptr;
}

FrontalCleaveSpec const* FindCleave(uint32 mapId, uint32 entry)
{
    for (FrontalCleaveSpec const& spec : FRONTAL_CLEAVES)
        if (spec.mapId == mapId && spec.entry == entry)
            return &spec;

    return nullptr;
}

// The cleaving mobs in the fight near the bot that are not turned on the bot itself.
std::vector<std::pair<Unit*, FrontalCleaveSpec const*>> CollectCleavers(Player* bot, PlayerbotAI* botAI)
{
    std::vector<std::pair<Unit*, FrontalCleaveSpec const*>> cleavers;
    uint32 const mapId = bot->GetMapId();
    if (std::none_of(FRONTAL_CLEAVES.begin(), FRONTAL_CLEAVES.end(),
                     [mapId](FrontalCleaveSpec const& spec) { return spec.mapId == mapId; }))
        return cleavers;

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        FrontalCleaveSpec const* spec = FindCleave(mapId, guid.GetEntry());
        if (!spec)
            continue;

        Unit* mob = botAI->GetUnit(guid);
        if (!mob || !mob->IsAlive() || mob->GetVictim() == bot || bot->GetExactDist2d(mob) > CLEAVE_SCAN_RANGE)
            continue;

        cleavers.emplace_back(mob, spec);
    }
    return cleavers;
}

// A cone also hits every unit within max(its bounding radius, 2 yd) of the caster's centre, whatever the angle
// (WorldObjectSpellConeTargetCheck, Unit::IsWithinBoundaryRadius).
bool SpotInCleave(Player* bot, Unit* mob, FrontalCleaveSpec const& spec, float x, float y, float distMargin,
                  float angleMargin)
{
    float const dist = mob->GetExactDist2d(x, y);
    if (dist < std::max(bot->GetBoundaryRadius(), MIN_MELEE_REACH) + distMargin)
        return true;

    if (dist > spec.radius + bot->GetObjectSize() + distMargin)
        return false;

    Position const spot(x, y, mob->GetPositionZ());
    return mob->HasInArc(spec.arc + 2.0f * angleMargin, &spot);
}

bool SpotInAnyCleave(Player* bot, std::vector<std::pair<Unit*, FrontalCleaveSpec const*>> const& cleavers, float x,
                     float y, float distMargin, float angleMargin)
{
    for (auto const& [mob, spec] : cleavers)
        if (SpotInCleave(bot, mob, *spec, x, y, distMargin, angleMargin))
            return true;

    return false;
}
}  // namespace

ObjectGuid FindRoomTrashFight(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsInCombat())
        return ObjectGuid::Empty;

    for (BossRoom const& room : BOSS_ROOMS)
    {
        if (room.mapId != bot->GetMapId() || bot->GetExactDist2d(room.bossX, room.bossY) > ROOM_SCAN_RANGE)
            continue;

        GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
        bool const roomTrash = std::any_of(attackers.begin(), attackers.end(),
                                           [botAI, &room](ObjectGuid const& guid)
                                           {
                                               Unit* unit = botAI->GetUnit(guid);
                                               return unit && unit->IsAlive() && unit->GetEntry() != room.bossEntry &&
                                                      unit->GetExactDist2d(room.bossX, room.bossY) < room.trashRadius;
                                           });
        if (!roomTrash)
            continue;

        Creature* boss = bot->FindNearestCreature(room.bossEntry, ROOM_SCAN_RANGE, true);
        if (boss && !boss->IsInCombat())
            return boss->GetGUID();
    }
    return ObjectGuid::Empty;
}

BossRoom const* RoomTrashFight(PlayerbotAI* botAI, Unit*& boss)
{
    boss = nullptr;
    ObjectGuid const guid = botAI->GetAiObjectContext()->GetValue<ObjectGuid>("dungeon boss room")->Get();
    if (guid.IsEmpty())
        return nullptr;

    Unit* unit = botAI->GetUnit(guid);
    if (!unit || !unit->IsAlive() || unit->IsInCombat())
        return nullptr;

    // [bear_aoe_threat] Selin's entry pull (MgTShared.h, [selin_final_entry] / [selin_heroic_entry_pull]): the group
    // walks INTO her room as the pack comes and she joins 2 s later with the door (188065) shut behind them. This older
    // rule (fight her trash at the corridor rally while she is idle) pulled the tank back out in exactly those 2 s,
    // and its hold multiplier zeroed "mgt return to room" (a MovementAction) while the target stood near idle Selin:
    // mgt-h 20261025-184229-5D-21, the bear at (218,-5) inside was walked to the rally, the door shut at (211.7,-6.4),
    // 75 s outside (only Swipe reached through), 59 aggro losses, tank dead. The room tactic owns that walk.
    if (MagistersTerrace::SelinRoom::FinalEntryActive(botAI->GetBot()))
        return nullptr;

    BossRoom const* room = FindRoom(botAI->GetBot()->GetMapId(), unit->GetEntry());
    if (room)
        boss = unit;

    return room;
}

bool RangedDpsAlive(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
            bot->GetExactDist(member) > RANGED_ALIVE_RANGE)
            continue;

        if (PlayerbotAI::IsRangedDps(member) && !PlayerbotAI::IsHeal(member) && !PlayerbotAI::IsTank(member))
            return true;
    }
    return false;
}

bool MeleeShouldHold(Player* bot, Unit* target)
{
    if (!target || !target->IsAlive() || !IsMeleeDps(bot))
        return false;

    for (MeleeHoldSpec const& spec : MELEE_HOLDS)
    {
        if (spec.mapId != bot->GetMapId() || spec.entry != target->GetEntry())
            continue;

        if (spec.aura && !target->HasAura(spec.aura))
            continue;

        if (spec.whileRangedDps && !RangedDpsAlive(bot))
            continue;

        return true;
    }
    return false;
}

void AppendMeleeHoldExclusions(PlayerbotAI* botAI, GuidSet& exclusions)
{
    Player* bot = botAI->GetBot();
    if (!IsMeleeDps(bot))
        return;

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
        if (MeleeShouldHold(bot, botAI->GetUnit(guid)))
            exclusions.insert(guid);
}

Unit* KillFirstTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    uint32 const mapId = bot->GetMapId();
    if (std::none_of(KILL_FIRST.begin(), KILL_FIRST.end(),
                     [mapId](KillFirstSpec const& spec) { return spec.mapId == mapId; }))
        return nullptr;

    Unit* best = nullptr;
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        uint32 const entry = guid.GetEntry();
        auto const spec = std::find_if(KILL_FIRST.begin(), KILL_FIRST.end(), [mapId, entry](KillFirstSpec const& s)
                                       { return s.mapId == mapId && s.entry == entry; });
        if (spec == KILL_FIRST.end())
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || IsCrowdControlled(unit) || !AttackersValue::IsValidTarget(unit, bot) ||
            (spec->condition && !spec->condition(unit, bot)) ||
            (!spec->anywhere && !KillOrder::IsInGroupFight(bot, unit)))
            continue;

        if (!best || bot->GetExactDist(unit) < bot->GetExactDist(best))
            best = unit;
    }
    return best;
}

void AppendKillFirstExclusions(PlayerbotAI* botAI, GuidSet& exclusions)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsInCombat() || !PlayerbotAI::IsDps(bot) || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return;

    Unit* first = KillFirstTarget(botAI);
    if (!first)
        return;

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
        if (guid != first->GetGUID())
            exclusions.insert(guid);
}

bool SpotInFrontalCleave(Player* bot, PlayerbotAI* botAI, float x, float y, float distMargin, float angleMargin)
{
    return SpotInAnyCleave(bot, CollectCleavers(bot, botAI), x, y, distMargin, angleMargin);
}

bool InFrontalCleave(Player* bot, PlayerbotAI* botAI, float distMargin, float angleMargin)
{
    return SpotInFrontalCleave(bot, botAI, bot->GetPositionX(), bot->GetPositionY(), distMargin, angleMargin);
}

Unit* FrontalCleaverAt(Player* bot, PlayerbotAI* botAI, float distMargin, float angleMargin)
{
    for (auto const& [mob, spec] : CollectCleavers(bot, botAI))
        if (SpotInCleave(bot, mob, *spec, bot->GetPositionX(), bot->GetPositionY(), distMargin, angleMargin))
            return mob;
    return nullptr;
}

bool CleaverNearby(Player* bot, PlayerbotAI* botAI) { return !CollectCleavers(bot, botAI).empty(); }

bool IsKnownCleaver(uint32 mapId, uint32 entry) { return FindCleave(mapId, entry) != nullptr; }
}  // namespace DungeonTactics

bool DungeonBossRoomTrashTrigger::IsActive()
{
    Unit* boss = nullptr;
    DungeonTactics::BossRoom const* room = DungeonTactics::RoomTrashFight(botAI, boss);
    return room && bot->GetExactDist2d(boss) < room->safeDistance &&
           bot->GetExactDist2d(room->rallyX, room->rallyY) > DungeonTactics::RALLY_ARRIVED;
}

bool DungeonBossRoomPullOutAction::Execute(Event /*event*/)
{
    Unit* boss = nullptr;
    DungeonTactics::BossRoom const* room = DungeonTactics::RoomTrashFight(botAI, boss);
    if (!room || bot->GetExactDist2d(room->rallyX, room->rallyY) <= DungeonTactics::RALLY_ARRIVED)
        return false;

    return MoveTo(bot->GetMapId(), room->rallyX, room->rallyY, room->rallyZ, false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT, true);
}

float DungeonBossRoomHoldMultiplier::GetValue(Action* action)
{
    if (!TbcHazard::IsRepositionAction(action))
        return 1.0f;

    Unit* boss = nullptr;
    DungeonTactics::BossRoom const* room = DungeonTactics::RoomTrashFight(botAI, boss);
    if (!room)
        return 1.0f;

    Unit* target = AI_VALUE(Unit*, "current target");
    return target && target->GetExactDist2d(boss) < room->safeDistance ? 0.0f : 1.0f;
}

bool DungeonMeleeHoldTrigger::IsActive()
{
    return bot->IsInCombat() && DungeonTactics::MeleeShouldHold(bot, AI_VALUE(Unit*, "current target"));
}

bool DungeonMeleeHoldAction::Execute(Event /*event*/)
{
    Unit* current = AI_VALUE(Unit*, "current target");
    if (!DungeonTactics::MeleeShouldHold(bot, current))
        return false;

    Unit* other = nullptr;
    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    for (ObjectGuid const& guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == current || !unit->IsAlive() || IsCrowdControlled(unit) ||
            DungeonTactics::MeleeShouldHold(bot, unit) || !AttackersValue::IsValidTarget(unit, bot) ||
            !KillOrder::IsInGroupFight(bot, unit))
            continue;

        if (!other || bot->GetExactDist(unit) < bot->GetExactDist(other))
            other = unit;
    }

    if (other)
        return Attack(other);

    // Nothing else to hit: stop and wait (the strategy's target exclusions keep the pickers off the held mob).
    bot->AttackStop();
    context->GetValue<Unit*>("current target")->Set(nullptr);
    bot->SetTarget(ObjectGuid::Empty);
    bot->SetSelection(ObjectGuid::Empty);
    return true;
}

bool DungeonKillFirstTrigger::IsActive()
{
    if (!bot->IsInCombat() || !PlayerbotAI::IsDps(bot) || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;

    Unit* target = DungeonTactics::KillFirstTarget(botAI);
    return target && target != AI_VALUE(Unit*, "current target");
}

bool DungeonKillFirstAction::Execute(Event /*event*/)
{
    Unit* target = DungeonTactics::KillFirstTarget(botAI);
    if (!target || target == AI_VALUE(Unit*, "current target"))
        return false;

    // [b35_gnomer] A kill-first mob of an "anywhere" spec that fights somebody from outside the group's fight (the
    // Mobile Alert System, rooted 35-39 yd from the tank) was dropped again by "invalid target" (KillOrder::
    // IsInGroupFight) right after this switch. A prioritized target is in the group's fight wherever it stands.
    uint32 const mapId = bot->GetMapId();
    uint32 const entry = target->GetEntry();
    if (std::any_of(DungeonTactics::KILL_FIRST.begin(), DungeonTactics::KILL_FIRST.end(),
                    [mapId, entry](DungeonTactics::KillFirstSpec const& s)
                    { return s.anywhere && s.mapId == mapId && s.entry == entry; }))
        context->GetValue<GuidVector>("prioritized targets")->Set({target->GetGUID()});

    return Attack(target);
}

bool DungeonFrontalCleaveTrigger::IsActive()
{
    if (!bot->IsInCombat() || PlayerbotAI::IsTank(bot))
        return false;

    return DungeonTactics::InFrontalCleave(bot, botAI, DungeonTactics::CLEAVE_TRIGGER_DIST_MARGIN,
                                           DungeonTactics::CLEAVE_TRIGGER_ANGLE_MARGIN);
}

namespace
{
    // [lead14] when each bot last stepped out of a cleave (DungeonAvoidFrontalCleaveAction), for the multiplier's
    // hysteresis: no lower-priority walk takes it back into the cones for CLEAVE_STEP_OUT_HOLD_MS
    constexpr uint32 CLEAVE_STEP_OUT_HOLD_MS = 4 * IN_MILLISECONDS;
    std::mutex cleaveStepOutLock;
    std::unordered_map<uint64, uint32> cleaveStepOutMs;

    bool SteppedOutOfCleave(Player* bot)
    {
        std::lock_guard<std::mutex> lock(cleaveStepOutLock);
        auto const found = cleaveStepOutMs.find(bot->GetGUID().GetRawValue());
        return found != cleaveStepOutMs.end() && getMSTimeDiff(found->second, getMSTime()) < CLEAVE_STEP_OUT_HOLD_MS;
    }
}

bool DungeonAvoidFrontalCleaveAction::Execute(Event /*event*/)
{
    auto const cleavers = DungeonTactics::CollectCleavers(bot, botAI);
    if (cleavers.empty())
        return false;

    {
        std::lock_guard<std::mutex> lock(cleaveStepOutLock);
        uint32 const now = getMSTime();
        cleaveStepOutMs[bot->GetGUID().GetRawValue()] = now ? now : 1;  // [lead14]
    }

    {
        static std::mutex mutex;
        static std::unordered_map<uint64, uint32> lastLogMs;
        uint32 const now = getMSTime();
        bool log = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            uint32& at = lastLogMs[bot->GetGUID().GetRawValue()];
            if (!at || getMSTimeDiff(at, now) >= DungeonTactics::CLEAVE_LOG_INTERVAL_MS)
            {
                at = now ? now : 1;
                log = true;
            }
        }
        if (log)
            LOG_INFO("playerbots", "[dungeon frontal cleave] {}: steps out of the cone of {} ({:.1f} yd, map {})",
                     bot->GetName(), cleavers.front().first->GetName(), bot->GetExactDist2d(cleavers.front().first),
                     bot->GetMapId());
    }

    float const distMargin = DungeonTactics::CLEAVE_EXIT_DIST_MARGIN;
    float const angleMargin = DungeonTactics::CLEAVE_EXIT_ANGLE_MARGIN;
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();

    auto const tryMove = [&](float x, float y) -> bool
    {
        float z = botZ;
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, botX, botY, botZ, x, y, z))
            return false;

        // A wall may cut the line short; the shortened spot must still be clear.
        if (DungeonTactics::SpotInAnyCleave(bot, cleavers, x, y, distMargin, angleMargin))
            return false;

        return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true);
    };

    // Melee: behind the current target, inside its melee range but outside every body radius.
    Unit* target = AI_VALUE(Unit*, "current target");
    if (PlayerbotAI::IsMelee(bot) && target && target->IsAlive())
    {
        float const meleeRange = bot->GetMeleeRange(target);
        float const minRing = std::max(bot->GetBoundaryRadius(), MIN_MELEE_REACH) + distMargin + 0.5f;
        float const ring = std::max(minRing, meleeRange - 1.5f);
        float const back = target->GetOrientation() + static_cast<float>(M_PI);
        for (float offset : DungeonTactics::BEHIND_OFFSETS)
        {
            float const x = target->GetPositionX() + std::cos(back + offset) * ring;
            float const y = target->GetPositionY() + std::sin(back + offset) * ring;
            if (target->GetExactDist2d(x, y) > meleeRange - 0.2f ||
                DungeonTactics::SpotInAnyCleave(bot, cleavers, x, y, distMargin, angleMargin))
                continue;

            if (tryMove(x, y))
                return true;
        }
    }

    // Anybody else, or no free melee spot: the nearest spot out of every cone.
    for (float step : DungeonTactics::CLEAVE_EXIT_STEPS)
    {
        for (uint32 i = 0; i < DungeonTactics::CLEAVE_EXIT_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / DungeonTactics::CLEAVE_EXIT_DIRECTIONS;
            float const x = botX + std::cos(angle) * step;
            float const y = botY + std::sin(angle) * step;
            if (DungeonTactics::SpotInAnyCleave(bot, cleavers, x, y, distMargin, angleMargin))
                continue;

            if (tryMove(x, y))
                return true;
        }
    }
    return false;
}

float DungeonFrontalCleaveMultiplier::GetValue(Action* action)
{
    // [lead14] hysteresis: a bot that just stepped out of a cleave is not walked back by a lower-priority move (the
    // los pull gather, its follow, formation or ranged spot) for CLEAVE_STEP_OUT_HOLD_MS; its casts go on
    if (action && action->getName() != "dungeon avoid frontal cleave" && bot->IsInCombat() && SteppedOutOfCleave(bot))
    {
        std::string const& name = action->getName();
        if (name == "move to los pull spot" || name == "follow" || name == "combat formation move" ||
            name == "rear flank" || name == "move to ranged position" ||
            (!PlayerbotAI::IsMelee(bot) && (name == "reach melee" || name == "set behind")))
            return 0.0f;
    }

    if (!dynamic_cast<SetBehindTargetAction*>(action))
        return 1.0f;

    return DungeonTactics::CleaverNearby(bot, botAI) ? 0.0f : 1.0f;
}

namespace DungeonTactics
{
namespace
{
enum DungeonEncounterIds : uint32
{
    MAP_SHADOW_LABYRINTH = 555,
    MAP_SETHEKK_HALLS = 556,
    MAP_UTGARDE_KEEP = 574,
    MAP_FORGE_OF_SOULS = 632,
    MAP_PIT_OF_SARON = 658,
    MAP_HALLS_OF_REFLECTION = 668,
    MAP_SCHOLOMANCE_ENCOUNTERS = 289,
    MAP_STRATHOLME_ENCOUNTERS = 329,

    // boss_ingvar_the_plunderer.cpp: the entry stays 23954 through the undead phase (only the display changes).
    NPC_INGVAR = 23954,
    NPC_FALRIC = 38112,
    NPC_BLACKHEART = 18667,
    NPC_BRONJAHM = 36497,
    NPC_TALON_KING_IKISS = 18473,
    NPC_PHANTOM_MAGE = 38172,
    NPC_RISEN_WITCH_DOCTOR = 36941,  // [b33_hor]
    NPC_SCHOLOMANCE_ACOLYTE_CASTER = 10471,
    NPC_SCHOLOMANCE_NECROMANCER = 10477,
    NPC_INSTRUCTOR_MALICIA = 10505,

    // Ingvar: Woe Strike 42730 / 59735 (a Curse) makes every heal on its target hit the healer for 875-1125 /
    // 1750-2250 shadow (42739 / 59736): the top-up leaves that member to the decurse and the emergency heals.
    SPELL_WOE_STRIKE = 42730,
    SPELL_WOE_STRIKE_H = 59735,
    // Smash 42669 / 59706 (physical, 13-17k / 37-43k) and Dark Smash 42723 / 59709 (shadow, 8.7-11.2k / 26-33k):
    // 3 s casts, a 10 yd cone, Ingvar rooted with rotation disabled.
    SPELL_SMASH = 42669,
    SPELL_SMASH_H = 59706,
    SPELL_DARK_SMASH = 42723,
    SPELL_DARK_SMASH_H = 59709,

    // Garfrost: Deep Freeze 70381 / 72930 (2 s cast on a random player, 14 s aura) ends with 70380 / 70384 (4000 /
    // 6000 frost and a stun), raised 10 / 20 % per Permafrost stack (68786 / 70336): 21.6k on a dps in wave 10.
    SPELL_DEEP_FREEZE = 70381,
    SPELL_DEEP_FREEZE_H = 72930,

    // Bronjahm: Fear 68950 (2 s cast, 3 s, 10 yd, MECHANIC_FEAR, InterruptFlags 0: no kick) on one player within
    // 10 yd, only scheduled once he has teleported for Soulstorm at 35 % (boss_bronjahm.cpp DamageTaken): a feared
    // player runs out of the storm's eye into Soulstorm 68921 / 69049.
    SPELL_BRONJAHM_FEAR = 68950,

    // Halls of Reflection, Phantom Mage (npc_phantom_mage): Fireball 72163 / 72164, Frostbolt 72166 / 72167,
    // Flamestrike 72169 / 72170, all PreventionType silence with the interrupt flag (wave 10: a 6.4k Fireball
    // killed the tank).
    SPELL_PHANTOM_FIREBALL = 72163,
    SPELL_PHANTOM_FIREBALL_H = 72164,
    SPELL_PHANTOM_FROSTBOLT = 72166,
    SPELL_PHANTOM_FROSTBOLT_H = 72167,
    SPELL_PHANTOM_FLAMESTRIKE = 72169,
    SPELL_PHANTOM_FLAMESTRIKE_H = 72170,

    // [b33_hor] Halls of Reflection escape, Risen Witch Doctor (npc_hor_risen_witch_doctor): Shadow Bolt Volley 70145 /
    // 70184 (2 s, every 9 s, everyone within 30 yd, interrupt flag 15). Its Curse of Doom 70144 / 70183 is instant
    // and its Shadow Bolt is left to the rota (hor-n 20261026-000434 5G: 11.6k of Volley in the warrior's last 15 s,
    // four Witch Doctors on wall 4).
    SPELL_RISEN_WITCH_DOCTOR_VOLLEY = 70145,
    SPELL_RISEN_WITCH_DOCTOR_VOLLEY_H = 70184,

    // Scholomance (SmartAI, Spell.dbc): Acolyte Dark Mending 17613 (3.5 s heal on a hurt ally within 30 yd),
    // Necromancer Shadow Bolt Volley 14887 (3 s, everyone within 30 yd), Instructor Malicia Heal 15586 (3.5 s),
    // Flash Heal 17843 (1.5 s) and Renew 8362 (2 s); all PreventionType silence with the interrupt flag. The
    // Neophytes' Shadow Bolt 12739 is left to the interrupt rota: six of them at once would drain every kick.
    SPELL_DARK_MENDING = 17613,
    SPELL_NECROMANCER_SHADOW_BOLT_VOLLEY = 14887,
    SPELL_MALICIA_HEAL = 15586,
    SPELL_MALICIA_FLASH_HEAL = 17843,
    SPELL_MALICIA_RENEW = 8362,

    // The Oculus, Ring-Lord Sorceress (SmartAI 27639, Spell.dbc): Flamestrike 16102 (3 s) / 61402 (heroic 2 s, 5 yd
    // + a burning patch) and the Blizzard channel 50715 / 59278 (8 yd, 903 frost every 2 s on the ranged and the
    // healer it picks); all carry the interrupt flag, so a kick stops the cast or breaks the channel.
    SPELL_RING_LORD_FLAMESTRIKE = 16102,
    SPELL_RING_LORD_FLAMESTRIKE_H = 61402,
    SPELL_RING_LORD_BLIZZARD = 50715,
    SPELL_RING_LORD_BLIZZARD_H = 59278,

    // Ahn'kahet, Ahn'kahar Spell Flinger (SmartAI 30278): Shadow Blast 56698 / 59102, a 6 s cast for 50 % / 80 % of
    // its target's maximum health, interrupt flag set. Six seconds leave time for any kick. Shadow Sickle 56702 /
    // 59103 is a 6 s self channel (SPELL_ATTR1_IS_SELF_CHANNELED, channel interrupt flag, PreventionType silence)
    // whose every 0.8 s tick (spell_shadow_sickle_periodic_damage) hits a random member within 40 yd for 1.9-2.5k on
    // heroic: the batch 6 wipe (20261025-000027) died to it, not to Shadow Blast.
    SPELL_SHADOW_BLAST = 56698,
    SPELL_SHADOW_BLAST_H = 59102,
    SPELL_SHADOW_SICKLE = 56702,
    SPELL_SHADOW_SICKLE_H = 59103,

    // The Steamvault: Coilfang Oracle Heal 22883 / 31730 (3.5 s, on an ally below 40 %), Coilfang Siren Fear 38660
    // (1 s, everyone within 10 yd); both carry the interrupt flag. The Sorceress' Frostbolt is left to the rota.
    SPELL_COILFANG_ORACLE_HEAL = 22883,
    SPELL_COILFANG_ORACLE_HEAL_H = 31730,
    SPELL_COILFANG_SIREN_FEAR = 38660,

    // Stratholme, Skeletal Guardian (SmartAI 10390, Spell.dbc): Blizzard 8364 (channel, 8 yd) and Arcane Explosion
    // 11975 (1.5 s, 10 yd around it: the tank and the melee) carry the interrupt flag; its Shadow Bolt and Frostbolt
    // are left to the rota.
    NPC_STRATHOLME_SKELETAL_GUARDIAN = 10390,
    SPELL_SKELETAL_GUARDIAN_BLIZZARD = 8364,
    SPELL_SKELETAL_GUARDIAN_ARCANE_EXPLOSION = 11975,
};

struct PriorityInterruptSpec
{
    uint32 mapId;
    uint32 casterEntry;
    std::array<uint32, 6> spells;
};

constexpr std::array<PriorityInterruptSpec, 12> PRIORITY_INTERRUPTS = {{  // [b47_plan] 10 -> 11, [b48_scholo] 12
    {MAP_BLACKFATHOM_DEEPS, NPC_TWILIGHT_SHADOWMAGE, {SPELL_TWILIGHT_SHADOWMAGE_DOMINATE_MIND}},  // [b47_plan]
    {MAP_SCHOLOMANCE, NPC_SCHOLOMANCE_DARK_SUMMONER, {SPELL_DARK_SUMMONER_SUMMON_RISEN_LACKEY}},  // [b48_scholo]
    {MAP_HALLS_OF_REFLECTION, NPC_PHANTOM_MAGE,
     {SPELL_PHANTOM_FIREBALL, SPELL_PHANTOM_FIREBALL_H, SPELL_PHANTOM_FROSTBOLT, SPELL_PHANTOM_FROSTBOLT_H,
      SPELL_PHANTOM_FLAMESTRIKE, SPELL_PHANTOM_FLAMESTRIKE_H}},
    // [b33_hor]
    {MAP_HALLS_OF_REFLECTION, NPC_RISEN_WITCH_DOCTOR,
     {SPELL_RISEN_WITCH_DOCTOR_VOLLEY, SPELL_RISEN_WITCH_DOCTOR_VOLLEY_H}},
    {MAP_SCHOLOMANCE_ENCOUNTERS, NPC_SCHOLOMANCE_ACOLYTE_CASTER, {SPELL_DARK_MENDING}},
    {MAP_SCHOLOMANCE_ENCOUNTERS, NPC_SCHOLOMANCE_NECROMANCER, {SPELL_NECROMANCER_SHADOW_BOLT_VOLLEY}},
    {MAP_SCHOLOMANCE_ENCOUNTERS, NPC_INSTRUCTOR_MALICIA,
     {SPELL_MALICIA_HEAL, SPELL_MALICIA_FLASH_HEAL, SPELL_MALICIA_RENEW}},
    {MAP_OCULUS, NPC_RING_LORD_SORCERESS,
     {SPELL_RING_LORD_FLAMESTRIKE, SPELL_RING_LORD_FLAMESTRIKE_H, SPELL_RING_LORD_BLIZZARD,
      SPELL_RING_LORD_BLIZZARD_H}},
    {MAP_AHNKAHET, NPC_AHNKAHAR_SPELL_FLINGER,
     {SPELL_SHADOW_BLAST, SPELL_SHADOW_BLAST_H, SPELL_SHADOW_SICKLE, SPELL_SHADOW_SICKLE_H}},
    {MAP_STEAMVAULT, NPC_COILFANG_ORACLE, {SPELL_COILFANG_ORACLE_HEAL, SPELL_COILFANG_ORACLE_HEAL_H}},
    {MAP_STEAMVAULT, NPC_COILFANG_SIREN, {SPELL_COILFANG_SIREN_FEAR}},
    {MAP_STRATHOLME_ENCOUNTERS, NPC_STRATHOLME_SKELETAL_GUARDIAN,
     {SPELL_SKELETAL_GUARDIAN_BLIZZARD, SPELL_SKELETAL_GUARDIAN_ARCANE_EXPLOSION}},
}};

// Group-wide hits nothing can stop: Dreadful Roar 42729 / 59734 (60 yd, 3.1-3.9k / 5.3-6.8k, PreventionType 0, so
// no kick) and Staggering Roar 42708 / 59708; Defiling Horror 72435 / 72452 (every 20 s, 200 yd, MECHANIC_HORROR:
// Fear Ward, Tremor Totem and Berserker Rage do not work on it; 4 x 2500 / 4000 shadow); Incite Chaos 33684 (the
// group fights itself for 15 s). Everybody goes into the next one at full health.
struct TopUpSpec
{
    uint32 mapId;
    uint32 bossEntry;
    float belowPct;
};

constexpr std::array<TopUpSpec, 3> TOP_UPS = {{
    {MAP_UTGARDE_KEEP, NPC_INGVAR, 90.0f},
    {MAP_HALLS_OF_REFLECTION, NPC_FALRIC, 92.0f},
    {MAP_SHADOW_LABYRINTH, NPC_BLACKHEART, 85.0f},
}};

struct ProtectSpec
{
    uint32 mapId;
    uint32 aura;
    uint32 auraHeroic;
    float belowPct;
};

constexpr std::array<ProtectSpec, 1> PROTECTS = {{
    {MAP_PIT_OF_SARON, SPELL_DEEP_FREEZE, SPELL_DEEP_FREEZE_H, 99.0f},
}};

struct TankBusterSpec
{
    uint32 mapId;
    uint32 spell;
};

constexpr std::array<TankBusterSpec, 4> TANK_BUSTERS = {{
    {MAP_UTGARDE_KEEP, SPELL_SMASH},
    {MAP_UTGARDE_KEEP, SPELL_SMASH_H},
    {MAP_UTGARDE_KEEP, SPELL_DARK_SMASH},
    {MAP_UTGARDE_KEEP, SPELL_DARK_SMASH_H},
}};

struct FearGuardSpec
{
    uint32 mapId;
    uint32 bossEntry;
    uint32 fearSpell;
    float belowPct;  // the fear phase starts here
    float fearRadius;
};

constexpr std::array<FearGuardSpec, 1> FEAR_GUARDS = {{
    {MAP_FORGE_OF_SOULS, NPC_BRONJAHM, SPELL_BRONJAHM_FEAR, 36.0f, 10.0f},
}};

struct WaitForTankSpec
{
    uint32 mapId;
    uint32 bossEntry;
};

// boss_talon_king_ikiss.cpp: at 80 / 50 / 25 % Blink, Arcane Explosion 1 s later and ResetAllThreat 7 s later; the
// first damage dealer to hit him then has him (wave 10: melee on the rogue and the shaman).
constexpr std::array<WaitForTankSpec, 1> WAIT_FOR_TANK = {{
    {MAP_SETHEKK_HALLS, NPC_TALON_KING_IKISS},
}};

constexpr float PRIORITY_INTERRUPT_RANGE = 30.0f;
constexpr float HEAL_RANGE = 40.0f;
constexpr float OTHER_MEMBER_LOW_PCT = 50.0f;  // the generic heals answer that member first
constexpr float TANK_BUSTER_RANGE = 20.0f;
constexpr float FEAR_WARD_RANGE = 30.0f;
constexpr float BERSERKER_RAGE_MARGIN = 5.0f;
constexpr float WAIT_TANK_RANGE = 40.0f;
constexpr uint32 WAIT_FOR_TANK_MAX_MS = 4000;  // a tank that cannot get it back in this time does not stop the kill

template <class Spec, size_t N>
bool MapHasSpec(std::array<Spec, N> const& specs, uint32 mapId)
{
    return std::any_of(specs.begin(), specs.end(), [mapId](Spec const& spec) { return spec.mapId == mapId; });
}

Unit* AttackerWithEntry(PlayerbotAI* botAI, uint32 entry)
{
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        if (guid.GetEntry() != entry)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive())
            return unit;
    }
    return nullptr;
}

template <size_t N>
bool IsCastingOneOf(Unit* unit, std::array<uint32, N> const& spells)
{
    if (!unit->IsNonMeleeSpellCast(false))
        return false;

    return std::any_of(spells.begin(), spells.end(),
                       [unit](uint32 spell) { return spell && unit->FindCurrentSpellBySpellId(spell); });
}

bool IsTankPlayer(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && PlayerbotAI::IsTank(player);
}

bool CanHeal(Player* bot, Player* member)
{
    return member->IsInWorld() && member->GetMap() == bot->GetMap() && bot->IsWithinDistInMap(member, HEAL_RANGE) &&
           bot->IsWithinLOSInMap(member);
}

bool FearCastInReach(Player* bot, Unit* boss)
{
    for (FearGuardSpec const& spec : FEAR_GUARDS)
        if (spec.mapId == bot->GetMapId() && boss->FindCurrentSpellBySpellId(spec.fearSpell) &&
            bot->GetExactDist2d(boss) <= spec.fearRadius + BERSERKER_RAGE_MARGIN)
            return true;

    return false;
}
}  // namespace

Unit* PriorityInterruptTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    uint32 const mapId = bot->GetMapId();
    if (!MapHasSpec(PRIORITY_INTERRUPTS, mapId))
        return nullptr;

    Unit* best = nullptr;
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        for (PriorityInterruptSpec const& spec : PRIORITY_INTERRUPTS)
        {
            if (spec.mapId != mapId || spec.casterEntry != guid.GetEntry())
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || bot->GetExactDist(unit) > PRIORITY_INTERRUPT_RANGE ||
                !IsCastingOneOf(unit, spec.spells))
                continue;

            if (!best || bot->GetExactDist(unit) < bot->GetExactDist(best))
                best = unit;
        }
    }
    return best;
}

Player* TopUpTarget(PlayerbotAI* botAI, bool& protect)
{
    protect = false;
    Player* bot = botAI->GetBot();
    uint32 const mapId = bot->GetMapId();
    if (!PlayerbotAI::IsHeal(bot) || !bot->IsInCombat() ||
        (!MapHasSpec(TOP_UPS, mapId) && !MapHasSpec(PROTECTS, mapId)))
        return nullptr;

    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    float topUpBelow = 0.0f;
    for (TopUpSpec const& spec : TOP_UPS)
        if (spec.mapId == mapId && AttackerWithEntry(botAI, spec.bossEntry))
            topUpBelow = std::max(topUpBelow, spec.belowPct);

    Player* marked = nullptr;
    Player* lowest = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || member->GetMapId() != mapId)
            continue;

        float const health = member->GetHealthPct();
        if (health < OTHER_MEMBER_LOW_PCT)
            return nullptr;

        for (ProtectSpec const& spec : PROTECTS)
            if (spec.mapId == mapId && health < spec.belowPct &&
                (member->HasAura(spec.aura) || member->HasAura(spec.auraHeroic)) && CanHeal(bot, member))
                marked = member;

        if (health >= topUpBelow || member->HasAura(SPELL_WOE_STRIKE) || member->HasAura(SPELL_WOE_STRIKE_H))
            continue;

        if ((!lowest || health < lowest->GetHealthPct()) && CanHeal(bot, member))
            lowest = member;
    }

    if (marked)
    {
        protect = true;
        return marked;
    }
    return lowest;
}

char const* TankCooldown(PlayerbotAI* botAI)
{
    struct Defensive
    {
        uint8 classId;
        char const* spell;
    };

    static std::array<Defensive, 7> const DEFENSIVES = {{{CLASS_WARRIOR, "shield wall"},
                                                         {CLASS_WARRIOR, "shield block"},
                                                         {CLASS_PALADIN, "divine protection"},
                                                         {CLASS_DEATH_KNIGHT, "icebound fortitude"},
                                                         {CLASS_DEATH_KNIGHT, "anti-magic shell"},
                                                         {CLASS_DRUID, "survival instincts"},
                                                         {CLASS_DRUID, "barkskin"}}};

    Player* bot = botAI->GetBot();
    uint32 const mapId = bot->GetMapId();
    if (!PlayerbotAI::IsTank(bot) || !MapHasSpec(TANK_BUSTERS, mapId))
        return nullptr;

    bool incoming = false;
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsNonMeleeSpellCast(false) ||
            bot->GetExactDist(unit) > TANK_BUSTER_RANGE ||
            (unit->GetVictim() != bot && !unit->HasInArc(static_cast<float>(M_PI), bot)))
            continue;

        incoming = std::any_of(TANK_BUSTERS.begin(), TANK_BUSTERS.end(), [mapId, unit](TankBusterSpec const& spec)
                               { return spec.mapId == mapId && unit->FindCurrentSpellBySpellId(spec.spell); });
        if (incoming)
            break;
    }
    if (!incoming)
        return nullptr;

    // One at a time: a second one on top is wasted.
    uint8 const classId = bot->getClass();
    for (Defensive const& option : DEFENSIVES)
        if (option.classId == classId && botAI->HasAura(option.spell, bot))
            return nullptr;

    for (Defensive const& option : DEFENSIVES)
    {
        if (option.classId != classId ||
            !botAI->GetAiObjectContext()->GetValue<uint32>("spell id", option.spell)->Get() ||
            !botAI->CanCastSpell(option.spell, bot))
            continue;

        return option.spell;
    }
    return nullptr;
}

Unit* FearGuardBoss(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    uint32 const mapId = bot->GetMapId();
    if (!MapHasSpec(FEAR_GUARDS, mapId))
        return nullptr;

    for (FearGuardSpec const& spec : FEAR_GUARDS)
    {
        if (spec.mapId != mapId)
            continue;

        Unit* boss = AttackerWithEntry(botAI, spec.bossEntry);
        if (boss && boss->GetHealthPct() <= spec.belowPct)
            return boss;
    }
    return nullptr;
}

Player* FearWardTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    if (!group || !botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "fear ward")->Get())
        return nullptr;

    // The healer first (a feared healer turns the storm into a wipe), then anybody without the ward.
    Player* best = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || member->GetMapId() != bot->GetMapId() ||
            !bot->IsWithinDistInMap(member, FEAR_WARD_RANGE) || botAI->HasAura("fear ward", member))
            continue;

        if (!best || (PlayerbotAI::IsHeal(member) && !PlayerbotAI::IsHeal(best)))
            best = member;
    }
    return best && botAI->CanCastSpell("fear ward", best) ? best : nullptr;
}

Unit* WaitForTankBoss(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    uint32 const mapId = bot->GetMapId();
    if (PlayerbotAI::IsTank(bot) || !MapHasSpec(WAIT_FOR_TANK, mapId))
        return nullptr;

    for (WaitForTankSpec const& spec : WAIT_FOR_TANK)
    {
        if (spec.mapId != mapId)
            continue;

        Unit* boss = AttackerWithEntry(botAI, spec.bossEntry);
        if (boss && boss->GetVictim() && !IsTankPlayer(boss->GetVictim()))
            return boss;
    }
    return nullptr;
}
}  // namespace DungeonTactics

bool DungeonPriorityInterruptTrigger::IsActive()
{
    return bot->IsInCombat() && DungeonTactics::PriorityInterruptTarget(botAI) && DungeonHoldFire::HasInterrupt(botAI);
}

bool DungeonPriorityInterruptAction::Execute(Event /*event*/)
{
    return DungeonHoldFire::CastInterrupt(botAI, DungeonTactics::PriorityInterruptTarget(botAI));
}

bool DungeonTopUpTrigger::IsActive()
{
    bool protect = false;
    return DungeonTactics::TopUpTarget(botAI, protect) != nullptr;
}

bool DungeonTopUpAction::Execute(Event /*event*/)
{
    constexpr float SWIFTMEND_BELOW_PCT = 70.0f;

    bool protect = false;
    Player* target = DungeonTactics::TopUpTarget(botAI, protect);
    if (!target)
        return false;

    auto const cast = [this, target](char const* spell)
    { return botAI->CanCastSpell(spell, target) && botAI->CastSpell(spell, target); };
    auto const castHot = [this, target, &cast](char const* spell)
    { return !botAI->HasAura(spell, target, false, true) && cast(spell); };

    // A marked member gets the shield or HoT first; a plain top-up uses the efficient heal of the class.
    switch (bot->getClass())
    {
        case CLASS_PRIEST:
            if (protect && !botAI->HasAura("weakened soul", target) && cast("power word: shield"))
                return true;
            return (protect && cast("penance")) || castHot("renew") || cast("flash heal");
        case CLASS_DRUID:
            if (castHot("rejuvenation"))
                return true;
            return (protect && target->GetHealthPct() < SWIFTMEND_BELOW_PCT && cast("swiftmend")) ||
                   cast("nourish") || cast("regrowth");
        case CLASS_SHAMAN:
            return cast("riptide") || cast("lesser healing wave") || cast("healing wave");
        case CLASS_PALADIN:
            return (protect && cast("holy shock")) || cast("flash of light") || cast("holy light");
        default:
            return false;
    }
}

bool DungeonTankCooldownTrigger::IsActive() { return DungeonTactics::TankCooldown(botAI) != nullptr; }

bool DungeonTankCooldownAction::Execute(Event /*event*/)
{
    char const* spell = DungeonTactics::TankCooldown(botAI);
    return spell && botAI->CastSpell(spell, bot);
}

bool DungeonFearGuardTrigger::IsActive()
{
    uint8 const classId = bot->getClass();
    if (classId != CLASS_SHAMAN && classId != CLASS_PRIEST && classId != CLASS_WARRIOR)
        return false;

    Unit* boss = DungeonTactics::FearGuardBoss(botAI);
    if (!boss)
        return false;

    switch (classId)
    {
        case CLASS_SHAMAN:
            return AI_VALUE2(uint32, "spell id", "tremor totem") && !AI_VALUE2(bool, "has totem", "tremor totem");
        case CLASS_PRIEST:
            return DungeonTactics::FearWardTarget(botAI) != nullptr;
        default:
            return DungeonTactics::FearCastInReach(bot, boss) && !botAI->HasAura("berserker rage", bot) &&
                   botAI->CanCastSpell("berserker rage", bot);
    }
}

bool DungeonFearGuardAction::Execute(Event /*event*/)
{
    Unit* boss = DungeonTactics::FearGuardBoss(botAI);
    if (!boss)
        return false;

    switch (bot->getClass())
    {
        case CLASS_SHAMAN:
            return botAI->CanCastSpell("tremor totem", bot) && botAI->CastSpell("tremor totem", bot);
        case CLASS_PRIEST:
        {
            Player* target = DungeonTactics::FearWardTarget(botAI);
            return target && botAI->CastSpell("fear ward", target);
        }
        case CLASS_WARRIOR:
            return DungeonTactics::FearCastInReach(bot, boss) && botAI->CanCastSpell("berserker rage", bot) &&
                   botAI->CastSpell("berserker rage", bot);
        default:
            return false;
    }
}

float DungeonFearGuardMultiplier::GetValue(Action* action)
{
    if (bot->getClass() != CLASS_SHAMAN)
        return 1.0f;

    bool const competing =
        dynamic_cast<CastStrengthOfEarthTotemAction*>(action) || dynamic_cast<CastStoneskinTotemAction*>(action) ||
        dynamic_cast<CastEarthbindTotemAction*>(action) || dynamic_cast<CastStoneclawTotemAction*>(action);
    if (!competing || !DungeonTactics::FearGuardBoss(botAI))
        return 1.0f;

    return 0.0f;
}

bool DungeonWaitForTankValue::Calculate()
{
    Unit* boss = DungeonTactics::WaitForTankBoss(botAI);
    if (!boss)
    {
        _offTankSinceMs = 0;
        return false;
    }

    // Only while a tank of the group is near enough to take it back.
    bool tankNear = false;
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref && !tankNear; ref = ref->next())
    {
        Player* member = ref->GetSource();
        tankNear = member && member != bot && member->IsAlive() && PlayerbotAI::IsTank(member) &&
                   member->IsWithinDistInMap(boss, DungeonTactics::WAIT_TANK_RANGE);
    }
    if (!tankNear)
        return false;

    uint32 const now = getMSTime();
    if (!_offTankSinceMs)
        _offTankSinceMs = now;

    return getMSTimeDiff(_offTankSinceMs, now) < DungeonTactics::WAIT_FOR_TANK_MAX_MS;
}

bool DungeonWaitForTankTrigger::IsActive()
{
    return AI_VALUE(bool, "dungeon wait for tank") &&
           DungeonHoldFire::IsDamaging(botAI, DungeonTactics::WaitForTankBoss(botAI));
}

bool DungeonWaitForTankAction::Execute(Event /*event*/)
{
    Unit* boss = DungeonTactics::WaitForTankBoss(botAI);
    return boss && DungeonHoldFire::StopDamage(botAI, boss);
}

float DungeonWaitForTankMultiplier::GetValue(Action* action)
{
    if (PlayerbotAI::IsTank(bot) || dynamic_cast<DungeonWaitForTankAction*>(action) ||
        !AI_VALUE(bool, "dungeon wait for tank"))
        return 1.0f;

    return DungeonHoldFire::IsDamageAction(bot, action, DungeonTactics::WaitForTankBoss(botAI)) ? 0.0f : 1.0f;
}
