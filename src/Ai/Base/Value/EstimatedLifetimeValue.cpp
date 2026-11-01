/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EstimatedLifetimeValue.h"
#include "AiFactory.h"
#include "Group.h"
#include "Map.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "StringFormat.h"  // [b35_mvaway]
#include "Timer.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    constexpr uint32 GROUP_MODEL_REFRESH_MS = 20 * IN_MILLISECONDS;  // the gear-table model is rebuilt this often
}

float EstimatedLifetimeValue::Calculate()
{
    Unit* target = AI_VALUE(Unit*, qualifier);
    if (!target || !target->IsAlive())
    {
        return 0.0f;
    }
    float dps = AI_VALUE(float, "estimated group dps");
    bool aoePenalty = AI_VALUE(uint8, "attacker count") >= 3;
    if (aoePenalty)
        dps *= 0.75;
    float res = target->GetHealth() / dps;
    // bot->Say(target->GetName() + " lifetime: " + std::to_string(res), LANG_UNIVERSAL);
    return res;
}

float EstimatedGroupDpsValue::Calculate()
{
    uint32 const now = getMSTime();
    if (!_modelMs || getMSTimeDiff(_modelMs, now) >= GROUP_MODEL_REFRESH_MS)
    {
        _modelDps = ModelDps();
        _modelMs = now ? now : 1;
    }

    // [lifetime-calibrated] The group's measured damage once a fight of it is known: the model put a level 45-50
    // group at ~500 dps where it did 848 (Sunken Temple 2026-10-25), at 269 against 550-1100 (Zul'Farrak).
    float const calibrated = GroupDamage::CalibratedDps(bot);
    return calibrated > 0.0f ? calibrated : _modelDps;
}

float EstimatedGroupDpsValue::ModelDps()
{
    float totalDps = 0;

    std::vector<Player*> groupPlayer = {bot};
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (member == bot)  // calculated
                continue;

            // Ignore real players and SelfBots as they may not help with damage.
            if (!GET_PLAYERBOT_AI(member) || IsSelfBot(member))
                continue;

            if (!member || !member->IsInWorld() || !member->IsAlive())
                continue;

            if (member->GetMapId() != bot->GetMapId())
                continue;

            if (member->GetExactDist(bot) > sPlayerbotAIConfig.SightDistance)
                continue;

            groupPlayer.push_back(member);
        }
    }
    for (Player* player : groupPlayer)
    {
        float roleMultiplier;
        if (PlayerbotAI::IsTank(player))
            roleMultiplier = 0.3f;
        else if (PlayerbotAI::IsHeal(player))
            roleMultiplier = 0.1f;
        else
            roleMultiplier = 1.0f;
        float basicDps = GetBasicDps(player->GetLevel());
        float basicGs = GetBasicGs(player->GetLevel());
        uint32 mixedGearScore = PlayerbotAI::GetMixedGearScore(player, true, false, 12);
        float gs_modifier = (float)mixedGearScore / basicGs;
        // bonus for wotlk epic gear
        if (mixedGearScore >= 300)
        {
            gs_modifier *= 1 + (mixedGearScore - 300) * 0.01;
        }
        if (gs_modifier < 0.75)
            gs_modifier = 0.75;
        if (gs_modifier > 4)
            gs_modifier = 4;
        totalDps += basicDps * roleMultiplier * gs_modifier;
    }
    // Group buff bonus
    if (groupPlayer.size() >= 25)
        totalDps *= 1.2;
    else if (groupPlayer.size() >= 10)
        totalDps *= 1.1;
    else if (groupPlayer.size() >= 5)
        totalDps *= 1.05;
    return totalDps;
}

float EstimatedGroupDpsValue::GetBasicDps(uint32 level)
{
    float basic_dps;

    if (level <= 15)
    {
        basic_dps = 5 + level * 1;
    }
    else if (level <= 25)
    {
        basic_dps = 20 + (level - 15) * 2;
    }
    else if (level <= 45)
    {
        basic_dps = 40 + (level - 25) * 3;
    }
    else if (level <= 55)
    {
        basic_dps = 100 + (level - 45) * 20;
    }
    else if (level <= 60)
    {
        basic_dps = 300 + (level - 55) * 50;
    }
    else if (level <= 70)
    {
        basic_dps = 550 + (level - 60) * 65;
    }
    else
    {
        basic_dps = 1200 + (level - 70) * 200;
    }
    return basic_dps;
}

