/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AccountMgr.h"
#include "BattleGroundTactics.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommandTags.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Engine.h"
#include "GoToInstance.h"  // [tele20] which copy of the dungeon a "go to him" lands in, as a seam
#include "Group.h"
#include "GuildTaskMgr.h"
#include "InstanceSaveMgr.h"
#include "Item.h"
#include "Map.h"
#include "MapMgr.h"  // [b50_watchmap]
#include "Player.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "PlayerbotsUserPack.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TestHarnessMgr.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <memory>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include "GuildCoordinator.h"  // [guild60] playerbots guild ...
#include "GuildHonest.h"      // [honest]
#include "Spell.h"     // [guild13] diag cast
#include "SpellMgr.h"  // [guild13]

using namespace Acore::ChatCommands;

class playerbots_commandscript : public CommandScript
{
public:
    playerbots_commandscript() : CommandScript("playerbots_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerbotsDebugCommandTable = {
            {"bg", HandleDebugBGCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsAccountCommandTable = {
            {"setKey", HandleSetSecurityKeyCommand, SEC_PLAYER, Console::No},
            {"link", HandleLinkAccountCommand, SEC_PLAYER, Console::No},
            {"linkedAccounts", HandleViewLinkedAccountsCommand, SEC_PLAYER, Console::No},
            {"unlink", HandleUnlinkAccountCommand, SEC_PLAYER, Console::No},
        };

        static ChatCommandTable playerbotsTestCommandTable = {
            {"setup", HandleTestSetupCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"prepare", HandleTestPrepareCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"run", HandleTestRunCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"runall", HandleTestRunAllCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"status", HandleTestStatusCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"stop", HandleTestStopCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"list", HandleTestListCommand, SEC_ADMINISTRATOR, Console::Yes},
        };

        static ChatCommandTable playerbotsPackCommandTable = {
            {"status", HandlePackStatusCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"cmd", HandlePackCmdCommand, SEC_ADMINISTRATOR, Console::Yes},
        };

        static ChatCommandTable playerbotsWatcherCommandTable = {
            {"create", HandleWatcherCreateCommand, SEC_ADMINISTRATOR, Console::Yes},
        };

        static ChatCommandTable playerbotsRouteCommandTable = {
            {"show", HandleRouteShowCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"hide", HandleRouteHideCommand, SEC_ADMINISTRATOR, Console::Yes},
        };

        // [guild60] the guild coordinator's fives
        static ChatCommandTable playerbotsGuildCommandTable = {
            {"formfive", HandleGuildFormFiveCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"disband", HandleGuildDisbandCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"fives", HandleGuildFivesCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"dungeons", HandleGuildDungeonsCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"autofives", HandleGuildAutoFivesCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"need", HandleGuildNeedCommand, SEC_ADMINISTRATOR, Console::Yes},  // [ord10]
            {"diagnose", HandleGuildDiagnoseCommand, SEC_ADMINISTRATOR, Console::Yes},      // [feed20]
            {"undiagnosed", HandleGuildUndiagnosedCommand, SEC_ADMINISTRATOR, Console::Yes},
        };

        static ChatCommandTable playerbotsCommandTable = {
            {"guild", playerbotsGuildCommandTable},
            {"bot", HandlePlayerbotCommand, SEC_PLAYER, Console::No},
            {"exec", HandleExecCommand, SEC_GAMEMASTER, Console::Yes},
            {"inspect", HandleInspectCommand, SEC_GAMEMASTER, Console::Yes},
            {"gtask", HandleGuildTaskCommand, SEC_GAMEMASTER, Console::Yes},
            {"pmon", HandlePerfMonCommand, SEC_GAMEMASTER, Console::Yes},
            {"rndbot", HandleRandomPlayerbotCommand, SEC_GAMEMASTER, Console::Yes},
            {"debug", playerbotsDebugCommandTable},
            {"account", playerbotsAccountCommandTable},
            {"botas", HandleBotAsCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"tell", HandleTellBotCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"diag", HandleDiagBotCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"additem", HandleAddItemCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"test", playerbotsTestCommandTable},
            {"pack", playerbotsPackCommandTable},
            {"watcher", playerbotsWatcherCommandTable},
            {"watch", HandleWatchCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"goto", HandleGoToCommand, SEC_ADMINISTRATOR, Console::Yes},  // [tele20] into HIS copy of the dungeon
            {"route", playerbotsRouteCommandTable},
        };

        static ChatCommandTable commandTable = {
            {"playerbots", playerbotsCommandTable},
        };

        return commandTable;
    }

    static bool HandlePlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return PlayerbotMgr::HandlePlayerbotMgrCommand(handler, args);
    }

    // Splits "<name> <rest>" and resolves the online player by name.
    static Player* ParseTarget(ChatHandler* handler, char const* args, std::string& rest)
    {
        std::string input = args ? args : "";
        size_t const space = input.find(' ');
        std::string name = input.substr(0, space);
        rest = space == std::string::npos ? "" : input.substr(space + 1);

        if (name.empty() || !normalizePlayerName(name))
        {
            handler->PSendSysMessage("Usage: <playerName> <command>");
            return nullptr;
        }

        Player* player = ObjectAccessor::FindPlayerByName(name);
        if (!player)
            handler->PSendSysMessage("Player {} is not online", name);

        return player;
    }

    // Console equivalent of ".playerbots bot <args>" typed by an online player.
    static bool HandleBotAsCommand(ChatHandler* handler, char const* args)
    {
        std::string rest;
        Player* master = ParseTarget(handler, args, rest);
        if (!master)
            return false;

        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master);
        if (!mgr)
        {
            handler->PSendSysMessage("{} cannot control bots", master->GetName());
            return false;
        }

        for (std::string const& message : mgr->HandlePlayerbotCommand(rest.c_str(), master))
            handler->PSendSysMessage("{}", message);

