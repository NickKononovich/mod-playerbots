/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "KillOrder.h"

#include "Creature.h"
#include "EstimatedLifetimeValue.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MobRoles.h"
#include "MotionMaster.h"
#include "PartyStrengthValue.h"
#include "PlayerbotAIConfig.h"
#include "LeaderOrders.h"  // [lead18] LEADER_HEALER_HEALS_ONLY
#include "LeaderPlannerShared.h"  // [dun180] MobLandedDps
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "RtiTargetValue.h"
#include "RtiValue.h"
#include "TargetValue.h"
#include "ThreatManager.h"
#include "ThreatValues.h"
#include "Timer.h"

#include <algorithm>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace
{
    constexpr float PACK_RADIUS = 10.0f;            // a mob this close to its tank stands in the tank's pack
    // [b32_engage] milliseconds (getMSTime): on the whole-second time(nullptr) clock a 2 s wait lasted up to 3 s
    constexpr uint32 PULL_GRACE_MS = 2 * IN_MILLISECONDS;  // the tank's time to pick the pack up before the dps open
    // A tank soft for the content, or dps that outgear it (party strength): the dps give it longer to pick the
    // pack up, and hold the opening seconds even on what it already holds, so its first hits build a lead.
    constexpr uint32 CAREFUL_PULL_GRACE_MS = 4 * IN_MILLISECONDS;
    // The longest a dps waits for the tank's threat lead on a mob the tank holds ([threat-open]).
    constexpr uint32 OPEN_MAX_WAIT_MS = 4 * IN_MILLISECONDS;

    // One line per change of a bot's opening state ([threat-open]): waiting for the lead, opening.
    void LogThreatOpen(Player* bot, char const* state, uint32 sincePullMs)
    {
        static std::mutex logLock;
        static std::unordered_map<uint64, char const*> logged;
        {
            std::lock_guard<std::mutex> guard(logLock);
            char const*& last = logged[bot->GetGUID().GetRawValue()];
            if (last == state)
                return;
            last = state;
        }

        LOG_INFO("playerbots", "[threat-open] {} {} ({:.1f} s into the pull)", bot->GetName(), state,
                 sincePullMs / 1000.0f);
    }

    // A mob this close to the group member it attacks fights the group there (it reached a member it chased or
    // that walked off); a caster nuking from farther is the tank's to take, not the dps' to walk to.
    constexpr float GROUP_FIGHT_VICTIM_REACH = 10.0f;
    // A target the bot already fights stays one this much farther out, so it does not flip at the edge.
    constexpr float GROUP_FIGHT_KEEP_SLACK = 5.0f;
    constexpr uint32 GROUP_FIGHT_LOG_MS = 10 * IN_MILLISECONDS;

    bool IsTankOfGroup(Unit* unit, Player* bot)
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        return player && player->IsAlive() && player->IsInSameRaidWith(bot) && PlayerbotAI::IsTank(player);
    }

    bool HasCrowdControlIcon(Group* group, ObjectGuid guid)
    {
        for (uint8 icon = 0; icon < TARGETICONCOUNT; ++icon)
        {
            if (icon == RtiTargetValue::skullIndex || icon == RtiTargetValue::crossIndex)
                continue;

            if (group->GetTargetIcon(icon) == guid)
                return true;
        }

        return false;
    }
}

namespace KillOrder
{
    uint8 GetCasterTier(Unit* unit)
    {
        if (!unit)
            return 0;

        if (Creature* creature = unit->ToCreature())
        {
            uint8 const unitClass = creature->getClass();
            return (unitClass == CLASS_MAGE || unitClass == CLASS_PALADIN) && creature->GetMaxPower(POWER_MANA) > 0;
        }

        if (Player* player = unit->ToPlayer())
            return PlayerbotAI::IsCaster(player) || PlayerbotAI::IsHeal(player);

        return 0;
    }

    bool IsHeldByTank(Unit* unit, Player* bot) { return unit && IsTankOfGroup(unit->GetVictim(), bot); }

