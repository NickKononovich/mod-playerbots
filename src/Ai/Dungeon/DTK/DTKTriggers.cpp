/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DTKTriggers.h"
#include "AiObjectContext.h"
#include "DungeonInteract.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Pet.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include "StringFormat.h"  // [b33_dtk]
#include "TbcHazard.h"
#include "Timer.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace DTKShared
{
// The unit at the centre of the field: the caster of the bot's own field aura, else Novos while his field is up.
Unit* ArcaneFieldCenter(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (Aura* aura = bot->GetAura(SPELL_ARCANE_FIELD))
        if (Unit* caster = aura->GetCaster())
            return caster;

    Unit* novos = TbcHazard::FindBoss(botAI, "novos the summoner");
    return novos && novos->GetDynObject(SPELL_ARCANE_FIELD) ? novos : nullptr;
}

void Log(Player* bot, char const* tag, std::string const& key, std::string const& message, uint32 intervalMs)
{
    static std::mutex lock;
    static std::unordered_map<std::string, uint32> lastLogged;

    std::string const fullKey = bot->GetGUID().ToString() + ":" + tag + ":" + key;
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const it = lastLogged.find(fullKey);
        if (it != lastLogged.end() && getMSTimeDiff(it->second, now) < intervalMs)
            return;
        lastLogged[fullKey] = now;
    }

    LOG_INFO("playerbots", "[{}] {}: {}", tag, bot->GetName(), message);
}

Unit* AttackerWithEntry(PlayerbotAI* botAI, uint32 entry)
{
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != entry)
            continue;

        if (Unit* unit = botAI->GetUnit(guid); unit && unit->IsAlive())
            return unit;
    }
    return nullptr;
}

Unit* KingDred(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != DTKIds::MAP_ID || !bot->IsInCombat())
        return nullptr;

    Unit* dred = AttackerWithEntry(botAI, DTKIds::NPC_KING_DRED);
    return dred && dred->IsInCombat() ? dred : nullptr;
}

bool DredFearWindow(Unit* dred)
{
    if (dred->FindCurrentSpellBySpellId(DTKIds::SPELL_BELLOWING_ROAR))
        return true;

    float const health = dred->GetHealthPct();
    for (float const threshold : {DTKIds::DRED_FIRST_ROAR_PCT, DTKIds::DRED_SECOND_ROAR_PCT})
        if (health <= threshold + DTKIds::DRED_ROAR_LEAD_PCT && health > threshold - DTKIds::DRED_ROAR_LATE_PCT)
            return true;

    return false;
}

bool EngagedWithGroup(Player* bot, Unit* unit)
{
    if (!unit->IsInCombat())
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return unit->IsInCombatWith(bot);

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;

        if (unit->IsInCombatWith(member))
            return true;

        if (Pet* pet = member->GetPet(); pet && unit->IsInCombatWith(pet))
            return true;
    }
    return false;
}

Player* GroupTank(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && member->IsInWorld() && member->GetMap() == bot->GetMap() &&
            PlayerbotAI::IsTank(member))
            return member;
    }
    return nullptr;
}

Player* DredFearWardTarget(PlayerbotAI* botAI, Unit* dred)
{
    constexpr float FEAR_WARD_RANGE = 30.0f;

    // Fear Ward lasts 3 min and takes one fear: up before each roar, useless after the second.
    if (dred->GetHealthPct() <= DTKIds::DRED_SECOND_ROAR_PCT - DTKIds::DRED_ROAR_LATE_PCT ||
        !botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "fear ward")->Get())
        return nullptr;

    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    // The tank first (a feared tank leaves him free on the healer), then the healer.
    Player* best = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            !bot->IsWithinDistInMap(member, FEAR_WARD_RANGE) || botAI->HasAura("fear ward", member))
            continue;

        if (PlayerbotAI::IsTank(member))
        {
            best = member;
            break;
        }
        if (PlayerbotAI::IsHeal(member) && !best)
            best = member;
    }
    return best && botAI->CanCastSpell("fear ward", best) ? best : nullptr;
}

