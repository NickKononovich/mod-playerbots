/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderPrepull.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <set>

#include "AiFactory.h"
#include "CellImpl.h"
#include "ChooseTargetActions.h"  // [lead1240] PullReadiness::MemberBossManaLine
#include "Creature.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "HealthTriggers.h"  // RestState
#include "InstanceGuid.h"    // [lead823]
#include "LeaderFightChecks.h"
#include "LeaderRest.h"  // CountRestItems
#include "LeaderOrders.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RouteTargets.h"  // [lead821] Travel::JoinsBossFight
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"

namespace Leader::Prepull
{
namespace
{
using Leader::Fight::SPELL_ARMY_OF_THE_DEAD;

// a death knight that casts Army before a pull in wowsims: Blood (tank and dps) and Frost, not Unholy
// [lead750] the tank again (wowsims blood_aggro: Army at -6 s; b220 without it the blood tank read 69-73 % of a sim
// that has a fresh Army at every boss). lead725 took it off for b219 rfc-h 5C: the channel's ghouls woke a Ragefire
// Shaman beside the knight (pull 17 at 268 s started during the channel, "firstMember" none) and fought it with the
// boss on them. The ghouls attack only what fights their owner (npc_pet_dk_army_of_the_dead::CanAIAttack) and the
// owner's target when it attacks (OwnerAttacked): the tank's opener goes first. What they wake by standing there is
// the knight's to judge before the channel: ArmyBlocked's idle-mob reach
bool ArmyCaster(Player* bot)
{
    return bot && bot->IsAlive() && bot->getClass() == CLASS_DEATH_KNIGHT && bot->HasSpell(SPELL_ARMY_OF_THE_DEAD) &&
           AiFactory::GetPlayerSpecTab(bot) != DEATH_KNIGHT_TAB_UNHOLY;
}

// [lead750] how far from the knight its ghouls land: the largest radius of the Army's effects and of the spells they
// trigger (the summons) - spell data
float GhoulSpreadYd()
{
    float spread = 0.0f;
    SpellInfo const* army = sSpellMgr->GetSpellInfo(SPELL_ARMY_OF_THE_DEAD);
    if (!army)
        return spread;
    for (SpellEffectInfo const& effect : army->GetEffects())
    {
        spread = std::max(spread, effect.CalcRadius());
        if (SpellInfo const* summon = effect.TriggerSpell ? sSpellMgr->GetSpellInfo(effect.TriggerSpell) : nullptr)
            for (SpellEffectInfo const& landing : summon->GetEffects())
                spread = std::max(spread, landing.CalcRadius());
    }
    return spread;
}

// [lead823] the trash in the tank knight's ghoul landing where it would channel for the boss, per boss (instance,
// GUID): the boss's trash to clear before his pull (Travel::BossAreaTrashOf reads ArmyLandingTrash)
std::mutex landingLock;
std::map<InstanceGuid, std::set<ObjectGuid>> landingTrash;

void SetLanding(Creature* boss, std::set<ObjectGuid> trash)
{
    std::lock_guard<std::mutex> lock(landingLock);
    if (trash.empty())
        landingTrash.erase(InstanceKey(boss));
    else
        landingTrash[InstanceKey(boss)] = std::move(trash);
}

bool LandingHasTrash(Creature* boss)
{
    std::lock_guard<std::mutex> lock(landingLock);
    return landingTrash.count(InstanceKey(boss)) != 0;
}

// [lead821] (boss, mob) pairs already logged as joining the boss's fight
std::mutex joinLoggedLock;
std::set<std::pair<InstanceGuid, ObjectGuid>> joinLogged;

// What the knight's ghouls wake where they land: the first mob that stops the Army (nullptr: none), why, and the trash
// in that reach to clear before the boss
struct Wake
{
    Unit* blocker = nullptr;
    std::string why;
    std::set<ObjectGuid> clearFirst;
};

// [lead750] a mob out of combat (the boss included) whose aggro reach (Creature::GetAggroRange, the game's) takes in
// a place the knight's ghouls land on: within the ghouls' spread of the knight
// [lead822] by the core's own aggro test (Creature::CanStartAttack: GetAggroRange + m_CombatDistance in 3D, the
// height within CREATURE_Z_ATTACK_RANGE + m_CombatDistance unless it flies, and its line of sight), the ghouls standing
// round the knight - the 2D distance alone took in mobs on another floor (RFC Oggleflint's ledge, dz 13)
// [lead821] user 2026-11-03: a mob that joins the boss's fight anyway (Travel::JoinsBossFight: his formation, a script
// call for help, the assistance at his pull, its aggro reach on his spot) never stops it - it is in that fight
// whether the ghouls wake it or not. b222 rfc-h 5G 211 s, 5C 269 / 274 s: "its ghouls land in Ragefire Trogg's aggro
// reach" before Oggleflint (which Trogg the line did not say: the reason now carries its distance, height and
// reach); his Ragefire Shaman and Trogg 11-13 yd from him (spawns 48643 / 48645) were in his 5G pull (pull 17, 3 kills)
// [lead823] user 2026-11-03: the others are the boss's trash, on his path: cleared before his pull (`clearFirst`)
// [lead910] `at`: where the knight would channel (its own place, or a step out of the boss's reach)
Wake GhoulsWakeAt(Player* dk, Creature* boss, Position const& at)
{
    Wake wake;
    float const spread = GhoulSpreadYd();
    float const search = MAX_AGGRO_RADIUS * sWorld->getRate(RATE_CREATURE_AGGRO) + spread + dk->GetExactDist(at);
    std::list<Unit*> nearUnits;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(dk, dk, search);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(dk, nearUnits, check);
    Cell::VisitObjects(dk, searcher, search);
    for (Unit* unit : nearUnits)
    {
        Creature* mob = unit->ToCreature();
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->IsCivilian() || !dk->IsValidAttackTarget(mob))
            continue;
        float const reach = mob->GetAggroRange(dk) + mob->m_CombatDistance;
        float const dz = std::fabs(mob->GetPositionZ() - at.GetPositionZ());
        if ((!mob->CanFly() && dz > CREATURE_Z_ATTACK_RANGE + mob->m_CombatDistance) ||
            mob->GetExactDist(at) > reach + spread ||
            !mob->IsWithinLOS(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ() + dk->GetCollisionHeight()))
            continue;
        if (boss && mob != boss)
            if (char const* joins = Leader::Travel::JoinsBossFight(dk, boss, mob))
            {
                std::pair<InstanceGuid, ObjectGuid> const key{InstanceKey(boss), mob->GetGUID()};
                std::lock_guard<std::mutex> lock(joinLoggedLock);
                if (joinLogged.insert(key).second)
                    LOG_INFO("playerbots",
                             "[lead821] {}: {} in its ghouls' landing ({:.1f} yd, reach {:.1f} + {:.1f} spread) is in "
                             "{}'s fight anyway ({}): the prepull Army goes",
                             dk->GetName(), mob->GetName(), dk->GetExactDist(mob), reach, spread, boss->GetName(),
                             joins);
                continue;
            }
        bool const trash = boss && mob != boss && !mob->IsDungeonBoss() && !mob->isWorldBoss() &&
                           mob->GetSpawnId() && !mob->IsSummon();
        if (trash)
            wake.clearFirst.insert(mob->GetGUID());
        if (!wake.blocker)
        {
            wake.blocker = mob;
            wake.why = Acore::StringFormat("its ghouls land in {}'s aggro reach ({:.1f} yd, dz {:.1f}, reach {:.1f} + "
                                           "{:.1f} spread){}",
                                           mob->GetName(), mob->GetExactDist(at), dz, reach, spread,
                                           mob == boss ? ": the boss himself" : "");
        }
    }
    if (!wake.clearFirst.empty())
        wake.why += Acore::StringFormat("; {} mob(s) there cleared before {}", wake.clearFirst.size(),
                                        boss ? boss->GetName() : std::string("the boss"));
    return wake;
}

Wake GhoulsWake(Player* dk, Creature* boss) { return GhoulsWakeAt(dk, boss, dk->GetPosition()); }

// [lead910] how near the tank knight's step move stops (the MoveTo tolerance its step order carries)
constexpr float STEP_ARRIVE_YD = 1.0f;

// [lead910] user 2026-11-03 (44): the tank knight's place where the boss's own aggro reach takes in its ghouls'
// landing: the nearest spot by its walk (navmesh, Leader::WalkYd) round it where the ghouls wake nothing (the boss's
// reach plus their spread, every other idle mob's too) and every group member still heals / sees it (HealReaches,
// the group's reach); false: none
bool StepOutSpot(Player* dk, Creature* boss, std::vector<Player*> const& members, Position& out)
{
    constexpr float STEP_YD = 3.0f;
    constexpr uint32 DIRECTIONS = 16;
    float const farthest = boss->GetAggroRange(dk) + boss->m_CombatDistance + GhoulSpreadYd() + STEP_YD;
    Map* map = dk->GetMap();
    for (float ring = STEP_YD; ring <= farthest; ring += STEP_YD)
    {
        std::optional<Position> best;
        float bestWalk = 0.0f;
        for (uint32 i = 0; i < DIRECTIONS; ++i)
        {
            float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
            float x = dk->GetPositionX() + std::cos(angle) * ring;
            float y = dk->GetPositionY() + std::sin(angle) * ring;
            float z = dk->GetPositionZ();
            if (!map->CheckCollisionAndGetValidCoords(dk, dk->GetPositionX(), dk->GetPositionY(), dk->GetPositionZ(), x,
                                                      y, z) ||
                std::fabs(z - dk->GetPositionZ()) > 4.0f)
                continue;
            Position const spot(x, y, z);
            // [lead1270] b250 rfc-h 5C-3 646 s: the collision clip put a spot 0.2 yd off - inside the step move's
            // stop, so the knight never walked and never left the boss's reach: no step
            if (dk->GetExactDist(spot) <= STEP_ARRIVE_YD)
                continue;
            if (best && dk->GetExactDist(spot) >= bestWalk)
                continue;
            if (GhoulsWakeAt(dk, boss, spot).blocker)
                continue;
            bool reached = true;
            for (Player* member : members)
                if (member != dk && member->IsAlive() && !Leader::Fight::HealReaches(dk, member->GetPosition(), spot))
                    reached = false;
            if (!reached)
                continue;
            Leader::RouteResult const route = Leader::Route(dk, spot);
            if (!route.found || !route.complete)
                continue;
            if (float const walk = Leader::WalkYd(dk, spot); !best || walk < bestWalk)
            {
                best = spot;
                bestWalk = walk;
            }
        }
        if (best)
        {
            out = *best;
            return true;
        }
    }
    return false;
}

// [lead881] a reason ArmyBlocked gives that ends by itself while the knight stands out of combat: its walk (it
// stops at its spot) and its runes (they come back by their own cooldowns, Player::GetRuneCooldown)
bool ArmyBlockPasses(std::string const& why) { return why == "walking" || why == "its runes are not ready"; }

// [lead910] the tank knight's step for its prepull Army, kept per plan: searched once from where it stands when the
// boss's own reach blocks its landing; true while the step is the way to its channel (the leader keeps the move on)
bool StepOut(LeaderGroup& group, DungeonLeaderState::ArmyPrepull& plan, Player* dk, Creature* boss,
             std::vector<Player*> const& members, std::string const& blocked)
{
    if (!PlayerbotAI::IsTank(dk) || plan.done || plan.startedMs)
        return false;
    if (!plan.stepTo)
    {
        if (blocked.find(": the boss himself") == std::string::npos || dk->isMoving() || dk->IsInCombat())
            return false;
        if (plan.stepNone && dk->GetExactDist(plan.stepFrom) <= 1.5f)
            return false;
        plan.stepFrom = dk->GetPosition();
        Position spot;
        plan.stepNone = !StepOutSpot(dk, boss, members, spot);
        if (plan.stepNone)
        {
            LOG_INFO("playerbots", "[lead910] {}: {}; no spot out of {}'s reach in the group's reach", dk->GetName(),
                     blocked, boss->GetName());
            return false;
        }
        plan.stepTo = spot;
        LOG_INFO("playerbots", "[lead910] {}: {}; steps {:.1f} yd out of {}'s reach to channel", dk->GetName(),
                 blocked, Leader::WalkYd(dk, spot), boss->GetName());
        group.LogEvent("army_prepull_step", JsonObject()
                                                .Str("dk", dk->GetName())
                                                .Str("boss", boss->GetName())
                                                .Str("why", blocked)
                                                .Real("walk", Leader::WalkYd(dk, spot)));
    }
    // [lead1270] b250 rfc-h 5C-3 646-885 s: the knight stood on its step and its ghouls still woke the boss there; the
    // boss pull waited for the step forever. Standing at the step with a reason that does not pass by itself: the
    // step does not clear it - dropped, the gate goes on without the Army (NoteArmyBlocked logs the reason)
    if (!blocked.empty() && !ArmyBlockPasses(blocked) && !dk->isMoving() &&
        dk->GetExactDist(*plan.stepTo) <= STEP_ARRIVE_YD)
    {
        LOG_INFO("playerbots", "[lead1270] {}: stands on its step and {}; no prepull Army before {}", dk->GetName(),
                 blocked, boss->GetName());
        group.LogEvent("army_prepull_step_failed", JsonObject()
                                                       .Str("dk", dk->GetName())
                                                       .Str("boss", boss->GetName())
                                                       .Str("why", blocked)
                                                       .Str("tag", "[lead1270]"));
        plan.stepTo.reset();
        plan.stepFrom = dk->GetPosition();
        plan.stepNone = true;
        return false;
    }
    Leader::Events::SetMove(dk->GetGUID(), Leader::MoveEvent::MoveTo, *plan.stepTo, STEP_ARRIVE_YD,
                            "army: steps out of the boss's reach to channel");
    return true;
}

Spell* ArmyChannel(Player* dk)
{
    Spell* channel = dk->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    return channel && channel->GetSpellInfo() && channel->GetSpellInfo()->Id == SPELL_ARMY_OF_THE_DEAD ? channel
                                                                                                       : nullptr;
}

// the channel's length (spell data)
float ChannelSec()
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_ARMY_OF_THE_DEAD);
    return info ? std::max(info->GetDuration(), 0) / 1000.0f : 0.0f;
}

