/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DTKActions.h"
#include "Pet.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "TbcHazard.h"

#include <string>

bool CorpseExplodeSpreadAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "trollgore");
    if (!boss) { return false; }

    float distance = 5.0f;
    float distanceExtra = 2.0f;
    GuidVector corpses = AI_VALUE(GuidVector, "nearest corpses");
    for (auto i = corpses.begin(); i != corpses.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && DTKIds::IsTrollgoreInvader(unit->GetEntry()))
        {
            float currentDistance = bot->GetExactDist2d(unit);
            if (currentDistance < distance + distanceExtra)
                return MoveAway(unit, distance + distanceExtra - currentDistance);
        }
    }
    return false;
}

bool TrollgoreHoldRoomAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "trollgore");
    if (!boss)
        return false;

    // The tank brings him back to his home spot; the others close in around it (melee keep hitting from there).
    float const spread = boss->GetVictim() == bot ? 2.0f : 8.0f;
    return MoveNear(bot->GetMapId(), DTKIds::TROLLGORE_HOLD_X, DTKIds::TROLLGORE_HOLD_Y, DTKIds::TROLLGORE_HOLD_Z,
                    spread, MovementPriority::MOVEMENT_COMBAT);
}

bool AvoidArcaneFieldAction::Execute(Event /*event*/)
{
    // The field slows by 50 %: leave on the shortest free line and stop any cast (heals outranked it before).
    Unit* center = DTKShared::ArcaneFieldCenter(botAI);
    if (!center)
        return false;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float const exitRadius = DTKShared::ARCANE_FIELD_RADIUS + bot->GetCombatReach() + TbcHazard::EXIT_MARGIN;
    if (!TbcHazard::FindExitPoint(bot, center->GetPosition(), exitRadius, x, y, z))
        return false;

    bot->CastStop();
    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true);
}

bool NovosDefaultPositionAction::isUseful()
{
    // Distance to tether to centre of room
    float threshold = 15.0f;
    return bot->GetDistance(NOVOS_PARTY_POSITION) > threshold;
}
bool NovosDefaultPositionAction::Execute(Event /*event*/)
{
    float clusterDistance = 4.0f;
    // Only reposition if we're not killing anything
    if (!bot->GetTarget())
    {
        return MoveNear(bot->GetMap()->GetId(),
            NOVOS_PARTY_POSITION.GetPositionX(),
            NOVOS_PARTY_POSITION.GetPositionY(),
            NOVOS_PARTY_POSITION.GetPositionZ(),
            clusterDistance, MovementPriority::MOVEMENT_NORMAL);
    }
    return false;
}

bool NovosDropSummonTargetAction::Execute(Event /*event*/)
{
    // Killing a Novos Summon Target stops its Crystal Handlers, so the shield never drops (softlock).
    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && DTKIds::IsNovosNeverTarget(target->GetEntry()))
        context->GetValue<Unit*>("current target")->Set(nullptr);

    Unit* victim = bot->GetVictim();
    if (victim && DTKIds::IsNovosNeverTarget(victim->GetEntry()))
    {
        bot->AttackStop();
        bot->SetTarget(ObjectGuid::Empty);
        bot->SetSelection(ObjectGuid::Empty);
    }

    if (Pet* pet = bot->GetPet())
        if (Unit* petVictim = pet->GetVictim(); petVictim && DTKIds::IsNovosNeverTarget(petVictim->GetEntry()))
            pet->AttackStop();

    return true;
}

bool NovosTargetPriorityAction::Execute(Event /*event*/)
{
    // TODO: This can be improved, some parts are still buggy.
    // But it works for now and this fight is very easy

    // Designate a dps char to handle the stairs adds.
    // This is probably better as a melee, so just pick the first
    // melee dps in the party. If none exist, pick the first ranged.

    // TODO: Switch to botAI->Index instead, cleaner
    Player* stairsDps = nullptr;
    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Player* groupMember = botAI->GetPlayer(member);
        if (!groupMember) { continue; }

        if (PlayerbotAI::IsDps(groupMember))
        {
            if (PlayerbotAI::IsMelee(groupMember))
            {
                // Found our first melee dps, grab handle and break
                stairsDps = groupMember;
                break;
            }
            else
            {
                // Ranged dps, only set if none already assigned.
                // Don't break, we want to keep searching for a melee instead.
                if (!stairsDps)
                    stairsDps = groupMember;
            }
        }
    }

    Unit* selectedTargets[2] = {nullptr, nullptr};
    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit) { continue; }
        uint32 creatureId = unit->GetEntry();

        // Tank priority:
        // Hulking Corpse -> Crystal Handler
        if (PlayerbotAI::IsTank(bot))
        {
            if (creatureId == NPC_HULKING_CORPSE)
                selectedTargets[0] = unit;

            else if (creatureId == NPC_CRYSTAL_HANDLER)
                selectedTargets[1] = unit;

        }
        // Dedicated stairs dps is assigned.
        // Priority: Risen Shadowcaster -> Fetid Troll Corpse
        else if (stairsDps && bot == stairsDps)
        {
            if (creatureId == NPC_RISEN_SHADOWCASTER)
            {
                if (!selectedTargets[0] || bot->GetDistance(unit) < bot->GetDistance(selectedTargets[0]) - 5.0f)
                    selectedTargets[0] = unit;

            }
            else if (creatureId == NPC_FETID_TROLL_CORPSE)
            {
                if (!selectedTargets[1] || bot->GetDistance(unit) < bot->GetDistance(selectedTargets[1]) - 5.0f)
                    selectedTargets[1] = unit;
            }
        }
        // All other dps priority:
        // Crystal Handler -> Hulking Corpse
        else if (PlayerbotAI::IsDps(bot))
        {
            if (creatureId == NPC_CRYSTAL_HANDLER)
                selectedTargets[0] = unit;

            else if (creatureId == NPC_HULKING_CORPSE)
                selectedTargets[1] = unit;
        }
    }

    for (Unit* primaryTarget : selectedTargets)
    {
        // Attack the first valid target in the priority list
        if (primaryTarget)
        {
            if (AI_VALUE(Unit*, "current target") != primaryTarget)
            {
                // bot->Yell(primaryTarget->GetName(), LANG_UNIVERSAL);
                return Attack(primaryTarget);
            }
            // Don't continue loop here, the target exists so we don't
            // want to move down the prio list. We just don't need to send attack
            // command again, just return false and exit the loop that way
            return false;
        }
    }
    return false;
}

