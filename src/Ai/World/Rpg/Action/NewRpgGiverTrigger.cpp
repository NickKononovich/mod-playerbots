/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgGiverTrigger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Creature.h"
#include "CreatureData.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "NewRpgAction.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
using NewRpgGiverTrigger::Step;
using NewRpgGiverTrigger::StepKind;
using TriggerT = NewRpgGiverTrigger::Trigger;

std::unordered_map<uint32, std::vector<TriggerT>> triggersOf;  // quest -> the triggers of its givers
std::once_flag built;
std::mutex logLock;
std::set<std::pair<uint32, uint32>> cannotLogged;  // (bot, quest) whose refusal was logged

std::string CreatureName(uint32 entry)
{
    CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
    return tmpl ? tmpl->Name : "creature " + std::to_string(entry);
}

std::string SpellName(uint32 spellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    return info && info->SpellName[0] ? std::string(info->SpellName[0]) : "spell " + std::to_string(spellId);
}

std::string ItemName(uint32 itemId)
{
    ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(itemId);
    return tmpl ? tmpl->Name1 : "item " + std::to_string(itemId);
}

bool SetsGiverFlag(SmartScriptHolder const& h)
{
    return (h.GetActionType() == SMART_ACTION_SET_NPC_FLAG || h.GetActionType() == SMART_ACTION_ADD_NPC_FLAG) &&
           (h.action.flag.flag & UNIT_NPC_FLAG_QUESTGIVER);
}

bool IdleEvent(uint32 type)
{
    return type == SMART_EVENT_UPDATE_OOC || type == SMART_EVENT_UPDATE || type == SMART_EVENT_RESPAWN ||
           type == SMART_EVENT_RESET || type == SMART_EVENT_JUST_CREATED;
}

// the event that starts a linked chain (a SMART_EVENT_LINK holder runs when the one linking to it ran)
SmartScriptHolder const* RootOf(SmartAIEventList const& events, SmartScriptHolder const* h)
{
    for (size_t guard = 0; h && h->GetEventType() == SMART_EVENT_LINK && guard < events.size(); ++guard)
    {
        auto const from = std::find_if(events.begin(), events.end(),
                                       [h](SmartScriptHolder const& e) { return e.link && e.link == h->event_id; });
        if (from == events.end())
            break;
        h = &*from;
    }
    return h;
}

// a holder sets the flag itself, or calls a timed action list that does: the list's delays up to the flag
bool FlagSource(SmartScriptHolder const& h, uint32& delayMs)
{
    delayMs = 0;
    if (SetsGiverFlag(h))
        return true;
    if (h.GetActionType() != SMART_ACTION_CALL_TIMED_ACTIONLIST)
        return false;
    SmartAIEventList const list =
        sSmartScriptMgr->GetScript(int32(h.action.timedActionList.id), SMART_SCRIPT_TYPE_TIMED_ACTIONLIST);
    uint32 delay = 0;
    for (SmartScriptHolder const& a : list)
    {
        delay += std::max(a.event.minMaxRepeat.min, a.event.minMaxRepeat.max);
        if (SetsGiverFlag(a))
        {
            delayMs = delay;
            return true;
        }
    }
    return false;
}

// spell -> the first item whose on-use spell it is; GO entry -> the spells that summon it
struct ItemSpells
{
    std::unordered_map<uint32, uint32> itemOf;
    std::unordered_map<uint32, std::vector<uint32>> summonsObject;
    bool loaded = false;

    void Load()
    {
        if (loaded)
            return;
        loaded = true;
        for (auto const& [itemId, tmpl] : *sObjectMgr->GetItemTemplateStore())
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                if (tmpl.Spells[i].SpellId > 0 && tmpl.Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                    itemOf.emplace(uint32(tmpl.Spells[i].SpellId), itemId);
        for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id))
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                {
                    uint32 const effect = info->Effects[e].Effect;
                    if ((effect == SPELL_EFFECT_SUMMON_OBJECT_WILD || effect == SPELL_EFFECT_TRANS_DOOR ||
                         (effect >= SPELL_EFFECT_SUMMON_OBJECT_SLOT1 && effect <= SPELL_EFFECT_SUMMON_OBJECT_SLOT4)) &&
                        info->Effects[e].MiscValue > 0)
                        summonsObject[uint32(info->Effects[e].MiscValue)].push_back(id);
                }
    }
};

// who sets data (field, value) on a creature entry, read from every SmartAI creature and object script once
struct DataSetter
{
    uint32 entry = 0;  // the setter's creature or object entry
    bool object = false;
    SmartScriptHolder root;  // the event that starts the set
    float dist = 0.0f;       // the target's reach (0: none given)
};
struct DataSetters
{
    std::multimap<std::tuple<uint32, uint32, uint32>, DataSetter> of;  // (target entry, field, value)
    bool loaded = false;

