/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DTKStrategy.h"
#include "DTKMultipliers.h"
#include "DTKTriggers.h"
#include "DungeonHazard.h"
#include "Playerbots.h"
#include "StringFormat.h"

void WotlkDungeonDTKStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Trollgore
    triggers.push_back(new TriggerNode("trollgore out of room",
        { NextAction("trollgore hold room", ACTION_MOVE + 6) }));
    triggers.push_back(new TriggerNode("corpse explode",
        { NextAction("corpse explode spread", ACTION_MOVE + 5) }));

    // Novos the Summoner
    // TODO: Can be improved - it's a pretty easy fight but complex to program, revisit if needed
    // Keyed to the bot's own field aura: the old "arcane field" test (a current spell) never fired, and a healer
    // casting at ACTION_EMERGENCY outranked an ACTION_MOVE escape anyway (wave 6 death).
    triggers.push_back(new TriggerNode("in arcane field",
        { NextAction("avoid arcane field", ACTION_EMERGENCY + 5) }));
    triggers.push_back(new TriggerNode("arcane field",
        { NextAction("novos positioning", ACTION_MOVE + 4) }));
    triggers.push_back(new TriggerNode("arcane field",
        { NextAction("novos target priority", ACTION_NORMAL + 1) }));
    triggers.push_back(new TriggerNode("novos summon target targeted",
        { NextAction("novos drop summon target", ACTION_RAID + 5) }));
    // He never aggroes on sight: the tank opens once the group is with it.
    triggers.push_back(new TriggerNode("novos pull",
        { NextAction("novos pull", ACTION_RAID + 1) }));

    // King Dred: Tremor Totem / Fear Ward / Berserker Rage around Bellowing Roar (67 / 34 %), Grievous Bite healed
    // off at full; the heroic Raptor Call raptors die first (AppendTargetExclusions).
    triggers.push_back(new TriggerNode("king dred fear guard",
        { NextAction("king dred fear guard", ACTION_RAID + 6) }));
    triggers.push_back(new TriggerNode("king dred grievous bite",
        { NextAction("king dred grievous bite heal", ACTION_LIGHT_HEAL + 6) }));

    //The Prophet Tharon'ja
    triggers.push_back(new TriggerNode("gift of tharon'ja",
        { NextAction("touch of life", ACTION_NORMAL + 5) }));
    triggers.push_back(new TriggerNode("gift of tharon'ja",
        { NextAction("bone armor", ACTION_NORMAL + 4) }));
    // Run ranged chars (who would normally stand at range) into melee, to dps in skeleton form
    triggers.push_back(new TriggerNode("tharon'ja out of melee",
        { NextAction("reach melee", ACTION_NORMAL + 3) }));
    triggers.push_back(new TriggerNode("gift of tharon'ja",
        { NextAction("tharonja taunt", ACTION_NORMAL + 2) }));
    triggers.push_back(new TriggerNode("gift of tharon'ja",
        { NextAction("slaying strike", ACTION_NORMAL + 2) }));
    // [boss_puddles] Tharon'ja's Poison Cloud 49548 / 59969 (8 yd, 10 s)
    // (DungeonHazard.cpp, BOSS_PUDDLES.md): out of it the short way, and the hold keeps everyone from walking
    // back in while the tank drags the boss out (BossKiteRooms).
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void WotlkDungeonDTKStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new NovosMultiplier(botAI));
    multipliers.push_back(new TharonjaMultiplier(botAI));
    multipliers.push_back(new KingDredMultiplier(botAI));
    multipliers.push_back(new DTKTankOpensMultiplier(botAI));
    multipliers.push_back(new TrollgoreBodyPullMultiplier(botAI));
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}

void WotlkDungeonDTKStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != DTKIds::MAP_ID)
        return;

    AiObjectContext* context = botAI->GetAiObjectContext();
    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    for (ObjectGuid const guid : attackers)
        if (Unit* unit = botAI->GetUnit(guid); unit && DTKIds::IsNovosNeverTarget(unit->GetEntry()))
            exclusions.insert(guid);

    // King Dred: a raptor his Raptor Call sent dies before him, so a damage dealer's "dps target" is the raptor.
    if (type != TargetValueExclusionType::DpsTarget || PlayerbotAI::IsTank(bot) || !DTKShared::KingDred(botAI))
        return;

    Unit* raptor = nullptr;
    for (ObjectGuid const guid : attackers)
    {
        if (!DTKIds::IsDredRaptor(guid.GetEntry()))
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && DTKShared::EngagedWithGroup(bot, unit) &&
            (!raptor || bot->GetExactDist(unit) < bot->GetExactDist(raptor)))
            raptor = unit;
    }
    if (!raptor)
        return;

    for (ObjectGuid const guid : attackers)
        if (!DTKIds::IsDredRaptor(guid.GetEntry()))
            exclusions.insert(guid);

    DTKShared::Log(bot, "dtk dred raptor", raptor->GetGUID().ToString(),
                   Acore::StringFormat("{} ({}) first, {:.0f} % hp, {:.1f} yd; king dred held as dps target",
                                       raptor->GetName(), raptor->GetEntry(), raptor->GetHealthPct(),
                                       bot->GetExactDist(raptor)),
                   10 * IN_MILLISECONDS);
}
