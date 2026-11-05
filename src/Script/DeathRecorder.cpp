/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Creature.h"
#include "DamageTaken.h"
#include "EncounterValues.h"
#include "EstimatedLifetimeValue.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <algorithm>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Records, for every death inside a dungeon or raid, what led to it: the damage and healing taken in the
// last seconds, the auras, and the state of the group and the boss. One JSON object per line in
// <LogsDir>/deaths.jsonl, for later analysis of each wipe.
namespace
{
    constexpr uint32 DEATH_LOG_WINDOW_MS = 15 * IN_MILLISECONDS;
    constexpr size_t DEATH_LOG_MAX_EVENTS = 400;

    struct CombatEvent
    {
        uint32 time;
        bool heal;
        std::string source;
        uint32 sourceEntry;
        uint32 spellId;  // 0 for melee
        int32 amount;
        uint32 health;
        uint32 maxHealth;
        bool inCombat;  // false mid-encounter shows the combat drop between waves/phases (AC #28026)
        bool environment = false;  // drowning / fall / lava / slime / fatigue: source holds the kind
    };

    struct TrailPoint
    {
        uint32 time;
        float x, y, z;
        bool falling;
    };

    constexpr uint32 TRAIL_SAMPLE_MS = 500;

    // The core drops a dying player from every threat list before OnPlayerJustDied, so the enemies around the
    // player are snapshotted while it is still alive and the last snapshot is written with the death.
    struct EnemySnapshot
    {
        std::string name;
        uint32 entry;
        float hpPct, x, y, z, o, dist;
        std::string target;
        uint32 casting;
    };

    std::mutex eventsLock;
    std::unordered_map<ObjectGuid, std::deque<CombatEvent>> recentEvents;
    std::unordered_map<ObjectGuid, std::deque<TrailPoint>> recentTrail;
    std::unordered_map<ObjectGuid, std::vector<EnemySnapshot>> lastEnemies;
    std::unordered_map<ObjectGuid, uint32> sinceLastSample;
    std::mutex fileLock;

    void SampleTrail(Player* player, uint32 diff)
    {
        std::lock_guard<std::mutex> lock(eventsLock);
        uint32& elapsed = sinceLastSample[player->GetGUID()];
        elapsed += diff;
        if (elapsed < TRAIL_SAMPLE_MS)
            return;
        elapsed = 0;

        std::deque<TrailPoint>& trail = recentTrail[player->GetGUID()];
        uint32 const now = getMSTime();
        trail.push_back({now, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                         player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING)});
        while (!trail.empty() && getMSTimeDiff(trail.front().time, now) > DEATH_LOG_WINDOW_MS)
            trail.pop_front();

