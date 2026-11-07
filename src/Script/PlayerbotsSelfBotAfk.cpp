/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotControl.h"
#include "GuildCoordinator.h"

#include "Bag.h"
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "ObjectMgr.h"
#include "PlayerbotsDatabase.h"
#include "NewRpgProfession.h"
#include "QuestDef.h"
#include "Chat.h"
#include "Group.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PlayerbotOperation.h"
#include "PlayerbotRepository.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr uint32 IDLE_RETURN_CHECK_MS = 1000;
    constexpr size_t ADDON_LINE_MAX = 240;  // the client drops addon messages longer than 254 with the prefix

    // Shared by the map threads (client packets) and the world thread (activation, removal).
    struct ControlState
    {
        std::mutex lock;
        std::unordered_map<ObjectGuid, uint32> lastInputMs;
        std::unordered_map<ObjectGuid, uint32> activatedMs;
        std::unordered_map<ObjectGuid, SelfBotControl::Reason> offReason;
        std::unordered_set<ObjectGuid> suspended;
        std::unordered_set<ObjectGuid> takenByInput;
        std::atomic<uint32> suspendedCount{0};
    };

    ControlState& State()
    {
        static ControlState state;
        return state;
    }

    uint32 NowMs() { return std::max(getMSTime(), 1u); }

    void ClearSuspended(ObjectGuid guid)
    {
        ControlState& state = State();
        std::lock_guard<std::mutex> guard(state.lock);
        if (state.suspended.erase(guid))
            state.suspendedCount.fetch_sub(1, std::memory_order_relaxed);
    }

    void Tell(Player* player, std::vector<std::string> const& lines)
    {
        if (!player || !player->GetSession())
            return;

        ChatHandler handler(player->GetSession());
        for (std::string const& line : lines)
            handler.SendSysMessage(line);
    }

    // Movement keys, jump, spell cast, target change: what only a person at the client sends. Stops, heartbeats
    // and acknowledgements are left out, the client also sends those for movement the server started.
    bool IsManualInputOpcode(uint16 opcode)
    {
        switch (opcode)
        {
            case MSG_MOVE_START_FORWARD:
            case MSG_MOVE_START_BACKWARD:
            case MSG_MOVE_START_STRAFE_LEFT:
            case MSG_MOVE_START_STRAFE_RIGHT:
            case MSG_MOVE_START_TURN_LEFT:
            case MSG_MOVE_START_TURN_RIGHT:
            case MSG_MOVE_JUMP:
            case CMSG_CAST_SPELL:
            case CMSG_SET_SELECTION:
                return true;
            default:
                return false;
        }
    }

    bool IsMovementOpcode(uint16 opcode) { return opcode != CMSG_CAST_SPELL && opcode != CMSG_SET_SELECTION; }

    // The AI's own server-driven movement (a move spline) would make the core reject the client's movement.
    void StopAiMovement(Player* player)
    {
        if (player->IsInFlight())
            return;

        player->GetMotionMaster()->Clear();
        player->StopMoving();
    }

    // Removing the SelfBot AI touches the global AI map: done on the world thread.
    class SelfBotTakeControlOperation : public PlayerbotOperation
    {
    public:
        explicit SelfBotTakeControlOperation(ObjectGuid guid) : _guid(guid) {}

        bool Execute() override
        {
            Player* player = ObjectAccessor::FindPlayer(_guid);
            bool done = false;
            if (player && IsSelfBot(player))
            {
                Tell(player, SelfBotControl::Deactivate(player, SelfBotControl::Reason::ManualInput));
                SelfBotControl::SendAddonStatus(player);
                done = true;
            }

            ClearSuspended(_guid);
            return done;
        }

        ObjectGuid GetBotGuid() const override { return _guid; }
        uint32 GetPriority() const override { return 50; }
        std::string GetName() const override { return "SelfBotTakeControl"; }

    private:
        ObjectGuid _guid;
    };

    char const* ReasonText(SelfBotControl::Reason reason)
    {
        switch (reason)
        {
            case SelfBotControl::Reason::Command:
                return "command";
            case SelfBotControl::Reason::ManualInput:
                return "input";
            case SelfBotControl::Reason::IdleReturn:
                return "idle";
            default:
                return "-";
        }
    }

    char const* BotMode(PlayerbotAI* botAI)
    {
        if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
            return "stay";
        if (botAI->HasStrategy("passive", BOT_STATE_NON_COMBAT))
            return "flee";
        if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            return "follow";
        return "free";
    }

    void SendAddonLine(Player* player, std::string const& line)
    {
        std::string text = std::string(SelfBotControl::ADDON_PREFIX) + "\t" + line.substr(0, ADDON_LINE_MAX);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }
}

