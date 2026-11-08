/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgQuestUse.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

#include "ConditionMgr.h"
#include "Creature.h"
#include "GameObject.h"
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
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    using NewRpgQuestUse::Plan;
    using NewRpgQuestUse::TargetKind;

    // a spell focus spot is stood at this close: a spell's own script may check closer than the focus radius
    // (q9452 Red Snapper: spell_q9452_cast_net wants the school within 3 yd, its focus radius is 5)
    constexpr float FOCUS_STAND = 2.0f;
    constexpr float NEAR_STAND = 10.0f;  // a Near cast: this close to the creature whose script credits the aura

    // Quests whose credit the data does not name (a C++ script). Each entry is checked against its script name, so a
    // core that changes the script drops the exception instead of guessing.
    struct ScriptException
    {
        uint32 questId;
        uint32 creature;
        char const* scriptName;
        std::vector<uint32> spells;  // the spell ranks the script accepts
        char const* why;
    };
    // q9283 Rescue the Survivors!: npc_draenei_survivor::SpellHit takes any Gift of the Naaru (SpellFamilyFlags[2]
    // 0x80000000, one rank per class) and calls TalkedToCreature 5 s later, while the heal's aura still runs
    std::vector<ScriptException> const EXCEPTIONS = {
        {9283, 16483, "npc_draenei_survivor", {28880, 59542, 59543, 59544, 59545, 59547, 59548},
         "C++ npc_draenei_survivor credits a Gift of the Naaru hit"},
    };

    struct SpellCredit
    {
        uint32 owner;   // the creature whose script runs
        uint32 credit;  // the kill credit entry it gives
    };

    std::unordered_map<uint32, std::vector<Plan>> plans;  // quest id -> plans
    std::once_flag built;

    void TargetConditionEntries(SpellInfo const* info, std::vector<uint32>& out)
    {
        auto const read = [&out](std::list<Condition*> const& conditions)
        {
            for (Condition const* cond : conditions)
                if (cond->ConditionType == CONDITION_OBJECT_ENTRY_GUID && cond->ConditionValue1 == TYPEID_UNIT &&
                    cond->ConditionValue2 && !cond->NegativeCondition)
                    out.push_back(cond->ConditionValue2);
        };
        read(sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL, info->Id));
        for (SpellEffectInfo const& effect : info->Effects)
            if (effect.ImplicitTargetConditions)
                read(*effect.ImplicitTargetConditions);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void BuildPlans()
    {
        // SmartAI: spell -> the creatures whose spell hit gives a kill credit (the hit event and its linked events),
        // and spell -> the credits given to players under that spell's aura (SMART_TARGET_PLAYER_WITH_AURA)
        std::unordered_map<uint32, std::vector<SpellCredit>> hitCredits;
        std::unordered_map<uint32, std::vector<SpellCredit>> auraCredits;
        for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
        {
            SmartAIEventList const events = sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE);
            if (events.empty())
                continue;
            std::unordered_map<uint32, SmartScriptHolder const*> byId;
            for (SmartScriptHolder const& e : events)
                byId[e.event_id] = &e;
            for (SmartScriptHolder const& e : events)
            {
                if (e.GetActionType() == SMART_ACTION_CALL_KILLEDMONSTER && e.action.killedMonster.creature &&
                    e.GetTargetType() == SMART_TARGET_PLAYER_WITH_AURA && e.target.playerWithAura.spellId &&
                    !e.target.playerWithAura.negation)
                    auraCredits[e.target.playerWithAura.spellId].push_back({entry, e.action.killedMonster.creature});
                if (e.GetEventType() != SMART_EVENT_SPELLHIT || !e.event.spellHit.spell)
                    continue;
                std::set<uint32> seen;
                for (SmartScriptHolder const* link = &e; link && seen.insert(link->event_id).second;)
                {
                    if (link->GetActionType() == SMART_ACTION_CALL_KILLEDMONSTER && link->action.killedMonster.creature)
                        hitCredits[e.event.spellHit.spell].push_back({entry, link->action.killedMonster.creature});
                    auto const next = link->link ? byId.find(link->link) : byId.end();
                    link = next != byId.end() && next->second->GetEventType() == SMART_EVENT_LINK ? next->second
                                                                                                  : nullptr;
                }
            }
        }
        std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> focusObjects;  // focus id -> (entry, radius)
        for (auto const& [entry, goTemplate] : *sObjectMgr->GetGameObjectTemplates())
            if (goTemplate.type == GAMEOBJECT_TYPE_SPELL_FOCUS && goTemplate.spellFocus.focusId)
                focusObjects[goTemplate.spellFocus.focusId].push_back({entry, float(goTemplate.spellFocus.dist)});

        auto const withFocus = [&focusObjects](SpellInfo const* info, Plan& plan)
        {
            auto const found = focusObjects.find(info->RequiresSpellFocus);
            if (!info->RequiresSpellFocus || found == focusObjects.end())
                return false;
            plan.kind = TargetKind::Focus;
            for (auto const& [entry, radius] : found->second)
            {
                plan.objects.push_back(entry);
                plan.focusRadius = plan.focusRadius ? std::min(plan.focusRadius, radius) : radius;
            }
            return true;
        };

        size_t total = 0;
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            std::vector<Plan> found;
            // the tool: the item the quest hands out at accept, its on-use spells
            if (ItemTemplate const* tool = quest->GetSrcItemId() ? sObjectMgr->GetItemTemplate(quest->GetSrcItemId())
                                                                  : nullptr)
                for (auto const& use : tool->Spells)
                {
                    SpellInfo const* info = use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE
                                                ? sSpellMgr->GetSpellInfo(uint32(use.SpellId))
                                                : nullptr;
                    if (!info)
                        continue;
                    std::vector<uint32> targets;
                    TargetConditionEntries(info, targets);
                    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    {
                        int32 const credit = quest->RequiredNpcOrGo[i];
                        if (credit <= 0 || !quest->RequiredNpcOrGoCount[i])
                            continue;
                        Plan plan;
                        plan.itemId = tool->ItemId;
                        plan.spells = {info->Id};
                        plan.objective = i;
                        for (SpellCredit const& hit : hitCredits[info->Id])
                            if (hit.credit == uint32(credit))
                                plan.creatures.push_back(hit.owner);
                        if (!plan.creatures.empty())
                        {
                            plan.hitCredit = true;
                            plan.source = "SmartAI spell hit credit";
                            found.push_back(plan);
                            continue;
                        }
                        for (SpellCredit const& aura : auraCredits[info->Id])
                            if (aura.credit == uint32(credit))
                                plan.creatures.push_back(aura.owner);
                        if (!plan.creatures.empty())
                        {
                            if (!withFocus(info, plan))
                                plan.kind = TargetKind::Near;
                            plan.source = "SmartAI credit to players under the aura";
                            found.push_back(plan);
                            continue;
                        }
                        // the spell's own kill credit effect: on its condition targets, else at its focus
                        bool killCredit = false;
                        for (SpellEffectInfo const& effect : info->Effects)
                            killCredit = killCredit || ((effect.Effect == SPELL_EFFECT_KILL_CREDIT ||
                                                         effect.Effect == SPELL_EFFECT_KILL_CREDIT2) &&
                                                        effect.MiscValue == credit);
                        if (!killCredit)
                            continue;
                        plan.hitCredit = true;
                        plan.source = "spell kill credit effect";
                        plan.creatures = targets;
                        if (!plan.creatures.empty() || withFocus(info, plan))
                            found.push_back(plan);
                    }
                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    {
                        uint32 const item = quest->RequiredItemId[i];
                        if (!item || item == tool->ItemId || !quest->RequiredItemCount[i])
                            continue;
                        bool creates = false;
                        for (SpellEffectInfo const& effect : info->Effects)
                            creates = creates || (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType == item);
                        SpellScriptsBounds const scripts = sObjectMgr->GetSpellScriptsBounds(info->Id);
                        // a spell script decides the loot (q9452: 66 % a Red Snapper, else an Angry Murloc)
                        if (!creates && scripts.first == scripts.second)
                            continue;
                        Plan plan;
                        plan.itemId = tool->ItemId;
                        plan.spells = {info->Id};
                        plan.objective = uint8(QUEST_OBJECTIVES_COUNT + i);
                        plan.creatures = targets;
                        plan.hitCredit = false;
                        plan.source = creates ? "spell creates the item" : "spell script gives the item";
                        if (!plan.creatures.empty() || withFocus(info, plan))
                            found.push_back(plan);
                    }
                }
            for (ScriptException const& ex : EXCEPTIONS)
            {
                if (ex.questId != questId)
                    continue;
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(ex.creature);
                if (!tmpl || sObjectMgr->GetScriptName(tmpl->ScriptID) != ex.scriptName)
                {
                    LOG_INFO("playerbots", "[quest] use plan q{}: exception dropped, creature {} no longer runs {}",
                             questId, ex.creature, ex.scriptName);
                    continue;
                }
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredNpcOrGo[i] == int32(ex.creature) && quest->RequiredNpcOrGoCount[i])
                    {
                        Plan plan;
                        plan.spells = ex.spells;
                        plan.objective = i;
                        plan.creatures = {ex.creature};
                        plan.source = ex.why;
                        found.push_back(plan);
                    }
            }
            if (found.empty())
                continue;
            total += found.size();
            plans[questId] = std::move(found);
        }
        LOG_INFO("playerbots", "[quest] use plans: {} objectives of {} quests are done by using an item or a spell",
                 total, plans.size());
    }

    uint32 FirstKnown(Player* bot, Plan const& plan)
    {
        if (plan.itemId)
            return plan.spells.empty() ? 0 : plan.spells.front();
        for (uint32 const spell : plan.spells)
            if (!bot || bot->HasSpell(spell))
                return spell;
        return 0;
    }

    uint32 ObjectiveCount(Player* bot, Quest const* quest, uint8 objective)
    {
        auto const found = bot->getQuestStatusMap().find(quest->GetQuestId());
        if (found == bot->getQuestStatusMap().end())
            return 0;
        if (objective < QUEST_OBJECTIVES_COUNT)
            return found->second.CreatureOrGOCount[objective];
        return found->second.ItemCount[objective - QUEST_OBJECTIVES_COUNT];
    }

    uint32 ObjectiveNeed(Quest const* quest, uint8 objective)
    {
        return objective < QUEST_OBJECTIVES_COUNT ? quest->RequiredNpcOrGoCount[objective]
                                                  : quest->RequiredItemCount[objective - QUEST_OBJECTIVES_COUNT];
    }

    // a cast made, its credit not seen yet
    struct Pending
    {
        uint32 questId = 0;
        uint8 objective = 0;
        uint32 itemId = 0;
        uint32 spellId = 0;
        uint32 entry = 0;
        ObjectGuid target;
        uint32 before = 0;
        bool hitCredit = false;
    };
    std::mutex useLock;
    std::unordered_map<ObjectGuid::LowType, Pending> pending;                                  // bot -> its cast
    std::unordered_map<ObjectGuid::LowType, std::set<std::pair<uint32, ObjectGuid>>> tried;  // bot -> (quest, target)

    bool WasTried(Player* bot, uint32 questId, ObjectGuid guid)
    {
        std::lock_guard<std::mutex> guard(useLock);
        auto const found = tried.find(bot->GetGUID().GetCounter());
        return found != tried.end() && found->second.count({questId, guid});
    }
    void MarkTried(Player* bot, uint32 questId, ObjectGuid guid, bool on)
    {
        std::lock_guard<std::mutex> guard(useLock);
        if (on)
            tried[bot->GetGUID().GetCounter()].insert({questId, guid});
        else if (auto const found = tried.find(bot->GetGUID().GetCounter()); found != tried.end())
            found->second.erase({questId, guid});
    }

    std::string ToolText(uint32 itemId, uint32 spellId)
    {
        return itemId ? "item " + std::to_string(itemId) + " (spell " + std::to_string(spellId) + ")"
                      : "spell " + std::to_string(spellId);
    }
}