    bool HasHoldingTank(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->GetMap() == bot->GetMap() && member->IsAlive() &&
                PlayerbotAI::IsTank(member))
                return true;
        }

        return false;
    }

    PackFocus NarrowToTankPack(PlayerbotAI* botAI, GuidSet const& exclusions, GuidSet& narrowed)
    {
        Player* bot = botAI->GetBot();
        if (!bot->GetGroup() || bot->InBattleground() || PlayerbotAI::IsTank(bot))
            return PackFocus::None;

        AiObjectContext* context = botAI->GetAiObjectContext();
        // Read every time so the fallback clock sees the whole fight; "combat start time" is only kept by the
        // "wait for attack" strategy.
        // [b32_engage] The pull clock in milliseconds: the grace start's own getMSTime() stamp; "combat start
        // time" (the "wait for attack" strategy) is kept in whole seconds only.
        Value<time_t>* grace = context->GetValue<time_t>("pull grace start");
        time_t const graceStart = grace->Get();
        PullGraceStartValue const* graceValue = dynamic_cast<PullGraceStartValue const*>(grace);
        uint32 const graceStartMs = graceStart && graceValue ? graceValue->StartMs() : 0;
        time_t const combatStart = context->GetValue<time_t>("combat start time")->Get();

        // an encounter script that names its targets keeps them
        if (!context->GetValue<GuidVector>("prioritized targets")->Get().empty() || !HasHoldingTank(bot))
            return PackFocus::None;

        bool const ranged = PlayerbotAI::IsRanged(bot);
        GuidVector const attackers = context->GetValue<GuidVector>("attackers")->Get();
        std::vector<std::pair<ObjectGuid, int8>> tiers;
        GuidSet onMember;  // [dun185] healer mobs on a member, not the tank: no threat lead to wait for
        int8 best = -1;
        for (ObjectGuid const& guid : attackers)
        {
            if (exclusions.find(guid) != exclusions.end())
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive())
                continue;

            // A raid boss fight belongs to its strategy and the usual choice, not to the trash kill order. A 5-man
            // boss pulled with its trash (Mechanar's Gatewatcher Gyro-Kill) keeps it: dps on what the tank holds.
            if (Creature* creature = unit->ToCreature())
            {
                if ((creature->IsDungeonBoss() || creature->isWorldBoss()) && bot->GetMap() &&
                    bot->GetMap()->IsRaid())
                    return PackFocus::None;
            }

            int8 tier = -1;
            // [dun185] a healer mob fighting a member of the group is killed first wherever it stands, held by the
            // tank or not: it heals the pack from range and is on a member already (no threat to keep off it).
            // b433 wc-n five4 / five5: the Druids of the Fang (Healing Touch, Druid's Slumber) nuked and slept from
            // 19-32 yd, out of the kill order, while the group died to the pack they healed
            Unit* const victim = unit->GetVictim();
            Player* const victimPlayer = victim ? victim->ToPlayer() : nullptr;
            if (!IsCrowdControlled(unit) && MobRoles::IsHealer(unit) && victimPlayer &&
                victimPlayer->IsInSameRaidWith(bot) && IsInGroupFight(bot, unit))
            {
                tier = 2;
                if (!IsHeldByTank(unit, bot))
                    onMember.insert(guid);
            }
            else if (!IsCrowdControlled(unit) && IsHeldByTank(unit, bot))
            {
                tier = 0;
                // melee kill the casters standing in the tank's pack; ranged also those nuking from range
                if (GetCasterTier(unit) && (ranged || unit->GetDistance(unit->GetVictim()) <= PACK_RADIUS))
                    tier = 1;
                // [b30_mech_pos] a healer (SmartAI heal casts, MobRoles) before the other casters: zf-h
                // 20261025-184229, the Sandfury Witch Doctors' Flash Heal / Healing Ward kept 12 mobs alive 287 s
                if (MobRoles::IsHealer(unit) && (ranged || unit->GetDistance(unit->GetVictim()) <= PACK_RADIUS))
                    tier = 2;
            }

            tiers.emplace_back(guid, tier);
            best = std::max(best, tier);
        }

        bool const careful = context->GetValue<PartyStrength>("party strength")->Get().CarefulThreat();
        uint32 const sincePull =
            combatStart  ? static_cast<uint32>(std::max<time_t>(0, time(nullptr) - combatStart)) * IN_MILLISECONDS
            : graceStartMs ? getMSTimeDiff(graceStartMs, getMSTime())
            : graceStart   ? static_cast<uint32>(std::max<time_t>(0, time(nullptr) - graceStart)) * IN_MILLISECONDS
                           : CAREFUL_PULL_GRACE_MS;
        if (best < 0)
        {
            bool const pullStarting = sincePull < (careful ? CAREFUL_PULL_GRACE_MS : PULL_GRACE_MS);
            // [combat_no_damage_release] no member has hit anything for a while: no more waiting
            return pullStarting && !tiers.empty() && !GroupDamage::NoDamageRelease(bot) ? PackFocus::Wait
                                                                                       : PackFocus::None;
        }

        // [threat-open] User 2026-10-25: «Дать танку время забрать аггро — но боты не считают аггро. Пусть считают».
        // The engage audit of 161 runs that day: from the tank's first hit to the dps' own first action 1.9 s
        // (ranged), 2.0 s (melee), 2.7 s (casters) median, 4.6-6.7 s p90, behind fixed clocks (a careful group held
        // its first two seconds on everything). A dps opens on a mob the tank holds as soon as the mob's threat
        // table says it is safe: its threat after the next cast under the open share of the tank's (ThreatLead,
        // Omen's data), on the best tier first; with no such mob it waits for the tank's lead, at most
        // OPEN_MAX_WAIT_MS from the pull (a stunned or silenced tank builds none).
        // A mob this bot already fights stays open: from then on the threat multiplier ([threat-hold]) paces it.
        auto const open = [botAI, bot, careful, &onMember](ObjectGuid const& guid)
        {
            if (onMember.count(guid))
                return true;
            Unit* unit = botAI->GetUnit(guid);
            return unit && (unit->GetThreatMgr().GetThreat(bot) > 0.0f || ThreatLead::SafeToOpen(botAI, unit, careful));
        };

        narrowed = exclusions;
        bool anySafe = false;
        for (auto const& [guid, tier] : tiers)
        {
            if (tier < best)
            {
                narrowed.insert(guid);
                continue;
            }

            if (open(guid))
                anySafe = true;
        }

        bool const timedOut = sincePull >= OPEN_MAX_WAIT_MS;
        if (!anySafe && !timedOut && !GroupDamage::NoDamageRelease(bot))
        {
            LogThreatOpen(bot, "waits for the tank's threat lead", sincePull);
            return PackFocus::Wait;
        }

        // Of the best tier, the mobs the tank holds a safe lead on (all of them after the wait).
        if (anySafe)
        {
            for (auto const& [guid, tier] : tiers)
            {
                if (tier == best && !open(guid))
                    narrowed.insert(guid);
            }
        }

        LogThreatOpen(bot, anySafe ? "opens: the tank holds a safe threat lead" : "opens: waited the longest",
                      sincePull);
        return PackFocus::Narrowed;
    }

    std::vector<Unit*> RankTankPack(PlayerbotAI* tankAI, GuidSet const& skip)
    {
        std::vector<Unit*> pack;
        Player* tank = tankAI->GetBot();
        Group* group = tank->GetGroup();
        if (!group)
            return pack;

        // In a 5-man dungeon the Skull and the Cross never send the dps to a mob that holds the tank from deep in the
        // room (a caster left at range by a line-of-sight pull): it gets a mark once it comes (IsInTankFight).
        bool const limitToFight = tank->GetMap() && tank->GetMap()->IsNonRaidDungeon();
        GuidVector const attackers = tankAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
        for (ObjectGuid const& guid : attackers)
        {
            if (skip.find(guid) != skip.end())
                continue;

            Unit* unit = tankAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || unit->IsPlayer() || unit->GetVictim() != tank || IsCrowdControlled(unit) ||
                HasCrowdControlIcon(group, guid) || (limitToFight && !IsInTankFight(tank, unit)))
                continue;

            pack.push_back(unit);
        }

        SortKillOrder(pack, tank, tank->GetVictim());
        return pack;
    }

    void SortKillOrder(std::vector<Unit*>& pack, WorldObject const* anchor, Unit const* current)
    {
        // [b30_mech_pos] healers (MobRoles) first: [dun185] a healer that fights heals from wherever it stands
        // (b433 wc-n: the Druids of the Fang healed and slept the group from 19-32 yd, outside the 10 yd window);
        // one that fights nobody yet by the pack window
        auto const healerOf = [anchor](Unit* unit)
        {
            return MobRoles::IsHealer(unit) &&
                   (unit->GetVictim() || (anchor && unit->GetDistance(anchor) <= PACK_RADIUS));
        };
        // [dun180] then the damage each mob lands per second of its life under the group's fire: what it lands on
        // the member it fights (the planner's LandedMeleeDps: melee through the game's attack table and armor,
        // spells from its spell data) over its health - the death that takes the most damage off the group per
        // second of the group's damage first, wherever it stands (b424 rfc-n five5 pull 36: a Searing Blade
        // Warlock 17-20 yd off cast 33/s on the tank for 30 s while 8/s Cultists in the pack died first). A mob
        // that fights nobody yet (a pull's plan) keeps the caster prior: the casters in the pack first
        Player* const anchorPlayer = anchor ? ObjectAccessor::FindPlayer(anchor->GetGUID()) : nullptr;
        std::unordered_map<Unit*, float> rate;  // -1: it fights nobody
        for (Unit* unit : pack)
        {
            Unit* victim = unit->GetVictim();
            Player* on = victim ? victim->ToPlayer() : nullptr;
            if (!on && victim)
                on = anchorPlayer;
            rate[unit] = on ? PullPlanning::MobLandedDps(unit, on) / float(std::max<uint32>(unit->GetHealth(), 1))
                            : -1.0f;
        }
        auto const casterOf = [anchor](Unit* unit)
        { return anchor && unit->GetDistance(anchor) <= PACK_RADIUS && GetCasterTier(unit); };
        std::stable_sort(pack.begin(), pack.end(), [&](Unit* a, Unit* b)
        {
            bool const healerA = healerOf(a);
            bool const healerB = healerOf(b);
            if (healerA != healerB)
                return healerA;

            float const rateA = rate[a];
            float const rateB = rate[b];
            if (rateA >= 0.0f && rateB >= 0.0f)
            {
                if (rateA != rateB)
                    return rateA > rateB;
            }
            else if (bool const casterA = casterOf(a), casterB = casterOf(b); casterA != casterB)
                return casterA;

            if ((a == current) != (b == current))
                return a == current;

            return a->GetHealth() < b->GetHealth();
        });
    }

    bool IsKillMarkStale(PlayerbotAI* tankAI, uint8 icon, ObjectGuid ours)
    {
        Group* group = tankAI->GetBot()->GetGroup();
        if (!group)
            return false;

        ObjectGuid const current = group->GetTargetIcon(icon);
        if (!current)
            return true;

        Unit* unit = tankAI->GetUnit(current);
        if (!unit || !unit->IsAlive())
            return true;

        if (current != ours)
            return false;

        return !unit->IsInCombat() || IsCrowdControlled(unit);
    }

    bool IsTrashFight(Player* bot)
    {
        if (!bot->IsInCombat() || !bot->GetGroup() || bot->InBattleground())
            return false;

        InstanceScript* instance = bot->GetInstanceScript();
        return !instance || !instance->IsEncounterInProgress();
    }

    Player* FindFightTank(Player* bot)
    {
        Group* group = bot ? bot->GetGroup() : nullptr;
        if (!group)
            return nullptr;

        auto const usable = [bot](Player* member)
        {
            return member && member != bot && member->IsInWorld() && member->IsAlive() &&
                   member->GetMap() == bot->GetMap();
        };

        Player* mainTank = ObjectAccessor::FindPlayer(PlayerbotAI::GetMainTankGuid(group));
        if (usable(mainTank))
            return mainTank;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (usable(member) && PlayerbotAI::IsTank(member))
                return member;
        }

        return nullptr;
    }

    bool IsInTankFight(Player* tank, Unit* unit, bool keep)
    {
        float const radius = sPlayerbotAIConfig.DpsGroupFightRadius;
        if (!tank || !unit || radius <= 0.0f || !unit->IsInCombat() || unit->GetMap() != tank->GetMap())
            return true;

        // A boss fight belongs to its encounter strategy: its adds, totems and crystals stand where they stand.
        Creature* creature = unit->ToCreature();
        if (!creature || creature->IsDungeonBoss() || creature->isWorldBoss())
            return true;

        if (InstanceScript* instance = tank->GetInstanceScript())
        {
            if (instance->IsEncounterInProgress())
                return true;
        }

        // A mob running for help is shot before it brings the next pack (DpsTargetValue). The tank's own target
        // counts by the radius like any other: the mob a line-of-sight pull just hit stands 40 yd out in the room,
        // and the dps opening on it there walk to it (the tank chasing one far out takes the fight with it).
        if (unit->GetMotionMaster()->GetCurrentMovementGeneratorType() == ASSISTANCE_MOTION_TYPE)
            return true;

        float const slack = keep ? GROUP_FIGHT_KEEP_SLACK : 0.0f;
        if (unit->GetExactDist(tank) <= radius + slack)
            return true;

        Unit* victim = unit->GetVictim();
        Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
        return victimPlayer && victimPlayer->IsInSameRaidWith(tank) &&
               unit->GetExactDist(victimPlayer) <= GROUP_FIGHT_VICTIM_REACH + slack;
    }

    bool IsInGroupFight(Player* bot, Unit* unit, bool keep)
    {
        if (!bot || !unit || sPlayerbotAIConfig.DpsGroupFightRadius <= 0.0f || bot->InBattleground() ||
            PlayerbotAI::IsTank(bot))
            return true;

        Map* map = bot->GetMap();
        Group* group = bot->GetGroup();
        if (!map || !map->IsNonRaidDungeon() || !group || !unit->IsInCombat())
            return true;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return true;

        Player* tank = FindFightTank(bot);
        if (!tank)
            return true;

        // An encounter script, or a mark put out on purpose (a player, a script, a command), names the mob wherever
        // it stands. The tank's own kill marks follow its fight: a mark it put on a mob that stays out in the room
        // after a line-of-sight pull is no reason to walk there.
        AiObjectContext* context = botAI->GetAiObjectContext();
        ObjectGuid const guid = unit->GetGUID();
        GuidVector const& prioritized = context->GetValue<GuidVector>("prioritized targets")->RefGet();
        if (std::find(prioritized.begin(), prioritized.end(), guid) != prioritized.end())
            return true;

        int32 const rtiIndex = RtiTargetValue::GetRtiIndex(context->GetValue<std::string>("rti")->Get());
        if (group->GetTargetIcon(RtiTargetValue::skullIndex) == guid ||
            group->GetTargetIcon(RtiTargetValue::crossIndex) == guid ||
            (rtiIndex >= 0 && group->GetTargetIcon(uint8(rtiIndex)) == guid))
        {
            PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
            TankKillMarks const* own =
                tankAI ? &tankAI->GetAiObjectContext()->GetValue<TankKillMarks>("tank kill marks")->RefGet() : nullptr;
            if (!own || (own->skull != guid && own->cross != guid))
                return true;
        }

        return IsInTankFight(tank, unit, keep);
    }

    void ExcludeOutsideGroupFight(PlayerbotAI* botAI, GuidSet& exclusions)
    {
        Player* bot = botAI->GetBot();
        Map* map = bot->GetMap();
        if (sPlayerbotAIConfig.DpsGroupFightRadius <= 0.0f || !map || !map->IsNonRaidDungeon() ||
            PlayerbotAI::IsTank(bot))
            return;
        // [lead18] LEADER_HEALER_HEALS_ONLY: a led healer evaluates no dps target (vetoed_eval when asked)
        if (Leader::LEADER_HEALER_HEALS_ONLY && PlayerbotAI::IsHeal(bot) && Leader::ExecutorMember(bot))
        {
            Leader::Events::NoteVetoedEval(bot->GetGUID(), "dps range (KillOrder)");
            return;
        }

        Player* tank = FindFightTank(bot);
        if (!tank)
            return;

        uint32 const now = getMSTime();
        for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            if (exclusions.find(guid) != exclusions.end())
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || IsInGroupFight(bot, unit))
                continue;

            exclusions.insert(guid);

            // Bots of different maps run on different threads.
            static std::mutex lock;
            static std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> logged;  // by bot: mob, ms
            bool log = false;
            {
                std::lock_guard<std::mutex> guard(lock);
                auto& [mob, ms] = logged[bot->GetGUID()];
                if (mob != guid || getMSTimeDiff(ms, now) >= GROUP_FIGHT_LOG_MS)
                {
                    mob = guid;
                    ms = now;
                    log = true;
                }
            }

            if (log)
            {
                Unit* victim = unit->GetVictim();
                LOG_INFO("playerbots", "[dps range] {} skips {} ({}): {:.1f} yd from the tank {}, {:.1f} from the bot, "
                         "on {}", bot->GetName(), unit->GetName(), guid.GetCounter(), unit->GetExactDist(tank),
                         tank->GetName(), unit->GetExactDist(bot), victim ? victim->GetName() : std::string("nobody"));
            }
        }
    }

    // [b33_hadronox] boss fight with adds -------------------------------------------------------------------------

    namespace
    {
        // A group healer this low with a mob on it that no tank holds: the damage dealers help peel it.
        constexpr float BOSS_FOCUS_HEALER_PEEL_PCT = 50.0f;
        // A healer mob counts as kill-first in the group's fight (it heals the boss or the adds).
        constexpr float BOSS_FOCUS_HEALER_MOB_REACH = 40.0f;
        constexpr uint32 BOSS_FOCUS_LOG_MS = 15 * IN_MILLISECONDS;

        bool IsBossFightMap(Player* bot)
        {
            Map* map = bot->GetMap();
            if (!map || !map->IsNonRaidDungeon() || !bot->GetGroup() || bot->InBattleground() || !bot->IsInCombat())
                return false;

            InstanceScript* instance = bot->GetInstanceScript();
            return instance && instance->IsEncounterInProgress();
        }

        bool IsFightableBoss(Player* bot, Unit* unit)
        {
            Creature* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || (!creature->IsDungeonBoss() && !creature->isWorldBoss()) ||
                !creature->IsInCombat() || creature->IsInEvadeMode() ||
                creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
                creature->IsImmuneToPC())
                return false;

            // fighting the group: on a member (or a member's pet), or a member on its threat list
            Unit* victim = creature->GetVictim();
            Player* owner = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            return (owner && (owner == bot || bot->IsInSameRaidWith(owner))) || creature->IsInCombatWith(bot);
        }

        void LogBossFocus(Player* bot, char const* who, Unit* boss, char const* why)
        {
            static std::mutex lock;
            static std::unordered_map<uint64, std::pair<char const*, uint32>> logged;
            uint32 const now = getMSTime();
            {
                std::lock_guard<std::mutex> guard(lock);
                auto& [last, ms] = logged[bot->GetGUID().GetRawValue()];
                if (last == why && getMSTimeDiff(ms, now) < BOSS_FOCUS_LOG_MS)
                    return;
                last = why;
                ms = now;
            }
            LOG_INFO("playerbots", "[boss focus] {} ({}) {}: {}", bot->GetName(), who, boss->GetName(), why);
        }
    }

    bool IsTankOwnKillMark(Player* bot, ObjectGuid guid)
    {
        Player* tank = FindFightTank(bot);
        if (PlayerbotAI::IsTank(bot) && !tank)
            tank = bot;
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!tankAI || !guid)
            return false;

        TankKillMarks const& own = tankAI->GetAiObjectContext()->GetValue<TankKillMarks>("tank kill marks")->RefGet();
        return own.skull == guid || own.cross == guid;
    }

    Unit* FindFightBoss(PlayerbotAI* botAI, GuidSet const& exclusions)
    {
        Player* bot = botAI->GetBot();
        if (!IsBossFightMap(bot))
            return nullptr;

        AiObjectContext* context = botAI->GetAiObjectContext();
        Unit* current = context->GetValue<Unit*>("current target")->Get();
        Unit* best = nullptr;
        bool currentIsBoss = false;
        // [b33_fix1] A spawned trash mob (no boss, no summon) among the attackers: a trash fight the boss joined by
        // his own clock, not a boss fight with his adds. dtk-n 20261026-012429-5A-43 (194-233 s): the Wretched
        // Belcher patrol came while Trollgore's invader wave ran (encounter in progress, he on the group from 84 yd);
        // the tank held him and the damage dealers focused him while 6 Ghoul Tormentors, 3 Flesheating Ghouls and a
        // Soulmage roamed the healer and the hunter (32 aggro losses, wipe). The same patrol + wave on build 30
        // (20261025-184228-dtk-n-5A-1, 212-285 s) was tanked add by add: 20 kills, 6 aggro losses, no death.
        bool trash = false;
        for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
        {
            if (exclusions.find(guid) != exclusions.end())
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!IsFightableBoss(bot, unit))
            {
                Creature* creature = unit ? unit->ToCreature() : nullptr;
                if (creature && creature->IsAlive() && creature->IsInCombat() && !creature->IsSummon() &&
                    !creature->IsDungeonBoss() && !creature->isWorldBoss())
                    trash = true;
                continue;
            }

            if (unit == current)
                currentIsBoss = true;

            if (!currentIsBoss && (!best || unit->GetHealthPct() < best->GetHealthPct()))
                best = unit;
        }
        if (trash)
            return nullptr;
        return currentIsBoss ? current : best;
    }

    Unit* BossFocusTarget(PlayerbotAI* botAI, GuidSet const& exclusions)
    {
        Player* bot = botAI->GetBot();
        if (PlayerbotAI::IsTank(bot) || !IsBossFightMap(bot))
            return nullptr;

        AiObjectContext* context = botAI->GetAiObjectContext();
        // an encounter script that names its targets keeps them
        if (!context->GetValue<GuidVector>("prioritized targets")->RefGet().empty())
            return nullptr;

        Unit* boss = FindFightBoss(botAI, exclusions);
        if (!boss)
            return nullptr;

        Group* group = bot->GetGroup();
        for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
        {
            if (guid == boss->GetGUID() || exclusions.find(guid) != exclusions.end())
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || IsCrowdControlled(unit))
                continue;

            // a mark put out on purpose (a player, a script), not the tank's own automatic one
            if (group && (group->GetTargetIcon(RtiTargetValue::skullIndex) == guid ||
                          group->GetTargetIcon(RtiTargetValue::crossIndex) == guid) &&
                !IsTankOwnKillMark(bot, guid))
            {
                LogBossFocus(bot, "dps", boss, "an add carries an explicit kill mark, it goes first");
                return nullptr;
            }

            // a healer mob in the fight heals the boss or the adds ([b30_mech_pos] tier 2)
            if (MobRoles::IsHealer(unit) && bot->GetExactDist(unit) <= BOSS_FOCUS_HEALER_MOB_REACH)
            {
                LogBossFocus(bot, "dps", boss, "a healer add fights, it goes first");
                return nullptr;
            }

            // a mob on a group healer in trouble that no tank holds: help peel it (the tank comes for it too)
            Unit* victim = unit->GetVictim();
            Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
            if (victimPlayer && victimPlayer != bot && bot->IsInSameRaidWith(victimPlayer) &&
                PlayerbotAI::IsHeal(victimPlayer) && victimPlayer->GetHealthPct() < BOSS_FOCUS_HEALER_PEEL_PCT)
            {
                LogBossFocus(bot, "dps", boss, "an add is on the healer below 50 %, peel it first");
                return nullptr;
            }
        }

        LogBossFocus(bot, "dps", boss, "single target on the boss, area spells on the adds stacked on it");
        return boss;
    }

    Unit* TankHoldBoss(PlayerbotAI* tankAI, GuidSet const& exclusions)
    {
        Player* bot = tankAI->GetBot();
        if (!PlayerbotAI::IsTank(bot) || !IsBossFightMap(bot))
            return nullptr;

        Unit* boss = FindFightBoss(tankAI, exclusions);
        if (!boss)
            return nullptr;

        // another tank of the group holds it (an off-tank fight): the usual choice
        Unit* bossVictim = boss->GetVictim();
        if (bossVictim && bossVictim != bot && IsTankOfGroup(bossVictim, bot))
            return nullptr;

        // a mob on a group healer comes first, as before (FindTankTargetSmartStrategy level 4)
        AiObjectContext* context = tankAI->GetAiObjectContext();
        for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
        {
            if (guid == boss->GetGUID() || exclusions.find(guid) != exclusions.end())
                continue;

            Unit* unit = tankAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || IsCrowdControlled(unit))
                continue;

            Unit* victim = unit->GetVictim();
            Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
            if (victimPlayer && victimPlayer != bot && bot->IsInSameRaidWith(victimPlayer) &&
                PlayerbotAI::IsHeal(victimPlayer))
                return nullptr;
        }

        LogBossFocus(bot, "tank", boss, "holds the boss, the adds come to it by taunt and area threat");
        return boss;
    }
}
