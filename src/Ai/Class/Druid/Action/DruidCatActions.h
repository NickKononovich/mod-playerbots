/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DRUIDCATACTIONS_H
#define PLAYERBOTS_DRUIDCATACTIONS_H

#include "DruidTriggers.h"
#include "GenericSpellActions.h"
#include "ReachTargetActions.h"

constexpr uint32 SPELL_POUNCE_RANK_1 = 9005;
constexpr uint32 SPELL_RAVAGE_RANK_1 = 6785;

class PlayerbotAI;

class CastFeralChargeCatAction : public CastReachTargetSpellAction
{
public:
    CastFeralChargeCatAction(PlayerbotAI* botAI) : CastReachTargetSpellAction(botAI, "feral charge - cat", 1.5f) {}

    bool isUseful() override
    {
        if (botAI->HasAura("prowl", bot))
            return false;

        return CastReachTargetSpellAction::isUseful();
    }
};

class CastCowerAction : public CastBuffSpellAction
{
public:
    CastCowerAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "cower") {}
    bool isUseful() override;
};

class CastBerserkAction : public CastBuffSpellAction
{
public:
    CastBerserkAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "berserk") {}
};

class CastTigersFuryAction : public CastBuffSpellAction
{
public:
    CastTigersFuryAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "tiger's fury") {}
};

// [rot41] Savage Roar 52610's explicit target is the enemy that holds the combo points (as Slice and Dice): cast on
// the cat itself SpellInfo::CheckExplicitTarget refused it ("CheckCast BAD_TARGETS", b180 5H Rtdruaf: 0 Roars in every
// boss fight against the sim's 3/min). Cast on the current target; useful while the cat's own Roar is missing or
// inside SAVAGE_ROAR_REFRESH_WINDOW_MS.
class CastSavageRoarAction : public CastMeleeSpellAction
{
public:
    CastSavageRoarAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "savage roar") {}
    bool isUseful() override;
};

// [b31_sd] Roar recast early to desync it from Rip ("savage roar clip", SavageRoarClipTrigger decides when): no
// refresh-window check, only a combo point and the spell itself.
class CastSavageRoarClipAction : public CastSpellAction
{
public:
    CastSavageRoarClipAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "savage roar") {}

    // [rot41] on the current target, the holder of the combo points (see CastSavageRoarAction)
    std::string const getName() override { return "savage roar clip"; }
    bool isUseful() override
    {
        return AI_VALUE2(uint8, "combo", "current target") >= 1 && CastSpellAction::isUseful();
    }
};

// [b31_sd] A melee debuff (range = melee, isUseful checks melee range): as a plain debuff it ran at spell range and
// the core refused it out of melee, a lost tick (FERAL_CAT_AUDIT.md 2.2.9).
class CastRakeAction : public CastMeleeDebuffSpellAction
{
public:
    CastRakeAction(PlayerbotAI* botAI) : CastMeleeDebuffSpellAction(botAI, "rake", true, 6.0f)
    {
        beforeDuration = FERAL_DOT_REFRESH_WINDOW_MS;
    }
};

// [b31_sd] Energy pooling (FERAL_CAT_AUDIT.md 2.2.13): a combo-point builder is held back when its cost would leave
// too little energy for a finisher that is due (Savage Roar ending with points on the target, Rip missing at 5
// points on a long-lived target). Never under Clearcasting (the builder is free).
bool CatBuilderStarvesFinisher(PlayerbotAI* botAI, Player* bot, std::string const& spell);
// [b31_sd] On a boss without this druid's Rip, at 5 points, Swipe (cat) waits for Rip.
bool CatSwipeWaitsForRip(PlayerbotAI* botAI, Player* bot);
// [b31_sd] Shred and Ravage need the cat behind the target (SPELL_FAILED_NOT_BEHIND, Spell::CheckCast).
inline bool CatIsBehind(Player* bot, Unit* target)
{
    return target && !target->HasInArc(static_cast<float>(M_PI), bot);
}

class CastClawAction : public CastMeleeSpellAction
{
public:
    CastClawAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "claw") {}

    bool isUseful() override
    {
        // Block Claw once Pounce is learned; Claw remains available as the stealth opener before then.
        if (botAI->HasAura("prowl", bot) && bot->HasSpell(SPELL_POUNCE_RANK_1))
            return false;

        // [b31_sd] energy pooling for a due finisher
        return !CatBuilderStarvesFinisher(botAI, bot, spell) && CastMeleeSpellAction::isUseful();
    }
};

class CastMangleCatAction : public CastMeleeSpellAction
{
public:
    CastMangleCatAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "mangle (cat)") {}

    bool isUseful() override
    {
        if (botAI->HasAura("prowl", bot))
            return false;

        return !CatBuilderStarvesFinisher(botAI, bot, spell) && CastMeleeSpellAction::isUseful();  // [b31_sd]
    }
};

class CastSwipeCatAction : public CastMeleeSpellAction
{
public:
    CastSwipeCatAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "swipe (cat)") {}

    bool isUseful() override
    {
        if (botAI->HasAura("prowl", bot))
            return false;

        return !CatSwipeWaitsForRip(botAI, bot) && CastMeleeSpellAction::isUseful();  // [b31_sd]
    }
};

class CastFerociousBiteAction : public CastMeleeSpellAction
{
public:
    CastFerociousBiteAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "ferocious bite") {}
};

class CastMaimAction : public CastMeleeSpellAction
{
public:
    CastMaimAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "maim") {}

    bool isUseful() override
    {
        Unit* target = GetTarget();
        if (!target || !target->ToPlayer())
            return false;

        return CastMeleeSpellAction::isUseful();
    }
};

class CastRipAction : public CastMeleeDebuffSpellAction
{
public:
    CastRipAction(PlayerbotAI* botAI) : CastMeleeDebuffSpellAction(botAI, "rip", true, RIP_MIN_TARGET_LIFETIME)
    {
        beforeDuration = FERAL_DOT_REFRESH_WINDOW_MS;
    }
};

class CastShredAction : public CastMeleeSpellAction
{
public:
    CastShredAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "shred") {}

    bool isUseful() override
    {
        if (botAI->HasAura("prowl", bot) && bot->HasSpell(SPELL_RAVAGE_RANK_1))
            return false;

        // [b31_sd] Shred requires the cat behind the target (the core fails it with SPELL_FAILED_NOT_BEHIND when the
        // target has the caster in its front arc, Spell::CheckCast). In front, every Shred node falls through to the
        // Mangle (cat) queued next to it.
        if (!CatIsBehind(bot, GetTarget()))
            return false;

        return !CatBuilderStarvesFinisher(botAI, bot, spell) && CastMeleeSpellAction::isUseful();  // [b31_sd]
    }
};

class CastProwlAction : public CastBuffSpellAction
{
public:
    CastProwlAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "prowl") {}
};

class CastDashAction : public CastBuffSpellAction
{
public:
    CastDashAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "dash") {}
};

class CastRavageAction : public CastMeleeSpellAction
{
public:
    CastRavageAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "ravage") {}

    bool isUseful() override
    {
        // [b31_sd] Ravage, like Shred, needs the cat behind the target; in front Pounce (5.5) opens instead.
        if (!botAI->HasAura("prowl", bot) || !CatIsBehind(bot, GetTarget()))
            return false;

        return CastMeleeSpellAction::isUseful();
    }
};

class CastPounceAction : public CastMeleeSpellAction
{
public:
    CastPounceAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "pounce") {}

    bool isUseful() override
    {
        if (!botAI->HasAura("prowl", bot))
            return false;

        return CastMeleeSpellAction::isUseful();
    }
};

#endif
