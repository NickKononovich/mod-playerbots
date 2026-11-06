/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROGUEFINISHINGACTIONS_H
#define PLAYERBOTS_ROGUEFINISHINGACTIONS_H

#include "GenericSpellActions.h"

class PlayerbotAI;
class Player;
class Unit;

// Finishers are refreshed inside this window rather than after they drop
constexpr uint32 SLICE_AND_DICE_REFRESH_MS = 1000;
constexpr uint32 RUPTURE_REFRESH_MS = 1000;
// [b31_wr] Hunger for Blood's self buff (60 s) is refreshed below this
constexpr uint32 HUNGER_FOR_BLOOD_REFRESH_MS = 5000;
// The Hunger for Blood enabler Rupture (Assassination, "assassination rupture") is its bleed, not its damage: it keeps
// the target-lifetime gate it had
constexpr float RUPTURE_MIN_TARGET_LIFETIME = 12.0f;

// [rot561] Rupture where the ticks that land before the target dies (ExpectedFightLifetime) beat the
// Eviscerate the same combo points would deal (wowsims rogue__combat.apl.json: Rupture or Eviscerate at 4-5 points;
// its "remainingTime > 10 s" is the fight's end, here the target's computed life), in place of the fixed 12 s by
// health / group dps. The ticks at the points it is cast with (WorldObject::CalcSpellDuration, SPELLMOD_DURATION:
// Glyph of Rupture). Both spells measured (lead1542): SpellPayoff::DotPays; before either has landed the spells'
// estimate plus the attack power parts the core adds outside the spell data (spell_rog_rupture per point,
// Spell::EffectSchoolDMG Eviscerate 0.07 x points), Eviscerate after armor. b276 rfc-h 5B Rtrogaa: Rupture 1.0 a
// minute vs the sim's 1.9, Eviscerate 2.7 vs 1.2. The Hunger for Blood enabler keeps RUPTURE_MIN_TARGET_LIFETIME.
// `loggedMs`: a held Rupture is logged at most every RUPTURE_PAYOFF_LOG_MS (none: no log). Pays when either spell is
// unknown.
constexpr uint32 RUPTURE_PAYOFF_LOG_MS = 5000;
bool RupturePays(PlayerbotAI* botAI, Player* bot, Unit* target, uint32* loggedMs = nullptr);

class CastEviscerateAction : public CastMeleeSpellAction
{
public:
    CastEviscerateAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "eviscerate") {}
};

class CastSliceAndDiceAction : public CastMeleeSpellAction
{
public:
    CastSliceAndDiceAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "slice and dice") {}
};

class CastExposeArmorAction : public CastDebuffSpellAction
{
public:
    CastExposeArmorAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "expose armor", false, 25.0f) {}
};

class CastRuptureAction : public CastDebuffSpellAction
{
public:
    CastRuptureAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "rupture", true, 0.0f)
    {
        beforeDuration = RUPTURE_REFRESH_MS;
    }

    // [rot561] RupturePays; the Hunger for Blood enabler by the target's lifetime
    bool isUseful() override;

private:
    uint32 _loggedMs = 0;
};

// [rot432] Kick's fallback when Kick cannot go (its cooldown, its energy). User rule (WotLK PvE practice): a rogue
// interrupts a boss with Kick only, Kidney Shot never; on trash it stops a mob that hits a non-tank member.
class CastKidneyShotAction : public CastMeleeSpellAction
{
public:
    CastKidneyShotAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "kidney shot") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
