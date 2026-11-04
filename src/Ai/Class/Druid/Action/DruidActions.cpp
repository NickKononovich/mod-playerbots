/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidActions.h"
#include "AoeValues.h"
#include "DruidTriggers.h"
#include "Event.h"
#include "Group.h"
#include "PartyMemberToHeal.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SpellMgr.h"  // [lead1061]
#include "TargetValue.h"
#include "Timer.h"  // [rot1]
#include "LeaderOrders.h"  // [rot38] Leader::ExecutorMember, [rot371] Leader::LedBot

#include <unordered_map>  // [rot371]
#include <unordered_set>  // [lead1061]

constexpr uint32 SPELL_ECLIPSE_SOLAR = 48517;
constexpr uint32 SPELL_ECLIPSE_LUNAR = 48518;
// Titanforged Starcaller's Regalia (E1) 2p charge: the next Starfire is instant ([setbonus-rot]).
constexpr uint32 SPELL_LUNAR_SURGE = 127174;

namespace
{
    bool PrepareThornsTarget(PlayerbotAI* botAI, Unit* target)
    {
        if (!target)
            return false;

        Aura* existingThorns = botAI->GetAura("thorns", target, true);
        if (!existingThorns)
            return true;

        target->RemoveOwnedAura(existingThorns, AURA_REMOVE_BY_CANCEL);
        return true;
    }
}

std::vector<NextAction> CastAbolishPoisonAction::getAlternatives()
{
    return NextAction::merge({ NextAction("cure poison") },
                             CastSpellAction::getPrerequisites());
}

std::vector<NextAction> CastAbolishPoisonOnPartyAction::getAlternatives()
{
    return NextAction::merge({ NextAction("cure poison on party") },
                             CastSpellAction::getPrerequisites());
}

bool CastLifebloomOnMainTankAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !CastSpellAction::isUseful() || HotUpkeepYields())
        return false;

    Aura* lifebloom = botAI->GetAura("lifebloom", target, true, true);
    if (LifebloomLetBloom(bot, target, lifebloom))  // [b31_sd] the trigger's rule, for the Clearcasting node too
        return false;

    return !lifebloom || lifebloom->GetStackAmount() < 3 || lifebloom->GetDuration() < LIFEBLOOM_REFRESH_WINDOW_MS;
}

bool CastThornsAction::Execute(Event event)
{
    return PrepareThornsTarget(botAI, GetTarget()) && CastBuffSpellAction::Execute(event);
}

bool CastThornsOnPartyAction::Execute(Event event)
{
    return PrepareThornsTarget(botAI, GetTarget()) && BuffOnPartyAction::Execute(event);
}

bool CastThornsOnMainTankAction::Execute(Event event)
{
    return PrepareThornsTarget(botAI, GetTarget()) && BuffOnMainTankAction::Execute(event);
}

// [b31_deep] Eclipse fishing by the Lunar proc cooldown (wowsims balance APL; spell_dru_eclipse: Lunar procs from a
// Wrath crit, Solar from a Starfire crit, each with its own 30 s cooldown from its proc, neither while an eclipse is
// up). Outside an eclipse: Wrath while the Lunar cooldown is ready (fish Lunar, also in the opener), Starfire while
// it is not (fish Solar). The old rule blocked each nuke for 30 s after the other eclipse and cleared the Lunar
// timer on a Solar proc, so after Solar ended the default Starfire (5.4) won with Lunar ready and not yet procced.
namespace
{
    constexpr time_t ECLIPSE_PROC_COOLDOWN_S = 30;
    constexpr uint32 SPELL_ECLIPSE_TALENT_R1 = 48516;
    constexpr uint32 SPELL_ECLIPSE_TALENT_R2 = 48521;
    constexpr uint32 SPELL_ECLIPSE_TALENT_R3 = 48525;

    // [b35_bal] Eclipse is a PASSIVE talent: Player::LearnTalent never adds it to m_spells, so HasSpell() was always
    // false and Lunar fishing never ran. Check the active spec's talent map instead.
    bool HasEclipseTalent(Player* bot)
    {
        uint8 const spec = bot->GetActiveSpec();
        return bot->HasTalent(SPELL_ECLIPSE_TALENT_R1, spec) || bot->HasTalent(SPELL_ECLIPSE_TALENT_R2, spec) ||
               bot->HasTalent(SPELL_ECLIPSE_TALENT_R3, spec);
    }

