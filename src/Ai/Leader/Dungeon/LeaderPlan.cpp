/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderPlan.h"
#include "LeaderRest.h"  // [rest75] CountRestItems

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
#include "PartyMemberToHeal.h"  // [lead940] HealTriage
#include "PathGenerator.h"
#include "RouteLegs.h"  // [lead110] PackMobCheck
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "RouteSteer.h"   // [lead167] BossBroughtBy
#include "RouteTargets.h"  // [lead1084] EventWaveFresh
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
#include "LeaderPlannerShared.h"  // [lead369] the planner's pack rule
#include "PullActions.h"  // [b39_strat] FightHold: the opening fight held on the use side of a key gate
#include "ChooseTargetActions.h"  // [b47_door] PullReadiness::GroupPreparing
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"  // [lead940]
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
#include <cctype>  // [lead1390]
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "LeaderEvents.h"
#include "LeaderPrepull.h"  // [lead270]
#include "LeaderConsumables.h"  // [lead1090] BossBuffPassHolds, OpenBossBuffPass
#include "InterruptActions.h"   // [lead1390] LeaderOrderCheck

namespace Leader::Dungeon
{
    // [dun226] the patrol counts as at its beat's far end within this of it (a waypoint's arrival radius)
    constexpr float PATROL_BEAT_FAR_SLACK = 2.0f;
    using namespace Leader::Fight;
    using Leader::Travel::PointOnPath;

    namespace
    {
        // [lead221] the mana line a held answer names for `who` ("<who> at 46 % mana (needs 77 % = ...)",
        // "(needs 51 %)"), -1 when it names none (a health gap, "the tank rests")
        float NamedManaLine(std::string const& why, std::string const& who)
        {
            std::string::size_type const at = why.find(who + " at ");
            if (at == std::string::npos)
                return -1.0f;
            std::string::size_type const mana = why.find("% mana", at);
            std::string::size_type const needs = why.find("(needs ", at);
            if (mana == std::string::npos || needs == std::string::npos || needs < mana)
                return -1.0f;
            float const line = std::strtof(why.c_str() + needs + std::strlen("(needs "), nullptr);
            return line > 0.0f ? line : -1.0f;
        }

        // [lead1131] a warlock with no pet (NoPetTrigger's fact) that can cast a summon: Summon Imp takes no reagent,
        // Voidwalker / Succubus / Felhunter / Felguard a Soul Shard (Spell.dbc reagents)
        bool WarlockSummonsPet(Player* bot)
        {
            constexpr uint32 SPELL_SUMMON_IMP = 688;
            constexpr uint32 SPELL_SHARD_SUMMONS[] = {697, 712, 691, 30146};
            constexpr uint32 ITEM_SOUL_SHARD = 6265;
            if (!bot || bot->getClass() != CLASS_WARLOCK || !bot->IsAlive() || bot->IsMounted() ||
                !bot->GetMinionGUID().IsEmpty() || bot->GetGuardianPet() || bot->GetFirstControlled())
                return false;
            bool const shardSummon = std::any_of(std::begin(SPELL_SHARD_SUMMONS), std::end(SPELL_SHARD_SUMMONS),
                                                 [bot](uint32 spell) { return bot->HasSpell(spell); });
            return shardSummon ? bot->HasItemCount(ITEM_SOUL_SHARD, 1) : bot->HasSpell(SPELL_SUMMON_IMP);
        }
    }

