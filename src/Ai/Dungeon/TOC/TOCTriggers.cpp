/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TOCTriggers.h"
#include "AiObject.h"
#include "AiObjectContext.h"
#include "DungeonHoldFire.h"
#include "DungeonInteract.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "TOCShared.h"
#include "Timer.h"

bool ToCLanceTrigger::IsActive()
{
    if (bot->GetVehicle())
        return false;

    Unit* mount1 = bot->FindNearestCreature(NPC_ARGENT_WARHORSE, 100.0f);
    if (!mount1)
        return false;

    Unit* mount2 = bot->FindNearestCreature(NPC_ARGENT_BATTLEWORG, 100.0f);
    if (!mount2)
        return false;

    if (!bot->HasItemOrGemWithIdEquipped(ITEM_LANCE, 1))
        return true;

    if (bot->HasItemCount(ITEM_LANCE, 1))
        return false;

    // Find the nearest spear
    GameObject* lanceRack = bot->FindNearestGameObject(OBJECT_LANCE_RACK, 100.0f);
    if (!lanceRack)
        return false;

    return true;
}

bool ToCUELanceTrigger::IsActive()
{
    if (bot->GetVehicle())
        return false;

    Unit* mount1 = bot->FindNearestCreature(NPC_ARGENT_WARHORSE, 100.0f);
    if (!mount1 && bot->HasItemOrGemWithIdEquipped(ITEM_LANCE, 1))
        return true;

    Unit* mount2 = bot->FindNearestCreature(NPC_ARGENT_BATTLEWORG, 100.0f);
    if (!mount2 && bot->HasItemOrGemWithIdEquipped(ITEM_LANCE, 1))
        return true;

    return false;
}

bool ToCMountedTrigger::IsActive()
{
    Unit* vehicleBase = bot->GetVehicleBase();
    Vehicle* vehicle = bot->GetVehicle();
    if (!vehicleBase || !vehicle)
        return false;

    uint32 entry = vehicleBase->GetEntry();
    return entry == NPC_ARGENT_BATTLEWORG || entry == NPC_ARGENT_WARHORSE;
}

bool ToCMountNearTrigger::IsActive()
{
    if (bot->GetVehicle())
        return false;

    Unit* mount1 = bot->FindNearestCreature(NPC_ARGENT_WARHORSE, 100.0f);
    if (!mount1)
        return false;

    Unit* mount2 = bot->FindNearestCreature(NPC_ARGENT_BATTLEWORG, 100.0f);
    if (!mount2)
        return false;

    return true;
}

bool ToCEadricTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "eadric the pure");
    if (!boss)
        return false;

    return true;
}

bool ToCStartEventTrigger::IsActive()
{
    InstanceScript* instance = ToCShared::GetInstance(bot);
    if (!instance)
        return false;

    uint32 const progress = instance->GetData(ToCIds::DATA_INSTANCE_PROGRESS);
    Creature* announcer = ToCShared::StartGossipAction(progress) ? ToCShared::GetAnnouncer(bot) : nullptr;
    if (!announcer || !announcer->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) || bot->IsInCombat() ||
        !DungeonInteract::IsInteractor(botAI))
    {
        readySince = 0;
        waitingSince = 0;
        return false;
    }

    return IsGroupReady(announcer, progress);
}

