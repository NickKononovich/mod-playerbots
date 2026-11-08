/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AttackAction.h"
#include "AttackerCountValues.h"
#include "AttackersValue.h"
#include "BotMoveEvents.h"
#include "CreatureAI.h"
#include "Event.h"
#include "GenericSpellActions.h"
#include "LastMovementValue.h"
#include "LootObjectStack.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "PvpValues.h"
#include "ReachTargetActions.h"
#include "ReflectAuras.h"
#include "ServerFacade.h"
#include "TankTargetValue.h"
#include "Timer.h"
#include "Unit.h"
#include "WaitForAttackStrategy.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <vector>

bool AttackAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    if (!target->IsInWorld())
        return false;

    return Attack(target);
}

bool AttackMyTargetAction::Execute(Event /*event*/)
{
    Player* master = GetMaster();
    if (!master)
        return false;

    ObjectGuid guid = master->GetTarget();
    if (!guid)
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pull_no_target_error", "You have no target", {}));

        return false;
    }

    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({guid});
    bool result = Attack(botAI->GetUnit(guid));
    if (result)
        context->GetValue<ObjectGuid>("pull target")->Set(guid);

    return result;
}

bool AttackAction::Attack(Unit* target, bool /*with_pet*/ /*true*/)
{
    if (!target)
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_no_target_error", "I have no target", {}));

        return false;
    }

    if (!target->IsInWorld())
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_target_not_in_world_error",
                "%target is no longer in the world.",
                {{"%target", target->GetName()}}));

        return false;
    }

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE ||
        bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_in_flight_error", "I cannot attack in flight", {}));

        return false;
    }

    // Check if bot OR target is in prohibited zone/area (skip for duels)
    if ((target->IsPlayer() || target->IsPet()) &&
        (!bot->duel || bot->duel->Opponent != target) &&
        (sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId()) ||
        sPlayerbotAIConfig.IsPvpProhibited(target->GetZoneId(), target->GetAreaId())))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_pvp_prohibited_error",
                "I cannot attack other players in PvP prohibited areas.",
                {}));

        return false;
    }

    if (bot->IsFriendlyTo(target))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_target_friendly_error",
                "%target is friendly to me.",
                {{"%target", target->GetName()}}));

        return false;
    }

    if (target->isDead())
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_target_dead_error",
                "%target is dead.",
                {{"%target", target->GetName()}}));

        return false;
    }

    if (IsUnavailableCombatTarget(bot, target))
        return false;

    // [rjoin-oos] A mob the group fights round a corner is attacked from here: the combat engine's reach actions
    // ("enemy out of spell range" covers sight, "reach melee") walk the bot into view of it. Without it the non-combat
    // engine never joined a fight it could not see (Sunken Temple 2026-10-25, the hunter 18 s idle, pull 48).
    if (!bot->IsWithinLOSInMap(target) && !AttackersValue::IsFoughtOutOfSight(target, bot))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_target_not_in_sight_error",
                "%target is not in my sight.",
                {{"%target", target->GetName()}}));

        return false;
    }

    // Infantry attacks are not allowed from vehicles drivers.
    // Check is needed to stop some auto-attack situations.
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    Unit* oldTarget = context->GetValue<Unit*>("current target")->Get();
    bool shouldMelee = bot->IsWithinMeleeRange(target) || PlayerbotAI::IsMelee(bot);
    // No swings into a counterattack aura (Retaliation): a damage dealer stops them, the tank keeps its threat.
    if (shouldMelee && ReflectAuras::CountersMelee(target, bot) && !PlayerbotAI::IsTank(bot))
        shouldMelee = false;

    bool sameTarget = oldTarget == target && bot->GetVictim() == target;
    // "Already attacking" only while the target fights: an idle boss that is the current target and the victim
    // already got no attack for minutes (occ-h Drakos, VH Moragg; the engine state alone said combat).
    bool inCombat = botAI->GetState() == BOT_STATE_COMBAT && target->IsInCombat();
    bool sameAttackMode = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == shouldMelee;

    if (sameTarget && inCombat && sameAttackMode)
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_already_attacking_error",
                "I am already attacking %target.",
                {{"%target", target->GetName()}}));

        return false;
    }

    if (!bot->IsValidAttackTarget(target))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_invalid_target_error", "I cannot attack an invalid target.", {}));

        return false;
    }

    if (RefusesNewPull(target))
        return false;

    // [attack-dismount] A mounted player cannot attack (Unit::Attack returns false), and an attack that "succeeded"
    // anyway outranked "check mount state" every tick: Oculus Drakos pull, a Black Morass tank fetching a rift keeper,
    // both stood mounted for minutes. In a dungeon the mount is cancelled as a client does; vehicles (Oculus drakes,
    // encounter seats) are not mounts and stay untouched, nor is anyone dismounted in mid-air.
    if (bot->IsMounted() && !bot->GetVehicle() && !bot->IsFlying() && bot->GetMap()->IsDungeon() &&
        PlayerbotAI::HasGroundSupport(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
    {
        if (bot->isMoving())
            bot->StopMoving();

        WorldPacket emptyPacket;
        bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);

        thread_local std::unordered_map<ObjectGuid, uint32> lastDismountLogMs;
        uint32 const now = getMSTime();
        uint32& lastLog = lastDismountLogMs[bot->GetGUID()];
        if (!lastLog || getMSTimeDiff(lastLog, now) >= 10 * IN_MILLISECONDS)
        {
            lastLog = now;
            LOG_INFO("playerbots", "[attack-dismount] {}: dismounts to attack {} ({:.1f} yd, map {})", bot->GetName(),
                     target->GetName(), bot->GetExactDist(target), bot->GetMapId());
        }
    }

    ObjectGuid guid = target->GetGUID();
    bot->SetSelection(target->GetGUID());

    context->GetValue<Unit*>("old target")->Set(oldTarget);
    context->GetValue<Unit*>("current target")->Set(target);
    context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);

    LastMovement& lastMovement = AI_VALUE(LastMovement&, "last movement");
    bool moveControlled = bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
    if (lastMovement.priority < MovementPriority::MOVEMENT_COMBAT && bot->isMoving() && !moveControlled)
    {
        AI_VALUE(LastMovement&, "last movement").clear();
        bot->GetMotionMaster()->Clear(false);
        bot->StopMoving();
    }

    if (botAI->CanMove() && !bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
        ServerFacade::instance().SetFacingTo(bot, target);

    botAI->ChangeEngine(BOT_STATE_COMBAT);

    // [attack-dismount] A refused attack is not a success: the engine falls through to the other actions (the mount
    // check among them). Already swinging at it counts (Unit::Attack is false for an unchanged victim and mode).
    if (!WaitForAttackStrategy::ShouldWait(botAI) && !bot->Attack(target, shouldMelee) && bot->GetVictim() != target)
        return false;
    /* prevent pet dead immediately in group */
    // if (bot->GetMap()->IsDungeon() && bot->GetGroup() && !target->IsInCombat())
    // {
    //     with_pet = false;
    // }
    // if (Pet* pet = bot->GetPet())
    // {
    //     if (with_pet)
    //     {
    //         pet->SetReactState(REACT_DEFENSIVE);
    //         pet->SetTarget(target->GetGUID());
    //         pet->GetCharmInfo()->SetIsCommandAttack(true);
    //         pet->AI()->AttackStart(target);
    //     }
    //     else
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->GetCharmInfo()->IsReturning();
    //     }
    // }
    return true;
}

