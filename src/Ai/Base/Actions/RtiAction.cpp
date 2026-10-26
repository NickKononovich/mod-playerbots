/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RtiAction.h"
#include "AggressiveTargetValue.h"
#include "AiFactory.h"
#include "AttackersValue.h"  // [b44_st]
#include "AttackerCountValues.h"
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "Event.h"
#include "GameObject.h"
#include "KillOrder.h"
#include "MobRoles.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "RaidMarkOwnership.h"
#include "RtiTargetValue.h"
#include "RtiValue.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Strategy.h"
#include "TargetValue.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32 AUTO_CC_KEEP_KILLABLE = 2;  // a pack keeps at least this many mobs free for the kill
    constexpr float AUTO_CC_MIN_HEALTH_PCT = 90.0f;
    constexpr float AUTO_CC_HEALER_BONUS = 200.0f;  // beats any distance difference inside a pack
    constexpr float AUTO_CC_CASTER_BONUS = 100.0f;
    constexpr float AUTO_CC_MELEE_BONUS = 50.0f;     // the hardest-hitting melee mob of the candidates, in full
    constexpr float AUTO_CC_DISTANCE_WEIGHT = 0.5f;  // among equals, the mob farther from the tank
    constexpr float AUTO_CC_MAX_DISTANCE = 30.0f;
    constexpr float AUTO_CC_RANGE_MARGIN = 2.0f;       // a caster walks this far inside its spell's range
    constexpr float SAP_REACH = 2.0f;                  // the rogue stops this close to the mob it saps
    constexpr float SAP_CAST_DISTANCE = 4.0f;
    constexpr float TRAP_REACH = 3.0f;                 // a trap this close to the hunter lies at its feet
    constexpr float BLIND_DISTANCE = 8.0f;
    // A pre-pull Sap's walk against the core's stealth detection (WorldObject::CanDetectStealthOf) and proximity
    // aggro (Creature::CanStartAttack).
    constexpr int32 STEALTH_DETECT_BASE = 30;        // detection points of a level 1 mob
    constexpr int32 STEALTH_DETECT_PER_LEVEL = 5;
    constexpr float STEALTH_YARDS_PER_POINT = 0.3f;
    constexpr float STEALTH_ALERT_SCALE = 1.08f;     // the alert range (the mob turns to look): visibility * 1.08 + 1.5
    constexpr float STEALTH_ALERT_BONUS = 1.5f;
    constexpr float SAP_DETECT_MARGIN = 2.0f;        // path corners and the mob's own small steps
    constexpr float SAP_MOVER_MARGIN = 5.0f;         // a mob that walks (wander, waypoints) may come closer meanwhile
    constexpr float SAP_TURN_MARGIN = 1.5f;          // the turn point behind the target lies this far past its reach
    constexpr float SAP_COMBAT_REACH_MARGIN = 0.5f;  // inside its combat reach a mob sees through stealth
    constexpr float SAP_MAX_WALK = 60.0f;
    constexpr float SAP_WALK_SAMPLE = 1.0f;
    constexpr uint32 PRE_PULL_SKIP_LOG_MS = 10 * IN_MILLISECONDS;

    bool IsCrowdControlIcon(int32 index, int32 killIndex)
    {
        return index != RtiTargetValue::skullIndex && index != RtiTargetValue::crossIndex && index != killIndex;
    }

    float MeleeDps(Unit* unit)
    {
        uint32 const attackTime = unit->GetAttackTime(BASE_ATTACK);
        if (!attackTime)
            return 0.0f;

        float const hit =
            (unit->GetFloatValue(UNIT_FIELD_MINDAMAGE) + unit->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        return hit * float(IN_MILLISECONDS) / float(attackTime);
    }

    struct CcCandidate
    {
        Unit* unit = nullptr;
        CrowdControlOption const* option = nullptr;
    };

    // A healer first, then a caster, then the hardest-hitting melee mob; among equals the one farther from `anchor`
    // (the tank, or the mob it pulls), the one the tank would struggle to pick up.
    CcCandidate PickCcTarget(std::vector<CcCandidate> const& candidates, Unit const* anchor)
    {
        float maxMelee = 0.0f;
        for (CcCandidate const& candidate : candidates)
            maxMelee = std::max(maxMelee, MeleeDps(candidate.unit));

        CcCandidate best;
        float bestScore = -1.0f;
        for (CcCandidate const& candidate : candidates)
        {
            Unit* unit = candidate.unit;
            float score = anchor ? std::min(unit->GetDistance(anchor), AUTO_CC_MAX_DISTANCE) * AUTO_CC_DISTANCE_WEIGHT
                                 : 0.0f;
            if (MobRoles::IsHealer(unit))
                score += AUTO_CC_HEALER_BONUS;
            else if (MobRoles::IsCaster(unit))
                score += AUTO_CC_CASTER_BONUS;
            else if (maxMelee > 0.0f)
                score += AUTO_CC_MELEE_BONUS * MeleeDps(unit) / maxMelee;

            if (score > bestScore)
            {
                best = candidate;
                bestScore = score;
            }
        }

        return best;
    }

    // Takes the bot's own crowd control icon back off the mob it marked (someone may have moved it since) and
    // forgets the mark.
    void ClearOwnMark(PlayerbotAI* botAI)
    {
        AiObjectContext* context = botAI->GetAiObjectContext();
        AutoCcMark const mark = context->GetValue<AutoCcMark>("auto cc mark")->Get();
        context->GetValue<AutoCcMark>("auto cc mark")->Set(AutoCcMark());

        Player* bot = botAI->GetBot();
        Group* group = bot->GetGroup();
        int32 const index = RtiTargetValue::GetRtiIndex(context->GetValue<std::string>("rti cc")->Get());
        if (!group || index < 0 || !mark.target || RaidMarkOwnership::IsOwned(bot))
            return;

        if (group->GetTargetIcon(uint8(index)) == mark.target)
            group->SetTargetIcon(uint8(index), bot->GetGUID(), ObjectGuid::Empty);
    }

    // The bot's crowd control icon is free: on no living mob.
    bool IsOwnIconFree(PlayerbotAI* botAI)
    {
        Group* group = botAI->GetBot()->GetGroup();
        AiObjectContext* context = botAI->GetAiObjectContext();
        int32 const index = RtiTargetValue::GetRtiIndex(context->GetValue<std::string>("rti cc")->Get());
        int32 const killIndex = RtiTargetValue::GetRtiIndex(context->GetValue<std::string>("rti")->Get());
        if (!group || index < 0 || !IsCrowdControlIcon(index, killIndex) || RaidMarkOwnership::IsOwned(botAI->GetBot()))
            return false;

        ObjectGuid const current = group->GetTargetIcon(uint8(index));
        Unit* unit = current ? botAI->GetUnit(current) : nullptr;
        return !unit || !unit->IsAlive();
    }

    // Puts the bot's crowd control icon on the mob and remembers it as its mark.
    bool SetOwnMark(PlayerbotAI* botAI, Unit* unit)
    {
        Player* bot = botAI->GetBot();
        Group* group = bot->GetGroup();
        AiObjectContext* context = botAI->GetAiObjectContext();
        int32 const index = RtiTargetValue::GetRtiIndex(context->GetValue<std::string>("rti cc")->Get());
        if (!group || index < 0)
            return false;

        group->SetTargetIcon(uint8(index), bot->GetGUID(), unit->GetGUID());

        AutoCcMark mark;
        mark.target = unit->GetGUID();
        mark.markedAtMs = getMSTime();
        mark.mechanics = GetKnownCrowdControlMechanics(botAI);
        context->GetValue<AutoCcMark>("auto cc mark")->Set(mark);
        return true;
    }

    // An idle mob, other than those in `ignore`, would notice the bot at `spot` (the careful pull's test).
    bool SpotDrawsIdleMob(PlayerbotAI* botAI, Position const& spot, GuidVector const& ignore)
    {
        Player* bot = botAI->GetBot();
        for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get())
        {
            if (std::find(ignore.begin(), ignore.end(), guid) != ignore.end())
                continue;

            if (IdleMobReachesSpot(bot, botAI->GetUnit(guid), spot))
                return true;
        }

        return false;
    }

    // Where the bot stands after walking straight at the mob until `reach` yards (exact distance) from it.
    Position ApproachSpot(Player* bot, Unit* target, float reach)
    {
        float const distance = bot->GetExactDist2d(target);
        if (distance <= reach)
            return bot->GetPosition();

        float const share = reach / distance;
        return Position(target->GetPositionX() + (bot->GetPositionX() - target->GetPositionX()) * share,
                        target->GetPositionY() + (bot->GetPositionY() - target->GetPositionY()) * share,
                        target->GetPositionZ());
    }

    // "[pre pull cc] <bot>: skip (<reason>)", once per bot per PRE_PULL_SKIP_LOG_MS (bots of several maps log it).
    void LogPrePullSkip(Player* bot, std::string const& reason)
    {
        static std::mutex mutex;
        static std::unordered_map<uint64, uint32> last;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(mutex);
            uint32& at = last[bot->GetGUID().GetRawValue()];
            if (at && getMSTimeDiff(at, now) < PRE_PULL_SKIP_LOG_MS)
                return;

            at = now;
        }

        LOG_INFO("playerbots", "[pre pull cc] {}: skip ({})", bot->GetName(), reason);
    }

    // The tank's careful planner refused the pack of `mob` as it stands (a heavy plan, a fresh refused verdict): a Sap
    // may be claimed on it, not cast. The planner counts a claimed Sap as applied (ChooseTargetActions
    // SapClaimScope), so the pack may fit with it; the Sap goes out once the verdict is accepted (TankPullsPack).
    bool TankRefusedPack(PrePullCc::Plan const& plan, ObjectGuid mob)
    {
        bool accepted = false;
        uint32 mobs = 0;
        return plan.heavy && CarefulPlanVerdict::Get(plan.tank, mob, accepted, mobs) && !accepted;
    }

    // The tank's careful planner has chosen the pack of `mob` and the group is ready (CarefulPlanVerdict, accepted),
    // or the plan is a default-mode pull the tank found manageable (no careful verdict, not `heavy`). User rule: a
    // damage dealer never starts a fight before the tank, so a pack the tank refused is not walked into.
    bool TankPullsPack(PrePullCc::Plan const& plan, ObjectGuid mob, std::string& why)
    {
        bool accepted = false;
        uint32 mobs = 0;
        if (CarefulPlanVerdict::Get(plan.tank, mob, accepted, mobs))
        {
            if (!accepted)
                why = "the tank's careful plan refused this pack";

            return accepted;
        }

        if (CarefulPlanVerdict::Get(plan.tank, plan.pullTarget, accepted, mobs))
        {
            why = accepted ? Acore::StringFormat("the tank's plan ({} mobs) does not pull this mob", mobs)
                           : std::string("the tank's careful plan refused this pack");
            return false;
        }

        if (plan.heavy)
        {
            why = "the tank refused this pack as it stands";
            return false;
        }

        return true;
    }

    // The rogue's stealth level once Stealth is on (WorldObject::m_stealth: the Stealth aura's amount plus the stealth
    // level talents, which count without the aura too).
    int32 PlannedStealth(PlayerbotAI* botAI)
    {
        Player* bot = botAI->GetBot();
        int32 const value = bot->m_stealth.GetValue(STEALTH_GENERAL);
        if (bot->HasStealthAura())
            return value;

        uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "stealth")->Get();
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info)
            return value;

        int32 amount = 0;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.ApplyAuraName == SPELL_AURA_MOD_STEALTH && effect.MiscValue == STEALTH_GENERAL)
                amount += effect.CalcValue(bot);
        }

        return value + amount;
    }

    // How close a stealthed bot gets to `mob` before it is noticed: the smaller of the mob's proximity aggro
    // (Creature::CanStartAttack: GetAggroRange + m_CombatDistance) and its stealth alert range
    // (WorldObject::CanDetectStealthOf: (30 + 5 * (mob level - 1) + its detection - stealth) * 0.3 yd + its combat
    // reach, then * 1.08 + 1.5 for the alert, which turns it to look). The front-arc rule is left out: a mob turns.
    float StealthReach(Creature* mob, Player* bot, int32 stealth)
    {
        float const aggro = mob->GetAggroRange(bot) + mob->m_CombatDistance;
        if (mob->HasAuraTypeWithMiscvalue(SPELL_AURA_DETECT_STEALTH, STEALTH_GENERAL))
            return aggro;

        int32 const detection = STEALTH_DETECT_BASE +
                                int32(mob->getLevelForTarget(bot) - 1) * STEALTH_DETECT_PER_LEVEL +
                                mob->m_stealthDetect.GetValue(STEALTH_GENERAL) - stealth;
        float const visibility = float(std::max(detection, 0)) * STEALTH_YARDS_PER_POINT + mob->GetCombatReach();
        return std::min(aggro, visibility * STEALTH_ALERT_SCALE + STEALTH_ALERT_BONUS);
    }

    struct SapDetector
    {
        Creature* mob = nullptr;
        float reach = 0.0f;
    };

    // Where a pre-pull Sap is cast from and the way there: from behind the target (a mob notices stealth in its front
    // half only), round a turn point behind it when the rogue comes from its front.
    struct SapRoute
    {
        Position spot;
        Position turn;
        bool viaTurn = false;
        float closest = 0.0f;  // the least margin to any other mob's reach on the way (diagnostics)
        std::string closestMob;
        std::string why;       // empty: no idle mob notices the rogue on the way nor at the spot
    };

    // Walks the navmesh path from `from` to `to`: no other idle mob within its stealth reach (and seeing the point),
    // the target only behind its back or out of its reach.
    bool SapLegIsSafe(Player* bot, Position const& from, Position const& to, std::vector<SapDetector> const& detectors,
                      Creature* target, float targetReach, SapRoute& route)
    {
        PathGenerator path(bot);
        path.CalculatePath(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ(), to.GetPositionX(),
                           to.GetPositionY(), to.GetPositionZ(), false);
        if (path.GetPathType() != PATHFIND_NORMAL || path.getPathLength() > SAP_MAX_WALK)
        {
            route.why = Acore::StringFormat("no walkable way of at most {:.0f} yd to the Sap spot", SAP_MAX_WALK);
            return false;
        }

        float const walked = path.getPathLength();
        std::vector<SapDetector> nearby;
        for (SapDetector const& detector : detectors)
        {
            if (detector.mob->GetExactDist(from) <= walked + detector.reach)
                nearby.push_back(detector);
        }

        float const eye = bot->GetCollisionHeight();
        Movement::PointsArray const& points = path.GetPath();
        for (size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const segment = points[i] - points[i - 1];
            float const length = segment.length();
            for (float along = 0.0f; along <= length; along += SAP_WALK_SAMPLE)
            {
                G3D::Vector3 const point = points[i - 1] + (length > 0.0f ? segment * (along / length) : segment);
                Position const at(point.x, point.y, point.z);
                for (SapDetector const& detector : nearby)
                {
                    Creature* mob = detector.mob;
                    float const distance = mob->GetExactDist(point.x, point.y, point.z);
                    // Creature::CanStartAttack: no proximity aggro across a height difference
                    if (!mob->CanFly() &&
                        std::fabs(mob->GetPositionZ() - point.z) > CREATURE_Z_ATTACK_RANGE + mob->m_CombatDistance)
                        continue;

                    float const margin = distance - detector.reach;
                    if (route.closestMob.empty() || margin < route.closest)
                    {
                        route.closest = margin;
                        route.closestMob = mob->GetName();
                    }

                    if (margin < 0.0f && mob->IsWithinLOS(point.x, point.y, point.z + eye))
                    {
                        route.why = Acore::StringFormat("{} would notice the rogue: {:.1f} yd on the way, its stealth "
                                                        "reach {:.1f} yd", mob->GetName(), distance, detector.reach);
                        return false;
                    }
                }

                float const toTarget = target->GetExactDist(point.x, point.y, point.z);
                if (toTarget < target->GetCombatReach() + SAP_COMBAT_REACH_MARGIN ||
                    (toTarget < targetReach && target->HasInArc(float(M_PI), &at) &&
                     target->IsWithinLOS(point.x, point.y, point.z + eye)))
                {
                    route.why = Acore::StringFormat("{} would see the rogue in front of it: {:.1f} yd, its stealth "
                                                    "reach {:.1f} yd", target->GetName(), toTarget, targetReach);
                    return false;
                }
            }
        }

        return true;
    }

    // The Sap spot behind `target` and the way there; `route.why` says why not. Every idle hostile mob that would
    // aggro by sight counts (the target's own pack included, a minion of it too), a crowd-controlled one does not.
    SapRoute PlanSapRoute(PlayerbotAI* botAI, Creature* target)
    {
        SapRoute route;
        Player* bot = botAI->GetBot();
        int32 const stealth = PlannedStealth(botAI);
        float const targetReach = StealthReach(target, bot, stealth) + SAP_DETECT_MARGIN;
        std::vector<SapDetector> detectors;
        for (ObjectGuid const& guid :
             botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
        {
            Creature* mob = botAI->GetCreature(guid);
            if (!mob || mob == target || !mob->IsAlive() || mob->IsInCombat() || !bot->IsHostileTo(mob) ||
                mob->IsCivilian() || IsCrowdControlled(mob) ||
                (!mob->HasReactState(REACT_AGGRESSIVE) && !mob->GetCharmerOrOwnerGUID()))
                continue;

            float reach = StealthReach(mob, bot, stealth) + SAP_DETECT_MARGIN;
            if (mob->isMoving() || mob->GetDefaultMovementType() != IDLE_MOTION_TYPE)
                reach += SAP_MOVER_MARGIN + mob->GetWanderDistance();

            detectors.push_back({mob, reach});
        }

        float const stop = std::max(SAP_REACH, target->GetCombatReach() + SAP_COMBAT_REACH_MARGIN);
        float const back = target->GetOrientation() + float(M_PI);
        route.spot = Position(target->GetPositionX() + std::cos(back) * stop,
                              target->GetPositionY() + std::sin(back) * stop, target->GetPositionZ());
        // The half plane behind the target is convex: from inside it the walk in stays behind its back.
        Position const from = bot->GetPosition();
        route.viaTurn = target->HasInArc(float(M_PI), &from);
        if (!route.viaTurn)
        {
            SapLegIsSafe(bot, from, route.spot, detectors, target, targetReach, route);
            return route;
        }

        float const turn = targetReach + SAP_TURN_MARGIN;
        route.turn = Position(target->GetPositionX() + std::cos(back) * turn,
                              target->GetPositionY() + std::sin(back) * turn, target->GetPositionZ());
        if (SapLegIsSafe(bot, from, route.turn, detectors, target, targetReach, route))
            SapLegIsSafe(bot, route.turn, route.spot, detectors, target, targetReach, route);

        return route;
    }

    // A crowd control damage dealer of the group is alive besides this healer.
    bool GroupHasDpsCrowdControl(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                !PlayerbotAI::IsTank(member) && !PlayerbotAI::IsHeal(member) && GetCrowdControlRole(member->getClass()))
                return true;
        }

        return false;
    }

    bool IsFeralDruid(Player* bot)
    {
        return bot->getClass() == CLASS_DRUID && AiFactory::GetPlayerSpecTab(bot) == DRUID_TAB_FERAL;
    }
}