// the DK can start the channel now: out of combat, standing, the cooldown off, the spell's runes ready
// (SpellRuneCost.dbc by RuneType, as Spell::CheckRuneCost reads it; a Death rune stands in for any); empty: yes
// [lead823] `atPrep`: the knight stands where it would channel for `boss` (his preparation, the buff pass's end, the
// boss pull's gate) - the trash its ghouls would wake there is his to clear first (ArmyLandingTrash); a stand on the
// way to him (TickArmy before the preparation) only stops the channel
std::string ArmyBlocked(Player* dk, Creature* boss, bool atPrep)
{
    bool const tank = PlayerbotAI::IsTank(dk);
    if (!dk->IsAlive() || dk->HasSpellCooldown(SPELL_ARMY_OF_THE_DEAD))
    {
        if (tank && boss)
            SetLanding(boss, {});  // [lead823] no channel, no landing
        return !dk->IsAlive() ? "dead" : "on cooldown";
    }
    if (dk->IsInCombat())
        return "in combat";
    if (dk->isMoving())
        return "walking";
    // [lead750] the tank's ghouls wake nothing where they land: the boss is the tank's to open, its adds stay idle
    if (tank)
    {
        Wake wake = GhoulsWake(dk, boss);
        if (atPrep && boss)
            SetLanding(boss, std::move(wake.clearFirst));  // [lead823]
        if (wake.blocker)
            return wake.why;
    }
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_ARMY_OF_THE_DEAD);
    SpellRuneCostEntry const* cost = info ? sSpellRuneCostStore.LookupEntry(info->RuneCostID) : nullptr;
    if (!cost)
        return std::string();
    uint32 ready[NUM_RUNE_TYPES] = {};
    for (uint8 i = 0; i < MAX_RUNES; ++i)
        if (!dk->GetRuneCooldown(i))
            ++ready[dk->GetCurrentRune(i)];
    uint32 death = ready[RUNE_DEATH];
    for (uint8 i = 0; i < RUNE_DEATH; ++i)
    {
        uint32 const have = ready[i];
        if (have >= cost->RuneCost[i])
            continue;
        uint32 const missing = cost->RuneCost[i] - have;
        if (death < missing)
            return "its runes are not ready";
        death -= missing;
    }
    return std::string();
}

