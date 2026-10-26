/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GDTriggers.h"
#include "AiObjectContext.h"
#include "AttackersValue.h"
#include "Creature.h"
#include "DungeonInteract.h"
#include "EncounterHelpers.h"
#include "InstanceScript.h"
#include "Playerbots.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace EncounterHelpers;

namespace GundrakSladran
{

static bool IsClosestSnakeWrapFreer(Player* bot, Unit* snakeWrap, float distance)
{
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || !member->IsInCombat() ||
            !GET_PLAYERBOT_AI(member) || member->GetMap() != bot->GetMap() ||
            !PlayerbotAI::IsDps(member) || member->HasAura(SPELL_SNAKE_WRAP))
        {
            continue;
        }

        float memberDistance = member->GetExactDist2d(snakeWrap);
        if (memberDistance > distance ||
            (memberDistance == distance && bot->GetGUID() < member->GetGUID()))
        {
            continue;
        }

        if (!member->IsWithinLOSInMap(snakeWrap))
        {
            continue;
        }

        return false;
    }

    return true;
}

bool IsAdd(Unit* unit)
{
    if (!unit) { return false; }

    uint32 entry = unit->GetEntry();
    return entry == NPC_SLADRAN_VIPER || entry == NPC_SLADRAN_CONSTRICTOR;
}

ObjectGuid CalculateAssignedSnakeWrap(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || !bot->IsInCombat() || !PlayerbotAI::IsDps(bot) ||
        bot->HasAura(SPELL_SNAKE_WRAP) || !bot->GetGroup())
    {
        return ObjectGuid::Empty;
    }

    std::list<Creature*> wraps;
    bot->GetCreatureListWithEntryInGrid(wraps, NPC_SNAKE_WRAP, sPlayerbotAIConfig.SightDistance);

    std::vector<std::pair<float, Creature*>> candidates;
    candidates.reserve(wraps.size());
    for (Creature* snakeWrap : wraps)
    {
        if (!snakeWrap->IsAlive() || !AttackersValue::IsPossibleTarget(snakeWrap, bot)) { continue; }

        candidates.emplace_back(bot->GetExactDist2d(snakeWrap), snakeWrap);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](std::pair<float, Creature*> const& lhs, std::pair<float, Creature*> const& rhs)
              { return lhs.first < rhs.first; });

    for (auto const& candidate : candidates)
    {
        if (!bot->IsWithinLOSInMap(candidate.second)) { continue; }

        if (IsClosestSnakeWrapFreer(bot, candidate.second, candidate.first))
        {
            return candidate.second->GetGUID();
        }
    }

    return ObjectGuid::Empty;
}

Unit* GetAssignedSnakeWrap(PlayerbotAI* botAI)
{
    ObjectGuid guid =
        botAI->GetAiObjectContext()->GetValue<ObjectGuid>("slad'ran snake wrap target")->Get();
    if (guid.IsEmpty()) { return nullptr; }

    Unit* snakeWrap = botAI->GetUnit(guid);
    if (!snakeWrap || !snakeWrap->IsAlive() || snakeWrap->GetEntry() != NPC_SNAKE_WRAP)
    {
        return nullptr;
    }

    return snakeWrap;
}

Player* GetStackTank(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || PlayerbotAI::IsTank(bot))
    {
        return nullptr;
    }

    AiObjectContext* context = botAI->GetAiObjectContext();
    if (!context->GetValue<Unit*>("find target", "slad'ran")->Get())
    {
        return nullptr;
    }

    if (GetAssignedSnakeWrap(botAI))
    {
        return nullptr;
    }

    Unit* currentTarget = context->GetValue<Unit*>("current target")->Get();
    if (currentTarget && currentTarget->IsAlive() && currentTarget->GetEntry() == NPC_SNAKE_WRAP)
    {
        return nullptr;
    }

    Player* tank = GetGroupMainTank(bot);
    if (!tank || tank == bot || tank->GetMap() != bot->GetMap())
    {
        return nullptr;
    }

    if (bot->GetExactDist2d(tank) <= STACK_LEASH_YD)
    {
        return nullptr;
    }

    return tank;
}