float EstimatedGroupDpsValue::GetBasicGs(uint32 level)
{
    float basic_gs;

    if (level <= 8)
    {
        basic_gs = PlayerbotFactory::CalcMixedGearScore(level + 5, ITEM_QUALITY_NORMAL);
    }
    else if (level <= 15)
    {
        basic_gs = PlayerbotFactory::CalcMixedGearScore(level + 5, ITEM_QUALITY_UNCOMMON);
    }
    else if (level <= 60)
    {
        basic_gs = PlayerbotFactory::CalcMixedGearScore(level + 5, ITEM_QUALITY_RARE);
    }
    else if (level <= 70)
    {
        basic_gs = PlayerbotFactory::CalcMixedGearScore(85 + (level - 60) * 3, ITEM_QUALITY_RARE);
    }
    else
    {
        basic_gs = PlayerbotFactory::CalcMixedGearScore(155 + (level - 70) * 4, ITEM_QUALITY_RARE);
    }
    return basic_gs;
}

namespace
{
    constexpr uint32 LIFE_SAMPLE_MS = 500;        // a health sample is kept this often per unit
    constexpr uint32 LIFE_MIN_WINDOW_MS = 700;    // a rate over less than this is noise
    constexpr uint32 LIFE_MAX_WINDOW_MS = 4000;   // and over more than this is history
    constexpr uint32 LIFE_FORGET_MS = 10000;      // a unit not looked at for this long is forgotten
    constexpr size_t LIFE_MAX_TRACKED = 512;      // the table is swept of forgotten units past this size
    constexpr size_t LIFE_SAMPLES = 8;

    struct HealthTrack
    {
        std::array<uint32, LIFE_SAMPLES> ms{};
        std::array<uint32, LIFE_SAMPLES> health{};
        size_t count = 0;
        uint32 lastSeenMs = 0;
    };
}

float ObservedLifetime(Unit* target)
{
    float const unknown = std::numeric_limits<float>::max();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return unknown;

    // Map threads tick their units; each keeps its own table.
    thread_local std::unordered_map<ObjectGuid, HealthTrack> tracks;
    uint32 const now = getMSTime();
    if (tracks.size() > LIFE_MAX_TRACKED)
    {
        for (auto it = tracks.begin(); it != tracks.end();)
        {
            if (getMSTimeDiff(it->second.lastSeenMs, now) > LIFE_FORGET_MS)
                it = tracks.erase(it);
            else
                ++it;
        }
    }

    HealthTrack& track = tracks[target->GetGUID()];
    uint32 const health = target->GetHealth();
    // Healed, reset or evaded since: start over.
    if (track.count && (health > track.health[track.count - 1] ||
                        getMSTimeDiff(track.lastSeenMs, now) > LIFE_FORGET_MS))
        track.count = 0;

    track.lastSeenMs = now;
    if (!track.count || getMSTimeDiff(track.ms[track.count - 1], now) >= LIFE_SAMPLE_MS)
    {
        if (track.count == LIFE_SAMPLES)
        {
            for (size_t i = 1; i < LIFE_SAMPLES; ++i)
            {
                track.ms[i - 1] = track.ms[i];
                track.health[i - 1] = track.health[i];
            }
            --track.count;
        }

        track.ms[track.count] = now;
        track.health[track.count] = health;
        ++track.count;
    }

    // The oldest sample inside the window gives the rate.
    for (size_t i = 0; i < track.count; ++i)
    {
        uint32 const age = getMSTimeDiff(track.ms[i], now);
        if (age > LIFE_MAX_WINDOW_MS)
            continue;
        if (age < LIFE_MIN_WINDOW_MS)
            break;

        if (track.health[i] <= health)
            return unknown;

        float const perSecond = static_cast<float>(track.health[i] - health) * IN_MILLISECONDS / age;
        return static_cast<float>(health) / perSecond;
    }

    return unknown;
}

namespace
{
    // [lifetime-calibrated] Against a mob above the group's level more of its damage misses and is resisted: the
    // measured dps is slowed by this much per level above the bot, to at most LEVEL_DPS_FLOOR of it.
    constexpr float LEVEL_DPS_STEP = 0.05f;
    constexpr float LEVEL_DPS_FLOOR = 0.75f;
}