// [lead881] seconds until every rune is back (the longest rune cooldown; a log fact, no decision reads it)
float RuneWaitSec(Player* dk)
{
    uint32 longest = 0;
    for (uint8 i = 0; i < MAX_RUNES; ++i)
        longest = std::max(longest, dk->GetRuneCooldown(i));
    return longest / 1000.0f;
}

// the boss whose pull the group prepares for: the route's boss target
Creature* PreparedBoss(LeaderGroup& group, Map* map)
{
    ObjectGuid const guid = group.RouteTargetIsBoss() ? group.RouteTarget() : ObjectGuid::Empty;
    Creature* boss = guid.IsEmpty() || !map ? nullptr : map->GetCreature(guid);
    if (!boss || !boss->IsAlive() || boss->IsInCombat() || !(boss->IsDungeonBoss() || boss->isWorldBoss()))
        return nullptr;
    return boss;
}

// the member's mana regen per second now (the game's own: UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER, the drink's
// SPELL_AURA_MOD_POWER_REGEN included, times the server's mana rate)
float ManaPerSec(Player* bot)
{
    return bot->GetFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER + AsUnderlyingType(POWER_MANA)) *
           sWorld->getRate(RATE_POWER_MANA);
}

// the member's food heal per second (its SPELL_AURA_OBS_MOD_HEALTH auras: amount per amplitude)
float FoodHealthPerSec(Player* bot)
{
    float perSec = 0.0f;
    for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_OBS_MOD_HEALTH))
        if (effect->GetAmplitude() > 0)
            perSec += effect->GetAmount() * 1000.0f / effect->GetAmplitude();
    return perSec;
}

