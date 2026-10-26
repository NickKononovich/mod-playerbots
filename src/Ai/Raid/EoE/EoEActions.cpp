/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EoEActions.h"
#include "EoETriggers.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include <algorithm>
#include <cmath>

bool MalygosPositionAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "malygos");
    if (!boss) { return false; }

    uint8 phase = MalygosTrigger::getPhase(bot, boss);

    float distance = 5.0f;

    if (phase == 1)
    {
        Unit* spark = nullptr;

        GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
        for (auto& target : targets)
        {
            Unit* unit = botAI->GetUnit(target);
            if (unit && unit->GetEntry() == NPC_POWER_SPARK)
            {
                spark = unit;
                break;
            }
        }

        // Position tank
        if (PlayerbotAI::IsMainTank(bot))
        {
            if (bot->GetDistance2d(MALYGOS_MAINTANK_POSITION.first, MALYGOS_MAINTANK_POSITION.second) > distance)
            {
                return MoveTo(EOE_MAP_ID, MALYGOS_MAINTANK_POSITION.first, MALYGOS_MAINTANK_POSITION.second, bot->GetPositionZ(),
                    false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
            }
            return false;
        }
        // Position DK for spark pull
        // else if (spark && bot->IsClass(CLASS_DEATH_KNIGHT))
        // {
        //     if (bot->GetDistance2d(MALYGOS_STACK_POSITION.first, MALYGOS_STACK_POSITION.second) > distance)
        //     {
        //         bot->Yell("SPARK SPAWNED, MOVING TO STACK", LANG_UNIVERSAL);
        //         return MoveTo(EOE_MAP_ID, MALYGOS_STACK_POSITION.first, MALYGOS_STACK_POSITION.second, bot->GetPositionZ(),
        //             false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
        //     }
        //     return false;
        // }
        else if (spark)
        {
            return false;
        }
        else if (!bot->IsClass(CLASS_HUNTER))
        {
            if (bot->GetDistance2d(MALYGOS_STACK_POSITION.first, MALYGOS_STACK_POSITION.second) > (distance * 3.0f))
            {
                return MoveTo(EOE_MAP_ID, MALYGOS_STACK_POSITION.first, MALYGOS_STACK_POSITION.second, bot->GetPositionZ(),
                    false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
            }
            return false;
        }
    }

    return false;
}

bool MalygosTargetAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "malygos");
    if (!boss) { return false; }

    uint8 phase = MalygosTrigger::getPhase(bot, boss);

    if (phase == 1)
    {
        if (PlayerbotAI::IsHeal(bot)) { return false; }

        // Init this as boss by default, if no better target is found just fall back to Malygos
        Unit* newTarget = boss;
        // Unit* spark = nullptr;

        // GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
        // for (auto& target : targets)
        // {
        //     Unit* unit = botAI->GetUnit(target);
        //     if (unit && unit->GetEntry() == NPC_POWER_SPARK)
        //     {
        //         spark = unit;
        //         break;
        //     }
        // }

        // if (spark && PlayerbotAI::IsRangedDps(bot))
        // {
        //     newTarget = spark;
        // }

        Unit* currentTarget = AI_VALUE(Unit*, "current target");

        if (!currentTarget || currentTarget->GetEntry() != newTarget->GetEntry())
        {
            return Attack(newTarget);
        }
    }
    else if (phase == 2)
    {
        if (PlayerbotAI::IsHeal(bot)) { return false; }

        Unit* newTarget = nullptr;
        Unit* nexusLord = nullptr;
        Unit* scionOfEternity = nullptr;

        GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
        for (auto& target : targets)
        {
            Unit* unit = botAI->GetUnit(target);
            if (!unit) { continue; }

            if (unit->GetEntry() == NPC_NEXUS_LORD)
            {
                nexusLord = unit;
            }
            else if (unit->GetEntry() == NPC_SCION_OF_ETERNITY)
            {
                scionOfEternity = unit;
            }
        }

        if (PlayerbotAI::IsRangedDps(bot) && scionOfEternity)
        {
            newTarget = scionOfEternity;
        }
        else
        {
            newTarget = nexusLord;
        }

        if (!newTarget) { return false; }

        Unit* currentTarget = AI_VALUE(Unit*, "current target");
        if (!currentTarget || currentTarget->GetEntry() != newTarget->GetEntry())
        {
            return Attack(newTarget);
        }
    }

    // else if (phase == 3)
    // {}

    return false;
}

