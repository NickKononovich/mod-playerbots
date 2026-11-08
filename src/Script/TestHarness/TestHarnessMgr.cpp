/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestHarnessMgr.h"
#include "TerrainHazard.h"  // [lead226] InHarmfulLiquid: no dry place in lava

#include "ANAddTactics.h"
#include "AccountMgr.h"
#include "AiFactory.h"  // [guild60] a guild member's talent tab
#include "AttackerCountValues.h"
#include "AiObjectContext.h"
#include "Bag.h"
#include "ChooseTargetActions.h"  // [lead7] PullPlanner
#include "CharacterCache.h"
#include "Chat.h"
#include "CoSTriggers.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "EncounterValues.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "HealthTriggers.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "LeaderOrders.h"  // [lead1]
#include "LeaderCombatEnd.h"  // [lead107] the leader's phantom-combat end / stale-victim drop
#include "LeaderRest.h"       // [lead107] the leader's rest / far wait
#include "RouteHolds.h"       // [lead107] the leader's route holds after the rest
#include "RouteReenter.h"     // [lead341] the leader's re-entries
#include "LeaderDungeonRun.h"  // [guild70] the five-leader's run: this harness is its clearing engine
#include "RouteMeet.h"         // [lead345] the leader's walk to a member under attack
#include "Log.h"
#include "Map.h"
#include "MobRoles.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PoolMgr.h"  // [world_boss] the pooled spot of a world boss
#include "Player.h"
#include "World.h"  // [peaklog] sWorld->QueueCliCommand
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PathGenerator.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "Random.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "TargetValue.h"  // [lead217] IsCrowdControlled
#include "RandomPlayerbotMgr.h"
#include "SelfTravelActions.h"
#include "StringFormat.h"
#include "TestRunnerStrategy.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace TestHarness
{
    constexpr uint32 MAX_WIPES_PER_RUN = 2;
    // [b30_st_spirit] user 2026-10-25: DEATH_BURST_COUNT deaths within DEATH_BURST_WINDOW_MS and somebody still dead or
    // outside DEATH_BURST_RECOVER_MS later is a wipe (st-n 20261025-184229-st-n-5F-11: four deaths to the Atal'ai
    // Deathwalker's Spirit, two ghosts that could not walk back, 295 s idle and no "wipe" for the stop-on-wipe rule).
    constexpr uint32 DEATH_BURST_COUNT = 3;
    constexpr uint32 DEATH_BURST_WINDOW_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 DEATH_BURST_RECOVER_MS = 120 * IN_MILLISECONDS;
    // [b33_mech2] The boss the last death of the party fell during (its "death" line's "during"), per party seq
    // (world thread): a wipe found after the attempt closed (a death burst, a lone survivor) keeps that boss.
    // mech-n 20261026-000434-mech-n-5D-17: four deaths during Nethermancer Sepethrea, the wipe written 106 s later
    // as "trash, partial".
    struct WipeBossNote
    {
        std::string boss;
        uint32 ms = 0;
    };
    static std::map<uint32, WipeBossNote> wipeBossNotes;

    namespace
    {
        constexpr uint32 TICK_MS = 1000;
        constexpr uint32 POSITIONS_SETUP_MS = 5 * IN_MILLISECONDS;  // out of combat, while a pull is set up
        constexpr uint32 POSITIONS_MS = 2 * IN_MILLISECONDS;  // the run ticks every second: at most every 2nd tick
        constexpr uint32 SETUP_TICK_MS = 1000;
        constexpr uint32 QUEUE_TICK_MS = 2000;
        constexpr uint32 LOGIN_TIMEOUT_MS = 120 * IN_MILLISECONDS;
        constexpr uint32 TELEPORT_TIMEOUT_MS = 60 * IN_MILLISECONDS;
        constexpr uint32 ACCOUNT_WAIT_MS = 60 * IN_MILLISECONDS;
        constexpr uint32 PREPARES_PER_TICK = 2;
        constexpr uint32 CHARACTERS_PER_SETUP_TICK = 10;
        constexpr uint32 DUNGEON_TIMEOUT_MS = 90 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 RAID_TIMEOUT_MS = 4 * HOUR * IN_MILLISECONDS;
        constexpr uint8 DEFAULT_LEVEL = 80;
        // The test group wears the content's boss loot minus this (user rule 2026-10-24: clamp(15 % of the loot, 3, 10),
        // about 10 at level 60-70 content, less for the low dungeons where a flat 10 left starter gear).
        uint32 IlvlBelowContent(uint32 contentIlvl) { return std::clamp<uint32>((contentIlvl * 15 + 50) / 100, 3, 10); }

        std::string NonCombatCleanup()
        {
            return "-quest,-gather,-duel,-start duel,-pvp,-emote,-loot,-rpg,-new rpg,-grind,-travel,-move random,"
                   "-lfg,-bg";
        }

        std::string Stamp()
        {
            std::string stamp = WallClock();  // "YYYY-MM-DD HH:MM:SS"
            stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
            stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
            std::replace(stamp.begin(), stamp.end(), ' ', '-');
            return stamp;
        }

        std::string RandomPassword()
        {
            std::string password;
            for (uint32 i = 0; i < 16; ++i)
                password += char(urand('a', 'z'));
            return password;
        }

        bool IsTerminal(Phase phase) { return IsTerminalPhase(phase); }

        using Leader::Dungeon::REGEN_SIT_GRACE_MS;  // [lead107] LeaderRest.h
        using Leader::Dungeon::REGEN_WAIT_BELOW_LINE;  // [lead107] LeaderRest.h
        // [downtime_positions] / [downtime_phase] diagnostics for tools/downtime_audit.py
        constexpr bool DOWNTIME_POSITIONS = true;
        constexpr bool DOWNTIME_PHASE_LOG = true;

        // Violet Hold (instance_violet_hold.cpp): six prisoners, two released at random; instance_encounters credits
        // only Erekem 29315 / Moragg 29316 (vh-h 2026-10-25: Xevozz was trash, "not pulled" 10 min, then the trash loop
        // skipped him 15 min, no wave 7 in 17 minutes). They are boss creatures of the run.
        constexpr uint32 VIOLET_HOLD_MAP = 608;
        constexpr uint32 VIOLET_HOLD_PRISONERS[] = {29266, 29312, 29313, 29314, 29315, 29316};
        // A released prisoner never comes by himself (boss_*.cpp MoveInLineOfSight {}) and the next wave waits for his
        // death: nobody fighting him this long fails the run (the bots' VH strategy does not engage him).
        constexpr uint32 RELEASED_BOSS_IDLE_MS = 2 * MINUTE * IN_MILLISECONDS;

        bool IsVioletHoldPrisoner(Creature const* creature)
        {
            return creature->GetMapId() == VIOLET_HOLD_MAP &&
                   std::find(std::begin(VIOLET_HOLD_PRISONERS), std::end(VIOLET_HOLD_PRISONERS),
                             creature->GetEntry()) != std::end(VIOLET_HOLD_PRISONERS);
        }
        using Leader::Dungeon::REGROUP_PING_PONG_GUARD;  // [lead107] LeaderRest.h
        using Leader::Dungeon::DECISION_MEET;  // [lead107] LeaderRest.h
        using Leader::Dungeon::DECISION_ROUTE;  // [lead107] LeaderRest.h
        using Leader::Dungeon::REGROUP_PING_PONG_TOGETHER;  // [lead107] LeaderRest.h
        using Leader::Dungeon::REGROUP_PING_PONG_LEVEL_DZ;  // [lead107] LeaderRest.h
        using Leader::Dungeon::REGROUP_PING_PONG_MEET_RANGE;  // [lead107] LeaderRest.h
        using Leader::Dungeon::FAR_SLOPE_DZ_ALLOWANCE;  // [lead107] LeaderRest.h
        using Leader::Dungeon::FAR_WAIT_RANGE;  // [lead107] LeaderRest.h
        using Leader::Dungeon::FAR_TRAIL_RANGE;  // [lead107] LeaderRest.h
        using Leader::Dungeon::FAR_RESUME_RANGE;  // [lead107] LeaderRest.h
        using Leader::Dungeon::ApartMetric;  // [lead107] LeaderRest.h
        using Leader::Dungeon::CountRestItems;  // [lead107] LeaderRest.h
    }

    bool IsTerminalPhase(Phase phase)
    {
        return phase == Phase::Done || phase == Phase::Failed || phase == Phase::Stalled || phase == Phase::Stopped;
    }

    char const* PhaseName(Phase phase)
    {
        switch (phase)
        {
            case Phase::Picking:
                return "picking";
            case Phase::LoggingIn:
                return "logging in";
            case Phase::Preparing:
                return "preparing";
            case Phase::Grouping:
                return "grouping";
            case Phase::Idle:
                return "idle";
            case Phase::Teleporting:
                return "teleporting";
            case Phase::Buffing:
                return "buffing";
            case Phase::ReadyCheck:
                return "ready check";
            case Phase::Traveling:
                return "traveling";
            case Phase::Waiting:
                return "waiting";
            case Phase::Drinking:
                return "drinking";
            case Phase::EventStart:
                return "event start";
            case Phase::Clearing:
                return "clearing trash";
            case Phase::BossFight:
                return "boss fight";
            case Phase::Reviving:
                return "reviving";
            case Phase::Done:
                return "done";
            case Phase::Failed:
                return "failed";
            case Phase::Stalled:
                return "stalled";
            case Phase::Stopped:
                return "stopped";
            case Phase::Entering:
                return "entering";
        }
        return "?";
    }

    TestHarnessMgr& TestHarnessMgr::Instance()
    {
        static TestHarnessMgr instance;
        return instance;
    }

    bool TestHarnessMgr::IsTestCharacter(ObjectGuid guid) const
    {
        for (PoolCharacter const& character : _pool)
            if (character.guid == guid)
                return true;
        return false;
    }

    std::map<uint8, uint32> TestHarnessMgr::RequiredPerClass(uint32 groups, std::vector<uint8> const& sizes,
                                                             bool variants) const
    {
        std::map<uint8, uint32> total;
        for (uint8 size : sizes)
        {
            std::map<uint8, uint32> maxCount;
            for (Composition const* comp : CompositionsOfSize(size))
            {
                if (!variants && !comp->optimal)
                    continue;
                std::map<uint8, uint32> count;
                for (CompSlot const& slot : comp->slots)
                    if (SpecDef const* spec = FindSpec(slot.spec))
                        ++count[spec->cls];
                for (auto const& [cls, n] : count)
                    maxCount[cls] = std::max(maxCount[cls], n);
            }
            for (auto const& [cls, n] : maxCount)
                total[cls] += n * groups;
        }
        return total;
    }

    bool TestHarnessMgr::LoadPool(std::string& error)
    {
        _pool.clear();
        _poolLoaded = false;

        std::vector<uint32> accounts;
        if (QueryResult result =
                LoginDatabase.Query("SELECT id, username FROM account WHERE username LIKE '{}%%'", TEST_ACCOUNT_PREFIX))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const id = fields[0].Get<uint32>();
                std::string const name = fields[1].Get<std::string>();
                if (id == PROTECTED_ACCOUNT_ID || sPlayerbotAIConfig.IsInRandomAccountList(id) ||
                    sRandomPlayerbotMgr.IsAccountType(id, 1) || sRandomPlayerbotMgr.IsAccountType(id, 2))
                {
                    LOG_WARN("playerbots", "Test harness: account {} ({}) is a bot pool account, skipped", name, id);
                    continue;
                }
                accounts.push_back(id);
            } while (result->NextRow());
        }

        if (accounts.empty())
        {
            _poolLoaded = true;
            return true;
        }

        std::string list;
        for (uint32 id : accounts)
            list += (list.empty() ? "" : ",") + std::to_string(id);

        // Characters grouped with anybody outside the test pool, or in a guild, are left alone.
        std::set<uint32> unsafe;
        if (QueryResult result = CharacterDatabase.Query("SELECT guid FROM guild_member WHERE guid IN "
                                                          "(SELECT guid FROM characters WHERE account IN ({}))",
                                                          list))
        {
            do
            {
                unsafe.insert(result->Fetch()[0].Get<uint32>());
            } while (result->NextRow());
        }

        std::map<uint32, std::vector<uint32>> groupMembers;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT gm.guid, gm.memberGuid, c.account FROM group_member gm LEFT JOIN characters c ON "
                "c.guid = gm.memberGuid WHERE gm.guid IN (SELECT guid FROM group_member WHERE memberGuid IN "
                "(SELECT guid FROM characters WHERE account IN ({})))",
                list))
        {
            std::set<uint32> testAccounts(accounts.begin(), accounts.end());
            std::set<uint32> badGroups;
            do
            {
                Field* fields = result->Fetch();
                uint32 const groupId = fields[0].Get<uint32>();
                uint32 const member = fields[1].Get<uint32>();
                uint32 const account = fields[2].Get<uint32>();
                groupMembers[groupId].push_back(member);
                if (!testAccounts.count(account))
                    badGroups.insert(groupId);
            } while (result->NextRow());

            for (uint32 groupId : badGroups)
                for (uint32 member : groupMembers[groupId])
                    unsafe.insert(member);
        }

        if (QueryResult result =
                CharacterDatabase.Query("SELECT guid, name, class, account, level, race FROM characters "
                                        "WHERE account IN ({})",
                                        list))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const low = fields[0].Get<uint32>();
                if (unsafe.count(low))
                {
                    LOG_WARN("playerbots", "Test harness: character {} is in a guild or a foreign group, skipped",
                             fields[1].Get<std::string>());
                    continue;
                }

                PoolCharacter character;
                character.guid = ObjectGuid::Create<HighGuid::Player>(low);
                character.name = fields[1].Get<std::string>();
                character.cls = fields[2].Get<uint8>();
                character.account = fields[3].Get<uint32>();
                character.level = fields[4].Get<uint8>();
                character.leveling = character.name.rfind(NAME_PREFIX_LEVELING, 0) == 0;
                character.horde = Player::TeamIdForRace(fields[5].Get<uint8>()) == TEAM_HORDE;
                _pool.push_back(character);
            } while (result->NextRow());
        }

        error.clear();
        _poolLoaded = true;
        return true;
    }

    std::string TestHarnessMgr::NextName(uint8 cls, char const* prefix)
    {
        static std::map<std::pair<std::string, uint8>, uint32> counters;
        char const* code = "tst";
        switch (cls)
        {
            case CLASS_WARRIOR:
                code = "war";
                break;
            case CLASS_PALADIN:
                code = "pal";
                break;
            case CLASS_HUNTER:
                code = "hun";
                break;
            case CLASS_ROGUE:
                code = "rog";
                break;
            case CLASS_PRIEST:
                code = "pri";
                break;
            case CLASS_DEATH_KNIGHT:
                code = "dek";
                break;
            case CLASS_SHAMAN:
                code = "sha";
                break;
            case CLASS_MAGE:
                code = "mag";
                break;
            case CLASS_WARLOCK:
                code = "loc";
                break;
            case CLASS_DRUID:
                code = "dru";
                break;
            default:
                break;
        }

        for (uint32 tries = 0; tries < 26 * 26; ++tries)
        {
            uint32 const n = counters[{prefix, cls}]++ % (26 * 26);
            std::string name = std::string(prefix) + code + char('a' + n / 26) + char('a' + n % 26);
            if (ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS || sObjectMgr->IsReservedName(name))
                continue;
            if (!sCharacterCache->GetCharacterGuidByName(name).IsEmpty())
                continue;

            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
            stmt->SetData(0, name);
            if (CharacterDatabase.Query(stmt))
                continue;

            return name;
        }
        return "";
    }

    bool TestHarnessMgr::CreateCharacter(uint32 accountId, uint8 cls, std::string& error)
    {
        uint8 const race = RaceForClass(cls, _setup.horde);
        uint8 const gender = GENDER_MALE;
        std::string const name = NextName(cls, _setup.leveling ? NAME_PREFIX_LEVELING : NAME_PREFIX_MAX_LEVEL);
        if (name.empty())
        {
            error = "no free name";
            return false;
        }

        uint8 skin = 0;
        uint8 face = 0;
        uint8 hairStyle = 0;
        uint8 hairColor = 0;
        uint8 facialHair = 0;
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
                    {
                        face = section->VariationIndex;
                        skin = haveSkin ? skin : section->ColorIndex;
                    }
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

        WorldSession* session = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER,
                                                 EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0, false,
                                                 false, 0);
        CharacterCreateInfo info(name, race, cls, gender, skin, face, hairStyle, hairColor, facialHair);

        Player* player = new Player(session);
        player->GetMotionMaster()->Initialize();
        if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &info))
        {
            player->CleanupsBeforeDelete();
            delete player;
            delete session;
            error = "Player::Create failed for " + name;
            return false;
        }

        player->setCinematic(2);
        player->SetAtLoginFlag(AT_LOGIN_NONE);
        player->SaveToDB(true, false);
        sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), accountId, player->GetName(), player->getGender(),
                                                player->getRace(), player->getClass(), player->GetLevel());

        PoolCharacter character;
        character.guid = player->GetGUID();
        character.name = player->GetName();
        character.cls = cls;
        character.account = accountId;
        character.level = player->GetLevel();
        character.leveling = _setup.leveling;
        character.horde = _setup.horde;
        _pool.push_back(character);

        LOG_INFO("playerbots", "Test harness: created {} ({}, level {}{}) on account {}", character.name,
                 ClassName(cls), character.level, character.leveling ? ", levelling pool" : "", accountId);

        player->CleanupsBeforeDelete();
        delete player;
        delete session;
        return true;
    }

    constexpr uint8 LOW_POOL_MAX_LEVEL = 20;

    bool TestHarnessMgr::HandleSetup(ChatHandler* handler, uint32 groups, bool leveling, bool variants, bool horde)
    {
        if (_setup.active)
        {
            handler->PSendSysMessage("Test harness: setup is already running (stage {})", _setup.stage);
            return false;
        }

        if (!groups || groups > 20)
        {
            handler->PSendSysMessage("Usage: playerbots test setup <groups 1..20> [leveling] [variants]");
            return false;
        }

        std::string error;
        if (!LoadPool(error))
        {
            handler->PSendSysMessage("Test harness: cannot load the pool: {}", error);
            return false;
        }

        // The level-80 pool covers 5 / 10 / 25-player groups (all variants, as before); the levelling pool the 5-player
        // normals and the 10-player Upper Blackrock Spire, optimal compositions unless variants.
        std::map<uint8, uint32> have;
        for (PoolCharacter const& character : _pool)
            // A levelling character never goes back down: only the low ones can still serve the level-15 normals
            // (wave 11: 78 characters below 20 but no hunter, so no 5A group could start below level 58).
            if (character.leveling == leveling && character.horde == horde &&
                (!leveling || character.level < LOW_POOL_MAX_LEVEL))
                ++have[character.cls];

        // The Horde pool: the Horde composition only (HORDE_COMPOSITION), per parallel group.
        std::map<uint8, uint32> required;
        if (horde)
        {
            if (Composition const* comp = FindComposition(HORDE_COMPOSITION))
                for (CompSlot const& slot : comp->slots)
                    if (SpecDef const* spec = FindSpec(slot.spec))
                        required[spec->cls] += groups;
        }
        else
            required = leveling ? RequiredPerClass(groups, {5, 10}, variants)
                                : RequiredPerClass(groups, {5, 10, 25}, true);

        _setup = SetupJob();
        _setup.leveling = leveling;
        _setup.horde = horde;
        handler->PSendSysMessage("Test harness: {} pool for {} parallel group(s)",
                                 leveling ? "levelling (below 80, 'Rl' names)" : "level-80 ('Rt' names)", groups);
        uint32 totalMissing = 0;
        for (auto const& [cls, need] : required)
        {
            uint32 const missing = need > have[cls] ? need - have[cls] : 0;
            if (missing)
                _setup.missing[cls] = missing;
            totalMissing += missing;
            handler->PSendSysMessage("  {}: need {}, have {}, create {}", ClassName(cls), need, have[cls], missing);
        }

        if (!totalMissing)
        {
            handler->PSendSysMessage("Test harness: the pool already holds {} characters ({} parallel group(s) fit)",
                                     _pool.size(), groups);
            return true;
        }

        uint32 existingAccounts = 0;
        if (QueryResult result =
                LoginDatabase.Query("SELECT COUNT(*) FROM account WHERE username LIKE '{}%%'", TEST_ACCOUNT_PREFIX))
            existingAccounts = result->Fetch()[0].Get<uint32>();

        uint32 const totalCharacters = uint32(_pool.size()) + totalMissing;
        uint32 const neededAccounts = (totalCharacters + CHARACTERS_PER_ACCOUNT - 1) / CHARACTERS_PER_ACCOUNT;
        for (uint32 index = existingAccounts + 1; index <= neededAccounts; ++index)
        {
            std::string const name = Acore::StringFormat("{}{:02}", TEST_ACCOUNT_PREFIX, index);
            if (AccountMgr::GetId(name))
                continue;
            AccountOpResult const result = sAccountMgr->CreateAccount(name, RandomPassword());
            if (result != AOR_OK)
                LOG_ERROR("playerbots", "Test harness: cannot create account {} ({})", name, uint32(result));
            _setup.accounts.push_back(name);
        }

        _setup.active = true;
        _setup.stage = 1;
        _setup.startedMs = getMSTime();
        handler->PSendSysMessage("Test harness: setup started - {} new account(s), {} new character(s); watch the "
                                 "worldserver log",
                                 _setup.accounts.size(), totalMissing);
        return true;
    }

    void TestHarnessMgr::TickSetup()
    {
        if (!_setup.active)
            return;

        if (_setup.stage == 1)
        {
            bool ready = true;
            for (std::string const& name : _setup.accounts)
                if (!AccountMgr::GetId(name))
                    ready = false;

            if (!ready)
            {
                if (getMSTimeDiff(_setup.startedMs, getMSTime()) > ACCOUNT_WAIT_MS)
                {
                    LOG_ERROR("playerbots",
                              "Test harness: accounts did not appear in the login database, setup aborted");
                    _setup.active = false;
                }
                return;
            }
            _setup.stage = 2;
        }

        if (_setup.stage != 2)
            return;

        // Character counts per account; the pool already contains what this session created.
        std::vector<std::pair<uint32, uint32>> accounts;
        if (QueryResult result =
                LoginDatabase.Query("SELECT id FROM account WHERE username LIKE '{}%%' ORDER BY id",
                                    TEST_ACCOUNT_PREFIX))
        {
            do
            {
                uint32 const id = result->Fetch()[0].Get<uint32>();
                if (id == PROTECTED_ACCOUNT_ID)
                    continue;
                uint32 count = 0;
                for (PoolCharacter const& character : _pool)
                    if (character.account == id)
                        ++count;
                accounts.push_back({id, std::max(count, AccountMgr::GetCharactersCount(id))});
            } while (result->NextRow());
        }

        uint32 created = 0;
        for (auto& [cls, missing] : _setup.missing)
        {
            while (missing && created < CHARACTERS_PER_SETUP_TICK)
            {
                auto account = std::find_if(accounts.begin(), accounts.end(),
                                            [](auto const& entry) { return entry.second < CHARACTERS_PER_ACCOUNT; });
                if (account == accounts.end())
                {
                    LOG_ERROR("playerbots", "Test harness: no test account with a free character slot, setup aborted");
                    _setup.active = false;
                    return;
                }

                std::string error;
                if (!CreateCharacter(account->first, cls, error))
                {
                    LOG_ERROR("playerbots", "Test harness: {}", error);
                    _setup.active = false;
                    return;
                }
                ++account->second;
                --missing;
                ++created;
            }
        }

        bool done = true;
        for (auto const& [cls, missing] : _setup.missing)
            if (missing)
                done = false;

        if (done)
        {
            LOG_INFO("playerbots", "Test harness: setup finished, pool holds {} characters", _pool.size());
            _setup.active = false;
            _poolLoaded = true;
        }
    }

    void TestHarnessMgr::AssignProfessions(Party& party)
    {
        std::map<Role, uint32> roleIndex;
        for (Member& member : party.members)
            member.professions = ProfessionPairFor(member.spec->role, roleIndex[member.spec->role]++);

        if (party.members.size() < 5)
            return;

        auto has = [&party](uint16 skill)
        {
            for (Member const& member : party.members)
                if (member.professions.first == skill || member.professions.second == skill)
                    return true;
            return false;
        };

        // At least one of each table-buff profession; the replacement goes to a role that uses its perk.
        std::vector<std::pair<uint16, std::vector<Role>>> const required = {
            {SKILL_JEWELCRAFTING, {Role::Melee, Role::Ranged, Role::Tank, Role::Caster}},
            {SKILL_ENCHANTING, {Role::Caster, Role::Healer, Role::Melee}},
            {SKILL_ALCHEMY, {Role::Healer, Role::Caster}},
            {SKILL_INSCRIPTION, {Role::Healer, Role::Caster}},
        };

        std::set<uint16> const protectedSkills = {SKILL_JEWELCRAFTING, SKILL_ENCHANTING, SKILL_ALCHEMY,
                                                  SKILL_INSCRIPTION};
        for (auto const& [skill, roles] : required)
        {
            if (has(skill))
                continue;

            bool placed = false;
            for (Role role : roles)
            {
                for (auto itr = party.members.rbegin(); itr != party.members.rend() && !placed; ++itr)
                {
                    Member& member = *itr;
                    if (member.spec->role != role || member.professions.first == skill)
                        continue;
                    if (protectedSkills.count(member.professions.second))
                        continue;
                    member.professions.second = skill;
                    placed = true;
                }
                if (placed)
                    break;
            }
        }
    }

    PoolCharacter const* TestHarnessMgr::PickCharacter(uint8 cls, uint8 level, std::set<ObjectGuid> const& picked,
                                                       bool horde) const
    {
        PoolCharacter const* best = nullptr;
        for (PoolCharacter const& character : _pool)
        {
            if (character.cls != cls || character.level > level || _inUse.count(character.guid) ||
                picked.count(character.guid) || character.horde != horde)
                continue;

            // The levelling pool never goes to level-80 content (it would be raised past the dungeons it is for).
            if (level >= MAX_TEST_LEVEL && character.leveling)
                continue;

            // Online without bot AI means somebody logged in with the client: leave it alone.
            if (Player* online = ObjectAccessor::FindConnectedPlayer(character.guid))
                if (!GET_PLAYERBOT_AI(online))
                    continue;

            // Highest level first: least to raise, and the low characters stay free for low dungeons.
            if (!best || character.level > best->level ||
                (character.level == best->level && character.leveling && !best->leveling))
                best = &character;
        }
        return best;
    }

    Party* TestHarnessMgr::CreateParty(Composition const* comp, ContentDef const* content, uint8 level, uint32 ilvl,
                                      std::string& error)
    {
        if (!comp)
        {
            error = "unknown composition";
            return nullptr;
        }

        if (!_poolLoaded && !LoadPool(error))
            return nullptr;

        auto party = std::make_unique<Party>();
        party->comp = comp;
        party->content = content;
        party->level = level;
        party->targetIlvl = ilvl;
        party->createdMs = getMSTime();
        party->phaseSinceMs = party->createdMs;
        party->nextTickMs = party->createdMs;

        std::set<ObjectGuid> picked;
        for (CompSlot const& slot : comp->slots)
        {
            SpecDef const* spec = FindSpec(slot.spec);
            if (!spec)
            {
                error = std::string("unknown spec ") + slot.spec;
                return nullptr;
            }

            PoolCharacter const* chosen = PickCharacter(spec->cls, level, picked, comp->horde);
            if (!chosen)
            {
                if (comp->horde)
                {
                    // None at all in the pool: dropped from the queue at once (no "not enough": no wait).
                    bool const anyHorde = std::any_of(_pool.begin(), _pool.end(), [&](PoolCharacter const& candidate)
                                                      { return candidate.horde && candidate.cls == spec->cls; });
                    error = Acore::StringFormat("{} Horde {} test characters{} (run 'playerbots test "
                                                "setup <n>{} horde')",
                                                anyHorde ? "not enough free" : "no", ClassName(spec->cls),
                                                level < MAX_TEST_LEVEL ? " at level " + std::to_string(level) +
                                                                             " or below"
                                                                       : std::string(),
                                                level < MAX_TEST_LEVEL ? " leveling" : "");
                    return nullptr;
                }
                error = level < MAX_TEST_LEVEL
                            ? Acore::StringFormat("not enough free {} test characters at level {} or below (run "
                                                  "'playerbots test setup <n> leveling')",
                                                  ClassName(spec->cls), level)
                            : Acore::StringFormat("not enough free {} test characters (run 'playerbots test "
                                                  "setup <n>')",
                                                  ClassName(spec->cls));
                return nullptr;
            }

            picked.insert(chosen->guid);
            Member member;
            member.guid = chosen->guid;
            member.name = chosen->name;
            member.cls = chosen->cls;
            member.spec = spec;
            member.slot = slot;
            party->members.push_back(member);
            if ((slot.flags & SLOT_MAIN_TANK) && party->leader.IsEmpty())
                party->leader = chosen->guid;
        }

        if (party->leader.IsEmpty())
            for (Member const& member : party->members)
                if (member.spec->role == Role::Tank)
                {
                    party->leader = member.guid;
                    break;
                }
        if (party->leader.IsEmpty())
            party->leader = party->members.front().guid;

        for (ObjectGuid const& guid : picked)
            _inUse.insert(guid);

        // Numbered only once it has its characters (the queue retries waiting items every tick).
        party->seq = _nextSeq++;
        party->id = content ? Acore::StringFormat("{}-{}-{}-{}", Stamp(), content->key, comp->id, party->seq)
                            : Acore::StringFormat("P{}", party->seq);

        AssignProfessions(*party);
        _parties.push_back(std::move(party));
        return _parties.back().get();
    }

    Party* TestHarnessMgr::FindParty(std::string const& id)
    {
        for (auto const& party : _parties)
            if (party->id == id || std::to_string(party->seq) == id)
                return party.get();
        return nullptr;
    }

    void TestHarnessMgr::SetPhase(Party& party, Phase phase)
    {
        if (party.phase == phase)
            return;
        // [downtime_phase] every phase change, with how long the old phase lasted and who the group waits for, so
        // tools/downtime_audit.py can cut the gaps between pulls at the phase changes
        uint32 const now = getMSTime();
        if (DOWNTIME_PHASE_LOG && party.phaseSinceMs)
            LogEvent(party, "phase", JsonObject().Str("from", PhaseName(party.phase)).Str("to", PhaseName(phase))
                                         .Num("sec", getMSTimeDiff(party.phaseSinceMs, now) / IN_MILLISECONDS)
                                         .Str("waitingFor", party.waitingFor));
        party.phase = phase;
        party.phaseSinceMs = now;
    }

    void TestHarnessMgr::Update(uint32 diff)
    {
        _setupTimer += diff;
        if (_setupTimer >= SETUP_TICK_MS)
        {
            _setupTimer = 0;
            TickSetup();
        }

        _queueTimer += diff;
        if (_queueTimer >= QUEUE_TICK_MS)
        {
            _queueTimer = 0;
            StartQueued();
        }

        uint32 const now = getMSTime();
        // [lead7] the leader's pull planner looks on every world update (the tank's ask is the clock), not on the
        // party's 1 s tick: the tank waits one world update for its decision, as it waited for its own look before
        for (auto const& party : _parties)
            if (!IsTerminal(party->phase))
                TickLeaderPlanner(*party);

        for (auto const& party : _parties)
        {
            if (IsTerminal(party->phase) || int32(now - party->nextTickMs) < 0)
                continue;

            party->nextTickMs = now + TICK_MS;
            TickParty(*party);
        }

        _parties.erase(std::remove_if(_parties.begin(), _parties.end(),
                                      [](std::unique_ptr<Party> const& party) { return IsTerminal(party->phase); }),
                       _parties.end());
    }

    void TestHarnessMgr::TickParty(Party& party)
    {
        switch (party.phase)
        {
            case Phase::Picking:
                for (Member const& member : party.members)
                    if (!ObjectAccessor::FindConnectedPlayer(member.guid))
                        sRandomPlayerbotMgr.AddPlayerBot(member.guid, 0);
                SetPhase(party, Phase::LoggingIn);
                break;
            case Phase::LoggingIn:
                if (TickLogin(party))
                    SetPhase(party, Phase::Preparing);
                break;
            case Phase::Preparing:
                if (TickPrepare(party))
                    SetPhase(party, Phase::Grouping);
                break;
            case Phase::Grouping:
                if (TickGrouping(party))
                {
                    if (party.guild)
                        SetPhase(party, Phase::Entering);  // [guild60] the five walks in itself
                    else if (party.content)
                        SetPhase(party, Phase::Teleporting);
                    else
                    {
                        SetPhase(party, Phase::Idle);
                        LOG_INFO("playerbots", "Test harness: party {} ({}) is prepared and grouped", party.id,
                                 party.comp->id);
                    }
                }
                break;
            case Phase::Teleporting:
                TickTeleport(party);
                break;
            case Phase::Entering:
                TickGuildEnter(party);
                break;
            case Phase::Idle:
                break;
            default:
                if (!IsTerminal(party.phase))
                    TickRun(party);
                break;
        }
    }

    bool TestHarnessMgr::TickLogin(Party& party)
    {
        bool all = true;
        for (Member const& member : party.members)
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !GET_PLAYERBOT_AI(bot))
                all = false;
        }

        if (!all && getMSTimeDiff(party.phaseSinceMs, getMSTime()) > LOGIN_TIMEOUT_MS)
        {
            Finish(party, "failed", "login timeout");
            return false;
        }
        return all;
    }

    bool TestHarnessMgr::TickPrepare(Party& party)
    {
        bool hasWarlock = false;
        for (Member const& member : party.members)
            if (member.cls == CLASS_WARLOCK)
                hasWarlock = true;

        uint32 done = 0;
        for (Member& member : party.members)
        {
            if (member.prepared)
                continue;
            if (done >= PREPARES_PER_TICK)
                return false;

            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot)
            {
                Finish(party, "failed", member.name + " went offline while preparing");
                return false;
            }

            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* other = ref->GetSource())
                        if (!IsTestCharacter(other->GetGUID()))
                        {
                            Finish(party, "failed", member.name + " is grouped with a non-test character");
                            return false;
                        }
                group->RemoveMember(bot->GetGUID());
            }

            member.prep = PrepareCharacter(bot, *member.spec, party.level, party.targetIlvl, member.professions,
                                           hasWarlock, party.content);
            if (!member.prep.ok)
            {
                Finish(party, "failed", member.name + ": " + member.prep.error);
                return false;
            }
            // [b31_dk_talents] A talent shortfall is visible in the run itself (DKs prepared in Ebon Hold tanked and
            // dealt damage with 25 of 71 points); warned, the run goes on, as gear_above_content.
            if (member.prep.talentsSpent < member.prep.talentsExpected)
                LogEvent(party, "talent_shortfall",
                         JsonObject().Str("name", member.name).Str("class", ClassName(member.cls))
                             .Str("spec", member.spec->label).Num("specNo", member.prep.specNo)
                             .Num("level", member.prep.level).Num("spent", member.prep.talentsSpent)
                             .Num("expected", member.prep.talentsExpected).Num("core", member.prep.talentsCore)
                             .Num("free", member.prep.talentsFree).Num("map", member.prep.talentsMap));

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                botAI->ResetStrategies(false);

            member.prepared = true;
            ++done;
            for (PoolCharacter& character : _pool)
                if (character.guid == member.guid)
                    character.level = bot->GetLevel();
            LOG_INFO("playerbots", "Test harness: {} prepared as {} - avg ilvl {:.1f} (target {}), {} + {}",
                     member.name, member.spec->label, member.prep.avgIlvl, party.targetIlvl,
                     ProfessionName(member.professions.first), ProfessionName(member.professions.second));
        }
        return true;
    }

    void TestHarnessMgr::ApplyStrategies(Party& party)
    {
        for (Member const& member : party.members)
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
            if (!botAI)
                continue;

            botAI->ChangeStrategy(NonCombatCleanup(), BOT_STATE_NON_COMBAT);
            // [guild60] a guild five loots its kills (the drops are its gear; the five-leader decides the rolls)
            if (party.guild)
                botAI->ChangeStrategy("+loot", BOT_STATE_NON_COMBAT);
            if (member.guid == party.leader)
            {
                botAI->ChangeStrategy("+aggressive,+test runner", BOT_STATE_NON_COMBAT);
                botAI->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
            }
            else
                botAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);

            // Bots walk by themselves: every member walks to the waypoint it is given (the leader's "come here",
            // "test runner"), into the instance through its portal alive ("instance portal") and as a ghost ("ghost
            // portal"), and swims up for air ("breathe") - its own actions, the same a human leader's bots get with
            // "nc +instance portal" and so on.
            botAI->ChangeStrategy("+test runner,+instance portal,+breathe", BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("+breathe", BOT_STATE_COMBAT);
            botAI->ChangeStrategy("+ghost portal", BOT_STATE_DEAD);
        }
    }

    bool TestHarnessMgr::TickGrouping(Party& party)
    {
        Player* leader = ObjectAccessor::FindPlayer(party.leader);
        if (!leader)
        {
            Finish(party, "failed", "leader offline");
            return false;
        }

        if (Group* old = leader->GetGroup())
            old->RemoveMember(leader->GetGUID());

        Group* group = new Group();
        if (!group->Create(leader))
        {
            delete group;
            Finish(party, "failed", "cannot create the group");
            return false;
        }
        sGroupMgr->AddGroup(group);

        if (party.comp->size > 5)
            group->ConvertToRaid();

        for (Member const& member : party.members)
        {
            if (member.guid == party.leader)
                continue;
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot)
                continue;
            if (Group* old = bot->GetGroup())
                old->RemoveMember(bot->GetGUID());
            if (!group->AddMember(bot))
                LOG_ERROR("playerbots", "Test harness: {} could not join the group of {}", member.name, party.id);
        }

        for (Member const& member : party.members)
        {
            if (group->isRaidGroup() && member.slot.subgroup >= 1 &&
                group->GetMemberGroup(member.guid) != member.slot.subgroup - 1)
                group->ChangeMembersGroup(member.guid, member.slot.subgroup - 1);
            if (member.slot.flags & SLOT_MAIN_TANK)
                group->SetGroupMemberFlag(member.guid, true, MEMBER_FLAG_MAINTANK);
            if (member.slot.flags & SLOT_OFF_TANK)
                group->SetGroupMemberFlag(member.guid, true, MEMBER_FLAG_MAINASSIST);
        }

        if (party.content)
        {
            Difficulty const difficulty = Difficulty(party.content->difficulty);
            if (party.content->raid)
                group->SetRaidDifficulty(difficulty);
            else
                group->SetDungeonDifficulty(difficulty);

            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                {
                    if (party.content->raid)
                        bot->SetRaidDifficulty(difficulty);
                    else
                        bot->SetDungeonDifficulty(difficulty);
                }
        }

        party.groupGuid = group->GetGUID();
        ApplyStrategies(party);
        return true;
    }

    void TestHarnessMgr::GrantAccess(Party& party, Player* bot)
    {
        DungeonProgressionRequirements const* access =
            sObjectMgr->GetAccessRequirement(party.content->mapId, Difficulty(party.content->difficulty));
        if (!access)
            return;

        TeamId const team = bot->GetTeamId(true);
        auto applies = [team](ProgressionRequirement const* requirement)
        { return requirement && (requirement->faction == TEAM_NEUTRAL || requirement->faction == team); };

        for (ProgressionRequirement const* requirement : access->quests)
        {
            if (!applies(requirement) || bot->GetQuestRewardStatus(requirement->id))
                continue;
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(requirement->id))
            {
                bot->SetQuestStatus(requirement->id, QUEST_STATUS_COMPLETE);
                bot->RewardQuest(quest, 0, bot, false);
                LogEvent(party, "access_granted",
                         JsonObject().Str("name", bot->GetName()).Str("kind", "quest").Num("id", requirement->id));
            }
        }

        for (ProgressionRequirement const* requirement : access->items)
        {
            if (!applies(requirement) || bot->HasItemCount(requirement->id, 1, true))
                continue;
            if (bot->StoreNewItemInBestSlots(requirement->id, 1))
                LogEvent(party, "access_granted",
                         JsonObject().Str("name", bot->GetName()).Str("kind", "item").Num("id", requirement->id));
        }

        for (ProgressionRequirement const* requirement : access->achievements)
        {
            if (!applies(requirement) || bot->HasAchieved(requirement->id))
                continue;
            if (AchievementEntry const* achievement = sAchievementStore.LookupEntry(requirement->id))
            {
                bot->CompletedAchievement(achievement);
                LogEvent(party, "access_granted",
                         JsonObject().Str("name", bot->GetName()).Str("kind", "achievement")
                             .Num("id", requirement->id));
            }
        }
    }

    bool TestHarnessMgr::LoadEntrances(Party& party)
    {
        ContentDef const* content = party.content;
        if (!party.entrances.empty() || !content)
            return !party.entrances.empty();

        if (content->worldBoss)
        {
            // [world_boss] the boss's live copy on the content's map (Highlord Kruul: the pooled spot he stands on),
            // else its first spawn there. The harness ticks on the world thread outside the map updates.
            ObjectGuid::LowType spawnId = 0;
            Map* map = sMapMgr->FindBaseMap(content->mapId);
            if (map)
                for (auto const& [id, creature] : map->GetCreatureBySpawnIdStore())
                    if (creature && creature->GetEntry() == content->worldBoss && creature->IsAlive())
                    {
                        spawnId = id;
                        break;
                    }
            // a pooled boss not loaded yet: the spot its pool spawned (the other spots of the pool hold nothing)
            auto const poolSpot = [map](ObjectGuid::LowType id)
            {
                return !sPoolMgr->IsPartOfAPool<Creature>(id) ||
                       (map && map->GetPoolData().IsSpawnedObject<Creature>(id));
            };
            CreatureData const* boss = nullptr;
            for (auto const& [id, data] : sObjectMgr->GetAllCreatureData())
                if (data.id == content->worldBoss && data.mapid == content->mapId &&
                    (spawnId ? id == spawnId : poolSpot(id)) && (!boss || id < boss->spawnId))
                    boss = &data;
            if (!boss)
                return false;
            party.worldBossSpawn = boss->spawnId;
            // the stand-off spot: the nearest other creature spawn WORLD_BOSS_STANDOFF_MIN..MAX yd from the boss - a
            // point on the ground the boss does not aggro from; none: the boss's own spot
            constexpr float WORLD_BOSS_STANDOFF_MIN = 35.0f;
            constexpr float WORLD_BOSS_STANDOFF_MAX = 90.0f;
            Position spot(boss->posX, boss->posY, boss->posZ, boss->orientation);
            float best = WORLD_BOSS_STANDOFF_MAX;
            for (auto const& [id, data] : sObjectMgr->GetAllCreatureData())
            {
                if (data.mapid != boss->mapid || data.id == content->worldBoss)
                    continue;
                float const dx = data.posX - boss->posX;
                float const dy = data.posY - boss->posY;
                float const dist = std::sqrt(dx * dx + dy * dy);
                if (dist >= WORLD_BOSS_STANDOFF_MIN && dist < best && std::fabs(data.posZ - boss->posZ) < 15.0f)
                {
                    best = dist;
                    spot.Relocate(data.posX, data.posY, data.posZ, std::atan2(-dy, -dx));
                }
            }
            party.entrances.push_back(spot);
            LogEvent(party, "world_boss_spot", JsonObject().Num("entry", content->worldBoss)
                                                   .Num("spawn", int64(boss->spawnId)).Num("map", content->mapId)
                                                   .Num("x", spot.GetPositionX()).Num("y", spot.GetPositionY())
                                                   .Num("z", spot.GetPositionZ()).Num("standoff", best));
            return true;
        }

        if (!content->entrances.empty())
        {
            // A wing: its own portals, in the order of the content table.
            for (uint32 triggerId : content->entrances)
                if (AreaTriggerTeleport const* teleport = sObjectMgr->GetAreaTriggerTeleport(triggerId))
                    if (teleport->target_mapId == content->mapId)
                        party.entrances.emplace_back(teleport->target_X, teleport->target_Y, teleport->target_Z,
                                                     teleport->target_Orientation);
        }
        else if (QueryResult result = WorldDatabase.Query(
                     "SELECT target_position_x, target_position_y, target_position_z, target_orientation, Name "
                     "FROM areatrigger_teleport WHERE target_map = {} ORDER BY ID",
                     content->mapId))
        {
            do
            {
                Field* fields = result->Fetch();
                std::string const name = fields[4].Get<std::string>();
                if (name.find("Exit") != std::string::npos || name.find("exit") != std::string::npos)
                    continue;
                party.entrances.emplace_back(fields[0].Get<float>(), fields[1].Get<float>(), fields[2].Get<float>(),
                                             fields[3].Get<float>());
            } while (result->NextRow());
        }
        return !party.entrances.empty();
    }

    bool TestHarnessMgr::PrepareWorldBoss(Party& party)
    {
        // [world_boss] test preparation of the harness test bots (the coordinator's OK 2026-11-08): the boss of the
        // run alive. World thread, outside the map updates (TestHarnessScripts: WorldScript OnUpdate).
        ContentDef const* content = party.content;
        Map* map = content && party.worldBossSpawn ? sMapMgr->FindBaseMap(content->mapId) : nullptr;
        if (!map)
            return false;
        char const* what = "alive";
        bool alive = false;
        auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(party.worldBossSpawn);
        if (bounds.first != bounds.second)
        {
            Creature* boss = bounds.first->second;
            if (boss && !boss->IsAlive())
            {
                boss->Respawn(true);
                what = "respawned";
            }
            alive = boss && boss->IsAlive();
        }
        else if (map->GetCreatureRespawnTime(party.worldBossSpawn))
        {
            map->RemoveCreatureRespawnTime(party.worldBossSpawn);  // the grid spawns it when the group arrives
            what = "respawn timer dropped";
        }
        else
            what = "not loaded";
        if (party.worldBossPrepState != what)
        {
            party.worldBossPrepState = what;
            LogEvent(party, "world_boss_prep", JsonObject().Num("entry", content->worldBoss)
                                                   .Num("spawn", int64(party.worldBossSpawn)).Str("state", what));
        }
        return alive;
    }

    void TestHarnessMgr::TickGuildEnter(Party& party)
    {
        ContentDef const* content = party.content;
        if (!LoadEntrances(party))
        {
            Finish(party, "failed", "no entrance in areatrigger_teleport");
            return;
        }

        // Once: every member's own walk in through the portal (its "instance portal" with this goal), as a leader
        // says "we go in through that portal". A wing goes in through its own entrance (exact goal).
        if (!party.teleportTries)
        {
            ++party.teleportTries;
            Position const& entrance = party.entrances.front();
            for (Member& member : party.members)
            {
                SelfTravel::MoveReport stale;
                while (SelfTravel::TakeReport(member.guid, stale))
                {
                }
                Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
                if (!bot || !GET_PLAYERBOT_AI(bot))
                    continue;  // a human walks in himself; the group waits for him
                SelfTravel::SetPortalGoal(member.guid, {content->mapId, entrance.GetPositionX(),
                                                        entrance.GetPositionY(), entrance.GetPositionZ(),
                                                        !content->entrances.empty()});
                member.hasPortalGoal = true;
            }
            LogEvent(party, "guild_enter", JsonObject().Num("five", party.guildFiveId).Str("content", content->key)
                                               .Num("members", int64(party.members.size())));
        }

        bool all = true;
        uint32 instanceId = 0;
        bool mixed = false;
        for (Member& member : party.members)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!bot)
            {
                Finish(party, "failed", member.name + " offline before entering");
                return;
            }
            // what the member's own walk in could not do (a fact: the core refused it, or no walk there exists)
            SelfTravel::MoveReport report;
            while (SelfTravel::TakeReport(member.guid, report))
                if (report.action == "move to instance portal" && bot->IsAlive())
                {
                    LogCannotMove(party, bot, report.action, report.reason, report.toX, report.toY, report.toZ);
                    Finish(party, "failed", member.name + " cannot walk in: " + report.reason);
                    return;
                }
            if (bot->IsBeingTeleported() || !bot->IsInWorld() || bot->GetMapId() != content->mapId)
            {
                all = false;
                continue;
            }
            if (!bot->m_InstanceValid && bot->GetGroup() && bot->GetGroup()->GetGUID() == party.groupGuid)
                bot->m_InstanceValid = true;  // as TickTeleport: in the group that owns this instance
            uint32 const id = bot->GetMap()->GetInstanceId();
            if (instanceId && id != instanceId)
                mixed = true;
            instanceId = id;
        }
        if (mixed)
        {
            Finish(party, "failed", "members in different instances of " + std::string(content->name));
            return;
        }
        if (!all)
            return;

        Player* leader = ObjectAccessor::FindPlayer(party.leader);
        if (!leader)
            return;
        for (Member& member : party.members)
        {
            SelfTravel::ClearPortalGoal(member.guid);  // inside: from here the leader leads (re-entries its own)
            member.hasPortalGoal = false;
            SelfTravel::MoveReport stale;
            while (SelfTravel::TakeReport(member.guid, stale))
            {
            }
        }
        EnterInstance(party, leader);
    }

    std::string TestHarnessMgr::StartGuildRun(std::string const& contentKey,
                                              std::vector<std::pair<ObjectGuid, std::string>> const& members,
                                              ObjectGuid leader, uint32 fiveId, std::string const& runId,
                                              std::string& error)
    {
        ContentDef const* content = FindContent(contentKey);
        if (!content || content->raid || content->size != members.size())
        {
            error = "no " + std::to_string(members.size()) + "-player content '" + contentKey + "'";
            return "";
        }
        for (auto const& party : _parties)
            if (!IsTerminal(party->phase) && party->guild && party->guildFiveId == fiveId)
            {
                error = "five " + std::to_string(fiveId) + " already runs " + party->id;
                return "";
            }

        // The dungeon leader reads each member's spec (its role, its class kit): the bot's own talent tree.
        auto specFor = [](Player* bot, std::string const& role) -> SpecDef const*
        {
            Role const want = role == "tank" ? Role::Tank : role == "healer" ? Role::Healer : Role::Melee;
            auto fits = [&](SpecDef const& spec)
            {
                if (want == Role::Tank || want == Role::Healer)
                    return spec.role == want;
                return spec.role != Role::Tank && spec.role != Role::Healer;
            };
            uint8 const tab = AiFactory::GetPlayerSpecTab(bot);
            SpecDef const* first = nullptr;
            SpecDef const* any = nullptr;
            uint32 index = 0;
            for (SpecDef const& spec : Specs())
            {
                if (spec.cls != bot->getClass())
                    continue;
                if (!any)
                    any = &spec;
                // the class's specs follow its talent tabs (the druid's feral tab is cat and bear)
                uint32 const specTab = spec.cls == CLASS_DRUID ? (index >= 2 ? index - 1 : index) : index;
                ++index;
                if (!fits(spec))
                    continue;
                if (specTab == tab)
                    return &spec;
                if (!first)
                    first = &spec;
            }
            return first ? first : any;
        };

        auto party = std::make_unique<Party>();
        _guildComps.push_back(Composition{"G5", uint8(members.size()), false, "guild five", {}});
        Composition& comp = _guildComps.back();
        party->comp = &comp;
        party->content = content;
        party->guild = true;
        party->guildFiveId = fiveId;
        party->createdMs = getMSTime();
        party->phaseSinceMs = party->createdMs;
        party->nextTickMs = party->createdMs;
        party->leader = leader;
        uint8 level = 1;
        for (auto const& [guid, role] : members)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
            if (!bot || !bot->IsInWorld())
            {
                error = "member " + guid.ToString() + " not in the world";
                return "";
            }
            SpecDef const* spec = specFor(bot, role);
            if (!spec)
            {
                error = bot->GetName() + ": no spec of its class";
                return "";
            }
            Member member;
            member.guid = guid;
            member.name = bot->GetName();
            member.cls = bot->getClass();
            member.spec = spec;
            member.slot = CompSlot{spec->key, 1, uint8(guid == leader ? SLOT_MAIN_TANK : 0)};
            comp.slots.push_back(member.slot);
            // what the character really is (no preparation): its level and its worn item level
            member.prep.ok = true;
            member.prep.level = bot->GetLevel();
            uint32 items = 0;
            float sum = 0.0f;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    sum += float(item->GetTemplate()->ItemLevel);
                    ++items;
                }
            member.prep.avgIlvl = items ? sum / float(items) : 0.0f;
            member.prepared = true;
            level = std::max<uint8>(level, bot->GetLevel());
            party->members.push_back(member);
        }
        party->level = level;
        party->targetIlvl = 0;
        party->seq = _nextSeq++;
        // [guild70] the five-leader's run id (its decision log and the party's events share it)
        party->id = runId.empty() ? Acore::StringFormat("{}-{}-guild{}-{}", Stamp(), content->key, fiveId, party->seq)
                                  : runId;
        // [guild70] the five-leader grouped the five and took it in (LeaderDungeonRun): the clearing starts at the
        // entering check (every member inside, one instance), no grouping, no teleport
        Player* leaderPlayer = ObjectAccessor::FindConnectedPlayer(leader);
        Group* group = leaderPlayer ? leaderPlayer->GetGroup() : nullptr;
        if (!group || group->GetLeaderGUID() != leader)
        {
            error = "the five-leader leads no group";
            return "";
        }
        party->groupGuid = group->GetGUID();
        ApplyStrategies(*party);
        SetPhase(*party, Phase::Entering);
        LOG_INFO("playerbots", "[guild70] five {} clears {} as party {} (leader {})", fiveId, content->key, party->id,
                 leader.ToString());
        _parties.push_back(std::move(party));
        return _parties.back()->id;
    }

    void TestHarnessMgr::StopGuildRun(uint32 fiveId, std::string const& reason)
    {
        for (auto const& party : _parties)
            if (!IsTerminal(party->phase) && party->guild && party->guildFiveId == fiveId)
                Finish(*party, "stopped", reason);
    }

    std::string TestHarnessMgr::GuildRunPhase(uint32 fiveId) const
    {
        for (auto const& party : _parties)
            if (!IsTerminal(party->phase) && party->guild && party->guildFiveId == fiveId)
                return PhaseName(party->phase);
        return "";
    }

    // [guild70] the five-leader's run (Ai/Leader/Dungeon/LeaderDungeonRun.h) has its instance cleared by this harness's
    // party state machine in honest mode (Party::guild: no preparation, test setup, handouts or teleports) until the
    // clearing itself lives in Ai/Leader. Registered once at startup.
    namespace
    {
        class HarnessClearEngine : public Leader::Dungeon::ClearEngine
        {
        public:
            bool Begin(Leader::Dungeon::FiveRunRequest const& request, std::string const& runId,
                       std::string& error) override
            {
                std::vector<std::pair<ObjectGuid, std::string>> members;
                members.emplace_back(request.leader, "tank");
                for (Leader::Dungeon::FiveRunMember const& member : request.members)
                {
                    if (member.guid == request.leader)
                        members.front().second = member.role;
                    else
                        members.emplace_back(member.guid, member.role);
                }
                return !TestHarnessMgr::Instance()
                            .StartGuildRun(request.contentKey, members, request.leader, request.fiveId, runId, error)
                            .empty();
            }
            std::string Phase(uint32 fiveId) const override
            {
                return TestHarnessMgr::Instance().GuildRunPhase(fiveId);
            }
            void Stop(uint32 fiveId, std::string const& reason) override
            {
                TestHarnessMgr::Instance().StopGuildRun(fiveId, reason);
            }
        };

        HarnessClearEngine harnessClearEngine;
        bool const harnessClearEngineSet =
            (Leader::Dungeon::DungeonRuns::Instance().SetEngine(&harnessClearEngine), true);
    }

    bool TestHarnessMgr::TickTeleport(Party& party)
    {
        ContentDef const* content = party.content;
        uint32 const now = getMSTime();

        if (!LoadEntrances(party))
        {
            Finish(party, "failed", "no entrance in areatrigger_teleport");
            return false;
        }

        if (!party.teleportTries)
        {
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                {
                    GrantAccess(party, bot);
                    // A fresh instance, as the dungeon finder gives one: a heroic save of an earlier run outlives the
                    // test preparation (Scarlet Monastery heroic wave 8 entered instance 66 of the 00:06 run - Vishas
                    // and Thalnos lay dead from it, "dead, kill credit not observed"). Members still inside that map
                    // keep theirs (an unbind there starts the homebind timer, wave 7).
                    // [world_boss] an open-world map has no instance and no bind: a member on it is ported below
                    if (content->worldBoss)
                        continue;
                    if (bot->GetMapId() != content->mapId)
                        sInstanceSaveMgr->PlayerUnbindInstance(bot->GetGUID(), content->mapId,
                                                               Difficulty(content->difficulty), true, bot);
                    else
                    {
                        // Still inside an earlier run's instance (HoR-n wave 15: logged in inside instance 4 of the
                        // 05:17 run, Falric and Marwyn long dead; after a corpse run the re-entry made instance 48
                        // and the run failed "instance bind lost"). Out to the homebind first (test preparation),
                        // then the same fresh instance as the others.
                        party.staleInstances[bot->GetGUID()] = bot->GetMap()->GetInstanceId();
                        bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY,
                                        bot->m_homebindZ, bot->GetOrientation());
                    }
                }
            PrepareEventItems(party, ObjectAccessor::FindPlayer(party.leader), true);
            if (content->worldBoss)
                PrepareWorldBoss(party);
        }

        Position const& entrance = party.entrances[party.entranceIdx];
        // [world_boss] on an open-world map a member counts as arrived only near the stand-off spot
        constexpr float WORLD_BOSS_ARRIVED_YD = 40.0f;
        bool all = true;
        uint32 instanceId = 0;
        bool mixed = false;
        for (Member const& member : party.members)
        {
            // [lead443] FindPlayer is null while a member is out of the world in a far teleport (the homebind hop out
            // of a stale instance above): that is not offline, the check below waits for it
            Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!bot)
            {
                Finish(party, "failed", member.name + " offline before entering");
                return false;
            }

            if (bot->IsBeingTeleported() || !bot->IsInWorld())
            {
                all = false;
                continue;
            }

            if (bot->GetMapId() != content->mapId ||
                (content->worldBoss && bot->GetExactDist2d(entrance.GetPositionX(), entrance.GetPositionY()) >
                                           WORLD_BOSS_ARRIVED_YD))
            {
                all = false;
                // Out of the stale instance: its temporary bind goes, as for the members that were outside.
                bool ejected = false;
                if (auto const stale = party.staleInstances.find(member.guid); stale != party.staleInstances.end())
                {
                    ejected = true;
                    InstancePlayerBind const* bind = sInstanceSaveMgr->PlayerGetBoundInstance(
                        bot->GetGUID(), content->mapId, Difficulty(content->difficulty));
                    if (bind && bind->save && !bind->perm && bind->save->GetInstanceId() == stale->second)
                        sInstanceSaveMgr->PlayerUnbindInstance(bot->GetGUID(), content->mapId,
                                                               Difficulty(content->difficulty), true, bot);
                    party.staleInstances.erase(stale);
                }
                if (ejected || !party.teleportTries ||
                    getMSTimeDiff(party.phaseSinceMs, now) > TELEPORT_TIMEOUT_MS / 2)
                {
                    if (!bot->TeleportTo(content->mapId, entrance.GetPositionX(), entrance.GetPositionY(),
                                         entrance.GetPositionZ(), entrance.GetOrientation()))
                        LogEvent(party, "teleport_refused", JsonObject().Str("name", bot->GetName()));
                }
                continue;
            }

            // Wave 7 (ramps, sp, sh, up, cos, bm, dmw ejected at 51-57 s): a member still inside the previous run's
            // dungeon left its old group in TickGrouping, and Group::_homebindIfInstance cleared m_InstanceValid. The
            // flag is reset only by a port to a non-instance map (HandleMoveWorldportAck), never by this
            // instance-to-instance teleport, so Player::UpdateHomebindTime ported it out after 60 s and the port
            // dropped its temporary bind: the leader came back into a NEW instance and Retarget saw nothing. The
            // member is in the group that owns this instance now, as Group::_cancelHomebindIfInstance accepts.
            if (!bot->m_InstanceValid && bot->GetGroup() && bot->GetGroup()->GetGUID() == party.groupGuid)
            {
                bot->m_InstanceValid = true;
                LogEvent(party, "instance_valid_restored", JsonObject().Str("name", bot->GetName())
                                                               .Num("instanceId", bot->GetMap()->GetInstanceId()));
            }

            uint32 const id = bot->GetMap()->GetInstanceId();
            if (instanceId && id != instanceId)
                mixed = true;
            instanceId = id;
        }

        if (!party.teleportTries || getMSTimeDiff(party.phaseSinceMs, now) > TELEPORT_TIMEOUT_MS / 2)
        {
            ++party.teleportTries;
            if (party.teleportTries > 1)
                party.phaseSinceMs = now;
        }

        if (mixed)
            LogEvent(party, "instance_mismatch", JsonObject().Str("note", "members in different instances"));

        if (all && !mixed)
        {
            if (Player* leader = ObjectAccessor::FindPlayer(party.leader))
            {
                // While the setup teleport ran, members still outside saw their leader inside and their own
                // "instance portal" tried to walk there from wherever they stood: that is no gap of the run.
                for (Member const& member : party.members)
                {
                    SelfTravel::MoveReport stale;
                    SelfTravel::TakeReport(member.guid, stale);
                }
                // [b342] the world boss's grid is loaded now that the group stands near it: the prep again; a boss
                // whose respawn timer was just dropped appears with the next grid update - the run starts then
                if (content->worldBoss && !PrepareWorldBoss(party))
                {
                    constexpr uint32 WORLD_BOSS_APPEAR_MS = 30 * IN_MILLISECONDS;
                    if (!party.worldBossWaitMs)
                        party.worldBossWaitMs = now ? now : 1;
                    else if (getMSTimeDiff(party.worldBossWaitMs, now) > WORLD_BOSS_APPEAR_MS)
                        Finish(party, "failed", "the world boss did not appear at its spawn");
                    return false;
                }
                EnterInstance(party, leader);
                return true;
            }
        }

        if (party.teleportTries > 4)
            Finish(party, "failed", "could not enter the instance (access, lockout or group size)");
        return false;
    }

    namespace
    {
        constexpr uint32 AGGRO_LOSS_MS = 2 * IN_MILLISECONDS;
        // [b33_sh2] for [b33_brd] (brd-n 20261026-000434-brd-n-5F-6): the core flags a pet's owner only with
        // UNIT_FLAG_PET_IN_COMBAT, never IN_COMBAT, so a train chasing a hunter's pet was no fight for the harness
        // (pull_end of #59 at 3026 s while the mobs still walked in, the next pull planned into them). A member's pet
        // or guardian with a live, non-evading creature attacker counts as fighting, as IsPetFighting does for the
        // bots (ChooseTargetActions.cpp), at most PET_FIGHT_MAX_MS per episode (an unreachable chaser lets go).
        constexpr uint32 PET_FIGHT_MAX_MS = 60 * IN_MILLISECONDS;
        bool HarnessPetFighting(Player* owner, uint32 now)
        {
            static std::unordered_map<uint64, uint32> fightSince;  // owner guid -> first seen fighting, world thread
            bool attacked = false;
            for (Unit* controlled : owner->m_Controlled)
            {
                if (!controlled || !controlled->IsInWorld() || !controlled->IsAlive() || !controlled->IsInCombat())
                    continue;
                for (Unit* attacker : controlled->getAttackers())
                {
                    Creature* creature = attacker ? attacker->ToCreature() : nullptr;
                    if (creature && creature->IsAlive() && !creature->IsInEvadeMode())
                    {
                        attacked = true;
                        break;
                    }
                }
                if (attacked)
                    break;
            }
            uint64 const key = owner->GetGUID().GetRawValue();
            if (!attacked)
            {
                fightSince.erase(key);
                return false;
            }
            auto const [it, inserted] = fightSince.try_emplace(key, now ? now : 1);
            return inserted || getMSTimeDiff(it->second, now) < PET_FIGHT_MAX_MS;
        }
        constexpr uint32 AWAY_MAX_MS = 20 * MINUTE * IN_MILLISECONDS;   // a dead / outside member must be back
        // Longest gap between two boss kills in the "done" runs of waves 1-3 without a wipe: 16.4 min (zf-h), with a
        // wipe 21.4 (fos-h, the corpse run is not counted here); no raid "done" runs yet, raids get more room.
        constexpr uint32 NO_BOSS_PROGRESS_DUNGEON_MS = 25 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 NO_BOSS_PROGRESS_RAID_MS = 45 * MINUTE * IN_MILLISECONDS;
        // [guard_kill_progress] The death of a guard of a door that opens once its own guards are dead (GuardedDoorDef
        // without guarded objects: Sunken Temple's six defenders before Jammal'an's forcefield, Uldaman's Stone
        // Keepers) is boss progress: the encounter's gate (instance data DATA_DEFENDER_KILLED). st-n build 23
        // (103125, 103436 -2 / -3): Atal'alarion at 1096-1143 s, the six defenders dead by 2304-2402 s, the
        // forcefield open, the group on the z -91 ring 130-250 yd from Jammal'an when no_boss_progress (25 min from
        // Atal'alarion) ended all three runs at 2597-2643 s.
        constexpr bool GUARD_KILL_PROGRESS = true;
        // [guarded_object_progress] ...and a guarded object leaving its guarded state (Dire Maul West: a Crystal
        // Generator going down once its Arcane Aberrations / Mana Remnants within 70 yd are dead). dmw-n
        // 20261025-123432: Tendris at 772 s, the five generators cleared by ~1948 s, Immol'thar engaged at 2164 s,
        // then "no boss progress for 25 min" at 2272 s, 3/5.
        constexpr uint32 GUARDED_OBJECT_CHECK_MS = 5 * IN_MILLISECONDS;
        constexpr uint32 BOSS_ATTEMPT_GRACE_MS = 5 * MINUTE * IN_MILLISECONDS;  // a running attempt outlasts it
        constexpr uint32 BOSS_ATTEMPT_LOOP_MS = 5 * MINUTE * IN_MILLISECONDS;  // a boss attempt fed by trash kills
        // [queue-scripted-event-loop] Scripted wave events whose trash IS the encounter: the loop window is the
        // event's own length while the script slot stays IN_PROGRESS (the script resets it to NOT_STARTED when the
        // event fails). hos-n 20261025-132016-hos-n-5A-15: Tribunal of Ages (599 slot BOSS_TRIBUNAL_OF_AGES 2)
        // IN_PROGRESS at 1276 s, boss_attempt_loop "5 min, 37 trash kills" at 1577 s - brann_bronzebeard.cpp: "3
        // phases, each ~100 s" plus the transition talks, the heads are TEMPSUMMON_TIMED_DESPAWN 580 s.
        struct ScriptedEventLoopDef
        {
            uint32 mapId;
            uint32 bossId;
            uint32 windowMs;
        };
        constexpr std::array<ScriptedEventLoopDef, 2> SCRIPTED_EVENT_LOOP = {{
            {599, 2, 10 * MINUTE * IN_MILLISECONDS},  // Halls of Stone: Tribunal of Ages (Brann's console event)
            {601, 1, 8 * MINUTE * IN_MILLISECONDS},   // Azjol-Nerub: Hadronox (the window opens at Crusher contact)
        }};
        constexpr uint32 BOSS_PHANTOM_LOG_MS = 60 * IN_MILLISECONDS;  // boss health unchanged in an attempt: logged
        constexpr uint32 BOSS_PHANTOM_END_MS = 3 * MINUTE * IN_MILLISECONDS;  // then the run ends (5-man only)
        constexpr uint32 TRASH_PHANTOM_LOG_MS = 60 * IN_MILLISECONDS;  // a trash fight: no mob lost health, logged
        constexpr uint32 TRASH_PHANTOM_END_MS = 3 * MINUTE * IN_MILLISECONDS;  // then the run ends (5-man only)
        // Escape encounters whose boss is never damaged (the progress is the escort NPC and its waves).
        constexpr std::array<uint32, 2> BOSS_PHANTOM_EXEMPT = {36954, 37226};  // HoR: The Lich King (escape)
        constexpr float TRASH_LOOP_RADIUS = 60.0f;
        constexpr uint32 TRASH_LOOP_DUNGEON_MS = 6 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 TRASH_LOOP_RAID_MS = 10 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 TRASH_LOOP_MIN_KILLS = 15;
        constexpr uint32 TRASH_LOOP_MIN_PROGRESS = 5;  // [trash_loop_progress] spawned mobs fewer: no loop
        constexpr uint32 BOSSES_DONE_COMBAT_MS = 20 * IN_MILLISECONDS;

        // "entry name x count" of the most killed creatures, for the loop / no-progress diagnosis.
        std::string TopKills(std::map<uint32, uint32> const& kills, uint32 top)
        {
            std::vector<std::pair<uint32, uint32>> sorted(kills.begin(), kills.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](auto const& left, auto const& right) { return left.second > right.second; });
            std::string text;
            for (size_t index = 0; index < sorted.size() && index < top; ++index)
            {
                CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(sorted[index].first);
                text += Acore::StringFormat("{}{} {} x{}", text.empty() ? "" : ", ", sorted[index].first,
                                            info ? info->Name : "?", sorted[index].second);
            }
            return text;
        }

        std::string LowerTrimmed(std::string text)
        {
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // DungeonEncounter.dbc names follow the creature name ("Bronjahm ", "Sjonnir the Ironshaper" vs "Sjonnir
        // The Ironshaper", "Skarvald & Dalronn"): one contains the other, ignoring case and trailing blanks.
        bool EncounterNameMatches(std::string const& creatureName, std::string const& encounterName)
        {
            std::string const creature = LowerTrimmed(creatureName);
            std::string const encounter = LowerTrimmed(encounterName);
            if (creature.size() < 4 || encounter.size() < 4)
                return false;
            return creature.find(encounter) != std::string::npos || encounter.find(creature) != std::string::npos;
        }

        bool IsBossLike(Creature const* creature) { return creature->IsDungeonBoss() || creature->isWorldBoss(); }

        // DungeonEncounter.dbc order of the encounter whose kill credit is this creature entry, -1 for none.
        int32 BossOrderOfEntry(Party const& party, uint32 entry)
        {
            for (EncounterInfo const& info : party.encounters)
                if (std::find(info.credits.begin(), info.credits.end(), entry) != info.credits.end())
                    return int32(info.index);
            return -1;
        }
    }

    void TestHarnessMgr::LoadEncounters(Party& party, Map* map)
    {
        party.encounters.clear();
        uint32 const mapId = map->GetId();
        uint32 difficulty = uint32(map->GetDifficulty());

        auto collect = [&party, mapId](uint32 wanted)
        {
            for (uint32 row = 0; row < sDungeonEncounterStore.GetNumRows(); ++row)
            {
                DungeonEncounterEntry const* entry = sDungeonEncounterStore.LookupEntry(row);
                if (!entry || entry->mapId != mapId || entry->difficulty != wanted)
                    continue;

                EncounterInfo info;
                info.index = entry->encounterIndex;
                info.name = entry->encounterName[LOCALE_enUS] ? entry->encounterName[LOCALE_enUS] : "";
                // [event_boss_tbcwotlk] DungeonEncounter.dbc's own typo (Pit of Saron row 837 / 838, credit 36658
                // Scourgelord Tyrannus): reports and the gaps list read "Overlrod Tyrannus"; the credit was right.
                if (info.name == "Overlrod Tyrannus")
                    info.name = "Overlord Tyrannus";
                party.encounters.push_back(info);
            }
        };

        collect(difficulty);
        // Classic and TBC maps run as heroics here; DungeonEncounter.dbc only lists their normal mode.
        if (party.encounters.empty() && difficulty)
        {
            difficulty = 0;
            collect(difficulty);
        }

        if (DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(mapId, Difficulty(difficulty)))
            for (DungeonEncounter const* encounter : *list)
                if (encounter && encounter->dbcEntry && encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE)
                    for (EncounterInfo& info : party.encounters)
                        if (info.index == encounter->dbcEntry->encounterIndex)
                        {
                            info.credits.push_back(encounter->creditEntry);
                            // The other team's twin the instance swaps in (The Nexus: Stoutbeard -> Kolurg).
                            for (uint32 alias : EncounterCreditAliases(mapId, encounter->creditEntry))
                                info.credits.push_back(alias);
                        }

        std::sort(party.encounters.begin(), party.encounters.end(),
                  [](EncounterInfo const& left, EncounterInfo const& right) { return left.index < right.index; });
    }

    int32 TestHarnessMgr::BossOrder(Party const& party, Creature const* creature) const
    {
        if (!creature)
            return -1;

        for (BossTrack const& boss : party.bosses)
            if (boss.entry == creature->GetEntry() && boss.order >= 0)
                return boss.order;

        for (EncounterInfo const& info : party.encounters)
            if (std::find(info.credits.begin(), info.credits.end(), creature->GetEntry()) != info.credits.end())
                return int32(info.index);

        for (EncounterInfo const& info : party.encounters)
            if (EncounterNameMatches(creature->GetName(), info.name))
                return int32(info.index);

        return -1;
    }

    // [b31_dg] A script slot whose IN_PROGRESS starts with a gauntlet of adds before the boss joins: the "wave
    // encounter" fallback (any member in combat = engaged) must not count the gauntlet as the boss. Azjol-Nerub's
    // DATA_HADRONOX goes IN_PROGRESS when Crusher pack 1 engages (boss_hadronox.cpp ACTION_CRUSHER_ENGAGED), Hadronox
    // climbs passive for 70+ s and the door stream runs: an-n 20261025-184229-an-n-5G-32 logged 14 "boss 1" attempts,
    // all "reset", all gauntlet with Hadronox engaged by herself (boss_failed after 3). Checked against the other
    // fallback attempts of 2026-10-25 (VH waves, HoS Tribunal / Sjonnir events, CoS / BM / Arcatraz event slots, ToC5,
    // Mechanar): those are real event encounters. Krik'thir's slot only goes IN_PROGRESS on his own engage (BossAI
    // _JustEngagedWith), so his attempts never come from the fallback.
    bool TestHarnessMgr::GauntletSlot(Party const& party, BossTrack const& boss) const
    {
        constexpr uint32 AZJOL_NERUB_MAP = 601;
        constexpr uint32 AN_DATA_HADRONOX = 1;
        return !boss.dbc && party.content && party.content->mapId == AZJOL_NERUB_MAP &&
               boss.bossId == AN_DATA_HADRONOX;
    }

    // Finds the creature of a dbc track by its kill-credit entry. Script slots are bound by entry on engage / kill
    // (TickRun), never by GetGuidData(bossId) (a DATA_* id, not the boss id).
    void TestHarnessMgr::ResolveBoss(Party& party, BossTrack& boss, Map* map, InstanceScript* /*script*/)
    {
        if (!boss.creature.IsEmpty() || !map)
            return;

        uint32 const now = getMSTime();
        if (boss.nextResolveMs && int32(now - boss.nextResolveMs) < 0)
            return;
        boss.nextResolveMs = now + 10 * IN_MILLISECONDS;

        Creature* creature = nullptr;
        if (boss.dbc)
        {
            if (boss.credits.empty())
                return;
            for (auto const& [spawnId, candidate] : map->GetCreatureBySpawnIdStore())
                if (candidate && std::find(boss.credits.begin(), boss.credits.end(), candidate->GetEntry()) !=
                                     boss.credits.end())
                {
                    creature = candidate;
                    break;
                }
            // [b30_mech_pos] a summoned boss has no spawn id, so the store above never holds it (zf-h
            // 20261025-184229: the Gong's Gahz'rilla 7273 stayed "Ghaz'rilla, entry 0", fought 287 s as a "trash"
            // pull with no boss_engaged, no phantom / attempt watchdog). Looked up around the leader instead.
            Player* leader = creature ? nullptr : ObjectAccessor::FindPlayer(party.leader);
            if (leader && leader->GetMap() == map)
            {
                constexpr float SUMMONED_BOSS_SCAN = 150.0f;
                for (uint32 entry : boss.credits)
                {
                    std::list<Creature*> found;
                    leader->GetCreatureListWithEntryInGrid(found, entry, SUMMONED_BOSS_SCAN);
                    for (Creature* candidate : found)
                        if (candidate && candidate->IsAlive() && candidate->IsSummon())
                        {
                            creature = candidate;
                            break;
                        }
                    if (creature)
                    {
                        party.bossCreatures.insert(creature->GetGUID());
                        LogEvent(party, "summoned_boss_bound", JsonObject().Num("bossId", boss.bossId)
                                                                   .Str("boss", boss.name)
                                                                   .Str("name", creature->GetName())
                                                                   .Num("entry", creature->GetEntry()));
                        break;
                    }
                }
            }
        }
        // Script slots are not resolved here: GetGuidData takes the script's DATA_* id, which is not its boss id
        // (Shadow Labyrinth: GetGuidData(1) is Hellmaw, Hellmaw's boss slot is 0 - Hellmaw was counted twice and the
        // full clear waited for "boss 0"). A slot is bound by entry when the party engages or kills its creature.

        if (!creature)
            return;

        boss.creature = creature->GetGUID();
        boss.entry = creature->GetEntry();
        if (!boss.dbc)
            boss.name = creature->GetName();
        if (boss.order < 0)
            boss.order = BossOrder(party, creature);
    }

    void TestHarnessMgr::CloseLooseMob(Party& party, ObjectGuid /*guid*/, LooseMob& mob, char const* end)
    {
        if (!mob.logged)
            return;

        double const seconds = double(getMSTimeDiff(mob.sinceMs, getMSTime())) / double(IN_MILLISECONDS);
        LogEvent(party, "aggro_loss", JsonObject().Str("mob", mob.name).Num("entry", mob.entry).Bool("boss", mob.boss)
                                          .Str("victim", mob.victimName).Str("victimRole", mob.victimRole)
                                          .Real("durationSec", seconds).Str("end", end));
    }

    // Loose mobs: a hostile in combat with the party whose victim is a non-tank member for AGGRO_LOSS_MS or longer
    // counts once (aggro_loss, logged when it returns to a tank, switches victim, dies or leaves combat).
    void TestHarnessMgr::TrackAggro(Party& party, std::vector<Player*> const& players)
    {
        uint32 const now = getMSTime();
        std::set<ObjectGuid> seen;
        for (Player* bot : players)
        {
            Member* member = nullptr;
            for (Member& candidate : party.members)
                if (candidate.guid == bot->GetGUID())
                    member = &candidate;
            if (!member || !bot->IsAlive() || !bot->IsInCombat() || member->spec->role == Role::Tank)
                continue;

            for (Unit* attacker : bot->getAttackers())
            {
                Creature* mob = attacker ? attacker->ToCreature() : nullptr;
                // [lead217] b184 rfc-h 5D 900-937 / 1094-1124 s: Polymorphed Searing Blade Warlocks kept the member as
                // their victim, 36 s and 29 s of "aggro off the tank" with 0 / 2937 damage taken: a mob under the
                // group's crowd control hits nobody
                if (!mob || !mob->IsAlive() || mob->GetVictim() != bot || mob->IsTotem() || mob->IsCritter() ||
                    mob->GetOwnerGUID().IsPlayer() || IsCrowdControlled(mob))
                    continue;

                seen.insert(mob->GetGUID());
                auto itr = party.looseMobs.find(mob->GetGUID());
                if (itr != party.looseMobs.end() && itr->second.victim != bot->GetGUID())
                {
                    CloseLooseMob(party, itr->first, itr->second, "switched");
                    party.looseMobs.erase(itr);
                    itr = party.looseMobs.end();
                }

                if (itr == party.looseMobs.end())
                {
                    LooseMob loose;
                    loose.victim = bot->GetGUID();
                    loose.sinceMs = now;
                    loose.boss = IsBossLike(mob);
                    loose.entry = mob->GetEntry();
                    loose.name = mob->GetName();
                    loose.victimName = member->name;
                    loose.victimRole = RoleName(member->spec->role);
                    itr = party.looseMobs.emplace(mob->GetGUID(), loose).first;
                }

                if (!itr->second.logged && getMSTimeDiff(itr->second.sinceMs, now) >= AGGRO_LOSS_MS)
                {
                    itr->second.logged = true;
                    ++member->aggroPulled;
                    ++party.aggroLosses;
                }
            }
        }

        Player* leader = ObjectAccessor::FindPlayer(party.leader);
        Map* map = leader && leader->GetMap() && leader->GetMap()->GetInstanceId() == party.instanceId
                       ? leader->GetMap()
                       : nullptr;
        for (auto itr = party.looseMobs.begin(); itr != party.looseMobs.end();)
        {
            if (seen.count(itr->first))
            {
                ++itr;
                continue;
            }

            Creature* mob = map ? map->GetCreature(itr->first) : nullptr;
            char const* end = !mob || !mob->IsAlive() ? "died" : mob->IsInCombat() ? "returned" : "left combat";
            CloseLooseMob(party, itr->first, itr->second, end);
            itr = party.looseMobs.erase(itr);
        }
    }

    void TestHarnessMgr::EnterInstance(Party& party, Player* leader)
    {
        Map* map = leader->GetMap();
        party.instanceId = map->GetInstanceId();
        uint32 const now = getMSTime();
        party.runStartMs = now;
        party.lastProgressMs = now;
        party.lastHeartbeatMs = now;
        party.progressPos = leader->GetPosition();
        party.progressMapId = leader->GetMapId();
        party.heartbeatPos = leader->GetPosition();
        party.timeoutMs = party.content->raid ? RAID_TIMEOUT_MS : DUNGEON_TIMEOUT_MS;
        if (uint32 const budget = RunBudgetMinutes(party.content->key))  // [hleft-run-budget]
            party.timeoutMs = budget * MINUTE * IN_MILLISECONDS;
        party.lastBossProgressMs = now;
        party.loopSinceMs = now;
        party.loopAnchor = leader->GetPosition();

        StatsRegistry::Instance().TrackInstance(party.instanceId, party.seq);
        for (Member const& member : party.members)
            StatsRegistry::Instance().TrackMember(member.guid, party.seq);
        Leader::Orders::Track(party.leader);  // [lead1] the tank's pull orders are posted from now on

        // [hleft-daily-reset] heroics and raids are reset by the server at Instance.ResetTimeHour: every member is put
        // out and the bind dropped (2026-10-25 05:00: all ten WotLK / TBC heroics of the 04:47 batch, uk-h, hol-h,
        // up-h among them, "members_away" all outside at once, the re-entry made a new instance).
        party.instanceResetAt = party.content->difficulty != DUNGEON_DIFFICULTY_NORMAL || party.content->raid
                                    ? sInstanceSaveMgr->GetResetTimeFor(party.content->mapId,
                                                                        Difficulty(party.content->difficulty))
                                    : time_t(0);

        LoadEncounters(party, map);
        InstanceScript* script = map->ToInstanceMap() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
        if (script)
            for (uint32 bossId = 0; bossId < script->GetEncounterCount(); ++bossId)
            {
                BossTrack boss;
                boss.bossId = bossId;
                boss.lastState = uint8(script->GetBossState(bossId));
                boss.killed = boss.lastState == DONE;
                boss.name = "boss " + std::to_string(bossId);
                // [hleft-script-slot-names] the slot's name, creature and order from the instance enum
                for (ScriptSlotDef const* def : ScriptSlots(party.content->mapId, party.content->difficulty))
                    if (def->bossId == bossId)
                    {
                        boss.name = def->name;
                        boss.entry = def->entry;
                        boss.order = def->order;
                    }
                party.bosses.push_back(boss);
            }

        // One track per DungeonEncounter.dbc row, followed through its kill-credit creature and the completed-encounter
        // mask: every encounter of a dungeon when the InstanceScript keeps no boss states (classic maps, Pit of Saron,
        // Utgarde Pinnacle, Oculus), and in dungeons also next to the script's tracks - a script that keeps fewer
        // bosses than the dbc (The Stockade) must not end the run early. Twins of a script track are dropped once the
        // script track knows its creature (DropDbcTwins). Raids keep their script tracks only.
        bool const scriptTracks = !party.bosses.empty();
        if (!scriptTracks || !party.content->raid)
            for (EncounterInfo const& info : party.encounters)
            {
                BossTrack boss;
                boss.bossId = info.index;
                boss.dbc = true;
                boss.order = int32(info.index);
                boss.name = info.name;
                while (!boss.name.empty() && boss.name.back() == ' ')
                    boss.name.pop_back();
                boss.credits = info.credits;
                boss.killed = script && (script->GetCompletedEncounterMask() & (1u << info.index));
                boss.lastState = boss.killed ? uint8(DONE) : uint8(NOT_STARTED);
                party.bosses.push_back(boss);
            }

        // [world_boss] b346 wb-kazzak 25A-1: an open-world boss has no InstanceScript slot and no DungeonEncounter.dbc
        // row, so the run had no boss track - every pull was "trash", no boss attempt, and run_end read "Lord Kazzak -
        // alive, not engaged" after his fight. His own track: a dbc-style one credited by his entry (the kill), in
        // progress while he fights, his creature known from the spawn LoadEntrances found.
        if (party.content->worldBoss)
        {
            BossTrack boss;
            boss.bossId = 0;
            boss.dbc = true;
            boss.order = 0;
            boss.entry = party.content->worldBoss;
            boss.credits = {party.content->worldBoss};
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(party.content->worldBoss);
            boss.name = info ? info->Name : party.content->name;
            auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(party.worldBossSpawn);
            if (bounds.first != bounds.second && bounds.first->second)
            {
                boss.creature = bounds.first->second->GetGUID();
                boss.homePos = bounds.first->second->GetHomePosition();
            }
            boss.killed = false;
            boss.lastState = uint8(NOT_STARTED);
            party.bosses.push_back(boss);
        }

        for (BossTrack& boss : party.bosses)
            ResolveBoss(party, boss, map, script);
        DropDbcTwins(party);

        std::vector<uint32> const& wingBosses = party.content->bossEntries;
        auto inWing = [&wingBosses](uint32 entry)
        { return std::find(wingBosses.begin(), wingBosses.end(), entry) != wingBosses.end(); };
        // Other wings are told by their DungeonEncounter.dbc kill-credit creatures only (instance_encounters): a
        // boss-flagged event NPC (Lord Victor Nefarius next to Rend) must not cut its own wing in two.
        auto isEncounterCredit = [&party](uint32 entry)
        {
            for (EncounterInfo const& info : party.encounters)
                if (std::find(info.credits.begin(), info.credits.end(), entry) != info.credits.end())
                    return true;
            return false;
        };

        // Boss creatures come from the encounter data: instance_encounters credits (IsDungeonBoss) and the creatures
        // the script names for its boss ids. Rank 3 alone also marks event NPCs (ToC5 Tirion, Ymiron's ancestor
        // spirits, the Halls of Reflection Lich King): such a creature counts only when it can be fought, and in a
        // dungeon with encounter data not at all (DungeonEncounter.dbc lists every 5-man boss, rares included).
        std::set<ObjectGuid> scriptCreatures;
        for (BossTrack const& boss : party.bosses)
            if (!boss.creature.IsEmpty())
                scriptCreatures.insert(boss.creature);
        bool const encounterData = !party.encounters.empty() || scriptTracks;
        auto notABoss = [&](Creature* creature) -> char const*
        {
            if (creature->IsDungeonBoss() || IsVioletHoldPrisoner(creature) ||
                scriptCreatures.count(creature->GetGUID()) || isEncounterCredit(creature->GetEntry()))
                return nullptr;
            // [dm_shredder_required] EncounterCarrierCredit: its death brings the credit creature (Sneed's Shredder ->
            // Sneed): a required boss creature of the route, ordered by its encounter (BossOrder name match).
            if (uint32 const carried = EncounterCarrierCredit(party.content->mapId, creature->GetEntry()))
            {
                LogEvent(party, "dm_shredder_required", JsonObject().Str("name", creature->GetName())
                                                            .Num("entry", creature->GetEntry()).Num("credit", carried));
                return nullptr;
            }
            CreatureTemplate const* info = creature->GetCreatureTemplate();
            uint32 const blocking = UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC;
            if (creature->IsSummon())
                return "summoned";
            if (info->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER | CREATURE_FLAG_EXTRA_CIVILIAN))
                return "trigger / civilian";
            if ((info->unit_flags & blocking) || creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
                creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || creature->IsImmuneToPC())
                return "not attackable";
            if (creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) || creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                return "gossip / quest npc";
            if (creature->IsFriendlyTo(leader))
                return "friendly";
            if (encounterData && !party.content->raid)
                return "not in the encounter data";
            return nullptr;
        };

        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
        {
            if (!creature)
                continue;
            // Encounter creatures without the boss flag count too (Drak'Tharon wave 8: The Prophet Tharon'ja, rank 1,
            // no CREATURE_FLAG_EXTRA_DUNGEON_BOSS, his dbc row credited by spell - he was trash for the route, the run
            // stalled under his platform with "credit not observed"): a dbc credit entry or the encounter's name.
            bool encounterCreature = false;
            if (!party.content->raid)
            {
                encounterCreature = BossOrderOfEntry(party, creature->GetEntry()) >= 0;
                for (EncounterInfo const& info : party.encounters)
                    encounterCreature |= EncounterNameMatches(creature->GetName(), info.name);
            }
            bool const bossLike = creature->IsDungeonBoss() || creature->isWorldBoss() || encounterCreature ||
                                  IsVioletHoldPrisoner(creature);
            if (wingBosses.empty())
            {
                if (creature->IsAlive() && bossLike)
                {
                    if (char const* why = notABoss(creature))
                        party.bossCreaturesSkipped.push_back(Acore::StringFormat("{} ({}): {}", creature->GetName(),
                                                                                 creature->GetEntry(), why));
                    else
                        party.bossCreatures.insert(creature->GetGUID());
                }
                continue;
            }

            // A wing run: its bosses (whether or not flagged as dungeon bosses) and where the other wings are.
            if (inWing(creature->GetEntry()))
            {
                party.wingAnchors.push_back(creature->GetHomePosition());
                if (creature->IsAlive())
                    party.bossCreatures.insert(creature->GetGUID());
            }
            else if (isEncounterCredit(creature->GetEntry()))
                party.otherWingAnchors.push_back(creature->GetHomePosition());
        }

        if (!wingBosses.empty())
        {
            for (Position const& entrance : party.entrances)
                party.wingAnchors.push_back(entrance);

            // Encounter tracks of the other wings are left out (their bosses are never reached from this one). A
            // script slot that took over its dbc row (DropDbcTwins) is kept by the row's credits.
            party.bosses.erase(std::remove_if(party.bosses.begin(), party.bosses.end(),
                                              [&inWing](BossTrack const& boss)
                                              {
                                                  if (boss.entry && inWing(boss.entry))
                                                      return false;
                                                  return std::none_of(boss.credits.begin(), boss.credits.end(),
                                                                      inWing);
                                              }),
                               party.bosses.end());
        }

        for (EventStarterDef const* def : EventStarters(party.content->mapId))
        {
            StarterTrack starter;
            starter.def = def;
            party.starters.push_back(starter);
        }

        if (QueryResult result = WorldDatabase.Query(
                "SELECT a.entry FROM areatrigger a JOIN areatrigger_scripts s ON s.entry = a.entry "
                "WHERE a.map = {}",
                party.content->mapId))
        {
            do
            {
                uint32 const id = result->Fetch()[0].Get<uint32>();
                if (sObjectMgr->GetAreaTriggerTeleport(id))
                    continue;
                TriggerTrack track;
                track.triggerId = id;
                party.triggers.push_back(track);
            } while (result->NextRow());
        }

        if (QueryResult result =
                WorldDatabase.Query("SELECT ID FROM areatrigger_teleport WHERE target_map = {}", party.content->mapId))
        {
            do
            {
                TriggerTrack track;
                track.triggerId = result->Fetch()[0].Get<uint32>();
                track.entrance = true;
                party.triggers.push_back(track);
            } while (result->NextRow());
        }

        LogRunStart(party);
        party.needBuff = true;
        party.buffStep = 0;
        party.buffReason = "run start";
        SetPhase(party, Phase::Buffing);
    }

    char const* TrackRequirement(uint32 mapId, BossTrack const& boss, char const** note)
    {
        if (boss.entry)
            if (char const* kind = EncounterRequirement(mapId, boss.entry, note))
                return kind;
        for (uint32 credit : boss.credits)
            if (char const* kind = EncounterRequirement(mapId, credit, note))
                return kind;
        return nullptr;
    }

    char const* TrackClassGate(Party const& party, BossTrack const& boss)
    {
        if (!party.content)
            return nullptr;
        uint32 const mapId = party.content->mapId;
        uint8 const difficulty = party.content->difficulty;
        if (boss.entry)
            if (char const* reason = ClassGatedBoss(mapId, difficulty, boss.entry))
                return reason;
        for (uint32 credit : boss.credits)
            if (char const* reason = ClassGatedBoss(mapId, difficulty, credit))
                return reason;
        return nullptr;
    }

    uint32 RequiredBossCount(Party const& party)
    {
        uint32 count = 0;
        for (BossTrack const& boss : party.bosses)
            if (!IsPhantomSlot(party, boss) && (boss.killed || !TrackClassGate(party, boss)))
                ++count;
        return count;
    }

    bool IsPhantomSlot(Party const& party, BossTrack const& boss)
    {
        // Evidence that a script slot is a boss of its own: bound to a creature (entry / guid, the party engaged it),
        // an encounter order or credits, or killed. A state alone is none: an untouched slot reads NOT_STARTED, or
        // TO_BE_DECIDED when the script never set or loaded it (InstanceScript BossInfo default - Black Morass,
        // Culling, Shattered Halls, ToC5, Violet Hold in wave 7: every slot counted next to its dbc row), and event
        // scripts flip slots without the party (Sethekk wave 8: Anzu's normal-mode slot 1 kept the run "stalled").
        auto untouchedState = [](uint8 state) { return state == NOT_STARTED || state == TO_BE_DECIDED; };
        // [event_boss_tbcwotlk] a listed absent slot never bound to a creature is no boss even once its state reads
        // DONE: The Nexus orb slots DATA_TELESTRA_ORB / ANOMALUS_ORB / ORMOROK_ORB 5-7 go DONE when the orb is used
        // (nex-h 20261025-170140 "done" with bossesTotal 8 for 5 bosses, "boss 5", "boss 6", "boss 7" killed).
        if (party.content && !boss.dbc && !boss.entry && boss.order < 0 && boss.credits.empty() &&
            boss.creature.IsEmpty() && boss.name == "boss " + std::to_string(boss.bossId) &&
            AbsentScriptSlot(party.content->mapId, party.content->difficulty, boss.bossId))
            return true;
        if (boss.dbc || boss.entry || boss.order >= 0 || !boss.credits.empty() || !boss.creature.IsEmpty() ||
            boss.killed)
            return false;
        if (boss.name != "boss " + std::to_string(boss.bossId))
            return false;

        // A slot of a boss that does not exist on this difficulty (Anzu, Yor: heroic only), whatever its state.
        if (party.content && AbsentScriptSlot(party.content->mapId, party.content->difficulty, boss.bossId))
            return true;

        // A dungeon with DungeonEncounter.dbc rows: those rows are the bosses until a slot is bound by entry (the
        // party engaged or killed its creature, DropDbcTwins then drops the row). An unbound slot is no boss of its
        // own - counting it next to the rows counted every boss twice (Shadow Labyrinth 7 for 4).
        if (party.content && !party.content->raid)
            for (BossTrack const& other : party.bosses)
                if (other.dbc)
                    return true;

        if (boss.stateChanged || !untouchedState(boss.lastState))
            return false;

        // More script slots than dbc encounters: the untouched unnamed slots beyond the dbc count are no bosses
        // (Blood Furnace keeps 4 slots for 3 bosses). Decided by count, never by index: slot ids are the script's
        // own enum, not the dbc order.
        size_t scriptSlots = 0;
        size_t untouched = 0;
        for (BossTrack const& other : party.bosses)
        {
            if (other.dbc)
                continue;
            ++scriptSlots;
            if (other.entry == 0 && other.creature.IsEmpty() && !other.stateChanged && !other.killed &&
                untouchedState(other.lastState))
                ++untouched;
        }
        size_t const encounters = party.encounters.size();
        return scriptSlots > encounters && untouched <= scriptSlots - encounters;
    }

    void TestHarnessMgr::DropDbcTwins(Party& party)
    {
        struct ScriptKey
        {
            uint32 entry;
            int32 order;
            std::string name;
        };

        // A script slot that never names its creature ("boss N": no guid per boss id, Blood Furnace, Ramparts,
        // Auchenai Crypts, ToC5, Sethekk Halls) is NOT given the dbc row of its index any more: script slot ids are
        // the instance script's own enum, not the DungeonEncounter.dbc order (Sethekk normal: slots Syth 0, Anzu 1,
        // Ikiss 2 against dbc rows Syth 0, Ikiss 1 - slot 1 became "Talon King Ikiss" with Ikiss's credits, the real
        // kill in slot 2 stayed "boss 2", the run ended stalled with Ikiss "missing"; Halls of Lightning: slot 3
        // logged "Loken" on engage and "Volkhan" on the kill). Such a slot is bound to its creature by entry when the
        // party engages it (TickRun), and its dbc twin is dropped then by the creature's kill credit.

        std::vector<ScriptKey> keys;
        for (BossTrack const& script : party.bosses)
            if (!script.dbc && (script.entry || script.order >= 0))
                keys.push_back({script.entry, script.order, script.name});
        if (keys.empty())
            return;

        auto isTwin = [&keys](BossTrack const& boss)
        {
            if (!boss.dbc)
                return false;
            for (ScriptKey const& key : keys)
            {
                bool const byCredit =
                    key.entry && std::find(boss.credits.begin(), boss.credits.end(), key.entry) != boss.credits.end();
                bool const byOrder = key.order >= 0 && key.order == boss.order;
                bool const byName = key.entry && EncounterNameMatches(key.name, boss.name);
                if (byCredit || byOrder || byName)
                    return true;
            }
            return false;
        };
        party.bosses.erase(std::remove_if(party.bosses.begin(), party.bosses.end(), isTwin), party.bosses.end());
    }

    // A boss credited by instance data, not by its death (EncounterDoneData: Cho'Rush surrenders when King Gordok
    // dies): alive and resolved once the data says DONE and it is no enemy any more.
    static bool DoneByData(uint32 mapId, Map* map, Creature const* creature)
    {
        InstanceScript* script = map && map->ToInstanceMap() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
        uint32 const dataId = EncounterDoneData(mapId, creature->GetEntry());
        return dataId && script && script->GetData(dataId) == DONE && !creature->IsInCombat() &&
               !creature->IsHostileToPlayers();
    }

    // Violet Hold releases two of its six prisoners per run: the four still in their cells (not attackable, out of
    // combat) are no boss of this run (wave 8 vh-n: Cyanigosa dead, the run "missing" Xevozz / Lavanthor / Zuramat,
    // the Sinclari gossip restarted the event).
    static bool UnreleasedPrisoner(Creature const* creature)
    {
        return IsVioletHoldPrisoner(creature) && creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) &&
               !creature->IsInCombat();
    }

    // User decision 2026-10-25 (build 22): a required boss friendly to the group's faction is no valid encounter for
    // it (Uldaman's Lost Dwarves Baelog 6906 / Eric 6907 / Olaf 6908, faction 122 Ironforge, for an Alliance group; no
    // script changes that): "not applicable (friendly to <faction>)" - not required, listed as n/a in run_end, never
    // as missing. Empty: applicable.
    // [w24c-friendly-allowlist] The live reaction misclassified hostile bosses: a member charmed by a Time-Lost
    // Controller's Charming Totem (seth-h 20261025-123520, leader on the monster faction) made Darkweaver Syth and
    // Talon King Ikiss "friendly" and the run ended "done" with 0 kills; event bosses still on their pre-event
    // faction 35 (Phalanx, The Seven, Ribbly, Zum'rah) were excluded too. Only a fixed list of bosses that really
    // belong to one player faction qualifies, judged by the creature_template faction against the leader's race
    // faction (DBC, not the live - charmable, temporary - faction of either side).
    static bool FactionLockedBoss(uint32 entry)
    {
        switch (entry)
        {
            case 6906:  // Baelog (Uldaman Lost Dwarves, faction 122 Ironforge)
            case 6907:  // Eric "The Swift"
            case 6908:  // Olaf
                return true;
            default:
                return false;
        }
    }

    static bool TemplateFriendlyToRace(Creature const* creature, Player const* player)
    {
        CreatureTemplate const* proto = creature->GetCreatureTemplate();
        ChrRacesEntry const* race = sChrRacesStore.LookupEntry(player->getRace(true));
        FactionTemplateEntry const* mine = proto ? sFactionTemplateStore.LookupEntry(proto->faction) : nullptr;
        FactionTemplateEntry const* theirs = race ? sFactionTemplateStore.LookupEntry(race->FactionID) : nullptr;
        return mine && theirs && mine->IsFriendlyTo(*theirs) && !mine->IsHostileTo(*theirs);
    }

    static std::string FriendlyBossReason(Party const& party, Creature const* creature)
    {
        Player* leader = ObjectAccessor::FindPlayer(party.leader);
        if (!leader || !creature || !creature->IsAlive() || creature->IsInCombat() ||
            !FactionLockedBoss(creature->GetEntry()) || !TemplateFriendlyToRace(creature, leader))
            return std::string();
        return std::string("not applicable (friendly to ") +
               (leader->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde") + ")";
    }

    static std::string FriendlyTrackReason(Party const& party, Map* map, BossTrack const& boss)
    {
        if (boss.killed || !map || boss.creature.IsEmpty())
            return std::string();
        return FriendlyBossReason(party, map->GetCreature(boss.creature));
    }

    // [w24c-hor-escape] Halls of Reflection (668) escape from The Lich King (36954 boss / 37226 event): the four ice
    // walls stand at the instance's NPC_ICE_WALL_TARGET 37014..37017 guids, GO_ICE_WALL 201385 is the wall itself
    // (npc_hor_lich_king::WallCompleted opens it). A wall seen shut and then open or gone counts as broken.
    void TestHarnessMgr::WatchHorEscapeWalls(Party& party, Map* map, BossTrack& boss, Player* leader, uint32 now)
    {
        constexpr uint32 HOR_MAP_ID = 668;
        constexpr uint32 HOR_LK_BOSS = 36954;
        constexpr uint32 HOR_LK_EVENT = 37226;
        constexpr uint32 HOR_ICE_WALL_TARGET = 37014;
        constexpr uint32 HOR_GO_ICE_WALL = 201385;
        constexpr uint32 HOR_ICE_WALLS = 4;
        constexpr float HOR_WALL_SEARCH = 25.0f;
        if (party.content->mapId != HOR_MAP_ID || (boss.entry != HOR_LK_BOSS && boss.entry != HOR_LK_EVENT))
            return;
        InstanceScript* script = map->ToInstanceMap() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
        if (!script)
            return;
        uint8 up = 0;
        int32 nearest = -1;
        float nearestDist = 0.0f;
        for (uint32 i = 0; i < HOR_ICE_WALLS; ++i)
        {
            Creature* target = map->GetCreature(script->GetGuidData(HOR_ICE_WALL_TARGET + i));
            if (!target)
                continue;
            // ACTION_DELETE_ICE_WALL opens the wall (HandleGameObject(GO_ICE_WALL, true)): up = there and still shut.
            if (GameObject* wall = target->FindNearestGameObject(HOR_GO_ICE_WALL, HOR_WALL_SEARCH);
                wall && wall->GetGoState() == GO_STATE_READY)
                up |= uint8(1u << i);
            float const dist = leader->GetExactDist(target);
            if (nearest < 0 || dist < nearestDist)
            {
                nearest = int32(i);
                nearestDist = dist;
            }
        }
        uint8 const newlyUp = up & ~boss.horWallsUp & ~boss.horWallsBroken;
        uint8 const newlyBroken = boss.horWallsUp & ~up;
        if (!newlyUp && !newlyBroken)
            return;
        boss.horWallsUp = (boss.horWallsUp | newlyUp) & ~newlyBroken;
        boss.horWallsBroken |= newlyBroken;
        uint32 broken = 0;
        for (uint32 i = 0; i < HOR_ICE_WALLS; ++i)
            broken += (boss.horWallsBroken >> i) & 1u;
        if (newlyBroken)
        {
            boss.loopProgressMs = now ? now : 1;
            boss.loopProgressKills = party.kills;
        }
        LogEvent(party, "hor_escape_wall",
                 JsonObject().Str("change", newlyBroken ? "broken" : "up").Num("wallsUpMask", boss.horWallsUp)
                     .Num("wallsBrokenMask", boss.horWallsBroken).Num("broken", broken).Num("nearestWall", nearest + 1)
                     .Real("nearestWallDist", nearestDist).Num("killsSinceEngage", party.kills - boss.attemptKills)
                     .Num("sinceEngageSec", getMSTimeDiff(boss.attemptStartMs, now) / IN_MILLISECONDS)
                     .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY()));
        LOG_INFO("playerbots",
                 "[w24c-hor-escape] {}: ice wall mask {} {} ({} of 4 broken, up mask {}, nearest wall {} at {:.1f} yd, "
                 "{} kills since engage)",
                 party.id, uint32(newlyBroken ? newlyBroken : newlyUp), newlyBroken ? "broken" : "up", broken,
                 uint32(boss.horWallsUp), nearest + 1, nearestDist, party.kills - boss.attemptKills);
    }

    bool TestHarnessMgr::AllBossesResolved(Party& party, Map* map)
    {
        // User rule: a dungeon (or raid) is done only when EVERY boss is dead - script tracks, every
        // DungeonEncounter.dbc encounter that no script track covers, and every boss creature that was spawned.
        // Event / item-summoned / rare bosses count too, a failed (3 wipes) or unreachable boss is not dead, and a
        // skipped boss stays open: such a run ends "partial" with bossesMissing (Finish), never "done".
        bool tracked = false;
        bool anyKilled = false;
        bool anyFriendly = false;
        for (BossTrack const& boss : party.bosses)
        {
            if (IsPhantomSlot(party, boss) || (!boss.killed && TrackClassGate(party, boss)))
                continue;
            tracked = true;
            anyKilled |= boss.killed;
            if (boss.killed)
                continue;
            if (FriendlyTrackReason(party, map, boss).empty())
                return false;
            anyFriendly = true;
        }
        // [w24c-no-done-by-exclusion] Exclusions never resolve a run on their own: 0 kills with a "friendly" boss
        // left is no dungeon done (seth-h 20261025-123520: "done" after 189 s, bossesKilled []).
        if (anyFriendly && !anyKilled && !party.bossProgressCount)
        {
            if (!party.friendlyOnlyLogged)
            {
                party.friendlyOnlyLogged = true;
                LOG_INFO("playerbots", "[w24c-no-done-by-exclusion] {}: no boss killed, friendly exclusions alone do "
                                       "not resolve the run", party.content->key);
            }
            return false;
        }

        for (ObjectGuid const& guid : party.bossCreatures)
        {
            Creature* creature = map->GetCreature(guid);
            if (creature && creature->IsAlive() &&
                !ClassGatedBoss(party.content->mapId, party.content->difficulty, creature->GetEntry()) &&
                !DoneByData(party.content->mapId, map, creature) && !UnreleasedPrisoner(creature) &&
                FriendlyBossReason(party, creature).empty())
                return false;
        }

        return tracked || !party.bossCreatures.empty();
    }

    JsonArray TestHarnessMgr::MissingBosses(Party& party, std::vector<std::string>* lines)
    {
        JsonArray missing;
        // The instance as seen by any member inside; with the whole group out (corpse run, a ghost at the
        // graveyard) its creatures cannot be checked, which is said instead of guessing why a boss is missing.
        Map* map = nullptr;
        for (Member const& member : party.members)
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                if (bot->IsInWorld() && bot->GetMapId() == party.content->mapId && bot->GetMap() &&
                    bot->GetMap()->GetInstanceId() == party.instanceId)
                {
                    map = bot->GetMap();
                    break;
                }

        std::set<uint32> reported;
        for (BossTrack const& boss : party.bosses)
        {
            if (boss.killed || IsPhantomSlot(party, boss))
                continue;
            if (TrackClassGate(party, boss))
            {
                // Whitelisted (only one class can unlock it): excluded, listed in run_end.bossesExcluded.
                if (boss.entry)
                    reported.insert(boss.entry);
                for (uint32 credit : boss.credits)
                    reported.insert(credit);
                continue;
            }

            // Friendly to the group's faction: n/a (run_end.bossesExcluded), not missing.
            if (!FriendlyTrackReason(party, map, boss).empty())
            {
                if (boss.entry)
                    reported.insert(boss.entry);
                for (uint32 credit : boss.credits)
                    reported.insert(credit);
                continue;
            }

            char const* note = nullptr;
            char const* need = TrackRequirement(party.content->mapId, boss, &note);
            Creature* creature = map && !boss.creature.IsEmpty() ? map->GetCreature(boss.creature) : nullptr;
            // [b34_arc] A script slot named from ScriptSlots ([hleft-script-slot-names]) carries its entry but is bound
            // to its creature only on engage / kill: never reached, it read "not spawned (event not started)" while
            // its creature stood alive in party.bossCreatures (arc-h 20261026-014521-arc-h-5F-31 and every arc-h
            // since 20261025-184229: Dalliah and Soccothrates are plain spawns, instance_arcatraz gates neither).
            // The slot's entry is looked up among the boss creatures seen at run start.
            if (!creature && boss.creature.IsEmpty() && map && boss.entry)
                for (ObjectGuid const& guid : party.bossCreatures)
                    if (Creature* candidate = map->GetCreature(guid); candidate && candidate->GetEntry() == boss.entry)
                    {
                        creature = candidate;
                        break;
                    }
            std::string reason;
            if (boss.failed && !boss.failReason.empty())
                reason = "failed: " + boss.failReason;  // [hleft-script-slot-names]
            else if (boss.failed)
                reason = "failed (" + std::to_string(boss.wipes) + " wipes)";
            else if (!map)
                reason = boss.attempts ? "group outside instance, after " + std::to_string(boss.attempts) +
                                             " attempt(s)"
                                       : "group outside instance (not checked)";
            else if (!creature && !boss.creature.IsEmpty())
                reason = "creature not loaded (grid inactive)";
            else if (creature && creature->IsAlive() && party.skipUntil.count(creature->GetGUID()))
                reason = party.skipReason.count(creature->GetGUID())
                             ? "skipped: " + party.skipReason.at(creature->GetGUID())
                             : std::string("unreachable");
            else if (creature && creature->IsAlive())
                reason = boss.attempts ? "alive after " + std::to_string(boss.attempts) + " attempt(s)"
                                       : "alive, not engaged";
            else if (creature && !creature->IsAlive())
                reason = "dead, kill credit not observed";
            else if (need)
            {
                reason = need;
                // A "summon item missing" whose item a member carries: the event is what is missing.
                uint32 item = boss.entry ? EncounterRequirementItem(party.content->mapId, boss.entry) : 0;
                for (uint32 credit : boss.credits)
                    if (!item)
                        item = EncounterRequirementItem(party.content->mapId, credit);
                if (item && strcmp(need, "summon item missing") == 0)
                {
                    std::string holders;
                    for (Member const& member : party.members)
                        if (Player* bot = ObjectAccessor::FindPlayer(member.guid); bot && bot->HasItemCount(item, 1))
                            holders += (holders.empty() ? "" : ",") + member.name;
                    if (!holders.empty())
                        reason = Acore::StringFormat("event not started (summon item {} in bags: {})", item, holders);
                }
            }
            else if (boss.dbc && boss.credits.empty())
                reason = "credit not observed (spell credit)";
            else
                reason = "not spawned (event not started)";

            if (boss.entry)
                reported.insert(boss.entry);
            for (uint32 credit : boss.credits)
                reported.insert(credit);

            missing.Raw(JsonObject().Str("boss", boss.name).Num("entry", boss.entry).Bool("dbc", boss.dbc)
                            .Str("reason", reason).Str("note", note ? note : "").Build());
            if (lines)
                lines->push_back(boss.name + " - " + reason);
        }

        // Boss creatures alive that no track names.
        if (map)
            for (ObjectGuid const& guid : party.bossCreatures)
            {
                Creature* creature = map->GetCreature(guid);
                if (!creature || !creature->IsAlive() || reported.count(creature->GetEntry()) ||
                    DoneByData(party.content->mapId, map, creature) || UnreleasedPrisoner(creature) ||
                    ClassGatedBoss(party.content->mapId, party.content->difficulty, creature->GetEntry()) ||
                    !FriendlyBossReason(party, creature).empty())
                    continue;
                reported.insert(creature->GetEntry());
                std::string const reason = !party.skipUntil.count(guid) ? std::string("alive, not engaged")
                                           : party.skipReason.count(guid) ? "skipped: " + party.skipReason.at(guid)
                                                                          : std::string("unreachable");
                missing.Raw(JsonObject().Str("boss", creature->GetName()).Num("entry", creature->GetEntry())
                                .Bool("dbc", false).Str("reason", reason).Str("note", "").Build());
                if (lines)
                    lines->push_back(creature->GetName() + " - " + reason);
            }
        return missing;
    }

    void TestHarnessMgr::TickRun(Party& party)
    {
        uint32 const now = getMSTime();
        Player* leader = ObjectAccessor::FindPlayer(party.leader);
        if (!leader || !leader->IsInWorld())
        {
            Finish(party, "failed", "leader offline");
            return;
        }

        // The daily instance reset (user 2026-10-25: it stays at Instance.ResetTimeHour, real servers reset then): a
        // run whose instance and its save are gone within the reset hour ends "void" (no failure, no streak) and
        // goes back to the front of the queue.
        if (party.instanceId && party.content && !party.content->raid &&
            !sInstanceSaveMgr->GetInstanceSave(party.instanceId) &&
            !sMapMgr->FindMap(party.content->mapId, party.instanceId))
        {
            time_t const wall = time(nullptr);
            tm local{};
            localtime_r(&wall, &local);
            if (uint32(local.tm_hour) == sWorld->getIntConfig(CONFIG_INSTANCE_RESET_TIME_HOUR))
            {
                if (party.fromQueue)
                    _queue.push_front({party.content, party.comp});
                Finish(party, "void", "void: daily instance reset");
                return;
            }
        }

        // A map change or group update resets bot strategies; the run needs its own back on the leader, and (bots
        // walk by themselves) on every member: a portal walked through resets them on arrival.
        if (PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader))
        {
            if (!leaderAI->HasStrategy("test runner", BOT_STATE_NON_COMBAT))
            {
                ApplyStrategies(party);
                LogEvent(party, "strategies_reapplied", JsonObject().Str("reason", "leader lost test runner"));
            }
        }
        for (Member const& member : party.members)
            if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                    if (bot->IsInWorld() && !bot->IsBeingTeleported() &&
                        (!botAI->HasStrategy("instance portal", BOT_STATE_NON_COMBAT) ||
                         !botAI->HasStrategy("ghost portal", BOT_STATE_DEAD)))
                    {
                        ApplyStrategies(party);
                        LogEvent(party, "strategies_reapplied",
                                 JsonObject().Str("reason", member.name + " lost instance portal"));
                        break;
                    }
        // ApplyInstanceStrategies puts classic-brs back on every worldport (PlayerbotAI.cpp, after the map attach)
        // without touching "test runner": its UBRS steps (Blackrock Altar) walked the lbrs-n tank into the Dragonspire
        // Hall after the wipe (2026-10-25 wave 7). An LBRS group has none of its business.
        if (std::strcmp(party.content->key, "lbrs-n") == 0)
            for (Member const& member : party.members)
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid))
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        if (bot->IsInWorld() && !bot->IsBeingTeleported() &&
                            (botAI->HasStrategy("classic-brs", BOT_STATE_NON_COMBAT) ||
                             botAI->HasStrategy("classic-brs", BOT_STATE_COMBAT)))
                        {
                            botAI->ChangeStrategy("-classic-brs", BOT_STATE_NON_COMBAT);
                            botAI->ChangeStrategy("-classic-brs", BOT_STATE_COMBAT);
                        }

        std::vector<Player*> players;
        uint32 alive = 0;
        bool anyInCombat = false;
        // Combat time per member, for DPS over the member's own combat seconds (pull_end, boss_attempt, run_end).
        uint32 const combatSampleMs =
            party.combatSampleMs ? std::min<uint32>(getMSTimeDiff(party.combatSampleMs, now), 3 * TICK_MS) : 0;
        party.combatSampleMs = now;
        for (Member& member : party.members)
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || !bot->IsInWorld())
                continue;
            players.push_back(bot);
            // Back in through the portal (MeetMember's regroup): only a member that was inside, left and came back.
            if (bot->GetMapId() != party.content->mapId)
                member.wasOutside = member.seenInside;
            else if (!bot->IsBeingTeleported())
            {
                if (member.wasOutside)
                    member.backInMs = now ? now : 1;
                member.seenInside = true;
                member.wasOutside = false;
            }
            if (bot->IsAlive())
            {
                ++alive;
                member.deadSinceMs = 0;
                if (bot->GetMapId() == party.content->mapId && !bot->IsBeingTeleported())
                    member.awaySinceMs = 0;
                // [b33_sh2] a pet that fights for its out-of-combat owner keeps the pull on (HarnessPetFighting)
                bool const petFighting = HarnessPetFighting(bot, now);
                if (petFighting && !bot->IsInCombat())
                    anyInCombat = true;
                if (bot->IsInCombat())
                {
                    anyInCombat = true;
                    member.combatMs += combatSampleMs;
                }
            }
            else if (!member.deadSinceMs)
                member.deadSinceMs = now;
        }

        Map* map = nullptr;
        for (Player* bot : players)
            if (bot->GetMapId() == party.content->mapId && bot->GetMap() &&
                bot->GetMap()->GetInstanceId() == party.instanceId)
            {
                map = bot->GetMap();
                break;
            }

        // The leader is on the content map but in another instance (its bind was lost: the wave 7 homebind ejection
        // above): every check against party.instanceId is blind from now on and the run only waits for the stall
        // watchdog ("group outside instance" for 4 minutes). Said at once, with both ids.
        if (leader->GetMapId() == party.content->mapId && leader->GetMap() &&
            leader->GetMap()->GetInstanceId() != party.instanceId && !leader->IsBeingTeleported())
        {
            uint32 const other = leader->GetMap()->GetInstanceId();
            LogEvent(party, "instance_changed", JsonObject().Num("runInstanceId", party.instanceId)
                                                    .Num("leaderInstanceId", other));
            // [hleft-daily-reset] the server's own reset, not a lost bind: said so (the run is void, no bot fault).
            if (party.instanceResetAt && GameTime::GetGameTime().count() >= party.instanceResetAt)
            {
                Finish(party, "failed", Acore::StringFormat("instance reset by the server's scheduled reset "
                                                            "(Instance.ResetTimeHour; instance {} -> {}): run void",
                                                            party.instanceId, other));
                return;
            }
            Finish(party, "failed", Acore::StringFormat("leader re-entered another instance ({} instead of {}): "
                                                        "instance bind lost", other, party.instanceId));
            return;
        }

        // A route link being crossed (Gundrak's bridge): every member over it, also while the group waits for them.
        if (map)
            CrossLinks(party, players, leader);

        uint32 const kills = StatsRegistry::Instance().TakeKills(party.seq);
        if (kills)
        {
            party.kills += kills;
            party.windowKills += kills;
            party.pullKills += kills;
            party.lastProgressMs = now;
            party.stallLevel = 0;
        }

        // Encounter states, straight from the InstanceScript; dbc tracks from their kill-credit creature.
        InstanceScript* script = map && map->ToInstanceMap() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
        std::vector<uint32> const killedEntries = StatsRegistry::Instance().TakeKilledEntries(party.seq);
        BossTrack* activeBoss = nullptr;
        if (map)
        {
            // Script tracks resolved during the last tick make their dbc twins redundant (before any pointer is taken).
            DropDbcTwins(party);
            for (BossTrack& boss : party.bosses)
            {
                ResolveBoss(party, boss, map, script);
                Creature* bossCreature = boss.creature.IsEmpty() ? nullptr : map->GetCreature(boss.creature);

                uint8 state = boss.lastState;
                if (!boss.dbc)
                {
                    if (!script)
                        continue;
                    state = uint8(script->GetBossState(boss.bossId));
                    // [kara3] a slot the instance keeps as data (Karazhan's Chess Event: SetData 8, never SetBossState)
                    if (uint32 const dataId = ScriptSlotData(party.content->mapId, boss.bossId);
                        dataId != UINT32_MAX && state != DONE)
                    {
                        uint32 const data = script->GetData(dataId);
                        if (data == DONE || data == IN_PROGRESS)
                            state = uint8(data);
                    }
                }
                else if (boss.killed)
                    state = DONE;
                else
                {
                    bool credited = script && (script->GetCompletedEncounterMask() & (1u << boss.bossId));
                    // Violet Hold: the dbc "First / Second Prisoner" tracks credit only Erekem / Moragg; the run
                    // releases two of six at random - the script's slots DATA_1ST_BOSS 0 / DATA_2ND_BOSS 1 say it
                    // (wave 8 vh-n: Cyanigosa dead, "Second Prisoner" never credited, the run never done).
                    if (!credited && script && party.content->mapId == VIOLET_HOLD_MAP && boss.bossId < 2 &&
                        script->GetBossState(boss.bossId) == DONE)
                        credited = true;
                    // Credited by no kill (the Ring of Law on heroic: no encounter list, no completed mask).
                    for (uint32 credit : boss.credits)
                        if (uint32 const dataId = EncounterDoneData(party.content->mapId, credit); dataId && script)
                            credited = credited || script->GetData(dataId) == DONE;
                    for (uint32 entry : killedEntries)
                        credited = credited ||
                                   std::find(boss.credits.begin(), boss.credits.end(), entry) != boss.credits.end();
                    if (credited)
                        state = DONE;
                    else
                        state = bossCreature && bossCreature->IsAlive() && bossCreature->IsInCombat()
                                    ? uint8(IN_PROGRESS)
                                    : uint8(NOT_STARTED);
                }

                // An attempt is the party's fight: script NPCs also engage bosses (Trollgore's Drakkari Invaders
                // every 30 s, Hadronox's crusher packs), which flips the state without anybody of the group there.
                if (state == IN_PROGRESS && boss.lastState == IN_PROGRESS && !boss.engaged)
                {
                    Creature* fought = bossCreature;
                    bool bossInCombat = fought && fought->IsAlive() && fought->IsInCombat();
                    if (!fought)
                    {
                        for (auto const& [spawnId, candidate] : map->GetCreatureBySpawnIdStore())
                        {
                            if (!candidate || !candidate->IsAlive() || !candidate->IsInCombat() ||
                                !(IsBossLike(candidate) || BossOrder(party, candidate) >= 0))
                                continue;

                            // Claimed by another script slot; a dbc row naming the same creature is its twin
                            // (dropped once this slot is bound), not a claim.
                            bool claimed = false;
                            for (BossTrack const& other : party.bosses)
                                claimed |= &other != &boss && other.dbc == boss.dbc &&
                                           other.entry == candidate->GetEntry();
                            if (claimed)
                                continue;

                            bossInCombat = true;
                            bool byParty = false;
                            for (Player* bot : players)
                                byParty |= bot->IsAlive() && candidate->IsEngagedBy(bot);
                            if (byParty || !fought)
                                fought = candidate;
                            if (byParty)
                                break;
                        }
                    }

                    if (fought && bossInCombat)
                    {
                        for (Player* bot : players)
                            boss.engaged |= bot->IsAlive() && fought->IsEngagedBy(bot);
                        if (boss.engaged && boss.creature.IsEmpty())
                        {
                            boss.creature = fought->GetGUID();
                            boss.entry = fought->GetEntry();
                            boss.name = fought->GetName();
                            if (boss.order < 0)
                                boss.order = BossOrder(party, fought);
                        }
                    }
                    else if (!GauntletSlot(party, boss))
                    {
                        // Wave encounters (Violet Hold, Halls of Reflection): the boss stands aside while the party
                        // fights the encounter's adds.
                        for (Player* bot : players)
                            boss.engaged |= bot->IsAlive() && bot->IsInCombat();
                    }
                    // [b31_dg] else a gauntlet in front of the boss (GauntletSlot): its adds are no attempt; the
                    // attempt starts when the boss creature itself fights the party (the branch above).

                    if (boss.engaged)
                    {
                        ++boss.attempts;
                        boss.attemptStartMs = now;
                        boss.attemptKills = party.kills;
                        boss.loopProgressMs = 0;
                        boss.loopProgressKills = 0;
                        boss.horWallsUp = 0;
                        boss.horWallsBroken = 0;
                        boss.attemptDeaths.clear();
                        boss.homePos = fought ? fought->GetPosition() : leader->GetPosition();
                        for (Member const& member : party.members)
                        {
                            boss.attemptStart[member.guid] = StatsRegistry::Instance().Snapshot(member.guid);
                            boss.attemptStartCombatMs[member.guid] = member.combatMs;
                            boss.attemptStartAggro[member.guid] = member.aggroPulled;
                        }
                        boss.engageLevel = fought ? fought->GetLevel() : 0;
                        boss.engageMaxHp = fought ? fought->GetMaxHealth() : 0;
                        party.pullIsBoss = true;
                        LogPull(party, players, true, boss.name);
                        LogEvent(party, "boss_engaged", JsonObject().Num("bossId", boss.bossId).Str("boss", boss.name)
                                                            .Num("entry", boss.entry).Num("order", boss.order)
                                                            .Num("level", boss.engageLevel)
                                                            .Num("maxHp", boss.engageMaxHp)
                                                            .Num("groupLevel", party.level)
                                                            .Real("expectedGroupDps", ExpectedGroupDps(party)));
                        LogBossPullState(party, fought, boss.name);  // [harness20]
                        LogAuraStacks(party, players, "boss_pull");  // [harness30]
                    }
                }

                if (state == IN_PROGRESS && boss.engaged)
                    activeBoss = &boss;
                if (state == boss.lastState)
                    continue;

                uint8 const previous = boss.lastState;
                boss.lastState = state;
                // TO_BE_DECIDED -> NOT_STARTED is the script initialising the slot, not an encounter moving.
                if (!((previous == TO_BE_DECIDED && state == NOT_STARTED) ||
                      (previous == NOT_STARTED && state == TO_BE_DECIDED)))
                    boss.stateChanged = true;

                // A script slot done before it was bound (killed in one tick, or engaged by NPCs first): the boss
                // creature the party killed in this tick that no other track claims names it.
                if (state == DONE && !boss.dbc && !boss.entry)
                    for (uint32 entry : killedEntries)
                    {
                        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
                        bool const bossEntry = info && (info->rank == CREATURE_ELITE_WORLDBOSS ||
                                                        info->HasFlagsExtra(CREATURE_FLAG_EXTRA_DUNGEON_BOSS) ||
                                                        BossOrderOfEntry(party, entry) >= 0);
                        bool claimed = false;
                        for (BossTrack const& other : party.bosses)
                            claimed |= &other != &boss && !other.dbc && other.entry == entry;
                        if (!bossEntry || claimed)
                            continue;
                        boss.entry = entry;
                        boss.name = info->Name;
                        if (boss.order < 0)
                            boss.order = BossOrderOfEntry(party, entry);
                        break;
                    }

                // [hleft-script-slot-names] a slot whose FAIL loses the boss for good (the Infinite Corruptor's
                // Guardian of Time timer): failed with its reason, whatever the party did.
                if (state == FAIL && !boss.dbc && !boss.failed && party.content)
                    for (ScriptSlotDef const* def : ScriptSlots(party.content->mapId, party.content->difficulty))
                        if (def->bossId == boss.bossId && def->failNote)
                        {
                            boss.failed = true;
                            boss.failReason = def->failNote;
                            party.lastProgressMs = now;
                            LogEvent(party, "boss_failed", JsonObject().Num("bossId", boss.bossId)
                                                               .Str("boss", boss.name).Num("wipes", boss.wipes)
                                                               .Str("reason", boss.failReason));
                        }

                if (state == IN_PROGRESS)
                {
                    // Engagement is decided on the next tick (above), once the party had a chance to enter combat.
                    boss.engaged = false;
                    party.lastProgressMs = now;
                }
                else if (previous == IN_PROGRESS && !boss.engaged)
                {
                    if (state == DONE)
                        boss.killed = true;
                    else
                        ++boss.npcResets;
                    LogEvent(party, "boss_npc_engage", JsonObject().Num("bossId", boss.bossId).Str("boss", boss.name)
                                                           .Num("to", state).Num("times", boss.npcResets)
                                                           .Str("note", "state flipped without the party fighting"));
                }
                else if (previous == IN_PROGRESS && state == DONE)
                {
                    party.lastProgressMs = now;
                    party.stallLevel = 0;
                    boss.killed = true;
                    LogAttemptEnd(party, boss, "kill");
                }
                else if (previous == IN_PROGRESS)
                {
                    party.lastProgressMs = now;
                    party.stallLevel = 0;
                    // [b35_urom] A reset the boss's own script makes is no wipe. boss_urom.cpp: on platforms 1-3 his
                    // JustEngagedWith casts Summon Menagerie, LeaveCombat() -> Reset() -> SetData(DATA_UROM,
                    // NOT_STARTED) and he moves on; only the 4th engage (centre) is the fight. occ-n
                    // 20261026-024524-occ-n-5E-24: three 3 s "resets", no death, boss_failed (3 wipes), killed after.
                    // Generally: back to NOT_STARTED (not FAIL), nobody died in the attempt nor lies dead now, and the
                    // attempt was short (or the boss is a known scripted-reset one). At most SCRIPTED_RESET_MAX per
                    // boss, so a boss evading in a loop still fails.
                    constexpr uint32 SCRIPTED_RESET_MS = 10 * IN_MILLISECONDS;
                    constexpr uint32 SCRIPTED_RESET_MAX = 3;
                    constexpr uint32 UROM_ENTRY = 27655;
                    bool deadNow = false;
                    for (Player* bot : players)
                        deadNow |= !bot->IsAlive();
                    uint32 const fightMs = boss.attemptStartMs ? getMSTimeDiff(boss.attemptStartMs, now) : 0;
                    bool const knownScripted = boss.entry == UROM_ENTRY;
                    bool const scriptedReset = state == NOT_STARTED && boss.attemptDeaths.empty() && !deadNow &&
                                               boss.scriptedResets < SCRIPTED_RESET_MAX &&
                                               (knownScripted || fightMs < SCRIPTED_RESET_MS);
                    if (scriptedReset)
                    {
                        ++boss.scriptedResets;
                        LogAttemptEnd(party, boss, "scripted_reset");
                        LogEvent(party, "boss_scripted_reset",
                                 JsonObject().Num("bossId", boss.bossId).Str("boss", boss.name).Num("entry", boss.entry)
                                     .Num("fightMs", fightMs).Num("times", boss.scriptedResets)
                                     .Bool("known", knownScripted));
                        party.lastAttemptEndBoss = boss.name;
                        party.lastAttemptEndMs = now ? now : 1;
                    }
                    else
                    {
                        ++boss.wipes;
                        LogAttemptEnd(party, boss, state == FAIL ? "wipe" : "reset");
                        party.lastAttemptEndBoss = boss.name;
                        party.lastAttemptEndMs = now ? now : 1;
                        if (boss.wipes >= 3)
                        {
                            boss.failed = true;
                            LogEvent(party, "boss_failed", JsonObject().Num("bossId", boss.bossId)
                                                               .Str("boss", boss.name).Num("wipes", boss.wipes));
                        }
                    }
                }
                else if (state == DONE)
                {
                    party.lastProgressMs = now;
                    boss.killed = true;
                }

                if (state != IN_PROGRESS)
                    boss.engaged = false;

                LogEvent(party, "boss_state", JsonObject().Num("bossId", boss.bossId).Str("boss", boss.name)
                                                  .Num("from", previous).Num("to", state).Bool("dbc", boss.dbc)
                                                  .Num("level", bossCreature ? bossCreature->GetLevel() : 0)
                                                  .Num("maxHp", bossCreature ? bossCreature->GetMaxHealth() : 0));
            }
        }

        // Boss progress and the kills since (diagnosis of a loop on reviving / endless trash).
        for (uint32 entry : killedEntries)
        {
            ++party.killsSinceBoss[entry];
            ++party.deadEntries[entry];
        }
        if (!killedEntries.empty())
            PrepareEventItems(party, leader, false);
        {
            uint32 progress = 0;
            for (BossTrack const& boss : party.bosses)
                if (boss.killed || boss.failed)
                    ++progress;
            if (progress > party.bossProgressCount)
            {
                party.lastBossProgressMs = now;
                party.killsAtBossProgress = party.kills;
                party.killsSinceBoss.clear();
                party.loopSinceMs = now;
                party.loopAnchor = leader->GetPosition();
                party.loopKills = party.kills;
                party.bossRushUntilMs = 0;
            }
            party.bossProgressCount = progress;  // DropDbcTwins may lower it: only a rise is progress
        }
        if (GUARD_KILL_PROGRESS && leader->GetMap() && leader->GetMap()->GetInstanceId() == party.instanceId &&
            (!party.guardedObjectCheckMs || getMSTimeDiff(party.guardedObjectCheckMs, now) >= GUARDED_OBJECT_CHECK_MS))
        {
            party.guardedObjectCheckMs = now ? now : 1;
            uint32 down = 0;
            char const* downNote = nullptr;
            std::vector<GuardedDoorDef const*> const guarded = GuardedDoors(party.content->mapId);
            bool const anyObjects = std::any_of(guarded.begin(), guarded.end(),
                                                [](GuardedDoorDef const* def) { return !def->goEntries.empty(); });
            if (anyObjects)
                for (auto const& [spawnId, go] : leader->GetMap()->GetGameObjectBySpawnIdStore())
                    if (go && go->isSpawned())
                        for (GuardedDoorDef const* def : guarded)
                            if (std::find(def->goEntries.begin(), def->goEntries.end(), go->GetEntry()) !=
                                    def->goEntries.end() &&
                                uint32(go->GetGoState()) != def->goState)
                            {
                                ++down;
                                downNote = def->note;
                            }
            if (down > party.guardedObjectsDown)
            {
                LogEvent(party, "guarded_object_progress",
                         JsonObject().Num("down", down).Num("before", party.guardedObjectsDown)
                             .Num("sinceProgressSec", party.lastBossProgressMs
                                                          ? getMSTimeDiff(party.lastBossProgressMs, now) /
                                                                IN_MILLISECONDS
                                                          : 0)
                             .Str("note", downNote ? downNote : ""));
                party.lastBossProgressMs = now;
                party.loopSinceMs = now;
                party.loopAnchor = leader->GetPosition();
                party.loopKills = party.kills;
            }
            party.guardedObjectsDown = down;
        }
        if (GUARD_KILL_PROGRESS && !killedEntries.empty())
            for (GuardedDoorDef const* def : GuardedDoors(party.content->mapId))
            {
                if (!def->goEntries.empty())
                    continue;
                for (uint32 entry : killedEntries)
                {
                    if (std::find(def->guardEntries.begin(), def->guardEntries.end(), entry) == def->guardEntries.end())
                        continue;
                    uint32 guardKills = 0;
                    for (uint32 guardEntry : def->guardEntries)
                        if (auto const dead = party.deadEntries.find(guardEntry); dead != party.deadEntries.end())
                            guardKills += dead->second;
                    LogEvent(party, "guard_kill_progress",
                             JsonObject().Num("entry", entry).Num("door", def->doorEntry).Num("guardKills", guardKills)
                                 .Num("guardEntries", uint32(def->guardEntries.size()))
                                 .Num("sinceProgressSec",
                                      party.lastBossProgressMs
                                          ? getMSTimeDiff(party.lastBossProgressMs, now) / IN_MILLISECONDS
                                          : 0)
                                 .Str("note", def->note));
                    party.lastBossProgressMs = now;
                    party.loopSinceMs = now;
                    party.loopAnchor = leader->GetPosition();
                    party.loopKills = party.kills;
                }
            }

        for (DeathNote const& death : StatsRegistry::Instance().TakeDeaths(party.seq))
        {
            // [b30_topped_up] a stranded re-entry (out of combat) is a death of the run, of no pull or attempt
            bool const reentry = death.killer.find("_reenter: ") != std::string::npos;  // [b30_reenter] either kind
            if (!reentry)
                party.pullDeaths.push_back(death.name);
            // [b30_st_spirit] the death burst (a partial wipe, DEATH_BURST_*)
            if (!reentry && !party.wiped)
            {
                party.recentDeathMs.push_back(now);
                while (!party.recentDeathMs.empty() &&
                       getMSTimeDiff(party.recentDeathMs.front(), now) > DEATH_BURST_WINDOW_MS)
                    party.recentDeathMs.pop_front();
                if (party.recentDeathMs.size() >= DEATH_BURST_COUNT && !party.deathBurstMs)
                {
                    party.deathBurstMs = now ? now : 1;
                    party.deathBurstCount = uint32(party.recentDeathMs.size());
                    LogEvent(party, "death_burst",
                             JsonObject().Num("deaths", party.deathBurstCount)
                                 .Num("windowSec", DEATH_BURST_WINDOW_MS / IN_MILLISECONDS)
                                 .Num("recoverSec", DEATH_BURST_RECOVER_MS / IN_MILLISECONDS));
                }
            }
            if (activeBoss && !reentry)
                activeBoss->attemptDeaths.push_back(death.name);
            // [queue-wipe-during-boss] a death in the tick its boss's attempt reset is still that boss's
            bool const justEnded = party.lastAttemptEndMs && getMSTimeDiff(party.lastAttemptEndMs, now) <= 2000;
            LogEvent(party, "death", JsonObject().Str("name", death.name).Str("killer", death.killer)
                                         .Num("killerEntry", death.killerEntry)
                                         .Str("during", activeBoss ? activeBoss->name
                                                        : justEnded ? party.lastAttemptEndBoss
                                                                    : std::string("trash")));
            // [b33_mech2] the boss this death fell during (empty: trash), for a wipe found later
            if (!reentry && !party.wiped)
            {
                WipeBossNote& note = wipeBossNotes[party.seq];
                note.boss = activeBoss ? activeBoss->name : justEnded ? party.lastAttemptEndBoss : std::string();
                note.ms = now ? now : 1;
            }
        }

        TrackAggro(party, players);
        // Out of combat too, every POSITIONS_SETUP_MS, while a pull is being set up: ready check / buffing, a body
        // pull (party.approach), a careful guarantee hold, a boss target (occ-h build 15: the positions stopped
        // before each Drakos body pull, the planner could not see the set-up).
        // [downtime_positions] DOWNTIME_AUDIT 2026-10-25: with positions only during a pull's set-up, a third of the
        // downtime (post-fight settle 6,315 s, walk stalled 5,656 s) had nothing but the 30 s heartbeat: out of combat
        // they are written every POSITIONS_SETUP_MS whatever the phase.
        bool const pullSetup = DOWNTIME_POSITIONS || party.phase == Phase::ReadyCheck ||
                               party.phase == Phase::Buffing || party.approach || party.guaranteeHolding ||
                               (party.targetIsBoss && !party.target.IsEmpty());
        if ((anyInCombat && getMSTimeDiff(party.lastPositionsMs, now) >= POSITIONS_MS) ||
            (!anyInCombat && pullSetup && getMSTimeDiff(party.lastPositionsMs, now) >= POSITIONS_SETUP_MS))
        {
            party.lastPositionsMs = now;
            LogPositions(party, players);
            if (anyInCombat)
                LogAuraStacks(party, players, "tick");  // [harness30]
        }

        // [b34_arc] arc-h 20261026-014521-arc-h-5F-31 (696-1898 s): a Warder Corpse (21304, Arcatraz rooted trap;
        // SmartAI bursts and despawns on aggro or on a player within 8 yd out of combat) was shot by the planner's
        // "pull back"; the hit put it in combat with the group but its AI never engaged (no threat ref, so no victim,
        // no aggro event, no burst, taunt does nothing: EffectTaunt needs a threat list). In combat it never bursts
        // and a PvE reference never times out: 20 min "in combat on nobody", the rogue's and the AoE's hits reset the
        // phantom clock below every few seconds, so its 45 s end came twice. A creature in combat with a member that
        // its AI has not engaged (CreatureAI::IsEngaged false), with no victim, standing, is no fight: that
        // reference is ended whatever the damage (the trap then bursts on the next walk past it, out of combat).
        if (anyInCombat && !activeBoss)
        {
            // [lead128] user: no elapsed-time rule decides (UNENGAGED_REF_MS 20 s was the old clock). The facts: in
            // combat, its AI never engaged (CreatureAI::IsEngaged), no victim, not evading and standing still - a
            // creature that fights chases a victim; such a reference is no fight and ends at once
            std::vector<std::pair<Player*, ObjectGuid>> stale;  // looked up again at the end (an end may free refs)
            for (Player* bot : players)
            {
                if (!bot->IsAlive())
                    continue;
                for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                {
                    Unit* other = ref ? ref->GetOther(bot) : nullptr;
                    Creature* creature = other ? other->ToCreature() : nullptr;
                    if (!creature || !creature->IsAlive() || !creature->IsInCombat() || creature->IsEngaged() ||
                        creature->GetVictim() || creature->IsInEvadeMode() || creature->isMoving() ||
                        creature->IsDungeonBoss() || creature->isWorldBoss() || creature->IsPet() ||
                        creature->IsGuardian())
                        continue;
                    stale.emplace_back(bot, guid);
                }
            }
            for (auto const& [bot, guid] : stale)
            {
                auto const& refs = bot->GetCombatManager().GetPvECombatRefs();
                auto const found = refs.find(guid);
                CombatReference* ref = found != refs.end() ? found->second : nullptr;
                if (!ref)
                    continue;
                Unit* first = ref->first;
                Unit* second = ref->second;
                LOG_INFO("playerbots", "[b34_arc] {}: ends the combat of {} with {} (in combat, never engaged by its "
                         "AI, no victim, standing)", party.id, first ? first->GetName() : "?",
                         second ? second->GetName() : "?");
                LogEvent(party, "unengaged_ref_end",
                         JsonObject().Str("a", first ? first->GetName() : "").Str("b", second ? second->GetName() : "")
                             .Num("entry", second && second->IsCreature() ? second->GetEntry()
                                           : first && first->IsCreature() ? first->GetEntry() : 0));
                ref->EndCombat();
            }
        }

        TickPhantomCombat(party, players, leader, now, activeBoss != nullptr, anyInCombat);

        TickStaleVictim(party, leader, now, anyInCombat, activeBoss != nullptr);

        // trash_phantom (build 18, mgt-h build 17: a planned trash pull stayed "in fight" for 12+ minutes): in a
        // trash fight no mob of the party's combat lost health for TRASH_PHANTOM_LOG_MS - logged once with every
        // member's position, line of sight and target; TRASH_PHANTOM_END_MS ends the run (5-man only).
        if (anyInCombat && !activeBoss && map)
        {
            uint64 hp = 0;
            uint32 mobs = 0;
            JsonArray refs;
            for (Player* bot : players)
            {
                if (!bot->IsAlive() || !bot->IsInCombat())
                    continue;
                for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                    // Only mobs the group can hurt (an immune event NPC in the combat is no fight).
                    if (Unit* other = ref ? ref->GetOther(bot) : nullptr;
                        other && other->IsCreature() && other->IsAlive() &&
                        !other->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) && !other->IsImmuneToPC())
                    {
                        hp += other->GetHealth();
                        ++mobs;
                    }
            }
            if (!party.trashFlatSinceMs || !mobs || hp < party.trashHp || mobs != party.trashMobs)
            {
                party.trashHp = hp;
                party.trashMobs = mobs;
                party.trashFlatSinceMs = now ? now : 1;
                party.trashPhantomLogged = false;
            }
            else
            {
                party.trashHp = hp;  // regen counts as no progress
                uint32 const flatMs = getMSTimeDiff(party.trashFlatSinceMs, now);
                if (flatMs > TRASH_PHANTOM_LOG_MS && !party.trashPhantomLogged)
                {
                    party.trashPhantomLogged = true;
                    for (Player* bot : players)
                    {
                        if (!bot->IsAlive() || !bot->IsInCombat())
                            continue;
                        for (auto const& [guid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                            if (Unit* other = ref ? ref->GetOther(bot) : nullptr; other && other->IsCreature())
                                refs.Raw(JsonObject().Str("member", bot->GetName()).Str("with", other->GetName())
                                             .Num("entry", other->GetEntry()).Real("hpPct", other->GetHealthPct())
                                             .Bool("evading", other->ToCreature()->IsInEvadeMode())
                                             .Real("dist", bot->GetExactDist(other))
                                             .Real("dz", other->GetPositionZ() - bot->GetPositionZ())
                                             .Bool("los", bot->IsWithinLOSInMap(other))
                                             .Str("memberTarget", bot->GetVictim() ? bot->GetVictim()->GetName() : "")
                                             .Real("x", other->GetPositionX()).Real("y", other->GetPositionY())
                                             .Real("z", other->GetPositionZ()).Build());
                    }
                    LogEvent(party, "trash_phantom", JsonObject().Num("sec", flatMs / IN_MILLISECONDS)
                                                         .Num("mobs", mobs).Raw("refs", refs.Build()));
                    LogPositions(party, players);
                }
                if (flatMs > TRASH_PHANTOM_END_MS && !party.content->raid)
                {
                    Finish(party, "failed",
                           "trash fight idle: no mob lost health for " + std::to_string(flatMs / IN_MILLISECONDS) +
                               " s (" + std::to_string(mobs) + " mobs in combat)");
                    return;
                }
            }
        }
        else
            party.trashFlatSinceMs = 0;

        // The last dry place of the leader inside, out of combat: where the group rests and where a member who stays
        // under water goes (bots do not surface to breathe).
        if (map && leader->IsAlive() && leader->GetMap() == map && !leader->IsInCombat() &&
            !leader->Unit::IsInWater() && !leader->IsFlying() &&
            !InHarmfulLiquid(leader, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
        {
            party.dryPos = leader->GetPosition();
            party.hasDryPos = true;
        }
        LeaveWater(party, players, false);

        // Pull start / end.
        if (anyInCombat && !party.inCombat)
        {
            party.inCombat = true;
            ++party.pullSeq;
            party.pullStartMs = now;
            party.pullKills = 0;
            party.pullDeaths.clear();
            party.pullIsBoss = activeBoss != nullptr;
            for (Member& member : party.members)
            {
                member.pullStart = StatsRegistry::Instance().Snapshot(member.guid);
                member.pullStartCombatMs = member.combatMs;
            }
            TestRunner::ClearWaypoint(party.leader);
            ClearMemberIntent(party);  // a member's "come here" does not outlive the fight
            // A fight ends the no-target wait (event waves come to the group: the wait restarts after each wave).
            party.noTargets = false;
            party.eventGatherLogged = false;
            party.eventGatherArrived = false;
            if (!activeBoss)
                LogPull(party, players, false, "");
            LogPullStart(party, players);
            CheckPullHealer(party, players);
        }
        else if (!anyInCombat && party.inCombat)
        {
            party.inCombat = false;
            JsonArray deaths;
            for (std::string const& name : party.pullDeaths)
                deaths.Str(name);
            party.lastPullEndMs = getMSTime() ? getMSTime() : 1;  // [pull_label]
            LogEvent(party, "pull_end", JsonObject().Num("pull", party.pullSeq)
                                            .Num("durationSec", getMSTimeDiff(party.pullStartMs, now) / IN_MILLISECONDS)
                                            .Num("kills", party.pullKills).Raw("deaths", deaths.Build())
                                            .Raw("members", PullMembersJson(party))
                                            .Str("starter", party.pullStarter)
                                            .Raw("opener", PullOpenerJson(party, players, false)));
            StatsRegistry::Instance().ResetOpener(party.seq);
            party.lastProgressMs = now;
            // The next target (and its walk) right after the fight, not on the 4 s retarget clock.
            party.nextRetargetMs = now;
        }

        EmulateAreaTriggers(party, players);
        WatchEventState(party, leader->GetMap());

        // Bots walk by themselves: what their own movement could not do (bot_cannot_move), and a walked wing change.
        DrainMoveReports(party, players);
        DrainOrderReports(party, leader);  // [lead1]
        // [lead342] the wing walk is the leader's (RouteReenter.h)
        bool const wingWalking = [&]()
        {
            PartyLeaderGroup group(*this, party);
            return Leader::Travel::TickWingWalk(group, players, leader);
        }();
        if (IsTerminal(party.phase))
            return;  // Finish() logged the members out
        if (wingWalking)
            party.lastEvent = "wing change walk";

        // [b30_st_spirit] A death burst not recovered from in DEATH_BURST_RECOVER_MS: a wipe like the full one.
        // Recovered once everybody is alive and inside again (members left outside count as back, as in the away list).
        bool burstWipe = false;
        if (party.deathBurstMs && !party.wiped)
        {
            bool anyAway = false;
            for (Member const& member : party.members)
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || !bot->IsInWorld())
                    continue;
                bool const outside = bot->GetMapId() != party.content->mapId || bot->IsBeingTeleported();
                anyAway |= !bot->IsAlive() || (outside && !member.leftOutside);
            }
            if (!anyAway)
            {
                party.deathBurstMs = 0;
                party.recentDeathMs.clear();
            }
            else if (getMSTimeDiff(party.deathBurstMs, now) > DEATH_BURST_RECOVER_MS && alive)
                burstWipe = true;
        }

        // [b33_mech2] A lone survivor is a wipe at once: every other member dead and each one alive out of combat and
        // hidden (Invisibility / stealth / Feign Death) or stranded (MemberStranded) - nobody fights on or resurrects.
        // mech-n 20261026-000434-mech-n-5D-17: four dead to Sepethrea at 1214-1235 s, the mage alive in Invisibility;
        // the wipe waited for the death burst's DEATH_BURST_RECOVER_MS and came at 1341 s, after the corpse runs, as
        // "trash, partial".
        constexpr uint32 MAGE_INVISIBILITY_FADE = 66;  // the fading aura before the invisibility itself (32612)
        bool loneSurvivorWipe = false;
        std::string loneSurvivors;
        if (alive && !party.wiped)
        {
            uint32 dead = 0;
            bool active = false;
            for (Member const& member : party.members)
            {
                Player* bot = ObjectAccessor::FindPlayer(member.guid);
                if (!bot || !bot->IsInWorld())
                    continue;
                if (!bot->IsAlive())
                {
                    ++dead;
                    continue;
                }
                bool const outside = bot->GetMapId() != party.content->mapId || bot->IsBeingTeleported();
                bool const hidden = bot->HasAuraType(SPELL_AURA_MOD_INVISIBILITY) ||
                                    bot->HasAuraType(SPELL_AURA_MOD_STEALTH) ||
                                    bot->HasAuraType(SPELL_AURA_FEIGN_DEATH) || bot->HasAura(MAGE_INVISIBILITY_FADE);
                if (outside || bot->IsInCombat() || (!hidden && !member.stranded))
                {
                    active = true;
                    break;
                }
                loneSurvivors += (loneSurvivors.empty() ? "" : ",") + member.name +
                                 (hidden ? "(hidden)" : "(stranded)");
            }
            loneSurvivorWipe = !active && dead > 0;
        }

        // Wipe and corpse run: the bots release, run back and resurrect on their own.
        if ((!alive || burstWipe || loneSurvivorWipe) && !party.wiped)
        {
            party.wiped = true;
            party.wipeMs = now;
            ++party.wipes;
            party.inCombat = false;
            TestRunner::ClearWaypoint(party.leader);
            // [queue-wipe-during-boss] the boss's attempt closes as "reset" in the very tick the last member dies
            // (the script resets the encounter once nobody is left), so activeBoss is already gone: sh-h Kargath was
            // logged as a wipe "during trash" (WIPE_ANALYSIS_w24_25). An engaged attempt that ended within
            // WIPE_ATTEMPT_END_MS names the wipe.
            constexpr uint32 WIPE_ATTEMPT_END_MS = 5 * IN_MILLISECONDS;
            std::string wipeDuring =
                activeBoss ? activeBoss->name
                : (party.lastAttemptEndMs && getMSTimeDiff(party.lastAttemptEndMs, now) <= WIPE_ATTEMPT_END_MS)
                    ? party.lastAttemptEndBoss
                    : std::string("trash");
            // [b33_mech2] a wipe found after its attempt closed (death burst, lone survivor) is the boss's its last
            // death fell during (wipeBossNotes), within the burst's window and recovery time
            if (wipeDuring == "trash" && (burstWipe || loneSurvivorWipe))
                if (auto const note = wipeBossNotes.find(party.seq);
                    note != wipeBossNotes.end() && !note->second.boss.empty() &&
                    getMSTimeDiff(note->second.ms, now) <= DEATH_BURST_WINDOW_MS + DEATH_BURST_RECOVER_MS)
                    wipeDuring = note->second.boss;
            LogEvent(party, "wipe", JsonObject().Str("during", wipeDuring).Bool("boss", wipeDuring != "trash")
                                        .Num("wipes", party.wipes)
                                        .Bool("partial", burstWipe && !loneSurvivorWipe)
                                        .Num("alive", alive)
                                        .Num("burstDeaths", burstWipe ? party.deathBurstCount : 0)
                                        .Str("loneSurvivor", loneSurvivorWipe ? loneSurvivors : std::string()));
            party.deathBurstMs = 0;
            party.recentDeathMs.clear();
            SetPhase(party, Phase::Reviving);

            // Nobody should die (user, 2026-10-23), but one death and one wipe are allowed so the recovery itself
            // (res, corpse run, re-entry, rebuff) is exercised. The second wipe ends the run for a fix.
            if (party.wipes >= MAX_WIPES_PER_RUN)
            {
                Finish(party, "failed", "wipe " + std::to_string(party.wipes) + " during " + wipeDuring);
                return;
            }
        }

        // Members dead or outside the instance (ghost at the graveyard, revived at a spirit healer outside).
        // A member left outside (Member::leftOutside: alive or a ghost, no walk back for three minutes) is not
        // waited for while it is still outside (bfd-h build 10: a ghost healer with "no walk to the portal" of the
        // underwater Blackfathom entrance held the group 265 s).
        std::vector<std::string> away;
        uint32 leftDead = 0;
        for (Member const& member : party.members)
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || !bot->IsInWorld())
                continue;
            bool const outside = bot->GetMapId() != party.content->mapId || bot->IsBeingTeleported();
            if (member.leftOutside && outside)
            {
                leftDead += bot->IsAlive() ? 0 : 1;
                continue;
            }
            if (!bot->IsAlive())
                away.push_back(member.name + "(dead)");
            else if (outside)
                away.push_back(member.name + "(outside)");
        }

        if (!party.wiped)
        {
            // Not a wipe, but somebody is dead or outside: no pull until everybody is back (Deadmines: Cookie was
            // pulled with the healer dead). The ghost walks / the revived member flies back to the portal.
            if (!away.empty())
            {
                if (!party.membersAway)
                {
                    party.membersAway = true;
                    party.awaySinceMs = now;
                    JsonArray names;
                    for (std::string const& name : away)
                        names.Str(name);
                    LogEvent(party, "members_away", JsonObject().Raw("members", names.Build()));
                }

                // A dead member releases its spirit by itself (AutoReleaseSpiritAction, ReleaseSpiritAction's
                // self-resurrect wait; user rule 2026-10-24).
                GuideGhosts(party, players);
                if (IsTerminal(party.phase))
                    return;
                // Per member: Upper Blackrock Spire wave 6 failed "did not come back in 20 minutes" although every
                // ghost came back - the mage died again and again to the Rage Talon Dragonspawn, so somebody was
                // always away and the party's away timer never restarted.
                for (Member& member : party.members)
                {
                    Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bool const away = bot && bot->IsInWorld() &&
                                      (!bot->IsAlive() || bot->GetMapId() != party.content->mapId ||
                                       bot->IsBeingTeleported());
                    if (!away)
                    {
                        member.awaySinceMs = 0;
                        continue;
                    }
                    if (!member.awaySinceMs)
                        member.awaySinceMs = now;
                    if (getMSTimeDiff(member.awaySinceMs, now) > AWAY_MAX_MS)
                    {
                        Finish(party, "failed", member.name + " (dead / outside) did not come back in 20 minutes");
                        return;
                    }
                }
            }
            else if (party.membersAway)
            {
                party.membersAway = false;
                LogEvent(party, "members_back",
                         JsonObject().Num("awaySec", getMSTimeDiff(party.awaySinceMs, now) / IN_MILLISECONDS));
                party.needBuff = true;
                party.buffStep = 0;
                party.buffReason = "after a death";
            }
        }

        if (party.wiped)
        {
            // Who is away now (hor-h 2026-10-25: "Rtdruan(dead),Rtmagan(dead)" from before the wipe stood in every
            // idle line for 6 minutes while both were alive inside).
            party.waitingFor.clear();
            for (std::string const& name : away)
                party.waitingFor += (party.waitingFor.empty() ? "" : ",") + name;
            GuideGhosts(party, players);
            if (IsTerminal(party.phase))
                return;
            // Recovered only when everybody is alive AND back inside (Mechanar: the group revived at the Netherstorm
            // graveyard, was declared recovered and stood there).
            if (alive + leftDead == party.members.size() && away.empty())
            {
                uint32 const corpseRun = getMSTimeDiff(party.wipeMs, now);
                party.corpseRunMs += corpseRun;
                // The corpse run does not count against boss progress - only its part after the last progress: a boss
                // an event finished while the group was dead (Halls of Stone heroic 2026-10-25: the Tribunal went DONE
                // 51 s before the recovery) put lastBossProgressMs + corpseRun in the future, getMSTimeDiff wrapped
                // and no_boss_progress ("25 min") ended the run on the recovery tick.
                party.lastBossProgressMs += std::min(corpseRun, getMSTimeDiff(party.lastBossProgressMs, now));
                party.loopSinceMs = now;
                party.loopAnchor = leader->GetPosition();
                party.loopKills = party.kills;
                party.wiped = false;
                party.membersAway = false;  // the wipe recovery already rebuffs
                party.needBuff = true;
                party.buffStep = 0;
                party.buffReason = "after wipe";
                LogEvent(party, "recovered", JsonObject().Num("corpseRunSec", corpseRun / IN_MILLISECONDS));
                SetPhase(party, Phase::Buffing);
            }
            else if (getMSTimeDiff(party.wipeMs, now) > 20 * MINUTE * IN_MILLISECONDS)
            {
                Finish(party, "failed", "corpse run did not finish in 20 minutes");
                return;
            }
        }

        if (getMSTimeDiff(party.runStartMs, now) > party.timeoutMs)
        {
            Finish(party, "timeout", "run timeout");
            return;
        }

        // User rule: no boss killed (or failed) for that long is a failure - stop and find the cause.
        uint32 noProgressMs = party.content->raid ? NO_BOSS_PROGRESS_RAID_MS : NO_BOSS_PROGRESS_DUNGEON_MS;
        // [scholo-progress-window] user 2026-10-25: Scholomance's first boss (Jandice) sits behind the long
        // Necromancer/Adept halls; scholo-h-5A-4 cleared 191 mobs with no wipe and was cut at 25 min short of her.
        // user 2026-10-25: the big classic dungeons get a longer window too (long trash halls between bosses).
        static std::set<std::string> const bigDungeons = {"scholo-n", "scholo-h", "strat-n", "strat-h", "brd-n",
            "brd-h", "brs-h", "lbrs-n", "ubrs-n", "dmw-n", "dme-n", "dme-h", "dmn-n", "mara-n", "mara-h",
            "st-n", "st-h", "gnomer-n", "gnomer-h", "ulda-n", "ulda-h"};
        std::string const progressKey = party.content->key;
        if (bigDungeons.count(progressKey))
            noProgressMs = 45 * MINUTE * IN_MILLISECONDS;
        // A boss attempt under way is not "no progress" (strat-n build 10: The Unforgiven engaged at 1457 s, the
        // rule ended the run 43 s into the fight): the fight gets up to BOSS_ATTEMPT_GRACE_MS.
        bool const attemptRunning = activeBoss && activeBoss->attemptStartMs &&
                                    getMSTimeDiff(activeBoss->attemptStartMs, now) < BOSS_ATTEMPT_GRACE_MS;
        // [progress-clock-future] event progress may stamp getMSTime() later in the same tick than this "now"
        // (bm-n build 27: rift 1 at 305 s, getMSTimeDiff wrapped and ended the run "no boss progress for 25 min").
        uint32 const sinceProgressMs =
            party.lastBossProgressMs > now ? 0 : getMSTimeDiff(party.lastBossProgressMs, now);
        if (!party.wiped && !attemptRunning && sinceProgressMs > noProgressMs)
        {
            uint32 const minutes = noProgressMs / MINUTE / IN_MILLISECONDS;
            LogEvent(party, "no_boss_progress",
                     JsonObject().Num("minutes", minutes).Str("phase", PhaseLabel(party)).Num("map", leader->GetMapId())
                         .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                         .Real("z", leader->GetPositionZ())
                         .Num("killsInWindow", party.kills - party.killsAtBossProgress)
                         .Str("topKills", TopKills(party.killsSinceBoss, 5))
                         .Num("bossesKilled", party.bossProgressCount).Num("bossesTotal", RequiredBossCount(party))
                         .Str("lastEvent", party.lastEvent));
            Finish(party, "failed", "no boss progress for " + std::to_string(minutes) + " min");
            return;
        }

        // boss_attempt_loop (build 21, mgt-h build 20: Selin's attempt ran 12 minutes while Wretched trash / Fel
        // Crystals kept dying, trash_loop logged and nothing ended it): an engaged boss attempt older than
        // BOSS_ATTEMPT_LOOP_MS with TRASH_LOOP_MIN_KILLS or more trash kills since it began ends the run (5-man), the
        // looping entries named.
        // [hleft-an-hadronox] Hadronox climbs only after all three Crushers die plus her 70 s MOVE3: slow by design,
        // never a loop before she has webbed the front doors (ANAdds::HadronoxHandsOff).
        PlayerbotAI* const leaderAI = GET_PLAYERBOT_AI(leader);
        bool const hadronoxClimbs = leaderAI && leader->GetMapId() == ANWatcherRoom::MAP_AZJOL_NERUB &&
                                    ANAdds::HadronoxHandsOff(leaderAI);
        // [w24c-hor-escape] The Halls of Reflection escape (hor-h 20261025-121621: all four ice walls passed, the
        // group at the end of the corridor (5269, 1678) when "5 min, 51 kills" fired at 1320 s) is a wave event:
        // each broken ice wall is progress and restarts the loop window; the wall state is logged.
        if (activeBoss && activeBoss->attemptStartMs && map)
            WatchHorEscapeWalls(party, map, *activeBoss, leader, now);
        bool const sinceProgress = activeBoss && activeBoss->loopProgressMs;
        uint32 const loopBaseMs =
            !activeBoss ? 0 : sinceProgress ? activeBoss->loopProgressMs : activeBoss->attemptStartMs;
        uint32 const loopBaseKills =
            !activeBoss ? 0 : sinceProgress ? activeBoss->loopProgressKills : activeBoss->attemptKills;
        uint32 loopWindowMs = BOSS_ATTEMPT_LOOP_MS;
        if (activeBoss && activeBoss->lastState == IN_PROGRESS)
            for (ScriptedEventLoopDef const& def : SCRIPTED_EVENT_LOOP)
                if (def.mapId == party.content->mapId && def.bossId == activeBoss->bossId)
                    loopWindowMs = std::max(loopWindowMs, def.windowMs);
        if (activeBoss && activeBoss->attemptStartMs && !party.content->raid && !party.wiped && !hadronoxClimbs &&
            getMSTimeDiff(loopBaseMs, now) > loopWindowMs &&
            party.kills - loopBaseKills >= TRASH_LOOP_MIN_KILLS)
        {
            std::string const looping = TopKills(party.killsSinceBoss, 5);
            uint32 const minutes = getMSTimeDiff(loopBaseMs, now) / MINUTE / IN_MILLISECONDS;
            LogEvent(party, "boss_attempt_loop",
                     JsonObject().Str("boss", activeBoss->name).Num("minutes", minutes)
                         .Num("windowMin", loopWindowMs / MINUTE / IN_MILLISECONDS)
                         .Num("kills", party.kills - loopBaseKills).Str("topKills", looping)
                         .Num("horWallsBroken", activeBoss->horWallsBroken)
                         .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                         .Real("z", leader->GetPositionZ()));
            Finish(party, "failed",
                   "boss attempt loop: " + activeBoss->name + (sinceProgress ? " last progress " : " engaged ") +
                       std::to_string(minutes) + " min, " + std::to_string(party.kills - loopBaseKills) +
                       " trash kills (" + looping + ")");
            return;
        }

        // Trash loop: kills keep coming in one area without boss progress.
        if (map && leader->IsAlive() && leader->GetMap() == map && !party.wiped)
        {
            uint32 const loopMs = party.content->raid ? TRASH_LOOP_RAID_MS : TRASH_LOOP_DUNGEON_MS;
            if (leader->GetExactDist(&party.loopAnchor) > TRASH_LOOP_RADIUS)
            {
                party.loopAnchor = leader->GetPosition();
                party.loopSinceMs = now;
                party.loopKills = party.kills;
            }
            // [trash_loop_progress] plan29 follow-up 2 (scholo-n 20261025-163739-8, 1077 s: "trash loop" over 48
            // kills in 6 min while the group cleared the hall - the top kills were summons, Frail Skeleton x62 and
            // Spectral Projection x25 - 59 live mobs skipped, then a boss rush through them and a wipe): kills are a
            // loop only when the spawned hostiles round the anchor do not get fewer. Counted once per window.
            auto const aliveSpawned = [&]()
            {
                uint32 alive = 0;
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                    if (creature && creature->IsAlive() && !creature->IsSummon() && !creature->IsDungeonBoss() &&
                        creature->IsHostileTo(leader) &&
                        creature->GetExactDist(&party.loopAnchor) <= TRASH_LOOP_RADIUS)
                        ++alive;
                return alive;
            };
            if (party.loopAliveSinceMs != party.loopSinceMs)
            {
                party.loopAliveSinceMs = party.loopSinceMs;
                party.loopAliveAtStart = aliveSpawned();
            }
            bool loopDue = getMSTimeDiff(party.loopSinceMs, now) > loopMs &&
                           party.kills - party.loopKills >= TRASH_LOOP_MIN_KILLS;
            if (loopDue)
            {
                uint32 const aliveNow = aliveSpawned();
                if (party.loopAliveAtStart >= aliveNow + TRASH_LOOP_MIN_PROGRESS)
                {
                    LogEvent(party, "trash_loop_progress",
                             JsonObject().Num("kills", party.kills - party.loopKills)
                                 .Num("aliveAtStart", party.loopAliveAtStart).Num("aliveNow", aliveNow)
                                 .Str("topKills", TopKills(party.killsSinceBoss, 5)));
                    party.loopSinceMs = now;
                    party.loopKills = party.kills;
                    party.loopAliveSinceMs = now;
                    party.loopAliveAtStart = aliveNow;
                    loopDue = false;
                }
            }
            if (loopDue)
            {
                // [lead346] user: «Стенд никого не двигает»: a report only - the harness no longer skips the area's
                // trash, rushes the boss, drops the leader's target or clears its walk
                LogEvent(party, "trash_loop",
                         JsonObject().Num("minutes", loopMs / MINUTE / IN_MILLISECONDS)
                             .Num("kills", party.kills - party.loopKills).Real("x", party.loopAnchor.GetPositionX())
                             .Real("y", party.loopAnchor.GetPositionY()).Real("z", party.loopAnchor.GetPositionZ())
                             .Str("action", "report only").Str("topKills", TopKills(party.killsSinceBoss, 5)));
                party.loopAnchor = leader->GetPosition();
                party.loopSinceMs = now;
                party.loopKills = party.kills;
            }
        }

        // Violet Hold: a released prisoner nobody fights (vh-h / vh-n 2026-10-25: Xevozz / Erekem stood released 17
        // minutes, the tank "attacked" from 30-48 yd and the bots' "vh rally" walked it back every tick).
        if (map && map->GetId() == VIOLET_HOLD_MAP && !party.wiped)
        {
            Creature* released = nullptr;
            for (ObjectGuid const& guid : party.bossCreatures)
                // Released means attackable for the group: the cell door and the yell come before (vh-h wave 9:
                // Zuramat "not engaged for 2 min" while still walking out, not yet a valid target).
                if (Creature* boss = map->GetCreature(guid);
                    boss && boss->IsAlive() && !boss->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) &&
                    !boss->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && !boss->IsImmuneToPC() && !boss->IsInCombat() &&
                    IsVioletHoldPrisoner(boss) && leader->IsValidAttackTarget(boss))
                    released = boss;
            if (!released || anyInCombat)
                party.releasedIdleSinceMs = 0;
            else if (!party.releasedIdleSinceMs)
                party.releasedIdleSinceMs = now;
            else if (getMSTimeDiff(party.releasedIdleSinceMs, now) > RELEASED_BOSS_IDLE_MS)
            {
                PlayerbotAI* tankAI = GET_PLAYERBOT_AI(leader);
                Unit* victim = leader->GetVictim();
                LogEvent(party, "boss_not_engaged",
                         JsonObject().Str("name", released->GetName()).Num("entry", released->GetEntry())
                             .Real("distance", leader->GetExactDist(released))
                             .Bool("tankCombatEngine", tankAI && tankAI->GetState() == BOT_STATE_COMBAT)
                             .Str("tankVictim", victim ? victim->GetName() : "")
                             .Bool("nonAttackable", released->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
                             .Bool("notSelectable", released->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                             .Bool("immuneToPc", released->IsImmuneToPC())
                             .Bool("validTarget", leader->IsValidAttackTarget(released)));
                Finish(party, "failed", "released boss " + released->GetName() + " not engaged for 2 min");
                return;
            }
        }

        // Every boss dead: done once out of combat, or BOSSES_DONE_COMBAT_MS after the last kill whatever is still
        // flagged (Oculus: after Eregos the bots sit on their drakes in combat with the drake adds).
        bool const settled = !anyInCombat || (party.lastBossProgressMs &&
                                               party.lastBossProgressMs <= now &&
                                               getMSTimeDiff(party.lastBossProgressMs, now) > BOSSES_DONE_COMBAT_MS);
        if (map && settled && AllBossesResolved(party, map))
        {
            Finish(party, "done", "all bosses resolved");
            return;
        }

        if (!party.wiped)
        {
            if (anyInCombat)
            {
                // [regen_clock_combat_reset] A fight ends the regen stretch: the sit-down grace starts again after it
                // (all 1285 tests/20261025-*.jsonl: 82 % of the 2658 "does not sit down" stalls were logged in the
                // second of pull_end, the clock had run on from before the fight).
                for (Member& member : party.members)
                {
                    Leader::MemberRest& rest = party.rest[member.guid];  // [lead107] the leader's rest facts
                    rest.regenSinceMs = 0;
                    rest.regenStallLogged = false;
                    rest.regenStallMs = 0;
                    rest.regenAboveLineLogged = false;
                }
                SetPhase(party, activeBoss ? Phase::BossFight : Phase::Clearing);
                // [lead1022] a fight under an open buff pass: the pass waits for it (TickBuffing does not run), its
                // step's seconds do not count the fight (b229 5G-15 555-588 s: step 1 "33 s" was pull 33's 29 s)
                if (party.needBuff && party.buffStep >= 1 && !party.buffFightMs)
                    party.buffFightMs = now ? now : 1;
                {
                    PartyLeaderGroup group(*this, party);
                    Leader::Travel::AssistMember(group, players, leader);  // [lead345] the leader's (RouteMeet.h)
                }
                ReopenKeyDoorInCombat(party, leader);
                if (activeBoss)
                {
                    uint32 const fightMs = getMSTimeDiff(activeBoss->attemptStartMs, now);
                    // boss_phantom (build 11, mgt-n Selin / brs-h Zigris: an attempt "running" while nobody hurt the
                    // boss): no change of the boss's health for BOSS_PHANTOM_LOG_MS logs every member's position,
                    // distance, line of sight and target once; BOSS_PHANTOM_END_MS ends the run with that reason.
                    // The harness never resets the boss.
                    if (Creature* boss = map ? map->GetCreature(activeBoss->creature) : nullptr;
                        boss && boss->IsAlive())
                    {
                        // A phase out of reach (Skadi on his drake, a submerge) is no phantom: the clock restarts.
                        // Neither is an escape / escort encounter, where the boss is never damaged (Halls of
                        // Reflection: the group runs from The Lich King and kills the waves at the ice walls while
                        // Jaina / Sylvanas break them - hor-n build 17 ended "no damage on The Lich King for 180 s"):
                        // the boss entries of BOSS_PHANTOM_EXEMPT, and any boss while an escort runs.
                        bool const outOfReach = boss->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
                                                boss->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || boss->IsImmuneToPC() ||
                                                !party.escort.IsEmpty() ||
                                                std::find(BOSS_PHANTOM_EXEMPT.begin(), BOSS_PHANTOM_EXEMPT.end(),
                                                          boss->GetEntry()) != BOSS_PHANTOM_EXEMPT.end();
                        if (activeBoss->phantomAttemptMs != activeBoss->attemptStartMs ||
                            boss->GetHealth() != activeBoss->phantomHp || outOfReach)
                        {
                            activeBoss->phantomAttemptMs = activeBoss->attemptStartMs;
                            activeBoss->phantomHp = boss->GetHealth();
                            activeBoss->phantomSinceMs = now;
                            activeBoss->phantomLogged = false;
                        }
                        uint32 const flatMs = getMSTimeDiff(activeBoss->phantomSinceMs, now);
                        if (flatMs > BOSS_PHANTOM_LOG_MS && !activeBoss->phantomLogged)
                        {
                            activeBoss->phantomLogged = true;
                            JsonArray members;
                            for (Player* bot : players)
                                members.Raw(JsonObject().Str("name", bot->GetName()).Bool("alive", bot->IsAlive())
                                                .Bool("inCombat", bot->IsInCombat())
                                                .Real("distance", bot->GetExactDist(boss))
                                                .Bool("los", bot->IsWithinLOSInMap(boss))
                                                .Real("dz", bot->GetPositionZ() - boss->GetPositionZ())
                                                .Str("target", bot->GetVictim() ? bot->GetVictim()->GetName() : "")
                                                .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                                                .Real("z", bot->GetPositionZ()).Build());
                            LogEvent(party, "boss_phantom",
                                     JsonObject().Str("boss", activeBoss->name).Num("entry", boss->GetEntry())
                                         .Num("sec", flatMs / IN_MILLISECONDS).Real("hpPct", boss->GetHealthPct())
                                         .Bool("bossInCombat", boss->IsInCombat())
                                         .Bool("evading", boss->IsInEvadeMode())
                                         .Str("bossVictim", boss->GetVictim() ? boss->GetVictim()->GetName() : "")
                                         .Real("x", boss->GetPositionX()).Real("y", boss->GetPositionY())
                                         .Real("z", boss->GetPositionZ()).Raw("members", members.Build()));
                        }
                        if (flatMs > BOSS_PHANTOM_END_MS && !party.content->raid)
                        {
                            Finish(party, "failed",
                                   "boss attempt idle: no damage on " + activeBoss->name + " for " +
                                       std::to_string(flatMs / IN_MILLISECONDS) + " s");
                            return;
                        }
                    }
                    // [lead344] no item use by the harness in a boss fight (it used Hyperspeed Acceleration at 2 s
                    // and a potion at 60 s): a member's consumables are the leader's and the bot's own
                }
                else
                {
                    // [b30_mech_pos] trash_stalemate: a trash fight with no kill and no enemy at a new lowest health
                    // for STALEMATE_FOCUS_MS (heals, flee for help, leeches undo the damage) gets one line and a
                    // decision - the Skull on its weakest healer, else its weakest mob; STALEMATE_END_MS more and the
                    // run ends with that reason. zf-h 20261025-184229-zf-h-5E-62: 12 Sandfury at the pool, Witch
                    // Doctors' heals and Blood Drinkers fleeing at 15 %, 287 s and 0 kills until the wipe.
                    constexpr uint32 STALEMATE_FOCUS_MS = 90 * IN_MILLISECONDS;
                    constexpr uint32 STALEMATE_END_MS = 180 * IN_MILLISECONDS;
                    constexpr float STALEMATE_NEW_LOW_PCT = 2.0f;
                    if (party.stalemateSeq != party.pullSeq || !party.stalemateSinceMs)
                    {
                        party.stalemateSeq = party.pullSeq;
                        party.stalemateSinceMs = now ? now : 1;
                        party.stalemateKills = party.kills;
                        party.stalemateLowPct.clear();
                        party.stalemateFocused = false;
                    }
                    std::vector<Unit*> enemies;
                    for (Player* bot : players)
                        if (bot->IsAlive())
                            for (Unit* attacker : bot->getAttackers())
                                // an outro NPC / phantom combat partner (Krick after Ick) is no enemy here
                                if (attacker && attacker->IsAlive() && !attacker->IsTotem() &&
                                    !attacker->IsImmuneToPC() && !attacker->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) &&
                                    !attacker->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) &&
                                    std::find(enemies.begin(), enemies.end(), attacker) == enemies.end())
                                    enemies.push_back(attacker);
                    bool progress = party.kills != party.stalemateKills || enemies.empty();
                    for (Unit* enemy : enemies)
                    {
                        float const pct = enemy->GetHealthPct();
                        auto const low = party.stalemateLowPct.find(enemy->GetGUID());
                        if (low == party.stalemateLowPct.end())
                            party.stalemateLowPct[enemy->GetGUID()] = pct;
                        else if (pct + STALEMATE_NEW_LOW_PCT <= low->second)
                        {
                            low->second = pct;
                            progress = true;
                        }
                    }
                    if (progress)
                    {
                        party.stalemateSinceMs = now ? now : 1;
                        party.stalemateKills = party.kills;
                        party.stalemateFocused = false;
                    }
                    uint32 const flatMs = getMSTimeDiff(party.stalemateSinceMs, now);
                    if (flatMs > STALEMATE_FOCUS_MS && !party.stalemateFocused && !enemies.empty())
                    {
                        party.stalemateFocused = true;
                        Unit* focus = nullptr;
                        bool focusHealer = false;
                        JsonArray mobs;
                        for (Unit* enemy : enemies)
                        {
                            Creature* creature = enemy->ToCreature();
                            bool const healer = creature && MobRoles::IsHealer(creature);
                            bool const running = FledMobs::IsRunning(enemy);
                            mobs.Raw(JsonObject().Str("name", enemy->GetName()).Num("entry", enemy->GetEntry())
                                         .Real("hpPct", enemy->GetHealthPct())
                                         .Real("lowPct", party.stalemateLowPct[enemy->GetGUID()])
                                         .Bool("healer", healer).Bool("running", running)
                                         .Str("victim", enemy->GetVictim() ? enemy->GetVictim()->GetName() : "")
                                         .Build());
                            if (!focus || (healer && !focusHealer) ||
                                (healer == focusHealer && enemy->GetHealth() < focus->GetHealth()))
                            {
                                focus = enemy;
                                focusHealer = healer;
                            }
                        }
                        // [lead128] user: no elapsed-time rule decides a bot's target (the skull after STALEMATE_FOCUS_MS
                        // was the old clock's decision): the stalemate is logged with the mob that would be the
                        // focus, the leader's kill order decides the focus; STALEMATE_END_MS stays the run's stop
                        LogEvent(party, "trash_stalemate",
                                 JsonObject().Num("sec", flatMs / IN_MILLISECONDS).Num("pull", party.pullSeq)
                                     .Num("enemies", uint32(enemies.size()))
                                     .Str("decision", "logged only (the leader's kill order); the weakest " +
                                                          std::string(focusHealer ? "healer" : "mob"))
                                     .Str("focus", focus ? focus->GetName() : "")
                                     .Real("focusHpPct", focus ? focus->GetHealthPct() : 0.0f)
                                     .Raw("mobs", mobs.Build()));
                    }
                    if (flatMs > STALEMATE_END_MS && !party.content->raid)
                    {
                        Finish(party, "failed",
                               "trash stalemate: " + std::to_string(enemies.size()) + " enemies, no kill and no new "
                               "lowest health for " + std::to_string(flatMs / IN_MILLISECONDS) + " s");
                        return;
                    }
                }
            }
            else if (!away.empty())
            {
                WaitForAway(party, players, leader, away);
            }
            else if (party.needBuff && alive)
            {
                SetPhase(party, party.buffStep >= 3 ? Phase::ReadyCheck : Phase::Buffing);
                TickBuffing(party, players, leader);
            }
            else if (leader->IsAlive() && map)
            {
                // Recover before the next pull: mana, health, members far behind.
                bool ledGather = false;
                if (RestWait(party, players, leader, now, wingWalking, ledGather) &&
                    TickTravel(party, players, leader, now, wingWalking, ledGather))
                    return;
            }
        }

        if (!IsTerminalPhase(party.phase))
        {
            CheckExpectedDoors(party, leader);  // [expect_door]
            TickGateTrapGather(party, leader);  // [b51_strat] rat-trap alleys: in together
            TickKeyDoorPass(party, leader);     // [strat_key_pass]
            TickDoorCut(party, leader);         // [b32_strat_gate] a member cut off behind a closed door
            MemberKeyAtDoor(party, leader);     // [b32_strat_gate] ... keys it from its side (in a fight too)
        }
        if (!anyInCombat && !IsTerminalPhase(party.phase))
            ReopenKeyDoorHeld(party, leader);
        Heartbeat(party, players, leader);
        CheckStall(party, players, leader, anyInCombat);
        if (!IsTerminalPhase(party.phase))
            CheckIdle(party, players, leader, anyInCombat);
    }

    // [lead107 stage 5] the phantom-combat end is the leader's (Ai/Leader/Fight/LeaderCombatEnd.h)
    void TestHarnessMgr::TickPhantomCombat(Party& party, std::vector<Player*> const& players, Player* leader,
                                           uint32 now, bool activeBoss, bool& anyInCombat)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Fight::TickPhantomCombat(group, players, leader, now, activeBoss, anyInCombat);
    }

    // [lead107 stage 5] the stale-victim drop is the leader's (LeaderCombatEnd.h)
    void TestHarnessMgr::TickStaleVictim(Party& party, Player* leader, uint32 now, bool anyInCombat, bool activeBoss)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Fight::TickStaleVictim(group, leader, now, anyInCombat, activeBoss);
    }

    // [lead107 stage 5] the wait for the dead / outside members is the leader's (Ai/Leader/Dungeon/LeaderRest.h)
    void TestHarnessMgr::WaitForAway(Party& party, std::vector<Player*> const& players, Player* leader,
                                     std::vector<std::string> const& away)
    {
        PartyLeaderGroup group(*this, party);
        Leader::Dungeon::WaitForAway(group, players, leader, away);
    }

    // [lead107 stage 5] the rest / far wait and the regroup ping-pong are the leader's (LeaderRest.h); true: the
    // group travels on (TickTravel)
    bool TestHarnessMgr::RestWait(Party& party, std::vector<Player*> const& players, Player* leader, uint32 now,
                                  bool wingWalking, bool& ledGather)
    {
        PartyLeaderGroup group(*this, party);
        return Leader::Dungeon::RestWait(group, players, leader, now, wingWalking, ledGather);
    }

    // [lead107 stage 5] the travel pause for a trailing member is the leader's (LeaderRest.h); true: it pauses
    bool TestHarnessMgr::CompactPause(Party& party, Player* leader, uint32 now, bool ledGather)
    {
        PartyLeaderGroup group(*this, party);
        return Leader::Dungeon::CompactPause(group, leader, now, ledGather);
    }

    // [lead107 stage 5] the travel step after the rest, out of TickRun unchanged; true: the run ended
    bool TestHarnessMgr::TickTravel(Party& party, std::vector<Player*> const& players, Player* leader, uint32 now,
                                    bool wingWalking, bool ledGather)
    {
        // A walked wing change: every member is on its way to the next entrance, no route meanwhile.
        // The leader casts an event item (UseStarterItem): nothing walks it away meanwhile.
        bool const castHold = party.castHoldUntilMs && int32(now - party.castHoldUntilMs) < 0 &&
                              !leader->IsInCombat();
        // [lead107] the post-kill wave hold is the leader's (Ai/Leader/Travel/RouteHolds.h)
        PartyLeaderGroup group(*this, party);
        bool const waveHold = Leader::Travel::PostKillWaveHold(group, leader, now);
        // [b51_strat] the group gathers inside a rat-trap alley first (TickGateTrapGather walks it)
        bool const trapHold = party.trapHoldUntilMs && int32(now - party.trapHoldUntilMs) < 0;
        if (!wingWalking && !castHold && !waveHold && !trapHold)
        {
            // [lead129] the route chooses its target again when a fact changes (Leader::Travel::RetargetDue), not
            // on a 4 s clock
            if (Leader::Travel::RetargetDue(group, leader, now))
                Retarget(party, leader);
            bool const compactPause = CompactPause(party, leader, now, ledGather);
            // [lead107] the level-split rejoin is the leader's (Ai/Leader/Travel/RouteHolds.h)
            bool const levelRejoin = Leader::Travel::LevelSplitRejoin(group, leader, now);
            if (levelRejoin)
            {
                // [b35_brd] the rejoin walk holds the leader's waypoint: no pause, no Steer
            }
            else if (compactPause)
                TestRunner::ClearWaypoint(party.leader);
            // [b30_reenter] out and in again when the entrance is much the shorter way to the goal
            else if (!Leader::Travel::TickShortcutReenter(group, players, leader))
                Steer(party, leader);
            if (IsTerminal(party.phase))
                return true;  // Finish() logged the members out, the pointers above are gone
            // [lead107] the no-walk watch is the leader's (RouteHolds.h)
            Leader::Travel::NoWalkWatch(group, leader, now, compactPause, party.phase == Phase::Traveling);
            Leader::Dungeon::BossBuffPass(group);  // [lead344] the leader's (LeaderConsumables.h)
        }
        return false;
    }

    void TestHarnessMgr::Finish(Party& party, std::string const& requestedResult, std::string const& requestedReason)
    {
        if (IsTerminal(party.phase))
            return;

        // [lead1] the run's decision counts and conflicts (C:/WoW/LEADER_REFACTOR_PLAN.md step 1: the baseline)
        if (Leader::LEADER_DECISION_LOG && party.content && party.runStartMs)
            LogEvent(party, "leader_decisions",
                     JsonObject().Raw("summary", DecisionLog::Instance().SummaryJson(party.seq)));
        // [lead15] LEADER_OWN_DECISION_CHECK: own_decision counts per member and per action (the goal is 0; every
        // row names an action that still decides on its own)
        if (Leader::LEADER_OWN_DECISION_CHECK && party.content && party.runStartMs)
        {
            JsonObject byMember;
            std::map<std::string, uint32> actions;
            uint32 total = 0;
            for (auto const& [name, counts] : party.lead.ownDecisions)
            {
                JsonObject rows;
                uint32 memberTotal = 0;
                for (auto const& [action, count] : counts)
                {
                    rows.Num(action.c_str(), count);
                    actions[action] += count;
                    memberTotal += count;
                }
                total += memberTotal;
                byMember.Raw(name.c_str(), rows.Num("total", memberTotal).Build());
            }
            JsonObject byAction;
            for (auto const& [action, count] : actions)
                byAction.Num(action.c_str(), count);
            JsonObject stale;
            uint32 staleTotal = 0;
            for (auto const& [name, count] : party.lead.staleCommands)
            {
                stale.Num(name.c_str(), count);
                staleTotal += count;
            }
            JsonObject blocked;
            uint32 blockedTotal = 0;
            for (auto const& [key, count] : party.lead.blockedMoves)
            {
                blocked.Num(key.c_str(), count);
                blockedTotal += count;
            }
            JsonObject vetoedJson;
            uint32 vetoedTotal = 0;
            for (auto const& [key, count] : party.lead.vetoedEvals)
            {
                vetoedJson.Num(key.c_str(), count);
                vetoedTotal += count;
            }
            // [lead18] vetoed_eval: a blocker unless 0
            LogEvent(party, "vetoed_evals", JsonObject().Num("total", vetoedTotal).Raw("byMember", vetoedJson.Build()));
            {
                // [lead27] leader_event per member per minute (a handful per pull, not per tick)
                float const minutes = std::max(1.0f / 60.0f, float(getMSTimeDiff(party.runStartMs, getMSTime())) /
                                                                 float(MINUTE * IN_MILLISECONDS));
                JsonObject perMinute;
                for (auto const& [name, count] : party.lead.leaderEventCounts)
                    perMinute.Real(name.c_str(), float(count) / minutes);
                LogEvent(party, "leader_events", JsonObject().Raw("perMinute", perMinute.Build()));
                // [lead29] member -> tank distance on the walk (out of combat, the tank walking)
                JsonObject walk;
                for (auto& [name, samples] : party.lead.walkDistances)
                {
                    if (samples.empty())
                        continue;
                    std::vector<float> sorted = samples;
                    std::sort(sorted.begin(), sorted.end());
                    walk.Raw(name.c_str(), JsonObject().Real("max", sorted.back())
                                               .Real("median", sorted[sorted.size() / 2])
                                               .Num("samples", int64(sorted.size())).Build());
                }
                LogEvent(party, "walk_distance", walk);
                JsonObject refusals;
                uint32 refusalTotal = 0;
                for (auto const& [name, count] : party.lead.memberRefusals)
                {
                    refusals.Num(name.c_str(), count);
                    refusalTotal += count;
                }
                LogEvent(party, "member_refusals",
                         JsonObject().Num("total", refusalTotal).Raw("byMember", refusals.Build()));
            }
            LogEvent(party, "healer_lost_tank",
                     JsonObject().Real("healer_lost_tank_s", float(party.lead.healerLostTankMs) / 1000.0f));
            LogEvent(party, "blocked_moves",
                     JsonObject().Num("total", blockedTotal).Raw("byAction", blocked.Build())
                         .Str("content", party.content ? party.content->key : ""));
            LogEvent(party, "own_decisions",
                     JsonObject().Num("total", total).Raw("byMember", byMember.Build())
                         .Raw("byAction", byAction.Build()).Num("staleCommandTotal", staleTotal)
                         .Raw("staleCommands", stale.Build()));
        }

        // [lead7] LEADER_PLANNER_DIFF: the planner library's shadow looks against the tank's own (plan_diff lines in
        // <runId>.decisions.jsonl): how many looks were compared, agreed, and disagreed by kind
        PullPlanner::DiffCounts diffCounts;
        if (party.content && party.runStartMs && PullPlanner::GetDiffCounts(party.leader, diffCounts))
        {
            JsonObject kinds;
            for (auto const& [kind, count] : diffCounts.byKind)
                kinds.Num(kind.c_str(), count);
            LogEvent(party, "plan_diff_summary",
                     JsonObject().Num("compared", diffCounts.compared).Num("same", diffCounts.same)
                         .Num("differ", diffCounts.compared - diffCounts.same).Num("whyOnly", diffCounts.whyOnly)
                         .Raw("byKind", kinds.Build()));
        }

        std::string result = requestedResult;
        std::string reason = requestedReason;
        JsonArray missing;
        if (party.content && party.runStartMs)
        {
            // User rule: the content is cleared only with every boss dead. Each boss still alive (or never spawned)
            // is listed with its reason; "done" with any of them becomes "partial".
            std::vector<std::string> missingLines;
            missing = MissingBosses(party, &missingLines);
            if (!missingLines.empty())
            {
                std::string list;
                for (std::string const& line : missingLines)
                    list += (list.empty() ? "" : "; ") + line;
                if (result == "done")
                    result = "partial";
                reason += " | not killed: " + list;

                JsonArray lines;
                for (std::string const& line : missingLines)
                    lines.Str(line);
                LogEvent(party, "boss_missing", JsonObject().Str("result", result)
                                                    .Num("count", int64(missingLines.size()))
                                                    .Raw("missing", lines.Build()));
            }
        }

        party.result = result;
        party.reason = reason;

        if (party.content && party.runStartMs)
        {

            JsonArray killed;
            JsonArray failed;
            uint32 wipesOnBosses = 0;
            for (BossTrack const& boss : party.bosses)
            {
                if (boss.killed)
                    killed.Str(boss.name);
                if (boss.failed)
                    failed.Str(boss.name);
                wipesOnBosses += boss.wipes;
            }

            JsonArray members;
            uint32 deaths = 0;
            uint32 unplannedStarts = 0;
            for (Member const& member : party.members)
            {
                MemberCounters const counters = StatsRegistry::Instance().Snapshot(member.guid);
                deaths += counters.deaths;
                unplannedStarts += member.unplannedStarts;
                members.Raw(JsonObject().Str("name", member.name).Str("spec", member.spec->label)
                                .Num("damageDone", int64(counters.damageDone))
                                .Num("healingDone", int64(counters.healingDone))
                                .Num("damageTaken", int64(counters.damageTaken)).Num("deaths", counters.deaths)
                                .Num("interrupts", counters.interrupts).Num("dispels", counters.dispels)
                                .Num("aggroPulled", member.aggroPulled)
                                .Num("unplannedStarts", member.unplannedStarts)
                                .Merge(CombatStatsJson(counters, MemberCounters(), member.combatMs))
                                .Build());
            }

            JsonArray suspicious;
            for (std::string const& line : party.suspicious)
                suspicious.Str(line);

            JsonArray excluded;
            Player* const endLeader = ObjectAccessor::FindPlayer(party.leader);
            Map* const endMap = endLeader && endLeader->GetMapId() == party.content->mapId ? endLeader->GetMap()
                                                                                           : nullptr;
            std::set<uint32> excludedEntries;
            for (BossTrack const& boss : party.bosses)
                if (!boss.killed)
                {
                    if (char const* gate = TrackClassGate(party, boss))
                        excluded.Raw(JsonObject().Str("boss", boss.name).Num("entry", boss.entry)
                                         .Str("reason", gate).Build());
                    else if (std::string const na = FriendlyTrackReason(party, endMap, boss); !na.empty())
                    {
                        excludedEntries.insert(boss.entry);
                        excluded.Raw(JsonObject().Str("boss", boss.name).Num("entry", boss.entry)
                                         .Str("reason", na).Build());
                    }
                }
            if (endMap)
                for (ObjectGuid const& guid : party.bossCreatures)
                    if (Creature* creature = endMap->GetCreature(guid);
                        creature && !excludedEntries.count(creature->GetEntry()))
                        if (std::string const na = FriendlyBossReason(party, creature); !na.empty())
                        {
                            excludedEntries.insert(creature->GetEntry());
                            excluded.Raw(JsonObject().Str("boss", creature->GetName())
                                             .Num("entry", creature->GetEntry()).Str("reason", na).Build());
                        }

            uint32 const now = getMSTime();
            LogEvent(party, "run_end",
                     JsonObject().Str("result", result).Str("reason", reason)
                         .Num("durationSec", getMSTimeDiff(party.runStartMs, now) / IN_MILLISECONDS)
                         .Num("bossesTotal", RequiredBossCount(party)).Raw("bossesKilled", killed.Build())
                         .Raw("bossesFailed", failed.Build()).Num("wipes", party.wipes)
                         .Num("bossWipes", wipesOnBosses).Num("kills", party.kills)
                         .Num("stuckEvents", party.stuckEvents).Num("aggroLosses", party.aggroLosses)
                         .Num("unplannedStarts", unplannedStarts)
                         .Num("encountersDbc", int64(party.encounters.size())).Raw("bossesMissing", missing.Build())
                         .Raw("bossesExcluded", excluded.Build())
                         .Num("corpseRunSec", party.corpseRunMs / IN_MILLISECONDS)
                         .Num("botCannotMove", party.cannotMoveEvents)
                         .Bool("suspicious", !party.suspicious.empty()).Raw("suspiciousReasons", suspicious.Build())
                         .Raw("members", members.Build()));

            // Clean-run streak for the runall order: done, no boss missing, no death, no wipe; "stopped" counts
            // neither way.
            if (result != "stopped" && result != "void" && !party.guild)
                RecordStreak(party.content->key, result == "done" && missing.Empty() && party.wipes == 0 && deaths == 0,
                             result, WallClock());
        }
        else if (party.content)
            LogEvent(party, "run_end", JsonObject().Str("result", result).Str("reason", reason).Num("durationSec", 0));

        LOG_INFO("playerbots", "Test harness: {} finished: {} ({})", party.id, result, reason);

        // [peaklog] mod-raid-rescale's per-run peak log: flush it at run end through its console command (decoupled:
        // without the module the command is unknown and does nothing)
        {
            std::string const flush = "raidrescale peakflush run_end " + party.id;
            sWorld->QueueCliCommand(new CliCommandHolder(nullptr, flush.c_str(), [](void*, std::string_view) { },
                                                         [](void*, bool) { }));
        }

        // [guild60] the five's report for the global leader (before the release drops the run's counters)
        Leader::Dungeon::ClearResult guildResult;
        if (party.guild)
        {
            guildResult.engineRunId = party.id;
            guildResult.result = result;
            guildResult.reason = reason;
            guildResult.wipes = party.wipes;
            guildResult.durationSec = party.runStartMs ? getMSTimeDiff(party.runStartMs, getMSTime()) / IN_MILLISECONDS : 0;
            if (party.runStartMs)
            {
                guildResult.bossesTotal = RequiredBossCount(party);
                for (BossTrack const& boss : party.bosses)
                    if (boss.killed)
                        guildResult.bossesKilled.push_back(boss.name);
                for (Member const& member : party.members)
                    if (uint32 const deaths = StatsRegistry::Instance().Snapshot(member.guid).deaths)
                        guildResult.deaths.emplace_back(member.name, deaths);
            }
        }

        ReleaseParty(party);

        if (result == "done" || result == "partial" || result == "timeout")
            party.phase = Phase::Done;
        else if (result == "stalled")
            party.phase = Phase::Stalled;
        else if (result == "stopped")
            party.phase = Phase::Stopped;
        else
            party.phase = Phase::Failed;

        if (party.guild)  // [guild70] the five-leader's run gets its clearing back
            Leader::Dungeon::DungeonRuns::Instance().OnCleared(party.guildFiveId, guildResult);
    }

    void TestHarnessMgr::ReleaseParty(Party& party)
    {
        TestRunner::ClearWaypoint(party.leader);
        for (Member const& member : party.members)
        {
            TestRunner::ClearWaypoint(member.guid);
            Leader::Knowledge::SetDoorCut(member.guid, false);  // [b32_strat_gate]
            SelfTravel::ClearPortalGoal(member.guid);
        }
        StatsRegistry::Instance().UntrackRun(party.seq);
        Leader::Orders::Untrack(party.leader);  // [lead1]
        PullPlanner::Release(party.leader);     // [lead7] the leader's planner instance for this tank
        Leader::Readiness::Clear(party.groupGuid);  // [lead6] the leader's readiness verdict for its group
        for (Member const& member : party.members)  // [lead27] no command outlives the run (no TTL ends them)
            Leader::Events::Clear(member.guid);
        DecisionLog::Instance().EndRun(party.seq);

        if (Player* leader = ObjectAccessor::FindPlayer(party.leader))
            if (Group* group = leader->GetGroup())
                if (group->GetGUID() == party.groupGuid)
                    group->Disband();

        for (Member const& member : party.members)
        {
            if (Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid))
            {
                // [guild60] a guild five stays in the world: each member is its own leader again (its own
                // strategies, New RPG); out of the group inside the instance, the core ports it out (homebind)
                if (party.guild)
                {
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        botAI->ResetStrategies(false);
                }
                else if (GET_PLAYERBOT_AI(bot) && !IsSelfBot(bot))
                    sRandomPlayerbotMgr.LogoutPlayerBot(member.guid);
            }
            _inUse.erase(member.guid);
        }
    }

    void TestHarnessMgr::StartQueued()
    {
        uint32 active = 0;
        bool anyParty = false;  // any party (queued, single run or prepared) holds pool characters
        // User 2026-10-24: at most MAX_RUNS_PER_CONTENT parties in one dungeon at a time - a failure is a shared
        // script's (the tank's, a path's), so ten parties in one dungeon all stall at the same spot; the free slots
        // go to the next dungeons in the queue.
        constexpr uint32 MAX_RUNS_PER_CONTENT = 2;
        std::map<ContentDef const*, uint32> perContent;
        for (auto const& party : _parties)
        {
            if (IsTerminal(party->phase))
                continue;
            anyParty = true;
            if (party->content && party->fromQueue)
            {
                ++active;
                ++perContent[party->content];
            }
        }

        // In queue order; an item whose characters are all busy waits and the next one may start (heroics run on the
        // level-80 pool while the normals wait for the levelling pool). Normals are queued by rising level, so a
        // later normal never takes a character an earlier one could still use.
        std::map<ContentDef const*, std::string> waiting;
        for (auto itr = _queue.begin(); itr != _queue.end() && active < _queueParallel;)
        {
            QueueItem const item = *itr;
            if (perContent[item.content] >= MAX_RUNS_PER_CONTENT)
            {
                ++itr;  // waits for a party of the same dungeon to finish
                continue;
            }
            std::string error;
            Party* party = CreateParty(item.comp, item.content, item.content->level,
                                       item.content->ilvl - IlvlBelowContent(item.content->ilvl), error);
            if (!party)
            {
                if (error.find("not enough") != std::string::npos)
                {
                    waiting[item.content] = error;
                    ++itr;  // wait for a running party to give its characters back
                    continue;
                }

                LOG_ERROR("playerbots", "Test harness: queued {} / {} dropped: {}", item.content->key, item.comp->id,
                          error);
                itr = _queue.erase(itr);
                continue;
            }

            party->fromQueue = true;
            itr = _queue.erase(itr);
            ++active;
            ++perContent[item.content];
            anyParty = true;
            LOG_INFO("playerbots", "Test harness: started {} (level {}, {} left in the queue)", party->id,
                     party->level, _queue.size());
        }

        // Nothing runs and nothing could start: the pool can never serve what is left.
        if (!anyParty && !_queue.empty())
        {
            for (QueueItem const& item : _queue)
                LOG_ERROR("playerbots", "Test harness: queued {} / {} dropped: {}", item.content->key, item.comp->id,
                          waiting.count(item.content) ? waiting[item.content] : std::string("no characters"));
            _queue.clear();
        }
    }

    void TestHarnessMgr::Shutdown()
    {
        for (auto const& party : _parties)
            if (!IsTerminal(party->phase))
                Finish(*party, "stopped", "server shutdown");
        _queue.clear();
    }

    bool TestHarnessMgr::HandlePrepare(ChatHandler* handler, std::string const& compId, uint8 level, uint32 ilvl)
    {
        std::string error;
        uint8 const partyLevel = level ? level : DEFAULT_LEVEL;
        Party* party = CreateParty(FindComposition(compId), nullptr, partyLevel,
                                   ilvl ? ilvl : DefaultTargetIlvl(partyLevel), error);
        if (!party)
        {
            handler->PSendSysMessage("Test harness: {}", error);
            return false;
        }

        handler->PSendSysMessage("Test harness: party {} ({}) logs in and prepares to level {}, average ilvl {}",
                                 party->id, compId, party->level, party->targetIlvl);
        return true;
    }

    bool TestHarnessMgr::HandleRun(ChatHandler* handler, std::string const& contentKey, uint32 count,
                                   std::string const& compId)
    {
        ContentDef const* content = FindContent(contentKey);
        if (!content)
        {
            handler->PSendSysMessage("Test harness: unknown content '{}' (playerbots test list)", contentKey);
            return false;
        }

        Composition const* comp = compId.empty() ? OptimalComposition(content->size) : FindComposition(compId);
        if (!comp || comp->size != content->size)
        {
            handler->PSendSysMessage("Test harness: composition '{}' does not fit a {}-player content", compId,
                                     content->size);
            return false;
        }

        for (uint32 i = 0; i < std::max<uint32>(count, 1); ++i)
        {
            std::string error;
            Party* party = CreateParty(comp, content, content->level, content->ilvl - IlvlBelowContent(content->ilvl), error);
            if (!party)
            {
                handler->PSendSysMessage("Test harness: {}", error);
                return i > 0;
            }
            handler->PSendSysMessage("Test harness: run {} started ({}, comp {}, level {}, target avg ilvl {})",
                                     party->id, content->name, comp->id, party->level, party->targetIlvl);
        }
        return true;
    }

    namespace
    {
        constexpr uint32 STREAK_CLEAN_RUNS = 3;
        // streaks.json is seeded from the run logs from this run id on (user rule: tonight's history counts).
        constexpr char const* STREAK_SEED_FROM = "20261023-233700";
        constexpr char const* STREAKS_FILE = "streaks.json";

        // The value after "key": in one JSON line (JsonObject writes no spaces; one after the colon is accepted):
        // a string without its quotes or a number / literal as written.
        std::string JsonField(std::string const& line, char const* key)
        {
            std::string const needle = std::string("\"") + key + "\":";
            size_t pos = line.find(needle);
            if (pos == std::string::npos)
                return "";
            pos += needle.size();
            while (pos < line.size() && line[pos] == ' ')
                ++pos;
            if (pos < line.size() && line[pos] == '"')
            {
                size_t const end = line.find('"', pos + 1);
                return end == std::string::npos ? "" : line.substr(pos + 1, end - pos - 1);
            }
            size_t end = pos;
            while (end < line.size() && line[end] != ',' && line[end] != '}' && line[end] != ']')
                ++end;
            return line.substr(pos, end - pos);
        }

        uint32 JsonNumber(std::string const& line, char const* key)
        {
            std::string const value = JsonField(line, key);
            return value.empty() ? 0 : uint32(std::strtoul(value.c_str(), nullptr, 10));
        }
    }

    void TestHarnessMgr::LoadStreaks()
    {
        if (_streaksLoaded)
            return;
        _streaksLoaded = true;
        _streaks.clear();

        std::string const path = TestsDir() + STREAKS_FILE;
        std::ifstream file(path);
        if (file)
        {
            // One content per line: "key":{"streak":N,"lastResult":"...","lastTime":"..."}
            std::string line;
            while (std::getline(file, line))
            {
                size_t const open = line.find('"');
                size_t const close = open == std::string::npos ? open : line.find('"', open + 1);
                if (close == std::string::npos || line.find("\"streak\"") == std::string::npos)
                    continue;
                StreakInfo& info = _streaks[line.substr(open + 1, close - open - 1)];
                info.streak = JsonNumber(line, "streak");
                info.lastResult = JsonField(line, "lastResult");
                info.lastTime = JsonField(line, "lastTime");
            }
            return;
        }

        // First use: seeded once from the run logs (<runId>.jsonl, run ids sort by time), "stopped" runs count
        // neither way.
        std::vector<std::string> logs;
        std::error_code error;
        for (auto const& entry : std::filesystem::directory_iterator(TestsDir(), error))
        {
            std::string const name = entry.path().filename().string();
            if (entry.is_regular_file(error) && name.size() > 6 && name[0] == '2' &&
                name.compare(name.size() - 6, 6, ".jsonl") == 0 && name.substr(0, 15) >= STREAK_SEED_FROM)
                logs.push_back(entry.path().string());
        }
        std::sort(logs.begin(), logs.end());
        for (std::string const& log : logs)
        {
            std::ifstream run(log);
            std::string line;
            while (std::getline(run, line))
            {
                if (line.find("\"type\":\"run_end\"") == std::string::npos)
                    continue;
                std::string const result = JsonField(line, "result");
                std::string const content = JsonField(line, "content");
                if (content.empty() || result.empty() || result == "stopped")
                    break;
                uint32 deaths = 0;
                for (size_t pos = line.find("\"deaths\":"); pos != std::string::npos;
                     pos = line.find("\"deaths\":", pos + 1))
                    deaths += uint32(std::strtoul(line.c_str() + pos + 9, nullptr, 10));
                bool const clean = result == "done" && line.find("\"bossesMissing\":[]") != std::string::npos &&
                                   JsonNumber(line, "wipes") == 0 && deaths == 0;
                StreakInfo& info = _streaks[content];
                info.streak = clean ? info.streak + 1 : 0;
                info.lastResult = result;
                info.lastTime = JsonField(line, "time");
                break;
            }
        }
        SaveStreaks();
        LOG_INFO("playerbots", "Test harness: {} seeded from {} run logs ({} contents)", path, logs.size(),
                 _streaks.size());
    }

    void TestHarnessMgr::SaveStreaks() const
    {
        std::string body = "{\n";
        size_t index = 0;
        for (auto const& [key, info] : _streaks)
            body += JsonEscape(key) + ":" +
                    JsonObject().Num("streak", info.streak).Str("lastResult", info.lastResult)
                        .Str("lastTime", info.lastTime).Build() +
                    (++index < _streaks.size() ? ",\n" : "\n");
        body += "}\n";
        std::error_code error;
        std::filesystem::create_directories(TestsDir(), error);
        std::ofstream file(TestsDir() + STREAKS_FILE, std::ios::trunc);
        if (file)
            file << body;
        else
            LOG_ERROR("playerbots", "Test harness: cannot write {}{}", TestsDir(), STREAKS_FILE);
    }

    void TestHarnessMgr::RecordStreak(std::string const& contentKey, bool clean, std::string const& result,
                                      std::string const& time)
    {
        LoadStreaks();
        StreakInfo& info = _streaks[contentKey];
        info.streak = clean ? info.streak + 1 : 0;
        info.lastResult = result;
        info.lastTime = time;
        SaveStreaks();
    }

    bool TestHarnessMgr::HandleRunAll(ChatHandler* handler, std::string const& set, uint32 parallel, bool variants,
                                      uint32 window)
    {
        // "frontier" (user rule 2026-10-24): dungeons bottom-up as a character levels, from the lowest content not yet
        // at STREAK_CLEAN_RUNS clean runs to that level + window.
        bool const frontier = set == "frontier";
        bool const normal = set == "normal" || set == "dungeons" || set == "all" || frontier;
        bool const heroic = set == "heroic" || set == "dungeons" || set == "all" || frontier;
        bool const raids = set == "raids" || set == "all";
        if (!normal && !heroic && !raids)
        {
            handler->PSendSysMessage(
                "Usage: playerbots test runall dungeons|normal|heroic|raids|all [parallel N] [variants]");
            return false;
        }

        if (parallel)
            _queueParallel = parallel;

        // Normals first and by rising level (the levelling pool climbs with them), then heroics, then raids.
        // blocked.txt in the tests dir: one content key per line ("#" starts a comment). User rule 2026-10-24: a
        // content whose known blocker is not fixed is not rerun.
        std::set<std::string> blocked;
        {
            std::ifstream blockedFile(TestsDir() + "blocked.txt");
            std::string line;
            while (std::getline(blockedFile, line))
            {
                size_t const hash = line.find('#');
                if (hash != std::string::npos)
                    line.resize(hash);
                size_t const begin = line.find_first_not_of(" \t\r");
                if (begin == std::string::npos)
                    continue;
                size_t const end = line.find_last_not_of(" \t\r");
                blocked.insert(line.substr(begin, end - begin + 1));
            }
        }

        std::vector<ContentDef const*> selected;
        for (ContentDef const& content : Contents())
        {
            if (blocked.count(content.key))
            {
                LOG_INFO("playerbots", "Test harness: runall skips blocked {}", content.key);
                continue;
            }
            if (IsNormalDungeon(content) ? normal : content.raid ? raids : heroic)
                selected.push_back(&content);
        }
        std::stable_sort(selected.begin(), selected.end(),
                         [](ContentDef const* left, ContentDef const* right)
                         {
                             auto rank = [](ContentDef const* content)
                             { return IsNormalDungeon(*content) ? 0 : content->raid ? 2 : 1; };
                             if (rank(left) != rank(right))
                                 return rank(left) < rank(right);
                             return IsNormalDungeon(*left) && left->level < right->level;
                         });

        // Test need first (user rule 2026-10-24): STREAK_CLEAN_RUNS consecutive clean runs go to the end, the rest by
        // rising streak (never run or never clean first), the order above within the same streak.
        LoadStreaks();
        auto streakOf = [this](ContentDef const* content)
        {
            auto const itr = _streaks.find(content->key);
            return itr == _streaks.end() ? 0u : itr->second.streak;
        };
        std::stable_sort(selected.begin(), selected.end(),
                         [&streakOf](ContentDef const* left, ContentDef const* right)
                         {
                             uint32 const leftStreak = streakOf(left);
                             uint32 const rightStreak = streakOf(right);
                             bool const leftClean = leftStreak >= STREAK_CLEAN_RUNS;
                             bool const rightClean = rightStreak >= STREAK_CLEAN_RUNS;
                             if (leftClean != rightClean)
                                 return rightClean;
                             return leftStreak < rightStreak;
                         });

        // [runall-priority] priority.txt in the tests dir (user rule 2026-10-25): one content key per line, the
        // listed keys run first in file order (most problematic, then never run), the rest after them as above.
        {
            std::map<std::string, uint32> priority;
            std::ifstream priorityFile(TestsDir() + "priority.txt");
            std::string line;
            while (std::getline(priorityFile, line))
            {
                size_t const hash = line.find('#');
                if (hash != std::string::npos)
                    line.resize(hash);
                size_t const begin = line.find_first_not_of(" \t\r");
                if (begin == std::string::npos)
                    continue;
                size_t const end = line.find_last_not_of(" \t\r");
                priority.emplace(line.substr(begin, end - begin + 1), uint32(priority.size()));
            }
            if (!priority.empty())
            {
                auto const rankOf = [&priority](ContentDef const* content)
                {
                    auto const itr = priority.find(content->key);
                    return itr == priority.end() ? uint32(priority.size()) : itr->second;
                };
                std::stable_sort(selected.begin(), selected.end(),
                                 [&rankOf](ContentDef const* left, ContentDef const* right)
                                 { return rankOf(left) < rankOf(right); });
                LOG_INFO("playerbots", "Test harness: runall priority.txt ranks {} contents first", priority.size());
            }
        }
        if (frontier)
        {
            // The group's level for the content: heroics of a tier run at its cap.
            auto const levelOf = [](ContentDef const* content) -> uint32
            {
                std::string const tier = content->tier ? content->tier : "";
                if (tier == "tbc-h")
                    return 70;
                if (tier == "wotlk-h" || tier == "classic-h")
                    return 80;
                return content->level;
            };
            uint32 lowest = 0;
            for (ContentDef const* content : selected)
                if (streakOf(content) < STREAK_CLEAN_RUNS && (!lowest || levelOf(content) < lowest))
                    lowest = levelOf(content);
            uint32 const top = lowest + window;
            selected.erase(std::remove_if(selected.begin(), selected.end(),
                                          [&](ContentDef const* content)
                                          {
                                              return streakOf(content) >= STREAK_CLEAN_RUNS ||
                                                     levelOf(content) > top;
                                          }),
                           selected.end());
            std::stable_sort(selected.begin(), selected.end(),
                             [&](ContentDef const* left, ContentDef const* right)
                             { return levelOf(left) < levelOf(right); });
            LOG_INFO("playerbots", "Test harness: runall frontier levels {}..{} ({} contents)", lowest, top,
                     selected.size());
        }

        std::string order;
        for (ContentDef const* content : selected)
        {
            order += (order.empty() ? "" : " ") + std::string(content->key);
            LOG_INFO("playerbots", "Test harness: runall order {} streak {}", content->key, streakOf(content));
        }
        LOG_INFO("playerbots", "Test harness: runall order: {}", order);

        uint32 queued = 0;
        uint32 queuedNormal = 0;
        for (ContentDef const* content : selected)
        {
            // [runall-random-comp] one run per content with a random composition of its size (Horde-only variants
            // and Death Knights below 55 left out), so every tank / healer / dps spec gets tested over the waves
            Composition const* randomComp = nullptr;
            if (_runAllRandomComp)
            {
                std::vector<Composition const*> pool;
                for (Composition const* comp : CompositionsOfSize(content->size))
                {
                    if (std::string(comp->note).find("Horde") != std::string::npos)
                        continue;
                    if (IsNormalDungeon(*content) && content->level < 55 &&
                        std::any_of(comp->slots.begin(), comp->slots.end(), [](CompSlot const& slot)
                                    {
                                        SpecDef const* spec = FindSpec(slot.spec);
                                        return spec && spec->cls == CLASS_DEATH_KNIGHT;
                                    }))
                        continue;
                    pool.push_back(comp);
                }
                if (!pool.empty())
                    randomComp = pool[urand(0, uint32(pool.size()) - 1)];
            }
            for (Composition const* comp : CompositionsOfSize(content->size))
            {
                if (randomComp ? comp != randomComp : (!comp->optimal && !variants))
                    continue;
                // A Death Knight starts at 55: a composition with one is not a real group below that (Stockade wave
                // 36: a level 24 DK tank with 1133 hp).
                if (IsNormalDungeon(*content) && content->level < 55 &&
                    std::any_of(comp->slots.begin(), comp->slots.end(), [](CompSlot const& slot)
                                {
                                    SpecDef const* spec = FindSpec(slot.spec);
                                    return spec && spec->cls == CLASS_DEATH_KNIGHT;
                                }))
                    continue;
                _queue.push_back({content, comp});
                ++queued;
                if (IsNormalDungeon(*content))
                    ++queuedNormal;
            }
            // The Horde run of a content whose encounters depend on the faction (Uldaman's Lost Dwarves).
            if (HordeRunContent(content->key))
                if (Composition const* horde = FindComposition(HORDE_COMPOSITION))
                {
                    _queue.push_back({content, horde});
                    ++queued;
                    if (IsNormalDungeon(*content))
                        ++queuedNormal;
                }
        }

        handler->PSendSysMessage("Test harness: {} runs queued ({} normal, {} heroic / raid), {} in parallel", queued,
                                 queuedNormal, queued - queuedNormal, _queueParallel);
        return true;
    }

    std::string TestHarnessMgr::PhaseLabel(Party const& party) const
    {
        if (party.phase == Phase::BossFight)
            for (BossTrack const& boss : party.bosses)
                if (boss.lastState == IN_PROGRESS)
                    return "boss fight " + boss.name;
        return PhaseName(party.phase);
    }

    bool TestHarnessMgr::HandleStatus(ChatHandler* handler)
    {
        uint32 maxLevel = 0;
        uint32 levelingPool = 0;
        for (PoolCharacter const& character : _pool)
        {
            if (character.level >= MAX_TEST_LEVEL)
                ++maxLevel;
            if (character.leveling)
                ++levelingPool;
        }
        handler->PSendSysMessage("Test harness: {} part(ies), {} queued (parallel {}), pool {} characters ({} at 80, "
                                 "{} in the levelling pool){}",
                                 _parties.size(), _queue.size(), _queueParallel, _pool.size(), maxLevel, levelingPool,
                                 _setup.active ? ", setup running" : "");
        uint32 const now = getMSTime();
        for (auto const& party : _parties)
        {
            uint32 killed = 0;
            for (BossTrack const& boss : party->bosses)
                if (boss.killed)
                    ++killed;
            handler->PSendSysMessage("  {} [{}] {} L{} - {}, bosses {}/{}, wipes {}, kills {}, {} min", party->seq,
                                     party->id, party->comp->id, party->level, PhaseLabel(*party), killed,
                                     party->bosses.size(),
                                     party->wipes, party->kills,
                                     party->runStartMs
                                         ? getMSTimeDiff(party->runStartMs, now) / MINUTE / IN_MILLISECONDS
                                         : 0);
        }
        return true;
    }

    bool TestHarnessMgr::HandleStop(ChatHandler* handler, std::string const& id)
    {
        if (id == "all")
        {
            _queue.clear();
            for (auto const& party : _parties)
                Finish(*party, "stopped", "stopped from the console");
            handler->PSendSysMessage("Test harness: all runs stopped, queue cleared");
            return true;
        }

        Party* party = FindParty(id);
        if (!party)
        {
            handler->PSendSysMessage("Test harness: no party '{}'", id);
            return false;
        }

        Finish(*party, "stopped", "stopped from the console");
        handler->PSendSysMessage("Test harness: {} stopped", party->id);
        return true;
    }

    bool TestHarnessMgr::HandleList(ChatHandler* handler)
    {
        std::string line;
        for (ContentDef const& content : Contents())
        {
            line += IsNormalDungeon(content) ? Acore::StringFormat("{}(L{} i{}) ", content.key, content.level,
                                                                    content.ilvl - IlvlBelowContent(content.ilvl))
                                              : Acore::StringFormat("{}({}) ", content.key, content.ilvl);
            if (line.size() > 200)
            {
                handler->PSendSysMessage("  {}", line);
                line.clear();
            }
        }
        if (!line.empty())
            handler->PSendSysMessage("  {}", line);

        for (Composition const& comp : Compositions())
        {
            std::string specs;
            for (CompSlot const& slot : comp.slots)
                specs += std::string(specs.empty() ? "" : ", ") + slot.spec;
            handler->PSendSysMessage("  comp {} ({}): {}", comp.id, comp.note, specs);
        }
        return true;
    }

    std::vector<ObjectGuid> PartyLeaderGroup::MemberGuids() const
    {
        std::vector<ObjectGuid> out;
        out.reserve(_party.members.size());
        for (Member const& member : _party.members)
            out.push_back(member.guid);
        return out;
    }

    uint32 PartyLeaderGroup::MapId() const { return _party.content ? _party.content->mapId : 0; }

    void PartyLeaderGroup::SkipTarget(ObjectGuid guid, uint32 ms, std::string const& reason)
    {
        _mgr.SkipTarget(_party, guid, ms, reason);
    }

    void PartyLeaderGroup::LogEvent(std::string const& type, JsonObject fields)
    {
        _mgr.LogEvent(_party, type, std::move(fields));
    }
}
