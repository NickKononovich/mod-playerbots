/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DTKMultipliers.h"
#include "Action.h"
#include "ChooseTargetActions.h"
#include "DTKActions.h"
#include "DTKTriggers.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"
#include "Playerbots.h"
#include "ShamanActions.h"
#include "StringFormat.h"
#include "TbcHazard.h"
#include "Timer.h"

namespace
{
constexpr float TROLLGORE_TRASH_RADIUS = 30.0f;
constexpr uint32 TROLLGORE_HOLD_MAX_MS = 120 * IN_MILLISECONDS;
// Drak'Tharon Keep doc: a Darkweb Victim cocoon summons another Drakkari when killed; it is left alone, not cleared.
constexpr uint32 NPC_DARKWEB_VICTIM = 27909;
}  // namespace

float NovosMultiplier::GetValue(Action* action)
{
    // Inside the field nothing but the escape may move the bot; just outside it, nothing may carry it back in.
    if (TbcHazard::IsRepositionAction(action) || dynamic_cast<MovementAction*>(action))
    {
        bool const escape = dynamic_cast<AvoidArcaneFieldAction*>(action) != nullptr;
        if (!escape && bot->HasAura(SPELL_ARCANE_FIELD) && dynamic_cast<MovementAction*>(action))
            return 0.0f;

        if (!escape && TbcHazard::IsRepositionAction(action))
            if (Unit* center = DTKShared::ArcaneFieldCenter(botAI))
                if (TbcHazard::ShouldHoldOutside(botAI, center->GetPosition(),
                                                 DTKShared::ARCANE_FIELD_RADIUS + bot->GetCombatReach()))
                    return 0.0f;
    }

    Unit* boss = AI_VALUE2(Unit*, "find target", "novos the summoner");
    if (!boss) { return 1.0f; }

    if (boss->FindCurrentSpellBySpellId(SPELL_ARCANE_FIELD) && bot->GetTarget())
    {
        if (dynamic_cast<DpsAssistAction*>(action)
            || dynamic_cast<TankAssistAction*>(action))
        {
            return 0.0f;
        }
    }
    return 1.0f;
}

float TharonjaMultiplier::GetValue(Action* action)
{
    if (!bot->HasAura(SPELL_GIFT_OF_THARONJA)) { return 1.0f; }

    // Suppress all skills that are not enabled in skeleton form.
    // Still allow non-ability actions such as movement
    if (dynamic_cast<CastSpellAction*>(action)
        && !dynamic_cast<CastSlayingStrikeAction*>(action)
        && !dynamic_cast<CastTauntAction*>(action)
        && !dynamic_cast<CastBoneArmorAction*>(action)
        && !dynamic_cast<CastTouchOfLifeAction*>(action))
    {
        return 0.0f;
    }
    // Also suppress FleeAction to prevent ranged characters from avoiding melee range
    if (dynamic_cast<FleeAction*>(action))
    {
        return 0.0f;
    }

    // Tanks should only taunt, no slaying strike
    if (PlayerbotAI::IsTank(bot))
    {
        if (dynamic_cast<CastSlayingStrikeAction*>(action))
        {
            return 0.0f;
        }
    }
    // Dps & healer should not taunt
    else
    {
        if (dynamic_cast<CastTauntAction*>(action))
        {
            return 0.0f;
        }
    }
    return 1.0f;
}

float KingDredMultiplier::GetValue(Action* action)
{
    if (bot->getClass() != CLASS_SHAMAN)
        return 1.0f;

    bool const competing =
        dynamic_cast<CastStrengthOfEarthTotemAction*>(action) || dynamic_cast<CastStoneskinTotemAction*>(action) ||
        dynamic_cast<CastEarthbindTotemAction*>(action) || dynamic_cast<CastStoneclawTotemAction*>(action);
    if (!competing)
        return 1.0f;

    Unit* dred = DTKShared::KingDred(botAI);
    if (!dred || !DTKShared::DredFearWindow(dred) || !AI_VALUE2(uint32, "spell id", "tremor totem"))
        return 1.0f;

    DTKShared::Log(bot, "dtk dred tremor hold", action->getName(),
                   Acore::StringFormat("{} held for tremor totem, king dred at {:.0f} %", action->getName(),
                                       dred->GetHealthPct()),
                   10 * IN_MILLISECONDS);
    return 0.0f;
}

