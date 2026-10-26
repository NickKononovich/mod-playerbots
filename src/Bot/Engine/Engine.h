/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ENGINE_H
#define PLAYERBOTS_ENGINE_H

#include "Multiplier.h"
#include "PlayerbotAIAware.h"
#include "Queue.h"
#include "Strategy.h"
#include "Trigger.h"
#include <map>

class Action;
class ActionNode;
class AiObjectContext;
class Event;
class NextAction;
class PlayerbotAI;

enum ActionResult
{
    ACTION_RESULT_UNKNOWN,
    ACTION_RESULT_OK,
    ACTION_RESULT_IMPOSSIBLE,
    ACTION_RESULT_USELESS,
    ACTION_RESULT_FAILED
};

// Test harness diagnostics (opt-in, only while AiObjectContext::intentWatchUntilMs is set): what the engine did with
// each action of its last tick. Recorded on the bot's own map thread; the sink (Engine::intentSink) reads it there.
enum IntentOutcome : uint8
{
    INTENT_PENDING,     // pushed, never popped this tick (outranked by the executed action, or the tick ran out)
    INTENT_UNKNOWN,     // no such action
    INTENT_USELESS,     // isUseful() false
    INTENT_MULTIPLIER,  // a multiplier scaled the relevance to 0
    INTENT_IMPOSSIBLE,  // isPossible() false
    INTENT_PREREQ,      // its prerequisites were pushed first
    INTENT_FAILED,      // Execute() returned false
    INTENT_EXECUTED,
    INTENT_MINIMAL      // minimal tick (AllowActivity false): relevance below 100 is skipped
};

struct IntentCandidate
{
    Action* action = nullptr;  // owned by the AiObjectContext, valid after the tick
    std::string name;
    std::string by;            // "trigger <name> / <strategy>", "default / <strategy>", "alt of <action>", ...
    float relevance = 0.0f;    // as pushed, then after the multipliers once evaluated
    uint8 outcome = INTENT_PENDING;
    std::string multiplier;    // INTENT_MULTIPLIER: which one
    float multiplierValue = 1.0f;
};

struct IntentExecuted
{
    std::string name;
    std::string by;
    std::string target;
    float relevance = 0.0f;
    float targetDistance = -1.0f;
    uint32 ms = 0;  // getMSTime of the execution, 0 = none yet
};

class ActionExecutionListener
{
public:
    virtual ~ActionExecutionListener(){};

    virtual bool Before(Action* action, Event event) = 0;
    virtual bool AllowExecution(Action* action, Event event) = 0;
    virtual void After(Action* action, bool executed, Event event) = 0;
    virtual bool OverrideResult(Action* action, bool executed, Event event) = 0;
};

class ActionExecutionListeners : public ActionExecutionListener
{
public:
    virtual ~ActionExecutionListeners();

    bool Before(Action* action, Event event) override;
    bool AllowExecution(Action* action, Event event) override;
    void After(Action* action, bool executed, Event event) override;
    bool OverrideResult(Action* action, bool executed, Event event) override;

    void Add(ActionExecutionListener* listener) { listeners.push_back(listener); }

    void Remove(ActionExecutionListener* listener) { listeners.remove(listener); }

private:
    std::list<ActionExecutionListener*> listeners;
};

class Engine : public PlayerbotAIAware
{
public:
    Engine(PlayerbotAI* botAI, AiObjectContext* factory);

    virtual void Init();
    void addStrategy(std::string const name, bool init = true);
    void addStrategies(std::string first, ...);
    void addStrategiesNoInit(std::string first, ...);
    bool removeStrategy(std::string const name, bool init = true);
    bool HasStrategy(std::string const name);
    Strategy* GetStrategy(std::string const name);
    void removeAllStrategies();
    void toggleStrategy(std::string const name);
    std::string const ListStrategies();
    std::vector<std::string> GetStrategies();
    bool ContainsStrategy(StrategyType type);
    void ChangeStrategy(std::string const names);
    std::string const GetLastAction() { return lastAction; }

