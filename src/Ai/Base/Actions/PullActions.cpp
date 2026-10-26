/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Ported from the CMaNGOS playerbots project (https://github.com/cmangos/playerbots), GPL v2,
 * with modifications for AzerothCore.
 * Original authors:
 *   David Parra Ausina (davidonete/Flekz) <davidparraausina@gmail.com> - original author
 *   Sebastiaan Keek (mostlikely4r) <sebastiaan.keek@gmail.com>
 *   Cyberium <cyberium@users.noreply.github.com>
 */

#include "PullActions.h"
#include "AggressiveTargetValue.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "AttackersValue.h"
#include "ChooseTargetActions.h"
#include "CreatureAI.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "PullStrategy.h"
#include "RtiTargetValue.h"
#include "Creature.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "LastMovementValue.h"  // [b38_scholo]
#include "PathGenerator.h"
#include "ArcShared.h"  // [b34_arc] Arcatraz::IsCorpseTrap
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace
{
float GetPullReachDistance(Player* bot, Unit* target, PullStrategy const* strategy)
{
    if (!bot || !target || !strategy)
        return 0.0f;

    float const combatDistance = bot->GetCombatReach() + target->GetCombatReach();
    return std::max(0.0f, strategy->GetRange() - combatDistance);
}

bool IsWithinPullRange(Player* bot, Unit* target, PullStrategy const* strategy)
{
    // In range means castable: in sight too (Zul'Farrak wave 27: the tank on the pyramid stairs, the Shadowcaster
    // 20 yd below, 446 failed Hand of Reckoning casts from "in range").
    return bot && target && strategy && bot->GetExactDist(target) <= strategy->GetRange() &&
           bot->IsWithinLOSInMap(target);
}

// [b34_wc] pull_abort_cap: every give-up of a started pull before the cast (no step closer, another floor, the walk
// leaves the healer) goes through here. NoteAborted alone held the group 10 s and blocked a ranged pull of that mob
// for 30 s, then the planner planned the same mob again: wc-h 20261026-014521-wc-h-5A-13 (520-1030 s) - Rtpalac's
// lone-mob pull of the Deviate Guardian (3637, Low 186) on the ledge by Lady Anacondra at (-10, 310, -88.5) walked
// into the pit below (z -106), was ended "out of sight on another floor" ([w24b-pull-floor-abort]) and planned again
// ~every 50 s, 163 pull_hold lines and 10 minutes with no progress. The [b32_smpull] cap counts only the "no step
// closer" give-ups from one spot. The PULL_ABORT_CAP-th abort of the same mob within PULL_ABORT_CAP_WINDOW_MS (any
// reason, any spot) stops chasing it: out of reach for the planner for PULL_ABORT_SKIP_MS (it plans another mob or
// holds with that reason, the harness takes another target and comes back to it later), logged with why.
constexpr uint32 PULL_ABORT_CAP = 3;
constexpr uint32 PULL_ABORT_CAP_WINDOW_MS = 300 * IN_MILLISECONDS;
constexpr uint32 PULL_ABORT_SKIP_MS = 180 * IN_MILLISECONDS;
struct PullAbortTally
{
    ObjectGuid mob;
    uint32 count = 0;
    uint32 firstMs = 0;
    bool sightOnly = true;  // [b37_sv] every abort of the tally was in range, out of sight, for the healer's sake
};
std::mutex pullAbortLock;
std::unordered_map<uint64, PullAbortTally> pullAborts;  // by tank (bots of several maps abort concurrently)

// [b37_sv] `sightOnly`: the abort was in the pull's range, out of the mob's sight, the step in for sight leaving the
// healer (ReachPullAction). sv-h 20261026-044253-sv-h-5B-21 (172-410 s): Rtwarae 23 yd from a Bog Overlord (21694,
// Low 116) behind a rock hump, in Shoot range, out of sight; three "pull back" plans each ended "walking for range
// leaves the healer" (the 8 yd step for sight), the cap marked it out of reach for 180 s, the planner held ("given up
// for range three times"), the harness skipped it and every mob behind it: 255 s idle. No range problem: such a cap
// leaves the mob to the engage (walk in / charge, the fight walked back into the healer's sight) instead.
void NotePullAbort(Player* bot, Unit* target, std::string const& why, bool sightOnly = false)
{
    CarefulPlanVerdict::NoteAborted(bot, target->GetGUID());

    uint32 const now = getMSTime();
    uint32 count = 0;
    uint32 sinceMs = 0;
    bool allSightOnly = false;
    {
        std::lock_guard<std::mutex> guard(pullAbortLock);
        PullAbortTally& tally = pullAborts[bot->GetGUID().GetRawValue()];
        if (tally.mob != target->GetGUID() || !tally.firstMs ||
            getMSTimeDiff(tally.firstMs, now) > PULL_ABORT_CAP_WINDOW_MS)
        {
            tally.mob = target->GetGUID();
            tally.count = 0;
            tally.firstMs = now ? now : 1;
            tally.sightOnly = true;
        }
        count = ++tally.count;
        tally.sightOnly = tally.sightOnly && sightOnly;
        allSightOnly = tally.sightOnly;
        sinceMs = getMSTimeDiff(tally.firstMs, now);
        if (count >= PULL_ABORT_CAP)
            tally = PullAbortTally();
    }

    LOG_INFO("playerbots", "[b34_wc] pull_abort: {}: pull of {} ({}) given up before the cast, {} of {} in {} s: {}",
             bot->GetName(), target->GetName(), target->GetGUID().GetCounter(), count, PULL_ABORT_CAP,
             sinceMs / IN_MILLISECONDS, why);
    if (count < PULL_ABORT_CAP)
        return;

    if (allSightOnly)  // [b37_sv]
    {
        LOG_INFO("playerbots", "[b37_sv] pull_abort_cap: {}: {} ({}) at {:.1f} yd, ranged pull aborted {} times in {} "
                 "s, each in range and out of sight (last: {}) from ({:.1f},{:.1f},{:.1f}): engage only for {} s",
                 bot->GetName(), target->GetName(), target->GetGUID().GetCounter(), bot->GetExactDist(target), count,
                 sinceMs / IN_MILLISECONDS, why, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                 PULL_ABORT_SKIP_MS / IN_MILLISECONDS);
        CarefulPlanVerdict::NoteEngageOnly(bot, target->GetGUID(), PULL_ABORT_SKIP_MS);
        return;
    }

    LOG_INFO("playerbots", "[b34_wc] pull_abort_cap: {}: {} ({}) at {:.1f} yd, pull aborted {} times in {} s (last: "
             "{}) from ({:.1f},{:.1f},{:.1f}): stops chasing it, out of reach for the planner for {} s",
             bot->GetName(), target->GetName(), target->GetGUID().GetCounter(), bot->GetExactDist(target), count,
             sinceMs / IN_MILLISECONDS, why, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
             PULL_ABORT_SKIP_MS / IN_MILLISECONDS);
    CarefulPlanVerdict::NotePullOutOfReach(bot, target->GetGUID(), PULL_ABORT_SKIP_MS);
}
}

namespace
{
constexpr float CHAIN_PULL_RANGE = 30.0f;                        // the next pull mob is this close to the tank
constexpr float CHAIN_PULL_PACK_RADIUS = 10.0f;                  // idle mobs this close to it come with it
constexpr uint32 CHAIN_PULL_MAX_MOBS = 2;                        // a single or a small pack only
constexpr size_t CHAIN_PULL_MAX_CHECKS = 3;                      // the nearest candidates get the full check
constexpr uint32 CHAIN_PULL_COOLDOWN_MS = 10 * IN_MILLISECONDS;  // one chained pull, then it has to come
constexpr float CHAIN_PULL_TANK_HEALTH = 60.0f;                  // the tank's own floor, as the planner's [chain_pull]
// [chain_pull_gate] no other idle mob within this of the next pull (its assistance, a neighbour pack, a patrol)
constexpr float CHAIN_PULL_CLEAR_RADIUS = 30.0f;

bool IsChainPullBoss(Unit* unit)
{
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    return creature && (creature->IsDungeonBoss() || creature->isWorldBoss() ||
                        creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS);
}
}

bool ChainPullAction::isUseful()
{
    if (!PlayerbotAI::IsTank(bot) || !bot->IsInCombat() || bot->GetHealthPct() < CHAIN_PULL_TANK_HEALTH)
        return false;

    return !_lastChainMs || getMSTimeDiff(_lastChainMs, getMSTime()) >= CHAIN_PULL_COOLDOWN_MS;
}

bool ChainPullAction::GroupAboveRestLines(std::string& below)
{
    auto const above = [&below](Player* member)
    {
        if (!member->IsAlive() || member->IsSitState() || RestState::WantsDrink(member) ||
            RestState::WantsFood(member))
            below = member->GetName();
        return below.empty();
    };

    if (!above(bot))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;

        // a member elsewhere (another map, far behind) is no group to chain a pull for
        if (!member->IsInWorld() || member->GetMap() != bot->GetMap() ||
            bot->GetExactDist(member) > sPlayerbotAIConfig.SightDistance)
        {
            below = member->GetName();
            return false;
        }

        if (!above(member))
            return false;
    }

    return true;
}