// The target choosers a dungeon tank picks mobs with on its own. A master's "attack my target", the defence of a
// member (the mob is on the group already) and encounter actions (their own tactic) are not held.
bool AttackAction::RefusesNewPull(Unit* target)
{
    static constexpr std::string_view choosers[] = {"tank assist",       "melee",      "attack anything",
                                                    "attack least hp target", "attack rti target",
                                                    "dps assist",        "dps aoe",    "aggressive target"};
    if (!PlayerbotAI::IsTank(bot) || !bot->GetMap() || !bot->GetMap()->IsNonRaidDungeon() || !target->ToCreature())
        return false;

    std::string const name = getName();
    if (std::find(std::begin(choosers), std::end(choosers), name) == std::end(choosers))
        return false;

    // The careful pull planner checks the group's readiness itself (pull gate) and has already told the group's
    // crowd control the pull is on: only a fight still going stops its pull here.
    std::string const refusal = TankPullRules::RefusesIdle(botAI, target, name != "aggressive target");
    if (refusal.empty())
        return false;

    TankPullRules::LogSkip(bot, target, refusal + " [" + name + "]");
    return true;
}

bool DefendAttackedGroupMemberAction::isUseful() { return PlayerbotAI::IsTank(bot) && !bot->IsInCombat(); }