    static uint32 TargetEntry(SmartScriptHolder const& h, float& dist)
    {
        dist = 0.0f;
        switch (h.GetTargetType())
        {
            case SMART_TARGET_CLOSEST_CREATURE:
                dist = float(h.target.unitClosest.dist);
                return h.target.unitClosest.entry;
            case SMART_TARGET_CREATURE_RANGE:
                dist = float(h.target.unitRange.maxDist);
                return h.target.unitRange.creature;
            case SMART_TARGET_CREATURE_DISTANCE:
                dist = float(h.target.unitDistance.dist);
                return h.target.unitDistance.creature;
            case SMART_TARGET_CREATURE_GUID:
                if (h.target.unitGUID.entry)
                    return h.target.unitGUID.entry;
                if (CreatureData const* data = sObjectMgr->GetCreatureData(h.target.unitGUID.dbGuid))
                    return data->id;
                return 0;
            default:
                return 0;
        }
    }

    void Add(uint32 entry, bool object, SmartAIEventList const& events)
    {
        for (SmartScriptHolder const& h : events)
        {
            if (h.GetActionType() != SMART_ACTION_SET_DATA)
                continue;
            DataSetter setter;
            uint32 const target = TargetEntry(h, setter.dist);
            if (!target)
                continue;
            setter.entry = entry;
            setter.object = object;
            setter.root = *RootOf(events, &h);
            of.emplace(std::make_tuple(target, h.action.setData.field, h.action.setData.data), setter);
        }
    }

    void Load()
    {
        if (loaded)
            return;
        loaded = true;
        for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
            if (tmpl.AIName == "SmartAI")
                Add(entry, false, sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE));
        for (auto const& [entry, tmpl] : *sObjectMgr->GetGameObjectTemplates())
            if (tmpl.AIName == "SmartGameObjectAI")
                Add(entry, true, sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_GAMEOBJECT));
    }
};

std::string Source(uint32 entry, SmartScriptHolder const& h)
{
    return Acore::StringFormat("SmartAI {} event {} type {}", entry, h.event_id, h.GetEventType());
}

// the step a root event of the giver's script asks for
Step StepOf(uint32 giver, SmartScriptHolder const& root, ItemSpells& items, DataSetters& setters)
{
    Step step;
    step.source = Source(giver, root);
    switch (root.GetEventType())
    {
        case SMART_EVENT_SPELLHIT:
        {
            uint32 const spell = root.event.spellHit.spell;
            if (!spell)
            {
                step.why = "a hit of any spell (no spell named)";
                return step;
            }
            items.Load();
            auto const item = items.itemOf.find(spell);
            step.kind = item != items.itemOf.end() ? StepKind::Item : StepKind::Spell;
            step.itemId = item != items.itemOf.end() ? item->second : 0;
            step.spells.push_back(spell);
            return step;
        }
        case SMART_EVENT_GOSSIP_SELECT:
            step.kind = StepKind::Gossip;
            step.menu = root.event.gossip.sender;
            step.option = root.event.gossip.action;
            return step;
        case SMART_EVENT_GOSSIP_HELLO:
            step.kind = StepKind::Gossip;
            step.hello = true;
            return step;
        case SMART_EVENT_HEALTH_PCT:
            step.kind = StepKind::Fight;
            step.healthPct = root.event.minMaxRepeat.max;
            return step;
        case SMART_EVENT_UPDATE_OOC:
        case SMART_EVENT_UPDATE:
            step.kind = StepKind::Wait;
            step.flagDelayMs = std::max(root.event.minMaxRepeat.repeatMin, root.event.minMaxRepeat.repeatMax);
            return step;
        case SMART_EVENT_DATA_SET:
        {
            setters.Load();
            auto const range =
                setters.of.equal_range(std::make_tuple(giver, root.event.dataSet.id, root.event.dataSet.value));
            std::string seen;
            for (auto it = range.first; it != range.second; ++it)
            {
                DataSetter const& setter = it->second;
                step.source += Acore::StringFormat(" <- {} {} event {} type {}", setter.object ? "object" : "SmartAI",
                                                   setter.entry, setter.root.event_id, setter.root.GetEventType());
                if (!setter.object && setter.root.GetEventType() == SMART_EVENT_DEATH)
                {
                    step.kind = StepKind::Kill;
                    step.killEntry = setter.entry;
                    step.radius = setter.dist;
                    return step;
                }
                if (setter.object && setter.root.GetEventType() == SMART_EVENT_JUST_CREATED)
                {
                    items.Load();
                    auto const spells = items.summonsObject.find(setter.entry);
                    if (spells != items.summonsObject.end())
                        for (uint32 spell : spells->second)
                            if (auto const item = items.itemOf.find(spell); item != items.itemOf.end())
                            {
                                step.kind = StepKind::Item;
                                step.itemId = item->second;
                                step.spells.push_back(spell);
                                step.radius = setter.dist;
                                return step;
                            }
                }
                seen += (seen.empty() ? "" : ", ") +
                        (setter.object ? "object " + std::to_string(setter.entry) : CreatureName(setter.entry)) +
                        " event type " + std::to_string(setter.root.GetEventType());
            }
            step.why = Acore::StringFormat("data {} {} on it comes from {}", root.event.dataSet.id,
                                           root.event.dataSet.value,
                                           seen.empty() ? std::string("no script of a creature or object") : seen);
            return step;
        }
        default:
            step.why = Acore::StringFormat("the flag comes on its script's event type {} (event {})",
                                           root.GetEventType(), root.event_id);
            return step;
    }
}