bool RtiAction::Execute(Event event)
{
    std::string text = event.getParam();
    std::string type = "rti";
    if (text.find("cc ") == 0)
    {
        type = "rti cc";
        text = text.substr(3);
    }

    if (text.empty() || text == "?")
    {
        std::ostringstream outRti;
        outRti << "rti"
               << ": ";
        AppendRti(outRti, "rti");
        botAI->TellMaster(outRti);

        std::ostringstream outRtiCc;
        outRtiCc << "rti cc"
                 << ": ";
        AppendRti(outRtiCc, "rti cc");
        botAI->TellMaster(outRtiCc);
        return true;
    }

    context->GetValue<std::string>(type)->Set(text);

    std::ostringstream out;
    out << type << " set to: ";
    AppendRti(out, type);
    botAI->TellMaster(out);
    return true;
}

void RtiAction::AppendRti(std::ostringstream& out, std::string const type)
{
    out << AI_VALUE(std::string, type);

    std::ostringstream n;
    n << type << " target";

    if (Unit* target = AI_VALUE(Unit*, n.str()))
        out << " (" << target->GetName() << ")";
}

bool MarkRtiAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    if (bot->InBattleground())
        return false;

    // An encounter script owns the icons right now; auto-marking would overwrite its Skull/Cross.
    if (RaidMarkOwnership::IsOwned(bot))
        return false;

    Unit* target = nullptr;
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    GuidSet const exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::DpsTarget);
    for (ObjectGuid const guid : attackers)
    {
        if (exclusions.find(guid) != exclusions.end())
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;

        // do not mark players
        if (unit->IsPlayer())
            continue;

        // [rot_b31_cc] nor a mob crowd control holds: the kill icon would take its own icon off and send the dps to
        // break it
        if (IsCrowdControlled(unit))
            continue;

        bool marked = false;
        for (uint8 i = 0; i < 8; i++)
        {
            ObjectGuid iconGUID = group->GetTargetIcon(i);
            if (iconGUID == unit->GetGUID())
            {
                marked = true;
                break;
            }
        }

        if (marked)
            continue;

        if (!target || target->GetHealth() > unit->GetHealth())
            target = unit;
    }

    if (!target)
        return false;

    std::string const rti = AI_VALUE(std::string, "rti");
    uint8 index = RtiTargetValue::GetRtiIndex(rti);
    group->SetTargetIcon(index, bot->GetGUID(), target->GetGUID());
    return true;
}

