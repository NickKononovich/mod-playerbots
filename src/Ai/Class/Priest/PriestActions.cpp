/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PriestActions.h"
#include "Event.h"
#include "Playerbots.h"

bool CastRemoveShadowformAction::Execute(Event /*event*/)
{
    botAI->RemoveAura("shadowform");
    return true;
}

// [b31_deep]
bool CastShadowWordDeathAction::isUseful()
{
    return AI_VALUE2(uint8, "health", "self target") > sPlayerbotAIConfig.LowHealth && CastSpellAction::isUseful();
}

// [b31_deep]
Unit* CastPowerWordShieldOnPartyAction::GetTarget()
{
    Unit* triaged = HealPartyMemberAction::GetTarget();
    if (!triaged || !botAI->HasAnyAuraOf(triaged, "weakened soul", "power word: shield", nullptr))
        return triaged;

    // a member in the low / critical / emergency bands keeps the GCD: the node's next heal goes to him
    Group* group = bot->GetGroup();
    if (!group || triaged->GetHealthPct() < sPlayerbotAIConfig.LowHealth)
        return triaged;

    Player* best = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == triaged || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            member->GetHealthPct() >= sPlayerbotAIConfig.MediumHealth ||
            member->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance ||
            botAI->HasAnyAuraOf(member, "weakened soul", "power word: shield", nullptr))
            continue;

        if (!best || member->GetHealthPct() < best->GetHealthPct())
            best = member;
    }

    return best ? best : triaged;
}

bool CastShadowWordPainAction::isUseful()
{
    if (!ai::priest::ShadowWeavingReady(bot))
        return false;

    beforeDuration = ai::priest::ShadowWordPainRefreshMs(bot);
    return CastPriestDotAction::isUseful();
}

bool CastVampiricTouchAction::isUseful()
{
    beforeDuration = ai::priest::VampiricTouchRefreshMs(bot, AI_VALUE2(uint32, "spell id", spell));
    return CastPriestDotAction::isUseful();
}

bool CastFadeAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return bot->GetGroup() && !(target && target->IsPlayer());  // players have no threat
}

bool CastPowerWordShieldOnMainTankAction::isUseful()
{
    return BuffOnMainTankAction::isUseful() && !botAI->HasAura("weakened soul", GetTarget());
}

bool CastRemoveShadowformAction::isUseful() { return botAI->HasAura("shadowform", AI_VALUE(Unit*, "self target")); }

Unit* CastPowerWordShieldOnAlmostFullHealthBelowAction::GetTarget()
{
    Group* group = bot->GetGroup();
    if (!group)  // [b31_deep]
        return nullptr;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead())
        {
            continue;
        }
        if (player->GetHealthPct() > sPlayerbotAIConfig.AlmostFullHealth)
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        return player;
    }
    return nullptr;
}

bool CastPowerWordShieldOnAlmostFullHealthBelowAction::isUseful()
{
    Group* group = bot->GetGroup();
    if (!group)  // [b31_deep]
        return false;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead())
        {
            continue;
        }
        if (player->GetHealthPct() > sPlayerbotAIConfig.AlmostFullHealth)
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        return true;
    }
    return false;
}

Unit* CastPowerWordShieldOnNotFullAction::GetTarget()
{
    Group* group = bot->GetGroup();
    if (!group)  // [b31_deep]
        return nullptr;

    MinValueCalculator calc(100);
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead() || player->IsFullHealth())
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        calc.probe(player->GetHealthPct(), player);
    }
    return (Unit*)calc.param;
}

bool CastPowerWordShieldOnNotFullAction::isUseful()
{
    return GetTarget();
}

bool CastPowerWordShieldOnValueTargetAction::isUseful()
{
    Unit* target = GetTarget();
    return target && CastHealOnValueTargetAction::isUseful() && !botAI->HasAura("weakened soul", target);
}