Unit* ChainPullAction::FindNext(Unit* fighting, std::string& spellName, uint32& packMobs)
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return nullptr;

    GuidVector const possible = AI_VALUE(GuidVector, "possible targets");
    std::vector<std::pair<float, Unit*>> candidates;
    for (ObjectGuid const guid : possible)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == fighting || !unit->IsAlive() || unit->IsInCombat() || !unit->ToCreature() ||
            !bot->IsValidAttackTarget(unit) || IsChainPullBoss(unit))
            continue;
        // [b34_arc] a corpse trap is no chain pull: the shot leaves it in combat unengaged (Arcatraz::IsCorpseTrap)
        if (Arcatraz::IsCorpseTrap(unit))
            continue;

        float const distance = bot->GetExactDist(unit);
        if (distance <= CHAIN_PULL_RANGE)
            candidates.emplace_back(distance, unit);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](auto const& left, auto const& right) { return left.first < right.first; });

    // the pack is counted round the corner too: a mob out of the tank's sight still comes with it
    GuidVector const unseen = AI_VALUE(GuidVector, "possible targets no los");
    Value<ObjectGuid>* pullTarget = context->GetValue<ObjectGuid>("pull strategy target");
    size_t checks = 0;
    for (auto const& candidate : candidates)
    {
        if (checks++ >= CHAIN_PULL_MAX_CHECKS)
            break;

        Unit* next = candidate.second;
        uint32 mobs = 1;
        bool boss = false;
        for (ObjectGuid const guid : unseen)
        {
            Unit* mate = botAI->GetUnit(guid);
            if (!mate || mate == next || mate == fighting || !mate->IsAlive() || mate->IsInCombat() ||
                !mate->ToCreature() || mate->GetExactDist(next) > CHAIN_PULL_PACK_RADIUS)
                continue;

            ++mobs;
            boss = boss || IsChainPullBoss(mate);
        }

        if (boss || mobs > CHAIN_PULL_MAX_MOBS)
            continue;

        // [chain_pull_gate] a single or a pair standing apart: any other idle mob within CHAIN_PULL_CLEAR_RADIUS of
        // it (round the corner included) may come with it, and the chain is no small pull any more (sh-h / scholo-n
        // 2026-10-25: chained fights of 20+ kills in 84-186 s ended in wipes).
        Unit* neighbour = nullptr;
        for (ObjectGuid const guid : unseen)
        {
            Unit* mob = botAI->GetUnit(guid);
            if (!mob || mob == next || mob == fighting || !mob->IsAlive() || mob->IsInCombat() || !mob->ToCreature())
                continue;

            float const gap = mob->GetExactDist(next);
            if (gap > CHAIN_PULL_PACK_RADIUS && gap <= CHAIN_PULL_CLEAR_RADIUS)
            {
                neighbour = mob;
                break;
            }
        }
        if (neighbour)
        {
            LOG_DEBUG("playerbots", "[chain_pull_gate] {}: no chain on {}: idle {} {:.1f} yd from it", bot->GetName(),
                      next->GetName(), neighbour->GetName(), neighbour->GetExactDist(next));
            continue;
        }

        // the tank's own pick rules: careful pull skip, encounter order, a walkable path
        if (AggressiveTargetValue::Explain(bot, next).compare(0, 8, "accepted") != 0)
            continue;

        // The class strategies pick the spell for the mob in "pull strategy target"; set only while asking.
        if (pullTarget)
            pullTarget->Set(next->GetGUID());
        bool const wasSingle = strategy->IsSingleMob();
        strategy->SetSingleMob(mobs == 1);
        bool const usable = strategy->CanDoPullAction(next);
        std::string const spell = strategy->GetSpellName();
        strategy->SetSingleMob(wasSingle);
        if (pullTarget)
            pullTarget->Set(ObjectGuid::Empty);

        if (!usable || spell.empty() || !botAI->CanCastSpell(spell, next))
            continue;

        uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
        if (mobs == 1 && PullStrategy::HitsMoreThanOne(sSpellMgr->GetSpellInfo(spellId)))
            continue;

        spellName = spell;
        packMobs = mobs;
        return next;
    }

    return nullptr;
}

bool ChainPullAction::Execute(Event /*event*/)
{
    PullStrategy const* strategy = PullStrategy::Get(botAI);
    if (!strategy || strategy->HasTarget() || strategy->IsPullPendingToStart() || strategy->HasPullStarted())
        return false;

    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    if (attackers.size() != 1)
        return false;

    Unit* fighting = botAI->GetUnit(attackers.front());
    if (!fighting || !fighting->IsAlive() || IsChainPullBoss(fighting))
        return false;

    std::string below;
    if (!GroupAboveRestLines(below))
        return false;

    std::string spellName;
    uint32 packMobs = 0;
    Unit* next = FindNext(fighting, spellName, packMobs);
    if (!next)
        return false;

    ObjectGuid const selected = bot->GetTarget();
    if (!botAI->CastSpell(spellName, next))
        return false;

    // the tank stays on the mob it fights: the pull is a cast, not a switch
    if (Unit* victim = bot->GetVictim())
        bot->SetSelection(victim->GetGUID());
    else if (!selected.IsEmpty())
        bot->SetSelection(selected);

    _lastChainMs = getMSTime();
    LOG_INFO("playerbots", "[chain_pull_in_combat] {}: {} on {} ({} mobs, {:.1f} yd) while {} is at {:.0f} %",
             bot->GetName(), spellName, next->GetName(), packMobs, bot->GetExactDist(next), fighting->GetName(),
             fighting->GetHealthPct());
    return true;
}

