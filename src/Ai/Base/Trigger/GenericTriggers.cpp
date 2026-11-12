/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericTriggers.h"
#include "AttackerCountValues.h"
#include "BuyAction.h"
#include "ChooseTargetActions.h"
#include "LeaderOrders.h"  // [lead95] Leader::Orders::IsTracked
#include "Corpse.h"
#include "CreatureAI.h"
#include "DpsEstimate.h"  // [rot610] DebuffPays
#include "EstimatedLifetimeValue.h"  // [rot70] ExpectedLifetime
#include "GenericBuffUtils.h"
#include "GenericSpellActions.h"
#include "InterruptValues.h"
#include "ItemVisitors.h"
#include "LastSpellCastValue.h"
#include "PartyStrengthValue.h"
#include "PetsAction.h"  // [dps236] DungeonPetControl::Controls / ConfigStance
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "RaidUtilityAssignment.h"
#include "SetBonusTriggers.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"  // [b44_cos] AuraEffect::GetBase (LossOfControlTrigger)
#include "SpellPayoff.h"       // [rot400]
#include "SpellAuras.h"
#include "Strategy.h"  // [rot490] TargetValueExclusionType
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"  // [rot70] RATE_POWER_ENERGY
#include "TankAoeThreat.h"  // [b32_peel]
#include "TargetValue.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include "ThreatValues.h"  // [b44_hos]
#include "Timer.h"
#include <algorithm>
#include <limits>
#include <cmath>
#include <string>
#include <vector>

bool LowManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") &&
           AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.LowMana;
}

bool MediumManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") &&
           AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.MediumMana;
}

bool LowEnergyTrigger::IsActive()
{
    return AI_VALUE2(uint8, "energy", "self target") < threshold;
}

bool NoPetTrigger::IsActive()
{
    return bot->GetMinionGUID().IsEmpty() && !AI_VALUE(Unit*, "pet target") && !bot->GetGuardianPet() &&
           !bot->GetFirstControlled() && !AI_VALUE2(bool, "mounted", "self target");
}

bool HasPetTrigger::IsActive()
{
    return AI_VALUE(Unit*, "pet target") && !AI_VALUE2(bool, "mounted", "self target");
}

bool PetAttackTrigger::IsActive()
{
    Guardian* pet = bot->GetGuardianPet();
    if (!pet)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (pet->GetVictim() == target && pet->GetCharmInfo()->IsCommandAttack())
        return false;

    if (bot->GetMap()->IsDungeon() && bot->GetGroup() && !target->IsInCombat())
        return false;

    return true;
}

bool HighManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") &&
           AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.HighMana;
}

bool AlmostFullManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") &&
           AI_VALUE2(uint8, "mana", "self target") > 85;
}

bool EnoughManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") &&
           AI_VALUE2(uint8, "mana", "self target") > sPlayerbotAIConfig.HighMana;
}

bool RageAvailable::IsActive() { return AI_VALUE2(uint8, "rage", "self target") >= amount; }

bool EnergyAvailable::IsActive() { return AI_VALUE2(uint8, "energy", "self target") >= amount; }

bool ComboPointsAvailableTrigger::IsActive() { return AI_VALUE2(uint8, "combo", "current target") >= amount; }

bool ComboPointsNotFullTrigger::IsActive() { return AI_VALUE2(uint8, "combo", "current target") < amount; }

float ComboFinishHorizon(PlayerbotAI* botAI, Player* bot, std::string const& builder, std::string const& finisher)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    SpellInfo const* builderInfo = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", builder));
    SpellInfo const* finisherInfo = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", finisher));
    if (!builderInfo || !finisherInfo)
        return -1.0f;
    constexpr float CORE_ENERGY_REGEN_PER_SEC = 10.0f;  // Player::Regenerate, POWER_ENERGY
    float const regen = (bot->GetFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER + AsUnderlyingType(POWER_ENERGY)) +
                         CORE_ENERGY_REGEN_PER_SEC) *
                        sWorld->getRate(RATE_POWER_ENERGY);
    if (regen <= 0.0f)
        return -1.0f;
    float const energy = float(bot->GetPower(POWER_ENERGY));
    float const builderCost = float(builderInfo->CalcPowerCost(bot, builderInfo->GetSchoolMask()));
    float const finisherCost = float(finisherInfo->CalcPowerCost(bot, finisherInfo->GetSchoolMask()));
    float const gcd = float(builderInfo->StartRecoveryTime) / IN_MILLISECONDS;
    float const builderAt = std::max(0.0f, (builderCost - energy) / regen);
    return std::max(builderAt + gcd, (builderCost + finisherCost - energy) / regen);
}

bool TargetWithComboPointsLowerHealTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || !target->IsInWorld() || !ComboPointsAvailableTrigger::IsActive())
        return false;

    // the builder and the finisher this rogue spends its points with (its talents decide which it knows)
    std::string builder = "sinister strike";
    std::string finisher = "eviscerate";
    if (botAI->HasSpell("mutilate"))  // the Assassination builder, Envenom its finisher
    {
        builder = "mutilate";
        finisher = "envenom";
    }
    else if (botAI->HasSpell("hemorrhage"))  // the Subtlety builder
        builder = "hemorrhage";
    float const horizon = ComboFinishHorizon(botAI, bot, builder, finisher);
    return horizon > 0.0f && ExpectedLifetime(botAI, target) < horizon;
}

bool LoseAggroTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->IsPlayer())
        return false;  // players have no threat: taunts only waste the cooldown
    return !AI_VALUE2(bool, "has aggro", "current target");
}

bool HasAggroTrigger::IsActive() { return AI_VALUE2(bool, "has aggro", "current target"); }

bool MobAttackingGroupMemberTrigger::IsActive()
{
    // Also for a mob beyond taunt range on a friend within Righteous Defense range (wave 6 Utgarde Keep: Forge
    // Masters out of taunt range killed casters 20-29 yd from the tank six times).
    return PlayerbotAI::IsTank(bot) && (AI_VALUE(Unit*, "group member attacker") ||
                                        (friendTarget && AI_VALUE(Unit*, "attacked group member")));
}

bool MobsAttackingGroupMembersTrigger::IsActive()
{
    return PlayerbotAI::IsTank(bot) && AI_VALUE2(uint8, "group member attacker count", range) >= minAttackers;
}

bool GroupMemberAttackerNearTrigger::IsActive()
{
    if (sPlayerbotAIConfig.TankPeelWalkRange <= 0.0f || !PlayerbotAI::IsTank(bot))
        return false;

    Unit* mob = AI_VALUE(Unit*, "group member attacker");
    return mob && bot->IsWithinDistInMap(mob, sPlayerbotAIConfig.TankPeelWalkRange) && !bot->IsWithinMeleeRange(mob);
}

bool GroupMemberAttackersFarTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot))
        return false;

    // Even one mob on the healer, also out of taunt range: wave 8 Slave Pens / Ahn'kahet healers died with the
    // tank 40-50 yd away and nothing bringing it back.
    if (AI_VALUE(Unit*, "far healer attacker"))
        return true;

    // within taunt range, but not all within the reach of Consecration / Thunder Clap / Swipe / Death and Decay
    uint8 const inRange = AI_VALUE2(uint8, "group member attacker count", "30");
    return inRange >= MobsAttackingGroupMembersTrigger::PEEL_AOE_MIN_ATTACKERS &&
           AI_VALUE2(uint8, "group member attacker count", "8") < inRange;
}

bool TauntReactionTrigger::IsActive()
{
    if (!bot->IsInCombat() || !PlayerbotAI::IsTank(bot))
        return false;

    TauntReactionAction::Spells const* spells = TauntReactionAction::ClassSpells(bot);
    if (!spells)
        return false;

    if (!AI_VALUE(Unit*, "group member attacker") && !AI_VALUE(Unit*, "attacked group member"))
        return false;

    // [b32_peel] more than 3 mobs in reach and one not held: the area threat ("tank aoe crowd") goes first and the
    // peel steps of the class strategy follow it; a mob on the healer or a boss is taken here at once, as before
    if (TankPeel::CrowdFirst(botAI))
        return false;

    // [lead526] ... or a backup (lead521's plan): with the taunt, the ranged spell and the shout all cooling the
    // trigger was off and the backups never ran
    return TauntReactionAction::SpellReady(botAI, spells->taunt) ||
           TauntReactionAction::SpellReady(botAI, spells->ranged) ||
           TauntReactionAction::SpellReady(botAI, spells->swarm) || TauntReactionAction::AnyBackupReady(botAI);
}

bool FleeingMobTrigger::IsActive()
{
    if (!bot->IsInCombat() || PlayerbotAI::IsHeal(bot))
        return false;

    Unit* mob = AI_VALUE(Unit*, "fleeing mob");
    return mob && (Leader::LedBot(bot) || !FledMobs::StoppedRecently(mob));  // [lead1230]
}

bool FledSurvivorNearTrigger::IsActive()
{
    if (bot->IsInCombat() || !bot->IsAlive() || !PlayerbotAI::IsTank(bot) || !bot->GetMap() ||
        !bot->GetMap()->IsDungeon())
        return false;

    return AI_VALUE(Unit*, "fled survivor") != nullptr;
}

bool HealerAttackedFarTrigger::IsActive()
{
    return PlayerbotAI::IsTank(bot) && AI_VALUE(Unit*, "far healer attacker");
}

bool GroupMemberAttackedTrigger::IsActive()
{
    if (bot->IsInCombat() || !bot->IsAlive() || !PlayerbotAI::IsTank(bot))
        return false;

    Unit* mob = AI_VALUE(Unit*, "defend member target");
    return mob && bot->GetVictim() != mob;
}

