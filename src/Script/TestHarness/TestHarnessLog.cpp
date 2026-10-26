/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestHarnessLog.h"

#include "Creature.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace TestHarness
{
    namespace
    {
        std::mutex fileLock;
        bool dirReady = false;

        Player* OwnerPlayer(Unit* unit)
        {
            if (!unit)
                return nullptr;
            return unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        }
    }

    std::string JsonEscape(std::string const& text)
    {
        std::string out;
        out.reserve(text.size() + 2);
        out += '"';
        for (char c : text)
        {
            switch (c)
            {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    break;
                case '\t':
                    out += ' ';
                    break;
                default:
                    if (static_cast<unsigned char>(c) >= 0x20)
                        out += c;
                    break;
            }
        }
        out += '"';
        return out;
    }

    std::string WallClock()
    {
        std::time_t const now = std::time(nullptr);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &now);
#else
        localtime_r(&now, &localTime);
#endif
        std::ostringstream out;
        out << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }

    void JsonObject::Key(char const* key)
    {
        if (!_body.empty())
            _body += ',';
        _body += JsonEscape(key);
        _body += ':';
    }

    JsonObject& JsonObject::Str(char const* key, std::string const& value)
    {
        Key(key);
        _body += JsonEscape(value);
        return *this;
    }

    JsonObject& JsonObject::Num(char const* key, int64 value)
    {
        Key(key);
        _body += std::to_string(value);
        return *this;
    }

    JsonObject& JsonObject::Real(char const* key, double value)
    {
        Key(key);
        if (!std::isfinite(value))
        {
            _body += "0";
            return *this;
        }
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << value;
        _body += out.str();
        return *this;
    }

    JsonObject& JsonObject::Bool(char const* key, bool value)
    {
        Key(key);
        _body += value ? "true" : "false";
        return *this;
    }

    JsonObject& JsonObject::Raw(char const* key, std::string const& json)
    {
        Key(key);
        _body += json.empty() ? "null" : json;
        return *this;
    }

    JsonObject& JsonObject::Merge(JsonObject const& other)
    {
        if (other._body.empty())
            return *this;
        if (!_body.empty())
            _body += ',';
        _body += other._body;
        return *this;
    }

    JsonArray& JsonArray::Raw(std::string const& json)
    {
        if (!_body.empty())
            _body += ',';
        _body += json;
        return *this;
    }

    JsonArray& JsonArray::Str(std::string const& value) { return Raw(JsonEscape(value)); }

    JsonArray& JsonArray::Num(int64 value) { return Raw(std::to_string(value)); }

    std::string TestsDir() { return sLog->GetLogsDir() + "tests/"; }

    void AppendLine(std::string const& fileName, std::string const& line)
    {
        std::lock_guard<std::mutex> lock(fileLock);
        if (!dirReady)
        {
            std::error_code error;
            std::filesystem::create_directories(TestsDir(), error);
            if (error)
                LOG_ERROR("playerbots", "Test harness: cannot create {}: {}", TestsDir(), error.message());
            dirReady = true;
        }

        std::ofstream file(TestsDir() + fileName, std::ios::app);
        if (file)
            file << line << '\n';
    }

    StatsRegistry& StatsRegistry::Instance()
    {
        static StatsRegistry instance;
        return instance;
    }

    void StatsRegistry::TrackMember(ObjectGuid guid, uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _memberRun[guid] = runSeq;
        _counters[guid] = MemberCounters();
        _active.store(true, std::memory_order_relaxed);
    }

    void StatsRegistry::TrackInstance(uint32 instanceId, uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _instanceRun[instanceId] = runSeq;
    }

    void StatsRegistry::UntrackRun(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        for (auto itr = _memberRun.begin(); itr != _memberRun.end();)
        {
            if (itr->second == runSeq)
            {
                _counters.erase(itr->first);
                itr = _memberRun.erase(itr);
            }
            else
                ++itr;
        }

        for (auto itr = _instanceRun.begin(); itr != _instanceRun.end();)
        {
            if (itr->second == runSeq)
                itr = _instanceRun.erase(itr);
            else
                ++itr;
        }

        _kills.erase(runSeq);
        _deaths.erase(runSeq);
        _killedEntries.erase(runSeq);
        _openers.erase(runSeq);
        _active.store(!_memberRun.empty(), std::memory_order_relaxed);
    }

    uint32 StatsRegistry::RunOfMember(ObjectGuid guid) const
    {
        auto itr = _memberRun.find(guid);
        return itr == _memberRun.end() ? 0 : itr->second;
    }

    MemberCounters StatsRegistry::Snapshot(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto itr = _counters.find(guid);
        return itr == _counters.end() ? MemberCounters() : itr->second;
    }

    uint32 StatsRegistry::TakeKills(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto itr = _kills.find(runSeq);
        if (itr == _kills.end())
            return 0;

        uint32 const kills = itr->second;
        itr->second = 0;
        return kills;
    }

    std::vector<uint32> StatsRegistry::TakeKilledEntries(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        std::vector<uint32> result;
        auto itr = _killedEntries.find(runSeq);
        if (itr != _killedEntries.end())
            result.swap(itr->second);
        return result;
    }

    std::vector<DeathNote> StatsRegistry::TakeDeaths(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        std::vector<DeathNote> result;
        auto itr = _deaths.find(runSeq);
        if (itr != _deaths.end())
            result.swap(itr->second);
        return result;
    }

    void StatsRegistry::OnDamage(Unit* attacker, Unit* victim, uint32 damage)
    {
        if (!Active() || !victim)
            return;

        // [opener_zero_hit] a member's hit on a mob that dealt nothing (evading, absorbed): noted, nothing counted
        if (!damage)
        {
            Player* source = victim->IsCreature() ? OwnerPlayer(attacker) : nullptr;
            if (!source)
                return;

            std::lock_guard<std::mutex> lock(_lock);
            uint32 const runSeq = RunOfMember(source->GetGUID());
            if (!runSeq)
                return;

            constexpr size_t MAX_OPENER_ZERO_HITS = 40;
            OpenerNote& note = _openers[runSeq];
            if (note.memberFirstZeroHits.size() < MAX_OPENER_ZERO_HITS &&
                std::none_of(note.memberFirstZeroHits.begin(), note.memberFirstZeroHits.end(),
                             [source](auto const& hit) { return hit.first == source->GetGUID(); }))
                note.memberFirstZeroHits.emplace_back(source->GetGUID(), getMSTime());
            return;
        }

        Player* source = OwnerPlayer(attacker);
        Player* target = victim->ToPlayer();
        if (!source && !target)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        if (source && RunOfMember(source->GetGUID()) && !victim->IsPlayer())
            _counters[source->GetGUID()].damageDone += damage;

        if (target && RunOfMember(target->GetGUID()))
            _counters[target->GetGUID()].damageTaken += damage;

        // The first hit between a member and a mob since the last fight ("who aggroed first").
        uint32 const sourceRun = source && victim->IsCreature() ? RunOfMember(source->GetGUID()) : 0;
        uint32 const targetRun = target && attacker && attacker->IsCreature() ? RunOfMember(target->GetGUID()) : 0;
        if (uint32 const runSeq = sourceRun ? sourceRun : targetRun)
        {
            OpenerNote& note = _openers[runSeq];
            if (!note.hitMs)
            {
                note.hitMs = getMSTime();
                note.hitByMember = sourceRun != 0;
                note.hitBy = sourceRun ? source->GetName() : attacker->GetName();
                note.hitByGuid = sourceRun ? source->GetGUID() : attacker->GetGUID();
                note.hitVictim = victim->GetName();
            }

            constexpr size_t MAX_OPENER_HITS = 40;
            if (sourceRun && note.memberFirstHits.size() < MAX_OPENER_HITS &&
                std::none_of(note.memberFirstHits.begin(), note.memberFirstHits.end(),
                             [source](auto const& hit) { return hit.first == source->GetGUID(); }))
                note.memberFirstHits.emplace_back(source->GetGUID(), getMSTime());
        }
    }

    void StatsRegistry::OnPlayerEnterCombat(Player* player, Unit* enemy)
    {
        if (!Active() || !player)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        uint32 const runSeq = RunOfMember(player->GetGUID());
        if (!runSeq)
            return;

        OpenerNote& note = _openers[runSeq];
        constexpr size_t MAX_OPENER_MEMBERS = 40;
        if (note.members.size() >= MAX_OPENER_MEMBERS)
            return;

        for (OpenerMember const& entry : note.members)
            if (entry.guid == player->GetGUID())
                return;

        OpenerMember entry;
        entry.guid = player->GetGUID();
        entry.name = player->GetName();
        entry.enemy = enemy ? enemy->GetName() : "";
        entry.enemyEntry = enemy && enemy->IsCreature() ? enemy->GetEntry() : 0;
        entry.ms = getMSTime();
        note.members.push_back(entry);
    }

    void StatsRegistry::OnCreatureEngage(Unit* creature, Unit* victim)
    {
        if (!Active() || !creature || !victim || !creature->IsCreature())
            return;

        Player* owner = OwnerPlayer(victim);
        if (!owner)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        uint32 const runSeq = RunOfMember(owner->GetGUID());
        if (!runSeq)
            return;

        OpenerNote& note = _openers[runSeq];
        if (note.mobMs)
            return;

        note.mobMs = getMSTime();
        note.mob = creature->GetName();
        note.mobEntry = creature->GetEntry();
        note.mobSpawnId = creature->ToCreature()->GetSpawnId();
        note.mobVictim = victim->GetName();
        note.mobVictimGuid = owner->GetGUID();
    }

    OpenerNote StatsRegistry::Opener(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto itr = _openers.find(runSeq);
        return itr == _openers.end() ? OpenerNote() : itr->second;
    }

    void StatsRegistry::ResetOpener(uint32 runSeq)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _openers.erase(runSeq);
    }

    void StatsRegistry::OnHeal(Unit* healer, Unit* /*target*/, uint32 gain)
    {
        if (!gain || !Active())
            return;

        Player* source = OwnerPlayer(healer);
        if (!source)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        if (RunOfMember(source->GetGUID()))
            _counters[source->GetGUID()].healingDone += gain;
    }

    void StatsRegistry::OnSpellCast(Player* player, Spell* spell, uint32 castItemEntry)
    {
        if (!Active() || !player || !spell || !spell->m_spellInfo)
            return;

        // [b31_harness] The cast mix counted at PLAYERHOOK_ON_SPELL_CAST, which runs at the START of Spell::_cast,
        // before the final CheckCast(false) and the target selection: a cast bar ending in a failure (target gone,
        // out of LOS, immune) and a next-swing spell refused at the swing (Heroic Strike / Cleave / Rune Strike
        // without rage / runic power) were counted as casts (Prot Warrior pulls at 70-80 casts/min, 20 HS+Cleave in a
        // 36 s pull). Now the caller is the end of _cast: the spell went off (SPELL_GO). Only the member's own casts:
        // not a triggered spell (procs, channel / periodic ticks, auto-repeat shots), not an item, not a cast whose
        // original caster is someone else.
        if (spell->GetCaster() != player || (spell->GetOriginalCaster() && spell->GetOriginalCaster() != player))
            return;

        SpellInfo const* info = spell->m_spellInfo;
        bool const triggered = spell->IsTriggered();
        bool const interrupt = info->HasEffect(SPELL_EFFECT_INTERRUPT_CAST);
        bool const dispel = info->HasEffect(SPELL_EFFECT_DISPEL);
        // Rotation review: what the member casts by its own choice in a fight (procs and triggered casts excluded).
        bool const rotation = player->IsInCombat() && !triggered && !castItemEntry;
        if (!interrupt && !dispel && !castItemEntry && !rotation)
            return;

        uint64 busy = 0;
        if (rotation)
        {
            // GCD: StartRecoveryTime (1.5 s, 1 s for energy users), hasted for spells, never below 1 s.
            float gcd = float(info->StartRecoveryTime);
            if (gcd > 0.0f && info->DmgClass != SPELL_DAMAGE_CLASS_MELEE && info->DmgClass != SPELL_DAMAGE_CLASS_RANGED)
                gcd = std::max(1000.0f, gcd * player->GetFloatValue(UNIT_MOD_CAST_SPEED));
            float const cast = float(std::max(spell->GetCastTime(), 0));
            float const channel = info->IsChanneled() ? float(std::max(info->GetDuration(), 0)) *
                                                            player->GetFloatValue(UNIT_MOD_CAST_SPEED)
                                                      : 0.0f;
            busy = uint64(std::max({gcd, cast, channel}));
        }

        uint32 const itemEntry = castItemEntry;
        std::lock_guard<std::mutex> lock(_lock);
        if (!RunOfMember(player->GetGUID()))
            return;

        MemberCounters& counters = _counters[player->GetGUID()];
        if (interrupt)
            ++counters.interrupts;
        if (dispel)
            ++counters.dispels;
        if (itemEntry)
            ++counters.itemUses[itemEntry];
        if (rotation)
        {
            ++counters.spells[info->Id].casts;
            counters.busyMs += busy;

            // [b31_harness] Long cooldowns (>= 60 s): a second counted cast well inside the cooldown is either a
            // real reset (Cold Snap, Preparation, Readiness, a harness re-prepare) or a counting error. The audits
            // saw Arcane Power 2.6/min, Presence of Mind 2.3/min, Metamorphosis 1.1/min; this line names the case.
            uint32 const recovery = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
            if (recovery >= 60 * IN_MILLISECONDS)
            {
                static std::map<std::pair<ObjectGuid, uint32>, uint32> lastLongCooldownCast;  // under _lock
                uint32 const now = getMSTime();
                auto [itr, fresh] = lastLongCooldownCast.try_emplace({player->GetGUID(), info->Id}, now);
                if (!fresh)
                {
                    uint32 const since = getMSTimeDiff(itr->second, now);
                    if (since < recovery * 9 / 10)
                        LOG_INFO("playerbots",
                                 "[b31_harness] cast inside cooldown: {} {} ({}) again after {:.1f} s, cooldown "
                                 "{:.0f} s",
                                 player->GetName(), info->SpellName[LOCALE_enUS] ? info->SpellName[LOCALE_enUS] : "",
                                 info->Id, since / 1000.0f, recovery / 1000.0f);
                    itr->second = now;
                }
            }
        }
    }

    void StatsRegistry::OnSpellAmount(Unit* source, Unit* other, SpellInfo const* spell, uint32 amount, bool heal)
    {
        if (!amount || !spell || !Active())
            return;

        Player* first = OwnerPlayer(source);
        Player* second = OwnerPlayer(other);
        std::lock_guard<std::mutex> lock(_lock);
        // The heal hooks pass (target, healer) from aura ticks but (healer, target) from HealBySpell: credit the side
        // that cast the spell.
        Player* caster = nullptr;
        for (Player* candidate : {first, second})
        {
            if (caster || !candidate || !RunOfMember(candidate->GetGUID()))
                continue;
            auto counters = _counters.find(candidate->GetGUID());
            if (counters != _counters.end() && counters->second.spells.count(spell->Id))
                caster = candidate;
        }
        if (!caster && !heal && first && RunOfMember(first->GetGUID()))
            caster = first;
        if (!caster)
            return;

        SpellCounters& counters = _counters[caster->GetGUID()].spells[spell->Id];
        if (heal)
            counters.healing += amount;
        else
            counters.damage += amount;
    }

    // [b30_topped_up] user rule 2026-10-25: «если нет пути обратно, то можно перезайти в данж. Но это будет
    // считаться как смерть» - [b30_reenter] every re-entry (user 2026-10-25: «перезаход это считается как смерть»)
    // is one more death in the member's counters (run_end deaths, the clean-pass
    // verdict, the streaks) and a death note (the "death" event, killer "stranded_reenter: ...").
    void StatsRegistry::RecordReentry(Player* player, std::string const& killer)
    {
        if (!Active() || !player)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        uint32 const runSeq = RunOfMember(player->GetGUID());
        if (!runSeq)
            return;

        ++_counters[player->GetGUID()].deaths;
        DeathNote note;
        note.guid = player->GetGUID();
        note.name = player->GetName();
        note.killer = killer;
        note.ms = getMSTime();
        _deaths[runSeq].push_back(note);
    }

    void StatsRegistry::OnUnitDeath(Unit* victim, Unit* killer)
    {
        if (!Active() || !victim || !victim->GetMap())
            return;

        uint32 const instanceId = victim->GetMap()->GetInstanceId();
        std::lock_guard<std::mutex> lock(_lock);
        if (Player* player = victim->ToPlayer())
        {
            uint32 const runSeq = RunOfMember(player->GetGUID());
            if (!runSeq)
                return;

            ++_counters[player->GetGUID()].deaths;
            DeathNote note;
            note.guid = player->GetGUID();
            note.name = player->GetName();
            note.killer = killer ? killer->GetName() : "";
            // Drowning, falling, lava: Player::EnvironmentalDamage kills with the player as its own killer.
            if (killer == victim)
            {
                LiquidData const& liquid = victim->GetLiquidData();
                if (liquid.Status == LIQUID_MAP_UNDER_WATER && !(liquid.Flags & MAP_LIQUID_TYPE_MAGMA))
                    note.killer = "environment: drowning";
                else if (liquid.Status != LIQUID_MAP_NO_WATER && (liquid.Flags & MAP_LIQUID_TYPE_MAGMA))
                    note.killer = "environment: lava";
                else
                    note.killer = "environment";
            }
            note.killerEntry = killer && killer->IsCreature() ? killer->GetEntry() : 0;
            note.ms = getMSTime();
            _deaths[runSeq].push_back(note);
            return;
        }

        if (!instanceId || !victim->IsCreature())
            return;

        auto itr = _instanceRun.find(instanceId);
        if (itr != _instanceRun.end())
        {
            ++_kills[itr->second];
            std::vector<uint32>& entries = _killedEntries[itr->second];
            if (entries.size() < 256)  // drained every harness tick; the cap only guards a stopped run
                entries.push_back(victim->GetEntry());
        }
    }
}
