/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RtiValue.h"
#include "Group.h"
#include "MobRoles.h"
#include "Pet.h"
#include "Playerbots.h"
#include "RtiTargetValue.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TargetValue.h"
#include "Timer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

RtiValue::RtiValue(PlayerbotAI* botAI) : ManualSetValue<std::string>(botAI, "skull", "rti") {}

std::string const RtiValue::Save() { return value; }

bool RtiValue::Load(std::string const text)
{
    value = text;
    return true;
}

CrowdControlRole const* GetCrowdControlRole(uint8 classId)
{
    // Freezing Arrow throws the trap onto the mob (level 80); below that the Freezing Trap goes at the hunter's feet
    // and Distracting Shot brings the mob onto it.
    static constexpr uint32 SPELL_FREEZING_ARROW_EFFECT = 60210;
    static constexpr uint32 SPELL_FREEZING_TRAP_EFFECT = 3355;  // rank 1: creature type and immunity checks only
    static CrowdControlRole const mage = {"moon", {{"polymorph"}}};
    static CrowdControlRole const hunter = {"square",
                                            {{"freezing arrow", SPELL_FREEZING_ARROW_EFFECT},
                                             {"wyvern sting"},
                                             {"freezing trap", SPELL_FREEZING_TRAP_EFFECT, CC_FLAG_TRAP},
                                             {"scare beast", 0, CC_FLAG_FEAR}}};
    static CrowdControlRole const warlock = {
        "diamond", {{"banish"}, {"seduction", 0, CC_FLAG_PET}, {"fear", 0, CC_FLAG_FEAR}}};
    static CrowdControlRole const priest = {"circle", {{"shackle undead"}}};
    static CrowdControlRole const druid = {"triangle", {{"hibernate"}, {"entangling roots", 0, CC_FLAG_MELEE_ONLY}}};
    static CrowdControlRole const rogue = {"star", {{"sap", 0, CC_FLAG_PRE_PULL}}};
    // No raid convention for Hex; star is free in practice, since Sap is never marked once the pull is on.
    static CrowdControlRole const shaman = {"star", {{"hex"}}};
    // Circle is the priest's, who in a 5-man mostly heals and then leaves crowd control to the dps.
    static CrowdControlRole const paladin = {"circle", {{"repentance"}, {"turn evil", 0, CC_FLAG_FEAR}}};

    switch (classId)
    {
        case CLASS_MAGE:
            return &mage;
        case CLASS_HUNTER:
            return &hunter;
        case CLASS_WARLOCK:
            return &warlock;
        case CLASS_PRIEST:
            return &priest;
        case CLASS_DRUID:
            return &druid;
        case CLASS_ROGUE:
            return &rogue;
        case CLASS_SHAMAN:
            return &shaman;
        case CLASS_PALADIN:
            return &paladin;
        default:
            return nullptr;
    }
}

namespace
{
    constexpr float FEAR_SAFE_TARGET_RADIUS = 30.0f;
    constexpr float FEAR_SAFE_CASTER_RADIUS = 45.0f;  // FleeingMovementGenerator stops 28-43 yd from the caster
    constexpr float FEAR_SAFE_FLOOR = 10.0f;
    constexpr uint32 AUTO_CC_DANGEROUS_PACK_SIZE = 3;
    constexpr uint32 KILL_TARGET_GUARD_LOG_MS = 10 * IN_MILLISECONDS;

    std::string DefaultCcIcon(PlayerbotAI* botAI)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        CrowdControlRole const* role = bot ? GetCrowdControlRole(bot->getClass()) : nullptr;
        return role ? role->icon : "moon";
    }

    // The rank the bot casts; a pet spell only while the pet is out and knows it.
    uint32 KnownSpellId(PlayerbotAI* botAI, CrowdControlOption const& option)
    {
        uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", option.spell)->Get();
        if (!spellId || !(option.flags & CC_FLAG_PET))
            return spellId;

        Pet* pet = botAI->GetBot()->GetPet();
        return pet && pet->IsAlive() && pet->HasSpell(spellId) ? spellId : 0;
    }

    bool IsPrePullOnly(CrowdControlOption const& option, SpellInfo const* castInfo)
    {
        return (option.flags & CC_FLAG_PRE_PULL) || castInfo->HasAttribute(SPELL_ATTR1_ONLY_PEACEFUL_TARGETS);
    }
}

