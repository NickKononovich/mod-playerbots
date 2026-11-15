/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReviveFromCorpseAction.h"  // [grave44]
#include "Playerbots.h"
#include "BattleGroundTactics.h"
#include "BattlefieldScript.h"
#include "Channel.h"
#include "CheckMountStateAction.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "AiFactory.h"
#include "DpsEstimate.h"  // [dps1]
#include "Spell.h"  // [dpscal]
#include "UnitScript.h"  // [dpscal]
#include "BuiltInConfig.h"
#include "DBUpdater.h"
#include "DatabaseEnv.h"
#include "PlayerbotsDatabase.h"
#include <mysqld_error.h>
#include "AllMapScript.h"
#include "GatherNodeMgr.h"
#include "GuildCoordinator.h"
#include "GuildGear.h"
#include "GuildScript.h"
#include "Item.h"
#include "NewRpgBaseAction.h"
#include "NewRpgCoop.h"  // [help]
#include "SoloSafety.h"  // [guild13]
#include "ThreatManager.h"  // [deaths14]
#include "GlobalScript.h"
#include "GuildTaskMgr.h"
#include "Journal.h"  // [jrn30] the fight-start, aggro and death events
#include "PlayerScript.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotCommandScript.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotSpellRepository.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SelfBotControl.h"
#include "ServerScript.h"
#include "SessionScript.h"
#include "WorldScript.h"
#include "WorldSessionMgr.h"  // [jrn31] the session count that makes a tick time comparable
#include "SlowUpdate.h"  // [perf212]
#include "WalkGraph.h"  // [nav180]
#include "cmath"

class PlayerbotsDatabaseScript : public DatabaseScript
{
public:
    PlayerbotsDatabaseScript() : DatabaseScript("PlayerbotsDatabaseScript") {}

    bool OnModuleDatabasesLoading() override
    {
        std::string const dbString = sConfigMgr->GetOption<std::string>("PlayerbotsDatabaseInfo", "");
        if (dbString.empty())
        {
            LOG_ERROR("server.playerbots", "Playerbots database is not specified in configuration file");
            return false;
        }

        uint8 const workerThreads = sConfigMgr->GetOption<uint8>("PlayerbotsDatabase.WorkerThreads", 1);
        uint8 const synchThreads = sConfigMgr->GetOption<uint8>("PlayerbotsDatabase.SynchThreads", 2);
        PlayerbotsDatabase.SetConnectionInfo(dbString, workerThreads, synchThreads);

        bool const updatesEnabled = sConfigMgr->GetOption<bool>("Playerbots.Updates.EnableDatabases", true);
        if (updatesEnabled && !DBUpdaterUtil::CheckExecutable())
            return false;

        uint32 error = PlayerbotsDatabase.Open();
        if (error == ER_BAD_DB_ERROR && updatesEnabled)
        {
            // Database missing: create it through the mysql CLI and connect again
            if (!ModuleDBUpdater::Create(PlayerbotsDatabase))
                return false;

            error = PlayerbotsDatabase.Open();
        }

        if (error)
        {
            LOG_ERROR("server.playerbots", "Cannot connect to the playerbots database, error {}", error);
            return false;
        }

        if (updatesEnabled)
        {
            DBUpdaterInfo const info = {
                "Playerbots",
                BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbots",
                BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbots/data/sql/playerbots/base/",
                "db_playerbot"
            };

            if (!ModuleDBUpdater::Populate(PlayerbotsDatabase, info))
            {
                LOG_ERROR("server.playerbots", "Could not populate the playerbots database, see log for details.");
                return false;
            }

            if (!ModuleDBUpdater::Update(PlayerbotsDatabase, info))
            {
                LOG_ERROR("server.playerbots", "Could not update the playerbots database, see log for details.");
                return false;
            }
        }

        if (!PlayerbotsDatabase.PrepareStatements())
        {
            LOG_ERROR("server.playerbots", "Could not prepare statements of the playerbots database, see log for details.");
            return false;
        }

        return true;
    }

    void OnModuleDatabasesKeepAlive() override { PlayerbotsDatabase.KeepAlive(); }

    void OnModuleDatabasesClosing() override { PlayerbotsDatabase.Close(); }

    void OnDatabaseWarnAboutSyncQueries(bool apply) override { PlayerbotsDatabase.WarnAboutSyncQueries(apply); }

    void OnDatabaseGetDBRevision(std::map<std::string, std::string>& revisions) override
    {
        std::string revision;
        if (QueryResult resultPlayerbot =
                PlayerbotsDatabase.Query("SELECT date FROM version_db_playerbots ORDER BY date DESC LIMIT 1"))
        {
            Field* fields = resultPlayerbot->Fetch();
            revision = fields[0].Get<std::string>();
        }

        if (revision.empty())
            revision = "Unknown Playerbots Database Revision";

        revisions["Playerbots"] = revision;
    }
};