bool IdleBossAtTankTrigger::IsActive()
{
    constexpr float IDLE_BOSS_RANGE = 10.0f;
    constexpr uint32 IDLE_BOSS_MS = 10 * IN_MILLISECONDS;
    if (bot->IsInCombat() || !bot->IsAlive() || !PlayerbotAI::IsTank(bot) || !bot->GetMap() ||
        !bot->GetMap()->IsDungeon())
    {
        _sinceMs = 0;
        return false;
    }

    // the current target, else the planner's pick: the body-pulled boss is often neither attacked nor the current
    // target (occ-h build 15: "careful pull: engage ... Drakos" every second, no swing, no current target)
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        target = AI_VALUE(Unit*, "aggressive target");
    Creature* boss = target ? target->ToCreature() : nullptr;
    if (!boss || !(boss->IsDungeonBoss() || boss->isWorldBoss()) || !boss->IsAlive() || boss->IsInCombat() ||
        !bot->IsWithinDistInMap(boss, IDLE_BOSS_RANGE) || !bot->IsValidAttackTarget(boss))
    {
        _sinceMs = 0;
        return false;
    }

    uint32 const now = getMSTime();
    if (!_sinceMs || _boss != boss->GetGUID())
    {
        _boss = boss->GetGUID();
        _sinceMs = now;
        return false;
    }

    return getMSTimeDiff(_sinceMs, now) >= IDLE_BOSS_MS;
}

bool BuffTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    Aura* aura = FindBuff(target);
    return ai::buff::BuffBelowRefreshTarget(botAI, aura, beforeDuration);
}

Aura* BuffTrigger::FindBuff(Unit* target) { return botAI->GetAura(spell, target, checkIsOwner, checkDuration); }

Value<Unit*>* BuffOnPartyTrigger::GetTargetValue()
{
    return context->GetValue<Unit*>(
        "party member without aura", ai::buff::MakeAuraQualifierForBuff(spell));
}

bool BuffOnPartyTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (ai::buff::ShouldDeferPartyBuffEvaluationForRecentLogin(bot, target, spell))
        return false;

    return BuffTrigger::IsActive();
}

bool ProtectPartyMemberTrigger::IsActive() { return AI_VALUE(Unit*, "party member to protect"); }

Value<Unit*>* DebuffOnAttackerTrigger::GetTargetValue()
{
    return context->GetValue<Unit*>("attacker without aura", spell);
}

Value<Unit*>* DebuffOnMeleeAttackerTrigger::GetTargetValue()
{
    return context->GetValue<Unit*>("melee attacker without aura", spell);
}

bool NoAttackersTrigger::IsActive()
{
    return !AI_VALUE(Unit*, "current target") && AI_VALUE(uint8, "my attacker count") > 0;
}

bool InvalidTargetTrigger::IsActive() { return AI_VALUE2(bool, "invalid target", "current target"); }

bool NoTargetTrigger::IsActive() { return !AI_VALUE(Unit*, "current target"); }

bool TankIncomingMobTrigger::IsActive() { return TankIncomingMob(botAI, bot) != nullptr; }

bool LeaderPullIssuedTrigger::IsActive()
{
    if (bot->IsInCombat() || !PlayerbotAI::IsTank(bot) || !Leader::Orders::IsTracked(bot->GetGUID()))
        return false;
    PullPlanner::Decision issued;
    return PullPlanner::TakeIssued(bot->GetGUID(), 0, issued, false);
}

bool MyAttackerCountTrigger::IsActive()
{
    return AI_VALUE2(bool, "combat", "self target") && AI_VALUE(uint8, "my attacker count") >= amount;
}

bool MediumThreatTrigger::IsActive()
{
    if (!AI_VALUE(Unit*, "main tank"))
        return false;

    return MyAttackerCountTrigger::IsActive();
}

bool LowTankThreatTrigger::IsActive()
{
    Unit* current_target = AI_VALUE(Unit*, "current target");
    if (!current_target || current_target->IsPlayer())
        return false;  // players have no threat: Tricks of the Trade / Misdirection would be wasted

    Unit* mainTank = AI_VALUE(Unit*, "main tank");
    if (!mainTank)
        return false;

    ThreatManager& mgr = current_target->GetThreatMgr();
    float threat = mgr.GetThreat(bot);
    float tankThreat = mgr.GetThreat(mainTank);
    return tankThreat == 0.0f || threat > tankThreat * 0.5f;
}

bool HighThreatTrigger::IsActive()
{
    // Percent of the tank's threat on the bot's target at which a real dps drops threat before the mob turns.
    constexpr uint8 HIGH_THREAT_PERCENT = 90;

    if (PlayerbotAI::IsTank(bot) || !bot->GetGroup())
        return false;

    if (_raidOnly)
    {
        Map* map = bot->GetMap();
        if (!map || !map->IsRaid())
            return false;
    }

    Unit* target = AI_VALUE(Unit*, "current target");
    // a mob nobody has threat on (the tank has not reached it yet) reads as 100 too: nothing to drop then
    if (!target || target->IsPlayer() || target->GetThreatMgr().GetThreat(bot) <= 0.0f)
        return false;

    if (AI_VALUE2(uint8, "threat", "current target") < HIGH_THREAT_PERCENT)
        return false;

    // the group's dps outgear its tank in practice: the others give it more margin for a while
    // [b44_hos] Not a mob on a friendly NPC (Brann, an escort): its threat lead says nothing about the group's tank,
    // and the careful limits it switches on held the whole group (AoE 30, single 70, melee 85) for 60 s at a time.
    if (!ThreatLead::NpcHolder(bot, target))
        PartyStrengthShared::NoteThreatScare(bot);
    return true;
}

bool AoeTrigger::IsActive()
{
    Unit* current_target = AI_VALUE(Unit*, "current target");
    uint32 const minCount = static_cast<uint32>(amount);
    // [b36_tankaoe] A tank in combat counts the mobs around itself too: its pile stands on the tank, not around the
    // current target (a caster at range, a fresh add walking in, none between tank assist picks). The tank AoE
    // spells (Thunder Clap, Swipe, Blood Boil, Death and Decay, Consecration) hit around the tank. hos-h-5G-25
    // Tribunal: the death knight's target was empty or a lone Protector while 3-8 mobs stood on it.
    if (bot->IsInCombat() && PlayerbotAI::IsTank(bot) &&
        HasEnoughAoeTargets(botAI, bot->GetPosition(), range, minCount))
        return true;

    if (!current_target)
        return false;

    return HasEnoughAoeTargets(botAI, current_target->GetPosition(), range, minCount);
}

bool AoePaysTrigger::IsActive()
{
    // the cheap counts first: two attackers at least, a target, the spell known
    uint32 const minimum = AOE_MIN_TARGETS + (excludesTarget ? 1 : 0);
    if (AI_VALUE(uint8, "attacker count") < minimum)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    uint32 const aoeId = AI_VALUE2(uint32, "spell id", aoeSpell);
    uint32 const fillerId = AI_VALUE2(uint32, "spell id", Filler());
    if (!aoeId || !fillerId)
        return false;

    float const radius = SpellPayoff::AreaRadius(bot, aoeId);
    float const scale = FillerScale();
    uint32 const breakEven = SpellPayoff::AoeBreakEvenTargets(bot, target, aoeId, fillerId, scale);
    if (radius <= 0.0f || !breakEven)
        return false;

    uint32 const needed = std::max(breakEven, AOE_MIN_TARGETS) + (excludesTarget ? 1 : 0);
    Position const center = selfCentred ? bot->GetPosition() : target->GetPosition();
    if (HasEnoughAoeTargets(botAI, center, radius, needed))
        return LandsOverFiller(target, aoeId, fillerId, scale, center, radius);

    // [rot420] the steady-state filler held an area the filler read now would have cast: logged for the triage
    uint32 const now = getMSTime();
    if (scale > 1.0f && (!_scaleLoggedMs || getMSTimeDiff(_scaleLoggedMs, now) >= AOE_SCALE_LOG_MS))
    {
        uint32 const nowBreakEven = SpellPayoff::AoeBreakEvenTargets(bot, target, aoeId, fillerId);
        uint32 const nowNeeded = std::max(nowBreakEven, AOE_MIN_TARGETS) + (excludesTarget ? 1 : 0);
        if (nowBreakEven && nowNeeded < needed && HasEnoughAoeTargets(botAI, center, radius, nowNeeded))
        {
            _scaleLoggedMs = now ? now : 1;
            LOG_INFO("playerbots", "[rot420] {} holds {} on {}: {} beats it from {} targets at its steady x{:.2f} "
                     "(from {} at its damage now)", bot->GetName(), aoeSpell, target->GetName(), Filler(), needed,
                     scale, nowNeeded);
        }
    }
    return false;
}

