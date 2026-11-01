/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HunterTriggers.h"
#include "HealthTriggers.h"  // [lead85] RestState::OwnManaLine
#include "LeaderOrders.h"    // [lead85] Leader::LedGroup
#include "DynamicObject.h"  // [guide_b31_dead]
#include "GenericBuffUtils.h"
#include "GenericSpellActions.h"
#include "GenericTriggers.h"
#include "HunterActions.h"
#include "Pet.h"  // [guide_b31_dead]
#include "PetDefines.h"             // [b31_deep] PetStable
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"  // [b31_deep]
#include "SharedDefines.h"
#include "TargetValue.h"

constexpr uint8 ASPECT_OF_THE_VIPER_MANA_PCT = 10;
// [b31_deep] wowsims hunter default: back from Viper at 30 % mana (was 60: Viper halves shot damage, 10 -> 60 %
// spent twice as long in it as 10 -> 30 %)
constexpr uint8 ASPECT_OF_THE_DRAGONHAWK_MANA_PCT = 30;
// [b31_deep] Misdirection window: the opening of a pull, or our threat closing on the tank's
constexpr uint32 MISDIRECTION_PULL_WINDOW_SECONDS = 10;
constexpr uint8 MISDIRECTION_THREAT_PCT = 70;
// [b31_deep] Beast Mastery revives a pet that died on a boss only with this much fight left (DEEP_AUDIT_HUNTER)
constexpr float REVIVE_PET_MIN_TARGET_LIFETIME = 30.0f;

// [guide_b31_dead] Kill Command buffs the PET, never the hunter, so the old self-aura check was always true.
// Now: spell off cooldown, a live pet fighting a target, and the pet's Kill Command aura not already up.
bool KillCommandTrigger::IsActive()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", "kill command");
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive() || !pet->GetVictim())
        return false;

    return !botAI->HasAura("kill command", pet);
}

// [guide_b31_dead]
bool IntimidationTrigger::IsActive()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", "intimidation");
    if (!spellId || bot->HasSpellCooldown(spellId))
        return false;

    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive())
        return false;

    Unit* victim = pet->GetVictim();
    if (!victim || !victim->IsAlive())
        return false;

    if (victim->HasUnitState(UNIT_STATE_CASTING))
        return true;

    Unit* victimTarget = victim->GetVictim();
    if (!victimTarget)
        return false;

    if (victimTarget == bot)
        return true;

    // Healer check only for bots (a strategy lookup); a real player's spec would need a talent scan.
    Player* player = victimTarget->ToPlayer();
    return player && GET_PLAYERBOT_AI(player) && PlayerbotAI::IsHeal(player);
}

bool BlackArrowTrigger::IsActive()
{
    if (botAI->HasStrategy("trap weave", BOT_STATE_COMBAT))
        return false;

    return DebuffTrigger::IsActive();
}

bool HunterAspectOfTheDragonhawkTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    if (!SpellTrigger::IsActive())
        return false;

    if (botAI->HasAura("aspect of the hawk", target) ||
        botAI->HasAura("aspect of the dragonhawk", target))
        return false;

    if (botAI->HasAura("aspect of the viper", target))
    {
        uint8 const mana = AI_VALUE2(uint8, "mana", "self target");
        if (mana < ASPECT_OF_THE_DRAGONHAWK_MANA_PCT)
            return false;
        // [lead85] a led group's fight: back once the mana covers its own line after the rest of this fight (the
        // measured spend over the target's remaining seconds), else it would drink after the fight
        if (bot->IsInCombat() && Leader::LedGroup(bot))
        {
            Unit* fought = AI_VALUE(Unit*, "current target");
            float const groupDps = AI_VALUE(float, "estimated group dps");
            float const left = fought && groupDps > 0.0f ? float(fought->GetHealth()) / groupDps : 0.0f;
            float const spend = std::max(0.0f, RestState::FightManaLine(bot, left, 0.0f) -
                                                   RestState::FightManaLine(bot, 0.0f, 0.0f));
            return float(mana) >= RestState::OwnManaLine(bot) + spend;
        }
        return true;
    }

    return true;
}

