/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HunterActions.h"
#include "Event.h"
#include "GenericSpellActions.h"
#include "HunterTriggers.h"         // [b31_deep] SPELL_LOCK_AND_LOAD_PROC
#include "InterruptValues.h"        // [b31_deep] InterruptCoordination::CanStopCast
#include "Pet.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"  // [b31_deep] ai::raidutil::IsBossUnit
#include "RtiValue.h"               // [b33_aoe_class] IsCrowdControlledMobNear
#include "SpellMgr.h"               // [b31_deep]

constexpr uint32 ARCANE_SHOT_MAX_ARMOR_PEN_RATING = 435;
// [b33_aoe_class] Volley's 8 yd radius / Multi-Shot's jumps, with margin (the mage AoE check uses 12 yd too)
constexpr float HUNTER_AOE_CC_CHECK_RANGE = 12.0f;

// [b33_aoe_class] An area shot beside a sheep or a trapped mob breaks it.
bool CastMultiShotAction::isUseful()
{
    Unit* target = GetTarget();
    return CastSpellAction::isUseful() &&
           !(target && IsCrowdControlledMobNear(botAI, target, HUNTER_AOE_CC_CHECK_RANGE));
}

bool CastVolleyAction::isUseful()
{
    Unit* target = GetTarget();
    return CastSpellAction::isUseful() &&
           !(target && IsCrowdControlledMobNear(botAI, target, HUNTER_AOE_CC_CHECK_RANGE));
}
// [b31_deep] Explosive Shot rank 1 (the Survival talent); Arcane Shot shares its cooldown category (1173, DBC)
constexpr uint32 SPELL_EXPLOSIVE_SHOT_RANK_1 = 53301;
// [b31_deep] Black Arrow's DoT runs 15 s and feeds Lock and Load; a target dying sooner may take the fire trap
constexpr float BLACK_ARROW_MIN_TARGET_LIFETIME = 10.0f;

bool CastViperStingAction::isUseful()
{
    return CastAuraSpellAction::isUseful() && AI_VALUE2(uint8, "mana", "self target") < 50 &&
           AI_VALUE2(uint8, "mana", "current target") >= 30;
}

bool CastAspectOfTheHawkAction::isUseful()
{
    Unit* target = GetTarget();
    return target && !botAI->HasSpell("aspect of the dragonhawk");
}

bool CastArcaneShotAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    // [b31_deep] Survival never fires Arcane Shot (wowsims sv.apl, Warcraft Tavern SV): it shares Explosive Shot's
    // cooldown and the two Lock and Load charges, so each Arcane Shot cost an Explosive Shot (0.34-0.40/min on
    // bosses, each one a lost Lock and Load charge while our Explosive Shot DoT made Explosive Shot useless).
    if (bot->HasSpell(SPELL_EXPLOSIVE_SHOT_RANK_1))
        return false;

    // Arcane damage ignores armor, so a high-ArP build gains more from Steady Shot
    uint32 const armorPenRating =
        bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + static_cast<uint16>(CR_ARMOR_PENETRATION));
    return armorPenRating <= ARCANE_SHOT_MAX_ARMOR_PEN_RATING;
}

// [b31_deep] Black Arrow and the fire traps share cooldown category 1250 (DBC): a trap dropped under a melee mob
// held a ready Black Arrow (the Survival shot that feeds Lock and Load) for 30 s on targets that outlive its DoT.
bool CastExplosiveTrapAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    if (botAI->HasStrategy("trap weave", BOT_STATE_COMBAT))
        return true;

    uint32 const blackArrowId = AI_VALUE2(uint32, "spell id", "black arrow");
    if (!blackArrowId || bot->HasSpellCooldown(blackArrowId))
        return true;

    Unit* target = AI_VALUE(Unit*, "current target");
    float const groupDps = AI_VALUE(float, "estimated group dps");
    return !target || groupDps <= 0.0f || target->GetHealth() / groupDps < BLACK_ARROW_MIN_TARGET_LIFETIME;
}

// [b31_deep]
bool CastMisdirectionOnMainTankAction::isUseful()
{
    if (bot->HasAura(SPELL_MISDIRECTION) || bot->HasAura(SPELL_MISDIRECTION_TRANSFER))
        return false;

    return BuffOnMainTankAction::isUseful();
}