bool KingDredFearGuardAction::Execute(Event /*event*/)
{
    Unit* dred = DTKShared::KingDred(botAI);
    if (!dred)
        return false;

    std::string const health = Acore::StringFormat("king dred at {:.0f} %{}", dred->GetHealthPct(),
        dred->FindCurrentSpellBySpellId(DTKIds::SPELL_BELLOWING_ROAR) ? ", bellowing roar cast" : "");

    switch (bot->getClass())
    {
        case CLASS_SHAMAN:
            if (!botAI->CanCastSpell("tremor totem", bot) || !botAI->CastSpell("tremor totem", bot))
                return false;
            DTKShared::Log(bot, "dtk dred fear guard", "tremor", "tremor totem, " + health, 5 * IN_MILLISECONDS);
            return true;
        case CLASS_PRIEST:
        {
            Player* target = DTKShared::DredFearWardTarget(botAI, dred);
            if (!target || !botAI->CastSpell("fear ward", target))
                return false;
            DTKShared::Log(bot, "dtk dred fear guard", "ward",
                           Acore::StringFormat("fear ward on {}, {}", target->GetName(), health), 5 * IN_MILLISECONDS);
            return true;
        }
        case CLASS_WARRIOR:
            if (!dred->FindCurrentSpellBySpellId(DTKIds::SPELL_BELLOWING_ROAR) ||
                !botAI->CanCastSpell("berserker rage", bot) || !botAI->CastSpell("berserker rage", bot))
                return false;
            DTKShared::Log(bot, "dtk dred fear guard", "rage", "berserker rage, " + health, 5 * IN_MILLISECONDS);
            return true;
        default:
            return false;
    }
}

bool KingDredGrievousBiteHealAction::Execute(Event /*event*/)
{
    Player* target = DTKShared::DredBittenMember(botAI);
    if (!target)
        return false;

    auto const cast = [this, target](char const* spell)
    { return botAI->CanCastSpell(spell, target) && botAI->CastSpell(spell, target); };

    // Direct heals: a HoT ticks against the bite's own ticks and the target never reaches full.
    char const* used = nullptr;
    switch (bot->getClass())
    {
        case CLASS_PRIEST:
            for (char const* spell : {"penance", "flash heal", "greater heal"})
                if (!used && cast(spell))
                    used = spell;
            break;
        case CLASS_DRUID:
            for (char const* spell : {"swiftmend", "nourish", "regrowth", "healing touch"})
                if (!used && cast(spell))
                    used = spell;
            break;
        case CLASS_SHAMAN:
            for (char const* spell : {"riptide", "lesser healing wave", "healing wave"})
                if (!used && cast(spell))
                    used = spell;
            break;
        case CLASS_PALADIN:
            for (char const* spell : {"holy shock", "flash of light", "holy light"})
                if (!used && cast(spell))
                    used = spell;
            break;
        default:
            break;
    }
    if (!used)
        return false;

    DTKShared::Log(bot, "dtk dred bite heal", target->GetGUID().ToString(),
                   Acore::StringFormat("{} on {} ({:.0f} % hp, grievous bite)", used, target->GetName(),
                                       target->GetHealthPct()),
                   3 * IN_MILLISECONDS);
    return true;
}

bool NovosPullAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "novos the summoner");
    if (!boss || !boss->IsAlive() || boss->IsInCombat())
        return false;

    // From range when he is out of reach on his platform: the first hit starts the encounter (his adds come).
    if (bot->IsWithinLOSInMap(boss) && !bot->IsWithinMeleeRange(boss))
    {
        static constexpr char const* openers[] = {"avenger's shield", "hand of reckoning", "exorcism",
                                                  "judgement of wisdom", "judgement of light", "heroic throw",
                                                  "taunt", "death grip", "icy touch", "faerie fire (feral)",
                                                  "growl", "shoot", "throw"};
        for (char const* opener : openers)
            if (botAI->CanCastSpell(opener, boss) && botAI->CastSpell(opener, boss))
                return true;
    }

    if (AI_VALUE(Unit*, "current target") != boss || bot->GetVictim() != boss)
        if (Attack(boss))
            return true;

    if (!bot->IsWithinMeleeRange(boss))
        return MoveNear(boss, 1.0f);
    return false;
}
