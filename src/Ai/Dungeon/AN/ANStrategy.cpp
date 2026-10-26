/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ANStrategy.h"
#include "ANAddTactics.h"
#include "ANMultipliers.h"
#include "ANTriggers.h"
#include "DungeonHazard.h"
#include "AiObjectContext.h"
#include "InstanceScript.h"
#include "Playerbots.h"

void WotlkDungeonANStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Krik'thir the Gatewatcher
    // TODO: Add CC trigger while web wraps are casting?
    // TODO: Bring healer closer than ranged dps to avoid fixates?
    triggers.push_back(new TriggerNode("krik'thir web wrap",
        { NextAction("attack web wrap", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("krik'thir watchers",
        { NextAction("krik'thir priority", ACTION_RAID + 4) }));
    // Watcher packs: the Skirmisher's Fixate makes its random target taunt it, then it Backstabs that target from
    // behind; the fixated bot turns to face it and stops stepping away (SkirmisherFixateHoldMultiplier).
    triggers.push_back(new TriggerNode("anub'ar skirmisher fixate",
        { NextAction("face fixating skirmisher", ACTION_RAID + 6) }));
    // The Watcher room is Krik'thir's timed encounter (ANWatcherRoom, ANTriggers.h): the tank pulls one whole pack to
    // the rally at the foot of the ramp once the group is topped up, Krik'thir sends the rest there 10 s after each
    // pack dies, and nobody stands in reach of a pack that is not in the fight yet.
    triggers.push_back(new TriggerNode("krik'thir watcher pull",
        { NextAction("krik'thir watcher pull", 4.5f) }));
    triggers.push_back(new TriggerNode("krik'thir watcher drift",
        { NextAction("krik'thir back to rally", ACTION_MOVE + 7) }));
    // [b34_an2] The whole phase is held at the rally: out of combat everyone walks back to it (between sends, before
    // resting - above food and drink - and after each pack), and the rally is the group's event hold, so neither the
    // harness walk nor a LoS pull takes the group off it (an-n 20261026-014521-an-n-5G-25: Narjil's pack came to the
    // group 28 yd from Gashra's idle pack and both fought it).
    triggers.push_back(new TriggerNode("krik'thir watcher rally",
        { NextAction("krik'thir event rally", ACTION_MOVE + 6) }));

    // Hadronox
    // Hold her platform while the encounter runs: the Crusher packs come up to it, she follows once all three are
    // dead and webs the doors there (boss_hadronox.cpp). Below it the door swarm never ends.
    triggers.push_back(new TriggerNode("hadronox platform",
        { NextAction("hadronox return to platform", ACTION_MOVE + 6) }));
    // The three Crushers gate her climb and the door webbing (boss_hadronox.cpp _crushersLeft): kill them first.
    triggers.push_back(new TriggerNode("hadronox crusher",
        { NextAction("hadronox attack crusher", ACTION_RAID + 3) }));
    // Then Hadronox herself, once she stands on her web spot with the doors webbed (ANAdds::HadronoxHandsOff): a hit
    // on her before that stops her climb and the door swarm never ends.
    triggers.push_back(new TriggerNode("hadronox boss",
        { NextAction("hadronox attack boss", ACTION_RAID + 2) }));
    // [b33_hadronox2] Crushers dead, doors not webbed, nobody on her list: out of Web Grab's 60 yd of her and of her
    // web spot (that platform is the spot itself) until "[an hadronox webbed]"; above the attack and focus steps,
    // below the hazard dodges. Then the ranged and the healer stand outside her Leech Poison (the adds on them die
    // there, not healing her).
    triggers.push_back(new TriggerNode("hadronox hold back",
        { NextAction("hadronox hold back", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("hadronox leech standoff",
        { NextAction("hadronox leech standoff", ACTION_MOVE + 6) }));
    // [boss_puddles] Hadronox's Acid Cloud 53400 / 59419 (5 yd, 90 s) piles up on her platform
    // (DungeonHazard.cpp, BOSS_PUDDLES.md): out of it the short way, and the hold keeps everyone from walking
    // back in while the tank drags the boss out (BossKiteRooms).
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
    // After her the way on is a drop through the hole in the shaft floor (separate navmesh below).
    triggers.push_back(new TriggerNode("azjol-nerub lower kingdom",
        { NextAction("drop to lower kingdom", ACTION_MOVE + 2) }));

    //Anub'arak
    // Impale: an Impale Target NPC appears under a player and erupts 4 s later.
    triggers.push_back(new TriggerNode("anub'arak impale",
        { NextAction("avoid anub'arak impale", ACTION_MOVE + 6) }));
    triggers.push_back(new TriggerNode("anub'arak pound",
        { NextAction("dodge pound", ACTION_MOVE + 5) }));
    // Pound is a frontal cone: every non-tank keeps to his side all fight.
    triggers.push_back(new TriggerNode("anub'arak frontal",
        { NextAction("anub'arak flank", ACTION_MOVE + 4) }));
}

void WotlkDungeonANStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new KrikthirMultiplier(botAI));
    multipliers.push_back(new KrikthirWatcherRoomMultiplier(botAI));
    multipliers.push_back(new SkirmisherFixateHoldMultiplier(botAI));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "hadronox crusher focus", { ANAdds::NPC_ANUBAR_CRUSHER }));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "hadronox boss focus", { ANIds::NPC_HADRONOX }));
    // Not before her MOVE3 web spot: a hit on her while she climbs cancels the door webbing (boss_hadronox.cpp).
    multipliers.push_back(new HadronoxHandsOffMultiplier(botAI));
    // [b33_hadronox] once she is fought: no damage dealer's area spell on the adds in her Leech Poison (each one dying
    // there heals her 10 %, ANAddTactics.h)
    multipliers.push_back(new HadronoxLeechAoeMultiplier(botAI));
    multipliers.push_back(new HadronoxHoldBackMultiplier(botAI));  // [b33_hadronox2]
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}

void WotlkDungeonANStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType /*type*/)
{
    // Cheap exit outside the Hadronox encounter before touching the attackers.
    Player* bot = botAI->GetBot();
    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(ANIds::DATA_HADRONOX) != IN_PROGRESS)
        return;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && ANIds::IsBelowHadronoxPlatform(bot, unit))
            exclusions.insert(guid);
    }

    // Nobody picks her, the tank included, until she has webbed the doors (ANAdds::HadronoxHandsOff).
    if (ANAdds::HadronoxHandsOff(botAI))
        exclusions.insert(ANAdds::GetHadronoxGate(botAI).boss);
}
