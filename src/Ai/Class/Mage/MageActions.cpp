/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MageActions.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RtiValue.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "UseItemAction.h"
#include "BuyAction.h"
#include "HealthTriggers.h"
#include "Map.h"
#include "ObjectMgr.h"  // [b31_deep] mana gem use spells
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <cmath>
#include <mutex>
#include <set>
#include <string_view>

namespace
{
    // [rot_b31_cc] a crowd-controlled mob this close to an area spell's centre is inside it (10 yd areas, a margin)
    constexpr float MAGE_AOE_CC_CHECK_RANGE = 12.0f;
}

std::vector<NextAction> CastMoltenArmorAction::getAlternatives()
{
    if (!botAI->HasSpell("molten armor"))
        return NextAction::merge({ NextAction("mage armor") }, CastBuffSpellAction::getAlternatives());

    return CastBuffSpellAction::getAlternatives();
}

std::vector<NextAction> CastMageArmorAction::getAlternatives()
{
    if (!botAI->HasSpell("mage armor"))
        return NextAction::merge({ NextAction("ice armor") }, CastBuffSpellAction::getAlternatives());

    return CastBuffSpellAction::getAlternatives();
}

// [b31_deep] Frost Armor until Ice Armor (level 30) is learned
std::vector<NextAction> CastIceArmorAction::getAlternatives()
{
    if (!botAI->HasSpell("ice armor"))
        return NextAction::merge({ NextAction("frost armor") }, CastBuffSpellAction::getAlternatives());

    return CastBuffSpellAction::getAlternatives();
}

namespace
{
    // [b31_deep] the gem in the bags and its use spell off cooldown (the gems share one category cooldown). Without
    // the cooldown check "use mana sapphire" returned false 39 135 times in two days below 65 % mana.
    bool ManaGemReady(PlayerbotAI* botAI, Player* bot, uint32 itemId)
    {
        if (!botAI->GetAiObjectContext()->GetValue<bool>("combat", "self target")->Get() ||
            bot->GetItemCount(itemId, false) == 0)
            return false;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        uint32 const spellId = (proto && proto->Spells[0].SpellId > 0) ? uint32(proto->Spells[0].SpellId) : 0;
        return !spellId || !bot->HasSpellCooldown(spellId);
    }
}

bool UseManaSapphireAction::isUseful() { return ManaGemReady(botAI, botAI->GetBot(), 33312); }  // Mana Sapphire

bool UseManaEmeraldAction::isUseful() { return ManaGemReady(botAI, botAI->GetBot(), 22044); }  // Mana Emerald

bool UseManaRubyAction::isUseful() { return ManaGemReady(botAI, botAI->GetBot(), 8008); }  // Mana Ruby

bool UseManaCitrineAction::isUseful() { return ManaGemReady(botAI, botAI->GetBot(), 8007); }  // Mana Citrine

bool UseManaJadeAction::isUseful() { return ManaGemReady(botAI, botAI->GetBot(), 5513); }  // Mana Jade

bool UseManaAgateAction::isUseful() { return ManaGemReady(botAI, botAI->GetBot(), 5514); }  // Mana Agate

bool CastFrostNovaAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    // creature_immunities is indexed by mechanic (Creature::IsImmunedToSpell); the nova's damage breaks any
    // crowd control within its 10 yd
    if (!target || !target->IsInWorld() || target->isFrozen() ||
        (target->ToCreature() &&
         target->ToCreature()->HasMechanicTemplateImmunity((1ULL << MECHANIC_FREEZE) | (1ULL << MECHANIC_ROOT))) ||
        IsCrowdControlledMobNear(botAI, bot, 12.0f))
    {
        return false;
    }

    return ServerFacade::instance().IsDistanceLessOrEqualThan(
        AI_VALUE2(float, "distance", GetTargetName()), 10.f);
}

// [b31_deep] the checks CanCastSpell would do, without its aurastate check (Fingers of Frost makes the target count
// as frozen); the power cost explicitly, as CanCastSpell ignores it.
bool CastDeepFreezeAction::isPossible()
{
    constexpr uint32 SPELL_DEEP_FREEZE = 44572;
    if (!bot->HasSpell(SPELL_DEEP_FREEZE) || bot->HasSpellCooldown(SPELL_DEEP_FREEZE))
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_DEEP_FREEZE);
    if (!info || bot->GetGlobalCooldownMgr().HasGlobalCooldown(info) ||
        int32(bot->GetPower(POWER_MANA)) < info->CalcPowerCost(bot, info->GetSchoolMask()))
        return false;

    Unit* target = GetTarget();
    // the range with Arctic Reach (spell mods applied for the caster)
    return target && target->IsAlive() && bot->IsWithinCombatRange(target, info->GetMaxRange(false, bot));
}