    // [lead6] The one readiness judge: the group here (alive, in the instance, within LEADER_READY_RANGE, the
    // healer sees the leader), nobody buffing (needBuff), sitting, or preparing (PullReadiness::GroupPreparing). The
    // health and mana levels a pull needs stay the planner's (its danger-scaled topped-up rules: IsGroupToppedUp,
    // BossReadyGap); the harness's 80 % lines (GroupFullyReady) were the body pull's own gate, gone with it.
    std::string LeaderReadyWhy(LeaderGroup& group, Player* leader)
    {
        constexpr float LEADER_READY_RANGE = 40.0f;  // the planner's GATHER_HEALER_SIGHT_RANGE
        if (!leader || !leader->IsAlive())
            return "the leader is dead";
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || bot == leader)
                continue;
            if (!bot->IsAlive())
                return member.name + " is dead";
            if (bot->GetMap() != leader->GetMap())
                return member.name + " is not in the instance";
            // [lead17] LEADER_ONE_GATHER_OWNER: a member on a leader spot is placed (the spot gate judges it); one
            // the leader marks trailing is not ready; no distance rule of its own
            if (Leader::LEADER_ONE_GATHER_OWNER && group.LeaderDecides())
            {
                if (group.State().trailing.count(member.guid))
                    return member.name + " trails the group";
                if (bot->IsSitState() && !MemberFull(bot))
                    return member.name + " sits";
                // [lead1131] b238 rfc-h 5H-8 478.4 s: the pull was committed as the fight ended, the fire mage still
                // in combat at 14 % mana; 0.2 s later its combat dropped, the leader's Rest (its own line, 43 %) sat
                // it down and the tank walked the engage 60 yd away from it. A member still in combat below its own
                // rest line (the Rest's rule, RestState::BelowOwnLine) rests as soon as its combat drops: not ready
                if (bot->IsInCombat() && !MemberFull(bot) && RestState::BelowOwnLine(bot))
                    return member.name + " rests below its line [lead1131]";
                // [lead1453] b266 rfc-h 5D-12 868.4-868.9 s: the leader's Rest put the arcane mage (1.7 % mana, its
                // line 34 % for the next 5 mobs) down, and 0.5 s later - before it sat - the planner committed the
                // engage; its Rest ended at 1.8 % and it wanded the fight (b266: shadow priest 71 s, fire mage 59 s,
                // arcane 46 s of wand a run). The leader's Rest is its own decision by the game's math (the line:
                // the next fight's mana at the member's rate against its drink): a member under it, out of combat
                // and still below its line, rests - not ready until it stands at its line
                // [rest75] ... only while it has something to rest with: a member with no drink (a mana user) and no
                // food sits on spirit regen alone, which the regen judge does not wait for ("no drink in the bags,
                // spirit regen only - not waited for"). b406 rfc-n five 4: Feette (BM hunter, 48.7 % mana, 0 drinks)
                // held the Taragaman pull 18 times "rests below its line" while the regen judge let it go - the run
                // stalled at 1/4
                bool const hasRest = (bot->GetMaxPower(POWER_MANA) && Leader::Dungeon::CountRestItems(bot, 59)) ||
                                     Leader::Dungeon::CountRestItems(bot, 11);
                if (!bot->IsInCombat() && RestState::LeaderRests(bot) && !MemberFull(bot) &&
                    RestState::BelowOwnLine(bot) && hasRest)
                    return member.name + " rests below its line [lead1453]";
                // [lead1131] b238 5D-4 414.6 s / 5D-12 374.5 s / 5E-13 411.6 s: the warlock's pet died in the fight,
                // the engage went, the warlock stood casting its summon (691 / 688, its "no pet" trigger) and the
                // tank walked 60-85 yd away from it. A warlock without a pet that can summon one (Summon Imp needs
                // no reagent, the others a Soul Shard: Spell.dbc reagents) summons it before it follows
                if (WarlockSummonsPet(bot))
                    return member.name + " summons its pet [lead1131]";
                continue;
            }
            // a far member the far wait gave up on (far_wait_dropped) holds no readiness while far, never a healer
            bool const dropped = member.apartIgnoreUntilMs && int32(getMSTime() - member.apartIgnoreUntilMs) < 0 &&
                                 !PlayerbotAI::IsHeal(bot);
            if (!dropped && bot->GetDistance(leader) > LEADER_READY_RANGE)
                return Acore::StringFormat("{} is {:.0f} yd from the leader", member.name, bot->GetDistance(leader));
            if (PlayerbotAI::IsHeal(bot) && !bot->IsWithinLOSInMap(leader))
                return "the healer " + member.name + " does not see the leader";
            if (bot->IsSitState())
                return member.name + " sits";
        }
        if (group.NeedBuff())
            return "the group buffs";
        // [lead1511] b272 ac-h 5F-14 442.6-448 s: Shirrak died, the rogue stood at 7 % health with his Carnivorous
        // Bite bleeding on it; the leader's TopUp named it (healHolds: the pull waits on its health, lead1263) and
        // no Rest sat it down (a free healer heals it, a damage over time holds its Rest: lead1491), so nothing held
        // the pull - 1 s later the engage went, the rogue walked to the tank's stop point, the healer followed the
        // tank and the rogue bled out. A member the healer's TopUp holds for, below its own health line
        // (RestState::HealthLine), is not ready until the heal brings it there
        if (!leader->IsInCombat())
            for (LeaderGroup::MemberInfo const& member : group.Members())
            {
                Player* healer = ObjectAccessor::FindPlayer(member.guid);
                Leader::MemberEvents events;
                if (!healer || !healer->IsAlive() || !PlayerbotAI::IsHeal(healer) ||
                    !Leader::Events::Get(member.guid, events) || events.healTarget.IsEmpty() || !events.healHolds)
                    continue;
                Player* hurt = ObjectAccessor::FindPlayer(events.healTarget);
                if (hurt && hurt->IsAlive() && !hurt->IsInCombat() &&
                    hurt->GetHealthPct() < RestState::HealthLine(hurt))
                    return Acore::StringFormat("{} at {:.0f} % health < its line {:.0f} %: {} tops it up [lead1511]",
                                               hurt->GetName(), hurt->GetHealthPct(), RestState::HealthLine(hurt),
                                               member.name);
            }
        // [lead1084] [lead1090] the boss pull's wait for his buff pass is no group readiness: it holds only the pull
        // of that boss or a pull that brings him (TickLeaderPlanner's bossPassHold, BossBuffPassHolds)
        // [lead940] a heal over time another member cast out of the last fight (a top-up with no direct heal to cast)
        // still runs: its ticks would forward threat to its caster from the pull's first second (ThreatManager::
        // ForwardThreatForAssistingMe over every mob with the healed member on its threat list) - the pull waits for
        // its end (game data: the aura's duration). One cast in the last fight is the fight's heal, not held
        if (!leader->IsInCombat())
        {
            uint32 const now = getMSTime() ? getMSTime() : 1;
            uint32 const peace = group.State().peaceSinceMs;
            for (LeaderGroup::MemberInfo const& member : group.Members())
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || !bot->IsAlive() || bot->IsInCombat())
                    continue;
                // [lead981] user 2026-11-03 (warlock-detached): the pre-pull heal-over-time care is the TANK's. A
                // tick forwards its threat over the mobs that list the healed member; a damage dealer is listed by
                // no mob before it attacks, and it attacks only on the tank's threat (the leader's dps hold) - its
                // heal over time takes nothing off the tank. The tank is listed by every mob it pulls from its
                // first hit, at its lowest threat. b228 (16 runs): 175 s of pull holds on Renew / Rejuvenation
                // ticking on damage dealers (5C-3 Rtdruab, 5F-6 Rtlocac / Rtrogac / Rthunac, 5E-5 Rthunab / Rtrogab)
                if (bot != leader && !PlayerbotAI::IsTank(bot))
                    continue;
                for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_PERIODIC_HEAL))
                {
                    Aura const* aura = effect->GetBase();
                    if (aura->GetCasterGUID() == bot->GetGUID() || !aura->GetCasterGUID().IsPlayer() ||
                        aura->GetDuration() <= 0)
                        continue;
                    int32 const ranMs = aura->GetMaxDuration() - aura->GetDuration();
                    if (peace && ranMs >= int32(getMSTimeDiff(peace, now)))
                        continue;  // cast in the fight
                    if (!HealTriage::IsThreatHeal(aura->GetSpellInfo()))
                        continue;
                    // no seconds in the text: the verdict changes on a fact, not on each tick of its clock
                    return Acore::StringFormat("[lead940] {} has {} ticking (an out-of-fight heal over time)",
                                               member.name, aura->GetSpellInfo()->SpellName[0]);
                }
            }
        }
        if (MagistersTerrace::SelinRoom::EntryHolds(leader))
            return "a member is outside Selin's room";
        std::string preparing;
        if (PullReadiness::GroupPreparing(leader, &preparing))
            return preparing.empty() ? std::string("the group prepares") : preparing;
        return std::string();
    }

    void LeaderWholePack(LeaderGroup& group, Player* leader, Creature* pack, std::string const& why,
                               bool walkIn, bool required)
    {
        if (!pack || !leader)
            return;
        uint32 const now = getMSTime() ? getMSTime() : 1;
        group.ClearWaypoint(group.LeaderGuid());
        // the tank's pull order runs: the leader's decision is being executed, no new ask meanwhile
        if (Leader::Orders::IsOpen(group.LeaderGuid()))
            return;
        auto& lead = group.State();
        bool const fresh = lead.wholePack != pack->GetGUID() || !lead.wholeSeenMs ||
                           getMSTimeDiff(lead.wholeSeenMs, now) > Leader::LEADER_REQUEST_TTL_MS;
        if (fresh)
        {
            lead.wholePack = pack->GetGUID();
            lead.wholeSinceMs = now;
            LOG_INFO("playerbots", "[lead3] leader_whole_pack {}: {} {} ({:.1f} yd): the planner takes it whole{} ({})",
                     group.Run().runId, pack->GetName(), pack->GetEntry(), leader->GetExactDist(pack),
                     walkIn ? ", walked in if it has no ranged pull" : "", why);
            group.LogEvent("leader_whole_pack",
                     JsonObject().Str("name", pack->GetName()).Num("entry", pack->GetEntry()).Str("why", why)
                         .Bool("walkIn", walkIn).Bool("required", required)
                         .Real("distance", leader->GetExactDist(pack)));
        }
        lead.wholeSeenMs = now;
        lead.walkIn = walkIn;
        lead.required = required;
        lead.wholeWhy = why;
        if (required)
            group.MarkRequired(pack->GetGUID());
        group.ResetSteerClock(now);  // the leader's declared wait is no lack of progress
        group.ClearWaypoint(group.LeaderGuid());
    }

    void LeaderBossPull(LeaderGroup& group, Player* leader, Creature* boss, std::string const& why)
    {
        if (!boss || !leader)
            return;
        uint32 const now = getMSTime() ? getMSTime() : 1;
        group.ClearWaypoint(group.LeaderGuid());
        if (Leader::Orders::IsOpen(group.LeaderGuid()))  // the tank's pull order runs
            return;
        auto& lead = group.State();
        // [lead45] logged once per boss (it was every second: "waited 38 ... 42 s")
        if (lead.bossPullLoggedFor != boss->GetGUID())
            group.LogEvent("leader_boss_pull",
                     JsonObject().Str("name", boss->GetName()).Num("entry", boss->GetEntry()).Str("why", why)
                         .Real("distance", leader->GetExactDist(boss)));
        lead.bossPullLoggedFor = boss->GetGUID();
        lead.bossPull = boss->GetGUID();
        lead.bossPullMs = now;
        lead.bossPullWhy = why;
        group.ResetSteerClock(now);
        group.ClearWaypoint(group.LeaderGuid());
    }

    bool LeaderNoOption(LeaderGroup& group, Player* leader, Unit* mob, std::vector<ObjectGuid> const& pack,
                              std::string const& why, bool stops, JsonObject fields)
    {
        if (!mob || !leader)
            return false;
        auto& lead = group.State();
        if (lead.noOption.count(mob->GetGUID()) ||
            std::any_of(pack.begin(), pack.end(),
                        [&lead](ObjectGuid const& guid) { return lead.noOption.count(guid) != 0; }))
            return false;
        lead.noOption.insert(mob->GetGUID());
        lead.noOption.insert(pack.begin(), pack.end());
        bool const stopping = stops && Leader::LEADER_STOP_ON_NO_OPTION;
        fields.Str("name", mob->GetName()).Num("entry", mob->GetEntry()).Num("pack", int64(pack.size()))
            .Str("why", why).Bool("stops", stopping)
            .Real("tankX", leader->GetPositionX()).Real("tankY", leader->GetPositionY())
            .Real("tankZ", leader->GetPositionZ()).Real("mobX", mob->GetPositionX())
            .Real("mobY", mob->GetPositionY()).Real("mobZ", mob->GetPositionZ());
        LOG_INFO("playerbots", "[lead3] planner_no_option {}: {} {} ({} mobs){}: {}", group.Run().runId,
                 leader->GetName(), mob->GetName(), pack.size(), stopping ? ", the run stops" : "", why);
        Decisions().Write(group.Run(), "leader", "planner_no_option",
                                      mob->GetGUID(), mob->GetName(), true, fields);
        group.LogEvent("planner_no_option", fields);
        if (stopping)
            group.FailRun("planner_no_option: " + mob->GetName() + ": " + why);
        return true;
    }

    // [lead541] The boss a pull is for: its mob, or a boss the plan's pack counts (a trash pull that "brings" him,
    // LeaderPlanner's drawn boss). b212 rfc-h 5C 1008 s: the Cultist's pull brought Jergosh the Invoker and the
    // Army of the Dead prepull was never ordered - the prepull hold looked only at the pull's own mob.
    Creature* PullBoss(Map* map, Unit* mob, PullPlanner::Decision const& decision)
    {
        auto const isBoss = [](Creature const* creature)
        { return creature && creature->IsAlive() && (creature->IsDungeonBoss() || creature->isWorldBoss()); };
        if (Creature* creature = mob ? mob->ToCreature() : nullptr; isBoss(creature))
            return creature;
        if (!map)
            return nullptr;
        for (GuidVector const* list : {&decision.pack, &decision.verdictPack, &decision.losPack})
            for (ObjectGuid const& guid : *list)
                if (Creature* creature = map->GetCreature(guid); isBoss(creature))
                    return creature;
        return nullptr;
    }

    // [lead15] the spots of the pull `decision` (healer near enough to heal the FIGHT spot, ranged 22 yd behind it,
    // melee just behind it, all away from the pulled pack); false: the pull needs no gate (no fight spot / pack).
    bool LeaderAssignSpots(LeaderGroup& group, Player* tank, PullPlanner::Decision const& decision)
    {
        auto& lead = group.State();
        ObjectGuid const mobGuid = !decision.planTarget.IsEmpty() ? decision.planTarget : decision.attack;
        Unit* mob = mobGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, mobGuid);
        if (!mob || decision.los || decision.kind == "approach" || decision.kind == "side walk")
            return false;
        // [lead17] slabs-h 20261026-184720: spots around a patrol (an engage: the tank walks in and meets it
        // elsewhere) stood 30-45 yd from where the tank fought; no gate for a walk-in or a moving mob - the members
        // walk along with the tank
        if (decision.kind == "engage" || mob->isMoving() ||
            (mob->ToCreature() && mob->ToCreature()->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE))
            return false;
        Position fight = mob->GetPosition();
        if (!decision.killFor.IsEmpty() && decision.killFor == decision.planTarget)
            fight = decision.kill;
        else if (!decision.fightFor.IsEmpty() && decision.fightFor == decision.planTarget)
            fight = decision.fight;
        else if (decision.pullBack || decision.single)
            fight = tank->GetPosition();
        // "behind": away from the pack, along mob -> fight spot, else mob -> tank
        float dx = fight.GetPositionX() - mob->GetPositionX();
        float dy = fight.GetPositionY() - mob->GetPositionY();
        if (std::hypot(dx, dy) < 3.0f)
        {
            dx = tank->GetPositionX() - mob->GetPositionX();
            dy = tank->GetPositionY() - mob->GetPositionY();
        }
        float const length = std::hypot(dx, dy);
        if (length < 1.0f)
            return false;
        float const base = std::atan2(dy, dx);
        std::vector<ObjectGuid> pack = decision.pack;
        pack.push_back(mob->GetGUID());
        if (lead.spotsFor != mob->GetGUID())
        {
            lead.spots.clear();
            lead.spotBest.clear();
            lead.spotStuck.clear();
            lead.spotReassigns.clear();
            lead.spotRefused.clear();  // [lead930]
            lead.spotsFor = mob->GetGUID();
            lead.spotsSinceMs = getMSTime();
        }
        lead.spotsFight = fight;
        bool assigned = false;
        // [lead18] the idle mobs around the pull (the spot guard), the group's walked ground
        std::list<Creature*> idleNear;
        {
            IdleAggroCheck check{tank};
            Acore::CreatureListSearcher<IdleAggroCheck> searcher(tank, idleNear, check);
            Cell::VisitObjects(tank, searcher, LEADER_IDLE_SCAN_YD);
        }
        std::vector<Creature*> const idleVector(idleNear.begin(), idleNear.end());
        Position const idleAt = tank->GetPosition();  // [lead1121]
        g_leaderIdle = &idleVector;
        g_leaderIdleAt = &idleAt;
        g_leaderCleared = &lead.clearedGround;
        std::vector<ObjectGuid> pullMobs = LeaderPullMobs(lead.commit, lead.commitMob);
        pullMobs.insert(pullMobs.end(), pack.begin(), pack.end());
        g_leaderExempt = &pullMobs;
        // [lead464] a ranged member casts at the fight place from `at`: in its spell reach and in sight of it (its eye
        // to the mob's middle where the mob will stand)
        auto const castsAtFight = [&](Player* bot, Position const& at)
        {
            return at.GetExactDist(fight) <= LeaderSpellReach(bot) &&
                   tank->GetMap()->isInLineOfSight(at.GetPositionX(), at.GetPositionY(),
                                                   at.GetPositionZ() + bot->GetCollisionHeight(),
                                                   fight.GetPositionX(), fight.GetPositionY(),
                                                   fight.GetPositionZ() + mob->GetCollisionHeight() / 2.0f,
                                                   tank->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                   VMAP::ModelIgnoreFlags::Nothing);
        };
        uint32 index = 0;
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || bot == tank || !bot->IsAlive() || bot->GetMap() != tank->GetMap())
                continue;
            if (lead.spots.count(member.guid))
                continue;  // kept while the pull stands (no reshuffle)
            bool const healer = PlayerbotAI::IsHeal(bot);
            bool const ranged = !healer && PlayerbotAI::IsRanged(bot);
            float const distance = healer ? 18.0f : ranged ? 22.0f : 4.0f;
            float const spread = float(int32(index % 3) - 1) * (healer || ranged ? 0.35f : 0.6f);
            ++index;
            std::optional<Position> newGround;  // [lead18] new ground, when no cleared spot fits
            static constexpr float SHRINK[] = {1.0f, 0.75f, 0.55f};
            static constexpr float TURN[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
            bool found = false;
            // [lead24d] user: «они уже стояли на безопасной точке… танк забегает в зону агро или подзывает моба»: before
            // the pull nobody gets a spot search - every member stays where it stands (cleared ground) and is ready;
            // melee never get a "behind the target" spot pre-pull (rfc-h-5A-1: 60 of 73 rejections were the melee
            // shaman's). The healer stays when it reaches and sees the fight spot, else it re-spots after the tank
            // holds the mobs ([lead20])
            if (Leader::LEADER_PREPULL_STAY)
            {
                Position const here = bot->GetPosition();
                // [lead61] user: «хилу не обязательно бегать» - the healer stays when it reaches and sees the fight
                // spot (lead53's placement by the tank is gone); else the leader re-spots it after the tank holds
                // [lead464] b205 rfc-h 5F Oggleflint: the warlock and the hunter "stayed" for the pull 30 yd behind
                // the fight and 15 yd down the slope (-180,3,-54.7) / (-182.8,4.2,-56.1), out of sight of it, and
                // never cast. A ranged member stays when it reaches and sees the fight place from where it stands;
                // else it gets a spot of the search below (in range and sight of the fight place, out of every
                // reach)
                bool const rangedSees = !ranged || castsAtFight(bot, here);
                if (!healer && rangedSees)
                {
                    lead.spots[member.guid] = here;
                    continue;
                }
                if (healer)
                {
                    if (HealReaches(tank, here, fight, bot) &&
                        here.GetExactDist(fight) <= Leader::LEADER_HEALER_TANK_YD)
                        lead.spots[member.guid] = here;
                    continue;
                }
            }
            // [lead20] user: a member whose position already fits does not move (the healer: it reaches and sees the
            // fight spot within LEADER_HEALER_TANK_YD; a ranged member: in range of it; melee: anywhere safe)
            {
                Position const here = bot->GetPosition();
                bool const fits = healer ? (HealReaches(tank, here, fight, bot) &&
                                            here.GetExactDist(fight) <= Leader::LEADER_HEALER_TANK_YD)
                                         : (!ranged || castsAtFight(bot, here));  // [lead464]
                if (fits && !LeaderAggroAt(bot, here))
                {
                    lead.spots[member.guid] = here;
                    continue;
                }
                // [lead20] the healer never holds the pull: no fitting spot now - it re-spots after the pull, once the
                // tank holds the mobs (the fight's heal-reach invariant)
                if (healer)
                    continue;
            }
            for (float shrink : SHRINK)
            {
                for (float turn : TURN)
                {
                    float const angle = base + spread + turn;
                    Position spot(fight.GetPositionX() + std::cos(angle) * distance * shrink,
                                  fight.GetPositionY() + std::sin(angle) * distance * shrink, fight.GetPositionZ());
                    // [lead18] a member stands out of every unpulled mob's reach, the pulled pack's too, and walks
                    // there without passing one
                    // [lead930] b226 rfc-h 5H 259.2-491 s: Rthunad's pull spot passed this check and the issue's own
                    // guard (LeaderAggroOnWalk, the aggro margin, the pull's mobs idle for a non-tank) refused its walk
                    // ("its spot for the pull: stays (the spot or its walk is in the reach of Ragefire Shaman)"); the
                    // gate waited 230 s for a spot the hunter was never sent to. A spot the leader assigns passes the
                    // guard it is issued under, and a spot that guard refused before is no candidate
                    bool guardNoWalk = false;
                    auto const refused = lead.spotRefused.find(member.guid);
                    bool const refusedBefore =
                        refused != lead.spotRefused.end() &&
                        std::any_of(refused->second.begin(), refused->second.end(),
                                    [&spot](Position const& at) { return at.GetExactDist2d(&spot) <= 1.0f; });
                    if (refusedBefore || !LeaderSpotOk(tank, fight, spot, pullMobs) ||
                        !LeaderMemberWalks(bot, spot, idleNear, pullMobs) ||
                        LeaderAggroOnWalk(bot, spot, guardNoWalk) || guardNoWalk)
                    {
                        if (Creature* mob = LeaderAggroAt(bot, spot))
                            Decisions().Write(
                                group.Run(), "leader", "spot_rejected_aggro",
                                member.guid, member.name, false,
                                JsonObject().Str("member", member.name).Str("mob", mob->GetName())
                                    .Real("distance", mob->GetExactDist(spot)).Str("spot", "pull"));
                        continue;
                    }
                    if (Leader::LEADER_PREFER_CLEARED && !LeaderOnCleared(spot))
                    {
                        if (!newGround)
                            newGround = spot;
                        continue;
                    }
                    lead.spots[member.guid] = spot;
                    assigned = true;
                    found = true;
                    break;
                }
                if (found)
                    break;
            }
            // [lead20] user: «где он стоял - там тоже можно стоять»: with no cleared candidate, a member (not the
            // healer) stays where it stands when it is safe there - ready, no new ground walk before the pull
            if (!found && !healer && !LeaderAggroAt(bot, bot->GetPosition()) &&
                (!ranged || castsAtFight(bot, bot->GetPosition())))  // [lead464]
            {
                lead.spots[member.guid] = bot->GetPosition();
                found = true;
                Decisions().Write(group.Run(), "leader",
                                              "spot_current", member.guid, member.name, true,
                                              JsonObject().Str("member", member.name)
                                                  .Str("why", "no cleared spot: it stays where it stands"));
            }
            if (!found && newGround)
            {
                lead.spots[member.guid] = *newGround;
                assigned = true;
                found = true;
            }
            // [lead20] no candidate passes: where the member stands is its spot when it is safe there and fits its
            // role (the healer reaches the fight spot, a ranged member is in range of it); else it is not waited for
            // (spot_unfit) - the gate never holds on a member without a spot
            if (!found)
            {
                Position const here = bot->GetPosition();
                Creature* mob = LeaderAggroAt(bot, here);
                bool const fits = healer ? HealReaches(tank, here, fight, bot)
                                         : (!ranged || castsAtFight(bot, here));  // [lead464]
                if (!mob && fits)
                    lead.spots[member.guid] = here;
                else
                    lead.spotStuck.insert(member.guid);
                Decisions().Write(group.Run(), "leader",
                                              !mob && fits ? "spot_current" : "spot_unfit", member.guid, member.name,
                                              true,
                                              JsonObject().Str("member", member.name)
                                                  .Str("why", mob ? "an unpulled mob reaches where it stands: " +
                                                                        mob->GetName()
                                                           : fits ? "no candidate passes: it stays where it is"
                                                                  : "where it stands does not fit its role"));
            }
        }
        // [lead15] LEADER_HEAL_REACH: the healer's spot reaches the fight spot (the tank) and every other spot, else
        // the healer gets another spot that does (near the group's middle), else each gap is a heal_reach record
        Player* healer = nullptr;
        for (LeaderGroup::MemberInfo const& member : group.Members())
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                bot && bot != tank && bot->IsAlive() && PlayerbotAI::IsHeal(bot) && lead.spots.count(member.guid))
            {
                healer = bot;
                break;
            }
        if (Leader::LEADER_HEAL_REACH && assigned && healer)
        {
            auto const keeps = [&](Position const& from, std::vector<ObjectGuid>* out)
            {
                bool all = HealReaches(tank, from, fight, healer);
                if (!all && out)
                    out->push_back(tank->GetGUID());
                for (auto const& [guid, spot] : lead.spots)
                    if (guid != healer->GetGUID() && !HealReaches(tank, from, spot, healer))
                    {
                        all = false;
                        if (out)
                            out->push_back(guid);
                    }
                return all;
            };
            std::vector<ObjectGuid> gaps;
            if (!keeps(lead.spots[healer->GetGUID()], &gaps))
            {
                float cx = fight.GetPositionX();
                float cy = fight.GetPositionY();
                for (auto const& [guid, spot] : lead.spots)
                {
                    cx += spot.GetPositionX();
                    cy += spot.GetPositionY();
                }
                cx /= float(lead.spots.size() + 1);
                cy /= float(lead.spots.size() + 1);
                bool fixed = false;
                static constexpr float RINGS[] = {0.0f, 4.0f, 8.0f, 12.0f, 16.0f};
                for (float ring : RINGS)
                {
                    for (uint32 i = 0; i < (ring > 0.0f ? 16u : 1u) && !fixed; ++i)
                    {
                        float const angle = 2.0f * float(M_PI) * float(i) / 16.0f;
                        Position spot(cx + std::cos(angle) * ring, cy + std::sin(angle) * ring, fight.GetPositionZ());
                        if (spot.GetExactDist2d(fight.GetPositionX(), fight.GetPositionY()) < 10.0f ||
                            !LeaderSpotOk(tank, fight, spot, pack) || !keeps(spot, nullptr))
                            continue;
                        lead.spots[healer->GetGUID()] = spot;
                        fixed = true;
                    }
                    if (fixed)
                        break;
                }
                for (ObjectGuid const guid : gaps)
                    if (Player* other = ObjectAccessor::FindPlayer(guid))
                        LogHealReach(group, other, healer, "the healer's pull spot does not reach its spot",
                                     fixed ? "the healer gets another spot" : "none");
            }
        }
        g_leaderIdle = nullptr;
        g_leaderIdleAt = nullptr;
        g_leaderCleared = nullptr;
        g_leaderExempt = nullptr;
        return !lead.spots.empty();
    }

    // [lead15] every member on its spot (within LEADER_SPOT_TOLERANCE), nobody sitting, the healer in heal range and
    // sight of the fight spot; a member not nearer for LEADER_SPOT_STUCK_MS is reported once and no longer waited for
    std::string LeaderSpotsWait(LeaderGroup& group, Player* tank)
    {
        auto& lead = group.State();
        uint32 const now = getMSTime() ? getMSTime() : 1;
        std::string waitFor;
        for (LeaderGroup::MemberInfo const& member : group.Members())
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || bot == tank || !bot->IsAlive())
                continue;
            if (bot->IsSitState() && !MemberFull(bot))  // [lead22] a full sitter holds nothing
            {
                waitFor += (waitFor.empty() ? "" : ", ") + member.name + " sits";
                continue;
            }
            auto const spot = lead.spots.find(member.guid);
            if (spot == lead.spots.end() || lead.spotStuck.count(member.guid))
                continue;
            float const gap = bot->GetExactDist2d(spot->second.GetPositionX(), spot->second.GetPositionY());
            if (gap <= Leader::LEADER_SPOT_TOLERANCE)
                continue;
            auto& best = lead.spotBest[member.guid];
            if (!best.second || gap < best.first - 0.5f)
                best = {gap, now};
            else if (Leader::LEADER_NO_FORCING_TIMERS &&
                     getMSTimeDiff(best.second, now) >= Leader::LEADER_SPOT_STUCK_MS)
                WaitedLong(group, "member_to_spot:" + member.name, getMSTimeDiff(best.second, now),
                           Acore::StringFormat("{:.0f} yd from its spot", gap));
            else if (getMSTimeDiff(best.second, now) >= Leader::LEADER_SPOT_STUCK_MS &&
                     lead.spotReassigns[member.guid] < Leader::LEADER_SPOT_REASSIGNS)
            {
                // [lead17] the leader gives a stuck member a new spot (the next LeaderAssignSpots)
                ++lead.spotReassigns[member.guid];
                group.LogEvent("leader_spot_reassign",
                         JsonObject().Str("member", member.name).Real("gap", gap)
                             .Num("reassign", lead.spotReassigns[member.guid]));
                lead.spots.erase(member.guid);
                lead.spotBest.erase(member.guid);
                waitFor += (waitFor.empty() ? "" : ", ") + member.name + " gets a new spot";
                continue;
            }
            else if (getMSTimeDiff(best.second, now) >= Leader::LEADER_SPOT_STUCK_MS)
            {
                lead.spotStuck.insert(member.guid);
                group.LogEvent("member_stuck",
                         JsonObject().Str("member", member.name).Real("gap", gap)
                             .Real("x", spot->second.GetPositionX()).Real("y", spot->second.GetPositionY())
                             .Real("z", spot->second.GetPositionZ()).Str("why", "not nearer its spot for 15 s"));
                continue;
            }
            waitFor += (waitFor.empty() ? "" : ", ") + Acore::StringFormat("{} {:.0f} yd", member.name, gap);
        }
        // [lead20] LEADER_SPOT_GATE_STUCK_MS: no progress (the same wait, nobody sitting) this long - spot_gate_stuck,
        // everyone counts placed where they stand
        bool const sitting = waitFor.find(" sits") != std::string::npos;
        if (waitFor.empty() || sitting || waitFor != lead.spotGateLast || !lead.spotGateProgressMs)
        {
            lead.spotGateProgressMs = now;
            lead.spotGateLast = waitFor;
        }
        else if (Leader::LEADER_NO_FORCING_TIMERS &&
                 getMSTimeDiff(lead.spotGateProgressMs, now) >= Leader::LEADER_SPOT_GATE_STUCK_MS)
            WaitedLong(group, "spot_gate", getMSTimeDiff(lead.spotGateProgressMs, now), waitFor);
        else if (getMSTimeDiff(lead.spotGateProgressMs, now) >= Leader::LEADER_SPOT_GATE_STUCK_MS)
        {
            group.LogEvent("spot_gate_stuck", JsonObject().Str("waitFor", waitFor)
                                                   .Num("sec", getMSTimeDiff(lead.spotGateProgressMs, now) / 1000));
            for (LeaderGroup::MemberInfo const& member : group.Members())
                if (lead.spots.count(member.guid))
                    lead.spotStuck.insert(member.guid);
            lead.spotGateProgressMs = now;
            return std::string();
        }
        return waitFor;
    }

    void WaitedLong(LeaderGroup& group, std::string const& kind, uint32 ms, std::string const& why)
    {
        static std::unordered_map<std::string, uint32> logged;  // world thread
        uint32 const now = getMSTime() ? getMSTime() : 1;
        uint32& last = logged[std::to_string(group.Run().runSeq) + "|" + kind];
        if (last && getMSTimeDiff(last, now) < 30 * IN_MILLISECONDS)
            return;
        last = now;
        group.LogEvent("waited_long", JsonObject().Str("kind", kind).Num("sec", ms / IN_MILLISECONDS).Str("why", why));
    }

    void CheckLeaderInvariants(LeaderGroup& group, Player* tank)
    {
        if (!tank || !tank->IsInWorld())
            return;

        // world thread: the state last logged per run, member and kind (empty: none)
        static std::unordered_map<std::string, std::string> logged;
        DecisionRun const run = group.Run();
        auto const note = [&run](LeaderGroup::MemberInfo const& member, char const* kind,
                                 std::string const& state, std::string const& why)
        {
            std::string& last = logged[Acore::StringFormat("{}|{}|{}", run.runSeq, member.guid.GetRawValue(), kind)];
            if (state == last)
                return;
            last = state;
            if (state.empty())
                return;
            LOG_INFO("playerbots", "[lead1390] invariant_violation {} {} {}: {}", run.runId, kind, member.name, why);
            Decisions().Write(run, "leader", "invariant_violation", ObjectGuid::Empty, std::string(), false,
                              JsonObject().Str("kind", kind).Str("member", member.name).Str("why", why)
                                  .Str("tag", "[lead1390]"));
        };

        // [lead1482] b268ac ac-h: 55 of the 56 "cc ... on ...: OUT_OF_RANGE / NOT_SHAPESHIFT / LINE_OF_SIGHT"
        // order_impossible lines named an order the leader issued on an earlier tick: this check runs on every world
        // update, the leader's own pass (TickLeaderEvents, its lead1462 re-check) only every LEADER_FIGHT_TICK_MS -
        // a held mob that wandered off its caster's reach in between was a violation before the leader looked
        // again. The orders are judged as the leader's last pass left them: once per pass
        {
            static std::unordered_map<uint64, uint32> judgedPass;  // world thread: run -> the leader pass judged
            uint32 pass = 0;
            for (uint32 ticks : group.State().cadenceTicks)
                pass += ticks;
            uint32& judged = judgedPass[uint64(group.Run().runSeq)];
            if (judged == pass)
                return;
            judged = pass;
        }
        std::vector<LeaderGroup::MemberInfo> const members = group.Members();
        std::vector<Leader::MemberEvents> events(members.size());
        std::vector<bool> hasEvents(members.size(), false);
        for (size_t i = 0; i < members.size(); ++i)
            hasEvents[i] = Leader::Events::Get(members[i].guid, events[i]);

        // wait_without_order: the pull waits on a member (the readiness verdict names it, out of a fight) that no
        // order moves or readies - its own (a walk, Rest / Prepare / Ready?, a cast, a use, a heal) or another
        // member's on it (a resurrection, a soulstone, a dispel, a heal)
        auto const& lead = group.State();
        std::string const& waitWhy = lead.notReadyWhy;
        bool const waits = !waitWhy.empty() && !group.InCombat() && !tank->IsInCombat();
        size_t waitedAt = std::string::npos;
        size_t waited = members.size();
        if (waits)
            for (size_t i = 0; i < members.size(); ++i)
            {
                std::string const& name = members[i].name;
                for (size_t at = waitWhy.find(name); !name.empty() && at != std::string::npos;
                     at = waitWhy.find(name, at + 1))
                {
                    size_t const end = at + name.size();
                    if (end < waitWhy.size() && std::isalnum(static_cast<unsigned char>(waitWhy[end])))
                        continue;
                    if (at < waitedAt)
                    {
                        waitedAt = at;
                        waited = i;
                    }
                    break;
                }
            }
        bool orderFor = false;
        if (waited < members.size())
        {
            ObjectGuid const guid = members[waited].guid;
            if (hasEvents[waited])
            {
                Leader::MemberEvents const& own = events[waited];
                orderFor = own.move == Leader::MoveEvent::MoveTo || own.move == Leader::MoveEvent::StandAt ||
                           own.move == Leader::MoveEvent::Follow || own.ready != Leader::ReadyEvent::None ||
                           own.externalSpell || own.dispelSpell || own.consumeItem || !own.consumeObject.IsEmpty() ||
                           !own.healTarget.IsEmpty();
            }
            for (size_t i = 0; i < members.size() && !orderFor; ++i)
                orderFor = hasEvents[i] && i != waited &&
                           (events[i].externalTarget == guid || events[i].dispelTarget == guid ||
                            events[i].consumeTarget == guid || events[i].healTarget == guid);
            // [lead1412] b262 wave 1 (60+ lines): "[lead940] X has Rejuvenation ticking": the wait for an
            // out-of-fight heal over time on the tank is right (its ticks after the pull heal the damage the tank
            // takes and forward 0.5 x that heal as the healer's threat, AuraEffect::HandlePeriodicHealAurasTick,
            // past the leader's in-fight Heal gate) and it ends by itself: the aura's end readies the member, no
            // order is missing
            // [dun221] the healer's TopUp holds the pull for the hurt member ([lead1511]): the healer's heal order
            // readies it (b442: 9 wait_without_order "X at N % health < its line: Y tops it up")
            if (!orderFor && waitWhy.find("[lead1511]") != std::string::npos)
                orderFor = true;
            // [dun222] the wait's member is under the leader's verdict or on its own ordered way back: a dead member
            // whose resurrection the leader judged ([lead1367]: a resurrecter comes - it drinks or its fight ends -
            // or none can, and it releases and runs back, AutoReleaseSpiritAction reading that verdict); a member
            // out of the instance or trailing that walks to the group (its ghost run / portal walk / follow, the
            // walk ending nearer the leader); one that sits eating or drinking (its rest's aura). b442: 4 "is dead",
            // 3 "is not in the instance", 4 "trails the group", 3 "sits" without an order
            if (!orderFor)
                if (Player* member = ObjectAccessor::FindPlayer(guid))
                {
                    if (!member->IsAlive())
                        orderFor = RaidLeader::GetResurrectVerdict(member) != RaidLeader::ResurrectVerdict::Unknown;
                    else if (member->HasAuraType(SPELL_AURA_MOD_REGEN) ||
                             member->HasAuraType(SPELL_AURA_MOD_POWER_REGEN))
                        orderFor = true;
                    else if (member->isMoving() &&
                             (member->GetMap() != tank->GetMap() || Leader::Dungeon::MemberClosing(member, tank)))
                        orderFor = true;
                    if (orderFor)
                    {
                        static std::set<uint64> logged;  // world thread
                        if (logged.size() > 4096)
                            logged.clear();
                        if (logged.insert(guid.GetRawValue()).second)
                            LOG_INFO("playerbots", "[dun222] {}: the wait for {} ({}) is its own ordered way: no "
                                     "missing order", run.runId, member->GetName(), waitWhy);
                    }
                }
            if (!orderFor && waitWhy.compare(0, 10, "[lead940] ") == 0)
                if (Player* bot = ObjectAccessor::FindPlayer(guid))
                    // [dun223] its own heal over time too (b442: Enyssia's Rejuvenation, Brelir's Regrowth)
                    for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_PERIODIC_HEAL))
                        if (effect->GetBase()->GetDuration() > 0)
                            orderFor = true;
        }

        for (size_t i = 0; i < members.size(); ++i)
        {
            LeaderGroup::MemberInfo const& member = members[i];
            // the verdict's text without its numbers: a new state is a new reason, not a new distance
            std::string const waitState =
                i == waited && !orderFor ? waitWhy.substr(0, waitWhy.find_first_of("0123456789")) : std::string();
            note(member, "wait_without_order", waitState,
                 waitWhy + "; no order moves or readies " + member.name);
            // [lead1420] fight_no_action: wait_without_order judges the pull's readiness out of a fight only - b262
            // rfc-h 5G-15 123-177 s (52 s, no member acting in the fight) raised nothing. In a fight: the leader's
            // tick ended with no member acting (lead.fightIdle, the deadlock guarantee's own state), on the tank's
            // line; its state is the verdict's text without its numbers
            std::string const& idleWhy = lead.fightIdleWhy;
            std::string const idleState = member.guid == tank->GetGUID() && lead.fightIdle && tank->IsInCombat()
                                              ? idleWhy.substr(0, idleWhy.find_first_of("0123456789"))
                                              : std::string();
            note(member, "fight_no_action", idleState, lead.fightIdleWhy);
            // [lead1451] member_no_action: the member ended two leader ticks of the fight in a row without an action
            // of its own (lead.memberNoAction, the member break's own state); its state is its move purpose without
            // the numbers
            auto const noAction = lead.memberNoAction.find(member.guid);
            std::string const noActionWhy =
                noAction != lead.memberNoAction.end() && tank->IsInCombat() ? noAction->second : std::string();
            note(member, "member_no_action", noActionWhy.substr(0, noActionWhy.find_first_of("0123456789")),
                 "no action of its own in the fight for two leader decisions: " + noActionWhy);

            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
            bool const here = botAI && bot->IsInWorld() && !bot->IsBeingTeleported() && bot->IsAlive() &&
                              bot->GetMap() == tank->GetMap();

            // order_impossible: an order of the member the game refuses now (the orders' own check)
            std::string impossible =
                here && hasEvents[i] ? LeaderOrderCheck::OrderRefusal(botAI, events[i]) : std::string();
            // [dun221] the member's loss of control (stunned, feared, confused, silenced, pacified: the game's state
            // for its duration) refuses every order alike - no wrong order, the order runs when it ends (b442: 8
            // "external 14318 on itself: SPELL_FAILED_STUNNED")
            for (char const* passing : {"SPELL_FAILED_STUNNED", "SPELL_FAILED_FLEEING", "SPELL_FAILED_CONFUSED",
                                        "SPELL_FAILED_SILENCED", "SPELL_FAILED_PACIFIED"})
                if (impossible.find(passing) != std::string::npos)
                    impossible.clear();
            note(member, "order_impossible", impossible, impossible);

            // credit_unguaranteed: a tool or a potion the member's mana line counted ready now whose use the game
            // refuses now. Not a refusal: its cooldown or the potion of this combat (the line's own input: the
            // credit was used since), a full pool (it is for the mana spent later)
            std::string credit;
            if (here)
                for (RestState::ManaLineCredit const& counted : RestState::ManaLineCredits(member.guid))
                {
                    SpellCastResult refusal = SPELL_CAST_OK;
                    if (counted.spell)
                    {
                        if (bot->HasSpellCooldown(counted.spell))
                            continue;
                        // the summon's target: an enemy in its reach (out of a fight: the next fight's)
                        Unit* on =
                            counted.spell == RestState::SPELL_SHADOWFIEND ? RestState::ShadowfiendReach(bot) : bot;
                        if (!on)
                            continue;
                        refusal = botAI->CheckCastNow(counted.spell, on).refusal;
                    }
                    else if (Item* item = counted.item ? bot->GetItemByEntry(counted.item) : nullptr)
                    {
                        uint32 spell = 0;
                        if (ItemTemplate const* proto = item->GetTemplate())
                            for (uint8 s = 0; s < MAX_ITEM_PROTO_SPELLS && !spell; ++s)
                                if (proto->Spells[s].SpellId > 0 &&
                                    proto->Spells[s].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                                    spell = uint32(proto->Spells[s].SpellId);
                        if (!spell || bot->HasSpellCooldown(spell))
                            continue;
                        refusal = botAI->CheckCastNow(spell, bot, item).refusal;
                    }
                    // [lead1463] b267 rfc-h: 30 "mana potion: SPELL_FAILED_STUNNED" (5A-1 Rtpalaa x11, 5A-9 x7, ...):
                    // a stun is a passing state - its aura runs out and the potion is usable again (Spell::
                    // CheckCasterAuras refuses only while the stun's UNIT_FLAG_STUNNED holds); the credit stands
                    if (refusal == SPELL_CAST_OK || refusal == SPELL_FAILED_NOT_READY ||
                        refusal == SPELL_FAILED_ALREADY_AT_FULL_POWER ||
                        refusal == SPELL_FAILED_ALREADY_AT_FULL_HEALTH || refusal == SPELL_FAILED_STUNNED)
                        continue;
                    credit = counted.name + ": " + PlayerbotAI::CastResultName(refusal);
                    break;
                }
            note(member, "credit_unguaranteed", credit, credit);
        }
    }

    // [lead7] C:/WoW/LEADER_REFACTOR_PLAN.md step 7, LEADER_PLANNER_IN_LEADER: the leader plans the tank's pull.
    // Called on every world update (the world thread, after the map updates: no map thread runs), it looks only when
    // the tank asked (its engine looked at "aggressive target", as it looked at the tank's own planner before). The
    // planner instance's look publishes the verdicts as the tank's did (CarefulPlanVerdict, the crowd control plan);
    // a Go decision is issued (the tank executes it, PullPlanner::TakeIssued) and logged as the leader's pull_order;
    // a hopeless refusal (no pull of the pack fits at all) is the planner_no_option bug event (user decision 1).
    void TickLeaderPlanner(LeaderGroup& group)
    {
        if (!Leader::LEADER_PLANNER_IN_LEADER || group.LeaderGuid().IsEmpty() || !group.HasContent() ||
            !Leader::Orders::IsTracked(group.LeaderGuid()))
            return;

        Player* tank = ObjectAccessor::FindPlayer(group.LeaderGuid());
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!tank || !tankAI || !tank->IsInWorld() || tank->IsBeingTeleported())
            return;

        auto& lead = group.State();
        bool const decides = group.LeaderDecides();  // [lead3] steps 3-10 (a 5-man): the leader's own inputs
        uint32 const now = getMSTime() ? getMSTime() : 1;

        // [lead6] LEADER_ONE_READINESS: the one readiness verdict, at most every LEADER_READY_CHECK_MS, published for
        // the bot layers that judged it themselves (RaidLeader's ready-check pull hold)
        constexpr uint32 LEADER_READY_CHECK_MS = 500;
        if (decides && Leader::Leads(Leader::LEADER_ONE_READINESS) &&
            (!lead.readyCheckMs || getMSTimeDiff(lead.readyCheckMs, now) >= LEADER_READY_CHECK_MS))
        {
            lead.readyCheckMs = now;
            lead.notReadyWhy = LeaderReadyWhy(group, tank);
            lead.ready = lead.notReadyWhy.empty();
            if (Group* group = tank->GetGroup())
                Leader::Readiness::Publish(group->GetGUID(), lead.ready, lead.notReadyWhy);
        }

        // [lead13] toc5-n b56 (2/4, stalled): the tank's engine looked at "aggressive target" only now and then ("not
        // evaluated: toc mounted (66.00) ran first" 37 of 53 looks), each look asked, and the decision answering it
        // was stale (5 s) by the next one: 0 tank lines in the whole run, the Argent Monk pull_order never executed.
        // The leader keeps a decision ready: besides answering an ask, it looks once per LEADER_LOOK_PERIOD_MS while
        // the tank asked within LEADER_ASKING_MS (it stands out of combat without a target), so whenever the tank
        // looks a fresh decision waits.
        if (decides)
        {
            TickLeaderEvents(group, tank);       // [lead15] the events to every member
            CheckLeaderInvariants(group, tank);  // [lead1390]
        }
        constexpr uint32 LEADER_LOOK_PERIOD_MS = 1000;
        constexpr uint32 LEADER_ASKING_MS = 30 * IN_MILLISECONDS;
        bool const asked = PullPlanner::TakeAsk(group.LeaderGuid(), Leader::LEADER_ASK_FRESH_MS);
        // [lead30] the leader asks its planner by itself (no wait for the tank's ask, no ask window): the route's
        // target always has an answer - a plan or a refusal with its reason (planner_answer)
        bool const keepReady = !asked && decides && !tank->IsInCombat() &&
                               (!lead.lastLookMs || getMSTimeDiff(lead.lastLookMs, now) >= LEADER_LOOK_PERIOD_MS) &&
                               (Leader::LEADER_NO_FORCING_TIMERS ||
                                PullPlanner::AskedWithin(group.LeaderGuid(), LEADER_ASKING_MS));
        if (!asked && !keepReady)
            return;
        lead.lastLookMs = now;

        DecisionRun const run = group.Run();
        auto const spot = [](JsonObject& fields, char const* key, Position const& at)
        {
            JsonArray xyz;
            xyz.Raw(Acore::StringFormat("{:.1f}", at.GetPositionX()))
                .Raw(Acore::StringFormat("{:.1f}", at.GetPositionY()))
                .Raw(Acore::StringFormat("{:.1f}", at.GetPositionZ()));
            fields.Raw(key, xyz.Build());
        };
        Map* map = tank->GetMap();
        auto const fresh = [now](uint32 ms) { return ms && getMSTimeDiff(ms, now) <= Leader::LEADER_REQUEST_TTL_MS; };
        auto const idleAlive = [map](ObjectGuid guid) -> Creature*
        {
            Creature* creature = guid.IsEmpty() || !map ? nullptr : map->GetCreature(guid);
            return creature && creature->IsInWorld() && creature->IsAlive() && !creature->IsInCombat() ? creature
                                                                                                     : nullptr;
        };
        // [lead1090] user 2026-11-04 (Q55): a boss's pull opens only after his buff pass, and no pass opens while a
        // pull is open or a fight is on. The wait holds only the pull of that boss or a pull that brings him
        // (BossBuffPassHolds), and the leader opens the pass it waits for itself, out of combat, whatever the
        // harness phase (BossBuffPass runs on the harness's travel step only). b234 rfc-h 5F-14 840-1096 s:
        // lead1084 held the Jergosh pull for a Bazzalan pass that nothing opened - the run stalled
        auto const bossPassHold = [&](Unit* pull) -> std::string
        {
            Creature* boss = BossBuffPassHolds(group, tank, pull);
            if (!boss)
                return std::string();
            bool const needBuffBefore = group.NeedBuff();
            if (OpenBossBuffPass(group, boss) && !needBuffBefore)
                Decisions().Write(run, "leader", "boss_buff_pass_opened", boss->GetGUID(), boss->GetName(), true,
                                  JsonObject().Str("pull", pull->GetName()).Str("tag", "[lead1090]"));
            // [lead1295] b251 rfc-h 5B-2 1083-1110 s: the pass before Bazzalan held the group 15 s (the shadow priest
            // at 2 % drank for its buff orders only once they were issued, 1089-1100 s), and only after it did the
            // planner read the boss lines - the shaman at 55 % drank to 100 % for 10 s more (1100-1110 s). This hold
            // is the boss's own pull or one that brings him (BossBuffPassHolds), so his fight is the next one: his
            // wait starts with his pass - every member below its boss line rests while the others buff (lead46).
            // lead1260's trash-first case does not arise: no other pull is held by his pass
            if (lead.bossWaitFor != boss->GetGUID())
            {
                lead.bossWaitFor = boss->GetGUID();
                Decisions().Write(run, "leader", "boss_wait_with_pass", boss->GetGUID(), boss->GetName(), true,
                                  JsonObject().Str("pull", pull->GetName()).Str("tag", "[lead1295]"));
            }
            return "[lead1084] the buff pass before " + boss->GetName() + " first";
        };
        // [lead1370] b259 rfc-h 5H-16 301.4-539 s (stalled): boss_pull_waits and commit_wait said "a member cannot
        // follow (trailing / its walk blocked)" and named nobody - the five stood within 3 yd of the tank. The wait
        // names the member it holds for, what holds it (trails / the mob its walk would wake / no mob named) and
        // its live order (what moves it: the member loop's Follow for a trailing one, the blocking mob's pull for a
        // blocked one, lead364). `counts`: the blocks this wait reads (the commit's: those outside its pull)
        std::string waitMember;  // [lead1370] the member the last cannotFollow named (the decision line's "member")
        auto const cannotFollow = [&](auto const& counts) -> std::string
        {
            waitMember.clear();
            auto const named = [&](ObjectGuid member, std::string const& holdsIt) -> std::string
            {
                Player* bot = ObjectAccessor::FindPlayer(member);
                waitMember = bot ? bot->GetName() : std::string("a member not in the world");
                Leader::MemberEvents live;
                std::string const order = Leader::Events::Get(member, live) && !live.moveWhy.empty()
                                              ? live.moveWhy
                                              : std::string("no move order");
                return "a member cannot follow (trailing / its walk blocked): " + waitMember + " " + holdsIt +
                       "; its order: " + order;
            };
            if (!lead.trailing.empty())
                return named(*lead.trailing.begin(), "trails the group");
            for (auto const& blocked : lead.groupBlockedBy)
            {
                if (!counts(blocked))
                    continue;
                Creature* mob = blocked.second.IsEmpty() || !map ? nullptr : map->GetCreature(blocked.second);
                return named(blocked.first, mob ? "its walk would wake " + mob->GetName()
                                                : std::string("its walk blocked (no mob named)"));
            }
            return std::string();
        };

        // The issued decision's pull_order line: one per new order (another mob or kind), or again after
        // PULL_ORDER_LOG_MS. `pickWhy`: [lead5] what the leader's pick was.
        auto const issue = [&](PullPlanner::Decision const& decision, std::string const& pickWhy)
        {
            PullPlanner::Issue(group.LeaderGuid(), decision);
            // [lead171] b180 pull_trace (523 orders): the tank's engine first looked at an issued order 1.5 s median
            // (p90 2.5 s) after it - out of combat a led tank kept the idle 10-30 x react delay (GetReactDelay; only
            // executor members are woken by their events). The order wakes it like a member's event
            if (Leader::LEADER_FAST_REACT)
                if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank))
                    tankAI->SetNextCheckDelay(0);
            // [lead14] the route view's red line: the issued pull's mob and the spot the tank pulls from
            {
                ObjectGuid const mob = !decision.planTarget.IsEmpty()
                                           ? decision.planTarget
                                           : (!decision.attack.IsEmpty() ? decision.attack : decision.lookTarget);
                Position spot;
                if (decision.side)
                    spot = decision.sideSpot;
                else if (decision.los)
                    spot = decision.losSpot;
                else if (decision.hasCast)
                    spot = decision.castSpot;
                else if (Unit* unit = mob.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, mob))
                    spot = unit->GetPosition();
                lead.issuedMob = mob;
                lead.issuedKind = decision.kind;
                lead.issuedSpot = spot;
                lead.issuedMs = now;
                // [lead18] the whole plan, latched: from where the tank stands now, the fight spot, the target
                lead.issuedFrom = tank->GetPosition();
                if (!decision.killFor.IsEmpty() && decision.killFor == decision.planTarget)
                    lead.issuedFight = decision.kill;
                else if (!decision.fightFor.IsEmpty() && decision.fightFor == decision.planTarget)
                    lead.issuedFight = decision.fight;
                else if (decision.pullBack)
                    lead.issuedFight = tank->GetPosition();
                else
                    lead.issuedFight = spot;
                if (Unit* unit = mob.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, mob))
                    lead.issuedTargetAt = unit->GetPosition();
                else
                    lead.issuedTargetAt = spot;
            }
            struct IssueLog
            {
                std::string sig;
                uint32 ms = 0;
            };
            static std::unordered_map<uint32, IssueLog> issueLogs;  // party seq -> last line (world thread)
            ObjectGuid const mobGuid =
                !decision.planTarget.IsEmpty()
                    ? decision.planTarget
                    : (!decision.approach.IsEmpty()
                           ? decision.approach
                           : (!decision.attack.IsEmpty() ? decision.attack : decision.lookTarget));
            std::string const sig = decision.kind + ":" + std::to_string(mobGuid.GetRawValue());
            IssueLog& last = issueLogs[group.Run().runSeq];
            if (last.sig == sig && last.ms && getMSTimeDiff(last.ms, now) < Leader::PULL_ORDER_LOG_MS)
                return;
            last.sig = sig;
            last.ms = now;

            Unit* mob = mobGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, mobGuid);
            JsonObject fields;
            fields.Str("kind", decision.kind).Str("method", Leader::MethodName(decision.method))
                .Num("expected", decision.mobs).Real("cost", decision.cost).Bool("single", decision.single)
                .Bool("careful", decision.careful).Num("pack", int64(decision.pack.size()))
                .Str("reason", decision.reason);
            if (!pickWhy.empty())
                fields.Str("pick", pickWhy);
            if (decision.side)
                spot(fields, "sideSpot", decision.sideSpot);
            if (decision.los)
            {
                spot(fields, "standSpot", decision.losSpot);
                spot(fields, "gather", decision.losGather);
            }
            else if (decision.hasCast)
                spot(fields, "standSpot", decision.castSpot);
            if (!decision.killFor.IsEmpty() && decision.killFor == decision.planTarget)
                spot(fields, "fightSpot", decision.kill);
            else if (!decision.fightFor.IsEmpty() && decision.fightFor == decision.planTarget)
                spot(fields, "fightSpot", decision.fight);
            spot(fields, "tank", tank->GetPosition());
            // [lead1325] b255 rfc-h 5B-2 / 5B-10 pull 1 t=7: the same pull read "the walk in wakes Earthborer (41 yd)"
            // (pull back, 4 mobs) in one run and "3 mobs at its spot" (engage) in the other - a wandering Earthborer
            // stood on the walk in one, beside the pack in the other; the decision logged no mob's place, so the
            // triage read the inputs as equal. The decision's mobs where they stand now (a wanderer's spot is RNG)
            JsonArray packAt;
            for (ObjectGuid const& guid : decision.pack)
                if (Creature* packMob = tank->GetMap()->GetCreature(guid))
                    packAt.Raw(JsonObject()
                                   .Num("low", int64(guid.GetCounter()))
                                   .Real("x", packMob->GetPositionX())
                                   .Real("y", packMob->GetPositionY())
                                   .Real("tankYd", tank->GetExactDist(packMob))
                                   .Bool("wanders", packMob->GetDefaultMovementType() != IDLE_MOTION_TYPE)
                                   .Build());
            fields.Raw("packAt", packAt.Build());
            // [lead1331] the wanderers this plan judged by their wander area (spawn point + wander distance), and
            // by which checks
            std::map<uint32, std::string>& judged = Leader::Fight::WanderJudged();
            if (!judged.empty())
            {
                JsonArray wanderArea;
                for (auto const& [spawnId, checks] : judged)
                    wanderArea.Raw(JsonObject().Num("spawn", int64(spawnId)).Str("checks", checks).Build());
                fields.Raw("wanderArea", wanderArea.Build());
                judged.clear();
            }
            Decisions().Write(run, "leader", "pull_order", mobGuid, mob ? mob->GetName() : std::string(),
                                          true, fields);
        };

        // [lead18] LEADER_COMMIT_PULL: the committed pull's gate, window and order; true: the look ends here
        auto const runCommit = [&]() -> bool
        {
            if (!lead.commit)
                return false;
            PullPlanner::Decision const& committed = *lead.commit;
            Unit* mob = ObjectAccessor::GetUnit(*tank, lead.commitMob);
            // [lead20] a member resting: the commitment's clock stands still (rest time is no stuck time)
            {
                bool resting = false;
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid); bot && bot->IsSitState())
                        resting = true;
                if (resting && lead.commitTickMs)
                    lead.commitSinceMs += getMSTimeDiff(lead.commitTickMs, now);
                // [lead23] the leader's own hold (rest, a closed window, the spot gate) runs no take clock
                // [lead24] rfc-h 20261031-115146: the tank could not take the issued engage while the leader's own
                // readiness held the pull (the shaman drank): the leader's wait, not the tank's
                if (lead.commitIssued && !lead.commitOrderSeen && lead.commitTickMs &&
                    (resting || !lead.patrolWindowOpen || lead.spotsWaiting || !lead.ready))
                    lead.commitIssuedMs += getMSTimeDiff(lead.commitTickMs, now);
                lead.commitTickMs = now;
            }
            std::string broken;
            // [lead113] b147 rfc-h 60.9 s: the engage on Earthborer 6 was released "the fight is on" while only the
            // healer fought (Earthborer 4 had jumped it); the tank, in no fight, stopped. The order ends with the
            // tank's own fight (a mob on it or its victim); the mob on a member is the tank's take-back (lead112)
            bool const tankFights = tank->IsInCombat() && (tank->GetVictim() || !tank->getAttackers().empty());
            // [lead908] a mob of none of this pull's mobs (the plan's, the walkers taken along) on a member
            Unit* unplanned = nullptr;
            if (group.FightOn(tank) && !tankFights)
            {
                std::vector<ObjectGuid> planned = LeaderPullMobs(lead.commit, lead.commitMob);
                planned.insert(planned.end(), lead.takenWalkers.begin(), lead.takenWalkers.end());
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid); bot && bot->IsAlive() && !unplanned)
                        for (Unit* attacker : bot->getAttackers())
                            if (attacker && attacker->IsAlive() && LeaderFightMob(tank, attacker) &&
                                std::find(planned.begin(), planned.end(), attacker->GetGUID()) == planned.end())
                            {
                                unplanned = attacker;
                                break;
                            }
            }
            if (group.FightOn(tank) && tankFights)
            {
                broken = "the fight is on";
                // [lead25] user: «если лишние мобы сагрятся, то танк неправильно выбирает позицию» - an add outside
                // the committed pull: extra_aggro with the fight spot and its distance
                std::vector<ObjectGuid> const planned = LeaderPullMobs(lead.commit, lead.commitMob);
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                        for (Unit* attacker : bot->getAttackers())
                            if (attacker && LeaderFightMob(tank, attacker) &&
                                std::find(planned.begin(), planned.end(), attacker->GetGUID()) == planned.end() &&
                                std::find(lead.extraLogged.begin(), lead.extraLogged.end(), attacker->GetGUID()) ==
                                    lead.extraLogged.end())
                            {
                                lead.extraLogged.push_back(attacker->GetGUID());
                                // [lead1210] the runner that brought it: the nearest living mob of this instance
                                // the group saw flee (FledMobs), with its distance to the add
                                Unit* fleer = nullptr;
                                for (ObjectGuid const& fled : FledMobs::Noted(tank->GetMapId(), tank->GetInstanceId()))
                                    if (Unit* ran = ObjectAccessor::GetUnit(*tank, fled);
                                        ran && ran != attacker && ran->IsAlive() &&
                                        (!fleer || ran->GetExactDist(attacker) < fleer->GetExactDist(attacker)))
                                        fleer = ran;
                                Decisions().Write(
                                    run, "leader", "extra_aggro", attacker->GetGUID(), attacker->GetName(), true,
                                    JsonObject().Str("add", attacker->GetName()).Str("on", member.name)
                                        .Real("fightSpotDistance", attacker->GetExactDist(lead.meetPoint))
                                        .Real("fightX", lead.meetPoint.GetPositionX())
                                        .Real("fightY", lead.meetPoint.GetPositionY())
                                        .Str("fleer", fleer ? fleer->GetName() : "-")
                                        .Num("fleerLow", fleer ? int64(fleer->GetGUID().GetCounter()) : 0)
                                        .Real("fleerDistance", fleer ? fleer->GetExactDist(attacker) : -1.0f));
                            }
            }
            // [lead908] user 2026-11-03 (42): an unplanned fight cancels the open pull - a mob of none of this pull's
            // mobs (the plan's, the walkers taken along) attacks a member while the tank is not yet in its own fight:
            // the tank stops walking to the planned pull and finishes the running fight first (its take-back, lead112);
            // the planned mob stays the leader's next pick (lead101)
            else if (unplanned)
            {
                broken = "an unplanned fight: " + unplanned->GetName() + " attacks the group, finished first";
                if (Leader::Orders::Close(group.LeaderGuid(), Leader::OrderState::Aborted, broken,
                                          tank->GetPosition(), nullptr))
                    LOG_INFO("playerbots", "[lead908] {}: the open pull on {} cancelled: {}", tank->GetName(),
                             mob ? mob->GetName() : std::string("?"), broken);
                if (Creature* own = mob ? mob->ToCreature() : nullptr;
                    own && Leader::Fight::OrderMobStaysPick(tank, own))
                {
                    lead.keptPick = own->GetGUID();
                    lead.keptPickWhy = "its pull was cancelled by an unplanned fight";
                }
            }
            else if (!mob || !mob->IsAlive())
                broken = "the mob is dead";
            else if (mob->IsInCombat())
                broken = "the mob is in a fight";
            else if (!lead.commitMoving && mob->GetExactDist(lead.commitPos) > Leader::LEADER_COMMIT_MOVED_YD)
                broken = "the mob moved away";
            else if (getMSTimeDiff(lead.commitSinceMs, now) > Leader::LEADER_COMMIT_MAX_MS)
            {
                if (Leader::LEADER_NO_FORCING_TIMERS)  // [lead24]
                    WaitedLong(group, "commit_age", getMSTimeDiff(lead.commitSinceMs, now),
                               mob ? mob->GetName() : std::string("?"));
                else
                    broken = "committed too long";
            }
            float patrolGap = 0.0f;
            float patrolReach = 0.0f;
            bool beatHold = false;  // [dun226] the window is the patrol at its beat's far end
            float beatFar = 0.0f;
            Creature* patrol = broken.empty()
                                   ? LeaderPatrolNear(tank, lead.commitSpot, committed, patrolGap, patrolReach)
                                   : nullptr;
            // [lead23] rfc-h 20261031-113954: the window closed on the pull's target itself (a wanderer)
            if (patrol && patrol->GetGUID() == lead.commitMob)
                patrol = nullptr;
            // [dun228] a patrol behind closed doors (out of the tank's sight, no walk round them) never comes to the
            // pull and is no pull itself: no window, no "patrol first". b444 sfk-n five1-1 / four 0/8 stalls: the
            // Wailing Guardsman beyond the closed Courtyard Door 18895 was the patrol target 30+ times ("plan:
            // charge"), every plan "crosses the closed Courtyard Door" (plan_through_door), nothing committed
            if (patrol && Leader::Travel::CagedBlocker(tank, patrol))
            {
                if (lead.patrolCagedLogged != patrol->GetGUID())
                {
                    lead.patrolCagedLogged = patrol->GetGUID();
                    LOG_INFO("playerbots", "[dun228] {}: the patrol {} is behind closed doors: no window for {}",
                             run.runId, patrol->GetName(), mob ? mob->GetName() : std::string("?"));
                }
                patrol = nullptr;
            }
            // [lead34] b83 rfc-h: the leader's target (its last plan) is no patrol either
            if (patrol && (patrol->GetGUID() == lead.planTarget ||
                           std::find(lead.planMobs.begin(), lead.planMobs.end(), patrol->GetGUID()) !=
                               lead.planMobs.end()))
                patrol = nullptr;
            // [lead33] user: «должна оцениваться опасность пака»: the window is only for a patrol whose joining makes
            // the pull unsafe by the planner's danger model - the pull's own mobs plus the patrol's whole walking
            // group, judged as one fight (PullPlanner::JudgePack). Within the budget: taken along (PullPack), no
            // window
            if (patrol)
            {
                // [lead34] b83 rfc-h: «walkers 14 / 16» round one Ragefire Trogg - the cell visit's searcher has no
                // range check (every idle mob of the cells). The walking group is the patrol and its formation's
                // members (creature_formations), the ones that walk with it
                std::list<Creature*> walkers{patrol};
                IdleAggroCheck check{tank};
                if (CreatureGroup* formation = patrol->GetFormation())
                    for (auto const& [member, info] : formation->GetMembers())
                        if (member && member != patrol && member->IsInWorld() && check(member))
                            walkers.push_back(member);
                // [dun176] each walker's engage call where it walks into the fight (Creature::CallAssistance, the
                // core's Acore::AnyAssistCreatureInRangeCheck within CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS) brings
                // the idle mobs walking with it: b424 rfc-n five5 pull 7 judged "3 mobs" with one Ragefire Trogg
                // walker taken along, 8 came and the five wiped
                if (float const assistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
                    assistRadius > 0.0f)
                {
                    std::list<Creature*> helpers;
                    for (Creature* walker : walkers)
                    {
                        if (walker->HasFlagsExtra(CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE))
                            continue;
                        std::list<Creature*> found;
                        Acore::AnyAssistCreatureInRangeCheck assists(walker, tank, assistRadius);
                        Acore::CreatureListSearcher<Acore::AnyAssistCreatureInRangeCheck> searcher(walker, found,
                                                                                                    assists);
                        Cell::VisitObjects(walker, searcher, assistRadius);
                        for (Creature* helper : found)
                            if (helper && check(helper) &&
                                std::find(walkers.begin(), walkers.end(), helper) == walkers.end() &&
                                std::find(helpers.begin(), helpers.end(), helper) == helpers.end())
                                helpers.push_back(helper);
                    }
                    walkers.splice(walkers.end(), helpers);
                }
                // [dun212] the patrol comes to the group: the fight is where the group stands (the tank, its
                // members a follow spread round it) and where its pull is fought (the commit spot); every idle mob
                // whose aggro reaches there in its sight (Creature::MoveInLineOfSight: Leader::Travel::AggroReach)
                // joins it. b442 sfk-n five7-7 pull 7: the Slavering Worg walker "taken along: 6 mobs, overload
                // 0.98" at the courtyard's edge, the courtyard's Moonwalkers, Worgs and Servitors standing in reach
                // came too and the five wiped (4 dead, the same in five2-1 with the Tormented Officers)
                {
                    std::list<Creature*> standing;
                    IdleAggroCheck idle{tank};
                    float constexpr searchYd = 60.0f;  // GetAggroRange's 45 yd cap + combat reach, wander, margin
                    auto const addStanding = [&](Position const& at)
                    {
                        struct InRange
                        {
                            Position at;
                            float range;
                            bool operator()(Creature* c) const { return c->GetExactDist(at) <= range; }
                        };
                        std::list<Creature*> found;
                        InRange inRange{at, searchYd};
                        Acore::CreatureListSearcher<InRange> searcher(tank, found, inRange);
                        Cell::VisitObjects(at.GetPositionX(), at.GetPositionY(), tank->GetMap(), searcher, searchYd);
                        for (Creature* mob : found)
                        {
                            if (!mob || !idle(mob) || std::find(walkers.begin(), walkers.end(), mob) != walkers.end() ||
                                std::find(standing.begin(), standing.end(), mob) != standing.end())
                                continue;
                            float const reach =
                                Leader::Travel::AggroReach(mob, tank) + Leader::Travel::APPROACH_FOLLOW_SPREAD;
                            if (mob->GetExactDist(at) <= reach &&
                                mob->IsWithinLOS(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ() + 2.0f))
                                standing.push_back(mob);
                        }
                    };
                    addStanding(tank->GetPosition());
                    if (lead.commitSpot.GetExactDist(tank) > Leader::Travel::APPROACH_FOLLOW_SPREAD)
                        addStanding(lead.commitSpot);
                    std::vector<ObjectGuid> const own = LeaderPullMobs(lead.commit, lead.commitMob);
                    std::string names;
                    for (Creature* mob : standing)
                        if (std::find(own.begin(), own.end(), mob->GetGUID()) == own.end() &&
                            std::find(lead.takenWalkers.begin(), lead.takenWalkers.end(), mob->GetGUID()) ==
                                lead.takenWalkers.end())
                        {
                            walkers.push_back(mob);
                            names += (names.empty() ? "" : ", ") + mob->GetName();
                        }
                    if (!names.empty() && lead.patrolStandingFor != patrol->GetGUID())
                    {
                        lead.patrolStandingFor = patrol->GetGUID();
                        LOG_INFO("playerbots", "[dun212] {}: the patrol {} is fought where the group stands: in "
                                 "reach there {}",
                                 run.runId, patrol->GetName(), names);
                    }
                }
                std::vector<ObjectGuid> combined = LeaderPullMobs(lead.commit, lead.commitMob);
                combined.insert(combined.end(), lead.takenWalkers.begin(), lead.takenWalkers.end());
                for (Creature* walker : walkers)
                    combined.push_back(walker->GetGUID());
                float overload = 0.0f;
                std::string judged;
                bool fits = PullPlanner::JudgePack(tank, combined, overload, judged);
                // [lead167] b179 rfc-h 5G 866-930 s: Taragaman released 39 times for "the patrol first" - the
                // patrol Searing Blade Enforcer comes into his fight anyway (the planner refuses its own pull as
                // "brings a boss, pulled as his fight"), JudgePack's "a boss" made the pair over the budget. A patrol
                // the boss brings is his fight: taken along
                Creature* const bossPull = mob ? mob->ToCreature() : nullptr;
                // [lead1216] b240 rfc-h 5G-15 671 s: this took the patrol Enforcer along with Taragaman as "the
                // combined fight fits the budget ... overload 1.78" - it did not fit; the boss brings it by game
                // rule, so it is his fight whatever the budget (the 1.78 itself came from the group damage fit a
                // deadlock broke, lead1213). Named as what it is
                bool broughtByBoss = false;
                if (!fits && bossPull && IsBossLook(tank, bossPull->GetGUID()) &&
                    Leader::Travel::BossBroughtBy(tank, bossPull, patrol))
                {
                    fits = true;
                    broughtByBoss = true;
                    judged += "; the boss brings it: his fight";
                }
                if (lead.patrolDangerFor != patrol->GetGUID() || lead.patrolDangerFits != fits)
                {
                    lead.patrolDangerFor = patrol->GetGUID();
                    lead.patrolDangerFits = fits;
                    Decisions().Write(run, "leader", "patrol_danger", patrol->GetGUID(), patrol->GetName(),
                                                  true,
                                                  JsonObject().Str("pull", mob ? mob->GetName() : "?")
                                                      .Num("walkers", int64(walkers.size())).Real("overload", overload)
                                                      .Str("judged", judged)
                                                      .Str("decision", fits ? "taken along: no window"
                                                                            : "over the budget: the window holds"));
                }
                // [lead34] LEADER_PATROL_IS_TARGET: the danger chooses along / own pull, never a wait. Only a patrol
                // that would close the old window (within twice its reach of the pull or the tank) is judged.
                // [dun217] the patrol's own pull judged as the planner would (its walkers, their helpers and the
                // standing mobs in reach of where it is fought, PullPlanner::JudgePack): when that is over the budget
                // as well, nothing here can be pulled safely - the commit is kept and its pull waits while the patrol
                // walks within twice its reach (the window, judged by the patrol's walk). b442 sfk-n five15-15: 28
                // "the patrol first: its own pull" releases of Rethilgore's pack, the patrol's own pull "no safe pull
                // ... overload 2.11", nothing issued for 4 minutes, stalled at 0/8
                bool patrolOwnRefused = false;
                if (Leader::LEADER_PATROL_IS_TARGET && !fits && patrolGap <= 2.0f * patrolReach &&
                    !lead.commitIssued && broken.empty())
                {
                    std::vector<ObjectGuid> own;
                    for (Creature* walker : walkers)
                        own.push_back(walker->GetGUID());
                    float ownOverload = 0.0f;
                    std::string ownJudged;
                    patrolOwnRefused = !PullPlanner::JudgePack(tank, own, ownOverload, ownJudged);
                    // [dun219] a beat that never leaves twice its reach of the pull opens no window: the commit is
                    // planned again, its way back searched out of the beat (the planner's kill spot, priced as any
                    // pull) - no wait that never ends
                    // [dun224] whether or not its own pull fits: b443 sfk-n five4-4 / five1-1, the Wailing Guardsman's
                    // own pull "plan: walks in" was never issued (its beat took it out of reach and back), the
                    // Whitescalp commit released 14 times "the patrol first", stalled at 0/8
                    // [dun226] once per patrol and commit: the plan again came back inside the beat (the way back
                    // did not clear it), so the verdict holds - the pull waits for the patrol at its beat's far end
                    // (the window opens as far as it ever walks, its waypoints), logged once; no release and
                    // recommit every tick
                    bool const beatReleased =
                        lead.beatReleasedPatrol == patrol->GetGUID() && lead.beatReleasedMob == lead.commitMob;
                    if (beatReleased && PullPlanning::PatrolBeatStays(patrol, lead.commitSpot, 2.0f * patrolReach))
                    {
                        patrolOwnRefused = true;
                        beatHold = true;
                        beatFar = PullPlanning::PatrolBeatFarthest(patrol, lead.commitSpot);
                        if (lead.beatHoldLogged != patrol->GetGUID())
                        {
                            lead.beatHoldLogged = patrol->GetGUID();
                            LOG_INFO("playerbots",
                                     "[dun226] {}: {}: the way back did not clear the patrol {}'s beat: the pull waits "
                                     "for it at its beat's far end ({:.0f} yd)",
                                     run.runId, mob ? mob->GetName() : std::string("?"), patrol->GetName(), beatFar);
                        }
                    }
                    else if (PullPlanning::PatrolBeatStays(patrol, lead.commitSpot, 2.0f * patrolReach))
                    {
                        lead.beatReleasedPatrol = patrol->GetGUID();
                        lead.beatReleasedMob = lead.commitMob;
                        patrolOwnRefused = false;
                        broken = "the patrol's beat stays at the pull: planned again out of its beat";
                        if (lead.patrolHeldFor != patrol->GetGUID())
                        {
                            lead.patrolHeldFor = patrol->GetGUID();
                            LOG_INFO("playerbots",
                                     "[dun219] {}: {}: the patrol {}'s beat never leaves {:.0f} yd of it, no window: "
                                     "planned again, its way back out of the beat",
                                     run.runId, mob ? mob->GetName() : std::string("?"), patrol->GetName(),
                                     2.0f * patrolReach);
                        }
                    }
                    if (patrolOwnRefused && lead.patrolHeldFor != patrol->GetGUID())
                    {
                        lead.patrolHeldFor = patrol->GetGUID();
                        LOG_INFO("playerbots",
                                 "[dun217] {}: {} kept, its pull waits out the patrol {} ({:.0f} yd, reach {:.0f}): "
                                 "with it {}; alone {}",
                                 run.runId, mob ? mob->GetName() : std::string("?"), patrol->GetName(), patrolGap,
                                 patrolReach, judged, ownJudged);
                        Decisions().Write(run, "leader", "patrol_target", patrol->GetGUID(), patrol->GetName(), true,
                                          JsonObject().Str("choice", "wait out")
                                              .Str("pull", mob ? mob->GetName() : "?")
                                              .Str("reason", "no safe pull: with it " + judged + "; alone " +
                                                                 ownJudged + " [dun217]")
                                              .Real("overload", overload).Real("gap", patrolGap));
                    }
                }
                if (Leader::LEADER_PATROL_IS_TARGET && !fits && patrolGap <= 2.0f * patrolReach &&
                    !lead.commitIssued && broken.empty() && !patrolOwnRefused)
                {
                    lead.patrolTarget = patrol->GetGUID();
                    Decisions().Write(run, "leader", "patrol_target", patrol->GetGUID(), patrol->GetName(),
                                                  true,
                                                  JsonObject().Str("choice", "own pull")
                                                      .Str("pull", mob ? mob->GetName() : "?")
                                                      .Str("reason", "the combined fight is over the budget: " +
                                                                         judged)
                                                      .Real("overload", overload).Real("gap", patrolGap));
                    broken = "the patrol first: its own pull";
                    patrol = nullptr;
                }
                // [lead50] b99 rfc-h 109.2 s: a patrol 84.9 yd off was "taken along" (into the PullPack, exempt from
                // every idle guard) - along only when it can join, within the same twice its reach
                else if (Leader::LEADER_PATROL_IS_TARGET && fits && patrolGap <= 2.0f * patrolReach &&
                         lead.patrolDangerLoggedAlong != patrol->GetGUID())
                {
                    lead.patrolDangerLoggedAlong = patrol->GetGUID();
                    Decisions().Write(run, "leader", "patrol_target", patrol->GetGUID(), patrol->GetName(),
                                                  true,
                                                  JsonObject().Str("choice", "along")
                                                      .Str("pull", mob ? mob->GetName() : "?")
                                                      .Str("reason", (broughtByBoss
                                                                          ? "over the budget, the boss brings it: "
                                                                          : "the combined fight fits the budget: ") +
                                                                         judged)
                                                      .Real("overload", overload).Real("gap", patrolGap));
                }
                if (Leader::LEADER_PATROL_IS_TARGET && (!fits || patrolGap > 2.0f * patrolReach) &&
                    !(patrolOwnRefused && patrolGap <= 2.0f * patrolReach))  // [dun217] the window holds
                    patrol = nullptr;  // far off: no window, not along, the pull goes on
                if (patrol && fits)
                {
                    for (Creature* walker : walkers)
                        if (std::find(lead.takenWalkers.begin(), lead.takenWalkers.end(), walker->GetGUID()) ==
                            lead.takenWalkers.end())
                            lead.takenWalkers.push_back(walker->GetGUID());
                    if (Group* group = tank->GetGroup())
                    {
                        std::vector<ObjectGuid> mobs = LeaderPullMobs(lead.commit, lead.commitMob);
                        mobs.insert(mobs.end(), lead.takenWalkers.begin(), lead.takenWalkers.end());
                        Leader::PullPack::Set(group->GetGUID(), mobs);
                    }
                    patrol = nullptr;
                }
            }
            bool const windowOpen =
                !patrol || patrolGap > 2.0f * patrolReach || (beatHold && patrolGap >= beatFar - PATROL_BEAT_FAR_SLACK);
            if (broken.empty() && windowOpen != lead.patrolWindowOpen)
            {
                lead.patrolWindowOpen = windowOpen;
                Decisions().Write(run, "leader", "patrol_window", lead.commitMob,
                                              mob ? mob->GetName() : std::string(), true,
                                              JsonObject().Str("state", windowOpen ? "open" : "close")
                                                  .Str("patrol", patrol ? patrol->GetName() : "-")
                                                  .Real("gap", patrolGap).Real("reach", patrolReach));
            }
            // [lead30] a patrol walking into an issued pull no longer releases it (that looped forever): the planner's
            // walker rules took it along or pulled it first when it planned; the pull goes on, the patrol joins
            if (broken.empty() && lead.commitIssued && patrol &&
                patrolGap <= patrolReach + Leader::LEADER_PATROL_AREA_MARGIN && !lead.patrolJoinLogged)
            {
                lead.patrolJoinLogged = true;
                Decisions().Write(run, "leader", "patrol_joins", patrol->GetGUID(), patrol->GetName(),
                                              true,
                                              JsonObject().Str("pull", mob ? mob->GetName() : "?")
                                                  .Real("gap", patrolGap).Real("reach", patrolReach));
            }
            Leader::PullOrder order;
            bool const open = broken.empty() && lead.commitIssued && Leader::Orders::GetOpen(group.LeaderGuid(), order);
            // [lead49] user: no expiry and no re-issue of an order. The issued order waits in the tank's slot until
            // the tank takes it (the leader's own waits clear) or the leader replaces / cancels it on a change of
            // facts; taken without an order opened is such a fact
            if (broken.empty() && lead.commitIssued && !open)
            {
                PullPlanner::Decision waiting;
                if (lead.commitOrderSeen)
                    broken = "the order ended";
                else if (!PullPlanner::TakeIssued(group.LeaderGuid(), 0, waiting, false))
                    broken = "the tank took the order and opened none";
            }
            // [lead49] one reachability verdict, before the order goes out, from where the tank walks from now: the
            // pick's verdict while the tank still stands where it was judged, else judged once here (b97: the pick
            // passed at 94.8 s, the walk-in judged "detour (184 yd for 42 yd)" after the order went out)
            if (broken.empty() && !lead.commitIssued && mob && mob->ToCreature() && !IsBossLook(tank, mob->GetGUID()) &&
                mob->ToCreature()->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE &&
                mob->ToCreature()->GetDefaultMovementType() != RANDOM_MOTION_TYPE &&
                !(lead.pickJudgedFor == mob->GetGUID() &&
                  tank->GetExactDist(lead.pickJudgedAt) <= Leader::LEADER_COMMIT_MOVED_YD))
            {
                lead.pickJudgedFor = mob->GetGUID();
                lead.pickJudgedAt = tank->GetPosition();
                auto const routeVerdict = group.PathVerdicts().find(mob->GetGUID());
                bool const onRoute = mob->GetGUID() == group.RouteTarget() ||
                                     (routeVerdict != group.PathVerdicts().end() && routeVerdict->second.on);
                std::string verdict = LeaderWalkVerdict(tank, mob->ToCreature(), onRoute);
                // [lead70] one verdict: the tank's own walk-in guards, run dry from where it stands (the same code
                // that refuses "the tank's walk in for sight" after the order)
                if (verdict.empty() && committed.kind == "engage")
                    if (PlayerbotAI* walkerAI = GET_PLAYERBOT_AI(tank))
                        if (auto* walker = dynamic_cast<AggressiveTargetAction*>(
                                walkerAI->GetAiObjectContext()->GetAction("aggressive target"));
                            walker && !walker->DryWalkIn(mob))
                            verdict = "the tank's walk guards refuse the walk in";
                if (!verdict.empty())
                {
                    lead.unreachable[mob->GetGUID()] = LeaderUnreachable(tank, mob, verdict, lead.peaceSinceMs);
                    Decisions().Write(run, "leader", "pick_unreachable", mob->GetGUID(), mob->GetName(),
                                                  true,
                                                  JsonObject().Str("why", verdict).Str("pick", "before the order"));
                    broken = "the walk to it is " + verdict;
                }
            }
            // [lead101] b139 rfc-h 264.7-301.5 s: the engage on the patrol Ragefire Shaman low 38 (a mover: no walk
            // verdict above) walked the tank in through another pack's reach - Troggs 28 / 36 / 34 and Shamans 39 / 40
            // came, the patrol never joined and walked on, and the leader walked back ~105 yd for it later. A
            // patrol's walk-in is judged too: the tank's walk to where it walks now (the leader's route search)
            // against every idle pack's reach (the patrol and its own formation not judged); one it wakes is the
            // pull first
            // [lead103] b140 rfc-h 475.9-508.1 s: the boss engage on Taragaman walked the tank in past the Earthborer
            // pack (unplanned start, 8 mobs) - one check for EVERY engage (a still mob, a patrol, a boss): its walk-in
            // judged against every idle pack's reach, the plan's own mobs and the target's formation not judged
            // [lead104] the judgement is the leader's (Ai/Leader/Fight/PullFacts): the harness calls it and logs
            // [lead110] user: the woken pack is planned as part of the pull, not refused separately - the pull's
            // mobs, the walkers taken along and the woken mob's pack judged as one fight (PullPlanner::JudgePack, the
            // planner's estimate): it fits, the pull goes with them (taken along: PullPack, the walk guards let them
            // come); it does not, the woken pack is the pull first (as before)
            if (broken.empty() && !lead.commitIssued && mob && mob->ToCreature() && committed.kind == "engage")
            {
                std::vector<ObjectGuid> pullMobs = LeaderPullMobs(lead.commit, lead.commitMob);
                pullMobs.insert(pullMobs.end(), lead.takenWalkers.begin(), lead.takenWalkers.end());
                Unit* woken = Leader::Fight::WalkInWakes(tank, mob, pullMobs);
                // [dun228] a mob behind a closed door wakes from no walk on this side of it (b444 sfk-n five1-1: the
                // Tormented Officer commits released "the walk in to it wakes Wailing Guardsman", the Guardsman
                // patrolling the courtyard behind the closed Courtyard Door 18895)
                if (Creature* wokenCreature = woken ? woken->ToCreature() : nullptr;
                    wokenCreature && Leader::Travel::CagedBlocker(tank, wokenCreature))
                    woken = nullptr;
                if (woken)
                {
                    std::vector<ObjectGuid> wokenPack{woken->GetGUID()};
                    if (Creature* wokenMob = woken->ToCreature())
                    {
                        std::list<Creature*> pack;
                        Leader::Travel::PackMobCheck check{tank, wokenMob};
                        Acore::CreatureListSearcher<Leader::Travel::PackMobCheck> searcher(wokenMob, pack, check);
                        Cell::VisitObjects(wokenMob, searcher, Leader::Travel::PACK_RADIUS);
                        for (Creature* member : pack)
                            if (member != wokenMob)
                                wokenPack.push_back(member->GetGUID());
                    }
                    std::vector<ObjectGuid> combined = pullMobs;
                    combined.insert(combined.end(), wokenPack.begin(), wokenPack.end());
                    float overload = 0.0f;
                    std::string judged;
                    bool fits = PullPlanner::JudgePack(tank, combined, overload, judged);
                    // [lead168] b179 rfc-h 5C 389-508 s / 5G 264-381 s: the planner refused the woken Trogg's pack as
                    // "brings a boss, pulled as his fight", the walk in to that boss released him for it as "the pull
                    // first" - two verdicts on one fight. The planner's own refusal says it is his fight: one pull
                    if (auto const refused = lead.refusedPicks.find(woken->GetGUID());
                        !fits && IsBossLook(tank, mob->GetGUID()) && refused != lead.refusedPicks.end() &&
                        refused->second.find("brings a boss") != std::string::npos)
                    {
                        fits = true;
                        judged += "; the planner pulls it as the boss's fight";
                    }
                    // [lead180] b180 rfc-h 5F 1377.8-1609 s: the planner's boss plan said "boss pulled where it
                    // stands; the walk wakes Searing Blade Cultist: it joins his fight" (no plan takes that pack
                    // first, lead90), the walk-in check held the boss for the Cultist "not pullable first" until the
                    // run stalled. The plan already took the woken mob into the boss's fight: one pull
                    if (!fits && IsBossLook(tank, mob->GetGUID()) &&
                        committed.reason.find("the walk wakes " + woken->GetName() + ": it joins his fight") !=
                            std::string::npos)
                    {
                        fits = true;
                        judged += "; the boss plan takes it into his fight";
                    }
                    // [lead208] b182 rfc-h 5G 507-717 s: Taragaman released for the Searing Blade Cultist "the
                    // pull first", the Cultist refused (no safe pull), Taragaman held "not pullable first" - the run
                    // stalled. The same release again means the woken pack cannot be pulled first: the one option
                    // left, the pull with it taken along, goes (the cheapest of the options there are), no wait
                    if (!fits && lead.walkWakesFirst == woken->GetGUID() && lead.walkWakesReleased == mob->GetGUID())
                    {
                        auto const refused = lead.refusedPicks.find(woken->GetGUID());
                        auto const unreachable = lead.unreachable.find(woken->GetGUID());
                        fits = true;
                        judged += "; every option over the budget, the cheapest: taken along (not pullable first: " +
                                  (refused != lead.refusedPicks.end()      ? refused->second
                                   : unreachable != lead.unreachable.end() ? unreachable->second.why
                                                                           : std::string("not picked")) +
                                  ")";
                    }
                    bool const sameFact = !fits && lead.walkWakesFirst == woken->GetGUID() &&
                                          lead.walkWakesReleased == mob->GetGUID();  // logged once
                    if (!sameFact)
                        Decisions().Write(run, "leader", "walk_in_wakes", mob->GetGUID(), mob->GetName(), true,
                                          JsonObject().Str("wakes", woken->GetName())
                                              .Num("wakesLow", woken->GetGUID().GetCounter())
                                              .Real("distance", tank->GetExactDist(woken))
                                              .Num("wokenPack", int64(wokenPack.size())).Real("overload", overload)
                                              .Str("judged", judged)
                                              .Str("decision", fits ? "one pull: taken along" : "the pull first"));
                    if (fits)
                    {
                        for (ObjectGuid const& guid : wokenPack)
                            if (std::find(lead.takenWalkers.begin(), lead.takenWalkers.end(), guid) ==
                                lead.takenWalkers.end())
                                lead.takenWalkers.push_back(guid);
                        if (Group* tankGroup = tank->GetGroup())
                        {
                            std::vector<ObjectGuid> mobs = LeaderPullMobs(lead.commit, lead.commitMob);
                            mobs.insert(mobs.end(), lead.takenWalkers.begin(), lead.takenWalkers.end());
                            Leader::PullPack::Set(tankGroup->GetGUID(), mobs);
                        }
                    }
                    // [lead167] b179 rfc-h 5F: 99 releases of Oggleflint for "wakes Earthborer: it is the pull first"
                    // and Earthborer never pulled (its pick was skipped: off the route / refused). The woken mob is the
                    // next pick (on the path: the walk in crosses it); one release per fact - the same release again
                    // means it cannot be the pull first: [lead208] taken along above
                    else
                    {
                        lead.walkWakesFirst = woken->GetGUID();
                        lead.walkWakesReleased = mob->GetGUID();
                        broken = "the walk in to it wakes " + woken->GetName() + ": it is the pull first";
                    }
                }
            }
            if (!broken.empty())
            {
                Decisions().Write(run, "leader", "commit_released", lead.commitMob,
                                              mob ? mob->GetName() : std::string(), true,
                                              JsonObject().Str("why", broken).Str("kind", committed.kind)
                                                  .Real("sec", getMSTimeDiff(lead.commitSinceMs, now) / 1000.0f)
                                                  .Bool("issued", lead.commitIssued));
                // [lead1282] b250 rfc-h 5B-10 207 / 230 s: the orders on Ragefire Trogg 18 and Ragefire Shaman 16
                // ended with their mob out of the fight (74 / 101 yd off); it stayed the kept pick and the route's
                // target (the commit's mob), on the path by that alone, and the route walked 100 yd back for it. An
                // order that ended without its mob fighting leaves no verdict behind: the pick, the route's target
                // and its path verdict are judged again from scratch on the next look
                if (broken == "the order ended" && mob && mob->IsAlive() && !mob->IsInCombat())
                {
                    bool const routeTarget = group.Route().target == lead.commitMob && !group.Route().targetIsStarter;
                    if (lead.keptPick == lead.commitMob)
                        lead.keptPick.Clear();
                    if (lead.pickJudgedFor == lead.commitMob)
                        lead.pickJudgedFor.Clear();
                    group.Route().pathVerdicts.erase(lead.commitMob);  // "the route's target" by the commit
                    if (routeTarget)
                    {
                        group.Route().target.Clear();
                        group.ClearWaypoint(group.LeaderGuid());
                        group.RetargetAt(now);
                    }
                    Decisions().Write(run, "leader", "order_mob_rejudged", lead.commitMob, mob->GetName(), true,
                                      JsonObject().Real("distance", tank->GetExactDist(mob))
                                          .Bool("routeTarget", routeTarget)
                                          .Str("why", "[lead1282] its order ended with it out of the fight"));
                }
                lead.commit.reset();
                lead.commitIssued = false;
                lead.commitOrderSeen = false;
                lead.patrolWindowOpen = true;
                lead.groupBlockedBy.clear();  // [lead82] the pull's walk-alongs end with it
                PullPlanner::CancelIssued(group.LeaderGuid());  // [lead49] the replaced order leaves the slot
                if (Group* group = tank->GetGroup())
                    Leader::PullPack::Clear(group->GetGUID());
                lead.engageRefused.clear();
                lead.takenWalkers.clear();
                return false;
            }
            if (!lead.commitIssued)
            {
                std::string waitFor;
                bool const gate = Leader::LEADER_MEMBER_EVENTS && Leader::LEADER_ASSIGNS_SPOTS &&
                                  LeaderAssignSpots(group, tank, committed);
                if (gate)
                    waitFor = LeaderSpotsWait(group, tank);
                // [lead77] pull readiness includes positions: no order while a member trails or is blocked
                // [lead368] b200 rfc-h 5E 630.8-870 s: the committed engage of Searing Blade Enforcer 77's pack (8
                // mobs) waited "a member cannot follow" on Rtpriac / Rthunab / Rtlocab, whose walks along with the
                // tank would wake a Searing Blade Cultist - of that same pack. A block by a mob of this pull is
                // resolved by the pull itself (lead364 / lead365): only a block by a mob outside it holds the order
                // [lead369] b200 5G 632-862 s: Rtdekad's walk would wake a Searing Blade Cultist (and a Voidwalker
                // Minion) beside the committed Searing Blade Warlock - not in the pull's own list, but the same room:
                // "of this pull" is the pull's mobs and what comes with them by the game's rules - the pack rule
                // (SpawnsWith), a summon of one of them, the core's call for assistance (Creature::CallAssistance:
                // CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS, in sight, CanAssistTo). 5H 695.8 s: a los pull of the
                // blocking Cultist itself (its target)
                std::vector<ObjectGuid> pullGuids = {lead.commitMob, committed.lookTarget, committed.planTarget,
                                                     committed.attack};
                pullGuids.insert(pullGuids.end(), committed.pack.begin(), committed.pack.end());
                pullGuids.insert(pullGuids.end(), committed.verdictPack.begin(), committed.verdictPack.end());
                pullGuids.insert(pullGuids.end(), committed.losPack.begin(), committed.losPack.end());
                float const assistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
                auto const ofThisPull = [&](ObjectGuid const& blocker)
                {
                    if (blocker.IsEmpty())
                        return false;
                    Creature* mob = map->GetCreature(blocker);
                    ObjectGuid const owner = mob ? mob->GetCharmerOrOwnerOrOwnGUID() : blocker;
                    for (ObjectGuid const& guid : pullGuids)
                    {
                        if (guid.IsEmpty())
                            continue;
                        if (guid == blocker || guid == owner)
                            return true;
                        Creature* pulled = mob ? map->GetCreature(guid) : nullptr;
                        if (pulled && (PullPlanning::SpawnsWith(mob, pulled) ||
                                       (assistRadius > 0.0f && mob->IsWithinDistInMap(pulled, assistRadius) &&
                                        mob->CanAssistTo(pulled, tank) && mob->IsWithinLOSInMap(pulled))))
                            return true;
                    }
                    return false;
                };
                // [lead1370] the wait names its member (cannotFollow)
                if (waitFor.empty())
                    waitFor = cannotFollow([&ofThisPull](auto const& blocked) { return !ofThisPull(blocked.second); });
                if (waitFor.empty() && !windowOpen)
                    waitFor = Acore::StringFormat("the patrol window: {} {:.0f} yd (reach {:.0f})", patrol->GetName(),
                                                  patrolGap, patrolReach);
                // [lead1090] the pull of a boss, or one that brings him, waits for his buff pass (opened at once)
                if (waitFor.empty() && mob)
                    waitFor = bossPassHold(mob);
                if (Creature* boss = waitFor.empty() ? PullBoss(tank->GetMap(), mob, committed) : nullptr)
                    waitFor = bossPassHold(boss);
                // [lead270] a prepull Army's channel under way ends before the boss pull goes
                // [lead541] the boss the pull brings too (PullBoss)
                if (Creature* boss = waitFor.empty() ? PullBoss(tank->GetMap(), mob, committed) : nullptr)
                {
                    waitFor = Leader::Prepull::ArmyHolds(group, boss, false);
                    if (waitFor.empty())
                        waitFor = Leader::Prepull::StealthHolds(group, boss);  // [lead773]
                }
                if (!waitFor.empty())
                {
                    lead.spotsWaiting = gate;
                    // [lead1270] b250 rfc-h 5C-3 654.9-885 s: the Taragaman commit waited for the Army step with no
                    // decision line (the run's leader_spots_wait only): each new reason a commit waits on is logged
                    if (std::string const key = waitFor.substr(0, waitFor.find_first_of("0123456789"));
                        key != lead.commitWaitKey)
                    {
                        lead.commitWaitKey = key;
                        Decisions().Write(run, "leader", "commit_wait", lead.commitMob,
                                          mob ? mob->GetName() : std::string(), true,
                                          JsonObject().Str("why", waitFor).Str("member", waitMember)
                                              .Str("kind", committed.kind)
                                              .Real("sec", getMSTimeDiff(lead.commitSinceMs, now) / 1000.0f)
                                              .Str("tag", "[lead1270]"));
                    }
                    if (!lead.spotsLogMs || getMSTimeDiff(lead.spotsLogMs, now) >= 10 * IN_MILLISECONDS)
                    {
                        lead.spotsLogMs = now;
                        group.LogEvent("leader_spots_wait",
                                 JsonObject().Str("name", mob ? mob->GetName() : "?").Str("kind", committed.kind)
                                     .Str("waitFor", waitFor).Num("spots", int64(lead.spots.size()))
                                     .Bool("committed", true));
                    }
                    return true;
                }
                lead.spotsWaiting = false;
                lead.spotsIssuedMs = now;
                issue(committed, "committed: " + lead.commitWhy);
                lead.patrolJoinLogged = false;
                if (Group* group = tank->GetGroup())  // [lead29] the combined plan's every mob is the pull
                {
                    std::vector<ObjectGuid> mobs = LeaderPullMobs(lead.commit, lead.commitMob);
                    mobs.insert(mobs.end(), lead.takenWalkers.begin(), lead.takenWalkers.end());  // [lead32]
                    Leader::PullPack::Set(group->GetGUID(), mobs);
                }
                // [lead25] the meeting point of the tank and the pulled mob: an engage meets at the mob, a pull back /
                // los / range pull at the fight spot the mobs come to
                lead.meetPoint = committed.kind == "engage" && mob ? mob->GetPosition() : lead.issuedFight;
                lead.extraLogged.clear();
                Decisions().Write(run, "leader", "meeting_point", lead.commitMob,
                                              mob ? mob->GetName() : std::string(), true,
                                              JsonObject().Str("kind", committed.kind)
                                                  .Real("x", lead.meetPoint.GetPositionX())
                                                  .Real("y", lead.meetPoint.GetPositionY())
                                                  .Real("z", lead.meetPoint.GetPositionZ()));
                lead.commitIssued = true;
                lead.commitIssuedMs = now;
                return true;
            }
            if (open)
            {
                lead.commitOrderSeen = true;
                // [lead31b] the tank's executor carries the open order through its steps itself (no re-issue)
                return true;
            }
            return true;  // [lead49] the issued order waits in the slot (no re-issue)
        };
        if (decides && Leader::LEADER_COMMIT_PULL && runCommit())
            return;

        // [lead3] LEADER_NO_BODY_FALLBACK: a boss pull the leader decided (Steer, LeaderBossPull) is issued at once
        if (decides && Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && fresh(lead.bossPullMs))
        {
            Creature* boss = idleAlive(lead.bossPull);
            std::string const why = lead.bossPullWhy;
            lead.bossPull.Clear();
            lead.bossPullMs = 0;
            PullPlanner::Decision bossPull;
            // [lead45] b94 rfc-h 363-421 s: the boss pull was issued, then the planner held the same boss
            // ("Rtpalaa at 71 % mana"), the order stood without a target and the tank idled 50 s. The leader checks
            // the boss readiness (the planner's own BossReadyGap) BEFORE it issues; not ready: no order - the group
            // rests to its boss top-up line (the Rest commands) and the boss pull comes when it is ready
            waitMember.clear();  // [lead1370] the commit's wait above named its own
            std::string notReady = boss ? PullReadiness::BossNotReady(tank, boss) : std::string();
            if (boss && notReady.empty())
                notReady = bossPassHold(boss);  // [lead1090] his buff pass first (opened at once)
            // [lead77] b120 537.5 s: "boss pulled where it stands" with the group ~100 yd away
            if (boss && notReady.empty())  // [lead1370] the wait names its member (cannotFollow)
                notReady = cannotFollow([](auto const&) { return true; });
            if (boss && notReady.empty())  // [lead773] the rogue's pre-pull Stealth before the boss pull goes
                notReady = Leader::Prepull::StealthHolds(group, boss);
            if (boss && !notReady.empty())
            {
                lead.bossWaitFor = boss->GetGUID();  // [lead46] the members' Rest reads this wait
                if (lead.bossWaitWhy != notReady)
                {
                    lead.bossWaitWhy = notReady;
                    Decisions().Write(run, "leader", "boss_pull_waits", boss->GetGUID(), boss->GetName(),
                                                  true, JsonObject().Str("why", notReady).Str("member", waitMember));
                }
            }
            else if (boss && PullPlanner::BossPullDecision(tank, boss, why, bossPull))
            {
                lead.bossWaitWhy.clear();
                lead.bossWaitFor.Clear();
                if (lead.bossPullIssued != boss->GetGUID())
                {
                    lead.bossPullIssued = boss->GetGUID();
                    lead.bossPullIssuedMs = now;
                }
                issue(bossPull, "boss pull: " + why);
                return;
            }
        }

        // [lead3] .. [lead10] the leader's inputs to its planner: its pick and requests, readiness, doors, cleared
        PullPlanner::Snapshot snap(tankAI);
        std::string pickWhy;
        if (decides)
        {
            PullPlanner::LeaderInputs inputs;
            inputs.ready = !Leader::Leads(Leader::LEADER_ONE_READINESS) || lead.ready;
            inputs.notReadyWhy = lead.notReadyWhy;
            if (Leader::Leads(Leader::LEADER_DOOR_INPUTS) && group.DoorCutHold() && !group.DoorCutMember().IsEmpty())
                inputs.doorCut.push_back(group.DoorCutMember());
            if (Leader::Leads(Leader::LEADER_FIGHT_SPOT_COUNT))
                inputs.cleared = lead.cleared;
            for (auto it = lead.requiredMarks.begin(); it != lead.requiredMarks.end();)
            {
                if (getMSTimeDiff(it->second, now) > 10 * IN_MILLISECONDS)
                    it = lead.requiredMarks.erase(it);
                else
                {
                    inputs.required.push_back(it->first);
                    ++it;
                }
            }
            // [lead34] the patrol the leader chose as its own pull stays its pick until it is fought or dead
            if (Leader::LEADER_PATROL_IS_TARGET && !lead.patrolTarget.IsEmpty())
            {
                if (Creature* patrolMob = idleAlive(lead.patrolTarget))
                {
                    lead.intercept = patrolMob->GetGUID();
                    lead.interceptMs = now;
                    lead.interceptWhy = "patrol target: its own pull";
                }
                else
                    lead.patrolTarget.Clear();
            }
            Creature* const intercept = Leader::Leads(Leader::LEADER_ONE_PATROL_OWNER) && fresh(lead.interceptMs)
                                            ? idleAlive(lead.intercept)
                                            : nullptr;
            Creature* const whole = Leader::Leads(Leader::LEADER_NO_BODY_FALLBACK) && fresh(lead.wholeSeenMs)
                                        ? idleAlive(lead.wholePack)
                                        : nullptr;
            if (whole)
            {
                inputs.wholePack = whole->GetGUID();
                inputs.walkIn = lead.walkIn;
            }
            if (intercept)
                inputs.intercept = intercept->GetGUID();
            if (Leader::Leads(Leader::LEADER_ONE_PICKER))
            {
                // [lead41] b90 rfc-h 507-606 s: ~50 cycles of commit -> engage walk-in -> "detour (311 yd for 68
                // yd)" -> released -> the same mob picked again (refusedPicks is cleared on every Go, the edge pick
                // skipped no refusal). An unreachable verdict stays until its facts change - the tank or the mob moved
                // off where it was judged (LEADER_COMMIT_MOVED_YD, the commitment's own "moved" measure) - and every
                // pick source skips it
                // [lead41] user: «не пулить лишнее, если они не находятся на пути. Это должен лидер решать» - the
                // leader's on-the-path verdict (Retarget's route rules); off the path is never a pick
                auto const onPath = [&](ObjectGuid guid)
                {
                    auto const found = group.PathVerdicts().find(guid);
                    // the route's target, the patrol the leader pulls and the pack it must clear are the path's
                    // [lead167] and the mob the walk in to the pull wakes (the walk crosses it)
                    // [lead364] b197 rfc-h 5E 847-1095 s: Rtlocab / Rthunab's walk along with the tank was refused
                    // ("the walk would wake Searing Blade Enforcer"), lead77 made the Enforcer the pick - and this
                    // verdict, "off the route", dropped it: "every pick here is refused or unreachable: the route
                    // walk goes on", while the route walk waits for the blocked members (lead77) and the patrol
                    // pull halts it. The mob that blocks the group's walk is on the group's path by that fact
                    bool const blocksGroup =
                        std::any_of(lead.groupBlockedBy.begin(), lead.groupBlockedBy.end(),
                                    [&guid](auto const& blocked) { return blocked.second == guid; });
                    bool const on = guid == group.RouteTarget() || guid == lead.intercept || guid == lead.wholePack ||
                                    guid == lead.walkWakesFirst || blocksGroup ||
                                    (found != group.PathVerdicts().end() && found->second.on);
                    auto const logged = group.PathVerdictLogged().find(guid);
                    if (found != group.PathVerdicts().end() &&
                        (logged == group.PathVerdictLogged().end() || logged->second != on))
                    {
                        group.PathVerdictLogged()[guid] = on;
                        Creature* mob = map->GetCreature(guid);
                        std::string const why = guid == lead.intercept   ? std::string("the patrol the leader pulls")
                                                : guid == lead.wholePack ? std::string("the pack the route must clear")
                                                : guid == lead.walkWakesFirst
                                                    ? std::string("the walk in to the pull wakes it")
                                                : blocksGroup ? std::string("it blocks the group's walk")  // [lead364]
                                                              : found->second.why;
                        JsonObject verdict;
                        verdict.Str("verdict", on ? "on_path" : "off_path")
                            .Str("why", why)
                            .Real("routeDist", found->second.routeDist)
                            .Real("reach", found->second.reach);
                        // [lead1331] a wanderer's route distance is its wander area's (BlocksRoute)
                        if (Leader::Fight::WanderArea area; Leader::Fight::WanderAreaOf(mob, area))
                            verdict.Real("wanderYd", area.radius);
                        Decisions().Write(run, "leader", "path_verdict", guid,
                                                      mob ? mob->GetName() : std::string("?"), true, verdict);
                    }
                    return on;
                };
                auto const skipPick = [&](ObjectGuid guid)
                {
                    if (!onPath(guid))
                        return true;
                    if (lead.refusedPicks.count(guid))
                        return true;
                    auto const found = lead.unreachable.find(guid);
                    if (found == lead.unreachable.end())
                        return false;
                    Creature const* mob = map->GetCreature(guid);
                    if (std::string const changed =
                            LeaderUnreachableChanged(found->second, tank, mob, lead.peaceSinceMs);
                        !changed.empty())
                    {
                        Decisions().Write(run, "leader", "unreachable_released", guid,
                                          mob ? mob->GetName() : std::string("?"), true,
                                          JsonObject().Str("why", changed).Str("verdict", found->second.why)
                                              .Bool("boss", mob && (mob->IsDungeonBoss() || mob->isWorldBoss())));
                        lead.unreachable.erase(found);
                        return false;
                    }
                    return true;
                };
                // [lead96] a route target the route skips because the leader refused it comes back the moment the
                // refusal ends (the same facts as skipPick's)
                for (auto skipped = lead.routeSkips.begin(); skipped != lead.routeSkips.end();)
                {
                    Creature const* mob = map->GetCreature(*skipped);
                    if (!mob || !mob->IsAlive() || !skipPick(*skipped))
                    {
                        group.ReleaseSkip(*skipped);
                        skipped = lead.routeSkips.erase(skipped);
                    }
                    else
                        ++skipped;
                }
                // [lead5] the leader's pick: a patrol it decided to pull, the pack it must clear, else the same
                // choice the tank's own value makes (AggressiveTargetValue::PickFor) with the leader's route target
                // [lead43] b92 rfc-h 387-485 s: the patrol pick (held for pre-pull cc, refused) shadowed every other
                // pick - "every pick here is refused" while the route stood at Trogg 29's stop. A refused / unreachable
                // patrol or whole-pack pick falls through to the next pick (the route target first)
                // [lead77] the mob that blocks a member's walk with the tank is on the path: the next pull
                Creature* blocker = nullptr;
                for (auto const& [blockedMember, blockingMob] : lead.groupBlockedBy)
                    if (!blockingMob.IsEmpty())
                        if (Creature* mob = idleAlive(blockingMob); mob && !IsBossLook(tank, mob->GetGUID()))
                            blocker = mob;
                // [lead101] the pack the walk to a patrol wakes is the pull first (released when it is dead or fights)
                Creature* wakesFirst = lead.walkWakesFirst.IsEmpty() ? nullptr : idleAlive(lead.walkWakesFirst);
                if (!wakesFirst)
                {
                    lead.walkWakesFirst.Clear();
                    lead.walkWakesReleased.Clear();
                }
                // [lead365] the blocker is a pack the leader must clear (the whole-pack input): with every option
                // over the budget the planner takes the cheapest (lead208), never a refusal that leaves the group
                // blocked and the route walk waiting for it (lead77). Unreachable, it falls through (lead43)
                if (blocker && !skipPick(blocker->GetGUID()))
                {
                    inputs.pick = blocker->GetGUID();
                    inputs.wholePack = blocker->GetGUID();
                    inputs.walkIn = false;
                    pickWhy = "it blocks the group's walk ([lead208] every option over the budget, the cheapest: the "
                              "blocker)";
                }
                // [lead503] b210 rfc-h 5F 120.2-130.6 s: the patrol rule broke the walk-wakes-first Shaman commit
                // 11 times ("the patrol first: its own pull", the Shaman + the patrol Trogg over the budget) and the
                // next look picked the Shaman again ahead of the patrol - 11 s of loss, a commit / release flip every
                // second. The patrol the leader chose as its own pull is the pick; the pack the walk to it wakes the
                // planner puts first with the patrol in the plan (131.3 s: "the way to Ragefire Trogg wakes Ragefire
                // Shaman: Ragefire Shaman is the pull", committed)
                else if (intercept && intercept->GetGUID() == lead.patrolTarget && !skipPick(intercept->GetGUID()))
                {
                    inputs.pick = intercept->GetGUID();
                    pickWhy = "patrol pull: " + lead.interceptWhy;
                }
                else if (wakesFirst && !skipPick(wakesFirst->GetGUID()))
                {
                    inputs.pick = wakesFirst->GetGUID();
                    pickWhy = "the walk in to the next pull wakes it: the pull first";
                }
                else if (intercept && !skipPick(intercept->GetGUID()))
                {
                    inputs.pick = intercept->GetGUID();
                    pickWhy = "patrol pull: " + lead.interceptWhy;
                }
                else if (whole && !skipPick(whole->GetGUID()))
                {
                    inputs.pick = whole->GetGUID();
                    pickWhy = "whole pack: " + lead.wholeWhy;
                }
                // [lead39] b88 rfc-h 0-24 s: the pick flipped every second between two Earthborers and a Molten
                // Elemental - the room's edge was recomputed from wandering mobs. The leader keeps its pick (a
                // commitment of the pick) until it is dead, in a fight, out of reach or refused by the planner
                else if (Creature* kept = lead.keptPick.IsEmpty() ? nullptr : idleAlive(lead.keptPick);
                         kept && !skipPick(kept->GetGUID()) && tank->GetExactDist(kept) <= 90.0f &&
                         std::fabs(kept->GetPositionZ() - tank->GetPositionZ()) < 15.0f)
                {
                    inputs.pick = kept->GetGUID();
                    pickWhy = "kept pick: " + lead.keptPickWhy;
                }
                else
                {
                    lead.keptPick.Clear();
                    static std::unordered_map<uint32, TargetReachability> reach;  // party seq (world thread)
                    ObjectGuid const route = group.RouteTargetIsStarter() ? ObjectGuid::Empty : group.RouteTarget();
                    // [lead33] the leader's own pick: the route target unless the planner refused / held it, else the
                    // nearest idle mob the planner did not refuse (no tank pick)
                    Unit* leaderPick = nullptr;
                    bool plannedPick = false;  // [lead204]
                    if (Leader::LEADER_NO_FORCING_TIMERS)
                    {
                        Unit* routeMob = route.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, route);
                        if (routeMob && routeMob->IsAlive() && !routeMob->IsInCombat() &&
                            !skipPick(route))
                            leaderPick = routeMob;
                        else
                        {
                            std::list<Creature*> idle;
                            IdleAggroCheck check{tank};
                            Acore::CreatureListSearcher<IdleAggroCheck> searcher(tank, idle, check);
                            Cell::VisitObjects(tank, searcher, 60.0f);
                            // [lead204] the leader's planned order first (its earliest stop), the nearest after it
                            auto const planRank = [&group](ObjectGuid guid)
                            {
                                auto const& plan = group.Route().routePlan;
                                for (size_t i = 0; i < plan.size(); ++i)
                                    if (std::find(plan[i].mobs.begin(), plan[i].mobs.end(), guid) != plan[i].mobs.end())
                                        return i;
                                return plan.size();
                            };
                            float best = 0.0f;
                            size_t bestRank = 0;
                            for (Creature* mob : idle)
                            {
                                if (skipPick(mob->GetGUID()) ||
                                    std::fabs(mob->GetPositionZ() - tank->GetPositionZ()) >= 8.0f)
                                    continue;
                                size_t const rank = planRank(mob->GetGUID());
                                if (!leaderPick || rank < bestRank ||
                                    (rank == bestRank && tank->GetExactDist(mob) < best))
                                {
                                    leaderPick = mob;
                                    best = tank->GetExactDist(mob);
                                    bestRank = rank;
                                }
                            }
                            plannedPick = leaderPick && bestRank < group.Route().routePlan.size();
                            // [lead860] b224 rfc-h 5F 580.7-811 s: the route target Taragaman (a boss: never skipped)
                            // was unreachable from the tank's walk, no idle mob within the look's 60 yd was left, and
                            // the leader picked no mob for 230 s ("the route target out of the pick") with three
                            // bosses alive. A route target out of the pick leads to another target: the route plan's
                            // next stop the leader does not refuse, at any distance
                            if (!leaderPick && routeMob && routeMob->IsAlive())
                                for (auto const& stop : group.Route().routePlan)
                                {
                                    for (ObjectGuid const& guid : stop.mobs)
                                        if (Creature* mob = idleAlive(guid); mob && !skipPick(guid))
                                        {
                                            leaderPick = mob;
                                            break;
                                        }
                                    if (leaderPick)
                                    {
                                        plannedPick = true;
                                        break;
                                    }
                                }
                        }
                    }
                    Unit* ownPick =
                        leaderPick ? nullptr : AggressiveTargetValue::PickFor(tankAI, route, reach[group.Run().runSeq]);
                    if (ownPick && skipPick(ownPick->GetGUID()))  // [lead41] off the path: no pick
                        ownPick = nullptr;
                    if (Unit* picked = leaderPick ? leaderPick : ownPick)
                    {
                        inputs.pick = picked->GetGUID();
                        pickWhy = picked->GetGUID() == group.RouteTarget() ? "route target"
                                  : plannedPick                            ? "planned order"
                                                                           : "nearest pick";
                        // [lead18] LEADER_EDGE_FIRST: a room of linked packs is pulled from its edge inward
                        if (Leader::LEADER_EDGE_FIRST)
                        {
                            std::string edgeWhy;
                            std::string sig;
                            JsonArray order;
                            Unit* edge = LeaderEdgePick(tank, picked, edgeWhy, order, sig);
                            if (!sig.empty() && sig != lead.roomSig)
                            {
                                lead.roomSig = sig;
                                Decisions().Write(run, "leader", "room_order", picked->GetGUID(),
                                                              picked->GetName(), true,
                                                              JsonObject().Raw("packs", order.Build())
                                                                  .Str("edge", edge ? edge->GetName() : "-")
                                                                  .Str("why", edge ? edgeWhy
                                                                                   : "the pick is on the edge"));
                            }
                            if (edge && !skipPick(edge->GetGUID()))  // [lead41] never a refused / unreachable edge
                            {
                                inputs.pick = edge->GetGUID();
                                pickWhy = "edge first: " + edgeWhy;
                            }
                        }
                    }
                    else
                        pickWhy = group.RouteTarget().IsEmpty() ? "no route target"
                                                                : "the route target out of the pick";
                    if (!inputs.pick.IsEmpty())
                    {
                        lead.keptPick = inputs.pick;
                        lead.keptPickWhy = pickWhy;
                    }
                }
                // [lead40] b89 rfc-h 54-123 s: the kept pick Earthborer was "skip: detour (511 yd for 84 yd)" by the
                // leader's own skip_target, yet planned, committed and walked at (the tank stood 55 s). A new pick is
                // judged by the same detour metric before the planner sees it: a detour / unreachable mob is refused
                // (refusedPicks, the kept pick released) - never planned nor committed. A boss is always walked to.
                if (!inputs.pick.IsEmpty() && skipPick(inputs.pick))
                {
                    if (lead.keptPick == inputs.pick)
                        lead.keptPick.Clear();
                    inputs.pick.Clear();
                    pickWhy = "every pick here is refused or unreachable: the route walk goes on";
                }
                // [lead41] judged again once the tank moved off where it was judged (the verdict's facts changed)
                if (!inputs.pick.IsEmpty() &&
                    (inputs.pick != lead.pickJudgedFor ||
                     tank->GetExactDist(lead.pickJudgedAt) > Leader::LEADER_COMMIT_MOVED_YD))
                {
                    lead.pickJudgedFor = inputs.pick;
                    lead.pickJudgedAt = tank->GetPosition();
                    Creature* picked = map->GetCreature(inputs.pick);
                    // [lead42] b91 149.6 s: the patrol Shaman coming to the group was refused "detour (177 yd for
                    // 38 yd)" - a patrol / wanderer walks to the group itself: no detour verdict for it
                    bool const walksItself = picked && (picked->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE ||
                                                        picked->GetDefaultMovementType() == RANDOM_MOTION_TYPE);
                    std::string const verdict = picked && !walksItself && !picked->IsDungeonBoss() &&
                                                        !picked->isWorldBoss() && !picked->IsInCombat()
                                                    ? LeaderWalkVerdict(tank, picked, onPath(inputs.pick))
                                                    : std::string();
                    if (!verdict.empty())
                    {
                        lead.unreachable[inputs.pick] = LeaderUnreachable(tank, picked, verdict, lead.peaceSinceMs);
                        Decisions().Write(run, "leader", "pick_unreachable", inputs.pick, picked->GetName(),
                                                      true,
                                                      JsonObject().Str("why", verdict).Str("pick", pickWhy));
                        if (lead.keptPick == inputs.pick)
                            lead.keptPick.Clear();
                        inputs.pick.Clear();
                        pickWhy = "the pick is " + verdict;
                    }
                }
                // [lead43] one verdict: a route target the leader does not pick is a contradiction - logged with the
                // store that refuses it (the route then has no planner to stop for)
                if (inputs.pick.IsEmpty() && !group.RouteTarget().IsEmpty() &&
                    lead.pickConflictFor != group.RouteTarget())
                    if (Creature* routeMob = idleAlive(group.RouteTarget()))
                    {
                        lead.pickConflictFor = group.RouteTarget();
                        // [lead96] b136 rfc-h: 46 s of "the leader picks no mob (the route target out of the pick / no
                        // route target)" with the tank at the route's stop ("no route point") - the leader refused the
                        // route target (unreachable, off the path, refused) and the route stood waiting for its
                        // planner. Not a boss (bosses are always walked to): the route skips it, the group walks on
                        // to the next
                        // [lead96] user: no elapsed-time rule - the skip has no time: it lasts until the leader's
                        // refusal ends by a fact (released below with lead.unreachable / refusedPicks: the tank or
                        // the mob moved, the fight changed the room, the leader picks it again)
                        // [lead108] the store is read before the skip (SkipTarget clears the route target: b141 logged
                        // "none" for a route target refusedPicks held)
                        ObjectGuid const conflicted = group.RouteTarget();
                        auto const unreachable = lead.unreachable.find(conflicted);
                        auto const refused = lead.refusedPicks.find(conflicted);
                        std::string const store =
                            refused != lead.refusedPicks.end()       ? "refusedPicks: " + refused->second
                            : unreachable != lead.unreachable.end() ? "unreachable: " + unreachable->second.why
                            : !onPath(conflicted)                   ? std::string("off the path")
                                                                    : std::string("none (" + pickWhy + ")");
                        if (!IsBossLook(tank, group.RouteTarget()) && !routeMob->IsDungeonBoss())
                        {
                            constexpr uint32 ROUTE_SKIP_UNTIL_RELEASED = 0x3FFFFFFF;  // no time: released by facts
                            lead.routeSkips.insert(group.RouteTarget());
                            group.SkipTarget(group.RouteTarget(), ROUTE_SKIP_UNTIL_RELEASED,
                                       "the leader does not pick it (" + pickWhy + ")");
                        }
                        Decisions().Write(run, "leader", "pick_route_conflict", conflicted,
                                                      routeMob->GetName(), true,
                                                      JsonObject().Str("store", store).Str("pick", pickWhy));
                    }
                if (!inputs.pick.IsEmpty())
                    lead.pickConflictFor.Clear();
                // [lead1090] the leader's pick that is a boss, or brings one, waits for his buff pass (opened at
                // once); any other pick is not held by a boss's pass
                if (!inputs.pick.IsEmpty() && !tank->IsInCombat())
                    if (std::string const passHold = bossPassHold(map->GetCreature(inputs.pick)); !passHold.empty())
                    {
                        inputs.ready = false;
                        inputs.notReadyWhy = passHold;
                    }
                inputs.pickWhy = pickWhy;
                // [lead253] the off-path verdicts for the planner (onPath's rule, without its log)
                for (auto const& [mobGuid, verdict] : group.PathVerdicts())
                    if (!verdict.on && mobGuid != group.RouteTarget() && mobGuid != lead.intercept &&
                        mobGuid != lead.wholePack && mobGuid != lead.walkWakesFirst)
                        inputs.offPath.push_back(mobGuid);
            }
            snap.BindLeader(inputs);
        }

        PullPlanner::Decision decision;
        if (!PullPlanner::Decide(tank, snap, false, decision))
            return;
        // [lead33] the leader's ONE target: its plan's mob (the route follows it, Steer)
        if (decides && decision.verdict == PullPlanner::Verdict::Go)
        {
            lead.planTarget = !decision.planTarget.IsEmpty() ? decision.planTarget
                              : !decision.attack.IsEmpty()   ? decision.attack
                                                             : decision.lookTarget;
            lead.planMobs = decision.pack;
            lead.planMobs.insert(lead.planMobs.end(), decision.verdictPack.begin(), decision.verdictPack.end());
        }
        // [lead33] the planner's hold or refusal of the leader's pick is a reason, not a wait: the pick is skipped and
        // the next look asks for the next target at once - unless the hold is the group's justified rest / readiness
        if (decides && Leader::LEADER_NO_FORCING_TIMERS && !decision.lookTarget.IsEmpty() &&
            (decision.verdict == PullPlanner::Verdict::Refused ||
             (decision.verdict == PullPlanner::Verdict::Held && lead.ready)) &&
            !lead.refusedPicks.count(decision.lookTarget) && !IsBossLook(tank, decision.lookTarget) &&
            // [lead73] b117 266-271 s: "paladin tank at 26 % mana" put every on-path mob into refusedPicks and the
            // route walked past them - a member's mana / health is readiness (its Rest), never a refusal of the mob
            decision.why.find("% mana") == std::string::npos && decision.why.find("% health") == std::string::npos &&
            // [lead108] b141 rfc-h 116.3 s: "held for now: the tank rests (health 100 %, mana 48 %)" refused the route
            // target Earthborer 1, the route skipped it for good and every mob behind it - a readiness hold
            // (Decision::readinessHold) is the group's wait, never a refusal of the mob
            !decision.readinessHold &&
            // [lead621] b218 rfc-h 5E 399.6 s: "held for pre-pull cc" on the route target Ragefire Trogg 18 went into
            // refusedPicks, the route skipped it (pick_route_conflict "refusedPicks: held for pre-pull cc") and the
            // tank walked 83 yd north to Earthborer 60, then 82 yd back south for the patrol Shaman and 97 yd north
            // again (pulls 17-20: 313 yd for ~160 yd of ground; the same store in 5A 323.6, 5D 383.5, 5F 1117.0 s).
            // The crowd control is the first step of this mob's own pull (5F 899.2 -> 900.9 s, 969.3 -> 970.3 s: the
            // plan followed the hold), never a refusal of the mob
            decision.why.find("pre-pull cc") == std::string::npos &&
            // [lead365] never the mob that blocks the group's walk: it is the pull the walk waits for (lead208)
            std::none_of(lead.groupBlockedBy.begin(), lead.groupBlockedBy.end(),
                         [&decision](auto const& blocked) { return blocked.second == decision.lookTarget; }))
            lead.refusedPicks[decision.lookTarget] = decision.why;  // [lead45] never a boss
        // [lead50] b99 rfc-h 543-608 s: the planner held the boss pick for "Rtpalaa at 76 % mana" and no Rest came
        // (lead46's boss Rest ran only while the boss-pull gate waited). One readiness path: a held answer that
        // names a member below a line ("<member> at N % mana / health") is the leader's Rest for that member,
        // until the answer changes
        if (decides)
        {
            std::string restFor;
            bool const lineHold = decision.why.find("% mana") != std::string::npos ||
                                  decision.why.find("% health") != std::string::npos;
            // [lead82] b125 rfc-h 526.8-543.4 s: the answer flipped "Rthunaa at 56 % mana (needs 68 %)" <-> "the
            // leader: Rthunaa sits" and the Rest with it - Rthunaa stood up and sat down again 6 times on the way to
            // its line. The hold on the member's own rest is the same wait: its Rest goes on
            // [lead514] b210 rfc-h 5E 1030.4-1040.4 s: the boss hold "boss pull: Jergosh the Invoker comes with it,
            // Rtpalac at 61 % mana (needs 83 %)" put the tank under Rest with its 83 % line; the planner's next look
            // read that Rest as "held for now: the tank rests (health 100 %, mana 61 %)" - no member name, no line:
            // the Rest's member and line were dropped, the tank's own line was reached and the leader told it Prepare
            // ("its rest line is reached" at 55 / 61 / 66 %), the boss hold the Rest again - 7 Rest / Prepare flips
            // in 7 s. The planner's "the tank rests" while the leader's Rest is on the tank is that same wait
            bool const restHold = !lead.plannerRestFor.empty() &&
                                  (decision.why.find(lead.plannerRestFor + " sits") != std::string::npos ||
                                   decision.why.find(lead.plannerRestFor + " rests") != std::string::npos ||
                                   (lead.plannerRestFor == tank->GetName() &&
                                    decision.why.find("the tank rests") != std::string::npos));
            float restLine = -1.0f;
            if (decision.verdict == PullPlanner::Verdict::Held && restHold)
            {
                restFor = lead.plannerRestFor;
                restLine = lead.plannerRestLine;
            }
            else if (decision.verdict == PullPlanner::Verdict::Held && lineHold)
            {
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (decision.why.find(member.name + " at ") != std::string::npos)
                    {
                        restFor = member.name;
                        break;
                    }
                // [lead73] the planner's tank lines name it "tank" ("paladin tank at 26 % mana", "the tank rests")
                if (restFor.empty() && decision.why.find("tank") != std::string::npos)
                    restFor = tank->GetName();
                // [lead221] b185 rfc-h 5H 753-776 s: "Rtpriad at 14 % mana (needs 15 %)" - the Rest held while the
                // answer named it, the answer turned to "the leader: Rtpriad sits" (its Rest goes on, lead82) and the
                // priest drank to 100 %. The Rest ends at the line the answer named
                if (!restFor.empty())
                    restLine = NamedManaLine(decision.why, restFor);
                if (restLine < 0.0f && restFor == tank->GetName())
                    restLine = NamedManaLine(decision.why, "tank");
            }
            lead.plannerRestLine = restLine;
            // [lead76] a health gap is the healer's "top up X", not a Rest of the member (b118: 15 s for 94 -> 95 %)
            lead.topUpFor.Clear();
            if (!restFor.empty() && decision.why.find("% health") != std::string::npos)
                for (LeaderGroup::MemberInfo const& member : group.Members())
                    if (member.name == restFor)
                        lead.topUpFor = member.guid;
            // [lead85] b128 rfc-h 756.6-772.6 / 1083-1102.6 s: before a boss the planner named one member at a time
            // ("Rtshaab at 49 %", then "Rthunaa at 67 %") and they drank one after the other. A line hold on a boss is
            // the boss wait: every member below its boss line rests at once (MemberBossGap, lead46)
            if (decision.verdict == PullPlanner::Verdict::Held && lineHold && !decision.lookTarget.IsEmpty() &&
                IsBossLook(tank, decision.lookTarget))
                lead.bossWaitFor = decision.lookTarget;
            // [lead514] a trash pull that brings a boss is his pull: the planner's hold "boss pull: <boss> comes with
            // it, <member> at N % mana (needs M %)" is the wait for that boss, every member below its boss line rests
            // at once (lead85), the tank's Rest holds to its boss line (MemberBossGap)
            else if (decision.verdict == PullPlanner::Verdict::Held && lineHold &&
                     decision.why.rfind("boss pull: ", 0) == 0)
                for (ObjectGuid const& bossGuid : group.BossCreatures())
                    if (Creature* boss = tank->GetMap()->GetCreature(bossGuid);
                        boss && boss->IsAlive() && !boss->IsInCombat() &&
                        decision.why.compare(std::strlen("boss pull: "), boss->GetName().size(), boss->GetName()) == 0)
                    {
                        lead.bossWaitFor = bossGuid;
                        break;
                    }
            // [lead1260] b249 rfc-h (16 runs) took lead1250's early wait out again: a readiness hold (buff pass, a
            // member sitting, a HoT) on a look that is or brings a boss started his wait at once - 39 boss-line Rests
            // (252 s) began under such holds against 8 (46 s) in b248, 12 of them (108 s) before a trash pull the
            // planner put ahead of the boss (his escort or a patrol taken first: 5A-1 / 5E-5 Searing Blade Enforcer
            // before Taragaman): the member drank to its boss line, spent it on the trash and sat again (5B-2 Rtshaac
            // 32 -> 100 % before Bazzalan, then 77 -> 100 %). The wait starts where the planner reads the boss lines
            if (restFor != lead.plannerRestFor)
            {
                lead.plannerRestFor = restFor;
                if (!restFor.empty())
                    Decisions().Write(run, "leader", "readiness_rest", ObjectGuid(), restFor, true,
                                                  JsonObject().Str("member", restFor).Str("why", decision.why));
            }
        }
        if (decision.verdict == PullPlanner::Verdict::Go || group.InCombat())
            lead.refusedPicks.clear();  // a pull goes / a fight changes the room: everything is asked again
        // [lead30] planner_answer: the planner's answer for the leader's pick (logged when it changes)
        if (decides)
        {
            Unit* answered =
                decision.lookTarget.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, decision.lookTarget);
            std::string const verdictName = decision.verdict == PullPlanner::Verdict::Go        ? "plan"
                                            : decision.verdict == PullPlanner::Verdict::Held    ? "held"
                                            : decision.verdict == PullPlanner::Verdict::Refused ? "refused"
                                                                                                : "none";
            std::string const sig = verdictName + ":" + std::to_string(decision.lookTarget.GetRawValue()) + ":" +
                                    decision.kind + ":" + decision.why.substr(0, 60);
            if (sig != lead.answerSig)
            {
                lead.answerSig = sig;
                Decisions().Write(run, "leader", "planner_answer", decision.lookTarget,
                                              answered ? answered->GetName() : std::string("-"), true,
                                              JsonObject().Str("answer", verdictName).Str("kind", decision.kind)
                                                  .Str("why", decision.why).Str("pick", pickWhy)
                                                  .Str("routeTarget", group.RouteTarget().IsEmpty() ? "-" :
                                                                          group.RouteTarget().ToString()));
            }
        }

        // [lead5] the leader's own verdict memory (Steer reads it: PartyRefusal / PartyVerdict) and its aoe plan
        if (decides && decision.hasVerdict)
        {
            lead.verdictMs = now;
            lead.verdictTarget = decision.verdictTarget;
            lead.verdictPack = decision.verdictPack;
            lead.verdictAccepted = decision.verdictAccepted;
            lead.verdictMobs = decision.verdictMobs;
            lead.verdictHopeless = decision.verdictHopeless;
            lead.verdictWaitMs = decision.verdictWaitMs;
            lead.verdictWhy = decision.verdictWhy;
        }
        if (decides && !decision.aoePlan.empty())
        {
            if (lead.aoePlan != decision.aoePlan || !fresh(lead.aoePlanMs))
                lead.aoePlanSinceMs = now;
            lead.aoePlan = decision.aoePlan;
            lead.aoePlanMs = now;
        }

        Unit* const lookMob =
            decision.lookTarget.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, decision.lookTarget);
        auto const packOf = [&decision]()
        {
            std::vector<ObjectGuid> pack = decision.pack.empty() ? decision.verdictPack : decision.pack;
            if (!decision.lookTarget.IsEmpty() &&
                std::find(pack.begin(), pack.end(), decision.lookTarget) == pack.end())
                pack.push_back(decision.lookTarget);
            return pack;
        };
        // [lead12] the boss a look's pack brings (a dungeon boss or a tracked encounter boss, idle): the planner
        // never pulls him, his pull is the leader's (BossPull at its stop, as the old ROUTE_BOSS_PULL_IMMEDIATE)
        auto const packBoss = [&]() -> Creature*
        {
            std::vector<ObjectGuid> pack = decision.pack.empty() ? decision.verdictPack : decision.pack;
            pack.push_back(decision.lookTarget);
            for (ObjectGuid const& guid : pack)
                if (Creature* creature = idleAlive(guid);
                    creature && (creature->IsDungeonBoss() || group.BossCreatures().count(guid)))
                    return creature;
            return nullptr;
        };
        bool const requestCovers = decides && !lead.wholePack.IsEmpty() && fresh(lead.wholeSeenMs) &&
                                   (decision.lookTarget == lead.wholePack ||
                                    std::find(decision.pack.begin(), decision.pack.end(), lead.wholePack) !=
                                        decision.pack.end());

        if (decision.verdict == PullPlanner::Verdict::Go)
        {
            lead.heldReadyMob.Clear();
            lead.heldReadySinceMs = 0;
            // [lead5] an attack of the leader's own pick names its mob (the tank's aggressive target may differ)
            if (decides && Leader::Leads(Leader::LEADER_ONE_PICKER) && decision.kind == "attack" &&
                decision.attack.IsEmpty())
                decision.attack = decision.lookTarget;

            // [lead10] LEADER_DOOR_INPUTS: a pull whose walk (the tank to its stand spot, the stand to the fight
            // spot, the tank to the group's gather) crosses a closed door is no valid order: not issued
            if (decides && Leader::Leads(Leader::LEADER_DOOR_INPUTS))
            {
                Position const here = tank->GetPosition();
                Position stand = here;
                if (decision.side)
                    stand = decision.sideSpot;
                else if (decision.los)
                    stand = decision.losSpot;
                else if (decision.hasCast)
                    stand = decision.castSpot;
                else if (Unit* engaged = ObjectAccessor::GetUnit(
                             *tank, !decision.planTarget.IsEmpty() ? decision.planTarget : decision.attack))
                    stand = engaged->GetPosition();
                Position fightAt = stand;
                if (!decision.killFor.IsEmpty() && decision.killFor == decision.planTarget)
                    fightAt = decision.kill;
                else if (!decision.fightFor.IsEmpty() && decision.fightFor == decision.planTarget)
                    fightAt = decision.fight;
                G3D::Vector3 doorAt;  // [lead11] DOOR_OPEN_BACK before the door on the crossing walk
                auto const crosses = [&](Position const& from, Position const& to) -> GameObject*
                {
                    // [sfk241] any closed door gates the check, not one within 5 yd of the straight line: the
                    // route from the tank can run far off it (b445 sfk-n five5-5 186.7 s: the engage on the
                    // ramparts' Tormented Officer 44 yd off, 647 yd round through the closed Courtyard Door 18895
                    // 50 yd off that line, went out; the tank walked through it alone and died in the courtyard)
                    if (from.GetExactDist(to) < 1.0f || !AnyClosedDoor(tank))
                        return nullptr;
                    std::vector<G3D::Vector3> points{G3D::Vector3(from.GetPositionX(), from.GetPositionY(),
                                                                  from.GetPositionZ()),
                                                     G3D::Vector3(to.GetPositionX(), to.GetPositionY(),
                                                                  to.GetPositionZ())};
                    if (from.GetExactDist(here) < 1.0f)
                    {
                        Leader::RouteResult const route = Leader::Route(tank, to);  // [lead99]
                        if (route.found && route.points.size() >= 2)
                            points = route.points;
                    }
                    ClosedDoorHit const hit = PathThroughClosedDoor(tank, points);
                    if (hit.Found() && from.GetExactDist(here) < 1.0f)  // [sfk241]
                        NoteDoorOffLine(tank, hit.door,
                                        G3D::Vector3(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ()), 5.0f,
                                        "the pull's walk");
                    if (hit.Found())
                        doorAt = PointOnPath(Movement::PointsArray(points.begin(), points.end()),
                                             std::max(0.0f, hit.walked - DOOR_OPEN_BACK));
                    return hit.Found() ? hit.door : nullptr;
                };
                GameObject* door = crosses(here, stand);
                char const* leg = "the walk to the stand spot";
                if (!door && (door = crosses(stand, fightAt)))
                    leg = "the walk back to the fight spot";
                if (!door && decision.los && (door = crosses(here, decision.losGather)))
                    leg = "the group's walk to the gather spot";
                // [lead11] user rule: the group has the key and the gate is there - it is opened, no detour. A door
                // this group can open is no invalid plan but a step of it the leader takes first: a door opened by
                // use - the leader's walk to it and the use ([pull_after_hold] doorWalks, its auto-closing gather);
                // a key / lever door - its starter leads the route (Retarget: doorStepEntry), the key pass gathers
                // the whole group at the opener with no cap and re-opens it for a straggler. Only a door no member
                // can open stays an invalid plan (and planner_no_option after LEADER_DOOR_INVALID_LOOKS).
                if (door)
                {
                    int32 const doorStarter = group.DoorStarterFor(door);
                    bool const byUse = doorStarter < 0 && DoorOpensByUse(door) && lookMob && lookMob->ToCreature();
                    if (doorStarter >= 0 || byUse)
                    {
                        lead.doorInvalidLooks = 0;
                        if (byUse && !group.HasDoorWalk())
                            group.StartDoorWalk(door->GetGUID(), lookMob->GetGUID(), doorAt, now + DOOR_WALK_MS);
                        if (doorStarter >= 0)
                        {
                            if (lead.doorStepEntry != door->GetEntry() || !fresh(lead.doorStepMs))
                                group.RetargetAt(now);  // the starter leads from the next retarget on
                            lead.doorStepEntry = door->GetEntry();
                            lead.doorStepMs = now;
                        }
                        if (!lead.doorStepLogMs || getMSTimeDiff(lead.doorStepLogMs, now) >= 10 * IN_MILLISECONDS)
                        {
                            lead.doorStepLogMs = now;
                            group.LogEvent("plan_door_step",
                                     JsonObject().Str("kind", decision.kind)
                                         .Str("name", lookMob ? lookMob->GetName() : "?")
                                         .Str("door", door->GetName()).Num("doorEntry", door->GetEntry())
                                         .Str("leg", leg).Str("open", byUse ? "by use" : "its starter")
                                         .Str("starter", doorStarter >= 0
                                                             ? group.StarterNote(doorStarter)
                                                             : ""));
                        }
                        return;
                    }
                    if (lead.doorInvalidMob != decision.lookTarget)
                    {
                        lead.doorInvalidMob = decision.lookTarget;
                        lead.doorInvalidLooks = 0;
                    }
                    if (++lead.doorInvalidLooks == 1)
                        group.LogEvent("plan_through_door",
                                 JsonObject().Str("kind", decision.kind).Str("name", lookMob ? lookMob->GetName() : "?")
                                     .Str("door", door->GetName()).Num("doorEntry", door->GetEntry()).Str("leg", leg));
                    std::string why;
                    if (lead.ready && lead.doorInvalidLooks >= Leader::LEADER_DOOR_INVALID_LOOKS && lookMob)
                        LeaderNoOption(group, tank, lookMob, packOf(),
                                       Acore::StringFormat("every plan's walk crosses the closed {} {} ({})",
                                                           door->GetName(), door->GetEntry(), leg),
                                       group.NeedsPack(map, lookMob, why), JsonObject().Str("kind", "door"));
                    return;
                }
                lead.doorInvalidLooks = 0;
            }

            // [lead18] LEADER_COMMIT_PULL: the Go decision is the leader's commitment; its gate, window and order
            // run in runCommit from now on (the next looks do not re-plan)
            if (decides && Leader::LEADER_COMMIT_PULL)
            {
                ObjectGuid const mobGuid = !decision.planTarget.IsEmpty() ? decision.planTarget
                                           : !decision.attack.IsEmpty()   ? decision.attack
                                                                          : decision.lookTarget;
                Unit* mob = mobGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, mobGuid);
                // [lead82] b125 rfc-h 510.9-519.6 s: an engage on an Earthborer on the ledge 8 yd above the tank, out
                // of its sight - only the route's long navmesh way leads up; the walk-in was refused and the mob
                // marked unreachable. The route is data: while the tank walks the route (its "route walk" command,
                // accepted by the leader's checks) toward a mob it does not see yet, no engage is committed - the group
                // walks the route; the engage comes once the tank sees the mob or the route walk ends
                Leader::MemberEvents tankWalk;
                // [lead98] a boss too: "boss pulled where it stands" with "the tank's walk in for sight refused"
                // (b138 328.5 s, Taragaman) - the route walk takes the group there first
                // [lead100] b139: route_first held 34 s (b136: 1 s) - not when the route already walks the tank to
                // this same mob (the route target): only a mob the route leads past or toward first
                if (mob && decision.kind == "engage" && mobGuid != group.RouteTarget() &&
                    !tank->IsWithinLOSInMap(mob) &&
                    Leader::Events::Get(tank->GetGUID(), tankWalk) && tankWalk.move == Leader::MoveEvent::MoveTo &&
                    tankWalk.moveWhy == "route walk" &&
                    tank->GetExactDist2d(tankWalk.spot.GetPositionX(), tankWalk.spot.GetPositionY()) >
                        tankWalk.tolerance)
                {
                    if (lead.routeFirstFor != mobGuid)
                    {
                        lead.routeFirstFor = mobGuid;
                        Decisions().Write(run, "leader", "route_first", mobGuid, mob->GetName(), true,
                                                      JsonObject().Str("why", "out of the tank's sight: the route "
                                                                              "walk leads there first")
                                                          .Real("distance", tank->GetExactDist(mob))
                                                          .Real("walkTo", tank->GetExactDist(tankWalk.spot)));
                    }
                    return;
                }
                if (mob)
                {
                    lead.routeFirstFor.Clear();
                    lead.commit = std::make_shared<PullPlanner::Decision>(decision);
                    lead.commitMob = mobGuid;
                    lead.commitPos = mob->GetPosition();
                    lead.commitSpot = decision.side ? decision.sideSpot
                                      : decision.los ? decision.losSpot
                                      : decision.hasCast ? decision.castSpot
                                                         : mob->GetPosition();
                    lead.commitSinceMs = now;
                    lead.commitIssued = false;
                    lead.commitOrderSeen = false;
                    lead.commitMoving = mob->isMoving() || (mob->ToCreature() &&
                                                            mob->ToCreature()->GetDefaultMovementType() ==
                                                                WAYPOINT_MOTION_TYPE);
                    lead.commitWhy = pickWhy;
                    lead.commitWaitKey.clear();
                    Decisions().Write(run, "leader", "pull_commit", mobGuid, mob->GetName(), true,
                                                  JsonObject().Str("kind", decision.kind).Str("pick", pickWhy)
                                                      .Str("reason", decision.reason));
                    runCommit();
                    return;
                }
            }
            // [lead15] LEADER_ASSIGNS_SPOTS: before the pull every member gets its spot (StandAt), and the pull
            // is issued only once everyone stands on it, nobody sits, the healer sees the fight spot from its
            // spot in heal range (checked when assigned). No time cap; a member stuck is reported and not waited.
            if (decides && Leader::LEADER_MEMBER_EVENTS && Leader::LEADER_ASSIGNS_SPOTS &&
                LeaderAssignSpots(group, tank, decision))
            {
                std::string const waitFor = LeaderSpotsWait(group, tank);
                if (!waitFor.empty())
                {
                    lead.spotsWaiting = true;
                    if (!lead.spotsLogMs || getMSTimeDiff(lead.spotsLogMs, now) >= 10 * IN_MILLISECONDS)
                    {
                        lead.spotsLogMs = now;
                        group.LogEvent("leader_spots_wait",
                                 JsonObject().Str("name", lookMob ? lookMob->GetName() : "?")
                                     .Str("kind", decision.kind).Str("waitFor", waitFor)
                                     .Num("spots", int64(lead.spots.size()))
                                     .Real("fightX", lead.spotsFight.GetPositionX())
                                     .Real("fightY", lead.spotsFight.GetPositionY())
                                     .Real("fightZ", lead.spotsFight.GetPositionZ()));
                    }
                    return;
                }
                lead.spotsWaiting = false;
                lead.spotsIssuedMs = now;
            }
            // [lead270] a prepull Army's channel under way ends before the boss pull is issued
            // [lead541] the boss the pull brings too (PullBoss)
            // [lead773] and the rogue's pre-pull Stealth (StealthHolds)
            if (Creature* pullBoss = decides ? PullBoss(tank->GetMap(), lookMob, decision) : nullptr)
                if (std::string army = Leader::Prepull::ArmyHolds(group, pullBoss, false);
                    !army.empty() || !(army = Leader::Prepull::StealthHolds(group, pullBoss)).empty())
                {
                    if (lead.armyHoldFor != pullBoss->GetGUID())
                    {
                        lead.armyHoldFor = pullBoss->GetGUID();
                        group.LogEvent("leader_spots_wait", JsonObject().Str("name", pullBoss->GetName())
                                                                .Str("kind", decision.kind).Str("waitFor", army));
                    }
                    return;
                }
            issue(decision, pickWhy);
            return;
        }

        // [lead3] [lead6] a "held for now" with the leader's group ready: the hold no rest can change (the old
        // [held_for_now_cap] walked in after it) is reported as no option after LEADER_HELD_READY_MAX_MS. The
        // declared waits with their own end (a patrol / overlap wait: the forecast; crowd control claims) do not
        // count.
        if (decides && decision.verdict == PullPlanner::Verdict::Held && lookMob)
        {
            // [lead12] sfk-h b56: Rethilgore "held 75 s ... (a boss pull: no fallback plan)" while the leader stood in
            // its own patrol waits, 50 yd short of its stop: a boss's pack is the leader's boss pull at its stop (no
            // planner hold to time out), and the clock runs only while the leader stands at its stop, no patrol wait
            bool const bossPack = packBoss() != nullptr;
            bool const atStop = group.ArrivedMs() && lead.waitPatrol.IsEmpty();
            bool const declaredWait = bossPack || !atStop ||
                                      decision.why.find("patrol wait") != std::string::npos ||
                                      decision.why.find("overlap wait") != std::string::npos ||
                                      decision.why.find("pre-pull cc") != std::string::npos ||
                                      decision.why.find("the leader:") != std::string::npos ||
                                      decision.why.find("the leader picks no mob") != std::string::npos;
            if (!lead.ready || declaredWait || lead.heldReadyMob != decision.lookTarget)
            {
                lead.heldReadyMob = lead.ready && !declaredWait ? decision.lookTarget : ObjectGuid::Empty;
                lead.heldReadySinceMs = now;
            }
            else if (Leader::LEADER_NO_FORCING_TIMERS &&
                     getMSTimeDiff(lead.heldReadySinceMs, now) >= Leader::LEADER_HELD_READY_MAX_MS)
                WaitedLong(group, "held_ready", getMSTimeDiff(lead.heldReadySinceMs, now),
                           (lookMob ? lookMob->GetName() : std::string("?")) + ": " + decision.why);
            else if (getMSTimeDiff(lead.heldReadySinceMs, now) >= Leader::LEADER_HELD_READY_MAX_MS)
            {
                std::string why;
                bool const needs = group.NeedsPack(map, lookMob, why);
                bool const reported =
                    LeaderNoOption(group, tank, lookMob, packOf(),
                                   Acore::StringFormat("held {} s with the group ready: {}",
                                                       getMSTimeDiff(lead.heldReadySinceMs, now) / IN_MILLISECONDS,
                                                       decision.why),
                                   needs, JsonObject().Str("kind", "held").Str("rule", why));
                if (reported && !needs && lookMob->GetGUID() == group.RouteTarget())
                {
                    group.LogEvent("leader_skip_no_option",
                             JsonObject().Str("name", lookMob->GetName()).Str("why", decision.why).Str("rule", why));
                    group.SkipTarget(group.RouteTarget(), CAREFUL_REFUSED_SKIP_MS,
                               "the leader: no pull option, not on the required way (" + decision.why + ")");
                }
                lead.heldReadyMob.Clear();
                lead.heldReadySinceMs = 0;
            }
            return;
        }

        if (decision.verdict != PullPlanner::Verdict::Refused || !lookMob)
            return;
        lead.heldReadyMob.Clear();
        lead.heldReadySinceMs = 0;

        // [lead4] the patrol the leader decided to pull, refused by its planner with the group ready and topped up:
        // no option (the route crosses its walk, or it never leaves the group's spot: no wait is valid either)
        // [lead4b] only a hopeless refusal is no option: "refused for now" (the stand's guarantee still to come, its
        // verdict wait running) is a hold, re-asked on the next look - build 53 stopped ac-h / dme-n / uk-h on it
        // [lead12] ... and a hopeless one stops nothing either: a patrol is no pack the leader must clear (b55 sh-h:
        // "the patrol pull refused: ... Shattered Hand Heathen brings 11 mobs over the cap 9"; the old path waited it
        // out and walked on after its budget). The patrol is banned from intercepts for LEADER_INTERCEPT_BAN_MS and
        // its lane is crossed while it is far from the leader (LeaderPatrolTick); logged, not stopping.
        if (decides && Leader::Leads(Leader::LEADER_ONE_PATROL_OWNER) && fresh(lead.interceptMs) &&
            decision.lookTarget == lead.intercept && decision.hopeless && decision.toppedUp && lead.ready &&
            getMSTimeDiff(lead.interceptSinceMs, now) >= Leader::LEADER_REQUEST_ANSWER_MS)
        {
            constexpr uint32 LEADER_INTERCEPT_BAN_MS = 5 * MINUTE * IN_MILLISECONDS;
            lead.interceptBanned[lead.intercept] = now + LEADER_INTERCEPT_BAN_MS;
            LeaderNoOption(group, tank, lookMob, packOf(), "the patrol pull refused: " + decision.why, false,
                           JsonObject().Str("kind", "patrol").Str("patrolWhy", lead.interceptWhy)
                               .Str("action", "no intercept; its lane is crossed while it is far"));
            lead.intercept.Clear();
            lead.interceptMs = 0;
            return;
        }

        // [lead12] dmn-n b56: "the whole pack offered, refused: refused for now: the pack of Gordok Mastiff brings a
        // boss, pulled as his fight once the group is ready for him" stopped the run. That refusal is the planner's
        // deferral to the boss's fight, no answer: the boss is the next pull (the old [b51_dme] boss_on_the_way_first
        // rule), and the leader pulls him at his stop once ready (Steer: LeaderBossPull).
        if (decides && !decision.hopeless)
            if (Creature* boss = packBoss())
            {
                if (group.RouteTarget() != boss->GetGUID())
                {
                    group.LogEvent("boss_on_the_way_first",
                             JsonObject().Str("first", boss->GetName()).Num("firstEntry", boss->GetEntry())
                                 .Real("firstDistance", tank->GetExactDist(boss)).Str("via", lookMob->GetName())
                                 .Num("viaEntry", lookMob->GetEntry())
                                 .Str("why", "[lead12] the planner defers the pack to its boss's fight: " +
                                                 decision.why));
                    group.SetBossOnTheWay(boss->GetGUID(), now);
                }
                if (requestCovers)
                {
                    lead.wholePack.Clear();
                    lead.wholeSeenMs = 0;
                }
                return;
            }

        // planner_no_option (user decision 1): a hopeless refusal (no pull of the pack fits at all), or [lead3] any
        // refusal of the pack the leader asked for whole (LEADER_REQUEST_ANSWER_MS after the ask: the planner's 1 s
        // plan cache may answer before it). It stops the run when the pack is one the leader must clear; a hopeless
        // side pack is a route decision: the walks go round it, the route target of it is skipped.
        // [lead4b] ... nor a "refused for now" whose guarantee wait still runs (verdictWaitMs): only a final refusal
        bool const finalRefusal = decision.hopeless || (decision.hasVerdict && !decision.verdictWaitMs);
        bool const requestAnswered = requestCovers && finalRefusal && decision.toppedUp && lead.ready &&
                                     getMSTimeDiff(lead.wholeSinceMs, now) >= Leader::LEADER_REQUEST_ANSWER_MS;
        if (!decision.hopeless && !requestAnswered)
            return;
        std::string rule;
        bool const needs =
            decides && ((requestAnswered && lead.required) || group.NeedsPack(map, lookMob, rule));
        // a hopeless pack the leader must clear is asked of the planner whole first (marked required: its
        // aoe_pull_plan may run, the stand's guarantee takes it whole); only that ask's refusal is no option
        if (needs && !requestAnswered && lookMob->ToCreature())
        {
            LeaderWholePack(group, tank, lookMob->ToCreature(), "hopeless as it stands (" + rule + "): " + decision.why,
                            true, true);
            return;
        }
        JsonObject fields;
        fields.Num("smallestPull", decision.mobs).Real("cost", decision.cost).Bool("hopeless", decision.hopeless)
            .Bool("wholePackAsked", requestAnswered).Str("rule", rule);
        std::string const why = (requestAnswered ? "the whole pack offered, refused: " : std::string()) + decision.why;
        if (!LeaderNoOption(group, tank, lookMob, packOf(), why, needs, fields) || !decides || needs)
            return;
        if (Creature* hopelessMob = lookMob->ToCreature(); hopelessMob && decision.hopeless)
            group.NoteHopelessPack(tank, hopelessMob);  // [b32_brs] the walks go round it (HopelessDetourLeg)
        std::vector<ObjectGuid> const pack = packOf();
        if (!group.RouteTarget().IsEmpty() && std::find(pack.begin(), pack.end(), group.RouteTarget()) != pack.end())
        {
            group.LogEvent("leader_skip_no_option",
                     JsonObject().Str("name", lookMob->GetName()).Str("why", why).Str("rule", rule));
            group.SkipTarget(group.RouteTarget(), CAREFUL_REFUSED_SKIP_MS,
                       "the leader: no pull option, not on the required way (" + why + ")");
        }
        if (lead.wholePack == decision.lookTarget || requestAnswered)
        {
            lead.wholePack.Clear();
            lead.wholeSeenMs = 0;
        }
    }
}
