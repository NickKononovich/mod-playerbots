/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AttackerCountValues.h"
#include "AttackersValue.h"
#include "BotMoveEvents.h"
#include "Creature.h"
#include "Engine.h"
#include "Group.h"
#include "LeaderKnowledge.h"  // [lead102] flee-for-assist health
#include "LeaderOrders.h"     // [lead104] the leader's flee-stop command
#include "Map.h"
#include "MotionMaster.h"
#include "Playerbots.h"
#include "RtiValue.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "Strategy.h"
#include "TargetValue.h"
#include "ThreatManager.h"
#include "Timer.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr float PEEL_RANGE = 30.0f;            // taunt range
constexpr float RESCUE_RANGE = 40.0f;          // Righteous Defense range, measured to the friend
constexpr float HEALER_RESCUE_RANGE = 60.0f;   // wave 8: healers died with the tank 40-54 yd away
constexpr float HEALER_RESCUE_MAX_DZ = 8.0f;   // a mob on another floor is not walked to
constexpr float HEALER_RESCUE_AREA = 6.0f;     // within this the tank's area threat already reaches the mob
constexpr float HEALER_VICTIM_SCORE = 200.0f;  // a mob on the healer before any on a damage dealer
constexpr float PEELED_PENALTY = 300.0f;       // a mob just taunted ranks below every other victim's mob
constexpr size_t LOOSE_LOG_TRAIL = 240;        // peel diagnostics: the end of the tank's action trail

// The non-tank group member `attacker` is hitting, or nullptr when the tank should leave it alone.
Player* GetAttackedMember(PlayerbotAI* botAI, Creature* attacker)
{
    Player* bot = botAI->GetBot();
    if (!attacker || !attacker->IsInWorld() || !attacker->IsAlive() || attacker->IsControlledByPlayer())
        return nullptr;

    if ((attacker->IsDungeonBoss() || attacker->isWorldBoss()) && bot->GetMap() && bot->GetMap()->IsRaid())
        return nullptr;

    Unit* victimUnit = attacker->GetVictim();
    Player* victim = victimUnit ? victimUnit->ToPlayer() : nullptr;
    if (!victim || victim == bot || !victim->IsAlive() || !bot->IsInSameRaidWith(victim) ||
        PlayerbotAI::IsTank(victim))
        return nullptr;

    return victim;
}

// As GetAttackedMember, for a mob within `range` yards of the bot.
Player* GetPeelVictim(PlayerbotAI* botAI, Creature* attacker, float range)
{
    Player* victim = GetAttackedMember(botAI, attacker);
    return victim && botAI->GetBot()->IsWithinDistInMap(attacker, range) ? victim : nullptr;
}

// The mobs the peel looks at: "attackers", and every mob hitting a non-tank member read from the member itself.
// "attackers" is refreshed once a second and built from threat lists: wave 25 logged 47 of 188 loose mobs as "not in
// attackers", a Ravaged Cadaver 3 yd from the tank among them while it killed the healer. A mob the tank cannot
// attack or see, under crowd control or evading is left out, as "attackers" does.
void GatherPeelCandidates(PlayerbotAI* botAI, GuidVector const& attackers, std::vector<Creature*>& candidates)
{
    Player* bot = botAI->GetBot();
    for (ObjectGuid const guid : attackers)
    {
        if (Creature* creature = botAI->GetCreature(guid))
            candidates.push_back(creature);
    }

    Group* group = bot->GetGroup();
    if (!group)
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || !member->IsInWorld() ||
            member->GetMap() != bot->GetMap() || PlayerbotAI::IsTank(member))
            continue;

        for (Unit* unit : member->getAttackers())
        {
            Creature* mob = unit ? unit->ToCreature() : nullptr;
            if (!mob || !mob->IsAlive() || mob->GetVictim() != member || mob->IsTotem() ||
                mob->GetOwnerGUID().IsPlayer() || mob->IsInEvadeMode() || IsCrowdControlled(mob) ||
                !bot->IsValidAttackTarget(mob) || !bot->CanSeeOrDetect(mob) ||
                std::find(candidates.begin(), candidates.end(), mob) != candidates.end())
                continue;

            candidates.push_back(mob);
        }
    }
}

