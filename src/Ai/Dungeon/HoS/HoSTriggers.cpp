/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoSTriggers.h"
#include "AiObjectContext.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "DungeonInteract.h"
#include "Group.h"
#include "InstanceScript.h"
#include "MoveConflict.h"  // [b33_harn]
#include "Pet.h"
#include "Playerbots.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>
#include <list>
#include <vector>

namespace
{
constexpr float ESCORT_STAY_RADIUS = 10.0f;
constexpr float TRIBUNAL_STAY_RADIUS = 15.0f;
constexpr float HAZARD_SEARCH_RADIUS = 15.0f;
constexpr float HAZARD_RADIUS = HoSShared::TRIBUNAL_HAZARD_RADIUS;  // Searing Gaze 51125, Dark Matter 51012: 5 yd
constexpr float HOLD_HAZARD_SEARCH = 40.0f;
}  // namespace

namespace HoSShared
{
Creature* GetBrann(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance)
        return nullptr;

    ObjectGuid const guid = instance->GetGuidData(HoSIds::NPC_BRANN);
    return guid ? bot->GetMap()->GetCreature(guid) : nullptr;
}

bool IsBrannEventActive(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    return instance && (instance->GetBossState(HoSIds::BRANN_BRONZEBEARD) == IN_PROGRESS ||
                        instance->GetBossState(HoSIds::BOSS_TRIBUNAL_OF_AGES) == IN_PROGRESS);
}

// The gossip Brann offers right now when it is the next step for the instance state, else 0.
uint32 NextBrannGossip(Player* bot, Creature* brann)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || !brann || !brann->IsAlive() || !brann->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return 0;

    EncounterState const escort = instance->GetBossState(HoSIds::BRANN_BRONZEBEARD);
    EncounterState const tribunal = instance->GetBossState(HoSIds::BOSS_TRIBUNAL_OF_AGES);
    if (instance->GetBossState(HoSIds::BRANN_DOOR) == DONE || escort == IN_PROGRESS || tribunal == IN_PROGRESS)
        return 0;

    uint32 const menu = brann->GetGossipMenuId();
    if (tribunal == DONE)
        return (menu == HoSIds::GOSSIP_TRIBUNAL_END || menu == HoSIds::GOSSIP_SJONNIR_DOOR) ? menu : 0;

    if (escort == DONE)
        return menu == HoSIds::GOSSIP_TRIBUNAL_START ? menu : 0;

    return menu == HoSIds::GOSSIP_TRIBUNAL_BEFORE ? menu : 0;
}

// Tribunal adds taunt Brann on spawn (spell_taunt_brann); the one closest to him is the most urgent.
Unit* NearestBrannAttacker(Player* /*bot*/, Creature* brann)
{
    if (!brann || !brann->IsAlive())
        return nullptr;

    Unit* nearest = nullptr;
    for (Unit* attacker : brann->getAttackers())
    {
        if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != brann)
            continue;

        if (!nearest || brann->GetExactDist(attacker) < brann->GetExactDist(nearest))
            nearest = attacker;
    }
    return nearest;
}

Unit* NearestTribunalHazard(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(HoSIds::BOSS_TRIBUNAL_OF_AGES) != IN_PROGRESS)
        return nullptr;

    for (uint32 entry : { HoSIds::NPC_SEARING_GAZE_TRIGGER, HoSIds::NPC_DARK_MATTER_TARGET })
        if (Creature* hazard = bot->FindNearestCreature(entry, HAZARD_SEARCH_RADIUS))
            if (bot->GetExactDist2d(hazard) < HAZARD_RADIUS)
                return hazard;

    return nullptr;
}