    virtual bool DoNextAction(Unit*, uint32 depth = 0, bool minimal = false);
    ActionResult ExecuteAction(std::string const name, Event event = Event(), std::string const qualifier = "");

    void AddActionExecutionListener(ActionExecutionListener* listener) { actionExecutionListeners.Add(listener); }

    void removeActionExecutionListener(ActionExecutionListener* listener) { actionExecutionListeners.Remove(listener); }
    bool HasStrategyType(StrategyType type) { return strategyTypeMask & type; }
    bool HasTargetExclusions() const { return hasTargetExclusions; }
    std::vector<Multiplier*> const& GetMultipliers() const { return multipliers; }
    virtual ~Engine(void);

    bool testMode;

    // Intention diagnostics (see IntentOutcome). The sink is called at the end of a watched tick, at most every
    // INTENT_SINK_MS, on the bot's thread; nullptr = nothing records.
    typedef void (*IntentSink)(PlayerbotAI* botAI, Engine* engine);
    static IntentSink intentSink;
    std::vector<IntentCandidate> const& GetIntentCandidates() const { return intentCandidates; }
    IntentExecuted const& GetIntentExecuted() const { return intentExecuted; }
    std::string const& GetIntentMover() const { return intentMover; }
    uint32 GetIntentMoverMs() const { return intentMoverMs; }
    uint32 GetIntentTickMs() const { return intentTickMs; }
    bool WasIntentTickMinimal() const { return intentMinimal; }
    bool DidIntentTickExecute() const { return intentTickExecuted; }

private:
    std::string const ResolveStrategyName(std::string const name);
    void PushDefaultActions();
    ActionNode* CreateActionNode(std::string const name);

    void LogValues();

    ActionExecutionListeners actionExecutionListeners;

protected:
    void LogAction(char const* format, ...);
    bool MultiplyAndPush(std::vector<NextAction> actions, float forceRelevance, bool skipPrerequisites, Event event,
                         char const* pushType);
    void Reset();
    void ProcessTriggers(bool minimal);
    void PushAgain(ActionNode* actionNode, float relevance, Event event);
    virtual Action* InitializeAction(ActionNode* actionNode);
    virtual bool ListenAndExecute(Action* action, Event event);

    Queue queue;
    std::vector<TriggerNode*> triggers;
    std::vector<Multiplier*> multipliers;
    AiObjectContext* aiObjectContext;
    std::map<std::string, Strategy*> strategies;
    float lastRelevance;
    std::string lastAction;
    uint32 strategyTypeMask;
    bool hasTargetExclusions = false;
    NamedObjectFactoryList<ActionNode> actionNodeFactories;

private:
    // [combat_no_damage_release] a multiplier's veto on this attack is lifted (GroupDamage::NoDamageRelease)
    bool IsNoDamageReleased(Action* action);
    bool IsIntentWatched();
    void IntentPush(std::string const& name, float relevance);
    IntentCandidate* IntentFind(std::string const& name);
    void IntentMark(std::string const& name, uint8 outcome, float relevance, Action* action);
    void IntentMarkMultiplier(Action* action, float relevance, std::string const& multiplier, float value);
    std::string IntentTriggerOwner(size_t triggerIndex);

    bool intentTick = false;  // the current DoNextAction records (watched bot)
    bool intentMinimal = false;
    bool intentTickExecuted = false;
    std::string intentPushBy;  // who pushes the actions MultiplyAndPush is handed now
    std::vector<IntentCandidate> intentCandidates;
    IntentExecuted intentExecuted;
    std::string intentMover;  // the last action that started a new movement spline
    uint32 intentMoverMs = 0;
    uint32 intentTickMs = 0;
    uint32 intentSinkMs = 0;
    std::vector<std::pair<size_t, Strategy*>> triggerOwners;  // end index into `triggers` of each strategy's nodes
};

#endif