Player* DredBittenMember(PlayerbotAI* botAI)
{
    constexpr float HEAL_RANGE = 40.0f;
    constexpr float OTHER_MEMBER_LOW_PCT = 50.0f;  // the generic heals answer that member first

    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    if (!PlayerbotAI::IsHeal(bot) || !group || !KingDred(botAI))
        return nullptr;

    Player* bitten = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;

        if (member->GetHealthPct() < OTHER_MEMBER_LOW_PCT)
            return nullptr;

        if (member->IsFullHealth() || !member->HasAura(DTKIds::SPELL_GRIEVOUS_BITE) ||
            !bot->IsWithinDistInMap(member, HEAL_RANGE) || !bot->IsWithinLOSInMap(member))
            continue;

        if (!bitten || member->GetHealthPct() < bitten->GetHealthPct())
            bitten = member;
    }
    return bitten;
}

bool BeforeTrollgore(Player* bot)
{
    if (bot->GetMapId() != DTKIds::MAP_ID)
        return false;

    InstanceScript* instance = bot->GetInstanceScript();
    return instance && instance->GetBossState(DTKIds::DATA_TROLLGORE) != DONE;
}
}  // namespace DTKShared

bool KingDredFearGuardTrigger::IsActive()
{
    uint8 const classId = bot->getClass();
    if (classId != CLASS_SHAMAN && classId != CLASS_PRIEST && classId != CLASS_WARRIOR)
        return false;

    Unit* dred = DTKShared::KingDred(botAI);
    if (!dred)
        return false;

    switch (classId)
    {
        case CLASS_SHAMAN:
            return DTKShared::DredFearWindow(dred) && AI_VALUE2(uint32, "spell id", "tremor totem") &&
                   !AI_VALUE2(bool, "has totem", "tremor totem");
        case CLASS_PRIEST:
            return DTKShared::DredFearWardTarget(botAI, dred) != nullptr;
        default:
            return dred->FindCurrentSpellBySpellId(DTKIds::SPELL_BELLOWING_ROAR) &&
                   bot->GetExactDist2d(dred) <= DTKIds::BELLOWING_ROAR_RADIUS + dred->GetCombatReach() &&
                   !botAI->HasAura("berserker rage", bot) && botAI->CanCastSpell("berserker rage", bot);
    }
}

bool KingDredGrievousBiteTrigger::IsActive() { return DTKShared::DredBittenMember(botAI) != nullptr; }

bool InArcaneFieldTrigger::IsActive() { return bot->HasAura(SPELL_ARCANE_FIELD); }

bool CorpseExplodeTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "trollgore");
    if (!boss) { return false; }

    float distance = 6.0f;  // 5 unit radius, 1 unit added as buffer
    GuidVector corpses = AI_VALUE(GuidVector, "nearest corpses");
    for (auto i = corpses.begin(); i != corpses.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && DTKIds::IsTrollgoreInvader(unit->GetEntry()))
        {
            if (bot->GetExactDist2d(unit) < distance)
            {
                return true;
            }
        }
    }
    return false;
}