bool PullRequestAction::Execute(Event event)
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return false;

    if (!PlayerbotAI::IsTank(bot))
        return false;

    Unit* target = GetPullTarget(event);
    if (!target || !target->IsInWorld())
    {
        std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pull_no_target_error", "You have no target", {});
        botAI->TellError(text);
        return false;
    }

    float const maxPullDistance = sPlayerbotAIConfig.ReactDistance * 3.0f;
    if (target->GetMapId() != bot->GetMapId() || bot->GetDistance(target) > maxPullDistance)
    {
        std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pull_target_too_far_error", "The target is too far away", {});
        botAI->TellError(text);
        return false;
    }

    if (!AttackersValue::IsPossibleTarget(target, bot))
    {
        std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pull_invalid_target_error", "The target can't be pulled", {});
        botAI->TellError(text);
        return false;
    }

    if (!strategy->CanDoPullAction(target))
    {
        std::string const actionName = strategy->GetPullActionName();
        std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pull_action_unavailable_error",
            "Can't perform pull action '%action_name'",
            {{"%action_name", actionName}});
        botAI->TellError(text);
        return false;
    }

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo pullPosition = posMap["pull"];
    pullPosition.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
    posMap["pull"] = pullPosition;

    strategy->RequestPull(target);
    context->GetValue<Unit*>("current target")->Set(target);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    botAI->SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
    return true;
}

Unit* PullMyTargetAction::GetPullTarget(Event event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (event.GetSource() == "attack anything")
        return botAI->GetCreature(event.getObject());

    return requester ? requester->GetSelectedUnit() : nullptr;
}

Unit* PullRtiTargetAction::GetPullTarget(Event /*event*/)
{
    Unit* rtiTarget = AI_VALUE(Unit*, "rti target");
    if (rtiTarget)
        return rtiTarget;

    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    std::string const rti = AI_VALUE(std::string, "rti");
    int32 const index = RtiTargetValue::GetRtiIndex(rti);
    if (index < 0)
        return nullptr;

    ObjectGuid const guid = group->GetTargetIcon(index);
    return guid.IsEmpty() ? nullptr : botAI->GetUnit(guid);
}

bool PullStartAction::Execute(Event event)
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return false;

    Unit* target = strategy->GetTarget();
    if (!target)
        return false;

    std::string const preActionName = strategy->GetPreActionName();
    if (!preActionName.empty() && !botAI->DoSpecificAction(preActionName, event, true))
        return false;

    if (Pet* pet = bot->GetPet())
    {
        Creature* creature = pet->ToCreature();
        if (creature)
        {
            strategy->SetPetReactState(creature->GetReactState());
            creature->SetReactState(REACT_PASSIVE);
        }
    }

    strategy->OnPullStarted();
    return true;
}

PullAction::PullAction(PlayerbotAI* botAI, std::string const name) : CastSpellAction(botAI, name) { InitPullAction(); }

Unit* PullAction::GetTarget()
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return nullptr;

    return strategy->GetTarget();
}

std::vector<NextAction> PullAction::getPrerequisites()
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    Unit* target = strategy ? strategy->GetTarget() : nullptr;
    if (!strategy || !target)
        return {};

    // [b38_scholo] a target already in combat (the pull landed, it comes) needs no walk for range: Execute ends it
    if (target->IsInCombat())
        return {};

    return IsWithinPullRange(bot, target, strategy) ? std::vector<NextAction>{}
                                                     : std::vector<NextAction>{ NextAction("reach pull", ACTION_MOVE) };
}

