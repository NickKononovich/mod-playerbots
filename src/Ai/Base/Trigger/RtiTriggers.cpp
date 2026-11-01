/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RtiTriggers.h"
#include "LeaderOrders.h"  // [lead159] LedBot
#include "AttackersValue.h"  // [b44_st]
#include "KillOrder.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "RaidMarkOwnership.h"
#include "RtiTargetValue.h"
#include "RtiValue.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "TargetValue.h"
#include "Timer.h"

#include <algorithm>

namespace
{
    constexpr time_t AUTO_CC_MARK_PULL_WINDOW_SECONDS = 10;  // marks go out at the pull, not mid-fight
    constexpr uint32 AUTO_CC_MARK_UNCONTROLLED_MS = 8000;    // mark with no CC on it this long is dropped
    constexpr float PSYCHIC_SCREAM_CHECK_RADIUS = 10.0f;     // the scream reaches 8 yd
    // [rot_b31_cc] a hold freed this much before its aura should have run out broke (damage), it did not expire
    constexpr uint32 CC_EXPIRY_SLACK_MS = 2000;
    constexpr uint32 CC_QUICK_BREAK_MS = 10000;  // the rule when the aura's duration is unknown
    // [b31_shared] a kill icon with no mob for it is looked at again this often (a mob turning to the tank)
    constexpr uint32 TANK_KILL_MARK_IDLE_RECHECK_MS = 4000;

    // The longest remaining duration of an aura holding the mob (breakable crowd control, or one of `mechanics`),
    // in ms; 0 when none has one. Read once, when the hold is first seen.
    uint32 CrowdControlRemainingMs(Unit* unit, uint64 mechanics)
    {
        uint64 const holding = mechanics | (1ULL << MECHANIC_POLYMORPH) | (1ULL << MECHANIC_SAPPED) |
                               (1ULL << MECHANIC_FREEZE) | (1ULL << MECHANIC_SHACKLE) | (1ULL << MECHANIC_SLEEP) |
                               (1ULL << MECHANIC_BANISH) | (1ULL << MECHANIC_KNOCKOUT) | (1ULL << MECHANIC_CHARM);
        int32 remaining = 0;
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            Aura const* aura = application ? application->GetBase() : nullptr;
            if (aura && (aura->GetSpellInfo()->GetAllEffectsMechanicMask() & holding))
                remaining = std::max(remaining, aura->GetDuration());
        }

        return remaining > 0 ? uint32(remaining) : 0;
    }
}

bool NoRtiTrigger::IsActive()
{
    // Do not auto-react to raid icons while out of combat.
    // Out-of-combat RTI usage (explicit chat commands) is handled by chat triggers,
    // not by this generic trigger.
    if (!bot->IsInCombat())
        return false;

    Unit* target = AI_VALUE(Unit*, "rti target");
    return target == nullptr;
}

// Fires when the RTI CC target should be crowd controlled by this spell.
// Standard path: the target is already in the attackers list and "cc target" matches the RTI
// mark — delegates to HasCcTargetTrigger to confirm no one else is already CCing it.
bool RtiCcTrigger::IsActive()
{
    Unit* rtiCcTarget = AI_VALUE(Unit*, "rti cc target");
    // [rot_b31_cc] a mob whose crowd control broke early is not controlled again for a while (one hash lookup)
    if (!rtiCcTarget || IsCrowdControlBackedOff(rtiCcTarget))
        return false;

    Unit* ccTarget = AI_VALUE2(Unit*, "cc target", getName());
    if (ccTarget && ccTarget == rtiCcTarget)
        return IsCcTargetFree(ccTarget, rtiCcTarget);

    return botAI->CanCastSpell(getName(), rtiCcTarget);
}

bool NeedTankKillMarksTrigger::IsActive()
{
    if (!KillOrder::IsTrashFight(bot) || !PlayerbotAI::IsMainTank(bot) || RaidMarkOwnership::IsOwned(bot))
        return false;
    // [lead159] b176 20261101-163239: "tank kill marks" executed in 10 % of the tank's fight samples and pre-empted
    // Shield of Righteousness 18x, Holy Shield 7x. A led tank's kill order is the leader's (its Attack events): no
    // own raid marks taking the tank's tick
    if (Leader::LedBot(bot))
        return false;

    TankKillMarks const& marks = context->GetValue<TankKillMarks>("tank kill marks")->RefGet();
    if (!KillOrder::IsKillMarkStale(botAI, RtiTargetValue::skullIndex, marks.skull) &&
        !KillOrder::IsKillMarkStale(botAI, RtiTargetValue::crossIndex, marks.cross))
        return false;

    // [b31_shared] A stale icon the last run had no mob for (a lone mob leaves the Cross empty, a dead one keeps
    // its icon) re-fired the action every check: 11-15 % of Prot combat ticks. Re-mark when the fight changes.
    if (!marks.idleMs)
        return true;

    Unit* victim = bot->GetVictim();
    ObjectGuid const victimGuid = victim ? victim->GetGUID() : ObjectGuid::Empty;
    return getMSTimeDiff(marks.idleMs, getMSTime()) >= TANK_KILL_MARK_IDLE_RECHECK_MS ||
           victimGuid != marks.idleVictim ||
           context->GetValue<GuidVector>("attackers")->RefGet().size() != marks.idleAttackers;
}