// [rot_b31_cc] A mob under a crowd control icon of the group (any but Skull and Cross) is its controller's: a taunt
// made it the main tank's target and the Polymorph / Hex was refused on it (build 30: 300+ "refuses crowd control
// ... the group kills it now (main tank target)"). Its icon goes once the hold breaks or never lands
// (StaleAutoCcMarkTrigger); a healer it beats on is peeled at once.
bool LeftToCrowdControl(Player* bot, Creature* mob, Player* victim)
{
    return !PlayerbotAI::IsHeal(victim) && HasCrowdControlIcon(bot, mob);
}

// Healer victims first, then the most hurt victim, then the nearest mob.
float PeelScore(Player* bot, Creature* attacker, Player* victim)
{
    return (PlayerbotAI::IsHeal(victim) ? HEALER_VICTIM_SCORE : 0.0f) + (100.0f - victim->GetHealthPct()) -
           bot->GetDistance(attacker);
}

// The tank's single-target taunt is ready (Hand of Reckoning, Taunt, Growl, Dark Command): for the diagnostics.
bool IsTauntReady(PlayerbotAI* botAI)
{
    static char const* const TAUNTS[] = {"hand of reckoning", "taunt", "growl", "dark command"};
    AiObjectContext* context = botAI->GetAiObjectContext();
    Player* bot = botAI->GetBot();
    for (char const* taunt : TAUNTS)
    {
        uint32 const spellId = context->GetValue<uint32>("spell id", taunt)->Get();
        if (spellId && bot->HasSpell(spellId))
            return !bot->HasSpellCooldown(spellId);
    }

    return false;
}

// In a raid the main tank stays on its boss; the off-tanks pick up the adds.
bool MayPeel(Player* bot)
{
    if (!bot->GetGroup() || bot->InBattleground())
        return false;

    return !(bot->GetMap() && bot->GetMap()->IsRaid() && PlayerbotAI::IsMainTank(bot));
}
}

uint8 MyAttackerCountValue::Calculate() { return bot->getAttackers().size(); }

bool HasAggroValue::Calculate()
{
    Unit* target = GetTarget();
    if (!target)
    {
        return true;
    }
    Unit* victim = target->GetVictim();
    if (!victim)
    {
        return true;
    }
    bool isMT = PlayerbotAI::IsExplicitMainTank(bot);
    if (victim && (victim->GetGUID() == bot->GetGUID() ||
                   (!isMT && victim->ToPlayer() && PlayerbotAI::IsTank(victim->ToPlayer()))))
    {
        return true;
    }
    return false;
}

uint8 AttackerCountValue::Calculate()
{
    uint32 count = 0;
    float range = sPlayerbotAIConfig.SightDistance;

    GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        float distance = bot->GetDistance(unit);
        if (distance <= range)
            ++count;
    }

    return count;
}

uint8 BalancePercentValue::Calculate()
{
    float playerLevel = 0, attackerLevel = 0;

    if (Group* group = bot->GetGroup())
    {
        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            Player* player = ObjectAccessor::FindPlayer(itr->guid);
            if (!player || !player->IsAlive())
                continue;

            playerLevel += player->GetLevel();
        }
        uint32 memberCount = group->GetMembersCount();
        playerLevel /= memberCount;
        if (memberCount <= 10)
            playerLevel *= memberCount;
        else
            playerLevel *= 10;
    }
    GuidVector v = context->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : v)
    {
        Creature* creature = botAI->GetCreature((guid));
        if (!creature || !creature->IsAlive())
            continue;

        uint32 level = creature->GetLevel();

        switch (creature->GetCreatureTemplate()->rank)
        {
            case CREATURE_ELITE_RARE:
                level *= 2;
                break;
            case CREATURE_ELITE_ELITE:
                level *= 3;
                break;
            case CREATURE_ELITE_RAREELITE:
                level *= 3;
                break;
            case CREATURE_ELITE_WORLDBOSS:
                level *= 20;
                break;
        }

        attackerLevel += level;
    }

    if (!attackerLevel)
        return 100;

    float percent = playerLevel * 100 / attackerLevel;
    return percent <= 200 ? (uint8)percent : 200;
}

