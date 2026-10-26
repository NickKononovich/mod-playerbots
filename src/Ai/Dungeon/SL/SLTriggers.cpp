/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SLTriggers.h"
#include "CharmInfo.h"
#include "Group.h"
#include "DungeonStrategyUtils.h"
#include "Playerbots.h"
#include "SLShared.h"
#include "SpellAuras.h"
#include "TbcHazard.h"

using namespace ShadowLabyrinth;

bool MurmurSonicBoomTrigger::IsActive()
{
    Unit* murmur = TbcHazard::FindBoss(botAI, MURMUR_NAME);
    if (!IsCastingSonicBoom(murmur))
        return false;

    return TbcHazard::IsInside(bot, murmur->GetPosition(), SonicBoomDangerRadius(bot));
}

bool MurmurThunderingStormRangeTrigger::IsActive()
{
    if (bot->GetMap()->GetDifficulty() != DUNGEON_DIFFICULTY_HEROIC)
        return false;

    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsMelee(bot))
        return false;

    Unit* murmur = TbcHazard::FindBoss(botAI, MURMUR_NAME);
    if (!murmur || IsCastingSonicBoom(murmur) || bot->HasAura(SPELL_MURMURS_TOUCH))
        return false;

    constexpr float slack = 1.0f;
    return bot->GetExactDist2d(murmur) > THUNDERING_STORM_MIN_RANGE - slack;
}

bool MurmursTouchTrigger::IsActive()
{
    Aura* touch = bot->GetAura(SPELL_MURMURS_TOUCH);
    if (!touch || touch->GetDuration() > TOUCH_SPREAD_MS)
        return false;

    return TbcHazard::FindBoss(botAI, MURMUR_NAME) != nullptr;
}

bool HellmawFearTrigger::IsActive()
{
    if (bot->getClass() != CLASS_SHAMAN)
        return false;

    if (!TbcHazard::FindBoss(botAI, HELLMAW_NAME))
        return false;

    return !AI_VALUE2(bool, "has totem", "tremor totem");
}

namespace
{
bool BlackheartFighting(PlayerbotAI* botAI)
{
    return TbcHazard::FindAttackerByEntry(botAI, {NPC_BLACKHEART}) != nullptr;
}

bool HasPet(Player* bot, bool passive)
{
    for (Unit* controlled : bot->m_Controlled)
    {
        Creature* pet = controlled ? controlled->ToCreature() : nullptr;
        if (pet && pet->IsAlive() && !pet->IsTotem() && pet->GetCharmInfo() &&
            pet->HasReactState(REACT_PASSIVE) == passive)
            return true;
    }
    return false;
}
}  // namespace

bool BlackheartPetsTrigger::IsActive()
{
    if (bot->m_Controlled.empty())
        return false;

    uint32 const now = getMSTime();
    if (BlackheartFighting(botAI))
    {
        _releaseUntilMs = now + BLACKHEART_PET_RELEASE_MS;
        return HasPet(bot, false);
    }

    // After the fight, until the parked pets have their stance back (a pull may keep its own pet passive).
    if (!_releaseUntilMs || now > _releaseUntilMs)
    {
        _releaseUntilMs = 0;
        return false;
    }
    return HasPet(bot, true);
}

bool BlackheartSpreadTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsMelee(bot) || !BlackheartFighting(botAI))
        return false;

    float const spread = BlackheartSpread(bot);
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && member->GetMapId() == bot->GetMapId() &&
            bot->GetExactDist2d(member) < spread)
            return true;
    }
    return false;
}
