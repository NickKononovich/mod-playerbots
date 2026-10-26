/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

/*
 * Ported from the CMaNGOS playerbots project (https://github.com/cmangos/playerbots), GPL v2,
 * with modifications for AzerothCore.
 * Original authors:
 *   David Parra Ausina (davidonete/Flekz) <davidparraausina@gmail.com> - original author
 *   Sebastiaan Keek (mostlikely4r) <sebastiaan.keek@gmail.com>
 *   Cyberium <cyberium@users.noreply.github.com>
 */

#include "PullTriggers.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "PullStrategy.h"
#include "EstimatedLifetimeValue.h"
#include "Creature.h"
#include "Group.h"

bool PullStartTrigger::IsActive()
{
    PullStrategy const* strategy = PullStrategy::Get(botAI);
    return strategy && strategy->IsPullPendingToStart();
}

bool ChainPullTrigger::IsActive()
{
    constexpr float CHAIN_PULL_HEALTH_PCT = 30.0f;    // the last mob is this far down
    constexpr float CHAIN_PULL_LIFETIME_SEC = 3.0f;  // or has about this long to live (ExpectedLifetime)

    if (!bot->IsInCombat() || !bot->GetGroup() || !bot->GetMap()->IsDungeon() || !PlayerbotAI::IsTank(bot))
        return false;

    PullStrategy const* strategy = PullStrategy::Get(botAI);
    if (!strategy || strategy->HasTarget() || strategy->IsPullPendingToStart() || strategy->HasPullStarted())
        return false;

    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    if (attackers.size() != 1)
        return false;

    Unit* last = botAI->GetUnit(attackers.front());
    if (!last || !last->IsAlive())
        return false;

    if (Creature* creature = last->ToCreature())
        if (creature->IsDungeonBoss() || creature->isWorldBoss())
            return false;

    // [chain_pull_gate] the lifetime estimate alone opened chains on a mob at 86 % (scholo-n 2026-10-25,
    // 20261025-115805-scholo-n-5A-7: "hand of reckoning on Risen Guard (2 mobs) while Risen Guard is at 86 %"): it
    // counts only once the mob is at least half down.
    constexpr float CHAIN_PULL_LIFETIME_MAX_HEALTH_PCT = 50.0f;
    float const health = last->GetHealthPct();
    return health <= CHAIN_PULL_HEALTH_PCT ||
           (health <= CHAIN_PULL_LIFETIME_MAX_HEALTH_PCT && ExpectedLifetime(botAI, last) <= CHAIN_PULL_LIFETIME_SEC);
}