// Righteous Defense is cast on the friend; every other opener on the mob. Charge only works out of combat. A gap
// closer (Charge, Feral Charge) is no opener when the mob stands in the aggro reach of an idle pack
// (TankWalkIntoIdlePack): it lands the tank in that pack; the taunts take the mob from here (Death Grip brings it).
bool DefendAttackedGroupMemberAction::CastOpener(Unit* mob, Player* victim)
{
    struct Opener
    {
        char const* spell;
        bool onVictim;
        bool closesGap;
    };
    // [reach_healer_taunt_first] The taunt from where the tank stands first, then a ranged threat spell, a gap
    // closer last (user 2026-10-25: a taunt needs no walking; Charge first had the warrior leave its spot for a mob
    // a Taunt would have brought).
    static Opener const PALADIN[] = {{"righteous defense", true, false},
                                     {"hand of reckoning", false, false},
                                     {"avenger's shield", false, false}};
    static Opener const WARRIOR[] = {{"taunt", false, false}, {"heroic throw", false, false}, {"charge", false, true}};
    static Opener const DRUID[] = {{"growl", false, false},
                                   {"faerie fire (feral)", false, false},
                                   {"feral charge - bear", false, true}};
    static Opener const DEATH_KNIGHT[] = {{"dark command", false, false}, {"death grip", false, false}};

    Opener const* begin = nullptr;
    Opener const* end = nullptr;
    switch (bot->getClass())
    {
        case CLASS_PALADIN:
            begin = std::begin(PALADIN);
            end = std::end(PALADIN);
            break;
        case CLASS_WARRIOR:
            begin = std::begin(WARRIOR);
            end = std::end(WARRIOR);
            break;
        case CLASS_DRUID:
            begin = std::begin(DRUID);
            end = std::end(DRUID);
            break;
        case CLASS_DEATH_KNIGHT:
            begin = std::begin(DEATH_KNIGHT);
            end = std::end(DEATH_KNIGHT);
            break;
        default:
            return false;
    }

    Creature* const idle = TankWalkIntoIdlePack(botAI, bot, mob);
    for (Opener const* opener = begin; opener != end; ++opener)
    {
        if (opener->closesGap && idle)
        {
            LOG_DEBUG("playerbots", "[defend member] {}: no {} to {} ({:.1f} yd): idle {} {:.1f} yd from it",
                      bot->GetName(), opener->spell, mob->GetName(), bot->GetExactDist(mob), idle->GetName(),
                      idle->GetExactDist(mob));
            continue;
        }

        Unit* target = opener->onVictim ? victim : mob;
        if (botAI->CanCastSpell(opener->spell, target) && botAI->CastSpell(opener->spell, target))
        {
            GroupMemberAttackerValue::NoteReaction(botAI, mob, victim, opener->spell, getName());
            LOG_DEBUG("playerbots", "[defend member] {}: {} on {} ({:.1f} yd)", bot->GetName(), opener->spell,
                      target->GetName(), bot->GetExactDist(target));
            return true;
        }
    }

    return false;
}

bool DefendAttackedGroupMemberAction::Execute(Event /*event*/)
{
    Unit* mob = GetTarget();
    if (!mob || !mob->IsAlive() || !mob->IsInWorld())
        return false;

    Unit* victimUnit = mob->GetVictim();
    Player* victim = victimUnit ? victimUnit->ToPlayer() : nullptr;
    if (!victim)
        return false;

    LOG_DEBUG("playerbots", "[defend member] {} (not in combat): {} hits {} ({}, {:.0f}% hp), {:.1f} yd away",
              bot->GetName(), mob->GetName(), victim->GetName(), PlayerbotAI::IsHeal(victim) ? "healer" : "dps",
              victim->GetHealthPct(), bot->GetExactDist(mob));

    // [b44_gerst] Out of combat the tank's own cast or channel is a buff, a ritual's click or a pull spell: it gives
    // way to the member's defence (brd-h 20261026-060218-brd-h-5F-5, 484 s: the tank channelled 58887 while
    // Gerstahn hit the hunter; the pull answered "channeling").
    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);

    bool const opened = CastOpener(mob, victim);
    return Attack(mob) || opened;
}