namespace SelfBotControl
{
    std::vector<std::string> Activate(Player* player, Reason reason)
    {
        if (!player)
            return {};

        if (GET_PLAYERBOT_AI(player))
            return {"SelfBot is already active."};

        if (sPlayerbotAIConfig.SelfBotLevel == 0)
            return {"SelfBot is disabled server-wide."};

        if (sPlayerbotAIConfig.SelfBotLevel == 1 && !player->CanBeGameMaster())
            return {"SelfBot is restricted for this account."};

        PlayerbotsMgr::instance().AddPlayerbotData(player, true);
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI)
            return {"SelfBot could not be started."};

        botAI->SetMaster(player);
        PlayerbotRepository::instance().Load(botAI);

        ControlState& state = State();
        {
            std::lock_guard<std::mutex> guard(state.lock);
            state.activatedMs[player->GetGUID()] = NowMs();
            state.offReason.erase(player->GetGUID());
            state.takenByInput.erase(player->GetGUID());
        }

        if (reason == Reason::IdleReturn)
            return {"SelfBot is now active (client idle)."};

        return {"SelfBot is now active."};
    }

    std::vector<std::string> Deactivate(Player* player, Reason reason)
    {
        if (!player)
            return {};

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI)
            return {"SelfBot is already inactive."};

        delete botAI;

        if (player->isTaxiCheater())
            player->SetTaxiCheater(false);

        StopAiMovement(player);

        ControlState& state = State();
        {
            std::lock_guard<std::mutex> guard(state.lock);
            state.offReason[player->GetGUID()] = reason;
            if (reason == Reason::ManualInput)
                state.takenByInput.insert(player->GetGUID());
            else
                state.takenByInput.erase(player->GetGUID());
        }

        if (reason == Reason::ManualInput)
            return {"SelfBot is now deactivated (manual input). \".playerbots bot self on\" gives control back."};

        return {"SelfBot is now deactivated."};
    }

    std::string StatusLine(Player* player)
    {
        if (!player)
            return "";

        if (IsSelfBot(player))
            return "SelfBot status: active";

        Reason reason = Reason::None;
        {
            ControlState& state = State();
            std::lock_guard<std::mutex> guard(state.lock);
            auto const itr = state.offReason.find(player->GetGUID());
            if (itr != state.offReason.end())
                reason = itr->second;
        }

        if (reason == Reason::ManualInput)
            return "SelfBot status: inactive (manual input)";

        return "SelfBot status: inactive";
    }

    std::vector<std::string> HandleCommand(Player* player, std::string const& mode)
    {
        if (!player)
            return {};

        std::string lower = mode;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

        std::vector<std::string> lines;
        if (lower.empty())
            lines = GET_PLAYERBOT_AI(player) ? Deactivate(player, Reason::Command) : Activate(player, Reason::Command);
        else if (lower == "on")
            lines = Activate(player, Reason::Command);
        else if (lower == "off")
            lines = Deactivate(player, Reason::Command);
        else if (lower == "status" || lower == "?")
            lines = {StatusLine(player)};
        else
            return {"usage: .playerbots bot self [on|off|status]"};

        SendAddonStatus(player);
        return lines;
    }

    void OnClientPacket(WorldSession* session, WorldPacket const& packet)
    {
        uint16 const opcode = packet.GetOpcode();
        if (!IsManualInputOpcode(opcode) || !session || session->IsHeadless())
            return;

        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || player->IsInFlight())
            return;

        if (opcode == CMSG_SET_SELECTION)
        {
            // The same target again (or none) is no decision of the person.
            if (packet.size() < sizeof(uint64))
                return;

            WorldPacket copy(packet);
            copy.rpos(0);
            ObjectGuid selection;
            copy >> selection;
            if (!selection || selection == player->GetGuidValue(UNIT_FIELD_TARGET))
                return;
        }

        uint32 const now = NowMs();
        ObjectGuid const guid = player->GetGUID();
        bool takeControl = false;
        {
            ControlState& state = State();
            std::lock_guard<std::mutex> guard(state.lock);
            state.lastInputMs[guid] = now;

            if (sPlayerbotAIConfig.SelfBotInputTakesControl && IsSelfBot(player) && !state.suspended.count(guid))
            {
                auto const activated = state.activatedMs.find(guid);
                bool const inGrace = activated != state.activatedMs.end() &&
                                     getMSTimeDiff(activated->second, now) < sPlayerbotAIConfig.SelfBotInputGraceMs;
                if (!inGrace)
                {
                    state.suspended.insert(guid);
                    state.suspendedCount.fetch_add(1, std::memory_order_relaxed);
                    takeControl = true;
                }
            }
        }

        if (!takeControl)
            return;

        LOG_DEBUG("playerbots", "SelfBot {}: client input (opcode {}) takes control", player->GetName(), opcode);

        // This very packet must be accepted: the AI's move spline would make the core drop it.
        if (IsMovementOpcode(opcode))
            StopAiMovement(player);

        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<SelfBotTakeControlOperation>(guid));
    }

    uint32 GetLastInputMs(ObjectGuid guid)
    {
        ControlState& state = State();
        std::lock_guard<std::mutex> guard(state.lock);
        auto const itr = state.lastInputMs.find(guid);
        return itr == state.lastInputMs.end() ? 0 : itr->second;
    }

    bool IsSuspended(Player* player)
    {
        ControlState& state = State();
        if (!player || !state.suspendedCount.load(std::memory_order_relaxed))
            return false;

        std::lock_guard<std::mutex> guard(state.lock);
        return state.suspended.count(player->GetGUID()) != 0;
    }

    void UpdateWorld(uint32 diff)
    {
        static uint32 sinceCheck = 0;
        sinceCheck += diff;
        if (sinceCheck < IDLE_RETURN_CHECK_MS)
            return;

        sinceCheck = 0;
        uint32 const idleMs = sPlayerbotAIConfig.SelfBotReturnAfterIdleSecs * IN_MILLISECONDS;
        if (!idleMs)
            return;

        std::vector<std::pair<ObjectGuid, uint32>> candidates;
        {
            ControlState& state = State();
            std::lock_guard<std::mutex> guard(state.lock);
            for (ObjectGuid const& guid : state.takenByInput)
            {
                auto const itr = state.lastInputMs.find(guid);
                candidates.emplace_back(guid, itr == state.lastInputMs.end() ? 0 : itr->second);
            }
        }

        uint32 const now = NowMs();
        for (auto const& [guid, lastInput] : candidates)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player || GET_PLAYERBOT_AI(player))
            {
                ControlState& state = State();
                std::lock_guard<std::mutex> guard(state.lock);
                state.takenByInput.erase(guid);
                continue;
            }

            if (lastInput && getMSTimeDiff(lastInput, now) < idleMs)
                continue;

            Tell(player, Activate(player, Reason::IdleReturn));
            SendAddonStatus(player);
        }
    }

    void SendAddonStatus(Player* player)
    {
        if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
            return;

        uint32 const now = NowMs();
        Reason reason = Reason::None;
        uint32 lastInput = 0;
        {
            ControlState& state = State();
            std::lock_guard<std::mutex> guard(state.lock);
            auto const reasonItr = state.offReason.find(player->GetGUID());
            if (reasonItr != state.offReason.end())
                reason = reasonItr->second;

            auto const inputItr = state.lastInputMs.find(player->GetGUID());
            if (inputItr != state.lastInputMs.end())
                lastInput = inputItr->second;
        }

        Group* group = player->GetGroup();
        Player* leader = group ? ObjectAccessor::FindPlayer(group->GetLeaderGUID()) : nullptr;
        char const* leaderKind = "-";
        if (leader)
            leaderKind = !GET_PLAYERBOT_AI(leader) ? "person" : (IsSelfBot(leader) ? "selfbot" : "bot");

        Player* coordinator = RaidLeader::FindCoordinator(player);
        ObjectGuid pausedBy;
        uint32 const pausedMs = RaidLeader::GetPausedMs(player, &pausedBy);
        Player* pauser = pausedBy ? ObjectAccessor::FindPlayer(pausedBy) : nullptr;

        std::ostringstream status;
        status << "S;v=1;sb=" << (IsSelfBot(player) ? 1 : 0) << ";why=" << ReasonText(reason)
               << ";ld=" << (leader ? leader->GetName() : "-") << ";lk=" << leaderKind
               << ";co=" << (coordinator ? coordinator->GetName() : "-") << ";pz=" << (pausedMs ? 1 : 0)
               << ";pt=" << pausedMs / IN_MILLISECONDS << ";pb=" << (pauser ? pauser->GetName() : "-")
               << ";idle=" << (lastInput ? int32(getMSTimeDiff(lastInput, now) / IN_MILLISECONDS) : -1);
        SendAddonLine(player, status.str());

        uint32 bots = 0;
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player || !member->IsInWorld())
                    continue;

                PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
                if (!memberAI)
                    continue;

                std::ostringstream line;
                line << "B;n=" << member->GetName() << ";m=" << BotMode(memberAI)
                     << ";dead=" << (member->IsAlive() ? 0 : 1) << ";fight=" << (member->IsInCombat() ? 1 : 0);
                SendAddonLine(player, line.str());
                ++bots;
            }
        }

        SendAddonLine(player, "E;n=" + std::to_string(bots));
    }
}

