/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AttackersValue.h"

#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "InstanceGuid.h"  // [rot44]
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "ServerFacade.h"
#include "Timer.h"

GuidVector AttackersValue::Calculate()
{
    std::unordered_set<Unit*> targets;

    GuidVector result;
    if (!botAI->AllowActivity(ALL_ACTIVITY))
        return result;

    AddAttackersOf(bot, targets);

    if (Group* group = bot->GetGroup())
        AddAttackersOf(group, targets);

    RemoveNonThreating(targets);
    AddMeleeAttackersOfGroup(targets);
    AddCombatOnlyRefs(targets);  // [b30_stalls]

    // prioritized target
    GuidVector prioritizedTargets = AI_VALUE(GuidVector, "prioritized targets");
    for (ObjectGuid target : prioritizedTargets)
    {
        Unit* unit = botAI->GetUnit(target);
        if (unit && IsValidTarget(unit, bot))
            targets.insert(unit);
    }
    if (Group* group = bot->GetGroup())
    {
        ObjectGuid skullGuid = group->GetTargetIcon(7);
        Unit* skullTarget = botAI->GetUnit(skullGuid);
        if (skullTarget && IsValidTarget(skullTarget, bot))
            targets.insert(skullTarget);
    }

    for (Unit* unit : targets)
        result.push_back(unit->GetGUID());

    if (bot->duel && bot->duel->Opponent)
        result.push_back(bot->duel->Opponent->GetGUID());

    // players are never on threat lists: add valid enemy players in arenas, and within 40 yd in battlegrounds (the
    // distance an idle bot picks enemy players from)
    if (bot->InBattleground())
    {
        GuidVector possibleTargets = AI_VALUE(GuidVector, "possible targets");
        for (ObjectGuid const guid : possibleTargets)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && unit->IsPlayer() && IsValidTarget(unit, bot) &&
                (bot->InArena() || bot->IsWithinDist(unit, 40.0f)))
                result.push_back(unit->GetGUID());
        }
    }

    return result;
}

void AttackersValue::AddAttackersOf(Group* group, std::unordered_set<Unit*>& targets)
{
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot || member->GetMapId() != bot->GetMapId() ||
            ServerFacade::instance().GetDistance2d(bot, member) > sPlayerbotAIConfig.SightDistance)
            continue;

        AddAttackersOf(member, targets);
    }
}

struct AddGuardiansHelper
{
    explicit AddGuardiansHelper(std::vector<Unit*>& units) : units(units) {}

    void operator()(Unit* target) const { units.push_back(target); }

    std::vector<Unit*>& units;
};

void AttackersValue::AddAttackersOf(Player* player, std::unordered_set<Unit*>& targets)
{
    if (!player || !player->IsInWorld() || player->IsBeingTeleported())
        return;

    for (auto const& [guid, ref] : player->GetThreatMgr().GetThreatenedByMeList())
    {
        Unit* attacker = ref->GetOwner();
        if (!attacker)
            continue;

        if (player->IsValidAttackTarget(attacker) &&
            player->GetDistance2d(attacker) < sPlayerbotAIConfig.SightDistance)
            targets.insert(attacker);
    }
}

