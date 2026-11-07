/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDCOORDINATOR_H
#define PLAYERBOTS_GUILDCOORDINATOR_H

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Define.h"
#include "TravelMgr.h"

class Creature;
class Item;
class Player;
class PlayerbotAI;
class Quest;

// [guild30] The global leader of the guild project = the server itself (no character). It runs on the world thread
// every few seconds over the players in memory (no DB query after startup) and holds:
//   - the chain of command per bot: member -> five-leader -> raid leader (A / B, a bot) -> global;
//   - one order list per bot (QUEST is the default, New RPG runs any other executable order first);
//   - the computed wanted list (crafters' skill-up recipes x reagents, members' quest items);
//   - the loot flow (every looted item is reported here and gets a fate: upgrade for a member, guild bank, disenchant,
//     vendor);
//   - the health monitor (stuck / death / loop / perf alerts, tick and AI time, per-hour rates).
// Map threads only call the small thread-safe entry points marked "map thread".

enum class GuildOrderType : uint8
{
    QUEST = 0,
    PROFESSION,
    BANK_DEPOSIT,
    BANK_WITHDRAW,
    CRAFT_FOR,
    GATHER,
    BUY,
    DUNGEON,
    DISENCHANT,
    QUEST_REWARD  // [guild50] which choice reward to take at the turn-in (applied by the turn-in itself)
};

struct GuildOrder
{
    GuildOrderType type{GuildOrderType::QUEST};
    uint32 itemId{0};
    uint32 count{0};
    uint32 forLow{0};        // the member it is for (0 = the guild bank)
    std::string details;
    std::string reason;
    std::string status;
    bool executable{false};  // false = dormant (no guild / no tab / stage 2): New RPG goes on with quests
    uint32 since{0};         // game time seconds
    // [guild50] BUY: the vendor spawn and price; CRAFT_FOR: the recipe
    uint32 npcEntry{0};
    WorldPosition pos{};
    uint32 cost{0};
    uint32 spellId{0};
};

// What New RPG does for the order (map thread).
struct GuildRunStep
{
    enum Kind : uint8
    {
        NONE,    // no executable order: quests
        GATHER,  // DO_GATHER in this zone
        WALK,    // GO_CAMP to pos (the guild vault)
        BUSY     // the order acted this tick (cast started)
    };
    Kind kind{NONE};
    WorldPosition pos{};
    std::string reason;
};

// [guild60] a five the global leader sent to a dungeon: it walks to the entrance (each member its own safe walk),
// becomes a real group there, and the dungeon leader (its five-leader) runs the dungeon; afterwards authority goes
// back to the global leader with a report.
struct GuildFive
{
    enum class Stage : uint8
    {
        Gather,  // walking to the entrance; waits there for the rest (a human: high priority, the five waits)
        Run,     // grouped, inside or walking in through the portal: the five-leader leads (dungeon leader)
        Done
    };

    uint32 id{0};
    uint32 leaderLow{0};
    std::vector<uint32> members;  // the five-leader included
    std::string dungeon;          // the dungeon's name
    uint32 since{0};
    Stage stage{Stage::Gather};
    std::string contentKey;  // the dungeon leader's content ("wc-n")
    uint32 lfgId{0};
    WorldPosition entrance{};  // the outside portal (AreaTrigger)
    float entranceReach{10.0f};
    std::map<uint32, std::string> roles;  // member -> tank / healer / dps
    std::set<uint32> arrived;             // members standing at the entrance
    std::set<uint32> humans;              // human players in the five: never commanded, waited for
    uint32 quartermasterLow{0};           // the five-leader: the consumables reserve in the dungeon
    uint32 enchanterLow{0};               // the five's enchanter (spare BoP is disenchanted by it), 0: none
    std::string why;                      // the global leader's reason for this five
    std::string runId;                    // the dungeon leader's run
    std::vector<std::string> drops;       // loot of the run, "item (who)"
    std::map<uint32, uint32> deaths;      // member -> deaths in the run
    bool forced{false};                   // formed by the command
    bool humanAlerted{false};
    // the five-leader's roll decisions: (loot source guid, item) -> (member who needs it, 0: none; disenchant)
    std::map<std::pair<uint64, uint32>, std::pair<uint32, bool>> rolls;
};

