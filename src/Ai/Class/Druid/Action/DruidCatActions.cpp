/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidCatActions.h"

#include "Creature.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>

// [rot41]
bool CastSavageRoarAction::isUseful()
{
    if (!CastMeleeSpellAction::isUseful() || !AI_VALUE2(uint8, "combo", "current target"))
        return false;

    Aura* roar = botAI->GetAura("savage roar", bot);
    return !roar || roar->GetDuration() <= int32(SAVAGE_ROAR_REFRESH_WINDOW_MS);
}

bool CastCowerAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return CastBuffSpellAction::isUseful() && !(target && target->IsPlayer());  // players have no threat
}

namespace
{
// [b31_sd] Finishers count as due this long before their buff / bleed ends.
constexpr int32 CAT_FINISHER_DUE_MS = 3 * IN_MILLISECONDS;

int32 CatSpellEnergyCost(PlayerbotAI* botAI, Player* bot, std::string const& spell)
{
    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    return info ? info->CalcPowerCost(bot, info->GetSchoolMask()) : -1;  // -1: not known
}
}  // namespace

bool CatBuilderStarvesFinisher(PlayerbotAI* botAI, Player* bot, std::string const& spell)
{
    if (botAI->HasAura("clearcasting", bot))
        return false;

    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!target)
        return false;

    uint8 const combo = botAI->GetAiObjectContext()->GetValue<uint8>("combo", "current target")->Get();
    int32 reserve = 0;

    // Savage Roar ending (or gone) with points to spend on it.
    if (combo >= 1)
    {
        Aura* roar = botAI->GetAura("savage roar", bot);
        if (!roar || roar->GetDuration() < CAT_FINISHER_DUE_MS)
            reserve = std::max(reserve, CatSpellEnergyCost(botAI, bot, "savage roar"));
    }

    // Rip due at 5 points on a target that lives through it (the Rip action's own lifetime gate).
    if (combo >= 5)
    {
        Aura* rip = botAI->GetAura("rip", target, true);
        if ((!rip || rip->GetDuration() < CAT_FINISHER_DUE_MS) && CatRipTicksOut(botAI, target))  // [rot240]
            reserve = std::max(reserve, CatSpellEnergyCost(botAI, bot, "rip"));
    }

    if (reserve <= 0)
        return false;

    int32 const cost = CatSpellEnergyCost(botAI, bot, spell);
    int32 const energy = botAI->GetAiObjectContext()->GetValue<uint8>("energy", "self target")->Get();
    return cost > 0 && energy < cost + reserve;
}

bool CatSwipeWaitsForRip(PlayerbotAI* botAI, Player* bot)
{
    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!target || !botAI->HasSpell("rip") || botAI->GetAura("rip", target, true))
        return false;

    // [rot421] Swipe builds no point: while the target lives out a missing Rip the energy builds that Rip (wowsims
    // doRotation never swipes; b230 rfc-h 5H swiped on boss + 2 adds and cast Rip 1.3 / min vs the sim's 2.1)
    if (!CatRipTicksOut(botAI, target))
        return false;

    LOG_DEBUG("playerbots", "[rot421] {} holds Swipe (Cat): {} lives out a Rip that is not on it ({} points)",
              bot->GetName(), target->GetName(),
              botAI->GetAiObjectContext()->GetValue<uint8>("combo", "current target")->Get());
    return true;
}