bool HunterNoStingsActiveTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!DebuffTrigger::IsActive() || !target)
        return false;

    if (botAI->HasAura("scorpid sting", target, false, true) || botAI->HasAura("viper sting", target, false, true))
        return false;

    // Stings from different hunters stack, so only our own Serpent Sting counts, refreshed before it expires
    Aura* serpentSting = botAI->GetAura("serpent sting", target, true);
    // [guide_b31_dead] Chimera Shot (53209) refreshes our Serpent Sting: with it, only re-apply a missing sting
    uint32 const refreshMs = bot->HasSpell(SPELL_CHIMERA_SHOT) ? 0 : SERPENT_STING_REFRESH_MS;
    return ai::buff::BuffBelowRefreshTarget(botAI, serpentSting, refreshMs);
}

bool HuntersPetDeadTrigger::IsActive()
{
    return AI_VALUE(bool, "pet dead") && !AI_VALUE2(bool, "mounted", "self target");
}

// [b31_deep] Hunter pets stay as a corpse when they die (Pet.cpp), so a summoned dead pet is the usual case; a
// pet stored dead (Health 0) is revived the same way.
bool HuntersPetDeadOnBossTrigger::IsActive()
{
    if (AI_VALUE2(bool, "mounted", "self target") || bot->GetTemporaryUnsummonedPetNumber())
        return false;

    if (Pet* pet = bot->GetPet())
    {
        if (pet->IsAlive())
            return false;
    }
    else
    {
        PetStable const* stable = bot->GetPetStable();
        if (!stable || !stable->CurrentPet || stable->CurrentPet->Health)
            return false;
    }

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || !ai::raidutil::IsBossUnit(target))
        return false;

    float const groupDps = AI_VALUE(float, "estimated group dps");
    return groupDps > 0.0f && target->GetHealth() / groupDps >= REVIVE_PET_MIN_TARGET_LIFETIME;
}

// [b31_deep]
bool HuntersPetDismissedTrigger::IsActive()
{
    if (bot->GetPet() || bot->GetTemporaryUnsummonedPetNumber() || AI_VALUE2(bool, "mounted", "self target"))
        return false;

    if (botAI->IsInVehicle())
        return false;

    PetStable const* stable = bot->GetPetStable();
    return stable && stable->CurrentPet && stable->CurrentPet->Health;
}

bool HuntersPetLowHealthTrigger::IsActive()
{
    Unit* pet = AI_VALUE(Unit*, "pet target");
    return pet && AI_VALUE2(uint8, "health", "pet target") < 40 &&
           !AI_VALUE2(bool, "dead", "pet target") &&
           !AI_VALUE2(bool, "mounted", "self target");
}

