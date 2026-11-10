/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotsUserPack.h"
#include "BotPath.h"  // [lead109]

#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LeaderDungeonRun.h"  // [diag251]
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SelfTravelActions.h"
#include "TestHarnessData.h"
#include "Trainer.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "World.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr uint32 TICK_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 LOGIN_RETRY_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 STREAKS_RELOAD_MS = 60 * IN_MILLISECONDS;
    constexpr float GO_ARRIVED_RANGE = 30.0f;
    constexpr float TRAINER_REACH = 6.0f;

    struct PackConfig
    {
        bool enabled = false;
        std::vector<ObjectGuid> guids;
        std::unordered_set<uint32> lows;
        uint32 leader = 0;
        float xpRate = 0.5f;
        uint32 loginDelaySecs = 30;
        std::string streaksFile;
        uint32 minStreak = 3;
        uint32 levelBelow = 3;
        uint32 levelAbove = 10;
        uint32 statusIntervalSecs = 600;
        std::string statusLog;
        uint32 autoTrainEvery = 2;
        bool solo = false;  // [guild1] every character plays alone, no pack group, no hand-over to the user
    };

    PackConfig cfg;

    // Read on map threads (teleport gate), written on the world thread.
    std::mutex streakLock;
    std::map<std::string, uint32> streaks;

    // Deaths since midnight (server local time), written on map threads.
    std::mutex deathLock;
    std::unordered_map<uint32, uint32> deathsToday;
    int deathDay = -1;

    // Orders typed in chat (any thread), executed on the world thread.
    struct PendingCommand
    {
        ObjectGuid from;
        std::string text;
    };
    std::mutex commandLock;
    std::vector<PendingCommand> pendingCommands;

    int LocalDay()
    {
        time_t const now = time(nullptr);
        tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        return local.tm_year * 1000 + local.tm_yday;
    }

    std::string WallClock()
    {
        time_t const now = time(nullptr);
        tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
        return buf;
    }

    // ASCII and Cyrillic to lower case (UTF-8, the byte length stays the same).
    std::string Lower(std::string const& text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            unsigned char const c = text[i];
            unsigned char const next = i + 1 < text.size() ? text[i + 1] : 0;
            if (c == 0xD0 && next >= 0x90 && next <= 0x9F)
            {
                out += char(0xD0);
                out += char(next + 0x20);
                ++i;
            }
            else if (c == 0xD0 && next >= 0xA0 && next <= 0xAF)
            {
                out += char(0xD1);
                out += char(next - 0x20);
                ++i;
            }
            else if (c == 0xD0 && next == 0x81)
            {
                out += char(0xD1);
                out += char(0x91);
                ++i;
            }
            else
                out += c < 0x80 ? char(std::tolower(c)) : char(c);
        }
        return out;
    }

    std::string Trim(std::string const& text)
    {
        size_t const begin = text.find_first_not_of(" \t");
        if (begin == std::string::npos)
            return "";
        size_t const end = text.find_last_not_of(" \t");
        return text.substr(begin, end - begin + 1);
    }

    bool Contains(char const* haystack, std::string const& needleLower)
    {
        return haystack && *haystack && Lower(haystack).find(needleLower) != std::string::npos;
    }

    bool Contains(std::string const& haystack, std::string const& needleLower)
    {
        return !haystack.empty() && Lower(haystack).find(needleLower) != std::string::npos;
    }

    LocaleConstant DbcLocale() { return sWorld->GetDefaultDbcLocale(); }

    char const* DbcName(char const* const* names)
    {
        char const* name = names[DbcLocale()];
        if (!name || !*name)
            name = names[0];
        return name ? name : "";
    }

    std::string ZoneName(uint32 id)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
        return area ? DbcName(area->area_name) : std::to_string(id);
    }

    std::string MapName(uint32 id)
    {
        MapEntry const* map = sMapStore.LookupEntry(id);
        return map ? DbcName(map->name) : std::to_string(id);
    }

    void LoadConfig()
    {
        PackConfig next;
        next.enabled = sConfigMgr->GetOption<bool>("Playerbots.UserPack.Enabled", false, false);
        std::string const guids = sConfigMgr->GetOption<std::string>("Playerbots.UserPack.Guids", "", false);
        std::stringstream in(guids);
        std::string token;
        while (std::getline(in, token, ','))
        {
            uint32 const low = uint32(std::strtoul(Trim(token).c_str(), nullptr, 10));
            if (low && next.lows.insert(low).second)
                next.guids.push_back(ObjectGuid::Create<HighGuid::Player>(low));
        }
        next.leader = sConfigMgr->GetOption<uint32>("Playerbots.UserPack.Leader", 0, false);
        next.xpRate = sConfigMgr->GetOption<float>("Playerbots.UserPack.XpRate", 0.5f, false);
        next.loginDelaySecs = sConfigMgr->GetOption<uint32>("Playerbots.UserPack.LoginDelaySecs", 30, false);
        next.streaksFile = sConfigMgr->GetOption<std::string>("Playerbots.UserPack.StreaksFile", "", false);
        if (next.streaksFile.empty())
            next.streaksFile = sLog->GetLogsDir() + "tests/streaks.json";
        next.minStreak = sConfigMgr->GetOption<uint32>("Playerbots.UserPack.MinStreak", 3, false);
        next.levelBelow = sConfigMgr->GetOption<uint32>("Playerbots.UserPack.DungeonLevelBelow", 3, false);
        next.levelAbove = sConfigMgr->GetOption<uint32>("Playerbots.UserPack.DungeonLevelAbove", 10, false);
        next.statusIntervalSecs = sConfigMgr->GetOption<uint32>("Playerbots.UserPack.StatusIntervalSecs", 600, false);
        next.statusLog = sConfigMgr->GetOption<std::string>("Playerbots.UserPack.StatusLog", "pack_status.log", false);
        if (!next.statusLog.empty() && next.statusLog.find(':') == std::string::npos && next.statusLog[0] != '/')
            next.statusLog = sLog->GetLogsDir() + next.statusLog;
        next.autoTrainEvery = sConfigMgr->GetOption<uint32>("Playerbots.UserPack.AutoTrainEveryLevels", 2, false);
        next.solo = sConfigMgr->GetOption<bool>("Playerbots.UserPack.Solo", false, false);
        if (next.guids.empty())
            next.enabled = false;
        cfg = next;

        if (cfg.enabled)
            LOG_INFO("playerbots", "User pack: {} characters, xp rate {:.2f}, dungeons need {} clean test runs ({})",
                     cfg.guids.size(), cfg.xpRate, cfg.minStreak, cfg.streaksFile);
    }

    // streaks.json of the test harness, one content per line: "key":{"streak":N,"lastResult":"...",...}
    void LoadStreaks()
    {
        std::map<std::string, uint32> loaded;
        std::ifstream file(cfg.streaksFile);
        std::string line;
        while (file && std::getline(file, line))
        {
            size_t const open = line.find('"');
            size_t const close = open == std::string::npos ? open : line.find('"', open + 1);
            size_t const field = line.find("\"streak\":");
            if (close == std::string::npos || field == std::string::npos)
                continue;
            loaded[line.substr(open + 1, close - open - 1)] =
                uint32(std::strtoul(line.c_str() + field + 9, nullptr, 10));
        }
        std::lock_guard<std::mutex> guard(streakLock);
        streaks.swap(loaded);
    }

    uint32 Streak(std::string const& key)
    {
        std::lock_guard<std::mutex> guard(streakLock);
        auto const itr = streaks.find(key);
        return itr == streaks.end() ? 0 : itr->second;
    }

    bool Proven(TestHarness::ContentDef const& content) { return Streak(content.key) >= cfg.minStreak; }

    // Arrival point of an areatrigger_teleport id.
    AreaTriggerTeleport const* Entrance(uint32 trigger) { return sObjectMgr->GetAreaTriggerTeleport(trigger); }

    // The content a teleport into (mapId, difficulty) at (x, y, z) belongs to; nullptr: none the harness knows.
    // A map with wings (Scarlet Monastery): the wing whose entrance arrival is nearest.
    TestHarness::ContentDef const* ContentFor(uint32 mapId, uint8 difficulty, float x, float y, float z,
                                              bool& ambiguous)
    {
        ambiguous = false;
        std::vector<TestHarness::ContentDef const*> candidates;
        for (TestHarness::ContentDef const& content : TestHarness::Contents())
            if (content.mapId == mapId && content.difficulty == difficulty)
                candidates.push_back(&content);
        if (candidates.size() <= 1)
            return candidates.empty() ? nullptr : candidates.front();

        TestHarness::ContentDef const* best = nullptr;
        float bestDist = 0.0f;
        for (TestHarness::ContentDef const* content : candidates)
            for (uint32 trigger : content->entrances)
                if (AreaTriggerTeleport const* at = Entrance(trigger))
                {
                    float const dx = at->target_X - x;
                    float const dy = at->target_Y - y;
                    float const dz = at->target_Z - z;
                    float const dist = dx * dx + dy * dy + dz * dz;
                    if (!best || dist < bestDist)
                    {
                        best = content;
                        bestDist = dist;
                    }
                }
        if (best)
            return best;
        ambiguous = true;
        return candidates.front();
    }

    // Teleport gate: the instance (mapId, difficulty) at that arrival point is proven by the test harness.
    bool InstanceAllowed(uint32 mapId, uint8 difficulty, float x, float y, float z, std::string& why)
    {
        bool ambiguous = false;
        TestHarness::ContentDef const* content = ContentFor(mapId, difficulty, x, y, z, ambiguous);
        if (!content)
        {
            why = "no test content for map " + std::to_string(mapId) + " difficulty " + std::to_string(difficulty);
            return false;
        }
        if (ambiguous)
        {
            for (TestHarness::ContentDef const& other : TestHarness::Contents())
                if (other.mapId == mapId && other.difficulty == difficulty && !Proven(other))
                {
                    why = std::string(other.key) + " streak " + std::to_string(Streak(other.key));
                    return false;
                }
            return true;
        }
        if (!Proven(*content))
        {
            why = std::string(content->key) + " streak " + std::to_string(Streak(content->key));
            return false;
        }
        return true;
    }

    bool IsHuman(Player* player) { return player && player->GetSession() && !player->GetSession()->IsHeadless(); }

    char const* RpgStatusName(NewRpgStatus status)
    {
        switch (status)
        {
            case RPG_IDLE:
                return "IDLE";
            case RPG_GO_GRIND:
                return "GO_GRIND";
            case RPG_GO_CAMP:
                return "GO_CAMP";
            case RPG_WANDER_RANDOM:
                return "WANDER_RANDOM";
            case RPG_WANDER_NPC:
                return "WANDER_NPC";
            case RPG_DO_QUEST:
                return "DO_QUEST";
            case RPG_TRAVEL_FLIGHT:
                return "TRAVEL_FLIGHT";
            case RPG_REST:
                return "REST";
            case RPG_OUTDOOR_PVP:
                return "OUTDOOR_PVP";
            case RPG_DO_GATHER:
                return "DO_GATHER";
            case RPG_DO_PROFESSION:
                return "DO_PROFESSION";
            default:
                return "?";
        }
    }

    std::string RpgStrategy() { return sPlayerbotAIConfig.EnableNewRpgStrategy ? "new rpg" : "rpg"; }

    enum class Mode : uint8
    {
        Off,
        Autonomous,  // nobody plays: the pack plays on its own under a bot leader
        Human,       // the user plays one of the pack: the others are his bots
        Paused       // the pack account sits at the character screen or is logging in: nobody is logged in
    };

    char const* ModeName(Mode mode)
    {
        switch (mode)
        {
            case Mode::Autonomous:
                return "autonomous";
            case Mode::Human:
                return "user plays";
            case Mode::Paused:
                return "paused (account at character screen)";
            default:
                return "off";
        }
    }

    enum class OrderType : uint8
    {
        None,
        Go,
        Fly,
        Dungeon,
        Train
    };

    struct Order
    {
        OrderType type = OrderType::None;
        std::string title;
        WorldPosition pos;
        uint32 toNode = 0;
        TestHarness::ContentDef const* content = nullptr;
        bool human = false;  // given while the user plays: every bot carries it out, then waits ("stay")
        uint32 startMs = 0;
        std::set<ObjectGuid> executors;
        std::map<ObjectGuid, uint32> trainers;  // Train: the trainer entry per bot
        std::map<ObjectGuid, WorldPosition> trainerPos;
        struct Flight
        {
            uint32 masterEntry = 0;
            WorldPosition masterPos;
            std::vector<uint32> path;
        };
        std::map<ObjectGuid, Flight> flights;
        std::set<ObjectGuid> reissued;  // the status was set again once (a strategy change may have reset it)
    };

    class UserPackMgr
    {
    public:
        static UserPackMgr& Instance()
        {
            static UserPackMgr instance;
            return instance;
        }

        void Start()
        {
            _startMs = getMSTime();
            _nextStatusLogMs = getMSTime() + 60 * IN_MILLISECONDS;
            LoadStreaks();
            _streaksMs = getMSTime();
        }

        Mode GetMode() const { return _mode; }
        bool IsLeader(ObjectGuid guid) const { return _leader == guid; }

        void Update(uint32 diff)
        {
            if (!cfg.enabled)
            {
                _mode = Mode::Off;
                return;
            }

            _timer += diff;
            if (_timer < TICK_MS)
                return;
            _timer = 0;

            uint32 const now = getMSTime();
            if (getMSTimeDiff(_streaksMs, now) > STREAKS_RELOAD_MS)
            {
                LoadStreaks();
                _streaksMs = now;
            }

            Player* human = nullptr;
            _mode = ComputeMode(human);
            _humanGuid = human ? human->GetGUID() : ObjectGuid::Empty;

            bool const startDelayOver = getMSTimeDiff(_startMs, now) >= cfg.loginDelaySecs * IN_MILLISECONDS;
            if (startDelayOver)
            {
                // Paused: the user is at the character screen; the bots still online play on, none is logged in.
                // [guild1] solo: the characters the user does not play live on alone whatever he does; the one he
                // logs into is his (the secure login takes it from the bot holder), the others are not handed over.
                if (cfg.solo)
                    TickAutonomous(now, true);
                else if (_mode == Mode::Autonomous || _mode == Mode::Paused)
                    TickAutonomous(now, _mode == Mode::Autonomous);
                else if (_mode == Mode::Human)
                    TickHuman(human, now);
            }

            ProcessCommands();
            TickOrder(now);

            if (cfg.statusIntervalSecs && int32(now - _nextStatusLogMs) >= 0)
            {
                _nextStatusLogMs = now + cfg.statusIntervalSecs * IN_MILLISECONDS;
                WriteStatusLog();
            }
        }

        // Any thread.
        static void QueueCommand(ObjectGuid from, std::string const& text)
        {
            std::lock_guard<std::mutex> guard(commandLock);
            pendingCommands.push_back({from, text});
        }

        std::vector<std::string> Console(std::string const& text)
        {
            std::vector<std::string> replies;
            _console = &replies;
            if (!cfg.enabled)
                Say("User pack is off (Playerbots.UserPack.Enabled = 0)");
            else if (!HandleCommand(nullptr, text))
                Say("Unknown pack command. Try: dungeon list | dungeon <name> | fly <place> | goto <place> | train |"
                    " pack stop | pack status");
            _console = nullptr;
            return replies;
        }

        std::vector<std::string> Status()
        {
            std::vector<std::string> lines;
            std::ostringstream head;
            head << "User pack: " << ModeName(_mode);
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(_leader))
                head << ", leader " << leader->GetName();
            if (_order.type != OrderType::None)
                head << ", order: " << _order.title << " (" << getMSTimeDiff(_order.startMs, getMSTime()) / 60000
                     << " min)";
            lines.push_back(head.str());
            for (ObjectGuid const& guid : cfg.guids)
                lines.push_back(StatusLine(guid));
            return lines;
        }

    private:
        Mode ComputeMode(Player*& human)
        {
            std::set<uint32> accounts;
            for (ObjectGuid const& guid : cfg.guids)
                if (uint32 account = sCharacterCache->GetCharacterAccountIdByGuid(guid))
                    accounts.insert(account);

            bool paused = false;
            for (uint32 account : accounts)
            {
                WorldSession* session = sWorldSessionMgr->FindSession(account);
                if (!session)
                    continue;
                Player* player = session->GetPlayer();
                if (!player || !player->IsInWorld())
                    paused = true;
                else if (cfg.lows.count(player->GetGUID().GetCounter()))
                    human = player;
            }
            if (human)
                return Mode::Human;
            return paused ? Mode::Paused : Mode::Autonomous;
        }

        bool MayTryLogin(ObjectGuid guid, uint32 now)
        {
            auto itr = _lastLogin.find(guid);
            if (itr != _lastLogin.end() && getMSTimeDiff(itr->second, now) < LOGIN_RETRY_MS)
                return false;
            _lastLogin[guid] = now;
            return true;
        }

        // Bots of the pack that are in the world and played by the AI, the human's character excluded.
        std::vector<Player*> OnlineBots()
        {
            std::vector<Player*> bots;
            for (ObjectGuid const& guid : cfg.guids)
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
                // [guild6] the user's own character under ".playerbots bot self" plays like the rest of the pack
                if (bot && bot->IsInWorld() && (!IsHuman(bot) || IsSelfBot(bot)) && GET_PLAYERBOT_AI(bot) &&
                    !bot->IsBeingTeleported())
                    bots.push_back(bot);
            }
            return bots;
        }

        void TickAutonomous(uint32 now, bool allowLogin)
        {
            for (ObjectGuid const& guid : cfg.guids)
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
                if (!bot)
                {
                    if (allowLogin && MayTryLogin(guid, now))
                    {
                        LOG_INFO("playerbots", "User pack: logging in {} as a bot", guid.ToString());
                        sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
                    }
                    continue;
                }

                // Left over from the user's session (his bot): his manager logs it out with him; until then wait.
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                if (!cfg.solo && botAI && botAI->GetMaster() && IsRealPlayer(botAI->GetMaster()))
                    return;
            }

            std::vector<Player*> bots = OnlineBots();
            if (bots.empty())
                return;

            if (cfg.solo)
            {
                // Each bot leads itself; one taken by the user as his bot (his master) is left to him.
                _leader = bots.front()->GetGUID();
                for (Player* bot : bots)
                {
                    if (_order.type != OrderType::None && _order.executors.count(bot->GetGUID()))
                        continue;
                    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                    if (botAI && botAI->GetMaster() && IsRealPlayer(botAI->GetMaster()) && !IsSelfBot(bot))
                        continue;
                    EnsureAutonomousStrategies(bot, true);
                }
                MaybeAutoTrain(bots);
                return;
            }

            Player* leader = PickLeader(bots);
            _leader = leader->GetGUID();
            if (bots.size() > 1)
                EnsureGroup(leader, bots);

            for (Player* bot : bots)
            {
                if (_order.type != OrderType::None && _order.executors.count(bot->GetGUID()))
                    continue;
                EnsureAutonomousStrategies(bot, bot == leader);
                // RandomPlayerbotMgr::OnBotLoginInternal freezes XP of every bot it logs in when RandomBotFixedLevel
                // is on; the pack levels by playing, so the flag set there is taken back.
                if (sPlayerbotAIConfig.RandomBotFixedLevel && bot->HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN))
                    bot->RemovePlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
            }

            MaybeAutoTrain(bots);
        }

        Player* PickLeader(std::vector<Player*> const& bots)
        {
            // Keep a pack group's current leader while it is one of these bots.
            if (Group* group = bots.front()->GetGroup())
                for (Player* bot : bots)
                    if (group->GetLeaderGUID() == bot->GetGUID() && GroupIsPackOnly(group))
                        return bot;
            if (cfg.leader)
                for (Player* bot : bots)
                    if (bot->GetGUID().GetCounter() == cfg.leader)
                        return bot;
            return bots.front();
        }

        static bool GroupIsPackOnly(Group* group)
        {
            if (group->isBGGroup() || group->isBFGroup() || group->isLFGGroup())
                return false;
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                if (!cfg.lows.count(slot.guid.GetCounter()))
                    return false;
            return true;
        }

        void EnsureGroup(Player* leader, std::vector<Player*> const& bots)
        {
            Group* group = leader->GetGroup();
            if (group && !GroupIsPackOnly(group))
            {
                // Only the pack's own character leaves; the other group and its members are not touched.
                group->RemoveMember(leader->GetGUID());
                group = leader->GetGroup();
            }
            if (!group)
            {
                group = new Group();
                if (!group->Create(leader))
                {
                    delete group;
                    return;
                }
                sGroupMgr->AddGroup(group);
                LOG_INFO("playerbots", "User pack: group created, leader {}", leader->GetName());
            }
            if (group->GetLeaderGUID() != leader->GetGUID())
                group->ChangeLeader(leader->GetGUID());

            for (Player* bot : bots)
            {
                if (bot == leader || bot->GetGroup() == group)
                    continue;
                if (Group* old = bot->GetGroup())
                    old->RemoveMember(bot->GetGUID());
                if (!group->IsFull() && group->AddMember(bot))
                    LOG_INFO("playerbots", "User pack: {} joined the group of {}", bot->GetName(), leader->GetName());
            }
        }

        // The random bots' open-world play (AiFactory: "grind" + "new rpg"/"rpg" for a group leader, "follow" for
        // its members), given explicitly: the pack is not a random bot, so AiFactory gives it none of it.
        void EnsureAutonomousStrategies(Player* bot, bool leader)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return;
            std::string const rpg = RpgStrategy();
            MapEntry const* map = sMapStore.LookupEntry(bot->GetMapId());
            bool const inDungeon = map && map->IsDungeon();
            // [diag251] a member of a guild five queued for the Dungeon Finder keeps the run's strategies (the run
            // turned off its rpg / grind / quest play). Put back here on the next tick, the members quested and fought
            // on while queued: their answers waited for those fights and the port left the fighting ones outside (b428
            // fives 11 / 17 / 22 / 40 / 49: "User pack: Rumanatol plays as leader" right after the queue)
            // [dun178] inside too, for the whole run: its leader owns its members' strategies (the run turned off
            // grind, rpg, travel and quest play and gave "instance portal"). b428: 27617 "plays as leader (in a
            // dungeon)" passes put grind back and took "instance portal" off the five's members, run after run
            Leader::Dungeon::DungeonRuns const& runs = Leader::Dungeon::DungeonRuns::Instance();
            if (runs.OwnsMember(bot->GetGUID()) || (!inDungeon && runs.PlannedLfgRoles(bot->GetGUID())))
                return;
            bool ok = true;
            if (leader)
                ok = botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT) &&
                     botAI->HasStrategy(rpg, BOT_STATE_NON_COMBAT) != inDungeon &&
                     !botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) &&
                     !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT);
            else
                ok = botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) &&
                     !botAI->HasStrategy(rpg, BOT_STATE_NON_COMBAT) &&
                     !botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT) &&
                     !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT);
            ok = ok && botAI->HasStrategy("ghost portal", BOT_STATE_DEAD);
            if (ok)
                return;

            // Never the random bots' "lfg", "bg", "start duel" or "maintenance".
            std::string const common = ",-lfg,-bg,-start duel,-stay,-passive,-instance portal,+breathe";
            if (leader)
                botAI->ChangeStrategy("+grind,-follow,-move random,-travel" + common + (inDungeon ? ",-" : ",+") + rpg,
                                      BOT_STATE_NON_COMBAT);
            else
                botAI->ChangeStrategy("+follow,-grind,-new rpg,-rpg,-move random,-travel" + common,
                                      BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("-stay,-passive,+breathe", BOT_STATE_COMBAT);
            botAI->ChangeStrategy("+ghost portal", BOT_STATE_DEAD);
            LOG_INFO("playerbots", "User pack: {} plays as {}{}", bot->GetName(), leader ? "leader" : "member",
                     inDungeon ? " (in a dungeon: no new rpg)" : "");
        }

        void TickHuman(Player* human, uint32 now)
        {
            PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(human);
            if (!mgr)
                return;

            _leader = human->GetGUID();
            for (ObjectGuid const& guid : cfg.guids)
            {
                if (guid == human->GetGUID())
                    continue;

                Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
                if (!bot)
                {
                    if (MayTryLogin(guid, now))
                    {
                        LOG_INFO("playerbots", "User pack: {} plays, logging in {} as his bot", human->GetName(),
                                 guid.ToString());
                        mgr->AddPlayerBot(guid, human->GetSession()->GetAccountId());
                    }
                    continue;
                }

                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                if (!botAI || IsHuman(bot) || botAI->GetMaster() == human)
                    continue;

                // In his group, the bot takes him as its master by itself (RandomPlayerbotMgr::OnPlayerLogin,
                // PlayerbotAI::UpdateAIGroupMaster) and plays on alone when he logs out (OnPlayerLogout).
                if (human->GetGroup() && bot->GetGroup() == human->GetGroup())
                    continue;

                // Not in his group: log it out of the random bot holder, the next tick logs it in again as the user's
                // own bot, the alt bot path (group invite, follow, his commands).
                if (sRandomPlayerbotMgr.GetPlayerBot(guid))
                {
                    LOG_INFO("playerbots", "User pack: handing {} over to {}", bot->GetName(), human->GetName());
                    SelfTravel::ClearPortalGoal(guid);
                    sRandomPlayerbotMgr.LogoutPlayerBot(guid);
                    _lastLogin.erase(guid);
                }
            }

            // The user leads his pack: the pack group's bot leader hands the lead over.
            if (Group* group = human->GetGroup())
                if (group->GetLeaderGUID() != human->GetGUID() && GroupIsPackOnly(group) &&
                    cfg.lows.count(group->GetLeaderGUID().GetCounter()))
                    group->ChangeLeader(human->GetGUID());
        }

        // ---- orders -------------------------------------------------------------------------------------------

        void ProcessCommands()
        {
            std::vector<PendingCommand> commands;
            {
                std::lock_guard<std::mutex> guard(commandLock);
                commands.swap(pendingCommands);
            }
            for (PendingCommand const& command : commands)
            {
                Player* from = ObjectAccessor::FindConnectedPlayer(command.from);
                if (from)
                    HandleCommand(from, command.text);
            }
        }

        // Says a reply in party chat through a pack bot, to the user as a system message without one, and to the
        // console when the order came from there.
        void Say(std::string const& text)
        {
            LOG_INFO("playerbots", "User pack: {}", text);
            if (_console)
            {
                _console->push_back(text);
                return;
            }
            for (Player* bot : OnlineBots())
                if (Group* group = bot->GetGroup())
                {
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_UNIVERSAL, bot, nullptr, text);
                    group->BroadcastPacket(&data, false);
                    return;
                }
            if (Player* human = ObjectAccessor::FindConnectedPlayer(_humanGuid))
                ChatHandler(human->GetSession()).SendSysMessage(text);
        }

        // True when the text is a pack order (handled, also when refused).
        bool HandleCommand(Player* /*from*/, std::string const& raw)
        {
            std::string text = Trim(raw);
            std::string lower = Lower(text);

            auto strip = [&text, &lower](std::string const& prefix) -> bool
            {
                if (lower.compare(0, prefix.size(), prefix) != 0 ||
                    (lower.size() > prefix.size() && lower[prefix.size()] != ' '))
                    return false;
                text = Trim(text.substr(prefix.size()));
                lower = Lower(text);
                return true;
            };

            bool const packPrefix = strip("pack") || strip("\xD0\xBF\xD0\xB0\xD0\xBA");  // "пак"
            if (packPrefix && (lower.empty() || lower == "status" || lower == "?"))
            {
                for (std::string const& line : Status())
                    Say(line);
                return true;
            }
            if (packPrefix && (lower == "stop" || lower == "\xD1\x81\xD1\x82\xD0\xBE\xD0\xBF"))  // "стоп"
            {
                CancelOrder("cancelled by command");
                return true;
            }
            if (lower == "\xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0")  // "отмена"
            {
                CancelOrder("cancelled by command");
                return true;
            }
            // "данж", "лети", "иди"
            if (strip("dungeon") || strip("\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB6"))
            {
                CommandDungeon(text, lower);
                return true;
            }
            if (strip("fly") || strip("\xD0\xBB\xD0\xB5\xD1\x82\xD0\xB8"))
            {
                CommandFly(lower);
                return true;
            }
            if (strip("goto") || (packPrefix && strip("go")) || strip("\xD0\xB8\xD0\xB4\xD0\xB8"))
            {
                CommandGo(text, lower);
                return true;
            }
            if (packPrefix && (strip("train") || strip("\xD1\x83\xD1\x87\xD0\xB8\xD1\x82\xD1\x8C\xD1\x81\xD1\x8F")))
            {
                CommandTrain();
                return true;
            }
            return packPrefix;
        }

        void PackLevels(uint8& minLevel, uint8& maxLevel)
        {
            minLevel = 255;
            maxLevel = 0;
            for (ObjectGuid const& guid : cfg.guids)
            {
                uint8 level = 0;
                if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                    level = player->GetLevel();
                else if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid))
                    level = entry->Level;
                if (!level)
                    continue;
                minLevel = std::min(minLevel, level);
                maxLevel = std::max(maxLevel, level);
            }
            if (minLevel == 255)
                minLevel = maxLevel = 1;
        }

        bool LevelFits(TestHarness::ContentDef const& content, uint8 minLevel, uint8 maxLevel)
        {
            return uint32(minLevel) + cfg.levelBelow >= content.level &&
                   uint32(maxLevel) <= uint32(content.level) + cfg.levelAbove;
        }

        // Dungeon by key ("wc", "wc-n", "sm-gy-n") or name (content name or the map name in any DBC locale).
        std::vector<TestHarness::ContentDef const*> FindDungeons(std::string const& lower)
        {
            std::vector<TestHarness::ContentDef const*> exact;
            std::vector<TestHarness::ContentDef const*> named;
            bool const heroic = lower.size() > 2 && lower.compare(lower.size() - 2, 2, "-h") == 0;
            for (TestHarness::ContentDef const& content : TestHarness::Contents())
            {
                std::string const key = content.key;
                if (key == lower || key == lower + "-n")
                {
                    exact.push_back(&content);
                    continue;
                }
                if (lower.size() < 3 || (content.difficulty != 0) != heroic)
                    continue;
                bool match = Contains(content.name, lower);
                if (MapEntry const* map = sMapStore.LookupEntry(content.mapId))
                    for (uint8 i = 0; i < TOTAL_LOCALES && !match; ++i)
                        match = Contains(map->name[i], lower);
                if (match)
                    named.push_back(&content);
            }
            return exact.empty() ? named : exact;
        }

        std::string StreakText(TestHarness::ContentDef const& content)
        {
            return std::to_string(Streak(content.key)) + "/" + std::to_string(cfg.minStreak);
        }

        void CommandDungeon(std::string const& text, std::string const& lower)
        {
            uint8 minLevel = 0;
            uint8 maxLevel = 0;
            PackLevels(minLevel, maxLevel);

            if (lower.empty() || lower == "list" || lower == "?" ||
                lower == "\xD1\x81\xD0\xBF\xD0\xB8\xD1\x81\xD0\xBE\xD0\xBA")  // "список"
            {
                std::ostringstream allowed;
                std::ostringstream waiting;
                for (TestHarness::ContentDef const& content : TestHarness::Contents())
                {
                    if (content.raid || !LevelFits(content, minLevel, maxLevel))
                        continue;
                    std::ostringstream& out = Proven(content) ? allowed : waiting;
                    out << (out.tellp() > 0 ? ", " : "") << content.name << " [" << content.key << "]";
                    if (!Proven(content))
                        out << " " << StreakText(content);
                }
                Say("Group level " + std::to_string(minLevel) + "-" + std::to_string(maxLevel) +
                    ". Possible: " + (allowed.tellp() > 0 ? allowed.str() : std::string("nothing")));
                if (waiting.tellp() > 0)
                    Say("Not yet checked by test bots (series): " + waiting.str());
                return;
            }

            std::vector<TestHarness::ContentDef const*> found = FindDungeons(lower);
            if (found.empty())
            {
                Say("Unknown dungeon \"" + text + "\". Say: dungeon list");
                return;
            }
            if (found.size() > 1)
            {
                std::ostringstream out;
                for (TestHarness::ContentDef const* content : found)
                    out << (out.tellp() > 0 ? ", " : "") << content->name << " [" << content->key << "]";
                Say("Which one: " + out.str());
                return;
            }
            StartDungeon(*found.front(), minLevel, maxLevel);
        }

        void StartDungeon(TestHarness::ContentDef const& content, uint8 minLevel, uint8 maxLevel)
        {
            if (!Proven(content))
            {
                Say(std::string(content.name) + " not yet checked by test bots (series " + StreakText(content) + ")");
                return;
            }
            if (!LevelFits(content, minLevel, maxLevel))
            {
                Say(std::string(content.name) + ": group level " + std::to_string(minLevel) + "-" +
                    std::to_string(maxLevel) + ", fits from " +
                    std::to_string(std::max<int32>(1, int32(content.level) - int32(cfg.levelBelow))) + " to " +
                    std::to_string(content.level + cfg.levelAbove));
                return;
            }

            std::vector<Player*> bots = OnlineBots();
            if (bots.empty())
            {
                Say("Nobody of the group is in game");
                return;
            }

            CancelOrder("replaced by a new command", false);
            Order order;
            order.type = OrderType::Dungeon;
            order.title = std::string("dungeon ") + content.name;
            order.content = &content;
            for (Player* bot : bots)
                order.executors.insert(bot->GetGUID());
            BeginOrder(order);

            SelfTravel::PortalGoal goal;
            goal.mapId = content.mapId;
            for (uint32 trigger : content.entrances)
                if (AreaTriggerTeleport const* at = Entrance(trigger))
                {
                    goal.x = at->target_X;
                    goal.y = at->target_Y;
                    goal.z = at->target_Z;
                    goal.exact = true;
                    break;
                }

            for (Player* bot : bots)
            {
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                if (content.raid)
                    bot->SetRaidDifficulty(Difficulty(content.difficulty));
                else
                    bot->SetDungeonDifficulty(Difficulty(content.difficulty));
                SelfTravel::SetPortalGoal(bot->GetGUID(), goal);
                std::string change = "+instance portal,+breathe,-stay";
                if (_order.human || bot->GetGUID() == _leader)
                    change += ",-follow,-grind,-new rpg,-rpg";
                botAI->ChangeStrategy(change, BOT_STATE_NON_COMBAT);
                botAI->ChangeStrategy("+ghost portal", BOT_STATE_DEAD);
            }
            if (Group* group = bots.front()->GetGroup())
            {
                if (content.raid)
                    group->SetRaidDifficulty(Difficulty(content.difficulty));
                else
                    group->SetDungeonDifficulty(Difficulty(content.difficulty));
            }
            Say(std::string("Going to ") + content.name + " (test series " + StreakText(content) +
                "): on foot to the entrance and inside.");
        }

        // A taxi node of the bot's faction whose name contains the text; known ones first.
        uint32 FindTaxiNode(Player* bot, std::string const& lower, bool knownOnly)
        {
            uint8 const mountIndex = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0;
            uint32 best = 0;
            for (uint32 i = 1; i < sTaxiNodesStore.GetNumRows(); ++i)
            {
                TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
                if (!node || !node->MountCreatureID[mountIndex])
                    continue;
                bool match = false;
                for (uint8 l = 0; l < TOTAL_LOCALES && !match; ++l)
                    match = Contains(node->name[l], lower);
                if (!match)
                    continue;
                bool const known = bot->m_taxi.IsTaximaskNodeKnown(i);
                if (knownOnly && !known)
                    continue;
                if (known)
                    return i;
                if (!best)
                    best = i;
            }
            return best;
        }

        std::string KnownNodes(Player* bot)
        {
            std::ostringstream out;
            for (uint32 i = 1; i < sTaxiNodesStore.GetNumRows(); ++i)
            {
                TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
                if (node && node->map_id == bot->GetMapId() && bot->m_taxi.IsTaximaskNodeKnown(i))
                    out << (out.tellp() > 0 ? "; " : "") << DbcName(node->name);
            }
            return out.tellp() > 0 ? out.str() : std::string("none");
        }

        // Who carries out an order: every bot while the user plays (he does not walk with them), the bot leader
        // alone in autonomous play (the members follow it).
        std::vector<Player*> Executors(bool everyone)
        {
            std::vector<Player*> bots = OnlineBots();
            if (everyone || _mode == Mode::Human)
                return bots;
            std::vector<Player*> leaderOnly;
            for (Player* bot : bots)
                if (bot->GetGUID() == _leader)
                    leaderOnly.push_back(bot);
            return leaderOnly;
        }

        void CommandFly(std::string const& lower)
        {
            std::vector<Player*> executors = Executors(false);
            if (executors.empty())
            {
                Say("Nobody of the group is in game");
                return;
            }
            Player* first = executors.front();
            if (lower.empty() || lower == "?" || lower == "list")
            {
                Say(first->GetName() + " knows flight points: " + KnownNodes(first));
                return;
            }

            uint32 const node = FindTaxiNode(first, lower, false);
            if (!node)
            {
                Say("No such flight point. Known: " + KnownNodes(first));
                return;
            }
            TaxiNodesEntry const* nodeEntry = sTaxiNodesStore.LookupEntry(node);
            std::string const nodeName = DbcName(nodeEntry->name);

            CancelOrder("replaced by a new command", false);
            Order order;
            order.type = OrderType::Fly;
            order.title = "flight to " + nodeName;
            order.toNode = node;
            std::ostringstream refused;
            for (Player* bot : executors)
            {
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot);
                std::vector<uint32> path;
                if (master && master->taxiNodeId && bot->m_taxi.IsTaximaskNodeKnown(node))
                    path = sTravelNodeMap.FindKnownTaxiPath(bot, master->taxiNodeId, node);
                if (path.empty())
                {
                    refused << (refused.tellp() > 0 ? ", " : "") << bot->GetName()
                            << (bot->m_taxi.IsTaximaskNodeKnown(node) ? " (no route over known points)"
                                                                      : " (point not discovered)");
                    continue;
                }
                order.executors.insert(bot->GetGUID());
                order.flights[bot->GetGUID()] = {master->templateEntry, master->pos, path};
                botAI->ChangeStrategy("+" + RpgStrategy() + ",-follow,-stay,-grind", BOT_STATE_NON_COMBAT);
                botAI->rpgInfo.ChangeToTravelFlight(master->templateEntry, master->pos, path);
            }
            if (order.executors.empty())
            {
                Say("Cannot fly to " + nodeName + ": " + refused.str() + ". " + first->GetName() +
                    " knows: " + KnownNodes(first));
                return;
            }
            BeginOrder(order);
            Say("Flying to " + nodeName + ": walking to the nearest flight master." +
                (refused.tellp() > 0 ? " Will not fly: " + refused.str() : std::string()));
        }

        // A place: zone coordinates "x,y" of the leader's zone, a dungeon, a taxi node, a zone, an NPC name.
        bool ResolvePlace(Player* bot, std::string const& lower, WorldPosition& pos, std::string& title)
        {
            float x = 0.0f;
            float y = 0.0f;
            std::string numbers = lower;
            std::replace(numbers.begin(), numbers.end(), ',', ' ');
            std::istringstream coords(numbers);
            if ((coords >> x >> y) && coords.eof() && x >= 0.0f && x <= 100.0f && y >= 0.0f && y <= 100.0f)
            {
                uint32 const zone = bot->GetZoneId();
                Zone2MapCoordinates(x, y, zone);
                float z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, MAX_HEIGHT);
                if (z <= INVALID_HEIGHT)
                    z = bot->GetPositionZ();
                pos = WorldPosition(bot->GetMapId(), x, y, z);
                title = lower + " (" + ZoneName(zone) + ")";
                return true;
            }

            if (uint32 node = FindTaxiNode(bot, lower, false))
            {
                TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(node);
                pos = WorldPosition(entry->map_id, entry->x, entry->y, entry->z);
                title = DbcName(entry->name);
                return true;
            }

            // A zone (top-level area): its flight master of the bot's faction, else the middle of its map.
            for (uint32 i = 1; i < sAreaTableStore.GetNumRows(); ++i)
            {
                AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
                if (!area || area->zone)
                    continue;
                bool match = false;
                for (uint8 l = 0; l < TOTAL_LOCALES && !match; ++l)
                    match = area->area_name[l] && Lower(area->area_name[l]) == lower;
                if (!match)
                    continue;
                std::vector<uint32> nodes = sTravelMgr.GetFlightNodesInZone(area->ID, bot->GetTeamId());
                if (!nodes.empty())
                    if (TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(nodes.front()))
                    {
                        pos = WorldPosition(entry->map_id, entry->x, entry->y, entry->z);
                        title = std::string(DbcName(area->area_name)) + " (" + DbcName(entry->name) + ")";
                        return true;
                    }
                float cx = 50.0f;
                float cy = 50.0f;
                Zone2MapCoordinates(cx, cy, area->ID);
                pos = WorldPosition(area->mapid, cx, cy, 0.0f);
                if (area->mapid == bot->GetMapId())
                {
                    float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), cx, cy, MAX_HEIGHT);
                    if (z > INVALID_HEIGHT)
                        pos = WorldPosition(area->mapid, cx, cy, z);
                }
                title = std::string(DbcName(area->area_name)) + " (middle of the zone)";
                return true;
            }

            // An NPC by name: its nearest spawn on this map, else anywhere.
            if (lower.size() >= 4)
            {
                uint32 bestEntry = 0;
                bool bestExact = false;
                for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
                {
                    bool exact = Lower(creature.Name) == lower;
                    bool match = exact || Contains(creature.Name, lower);
                    if (!match)
                        if (CreatureLocale const* locale = sObjectMgr->GetCreatureLocale(entry))
                            for (std::string const& name : locale->Name)
                                if (Contains(name, lower))
                                {
                                    match = true;
                                    exact = exact || Lower(name) == lower;
                                }
                    if (!match || (bestEntry && (bestExact || !exact)))
                        continue;
                    if (sTravelRouteMgr.GetSpawns(entry).empty())
                        continue;
                    bestEntry = entry;
                    bestExact = exact;
                }
                if (bestEntry)
                {
                    WorldPosition here(bot);
                    float best = -1.0f;
                    for (WorldPosition const& spawn : sTravelRouteMgr.GetSpawns(bestEntry))
                    {
                        float const dist = spawn.GetMapId() == bot->GetMapId() ? here.distance(spawn) : 1e9f;
                        if (best < 0.0f || dist < best)
                        {
                            best = dist;
                            pos = spawn;
                        }
                    }
                    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(bestEntry);
                    title = creature ? creature->Name : std::to_string(bestEntry);
                    return true;
                }
            }
            return false;
        }

        void CommandGo(std::string const& text, std::string const& lower)
        {
            std::vector<Player*> executors = Executors(false);
            if (executors.empty())
            {
                Say("Nobody of the group is in game");
                return;
            }
            if (lower.empty())
            {
                Say("Where? goto <x>,<y> | goto <zone/town/flight point/NPC name> | dungeon <name>");
                return;
            }

            // A dungeon name goes through the dungeon gate.
            std::vector<TestHarness::ContentDef const*> dungeons = FindDungeons(lower);
            if (dungeons.size() == 1 && !dungeons.front()->raid)
            {
                uint8 minLevel = 0;
                uint8 maxLevel = 0;
                PackLevels(minLevel, maxLevel);
                StartDungeon(*dungeons.front(), minLevel, maxLevel);
                return;
            }

            WorldPosition pos;
            std::string title;
            if (!ResolvePlace(executors.front(), lower, pos, title))
            {
                Say("Place not found \"" + text +
                    "\". Possible: goto 52,41 (zone map coordinates), goto <zone>, "
                    "goto <flight point>, goto <NPC name>");
                return;
            }

            CancelOrder("replaced by a new command", false);
            Order order;
            order.type = OrderType::Go;
            order.title = "walking to " + title;
            order.pos = pos;
            for (Player* bot : executors)
            {
                order.executors.insert(bot->GetGUID());
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                botAI->ChangeStrategy("+" + RpgStrategy() + ",-follow,-stay,-grind", BOT_STATE_NON_COMBAT);
                botAI->rpgInfo.ChangeToGoCamp(pos);
            }
            BeginOrder(order);
            std::string const where =
                pos.GetMapId() == executors.front()->GetMapId() ? "" : " (another continent: ship/zeppelin)";
            Say("Walking to " + title + where + ", on foot.");
        }

        // The class trainer of the bot's faction nearest to it on its map.
        bool FindTrainer(Player* bot, uint32& entryOut, WorldPosition& posOut)
        {
            FactionTemplateEntry const* mine = bot->GetFactionTemplateEntry();
            WorldPosition here(bot);
            float best = -1.0f;
            for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
            {
                if (!(creature.npcflag & UNIT_NPC_FLAG_TRAINER))
                    continue;
                Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
                if (!trainer || trainer->GetTrainerType() != Trainer::Type::Class ||
                    !trainer->IsTrainerValidForPlayer(bot))
                    continue;
                FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creature.faction);
                if (!faction || !mine || faction->IsHostileTo(*mine))
                    continue;
                for (WorldPosition const& spawn : sTravelRouteMgr.GetSpawns(entry))
                {
                    if (spawn.GetMapId() != bot->GetMapId())
                        continue;
                    float const dist = here.distance(spawn);
                    if (best < 0.0f || dist < best)
                    {
                        best = dist;
                        entryOut = entry;
                        posOut = spawn;
                    }
                }
            }
            return best >= 0.0f;
        }

        // Every bot walks to its class trainer and learns what it can pay for (TrainerAction "learn", the money
        // goes to the trainer); then the pack plays on.
        void CommandTrain()
        {
            std::vector<Player*> bots = Executors(true);
            if (bots.empty())
            {
                Say("Nobody of the group is in game");
                return;
            }
            CancelOrder("replaced by a new command", false);
            Order order;
            order.type = OrderType::Train;
            order.title = "to the trainers";
            std::ostringstream missing;
            for (Player* bot : bots)
            {
                uint32 entry = 0;
                WorldPosition pos;
                if (!FindTrainer(bot, entry, pos))
                {
                    missing << (missing.tellp() > 0 ? ", " : "") << bot->GetName();
                    continue;
                }
                order.executors.insert(bot->GetGUID());
                order.trainers[bot->GetGUID()] = entry;
                order.trainerPos[bot->GetGUID()] = pos;
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                botAI->ChangeStrategy("+" + RpgStrategy() + ",-follow,-stay,-grind", BOT_STATE_NON_COMBAT);
                botAI->rpgInfo.ChangeToGoCamp(pos);
            }
            if (order.executors.empty())
            {
                Say("No class trainers found on this continent");
                return;
            }
            BeginOrder(order);
            Say("Walking to the class trainers to learn new spells (own money)." +
                (missing.tellp() > 0 ? " No trainer nearby: " + missing.str() : std::string()));
        }

        void MaybeAutoTrain(std::vector<Player*> const& bots)
        {
            if (!cfg.autoTrainEvery || _order.type != OrderType::None ||
                (!cfg.solo && bots.size() < cfg.guids.size()))
                return;
            uint8 minLevel = 255;
            for (Player* bot : bots)
            {
                MapEntry const* map = sMapStore.LookupEntry(bot->GetMapId());
                // solo: the pack does not wait for all of them; a bot busy now walks after its fight
                if (!cfg.solo && (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || (map && map->IsDungeon())))
                    return;
                minLevel = std::min(minLevel, bot->GetLevel());
            }
            if (!_trainedLevel)
            {
                _trainedLevel = minLevel;  // nothing new since the last start is assumed
                return;
            }
            if (minLevel < _trainedLevel + cfg.autoTrainEvery)
                return;
            _trainedLevel = minLevel;
            LOG_INFO("playerbots", "User pack: level {} reached, visiting the class trainers", minLevel);
            CommandTrain();
        }

        void BeginOrder(Order& order)
        {
            order.human = _mode == Mode::Human;
            order.startMs = getMSTime();
            _order = order;
        }

        void RestoreAfterOrder(Player* bot, bool arrived)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return;
            SelfTravel::ClearPortalGoal(bot->GetGUID());
            if (_order.human && _mode == Mode::Human)
            {
                // While the user plays, the bots wait where they arrived until he says "follow".
                botAI->ChangeStrategy("-new rpg,-rpg,-instance portal," + std::string(arrived ? "+stay" : "+follow"),
                                      BOT_STATE_NON_COMBAT);
                botAI->rpgInfo.ChangeToIdle();
                return;
            }
            botAI->ChangeStrategy("-instance portal", BOT_STATE_NON_COMBAT);
            if (bot->GetGUID() != _leader)
                botAI->rpgInfo.ChangeToIdle();
            // Autonomous: EnsureAutonomousStrategies puts the usual play back on the next tick.
        }

        void CancelOrder(std::string const& reason, bool say = true)
        {
            if (_order.type == OrderType::None)
            {
                if (say)
                    Say("No active command");
                return;
            }
            for (ObjectGuid const& guid : _order.executors)
                if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid))
                    RestoreAfterOrder(bot, false);
            if (say)
                Say(_order.title + ": " + reason);
            _order = Order();
        }

        void TickOrder(uint32 /*now*/)
        {
            if (_order.type == OrderType::None)
                return;
            // [timers] no order timeout: the user's command stays until it is done, replaced, cancelled or has no
            // executor left (leader commands are final)
            // The user logged in or out while the order ran: it stops, the mode's own play takes over.
            if (_order.human != (_mode == Mode::Human))
            {
                _order = Order();
                return;
            }

            std::ostringstream report;
            for (auto itr = _order.executors.begin(); itr != _order.executors.end();)
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(*itr);
                PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
                if (!bot || !botAI || IsHuman(bot))
                {
                    itr = _order.executors.erase(itr);
                    continue;
                }
                if (bot->IsBeingTeleported() || !bot->IsInWorld())
                {
                    ++itr;
                    continue;
                }

                bool done = false;
                bool arrived = false;
                NewRpgStatus status = botAI->rpgInfo.GetStatus();
                if (!_order.reissued.count(*itr))
                {
                    _order.reissued.insert(*itr);
                    if (_order.type == OrderType::Go && status != RPG_GO_CAMP)
                        botAI->rpgInfo.ChangeToGoCamp(_order.pos);
                    else if (_order.type == OrderType::Train && status != RPG_GO_CAMP)
                        botAI->rpgInfo.ChangeToGoCamp(_order.trainerPos[*itr]);
                    else if (_order.type == OrderType::Fly && status != RPG_TRAVEL_FLIGHT && !bot->IsInFlight())
                    {
                        Order::Flight const& flight = _order.flights[*itr];
                        botAI->rpgInfo.ChangeToTravelFlight(flight.masterEntry, flight.masterPos, flight.path);
                    }
                    status = botAI->rpgInfo.GetStatus();
                }
                switch (_order.type)
                {
                    case OrderType::Go:
                        arrived = bot->GetMapId() == _order.pos.GetMapId() &&
                                  bot->GetExactDist(_order.pos) < GO_ARRIVED_RANGE;
                        done = arrived || status != RPG_GO_CAMP;
                        break;
                    case OrderType::Fly:
                        done = status != RPG_TRAVEL_FLIGHT && !bot->IsInFlight();
                        arrived = done;
                        break;
                    case OrderType::Dungeon:
                        arrived = _order.content && bot->GetMapId() == _order.content->mapId;
                        done = arrived;
                        break;
                    case OrderType::Train:
                    {
                        WorldPosition const& pos = _order.trainerPos[*itr];
                        bool const atTrainer = bot->GetMapId() == pos.GetMapId() && bot->GetExactDist(pos) < 20.0f;
                        if (!atTrainer && status == RPG_GO_CAMP)
                            break;
                        done = true;
                        Creature* trainer = bot->FindNearestCreature(_order.trainers[*itr], 30.0f);
                        if (!trainer)
                            break;
                        if (bot->GetDistance(trainer) > TRAINER_REACH)
                        {
                            done = false;
                            if (!WalkBotRoute(bot, trainer->GetPositionX(), trainer->GetPositionY(),
                                              trainer->GetPositionZ()))  // [lead109] the leader's route
                                bot->GetMotionMaster()->MovePoint(0, trainer->GetPositionX(),
                                                                  trainer->GetPositionY(), trainer->GetPositionZ());
                            break;
                        }
                        arrived = true;
                        // A bot with a master learns from the unit its master has selected (TrainerAction).
                        if (botAI->GetMaster())
                        {
                            report << (report.tellp() > 0 ? ", " : "") << bot->GetName() << " at " << trainer->GetName()
                                   << ": target the trainer and say /w " << bot->GetName() << " trainer learn";
                            break;
                        }
                        bot->SetSelection(trainer->GetGUID());
                        uint32 const money = bot->GetMoney();
                        botAI->DoSpecificAction("trainer", Event("pack", "learn"), true);
                        report << (report.tellp() > 0 ? ", " : "") << bot->GetName() << " "
                               << (money - bot->GetMoney()) / 100 << "s";
                        break;
                    }
                    default:
                        done = true;
                        break;
                }

                if (!done)
                {
                    ++itr;
                    continue;
                }
                if (!arrived)
                    report << (report.tellp() > 0 ? ", " : "") << bot->GetName() << " did not arrive";
                RestoreAfterOrder(bot, arrived);
                itr = _order.executors.erase(itr);
            }

            if (report.tellp() > 0)
                Say(_order.title + ": " + report.str());
            if (_order.executors.empty())
            {
                Say(_order.title + ": done." +
                    (_order.human ? std::string(" Waiting for you; say follow to have them follow you.")
                                  : std::string()));
                _order = Order();
            }
        }

        // ---- status ---------------------------------------------------------------------------------------------

        std::string StatusLine(ObjectGuid guid)
        {
            std::ostringstream out;
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            uint32 deaths = 0;
            {
                std::lock_guard<std::mutex> guard(deathLock);
                if (deathDay == LocalDay())
                {
                    auto const itr = deathsToday.find(guid.GetCounter());
                    deaths = itr == deathsToday.end() ? 0 : itr->second;
                }
            }
            if (!player)
            {
                std::string name = guid.ToString();
                uint32 level = 0;
                if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid))
                {
                    name = entry->Name;
                    level = entry->Level;
                }
                out << name << " (" << guid.GetCounter() << ") L" << level << " | offline | deaths today " << deaths;
                return out.str();
            }

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
            out << player->GetName() << " (" << guid.GetCounter() << ") L" << uint32(player->GetLevel()) << " xp "
                << player->GetUInt32Value(PLAYER_XP) << "/" << player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
            if (player->HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN))
                out << " (XP OFF)";

            out << " | ";
            if (IsHuman(player))
                out << "USER PLAYS";
            else if (!botAI)
                out << "no AI";
            else if (Player* master = botAI->GetMaster())
                out << "bot of " << master->GetName();
            else
                out << (guid == _leader ? "bot leader" : "bot");

            out << " | " << MapName(player->GetMapId()) << " / " << ZoneName(player->GetZoneId()) << " / "
                << ZoneName(player->GetAreaId()) << " (" << player->GetMapId() << ": " << std::fixed
                << std::setprecision(1) << player->GetPositionX() << ", " << player->GetPositionY() << ", "
                << player->GetPositionZ() << ")";

            out << " | ";
            if (!player->IsAlive())
                out << (player->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? "GHOST" : "DEAD");
            else if (player->IsInFlight())
                out << "FLYING";
            else if (player->IsInCombat())
                out << "COMBAT";
            else if (botAI && botAI->HasStrategy(RpgStrategy(), BOT_STATE_NON_COMBAT))
            {
                out << "rpg " << RpgStatusName(botAI->rpgInfo.GetStatus());
                if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
                    out << " q" << quest->questId;
            }
            else
                out << "idle";

            if (botAI)
            {
                std::ostringstream strategies;
                for (char const* name : {"new rpg", "rpg", "grind", "follow", "stay", "instance portal", "passive"})
                    if (botAI->HasStrategy(name, BOT_STATE_NON_COMBAT))
                        strategies << (strategies.tellp() > 0 ? "," : "") << name;
                out << " [" << strategies.str() << "]";
            }
            if (_order.type != OrderType::None && _order.executors.count(guid))
                out << " order: " << _order.title;

            Unit* target = player->GetVictim();
            if (!target)
                target = player->GetSelectedUnit();
            out << " | target " << (target ? target->GetName() : std::string("-"));

            uint32 quests = 0;
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (player->GetQuestSlotQuestId(slot))
                    ++quests;
            uint32 const money = player->GetMoney();
            out << " | quests " << quests << " | " << money / GOLD << "g " << (money % GOLD) / SILVER << "s "
                << money % SILVER << "c | deaths today " << deaths;
            return out.str();
        }

        void WriteStatusLog()
        {
            if (cfg.statusLog.empty())
                return;
            std::ofstream file(cfg.statusLog, std::ios::app);
            if (!file)
            {
                LOG_ERROR("playerbots", "User pack: cannot write {}", cfg.statusLog);
                return;
            }
            std::string const stamp = WallClock();
            for (std::string const& line : Status())
                file << stamp << " " << line << "\n";
        }

        Mode _mode = Mode::Off;
        uint32 _timer = 0;
        uint32 _startMs = 0;
        uint32 _streaksMs = 0;
        uint32 _nextStatusLogMs = 0;
        uint8 _trainedLevel = 0;
        ObjectGuid _leader;
        ObjectGuid _humanGuid;
        std::unordered_map<ObjectGuid, uint32> _lastLogin;
        Order _order;
        std::vector<std::string>* _console = nullptr;
    };

    bool IsPackLow(uint32 low) { return cfg.enabled && cfg.lows.count(low); }

    // A pack order said by the user (party chat, or a whisper to a pack bot) from a pack character.
    bool LooksLikePackCommand(std::string const& msg)
    {
        std::string const lower = Lower(Trim(msg));
        static std::vector<std::string> const prefixes = {
            "pack", "dungeon", "fly", "goto", "\xD0\xBF\xD0\xB0\xD0\xBA", "\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB6",
            "\xD0\xBB\xD0\xB5\xD1\x82\xD0\xB8", "\xD0\xB8\xD0\xB4\xD0\xB8",
            "\xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0"};
        for (std::string const& prefix : prefixes)
            if (lower.compare(0, prefix.size(), prefix) == 0 &&
                (lower.size() == prefix.size() || lower[prefix.size()] == ' '))
                return true;
        return false;
    }

    // Bot commands that hand out items, gold, spells or teleports for free: never for the pack.
    bool IsCheatCommand(std::string const& msg)
    {
        std::string const lower = Lower(Trim(msg));
        static std::vector<std::string> const cheats = {"maintenance", "autogear", "cheat", "summon"};
        for (std::string const& cheat : cheats)
            if (lower.compare(0, cheat.size(), cheat) == 0)
                return true;
        return false;
    }
}

