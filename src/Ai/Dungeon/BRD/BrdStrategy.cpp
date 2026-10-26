/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrdStrategy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <list>

#include "AttackersValue.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "Playerbots.h"
#include "Spell.h"

namespace
{
enum BrdIds : uint32
{
    // map id: MAP_BLACKROCK_DEPTHS (230) from AreaDefines.h

    NPC_ANVILRAGE_MEDIC = 8894,
    NPC_RAGEREAVER_GOLEM = 8906,
    NPC_WRATH_HAMMER_CONSTRUCT = 8907,
    NPC_MOIRA_BRONZEBEARD = 8929,
    NPC_IRONHAND_GUARDIAN = 8982,
    NPC_GOLEM_LORD_ARGELMACH = 8983,
    NPC_LORD_INCENDIUS = 9017,
    NPC_EMPEROR_THAURISSAN = 9019,
    NPC_WARDER_STILGISS = 9041,
    NPC_VEREK = 9042,

    SPELL_GOUT_OF_FLAMES = 15529,  // blackrock_depths.cpp brd_ironhand_guardian
};

// Rank order: the lower index wins when two kill-first mobs fight at once. withEntry: only while a creature of that
// entry is an attacker of the group too (0: always).
struct KillFirstDef
{
    uint32 entry;
    uint32 withEntry;
};

constexpr std::array<KillFirstDef, 5> KILL_FIRST = {{
    {NPC_MOIRA_BRONZEBEARD, NPC_EMPEROR_THAURISSAN},     // heals the Emperor
    {NPC_ANVILRAGE_MEDIC, 0},                            // Heal / Prayer of Healing on its pack
    {NPC_RAGEREAVER_GOLEM, NPC_GOLEM_LORD_ARGELMACH},    // Argelmach's aggro wakes his golems
    {NPC_WRATH_HAMMER_CONSTRUCT, NPC_GOLEM_LORD_ARGELMACH},
    {NPC_VEREK, NPC_WARDER_STILGISS},
}};

// Iron Hall (acore_world.creature map 230): guardians at x 1353 / 1407 facing each other, rows y -587.3 / -632.6 /
// -677.5, z -92.0. The breath crosses the hall along its row.
constexpr float IRON_HALL_MIN_X = 1345.0f;
constexpr float IRON_HALL_MAX_X = 1415.0f;
constexpr float IRON_HALL_MIN_Y = -700.0f;
constexpr float IRON_HALL_MAX_Y = -565.0f;
constexpr float IRON_HALL_MIN_Z = -100.0f;
constexpr float IRON_HALL_MAX_Z = -80.0f;
constexpr std::array<float, 3> IRON_HALL_ROWS = {-587.3f, -632.6f, -677.5f};
constexpr float GOUT_ROW_HALF_WIDTH = 9.0f;  // a cone 55 yd long across the hall: about this wide in its middle
constexpr float GOUT_SEARCH_RANGE = 90.0f;

constexpr float INCENDIUS_MELEE_DIST = 4.0f;
constexpr float INCENDIUS_WALL_PROBE = 14.0f;  // Mighty Blow throws farther than this on open ground
constexpr float INCENDIUS_ARRIVED = 1.5f;
constexpr uint32 INCENDIUS_SPOT_ANGLES = 16;

bool InIronHall(Player* bot)
{
    return bot->GetMapId() == MAP_BLACKROCK_DEPTHS && bot->GetPositionX() > IRON_HALL_MIN_X &&
           bot->GetPositionX() < IRON_HALL_MAX_X && bot->GetPositionY() > IRON_HALL_MIN_Y &&
           bot->GetPositionY() < IRON_HALL_MAX_Y && bot->GetPositionZ() > IRON_HALL_MIN_Z &&
           bot->GetPositionZ() < IRON_HALL_MAX_Z;
}

bool AttackersHaveEntry(GuidVector const& attackers, uint32 entry)
{
    return std::any_of(attackers.begin(), attackers.end(),
                       [entry](ObjectGuid const& guid) { return guid.GetEntry() == entry; });
}

// The guardian breathing now whose row the bot stands in, or nullptr.
Creature* BreathingGuardian(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!InIronHall(bot))
        return nullptr;