// the steps that put the giver's script into one of the phases of `mask` (none: the creature starts in one, or its
// reset / respawn sets it); false: nothing sets it
bool PhaseSteps(uint32 giver, SmartAIEventList const& events, uint32 mask, ItemSpells& items, DataSetters& setters,
                std::vector<Step>& out)
{
    Step merged;  // the spell hits that set the phase: one step of their spells (Gift of the Naaru's class ranks)
    bool any = false;
    for (SmartScriptHolder const& h : events)
    {
        if (h.GetActionType() != SMART_ACTION_SET_EVENT_PHASE || !h.action.setEventPhase.phase ||
            !(mask & (1u << (h.action.setEventPhase.phase - 1))))
            continue;
        SmartScriptHolder const* root = RootOf(events, &h);
        if (IdleEvent(root->GetEventType()))
            return true;  // its own reset or spawn puts it there
        Step step = StepOf(giver, *root, items, setters);
        if (step.kind == StepKind::Unsupported)
            continue;
        if (step.kind == StepKind::Spell && merged.kind == StepKind::Spell)
        {
            merged.spells.insert(merged.spells.end(), step.spells.begin(), step.spells.end());
            merged.source += "," + std::to_string(root->event_id);
        }
        else if (!any)
            merged = step;
        any = true;
    }
    if (any)
        out.push_back(merged);
    return any;
}

std::vector<TriggerT> TriggersOf(uint32 giver, ItemSpells& items, DataSetters& setters)
{
    std::vector<TriggerT> triggers;
    SmartAIEventList const events = sSmartScriptMgr->GetScript(int32(giver), SMART_SCRIPT_TYPE_CREATURE);
    std::set<uint32> roots;
    for (SmartScriptHolder const& h : events)
    {
        uint32 delayMs = 0;
        if (!FlagSource(h, delayMs))
            continue;
        SmartScriptHolder const* root = RootOf(events, &h);
        if (!roots.insert(root->event_id).second)
            continue;
        TriggerT trigger;
        trigger.giver = giver;
        if (uint32 const mask = root->event.event_phase_mask)
            if (!PhaseSteps(giver, events, mask, items, setters, trigger.steps))
            {
                Step none;
                none.why = Acore::StringFormat("no event of its script sets the phase (mask {}) of event {}", mask,
                                               root->event_id);
                none.source = Source(giver, *root);
                trigger.steps.push_back(none);
            }
        Step main = StepOf(giver, *root, items, setters);
        // a fight the giver's script starts from a gossip (its faction and attack come from that option) - no phase
        // ties them, the gossip goes first (Empoor: gossip 7704, then fought down to 1 %)
        if (main.kind == StepKind::Fight && trigger.steps.empty())
        {
            auto const gossip = std::find_if(events.begin(), events.end(), [](SmartScriptHolder const& e)
                                             { return e.GetEventType() == SMART_EVENT_GOSSIP_SELECT; });
            if (gossip != events.end())
                trigger.steps.push_back(StepOf(giver, *gossip, items, setters));
        }
        main.flagDelayMs += delayMs;
        trigger.steps.push_back(main);
        trigger.retry = std::any_of(trigger.steps.begin(), trigger.steps.end(),
                                    [](Step const& s) { return s.kind == StepKind::Kill; });
        triggers.push_back(std::move(trigger));
    }
    if (triggers.empty())
    {
        TriggerT trigger;
        trigger.giver = giver;
        Step none;
        none.why = "no event of its script gives the quest-giver flag";
        trigger.steps.push_back(none);
        triggers.push_back(std::move(trigger));
    }
    return triggers;
}

