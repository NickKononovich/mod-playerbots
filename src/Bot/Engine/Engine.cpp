/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Engine.h"
#include "ReadyCheckAction.h"  // [lead154] the rebuff window's close
#include "Action.h"
#include "EstimatedLifetimeValue.h"
#include "Event.h"
#include "InstanceScript.h"
#include "LeaderOrders.h"        // [lead15] the own-decision check
#include "RaidLeaderStrategy.h"  // [lead15]
#include "MapUpdater.h"
#include "MoveSpline.h"
#include "PerfMonitor.h"
#include "Playerbots.h"
#include "Queue.h"
#include "Strategy.h"
#include "SlowUpdate.h"  // [perf212]
#include "Timer.h"
#include "Value.h"

namespace
{
    // The intention sink runs at most this often per engine (the harness reads it every 2 s).
    constexpr uint32 INTENT_SINK_MS = 1000;
    constexpr size_t INTENT_MAX_CANDIDATES = 64;
}

Engine::IntentSink Engine::intentSink = nullptr;

bool Engine::IsNoDamageReleased(Action* action)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld() || !bot->GetMap() || !bot->GetMap()->IsDungeon())
        return false;

    if (InstanceScript* instance = bot->GetInstanceScript())
        if (instance->IsEncounterInProgress())
            return false;

    if (!GroupDamage::NoDamageRelease(bot))
        return false;

    Unit* target = action->GetTarget();
    return target && target != bot && target->IsAlive() && bot->IsValidAttackTarget(target);
}

Engine::Engine(PlayerbotAI* botAI, AiObjectContext* factory) : PlayerbotAIAware(botAI), aiObjectContext(factory)
{
    lastRelevance = 0.0f;
    testMode = false;
}

bool ActionExecutionListeners::Before(Action* action, Event event)
{
    bool result = true;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        result &= (*i)->Before(action, event);
    }

    return result;
}

void ActionExecutionListeners::After(Action* action, bool executed, Event event)
{
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        (*i)->After(action, executed, event);
    }
}

bool ActionExecutionListeners::OverrideResult(Action* action, bool executed, Event event)
{
    bool result = executed;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        result = (*i)->OverrideResult(action, result, event);
    }

    return result;
}

bool ActionExecutionListeners::AllowExecution(Action* action, Event event)
{
    bool result = true;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        result &= (*i)->AllowExecution(action, event);
    }

    return result;
}

ActionExecutionListeners::~ActionExecutionListeners()
{
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i != listeners.end(); i++)
    {
        delete *i;
    }

    listeners.clear();
}

Engine::~Engine(void)
{
    Reset();

    // for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    // {
    //     Strategy* strategy = i->second;
    //     if (strategy)
    //     {
    //         delete strategy;
    //     }
    // }

    strategies.clear();
}

void Engine::Reset()
{
    strategyTypeMask = 0;

    ActionNode* action = nullptr;

    while ((action = queue.Pop()) != nullptr)
    {
        delete action;
    }

    for (TriggerNode* trigger : triggers)
    {
        delete trigger;
    }

    triggers.clear();

    for (Multiplier* multiplier : multipliers)
    {
        delete multiplier;
    }

    multipliers.clear();

    actionNodeFactories.creators.clear();
}

void Engine::Init()
{
    Reset();

    hasTargetExclusions = false;
    triggerOwners.clear();
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        Strategy* strategy = i->second;
        strategyTypeMask |= strategy->GetType();
        hasTargetExclusions |= strategy->HasTargetExclusions();
        strategy->InitMultipliers(multipliers);
        strategy->InitTriggers(triggers);
        triggerOwners.emplace_back(triggers.size(), strategy);
        for (auto &iter : strategy->actionNodeFactories.creators)
        {
            actionNodeFactories.creators[iter.first] = iter.second;
        }
    }

    if (testMode)
    {
        FILE* file = fopen("test.log", "w");
        fprintf(file, "\n");
        fclose(file);
    }
}

