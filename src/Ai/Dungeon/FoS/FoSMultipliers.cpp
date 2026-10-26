/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FoSMultipliers.h"
#include "ChooseTargetActions.h"
#include "DungeonHoldFire.h"
#include "FoSActions.h"
#include "FoSTriggers.h"
#include "GenericSpellActions.h"
#include "HunterActions.h"
#include "MageActions.h"
#include "MovementActions.h"
#include "ReachTargetActions.h"

namespace
{
// Moves the bot (walks, blinks, leaps, charges) somewhere of its own choosing. Attack actions derive from
// MovementAction but only pick and engage a target; the boss stands still in Soulstorm.
bool IsSoulstormReposition(Action* action)
{
    if (dynamic_cast<BronjahmGroupPositionAction*>(action) || dynamic_cast<AttackAction*>(action))
        return false;

    return dynamic_cast<MovementAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action) ||
           dynamic_cast<CastBlinkBackAction*>(action) || dynamic_cast<CastDisengageAction*>(action);
}
}  // namespace

float BronjahmMultiplier::GetValue(Action* action) {
    Unit* boss = AI_VALUE2(Unit *, "find target", "bronjahm");
    if (!boss)
        return 1.0f;

    if (dynamic_cast<TankAssistAction*>(action))
    {
        // Keeps a tank that holds Bronjahm off the Corrupted Soul Fragments (they walk into the boss, the dps kill
        // them). Tank assist is also how a tank in the non-combat engine joins a fight: zeroed for the whole fight,
        // a tank that had dropped its target stayed in "combat true state 1" - no rotation, no taunts - while the
        // boss killed the healer (two wipes). So it only stops a switch from the boss to a fragment.
        if (botAI->GetState() != BOT_STATE_COMBAT || AI_VALUE(Unit*, "current target") != boss)
            return 1.0f;

        Unit* target = action->GetTarget();
        return (target && target->GetEntry() == NPC_CORRUPTED_SOUL_FRAGMENT) ? 0.0f : 1.0f;
    }

    // Soulstorm: the only safe ground is the 10 yd eye at Bronjahm's feet. Wave 8: the ranged position (an enemy
    // fighting the tank within 8.25 yd) walked the hunter and the healer back out to 22 yd every few seconds until
    // the hunter died. Flee, blink, disengage, formation spread, follow and every other reposition wait; the stack
    // move of "bronjahm group position" brings each bot back in after a fear.
    if (FoSShared::IsSoulstorm(boss))
        return IsSoulstormReposition(action) ? 0.0f : 1.0f;

    // the tank keeps tanking while it carries Corrupt Soul; everyone else only runs
    if (bot->HasAura(SPELL_CORRUPT_SOUL) && !PlayerbotAI::IsTank(bot))
    {
        if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<MoveFromBronjahmAction*>(action))
        {
            return 0.0f;
        }
    }
    return 1.0f;
}

float AttackFragmentMultiplier::GetValue(Action* action)
{
    // cheap checks first: this runs for every queued action anywhere in the Forge of Souls
    if (!dynamic_cast<BronjahmGroupPositionAction*>(action) || !PlayerbotAI::IsDps(bot))
        return 1.0f;

    // in Soulstorm the stack comes first: a fragment walking in is shot from the eye
    if (FoSShared::IsSoulstorm(AI_VALUE2(Unit*, "find target", "bronjahm")))
        return 1.0f;

    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && unit->GetEntry() == NPC_CORRUPTED_SOUL_FRAGMENT)
            return 0.0f;
    }

    return 1.0f;
}

float DevourerOfSoulsMultiplier::GetValue(Action* action)
{
    if (!bot->IsInCombat() || dynamic_cast<DevourerOfSoulsAction*>(action) ||
        dynamic_cast<AvoidWailingSoulsAction*>(action))
        return 1.0f;

    if (Unit* boss = FoSShared::GetMirroringDevourer(botAI);
        boss && DungeonHoldFire::IsDamageAction(bot, action, boss))
        return 0.0f;

    // attacks keep running from behind; every other move (follow, formation, reach) could walk into the beam
    if (FoSShared::GetWailingDevourer(botAI) && dynamic_cast<MovementAction*>(action) &&
        !dynamic_cast<AttackAction*>(action))
        return 0.0f;

    return 1.0f;
}
