/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

/*
 * Ported from the CMaNGOS playerbots project (https://github.com/cmangos/playerbots), GPL v2,
 * with modifications for AzerothCore.
 * Original authors:
 *   ike3 <ike@email.org> - original author
 *   Sebastiaan Keek (mostlikely4r) <sebastiaan.keek@gmail.com>
 *   David Parra Ausina (Flekz) <davidparraausina@gmail.com>
 */

#include "PullStrategy.h"
#include "AiObjectContext.h"
#include "CheckMountStateAction.h"  // [b32_engage]
#include "ChooseTargetActions.h"
#include "FollowActions.h"
#include "Group.h"
#include "Map.h"
#include "MoveConflict.h"  // [b33_harn]
#include "MovementActions.h"
#include "ObjectAccessor.h"
#include "PassiveMultiplier.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PullActions.h"
#include "RangedPositionActions.h"
#include "ReachTargetActions.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr uint32 PULL_CHECK_SAMPLE_MS = 1000;        // the pull check looks at the tank's fight this often
    constexpr uint32 LOS_PULL_EVAL_MS = 300;             // one look at the pack per group this often is enough
    constexpr uint32 LOS_PULL_MAX_PLAN_MS = 30000;       // planned and never pulled: given up
    constexpr uint32 LOS_PULL_MAX_HOLD_MS = 15000;       // after the pull the group holds this long at most
    constexpr uint32 LOS_PULL_ENGAGE_GRACE_MS = 1500;    // the pull spell may still be on its way
    constexpr uint32 LOS_PULL_NO_ENGAGE_MS = 4000;       // nothing of the pack fights by then: the pull missed
    constexpr uint32 LOS_PULL_HOLD_CHECK_MS = 200;       // the hold multiplier reads the registry this often
    constexpr float LOS_PULL_SPOT_RADIUS = 10.0f;        // a mob this close to the tank's spot has come round
    constexpr float LOS_PULL_GATHER_FAR = 100.0f;        // a member this far from the tank is not waited for

    // [b32_engage] The first pull of a pack in a fight, by group: a plan registered again on the same pack (or any
    // of its mobs) while the tank fights keeps it, so LOS_PULL_MAX_HOLD_MS is absolute per pull. nex-h-5B-73
    // 20261025-223303 pulls #3 / #7: re-registered plans stretched the hold, the rogues hit nothing in 51-76 s.
    struct LosPullHoldStart
    {
        uint32 ms = 0;
        GuidVector pack;
    };

    struct LosPullRegistry
    {
        std::mutex lock;
        std::unordered_map<ObjectGuid, LosPull::Plan> plans;  // by group
        std::unordered_map<ObjectGuid, LosPullHoldStart> holdStarts;  // by group [b32_engage]
        uint32 nextId = 0;
        std::atomic<uint32> count{0};  // plans.size(), read without the lock: most groups never have one
    };

    LosPullRegistry& GetLosPullRegistry()
    {
        static LosPullRegistry registry;
        return registry;
    }

    bool IsLosPullExpired(LosPull::Plan const& plan, uint32 now)
    {
        if (plan.released)
            return true;

        if (plan.holdStartMs && getMSTimeDiff(plan.holdStartMs, now) > LOS_PULL_MAX_HOLD_MS)
            return true;  // [b32_engage]

        if (!plan.castMs)
            return getMSTimeDiff(plan.plannedMs, now) > LOS_PULL_MAX_PLAN_MS;

        return getMSTimeDiff(plan.castMs, now) > LOS_PULL_MAX_HOLD_MS;
    }

    // [b32_engage] The plan's pull is cast: its hold start is the pack's first pull in this fight (registry locked).
    void NoteLosPullHoldStart(LosPullRegistry& registry, ObjectGuid const& group, LosPull::Plan& plan)
    {
        if (!plan.castMs || plan.holdStartMs)
            return;

        LosPullHoldStart& start = registry.holdStarts[group];
        if (!start.ms)
        {
            start.ms = plan.castMs;
            start.pack = plan.pack;
        }

        plan.holdStartMs = start.ms;
    }

    // Works on a copy of the plan, outside the registry lock: LOS checks against every mob of the pack.
    void EvaluateLosPull(Player* bot, LosPull::Plan& plan, uint32 now)
    {
        Unit* tank = ObjectAccessor::GetUnit(*bot, plan.tank);
        if (!tank || !tank->IsAlive())
        {
            plan.released = true;
            return;
        }

        if (!plan.castMs)
        {
            // Engaged without the tank's cast being seen yet (the spell landed, a mob noticed someone): pulled.
            for (ObjectGuid const& guid : plan.pack)
            {
                Unit* mob = ObjectAccessor::GetUnit(*bot, guid);
                if (mob && mob->IsAlive() && mob->IsInCombat())
                {
                    plan.castMs = now;
                    break;
                }
            }
        }

        if (!plan.castMs)
        {
            Unit* target = ObjectAccessor::GetUnit(*bot, plan.target);
            if (!target || !target->IsAlive())
            {
                plan.released = true;
                return;
            }

            bool gathered = true;
            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref && gathered; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (!member || member->GetGUID() == plan.tank || !member->IsAlive() ||
                        member->GetMap() != bot->GetMap())
                        continue;

                    // [hleft-los-gather-far] A member farther than LOS_PULL_GATHER_FAR from the tank is not on his
                    // way to this pull (stuck at the entrance after a corpse run, Ahn'kahet heroic 2026-10-25
                    // 20261025-064751-ok-h-5A-68: Rtshaab 280 yd off, "move to los pull spot" FAILED): the gather
                    // never completed, the tank re-planned every 30 s and the group stood 241 s without a fight.
                    if (member->GetExactDist(tank) > LOS_PULL_GATHER_FAR)
                        continue;

                    if (member->GetExactDist2d(plan.gather.GetPositionX(), plan.gather.GetPositionY()) >
                        LosPull::GATHER_RADIUS)
                        gathered = false;
                }
            }

            plan.gathered = gathered;
            return;
        }

        uint32 engaged = 0;
        bool everyoneCame = true;
        for (ObjectGuid const& guid : plan.pack)
        {
            Unit* mob = ObjectAccessor::GetUnit(*bot, guid);
            if (!mob || !mob->IsAlive() || !mob->IsInCombat())
                continue;  // a mob that stayed idle is not coming

            ++engaged;
            if (!plan.arrived && tank->IsWithinMeleeRange(mob))
                plan.arrived = true;

            if (everyoneCame &&
                mob->GetExactDist2d(plan.spot.GetPositionX(), plan.spot.GetPositionY()) > LOS_PULL_SPOT_RADIUS &&
                !mob->IsWithinLOS(plan.spot.GetPositionX(), plan.spot.GetPositionY(), plan.spot.GetPositionZ()))
                everyoneCame = false;
        }

        // [lead92] the hold ends once every mob of the pack in the fight is on the tank (its victim): the casters
        // the corner was for are the tank's, wherever they stand
        bool allOnTank = engaged > 0;
        for (ObjectGuid const& guid : plan.pack)
            if (Unit* mob = ObjectAccessor::GetUnit(*bot, guid);
                allOnTank && mob && mob->IsAlive() && mob->IsInCombat() && mob->GetVictim() != tank)
                allOnTank = false;

        uint32 const sinceCast = getMSTimeDiff(plan.castMs, now);
        if (sinceCast < LOS_PULL_ENGAGE_GRACE_MS)
            return;

        if ((engaged && (everyoneCame || allOnTank)) || (!engaged && sinceCast >= LOS_PULL_NO_ENGAGE_MS))
            plan.released = true;
    }
}