// [guild5] One line per death by a creature (killer, levels, place) for the guild stats; and on a logout only that
// character is marked offline: the core marked every character of the account offline (CHAR_UPD_ACCOUNT_ONLINE), so
// the user's logout or a pack character handed to him flagged the account's other pack bots offline while they played
// on (Player::SaveToDB set them back at their next save).
class PlayerbotsDeathAndOnlineScript : public PlayerScript
{
public:
    PlayerbotsDeathAndOnlineScript()
        : PlayerScript("PlayerbotsDeathAndOnlineScript",
                       {PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE, PLAYERHOOK_CAN_MARK_ACCOUNT_OFFLINE})
    {
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(killed->GetAreaId());
        // [deaths14] the killer's own threat list at the kill: whom it fought besides the bot (a party member's fight,
        // a pull of another bot). b354 Geron L10 died to a Tunnel Rat Kobold L12 that was in none of its tracked
        // attackers ("hit by 2: Tunnel Rat Vermin x2"): the triage had no cause; the kobold was Manue's
        std::string threat;
        if (killer->CanHaveThreatList())
            for (ThreatReference const* ref : killer->GetThreatMgr().GetSortedThreatList())
                if (Unit* victim = ref ? ref->GetVictim() : nullptr)
                    threat += Acore::StringFormat("{}{} {:.0f}", threat.empty() ? "" : ", ", victim->GetName(),
                                                  ref->GetThreat());
        // [guild13] its fight. [runpast88] one pass fills the sentence AND the same numbers as values: asking
        // SoloSafety twice would be two readings of one state at two instants, which is two judges of one quantity
        Journal::DeathFacts facts;
        std::string const context = SoloSafety::DeathContext(killed, killer, &facts);
        // [jrn30] THE DEATH AS AN EVENT, in a table a prevention line cannot reach. The line below stays, but it
        // must not be what a death is counted by: 20 502 stream lines in one window carried a "[death*" tag and 17
        // of them were deaths - the rest is the prevention domain printing that a death did NOT happen, so a count
        // by tag overstated deaths by about 1 200 times. The count is COUNT(*) over pb_journal_death.
        Journal::NoteDeath(killed, killer, context, threat, facts);
        LOG_INFO("playerbots", "[death] {} L{} killed by {} (entry {}, L{}, {}% hp) at map {} ({:.1f},{:.1f}) {} | {}{}",
                 killed->GetName(), killed->GetLevel(), killer->GetName(), killer->GetEntry(), killer->GetLevel(),
                 uint32(killer->GetHealthPct()), killed->GetMapId(), killed->GetPositionX(), killed->GetPositionY(),
                 // [surv20] the killer goes in: its run-past chase line is this death's only when the killer (or a
                 // mob that hit it) is one of that chase's own chasers
                 area ? area->area_name[0] : "?", context,
                 threat.empty() ? std::string() : "; [deaths14] the killer's threat list: " + threat);
        SelfBotControl::RecordGuildDeath(killed, killer);  // [live1]
        NewRpgBaseAction::RecordDeath(killed, killer);       // [guild12]
        NoteGhostDeath(killed);                              // [grave44]
    }

    bool OnPlayerCanMarkAccountOffline(ObjectGuid guid, uint32 /*accountId*/) override
    {
        CharacterDatabase.Execute("UPDATE characters SET online = 0 WHERE guid = {}", guid.GetCounter());
        return false;
    }
};

// [dps1] a bot's damage formula at every boss pull in an instance: a [dpsf] line for dps_formula_check.py, which
// sets it against the wowsims number simpct.py computes for the same bot and boss (no decision reads it)
class PlayerbotsDpsFormulaScript : public PlayerScript
{
public:
    PlayerbotsDpsFormulaScript()
        : PlayerScript("PlayerbotsDpsFormulaScript", {PLAYERHOOK_ON_PLAYER_ENTER_COMBAT, PLAYERHOOK_ON_SPELL_CAST})
    {
    }

