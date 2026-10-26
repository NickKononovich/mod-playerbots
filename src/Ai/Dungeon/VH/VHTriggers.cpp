/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "VHTriggers.h"
#include "AiObjectContext.h"
#include "Creature.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "Playerbots.h"
#include "SpellAuras.h"

namespace VHShared
{
uint32 EventStatus(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    return instance ? instance->GetData(VHIds::DATA_ENCOUNTER_STATUS) : static_cast<uint32>(NOT_STARTED);
}

bool IsEventInProgress(Player* bot) { return EventStatus(bot) == IN_PROGRESS; }

bool IsOutsideSeal(Player* bot)
{
    if (bot->GetPositionX() >= VHIds::SEAL_X || !IsEventInProgress(bot))
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    GameObject* door = instance ? instance->GetGameObject(VHIds::DATA_MAIN_DOOR) : nullptr;
    return door && door->GetGoState() == GO_STATE_READY;
}

bool IsSealUnderAttack(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance)
        return false;

    Creature* seal = instance->GetCreature(VHIds::DATA_DOOR_SEAL);
    return seal && seal->HasAura(VHIds::SPELL_DESTROY_DOOR_SEAL);
}

// Trash that reached the door is ImmuneToNPC and channels Destroy Door Seal on the seal; its caster is the target.
Unit* NearestSealAttacker(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance)
        return nullptr;

    Creature* seal = instance->GetCreature(VHIds::DATA_DOOR_SEAL);
    if (!seal)
        return nullptr;

    Unit* nearest = nullptr;
    auto const bounds = seal->GetAppliedAuras().equal_range(VHIds::SPELL_DESTROY_DOOR_SEAL);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
    {
        Aura const* aura = itr->second ? itr->second->GetBase() : nullptr;
        Unit* caster = aura ? aura->GetCaster() : nullptr;
        if (!caster || !caster->IsAlive() || !caster->IsInWorld())
            continue;

        if (!nearest || bot->GetExactDist(caster) < bot->GetExactDist(nearest))
            nearest = caster;
    }
    return nearest;
}

// Guardian/Keeper portals stay visible while channelled; elite-squad portals turn invisible right after the spawn.
Creature* ActiveKeeperPortal(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetData(VHIds::DATA_ENCOUNTER_STATUS) != IN_PROGRESS)
        return nullptr;

    uint32 const wave = instance->GetData(VHIds::DATA_WAVE_COUNT);
    if (wave % VHIds::WAVE_FIRST_BOSS == 0)
        return nullptr;

    Creature* portal = instance->GetCreature(VHIds::DATA_TELEPORTATION_PORTAL);
    if (!portal || !portal->IsAlive() || !portal->IsVisible())
        return nullptr;

    return portal;
}

Unit* PortalKeeper(Creature* portal)
{
    if (!portal)
        return nullptr;

    for (uint32 entry : { VHIds::NPC_PORTAL_KEEPER_1, VHIds::NPC_PORTAL_KEEPER_2, VHIds::NPC_PORTAL_GUARDIAN })
        if (Creature* keeper = portal->FindNearestCreature(entry, VHIds::KEEPER_SEARCH_RADIUS))
            return keeper;

    return nullptr;
}

bool IsPortalKeeperPhase(Player* bot)
{
    return !PlayerbotAI::IsHeal(bot) && !IsOutsideSeal(bot) && !IsSealUnderAttack(bot) && ActiveKeeperPortal(bot);
}

// The boss released by the Saboteur (waves 6/12) or Cyanigosa (wave 18) once it became attackable.
Creature* ReleasedBoss(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetData(VHIds::DATA_ENCOUNTER_STATUS) != IN_PROGRESS)
        return nullptr;

    auto const attackable = [](Creature* boss)
    { return boss && boss->IsAlive() && !boss->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) && !boss->IsImmuneToPC(); };

    uint32 const wave = instance->GetData(VHIds::DATA_WAVE_COUNT);
    if (wave == VHIds::WAVE_CYANIGOSA)
    {
        Creature* cyanigosa = instance->GetCreature(VHIds::DATA_CYANIGOSA);
        return attackable(cyanigosa) ? cyanigosa : nullptr;
    }

    if (wave != VHIds::WAVE_FIRST_BOSS && wave != VHIds::WAVE_SECOND_BOSS)
        return nullptr;

    for (uint32 id = VHIds::BOSS_MORAGG; id <= VHIds::BOSS_ZURAMAT; ++id)
        if (Creature* boss = instance->GetCreature(id); attackable(boss))
            return boss;

    return nullptr;
}
}  // namespace VHShared