bool AoePaysTrigger::LandsOverFiller(Unit* target, uint32 aoeId, uint32 fillerId, float scale, Position const& center,
                                     float radius)
{
    SpellPayoff::Cast area;
    float const fillerDps = SpellPayoff::FillerDps(bot, target, fillerId) * scale;
    if (fillerDps <= 0.0f || !SpellPayoff::Read(bot, target, aoeId, area))
        return true;  // no verdict from the spells: the count decides

    // the mobs HasEnoughAoeTargets counted: alive inside the area, no crowd control, no strategy exclusion
    GuidSet const exclusions = botAI->HasTargetExclusions()
                                   ? GatherStrategyTargetExclusions(botAI, TargetValueExclusionType::Aoe)
                                   : GuidSet();
    std::vector<std::pair<float, float>> mobs;  // lifetime ms, health
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->GetDistance(center) > radius || IsCrowdControlled(unit) ||
            exclusions.count(guid) || (excludesTarget && unit == target))
            continue;
        mobs.emplace_back(ExpectedFightLifetime(botAI, unit) * static_cast<float>(IN_MILLISECONDS),
                          static_cast<float>(unit->GetHealth()));
    }
    if (mobs.size() < AOE_MIN_TARGETS)
        return false;

    // a channel the class stops below the minimum lasts till the minimum-th longest life ends
    float window = area.channelMs;
    if (area.channel && ChannelStopsBelowMinimum())
    {
        std::nth_element(mobs.begin(), mobs.begin() + (AOE_MIN_TARGETS - 1), mobs.end(),
                         [](auto const& a, auto const& b) { return a.first > b.first; });
        window = std::min(window, mobs[AOE_MIN_TARGETS - 1].first);
    }
    float const spentMs = area.channel ? std::max(window, area.gcdMs) : area.executeMs;

    float landed = 0.0f;
    float shortest = std::numeric_limits<float>::max();
    for (auto const& [lifetimeMs, health] : mobs)
    {
        landed += SpellPayoff::LandedOn(area, lifetimeMs, window, health);
        shortest = std::min(shortest, lifetimeMs);
    }
    float filler = fillerDps * spentMs / static_cast<float>(IN_MILLISECONDS);
    // [rot550] [lead1539] b275: the frost mage's Blizzard (74 % base mana) on bosses with one add where Frostbolt (11 %)
    // dealt more. When the mana the filler spends over the fight left (the target's expected life) is short of the
    // pool plus its in-combat regeneration, the area's mana above the filler's for the same time is filler damage
    // the mage will not cast later: it is charged at the filler's damage per mana (spell data: CalcPowerCost)
    std::string manaNote;
    SpellInfo const* aoeInfo = sSpellMgr->GetSpellInfo(aoeId);
    SpellInfo const* fillerInfo = sSpellMgr->GetSpellInfo(fillerId);
    SpellPayoff::Cast fillerCast;
    if (aoeInfo && fillerInfo && aoeInfo->PowerType == POWER_MANA && fillerInfo->PowerType == POWER_MANA &&
        bot->GetMaxPower(POWER_MANA) && SpellPayoff::Read(bot, target, fillerId, fillerCast) &&
        fillerCast.executeMs > 0.0f)
    {
        float const aoeCost = static_cast<float>(std::max(aoeInfo->CalcPowerCost(bot, aoeInfo->GetSchoolMask()), 0));
        float const fillerCost =
            static_cast<float>(std::max(fillerInfo->CalcPowerCost(bot, fillerInfo->GetSchoolMask()), 0));
        float const fillerManaPerSec = fillerCost * static_cast<float>(IN_MILLISECONDS) / fillerCast.executeMs;
        float const fightSec = ExpectedFightLifetime(botAI, target);
        float const regen = std::max(0.0f, bot->GetFloatValue(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER +
                                                              POWER_MANA));
        // a life with no end in sight (no damage on it yet) is short of any pool the filler drains
        bool const short_ = fillerManaPerSec > regen && fightSec > 0.0f &&
                            (!std::isfinite(fightSec) ||
                             (fillerManaPerSec - regen) * fightSec > static_cast<float>(bot->GetPower(POWER_MANA)));
        float const extraMana = aoeCost - fillerManaPerSec * spentMs / static_cast<float>(IN_MILLISECONDS);
        if (short_ && extraMana > 0.0f && fillerCost > 0.0f)
        {
            float const perMana = fillerDps * fillerCast.executeMs / static_cast<float>(IN_MILLISECONDS) / fillerCost;
            filler += extraMana * perMana;
            manaNote = Acore::StringFormat(" + its {:.0f} mana over the filler's at {:.2f} per mana (the fight's "
                                           "{:.0f} s short of mana) [lead1539]",
                                           extraMana, perMana, fightSec);
        }
    }
    if (landed >= filler)
        return true;

    uint32 const now = getMSTime();
    if (!_landedLoggedMs || getMSTimeDiff(_landedLoggedMs, now) >= AOE_SCALE_LOG_MS)
    {
        _landedLoggedMs = now ? now : 1;
        LOG_INFO("playerbots", "[rot490] {} holds {} on {}: it lands {:.0f} on {} mobs before they die (shortest "
                 "{:.1f} s) vs {} {:.0f} in its {:.1f} s{}", bot->GetName(), aoeSpell, target->GetName(), landed,
                 mobs.size(), shortest / static_cast<float>(IN_MILLISECONDS), Filler(), filler,
                 spentMs / static_cast<float>(IN_MILLISECONDS), manaNote);
    }
    return false;
}

bool NoFoodTrigger::IsActive()
{
    return AI_VALUE2(std::vector<Item*>, "inventory items", "conjured food").empty();
}

bool NoDrinkTrigger::IsActive()
{
    std::vector<Item*> const water = AI_VALUE2(std::vector<Item*>, "inventory items", "conjured water");
    if (water.empty())
        return true;
    // [dun199] a mage of a guild five conjures up to the five's priced line - [dun239] in the run too, while it is out
    // of combat (MageConjureAllowed: inside, while the group rests)
    if (bot->IsInCombat())
        return false;
    uint32 const goal = ConjureGoal(bot->GetGUID());
    if (!goal)
        return false;
    uint32 held = 0;
    for (Item* item : water)
        if (item)
            held += item->GetCount();
    return held < goal;
}

bool NeedFoodRestockTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->InBattleground() || bot->IsInFlight() || IsSelfBot(bot))
        return false;

    if (CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD) < RESTOCK_FOOD_MIN)
        return true;

    return bot->GetMaxPower(POWER_MANA) > 0 && CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK) < RESTOCK_FOOD_MIN;
}

bool TargetInSightTrigger::IsActive() { return AI_VALUE(Unit*, "grind target"); }

bool DebuffTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;

    // [rot610] the payoff by the formula, not a fixed lifetime
    return BuffTrigger::IsActive() && DpsEstimate::DebuffPays(bot, target, AI_VALUE2(uint32, "spell id", spell)) &&
           (DotFiller().empty() || DotPaysOff(target));
}

bool DebuffTrigger::DotPaysOff(Unit* target)
{
    std::string const filler = DotFiller();
    uint32 const dotId = AI_VALUE2(uint32, "spell id", spell);
    uint32 const fillerId = AI_VALUE2(uint32, "spell id", filler);
    if (!dotId || !fillerId)
        return true;

    float const lifetime = ExpectedFightLifetime(botAI, target);
    SpellPayoff::DotVerdict const verdict =
        SpellPayoff::DotPays(bot, target, dotId, fillerId, lifetime, DotUpfrontShare());
    if (verdict.pays)
        return true;

    uint32 const now = getMSTime();
    if (!_payoffLoggedMs || getMSTimeDiff(_payoffLoggedMs, now) >= DOT_PAYOFF_LOG_MS)
    {
        _payoffLoggedMs = now ? now : 1;
        LOG_INFO("playerbots",
                 "[rot400] {} holds {} on {}: lives {:.1f} s, {} tick(s) land for {:.0f} vs {} {:.0f} in its time",
                 bot->GetName(), spell, target->GetName(), lifetime, verdict.ticks, verdict.dot, filler,
                 verdict.filler);
    }
    return false;
}

bool DebuffOnBossTrigger::IsActive()
{
    if (!DebuffTrigger::IsActive())
        return false;

    Creature* creature = GetTarget()->ToCreature();
    return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
}

bool SpellTrigger::IsActive() { return GetTarget(); }

bool SpellCanBeCastTrigger::IsActive()
{
    Unit* target = GetTarget();
    return target && botAI->CanCastSpell(spell, target);
}

bool SpellNoCooldownTrigger::IsActive()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", name);
    if (!spellId)
        return false;

    return !bot->HasSpellCooldown(spellId);
}

bool SpellCooldownTrigger::IsActive()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", name);
    if (!spellId)
        return false;

    return bot->HasSpellCooldown(spellId);
}

RandomTrigger::RandomTrigger(PlayerbotAI* botAI, std::string const name, int32 probability)
    : Trigger(botAI, name), probability(probability), lastCheck(getMSTime()) {}

bool RandomTrigger::IsActive()
{
    if (getMSTime() - lastCheck < sPlayerbotAIConfig.RepeatDelay)
        return false;

    lastCheck = getMSTime();
    int32 k = (int32)(probability / sPlayerbotAIConfig.RandomChangeMultiplier);
    if (k < 1)
        k = 1;

    return (rand() % k) == 0;
}

bool AndTrigger::IsActive() { return ls && rs && ls->IsActive() && rs->IsActive(); }

std::string const AndTrigger::getName()
{
    std::string name(ls->getName());
    name = name + " and ";
    name = name + rs->getName();
    return name;
}

bool TwoTriggers::IsActive()
{
    if (name1.empty() || name2.empty())
        return false;

    Trigger* trigger1 = botAI->GetAiObjectContext()->GetTrigger(name1);
    Trigger* trigger2 = botAI->GetAiObjectContext()->GetTrigger(name2);

    if (!trigger1 || !trigger2)
        return false;

    return trigger1->IsActive() && trigger2->IsActive();
}

std::string const TwoTriggers::getName()
{
    std::string name;
    name = name1 + " and " + name2;
    return name;
}