// bool PullPowerSparkAction::Execute(Event event)
// {
//     Unit* spark = nullptr;

//     GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
//     for (auto& target : targets)
//     {
//         Unit* unit = botAI->GetUnit(target);
//         if (unit && unit->GetEntry() == NPC_POWER_SPARK)
//         {
//             spark = unit;
//             break;
//         }
//     }

//     if (!spark) { return false; }

//     if (spark->GetDistance2d(MALYGOS_STACK_POSITION.first, MALYGOS_STACK_POSITION.second) > 3.0f)
//     {
//         bot->Yell("GRIPPING SPARK", LANG_UNIVERSAL);
//         return botAI->CastSpell("death grip", spark);
//     }

//     return false;
// }

// bool PullPowerSparkAction::isPossible()
// {
//     Unit* spark = nullptr;

//     GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
//     for (auto& target : targets)
//     {
//         Unit* unit = botAI->GetUnit(target);
//         if (unit && unit->GetEntry() == NPC_POWER_SPARK)
//         {
//             spark = unit;
//             break;
//         }
//     }

//     return botAI->CanCastSpell(spell, spark);
// }

// bool PullPowerSparkAction::isUseful()
// {
//     Unit* spark = nullptr;

//     GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
//     for (auto& target : targets)
//     {
//         Unit* unit = botAI->GetUnit(target);
//         if (unit && unit->GetEntry() == NPC_POWER_SPARK)
//         {
//             spark = unit;
//             break;
//         }
//     }

//     if (!spark)
//         return false;

//     if (!spark->IsInWorld() || spark->GetMapId() != bot->GetMapId())
//         return false;

//     return bot->GetDistance2d(MALYGOS_STACK_POSITION.first, MALYGOS_STACK_POSITION.second) < 3.0f;
// }

// bool KillPowerSparkAction::Execute(Event event)
// {
//     return false;
// }

