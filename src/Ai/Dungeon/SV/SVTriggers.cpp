/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SVTriggers.h"
#include "AiObjectContext.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SVShared.h"
#include "TbcHazard.h"

using namespace Steamvault;

bool KalithreshDistillerTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;

    return TbcHazard::FindBoss(botAI, KALITHRESH_NAME) != nullptr;
}

bool KalithreshSpellReflectionTrigger::IsActive()
{
    Unit* kalithresh = TbcHazard::FindBoss(botAI, KALITHRESH_NAME);
    if (!kalithresh || !kalithresh->HasAura(SPELL_KALITHRESH_REFLECTION))
        return false;

    Spell* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell)
        spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

    return spell && spell->m_targets.GetUnitTarget() == kalithresh;
}

Unit* Steamvault::LooseBogOverlord(PlayerbotAI* botAI)
{
    // The Overlord's own reach and the tank's taunts (30 yd).
    constexpr float looseRange = 30.0f;

    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != MAP_STEAMVAULT || !bot->IsInCombat())
        return nullptr;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != NPC_BOG_OVERLORD)
            continue;

        Unit* overlord = botAI->GetUnit(guid);
        if (!overlord || !overlord->IsAlive() || bot->GetExactDist(overlord) > looseRange)
            continue;

        Player* victim = overlord->GetVictim() ? overlord->GetVictim()->ToPlayer() : nullptr;
        if (victim && !PlayerbotAI::IsTank(victim))
            return overlord;
    }
    return nullptr;
}

bool SvBogOverlordLooseTrigger::IsActive()
{
    return PlayerbotAI::IsTank(bot) && Steamvault::LooseBogOverlord(botAI) != nullptr;
}