        std::vector<EnemySnapshot> enemies;
        for (auto const& [guid, ref] : player->GetThreatMgr().GetThreatenedByMeList())
        {
            Unit* enemy = ref->GetOwner();
            if (!enemy || !enemy->IsAlive())
                continue;

            Unit* victim = enemy->GetVictim();
            Spell* spell = enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            enemies.push_back({enemy->GetName(), enemy->GetEntry(), enemy->GetHealthPct(), enemy->GetPositionX(),
                               enemy->GetPositionY(), enemy->GetPositionZ(), enemy->GetOrientation(),
                               player->GetDistance(enemy),
                               victim ? victim->GetName() : "", spell ? spell->m_spellInfo->Id : 0});
        }
        if (!enemies.empty() || player->IsInCombat())
            lastEnemies[player->GetGUID()] = std::move(enemies);
    }

    bool IsTracked(Unit* unit)
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        return player && player->IsInWorld() && player->GetMap() && player->GetMap()->IsDungeon();
    }

    // Player::EnvironmentalDamage deals its damage as SELF_DAMAGE with the player as attacker and no type left;
    // the kind is read back from where the player is. Drowning is max health / 5 + level each second under water
    // (Player::HandleDrowning), which is what the "?" melee deaths of Underbog / Blackfathom Deeps were.
    std::string EnvironmentSource(Unit const* victim)
    {
        LiquidData const& liquid = victim->GetLiquidData();
        if (liquid.Status != LIQUID_MAP_NO_WATER)
        {
            if (liquid.Flags & MAP_LIQUID_TYPE_MAGMA)
                return "environment: lava";
            if (liquid.Flags & MAP_LIQUID_TYPE_SLIME)
                return "environment: slime";
            if (liquid.Flags & MAP_LIQUID_TYPE_DARK_WATER)
                return "environment: fatigue";
            if (liquid.Status == LIQUID_MAP_UNDER_WATER)
                return "environment: drowning";
        }
        return "environment: fall";
    }

    std::mutex takenLock;
    std::unordered_map<ObjectGuid, uint64> takenTotals;  // DamageTaken: from creatures, by player
    std::unordered_map<ObjectGuid, uint64> takenSpell;   // [lead256] the part of it armor does not reduce
    std::unordered_map<ObjectGuid, uint64> healedDone;   // [lead256] HealingDone: effective heals, by healer
    // [lead1344] DamageTaken::From: a player's damage from each creature, this instance only (emptied when the
    // player's instance changes, so it holds one dungeon's creatures at most)
    struct TakenFrom
    {
        uint32 instanceId = 0;
        std::unordered_map<ObjectGuid, uint64> byAttacker;
    };
    std::unordered_map<ObjectGuid, TakenFrom> takenFrom;
    std::unordered_map<ObjectGuid, uint64> toolMana;  // [lead1400] ManaReturned::FromTools, by player
    std::unordered_map<ObjectGuid, uint64> upkeepMana;  // [lead1431] ManaSpent::Upkeep, by player
    // [lead1431] an absorb aura's amount at its last apply: (target, caster, spell) -> absorb amount
    std::map<std::tuple<ObjectGuid, ObjectGuid, uint32>, int32> shieldAmounts;

    // [lead1431] heals the spell data casts through a dummy effect (the cast picks its heal or damage spell), by first
    // rank: Holy Shock, Penance, Prayer of Mending
    constexpr uint32 SPELL_HOLY_SHOCK = 20473;
    constexpr uint32 SPELL_PENANCE = 47540;
    constexpr uint32 SPELL_PRAYER_OF_MENDING = 33076;

    // [lead1431] a cast that heals by itself: a heal, a heal over time, an absorb, or one of the dummy heals above
    bool IsHealCast(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_PCT ||
                effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH)
                return true;
            bool const aura = effect.Effect == SPELL_EFFECT_APPLY_AURA ||
                              effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_RAID ||
                              effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_PARTY;
            if (aura && (effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL ||
                         effect.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB))
                return true;
        }
        uint32 const first = info->GetFirstRankSpell()->Id;
        return first == SPELL_HOLY_SHOCK || first == SPELL_PENANCE || first == SPELL_PRAYER_OF_MENDING;
    }

    // [lead1431] what an aura can still absorb (its absorb effects' amounts; an unlimited one counts nothing)
    int32 AbsorbLeft(Aura const* aura)
    {
        int32 left = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* effect = aura->GetEffect(i);
                effect && effect->GetAuraType() == SPELL_AURA_SCHOOL_ABSORB && effect->GetAmount() > 0)
                left += effect->GetAmount();
        return left;
    }

    // [lead1400] the energize spells of the mana returns a boss gate's line credits on top of the measured use
    // (RestState::FightManaLine: Evocation, Divine Plea, Innervate, Shadowfiend - its Mana Leech hit -, a mana potion
    // and a mana gem): the tools by their first rank, a consumable by its item's use spell (item data, read once)
    constexpr uint32 SPELL_MANA_LEECH = 34650;  // the Shadowfiend's hit: mana to its priest (spell_pri_mana_leech)

    bool IsCreditedManaReturn(uint32 spellId)
    {
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info)
            return false;
        uint32 const first = info->GetFirstRankSpell()->Id;
        if (first == RestState::SPELL_EVOCATION || first == RestState::SPELL_DIVINE_PLEA ||
            first == RestState::SPELL_INNERVATE || spellId == SPELL_MANA_LEECH)
            return true;
        static std::unordered_set<uint32> const consumables = []
        {
            std::unordered_set<uint32> spells;
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
            {
                if (proto.Class != ITEM_CLASS_CONSUMABLE)
                    continue;
                for (auto const& use : proto.Spells)
                    if (use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(use.SpellId)))
                            for (SpellEffectInfo const& effect : spell->Effects)
                                if (effect.Effect == SPELL_EFFECT_ENERGIZE && effect.MiscValue == POWER_MANA)
                                    spells.insert(spell->Id);
            }
            return spells;
        }();
        return consumables.count(spellId) != 0;
    }

    // [lead256] a spell's damage armor does not reduce (any school but physical)
    bool IsMagicDamage(uint32 spellId)
    {
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        return info && !(info->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL);
    }

    void Record(Unit* victim, Unit* source, uint32 spellId, int32 amount, bool heal, bool environment = false)
    {
        if (amount <= 0 || !IsTracked(victim))
            return;

        if (!heal && source && source->IsCreature())
        {
            bool const magic = IsMagicDamage(spellId);
            std::lock_guard<std::mutex> lock(takenLock);
            takenTotals[victim->GetGUID()] += uint64(amount);
            if (magic)
                takenSpell[victim->GetGUID()] += uint64(amount);
            TakenFrom& from = takenFrom[victim->GetGUID()];
            if (from.instanceId != victim->GetInstanceId())
            {
                from.byAttacker.clear();
                from.instanceId = victim->GetInstanceId();
            }
            from.byAttacker[source->GetGUID()] += uint64(amount);
        }
        // [lead256] the heal hook runs before the heal lands: what it restores, overheal cut off
        if (heal && source && source->IsPlayer() && victim->GetHealth() < victim->GetMaxHealth())
        {
            uint64 const missing = uint64(victim->GetMaxHealth() - victim->GetHealth());
            std::lock_guard<std::mutex> lock(takenLock);
            healedDone[source->GetGUID()] += std::min<uint64>(uint64(amount), missing);
        }

        CombatEvent event{getMSTime(),
                          heal,
                          environment ? EnvironmentSource(victim) : source ? source->GetName() : "?",
                          source && source->IsCreature() ? source->GetEntry() : 0,
                          spellId,
                          amount,
                          victim->GetHealth(),
                          victim->GetMaxHealth(),
                          victim->IsInCombat(),
                          environment};

        std::lock_guard<std::mutex> lock(eventsLock);
        std::deque<CombatEvent>& events = recentEvents[victim->GetGUID()];
        events.push_back(std::move(event));
        uint32 const now = events.back().time;
        while (!events.empty() &&
               (getMSTimeDiff(events.front().time, now) > DEATH_LOG_WINDOW_MS || events.size() > DEATH_LOG_MAX_EVENTS))
            events.pop_front();
    }

    std::string Json(std::string const& text)
    {
        std::ostringstream out;
        out << '"';
        for (char c : text)
        {
            switch (c)
            {
                case '"':
                    out << "\\\"";
                    break;
                case '\\':
                    out << "\\\\";
                    break;
                case '\n':
                    out << "\\n";
                    break;
                default:
                    if (static_cast<unsigned char>(c) >= 0x20)
                        out << c;
                    break;
            }
        }
        out << '"';
        return out.str();
    }

    std::string SpellName(uint32 spellId)
    {
        if (!spellId)
            return "melee";
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        return info ? info->SpellName[LOCALE_enUS] : "?";
    }

    char const* RoleOf(Player* player)
    {
        if (PlayerbotAI::IsTank(player))
            return "tank";
        if (PlayerbotAI::IsHeal(player))
            return "healer";
        return "dps";
    }

    // The boss (or, failing that, the strongest enemy) the dead player was fighting.
    Unit* FindEncounterTarget(Player* player)
    {
        Unit* best = nullptr;
        for (auto const& [guid, ref] : player->GetThreatMgr().GetThreatenedByMeList())
        {
            Unit* enemy = ref->GetOwner();
            if (!enemy || !enemy->IsAlive())
                continue;

            Creature* creature = enemy->ToCreature();
            if (creature && (creature->IsDungeonBoss() || creature->isWorldBoss()))
                return enemy;

            if (!best || enemy->GetMaxHealth() > best->GetMaxHealth())
                best = enemy;
        }
        return best;
    }

    // [b30_reenter] `reentryKiller` / `reentryJson`: a re-entry of the instance (stranded with no path back, or a
    // route choice), user 2026-10-25: «перезаход это считается как смерть ... смерти мы все должны анализировать» -
    // written as a death with its killer and a "reentry" object (goal, walks, intent, watchdog) instead of the blow.
    void WriteDeath(Player* player, std::string const* reentryKiller = nullptr,
                    std::string const* reentryJson = nullptr)
    {
        std::deque<CombatEvent> events;
        std::deque<TrailPoint> trail;
        std::vector<EnemySnapshot> snapshot;
        {
            std::lock_guard<std::mutex> lock(eventsLock);
            auto itr = recentEvents.find(player->GetGUID());
            if (itr != recentEvents.end())
            {
                events = std::move(itr->second);
                recentEvents.erase(itr);
            }
            auto trailItr = recentTrail.find(player->GetGUID());
            if (trailItr != recentTrail.end())
            {
                trail = std::move(trailItr->second);
                recentTrail.erase(trailItr);
            }
            auto enemyItr = lastEnemies.find(player->GetGUID());
            if (enemyItr != lastEnemies.end())
            {
                snapshot = std::move(enemyItr->second);
                lastEnemies.erase(enemyItr);
            }
        }

        uint32 const now = getMSTime();
        std::time_t const wallClock = std::time(nullptr);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &wallClock);