class PlayerbotsSelfBotAfkServerScript : public ServerScript
{
public:
    PlayerbotsSelfBotAfkServerScript()
        : ServerScript("PlayerbotsSelfBotAfkServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) {}

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        SelfBotControl::OnClientPacket(session, packet);

        if (packet.GetOpcode() != CMSG_LOGOUT_REQUEST)
            return true;

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !IsSelfBot(player) || !player->isAFK())
            return true;

        LOG_DEBUG("playerbots", "SelfBot {} stays in world: refused the AFK logout", player->GetName());

        WorldPackets::Character::LogoutResponse logoutResponse;
        logoutResponse.LogoutResult = 2;
        logoutResponse.Instant = false;
        session->SendPacket(logoutResponse.Write());

        return false;
    }
};

// [live1] Guild project board for the BotControl addon: the plan (guild_project_plan + Playerbots.UserPack.Guids) is
// read once at startup; deaths are counted in memory by the creature-kill hook; every row is built on request from
// the players in memory (world thread: chat packets are handled while the map threads are idle). No per-tick work, no
// DB query after startup.
namespace
{
    struct GuildPlanRow
    {
        uint32 low{0};
        uint32 num{0};  // [guild12] permanent number #1-#51 (guild_project_plan.num)
        std::string name;
        std::string grp;
        std::string role;
    };

