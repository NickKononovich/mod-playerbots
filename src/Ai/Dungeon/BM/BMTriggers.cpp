/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BMTriggers.h"
#include "AiObjectContext.h"
#include "BMShared.h"
#include "Creature.h"
#include "Playerbots.h"
#include "PullActions.h"

namespace
{
constexpr float FIGHT_SPOT_AT_MEDIVH = 3.0f;  // the tank's fight spot counts as Medivh's hold spot this close to it
constexpr float FETCH_MIN_HEALTH_PCT = 40.0f;
constexpr float DRAG_FOLLOW_RANGE = 40.0f;  // a keeper farther behind the tank does not follow: no drag
}  // namespace

namespace BMShared
{
Unit* GuardianToFetch(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!PlayerbotAI::IsTank(bot) || !bot->IsAlive() || bot->GetHealthPct() <= FETCH_MIN_HEALTH_PCT ||
        !IsEventInProgress(bot))
        return nullptr;

    // The damage dealers break the channels; the tank stays for them only once the shield slipped below
    // TANK_CHANNEL_SHIELD_PCT, and for any add once it runs low. A living keeper keeps its rift summoning: it is
    // fetched whenever Medivh can spare the tank.
    int32 const shield = ShieldPct(bot);
    if ((shield < TANK_CHANNEL_SHIELD_PCT && AnyCorrupting(botAI)) || (shield < FETCH_SHIELD_PCT && FirstThreat(botAI)))
        return nullptr;

    Unit* guardian = NearestGuardian(botAI);
    if (!guardian || guardian->GetVictim() == bot)
        return nullptr;

    // A fight of its own comes first, but not one with the Infinite adds: they come every 15 s per rift and kept the
    // tank home for good (bm-n build 25). They follow the tank off Medivh, where they drain nothing.
    Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (bot->IsInCombat() && current && current->IsAlive() && current != guardian && !IsAddEntry(current->GetEntry()))
        return nullptr;

    return guardian;
}

Unit* GuardianToDrag(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!PlayerbotAI::IsTank(bot) || !bot->IsAlive() || !IsEventInProgress(bot))
        return nullptr;

    if (bot->GetExactDist2d(HOLD_X, HOLD_Y) <= DRAG_ARRIVED)
        return nullptr;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("bm rift guardians")->Get())
    {
        Unit* guardian = botAI->GetUnit(guid);
        // a keeper that stands off at its cast range within GUARDIAN_STANDOFF_RADIUS of Medivh comes no closer however
        // far the tank walks: the damage dealers take it where it stands (BMShared::IsFarGuardian). Farther out it
        // still follows the tank to its cast range, so the drag goes on.
        if (guardian && guardian->IsAlive() && guardian->GetVictim() == bot &&
            guardian->GetExactDist2d(bot) <= DRAG_FOLLOW_RANGE &&
            !(IsStandingOff(bot, guardian) && !IsFarGuardian(bot, guardian)))
            return guardian;
    }
    return nullptr;
}

Unit* BossBuffToRemove(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    uint8 const cls = bot->getClass();
    bool const removesMagic = cls == CLASS_HUNTER || cls == CLASS_SHAMAN || cls == CLASS_MAGE ||
                              (cls == CLASS_PRIEST && !PlayerbotAI::IsHeal(bot));
    bool const removesEnrage = cls == CLASS_HUNTER;
    if ((!removesMagic && !removesEnrage) || !bot->IsAlive() || !IsEventInProgress(bot))
        return nullptr;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("bm rift guardians")->Get())
    {
        Unit* boss = botAI->GetUnit(guid);
        if (!boss || !boss->IsAlive())
            continue;

        uint32 const entry = boss->GetEntry();
        if (removesMagic && (entry == NPC_TEMPORUS || entry == NPC_INFINITE_TIMEREAVER) && boss->HasAura(SPELL_HASTEN))
            return boss;
        if (removesEnrage && entry == NPC_AEONUS && boss->HasAura(SPELL_AEONUS_ENRAGE))
            return boss;
    }
    return nullptr;
}
}  // namespace BMShared