#else
        localtime_r(&wallClock, &localTime);
#endif
        Map* map = player->GetMap();

        std::ostringstream out;
        out << "{\"time\":\"" << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << "\"";
        out << ",\"map\":" << map->GetId() << ",\"mapName\":" << Json(map->GetMapName())
            << ",\"difficulty\":" << uint32(map->GetDifficulty()) << ",\"instance\":" << map->GetInstanceId();
        out << ",\"pos\":[" << player->GetPositionX() << "," << player->GetPositionY() << "," << player->GetPositionZ()
            << "]";
        out << ",\"name\":" << Json(player->GetName()) << ",\"class\":" << uint32(player->getClass())
            << ",\"role\":\"" << RoleOf(player) << "\",\"level\":" << uint32(player->GetLevel())
            << ",\"bot\":" << (GET_PLAYERBOT_AI(player) ? "true" : "false");

        if (reentryKiller)
            out << ",\"killer\":" << Json(*reentryKiller) << ",\"killerEntry\":0,\"killingSpell\":0"
                << ",\"killingSpellName\":\"re-entry\",\"killingAmount\":0,\"reentry\":"
                << (reentryJson && !reentryJson->empty() ? *reentryJson : std::string("{}"));
        else if (!events.empty())
        {
            CombatEvent const* killingBlow = nullptr;
            for (auto itr = events.rbegin(); itr != events.rend() && !killingBlow; ++itr)
                if (!itr->heal)
                    killingBlow = &*itr;
            if (killingBlow)
                out << ",\"killer\":" << Json(killingBlow->source) << ",\"killerEntry\":" << killingBlow->sourceEntry
                    << ",\"killingSpell\":" << killingBlow->spellId
                    << ",\"killingSpellName\":"
                    << Json(killingBlow->environment ? "environment" : SpellName(killingBlow->spellId))
                    << ",\"killingAmount\":" << killingBlow->amount;
        }

        Unit* encounter = FindEncounterTarget(player);
        if (encounter)
            out << ",\"encounter\":{\"name\":" << Json(encounter->GetName())
                << ",\"entry\":" << encounter->GetEntry() << ",\"hpPct\":" << encounter->GetHealthPct() << "}";

        // From the InstanceScript, so it holds when the threat list is empty because units left combat.
        int32 const bossId = InstanceEncounter::GetInProgressBossId(map);
        out << ",\"encounterInProgress\":" << (bossId != InstanceEncounter::NO_ENCOUNTER ? "true" : "false");
        if (bossId != InstanceEncounter::NO_ENCOUNTER)
        {
            std::string name = InstanceEncounter::GetEncounterName(map, bossId);
            Creature* target = encounter ? encounter->ToCreature() : nullptr;
            if (name.empty() && target && (target->IsDungeonBoss() || target->isWorldBoss()))
                name = target->GetName();
            out << ",\"encounterBossId\":" << bossId << ",\"encounterName\":" << Json(name);
        }

        out << ",\"events\":[";
        bool first = true;
        for (CombatEvent const& event : events)
        {
            out << (first ? "" : ",") << "{\"t\":-" << getMSTimeDiff(event.time, now) << ",\"type\":\""
                << (event.heal ? "heal" : "dmg") << "\",\"src\":" << Json(event.source) << ",\"srcEntry\":"
                << event.sourceEntry << ",\"spell\":" << event.spellId << ",\"spellName\":"
                << Json(event.environment ? "environment" : SpellName(event.spellId)) << ",\"amount\":"
                << event.amount << ",\"hp\":" << event.health
                << ",\"maxHp\":" << event.maxHealth << ",\"combat\":" << (event.inCombat ? "true" : "false") << "}";
            first = false;
        }
        out << "]";

        out << ",\"trail\":[";
        first = true;
        for (TrailPoint const& point : trail)
        {
            out << (first ? "" : ",") << "{\"t\":-" << getMSTimeDiff(point.time, now) << ",\"x\":" << point.x
                << ",\"y\":" << point.y << ",\"z\":" << point.z << ",\"falling\":" << (point.falling ? "true" : "false")
                << "}";
            first = false;
        }
        out << "]";

        // Enemies as of the last snapshot before death (at most TRAIL_SAMPLE_MS old).
        out << ",\"enemies\":[";
        first = true;
        for (EnemySnapshot const& enemy : snapshot)
        {
            out << (first ? "" : ",") << "{\"name\":" << Json(enemy.name) << ",\"entry\":" << enemy.entry
                << ",\"hpPct\":" << enemy.hpPct << ",\"x\":" << enemy.x << ",\"y\":" << enemy.y
                << ",\"z\":" << enemy.z << ",\"o\":" << enemy.o << ",\"dist\":" << enemy.dist
                << ",\"target\":" << Json(enemy.target)
                << ",\"casting\":" << enemy.casting << "}";
            first = false;
        }
        out << "]";

        out << ",\"auras\":[";
        first = true;
        for (auto const& [spellId, aurApp] : player->GetAppliedAuras())
        {
            Aura const* aura = aurApp->GetBase();
            Unit* caster = aura->GetCaster();
            out << (first ? "" : ",") << "{\"id\":" << spellId << ",\"name\":" << Json(SpellName(spellId))
                << ",\"stacks\":" << uint32(aura->GetStackAmount()) << ",\"remainingMs\":" << aura->GetDuration()
                << ",\"positive\":" << (aurApp->IsPositive() ? "true" : "false")
                << ",\"caster\":" << Json(caster ? caster->GetName() : "?") << "}";
            first = false;
        }
        out << "]";

        out << ",\"group\":[";
        first = true;
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player || member->GetMap() != map)
                    continue;

                out << (first ? "" : ",") << "{\"name\":" << Json(member->GetName()) << ",\"role\":\""
                    << RoleOf(member) << "\",\"alive\":" << (member->IsAlive() ? "true" : "false")
                    << ",\"hpPct\":" << member->GetHealthPct() << ",\"manaPct\":"
                    << (member->GetMaxPower(POWER_MANA) ? member->GetPowerPct(POWER_MANA) : 0.0f)
                    << ",\"dist\":" << player->GetDistance(member) << ",\"x\":" << member->GetPositionX()
                    << ",\"y\":" << member->GetPositionY() << ",\"z\":" << member->GetPositionZ() << "}";
                first = false;
            }
        }
        out << "]}";

        std::lock_guard<std::mutex> lock(fileLock);
        std::ofstream file(sLog->GetLogsDir() + "deaths.jsonl", std::ios::app);
        if (file)
            file << out.str() << '\n';
    }
}