// [b33_dtk] The hold-room move exists to keep his fight inside his evade band (CheckInRoom). It assumes the group
// fights him; engaged from afar (wave / far threat entry, the group still in the hall) it ran the tank ~80 yd through
// his room's trash (dtk-n-5A-43 wipe, dtk-n-5A-1 212-285 s). Returns true while this bot must not start that move.
bool TrollgoreOutOfRoomTrigger::FarEngageHold(Unit* boss)
{
    constexpr float AT_BOSS_RANGE = 40.0f;       // within this of him the fight is "at Trollgore": old logic
    constexpr float ON_GROUP_RANGE = 30.0f;      // an invader this close to the bot is trash on the group
    constexpr float ROOM_TRASH_RADIUS = 30.0f;   // the same radius TrollgoreBodyPullMultiplier holds on
    constexpr float REGROUP_RANGE = 25.0f;       // the tank waits for members farther than this
    constexpr float AHEAD_MARGIN = 5.0f;         // a non-tank may not be this much closer to the hold spot
    constexpr float READY_HEALTH_PCT = 70.0f;
    constexpr float READY_MANA_PCT = 40.0f;
    constexpr uint32 WAIT_MAX_MS = 45 * IN_MILLISECONDS;  // regroup / readiness wait cap (the trash wait is not)
    constexpr uint32 NPC_DARKWEB_VICTIM = 27909;

    // His victim fights him (within 40 yd): the real boss fight, old behaviour for everybody (laggers join it).
    Unit* victim = boss->GetVictim();
    if (victim && boss->GetExactDist2d(victim) <= AT_BOSS_RANGE)
    {
        _waitSinceMs = 0;
        return false;
    }

    // 1. Trash on the group: every non-Trollgore attacker, invaders only once they are on us.
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
    {
        if (guid.GetEntry() == DTKIds::NPC_TROLLGORE)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        if (DTKIds::IsTrollgoreInvader(unit->GetEntry()) && bot->GetExactDist2d(unit) > ON_GROUP_RANGE)
            continue;

        _waitSinceMs = 0;
        DTKShared::Log(bot, "dtk trollgore far engage", "trash",
                       Acore::StringFormat("hold room held: trollgore {:.0f} yd away (victim {}), {} ({}) on the "
                                           "group: finishing the trash here",
                                           boss->GetExactDist2d(bot), victim ? victim->GetName() : "none",
                                           unit->GetName(), unit->GetEntry()),
                       10 * IN_MILLISECONDS);
        return true;
    }

    uint32 const now = getMSTime();
    if (!_waitSinceMs)
        _waitSinceMs = now;
    bool const waitExpired = getMSTimeDiff(_waitSinceMs, now) > WAIT_MAX_MS;

    // 2. One group: the tank leads and waits for stragglers; nobody walks in ahead of the tank.
    float const hereToHold = bot->GetExactDist2d(DTKIds::TROLLGORE_HOLD_X, DTKIds::TROLLGORE_HOLD_Y);
    bool ready = true;
    Player* tank = nullptr;
    Player* straggler = nullptr;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld() || member->GetMap() != bot->GetMap())
                continue;

            if (!member->IsAlive())
            {
                ready = false;
                continue;
            }

            if (member->GetHealthPct() < READY_HEALTH_PCT ||
                (PlayerbotAI::IsHeal(member) && member->GetPowerPct(POWER_MANA) < READY_MANA_PCT))
                ready = false;

            if (member != bot && PlayerbotAI::IsTank(member) && !tank)
                tank = member;

            if (member != bot && bot->GetExactDist2d(member) > REGROUP_RANGE &&
                member->GetExactDist2d(DTKIds::TROLLGORE_HOLD_X, DTKIds::TROLLGORE_HOLD_Y) > hereToHold)
                straggler = member;
        }
    }

    if (!waitExpired)
    {
        if (PlayerbotAI::IsTank(bot) && straggler)
        {
            DTKShared::Log(bot, "dtk trollgore far engage", "regroup",
                           Acore::StringFormat("hold room held: waiting for {} ({:.0f} yd) before walking in",
                                               straggler->GetName(), bot->GetExactDist2d(straggler)),
                           10 * IN_MILLISECONDS);
            return true;
        }

        if (!PlayerbotAI::IsTank(bot) && tank &&
            tank->GetExactDist2d(DTKIds::TROLLGORE_HOLD_X, DTKIds::TROLLGORE_HOLD_Y) > hereToHold + AHEAD_MARGIN)
        {
            DTKShared::Log(bot, "dtk trollgore far engage", "behind tank",
                           Acore::StringFormat("hold room held: not ahead of the tank {}", tank->GetName()),
                           10 * IN_MILLISECONDS);
            return true;
        }
    }

    // 3. Room trash still idle around him: walk in only once the group is ready.
    if (ready || waitExpired)
        return false;

    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        uint32 const entry = guid.GetEntry();
        if (!guid.IsCreature() || entry == DTKIds::NPC_TROLLGORE || DTKIds::IsTrollgoreInvader(entry) ||
            entry == NPC_DARKWEB_VICTIM)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->IsInCombat() || unit->IsCritter() || !bot->IsHostileTo(unit) ||
            boss->GetExactDist(unit) > ROOM_TRASH_RADIUS)
            continue;

        DTKShared::Log(bot, "dtk trollgore far engage", "not ready",
                       Acore::StringFormat("hold room held: group not ready and {} ({}) idle {:.0f} yd from "
                                           "trollgore",
                                           unit->GetName(), entry, boss->GetExactDist(unit)),
                       10 * IN_MILLISECONDS);
        return true;
    }
    return false;
}