// [ranged_audit_b30] Deep Freeze deals damage only to a target permanently immune to stuns (71761, Deep Freeze
// Immunity State: Creature::HasMechanicTemplateImmunity(MECHANIC_STUN)); on other mobs it is a 5 s stun. Build 30:
// frost mages at 80 spent 33 Fingers of Frost GCDs on it on trash for 1 % of their damage. In a group the stun is
// not worth the proc, Frostbolt with Shatter takes it (FrostMageStrategy "fingers of frost"); solo it still stuns.
bool CastDeepFreezeAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    if (!bot->GetGroup())
        return true;

    Unit* target = GetTarget();
    Creature* creature = target ? target->ToCreature() : nullptr;
    return creature && creature->HasMechanicTemplateImmunity(1ULL << MECHANIC_STUN);
}

bool CastConeOfColdAction::isUseful()
{
    bool facingTarget = AI_VALUE2(bool, "facing", "current target");
    bool targetClose = ServerFacade::instance().IsDistanceLessOrEqualThan(
        AI_VALUE2(float, "distance", GetTargetName()), 10.f);

    return facingTarget && targetClose && !IsCrowdControlledMobNear(botAI, bot, MAGE_AOE_CC_CHECK_RANGE);
}

// [rot_b31_cc] Polymorph loop (build 30: 111 Polymorphs from one Fire mage in a level-80 wave): its own area damage
// broke the sheep it had just cast. Living Bomb explodes for 10 yd round its target, Flamestrike and Blizzard cover
// 8 yd round theirs: none goes on a target with a crowd-controlled mob that near.
// [b31_deep] CastDebuffSpellAction::isUseful, not CastAuraSpellAction: the 12 s target lifetime check (the bomb does
// not explode when its target dies, spell_mage_living_bomb::AfterRemove)
bool CastLivingBombAction::isUseful()
{
    Unit* target = GetTarget();
    return CastDebuffSpellAction::isUseful() &&
           !(target && IsCrowdControlledMobNear(botAI, target, MAGE_AOE_CC_CHECK_RANGE));
}

bool CastLivingBombOnAttackersAction::isUseful()
{
    Unit* target = GetTarget();
    return CastDebuffSpellAction::isUseful() &&
           !(target && IsCrowdControlledMobNear(botAI, target, MAGE_AOE_CC_CHECK_RANGE));
}

bool CastBlizzardAction::isUseful()
{
    Unit* target = GetTarget();
    return CastSpellAction::isUseful() && !(target && IsCrowdControlledMobNear(botAI, target, MAGE_AOE_CC_CHECK_RANGE));
}

bool CastFlamestrikeAction::isUseful()
{
    Unit* target = GetTarget();
    return CastDebuffSpellAction::isUseful() &&
           !(target && IsCrowdControlledMobNear(botAI, target, MAGE_AOE_CC_CHECK_RANGE));
}

bool CastDragonsBreathAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    bool facingTarget = AI_VALUE2(bool, "facing", "current target");
    bool targetClose = bot->IsWithinCombatRange(target, 10.0f);
    return facingTarget && targetClose && !IsCrowdControlledMobNear(botAI, bot, MAGE_AOE_CC_CHECK_RANGE);
}

bool CastBlastWaveAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    bool targetClose = bot->IsWithinCombatRange(target, 10.0f);
    return targetClose && !IsCrowdControlledMobNear(botAI, bot, MAGE_AOE_CC_CHECK_RANGE);
}

bool CastArcaneExplosionAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    return CastSpellAction::isUseful() && bot->IsWithinCombatRange(target, 10.0f) &&
           !IsCrowdControlledMobNear(botAI, bot, MAGE_AOE_CC_CHECK_RANGE);
}

Unit* CastFocusMagicOnPartyAction::GetTarget()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    // Raid utility coordinator: the best caster nobody else's Focus Magic is planned for.
    Unit* assigned = AI_VALUE(Unit*, "utility buff target");
    if (assigned && assigned != bot && !assigned->HasAura(54646))  // Focus Magic
        return assigned;

    Unit* casterDps = nullptr;
    Unit* healer = nullptr;
    Unit* target = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            bot->GetDistance(member) > sPlayerbotAIConfig.SpellDistance || member->HasAura(54646))  // Focus Magic
        {
            continue;
        }

        if (member->getClass() == CLASS_MAGE)
            return member;

        if (!casterDps && PlayerbotAI::IsCaster(member) && PlayerbotAI::IsDps(member))
            casterDps = member;

        if (!healer && PlayerbotAI::IsHeal(member))
            healer = member;

        if (!target)
            target = member;
    }

    if (casterDps)
        return casterDps;

    if (healer)
        return healer;

    return target;
}

