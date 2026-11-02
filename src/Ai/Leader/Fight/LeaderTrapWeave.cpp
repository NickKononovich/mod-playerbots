/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderTrapWeave.h"

#include <algorithm>
#include <cmath>
#include <list>

#include "AiObjectContext.h"
#include "CellImpl.h"
#include "Creature.h"
#include "EstimatedLifetimeValue.h"  // GroupDamage
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridTerrainData.h"  // INVALID_HEIGHT
#include "LeaderOrders.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TargetValue.h"  // IsCrowdControlled
#include "Timer.h"

namespace Leader::TrapWeave
{
namespace
{
// The trap's game data: the trap object its spell summons (GAMEOBJECT_TYPE_TRAP: trigger diameter, arming
// delay in combat, the spell it casts) and that spell's damage (its direct hit, its periodic area aura)
struct TrapData
{
    float triggerRadius = 0.0f;
    float armSec = 0.0f;
    float gcdSec = 0.0f;
    SpellInfo const* damage = nullptr;
    float radius = 0.0f;
    int8 directEffect = -1;
    int8 periodicEffect = -1;
    uint32 ticks = 0;
    float tickSec = 0.0f;
};

float AverageValue(SpellEffectInfo const& effect)
{
    return effect.DieSides > 1 ? effect.BasePoints + (1.0f + effect.DieSides) / 2.0f
                               : float(effect.BasePoints + effect.DieSides);
}

bool ReadTrap(Player* hunter, uint32 trapSpell, TrapData& data)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(trapSpell);
    if (!info)
        return false;
    data.gcdSec = info->StartRecoveryTime / 1000.0f;
    GameObjectTemplate const* trap = nullptr;
    for (SpellEffectInfo const& effect : info->Effects)
        if (effect.Effect >= SPELL_EFFECT_SUMMON_OBJECT_SLOT1 && effect.Effect <= SPELL_EFFECT_SUMMON_OBJECT_SLOT4)
            trap = sObjectMgr->GetGameObjectTemplate(effect.MiscValue);
    if (!trap || trap->type != GAMEOBJECT_TYPE_TRAP || !trap->trap.diameter)
        return false;
    data.triggerRadius = trap->trap.diameter * 0.5f;
    data.armSec = float(trap->trap.startDelay);
    data.damage = sSpellMgr->GetSpellInfo(trap->trap.spellId);
    if (!data.damage)
        return false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = data.damage->Effects[i];
        if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
            data.directEffect = int8(i);
        else if (effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA &&
                 effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE && effect.Amplitude > 0)
        {
            data.periodicEffect = int8(i);
            data.tickSec = effect.Amplitude / 1000.0f;
            data.ticks = uint32(std::max(data.damage->GetDuration(), 0) / effect.Amplitude);
        }
        if (effect.Effect)
            data.radius = std::max(data.radius, effect.CalcRadius(hunter));
    }
    return data.radius > 0.0f && (data.directEffect >= 0 || data.periodicEffect >= 0);
}

// the trap's damage on one mob (the hunter's and the mob's bonuses, the game's own math) given the seconds
// the mob lives once the trap goes off
float DamageOn(Player* hunter, Unit* mob, TrapData const& data, float lifeSec)
{
    if (lifeSec <= 0.0f)
        return 0.0f;
    float total = 0.0f;
    if (data.directEffect >= 0)
    {
        uint32 hit = uint32(AverageValue(data.damage->Effects[data.directEffect]));
        hit = hunter->SpellDamageBonusDone(mob, data.damage, hit, SPELL_DIRECT_DAMAGE, data.directEffect);
        total += mob->SpellDamageBonusTaken(hunter, data.damage, hit, SPELL_DIRECT_DAMAGE);
    }
    if (data.periodicEffect >= 0)
    {
        uint32 tick = uint32(AverageValue(data.damage->Effects[data.periodicEffect]));
        tick = hunter->SpellDamageBonusDone(mob, data.damage, tick, DOT, data.periodicEffect);
        tick = mob->SpellDamageBonusTaken(hunter, data.damage, tick, DOT);
        uint32 const ticks = std::min(data.ticks, uint32(lifeSec / data.tickSec));
        total += float(tick) * ticks;
    }
    return total;
}

// the group's fight mobs (the members' and the tank's attackers)
std::vector<Unit*> FightMobs(Player* tank, std::vector<Player*> const& seats)
{
    std::vector<Unit*> mobs;
    auto const add = [&](Player* bot)
    {
        for (Unit* attacker : bot->getAttackers())
            if (attacker && attacker->IsAlive() && attacker->GetMap() == tank->GetMap() &&
                std::find(mobs.begin(), mobs.end(), attacker) == mobs.end())
                mobs.push_back(attacker);
    };
    add(tank);
    for (Player* bot : seats)
        add(bot);
    return mobs;
}

uint32 TrapSpell(Player* hunter)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(hunter);
    return botAI ? botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "explosive trap")->Get() : 0;
}

