/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderBuffing.h"
#include "LeaderConsumables.h"  // [lead344]
#include "LeaderRest.h"  // CountRestItems
#include "LeaderPrepull.h"  // [lead270]
#include "LeaderSupport.h"  // [lead403]

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
#include "PaladinBlessingAction.h"    // [lead982] the blessing planner
#include "PaladinBlessingRegistry.h"  // [lead982] HighestKnownBlessingRank
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

    bool HasAuraLasting(Player* bot, char const* text, int32 needMs)
    {
        for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || !info->SpellName[LOCALE_enUS] || !strstr(info->SpellName[LOCALE_enUS], text))
                continue;
            Aura const* aura = aurApp ? aurApp->GetBase() : nullptr;
            int32 const left = aura ? aura->GetDuration() : -1;
            if (left < 0 || left >= needMs)  // -1: no end
                return true;
        }
        return false;
    }

    namespace
    {
        // Buffing before a pull (TickBuffing): every step ends on its own facts (user: no elapsed-time rule decides;
        // BUFF_STEP_MAX_MS / BUFF_STEP_MIN_MS / BUFF_IDLE_GRACE_MS / FOOD_STEP_MIN_MS / FOOD_STEP_MAX_MS are gone).
        constexpr uint32 FLASK_GCD_MS = 1600;  // the game's global cooldown after the flask (1.5 s), before the food
        // [ready_reply_now] mirrors ReadyCheckAction: the bots answer a ready check when it reaches them.
        constexpr bool READY_REPLY_ON_RECEIPT = true;
        // [single_ready_check] User rule 2026-10-25: one ready check per boss. With the bots' raid leader layer
        // running the boss's own ready check (RaidLeader "pull hold"), the harness skips its step-4 check before a
        // boss (brd-n Gerstahn: buffing 30 s, then the leader's pull hold 22 s more with everyone at 100 %).
        constexpr bool HARNESS_SINGLE_READY_CHECK = true;
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

        // [lead107 stage 5 timers] the caster can give the buff now: it knows a rank of one of `firstRanks` and has
        // the reagents of its highest known rank (or casts without reagents)
        bool CanGive(Player* caster, std::vector<uint32> const& firstRanks)
        {
            for (uint32 const first : firstRanks)
            {
                uint32 known = 0;
                for (uint32 spell = first; spell; spell = sSpellMgr->GetNextSpellInChain(spell))
                    if (caster->HasSpell(spell))
                        known = spell;
                SpellInfo const* info = known ? sSpellMgr->GetSpellInfo(known) : nullptr;
                if (!info)
                    continue;
                if (caster->CanNoReagentCast(info))
                    return true;
                bool reagents = true;
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (info->Reagent[i] > 0 && !caster->HasItemCount(uint32(info->Reagent[i]), info->ReagentCount[i]))
                        reagents = false;
                if (reagents)
                    return true;
            }
            return false;
        }

        // [lead165] the caster's highest known rank of one of `firstRanks` (0: none)
        uint32 KnownRank(Player* caster, std::vector<uint32> const& firstRanks)
        {
            for (uint32 const first : firstRanks)
            {
                uint32 known = 0;
                for (uint32 spell = first; spell; spell = sSpellMgr->GetNextSpellInChain(spell))
                    if (caster->HasSpell(spell))
                        known = spell;
                if (known)
                    return known;
            }
            return 0;
        }

        bool InBuffRange(Player* bot, Player* leader)
        {
            return bot->IsAlive() && bot->IsInWorld() && bot->GetMap() == leader->GetMap() && !bot->IsInCombat() &&
                   bot->GetDistance(leader) <= BUFF_GROUP_RANGE;
        }

        // "<member> <buff>, ..." for every buff the group can give that a member lacks; empty: all present. A buff
        // nobody can give (no caster of its class, the casters out of range, no reagent) is not missing.
        // [lead165] a buff the leader orders: `caster` casts `spell` on `target`
        // [lead976] spell 0: a missing buff no caster has a rank of (nobody is ordered to give it)
        struct BuffOrder
        {
            ObjectGuid caster;
            uint32 spell = 0;
            ObjectGuid target;
            std::string what;  // [lead975] "<caster> <spell name> on <target>" ("<target> <buff>" for spell 0)
        };

        // [lead982] the aura name of a blessing category (its single and greater forms both carry it)
        char const* BlessingAuraName(ai::blessing::BaseBlessingCategory category)
        {
            switch (category)
            {
                case ai::blessing::BASE_MIGHT:
                    return "Blessing of Might";
                case ai::blessing::BASE_WISDOM:
                    return "Blessing of Wisdom";
                case ai::blessing::BASE_KINGS:
                    return "Blessing of Kings";
                case ai::blessing::BASE_SANCTUARY:
                    return "Blessing of Sanctuary";
                default:
                    return nullptr;
            }
        }

        // [lead982] every member's blessing from each paladin in range, as that paladin's blessing planner assigns
        // it (PaladinBlessingPlanner::PlanAssignments): missing when the member lacks it for the next fight; the
        // order is the paladin's highest known single rank of it on the member
        void MissingBlessings(std::vector<Player*> const& players, Player* leader, int32 needMs, std::string& missing,
                              std::vector<BuffOrder>* orders)
        {
            static std::vector<uint32> const BLESSINGS = {ai::blessing::SPELL_BLESSING_OF_MIGHT,
                                                          ai::blessing::SPELL_BLESSING_OF_WISDOM,
                                                          ai::blessing::SPELL_BLESSING_OF_KINGS,
                                                          ai::blessing::SPELL_BLESSING_OF_SANCTUARY};
            for (Player* paladin : players)
            {
                PlayerbotAI* paladinAI = paladin->getClass() == CLASS_PALADIN ? GET_PLAYERBOT_AI(paladin) : nullptr;
                if (!paladinAI || !InBuffRange(paladin, leader) || !CanGive(paladin, BLESSINGS))
                    continue;
                ai::blessing::PaladinBlessingPlanner planner(paladinAI);
                planner.PlanAssignments();
                for (Player* bot : players)
                {
                    if (!InBuffRange(bot, leader))
                        continue;
                    ai::blessing::BaseBlessingCategory const category = planner.IntendedFor(bot->GetGUID());
                    char const* const aura = BlessingAuraName(category);
                    if (!aura || HasAuraLasting(bot, aura, needMs))
                        continue;
                    missing += (missing.empty() ? "" : ", ") + bot->GetName() + " " + aura;
                    if (!orders)
                        continue;
                    if (std::optional<uint32> const rank =
                            ai::blessing::HighestKnownBlessingRank(paladin, category, false))
                    {
                        SpellInfo const* info = sSpellMgr->GetSpellInfo(*rank);
                        orders->push_back({paladin->GetGUID(), *rank, bot->GetGUID(),
                                           paladin->GetName() + " " +
                                               (info ? std::string(info->SpellName[LOCALE_enUS]) : "?") + " (" +
                                               std::to_string(*rank) + ") on " + bot->GetName()});
                    }
                    else  // [lead976] no order for it
                        orders->push_back({paladin->GetGUID(), 0, bot->GetGUID(), bot->GetName() + " " + aura});
                }
            }
        }

        std::string MissingGivableBuffs(LeaderGroup& group, std::vector<Player*> const& players, Player* leader,
                                        std::vector<BuffOrder>* orders = nullptr)
        {
            std::vector<LeaderGroup::MemberInfo> const members = group.Members();
            // [lead136] a buff counts as present when it lasts the next fight: its remaining time against the
            // planner's estimate of that fight's length (CarefulPlanVerdict::GetNextFight); no estimate: present
            CarefulPlanVerdict::Fight nextFight;
            int32 const needMs = CarefulPlanVerdict::GetNextFight(leader->GetGUID(), nextFight)
                                     ? int32(nextFight.fightSec * IN_MILLISECONDS)
                                     : 0;
            std::string missing;
            for (GroupBuffDef const& def : groupBuffs)
            {
                // [lead982] b228: the blessing def's order was the paladin's first known rank of its list (Might),
                // given by the first paladin only, and "Blessing of" anything counted as present - every member got
                // Blessing of Might (the priest, the mage, the warlock, the healers; 23 orders, 21 never cast). Each
                // paladin gives what the game's blessing planner assigns it per member (PaladinBlessingPlanner: the
                // role's blessing priorities - Might for physical damage, Kings / Wisdom for casters, Sanctuary for a
                // paladin tank - one distinct blessing per paladin, the human paladin's own blessings respected)
                if (def.casterClass == CLASS_PALADIN)
                {
                    MissingBlessings(players, leader, needMs, missing, orders);
                    continue;
                }
                bool givable = false;
                Player* giver = nullptr;
                for (Player* caster : players)
                    if (!givable && caster->getClass() == def.casterClass && InBuffRange(caster, leader) &&
                        CanGive(caster, def.spells))
                    {
                        givable = true;
                        giver = caster;
                    }
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
                        has = has || HasAuraLasting(bot, aura, needMs);
                    if (!has)
                    {
                        missing += (missing.empty() ? "" : ", ") + bot->GetName() + " " + def.label;
                        if (orders && giver)
                        {
                            if (uint32 const spell = KnownRank(giver, def.spells))
                            {
                                // a buff the caster spreads round itself (Horn of Winter) is cast on itself
                                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
                                bool const self = info && !info->NeedsExplicitUnitTarget();
                                Player* on = self ? giver : bot;
                                orders->push_back({giver->GetGUID(), spell, on->GetGUID(),
                                                   giver->GetName() + " " +
                                                       (info ? std::string(info->SpellName[LOCALE_enUS]) : "?") +
                                                       " (" + std::to_string(spell) + ") on " + on->GetName()});
                            }
                            else  // [lead976] no order for it: the buff would wait on a member's own (held) action
                                orders->push_back({giver->GetGUID(), 0, bot->GetGUID(),
                                                   bot->GetName() + " " + def.label});
                        }
                    }
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
            bool consumableUsed = false;  // step 1 -> 2: a flask was used; step 2 -> 3: food was used
            std::set<ObjectGuid> fed;     // step 2 -> 3: the members given food
            bool ordering = false;  // [lead165] [lead975] step 1: the leader orders the missing buffs
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
            // [lead107 stage 5 timers] user: no elapsed-time rule decides (BUFF_TOTAL_MAX_MS: a whole pass over 30 s
            // jumped to the ready check's end) - every step ends on its own facts
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
                std::set<ObjectGuid> const fed = wait.fed;
                wait = BuffWait();
                wait.consumableUsed = consumableUsed;
                wait.fed = fed;
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
                    // Buffs: the step ends on its facts - nobody works on a buff any more (no cast, no walk to a
                    // buff target, no rebuff window open: Begin opens it at once and the bot's own pass closes it
                    // once no buff trigger fires), and every buff the group can give now is on every member it is
                    // for; a buff still missing when every caster's pass ended is one its casters do not give
                    std::string workers = BuffWorkers(players, leader);
                    std::vector<BuffOrder> orders;
                    std::string const missing = MissingGivableBuffs(group, players, leader, &orders);
                    // [lead165] b178 rfc-h 5D (20261101-171156): the bear tank never gave Mark of the Wild to the DK
                    // (run start and after its resurrection: "every caster's buff pass ended, missing: Rtdekab Mark
                    // of the Wild") - the pass waited for the casters' own triggers. When every caster's own pass has
                    // ended with a buff still missing that one of them can give, the leader orders it (its External:
                    // the caster's highest known rank on the member); the step waits while an order stands
                    // [lead975] b228 rfc-h 5E-13 (20261103-224151) 276.3 s: the three Blessing of Might orders of
                    // Rtpalac (on Rtpriac, Rtlocab, Rthunab) went into its one External slot in the same tick - each
                    // replaced the one before (two leader_contradiction lines), and the last, set from the harness
                    // tick and never asserted again, ended with the next leader tick (EndTick drops what a tick does
                    // not assert) while the paladin stood at 1 % mana: none was cast, and none was issued again ("once
                    // per member and spell"). The step queues its orders here (Support::buffQueue); the leader tick
                    // asserts one per caster at a time, every tick, and takes the next once the buff is on or the
                    // caster reported it cannot cast it (TickBuffPass). The step waits while an order is queued
                    DungeonLeaderState::Support& orderState = group.State().support;
                    uint32 const passKey = route.buffSinceMs ? route.buffSinceMs : 1;
                    if (orderState.buffQueuePass != passKey)
                    {
                        orderState.buffQueuePass = passKey;
                        orderState.buffQueue.clear();
                        orderState.buffTold.clear();
                        orderState.buffFailed.clear();
                        orderState.buffIssuedKeys.clear();
                        orderState.buffCast = 0;
                        orderState.buffOrderWork.clear();
                    }
                    if (workers.empty() && !missing.empty())
                        wait.ordering = true;
                    orderState.buffQueue.clear();
                    std::string failed;    // [lead975] the missing buffs a caster reported it cannot cast
                    std::string unordered;  // [lead976] the missing buffs no order gives
                    std::set<DungeonLeaderState::Support::BuffKey> failedListed;
                    if (wait.ordering)
                        for (BuffOrder const& order : orders)
                        {
                            if (!order.spell)
                            {
                                unordered += (unordered.empty() ? "" : ", ") + order.what;
                                continue;
                            }
                            DungeonLeaderState::Support::BuffKey const key{order.caster, order.spell, order.target};
                            if (auto const report = orderState.buffFailed.find(key);
                                report != orderState.buffFailed.end())
                            {
                                if (failedListed.insert(key).second)
                                    failed += (failed.empty() ? "" : ", ") + order.what + " (" + report->second + ")";
                                continue;
                            }
                            bool queued = false;
                            for (DungeonLeaderState::Support::BuffOrder const& one : orderState.buffQueue)
                                queued |= one.caster == order.caster && one.spell == order.spell &&
                                          one.target == order.target;
                            if (!queued)
                                orderState.buffQueue.push_back({order.caster, order.spell, order.target, order.what});
                        }
                    if (!orderState.buffQueue.empty())
                        workers += (workers.empty() ? "" : ", ") +
                                   (orderState.buffOrderWork.empty()
                                        ? std::to_string(orderState.buffQueue.size()) + " buff order(s) queued"
                                        : "buff orders: " + orderState.buffOrderWork);
                    // [lead403] [lead405] the leader's soulstone / Ritual of Souls / Soulwell orders still work
                    std::string const support = Leader::Support::BuffPassWork(group);
                    if (!support.empty())
                        workers += (workers.empty() ? "" : ", ") + support;
                    waitingFor(workers.empty() ? missing : workers);
                    // [lead136] b162 20261101-121409 (271 s: 12 s "every buff the group can give is present",
                    // waited for "Rtdruaa rebuff window open"; 73 s over the run): every buff present (lasting the
                    // next fight) ends the step at once - an open rebuff window with nothing missing to cast is no
                    // work; with a buff missing the step waits for the casters' passes
                    std::string why;
                    if (missing.empty() && support.empty())
                        why = "every buff the group can give is present";
                    else if (workers.empty())
                        why = "every caster's buff pass ended, missing: " + missing;
                    if (missing.empty() && support.empty())
                        wait.waitedFor.clear();
                    if (why.empty())
                        break;
                    // [lead975] the step's buff orders end with it: the queue empties, the leader tick no longer
                    // asserts them (EndTick drops an External a tick does not assert)
                    if (!failed.empty())
                        why += "; failed: " + failed;
                    if (!unordered.empty())
                        why += "; no order: " + unordered;
                    if (!orderState.buffIssuedKeys.empty() || !unordered.empty())
                    {
                        LOG_INFO("playerbots",
                                 "[lead975] buff_orders: {} issued, {} cast, {} failed{}{} ({}, {})",
                                 orderState.buffIssuedKeys.size(), orderState.buffCast, orderState.buffFailed.size(),
                                 failed.empty() ? std::string() : ": " + failed,
                                 unordered.empty() ? std::string() : "; [lead976] no order: " + unordered,
                                 group.Run().runId, route.buffReason);
                        group.LogEvent("buff_orders", JsonObject()
                                                          .Num("issued", int64(orderState.buffIssuedKeys.size()))
                                                          .Num("cast", orderState.buffCast)
                                                          .Num("failed", int64(orderState.buffFailed.size()))
                                                          .Str("failures", failed)
                                                          .Str("unordered", unordered)
                                                          .Str("reason", route.buffReason));
                    }
                    orderState.buffQueue.clear();
                    orderState.buffTold.clear();
                    orderState.buffOrderWork.clear();
                    route.buffEndMissing = missing;  // [lead775] what this pass could not give
                    stepDone(2, why);
                    for (Player* bot : players)
                        wait.consumableUsed = UseConsumables(group, bot, false) || wait.consumableUsed;
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
                    wait.fed.clear();
                    for (Player* bot : players)
                        if (UseConsumables(group, bot, true))
                        {
                            ++eating;
                            wait.fed.insert(bot->GetGUID());
                        }
                    stepDone(3, eating ? std::to_string(eating) + " member(s) sat down to eat"
                                       : std::string("Well Fed on everyone, no food needed"));
                    wait.consumableUsed = eating != 0;  // step 3: somebody eats
                    break;
                }
                case 3:
                {
                    // Food: while a member given food eats (its food aura, or the food's cast still under way) and
                    // has no Well Fed yet; one that stood up without it is no longer waited for
                    std::string eaters;
                    for (Player* bot : players)
                        if (wait.fed.count(bot->GetGUID()) && InBuffRange(bot, leader) && !HasWellFed(bot) &&
                            (RestState::IsEating(bot) || bot->GetCurrentSpell(CURRENT_GENERIC_SPELL)))
                            eaters += (eaters.empty() ? "" : ", ") + bot->GetName();
                    waitingFor(eaters.empty() ? std::string() : "eating: " + eaters);
                    std::string why;
                    if (eaters.empty())
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
                    // [lead107 stage 5 timers] user: the group is ready when every buff it can give is on (step 1) and
                    // every member is at its rest line - above it, or with nothing to rest with (no drink / food in
                    // its bags); a member out of the group's buff range is the far wait's (RestWait), not the check's.
                    // No elapsed-time end (READY_STEP_MAX_MS 10 s / READY_STEP_MIN_MS 1.5 s were the old clocks)
                    else if (unanswered.empty())
                    {
                        std::string belowLine;
                        for (Player* bot : players)
                        {
                            if (!bot->IsAlive() || !InBuffRange(bot, leader))
                                continue;
                            // [lead128] only a member that can rest is waited for: sitting, or out of the fight on
                            // dry ground and unmounted (RestState::CanRest), with the drink / food in its bags - the
                            // leader's Rest (LeaderEvents, the same rest line) sits it down; one that cannot is not
                            bool const canRest = bot->IsSitState() || RestState::CanRest(bot);
                            bool const drinks = canRest && RestState::WantsDrink(bot) && CountRestItems(bot, 59) > 0;
                            bool const eats = canRest && RestState::WantsFood(bot) && CountRestItems(bot, 11) > 0;
                            if (drinks || eats)
                                belowLine += (belowLine.empty() ? "" : ", ") + bot->GetName() +
                                             (drinks ? " mana" : " health");
                        }
                        if (belowLine.empty())
                            why = "ready: buffs present, everyone at its rest line";
                        else
                            waitingFor("below its rest line: " + belowLine);
                    }
                    if (why.empty())
                        break;
                    // [lead403] [lead405] the leader's soulstone / Ritual of Souls / Soulwell orders end first
                    if (std::string const support = Leader::Support::BuffPassWork(group); !support.empty())
                    {
                        waitingFor(support);
                        break;
                    }
                    // [lead270] the pass is the boss's preparation and the group walks to the pull after it: a prepull
                    // Army not timed into it yet starts now, and its channel ends before the pass does
                    if (Creature* boss = route.targetIsBoss && !route.target.IsEmpty()
                                             ? leader->GetMap()->GetCreature(route.target)
                                             : nullptr)
                        if (std::string const army = Leader::Prepull::ArmyHolds(group, boss, true); !army.empty())
                        {
                            waitingFor(army);
                            break;
                        }
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

    std::string GivableBuffsMissing(LeaderGroup& group, std::vector<Player*> const& players, Player* leader)
    {
        return leader ? MissingGivableBuffs(group, players, leader) : std::string();
    }
}