    // Records the start of an active Lunar Eclipse (from its remaining duration, so a missed tick does not shift it)
    // and tells whether the Lunar proc cooldown is over.
    bool LunarProcReady(AiObjectContext* context, Player* bot)
    {
        time_t const now = time(nullptr);
        if (Aura* lunar = bot->GetAura(SPELL_ECLIPSE_LUNAR))
        {
            time_t const elapsed = (lunar->GetMaxDuration() - lunar->GetDuration()) / IN_MILLISECONDS;
            context->GetValue<time_t>("eclipse lunar proc time")->Set(now - elapsed);
            return false;
        }

        time_t const lunarTime = context->GetValue<time_t>("eclipse lunar proc time")->Get();
        return !lunarTime || now - lunarTime >= ECLIPSE_PROC_COOLDOWN_S;
    }
}

bool DruidLunarProcReady(AiObjectContext* context, Player* bot) { return LunarProcReady(context, bot); }

namespace
{
    // [lead1061] b231 rfc-h 5C-3 pull 25: the balance druid stood 20 s at a Molten Elemental with the leader's Attack
    // order: "wrath ... target immune" (its nature school), Starfire held for the Wrath that fishes for Lunar Eclipse,
    // Moonfire ticking. A nuke that cannot hit the target (Unit::IsImmunedToDamageOrSchool, the core's own immunity
    // check that refuses its cast) fishes for nothing: the other nuke goes.
    bool NukeCannotHit(AiObjectContext* context, Unit* target, char const* nuke)
    {
        if (!target)
            return false;
        uint32 const spellId = context->GetValue<uint32>("spell id", nuke)->Get();
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info || !target->IsImmunedToDamageOrSchool(info->GetSchoolMask()))
            return false;
        // one line per target (the log only)
        thread_local std::unordered_set<uint64> logged;
        if (logged.insert(target->GetGUID().GetRawValue() ^ spellId).second)
            LOG_INFO("playerbots", "[lead1061] {} is immune to {}: the other nuke goes", target->GetName(), nuke);
        return true;
    }
}

// [rot38] b179 rfc-h 5C (run 20261101-182149): the balance druid's own "medium mana" Innervate went out 10 times as
// "own_decision external: an external without a leader assignment". In a led group Innervate is the leader's
// external ("leader external"); a led healer keeps its own mana Innervate (the 5A resto druid's mana tool).
bool CastInnervateAction::isUseful()
{
    if (Leader::ExecutorMember(bot) && !PlayerbotAI::IsHeal(bot))
        return false;

    return CastSpellAction::isUseful();
}

bool BearSaveLeaderOwned(Player* bot, char const* spell)
{
    if (!PlayerbotAI::IsTank(bot) || !Leader::LedBot(bot))
        return false;

    // one line per bot and 10 s (the log only; the hold itself is the led state)
    constexpr uint32 LOG_MS = 10 * IN_MILLISECONDS;
    thread_local std::unordered_map<ObjectGuid, uint32> logged;
    uint32 const now = getMSTime();
    uint32& last = logged[bot->GetGUID()];
    if (!last || getMSTimeDiff(last, now) >= LOG_MS)
    {
        last = now ? now : 1;
        LOG_DEBUG("playerbots", "[rot371] {}: {} held at {:.0f} % health: a led bear's save is the leader's External",
                  bot->GetName(), spell, bot->GetHealthPct());
    }
    return true;
}

bool DruidHasEclipseTalent(Player* bot) { return HasEclipseTalent(bot); }

bool CastWrathAction::isUseful()
{
    bool const lunarReady = LunarProcReady(context, bot);
    if (bot->HasAura(SPELL_ECLIPSE_SOLAR))
        return CastSpellAction::isUseful();

    // Under Lunar Eclipse Starfire is the nuke; outside an eclipse Wrath only fishes for a ready Lunar proc. A druid
    // without the Eclipse talent keeps the plain order (Starfire default above Wrath).
    if ((bot->HasAura(SPELL_ECLIPSE_LUNAR) || (HasEclipseTalent(bot) && !lunarReady)) &&
        !NukeCannotHit(context, GetTarget(), "starfire"))
        return false;

    return CastSpellAction::isUseful();
}