namespace DeathRecorder
{
    void WriteReentry(Player* player, std::string const& killer, std::string const& context)
    {
        if (IsTracked(player))
            WriteDeath(player, &killer, &context);
    }
}

namespace DamageTaken
{
    uint64 Total(ObjectGuid player)
    {
        std::lock_guard<std::mutex> lock(takenLock);
        auto const found = takenTotals.find(player);
        return found != takenTotals.end() ? found->second : 0;
    }

    uint64 Spell(ObjectGuid player)
    {
        std::lock_guard<std::mutex> lock(takenLock);
        auto const found = takenSpell.find(player);
        return found != takenSpell.end() ? found->second : 0;
    }

    uint64 From(ObjectGuid player, ObjectGuid attacker)
    {
        std::lock_guard<std::mutex> lock(takenLock);
        auto const found = takenFrom.find(player);
        if (found == takenFrom.end())
            return 0;
        auto const hit = found->second.byAttacker.find(attacker);
        return hit != found->second.byAttacker.end() ? hit->second : 0;
    }
}

namespace ManaReturned
{
    uint64 FromTools(ObjectGuid player)
    {
        std::lock_guard<std::mutex> lock(takenLock);
        auto const found = toolMana.find(player);
        return found != toolMana.end() ? found->second : 0;
    }
}