// ms until an eater's food gives Well Fed: the food aura's periodic trigger; -1: no such timer
int32 WellFedMs(Player* bot)
{
    int32 left = -1;
    for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL))
        if (effect->GetBase() && effect->GetBase()->GetSpellInfo()->HasAura(SPELL_AURA_OBS_MOD_HEALTH))
            left = std::max(left, effect->GetPeriodicTimer());
    return left;
}

// [lead1240] the first member under its boss mana line (BossReadyGap's) that has a drink to rest with; empty: none
std::string UnderBossManaLine(std::vector<Player*> const& members, Unit* boss)
{
    for (Player* bot : members)
    {
        if (!bot->IsAlive() || !bot->GetMaxPower(POWER_MANA) || Leader::Dungeon::CountRestItems(bot, 59) == 0)
            continue;
        float const line = PullReadiness::MemberBossManaLine(bot, boss);
        if (bot->GetPowerPct(POWER_MANA) < line)
            return Acore::StringFormat("{} at {:.0f} % mana under its boss line {:.0f} %", bot->GetName(),
                                       bot->GetPowerPct(POWER_MANA), line);
    }
    return std::string();
}

// The boss preparation still ahead, seconds, from the members' facts; -1: not computable yet (the group is
// not in the preparation, the buff pass still gives buffs, a member that must rest has not sat down)
float PrepLeftSec(LeaderGroup& group, Creature* boss, std::vector<Player*> const& members, std::string& why)
{
    GroupRouteState const& route = group.Route();
    // the buff pass is the boss's preparation: the group stands at the boss's buff distance, the pull follows it
    if (!route.needBuff || !route.buffStep || route.target != boss->GetGUID())
    {
        why = "the group is not in the boss's preparation";
        return -1.0f;
    }
    if (route.buffStep < 3)
    {
        why = "the buff pass gives its buffs";
        return -1.0f;
    }
    float left = 0.0f;
    why = "nothing left";
    auto const note = [&](float sec, std::string const& what)
    {
        if (sec > left)
        {
            left = sec;
            why = what;
        }
    };
    for (Player* bot : members)
    {
        if (!bot->IsAlive())
            continue;
        // [lead504] b210 rfc-h 5G 230.3 s: Army of the Dead "the preparation is over" at the buff pass end, while
        // the holy paladin's rebuff window stayed open (boss_prep, Holy Light top-ups) and the leader's "the group
        // buffs" held Oggleflint to 247 s - the ghouls went for the unpulled boss and were stopped 8 times ("not in
        // the group's fight"), the same at Bazzalan. A member still buffing (BuffWorkers: its rebuff window open or
        // casting) holds the pull for an unknown time: the channel waits for it
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot); botAI && botAI->forceRebuff.IsPending())
        {
            why = bot->GetName() + " rebuff window open";
            return -1.0f;
        }
        if (bot->IsNonMeleeSpellCast(false) && !ArmyCaster(bot))
        {
            why = bot->GetName() + " casting";
            return -1.0f;
        }
        // the buff pass's food step: an eater without Well Fed waits for its food's trigger
        if (route.buffStep == 3 && RestState::IsEating(bot))
        {
            int32 const fed = WellFedMs(bot);
            if (fed < 0)
            {
                why = bot->GetName() + " eats (no Well Fed timer)";
                return -1.0f;
            }
            note(fed / 1000.0f, bot->GetName() + " until Well Fed");
        }
        // the pass's ready step waits for a member below its own rest line that can rest (TickBuffing's rule)
        bool const canRest = bot->IsSitState() || RestState::CanRest(bot);
        float const ownLine = RestState::OwnManaLine(bot);
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        float const mana = maxMana ? bot->GetPowerPct(POWER_MANA) : 100.0f;
        // [lead1240] b246 rfc-h 5C-11 243 s: the Army went with the moonkin at 2 % mana walking to its spot (not
        // resting yet: no own-line wait) - the boss pull then held 49 s for its 36 % boss line, the ghouls woke a
        // Trogg of Oggleflint's pack and fought it alone 22 s, the Army was spent before the boss. The boss pull
        // waits for the boss mana line (BossReadyGap): a member under it drinks before the pull, walking or not
        float const bossLine = PullReadiness::MemberBossManaLine(bot, boss);
        float const manaLine = std::max(ownLine, bossLine);
        bool const underBossLine = maxMana && mana < bossLine;
        if ((canRest || underBossLine) && (RestState::WantsDrink(bot) || underBossLine) &&
            Leader::Dungeon::CountRestItems(bot, 59) > 0 && maxMana && mana < manaLine)
        {
            float const perSec = ManaPerSec(bot);
            if (!RestState::IsDrinking(bot) || perSec <= 0.0f)
            {
                why = bot->GetName() + " must drink and does not yet";
                return -1.0f;
            }
            note((manaLine - mana) * maxMana / 100.0f / perSec, bot->GetName() + " drinks to its line");
        }
        float const healthLine = RestState::HealthLine(bot);
        if (canRest && RestState::WantsFood(bot) && Leader::Dungeon::CountRestItems(bot, 11) > 0 &&
            bot->GetHealthPct() < healthLine)
        {
            float const perSec = FoodHealthPerSec(bot);
            if (!RestState::IsEating(bot) || perSec <= 0.0f)
            {
                why = bot->GetName() + " must eat and does not yet";
                return -1.0f;
            }
            note((healthLine - bot->GetHealthPct()) * bot->GetMaxHealth() / 100.0f / perSec,
                 bot->GetName() + " eats to its line");
        }
    }
    return left;
}