SpellInfo const* GetCrowdControlHoldInfo(PlayerbotAI* botAI, CrowdControlOption const& option)
{
    uint32 const spellId = KnownSpellId(botAI, option);
    if (!spellId)
        return nullptr;

    return sSpellMgr->GetSpellInfo(option.holdSpell ? option.holdSpell : spellId);
}

uint64 GetCrowdControlMechanics(SpellInfo const* holdInfo)
{
    return holdInfo ? holdInfo->GetAllEffectsMechanicMask() : 0;
}

bool CanCrowdControl(PlayerbotAI* botAI, CrowdControlOption const& option, Unit* target, bool prePull)
{
    Player* bot = botAI->GetBot();
    uint32 const spellId = KnownSpellId(botAI, option);
    SpellInfo const* castInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    SpellInfo const* holdInfo = castInfo && option.holdSpell ? sSpellMgr->GetSpellInfo(option.holdSpell) : castInfo;
    if (!castInfo || !holdInfo || !target || !target->IsAlive())
        return false;

    if (IsPrePullOnly(option, castInfo) && (!prePull || bot->IsInCombat() || target->IsInCombat()))
        return false;

    if (!castInfo->CheckTargetCreatureType(target) || !holdInfo->CheckTargetCreatureType(target))
        return false;

    if (target->IsImmunedToSpell(holdInfo))
        return false;

    Creature* creature = target->ToCreature();
    if (creature && creature->HasMechanicTemplateImmunity(GetCrowdControlMechanics(holdInfo)))
        return false;

    if ((option.flags & CC_FLAG_MELEE_ONLY) && (MobRoles::IsCaster(target) || MobRoles::IsHealer(target)))
        return false;

    return !(option.flags & CC_FLAG_FEAR) || IsFearSafe(botAI, target);
}

CrowdControlOption const* FindCrowdControl(PlayerbotAI* botAI, Unit* target, bool prePull)
{
    CrowdControlRole const* role = GetCrowdControlRole(botAI->GetBot()->getClass());
    if (!role)
        return nullptr;

    for (CrowdControlOption const& option : role->options)
    {
        if (!option.spell)
            break;

        if (CanCrowdControl(botAI, option, target, prePull))
            return &option;
    }

    return nullptr;
}

bool KnowsCrowdControl(PlayerbotAI* botAI, bool prePull)
{
    CrowdControlRole const* role = GetCrowdControlRole(botAI->GetBot()->getClass());
    if (!role)
        return false;

    for (CrowdControlOption const& option : role->options)
    {
        if (!option.spell)
            break;

        uint32 const spellId = KnownSpellId(botAI, option);
        SpellInfo const* castInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (castInfo && (prePull || !IsPrePullOnly(option, castInfo)))
            return true;
    }

    return false;
}

uint64 GetKnownCrowdControlMechanics(PlayerbotAI* botAI)
{
    CrowdControlRole const* role = GetCrowdControlRole(botAI->GetBot()->getClass());
    uint64 mechanics = 0;
    if (!role)
        return mechanics;

    for (CrowdControlOption const& option : role->options)
    {
        if (!option.spell)
            break;

        mechanics |= GetCrowdControlMechanics(GetCrowdControlHoldInfo(botAI, option));
    }

    return mechanics;
}

bool IsHeldByCrowdControl(Unit* unit, uint64 mechanics)
{
    if (!unit || !unit->IsAlive())
        return false;

    return IsCrowdControlled(unit) || (mechanics && unit->HasAuraWithMechanic(mechanics));
}

bool IsCrowdControlledMobNear(PlayerbotAI* botAI, Unit const* center, float range)
{
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->GetExactDist2d(center) <= range && IsCrowdControlled(unit))
            return true;
    }

    return false;
}