// [b31_deep]
bool CastSilencingShotOnCooldownAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    Unit* target = GetTarget();
    if (!ai::raidutil::IsBossUnit(target))
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "silencing shot");
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo || target->IsImmunedToSpell(spellInfo))
        return false;

    // Still able to silence this boss: keep it for its casts (the "silencing shot" trigger).
    return !InterruptCoordination::CanStopCast(target, spellInfo);
}

// [b31_deep] The highest known rank while our own DoT of it is absent, else the next lower known rank while its own
// DoT is absent. Lower ranks than those two would only spend a free charge on a weaker DoT.
uint32 CastExplosiveShotLockAndLoadAction::PickRank()
{
    Unit* target = GetTarget();
    if (!target)
        return 0;

    ObjectGuid const casterGuid = bot->GetGUID();
    uint8 knownRanks = 0;
    for (uint32 rankId : EXPLOSIVE_SHOT_RANKS)
    {
        if (!bot->HasSpell(rankId))
            continue;

        if (!target->HasAura(rankId, casterGuid))
            return rankId;

        if (++knownRanks == 2)
            break;
    }

    return 0;
}

bool CastExplosiveShotLockAndLoadAction::isUseful()
{
    return bot->HasAura(SPELL_LOCK_AND_LOAD_PROC) && CastSpellAction::isUseful() && PickRank();
}

bool CastExplosiveShotLockAndLoadAction::Execute(Event /*event*/)
{
    uint32 const rankId = PickRank();
    return rankId && botAI->CastSpell(rankId, GetTarget());
}

bool CastImmolationTrapAction::isUseful()
{
    Unit* target = GetTarget();
    return target && !botAI->HasSpell("explosive trap");
}

Value<Unit*>* CastFreezingTrap::GetTargetValue()
{
    return context->GetValue<Unit*>("cc target", "freezing trap");
}

namespace
{
    constexpr uint32 SPELL_FEED_PET = 6991;
    constexpr uint32 FEED_PET_NO_FOOD_RECHECK_MS = 30 * IN_MILLISECONDS;
}

Item* FeedPetAction::FindPetFood(Pet* pet)
{
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && pet->HaveInDiet(proto) && pet->GetCurrentFoodBenefitLevel(proto->ItemLevel))
            return item;
    }

    return nullptr;
}

bool FeedPetAction::isUseful()
{
    Pet* pet = bot->GetPet();
    if (!pet || pet->getPetType() != HUNTER_PET || !pet->IsAlive() || pet->GetHappinessState() == HAPPY)
        return false;

    if (bot->IsInCombat() || !bot->HasSpell(SPELL_FEED_PET) || bot->HasSpellCooldown(SPELL_FEED_PET))
        return false;

    // The bag scan only runs again a while after no food was found.
    return !_noFoodUntil || getMSTime() >= _noFoodUntil;
}

bool FeedPetAction::Execute(Event /*event*/)
{
    Pet* pet = bot->GetPet();
    if (!pet || pet->getPetType() != HUNTER_PET)
        return false;

    Item* food = FindPetFood(pet);
    if (!food)
    {
        _noFoodUntil = getMSTime() + FEED_PET_NO_FOOD_RECHECK_MS;
        return false;
    }

    _noFoodUntil = 0;
    return botAI->CastSpell(SPELL_FEED_PET, pet, food);
}

bool CastAutoShotAction::isUseful()
{
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    if (AI_VALUE(Unit*, "current target") && bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) &&
        bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL)->m_targets.GetUnitTargetGUID() ==
            AI_VALUE(Unit*, "current target")->GetGUID())
    {
        return false;
    }

    return AI_VALUE(uint32, "active spell") != AI_VALUE2(uint32, "spell id", getName());
}

bool CastDisengageAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    // can cast spell check passed in isUseful()
    bot->SetOrientation(bot->GetAngle(target));
    return CastSpellAction::Execute(event);
}

bool CastDisengageAction::isUseful()
{
    return !botAI->HasStrategy("trap weave", BOT_STATE_COMBAT);
}

Value<Unit*>* CastScareBeastCcAction::GetTargetValue()
{
    return context->GetValue<Unit*>("cc target", "scare beast");
}

bool CastScareBeastCcAction::Execute(Event /*event*/)
{
    return botAI->CastSpell("scare beast", GetTarget());
}

bool CastWingClipAction::isUseful()
{
    return CastSpellAction::isUseful() && !botAI->HasAura(spell, GetTarget());
}

std::vector<NextAction> CastWingClipAction::getPrerequisites()
{
    return {};
}