bool BlackMorassMedivhThreatenedTrigger::IsActive()
{
    if (!bot->IsAlive() || PlayerbotAI::IsHeal(bot) || !BMShared::IsEventInProgress(bot))
        return false;

    if (PlayerbotAI::IsTank(bot))
    {
        // The tank fetches or drags the keepers. It turns to the adds when the shield runs low, and to a channeling
        // add when the shield slipped below TANK_CHANNEL_SHIELD_PCT or no keeper waits. A held keeper comes first.
        if (BMShared::GuardianToDrag(botAI))
            return false;
        int32 const shield = BMShared::ShieldPct(bot);
        if (shield < BMShared::FETCH_SHIELD_PCT && BMShared::FirstThreat(botAI))
            return true;
        return BMShared::AnyCorrupting(botAI) &&
               (shield < BMShared::TANK_CHANNEL_SHIELD_PCT || !BMShared::GuardianToFetch(botAI));
    }

    // Channels first; without one a keeper at Medivh comes before the adds that only fight ("bm kill rift guardian").
    if (BMShared::AnyCorrupting(botAI))
        return true;
    return BMShared::FirstThreat(botAI) && !BMShared::GuardianAtMedivh(botAI);
}

bool BlackMorassKillRiftGuardianTrigger::IsActive()
{
    if (!bot->IsAlive() || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot) || !BMShared::IsEventInProgress(bot) ||
        BMShared::AnyCorrupting(botAI))
        return false;

    Unit* guardian = BMShared::GuardianAtMedivh(botAI);
    return guardian && AI_VALUE(Unit*, "current target") != guardian;
}

bool BlackMorassHoldNearMedivhTrigger::IsActive()
{
    if (!bot->IsAlive() || PlayerbotAI::IsTank(bot) || !BMShared::IsEventInProgress(bot))
        return false;

    Creature* medivh = BMShared::Medivh(bot);
    if (!medivh || bot->GetExactDist2d(medivh) <= BMShared::HOLD_LEASH)
        return false;

    Unit* current = AI_VALUE(Unit*, "current target");
    if (current && BMShared::IsThreat(botAI, current))
        return false;

    // Without a tank nobody brings the keeper: the damage dealers go to it.
    if (current && current->IsAlive() && BMShared::IsGuardianEntry(current->GetEntry()) &&
        !BMShared::HasLivingTank(bot))
        return false;

    return true;
}

bool BlackMorassFetchRiftGuardianTrigger::IsActive() { return BMShared::GuardianToFetch(botAI) != nullptr; }

bool BlackMorassDragRiftGuardianTrigger::IsActive() { return BMShared::GuardianToDrag(botAI) != nullptr; }

bool BlackMorassChronoBeaconFromSaatTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->GetMapId() != BMShared::MAP_ID)
        return false;

    if (bot->GetExactDist2d(BMShared::SAAT_X, BMShared::SAAT_Y) > BMShared::SAAT_DETOUR_RANGE)
        return false;

    return BMShared::IsEventNotStarted(bot) && !bot->HasItemCount(BMShared::ITEM_CHRONO_BEACON, 1);
}

bool BlackMorassUseChronoBeaconTrigger::IsActive()
{
    if (!bot->IsAlive() || !bot->IsInCombat() || !BMShared::IsEventInProgress(bot))
        return false;

    uint32 const rift = BMShared::RiftNumber(bot);
    if (rift < BMShared::RIFT_HARD_FIRST || rift >= BMShared::RIFT_AEONUS)
        return false;

    if (bot->GetExactDist2d(BMShared::HOLD_X, BMShared::HOLD_Y) > BMShared::BEACON_USE_RANGE)
        return false;

    if (BMShared::ShieldPct(bot) >= BMShared::BEACON_SHIELD_PCT &&
        AI_VALUE(GuidVector, "bm medivh threats").size() < BMShared::BEACON_MIN_THREATS)
        return false;

    return bot->HasItemCount(BMShared::ITEM_CHRONO_BEACON, 1);
}

bool BlackMorassBossBuffTrigger::IsActive() { return BMShared::BossBuffToRemove(botAI) != nullptr; }

bool BlackMorassFightSpotAtMedivhTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot) || !bot->IsAlive() || !bot->IsInCombat() || bot->GetMapId() != BMShared::MAP_ID ||
        !BMShared::IsEventInProgress(bot))
        return false;

    Position anchor;
    return FightHold::Get(bot, anchor) &&
           anchor.GetExactDist2d(BMShared::HOLD_X, BMShared::HOLD_Y) > FIGHT_SPOT_AT_MEDIVH;
}