std::vector<Player*> GroupPlayers(LeaderGroup& group, Map* map)
{
    std::vector<Player*> players;
    for (ObjectGuid const& guid : group.MemberGuids())
        if (Player* bot = ObjectAccessor::FindPlayer(guid); bot && bot->GetMap() == map)
            players.push_back(bot);
    return players;
}

float Sec(uint32 fromMs, uint32 toMs) { return int32(getMSTimeDiff(fromMs, toMs)) / 1000.0f; }

// [lead803] b221 rfc-h: lead750's tank Army never fired - 5G's blood tank Rtdekac was never told one (the dps knight
// Rtdekad was, 202 s before Oggleflint), 5C's Rtdekaa only before Taragaman, and no refusal was logged: ArmyBlocked's
// reason (in combat, walking, on cooldown, the runes, "its ghouls land in <mob>'s aggro reach") went to a silent
// `continue` in both gates. Every reason a knight is not told the prepull Army is logged once per boss and reason
void NoteArmyBlocked(LeaderGroup& group, DungeonLeaderState::ArmyPrepull& plan, Player* dk, Unit* boss,
                     std::string const& why, char const* gate)
{
    // [lead823] one line per reason and mob: the numbers in brackets (distance, reach) move with a wandering mob
    std::string const key = why.substr(0, why.find(" ("));
    if (why.empty() || plan.blockedLogged == key)
        return;
    plan.blockedLogged = key;
    LOG_INFO("playerbots", "[lead803] {}: no prepull Army of the Dead before {} ({}): {}", dk->GetName(),
             boss->GetName(), gate, why);
    group.LogEvent("army_prepull_blocked", JsonObject()
                                               .Str("dk", dk->GetName())
                                               .Str("boss", boss->GetName())
                                               .Bool("tank", PlayerbotAI::IsTank(dk))
                                               .Str("gate", gate)
                                               .Str("why", why));
}
}  // namespace