float DTKTankOpensMultiplier::GetValue(Action* action)
{
    if (bot->IsInCombat() || PlayerbotAI::IsTank(bot) || bot->GetMapId() != DTKIds::MAP_ID)
        return 1.0f;

    // Planned pre-pull crowd control belongs to the tank's pull plan.
    if ((!dynamic_cast<AttackAction*>(action) && !dynamic_cast<CastSpellAction*>(action)) ||
        dynamic_cast<CastCrowdControlSpellAction*>(action))
        return 1.0f;

    if (!DTKShared::BeforeTrollgore(bot))
        return 1.0f;

    Unit* target = action->GetTarget();
    if (!target || target == bot || !target->IsCreature() || !target->IsAlive() || !bot->IsHostileTo(target) ||
        DTKShared::EngagedWithGroup(bot, target))
        return 1.0f;

    // Without a living tank nobody else could open the fight.
    if (!DTKShared::GroupTank(bot))
        return 1.0f;

    DTKShared::Log(bot, "dtk tank opens", target->GetGUID().ToString(),
                   Acore::StringFormat("{} held on idle {} ({}), {:.1f} yd: only the tank opens before Trollgore",
                                       action->getName(), target->GetName(), target->GetEntry(),
                                       bot->GetExactDist(target)),
                   10 * IN_MILLISECONDS);
    return 0.0f;
}

float TrollgoreBodyPullMultiplier::GetValue(Action* action)
{
    if (bot->IsInCombat())
    {
        _holdSinceMs = 0;
        return 1.0f;
    }

    if (bot->GetMapId() != DTKIds::MAP_ID || !dynamic_cast<AttackAction*>(action))
        return 1.0f;

    Unit* boss = action->GetTarget();
    if (!boss || boss->GetEntry() != DTKIds::NPC_TROLLGORE || !boss->IsAlive() || boss->IsInCombat())
        return 1.0f;

    Unit* nearest = nullptr;
    uint32 idle = 0;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        uint32 const entry = guid.GetEntry();
        if (!guid.IsCreature() || entry == DTKIds::NPC_TROLLGORE || DTKIds::IsTrollgoreInvader(entry) ||
            entry == NPC_DARKWEB_VICTIM)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->IsInCombat() || unit->IsCritter() || !bot->IsHostileTo(unit) ||
            boss->GetExactDist(unit) > TROLLGORE_TRASH_RADIUS)
            continue;

        ++idle;
        if (!nearest || boss->GetExactDist(unit) < boss->GetExactDist(nearest))
            nearest = unit;
    }
    if (!nearest)
        return 1.0f;

    uint32 const now = getMSTime();
    if (!_holdSinceMs)
        _holdSinceMs = now;

    if (getMSTimeDiff(_holdSinceMs, now) > TROLLGORE_HOLD_MAX_MS)
    {
        DTKShared::Log(bot, "dtk trollgore body pull", "released",
                       Acore::StringFormat("{} on trollgore released after {} s, {} idle mob(s) still within {:.0f} yd "
                                           "(nearest {} {:.1f} yd)",
                                           action->getName(), TROLLGORE_HOLD_MAX_MS / IN_MILLISECONDS, idle,
                                           TROLLGORE_TRASH_RADIUS, nearest->GetName(), boss->GetExactDist(nearest)),
                       30 * IN_MILLISECONDS);
        return 1.0f;
    }

    DTKShared::Log(bot, "dtk trollgore body pull", "held",
                   Acore::StringFormat("{} on trollgore held: {} idle mob(s) within {:.0f} yd of him (nearest {} ({}) "
                                       "{:.1f} yd)",
                                       action->getName(), idle, TROLLGORE_TRASH_RADIUS, nearest->GetName(),
                                       nearest->GetEntry(), boss->GetExactDist(nearest)),
                   10 * IN_MILLISECONDS);
    return 0.0f;
}
