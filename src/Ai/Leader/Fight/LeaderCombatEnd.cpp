/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IssueKey.h"  // [jrn43] the deduplication key and the is_real criterion, as a seam a test asserts
#include "Journal.h"  // [jrn40] the stale-victim stuck row and its issue
#include "Common.h"
#include "LeaderCombatEnd.h"

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
#include "SpellAuraEffects.h"  // [fight105] AuraEffect
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

namespace Leader::Fight
{
    // [lead107 stage 5 timers] what a unit of the group's combat does: it attacks (its victim in its melee reach, a
    // cast, or a complete walk to its victim), the leader can walk to it (a complete path, a valid target on its
    // map), or neither (dead, evading, unreachable)
    enum class RefFight : uint8
    {
        None,
        Reachable,
        Attacks
    };

    // `owner`: the group's unit whose combat ref this is (nullptr: none)
    RefFight JudgeRef(Player* leader, Unit* other, float& pathDistance, std::string& pathNote, Unit* owner = nullptr)
    {
        Creature* creature = other->ToCreature();
        if (!other->IsAlive() || (creature && creature->IsInEvadeMode()))
            return RefFight::None;
        // [fight105] b417 rfc-n five 4 (20261110-064015) 430 s: the Ragefire Shaman of pull 15 on the tank, 42 yd
        // off, between its casts, no complete path to its victim; the hunter's Serpent Sting and the warlock's
        // Immolate ticking on it (96 -> 80 %), the hunter's Auto Shot on it. "Nothing attacks, a mob reachable"
        // counted the group out of combat: pull 15 ended after 3 s and the same fight came back at 442 s as pull 16,
        // "unplanned: Rumanatol" (the healer's totems put it in the tank's combat - no cast of its own on a mob).
        // A mob the group's unit hits - its cast or auto-repeat shot on it, or its damage over time ticking on it -
        // is in the fight
        if (owner && owner->IsAlive())
        {
            for (CurrentSpellTypes const type :
                 {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL})
                if (Spell const* spell = owner->GetCurrentSpell(type);
                    spell && spell->m_targets.GetUnitTarget() == other && !spell->GetSpellInfo()->IsPositive())
                    return RefFight::Attacks;
            for (AuraType const type : {SPELL_AURA_PERIODIC_DAMAGE, SPELL_AURA_PERIODIC_LEECH,
                                        SPELL_AURA_PERIODIC_DAMAGE_PERCENT})
                for (AuraEffect const* effect : other->GetAuraEffectsByType(type))
                    if (effect->GetCasterGUID() == owner->GetGUID())
                    {
                        thread_local std::set<std::pair<uint64, uint64>> hitLogged;
                        if (hitLogged.size() > 4096)
                            hitLogged.clear();
                        if (hitLogged.insert({other->GetGUID().GetRawValue(), owner->GetGUID().GetRawValue()}).second)
                            LOG_INFO("playerbots", "[fight105] {}: {}'s {} ticks on {}: the fight is on, no phantom "
                                     "combat", leader->GetName(), owner->GetName(),
                                     effect->GetSpellInfo()->SpellName[0], other->GetName());
                        return RefFight::Attacks;
                    }
        }
        if (Unit* victim = other->GetVictim())
        {
            if (other->IsWithinMeleeRange(victim) || other->HasUnitState(UNIT_STATE_CASTING))
                return RefFight::Attacks;
            if (victim->GetMap() == other->GetMap())
            {
                PathGenerator path(other);
                path.CalculatePath(victim->GetPositionX(), victim->GetPositionY(), victim->GetPositionZ());
                if (!(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
                    return RefFight::Attacks;
            }
        }
        // [lead980] b228 rfc-h 5D-4 (20261103-221518) 318 / 328 s: eight Troggs and Shamans stood 0.8-9 yd from the
        // arcane mage, their victim the shaman healer on the level above (no path to it): "nothing attacks or can be
        // reached" ended every member's combat twice, the leader called the fight over and walked the group 80 yd
        // on while those mobs hit the mage from 59 % to its death. A live mob that has the ref's member on its
        // threat list and stands in its melee reach hits it on its next victim pick: a fight
        if (owner && owner->IsAlive() && other->IsWithinMeleeRange(owner) &&
            other->GetThreatMgr().GetThreat(owner) > 0.0f)
        {
            Unit* const victim = other->GetVictim();
            if (victim != owner)
            {
                thread_local std::set<std::pair<uint64, uint64>> logged;
                if (logged.size() > 4096)
                    logged.clear();
                if (logged.insert({other->GetGUID().GetRawValue(), owner->GetGUID().GetRawValue()}).second)
                    LOG_INFO("playerbots", "[lead980] {}: {} stands in {}'s melee reach with it on its threat list "
                             "(victim {}): the fight is on, no phantom combat",
                             leader->GetName(), other->GetName(), owner->GetName(),
                             victim ? victim->GetName() : std::string("nobody"));
            }
            return RefFight::Attacks;
        }
        if (!leader->IsValidAttackTarget(other) || other->GetMap() != leader->GetMap())
            return RefFight::None;
        PathGenerator path(leader);
        path.CalculatePath(other->GetPositionX(), other->GetPositionY(), other->GetPositionZ());
        if (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
        {
            // [lead980] ... and the mobs were walkable: the mage had walked there. PathGenerator's straight search
            // stops at its own length cap; the leader's route search (the walks it orders) is the reach
            Leader::RouteResult const route = Leader::Route(leader, other->GetPosition());
            if (!route.found || !route.complete)
                return RefFight::None;
            pathDistance = route.length;
            pathNote = Acore::StringFormat("{}, {:.0f} yd", route.Describe(), route.length);
            return RefFight::Reachable;
        }
        pathDistance = leader->GetExactDist(other);
        pathNote = Acore::StringFormat("path type {}, {} points", uint32(path.GetPathType()), path.GetPath().size());
        return RefFight::Reachable;
    }

    void TickPhantomCombat(LeaderGroup& group, std::vector<Player*> const& players, Player* leader, uint32 now,
                           bool activeBoss, bool& anyInCombat)
    {
        GroupRouteState& route = group.Route();
        // Phantom combat: somebody stays flagged in combat while nothing of the fight fights (a mob stuck on a ledge,
        // a fish below the surface). It held Ragefire Chasm in "clearing trash" for 18 minutes: no stall detection,
        // no resurrection (the paladin's Redemption is a non-combat action). Not during a boss attempt (phases
        // without damage).
        // [lead107 stage 5 timers] user: no elapsed-time rule decides (PHANTOM_COMBAT_MS / PHANTOM_COMBAT_END_MS were
        // the old clocks). The facts of every unit in the group's combat refs (members, their pets and guardians):
        // one that attacks keeps the fight on; none attacks but one the leader can walk to: the group is sent to it
        // (the phantom mob hunt) and travels; none attacks and none can be reached (dead, evading, unreachable):
        // the run counts the group out of combat and the refs are ended - once per set of refs (a new ref after
        // the end is a new fact, ended again).
        if (anyInCombat && !activeBoss)
        {
            bool attacks = false;
            Creature* hunt = nullptr;
            float huntDistance = 0.0f;
            std::string huntPath;
            std::set<ObjectGuid> others;
            auto const judge = [&](Unit* owner)
            {
                for (auto const& [guid, ref] : owner->GetCombatManager().GetPvECombatRefs())
                {
                    Unit* other = ref ? ref->GetOther(owner) : nullptr;
                    if (!other)
                        continue;
                    others.insert(other->GetGUID());
                    if (attacks)
                        continue;
                    float distance = 0.0f;
                    std::string note;
                    RefFight const fight = JudgeRef(leader, other, distance, note, owner);
                    if (fight == RefFight::Attacks)
                        attacks = true;
                    // [dun255] b463 wc-n five6-6 4029-4321 s: a Snake (critter 2914) held the escort 240 s - no
                    // member ever targets a critter (AggressiveTargetValue "possible targets: critter"), so its
                    // hunt order stood Issued; a critter's ref is ended like an unreachable one
                    else if (fight == RefFight::Reachable && other->IsCreature() && !other->IsCritter() &&
                             (!hunt || distance < huntDistance))
                    {
                        hunt = other->ToCreature();
                        huntDistance = distance;
                        huntPath = note;
                    }
                }
            };
            for (Player* bot : players)
            {
                if (!bot->IsAlive())
                    continue;
                judge(bot);
                for (Unit* controlled : bot->m_Controlled)
                    if (controlled && controlled->IsInWorld())
                        judge(controlled);
            }

            if (attacks)
            {
                // a real fight: the next phantom episode starts from nothing
                route.phantomCombatLogged = false;
                route.phantomCombatEnded = false;
                route.phantomEndedRefs.clear();
                return;
            }

            if (!route.phantomCombatLogged)
            {
                route.phantomCombatLogged = true;
                route.phantomCombatSinceMs = now ? now : 1;  // the episode's start, for the logs only
                route.phantomCombatEnds = 0;
                JsonArray refs;
                for (Player* bot : players)
                {
                    if (!bot->IsAlive() || !bot->IsInCombat())
                        continue;
                    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                    {
                        Unit* other = ref ? ref->GetOther(bot) : nullptr;
                        if (!other)
                            continue;
                        Unit* victim = other->GetVictim();
                        refs.Raw(JsonObject().Str("member", bot->GetName()).Str("with", other->GetName())
                                     .Num("entry", other->IsCreature() ? other->GetEntry() : 0)
                                     .Bool("alive", other->IsAlive())
                                     .Bool("evading", other->IsCreature() && other->ToCreature()->IsInEvadeMode())
                                     .Bool("underWater", other->IsUnderWater())
                                     .Real("dist", bot->GetExactDist(other))
                                     .Real("dz", other->GetPositionZ() - bot->GetPositionZ())
                                     .Str("victim", victim ? victim->GetName() : "").Build());
                    }
                }
                group.LogEvent("phantom_combat", JsonObject().Str("why", hunt ? "nothing attacks, a mob reachable"
                                                                               : "nothing attacks or can be reached")
                                                     .Raw("refs", refs.Build()));
            }

            // [phantom_mob_hunt] User 2026-10-25: the group stood while a mob stood. strat-h
            // 20261025-163739-strat-h-5A-11 pulls 52-55: a Crimson Initiate 10420 alive, not evading, 97-133 yd off on
            // the hunter's Raptor kept every pull 45 s in phantom combat. A live hostile the leader can walk to (a
            // complete path) is made the group's target, once per mob; only an unreachable one has its combat ended.
            // [lead128] user: no elapsed-time rule decides (PHANTOM_HUNT_MS 2 min per hunt, PHANTOM_HUNT_MAX_TRIES 3
            // were the old clock and cap). The hunt lasts while its facts hold - the mob alive, still the group's
            // target, still one the leader can walk to (or attacking) - and each mob is hunted once per episode
            Creature* hunted = route.phantomHunt ? leader->GetMap()->GetCreature(route.phantomHunt) : nullptr;
            float huntedDistance = 0.0f;
            std::string huntedNote;
            bool hunting = hunted && route.target == route.phantomHunt &&
                           JudgeRef(leader, hunted, huntedDistance, huntedNote) != RefFight::None;
            if (!hunting && !route.phantomHunt.IsEmpty())
            {
                if (route.neighbourTarget == route.phantomHunt)
                    route.neighbourTarget.Clear();  // the route's hold on it ends with the hunt
                route.phantomHunt.Clear();
            }
            if (!hunting && hunt && !route.phantomHunted.count(hunt->GetGUID()))
            {
                route.phantomHunted.insert(hunt->GetGUID());
                route.phantomHunt = hunt->GetGUID();
                route.neighbourTarget = hunt->GetGUID();
                // the route keeps it as its target until the hunt's facts end it (above)
                route.target = hunt->GetGUID();
                route.targetIsBoss = false;
                route.targetIsStarter = false;
                route.targetRouteRule = "phantom mob hunt";
                route.targetSinceMs = now;
                route.guaranteeHolding = false;
                route.arrivedMs = 0;
                route.approach = 0;
                route.approachBlocker.Clear();
                route.stopSinceMs = 0;
                hunting = true;
                Unit* victim = hunt->GetVictim();
                LOG_INFO("playerbots", "[phantom_mob_hunt] {}: {} {} alive, {:.0f} % health, {:.1f} yd "
                         "(dz {:.1f}), on {}: reachable ({}), the group's target (hunt {})",
                         leader->GetName(), hunt->GetName(), hunt->GetEntry(), hunt->GetHealthPct(),
                         huntDistance, hunt->GetPositionZ() - leader->GetPositionZ(),
                         victim ? victim->GetName() : "nobody", huntPath, route.phantomHunted.size());
                group.LogEvent("phantom_mob_hunt",
                               JsonObject().Str("mob", hunt->GetName()).Num("entry", hunt->GetEntry())
                                   .Real("hpPct", hunt->GetHealthPct()).Real("dist", huntDistance)
                                   .Real("dz", hunt->GetPositionZ() - leader->GetPositionZ())
                                   .Str("victim", victim ? victim->GetName() : "").Str("path", huntPath)
                                   .Num("try", uint32(route.phantomHunted.size())));
            }

            // nothing attacks and no hunt runs: the refs are ended, once per set ([b33_sh2] sh-n 5B-89: a ref that
            // came back after the end - a new set - is ended again)
            if (!hunting && others != route.phantomEndedRefs)
            {
                route.phantomEndedRefs = others;
                route.phantomCombatEnded = true;
                route.phantomCombatEndedMs = now ? now : 1;
                ++route.phantomCombatEnds;
                // the pets and guardians too: a pet's ref put its owner back in combat after every pull
                // (strat-h 5A-11: the Crimson Initiate on the hunter's Raptor, 45 s after pulls 52-55)
                uint32 controlledEnded = 0;
                for (Player* bot : players)
                {
                    if (!bot->IsAlive())
                        continue;
                    bot->GetCombatManager().EndAllPvECombat();
                    for (Unit* controlled : bot->m_Controlled)
                    {
                        if (controlled && controlled->IsInWorld() && controlled->IsInCombat())
                        {
                            controlled->GetCombatManager().EndAllPvECombat();
                            ++controlledEnded;
                        }
                    }
                }
                // [b33_sh2] who is still flagged right after the end, and with what (the ref that comes back)
                JsonArray stillFlagged;
                for (Player* bot : players)
                {
                    if (!bot->IsAlive() || !bot->IsInCombat())
                        continue;
                    std::string with;
                    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                        if (Unit* other = ref ? ref->GetOther(bot) : nullptr)
                            with += (with.empty() ? "" : ", ") + std::string(other->GetName());
                    stillFlagged.Raw(JsonObject().Str("member", bot->GetName()).Str("with", with)
                                         .Num("attackers", bot->getAttackers().size())
                                         .Bool("pvp", bot->GetCombatManager().HasPvPCombat()).Build());
                }
                group.LogEvent("phantom_combat_end",
                               JsonObject().Num("refs", uint32(others.size())).Num("petsEnded", controlledEnded)
                                   .Num("huntTries", uint32(route.phantomHunted.size()))
                                   .Num("end", route.phantomCombatEnds)
                                   .Raw("stillFlagged", stillFlagged.Build()));
            }
            anyInCombat = false;
        }
        else if (!anyInCombat)
        {
            route.phantomCombatSinceMs = 0;
            route.phantomCombatLogged = false;
            route.phantomEndedRefs.clear();
            route.phantomHunted.clear();  // [phantom_mob_hunt] a real fight end: the next phantom hunts anew
            route.phantomHunt.Clear();
        }
    }

    void TickStaleVictim(LeaderGroup& group, Player* leader, uint32 now, bool anyInCombat, bool activeBoss)
    {
        GroupRouteState& route = group.Route();
        // [queue-stale-idle-victim] the leader's swing / target on a mob that is no fight any more, outside any fight
        // (no attacker, not in combat), is dropped: AttackStop, no current target, the non-combat engine - the rest
        // triggers run, and the tank's own pick takes a target again.
        // [lead107 stage 5 timers] user: no elapsed-time rule decides (STALE_VICTIM_MS: 15 s on an idle mob out of
        // melee range was the old clock). The victim's own facts decide: dead, gone (its guid resolves to nothing),
        // evading, or off the leader's map / out of the world; once per victim (one command per change of facts).
        if (!anyInCombat && !activeBoss && leader->IsAlive() && !leader->IsInCombat() &&
            leader->getAttackers().empty())
        {
            PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
            Unit* victim = leader->GetVictim();
            Unit* current = leaderAI ? leaderAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get()
                                     : nullptr;
            Unit* stale = victim ? victim : current;
            ObjectGuid staleGuid;
            std::string why;
            if (stale)
            {
                staleGuid = stale->GetGUID();
                if (!stale->IsInWorld() || stale->GetMap() != leader->GetMap())
                    why = "out of the map";
                else if (!stale->IsAlive())
                    why = "dead";
                else if (stale->IsCreature() && stale->ToCreature()->IsInEvadeMode())
                    why = "evading";
                // [lead128] build 27 (Shirrak / Hungarfen / Hearthsinger): the tank's own pick held an idle boss in
                // its swing out of melee range. In a group the leader leads, the tank's target outside a fight is
                // the leader's pull order's: an idle mob no open order is for is no target of the group
                else if (group.LeaderDecides() && stale->IsCreature() && !stale->IsInCombat() &&
                         !leader->IsWithinMeleeRange(stale))
                {
                    Leader::PullOrder order;
                    if (!Leader::Orders::GetOpen(group.LeaderGuid(), order) || order.target != staleGuid)
                        why = "an idle mob no pull order is for";
                }
            }
            else if (ObjectGuid const selected = leader->GetTarget();
                     !selected.IsEmpty() && !ObjectAccessor::GetUnit(*leader, selected))
            {
                staleGuid = selected;
                why = "gone";
            }
            if (why.empty())
            {
                route.staleVictim.Clear();
                route.staleVictimSinceMs = 0;
            }
            else if (route.staleVictim != staleGuid)
            {
                route.staleVictim = staleGuid;
                route.staleVictimSinceMs = now ? now : 1;
                ++route.staleVictimReleases;
                group.LogEvent("stale_idle_victim_release",
                               JsonObject().Str("name", stale ? stale->GetName() : "").Str("why", why)
                                   .Num("entry", stale ? stale->GetEntry() : 0)
                                   .Bool("boss", stale && stale->IsCreature() && stale->ToCreature()->IsDungeonBoss())
                                   .Real("distance", stale && stale->IsInWorld() && stale->GetMap() == leader->GetMap()
                                                         ? leader->GetExactDist(stale)
                                                         : -1.0f)
                                   .Real("leaderMana",
                                         leader->GetMaxPower(POWER_MANA) ? leader->GetPowerPct(POWER_MANA) : 100.0f)
                                   .Num("releases", route.staleVictimReleases));
                LOG_INFO("playerbots",
                         "[queue-stale-idle-victim] {}: {} held {} as its target outside a fight, the victim {} - "
                         "dropped (release {})",
                         group.Run().runId, leader->GetName(), stale ? stale->GetName() : staleGuid.ToString(), why,
                         route.staleVictimReleases);
                // [jrn40] the same event as a row. This emitter HAS a run id, unlike the bot-side detectors, so
                // the slice joins by identity here and not by nearness in time.
                {
                    Journal::Stuck stuck;
                    stuck.kind = "stale_victim";
                    stuck.botName = leader->GetName();
                    stuck.botLow = leader->GetGUID().GetCounter();
                    stuck.botLevel = leader->GetLevel();
                    stuck.role = "tank";  // the leader of a five holds the target; its role in this five
                    stuck.mapId = leader->GetMapId();
                    stuck.zoneId = leader->GetZoneId();
                    stuck.x = leader->GetPositionX();
                    stuck.y = leader->GetPositionY();
                    stuck.z = leader->GetPositionZ();
                    stuck.targetName = stale ? stale->GetName() : staleGuid.ToString();
                    stuck.targetLow = staleGuid.GetCounter();
                    stuck.targetEntry = staleGuid.IsCreature() ? staleGuid.GetEntry() : 0;
                    if (stale)
                        stuck.targetLevel = stale->GetLevel();
                    if (stale && stale->IsInWorld() && stale->GetMap() == leader->GetMap())
                        stuck.targetYd = leader->GetExactDist(stale);
                    stuck.inCombat = false;  // the guard above requires it: asked and answered, not a gap
                    stuck.why = "the leader held a target outside a fight: " + why;
                    stuck.runId = group.Run().runId;
                    uint64 const stuckRowId = Journal::RecordStuck(stuck);

                    Journal::Issue issue;
                    issue.kind = "stale_victim";
                    issue.severity = "mid";
                    issue.reproClass = "decision";
                    // the key is WHY it went stale, numbers out; the mob and the release count are what would fan
                    // one cause into hundreds
                    issue.dedupKey = "stale_victim|" + Journal::WithoutNumbers(why);
                    issue.why = stuck.why;
                    // [jrn43] AND THE REASON DECIDES WHETHER THERE IS A PROBLEM AT ALL. Three of the five reasons
                    // are the normal end of a fight: the victim is dead, its guid resolves to nothing, or it is
                    // off the leader's map. This function runs on the leader's own update and drops on the FIRST
                    // tick the facts change, once per victim, with no clock in it (that was the point of taking
                    // STALE_VICTIM_MS out), so in those three nothing ever waited on the stale target - the group
                    // had just killed the mob. Measured on b502: 178 rows `dead` over 116 distinct guids, 6
                    // leaders and 10 runs, 3 `gone`, 1 `an idle mob no pull order is for` - 181 of 182 of them the
                    // routine clearing after a kill, counted as 172 occurrences of a problem.
                    // The two that stay real: `evading` is a mob the group cannot reach, and `an idle mob no pull
                    // order is for` is a target held against the leader's order. The criterion is in the seam
                    // (Journal::Real::StaleVictim), where a test over the live reasons asserts it.
                    bool const realProblem = Journal::Real::StaleVictim(why);
                    issue.realWhen = "the leader's target is no part of any fight any more and the leader is out "
                                     "of combat, and the reason it went stale is not the normal end of a fight "
                                     "(dead / gone / off the map)";
                    issue.real = realProblem ? "yes" : "no";
                    issue.realWhy =
                        realProblem
                            ? "the target had to be dropped by this code for the group to move on at all"
                            : "the victim stopped being a victim in the normal course of the fight (" + why +
                                  "): this runs on the leader's own update and dropped it on the first tick it "
                                  "could, so nothing waited on it - the row keeps the release count it measured";
                    issue.realSource = "emitter";
                    issue.value = double(route.staleVictimReleases);
                    issue.valueUnit = "release";
                    issue.botName = stuck.botName;
                    issue.botLow = stuck.botLow;
                    issue.mapId = stuck.mapId;
                    issue.zoneId = stuck.zoneId;
                    issue.x = stuck.x;
                    issue.y = stuck.y;
                    issue.z = stuck.z;
                    issue.runId = stuck.runId;
                    issue.sourceTable = "pb_journal_stuck";
                    issue.sourceId = stuckRowId;
                    Journal::RecordIssue(issue);
                }
                leader->AttackStop();
                leader->SetTarget(ObjectGuid::Empty);
                if (leaderAI)
                {
                    leaderAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
                    leaderAI->ChangeEngine(BOT_STATE_NON_COMBAT);
                }
            }
        }
    }
}