bool CastStarfireAction::isUseful()
{
    bool const lunarReady = LunarProcReady(context, bot);
    // Instant Starfire (Lunar Surge) outdamages the Wrath of the same GCD even inside Solar Eclipse.
    if (bot->HasAura(SPELL_LUNAR_SURGE) || bot->HasAura(SPELL_ECLIPSE_LUNAR))
        return CastSpellAction::isUseful();

    if ((bot->HasAura(SPELL_ECLIPSE_SOLAR) || (HasEclipseTalent(bot) && lunarReady)) &&
        !NukeCannotHit(context, GetTarget(), "wrath"))
        return false;

    return CastSpellAction::isUseful();
}

// Wild Growth only pays off when several players near the target need healing.
namespace
{
    constexpr float WILD_GROWTH_RADIUS = 30.0f;
    constexpr uint8 WILD_GROWTH_MIN_INJURED = 3;
    // [healer_audit_b30] In a five-man two hurt members (tank + one) already pay for Wild Growth, the strongest
    // heal per cast a resto druid has; with 3 the build 30 druids at 80 cast it 4 times in 3520 s of combat
    // against 635 Lifeblooms.
    constexpr uint8 WILD_GROWTH_MIN_INJURED_PARTY = 2;

    uint8 WildGrowthMinInjured(Group* group)
    {
        return group->isRaidGroup() ? WILD_GROWTH_MIN_INJURED : WILD_GROWTH_MIN_INJURED_PARTY;
    }
    constexpr uint32 WILD_GROWTH_CENTER_CACHE_MS = 200;
    // [b31_sd] A member counts as injured for Wild Growth below 95 %, not below the almost-full band (85 %): the
    // guide casts it on cooldown on a group taking damage (GUIDE_AUDIT_HEALERS.md: 0.21/min on bosses). The band
    // that queues it still needs its lowest member below 85 %.
    constexpr float WILD_GROWTH_INJURED_PCT = 95.0f;

    uint8 WildGrowthInjured(Player* bot, Group* group, Unit* center)
    {
        uint8 injured = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsAlive() || !member->IsInMap(center))
                continue;

            if (member->GetHealthPct() >= WILD_GROWTH_INJURED_PCT || HealTriage::IsHealImmune(bot, member))
                continue;

            if (member != center && !member->IsWithinDist(center, WILD_GROWTH_RADIUS))
                continue;

            ++injured;
        }

        return injured;
    }
}

// [heal_triage] HoR heroic Falric 2026-10-25: the lowest member stood 30+ yd from the other three, so Wild Growth on
// it was "isUseful false" with four members at 17-40 %; the member in the middle of the hurt ones is the centre.
Unit* CastWildGrowthOnPartyAction::GetTarget()
{
    Unit* lowest = HealPartyMemberAction::GetTarget();
    Group* group = bot->GetGroup();
    if (!lowest || !group)
        return lowest;

    uint32 const now = getMSTime();
    if (_centerMs && getMSTimeDiff(_centerMs, now) < WILD_GROWTH_CENTER_CACHE_MS)
    {
        Unit* cached = _center ? botAI->GetUnit(_center) : nullptr;
        return cached && cached->IsAlive() ? cached : lowest;
    }

    _centerMs = now;
    _center = lowest->GetGUID();
    uint8 best = WildGrowthInjured(bot, group, lowest);
    if (best >= WildGrowthMinInjured(group))
        return lowest;

    Unit* center = lowest;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == lowest || !member->IsAlive() || !member->IsInMap(bot) ||
            bot->GetDistance(member) > sPlayerbotAIConfig.HealDistance || !bot->IsWithinLOSInMap(member) ||
            HealTriage::IsHealImmune(bot, member))
            continue;

        uint8 const injured = WildGrowthInjured(bot, group, member);
        if (injured > best)
        {
            best = injured;
            center = member;
        }
    }

    _center = center->GetGUID();
    return center;
}

bool CastWildGrowthOnPartyAction::isUseful()
{
    if (!HealPartyMemberAction::isUseful())
        return false;

    Unit* target = GetTarget();
    Group* group = bot->GetGroup();
    if (!target || !group)
        return false;

    return WildGrowthInjured(bot, group, target) >= WildGrowthMinInjured(group);
}