void End(LeaderGroup& group, Player* hunter, char const* why)
{
    auto& plans = group.State().trapWeave;
    auto const found = plans.find(hunter->GetGUID());
    if (found == plans.end())
        return;
    Decisions().Write(group.Run(), "leader", "trap_weave_end", hunter->GetGUID(), hunter->GetName(), true,
                      JsonObject()
                          .Str("member", hunter->GetName())
                          .Str("why", why)
                          .Real("sec", getMSTimeDiff(found->second.orderedMs, getMSTime()) / 1000.0f));
    plans.erase(found);
}

// the trap spot: the trap's trigger radius off the target, toward the hunter (the shortest walk)
Position TrapSpot(Player* hunter, Unit* target, float triggerRadius)
{
    float const dx = hunter->GetPositionX() - target->GetPositionX();
    float const dy = hunter->GetPositionY() - target->GetPositionY();
    float const apart = std::hypot(dx, dy);
    if (apart <= triggerRadius)
        return hunter->GetPosition();
    float const x = target->GetPositionX() + dx / apart * triggerRadius;
    float const y = target->GetPositionY() + dy / apart * triggerRadius;
    float const z = hunter->GetMap()->GetHeight(hunter->GetPhaseMask(), x, y, target->GetPositionZ() + 2.0f);
    return Position(x, y, z > INVALID_HEIGHT ? z : target->GetPositionZ());
}

// The computed choice: the spot and its verdict for this hunter and target; false: no trap now (why)
bool Decide(Player* hunter, Player* tank, Unit* target, std::vector<Player*> const& seats, uint32 trapSpell,
            Position& spot, JsonObject& facts, std::string& why)
{
    TrapData data;
    if (!ReadTrap(hunter, trapSpell, data))
    {
        why = "no trap data";
        return false;
    }
    spot = TrapSpot(hunter, target, data.triggerRadius);
    float const walkSec = hunter->GetExactDist(spot) / std::max(hunter->GetSpeed(MOVE_RUN), 0.1f);
    float const hunterDps = GroupDamage::MemberSustainedDps(hunter);
    float const groupDps = GroupDamage::SustainedDps(tank);
    if (hunterDps <= 0.0f || groupDps <= 0.0f)
    {
        why = "no measured damage yet";
        return false;
    }
    // the mobs the trap covers; none crowd-controlled, none unpulled within its radius
    std::list<Unit*> nearUnits;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, hunter, data.radius + data.triggerRadius);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, nearUnits, check);
    Cell::VisitObjects(target, searcher, data.radius + data.triggerRadius);
    std::vector<Unit*> const fight = FightMobs(tank, seats);
    std::vector<Unit*> covered;
    for (Unit* unit : nearUnits)
    {
        if (!unit->IsAlive() || unit->GetExactDist(spot) > data.radius)
            continue;
        if (IsCrowdControlled(unit))
        {
            why = unit->GetName() + " is crowd-controlled in the trap's radius";
            return false;
        }
        if (std::find(fight.begin(), fight.end(), unit) == fight.end())
        {
            if (unit->IsCreature() && !unit->IsInCombat())
            {
                why = unit->GetName() + " is not pulled, in the trap's radius";
                return false;
            }
            continue;
        }
        covered.push_back(unit);
    }
    if (std::find(covered.begin(), covered.end(), target) == covered.end())
        covered.push_back(target);
    // the group kills them one after another: each lives its own health and the ones before it over the
    // group's measured dps; the trap goes off after the walk and its arming
    std::sort(covered.begin(), covered.end(), [](Unit* a, Unit* b) { return a->GetHealth() < b->GetHealth(); });
    float const offAfter = walkSec + data.armSec;
    float health = 0.0f;
    float gain = 0.0f;
    for (Unit* mob : covered)
    {
        health += mob->GetHealth();
        gain += DamageOn(hunter, mob, data, health / groupDps - offAfter);
    }
    float const lostSec = 2.0f * walkSec + data.gcdSec;
    float const cost = lostSec * hunterDps;
    facts.Num("spell", trapSpell)
        .Num("mobs", int64(covered.size()))
        .Real("gain", gain)
        .Real("cost", cost)
        .Real("walkSec", walkSec)
        .Real("lostSec", lostSec)
        .Real("hunterDps", hunterDps)
        .Real("triggerRadius", data.triggerRadius)
        .Real("radius", data.radius)
        .Real("armSec", data.armSec)
        .Real("spotToTarget", target->GetExactDist(spot))
        .Real("targetMelee", target->GetMeleeRange(hunter))
        .Real("x", spot.GetPositionX())
        .Real("y", spot.GetPositionY())
        .Real("z", spot.GetPositionZ());
    if (gain <= cost)
    {
        why = "the shots lost cost more than the trap deals";
        return false;
    }
    return true;
}
}  // namespace