// [guild60] one finished five run (the completion table, playerbots DB guild_project_runs)
struct GuildRunRecord
{
    uint32 id{0};
    std::string contentKey;
    std::string kind;  // "dungeon" / "raid"
    uint32 at{0};      // unix time
    uint32 leaderLow{0};
    std::vector<uint32> members;
    std::string result;
    uint32 bossesKilled{0};
    uint32 bossesTotal{0};
    uint32 deaths{0};
};

// [guild60] the dungeon leader's end of a five's run, as the global leader gets it
struct GuildFiveReport
{
    std::string runId;
    std::string result;
    std::string reason;
    std::vector<std::string> bossesKilled;
    uint32 bossesTotal{0};
    uint32 wipes{0};
    uint32 durationSec{0};
    std::vector<std::pair<std::string, uint32>> deaths;
};

class GuildCoordinator
{
public:
    static GuildCoordinator& instance()
    {
        static GuildCoordinator instance;
        return instance;
    }

    // world thread
    void Load();
    void Update(uint32 diff);
    void SendOrders(Player* player);  // addon "BOTCTL\torders"
    void SendResources(Player* player);  // addon "BOTCTL\tresources": the virtual guild bank
    bool IsPlanned(uint32 low) const { return planIndex_.count(low) != 0; }

    // [guild60] fives: the global leader forms them (UpdateFives) or the command does; world thread.
    // FormFive: members incl. the leader, `dungeon` = the content key ("wc-n"); 0 with `error` when it cannot.
    uint32 FormFive(uint32 leaderLow, std::vector<uint32> const& members, std::string const& dungeon,
                    std::string* error = nullptr, bool forced = false);
    void DisbandFive(uint32 fiveId, std::string const& reason = "");
    void SetOrder(uint32 low, GuildOrder order, std::string const& decision);
    // "playerbots guild formfive <dungeon> [me] [name ...]": picks the rest itself (tank, healer, 3 dps of the
    // dungeon's band), skips the need check (a forced test); returns the answer text
    std::string FormFiveCommand(Player* issuer, std::string const& contentKey, std::vector<std::string> const& names,
                                bool withIssuer);
    std::string FivesText() const;  // "playerbots guild fives"
    std::string DungeonsText(uint8 level) const;  // "playerbots guild dungeons [level]": the band and why / why not
    void SetAutoFives(bool on) { autoFives_ = on; }
    bool AutoFives() const { return autoFives_; }
    // the dungeon leader's end of the five's run (TestHarnessMgr hook, world thread)
    void OnFiveRunEnd(uint32 fiveId, GuildFiveReport const& report);
    // map thread: the five-leader's word on a group roll of a BoP item (false: not a five's roll, the bot decides)
    bool FiveRollVote(Player* bot, uint64 lootGuid, uint32 itemId, int32 randomProperty, uint8& vote);

    // map thread
    GuildRunStep NextStep(Player* bot, PlayerbotAI* botAI);
    uint8 OnCampReached(Player* bot);  // 0 = not a guild run, 1 = done (-> IDLE), 2 = still at it
    bool IsHeld(Player* bot, Item* item);
    void SellMarked(PlayerbotAI* botAI);
    void SetOrderStatus(Player* bot, std::string const& status);
    void OnLoot(Player* player, Item* item, uint32 count);
    void OnKilledByCreature(Player* killed, Creature* killer);
    void OnJustDied(Player* killed);
    void OnRpgStatus(Player* bot, uint8 status);
    void SetWait(Player* bot, std::string const& reason);  // a legitimate wait the bot sets itself ("" clears)
    void AddAiTime(Player* bot, uint64 microseconds);
    // [guild50] the choice reward the global leader picked for this quest (-1 = none: the bot picks itself)
    int32 RewardChoice(Player* bot, Quest const* quest);

