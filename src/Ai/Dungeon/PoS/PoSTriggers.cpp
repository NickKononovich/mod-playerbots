/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PoSTriggers.h"
#include "AiObjectContext.h"
#include "DungeonHoldFire.h"
#include "DungeonInteract.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <string>

namespace
{
// Tunnel of the falling icicles: bounding box of its 28 Invisible Stalkers 36848 (world DB) plus a margin.
constexpr float TUNNEL_MIN_X = 944.0f;
constexpr float TUNNEL_MAX_X = 1092.0f;
constexpr float TUNNEL_MIN_Y = -148.0f;
constexpr float TUNNEL_MAX_Y = 94.0f;

constexpr float ROCK_SEARCH_RANGE = 60.0f;
constexpr float ROCK_HIDE_OFFSET = 2.5f;        // behind the rock, inside the 4 yd band of the script
constexpr float ROCK_MIN_BOSS_DISTANCE = 8.0f;  // a spot this close sits in melee range: always hit
constexpr uint32 SARONITE_DODGE_MS = 1500;      // cast end + missile flight

bool IsInTunnel(Player* bot)
{
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    return x >= TUNNEL_MIN_X && x <= TUNNEL_MAX_X && y >= TUNNEL_MIN_Y && y <= TUNNEL_MAX_Y;
}

bool IsUsableRock(GameObject* rock)
{
    return rock && rock->isSpawned() && !rock->IsInvisibleDueToDespawn();
}

// The route triggers in order: standing in a later one while an earlier one is still pending means the group walked
// on before an ambush wave died (the script only advances on a trigger received after the kills).
void WarnIfPastAreaTrigger(Player* bot, uint32 pendingId)
{
    constexpr std::array<uint32, 4> ROUTE = {PoSIds::AT_AMBUSH_1, PoSIds::AT_AMBUSH_2, PoSIds::AT_TUNNEL,
                                             PoSIds::AT_TYRANNUS_INTRO};
    auto const pending = std::find(ROUTE.begin(), ROUTE.end(), pendingId);
    if (pending == ROUTE.end())
        return;

    for (auto later = pending + 1; later != ROUTE.end(); ++later)
    {
        AreaTrigger const* areaTrigger = sObjectMgr->GetAreaTrigger(*later);
        if (!areaTrigger || !bot->IsInAreaTriggerRadius(areaTrigger))
            continue;

        RaidBotDuty::LogWorkaroundOnce(bot, "pos-at-passed-" + std::to_string(pendingId),
                                       "Pit of Saron: needs path fix: inside area trigger " + std::to_string(*later) +
                                           " while the Tyrannus event still waits for " +
                                           std::to_string(pendingId) + " (go back once the ambush wave is dead)");
        return;
    }
}
}