// [giver14] the quests: every giver a creature whose SmartAI takes its quest-giver flag off in its idle state (SET
// without the flag, REMOVE with it, on out-of-combat / respawn / reset / creation events or those linked from them)
// or whose template has no flag; no game object or item starts it
std::set<uint32> ScriptedQuests()
{
    std::unordered_map<uint32, bool> scriptedEntry;
    auto const scripted = [&scriptedEntry](uint32 entry)
    {
        auto const found = scriptedEntry.find(entry);
        if (found != scriptedEntry.end())
            return found->second;
        bool drops = false;
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        if (tmpl && tmpl->AIName == "SmartAI")
        {
            SmartAIEventList const events = sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE);
            auto const idle = [&](SmartScriptHolder const& holder)
            {
                uint32 const type = holder.GetEventType();
                if (type == SMART_EVENT_UPDATE_OOC || type == SMART_EVENT_RESPAWN || type == SMART_EVENT_RESET ||
                    type == SMART_EVENT_JUST_CREATED)
                    return true;
                if (type != SMART_EVENT_LINK)
                    return false;
                uint32 const from = RootOf(events, &holder)->GetEventType();
                return from == SMART_EVENT_UPDATE_OOC || from == SMART_EVENT_RESPAWN || from == SMART_EVENT_RESET ||
                       from == SMART_EVENT_JUST_CREATED;
            };
            if (!(tmpl->npcflag & UNIT_NPC_FLAG_QUESTGIVER))
                drops = true;
            for (SmartScriptHolder const& holder : events)
                if (((holder.GetActionType() == SMART_ACTION_SET_NPC_FLAG &&
                      !(holder.action.flag.flag & UNIT_NPC_FLAG_QUESTGIVER)) ||
                     (holder.GetActionType() == SMART_ACTION_REMOVE_NPC_FLAG &&
                      (holder.action.flag.flag & UNIT_NPC_FLAG_QUESTGIVER))) &&
                    idle(holder))
                    drops = true;
        }
        scriptedEntry[entry] = drops;
        return drops;
    };
    std::unordered_map<uint32, bool> allScripted;
    for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
    {
        bool const s = scripted(entry);
        auto const found = allScripted.find(questId);
        if (found == allScripted.end())
            allScripted[questId] = s;
        else
            found->second = found->second && s;
    }
    for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestRelationMap())
        allScripted[questId] = false;
    for (auto const& [itemId, tmpl] : *sObjectMgr->GetItemTemplateStore())
        if (tmpl.StartQuest)
            allScripted[tmpl.StartQuest] = false;
    std::set<uint32> quests;
    for (auto const& [questId, s] : allScripted)
        if (s)
            quests.insert(questId);
    return quests;
}

float CreatureHealth(uint32 entry)
{
    CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
    if (!tmpl)
        return 0.0f;
    uint8 const level = uint8((uint32(tmpl->minlevel) + tmpl->maxlevel) / 2);
    CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, tmpl->unit_class);
    return stats ? float(stats->GenerateHealth(tmpl)) : 0.0f;
}

uint32 FirstKnown(Player* bot, Step const& step)
{
    if (step.kind == StepKind::Item)
        return step.spells.empty() ? 0 : step.spells.front();
    for (uint32 spell : step.spells)
        if (bot->HasSpell(spell))
            return spell;
    return 0;
}

bool CanDoStep(Player* bot, Step const& step, std::string* why)
{
    switch (step.kind)
    {
        case StepKind::Spell:
            if (FirstKnown(bot, step))
                return true;
            if (why)
                *why = "it knows no rank of " + SpellName(step.spells.front()) + " (" + step.source + ")";
            return false;
        case StepKind::Item:
            if (bot->HasItemCount(step.itemId, 1))
                return true;
            if (why)
                *why = Acore::StringFormat("no {} (item {}) in its bags ({})", ItemName(step.itemId), step.itemId,
                                           step.source);
            return false;
        case StepKind::Unsupported:
            if (why)
                *why = step.why + " (" + step.source + ")";
            return false;
        default:
            return true;
    }
}
}  // namespace