    struct PlanRow
    {
        uint32 low{0};
        uint32 num{0};
        std::string name;
        char grp{'P'};
        std::string role;
        std::vector<uint32> profs;  // planned profession skill lines
    };

    struct WantedItem
    {
        uint32 itemId{0};
        uint32 need{0};
        uint32 have{0};
        uint32 gatherSkill{0};     // SKILL_MINING / SKILL_HERBALISM when nodes give it
        uint32 gatherReq{0};
        bool vendor{false};
        std::string forWhom;       // "#9 BS 47->75"
        uint32 Short() const { return need > have ? need - have : 0; }
    };

private:
    GuildCoordinator() = default;

    struct Recipe
    {
        uint32 spellId{0};
        uint32 skillLine{0};
        uint32 reqSkill{0};      // trainer ReqSkillRank / SkillLineAbility MinSkillLineRank
        uint32 reqLevel{0};
        uint32 yellow{0};        // TrivialSkillLineRankLow
        uint32 gray{0};          // TrivialSkillLineRankHigh
        bool trainer{false};
        bool autoLearned{false};
        std::vector<std::pair<uint32, uint32>> reagents;
    };

    struct HeldItem
    {
        uint32 itemGuidLow{0};
        uint32 itemId{0};
        uint32 count{0};
        uint32 forLow{0};      // member it is for, 0 = bank/wanted
        std::string purpose;   // "upgrade #12", "wanted", "enchanter #19", "pending"
    };

    // [guild50] one upgrade of a member: the item, its slot group, the gain vs the weakest worn piece, the source
    struct GearPick
    {
        uint8 group{0};
        uint32 itemId{0};
        float gain{0.0f};
        uint8 kind{0};        // GuildGear::SourceKind
        uint32 srcId{0};      // quest / boss / recipe spell / vendor entry
        uint32 aux{0};        // quest choice index / skill line / price
        uint32 mapId{0};
        float chance{0.0f};
        uint32 crafterLow{0};
        WorldPosition pos{};  // vendor spawn
        std::string text;     // where from, for the tab
    };

    struct RewardPick
    {
        uint32 index{0};
        uint32 itemId{0};
        uint32 forLow{0};  // another member it goes to (via the bank); 0 = the bot itself / sale
        std::string why;
    };

    struct Expect
    {
        uint32 itemId{0};
        uint32 forLow{0};
        std::string purpose;
    };

    struct BotState
    {
        std::vector<GuildOrder> orders;  // [0] = shown; executable ones are run in this order
        std::vector<GuildOrder> external;  // set through SetOrder (stage 2 kinds)
        bool run{false};                 // walking to the vault for runType
        GuildOrderType runType{GuildOrderType::QUEST};
        WorldPosition runPos{};
        bool runApproached{false};
        std::string vaultFail;
        std::vector<HeldItem> held;
        std::vector<uint32> sell;        // item guid lows to sell at the next vendor
        std::vector<uint32> disenchant;  // item guid lows to disenchant itself
        uint32 raidLeaderLow{0};
        uint32 fiveId{0};
        std::string fiveRefused;  // [guild60] map thread: no safe walk to the five's entrance (the five is called off)
        // health
        uint32 mapId{0};
        float ax{0}, ay{0}, az{0};
        uint32 anchorAt{0};
        bool stuckAlerted{false};
        std::string stuckKey;
        std::string waitReason;
        std::deque<uint32> deaths;  // game time
        bool deathOpen{false};      // dead since the last DEATH alert
        uint64 xpTotal{0};
        uint32 questsDone{0};
        uint32 money{0};
        bool seen{false};
        std::map<uint8, std::tuple<uint64, uint32, float, float, float, uint32>> statusEntry;  // xp, progress, pos, map
        std::unordered_set<uint8> loopAlerted;
        // [guild50] gear through the global leader
        std::vector<GearPick> gearNow;   // per slot group: the best upgrade reachable now (bank, reward, craft, buy)
        std::vector<GearPick> gearPlan;  // per slot group: the best reachable upgrade of any source
        std::map<uint32, RewardPick> rewards;  // quest -> the choice to take
        std::vector<Expect> expect;      // items about to appear in its bags that belong to another member
        uint32 gearAt{0};
    };