bool Engine::DoNextAction(Unit* /*unit*/, uint32 /*depth*/, bool minimal)
{
    LogAction("--- AI Tick ---");

    if (sPlayerbotAIConfig.LogValuesPerTick)
        LogValues();

    bool actionExecuted = false;
    ActionBasket* basket = nullptr;
    time_t currentTime = time(nullptr);

    if (!minimal)
        botAI->forceRebuff.RollBuffPendingCycle();

    intentTick = IsIntentWatched();
    if (intentTick)
    {
        intentCandidates.clear();
        intentMinimal = minimal;
        intentTickExecuted = false;
        intentTickMs = getMSTime();
    }

    // Update triggers and push default actions
    MapWorkerTrace::SetStage("triggers", nullptr);
    SlowUpdate::Stage("triggers");  // [perf212]
    ProcessTriggers(minimal);
    SlowUpdate::Stage("default actions");  // [perf212]
    PushDefaultActions();

    uint32 iterations = 0;
    uint32 iterationsPerTick = queue.Size() * (minimal ? 2 : sPlayerbotAIConfig.IterationsPerTick);

    while (++iterations <= iterationsPerTick)
    {
        basket = queue.Peek();
        if (!basket)
            break;

        float relevance = basket->getRelevance();  // for reference
        bool skipPrerequisites = basket->isSkipPrerequisites();

        if (minimal && (relevance < 100))
        {
            if (intentTick)
                IntentMark(basket->getAction()->getName(), INTENT_MINIMAL, relevance, nullptr);
            continue;
        }

        Event event = basket->getEvent();
        ActionNode* actionNode = queue.Pop();  // NOTE: Pop() deletes basket
        Action* action = InitializeAction(actionNode);
        if (intentTick)
            intentPushBy.clear();

        if (action)
            SlowUpdate::Stage("checks ", action);  // [perf212] its isUseful, multipliers, isPossible
        if (!action)
        {
            LogAction("A:%s - UNKNOWN", actionNode->getName().c_str());
            if (intentTick)
                IntentMark(actionNode->getName(), INTENT_UNKNOWN, relevance, nullptr);
        }
        else if (action->isUseful())
        {
            // Apply multipliers early to avoid unnecessary iterations
            for (Multiplier* multiplier : multipliers)
            {
                float value;
                {
                    PerfMonitorScope scope(multiplier->GetPerfData());
                    value = multiplier->GetValue(action);
                }

                // [combat_no_damage_release] A veto (a multiplier of 0) on an action aimed at an enemy the bot may
                // attack lifts while its group fights and nobody has hit anything for a while; outside an encounter
                // only (a boss' own mechanics keep their vetoes), and only attacks: movement vetoes stay.
                // [b41_mgt] ... except a veto its multiplier keeps (KeepsVeto): mgt-n 20261026-063902-mgt-n-5A-14
                // (360 s) lifted the Selin room's leash on the tank's "reach melee" to an inert Selin (her encounter
                // not in progress yet), the tank walked into her room alone and the door shut on the other four.
                if (value <= 0.0f && !multiplier->KeepsVeto(action) && IsNoDamageReleased(action))
                    value = 1.0f;

                relevance *= value;
                action->setRelevance(relevance);

                if (relevance <= 0)
                {
                    if (value <= 0)
                        multiplier->NoteBlock();

                    LogAction("Multiplier %s made action %s useless", multiplier->getName().c_str(), action->getName().c_str());
                    botAI->NoteIdleDiagRefusal(PlayerbotAI::IDLE_REFUSAL_BLOCKED);  // [grp20]
                    if (intentTick)
                        IntentMarkMultiplier(action, relevance, multiplier->getName(), value);
                    break;
                }
            }

            if (action->isPossible() && relevance > 0)
            {
                if (!skipPrerequisites)
                {
                    LogAction("A:%s - PREREQ", action->getName().c_str());

                    if (intentTick)
                        intentPushBy = "prereq of " + action->getName();
                    if (MultiplyAndPush(actionNode->getPrerequisites(), relevance + 0.002f, false, event, "prereq"))
                    {
                        // [grp20] marked where the action is actually deferred, not where PREREQ is logged: the log
                        // line prints before the push is tried, and a push that fails leaves the action to run
                        botAI->NoteIdleDiagRefusal(PlayerbotAI::IDLE_REFUSAL_PREREQ);
                        if (intentTick)
                            IntentMark(action->getName(), INTENT_PREREQ, relevance, action);
                        PushAgain(actionNode, relevance + 0.001f, event);
                        continue;
                    }
                }

                PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_ACTION, action->getName(), &aiObjectContext->performanceStack);
                Unit* const victimBefore = botAI->GetBot() ? botAI->GetBot()->GetVictim() : nullptr;  // [lead15]
                actionExecuted = ListenAndExecute(action, event);
                if (pmo)
                    pmo->finish();
                // [lead15] LEADER_OWN_DECISION_CHECK: a member's action without a matching leader command
                if (actionExecuted && Leader::LEADER_OWN_DECISION_CHECK)
                    RaidLeader::CheckOwnDecision(botAI, action, victimBefore);

                if (actionExecuted)
                {
                    LogAction("A:%s - OK", action->getName().c_str());
                    if (intentTick)
                    {
                        IntentMark(action->getName(), INTENT_EXECUTED, relevance, action);
                        intentTickExecuted = true;
                        intentPushBy = "cont of " + action->getName();
                    }
                    MultiplyAndPush(actionNode->getContinuers(), relevance, false, event, "cont");
                    lastRelevance = relevance;
                    delete actionNode;  // Safe memory management
                    break;
                }
                else
                {
                    LogAction("A:%s - FAILED", action->getName().c_str());
                    botAI->NoteIdleDiagRefusal(PlayerbotAI::IDLE_REFUSAL_FAILED);  // [grp20]
                    if (intentTick)
                    {
                        IntentMark(action->getName(), INTENT_FAILED, relevance, action);
                        intentPushBy = "alt of " + action->getName();
                    }
                    MultiplyAndPush(actionNode->getAlternatives(), relevance + 0.003f, false, event, "alt");
                }
            }
            else
            {
                LogAction("A:%s - IMPOSSIBLE", action->getName().c_str());
                botAI->NoteIdleDiagRefusal(PlayerbotAI::IDLE_REFUSAL_IMPOSSIBLE);  // [grp20]
                if (intentTick)
                {
                    // A multiplier that scaled it to 0 stays the reason (isPossible() still ran first).
                    if (relevance > 0)
                        IntentMark(action->getName(), INTENT_IMPOSSIBLE, relevance, action);
                    intentPushBy = "alt of " + action->getName();
                }
                MultiplyAndPush(actionNode->getAlternatives(), relevance + 0.003f, false, event, "alt");
            }
        }
        else
        {
            LogAction("A:%s - USELESS", action->getName().c_str());
            botAI->NoteIdleDiagRefusal(PlayerbotAI::IDLE_REFUSAL_USELESS);  // [grp20]
            if (intentTick)
                IntentMark(action->getName(), INTENT_USELESS, relevance, action);
            lastRelevance = relevance;
        }

        delete actionNode;  // Always delete after processing the action node
    }

    if (time(nullptr) - currentTime > 1)
    {
        LogAction("Execution time exceeded 1 second");
    }

    if (!actionExecuted)
    {
        LogAction("no actions executed");
        botAI->NoteIdleDiagRefusal(PlayerbotAI::IDLE_REFUSAL_NO_CANDIDATE);  // [grp20]
    }

    // [lead154] b173 20261101-154539 281-366 s: "Rtshaab buffing" 85 s - its Follow ran every tick at 94 and the
    // "ready reply" (6) that closes the rebuff window was never reached. The window's own fact: a full tick in which
    // no buff trigger fired and nothing is cast closes it here, whatever action ran
    if (!minimal)
    {
        ForceRebuffState& rebuff = botAI->forceRebuff;
        Player* self = botAI->GetBot();
        if (rebuff.IsPending() && !rebuff.IsBuffProposedThisCycle() && !rebuff.IsBuffPendingThisCycle() &&
            !rebuff.IsOnGlobalCooldown() && self && !self->IsInCombat() &&
            !self->GetCurrentSpell(CURRENT_GENERIC_SPELL) && !self->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            if (rebuff.ShouldReplyToReadyCheck())
                ReadyCheckAction::AnswerReadyCheck(botAI);
            rebuff.End();
        }
    }

    queue.RemoveExpired();

    if (intentTick)
    {
        intentTick = false;
        intentPushBy.clear();
        if (intentSink && getMSTimeDiff(intentSinkMs, intentTickMs) >= INTENT_SINK_MS)
        {
            intentSinkMs = intentTickMs;
            intentSink(botAI, this);
        }
    }

    return actionExecuted;
}