// Swiftmend consumes a Rejuvenation or Regrowth (any druid's); without one the cast only fails.
bool CastPartySwiftmendAction::isUseful()
{
    if (!HealPartyMemberAction::isUseful())
        return false;

    Unit* target = GetTarget();
    return target && (botAI->HasAura("rejuvenation", target) || botAI->HasAura("regrowth", target));
}

Value<Unit*>* CastEntanglingRootsCcAction::GetTargetValue()
{
    return context->GetValue<Unit*>("rti cc target");
}

Value<Unit*>* CastHibernateCcAction::GetTargetValue() { return context->GetValue<Unit*>("rti cc target"); }

Value<Unit*>* CastCycloneCcAction::GetTargetValue() { return context->GetValue<Unit*>("rti cc target"); }

// [rot1] WotLK 5-man play uses Cyclone only as a pull-side hold: a mob in the fight under Cyclone is immune, the tank
// builds no threat on it and it comes back to the druid (strat-h pull 3: 6 Cyclones, 3 aggro losses; the auto-cc
// had held Roots on the same mob, "within a tank's area damage"). In a raid group the ThreatMultiplier gate decides.
bool CastCycloneCcAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    Group* group = bot->GetGroup();
    if (group && !group->isRaidGroup() && target->IsInCombat())
    {
        uint32 const now = getMSTime();
        if (!_loggedMs || getMSTimeDiff(_loggedMs, now) >= LOG_MS)
        {
            _loggedMs = now ? now : 1;
            LOG_INFO("playerbots", "[rot1] [cyclone] {} keeps Cyclone off {} {}: in the fight, a 5-man Cyclone is "
                     "pre-pull only", bot->GetName(), target->GetName(), target->GetEntry());
        }
        return false;
    }

    return CastCrowdControlSpellAction::isUseful();
}

bool CastMoonfireOnTheMoveAction::isUseful()
{
    if (!bot->isMoving() || !CastSpellAction::isUseful())
        return false;

    Unit* target = GetTarget();
    if (!target)
        return false;

    // the druid's own Moonfire only: another druid's does not stack with it and is no reason to wait
    Aura* own = botAI->GetAura("moonfire", target, true);
    return !own || (own->GetDuration() >= 0 && own->GetDuration() < MOONFIRE_TICK_MS);
}

bool CastTyphoonAction::isUseful()
{
    // The knockback scatters mobs off the tank, so in a group it is kept for enemy players.
    Unit* target = GetTarget();
    if (!target || (bot->GetGroup() && !target->IsPlayer()))
        return false;

    bool facingTarget = AI_VALUE2(bool, "facing", "current target");
    bool targetClose  = ServerFacade::instance().IsDistanceLessOrEqualThan(
        AI_VALUE2(float, "distance", GetTargetName()), 15.f);
    return facingTarget && targetClose;
}

bool CastStarfallAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    // Avoid breaking CC
    WorldLocation aoePos = *context->GetValue<WorldLocation>("aoe position");
    Unit* ccTarget = context->GetValue<Unit*>("current cc target")->Get();
    if (ccTarget && ccTarget->IsAlive())
    {
        float dist2d = ServerFacade::instance().GetDistance2d(ccTarget, aoePos.GetPositionX(), aoePos.GetPositionY());
        if (ServerFacade::instance().IsDistanceLessOrEqualThan(dist2d, sPlayerbotAIConfig.AoeRadius))
            return false;
    }

    // Suppress if an unengaged hostile unit is in the stars' reach - casting would pull it.
    // [rot190] The reach is the core's, not 40 yd in 2D: the stars come from the dummy 50286 (rank 1; every rank's
    // dummy the same), effect 0 TARGET_UNIT_SRC_AREA_ENEMY, radius index 10 = 30 yd round the caster, and
    // WorldObjectSpellAreaTargetCheck (Spell.cpp) hits a creature whose exact 3D distance is within that radius plus
    // its combat reach. b218 rfc-h 5C Taragaman (661-711 s): Starfall ready (last cast in pull 27, <= 548 s, 60 s
    // glyphed) and never cast in the 51 s fight - the Searing Blade Cultist / Warlock spawns 34 / 38 yd from the
    // druid's spot (-221.2, 152.1, -19.1), pulled only after the boss (pulls 34-35), held it; sim Starfall 11 % of
    // the fight's damage (489 DPS)
    constexpr float STARFALL_STAR_RADIUS = 30.0f;
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    GuidVector const& nearbyNpcs = AI_VALUE(GuidVector, "possible targets");
    for (ObjectGuid const& guid : nearbyNpcs)
    {
        Unit* unit = botAI->GetUnit(guid);
        // Standard null/world-state guard before touching the unit.
        if (!unit || !unit->IsAlive() || !unit->IsInWorld())
            continue;
        // Already our target — its in-combat flag covers it.
        if (unit == currentTarget)
            continue;
        // Safety net for any hostile-faction trigger creature that carries NON_ATTACKABLE flags.
        if (!bot->IsValidAttackTarget(unit))
            continue;
        // Outside the stars' reach; no pull risk.
        if (bot->GetExactDist(unit) > STARFALL_STAR_RADIUS + unit->GetCombatReach())
            continue;
        // Unengaged mob within range — casting would pull it.
        if (!unit->IsInCombat())
            return false;
    }

    return true;
}