bool AutoCcMarkAction::isUseful()
{
    if (!sPlayerbotAIConfig.AutoCc || !GetCrowdControlRole(bot->getClass()) || RaidMarkOwnership::IsOwned(bot))
        return false;

    // Crowd control is the damage dealers' job; the tank holds the pack, and the healer keeps everyone up unless
    // the group has no other crowd control.
    if (PlayerbotAI::IsTank(bot) || (PlayerbotAI::IsHeal(bot) && GroupHasDpsCrowdControl(bot)))
        return false;

    // Sap and the like need an unaware target, so they are never placed once the pull is on. A feral druid only
    // hibernates on a Predator's Swiftness proc, too rare to hold a mark for.
    return KnowsCrowdControl(botAI, false) && !IsFeralDruid(bot);
}

bool AutoCcMarkAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group || !IsOwnIconFree(botAI) || !AI_VALUE(bool, "auto cc hard pack"))
        return false;

    int32 const killIndex = RtiTargetValue::GetRtiIndex(AI_VALUE(std::string, "rti"));
    Map* map = bot->GetMap();
    bool const instance = map && map->IsDungeon();
    Player* mainTank = ObjectAccessor::FindPlayer(PlayerbotAI::GetMainTankGuid(group));
    Unit* tankVictim = mainTank ? mainTank->GetVictim() : nullptr;

    uint32 packSize = 0;
    uint32 crowdControlled = 0;
    std::vector<Unit*> order;  // what the group still kills
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsCreature())
            continue;

        Creature* creature = unit->ToCreature();
        // A boss fight belongs to its encounter script, not to trash crowd control.
        if (creature->IsDungeonBoss() || creature->isWorldBoss())
            return false;

        ++packSize;
        bool held = IsCrowdControlled(unit);
        for (uint8 icon = 0; icon < TARGETICONCOUNT && !held; ++icon)
        {
            if (group->GetTargetIcon(icon) == guid && IsCrowdControlIcon(icon, killIndex))
                held = true;
        }

        if (held)
            ++crowdControlled;
        else
            order.push_back(unit);
    }

    if (crowdControlled >= sPlayerbotAIConfig.AutoCcMarkMaxPerPack ||
        crowdControlled + AUTO_CC_KEEP_KILLABLE >= packSize)
        return false;

    // One plan for the kill and the crowd control: the order the tank's kill marks follow, led by the Skull and
    // the Cross already out. Only its tail is held; the group fights the head now.
    KillOrder::SortKillOrder(order, mainTank, tankVictim);
    ObjectGuid const skull = group->GetTargetIcon(RtiTargetValue::skullIndex);
    ObjectGuid const cross = group->GetTargetIcon(RtiTargetValue::crossIndex);
    std::stable_sort(order.begin(), order.end(),
                     [&](Unit* a, Unit* b)
                     {
                         auto const rank = [&](Unit* unit)
                         { return unit->GetGUID() == skull ? 0 : (unit->GetGUID() == cross ? 1 : 2); };
                         return rank(a) < rank(b);
                     });

    std::vector<CcCandidate> candidates;
    for (size_t i = PrePullCc::KILL_FIRST; i < order.size(); ++i)
    {
        Unit* unit = order[i];
        if (IsGroupKillTarget(bot, unit) || unit->GetHealthPct() < AUTO_CC_MIN_HEALTH_PCT)
            continue;

        // [rot_b31_cc] not a mob whose crowd control broke early this pull (back-off)
        if (IsCrowdControlBackedOff(unit))
            continue;

        // 5-man dungeons: a mob deep in the room the group does not fight yet is no mark (the caster walked to it and
        // drew the next packs, Scholomance wave 29); it is a candidate once it comes.
        if (!KillOrder::IsInGroupFight(bot, unit))
            continue;

        if (instance && !unit->ToCreature()->isElite())
            continue;

        CrowdControlOption const* option = FindCrowdControl(botAI, unit, false);
        // [rot_b31_cc] never into the group's own area damage (the tank's and the melee's reach, a ground spell, a
        // damage over time): it breaks at once (replaces the 8 yd from the main tank)
        if (option && (IsUnbreakableCrowdControl(botAI, *option) || !CrowdControlWouldBreak(botAI, unit)))
            candidates.push_back({unit, option});
    }

    CcCandidate const best = PickCcTarget(candidates, mainTank);
    if (!best.unit || !SetOwnMark(botAI, best.unit))
        return false;

    LOG_DEBUG("playerbots", "[auto cc] {} marks {} for {} ({} of {} in the kill order, {} mobs)", bot->GetName(),
              best.unit->GetName(), best.option->spell,
              std::find(order.begin(), order.end(), best.unit) - order.begin() + 1, order.size(), packSize);
    return true;
}