bool Engine::IsIntentWatched()
{
    uint32 const until = aiObjectContext->intentWatchUntilMs.load(std::memory_order_relaxed);
    if (!until)
        return false;

    if (int32(getMSTime() - until) >= 0)
    {
        aiObjectContext->intentWatchUntilMs.store(0, std::memory_order_relaxed);
        return false;
    }

    return intentSink != nullptr;
}

void Engine::IntentPush(std::string const& name, float relevance)
{
    for (IntentCandidate& candidate : intentCandidates)
        if (candidate.name == name)
        {
            // The queue keeps one basket per name with the higher relevance (Queue::Push).
            if (candidate.outcome == INTENT_PENDING && relevance > candidate.relevance)
                candidate.relevance = relevance;
            return;
        }

    if (intentCandidates.size() >= INTENT_MAX_CANDIDATES)
        return;

    IntentCandidate candidate;
    candidate.name = name;
    candidate.by = intentPushBy;
    candidate.relevance = relevance;
    intentCandidates.push_back(std::move(candidate));
}

IntentCandidate* Engine::IntentFind(std::string const& name)
{
    for (IntentCandidate& candidate : intentCandidates)
        if (candidate.name == name)
            return &candidate;

    if (intentCandidates.size() >= INTENT_MAX_CANDIDATES)
        return nullptr;

    // Still queued from an earlier tick.
    intentCandidates.emplace_back();
    IntentCandidate* added = &intentCandidates.back();
    added->name = name;
    added->by = "earlier tick";
    return added;
}