namespace ManaSpent
{
    uint64 Upkeep(ObjectGuid player)
    {
        std::lock_guard<std::mutex> lock(takenLock);
        auto const found = upkeepMana.find(player);
        return found != upkeepMana.end() ? found->second : 0;
    }
}

namespace HealingDone
{
    uint64 Effective(ObjectGuid healer)
    {
        std::lock_guard<std::mutex> lock(takenLock);
        auto const found = healedDone.find(healer);
        return found != healedDone.end() ? found->second : 0;
    }
}

class PlayerbotsDeathRecorderUnitScript : public UnitScript
{
public:
    PlayerbotsDeathRecorderUnitScript()
        : UnitScript("PlayerbotsDeathRecorderUnitScript", true,
                     {UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
                      UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_ON_POWER_GAIN,
                      UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE})
    {
    }

    // [lead1431] an absorb on a tracked player: its amount now (a refresh starts it again: what the refreshed shield
    // took before is not counted, the safe side of the healer's mana per health)
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!aura || !aura->GetSpellInfo()->HasAura(SPELL_AURA_SCHOOL_ABSORB) || !aura->GetCasterGUID().IsPlayer() ||
            !IsTracked(unit))
            return;
        int32 const left = AbsorbLeft(aura);
        std::lock_guard<std::mutex> lock(takenLock);
        if (shieldAmounts.size() > 4096)  // a bound on shields whose removal was never seen
            shieldAmounts.clear();
        shieldAmounts[{unit->GetGUID(), aura->GetCasterGUID(), aura->GetId()}] = left;
    }

    // [lead1431] the shield's end: what it took since its apply is its caster's effective healing (HealingDone)
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        Aura const* aura = aurApp ? aurApp->GetBase() : nullptr;
        if (!unit || !aura || !aura->GetSpellInfo()->HasAura(SPELL_AURA_SCHOOL_ABSORB) ||
            !aura->GetCasterGUID().IsPlayer())
            return;
        int32 const left = AbsorbLeft(aura);
        std::lock_guard<std::mutex> lock(takenLock);
        auto const found = shieldAmounts.find({unit->GetGUID(), aura->GetCasterGUID(), aura->GetId()});
        if (found == shieldAmounts.end())
            return;
        if (found->second > left)
            healedDone[aura->GetCasterGUID()] += uint64(found->second - left);
        shieldAmounts.erase(found);
    }

    // [lead1400] the mana a credited return gave (what the bar took): ManaReturned::FromTools
    void OnPowerGain(Unit* unit, uint8 power, int32 gain, uint8 source, uint32 spellId) override
    {
        if (gain <= 0 || power != POWER_MANA || source != POWER_GAIN_SOURCE_SPELL || !IsTracked(unit) ||
            !IsCreditedManaReturn(spellId))
            return;
        std::lock_guard<std::mutex> lock(takenLock);
        toolMana[unit->GetGUID()] += uint64(gain);
    }

    // Amounts come from DealDamage, which sees the damage after armor, resistances, absorbs and avoidance. The
    // Modify* hooks run earlier (melee even before miss/dodge/parry) and only note which spell is about to land.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* /*attacker*/, uint32& /*damage*/,
                                       SpellInfo const* spellInfo) override
    {
        // The core also calls this for periodic heals, right before ModifyHealReceived with the right order.
        if (spellInfo &&
            (spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL) || spellInfo->HasAura(SPELL_AURA_OBS_MOD_HEALTH)))
        {
            tl_periodicHeal = true;
            return;
        }
        NotePendingSpell(target, spellInfo);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* /*attacker*/, int32& /*damage*/,
                                SpellInfo const* spellInfo) override
    {
        NotePendingSpell(target, spellInfo);
    }

    void ModifyHealReceived(Unit* target, Unit* healer, uint32& heal, SpellInfo const* spellInfo) override
    {
        // Direct heals reach this hook with healer and target swapped (Unit::HealBySpell passes the caster first);
        // periodic heals come in the declared order, flagged by the periodic hook just before.
        bool const periodic = tl_periodicHeal;
        tl_periodicHeal = false;
        Unit* realTarget = periodic ? target : healer;
        Unit* realHealer = periodic ? healer : target;
        Record(realTarget, realHealer, spellInfo ? spellInfo->Id : 0, heal, true);
    }

    uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType damageType) override
    {
        uint32 spellId = 0;
        if (damageType == SPELL_DIRECT_DAMAGE || damageType == DOT)
            spellId = TakePendingSpell(victim);
        else
            TakePendingSpell(victim);

        // Falling, lava and drowning reach the player as self damage with no attacker of its own.
        bool const environment = damageType == SELF_DAMAGE && attacker == victim;
        Record(victim, environment ? nullptr : attacker, spellId, damage, false, environment);

        // [lifetime-calibrated] A group member's (or its pet's / totem's) damage on a dungeon creature, overkill cut
        // off: the group's measured dps (GroupDamage, EstimatedLifetimeValue.cpp).
        if (damage && attacker && victim && victim->IsCreature() && victim->IsAlive() && victim->GetMap() &&
            victim->GetMap()->IsDungeon() && !victim->GetCharmerOrOwnerPlayerOrPlayerItself())
        {
            Player* owner = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (owner && owner->GetGroup())
                GroupDamage::NoteDealt(owner, std::min(damage, victim->GetHealth()));
        }

        return damage;
    }

