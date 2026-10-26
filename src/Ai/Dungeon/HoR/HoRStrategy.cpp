/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoRStrategy.h"
#include "DungeonTactics.h"
#include "HoRMultipliers.h"
#include "HoRTriggers.h"
#include "InstanceScript.h"
#include "Playerbots.h"

#include <algorithm>

void WotlkDungeonHoRStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Part 1: start gossip, then the ten waves at the Frostmourne altar.
    triggers.push_back(new TriggerNode("hor start intro", { NextAction("hor start intro", ACTION_RAID + 1) }));
    triggers.push_back(
        new TriggerNode("hor wave position", { NextAction("hor move to wave position", ACTION_RAID + 5) }));
    triggers.push_back(
        new TriggerNode("hor wave priority", { NextAction("hor attack wave priority", ACTION_RAID + 1) }));
    // The last one alive leaves the waves' range so they reset and the released ones can come back in.
    triggers.push_back(
        new TriggerNode("hor wave lost", { NextAction("hor leave wave circle", ACTION_EMERGENCY + 2) }));
    // Phantom Mage Fireball / Frostbolt / Flamestrike are kicked whenever someone can (wave 10: 6.4k on the tank).
    triggers.push_back(new TriggerNode("dungeon priority interrupt",
                                       { NextAction("dungeon priority interrupt", ACTION_INTERRUPT + 5) }));
    // Falric: Defiling Horror (every 20 s, 200 yd, Horror: no Tremor Totem or Fear Ward against it) lands on a group
    // at full health.
    triggers.push_back(new TriggerNode("dungeon top up", { NextAction("dungeon top up", ACTION_LIGHT_HEAL + 6) }));
    // Impending Despair (stun after 6 s) and Quivering Strike are Magic: dispelled before any heal.
    triggers.push_back(new TriggerNode("hor falric dispel", { NextAction("hor falric dispel", ACTION_DISPEL + 2) }));
    // Falric and Marwyn deal shadow damage to the whole group: Shadow Resistance Aura (HoRShadowAuraMultiplier keeps
    // Devotion Aura from swapping it back).
    triggers.push_back(new TriggerNode("hor shadow aura", { NextAction("hor shadow aura", ACTION_RAID + 3) }));

    // Frostsworn General: fought in his corridor (he evades past 30 yd from his home); his Spiritual Reflections are
    // killed first by the damage dealers once they are attackable.
    triggers.push_back(
        new TriggerNode("hor general leash", { NextAction("hor general leash", ACTION_EMERGENCY + 1) }));
    triggers.push_back(new TriggerNode("hor spiritual reflection",
                                       { NextAction("hor attack reflection", ACTION_RAID + 1) }));

    // Part 2: Shadow Throne area trigger, then the escape from the Lich King.
    triggers.push_back(
        new TriggerNode("hor lich king intro", { NextAction("hor enter throne room", ACTION_RAID + 1) }));
    triggers.push_back(
        new TriggerNode("hor escape gather", { NextAction("hor move to escape leader", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("hor start escape", { NextAction("hor start escape", ACTION_RAID + 2) }));
    triggers.push_back(
        new TriggerNode("hor escape position", { NextAction("hor escape move to leader", ACTION_EMERGENCY + 1) }));
    triggers.push_back(
        new TriggerNode("hor escape priority", { NextAction("hor attack escape priority", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("hor lich king targeted",
                                       { NextAction("hor stop attacking lich king", ACTION_EMERGENCY + 5) }));
    // Each summon starts on the player nearest to it with 1000 threat: the tank waits on the Lich King's side of the
    // group, takes back any summon on somebody else, and the healer keeps behind the tank at the leader.
    triggers.push_back(
        new TriggerNode("hor escape loose mob", { NextAction("hor escape taunt loose mob", ACTION_RAID + 3) }));
    triggers.push_back(
        new TriggerNode("hor escape tank position", { NextAction("hor escape tank position", ACTION_RAID + 2) }));
    triggers.push_back(
        new TriggerNode("hor escape healer hold", { NextAction("hor escape healer hold", ACTION_MOVE + 4) }));
    // Lumbering Abomination: Cleave + Vomit Spray in front, everybody but the tank behind or beside it.
    triggers.push_back(new TriggerNode("dungeon frontal cleave",
                                       { NextAction("dungeon avoid frontal cleave", ACTION_EMERGENCY + 3) }));
}

void WotlkDungeonHoRStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new HoRSharedSufferingMultiplier(botAI));
    multipliers.push_back(new HoRWaveHoldMultiplier(botAI));
    multipliers.push_back(new HoRShadowAuraMultiplier(botAI));
    multipliers.push_back(new HoRFalricHeroismMultiplier(botAI));
    multipliers.push_back(new HoRFrostswornGeneralMultiplier(botAI));
    multipliers.push_back(new HoREscapeMeetAtLeaderMultiplier(botAI));
    multipliers.push_back(new DungeonFrontalCleaveMultiplier(botAI));
}

void WotlkDungeonHoRStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = HoRHelpers::GetInstance(bot);
    if (!instance)
        return;

    // A damage dealer's "dps target" is the best rank of the kill order while one of it is in the fight, so
    // "dps assist" (50) keeps the target that "hor attack wave/escape priority" (61) chose instead of switching it
    // back every tick (spirit waves: a shaman, a mage and a hunter cast 0-2 times in 15 s,
    // "[idle diag] ... actions dps assist:21 hor attack wave priority:21").
    if (type == TargetValueExclusionType::DpsTarget && !PlayerbotAI::IsTank(bot))
    {
        for (HoRHelpers::PriorityList const list : { HoRHelpers::PriorityList::Wave, HoRHelpers::PriorityList::Escape })
        {
            if (!HoRHelpers::IsPriorityListActive(bot, list))
                continue;

            std::vector<Unit*> const rank = HoRHelpers::GetPriorityRank(botAI, list);
            if (rank.empty())
                continue;

            GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
            for (ObjectGuid const& guid : attackers)
                if (std::none_of(rank.begin(), rank.end(), [&guid](Unit* unit) { return unit->GetGUID() == guid; }))
                    exclusions.insert(guid);
            break;
        }
    }

    // Frostsworn General: while a Spiritual Reflection can be attacked, a damage dealer's "dps target" is a reflection
    // (the copies hit with Baleful Strike; "dps assist" would switch back to the General every tick).
    if (type == TargetValueExclusionType::DpsTarget && !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot))
        if (Creature* general = HoRHelpers::GetFrostswornGeneral(bot))
            if (HoRHelpers::GetAttackableReflection(botAI))
                exclusions.insert(general->GetGUID());

    // The escape Lich King heals himself back above 70% and only exists to chase the group: never a target.
    for (uint32 const dataId : { HoRIds::NPC_LICH_KING_BOSS, HoRIds::NPC_LICH_KING_EVENT })
        if (ObjectGuid const guid = instance->GetGuidData(dataId))
            exclusions.insert(guid);

    // Falric and Marwyn stand hidden and immune until their wave calls them.
    for (uint32 const dataId : { HoRIds::DATA_FALRIC, HoRIds::DATA_MARWYN })
        if (Creature* boss = HoRHelpers::GetInstanceCreature(bot, dataId))
            if (!boss->IsVisible() || boss->IsImmuneToPC())
                exclusions.insert(boss->GetGUID());
}