namespace PoSShared
{
InstanceScript* GetInstance(Player* bot)
{
    if (!bot || bot->GetMapId() != PoSIds::MAP_PIT_OF_SARON)
        return nullptr;

    return bot->GetInstanceScript();
}

Unit* GetGarfrost(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return nullptr;

    Creature* garfrost = bot->GetMap()->GetCreature(instance->GetGuidData(PoSIds::DATA_GARFROST_GUID));
    return garfrost && garfrost->IsAlive() && garfrost->IsInCombat() ? garfrost : nullptr;
}

uint8 PermafrostStacks(Unit* unit)
{
    uint8 stacks = 0;
    if (Aura* aura = unit->GetAura(PoSIds::SPELL_PERMAFROST_STACK))
        stacks = aura->GetStackAmount();

    if (Aura* aura = unit->GetAura(PoSIds::SPELL_PERMAFROST_STACK_H))
        stacks = std::max<uint8>(stacks, aura->GetStackAmount());

    return stacks;
}

GameObject* GetCoveringRock(Player* bot, Unit* garfrost)
{
    if (!garfrost || garfrost->IsWithinMeleeRange(bot))
        return nullptr;

    std::list<GameObject*> rocks;
    bot->GetGameObjectListWithEntryInGrid(rocks, PoSIds::GO_SARONITE_ROCK, ROCK_SEARCH_RANGE);
    for (GameObject* rock : rocks)
        if (IsUsableRock(rock) && rock->IsInBetween(garfrost, bot, PoSIds::PERMAFROST_ROCK_SIZE))
            return rock;

    return nullptr;
}

GameObject* FindHidingSpot(Player* bot, Unit* garfrost, Position& spot)
{
    std::list<GameObject*> rocks;
    bot->GetGameObjectListWithEntryInGrid(rocks, PoSIds::GO_SARONITE_ROCK, ROCK_SEARCH_RANGE);

    GameObject* best = nullptr;
    float bestDistance = 0.0f;
    for (GameObject* rock : rocks)
    {
        if (!IsUsableRock(rock) || rock->GetExactDist2d(garfrost) < ROCK_MIN_BOSS_DISTANCE)
            continue;

        float const angle = garfrost->GetAngle(rock);
        Position const candidate(rock->GetPositionX() + std::cos(angle) * ROCK_HIDE_OFFSET,
                                 rock->GetPositionY() + std::sin(angle) * ROCK_HIDE_OFFSET, rock->GetPositionZ());
        float const distance = bot->GetExactDist2d(candidate);
        if (!best || distance < bestDistance)
        {
            best = rock;
            bestDistance = distance;
            spot = candidate;
        }
    }

    return best;
}

bool ShouldHideFromPermafrost(Player* bot)
{
    constexpr uint8 HIDE_STACKS_NORMAL = 9;
    constexpr uint8 HIDE_STACKS_HEROIC = 6;
    // Melee damage dealers keep hitting longer, then drop their stacks behind a rock too (wave 10: a dps took Deep
    // Freeze at ~13 heroic stacks, 6000 x 3.6 = 21.6k). The Deep Freeze target (70381 / 72930, 14 s until it
    // lands) clears its stacks first whatever its role.
    constexpr uint8 MELEE_HIDE_STACKS_NORMAL = 15;
    constexpr uint8 MELEE_HIDE_STACKS_HEROIC = 10;
    constexpr uint8 DEEP_FREEZE_HIDE_STACKS = 3;
    constexpr uint32 SPELL_DEEP_FREEZE = 70381;
    constexpr uint32 SPELL_DEEP_FREEZE_H = 72930;

    // the tank is in melee range, where Permafrost always lands
    if (PlayerbotAI::IsTank(bot))
        return false;

    uint8 const stacks = PermafrostStacks(bot);
    if (!stacks)
        return false;

    Unit* garfrost = GetGarfrost(bot);
    if (!garfrost)
        return false;

    bool const deepFreeze = bot->HasAura(SPELL_DEEP_FREEZE) || bot->HasAura(SPELL_DEEP_FREEZE_H);
    bool const meleeDps = PlayerbotAI::IsMelee(bot) && !PlayerbotAI::IsHeal(bot);
    uint8 const threshold = deepFreeze ? DEEP_FREEZE_HIDE_STACKS
                            : meleeDps ? DUNGEON_MODE(bot, MELEE_HIDE_STACKS_NORMAL, MELEE_HIDE_STACKS_HEROIC)
                                       : DUNGEON_MODE(bot, HIDE_STACKS_NORMAL, HIDE_STACKS_HEROIC);
    if (stacks >= threshold)
        return true;

    // already behind a rock: stay there until the stacks fall off
    return GetCoveringRock(bot, garfrost) != nullptr;
}

uint32 PendingAreaTrigger(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance || instance->GetData(PoSIds::DATA_GARFROST) != DONE || instance->GetData(PoSIds::DATA_ICK) != DONE ||
        instance->GetData(PoSIds::DATA_TYRANNUS) == DONE)
        return 0;

    switch (instance->GetData(PoSIds::DATA_INSTANCE_PROGRESS))
    {
        case PoSIds::PROGRESS_FINISHED_KRICK_SCENE:
            return PoSIds::AT_AMBUSH_1;
        case PoSIds::PROGRESS_AFTER_WARN_1:
            return PoSIds::AT_AMBUSH_2;
        case PoSIds::PROGRESS_AFTER_WARN_2:
            return PoSIds::AT_TUNNEL;
        case PoSIds::PROGRESS_AFTER_TUNNEL_WARN:
            return PoSIds::AT_TYRANNUS_INTRO;
        default:
            return 0;
    }
}

