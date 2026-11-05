/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cmath>
#include <mutex>
#include <unordered_map>

#include "SpellAuraEffects.h"  // [lead111] [sting-tick]
#include "Creature.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SelfTravelActions.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "TestHarnessLog.h"
#include "TestHarnessMgr.h"
#include "TestRunnerStrategy.h"
#include "Timer.h"

namespace TestHarness
{
    // [b31_harness] Cast item of a player cast, from the start of Spell::_cast to its end. Spell::_cast runs on one
    // thread start to end, and a nested cast (a linked or triggered spell inside it) has its own Spell object, so a
    // per-thread map by Spell* is enough. A cast that fails in between leaves its entry; the next _cast of a Spell at
    // that address overwrites it first (every player _cast passes NoteCastStart), and the map is capped.
    namespace
    {
        thread_local std::unordered_map<Spell const*, uint32> castItemAtStart;
        constexpr size_t CAST_ITEM_MAP_CAP = 512;
    }

    void NoteCastStart(Spell const* spell)
    {
        if (!spell)
            return;
        if (castItemAtStart.size() >= CAST_ITEM_MAP_CAP)
            castItemAtStart.clear();
        castItemAtStart[spell] = spell->m_CastItem ? spell->m_CastItem->GetEntry() : 0;
    }

    uint32 TakeCastItemEntry(Spell const* spell)
    {
        auto itr = castItemAtStart.find(spell);
        if (itr == castItemAtStart.end())
            return spell->m_CastItem ? spell->m_CastItem->GetEntry() : 0;
        uint32 const entry = itr->second;
        castItemAtStart.erase(itr);
        return entry;
    }
}

// Hooks of the headless dungeon / raid test harness: the run state machine on the world thread (after the maps
// have updated), and damage / healing / casts / deaths counted on the map threads.
class PlayerbotsTestHarnessWorldScript : public WorldScript
{
public:
    PlayerbotsTestHarnessWorldScript()
        : WorldScript("PlayerbotsTestHarnessWorldScript", {WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnUpdate(uint32 diff) override { TestHarness::TestHarnessMgr::Instance().Update(diff); }

    void OnShutdown() override
    {
        TestHarness::TestHarnessMgr::Instance().Shutdown();
        TestRunner::ClearAll();
        SelfTravel::ClearAll();
    }
};

class PlayerbotsTestHarnessUnitScript : public UnitScript
{
public:
    PlayerbotsTestHarnessUnitScript()
        : UnitScript("PlayerbotsTestHarnessUnitScript", true,
                     {UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_ON_UNIT_DEATH,
                      UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
                      UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_ON_UNIT_ENTER_COMBAT, UNITHOOK_ON_POWER_GAIN})
    {
    }

    // [harness20] the member's power per source (rage from damage dealt / taken, energize spells and ticks)
    void OnPowerGain(Unit* unit, uint8 power, int32 gain, uint8 source, uint32 spellId) override
    {
        TestHarness::StatsRegistry::Instance().OnPowerGain(unit, power, gain, source, spellId);
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        TestHarness::StatsRegistry::Instance().OnDamage(attacker, victim, damage);
    }

    void OnHeal(Unit* healer, Unit* reciever, uint32& gain) override
    {
        TestHarness::StatsRegistry::Instance().OnHeal(healer, reciever, gain);
    }

    // Per-spell amounts for the rotation review; the hooks only read the value.
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage > 0)
            TestHarness::StatsRegistry::Instance().OnSpellAmount(attacker, target, spellInfo, uint32(damage), false);
    }

    // [rot65] A dynamic object's periodic damage (Consecration, Death and Decay, Blizzard, Rain of Fire, Hurricane,
    // Volley) reaches this hook without the caster's spell power / attack power bonus: AuraEffect::
    // HandlePeriodicDamageAurasTick adds SpellDamageBonusDone after the hook for DYNOBJ_AURA_TYPE only (a unit aura has
    // it in its amount). b201 rfc-h 5G: the ret paladin's Consecration logged 1130 a cast = 10 ticks of the bare 113
    // (sim 4349); the missing part went to the white-swing remainder. The tick is logged with that bonus added, as
    // every other spell row (before the target's mitigation and the crit).
    static uint32 DynObjTickWithCasterBonus(Unit* target, Unit* attacker, uint32 damage, SpellInfo const* spellInfo)
    {
        if (!target || !attacker || !damage)
            return damage;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA &&
                spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE)
                return attacker->SpellDamageBonusDone(target, spellInfo, damage, DOT, i);

