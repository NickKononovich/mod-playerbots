/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UKMultipliers.h"
#include "ChooseTargetActions.h"
#include "GenericSpellActions.h"
#include "TbcHazard.h"
#include "UKActions.h"
#include "UKTriggers.h"
#include <algorithm>

namespace
{
// [b33_uk] Woe Strike (see SPELL_WOE_STRIKE): every positive spell landing on the struck unit costs its caster a
// 1750-2250 Shadow backlash. Wipe uk-h-5G-34: an Arms warrior's Battle Shout (24 casts in the pull) hit the struck
// tank every GCD, 2.1-2.3k a time, 8.7k in the 5 s before Dreadful Roar finished him; the healer's Hand of Sacrifice
// on that tank cost her 2.4k plus 3.2k of transferred melee right before the Roar that killed her. Heals keep going
// (that backlash is the encounter) and so do absorbs and the caster's own self-only cooldowns; buffs, hands, shouts
// and party auras that would reach a struck unit wait until it wears off. O(1): only Ingvar's victim and the bot.
bool CarriesWoeStrike(Unit* unit, uint32 woeStrike) { return unit && unit->IsAlive() && unit->HasAura(woeStrike); }

bool WouldFeedWoeStrike(PlayerbotAI* botAI, Player* bot, Unit* boss, CastSpellAction* cast)
{
    if (dynamic_cast<CastHealingSpellAction*>(cast))
        return false;

    uint32 const woeStrike = SPELL_WOE_STRIKE;
    Unit* victim = boss->GetVictim();
    bool const victimStruck = CarriesWoeStrike(victim, woeStrike);
    bool const selfStruck = CarriesWoeStrike(bot, woeStrike);
    if (!victimStruck && !selfStruck)
        return false;

    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", cast->getSpell())->Get();
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo || !spellInfo->IsPositive())
        return false;

    bool selfOnly = true;
    bool area = false;
    float radius = 0.0f;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = spellInfo->Effects[i];
        if (!effect.IsEffect())
            continue;

        if (effect.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB)
            return false;

        if (effect.IsAreaAuraEffect() || effect.IsTargetingArea())
        {
            area = true;
            radius = std::max(radius, effect.CalcRadius(bot));
        }
        if (area || effect.TargetA.GetTarget() != TARGET_UNIT_CASTER)
            selfOnly = false;
    }
    if (selfOnly)
        return false;

    if (area)
        return selfStruck || (victimStruck && bot->GetExactDist2d(victim) <= (radius > 0.0f ? radius : 30.0f));

    return CarriesWoeStrike(cast->GetTarget(), woeStrike);
}
}  // namespace

float PrinceKelesethMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "prince keleseth");
    if (!boss) { return 1.0f; }

    // Suppress auto-targeting behaviour only when a tomb is up
    if (dynamic_cast<DpsAssistAction*>(action))
    {
        GuidVector members = AI_VALUE(GuidVector, "group members");
        for (auto& member : members)
        {
            Unit* unit = botAI->GetUnit(member);
            if (unit && unit->HasAura(SPELL_FROST_TOMB))
            {
                return 0.0f;
            }
        }
    }
    return 1.0f;
}

float SkarvaldAndDalronnMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<DpsAssistAction*>(action))
        return 1.0f;

    // While the living Skarvald is up, "attack skarvald" picks the target; once he is dead the normal dps target
    // logic takes the living Dalronn (both ghosts are unselectable).
    Unit* skarvald = AI_VALUE2(Unit*, "find target", "skarvald the constructor");
    if (skarvald && skarvald->GetEntry() == UKIds::NPC_SKARVALD && skarvald->isTargetableForAttack())
        return 0.0f;

    return 1.0f;
}

float IngvarThePlundererMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "ingvar the plunderer");
    bool isTank = PlayerbotAI::IsTank(bot);
    if (!boss) { return 1.0f; }

    // [b33_uk] no buff, hand or shout onto a Woe Strike target (see WouldFeedWoeStrike above).
    if (CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action))
        if (WouldFeedWoeStrike(botAI, bot, boss, cast))
            return 0.0f;

    // Prevent movement actions overriding current movement, we're probably dodging a slam
    if (isTank && bot->isMoving() && dynamic_cast<MovementAction*>(action))
    {
        return 0.0f;
    }

    // If boss is casting a roar, do not allow beginning a spell cast that is non-instant
    if (boss->HasUnitState(UNIT_STATE_CASTING))
    {
        if (boss->FindCurrentSpellBySpellId(SPELL_STAGGERING_ROAR) ||
            boss->FindCurrentSpellBySpellId(SPELL_DREADFUL_ROAR))
        {
            if (CastSpellAction* rcast = dynamic_cast<CastSpellAction*>(action))
            {
                // [b33_uk] the spell, not the action name: "flash of light on party" resolved to no spell id, so
                // every party heal slipped through this guard and could be started during the Roar's cast, to be
                // cut by its 6-8 s school lockout.
                uint32 spellId = AI_VALUE2(uint32, "spell id", rcast->getSpell());
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
                if (!spellInfo) { return 1.0f; }

                uint32 castTime = spellInfo->CalcCastTime(bot);
                if (castTime != 0)
                {
                    return 0.0f;
                }
            }
        }
        // Non-tanks: during Smash only "ingvar avoid smash" moves them (it is exempt as a hazard move); a reach or
        // formation move would carry a bot that has just left the cone straight back into it.
        if (!isTank)
        {
            if (UKIds::IsIngvarSmashing(bot, boss) && TbcHazard::IsRepositionAction(action))
                return 0.0f;

            return 1.0f;
        }

        // TANK ONLY
        if (boss->FindCurrentSpellBySpellId(SPELL_SMASH) ||
            boss->FindCurrentSpellBySpellId(SPELL_DARK_SMASH))
        {
            // Prevent movement actions during smash which can mess up boss position.
            // Allow through IngvarDodgeSmashAction only, as well as any non-movement actions.
            if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<IngvarDodgeSmashAction*>(action))
            {
                return 0.0f;
            }
        }
    }
    return 1.0f;
}