private:
    static inline thread_local bool tl_periodicHeal = false;
    static inline thread_local Unit* tl_pendingVictim = nullptr;
    static inline thread_local uint32 tl_pendingSpell = 0;

    static void NotePendingSpell(Unit* victim, SpellInfo const* spellInfo)
    {
        tl_pendingVictim = victim;
        tl_pendingSpell = spellInfo ? spellInfo->Id : 0;
    }

    static uint32 TakePendingSpell(Unit* victim)
    {
        uint32 const spellId = tl_pendingVictim == victim ? tl_pendingSpell : 0;
        tl_pendingVictim = nullptr;
        tl_pendingSpell = 0;
        return spellId;
    }
};

class PlayerbotsDeathRecorderPlayerScript : public PlayerScript
{
public:
    PlayerbotsDeathRecorderPlayerScript()
        : PlayerScript("PlayerbotsDeathRecorderPlayerScript",
                       {PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_SPELL_CAST})
    {
    }

    // [lead1431] the mana a cast that heals nothing itself costs (ManaSpent::Upkeep): the cost the game takes
    // (Spell::TakePower: none for an item's cast, a cast triggered by an aura, or one that ignores its cost)
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
        if (!info || info->PowerType != POWER_MANA || spell->GetPowerCost() <= 0 || spell->m_CastItem ||
            spell->GetTriggeredByAuraSpellInfo() ||
            spell->HasTriggeredCastFlag(TRIGGERED_IGNORE_POWER_AND_REAGENT_COST) || !IsTracked(player) ||
            IsHealCast(info))
            return;
        std::lock_guard<std::mutex> lock(takenLock);
        upkeepMana[player->GetGUID()] += uint64(spell->GetPowerCost());
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (IsTracked(player) && player->IsAlive())
            SampleTrail(player, diff);
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (IsTracked(player))
            WriteDeath(player);
    }
};

void AddPlayerbotsDeathRecorder()
{
    new PlayerbotsDeathRecorderUnitScript();
    new PlayerbotsDeathRecorderPlayerScript();
}