Unit* LooseTribunalAdd(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(HoSIds::BOSS_TRIBUNAL_OF_AGES) != IN_PROGRESS)
        return nullptr;

    // [b36_hos] Rank: a melee add (Protector, Golem) before a Stormcaller, then on the healer before another member,
    // then the nearest. hos-h 20261026-034959-hos-h-5G-25: the DK tank's taunt went on a Stormcaller hitting the
    // warrior ("ranged taunt ... Stormcaller ... on Rtwarah") while two Protectors killed the healer 4 yd from it
    // (1643-1647 s, 91 % -> 0 in 4 s). deaths.jsonl, HoS heroic, hits on healers (median max HP 14.0k): Protector
    // melee p90 9.0k, Cleave p90 8.9k, Charge p90 7.0k; Stormcaller Lightning Bolt p90 4.9k (34 %).
    Unit* best = nullptr;
    int bestRank = -1;
    float bestDistance = FLT_MAX;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* add = botAI->GetUnit(guid);
        if (!add || !add->IsAlive())
            continue;

        uint32 const entry = add->GetEntry();
        if (entry != HoSIds::NPC_DARK_RUNE_PROTECTOR && entry != HoSIds::NPC_DARK_RUNE_STORMCALLER &&
            entry != HoSIds::NPC_IRON_GOLEM_CUSTODIAN)
            continue;

        Player* victim = add->GetVictim() ? add->GetVictim()->ToPlayer() : nullptr;
        if (!victim || victim == bot || PlayerbotAI::IsTank(victim) || !victim->IsInSameRaidWith(bot))
            continue;

        int const rank = (entry != HoSIds::NPC_DARK_RUNE_STORMCALLER ? 2 : 0) + (PlayerbotAI::IsHeal(victim) ? 1 : 0);
        float const distance = bot->GetExactDist(add);
        if (rank > bestRank || (rank == bestRank && distance < bestDistance))
        {
            best = add;
            bestRank = rank;
            bestDistance = distance;
        }
    }
    return best;
}

bool IsTribunalActive(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    return instance && instance->GetBossState(HoSIds::BOSS_TRIBUNAL_OF_AGES) == IN_PROGRESS;
}

Player* GroupTank(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Player* first = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member == bot || !DungeonInteract::IsActiveMember(bot, member) || !PlayerbotAI::IsTank(member))
            continue;

        if (PlayerbotAI::IsMainTank(member))
            return member;

        if (!first)
            first = member;
    }
    return first;
}

Unit* ProtectorOnHealer(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!IsTribunalActive(bot))
        return nullptr;

    auto const onHealer = [bot](Unit* add)
    {
        if (!add || !add->IsAlive() || add->GetEntry() != HoSIds::NPC_DARK_RUNE_PROTECTOR)
            return false;

        Player* victim = add->GetVictim() ? add->GetVictim()->ToPlayer() : nullptr;
        return victim && PlayerbotAI::IsHeal(victim) && !PlayerbotAI::IsTank(victim) && victim->IsInSameRaidWith(bot);
    };

    // The bot keeps the Protector it is already on: re-picking the lowest one every tick swaps targets as their health
    // crosses (and "dps target", which excludes all but this one, follows the same choice).
    Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (onHealer(current))
        return current;

    Unit* best = nullptr;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != HoSIds::NPC_DARK_RUNE_PROTECTOR)
            continue;

        Unit* add = botAI->GetUnit(guid);
        if (!onHealer(add))
            continue;

        if (!best || add->GetHealthPct() < best->GetHealthPct())
            best = add;
    }
    return best;
}

// [b44_hos] DPS_GAP_TRIBUNAL_COS.md 2.5 (MEDIUM): with the Protector the tank took off the healer no longer "on the
// healer", "dps target" fell back to the Stormcaller rule and the bot switched away from a half-dead Protector (the
// warlock lost its DoTs on every swap). A player finishes the add he peeled.
Unit* PeeledProtector(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!IsTribunalActive(bot))
        return nullptr;

    Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!current || !current->IsAlive() || current->GetEntry() != HoSIds::NPC_DARK_RUNE_PROTECTOR ||
        current->GetThreatMgr().GetThreat(bot) <= 0.0f ||
        current->GetExactDist2d(HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y) > HoSIds::TRIBUNAL_MELEE_GUARD)
        return nullptr;

    // peeled: on a member of the group now (the tank's taunt), not one of Brann's that the bot only started on
    Player* victim = current->GetVictim() ? current->GetVictim()->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!victim || !victim->IsInSameRaidWith(bot))
        return nullptr;

    return current;
}

bool GroupHasRangedDps(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && member->IsInWorld() && member->GetMap() == bot->GetMap() &&
            PlayerbotAI::IsRanged(member) && !PlayerbotAI::IsHeal(member) && !PlayerbotAI::IsTank(member))
            return true;
    }
    return false;
}

bool LeftToRanged(Player* bot, Unit* add)
{
    if (add && (add->GetEntry() != HoSIds::NPC_DARK_RUNE_STORMCALLER ||
                add->GetExactDist2d(HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y) <= HoSIds::TRIBUNAL_MELEE_GUARD))
        return false;

    return GroupHasRangedDps(bot);
}