bool BoostTrigger::IsActive()
{
    if (!BuffTrigger::IsActive())
        return false;

    // [lead178] user: a led bot's big offensive cooldown is the leader's decision - on trash only one that is back
    // before the next boss pull, else held for the boss (the leader's Intensity burn and its allowed list)
    // (the tool uses stay the bot's, as ThreatMultiplier's burn exceptions: a healer's, Mirror Image with attackers on
    // it, Shadowfiend for mana)
    std::string const name = getName();
    // [lead422] a damage dealer's Shadowfiend below half mana is no tool use: the leader's verdict decides it
    // [lead423] a healer's Shadowfiend too: the leader's mana tool
    bool const toolUse = (PlayerbotAI::IsHeal(bot) && name != "shadowfiend") ||
                         (name == "mirror image" && AI_VALUE(uint8, "my attacker count"));
    // [lead770] b220 rfc-h: the bear tanks cast Berserk on trash 6 times each (5D Rtdruac, 5H Rtdruae) and on 0 of
    // their 8 bosses - 5H 209-230 s, 569-608 s, 776-800 s, 955-982 s under the leader's "berserk: held for <boss>"
    // (the protection paladins' Avenging Wrath 2 / 3 and 3 / 2 trash / boss). The led tank is no executor member
    // (Orders::IsTracked), so its boost triggers fell through to "instances: on cooldown" while the leader judged
    // its cooldowns every fight tick (LeaderEvents, SetIntensity on the tank). Every led bot's burn cooldown is the
    // leader's verdict, the tank's too
    if (!toolUse && Leader::IsBurnCooldown(name) && Leader::LedBot(bot))
    {
        Leader::MemberEvents events;
        return Leader::Events::Get(bot->GetGUID(), events) && Leader::BurnAllows(events, name);
    }

    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->ToPlayer())
        return true;

    // Offensive cooldowns line up with Bloodlust/Heroism (or a boss pull with no lust coming), and wait on a boss
    // pull while the group's shaman can cast it now.
    if (ai::raidutil::IsBurstWindow(botAI))
        return true;
    if (ai::raidutil::ShouldHoldBurstForLust(botAI))
        return false;

    // [rot9] wowsims: offensive cooldowns on cooldown, trash too (instances); outdoors the losing-fight rule
    if (ai::raidutil::IsInstanceMap(bot))
        return true;

    return AI_VALUE(uint8, "balance") <= balance;
}

bool GenericBoostTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->ToPlayer())
        return true;

    if (ai::raidutil::IsBurstWindow(botAI))
        return true;
    if (ai::raidutil::ShouldHoldBurstForLust(botAI))
        return false;

    if (ai::raidutil::IsInstanceMap(bot))  // [rot9] as BoostTrigger
        return true;

    return AI_VALUE(uint8, "balance") <= balance;
}

bool SoloHealerShouldAttackTrigger::IsActive()
{
    return botAI->GetNearGroupMemberCount(sPlayerbotAIConfig.SightDistance) <= 1 || DpsEstimate::FightsAlone(bot);
}

bool HealerShouldAttackTrigger::IsActive()
{
    if (botAI->GetNearGroupMemberCount(sPlayerbotAIConfig.SightDistance) <= 1)
        return true;
    // [deaths9] alone by SoloSafety's rule (no living member within the group reward distance): it fights as a solo
    // bot - its judge and the dps formula count its spells. b345 Guinudrond L11 (holy, a user pack's leader questing
    // apart from its members, one within sight): under 65 % mana or 85 % health it held every spell, meleed at
    // 4.9-5.8 dps where the formula read 19.5, and died 4 times (3x Elder Black Bear, 2 Tunnel Rats)
    if (DpsEstimate::FightsAlone(bot))
        return true;

    if (AI_VALUE2(uint8, "health", "party member to heal") < sPlayerbotAIConfig.AlmostFullHealth)
        return false;

    if (bot->GetAura(33891)) // Tree of Life
    {
        LastSpellCast& lastSpell = botAI->GetAiObjectContext()->GetValue<LastSpellCast&>("last spell cast")->Get();
        if (lastSpell.timer + 5 > time(nullptr))
            return false;
    }

    int manaThreshold;
    int balance = AI_VALUE(uint8, "balance");
    if (balance <= 50)
        manaThreshold = 85;
    else if (balance <= 100)
        manaThreshold = sPlayerbotAIConfig.HighMana;
    else
        manaThreshold = sPlayerbotAIConfig.MediumMana;

    if (AI_VALUE2(bool, "has mana", "self target") && AI_VALUE2(uint8, "mana", "self target") < manaThreshold)
        return false;

    return true;
}

bool ItemCountTrigger::IsActive() { return AI_VALUE2(uint32, "item count", item) < uint32(count); }

bool InterruptSpellTrigger::IsActive()
{
    // The group interrupt rotation may have given this cast to another member: one kick per cast.
    return SpellTrigger::IsActive() && botAI->IsInterruptableSpellCasting(GetTarget(), getName()) &&
           InterruptCoordination::Allows(botAI, GetTarget(), spell);
}

bool DeflectSpellTrigger::IsActive()
{
    // [bear_aoe_threat] Spell Reflection takes a bolt from any caster, not only the tank's current target: hos-h
    // build 30 (20261025-184229-hos-h-5F-39, Tribunal of Ages) the Prot Warrior reflected once in 158 s and died to a
    // Dark Rune Stormcaller's Lightning Bolt (7560) while it held a Protector.
    if (spell == "spell reflection")
    {
        Unit* current = GetTarget();
        bool const currentCasts =
            current && current->IsNonMeleeSpellCast(true) && current->GetTarget() == bot->GetGUID();
        if (!currentCasts)
        {
            bool casterOnBot = false;
            for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
            {
                Unit* mob = botAI->GetUnit(guid);
                if (!mob || !mob->IsAlive() || !mob->IsNonMeleeSpellCast(false, false, true))
                    continue;

                Spell* cast = mob->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                if (cast && cast->m_targets.GetUnitTargetGUID() == bot->GetGUID())
                {
                    casterOnBot = true;
                    break;
                }
            }
            if (!casterOnBot)
                return false;
        }

        return context->GetValue<uint32>("spell id", spell)->Get() != 0;
    }

    Unit* target = GetTarget();
    if (!target || !target->IsNonMeleeSpellCast(true) || target->GetTarget() != bot->GetGUID())
        return false;

    uint32 spellid = context->GetValue<uint32>("spell id", spell)->Get();
    if (!spellid)
        return false;

    SpellInfo const* deflectSpell = sSpellMgr->GetSpellInfo(spellid);
    if (!deflectSpell)
        return false;

    // warrior deflects all
    if (spell == "spell reflection")
        return true;

    // human priest feedback
    if (spell == "feedback")
        return true;

    SpellSchoolMask deflectSchool = SpellSchoolMask(deflectSpell->Effects[EFFECT_0].MiscValue);
    SpellSchoolMask attackSchool = SPELL_SCHOOL_MASK_NONE;

    if (Spell* spell = target->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        if (SpellInfo const* tarSpellInfo = spell->GetSpellInfo())
        {
            attackSchool = tarSpellInfo->GetSchoolMask();
            if (attackSchool != SPELL_SCHOOL_MASK_NONE && (deflectSchool & attackSchool))
                return true;
        }
    }

    return false;
}

bool AttackerCountTrigger::IsActive() { return AI_VALUE(uint8, "attacker count") >= amount; }

bool HasAuraTrigger::IsActive() { return botAI->HasAura(getName(), GetTarget(), false, false, -1, true); }

bool LossOfControlTrigger::IsActive()
{
    // [b44_cos] Only a control that still lasts: cos-h spent Every Man for Himself (2 min cooldown) on Epoch's
    // heroic Time Stop, a 2 s stun that ends before the trinket-like break pays off. A control of unknown length
    // (permanent / no duration) still counts.
    constexpr int32 MIN_CONTROL_LEFT_MS = 3000;
    static AuraType const controls[] = {SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_ROOT,
                                        SPELL_AURA_MOD_CONFUSE, SPELL_AURA_MOD_CHARM};
    for (AuraType const type : controls)
        for (AuraEffect const* effect : bot->GetAuraEffectsByType(type))
        {
            Aura const* aura = effect ? effect->GetBase() : nullptr;
            if (aura && (aura->GetDuration() < 0 || aura->GetDuration() >= MIN_CONTROL_LEFT_MS))
                return true;
        }
    return false;
}

bool FearCharmSleepTrigger::IsActive()
{
    return bot->HasAuraType(SPELL_AURA_MOD_FEAR) ||
           bot->HasAuraType(SPELL_AURA_MOD_CHARM) ||
           bot->HasAuraType(SPELL_AURA_AOE_CHARM) ||
           bot->HasAuraWithMechanic(1 << MECHANIC_SLEEP);
}

bool FearSleepSapTrigger::IsActive()
{
    return bot->HasAuraType(SPELL_AURA_MOD_FEAR) ||
           bot->HasAuraWithMechanic(1 << MECHANIC_SLEEP) ||
           bot->HasAuraWithMechanic(1 << MECHANIC_SAPPED);
}

bool PoisonDiseaseBleedTrigger::IsActive()
{
    return botAI->HasAuraToDispel(bot, DISPEL_POISON) ||
           botAI->HasAuraToDispel(bot, DISPEL_DISEASE) ||
           bot->HasAuraWithMechanic(1 << MECHANIC_BLEED);
}

bool MovementImpairedTrigger::IsActive()
{
    return botAI->IsMovementImpaired(bot) &&
           !botAI->HasAnyAuraOf(bot, "stealth", "prowl", nullptr);
}

bool HasAuraStackTrigger::IsActive()
{
    return botAI->GetAura(getName(), GetTarget(), false, true, stack);
}

bool TimerTrigger::IsActive()
{
    time_t now = time(nullptr);

    if (now != lastCheck)
    {
        lastCheck = now;
        return true;
    }

    return false;
}