void AttackersValue::RemoveNonThreating(std::unordered_set<Unit*>& targets)
{
    for (std::unordered_set<Unit*>::iterator tIter = targets.begin(); tIter != targets.end();)
    {
        Unit* unit = *tIter;
        if (bot->GetMapId() != unit->GetMapId() || !hasRealThreat(unit) || !IsValidTarget(unit, bot) ||
            IsIgnoredCritter(unit, bot))
        {
            std::unordered_set<Unit*>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else
            ++tIter;
    }
}

// [attacker-unlisted] ac-h build 25 (20261025-123424-ac-h-5A-87, pull 5): a neutral Phasing Cleric (faction 7), woken
// by the shaman's Magma Totem, meleed the shaman 3 yd from the whole group for 270 s, and no bot listed it: every bot
// stayed on the non-combat engine (no "dps target", no "tank assist", the tank's "possible targets" left it out), the
// run failed "trash fight idle". A creature hitting the bot or a member of its group in melee is fought back whenever
// the core lets this bot attack it (Unit::IsValidAttackTarget: flags, sight, faction, evade) - whatever the module's
// own target filters say. Each such keep is logged with the filter that refused it, so the filter can be fixed at its
// root.
void AttackersValue::AddMeleeAttackersOfGroup(std::unordered_set<Unit*>& targets)
{
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon() || !bot->IsAlive())
        return;

    std::vector<Player*> victims{bot};
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->IsInWorld() && member->GetMap() == map &&
                bot->IsWithinDist(member, sPlayerbotAIConfig.SightDistance))
                victims.push_back(member);
        }
    }

    constexpr uint32 UNLISTED_LOG_MS = 10 * IN_MILLISECONDS;
    static std::mutex logLock;
    static std::unordered_map<uint64, uint32> logged;  // bot ^ mob -> when

    for (Player* victim : victims)
    {
        for (Unit* attacker : victim->getAttackers())
        {
            Creature* mob = attacker ? attacker->ToCreature() : nullptr;
            if (!mob || targets.count(mob) || !mob->IsAlive() || !mob->IsInWorld() || mob->GetMap() != map ||
                mob->GetVictim() != victim || !mob->IsWithinMeleeRange(victim) || mob->IsInEvadeMode() ||
                mob->IsFriendlyTo(bot) || IsIgnoredCritter(mob, bot))
                continue;

            // Valid by the module's own rules but on no member's threat list (AddAttackersOf reads only those: a mob
            // woken by a totem), or refused by one of its filters.
            char const* rejection = PossibleTargetRejection(mob, bot);
            bool const valid = !rejection && IsValidTarget(mob, bot);
            if (!rejection && !valid)
                rejection = "no line of sight";
            bool const attackable = valid || (bot->IsValidAttackTarget(mob) &&
                                              !(mob->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) &&
                                                mob->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC)));
            if (attackable)
                targets.insert(mob);

            uint32 const now = getMSTime();
            {
                std::lock_guard<std::mutex> guard(logLock);
                if (logged.size() > 4096)
                    logged.clear();
                uint32& at = logged[bot->GetGUID().GetRawValue() ^ (mob->GetGUID().GetRawValue() << 1)];
                if (at && getMSTimeDiff(at, now) < UNLISTED_LOG_MS)
                    continue;
                at = now ? now : 1;
            }

            LOG_INFO("playerbots", "[attacker-unlisted] {}: {} {} hits {} in melee ({:.1f} yd from the bot): {} "
                     "(module filter: {})", bot->GetName(), mob->GetName(), mob->GetEntry(), victim->GetName(),
                     bot->GetExactDist(mob), attackable ? "kept as an attacker" : "the core refuses the attack",
                     valid ? "none, on no member's threat list" : rejection);
        }
    }
}

// [b30_stalls] A mob the bot is in combat with (a PvE combat reference) that holds it on no threat list and
// attacks nobody. arc-h / arc-n 2026-10-25 (20261025-184229-arc-h-5F-53, -arc-n-5D-46): after the Eredar
// Soul-Eater died, its Eredar Deathbringer stayed in combat with all five members at 26-32 yd, in sight, 100 %
// health, not evading, no victim; AddAttackersOf reads only threat lists, so every bot stayed on the non-combat
// engine ("non combat, 0 attackers" while IsInCombat) and the run failed "trash fight idle" after 180 s. A player
// attacks what keeps him in combat: such a mob is an attacker (the tank's pull / attack gives it threat).
constexpr float COMBAT_ONLY_REF_RANGE = 40.0f;
constexpr float COMBAT_ONLY_REF_MAX_DZ = 8.0f;