    // [dpscal] a solo bot's casts, for its fight's [dpsparts] line (DpsEstimate::NoteCast)
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (spell && !spell->IsTriggered())
            DpsEstimate::NoteCast(player, spell->GetSpellInfo());
    }

    void OnPlayerEnterCombat(Player* player, Unit* enemy) override
    {
        // [jrn30] THE FIGHT BEGINS. This hook is the combat-state TRANSITION (CombatManager only calls it when the
        // owner was not in combat and now is), so it is one row per fight per bot, for every bot and with no clock
        // of its own - which is what 9 467 fights with an end and no beginning were missing. It goes before the
        // [dpsf] filter below on purpose: that line is only about bosses in instances, and a fight's start is about
        // every fight anywhere.
        Journal::NoteFightStart(player, enemy);
        if (!player || !enemy || !player->IsInWorld() || !GET_PLAYERBOT_AI(player) || !player->GetMap() ||
            !player->GetMap()->Instanceable())
            return;
        Creature* boss = enemy->ToCreature();
        if (!boss || !(boss->IsDungeonBoss() || boss->isWorldBoss()))
            return;
        DpsEstimate::BuffContext const ctx = DpsEstimate::ContextOf(player);
        DpsEstimate::Result const r = DpsEstimate::Estimate(player, boss, ctx);
        LOG_INFO("playerbots",
                 "[dpsf] {} L{} {}/{} ctx={} target={} L{} formula={:.1f} white={:.1f} pet={:.1f} tps={:.1f} "
                 "mana_s={:.1f} spells={} map={} boss={}",
                 player->GetName(), player->GetLevel(), uint32(player->getClass()),
                 AiFactory::GetPlayerSpecName(player), DpsEstimate::ContextName(ctx), boss->GetEntry(),
                 uint32(boss->GetLevel()), r.burstDps, r.whiteDps, r.petDps, r.tps, r.manaPerSec, uint32(r.spells),
                 player->GetMapId(), boss->GetName());
    }
};

// [dpscal] a solo bot's hits dealt and taken, overkill included, for its fight's [dps] line (DpsEstimate::NoteDealt;
// DealDamage runs every UnitScript before the victim's health changes)
class PlayerbotsDpsMeasureScript : public UnitScript
{
public:
    PlayerbotsDpsMeasureScript() : UnitScript("PlayerbotsDpsMeasureScript") {}

    uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType damageType) override
    {
        if (damage && attacker && victim && attacker != victim)
            DpsEstimate::NoteDealt(attacker, victim, damage, uint8(damageType));
        return damage;
    }

    // [jrn30] AGGRO GAINED. Creature::AtEngage announces a creature engaging its first victim, once per creature
    // per fight, so this is where the reason a mob is on a bot is a fact rather than a reconstruction: whether the
    // bot was already fighting, whether it was walking, and whether a pull order the leader issued was open and
    // about this mob. Journal::NoteAggro turns those into the reason code; nothing is decided here.
    //
    // The one code it cannot produce is "off_tank", a mid-fight threat switch: that is a change of GetVictim()
    // between two ticks and not an engage, so it needs a watch in the leader's own tick. The code is in the schema,
    // no emitter writes it yet, and a query that finds none of it must read that as "not measured".
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        if (Creature* mob = unit ? unit->ToCreature() : nullptr)
            Journal::NoteAggro(mob, victim);
    }

    // [jrn40] THE STANDING WATCH, by the FACT of position. The user asked for it in these words: "if it stands in
    // one place for more than a minute, STANDING exactly, then for what reason" - and the project's rule is that
    // idleness is counted by fact, because a declared TRAVEL or DUNGEON status hides a hang.
    //
    // No threshold is compiled in: the row carries the duration and "longer than 30 seconds" is a WHERE clause,
    // which is also why the user's later "30 seconds was figurative" costs nothing. It writes and decides nothing.
    //
    // WHY THIS HOOK AND NOT THE BOT'S OWN TICK: Unit::Update announces every unit on its map thread, this script is
    // already registered for every unit hook, so the dispatch is already paid and what is added is one branch for a
    // non-player. The bot's own tick (PlayerbotAI) would be the other candidate, and it is not used because another
    // owner has uncommitted work across that exact function today - see the report.
    void OnUnitUpdate(Unit* unit, uint32 /*diff*/) override
    {
        if (Player* player = unit && unit->IsPlayer() ? unit->ToPlayer() : nullptr)
            Journal::NoteStanding(player);
    }
};

// [guild15] a guild bot's gear history (GuildGear::History*) and its measured damage reset when its gear, class
// spells or talents change (SoloSafety::OnPowerChanged)
class PlayerbotsGearScript : public PlayerScript
{
public:
    PlayerbotsGearScript()
        : PlayerScript("PlayerbotsGearScript",
                       {PLAYERHOOK_ON_EQUIP, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_CREATE_ITEM,
                        PLAYERHOOK_ON_QUEST_REWARD_ITEM, PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM,
                        PLAYERHOOK_ON_AFTER_STORE_OR_EQUIP_NEW_ITEM, PLAYERHOOK_ON_AFTER_TAKE_ITEM_FROM_MAIL,
                        PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_FREE_TALENT_POINTS_CHANGED, PLAYERHOOK_ON_TALENTS_RESET})
    {
    }