bool TimerBGTrigger::IsActive()
{
    time_t now = time(nullptr);

    if (now - lastCheck >= 60)
    {
        lastCheck = now;
        return true;
    }

    return false;
}

bool HasNoAuraTrigger::IsActive() { return !botAI->HasAura(getName(), GetTarget()); }

namespace
{
    constexpr float TANK_ASSIST_AOE_REACH = 8.0f;      // Consecration, Thunder Clap, Swipe around the tank
    constexpr uint8 TANK_ASSIST_AOE_PEEL_MOBS = 2;     // as "mobs attacking group members nearby" (10 yd)
    constexpr uint32 TANK_ASSIST_AOE_GRACE_MS = 1500;  // ~1.5 Consecration ticks: loose longer, the AoE is not enough
    constexpr float TANK_ASSIST_SECURE_LEAD = 1.3f;    // the current target stays on the tank with this margin
    constexpr float TANK_ASSIST_SLIPPING_LEAD = 1.1f;  // a mob held by less than this needs the tank's attention

    // The tank's threat on `mob` over the highest threat anyone else has on it (2.0: nobody else has any).
    float TankThreatLead(Player* bot, Unit* mob)
    {
        if (!mob->CanHaveThreatList())
            return 2.0f;

        float mine = 0.0f;
        float others = 0.0f;
        for (ThreatReference const* ref : mob->GetThreatMgr().GetUnsortedThreatList())
        {
            if (ref->GetVictim() == bot)
                mine = ref->GetThreat();
            else
                others = std::max(others, ref->GetThreat());
        }

        if (others <= 0.0f)
            return mine > 0.0f ? 2.0f : 0.0f;

        return mine / others;
    }
}

// Wave 6: re-targeting the lowest-threat mob every time another one dipped below the current target was a third
// of a tank's combat ticks (relevance 50, above the peel taunts). The tank now keeps a target it holds, and moves
// on for a mob on the healer, for a mob on a damage dealer that its area threat does not reach, for a mob in melee
// when the current target is not, or for a mob slipping off while the current target is held with a safe margin.
bool TankAssistTrigger::IsActive()
{
    // Out of combat a mob on a member counts even before the cached threat-list "attackers" lists it.
    bool const idle = !bot->IsInCombat();
    if (!AI_VALUE(uint8, "attacker count") && (!idle || !AI_VALUE(Unit*, "defend member target")))
        return false;

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (!currentTarget)
        return true;

    Unit* tankTarget = AI_VALUE(Unit*, "tank target");
    if (!tankTarget)
        return false;

    // Out of combat "current target" is only a selection: the non-combat engine has no rotation to act on it.
    if (currentTarget == tankTarget)
        return idle && tankTarget->IsInCombat() && bot->GetVictim() != tankTarget;

    Unit* victim = tankTarget->GetVictim();
    Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
    bool const onGroupMember = victimPlayer && victimPlayer != bot && bot->IsInSameRaidWith(victimPlayer) &&
                               !PlayerbotAI::IsTank(victimPlayer);
    bool const onHealer = onGroupMember && PlayerbotAI::IsHeal(victimPlayer);

    // A current target loose on a damage dealer is still worked on, but not while another mob hits the healer
    // (wave 8 Slave Pens: the tank kept a mob on the hunter while four mobs were already on the healer).
    bool const currentInMelee = bot->IsWithinMeleeRange(currentTarget);
    if (!AI_VALUE2(bool, "has aggro", "current target"))
    {
        if (!onGroupMember)
            return false;

        // a loose boss is taunted back ("lose aggro"); a raid's main tank leaves the adds to the off-tanks
        Creature* currentCreature = currentTarget->ToCreature();
        if ((currentCreature && (currentCreature->IsDungeonBoss() || currentCreature->isWorldBoss())) ||
            (bot->GetMap() && bot->GetMap()->IsRaid() && PlayerbotAI::IsMainTank(bot)))
            return false;

        Unit* currentVictim = currentTarget->GetVictim();
        Player* currentVictimPlayer = currentVictim ? currentVictim->ToPlayer() : nullptr;
        bool const currentOnMember = currentVictimPlayer && currentVictimPlayer != bot &&
                                     bot->IsInSameRaidWith(currentVictimPlayer) &&
                                     !PlayerbotAI::IsTank(currentVictimPlayer);
        if (onHealer)
            return !currentOnMember || !PlayerbotAI::IsHeal(currentVictimPlayer);

        // Wave 25: a current target on a pet, a totem or nobody kept the tank while a mob hit a damage dealer; one
        // out of melee kept it while the loose mob stood in reach of the tank's swings.
        return !currentOnMember || (!currentInMelee && bot->IsWithinMeleeRange(tankTarget));
    }

    if (onGroupMember)
    {
        // Consecration ticks lose to a healer's heal threat: a mob on the healer is taken by hand. The area threat
        // covers a damage dealer's mob only for a moment: on cooldown it holds nothing (wave 25: two mobs on the
        // hunter 0-3 yd from the tank for 5-11 s while the tank kept swinging at its held target).
        bool const coveredByAreaThreat =
            !onHealer && bot->IsWithinDist(tankTarget, TANK_ASSIST_AOE_REACH) &&
            AI_VALUE2(uint8, "group member attacker count", "10") >= TANK_ASSIST_AOE_PEEL_MOBS &&
            GroupMemberAttackerValue::LooseForMs(botAI, tankTarget->GetGUID()) < TANK_ASSIST_AOE_GRACE_MS;
        return !coveredByAreaThreat || !currentInMelee;
    }

    if (!currentInMelee && bot->IsWithinMeleeRange(tankTarget))
        return true;

    return TankThreatLead(bot, currentTarget) >= TANK_ASSIST_SECURE_LEAD &&
           TankThreatLead(bot, tankTarget) < TANK_ASSIST_SLIPPING_LEAD;
}

bool IsBehindTargetTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    return target && AI_VALUE2(bool, "behind", "current target");
}

bool IsNotBehindTargetTrigger::IsActive()
{
    if (botAI->HasStrategy("stay", botAI->GetState()))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return target && !AI_VALUE2(bool, "behind", "current target");
}

bool IsNotFacingTargetTrigger::IsActive()
{
    if (botAI->HasStrategy("stay", botAI->GetState()))
        return false;

    // A moving bot's spline rewrites its orientation every update, so a turn made now is undone at once and the
    // trigger fired again on the next tick (a Balance druid made 265 turns on Gruul). A cast turns the bot by itself
    // (PlayerbotAI::CastSpell); this trigger only keeps auto-attack and auto-repeat shots facing.
    if (bot->isMoving())
        return false;

    // A melee swing needs the target within 120 degrees in front (Player::Update), a spell within 180: the narrower
    // 90-degree "facing" value turned the bot each time the target drifted a little.
    constexpr float MELEE_FACING_ARC = 2.0f * static_cast<float>(M_PI) / 3.0f;
    Unit* target = AI_VALUE(Unit*, "current target");
    return target && !bot->HasInArc(MELEE_FACING_ARC, target);
}

bool HasCcTargetTrigger::IsActive()
{
    Unit* rtiCcTarget = nullptr;
    if (botAI->IsInNonRaidDungeon())
    {
        rtiCcTarget = AI_VALUE(Unit*, "rti cc target");
        if (!rtiCcTarget)
            return false;
    }

    return IsCcTargetFree(AI_VALUE2(Unit*, "cc target", getName()), rtiCcTarget);
}

bool HasCcTargetTrigger::IsCcTargetFree(Unit* ccTarget, Unit* rtiCcTarget)
{
    if (!ccTarget || (rtiCcTarget && ccTarget != rtiCcTarget))
        return false;

    return !AI_VALUE2(Unit*, "current cc target", getName());
}

bool NoMovementTrigger::IsActive() { return !AI_VALUE2(bool, "moving", "self target"); }

bool NoPossibleTargetsTrigger::IsActive()
{
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    return !targets.size();
}

bool PossibleAddsTrigger::IsActive()
{
    return AI_VALUE(bool, "possible adds") && !AI_VALUE(ObjectGuid, "pull target");
}

bool NotDpsTargetActiveTrigger::IsActive()
{
    // [healer_heals_first] A healer with healing to do does not chase the "dps target" (see
    // DpsAssistAction::HealerHasHealingWork); with the group topped up it may help kill again.
    if (DpsAssistAction::HealerHasHealingWork(botAI))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && target->IsAlive())
    {
        Unit* enemy = AI_VALUE(Unit*, "enemy player target");
        if (target == enemy)
            return false;
    }

    Unit* dps = AI_VALUE(Unit*, "dps target");
    return dps && target != dps;
}

bool NotDpsAoeTargetActiveTrigger::IsActive()
{
    Unit* dps = AI_VALUE(Unit*, "dps aoe target");
    Unit* target = AI_VALUE(Unit*, "current target");
    Unit* enemy = AI_VALUE(Unit*, "enemy player target");

    if (target && target == enemy && target->IsAlive())
        return false;

    return dps && target != dps;
}

bool IsSwimmingTrigger::IsActive() { return AI_VALUE2(bool, "swimming", "self target"); }

bool HasItemForSpellTrigger::IsActive()
{
    std::string const spell = getName();
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && AI_VALUE2(Item*, "item for spell", spellId);
}

bool TargetChangedTrigger::IsActive()
{
    Unit* oldTarget = context->GetValue<Unit*>("old target")->Get();
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    return target && oldTarget != target;
}

Value<Unit*>* InterruptEnemyHealerTrigger::GetTargetValue()
{
    return context->GetValue<Unit*>("enemy healer target", spell);
}

