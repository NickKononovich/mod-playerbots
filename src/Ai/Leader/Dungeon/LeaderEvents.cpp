/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
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
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
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

namespace Leader::Dungeon
{
    using namespace Leader::Fight;

    // [lead22] slabs-h 20261026-201857: the leader told a full healer to Rest 52 times («[rest] Rtdruaa stands up:
    // full (hp 100 %, mana 100 %)») and its readiness waited for that sitter forever. A member at 99 % health and mana
    // never rests and a full sitter holds nothing (the bot's own stand-up rule: RestDone, full)
    bool MemberFull(Player* bot)
    {
        return bot && bot->GetHealthPct() >= 99.0f &&
               (!bot->GetMaxPower(POWER_MANA) || bot->GetPowerPct(POWER_MANA) >= 99.0f);
    }

    void TickLeaderEvents(LeaderGroup& group, Player* tank)
    {
        auto& lead = group.State();
        uint32 const now = getMSTime() ? getMSTime() : 1;
        if (!Leader::LEADER_MEMBER_EVENTS || !tank || !tank->IsInWorld())
            return;
        // [lead15] the cadence (user: «чем быстрее у него реакция, тем лучше»): the FAST layer runs on every update
        // while the group fights, a pull waits for its spots or a hazard is active; calm, every LEADER_CALM_TICK_MS.
        // The SLOW layer (exit / re-spot searches, the AoE rule's grid visit) runs only on a change (cached).
        // [lead93] b134 rfc-h 1206 s: the whole group was ported to the entrance (map 1) and the leader went on with
        // in-instance moves ("refused a straight move over open air on map 1 ... to (-330.8,243.0,-11.7)"). Outside
        // the instance the leader issues nothing (logged once); the harness's own re-entry walk brings them back
        if (tank->GetMapId() != group.MapId())
        {
            if (!lead.outsideLogged)
            {
                lead.outsideLogged = true;
                Decisions().Write(group.Run(), "leader",
                                              "leader_outside", tank->GetGUID(), tank->GetName(), true,
                                              JsonObject().Num("map", tank->GetMapId())
                                                  .Num("instanceMap", group.MapId()));
            }
            return;
        }
        lead.outsideLogged = false;
        bool anyCombat = group.FightOn(tank);  // [lead84] the one definition
        for (ObjectGuid const& groupMember : group.MemberGuids())
            if (!anyCombat)
                if (Player* bot = ObjectAccessor::FindPlayer(groupMember);
                    bot && bot->IsInCombat() && !bot->getAttackers().empty())
                    anyCombat = true;
        bool const prePull = lead.spotsWaiting || !lead.spots.empty();
        uint32 const state = lead.hazardActive ? 3 : anyCombat ? 2 : prePull ? 1 : 0;
        // [lead72] user: 0.3 s is fast enough - the fight / pre-pull / hazard pass runs at most every
        // LEADER_FIGHT_TICK_MS, not on every world update (b116: ~100 healer events in 0.2 s)
        uint32 const period = state ? LEADER_FIGHT_TICK_MS : LEADER_CALM_TICK_MS;
        if (lead.eventTickMs && getMSTimeDiff(lead.eventTickMs, now) < period)
            return;
        lead.eventTickMs = now;
        auto const tickStart = std::chrono::steady_clock::now();
        lead.slowUsTick = 0;
        ++lead.cadenceTicks[state];
        bool hazardSeen = false;
        DecisionRun const run = group.Run();
        Map* map = tank->GetMap();
        // [lead18] the group's walked ground (cleared): a sample per member and the tank every 3 yd
        for (ObjectGuid const& groupMember : group.MemberGuids())
            if (Player* walker = ObjectAccessor::FindPlayer(groupMember);
                walker && walker->IsAlive() && walker->GetMap() == map)
            {
                Position& last = lead.clearedLast[groupMember];
                if (last.GetExactDist(walker->GetPosition()) >= 3.0f)
                {
                    last = walker->GetPosition();
                    lead.clearedGround.push_back(last);
                    if (lead.clearedGround.size() > 800)
                        lead.clearedGround.erase(lead.clearedGround.begin(), lead.clearedGround.begin() + 100);
                }
            }
        // [lead18] the spot guard's idle mobs (refreshed every 500 ms: the slow layer)
        if (!lead.idleScanMs || getMSTimeDiff(lead.idleScanMs, now) >= 500)
        {
            lead.idleScanMs = now;
            std::list<Creature*> idle;
            IdleAggroCheck check{tank};
            Acore::CreatureListSearcher<IdleAggroCheck> searcher(tank, idle, check);
            Cell::VisitObjects(tank, searcher, 80.0f);
            lead.idleGuids.clear();
            for (Creature* mob : idle)
                lead.idleGuids.push_back(mob->GetGUID());
        }
        std::vector<Creature*> idleMobs;
        for (ObjectGuid const& guid : lead.idleGuids)
            if (Creature* mob = map->GetCreature(guid); mob && mob->IsAlive() && !mob->IsInCombat())
                idleMobs.push_back(mob);
        g_leaderIdle = &idleMobs;
        g_leaderCleared = &lead.clearedGround;
        std::vector<ObjectGuid> pullMobs = LeaderPullMobs(lead.commit, lead.commitMob);
        pullMobs.insert(pullMobs.end(), lead.joinedMobs.begin(), lead.joinedMobs.end());  // [lead76]
        g_leaderExempt = &pullMobs;
        // [lead115] the pull walks: the engage order open (not fighting yet) and the tank without threat on its mob
        bool pullWalk = false;
        uint32 pullWalkOrder = 0;
        {
            Leader::PullOrder walkOrder;
            if (lead.commit && lead.commitIssued && Leader::Orders::GetOpen(group.LeaderGuid(), walkOrder) &&
                walkOrder.method == Leader::PullMethod::Engage && walkOrder.state != Leader::OrderState::Fighting)
                if (Unit* walkMob = ObjectAccessor::GetUnit(*tank, lead.commitMob); walkMob && walkMob->IsAlive())
                {
                    // [lead118] b152 rfc-h 870-931 s: the healer's pull-walk Hold outlived the pull (pull_start at
                    // 875 s, the healer 58 yd off the tank to the wipe) - the walk ends with the fight: the tank in
                    // combat, the group's pull on, or the tank's threat on the pull's mob
                    pullWalk = !tank->IsInCombat() && !group.InCombat() &&
                               (!walkMob->IsInCombat() || walkMob->GetThreatMgr().GetThreat(tank) <= 0.0f);
                    pullWalkOrder = walkOrder.id;
                }
        }
        if (!pullWalk && !lead.pullWalkSpots.empty())
        {
            // [lead117] the pull walk's decisions end with it; [lead118] and their commands: a member held by the
            // pull walk is let go at once (its fight spot / heal reach takes it this tick)
            for (auto const& [member, spot] : lead.pullWalkSpots)
            {
                Leader::MemberEvents events;
                if (Leader::Events::Get(member, events) && events.moveWhy == "the pull walks")
                    Leader::Events::SetMove(member, Leader::MoveEvent::None, spot.spot, 0.0f, "the pull walk ended");
            }
            lead.pullWalkSpots.clear();
        }
        struct GuardReset
        {
            ~GuardReset()
            {
                g_leaderIdle = nullptr;
                g_leaderCleared = nullptr;
                g_leaderExempt = nullptr;
            }
        } guardReset;
        // [lead18] every MoveTo / StandAt the leader issues: the spot and the walk out of every unpulled mob's reach;
        // a rejected one is not issued (the member stays) and logged as spot_rejected_aggro
        auto const checkedMove = [&](ObjectGuid member, Leader::MoveEvent move, Position const& spot, float tolerance,
                                     std::string const& why) -> bool
        {
            Player* bot = ObjectAccessor::FindPlayer(member);
            if (bot && move != Leader::MoveEvent::Hold && Leader::LEADER_SPOT_AGGRO_GUARD)
            {
                auto& checked = lead.spotChecked[member];
                bool const again = checked.first.GetExactDist(spot) > 1.0f || !checked.second ||
                                   getMSTimeDiff(checked.second, now) > 2 * IN_MILLISECONDS;
                std::string reject;
                Creature* mob = LeaderAggroAt(bot, spot);
                bool noPath = false;
                if (!mob && again)
                    mob = LeaderAggroOnWalk(bot, spot, noPath);
                if (mob)
                    reject = "the spot or its walk is in the reach of " + mob->GetName();
                else if (noPath)
                    reject = "no walk to the spot";
                // [lead93] no spot in the instance's exit areatrigger (it ports the member out)
                else if (uint32 const exit = InInstanceExit(bot->GetMapId(), spot.GetPositionX(), spot.GetPositionY(),
                                                            spot.GetPositionZ(), bot->GetCombatReach()))
                    reject = Acore::StringFormat("the spot is in the instance's exit (areatrigger {})", exit);
                // [lead19] the stricter model: the member's own guard (its idle-wake rule, a closed door) agrees too
                if (reject.empty() && again)
                    if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(bot))
                        if (auto* mover = dynamic_cast<LeaderMoveOrderAction*>(
                                memberAI->GetAiObjectContext()->GetAction("leader move order")))
                        {
                            reject = mover->WalkRefusal(spot);
                            // [lead71] b115 220.8 s: Rtshaab refused the leader's meeting-point move ("a straight
                            // move over open air") - one verdict: the member's own MoveTo guards, run dry from where
                            // it stands, before the move is issued
                            if (reject.empty() && !mover->DryMoveTo(spot))
                                reject = "the member's own move guards refuse it: " + LastMoveRefusal();  // [lead112]
                        }
                if (!reject.empty())
                {
                    // [lead81] b124: group_blocked / spot_rejected_aggro every tick (the reason's running numbers, a
                    // stay that flipped between two texts) - the reason without its numbers, logged on a change only
                    std::string const reason = reject.substr(0, reject.find(" ("));
                    lead.spotRejectLogMs[member] = now;
                    std::string& logged = lead.spotRejectLogged[member];
                    if (logged != why + "|" + reason)
                    {
                        logged = why + "|" + reason;
                        Decisions().Write(run, "leader", "spot_rejected_aggro", member, bot->GetName(),
                                                      false,
                                                      JsonObject().Str("member", bot->GetName()).Str("why", why)
                                                          .Str("reject", reject)
                                                          .Str("mob", mob ? mob->GetName() : "-")
                                                          .Real("distance", mob ? mob->GetExactDist(spot) : -1.0f)
                                                          .Real("aggroRange", mob ? mob->GetAggroRange(bot) : -1.0f)
                                                          .Real("margin", Leader::LEADER_SPOT_AGGRO_MARGIN));
                    }
                    // [lead76] user: the leader computes, bigger pulls are better if survivable - a fight whose spots
                    // an idle pack's reach blocks: once per pack, the capacity model (JudgePack) on the fight plus that
                    // pack decides - it fits: the pack joins the pull (its mobs count, no longer block); it does not:
                    // the fight moves away from it (the tank to a spot out of its reach, on the group's side)
                    if (mob && group.FightOn(tank) && !lead.joinedPack.count(mob->GetGUID()))
                    {
                        std::vector<ObjectGuid> combined;
                        for (Unit* attacker : tank->getAttackers())
                            if (attacker && attacker->IsAlive())
                                combined.push_back(attacker->GetGUID());
                        std::list<Creature*> pack;
                        IdleAggroCheck packCheck{tank};
                        Acore::CreatureListSearcher<IdleAggroCheck> packSearcher(mob, pack, packCheck);
                        float const packReach = mob->GetAggroRange(tank);
                        Cell::VisitObjects(mob, packSearcher, packReach);
                        std::vector<ObjectGuid> packMobs{mob->GetGUID()};
                        for (Creature* other : pack)
                            if (other != mob && other->GetExactDist(mob) <= packReach)
                                packMobs.push_back(other->GetGUID());
                        combined.insert(combined.end(), packMobs.begin(), packMobs.end());
                        float overload = 0.0f;
                        std::string judged;
                        bool const fits = PullPlanner::JudgePack(tank, combined, overload, judged);
                        lead.joinedPack[mob->GetGUID()] = fits ? mob->GetGUID() : ObjectGuid::Empty;
                        if (fits)
                        {
                            lead.joinedMobs.insert(packMobs.begin(), packMobs.end());
                            if (Group* group = tank->GetGroup())
                            {
                                std::vector<ObjectGuid> all = LeaderPullMobs(lead.commit, lead.commitMob);
                                all.insert(all.end(), lead.joinedMobs.begin(), lead.joinedMobs.end());
                                Leader::PullPack::Set(group->GetGUID(), all);
                            }
                        }
                        else
                        {
                            // away from the pack, out of its reach (its aggro range + the spot guard's margin)
                            float const dx = tank->GetPositionX() - mob->GetPositionX();
                            float const dy = tank->GetPositionY() - mob->GetPositionY();
                            float const length = std::max(0.1f, std::hypot(dx, dy));
                            float const away = packReach + Leader::LEADER_SPOT_AGGRO_MARGIN;
                            float x = mob->GetPositionX() + dx / length * std::max(away, length);
                            float y = mob->GetPositionY() + dy / length * std::max(away, length);
                            float z = tank->GetPositionZ();
                            if (tank->GetMap()->CheckCollisionAndGetValidCoords(tank, tank->GetPositionX(),
                                                                                tank->GetPositionY(),
                                                                                tank->GetPositionZ(), x, y, z))
                                lead.fightMoveTo = Position(x, y, z);
                        }
                        Decisions().Write(run, "leader", fits ? "pack_joins" : "fight_moves_away",
                                                      mob->GetGUID(), mob->GetName(), true,
                                                      JsonObject().Num("packMobs", int64(packMobs.size()))
                                                          .Real("overload", overload).Str("judged", judged)
                                                          .Str("blocked", bot->GetName()));
                    }
                    // [lead99] b138 rfc-h 339-427 s: the engage's range / meeting spots on the ledge had no walk
                    // ("no walk to the spot"), the members stayed and blocked the group - the order stood open and
                    // nobody could execute it. A walk-along spot with no walk at all (the route search found none) is
                    // no spot: the member follows the tank on the tank's own way (a fact, not a wait)
                    if (noPath && member != tank->GetGUID() && why.rfind("along with the tank", 0) == 0)
                    {
                        if (Leader::Events::SetFollow(member, tank->GetGUID(), 0, "along with the tank"))
                            Decisions().Write(run, "leader", "spot_no_walk_follow", member, bot->GetName(),
                                                          true,
                                                          JsonObject().Str("member", bot->GetName()).Str("why", why));
                        return false;
                    }
                    // [lead77] the group moves as one: a member's walk with the tank refused (an idle mob's reach on
                    // its path, no walk) out of a fight blocks the group - the tank stays until it is clear, the mob
                    // in the way is the next pull
                    if (!group.FightOn(tank) && member != tank->GetGUID() &&
                        why.rfind("along with the tank", 0) == 0)
                    {
                        if (!lead.groupBlockedBy.count(member))
                            Decisions().Write(run, "leader", "group_blocked", member, bot->GetName(), true,
                                                          JsonObject().Str("member", bot->GetName()).Str("why", reject)
                                                              .Str("mob", mob ? mob->GetName() : "-")
                                                              .Str("walk", why));
                        lead.groupBlockedBy[member] = mob ? mob->GetGUID() : ObjectGuid::Empty;
                    }
                    // [lead71] the arbiter's next candidate: the member stays where it is (never a move it refuses)
                    if (move != Leader::MoveEvent::StandAt || spot.GetExactDist(bot->GetPosition()) > 1.0f)
                        Leader::Events::SetMove(member, Leader::MoveEvent::StandAt, bot->GetPosition(), 2.0f,
                                                why + ": stays (" + reason + ")");
                    return false;
                }
                if (again)
                    checked = {spot, now};
            }
            // [lead27] a new command overrides the old one: command_override (old -> new) when its purpose changes or
            // the point jumps (the member's executor reads only the current command, its walk is replaced)
            Leader::MemberEvents before;
            bool const hadMove = Leader::Events::Get(member, before) && before.move != Leader::MoveEvent::None;
            bool const changed = Leader::Events::SetMove(member, move, spot, tolerance, why);
            if (changed)
                Leader::Events::StampPhase(member, lead.movePhase);  // [lead74] the purpose's phase
            // [lead77] its walk is clear again - [lead81] a real walk, not its own stay where it stands
            if (bot && (move != Leader::MoveEvent::StandAt || spot.GetExactDist(bot->GetPosition()) > tolerance))
            {
                lead.groupBlockedBy.erase(member);
                lead.spotRejectLogged.erase(member);
            }
            // [lead55] command_repeat: the new command is the one before the current (a ping-pong) - logged once per
            // member and purpose, counted (should be 0)
            if (changed && hadMove && bot)
            {
                auto const prev = lead.prevMoves.find(member);
                // [lead64] A -> B -> A after a higher-precedence B ended (a run to the tank done) is no ping-pong
                // [lead65] ... and only within one phase (b110: engage -> follow -> a NEW pull's engage was flagged)
                if (prev != lead.prevMoves.end() && prev->second.second == why &&
                    lead.prevMovePhase[member] == lead.movePhase &&
                    prev->second.first.GetExactDist(spot) <= std::max(tolerance, 1.0f) &&
                    Leader::Events::MovePrecedence(before.moveWhy) <= Leader::Events::MovePrecedence(why))
                {
                    uint32& repeats = lead.commandRepeats[bot->GetName() + "|" + why];
                    if (!repeats++)
                        Decisions().Write(run, "leader", "command_repeat", member, bot->GetName(), true,
                                                      JsonObject().Str("member", bot->GetName()).Str("why", why)
                                                          .Str("was", before.moveWhy));
                }
                lead.prevMoves[member] = {before.spot, before.moveWhy};
                lead.prevMovePhase[member] = lead.movePhase;
            }
            if (changed && hadMove && bot &&
                (before.moveWhy != why || before.spot.GetExactDist(spot) > 8.0f))
                Decisions().Write(run, "leader", "command_override", member, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("old", before.moveWhy)
                                                  .Str("new", why).Real("jump", before.spot.GetExactDist(spot)));
            if (changed && bot && (!hadMove || before.moveWhy != why))
                Decisions().Write(run, "leader", "position_owner", member, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("owner", why)
                                                  .Num("precedence", Leader::Events::MovePrecedence(why))
                                                  .Str("was", hadMove ? before.moveWhy : std::string("-")));
            if (changed && bot && move != Leader::MoveEvent::Hold)
                Decisions().Write(run, "leader", "leader_spot", member, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("why", why)
                                                  .Str("ground", LeaderOnCleared(spot) ? "cleared" : "new"));
            return changed;
        };
        // [lead66] b111: the leader rejected its own meeting-point move (in a Trogg's reach) and left the melee member
        // with no command - its own reach melee then walked it. The move, else it stays where it is (StandAt here),
        // else Hold; the alternative is logged
        auto const moveOrStay = [&](Player* bot, Position const& spot, float tolerance, std::string const& why)
        {
            ObjectGuid const member = bot->GetGUID();
            bool const changed = checkedMove(member, Leader::MoveEvent::MoveTo, spot, tolerance, why);
            Leader::MemberEvents now;
            // [lead81] checkedMove's own stay ("<why>: stays (...)") is the answer too: no second stay text flipping
            if (Leader::Events::Get(member, now) && now.moveWhy.rfind(why, 0) == 0)
                return changed;
            std::string const stay = why + ": stays (the spot is in an unpulled mob's reach)";
            bool const stays = checkedMove(member, Leader::MoveEvent::StandAt, bot->GetPosition(), 2.0f, stay);
            if (!(Leader::Events::Get(member, now) && now.moveWhy == stay))
                Leader::Events::SetMove(member, Leader::MoveEvent::Hold, bot->GetPosition(), 0.0f, stay);
            if (stays)
                Decisions().Write(run, "leader", "position_owner", member, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("owner", stay)
                                                  .Str("was", why));
            return stays;
        };
        // [lead83] b126 rfc-h 598.5-603.7 s: Rtshaab's "to the meeting point" was re-derived from where it stood every
        // tick (LeaderMeleeWait) and flipped between two points (command_repeat). Like the range spot (lead68): the
        // melee place is computed once from the leader's meeting point and kept until a fact changes - a new order or
        // fight start (the meeting point moves) or another target
        auto const meleeMeetAt = [&](Player* bot, ObjectGuid target) -> Position
        {
            auto& kept = lead.meleeMeet[bot->GetGUID()];
            if (kept.target != target || kept.meet.GetExactDist(lead.meetPoint) > 1.0f)
                kept = {target, lead.meetPoint, LeaderMeleeWait(bot, lead.meetPoint)};
            return kept.point;
        };
        // a slow-layer call, timed
        auto const slow = [&](auto&& call, std::source_location where = std::source_location::current())
        {
            auto const start = std::chrono::steady_clock::now();
            auto result = call();
            uint32 const us = uint32(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - start).count());
            lead.slowUsTick += us;
            // [lead76] the slowest slow-layer call this minute (its call site's line), logged once a minute
            if (us > lead.slowestPassUs)
            {
                lead.slowestPassUs = us;
                lead.slowestPass = "TestHarnessRunner.cpp:" + std::to_string(where.line());
            }
            return result;
        };
        if (!lead.slowestPassLogMs)
            lead.slowestPassLogMs = now;
        else if (getMSTimeDiff(lead.slowestPassLogMs, now) >= MINUTE * IN_MILLISECONDS && lead.slowestPassUs)
        {
            Decisions().Write(run, "leader", "leader_cadence", tank->GetGUID(), tank->GetName(), false,
                                          JsonObject().Str("slowestPass", lead.slowestPass)
                                              .Num("slowestUs", lead.slowestPassUs));
            lead.slowestPassUs = 0;
            lead.slowestPassLogMs = now;
        }
        auto const wake = [&](Player* bot)
        {
            if (!Leader::LEADER_FAST_REACT)
                return;
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                botAI->SetNextCheckDelay(0);  // the event is executed on its next update, not 1-3 s later
                ++lead.wakeCount;
            }
        };
        auto const logEvent = [&](Player* bot, char const* kind, JsonObject fields)
        {
            ++lead.leaderEventCounts[bot->GetName()];  // [lead27] leader_event per member (run summary)
            fields.Str("member", bot->GetName()).Str("event", kind);
            Decisions().Write(run, "leader", "leader_event", bot->GetGUID(), bot->GetName(), true,
                                          fields);
            wake(bot);
        };
        // where a member will stand: its fresh MoveTo / StandAt spot, else where it is
        auto const plannedAt = [](Player* bot)
        {
            Leader::MemberEvents events;
            if (Leader::Events::Get(bot->GetGUID(), events) &&
                (events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt))
                return events.spot;
            return bot->GetPosition();
        };

        // the tank stands (the group is held): the members prepare
        if (!lead.tankStillSinceMs || tank->GetExactDist(lead.tankStillPos) > 1.0f)
        {
            lead.tankStillPos = tank->GetPosition();
            lead.tankStillSinceMs = now;
        }
        bool const tankStill = getMSTimeDiff(lead.tankStillSinceMs, now) >= LEADER_TANK_STILL_MS;

        std::vector<Player*> seats;
        Player* healer = nullptr;
        bool anySitting = false;
        bool anyDead = false;
        bool rebuff = false;
        bool fighting = group.FightOn(tank);  // [lead84] the one definition (Steer reads the same)
        for (ObjectGuid const& groupMember : group.MemberGuids())
        {
            Player* bot = ObjectAccessor::FindPlayer(groupMember);
            if (!bot || bot == tank || bot->GetMap() != map)
                continue;
            if (!bot->IsAlive())
            {
                anyDead = true;
                continue;
            }
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                continue;
            if (!healer && PlayerbotAI::IsHeal(bot))
                healer = bot;
            anySitting |= bot->IsSitState();
            rebuff |= botAI->forceRebuff.IsPending();
            // [lead37] the boss prep per member: its rebuff window, what the leader commands it, what it does
            {
                std::string prep;
                if (botAI->forceRebuff.IsPending())
                {
                    Leader::MemberEvents prepEvents;
                    Leader::Events::Get(bot->GetGUID(), prepEvents);
                    Spell const* casting = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    std::string const cast = casting && casting->GetSpellInfo()
                                                 ? "casting " + std::string(casting->GetSpellInfo()->SpellName[0])
                                                 : std::string("no cast");
                    prep = Acore::StringFormat("rebuff window open; ready {} move {}; {}{}", int32(prepEvents.ready),
                                               int32(prepEvents.move), cast,
                                               botAI->forceRebuff.IsOnGlobalCooldown() ? ", on the global cooldown"
                                                                                       : "");
                }
                std::string& logged = lead.bossPrepLogged[bot->GetGUID()];
                if (prep != logged)
                {
                    if (!prep.empty() || !logged.empty())
                        Decisions().Write(
                            run, "leader", "boss_prep", bot->GetGUID(), bot->GetName(), false,
                            JsonObject().Str("member", bot->GetName())
                                .Str("state", prep.empty() ? "rebuff window closed" : prep)
                                .Real("openSec", prep.empty() ? 0.0f
                                                              : getMSTimeDiff(botAI->forceRebuff.BeganMs(), now) /
                                                                    1000.0f));
                    logged = prep;
                }
            }
            fighting |= bot->IsInCombat();
            seats.push_back(bot);
        }
        // the healer first: the others' moves keep the reach to where the healer will stand
        std::stable_partition(seats.begin(), seats.end(), [healer](Player* bot) { return bot == healer; });
        // [lead27] this tick asserts every command it wants; what it does not assert ends with the tick (no TTL)
        for (Player* bot : seats)
            Leader::Events::BeginTick(bot->GetGUID());
        Leader::Events::BeginTick(tank->GetGUID());
        struct TickEnd
        {
            std::vector<Player*> const& seats;
            Player* tank;
            uint32 const& phase;
            ~TickEnd()
            {
                // [lead74] the stored position decisions stand until they arrive (generalised lead72)
                for (Player* bot : seats)
                {
                    Leader::Events::HoldMove(bot->GetGUID(), bot->GetPosition(), phase);
                    Leader::Events::EndTick(bot->GetGUID());
                }
                Leader::Events::HoldMove(tank->GetGUID(), tank->GetPosition(), phase);
                Leader::Events::EndTick(tank->GetGUID());
            }
        } tickEnd{seats, tank, lead.movePhase};
        for (ObjectGuid const& groupMember : group.MemberGuids())
            if (Player* dead = ObjectAccessor::FindPlayer(groupMember); dead && !dead->IsAlive())
                Leader::Events::Clear(groupMember);

        Leader::PullOrder order;
        bool const orderOpen = Leader::Orders::GetOpen(group.LeaderGuid(), order);
        constexpr uint32 LEADER_SPOTS_AFTER_ISSUE_MS = 6 * IN_MILLISECONDS;  // the tank's commit comes after
        bool const spotsHeld = Leader::LEADER_ASSIGNS_SPOTS && !lead.spots.empty() && !group.InCombat() &&
                               (lead.spotsWaiting || (orderOpen && order.target == lead.spotsFor) ||
                                (lead.spotsIssuedMs &&
                                 getMSTimeDiff(lead.spotsIssuedMs, now) < LEADER_SPOTS_AFTER_ISSUE_MS));
        if (!spotsHeld && !lead.spots.empty())
        {
            lead.spots.clear();
            lead.spotsFor.Clear();
        }
        // [lead17] an order for another mob runs (the patrol path issued it): the spots of the old pull go, its
        // gate too
        if (orderOpen && !lead.spotsFor.IsEmpty() && order.target != lead.spotsFor &&
            !(lead.commit && lead.commitIssued))
        {
            lead.spots.clear();
            lead.spotsFor.Clear();
            lead.spotsWaiting = false;
        }

        // the fight's target: the tank's, else whatever attacks a member (healer first) or the tank
        // [lead42] b91 rfc-h 203-245 s: every member's Attack on one Trogg was refused for 40 s and the leader kept it.
        // A target the members refused is replaced by another attacker nobody refused (if one fights the group)
        if (!fighting)
            lead.attackRefused.clear();
        auto const refusedTarget = [&](Unit* unit) { return unit && lead.attackRefused.count(unit->GetGUID()) > 0; };
        Unit* target = nullptr;
        if (fighting)
        {
            if (Unit* victim = tank->GetVictim();
                victim && victim->IsAlive() && victim->IsInCombat() && victim->GetMap() == map)
                target = victim;
            std::vector<Player*> attacked = seats;
            attacked.push_back(tank);
            if (refusedTarget(target))
                target = nullptr;
            Unit* refusedFallback = nullptr;
            std::string attackersSig;
            JsonArray attackerList;
            for (Player* bot : attacked)
            {
                for (Unit* attacker : bot->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker->IsInWorld() && attacker->GetMap() == map &&
                        bot->IsValidAttackTarget(attacker) && LeaderFightMob(tank, attacker))
                    {
                        Creature const* creature = attacker->ToCreature();
                        bool const evades = creature && creature->IsInEvadeMode();
                        attackersSig += std::to_string(attacker->GetGUID().GetCounter()) + (evades ? "e" : "") +
                                        (refusedTarget(attacker) ? "r" : "") + ",";
                        attackerList.Raw(JsonObject().Str("name", attacker->GetName())
                                             .Num("entry", attacker->GetEntry())
                                             .Num("low", attacker->GetGUID().GetCounter())
                                             .Str("on", bot->GetName())
                                             .Real("tankDistance", tank->GetExactDist(attacker))
                                             .Real("dz", attacker->GetPositionZ() - tank->GetPositionZ())
                                             .Bool("evades", evades).Bool("tankSees", tank->IsWithinLOSInMap(attacker))
                                             .Num("refusedBy", RefusedCount(lead.attackRefused, attacker))
                                             .Build());
                        if (target)
                            continue;
                        if (refusedTarget(attacker))
                        {
                            if (!refusedFallback)
                                refusedFallback = attacker;
                            continue;
                        }
                        target = attacker;
                    }
            }
            // every attacker refused: the refused one stays the target (the fight always has one, lead38)
            if (!target)
                target = refusedFallback;
            // [lead42] the attacker list on every change (the next stall is visible)
            if (attackersSig != lead.fightStateSig)
            {
                lead.fightStateSig = attackersSig;
                Decisions().Write(run, "leader", "fight_attackers",
                                              target ? target->GetGUID() : ObjectGuid(),
                                              target ? target->GetName() : std::string("-"), true,
                                              JsonObject().Raw("attackers", attackerList.Build())
                                                  .Bool("targetRefused", refusedTarget(target)));
            }
        }

        // the fight's rules around the target: the kill order (the target, then the rest of what fights the group,
        // nearest to the target first), the AoE rule, the intensity, the casting mobs
        std::vector<ObjectGuid> killOrder;
        std::vector<Unit*> casters;
        Leader::AoeRule aoe = Leader::AoeRule::Forbidden;
        std::string aoeWhy;
        Leader::Intensity intensity = Leader::Intensity::Normal;
        if (target)
        {
            std::vector<Unit*> fightingMobs;
            std::set<ObjectGuid> seen;
            std::vector<Player*> attacked = seats;
            attacked.push_back(tank);
            for (Player* bot : attacked)
                for (Unit* attacker : bot->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker->GetMap() == map &&
                        LeaderFightMob(tank, attacker) &&
                        seen.insert(attacker->GetGUID()).second)
                        fightingMobs.push_back(attacker);
            if (seen.insert(target->GetGUID()).second)
                fightingMobs.push_back(target);
            std::sort(fightingMobs.begin(), fightingMobs.end(),
                      [target](Unit* a, Unit* b)
                      {
                          if ((a == target) != (b == target))
                              return a == target;
                          return target->GetExactDist(a) < target->GetExactDist(b);
                      });
            for (Unit* mob : fightingMobs)
            {
                killOrder.push_back(mob->GetGUID());
                if (Spell* spell = mob->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    spell && spell->GetSpellInfo() &&
                    (spell->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT))
                    casters.push_back(mob);
            }
            if (lead.aoeTarget != target->GetGUID() || !lead.aoeMs ||
                getMSTimeDiff(lead.aoeMs, now) >= LEADER_AOE_RECHECK_MS)
            {
                lead.aoeRule = uint8(slow([&] { return LeaderAoeRule(tank, target, lead.aoeWhy); }));
                lead.aoeTarget = target->GetGUID();
                lead.aoeMs = now;
            }
            aoe = Leader::AoeRule(lead.aoeRule);
            aoeWhy = lead.aoeWhy;
            if (Creature* creature = target->ToCreature(); creature && creature->IsDungeonBoss())
                intensity = Leader::Intensity::Burn;
            // [lead80] the fight's expected seconds (its mobs' health over the estimated group dps) and the walk to
            // the nearest standing boss (its distance over the tank's run speed): what each member's burn is judged by
            float health = 0.0f;
            for (Unit* mob : fightingMobs)
                health += float(mob->GetHealth());
            PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
            float const groupDps =
                tankAI ? tankAI->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get() : 0.0f;
            lead.fightSecLeft = health / std::max(groupDps, 1.0f);
            float bossYd = -1.0f;
            for (ObjectGuid const& bossGuid : group.BossCreatures())
                if (Creature* boss = map->GetCreature(bossGuid); boss && boss->IsAlive() && !boss->IsInCombat())
                    if (bossYd < 0.0f || tank->GetExactDist(boss) < bossYd)
                        bossYd = tank->GetExactDist(boss);
            lead.bossWalkSec = bossYd < 0.0f ? -1.0f : bossYd / std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
        }

        // [lead20] the tank holds every fight mob near `to` and the walk from `from` (its victim is the tank, or it is
        // crowd-controlled)
        auto const tankHolds = [&](Position const& from, Position const& to)
        {
            Position const mid((from.GetPositionX() + to.GetPositionX()) / 2.0f,
                               (from.GetPositionY() + to.GetPositionY()) / 2.0f, to.GetPositionZ());
            for (ObjectGuid const guid : killOrder)
            {
                Unit* mob = ObjectAccessor::GetUnit(*tank, guid);
                if (!mob || !mob->IsAlive() || mob->GetVictim() == tank || mob->HasBreakableByDamageCrowdControlAura())
                    continue;
                if (mob->GetExactDist(to) <= 15.0f || mob->GetExactDist(mid) <= 15.0f)
                    return false;
            }
            return true;
        };
        Position healerAt = healer ? plannedAt(healer) : Position();
        auto const reaches = [tank](Position const& from, Position const& to) { return HealReaches(tank, from, to); };
        // [lead120] where the tank fights: its fight anchor in a fight (moved on with it, LEADER_ANCHOR_MOVE_YD), else
        // where it stands - a heal-reach spot is judged against it, not against the tank's every step
        auto const tankFightAt = [&]()
        { return fighting && lead.fightAnchorSerial ? lead.fightAnchor : tank->GetPosition(); };
        // the healer at `spot` reaches every member (where it will stand) and the tank
        auto const healerKeeps = [&](Position const& spot)
        {
            // [lead18] the tank first: within LEADER_HEALER_TANK_YD of where it fights and in sight
            Position const tankAt = tankFightAt();
            if (!reaches(spot, tankAt) ||
                spot.GetExactDist(tankAt) > Leader::LEADER_HEALER_TANK_YD - g_spotAcquireMargin)
                return false;
            // [lead38] a held spot (no acquire margin) keeps a member it reaches where it stands or where it goes:
            // b87 rfc-h 23.6-24.0 s, 6 "heal reach" re-spots in 0.4 s while the counterparts' planned spots moved
            for (Player* other : seats)
                if (other != healer && !reaches(spot, plannedAt(other)) &&
                    !(g_spotAcquireMargin == 0.0f && reaches(spot, other->GetPosition())))
                    return false;
            return true;
        };
        auto const keepsHealer = [&](Position const& spot)
        {
            return !Leader::LEADER_HEAL_REACH || !healer || reaches(healerAt, spot) ||
                   (g_spotAcquireMargin == 0.0f && reaches(healer->GetPosition(), spot));
        };

        // [lead15] the aggro-loss rule (user: «если кто-то кого-то сагрил - подбегай к танку, пусть танк АОЕ делает и
        // снимает агр с тебя»): a mob on a member (not the tank) - the victim runs next to the tank (in its taunt /
        // AoE reach, in heal reach) and stops its damage, the tank takes the mob back (a taunt when ready, else an
        // AoE threat spell, else it attacks it), the others leave the mob alone until the tank has it; the healer
        // keeps the victim up (Heal(group)). Logged: leader_aggro_loss -> the commands -> leader_aggro_recovered.
        std::map<ObjectGuid, Unit*> looseOn;  // victim -> the loose mob on it
        std::set<ObjectGuid> looseMobs;
        if (fighting)
        {
            for (Player* bot : seats)
                for (Unit* attacker : bot->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker->GetVictim() == bot && attacker->ToCreature() &&
                        LeaderFightMob(tank, attacker) &&
                        !looseOn.count(bot->GetGUID()))
                    {
                        looseOn[bot->GetGUID()] = attacker;
                        looseMobs.insert(attacker->GetGUID());
                    }
            for (auto const& [victimGuid, mob] : looseOn)
                if (!lead.looseMobs.count(mob->GetGUID()))
                {
                    lead.looseMobs[mob->GetGUID()] = {victimGuid, now};
                    Player* victim = ObjectAccessor::FindPlayer(victimGuid);
                    Decisions().Write(run, "leader", "leader_aggro_loss", mob->GetGUID(), mob->GetName(),
                                                  true,
                                                  JsonObject().Str("mob", mob->GetName()).Num("entry", mob->GetEntry())
                                                      .Str("victim", victim ? victim->GetName() : "?")
                                                      .Real("tankDistance",
                                                            victim ? victim->GetExactDist(tank) : -1.0f));
                }
        }
        for (auto it = lead.looseMobs.begin(); it != lead.looseMobs.end();)
        {
            if (looseMobs.count(it->first))
            {
                ++it;
                continue;
            }
            Unit* mob = ObjectAccessor::GetUnit(*tank, it->first);
            char const* how = !mob || !mob->IsAlive()       ? "the mob died"
                              : mob->GetVictim() == tank    ? "the tank has it back"
                              : !mob->IsInCombat()          ? "the fight ended"
                                                            : "it left the member";
            Decisions().Write(run, "leader", "leader_aggro_recovered", it->first,
                                          mob ? mob->GetName() : std::string("?"), true,
                                          JsonObject().Str("how", how)
                                              .Real("seconds", float(getMSTimeDiff(it->second.second, now)) / 1000.0f));
            lead.tauntLogged.erase(it->first);  // [lead54] the episode ended
            lead.threatDoneFor.erase(it->first);  // [lead56]
            it = lead.looseMobs.erase(it);
        }
        if (looseOn.empty())
            lead.tankCannotTake = false;
        if (!looseOn.empty())
        {
            // the tank: the first loose mob (its taunt when ready, else its AoE threat, else an attack)
            Unit* mob = looseOn.begin()->second;
            // [lead112] b147 rfc-h 60-78 s: the tank owns its fight, but a mob on a member while the tank fights
            // nothing is the leader's command: the tank attacks it (issued once per change of the mob)
            if (Leader::LEADER_TANK_OWNS_FIGHT && !tank->IsInCombat() &&
                Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack, mob->GetGUID(), {}))
                logEvent(tank, "Attack", JsonObject().Str("mob", mob->GetName())
                                             .Str("why", "aggro loss: the tank fights nothing"));
            // [lead54] b103 rfc-h pull 8 (wipe): 10 754 "taunt 31789" (Righteous Defense) commands - it is cast on
            // the friend, the executor casts the leader's taunt on the mob: it never went off and stayed "ready". The
            // taunts cast on the mob (Hand of Reckoning, Taunt, Growl, Dark Command); none ready: the AoE threat, else
            // the attack
            uint32 spell = LeaderReadySpell(tank, {TANK_TAUNTS[0], TANK_TAUNTS[1], TANK_TAUNTS[2], TANK_TAUNTS[3]});
            char const* how = "taunt";
            if (!spell)
            {
                for (uint32 aoeSpell : TANK_AOE_THREAT)
                    if (tank->HasSpell(aoeSpell) && !tank->HasSpellCooldown(aoeSpell))
                        spell = aoeSpell;
                how = spell ? "aoe threat" : "attack";
            }
            // [lead56] b105 rfc-h pull 7 wipe: the tank stood in "leader threat" with no target (its rotation never
            // ran, no threat, every damage dealer without headroom). A pick-up is one cast: issued only with a ready
            // spell and only until the tank reports it (its ack), never as a standing state; the tank's Attack on the
            // mob stays - it attacks the mob it took back and runs its rotation
            auto const done = lead.threatDoneFor.find(mob->GetGUID());
            bool const castDone = done != lead.threatDoneFor.end() && done->second == spell;
            bool const threatChanged = !Leader::LEADER_TANK_OWNS_FIGHT && spell && !castDone &&
                                       Leader::Events::SetThreat(tank->GetGUID(), spell, mob->GetGUID());
            bool const attackChanged =
                !Leader::LEADER_TANK_OWNS_FIGHT &&
                Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack, mob->GetGUID(), {});
            // [lead54] issued (logged, the tank woken) once per change of the pick-up: a new loose mob or a new spell
            // (the taunt came off cooldown / went on it) - not every tick the loose state flickers
            auto const logged = lead.tauntLogged.find(mob->GetGUID());
            if ((threatChanged || attackChanged) && (logged == lead.tauntLogged.end() || logged->second != spell))
            {
                lead.tauntLogged[mob->GetGUID()] = spell;
                logEvent(tank, "Taunt", JsonObject().Str("mob", mob->GetName()).Num("spell", spell).Str("how", how));
            }
            // [lead54] the tank can take it back now only with a ready taunt / AoE threat: else the damage dealers
            // with threat headroom kill it (no DontAttack while the healer dies)
            lead.tankCannotTake = spell == 0;
            // [lead36] b85 rfc-h: the tank stood 7.3 yd off the Trogg on the healer ("0 attackers ... going idle"):
            // the leader's Taunt / Attack includes the walk to melee - a MoveTo to the mob while out of its melee
            // reach (re-issued only when the mob moved off the issued point)
            // [lead39] b88 rfc-h pull 6 (112-183 s): the walk to the loose Trogg dropped the tank 6.9 yd off a
            // ledge ("[ledge drop] ... shortcut_drop leader move order") into the unpulled Troggs below (12 came for 2
            // planned). The walk only on the tank's own floor along a path with no drop; else the taunt / AoE alone
            if (!Leader::LEADER_TANK_OWNS_FIGHT && tank->IsAlive() && !tank->IsWithinMeleeRange(mob) &&
                LeaderWalkStaysOnFloor(tank, mob->GetPosition()))
            {
                Leader::MemberEvents tankEvents;
                bool const has = Leader::Events::Get(tank->GetGUID(), tankEvents) &&
                                 tankEvents.move == Leader::MoveEvent::MoveTo &&
                                 tankEvents.moveWhy == "aggro loss: to the mob";
                // [lead50] b99 rfc-h 245.3-246.1 s: ~15 "aggro loss: to the mob" moves in 1 s (a new point every tick).
                // The point changes only when the mob left it by the walk's own tolerance; logged once per loose mob
                float const reachTolerance = tank->GetCombatReach() + mob->GetCombatReach();
                bool const keepPoint = has && tankEvents.spot.GetExactDist(mob) <= reachTolerance;
                Position const point = keepPoint ? tankEvents.spot : mob->GetPosition();
                if (checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, point, reachTolerance,
                                "aggro loss: to the mob") &&
                    lead.dragFor != mob->GetGUID())
                {
                    lead.dragFor = mob->GetGUID();
                    logEvent(tank, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", "aggro loss: to the mob").Str("who", "tank")
                                 .Str("mob", mob->GetName()).Real("distance", tank->GetExactDist(mob)));
                    LogFightDrag(run, tank, point, lead.meetPoint, "aggro loss: to the mob");
                }
            }
            // the others' target is never a loose mob
            // [lead38] b87 rfc-h 507-600 s: the one Earthborer of the fight was loose on the healer (12.5 yd below
            // it, the tank never reached it): no target, nobody attacked for 90 s. A fight always has an Attack
            // target: with every fight mob loose, the loose mob is the target (the damage dealers kill it)
            Unit* const looseTarget = target && looseMobs.count(target->GetGUID()) ? target : nullptr;
            if (looseTarget && !lead.tankCannotTake)
            {
                target = nullptr;
                for (ObjectGuid const guid : killOrder)
                    if (!looseMobs.count(guid))
                        if (Unit* other = ObjectAccessor::GetUnit(*tank, guid))
                        {
                            target = other;
                            break;
                        }
            }
            if (!target || lead.tankCannotTake)
            {
                target = target ? target : looseTarget ? looseTarget : mob;
                lead.looseIsTarget = true;
            }
            else
            {
                lead.looseIsTarget = false;
                killOrder.erase(std::remove_if(killOrder.begin(), killOrder.end(),
                                               [&looseMobs](ObjectGuid guid) { return looseMobs.count(guid) > 0; }),
                                killOrder.end());
            }
        }

        // [lead16] LEADER_CC_ASSIGN: the fight's crowd control - which mob, which member. A mob a member already
        // marked for the pack (the pre-pull plan's claim) stays its; more come from the tail of the kill order (never
        // the target, a boss, a backed-off or a group kill target) for a member that can hold it and reaches it (same
        // floor, sight: CrowdControlMarkReachable). The CC'd mobs leave the kill order.
        if (fighting && Leader::LEADER_CC_ASSIGN && target)
        {
            for (auto it = lead.ccAssign.begin(); it != lead.ccAssign.end();)
            {
                Unit* mob = ObjectAccessor::GetUnit(*tank, it->first);
                Player* member = ObjectAccessor::FindPlayer(it->second);
                if (!mob || !mob->IsAlive() || !mob->IsInCombat() || !member || !member->IsAlive() ||
                    mob == target || looseOn.count(member->GetGUID()))
                    it = lead.ccAssign.erase(it);
                else
                    ++it;
            }
            std::set<ObjectGuid> controlling;
            for (auto const& [mob, member] : lead.ccAssign)
                controlling.insert(member);
            for (Player* bot : seats)
                if (bot != healer && !controlling.count(bot->GetGUID()))
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        if (Unit* marked = GetAutoCcTarget(botAI);
                            marked && marked->IsAlive() && marked != target && !lead.ccAssign.count(marked->GetGUID()))
                        {
                            lead.ccAssign[marked->GetGUID()] = bot->GetGUID();
                            controlling.insert(bot->GetGUID());
                        }
            PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
            bool const hardPack =
                tankAI && tankAI->GetAiObjectContext()->GetValue<bool>("auto cc hard pack")->Get();
            size_t const wanted = !hardPack ? 0 : killOrder.size() >= 5 ? 2 : 1;
            for (size_t i = killOrder.size(); i-- > 2 && lead.ccAssign.size() < wanted;)
            {
                Unit* mob = ObjectAccessor::GetUnit(*tank, killOrder[i]);
                Creature* creature = mob ? mob->ToCreature() : nullptr;
                // [lead28] by state only (no 15 s break backoff): not in the group's fight, nothing on it that breaks
                // the hold, and the hold spell's diminishing returns not at immune (the game's DR state)
                if (!creature || mob == target || lead.ccAssign.count(mob->GetGUID()) || creature->IsDungeonBoss() ||
                    IsGroupKillTarget(tank, mob))
                    continue;
                for (Player* bot : seats)
                {
                    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                    if (bot == healer || !botAI || controlling.count(bot->GetGUID()) || looseOn.count(bot->GetGUID()) ||
                        !CrowdControlMarkReachable(bot, mob))
                        continue;
                    CrowdControlOption const* option = FindCrowdControl(botAI, mob, false);
                    if (!option || CrowdControlWouldBreak(botAI, mob))
                        continue;
                    if (SpellInfo const* hold = GetCrowdControlHoldInfo(botAI, *option))
                    {
                        DiminishingGroup const group = GetDiminishingReturnsGroupForSpell(hold, false);
                        if (group != DIMINISHING_NONE && mob->GetDiminishing(group) >= DIMINISHING_LEVEL_IMMUNE)
                            continue;
                    }
                    lead.ccAssign[mob->GetGUID()] = bot->GetGUID();
                    controlling.insert(bot->GetGUID());
                    break;
                }
            }
            for (auto const& [mobGuid, memberGuid] : lead.ccAssign)
            {
                Player* member = ObjectAccessor::FindPlayer(memberGuid);
                Unit* mob = ObjectAccessor::GetUnit(*tank, mobGuid);
                if (!member || !mob)
                    continue;
                if (Leader::Events::SetCc(memberGuid, mobGuid))
                    logEvent(member, "Cc", JsonObject().Str("mob", mob->GetName()).Num("entry", mob->GetEntry())
                                               .Bool("held", IsHeldByCrowdControl(mob, 0)));
                killOrder.erase(std::remove(killOrder.begin(), killOrder.end(), mobGuid), killOrder.end());
            }
        }
        else if (!fighting)
            lead.ccAssign.clear();

        // [lead16] LEADER_PRESAVE: a big hit foreseen - a heavy cast (its damage >= LEADER_PRESAVE_HIT_PCT of the
        // victim's health) of a fight mob on a member or the tank, an enraged boss on the tank, the tank low under a
        // boss: PreSave to that bot until the hit lands (a decision line leader_presave with the reason)
        if (fighting && Leader::LEADER_PRESAVE)
        {
            std::vector<Player*> everyone = seats;
            everyone.push_back(tank);
            auto const preSave = [&](Player* who, uint32 untilMs, std::string const& why)
            {
                if (!who || !who->IsAlive())
                    return;
                if (Leader::Events::SetPreSave(who->GetGUID(), untilMs, why))
                {
                    Decisions().Write(run, "leader", "leader_presave", who->GetGUID(), who->GetName(),
                                                  true, JsonObject().Str("member", who->GetName()).Str("why", why)
                                                            .Num("inMs", getMSTimeDiff(now, untilMs)));
                    wake(who);
                }
            };
            for (ObjectGuid const guid : killOrder)
            {
                Unit* mob = ObjectAccessor::GetUnit(*tank, guid);
                Spell* spell = mob ? mob->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
                SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
                if (!info || info->IsPositive())
                    continue;
                int32 damage = 0;
                bool area = false;
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (info->Effects[i].Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                        info->Effects[i].Effect == SPELL_EFFECT_WEAPON_DAMAGE ||
                        info->Effects[i].Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG ||
                        info->Effects[i].Effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE)
                    {
                        damage += info->Effects[i].CalcValue(mob);
                        area |= info->Effects[i].HasRadius();
                    }
                if (damage <= 0)
                    continue;
                uint32 const until = now + uint32(std::max(0, spell->GetCastTime() > 0 ? int32(spell->GetCastTime())
                                                                                        : 0)) +
                                     IN_MILLISECONDS;
                std::string const why = Acore::StringFormat("{} casts {} ({} damage)", mob->GetName(),
                                                            info->SpellName[LOCALE_enUS], damage);
                Unit* victim = spell->m_targets.GetUnitTarget();
                for (Player* bot : everyone)
                {
                    bool const hit = victim == bot ||
                                     (area && bot->GetExactDist(victim ? victim : mob) <=
                                                  info->Effects[EFFECT_0].CalcRadius(mob) + bot->GetCombatReach());
                    if (hit && float(damage) >= float(bot->GetMaxHealth()) * Leader::LEADER_PRESAVE_HIT_PCT / 100.0f)
                        preSave(bot, until, why);
                }
            }
            if (Unit* victim = tank->GetVictim();
                victim && victim->ToCreature() && victim->ToCreature()->IsDungeonBoss())
            {
                bool enraged = false;
                for (auto const& [id, application] : victim->GetAppliedAuras())
                    if (application && application->GetBase() && application->GetBase()->GetSpellInfo()->Dispel ==
                                                                      DISPEL_ENRAGE)
                        enraged = true;
                if (enraged)
                    preSave(tank, now + 2 * IN_MILLISECONDS, victim->GetName() + " is enraged");
                else if (tank->GetHealthPct() < Leader::LEADER_PRESAVE_TANK_PCT && victim->GetVictim() == tank)
                    preSave(tank, now + 2 * IN_MILLISECONDS,
                            Acore::StringFormat("the tank at {:.0f} % under {}", tank->GetHealthPct(),
                                                victim->GetName()));
            }
        }

        // [lead16] LEADER_HOLD_EVENTS: no target of the fight takes damage (immune: a boss phase) - the next one
        // that does, else StopDps for everyone
        bool stopDps = false;
        if (fighting && Leader::LEADER_HOLD_EVENTS && target && target->IsImmunedToDamage(SPELL_SCHOOL_MASK_ALL))
        {
            Unit* other = nullptr;
            for (ObjectGuid const guid : killOrder)
                if (Unit* mob = ObjectAccessor::GetUnit(*tank, guid);
                    mob && mob != target && mob->IsAlive() && !mob->IsImmunedToDamage(SPELL_SCHOOL_MASK_ALL))
                {
                    other = mob;
                    break;
                }
            if (other)
                target = other;
            else
                stopDps = true;
        }
        // [lead16] LEADER_TANK_EXECUTES: the tank's target (no aggro loss: its own victim, else the kill order's head)
        if (fighting && Leader::LEADER_TANK_EXECUTES && !Leader::LEADER_TANK_OWNS_FIGHT && looseOn.empty() &&
            Leader::LEADER_ATTACK_EVENTS)
        {
            Unit* tankTarget = tank->GetVictim();
            if (!tankTarget || !tankTarget->IsAlive() || !tankTarget->IsInCombat() ||
                lead.ccAssign.count(tankTarget->GetGUID()))
                tankTarget = target;
            if (tankTarget && Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack,
                                                        tankTarget->GetGUID(), killOrder))
                logEvent(tank, "Attack", JsonObject().Str("target", tankTarget->GetName())
                                             .Num("targetEntry", tankTarget->GetEntry()).Str("who", "tank"));
        }
        // [lead16] LEADER_LOOT_EVENTS: when a fight ends, one member (a damage dealer) loots for LEADER_LOOT_MS
        if (Leader::LEADER_LOOT_EVENTS)
        {
            if (lead.lastFighting && !fighting)
            {
                lead.looter.Clear();
                for (Player* bot : seats)
                    if (bot != healer)
                    {
                        lead.looter = bot->GetGUID();
                        break;
                    }
                lead.lootSinceMs = now;
            }
            if (fighting || (lead.lootSinceMs && getMSTimeDiff(lead.lootSinceMs, now) > Leader::LEADER_LOOT_MS))
                lead.looter.Clear();
            if (!lead.looter.IsEmpty())
                if (Player* looter = ObjectAccessor::FindPlayer(lead.looter))
                    if (Leader::Events::SetLoot(lead.looter, true))
                        logEvent(looter, "Loot", JsonObject().Str("why", "the fight is over"));
        }
        // [lead39] b88 rfc-h pull 6: extra_aggro read "fightSpotDistance 103.8" against the meeting point of an
        // earlier pull - an unplanned fight meets where the tank stands when it starts
        if (fighting && !lead.lastFighting && !(lead.commit && lead.commitIssued))
            lead.meetPoint = tank->GetPosition();
        // [lead88] the fight's estimated length at its start (the planner's published next fight) against its real
        // length at its end: the measured uncertainty the mana lines' reserve is computed from
        if (fighting && !lead.lastFighting)
        {
            RestState::NextFight next;
            lead.fightLenStartMs = now;
            lead.fightLenEstSec = RestState::PredictNextFight(tank, next)
                                      ? (next.fightSec > 0.0f ? next.fightSec
                                                              : RestState::FIGHT_BASE_SEC +
                                                                    RestState::FIGHT_SEC_PER_MOB * float(next.mobs))
                                      : -1.0f;
        }
        else if (!fighting && lead.lastFighting && lead.fightLenStartMs && lead.fightLenEstSec >= 0.0f)
        {
            float const realSec = getMSTimeDiff(lead.fightLenStartMs, now) / 1000.0f;
            if (Group* group = tank->GetGroup())
                RestState::NoteFightLengthError(group->GetGUID(), std::fabs(realSec - lead.fightLenEstSec));
            Decisions().Write(run, "leader", "fight_length", ObjectGuid(), tank->GetName(), false,
                                          JsonObject().Real("estimatedSec", lead.fightLenEstSec)
                                              .Real("realSec", realSec));
            lead.fightLenStartMs = 0;
        }
        lead.lastFighting = fighting;
        // [lead84] b127 rfc-h 206-306 s: a Ragefire Trogg 5.5 yd below on Rtdruaa (not evading, the tank out of its
        // sight, the attacks refused "no line of sight") kept the tank flagged in combat with nobody on it; the
        // harness counted the fight over at 221 s, the leader stayed in its fight mode and issued nothing for 85 s.
        // Out of a fight (Party::FightOn) with the group still flagged: the lingering mob set is logged on a change;
        // an attackable one (alive, not evading) the tank walks to (checked); refused, it is unreachable - the
        // leader's out-of-combat flow goes on and the harness's hunt for it ends (its stale combat ends as an
        // evading mob's would)
        bool lingeringWalk = false;
        if (!fighting && tank->IsInCombat())
        {
            std::vector<Player*> flagged = seats;
            flagged.push_back(tank);
            std::set<ObjectGuid> listed;
            std::string sig;
            JsonArray refs;
            Creature* lingering = nullptr;
            for (Player* bot : flagged)
                for (auto const& [refGuid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                {
                    Unit* other = ref ? ref->GetOther(bot) : nullptr;
                    Creature* mob = other ? other->ToCreature() : nullptr;
                    if (!mob || !mob->IsAlive() || mob->GetMap() != map || !listed.insert(mob->GetGUID()).second)
                        continue;
                    bool const evades = mob->IsInEvadeMode();
                    sig += std::to_string(mob->GetGUID().GetCounter()) + (evades ? "e" : "") + ",";
                    Unit* victim = mob->GetVictim();
                    refs.Raw(JsonObject().Str("name", mob->GetName()).Num("low", mob->GetGUID().GetCounter())
                                 .Str("victim", victim ? victim->GetName() : "-").Bool("evades", evades)
                                 .Real("tankDistance", tank->GetExactDist(mob))
                                 .Real("dz", mob->GetPositionZ() - tank->GetPositionZ())
                                 .Bool("tankSees", tank->IsWithinLOSInMap(mob)).Bool("moving", mob->isMoving())
                                 .Build());
                    if (!evades && tank->IsValidAttackTarget(mob) &&
                        (!lingering || tank->GetExactDist(mob) < tank->GetExactDist(lingering)))
                        lingering = mob;
                }
            if (sig != lead.lingeringSig)
            {
                lead.lingeringSig = sig;
                Decisions().Write(run, "leader", "lingering_combat",
                                              lingering ? lingering->GetGUID() : ObjectGuid(),
                                              lingering ? lingering->GetName() : std::string("-"), true,
                                              JsonObject().Raw("mobs", refs.Build())
                                                  .Bool("harnessFight", group.InCombat())
                                                  .Num("tankAttackers", int64(tank->getAttackers().size())));
            }
            if (lingering && lead.lingeringUnreachable != lingering->GetGUID())
            {
                float const pickUp = LeaderTauntReach(tank);
                float const arrive = pickUp > 0.0f ? pickUp : NOMINAL_MELEE_RANGE;
                if (!(tank->GetExactDist(lingering) <= arrive && tank->IsWithinLOSInMap(lingering)))
                {
                    std::string const why = "lingering combat: the tank goes to the mob";
                    checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, lingering->GetPosition(), arrive, why);
                    Leader::MemberEvents tankNow;
                    lingeringWalk = Leader::Events::Get(tank->GetGUID(), tankNow) && tankNow.moveWhy == why;
                    if (!lingeringWalk)
                    {
                        lead.lingeringUnreachable = lingering->GetGUID();
                        if (group.PhantomHunt() == lingering->GetGUID())
                            group.ClearPhantomHunt();
                        Decisions().Write(run, "leader", "lingering_unreachable", lingering->GetGUID(),
                                                      lingering->GetName(), true,
                                                      JsonObject().Str("why", tankNow.moveWhy)
                                                          .Real("tankDistance", tank->GetExactDist(lingering)));
                    }
                }
            }
        }
        else
        {
            lead.lingeringSig.clear();
            lead.lingeringUnreachable.Clear();
        }
        // [lead34] the route is data: its waypoint for the tank becomes the leader's MoveTo ("route walk") while no
        // pull is committed and no fight is on (the special waypoints stay the tank's own walk: a jump, a straight
        // line, a gate pass)
        if (fighting)
            lead.groupBlockedBy.clear();
        // [lead82] b125 rfc-h 517.3-616 s: Rtmagaa's "to range of the engage" was blocked ("no walk to the spot"), the
        // engage ended at 520.2 and the block stayed - the Taragaman pull waited "a member cannot follow" 70 s. A block
        // holds only while the member's current command is still that walk's stay
        for (auto it = lead.groupBlockedBy.begin(); it != lead.groupBlockedBy.end();)
        {
            Leader::MemberEvents blocked;
            bool const holds = Leader::Events::Get(it->first, blocked) && blocked.move == Leader::MoveEvent::StandAt &&
                               blocked.moveWhy.rfind("along with the tank", 0) == 0 &&
                               blocked.moveWhy.find(": stays (") != std::string::npos;
            if (holds)
                ++it;
            else
            {
                lead.spotRejectLogged.erase(it->first);
                it = lead.groupBlockedBy.erase(it);
            }
        }
        // [lead94] travel per route leg (the route target): its real seconds, the tank's moving seconds and every stop
        // by its cause, logged when the leg ends (travel_leg)
        {
            float const dt = lead.travelTickMs ? getMSTimeDiff(lead.travelTickMs, now) / 1000.0f : 0.0f;
            lead.travelTickMs = now;
            if (group.RouteTarget() != lead.travelTarget)
            {
                if (!lead.travelTarget.IsEmpty() && lead.travelSec > 0.0f)
                {
                    JsonObject stops;
                    for (auto const& [cause, sec] : lead.travelStops)
                        stops.Real(cause.c_str(), sec);
                    Decisions().Write(run, "leader", "travel_leg", lead.travelTarget, tank->GetName(),
                                                  false,
                                                  JsonObject().Real("straight",
                                                                    lead.travelFrom.GetExactDist(tank->GetPosition()))
                                                      .Real("sec", lead.travelSec).Real("moving", lead.travelMoving)
                                                      .Real("fights", lead.travelFight).Raw("stops", stops.Build())
                                                      .Str("pathSource", lead.routePathSource)
                                                      .Bool("pathComplete", lead.routePathComplete));
                }
                lead.travelTarget = group.RouteTarget();
                lead.travelFrom = tank->GetPosition();
                lead.travelSec = lead.travelMoving = lead.travelFight = 0.0f;
                lead.travelStops.clear();
            }
            else if (dt > 0.0f && dt < 5.0f && !group.RouteTarget().IsEmpty())
            {
                lead.travelSec += dt;
                if (fighting)
                    lead.travelFight += dt;
                else if (tank->isMoving())
                    lead.travelMoving += dt;
                else
                {
                    LeaderGroup::WalkPoint routePoint;
                    std::string const cause =
                        lead.commit ? (lead.spotsWaiting ? "pull: the spots wait" : "pull committed")
                        : !lead.groupBlockedBy.empty() ? "a member's walk blocked"
                        : !lead.trailing.empty()       ? "a member trails"
                        : anySitting || tank->IsSitState() ? "rest"
                        : group.NeedBuff() || rebuff     ? "buffs"
                        : !lead.routeFirstFor.IsEmpty() ? "route first"
                        : !group.GetWaypoint(tank->GetGUID(), routePoint) ? "no route point"
                                                                                : "other";
                    lead.travelStops[cause] += dt;
                }
            }
        }
        // [lead91] a fight or a pull moves the tank: the kept route point is judged afresh after it
        if (fighting || lead.commit)
        {
            lead.routeWalkPoint.reset();
            lead.routePath.clear();  // [lead97] the leg's path is taken again from where the tank stands after it
        }
        // [lead77] the tank walks on only where the group follows: no route walk while a member is blocked
        if (Leader::LEADER_ROUTE_IS_DATA && !fighting && !lead.commit && !Leader::Orders::IsOpen(group.LeaderGuid()) &&
            lead.groupBlockedBy.empty() && !lingeringWalk)
        {
            LeaderGroup::WalkPoint waypoint;
            if (group.GetWaypoint(tank->GetGUID(), waypoint) && waypoint.mapId == tank->GetMapId() &&
                !waypoint.jump && !waypoint.straight && !waypoint.gatePass)
            {
                Position point(waypoint.x, waypoint.y, waypoint.z);
                // [lead91] b133 rfc-h 304.4-308.5 s: the route point was recomputed every second from where the
                // tank walked and stepped back (-165.5 -> -173.3: command_repeat). One command per route point:
                // the point the tank walks to is kept until it reaches it, a new one is taken when it lies further
                // along (nearer the route target) or the route itself changes (a new target) - never a step back
                // [lead94] b135: the straight-line "nearer the target" test held the kept point on a winding route
                // until the tank reached it - a stop every few yards. The test is the walk's own direction: a new
                // point on the kept point's side of the tank is ahead (taken at once, the walk goes on); one behind
                // the tank waits until the kept point is reached
                // [lead97] b137 rfc-h 297.9-300.9 s: the harness's per-second recompute went back again (-167.1 ->
                // -174.2, still "ahead" of the tank) - command_repeat at b133's place. The leg's path is taken ONCE
                // (the navmesh path to the route target) and walked by index: the next path point when the tank
                // reaches the current one; a new path only when the leg's target changes, the path is walked to its
                // end, or the tank stands off it by more than its arrival tolerance (a fight / a pull resets it too)
                constexpr float ROUTE_ARRIVED_YD = 1.5f;  // the route walk's MoveTo tolerance
                Unit* legTarget =
                    group.RouteTarget().IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, group.RouteTarget());
                if (!lead.routePath.empty() && lead.routePathIndex < lead.routePath.size() &&
                    lead.routePathIndex > 0)
                {
                    // off the path: farther from the current segment than the arrival tolerance + the tank's reach
                    G3D::Vector3 const& a = lead.routePath[lead.routePathIndex - 1];
                    G3D::Vector3 const& b = lead.routePath[lead.routePathIndex];
                    G3D::Vector3 const p(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
                    G3D::Vector3 const ab = b - a;
                    float const len2 = ab.squaredLength();
                    float const k = len2 > 0.0f ? std::clamp((p - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
                    if ((a + ab * k - p).length() > ROUTE_ARRIVED_YD + tank->GetCombatReach())
                        lead.routePath.clear();
                }
                if (legTarget && (lead.routePathTarget != group.RouteTarget() || lead.routePath.empty() ||
                                  lead.routePathIndex >= lead.routePath.size()))
                {
                    // [lead98] b138 rfc-h 335-427 s: from the Oggleflint ledge the plain navmesh path to the route
                    // target was partial and the walk stood at its end - the route is data the leader reads: the
                    // harness's own route leg (SteerLeg: its long route search, door and detour legs, the route
                    // links), taken once per leg and walked by index
                    // [lead99] the leader's own route search (Ai/Leader/Travel, Leader::Route): the leg's corners
                    Leader::RouteResult const leg = Leader::Route(tank, legTarget->GetPosition());
                    lead.routePath.clear();
                    if (leg.found && leg.corners.size() >= 2)
                        lead.routePath = leg.corners;
                    // the leg path's source and completeness, for travel_leg
                    lead.routePathSource = leg.Describe();
                    lead.routePathComplete = leg.found && leg.complete;
                    lead.routePathIndex = 0;  // the arrived points are skipped below
                    lead.routePathTarget = group.RouteTarget();
                }
                while (!lead.routePath.empty() && lead.routePathIndex + 1 < lead.routePath.size() &&
                       tank->GetExactDist2d(lead.routePath[lead.routePathIndex].x,
                                            lead.routePath[lead.routePathIndex].y) <= ROUTE_ARRIVED_YD)
                    ++lead.routePathIndex;
                if (!lead.routePath.empty() && lead.routePathIndex < lead.routePath.size())
                {
                    G3D::Vector3 const& next = lead.routePath[lead.routePathIndex];
                    point.Relocate(next.x, next.y, next.z);
                }
                lead.routeWalkPoint = point;
                lead.routeWalkTarget = group.RouteTarget();
                // [lead73] b117 271.4 s: the route walk woke a Trogg ("would wake ..., walked anyway") - it goes
                // through the leader's checks and the tank's own guards run dry (checkedMove); refused, the tank
                // stays and the mob on the path is the next pull
                if (checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, point, 1.5f, "route walk") &&
                    lead.routeWalkSerial != waypoint.serial)
                {
                    lead.routeWalkSerial = waypoint.serial;
                    logEvent(tank, "RouteWalk", JsonObject().Str("who", "tank").Real("x", point.GetPositionX())
                                                    .Real("y", point.GetPositionY()).Real("z", point.GetPositionZ()));
                }
            }
        }
        // [lead34] b83 rfc-h 20261031-152252 (98-180 s): an engage on a Ragefire Trogg 64 yd out of the tank's sight,
        // the straight walk for sight refused, «walks its path in» walked nothing and the order ended unbegun. The
        // open engage order out of sight: the leader walks the tank's path in toward the mob until it sees it (then
        // the tank's executor takes its next step, the attack)
        if (Leader::LEADER_ROUTE_IS_DATA && !Leader::LEADER_TANK_WALKS_PULL && !fighting)
        {
            Leader::PullOrder order;
            if (Leader::Orders::GetOpen(group.LeaderGuid(), order) && order.method == Leader::PullMethod::Engage &&
                order.state == Leader::OrderState::Issued)
                if (Unit* mob = ObjectAccessor::GetUnit(*tank, order.target))
                    // [lead49] the walk is judged once before the order goes out (runCommit), not again here
                    if (mob->IsAlive() && !mob->IsInCombat() && !tank->IsWithinLOSInMap(mob))
                    {
                        Position const point = mob->GetPosition();
                        if (Leader::Events::SetMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, point, 3.0f,
                                                    "engage path in") &&
                            lead.engageWalkFor != mob->GetGUID())
                        {
                            lead.engageWalkFor = mob->GetGUID();
                            logEvent(tank, "EngagePathIn",
                                     JsonObject().Str("who", "tank").Str("mob", mob->GetName())
                                         .Real("distance", tank->GetExactDist(mob)));
                        }
                    }
        }
        // [lead38] b87 rfc-h pull 1 (16-22 s): the hunter's pet on a Crab ("kill command -> Crab"). A member's pet
        // fights the leader's target only: a pet on a critter or on a mob outside the group's fight is sent to the
        // member's Attack target (the client's pet Attack button), else called back (Follow)
        if (fighting)
            for (Player* bot : seats)
            {
                Pet* pet = bot->GetPet();
                Unit* petVictim = pet && pet->IsAlive() ? pet->GetVictim() : nullptr;
                Creature* victimMob = petVictim ? petVictim->ToCreature() : nullptr;
                if (!victimMob || !pet->GetCharmInfo())
                    continue;
                bool const wrong = victimMob->IsCritter() || !LeaderFightMob(tank, victimMob) ||
                                   (!victimMob->IsInCombat() && victimMob->GetVictim() != pet);
                if (!wrong)
                    continue;
                Leader::MemberEvents petEvents;
                Unit* petTarget = Leader::Events::Get(bot->GetGUID(), petEvents) &&
                                          petEvents.attack == Leader::AttackEvent::Attack
                                      ? ObjectAccessor::GetUnit(*bot, petEvents.target)
                                      : nullptr;
                CharmInfo* charmInfo = pet->GetCharmInfo();
                pet->AttackStop();
                pet->InterruptNonMeleeSpells(false);
                if (petTarget && petTarget->IsAlive() && petTarget->IsInCombat())
                {
                    pet->ClearUnitState(UNIT_STATE_FOLLOW);
                    charmInfo->SetIsCommandAttack(true);
                    charmInfo->SetIsAtStay(false);
                    charmInfo->SetIsFollowing(false);
                    charmInfo->SetIsCommandFollow(false);
                    charmInfo->SetIsReturning(false);
                    pet->AI()->AttackStart(petTarget);
                }
                else
                {
                    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
                    charmInfo->SetCommandState(COMMAND_FOLLOW);
                    charmInfo->SetIsCommandAttack(false);
                    charmInfo->SetIsReturning(true);
                    charmInfo->SetIsCommandFollow(true);
                }
                Decisions().Write(run, "leader", "pet_redirect", victimMob->GetGUID(), victimMob->GetName(),
                                              true,
                                              JsonObject().Str("member", bot->GetName()).Str("pet", pet->GetName())
                                                  .Str("was", victimMob->GetName())
                                                  .Bool("critter", victimMob->IsCritter())
                                                  .Str("to", petTarget ? petTarget->GetName() : "follow"));
            }
        // [lead61] b106 pull 1 started "unplanned: Rthunaa" 35 ms before the tank: only the tank opens a pull - out
        // of a fight a member's pet on a mob is called back
        if (!fighting)
            for (Player* bot : seats)
                if (Pet* pet = bot->GetPet(); pet && pet->IsAlive() && pet->GetVictim() && pet->GetCharmInfo())
                {
                    pet->AttackStop();
                    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
                    pet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
                    pet->GetCharmInfo()->SetIsCommandAttack(false);
                    Decisions().Write(run, "leader", "pet_redirect", bot->GetGUID(), bot->GetName(), true,
                                                  JsonObject().Str("member", bot->GetName())
                                                      .Str("pet", pet->GetName()).Str("to", "follow")
                                                      .Str("why", "only the tank opens a pull"));
                }
        // [lead69] command_dupe_blocked: identical commands the arbiter refused (should be 0), logged when they happen
        {
            std::vector<Player*> commanded = seats;
            commanded.push_back(tank);
            for (Player* member : commanded)
                if (uint32 const dupes = Leader::Events::TakeDupesBlocked(member->GetGUID()))
                {
                    uint32& total = lead.dupesBlocked[member->GetName()];
                    if (!total)
                        Decisions().Write(
                            run, "leader", "command_dupe_blocked", member->GetGUID(), member->GetName(), false,
                            JsonObject().Str("member", member->GetName()).Num("count", dupes));
                    total += dupes;
                }
        }
        // [lead76] the fight moves away from a pack it cannot take (the leader's fight spot): one tank MoveTo
        if (!fighting)
        {
            lead.fightMoveTo.reset();
            lead.joinedPack.clear();
            lead.joinedMobs.clear();
        }
        else if (lead.fightMoveTo)
        {
            if (tank->GetExactDist(*lead.fightMoveTo) <= 2.0f)
                lead.fightMoveTo.reset();
            else if (checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, *lead.fightMoveTo, 2.0f,
                                 "hazard: the fight moves away from an unpulled pack"))
                LogFightDrag(run, tank, *lead.fightMoveTo, lead.meetPoint, "away from an unpulled pack");
        }
        // [lead81] b124 rfc-h 167.8-182 s: Rtshaab pulled a Trogg 150 yd from the tank (no sight), the tank stood and
        // he died alone - a member a fight mob is on, beyond the tank's pick-up reach (its taunt's range, in sight):
        // the fight spot moves to it - the tank walks to the member (its walk checked) while the member runs to the
        // tank (the aggro-loss run) and the others leave that mob alone (the aggro-loss rule)
        if (fighting && !lead.fightMoveTo)
        {
            float const pickUp = LeaderTauntReach(tank);
            float const arrive = pickUp > 0.0f ? pickUp : NOMINAL_MELEE_RANGE;
            Player* alone = nullptr;
            for (auto const& [victimGuid, mob] : looseOn)
                if (Player* victim = ObjectAccessor::FindPlayer(victimGuid);
                    victim && victim != tank && victim->IsAlive() && victim->GetMap() == map &&
                    !(victim->GetExactDist(tank) <= arrive && tank->IsWithinLOSInMap(victim)) &&
                    (!alone || victim->GetExactDist(tank) < alone->GetExactDist(tank)))
                    alone = victim;
            if (alone && checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, alone->GetPosition(), arrive,
                                     "aggro loss: the tank goes to the member"))
                Decisions().Write(run, "leader", "fight_spot_to_member", alone->GetGUID(),
                                              alone->GetName(), true,
                                              JsonObject().Str("member", alone->GetName())
                                                  .Real("distance", alone->GetExactDist(tank))
                                                  .Real("pickUp", pickUp)
                                                  .Bool("sight", tank->IsWithinLOSInMap(alone)));
        }
        // [lead29] member -> tank distance while the tank walks out of combat (run summary: max / median)
        if (!fighting && tank->isMoving())
            for (Player* bot : seats)
            {
                std::vector<float>& samples = lead.walkDistances[bot->GetName()];
                if (samples.size() < 4000)
                    samples.push_back(bot->GetExactDist(tank));
            }
        // [lead21] slabs-h 20261026-195203: the tank executing a leader PullOrder out of combat had no fresh command -
        // rot3's stale mode ("keeps to its current target, no area spell") vetoed its pull: 17 orders "nothing came".
        // While an order runs, its target is the tank's fresh Attack, an area pull allowed on that pack
        if (!fighting && Leader::LEADER_TANK_EXECUTES)
        {
            Leader::PullOrder order;
            if (Leader::Orders::GetOpen(group.LeaderGuid(), order) && !order.target.IsEmpty())
                if (Unit* pullMob = ObjectAccessor::GetUnit(*tank, order.target); pullMob && pullMob->IsAlive())
                {
                    bool const changed = Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack,
                                                                   order.target, {order.target});
                    Leader::Events::SetAoe(tank->GetGUID(), Leader::AoeRule::AllowedOnPack, pullMob->GetPosition(),
                                           LEADER_AOE_RADIUS);
                    if (changed)
                        logEvent(tank, "Attack", JsonObject().Str("target", pullMob->GetName()).Str("who", "tank")
                                                     .Str("why", "the leader's pull order runs"));
                }
        }
        // [lead16] LEADER_BOSS_PREPARE: the tank's Prepare until the pull is on (and a moment after the commit)
        if (fighting)
            lead.bossPrepareFor.Clear();
        else if (!lead.bossPrepareFor.IsEmpty() &&
                 getMSTimeDiff(lead.bossPrepareSinceMs, now) < Leader::LEADER_BOSS_PREPARE_MS + 3 * IN_MILLISECONDS)
        {
            if (Leader::Events::SetReady(tank->GetGUID(), Leader::ReadyEvent::Prepare))
                logEvent(tank, "Prepare", JsonObject().Str("why", "Army of the Dead before the boss pull"));
        }

        // [lead16] the hazard scan (slow layer, every LEADER_HAZARD_SCAN_MS); membership is checked every tick
        if (Leader::LEADER_HAZARD_SPOTS &&
            (!lead.hazardScanMs || getMSTimeDiff(lead.hazardScanMs, now) >= LEADER_HAZARD_SCAN_MS))
        {
            lead.hazardScanMs = now;
            lead.hazardZones.clear();
            for (Player* bot : seats)
                slow([&]
                     {
                         LeaderCollectHazards(bot, lead.hazardZones[bot->GetGUID()]);
                         return true;
                     });
            if (tank->IsAlive())  // [lead17] the tank's hazards too
                slow([&]
                     {
                         LeaderCollectHazards(tank, lead.hazardZones[tank->GetGUID()]);
                         return true;
                     });
        }
        if (!fighting)
        {
            lead.combatSpots.clear();
            lead.combatSpotAnchor.clear();
            lead.fightAnchorSerial = 0;
            lead.healReachOut.clear();  // [lead120] the next fight's heal reach is decided anew
            lead.healReachAnchor = 0;
        }
        else if (!lead.fightAnchorSerial || tank->GetExactDist(lead.fightAnchor) > Leader::LEADER_ANCHOR_MOVE_YD)
        {
            // [lead18] the fight spot: where the tank holds the mobs; a new anchor re-spots every member once
            lead.fightAnchor = tank->GetPosition();
            ++lead.fightAnchorSerial;
        }

        // [lead17] LEADER_ONE_GATHER_OWNER: who the group waits for - a member without a leader spot farther than
        // LEADER_TRAIL_YD from the tank (back under LEADER_TRAIL_BACK_YD); the harness's far gate reads this set
        if (Leader::LEADER_ONE_GATHER_OWNER)
            for (Player* bot : seats)
            {
                ObjectGuid const guid = bot->GetGUID();
                // [lead24d] a member whose leader walk was just rejected (it would wake a mob) stays: not trailing
                auto const rejected = lead.spotRejectLogMs.find(guid);
                bool const walkRejected = rejected != lead.spotRejectLogMs.end() && rejected->second &&
                                          getMSTimeDiff(rejected->second, now) < 5 * IN_MILLISECONDS;
                // [lead119] b153 rfc-h 90.6-158 s: the four members the pull walk held (Hold "the tank takes the mob
                // first") trailed at 36 yd as the tank walked in, the far gate held the tank for them: a deadlock. A
                // member the pull walk holds stands where the leader put it - placed, not trailing
                bool const placed = lead.spots.count(guid) || lead.hazardSpots.count(guid) ||
                                    lead.combatSpots.count(guid) || lead.pullWalkSpots.count(guid) || walkRejected;
                float const gap = bot->GetExactDist(tank);
                bool const was = lead.trailing.count(guid) > 0;
                bool const trails = !fighting && !placed &&
                                    (was ? gap > Leader::LEADER_TRAIL_BACK_YD : gap > Leader::LEADER_TRAIL_YD);
                if (trails != was)
                {
                    if (trails)
                        lead.trailing.insert(guid);
                    else
                        lead.trailing.erase(guid);
                    Decisions().Write(run, "leader", "leader_trail", guid, bot->GetName(), true,
                                                  JsonObject().Str("member", bot->GetName())
                                                      .Str("state", trails ? "waits for it" : "with the group")
                                                      .Real("gap", gap));
                }
            }

        // [lead45] the tank rests too (it is no seat): the boss pull waits for its mana, so the leader tells it to
        // drink (its own rest line / the boss top-up line, RestState) while no order runs
        if (!fighting)
        {
            lead.healStarted = false;  // [lead61] the next fight's heals start on the leader's word again
            lead.burnLogged.clear();   // [lead80]
        }
        // [lead67] the tank's threat lead at 2 s and 5 s into each fight: per fight mob the tank's threat over the
        // top other's (threat_lead) - how well the tank picks the pack up
        // [lead67] user metric: threat per second per member over the pull (the highest threat it held on each mob,
        // sampled at 2 s, 5 s and every 5 s after - not every tick), the tank's per mob, and the margin (tank over the
        // top non-tank) min / median at 2 s, 5 s and the last sample: threat_tps at the fight's end
        auto const margins = [](std::vector<float> values)
        {
            std::sort(values.begin(), values.end());
            return values.empty() ? std::make_pair(-1.0f, -1.0f)
                                  : std::make_pair(values.front(), values[values.size() / 2]);
        };
        if (!fighting && lead.fightStartMs)
        {
            float const sec = std::max(getMSTimeDiff(lead.fightStartMs, now) / 1000.0f, 1.0f);
            std::map<std::string, float> perMember;
            JsonArray tankMobs;
            for (auto const& [mob, byMember] : lead.threatMax)
                for (auto const& [who, threat] : byMember)
                {
                    perMember[who] += threat;
                    if (who == tank->GetName())
                        tankMobs.Raw(JsonObject().Num("low", mob.GetCounter()).Real("tps", threat / sec).Build());
                }
            JsonObject members;
            for (auto const& [who, threat] : perMember)
                members.Real(who.c_str(), threat / sec);
            JsonObject marginsJson;
            for (auto const& [mark, values] : lead.threatMargins)
            {
                auto const [low, median] = margins(values);
                std::string const key = mark == 99000 ? std::string("end") : std::to_string(mark / 1000) + "s";
                marginsJson.Raw(key.c_str(), JsonObject().Real("min", low).Real("median", median).Build());
            }
            Decisions().Write(run, "leader", "threat_tps", tank->GetGUID(), tank->GetName(), false,
                                          JsonObject().Real("fightSec", sec).Raw("tps", members.Build())
                                              .Raw("tankPerMob", tankMobs.Build()).Raw("margin", marginsJson.Build()));
            lead.threatMax.clear();
            lead.threatMargins.clear();
        }
        if (!fighting)
        {
            lead.fightStartMs = 0;
            lead.threatLeadLogged = 0;
        }
        else
        {
            if (!lead.fightStartMs)
                lead.fightStartMs = now;
            uint32 const into = getMSTimeDiff(lead.fightStartMs, now);
            uint32 const nextMark = lead.threatLeadLogged < 2000   ? 2000
                                    : lead.threatLeadLogged < 5000 ? 5000
                                                                   : lead.threatLeadLogged + 5000;
            for (uint32 const mark : {nextMark})
                if (into >= mark && lead.threatLeadLogged < mark)
                {
                    lead.threatLeadLogged = mark;
                    std::vector<float>& marginValues = lead.threatMargins[mark <= 5000 ? mark : 99000];
                    if (mark > 5000)
                        marginValues.clear();  // the last sample stands for the end
                    JsonArray mobs;
                    std::set<ObjectGuid> seenMobs;
                    std::vector<Player*> sides = seats;
                    sides.push_back(tank);
                    for (Player* side : sides)
                        for (Unit* attacker : side->getAttackers())
                        {
                            if (!attacker || !attacker->IsAlive() || !seenMobs.insert(attacker->GetGUID()).second)
                                continue;
                            float const tankThreat = attacker->GetThreatMgr().GetThreat(tank);
                            float top = 0.0f;
                            std::string topName = "-";
                            auto& held = lead.threatMax[attacker->GetGUID()];
                            held[tank->GetName()] = std::max(held[tank->GetName()], tankThreat);
                            for (Player* other : seats)
                            {
                                float const threat = attacker->GetThreatMgr().GetThreat(other);
                                held[other->GetName()] = std::max(held[other->GetName()], threat);
                                if (threat > top)
                                {
                                    top = threat;
                                    topName = other->GetName();
                                }
                            }
                            if (top > 0.0f)
                                marginValues.push_back(tankThreat / top);
                            mobs.Raw(JsonObject().Str("mob", attacker->GetName())
                                         .Num("low", attacker->GetGUID().GetCounter())
                                         .Real("tank", tankThreat).Real("top", top).Str("topWho", topName)
                                         .Real("lead", top > 0.0f ? tankThreat / top : -1.0f)
                                         .Bool("onTank", attacker->GetVictim() == tank)
                                         .Build());
                        }
                    if (mark <= 5000)
                        Decisions().Write(run, "leader", "threat_lead", tank->GetGUID(), tank->GetName(),
                                                      false,
                                                      JsonObject().Num("sec", mark / 1000).Raw("mobs", mobs.Build()));
                }
        }
        // [lead65] the move phase: the fight state and the pull order - a change starts a new phase
        {
            Leader::PullOrder phaseOrder;
            uint32 const orderId = Leader::Orders::GetOpen(group.LeaderGuid(), phaseOrder) ? phaseOrder.id : 0;
            // [lead74] ... and the fight target (a held move's purpose ends when its target changes or dies)
            std::string const key = std::string(fighting ? "fight:" : "out:") + std::to_string(orderId) + ":" +
                                    std::to_string(lead.commitMob.GetCounter()) + ":" +
                                    std::to_string(target ? target->GetGUID().GetCounter() : 0);
            if (key != lead.movePhaseKey)
            {
                lead.movePhaseKey = key;
                ++lead.movePhase;
            }
        }

        if (fighting)
        {
            lead.bossWaitFor.Clear();  // [lead46] the wait ends with a fight
            lead.plannerRestFor.clear();
        }
        // [lead51] the members' measured mana spend in fights (% of max mana per second of fight): the model's rate
        {
            float const dt = fighting && lead.manaTickMs ? getMSTimeDiff(lead.manaTickMs, now) / 1000.0f : 0.0f;
            lead.manaTickMs = fighting ? now : 0;
            std::vector<Player*> spenders = seats;
            spenders.push_back(tank);
            for (Player* member : spenders)
            {
                if (!member->GetMaxPower(POWER_MANA))
                    continue;
                auto& spend = lead.manaSpend[member->GetGUID()];
                float const pct = member->GetPowerPct(POWER_MANA);
                // [lead69] the NET use (mana at the fight's start minus at its end, per fight second): b114 asked the
                // paladin tank for 100 % before Taragaman from its gross spend (it regains much of it in the fight)
                if (!fighting)
                {
                    if (spend.prev >= 0.0f)  // the fight just ended: its net use counts
                    {
                        spend.spent += std::max(0.0f, spend.prev - pct);
                        spend.sec += spend.fightSec;
                        if (spend.sec > 0.0f)
                            RestState::NoteManaRate(member->GetGUID(), spend.spent / spend.sec);
                    }
                    spend.prev = -1.0f;
                    spend.fightSec = 0.0f;
                    continue;
                }
                if (spend.prev < 0.0f)
                    spend.prev = pct;  // the fight's start
                spend.fightSec += dt;
            }
        }
        if (!fighting && !tank->IsInCombat() && !Leader::Orders::IsOpen(group.LeaderGuid()))
        {
            std::string tankRestWhy;
            Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor);
            std::string const tankGap =
                waitBoss && waitBoss->IsAlive() ? PullReadiness::MemberBossGap(tank, waitBoss) : std::string();
            if (!tankGap.empty())
                tankRestWhy = "the boss pull waits: " + tankGap;
            bool const plannerWaits = !lead.plannerRestFor.empty() && tank->GetName() == lead.plannerRestFor &&
                                      !MemberFull(tank);  // [lead50]
            if (plannerWaits)
                tankRestWhy = "the planner waits for it";
            bool const tankRests = plannerWaits ? (tank->IsSitState() || RestState::CanRest(tank))
                                                : (waitBoss && waitBoss->IsAlive()
                                        ? !tankGap.empty()
                                        : (RestState::IsDrinking(tank) || RestState::IsEating(tank) ||
                                           RestState::WantsDrink(tank, &tankRestWhy) ||
                                           RestState::WantsFood(tank, &tankRestWhy)) &&
                                              !MemberFull(tank)) &&
                                                      (tank->IsSitState() || RestState::CanRest(tank));
            if (tankRests && Leader::Events::SetReady(tank->GetGUID(), Leader::ReadyEvent::Rest))
                logEvent(tank, "Rest", JsonObject().Str("who", "tank")
                                           .Str("why", tankRestWhy.empty() ? "sits: to full" : tankRestWhy));
        }

        // [lead76] the leader's "top up X": the healer heals the member whose health holds the pull (out of a fight)
        if (!fighting && healer)
        {
            ObjectGuid topUp = lead.topUpFor;
            // [lead116] a hurt member with the healer above its own mana line is topped up by the healer (one
            // command per change), not fed: the most hurt one
            if (topUp.IsEmpty() && !RestState::WantsDrink(healer) && !healer->IsSitState())
            {
                std::vector<Player*> all = seats;
                all.push_back(tank);
                float lowest = 100.0f;
                for (Player* member : all)
                    if (member->IsAlive() && member->GetHealthPct() < lowest)
                    {
                        lowest = member->GetHealthPct();
                        topUp = member->GetGUID();
                    }
            }
            if (topUp.IsEmpty())
                if (Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor))
                {
                    std::vector<Player*> all = seats;
                    all.push_back(tank);
                    for (Player* member : all)
                        if (PullReadiness::MemberBossGap(member, waitBoss).rfind("health", 0) == 0)
                        {
                            topUp = member->GetGUID();
                            break;
                        }
                }
            Player* healed = topUp.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(topUp);
            if (healed && healed->IsAlive() && healed->GetHealthPct() < 100.0f &&
                Leader::Events::SetHealTarget(healer->GetGUID(), topUp))
                logEvent(healer, "TopUp", JsonObject().Str("member", healed->GetName())
                                              .Real("health", healed->GetHealthPct())
                                              .Str("why", "a readiness gap on health"));
        }

        uint32 seat = 0;
        for (Player* bot : seats)
        {
            ObjectGuid const guid = bot->GetGUID();
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            bool const combat = fighting || bot->IsInCombat();
            uint32 const index = seat++;
            // [lead17] the harness's (the leader's) own walk for this member (a link, out of the water, an event
            // gather): it runs, the leader issues no other move meanwhile
            LeaderGroup::WalkPoint harnessWalk;
            bool const walking = group.GetWaypoint(guid, harnessWalk);

            bool restCommanded = false;  // [lead47] the Rest command this tick: no move replaces it
            // 1. readiness, out of combat: Rest to whoever needs it (to full), Ready? while the pull waits for the
            // spots, Prepare (buffs, forms, a resurrection) while the group stands
            if (!combat)
            {
                std::string restWhy;
                // [lead59] user: no "sits: to full" - the Rest holds while the member is below its own computed line
                // for the next fight (RestState::BelowOwnLine, FightManaLine) and stands it up at that line
                bool needsRest = RestState::BelowOwnLine(bot, &restWhy);
                bool const full = MemberFull(bot);
                needsRest &= !full;  // [lead22] never Rest a full member
                lead.restingToFull.erase(guid);
                // [lead46] b95 rfc-h 348-423 s: the boss pull waited for "Rtshaab at 66 % mana" (BossReadyGap, 90 %
                // for a real boss) while the Rest rule read Oggleflint as a mini-boss (70 %) and gave Rtshaab no Rest.
                // While the boss pull waits there is ONE readiness: the member rests exactly while it is below the
                // boss ready lines (PullReadiness::MemberBossGap) and stands when it reaches them - the pull goes as
                // the last one is ready
                if (Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor);
                    waitBoss && waitBoss->IsAlive())
                {
                    std::string const gap = PullReadiness::MemberBossGap(bot, waitBoss);
                    needsRest = !gap.empty();
                    restWhy = gap.empty() ? restWhy : "the boss pull waits: " + gap;
                    lead.restingToFull.erase(guid);
                }
                // [lead50] the planner's held answer waits for this member: it rests
                if (!lead.plannerRestFor.empty() && bot->GetName() == lead.plannerRestFor && !MemberFull(bot))
                {
                    needsRest = true;
                    restWhy = "the planner waits for it";
                }
                {
                    // [lead76] the Rest's line vs the member's mana, logged at the Rest's start and end
                    // [lead79] logged at the Rest's start and end only (the line's text changes with the mana)
                    std::string const state = needsRest ? "rest: " + restWhy : std::string();
                    std::string& logged = lead.restLogged[guid];
                    if (needsRest != !logged.empty())
                    {
                        Decisions().Write(
                            run, "leader", "rest_line", guid, bot->GetName(), false,
                            JsonObject().Str("member", bot->GetName()).Str("state", needsRest ? "start" : "end")
                                .Str("line", needsRest ? restWhy : logged)
                                .Real("mana", bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f)
                                .Real("health", bot->GetHealthPct()));
                        logged = state;
                    }
                }
                if (needsRest && (bot->IsSitState() || RestState::CanRest(bot)))
                {
                    restCommanded = true;
                    if (Leader::Events::SetReady(guid, Leader::ReadyEvent::Rest))
                        logEvent(bot, "Rest", JsonObject().Str("why", restWhy.empty() ? "sits: to full" : restWhy));
                }
                else if (lead.spotsWaiting)
                {
                    if (Leader::Events::SetReady(guid, Leader::ReadyEvent::ReadyCheck))
                        logEvent(bot, "Ready?", JsonObject().Str("why", "the pull waits for the spots"));
                }
                else if (group.NeedBuff() || rebuff || anyDead || anySitting || tankStill)
                {
                    char const* why = group.NeedBuff() || rebuff ? "the buff pass"
                                      : anyDead                ? "a member is dead"
                                      : anySitting             ? "a member rests: the group waits"
                                                               : "the tank stands";
                    if (Leader::Events::SetReady(guid, Leader::ReadyEvent::Prepare))
                        logEvent(bot, "Prepare", JsonObject().Str("why", why));
                }
            }

            // 2. the moves: out of a cleave (keeping the heal reach), the pull's spot, along with the tank
            bool moved = walking;
            if (Leader::LEADER_OWNS_DODGE && combat &&
                DungeonTactics::InFrontalCleave(bot, botAI, 1.0f, 20.0f * float(M_PI) / 180.0f))
            {
                hazardSeen = true;
                Position exit;
                Leader::MemberEvents current;
                bool const kept = Leader::Events::Get(guid, current) && current.move == Leader::MoveEvent::MoveTo &&
                                  current.moveWhy == "out of a cleave cone" &&
                                  !DungeonTactics::SpotInFrontalCleave(bot, botAI, current.spot.GetPositionX(),
                                                                       current.spot.GetPositionY(), 1.0f,
                                                                       20.0f * float(M_PI) / 180.0f);
                if (kept)
                    exit = current.spot;
                if (kept || slow([&]
                                 {
                                     return LeaderCleaveExit(
                                         bot, botAI, exit,
                                         bot == healer ? std::function<bool(Position const&)>(healerKeeps)
                                                       : std::function<bool(Position const&)>(keepsHealer));
                                 }))
                {
                    if (checkedMove(guid, Leader::MoveEvent::MoveTo, exit, 1.5f, "out of a cleave cone"))
                        logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                 JsonObject().Str("why", "out of a cleave cone").Real("x", exit.GetPositionX())
                                     .Real("y", exit.GetPositionY()).Real("z", exit.GetPositionZ()));
                    if (bot == healer)
                        healerAt = exit;
                    moved = true;
                }
                else if (Leader::LEADER_HEAL_REACH && healer)
                    LogHealReach(group, bot, bot == healer ? tank : healer, "no cleave exit keeps the heal reach",
                                 "none");
            }
            // [lead16] LEADER_HAZARD_SPOTS: out of every ground hazard the leader sees for this member (to the nearest
            // safe spot that keeps the heal reach), and held there while the hazard stays next to it
            if (!moved && Leader::LEADER_HAZARD_SPOTS)
            {
                std::vector<HazardZone> const& zones = lead.hazardZones[guid];
                HazardZone const* in =
                    zones.empty() ? nullptr
                                  : LeaderZoneAt(zones, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                                 0.5f);
                auto const held = lead.hazardSpots.find(guid);
                bool const keepHeld =
                    !in && held != lead.hazardSpots.end() && !zones.empty() &&
                    LeaderZoneAt(zones, held->second.GetPositionX(), held->second.GetPositionY(),
                                 held->second.GetPositionZ(), LEADER_HAZARD_EDGE + 6.0f) &&
                    LeaderSpotSafe(bot, botAI, zones, held->second.GetPositionX(), held->second.GetPositionY(),
                                   held->second.GetPositionZ());
                if (in || keepHeld)
                {
                    hazardSeen = true;
                    Position spot;
                    bool const found =
                        keepHeld ? (spot = held->second, true)
                                 : slow([&]
                                        {
                                            return LeaderSafeSpot(
                                                bot, botAI, zones,
                                                bot == healer ? std::function<bool(Position const&)>(healerKeeps)
                                                              : std::function<bool(Position const&)>(keepsHealer),
                                                spot);
                                        });
                    if (found)
                    {
                        lead.hazardSpots[guid] = spot;
                        std::string const why = std::string("hazard: ") + (in ? in->what : "held out of it");
                        if (checkedMove(guid, Leader::MoveEvent::MoveTo, spot, 1.0f, why))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                     JsonObject().Str("why", why).Real("x", spot.GetPositionX())
                                         .Real("y", spot.GetPositionY()).Real("z", spot.GetPositionZ()));
                        if (bot == healer)
                            healerAt = spot;
                        moved = true;
                    }
                    else if (in && Leader::LEADER_HEAL_REACH && healer)
                        LogHealReach(group, bot, bot == healer ? tank : healer,
                                     "no safe spot out of " + in->what + " keeps the heal reach", "none");
                }
                else
                    lead.hazardSpots.erase(guid);
            }

            // [lead59] b106: Rtdruaa combat spot -> "aggro loss: to the tank" -> combat spot (command_repeat) - two
            // rules owned its position. The run, once issued, holds until the member arrived (the mob may switch
            // back meanwhile); the combat spot does not overwrite it
            if (!moved && combat && lead.runHold.count(guid))
            {
                Leader::MemberEvents held;
                if (Leader::Events::Get(guid, held) && held.move == Leader::MoveEvent::MoveTo &&
                    held.moveWhy == "aggro loss: to the tank" &&
                    bot->GetExactDist2d(held.spot.GetPositionX(), held.spot.GetPositionY()) > held.tolerance)
                {
                    Leader::Events::SetMove(guid, held.move, held.spot, held.tolerance, held.moveWhy);
                    moved = true;
                }
                else
                    lead.runHold.erase(guid);
            }
            if (!combat)
                lead.runHold.erase(guid);
            // the aggro-loss victim runs next to the tank (between it and the tank, in heal reach)
            // [lead67] only a victim beyond the tank's pick-up reach (its taunt's range, in sight) runs: one standing in
            // it is picked up where it stands (b112: the shaman behind the target flipped run <-> combat spot)
            auto const loose = looseOn.find(guid);
            float const pickUp = LeaderTauntReach(tank);
            bool const inPickUp = pickUp > 0.0f && bot->GetExactDist(tank) <= pickUp && tank->IsWithinLOSInMap(bot);
            if (!moved && loose != looseOn.end() && !inPickUp)
            {
                float const dx = bot->GetPositionX() - tank->GetPositionX();
                float const dy = bot->GetPositionY() - tank->GetPositionY();
                float const length = std::max(0.1f, std::hypot(dx, dy));
                float x = tank->GetPositionX() + dx / length * LEADER_LOOSE_TANK_YD;
                float y = tank->GetPositionY() + dy / length * LEADER_LOOSE_TANK_YD;
                float z = tank->GetPositionZ();
                Position spot = tank->GetPosition();
                if (map->CheckCollisionAndGetValidCoords(tank, tank->GetPositionX(), tank->GetPositionY(),
                                                         tank->GetPositionZ(), x, y, z) &&
                    (bot == healer ? healerKeeps(Position(x, y, z)) : keepsHealer(Position(x, y, z))))
                    spot.Relocate(x, y, z);
                // [lead55] b100: Rtdruaa 2 304 "aggro loss: to the tank" moves - the run's point is kept while it
                // still lies by the tank (within LEADER_LOOSE_TANK_YD and the walk's tolerance)
                Leader::MemberEvents runEvents;
                if (Leader::Events::Get(guid, runEvents) && runEvents.move == Leader::MoveEvent::MoveTo &&
                    runEvents.moveWhy == "aggro loss: to the tank" &&
                    runEvents.spot.GetExactDist(tank->GetPosition()) <= LEADER_LOOSE_TANK_YD + 1.5f)
                    spot = runEvents.spot;
                lead.runHold.insert(guid);  // [lead59]
                bool runs = checkedMove(guid, Leader::MoveEvent::MoveTo, spot, 1.5f, "aggro loss: to the tank");
                // [lead112] b147: the spot by the tank refused by the healer's own move guards, it stayed and died -
                // the tank's own spot is the second choice (the same checks before it is issued)
                if (!runs && spot.GetExactDist(tank->GetPosition()) > 0.5f)
                {
                    spot = tank->GetPosition();
                    runs = checkedMove(guid, Leader::MoveEvent::MoveTo, spot, 1.5f, "aggro loss: to the tank");
                }
                if (runs)
                    logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", "aggro loss: to the tank").Str("mob", loose->second->GetName())
                                 .Real("gap", bot->GetExactDist(tank)));
                if (bot == healer)
                    healerAt = spot;
                moved = true;
            }
            // [lead16] LEADER_COMBAT_SPOTS: the member's combat spot (StandAt), kept while it fits, a new one when the
            // target moved off it, it lost sight, a cleave or a hazard covers it or it broke the heal reach
            if (!moved && combat && Leader::LEADER_COMBAT_SPOTS && target && target->IsAlive())
            {
                bool const melee = !PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsHeal(bot);
                std::vector<HazardZone> const& zones = lead.hazardZones[guid];
                std::function<bool(Position const&)> const keeps =
                    bot == healer ? std::function<bool(Position const&)>(healerKeeps)
                                  : std::function<bool(Position const&)>(keepsHealer);
                auto found = lead.combatSpots.find(guid);
                uint32& losMs = lead.combatSpotLosMs[guid];
                bool const sight = !losMs || getMSTimeDiff(losMs, now) >= LEADER_SPOT_LOS_MS;
                if (sight)
                    losMs = now;
                if (Leader::LEADER_ANCHORED_SPOTS)
                {
                    // [lead18] anchored: kept for the whole fight while it fits; a target change alone never moves
                    // anyone; a new spot is the nearest that fits (from where the member stands), else a fresh one
                    std::string breaks;
                    if (found == lead.combatSpots.end())
                        breaks = "first spot";
                    // [lead38] b86: 135 "the fight spot moved" re-spots - a new anchor re-spots a member only when its
                    // held spot no longer fits; a ranged / healer spot is not re-judged while the target runs in and
                    // the tank does not hold it yet (b87 pull 1: spots chased the incoming Earthborer)
                    // [lead64] melee too: no spot chases a mob still running to the tank (b109 30.6-31.5 s)
                    else if (target->isMoving() &&
                             (target->GetVictim() != tank || !tank->IsWithinMeleeRange(target)))
                        breaks.clear();
                    else
                    {
                        breaks = LeaderCombatSpotBreaks(bot, botAI, target, found->second.second, melee, zones,
                                                        keeps, sight);
                        if (lead.combatSpotAnchor[guid] != lead.fightAnchorSerial)
                        {
                            if (breaks.empty())
                                lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;
                            else
                                breaks = "the fight spot moved: " + breaks;
                        }
                    }
                    // [lead38] no first spot for a ranged / healer while the target runs in: it stays (and casts)
                    if (breaks == "first spot" && target->isMoving() &&
                        (target->GetVictim() != tank || !tank->IsWithinMeleeRange(target)))
                    {
                        breaks.clear();
                        moved = true;
                        // [lead64] user: «одной командой» - melee go to the meeting point once (the leader's
                        // predicted meeting of the tank and the mob) and adjust once the mob stands on the tank
                        if (melee && !bot->IsWithinMeleeRange(target) &&
                            moveOrStay(bot, meleeMeetAt(bot, target->GetGUID()), 3.0f,
                                       "along with the tank: to the meeting point"))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                     JsonObject().Str("why", "along with the tank: to the meeting point"));
                    }
                    if (!breaks.empty())
                    {
                        Position spot;
                        bool const got =
                            slow([&]
                                 {
                                     SpotAcquire const acquire;  // [lead36]
                                     return LeaderNearestCombatSpot(bot, botAI, target, bot->GetPosition(), melee,
                                                                    zones, keeps, spot) ||
                                            LeaderCombatSpot(bot, botAI, target, melee, zones, keeps, spot);
                                 });
                        if (got)
                        {
                            if (breaks != "first spot")
                                Decisions().Write(
                                    run, "leader", "combat_respot", guid, bot->GetName(), true,
                                    JsonObject().Str("member", bot->GetName()).Str("why", breaks)
                                        .Str("target", target->GetName())
                                        .Real("walk", bot->GetExactDist(spot)));
                            lead.combatSpots[guid] = {target->GetGUID(), spot};
                            lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;
                        }
                        else if (found != lead.combatSpots.end() && breaks.rfind("the fight spot moved", 0) == 0)
                            lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;  // keep it, nothing better
                        else
                            lead.combatSpots.erase(guid);
                        found = lead.combatSpots.find(guid);
                    }
                }
                else
                {
                    bool const valid = found != lead.combatSpots.end() && found->second.first == target->GetGUID() &&
                                       LeaderCombatSpotValid(bot, botAI, target, found->second.second, melee, zones,
                                                             keeps, sight);
                    if (!valid)
                    {
                        Position spot;
                        if (slow([&] { return LeaderCombatSpot(bot, botAI, target, melee, zones, keeps, spot); }))
                            lead.combatSpots[guid] = {target->GetGUID(), spot};
                        else
                            lead.combatSpots.erase(guid);
                        found = lead.combatSpots.find(guid);
                    }
                }
                // [lead20] user: «когда танк захватил уже всех с этого радиуса, хил может зайти» - a walk to the
                // combat spot only once the tank holds every fight mob near the spot and the walk (else it stays)
                if (found != lead.combatSpots.end() &&
                    bot->GetExactDist2d(found->second.second.GetPositionX(), found->second.second.GetPositionY()) >
                        (melee ? 1.0f : 2.0f))
                {
                    // [lead25] user: «танк пулит, моб бежит к нему… через секунду все ДД начинают дамажить»: melee
                    // walk to the meeting at the tag (the pulled mob already comes); ranged / healer wait for the hold
                    if (!melee && !tankHolds(bot->GetPosition(), found->second.second))
                    {
                        if (!lead.holdWaitMs.count(guid))
                            lead.holdWaitMs[guid] = now;
                        found = lead.combatSpots.end();
                        moved = true;  // it stays where it is meanwhile
                    }
                    else if (auto const waited = lead.holdWaitMs.find(guid); waited != lead.holdWaitMs.end())
                    {
                        Decisions().Write(run, "leader", "respot_after_tank_hold", guid, bot->GetName(),
                                                      true,
                                                      JsonObject().Str("member", bot->GetName())
                                                          .Real("waitSec", getMSTimeDiff(waited->second, now) /
                                                                               1000.0f));
                        lead.holdWaitMs.erase(waited);
                    }
                }
                if (found != lead.combatSpots.end())
                {
                    Position const& spot = found->second.second;
                    if (checkedMove(guid, Leader::MoveEvent::StandAt, spot, melee ? 1.0f : 2.0f,
                                                melee ? "combat spot: behind the target" : "combat spot"))
                        logEvent(bot, MoveEventName(Leader::MoveEvent::StandAt),
                                 JsonObject().Str("why", melee ? "combat spot: behind the target" : "combat spot")
                                     .Str("target", target->GetName())
                                     .Real("distance", target->GetExactDist(spot))
                                     .Real("x", spot.GetPositionX()).Real("y", spot.GetPositionY())
                                     .Real("z", spot.GetPositionZ()));
                    if (bot == healer)
                        healerAt = spot;
                    moved = true;
                }
            }

            if (!moved && spotsHeld)
                if (auto const spot = lead.spots.find(guid); spot != lead.spots.end())
                {
                    if (checkedMove(guid, Leader::MoveEvent::StandAt, spot->second,
                                                Leader::LEADER_SPOT_TOLERANCE, "its spot for the pull"))
                        logEvent(bot, MoveEventName(Leader::MoveEvent::StandAt),
                                 JsonObject().Str("why", "its spot for the pull")
                                     .Real("x", spot->second.GetPositionX()).Real("y", spot->second.GetPositionY())
                                     .Real("z", spot->second.GetPositionZ()));
                    moved = true;
                }
            // the member's own follow is off (LEADER_NO_OWN_FOLLOW): the leader walks it along with the tank; a
            // member the harness holds ("stay", a regroup) or one that sits is not moved
            // [lead16] LEADER_HOLD_EVENTS: the group is held (a patrol wait, the harness's "stay"): Hold, no walk
            if (!moved && !combat && Leader::LEADER_HOLD_EVENTS && Leader::ExecutorMember(bot) &&
                (!lead.waitPatrol.IsEmpty() ||  // [lead33] no Hold <-> Follow flip on the step-back walk
                 botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT)))
            {
                if (checkedMove(guid, Leader::MoveEvent::Hold, bot->GetPosition(), 0.0f, "the group holds"))
                    logEvent(bot, "Hold", JsonObject().Str("why", lead.waitPatrol.IsEmpty() ? "stay (regroup)"
                                                                                          : "patrol wait"));
                moved = true;
            }
            // [lead115] user: on a pull the healer and the damage dealers do not walk with the tank. b149 rfc-h
            // 14.1-17.4 s: "Follow - along with the tank, gap 1.3" on the engage walk, Earthborer 3 took the shaman
            // 3 yd from the tank. While the pull walks (the engage order open, the tank without threat on its mob)
            // every member holds where it stands, outside the reach of the pull's mobs and of every idle mob, else
            // steps back from the pull's mob out of its reach; it comes in on the leader's commands once the tank
            // has the mob
            if (!moved && !combat && pullWalk && Leader::ExecutorMember(bot) && !bot->IsSitState())
            {
                // [lead117] b151 rfc-h 72.9-74.1 s: the shaman at a mob's reach edge flipped "out of its reach"
                // MoveTo <-> Hold every 0.5 s - one decision per order and per fact: the spot decided for this order
                // stands while its mob stays where it was judged (LEADER_COMMIT_MOVED_YD); a new order or the mob
                // moved off decides again
                auto memo = lead.pullWalkSpots.find(guid);
                if (memo != lead.pullWalkSpots.end())
                {
                    Creature* judged = memo->second.mob.IsEmpty() ? nullptr : map->GetCreature(memo->second.mob);
                    if (memo->second.orderId != pullWalkOrder ||
                        (judged && judged->GetExactDist(memo->second.mobAt) > Leader::LEADER_COMMIT_MOVED_YD))
                    {
                        lead.pullWalkSpots.erase(memo);
                        memo = lead.pullWalkSpots.end();
                    }
                }
                if (memo != lead.pullWalkSpots.end())
                {
                    if (memo->second.hold)
                        checkedMove(guid, Leader::MoveEvent::Hold, memo->second.spot, 0.0f, "the pull walks");
                    else
                        checkedMove(guid, Leader::MoveEvent::MoveTo, memo->second.spot, LEADER_FOLLOW_TOLERANCE,
                                    "the pull walks");
                    moved = true;
                }
            }
            if (!moved && !combat && pullWalk && Leader::ExecutorMember(bot) && !bot->IsSitState())
            {
                Position holdAt = bot->GetPosition();
                Unit* reaching = nullptr;
                for (ObjectGuid const& mobGuid : pullMobs)
                    if (Creature* pullMob = map->GetCreature(mobGuid);
                        pullMob && pullMob->IsAlive() && IdleMobReachesSpot(bot, pullMob, holdAt))
                        reaching = pullMob;
                if (!reaching)
                    reaching = LeaderAggroAt(bot, holdAt);
                if (reaching)
                {
                    float const dx = bot->GetPositionX() - reaching->GetPositionX();
                    float const dy = bot->GetPositionY() - reaching->GetPositionY();
                    float const length = std::max(0.1f, std::hypot(dx, dy));
                    // [lead117] out of the reach as the mob's facts make it: its aggro range for this member and
                    // its combat distance, plus its wander radius (a wanderer), plus the follow spread
                    Creature* reachingMob = reaching->ToCreature();
                    float const away =
                        reachingMob ? reachingMob->GetAggroRange(bot) + reachingMob->m_CombatDistance +
                                          (reachingMob->GetDefaultMovementType() == RANDOM_MOTION_TYPE
                                               ? reachingMob->GetWanderDistance()
                                               : 0.0f) +
                                          LEADER_FOLLOW_START_YD
                                    : LEADER_FOLLOW_START_YD;
                    float x = reaching->GetPositionX() + dx / length * away;
                    float y = reaching->GetPositionY() + dy / length * away;
                    float z = bot->GetPositionZ();
                    bool found = map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                      bot->GetPositionZ(), x, y, z);
                    // [lead118] b152: the straight step back put members far behind and they trailed the group 44 s
                    // - the nearest spot around the member out of every pull mob's and idle mob's reach (rings of
                    // LEADER_LOOSE_TANK_YD), the straight step back the last
                    auto const outOfReach = [&](Position const& at)
                    {
                        for (ObjectGuid const& mobGuid : pullMobs)
                            if (Creature* pullMob = map->GetCreature(mobGuid);
                                pullMob && pullMob->IsAlive() && IdleMobReachesSpot(bot, pullMob, at))
                                return false;
                        return !LeaderAggroAt(bot, at);
                    };
                    bool nearFound = false;
                    for (float ring = LEADER_LOOSE_TANK_YD; ring < away && !nearFound; ring += LEADER_LOOSE_TANK_YD)
                        for (uint32 i = 0; i < 12 && !nearFound; ++i)
                        {
                            float const angle = 2.0f * float(M_PI) * float(i) / 12.0f;
                            float rx = bot->GetPositionX() + std::cos(angle) * ring;
                            float ry = bot->GetPositionY() + std::sin(angle) * ring;
                            float rz = bot->GetPositionZ();
                            if (map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                     bot->GetPositionZ(), rx, ry, rz) &&
                                outOfReach(Position(rx, ry, rz)))
                            {
                                x = rx;
                                y = ry;
                                z = rz;
                                nearFound = found = true;
                            }
                        }
                    if (found &&
                        checkedMove(guid, Leader::MoveEvent::MoveTo, Position(x, y, z), LEADER_FOLLOW_TOLERANCE,
                                    "the pull walks"))
                    {
                        lead.pullWalkSpots[guid] = {pullWalkOrder, reaching->GetGUID(), reaching->GetPosition(),
                                                    Position(x, y, z), false};
                        logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                 JsonObject().Str("why", "the pull walks: out of its reach")
                                     .Str("mob", reaching->GetName()));
                        moved = true;
                    }
                }
                if (!moved)
                {
                    if (checkedMove(guid, Leader::MoveEvent::Hold, bot->GetPosition(), 0.0f, "the pull walks"))
                        logEvent(bot, "Hold", JsonObject().Str("why", "the pull walks: the tank takes the mob first"));
                    lead.pullWalkSpots[guid] = {pullWalkOrder, ObjectGuid::Empty, Position(), bot->GetPosition(), true};
                    moved = true;
                }
            }
            if (!moved && !combat && Leader::LEADER_NO_OWN_FOLLOW && Leader::ExecutorMember(bot) &&
                !bot->IsSitState() && !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
            {
                bool const melee = !PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsHeal(bot);
                bool const following = lead.following.count(guid) > 0;
                float const gap = bot->GetExactDist(tank);
                // [lead27] user: «он выдал - и меняет приказ только если что-то поменялось»: one Follow command (the
                // member keeps its place behind the tank by itself); an engage walk-along to range stays a point
                Leader::PullOrder engage;
                Unit* engaged = nullptr;
                if (lead.commit && lead.commitIssued && lead.commit->kind == "engage" && !melee &&
                    Leader::Orders::GetOpen(group.LeaderGuid(), engage))
                    engaged = ObjectAccessor::GetUnit(*tank, lead.commitMob);
                // [lead81] b124 rfc-h 147.6-166 s: the group walked 120 yd to the engage's range / meeting point while
                // the tank never left (-90,-40) - the tank leads the walk: a walk-along point nearer the engaged mob
                // than the tank stands is ahead of the tank, the member follows the tank until the tank is past it
                Unit* const engagedMob = lead.commit && lead.commitIssued && lead.commit->kind == "engage"
                                             ? ObjectAccessor::GetUnit(*tank, lead.commitMob)
                                             : nullptr;
                auto const aheadOfTank = [&](Position const& at)
                { return engagedMob && engagedMob->GetExactDist(at) < engagedMob->GetExactDist(tank); };
                bool aheadHolds = false;
                // [lead64] melee at an engage: one command when the tank goes - to the meeting point
                bool meleeToMeeting = false;
                if (melee && lead.commit && lead.commitIssued && lead.commit->kind == "engage" &&
                    Leader::Orders::GetOpen(group.LeaderGuid(), engage))
                {
                    Position const meetAt = meleeMeetAt(bot, lead.commitMob);  // [lead83]
                    if (aheadOfTank(meetAt))
                        aheadHolds = true;
                    else
                    {
                        meleeToMeeting = true;
                        if (moveOrStay(bot, meetAt, 3.0f, "along with the tank: to the meeting point"))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                     JsonObject().Str("why", "along with the tank: to the meeting point"));
                        moved = true;
                    }
                }
                // [lead29] the range spot is validated before it is issued (ground, lava / slime / a drop-off, an
                // unpulled mob's reach, the member's own walk guards): the nearest valid one between its range and
                // the tank; none (or the member refused it): the member follows the tank - never "stand" while it
                // walks off
                Position rangeSpot;
                bool rangeValid = false;
                // [lead38] the range spot is kept for the pull while the mob stays put (one command, no re-issue)
                if (engaged)
                {
                    auto const kept = lead.engageRangeSpots.find(guid);
                    // [lead68] b113: the spot moved with the tank (re-issued every ~0.6 s) - it is computed once per
                    // order (the leader's plan) and kept until a new order
                    if (kept != lead.engageRangeSpots.end() && kept->second.first == engaged->GetGUID() &&
                        lead.engageRangeOrder[guid] == engage.id)
                    {
                        rangeSpot = kept->second.second;
                        rangeValid = true;
                    }
                }
                if (engaged && !rangeValid && !lead.engageRefused.count(guid))
                {
                    float const range = PlayerbotAI::IsHeal(bot) ? Leader::LEADER_HEALER_TANK_YD - 4.0f
                                                                 : LeaderSpellReach(bot) - 4.0f;
                    // [lead68] from the leader's meeting point back toward the member (the group's side), not from
                    // the walking tank
                    Position const meet = lead.meetPoint;
                    float const dx = bot->GetPositionX() - meet.GetPositionX();
                    float const dy = bot->GetPositionY() - meet.GetPositionY();
                    float const length = std::max(0.1f, std::hypot(dx, dy));
                    LeaderMoveOrderAction* mover = dynamic_cast<LeaderMoveOrderAction*>(
                        botAI->GetAiObjectContext()->GetAction("leader move order"));
                    for (float at = std::min(range, length); at <= length && !rangeValid; at += 3.0f)
                    {
                        float x = meet.GetPositionX() + dx / length * at;
                        float y = meet.GetPositionY() + dy / length * at;
                        float z = meet.GetPositionZ();
                        if (!map->CheckCollisionAndGetValidCoords(engaged, meet.GetPositionX(), meet.GetPositionY(),
                                                                  meet.GetPositionZ(), x, y, z) ||
                            NearHazard(map, bot->GetPhaseMask(), x, y, z, LEADER_TERRAIN_EDGE))
                            continue;
                        Position const spot(x, y, z);
                        if (LeaderAggroAt(bot, spot) || (mover && !mover->WalkRefusal(spot).empty()))
                            continue;
                        rangeSpot = spot;
                        rangeValid = true;
                    }
                }
                if (meleeToMeeting)
                {
                }
                else if (rangeValid)
                {
                    lead.engageRangeSpots[guid] = {engaged->GetGUID(), rangeSpot};
                    lead.engageRangeOrder[guid] = engage.id;
                    if (aheadOfTank(rangeSpot))
                        aheadHolds = true;  // [lead81]
                    else
                    {
                        if (checkedMove(guid, Leader::MoveEvent::MoveTo, rangeSpot, LEADER_FOLLOW_TOLERANCE,
                                        "along with the tank: to range of the engage"))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                     JsonObject().Str("why", "along with the tank: to range of the engage"));
                        moved = true;
                    }
                }
                // [lead44] the member buffs (its rebuff window): the buff is the command while the tank stands - no
                // Follow that would replace it, only a walk when the tank walks on or the member is far / out of sight
                // [lead47] b96: Rtshaab got fresh follow walks every few seconds under the leader's Rest and never
                // sat - a resting member is not walked along while the tank stands
                if (!moved && !meleeToMeeting &&
                    (aheadHolds ||
                     (!(restCommanded && !tank->isMoving()) &&
                      (engaged || (following && !botAI->forceRebuff.IsPending()) || tank->isMoving() ||
                       gap > LEADER_FOLLOW_START_YD || !bot->IsWithinLOSInMap(tank)))))
                {
                    lead.following.insert(guid);
                    if (Leader::Events::SetFollow(guid, tank->GetGUID(), uint8(index % 4), "along with the tank"))
                        logEvent(bot, "Follow", JsonObject().Str("why", "along with the tank").Real("gap", gap));
                    moved = true;
                }
            }
            // [lead29] a led member always has a command: the group walks and this member got none - no_command (a
            // leader bug), the fallback is Follow
            if (!moved && !combat && tank->isMoving() && Leader::ExecutorMember(bot) && !bot->IsSitState() &&
                !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
            {
                uint32& last = lead.noCommandLogMs[guid];
                if (!last || getMSTimeDiff(last, now) >= 5 * IN_MILLISECONDS)
                {
                    last = now;
                    Decisions().Write(run, "leader", "no_command", guid, bot->GetName(), true,
                                                  JsonObject().Str("member", bot->GetName())
                                                      .Str("why", "the group walks and no command fit: Follow")
                                                      .Real("gap", bot->GetExactDist(tank)));
                }
                if (Leader::Events::SetFollow(guid, tank->GetGUID(), uint8(index % 4), "along with the tank"))
                    logEvent(bot, "Follow", JsonObject().Str("why", "fallback: no other command fit"));
                moved = true;
            }
            if (!moved || combat)
                lead.following.erase(guid);

            // 3. the fight: the healer heals the group, every damage dealer attacks the leader's target
            if (combat && Leader::LEADER_ATTACK_EVENTS)
            {
                if (bot == healer || PlayerbotAI::IsHeal(bot))
                {
                    // [lead61] user: the leader starts the heals - once every fight mob is on the tank (no heal
                    // threat on a mob still coming), or at once when a member is about to die (the config's critical
                    // health)
                    bool allOnTank = true;
                    // [lead62] user: no fixed line - a member is about to die when its time to death (its health over
                    // the damage it took per second in this fight) is within the healer's time to land a heal (its
                    // fastest known heal's cast + the global cooldown + the walk into heal reach) plus the next cast
                    float castSec = 0.0f;
                    {
                        static constexpr uint32 HEALS[] = {2061, 8004, 19750, 8936, 50464, 331, 635, 5185, 2050};
                        for (uint32 spell : HEALS)
                            if (bot->HasSpell(spell))
                                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                                {
                                    float const sec = info->CalcCastTime(bot) / 1000.0f;
                                    castSec = castSec > 0.0f ? std::min(castSec, sec) : sec;
                                }
                    }
                    constexpr float GCD_SEC = 1.5f;  // the game's global cooldown
                    std::string emergencyWhy;
                    bool emergency = false;
                    std::vector<Player*> watched = seats;
                    watched.push_back(tank);
                    for (Player* member : watched)
                    {
                        if (member != tank)
                            for (Unit* attacker : member->getAttackers())
                                if (attacker && attacker->IsAlive() && attacker->GetVictim() == member)
                                    allOnTank = false;
                        // [lead63] predictive: the dps its attackers deal it now (AttackerDps), a new attacker counts
                        // before its first hit; time to death = health + absorbs over that sum
                        float incoming = 0.0f;
                        std::vector<std::pair<float, std::string>> byAttacker;
                        for (Unit* attacker : member->getAttackers())
                            if (attacker && attacker->IsAlive() && attacker->GetVictim() == member)
                            {
                                float spellPart = 0.0f;
                                float const dps = AttackerDps(attacker, member, spellPart);
                                incoming += dps;
                                byAttacker.emplace_back(dps, Acore::StringFormat("{} {:.0f}{}", attacker->GetName(),
                                                                                  dps, spellPart > 0.0f ? " (cast)" : ""));
                            }
                        if (incoming <= 0.0f)
                            continue;
                        float const absorbs = float(member->GetTotalAuraModifier(SPELL_AURA_SCHOOL_ABSORB));
                        float const toDeath = (float(member->GetHealth()) + absorbs) / incoming;
                        float const walk = std::max(0.0f, bot->GetExactDist(member) - Leader::LEADER_HEAL_REACH_YD) /
                                           std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
                        float const response = castSec + GCD_SEC + walk + castSec;
                        if (toDeath <= response)
                        {
                            emergency = true;
                            std::sort(byAttacker.begin(), byAttacker.end(),
                                      [](auto const& a, auto const& b) { return a.first > b.first; });
                            std::string top;
                            for (size_t i = 0; i < byAttacker.size() && i < 3; ++i)
                                top += (i ? ", " : "") + byAttacker[i].second;
                            emergencyWhy = Acore::StringFormat(
                                "{} dies in {:.1f} s ({:.0f} hp + {:.0f} absorb / {:.0f} dps: {}), the heal lands in "
                                "{:.1f} s",
                                member->GetName(), toDeath, float(member->GetHealth()), absorbs, incoming, top,
                                response);
                            break;
                        }
                    }
                    for (Unit* attacker : tank->getAttackers())
                        if (attacker && attacker->IsAlive() && attacker->GetVictim() != tank)
                            allOnTank = false;
                    // [lead89] b131 rfc-h: 4 of 9 aggro losses on the healer - after the first heal "the heals go on"
                    // for the whole fight, also while a joining mob the tank had not hit yet took the heals' threat.
                    // Past the start the heals go on while the healer has headroom on every fight mob (its threat
                    // below the tank's on each: the game's own measure, the damage dealers' rule), or a member dies
                    bool healerHeadroom = lead.healStarted;
                    for (ObjectGuid const& fightMob : killOrder)
                        if (Unit* mob = ObjectAccessor::GetUnit(*tank, fightMob); healerHeadroom && mob &&
                            mob->IsAlive() &&
                            !(mob->GetThreatMgr().GetThreat(bot) < mob->GetThreatMgr().GetThreat(tank)))
                            healerHeadroom = false;
                    if (healerHeadroom || allOnTank || emergency)
                    {
                        if (Leader::Events::SetAttack(guid, Leader::AttackEvent::Heal, ObjectGuid::Empty, {}))
                            logEvent(bot, "Heal", JsonObject().Str("why", allOnTank ? "every fight mob is on the tank"
                                                                         : emergency ? emergencyWhy
                                                                                     : "the heals go on: headroom on "
                                                                                       "every fight mob"));
                        lead.healStarted = true;
                    }
                }
                else if (stopDps)
                {
                    if (Leader::Events::SetAttack(guid, Leader::AttackEvent::StopDps, ObjectGuid::Empty, {}))
                        logEvent(bot, "StopDps", JsonObject().Str("why", "no target of the fight takes damage"));
                }
                else if (!target)
                {
                    if (Leader::Events::SetAttack(guid, Leader::AttackEvent::DontAttack, ObjectGuid::Empty, {}))
                        logEvent(bot, "DontAttack", JsonObject().Str("why", "the fight has no target for it yet"));
                }
                else
                {
                    // [lead53] user: «0 aggro off the tank»: each damage dealer attacks a mob it has threat headroom
                    // on - its threat below the tank's on THAT mob (ThreatManager, no number of ours): the leader's
                    // focus mob first, else the next of the kill order; AoE only while every fight mob in its reach
                    // leaves it headroom
                    auto const headroomOn = [&](Unit* mob)
                    {
                        return mob->GetThreatMgr().GetThreat(bot) < mob->GetThreatMgr().GetThreat(tank);
                    };
                    // [lead67] b112: Rtshaab out-threatened the tank on the OTHER Earthborer (procs, totems, cleave):
                    // a mob is attackable only while the member has headroom on it AND on every fight mob its damage
                    // can reach around it (the AoE radius)
                    // [lead89] b131 rfc-h: 35-44 headroom stops per damage dealer while the tank's lead on their target
                    // averaged 3.9 at the end - a mob near the target the tank had not hit yet (0 against 0) stopped
                    // a ranged single-target attacker too. The mobs around the target count for a member whose damage
                    // splashes onto them (a melee: cleave, procs, its totems); the AoE gate below holds every AoE
                    bool const splashes = !PlayerbotAI::IsRanged(bot);
                    auto const headroom = [&](Unit* mob)
                    {
                        if (!headroomOn(mob))
                            return false;
                        if (!splashes)
                            return true;
                        for (ObjectGuid const& other : killOrder)
                            if (Unit* close = ObjectAccessor::GetUnit(*tank, other);
                                close && close != mob && close->IsAlive() &&
                                mob->GetExactDist(close) <= LEADER_AOE_RADIUS && !headroomOn(close))
                                return false;
                        return true;
                    };
                    Unit* mine = headroom(target) ? target : nullptr;
                    for (ObjectGuid const& next : killOrder)
                    {
                        if (mine)
                            break;
                        if (Unit* mob = ObjectAccessor::GetUnit(*tank, next);
                            mob && mob->IsAlive() && mob->IsInCombat() && headroom(mob))
                            mine = mob;
                    }
                    Unit* const attackOn = mine ? mine : target;
                    Leader::AoeRule memberAoe = aoe;
                    std::string memberAoeWhy = aoeWhy;
                    if (memberAoe != Leader::AoeRule::Forbidden)
                        for (ObjectGuid const& other : killOrder)
                            if (Unit* mob = ObjectAccessor::GetUnit(*tank, other);
                                mob && mob->IsAlive() && attackOn->GetExactDist(mob) <= LEADER_AOE_RADIUS &&
                                !headroom(mob))
                            {
                                memberAoe = Leader::AoeRule::Forbidden;
                                memberAoeWhy = "no threat headroom on " + mob->GetName();
                                break;
                            }
                    lead.noHeadroom[guid] = !mine;  // no mob with headroom: it waits (Intensity Stop below)
                    bool const changed = Leader::Events::SetAttack(guid, Leader::AttackEvent::Attack,
                                                                   attackOn->GetGUID(), killOrder);
                    bool const aoeChanged = Leader::Events::SetAoe(guid, memberAoe, attackOn->GetPosition(),
                                                                   LEADER_AOE_RADIUS);
                    if (changed || aoeChanged)
                        logEvent(bot, "Attack", JsonObject().Str("target", attackOn->GetName())
                                                    .Num("targetEntry", attackOn->GetEntry())
                                                    .Num("killOrder", int64(killOrder.size()))
                                                    .Str("aoe", memberAoe == Leader::AoeRule::Forbidden
                                                                    ? "forbidden"
                                                                    : "allowed-on-pack")
                                                    .Str("aoeWhy", memberAoeWhy)
                                                    .Bool("headroom", mine != nullptr));
                }
                // the aggro-loss victim stops its damage until the tank has the mob back; the others leave the
                // loose mob alone
                // [lead18] a healer is never stopped: Stop is no offensive cast, its heals go on
                bool const victim = loose != looseOn.end() && bot != healer && !PlayerbotAI::IsHeal(bot);
                // [lead52] the tank's threat lead before the damage: a damage dealer whose threat on its target
                // reaches the tank's stops until the tank leads again (the tank's lead is the game's own measure, no
                // number of ours); the start of a pull too - nobody out-threats a tank that has not hit yet
                // [lead53] only when no fight mob leaves it headroom (it was every member at the tank's threat)
                bool const overThreat = !victim && bot != healer && !PlayerbotAI::IsHeal(bot) && target &&
                                        lead.noHeadroom[guid];
                // [lead80] a trash burn when it pays: the member's burn cooldown's effect fits in the fight's
                // seconds and the cooldown is back before the next boss (no boss left: it pays whenever it fits)
                Leader::Intensity burnable = intensity;
                if (burnable != Leader::Intensity::Burn && !victim && !overThreat && bot != healer && botAI)
                    for (char const* burn : Leader::BURN_COOLDOWNS)
                    {
                        uint32 const spell = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", burn)->Get();
                        SpellInfo const* info = spell && bot->HasSpell(spell) && !bot->HasSpellCooldown(spell)
                                                    ? sSpellMgr->GetSpellInfo(spell)
                                                    : nullptr;
                        if (!info)
                            continue;
                        float const effectSec = std::max(info->GetDuration(), 0) / 1000.0f;
                        float const rechargeSec =
                            std::max(info->RecoveryTime, info->CategoryRecoveryTime) / 1000.0f;
                        if (lead.fightSecLeft >= effectSec &&
                            (lead.bossWalkSec < 0.0f || lead.bossWalkSec >= rechargeSec))
                        {
                            burnable = Leader::Intensity::Burn;
                            if (lead.burnLogged.insert(bot->GetGUID()).second)
                                Decisions().Write(
                                    run, "leader", "burn_decision", guid, bot->GetName(), true,
                                    JsonObject().Str("member", bot->GetName()).Str("cooldown", burn)
                                        .Real("fightSec", lead.fightSecLeft).Real("effectSec", effectSec)
                                        .Real("rechargeSec", rechargeSec).Real("bossWalkSec", lead.bossWalkSec));
                            break;
                        }
                    }
                Leader::Intensity const mine = victim || overThreat ? Leader::Intensity::Stop : burnable;
                if (Leader::Events::SetIntensity(guid, mine) && mine != Leader::Intensity::Normal)
                    logEvent(bot, "Intensity",
                             JsonObject().Str("intensity", mine == Leader::Intensity::Stop ? "stop" : "burn")
                                 .Str("why", victim       ? "aggro loss: the tank takes the mob back"
                                             : overThreat ? std::string("threat: no mob with headroom")
                                                          : "a dungeon boss"));
                if (!victim && !looseOn.empty() && bot != healer && !lead.looseIsTarget &&
                    Leader::Events::SetDontAttack(guid, looseOn.begin()->second->GetGUID()))
                    logEvent(bot, "DontAttack", JsonObject().Str("mob", looseOn.begin()->second->GetName())
                                                    .Str("why", "loose on a member until the tank has it"));
            }
        }

        // [lead104] the flee stop is ONE leader command per fleer (project rule: the leader decides, a member
        // executes): a fight mob at its flee-for-assist health or running gets one member with a stop ready (kept
        // while it can), else it is the damage dealers' focus (burst before it runs). The facts are the leader's
        // (Ai/Leader/Fight/PullFacts)
        if (fighting && Leader::LEADER_ATTACK_EVENTS)
        {
            std::vector<Player*> stoppers = seats;
            stoppers.push_back(tank);
            for (ObjectGuid const& fightMob : std::vector<ObjectGuid>(killOrder))
            {
                Unit* fleer = ObjectAccessor::GetUnit(*tank, fightMob);
                if (!Leader::Fight::NeedsFleeStop(fleer))
                {
                    lead.fleeStoppers.erase(fightMob);
                    continue;
                }
                Player* stopper = nullptr;
                if (auto const kept = lead.fleeStoppers.find(fightMob); kept != lead.fleeStoppers.end())
                    if (Player* keptBot = ObjectAccessor::FindPlayer(kept->second))
                        if (Leader::Fight::FleeStopper({keptBot}, fleer) == keptBot)
                            stopper = keptBot;
                if (!stopper)
                    stopper = Leader::Fight::FleeStopper(stoppers, fleer);
                if (stopper)
                {
                    lead.fleeStoppers[fightMob] = stopper->GetGUID();
                    if (Leader::Events::SetFleeStop(stopper->GetGUID(), fightMob))
                        logEvent(stopper, "FleeStop", JsonObject().Str("mob", fleer->GetName())
                                                          .Real("health", fleer->GetHealthPct()));
                }
                else
                {
                    lead.fleeStoppers.erase(fightMob);
                    killOrder.erase(std::remove(killOrder.begin(), killOrder.end(), fightMob), killOrder.end());
                    killOrder.insert(killOrder.begin(), fightMob);  // nobody can stop it: the focus bursts it
                    if (lead.fleeFocusLogged != fightMob)
                    {
                        lead.fleeFocusLogged = fightMob;
                        Decisions().Write(run, "leader", "flee_focus", fightMob, fleer->GetName(), true,
                                                      JsonObject().Real("health", fleer->GetHealthPct()));
                    }
                }
            }
        }

        // 3b. interrupts: one kicker per casting mob, the member with an interrupt ready (kept while it holds)
        if (fighting && Leader::LEADER_ATTACK_EVENTS)
        {
            std::set<ObjectGuid> busy;
            for (Unit* caster : casters)
            {
                Player* kicker = nullptr;
                if (auto const kept = lead.kickers.find(caster->GetGUID()); kept != lead.kickers.end())
                    if (Player* bot = ObjectAccessor::FindPlayer(kept->second);
                        bot && bot->IsAlive() && LeaderReadyInterrupt(bot))
                        kicker = bot;
                for (Player* bot : seats)
                {
                    if (kicker)
                        break;
                    // [lead90] b131 946.1 s: Rtmagaa's Counterspell on a Searing Blade Warlock the tank had no threat
                    // on - a kicker only while it has threat headroom on the caster (its threat below the tank's)
                    if (bot != healer && !busy.count(bot->GetGUID()) && LeaderReadyInterrupt(bot) &&
                        bot->GetExactDist(caster) <= 30.0f &&
                        caster->GetThreatMgr().GetThreat(bot) < caster->GetThreatMgr().GetThreat(tank))
                        kicker = bot;
                }
                if (!kicker || busy.count(kicker->GetGUID()))
                    continue;
                busy.insert(kicker->GetGUID());
                lead.kickers[caster->GetGUID()] = kicker->GetGUID();
                if (Leader::Events::SetInterrupt(kicker->GetGUID(), caster->GetGUID()))
                    logEvent(kicker, "Interrupt", JsonObject().Str("caster", caster->GetName())
                                                      .Num("casterEntry", caster->GetEntry())
                                                      .Num("spell", LeaderReadyInterrupt(kicker)));
            }
        }
        else
            lead.kickers.clear();

        // 3c. external saves and buffs on a target: the leader picks who and when, the member casts when told
        if (fighting && Leader::LEADER_ATTACK_EVENTS)
        {
            std::vector<Player*> everyone = seats;
            everyone.push_back(tank);
            std::set<ObjectGuid> told;
            auto const tell = [&](Player* caster, uint32 spell, Unit* on, char const* why)
            {
                if (!caster || !spell || told.count(caster->GetGUID()))
                    return false;
                told.insert(caster->GetGUID());
                if (Leader::Events::SetExternal(caster->GetGUID(), spell, on ? on->GetGUID() : ObjectGuid::Empty))
                    logEvent(caster, "External", JsonObject().Num("spell", spell)
                                                     .Str("on", on ? on->GetName() : "group").Str("why", why));
                return true;
            };
            auto const withSpell = [&](std::initializer_list<uint32> spells, Player* except)
            {
                for (Player* bot : seats)
                    if (bot != except && !told.count(bot->GetGUID()))
                        if (uint32 const spell = LeaderReadySpell(bot, spells))
                            return std::make_pair(bot, spell);
                return std::make_pair(static_cast<Player*>(nullptr), uint32(0));
            };
            bool const opening = group.PullStartMs() && getMSTimeDiff(group.PullStartMs(), now) <= LEADER_OPENING_MS;
            float const healerMana = healer && healer->GetMaxPower(POWER_MANA) ? healer->GetPowerPct(POWER_MANA)
                                                                              : 100.0f;
            uint32 hurt = 0;
            for (Player* bot : everyone)
                hurt += bot->GetHealthPct() < 50.0f ? 1 : 0;
            if (opening)
                if (auto const [bot, spell] = withSpell({SPELL_MISDIRECTION, SPELL_TRICKS_OF_THE_TRADE}, tank); bot)
                    tell(bot, spell, tank, "the pull's threat onto the tank");
            if (tank->GetHealthPct() < LEADER_SAVE_HP_PCT)
                if (auto const [bot, spell] = withSpell({SPELL_PAIN_SUPPRESSION, SPELL_HAND_OF_SACRIFICE}, tank); bot)
                    tell(bot, spell, tank, "the tank is low");
            for (Player* member : seats)
                if (member != healer && !PlayerbotAI::IsTank(member) && !member->getAttackers().empty() &&
                    member->GetHealthPct() < LEADER_SAVE_HP_PCT)
                    if (auto const [bot, spell] = withSpell({SPELL_HAND_OF_PROTECTION_1, SPELL_HAND_OF_PROTECTION_2,
                                                             SPELL_HAND_OF_PROTECTION_3},
                                                            member);
                        bot)
                        tell(bot, spell, member, "a member is attacked and low");
            if (healer && healerMana < LEADER_SAVE_MANA_PCT)
            {
                if (auto const [bot, spell] = withSpell({SPELL_INNERVATE}, nullptr); bot)
                    tell(bot, spell, healer, "the healer is out of mana");
                else if (auto const [priest, hymn] = withSpell({SPELL_HYMN_OF_HOPE}, nullptr); priest)
                    tell(priest, hymn, nullptr, "the healer is out of mana");
            }
            if (hurt >= 3)
                if (auto const [bot, spell] = withSpell({SPELL_DIVINE_HYMN}, nullptr); bot)
                    tell(bot, spell, nullptr, "three members under half health");
            // [lead105] Heroism / Bloodlust is the group's cooldown: the leader's command at a boss fight's burn (one
            // shaman, the group as the target); the shaman's own trigger is off in a led group
            if (intensity == Leader::Intensity::Burn && target && target->ToCreature() &&
                target->ToCreature()->IsDungeonBoss())
                if (auto const [bot, spell] = withSpell({SPELL_HEROISM, SPELL_BLOODLUST}, nullptr); bot)
                    tell(bot, spell, nullptr, "the boss burn: the group's haste");
            if (intensity == Leader::Intensity::Burn)
                for (Player* member : seats)
                    if (member != healer && member->getPowerType() == POWER_MANA && !PlayerbotAI::IsTank(member))
                    {
                        if (auto const [bot, spell] = withSpell({SPELL_POWER_INFUSION}, member); bot)
                            tell(bot, spell, member, "the burn");
                        break;
                    }
        }

        // [lead17] the tank out of a hazard (its own "move fight off hazard" is off): a short step, in heal reach
        // [lead56] in a fight the tank's own (LEADER_TANK_OWNS_FIGHT)
        if (Leader::LEADER_HAZARD_SPOTS && tank->IsAlive() && !(Leader::LEADER_TANK_OWNS_FIGHT && tank->IsInCombat()))
        {
            std::vector<HazardZone> const& zones = lead.hazardZones[tank->GetGUID()];
            if (HazardZone const* in = zones.empty() ? nullptr
                                                     : LeaderZoneAt(zones, tank->GetPositionX(), tank->GetPositionY(),
                                                                    tank->GetPositionZ(), 0.5f))
            {
                hazardSeen = true;
                Position spot;
                if (slow([&] { return LeaderSafeSpot(tank, GET_PLAYERBOT_AI(tank), zones, keepsHealer, spot); }) &&
                    checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, spot, 1.0f,
                                            std::string("hazard: ") + in->what))
                {
                    logEvent(tank, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", std::string("hazard: ") + in->what).Str("who", "tank")
                                 .Real("x", spot.GetPositionX()).Real("y", spot.GetPositionY())
                                 .Real("z", spot.GetPositionZ()));
                    LogFightDrag(run, tank, spot, lead.meetPoint, "hazard");
                }
            }
        }

        // 4. the heal reach in the fight: a member out of the healer's reach or sight moves the healer (a spot that
        // reaches everyone), else it is reported
        if (Leader::LEADER_HEAL_REACH && healer && fighting &&
            (!lead.healReachMs || getMSTimeDiff(lead.healReachMs, now) >= 0))
        {
            lead.healReachMs = now;
            // [lead38] b87 rfc-h 511-596 s: 19 + 19 "out of the healer's reach" with the members 0.0 yd from the
            // healer - the healer's planned spot was judged, not where it stands: out only when neither reaches
            std::vector<Player*> out;
            auto const outOfReach = [&](Position const& at)
            { return !reaches(healerAt, at) && !reaches(healer->GetPosition(), at); };
            Position const tankAt = tankFightAt();  // [lead120]
            if (outOfReach(tankAt))
                out.push_back(tank);
            for (Player* other : seats)
                if (other != healer && outOfReach(plannedAt(other)) && outOfReach(other->GetPosition()))
                    out.push_back(other);
            // [lead55] the heal-reach spot it walks to is kept while it still keeps the group (no new spot per tick)
            // [lead120] b154 rfc-h 20261101-103101 127.2-134.7 s: the fallback spot was taken anew from the walking
            // tank and healer every 0.3 s (x -191 -> -174 -> -205, command_repeat) - one decision per fact: the spot
            // is also kept while it reaches where the tank fights, the fight anchor is the one it was judged
            // against and no member went out of reach since it was decided
            Leader::MemberEvents healerEvents;
            bool const ownSpot = Leader::Events::Get(healer->GetGUID(), healerEvents) &&
                                 healerEvents.move == Leader::MoveEvent::MoveTo &&
                                 healerEvents.moveWhy == "heal reach";
            bool sameFacts = ownSpot && lead.healReachAnchor == lead.fightAnchorSerial &&
                             reaches(healerEvents.spot, tankAt);
            for (Player* other : out)
                sameFacts = sameFacts && lead.healReachOut.count(other->GetGUID()) > 0;
            bool const keepsOwn = ownSpot && (healerKeeps(healerEvents.spot) || sameFacts);
            // [lead72] b116: the kept heal-reach spot was not re-asserted (the tick ended it), the combat spot took the
            // healer back the next tick ("was -") and the heal reach respotted again - the held spot is asserted
            // every tick while it still keeps the group (its end condition), so a lower rule cannot take it
            if (keepsOwn)
            {
                out.clear();
                Leader::Events::SetMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, healerEvents.spot,
                                        healerEvents.tolerance, "heal reach");
            }
            if (!out.empty())
            {
                Position fix;
                bool const fixed =
                    slow([&]
                         {
                             SpotAcquire const acquire;  // [lead36]
                             return LeaderHealerRespot(healer, GET_PLAYERBOT_AI(healer), healerKeeps, fix);
                         });
                bool issued = fixed && tankHolds(healer->GetPosition(), fix) &&
                              checkedMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, fix, 1.5f, "heal reach");
                // [lead118] b152 rfc-h 895-931 s: "Rtpalaa out of the healer's reach, 57.9 yd, fix: none" until the
                // wipe (the re-spot searches 16 yd round the healer) - in a fight the healer always gets a spot:
                // on the line from the tank to the healer within the heal reach (in sight of the tank), else the
                // tank's own spot (the same checks before it is issued); [lead120] the tank where it fights
                std::string how = issued ? "the healer re-spots" : "none";
                if (!issued)
                {
                    float const dx = healer->GetPositionX() - tankAt.GetPositionX();
                    float const dy = healer->GetPositionY() - tankAt.GetPositionY();
                    float const length = std::max(0.1f, std::hypot(dx, dy));
                    for (float at = std::min(length, Leader::LEADER_HEAL_REACH_YD - 5.0f); at > 0.0f && !issued;
                         at -= 5.0f)
                    {
                        float x = tankAt.GetPositionX() + dx / length * at;
                        float y = tankAt.GetPositionY() + dy / length * at;
                        float z = tankAt.GetPositionZ();
                        if (!map->CheckCollisionAndGetValidCoords(tank, tankAt.GetPositionX(), tankAt.GetPositionY(),
                                                                  tankAt.GetPositionZ(), x, y, z) ||
                            !map->isInLineOfSight(x, y, z + 2.0f, tankAt.GetPositionX(), tankAt.GetPositionY(),
                                                  tankAt.GetPositionZ() + 2.0f, tank->GetPhaseMask(),
                                                  LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                            continue;
                        fix.Relocate(x, y, z);
                        issued = checkedMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, fix, 1.5f, "heal reach");
                    }
                    if (!issued)
                    {
                        fix = tankAt;
                        issued = checkedMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, fix, 1.5f, "heal reach");
                    }
                    if (issued)
                        how = "the healer walks back into reach of the tank";
                }
                if (issued)
                {
                    // [lead120] the facts this decision was made for
                    lead.healReachAnchor = lead.fightAnchorSerial;
                    lead.healReachOut.clear();
                    for (Player* other : out)
                        lead.healReachOut.insert(other->GetGUID());
                }
                if (issued)
                    logEvent(healer, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", "heal reach").Real("x", fix.GetPositionX())
                                 .Real("y", fix.GetPositionY()).Real("z", fix.GetPositionZ()));
                for (Player* other : out)
                    LogHealReach(group, other, healer, "out of the healer's reach in the fight", how);
            }
        }

        lead.hazardActive = hazardSeen;
        // [lead18] healer_lost_tank_s: the fight's time with the healer out of sight or LEADER_HEALER_LOST_YD of the
        // tank (the heal-reach check above re-spots it the same update)
        if (fighting && healer && tank->IsAlive())
        {
            uint32 const step = lead.lostCheckMs ? std::min<uint32>(getMSTimeDiff(lead.lostCheckMs, now), 500) : 0;
            if (healer->GetExactDist(tank) > Leader::LEADER_HEALER_LOST_YD || !healer->IsWithinLOSInMap(tank))
                lead.healerLostTankMs += step;
        }
        lead.lostCheckMs = now;
        uint64 const totalUs = uint64(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - tickStart).count());
        lead.cadenceSlowUs += lead.slowUsTick;
        lead.cadenceFastUs += totalUs > lead.slowUsTick ? totalUs - lead.slowUsTick : 0;
        lead.cadenceMaxUs = std::max(lead.cadenceMaxUs, totalUs);

        if (!lead.wakeLogMs)
            lead.wakeLogMs = now;
        else if (getMSTimeDiff(lead.wakeLogMs, now) >= LEADER_WAKE_LOG_MS)
        {
            uint32 const sinceMs = getMSTimeDiff(lead.wakeLogMs, now);
            float const minutes = float(sinceMs) / float(MINUTE * IN_MILLISECONDS);
            uint32 ticks = 0;
            for (uint32 count : lead.cadenceTicks)
                ticks += count;
            group.LogEvent("member_wake", JsonObject().Num("count", lead.wakeCount)
                                               .Num("sinceSec", sinceMs / IN_MILLISECONDS)
                                               .Num("total", lead.wakeTotal + lead.wakeCount));
            // the leader's cadence per state (ticks per minute) and the layers' cost per tick (µs)
            group.LogEvent("leader_cadence",
                     JsonObject().Num("sinceSec", sinceMs / IN_MILLISECONDS)
                         .Real("calmPerMin", float(lead.cadenceTicks[0]) / minutes)
                         .Real("prePullPerMin", float(lead.cadenceTicks[1]) / minutes)
                         .Real("combatPerMin", float(lead.cadenceTicks[2]) / minutes)
                         .Real("hazardPerMin", float(lead.cadenceTicks[3]) / minutes)
                         .Real("fastUsPerTick", ticks ? float(lead.cadenceFastUs) / float(ticks) : 0.0f)
                         .Real("slowUsPerTick", ticks ? float(lead.cadenceSlowUs) / float(ticks) : 0.0f)
                         .Num("maxUs", int64(lead.cadenceMaxUs)).Num("ticks", ticks)
                         .Real("usPerSec", float(lead.cadenceFastUs + lead.cadenceSlowUs) /
                                               (float(sinceMs) / 1000.0f)));
            lead.wakeTotal += lead.wakeCount;
            lead.wakeCount = 0;
            lead.wakeLogMs = now;
            for (uint32& count : lead.cadenceTicks)
                count = 0;
            lead.cadenceFastUs = lead.cadenceSlowUs = lead.cadenceMaxUs = 0;
        }
    }

    // [lead15] a heal_reach violation as a decision record (per member at most every LEADER_HEAL_REACH_LOG_MS)
    void LogHealReach(LeaderGroup& group, Player* member, Player* healer, std::string const& why,
                      std::string const& fix)
    {
        auto& lead = group.State();
        uint32 const now = getMSTime() ? getMSTime() : 1;
        uint32& last = lead.healReachLogMs[member->GetGUID()];
        if (last && getMSTimeDiff(last, now) < LEADER_HEAL_REACH_LOG_MS)
            return;
        last = now;
        DecisionRun const run = group.Run();
        Decisions().Write(run, "leader", "heal_reach", member->GetGUID(), member->GetName(), true,
                                      JsonObject().Str("member", member->GetName())
                                          .Str("healer", healer ? healer->GetName() : "?")
                                          .Real("distance", healer ? healer->GetExactDist(member) : -1.0f)
                                          .Bool("los", healer && healer->IsWithinLOSInMap(member))
                                          .Str("why", why).Str("fix", fix));
    }
}