void AttackersValue::AddCombatOnlyRefs(std::unordered_set<Unit*>& targets)
{
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon() || !bot->IsAlive() || !bot->IsInCombat())
        return;

    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
    {
        Unit* other = ref ? ref->GetOther(bot) : nullptr;
        Creature* mob = other ? other->ToCreature() : nullptr;
        if (!mob || targets.count(mob) || !mob->IsAlive() || !mob->IsInWorld() || mob->GetMap() != map ||
            mob->GetVictim() || mob->IsInEvadeMode() || IsIgnoredCritter(mob, bot) ||
            !bot->IsWithinDist(mob, COMBAT_ONLY_REF_RANGE) ||
            std::fabs(mob->GetPositionZ() - bot->GetPositionZ()) > COMBAT_ONLY_REF_MAX_DZ ||
            !IsValidTarget(mob, bot) || !bot->IsValidAttackTarget(mob))
            continue;

        targets.insert(mob);
    }
}

bool AttackersValue::hasRealThreat(Unit* attacker)
{
    return attacker && attacker->IsInWorld() && attacker->IsAlive() && !attacker->IsPolymorphed() &&
           // !attacker->isInRoots() &&
           !attacker->IsFriendlyTo(bot);
}

bool AttackersValue::IsPossibleTarget(Unit* attacker, Player* bot, float /*range*/)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    // Basic check
    if (!attacker)
        return false;

    // bool inCannon = botAI->IsInVehicle(false, true);
    // bool enemy = botAI->GetAiObjectContext()->GetValue<Unit*>("enemy player target")->Get();

    // Validity checks
    if (!attacker->IsVisible() || !attacker->IsInWorld() || attacker->GetMapId() != bot->GetMapId())
        return false;

    if (attacker->isDead() || attacker->HasSpiritOfRedemptionAura())
        return false;

    // [b44_st] a charmed member of the group / team is never attacked
    if (IsCharmedAlly(attacker, bot))
        return false;

    // Flag checks
    if (attacker->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2))
        return false;

    if (attacker->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC) || attacker->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;

    // Skip targets that are immune to all damage (e.g., Ice Block, Divine Shield)
    if (attacker->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) &&
        attacker->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC))
        return false;

    // Relationship checks
    if (attacker->IsFriendlyTo(bot))
        return false;

    // Critter exception
    if (attacker->GetCreatureType() == CREATURE_TYPE_CRITTER && !attacker->IsInCombat())
        return false;

    // Visibility check
    if (!bot->CanSeeOrDetect(attacker))
        return false;

    // PvP prohibition checks (skip for duels)
    if ((attacker->GetGUID().IsPlayer() || attacker->GetGUID().IsPet()) &&
        (!bot->duel || bot->duel->Opponent != attacker) &&
        (sPlayerbotAIConfig.IsPvpProhibited(attacker->GetZoneId(), attacker->GetAreaId()) ||
        sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId())))
    {
        // This will stop aggresive pets from starting an attack.
        // This will stop currently attacking pets from continuing their attack.
        // This will first require the bot to change from a combat strat. It will
        // not be reached if the bot only switches targets, including NPC targets.
        for (Unit::ControlSet::const_iterator itr = bot->m_Controlled.begin();
            itr != bot->m_Controlled.end(); ++itr)
        {
            Creature* creature = dynamic_cast<Creature*>(*itr);
            if (creature && creature->GetVictim() == attacker)
            {
                creature->AttackStop();
                if (CharmInfo* charmInfo = creature->GetCharmInfo())
                    charmInfo->SetIsCommandAttack(false);
            }
        }

        return false;
    }

    // Unflagged player check
    if (attacker->IsPlayer() && !attacker->IsPvP() && !attacker->IsFFAPvP() &&
        (!bot->duel || bot->duel->Opponent != attacker))
        return false;

    // Creature-specific checks
    Creature* c = attacker->ToCreature();
    if (c)
    {
        if (c->IsInEvadeMode())
            return false;

        bool leaderHasThreat = false;
        if (bot->GetGroup() && botAI->GetMaster())
            leaderHasThreat = attacker->GetThreatMgr().GetThreat(botAI->GetMaster());

        // A player claims a creature by damaging it or landing a hostile spell on it, which is what
        // sets its loot recipient. The below code covers anti-kill-stealing mechanics.
        //
        // (1) Whatever the claim, the bot may attack if it (a) is in a raid/group and has a master
        //     holding nonzero threat on the creature, (b) has, or has a raid/group member that has,
        //     already tapped it, or (c) is already in combat with the creature.
        if (leaderHasThreat || c->isTappedBy(bot) || c->IsInCombatWith(bot))
            return true;

        // (2) Nobody has claimed the creature, so ask who it is attacking. If it is not attacking
        //     anything, or if it is attacking (a) something with no player behind it (e.g., a
        //     critter), (b) the bot, (c) the bot's master, or (d) a member of the bot's raid/group,
        //     then the victim is considered to belong to the bot and may be attacked. Clauses (b)
        //     through (d) also include a player's pets, guardians, totems, and charms.
        if (!c->hasLootRecipient())
        {
            Unit* victim = c->GetVictim();
            Player* victimOwner = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (!victim || !victimOwner || victimOwner == bot || victimOwner == botAI->GetMaster() ||
                (bot->GetGroup() && bot->GetGroup() == victimOwner->GetGroup()))
                return true;
        }

        // (3) Last because it is applied automatically only in battlegrounds and arenas: the
        //     "attack tagged" strategy always allows for attacking of creatures.
        return botAI->HasStrategy("attack tagged", BOT_STATE_NON_COMBAT);
    }

    return true;
}