void NewRpgQuestUse::Build() { std::call_once(built, BuildPlans); }

NewRpgQuestUse::Plan const* NewRpgQuestUse::PlanFor(Player* bot, Quest const* quest, uint8 objective)
{
    if (!quest)
        return nullptr;
    Build();
    auto const found = plans.find(quest->GetQuestId());
    if (found == plans.end())
        return nullptr;
    for (Plan const& plan : found->second)
    {
        if (plan.objective != objective || !FirstKnown(bot, plan))
            continue;
        // the tool is handed out at accept: before that the bot cannot hold it, after it, it must
        if (bot && plan.itemId && bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE &&
            !bot->HasItemCount(plan.itemId, 1))
            continue;
        return &plan;
    }
    return nullptr;
}

bool NewRpgQuestUse::CoversToolObjectives(Player* bot, Quest const* quest)
{
    if (!quest)
        return false;
    bool any = false;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i])
        {
            if (!PlanFor(bot, quest, i))
                return false;
            any = true;
        }
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i] && quest->RequiredItemCount[i] &&
            quest->RequiredItemId[i] != quest->GetSrcItemId())
        {
            if (!PlanFor(bot, quest, uint8(QUEST_OBJECTIVES_COUNT + i)))
                return false;
            any = true;
        }
    return any;
}