float ExpectedLifetime(PlayerbotAI* botAI, Unit* target, std::string* source)
{
    if (!target || !target->IsAlive())
        return 0.0f;

    // [b35_mvaway] The rate its health really falls at is the damage dealt to this target: it alone counts once known.
    float lifetime = ObservedLifetime(target);
    if (lifetime < std::numeric_limits<float>::max())
    {
        if (source)
            *source = "its observed health loss";
        return lifetime;
    }

    float dps = botAI->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get();
    uint32 members = 0;
    uint32 onTarget = 0;
    if (Player* bot = botAI->GetBot())
    {
        int32 const above = static_cast<int32>(target->GetLevel()) - static_cast<int32>(bot->GetLevel());
        if (above > 0)
            dps *= std::max(LEVEL_DPS_FLOOR, 1.0f - LEVEL_DPS_STEP * static_cast<float>(above));

        // [b35_mvaway] Before that, the group's dps by the share of its members on the target (the bot counts: it is
        // about to hit it).
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->GetSource();
                if (!member || !member->IsAlive() || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
                    continue;

                ++members;
                if (member == bot || member->GetVictim() == target || member->GetTarget() == target->GetGUID())
                    ++onTarget;
            }

            if (members > 0 && onTarget < members)
                dps *= static_cast<float>(std::max<uint32>(onTarget, 1)) / static_cast<float>(members);
        }
    }

    if (source)
        *source = members ? Acore::StringFormat("{} of {} member(s) on it", onTarget, members) : "no group";

    if (dps > 0.0f)
        lifetime = std::min(lifetime, static_cast<float>(target->GetHealth()) / dps);
    return lifetime;
}

namespace
{
    constexpr uint32 GROUP_FIGHT_GAP_MS = 5 * IN_MILLISECONDS;    // no hit of the group this long: the fight ended
    constexpr uint32 GROUP_HIT_GAP_CAP_MS = 2 * IN_MILLISECONDS;  // a gap between hits counts this much at most
    constexpr uint32 GROUP_FIGHT_MIN_MS = 5 * IN_MILLISECONDS;    // a shorter fight measures nothing
    constexpr float GROUP_DPS_WEIGHT = 0.35f;                     // the newest fight's weight in the average
    constexpr uint32 GROUP_DPS_TTL_MS = 15 * MINUTE * IN_MILLISECONDS;
    constexpr size_t GROUP_DPS_MAX_TRACKED = 256;                 // the table is swept of stale groups past this

    struct GroupFightTrack
    {
        uint32 lastHitMs = 0;  // 0: no fight going on
        uint32 activeMs = 0;
        uint64 damage = 0;
        uint32 members = 0;    // the group's size in the fight
        std::string who;       // the member of the first hit (the log names the group by it)
        float rollingDps = 0.0f;
        uint32 fights = 0;
        uint32 rollingMembers = 0;
        uint32 updatedMs = 0;
        uint32 lastDealtMs = 0;  // the group's last hit, kept across fights ([combat_no_damage_release])
        uint64 totalDamage = 0;   // [lead86] every measured fight's damage and seconds (SustainedDps)
        uint64 totalActiveMs = 0;
    };

    struct ClosedFight
    {
        bool measured = false;
        std::string who;
        float seconds = 0.0f;
        uint64 damage = 0;
        float dps = 0.0f;
        float rollingDps = 0.0f;
        uint32 fights = 0;
    };

    std::mutex groupDamageLock;
    std::unordered_map<uint64, GroupFightTrack> groupDamage;  // by group GUID, under groupDamageLock