Unit* GetTankHoldTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || !PlayerbotAI::IsTank(bot))
    {
        return nullptr;
    }

    AiObjectContext* context = botAI->GetAiObjectContext();
    Unit* boss = context->GetValue<Unit*>("find target", "slad'ran")->Get();
    if (!boss || !boss->IsAlive())
    {
        return nullptr;
    }

    Unit* currentTarget = context->GetValue<Unit*>("current target")->Get();
    if (currentTarget && currentTarget->IsAlive())
    {
        if (currentTarget == boss)
        {
            return nullptr;
        }

        if (IsAdd(currentTarget) && bot->GetExactDist2d(currentTarget) <= TANK_PICKUP_YD)
        {
            return nullptr;
        }
    }

    return boss;
}

Unit* GetTankParkBoss(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot->IsAlive() || !PlayerbotAI::IsTank(bot))
        return nullptr;

    Unit* boss = botAI->GetAiObjectContext()->GetValue<Unit*>("find target", "slad'ran")->Get();
    Creature* creature = boss ? boss->ToCreature() : nullptr;
    if (!creature || !creature->IsAlive() || creature->GetVictim() != bot)
        return nullptr;

    return bot->GetExactDist2d(creature->GetHomePosition()) > TANK_PARK_LEASH_YD ? boss : nullptr;
}

Unit* GetEngagedBoss(PlayerbotAI* botAI)
{
    Unit* boss = botAI->GetAiObjectContext()->GetValue<Unit*>("find target", "slad'ran")->Get();
    return boss && boss->IsAlive() && boss->IsInCombat() ? boss : nullptr;
}

bool IsSnakeKiller(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    Player* ranged = nullptr;
    Player* any = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || member->GetMap() != bot->GetMap() || !GET_PLAYERBOT_AI(member) ||
            !PlayerbotAI::IsDps(member) || PlayerbotAI::IsTank(member) || PlayerbotAI::IsHeal(member) ||
            member->HasAura(SPELL_SNAKE_WRAP))
            continue;

        if (!any || member->GetGUID() < any->GetGUID())
            any = member;

        if (PlayerbotAI::IsRanged(member) && (!ranged || member->GetGUID() < ranged->GetGUID()))
            ranged = member;
    }

    return (ranged ? ranged : any) == bot;
}

bool IsOnHealer(Unit* unit)
{
    if (!unit || !unit->IsAlive() || !IsAdd(unit))
        return false;

    Player* victim = unit->GetVictim() ? unit->GetVictim()->ToPlayer() : nullptr;
    return victim && PlayerbotAI::IsHeal(victim);
}

void AppendSnakeExclusions(PlayerbotAI* botAI, GuidSet& exclusions)
{
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        uint32 const entry = guid.GetEntry();
        if (entry == NPC_SLADRAN_VIPER || entry == NPC_SLADRAN_CONSTRICTOR)
            exclusions.insert(guid);
    }
}

}

bool SladranPoisonNovaTrigger::IsActive()
{
    // [b35_gd] The tank leaves it too, away from the snake spawns (AvoidPoisonNovaAction): Slad'ran stands while he
    // casts and follows it back towards his platform after. gd-h 20261026-024524-gd-h-5H-27: the bear held him
    // through the heroic nova (3.1k and then 1.6-1.8k every second for 10 s) with snakes on it and died to its poison.
    Unit* boss = AI_VALUE2(Unit*, "find target", "slad'ran");
    if (!boss) { return false; }

    return bool(boss->FindCurrentSpellBySpellId(SPELL_POISON_NOVA));
}

bool SladranSnakeWrapTrigger::IsActive()
{
    return GundrakSladran::GetAssignedSnakeWrap(botAI) != nullptr;
}

bool SladranStackOnTankTrigger::IsActive()
{
    return GundrakSladran::GetStackTank(botAI) != nullptr;
}

bool SladranTankHoldTrigger::IsActive()
{
    return GundrakSladran::GetTankHoldTarget(botAI) != nullptr;
}

bool SladranTankParkTrigger::IsActive()
{
    return GundrakSladran::GetTankParkBoss(botAI) != nullptr;
}

bool GaldarahWhirlingSlashTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "gal'darah");
    return boss && boss->HasAura(SPELL_WHIRLING_SLASH);
}

namespace
{
struct GundrakAltar
{
    uint32 bossData;
    uint32 entry;
};

constexpr GundrakAltar GUNDRAK_ALTARS[] = {
    { GundrakIds::DATA_SLAD_RAN, GundrakIds::GO_ALTAR_OF_SLAD_RAN },
    { GundrakIds::DATA_MOORABI, GundrakIds::GO_ALTAR_OF_MOORABI },
    { GundrakIds::DATA_DRAKKARI_COLOSSUS, GundrakIds::GO_ALTAR_OF_DRAKKARI },
};

// The bridge deck (GO 192633/193188) is not in the mmaps: west landing, statue island, east landing. Heights are the
// navmesh surface at both landings and on the island.
Position const GUNDRAK_BRIDGE_PATH[] = {
    { 1751.5f, 743.45f, 119.2f, 0.0f },
    { 1766.5f, 743.45f, 118.4f, 0.0f },
    { 1784.0f, 743.45f, 118.4f, 0.0f },
    { 1801.0f, 743.45f, 119.5f, 0.0f },
};
constexpr size_t GUNDRAK_BRIDGE_POINTS = sizeof(GUNDRAK_BRIDGE_PATH) / sizeof(GUNDRAK_BRIDGE_PATH[0]);
constexpr float BRIDGE_HALF_WIDTH_YD = 4.0f;
constexpr float BRIDGE_MIN_Z = 116.5f;
// past the end of the west landing's navmesh, so a member gathering at the start never counts as crossing
constexpr float BRIDGE_ENTERED_YD = 3.0f;
constexpr float BRIDGE_WAYPOINT_REACHED_YD = 1.0f;
// central chamber west of the bridge: the three altars and the Colossus platform
constexpr float CENTRAL_CHAMBER_MIN_X = 1640.0f;
constexpr float CENTRAL_CHAMBER_HALF_WIDTH_YD = 90.0f;
constexpr float CENTRAL_CHAMBER_MAX_Z = 150.0f;
constexpr float GALDARAH_WING_MAX_X = 1990.0f;
// [b34_gd] the moat under the bridge, from the west landing to past the east landing (water surface z 110.1; the
// pool bottom box of GundrakCircle ends at x 1716)
constexpr float MOAT_MIN_X = 1745.0f;
constexpr float MOAT_MAX_X = 1830.0f;
constexpr float MOAT_MIN_Y = 715.0f;
constexpr float MOAT_MAX_Y = 775.0f;
constexpr float MOAT_MAX_Z = 113.0f;
// the deck and the east landing above it, where a member stands over a mob in the moat
constexpr float MOAT_WATCH_MAX_X = 1830.0f;
constexpr float MOAT_WATCH_HALF_WIDTH_YD = 15.0f;
constexpr float MOAT_ENGAGED_YD = 30.0f;

InstanceScript* GetGundrakInstance(Player* bot)
{
    if (bot->GetMapId() != GundrakIds::MAP_ID)
        return nullptr;

    return bot->GetInstanceScript();
}

bool AreAltarBossesDone(InstanceScript const* instance)
{
    for (GundrakAltar const& altar : GUNDRAK_ALTARS)
        if (instance->GetBossState(altar.bossData) != DONE)
            return false;
    return true;
}

bool IsEastOfBridgeStart(Player const* player)
{
    return player->GetMapId() == GundrakIds::MAP_ID && player->GetPositionZ() > BRIDGE_MIN_Z &&
           player->GetPositionX() > GUNDRAK_BRIDGE_PATH[0].GetPositionX() + BRIDGE_ENTERED_YD &&
           player->GetPositionX() < GALDARAH_WING_MAX_X;
}
}  // namespace

namespace GundrakEvent
{
bool IsReadyToStart(Player* bot, WorldObject const* anchor, float radius)
{
    if (bot->IsInCombat() || !DungeonInteract::IsGroupGathered(bot, anchor, radius))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        // nobody dead, everyone rested (bots eat and drink out of combat)
        if (!member->IsAlive() || member->GetHealthPct() < START_MIN_HEALTH_PCT ||
            (member->getPowerType() == POWER_MANA && member->GetPowerPct(POWER_MANA) < START_MIN_MANA_PCT))
            return false;
    }
    return true;
}

