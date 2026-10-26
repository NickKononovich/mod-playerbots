/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PaladinPullStrategy.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

std::string PaladinPullStrategy::GetPullActionName() const
{
    Unit* target = GetTarget();
    if (!target ||
        (!botAI->HasStrategy("prot", BOT_STATE_COMBAT) && !botAI->HasStrategy("prot", BOT_STATE_NON_COMBAT)))
    {
        // [pal_pull_spell_fallback] The base pull spell "judgement" names no spell a 3.3.5 paladin knows (its
        // judgements are "judgement of light / wisdom / justice"): "spell id" reads 0 and the pull action stays
        // USELESS ("judgement on Dark Rune Warrior 41.0 yd - spell not known"). hos-n 2026-10-25
        // (20261025-115805-hos-n-5A-19, 195-354 s): the prot tank re-planned "pull one" for 160 s and never cast. A
        // paladin tank that knows Hand of Reckoning (30 yd, one mob) pulls with it.
        Player* bot = botAI->GetBot();
        uint32 const known = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "hand of reckoning")->Get();
        if (target && bot && PlayerbotAI::IsTank(bot) && known && bot->HasSpell(known))
            return "hand of reckoning";

        return PullStrategy::GetPullActionName();
    }

    // One mob only (the lone-mob pull): Avenger's Shield jumps to two more, and the fallback (Judgement, 10 yd)
    // walks the tank into the pack's aggro. Hand of Reckoning reaches 30 yd and hits the one mob.
    // Even on cooldown (it is 8 s): the cast then waits a moment instead of falling back to Judgement
    // (Stratholme wave 24: "plan one ... via spell judgement" -> 16 mobs came).
    uint32 const reckoning = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "hand of reckoning")->Get();
    if (IsSingleMob() && reckoning && botAI->GetBot()->HasSpell(reckoning))
        return "hand of reckoning";

    if (botAI->CanCastSpell("avenger's shield", target))
        return "avenger's shield";

    if (botAI->CanCastSpell("hand of reckoning", target))
        return "hand of reckoning";

    // Both fail CanCastSpell while the mob is out of sight (a LoS pull round a corner, a ramp edge) or Hand of
    // Reckoning cools down. Judgement then made the pull action walk to 10 yd of the pack: Scholomance wave 31
    // ("los pull ... with judgement", the hall casters engaged the tank on the ramp), Scarlet Monastery wave 34
    // ("via spell judgement" 4x). A 30 yd spell the tank knows is kept instead: the pull action walks into its
    // sight, and a cooldown waits a few seconds.
    Player* bot = botAI->GetBot();
    uint32 const shield = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "avenger's shield")->Get();
    if (shield && bot->HasSpell(shield) && !bot->HasSpellCooldown(shield))
        return "avenger's shield";

    if (reckoning && bot->HasSpell(reckoning))
        return "hand of reckoning";

    return PullStrategy::GetPullActionName();
}

std::string PaladinPullStrategy::GetPreActionName() const
{
    if (botAI->HasStrategy("prot", BOT_STATE_COMBAT) || botAI->HasStrategy("prot", BOT_STATE_NON_COMBAT))
        return "";

    return PullStrategy::GetPreActionName();
}

std::string PaladinPullStrategy::GetPrePullBuffActionName() const
{
    // [b31_holyshield_pull] the action checks tank, known spell, no aura, out of combat and mana itself
    if (!botAI->HasStrategy("prot", BOT_STATE_COMBAT) && !botAI->HasStrategy("prot", BOT_STATE_NON_COMBAT))
        return "";

    return "holy shield pre pull";
}