bool IsFearSafe(PlayerbotAI* botAI, Unit* center)
{
    Player* bot = botAI->GetBot();
    if (!center)
        return false;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("all targets")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || creature == center || !creature->IsAlive() || creature->IsInCombat() ||
            !creature->HasReactState(REACT_AGGRESSIVE) || !bot->IsHostileTo(creature))
            continue;

        if (std::fabs(creature->GetPositionZ() - center->GetPositionZ()) > FEAR_SAFE_FLOOR)
            continue;

        if (creature->GetExactDist2d(center) <= FEAR_SAFE_TARGET_RADIUS ||
            creature->GetExactDist2d(bot) <= FEAR_SAFE_CASTER_RADIUS)
            return false;
    }

    return true;
}

bool IsGroupKillTarget(Player* bot, Unit* unit, char const** reason)
{
    if (reason)
        *reason = nullptr;

    if (!unit)
        return false;

    auto const attacks = [unit](Player* member)
    {
        if (!member->IsAlive() || member->GetMap() != unit->GetMap())
            return false;

        Pet* pet = member->GetPet();
        return member->GetVictim() == unit || (pet && pet->IsAlive() && pet->GetVictim() == unit);
    };

    char const* why = nullptr;
    ObjectGuid const guid = unit->GetGUID();
    Group* group = bot->GetGroup();
    if (!group)
    {
        if (attacks(bot))
            why = "own target";
    }
    else if (group->GetTargetIcon(RtiTargetValue::skullIndex) == guid)
        why = "skull";
    else if (group->GetTargetIcon(RtiTargetValue::crossIndex) == guid)
        why = "cross";
    else
    {
        Player* mainTank = ObjectAccessor::FindPlayer(PlayerbotAI::GetMainTankGuid(group));
        if (mainTank && mainTank->IsAlive() && mainTank->GetVictim() == unit)
            why = "main tank target";

        // A mob the group marked for crowd control (any icon but skull and cross) is refused only for a member's
        // own swings, not for a pet or guardian on it: those pick their targets themselves (Feral Spirit, the
        // Water Elemental) and kept every lackey "attacked" for good (mgt-h 2026-10-25 Delrissa: no CC landed).
        bool ccMarked = false;
        for (uint8 icon = 0; icon < TARGETICONCOUNT && !ccMarked; ++icon)
            ccMarked = icon != RtiTargetValue::skullIndex && icon != RtiTargetValue::crossIndex &&
                       group->GetTargetIcon(icon) == guid;

        for (GroupReference* ref = group->GetFirstMember(); ref && !why; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;
            if (ccMarked ? (member->IsAlive() && member->GetVictim() == unit) : attacks(member))
                why = "attacked by a member";
        }
    }

    if (reason)
        *reason = why;

    return why != nullptr;
}

bool RefuseCrowdControlOnKillTarget(PlayerbotAI* botAI, Unit* unit, std::string const& spell)
{
    Player* bot = botAI->GetBot();
    char const* reason = nullptr;
    if (!IsGroupKillTarget(bot, unit, &reason))
        return false;

    struct LastLog
    {
        ObjectGuid target;
        uint32 ms = 0;
    };

    // Bots of different maps run on different threads.
    static std::mutex lock;
    static std::unordered_map<ObjectGuid, LastLog> logged;  // by bot
    uint32 const now = getMSTime();
    bool log = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        LastLog& last = logged[bot->GetGUID()];
        if (last.target != unit->GetGUID() || getMSTimeDiff(last.ms, now) >= KILL_TARGET_GUARD_LOG_MS)
        {
            last.target = unit->GetGUID();
            last.ms = now;
            log = true;
        }
    }

    if (log)
        LOG_INFO("playerbots", "[auto cc] {} refuses {} on {} ({}): the group kills it now ({})", bot->GetName(),
                 spell, unit->GetName(), unit->GetGUID().ToString(), reason);

    return true;
}