void Engine::IntentMark(std::string const& name, uint8 outcome, float relevance, Action* action)
{
    IntentCandidate* found = IntentFind(name);
    if (!found)
        return;

    found->outcome = outcome;
    found->relevance = relevance;
    if (action)
        found->action = action;

    if (outcome != INTENT_EXECUTED || !action)
        return;

    intentExecuted.name = name;
    intentExecuted.by = found->by;
    intentExecuted.relevance = relevance;
    intentExecuted.ms = getMSTime();
    intentExecuted.target.clear();
    intentExecuted.targetDistance = -1.0f;
    // The action's own target value (not a GetTarget() override, so an unknown value name cannot crash).
    if (Value<Unit*>* targetValue = action->GetTargetValue())
        if (Unit* target = targetValue->Get())
        {
            intentExecuted.target = target->GetName();
            Player* bot = botAI->GetBot();
            if (target->IsInWorld() && target->GetMap() == bot->GetMap())
                intentExecuted.targetDistance = bot->GetDistance(target);
        }
}

void Engine::IntentMarkMultiplier(Action* action, float relevance, std::string const& multiplier, float value)
{
    IntentCandidate* found = IntentFind(action->getName());
    if (!found)
        return;

    found->outcome = INTENT_MULTIPLIER;
    found->relevance = relevance;
    found->action = action;
    found->multiplier = multiplier;
    found->multiplierValue = value;
}

std::string Engine::IntentTriggerOwner(size_t triggerIndex)
{
    for (std::pair<size_t, Strategy*> const& owner : triggerOwners)
        if (triggerIndex < owner.first)
            return owner.second->getName();

    return "";
}

ActionNode* Engine::CreateActionNode(std::string const name)
{
    ActionNode* node = actionNodeFactories.GetContextObject(name, botAI);
    if (node)
        return node;

    return new ActionNode(name,
                          /*P*/ {},
                          /*A*/ {},
                          /*C*/ {});
}

bool Engine::MultiplyAndPush(
    std::vector<NextAction> actions,
    float forceRelevance,
    bool skipPrerequisites,
    Event event,
    char const* pushType
)
{
    bool pushed = false;

    for (NextAction nextAction : actions)
    {
        ActionNode* action = this->CreateActionNode(nextAction.getName());

        this->InitializeAction(action);

        float k = nextAction.getRelevance();

        if (forceRelevance > 0.0f)
        {
            k = forceRelevance;
        }

        if (k > 0)
        {
            this->LogAction("PUSH:%s - %f (%s)", action->getName().c_str(), k, pushType);
            if (intentTick)
                IntentPush(action->getName(), k);
            queue.Push(new ActionBasket(action, k, skipPrerequisites, event));
            pushed = true;

            continue;
        }

        delete action;

    }

    return pushed;
}

