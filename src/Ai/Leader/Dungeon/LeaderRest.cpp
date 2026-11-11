/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderRest.h"

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
#include "RouteLegs.h"  // [kara2] RoutePath, ClosedDoors
#include "RouteMeet.h"
#include "RouteWater.h"
#include "RouteReenter.h"  // [lead341] the stranded re-entry

namespace Leader::Dungeon
{
    float ApartMetric(Player* bot, Player* leader)
    {
        float const dz = std::fabs(bot->GetPositionZ() - leader->GetPositionZ());
        return bot->GetExactDist2d(leader) + std::max(0.0f, dz - FAR_SLOPE_DZ_ALLOWANCE);
    }

    uint32 CountRestItems(Player* bot, uint32 spellCategory)
    {
        uint32 count = 0;
        auto const add = [&count, spellCategory](Item const* item)
        {
            if (!item)
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
                (proto->SubClass == ITEM_SUBCLASS_FOOD || proto->SubClass == ITEM_SUBCLASS_CONSUMABLE) &&
                proto->Spells[0].SpellCategory == spellCategory)
                count += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag const* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    add(bag->GetItemByPos(uint8(slot)));
        return count;
    }

    bool MemberClosing(Player* bot, Player* leader)
    {
        if (!bot->isMoving() || bot->movespline->Finalized() || bot->GetMap() != leader->GetMap())
            return false;
        G3D::Vector3 const end = bot->movespline->FinalDestination();
        return leader->GetExactDist(end.x, end.y, end.z) < bot->GetExactDist(leader);
    }

    void WaitForAway(LeaderGroup& group, std::vector<Player*> const& players, Player* leader,
                     std::vector<std::string> const& away)
    {
        GroupRouteState& route = group.Route();
        // Wait for the dead / outside members (resurrection or the walk back); no buffing, no pull.
        route.waitingFor.clear();
        for (std::string const& name : away)
            route.waitingFor += (route.waitingFor.empty() ? "" : ",") + name;
        if (!Travel::MeetMember(group, players, leader))
            group.ClearWaypoint(group.LeaderGuid());
        group.NoteActivity(leader->IsAlive() ? LeaderGroup::Activity::Waiting : LeaderGroup::Activity::Reviving);
    }


    // [kara2] Why a member the route waits for is apart, read from the game state, and what answers it - one line per
    // member each time the cause changes ("[kara2] leader: <member> separated by <cause>; action <what>"). The
    // answers run where they are owned: a closed door the door-cut hold opens (TickDoorCut / MemberKeyAtDoor: a
    // click by whoever of the group stands in reach, a key, or the event's own opening), a busy member is waited
    // for, everything else is the leader's walk to meet it (MeetMember).
    static std::unordered_map<ObjectGuid, std::pair<std::string, uint32>> separatedCause;  // world thread only

    static void NoteSeparated(LeaderGroup& group, Player* leader, Player* bot, bool doorCut)
    {
        constexpr uint32 SEPARATED_CHECK_MS = 2 * IN_MILLISECONDS;  // the diagnosis' path cost only, no decision
        std::string cause;
        std::string action;
        if (!bot)
            return;
        uint32 const now = getMSTime();
        auto& [last, checkedMs] = separatedCause[bot->GetGUID()];
        if (checkedMs && getMSTimeDiff(checkedMs, now) < SEPARATED_CHECK_MS)
            return;
        checkedMs = now ? now : 1;
        if (bot->GetMap() != leader->GetMap())
        {
            cause = "another map";
            action = "waits for its walk in";
        }
        else if (bot->IsNonMeleeSpellCast(false))
        {
            Spell const* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            if (!spell)
                spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            cause = std::string("busy: casting ") +
                    (spell && spell->GetSpellInfo() ? spell->GetSpellInfo()->SpellName[0] : "a spell");
            action = "waits for the cast, then its follow";
        }
        else if (RestState::IsEating(bot) || RestState::IsDrinking(bot))
        {
            cause = "busy: eating / drinking";
            action = "waits for its rest line";
        }
        else if (bot->IsInCombat())
        {
            cause = "busy: in combat";
            action = "waits for its fight";
        }
        else
        {
            GameObject* door = nullptr;
            std::vector<GameObject*> doors = Travel::ClosedDoors(bot->GetMap(), leader);
            Travel::PathResult const way = Travel::RoutePath(bot, leader->GetPositionX(), leader->GetPositionY(),
                                                     leader->GetPositionZ());
            if (way.reachable && !doors.empty())
                door = Travel::RouteThroughClosedDoor(way.points, doors, leader->GetPhaseMask());
            if (door)
            {
                bool const click = !Travel::DoorEventOf(door) && DoorOpensByUse(door);
                cause = Acore::StringFormat("closed door {} {} ({})", door->GetName(), door->GetEntry(),
                                            click                      ? "opens by use"
                                            : Travel::DoorEventOf(door) ? "the instance's event opens it"
                                                                        : "a key or a script opens it");
                action = doorCut ? (click ? "the nearest member in reach (the leader too) clicks it"
                                          : "the door-cut hold: a key from either side, or the event")
                                 : "the leader waits on its side";
            }
            else if (!way.reachable)
            {
                cause = "no path to the leader (navmesh gap)";
                action = "the leader walks to meet it";
            }
            else if (bot->isMoving())
            {
                cause = "walking, closing in";
                action = "waits";
            }
            else
            {
                cause = "standing, not following";
                action = "the leader walks to meet it";
            }
        }
        if (last == cause)
            return;
        last = cause;
        LOG_INFO("playerbots", "[kara2] leader: {} separated by {}; action {} ({}, {:.1f} yd from {})",
                 bot->GetName(), cause, action, group.Run().runId, bot->GetExactDist(leader), leader->GetName());
        group.LogEvent("member_separated", JsonObject().Str("name", bot->GetName()).Str("cause", cause)
                                               .Str("action", action).Real("distance", bot->GetExactDist(leader)));
    }