namespace UserPack
{
    bool IsPackCharacter(Player* player) { return player && IsPackLow(player->GetGUID().GetCounter()); }

    bool IsAutonomousLeader(Player* bot)
    {
        if (!bot || !IsPackLow(bot->GetGUID().GetCounter()))
            return false;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || botAI->HasGameClientMaster())
            return false;
        Group* group = bot->GetGroup();
        return group && group->GetLeaderGUID() == bot->GetGUID();
    }

    std::vector<std::string> StatusLines()
    {
        if (!cfg.enabled)
            return {"User pack is off (Playerbots.UserPack.Enabled = 0)"};
        return UserPackMgr::Instance().Status();
    }

    std::vector<std::string> ConsoleCommand(std::string const& text) { return UserPackMgr::Instance().Console(text); }
}

class PlayerbotsUserPackWorldScript : public WorldScript
{
public:
    PlayerbotsUserPackWorldScript()
        : WorldScript("PlayerbotsUserPackWorldScript",
                      {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE})
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LoadConfig();
    }

    void OnStartup() override
    {
        LoadConfig();
        UserPackMgr::Instance().Start();
    }

    void OnUpdate(uint32 diff) override { UserPackMgr::Instance().Update(diff); }
};

class PlayerbotsUserPackPlayerScript : public PlayerScript
{
public:
    PlayerbotsUserPackPlayerScript()
        : PlayerScript("PlayerbotsUserPackPlayerScript",
                       {PLAYERHOOK_ON_GIVE_EXP, PLAYERHOOK_ON_BEFORE_TELEPORT, PLAYERHOOK_CAN_JOIN_LFG,
                        PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
                        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT})
    {
    }