    struct LootReport
    {
        uint32 playerLow{0};
        uint32 itemGuidLow{0};
        uint32 itemId{0};
        uint32 count{0};
    };

    struct DeathReport
    {
        uint32 playerLow{0};
        bool creature{false};
        std::string killer;
        std::string cause;
    };

    // [guild60] a normal 5-man dungeon the fives can run: its LFGDungeons band, the dungeon leader's content, the
    // outside portal and its bosses' equipment drops (the upgrade check)
    struct DungeonOption
    {
        uint32 lfgId{0};
        std::string name;
        std::string contentKey;
        uint32 mapId{0};
        uint32 minLevel{0};
        uint32 maxLevel{0};
        uint32 targetLevel{0};
        uint32 expansion{0};
        WorldPosition entrance{};
        float reach{10.0f};
        uint32 entranceZone{0};
        std::vector<uint32> bossItems;
    };

    struct FiveCandidate
    {
        uint32 low{0};
        Player* player{nullptr};
        std::string role;  // tank / healer / dps
        bool needsClear{false};
        bool upgrade{false};
        bool enchanter{false};
    };

    void LoadPlan();
    void LoadRecipes();
    void LoadGatherSources();
    void LoadDungeons();
    void LoadRuns();

    // [guild60] fives (world thread)
    void UpdateFives();
    void AutoFive();
    DungeonOption const* FindDungeon(std::string const& contentKey) const;
    std::string DungeonRefusal(DungeonOption const& d, Player* p) const;  // "" = the dungeon fits this character
    bool HoldsUpgrade(DungeonOption const& d, Player* p) const;
    bool FreeForFive(uint32 low, Player* p, std::string* why = nullptr) const;  // lock_ held
    std::string PlanRole(uint32 low, Player* p) const;
    bool Cleared(std::string const& contentKey, uint32 low) const;  // lock_ held
    std::vector<FiveCandidate> Candidates(DungeonOption const& d, bool needCheck);  // lock_ held
    bool PickFive(std::vector<FiveCandidate> const& pool, std::vector<FiveCandidate>& out) const;
    uint32 FormFiveLocked(uint32 leaderLow, std::vector<uint32> const& members, DungeonOption const& d,
                          std::map<uint32, std::string> const& roles, std::string const& why, bool forced,
                          std::string* error);
    void FiveOrder(BotState const& st, uint32 low, Player* p, std::vector<GuildOrder>& orders) const;  // lock_ held
    void RecordRun(GuildFive const& five, GuildFiveReport const& report);  // lock_ held
    // [guild60] hooks of the order flow (lock_ held): the member's walk to the entrance (true: `step` set), its
    // arrival there (OnCampReached), its legitimate wait there, the tab lines, a five's loot (map thread, no lock)
    bool FiveStep(Player* bot, BotState& st, GuildRunStep& step);
    uint8 FiveCampReached(Player* bot, BotState& st);
    std::string FiveWait(BotState const& st) const;
    void SendFives(Player* player) const;
    void FiveLoot(Player* player, Item* item, uint32 count);

    void UpdateLeaders();
    void UpdateWanted();
    void UpdateOrders();
    void UpdateGear();  // [guild50] one member's upgrade plan per call
    void PlanGear(Player* m, std::vector<GearPick>& now, std::vector<GearPick>& plan,
                  std::map<uint32, RewardPick>& rewards, std::vector<Player*> const& bots,
                  std::map<uint32, uint32> const& bank, std::vector<WantedItem> const& wanted);
    void UpdateCrafts(uint32 now);  // [guild50] craft queue (upgrade crafts + bags), its wanted mats; lock_ held
    static bool HumanControls(Player* p);
    void ProcessLoot();
    void ProcessDeaths();
    void UpdateHealth();
    void DecideLoot(Player* looter, Item* item, LootReport const& report);