    struct GuildDeaths
    {
        std::deque<uint32> times;  // game time seconds, last hour
        uint32 total{0};
        std::string killer;
    };

    struct GuildStill
    {
        uint32 mapId{0};
        float x{0}, y{0}, z{0};
        uint32 since{0};
    };

    std::vector<GuildPlanRow> guildPlan;  // written once at startup, read-only afterwards
    std::unordered_set<uint32> guildPlanLows;
    std::mutex guildDeathLock;
    std::unordered_map<uint32, GuildDeaths> guildDeaths;  // under guildDeathLock (map threads)
    std::unordered_map<uint32, GuildStill> guildStill;     // world thread only

    std::string BoardField(std::string text, size_t max)
    {
        std::replace(text.begin(), text.end(), ';', ',');
        std::replace(text.begin(), text.end(), '\t', ' ');
        return text.size() > max ? text.substr(0, max) : text;
    }

    // "STATUS|detail": the addon translates the status and shows the detail (quest title, vendor, distance).
    std::string GuildActivity(Player* bot, PlayerbotAI* botAI)
    {
        if (!botAI)
            return "PLAYER|";
        if (!bot->IsAlive())
            return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? "GHOST|" : "DEAD|";
        if (bot->IsInCombat())
        {
            Unit* victim = bot->GetVictim();
            return std::string("COMBAT|") + (victim ? victim->GetName() + " L" + std::to_string(victim->GetLevel()) : "");
        }
        if (bot->IsInFlight())
            return "TRAVEL_FLIGHT|";
        static char const* const names[] = {"IDLE", "GO_GRIND", "GO_CAMP", "WANDER_RANDOM", "WANDER_NPC",
                                             "DO_QUEST", "TRAVEL_FLIGHT", "REST", "OUTDOOR_PVP", "DO_GATHER", "DO_PROFESSION"};
        NewRpgInfo& info = botAI->rpgInfo;
        NewRpgStatus const status = info.GetStatus();
        std::string text = status < RPG_STATUS_END ? names[status] : "?";
        if (info.vendorRunEntry)
        {
            CreatureTemplate const* vendor = sObjectMgr->GetCreatureTemplate(info.vendorRunEntry);
            return "VENDOR|" + (vendor ? vendor->Name : std::to_string(info.vendorRunEntry)) + ": " +
                   info.vendorRunReason;
        }
        text += "|";
        if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
            text += (quest->quest ? quest->quest->GetTitle() : std::string("?")) + " (q" +
                    std::to_string(quest->questId) + ")";
        else if (auto const* grind = std::get_if<NewRpgInfo::GoGrind>(&info.data))
            text += std::to_string(uint32(bot->GetExactDist(grind->pos))) + " yd";
        else if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&info.data))
            text += std::to_string(uint32(bot->GetExactDist(camp->pos))) + " yd";
        else if (auto const* prof = std::get_if<NewRpgInfo::DoProfession>(&info.data))
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(prof->skill);
            CreatureTemplate const* npc = sObjectMgr->GetCreatureTemplate(prof->npcEntry);
            text += std::string(prof->kind == 1 ? "learn " : prof->kind == 3 ? "recipes " : prof->kind == 4 ? "craft "
                                                                                                     : "buy tool ") +
                    (line ? line->name[0] : "?") + " @ " +
                    (npc ? npc->Name : "?") + " " + std::to_string(uint32(bot->GetExactDist(prof->pos))) + " yd";
        }
        return text;
    }
}