Unit* AttackerCountValue::GetTarget()
{
    AiObjectContext* ctx = AiObject::context;
    return ctx->GetValue<Unit*>(qualifier)->Get();
}

Unit* MyAttackerCountValue::GetTarget()
{
    AiObjectContext* ctx = AiObject::context;
    return ctx->GetValue<Unit*>(qualifier)->Get();
}

Unit* HasAggroValue::GetTarget()
{
    AiObjectContext* ctx = AiObject::context;
    return ctx->GetValue<Unit*>(qualifier)->Get();
}

Unit* BalancePercentValue::GetTarget()
{
    AiObjectContext* ctx = AiObject::context;
    return ctx->GetValue<Unit*>(qualifier)->Get();
}

Unit* GroupMemberAttackerValue::Calculate()
{
    if (!MayPeel(bot))
        return nullptr;

    uint32 const now = getMSTime();
    for (auto itr = _peeled.begin(); itr != _peeled.end();)
    {
        if (getMSTimeDiff(itr->second, now) > PEEL_SPREAD_MS)
            itr = _peeled.erase(itr);
        else
            ++itr;
    }

    Unit* best = nullptr;
    float bestScore = 0.0f;
    GuidVector const attackers = context->GetValue<GuidVector>("attackers")->Get();
    std::vector<Creature*> candidates;
    GatherPeelCandidates(botAI, attackers, candidates);
    for (Creature* attacker : candidates)
    {
        Player* victim = GetPeelVictim(botAI, attacker, PEEL_RANGE);
        // a taunt needs sight: a mob behind a cell door picked every tick kept every peel spell impossible
        if (!victim || LeftToCrowdControl(bot, attacker, victim) || !bot->IsWithinLOSInMap(attacker))
            continue;

        float score = PeelScore(bot, attacker, victim);
        if (_peeled.find(attacker->GetGUID()) != _peeled.end())
            score -= PEELED_PENALTY;

        if (!best || score > bestScore)
        {
            best = attacker;
            bestScore = score;
        }
    }

    TrackLooseMobs(attackers, best);
    return best;
}

Unit* GroupMemberAttackerValue::FindInReach(PlayerbotAI* botAI, float range)
{
    Player* bot = botAI->GetBot();
    if (!MayPeel(bot))
        return nullptr;

    std::vector<Creature*> candidates;
    GatherPeelCandidates(botAI, botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get(), candidates);
    Unit* best = nullptr;
    float bestScore = 0.0f;
    for (Creature* attacker : candidates)
    {
        Player* victim = GetAttackedMember(botAI, attacker);
        if (!victim || LeftToCrowdControl(bot, attacker, victim) || !bot->IsWithinCombatRange(attacker, range) ||
            !bot->IsWithinLOSInMap(attacker))
            continue;

        float const score = PeelScore(bot, attacker, victim);
        if (!best || score > bestScore)
        {
            best = attacker;
            bestScore = score;
        }
    }

    return best;
}

Unit* GroupMemberAttackerValue::FindForTaunt(PlayerbotAI* botAI, float range)
{
    Player* bot = botAI->GetBot();
    if (!MayPeel(bot))
        return nullptr;

    AiObjectContext* context = botAI->GetAiObjectContext();
    auto* value = dynamic_cast<GroupMemberAttackerValue*>(context->GetValue<Unit*>("group member attacker"));
    std::vector<Creature*> candidates;
    GatherPeelCandidates(botAI, context->GetValue<GuidVector>("attackers")->Get(), candidates);
    GuidSet const exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::TankTarget);
    uint32 const now = getMSTime();
    Unit* best = nullptr;
    float bestScore = 0.0f;
    for (Creature* attacker : candidates)
    {
        Player* victim = GetAttackedMember(botAI, attacker);
        if (!victim || !bot->IsWithinCombatRange(attacker, range) ||
            exclusions.find(attacker->GetGUID()) != exclusions.end() || !bot->IsWithinLOSInMap(attacker))
            continue;

        float score = PeelScore(bot, attacker, victim);
        if (!bot->IsWithinDistInMap(attacker, TAUNT_AREA_REACH))
            score += TAUNT_FAR_BONUS;

        if (value)
        {
            auto const peeled = value->_peeled.find(attacker->GetGUID());
            if (peeled != value->_peeled.end() && getMSTimeDiff(peeled->second, now) <= PEEL_SPREAD_MS)
                score -= PEELED_PENALTY;
        }

        if (!best || score > bestScore)
        {
            best = attacker;
            bestScore = score;
        }
    }

    return best;
}