        return damage;
    }

    // [rot530] A periodic damage tick reaches this hook before the target's damage-taken modifiers
    // (AuraEffect::HandlePeriodicDamageAurasTick applies Unit::SpellDamageBonusTaken after it), a direct spell's
    // amount at ModifySpellDamageTaken after them; simpct.py turns both into dealt damage with the sim's
    // AfterPostOutcome / (AfterAttackerMods x target mods) ratio, i.e. it expects the target's modifiers in the logged
    // amount. b266 rfc-h 5D: the affliction warlock's Corruption, Unstable Affliction and Curse of Agony came out at
    // 55-61 % of the sim per cast, the missing part Haunt (+23 % glyphed), Shadow Embrace (+15 %) and Ebon Plague
    // (+13 %), all taken modifiers. The tick is logged with them, as the core deals it (Ignite and Languish take none).
    static uint32 TickWithTargetMods(Unit* target, Unit* attacker, uint32 damage, SpellInfo const* spellInfo)
    {
        if (!target || !damage || spellInfo->Id == 12654 || spellInfo->Id == 71023)
            return damage;

        bool periodicDamage = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE)
                periodicDamage = true;
        if (!periodicDamage)
            return damage;

        Aura const* aura = attacker ? target->GetAura(spellInfo->Id, attacker->GetGUID()) : nullptr;
        uint32 const stack = aura ? std::max<uint32>(1, aura->GetStackAmount()) : 1;
        return target->SpellDamageBonusTaken(attacker, spellInfo, damage, DOT, stack);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage,
                                       SpellInfo const* spellInfo) override
    {
        // Also called for periodic heals (before ModifyHealReceived): only harmful ticks count as damage.
        if (spellInfo && !spellInfo->IsPositive())
        {
            uint32 const logged =
                TickWithTargetMods(target, attacker, DynObjTickWithCasterBonus(target, attacker, damage, spellInfo),
                                   spellInfo);  // [rot65] [rot530]
            TestHarness::StatsRegistry::Instance().OnSpellAmount(attacker, target, spellInfo, logged, false);
        }
        // [lead111] b143 rfc-h Taragaman: Serpent Sting and Chimera Shot - Serpent dealt about half the sim's while
        // the formulas match (242 + 0.04 RAP a tick); the tick, the caster's ranged attack power and the aura's
        // ticks, once per caster every 10 s, for the check against the sim
        if (spellInfo && attacker && attacker->IsPlayer() && spellInfo->SpellFamilyName == SPELLFAMILY_HUNTER &&
            (spellInfo->SpellFamilyFlags[0] & 0x4000))
        {
            thread_local std::unordered_map<ObjectGuid, uint32> logged;
            uint32 const now = getMSTime();
            uint32& last = logged[attacker->GetGUID()];
            if (!last || getMSTimeDiff(last, now) >= 10 * IN_MILLISECONDS)
            {
                last = now ? now : 1;
                AuraEffect const* effect =
                    target ? target->GetAuraEffect(spellInfo->Id, EFFECT_0, attacker->GetGUID()) : nullptr;
                LOG_INFO("playerbots", "[sting-tick] {}: {} on {}: tick {} (aura amount {}, ticks {}, duration {} ms), "
                         "ranged AP {:.0f}, melee AP {:.0f}", attacker->GetName(), spellInfo->SpellName[0],
                         target ? target->GetName() : "?", damage, effect ? effect->GetAmount() : -1,
                         effect ? effect->GetTotalTicks() : -1, effect ? effect->GetBase()->GetMaxDuration() : -1,
                         attacker->GetTotalAttackPowerValue(RANGED_ATTACK),
                         attacker->GetTotalAttackPowerValue(BASE_ATTACK));
            }
        }
    }

    void ModifyHealReceived(Unit* target, Unit* healer, uint32& heal, SpellInfo const* spellInfo) override
    {
        TestHarness::StatsRegistry::Instance().OnSpellAmount(healer, target, spellInfo, heal, true);
    }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        TestHarness::StatsRegistry::Instance().OnUnitDeath(unit, killer);
    }

    // Who started a fight with a dungeon mob beyond pull range or on another floor (Scholomance wave 18: the pit 24 yd
    // under the entrance bridge came up with no pull of the tank). Called once per creature engage; an assist
    // chain engages on the same victim, so the first lines of a burst name the trigger. At most a few per instance.
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        TestHarness::StatsRegistry::Instance().OnCreatureEngage(unit, victim);

        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !victim || !creature->GetMap() || !creature->GetMap()->IsDungeon())
            return;

        Player* owner = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!owner)
            return;

        float const dz = victim->GetPositionZ() - creature->GetPositionZ();
        float const distance = creature->GetExactDist(victim);
        if (std::fabs(dz) < FAR_ENGAGE_HEIGHT && distance < FAR_ENGAGE_DISTANCE)
            return;

        {
            static std::mutex lock;
            static std::unordered_map<uint32, std::pair<uint32, uint32>> recent;  // instance -> window start, lines
            uint32 const now = getMSTime();
            std::lock_guard<std::mutex> guard(lock);
            auto& [start, lines] = recent[creature->GetInstanceId()];
            if (!start || getMSTimeDiff(start, now) > FAR_ENGAGE_WINDOW_MS)
            {
                start = now;
                lines = 0;
            }
            if (++lines > FAR_ENGAGE_LINES)
                return;
        }

        uint32 spell = 0;
        if (Spell const* current = owner->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            spell = current->GetSpellInfo()->Id;
        else if (Spell const* channel = owner->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            spell = channel->GetSpellInfo()->Id;

        LOG_INFO("playerbots", "[far engage] {} ({}) at {:.1f} {:.1f} {:.1f} engaged {}{} at {:.1f} {:.1f} {:.1f}: "
                 "dz {:.1f}, {:.1f} yd; {} casts {}, targets {}", creature->GetName(), creature->GetSpawnId(),
                 creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), victim->GetName(),
                 victim == owner ? "" : " (pet of " + owner->GetName() + ")", victim->GetPositionX(),
                 victim->GetPositionY(), victim->GetPositionZ(), dz, distance, owner->GetName(), spell,
                 owner->GetVictim() ? owner->GetVictim()->GetName() : std::string("nothing"));
    }