bool HuntersPetMediumHealthTrigger::IsActive()
{
    Unit* pet = AI_VALUE(Unit*, "pet target");
    return pet && AI_VALUE2(uint8, "health", "pet target") < sPlayerbotAIConfig.MediumHealth &&
           !AI_VALUE2(bool, "dead", "pet target") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HunterPetNotHappy::IsActive()
{
    return !AI_VALUE(bool, "pet happy") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HunterAspectOfTheViperTrigger::IsActive()
{
    if (botAI->HasStrategy("rnature", BotState::BOT_STATE_COMBAT) ||
        botAI->HasStrategy("rnature", BotState::BOT_STATE_NON_COMBAT) ||
        botAI->HasStrategy("bspeed", BotState::BOT_STATE_COMBAT) ||
        botAI->HasStrategy("bspeed", BotState::BOT_STATE_NON_COMBAT))
        return false;

    if (!BuffTrigger::IsActive())
        return false;
    uint8 const mana = AI_VALUE2(uint8, "mana", "self target");
    if (mana < ASPECT_OF_THE_VIPER_MANA_PCT)
        return true;
    // [lead85] b128 rfc-h: Rthunaa drank 9 times (26-45 % mana, net spend 0.8-1.2 %/s) - in a led group's fight a
    // hunter below its own computed line (what its next fight needs) would drink after this fight: Viper now
    return bot->IsInCombat() && Leader::LedGroup(bot) && float(mana) < RestState::OwnManaLine(bot);
}

bool HunterAspectOfThePackTrigger::IsActive()
{
    return BuffTrigger::IsActive() && !botAI->HasAura("aspect of the cheetah", GetTarget());
};

bool HunterLowAmmoTrigger::IsActive()
{
    uint32 ammoCount = AI_VALUE2(uint32, "item count", "ammo");
    return bot->GetGroup() && ammoCount > 0 && ammoCount < 100;
}

bool HunterHasAmmoTrigger::IsActive()
{
    return !AmmoCountTrigger::IsActive();
}

// Valid targets for "Improved Tracking".
// Optional/Utility targets (uncomment for SelfBot).
bool NoTrackTrigger::IsActive()
{
    std::vector<std::string> track_list = {
        "track beasts",
        "track demons",
        "track dragonkin",
        "track elementals",
        "track giants",
        "track humanoids",
        "track undead",
        // "track hidden",
        // "find herbs",
        // "find minerals",
        // "find fish",
        // "find treasure",
    };

    for (auto &track: track_list)
    {
        if (botAI->HasAura(track, bot))
            return false;
    }

    return true;
}

bool SerpentStingOnAttackerTrigger::IsActive()
{
    if (!DebuffOnAttackerTrigger::IsActive())
        return false;

    Unit* target = GetTarget();
    if (!target)
        return false;

    return !botAI->HasAura("scorpid sting", target, false, true) &&
           !botAI->HasAura("viper sting", target, false, true);
}

const std::set<uint32> VolleyChannelCheckTrigger::VOLLEY_SPELL_IDS =
{
    1510,   // Volley Rank 1
    14294,  // Volley Rank 2
    14295,  // Volley Rank 3
    27022,  // Volley Rank 4
    58431,  // Volley Rank 5
    58434   // Volley Rank 6
};

bool VolleyChannelCheckTrigger::IsActive()
{
    if (Spell* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        spell && VOLLEY_SPELL_IDS.count(spell->m_spellInfo->Id))
    {
        // [guide_b31_dead] count attackers inside the Volley area (as HurricaneChannelCheckTrigger does), not
        // within SightDistance of the hunter, which almost never dropped below the threshold.
        DynamicObject* dynObj = nullptr;
        for (uint32 spellId : VOLLEY_SPELL_IDS)
        {
            dynObj = bot->GetDynObject(spellId);
            if (dynObj)
                break;
        }

        if (!dynObj)
            return false;

        return !HasEnoughAoeTargets(botAI, dynObj->GetPosition(), dynObj->GetRadius(), minEnemies);
    }

    return false;
}

bool MisdirectionOnMainTankTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || target->IsPlayer())
        return false;  // players have no threat table

    Unit* mainTank = GetTarget();
    if (!mainTank || mainTank == bot)
        return false;

    // [b31_deep] The aura sits on the hunter (see SPELL_MISDIRECTION), so the base check on the tank was always true
    // and Misdirection went out on every cooldown (1.7-2.0/min, 4-6 % of the fight on its GCD).
    if (bot->HasAura(SPELL_MISDIRECTION) || bot->HasAura(SPELL_MISDIRECTION_TRANSFER))
        return false;

    if (!BuffOnMainTankTrigger::IsActive())
        return false;

    return ai::raidutil::GetCombatSeconds(botAI) <= MISDIRECTION_PULL_WINDOW_SECONDS ||
           AI_VALUE2(uint8, "threat", "current target") >= MISDIRECTION_THREAT_PCT;
}

// [b35_bal] see the header: Rapid Fire on the boss, not on instance trash (same pacing as Bloodlust/Heroism)
bool RapidFireTrigger::IsActive()
{
    if (!BuffTrigger::IsActive())
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive())
        return false;

    if (target->ToPlayer() || ai::raidutil::IsBurstWindow(botAI))
        return true;
    if (ai::raidutil::ShouldHoldBurstForLust(botAI))
        return false;

    if (ai::raidutil::IsInstanceMap(bot) || ai::raidutil::IsBossUnit(target))
        return ai::raidutil::IsBossEngaged(botAI, target);

    // outdoors / solo: the old "losing this fight" heuristic of BoostTrigger
    return AI_VALUE(uint8, "balance") <= balance;
}