void GroupMemberAttackerValue::NoteReaction(PlayerbotAI* botAI, Unit* mob, Unit* victim, std::string const& spell,
                                            std::string const& via)
{
    Player* bot = botAI->GetBot();
    Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
    if (!mob || !victimPlayer || victimPlayer == bot || !bot->GetGroup())
        return;

    auto* value =
        dynamic_cast<GroupMemberAttackerValue*>(botAI->GetAiObjectContext()->GetValue<Unit*>("group member attacker"));
    if (!value)
        return;

    value->Get();  // the loose record, up to date within CHECK_INTERVAL_MS
    uint32 delayMs = 0;
    auto const found = value->_loose.find(mob->GetGUID());
    if (found != value->_loose.end() && found->second.victim == victimPlayer->GetGUID())
    {
        if (found->second.reacted)
            return;  // this loose spell is logged already (a second spell on the same mob)

        found->second.reacted = true;
        delayMs = getMSTimeDiff(found->second.sinceMs, getMSTime());
    }

    char const* role = PlayerbotAI::IsHeal(victimPlayer)   ? "healer"
                       : PlayerbotAI::IsTank(victimPlayer) ? "tank"
                       : PlayerbotAI::IsRanged(victimPlayer) ? "ranged"
                                                             : "melee";
    float const distance = bot->GetExactDist(mob);
    LOG_INFO("playerbots", "[taunt reaction] {}: {} on {} {} off {} ({}) after {} ms, {:.1f} yd [{}]", bot->GetName(),
             spell, mob->GetName(), mob->GetEntry(), victimPlayer->GetName(), role, delayMs, distance, via);
    BotMoveEvents::Push(bot->GetGUID(), BotMoveEvents::Event{"taunt_reaction", {}, {}}
                                            .Str("tank", bot->GetName())
                                            .Str("mob", mob->GetName())
                                            .Num("entry", mob->GetEntry())
                                            .Str("victim", victimPlayer->GetName())
                                            .Str("victimRole", role)
                                            .Num("delayMs", delayMs)
                                            .Str("spell", spell)
                                            .Str("via", via)
                                            .Real("distance", distance));
}

uint32 GroupMemberAttackerValue::LooseForMs(PlayerbotAI* botAI, ObjectGuid mob)
{
    auto* value =
        dynamic_cast<GroupMemberAttackerValue*>(botAI->GetAiObjectContext()->GetValue<Unit*>("group member attacker"));
    if (!value)
        return 0;

    value->Get();  // brings the record up to date, once per CHECK_INTERVAL_MS
    auto const found = value->_loose.find(mob);
    return found == value->_loose.end() ? 0 : getMSTimeDiff(found->second.sinceMs, getMSTime());
}