void TickArmy(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, bool fighting,
              std::function<void(Player*, char const*, JsonObject)> const& logEvent)
{
    auto& plans = group.State().armyPrepull;
    uint32 const now = getMSTime() ? getMSTime() : 1;
    Map* map = tank->GetMap();
    Creature* boss = fighting ? nullptr : PreparedBoss(group, map);
    // an order given stands for its boss while that one is still unpulled (the route may look elsewhere)
    for (auto const& [dkGuid, told] : plans)
        if (!boss && !fighting && told.orderedMs && !told.done && !told.startedMs)
            if (Creature* toldBoss = map->GetCreature(told.boss);
                toldBoss && toldBoss->IsAlive() && !toldBoss->IsInCombat())
                boss = toldBoss;
    std::vector<Player*> members = seats;
    members.push_back(tank);

    // the channel's course: started, ended (the added delay: its end after the preparation's)
    for (Player* dk : members)
    {
        auto found = plans.find(dk->GetGUID());
        if (found == plans.end() || found->second.done || !found->second.orderedMs)
            continue;
        DungeonLeaderState::ArmyPrepull& plan = found->second;
        Spell* channel = ArmyChannel(dk);
        bool const channeling = channel != nullptr;
        if (channel)
        {
            plan.channelLeftMs = channel->GetCastTimeRemaining();
            plan.channelSeenMs = now;
        }
        if (channeling && !plan.startedMs)
        {
            plan.startedMs = now;
            if (Creature* armyBoss = map->GetCreature(plan.boss); armyBoss && PlayerbotAI::IsTank(dk))
                SetLanding(armyBoss, {});  // [lead823] the ghouls are down: nothing more to clear for them
            group.LogEvent("army_prepull_started", JsonObject()
                                                       .Str("dk", dk->GetName())
                                                       .Real("afterOrderSec", Sec(plan.orderedMs, now))
                                                       .Real("channelSec", ChannelSec()));
        }
        else if (!channeling && plan.startedMs)
        {
            plan.done = true;
            // [lead272] interrupted: the channel had more left when last seen than has passed since (the cooldown
            // ran from its start: no second channel)
            bool const interrupted = plan.channelLeftMs > int32(getMSTimeDiff(plan.channelSeenMs, now));
            group.LogEvent("army_prepull_end",
                           JsonObject()
                               .Str("dk", dk->GetName())
                               .Bool("interrupted", interrupted)
                               .Real("channelLeftSec", interrupted ? plan.channelLeftMs / 1000.0f : 0.0f)
                               .Real("channelSec", Sec(plan.startedMs, now))
                               .Real("predictedPrepEndSec", Sec(plan.orderedMs, plan.predictedEndMs))
                               .Real("prepEndSec", plan.prepEndMs ? Sec(plan.orderedMs, plan.prepEndMs) : -1.0f)
                               .Real("addedDelaySec", plan.prepEndMs ? Sec(plan.prepEndMs, now) : 0.0f)
                               .Bool("prepStillRuns", !plan.prepEndMs));
        }
        else if (!channeling && !boss)
        {
            plan.done = true;
            group.LogEvent("army_prepull_dropped",
                           JsonObject()
                               .Str("dk", dk->GetName())
                               .Str("why", fighting ? "the fight started before the channel" : "no boss pull next"));
        }
    }
    if (!boss)
        return;

    float const channelSec = ChannelSec();
    std::vector<Player*> const players = GroupPlayers(group, map);
    for (Player* dk : members)
    {
        if (!ArmyCaster(dk))
            continue;
        DungeonLeaderState::ArmyPrepull& plan = plans[dk->GetGUID()];
        if (plan.boss != boss->GetGUID())
            plan = DungeonLeaderState::ArmyPrepull{boss->GetGUID()};
        // [lead910] the step's channel spot is kept while the channel runs; after it the knight walks back (until the
        // pull order takes it)
        if (plan.stepTo && plan.startedMs && !plan.done)
            Leader::Events::SetMove(dk->GetGUID(), Leader::MoveEvent::MoveTo, *plan.stepTo, STEP_ARRIVE_YD,
                                    "army: steps out of the boss's reach to channel");
        if (plan.stepTo && plan.done && !plan.stepBack)
        {
            if (dk->GetExactDist(plan.stepFrom) <= 1.5f || Leader::Orders::IsOpen(dk->GetGUID()))
                plan.stepBack = true;
            else
                Leader::Events::SetMove(dk->GetGUID(), Leader::MoveEvent::MoveTo, plan.stepFrom, 1.5f,
                                        "army: back to its spot after the channel");
        }
        if (plan.done || plan.startedMs)
            continue;
        // [lead823] the group stands in the boss's preparation (PrepLeftSec's own test): the knight's spot is his
        GroupRouteState const& route = group.Route();
        bool const atPrep = route.needBuff && route.buffStep && route.target == boss->GetGUID();
        std::string const blocked = ArmyBlocked(dk, boss, atPrep);
        // [lead910] the boss's own reach blocks the tank knight's landing: it steps out and channels there
        bool const stepping = StepOut(group, plan, dk, boss, members, blocked);
        if (!plan.orderedMs)
        {
            if (!blocked.empty())
            {
                if (!stepping)
                    NoteArmyBlocked(group, plan, dk, boss, blocked, "the buff pass");  // [lead803]
                continue;
            }
            std::string why;
            float const left = PrepLeftSec(group, boss, players, why);
            if (left < 0.0f || left > channelSec)
                continue;
            plan.orderedMs = now;
            plan.orderedRemainSec = left;
            plan.predictedEndMs = now + uint32(left * IN_MILLISECONDS);
            group.LogEvent("army_prepull_ordered", JsonObject()
                                                       .Str("boss", boss->GetName())
                                                       .Str("dk", dk->GetName())
                                                       .Real("prepLeftSec", left)
                                                       .Str("prepLeftWhy", why)
                                                       .Real("channelSec", channelSec)
                                                       .Real("plannedDelaySec", channelSec - left));
        }
        else if (!blocked.empty() && blocked != "walking")
        {
            // [lead823] trash in its landing is the boss's to clear first: the order is taken back and given anew
            // once the knight can channel (the trash dead or out of the landing), not dropped for this boss
            if (PlayerbotAI::IsTank(dk) && LandingHasTrash(boss))
            {
                if (Leader::MemberEvents events;
                    Leader::Events::Get(dk->GetGUID(), events) && events.externalSpell == SPELL_ARMY_OF_THE_DEAD)
                    Leader::Events::SetExternal(dk->GetGUID(), 0, ObjectGuid::Empty);
                group.LogEvent("army_prepull_paused", JsonObject().Str("dk", dk->GetName()).Str("why", blocked));
                std::string const logged = plan.blockedLogged;
                plan = DungeonLeaderState::ArmyPrepull{boss->GetGUID()};
                plan.blockedLogged = logged;
                continue;
            }
            plan.done = true;
            group.LogEvent("army_prepull_dropped", JsonObject().Str("dk", dk->GetName()).Str("why", blocked));
            continue;
        }
        // [lead272] the DK reported it cannot cast it (its own cast check or the cast): dropped, nothing waits on it
        if (std::string failed; Leader::Events::TakeExternalFailed(dk->GetGUID(), SPELL_ARMY_OF_THE_DEAD, failed))
        {
            plan.done = true;
            Leader::Events::SetExternal(dk->GetGUID(), 0, ObjectGuid::Empty);
            group.LogEvent("army_prepull_dropped",
                           JsonObject().Str("dk", dk->GetName()).Str("why", "the DK reports: " + failed));
            continue;
        }
        // [lead272] one command per fact: the order is off while the DK walks, and given again once it stands
        if (blocked == "walking")
        {
            if (!plan.paused)
            {
                plan.paused = true;
                group.LogEvent("army_prepull_paused", JsonObject().Str("dk", dk->GetName()).Str("why", "it walks"));
            }
            continue;
        }
        if (plan.paused)
        {
            plan.paused = false;
            group.LogEvent("army_prepull_reissued", JsonObject().Str("dk", dk->GetName()).Str("why", "it stands"));
        }
        // the order stands until the channel runs: the event channel keeps a command only while each leader tick
        // asserts it (lead27), and an unchanged assert is no new command (no seq, no log); a buff order of the
        // same tick keeps the member's one external slot
        Leader::MemberEvents events;
        if (Leader::Events::Get(dk->GetGUID(), events) && events.externalSpell &&
            events.externalSpell != SPELL_ARMY_OF_THE_DEAD)
            continue;
        if (Leader::Events::SetExternal(dk->GetGUID(), SPELL_ARMY_OF_THE_DEAD, dk->GetGUID()))
            logEvent(dk, "External",
                     JsonObject()
                         .Num("spell", SPELL_ARMY_OF_THE_DEAD)
                         .Str("on", dk->GetName())
                         .Str("why", "Army of the Dead before the boss pull"));
    }
}