std::vector<NextAction> CastReviveAction::getPrerequisites()
{
    return NextAction::merge({ NextAction("caster form") },
                             ResurrectPartyMemberAction::getPrerequisites());
}

std::vector<NextAction> CastRebirthAction::getPrerequisites()
{
    // No "reach party member to resurrect": that value can point at another resser's corpse, and a combat
    // battle res is only cast in range anyway (see isUseful).
    return NextAction::merge({ NextAction("caster form") }, Action::getPrerequisites());
}

bool CastRebirthAction::isUseful()
{
    return GetTarget() && CastSpellAction::isUseful() &&
           AI_VALUE2(float, "distance", GetTargetName()) <= sPlayerbotAIConfig.SpellDistance;
}

bool CastRebirthAction::Execute(Event event)
{
    Unit* target = GetTarget();
    if (!CastSpellAction::Execute(event))
        return false;

    RebirthFollowUp::Record(botAI, target);
    return true;
}

bool CastBearBattleResAction::isUseful()
{
    Unit* target = GetTarget();
    return target && CastSpellAction::isUseful() && bot->IsWithinDistInMap(target, BearBattleRes::RANGE);
}

bool CastBearBattleResAction::Execute(Event event)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // [b32_brez] logged before the form drops: the bear's state when it chose to go
    uint32 const health = static_cast<uint32>(bot->GetHealthPct());
    uint32 const attackers = static_cast<uint32>(bot->getAttackers().size());

    // the attempt counts before the cast: a failed or interrupted one is not retried on this member for
    // BearBattleRes::RETRY_MS, and "bear battle res reshift" puts the bear back in form either way
    BearBattleRes::RecordAttempt(botAI, target);
    botAI->RemoveShapeshift();

    if (!CastRebirthAction::Execute(event))
    {
        LOG_INFO("playerbots", "[b32_brez] {}: battle rez on {} failed to start (hp {}%, attackers {})",
                 bot->GetName(), target->GetName(), health, attackers);
        return false;
    }

    LOG_INFO("playerbots", "[b32_brez] {}: battle rez on {} (hp {}%, attackers {})", bot->GetName(),
             target->GetName(), health, attackers);
    return true;
}

void RebirthFollowUp::Record(PlayerbotAI* botAI, Unit* target)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    context->GetValue<ObjectGuid>("rebirth target")->Set(target ? target->GetGUID() : ObjectGuid::Empty);
    context->GetValue<uint32>("rebirth time")->Set(getMSTime());
}

Unit* RebirthFollowUp::GetTarget(PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    ObjectGuid const guid = context->GetValue<ObjectGuid>("rebirth target")->Get();
    if (guid.IsEmpty())
        return nullptr;

    if (getMSTimeDiff(context->GetValue<uint32>("rebirth time")->Get(), getMSTime()) > WINDOW_MS)
    {
        context->GetValue<ObjectGuid>("rebirth target")->Set(ObjectGuid::Empty);
        return nullptr;
    }

    Unit* target = botAI->GetUnit(guid);
    Player* bot = botAI->GetBot();
    if (!target || !target->IsAlive() || !target->IsInWorld() || target->GetMapId() != bot->GetMapId())
        return nullptr;

    return target;
}

bool CastSpellOnRebirthTargetAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || target->GetHealthPct() >= RebirthFollowUp::SAFE_HEALTH_PCT || !CastSpellAction::isUseful())
        return false;

    return !isHot || !botAI->HasAura(spell, target, false, true);
}

bool CastBuffOnRebirthTargetAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !CastSpellAction::isUseful())
        return false;

    if (onlyTank && (!target->IsPlayer() || !PlayerbotAI::IsTank(target->ToPlayer())))
        return false;

    return !botAI->HasAura(spell, target) && (coveringBuff.empty() || !botAI->HasAura(coveringBuff, target));
}

std::vector<NextAction> CastBuffOnRebirthTargetAction::getPrerequisites()
{
    return NextAction::merge({ NextAction("caster form") }, CastSpellAction::getPrerequisites());
}

bool CastInnervateOnHealerAction::isPossible()
{
    Unit* target = GetTarget();
    if (!target || !target->IsInWorld())
        return false;

    if (botAI->HasAura("innervate", target))
        return false;

    uint32 spellId = AI_VALUE2(uint32, "spell id", "innervate");
    return spellId && !bot->HasSpellCooldown(spellId);
}

std::vector<NextAction> CastInnervateOnHealerAction::getPrerequisites()
{
    return { NextAction("caster form") };
}

Unit* CastRejuvenationOnNotFullAction::GetTarget()
{
    Group* group = bot->GetGroup();
    MinValueCalculator calc(100);
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead() || player->IsFullHealth())
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.SpellDistance)
        {
            continue;
        }
        if (botAI->HasAura("rejuvenation", player))
        {
            continue;
        }
        calc.probe(player->GetHealthPct(), player);
    }
    return (Unit*)calc.param;
}

bool CastRejuvenationOnNotFullAction::isUseful()
{
    return GetTarget();
}

// --- Blanket HoT actions ---

ObjectGuid BlanketHotTargetValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return ObjectGuid::Empty;

    auto eligible = [&](Player* member) -> bool
    {
        return member && member->IsAlive() &&
               !member->IsGameMaster() &&
               bot->GetDistance2d(member) <= sPlayerbotAIConfig.SpellDistance &&
               !botAI->HasAura(qualifier, member, false, true) &&
               bot->IsWithinLOSInMap(member);
    };

    Player* firstMelee  = nullptr;
    Player* firstRanged = nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!eligible(member))
            continue;

        if (PlayerbotAI::IsTank(member))
            return member->GetGUID();
        else if (!firstMelee && PlayerbotAI::IsMelee(member) && !PlayerbotAI::IsTank(member))
            firstMelee = member;
        else if (!firstRanged && PlayerbotAI::IsRanged(member))
            firstRanged = member;

        if (firstMelee && firstRanged)
            break;
    }

    if (firstMelee)
        return firstMelee->GetGUID();

    if (firstRanged)
        return firstRanged->GetGUID();

    return ObjectGuid::Empty;
}

Unit* CastBlanketHotAction::GetTarget() { return botAI->GetUnit(AI_VALUE2(ObjectGuid, "blanket hot target", spell)); }

bool CastBlanketHotAction::isUseful() { return GetTarget() != nullptr; }

// [rot331] wowsims casts Enrage whenever it is ready: a major cooldown (druid/enrage.go:71 AddMajorCooldown,
// CooldownTypeDPS) that the feral tank APL's autocastOtherCooldowns (row 2, above every spell) uses at once, with no
// rage or health condition - its 0.84 armor scaling is part of the sim's tank model. The [b31_sd] gate (rage below 20,
// health at the almost-full band) came from no source: b220 rfc-h bosses Enrage 0.55 (5H Rtdruae) / 0.66 (5D Rtdruac)
// a minute vs the sim's 1.19 / 1.31, trash 9 of 37 (5H) and 7 of 41 (5D) pulls with an Enrage; pulls opened at 0 rage
// ("feral charge - bear ... not enough power (0 of 50)" on Taragaman, Earthborer, Searing Blade Cultist; "mangle (bear)
// ... not enough power (101 of 150)" after the opener). Only the base buff check stays (not while it is up).
bool CastEnrageAction::isUseful() { return CastBuffSpellAction::isUseful(); }

bool CastEnrageAction::Execute(Event event)
{
    if (!CastBuffSpellAction::Execute(event))
        return false;

    LOG_DEBUG("playerbots", "[rot331] {} casts Enrage on its cooldown (rage {})", bot->GetName(),
              bot->GetPower(POWER_RAGE) / 10);
    return true;
}