bool ErekemTargetTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "erekem");
    if (!boss) { return false; }

    return PlayerbotAI::IsDps(bot);
}

bool IchoronTargetTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ichoron");
    if (!boss) { return false; }

    return !PlayerbotAI::IsHeal(bot);
}

bool VoidShiftTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "zuramat the obliterator");
    if (!boss) { return false; }

    return bot->HasAura(SPELL_VOID_SHIFTED) && !PlayerbotAI::IsHeal(bot);
}

bool ShroudOfDarknessTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "zuramat the obliterator");
    if (!boss) { return false; }

    return boss->HasAura(SPELL_SHROUD_OF_DARKNESS);
}

bool CyanigosaPositioningTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "cyanigosa");
    if (!boss) { return false; }

    // Include healers here for now, otherwise they stand in things
    return !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsRangedDps(bot);
    // return PlayerbotAI::IsMelee(bot) && !PlayerbotAI::IsTank(bot);
}

bool VHSinclariStartTrigger::IsActive()
{
    if (bot->IsInCombat() || VHShared::EventStatus(bot) != NOT_STARTED || !DungeonInteract::IsInteractor(botAI))
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    Creature* sinclari = instance ? instance->GetCreature(VHIds::DATA_SINCLARI) : nullptr;
    return sinclari && sinclari->IsAlive() && sinclari->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
}

bool VHSealUnderAttackTrigger::IsActive()
{
    return !PlayerbotAI::IsHeal(bot) && VHShared::IsEventInProgress(bot) && !VHShared::IsOutsideSeal(bot) &&
           VHShared::IsSealUnderAttack(bot);
}

bool VHPortalKeeperTrigger::IsActive() { return VHShared::IsPortalKeeperPhase(bot); }

bool VHReleasedBossTrigger::IsActive()
{
    if (PlayerbotAI::IsHeal(bot) || VHShared::IsOutsideSeal(bot))
        return false;

    Creature* boss = VHShared::ReleasedBoss(bot);
    // In combat itself (the tank swinging at a prisoner out of reach, Moragg in his cell's mouth) while the
    // prisoner is not: the engage goes on until it is.
    if (bot->IsInCombat() && (!boss || boss->IsInCombat()))
        return false;
    // The tank pulls; the zone-wide combat of BossAI then brings everyone else in.
    return boss && (PlayerbotAI::IsTank(bot) || boss->IsInCombat());
}

bool VHRallyTrigger::IsActive()
{
    // Outside the seal the way in is Sinclari ("vh late join"), not a walk through the closed door.
    if (bot->IsInCombat() || !VHShared::IsEventInProgress(bot) || VHShared::IsOutsideSeal(bot))
        return false;

    if (bot->GetExactDist2d(VHIds::RALLY_X, VHIds::RALLY_Y) <= VHIds::RALLY_RADIUS)
        return false;

    // A released prisoner waits in front of its cell for the group (it never comes by itself): the rally would pull
    // the tank back from it every time it walked out to engage (vh-n/h 2026-10-25: neither prisoner engaged).
    if (VHShared::ReleasedBoss(bot))
        return false;

    return !VHShared::IsSealUnderAttack(bot) && !VHShared::ActiveKeeperPortal(bot);
}

bool VHLateJoinTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || !VHShared::IsOutsideSeal(bot))
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    Creature* sinclari = instance ? instance->GetCreature(VHIds::DATA_SINCLARI) : nullptr;
    return sinclari && sinclari->IsAlive() && sinclari->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
}

bool XevozzEtherealSphereTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "xevozz");
    if (!boss || boss->GetVictim() != bot)
        return false;

    return boss->FindNearestCreature(VHIds::NPC_ETHEREAL_SPHERE, VHIds::SPHERE_DANGER_RADIUS) != nullptr;
}
