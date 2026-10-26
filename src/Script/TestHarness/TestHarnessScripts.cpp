/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cmath>
#include <mutex>
#include <unordered_map>

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
                      UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_ON_UNIT_ENTER_COMBAT})
    {
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

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage,
                                       SpellInfo const* spellInfo) override
    {
        // Also called for periodic heals (before ModifyHealReceived): only harmful ticks count as damage.
        if (spellInfo && !spellInfo->IsPositive())
            TestHarness::StatsRegistry::Instance().OnSpellAmount(attacker, target, spellInfo, damage, false);
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