bool CastBlinkBackAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    bot->SetOrientation(bot->GetAngle(target) + M_PI);
    return CastSpellAction::Execute(event);
}

// [conjure_outside_instances] see MageActions.h
namespace
{
// The group leader is this many yards away and still walking off: the mage follows, conjuring waits.
constexpr float CONJURE_LEADER_WALKS_OFF_YD = 8.0f;

std::mutex conjureSkipLogLock;
std::set<std::pair<uint32, uint32>> conjureSkipLogged;  // (bot guid low, instance id): one line per run

void LogConjureSkipOnce(Player* bot, char const* what)
{
    Map* map = bot->GetMap();
    std::pair<uint32, uint32> const key(bot->GetGUID().GetCounter(), map ? map->GetInstanceId() : 0);
    {
        std::lock_guard<std::mutex> guard(conjureSkipLogLock);
        if (!conjureSkipLogged.insert(key).second)
            return;
    }
    LOG_INFO("playerbots", "[conjure_outside_instances] {} skips {} in instance map {} (id {}): conjure before the run, "
             "the own stock is not empty or the group does not rest (logged once per run)",
             bot->GetName(), what, map ? map->GetId() : 0, key.second);
}

bool HasManaGem(Player* bot)
{
    static uint32 const gemIds[] = {33312, 22044, 8008, 8007, 5513, 5514};
    for (uint32 gem : gemIds)
        if (bot->HasItemCount(gem, 1))
            return true;
    return false;
}
}  // namespace

bool MageConjureAllowed(PlayerbotAI* botAI, Player* bot, bool ownStockEmpty, char const* what)
{
    if (!CONJURE_OUTSIDE_INSTANCES || !bot || bot->IsInCombat())
        return true;

    Map* map = bot->GetMap();
    if (map && map->Instanceable() && !map->IsBattlegroundOrArena())
    {
        bool const groupRests = RestState::GroupResting(bot) || RestState::IsEating(bot) || RestState::IsDrinking(bot);
        if (ownStockEmpty && groupRests)
            return true;
        // [dun198] its own water (or food) gone in the run: it conjures for itself whatever the group does - with no
        // drink and no conjure it waits on spirit regen alone and the group does not wait for it (b436 dm-n five 7:
        // Feeli "no drink in the bags, spirit regen only" at 6 % mana, knowing Conjure Water)
        std::string_view const kind(what);
        if (ownStockEmpty && (kind == "conjure water" || kind == "conjure food"))
        {
            static std::mutex selfLogLock;
            static std::set<std::pair<uint32, uint32>> selfLogged;  // (bot, instance): once per run
            bool first = false;
            {
                std::lock_guard<std::mutex> guard(selfLogLock);
                first = selfLogged.insert({bot->GetGUID().GetCounter(), map->GetInstanceId()}).second;
            }
            if (first)
                LOG_INFO("playerbots", "[dun198] {} conjures for itself in instance map {} ({}): its own stock is gone",
                         bot->GetName(), map->GetId(), what);
            return true;
        }

        LogConjureSkipOnce(bot, what);
        return false;
    }

    // Open world: never while the group leader walks away (the conjure is 3 s standing still).
    Player* leader = botAI->GetGroupLeader();
    if (leader && leader != bot && leader->IsInWorld() && leader->GetMap() == bot->GetMap() && leader->isMoving() &&
        bot->GetDistance(leader) > CONJURE_LEADER_WALKS_OFF_YD)
        return false;

    return true;
}

bool CastConjureFoodAction::isUseful()
{
    return CastBuffSpellAction::isUseful() &&
           MageConjureAllowed(botAI, bot, !CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD), "conjure food");
}

bool CastConjureRefreshmentAction::isUseful()
{
    return CastBuffSpellAction::isUseful() &&
           MageConjureAllowed(botAI, bot, !CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD), "conjure refreshment");
}

bool CastConjureWaterAction::isUseful()
{
    return CastBuffSpellAction::isUseful() &&
           MageConjureAllowed(botAI, bot, !CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK), "conjure water");
}

bool CastConjureManaGemAction::isUseful()
{
    return CastBuffSpellAction::isUseful() && MageConjureAllowed(botAI, bot, !HasManaGem(bot), "conjure mana gem");
}

bool CastRitualOfRefreshmentAction::isUseful()
{
    // a table is a stock for the whole group: never in an instance (the own-stock exception is for the mage itself)
    return CastSpellAction::isUseful() && MageConjureAllowed(botAI, bot, false, "ritual of refreshment");
}