bool TickMove(LeaderGroup& group, Player* hunter, Player* tank, Unit* target, std::vector<Player*> const& seats,
              std::function<bool(Position const&, std::string const&)> const& move)
{
    if (!hunter || hunter->getClass() != CLASS_HUNTER || !target)
        return false;
    auto& plans = group.State().trapWeave;
    uint32 const now = getMSTime() ? getMSTime() : 1;
    auto& refused = group.State().trapWeaveRefused;
    // [lead276] b196 rfc-h 5A / 5E: the weave ended on every step of its target ("left the trap spot's reach") and on
    // its death, the combat spot took the hunter back for a tick and the next tick decided the weave again
    // (command_repeat combat spot <-> trap weave). One decision per fact: the spot follows its target while the plan
    // stands, and a plan whose target died is judged again on the hunter's next target at once, from where it walks
    bool retarget = false;
    auto found = plans.find(hunter->GetGUID());
    if (found != plans.end())
    {
        DungeonLeaderState::TrapWeave& plan = found->second;
        Unit* planned = ObjectAccessor::GetUnit(*hunter, plan.target);
        if (hunter->HasSpellCooldown(plan.spell))
        {
            End(group, hunter, "the trap is down");
            return false;
        }
        if (!planned || !planned->IsAlive() || planned->GetVictim() != tank)
        {
            End(group, hunter, !planned || !planned->IsAlive() ? "the target died" : "the target left the tank");
            retarget = true;
        }
        else
        {
            // the target stepped off the spot's trigger reach: the spot follows it once it stands again
            if (!planned->isMoving() &&
                planned->GetExactDist(plan.spot) > planned->GetCombatReach() + plan.triggerRadius)
            {
                plan.spot = TrapSpot(hunter, planned, plan.triggerRadius);
                plan.step = 0;
            }
            if (!move(plan.spot, "trap weave"))
            {
                refused[hunter->GetGUID()] = {plan.target, plan.spot};
                End(group, hunter, "the walk to the trap spot is refused");
                return false;
            }
            if (!plan.step && hunter->GetExactDist2d(plan.spot.GetPositionX(), plan.spot.GetPositionY()) <= 1.0f)
                plan.step = 1;
            return true;
        }
    }

    // a new weave: the target on the tank and standing, the trap ready, the hunter not under way (unless its weave
    // walk is what moves it: the next target is judged from there)
    uint32 const trapSpell = TrapSpell(hunter);
    if (!trapSpell || hunter->HasSpellCooldown(trapSpell) || (hunter->isMoving() && !retarget) ||
        target->isMoving() || target->GetVictim() != tank)
        return false;
    Position spot;
    JsonObject facts;
    std::string why;
    bool const pays = Decide(hunter, tank, target, seats, trapSpell, spot, facts, why);
    // the verdict once per target and answer
    std::string& logged = group.State().trapWeaveLogged[hunter->GetGUID()];
    std::string const key = target->GetGUID().ToString() + (pays ? "|go" : "|" + why);
    if (logged != key)
    {
        logged = key;
        Decisions().Write(group.Run(), "leader", "trap_weave", target->GetGUID(), target->GetName(), true,
                          facts.Str("member", hunter->GetName()).Bool("pays", pays).Str("why", pays ? "" : why));
    }
    if (!pays)
        return false;
    // a refused walk stands until a fact changes: another target, or the spot moved off the refused one
    if (auto const was = refused.find(hunter->GetGUID()); was != refused.end())
    {
        if (was->second.first == target->GetGUID() && was->second.second.GetExactDist(spot) <= 1.0f)
            return false;
        refused.erase(was);
    }
    TrapData data;
    ReadTrap(hunter, trapSpell, data);
    DungeonLeaderState::TrapWeave& plan = plans[hunter->GetGUID()];
    plan.target = target->GetGUID();
    plan.spell = trapSpell;
    plan.spot = spot;
    plan.triggerRadius = data.triggerRadius;
    plan.step = 0;
    plan.orderedMs = now;
    if (!move(spot, "trap weave"))
    {
        refused[hunter->GetGUID()] = {plan.target, spot};
        End(group, hunter, "the walk to the trap spot is refused");
        return false;
    }
    return true;
}

void TickDrop(LeaderGroup& group, std::vector<Player*> const& seats, bool fighting,
              std::function<void(Player*, char const*, JsonObject)> const& logEvent)
{
    auto& plans = group.State().trapWeave;
    if (plans.empty())
        return;
    for (Player* hunter : seats)
    {
        auto const found = plans.find(hunter->GetGUID());
        if (found == plans.end())
            continue;
        if (!fighting || !hunter->IsAlive())
        {
            End(group, hunter, "the fight ended");
            continue;
        }
        if (found->second.step != 1)
            continue;
        if (Leader::Events::SetExternal(hunter->GetGUID(), found->second.spell, ObjectGuid::Empty))
            logEvent(hunter, "External",
                     JsonObject().Num("spell", found->second.spell).Str("on", "its feet").Str("why", "trap weave"));
    }
}
}  // namespace Leader::TrapWeave