bool PullEndTrigger::IsActive()
{
    PullStrategy const* strategy = PullStrategy::Get(botAI);

    if (!strategy || !strategy->HasPullStarted())
        return false;

    Unit* target = strategy->GetTarget();
    if (!target || !target->IsInWorld() || !target->IsAlive())
        return true;

    // [b40_peel] A tank in combat whose pull has not landed (the pull target fights nobody) gives the pull up for a
    // mob on a group member: "pull start" (106) and "pull action" (105) outrank every peel step (ACTION_INTERRUPT +
    // 0.1..0.6), "taunt reaction" and "tank assist", and the pull action keeps the pull target as the current target.
    // occ-h 20261026-054314-occ-h-5A-16 (737-745 s): Rtpalah's pull of an Azure Ring Guardian 15 yd over the drop
    // stayed "out of pull range (40.5 yd)" for 9 s, "doing pull start (106.00) ... taunt reaction (not evaluated)",
    // while "[peel diag] ... Centrifuge Construct on Rtdruag (healer) 13.5 yd: picked, current Azure Ring Guardian,
    // taunt ready"; three Constructs killed the healer, then the group. Ended when the mob is on the healer, or when
    // the pull target is out of the pull's reach (range or sight); "pull end" clears that current target, so "tank
    // assist" takes the loose mob. Class-agnostic (every tank's pull runs through here); cached values only, the
    // sight check only in this rare case.
    // [b44_gerst] ... and out of combat: a tank whose pull has not landed while a mob hits a member gives the pull up
    // at once, whatever its reach. PullMultiplier vetoes every pull step then ("defend member target"), so the pull
    // only sat there holding the pull target as the current target until GetMaxPullTime. brd-h
    // 20261026-060218-brd-h-5F-5 (484-494 s): Rtwarab's pull of an Anvilrage Warden 32.9 yd off, out of sight, stayed
    // "pull start (106)" with combat false while High Interrogator Gerstahn killed the hunter's and then the druid's
    // health; "tank assist" came at 494 s. "pull end" frees the tank for "tank defend on attacked group member".
    if (!bot->IsInCombat() && PlayerbotAI::IsTank(bot) && !target->IsInCombat())
    {
        Unit* const mob = AI_VALUE(Unit*, "defend member target");
        if (mob && mob != target)
        {
            Unit* const victim = mob->GetVictim();
            LOG_INFO("playerbots", "[b44_gerst] pull_yields_to_defend: {}: pull of {} ({:.1f} yd) ended out of combat "
                     "for {} on {}", bot->GetName(), target->GetName(), bot->GetExactDist(target), mob->GetName(),
                     victim ? victim->GetName() : std::string("a member"));
            return true;
        }
    }

    if (bot->IsInCombat() && PlayerbotAI::IsTank(bot) && !target->IsInCombat())
    {
        Unit* peelMob = AI_VALUE(Unit*, "group member attacker");
        if (peelMob == target)
            peelMob = nullptr;

        Unit* attackedFriend = AI_VALUE(Unit*, "attacked group member");
        if (peelMob || attackedFriend)
        {
            Unit* const victim = peelMob ? peelMob->GetVictim() : attackedFriend;
            Player* const victimPlayer = victim ? victim->ToPlayer() : nullptr;
            Player* const friendPlayer = attackedFriend ? attackedFriend->ToPlayer() : nullptr;
            bool const onHealer = (victimPlayer && PlayerbotAI::IsHeal(victimPlayer)) ||
                                  (friendPlayer && PlayerbotAI::IsHeal(friendPlayer));
            bool const pullInReach =
                bot->GetExactDist(target) <= strategy->GetRange() && bot->IsWithinLOSInMap(target);
            if (onHealer || !pullInReach)
            {
                LOG_INFO("playerbots", "[b40_peel] pull_yields_to_peel: {}: pull of {} ({:.1f} yd, {}) ended for {} "
                         "on {}{}", bot->GetName(), target->GetName(), bot->GetExactDist(target),
                         pullInReach ? "in reach" : "out of reach", peelMob ? peelMob->GetName() : std::string("a mob"),
                         victim ? victim->GetName() : std::string("a member"), onHealer ? " (healer)" : "");
                return true;
            }
        }
    }

    time_t const secondsSincePullStarted = time(nullptr) - strategy->GetPullStartTime();
    if (secondsSincePullStarted >= PullStrategy::GetMaxPullTime())
        return true;

    // A line-of-sight pull ends when the pack has come round the corner to the tank, not at the first cast of a
    // caster: ending it then would send the tank running out to the casters.
    LosPull::Plan plan;
    if (LosPull::Get(bot, plan) && plan.tank == bot->GetGUID() && plan.target == target->GetGUID())
        return plan.arrived || bot->IsWithinMeleeRange(target);

    // A cast of the target means it answers the pull only once it fights: an idle mob channels its ritual
    // (Blood Furnace Shadowmoon Adepts, wave 21) and ended every pull of it before the cast.
    float distanceToPullTarget = bot->GetDistance(target);
    bool const answering = target->IsInCombat() && target->IsNonMeleeSpellCast(false, false, true);
    if (distanceToPullTarget > ATTACK_DISTANCE && !answering &&
        (!PlayerbotAI::IsRanged(bot) || distanceToPullTarget > botAI->GetRange("spell")))
        return false;

    if (!botAI->HasStrategy("pull back", BOT_STATE_COMBAT))
        return true;

    PositionInfo pullPosition = AI_VALUE(PositionMap&, "position")["pull"];
    if (!pullPosition.isSet() || pullPosition.mapId != bot->GetMapId())
        return true;

    return bot->GetDistance(pullPosition.x, pullPosition.y, pullPosition.z) <= botAI->GetRange("follow");
}

bool ReturnToPullPositionTrigger::IsActive()
{
    PullStrategy const* strategy = PullStrategy::Get(botAI);

    Unit* target = strategy ? strategy->GetTarget() : nullptr;
    if (!strategy || !strategy->HasPullStarted() || !target || !target->IsInCombat() ||
        !botAI->HasStrategy("pull back", BOT_STATE_COMBAT))
        return false;

    PositionInfo pullPosition = AI_VALUE(PositionMap&, "position")["pull"];
    return pullPosition.isSet() && pullPosition.mapId == bot->GetMapId() &&
           bot->GetDistance(pullPosition.x, pullPosition.y, pullPosition.z) > sPlayerbotAIConfig.FollowDistance;
}

bool LosPullGatherTrigger::IsActive()
{
    // The registry answers from its cache; the pack is looked at once per group every few hundred ms.
    LosPull::Plan plan;
    return bot->IsAlive() && LosPull::Get(bot, plan) && plan.tank != bot->GetGUID() &&
           LosPull::Holds(bot, plan) &&  // [b32_engage] a melee dps goes once the pack is in the tank's melee
           bot->GetExactDist2d(plan.gather.GetPositionX(), plan.gather.GetPositionY()) > LosPull::GATHER_STAND;
}