Unit* FindFallingIcicle(Player* bot)
{
    constexpr float ICICLE_SEARCH_RANGE = PoSIds::ICICLE_RADIUS + 2.0f;

    std::list<Creature*> icicles;
    bot->GetCreatureListWithEntryInGrid(icicles, PoSIds::NPC_COLLAPSING_ICICLE, ICICLE_SEARCH_RANGE);

    Unit* nearest = nullptr;
    for (Creature* icicle : icicles)
    {
        if (!icicle || !icicle->IsAlive())
            continue;

        // landed: the long fall cast still runs while the impact aura (one Ice Shards tick) is already gone
        if (icicle->FindCurrentSpellBySpellId(PoSIds::SPELL_ICICLE_FALL) &&
            !icicle->HasAura(PoSIds::SPELL_ICICLE_IMPACT))
            continue;

        if (!nearest || bot->GetExactDist2d(icicle) < bot->GetExactDist2d(nearest))
            nearest = icicle;
    }

    return nearest;
}

Player* FindMarkedMember(Player* bot, float range)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return nullptr;

    Creature* rimefang = bot->GetMap()->GetCreature(instance->GetGuidData(PoSIds::DATA_RIMEFANG_GUID));
    Spell* spell = rimefang ? rimefang->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    if (!spell || !spell->m_spellInfo || spell->m_spellInfo->Id != RIMEFANG_SPELL_HOARFROST)
        return nullptr;

    Unit* target = ObjectAccessor::GetUnit(*rimefang, spell->m_targets.GetUnitTargetGUID());
    Player* marked = target && target->IsInWorld() && target->GetMap() == rimefang->GetMap() ? target->ToPlayer()
                                                                                             : nullptr;
    if (!marked || !marked->IsAlive())
        return nullptr;

    if (marked != bot)
        return bot->IsWithinDist(marked, range) ? marked : nullptr;

    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member != bot && DungeonInteract::IsActiveMember(bot, member) && bot->IsWithinDist(member, range))
            return bot;
    }

    return nullptr;
}
bool IsPoisonNova(Unit* ick)
{
    return ick && (ick->FindCurrentSpellBySpellId(SPELL_POISON_NOVA_POS) ||
                   ick->FindCurrentSpellBySpellId(SPELL_POISON_NOVA_POS_HC));
}

float PoisonNovaDangerRadius(Player* bot)
{
    constexpr float radius = 15.0f;
    return radius + bot->GetCombatReach();
}

// [b33_pos]
Unit* GetTyrannus(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance || instance->GetData(PoSIds::DATA_TYRANNUS) != IN_PROGRESS)
        return nullptr;

    Creature* tyrannus = bot->GetMap()->GetCreature(instance->GetGuidData(PoSIds::DATA_TYRANNUS_GUID));
    return tyrannus && tyrannus->IsAlive() ? tyrannus : nullptr;
}

bool IsSlaveFightWarrior(Unit* unit)
{
    if (!unit || !unit->IsAlive() || unit->GetEntry() != PoSIds::NPC_FALLEN_WARRIOR ||
        unit->GetMapId() != PoSIds::MAP_PIT_OF_SARON)
        return false;

    // on its way in (no victim yet) or on a freed slave; one that hits a player or a pet is the group's to kill
    Unit* victim = unit->GetVictim();
    return !victim || !victim->GetCharmerOrOwnerPlayerOrPlayerItself();
}

bool TyrannusSpotFor(Player* bot, Position& spot)
{
    constexpr float PLATFORM_RANGE = 110.0f;   // Tyrannus evades beyond 100 yd of TSDistCheckPos
    constexpr float PLATFORM_HEIGHT = 15.0f;
    constexpr float TANK_SLACK = 10.0f;        // the tank may give this much ground to the puddles first
    constexpr float NON_TANK_MARGIN = 8.0f;    // a non-tank stops this far beyond the keep-out radius

    if (!bot->IsAlive() || !GetTyrannus(bot))
        return false;

    Position const& line = PoSIds::SLAVE_LINE_CENTER;
    Position const& tankSpot = PoSIds::TYRANNUS_TANK_SPOT;
    if (bot->GetExactDist2d(tankSpot) > PLATFORM_RANGE ||
        std::fabs(bot->GetPositionZ() - tankSpot.GetPositionZ()) > PLATFORM_HEIGHT)
        return false;

    float const fromLine = bot->GetExactDist2d(line);
    if (PlayerbotAI::IsTank(bot))
    {
        if (fromLine >= PoSIds::SLAVE_LINE_KEEP_OUT + TANK_SLACK)
            return false;

        spot = tankSpot;
        return true;
    }

    if (fromLine >= PoSIds::SLAVE_LINE_KEEP_OUT)
        return false;

    // on the slave side of the tank spot, short of it: ranged keep their distance from the tank
    float const dx = tankSpot.GetPositionX() - line.GetPositionX();
    float const dy = tankSpot.GetPositionY() - line.GetPositionY();
    float const length = std::sqrt(dx * dx + dy * dy);
    float const reach = std::min(length, PoSIds::SLAVE_LINE_KEEP_OUT + NON_TANK_MARGIN);
    spot.Relocate(line.GetPositionX() + dx / length * reach, line.GetPositionY() + dy / length * reach,
                  tankSpot.GetPositionZ());
    return true;
}