void NewRpgGiverTrigger::Build()
{
    std::call_once(built, []()
    {
        ItemSpells items;
        DataSetters setters;
        std::unordered_map<uint32, std::vector<TriggerT>> ofGiver;
        std::set<uint32> const quests = ScriptedQuests();
        std::map<std::string, uint32> kinds;
        uint32 doable = 0;
        for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
        {
            if (!quests.count(questId))
                continue;
            auto found = ofGiver.find(entry);
            if (found == ofGiver.end())
                found = ofGiver.emplace(entry, TriggersOf(entry, items, setters)).first;
            auto& list = triggersOf[questId];
            list.insert(list.end(), found->second.begin(), found->second.end());
        }
        // a kill step's creature with spawns of its own: a respawn brings it back (else a script summons it)
        std::unordered_set<uint32> spawned;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            spawned.insert(data.id);
        for (auto& [questId, list] : triggersOf)
            for (TriggerT& t : list)
                for (Step& s : t.steps)
                    if (s.kind == StepKind::Kill)
                        s.killSpawned = spawned.count(s.killEntry) > 0;
        for (auto const& [questId, list] : triggersOf)
        {
            bool any = false;
            for (TriggerT const& t : list)
            {
                bool ok = true;
                std::string steps;
                for (Step const& s : t.steps)
                {
                    static char const* const names[] = {"spell", "item", "gossip", "kill", "fight", "wait", "none"};
                    steps += (steps.empty() ? "" : "+") + std::string(names[uint8(s.kind)]);
                    ok = ok && s.kind != StepKind::Unsupported;
                }
                ++kinds[steps];
                any = any || ok;
                LOG_INFO("playerbots", "[giver15] q{} giver {} ({}): {}", questId, CreatureName(t.giver), t.giver,
                         Describe(t));
            }
            doable += any ? 1 : 0;
        }
        std::string summary;
        for (auto const& [steps, n] : kinds)
            summary += Acore::StringFormat("{}{} {}", summary.empty() ? "" : ", ", n, steps);
        LOG_INFO("playerbots", "[giver15] {} quests start only at creatures whose script gives the giver flag: {} with "
                 "a trigger a bot can do (triggers: {})",
                 triggersOf.size(), doable, summary.empty() ? "-" : summary);
    });
}

bool NewRpgGiverTrigger::IsScripted(uint32 questId) { return triggersOf.count(questId) > 0; }

std::vector<TriggerT> const* NewRpgGiverTrigger::For(uint32 questId)
{
    auto const found = triggersOf.find(questId);
    return found == triggersOf.end() ? nullptr : &found->second;
}

bool NewRpgGiverTrigger::CanDo(Player* bot, TriggerT const& trigger, std::string* why)
{
    for (Step const& s : trigger.steps)
        if (!CanDoStep(bot, s, why))
            return false;
    return true;
}

bool NewRpgGiverTrigger::CanTrigger(Player* bot, uint32 questId, std::string* why)
{
    std::vector<TriggerT> const* list = For(questId);
    if (!list)
        return true;
    std::string first;
    for (TriggerT const& t : *list)
    {
        std::string reason;
        if (CanDo(bot, t, &reason))
            return true;
        if (first.empty())
            first = CreatureName(t.giver) + ": " + reason;
    }
    if (why)
        *why = first;
    return false;
}

bool NewRpgGiverTrigger::Plannable(Player* bot, uint32 questId)
{
    std::string why;
    if (CanTrigger(bot, questId, &why))
        return true;
    std::lock_guard<std::mutex> guard(logLock);
    if (cannotLogged.insert({bot->GetGUID().GetCounter(), questId}).second)
        LOG_INFO("playerbots", "[giver15] {} cannot trigger q{}: {}", bot->GetName(), questId, why);
    return false;
}

float NewRpgGiverTrigger::Seconds(Player* bot, uint32 questId, float dps)
{
    std::vector<TriggerT> const* list = For(questId);
    if (!list)
        return 0.0f;
    float best = -1.0f;
    for (TriggerT const& t : *list)
    {
        if (!CanDo(bot, t, nullptr))
            continue;
        float seconds = 0.0f;
        for (Step const& s : t.steps)
        {
            switch (s.kind)
            {
                case StepKind::Spell:
                case StepKind::Item:
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(FirstKnown(bot, s)))
                        seconds += std::max(float(info->CalcCastTime(bot)), float(info->StartRecoveryTime)) / 1000.0f +
                                   float(bot->GetSpellCooldownDelay(info->Id)) / 1000.0f;
                    break;
                case StepKind::Kill:
                    seconds += CreatureHealth(s.killEntry) / std::max(dps, 1.0f);
                    break;
                case StepKind::Fight:
                    seconds += CreatureHealth(t.giver) * float(100 - std::min<uint32>(s.healthPct, 100)) / 100.0f /
                               std::max(dps, 1.0f);
                    break;
                default:
                    break;
            }
            seconds += float(s.flagDelayMs) / 1000.0f;
        }
        if (best < 0.0f || seconds < best)
            best = seconds;
    }
    return std::max(best, 0.0f);
}