    GuidVector const guardians = botAI->GetAiObjectContext()->GetValue<GuidVector>("brd ironhand guardians")->Get();
    for (ObjectGuid const& guid : guardians)
    {
        Creature* guardian = botAI->GetCreature(guid);
        if (!guardian || !guardian->IsAlive())
            continue;

        Spell const* spell = guardian->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell)
            spell = guardian->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!spell || spell->m_spellInfo->Id != SPELL_GOUT_OF_FLAMES)
            continue;

        if (std::fabs(bot->GetPositionY() - guardian->GetPositionY()) < GOUT_ROW_HALF_WIDTH)
            return guardian;
    }
    return nullptr;
}

Creature* FoughtIncendius(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != MAP_BLACKROCK_DEPTHS || !bot->IsInCombat() || !PlayerbotAI::IsTank(bot))
        return nullptr;

    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!target || target->GetEntry() != NPC_LORD_INCENDIUS || !target->IsAlive() || target->GetVictim() != bot)
        return nullptr;

    return target->ToCreature();
}

// A wall behind the spot, seen from Incendius: no line of sight from the spot to the point the blow would throw to.
bool WallBehind(Player* bot, Creature* boss, float x, float y, float z)
{
    float const angle = boss->GetAngle(x, y);
    float const backX = x + std::cos(angle) * INCENDIUS_WALL_PROBE;
    float const backY = y + std::sin(angle) * INCENDIUS_WALL_PROBE;
    return !bot->GetMap()->isInLineOfSight(x, y, z + 1.5f, backX, backY, z + 1.5f, bot->GetPhaseMask(),
                                          LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
}
}  // namespace

namespace BlackrockDepths
{
Unit* KillFirstTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != MAP_BLACKROCK_DEPTHS || !bot->IsInCombat())
        return nullptr;

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (KillFirstDef const& def : KILL_FIRST)
    {
        if (def.withEntry && !AttackersHaveEntry(attackers, def.withEntry))
            continue;

        Unit* best = nullptr;
        for (ObjectGuid const& guid : attackers)
        {
            if (guid.GetEntry() != def.entry)
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || !AttackersValue::IsValidTarget(unit, bot))
                continue;

            if (!best || bot->GetExactDist(unit) < bot->GetExactDist(best))
                best = unit;
        }
        if (best)
            return best;
    }
    return nullptr;
}
}  // namespace BlackrockDepths

GuidVector BrdIronhandGuardiansValue::Calculate()
{
    GuidVector result;
    if (!InIronHall(bot))
        return result;

    std::list<Creature*> guardians;
    Acore::AllCreaturesOfEntryInRange check(bot, NPC_IRONHAND_GUARDIAN, GOUT_SEARCH_RANGE);
    Acore::CreatureListSearcher<Acore::AllCreaturesOfEntryInRange> searcher(bot, guardians, check);
    Cell::VisitObjects(bot, searcher, GOUT_SEARCH_RANGE);
    for (Creature* guardian : guardians)
        result.push_back(guardian->GetGUID());

    return result;
}