bool PullAction::Execute(Event event)
{
    InitPullAction();

    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return false;

    Unit* target = strategy->GetTarget();
    if (!target || !target->IsInWorld())
        return false;

    if (target->IsInCombat())
    {
        // Somebody else engaged it first (a patrol, a dps, a pet): nothing to pull any more. The pending pull ended here,
        // or the pull multiplier kept the tank out of the fight until the pull timed out (wave 35: 100+ such ticks per
        // tank, the groups made a fraction of their usual progress).
        LOG_DEBUG("playerbots", "[pull exec] {}: {} already in combat, pull ended", bot->GetName(), target->GetName());
        // [b38_scholo] ...and the pull's own walk toward it ends here: the mob comes to the tank. scholo-h
        // 20261026-050341-scholo-h-5B-3 pull 28: a lone-mob "pull back" (expects 7); the Shoot landed, the mob turned
        // out of sight, "reach pull" walked the out-of-sight path (ShortenPathUntilDist keeps the last point that
        // sees the mob: 2 yd from its spot at (252.9,149.6)) and the spline ran on 5 s after "already in combat":
        // the tank stood in the Ghoul room with 9 -> 14 attackers, then the tank, the healer and the shaman died.
        LosPull::Plan losPlan;
        bool const ownLosPull =
            LosPull::Get(bot, losPlan) && losPlan.tank == bot->GetGUID() && losPlan.target == target->GetGUID();
        if (PlayerbotAI::IsTank(bot) && bot->isMoving() && !ownLosPull)
        {
            LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");
            float const endToMob = lastMove.lastMoveShort.GetExactDist(target->GetPositionX(), target->GetPositionY(),
                                                                       target->GetPositionZ());
            if (endToMob < bot->GetExactDist(target))
            {
                LOG_INFO("playerbots", "[b38_scholo] pull_walk_stop: {}: {} in combat ({:.1f} yd), the walk toward it "
                         "(ends {:.1f} yd from it) stops: it comes", bot->GetName(), target->GetName(),
                         bot->GetExactDist(target), endToMob);
                bot->StopMoving();
                lastMove.lastdelayTime = 0.0f;
            }
        }
        // [b41_scholo] The tank's own line-of-sight pull, cast already: "in combat" is the pull landing, not someone
        // else's engage. Its end is the pull-end trigger's (the pack came round the corner: plan.arrived, or the
        // target in melee). Until then "return to pull position" walks the tank back to its hidden spot. scholo-n
        // 20261026-063902-scholo-n-5H-4 (227-372 s): "[los pull] ... Scholomance Neophyte: 1 mobs ... back to the spot
        // 10.2 yd away", then "already in combat, pull ended (0 s)". The pull target was cleared, so
        // ReturnToPullPositionAction (it needs it) never ran. The tank stood where it had cast, 26.7 yd in the
        // caster's sight. The Neophyte cast from its spot by the hall's next packs, an Acolyte joined, the fight
        // hold walked the tank to its edge in the hall, and 3 -> 11 attackers killed the druid, priest and mage.
        if (ownLosPull && losPlan.castMs && !losPlan.arrived && !bot->IsWithinMeleeRange(target))
        {
            LOG_DEBUG("playerbots", "[b41_scholo] los_pull_kept: {}: {} in combat after the cast ({:.1f} yd), the los "
                      "pull goes on: back to the spot {:.1f} yd away", bot->GetName(), target->GetName(),
                      bot->GetExactDist(target),
                      bot->GetExactDist(losPlan.spot.GetPositionX(), losPlan.spot.GetPositionY(),
                                        losPlan.spot.GetPositionZ()));
            return false;
        }
        botAI->DoSpecificAction("pull end", event, true);
        return false;
    }

    if (!IsWithinPullRange(bot, target, strategy))
    {
        // [pull_out_of_range_loop] Out of range is the "reach pull" prerequisite's to fix. When that walk does not
        // come (refused, vetoed, blocked), the pull asked itself again every tick for good: fos-n w24
        // (20261025-123018-fos-n-5A-84) logged "Soul Horror out of pull range" 310 times, the harness withheld the
        // route for the running pull. With no step closer for PULL_NO_PROGRESS_MS the pull is given up
        // (NoteAborted, "pull end"): the planner plans again, from another spot or another mob.
        constexpr uint32 PULL_NO_PROGRESS_MS = 4 * IN_MILLISECONDS;
        constexpr float PULL_PROGRESS_STEP = 1.0f;
        uint32 const now = getMSTime();
        float const distance = bot->GetExactDist(target);
        if (_outOfRangeFor != target->GetGUID() || !_outOfRangeSinceMs ||
            distance < _outOfRangeBest - PULL_PROGRESS_STEP)
        {
            _outOfRangeFor = target->GetGUID();
            _outOfRangeSinceMs = now ? now : 1;
            _outOfRangeBest = distance;
        }
        else if (getMSTimeDiff(_outOfRangeSinceMs, now) >= PULL_NO_PROGRESS_MS && !bot->isMoving())
        {
            LOG_INFO("playerbots", "[pull_out_of_range_loop] {}: {} out of pull range ({:.1f} yd, spell {}) for {} s "
                     "without a step closer: pull ended, planned again", bot->GetName(), target->GetName(), distance,
                     strategy->GetSpellName(), getMSTimeDiff(_outOfRangeSinceMs, now) / IN_MILLISECONDS);
            _outOfRangeFor.Clear();
            _outOfRangeSinceMs = 0;
            NotePullAbort(bot, target, Acore::StringFormat("no step closer ({:.1f} yd)", distance));  // [b34_wc]
            // [b32_smpull] The give-up alone only re-planned the same pull: sm-h 20261025-221216-sm-h-5E-14, "Scarlet
            // Commander Mograine out of pull range (33.0 yd, spell hand of reckoning)" 48 times and "pull ended,
            // planned again" 6 times from one spot (the Mograine keep-away hold vetoed every walk for range) until
            // the harness skipped him as "route end". The third give-up of the same mob from about the same spot
            // (PULL_GIVE_UP_SPOT yd, PULL_GIVE_UP_WINDOW_MS apart at most) makes it out of reach for the planner for
            // PULL_OUT_OF_REACH_MS: it plans another mob (the linked trash first) or holds with that reason.
            constexpr uint32 PULL_GIVE_UP_LIMIT = 3;
            constexpr uint32 PULL_GIVE_UP_WINDOW_MS = 60 * IN_MILLISECONDS;
            constexpr float PULL_GIVE_UP_SPOT = 5.0f;
            constexpr uint32 PULL_OUT_OF_REACH_MS = 90 * IN_MILLISECONDS;
            bool const sameStand = _giveUpFor == target->GetGUID() && _giveUpMs &&
                                   getMSTimeDiff(_giveUpMs, now) <= PULL_GIVE_UP_WINDOW_MS &&
                                   bot->GetExactDist2d(_giveUpX, _giveUpY) <= PULL_GIVE_UP_SPOT;
            _giveUpCount = sameStand ? _giveUpCount + 1 : 1;
            _giveUpFor = target->GetGUID();
            _giveUpMs = now ? now : 1;
            _giveUpX = bot->GetPositionX();
            _giveUpY = bot->GetPositionY();
            if (_giveUpCount >= PULL_GIVE_UP_LIMIT)
            {
                LOG_INFO("playerbots", "[b32_smpull] pull_out_of_reach: {}: {} given up {} times for range ({:.1f} yd, "
                         "spell {}) from ({:.1f},{:.1f},{:.1f}): out of reach for the planner for {} s",
                         bot->GetName(), target->GetName(), _giveUpCount, distance, strategy->GetSpellName(),
                         bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                         PULL_OUT_OF_REACH_MS / IN_MILLISECONDS);
                CarefulPlanVerdict::NotePullOutOfReach(bot, target->GetGUID(), PULL_OUT_OF_REACH_MS);
                _giveUpCount = 0;
                _giveUpFor.Clear();
            }
            botAI->DoSpecificAction("pull end", event, true);
            return false;
        }

        LOG_DEBUG("playerbots", "[pull exec] {}: {} out of pull range ({:.1f} yd, spell {})", bot->GetName(),
                  target->GetName(), distance, strategy->GetSpellName());
        strategy->RequestPull(target, false);
        return false;
    }

    _outOfRangeFor.Clear();
    _outOfRangeSinceMs = 0;

    if (bot->isMoving())
    {
        bot->StopMoving();
        strategy->RequestPull(target, false);
        return false;
    }

    // Outdoor instances allow mounts: the pull spell fails with SPELL_FAILED_NOT_MOUNTED from the saddle (Zul'Farrak
    // wave 31: 446 failed Hand of Reckoning casts). A tank dismounts before it pulls.
    if (bot->IsMounted())
    {
        bot->Dismount();
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        strategy->RequestPull(target, false);
        return false;
    }

    // A line-of-sight pull: the group takes its spot behind the corner first, for a few seconds at most.
    LosPull::Plan plan;
    bool const losPull = LosPull::Get(bot, plan) && plan.tank == bot->GetGUID() && plan.target == target->GetGUID();
    if (losPull && !plan.castMs)
    {
        if (!plan.waitMs)
            strategy->RestartPullTimer();  // the walk to the cast spot does not eat into the pull time

        uint32 const waited = LosPull::BeginWait(bot);
        if (!plan.gathered && waited < LosPull::GATHER_WAIT_MS)
            return false;
    }

    // [b31_holyshield_pull] The pull commit: every wait is behind (the planner's group-ready, crowd control and
    // laggard holds before RequestPull, the walk, the line-of-sight gather above). The class's pre-pull buff goes
    // first, one GCD, once per pull; the pull spell follows on the next tick. A buff its action finds useless (aura
    // up, low mana) or that fails leaves the pull to go on now.
    if (!bot->IsInCombat() && !strategy->IsPrePullBuffDone())
    {
        std::string const buff = strategy->GetPrePullBuffActionName();
        if (!buff.empty() && botAI->DoSpecificAction(buff, event, true))
        {
            strategy->SetPrePullBuffDone();
            return true;
        }
    }

    context->GetValue<Unit*>("current target")->Set(target);
    bool const engaged = bot->IsInCombat();
    if (!botAI->DoSpecificAction(strategy->GetCastActionName(), event, true))
    {
        uint32 const spellId = AI_VALUE2(uint32, "spell id", strategy->GetSpellName());
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
        {
            Spell* probe = new Spell(bot, info, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
            probe->m_targets.SetUnitTarget(target);
            SpellCastResult const result = probe->CheckCast(true);
            delete probe;
            LOG_DEBUG("playerbots", "[pull exec] {}: {} ({}) check {}, immune {}, power {}/{}", bot->GetName(),
                      strategy->GetSpellName(), spellId, uint32(result), target->IsImmunedToSpell(info),
                      bot->GetPower(bot->getPowerType()), info->CalcPowerCost(bot, info->GetSchoolMask()));
        }
        Action* const cast = context->GetAction(strategy->GetCastActionName());
        LOG_DEBUG("playerbots", "[pull exec] {}: {} useful {}, possible {}, can cast {}, selected {}", bot->GetName(),
                  strategy->GetCastActionName(), cast && cast->isUseful(), cast && cast->isPossible(),
                  botAI->CanCastSpell(strategy->GetSpellName(), target),
                  bot->GetSelectedUnit() ? bot->GetSelectedUnit()->GetName() : std::string("none"));
        LOG_DEBUG("playerbots", "[pull exec] {}: {} on {} failed ({:.1f} yd, los {}, cooldown {}, known {}, dz {:.1f})",
                  bot->GetName(), strategy->GetCastActionName(), target->GetName(), bot->GetExactDist(target),
                  bot->IsWithinLOSInMap(target), spellId && bot->HasSpellCooldown(spellId), spellId != 0,
                  target->GetPositionZ() - bot->GetPositionZ());
        return false;
    }

    LOG_DEBUG("playerbots", "[pull exec] {}: {} on {} cast ({:.1f} yd)", bot->GetName(), strategy->GetCastActionName(),
              target->GetName(), bot->GetExactDist(target));

    PullCheck::NoteSpell(bot, engaged ? std::string() : strategy->GetSpellName());

    if (losPull)
    {
        LosPull::MarkCast(bot);
        strategy->RestartPullTimer();  // the pack still has to walk round the corner
        LOG_DEBUG("playerbots", "[los pull] {} pulls {} with {} (group gathered: {}), back to the spot {:.1f} yd away",
                  bot->GetName(), target->GetName(), strategy->GetCastActionName(), plan.gathered,
                  bot->GetExactDist(plan.spot.GetPositionX(), plan.spot.GetPositionY(), plan.spot.GetPositionZ()));
    }
    else if (PlayerbotAI::IsTank(bot) && botAI->HasStrategy("pull back", BOT_STATE_COMBAT))
    {
        // The pull lands: a mob that walks about and can reach the pull position while the fight lasts moves the
        // fight further back (FightSpot::Review), and the tank steps back there ("return to pull position").
        PositionMap& posMap = AI_VALUE(PositionMap&, "position");
        PositionInfo pullPosition = posMap["pull"];
        if (pullPosition.isSet() && pullPosition.mapId == bot->GetMapId())
        {
            Position spot(pullPosition.x, pullPosition.y, pullPosition.z);
            if (FightSpot::Review(botAI, bot, target, spot))
            {
                pullPosition.Set(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), bot->GetMapId());
                posMap["pull"] = pullPosition;
            }
        }
    }

    return true;
}