bool AutoCcHardPackValue::Calculate()
{
    uint32 const minPack = sPlayerbotAIConfig.AutoCcMarkMinPackSize;
    if (!minPack)
        return false;

    uint32 mobs = 0;
    uint32 healers = 0;
    uint32 casters = 0;
    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->IsAlive())
            continue;

        // A boss fight belongs to its encounter script, not to trash crowd control.
        if (creature->IsDungeonBoss() || creature->isWorldBoss())
            return false;

        ++mobs;
        if (MobRoles::IsHealer(creature))
            ++healers;
        else if (MobRoles::IsCaster(creature))
            ++casters;
    }

    return mobs >= minPack ||
           (mobs >= std::min(minPack, AUTO_CC_DANGEROUS_PACK_SIZE) && (healers > 0 || casters >= 2));
}

namespace
{
    // [rot_b31_cc] crowd control back-off (NoteCrowdControlBreak)
    constexpr uint32 CC_BREAK_BACKOFF_MS = 15 * IN_MILLISECONDS;
    constexpr uint8 CC_BREAKS_PER_PULL = 2;
    constexpr uint32 CC_BREAK_FORGET_MS = 180 * IN_MILLISECONDS;
    constexpr size_t CC_BREAK_PRUNE_SIZE = 256;
    // Swipe and Whirlwind 8 yd, Thunder Clap and Blood Boil 10 yd, Consecration and Death and Decay at the tank
    constexpr float GROUP_MELEE_AOE_RADIUS = 10.0f;

    struct CrowdControlBreak
    {
        uint32 instanceId = 0;
        uint32 lastBreakMs = 0;
        uint8 breaks = 0;
    };

    struct CrowdControlBreakRegistry
    {
        std::mutex lock;  // bots of different maps run on different threads
        std::unordered_map<ObjectGuid, CrowdControlBreak> breaks;
        std::atomic<uint32> count{0};
    };

    CrowdControlBreakRegistry& GetCrowdControlBreaks()
    {
        static CrowdControlBreakRegistry registry;
        return registry;
    }
}

void NoteCrowdControlBreak(Unit* unit, uint32 heldMs)
{
    if (!unit)
        return;

    uint32 const now = getMSTime();
    CrowdControlBreakRegistry& registry = GetCrowdControlBreaks();
    uint8 breaks = 0;
    {
        std::lock_guard<std::mutex> guard(registry.lock);
        if (registry.breaks.size() >= CC_BREAK_PRUNE_SIZE)
        {
            for (auto itr = registry.breaks.begin(); itr != registry.breaks.end();)
            {
                if (getMSTimeDiff(itr->second.lastBreakMs, now) > CC_BREAK_FORGET_MS)
                    itr = registry.breaks.erase(itr);
                else
                    ++itr;
            }
        }

        CrowdControlBreak& entry = registry.breaks[unit->GetGUID()];
        if (entry.instanceId != unit->GetInstanceId() || getMSTimeDiff(entry.lastBreakMs, now) > CC_BREAK_FORGET_MS)
            entry = CrowdControlBreak();

        entry.instanceId = unit->GetInstanceId();
        entry.lastBreakMs = now;
        entry.breaks = uint8(std::min<uint32>(entry.breaks + 1u, 255u));
        breaks = entry.breaks;
        registry.count = uint32(registry.breaks.size());
    }

    LOG_INFO("playerbots", "[cc backoff] {} ({}): crowd control broke after {} ms, break {} of the pull; not "
             "controlled again {}", unit->GetName(), unit->GetGUID().GetCounter(), heldMs, uint32(breaks),
             breaks >= CC_BREAKS_PER_PULL ? std::string("for the rest of its fight")
                                          : Acore::StringFormat("for {} s", CC_BREAK_BACKOFF_MS / IN_MILLISECONDS));
}