namespace LosPull
{
    void Register(Player* tank, Unit* target, GuidVector const& pack, Position const& spot, Position const& gather)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        if (!group || !target)
            return;

        // [b33_harn] not while a dungeon event holds the group at its spot (MoveConflict::NoteEventHold)
        if (MoveConflict::EventHoldBlocksSpot(tank, spot.GetPositionX(), spot.GetPositionY(), "los pull") ||
            MoveConflict::EventHoldBlocksSpot(tank, gather.GetPositionX(), gather.GetPositionY(), "los pull"))
            return;

        Plan plan;
        plan.mapId = tank->GetMapId();
        plan.instanceId = tank->GetInstanceId();
        plan.tank = tank->GetGUID();
        plan.target = target->GetGUID();
        plan.pack = pack;
        plan.spot = spot;
        plan.gather = gather;
        plan.plannedMs = getMSTime();

        LosPullRegistry& registry = GetLosPullRegistry();
        std::lock_guard<std::mutex> guard(registry.lock);
        // [b32_engage] The same pack planned again in the fight keeps its first pull's hold start; out of combat,
        // or another pack, starts over.
        auto const start = registry.holdStarts.find(group->GetGUID());
        if (start != registry.holdStarts.end())
        {
            bool samePack = false;
            for (ObjectGuid const& guid : pack)
                samePack = samePack || std::find(start->second.pack.begin(), start->second.pack.end(), guid) !=
                                           start->second.pack.end();

            if (tank->IsInCombat() && samePack)
                plan.holdStartMs = start->second.ms;
            else
                registry.holdStarts.erase(start);
        }