    // The fight is over: into the rolling average when it was long enough. Under groupDamageLock.
    ClosedFight CloseFight(GroupFightTrack& track, uint32 now)
    {
        ClosedFight closed;
        if (track.activeMs >= GROUP_FIGHT_MIN_MS && track.damage)
        {
            float const dps = static_cast<float>(track.damage) * IN_MILLISECONDS / static_cast<float>(track.activeMs);
            bool const fresh = track.fights && track.rollingMembers == track.members &&
                               getMSTimeDiff(track.updatedMs, now) < GROUP_DPS_TTL_MS;
            track.rollingDps = fresh ? track.rollingDps * (1.0f - GROUP_DPS_WEIGHT) + dps * GROUP_DPS_WEIGHT : dps;
            track.fights = fresh ? track.fights + 1 : 1;
            if (!fresh)
            {
                track.totalDamage = 0;
                track.totalActiveMs = 0;
            }
            track.totalDamage += track.damage;  // [lead86]
            track.totalActiveMs += track.activeMs;
            track.rollingMembers = track.members;
            track.updatedMs = now ? now : 1;
            closed.measured = true;
            closed.who = track.who;
            closed.seconds = static_cast<float>(track.activeMs) / IN_MILLISECONDS;
            closed.damage = track.damage;
            closed.dps = dps;
            closed.rollingDps = track.rollingDps;
            closed.fights = track.fights;
        }

        track.lastHitMs = 0;
        track.activeMs = 0;
        track.damage = 0;
        return closed;
    }

    void LogClosedFight(ClosedFight const& closed)
    {
        if (!closed.measured)
            return;

        LOG_INFO("playerbots",
                 "[lifetime-calibrated] group of {}: fight {:.1f} s, {} damage -> {:.0f} dps; rolling {:.0f} dps over "
                 "{} fight(s)",
                 closed.who, closed.seconds, closed.damage, closed.dps, closed.rollingDps, closed.fights);
    }
}

namespace GroupDamage
{
    void NoteDealt(Player* owner, uint32 amount)
    {
        Group* group = owner ? owner->GetGroup() : nullptr;
        if (!group || !amount)
            return;

        uint32 const now = getMSTime();
        ClosedFight closed;
        {
            std::lock_guard<std::mutex> guard(groupDamageLock);
            if (groupDamage.size() > GROUP_DPS_MAX_TRACKED)
            {
                for (auto it = groupDamage.begin(); it != groupDamage.end();)
                {
                    GroupFightTrack const& stale = it->second;
                    bool const idle = !stale.lastHitMs || getMSTimeDiff(stale.lastHitMs, now) > GROUP_DPS_TTL_MS;
                    bool const old = !stale.updatedMs || getMSTimeDiff(stale.updatedMs, now) > GROUP_DPS_TTL_MS;
                    if (idle && old)
                        it = groupDamage.erase(it);
                    else
                        ++it;
                }
            }

            GroupFightTrack& track = groupDamage[group->GetGUID().GetRawValue()];
            if (track.lastHitMs && getMSTimeDiff(track.lastHitMs, now) > GROUP_FIGHT_GAP_MS)
                closed = CloseFight(track, now);

            if (track.lastHitMs)
                track.activeMs += std::min(getMSTimeDiff(track.lastHitMs, now), GROUP_HIT_GAP_CAP_MS);
            else
            {
                track.members = group->GetMembersCount();
                track.who = owner->GetName();
            }

            track.lastHitMs = now ? now : 1;
            track.lastDealtMs = track.lastHitMs;
            track.damage += amount;
        }

        LogClosedFight(closed);
    }