bool BrandDamageHitsAlly(Player* bot)
{
    Unit* tyrannus = GetTyrannus(bot);
    if (!tyrannus)
        return !PlayerbotAI::IsTank(bot);

    return !PlayerbotAI::IsTank(bot) || tyrannus->GetVictim() != bot;
}
}  // namespace PoSShared

bool IckAndKrickTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "Ick");
    if (!boss)
        return false;

    return true;
}

bool TyrannusTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "scourgelord tyrannus");
    if (!boss)
        return false;

    return true;
}

bool PoSAreaTriggerTrigger::IsActive()
{
    uint32 const triggerId = PoSShared::PendingAreaTrigger(bot);
    if (!triggerId || !DungeonInteract::IsInteractor(botAI))
        return false;

    AreaTrigger const* areaTrigger = sObjectMgr->GetAreaTrigger(triggerId);
    if (!areaTrigger)
        return false;

    if (bot->IsInAreaTriggerRadius(areaTrigger))
        return true;

    WarnIfPastAreaTrigger(bot, triggerId);
    return false;
}

bool PoSPermafrostTrigger::IsActive()
{
    return PoSShared::ShouldHideFromPermafrost(bot);
}

bool PoSThrowSaroniteTrigger::IsActive()
{
    uint32 const now = getMSTime();
    if (!PlayerbotAI::IsTank(bot))
    {
        Unit* garfrost = PoSShared::GetGarfrost(bot);
        Spell* spell = garfrost ? garfrost->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
        if (spell && spell->m_spellInfo && spell->m_spellInfo->Id == PoSIds::SPELL_THROW_SARONITE &&
            spell->m_targets.GetUnitTarget() == bot)
            dodgeUntil = now + SARONITE_DODGE_MS;
    }

    if (dodgeUntil && now < dodgeUntil)
        return true;

    dodgeUntil = 0;
    return false;
}

bool PoSIcicleTrigger::IsActive()
{
    InstanceScript* instance = PoSShared::GetInstance(bot);
    if (!instance || instance->GetData(PoSIds::DATA_INSTANCE_PROGRESS) != PoSIds::PROGRESS_AFTER_TUNNEL_WARN ||
        !IsInTunnel(bot))
        return false;

    return PoSShared::FindFallingIcicle(bot) != nullptr;
}

bool PoSOverlordsBrandTrigger::IsActive()
{
    if (!bot->HasAura(PoSIds::SPELL_OVERLORDS_BRAND))
        return false;

    if (DungeonHoldFire::IsCastingHeal(bot))
        return true;

    // [b33_pos] a tank only keeps hitting while Tyrannus hits it (the copies land on the tank itself): a branded
    // bear tank revived after the wipe started went on swiping while Tyrannus was on the mage and the cat, and its
    // copies killed both (pos-n 20261026-000434-5H-54, "killer Rtdruar / Overlord's Brand")
    return PoSShared::BrandDamageHitsAlly(bot) && DungeonHoldFire::IsDamagingAnything(botAI);
}

bool PoSTyrannusPositionTrigger::IsActive()  // [b33_pos]
{
    Position spot;
    return PoSShared::TyrannusSpotFor(bot, spot);
}

bool PoSMarkOfRimefangTrigger::IsActive()
{
    InstanceScript* instance = PoSShared::GetInstance(bot);
    if (!instance || instance->GetData(PoSIds::DATA_TYRANNUS) != IN_PROGRESS)
        return false;

    Player* marked = PoSShared::FindMarkedMember(bot, PoSIds::HOARFROST_SPREAD);
    if (!marked)
        return false;

    // a marked tank holds Tyrannus where it is; the others make room instead
    return marked != bot || !PlayerbotAI::IsTank(bot);
}