    void OnPlayerEquip(Player* player, Item* item, uint8 bag, uint8 slot, bool /*update*/) override
    {
        if (!player || !player->IsInWorld() || bag != INVENTORY_SLOT_BAG_0 || slot >= EQUIPMENT_SLOT_END)
            return;
        if (GuildGear::HistoryOnEquip(player, item, slot))
            PowerChanged(player, "another item equipped");
    }

    static void PowerChanged(Player* player, char const* why)
    {
        if (!player || !player->IsInWorld() || !GET_PLAYERBOT_AI(player))
            return;
        SoloSafety::OnPowerChanged(player, why);
        NewRpgBaseAction::ForgetRefusedZones(player);
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override
    {
        GuildGear::HistoryNoteSource(player, item, "loot");
    }

    void OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 /*count*/, RollVote /*voteType*/,
                                     Roll* /*roll*/) override
    {
        GuildGear::HistoryNoteSource(player, item, "loot");
    }

    void OnPlayerCreateItem(Player* player, Item* item, uint32 /*count*/) override
    {
        GuildGear::HistoryNoteSource(player, item, "craft");
    }

    void OnPlayerQuestRewardItem(Player* player, Item* item, uint32 /*count*/) override
    {
        GuildGear::HistoryNoteSource(player, item, "quest reward");
    }

    void OnPlayerAfterStoreOrEquipNewItem(Player* player, uint32 /*vendorslot*/, Item* item, uint8 /*count*/,
                                          uint8 /*bag*/, uint8 /*slot*/, ItemTemplate const* /*pProto*/,
                                          Creature* /*pVendor*/, VendorItem const* /*crItem*/, bool /*bStore*/) override
    {
        GuildGear::HistoryNoteSource(player, item, "vendor buy");
    }

    void OnPlayerAfterTakeItemFromMail(Player* player, Item* item, uint32 /*count*/) override
    {
        GuildGear::HistoryNoteSource(player, item, "guild transfer");
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellID) override
    {
        if (player && player->IsInWorld() && SoloSafety::PowerSpell(spellID))
            PowerChanged(player, "a spell learned");
    }

    void OnPlayerFreeTalentPointsChanged(Player* player, uint32 /*points*/) override
    {
        PowerChanged(player, "talent points changed");
    }

    void OnPlayerTalentsReset(Player* player, bool /*noCost*/) override
    {
        PowerChanged(player, "talents reset");
    }
};

class PlayerbotsGuildBankScript : public GuildScript
{
public:
    PlayerbotsGuildBankScript() : GuildScript("PlayerbotsGuildBankScript", {GUILDHOOK_ON_ITEM_MOVE}) {}

    void OnItemMove(Guild* /*guild*/, Player* player, Item* pItem, bool isSrcBank, uint8 /*srcContainer*/,
                    uint8 /*srcSlotId*/, bool isDestBank, uint8 /*destContainer*/, uint8 /*destSlotId*/) override
    {
        if (isSrcBank && !isDestBank)
            GuildGear::HistoryNoteSource(player, pItem, "bank");
    }
};