private:
    static constexpr float FAR_ENGAGE_HEIGHT = 8.0f;
    static constexpr float FAR_ENGAGE_DISTANCE = 42.0f;  // beyond the tank's 40-yard pull range
    static constexpr uint32 FAR_ENGAGE_WINDOW_MS = 30 * IN_MILLISECONDS;
    static constexpr uint32 FAR_ENGAGE_LINES = 6;
};

class PlayerbotsTestHarnessPlayerScript : public PlayerScript
{
public:
    PlayerbotsTestHarnessPlayerScript()
        : PlayerScript("PlayerbotsTestHarnessPlayerScript",
                       {PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_PLAYER_ENTER_COMBAT})
    {
    }

    // The order the members entered combat, for "who aggroed first" (pull_start / pull_end).
    void OnPlayerEnterCombat(Player* player, Unit* enemy) override
    {
        TestHarness::StatsRegistry::Instance().OnPlayerEnterCombat(player, enemy);
    }

    // [b31_harness] The START of Spell::_cast (before the final CheckCast and the target selection): nothing is
    // counted here, the cast may still fail. Only the cast item is noted, because Spell::m_CastItem is gone by the
    // end of _cast for a consumed item (TakeCastItem); PlayerbotsTestHarnessSpellScript counts the cast.
    void OnPlayerSpellCast(Player* /*player*/, Spell* spell, bool /*skipCheck*/) override
    {
        TestHarness::NoteCastStart(spell);
    }
};

// [b31_harness] The END of Spell::_cast (ALLSPELLHOOK_ON_CAST, after TakePower / SendSpellCooldown / SMSG_SPELL_GO):
// only a cast that went off reaches it, so the cast mix (pull_end / run_end "casts", busyPct) counts real casts.
class PlayerbotsTestHarnessSpellScript : public AllSpellScript
{
public:
    PlayerbotsTestHarnessSpellScript()
        : AllSpellScript("PlayerbotsTestHarnessSpellScript", {ALLSPELLHOOK_ON_CAST})
    {
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*skipCheck*/) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || !spell)
            return;

        TestHarness::StatsRegistry::Instance().OnSpellCast(player, spell, TestHarness::TakeCastItemEntry(spell));
    }
};

void AddPlayerbotsTestHarness()
{
    new PlayerbotsTestHarnessWorldScript();
    new PlayerbotsTestHarnessUnitScript();
    new PlayerbotsTestHarnessPlayerScript();
    new PlayerbotsTestHarnessSpellScript();
}