        plan.id = ++registry.nextId;
        registry.plans[group->GetGUID()] = plan;
        registry.count = uint32(registry.plans.size());
    }

    bool Get(Player* bot, Plan& out)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        if (!group)
            return false;

        LosPullRegistry& registry = GetLosPullRegistry();
        if (!registry.count.load(std::memory_order_relaxed))
            return false;

        ObjectGuid const key = group->GetGUID();
        uint32 const now = getMSTime();
        Plan copy;
        {
            std::lock_guard<std::mutex> guard(registry.lock);
            auto const itr = registry.plans.find(key);
            if (itr == registry.plans.end())
                return false;

            Plan& plan = itr->second;
            if (IsLosPullExpired(plan, now))
            {
                registry.plans.erase(itr);
                registry.count = uint32(registry.plans.size());
                return false;
            }

            if (plan.mapId != bot->GetMapId() || plan.instanceId != bot->GetInstanceId())
                return false;

            // [b33_harn] a dungeon event hold began after the plan: the plan is dropped, the group stays on the spot
            // (hos-h 20261026-000434-hos-h-5B-37 1946 s: "move to los pull spot" mid-Tribunal).
            if (MoveConflict::EventHoldBlocksSpot(bot, plan.spot.GetPositionX(), plan.spot.GetPositionY(),
                                                  "los pull") ||
                MoveConflict::EventHoldBlocksSpot(bot, plan.gather.GetPositionX(), plan.gather.GetPositionY(),
                                                  "los pull"))
            {
                registry.plans.erase(itr);
                registry.count = uint32(registry.plans.size());
                return false;
            }

            if (plan.evalMs && getMSTimeDiff(plan.evalMs, now) < LOS_PULL_EVAL_MS)
            {
                out = plan;
                return true;
            }

            plan.evalMs = now;  // this bot takes the look for the group
            copy = plan;
        }

        EvaluateLosPull(bot, copy, now);

        std::lock_guard<std::mutex> guard(registry.lock);
        auto const itr = registry.plans.find(key);
        if (itr == registry.plans.end() || itr->second.id != copy.id)
            return false;

        Plan& plan = itr->second;
        if (!plan.castMs)
            plan.castMs = copy.castMs;
        NoteLosPullHoldStart(registry, key, plan);  // [b32_engage]
        plan.gathered = copy.gathered;
        plan.arrived = plan.arrived || copy.arrived;
        plan.released = plan.released || copy.released;
        if (plan.released)
        {
            LOG_DEBUG("playerbots", "[los pull] group of {}: hold over {} ms after the pull (mob in melee: {})",
                      bot->GetName(), plan.castMs ? getMSTimeDiff(plan.castMs, now) : 0, plan.arrived);
            registry.plans.erase(itr);
            registry.count = uint32(registry.plans.size());
            return false;
        }

        out = plan;
        return true;
    }

    bool IsHolding(Player* bot)
    {
        Plan plan;
        return Get(bot, plan) && Holds(bot, plan);
    }

    bool Holds(Player* bot, Plan const& plan)
    {
        // [lead14] the gather ends for every member once the pack is on the tank (the fight is on at the corner):
        // the ranged and healers stand at their own ranged spot, which keeps out of the cleaves (RangedPosition-
        // Value). b58 strat-h-5C-1: the owl and the mage held the gather after the pack arrived and ran between it
        // and "dungeon avoid frontal cleave" (Plague Ghoul / Skeletal Berserker cones at the corner): "dungeon avoid
        // frontal cleave:10, move to los pull spot:9, move to ranged position:3" in 15 s, 30 of 77 ticks no cast.
        if (Leader::LEADER_PLANNER_IN_LEADER)
            return !plan.arrived || plan.tank == bot->GetGUID();
        return !plan.arrived || plan.tank == bot->GetGUID() || PlayerbotAI::IsRanged(bot) ||
               PlayerbotAI::IsHeal(bot) || PlayerbotAI::IsTank(bot);
    }

    uint32 BeginWait(Player* tank)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        if (!group)
            return 0;

        uint32 const now = getMSTime();
        LosPullRegistry& registry = GetLosPullRegistry();
        std::lock_guard<std::mutex> guard(registry.lock);
        auto const itr = registry.plans.find(group->GetGUID());
        if (itr == registry.plans.end() || itr->second.tank != tank->GetGUID())
            return 0;

        if (!itr->second.waitMs)
            itr->second.waitMs = now;

        return getMSTimeDiff(itr->second.waitMs, now);
    }

    void MarkCast(Player* tank)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        if (!group)
            return;

        LosPullRegistry& registry = GetLosPullRegistry();
        std::lock_guard<std::mutex> guard(registry.lock);
        auto const itr = registry.plans.find(group->GetGUID());
        if (itr != registry.plans.end() && itr->second.tank == tank->GetGUID() && !itr->second.castMs)
        {
            itr->second.castMs = getMSTime();
            NoteLosPullHoldStart(registry, group->GetGUID(), itr->second);  // [b32_engage]
        }
    }

    void Cancel(Player* tank)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        if (!group)
            return;

        LosPullRegistry& registry = GetLosPullRegistry();
        std::lock_guard<std::mutex> guard(registry.lock);
        auto const itr = registry.plans.find(group->GetGUID());
        if (itr != registry.plans.end() && itr->second.tank == tank->GetGUID() && !itr->second.castMs)
        {
            LOG_DEBUG("playerbots", "[los pull] {}: pull ended before the cast, plan dropped", tank->GetName());
            registry.plans.erase(itr);
            registry.count = uint32(registry.plans.size());
        }
    }
}

namespace
{
    constexpr uint32 PRE_PULL_CC_MAX_PLAN_MS = 30000;   // planned and never pulled: given up
    constexpr uint32 PRE_PULL_CC_AFTER_PULL_MS = 15000;  // the members' marks outlive the pull this long

    struct PrePullCcRegistry
    {
        std::mutex lock;
        std::unordered_map<ObjectGuid, PrePullCc::Plan> plans;  // by group
        uint32 nextId = 0;
        std::atomic<uint32> count{0};  // plans.size(), read without the lock: most groups never have one
    };

    PrePullCcRegistry& GetPrePullCcRegistry()
    {
        static PrePullCcRegistry registry;
        return registry;
    }