ActionResult Engine::ExecuteAction(std::string const name, Event event, std::string const qualifier)
{
    bool result = false;

    ActionNode* actionNode = CreateActionNode(name);
    if (!actionNode)
        return ACTION_RESULT_UNKNOWN;

    Action* action = InitializeAction(actionNode);
    if (!action)
    {
        delete actionNode;
        return ACTION_RESULT_UNKNOWN;
    }

    if (!qualifier.empty())
    {
        if (Qualified* q = dynamic_cast<Qualified*>(action))
            q->Qualify(qualifier);
    }

    if (!action->isUseful())
    {
        delete actionNode;
        return ACTION_RESULT_USELESS;
    }

    if (!action->isPossible())
    {
        delete actionNode;
        return ACTION_RESULT_IMPOSSIBLE;
    }

    action->MakeVerbose();

    result = ListenAndExecute(action, event);
    MultiplyAndPush(action->getContinuers(), 0.0f, false, event, "default");

    delete actionNode;

    return result ? ACTION_RESULT_OK : ACTION_RESULT_FAILED;
}

void Engine::addStrategy(std::string const name, bool init)
{
    removeStrategy(name, init);

    if (Strategy* strategy = aiObjectContext->GetStrategy(name))
    {
        std::set<std::string> siblings = aiObjectContext->GetSiblingStrategy(name);
        for (std::set<std::string>::iterator i = siblings.begin(); i != siblings.end(); i++)
            removeStrategy(*i, init);

        LogAction("S:+%s", strategy->getName().c_str());
        strategies[strategy->getName()] = strategy;
    }
    if (init)
        Init();
}

void Engine::addStrategies(std::string first, ...)
{
    addStrategy(first, false);

    va_list vl;
    va_start(vl, first);

    char const* cur;
    do
    {
        cur = va_arg(vl, char const*);
        if (cur)
            addStrategy(cur, false);
    } while (cur);

    Init();

    va_end(vl);
}

void Engine::addStrategiesNoInit(std::string first, ...)
{
    addStrategy(first, false);

    va_list vl;
    va_start(vl, first);

    char const* cur;
    do
    {
        cur = va_arg(vl, char const*);
        if (cur)
            addStrategy(cur, false);
    } while (cur);

    va_end(vl);
}

bool Engine::removeStrategy(std::string const name, bool init)
{
    std::map<std::string, Strategy*>::iterator i = strategies.find(name);
    if (i == strategies.end())
        return false;

    LogAction("S:-%s", name.c_str());
    strategies.erase(i);
    if (init)
        Init();

    return true;
}

void Engine::removeAllStrategies()
{
    strategies.clear();
    Init();
}

void Engine::toggleStrategy(std::string const name)
{
    if (!removeStrategy(name))
        addStrategy(name);
}

bool Engine::HasStrategy(std::string const name) { return strategies.find(name) != strategies.end(); }

Strategy* Engine::GetStrategy(std::string const name)
{
    std::map<std::string, Strategy*>::iterator i = strategies.find(name);
    return i != strategies.end() ? i->second : nullptr;
}