// The engine asks isUseful before isPossible: the spell of this pull (it changes with the target and the
// single-mob mode) is set first, or "spell cast useful" judged the spell of an earlier pull (wave 19: the pull
// action was USELESS and lone-mob pulls never left the planner).
bool PullAction::isUseful()
{
    InitPullAction();
    return CastSpellAction::isUseful();
}

bool PullAction::isPossible()
{
    InitPullAction();

    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return false;

    Unit* target = strategy->GetTarget();
    std::string const spellName = strategy->GetSpellName();
    if (!target || !target->IsInWorld() || target->GetMapId() != bot->GetMapId() || spellName.empty())
        return false;

    return true;
}

void PullAction::InitPullAction()
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return;

    std::string const spellName = strategy->GetSpellName();
    if (spellName.empty())
        return;

    spell = spellName;

    bool isShoot = (spellName == "shoot" || spellName == "shoot bow" ||
                    spellName == "shoot gun" || spellName == "shoot crossbow" ||
                    spellName == "throw");
    range = botAI->GetRange(isShoot ? "shoot" : "spell");
}

bool PullEndAction::Execute(Event /*event*/)
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return false;

    Unit* pullTarget = strategy->GetTarget();

    if (!strategy->HasPullStarted() && !strategy->IsPullPendingToStart() && !strategy->HasTarget())
        return false;

    if (Pet* pet = bot->GetPet())
    {
        Creature* creature = pet->ToCreature();
        if (creature)
            creature->SetReactState(strategy->GetPetReactState());
    }

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo pullPosition = posMap["pull"];
    if (pullPosition.isSet())
        posMap.erase("pull");

    if (pullTarget && context->GetValue<Unit*>("current target")->Get() == pullTarget)
        context->GetValue<Unit*>("current target")->Set(nullptr);

    LOG_DEBUG("playerbots", "[pull exec] {}: pull ended (target {}, started {}, {} s)", bot->GetName(),
              pullTarget ? pullTarget->GetName() : "none", strategy->HasPullStarted(),
              strategy->GetPullStartTime() ? int64(time(nullptr) - strategy->GetPullStartTime()) : -1);
    LosPull::Cancel(bot);  // only a plan that was never pulled; a pulled one holds until the pack came round
    strategy->OnPullEnded();
    return true;
}

bool ReturnToPullPositionAction::Execute(Event /*event*/)
{
    PositionInfo pullPosition = AI_VALUE(PositionMap&, "position")["pull"];
    if (!pullPosition.isSet() || pullPosition.mapId != bot->GetMapId())
        return false;

    return MoveTo(pullPosition.mapId, pullPosition.x, pullPosition.y, pullPosition.z,
                  false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true);
}

bool ReturnToPullPositionAction::isUseful()
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    Unit* target = strategy ? strategy->GetTarget() : nullptr;
    if (!strategy || !target || !target->IsInCombat())
        return false;

    PositionInfo pullPosition = AI_VALUE(PositionMap&, "position")["pull"];
    return pullPosition.isSet() && pullPosition.mapId == bot->GetMapId() &&
           bot->GetDistance(pullPosition.x, pullPosition.y, pullPosition.z) > sPlayerbotAIConfig.FollowDistance;
}

bool MoveToLosPullSpotAction::Execute(Event /*event*/)
{
    LosPull::Plan plan;
    if (!LosPull::Get(bot, plan) || plan.tank == bot->GetGUID())
        return false;

    return MoveTo(bot->GetMapId(), plan.gather.GetPositionX(), plan.gather.GetPositionY(), plan.gather.GetPositionZ(),
                  false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true);
}

bool MoveToLosPullSpotAction::isUseful()
{
    LosPull::Plan plan;
    return bot->IsAlive() && LosPull::Get(bot, plan) && plan.tank != bot->GetGUID() &&
           LosPull::Holds(bot, plan) &&  // [b32_engage] a melee dps goes once the pack is in the tank's melee
           bot->GetExactDist2d(plan.gather.GetPositionX(), plan.gather.GetPositionY()) > LosPull::GATHER_STAND;
}