class PlayerbotsPlayerScript : public PlayerScript
{
public:
    PlayerbotsPlayerScript() : PlayerScript("PlayerbotsPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_BEFORE_LOGOUT,
        PLAYERHOOK_ON_AFTER_UPDATE,
        PLAYERHOOK_ON_CREATURE_KILL_CREDIT,
        PLAYERHOOK_ON_BEFORE_PETITION_SIGN,
        PLAYERHOOK_ON_BEFORE_CRITERIA_PROGRESS,
        PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_TELEPORT
    }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player->GetSession()->IsHeadless())
        {
            PlayerbotsMgr::instance().AddPlayerbotData(player, false);
            sRandomPlayerbotMgr.OnPlayerLogin(player);
            // user 2026-10-25: the spectator character "Watcher" always runs at x5 (".modify speed all 5"),
            // so it keeps up with the test bots without typing the command after every login
            if (player->GetName() == "Watcher")
                for (UnitMoveType type : {MOVE_RUN, MOVE_WALK, MOVE_SWIM, MOVE_FLIGHT})
                    player->SetSpeed(type, 5.0f, true);

            // Before modifying the following messages, please make sure it does not violate the GNU GPLv2
            // license especially if you are distributing a repack or hosting a public server
            // e.g. you can replace the URL with your own repository,
            // but it should be publicly accessible and include all modifications you've made
            if (sPlayerbotAIConfig.Enabled)
            {
                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cff00ff00This server runs with |cff00ccffmod-playerbots|r "
                    "|cffcccccchttps://github.com/mod-playerbots/mod-playerbots|r");
            }

            if (sPlayerbotAIConfig.Enabled || sPlayerbotAIConfig.RandomBotAutologin)
            {
                std::string maxAllowedBotCount = std::to_string(sRandomPlayerbotMgr.GetMaxAllowedBotCount());

                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cff00ff00Playerbots:|r The server is configured with " + maxAllowedBotCount + " bots.");
            }
        }
    }

    void OnPlayerBeforeLogout(Player* player) override
    {
        DpsEstimate::ForgetFight(player);  // [dps20] an open fight leaves with its bot: nothing to measure in it
        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
        {
            PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

            if (botAI == nullptr || IsSelfBot(player))
                playerbotMgr->LogoutAllBots();
        }

        sRandomPlayerbotMgr.OnPlayerLogout(player);
    }

    void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
    {
        GuildTaskMgr::instance().CheckKillTask(player, killed);
    }

    void OnPlayerBeforePetitionSign(Player* player, ObjectGuid /*petitionGuid*/, bool& alreadySignedByAccount) override
    {
        if (!alreadySignedByAccount)
            return;

        // bots share accounts, so the same-account rule must not stop them from signing each other
        if (PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr)
            alreadySignedByAccount = false;
    }

    bool OnPlayerBeforeTeleport(Player* /*player*/, uint32 /*mapid*/, float /*x*/, float /*y*/, float /*z*/,
                                float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        /* for now commmented out until proven its actually required
        * havent seen any proof CleanVisibilityReferences() is needed

        // If the player is not safe to touch, do nothing
        if (!player)
            return true;

        // If same map or not in world do nothing
        if (!player->IsInWorld() || player->GetMapId() == mapid)
            return true;

        // If this is a SelfBot, do nothing
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI || IsSelfBot(player))
            return true;

        // Cross-map bot teleport: defer visibility reference cleanup.
        // CleanVisibilityReferences() erases this bot's GUID from other objects' visibility containers.
        // This is intentionally done via the event queue (instead of directly here) because erasing
        // from other players' visibility maps inside the teleport call stack can hit unsafe re-entrancy
        // or iterator invalidation while visibility updates are in progress
        ObjectGuid guid = player->GetGUID();
        player->m_Events.AddEventAtOffset(
            [guid, mapid]()
            {
                // do nothing, if the player is not safe to touch
                Player* p = ObjectAccessor::FindPlayer(guid);
                if (!p || !p->IsInWorld() || p->IsDuringRemoveFromWorld())
                    return;

                // do nothing if we are already on the target map
                if (p->GetMapId() == mapid)
                    return;

                p->GetObjectVisibilityContainer().CleanVisibilityReferences();
            },
            Milliseconds(0));

        */

        return true;
    }

    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        // A SelfBot whose player just took control by input waits for its removal on the world thread.
        if (botAI != nullptr && !SelfBotControl::IsSuspended(player))
        {
            auto const aiStart = std::chrono::steady_clock::now();  // [guild30] bot AI time per tick
            SlowUpdate::Begin();  // [perf212] a slow update names its stages
            botAI->UpdateAI(diff);
            SlowUpdate::End(player->GetName());
            sGuildCoordinator.AddAiTime(player, uint64(std::chrono::duration_cast<std::chrono::microseconds>(
                                                    std::chrono::steady_clock::now() - aiStart)
                                                    .count()));
            // [dps20] a fight of this bot that no other judge closes (one in a party, a raid or any instance:
            // SoloSafety closes a solo bot's open-world fight itself). It returns on the first test while the bot is
            // in combat, and takes the measurement's lock only for a bot that is out of combat or dead.
            DpsEstimate::CloseIfOut(player);
        }

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
        {
            playerbotMgr->UpdateAI(diff);
        }
    }

    using PlayerScript::OnPlayerCanUseChat;  // keep the base overloads visible

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || lang == LANG_ADDON)
        {
            return true;
        }

        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);

        if (botAI == nullptr)
        {
            return true;
        }

        // [help] a human's help request whispered to a guild bot goes to the global leader, not the bot's commands
        if (receiver && sGuildCoordinator.IsPlanned(receiver->GetGUID().GetCounter()) &&
            NewRpgCoop::HelpChat(player, msg, receiver))
            return true;

        botAI->HandleCommand(type, msg, player);

        // hotfix; otherwise the server will crash when whispering logout
        // https://github.com/mod-playerbots/mod-playerbots/pull/1838
        // TODO: find the root cause and solve it. (does not happen in party chat)
        if (msg == "logout")
            return false;

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
    {
        // Addon traffic (DBM, Carbonite, ...) is no bot command; its text used to trigger item commands like trade.
        if (lang == LANG_ADDON)
            return true;

        // A SelfBot's own player gives orders to the group, not to the character the SelfBot drives;
        // "@me <command>" addresses only that SelfBot.
        bool const ownSelfBot = sPlayerbotAIConfig.SelfBotIgnoreOwnGroupCommands && IsSelfBot(player);
        bool const toSelfOnly = ownSelfBot && msg.compare(0, 4, "@me ") == 0;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* const member = itr->GetSource();

            if (member == nullptr)
                continue;

            PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);

            if (botAI == nullptr)
                continue;

            if (ownSelfBot && (member == player) != toSelfOnly)
                continue;

            botAI->HandleCommand(type, toSelfOnly ? msg.substr(4) : msg, player);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* /*guild*/) override
    {
        if (type != CHAT_MSG_GUILD || lang == LANG_ADDON)
            return true;

        // [help] «помогите <quest>» in guild chat: the global leader answers it
        if (NewRpgCoop::HelpChat(player, msg, nullptr))
            return true;

        PlayerbotMgr* playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);

        if (playerbotMgr == nullptr)
            return true;

        for (PlayerBotMap::const_iterator it = playerbotMgr->GetPlayerBotsBegin(); it != playerbotMgr->GetPlayerBotsEnd(); ++it)
        {
            Player* const bot = it->second;

            if (bot == nullptr)
                continue;

            if (bot->GetGuildId() != player->GetGuildId())
                continue;

            PlayerbotsMgr::instance().GetPlayerbotAI(bot)->HandleCommand(type, msg, player);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        PlayerbotMgr* const playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);

        if (playerbotMgr != nullptr && channel->GetFlags() & 0x18)
            playerbotMgr->HandleCommand(type, msg);

        sRandomPlayerbotMgr.HandleCommand(type, msg, player);

        return true;
    }

    bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if ((sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player)) &&
            (achievement->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_REACH | ACHIEVEMENT_FLAG_REALM_FIRST_KILL)))
        {
            return false;
        }

        return true;
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        // early return
        if (sPlayerbotAIConfig.RandomBotXPRate == 1.0 || !player)
            return;

        // no XP multiplier, when player is no bot.
        if (!player->GetSession()->IsHeadless() || !sRandomPlayerbotMgr.IsRandomBot(player))
            return;

        // no XP multiplier, when bot is in a group with a real player.
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->GetSource();
                if (!member)
                    continue;

                if (!member->GetSession()->IsHeadless())
                    return;
            }
        }

        // otherwise apply bot XP multiplier.
        amount = static_cast<uint32>(std::round(static_cast<float>(amount) * sPlayerbotAIConfig.RandomBotXPRate));
    }
};