std::string ArmyHolds(LeaderGroup& group, Unit* boss, bool orderNow)
{
    // [lead520] b211 rfc-h 5G 347 s: "army_prepull_ordered ... before Oggleflint" 6 s after he died (the late buff
    // pass's end): a boss dead or already fought has no prepull
    if (!boss || !boss->IsAlive() || boss->IsInCombat())
        return std::string();
    // [lead880] only a boss has a prepull Army: b224 rfc-h 5G 931 s the buff pass's end ordered it "before Searing
    // Blade Cultist" (the route target was a committed trash pull) and Bazzalan met it on cooldown
    if (Creature* creature = boss->ToCreature();
        !creature ||
        !(creature->IsDungeonBoss() || creature->isWorldBoss() || group.BossCreatures().count(creature->GetGUID())))
        return std::string();
    auto& plans = group.State().armyPrepull;
    uint32 const now = getMSTime() ? getMSTime() : 1;
    std::string holds;
    for (Player* dk : GroupPlayers(group, boss->GetMap()))
    {
        if (!ArmyCaster(dk))
            continue;
        DungeonLeaderState::ArmyPrepull& plan = plans[dk->GetGUID()];
        if (plan.boss != boss->GetGUID())
            plan = DungeonLeaderState::ArmyPrepull{boss->GetGUID()};
        if (plan.done)
            continue;
        Spell* channel = ArmyChannel(dk);
        if (!plan.orderedMs)
        {
            // the preparation ended before the leader could time the channel into it: the pull waits it whole
            // [lead520] b211 rfc-h 5C Oggleflint / Taragaman: the knight was in combat / walking during the one-second
            // buff pass end and was never told, then stood idle 14 s before the pull. Its pull orders it too, once
            // the knight can channel (wowsims prepulls Army whenever it is ready; one on cooldown is not waited for)
            // [lead823] both gates order the channel where the knight stands: its landing is the boss fight's
            // [lead1240] the buff pass's end does not order it while a member is under its boss mana line with a
            // drink in its bags: the boss pull waits for that drink, the Army's ghouls would fight alone before he is
            // pulled; TickArmy times it into the drink, else the pull's own gate orders it
            if (std::string const shortOf =
                    orderNow ? UnderBossManaLine(GroupPlayers(group, boss->GetMap()), boss) : std::string();
                !shortOf.empty())
            {
                if (plan.waitLogged != shortOf)
                {
                    plan.waitLogged = shortOf;
                    group.LogEvent("army_prepull_waits", JsonObject()
                                                             .Str("dk", dk->GetName())
                                                             .Str("boss", boss->GetName())
                                                             .Str("gate", "the buff pass end")
                                                             .Str("why", shortOf)
                                                             .Str("tag", "[lead1240]"));
                }
                continue;
            }
            if (std::string const blocked = ArmyBlocked(dk, boss->ToCreature(), true); !blocked.empty())
            {
                char const* const gate = orderNow ? "the buff pass end" : "the boss pull";
                // [lead910] the tank knight steps out of the boss's reach to channel: the gate waits for it
                if (boss->ToCreature() &&
                    StepOut(group, plan, dk, boss->ToCreature(), GroupPlayers(group, boss->GetMap()), blocked))
                {
                    holds = Acore::StringFormat("Army of the Dead: {} steps out of {}'s reach", dk->GetName(),
                                                boss->GetName());
                    continue;
                }
                NoteArmyBlocked(group, plan, dk, boss, blocked, gate);
                // [lead881] b224 rfc-h 5C Bazzalan: the tank knight's runes were not ready at -7.9 s, it walked at
                // -6.9 s, and the pull gate let the pull go past it without asking again. A reason that passes by
                // itself holds the pass's end / the pull; the next look orders the channel
                if (ArmyBlockPasses(blocked))
                {
                    if (plan.waitLogged != blocked)
                    {
                        plan.waitLogged = blocked;
                        LOG_INFO("playerbots", "[lead881] {}: {} waits for its prepull Army before {}: {} ({:.1f} s "
                                 "until every rune is back)",
                                 dk->GetName(), gate, boss->GetName(), blocked, RuneWaitSec(dk));
                        group.LogEvent("army_prepull_waits", JsonObject()
                                                                 .Str("dk", dk->GetName())
                                                                 .Str("boss", boss->GetName())
                                                                 .Str("gate", gate)
                                                                 .Str("why", blocked)
                                                                 .Real("runeWaitSec", RuneWaitSec(dk)));
                    }
                    holds = Acore::StringFormat("Army of the Dead: {} {}", dk->GetName(),
                                                blocked == "walking" ? "walks to its spot" : "waits for its runes");
                }
                continue;
            }
            plan.orderedMs = now;
            plan.predictedEndMs = now;
            group.LogEvent("army_prepull_ordered", JsonObject()
                                                       .Str("boss", boss->GetName())
                                                       .Str("dk", dk->GetName())
                                                       .Real("prepLeftSec", 0.0f)
                                                       .Str("prepLeftWhy", orderNow ? "the preparation is over"
                                                                                    : "the boss pull")
                                                       .Real("channelSec", ChannelSec())
                                                       .Real("plannedDelaySec", ChannelSec()));
        }
        // told, cannot start now (TickArmy drops it on a lasting reason): nothing to wait for
        else if (std::string const blocked =
                     channel || plan.startedMs ? std::string() : ArmyBlocked(dk, boss->ToCreature(), true);
                 !blocked.empty())
        {
            // [lead881] told and walking / its runes not back: TickArmy gives the order again once it stands
            if (ArmyBlockPasses(blocked))
                holds = Acore::StringFormat("Army of the Dead: {} {}", dk->GetName(),
                                            blocked == "walking" ? "walks to its spot" : "waits for its runes");
            continue;
        }
        if (!plan.prepEndMs)
        {
            plan.prepEndMs = now;
            group.LogEvent(
                "army_prepull_prep_end",
                JsonObject()
                    .Str("dk", dk->GetName())
                    .Str("at", orderNow ? "the buff pass end" : "the pull")
                    .Real("orderedBeforeSec", Sec(plan.orderedMs, now))
                    .Real("predictedPrepEndSec", Sec(plan.orderedMs, plan.predictedEndMs))
                    .Real("channelLeftSec", channel ? channel->GetCastTimeRemaining() / 1000.0f : ChannelSec()));
        }
        if (channel || !plan.startedMs)
            holds = Acore::StringFormat(
                "Army of the Dead: {} {}", dk->GetName(),
                channel ? Acore::StringFormat("channels, {:.1f} s left", channel->GetCastTimeRemaining() / 1000.0f)
                        : std::string("starts the channel"));
    }
    // [lead1164] b240 rfc-h 5C-11: the blood tank's Army was "on cooldown" at Oggleflint (its 10 min from 5C-3's
    // Bazzalan, 3.5 min before the run) and "held for Bazzalan" in 5C-3's Jergosh fight: what each gate saw, once per
    // knight, boss, gate and verdict - the plan's state, the cooldown left and what the gate holds
    for (Player* dk : GroupPlayers(group, boss->GetMap()))
    {
        if (!ArmyCaster(dk))
            continue;
        DungeonLeaderState::ArmyPrepull const& plan = plans[dk->GetGUID()];
        std::string const state = Acore::StringFormat(
            "{}: plan {}, cooldown {:.0f} s, last blocked '{}', last wait '{}', holds '{}'",
            orderNow ? "the buff pass end" : "the boss pull",
            plan.done ? "done" : plan.startedMs ? "channel started" : plan.orderedMs ? "ordered" : "not ordered",
            dk->GetSpellCooldownDelay(SPELL_ARMY_OF_THE_DEAD) / 1000.0f, plan.blockedLogged, plan.waitLogged, holds);
        thread_local std::map<std::pair<uint64, uint64>, std::string> logged;
        if (logged.size() > 1024)
            logged.clear();
        std::string& was = logged[{dk->GetGUID().GetRawValue(), boss->GetGUID().GetRawValue()}];
        if (was != state)
        {
            was = state;
            LOG_INFO("playerbots", "[lead1164] {}: Army before {} at {}", dk->GetName(), boss->GetName(), state);
        }
    }
    return holds;
}