    void Decision(std::string text);
    void Alert(std::string text);
    std::string Chain(uint32 low) const;
    std::string Who(uint32 low) const;
    std::string RaidLeaderText(char grp) const;
    PlanRow const* Row(uint32 low) const;
    uint32 SkillUpNeeds(Player* crafter, uint32 skillLine, std::map<uint32, uint32>& needs) const;
    static uint64 XpTotal(Player* bot);
    static uint32 QuestProgress(Player* bot);
    std::string WaitReason(Player* bot, PlayerbotAI* botAI, BotState const& state) const;
    std::string StatusName(Player* bot, PlayerbotAI* botAI) const;

    std::vector<PlanRow> plan_;
    std::unordered_map<uint32, size_t> planIndex_;  // low -> plan_ index; read-only after Load
    std::unique_ptr<std::atomic<uint64>[]> aiTimeUs_;  // per plan row, since the last health sample
    size_t aiCount_{0};
    std::atomic<uint64> aiTimeTotalUs_{0};

    std::unordered_map<uint32, std::vector<Recipe>> recipesBySkill_;
    std::unordered_map<uint32, std::pair<uint32, uint32>> gatherSource_;  // item -> (skill, min node skill)
    std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> smeltFrom_;  // bar -> ore x count per bar
    std::unordered_set<uint32> vendorItems_;

    mutable std::mutex lock_;
    std::unordered_map<uint32, BotState> bots_;
    std::map<uint32, GuildFive> fives_;
    uint32 nextFiveId_{1};
    std::vector<DungeonOption> dungeons_;  // [guild60] read-only after Load
    std::vector<GuildRunRecord> runs_;     // [guild60] the completion table (all runs, oldest first)
    std::unordered_map<std::string, std::unordered_set<uint32>> cleared_;  // content -> characters that cleared it
    bool autoFives_{true};
    uint32 fiveTimer_{0};
    uint32 autoFiveTimer_{0};
    uint32 raidLeader_[2]{0, 0};  // A, B
    std::vector<WantedItem> wanted_;
    std::unordered_map<uint32, std::vector<std::pair<uint32, std::string>>> withdraw_;  // member -> bank items for it
    uint32 deathsTotal_{0};
    std::deque<std::pair<uint32, std::string>> decisions_;
    std::deque<std::pair<uint32, std::string>> alerts_;
    std::vector<LootReport> lootQueue_;
    std::vector<DeathReport> deathQueue_;
    std::vector<std::pair<uint32, DeathReport>> envPending_;  // (update number, report)
    uint32 deathCall_{0};
    std::string healthRow_;
    std::string guildState_;
    // [guild50]
    struct CraftJob
    {
        uint32 crafterLow{0};
        uint32 spellId{0};
        uint32 itemId{0};
        uint32 forLow{0};
        bool bag{false};
        bool ready{false};  // the crafter holds the reagents of one craft
        std::string why;
    };
    std::vector<CraftJob> crafts_;
    std::map<uint32, std::pair<uint32, std::string>> gearWanted_;  // reagent -> (count, for whom)
    std::string bagState_;
    size_t gearCursor_{0};

    // health samples (world thread)
    std::deque<std::pair<uint32, uint32>> ticks_;  // (ms time, diff ms)
    std::deque<std::pair<uint32, uint32>> aiTicks_;  // (ms time, ai us in that tick)
    struct RateSample
    {
        uint32 at{0};
        uint64 xp{0};
        uint32 quests{0};
        uint32 deaths{0};
        int64 money{0};
    };
    std::deque<RateSample> rates_;  // per health update, last hour of deltas
    bool perfAlerted_{false};
    uint32 updateTimer_{0};
    uint32 healthTimer_{0};
    uint32 wantedTimer_{0};
    uint32 lastHealthLog_{0};
    bool loaded_{false};
};

#define sGuildCoordinator GuildCoordinator::instance()

void AddGuildCoordinatorScripts();

#endif