    bool RestWait(LeaderGroup& group, std::vector<Player*> const& players, Player* leader, uint32 now,
                  bool wingWalking, bool& ledGather)
    {
        GroupRouteState& route = group.Route();
        bool travel = false;
        std::vector<std::string> low;
        std::vector<std::string> farAway;
        // [lead17] LEADER_ONE_GATHER_OWNER: a led party's far / compact / ping-pong gates read the leader's
        // trailing set (TickLeaderEvents), never their own distance rules
        ledGather = Leader::LEADER_ONE_GATHER_OWNER && Leader::LEADER_PLANNER_IN_LEADER &&
                    group.HasContent() && !group.IsRaid() && group.ContentSize() <= 5 &&
                    Leader::Orders::IsTracked(group.LeaderGuid());
        // [b33_cos2] A timed run (heroic Culling, Guardian of Time running): a sitter is no far wait. Above its
        // line it stands up at that line by itself ([b33_cos] RestState::RestDone / TimedRunStandsAtLine,
        // the shorter timed-run lines of WantsDrink) and follows; below it the regen wait holds the group
        // where it stands - no MeetMember walk back to a drinker, no ping-pong hold for it. cos-h
        // 20261026-000434-cos-h-5H-22: 25 "sits above its line, not waited for", yet the same sitters were
        // "(far)" waits and ping-pong holds (00:15:02 "Rtpriae(56 yd)", 00:18:25-00:19:01, 00:21:55-00:22:52
        // ...), ~175 s of the timer.
        bool const timedRun = leader->GetMapId() == CoSIds::MAP_CULLING_OF_STRATHOLME &&
                              CoSHelpers::IsGuardianTimerRunning(leader);
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            MemberRest& rest = route.rest[member.guid];
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || !bot->IsAlive())
                continue;
            // [b30_topped_up] no walk back to the group: out and in again at the entrance (a death)
            if (Travel::StrandedReenter(group, member.guid, bot, leader))
                continue;
            bool const mana = member.usesMana && bot->GetMaxPower(POWER_MANA);
            float const manaPct = mana ? bot->GetPowerPct(POWER_MANA) : 100.0f;
            float const healthPct = bot->GetHealthPct();
            // Only real eating/drinking counts (the food / drink aura by its spell specific, not a passive
            // MOD_POWER_REGEN talent). A sitting member is waited for while it is below its own line
            // ([regen_wait_below_line] below; before: until it stood up full - the group walked off
            // 2026-10-24 18:57:49 while the healer still drank, *.bak_standup).
            bool const seated = RestState::IsEating(bot) || RestState::IsDrinking(bot);
            // The bots rest after a pull by themselves ("rest mana" / "rest health", food strategy): below
            // the rest line with water / food in the bags the group waits for them to sit down, as a group
            // waits for its drinker (Stockade 2026-10-24: the wait only held below 20 % mana, the bot only
            // drank below 15 %, so the group stood around a priest at 15-20 % who never sat down).
            uint32 drinks = mana ? CountRestItems(bot, 59) : 0;
            // [dun239] a mage that can conjure its water has a drink for this rest (its conjure is the Rest's own
            // step): waited for as a drinker. b446 wc-n five2-2 2279 s: "Thamesh ... no drink in the bags, spirit
            // regen only" at 13 % mana, knowing Conjure Water - not waited for, the five pulled on with him empty
            if (!drinks && mana && RestState::CanConjureDrink(bot))
                drinks = 1;
            uint32 const foods = CountRestItems(bot, 11);
            // The bots' own rest lines (RestState::WantsDrink / WantsFood): a hurt bot with a healer
            // close by gets healed, it does not sit down, and that is no stall.
            bool const drinkLine = mana && RestState::WantsDrink(bot);
            bool const foodLine = RestState::WantsFood(bot);
            bool const wantsDrink = drinkLine && drinks;
            bool const wantsFood = foodLine && foods;
            bool const lowMana = mana && manaPct < 20.0f;
            // [regen_wait_below_line] User rule 2026-10-25 (combat time as high as possible): a sitting member
            // is waited for only while it is below its own line (RestState::WantsDrink / WantsFood: trash
            // 75 %, company 45 %, a protection paladin 30 %, 95 % before a boss), and a member that does not
            // sit only below the hard lines (45 % health, 20 % mana). Above them the group goes on, the
            // sitter stands up by itself (rest_stand_at_line) or when the fight starts. DOWNTIME_AUDIT: regen
            // waits 20,724 s a wave, the group's average mana >= 80 % in 68 % of them.
            bool const hardLine = healthPct < 45.0f || lowMana;
            bool const seatedBelowLine = seated && ((RestState::IsDrinking(bot) && drinkLine) ||
                                                    (RestState::IsEating(bot) && foodLine));
            // [queue-rest-sit-grace] hor-h 20261025 build 29: the mage sat at 53 % after Marwyn, its line moved
            // after the boss, "sits above its line, not waited for", the group walked off and the meet brought the
            // tank back. [lead128] user: no elapsed-time rule decides (REST_SIT_GRACE_MS 10 s was the old clock): a
            // sitter the leader told to rest (its Rest command) is waited for - the leader's rest decision, not the
            // time it sat
            Leader::MemberEvents restEvents;
            bool const leaderRests =
                Leader::Events::Get(member.guid, restEvents) && restEvents.ready == Leader::ReadyEvent::Rest;
            bool const freshSitter = seated && leaderRests;
            // [lead15] LEADER_ONE_REST_RULE: in a group the leader leads every sitter is waited for
            bool const leaderRest = Leader::Leads(Leader::LEADER_ONE_REST_RULE) && group.HasContent() &&
                                    !group.IsRaid() && group.ContentSize() <= 5 &&
                                    Leader::Orders::IsTracked(group.LeaderGuid());
            bool const regenerating =
                REGEN_WAIT_BELOW_LINE && !leaderRest ? (seatedBelowLine || freshSitter) : seated;
            // [lead394] b201 rfc-h 5C: 353 s of "drinking" waits, 138 s of them under a leader Rest - 593-641 s the
            // route waited 48 s for "Rtdruab(regen)" (the balance druid under 20 % mana, the hard line) while the
            // leader's computed line for its next fight was ~10 % ("mana 8 % < need 10 %"): no Rest, no drink, spirit
            // regen standing; 5D 51 s for the resto shaman, 5A 38 s for the enhancement shaman. Two rest rules for
            // one fact. In a group the leader leads its Rest is the one rest rule: the route waits for exactly the
            // members it rests
            bool const waitedFor = leaderRest ? leaderRests : (!REGEN_WAIT_BELOW_LINE || regenerating || hardLine);
            if (seated && !regenerating && !hardLine && !rest.regenAboveLineLogged)
            {
                rest.regenAboveLineLogged = true;
                group.LogEvent("regen_wait_below_line",
                         JsonObject().Str("name", member.name).Real("hp", healthPct).Real("mana", manaPct)
                             .Str("state", "sits above its line, not waited for"));
            }
            if (seated || hardLine || wantsDrink || wantsFood || (leaderRest && leaderRests))
            {
                // Its combat engine runs out of combat (it "attacks" a mob that does not fight back: Violet
                // Hold's released prisoner): the drink never runs, a wait only holds the group. Not waited for.
                PlayerbotAI* memberAI = GET_PLAYERBOT_AI(bot);
                // [lead128] user: no elapsed-time rule decides (REGEN_STALL_RELEASE_MS: a member standing 20 s
                // after its regen_stall was let go). A standing member that should sit is waited for while it CAN
                // rest - the leader's Rest (LeaderEvents, the same rest line) sits it down, the member executes;
                // what keeps it from resting is the fact that ends the wait for it at once: no drink / food in its
                // bags, mounted, in the water, its group fighting (RestState::CanRest), its combat engine
                bool const noWater = lowMana && !drinks;
                // [lead1293] b251 5E-13 t=351: the tank (33 % mana) "waits for regen ... combat engine without
                // combat" on the tick the leader gave it the boss's pull (PullAction puts its engine in combat),
                // after the leader's boss gate had passed it at 33 % (its line 3 %): its own pre-boss drink line
                // (RestState::WantsDrink, 95 %) called it a sitter. In a group the leader leads, its Rest is the
                // one rest rule ([lead394]): a member sits when the leader rests it, or with no water at all
                bool const shouldSit = leaderRest ? (noWater || leaderRests)
                                                  : (noWater || drinkLine || foodLine);
                // [lead1293] b251: 24 of 27 "combat engine without combat" stalls were the fight's last second -
                // the member's combat engine still holds its dead / gone target and drops it on its own next tick
                // ("drop target" switches to the non-combat engine; 5E-5 t=434 the hunter drank 3 s later). Only
                // a combat engine with a living target to act on keeps the drink from running
                Unit* engineTarget = (memberAI && memberAI->GetState() == BOT_STATE_COMBAT)
                                         ? memberAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get()
                                         : nullptr;
                bool const combatEngineHeld = engineTarget && engineTarget->IsAlive();
                std::string cannot;
                if (!seated && shouldSit)
                {
                    cannot = noWater ? "no drink in the bags, spirit regen only"
                             : (drinkLine && !drinks && !(foodLine && foods)) ? "no drink in the bags"
                             : (foodLine && !foods && !(drinkLine && drinks)) ? "no food in the bags"
                             : bot->IsMounted()                               ? "mounted"
                             : bot->Unit::IsInWater()                         ? "in the water"
                             : !RestState::CanRest(bot)                       ? "cannot rest: its group fights"
                             : combatEngineHeld ? "combat engine without combat (drink is a non-combat action)"
                                                : "";
                }
                if (waitedFor && cannot.empty() && (seated || !memberAI || !combatEngineHeld || bot->IsInCombat()))
                    low.push_back(member.name);
                if (!rest.regenSinceMs || !route.waitSinceMs)
                {
                    rest.regenSinceMs = now;
                    rest.regenStallLogged = false;
                    rest.regenStallMs = 0;
                }
                // Waiting for regen must mean eating / drinking: a member that cannot rest is logged at once with
                // its fact; one that can and still stands after REGEN_SIT_GRACE_MS is logged too (a diagnostic only,
                // the wait for it is the leader's Rest)
                if (!seated && shouldSit && !rest.regenStallLogged && !bot->IsInCombat() &&
                    (!cannot.empty() || getMSTimeDiff(rest.regenSinceMs, now) > REGEN_SIT_GRACE_MS))
                {
                    rest.regenStallLogged = true;
                    rest.regenStallMs = now ? now : 1;
                    std::string const why =
                        !cannot.empty()                    ? cannot
                        : bot->IsNonMeleeSpellCast(false)  ? "casting"
                        : leaderRests                      ? "under the leader's Rest, does not sit down"
                                                           : "does not sit down (no Rest command yet)";
                    LOG_INFO("playerbots",
                             "[regen] {}: {} waits for regen (hp {:.0f}%, mana {:.0f}%) but is not "
                             "eating / drinking: {} (drinks {}, food {}){}",
                             group.Run().runId, member.name, healthPct, manaPct, why, drinks, foods,
                             cannot.empty() ? "" : " - not waited for");
                    group.LogEvent("regen_stall",
                             JsonObject().Str("name", member.name).Real("hp", healthPct).Real("mana", manaPct)
                                 .Num("drinks", drinks).Num("food", foods).Str("why", why)
                                 .Bool("waited", cannot.empty()));
                }
            }
            else
            {
                rest.regenSinceMs = 0;
                rest.regenStallLogged = false;
                rest.regenStallMs = 0;
            }
            // The straggler MeetMember handles (route.meeting) stays far until within its 45 yd: the leader's
            // walk back across 50 yd flipped the wait on and off, its 60 s timeout never came (SFK 5B-5).
            // [b31_far_threshold] the far line (FAR_WAIT_RANGE / FAR_TRAIL_RANGE, horizontal + steep dz),
            // waited for down to FAR_RESUME_RANGE
            bool isFar = bot->GetMap() != leader->GetMap();
            if (!isFar && bot != leader)
            {
                float const metric = ApartMetric(bot, leader);
                // [lead128] user: closing in is a fact of the member's walk, not elapsed time (FAR_NOT_CLOSING_MS
                // 5 s was the old clock): it walks, and its walk ends nearer the leader than it stands
                bool const notClosing = !MemberClosing(bot, leader);
                // [b32_dmw] A member sitting with food / drink inside FAR_WAIT_RANGE rests where its fight
                // ended, it does not trail: the regen wait above holds for it while it is below its line, and
                // it stands up and follows by itself above it (RestDone). dmw-n 20261025-221216-5E-12: the
                // priest ended the open-courtyard pulls 25-35 yd from the tank (flee from "enemy too close
                // for spell", wand at 30 yd; median 21 yd at pull end vs 14-16 in sfk-h / rfk-h 5E), sat down
                // there, never "closed" and was a "Rlpriad(far)" wait 54 times; MeetMember walked the leader
                // back to a drinker.
                bool const seatedNear = seated && metric <= FAR_WAIT_RANGE;
                isFar = (timedRun && seated) ? false :  // [b33_cos2] a timed run's sitter: no far wait
                      metric > FAR_WAIT_RANGE ||
                      (!seatedNear && metric > FAR_TRAIL_RANGE && notClosing && !leader->isMoving()) ||
                      (!seatedNear && (rest.farWaited || route.meeting == member.guid) &&
                       metric > FAR_RESUME_RANGE);
            }
            if (ledGather && bot != leader)
                isFar = bot->GetMap() != leader->GetMap() || group.State().trailing.count(member.guid) > 0;
            // [b32_strat_gate] a member cut off behind a closed door is waited for wherever it stands (8 yd
            // behind the shut gate is no "together"), stranded or ignored or not.
            bool const doorCut = route.doorCutHold && route.doorCutMember == member.guid;
            if (doorCut ||
                (isFar && !group.MemberStranded(bot, leader) && !Travel::BlackMorassHoldsAtMedivh(bot) &&
                 !(member.apartIgnoreUntilMs && int32(now - member.apartIgnoreUntilMs) < 0)))
            {
                farAway.push_back(member.name);
                rest.farWaited = true;
                NoteSeparated(group, leader, bot, doorCut);  // [kara2]
            }
            else
            {
                rest.farWaited = false;
                separatedCause.erase(member.guid);  // [kara2] together: the next separation is logged afresh
            }
        }

        // [lead128] user: no elapsed-time rule decides - the wait has no timeout and no cooldown (the 60 s / 180 s
        // wait_timeout, its 45 s cooldown and the far_wait_dropped after two of them were the old clocks). It ends on
        // its facts: the sitter at its line, the far member together (or stranded: no walk either way), the
        // door-cut member through.
        bool const doorCutWait = route.doorCutHold && !route.doorCutMember.IsEmpty();
        // A walked wing change: every member walks out and in on its own, apart by design (Dire Maul East
        // 2026-10-25: the far wait and its ping-pong hold pulled the leader back during the walk).
        // [cos-event-trash] (PATCH_queue_encounters section 2) no far wait while the heroic Culling's
        // Infinite Corruptor detour runs against the Guardian of Time (cos-h 132016: 80 s for 66 yd, a mage
        // 91 yd behind waited for); a low member is still waited for.
        bool const corruptorDetour = leader->GetMapId() == CoSIds::MAP_CULLING_OF_STRATHOLME &&
                                     CoSHelpers::GetCorruptorDetourTarget(leader) != nullptr;
        bool const waiting =
            (!low.empty() || (!farAway.empty() && !wingWalking && !corruptorDetour)) && route.escort.IsEmpty();

        // REGROUP_PING_PONG_GUARD: who is apart from the leader (another map, or farther than
        // REGROUP_PING_PONG_TOGETHER), and how many of them on another level.
        std::string apartNames;
        uint32 apart = 0;
        uint32 otherLevel = 0;
        std::vector<ObjectGuid> apartGuids;
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || bot == leader || !bot->IsAlive() || group.MemberStranded(bot, leader) ||
                Travel::BlackMorassHoldsAtMedivh(bot) ||  // [hleft-bm-tank-fetch]
                (member.apartIgnoreUntilMs && int32(now - member.apartIgnoreUntilMs) < 0) ||
                (timedRun && (RestState::IsEating(bot) || RestState::IsDrinking(bot))))  // [b33_cos2]
                continue;
            bool const otherMap = bot->GetMap() != leader->GetMap();
            float const dz = bot->GetPositionZ() - leader->GetPositionZ();
            // [b31_far_threshold] horizontal + steep dz; a ramp's dz up to FAR_SLOPE_DZ_ALLOWANCE is no level
            float const distance = otherMap ? 0.0f : ApartMetric(bot, leader);
            bool const levelApart =
                std::fabs(dz) > std::max(REGROUP_PING_PONG_LEVEL_DZ, FAR_SLOPE_DZ_ALLOWANCE);
            // [b31_far_threshold] a running hold ends only with everyone within FAR_RESUME_RANGE
            bool const holding = route.pingPongHoldUntilMs != 0;
            // Apart only where the meet acts: MeetMember walks to nobody within MEET_RANGE (ok-n 2026-10-25:
            // Rlshaah 30.07 yd, dz 2.1 held the group until the distance dipped under 30).
            if (!otherMap && (distance <= (holding ? FAR_RESUME_RANGE : REGROUP_PING_PONG_TOGETHER) ||
                              (!holding && distance <= REGROUP_PING_PONG_MEET_RANGE && !levelApart)))
                continue;
            ++apart;
            apartGuids.push_back(member.guid);
            if (!otherMap && levelApart)
                ++otherLevel;
            apartNames += (apartNames.empty() ? "" : ",") + member.name +
                          (otherMap ? std::string("(other map)")
                                    : Acore::StringFormat("({:.0f} yd, dz {:.1f})", bot->GetExactDist(leader),
                                                          bot->GetPositionZ() - leader->GetPositionZ()));
        }
        // The decision of this tick (the regen wait is no side of the ping-pong) and its flips.
        // [lead128] user: no elapsed-time rule decides (REGROUP_PING_PONG_MS / _GAP_MS / _HOLD_MS / _COOLDOWN_MS and
        // REGROUP_APART_IGNORE_MS were the old clocks). The ping-pong is a fact of the decisions: since the group was
        // last together, the decision came back to one it had already taken (meet -> route -> meet). Then the meet
        // is held - no new target - until the group is together, or a fight starts.
        uint8 const decision = !waiting ? DECISION_ROUTE : (low.empty() ? DECISION_MEET : 0);
        if (!apart && !route.pingPongHoldUntilMs)
            route.decisionFlips = 0;  // together: the next ping-pong counts from here
        if (REGROUP_PING_PONG_GUARD && decision && !leader->IsInCombat() && decision != route.decision)
        {
            if (route.decision && apart)
            {
                if (!route.decisionFlips)
                    route.flipSinceMs = now ? now : 1;  // for the logs
                ++route.decisionFlips;
                route.lastFlipMs = now;
            }
            route.decision = decision;
        }
        if (REGROUP_PING_PONG_GUARD && !ledGather && !wingWalking && !route.pingPongHoldUntilMs && apart &&
            !leader->IsInCombat() && route.decisionFlips >= 2)
        {
            route.pingPongHoldUntilMs = now ? now : 1;  // the hold is on (since)
            // The meet itself decides how: the members' own follow first (MeetMember's grace: the group
            // gathers on the leader's level), then the leader's walk back over the route to the farthest.
            std::string const how = otherLevel * 2 >= apart
                                        ? "the group on another level: members follow first, then the "
                                          "leader rejoins them over the route"
                                        : "the group gathers on the leader's level";
            LOG_INFO("playerbots",
                     "[regroup] {}: ping-pong meet <-> route ({} flips): hold the meet until the group is together, "
                     "no new target - {}; apart: {}",
                     group.Run().runId, route.decisionFlips, how, apartNames);
            group.LogEvent("regroup_ping_pong",
                     JsonObject().Str("state", "hold meet").Num("flips", route.decisionFlips)
                         .Num("sec", route.flipSinceMs ? getMSTimeDiff(route.flipSinceMs, now) / IN_MILLISECONDS : 0)
                         .Num("apart", apart).Num("otherLevel", otherLevel).Str("how", how)
                         .Str("members", apartNames).Real("x", leader->GetPositionX())
                         .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ()));
        }
        bool pingPongMeet = false;
        if (route.pingPongHoldUntilMs)
        {
            if (!apart || leader->IsInCombat())
            {
                char const* const why = !apart ? "together" : "combat";
                LOG_INFO("playerbots", "[regroup] {}: ping-pong hold ends: {}{}{}", group.Run().runId, why,
                         apart ? "; apart: " : "", apartNames);
                group.LogEvent("regroup_ping_pong",
                         JsonObject().Str("state", "end").Str("why", why).Str("members", apartNames));
                route.pingPongHoldUntilMs = 0;
                route.decisionFlips = 0;
                route.flipSinceMs = 0;
            }
            else
                pingPongMeet = low.empty();
        }

        if (pingPongMeet)
        {
            // The held meet (REGROUP_PING_PONG_GUARD): the route waits while the group is apart.
            route.waitingFor = "ping-pong hold: " + apartNames;
            if (!Travel::MeetMember(group, players, leader))
                group.ClearWaypoint(group.LeaderGuid());
            group.NoteActivity(LeaderGroup::Activity::Waiting);
        }
        // An escort walks on: the group keeps up with it instead of resting (it fights alone otherwise).
        else if (waiting)
        {
            if (!route.waitSinceMs)
                route.waitSinceMs = now;
            route.waitingFor.clear();
            for (std::string const& name : low)
                route.waitingFor += (route.waitingFor.empty() ? "" : ",") + name + "(regen)";
            for (std::string const& name : farAway)
                route.waitingFor += (route.waitingFor.empty() ? "" : ",") + name + "(far)";
            // [b32_strat_gate] the member behind a closed door: the leader walks toward it - its walk holds at
            // the door (door_hold, SetLeaderWaypoint) and ReopenKeyDoorHeld keys it from the leader's side;
            // the member keys it from its own side meanwhile (MemberKeyAtDoor). No other walk, no pull.
            Player* const cutBot =
                doorCutWait ? ObjectAccessor::FindPlayer(route.doorCutMember) : nullptr;
            if (cutBot && cutBot->GetMap() == leader->GetMap())
            {
                route.waitingFor += (route.waitingFor.empty() ? "" : ",") + std::string(cutBot->GetName()) +
                                    "(behind a closed door)";
                // [b35_ss] not while a key pass runs (TickKeyDoorPass): the leader's pass walk to the far
                // point stands - this walk to the cut member replaced it and held the tank at the gate while
                // the group went through (strat-n 20261026-024524-5E-2, 79-93 s).
                LeaderGroup::WalkPoint passWalk;
                bool const passing = !route.keyPassDoor.IsEmpty() &&
                                     group.GetWaypoint(group.LeaderGuid(), passWalk) && passWalk.gatePass;
                // [b51_strat] User 2026-10-26: the door is opened for the stranded member, nobody crosses
                // back to fetch it. The leader walks to its OWN side of the door (DOOR_REOPEN_STEP yd from
                // the door's centre toward the leader), never to the member: a waypoint at the member ran
                // the leader through the door the moment it opened.
                GameObject* cutDoor = leader->GetMap()->GetGameObject(route.doorCutDoor);
                if (!passing && low.empty() && cutDoor)
                {
                    constexpr float DOOR_REOPEN_STEP = 3.0f;
                    float const dx = leader->GetPositionX() - cutDoor->GetPositionX();
                    float const dy = leader->GetPositionY() - cutDoor->GetPositionY();
                    float const len = std::max(0.1f, std::hypot(dx, dy));
                    float const step = std::min(DOOR_REOPEN_STEP, len);
                    float const x = cutDoor->GetPositionX() + dx / len * step;
                    float const y = cutDoor->GetPositionY() + dy / len * step;
                    float const ground = leader->GetMap()->GetHeight(leader->GetPhaseMask(), x, y,
                                                                     cutDoor->GetPositionZ() + 3.0f, true,
                                                                     10.0f);
                    if (leader->GetExactDist2d(x, y) > 2.0f)
                        group.SetLeaderWaypoint(leader, x, y,
                                          ground > INVALID_HEIGHT ? ground : leader->GetPositionZ());
                    else
                        group.ClearWaypoint(group.LeaderGuid());
                    static std::unordered_map<uint64, uint32> reopenLogMs;  // world thread only
                    uint32& logged = reopenLogMs[cutBot->GetGUID().GetRawValue()];
                    if (!logged || getMSTimeDiff(logged, now) >= 10 * IN_MILLISECONDS)
                    {
                        logged = now ? now : 1;
                        group.LogEvent("door_reopen_for",
                                 JsonObject().Str("member", cutBot->GetName()).Str("door", cutDoor->GetName())
                                     .Num("doorEntry", cutDoor->GetEntry())
                                     .Real("leaderToDoor", leader->GetExactDist(cutDoor))
                                     .Real("memberToDoor", cutBot->GetExactDist(cutDoor)));
                        LOG_INFO("playerbots", "[b51_strat] door_reopen_for {}: {} waits on its side of {} {} "
                                 "({:.1f} yd), the door is opened for the member, nobody crosses back",
                                 cutBot->GetName(), leader->GetName(), cutDoor->GetName(),
                                 cutDoor->GetEntry(), leader->GetExactDist(cutDoor));
                    }
                }
                else if (!passing)
                    group.ClearWaypoint(group.LeaderGuid());
            }
            else if (!low.empty() &&
                     (group.LeaderDecides() && Leader::Leads(Leader::LEADER_ONE_PATROL_OWNER)
                          ? Travel::LeaderPatrolTick(group, leader)  // [lead4] the leader's one patrol decision
                          : Travel::PatrolStepBack(group, leader)))
            {
                // [b46_patrol] a member rests or is about to while a patrol walks in: the group steps back
                // out of its way first (the step-back walk holds the leader's waypoint), then rests there
            }
            else if (!low.empty() || !Travel::MeetMember(group, players, leader))
                group.ClearWaypoint(group.LeaderGuid());
            group.NoteActivity(low.empty() ? LeaderGroup::Activity::Waiting : LeaderGroup::Activity::Drinking);
            if (!low.empty())
                Travel::LeaveWater(group, players, true);  // rest on dry ground, never in the water
        }
        else
        {
            // The rest is over: the next target (and its walk) at once, not on the 4 s retarget clock.
            if (route.waitSinceMs)
                route.nextRetargetMs = now;
            route.waitSinceMs = 0;
            route.waitingFor.clear();
            // The rest / wait is over: the phase label said "drinking" for minutes in wave 6 (HoR) while
            // nobody drank and the route was running.
            group.NoteActivity(LeaderGroup::Activity::Traveling);
            travel = true;
        }
        return travel;
    }


    bool CompactPause(LeaderGroup& group, Player* leader, uint32 now, bool ledGather)
    {
        GroupRouteState& route = group.Route();
        bool compactPause = false;
        // [b31_far_threshold] the group travels together: the leader pauses for a member over
        // FAR_TRAIL_RANGE behind until it is within FAR_RESUME_RANGE, while it comes
        if (!route.approach && route.escort.IsEmpty() && route.linkIdx < 0 && !group.IsRaid() &&
            !leader->IsInCombat())
        {
            Player* trailing = nullptr;
            float trail = 0.0f;
            for (LeaderGroup::MemberInfo const& member : group.Members())
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || bot == leader || !bot->IsAlive() || bot->GetMap() != leader->GetMap() ||
                    bot->IsInCombat() || group.MemberStranded(bot, leader) ||
                    (member.apartIgnoreUntilMs && int32(now - member.apartIgnoreUntilMs) < 0))
                    continue;
                float const metric = ApartMetric(bot, leader);
                if (metric > trail)
                {
                    trail = metric;
                    trailing = bot;
                }
            }
            // [b33_harn] a healer drinking / out of sight / beyond FAR_RESUME_RANGE behind
            Player* healerBehind = nullptr;
            bool healerResting = false;
            for (LeaderGroup::MemberInfo const& member : group.Members())
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || bot == leader || !bot->IsAlive() || !PlayerbotAI::IsHeal(bot) ||
                    bot->GetMap() != leader->GetMap() || bot->IsInCombat() ||
                    group.MemberStranded(bot, leader) ||
                    (member.apartIgnoreUntilMs && int32(now - member.apartIgnoreUntilMs) < 0))
                    continue;
                bool const resting = bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN) ||
                                     bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->IsSitState();
                if (ApartMetric(bot, leader) > FAR_RESUME_RANGE || !bot->IsWithinLOSInMap(leader) ||
                    (resting && bot->GetExactDist(leader) > FAR_RESUME_RANGE * 0.5f))
                {
                    healerBehind = bot;
                    healerResting = resting;
                }
            }
            // [lead128] user: no elapsed-time rule decides (COMPACT_PAUSE_MAX_MS 10 s / _HEALER_MS 30 s / _GAP_MS
            // 20 s / _STALE_MS 3 s were the old clocks). The pause holds while its member comes: the trailing member
            // closing in (its walk ends nearer the leader, MemberClosing), the healer behind resting or closing in.
            // One that is not coming is no pause - the far wait's facts take it (RestWait: far, not closing).
            bool const trailComes = trailing && MemberClosing(trailing, leader);
            bool const healerComes = healerBehind && (healerResting || MemberClosing(healerBehind, leader));
            bool const want = !ledGather &&
                              ((trailing && trailComes &&
                                trail > (route.compactPauseSinceMs ? FAR_RESUME_RANGE : FAR_TRAIL_RANGE)) ||
                               (healerComes && route.compactPauseSinceMs));
            if (want && !route.compactPauseSinceMs)
            {
                route.compactPauseSinceMs = now ? now : 1;
                group.LogEvent("compact_pause", JsonObject().Str("state", "start")
                                                     .Str("member", trailing ? trailing->GetName() : "")
                                                     .Real("apart", trail));
            }
            if (route.compactPauseSinceMs)
            {
                if (want)
                    compactPause = true;
                else
                {
                    bool const together = !trailing || trail <= FAR_RESUME_RANGE;
                    group.LogEvent("compact_pause",
                             JsonObject().Str("state", together ? "together" : "not coming")
                                 .Num("sec", getMSTimeDiff(route.compactPauseSinceMs, now) /
                                                 IN_MILLISECONDS)
                                 .Real("apart", trail));
                    route.compactPauseSinceMs = 0;
                    route.compactPauseEndMs = now ? now : 1;
                }
            }
        }
        else if (route.compactPauseSinceMs)
        {
            route.compactPauseSinceMs = 0;
            route.compactPauseEndMs = now ? now : 1;
        }
        return compactPause;
    }
}