bool NeedAutoCcMarkTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.AutoCc || !sPlayerbotAIConfig.AutoCcMarkMinPackSize || bot->InBattleground())
        return false;

    // A crowd control icon hides its mob from the dps; the bot that places one casts on it ("cast auto cc").
    Group* group = bot->GetGroup();
    if (!group || !GetCrowdControlRole(bot->getClass()))
        return false;

    // "combat start time" is kept only by the "wait for attack" strategy; the pull grace clock runs for every bot.
    time_t combatStart = AI_VALUE(time_t, "combat start time");
    if (!combatStart)
        combatStart = AI_VALUE(time_t, "pull grace start");

    if (!combatStart || time(nullptr) - combatStart > AUTO_CC_MARK_PULL_WINDOW_SECONDS)
        return false;

    int32 const index = RtiTargetValue::GetRtiIndex(AI_VALUE(std::string, "rti cc"));
    if (index < 0)
        return false;

    if (ObjectGuid const current = group->GetTargetIcon(index))
    {
        Unit* unit = botAI->GetUnit(current);
        if (unit && unit->IsAlive())
            return false;
    }

    return AI_VALUE(bool, "auto cc hard pack");
}

bool AutoCcTargetFreeTrigger::IsActive() { return GetAutoCcTarget(botAI) != nullptr; }

bool StaleAutoCcMarkTrigger::IsActive()
{
    AutoCcMark& mark = context->GetValue<AutoCcMark>("auto cc mark")->RefGet();
    if (!mark.target)
        return false;

    Unit* unit = botAI->GetUnit(mark.target);
    if (!unit || !unit->IsAlive())
        return true;

    uint32 const now = getMSTime();
    if (!mark.fightMs)
        mark.fightMs = now;  // a mark put out before the pull ages from the fight, not from the claim

    // [rot_b31_cc] a hold that broke early: the mark goes, the mob backs off (NoteCrowdControlBreak)
    if (mark.broken)
        return true;

    if (IsHeldByCrowdControl(unit, mark.mechanics))
    {
        if (!mark.heldSinceMs)
        {
            mark.heldSinceMs = now;
            uint32 const remaining = CrowdControlRemainingMs(unit, mark.mechanics);
            mark.holdEndsMs = remaining ? now + remaining : 0;
        }

        mark.lastControlledMs = now;  // a re-cast after the CC expires keeps the mark
        return IsLastHeldToRelease(mark.target);
    }

    // [rot_b31_cc] Freed: an expired hold is cast again, a broken one is not (the group's area damage broke it and
    // would break the next one: build 30 slabs-h, 22-25 Polymorphs a pull on one Cabal Ritualist).
    if (mark.heldSinceMs)
    {
        uint32 const heldMs = getMSTimeDiff(mark.heldSinceMs, mark.lastControlledMs);
        // signed: the hold's aura had this long left when it was last seen (wraps safely, as getMSTime does)
        int32 const leftMs = int32(mark.holdEndsMs - mark.lastControlledMs);
        bool const brokeEarly = mark.holdEndsMs ? leftMs > int32(CC_EXPIRY_SLACK_MS) : heldMs < CC_QUICK_BREAK_MS;
        mark.heldSinceMs = 0;
        mark.holdEndsMs = 0;
        if (brokeEarly)
        {
            mark.broken = true;
            NoteCrowdControlBreak(unit, heldMs);
            return true;
        }
    }

    // A Sap that broke in the fight cannot be cast again: the icon goes at once, so the group fights the mob.
    if (mark.lastControlledMs && !FindCrowdControl(botAI, unit, false))
        return true;

    uint32 const since = mark.lastControlledMs ? mark.lastControlledMs : mark.fightMs;
    return getMSTimeDiff(since, now) > AUTO_CC_MARK_UNCONTROLLED_MS;
}