namespace SelfBotControl
{
    void LoadGuildPlan()
    {
        guildPlan.clear();
        guildPlanLows.clear();
        if (QueryResult result = PlayerbotsDatabase.Query("SELECT guid, name, grp, role, num FROM guild_project_plan ORDER BY guid"))
        {
            do
            {
                Field* fields = result->Fetch();
                GuildPlanRow row;
                row.low = fields[0].Get<uint32>();
                row.name = fields[1].Get<std::string>();
                row.grp = fields[2].Get<std::string>();
                row.role = fields[3].Get<std::string>();
                row.num = fields[4].Get<uint8>();
                if (guildPlanLows.insert(row.low).second)
                    guildPlan.push_back(row);
            } while (result->NextRow());
        }
        // The user's pack (his own selfbot character among them) is on the board too, as group "P" when not planned.
        std::stringstream in(sConfigMgr->GetOption<std::string>("Playerbots.UserPack.Guids", "", false));
        std::string token;
        while (std::getline(in, token, ','))
        {
            uint32 const low = uint32(std::strtoul(token.c_str(), nullptr, 10));
            if (low && guildPlanLows.insert(low).second)
                guildPlan.push_back({low, 0, "", "P", "pack"});
        }
        LOG_INFO("playerbots", "[live1] guild board: {} characters", guildPlan.size());
    }