bool CastAutoCcAction::isPossible()
{
    Unit* target = GetAutoCcTarget(botAI);
    return target && FindCrowdControl(botAI, target, false);
}

bool CastAutoCcAction::Execute(Event /*event*/)
{
    Unit* target = GetAutoCcTarget(botAI);
    CrowdControlRole const* role = GetCrowdControlRole(bot->getClass());
    if (!target || !role)
        return false;

    // The group kills it now (it is the last one left, or someone opened on it): nobody holds it any more.
    if (RefuseCrowdControlOnKillTarget(botAI, target, "crowd control"))
    {
        ClearOwnMark(botAI);
        return false;
    }

    // [rot_b31_cc] its crowd control broke early (another member's hold): the group kills it instead
    if (IsCrowdControlBackedOff(target))
    {
        ClearOwnMark(botAI);
        return false;
    }

    char const* breaks = nullptr;
    bool const wouldBreak = CrowdControlWouldBreak(botAI, target, &breaks);
    for (CrowdControlOption const& option : role->options)
    {
        if (!option.spell)
            break;

        if (!CanCrowdControl(botAI, option, target, false))
            continue;

        // [rot_b31_cc] a hold the group's own area damage breaks at once is not cast (the mark ages out unheld,
        // StaleAutoCcMarkTrigger, if the mob stays there); Banish takes no damage, and a trap holds the mob where the
        // hunter laid it, not where it stands now
        if (wouldBreak && !(option.flags & CC_FLAG_TRAP) && !IsUnbreakableCrowdControl(botAI, option))
        {
            LOG_DEBUG("playerbots", "[auto cc] {} holds {} on {}: {}", bot->GetName(), option.spell,
                      target->GetName(), breaks);
            continue;
        }

        if (option.flags & CC_FLAG_TRAP)
        {
            if (CastTrap(target, option))
                return true;

            continue;
        }

        if (option.flags & CC_FLAG_PET)
        {
            if (CastByPet(target, option))
                return true;

            continue;
        }

        uint32 const spellId = AI_VALUE2(uint32, "spell id", option.spell);
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || bot->HasSpellCooldown(spellId))
            continue;

        // Into range and sight, unless the way there shows the bot to an idle mob.
        float const range = std::max(info->GetMaxRange(false, bot) - AUTO_CC_RANGE_MARGIN, 1.0f);
        if (bot->GetDistance(target) > range || !bot->IsWithinLOSInMap(target))
        {
            // never walk into the room after a mob outside the group's fight: it is held once it comes
            if (!KillOrder::IsInGroupFight(bot, target, true) ||
                SpotDrawsIdleMob(botAI, ApproachSpot(bot, target, range), GuidVector()))
                continue;

            if (bot->GetDistance(target) <= range)
                return MoveToLOS(target, true);

            return MoveTo(target, range - 1.0f);
        }

        if (bot->isMoving() && info->CalcCastTime(bot))
        {
            bot->StopMoving();
            return true;
        }

        if (botAI->CanCastSpell(spellId, target, true) && botAI->CastSpell(option.spell, target))
            return true;
    }

    return false;
}