void Engine::ProcessTriggers(bool minimal)
{
    std::unordered_map<Trigger*, Event> fires;
    uint32 now = getMSTime();
    // [lead18] LEADER_EXECUTOR_TRIGGER_GATE: a led member evaluates no trigger whose every action is vetoed
    Leader::MemberEvents gateEvents;
    bool const gate = Leader::LEADER_EXECUTOR_TRIGGER_GATE && botAI && botAI->GetBot()->IsAlive() &&
                      Leader::ExecutorMember(botAI->GetBot());
    if (gate)
        Leader::Events::Get(botAI->GetBot()->GetGUID(), gateEvents);
    for (std::vector<TriggerNode*>::iterator i = triggers.begin(); i != triggers.end(); i++)
    {
        TriggerNode* node = *i;
        if (!node)
            continue;
        if (gate && RaidLeader::GateVetoes(botAI, gateEvents, node->getHandlers()))
            continue;

        Trigger* trigger = node->getTrigger();
        if (!trigger)
        {
            trigger = aiObjectContext->GetTrigger(node->getName());
            node->setTrigger(trigger);
        }

        if (!trigger)
            continue;

        if (fires.find(trigger) != fires.end())
            continue;

        if (testMode || trigger->needCheck(now))
        {
            if (minimal && node->getFirstRelevance() < 100)
                continue;

            SlowUpdate::Stage("trigger ", trigger);  // [perf212]
            PerfMonitorOperation* pmo =
                sPerfMonitor.start(PERF_MON_TRIGGER, trigger->getName(), &aiObjectContext->performanceStack);
            Event event = trigger->Check();
            if (pmo)
                pmo->finish();

            if (!event)
                continue;

            // [lead1095] a buff trigger names its spell (BuffTrigger: the spell's name); a buff its owner keeps up
            // is no work of the rebuff window
            if (trigger->IsBuffTrigger() && !trigger->IsDebuffTrigger() && botAI->forceRebuff.IsPending())
                botAI->forceRebuff.NoteBuffProposed(
                    aiObjectContext->GetValue<uint32>("spell id", trigger->getName())->Get());

            fires[trigger] = event;
            LogAction("T:%s", trigger->getName().c_str());
        }
    }

    for (std::vector<TriggerNode*>::iterator i = triggers.begin(); i != triggers.end(); i++)
    {
        TriggerNode* node = *i;
        Trigger* trigger = node->getTrigger();
        if (fires.find(trigger) == fires.end())
            continue;

        Event event = fires[trigger];
        if (intentTick)
            intentPushBy = "trigger " + trigger->getName() + " / " + IntentTriggerOwner(size_t(i - triggers.begin()));
        MultiplyAndPush(node->getHandlers(), 0.0f, false, event, "trigger");
    }

    for (std::vector<TriggerNode*>::iterator i = triggers.begin(); i != triggers.end(); i++)
    {
        if (Trigger* trigger = (*i)->getTrigger())
            trigger->Reset();
    }
}

void Engine::PushDefaultActions()
{
    // [lead18] LEADER_EXECUTOR_TRIGGER_GATE: a led member pushes no default action the executor model vetoes
    Leader::MemberEvents gateEvents;
    bool const gate = Leader::LEADER_EXECUTOR_TRIGGER_GATE && botAI && botAI->GetBot()->IsAlive() &&
                      Leader::ExecutorMember(botAI->GetBot());
    if (gate)
        Leader::Events::Get(botAI->GetBot()->GetGUID(), gateEvents);
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        Strategy* strategy = i->second;
        Event emptyEvent;
        if (intentTick)
            intentPushBy = "default / " + strategy->getName();
        std::vector<NextAction> defaults = strategy->getDefaultActions();
        if (gate)
            defaults.erase(std::remove_if(defaults.begin(), defaults.end(),
                                          [&](NextAction const& next)
                                          { return RaidLeader::GateVetoes(botAI, gateEvents, {next}); }),
                           defaults.end());
        MultiplyAndPush(defaults, 0.0f, false, emptyEvent, "default");
    }
}

std::string const Engine::ListStrategies()
{
    std::string s = "Strategies: ";

    if (strategies.empty())
        return s;

    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        s.append(i->first);
        s.append(", ");
    }

    return s.substr(0, s.length() - 2);
}

std::vector<std::string> Engine::GetStrategies()
{
    std::vector<std::string> result;
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        result.push_back(i->first);
    }

    return result;
}

void Engine::PushAgain(ActionNode* actionNode, float relevance, Event event)
{
    std::vector<NextAction> nextAction = { NextAction(actionNode->getName(), relevance) };

    MultiplyAndPush(nextAction, relevance, true, event, "again");

    delete actionNode;
}

bool Engine::ContainsStrategy(StrategyType type)
{
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        if (i->second->GetType() & type)
            return true;
    }
    return false;
}