// Every mob on a non-tank member is remembered with the time it got there (LooseForMs: the tank's target switch reads
// it). With the debug log on, a mob loose for LOOSE_LOG_MS is logged once with why the peel did or did not take it.
void GroupMemberAttackerValue::TrackLooseMobs(GuidVector const& attackers, Unit* best)
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        _loose.clear();
        return;
    }

    bool const logging = !group->isRaidGroup() && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    uint32 const now = getMSTime();
    std::unordered_map<ObjectGuid, LooseMob> seen;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            PlayerbotAI::IsTank(member))
            continue;

        for (Unit* unit : member->getAttackers())
        {
            Creature* mob = unit ? unit->ToCreature() : nullptr;
            if (!mob || !mob->IsAlive() || mob->GetVictim() != member || mob->IsTotem() ||
                mob->GetOwnerGUID().IsPlayer())
                continue;

            LooseMob entry;
            auto const known = _loose.find(mob->GetGUID());
            if (known != _loose.end() && known->second.victim == member->GetGUID())
                entry = known->second;
            else
            {
                entry.victim = member->GetGUID();
                entry.sinceMs = now;
            }

            if (logging && !entry.logged && getMSTimeDiff(entry.sinceMs, now) >= LOOSE_LOG_MS)
            {
                entry.logged = true;
                bool const listed = std::find(attackers.begin(), attackers.end(), mob->GetGUID()) != attackers.end();
                float const distance = bot->GetDistance(mob);
                // the peel takes unlisted mobs too (GatherPeelCandidates): why "attackers" left it out
                std::string why = "another mob picked";
                if (!bot->IsWithinDistInMap(mob, PEEL_RANGE))
                    why = "beyond taunt range";
                else if (!bot->IsWithinLOSInMap(mob))
                    why = "no line of sight";
                else if (mob == best)
                    why = "picked";
                if (!listed)
                {
                    char const* rejection = AttackersValue::PossibleTargetRejection(mob, bot);
                    if (!rejection)
                        rejection = mob->IsThreatenedBy(member) ? "listed later" : "victim not on its threat list";

                    why += std::string(", not in attackers: ") + rejection;
                }

                Engine* engine = botAI->GetCurrentEngine();
                std::string trail = engine ? engine->GetLastAction() : "";
                if (trail.size() > LOOSE_LOG_TRAIL)
                    trail = trail.substr(trail.size() - LOOSE_LOG_TRAIL);

                Unit* current = context->GetValue<Unit*>("current target")->Get();
                LOG_DEBUG("playerbots",
                          "[peel diag] {} ({}): {} on {} ({}) {:.1f} yd: {}, pick {}, current {}, taunt {}, trail {}",
                          bot->GetName(), botAI->GetState() == BOT_STATE_COMBAT ? "combat" : "not combat",
                          mob->GetName(), member->GetName(), PlayerbotAI::IsHeal(member) ? "healer" : "dps", distance,
                          why, best ? best->GetName() : "none", current ? current->GetName() : "none",
                          IsTauntReady(botAI) ? "ready" : "cooling", trail);
            }

            seen[mob->GetGUID()] = entry;
        }
    }

    _loose = std::move(seen);
}

void GroupMemberAttackerValue::RecordPeel(PlayerbotAI* botAI, GuidVector const& mobs)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    auto* value = dynamic_cast<GroupMemberAttackerValue*>(context->GetValue<Unit*>("group member attacker"));
    if (!value || mobs.empty())
        return;

    uint32 const now = getMSTime();
    for (ObjectGuid const guid : mobs)
        value->_peeled[guid] = now;

    value->Reset();
    context->GetValue<Unit*>("attacked group member")->Reset();
}

bool GroupMemberAttackerValue::IsRecentlyPeeled(PlayerbotAI* botAI, ObjectGuid mob)
{
    auto* value =
        dynamic_cast<GroupMemberAttackerValue*>(botAI->GetAiObjectContext()->GetValue<Unit*>("group member attacker"));
    if (!value)
        return false;

    auto const found = value->_peeled.find(mob);
    return found != value->_peeled.end() && getMSTimeDiff(found->second, getMSTime()) <= PEEL_SPREAD_MS;
}

namespace
{
struct FledNote
{
    uint32 seenMs = 0;
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint32 stoppedMs = 0;
};

std::mutex fledLock;
std::unordered_map<ObjectGuid, FledNote> fledMobs;
}

bool FledMobs::IsRunning(Unit const* unit)
{
    if (!unit || !unit->IsAlive() || unit->IsPlayer() || unit->IsControlledByPlayer())
        return false;

    MotionMaster* motion = const_cast<Unit*>(unit)->GetMotionMaster();
    if (motion && motion->GetCurrentMovementGeneratorType() == ASSISTANCE_MOTION_TYPE)
        return true;

    return unit->HasUnitState(UNIT_STATE_FLEEING) && !unit->HasAuraType(SPELL_AURA_MOD_FEAR);
}