ObjectGuid FindAltarToUse(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = GetGundrakInstance(bot);
    if (!instance || !DungeonInteract::IsInteractor(botAI))
        return ObjectGuid::Empty;

    for (GundrakAltar const& altar : GUNDRAK_ALTARS)
    {
        if (instance->GetBossState(altar.bossData) != DONE)
            continue;

        // instance_gundrak.cpp: the boss death clears NOT_SELECTABLE, the click turns the button ACTIVE
        GameObject* go = bot->FindNearestGameObject(altar.entry, ALTAR_SEARCH_YD, true);
        if (go && go->GetGoState() == GO_STATE_READY && !go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            return go->GetGUID();
    }
    return ObjectGuid::Empty;
}

GameObject* GetAltarToUse(PlayerbotAI* botAI)
{
    ObjectGuid const guid = botAI->GetAiObjectContext()->GetValue<ObjectGuid>("gundrak altar to use")->Get();
    if (guid.IsEmpty())
        return nullptr;

    GameObject* go = botAI->GetGameObject(guid);
    if (!go || !go->isSpawned() || go->GetGoState() != GO_STATE_READY || go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
        return nullptr;

    return go;
}

Creature* GetDormantColossus(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = GetGundrakInstance(bot);
    if (!instance || bot->IsInCombat())
        return nullptr;

    EncounterState const state = instance->GetBossState(GundrakIds::DATA_DRAKKARI_COLOSSUS);
    if (state == DONE || state == IN_PROGRESS)
        return nullptr;

    // a person in the group pulls; otherwise the main tank (or the interactor without one) opens on a mojo
    Player* interactor = DungeonInteract::GetInteractor(botAI);
    if (!interactor)
        return nullptr;

    Player* puller = GetGroupMainTank(bot);
    if (!puller || !puller->IsAlive() || !GET_PLAYERBOT_AI(puller))
        puller = interactor;

    if (puller != bot)
        return nullptr;

    Creature* colossus = bot->FindNearestCreature(GundrakIds::NPC_DRAKKARI_COLOSSUS, COLOSSUS_SEARCH_YD);
    if (!colossus || colossus->IsInCombat() || !colossus->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
        return nullptr;

    return IsReadyToStart(bot, colossus, COLOSSUS_SEARCH_YD) ? colossus : nullptr;
}

Creature* FindLivingMojo(Player* bot, Creature* colossus)
{
    std::list<Creature*> mojos;
    colossus->GetCreatureListWithEntryInGrid(mojos, GundrakIds::NPC_LIVING_MOJO, MOJO_SEARCH_YD);

    Creature* nearest = nullptr;
    for (Creature* mojo : mojos)
    {
        // only the colossus' own summons wake it (npc_living_mojo::JustEngagedWith)
        if (!mojo->IsAlive() || !mojo->IsSummon())
            continue;

        if (!nearest || bot->GetExactDist(mojo) < bot->GetExactDist(nearest))
            nearest = mojo;
    }
    return nearest;
}

bool IsOnBridge(Player* bot)
{
    if (bot->GetMapId() != GundrakIds::MAP_ID)
        return false;

    float const x = bot->GetPositionX();
    return x > GUNDRAK_BRIDGE_PATH[0].GetPositionX() + BRIDGE_ENTERED_YD &&
           x < GUNDRAK_BRIDGE_PATH[GUNDRAK_BRIDGE_POINTS - 1].GetPositionX() - BRIDGE_WAYPOINT_REACHED_YD &&
           std::fabs(bot->GetPositionY() - GUNDRAK_BRIDGE_PATH[0].GetPositionY()) < BRIDGE_HALF_WIDTH_YD &&
           bot->GetPositionZ() > BRIDGE_MIN_Z;
}

bool IsBridgeOpen(Player* bot)
{
    // instance_gundrak.cpp Update: three READY statues turn the statues and the collision ACTIVE_ALTERNATIVE
    GameObject* collision = bot->FindNearestGameObject(GundrakIds::GO_GUNDRAK_COLLISION, BRIDGE_SEARCH_YD);
    return collision && collision->GetGoState() == GO_STATE_ACTIVE_ALTERNATIVE;
}

bool ShouldCrossBridge(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = GetGundrakInstance(bot);
    if (!instance || bot->IsInCombat() || instance->GetBossState(GundrakIds::DATA_GAL_DARAH) == DONE ||
        !AreAltarBossesDone(instance))
        return false;

    float const x = bot->GetPositionX();
    if (x < CENTRAL_CHAMBER_MIN_X || x > GUNDRAK_BRIDGE_PATH[0].GetPositionX() + BRIDGE_ENTERED_YD ||
        std::fabs(bot->GetPositionY() - GUNDRAK_BRIDGE_PATH[0].GetPositionY()) > CENTRAL_CHAMBER_HALF_WIDTH_YD ||
        bot->GetPositionZ() > CENTRAL_CHAMBER_MAX_Z)
        return false;

    if (!botAI->GetAiObjectContext()->GetValue<bool>("gundrak bridge open")->Get())
        return false;

    Player* leader = DungeonInteract::GetInteractor(botAI);
    if (!leader)
        if (Group* group = bot->GetGroup())
            leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());

    if (!leader || leader == bot)
        return leader == bot;

    // the others cross behind the leader
    return leader->GetInstanceId() == bot->GetInstanceId() && IsEastOfBridgeStart(leader);
}

Position const* GetBridgeStart() { return &GUNDRAK_BRIDGE_PATH[0]; }

Position const* GetNextBridgeWaypoint(Player* bot)
{
    for (size_t i = 1; i < GUNDRAK_BRIDGE_POINTS; ++i)
        if (GUNDRAK_BRIDGE_PATH[i].GetPositionX() > bot->GetPositionX() + BRIDGE_WAYPOINT_REACHED_YD)
            return &GUNDRAK_BRIDGE_PATH[i];
    return nullptr;
}

bool IsInMoat(WorldObject const* object)
{
    float const x = object->GetPositionX();
    float const y = object->GetPositionY();
    return object->GetMapId() == GundrakIds::MAP_ID && x > MOAT_MIN_X && x < MOAT_MAX_X && y > MOAT_MIN_Y &&
           y < MOAT_MAX_Y && object->GetPositionZ() < MOAT_MAX_Z;
}

Creature* FindMoatAttacker(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != GundrakIds::MAP_ID || !bot->IsAlive() || !bot->IsInCombat())
        return nullptr;

    // on the deck or the east landing, above the water
    float const x = bot->GetPositionX();
    if (bot->GetPositionZ() <= BRIDGE_MIN_Z || x < GUNDRAK_BRIDGE_PATH[0].GetPositionX() || x > MOAT_WATCH_MAX_X ||
        std::fabs(bot->GetPositionY() - GUNDRAK_BRIDGE_PATH[0].GetPositionY()) > MOAT_WATCH_HALF_WIDTH_YD)
        return nullptr;

    InstanceScript* instance = GetGundrakInstance(bot);
    if (!instance || instance->GetBossState(GundrakIds::DATA_GAL_DARAH) == IN_PROGRESS)
        return nullptr;

    // anything it can fight up here comes first
    if (!botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get().empty())
        return nullptr;

    Creature* nearest = nullptr;
    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
    {
        Unit* other = ref ? ref->GetOther(bot) : nullptr;
        Creature* mob = other ? other->ToCreature() : nullptr;
        if (!mob || !mob->IsAlive() || !mob->IsInWorld() || mob->GetMap() != bot->GetMap() || mob->IsInEvadeMode() ||
            !IsInMoat(mob) || bot->GetExactDist2d(mob) > MOAT_ENGAGED_YD || !bot->IsValidAttackTarget(mob))
            continue;

        if (!nearest || bot->GetExactDist(mob) < bot->GetExactDist(nearest))
            nearest = mob;
    }
    return nearest;
}

namespace
{
std::mutex moatDropLock;
// bot guid counter -> (drop start, drop duration) in getMSTime() ms
std::unordered_map<ObjectGuid::LowType, std::pair<uint32, uint32>> moatDrops;
}  // namespace

void NoteMoatDrop(Player* bot, uint32 durationMs)
{
    std::lock_guard<std::mutex> guard(moatDropLock);
    if (moatDrops.size() > 256)
        moatDrops.clear();
    moatDrops[bot->GetGUID().GetCounter()] = std::make_pair(getMSTime(), durationMs);
}

bool IsDroppingIntoMoat(Player* bot)
{
    std::lock_guard<std::mutex> guard(moatDropLock);
    auto const it = moatDrops.find(bot->GetGUID().GetCounter());
    if (it == moatDrops.end())
        return false;

    if (getMSTimeDiff(it->second.first, getMSTime()) < it->second.second)
        return true;

    moatDrops.erase(it);
    return false;
}

bool ShouldLeaveMoat(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = GetGundrakInstance(bot);
    if (!instance || !bot->IsAlive() || bot->IsInCombat() || !IsInMoat(bot) ||
        instance->GetBossState(GundrakIds::DATA_GAL_DARAH) == DONE || !AreAltarBossesDone(instance))
        return false;

    return botAI->GetAiObjectContext()->GetValue<bool>("gundrak bridge open")->Get();
}
}  // namespace GundrakEvent

