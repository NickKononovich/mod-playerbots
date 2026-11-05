/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidLeaderState.h"

#include "AiObjectContext.h"
#include "AttackAction.h"
#include "Creature.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperation.h"
#include "PlayerbotTextMgr.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "SelfBotControl.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <fstream>
#include <memory>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace
{
    constexpr size_t REGISTRY_PRUNE_SIZE = 256;
    constexpr uint32 REGISTRY_STALE_MS = 2 * HOUR * IN_MILLISECONDS;

    constexpr uint32 SPELL_RESURRECTION_RANK_1 = 2006;
    constexpr uint32 SPELL_REDEMPTION_RANK_1 = 7328;
    constexpr uint32 SPELL_ANCESTRAL_SPIRIT_RANK_1 = 2008;
    constexpr uint32 SPELL_REVIVE_RANK_1 = 50769;

    std::mutex wipeLogLock;

    bool KnowsSpellChain(Player* player, uint32 firstRank, bool requireReady)
    {
        for (uint32 spellId = firstRank; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
            if (player->HasSpell(spellId) && (!requireReady || !player->HasSpellCooldown(spellId)))
                return true;

        return false;
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

    char const* RoleOf(Player* player)
    {
        if (PlayerbotAI::IsTank(player))
            return "tank";
        if (PlayerbotAI::IsHeal(player))
            return "healer";
        return "dps";
    }

    // One JSON object per line in <LogsDir>/wipes.jsonl, next to the death records of the same fight.
    void WriteWipeLog(Player* caller, Unit* boss, std::string const& reason, uint32 fightMs)
    {
        Map* map = caller->GetMap();
        Group* group = caller->GetGroup();
        if (!map || !group)
            return;

        std::time_t const wallClock = std::time(nullptr);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &wallClock);
#else
        localtime_r(&wallClock, &localTime);
#endif

        std::ostringstream out;
        out << "{\"time\":\"" << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << "\"";
        out << ",\"map\":" << map->GetId() << ",\"mapName\":" << Json(map->GetMapName())
            << ",\"difficulty\":" << uint32(map->GetDifficulty()) << ",\"instance\":" << map->GetInstanceId();
        out << ",\"reason\":" << Json(reason) << ",\"caller\":" << Json(caller->GetName())
            << ",\"fightSec\":" << fightMs / IN_MILLISECONDS;

        if (boss)
            out << ",\"boss\":{\"name\":" << Json(boss->GetName()) << ",\"entry\":" << boss->GetEntry()
                << ",\"hpPct\":" << boss->GetHealthPct() << "}";

        out << ",\"group\":[";
        bool first = true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetMap() != map)
                continue;

            out << (first ? "" : ",") << "{\"name\":" << Json(member->GetName()) << ",\"class\":"
                << uint32(member->getClass()) << ",\"role\":\"" << RoleOf(member)
                << "\",\"alive\":" << (member->IsAlive() ? "true" : "false") << ",\"hpPct\":" << member->GetHealthPct()
                << ",\"manaPct\":" << (member->GetMaxPower(POWER_MANA) ? member->GetPowerPct(POWER_MANA) : 0.0f)
                << ",\"dist\":" << caller->GetDistance(member) << "}";
            first = false;
        }
        out << "]}";

        std::lock_guard<std::mutex> lock(wipeLogLock);
        std::ofstream file(sLog->GetLogsDir() + "wipes.jsonl", std::ios::app);
        if (file)
            file << out.str() << '\n';
    }

    // Group member flags broadcast a group update and write to the characters DB: done on the world thread.
    class RaidLeaderTankFlagsOperation : public PlayerbotOperation
    {
    public:
        RaidLeaderTankFlagsOperation(ObjectGuid coordinatorGuid, ObjectGuid groupGuid, ObjectGuid mainTank,
                                     ObjectGuid mainAssist)
            : _coordinatorGuid(coordinatorGuid), _groupGuid(groupGuid), _mainTank(mainTank), _mainAssist(mainAssist)
        {
        }

        bool Execute() override
        {
            Player* coordinator = ObjectAccessor::FindPlayer(_coordinatorGuid);
            Group* group = coordinator ? coordinator->GetGroup() : nullptr;
            if (!group || group->GetGUID() != _groupGuid || !group->isRaidGroup())
                return false;

            Apply(group, _mainTank, MEMBER_FLAG_MAINTANK);
            Apply(group, _mainAssist, MEMBER_FLAG_MAINASSIST);
            return true;
        }

        ObjectGuid GetBotGuid() const override { return _coordinatorGuid; }
        std::string GetName() const override { return "RaidLeaderTankFlags"; }

    private:
        static void Apply(Group* group, ObjectGuid wanted, GroupMemberFlags flag)
        {
            if (!wanted || !group->IsMember(wanted))
                return;

            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                if (slot.guid == wanted && (slot.flags & flag))
                    return;

            group->SetGroupMemberFlag(wanted, true, flag);  // clears the flag from its previous holder
        }

        ObjectGuid _coordinatorGuid;
        ObjectGuid _groupGuid;
        ObjectGuid _mainTank;
        ObjectGuid _mainAssist;
    };
}