class PlayerbotsMiscScript : public MiscScript
{
public:
    PlayerbotsMiscScript() : MiscScript("PlayerbotsMiscScript", {MISCHOOK_ON_DESTRUCT_PLAYER}) {}

    void OnDestructPlayer(Player* player) override
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
            delete botAI;

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
            delete playerbotMgr;
    }
};

class PlayerbotsServerScript : public ServerScript
{
public:
    PlayerbotsServerScript() : ServerScript("PlayerbotsServerScript", {
        SERVERHOOK_ON_PACKET_SENT,
        SERVERHOOK_CAN_PACKET_RECEIVE
    }) {}

    void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
    {
        Player* player = session->GetPlayer();

        if (player == nullptr)
            return;

        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
            botAI->HandleBotOutgoingPacket(packet);

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
            playerbotMgr->HandleMasterOutgoingPacket(packet);
    }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (Player* player = session->GetPlayer())
            if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
                playerbotMgr->HandleMasterIncomingPacket(packet);

        return true;
    }
};

class PlayerbotsSessionScript : public SessionScript
{
public:
    PlayerbotsSessionScript() : SessionScript("PlayerbotsSessionScript", {
        SESSIONHOOK_ON_UPDATE
    }) {}

    // Runs on the world thread for every real session: drains the packet queues of the bots it owns
    void OnSessionUpdate(WorldSession* session, uint32 /*diff*/) override
    {
        if (Player* player = session->GetPlayer())
            if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
                playerbotMgr->UpdateSessions();
    }
};

class PlayerbotsAllMapScript : public AllMapScript
{
public:
    PlayerbotsAllMapScript() : AllMapScript("PlayerbotsAllMapScript", {
        ALLMAPHOOK_ON_MAP_UPDATE,  // [jrn31] each map thread's own update time, never their sum
        ALLMAPHOOK_CAN_SEND_OBJECT_UPDATES_TO_PLAYER
    }) {}

    // [jrn31] EACH MAP SEPARATELY. One stalled map thread among nineteen healthy ones is a sum that looks fine,
    // and "the total looked fine" is its own class of defect here - so the journal gets a row per map and there is
    // no row that adds them up. The sampler is one lock and one push_back of a float; it writes the window out
    // every ten seconds from the world thread, not from here.
    void OnMapUpdate(Map* map, uint32 diff) override
    {
        if (map)
            Journal::NoteMapTick(map->GetId(), map->GetInstanceId(), diff);
    }