std::string NewRpgGiverTrigger::Describe(Step const& step)
{
    switch (step.kind)
    {
        case StepKind::Spell:
            return "casts " + SpellName(step.spells.front()) + " on it";
        case StepKind::Item:
            return "uses " + ItemName(step.itemId) + " (" + SpellName(step.spells.front()) + ") on or next to it";
        case StepKind::Gossip:
            return step.hello ? std::string("talks to it (gossip hello)")
                              : Acore::StringFormat("gossip option {} of menu {}", step.option, step.menu);
        case StepKind::Kill:
            return step.radius > 0.0f
                       ? Acore::StringFormat("kills {} within {:.0f} yd of it", CreatureName(step.killEntry),
                                             step.radius)
                       : "kills " + CreatureName(step.killEntry);
        case StepKind::Fight:
            return Acore::StringFormat("fights it down to {} %", step.healthPct);
        case StepKind::Wait:
            return Acore::StringFormat("waits for its script's own flag (every {} s)", step.flagDelayMs / 1000);
        case StepKind::Unsupported:
            return "none: " + step.why;
    }
    return "?";
}

std::string NewRpgGiverTrigger::Describe(TriggerT const& trigger)
{
    std::string text;
    for (Step const& s : trigger.steps)
        text += (text.empty() ? "" : ", then ") + Describe(s) + " [" + s.source + "]";
    if (!trigger.steps.empty() && trigger.steps.back().flagDelayMs && trigger.steps.back().kind != StepKind::Wait)
        text += Acore::StringFormat(", the flag {:.1f} s later", trigger.steps.back().flagDelayMs / 1000.0f);
    return text;
}