// Dark Matter hovers 10 yd above the floor (899.8, 355.3, 214.3) for its first 5 s, out of reach of its 5 yd blast.
bool IsNearTribunalHazard(Player* bot, float x, float y, float z, float radius)
{
    if (!IsTribunalActive(bot))
        return false;

    for (uint32 entry : { HoSIds::NPC_SEARING_GAZE_TRIGGER, HoSIds::NPC_DARK_MATTER_TARGET })
    {
        std::list<Creature*> hazards;
        bot->GetCreatureListWithEntryInGrid(hazards, entry, HOLD_HAZARD_SEARCH);
        for (Creature* hazard : hazards)
            if (hazard && hazard->IsAlive() && hazard->GetExactDist2d(x, y) < radius &&
                std::fabs(hazard->GetPositionZ() - z) < radius)
                return true;
    }
    return false;
}

bool IsLightningRing(Player* bot, Unit* boss)
{
    if (!boss || !boss->HasUnitState(UNIT_STATE_CASTING))
        return false;

    return boss->FindCurrentSpellBySpellId(SPELL_LIGHTNING_RING) ||
           boss->FindCurrentSpellBySpellId(
               DUNGEON_MODE(bot, HoSIds::SPELL_LIGHTNING_RING_5S_N, HoSIds::SPELL_LIGHTNING_RING_5S_H));
}

// The 7 s from Ground Slam to Stoned are the only time to spread; both auras only exist in this encounter.
bool IsGroundSlamActive(Player* bot) { return bot->HasAura(SPELL_GROUND_SLAM) || bot->HasAura(DEBUFF_GROUND_SLAM); }

bool IsKrystallusAhead(Player* bot)
{
    if (bot->GetExactDist(HoSIds::KRYSTALLUS_X, HoSIds::KRYSTALLUS_Y, HoSIds::KRYSTALLUS_Z) >
        HoSIds::KRYSTALLUS_PET_RANGE)
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    return instance && instance->GetBossState(HoSIds::BOSS_KRYSTALLUS) != DONE;
}

// Ground Slam reaches pets too; a pet is done with the mechanic only once its Stoned is gone as well.
bool IsShatterPending(Unit* unit)
{
    return unit->HasAura(SPELL_GROUND_SLAM) || unit->HasAura(DEBUFF_GROUND_SLAM) || unit->HasAura(SPELL_STONED);
}

// Everything that will be stoned and Shatter on the bot: other members and every pet or guardian of the group.
void CollectShatterSources(Player* bot, bool includeOwnPets, std::vector<Position>& sources)
{
    Group* group = bot->GetGroup();
    if (!group)
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() ||
            member->GetInstanceId() != bot->GetInstanceId() || !member->IsAlive())
            continue;

        if (member != bot)
            sources.push_back(member->GetPosition());
        else if (!includeOwnPets)
            continue;

        for (Unit* controlled : member->m_Controlled)
            if (controlled && controlled->IsInWorld() && controlled->IsAlive() && !controlled->IsTotem())
                sources.push_back(controlled->GetPosition());
    }
}

float NearestShatterSourceDistance(Player* bot, std::vector<Position> const& sources)
{
    float nearest = FLT_MAX;
    for (Position const& source : sources)
        nearest = std::min(nearest, bot->GetExactDist2d(source));

    return nearest;
}

bool IsKrystallusShortSummon(Unit* unit)
{
    switch (unit->GetEntry())
    {
        case HoSIds::NPC_GREATER_FIRE_ELEMENTAL:
        case HoSIds::NPC_GREATER_EARTH_ELEMENTAL:
        case HoSIds::NPC_WATER_ELEMENTAL:
        case HoSIds::NPC_WATER_ELEMENTAL_ALT:
        case HoSIds::NPC_SHADOWFIEND:
        case HoSIds::NPC_TREANT:
        case HoSIds::NPC_SPIRIT_WOLF:
        case HoSIds::NPC_ARMY_GHOUL:
        case HoSIds::NPC_MIRROR_IMAGE:
        case HoSIds::NPC_EBON_GARGOYLE:
        case HoSIds::NPC_RISEN_GHOUL:
            return true;
        default:
            return false;
    }
}

Unit* OwnedShortSummon(Player* bot)
{
    for (Unit* controlled : bot->m_Controlled)
        if (controlled && controlled->IsInWorld() && controlled->IsAlive() && controlled->IsCreature() &&
            !controlled->IsTotem() && IsKrystallusShortSummon(controlled))
            return controlled;

    return nullptr;
}