    // Bots have no client to render object updates; only self bots still need them
    bool CanSendObjectUpdatesToPlayer(Map* /*map*/, Player* player) override
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI == nullptr)
            return true;

        return IsSelfBot(player);
    }
};

class PlayerbotsGlobalScript : public GlobalScript
{
public:
    PlayerbotsGlobalScript() : GlobalScript("PlayerbotsGlobalScript", {
        GLOBALHOOK_CAN_CREATE_LFG_PROPOSAL
    }) {}

    // Never form a dungeon group made only of bots
    bool CanCreateLfgProposal(lfg::Lfg5Guids const& guids) override
    {
        for (ObjectGuid const& guid : guids.guids)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);

            if (guid.IsGroup() || IsRealPlayer(player) || IsSelfBot(player))
                return true;
        }

        return false;
    }
};

class PlayerbotsWorldScript : public WorldScript
{
public:
    PlayerbotsWorldScript() : WorldScript("PlayerbotsWorldScript", {
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_SHUTDOWN
    }) {}

    void OnBeforeWorldInitialized() override
    {
        // Before modifying the following messages, please make sure it does not violate the GNU GPLv2
        // license especially if you are distributing a repack or hosting a public server
        // e.g. you can replace the URL with your own repository,
        // but it should be publicly accessible and include all modifications you've made
        LOG_INFO("server.loading", "╔══════════════════════════════════════════════════════════╗");
        LOG_INFO("server.loading", "║                                                          ║");
        LOG_INFO("server.loading", "║              AzerothCore Playerbots Module               ║");
        LOG_INFO("server.loading", "║                                                          ║");
        LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
        LOG_INFO("server.loading", "║     mod-playerbots is a community-driven open-source     ║");
        LOG_INFO("server.loading", "║  project based on AzerothCore, licensed under GNU GPLv2  ║");
        LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
        LOG_INFO("server.loading", "║     https://github.com/mod-playerbots/mod-playerbots     ║");
        LOG_INFO("server.loading", "╚══════════════════════════════════════════════════════════╝");

        uint32 oldMSTime = getMSTime();

        LOG_INFO("server.loading", " ");
        LOG_INFO("server.loading", "Load Playerbots Config...");

        sPlayerbotAIConfig.Initialize();

        LOG_INFO("server.loading", ">> Loaded playerbots config in {} ms", GetMSTimeDiffToNow(oldMSTime));
        LOG_INFO("server.loading", " ");

        PlayerbotSpellRepository::Instance().Initialize();
        CheckMountStateAction::LoadPreferredMounts();

        LOG_INFO("server.loading", "Loading gather node index...");
        GatherNodeMgr::instance().Load();

        LOG_INFO("server.loading", "Playerbots World Thread Processor initialized");
    }

    // [guild12] the New RPG quest indexes (world DB reads) once all world data is loaded, not in a bot's tick
    void OnStartup() override
    {
        // [guild40] main() runs OnStartup and then the world update loop on this same thread
        PlayerbotWorldThreadProcessor::MarkWorldThread();

        uint32 const started = getMSTime();
        NewRpgBaseAction::BuildQuestIndexes();
        LOG_INFO("server.loading", ">> New RPG quest indexes built in {} ms", GetMSTimeDiffToNow(started));
        Leader::Travel::WalkGraph::StartBuild();  // [nav180] in the background: the walks plan without it until ready
    }

    void OnUpdate(uint32 diff) override
    {
        PlayerbotAI::RefreshChannelDirectory();  // [guild40] channel copy for bot chat on map threads
        PlayerbotHolder::UpdatePendingLogins();  // Headless sessions whose login holder is in flight
        sRandomPlayerbotMgr.UpdateSessions();  // Per-bot packet queues, world thread only
        PlayerbotWorldThreadProcessor::instance().Update(diff);
        sRandomPlayerbotMgr.UpdateAI(diff);  // World thread only
        // [jrn31] the server's own statistics into pb_journal_metric, and the window is written out from here.
        // It is called LAST so the tick it measures is a whole tick, the bots' work included; `bots` and `sessions`
        // go in the row because a tick time at 75 bots and one at 300 are not the same number, and nothing else in
        // the row would say so.
        Journal::NoteWorldTick(diff, sRandomPlayerbotMgr.GetPlayerbotsCount(),
                               sWorldSessionMgr->GetActiveSessionCount());
    }

    // Runs before the sessions are kicked on server shutdown
    void OnShutdown() override
    {
        LOG_INFO("playerbots", "Logging out all bots...");
        sRandomPlayerbotMgr.LogoutAllBots();
        PlayerbotHolder::ClearPendingLogins();
        Leader::Travel::WalkGraph::StopBuild();  // [nav180]
    }
};

