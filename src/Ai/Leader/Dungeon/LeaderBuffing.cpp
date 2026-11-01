/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderBuffing.h"

#include "LeaderReports.h"
#include "LeaderEvents.h"
#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "AKTriggers.h"
#include "ANAddTactics.h"
#include "Bag.h"
#include "BotMoveEvents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "CoSTriggers.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DBCStores.h"
#include "Engine.h"
#include "EscortProtect.h"  // [b36_ohb] EscortGossipHeld
#include "EstimatedLifetimeValue.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "GDTriggers.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "RaidLeaderState.h"
#include "HoRTriggers.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MechShared.h"
#include "MgTShared.h"
#include "ClassicShared.h"  // [b32_smpull] Mograine's linked chapel trash
#include "MotionMaster.h"
#include "OCTriggers.h"
#include "MoveSpline.h"
#include "AttackerCountValues.h"
#include "MovementActions.h"
#include "LeaderOrders.h"  // [lead1] the tank's pull orders
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "LeaderKnowledge.h"
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
#include "LeaderPlan.h"         // [lead107] the leader's planner tick
#include "Pet.h"  // [lead38] the pet redirect
#include <source_location>  // [lead76]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "PullActions.h"  // [b39_strat] FightHold: the opening fight held on the use side of a key gate
#include "ChooseTargetActions.h"  // [b47_door] PullReadiness::GroupPreparing
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StuckWatchdog.h"
#include "DamageTaken.h"
#include "DungeonHazard.h"   // [lead16] the leader's hazard view
#include "LeaderActions.h"   // [lead19] the member guard model (LeaderMoveOrderAction::WalkRefusal)
#include "RtiValue.h"        // [lead16] the leader's CC assignments
#include "DungeonTactics.h"  // [lead15] the leader's cleave dodge (InFrontalCleave)
#include "InstanceScript.h"
#include "InstanceSaveMgr.h"
#include "SelfTravelActions.h"
#include "TemporarySummon.h"
#include "TerrainHazard.h"
#include "TestRunnerStrategy.h"
#include "Transport.h"
#include "UKTriggers.h"
#include "UPTriggers.h"
#include "Timer.h"
#include <chrono>  // [lead15] the leader cadence cost
#include <functional>
#include <optional>  // [lead18]
#include "WaypointMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSession.h"
#include "G3D/Vector4.h"  // [b32_brs] HopelessDetourLeg discs
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RouteTargets.h"
#include "RouteSteer.h"