// [giver15] at the giver of a quest it still has to take (DO_QUEST walked within reach of its spawn): the flag up,
// the quest is taken; else the trigger's next step - cast, use, talk, kill near it, fight it - and the flag awaited as
// a game state: the bot's cast under way, the kill's target alive, the script's own delay from the last step (its
// action list). Every step done and no flag: a kill's trigger again (the kill fell outside its reach or the phase
// lapsed), any other is postponed with the reason
bool NewRpgDoQuestAction::TriggerQuestGiver(uint32 questId)
{
    using namespace NewRpgGiverTrigger;
    std::vector<TriggerT> const* triggers = For(questId);
    if (!triggers)
        return false;
    if (bot->IsInCombat())
        return true;  // the fight first (a kill or fight step's own, or any other), the trigger after it
    GiverRun& run = _giverRun;
    if (run.questId != questId)
    {
        run = GiverRun();
        run.questId = questId;
    }
    Creature* giver = run.giver ? ObjectAccessor::GetCreature(*bot, run.giver) : nullptr;
    auto const cannot = [&](std::string const& why)
    {
        LOG_INFO("playerbots", "[giver15] {} cannot trigger q{}: {}", bot->GetName(), questId, why);
        LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: giver trigger: {}", bot->GetName(), bot->GetLevel(),
                 questId, why);
        NotePostpone(bot, questId, "giver trigger: " + why);
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgInfo.ChangeToIdle();
        run = GiverRun();
        return true;
    };
    float const sight = sPlayerbotAIConfig.SightDistance;
    if (!giver || !giver->IsAlive() || run.trigger >= triggers->size())
    {
        giver = nullptr;
        std::string why;
        float best = sight;
        for (size_t i = 0; i < triggers->size(); ++i)
        {
            TriggerT const& t = (*triggers)[i];
            if (!CanDo(bot, t, &why))
                continue;
            Creature* c = bot->FindNearestCreature(t.giver, sight);
            if (c && c->IsAlive() && bot->GetDistance(c) <= best)
            {
                best = bot->GetDistance(c);
                giver = c;
                run.trigger = i;
            }
        }
        if (!giver)
            return cannot(why.empty() ? "no giver alive in sight" : why);
        run.giver = giver->GetGUID();
        run.step = 0;
        run.kill.Clear();
        run.casting = false;
        run.fighting = false;
    }
    TriggerT const& trigger = (*triggers)[run.trigger];
    std::string const giverName = giver->GetName();

    if (giver->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
    {
        if (!bot->CanInteractWithQuestGiver(giver))
        {
            if (MoveWorldObjectTo(giver->GetGUID(), INTERACTION_DISTANCE - 1.0f) || bot->isMoving())
                return true;
            return cannot("no walk to " + giverName + " (its flag is up)");
        }
        InteractWithNpcOrGameObjectForQuest(giver->GetGUID());
        if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
            return cannot(giverName + " shows the quest-giver flag but does not give it the quest");
        LOG_INFO("playerbots", "[giver15] {} takes q{} from {} after its trigger", bot->GetName(), questId, giverName);
        run = GiverRun();
        return true;
    }

    if (run.step >= trigger.steps.size())
    {
        // every step done: the script's own delay to the flag (its action list), then the flag is looked at
        if (getMSTimeDiff(run.doneMs, getMSTime()) <= trigger.steps.back().flagDelayMs)
            return true;
        if (!trigger.retry)
            return cannot("no flag on " + giverName + " after " + Describe(trigger));
        LOG_INFO("playerbots", "[giver15] {} triggers {} for q{}: again, no flag after the kill", bot->GetName(),
                 giverName, questId);
        run.step = 0;
        run.kill.Clear();
        return true;
    }

    Step const& step = trigger.steps[run.step];
    auto const next = [&]()
    {
        if (++run.step >= trigger.steps.size())
            run.doneMs = getMSTime();
        return true;
    };
    auto const triggers15 = [&](std::string const& how)
    {
        LOG_INFO("playerbots", "[giver15] {} triggers {} for q{}: {}", bot->GetName(), giverName, questId, how);
    };
    auto const walkTo = [&](float distance)
    {
        if (MoveWorldObjectTo(giver->GetGUID(), distance) || bot->isMoving())
            return true;
        return cannot("no walk to " + giverName);
    };

    switch (step.kind)
    {
        case StepKind::Spell:
        case StepKind::Item:
        {
            if (run.casting)
            {
                if (bot->IsNonMeleeSpellCast(false))
                    return true;
                run.casting = false;
                return next();
            }
            uint32 const spellId = FirstKnown(bot, step);
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            Item* item = step.kind == StepKind::Item ? bot->GetItemByEntry(step.itemId) : nullptr;
            if (!info || (step.kind == StepKind::Item && !item))
            {
                std::string why;
                CanDo(bot, trigger, &why);
                return cannot(why.empty() ? "no spell or item for " + Describe(step) : why);
            }
            // the item's spell on the giver when it takes a unit; else it is used next to the giver (an object it
            // summons, an area it hits)
            bool const onUnit = step.kind == StepKind::Spell || info->NeedsExplicitUnitTarget();
            float reach = onUnit ? std::max(info->GetMaxRange(info->IsPositive(), bot) - 1.0f, 1.0f) : 0.0f;
            if (!onUnit)
            {
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                    reach = std::max(reach, info->Effects[e].CalcRadius(bot));
                if (step.radius > 0.0f)
                    reach = reach > 0.0f ? std::min(reach, step.radius) : step.radius;
                reach = reach > 0.0f ? reach * 0.5f : INTERACTION_DISTANCE - 1.0f;
            }
            if (bot->GetDistance(giver) > reach || !bot->IsWithinLOSInMap(giver))
                return walkTo(reach * 0.8f);
            if (bot->isMoving())
            {
                bot->StopMoving();
                return true;
            }
            if (bot->HasSpellCooldown(spellId))
                return true;  // its cooldown runs: it waits at the giver (the rate counted it)
            bot->SetFacingToObject(giver);
            Unit* target = onUnit ? static_cast<Unit*>(giver) : static_cast<Unit*>(bot);
            PlayerbotAI::CastCheck const check = botAI->CheckCastNow(spellId, target, item);
            if (!check.Castable())
            {
                if (!check.Refused())
                    return true;  // a passing state (the global cooldown, a cast under way)
                return cannot(Acore::StringFormat("{} refused: {}", Describe(step),
                                                  PlayerbotAI::CastResultName(check.refusal)));
            }
            bool cast = false;
            if (item)
            {
                WorldPacket packet(CMSG_USE_ITEM);
                packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot()) << uint8(1) << spellId
                       << item->GetGUID() << uint32(0) << uint8(0);
                if (onUnit)
                    packet << uint32(TARGET_FLAG_UNIT) << giver->GetGUID().WriteAsPacked();
                else
                    packet << uint32(TARGET_FLAG_NONE);
                bot->GetSession()->HandleUseItemOpcode(packet);
                cast = true;
            }
            else
                cast = botAI->CastSpell(spellId, target);
            if (!cast)
                return cannot(Describe(step) + ": the cast failed");
            run.casting = true;
            triggers15(Acore::StringFormat("{} ({}, {:.1f} yd)",
                                           step.kind == StepKind::Item
                                               ? "uses " + ItemName(step.itemId) + (onUnit ? " on it" : " next to it")
                                               : "casts " + SpellName(spellId) + " on it",
                                           step.source, bot->GetDistance(giver)));
            return true;
        }
        case StepKind::Gossip:
        {
            if (!bot->IsWithinDistInMap(giver, INTERACTION_DISTANCE))
                return walkTo(INTERACTION_DISTANCE - 1.0f);
            bot->StopMoving();
            bot->SetFacingToObject(giver);
            WorldPacket hello(CMSG_GOSSIP_HELLO, 8);
            hello << giver->GetGUID();
            bot->GetSession()->HandleGossipHelloOpcode(hello);
            if (!step.hello)
            {
                WorldPacket select(CMSG_GOSSIP_SELECT_OPTION, 8 + 4 + 4);
                select << giver->GetGUID() << step.menu << step.option;
                bot->GetSession()->HandleGossipSelectOptionOpcode(select);
            }
            bot->PlayerTalkClass->SendCloseGossip();
            triggers15(Describe(step) + " (" + step.source + ")");
            return next();
        }
        case StepKind::Kill:
        {
            Creature* target = run.kill ? ObjectAccessor::GetCreature(*bot, run.kill) : nullptr;
            if (run.kill && (!target || !target->IsAlive()))
            {
                // its death: within the reach the flag follows, else the trigger goes again (no flag)
                triggers15(target ? Acore::StringFormat("killed {} {:.1f} yd from it", target->GetName(),
                                                        target->GetDistance(giver))
                                  : std::string("its kill target is gone"));
                run.kill.Clear();
                return next();
            }
            if (!target)
            {
                std::list<Creature*> found;
                bot->GetCreatureListWithEntryInGrid(found, step.killEntry, sight);
                float best = std::numeric_limits<float>::max();
                for (Creature* c : found)
                {
                    if (!c->IsAlive() || !bot->IsValidAttackTarget(c) || (c->IsInCombat() && !c->IsInCombatWith(bot) &&
                                                                          !c->IsInCombatWith(giver)))
                        continue;
                    if (c->GetDistance(giver) < best)
                    {
                        best = c->GetDistance(giver);
                        target = c;
                    }
                }
                if (!target)
                {
                    // the trigger goes again once one is back: a spawned creature's respawn (the game's clock)
                    if (trigger.retry && step.killSpawned)
                    {
                        if (!run.waitLogged)
                            triggers15("waits for a " + CreatureName(step.killEntry) + " to respawn near it");
                        run.waitLogged = true;
                        return true;
                    }
                    return cannot("no " + CreatureName(step.killEntry) + " alive in sight to kill near " + giverName);
                }
                run.waitLogged = false;
                run.kill = target->GetGUID();  // a fight on the walk to the spot below is this kill too
            }
            // out of its reach of the giver: the bot draws it from a spot between them, half the reach from the
            // giver (ranged) or three quarters (melee: its aggro reach takes it over the rest), the fight comes to it
            if (step.radius > 0.0f && target->GetDistance(giver) > step.radius)
            {
                float const lure = step.radius * (PlayerbotAI::IsRanged(bot) ? 0.5f : 0.75f);
                float x = 0.0f, y = 0.0f, z = 0.0f;
                giver->GetNearPoint(bot, x, y, z, 0.0f, lure, giver->GetAngle(target));
                if (bot->GetExactDist2d(x, y) > CONTACT_DISTANCE)
                {
                    if (MoveTo(bot->GetMapId(), x, y, z) || bot->isMoving())
                        return true;
                    return cannot("no walk to the spot between " + giverName + " and " + target->GetName());
                }
            }
            if (bot->isMoving())
            {
                bot->StopMoving();
                return true;
            }
            context->GetValue<Unit*>("current target")->Set(target);
            bot->SetTarget(target->GetGUID());
            botAI->DoSpecificAction("attack", Event(), true);
            triggers15(Acore::StringFormat("attacks {} {:.1f} yd from it ({})", target->GetName(),
                                           target->GetDistance(giver), Describe(step)));
            return true;
        }
        case StepKind::Fight:
        {
            if (giver->IsAlive() && bot->IsValidAttackTarget(giver))
            {
                if (bot->GetDistance(giver) > sight)
                    return walkTo(INTERACTION_DISTANCE);
                run.fighting = true;
                context->GetValue<Unit*>("current target")->Set(giver);
                bot->SetTarget(giver->GetGUID());
                botAI->DoSpecificAction("attack", Event(), true);
                triggers15(Describe(step) + " (" + step.source + ")");
                return true;
            }
            // it is no foe (any more): the fight its script started is over, the flag follows
            run.fighting = false;
            return next();
        }
        case StepKind::Wait:
            if (!bot->IsWithinDistInMap(giver, INTERACTION_DISTANCE))
                return walkTo(INTERACTION_DISTANCE - 1.0f);
            triggers15(Describe(step) + " (" + step.source + ")");
            return next();
        case StepKind::Unsupported:
            return cannot(step.why + " (" + step.source + ")");
    }
    return false;
}
