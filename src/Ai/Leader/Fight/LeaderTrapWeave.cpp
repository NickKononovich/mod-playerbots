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
#include "LeaderGround.h"  // [navz10] LeaderGroundZ
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellPayoff.h"  // [lead1541]
#include "StringFormat.h"
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

// [lead1541] Lock and Load (talent 56342-56344, spell_hun_lock_and_load): a fire trap's periodic damage rolls a proc
// (Rawr ShotPriority.cs:235: 0.02 x the talent rank a tick - Rawr's value over wowsims'), none while its marker
// (67544, the 22 s internal cooldown) is up; the proc (56453) gives two Explosive Shots at no cost with the shared
// cooldown reset (wowsims applyLockAndLoad: 2 stacks)
constexpr uint32 LOCK_AND_LOAD_RANKS[] = {56342, 56343, 56344};
constexpr uint32 SPELL_LOCK_AND_LOAD_MARKER = 67544;
constexpr float RAWR_LOCK_AND_LOAD_PER_RANK = 0.02f;  // Rawr ShotPriority.cs:235
constexpr uint32 LOCK_AND_LOAD_SHOTS = 2;             // wowsims applyLockAndLoad SetStacks(2)
// [lead1541] wowsims explosive_trap.go TrapWeaveSpell: the ranged auto waits until the weave is done + 500 ms
constexpr float WOWSIMS_WEAVE_AUTO_DELAY_SEC = 0.5f;

float LockAndLoadChance(Player* hunter)
{
    if (hunter->HasAura(SPELL_LOCK_AND_LOAD_MARKER))
        return 0.0f;
    for (uint8 rank = 3; rank >= 1; --rank)
        if (hunter->HasSpell(LOCK_AND_LOAD_RANKS[rank - 1]))
            return RAWR_LOCK_AND_LOAD_PER_RANK * rank;
    return 0.0f;
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
    // [navz10] the floor the leader's other spot builders use ([dun206]), not Map::GetHeight raw: this site was
    // missed by that pass, and a raw answer here is the grid's placeholder in a WMO-only instance (2180 of 5744
    // .map tiles hold no height at all). A rejected height keeps the old fallback, the target's own z.
    float const z = Leader::Travel::LeaderGroundZ(hunter, x, y, target->GetPositionZ() + 2.0f);
    return Position(x, y, z > INVALID_HEIGHT ? z : target->GetPositionZ());
}

// [lead534] the first mob within the trap's radius of `spot` the leader spares from this hunter's area damage (its
// AoE verdict, the hunter's Attack event: no threat headroom on it), nullptr: none
Unit* SparedInRadius(Player* hunter, Position const& spot, float radius)
{
    Leader::MemberEvents events;
    if (!Leader::Events::Get(hunter->GetGUID(), events))
        return nullptr;
    for (ObjectGuid const& guid : events.aoeSpared)
        if (Unit* mob = ObjectAccessor::GetUnit(*hunter, guid);
            mob && mob->IsAlive() && mob->GetExactDist(spot) <= radius)
            return mob;
    return nullptr;
}

