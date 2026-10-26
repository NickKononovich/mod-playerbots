/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AKMultipliers.h"
#include "AKActions.h"
#include "AKTriggers.h"
#include "Action.h"
#include "ChooseTargetActions.h"
#include "GenericSpellActions.h"
#include "MovementActions.h"
#include "WlkTacticLog.h"

#include <string>
#include <unordered_set>

float ElderNadoxMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "elder nadox");
    if (!boss) { return 1.0f; }

    Unit* guardian = AI_VALUE2(Unit*, "find target", "ahn'kahar guardian");
    if (guardian)
    {
        if (dynamic_cast<DpsAssistAction*>(action))
        {
            return 0.0f;
        }
    }
    return 1.0f;
}

float JedogaShadowseekerMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "jedoga shadowseeker");
    if (!boss) { return 1.0f; }

    Unit* volunteer = nullptr;
    // Target is not findable from threat table using AI_VALUE2(),
    // therefore need to search manually for the unit name
    GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

    for (auto i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_TWILIGHT_VOLUNTEER)
        {
            volunteer = unit;
            break;
        }
    }

    if (volunteer)
    {
        if (dynamic_cast<DpsAssistAction*>(action))
        {
            return 0.0f;
        }
    }
    return 1.0f;
}

float JedogaRoomPullMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != AKJedogaRoom::MAP_AHNKAHET || bot->IsInCombat() ||
        !dynamic_cast<AggressiveTargetAction*>(action) || !AKJedogaRoom::OwnsPull(botAI))
        return 1.0f;

    if (WlkTacticLog::Allow(bot, "ak jedoga room owns pull"))
        LOG_INFO("playerbots", "[ak jedoga room owns pull] {}: the planner waits, {} Twilight Initiates are pulled "
                 "one at a time by the altar tactic", bot->GetName(), AKJedogaRoom::Get(botAI).initiatesAlive);
    return 0.0f;
}

float ForgottenOneMultiplier::GetValue(Action* action)
{
    Unit* unit = AI_VALUE2(Unit*, "find target", "forgotten one");
    if (!unit) { return 1.0f; }

    if (bot->isMoving())
    {
        if (dynamic_cast<MovementAction*>(action))
        {
            return 0.0f;
        }
    }
    return 1.0f;
}

float VolazjInsanityMultiplier::GetValue(Action* action)
{
    if (!dynamic_cast<AttackAction*>(action) || dynamic_cast<VolazjAttackVisageAction*>(action) ||
        !AKShared::IsInInsanity(bot))
        return 1.0f;

    Unit* target = action->GetTarget();
    if (!target)
        return 1.0f;

    return target->GetEntry() == AKIds::NPC_TWISTED_VISAGE && target->InSamePhase(bot) ? 1.0f : 0.0f;
}

namespace
{
// [b33_ok] AoE damage actions whose getThreatType() is not Aoe (melee and tank AoE, DK disease spread).
bool IsUntypedAoe(std::string const& name)
{
    static std::unordered_set<std::string> const names = {
        "blood boil", "pestilence", "death and decay", "howling blast", "corpse explosion", "whirlwind", "cleave",
        "bladestorm", "thunder clap", "shockwave", "sweeping strikes", "consecration", "hammer of the righteous",
        "divine storm", "holy wrath", "swipe", "swipe (bear)", "swipe (cat)", "starfall", "hurricane",
        "fan of knives", "blade flurry", "multi-shot", "volley", "explosive trap", "flamestrike", "blizzard",
        "arcane explosion", "blast wave", "dragon's breath", "cone of cold", "frost nova", "mind sear",
        "chain lightning", "fire nova", "magma totem", "rain of fire", "hellfire", "seed of corruption",
        "shadowfury", "avenger's shield"};
    return names.count(name) > 0;
}
}  // namespace

float AmanitarMushroomMultiplier::GetValue(Action* action)
{
    if (bot->GetMapId() != AKAmanitar::MAP_AHNKAHET || !bot->IsInCombat())
        return 1.0f;

    AKAmanitar::Snapshot const& snapshot = AKAmanitar::Get(botAI);
    if (!snapshot.active || dynamic_cast<AmanitarPotentFungusAction*>(action) ||
        dynamic_cast<CastHealingSpellAction*>(action))
        return 1.0f;

    // The bot stands at its Healthy Mushroom: no switch back to the boss or the add before the swing lands.
    if (dynamic_cast<AttackAction*>(action) && AKAmanitar::NeedsFungus(bot))
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->IsAlive() && target->GetEntry() == AKAmanitar::NPC_HEALTHY_MUSHROOM &&
            bot->GetExactDist2d(target) <= AKAmanitar::FUNGUS_KILL_DISTANCE + 1.0f && action->GetTarget() != target)
            return 0.0f;
    }

    if (snapshot.healthy.empty() && snapshot.poisonous.empty())
        return 1.0f;

    if (action->getThreatType() != Action::ActionThreatType::Aoe && !IsUntypedAoe(action->getName()))
        return 1.0f;

    if (WlkTacticLog::Allow(bot, "ak amanitar no aoe"))
        LOG_INFO("playerbots", "[ak amanitar no aoe] {}: {} held, {} Poisonous / {} Healthy Mushrooms round Amanitar",
                 bot->GetName(), action->getName(), snapshot.poisonous.size(), snapshot.healthy.size());
    return 0.0f;
}
