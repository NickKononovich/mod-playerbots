/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InterruptActions.h"
#include "Event.h"
#include "InterruptValues.h"
#include "LeaderCommandState.h"  // [rot2] [rot3]
#include "Log.h"
#include "RtiValue.h"        // [rot2] FindCrowdControl
#include "TargetValue.h"     // [rot2] IsCrowdControlled
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

Unit* InterruptAssignedCastAction::GetCaster()
{
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    Unit* caster = botAI->GetUnit(assignment.caster);
    if (!caster || !caster->IsAlive() || !caster->IsInWorld() || caster->GetMap() != bot->GetMap())
        return nullptr;

    return caster;
}

SpellInfo const* InterruptAssignedCastAction::GetAssignedSpell()
{
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    if (assignment.spell.empty())
        return nullptr;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", assignment.spell);
    return spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
}

bool InterruptAssignedCastAction::isUseful()
{
    Unit* caster = GetCaster();
    if (!caster || !caster->IsNonMeleeSpellCast(false, false, true))
        return false;

    // the cached assignment may lag a hand-off to the next member by a tick: the registry has the last word
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    return InterruptCoordination::Allows(botAI, caster, assignment.spell);
}

bool InterruptAssignedCastAction::isPossible()
{
    Unit* caster = GetCaster();
    SpellInfo const* spellInfo = GetAssignedSpell();
    if (!caster || !spellInfo || bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || bot->HasSpellCooldown(spellInfo->Id))
        return false;

    // An own channel is stopped in Execute; CanCastSpell would refuse while it runs.
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return true;

    Unit* target = spellInfo->NeedsExplicitUnitTarget() ? caster : bot;
    return botAI->CanCastSpell(spellInfo->Id, target, true);
}

bool InterruptAssignedCastAction::Execute(Event /*event*/)
{
    Unit* caster = GetCaster();
    SpellInfo const* spellInfo = GetAssignedSpell();
    if (!caster || !spellInfo)
        return false;

    // Stop-cast to kick, as a player would; a healer is never assigned while it casts.
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) && !PlayerbotAI::IsHeal(bot))
        bot->InterruptSpell(CURRENT_CHANNELED_SPELL);

    if (Spell* own = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        bot->InterruptSpell(own->GetCurrentContainer());

    Unit* target = spellInfo->NeedsExplicitUnitTarget() ? caster : bot;
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    if (!botAI->CastSpell(assignment.spell, target))
        return false;

    InterruptCoordination::MarkFired(botAI, caster);
    // the assignment is spent; recompute it at once instead of re-firing on a cached answer
    if (UntypedValue* value = context->GetUntypedValue("interrupt assignment"))
        value->Reset();
    return true;
}

namespace
{
    // [rot2] a unit of the bot's map, alive
    Unit* LeaderCommandUnit(PlayerbotAI* botAI, ObjectGuid guid)
    {
        if (guid.IsEmpty())
            return nullptr;

        Player* bot = botAI->GetBot();
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsInWorld() || unit->GetMap() != bot->GetMap())
            return nullptr;

        return unit;
    }

    void AckLeader(Player* bot, std::string const& what, std::string const& why)
    {
        Leader::MemberEvents command;
        if (LeaderCommand::CurrentCommand(bot, command))
            Leader::Events::Ack(bot->GetGUID(), command.seq, what, why);
    }
}

bool LeaderExternalAction::isUseful()
{
    uint32 spell = 0;
    ObjectGuid target;
    return LeaderCommand::AssignedExternal(bot, spell, target) && spell && bot->HasSpell(spell) &&
           (target.IsEmpty() || LeaderCommandUnit(botAI, target));
}

bool LeaderExternalAction::isPossible()
{
    uint32 spell = 0;
    ObjectGuid target;
    if (!LeaderCommand::AssignedExternal(bot, spell, target) || bot->HasSpellCooldown(spell))
        return false;

    Unit* unit = target.IsEmpty() ? bot : LeaderCommandUnit(botAI, target);
    return unit && botAI->CanCastSpell(spell, unit, true);
}

bool LeaderExternalAction::Execute(Event /*event*/)
{
    uint32 spell = 0;
    ObjectGuid target;
    if (!LeaderCommand::AssignedExternal(bot, spell, target))
        return false;

    Unit* unit = target.IsEmpty() ? bot : LeaderCommandUnit(botAI, target);
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!unit || !info || !botAI->CastSpell(spell, unit))
        return false;

    LOG_INFO("playerbots", "[rot2] [leader external] {} casts {} ({}) on {}", bot->GetName(), info->SpellName[0], spell,
             unit->GetName());
    AckLeader(bot, "external", std::string(info->SpellName[0]) + " on " + unit->GetName());
    return true;
}