bool EoEFlyDrakeAction::isPossible()
{
    Unit* vehicleBase = bot->GetVehicleBase();
    return (vehicleBase && vehicleBase->GetEntry() == NPC_WYRMREST_SKYTALON);
}
bool EoEFlyDrakeAction::Execute(Event /*event*/)
{
    Unit* vehicleBase = bot->GetVehicleBase();
    if (!vehicleBase) { return false; }

    // AC #25184: MoveFollow does not work on a Wyrmrest Skytalon, so following the master's drake leaves the
    // bot behind. Each drake instead flies (MovePoint, no path) to its own slot on a ring around Malygos, or
    // around a human master's drake when Malygos is not in sight. Works with a SelfBot leader or no client.
    constexpr float BOSS_RING_RADIUS = 35.0f;      // drake spells reach 60y
    constexpr float MASTER_RING_RADIUS = 8.0f;
    constexpr float SLOT_TOLERANCE = 6.0f;
    constexpr float RESTEER_DISTANCE = 5.0f;

    Unit* anchor = AI_VALUE2(Unit*, "find target", "malygos");
    float radius = BOSS_RING_RADIUS;
    if (!anchor)
    {
        Player* human = RaidBotDuty::GetHumanMaster(botAI);
        anchor = human ? human->GetVehicleBase() : nullptr;
        radius = MASTER_RING_RADIUS;
    }
    if (!anchor || anchor == vehicleBase) { return false; }

    uint8 const numPlayers = bot->GetRaidDifficulty() == RAID_DIFFICULTY_25MAN_NORMAL ? 25 : 10;
    int32 const slot = std::max<int32>(0, botAI->GetGroupSlotIndex(bot));
    float const angle = slot * 2.0f * static_cast<float>(M_PI) / numPlayers;
    Position destination(anchor->GetPositionX() + std::cos(angle) * radius,
                         anchor->GetPositionY() + std::sin(angle) * radius, anchor->GetPositionZ());

    MotionMaster* mm = vehicleBase->GetMotionMaster();
    if (vehicleBase->GetExactDist(&destination) <= SLOT_TOLERANCE)
    {
        if (anchor->IsHostileTo(bot))
            vehicleBase->SetFacingToObject(anchor);
        return false;
    }

    if (_hasDestination && mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
        _lastDestination.GetExactDist(&destination) < RESTEER_DISTANCE)
        return false;

    RaidBotDuty::LogWorkaroundOnce(bot, "eoe-skytalon-follow",
                                   "AC #25184 Skytalon cannot follow: holding a ring slot with MovePoint");

    vehicleBase->SetCanFly(true);  // a ridden Skytalon flies freely for a player too (client flight state)
    mm->Clear(false);
    mm->MovePoint(0, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(),
                  FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
    vehicleBase->SendMovementFlagUpdate();
    _lastDestination = destination;
    _hasDestination = true;
    return true;
}

bool EoEDrakeAttackAction::isPossible()
{
    Unit* vehicleBase = bot->GetVehicleBase();
    return (vehicleBase && vehicleBase->GetEntry() == NPC_WYRMREST_SKYTALON);
}

bool EoEDrakeAttackAction::Execute(Event /*event*/)
{
    vehicleBase = bot->GetVehicleBase();
    if (!vehicleBase)
    {
        return false;
    }

    // Unit* target = AI_VALUE(Unit*, "current target");
    Unit* boss = AI_VALUE2(Unit*, "find target", "malygos");
    // if (!boss) { return false; }

    if (!boss)
    {
        GuidVector npcs = AI_VALUE(GuidVector, "possible targets");
        for (auto& npc : npcs)
        {
            Unit* unit = botAI->GetUnit(npc);
            if (!unit || unit->GetEntry() != NPC_MALYGOS)
            {
                continue;
            }

            boss = unit;
            break;
        }
    }
    // Check this again to see if a target was assigned
    if (!boss)
    {
        return false;
    }

    uint8 numHealers;
    bot->GetRaidDifficulty() == RAID_DIFFICULTY_25MAN_NORMAL ? numHealers = 10 : numHealers = 4;

    Group* group = bot->GetGroup();
    if (!group)
        return false;
    std::vector<std::pair<ObjectGuid, Player*>> sortedMembers;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member)
            continue;
        sortedMembers.push_back(std::make_pair(member->GetGUID(), member));
    }
    std::sort(sortedMembers.begin(), sortedMembers.end());

    int botIndex = -1;
    for (size_t i = 0; i < sortedMembers.size(); ++i)
    {
        if (sortedMembers[i].first == bot->GetGUID())
        {
            botIndex = i;
            break;
        }
    }

    if (botIndex == -1)
        return false;

    if (botIndex > numHealers)
    {
        return DrakeDpsAction(boss);
    }
    else
    {
        return DrakeHealAction();
    }

    return false;
}

bool EoEDrakeAttackAction::CastDrakeSpellAction(Unit* target, uint32 spellId, uint32 cooldown)
{
    if (botAI->CanCastVehicleSpell(spellId, target))
        if (botAI->CastVehicleSpell(spellId, target))
        {
            vehicleBase->AddSpellCooldown(spellId, 0, cooldown);
            return true;
        }
    return false;
}

bool EoEDrakeAttackAction::DrakeDpsAction(Unit* target)
{
    Unit* vehicleBase = bot->GetVehicleBase();
    if (!vehicleBase) { return false; }

    uint8 comboPoints = vehicleBase->GetComboPoints(target);
    if (comboPoints >= 2)
    {
        return CastDrakeSpellAction(target, SPELL_ENGULF_IN_FLAMES, 0);
    }
    else
    {
        return CastDrakeSpellAction(target, SPELL_FLAME_SPIKE, 0);
    }
}

bool EoEDrakeAttackAction::DrakeHealAction()
{
    Unit* vehicleBase = bot->GetVehicleBase();
    if (!vehicleBase)
    {
        return false;
    }

    uint8 comboPoints = vehicleBase->GetComboPoints(vehicleBase);
    if (comboPoints >= 5)
    {
        return CastDrakeSpellAction(vehicleBase, SPELL_LIFE_BURST, 0);
    }
    else
    {
        // "Revivify" may be bugged server-side:
        // "botAI->CanCastVehicleSpell()" returns SPELL_FAILED_BAD_TARGETS when targeting drakes.
        // Forcing the cast attempt seems to succeed, not sure what's going on here.
        // return CastDrakeSpellAction(target, SPELL_REVIVIFY, 0);
        return botAI->CastVehicleSpell(SPELL_REVIVIFY, vehicleBase);
    }
}