bool ReachPullAction::Execute(Event event)
{
    Unit* target = GetTarget();
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!target || !strategy)
        return false;

    // [b32_tank_aoe] the tank's form first (one GCD), the walk on the next tick; a form that fails leaves the walk on
    if (!bot->IsInCombat())
    {
        std::string const form = strategy->GetApproachFormActionName();
        if (!form.empty() && botAI->DoSpecificAction(form, event, true))
            return true;
    }

    float reachDistance = GetPullReachDistance(bot, target, strategy);
    // [w24c-reach-pull-margin] The walk ends this far inside the pull's own range gate (IsWithinPullRange, exact
    // distance <= range): a walk to exactly the gate ended 6 mm outside it (fos-n 20261025-123018: Rtpalat at 29.506
    // yd of Soul Horror, gate 29.5), MoveTo dropped the sub-centimetre step and the pull looped "reach pull" /
    // "pull start" until it timed out.
    constexpr float PULL_REACH_MARGIN = 1.0f;
    if (bot->GetExactDist(target) <= strategy->GetRange() + PULL_REACH_MARGIN &&
        !IsWithinPullRange(bot, target, strategy))
        LOG_DEBUG("playerbots", "[w24c-reach-pull-margin] {}: {} at {:.2f} yd, gate {:.2f}: steps {:.1f} yd in",
                  bot->GetName(), target->GetName(), bot->GetExactDist(target), strategy->GetRange(),
                  PULL_REACH_MARGIN);
    reachDistance = std::max(0.0f, reachDistance - PULL_REACH_MARGIN);
    // Out of sight within range: step in until the line of sight opens, on the same floor only. A mob below or above
    // (LBRS wave 35: a Legionnaire under Dragonspire Hall) would lead the tank up the stairs into another wing: the
    // pull ends and the planner picks another mob.
    if (!bot->IsWithinLOSInMap(target))
    {
        if (std::fabs(target->GetPositionZ() - bot->GetPositionZ()) > 5.0f)
        {
            // [w24b-pull-slope] A mob down a ramp is on the tank's floor when the walk to it is about the straight
            // line; another floor (the stairs round to it) makes the walk much longer. Botanica bot-n 2026-10-25: the
            // tank on the ramp at z -23.4, Frayer Wildlings at -29.0 (dz 5.6) 29-37 yd off behind its crest: every
            // lone-mob pull ended here unseen and unlogged, 60+ plans in 186 s, the group stood to the stop.
            constexpr float PULL_SLOPE_DETOUR = 1.3f;  // walk / straight distance of a slope
            constexpr float PULL_SLOPE_SLACK = 5.0f;   // yd: the path's corners
            constexpr uint32 PULL_SLOPE_RECHECK_MS = 3 * IN_MILLISECONDS;
            constexpr uint32 PULL_FLOOR_LOG_MS = 10 * IN_MILLISECONDS;
            float const straight = bot->GetExactDist(target);
            uint32 const now = getMSTime();
            if (_slopeTarget != target->GetGUID() || !_slopeCheckMs ||
                getMSTimeDiff(_slopeCheckMs, now) >= PULL_SLOPE_RECHECK_MS)
            {
                PathGenerator path(bot);
                path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
                _slopeTarget = target->GetGUID();
                _slopeCheckMs = now ? now : 1;
                _slopeWalk = path.getPathLength();
                _slopeOk = path.GetPathType() == PATHFIND_NORMAL &&
                           _slopeWalk <= straight * PULL_SLOPE_DETOUR + PULL_SLOPE_SLACK;
            }
            bool const logDue = !_floorLogMs || getMSTimeDiff(_floorLogMs, now) >= PULL_FLOOR_LOG_MS;
            if (logDue)
                _floorLogMs = now ? now : 1;
            if (!_slopeOk)
            {
                if (logDue)
                    LOG_INFO("playerbots", "[w24b-pull-floor-abort] {}: {} out of sight on another floor ({:.1f} yd, "
                             "dz {:.1f}, walk {:.1f} yd): pull ended", bot->GetName(), target->GetName(), straight,
                             target->GetPositionZ() - bot->GetPositionZ(), _slopeWalk);
                NotePullAbort(bot, target,  // [b34_wc]
                              Acore::StringFormat("out of sight on another floor (dz {:.1f}, walk {:.1f} yd)",
                                                  target->GetPositionZ() - bot->GetPositionZ(), _slopeWalk));
                strategy->OnPullEnded();
                return false;
            }
            if (logDue)
                LOG_INFO("playerbots", "[w24b-pull-slope] {}: {} out of sight down a slope ({:.1f} yd, dz {:.1f}, "
                         "walk {:.1f} yd): steps in for sight", bot->GetName(), target->GetName(), straight,
                         target->GetPositionZ() - bot->GetPositionZ(), _slopeWalk);
        }
        reachDistance = std::max(ATTACK_DISTANCE, std::min(reachDistance, bot->GetExactDist(target) - 8.0f));
    }

    // The careful plan's cast spot (checked for the healer's sight, PullCastSpot): the walk for range goes there.
    // At the spot (or with none) the walk below on the line to the mob.
    constexpr float CAST_SPOT_ARRIVED = 2.0f;
    Position castSpot;
    bool hasCastSpot = PlayerbotAI::IsTank(bot) && PullCastSpot::Get(bot, target->GetGUID(), castSpot) &&
                       bot->GetExactDist(castSpot) > CAST_SPOT_ARRIVED;
    // [b34_wc] cast_spot_off_floor: the plan's cast spot is the point at the spell's range on the straight line to
    // the mob (PullBackSpot), its height interpolated - over a pit it hangs in the air. wc-h
    // 20261026-014521-wc-h-5A-13: from the corridor at (-40, 267.5, -92.6) the Deviate Guardian's spot on the ledge
    // (-8, 310, -88.7) put the cast spot at about (-25.6, 286.5, -90.9) over the pit; MoveTo grounded it to
    // (-24.8, 285.2, -105.9), 15 yd lower, and the walk ran 60 yd south down the ramp and back through the pit (the
    // tank 37 -> 97 yd from the mob), where the pull ended as "another floor". The navmesh joins the corridor to the
    // ledge directly (tools/navmesh_probe.py 43 -40 267.5 -92.6 -10 310 -88.5 --route: CONNECTED along z -92..-89;
    // clean runs pulled from (-41.4, 298.2, -90.6)).
    // A cast spot whose ground lies CAST_SPOT_DROP_DZ below it, or whose walk is longer than the walk to the mob
    // itself (not on the way in), is dropped: the walk for range follows the path to the mob (ReachCombatTo below).
    if (hasCastSpot)
    {
        constexpr float CAST_SPOT_DROP_DZ = 5.0f;
        constexpr float CAST_SPOT_WALK_SLACK = 10.0f;  // yd: the cast spot's path's corners off the mob's path
        constexpr uint32 CAST_SPOT_RECHECK_MS = 3 * IN_MILLISECONDS;
        uint32 const now = getMSTime();
        if (_castCheckTarget != target->GetGUID() || !_castCheckMs ||
            getMSTimeDiff(_castCheckMs, now) >= CAST_SPOT_RECHECK_MS ||
            _castCheckSpot.GetExactDist(castSpot) > CAST_SPOT_ARRIVED)
        {
            _castCheckTarget = target->GetGUID();
            _castCheckSpot = castSpot;
            _castCheckMs = now ? now : 1;
            float groundX = castSpot.GetPositionX();
            float groundY = castSpot.GetPositionY();
            float groundZ = castSpot.GetPositionZ();
            bot->UpdateAllowedPositionZ(groundX, groundY, groundZ);
            float const drop = castSpot.GetPositionZ() - groundZ;
            float spotWalk = 0.0f;
            float mobWalk = 0.0f;
            bool offPath = false;
            if (drop <= CAST_SPOT_DROP_DZ)
            {
                PathGenerator toSpot(bot);
                toSpot.CalculatePath(groundX, groundY, groundZ, false);
                PathGenerator toMob(bot);
                toMob.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
                spotWalk = toSpot.getPathLength();
                mobWalk = toMob.getPathLength();
                offPath = toSpot.GetPathType() == PATHFIND_NORMAL && toMob.GetPathType() == PATHFIND_NORMAL &&
                          spotWalk > mobWalk + CAST_SPOT_WALK_SLACK;
            }
            _castCheckBad = drop > CAST_SPOT_DROP_DZ || offPath;
            if (_castCheckBad)
                LOG_INFO("playerbots", "[b34_wc] cast_spot_off_floor: {}: cast spot ({:.1f},{:.1f},{:.1f}) for {} "
                         "({:.1f} yd) dropped: {}; walks the path to the mob instead", bot->GetName(),
                         castSpot.GetPositionX(), castSpot.GetPositionY(), castSpot.GetPositionZ(), target->GetName(),
                         bot->GetExactDist(target),
                         drop > CAST_SPOT_DROP_DZ
                             ? Acore::StringFormat("its ground {:.1f} yd below it (over a drop)", drop)
                             : Acore::StringFormat("its walk {:.1f} yd is longer than the walk to the mob {:.1f} yd",
                                                   spotWalk, mobWalk));
        }
        if (_castCheckBad)
        {
            PullCastSpot::Clear(bot);
            hasCastSpot = false;
        }
    }

    if (hasCastSpot)
        return MoveTo(bot->GetMapId(), castSpot.GetPositionX(), castSpot.GetPositionY(), castSpot.GetPositionZ(),
                      false, false, false, false, MovementPriority::MOVEMENT_COMBAT);

    // RFC 2026-10-24 18:28: the tank walked for Shoot range round a corner alone (33 -> 27 yd), out of the healer's
    // sight, and died under 7 mobs. Where the walk ends (on the line to the mob, at the reach distance) stays in the
    // healer's sight and heal range, else the pull is given up and the planner picks another.
    float const distance = bot->GetExactDist(target);
    if (PlayerbotAI::IsTank(bot) && distance > reachDistance && distance > 0.0f)
    {
        float const share = reachDistance / distance;
        Position const end(target->GetPositionX() + (bot->GetPositionX() - target->GetPositionX()) * share,
                           target->GetPositionY() + (bot->GetPositionY() - target->GetPositionY()) * share,
                           target->GetPositionZ() + (bot->GetPositionZ() - target->GetPositionZ()) * share);
        if (!HealerSeesSpot(bot, end))
        {
            LOG_INFO("playerbots", "[pull exec] {}: aborts: walking for range leaves the healer ({} at {:.1f} yd, "
                     "the walk ends {:.1f} yd from here)", bot->GetName(), target->GetName(), distance,
                     bot->GetExactDist(end));
            // [b37_sv] in range already: the walk was for sight
            NotePullAbort(bot, target, "walking for range leaves the healer",  // [b34_wc]
                          distance <= strategy->GetRange() && !bot->IsWithinLOSInMap(target));
            botAI->DoSpecificAction("pull end", event, true);
            return false;
        }
    }

    // [b38_scholo] The step in for sight is a step, not a walk to the mob: ReachCombatTo shortens its path to the mob
    // only while the shortened end still sees it (ShortenPathUntilDist), so round a door frame the walk ends at the
    // mob's side. scholo-h 20261026-050341-scholo-h-5B-3 pull 28: Risen Aberration in Shoot range (23.5 yd) out of
    // sight, the walk ended 2 yd from it in the Ghoul room (the plan: one mob pulled back, 7 expected; 14 came, three
    // deaths). A tank's walk whose path ends more than PULL_SIGHT_WALK_SLACK inside the planned reach is given up as
    // the healer's walk above (in range, out of sight: the [b37_sv] engage-only cap after three).
    if (PlayerbotAI::IsTank(bot) && !bot->IsWithinLOSInMap(target))
    {
        constexpr float PULL_SIGHT_WALK_SLACK = 5.0f;
        float const combatDistance = bot->GetCombatReach() + target->GetCombatReach();
        PathGenerator probe(bot);
        probe.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
        if (probe.GetPathType() & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT))
        {
            G3D::Vector3 const mob(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            probe.ShortenPathUntilDist(mob, reachDistance + combatDistance);
            if (!probe.GetPath().empty())
            {
                float const endToMob = (probe.GetPath().back() - mob).length();
                if (endToMob + PULL_SIGHT_WALK_SLACK < reachDistance)
                {
                    LOG_INFO("playerbots", "[b38_scholo] pull_sight_walk_abort: {}: {} at {:.1f} yd out of sight: the "
                             "walk for sight ends {:.1f} yd from it (reach {:.1f}), inside its pack: pull ended",
                             bot->GetName(), target->GetName(), bot->GetExactDist(target), endToMob, reachDistance);
                    NotePullAbort(bot, target, "the walk for sight ends at the mob",
                                  bot->GetExactDist(target) <= strategy->GetRange());
                    botAI->DoSpecificAction("pull end", event, true);
                    return false;
                }
            }
        }
    }

    return ReachCombatTo(target, reachDistance);
}