char const* AttackersValue::PossibleTargetRejection(Unit* attacker, Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !attacker)
        return "no unit";
    if (!attacker->IsVisible())
        return "not visible";
    if (!attacker->IsInWorld() || attacker->GetMapId() != bot->GetMapId())
        return "not in world";
    if (attacker->isDead() || attacker->HasSpiritOfRedemptionAura())
        return "dead";
    if (IsCharmedAlly(attacker, bot))
        return "charmed ally";  // [b44_st]
    if (attacker->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2))
        return "non attackable";
    if (attacker->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC))
        return "immune to pc";
    if (attacker->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return "not selectable";
    if (attacker->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) && attacker->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC))
        return "immune to damage";
    if (attacker->IsFriendlyTo(bot))
        return "friendly";
    if (attacker->GetCreatureType() == CREATURE_TYPE_CRITTER && !attacker->IsInCombat())
        return "critter";
    if (!bot->CanSeeOrDetect(attacker))
        return "cannot see or detect";
    if (attacker->IsPlayer() || attacker->GetGUID().IsPet())
        return nullptr;  // the PvP rules are not diagnosed here

    Creature* creature = attacker->ToCreature();
    if (!creature)
        return nullptr;
    if (creature->IsInEvadeMode())
        return "evading";

    bool const leaderHasThreat =
        bot->GetGroup() && botAI->GetMaster() && attacker->GetThreatMgr().GetThreat(botAI->GetMaster());
    if (leaderHasThreat || creature->isTappedBy(bot) || creature->IsInCombatWith(bot))
        return nullptr;
    if (!creature->hasLootRecipient())
    {
        Unit* victim = creature->GetVictim();
        Player* victimOwner = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!victim || !victimOwner || victimOwner == bot || victimOwner == botAI->GetMaster() ||
            (bot->GetGroup() && bot->GetGroup() == victimOwner->GetGroup()))
            return nullptr;
        return botAI->HasStrategy("attack tagged", BOT_STATE_NON_COMBAT) ? nullptr : "fights another player";
    }
    return botAI->HasStrategy("attack tagged", BOT_STATE_NON_COMBAT) ? nullptr : "tagged by another player";
}

