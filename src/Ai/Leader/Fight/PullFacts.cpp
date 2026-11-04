/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "PullFacts.h"

#include "AttackerCountValues.h"
#include "Creature.h"
#include "Group.h"
#include "CreatureGroups.h"
#include "GenericSpellActions.h"
#include "LeaderActions.h"
#include "LeaderKnowledge.h"
#include "Player.h"
#include "Playerbots.h"
#include "LeaderOrders.h"  // [lead108] Leader::Route
#include "MobRoles.h"      // [lead1041] HoldsRange
#include "RouteSteer.h"  // [lead167] BossBroughtBy
#include "ChooseTargetActions.h"  // [lead760] FightSpot
#include "LeaderFightChecks.h"   // [lead760] TANK_TAUNTS
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "HealthTriggers.h"  // [lead925] RestState::HealerThroughput
#include "Log.h"             // [lead925]
#include "StringFormat.h"    // [lead925]
#include <algorithm>
#include <mutex>          // [lead925]
#include <set>            // [lead925]
#include <unordered_map>  // [lead925]

namespace Leader::Fight
{
    Unit* WalkInWakes(Player* tank, Unit* target, std::vector<ObjectGuid> const& planMobs)
    {
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        Creature* creature = target ? target->ToCreature() : nullptr;
        if (!tankAI || !creature)
            return nullptr;
        auto* mover =
            dynamic_cast<LeaderMoveOrderAction*>(tankAI->GetAiObjectContext()->GetAction("leader move order"));
        if (!mover)
            return nullptr;
        // [lead167] b179 rfc-h 5G 264-330 s: the walk to Oggleflint "woke" Ragefire Trogg 54, which joins his fight by
        // script - the planner refuses that pack as "brings a boss, pulled as his fight", the walk-in check released
        // the boss for it as "the pull first": 74 releases. A mob the boss brings into his fight anyway is his pull
        bool const boss = creature->IsDungeonBoss() || creature->isWorldBoss();
        auto const counts = [&](Unit* woken)
        {
            return woken && woken->ToCreature() &&
                   std::find(planMobs.begin(), planMobs.end(), woken->GetGUID()) == planMobs.end() &&
                   !(creature->GetFormation() && woken->ToCreature()->GetFormation() == creature->GetFormation()) &&
                   !(boss && Leader::Travel::BossBroughtBy(tank, creature, woken->ToCreature()));
        };
        if (Unit* woken = mover->WalkWakes(target->GetPosition(), target->GetGUID()); counts(woken))
            return woken;
        // [lead113] b147 rfc-h 59.9 s: the healer following the tank 12 yd behind woke Earthborer 4 (45 yd from the
        // fight spot) - the followers walk in too: each member's own walk to the target (its route search against
        // every idle pack's reach, its own aggro check) is judged the same way
        if (Group* group = tank->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                PlayerbotAI* memberAI = member && member != tank ? GET_PLAYERBOT_AI(member) : nullptr;
                if (!memberAI || !member->IsAlive() || member->GetMap() != tank->GetMap())
                    continue;
                if (auto* follower = dynamic_cast<LeaderMoveOrderAction*>(
                        memberAI->GetAiObjectContext()->GetAction("leader move order")))
                    if (Unit* woken = follower->WalkWakes(target->GetPosition(), target->GetGUID()); counts(woken))
                        return woken;
            }
        return nullptr;
    }

    bool OrderMobStaysPick(Player* tank, Creature* own)
    {
        return own && own->IsAlive() && !own->IsInCombat() && TankHasWalk(tank, own);
    }

    bool TankHasWalk(Player* tank, Unit* mob)
    {
        if (!tank || !mob)
            return false;
        Leader::RouteResult const path = Leader::Route(tank, mob->GetPosition());
        if (!path.found)
            return false;
        if (path.complete || path.points.empty())
            return path.complete;
        float const straight = tank->GetExactDist(mob);
        float const left =
            (path.points.back() - G3D::Vector3(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ())).length();
        return left + 5.0f < straight;
    }

    bool NeedsFleeStop(Unit* mob)
    {
        if (!mob || !mob->IsAlive() || !mob->ToCreature() || mob->HasAuraType(SPELL_AURA_MOD_STUN) ||
            mob->HasAuraType(SPELL_AURA_MOD_ROOT) || mob->HasBreakableByDamageCrowdControlAura() ||
            FledMobs::StoppedRecently(mob))
            return false;
        float const fleeAt = Leader::Knowledge::FleeForAssistHealthPct(mob->GetEntry());
        return (fleeAt > 0.0f && mob->GetHealthPct() <= fleeAt) || FledMobs::IsRunning(mob);
    }

    bool IsFleer(Unit* mob)
    {
        if (!mob || !mob->IsAlive() || !mob->ToCreature())
            return false;
        float const fleeAt = Leader::Knowledge::FleeForAssistHealthPct(mob->GetEntry());
        return (fleeAt > 0.0f && mob->GetHealthPct() <= fleeAt) || FledMobs::IsRunning(mob);
    }