// A Freezing Trap at the hunter's feet, then Distracting Shot so the mob walks onto it.
bool CastAutoCcAction::CastTrap(Unit* target, CrowdControlOption const& option)
{
    uint32 const trapId = AI_VALUE2(uint32, "spell id", option.spell);
    if (!trapId)
        return false;

    if (GameObject* trap = bot->GetGameObject(trapId))
    {
        // the hunter has moved off its trap, or the mob is on its way onto it already
        if (trap->GetExactDist2d(bot) > TRAP_REACH || target->GetVictim() == bot)
            return false;

        uint32 const shotId = AI_VALUE2(uint32, "spell id", "distracting shot");
        SpellInfo const* shot = shotId ? sSpellMgr->GetSpellInfo(shotId) : nullptr;
        if (!shot || bot->HasSpellCooldown(shotId) || bot->GetDistance(target) > shot->GetMaxRange(false, bot) ||
            !bot->IsWithinLOSInMap(target) || !botAI->CanCastSpell(shotId, target, true))
            return false;

        return botAI->CastSpell("distracting shot", target);
    }

    if (bot->HasSpellCooldown(trapId) || !botAI->CanCastSpell(trapId, bot, true))
        return false;

    return botAI->CastSpell(option.spell, bot);
}

// The pet casts it at the mob, from where it stands (Seduction).
bool CastAutoCcAction::CastByPet(Unit* target, CrowdControlOption const& option)
{
    Pet* pet = bot->GetPet();
    uint32 const spellId = AI_VALUE2(uint32, "spell id", option.spell);
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!pet || !pet->IsAlive() || !info || !pet->HasSpell(spellId) || pet->HasSpellCooldown(spellId) ||
        pet->HasUnitState(UNIT_STATE_CASTING))
        return false;

    if (pet->GetDistance(target) > info->GetMaxRange(false, pet) || !pet->IsWithinLOSInMap(target))
        return false;

    return pet->CastSpell(target, spellId, false) == SPELL_CAST_OK;
}