    void RecordGuildDeath(Player* killed, Creature* killer)
    {
        if (!killed || !killer || !guildPlanLows.count(killed->GetGUID().GetCounter()))
            return;
        uint32 const now = uint32(GameTime::GetGameTime().count());
        std::lock_guard<std::mutex> guard(guildDeathLock);
        GuildDeaths& d = guildDeaths[killed->GetGUID().GetCounter()];
        d.times.push_back(now);
        while (!d.times.empty() && now - d.times.front() > HOUR)
            d.times.pop_front();
        ++d.total;
        d.killer = killer->GetName() + " L" + std::to_string(killer->GetLevel());
    }

    void SendGuildBoard(Player* player)
    {
        if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
            return;

        uint32 const now = uint32(GameTime::GetGameTime().count());
        SendAddonLine(player, "GS;n=" + std::to_string(guildPlan.size()));
        for (GuildPlanRow const& row : guildPlan)
        {
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(row.low);
            CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
            Player* bot = ObjectAccessor::FindPlayer(guid);

            uint32 d1h = 0, dAll = 0;
            std::string killer = "-";
            {
                std::lock_guard<std::mutex> guard(guildDeathLock);
                auto itr = guildDeaths.find(row.low);
                if (itr != guildDeaths.end())
                {
                    for (uint32 t : itr->second.times)
                        d1h += now - t <= HOUR ? 1 : 0;
                    dAll = itr->second.total;
                    killer = itr->second.killer;
                }
            }

            // G;num;name;class;level;grp;role;online(1 bot/p person/0);state;zoneId;zone;STATUS|detail;actMin;
            //   stillMin;deaths1h;deathsAll;lastKiller;money (copper);bagsUsed;bagsTotal;skills ([guild20] skill:value/max,...)
            std::ostringstream out;
            out << "G;" << row.num << ";" << BoardField(cache ? cache->Name : row.name, 12) << ";"
                << uint32(bot ? bot->getClass() : (cache ? cache->Class : 0)) << ";"
                << uint32(bot ? bot->GetLevel() : (cache ? cache->Level : 0)) << ";" << BoardField(row.grp, 2) << ";"
                << BoardField(row.role, 8) << ";";
            if (!bot || !bot->IsInWorld())
            {
                out << "0;-;0;-;OFFLINE|;0;0;" << d1h << ";" << dAll << ";" << BoardField(killer, 24) << ";-;-;-;-";
                SendAddonLine(player, out.str());
                continue;
            }

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            char const state = !bot->IsAlive() ? (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? 'g' : 'd')
                                               : (bot->IsInCombat() ? 'c' : 'a');
            AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());

            uint32 actMin = 0;
            if (botAI && botAI->rpgInfo.startT)
                actMin = GetMSTimeDiffToNow(botAI->rpgInfo.startT) / (MINUTE * IN_MILLISECONDS);

            // minutes without moving more than 3 yd (sampled on the requests while the window is open)
            GuildStill& still = guildStill[row.low];
            if (!still.since || still.mapId != bot->GetMapId() || bot->GetExactDist(still.x, still.y, still.z) > 3.0f)
            {
                still.mapId = bot->GetMapId();
                still.x = bot->GetPositionX();
                still.y = bot->GetPositionY();
                still.z = bot->GetPositionZ();
                still.since = now;
            }

            uint32 total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START, used = 0;
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                used += bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) ? 1 : 0;
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                {
                    total += bag->GetBagSize();
                    for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                        used += bag->GetItemByPos(uint8(j)) ? 1 : 0;
                }

