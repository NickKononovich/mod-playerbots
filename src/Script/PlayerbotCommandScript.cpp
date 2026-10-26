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

        static ChatCommandTable playerbotsCommandTable = {
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

    // playerbots test run <content> [count] [comp]
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
        return TestHarness::TestHarnessMgr::Instance().HandleRun(handler, tokens[0], count, comp);
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

    // playerbots watch list | playerbots watch <run id|number|content key> [name]: the named real player goes into
    // GM mode, invisible (".gm on", ".gm visible off"), and next to the run's tank inside its instance, as ".appear"
    // takes a GM into a dungeon. He is not added to the group.
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

        if (!watcher->IsInWorld() || watcher->IsBeingTeleported())
        {
            handler->PSendSysMessage("Watch: {} is loading, try again", watcher->GetName());
            return false;
        }

        // In a group the instance would be the group leader's bind (InstanceSaveMgr::PlayerGetDestinationInstanceId).
        if (watcher->GetGroup())
        {
            handler->PSendSysMessage("Watch: {} is in a group, leave it first", watcher->GetName());
            return false;
        }

        if (watcher->GetMap()->IsBattlegroundOrArena())
        {
            handler->PSendSysMessage("Watch: {} is in a battleground", watcher->GetName());
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

        Map* map = tank->GetMap();
        bool const sameMap = watcher->GetMapId() == tank->GetMapId();
        bool const sameInstance = sameMap && watcher->GetInstanceId() == tank->GetInstanceId();
        std::string const distance = sameInstance ? Acore::StringFormat("{:.1f} yd", watcher->GetDistance(tank))
                                                  : std::string("other map");

        if (!sameInstance)
        {
            if (sameMap)
            {
                // Inside an older instance of this dungeon: its bind cannot go while he is in it. Out first.
                handler->PSendSysMessage("Watch: {} is inside instance {} of {}, not the run's {}: sent to his "
                                         "homebind, run 'playerbots watch {}' again once he is there",
                                         watcher->GetName(), watcher->GetInstanceId(), map->GetMapName(),
                                         tank->GetInstanceId(), party->id);
                watcher->TeleportTo(watcher->m_homebindMapId, watcher->m_homebindX, watcher->m_homebindY,
                                    watcher->m_homebindZ, watcher->GetOrientation());
                return true;
            }

            InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(tank->GetInstanceId());
            if (!save)
            {
                handler->PSendSysMessage("Watch: instance {} has no instance save", tank->GetInstanceId());
                return false;
            }

            Difficulty const difficulty = Difficulty(map->GetSpawnMode());
            if (map->IsRaid())
                watcher->SetRaidDifficulty(difficulty);
            else
                watcher->SetDungeonDifficulty(difficulty);

            // ".appear" keeps an existing bind; a bind to an older run's instance would take him there instead.
            InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(watcher->GetGUID(), map->GetId(),
                                                                                difficulty);
            if (bind && bind->save != save)
            {
                handler->PSendSysMessage("Watch: {} unbound from instance {} ({})", watcher->GetName(),
                                         bind->save->GetInstanceId(), bind->perm ? "permanent" : "temporary");
                sInstanceSaveMgr->PlayerUnbindInstance(watcher->GetGUID(), map->GetId(), bind->save->GetDifficulty(),
                                                       true, watcher);
                bind = nullptr;
            }
            if (!bind)
                sInstanceSaveMgr->PlayerBindToInstance(watcher->GetGUID(), save, !save->CanReset(), watcher);
        }

        if (watcher->IsInFlight())
        {
            watcher->GetMotionMaster()->MovementExpired();
            watcher->CleanupAfterTaxiFlight();
        }
        else
            watcher->SaveRecallPosition();

        if (!watcher->TeleportTo(tank->GetMapId(), tank->GetPositionX(), tank->GetPositionY(),
                                 tank->GetPositionZ() + 0.25f, tank->GetOrientation(), TELE_TO_GM_MODE, tank))
        {
            handler->PSendSysMessage("Watch: the teleport of {} was refused", watcher->GetName());
            return false;
        }
        watcher->SetPhaseMask(tank->GetPhaseMask() | 1, false);

        handler->PSendSysMessage("Watch: {} (GM, invisible) -> {} (map {}, instance {}), run {} [{}] {}, tank {}, "
                                 "distance before {}",
                                 watcher->GetName(), map->GetMapName(), map->GetId(), tank->GetInstanceId(),
                                 party->seq, party->id, party->content->key, tank->GetName(), distance);
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