    Player* FleeStopper(std::vector<Player*> const& members, Unit* mob)
    {
        for (Player* member : members)
        {
            PlayerbotAI* memberAI = member ? GET_PLAYERBOT_AI(member) : nullptr;
            if (!memberAI || !member->IsAlive() || PlayerbotAI::IsHeal(member))
                continue;
            if (auto* stop = dynamic_cast<FleeStopAction*>(memberAI->GetAiObjectContext()->GetAction("flee stop"));
                stop && stop->CanStop(mob))
                return member;
        }
        return nullptr;
    }

    bool MobComesToTank(Player* tank, Unit* mob)
    {
        if (!tank || !mob || !mob->IsAlive() || !mob->IsInCombat() || mob->GetVictim() != tank ||
            mob->HasUnitState(UNIT_STATE_ROOT) || tank->IsWithinMeleeRange(mob))
            return false;
        Creature* creature = mob->ToCreature();
        if (creature && creature->CanNotReachTarget())
            return false;
        if (mob->isMoving())
            return true;
        // [lead1041] b230 rfc-h (16 runs): the tank's "reach melee" walked 26 s after a mob it had just taunted and
        // 73 s toward a mob already on it ("same mob, coming at the tank") - on the first ticks after the victim switch
        // the mob has not started its chase yet (isMoving false). A mob whose victim is the tank chases it by the
        // game's combat movement: one that may move in combat (Creature::IsCombatMovementAllowed), is not held (stun,
        // fear, confuse, its own cast) and does not stand to cast from range (MobRoles::HoldsRange) comes to the tank
        return creature && creature->IsCombatMovementAllowed() && !MobRoles::HoldsRange(mob) &&
               !mob->HasUnitState(UNIT_STATE_CONTROLLED | UNIT_STATE_CASTING);
    }

    uint32 TauntReachesFromHere(Player* tank, Unit* mob)
    {
        if (!tank || !mob || !mob->IsAlive())
            return 0;
        constexpr uint32 SPELL_RIGHTEOUS_DEFENSE = 31789;
        for (uint32 spell : TANK_TAUNTS)
        {
            if (!tank->HasSpell(spell) || tank->HasSpellCooldown(spell))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            // Righteous Defense goes on the attacked friend, the others on the mob
            Unit* on = spell == SPELL_RIGHTEOUS_DEFENSE ? mob->GetVictim() : mob;
            if (info && on && on != tank && tank->IsWithinCombatRange(on, info->GetMaxRange(on->IsFriendlyTo(tank))) &&
                tank->IsWithinLOSInMap(on))
                return spell;
        }
        return 0;
    }

    std::string TankWalkMovesFight(Player* tank, Unit* target, bool gapCloser)
    {
        if (!tank || !target || !target->IsAlive() || !Leader::LedBot(tank) || tank->IsWithinMeleeRange(target))
            return std::string();
        Creature* creature = target->ToCreature();
        // a mob that cannot reach its victim evades every hit: only a walk to it ends that (b39_hos)
        if (!creature || creature->CanNotReachTarget())
            return std::string();
        // [lead760] b220 rfc-h: 48 pulls walked back by "move fight spot back" (519 yd) after the tank met its mob
        // forward first (reach melee 190 yd, tank open incoming 108, charges 33, aggressive target 26 before the
        // walk back): an engage with a fight spot back is fought there - the tank goes there and the mob comes
        Position back;
        if (FightSpot::Ahead(tank, back) &&
            target->GetExactDist(back) > tank->GetCombatReach() + target->GetCombatReach() + NOMINAL_MELEE_RANGE)
            return "the plan's fight spot back is not where the mob is";
        if (!target->IsInCombat())
            return std::string();
        if (!gapCloser && MobComesToTank(tank, target))  // [lead925]
            return "it comes to the tank";
        Unit* victim = target->GetVictim();
        if (victim && victim != tank && victim->GetCharmerOrOwnerPlayerOrPlayerItself() &&
            TauntReachesFromHere(tank, target))
            return "a ready taunt reaches it from here";
        // [lead760] b220: "reach melee" to a mob on the tank standing at range (a caster) 75 s - the walk drags the
        // mobs the tank holds; while the tank holds another fight mob in its melee it stays, the ranged hit the caster
        if (victim == tank && !target->isMoving())
            for (Unit* attacker : tank->getAttackers())
                if (attacker && attacker != target && attacker->IsAlive() && attacker->GetVictim() == tank &&
                    tank->IsWithinMeleeRange(attacker))
                    return "a caster at range while the tank holds " + attacker->GetName() + " in its melee";
        return std::string();
    }

    namespace
    {
        // [lead925] the tank's defensives a gap closer's landing may wait for: up, or ready for the leader to tell
        constexpr uint32 SPELL_SURVIVAL_INSTINCTS = 61336;
        constexpr uint32 SPELL_BARKSKIN = 22812;
        constexpr uint32 SPELL_SHIELD_WALL = 871;
        constexpr uint32 SPELL_LAST_STAND = 12975;
        constexpr uint32 SPELL_LAST_STAND_AURA = 12976;

        std::mutex gapSaveLock;
        std::unordered_map<uint64, ObjectGuid> gapSaveAsked;  // [lead925] under gapSaveLock: tank -> target
    }

