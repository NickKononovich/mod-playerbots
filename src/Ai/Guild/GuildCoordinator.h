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
    DISENCHANT
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

struct GuildFive
{
    uint32 id{0};
    uint32 leaderLow{0};
    std::vector<uint32> members;
    std::string dungeon;
    uint32 since{0};
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

    // five-leaders (stage 5 forms them; data + API now)
    uint32 FormFive(uint32 leaderLow, std::vector<uint32> const& members, std::string const& dungeon);
    void DisbandFive(uint32 fiveId);
    void SetOrder(uint32 low, GuildOrder order, std::string const& decision);

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

    void LoadPlan();
    void LoadRecipes();
    void LoadGatherSources();

    void UpdateLeaders();
    void UpdateWanted();
    void UpdateOrders();
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
