/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FoSTriggers.h"
#include "AiObjectContext.h"
#include "DungeonHoldFire.h"
#include "Map.h"
#include "Playerbots.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"

#include <array>

namespace FoSShared
{
namespace
{
Unit* FindDevourer(PlayerbotAI* botAI)
{
    return botAI->GetAiObjectContext()->GetValue<Unit*>("find target", "devourer of souls")->Get();
}

// The Wailing Souls channel aura on the Devourer (68875 per spell_scripts; 68876 is its unused twin).
AuraEffect const* GetWailingPeriodic(Unit* boss)
{
    if (AuraEffect const* periodic = boss->GetAuraEffect(FoSIds::SPELL_WAILING_SOULS_PERIODIC, EFFECT_0))
        return periodic;

    return boss->GetAuraEffect(FoSIds::SPELL_WAILING_SOULS_PERIODIC_2, EFFECT_0);
}
}

Unit* GetMirroringDevourer(PlayerbotAI* botAI)
{
    Unit* boss = FindDevourer(botAI);
    if (!boss || !boss->IsAlive())
        return nullptr;

    // 69051 is a channel whose auras sit on the victim; the boss itself carries 69023 (spell_scripts of 69051)
    return boss->FindCurrentSpellBySpellId(FoSIds::SPELL_MIRRORED_SOUL_CHANNEL) ||
                   boss->HasAura(FoSIds::SPELL_MIRRORED_SOUL_PROC)
               ? boss
               : nullptr;
}

Unit* GetWailingDevourer(PlayerbotAI* botAI)
{
    Unit* boss = FindDevourer(botAI);
    if (!boss || !boss->IsAlive())
        return nullptr;

    if (boss->FindCurrentSpellBySpellId(SPELL_WAILING_SOULS))
        return boss;

    // after the last beam the aura lingers ~2 s while the boss is still rooted: nothing left to dodge
    AuraEffect const* periodic = GetWailingPeriodic(boss);
    return periodic && periodic->GetTickNumber() < FoSIds::WAILING_SOULS_LAST_BEAM_TICK ? boss : nullptr;
}

float GetWailingDangerAngle(Unit* boss)
{
    // during the 3 s cast no tick has run yet: the whole sweep is still ahead
    uint32 ticks = 0;
    if (AuraEffect const* periodic = GetWailingPeriodic(boss))
        ticks = periodic->GetTickNumber();

    uint32 const turnsLeft =
        ticks < FoSIds::WAILING_SOULS_LAST_BEAM_TICK ? FoSIds::WAILING_SOULS_LAST_BEAM_TICK - ticks : 0;

    uint32 const beamSpell =
        boss->GetMap()->IsHeroic() ? FoSIds::SPELL_WAILING_SOULS_DMG_H : FoSIds::SPELL_WAILING_SOULS_DMG_N;
    float coneDegrees = FoSIds::WAILING_SOULS_DEFAULT_CONE_DEGREES;
    if (SpellCone const* cone = sSpellMgr->GetSpellCone(beamSpell))
        coneDegrees = static_cast<float>(cone->cone_degrees);

    float const halfCone = coneDegrees * static_cast<float>(M_PI) / 360.0f;
    return halfCone + turnsLeft * FoSIds::WAILING_SOULS_TURN_PER_TICK + FoSIds::WAILING_SOULS_SAFETY_MARGIN;
}

bool IsSafeFromWailing(Unit* boss, float x, float y, float dangerAngle)
{
    if (boss->GetExactDist2d(x, y) < boss->GetCombatReach() + FoSIds::WAILING_SOULS_BOUNDARY_EXTRA)
        return false;

    float relative = boss->GetRelativeAngle(x, y);
    if (relative > static_cast<float>(M_PI))
        relative = 2.0f * static_cast<float>(M_PI) - relative;

    return relative > dangerAngle;
}

Unit* GetPhantomBlastCaster(PlayerbotAI* botAI)
{
    constexpr std::array<uint32, 2> PHANTOM_BLAST = {FoSIds::SPELL_PHANTOM_BLAST, FoSIds::SPELL_PHANTOM_BLAST_H};
    Unit* boss = FindDevourer(botAI);
    return DungeonHoldFire::IsCastingAny(boss, PHANTOM_BLAST) ? boss : nullptr;
}

bool IsSoulstorm(Unit* boss)
{
    // 68870 is applied (triggered) together with the 4 s Soulstorm cast: the group has those 4 s to stack.
    return boss && boss->IsAlive() &&
           (boss->FindCurrentSpellBySpellId(FoSIds::SPELL_BRONJAHM_TELEPORT) ||
            boss->FindCurrentSpellBySpellId(SPELL_SOULSTORM) || boss->HasAura(SPELL_SOULSTORM) ||
            boss->HasAura(SPELL_SOULSTORM_VISUAL));
}

Position GetSoulstormCentre(Unit* boss)
{
    if (boss->FindCurrentSpellBySpellId(FoSIds::SPELL_BRONJAHM_TELEPORT))
        return Position(FoSIds::SOULSTORM_CENTRE_X, FoSIds::SOULSTORM_CENTRE_Y, FoSIds::SOULSTORM_CENTRE_Z);

    return boss->GetPosition();
}
}  // namespace FoSShared

bool MoveFromBronjahmTrigger::IsActive()
{
    // Corrupt Soul is instant: the carrier only knows it from the 4 s aura, then the fragment spawns at its feet.
    // A tank stays with the boss; the tank logic of "bronjahm position" kites the boss off the fragment.
    if (!bot->HasAura(SPELL_CORRUPT_SOUL) || PlayerbotAI::IsTank(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "bronjahm");
    if (!boss)
        return false;

    // A run 25 yd out in Soulstorm ends ~15 yd deep in the storm (Teleport cancels the Corrupt Soul timer, but an
    // aura from just before it can still be ticking): the carrier stacks with the group instead.
    if (FoSShared::IsSoulstorm(boss))
        return false;

    return bot->GetExactDist2d(boss) < FoSIds::CORRUPT_SOUL_FLEE_DISTANCE;
}

bool SwitchToSoulFragment::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "bronjahm");
    if (!boss)
        return false;

    Unit* corruptedSoul = bot->FindNearestCreature(NPC_CORRUPTED_SOUL_FRAGMENT, 50.0f);
    bool activeSoulExists = corruptedSoul && corruptedSoul->IsAlive();

    if (!activeSoulExists)
        return false;

    return true;
}

bool BronjahmPositionTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "bronjahm");
    if (!boss)
        return false;

    if (bot->HasAura(SPELL_CORRUPT_SOUL) && !FoSShared::IsSoulstorm(boss))
        return false;

    return true;
}

bool DevourerOfSoulsTrigger::IsActive()
{
    return DungeonHoldFire::IsDamaging(botAI, FoSShared::GetMirroringDevourer(botAI));
}

bool WailingSoulsTrigger::IsActive()
{
    Unit* boss = FoSShared::GetWailingDevourer(botAI);
    return boss && !FoSShared::IsSafeFromWailing(boss, bot->GetPositionX(), bot->GetPositionY(),
                                                 FoSShared::GetWailingDangerAngle(boss));
}

bool FoSInterruptTrigger::IsActive()
{
    if (!bot->IsInCombat() || !DungeonHoldFire::HasInterrupt(botAI))
        return false;

    return FoSShared::GetPhantomBlastCaster(botAI) != nullptr;
}