bool ReachPullAction::isUseful()
{
    if (botAI->HasStrategy("stay", botAI->GetState()))
        return false;

    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
        return false;

    PullStrategy* strategy = PullStrategy::Get(botAI);
    Unit* target = strategy ? strategy->GetTarget() : nullptr;
    // [b38_scholo] no walk for range to a mob already in combat: it comes (the pull ends in PullAction::Execute)
    return target && !target->IsInCombat() && !IsWithinPullRange(bot, target, strategy);
}

Unit* ReachPullAction::GetTarget()
{
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy)
        return nullptr;

    return strategy->GetTarget();
}

bool FightSpotBackTrigger::IsActive()
{
    if (!bot->IsInCombat() || !bot->IsAlive() || !PlayerbotAI::IsTank(bot) || !bot->GetGroup())
        return false;

    // [drift_fight_hold] the tank's fight spot lives on its combat ticks
    FightHold::Tick(botAI, bot);

    // A pull under way keeps its own spot (the pull position, reviewed when the pull lands).
    PullStrategy const* strategy = PullStrategy::Get(botAI);
    if (strategy && (strategy->HasTarget() || strategy->IsPullPendingToStart() || strategy->HasPullStarted()))
        return false;

    // A line-of-sight pull of the tank holds its spot; the one a fight on went to round a corner
    // ([drift_caster_corner], FightSpot::Due) is walked here.
    LosPull::Plan plan;
    if (LosPull::Get(bot, plan) && plan.tank == bot->GetGUID())
    {
        Position walking;
        return FightSpot::Walking(bot, walking);
    }

    Position spot;
    return FightSpot::Due(botAI, bot, spot);
}