namespace
{
    // A dungeon boss the bot or its group fights stays a target round a corner: brs-h batch6, Quartermaster Zigris
    // walked 10 yd out of sight after Hooked Net / Stun Bomb, every bot dropped him and the attempt hung for 11 min.
    // The bots walk back into his sight (the reach actions move to a target that is out of range or out of sight).
    bool IsFoughtBossOutOfSight(Unit* attacker, Player* bot)
    {
        Creature* boss = attacker->ToCreature();
        if (!boss || !(boss->IsDungeonBoss() || boss->isWorldBoss()) || !boss->IsInCombat() ||
            boss->GetMap() != bot->GetMap() || !bot->GetMap()->IsDungeon())
            return false;

        if (boss->IsEngagedBy(bot))
            return true;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                boss->IsEngagedBy(member))
                return true;
        }

        return false;
    }

    // [rjoin-oos] Sunken Temple 2026-10-25 (20261025-081744-st-n-5A-92, pull 48): the hunter stood 16 yd from the Deep
    // Lurker the tank held, a pillar between them, 18 s idle while the other four fought it. Out of sight the mob was
    // no attacker of his, so no "dps target", no "dps assist", and the non-combat engine never joined; a target that
    // went round a corner was dropped the same way ("invalid target", the Frost Mage). A player walks round the pillar:
    // a mob in combat with a member of the bot's group, close and on the bot's floor, stays a target out of sight, and
    // the combat engine's reach actions ("enemy out of spell range" covers sight) bring the bot into view of it.
    constexpr float GROUP_FIGHT_SIGHT_RANGE = 35.0f;
    // [b47_idle] mgt-h 20261026-104908-mgt-h-5E-14 (300-480 s): a Sunblade Warlock stood still at (181.9,-5.7) for
    // 180 s on the priest (64.7 yd), out of sight of the tank, the warlock and the hunter at 46.1 yd: past the 35 yd
    // above, none of them listed it ("non combat, 0 attackers" while in combat), only the rogue saw it. A standing
    // caster on the group is the group's to walk to from farther (the reach actions' idle-pack guards still apply).
    constexpr float GROUP_FIGHT_STATIONARY_SIGHT_RANGE = 50.0f;
    constexpr float GROUP_FIGHT_SIGHT_MAX_DZ = 6.0f;
    constexpr uint32 GROUP_FIGHT_SIGHT_LOG_MS = 10 * IN_MILLISECONDS;

    bool IsGroupFightOutOfSight(Unit* attacker, Player* bot)
    {
        Creature* mob = attacker->ToCreature();
        Group* group = bot->GetGroup();
        Map* map = bot->GetMap();
        if (!mob || !group || !map || !map->IsDungeon() || !mob->IsInCombat() || mob->GetMap() != map ||
            std::fabs(mob->GetPositionZ() - bot->GetPositionZ()) > GROUP_FIGHT_SIGHT_MAX_DZ ||
            !bot->IsWithinDist(mob, GROUP_FIGHT_STATIONARY_SIGHT_RANGE))
            return false;

        // Fighting the member in melee: a mob still running to the tank of a line-of-sight pull, or a caster nuking
        // from deep in the room, is the tank's to bring (PullStrategy, KillOrder::IsInGroupFight).
        Unit* victim = mob->GetVictim();
        Player* owner = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!owner || !owner->IsAlive() || owner->GetGroup() != group)
            return false;

        // [b34_mgt] mgt-n 2026-10-26 (20261026-014521-mgt-n-5F-15, pull 30): a Sunblade Magister stood 18-24 yd from
        // the group round a corner for 180 s, nuking the healer (the only member left in combat with it) with
        // Frostbolts - it saw the healer, no bot saw it. Out of melee it was "the tank's to bring", but the tank had
        // left combat, the harness skull is filtered by sight too, and the run failed "trash stalemate" at 87 %.
        // A caster that has stood still on a member of the group for STATIONARY_CASTER_MS is no longer coming round
        // the corner of a line-of-sight pull: the bots walk into its sight (the reach actions) like a melee one.
        if (!mob->IsWithinMeleeRange(victim))
        {
            constexpr uint32 STATIONARY_CASTER_MS = 15 * IN_MILLISECONDS;
            constexpr float STATIONARY_CASTER_DRIFT = 2.0f;
            struct Seen
            {
                uint32 sinceMs;
                uint32 lastMs;
                float x;
                float y;
                uint64 victim;
            };
            static std::mutex seenLock;
            // [rot44] by (instance, GUID): one table for every map thread, parallel instances share creature GUIDs
            static std::map<InstanceGuid, Seen> seen;  // mob -> since when it stands on this victim, and where
            uint32 const nowMs = getMSTime() | 1u;
            uint64 const victimKey = victim->GetGUID().GetRawValue();
            uint32 standingMs = 0;
            {
                std::lock_guard<std::mutex> guard(seenLock);
                if (seen.size() > 4096)
                    seen.clear();
                Seen& entry = seen[InstanceKey(mob)];
                // A new victim, a move, or an entry nobody refreshed for 5 s starts the clock again.
                if (!entry.sinceMs || entry.victim != victimKey || getMSTimeDiff(entry.lastMs, nowMs) > 5000 ||
                    mob->GetExactDist2d(entry.x, entry.y) > STATIONARY_CASTER_DRIFT)
                    entry = { nowMs, nowMs, mob->GetPositionX(), mob->GetPositionY(), victimKey };
                entry.lastMs = nowMs;
                standingMs = getMSTimeDiff(entry.sinceMs, nowMs);
            }
            if (mob->isMoving() || standingMs < STATIONARY_CASTER_MS)
                return false;
        }
        else if (owner == bot || !bot->IsWithinDist(mob, GROUP_FIGHT_SIGHT_RANGE))  // [b47_idle] melee: 35 yd
            return false;

        static std::mutex logLock;
        static std::unordered_map<uint64, uint32> logged;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(logLock);
            uint32& at = logged[bot->GetGUID().GetRawValue()];
            if (at && getMSTimeDiff(at, now) < GROUP_FIGHT_SIGHT_LOG_MS)
                return true;
            at = now;
        }

        LOG_INFO("playerbots", "[rjoin-oos] {} keeps {} {} as a target out of sight ({:.1f} yd, on {})", bot->GetName(),
                 mob->GetName(), mob->GetEntry(), bot->GetExactDist(mob), owner->GetName());
        return true;
    }
}