bool TrollgoreOutOfRoomTrigger::IsActive()
{
    if (bot->GetMapId() != DTKIds::MAP_ID)
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "trollgore");
    if (!boss || !boss->IsAlive() || !boss->IsInCombat() || !boss->IsEngagedBy(bot))
        return false;

    // [b33_dtk] Engaged only from afar (the hall fight before his room): finish the trash where the group stands.
    if (FarEngageHold(boss))
        return false;

    // His victim drags him: it stays well inside; everybody else only has to stay inside (a healer pulling aggro
    // from outside would take him out).
    float const margin = boss->GetVictim() == bot ? DTKIds::TROLLGORE_ROOM_MARGIN : 2.0f;
    return !DTKIds::IsInsideTrollgoreRoom(bot->GetPositionY(), margin);
}

bool ArcaneFieldTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "novos the summoner");
    if (boss)
    {
        return boss->HasUnitState(UNIT_STATE_CASTING) && boss->FindCurrentSpellBySpellId(SPELL_ARCANE_FIELD);
    }
    return false;
}

// bool CrystalHandlerTrigger::IsActive()
// {
//     Unit* boss = AI_VALUE2(Unit*, "find target", "novos the summoner");
//     if (!boss) { return false; }

//     // Target is not findable from threat table using AI_VALUE2(),
//     // therefore need to search manually for the unit name
//     GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");

//     for (auto i = targets.begin(); i != targets.end(); ++i)
//     {
//         Unit* unit = botAI->GetUnit(*i);
//         if (unit && unit->GetEntry() == NPC_CRYSTAL_HANDLER)
//         {
//             return true;
//         }
//     }
//     return false;
// }

bool GiftOfTharonjaTrigger::IsActive()
{
    return bot->HasAura(SPELL_GIFT_OF_THARONJA);
}

bool NovosSummonTargetTargetedTrigger::IsActive()
{
    if (bot->GetMapId() != DTKIds::MAP_ID)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && DTKIds::IsNovosNeverTarget(target->GetEntry()))
        return true;

    Unit* victim = bot->GetVictim();
    return victim && DTKIds::IsNovosNeverTarget(victim->GetEntry());
}

namespace
{
constexpr float NOVOS_PULL_RANGE = 40.0f;
constexpr float NOVOS_GATHER_RANGE = 45.0f;
}  // namespace

bool NovosPullTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot) || !bot->IsAlive() || bot->IsInCombat())
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "novos the summoner");
    if (!boss || !boss->IsAlive() || boss->IsInCombat() || boss->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
        bot->GetDistance(boss) > NOVOS_PULL_RANGE)
        return false;

    return DungeonInteract::IsGroupGathered(bot, bot, NOVOS_GATHER_RANGE);
}