namespace GundrakCircle
{
bool IsOnPoolFloor(Player* bot)
{
    if (bot->GetMapId() != GundrakIds::MAP_ID)
        return false;

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    return x > POOL_MIN_X && x < POOL_MAX_X && y > POOL_MIN_Y && y < POOL_MAX_Y && z > FLOOR_MIN_Z &&
           z < FLOOR_MAX_Z;
}

bool HasBossAhead(Player* bot)
{
    InstanceScript* instance = GetGundrakInstance(bot);
    if (!instance)
        return false;

    return instance->GetBossState(GundrakIds::DATA_SLAD_RAN) != DONE ||
           instance->GetBossState(GundrakIds::DATA_MOORABI) != DONE ||
           instance->GetBossState(GundrakIds::DATA_DRAKKARI_COLOSSUS) != DONE ||
           instance->GetBossState(GundrakIds::DATA_GAL_DARAH) != DONE;
}
}  // namespace GundrakCircle

bool GundrakCircleRouteTrigger::IsActive()
{
    return bot->IsAlive() && !bot->IsInCombat() && GundrakCircle::IsOnPoolFloor(bot) &&
           GundrakCircle::HasBossAhead(bot);
}

bool GundrakAltarReadyTrigger::IsActive()
{
    return bot->GetMapId() == GundrakIds::MAP_ID && !bot->IsInCombat() && GundrakEvent::GetAltarToUse(botAI);
}

