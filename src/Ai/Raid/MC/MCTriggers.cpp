/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MCTriggers.h"
#include "LeaderHazardVerdict.h"  // [mcimps] LandedHit
#include "MCHelpers.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <vector>

using namespace MoltenCoreHelpers;

bool McLivingBombDebuffTrigger::IsActive()
{
    // No check for Baron Geddon, because bots may have the bomb even after Geddon died.
    return bot->HasAura(SPELL_LIVING_BOMB);
}

bool McBaronGeddonInfernoTrigger::IsActive()
{
    if (Unit* boss = AI_VALUE2(Unit*, "find target", "baron geddon"))
        return boss->HasAura(SPELL_INFERNO);
    return false;
}

bool McShazzrahRangedTrigger::IsActive()
{
    // Only fire inside Arcane Explosion range. The move-away action no-ops
    // beyond it, so an unconditional trigger makes every already-safe ranged
    // bot attempt a failing move each tick.
    if (!PlayerbotAI::IsRanged(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "shazzrah");
    return boss && bot->GetDistance2d(boss) < ARCANE_EXPLOSION_DISTANCE;
}

bool McInLavaTrigger::IsActive()
{
    LiquidData const& liquid = bot->GetLiquidData();
    return (liquid.Flags & MAP_LIQUID_TYPE_MAGMA) &&
           (liquid.Status & (LIQUID_MAP_WATER_WALK | LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER));
}

bool McGolemaggMagmaSplashTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "golemagg the incinerator");
    if (!boss)
        return false;

    // Below 10% the fight is a burn race (Earthquake pulses, Core Ragers
    // converge): stack management stops mattering, melee return to dps.
    if (boss->GetHealthPct() <= 10.0f)
        return false;

    Aura* splash = bot->GetAura(SPELL_MAGMA_SPLASH);
    if (!splash || splash->GetStackAmount() < MAGMA_SPLASH_BACK_OFF_STACKS)
        return false;

    // Only fire while still inside swing range; once the bot has backed off,
    // the multiplier keeps it from re-engaging until the stack expires.
    return bot->GetDistance2d(boss) < MAGMA_SPLASH_BACK_OFF_DISTANCE;
}

bool McGolemaggMarkBossTrigger::IsActive()
{
    // any tank may mark the boss
    return PlayerbotAI::IsTank(bot) && AI_VALUE2(Unit*, "find target", "golemagg the incinerator");
}

bool McGolemaggIsMainTankTrigger::IsActive()
{
    return PlayerbotAI::IsMainTank(bot) && AI_VALUE2(Unit*, "find target", "golemagg the incinerator");
}

bool McGolemaggIsAssistTankTrigger::IsActive()
{
    return PlayerbotAI::IsAssistTank(bot) && AI_VALUE2(Unit*, "find target", "golemagg the incinerator");
}

bool McGolemaggIsHealerTrigger::IsActive()
{
    return PlayerbotAI::IsHeal(bot) && AI_VALUE2(Unit*, "find target", "golemagg the incinerator");
}

bool McCoreHoundMarkTrigger::IsActive()
{
    return PlayerbotAI::IsMainTank(bot) && AI_VALUE2(Unit*, "find target", "core hound");
}

Unit* McOldestLavaSpawn(PlayerbotAI* botAI)
{
    Unit* oldest = nullptr;
    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != NPC_LAVA_SPAWN || (oldest && oldest->GetGUID() < guid))
            continue;
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive())
            oldest = unit;
    }
    return oldest;
}

bool McLavaSpawnFocusTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;

    Unit* spawn = McOldestLavaSpawn(botAI);
    return spawn && AI_VALUE(Unit*, "current target") != spawn;
}

namespace
{
// [mcimps] the Fire Nova's area round `imp` (its effects' radius at the imp's level)
float FireNovaRadius(SpellInfo const* nova, Unit* imp)
{
    float radius = 0.0f;
    for (SpellEffectInfo const& effect : nova->GetEffects())
        if (effect.IsEffect())
            radius = std::max(radius, effect.CalcRadius(imp));
    return radius;
}

bool IsTankPlayer(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && PlayerbotAI::IsTank(player);
}

bool IsMeleeDps(Player* bot)
{
    return !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot) && PlayerbotAI::IsMelee(bot);
}

// [mcimps] the novas at `at` with the hit known (one imp's hit on the bot is every imp's: same entry and level)
McFireNova FireNovaAt(PlayerbotAI* botAI, Position const& at, float& hit)
{
    McFireNova nova;
    Player* bot = botAI->GetBot();
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_FIRE_NOVA);
    if (!info)
        return nova;

    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != NPC_FLAME_IMP)
            continue;
        Unit* imp = botAI->GetUnit(guid);
        if (!imp || !imp->IsAlive() || imp->GetVictim() == bot)
            continue;
        // the core's area check from a creature caster: the 3D distance of the target's centre to the imp
        // (WorldObjectSpellAreaTargetCheck); the bot's body on top, so it stands out of it whole
        float const reach = FireNovaRadius(info, imp) + bot->GetCombatReach();
        float const distance = imp->GetExactDist(at);
        if (distance > reach)
            continue;
        if (hit < 0.0f)
            hit = Leader::HazardVerdict::LandedHit(imp, info, bot);
        ++nova.imps;
        nova.hits += hit;
        if (!nova.deepest || reach - distance > nova.depth)
        {
            nova.deepest = imp;
            nova.depth = reach - distance;
        }
    }
    return nova;
}
}  // namespace

McFireNova McFireNovaAt(PlayerbotAI* botAI, Position const& at)
{
    float hit = -1.0f;
    return FireNovaAt(botAI, at, hit);
}

bool McFireNovaKeepsOut(PlayerbotAI* botAI, McFireNova const& nova)
{
    Player* bot = botAI->GetBot();
    if (!nova.imps || PlayerbotAI::IsTank(bot))
        return false;
    if (!IsMeleeDps(bot))
        return true;
    return nova.hits >= float(bot->GetHealth());
}

Unit* McFlameImpFocus(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    std::vector<Unit*> imps;
    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != NPC_FLAME_IMP)
            continue;
        Unit* imp = botAI->GetUnit(guid);
        if (imp && imp->IsAlive() && IsTankPlayer(imp->GetVictim()))
            imps.push_back(imp);
    }
    // the least health first, the lower guid among equals: every bot of the raid picks the same imp
    std::sort(imps.begin(), imps.end(),
              [](Unit* a, Unit* b)
              {
                  if (a->GetHealth() != b->GetHealth())
                      return a->GetHealth() < b->GetHealth();
                  return a->GetGUID() < b->GetGUID();
              });
    if (!IsMeleeDps(bot))
        return imps.empty() ? nullptr : imps.front();

    float hit = -1.0f;
    for (Unit* imp : imps)
        if (!McFireNovaKeepsOut(botAI, FireNovaAt(botAI, imp->GetPosition(), hit)))
            return imp;
    return nullptr;
}

bool McFlameImpFireNovaTrigger::IsActive()
{
    if (!bot->IsAlive() || !bot->IsInCombat() || PlayerbotAI::IsTank(bot))
        return false;

    return McFireNovaKeepsOut(botAI, McFireNovaAt(botAI, bot->GetPosition()));
}

bool McFlameImpFocusTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;

    Unit* imp = McFlameImpFocus(botAI);
    return imp && AI_VALUE(Unit*, "current target") != imp;
}