// [lead1538] rot550 b275 ac-h 5A-9 697.9 s / 5E-13 537-549 s: the weave was ordered at 6 % mana and the External
// failed SPELL_FAILED_NO_POWER. The trap's cost is its spell's (SpellInfo::CalcPowerCost: the base mana share with the
// hunter's cost modifiers, Resourcefulness among them - wowsims explosive_trap.go's 19 % x (1 - 0.2 x
// Resourcefulness)): no weave the hunter cannot pay. [lead1551] b276 rfc-h 5E-5 528.2-530.3 s / ac-h 5E-13 692 s: the
// weave paid at its order and the shots on its walk spent the mana below the cost before the drop (NO_POWER again):
// the cost is judged on every step of the plan, the drop included
bool Pays(Player* hunter, uint32 trapSpell, std::string& why)
{
    SpellInfo const* trapInfo = sSpellMgr->GetSpellInfo(trapSpell);
    if (!trapInfo)
        return true;
    int32 const cost = trapInfo->CalcPowerCost(hunter, trapInfo->GetSchoolMask());
    if (cost <= 0 || int32(hunter->GetPower(POWER_MANA)) >= cost)
        return true;
    why = Acore::StringFormat("its mana {} < the trap's cost {} [lead1538] [lead1551]", hunter->GetPower(POWER_MANA),
                              cost);
    return false;
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
    if (!Pays(hunter, trapSpell, why))
        return false;
    spot = TrapSpot(hunter, target, data.triggerRadius);
    float const walkSec = hunter->GetExactDist(spot) / std::max(hunter->GetSpeed(MOVE_RUN), 0.1f);
    float const hunterDps = GroupDamage::MemberFormulaDps(hunter);  // [lead1303] the formula's
    float const groupDps = GroupDamage::GroupFormulaDps(tank);
    if (hunterDps <= 0.0f || groupDps <= 0.0f)
    {
        why = "no damage by the formula";
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
    // [lead534] b212 rfc-h 5H 555.0-560.7 s: the weave went to a 7-mob pull's Enforcers the leader spared from the
    // hunter's area damage ("spares Searing Blade Enforcer (no threat headroom)" on every Attack of it); the trap's
    // threat took three of them off the tank (5711 vs 3721, "threat on a mob it was not told to hit"). The trap is the
    // hunter's area damage: one that reaches a spared mob is no trap
    if (Unit* spared = SparedInRadius(hunter, spot, data.radius))
    {
        why = spared->GetName() + " is spared (no threat headroom), in the trap's radius";
        return false;
    }
    // the group kills them one after another: each lives its own health and the ones before it over the
    // group's measured dps; the trap goes off after the walk and its arming
    std::sort(covered.begin(), covered.end(), [](Unit* a, Unit* b) { return a->GetHealth() < b->GetHealth(); });
    float const offAfter = walkSec + data.armSec;
    float health = 0.0f;
    float gain = 0.0f;
    uint32 tickRolls = 0;  // [lead1541] the trap's periodic hits on the covered mobs
    for (Unit* mob : covered)
    {
        health += mob->GetHealth();
        float const lifeSec = health / groupDps - offAfter;
        gain += DamageOn(hunter, mob, data, lifeSec);
        if (lifeSec > 0.0f && data.periodicEffect >= 0 && data.tickSec > 0.0f)
            tickRolls += std::min(data.ticks, uint32(lifeSec / data.tickSec));
    }
    // [lead1541] the Lock and Load procs the ticks roll (at most one: the 22 s marker outlasts the trap), worth two
    // free Explosive Shots over the shots their global cooldowns would have been (the hunter's measured dps)
    float lockAndLoad = 0.0f;
    if (float const chance = LockAndLoadChance(hunter); chance > 0.0f && tickRolls)
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(hunter))
            if (uint32 const shot = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "explosive shot")->Get())
            {
                SpellPayoff::Cast es;
                SpellInfo const* esInfo = sSpellMgr->GetSpellInfo(shot);
                if (esInfo && SpellPayoff::Read(hunter, target, shot, es))
                {
                    float const procs = 1.0f - std::pow(1.0f - chance, float(tickRolls));
                    float const gcdSec = esInfo->StartRecoveryTime / 1000.0f;
                    lockAndLoad = procs * LOCK_AND_LOAD_SHOTS * std::max(0.0f, es.Total() - hunterDps * gcdSec);
                    gain += lockAndLoad;
                }
            }
    // [lead1541] wowsims' weave (TrapWeaveSpell): half the weave out, the trap laid there inside its global cooldown,
    // half back; no shot until it is done (the GCD set to its end) and the ranged auto 500 ms after - the shots lost
    // are the longer of the walk there and back and the trap's GCD, the auto shot's extra wait on top
    float autoDps = 0.0f;
    if (uint32 const attackMs = hunter->GetAttackTime(RANGED_ATTACK))
        autoDps = (hunter->GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE) + hunter->GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE)) /
                  2.0f / (attackMs / 1000.0f);
    float const lostSec = std::max(2.0f * walkSec, data.gcdSec);
    float const cost = lostSec * hunterDps + WOWSIMS_WEAVE_AUTO_DELAY_SEC * autoDps;
    facts.Num("spell", trapSpell)
        .Num("mobs", int64(covered.size()))
        .Real("gain", gain)
        .Real("lockAndLoad", lockAndLoad)
        .Num("tickRolls", int64(tickRolls))
        .Str("tag", "[lead1541]")
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
        if (std::string unpaid; !Pays(hunter, plan.spell, unpaid))
        {
            End(group, hunter, unpaid.c_str());
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
            // [lead534] ... and a mob spared since the weave was ordered ends it before the trap drops
            TrapData data;
            if (ReadTrap(hunter, plan.spell, data) && SparedInRadius(hunter, plan.spot, data.radius))
            {
                End(group, hunter, "a spared mob (no threat headroom) in the trap's radius");
                return false;
            }
            if (!move(plan.spot, "trap weave"))
            {
                refused[hunter->GetGUID()] = {plan.target, plan.spot};
                End(group, hunter, "the walk to the trap spot is refused");
                return false;
            }
            // [lead523] b211 rfc-h 5E: approved weaves ran 1.5-8 s and ended "the target died" with 0 Explosive Traps -
            // the hunter's walk stopped short of the 1 yd it waited for (the walk's own arrival), Execute "on the
            // spot". The trap drops at its feet: it is down where the target stands in its trigger reach (the
            // same reach the spot is kept by above)
            if (!plan.step &&
                (hunter->GetExactDist2d(plan.spot.GetPositionX(), plan.spot.GetPositionY()) <= 1.0f ||
                 (!hunter->isMoving() &&
                  planned->GetExactDist(hunter) <= planned->GetCombatReach() + plan.triggerRadius)))
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
        // [lead1551] the drop is paid now too (the walk's shots may have spent the mana)
        if (std::string unpaid; !Pays(hunter, found->second.spell, unpaid))
        {
            End(group, hunter, unpaid.c_str());
            continue;
        }
        if (Leader::Events::SetExternal(hunter->GetGUID(), found->second.spell, ObjectGuid::Empty))
            logEvent(hunter, "External",
                     JsonObject().Num("spell", found->second.spell).Str("on", "its feet").Str("why", "trap weave"));
    }
}
}  // namespace Leader::TrapWeave