namespace RaidLeader
{
    Registry& Registry::Instance()
    {
        static Registry instance;
        return instance;
    }

    GroupState& Registry::Touch(ObjectGuid groupGuid)
    {
        uint32 const now = getMSTime();
        if (_groups.size() > REGISTRY_PRUNE_SIZE)
        {
            for (auto itr = _groups.begin(); itr != _groups.end();)
            {
                if (getMSTimeDiff(itr->second.lastTouchMs, now) > REGISTRY_STALE_MS)
                    itr = _groups.erase(itr);
                else
                    ++itr;
            }
        }

        GroupState& state = _groups[groupGuid];
        state.lastTouchMs = now;
        return state;
    }

    bool IsEnabled() { return sPlayerbotAIConfig.RaidLeader; }

    Player* FindCoordinator(Player* member)
    {
        if (!member || !IsEnabled())
            return nullptr;

        Group* group = member->GetGroup();
        Map* map = member->GetMap();
        if (!group || !map || !map->IsDungeon() || member->InBattleground() || member->InArena())
            return nullptr;

        if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()))
        {
            if (!GET_PLAYERBOT_AI(leader))
            {
                // A person leads from the game client: a tank bot (else the first bot) coordinates for him; the
                // boss pull waits for the person (PullHoldValue, RaidLeaderHoldMultiplier).
                if (!sPlayerbotAIConfig.RaidLeaderForHumanLeader)
                    return nullptr;

                Player* firstBot = nullptr;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* player = ref->GetSource();
                    if (!player || !player->IsInWorld() || player->GetMap() != map || !player->IsAlive() ||
                        !GET_PLAYERBOT_AI(player))
                        continue;

                    if (PlayerbotAI::IsTank(player))
                        return player;

                    if (!firstBot)
                        firstBot = player;
                }

                return firstBot;
            }

            if (leader->IsAlive() && leader->GetMap() == map)
                return leader;
        }

        // The leader is dead or elsewhere: the first living bot of the group on this map speaks instead.
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* player = ref->GetSource();
            if (player && player->IsInWorld() && player->GetMap() == map && player->IsAlive() &&
                GET_PLAYERBOT_AI(player))
                return player;
        }

        return nullptr;
    }

    bool IsBossCreature(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
    }

    bool CanResurrect(Player* player)
    {
        static constexpr uint32 firstRanks[] = {SPELL_RESURRECTION_RANK_1, SPELL_REDEMPTION_RANK_1,
                                                SPELL_ANCESTRAL_SPIRIT_RANK_1, SPELL_REVIVE_RANK_1,
                                                SPELL_REBIRTH_RANK_1};

        for (uint32 firstRank : firstRanks)
            if (KnowsSpellChain(player, firstRank, false))
                return true;

        return false;
    }

    bool HasBattleResurrection(Player* player)
    {
        if (!player || !player->IsAlive())
            return false;

        return KnowsSpellChain(player, SPELL_REBIRTH_RANK_1, true) ||
               (player->HasSpell(SPELL_RAISE_ALLY) && !player->HasSpellCooldown(SPELL_RAISE_ALLY));
    }

    bool IsEatingOrDrinking(Player* player, bool& drinking)
    {
        drinking = false;
        bool eating = false;
        for (auto const& [spellId, aurApp] : player->GetAppliedAuras())
        {
            SpellInfo const* spellInfo = aurApp->GetBase()->GetSpellInfo();
            switch (spellInfo->GetSpellSpecific())
            {
                case SPELL_SPECIFIC_FOOD:
                    eating = true;
                    break;
                case SPELL_SPECIFIC_DRINK:
                case SPELL_SPECIFIC_FOOD_AND_DRINK:
                    drinking = true;
                    break;
                default:
                    break;
            }
        }

        return eating || drinking;
    }

    bool IsOffensiveAction(Action* action, Player* bot)
    {
        if (!action || dynamic_cast<CastHealingSpellAction*>(action))
            return false;

        // Encounter actions that derive from AttackAction only for its movement helpers and place the bot for a
        // mechanic ("bronjahm group position", dodge, spread): positioning, never held.
        if (dynamic_cast<AttackAction*>(action))
        {
            static constexpr std::string_view positioning[] = {"position", "avoid", "dodge", "spread",
                                                               "maneuver", "move",  "kite",  "flee"};
            std::string const& name = action->getName();
            for (std::string_view word : positioning)
                if (name.find(word) != std::string::npos)
                    return false;

            return true;
        }

        if (dynamic_cast<CastShootAction*>(action))
            return true;

        if (action->getThreatType() == Action::ActionThreatType::None)
            return false;

        Unit* target = action->GetTarget();
        return target && bot->IsHostileTo(target);
    }

    bool IsTankDefendingGroup(PlayerbotAI* botAI)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        if (!bot || !bot->IsAlive() || !PlayerbotAI::IsTank(bot))
            return false;

        if (bot->IsInCombat())
            return true;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        // a mob on a member before the cached "attackers" lists it
        if (botAI->GetAiObjectContext()->GetValue<Unit*>("defend member target")->Get())
            return true;

        // "attackers" also lists a skull mark nobody fights yet: a member's own combat state decides.
        if (botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get().empty())
            return false;

        Map* map = bot->GetMap();
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->GetMap() == map && member->IsAlive() && member->IsInCombat())
                return true;
        }

        return false;
    }

    void Announce(PlayerbotAI* botAI, std::string const& textKey, std::string const& defaultText,
                  std::map<std::string, std::string> placeholders)
    {
        Player* bot = botAI->GetBot();
        Group* group = bot->GetGroup();
        if (!group)
            return;

        std::string const text =
            PlayerbotTextMgr::instance().GetBotTextOrDefault(textKey, defaultText, std::move(placeholders));
        if (group->isRaidGroup())
            botAI->SayToRaid(text);
        else
            botAI->SayToParty(text);
    }

    void RecordReadyAnswer(Player* member, bool ready, std::string const& reason)
    {
        Group* group = member->GetGroup();
        if (!group)
            return;

        Registry::Instance().With(group->GetGUID(),
                                  [&](GroupState& state)
                                  {
                                      if (state.phase == ReadyCheckPhase::Pending)
                                          state.answers[member->GetGUID()] = {ready, reason};
                                  });
    }

    bool IsAutomaticReadyCheckPending(Player* member)
    {
        Group* group = member->GetGroup();
        if (!group)
            return false;

        return Registry::Instance().With(group->GetGUID(), [](GroupState& state)
                                         { return state.phase == ReadyCheckPhase::Pending; });
    }

    void NoteEngage(Player* member, Unit* boss)
    {
        Group* group = member->GetGroup();
        if (!group || !boss)
            return;

        uint32 const now = getMSTime();
        Registry::Instance().With(group->GetGUID(),
                                  [&](GroupState& state)
                                  {
                                      if (state.engagedBoss)
                                          return;

                                      state.engagedBoss = boss->GetGUID();
                                      state.engageMs = now;
                                      state.pullAnnounced = false;
                                      state.hpMarks = 0;
                                      state.lustAnnounced = false;
                                      state.battleResAnnounced.clear();
                                      state.wipeCalled = false;
                                      state.escapeOrderMs = 0;
                                      state.holdBoss.Clear();
                                      state.phase = ReadyCheckPhase::Idle;
                                      state.attempts = 0;
                                  });
    }

    void ClearEngage(Player* member)
    {
        Group* group = member->GetGroup();
        if (!group)
            return;

        Registry::Instance().With(group->GetGUID(),
                                  [](GroupState& state)
                                  {
                                      state.engagedBoss.Clear();
                                      state.engageMs = 0;
                                      state.pullAnnounced = false;
                                      state.hpMarks = 0;
                                      state.lustAnnounced = false;
                                      state.battleResAnnounced.clear();
                                      state.wipeCalled = false;
                                  });
    }

    bool CallWipe(Player* caller, Unit* boss, std::string const& reason)
    {
        Group* group = caller->GetGroup();
        if (!group)
            return false;

        uint32 const now = getMSTime();
        uint32 fightMs = 0;
        bool const fresh = Registry::Instance().With(
            group->GetGUID(),
            [&](GroupState& state)
            {
                if (state.wipeCalled || (state.recovering && !state.engagedBoss))
                    return false;

                fightMs = state.engageMs ? getMSTimeDiff(state.engageMs, now) : 0;
                state.wipeCalled = true;
                state.recovering = true;
                state.wipeMs = now;
                state.phase = ReadyCheckPhase::Idle;
                state.attempts = 0;
                state.readyBoss.Clear();
                state.readyMs = 0;
                state.holdBoss = state.engagedBoss;
                state.holdSinceMs = now;
                return true;
            });

        if (fresh)
            WriteWipeLog(caller, boss, reason, fightMs);

        return fresh;
    }

    bool OrderWipeEscape(Player* caller)
    {
        Group* group = caller->GetGroup();
        if (!group)
            return false;

        uint32 const now = std::max(getMSTime(), 1u);
        return Registry::Instance().With(group->GetGUID(),
                                         [now](GroupState& state)
                                         {
                                             if (state.escapeOrderMs &&
                                                 getMSTimeDiff(state.escapeOrderMs, now) < WIPE_ESCAPE_ORDER_MS)
                                                 return false;

                                             state.escapeOrderMs = now;
                                             return true;
                                         });
    }

    bool IsWipeEscapeOrdered(Player* member)
    {
        Group* group = member->GetGroup();
        if (!group)
            return false;

        uint32 const now = getMSTime();
        return Registry::Instance().With(group->GetGUID(),
                                         [now](GroupState& state)
                                         {
                                             return state.escapeOrderMs &&
                                                    getMSTimeDiff(state.escapeOrderMs, now) < WIPE_ESCAPE_ORDER_MS;
                                         });
    }

    void QueueTankFlags(Player* coordinator, ObjectGuid mainTank, ObjectGuid mainAssist)
    {
        Group* group = coordinator->GetGroup();
        if (!group)
            return;

        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<RaidLeaderTankFlagsOperation>(
            coordinator->GetGUID(), group->GetGUID(), mainTank, mainAssist));
    }

    namespace
    {
        // Group orders of a leader ("pause", "res <name>"). Kept apart from the Registry: every grouped bot asks
        // about them, and the Registry creates (and prunes) an entry per asked group. The counters keep the common
        // "no group has one" answer free of the lock.
        struct PauseInfo
        {
            uint32 sinceMs = 0;
            ObjectGuid by;
        };

        struct GroupOrders
        {
            std::mutex lock;
            std::unordered_map<ObjectGuid, PauseInfo> paused;
            std::unordered_map<ObjectGuid, ObjectGuid> resurrectFirst;
            std::atomic<uint32> pausedCount{0};
            std::atomic<uint32> resurrectFirstCount{0};
        };

        GroupOrders& Orders()
        {
            static GroupOrders orders;
            return orders;
        }

        // Drops the pause of `groupGuid` if it is still the one that started at `sinceMs`; true when dropped.
        bool ClearPause(ObjectGuid groupGuid, uint32 sinceMs)
        {
            GroupOrders& orders = Orders();
            std::lock_guard<std::mutex> guard(orders.lock);
            auto const itr = orders.paused.find(groupGuid);
            if (itr == orders.paused.end() || (sinceMs && itr->second.sinceMs != sinceMs))
                return false;

            orders.paused.erase(itr);
            orders.pausedCount.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
    }

    bool SetPaused(Player* member, bool paused, Player* by)
    {
        Group* group = member ? member->GetGroup() : nullptr;
        if (!group)
            return false;

        if (!paused)
            return ClearPause(group->GetGUID(), 0);

        GroupOrders& orders = Orders();
        std::lock_guard<std::mutex> guard(orders.lock);
        if (orders.paused.count(group->GetGUID()))
            return false;

        orders.paused[group->GetGUID()] = {std::max(getMSTime(), 1u), by ? by->GetGUID() : ObjectGuid::Empty};
        orders.pausedCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool IsPaused(Player* member)
    {
        GroupOrders& orders = Orders();
        if (!member || !orders.pausedCount.load(std::memory_order_relaxed))
            return false;

        Group* group = member->GetGroup();
        if (!group)
            return false;

        PauseInfo info;
        {
            std::lock_guard<std::mutex> guard(orders.lock);
            auto const itr = orders.paused.find(group->GetGUID());
            if (itr == orders.paused.end())
                return false;

            info = itr->second;
        }

        if (!info.by)
            return true;

        // Back at the client: the first input after the grace ends the pause.
        uint32 const lastInput = SelfBotControl::GetLastInputMs(info.by);
        if (!lastInput || int32(lastInput - info.sinceMs) < int32(PAUSE_INPUT_GRACE_MS))
            return true;

        if (ClearPause(group->GetGUID(), info.sinceMs))
        {
            LOG_DEBUG("playerbots", "Group pause ended: the person who paused is back ({})", member->GetName());
            if (Player* person = ObjectAccessor::FindPlayer(info.by))
                SelfBotControl::SendAddonStatus(person);
        }

        return false;
    }

    uint32 GetPausedMs(Player* member, ObjectGuid* by)
    {
        if (!IsPaused(member))
            return 0;

        GroupOrders& orders = Orders();
        std::lock_guard<std::mutex> guard(orders.lock);
        auto const itr = orders.paused.find(member->GetGroup()->GetGUID());
        if (itr == orders.paused.end())
            return 0;

        if (by)
            *by = itr->second.by;

        return std::max(getMSTimeDiff(itr->second.sinceMs, getMSTime()), 1u);
    }

    bool IsLedByPerson(Player* member)
    {
        Group* group = member ? member->GetGroup() : nullptr;
        if (!group)
            return false;

        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
        return leader && !GET_PLAYERBOT_AI(leader);
    }

    void SetResurrectFirst(Player* member, ObjectGuid target)
    {
        Group* group = member ? member->GetGroup() : nullptr;
        if (!group)
            return;

        GroupOrders& orders = Orders();
        std::lock_guard<std::mutex> guard(orders.lock);
        auto const itr = orders.resurrectFirst.find(group->GetGUID());
        if (!target)
        {
            if (itr != orders.resurrectFirst.end())
            {
                orders.resurrectFirst.erase(itr);
                orders.resurrectFirstCount.fetch_sub(1, std::memory_order_relaxed);
            }
            return;
        }

        if (itr == orders.resurrectFirst.end())
            orders.resurrectFirstCount.fetch_add(1, std::memory_order_relaxed);

        orders.resurrectFirst[group->GetGUID()] = target;
    }

    ObjectGuid GetResurrectFirst(Player* member)
    {
        GroupOrders& orders = Orders();
        if (!member || !orders.resurrectFirstCount.load(std::memory_order_relaxed))
            return ObjectGuid::Empty;

        Group* group = member->GetGroup();
        if (!group)
            return ObjectGuid::Empty;

        std::lock_guard<std::mutex> guard(orders.lock);
        auto const itr = orders.resurrectFirst.find(group->GetGUID());
        return itr == orders.resurrectFirst.end() ? ObjectGuid::Empty : itr->second;
    }

    namespace
    {
        struct ResurrectVerdicts
        {
            std::mutex lock;
            std::unordered_map<ObjectGuid, std::pair<ResurrectVerdict, ObjectGuid>> byBody;  // verdict, judge
        };

        ResurrectVerdicts& Verdicts()
        {
            static ResurrectVerdicts verdicts;
            return verdicts;
        }
    }

    void SetResurrectVerdict(ObjectGuid body, ResurrectVerdict verdict, ObjectGuid judge)
    {
        ResurrectVerdicts& verdicts = Verdicts();
        std::lock_guard<std::mutex> guard(verdicts.lock);
        if (verdict == ResurrectVerdict::Unknown)
            verdicts.byBody.erase(body);
        else
            verdicts.byBody[body] = {verdict, judge};
    }

    ResurrectVerdict GetResurrectVerdict(Player* body)
    {
        if (!body)
            return ResurrectVerdict::Unknown;
        ResurrectVerdict verdict = ResurrectVerdict::Unknown;
        ObjectGuid judge;
        {
            ResurrectVerdicts& verdicts = Verdicts();
            std::lock_guard<std::mutex> guard(verdicts.lock);
            auto const itr = verdicts.byBody.find(body->GetGUID());
            if (itr == verdicts.byBody.end())
                return ResurrectVerdict::Unknown;
            verdict = itr->second.first;
            judge = itr->second.second;
        }
        Player* const judging = judge.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(judge);
        if (!judging || !judging->IsAlive() || judging->GetMap() != body->GetMap())
            return ResurrectVerdict::Unknown;
        return verdict;
    }
}