    std::string TankGapCloserRisk(Player* tank, Unit* target, uint32* save)
    {
        if (save)
            *save = 0;
        if (!tank || !target || !target->IsAlive() || !Leader::LedBot(tank) || tank->IsWithinMeleeRange(target))
            return std::string();
        std::vector<uint32> defensives;
        if (tank->getClass() == CLASS_DRUID)
            defensives = {SPELL_SURVIVAL_INSTINCTS, SPELL_BARKSKIN};
        else if (tank->getClass() == CLASS_WARRIOR)
            defensives = {SPELL_SHIELD_WALL, SPELL_LAST_STAND};
        for (uint32 spell : defensives)
            if (GapCloserSaveUp(tank, spell))
                return std::string();
        Group* group = tank->GetGroup();
        if (!group)
            return std::string();
        Position const landing = target->GetPosition();
        Player* nearest = nullptr;
        float nearestSec = -1.0f;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* healer = ref->GetSource();
            if (!healer || healer == tank || !healer->IsAlive() || healer->GetMap() != tank->GetMap() ||
                !PlayerbotAI::IsHeal(healer))
                continue;
            if (HealReaches(tank, healer->GetPosition(), landing, healer))
                return std::string();
            // its walk into heal reach of the landing: the walk less the heal's reach, the whole walk where the
            // landing is in range but out of its sight (it walks round the corner)
            float const walk = Leader::WalkYd(healer, landing);
            float const reach = Leader::LEADER_HEAL_REACH_YD + 2.0f * tank->GetCombatReach();
            float const into = healer->GetExactDist(landing) > reach ? std::max(walk - reach, 0.0f) : walk;
            float const speed = std::max(healer->GetSpeed(MOVE_RUN), 0.1f);
            float const sec = into / speed + RestState::HealerThroughput(healer, tank).gapSec;
            if (nearestSec < 0.0f || sec < nearestSec)
            {
                nearest = healer;
                nearestSec = sec;
            }
        }
        if (!nearest)
            return std::string();  // no healer to wait for: no wait changes the landing
        float dps = 0.0f;
        float spellPart = 0.0f;
        bool counted = false;
        for (Unit* attacker : tank->getAttackers())
            if (attacker && attacker->IsAlive())
            {
                dps += AttackerDps(attacker, tank, spellPart);
                counted |= attacker == target;
            }
        if (!counted)
            dps += AttackerDps(target, tank, spellPart);
        float const health = float(tank->GetHealth());
        if (dps <= 0.0f || health > dps * nearestSec)
            return std::string();
        for (uint32 spell : defensives)
            if (save && !*save && tank->HasSpell(spell) && !tank->HasSpellCooldown(spell))
                *save = spell;
        return Acore::StringFormat("no heal reaches its landing at {}: {} needs {:.1f} s to heal it there, it takes "
                                   "{:.0f}/s against {:.0f} health",
                                   target->GetName(), nearest->GetName(), nearestSec, dps, health);
    }

    bool IsGapCloserSave(uint32 spell)
    {
        return spell && (spell == SPELL_SURVIVAL_INSTINCTS || spell == SPELL_BARKSKIN || spell == SPELL_SHIELD_WALL ||
                         spell == SPELL_LAST_STAND);
    }

    bool GapCloserSaveUp(Player* tank, uint32 spell)
    {
        return tank->HasAura(spell) || (spell == SPELL_LAST_STAND && tank->HasAura(SPELL_LAST_STAND_AURA));
    }

    void AskGapCloserSave(Player* tank, Unit* target)
    {
        std::lock_guard<std::mutex> lock(gapSaveLock);
        gapSaveAsked[tank->GetGUID().GetRawValue()] = target->GetGUID();
    }

    bool GapCloserSaveAsked(Player* tank, ObjectGuid& target)
    {
        std::lock_guard<std::mutex> lock(gapSaveLock);
        auto const found = gapSaveAsked.find(tank->GetGUID().GetRawValue());
        if (found == gapSaveAsked.end())
            return false;
        target = found->second;
        return true;
    }

    void ClearGapCloserSave(Player* tank)
    {
        std::lock_guard<std::mutex> lock(gapSaveLock);
        gapSaveAsked.erase(tank->GetGUID().GetRawValue());
    }

    bool TankGapCloserAllowed(Player* tank, Unit* target, char const* what)
    {
        uint32 save = 0;
        std::string const risk = TankGapCloserRisk(tank, target, &save);
        if (risk.empty())
            return true;
        if (save)
            AskGapCloserSave(tank, target);
        thread_local std::set<std::pair<uint64, uint64>> logged;
        if (logged.insert({tank->GetGUID().GetRawValue(), target->GetGUID().GetRawValue()}).second)
            LOG_INFO("playerbots", "[lead925] {}: no {} on {} ({:.1f} yd) - {}{}", tank->GetName(), what,
                     target->GetName(), tank->GetExactDist(target), risk,
                     save ? Acore::StringFormat("; its save {} asked for first", save)
                          : std::string("; no save ready"));
        return false;
    }
}