bool MoveFightSpotBackAction::Execute(Event /*event*/)
{
    Position spot;
    if (!FightSpot::Walking(bot, spot))
        return false;

    FightHold::Move(bot, spot);  // [drift_fight_hold] the hold goes with the fight walked back

    if (bot->GetExactDist(spot) <= 2.0f)
    {
        FightSpot::Arrived(bot);
        return false;
    }

    return MoveTo(bot->GetMapId(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), false, false, false,
                  false, MovementPriority::MOVEMENT_COMBAT, true);
}

bool MoveFightSpotBackAction::isUseful()
{
    Position spot;
    return FightSpot::Walking(bot, spot);
}

namespace
{
    // [drift_fight_hold] The tank's fight spot per tank (FightHold in PullActions.h), behind one lock.
    struct FightHoldEntry
    {
        Position anchor;
        uint32 seenMs = 0;  // the last combat tick (0: none yet)
        uint32 bossLookMs = 0;
        bool boss = false;  // a boss among the attackers: no hold (the encounter's own positions)
        uint32 staleSinceMs = 0;  // [fight_hold_stale] since when no attacker stands within 2 x RADIUS (0: one does)
    };

    constexpr uint32 FIGHT_HOLD_GAP_MS = 4 * IN_MILLISECONDS;  // no combat tick this long: the next is a new fight
    constexpr uint32 FIGHT_HOLD_FRESH_MS = 1500;                // Get: the tank ticked this recently
    constexpr uint32 FIGHT_HOLD_BOSS_LOOK_MS = 1000;
    constexpr float FIGHT_HOLD_PULL_NEAR = 30.0f;  // the pull position this close to the tank starts the fight spot
    constexpr uint32 FIGHT_HOLD_STALE_MS = 5 * IN_MILLISECONDS;  // [fight_hold_stale] no attacker at the spot this long

    std::mutex fightHoldLock;
    std::unordered_map<uint64, FightHoldEntry> fightHolds;

    bool IsFightHoldBoss(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
    }
}

namespace FightHold
{
    void Tick(PlayerbotAI* botAI, Player* tank)
    {
        Map* map = tank->GetMap();
        if (!map || !map->IsNonRaidDungeon() || !tank->IsInCombat() || !tank->IsAlive())
            return;

        uint32 const now = getMSTime();
        uint64 const key = tank->GetGUID().GetRawValue();
        bool newFight = false;
        bool lookBoss = false;
        {
            std::lock_guard<std::mutex> guard(fightHoldLock);
            FightHoldEntry& entry = fightHolds[key];
            newFight = !entry.seenMs || getMSTimeDiff(entry.seenMs, now) > FIGHT_HOLD_GAP_MS;
            entry.seenMs = now ? now : 1;
            lookBoss = newFight || getMSTimeDiff(entry.bossLookMs, now) >= FIGHT_HOLD_BOSS_LOOK_MS;
            if (lookBoss)
                entry.bossLookMs = now ? now : 1;
        }

        Position anchor = tank->GetPosition();
        if (newFight)
        {
            // where the pull put the fight (a line-of-sight spot, a pull back's spot), else where the tank stands
            LosPull::Plan plan;
            PositionInfo pull = botAI->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["pull"];
            if (LosPull::Get(tank, plan) && plan.tank == tank->GetGUID())
                anchor = plan.spot;
            else if (pull.isSet() && pull.mapId == tank->GetMapId() &&
                     tank->GetExactDist(pull.x, pull.y, pull.z) <= FIGHT_HOLD_PULL_NEAR)
                anchor.Relocate(pull.x, pull.y, pull.z);
        }

        bool boss = false;
        // [fight_hold_stale] a fight that never leaves combat (an event's waves) kept the spot of its first pull:
        // when no attacker stands within 2 x RADIUS (30 yd) of the spot, the fight is somewhere else and the spot
        // moves to the tank (bm-n 20261025-153805: the spot stayed 95 yd from Medivh for 14 min). Checked on the
        // boss look's 1 s throttle.
        bool staleSpot = false;
        if (lookBoss)
        {
            Position current;
            if (!newFight)
            {
                std::lock_guard<std::mutex> guard(fightHoldLock);
                current = fightHolds[key].anchor;
            }
            bool anyAttacker = false;
            bool attackerAtSpot = false;
            for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
            {
                Unit* unit = botAI->GetUnit(guid);
                if (!unit || !unit->IsAlive())
                    continue;
                anyAttacker = true;
                if (IsFightHoldBoss(unit))
                    boss = true;
                if (!newFight && unit->GetExactDist2d(&current) <= 2.0f * RADIUS)
                    attackerAtSpot = true;
            }
            staleSpot = !newFight && anyAttacker && !attackerAtSpot;
        }

        std::lock_guard<std::mutex> guard(fightHoldLock);
        FightHoldEntry& entry = fightHolds[key];
        if (newFight)
        {
            entry.anchor = anchor;
            LOG_DEBUG("playerbots", "[fight hold] {}: fight spot ({:.1f},{:.1f},{:.1f}), {:.0f} yd round it",
                      tank->GetName(), anchor.GetPositionX(), anchor.GetPositionY(), anchor.GetPositionZ(), RADIUS);
        }
        // the stale check runs on the 1 s look only
        if (lookBoss && (!staleSpot || newFight))
            entry.staleSinceMs = 0;
        else if (lookBoss && !entry.staleSinceMs)
            entry.staleSinceMs = now ? now : 1;
        else if (lookBoss && getMSTimeDiff(entry.staleSinceMs, now) >= FIGHT_HOLD_STALE_MS)
        {
            entry.staleSinceMs = 0;
            LOG_DEBUG("playerbots",
                      "[fight hold] {}: no attacker within {:.0f} yd of the fight spot ({:.1f},{:.1f},{:.1f}): "
                      "moves to the tank ({:.1f},{:.1f},{:.1f})",
                      tank->GetName(), 2.0f * RADIUS, entry.anchor.GetPositionX(), entry.anchor.GetPositionY(),
                      entry.anchor.GetPositionZ(), anchor.GetPositionX(), anchor.GetPositionY(),
                      anchor.GetPositionZ());
            entry.anchor = anchor;
        }

        if (lookBoss)
            entry.boss = boss;
    }

    bool Get(Player* tank, Position& anchor)
    {
        std::lock_guard<std::mutex> guard(fightHoldLock);
        auto const found = fightHolds.find(tank->GetGUID().GetRawValue());
        if (found == fightHolds.end() || !found->second.seenMs || found->second.boss ||
            getMSTimeDiff(found->second.seenMs, getMSTime()) > FIGHT_HOLD_FRESH_MS)
            return false;

        anchor = found->second.anchor;
        return true;
    }

    void Move(Player* tank, Position const& spot)
    {
        std::lock_guard<std::mutex> guard(fightHoldLock);
        auto const found = fightHolds.find(tank->GetGUID().GetRawValue());
        if (found != fightHolds.end())
            found->second.anchor = spot;
    }
}