void NewRpgQuestUse::SourceEntries(Quest const* quest, uint32 objective, std::vector<uint32>& creatures,
                                   std::vector<uint32>& objects)
{
    Plan const* plan = objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT
                           ? PlanFor(nullptr, quest, uint8(objective))
                           : nullptr;
    if (!plan)
        return;
    if (plan->kind == TargetKind::Focus)
        objects.insert(objects.end(), plan->objects.begin(), plan->objects.end());
    else
        creatures.insert(creatures.end(), plan->creatures.begin(), plan->creatures.end());
}

// [questitems] the credit of the last cast: seen on the objective's counter, awaited while the cast runs or its aura
// (from this bot) is on the bot or the target - the game's own clock, never a timer
bool NewRpgDoQuestAction::SettleQuestUse()
{
    Pending cast;
    {
        std::lock_guard<std::mutex> guard(useLock);
        auto const found = pending.find(bot->GetGUID().GetCounter());
        if (found == pending.end())
            return false;
        cast = found->second;
    }
    Quest const* quest = sObjectMgr->GetQuestTemplate(cast.questId);
    auto const drop = [this]()
    {
        std::lock_guard<std::mutex> guard(useLock);
        pending.erase(bot->GetGUID().GetCounter());
    };
    if (!quest || bot->GetQuestStatus(cast.questId) == QUEST_STATUS_NONE)
    {
        drop();
        return false;
    }
    uint32 const now = ObjectiveCount(bot, quest, cast.objective);
    uint32 const need = ObjectiveNeed(quest, cast.objective);
    std::string const tool = ToolText(cast.itemId, cast.spellId);
    if (now > cast.before || bot->GetQuestStatus(cast.questId) == QUEST_STATUS_COMPLETE)
    {
        LOG_INFO("playerbots", "[quest] use {} q{} {} on {} result credited {}/{}", bot->GetName(), cast.questId,
                 tool, cast.entry, now, need);
        drop();
        return false;
    }
    if (bot->IsInCombat())
        return false;  // the fight goes first, the cast's credit is judged after it
    Unit* target = cast.target.IsUnit() ? ObjectAccessor::GetUnit(*bot, cast.target) : nullptr;
    if (bot->IsNonMeleeSpellCast(false) || bot->HasAura(cast.spellId, bot->GetGUID()) ||
        (target && target->HasAura(cast.spellId, bot->GetGUID())))
        return true;
    std::string const state = !cast.target.IsUnit() ? "object"
                              : !target             ? "target gone"
                              : !target->IsAlive()  ? "target dead"
                                                    : "target alive, entry " + std::to_string(target->GetEntry());
    LOG_INFO("playerbots", "[quest] use {} q{} {} on {} result no credit: the cast ended, objective {} at {}/{} ({})",
             bot->GetName(), cast.questId, tool, cast.entry, cast.objective, now, need, state);
    drop();
    if (cast.hitCredit)
    {
        // the data credits the hit itself: the quest's credit is not where the data says
        std::string const why = Acore::StringFormat("{} on {} gave no credit at the hit ({})", tool, cast.entry, state);
        LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: {}", bot->GetName(), bot->GetLevel(), cast.questId,
                 why);
        NotePostpone(bot, cast.questId, why);
        botAI->lowPriorityQuest.insert(cast.questId);
        botAI->rpgInfo.ChangeToIdle();
    }
    return true;
}