// [b44_st] st-h 20261026-075135-st-h-5F-7 (3349-3367 s): Jammal'an's Hex of Jammal'an (12479, 10 s, a dummy whose
// removal casts the charm 12483: 15 s, -82 % speed, not dispellable) turned the hunter Rthunac hostile; the core's
// charm AI shot the resto druid dead. A charmed player is IsFriendlyTo() false and passes the hostile checks, so
// every path that picks targets (attackers, "enemy player target", the party-attacker helper) asks this first.
bool AttackersValue::IsCharmedAlly(Unit* unit, Player* bot)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    if (!player || !bot || player == bot || !player->IsCharmed())
        return false;

    Group* group = bot->GetGroup();
    if (group && player->GetGroup() == group)
        return true;

    return player->GetTeamId(true) == bot->GetTeamId(true) && !bot->InArena() &&
           (!bot->duel || bot->duel->Opponent != player);
}

Player* AttackersValue::FindCharmedAlly(Player* bot, float range)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return nullptr;

    Player* nearest = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() ||
            member->GetMap() != bot->GetMap() || !member->IsCharmed() || member->GetVehicle() ||
            !bot->IsWithinDist(member, range))
            continue;

        // A charm by the member's own side (a vehicle seat, a possess of its own) is no enemy's.
        Unit* charmer = member->GetCharmer();
        if (!charmer || charmer->IsFriendlyTo(bot))
            continue;

        if (member->HasUnitState(UNIT_STATE_CONTROLLED) || member->IsPolymorphed() ||
            member->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE) || member->HasAuraType(SPELL_AURA_MOD_PACIFY))
            continue;

        if (!nearest || bot->GetExactDist(member) < bot->GetExactDist(nearest))
            nearest = member;
    }
    return nearest;
}