    // Kill, quest, exploration XP of the pack, also while the user plays it (the rate belongs to the character).
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (!player || !IsPackLow(player->GetGUID().GetCounter()) || cfg.xpRate == 1.0f)
            return;
        amount = uint32(std::lround(double(amount) * cfg.xpRate));
    }

    // Dungeon gate: a pack character the AI plays never goes into an instance the test bots have not proven.
    // Default deny: no test content for that map and difficulty is no proof either. The user himself is not held.
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float x, float y, float z, float /*orientation*/,
                                uint32 /*options*/, Unit* /*target*/) override
    {
        if (!player || !IsPackLow(player->GetGUID().GetCounter()) || IsHuman(player) ||
            mapId == player->GetMapId())
            return true;
        MapEntry const* map = sMapStore.LookupEntry(mapId);
        if (!map || !map->IsDungeon())
            return true;

        Difficulty difficulty = player->GetDifficulty(map->IsRaid());
        if (Group* group = player->GetGroup())
            difficulty = group->GetDifficulty(map->IsRaid());
        std::string why;
        if (InstanceAllowed(mapId, uint8(difficulty), x, y, z, why))
            return true;
        LOG_INFO("playerbots", "User pack: {} may not enter {} ({}): {}", player->GetName(), MapName(mapId), mapId,
                 why);
        return false;
    }

    bool OnPlayerCanJoinLfg(Player* player, uint8 /*roles*/, std::set<uint32>& dungeons,
                            std::string const& /*comment*/) override
    {
        if (!player || !IsPackLow(player->GetGUID().GetCounter()) || IsHuman(player))
            return true;
        // [lfg25] a pack bot queues the Dungeon Finder for the dungeons it may enter (the same proven-content rule
        // as its walk in, OnPlayerBeforeTeleport above), not never: b373 the global leader's five 1 for Ragefire
        // Chasm (proven) was refused by the finder ("the dungeon finder did not take the five"), then disbanded
        if (dungeons.empty())
            return false;
        for (uint32 id : dungeons)
        {
            LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(id & 0x00FFFFFF);
            std::string why;
            if (!dungeon || !InstanceAllowed(dungeon->MapID, uint8(dungeon->Difficulty), 0.0f, 0.0f, 0.0f, why))
            {
                LOG_INFO("playerbots", "[lfg25] User pack: {} may not queue the Dungeon Finder for dungeon {}: {}",
                         player->GetName(), id, dungeon ? why : std::string("no such dungeon"));
                return false;
            }
        }
        LOG_INFO("playerbots", "[lfg25] User pack: {} queues the Dungeon Finder ({} dungeon(s), all proven)",
                 player->GetName(), dungeons.size());
        return true;
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!player || !IsPackLow(player->GetGUID().GetCounter()))
            return;
        std::lock_guard<std::mutex> guard(deathLock);
        int const today = LocalDay();
        if (deathDay != today)
        {
            deathsToday.clear();
            deathDay = today;
        }
        ++deathsToday[player->GetGUID().GetCounter()];
    }

    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || lang == LANG_ADDON || !receiver ||
            !IsPackLow(receiver->GetGUID().GetCounter()) || IsHuman(receiver))
            return true;
        return Filter(player, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Group* group) override
    {
        if (lang == LANG_ADDON || !group)
            return true;
        bool packGroup = false;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
            if (IsPackLow(slot.guid.GetCounter()))
                packGroup = true;
        return !packGroup || Filter(player, msg);
    }

private:
    // Runs before PlayerbotsPlayerScript (registered first): false drops a cheat command before any bot sees it.
    static bool Filter(Player* player, std::string const& msg)
    {
        if (IsCheatCommand(msg))
        {
            if (player && player->GetSession())
                ChatHandler(player->GetSession())
                    .SendSysMessage("The pack plays honestly: this command (free items/spells/teleport) is disabled.");
            return false;
        }
        if (player && IsHuman(player) && IsPackLow(player->GetGUID().GetCounter()) && LooksLikePackCommand(msg))
            UserPackMgr::QueueCommand(player->GetGUID(), msg);
        return true;
    }
};

void AddPlayerbotsUserPack()
{
    new PlayerbotsUserPackWorldScript();
    new PlayerbotsUserPackPlayerScript();
}
