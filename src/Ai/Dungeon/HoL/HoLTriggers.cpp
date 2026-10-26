/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoLTriggers.h"
#include "AiObjectContext.h"
#include "Creature.h"
#include "Playerbots.h"

namespace HoLShared
{
// The Brittle Golem too close to the bot while Shattering Stomp is imminent (or being cast), else nullptr.
Unit* BrittleGolemThreat(Player* bot, Unit* volkhan)
{
    if (!volkhan || !volkhan->IsAlive())
        return nullptr;

    uint32 const stomp = DUNGEON_MODE(bot, HoLIds::SPELL_SHATTERING_STOMP_N, HoLIds::SPELL_SHATTERING_STOMP_H);
    float const pct = volkhan->GetHealthPct();
    bool const imminent = pct > HoLIds::STOMP_WINDOW_LOW_PCT && pct <= HoLIds::STOMP_WINDOW_HIGH_PCT;
    if (!imminent && !volkhan->FindCurrentSpellBySpellId(stomp))
        return nullptr;

    Creature* golem = bot->FindNearestCreature(HoLIds::NPC_BRITTLE_GOLEM, HoLIds::SHATTER_SAFE_DISTANCE);
    if (!golem || bot->GetExactDist2d(golem) >= HoLIds::SHATTER_SAFE_DISTANCE)
        return nullptr;

    return golem;
}

// Loken channels Pulsing Shockwave (59414) all fight, so UNIT_STATE_CASTING stays set after his first Nova.
bool IsLightningNova(Player* bot, Unit* loken)
{
    return loken && loken->FindCurrentSpellBySpellId(SPELL_LIGHTNING_NOVA);
}
}  // namespace HoLShared

bool StormforgedLieutenantTrigger::IsActive()
{
    if (!PlayerbotAI::IsDps(bot)) { return false; }

    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");

    for (auto& target : targets)
    {
        Unit* unit = botAI->GetUnit(target);
        if (unit && unit->IsInCombat() &&
            unit->GetEntry() == NPC_STORMFORGED_LIEUTENANT)
        {
            return true;
        }
    }
    return false;
}

bool BjarngrimWhirlwindTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "general bjarngrim");
    if (!boss) { return false; }

    return boss->HasUnitState(UNIT_STATE_CASTING) && boss->FindCurrentSpellBySpellId(SPELL_WHIRLWIND_BJARNGRIM);
}

bool VolkhanTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "volkhan");
    return boss && !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot);
}

bool VolkhanBrittleGolemTrigger::IsActive()
{
    return HoLShared::BrittleGolemThreat(bot, AI_VALUE2(Unit*, "find target", "volkhan")) != nullptr;
}

bool IonarStaticOverloadTrigger::IsActive()
{
    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Unit* unit = botAI->GetUnit(member);
        if (unit && unit->HasAura(SPELL_STATIC_OVERLOAD))
        {
            return true;
        }
    }
    return false;
}

bool IonarBallLightningTrigger::IsActive()
{
    if (PlayerbotAI::IsMelee(bot)) { return false; }

    Unit* boss = AI_VALUE2(Unit*, "find target", "ionar");
    if (!boss) { return false; }

    return boss->HasUnitState(UNIT_STATE_CASTING) && boss->FindCurrentSpellBySpellId(SPELL_BALL_LIGHTNING);
}

bool IonarTankAggroTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot)) { return false; }

    Unit* boss = AI_VALUE2(Unit*, "find target", "ionar");
    if (!boss) { return false; }

    return AI_VALUE2(bool, "has aggro", "current target");
}

bool LokenRangedTrigger::IsActive()
{
    return !PlayerbotAI::IsMelee(bot) && AI_VALUE2(Unit*, "find target", "loken");
}

bool LokenLightningNovaTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "loken");
    if (!boss) { return false; }

    return HoLShared::IsLightningNova(bot, boss);
}