bool RandomBotUpdateTrigger::IsActive()
{
    return RandomTrigger::IsActive() && AI_VALUE(bool, "random bot update");
}

bool NoNonBotPlayersAroundTrigger::IsActive()
{
    return !botAI->HasPlayerNearby();
    /*if (!bot->InBattleground())
        return AI_VALUE(GuidVector, "nearest non bot players").empty();

    return false;
    */
}

bool NewPlayerNearbyTrigger::IsActive() { return AI_VALUE(ObjectGuid, "new player nearby"); }

bool CollisionTrigger::IsActive() { return AI_VALUE2(bool, "collision", "self target"); }

bool ReturnToStayPositionTrigger::IsActive()
{
    PositionInfo stayPosition = AI_VALUE(PositionMap&, "position")["stay"];
    if (stayPosition.isSet())
    {
        const float distance = bot->GetDistance(stayPosition.x, stayPosition.y, stayPosition.z);
        return distance > sPlayerbotAIConfig.FollowDistance;
    }

    return false;
}

bool GiveItemTrigger::IsActive()
{
    return AI_VALUE2(Unit*, "party member without item", item) && AI_VALUE2(uint32, "item count", item);
}

bool GiveFoodTrigger::IsActive()
{
    return AI_VALUE(Unit*, "party member without food") && AI_VALUE2(uint32, "item count", item);
}

bool GiveWaterTrigger::IsActive()
{
    return AI_VALUE(Unit*, "party member without water") && AI_VALUE2(uint32, "item count", item);
}

Value<Unit*>* SnareTargetTrigger::GetTargetValue() { return context->GetValue<Unit*>("snare target", spell); }

bool StayTimeTrigger::IsActive()
{
    time_t stayTime = AI_VALUE(time_t, "stay time");
    time_t now = time(nullptr);
    return delay && stayTime && now > stayTime + 2 * delay / 1000;
}

bool IsMountedTrigger::IsActive() { return AI_VALUE2(bool, "mounted", "self target"); }

bool CorpseNearTrigger::IsActive()
{
    return bot->GetCorpse() && bot->GetCorpse()->IsWithinDistInMap(bot, CORPSE_RECLAIM_RADIUS, true);
}

bool IsFallingTrigger::IsActive() { return bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING); }

bool IsFallingFarTrigger::IsActive() { return bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING_FAR); }

bool HasAreaDebuffTrigger::IsActive() { return AI_VALUE2(bool, "has area debuff", "self target"); }

Value<Unit*>* BuffOnMainTankTrigger::GetTargetValue() { return context->GetValue<Unit*>("main tank", spell); }

bool AmmoCountTrigger::IsActive()
{
    if (bot->GetUInt32Value(PLAYER_AMMO_ID) != 0)
        return ItemCountTrigger::IsActive();  // Ammo already equipped

    if (botAI->FindAmmo())
        return true;  // Found ammo in inventory but not equipped

    return ItemCountTrigger::IsActive();
}

bool NewPetTrigger::IsActive()
{
    ObjectGuid currentPetGuid = ObjectGuid::Empty;
    Creature* current = nullptr;

    if (Pet* pet = bot->GetPet())
    {
        currentPetGuid = pet->GetGUID();
        current = pet;
    }
    else if (Guardian* guardian = bot->GetGuardianPet())
    {
        currentPetGuid = guardian->GetGUID();
        current = guardian;
    }

    if (currentPetGuid != lastPetGuid)
    {
        triggered = false;
        fired = false;
        lastPetGuid = currentPetGuid;
    }

    if (currentPetGuid == ObjectGuid::Empty || triggered)
        return false;

    // [dps236] settled: the config stance on it, or the dungeon pet control holds it (it sets its own stance)
    bool const settled = DungeonPetControl::Controls(bot) ||
                         (current && current->GetReactState() == DungeonPetControl::ConfigStance());
    if (!fired)
    {
        fired = true;
        triggered = settled;
        return true;
    }
    if (settled)
    {
        triggered = true;
        return false;
    }
    return true;
}

bool ForceRebuffPendingTrigger::IsActive()
{
    return botAI->forceRebuff.IsPending();
}

// ---------------------------------------------------------------------------------------------------------------
// Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h)
// ---------------------------------------------------------------------------------------------------------------

namespace
{
bool HasAnyAura(Player* bot, std::vector<uint32> const& ids)
{
    for (uint32 id : ids)
        if (bot->HasAura(id))
            return true;

    return false;
}

float HealGatePct(ai::setbonus::HealGate gate)
{
    switch (gate)
    {
        case ai::setbonus::HealGate::ALMOST_FULL:
            return float(sPlayerbotAIConfig.AlmostFullHealth);
        case ai::setbonus::HealGate::MEDIUM:
            return float(sPlayerbotAIConfig.MediumHealth);
        case ai::setbonus::HealGate::LOW:
            return float(sPlayerbotAIConfig.LowHealth);
        default:
            return 100.0f;
    }
}
}  // namespace

bool SetBonusTrigger::IsWorn()
{
    if (_spec.worn.empty())
        return true;

    uint32 const now = getMSTime();
    if (!_wornCheckMs || getMSTimeDiff(_wornCheckMs, now) >= ai::setbonus::WORN_RECHECK_MS)
    {
        _wornCheckMs = now;
        _worn = HasAnyAura(bot, _spec.worn);
    }

    return _worn;
}

bool SetBonusTrigger::Conditions()
{
    if (!_spec.auras.empty() && !HasAnyAura(bot, _spec.auras))
        return false;

    if (!_spec.absent.empty() && HasAnyAura(bot, _spec.absent))
        return false;

    if (_spec.stackAura)
    {
        Aura const* aura = bot->GetAura(_spec.stackAura);
        if (!aura || aura->GetStackAmount() < _spec.minStacks)
            return false;
    }

    if (_spec.icdAura)
    {
        uint32 const now = getMSTime();
        if (bot->HasAura(_spec.icdAura))
        {
            _icdSeenMs = now;
            return false;
        }

        if (_icdSeenMs && getMSTimeDiff(_icdSeenMs, now) < _spec.icdMs)
            return false;
    }

    if (_spec.selfHealthBelow && bot->GetHealthPct() >= float(_spec.selfHealthBelow))
        return false;

    if (_spec.selfHealthAbove && bot->GetHealthPct() <= float(_spec.selfHealthAbove))
        return false;

    if (_spec.maxManaPct &&
        (bot->GetMaxPower(POWER_MANA) == 0 || bot->GetPowerPct(POWER_MANA) >= float(_spec.maxManaPct)))
    {
        return false;
    }

    if (_spec.minRage || _spec.maxRage)
    {
        uint32 const rage = bot->GetPower(POWER_RAGE) / 10;
        if (rage < _spec.minRage || (_spec.maxRage && rage > _spec.maxRage))
            return false;
    }

    if (!_spec.readySpell.empty())
    {
        uint32 const spellId = context->GetValue<uint32>("spell id", _spec.readySpell)->Get();
        if (!spellId || bot->HasSpellCooldown(spellId))
            return false;
    }

    if (_spec.minComboPoints || _spec.tankingBoss)
    {
        Unit* target = context->GetValue<Unit*>("current target")->Get();
        if (!target || !target->IsAlive())
            return false;

        if (_spec.minComboPoints && bot->GetComboPoints(target) < _spec.minComboPoints)
            return false;

        if (_spec.tankingBoss)
        {
            Creature* creature = target->ToCreature();
            if (!bot->IsInCombat() || !creature || !(creature->IsDungeonBoss() || creature->isWorldBoss()) ||
                target->GetVictim() != bot)
            {
                return false;
            }
        }
    }

    if (_spec.healGate != ai::setbonus::HealGate::NONE)
    {
        Unit* healTarget = context->GetValue<Unit*>("party member to heal")->Get();
        if (!healTarget || !healTarget->IsAlive() || healTarget->GetHealthPct() >= HealGatePct(_spec.healGate))
            return false;
    }

    if (_spec.aoeHealCount && context->GetValue<uint8>("aoe heal", "almost full")->Get() < _spec.aoeHealCount)
        return false;

    return true;
}

bool SetBonusTrigger::IsActive()
{
    if (!IsWorn() || !Conditions())
        return false;

    uint32 const now = getMSTime();
    if (!_lastLogMs || getMSTimeDiff(_lastLogMs, now) >= ai::setbonus::LOG_INTERVAL_MS)
    {
        _lastLogMs = now;
        LOG_INFO("playerbots", "[setbonus-rot] {} '{}': {}", bot->GetName(), getName(), _spec.note);
    }

    return true;
}