bool FinishFledSurvivorAction::isUseful()
{
    return PlayerbotAI::IsTank(bot) && !bot->IsInCombat() && bot->IsAlive() &&
           bot->GetHealthPct() >= MIN_HEALTH_PCT && GetTarget();
}

bool FinishFledSurvivorAction::Execute(Event /*event*/)
{
    Unit* mob = GetTarget();
    if (!mob || !mob->IsAlive())
        return false;

    static char const* const PALADIN[] = {"avenger's shield", "hand of reckoning"};
    static char const* const WARRIOR[] = {"heroic throw", "taunt"};
    static char const* const DRUID[] = {"faerie fire (feral)", "growl"};
    static char const* const DEATH_KNIGHT[] = {"death grip", "dark command", "icy touch"};
    std::vector<char const*> openers;
    switch (bot->getClass())
    {
        case CLASS_PALADIN:
            openers.assign(std::begin(PALADIN), std::end(PALADIN));
            break;
        case CLASS_WARRIOR:
            openers.assign(std::begin(WARRIOR), std::end(WARRIOR));
            break;
        case CLASS_DRUID:
            openers.assign(std::begin(DRUID), std::end(DRUID));
            break;
        case CLASS_DEATH_KNIGHT:
            openers.assign(std::begin(DEATH_KNIGHT), std::end(DEATH_KNIGHT));
            break;
        default:
            break;
    }

    float const distance = bot->GetExactDist(mob);
    std::string how;
    for (char const* spell : openers)
    {
        if (TauntReactionAction::Usable(botAI, spell, mob) && botAI->CastSpell(spell, mob))
        {
            how = spell;
            break;
        }
    }

    if (how.empty())
    {
        if (Creature* idle = TankWalkIntoIdlePack(botAI, bot, mob))
        {
            if (_logged != mob->GetGUID())
            {
                _logged = mob->GetGUID();
                LOG_INFO("playerbots",
                         "[fled survivor] {}: {} {} ({:.0f} % health) {:.1f} yd: no ranged pull ready and {} stands "
                         "by it, left to the planner",
                         bot->GetName(), mob->GetName(), mob->GetEntry(), mob->GetHealthPct(), distance,
                         idle->GetName());
            }
            return false;
        }

        if (!Attack(mob))
            return false;

        how = "walks to it";
    }

    LOG_INFO("playerbots", "[fled survivor] {}: {} {} ({:.0f} % health, out of combat) {:.1f} yd: {}", bot->GetName(),
             mob->GetName(), mob->GetEntry(), mob->GetHealthPct(), distance, how);
    BotMoveEvents::Push(bot->GetGUID(), BotMoveEvents::Event{"fled_survivor_pull", {}, {}}
                                            .Str("tank", bot->GetName())
                                            .Str("mob", mob->GetName())
                                            .Num("entry", mob->GetEntry())
                                            .Str("how", how)
                                            .Real("hpPct", mob->GetHealthPct())
                                            .Real("distance", distance));
    return true;
}