    bool IsPrePullCcExpired(PrePullCc::Plan const& plan, uint32 now)
    {
        if (plan.pulledMs)
            return getMSTimeDiff(plan.pulledMs, now) > PRE_PULL_CC_AFTER_PULL_MS;

        return getMSTimeDiff(plan.plannedMs, now) > PRE_PULL_CC_MAX_PLAN_MS;
    }

    // The live plan of the group, under the registry lock; expired plans are dropped on the way.
    PrePullCc::Plan* FindPrePullCc(PrePullCcRegistry& registry, Group* group, uint32 now)
    {
        auto const itr = registry.plans.find(group->GetGUID());
        if (itr == registry.plans.end())
            return nullptr;

        if (IsPrePullCcExpired(itr->second, now))
        {
            registry.plans.erase(itr);
            registry.count = uint32(registry.plans.size());
            return nullptr;
        }

        return &itr->second;
    }
}

namespace PrePullCc
{
    bool HasPlan(Player* tank, ObjectGuid pullTarget)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        if (!group || !registry.count.load(std::memory_order_relaxed))
            return false;

        std::lock_guard<std::mutex> guard(registry.lock);
        Plan const* plan = FindPrePullCc(registry, group, getMSTime());
        return plan && plan->tank == tank->GetGUID() && plan->pullTarget == pullTarget;
    }

    void Register(Player* tank, Unit* pullTarget, GuidVector const& pack, GuidVector const& controllers, bool heavy,
                  bool reopen)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        if (!group || !pullTarget)
            return;

        uint32 const now = getMSTime();
        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        std::lock_guard<std::mutex> guard(registry.lock);
        Plan* current = FindPrePullCc(registry, group, now);
        // The planner settled on another mob of the same pack (an add at its edge): the claims stand.
        if (current && current->tank == tank->GetGUID() && !current->pulledMs &&
            std::find(current->pack.begin(), current->pack.end(), pullTarget->GetGUID()) != current->pack.end())
        {
            current->pullTarget = pullTarget->GetGUID();
            if (!reopen)
            {
                current->heavy = current->heavy || heavy;
                return;
            }

            // Refused -> accepted (wave 44): `heavy` and the declines of the refusal no longer hold.
            current->heavy = heavy;
            current->claims.erase(std::remove_if(current->claims.begin(), current->claims.end(),
                                                 [](Claim const& claim) { return !claim.target || claim.failed; }),
                                  current->claims.end());
            for (Claim& claim : current->claims)
                claim.claimedMs = now;  // a claim held through the refusal gets its full time to be placed

            current->plannedMs = now;
            LOG_DEBUG("playerbots", "[pre-pull cc] {} reopens the plan on {}: the pack is accepted now, {} claims "
                      "stand", tank->GetName(), pullTarget->GetName(), current->claims.size());
            return;
        }

        Plan plan;
        plan.id = ++registry.nextId;
        plan.mapId = tank->GetMapId();
        plan.instanceId = tank->GetInstanceId();
        plan.tank = tank->GetGUID();
        plan.pullTarget = pullTarget->GetGUID();
        plan.pack = pack;
        plan.controllers = controllers;
        plan.plannedMs = now;
        plan.heavy = heavy;
        registry.plans[group->GetGUID()] = plan;
        registry.count = uint32(registry.plans.size());
        LOG_DEBUG("playerbots", "[pre-pull cc] {} plans {} ({} mobs{}), {} crowd control members asked",
                  tank->GetName(), pullTarget->GetName(), pack.size(), heavy ? ", refused as it stands" : "",
                  controllers.size());
    }

    bool IsHolding(Player* tank)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        if (!group || !registry.count.load(std::memory_order_relaxed))
            return false;

        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(registry.lock);
        Plan* plan = FindPrePullCc(registry, group, now);
        if (!plan || plan->tank != tank->GetGUID() || plan->pulledMs)
            return false;

        uint32 const age = getMSTimeDiff(plan->plannedMs, now);
        if (age >= MAX_WAIT_MS)
            return false;

        bool pending = false;
        for (Claim& claim : plan->claims)
        {
            if (!claim.target || claim.ready || claim.failed)
                continue;

            // [prepull_cc_claim_drop] not placed in time: dropped, the pull goes on
            if (getMSTimeDiff(claim.claimedMs, now) >= CLAIM_PLACE_MS)
            {
                claim.failed = true;
                LOG_DEBUG("playerbots", "[prepull_cc_claim_drop] {}: a claim not placed within {} ms is dropped, the "
                          "pull goes on", tank->GetName(), CLAIM_PLACE_MS);
                continue;
            }

            pending = true;
        }

        if (pending)
            return true;

        return age < CLAIM_WINDOW_MS && plan->claims.size() < plan->controllers.size();
    }

    void MarkPulled(Player* tank)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        if (!group || !registry.count.load(std::memory_order_relaxed))
            return;

        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(registry.lock);
        Plan* plan = FindPrePullCc(registry, group, now);
        if (plan && plan->tank == tank->GetGUID() && !plan->pulledMs)
        {
            plan->pulledMs = now;
            uint32 placed = 0;
            for (Claim const& claim : plan->claims)
            {
                if (claim.target && claim.ready && !claim.failed)
                    ++placed;
            }

            LOG_DEBUG("playerbots", "[pre-pull cc] {} pulls after {} ms: {} of {} claims placed", tank->GetName(),
                      getMSTimeDiff(plan->plannedMs, now), placed, plan->claims.size());
        }
    }

    void Cancel(Player* tank)
    {
        Group* group = tank ? tank->GetGroup() : nullptr;
        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        if (!group || !registry.count.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> guard(registry.lock);
        auto const itr = registry.plans.find(group->GetGUID());
        if (itr == registry.plans.end() || itr->second.tank != tank->GetGUID())
            return;

        LOG_DEBUG("playerbots", "[pre-pull cc] {} cancels the plan: a single mob is pulled", tank->GetName());
        registry.plans.erase(itr);
        registry.count = uint32(registry.plans.size());
    }

    bool Get(Player* bot, Plan& out)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        if (!group || !registry.count.load(std::memory_order_relaxed))
            return false;

        std::lock_guard<std::mutex> guard(registry.lock);
        Plan const* plan = FindPrePullCc(registry, group, getMSTime());
        if (!plan || plan->mapId != bot->GetMapId() || plan->instanceId != bot->GetInstanceId())
            return false;

        out = *plan;
        return true;
    }

    bool IsClaimed(Player* bot, ObjectGuid target)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        if (!group || !target || !registry.count.load(std::memory_order_relaxed))
            return false;

        std::lock_guard<std::mutex> guard(registry.lock);
        Plan const* plan = FindPrePullCc(registry, group, getMSTime());
        if (!plan)
            return false;

        for (Claim const& claim : plan->claims)
        {
            if (claim.target == target && !claim.failed)
                return true;
        }

        return false;
    }

    bool TryClaim(Player* bot, uint32 planId, ObjectGuid target, uint32 maxClaims, bool ready)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        if (!group)
            return false;

        uint32 const now = getMSTime();
        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        std::lock_guard<std::mutex> guard(registry.lock);
        Plan* plan = FindPrePullCc(registry, group, now);
        if (!plan || plan->id != planId || plan->pulledMs)
            return false;

        uint32 claimed = 0;
        for (Claim const& claim : plan->claims)
        {
            if (claim.bot == bot->GetGUID())
                return false;  // one answer per bot and plan

            if (claim.target && !claim.failed)
            {
                if (claim.target == target)
                    return false;

                ++claimed;
            }
        }

        if (target && claimed >= maxClaims)
            return false;

        Claim claim;
        claim.bot = bot->GetGUID();
        claim.target = target;
        claim.claimedMs = now;
        claim.ready = ready || !target;
        plan->claims.push_back(claim);
        return true;
    }

    void SetClaimState(Player* bot, uint32 planId, bool ready, bool failed)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        if (!group)
            return;

        PrePullCcRegistry& registry = GetPrePullCcRegistry();
        std::lock_guard<std::mutex> guard(registry.lock);
        Plan* plan = FindPrePullCc(registry, group, getMSTime());
        if (!plan || plan->id != planId)
            return;

        for (Claim& claim : plan->claims)
        {
            if (claim.bot != bot->GetGUID())
                continue;

            claim.ready = claim.ready || ready;
            claim.failed = claim.failed || failed;
        }
    }
}

class PullStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    PullStrategyActionNodeFactory()
    {
        creators["pull start"] = &pull_start;
    }

private:
    static ActionNode* pull_start(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode("pull start", {}, {}, { NextAction("pull action", ACTION_NORMAL) });
    }
};

PullStrategy::PullStrategy(PlayerbotAI* botAI, std::string const action, std::string const preAction)
    : Strategy(botAI), action(action), preAction(preAction)
{
    actionNodeFactories.Add(new PullStrategyActionNodeFactory());
}

PullStrategy* PullStrategy::Get(PlayerbotAI* botAI)
{
    if (!botAI)
        return nullptr;

    if (PullStrategy* strategy = dynamic_cast<PullStrategy*>(botAI->GetStrategy("pull", BOT_STATE_NON_COMBAT)))
    {
        if (strategy->IsPullPendingToStart() || strategy->HasPullStarted() || strategy->HasTarget())
            return strategy;
    }

    return dynamic_cast<PullStrategy*>(botAI->GetStrategy("pull", BOT_STATE_COMBAT));
}

Unit* PullStrategy::GetTarget() const
{
    ObjectGuid const guid = botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull strategy target")->Get();
    if (guid.IsEmpty())
        return nullptr;

    Unit* target = botAI->GetUnit(guid);
    Player* bot = botAI->GetBot();
    if (!bot || !target || !target->IsAlive() || !target->IsInWorld() ||
        target->GetMapId() != bot->GetMapId())
        return nullptr;

    return target;
}

bool PullStrategy::HasTarget() const { return GetTarget() != nullptr; }

void PullStrategy::SetTarget(Unit* target)
{
    botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull strategy target")->Set(target ? target->GetGUID() : ObjectGuid::Empty);
}

std::string PullStrategy::GetPullActionName() const
{
    return action;
}

bool PullStrategy::HitsMoreThanOne(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    if (spellInfo->IsAffectingArea())
        return true;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (spellInfo->Effects[i].IsEffect() && spellInfo->Effects[i].ChainTarget > 1)
            return true;
    }

    return false;
}