namespace ai::setbonus
{
namespace
{
// Set bonus passives (the aura the item set applies).
std::vector<uint32> const WARRIOR_TANK_SHIELD_WALL_4P = { 124011, 125011, 126011, 127011 };
constexpr uint32 WARRIOR_TANK_E2_2P = 127510;
constexpr uint32 WARRIOR_TANK_E2_4P = 127511;
constexpr uint32 PALADIN_HOLY_E2_2P = 127520;
constexpr uint32 PALADIN_PROT_E1_4P = 127031;
std::vector<uint32> const DK_TANK_VAMPIRIC_BLOOD = { 124101, 125101, 126101, 127100 };
std::vector<uint32> const DK_TANK_ICEBOUND_FORTITUDE = { 124101, 125101, 126101, 127101 };
constexpr uint32 DK_TANK_E2_2P = 127600;
constexpr uint32 DK_TANK_E2_4P = 127601;
constexpr uint32 PRIEST_HEAL_E1_4P = 127071;
std::vector<uint32> const SHAMAN_ENH_MAELSTROM_2P = { 127130, 127630 };
constexpr uint32 MAGE_E2_2P = 127640;
constexpr uint32 WARLOCK_E2_2P = 127650;
constexpr uint32 DRUID_FERAL_E1_4P = 127181;
constexpr uint32 DRUID_FERAL_E1_2P = 127180;
std::vector<uint32> const HUNTER_HARD_4P = { 124051, 125051, 126051 };

// Proc and charge auras the bonuses put on the bot ("native" = the Blizzard spell, else a clone).
constexpr uint32 SPELL_SLAM_INSTANT = 46916;           // Slam! (native)
constexpr uint32 SPELL_SUDDEN_DEATH = 52437;           // native
constexpr uint32 SPELL_TASTE_FOR_BLOOD = 60503;        // native
constexpr uint32 SPELL_ASCENDANT_WHIRL = 127504;
constexpr uint32 SPELL_ASCENDANT_WARD = 127604;
constexpr uint32 SPELL_DIVINE_FAVOR = 20216;           // native
std::vector<uint32> const SPELL_SHOCKING_LIGHT = { 124024, 125024, 126024 };
constexpr uint32 SPELL_TITANS_LIGHT = 127024;
constexpr uint32 SPELL_TITANIC_JUDGEMENT = 127034;
constexpr uint32 SPELL_RIGHTEOUS_AEGIS = 127534;
std::vector<uint32> const SPELL_DIVINE_STORM_RESET = { 124044, 125044, 126044, 127044 };
constexpr uint32 SPELL_WRATH_OF_THE_TITANS = 127045;
constexpr uint32 SPELL_IMPROVED_STEADY_SHOT = 53220;   // native
constexpr uint32 SPELL_TITANS_FOCUS = 127054;
constexpr uint32 SPELL_LOCK_AND_LOAD = 56453;          // native
constexpr uint32 SPELL_ASCENDANT_EXECUTION = 127554;
constexpr uint32 SPELL_TITANIC_FLOW = 127064;
constexpr uint32 SPELL_SURGE_OF_LIGHT = 33151;         // native
constexpr uint32 SPELL_ASCENDANT_PRAYER = 127574;
constexpr uint32 SPELL_TITANIC_INSIGHT = 127084;
constexpr uint32 SPELL_ASCENDANT_VOID = 127584;
constexpr uint32 SPELL_ASCENDANT_POWER = 127594;
std::vector<uint32> const SPELL_TIDAL_SURGE = { 124114, 125114, 126114 };
constexpr uint32 SPELL_TITANIC_TIDE = 127114;
constexpr uint32 SPELL_ASCENDANT_TIDE = 127614;
constexpr uint32 SPELL_LAVA_SURGE = 127124;
constexpr uint32 SPELL_ASCENDANT_STORM = 127624;
constexpr uint32 SPELL_TITANIC_STRIKE = 127134;
constexpr uint32 SPELL_MAELSTROM_WEAPON = 53817;       // native
constexpr uint32 SPELL_ARCANE_SURGE = 127144;
constexpr uint32 SPELL_HOT_STREAK = 48108;             // native
constexpr uint32 SPELL_FINGERS_OF_FROST = 44544;       // native
constexpr uint32 SPELL_ARCANE_BLAST_STACK = 36032;     // native
constexpr uint32 SPELL_MISSILE_BARRAGE = 44401;        // native
constexpr uint32 SPELL_BACKLASH = 34936;               // native
constexpr uint32 SPELL_ASCENDANT_PACT = 127654;
constexpr uint32 ASCENDANT_PACT_ICD_MS = 15 * IN_MILLISECONDS;
constexpr uint32 SPELL_LUNAR_SURGE = 127174;
constexpr uint32 SPELL_ASCENDANT_FRENZY = 127684;
constexpr uint32 SPELL_CLEARCASTING_FERAL = 16870;     // native (Omen of Clarity)
constexpr uint32 SPELL_TITANIC_BLOOM = 127164;
constexpr uint32 SPELL_ASCENDANT_BLOOM = 127664;

// Health / rage / mana windows of the bonus-driven actions.
constexpr uint8 TASTE_FOR_BLOOD_MIN_RAGE = 5;    // Overpower cost
constexpr uint8 TASTE_FOR_BLOOD_MAX_RAGE = 30;   // a stance swap keeps 10 rage: do not throw away more than 20
constexpr uint8 LAST_STAND_BELOW_HEALTH = 80;
constexpr uint8 RUNE_TAP_BELOW_HEALTH = 85;
constexpr uint8 ASCENDANT_PRAYER_MIN_INJURED = 3;
constexpr uint8 TITANIC_TIDE_MIN_INJURED = 2;
constexpr uint8 MAELSTROM_WEAPON_MIN_STACKS = 4;
constexpr uint8 ARCANE_BLAST_MAX_STACKS = 4;
constexpr uint8 LIFE_TAP_ABOVE_HEALTH = 60;
constexpr uint8 LIFE_TAP_BELOW_MANA = 95;
constexpr uint8 FEROCIOUS_BITE_MIN_COMBO_POINTS = 4;

TriggerSpec Proc(std::vector<uint32> auras, std::string note)
{
    TriggerSpec spec;
    spec.auras = std::move(auras);
    spec.note = std::move(note);
    return spec;
}

TriggerSpec HealProc(std::vector<uint32> auras, HealGate gate, std::string note)
{
    TriggerSpec spec = Proc(std::move(auras), std::move(note));
    spec.healGate = gate;
    return spec;
}

TriggerSpec WornReady(std::vector<uint32> worn, std::string spell, std::string note)
{
    TriggerSpec spec;
    spec.worn = std::move(worn);
    spec.readySpell = std::move(spell);
    spec.note = std::move(note);
    return spec;
}

TriggerSpec TankCooldown(std::vector<uint32> worn, std::string spell, std::string note)
{
    TriggerSpec spec = WornReady(std::move(worn), std::move(spell), std::move(note));
    spec.tankingBoss = true;
    return spec;
}
}  // namespace

void RegisterTriggers(TriggerCreatorMap& creators)
{
    auto add = [&creators](std::string const& name, TriggerSpec const& spec)
    {
        creators[name] = [name, spec](PlayerbotAI* botAI) { return new SetBonusTrigger(botAI, name, spec); };
    };

    // Warrior dps: Slam! (hard 4p, E1 4p) also for Arms, Sudden Death (E1 2p) and Taste for Blood (E2 2p) also for
    // Fury, the Whirlwind charge (E2 2p).
    add("set bonus slam!", Proc({ SPELL_SLAM_INSTANT }, "instant Slam"));
    add("set bonus sudden death", Proc({ SPELL_SUDDEN_DEATH }, "Execute on Sudden Death"));
    {
        TriggerSpec spec = Proc({ SPELL_TASTE_FOR_BLOOD }, "battle stance -> Overpower on Taste for Blood");
        spec.readySpell = "overpower";
        spec.minRage = TASTE_FOR_BLOOD_MIN_RAGE;
        spec.maxRage = TASTE_FOR_BLOOD_MAX_RAGE;
        add("set bonus taste for blood", spec);
    }
    add("set bonus ascendant whirl", Proc({ SPELL_ASCENDANT_WHIRL }, "Whirlwind without cooldown"));

    // Warrior tank: Shield Wall (hard 4p, E1 4p) and Last Stand (E2 4p) on cooldown on a boss, Revenge first (E2 2p).
    add("set bonus shield wall tank",
        TankCooldown(WARRIOR_TANK_SHIELD_WALL_4P, "shield wall", "Shield Wall on cooldown while tanking a boss"));
    {
        TriggerSpec spec =
            TankCooldown({ WARRIOR_TANK_E2_4P }, "last stand", "Last Stand on cooldown while tanking a boss");
        spec.selfHealthBelow = LAST_STAND_BELOW_HEALTH;
        add("set bonus last stand tank", spec);
    }
    add("set bonus revenge", WornReady({ WARRIOR_TANK_E2_2P }, "revenge", "Revenge first (Ascendant Block)"));

    // Paladin holy: Divine Favor (hard 4p, E2 2p), Shocking Light (hard 2p), Titan's Light (E1 2p), Judgement on
    // cooldown for Divine Favor (E2 2p).
    add("set bonus divine favor", HealProc({ SPELL_DIVINE_FAVOR }, HealGate::MEDIUM, "Holy Light on Divine Favor"));
    add("set bonus shocking light", HealProc(SPELL_SHOCKING_LIGHT, HealGate::MEDIUM, "Holy Light on Shocking Light"));
    add("set bonus titan's light", HealProc({ SPELL_TITANS_LIGHT }, HealGate::MEDIUM, "instant Holy Light"));
    add("set bonus judgement divine favor",
        WornReady({ PALADIN_HOLY_E2_2P }, "judgement of light", "Judgement on cooldown for Divine Favor"));

    // Paladin protection: the Judgement charge (E1 2p), Avenger's Shield on cooldown (E1 4p), Righteous Aegis (E2 4p).
    add("set bonus titanic judgement", Proc({ SPELL_TITANIC_JUDGEMENT }, "Judgement without cooldown"));
    add("set bonus avenger's shield",
        WornReady({ PALADIN_PROT_E1_4P }, "avenger's shield", "Avenger's Shield on cooldown"));
    add("set bonus righteous aegis", Proc({ SPELL_RIGHTEOUS_AEGIS }, "Shield of Righteousness on Righteous Aegis"));

    // Paladin retribution: the Divine Storm charge (hard 4p, E1 2p), Hammer of Wrath on any target (E1 4p).
    add("set bonus divine storm reset", Proc(SPELL_DIVINE_STORM_RESET, "Divine Storm without cooldown"));
    add("set bonus wrath of the titans", Proc({ SPELL_WRATH_OF_THE_TITANS }, "Hammer of Wrath on any target"));

    // Hunter: Improved Steady Shot (hard 4p), the Aimed / Chimera / Explosive charge (E1 2p), Lock and Load outside
    // Survival (E1 4p), Kill Shot on any target (E2 2p).
    {
        // Gated on the set: the Marksmanship talent procs the same aura, its stock rotation stays as it is.
        TriggerSpec spec = Proc({ SPELL_IMPROVED_STEADY_SHOT }, "Aimed / Chimera / Arcane Shot on Improved Steady");
        spec.worn = HUNTER_HARD_4P;
        add("set bonus improved steady shot", spec);
    }
    add("set bonus titan's focus", Proc({ SPELL_TITANS_FOCUS }, "Aimed / Chimera / Explosive Shot without cooldown"));
    add("set bonus lock and load", Proc({ SPELL_LOCK_AND_LOAD }, "Arcane Shot on Lock and Load"));
    add("set bonus ascendant execution", Proc({ SPELL_ASCENDANT_EXECUTION }, "Kill Shot on any target"));

    // Rogue: the free combo point builder (E1 4p).
    add("set bonus titanic flow", Proc({ SPELL_TITANIC_FLOW }, "free combo point builder"));

    // Priest healer: Surge of Light (hard 4p) also for Discipline, instant Prayer of Healing (E2 4p), Guardian Spirit
    // and Pain Suppression earlier with the shorter cooldown (E1 4p).
    add("set bonus surge of light",
        HealProc({ SPELL_SURGE_OF_LIGHT }, HealGate::ALMOST_FULL, "Flash Heal on Surge of Light"));
    {
        TriggerSpec spec = Proc({ SPELL_ASCENDANT_PRAYER }, "instant Prayer of Healing");
        spec.aoeHealCount = ASCENDANT_PRAYER_MIN_INJURED;
        add("set bonus ascendant prayer", spec);
    }
    {
        TriggerSpec spec;
        spec.worn = { PRIEST_HEAL_E1_4P };
        spec.healGate = HealGate::LOW;
        spec.note = "Guardian Spirit / Pain Suppression at low health";
        add("set bonus guardian spirit", spec);
    }

    // Priest shadow: the Mind Blast charge (E1 2p) and the Shadow Word: Death charge (E2 2p).
    add("set bonus titanic insight", Proc({ SPELL_TITANIC_INSIGHT }, "Mind Blast without cooldown"));
    add("set bonus ascendant void", Proc({ SPELL_ASCENDANT_VOID }, "Shadow Word: Death without cooldown"));

    // Death knight dps: the free Frost Strike / Death Coil (E2 2p).
    add("set bonus ascendant power", Proc({ SPELL_ASCENDANT_POWER }, "free Frost Strike / Death Coil"));

    // Death knight tank: Vampiric Blood (hard 4p, E1 2p) and Icebound Fortitude (hard 4p, E1 4p) on cooldown on a
    // boss, Rune Tap earlier (E2 4p), Death Strike for Ascendant Ward (E2 2p).
    add("set bonus vampiric blood tank",
        TankCooldown(DK_TANK_VAMPIRIC_BLOOD, "vampiric blood", "Vampiric Blood on cooldown while tanking a boss"));
    add("set bonus icebound fortitude tank", TankCooldown(DK_TANK_ICEBOUND_FORTITUDE, "icebound fortitude",
                                                          "Icebound Fortitude on cooldown while tanking a boss"));
    {
        TriggerSpec spec = TankCooldown({ DK_TANK_E2_4P }, "rune tap", "Rune Tap on cooldown while tanking a boss");
        spec.selfHealthBelow = RUNE_TAP_BELOW_HEALTH;
        add("set bonus rune tap tank", spec);
    }
    {
        TriggerSpec spec = WornReady({ DK_TANK_E2_2P }, "death strike", "Death Strike for Ascendant Ward");
        spec.absent = { SPELL_ASCENDANT_WARD };
        add("set bonus death strike ward", spec);
    }

    // Shaman restoration: the Riptide charge (hard 4p), instant Chain Heal (E1 4p), instant Healing Wave (E2 4p).
    add("set bonus tidal surge", HealProc(SPELL_TIDAL_SURGE, HealGate::ALMOST_FULL, "Riptide without cooldown"));
    {
        TriggerSpec spec = Proc({ SPELL_TITANIC_TIDE }, "instant Chain Heal");
        spec.aoeHealCount = TITANIC_TIDE_MIN_INJURED;
        add("set bonus titanic tide", spec);
    }
    add("set bonus ascendant tide", HealProc({ SPELL_ASCENDANT_TIDE }, HealGate::MEDIUM, "instant Healing Wave"));

    // Shaman elemental: the Lava Burst charge (E1 2p), instant Chain Lightning on a single target too (E2 2p).
    add("set bonus lava surge", Proc({ SPELL_LAVA_SURGE }, "Lava Burst without cooldown"));
    add("set bonus ascendant storm", Proc({ SPELL_ASCENDANT_STORM }, "instant Chain Lightning"));

    // Shaman enhancement: the Stormstrike charge (E1 4p), Lightning Bolt from 4 Maelstrom Weapon stacks with the
    // extra stacks of E1 2p / E2 2p.
    add("set bonus titanic strike", Proc({ SPELL_TITANIC_STRIKE }, "Stormstrike without cooldown"));
    {
        TriggerSpec spec;
        spec.worn = SHAMAN_ENH_MAELSTROM_2P;
        spec.stackAura = SPELL_MAELSTROM_WEAPON;
        spec.minStacks = MAELSTROM_WEAPON_MIN_STACKS;
        spec.note = "Lightning Bolt at 4 Maelstrom Weapon stacks";
        add("set bonus maelstrom weapon 4", spec);
    }

    // Mage: the instant nuke (E1 2p); Fire Blast for Hot Streak, Ice Lance on Fingers of Frost for Brain Freeze and
    // Arcane Barrage for Missile Barrage (E2 2p).
    add("set bonus arcane surge", Proc({ SPELL_ARCANE_SURGE }, "instant main nuke"));
    {
        TriggerSpec spec = WornReady({ MAGE_E2_2P }, "fire blast", "Fire Blast for Hot Streak");
        spec.absent = { SPELL_HOT_STREAK };
        add("set bonus fire blast", spec);
    }
    {
        TriggerSpec spec = Proc({ SPELL_FINGERS_OF_FROST }, "Ice Lance on Fingers of Frost for Brain Freeze");
        spec.worn = { MAGE_E2_2P };
        add("set bonus ice lance", spec);
    }
    {
        TriggerSpec spec =
            WornReady({ MAGE_E2_2P }, "arcane barrage", "Arcane Barrage at 4 Arcane Blast stacks for Missile Barrage");
        spec.stackAura = SPELL_ARCANE_BLAST_STACK;
        spec.minStacks = ARCANE_BLAST_MAX_STACKS;
        spec.absent = { SPELL_MISSILE_BARRAGE };
        add("set bonus arcane barrage", spec);
    }

    // Warlock: instant Shadow Bolt / Incinerate on Backlash (E1 2p) or Ascendant Pact (E2 2p), Life Tap for the pact
    // whenever its internal cooldown is over (E2 2p).
    add("set bonus backlash", Proc({ SPELL_BACKLASH, SPELL_ASCENDANT_PACT }, "instant Shadow Bolt / Incinerate"));
    {
        TriggerSpec spec;
        spec.worn = { WARLOCK_E2_2P };
        spec.icdAura = SPELL_ASCENDANT_PACT;
        spec.icdMs = ASCENDANT_PACT_ICD_MS;
        spec.selfHealthAbove = LIFE_TAP_ABOVE_HEALTH;
        spec.maxManaPct = LIFE_TAP_BELOW_MANA;
        spec.note = "Life Tap for Ascendant Pact";
        add("set bonus ascendant pact", spec);
    }

    // Druid: instant Starfire (E1 2p), the free Ferocious Bite / Maul (E2 2p), Maul on Clearcasting for the bear
    // (E1 2p), Survival Instincts on cooldown on a boss (E1 4p), the Swiftmend (E1 2p) and Regrowth (E2 2p) charges.
    add("set bonus lunar surge", Proc({ SPELL_LUNAR_SURGE }, "instant Starfire"));
    {
        TriggerSpec spec = Proc({ SPELL_ASCENDANT_FRENZY }, "free Ferocious Bite");
        spec.minComboPoints = FEROCIOUS_BITE_MIN_COMBO_POINTS;
        add("set bonus ascendant frenzy bite", spec);
    }
    add("set bonus ascendant frenzy maul", Proc({ SPELL_ASCENDANT_FRENZY }, "free Maul"));
    {
        // Gated on the set: Omen of Clarity procs the same aura, the stock bear rotation stays as it is.
        TriggerSpec spec = Proc({ SPELL_CLEARCASTING_FERAL }, "Maul on Clearcasting");
        spec.worn = { DRUID_FERAL_E1_2P };
        add("set bonus clearcasting maul", spec);
    }
    add("set bonus survival instincts tank", TankCooldown({ DRUID_FERAL_E1_4P }, "survival instincts",
                                                          "Survival Instincts on cooldown while tanking a boss"));
    add("set bonus titanic bloom", HealProc({ SPELL_TITANIC_BLOOM }, HealGate::MEDIUM, "Swiftmend without cooldown"));
    add("set bonus ascendant bloom", HealProc({ SPELL_ASCENDANT_BLOOM }, HealGate::MEDIUM, "instant Regrowth"));
}
}  // namespace ai::setbonus
