/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotControl.h"

#include "CharacterPackets.h"
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
        else if (command == "self on" || command == "self off" || command == "self")
            Tell(player, SelfBotControl::HandleCommand(player, command.size() > 5 ? command.substr(5) : ""));

        return false;  // answered; no echo back to the client
    }
};

class PlayerbotsSelfBotControlWorldScript : public WorldScript
{
public:
    PlayerbotsSelfBotControlWorldScript()
        : WorldScript("PlayerbotsSelfBotControlWorldScript", { WORLDHOOK_ON_UPDATE }) {}

    void OnUpdate(uint32 diff) override { SelfBotControl::UpdateWorld(diff); }
};

void AddPlayerbotsSelfBotAfkScripts()
{
    new PlayerbotsSelfBotAfkServerScript();
    new PlayerbotsSelfBotControlPlayerScript();
    new PlayerbotsSelfBotControlWorldScript();
}