void FledMobs::Note(Unit const* unit)
{
    if (!unit || !unit->IsInWorld())
        return;

    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> lock(fledLock);
    if (fledMobs.size() >= MAX_MOBS && !fledMobs.count(unit->GetGUID()))
    {
        auto oldest = fledMobs.begin();
        for (auto itr = fledMobs.begin(); itr != fledMobs.end(); ++itr)
        {
            if (getMSTimeDiff(itr->second.seenMs, now) > getMSTimeDiff(oldest->second.seenMs, now))
                oldest = itr;
        }
        fledMobs.erase(oldest);
    }

    FledNote& note = fledMobs[unit->GetGUID()];
    note.seenMs = now ? now : 1;
    note.mapId = unit->GetMapId();
    note.instanceId = unit->GetInstanceId();
}

bool FledMobs::IsSurvivor(Creature const* creature)
{
    if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsInCombat() ||
        creature->IsInEvadeMode())
        return false;

    // still hurt, or standing where it ran to: out of combat it regenerates, but it does not walk home (an evade
    // takes it home at full health: then it is ordinary trash again)
    if (creature->GetHealth() >= creature->GetMaxHealth() &&
        creature->GetExactDist(creature->GetHomePosition()) <= HOME_SLACK)
        return false;

    std::lock_guard<std::mutex> lock(fledLock);
    auto const found = fledMobs.find(creature->GetGUID());
    return found != fledMobs.end() && getMSTimeDiff(found->second.seenMs, getMSTime()) <= MEMO_MS;
}

GuidVector FledMobs::Noted(uint32 mapId, uint32 instanceId)
{
    GuidVector result;
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> lock(fledLock);
    for (auto const& [guid, note] : fledMobs)
    {
        if (note.mapId == mapId && note.instanceId == instanceId && getMSTimeDiff(note.seenMs, now) <= MEMO_MS)
            result.push_back(guid);
    }

    return result;
}

void FledMobs::MarkStopped(ObjectGuid mob)
{
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> lock(fledLock);
    auto const found = fledMobs.find(mob);
    if (found != fledMobs.end())
        found->second.stoppedMs = now ? now : 1;
}

bool FledMobs::StoppedRecently(ObjectGuid mob)
{
    std::lock_guard<std::mutex> lock(fledLock);
    auto const found = fledMobs.find(mob);
    return found != fledMobs.end() && found->second.stoppedMs &&
           getMSTimeDiff(found->second.stoppedMs, getMSTime()) <= STOP_SPREAD_MS;
}

Unit* FleeingMobValue::Calculate()
{
    if (!bot->IsInCombat() || !bot->GetGroup())
        return nullptr;

    // [lead104] project rule: the leader decides, the members execute - in a led group the fleer to stop is the
    // leader's command (one member per fleer), never the member's own pick
    if (Leader::LedBot(bot))
    {
        Leader::MemberEvents events;
        if (!Leader::Events::Get(bot->GetGUID(), events) || events.fleeStop.IsEmpty())
            return nullptr;
        Unit* commanded = botAI->GetUnit(events.fleeStop);
        return commanded && commanded->IsAlive() ? commanded : nullptr;
    }

    Unit* nearest = nullptr;
    float nearestDistance = 0.0f;
    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        // [lead102] b140 rfc-h 139-153 s: a Ragefire Shaman hit its SmartAI flee-for-assist health (smart_scripts
        // 11319: 0-15 %) and ran 20 -> 83 yd, 24 yd down into the Trogg camp before anyone stopped it - the stop came
        // only once it ran (and was soon out of range / sight). A mob at its known flee health is stopped as it gets
        // there, before the run (a game fact: its own script's health line)
        bool const atFleeHealth =
            unit && unit->IsAlive() && unit->ToCreature() &&
            unit->GetHealthPct() <= Leader::Knowledge::FleeForAssistHealthPct(unit->GetEntry()) &&
            !unit->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_ROOT) && !FledMobs::StoppedRecently(unit->GetGUID());
        if (!atFleeHealth && !FledMobs::IsRunning(unit))
            continue;

        FledMobs::Note(unit);
        float const distance = bot->GetExactDist(unit);
        if (distance > RANGE || (nearest && distance >= nearestDistance) || !bot->IsValidAttackTarget(unit) ||
            !bot->IsWithinLOSInMap(unit))
            continue;

        nearest = unit;
        nearestDistance = distance;
    }

    return nearest;
}