class PlayerBotsBGScript : public BGScript
{
public:
    PlayerBotsBGScript() : BGScript("PlayerBotsBGScript") {}

    void OnBattlegroundStart(Battleground* bg) override
    {
        BGStrategyData data;
        BattlegroundTypeId bgType = bg->GetBgTypeID();
        if (bgType == BATTLEGROUND_RB)  // a random-queue game: the rolled map
            bgType = bg->GetBgTypeID(true);

        switch (bgType)
        {
            case BATTLEGROUND_WS:
                data.allianceStrategy = urand(0, WS_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, WS_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_AB:
                data.allianceStrategy = urand(0, AB_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, AB_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_AV:
                data.allianceStrategy = urand(0, AV_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, AV_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_EY:
                data.allianceStrategy = urand(0, EY_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, EY_STRATEGY_MAX - 1);
                break;
            default:
                break;
        }

        BGTactics::SetBotStrategies(bg->GetInstanceID(), data);
    }

    void OnBattlegroundEnd(Battleground* bg, TeamId /*winnerTeam*/) override
    {
        BGTactics::ClearBotStrategies(bg->GetInstanceID());
    }
};

// Workaround for missing InitEnabledHooksIfNeeded for new BattlefieldScript in ScriptMgr
class PlayerbotsBattlefieldScript : public BattlefieldScript
{
public:
    PlayerbotsBattlefieldScript() : BattlefieldScript("PlayerbotsBattlefieldScript") { }
};

void AddPlayerbotsSecureLoginScripts();
void AddPlayerbotsSelfBotAfkScripts();
void AddLeaderDungeonRunScripts();  // [guild70] Ai/Leader/Dungeon/LeaderDungeonRun.cpp
void AddQuestStatsScripts();        // [guild90] Ai/World/Rpg/QuestStats.cpp
void AddNewRpgCoopScripts();        // [coop] Ai/World/Rpg/NewRpgCoop.cpp
void AddSC_MagtheridonBotScripts();
void AddSC_SerpentshrineCavernBotScripts();
void AddSC_TempestKeepBotScripts();
void AddSC_HyjalBotScripts();
void AddSC_DungeonBotScripts();
void AddSC_IcecrownBotScripts();
void AddSC_RubySanctumBotScripts();
void AddSC_randombot_level_mgr();
void AddPlayerbotsDeathRecorder();
void AddPlayerbotsInstanceResetCatchUp();
void AddPlayerbotsLowLevelDeathKnightStart();
void AddPlayerbotsTestHarness();
void AddPlayerbotsUserPack();

void AddPlayerbotsScripts()
{
    // First: its chat hook drops cheat commands to the user's pack before PlayerbotsPlayerScript hands them to bots.
    AddPlayerbotsUserPack();
    new PlayerbotsBattlefieldScript();
    new PlayerbotsDatabaseScript();
    new PlayerbotsPlayerScript();
    new PlayerbotsGearScript();       // [guild15]
    new PlayerbotsDpsFormulaScript();  // [dps1]
    new PlayerbotsDpsMeasureScript();  // [dpscal]
    new PlayerbotsGuildBankScript();  // [guild15]
    new PlayerbotsDeathAndOnlineScript();  // [guild5]
    new PlayerbotsMiscScript();
    new PlayerbotsServerScript();
    new PlayerbotsSessionScript();
    new PlayerbotsAllMapScript();
    new PlayerbotsGlobalScript();
    new PlayerbotsWorldScript();
    new PlayerBotsBGScript();
    AddPlayerbotsSecureLoginScripts();
    AddPlayerbotsSelfBotAfkScripts();
    AddGuildCoordinatorScripts();  // [guild30]
    AddLeaderDungeonRunScripts();  // [guild70] the five-leader's dungeon runs (world thread)
    AddQuestStatsScripts();        // [guild90] questing counters ([qstat] / [qprog] lines)
    AddNewRpgCoopScripts();        // [coop] co-op questing: shared kills, party XP, exits on death / logout
    AddPlayerbotsCommandscripts();
    AddPlayerbotsDeathRecorder();
    AddPlayerbotsInstanceResetCatchUp();
    AddPlayerbotsLowLevelDeathKnightStart();
    AddPlayerbotsTestHarness();
    PlayerBotsGuildValidationScript();
    AddSC_MagtheridonBotScripts();
    AddSC_SerpentshrineCavernBotScripts();
    AddSC_TempestKeepBotScripts();
    AddSC_HyjalBotScripts();
    AddSC_DungeonBotScripts();
    AddSC_IcecrownBotScripts();
    AddSC_RubySanctumBotScripts();
    AddSC_randombot_level_mgr();
}