// [questitems] the use step of an objective with a plan: the nearest untried target in sight, walked to within range
// and sight, the game's cast check, the item used (CMSG_USE_ITEM, as a player) or the spell cast
bool NewRpgDoQuestAction::UseQuestTool(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest || bot->IsInCombat() || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
        return false;
    {
        std::lock_guard<std::mutex> guard(useLock);
        if (pending.count(bot->GetGUID().GetCounter()))
            return false;  // SettleQuestUse judges it first
    }
    float const sight = sPlayerbotAIConfig.SightDistance;
    for (uint8 objective = 0; objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT; ++objective)
    {
        uint32 const need = objective < QUEST_OBJECTIVES_COUNT
                                ? (quest->RequiredNpcOrGo[objective] ? ObjectiveNeed(quest, objective) : 0)
                                : (quest->RequiredItemId[objective - QUEST_OBJECTIVES_COUNT]
                                       ? ObjectiveNeed(quest, objective)
                                       : 0);
        uint32 const before = ObjectiveCount(bot, quest, objective);
        if (!need || before >= need)
            continue;
        Plan const* plan = NewRpgQuestUse::PlanFor(nullptr, quest, objective);
        if (!plan)
            continue;
        uint32 const spellId = FirstKnown(bot, *plan);
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        Item* item = plan->itemId ? bot->GetItemByEntry(plan->itemId) : nullptr;
        if (!info || (plan->itemId && !item))
        {
            std::string const why = !info ? "it knows none of the spells " + ToolText(0, plan->spells.front())
                                          : "the tool item " + std::to_string(plan->itemId) + " is not in its bags";
            LOG_INFO("playerbots", "[quest] use {} q{} {} on - result cannot: {}", bot->GetName(), questId,
                     ToolText(plan->itemId, plan->spells.front()), why);
            LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: {}", bot->GetName(), bot->GetLevel(), questId,
                     why);
            NotePostpone(bot, questId, why);
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        std::string const tool = ToolText(plan->itemId, spellId);

        // the nearest target in sight not tried yet; a tried one seen dead or despawned comes back fresh at respawn
        WorldObject* target = nullptr;
        float best = sight;
        auto const consider = [&](WorldObject* object, bool usable)
        {
            if (!usable)
            {
                MarkTried(bot, questId, object->GetGUID(), false);
                return;
            }
            float const dist = bot->GetDistance(object);
            if (dist < best && !WasTried(bot, questId, object->GetGUID()))
            {
                best = dist;
                target = object;
            }
        };
        if (plan->kind == NewRpgQuestUse::TargetKind::Focus)
        {
            std::list<GameObject*> objects;
            bot->GetGameObjectListWithEntryInGrid(objects, plan->objects, sight);
            for (GameObject* go : objects)
                consider(go, go->isSpawned());
        }
        else
        {
            std::list<Creature*> creatures;
            bot->GetCreatureListWithEntryInGrid(creatures, plan->creatures, sight);
            for (Creature* creature : creatures)
                consider(creature, creature->IsAlive() && (!bot->IsValidAttackTarget(creature) ||
                                                           !creature->IsInCombat() || creature->IsInCombatWith(bot)));
        }
        if (!target)
            continue;

        Unit* unit = plan->kind == NewRpgQuestUse::TargetKind::Creature ? target->ToUnit() : nullptr;
        bool const hostile = unit && bot->IsValidAttackTarget(unit);
        float const stand = plan->kind == NewRpgQuestUse::TargetKind::Focus
                                ? std::min(plan->focusRadius, FOCUS_STAND)
                            : plan->kind == NewRpgQuestUse::TargetKind::Near
                                ? NEAR_STAND
                                : std::max(info->GetMaxRange(!hostile, bot) - 1.0f, 1.0f);
        if (bot->GetDistance(target) > stand || !bot->IsWithinLOSInMap(target))
        {
            // in range but out of sight: closer
            float const to = bot->GetDistance(target) > stand ? stand * 0.8f : std::min(stand, FOCUS_STAND * 2.0f);
            if (MoveWorldObjectTo(target->GetGUID(), to) || bot->isMoving())
                return true;
            MarkTried(bot, questId, target->GetGUID(), true);
            LOG_INFO("playerbots", "[quest] use {} q{} {} on {} result no walk to it ({:.0f} yd, {})", bot->GetName(),
                     questId, tool, target->GetEntry(), bot->GetDistance(target),
                     bot->IsWithinLOSInMap(target) ? "in sight" : "out of sight");
            return true;
        }
        if (bot->isMoving())
        {
            bot->StopMoving();
            return true;
        }
        if (unit && unit != bot)
            bot->SetFacingToObject(unit);

        PlayerbotAI::CastCheck const check = botAI->CheckCastNow(spellId, unit ? unit : bot, item);
        if (!check.Castable())
        {
            if (!check.Refused())
                return true;  // a passing state (the global cooldown, a cast under way): the next tick casts
            MarkTried(bot, questId, target->GetGUID(), true);
            LOG_INFO("playerbots", "[quest] use {} q{} {} on {} result refused: {}", bot->GetName(), questId, tool,
                     target->GetEntry(), PlayerbotAI::CastResultName(check.refusal));
            return true;
        }

        bool cast = false;
        if (item)
        {
            WorldPacket packet(CMSG_USE_ITEM);
            packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot()) << uint8(1) << spellId << item->GetGUID()
                   << uint32(0) << uint8(0);
            if (unit)
                packet << uint32(TARGET_FLAG_UNIT) << unit->GetGUID().WriteAsPacked();
            else
                packet << uint32(TARGET_FLAG_NONE);
            bot->GetSession()->HandleUseItemOpcode(packet);
            cast = true;
        }
        else
            cast = botAI->CastSpell(spellId, unit ? unit : bot);
        if (!cast)
        {
            MarkTried(bot, questId, target->GetGUID(), true);
            LOG_INFO("playerbots", "[quest] use {} q{} {} on {} result cast failed", bot->GetName(), questId, tool,
                     target->GetEntry());
            return true;
        }
        // each target is used once; one seen dead or despawned later (a school a net spent) is fresh at its respawn
        MarkTried(bot, questId, target->GetGUID(), true);
        {
            std::lock_guard<std::mutex> guard(useLock);
            Pending& entry = pending[bot->GetGUID().GetCounter()];
            entry = {questId, objective, plan->itemId, spellId, target->GetEntry(), target->GetGUID(), before,
                     plan->hitCredit};
        }
        LOG_INFO("playerbots", "[quest] use {} q{} {} on {} result cast ({}, {:.1f} yd, objective {} at {}/{})",
                 bot->GetName(), questId, tool, target->GetEntry(), plan->source, bot->GetDistance(target), objective,
                 before, need);
        return true;
    }
    return false;
}