namespace Leader::Dungeon
{
    bool HasAuraNamed(Player* bot, char const* text)
    {
        for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (info && info->SpellName[LOCALE_enUS] && strstr(info->SpellName[LOCALE_enUS], text))
                return true;
        }
        return false;
    }


    bool HasWellFed(Player* bot) { return HasAuraNamed(bot, "Well Fed"); }

    namespace
    {
        // Buffing before a pull (TickBuffing): every step ends on its condition; the old fixed timers (15 s buffs,
        // 2 s, 12 s food, 10 s ready check: 39 s before every boss, RFC 5B-1 Taragaman / Jergosh / Bazzalan) are
        // upper bounds only.
        constexpr uint32 BUFF_STEP_MAX_MS = 15 * IN_MILLISECONDS;
        constexpr uint32 BUFF_STEP_MIN_MS = 1500;                   // the bots' first ticks with the window open
        constexpr uint32 BUFF_IDLE_GRACE_MS = 3 * IN_MILLISECONDS;  // nobody buffs, a givable buff still missing
        constexpr uint32 FLASK_GCD_MS = 1600;                       // a flask used: its global cooldown before food
        constexpr uint32 FOOD_STEP_MIN_MS = IN_MILLISECONDS;        // the food just used shows its aura
        constexpr uint32 FOOD_STEP_MAX_MS = 12 * IN_MILLISECONDS;
        constexpr uint32 READY_STEP_MIN_MS = 1500;  // the ready check reaches the bots on their next ticks
        constexpr uint32 READY_STEP_MAX_MS = 10 * IN_MILLISECONDS;
        // [ready_reply_now] mirrors ReadyCheckAction: the bots answer a ready check when it reaches them, so the
        // harness waits READY_STEP_MIN_MS for the packet only (the 10 s "no answer from ..." of DOWNTIME_AUDIT).
        constexpr bool READY_REPLY_ON_RECEIPT = true;
        // [single_ready_check] User rule 2026-10-25: one ready check per boss. With the bots' raid leader layer
        // running the boss's own ready check (RaidLeader "pull hold"), the harness skips its step-4 check before a
        // boss (brd-n Gerstahn: buffing 30 s, then the leader's pull hold 22 s more with everyone at 100 %).
        constexpr bool HARNESS_SINGLE_READY_CHECK = true;
        // A whole buffing pass at most (the step caps add up to ~39 s; Stockade heroic 2026-10-25 counted 125 s idle
        // ending in buffing): past it the ready check ends the pass now.
        constexpr uint32 BUFF_TOTAL_MAX_MS = 30 * IN_MILLISECONDS;
        constexpr float BUFF_GROUP_RANGE = 40.0f;   // members farther from the leader neither give nor wait for buffs

        enum class BuffTargets : uint8
        {
            All,
            ManaUsers,
            Physical
        };

        // A buff the group gives when a member of the class knows any rank of one of `spells` at its level (a level
        // 15 paladin has no Kings, a level 15 priest no Shadow Protection); any aura named like `auras` counts.
        struct GroupBuffDef
        {
            uint8 casterClass;
            std::vector<uint32> spells;  // first ranks
            std::vector<char const*> auras;
            BuffTargets targets;
            char const* label;
        };

        std::vector<GroupBuffDef> const groupBuffs = {
            {CLASS_PRIEST, {1243}, {"Fortitude"}, BuffTargets::All, "Fortitude"},
            {CLASS_PRIEST, {14752}, {"Divine Spirit", "Prayer of Spirit"}, BuffTargets::All, "Divine Spirit"},
            {CLASS_PRIEST, {976}, {"Shadow Protection"}, BuffTargets::All, "Shadow Protection"},
            {CLASS_DRUID, {1126}, {"of the Wild"}, BuffTargets::All, "Mark of the Wild"},
            {CLASS_MAGE, {1459}, {"Intellect", "Brilliance"}, BuffTargets::ManaUsers, "Arcane Intellect"},
            // One blessing per paladin and target, whichever its blessing strategy gives.
            {CLASS_PALADIN, {19740, 19742, 20217, 20911}, {"Blessing of"}, BuffTargets::All, "a Blessing"},
            {CLASS_WARRIOR, {6673, 469}, {"Battle Shout", "Commanding Shout", "Blessing of Might"}, BuffTargets::All,
             "Battle/Commanding Shout"},
            {CLASS_DEATH_KNIGHT, {57330}, {"Horn of Winter", "Strength of Earth"}, BuffTargets::Physical,
             "Horn of Winter"},
        };

        bool KnowsAnyRank(Player* caster, std::vector<uint32> const& firstRanks)
        {
            for (uint32 const first : firstRanks)
                for (uint32 spell = first; spell; spell = sSpellMgr->GetNextSpellInChain(spell))
                    if (caster->HasSpell(spell))
                        return true;
            return false;
        }

        bool InBuffRange(Player* bot, Player* leader)
        {
            return bot->IsAlive() && bot->IsInWorld() && bot->GetMap() == leader->GetMap() && !bot->IsInCombat() &&
                   bot->GetDistance(leader) <= BUFF_GROUP_RANGE;
        }

        // "<member> <buff>, ..." for every buff the group can give that a member lacks; empty: all present.
        std::string MissingGivableBuffs(LeaderGroup& group, std::vector<Player*> const& players, Player* leader)
        {
            std::vector<LeaderGroup::MemberInfo> const members = group.Members();
            std::string missing;
            for (GroupBuffDef const& def : groupBuffs)
            {
                bool givable = false;
                for (Player* caster : players)
                    if (!givable && caster->getClass() == def.casterClass && InBuffRange(caster, leader) &&
                        KnowsAnyRank(caster, def.spells))
                        givable = true;
                if (!givable)
                    continue;

                for (Player* bot : players)
                {
                    LeaderGroup::MemberInfo const* member = nullptr;
                    for (LeaderGroup::MemberInfo const& candidate : members)
                        if (!member && candidate.guid == bot->GetGUID())
                            member = &candidate;
                    if (!member || !InBuffRange(bot, leader))
                        continue;
                    if ((def.targets == BuffTargets::ManaUsers && !member->usesMana) ||
                        (def.targets == BuffTargets::Physical && !member->physical))
                        continue;
                    bool has = false;
                    for (char const* aura : def.auras)
                        has = has || HasAuraNamed(bot, aura);
                    if (!has)
                        missing += (missing.empty() ? "" : ", ") + bot->GetName() + " " + def.label;
                }
            }
            return missing;
        }

        // Who still buffs: casting, its rebuff window open (ForceRebuffState closes it once no buff trigger fires),
        // or walking (to a buff target out of range).
        std::string BuffWorkers(std::vector<Player*> const& players, Player* leader)
        {
            std::string workers;
            for (Player* bot : players)
            {
                if (!InBuffRange(bot, leader))
                    continue;
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                char const* what = nullptr;
                if (bot->IsNonMeleeSpellCast(false))
                    what = "casting";
                else if (botAI && botAI->forceRebuff.IsPending())
                    what = "rebuff window open";
                else if (bot != leader && bot->isMoving())
                    what = "moving";
                if (what)
                    workers += (workers.empty() ? "" : ", ") + bot->GetName() + " " + what;
            }
            return workers;
        }

        // Eating for Well Fed: sitting with food and no Well Fed yet.
        std::string Eaters(std::vector<Player*> const& players, Player* leader)
        {
            std::string eaters;
            for (Player* bot : players)
                if (InBuffRange(bot, leader) && RestState::IsEating(bot) && !HasWellFed(bot))
                    eaters += (eaters.empty() ? "" : ", ") + bot->GetName();
            return eaters;
        }

        // A member answers the ready check by itself: at once, or with the "force rebuff" strategy when the rebuff
        // window the check opened (Begin(true): the reply flag, kept after End) closes (ReadyReplyAction). The
        // buffing step's Begin(false) cleared the flag of a closed window before.
        bool AnsweredReadyCheck(Player* bot)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            // [ready_reply_now] the bot answers on receipt (ReadyCheckAction), its rebuff pass runs after the answer
            if (READY_REPLY_ON_RECEIPT)
                return true;
            if (!botAI || !sPlayerbotAIConfig.ForceRebuffOnReadyCheck ||
                !botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT))
                return true;
            return botAI->forceRebuff.ShouldReplyToReadyCheck() && !botAI->forceRebuff.IsPending();
        }

        // The current buffing step of a party: what it waited for, the idle clock.
        struct BuffWait
        {
            std::string waitedFor;
            uint32 idleSinceMs = 0;
            bool consumableUsed = false;  // step 1 -> 2: a flask was used; step 2 -> 3: food was used
        };
        std::unordered_map<uint32, BuffWait> buffWaits;  // per instance, world thread only
    }

    void TickBuffing(LeaderGroup& group, std::vector<Player*> const& players, Player* leader)
    {
        GroupRouteState& route = group.Route();
        uint32 const now = getMSTime();
        group.ClearWaypoint(group.LeaderGuid());
        // A step done goes on to the next one in the same tick while that one has nothing to wait for (no flask, no
        // food): nothing missing takes about 4 s.
        for (uint32 pass = 0; pass < 4 && route.needBuff; ++pass)
        {
            if (route.buffStep > 0 && route.buffStep < 4 && getMSTimeDiff(route.buffSinceMs, now) > BUFF_TOTAL_MAX_MS)
            {
                LOG_INFO("playerbots", "[buffing] {}: pass over {} s at step {}: to the ready check end", group.Run().runId,
                         BUFF_TOTAL_MAX_MS / IN_MILLISECONDS, uint32(route.buffStep));
                route.buffStep = 4;
                route.buffStepMs = now - READY_STEP_MAX_MS - 1;
            }
            uint8 const stepBefore = route.buffStep;
            uint32 const stepMs = getMSTimeDiff(route.buffStepMs, now);
            BuffWait& wait = buffWaits[group.InstanceId()];

            // The step is over: its line ("[buffing] step N done after X s: why"), the next step's clock.
            auto const stepDone = [&](uint8 next, std::string const& why)
            {
                float const sec = stepMs / 1000.0f;
                LOG_INFO("playerbots", "[buffing] step {} done after {:.1f} s: {}{} ({}, {})", uint32(route.buffStep),
                         sec, why, wait.waitedFor.empty() ? std::string() : "; waited for " + wait.waitedFor,
                         group.Run().runId, route.buffReason);
                group.LogEvent("buff_step",
                         JsonObject().Num("step", route.buffStep).Real("sec", sec).Str("why", why)
                             .Str("waitedFor", wait.waitedFor).Str("reason", route.buffReason));
                route.buffStep = next;
                route.buffStepMs = now;
                bool const consumableUsed = wait.consumableUsed;
                wait = BuffWait();
                wait.consumableUsed = consumableUsed;
            };
            auto const waitingFor = [&wait](std::string const& what)
            {
                if (!what.empty())
                    wait.waitedFor = what;
            };

            switch (route.buffStep)
            {
                case 0:
                    group.BuffPassSetup(players);  // [lead107 stage 5] the test setup of a buffing pass
                    for (Player* bot : players)
                        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                            if (bot->IsAlive() && !bot->IsInCombat() &&
                                botAI->HasStrategy("force rebuff", BOT_STATE_NON_COMBAT))
                                botAI->forceRebuff.Begin(false);
                    group.LogEvent("buffing", JsonObject().Str("reason", route.buffReason));
                    route.lastEvent = "buffing: " + route.buffReason;
                    route.buffStep = 1;
                    route.buffStepMs = now;
                    route.buffSinceMs = now;
                    wait = BuffWait();
                    break;
                case 1:
                {
                    // Buffs: nobody casts, walks to buff or keeps its rebuff window open, and every buff the group can
                    // give is on every member it is for. Nobody buffing while one is missing ends it after the grace.
                    std::string const workers = BuffWorkers(players, leader);
                    std::string const missing = MissingGivableBuffs(group, players, leader);
                    waitingFor(workers.empty() ? missing : workers);
                    if (!workers.empty())
                        wait.idleSinceMs = 0;
                    else if (!wait.idleSinceMs)
                        wait.idleSinceMs = now;
                    std::string why;
                    if (stepMs >= BUFF_STEP_MAX_MS)
                        why = "upper bound " + std::to_string(BUFF_STEP_MAX_MS / IN_MILLISECONDS) + " s" +
                              (workers.empty() ? std::string() : ", still buffing: " + workers) +
                              (missing.empty() ? std::string() : ", missing: " + missing);
                    // [ready_instant] User 2026-10-25 («ботам зачем проверять готовность? просто стоять и тупить»):
                    // nothing missing and nobody buffing ends the step at once, no BUFF_STEP_MIN_MS pause
                    else if (workers.empty() && missing.empty())
                        why = "every buff the group can give is present";
                    else if (stepMs >= BUFF_STEP_MIN_MS && workers.empty() &&
                             getMSTimeDiff(wait.idleSinceMs, now) >= BUFF_IDLE_GRACE_MS)
                        why = "nobody buffs any more, missing: " + missing;
                    if (why.empty())
                        break;
                    stepDone(2, why);
                    for (Player* bot : players)
                        wait.consumableUsed = group.UseConsumables(bot, false) || wait.consumableUsed;
                    break;
                }
                case 2:
                {
                    // A flask's global cooldown, then the food.
                    if (wait.consumableUsed && stepMs < FLASK_GCD_MS)
                    {
                        waitingFor("flask global cooldown");
                        break;
                    }
                    uint32 eating = 0;
                    for (Player* bot : players)
                        eating += group.UseConsumables(bot, true) ? 1 : 0;
                    stepDone(3, eating ? std::to_string(eating) + " member(s) sat down to eat"
                                       : std::string("Well Fed on everyone, no food needed"));
                    wait.consumableUsed = eating != 0;  // step 3: somebody eats
                    break;
                }
                case 3:
                {
                    // Food: only while somebody actually eats and has no Well Fed yet.
                    std::string const eaters = Eaters(players, leader);
                    waitingFor(eaters.empty() ? std::string() : "eating: " + eaters);
                    std::string why;
                    if (stepMs >= FOOD_STEP_MAX_MS)
                        why = "upper bound " + std::to_string(FOOD_STEP_MAX_MS / IN_MILLISECONDS) + " s" +
                              (eaters.empty() ? std::string() : ", still eating: " + eaters);
                    else if (eaters.empty() && (!wait.consumableUsed || stepMs >= FOOD_STEP_MIN_MS))
                        why = wait.consumableUsed ? "Well Fed on everyone who ate" : "nobody eats";
                    if (why.empty())
                        break;
                    stepDone(4, why);
                    if (HARNESS_SINGLE_READY_CHECK && route.buffReason.rfind("before ", 0) == 0 &&
                        RaidLeader::FindCoordinator(leader))
                        group.LogEvent("single_ready_check",
                                 JsonObject().Str("reason", route.buffReason).Str("by", "raid leader"));
                    else if (leader->GetGroup() && leader->GetGroup()->IsLeader(leader->GetGUID()))
                    {
                        WorldPacket packet(MSG_RAID_READY_CHECK);
                        leader->GetSession()->HandleRaidReadyCheckOpcode(packet);
                        group.LogEvent("ready_check", JsonObject().Str("reason", route.buffReason));
                    }
                    break;
                }
                default:
                {
                    // Ready check: every member answered (AnsweredReadyCheck).
                    std::string unanswered;
                    for (Player* bot : players)
                        if (bot != leader && InBuffRange(bot, leader) && !AnsweredReadyCheck(bot))
                            unanswered += (unanswered.empty() ? "" : ", ") + bot->GetName();
                    waitingFor(unanswered.empty() ? std::string() : "ready answers: " + unanswered);
                    std::string why;
                    if (HARNESS_SINGLE_READY_CHECK && route.buffReason.rfind("before ", 0) == 0 &&
                        RaidLeader::FindCoordinator(leader))
                        why = "the raid leader runs the boss's ready check (single_ready_check)";
                    else if (stepMs >= READY_STEP_MAX_MS)
                        why = "upper bound " + std::to_string(READY_STEP_MAX_MS / IN_MILLISECONDS) + " s" +
                              (unanswered.empty() ? std::string() : ", no answer from " + unanswered);
                    // [ready_instant] the check is a check, not a pause: the bots answer on receipt
                    // (READY_REPLY_ON_RECEIPT), so with every member in range and nobody below its own rest line the
                    // group goes on in the same tick (else the READY_STEP_MIN_MS end below, as before)
                    else if (READY_REPLY_ON_RECEIPT && unanswered.empty() &&
                             std::none_of(players.begin(), players.end(),
                                          [leader](Player* bot)
                                          {
                                              return bot != leader &&
                                                     (!bot->IsAlive() || !InBuffRange(bot, leader) ||
                                                      RestState::WantsDrink(bot) || RestState::WantsFood(bot));
                                          }))
                        why = "nothing missing: buffs present, everyone in range and above its rest line";
                    else if (stepMs >= READY_STEP_MIN_MS && unanswered.empty())
                        why = "every member answered the ready check";
                    if (why.empty())
                        break;
                    stepDone(0, why);
                    buffWaits.erase(group.InstanceId());
                    {
                        JsonArray state;
                        for (Player* bot : players)
                            state.Raw(group.MemberBuffJson(bot));
                        group.LogEvent("buffed",
                                 JsonObject().Str("reason", route.buffReason).Raw("members", state.Build()));
                    }
                    route.needBuff = false;
                    route.buffStep = 0;
                    group.NoteProgress(now);
                    group.NoteActivity(LeaderGroup::Activity::Traveling);
                    break;
                }
            }
            if (route.buffStep == stepBefore)
                break;
        }
    }
}