// Nothing of the pack is left to kill but held mobs: the one with the lowest guid is let go first, and the group
// kills it (KillOrder: its icon goes, Skull follows); the others wait their turn. At most a handful of attackers.
bool StaleAutoCcMarkTrigger::IsLastHeldToRelease(ObjectGuid target)
{
    GuidVector const& attackers = context->GetValue<GuidVector>("attackers")->RefGet();
    Group* group = bot->GetGroup();
    if (!group || attackers.size() > sPlayerbotAIConfig.AutoCcMarkMaxPerPack + 1)
        return attackers.size() <= 1;

    ObjectGuid first;
    for (ObjectGuid const& guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        bool held = IsCrowdControlled(unit);
        for (uint8 icon = 0; icon < TARGETICONCOUNT && !held; ++icon)
        {
            if (icon != RtiTargetValue::skullIndex && icon != RtiTargetValue::crossIndex &&
                group->GetTargetIcon(icon) == guid)
                held = true;
        }

        if (!held)
            return false;  // a free mob is still there to kill

        if (!first || guid < first)
            first = guid;
    }

    return first == target;
}

bool PrePullCcTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.AutoCc || !sPlayerbotAIConfig.AutoCcPrePull || bot->IsInCombat())
        return false;

    PrePullCc::Plan plan;
    if (!PrePullCc::Get(bot, plan) || plan.pulledMs || plan.tank == bot->GetGUID() ||
        std::find(plan.controllers.begin(), plan.controllers.end(), bot->GetGUID()) == plan.controllers.end())
        return false;

    for (PrePullCc::Claim const& claim : plan.claims)
    {
        if (claim.bot == bot->GetGUID())
            return claim.target && !claim.ready && !claim.failed;
    }

    return true;  // not answered yet
}

bool StalePrePullCcMarkTrigger::IsActive()
{
    if (bot->IsInCombat())
        return false;

    AutoCcMark const& mark = context->GetValue<AutoCcMark>("auto cc mark")->RefGet();
    if (!mark.target)
        return false;

    Unit* unit = botAI->GetUnit(mark.target);
    if (!unit || !unit->IsAlive())
        return true;

    // the pack is coming, or the mob is still held: the mark is the fight's now
    if (unit->IsInCombat() || IsCrowdControlled(unit))
        return false;

    PrePullCc::Plan plan;
    if (!PrePullCc::Get(bot, plan))
        return true;

    for (PrePullCc::Claim const& claim : plan.claims)
    {
        if (claim.bot == bot->GetGUID())
            return claim.failed || claim.target != mark.target;
    }

    return std::find(plan.pack.begin(), plan.pack.end(), mark.target) == plan.pack.end();
}

bool EmergencyCcTrigger::IsActive()
{
    bool const rogue = bot->getClass() == CLASS_ROGUE;
    bool const priest = bot->getClass() == CLASS_PRIEST;
    if (!sPlayerbotAIConfig.AutoCc || (!rogue && !priest) || bot->InBattleground())
        return false;

    // Blind: a mob on any healer; Psychic Scream: one beating on the priest itself.
    Unit* mob = AI_VALUE2(Unit*, "healer attacker near", rogue ? "25" : "8");
    Creature* creature = mob ? mob->ToCreature() : nullptr;
    if (!creature || creature->IsDungeonBoss() || creature->isWorldBoss() || IsCrowdControlled(mob) ||
        IsGroupKillTarget(bot, mob))
        return false;

    if (rogue)
        return botAI->CanCastSpell("blind", mob);

    if (mob->GetVictim() != bot || !IsFearSafe(botAI, bot))
        return false;

    // An area fear: never with a held mob in it (it breaks), nor with the tank's own mobs (they would run off it).
    Player* mainTank = bot->GetGroup() ? ObjectAccessor::FindPlayer(PlayerbotAI::GetMainTankGuid(bot->GetGroup()))
                                       : nullptr;
    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->RefGet())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->GetExactDist2d(bot) > PSYCHIC_SCREAM_CHECK_RADIUS)
            continue;

        if (IsCrowdControlled(unit) || (mainTank && unit->GetVictim() == mainTank))
            return false;
    }

    return botAI->CanCastSpell("psychic scream", bot);
}

// [b44_st] within the reach of the control spells (30 yd and the bodies)
bool CharmedGroupMemberTrigger::IsActive()
{
    constexpr float CHARMED_ALLY_RANGE = 30.0f;
    return bot->IsInCombat() && AttackersValue::FindCharmedAlly(bot, CHARMED_ALLY_RANGE) != nullptr;
}