std::string PullStrategy::GetCastActionName() const
{
    std::string const chosen = GetPullActionName();
    if (!singleMob || chosen == action)
        return chosen;

    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", chosen)->Get();
    if (!HitsMoreThanOne(sSpellMgr->GetSpellInfo(spellId)))
        return chosen;

    // [pal_pull_spell_fallback] the class's base pull action is no fallback when the bot knows no spell of that name
    // (a paladin's "judgement"): the pull would never cast. The multi-target spell is cast instead.
    uint32 const baseId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", action)->Get();
    return baseId ? action : chosen;
}

std::string PullStrategy::GetSpellName() const
{
    Player* bot = botAI->GetBot();
    std::string spellName = GetCastActionName();
    if (!bot || spellName != "shoot")
        return spellName;

    Item* equippedWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!equippedWeapon)
        return spellName;

    ItemTemplate const* itemTemplate = equippedWeapon->GetTemplate();
    if (!itemTemplate)
        return spellName;

    // A bow, gun or crossbow fires the one "Shoot" (3018) of warriors and rogues, as CastShootAction casts it: 3.3.5
    // has no player spell "Shoot Bow" / "Shoot Gun" / "Shoot Crossbow", so "spell id" read 0 for those names and no
    // warrior with a ranged weapon ever had a ranged pull (wave 37: every lone-mob pull waited for good, Ragefire
    // Chasm and Deadmines).
    return itemTemplate->SubClass == ITEM_SUBCLASS_WEAPON_THROWN ? std::string("throw") : spellName;
}

float PullStrategy::GetRange() const
{
    Player* bot = botAI->GetBot();
    std::string const spellName = GetSpellName();
    if (bot && !spellName.empty())
    {
        uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spellName)->Get();
        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId))
            return bot->GetSpellMaxRangeForTarget(GetTarget(), spellInfo) - CONTACT_DISTANCE;
    }

    return (action == "shoot" ? botAI->GetRange("shoot") : botAI->GetRange("spell")) - CONTACT_DISTANCE;
}

std::string PullStrategy::GetPreActionName() const
{
    return preAction;
}