bool BrdKillFirstTrigger::IsActive()
{
    if (!PlayerbotAI::IsDps(bot) || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;

    Unit* target = BlackrockDepths::KillFirstTarget(botAI);
    return target && target != AI_VALUE(Unit*, "current target");
}

bool BrdKillFirstAction::Execute(Event /*event*/)
{
    Unit* target = BlackrockDepths::KillFirstTarget(botAI);
    if (!target || target == AI_VALUE(Unit*, "current target"))
        return false;

    LOG_INFO("playerbots", "[brd kill first] {}: {} ({:.1f} yd)", bot->GetName(), target->GetName(),
             bot->GetExactDist(target));
    return Attack(target);
}

bool BrdGoutOfFlamesTrigger::IsActive() { return BreathingGuardian(botAI) != nullptr; }

bool BrdAvoidGoutOfFlamesAction::Execute(Event /*event*/)
{
    Creature* guardian = BreathingGuardian(botAI);
    if (!guardian)
        return false;

    // The safe lines: halfway between two rows, and past the two end rows
    std::array<float, 4> const safeY = {IRON_HALL_ROWS[0] + 12.0f, (IRON_HALL_ROWS[0] + IRON_HALL_ROWS[1]) / 2.0f,
                                        (IRON_HALL_ROWS[1] + IRON_HALL_ROWS[2]) / 2.0f, IRON_HALL_ROWS[2] - 12.0f};
    float bestY = safeY[0];
    for (float const y : safeY)
        if (std::fabs(y - bot->GetPositionY()) < std::fabs(bestY - bot->GetPositionY()))
            bestY = y;

    float const x = bot->GetPositionX();
    float z = bot->GetPositionZ();
    bot->UpdateAllowedPositionZ(x, bestY, z);

    LOG_INFO("playerbots", "[brd gout of flames] {}: out of {}'s row (y {:.1f}) to y {:.1f}", bot->GetName(),
             guardian->GetName(), guardian->GetPositionY(), bestY);
    return MoveTo(bot->GetMapId(), x, bestY, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true);
}

bool BrdIncendiusWallTrigger::IsActive()
{
    Creature* boss = FoughtIncendius(botAI);
    if (!boss || bot->isMoving())
        return false;

    return !WallBehind(bot, boss, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
}

bool BrdIncendiusTankAtWallAction::Execute(Event /*event*/)
{
    Creature* boss = FoughtIncendius(botAI);
    if (!boss)
        return false;

    // The melee spot round him with a wall behind it, nearest to the tank
    float bestX = 0.0f;
    float bestY = 0.0f;
    float bestZ = 0.0f;
    float bestDist = -1.0f;
    for (uint32 i = 0; i < INCENDIUS_SPOT_ANGLES; ++i)
    {
        float const angle = float(i) * 2.0f * float(M_PI) / float(INCENDIUS_SPOT_ANGLES);
        float x = boss->GetPositionX() + std::cos(angle) * INCENDIUS_MELEE_DIST;
        float y = boss->GetPositionY() + std::sin(angle) * INCENDIUS_MELEE_DIST;
        float z = boss->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        if (std::fabs(z - boss->GetPositionZ()) > 3.0f || !boss->IsWithinLOS(x, y, z + 1.5f) ||
            !WallBehind(bot, boss, x, y, z))
            continue;

        float const dist = bot->GetExactDist2d(x, y);
        if (bestDist < 0.0f || dist < bestDist)
        {
            bestDist = dist;
            bestX = x;
            bestY = y;
            bestZ = z;
        }
    }

    if (bestDist < 0.0f || bestDist <= INCENDIUS_ARRIVED)
        return false;

    LOG_INFO("playerbots", "[brd incendius wall] {}: to ({:.1f},{:.1f}), {:.1f} yd, a wall behind (Mighty Blow)",
             bot->GetName(), bestX, bestY, bestDist);
    return MoveTo(bot->GetMapId(), bestX, bestY, bestZ, false, false, false, false, MovementPriority::MOVEMENT_COMBAT,
                  true);
}

void ClassicDungeonBlackrockDepthsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Gout of Flames: out of the breathing guardian's row, before anything else but survival
    triggers.push_back(new TriggerNode("brd gout of flames", {
        NextAction("brd avoid gout of flames", ACTION_EMERGENCY + 3) }));
    // Lord Incendius: the tank keeps a wall behind it against Mighty Blow
    triggers.push_back(new TriggerNode("brd incendius wall", {
        NextAction("brd incendius tank at wall", ACTION_MOVE + 5) }));
    // Moira (with the Emperor), Anvilrage Medics, Argelmach's golems, Verek: killed first
    triggers.push_back(new TriggerNode("brd kill first", {
        NextAction("brd kill first", ACTION_RAID + 2) }));
}

void ClassicDungeonBlackrockDepthsStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    Player* bot = botAI->GetBot();
    if (type != TargetValueExclusionType::DpsTarget || !PlayerbotAI::IsDps(bot) || PlayerbotAI::IsTank(bot) ||
        PlayerbotAI::IsHeal(bot))
        return;

    Unit* first = BlackrockDepths::KillFirstTarget(botAI);
    if (!first)
        return;

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
        if (guid != first->GetGUID())
            exclusions.insert(guid);
}