// Every bot of the group computes the same assignment: the tank holds the boss on spoke 0 (the boss faces it), the
// others take the remaining spokes, melee first on the ones nearest the boss's back, then by guid.
bool StarSpot(Player* bot, Unit* boss, float radius, float& x, float& y)
{
    Group* group = bot->GetGroup();
    if (!group || PlayerbotAI::IsTank(bot))
        return false;

    Player* tank = nullptr;
    std::vector<Player*> others;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() ||
            member->GetInstanceId() != bot->GetInstanceId() || !member->IsAlive())
            continue;

        if (!tank && PlayerbotAI::IsTank(member))
            tank = member;
        else
            others.push_back(member);
    }

    if (std::find(others.begin(), others.end(), bot) == others.end())
        return false;

    std::sort(others.begin(), others.end(),
              [](Player* a, Player* b)
              {
                  bool const meleeA = PlayerbotAI::IsMelee(a);
                  bool const meleeB = PlayerbotAI::IsMelee(b);
                  if (meleeA != meleeB)
                      return meleeA;

                  return a->GetGUID().GetCounter() < b->GetGUID().GetCounter();
              });

    // Spokes k = 1..n at k * 360 / (n + 1) degrees from the tank, ordered by closeness to the boss's back.
    uint32 const count = static_cast<uint32>(others.size());
    float const step = 2.0f * static_cast<float>(M_PI) / static_cast<float>(count + 1);
    std::vector<float> spokes;
    spokes.reserve(count);
    for (uint32 k = 1; k <= count; ++k)
        spokes.push_back(step * static_cast<float>(k));

    std::stable_sort(spokes.begin(), spokes.end(),
                     [](float a, float b)
                     {
                         return std::fabs(a - static_cast<float>(M_PI)) < std::fabs(b - static_cast<float>(M_PI));
                     });

    float const reference = tank ? boss->GetAngle(tank) : boss->GetOrientation();
    uint32 const index =
        static_cast<uint32>(std::distance(others.begin(), std::find(others.begin(), others.end(), bot)));
    float const angle = Position::NormalizeOrientation(reference + spokes[index]);
    x = boss->GetPositionX() + std::cos(angle) * radius;
    y = boss->GetPositionY() + std::sin(angle) * radius;
    return true;
}
}  // namespace HoSShared

bool KrystallusGroundSlamTrigger::IsActive() { return HoSShared::IsGroundSlamActive(bot); }

// A slammed pet or guardian (the killers of every Shatter wipe so far) must be parked; a parked one (stay + passive,
// a state bots never use otherwise) must be released after Shatter, even when Krystallus is already dead.
bool KrystallusPetSpreadTrigger::IsActive()
{
    bool const slammed = HoSShared::IsGroundSlamActive(bot);
    for (Unit* controlled : bot->m_Controlled)
    {
        if (!controlled || !controlled->IsAlive() || controlled->IsTotem() || !controlled->GetCharmInfo())
            continue;

        if (slammed || HoSShared::IsShatterPending(controlled))
            return true;

        if (controlled->GetCharmInfo()->HasCommandState(COMMAND_STAY) && controlled->IsCreature() &&
            controlled->ToCreature()->HasReactState(REACT_PASSIVE))
            return true;
    }
    return false;
}

bool KrystallusDismissPetTrigger::IsActive()
{
    if (bot->getClass() != CLASS_HUNTER || HoSShared::IsGroundSlamActive(bot) || bot->IsNonMeleeSpellCast(false))
        return false;

    Pet* pet = bot->GetPet();
    return pet && pet->IsAlive() && HoSShared::IsKrystallusAhead(bot);
}

// Also during Ground Slam: a summon dismissed before Stoned cannot Shatter (the pet frame needs no cast).
bool KrystallusDismissSummonTrigger::IsActive()
{
    return HoSShared::OwnedShortSummon(bot) && HoSShared::IsKrystallusAhead(bot);
}

// Ranged and healers hold their spoke of the star the whole fight, so Ground Slam only needs a short correction.
bool KrystallusRangedSpreadTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || !PlayerbotAI::IsRanged(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "krystallus");
    return boss && boss->IsAlive() && boss->IsInCombat();
}

bool SjonnirLightningRingTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "sjonnir the ironshaper");
    if (!boss) { return false; }

    return HoSShared::IsLightningRing(bot, boss);
}

bool HoSBrannGossipTrigger::IsActive()
{
    if (bot->IsInCombat() || !HoSShared::NextBrannGossip(bot, HoSShared::GetBrann(bot)))
        return false;

    return DungeonInteract::IsInteractor(botAI);
}