Action* Engine::InitializeAction(ActionNode* actionNode)
{
    Action* action = actionNode->getAction();
    if (!action)
    {
        action = aiObjectContext->GetAction(actionNode->getName());
        actionNode->setAction(action);
    }

    if (action)
    {
        action->SetReaction(false);
        // Clear duration leftovers so ListenAndExecute doesn't re-apply a stale delay
        action->ResetDuration();
    }

    return action;
}

bool Engine::ListenAndExecute(Action* action, Event event)
{
    bool actionExecuted = false;

    if (action == nullptr)
    {
        LOG_ERROR("playerbots", "Action is nullptr");

        return actionExecuted;
    }

    // Intention diagnostics: which action started the bot's current movement spline.
    Player* intentBot = intentTick ? botAI->GetBot() : nullptr;
    uint32 const splineBefore = intentBot ? intentBot->movespline->GetId() : 0;

    if (actionExecutionListeners.Before(action, event))
    {
        MapWorkerTrace::SetStage("action ", action->getName().c_str());
        SlowUpdate::Stage("action ", action);  // [perf212]
        actionExecuted = actionExecutionListeners.AllowExecution(action, event) ? action->Execute(event) : true;
    }

    if (intentBot && intentBot->movespline->GetId() != splineBefore)
    {
        intentMover = action->getName();
        intentMoverMs = getMSTime();
    }

    if (botAI->HasStrategy("debug", BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "do: ";
        out << action->getName();

        if (actionExecuted)
            out << " 1 (";
        else
            out << " 0 (";

        out << action->getRelevance() << ")";

        if (!event.GetSource().empty())
            out << " [" << event.GetSource() << "]";

        botAI->TellMasterNoFacing(out);
    }

    actionExecuted = actionExecutionListeners.OverrideResult(action, actionExecuted, event);
    actionExecutionListeners.After(action, actionExecuted, event);

    if (actionExecuted && action->GetDuration() > 0)
        botAI->SetActionDuration(action);

    return actionExecuted;
}

void Engine::LogAction(char const* format, ...)
{
    Player* bot = botAI->GetBot();
    // The short in-memory trail (GetLastAction, used by `playerbots diag` and the test harness) is kept for every
    // bot; only the debug log is limited to groups with a game client.
    bool const quiet = sPlayerbotAIConfig.LogInGroupOnly && (!bot->GetGroup() || !botAI->HasGameClientMaster()) &&
                       !testMode;

    char buf[1024];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    lastAction += "|";
    lastAction += buf;
    if (lastAction.size() > 512)
    {
        lastAction = lastAction.substr(512);
        size_t pos = lastAction.find("|");
        lastAction = (pos == std::string::npos ? "" : lastAction.substr(pos));
    }

    if (testMode)
    {
        FILE* file = fopen("test.log", "a");
        fprintf(file, "'%s'", buf);
        fprintf(file, "\n");
        fclose(file);
    }
    else if (!quiet)
    {
        LOG_DEBUG("playerbots", "{} {}", bot->GetName().c_str(), buf);
    }
}

std::string const Engine::ResolveStrategyName(std::string const name)
{
    if (HasStrategy(name))
        return name;

    Strategy* strategy = aiObjectContext->GetStrategy(name);
    return strategy ? strategy->getName() : name;
}

void Engine::ChangeStrategy(std::string const names)
{
    std::vector<std::string> splitted = split(names, ',');
    for (std::vector<std::string>::iterator i = splitted.begin(); i != splitted.end(); i++)
    {
        char const* name = i->c_str();
        switch (name[0])
        {
            case '+':
                addStrategy(name + 1);
                break;
            case '-':
                removeStrategy(ResolveStrategyName(name + 1));
                break;
            case '~':
                toggleStrategy(ResolveStrategyName(name + 1));
                break;
            case '?':
                botAI->TellMaster(ListStrategies());
                break;
        }
    }
}

void Engine::LogValues()
{
    if (testMode)
        return;

    Player* bot = botAI->GetBot();
    if (sPlayerbotAIConfig.LogInGroupOnly && (!bot->GetGroup() || !botAI->HasGameClientMaster()))
        return;

    std::string const text = botAI->GetAiObjectContext()->FormatValues();
    LOG_DEBUG("playerbots", "Values for {}: {}", bot->GetName().c_str(), text.c_str());
}
