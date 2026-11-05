/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InterruptTriggers.h"
#include "InterruptValues.h"
#include "LeaderCommandState.h"  // [rot2] [rot3]
#include "TargetValue.h"    // [rot2] IsCrowdControlled
#include "Playerbots.h"

bool AssignedInterruptTrigger::IsActive()
{
    return !AI_VALUE(InterruptAssignment, "interrupt assignment").caster.IsEmpty();
}

bool LeaderExternalTrigger::IsActive()
{
    uint32 spell = 0;
    ObjectGuid target;
    return LeaderCommand::AssignedExternal(bot, spell, target) && spell && bot->HasSpell(spell) &&
           !bot->HasSpellCooldown(spell);
}

bool LeaderSelfDefenseTrigger::IsActive()
{
    Leader::MemberEvents command;
    return LeaderCommand::CurrentCommand(bot, command) && command.externalSpell && command.externalSelfDefense &&
           bot->HasSpell(command.externalSpell) && !bot->HasSpellCooldown(command.externalSpell);
}

bool LeaderThreatTrigger::IsActive()
{
    uint32 spell = 0;
    ObjectGuid mob;
    if (!LeaderCommand::AssignedThreat(bot, spell, mob))
        return false;

    Unit* unit = botAI->GetUnit(mob);
    return unit && unit->IsAlive() && unit->GetMap() == bot->GetMap();
}

bool LeaderCcTrigger::IsActive()
{
    ObjectGuid const mob = LeaderCommand::AssignedCc(bot);
    if (mob.IsEmpty())
        return false;

    Unit* unit = botAI->GetUnit(mob);
    return unit && unit->IsAlive() && unit->GetMap() == bot->GetMap() && !IsCrowdControlled(unit);
}

bool LeaderPreSaveTrigger::IsActive() { return LeaderCommandState::PreSaveAssigned(bot); }

bool LeaderDispelTrigger::IsActive()
{
    uint32 spell = 0;
    ObjectGuid target;
    return LeaderCommand::AssignedDispel(bot, spell, target) && bot->HasSpell(spell) && !bot->HasSpellCooldown(spell);
}

bool LeaderConsumeTrigger::IsActive()
{
    uint32 item = 0;
    ObjectGuid target;
    ObjectGuid object;
    return LeaderCommand::AssignedConsume(bot, item, target, object, nullptr) && item;
}

bool LeaderUseObjectTrigger::IsActive()
{
    uint32 item = 0;
    ObjectGuid target;
    ObjectGuid object;
    return LeaderCommand::AssignedConsume(bot, item, target, object, nullptr) && !item && !object.IsEmpty();
}