        return true;
    }

    // Console equivalent of the bot's master whispering <command> to the bot (a selfbot is its own master).
    static bool HandleTellBotCommand(ChatHandler* handler, char const* args)
    {
        std::string rest;
        Player* bot = ParseTarget(handler, args, rest);
        if (!bot)
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            handler->PSendSysMessage("{} is not a bot", bot->GetName());
            return false;
        }

        Player* master = botAI->GetMaster() ? botAI->GetMaster() : bot;
        botAI->HandleCommand(CHAT_MSG_WHISPER, rest, master);
        handler->PSendSysMessage("{} <- {}", bot->GetName(), rest);
        return true;
    }

    // Console: why a bot does what it does - engine state, strategies, movement, follow target and how a few key
    // actions judge themselves right now ("<name> [action,action...]").
    static bool HandleDiagBotCommand(ChatHandler* handler, char const* args)
    {
        std::string rest;
        Player* bot = ParseTarget(handler, args, rest);
        if (!bot)
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            handler->PSendSysMessage("{} is not a bot", bot->GetName());
            return false;
        }

        BotState const state = botAI->GetState();
        handler->PSendSysMessage("{}: alive {} combat {} state {} map {} pos {:.1f} {:.1f} {:.1f} moving {} motion {} "
                                 "stand {} casting {}",
                                 bot->GetName(), bot->IsAlive(), bot->IsInCombat(), uint32(state), bot->GetMapId(),
                                 bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->isMoving(),
                                 uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                                 bot->IsStandState(), bot->IsNonMeleeSpellCast(false));
        if (Engine* engine = botAI->GetCurrentEngine())
            handler->PSendSysMessage("  last action: '{}' | {}", engine->GetLastAction(), engine->ListStrategies());

        Player* master = botAI->GetMaster();
        Group* group = bot->GetGroup();
        Player* leader = group ? ObjectAccessor::FindPlayer(group->GetLeaderGUID()) : nullptr;
        handler->PSendSysMessage("  master {} group leader {} dist {:.1f}", master ? master->GetName() : "-",
                                 leader ? leader->GetName() : "-", leader ? bot->GetDistance(leader) : -1.0f);

        // [guild13] "cast:<spell id>": Spell::CheckCast of that spell on the bot's current target (why a cast is
        // impossible), plus the bot's ammo
        if (rest.rfind("cast:", 0) == 0)
        {
            uint32 const spellId = uint32(std::strtoul(rest.c_str() + 5, nullptr, 10));
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            if (!info || !target)
            {
                handler->PSendSysMessage("  cast {}: no spell or no current target", spellId);
                return true;
            }
            Spell* spell = new Spell(bot, info, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
            spell->m_targets.SetUnitTarget(target);
            SpellCastResult const result = spell->CheckCast(true);
            delete spell;
            handler->PSendSysMessage("  cast {} on {} ({:.1f} yd, los {}, hostile {}): result {}, CanCastSpell {}, "
                                     "ammo id {} count {}",
                                     spellId, target->GetName(), bot->GetDistance(target),
                                     bot->IsWithinLOSInMap(target), bot->IsHostileTo(target), uint32(result),
                                     botAI->CanCastSpell(spellId, target), bot->GetUInt32Value(PLAYER_AMMO_ID),
                                     bot->GetItemCount(bot->GetUInt32Value(PLAYER_AMMO_ID)));
            return true;
        }

        std::string names = rest.empty() ? "follow,move to ranged position,dps assist,attack anything,stuck recovery"
                                         : rest;
        std::stringstream list(names);
        std::string name;
        while (std::getline(list, name, ','))
        {
            Action* action = botAI->GetAiObjectContext()->GetAction(name);
            if (!action)
            {
                handler->PSendSysMessage("  action '{}': not found", name);
                continue;
            }
            handler->PSendSysMessage("  action '{}': useful {} possible {}", name, action->isUseful(),
                                     action->isPossible());
        }
        return true;
    }

    // Console: put an item straight into an online character's bags ("<name> <itemId> [count]").
    static bool HandleAddItemCommand(ChatHandler* handler, char const* args)
    {
        std::string rest;
        Player* player = ParseTarget(handler, args, rest);
        if (!player)
            return false;

        uint32 itemId = 0;
        uint32 count = 1;
        std::istringstream(rest) >> itemId >> count;

        if (!itemId || !count || !sObjectMgr->GetItemTemplate(itemId))
        {
            handler->PSendSysMessage("Unknown item id {}", itemId);
            return false;
        }

        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) != EQUIP_ERR_OK)
        {
            handler->PSendSysMessage("{} has no room for {} x{}", player->GetName(), itemId, count);
            return false;
        }

        if (Item* item = player->StoreNewItem(dest, itemId, true))
            player->SendNewItem(item, count, true, false);
        // [honest] a console grant to a guild-plan bot is a handout: shown as a leftover
        GuildHonest::Handout(player,
                             ("item " + std::to_string(itemId) + " x" + std::to_string(count) + " (console)").c_str());

        handler->PSendSysMessage("{} received item {} x{}", player->GetName(), itemId, count);
        return true;
    }

    static std::vector<std::string> SplitArgs(char const* args)
    {
        std::vector<std::string> tokens;
        std::istringstream in(args ? args : "");
        std::string token;
        while (in >> token)
            tokens.push_back(token);
        return tokens;
    }

    // playerbots test setup <groups> [leveling] [variants] [horde]
    //   horde: Horde-race characters for the Horde composition 5AH (Uldaman's Lost Dwarves)
    //   without "leveling": the level-80 pool (heroics, raids, level-80 normals)
    //   leveling: level-1 characters for normal dungeons below 80 (raised as the dungeons go up, never lowered)
    static bool HandleTestSetupCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        uint32 const groups = tokens.empty() ? 1 : uint32(std::strtoul(tokens[0].c_str(), nullptr, 10));
        bool leveling = false;
        bool variants = false;
        bool horde = false;
        for (size_t i = 1; i < tokens.size(); ++i)
        {
            if (tokens[i] == "leveling" || tokens[i] == "levelling" || tokens[i] == "low")
                leveling = true;
            else if (tokens[i] == "variants")
                variants = true;
            else if (tokens[i] == "horde")
                horde = true;
        }
        return TestHarness::TestHarnessMgr::Instance().HandleSetup(handler, groups, leveling, variants, horde);
    }

    // playerbots test prepare <comp 5A|10A|25A|...> [level] [ilvl]
    static bool HandleTestPrepareCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->PSendSysMessage("Usage: playerbots test prepare <comp> [level] [average ilvl]");
            return false;
        }

        uint32 const level = tokens.size() > 1 ? uint32(std::strtoul(tokens[1].c_str(), nullptr, 10)) : 80;
        // 0: the levelling gear target of that level (200 at 80)
        uint32 const ilvl = tokens.size() > 2 ? uint32(std::strtoul(tokens[2].c_str(), nullptr, 10)) : 0;
        return TestHarness::TestHarnessMgr::Instance().HandlePrepare(handler, tokens[0],
                                                                     uint8(std::clamp<uint32>(level, 1, 80)), ilvl);
    }

    // playerbots test run <content> [count] [comp] [continue]
    static bool HandleTestRunCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->PSendSysMessage("Usage: playerbots test run <content> [count] [comp]");
            return false;
        }

        uint32 const count = tokens.size() > 1 ? uint32(std::strtoul(tokens[1].c_str(), nullptr, 10)) : 1;
        std::string const comp = tokens.size() > 2 ? tokens[2] : "";
        // [kara3] "continue": the same raid instance save as the last run of these members (their lockout)
        bool const resume = tokens.size() > 3 && tokens[3] == "continue";
        return TestHarness::TestHarnessMgr::Instance().HandleRun(handler, tokens[0], count, comp, resume);
    }

    // playerbots test runall dungeons|normal|heroic|raids|all [parallel N] [variants]
    //   dungeons = normal + heroic 5-man (and the 10-man Upper Blackrock Spire)
    static bool HandleTestRunAllCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->PSendSysMessage(
                "Usage: playerbots test runall dungeons|normal|heroic|raids|all|frontier [parallel N] [window L] [variants] [random]");
            return false;
        }

        uint32 parallel = 0;
        uint32 window = 10;
        bool variants = false;
        bool random = false;
        for (size_t i = 1; i < tokens.size(); ++i)
        {
            if (tokens[i] == "parallel" && i + 1 < tokens.size())
                parallel = uint32(std::strtoul(tokens[++i].c_str(), nullptr, 10));
            else if (tokens[i] == "window" && i + 1 < tokens.size())
                window = uint32(std::strtoul(tokens[++i].c_str(), nullptr, 10));
            else if (tokens[i] == "variants")
                variants = true;
            else if (tokens[i] == "random")
                random = true;
        }
        // [runall-random-comp] user 2026-10-25: every queued run picks a random composition of its size
        TestHarness::TestHarnessMgr::Instance().SetRunAllRandomComp(random);
        return TestHarness::TestHarnessMgr::Instance().HandleRunAll(handler, tokens[0], parallel, variants, window);
    }

    static bool HandleTestStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        return TestHarness::TestHarnessMgr::Instance().HandleStatus(handler);
    }

    // playerbots test stop <id|seq|all>
    static bool HandleTestStopCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->PSendSysMessage("Usage: playerbots test stop <run id|number|all>");
            return false;
        }
        return TestHarness::TestHarnessMgr::Instance().HandleStop(handler, tokens[0]);
    }

    // [guild60] playerbots guild formfive <dungeon key|name> [me] [character ...]: a forced five (the rest picked from
    // the free members; the level band and the need are not checked)
    static bool HandleGuildFormFiveCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->PSendSysMessage("Usage: playerbots guild formfive <dungeon> [me] [character ...]");
            return false;
        }
        bool withIssuer = false;
        std::vector<std::string> names;
        for (size_t i = 1; i < tokens.size(); ++i)
        {
            if (tokens[i] == "me")
                withIssuer = true;
            else
                names.push_back(tokens[i]);
        }
        Player* issuer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (withIssuer && !issuer)
        {
            handler->PSendSysMessage("'me' needs a character (not the console)");
            return false;
        }
        handler->PSendSysMessage("{}", sGuildCoordinator.FormFiveCommand(issuer, tokens[0], names, withIssuer));
        return true;
    }

    static bool HandleGuildDisbandCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->PSendSysMessage("Usage: playerbots guild disband <five id>");
            return false;
        }
        sGuildCoordinator.DisbandFive(uint32(std::strtoul(tokens[0].c_str(), nullptr, 10)), "command");
        handler->PSendSysMessage("{}", sGuildCoordinator.FivesText());
        return true;
    }

    // [drain114] playerbots guild fives [drain on|off]: the fives; drain - no new five, no formed five starts, the
    // started ones run to their result (before a server switch)
    static bool HandleGuildFivesCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (!tokens.empty() && tokens[0] == "drain")
        {
            if (tokens.size() < 2 || (tokens[1] != "on" && tokens[1] != "off"))
            {
                handler->PSendSysMessage("Usage: playerbots guild fives drain on|off");
                return false;
            }
            sGuildCoordinator.SetDrain(tokens[1] == "on");
        }
        handler->PSendSysMessage("{}", sGuildCoordinator.FivesText());
        return true;
    }

    // [feed20] "playerbots guild diagnose <run id> <the diagnosis in words>": the fact that an unclean run has been
    // looked into, onto the run's own row (guild_project_runs.diagnosed_at / diagnosis). The user's rule - every
    // unclean run is diagnosed before the next wave, a repeat without the analysis is how a problem stays behind -
    // was checkable by nothing until the fact lived beside the run. Nothing a bot decides reads it
    static bool HandleGuildDiagnoseCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.size() < 2)
        {
            handler->PSendSysMessage("Usage: playerbots guild diagnose <run id> <the diagnosis in words>");
            return false;
        }
        std::string text;
        for (size_t i = 1; i < tokens.size(); ++i)
            text += (text.empty() ? "" : " ") + tokens[i];
        handler->PSendSysMessage(
            "{}", sGuildCoordinator.DiagnoseRun(uint32(std::strtoul(tokens[0].c_str(), nullptr, 10)), text));
        return true;
    }

    // [feed20] "playerbots guild undiagnosed [content]": the rule itself, in one answer - the unclean runs that have
    // no diagnosis yet, newest first
    static bool HandleGuildUndiagnosedCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        handler->PSendSysMessage("{}", sGuildCoordinator.UndiagnosedText(tokens.empty() ? std::string() : tokens[0]));
        return true;
    }

    static bool HandleGuildDungeonsCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        uint32 const level = tokens.empty() ? 0 : uint32(std::strtoul(tokens[0].c_str(), nullptr, 10));
        handler->PSendSysMessage("{}", sGuildCoordinator.DungeonsText(uint8(std::min<uint32>(level, 255))));
        return true;
    }

    static bool HandleGuildAutoFivesCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (!tokens.empty())
            sGuildCoordinator.SetAutoFives(tokens[0] == "on" || tokens[0] == "1");
        handler->PSendSysMessage("guild auto fives: {}", sGuildCoordinator.AutoFives() ? "on" : "off");
        return true;
    }

    // [ord10] playerbots guild need [<item name|id> <count> [urgent]]: the user's own order into the guild leader's
    // wanted list - what, how many, and how urgent (the existing GuildTaskPriority levels: "urgent" may interrupt
    // questing, anything else is taken on the way). A count of 0 withdraws the order. With no arguments it prints the
    // leader's report, one line per standing order (the same lines he logs himself). There is no quoting in a command
    // line here, so a multi-word item name is read as every word before the count and joined back together
    static bool HandleGuildNeedCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> tokens = SplitArgs(args);
        if (tokens.empty())
        {
            std::vector<std::pair<uint32, std::string>> const lines = sGuildCoordinator.UserNeedLines();
            if (lines.empty())
                handler->PSendSysMessage("no standing order. Usage: playerbots guild need <item name|id> <count> "
                                         "[urgent]");
            for (auto const& [item, line] : lines)
                handler->PSendSysMessage("{}", line);
            return true;
        }
        bool urgent = false;
        if (tokens.back() == "urgent" || tokens.back() == "u")
        {
            urgent = true;
            tokens.pop_back();
        }
        if (tokens.size() < 2)
        {
            handler->PSendSysMessage("Usage: playerbots guild need <item name|id> <count> [urgent]");
            return false;
        }
        std::string const countText = tokens.back();
        if (countText.find_first_not_of("0123456789") != std::string::npos)
        {
            handler->PSendSysMessage("the count must be a number: playerbots guild need <item name|id> <count> "
                                     "[urgent]");
            return false;
        }
        tokens.pop_back();
        std::string name = tokens[0];  // the item name as the words before the count (no quoting in a command line)
        for (size_t i = 1; i < tokens.size(); ++i)
            name += " " + tokens[i];
        handler->PSendSysMessage("{}", sGuildCoordinator.UserNeedCommand(
                                           name, uint32(std::strtoul(countText.c_str(), nullptr, 10)), urgent));
        return true;
    }

    static bool HandleTestListCommand(ChatHandler* handler, char const* /*args*/)
    {
        return TestHarness::TestHarnessMgr::Instance().HandleList(handler);
    }

    static constexpr char const* WATCHER_DEFAULT_NAME = "Watcher";
    static constexpr uint32 SPELL_GM_INVISIBLE_VISUAL = 37800;  // the aura ".gm visible off" adds

    // Playerbots.UserPack.Guids, read per call: console commands only.
    static std::vector<uint32> UserPackLows()
    {
        std::vector<uint32> lows;
        std::stringstream in(sConfigMgr->GetOption<std::string>("Playerbots.UserPack.Guids", "", false));
        std::string token;
        while (std::getline(in, token, ','))
            if (uint32 const low = uint32(std::strtoul(token.c_str(), nullptr, 10)))
                lows.push_back(low);
        return lows;
    }

    static bool IsUserPackGuid(ObjectGuid guid)
    {
        std::vector<uint32> const lows = UserPackLows();
        return guid.IsPlayer() && std::find(lows.begin(), lows.end(), guid.GetCounter()) != lows.end();
    }

    // playerbots watcher create [name]: a level-1 spectator character on the user's account (the owner of the first
    // Playerbots.UserPack.Guids character, else TestHarness::PROTECTED_ACCOUNT_ID), created as the bot factory does.
    static bool HandleWatcherCreateCommand(ChatHandler* handler, char const* args)
    {
        if (handler->GetSession())
        {
            handler->SendSysMessage("playerbots watcher create: server console only");
            return false;
        }

        std::vector<std::string> const tokens = SplitArgs(args);
        std::string name = tokens.empty() ? WATCHER_DEFAULT_NAME : tokens[0];
        if (!normalizePlayerName(name) || ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS ||
            sObjectMgr->IsReservedName(name))
        {
            handler->PSendSysMessage("Watcher: '{}' is not a valid character name", name);
            return false;
        }

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
        stmt->SetData(0, name);
        if (!sCharacterCache->GetCharacterGuidByName(name).IsEmpty() || CharacterDatabase.Query(stmt))
        {
            handler->PSendSysMessage("Watcher: a character named {} already exists", name);
            return false;
        }

        uint32 accountId = 0;
        uint8 race = RACE_HUMAN;
        std::vector<uint32> const packLows = UserPackLows();
        CharacterCacheEntry const* first =
            packLows.empty()
                ? nullptr
                : sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(packLows.front()));
        if (first)
        {
            accountId = first->AccountId;
            race = IsAlliance(first->Race) ? RACE_HUMAN : RACE_ORC;  // the account's faction
        }
        if (!accountId)
            accountId = TestHarness::PROTECTED_ACCOUNT_ID;

        std::string accountName;
        if (!AccountMgr::GetName(accountId, accountName))
        {
            handler->PSendSysMessage("Watcher: account {} not found", accountId);
            return false;
        }

        if (sPlayerbotAIConfig.IsInRandomAccountList(accountId) || StringStartsWithI(accountName, "RNDBOT") ||
            StringStartsWithI(accountName, TestHarness::TEST_ACCOUNT_PREFIX))
        {
            handler->PSendSysMessage("Watcher: account {} ({}) is a bot account, refused", accountId, accountName);
            return false;
        }

        uint32 const characters = AccountMgr::GetCharactersCount(accountId);
        if (characters >= TestHarness::CHARACTERS_PER_ACCOUNT)
        {
            handler->PSendSysMessage("Watcher: account {} ({}) already has {} characters on this realm", accountId,
                                     accountName, characters);
            return false;
        }

        uint32 const disabledRaces = sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_RACEMASK);
        if ((1 << (race - 1)) & disabledRaces)
            race = race == RACE_HUMAN ? RACE_ORC : RACE_HUMAN;
        if ((1 << (race - 1)) & disabledRaces)
        {
            handler->SendSysMessage("Watcher: Human and Orc character creation are both disabled");
            return false;
        }

        uint8 const gender = GENDER_MALE;
        uint8 skin = 0;
        uint8 face = 0;
        uint8 hairStyle = 0;
        uint8 hairColor = 0;
        bool haveSkin = false;
        bool haveFace = false;
        bool haveHair = false;
        for (CharSectionsEntry const* section : sCharSectionsStore)
        {
            if (section->RaceID != race || section->SexID != gender)
                continue;
            switch (section->BaseSection)
            {
                case SECTION_TYPE_SKIN:
                    if (!haveSkin)
                        skin = section->ColorIndex;
                    haveSkin = true;
                    break;
                case SECTION_TYPE_FACE:
                    if (!haveFace)
                        face = section->VariationIndex;
                    haveFace = true;
                    break;
                case SECTION_TYPE_HAIR:
                    if (!haveHair)
                    {
                        hairStyle = section->VariationIndex;
                        hairColor = section->ColorIndex;
                    }
                    haveHair = true;
                    break;
                default:
                    break;
            }
        }

        std::unique_ptr<WorldSession> session = std::make_unique<WorldSession>(
            accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0, false,
            false, 0);
        CharacterCreateInfo info(name, race, CLASS_WARRIOR, gender, skin, face, hairStyle, hairColor, 0);

        Player* player = new Player(session.get());
        player->GetMotionMaster()->Initialize();
        if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &info))
        {
            player->CleanupsBeforeDelete();
            delete player;
            handler->PSendSysMessage("Watcher: Player::Create failed for {}", name);
            return false;
        }

        player->setCinematic(2);
        player->SetAtLoginFlag(AT_LOGIN_NONE);
        player->SaveToDB(true, false);
        sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), accountId, player->GetName(), player->getGender(),
                                                player->getRace(), player->getClass(), player->GetLevel());

        handler->PSendSysMessage("Watcher: created {} ({}, level {} {} Warrior) on account {} ({})", player->GetName(),
                                 player->GetGUID().ToString(), player->GetLevel(),
                                 race == RACE_HUMAN ? "Human" : "Orc", accountId, accountName);
        LOG_INFO("playerbots", "Watcher: created {} ({}) on account {}", player->GetName(),
                 player->GetGUID().ToString(), accountId);

        player->CleanupsBeforeDelete();
        delete player;
        return true;
    }

    // The run's tank inside its instance (main tank first), else the leader there, else any living member there.
    static Player* WatchTarget(TestHarness::Party const& party)
    {
        auto const inside = [&party](ObjectGuid guid) -> Player*
        {
            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->GetMap()->IsDungeon() ||
                bot->GetInstanceId() != party.instanceId)
                return nullptr;
            return bot;
        };

        if (!party.instanceId)
            return nullptr;

        Player* tank = nullptr;
        for (TestHarness::Member const& member : party.members)
        {
            if (!member.spec || member.spec->role != TestHarness::Role::Tank)
                continue;
            Player* bot = inside(member.guid);
            if (bot && bot->IsAlive() && (!tank || (member.slot.flags & TestHarness::SLOT_MAIN_TANK)))
                tank = bot;
        }
        if (tank)
            return tank;

        if (Player* leader = inside(party.leader))
            return leader;

        for (TestHarness::Member const& member : party.members)
            if (Player* bot = inside(member.guid))
                if (bot->IsAlive())
                    return bot;

        return nullptr;
    }

    // A run by id / number, else the first active run of that content key (inside its instance first).
    static TestHarness::Party const* FindWatchParty(std::string const& key)
    {
        TestHarness::Party const* inside = nullptr;
        TestHarness::Party const* active = nullptr;
        for (auto const& party : TestHarness::TestHarnessMgr::Instance().Parties())
        {
            if (party->id == key || std::to_string(party->seq) == key)
                return party.get();
            if (!party->content || !StringEqualI(party->content->key, key) ||
                TestHarness::IsTerminalPhase(party->phase))
                continue;
            if (!inside && party->instanceId)
                inside = party.get();
            if (!active)
                active = party.get();
        }
        return inside ? inside : active;
    }

    // playerbots route show <run id|number|content key> [seconds=300]: GM-only waypoint markers along the run's route
    // (backbone, leader's walk, route to the target, columns on the waypoint / target / next boss), redrawn every 10 s
    // for that long (TestHarnessMgr::HandleRouteShow). playerbots route hide <run id|number|content key>.
    static bool HandleRouteShowCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->SendSysMessage("Usage: playerbots route show <run id|number|content> [seconds=300]");
            return false;
        }

        TestHarness::Party const* party = FindWatchParty(tokens[0]);
        if (!party)
        {
            handler->PSendSysMessage("Route: no run '{}' (playerbots watch list)", tokens[0]);
            return false;
        }

        uint32 const seconds = tokens.size() > 1 ? uint32(std::strtoul(tokens[1].c_str(), nullptr, 10)) : 300;
        return TestHarness::TestHarnessMgr::Instance().HandleRouteShow(handler, party->seq, seconds ? seconds : 300);
    }

    static bool HandleRouteHideCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->SendSysMessage("Usage: playerbots route hide <run id|number|content>");
            return false;
        }

        TestHarness::Party const* party = FindWatchParty(tokens[0]);
        if (!party)
        {
            handler->PSendSysMessage("Route: no run '{}' (playerbots watch list)", tokens[0]);
            return false;
        }

        return TestHarness::TestHarnessMgr::Instance().HandleRouteHide(handler, party->seq);
    }

    static bool HandleWatchListCommand(ChatHandler* handler)
    {
        uint32 shown = 0;
        for (auto const& party : TestHarness::TestHarnessMgr::Instance().Parties())
        {
            if (!party->content || TestHarness::IsTerminalPhase(party->phase))
                continue;

            uint32 killed = 0;
            for (TestHarness::BossTrack const& boss : party->bosses)
                if (boss.killed)
                    ++killed;
            Player* tank = WatchTarget(*party);
            handler->PSendSysMessage("  {} [{}] {} - {}, tank {}, bosses {}/{}, instance {}", party->seq, party->id,
                                     party->content->key, TestHarness::PhaseName(party->phase),
                                     tank ? tank->GetName() : "-", killed, TestHarness::RequiredBossCount(*party),
                                     party->instanceId);
            ++shown;
        }
        if (!shown)
            handler->SendSysMessage("Watch: no active test runs");
        return true;
    }

    enum class GoToOutcome
    {
        Done,         // he is on his way into the target's own copy
        LeftDungeon,  // he stood in another copy of that dungeon and was sent out of it: run the command again
        Refused       // nothing was done, and the reason was said out loud
    };

    // [tele20] THE ONE WAY A REAL PLAYER GOES TO A BOT, map AND instance. Every five runs its own copy of the
    // dungeon, so a destination is a map and an instance id; a teleport that names only the map lands the player in
    // whatever copy his own bind points at, and in a brand new empty one when he has no bind
    // (InstanceSaveMgr::PlayerGetDestinationInstanceId, MapInstanced::CreateInstanceForPlayer). That is what the user
    // met: "I teleport into dungeons to the people, but I do not see them. I am in some other mirror."
    //
    // `.appear` (cs_misc.cpp HandleAppearCommand) binds the GM to the target's instance only when he has NO bind for
    // that map and difficulty yet, by its own comment "if the GM is bound to another instance, he will not be bound
    // to another one" - so a bind to an older copy survives it, and it still reports "Appearing at ...". Which copy
    // the bind has to point at is decided in GoToInstance::PlanGoTo (src/Script/GoToInstance.h), asserted by a test
    // over the real binds of 2026-11-15; this function only carries the plan out. It is the whole module's single
    // place for it: "playerbots goto" and "playerbots watch" both come through here.
    static GoToOutcome GoToPlayer(ChatHandler* handler, Player* mover, Player* target, std::string const& retry)
    {
        if (!mover->IsInWorld() || mover->IsBeingTeleported())
        {
            handler->PSendSysMessage("GoTo: {} is loading, try again", mover->GetName());
            return GoToOutcome::Refused;
        }

        if (!target->IsInWorld() || target->IsBeingTeleported())
        {
            handler->PSendSysMessage("GoTo: {} is loading, try again", target->GetName());
            return GoToOutcome::Refused;
        }

        // In a group the destination instance is the group leader's bind, not his own
        // (InstanceSaveMgr::PlayerGetDestinationInstanceId). In the target's OWN group that is the right instance
        // anyway and the core needs no help; in any other group nothing done here would decide where he lands.
        Group* const group = mover->GetGroup();
        if (group && group != target->GetGroup())
        {
            handler->PSendSysMessage("GoTo: {} is in another group - its leader's bind decides where he lands, so "
                                     "leave that group first",
                                     mover->GetName());
            return GoToOutcome::Refused;
        }

        if (mover->GetMap()->IsBattlegroundOrArena() || target->GetMap()->IsBattlegroundOrArena())
        {
            handler->PSendSysMessage("GoTo: a battleground or arena is not entered this way ({} -> {})",
                                     mover->GetName(), target->GetName());
            return GoToOutcome::Refused;
        }

        Map* map = target->GetMap();
        GoToInstance::Facts facts;
        facts.targetInDungeon = map->IsDungeon();
        facts.targetMap = map->GetId();
        facts.targetInstance = target->GetInstanceId();
        facts.moverMap = mover->GetMapId();
        facts.moverInstance = mover->GetInstanceId();
        facts.moverGroupedWithTarget = group != nullptr;  // checked above: the only group left here is the target's

        InstanceSave* save = nullptr;
        InstancePlayerBind* bind = nullptr;
        Difficulty const difficulty = Difficulty(map->GetSpawnMode());
        if (facts.targetInDungeon)
        {
            save = sInstanceSaveMgr->GetInstanceSave(facts.targetInstance);
            if (!save)
            {
                handler->PSendSysMessage("GoTo: instance {} of {} has no instance save", facts.targetInstance,
                                         map->GetMapName());
                return GoToOutcome::Refused;
            }

            bind = sInstanceSaveMgr->PlayerGetBoundInstance(mover->GetGUID(), facts.targetMap, difficulty);
            facts.boundInstance = bind ? bind->save->GetInstanceId() : 0;
        }

        GoToInstance::Plan const plan = GoToInstance::PlanGoTo(facts);

        if (plan.leaveFirst)
        {
            // Inside an older copy of this very dungeon: its bind cannot go while he stands in it. Out first.
            handler->PSendSysMessage("GoTo: {} is inside instance {} of {}, while {} is in instance {}: sent to his "
                                     "homebind, run '{}' again once he is there",
                                     mover->GetName(), facts.moverInstance, map->GetMapName(), target->GetName(),
                                     facts.targetInstance, retry);
            mover->TeleportTo(mover->m_homebindMapId, mover->m_homebindX, mover->m_homebindY, mover->m_homebindZ,
                              mover->GetOrientation());
            return GoToOutcome::LeftDungeon;
        }

        if (facts.targetInDungeon && facts.moverMap != facts.targetMap)
        {
            if (map->IsRaid())
                mover->SetRaidDifficulty(difficulty);
            else
                mover->SetDungeonDifficulty(difficulty);
        }

        if (plan.unbind)
        {
            handler->PSendSysMessage("GoTo: {} unbound from instance {} of {} ({}), it is not the copy {} is in",
                                     mover->GetName(), bind->save->GetInstanceId(), map->GetMapName(),
                                     bind->perm ? "permanent" : "temporary", target->GetName());
            sInstanceSaveMgr->PlayerUnbindInstance(mover->GetGUID(), facts.targetMap, bind->save->GetDifficulty(),
                                                   true, mover);
        }
        if (plan.bind)
            sInstanceSaveMgr->PlayerBindToInstance(mover->GetGUID(), save, !save->CanReset(), mover);

        if (mover->IsInFlight())
        {
            mover->GetMotionMaster()->MovementExpired();
            mover->CleanupAfterTaxiFlight();
        }
        else
            mover->SaveRecallPosition();

        if (!mover->TeleportTo(facts.targetMap, target->GetPositionX(), target->GetPositionY(),
                               target->GetPositionZ() + 0.25f, target->GetOrientation(), TELE_TO_GM_MODE, target))
        {
            handler->PSendSysMessage("GoTo: the teleport of {} was refused", mover->GetName());
            return GoToOutcome::Refused;
        }
        mover->SetPhaseMask(target->GetPhaseMask() | 1, false);

        return GoToOutcome::Done;
    }

    // [tele20] playerbots goto <name> [spectator]: the bot-control panel's "go to him". The spectator defaults to the
    // player who typed it (from the console, to WATCHER_DEFAULT_NAME). It is the panel's only teleport, because a
    // coordinate teleport (and ".appear" on a stale bind) puts the user in an empty copy of the dungeon.
    static bool HandleGoToCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->SendSysMessage("Usage: playerbots goto <name> [spectator]");
            return false;
        }

        std::string targetName = tokens[0];
        Player* target = normalizePlayerName(targetName) ? ObjectAccessor::FindPlayerByName(targetName) : nullptr;
        if (!target)
        {
            handler->PSendSysMessage("GoTo: {} is not online", tokens[0]);
            return false;
        }

        Player* mover = nullptr;
        if (tokens.size() > 1)
        {
            std::string moverName = tokens[1];
            mover = normalizePlayerName(moverName) ? ObjectAccessor::FindPlayerByName(moverName) : nullptr;
        }
        else if (handler->GetSession())
            mover = handler->GetSession()->GetPlayer();
        else
        {
            std::string moverName = WATCHER_DEFAULT_NAME;
            mover = normalizePlayerName(moverName) ? ObjectAccessor::FindPlayerByName(moverName) : nullptr;
        }

        if (!mover || !mover->GetSession())
        {
            handler->SendSysMessage("GoTo: the spectator is not online");
            return false;
        }

        if (!IsRealPlayer(mover) || mover->GetSession()->IsHeadless())
        {
            handler->PSendSysMessage("GoTo: {} is a bot, not a spectator", mover->GetName());
            return false;
        }

        if (mover->GetGUID() == target->GetGUID())
        {
            handler->PSendSysMessage("GoTo: {} is himself", mover->GetName());
            return false;
        }

        GoToOutcome const outcome =
            GoToPlayer(handler, mover, target, Acore::StringFormat("playerbots goto {}", target->GetName()));
        if (outcome == GoToOutcome::Done)
        {
            Map* map = target->GetMap();
            handler->PSendSysMessage("GoTo: {} -> {} ({}, map {}, instance {})", mover->GetName(), target->GetName(),
                                     map->GetMapName(), map->GetId(), target->GetInstanceId());
        }
        return outcome != GoToOutcome::Refused;
    }

    // playerbots watch list | playerbots watch <run id|number|content key> [name]: the named real player goes into
    // GM mode, invisible (".gm on", ".gm visible off"), and next to the run's tank inside its instance. He is not
    // added to the group. The teleport itself is GoToPlayer, shared with "playerbots goto".
    static bool HandleWatchCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const tokens = SplitArgs(args);
        if (tokens.empty())
        {
            handler->SendSysMessage("Usage: playerbots watch list | playerbots watch <run id|number|content> [name]");
            return false;
        }

        if (tokens[0] == "list")
            return HandleWatchListCommand(handler);

        TestHarness::Party const* party = FindWatchParty(tokens[0]);
        if (!party)
        {
            handler->PSendSysMessage("Watch: no run '{}' (playerbots watch list)", tokens[0]);
            return false;
        }

        if (!party->content || TestHarness::IsTerminalPhase(party->phase) || !party->instanceId)
        {
            handler->PSendSysMessage("Watch: run {} [{}] is {}, not inside a dungeon", party->seq, party->id,
                                     TestHarness::PhaseName(party->phase));
            return false;
        }

        Player* tank = WatchTarget(*party);
        if (!tank)
        {
            handler->PSendSysMessage("Watch: nobody of run {} is inside instance {} right now", party->id,
                                     party->instanceId);
            return false;
        }

        std::string name = tokens.size() > 1 ? tokens[1] : WATCHER_DEFAULT_NAME;
        Player* watcher = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
        if (!watcher || !watcher->GetSession())
        {
            handler->PSendSysMessage("Watch: {} is not online", name);
            return false;
        }

        if (!IsRealPlayer(watcher) || watcher->GetSession()->IsHeadless() || IsUserPackGuid(watcher->GetGUID()) ||
            TestHarness::TestHarnessMgr::Instance().IsTestCharacter(watcher->GetGUID()))
        {
            handler->PSendSysMessage("Watch: {} is a bot, a pack or a test character, not a spectator",
                                     watcher->GetName());
            return false;
        }

        // ".gm on" and ".gm visible off" (cs_gm.cpp)
        watcher->SetGameMaster(true);
        watcher->UpdateTriggerVisibility();
        if (!watcher->HasAura(SPELL_GM_INVISIBLE_VISUAL))
            watcher->AddAura(SPELL_GM_INVISIBLE_VISUAL, watcher);
        watcher->SetGMVisible(false);
        // user 2026-10-25: the spectator runs too slowly to keep up with the bots - ".modify speed all 5"
        watcher->SetSpeed(MOVE_RUN, 5.0f, true);
        watcher->SetSpeed(MOVE_WALK, 5.0f, true);
        watcher->SetSpeed(MOVE_SWIM, 5.0f, true);
        watcher->SetSpeed(MOVE_FLIGHT, 5.0f, true);
        watcher->UpdateObjectVisibility();

        GoToOutcome const outcome =
            GoToPlayer(handler, watcher, tank, Acore::StringFormat("playerbots watch {}", party->id));
        if (outcome != GoToOutcome::Done)
            return outcome == GoToOutcome::LeftDungeon;

        Map* map = tank->GetMap();
        handler->PSendSysMessage("Watch: {} (GM, invisible) -> {} (map {}, instance {}), run {} [{}] {}, tank {}",
                                 watcher->GetName(), map->GetMapName(), map->GetId(), tank->GetInstanceId(),
                                 party->seq, party->id, party->content->key, tank->GetName());
        ChatHandler(watcher->GetSession())
            .PSendSysMessage("Watching run {} ({}) next to {}. GM mode on, invisible; '.gm off' and "
                             "'.gm visible on' undo it.",
                             party->id, map->GetMapName(), tank->GetName());
        return true;
    }

    // ".playerbots pack status": the user's pack (Playerbots.UserPack), read only.
    static bool HandlePackStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        for (std::string const& line : UserPack::StatusLines())
            handler->SendSysMessage(line);
        return true;
    }

    // ".playerbots pack cmd <order>": a pack order from the console ("dungeon list", "goto Crossroads", ...).
    static bool HandlePackCmdCommand(ChatHandler* handler, char const* args)
    {
        for (std::string const& line : UserPack::ConsoleCommand(args ? args : ""))
            handler->SendSysMessage(line);
        return true;
    }

    // Sends its own error so the core does not append its generic help text.
    static PlayerbotAI* GetOnlineBotAI(ChatHandler* handler, PlayerIdentifier const& target)
    {
        Player* player = target.GetConnectedPlayer();
        PlayerbotAI* botAI = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
        if (!botAI)
            handler->SendErrorMessage("{} is not an online bot", target.GetName());
        return botAI;
    }

    // From the console or SOAP there is no player object to send the command, so the bot sends it to itself.
    // The bot's security check always lets a bot command itself.
    static bool HandleExecCommand(ChatHandler* handler, PlayerIdentifier target, Tail command)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;

        if (command.empty())
        {
            handler->SendErrorMessage("Usage: .playerbots exec <bot> <command> (state-changing commands; "
                                      "query replies go to the bot's master - use inspect)");
            return false;
        }

        Player* from = handler->GetSession() ? handler->GetSession()->GetPlayer() : botAI->GetBot();
        botAI->HandleCommand(CHAT_MSG_WHISPER, std::string(command), from);
        handler->PSendSysMessage("{}: '{}' sent", target.GetName(), std::string(command));
        return true;
    }

    static bool HandleInspectCommand(ChatHandler* handler, PlayerIdentifier target, Tail command)
    {
        PlayerbotAI* botAI = GetOnlineBotAI(handler, target);
        if (!botAI)
            return false;

        if (command.empty())
        {
            handler->SendErrorMessage("Usage: .playerbots inspect <bot> <query> (state, position, tpos, "
                                      "movement, target, hp, strategy, action, values, travel, budget)");
            return false;
        }

        std::string const result = botAI->HandleRemoteCommand(std::string(command));
        handler->PSendSysMessage("{}: {}", target.GetName(), result.empty() ? "<empty>" : result);
        return true;
    }

    static bool HandleRandomPlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(handler, args);
    }

    static bool HandleGuildTaskCommand(ChatHandler* handler, char const* args)
    {
        return GuildTaskMgr::HandleConsoleCommand(handler, args);
    }

    static bool HandlePerfMonCommand(ChatHandler* /*handler*/, char const* args)
    {
        if (!strcmp(args, "reset"))
        {
            sPerfMonitor.Reset();
            return true;
        }

        if (!strcmp(args, "tick"))
        {
            sPerfMonitor.PrintStats(true, false);
            sPerfMonitor.DumpJson(true);
            return true;
        }

        if (!strcmp(args, "stack"))
        {
            sPerfMonitor.PrintStats(false, true);
            sPerfMonitor.DumpJson(false);
            return true;
        }

        if (!strcmp(args, "toggle"))
        {
            sPlayerbotAIConfig.PerfMonEnabled = !sPlayerbotAIConfig.PerfMonEnabled;
            if (sPlayerbotAIConfig.PerfMonEnabled)
                LOG_INFO("playerbots", "Performance monitor enabled");
            else
                LOG_INFO("playerbots", "Performance monitor disabled");
            return true;
        }

        sPerfMonitor.PrintStats();
        sPerfMonitor.DumpJson(false);
        return true;
    }

    static bool HandleDebugBGCommand(ChatHandler* handler, char const* args)
    {
        return BGTactics::HandleConsoleCommand(handler, args);
    }

    static bool HandleSetSecurityKeyCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->PSendSysMessage("Usage: .playerbots account setKey <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();
        std::string key = args;

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleSetSecurityKeyCommand(player, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleLinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        char* key = strtok(nullptr, " ");

        if (!accountName || !key)
        {
            handler->PSendSysMessage("Usage: .playerbots account link <accountName> <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleLinkAccountCommand(player, accountName, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleViewLinkedAccountsCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleViewLinkedAccountsCommand(player);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleUnlinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        if (!accountName)
        {
            handler->PSendSysMessage("Usage: .playerbots account unlink <accountName>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleUnlinkAccountCommand(player, accountName);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }
};

// Redraws the "playerbots route show" markers (TestHarnessMgr::TickRouteViz). World thread: World::Update runs the
// world scripts after MapMgr::Update has waited for the map threads, so summoning in an instance map is safe here.
// [b50_watchmap] User request 2026-10-26: the spectator ("playerbots watch") sees himself and the whole bot group on
// the World Map. The 3.3.5 client knows no position inside Classic/TBC instances (no WorldMapArea there; the BossPins
// addon draws Cataclysm map tiles instead), and the watcher is not in the bots' group, so the server tells him.
// Once a second, for every active test run inside its instance, each real player in GM mode (or with GM security)
// on that map instance gets addon whispers from himself (LANG_ADDON, as AddonChannelCommandHandler::Send does):
//   prefix "BPPOS", body "<mapId>;<run seq>;<unit>;<unit>;..." (at most WATCH_MAP_MAX_BODY characters, more
//   messages when the group does not fit), unit = "name,class,role,flag,x,y,z,orientationDeg,wmoGroup"
//   role T tank / H healer / D damage / - the watcher; flag W watcher, A alive, D dead;
//   wmoGroup = Map::GetAreaInfo's WMO group id under the unit (the id WMOAreaTable / DungeonMapChunk /
//   UiMapAssignment use), -1 when it stands in no WMO. BossPins turns that into the floor and the map position.
// Only members inside the run's instance are sent. Cost: a party scan and one vmap area query per sent unit per
// second, only for runs a GM is watching. World thread (World::Update, after MapMgr::Update waited for the maps).
namespace WatchMap
{
    constexpr bool ENABLED = true;
    constexpr uint32 TICK_MS = 1000;
    constexpr char const* PREFIX = "BPPOS";
    constexpr size_t MAX_BODY = 230;  // the client takes 255 characters for prefix + tab + body

    bool IsWatcher(Player* player)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        return session && !session->IsHeadless() && IsRealPlayer(player) && player->IsInWorld() &&
               (player->IsGameMaster() || session->GetSecurity() >= SEC_GAMEMASTER);
    }

    std::string Unit(Map* map, Player* unit, char role, char flag)
    {
        uint32 mogpFlags = 0;
        int32 adtId = 0, rootId = 0, groupId = 0;
        int32 const group = map->GetAreaInfo(unit->GetPhaseMask(), unit->GetPositionX(), unit->GetPositionY(),
                                             unit->GetPositionZ(), mogpFlags, adtId, rootId, groupId)
                                ? groupId
                                : -1;
        int32 const degrees = int32(unit->GetOrientation() * 180.0f / float(M_PI) + 0.5f) % 360;
        return Acore::StringFormat("{},{},{},{},{:.0f},{:.0f},{:.0f},{},{}", unit->GetName(), uint32(unit->getClass()),
                                   role, flag, unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(),
                                   degrees, group);
    }

    void Send(Player* watcher, std::string const& body)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, watcher, watcher,
                                     std::string(PREFIX) + "\t" + body);
        watcher->GetSession()->SendPacket(&data);
    }

    void SendAll(Player* watcher, std::string const& header, std::vector<std::string> const& units)
    {
        std::string body = header;
        bool any = false;
        for (std::string const& unit : units)
        {
            if (any && body.size() + 1 + unit.size() > MAX_BODY)
            {
                Send(watcher, body);
                body = header;
                any = false;
            }
            body += ";" + unit;
            any = true;
        }
        if (any)
            Send(watcher, body);
    }

    void Tick()
    {
        for (auto const& party : TestHarness::TestHarnessMgr::Instance().Parties())
        {
            if (!party->content || TestHarness::IsTerminalPhase(party->phase) || !party->instanceId)
                continue;
            Map* map = sMapMgr->FindMap(party->content->mapId, party->instanceId);
            if (!map || !map->IsDungeon())
                continue;

            std::vector<Player*> watchers;
            map->DoForAllPlayers(
                [&watchers](Player* player)
                {
                    if (IsWatcher(player))
                        watchers.push_back(player);
                });
            if (watchers.empty())
                continue;

            std::vector<std::string> members;
            for (TestHarness::Member const& member : party->members)
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->GetMap() != map)
                    continue;
                char role = 'D';
                if (member.spec && member.spec->role == TestHarness::Role::Tank)
                    role = 'T';
                else if (member.spec && member.spec->role == TestHarness::Role::Healer)
                    role = 'H';
                members.push_back(Unit(map, bot, role, bot->IsAlive() ? 'A' : 'D'));
            }

            std::string const header = Acore::StringFormat("{};{}", map->GetId(), party->seq);
            for (Player* watcher : watchers)
            {
                std::vector<std::string> units;
                units.reserve(members.size() + 1);
                units.push_back(Unit(map, watcher, '-', 'W'));
                units.insert(units.end(), members.begin(), members.end());
                SendAll(watcher, header, units);
            }
        }
    }
}

class playerbots_routeviz_worldscript : public WorldScript
{
public:
    playerbots_routeviz_worldscript() : WorldScript("playerbots_routeviz_worldscript", {WORLDHOOK_ON_UPDATE}) {}

    void OnUpdate(uint32 diff) override
    {
        TestHarness::TestHarnessMgr::Instance().TickRouteViz();
        // [b50_watchmap] positions for the watcher's World Map, 1 Hz
        _watchMapMs += diff;
        if (WatchMap::ENABLED && _watchMapMs >= WatchMap::TICK_MS)
        {
            _watchMapMs = 0;
            WatchMap::Tick();
        }
    }

private:
    uint32 _watchMapMs = 0;
};

void AddPlayerbotsCommandscripts()
{
    new playerbots_commandscript();
    new playerbots_routeviz_worldscript();
}
