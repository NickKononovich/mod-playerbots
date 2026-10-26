/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTACTION_H
#define PLAYERBOTS_QUESTACTION_H

#include "Action.h"
#include "MovementActions.h"
#include "Object.h"
#include "QuestDef.h"

class GameObject;
class ObjectGuid;
class Quest;
class Player;
class PlayerbotAI;
class WorldObject;
class Object;

class QuestAction : public Action
{
public:
    QuestAction(PlayerbotAI* botAI, std::string const name) : Action(botAI, name) { }
    bool Execute(Event event) override;
    // Shared quest-accept path (guards, packet, sync fallback, broadcast); also used for quest-starting items
    bool AcceptQuest(Quest const* quest, ObjectGuid questGiver);
    // Level band below the bot's level at which a quest counts as grey (shared with QuestValues).
    static constexpr int32 GREY_QUEST_LEVEL_BAND = 10;
    // Whether the bot can and should accept this quest: core eligibility (level/race/class/rep/chain/...)
    // plus the module's "no grey quests" level band (same as QuestValues.cpp). Null-safe.
    // Used for quest-starting items (selection, use and item valuation) only; the NPC/GO quest-giver
    // path and player-driven sharing keep the core's CanTakeQuest + dialog-status behaviour.
    static bool CanAcceptQuest(Player* bot, Quest const* quest);

protected:
    virtual bool ProcessQuest(Quest const* quest, Object* questGiver) = 0;
    bool ProcessQuests(ObjectGuid questGiver);
    bool ProcessQuests(WorldObject* questGiver);
    bool IsImmediateTurnIn(Quest const* quest, Object* questGiver);
};

// A group member (the master, the selfbot leader) used a game object: when the bot needs that kind of object for
// a quest of its own, it walks to the nearest one and uses it too (#2725). Chests are left to the loot strategy.
class UseQuestGameObjectAction : public MovementAction
{
public:
    UseQuestGameObjectAction(PlayerbotAI* botAI) : MovementAction(botAI, "use quest game object") {}
    bool Execute(Event event) override;

private:
    GameObject* FindUsableCopy(uint32 entry);
    bool NeedsObject(GameObject* go);

    static constexpr float SEARCH_RANGE = 40.0f;
    static constexpr uint32 PENDING_TIMEOUT_MS = MINUTE * IN_MILLISECONDS;
};

class QuestUpdateCompleteAction : public Action
{
public:
    QuestUpdateCompleteAction(PlayerbotAI* botAI) : Action(botAI, "quest update complete") {}
    bool Execute(Event event) override;
};

class QuestUpdateAddKillAction : public Action
{
public:
    QuestUpdateAddKillAction(PlayerbotAI* botAI) : Action(botAI, "quest update add kill") {}
    bool Execute(Event event) override;
};

class QuestUpdateAddItemAction : public Action
{
public:
    QuestUpdateAddItemAction(PlayerbotAI* botAI) : Action(botAI, "quest update add item") {}
    bool Execute(Event event) override;
};

class QuestUpdateFailedAction : public Action
{
public:
    QuestUpdateFailedAction(PlayerbotAI* botAI) : Action(botAI, "quest update failed") {}
    bool Execute(Event event) override;
};

class QuestUpdateFailedTimerAction : public Action
{
public:
    QuestUpdateFailedTimerAction(PlayerbotAI* botAI) : Action(botAI, "quest update failed timer") {}
    bool Execute(Event event) override;
};

class QuestItemPushResultAction : public Action
{
public:
    QuestItemPushResultAction(PlayerbotAI* botAI) : Action(botAI, "quest item push result") {}
    bool Execute(Event event) override;
};

#endif