Unit* FledSurvivorValue::Calculate()
{
    if (!bot->GetGroup() || !bot->IsInWorld())
        return nullptr;

    Unit* nearest = nullptr;
    float nearestDistance = 0.0f;
    for (ObjectGuid const& guid : FledMobs::Noted(bot->GetMapId(), bot->GetInstanceId()))
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || creature->GetMap() != bot->GetMap() || !FledMobs::IsSurvivor(creature))
            continue;

        float const distance = bot->GetExactDist(creature);
        if (distance > RANGE || (nearest && distance >= nearestDistance) || !bot->IsValidAttackTarget(creature) ||
            !bot->IsWithinLOSInMap(creature))
            continue;

        nearest = creature;
        nearestDistance = distance;
    }

    return nearest;
}

uint8 GroupMemberAttackerCountValue::Calculate()
{
    if (!MayPeel(bot))
        return 0;

    float const range = qualifier.empty() ? PEEL_RANGE : float(atof(qualifier.c_str()));
    uint8 count = 0;
    std::vector<Creature*> candidates;
    GatherPeelCandidates(botAI, context->GetValue<GuidVector>("attackers")->Get(), candidates);
    for (Creature* attacker : candidates)
    {
        if (GetPeelVictim(botAI, attacker, range))
            ++count;
    }

    return count;
}

Unit* AttackedGroupMemberValue::Calculate()
{
    if (!MayPeel(bot))
        return nullptr;

    std::vector<std::pair<Player*, uint32>> victims;
    std::vector<Creature*> candidates;
    GatherPeelCandidates(botAI, context->GetValue<GuidVector>("attackers")->Get(), candidates);
    for (Creature* attacker : candidates)
    {
        Player* victim = GetAttackedMember(botAI, attacker);
        if (!victim || !bot->IsWithinDistInMap(victim, RESCUE_RANGE))
            continue;

        // only the mobs the peel has not just taken: Righteous Defense goes to the victim the last taunt left alone
        uint32 const fresh = GroupMemberAttackerValue::IsRecentlyPeeled(botAI, attacker->GetGUID()) ? 0 : 1;
        auto found = std::find_if(victims.begin(), victims.end(),
                                  [victim](std::pair<Player*, uint32> const& entry) { return entry.first == victim; });
        if (found == victims.end())
            victims.emplace_back(victim, fresh);
        else
            found->second += fresh;
    }

    Player* best = nullptr;
    float bestScore = 0.0f;
    for (auto const& [victim, mobs] : victims)
    {
        if (!bot->IsWithinLOSInMap(victim))
            continue;

        float score = (PlayerbotAI::IsHeal(victim) ? HEALER_VICTIM_SCORE : 0.0f) + (100.0f - victim->GetHealthPct()) +
                      15.0f * static_cast<float>(mobs);
        if (!mobs)
            score -= PEELED_PENALTY;

        if (!best || score > bestScore)
        {
            best = victim;
            bestScore = score;
        }
    }

    return best;
}