bool ToCStartEventTrigger::IsGroupReady(Creature* announcer, uint32 progress)
{
    constexpr float GATHER_RADIUS = 40.0f;
    constexpr float MIN_MANA_PCT = 60.0f;
    constexpr uint32 MANA_WAIT_MS = 45 * IN_MILLISECONDS;

    bool const joust = progress == ToCIds::PROGRESS_INITIAL;
    bool manaReady = true;
    auto const memberReady = [&](Player* member)
    {
        if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            return false;

        if (!member->IsAlive() || member->IsInCombat())
            return false;

        if (joust)
        {
            // the joust is lost when anyone fights on foot; the script itself only checks the clicker
            Unit* mount = member->GetVehicleBase();
            return mount && ToCShared::IsJoustMount(mount->GetEntry());
        }

        if (!member->IsWithinDistInMap(announcer, GATHER_RADIUS))
            return false;

        if (member->getPowerType() == POWER_MANA && member->GetPowerPct(POWER_MANA) < MIN_MANA_PCT)
            manaReady = false;

        return true;
    };

    Player* waitingFor = nullptr;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref && !waitingFor; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && !memberReady(member))
                waitingFor = member;
        }
    }
    else if (!memberReady(bot))
        waitingFor = bot;

    uint32 const now = getMSTime();
    if (waitingFor)
    {
        constexpr uint32 JOUST_WAIT_LOG_MS = 2 * MINUTE * IN_MILLISECONDS;
        readySince = 0;
        if (!waitingSince)
            waitingSince = now;

        if (joust && getMSTimeDiff(waitingSince, now) >= JOUST_WAIT_LOG_MS)
            RaidBotDuty::LogWorkaroundOnce(bot, "toc-joust-wait",
                                           "ToC: needs honest tactic: the joust start still waits for " +
                                               waitingFor->GetName() + " to sit on an Argent joust mount");
        return false;
    }

    waitingSince = 0;
    if (!readySince)
        readySince = now;

    // a member without water never refills: start anyway after a while
    return joust || manaReady || getMSTimeDiff(readySince, now) >= MANA_WAIT_MS;
}

bool ToCArgentSoldiersTrigger::IsActive()
{
    InstanceScript* instance = ToCShared::GetInstance(bot);
    if (!instance || bot->IsInCombat() ||
        instance->GetData(ToCIds::DATA_INSTANCE_PROGRESS) != ToCIds::PROGRESS_CHAMPIONS_DEAD)
        return false;

    // the stage has not been started while the announcer still offers it
    Creature* announcer = ToCShared::GetAnnouncer(bot);
    if (!announcer || announcer->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return false;

    constexpr float PULL_GATHER_RADIUS = 45.0f;
    if (!ToCShared::IsPuller(botAI) || !DungeonInteract::IsGroupGathered(bot, bot, PULL_GATHER_RADIUS))
        return false;

    return ToCShared::FindIdleSoldier(bot) != nullptr;
}

bool ToCReflectiveShieldTrigger::IsActive()
{
    return DungeonHoldFire::IsDamaging(botAI, ToCShared::GetShieldedPaletress(bot));
}

bool ToCInterruptTrigger::IsActive()
{
    if (!bot->IsInCombat() || !ToCShared::GetInstance(bot) || !DungeonHoldFire::HasInterrupt(botAI))
        return false;

    return ToCShared::FindInterruptTarget(botAI) != nullptr;
}

bool ToCGhoulExplodeTrigger::IsActive()
{
    InstanceScript* instance = ToCShared::GetInstance(bot);
    if (!instance || instance->GetBossState(ToCIds::BOSS_BLACK_KNIGHT) != IN_PROGRESS)
        return false;

    Position center;
    return ToCShared::GetGhoulExplodeCenter(botAI, center);
}

bool ToCPaletressRenewTrigger::IsActive()
{
    InstanceScript* instance = ToCShared::GetInstance(bot);
    if (!instance || !bot->IsInCombat() || PlayerbotAI::IsHeal(bot) ||
        instance->GetBossState(ToCIds::BOSS_ARGENT_CHALLENGE) != IN_PROGRESS ||
        !ToCShared::GetArgentChampion(bot, ToCIds::NPC_PALETRESS_BOSS))
        return false;

    return ToCShared::GetRenewDispel(botAI, ToCShared::FindRenewTarget(botAI)) != nullptr;
}

bool ToCMarkedForDeathTrigger::IsActive()
{
    return ToCShared::GetInstance(bot) && ToCShared::HasMarkedForDeath(bot) &&
           ToCShared::GetMarkedDefensive(botAI) != nullptr;
}

bool ToCMarkedForDeathHealTrigger::IsActive()
{
    InstanceScript* instance = ToCShared::GetInstance(bot);
    if (!instance || !PlayerbotAI::IsHeal(bot) || instance->GetBossState(ToCIds::BOSS_BLACK_KNIGHT) != IN_PROGRESS)
        return false;

    return ToCShared::FindMarkedMemberToHeal(botAI) != nullptr;
}