bool ArmyLandingTrash(Creature* boss, Creature* mob)
{
    if (!boss || !mob)
        return false;
    std::lock_guard<std::mutex> lock(landingLock);
    auto const found = landingTrash.find(InstanceKey(boss));
    return found != landingTrash.end() && found->second.count(mob->GetGUID());
}

bool OverkillStealther(Player* bot)
{
    constexpr uint32 SPELL_OVERKILL_TALENT = 58426;
    return bot && bot->getClass() == CLASS_ROGUE && bot->HasTalent(SPELL_OVERKILL_TALENT, bot->GetActiveSpec()) &&
           bot->HasSpell(SPELL_STEALTH);
}

std::string StealthHolds(LeaderGroup& group, Unit* boss)
{
    // [lead773] b220 rfc-h 5E Taragaman: "pre-pull Stealth for Overkill: the boss pull opens" at 703.0 s, the same
    // tick as the pull order and the rogue's pre-pull potion; the rogue took the potion first (member_ack 704.4 s),
    // the boss fight was on at 704.6 s and Stealth (no cast in combat) was never cast - its External gave way to
    // Tricks of the Trade at 704.9 s; Oggleflint's pull had 2.6 s and got it (ack 276.1 s). The rogue goes into
    // Stealth before the pull is issued, like the prepull Army's channel
    if (!boss || !boss->IsAlive() || boss->IsInCombat())
        return std::string();
    auto& plans = group.State().stealthPrepull;
    std::string holds;
    for (Player* rogue : GroupPlayers(group, boss->GetMap()))
    {
        if (!OverkillStealther(rogue))
            continue;
        DungeonLeaderState::StealthPrepull& plan = plans[rogue->GetGUID()];
        if (plan.boss != boss->GetGUID())
            plan = DungeonLeaderState::StealthPrepull{boss->GetGUID()};
        if (plan.done)
            continue;
        if (rogue->HasAura(SPELL_STEALTH) || !rogue->IsAlive() || rogue->IsInCombat() ||
            rogue->HasSpellCooldown(SPELL_STEALTH))
        {
            plan.done = rogue->HasAura(SPELL_STEALTH) || rogue->IsInCombat();
            continue;
        }
        if (!plan.ordered)
        {
            plan.ordered = true;
            group.LogEvent("stealth_prepull_ordered",
                           JsonObject().Str("boss", boss->GetName()).Str("rogue", rogue->GetName()));
        }
        holds = Acore::StringFormat("Stealth for Overkill: {} stealths", rogue->GetName());
    }
    return holds;
}
}  // namespace Leader::Prepull