bool LeaderThreatAction::isUseful()
{
    uint32 spell = 0;
    ObjectGuid mob;
    return LeaderCommand::AssignedThreat(bot, spell, mob) && LeaderCommandUnit(botAI, mob);
}

bool LeaderThreatAction::Execute(Event /*event*/)
{
    uint32 spell = 0;
    ObjectGuid mob;
    if (!LeaderCommand::AssignedThreat(bot, spell, mob))
        return false;

    Unit* unit = LeaderCommandUnit(botAI, mob);
    if (!unit)
        return false;

    // the rotation goes on against the mob taken back
    context->GetValue<Unit*>("current target")->Set(unit);

    SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
    bool cast = false;
    if (info && bot->HasSpell(spell) && !bot->HasSpellCooldown(spell))
    {
        Unit* spellTarget = info->NeedsExplicitUnitTarget() ? unit : bot;
        if (botAI->CanCastSpell(spell, spellTarget, true))
            cast = botAI->CastSpell(spell, spellTarget);
    }

    LOG_INFO("playerbots", "[rot2] [leader threat] {} takes back {} {}: {}", bot->GetName(), unit->GetName(),
             unit->GetEntry(), cast ? info->SpellName[0] : "attacks it");
    AckLeader(bot, "threat", std::string(cast ? info->SpellName[0] : "attack") + " on " + unit->GetName());
    return true;
}

bool LeaderCcAction::isUseful()
{
    Unit* unit = LeaderCommandUnit(botAI, LeaderCommand::AssignedCc(bot));
    return unit && !IsCrowdControlled(unit);
}

bool LeaderCcAction::isPossible()
{
    Unit* unit = LeaderCommandUnit(botAI, LeaderCommand::AssignedCc(bot));
    CrowdControlOption const* option = unit ? FindCrowdControl(botAI, unit, false) : nullptr;
    return option && option->spell && botAI->CanCastSpell(option->spell, unit);
}

bool LeaderCcAction::Execute(Event /*event*/)
{
    Unit* unit = LeaderCommandUnit(botAI, LeaderCommand::AssignedCc(bot));
    CrowdControlOption const* option = unit ? FindCrowdControl(botAI, unit, false) : nullptr;
    if (!option || !option->spell || !botAI->CastSpell(option->spell, unit))
        return false;

    LOG_INFO("playerbots", "[rot2] [leader cc] {} casts {} on {} {}", bot->GetName(), option->spell, unit->GetName(),
             unit->GetEntry());
    AckLeader(bot, "cc", std::string(option->spell) + " on " + unit->GetName());
    return true;
}

// [rot3] Best first: the full immunities and the big cuts, then the smaller ones. A tank skips the saves that stop it
// holding threat (Divine Shield halves its damage, Ice Block stops everything).
char const* LeaderPreSaveAction::PickSave()
{
    static char const* const saves[] = {"divine shield",       "ice block",         "shield wall",
                                        "survival instincts",  "icebound fortitude", "dispersion",
                                        "pain suppression",    "divine protection", "cloak of shadows",
                                        "evasion",             "deterrence",        "last stand",
                                        "vampiric blood",      "barkskin",          "anti-magic shell",
                                        "enraged regeneration", "frenzied regeneration"};

    bool const tank = PlayerbotAI::IsTank(bot);
    for (char const* save : saves)
    {
        std::string const name(save);
        if (tank && (name == "divine shield" || name == "ice block"))
            continue;

        uint32 const spellId = AI_VALUE2(uint32, "spell id", name);
        if (!spellId || !bot->HasSpell(spellId) || bot->HasSpellCooldown(spellId))
            continue;

        if (botAI->CanCastSpell(name, bot))
            return save;
    }

    return nullptr;
}

bool LeaderPreSaveAction::isUseful() { return LeaderCommandState::PreSaveAssigned(bot) && PickSave(); }

bool LeaderPreSaveAction::Execute(Event /*event*/)
{
    char const* save = PickSave();
    if (!save || !botAI->CastSpell(save, bot))
        return false;

    LOG_INFO("playerbots", "[rot3] [leader pre save] {} casts {} on the leader's PreSave", bot->GetName(), save);
    AckLeader(bot, "pre save", save);
    return true;
}
