/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UPMultipliers.h"
#include "ChooseTargetActions.h"
#include "GenericSpellActions.h"  // [b36_ymiron]
#include "MovementActions.h"
#include "Playerbots.h"
#include "UPActions.h"
#include "TbcHazard.h"
#include "UPTriggers.h"

float SkadiMultiplier::GetValue(Action* action)
{
    // Gauntlet (Skadi still on Grauf, not on any threat list): the Ymirjar waves keep coming until the harpoons
    // bring Grauf down, so the group holds the staging spot together. Nothing may chase an add away from it: in
    // wave 6 the tank ran 70 yd east after one while the healer stayed behind, and the waves killed all three.
    if (TbcHazard::IsRepositionAction(action) && !dynamic_cast<SkadiMoveToHarpoonsAction*>(action) &&
        !dynamic_cast<SkadiPickUpHarpoonAction*>(action) && !dynamic_cast<SkadiFireHarpoonAction*>(action) &&
        !dynamic_cast<AvoidFreezingCloudAction*>(action) && bot->GetMapId() == UPIds::MAP_ID &&
        // [b35_up2] a carrier whose launcher run is held stays with the group too
        (!bot->HasItemCount(UPIds::ITEM_HARPOON) || !SkadiHarpoonRunAllowed(botAI)) && UPEvent::IsGauntletRunning(bot))
    {
        constexpr float leash = 12.0f;
        Position const& staging = SkadiHarpoonStaging();
        Unit* target = AI_VALUE(Unit*, "current target");
        // [b32_pull] a mob already at the bot is fought where it stands (the tank holds a wave in the hall,
        // SkadiMoveToHarpoonsAction); only a chase away from both the bot and the staging spot is refused
        constexpr float localFightYd = 10.0f;
        if (target && target->GetExactDist2d(staging.GetPositionX(), staging.GetPositionY()) >
                          SkadiStagingReachedYd() + leash &&
            bot->GetExactDist2d(target) > localFightYd)
            return 0.0f;
    }

    Unit* boss = AI_VALUE2(Unit*, "find target", "skadi the ruthless");
    if (!boss) { return 1.0f; }

    Unit* bossMount = AI_VALUE2(Unit*, "find target", "grauf");

    if (!bossMount)
    // Actual bossfight (dismounted)
    {
        if (boss->HasAura(SPELL_SKADI_WHIRLWIND))
        {
            if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<AvoidSkadiWhirlwindAction*>(action))
            {
                return 0.0f;
            }
        }
    }
    else
    {
        // Bots tend to get stuck trying to attack the boss in the sky, not the adds on the ground
        if (dynamic_cast<AttackAction*>(action)
            && (action->GetTarget() == boss || action->GetTarget() == bossMount))
        {
            return 0.0f;
        }

        // TODO: BELOW IS EXPERIMENTAL
        // Meant to stop the jittery movement when dodging the breath.
        // Currently causes issues with making the bots unresponsive and often getting the healer killed.
        // Semi-glitchy movement is better than semi-afk bots, so this is commented out until it gets improved

        // bool cloudActive = false;
        // // Need to check two conditions here - the persistent ground effect doesn't
        // // seem to be detectable until 3-5 secs in, despite it dealing damage.
        // // The initial breath triggers straight away but once it's over, the bots will run back on
        // // to the frezzing cloud and take damage.
        // // Therefore check both conditions and trigger on either.

        // // Check this one early, if true then we don't need to iterate over any objects
        // if (bossMount->HasAura(SPELL_FREEZING_CLOUD_BREATH))
        // {
        //     cloudActive = true;
        // }

        // // Otherwise, check for persistent ground objects emitting the freezing cloud
        // GuidVector objects = AI_VALUE(GuidVector, "nearest hostile npcs");
        // for (auto i = objects.begin(); i != objects.end(); ++i)
        // {
        //     Unit* unit = botAI->GetUnit(*i);
        //     if (unit && unit->GetEntry() == NPC_BREATH_TRIGGER)
        //     {
        //         Unit::AuraApplicationMap const& Auras = unit->GetAppliedAuras();
        //         for (Unit::AuraApplicationMap::const_iterator itr = Auras.begin(); itr != Auras.end(); ++itr)
        //         {
        //             Aura* aura = itr->second->GetBase();
        //             if (aura && aura->GetId() == SPELL_FREEZING_CLOUD)
        //             {
        //                 cloudActive = true;
        //                 break;
        //             }
        //         }
        //     }
        // }

        // if (cloudActive)
        // {
        //     if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<AvoidFreezingCloudAction*>(action))
        //     {
        //         return 0.0f;
        //     }
        // }
    }

    return 1.0f;
}

// [b36_ymiron] Bane reflects every hit on Ymiron as 4-6k Shadow damage. Run 33: the old hold (auto-attack actions
// only, on the aura only) let Crusader Strike / Heroic Strike / Overpower and the queued swings through, and the
// aura reflected 3-4 of them per DPS inside 25 ms of landing: Rtpalaq, Rtwaraj and Rtdekak died in the same
// second. Now from the cast on, every bot (tank included) holds attack actions and every spell that would damage
// him (YmironBaneAllowsSpell: heals, buffs, interrupts, taunts, purges and single-target damage on adds go on).
// The tank keeps the boss through its threat lead plus taunts; YmironHoldDamageAction stops the running swings.
float YmironMultiplier::GetValue(Action* action)
{
    if (!UPEvent::IsBaneThreat(botAI))
        return 1.0f;

    Unit* boss = UPEvent::GetYmiron(botAI);
    if (!boss)
        return 1.0f;

    if (dynamic_cast<AttackAction*>(action))
        return 0.0f;

    CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
    if (!cast || dynamic_cast<CastHealingSpellAction*>(action))
        return 1.0f;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", cast->getSpell());
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!info)
        return 1.0f;

    return YmironBaneAllowsSpell(info, action->GetTarget(), boss) ? 1.0f : 0.0f;
}