bool PrePullCcAction::Execute(Event /*event*/)
{
    if (bot->IsInCombat())
        return false;

    PrePullCc::Plan plan;
    if (!PrePullCc::Get(bot, plan) || plan.pulledMs || plan.tank == bot->GetGUID() ||
        std::find(plan.controllers.begin(), plan.controllers.end(), bot->GetGUID()) == plan.controllers.end())
        return false;

    for (PrePullCc::Claim const& claim : plan.claims)
    {
        if (claim.bot != bot->GetGUID())
            continue;

        if (!claim.target || claim.ready || claim.failed)
            return false;

        return Place(plan, claim);
    }

    return ClaimTarget(plan);
}

// Picks the mob of the tail of the pack's kill order this bot holds best, claims and marks it; or declines.
bool PrePullCcAction::ClaimTarget(PrePullCc::Plan const& plan)
{
    size_t const tail = plan.pack.size() > PrePullCc::KILL_FIRST ? plan.pack.size() - PrePullCc::KILL_FIRST : 0;
    uint32 const maxClaims = std::min<uint32>(sPlayerbotAIConfig.AutoCcMarkMaxPerPack, uint32(tail));
    Map* map = bot->GetMap();
    bool const instance = map && map->IsDungeon();
    std::vector<CcCandidate> candidates;
    std::vector<Creature*> pool;  // [rot_b31_cc] every claimable mob of the tail, whoever can hold it
    std::string skipWhy;  // the last Sap candidate turned down, and why
    if (maxClaims && !IsFeralDruid(bot) && IsOwnIconFree(botAI))
    {
        for (size_t i = PrePullCc::KILL_FIRST; i < plan.pack.size(); ++i)
        {
            ObjectGuid const guid = plan.pack[i];
            bool taken = false;
            for (PrePullCc::Claim const& claim : plan.claims)
                taken = taken || (claim.target == guid && !claim.failed);

            Unit* unit = taken ? nullptr : botAI->GetUnit(guid);
            Creature* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->IsInCombat() || IsCrowdControlled(creature) ||
                (instance && !creature->isElite()) || IsGroupKillTarget(bot, creature))
                continue;

            pool.push_back(creature);
            CrowdControlOption const* option = FindCrowdControl(botAI, creature, true);
            if (!option)
                continue;

            // A Sap is cast from next to the mob, before the tank pulls: only for a pack the tank is about to pull,
            // on a way none of the pack's other mobs (nor any other) notices the stealthed rogue.
            if (option->flags & CC_FLAG_PRE_PULL)
            {
                std::string why;
                if (!TankPullsPack(plan, guid, why) && !TankRefusedPack(plan, guid))
                {
                    skipWhy = why;
                    continue;
                }

                SapRoute const route = PlanSapRoute(botAI, creature);
                if (!route.why.empty() || SpotDrawsIdleMob(botAI, route.spot, plan.pack))
                {
                    skipWhy = Acore::StringFormat(
                        "Sap on {}: {}", creature->GetName(),
                        route.why.empty() ? std::string("an idle mob of another pack reaches the spot") : route.why);
                    continue;
                }
            }

            candidates.push_back({creature, option});
        }
    }

    if (candidates.empty() && !skipWhy.empty())
        LogPrePullSkip(bot, skipWhy);

    // [rot_b31_cc] Creature type and immunities decide who holds what (Shackle: undead only, Banish: demons and
    // elementals, Hibernate: beasts and dragonkin, Hex: humanoids and beasts; the spells' own creature type masks,
    // CanCrowdControl): a mob that a controller still to answer can hold, and the only one of the tail it can, is
    // left to it while this bot has another. At most a few controllers times a few mobs.
    if (candidates.size() > 1)
    {
        GuidSet reserved;
        for (ObjectGuid const& guid : plan.controllers)
        {
            if (guid == bot->GetGUID())
                continue;

            bool answered = false;
            for (PrePullCc::Claim const& claim : plan.claims)
                answered = answered || claim.bot == guid;

            Player* member = answered ? nullptr : ObjectAccessor::FindPlayer(guid);
            PlayerbotAI* memberAI = member && member->IsAlive() && member->GetMap() == bot->GetMap()
                                        ? GET_PLAYERBOT_AI(member)
                                        : nullptr;
            if (!memberAI)
                continue;

            Creature* only = nullptr;
            uint32 holdable = 0;
            for (Creature* mob : pool)
            {
                if (FindCrowdControl(memberAI, mob, true))
                {
                    only = mob;
                    ++holdable;
                }
            }

            if (holdable == 1)
                reserved.insert(only->GetGUID());
        }

        std::vector<CcCandidate> unreserved;
        for (CcCandidate const& candidate : candidates)
        {
            if (reserved.find(candidate.unit->GetGUID()) == reserved.end())
                unreserved.push_back(candidate);
        }

        if (!unreserved.empty())
            candidates.swap(unreserved);
    }

    CcCandidate const best = PickCcTarget(candidates, botAI->GetUnit(plan.pullTarget));
    bool const placed = best.option && !(best.option->flags & (CC_FLAG_PRE_PULL | CC_FLAG_TRAP));
    if (!best.unit || !PrePullCc::TryClaim(bot, plan.id, best.unit->GetGUID(), maxClaims, placed))
    {
        // nothing to hold, or the mob was taken meanwhile: one answer per plan, so the tank does not wait
        PrePullCc::TryClaim(bot, plan.id, ObjectGuid::Empty, maxClaims, true);
        return true;
    }

    if (!SetOwnMark(botAI, best.unit))
    {
        PrePullCc::SetClaimState(bot, plan.id, false, true);
        return true;
    }

    LOG_DEBUG("playerbots", "[pre-pull cc] {} claims {} for {} ({})", bot->GetName(), best.unit->GetName(),
              best.option->spell, placed ? "cast on the pull" : "placed before the pull");
    return true;
}