bool IsCrowdControlBackedOff(Unit* unit)
{
    CrowdControlBreakRegistry& registry = GetCrowdControlBreaks();
    if (!unit || !registry.count.load(std::memory_order_relaxed))
        return false;

    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(registry.lock);
    auto const itr = registry.breaks.find(unit->GetGUID());
    if (itr == registry.breaks.end())
        return false;

    CrowdControlBreak const& entry = itr->second;
    uint32 const since = getMSTimeDiff(entry.lastBreakMs, now);
    // a mob out of its fight (it died, or evaded) starts the next pull clean
    if (entry.instanceId != unit->GetInstanceId() || since > CC_BREAK_FORGET_MS || !unit->IsInCombat())
    {
        registry.breaks.erase(itr);
        registry.count = uint32(registry.breaks.size());
        return false;
    }

    return since < CC_BREAK_BACKOFF_MS || entry.breaks >= CC_BREAKS_PER_PULL;
}

bool CrowdControlWouldBreak(PlayerbotAI* botAI, Unit* unit, char const** why)
{
    char const* reason = nullptr;
    Player* bot = botAI->GetBot();
    if (!unit)
        return false;

    // the next tick of a damage over time breaks it (Living Bomb, Flame Shock, Corruption, a bleed)
    if (unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) || unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT) ||
        unit->HasAuraType(SPELL_AURA_PERIODIC_LEECH))
        reason = "a damage over time ticks on it";

    if (!reason)
    {
        // inside a ground area spell of the group: its persistent area aura sits on the mob
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            Aura const* aura = application ? application->GetBase() : nullptr;
            if (!aura || aura->GetType() != DYNOBJ_AURA_TYPE)
                continue;

            Unit* caster = aura->GetCaster();
            Player* owner = caster ? caster->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (owner && (owner == bot || owner->IsInSameRaidWith(bot)))
            {
                reason = "inside a ground area spell of the group";
                break;
            }
        }
    }

    Group* group = bot->GetGroup();
    for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref && !reason; ref = ref->next())
    {
        Player* member = ref->GetSource();
        // the caster itself (a Retribution paladin's Repentance) keeps its own area damage off its mark
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != unit->GetMap() ||
            (!PlayerbotAI::IsTank(member) && !PlayerbotAI::IsMelee(member)))
            continue;

        if (member->GetExactDist2d(unit) <= GROUP_MELEE_AOE_RADIUS + unit->GetCombatReach())
            reason = "within a tank's or a melee's area damage";
    }

    if (why)
        *why = reason;

    return reason != nullptr;
}

bool IsUnbreakableCrowdControl(PlayerbotAI* botAI, CrowdControlOption const& option)
{
    return (GetCrowdControlMechanics(GetCrowdControlHoldInfo(botAI, option)) & (1ULL << MECHANIC_BANISH)) != 0;
}

bool HasCrowdControlIcon(Player* bot, Unit* unit)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group || !unit)
        return false;

    ObjectGuid const guid = unit->GetGUID();
    for (uint8 icon = 0; icon < TARGETICONCOUNT; ++icon)
    {
        if (icon != RtiTargetValue::skullIndex && icon != RtiTargetValue::crossIndex &&
            group->GetTargetIcon(icon) == guid)
            return true;
    }

    return false;
}

Unit* GetAutoCcTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    AiObjectContext* context = botAI->GetAiObjectContext();
    AutoCcMark const& mark = context->GetValue<AutoCcMark>("auto cc mark")->RefGet();
    // [rot_b31_cc] a hold that broke early is not cast again; the mark goes (StaleAutoCcMarkTrigger)
    if (!group || !mark.target || mark.broken)
        return nullptr;

    int32 const index = RtiTargetValue::GetRtiIndex(context->GetValue<std::string>("rti cc")->Get());
    if (index < 0 || group->GetTargetIcon(uint8(index)) != mark.target)
        return nullptr;

    Unit* unit = botAI->GetUnit(mark.target);
    if (!unit || !unit->IsAlive() || IsHeldByCrowdControl(unit, mark.mechanics))
        return nullptr;

    return unit;
}

RtiCcValue::RtiCcValue(PlayerbotAI* botAI) : ManualSetValue<std::string>(botAI, DefaultCcIcon(botAI), "rti cc") {}

std::string const RtiCcValue::Save() { return value; }

bool RtiCcValue::Load(std::string const text)
{
    value = text;
    return true;
}