bool AttackersValue::IsValidTarget(Unit* attacker, Player* bot)
{
    if (!IsPossibleTarget(attacker, bot))
        return false;

    return bot->IsWithinLOSInMap(attacker) || IsFoughtOutOfSight(attacker, bot);
}

bool AttackersValue::IsFoughtOutOfSight(Unit* attacker, Player* bot)
{
    if (!attacker)
        return false;
    if (IsFoughtBossOutOfSight(attacker, bot) || IsGroupFightOutOfSight(attacker, bot))
        return true;
    // [guild15] a mob hitting the bot in melee is in its reach whatever the sight ray says: the core's melee needs no
    // line of sight, and the ray from the bot's eyes to a small mob on a slope meets the ground (b332 / b333 Colian L12
    // warrior: a Bloodmyst Hatchling L10 at The Lost Fold hit him for 40 s, "melee 38.1 s dealt 0", no attacker in the
    // list, no target after the survival run stopped; he died twice to it at its full health)
    Unit* victim = attacker->GetVictim();
    return victim == bot && attacker->IsWithinMeleeRange(bot) && bot->IsWithinMeleeRange(attacker);
}

namespace
{
    bool IsQuestCritter(Creature* critter, Player* bot)
    {
        uint32 const entry = critter->GetEntry();
        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const required = quest->RequiredNpcOrGo[i];
                if (required > 0 && uint32(required) == entry &&
                    status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                    return true;
            }
        }

        CreatureTemplate const* creatureTemplate = critter->GetCreatureTemplate();
        return creatureTemplate && creatureTemplate->lootid &&
               LootTemplates_Creature.HaveQuestLootForPlayer(creatureTemplate->lootid, bot);
    }
}

bool AttackersValue::IsIgnoredCritter(Unit* unit, Player* bot)
{
    Creature* critter = unit ? unit->ToCreature() : nullptr;
    if (!critter || critter->GetCreatureType() != CREATURE_TYPE_CRITTER || critter->GetOwnerGUID().IsPlayer())
        return false;

    // The rare critter that actually fights (victim in reach, not running away) is an ordinary enemy.
    Unit* victim = critter->GetVictim();
    if (victim && !critter->HasUnitState(UNIT_STATE_FLEEING) && critter->IsWithinMeleeRange(victim))
        return false;

    return !IsQuestCritter(critter, bot);
}

bool PossibleAddsValue::Calculate()
{
    GuidVector possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();

    for (ObjectGuid const guid : possible)
    {
        if (find(attackers.begin(), attackers.end(), guid) != attackers.end())
            continue;
        Unit* add = botAI->GetUnit(guid);
        if (!add || !add->IsInWorld() || add->IsDuringRemoveFromWorld())
            continue;

        if (!add->GetTarget() && !add->GetThreatMgr().GetLastVictim() && add->IsHostileTo(bot))
        {
            for (ObjectGuid const attackerGUID : attackers)
            {
                Unit* attacker = botAI->GetUnit(attackerGUID);
                if (!attacker)
                    continue;

                float dist = ServerFacade::instance().GetDistance2d(attacker, add);
                if (ServerFacade::instance().IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.AoeRadius * 1.5f))
                    continue;

                if (ServerFacade::instance().IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.AggroDistance))
                    return true;
            }
        }
    }

    return false;
}