// Saps the claimed mob or lays the trap, before the tank pulls.
bool PrePullCcAction::Place(PrePullCc::Plan const& plan, PrePullCc::Claim const& claim)
{
    Unit* target = botAI->GetUnit(claim.target);
    uint32 const now = getMSTime();
    if (!target || !target->IsAlive() || target->IsInCombat() ||
        getMSTimeDiff(claim.claimedMs, now) >= PrePullCc::MAX_WAIT_MS)
    {
        PrePullCc::SetClaimState(bot, plan.id, false, true);
        return true;
    }

    // the fight starts later than the mark went out: its stale clock starts with the fight
    AutoCcMark& mark = context->GetValue<AutoCcMark>("auto cc mark")->RefGet();
    if (mark.target == claim.target)
        mark.markedAtMs = now;

    if (IsCrowdControlled(target))
    {
        PrePullCc::SetClaimState(bot, plan.id, true, false);
        return true;
    }

    CrowdControlOption const* option = FindCrowdControl(botAI, target, true);
    if (!option || !(option->flags & (CC_FLAG_PRE_PULL | CC_FLAG_TRAP)))
    {
        PrePullCc::SetClaimState(bot, plan.id, option != nullptr, option == nullptr);
        return true;
    }

    if (option->flags & CC_FLAG_TRAP)
    {
        uint32 const trapId = AI_VALUE2(uint32, "spell id", option->spell);
        if (trapId && bot->GetGameObject(trapId))
        {
            PrePullCc::SetClaimState(bot, plan.id, true, false);  // Distracting Shot follows on the pull
            return true;
        }

        if (!trapId || bot->HasSpellCooldown(trapId))
        {
            PrePullCc::SetClaimState(bot, plan.id, false, true);
            return true;
        }

        return botAI->CastSpell(option->spell, bot);
    }

    // Sap: only once the tank is about to pull this pack, from stealth, from behind the mob, on a way no idle mob
    // notices (the pack's own included). Checked again on every step: a mob turns, a patrol comes.
    std::string why;
    Creature* creature = target->ToCreature();
    if (!TankPullsPack(plan, claim.target, why))
    {
        // claimed on a refused pack: waits where it is for the tank's verdict (at most MAX_WAIT_MS, above)
        if (TankRefusedPack(plan, claim.target))
            return false;

        LogPrePullSkip(bot, why);
        PrePullCc::SetClaimState(bot, plan.id, false, true);
        return true;
    }

    if (!bot->HasStealthAura())
        return botAI->CastSpell("stealth", bot);

    if (bot->GetDistance(target) > SAP_CAST_DISTANCE)
    {
        SapRoute const route = creature ? PlanSapRoute(botAI, creature) : SapRoute();
        if (!creature || !route.why.empty() || SpotDrawsIdleMob(botAI, route.spot, plan.pack))
        {
            LogPrePullSkip(bot, Acore::StringFormat(
                                    "Sap on {}: {}", target->GetName(),
                                    !route.why.empty() ? route.why
                                                       : std::string("an idle mob of another pack reaches the Sap "
                                                                     "spot")));
            PrePullCc::SetClaimState(bot, plan.id, false, true);
            return true;
        }

        Position const& next = route.viaTurn ? route.turn : route.spot;
        LOG_DEBUG("playerbots", "[pre pull cc] {}: walks to sap {} ({}), closest other mob {} {:.1f} yd outside its "
                  "reach", bot->GetName(), target->GetName(), route.viaTurn ? "round its back" : "from behind",
                  route.closestMob.empty() ? std::string("none") : route.closestMob, route.closest);
        return MoveTo(bot->GetMapId(), next.GetPositionX(), next.GetPositionY(), next.GetPositionZ());
    }

    return botAI->CastSpell(option->spell, target);
}

bool EmergencyCcAction::Execute(Event /*event*/)
{
    bool const rogue = bot->getClass() == CLASS_ROGUE;
    if (!rogue && bot->getClass() != CLASS_PRIEST)
        return false;

    std::string const spell = rogue ? "blind" : "psychic scream";
    Unit* mob = AI_VALUE2(Unit*, "healer attacker near", rogue ? "25" : "8");
    if (!mob || RefuseCrowdControlOnKillTarget(botAI, mob, spell))
        return false;

    if (!rogue)
        return botAI->CastSpell(spell, bot);

    if (bot->GetDistance(mob) > BLIND_DISTANCE)
        return MoveTo(mob, BLIND_DISTANCE - 2.0f);

    return botAI->CastSpell(spell, mob);
}