Unit* DefendMemberTargetValue::Calculate()
{
    if (!bot->IsAlive() || !bot->GetGroup() || bot->InBattleground() || !PlayerbotAI::IsTank(bot))
        return nullptr;

    // the members' own attackers only: a mob whose victim is a member is in that member's getAttackers()
    std::vector<Creature*> candidates;
    GatherPeelCandidates(botAI, GuidVector(), candidates);
    GuidSet const exclusions = GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::TankTarget);
    Unit* best = nullptr;
    float bestScore = 0.0f;
    for (Creature* attacker : candidates)
    {
        Player* victim = GetAttackedMember(botAI, attacker);
        if (!victim || exclusions.find(attacker->GetGUID()) != exclusions.end())
            continue;

        bool const healerInReach =
            PlayerbotAI::IsHeal(victim) && bot->IsWithinDistInMap(attacker, HEALER_RESCUE_RANGE) &&
            std::fabs(attacker->GetPositionZ() - bot->GetPositionZ()) <= HEALER_RESCUE_MAX_DZ;
        if ((!healerInReach && !bot->IsWithinDistInMap(attacker, RESCUE_RANGE)) || !bot->IsWithinLOSInMap(attacker))
            continue;

        float const score = PeelScore(bot, attacker, victim);
        if (!best || score > bestScore)
        {
            best = attacker;
            bestScore = score;
        }
    }

    return best;
}

bool FarHealerAttackerValue::IsHealerAttackerInReach(PlayerbotAI* botAI, Creature* attacker)
{
    Player* victim = GetAttackedMember(botAI, attacker);
    if (!victim || !PlayerbotAI::IsHeal(victim))
        return false;

    Player* tank = botAI->GetBot();
    return tank->IsWithinDistInMap(attacker, HEALER_RESCUE_RANGE) &&
           std::fabs(attacker->GetPositionZ() - tank->GetPositionZ()) <= HEALER_RESCUE_MAX_DZ;
}

void FarHealerAttackerValue::GetHealerAttackers(PlayerbotAI* botAI, std::vector<Creature*>& attackers)
{
    Player* tank = botAI->GetBot();
    Group* group = tank->GetGroup();
    if (!group)
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == tank || !member->IsAlive() || !member->IsInWorld() ||
            member->GetMapId() != tank->GetMapId() || !PlayerbotAI::IsHeal(member))
            continue;

        for (auto const& [guid, threatRef] : member->GetThreatMgr().GetThreatenedByMeList())
        {
            Creature* attacker = threatRef ? threatRef->GetOwner() : nullptr;
            if (!attacker || !tank->IsValidAttackTarget(attacker) || !tank->CanSeeOrDetect(attacker) ||
                !IsHealerAttackerInReach(botAI, attacker) ||
                std::find(attackers.begin(), attackers.end(), attacker) != attackers.end())
                continue;

            attackers.push_back(attacker);
        }
    }
}

Unit* FarHealerAttackerValue::Calculate()
{
    if (!MayPeel(bot) || !botAI->IsInNonRaidDungeon())
        return nullptr;

    // A boss held by the tank stays where its encounter strategy wants it; the taunts still reach its adds.
    Unit* current = AI_VALUE(Unit*, "current target");
    Creature* currentCreature = current ? current->ToCreature() : nullptr;
    if (currentCreature && currentCreature->IsDungeonBoss() && current->GetVictim() == bot)
        return nullptr;

    std::vector<Creature*> attackers;
    GetHealerAttackers(botAI, attackers);
    Unit* nearest = nullptr;
    float nearestDistance = 0.0f;
    for (Creature* attacker : attackers)
    {
        float const distance = bot->GetExactDist2d(attacker);
        if (distance <= HEALER_RESCUE_AREA)
            continue;

        if (!nearest || distance < nearestDistance)
        {
            nearest = attacker;
            nearestDistance = distance;
        }
    }

    return nearest;
}

Unit* HealerAttackerNearValue::Calculate()
{
    if (!bot->GetGroup() || bot->InBattleground() || qualifier.empty())
        return nullptr;

    float const range = float(atof(qualifier.c_str()));
    Unit* nearest = nullptr;
    float nearestDistance = 0.0f;
    GuidVector const attackers = context->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Creature* attacker = botAI->GetCreature(guid);
        Player* victim = GetAttackedMember(botAI, attacker);
        if (!victim || !PlayerbotAI::IsHeal(victim))
            continue;

        float const distance = bot->GetExactDist2d(attacker);
        if (distance > range || (nearest && distance >= nearestDistance))
            continue;

        nearest = attacker;
        nearestDistance = distance;
    }

    return nearest;
}
