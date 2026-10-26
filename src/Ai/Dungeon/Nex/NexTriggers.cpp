/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NexTriggers.h"
#include "AiObject.h"
#include "AiObjectContext.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "Playerbots.h"

bool FactionCommanderWhirlwindTrigger::IsActive()
{
    Unit* boss = nullptr;
    uint8 faction = bot->GetTeamId();

    switch (bot->GetMap()->GetDifficulty())
    {
        case DUNGEON_DIFFICULTY_NORMAL:
            if (faction == TEAM_ALLIANCE)
            {
                boss = AI_VALUE2(Unit*, "find target", "horde commander");
            }
            else //if (faction == TEAM_HORDE)
            {
                boss = AI_VALUE2(Unit*, "find target", "alliance commander");
            }
            break;
        case DUNGEON_DIFFICULTY_HEROIC:
            if (faction == TEAM_ALLIANCE)
            {
                boss = AI_VALUE2(Unit*, "find target", "commander kolurg");
            }
            else //if (faction == TEAM_HORDE)
            {
                boss = AI_VALUE2(Unit*, "find target", "commander stoutbeard");
            }
            break;
        default:
            break;
    }

    if (boss && boss->HasUnitState(UNIT_STATE_CASTING))
    {
        if (boss->FindCurrentSpellBySpellId(SPELL_WHIRLWIND))
        {
            return true;
        }
    }
    return false;
}

bool TelestraFirebombTrigger::IsActive()
{
    if (PlayerbotAI::IsMelee(bot)) { return false; }

    Unit* boss = AI_VALUE2(Unit*, "find target", "grand magus telestra");
    // Avoid split phase with the fake Telestra units, only match the true boss id
    return boss && boss->GetEntry() == NPC_TELESTRA;
}

bool TelestraSplitPhaseTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "grand magus telestra");
    // Only match split phase with the fake Telestra units
    return boss && boss->GetEntry() != NPC_TELESTRA;
}

Unit* NexIds::FindChaoticRift(PlayerbotAI* botAI)
{
    // Rifts never enter the threat table, so they are not findable through "find target".
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && unit->GetEntry() == NPC_CHAOTIC_RIFT)
            return unit;
    }
    return nullptr;
}

bool ChaoticRiftTrigger::IsActive()
{
    // Kill every rift as it opens (wiki tactic): left alone they stack wraith spawns, worst on heroic (15 s).
    Unit* boss = AI_VALUE2(Unit*, "find target", "anomalus");
    return boss && (boss->HasAura(BUFF_RIFT_SHIELD) || NexIds::FindChaoticRift(botAI));
}

bool OrmorokSpikesTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ormorok the tree-shaper");
    if (!boss || !PlayerbotAI::IsTank(bot)) { return false; }

    GuidVector objects = AI_VALUE(GuidVector, "closest game objects");
    for (auto i = objects.begin(); i != objects.end(); ++i)
    {
        GameObject* go = botAI->GetGameObject(*i);
        if (go && go->GetEntry() == GO_CRYSTAL_SPIKE)
        {
            return true;
        }
    }
    return false;
}

bool OrmorokStackTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ormorok the tree-shaper");
    return (boss && !PlayerbotAI::IsTank(bot));
}

bool IntenseColdTrigger::IsActive()
{
    // Adjust as needed - too much interrupting loses dps time,
    // but too many stacks is deadly. Assuming 3-5 is a good number to clear
    int stackThreshold = 5;
    Unit* boss = AI_VALUE2(Unit*, "find target", "keristrasza");
    return boss && botAI->GetAura("intense cold", bot, false, false, stackThreshold);
}

bool KeristraszaPositioningTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "keristrasza");
    // Include healers here for now, otherwise they stand in things
    return boss && !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsRangedDps(bot);
    // return boss && PlayerbotAI::IsMelee(bot) && !PlayerbotAI::IsTank(bot);
}

NexIds::ContainmentSphere const* NexIds::FindReadySphere(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(DATA_KERISTRASZA_EVENT) == DONE)
        return nullptr;

    ContainmentSphere const* nearest = nullptr;
    float nearestDistSq = 0.0f;
    for (auto const& sphere : CONTAINMENT_SPHERES)
    {
        if (instance->GetBossState(sphere.bossData) != DONE || instance->GetBossState(sphere.orbData) == DONE)
            continue;

        float const distSq = bot->GetExactDistSq(sphere.x, sphere.y, sphere.z);
        if (!nearest || distSq < nearestDistSq)
        {
            nearest = &sphere;
            nearestDistSq = distSq;
        }
    }
    return nearest;
}

bool NexusContainmentSphereTrigger::IsActive()
{
    if (bot->IsInCombat() || !NexIds::FindReadySphere(bot))
        return false;

    return DungeonInteract::IsInteractor(botAI);
}