bool TankKillMarksAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group || !KillOrder::IsTrashFight(bot) || RaidMarkOwnership::IsOwned(bot))
        return false;

    TankKillMarks& marks = context->GetValue<TankKillMarks>("tank kill marks")->RefGet();
    int8 const icons[] = {RtiTargetValue::skullIndex, RtiTargetValue::crossIndex};
    ObjectGuid* owned[] = {&marks.skull, &marks.cross};

    // [b30_mech_pos] a low-health runner (fled for help) takes the Skull from the tank's own mark: the melee follow
    // the Skull, and a runner left alone reaches its helpers and is healed (zf-h 20261025-184229: 20 Blood Drinker
    // flights at 10-15 %, flee_stop slowed them, nobody finished them, 0 kills in 287 s).
    constexpr float RUNNER_FINISH_PCT = 30.0f;
    constexpr float RUNNER_FINISH_RANGE = 35.0f;
    Unit* runner = nullptr;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsAlive() && unit->GetHealthPct() <= RUNNER_FINISH_PCT && FledMobs::IsRunning(unit) &&
            !IsCrowdControlled(unit) && !HasCrowdControlIcon(bot, unit) &&  // [rot_b31_cc] never a held mob
            bot->IsWithinDistInMap(unit, RUNNER_FINISH_RANGE) &&
            (!runner || unit->GetHealth() < runner->GetHealth()))
            runner = unit;
    }
    if (runner && group->GetTargetIcon(RtiTargetValue::skullIndex) != runner->GetGUID() &&
        (marks.skull.IsEmpty() || group->GetTargetIcon(RtiTargetValue::skullIndex) == marks.skull))
    {
        group->SetTargetIcon(RtiTargetValue::skullIndex, bot->GetGUID(), runner->GetGUID());
        marks.skull = runner->GetGUID();
        return true;
    }

    bool stale[2];
    GuidSet keep;
    for (uint8 i = 0; i < 2; ++i)
    {
        stale[i] = KillOrder::IsKillMarkStale(botAI, icons[i], *owned[i]);
        if (!stale[i])
            keep.insert(group->GetTargetIcon(icons[i]));
    }

    std::vector<Unit*> const pack = KillOrder::RankTankPack(botAI, keep);
    size_t next = 0;
    bool changed = false;
    bool unfilled = false;  // [b31_shared]
    for (uint8 i = 0; i < 2; ++i)
    {
        if (!stale[i])
            continue;

        if (next >= pack.size())
            unfilled = true;

        if (next < pack.size())
        {
            ObjectGuid const guid = pack[next++]->GetGUID();
            group->SetTargetIcon(icons[i], bot->GetGUID(), guid);
            *owned[i] = guid;
            changed = true;
            continue;
        }

        // nothing left to mark: take our own icon off a mob that left the fight, so nobody opens on it later
        ObjectGuid const current = group->GetTargetIcon(icons[i]);
        if (current && current == *owned[i])
        {
            group->SetTargetIcon(icons[i], bot->GetGUID(), ObjectGuid::Empty);
            changed = true;
        }

        *owned[i] = ObjectGuid::Empty;
    }

    // [b31_shared] remember the fight the empty icon was judged in, so the trigger waits for it to change
    marks.idleMs = 0;
    if (unfilled)
    {
        Unit* victim = bot->GetVictim();
        marks.idleMs = std::max<uint32>(getMSTime(), 1);
        marks.idleAttackers = context->GetValue<GuidVector>("attackers")->RefGet().size();
        marks.idleVictim = victim ? victim->GetGUID() : ObjectGuid::Empty;
    }

    return changed;
}

bool ClearAutoCcMarkAction::Execute(Event /*event*/)
{
    ClearOwnMark(botAI);
    return true;
}

// [b44_st] st-h 20261026-075135-st-h-5F-7: Jammal'an's Hex charmed the hunter, whose charm AI killed the healer.
namespace
{
    constexpr float CHARMED_ALLY_RANGE = 30.0f;
    constexpr uint32 CHARMED_ALLY_CLAIM_MS = 3 * IN_MILLISECONDS;  // one bot's cast in flight per member

    struct CharmedAllyOption
    {
        char const* spell;
        bool fear;
    };

    // Control that deals no damage and works on a player; the first one the bot can cast is used.
    std::vector<CharmedAllyOption> CharmedAllyOptions(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_MAGE:
                return {{"polymorph", false}};
            case CLASS_SHAMAN:
                return {{"hex", false}};
            case CLASS_PALADIN:
                return {{"repentance", false}, {"hammer of justice", false}};
            case CLASS_DRUID:
                return {{"cyclone", false}};
            case CLASS_ROGUE:
                return {{"blind", false}};
            case CLASS_HUNTER:
                return {{"wyvern sting", false}};
            case CLASS_WARLOCK:
                return {{"fear", true}};
            default:
                return {};
        }
    }

    std::mutex charmedAllyClaimLock;
    std::unordered_map<uint64, std::pair<uint64, uint32>> charmedAllyClaims;  // member -> (bot, since)
}

bool ControlCharmedMemberAction::isUseful()
{
    return !CharmedAllyOptions(bot->getClass()).empty() &&
           AttackersValue::FindCharmedAlly(bot, CHARMED_ALLY_RANGE) != nullptr;
}

bool ControlCharmedMemberAction::Execute(Event /*event*/)
{
    Player* member = AttackersValue::FindCharmedAlly(bot, CHARMED_ALLY_RANGE);
    if (!member)
        return false;

    uint32 const now = getMSTime();
    uint64 const memberKey = member->GetGUID().GetRawValue();
    uint64 const botKey = bot->GetGUID().GetRawValue();
    {
        std::lock_guard<std::mutex> guard(charmedAllyClaimLock);
        auto const itr = charmedAllyClaims.find(memberKey);
        if (itr != charmedAllyClaims.end() && itr->second.first != botKey &&
            getMSTimeDiff(itr->second.second, now) < CHARMED_ALLY_CLAIM_MS)
            return false;  // another bot's control is on its way
    }

    for (CharmedAllyOption const& option : CharmedAllyOptions(bot->getClass()))
    {
        if (option.fear && !IsFearSafe(botAI, member))
            continue;
        if (!botAI->CanCastSpell(option.spell, member) || !botAI->CastSpell(option.spell, member))
            continue;

        {
            std::lock_guard<std::mutex> guard(charmedAllyClaimLock);
            if (charmedAllyClaims.size() > 256)
                charmedAllyClaims.clear();
            charmedAllyClaims[memberKey] = {botKey, now};
        }
        Unit* charmer = member->GetCharmer();
        LOG_INFO("playerbots", "[b44_st] [charmed ally] {} casts {} on {} ({:.1f} yd), charmed by {}", bot->GetName(),
                 option.spell, member->GetName(), bot->GetExactDist(member), charmer ? charmer->GetName() : "?");
        return true;
    }
    return false;
}
