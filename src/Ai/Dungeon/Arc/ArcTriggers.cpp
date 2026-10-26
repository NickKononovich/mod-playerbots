/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArcTriggers.h"
#include "ArcShared.h"
#include "Playerbots.h"
#include "InstanceScript.h"
#include "Spell.h"
#include "TbcHazard.h"

using namespace Arcatraz;

namespace Arcatraz
{
bool IsWardenEventActive(Player* bot)
{
    InstanceScript* instance = bot->GetInstanceScript();
    return bot->GetMapId() == MAP_ARCATRAZ && instance &&
           instance->GetBossState(BOSS_WARDEN_MELLICHAR) == IN_PROGRESS;
}

Unit* FindReleasedPrisoner(PlayerbotAI* botAI)
{
    return TbcHazard::FindAttackerByEntry(botAI, { NPC_BLAZING_TRICKSTER, NPC_PHASE_HUNTER, NPC_AKKIRIS, NPC_SULFURON,
                                                   NPC_TWILIGHT_DRAKONAAR, NPC_BLACKWING_DRAKONAAR });
}

bool FindMeteorPoint(PlayerbotAI* botAI, Position& point)
{
    Player* bot = botAI->GetBot();
    Unit* abyssal = TbcHazard::FindAttackerByEntry(botAI, { NPC_GARGANTUAN_ABYSSAL });
    if (!abyssal)
        return false;

    Spell* spell = abyssal->FindCurrentSpellBySpellId(SPELL_ABYSSAL_METEOR);
    if (!spell)
        return false;

    if (spell->m_targets.HasDst() && spell->m_targets.GetDstPos())
        point = spell->m_targets.GetDstPos()->GetPosition();
    else if (Unit* target = ObjectAccessor::GetUnit(*abyssal, spell->m_targets.GetUnitTargetGUID());
             target && target->IsInWorld() && target->GetMap() == abyssal->GetMap())
        point = target->GetPosition();
    else
        return false;

    return bot->GetExactDist2d(point) < METEOR_JOIN_RANGE;
}
}  // namespace Arcatraz

bool ArcWardenRoomTrigger::IsActive()
{
    return IsWardenEventActive(bot) && bot->GetExactDist2d(WARDEN_ROOM_X, WARDEN_ROOM_Y) > WARDEN_ROOM_LEASH;
}

bool ArcPrisonerTrigger::IsActive()
{
    if (PlayerbotAI::IsHeal(bot) || !IsWardenEventActive(bot))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    Unit* prisoner = FindReleasedPrisoner(botAI);
    return prisoner && prisoner != target;
}

bool AbyssalMeteorTrigger::IsActive()
{
    Position point;
    return FindMeteorPoint(botAI, point) && bot->GetExactDist2d(point) > METEOR_STACK_DISTANCE;
}

bool DalliahWhirlwindTrigger::IsActive()
{
    Unit* dalliah = TbcHazard::FindBoss(botAI, DALLIAH_NAME);
    if (!dalliah || !dalliah->HasAura(SPELL_DALLIAH_WHIRLWIND))
        return false;

    return TbcHazard::IsInside(bot, dalliah->GetPosition(), WhirlwindDangerRadius(bot, dalliah));
}