bool PullStrategy::CanDoPullAction(Unit* target)
{
    Player* bot = botAI->GetBot();
    if (!bot || !target)
        return false;

    if (!target->IsInWorld() || target->GetMapId() != bot->GetMapId())
        return false;

    if (bot->getClass() != CLASS_DRUID && bot->getClass() != CLASS_PALADIN &&
        GetPullActionName() == "shoot" && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
    {
        return false;
    }
    // [ammo84] a gun / bow / crossbow shoots only with its ammo in the bags (SPELL_FAILED_NO_AMMO): no ranged pull
    // without it. b410 rfc-n five 2: Brelir (warrior tank, Hand of Argus Crossfire, no bolts) - the planner's lone-mob
    // pull "at range" 13 times, each "shoot ... check 75 (no ammo)", "pull end: nothing came"; the run stalled at 0/4
    if (GetPullActionName() == "shoot")
        if (Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
            if (ItemTemplate const* proto = ranged->GetTemplate();
                proto->SubClass == ITEM_SUBCLASS_WEAPON_GUN || proto->SubClass == ITEM_SUBCLASS_WEAPON_BOW ||
                proto->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW)
            {
                // [ammo90] ... of the weapon's own kind: arrows for a bow / crossbow, bullets for a gun. b411 Ganna
                // (warrior tank, Fine Longbow) had Light Shot (bullets) set as its ammo: "shoot (3018) check 75" 1107
                // times, rfc-n five 2 stalled at 2/4 on "pull end: nothing came"
                uint32 const ammo = bot->GetUInt32Value(PLAYER_AMMO_ID);
                ItemTemplate const* ammoProto = ammo ? sObjectMgr->GetItemTemplate(ammo) : nullptr;
                uint32 const wanted =
                    proto->SubClass == ITEM_SUBCLASS_WEAPON_GUN ? ITEM_SUBCLASS_BULLET : ITEM_SUBCLASS_ARROW;
                if (!ammoProto || ammoProto->SubClass != wanted || !bot->HasItemCount(ammo, 1))
                    return false;
            }

    std::string const spellName = GetSpellName();
    if (spellName.empty())
        return false;

    return true;
}

void PullStrategy::RequestPull(Unit* target, bool resetTime, bool single)
{
    SetTarget(target);
    pendingToStart = true;
    if (resetTime)
    {
        pullStartTime = time(nullptr);
        singleMob = single;
        prePullBuffDone = false;  // [b31_holyshield_pull] a new pull: its own pre-pull buff
    }
}

void PullStrategy::OnPullStarted() { pendingToStart = false; }

void PullStrategy::OnPullEnded()
{
    pullStartTime = 0;
    pendingToStart = false;
    singleMob = false;
    prePullBuffDone = false;  // [b31_holyshield_pull]
    SetTarget(nullptr);
}

namespace
{
    // The tank's answers to a mob already on a member: the peel steps of the class strategies ("... on group member
    // attacker" and the like), the out-of-combat defence ("tank defend on attacked group member"), "tank assist",
    // "taunt reaction" and the finishers. PullMultiplier and MagePullMultiplier let them through during a pull.
    bool AnswersMobOnMember(std::string const& name)
    {
        auto const endsWith = [&name](char const* suffix)
        {
            size_t const length = std::strlen(suffix);
            return name.size() >= length && name.compare(name.size() - length, length, suffix) == 0;
        };
        return endsWith(" on group member attacker") || endsWith(" on attacked group member") ||
               endsWith(" on swarmed group member") || endsWith(" on group member attackers") ||
               name == "reach group member attacker" || name == "move to group member attackers" ||
               name == "reach healer attacker" || name == "tank assist" || name == "taunt reaction" ||
               name == "flee stop" || name == "finish fled survivor";
    }
}

PullMultiplier::PullMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "pull") {}

float PullMultiplier::GetValue(Action* action)
{
    uint32 const now = getMSTime();
    if (!checkedMs || getMSTimeDiff(checkedMs, now) >= PULL_CHECK_SAMPLE_MS)
    {
        checkedMs = now;
        PullCheck::Sample(botAI);
    }

    PullStrategy const* strategy = PullStrategy::Get(botAI);
    if (!strategy || !strategy->HasTarget() || !action)
        return 1.0f;

    if (!strategy->IsPullPendingToStart() && !strategy->HasPullStarted())
        return 1.0f;

    std::string const actionName = action->getName();
    bool const pullStep = actionName == "pull my target" || actionName == "pull rti target" ||
                          actionName == "reach pull" || actionName == "pull start" || actionName == "pull action" ||
                          actionName == "return to pull position";
    // A mob already on a member comes before the next pull: the tank defends first (dtk-n wave 10: "pull action"
    // at 105 kept casting at a Risen Drakkari Warrior while a Scourge Brute took the shaman from 100 to 11 %, then
    // the hunter; "tank defend on attacked group member" at 92 never ran, wipe). The pull waits; the planner plans
    // nothing new while a member fights (IsGroupFighting).
    if (pullStep && !bot->IsInCombat())
    {
        Unit* const attacker = AI_VALUE(Unit*, "defend member target");
        if (attacker && attacker != strategy->GetTarget())
            return 0.0f;
    }

    if (pullStep || actionName == "pull end" || actionName == "follow" || actionName == "set facing")
        return 1.0f;

    // A mob already on a member (an ambush from stealth, a patrol) is peeled during the pull: the taunts and the
    // threat strikes on it stay (Blood Furnace wave 25: Laughing Skull Rogue killed the healer 3 yd from the tank).
    auto const endsWith = [&actionName](char const* suffix)
    {
        size_t const length = std::strlen(suffix);
        return actionName.size() >= length && actionName.compare(actionName.size() - length, length, suffix) == 0;
    };
    if (AnswersMobOnMember(actionName))  // [b44_gerst] the list is shared with MagePullMultiplier
        return 1.0f;

    // [pull_aoe_threat] The pack arriving at the tank is threatened as it lands, not after "pull end": the first heal
    // or a damage dealer's opener took mobs held by proximity aggro alone ("tank aoe threat"). Not during the walk back
    // of a pull back (a Consecration left behind holds nothing).
    if ((endsWith(" tank aoe") || actionName == "death and decay" || actionName == "challenging shout" ||
         actionName == "challenging roar") &&
        !bot->isMoving())
        return 1.0f;

    return 0.0f;
}

class MagePullMultiplier : public PassiveMultiplier
{
public:
    MagePullMultiplier(PlayerbotAI* botAI, std::string const action) : PassiveMultiplier(botAI), actionName(action) {}

    float GetValue(Action* action) override;

private:
    std::string const actionName;
};

float MagePullMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    PullStrategy const* strategy = PullStrategy::Get(botAI);
    if (!strategy || !strategy->HasTarget())
        return 1.0f;

    std::string const name = action->getName();
    if (actionName == name || name == "pull action" || name == "pull start" || name == "pull end" ||
        name == "pull my target" || name == "pull rti target" ||
        name == "reach spell" || name == "reach pull" ||
        name == "return to pull position" || name == "follow" ||
        name == "set facing" || name == "change strategy")
        return 1.0f;

    // [b44_gerst] Every bot with the "pull" strategy carries this multiplier, tanks included, and while the pull
    // target is set it made every other action passive: the peel steps, "tank defend on attacked group member",
    // "tank assist" and "taunt reaction" that PullMultiplier lets through were zeroed here. brd-h
    // 20261026-060218-brd-h-5F-5 (484-494 s): Rtwarab's pull of an Anvilrage Warden 32.9 yd off, out of sight, sat
    // "out of pull range" while High Interrogator Gerstahn hit the hunter and killed the resto druid; the tank's
    // intents listed only isUseful-false steps (the zeroed ones sort last), "[peel diag] ... picked ... taunt ready",
    // no "[defend member]" line; "tank assist" ran at 494 s, once the pull had ended. A mob on a member now lets the
    // tank's answers through, as PullMultiplier does.
    if (AnswersMobOnMember(name) &&
        (AI_VALUE(Unit*, "defend member target") || AI_VALUE(Unit*, "group member attacker")))
        return 1.0f;

    return PassiveMultiplier::GetValue(action);
}

std::vector<NextAction> PullStrategy::getDefaultActions()
{
    return {
        NextAction("pull action", 105.0f),
    };
}

void PullStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "pull start",
        {
            NextAction("pull start", 106.0f),
            NextAction("pull action", ACTION_MOVE)
        }
    ));

    triggers.push_back(new TriggerNode(
        "pull end",
        {
            NextAction("pull end", 107.0f)
        }
    ));
}

void PullStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new PullMultiplier(botAI));
    multipliers.push_back(new MagePullMultiplier(botAI, action));
    multipliers.push_back(new TankWaitsForSittersMultiplier(botAI));  // [b32_engage]
}

// [b32_engage] ohb-h-5H-21 20261025-221216 pull #18: the harness waited ("drinking": Rthunah regen) while the
// tank's "dungeon event step" and "check mount state" walked it 46 yd on into the next pack; the hunter, drinking
// 63 yd behind, hit nothing for 85 s. bf-h-5H-84 20261025-223902 pull 18: the tank walked off while the priest sat,
// and pulled with the healer 48.6 yd away. Out of combat the tank does not walk on while a member near it rests
// below its line or buffs (PullReadiness::GroupPreparing), at most SITTER_HOLD_MAX_MS per rest.
float TankWaitsForSittersMultiplier::GetValue(Action* action)
{
    constexpr uint32 SITTER_CHECK_MS = 500;
    constexpr uint32 SITTER_HOLD_MAX_MS = 45 * IN_MILLISECONDS;
    Player* bot = botAI->GetBot();
    // a group a real player leads moves at the player's pace
    if (!action || bot->IsInCombat() || !PlayerbotAI::IsTank(bot) || !botAI->IsInNonRaidDungeon() ||
        IsRealPlayer(botAI->GetMaster()))
        return 1.0f;

    uint32 const now = getMSTime();
    if (!checkedMs || getMSTimeDiff(checkedMs, now) >= SITTER_CHECK_MS)
    {
        checkedMs = now;
        bool const preparing = PullReadiness::GroupPreparing(bot);
        if (!preparing)
            holdStartMs = 0;
        else if (!holdStartMs)
            holdStartMs = now ? now : 1;

        holding = preparing && getMSTimeDiff(holdStartMs, now) < SITTER_HOLD_MAX_MS;
    }

    if (!holding)
        return 1.0f;

    // [b44_gerst] A mob on a member is a fight that is on: the hold is for walking on, never for the tank's answer.
    // "tank defend on attacked group member" and "tank assist" are AttackActions (MovementActions), so a member
    // still buffing or drinking near the out-of-combat tank zeroed them here too (the same family as the
    // MagePullMultiplier veto, brd-h 20261026-060218-brd-h-5F-5, 484-494 s). Cached values.
    if (AI_VALUE(Unit*, "defend member target") || AI_VALUE(Unit*, "group member attacker"))
        return 1.0f;

    // Dodging, unsticking and the way out of water stay; every walk on (and the mount for it) waits.
    std::string const name = action->getName();
    if (name.find("avoid") != std::string::npos || name.find("flee") != std::string::npos ||
        name.find("stuck") != std::string::npos || name.find("hazard") != std::string::npos)
        return 1.0f;

    return dynamic_cast<MovementAction*>(action) || dynamic_cast<CheckMountStateAction*>(action) ? 0.0f : 1.0f;
}

void PossibleAddsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "possible adds",
            {
                NextAction("flee with pet", 60)
            }
        )
    );
}

void PullBackStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode(
        "return to pull position",
        {
            NextAction("return to pull position", ACTION_MOVE + 5.0f)
        }
    ));
}

void LosPullStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode(
        "los pull gather",
        {
            NextAction("move to los pull spot", ACTION_MOVE + 4.0f)
        }
    ));
}

void LosPullStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new LosPullHoldMultiplier(botAI));
}

float LosPullHoldMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    uint32 const now = getMSTime();
    if (!checkedMs || getMSTimeDiff(checkedMs, now) >= LOS_PULL_HOLD_CHECK_MS)
    {
        checkedMs = now;
        holding = LosPull::IsHolding(botAI->GetBot());
        // [lead1322] b255 rfc-h 5D-12 140-154 s (the healer's death): the in-fight caster corner held the bear
        // (its reach and its walk to a member's attackers off) while a Trogg and two Shamans killed the healer.
        // The hold waits for the pack to come to the tank; a mob already on another member does not come - the
        // tank's hold ends while one is
        Player* const bot = botAI->GetBot();
        if (holding && PlayerbotAI::IsTank(bot))
            if (Group* group = bot->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref && holding; ref = ref->next())
                    if (Player* member = ref->GetSource(); member && member != bot && member->IsAlive() &&
                                                           member->IsInMap(bot) && !member->getAttackers().empty())
                        holding = false;
    }

    if (!holding)
        return 1.0f;

    if (dynamic_cast<ReachPullAction*>(action) || dynamic_cast<ReachPartyMemberToHealAction*>(action) ||
        dynamic_cast<ReachPartyMemberToResurrectAction*>(action))
        return 1.0f;

    if (dynamic_cast<ReachTargetAction*>(action) || dynamic_cast<FollowAction*>(action) ||
        dynamic_cast<MoveToRangedPositionAction*>(action) || dynamic_cast<CombatFormationMoveAction*>(action) ||
        dynamic_cast<RearFlankAction*>(action) || dynamic_cast<MoveToGroupMemberAttackersAction*>(action))
        return 0.0f;

    return 1.0f;
}