            out << (botAI ? "1" : "p") << ";" << state << ";" << bot->GetZoneId() << ";"
                << BoardField(zone ? zone->area_name[0] : "?", 24) << ";" << BoardField(GuildActivity(bot, botAI), 64) << ";"
                << actMin << ";" << (now - still.since) / MINUTE << ";" << d1h << ";" << dAll << ";"
                << BoardField(killer, 24) << ";" << bot->GetMoney() /* [guild70] copper */ << ";" << used << ";" << total << ";"
                << BoardField(NewRpgProfession::PlannedSkills(bot), 60);
            SendAddonLine(player, out.str());
        }
        SendAddonLine(player, "GE;n=" + std::to_string(guildPlan.size()));
    }

    // "Q;bot;questId;title;have/need;state" per quest in the bot's log (state c = complete, f = failed, - = open),
    // then "QE;bot". Asked by the addon for the row under the mouse.
    void SendGuildQuests(Player* player, std::string const& name)
    {
        if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
            return;
        std::string const botName = BoardField(name, 12);
        Player* bot = ObjectAccessor::FindPlayerByName(name, false);
        if (bot && !guildPlanLows.count(bot->GetGUID().GetCounter()))
            bot = nullptr;
        if (bot && bot->IsInWorld())
        {
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                if (!quest)
                    continue;
                uint32 have = 0, need = 0;
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i])
                    {
                        need += quest->RequiredNpcOrGoCount[i];
                        have += std::min<uint32>(bot->GetQuestSlotCounter(slot, i), quest->RequiredNpcOrGoCount[i]);
                    }
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
                    {
                        need += quest->RequiredItemCount[i];
                        have += std::min<uint32>(bot->GetItemCount(quest->RequiredItemId[i], true),
                                                 quest->RequiredItemCount[i]);
                    }
                QuestStatus const status = bot->GetQuestStatus(questId);
                std::ostringstream out;
                out << "Q;" << botName << ";" << questId << ";" << BoardField(quest->GetTitle(), 60) << ";" << have << "/"
                    << need << ";"
                    << (status == QUEST_STATUS_COMPLETE ? "c" : (status == QUEST_STATUS_FAILED ? "f" : "-"));
                SendAddonLine(player, out.str());
            }
        }
        SendAddonLine(player, "QE;" + botName);
    }
}

// The BotControl addon asks for its status with an addon whisper to the player himself: "BOTCTL\tstatus".
class PlayerbotsSelfBotControlPlayerScript : public PlayerScript
{
public:
    PlayerbotsSelfBotControlPlayerScript()
        : PlayerScript("PlayerbotsSelfBotControlPlayerScript", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) {}

    using PlayerScript::OnPlayerCanUseChat;  // keep the base overloads visible

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || lang != LANG_ADDON || !player || receiver != player)
            return true;

        std::string const request = std::string(SelfBotControl::ADDON_PREFIX) + "\t";
        if (msg.compare(0, request.size(), request) != 0)
            return true;

        std::string const command = msg.substr(request.size());
        if (command == "status")
            SelfBotControl::SendAddonStatus(player);
        else if (command == "guild")
            SelfBotControl::SendGuildBoard(player);  // [live1]
        else if (command == "orders")
            sGuildCoordinator.SendOrders(player);  // [guild30]
        else if (command == "resources")
            sGuildCoordinator.SendResources(player);  // [guild30] the virtual guild bank
        else if (command == "plan")
            sGuildCoordinator.SendPlan(player);  // [guild70] the «План» tab
        else if (command.compare(0, 7, "quests ") == 0)
            SelfBotControl::SendGuildQuests(player, command.substr(7));  // [live1]
        else if (command == "self on" || command == "self off" || command == "self")
            Tell(player, SelfBotControl::HandleCommand(player, command.size() > 5 ? command.substr(5) : ""));

        return false;  // answered; no echo back to the client
    }
};

class PlayerbotsSelfBotControlWorldScript : public WorldScript
{
public:
    PlayerbotsSelfBotControlWorldScript()
        : WorldScript("PlayerbotsSelfBotControlWorldScript", { WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_STARTUP }) {}

    void OnUpdate(uint32 diff) override { SelfBotControl::UpdateWorld(diff); }
    void OnStartup() override
    {
        SelfBotControl::LoadGuildPlan();  // [live1]
        NewRpgProfession::LoadPlan();     // [guild20]
    }
};

void AddPlayerbotsSelfBotAfkScripts()
{
    new PlayerbotsSelfBotAfkServerScript();
    new PlayerbotsSelfBotControlPlayerScript();
    new PlayerbotsSelfBotControlWorldScript();
}