bool DrakkariColossusDormantTrigger::IsActive() { return GundrakEvent::GetDormantColossus(botAI) != nullptr; }

bool GundrakBridgeFormedTrigger::IsActive()
{
    if (bot->GetMapId() != GundrakIds::MAP_ID)
        return false;

    // [b34_gd] the drop off the deck into the moat passes over the deck: not walked back to the landing
    if (GundrakEvent::IsDroppingIntoMoat(bot))
        return false;

    return GundrakEvent::IsOnBridge(bot) || GundrakEvent::ShouldCrossBridge(botAI);
}

bool GundrakMoatFightTrigger::IsActive()
{
    if (bot->GetMapId() != GundrakIds::MAP_ID)
        return false;

    if (GundrakEvent::IsDroppingIntoMoat(bot))
        return true;

    if (!GundrakEvent::FindMoatAttacker(botAI))
    {
        _seenMs = 0;
        return false;
    }

    // held a few seconds: the attackers list can be empty for a moment between two targets
    uint32 const now = getMSTime();
    if (!_seenMs)
        _seenMs = now ? now : 1;
    return getMSTimeDiff(_seenMs, now) >= GundrakEvent::MOAT_DROP_HOLD_MS;
}

bool GundrakMoatExitTrigger::IsActive()
{
    return bot->GetMapId() == GundrakIds::MAP_ID && GundrakEvent::ShouldLeaveMoat(botAI);
}
