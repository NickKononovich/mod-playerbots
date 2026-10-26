/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UPStrategy.h"
#include "ChooseTargetActions.h"
#include "DungeonHazard.h"
#include "Playerbots.h"
#include "UPMultipliers.h"
#include "UPTriggers.h"

void WotlkDungeonUPStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Svala Sorrowgrave: the intro starts from area trigger 5140; Ritual of the Sword - kill the channelers
    triggers.push_back(new TriggerNode("svala start ready",
        { NextAction("svala start event", ACTION_MOVE + 4) }));
    // Ritual of the Sword: every damage dealer switches to the channelers at once (they stand clustered, AoE is
    // fine); the sacrifice dies when the 25 s run out.
    triggers.push_back(new TriggerNode("svala ritual",
        { NextAction("svala attack channeler", ACTION_RAID + 6) }));

    // Gortok Palehoof: the Stasis Generator starts the event
    triggers.push_back(new TriggerNode("gortok start ready",
        { NextAction("gortok start event", ACTION_MOVE + 4) }));

    // Skadi the Ruthless: area trigger 4991 starts the gauntlet; Harpooners drop harpoons for the launchers
    triggers.push_back(new TriggerNode("skadi start ready",
        { NextAction("skadi start event", ACTION_MOVE + 4) }));
    triggers.push_back(new TriggerNode("skadi gauntlet",
        { NextAction("skadi move to harpoons", ACTION_RAID) }));
    triggers.push_back(new TriggerNode("skadi harpoon on ground",
        { NextAction("skadi pick up harpoon", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("skadi harpoon in hand",
        { NextAction("skadi fire harpoon", ACTION_RAID + 2) }));
    triggers.push_back(new TriggerNode("skadi harpooner",
        { NextAction("skadi attack harpooner", ACTION_RAID + 1) }));
    // Grauf's breath covers one half of the lane: cross to the other one (wave 9: the staging spot sits on the
    // split line and a shaman died to the clouds there).
    triggers.push_back(new TriggerNode("skadi breath side",
        { NextAction("skadi cross lane", ACTION_EMERGENCY + 3) }));
    // ... and whoever still stands in a cloud left behind steps out of it (DungeonHazard.cpp)
    triggers.push_back(new TriggerNode("dungeon hazard",
        { NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 2) }));
    triggers.push_back(new TriggerNode("freezing cloud",
        { NextAction("avoid freezing cloud", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("skadi whirlwind",
        { NextAction("avoid skadi whirlwind", ACTION_RAID + 4) }));

    // King Ymiron
    // May need to avoid orb.. unclear if the generic avoid AoE does this well
    // [b36_ymiron] was "drop target" on the aura alone: too late (three DPS died to the reflect in the same
    // second, run 33) and the dropped target blocked interrupts. Now from the cast on, keeping the target;
    // YmironMultiplier holds every damaging action on him meanwhile.
    triggers.push_back(new TriggerNode("ymiron bane",
        { NextAction("ymiron hold damage", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("ymiron bane purge",
        { NextAction("ymiron purge bane", ACTION_RAID + 6) }));
}

void WotlkDungeonUPStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new SkadiMultiplier(botAI));
    multipliers.push_back(new YmironMultiplier(botAI));
}

void WotlkDungeonUPStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (botAI->GetBot()->GetMapId() != UPIds::MAP_ID)
        return;

    // Svala hangs out of reach over the altar while the Ritual Channelers hold the sacrifice
    AiObjectContext* context = botAI->GetAiObjectContext();
    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    ObjectGuid svala;
    bool ritual = false;
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;

        if (unit->GetEntry() == UPIds::NPC_SVALA_SORROWGRAVE)
            svala = guid;
        else if (unit->GetEntry() == UPIds::NPC_RITUAL_CHANNELER && unit->IsAlive())
            ritual = true;
    }

    if (ritual && !svala.IsEmpty())
        exclusions.insert(svala);

    // A damage dealer's "dps target" is the Harpooner that "skadi attack harpooner" (61) picks, so "dps assist" (50)
    // keeps it instead of switching back every tick ("[idle diag] ... dps assist:N skadi attack harpooner:N").
    Player* bot = botAI->GetBot();
    if (type != TargetValueExclusionType::DpsTarget || !PlayerbotAI::IsDps(bot) || PlayerbotAI::IsTank(bot) ||
        PlayerbotAI::IsHeal(bot) || bot->HasItemCount(UPIds::ITEM_HARPOON) || !UPEvent::IsGauntletRunning(bot))
        return;

    if (Unit* harpooner = UPEvent::FindStagingHarpooner(botAI))
        for (ObjectGuid const guid : attackers)
            if (guid != harpooner->GetGUID())
                exclusions.insert(guid);
}
