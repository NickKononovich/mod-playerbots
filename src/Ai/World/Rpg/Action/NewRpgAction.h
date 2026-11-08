/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGACTION_H
#define PLAYERBOTS_NEWRPGACTION_H

#include "Duration.h"
#include "MovementActions.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"
#include <string>

class Player;

class TellRpgStatusAction : public NewRpgBaseAction
{
public:
    TellRpgStatusAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "rpg status") {}

    bool Execute(Event event) override;

private:
    static constexpr char const* RPG_STATUS_CHANGED_KEY = "rpg_status_changed";
    static constexpr char const* RPG_STATUS_CHANGED_DEFAULT = "rpg status -> %status";

    void WhisperStatusChange(Player* owner, std::string const& statusName);
};

class StartRpgDoQuestAction : public Action
{
public:
    StartRpgDoQuestAction(PlayerbotAI* botAI) : Action(botAI, "start rpg do quest") {}

    bool Execute(Event event) override;
};

class NewRpgStatusUpdateAction : public NewRpgBaseAction
{
public:
    NewRpgStatusUpdateAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg status update")
    {
        // int statusCount = RPG_STATUS_END - 1;

        // transitionMat.resize(statusCount, std::vector<int>(statusCount, 0));

        // transitionMat[RPG_IDLE][RPG_GO_GRIND] = 20;
        // transitionMat[RPG_IDLE][RPG_GO_CAMP] = 15;
        // transitionMat[RPG_IDLE][RPG_WANDER_NPC] = 30;
        // transitionMat[RPG_IDLE][RPG_DO_QUEST] = 35;
    }
    bool Execute(Event event) override;

protected:
    // static NewRpgStatusTransitionProb transitionMat;
    // [timers] the IDLE choice's re-ask cadence while REST (class b); every other status ends on its facts
    const int32 statusRestDuration = 30 * IN_MILLISECONDS ;
    static constexpr uint32 QUEST_FIRST_CHANCE = 85;  // % of idle rolls that go straight to a quest (owned bots)
};

class NewRpgWaitForGroupAction : public NewRpgBaseAction
{
public:
    NewRpgWaitForGroupAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg wait for group") {}
    bool Execute(Event event) override;

private:
    ObjectGuid _meetLogged;  // [wait14] one line per member it walks to meet
};

// Out to the nearest inn and sit there while the selfbot leader is away (rested experience).
class NewRpgRestAtInnAction : public NewRpgBaseAction
{
public:
    NewRpgRestAtInnAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg rest at inn") {}
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    static constexpr float INN_SEARCH_DISTANCE = 600.0f;  // farther inns are not worth the walk
    static constexpr float INN_REACHED = 5.0f;
};

// Restarts a stalled taxi spline; after a couple of restarts ends the flight on the ground below the bot.
class NewRpgTaxiUnstallAction : public NewRpgBaseAction
{
public:
    NewRpgTaxiUnstallAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg taxi unstall") {}
    bool Execute(Event event) override;

private:
    static constexpr uint8 MAX_RESTARTS = 2;
    static constexpr float GROUND_SEARCH = 200.0f;
};

// Talks to a flight master it passes so its flight path becomes known, as a player does; group bots next to the
// flight master learn it too.
class NewRpgDiscoverFlightMasterAction : public NewRpgBaseAction
{
public:
    NewRpgDiscoverFlightMasterAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg discover flight master")
    {
    }
    bool Execute(Event event) override;
};

class NewRpgGoGrindAction : public NewRpgBaseAction
{
public:
    NewRpgGoGrindAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go grind") {}
    bool Execute(Event event) override;
};

class NewRpgGoCampAction : public NewRpgBaseAction
{
public:
    NewRpgGoCampAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go camp") {}
    bool Execute(Event event) override;
};

class NewRpgWanderRandomAction : public NewRpgBaseAction
{
public:
    NewRpgWanderRandomAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg wander random") {}
    bool Execute(Event event) override;

private:
    Creature* NextSkinMob();  // [skin12] the nearest live skinnable mob of a skinning grind beyond its reach
};

class NewRpgWanderNpcAction : public NewRpgBaseAction
{
public:
    NewRpgWanderNpcAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg move npcs") {}
    bool Execute(Event event) override;
};

class NewRpgDoQuestAction : public NewRpgBaseAction
{
public:
    NewRpgDoQuestAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do quest") {}
    bool Execute(Event event) override;

protected:
    bool DoIncompleteQuest(NewRpgInfo::DoQuest& data);
    bool DoCompletedQuest(NewRpgInfo::DoQuest& data);
    bool HandInBlocked(NewRpgInfo::DoQuest& data);  // [guild15] why the turn-in at the ender does not happen
    bool BuyQuestVendorItem(uint32 questId);        // [guild15] an item objective a vendor in sight sells
    bool TalkToQuestNpc(uint32 questId);  // [guild15]
    // [questitems] NewRpgQuestUse.cpp: an objective credited by using a quest item or a spell on a target
    bool UseQuestTool(uint32 questId);
    bool SettleQuestUse();  // the last use's credit (logged), awaited while its cast or aura runs
    uint32 _fishLogged{0};  // [fish14] one line per quest it fishes for
};

class NewRpgTravelFlightAction : public NewRpgBaseAction
{
public:
    NewRpgTravelFlightAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel flight") {}
    bool Execute(Event event) override;

protected:
    void ContinueCrossMapTaxi();
};

#endif