    float SustainedDps(Player* bot)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        if (!group || !bot->GetMap() || !bot->GetMap()->IsDungeon())
            return 0.0f;
        std::lock_guard<std::mutex> guard(groupDamageLock);
        auto const found = groupDamage.find(group->GetGUID().GetRawValue());
        if (found == groupDamage.end() || !found->second.totalActiveMs ||
            found->second.rollingMembers != group->GetMembersCount())
            return 0.0f;
        return float(found->second.totalDamage) * IN_MILLISECONDS / float(found->second.totalActiveMs);
    }

    float CalibratedDps(Player* bot, uint32* fights)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        if (!group || !bot->GetMap() || !bot->GetMap()->IsDungeon())
            return 0.0f;

        uint32 const now = getMSTime();
        float dps = 0.0f;
        ClosedFight closed;
        {
            std::lock_guard<std::mutex> guard(groupDamageLock);
            auto const found = groupDamage.find(group->GetGUID().GetRawValue());
            if (found == groupDamage.end())
                return 0.0f;

            GroupFightTrack& track = found->second;
            if (track.lastHitMs && getMSTimeDiff(track.lastHitMs, now) > GROUP_FIGHT_GAP_MS)
                closed = CloseFight(track, now);

            if (track.fights && track.rollingMembers == group->GetMembersCount() &&
                getMSTimeDiff(track.updatedMs, now) < GROUP_DPS_TTL_MS)
            {
                dps = track.rollingDps;
                if (fights)
                    *fights = track.fights;
            }
        }

        LogClosedFight(closed);
        return dps;
    }

    namespace
    {
        constexpr uint32 NO_DAMAGE_RELEASE_AFTER_MS = 15 * IN_MILLISECONDS;  // in combat this long without a hit
        constexpr uint32 NO_DAMAGE_RELEASE_MS = 10 * IN_MILLISECONDS;        // the holds stay lifted this long
        constexpr uint32 NO_DAMAGE_EVAL_MS = 500;                            // the group's combat state is re-read

        struct ReleaseState
        {
            uint32 evalMs = 0;
            bool combat = false;
            uint32 quietSinceMs = 0;    // the start of the combat, or the group's last hit since
            uint32 seenDealtMs = 0;     // the last hit already counted
            uint32 releaseStartMs = 0;  // 0: not released
        };

        std::mutex releaseLock;
        std::unordered_map<uint64, ReleaseState> releaseStates;  // by group GUID, under releaseLock

        // A member on this map has a victim or is attacked.
        bool GroupInCombat(Group* group, Map* map)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsInWorld() && member->IsAlive() && member->GetMap() == map &&
                    (member->GetVictim() || !member->getAttackers().empty()))
                    return true;
            }

            return false;
        }
    }

    bool NoDamageRelease(Player* bot)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        Map* map = bot ? bot->GetMap() : nullptr;
        if (!group || !map || !map->IsDungeon())
            return false;

        uint64 const key = group->GetGUID().GetRawValue();
        uint32 const now = getMSTime();
        uint32 const stamp = now ? now : 1;
        uint32 lastDealt = 0;
        {
            std::lock_guard<std::mutex> guard(groupDamageLock);
            auto const found = groupDamage.find(key);
            if (found != groupDamage.end())
                lastDealt = found->second.lastDealtMs;
        }

        uint32 quietMs = 0;
        {
            std::lock_guard<std::mutex> guard(releaseLock);
            if (releaseStates.size() > GROUP_DPS_MAX_TRACKED)
            {
                for (auto it = releaseStates.begin(); it != releaseStates.end();)
                {
                    if (!it->second.combat && getMSTimeDiff(it->second.evalMs, now) > GROUP_DPS_TTL_MS)
                        it = releaseStates.erase(it);
                    else
                        ++it;
                }
            }

            ReleaseState& state = releaseStates[key];
            if (!state.evalMs || getMSTimeDiff(state.evalMs, now) >= NO_DAMAGE_EVAL_MS)
            {
                state.evalMs = stamp;
                if (!GroupInCombat(group, map))
                {
                    state = ReleaseState();
                    state.evalMs = stamp;
                    return false;
                }

                if (!state.combat)
                {
                    state.combat = true;
                    state.quietSinceMs = stamp;
                    state.seenDealtMs = lastDealt;  // a hit of an earlier fight does not count
                }
            }

            if (!state.combat)
                return false;

            if (lastDealt && lastDealt != state.seenDealtMs)
            {
                // the group hits again: the clock restarts and a release ends
                state.seenDealtMs = lastDealt;
                state.quietSinceMs = lastDealt;
                state.releaseStartMs = 0;
            }

            if (state.releaseStartMs)
            {
                if (getMSTimeDiff(state.releaseStartMs, now) < NO_DAMAGE_RELEASE_MS)
                    return true;

                state.releaseStartMs = 0;
                state.quietSinceMs = stamp;
                return false;
            }

            quietMs = getMSTimeDiff(state.quietSinceMs, now);
            if (quietMs < NO_DAMAGE_RELEASE_AFTER_MS)
                return false;

            state.releaseStartMs = stamp;
        }

        LOG_INFO("playerbots", "[combat_no_damage_release] group of {}: in combat, no member damage for {} s: "
                 "threat-open waits, approach / rest / pull holds and target vetoes lifted for {} s", bot->GetName(),
                 quietMs / IN_MILLISECONDS, NO_DAMAGE_RELEASE_MS / IN_MILLISECONDS);
        return true;
    }
}