bool HoSStayWithBrannTrigger::IsActive()
{
    if (bot->IsInCombat() || !HoSShared::IsBrannEventActive(bot))
        return false;

    Creature* brann = HoSShared::GetBrann(bot);
    if (!brann || !brann->IsAlive())
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    float const radius = instance && instance->GetBossState(HoSIds::BOSS_TRIBUNAL_OF_AGES) == IN_PROGRESS
                             ? TRIBUNAL_STAY_RADIUS
                             : ESCORT_STAY_RADIUS;
    return bot->GetExactDist2d(brann) > radius;
}

bool HoSBrannAttackedTrigger::IsActive()
{
    if (PlayerbotAI::IsHeal(bot) || !HoSShared::IsBrannEventActive(bot))
        return false;

    return HoSShared::NearestBrannAttacker(bot, HoSShared::GetBrann(bot)) != nullptr;
}

bool HoSTribunalHazardTrigger::IsActive() { return HoSShared::NearestTribunalHazard(bot) != nullptr; }

bool HoSTribunalLooseAddTrigger::IsActive()
{
    // Not only in combat: a fresh wave goes for Brann and the members around him, and the tank (no attacker of
    // its own yet) stood out of combat while three Protectors killed the healer at its side (hos-h 2026-10-25
    // 02:32, build 8: "non combat, 0 attackers").
    return PlayerbotAI::IsTank(bot) && bot->IsAlive() && HoSShared::LooseTribunalAdd(botAI) != nullptr;
}

bool HoSTribunalHoldPointTrigger::IsActive()
{
    if (!bot->IsAlive() || !HoSShared::IsTribunalActive(bot))
        return false;

    // [b33_harn] Every member evaluates this: the Tribunal holds the group at the console, so the harness waypoint
    // walk and a LoS pull do not take anyone off it (MoveConflict::NoteEventHold; hos-h 20261026-000434-hos-h-5B-37:
    // "test runner move" 31 yd off at 1832 s, "move to los pull spot" 16 yd off at 1946 s).
    MoveConflict::NoteEventHold(bot, HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y, HoSIds::TRIBUNAL_HOLD_Z,
                                HoSIds::TRIBUNAL_TANK_LEASH, "hos tribunal");

    if (!PlayerbotAI::IsTank(bot))
        return false;

    // A main tank's partner (an off-tank) keeps its own fight.
    if (Player* other = HoSShared::GroupTank(bot); other && PlayerbotAI::IsMainTank(other) &&
                                                   !PlayerbotAI::IsMainTank(bot))
        return false;

    // [b34_hos] The tank's spot is TRIBUNAL_TANK_*, forward of the hold spot on the waves' walk in.
    if (bot->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y) <= HoSIds::TRIBUNAL_HOLD_TOLERANCE)
        return false;

    // Its current add right next to it at the edge of the spot: finish the swing first, then step back.
    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->IsAlive() && bot->IsWithinMeleeRange(target) &&
        bot->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y) <= HoSIds::TRIBUNAL_CHASE_LIMIT)
        return false;

    if (HoSShared::NearestTribunalHazard(bot))
        return false;

    // Not back into a Searing Gaze or Dark Matter that landed on the spot (10 s summons).
    for (uint32 entry : { HoSIds::NPC_SEARING_GAZE_TRIGGER, HoSIds::NPC_DARK_MATTER_TARGET })
        if (Creature* hazard = bot->FindNearestCreature(entry, HOLD_HAZARD_SEARCH))
            if (hazard->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y) < HAZARD_RADIUS)
                return false;

    return true;
}

bool HoSTribunalHealerHoldTrigger::IsActive()
{
    if (!PlayerbotAI::IsHeal(bot) || PlayerbotAI::IsTank(bot) || !bot->IsAlive() ||
        !HoSShared::IsTribunalActive(bot) || HoSShared::NearestTribunalHazard(bot))
        return false;

    // [b34_hos] The healer holds the spot by Brann, the tank meets the adds 10 yd forward of it (within heal range);
    // not back onto the spot while a Searing Gaze or Dark Matter lies on it.
    if (bot->GetExactDist2d(HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y) <= HoSIds::TRIBUNAL_HEALER_RADIUS)
        return false;

    return !HoSShared::IsNearTribunalHazard(bot, HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y,
                                            HoSIds::TRIBUNAL_HOLD_Z, HoSShared::TRIBUNAL_HAZARD_RADIUS);
}

bool HoSTribunalProtectorTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot) || !bot->IsAlive() || !bot->IsInCombat())
        return false;

    Unit* protector = HoSShared::ProtectorOnHealer(botAI);
    if (!protector)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return !target || target != protector;
}