bool OpenIdleBossAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE(Unit*, "current target");
    if (!boss)
        boss = AI_VALUE(Unit*, "aggressive target");  // as the trigger reads it
    Creature* const creature = boss ? boss->ToCreature() : nullptr;
    if (!creature || !(creature->IsDungeonBoss() || creature->isWorldBoss()) || !boss->IsAlive() ||
        boss->IsInCombat() || !bot->IsValidAttackTarget(boss) || creature->IsInEvadeMode() ||
        !boss->IsVisible() || !bot->IsWithinLOSInMap(boss))
        return false;

    constexpr uint32 IDLE_BOSS_TRIES = 3;
    constexpr uint32 IDLE_BOSS_LEAVE_MS = 60 * IN_MILLISECONDS;
    uint32 const now = getMSTime();
    if (_boss != boss->GetGUID())
    {
        _boss = boss->GetGUID();
        _tries = 0;
        _leaveUntilMs = 0;
    }

    if (_leaveUntilMs && int32(_leaveUntilMs - now) > 0)
        return false;  // left alone for now

    if (_tries >= IDLE_BOSS_TRIES)
    {
        if (!_leaveUntilMs)
        {
            _leaveUntilMs = now + IDLE_BOSS_LEAVE_MS;
            LOG_INFO("playerbots", "[idle boss] {}: {} did not engage after {} tries, left alone {} s", bot->GetName(),
                     boss->GetName(), _tries, IDLE_BOSS_LEAVE_MS / IN_MILLISECONDS);
            return false;
        }

        _tries = 0;  // the leave is over: tried again
        _leaveUntilMs = 0;
    }

    ++_tries;

    bool const inMelee = bot->IsWithinMeleeRange(boss);
    // not swinging at him yet: the attack starts the swings (and the combat engine)
    if (bot->GetVictim() != boss && Attack(boss))
    {
        LOG_INFO("playerbots", "[idle boss] {}: attacks {} ({:.1f} yd), idle beside the tank", bot->GetName(),
                 boss->GetName(), bot->GetExactDist(boss));
        return true;
    }

    if (inMelee && !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
    {
        bot->Attack(boss, true);
        LOG_INFO("playerbots", "[idle boss] {}: starts melee on {} ({:.1f} yd)", bot->GetName(), boss->GetName(),
                 bot->GetExactDist(boss));
        return true;
    }

    static char const* const RANGED_PULLS[] = {"avenger's shield", "hand of reckoning", "heroic throw", "taunt",
                                               "death grip", "icy touch", "faerie fire (feral)", "growl", "shoot",
                                               "throw"};
    for (char const* spell : RANGED_PULLS)
    {
        if (botAI->CanCastSpell(spell, boss) && botAI->CastSpell(spell, boss))
        {
            LOG_INFO("playerbots", "[idle boss] {}: {} on {} ({:.1f} yd), still idle", bot->GetName(), spell,
                     boss->GetName(), bot->GetExactDist(boss));
            return true;
        }
    }

    // [b32_bm] No ranged pull landed and no swing reaches him: into melee, inside Unit::GetMeleeRange (the reaches +
    // 4/3 yd, at least 5) - a MoveTo distance counts past both combat reaches. bm-h 20261025-223301-bm-h-5H-72: a
    // bear tank stood 140 s at 6.0 yd from an idle Aeonus (melee range 5.83), "did not engage after 3 tries".
    if (!inMelee)
    {
        float const reaches = bot->GetCombatReach() + boss->GetCombatReach();
        float const closeTo = std::max(0.0f, std::min(1.5f, bot->GetMeleeRange(boss) - reaches - 0.5f));
        if (MoveTo(boss, closeTo, MovementPriority::MOVEMENT_COMBAT))
        {
            LOG_INFO("playerbots", "[idle boss] {}: no ranged pull, closes to melee on {} ({:.1f} yd)", bot->GetName(),
                     boss->GetName(), bot->GetExactDist(boss));
            return true;
        }
    }

    return false;
}

bool AttackDuelOpponentAction::isUseful() { return AI_VALUE(Unit*, "duel target"); }

bool AttackDuelOpponentAction::Execute(Event /*event*/) { return Attack(AI_VALUE(Unit*, "duel target")); }

bool MeleeAction::isUseful()
{
    // do not allow if can't attack from vehicle
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    // Do not start autoattack while prowled — let opener spells break stealth intentionally.
    // Future rogue stealth implementation should use this instead:
    // return !(botAI->HasAura("stealth", bot) || botAI->HasAura("prowl", bot));
    if (botAI->HasAura("prowl", bot))
        return false;

    // [noattack] A ranged bot (a healer's "healer dps", a hunter after "reach melee") swings only at a mob already in
    // its melee reach: the action never walks, and out of reach it would only stop the bot's walk (Attack()).
    if (!PlayerbotAI::IsMelee(bot))
    {
        Unit* target = GetTarget();
        return target && bot->IsWithinMeleeRange(target);
    }

    return true;
}
