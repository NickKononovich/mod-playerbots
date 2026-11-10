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
#include <tuple>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"  // [lock23]
#include "QuestDef.h"  // [questrate] QUEST_OBJECTIVES_COUNT
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
    QUEST_REWARD,  // [guild50] which choice reward to take at the turn-in (applied by the turn-in itself)
    // [guild80] the bots found their guild themselves (GuildFounding.cpp)
    FOUND_GUILD,   // the founder: buys the charter at a Guild Master, waits there for the signatures, turns it in
    SIGN_CHARTER,  // a signer: walks to the founder at the Guild Master and signs
    JOIN,          // the guild exists: an online member with the invite right invites it (no walk), it accepts
    BANK_TAB,      // the guild master buys the first bank tab with its own gold at a guild vault
    // [guild81] honest bot <-> bot transfers (GuildTransfer.cpp)
    TRADE_TO,     // trade an item / gold with a member next to it (core TRADE_DISTANCE)
    MAIL_TO,      // mail an item / gold at a mailbox (core postage, core delivery time); the receiver takes it
    DONATE_GOLD,  // free surplus gold to the guild master for the first bank tab (by trade or mail)
    SELL,         // [tasks] items the global leader gave up: sold at the next vendor visit (SellMarked)
    COOP,         // [coop] help a member with a quest in a co-op party (New RPG co-op runs it, never NextStep)
    REPAIR        // [regress10] repair its gear at a repairer (New RPG's vendor run runs it, StartVendorRun)
};

// [tasks] a bot's task list: every order is a task with its urgency, place, origin and creation time
enum class GuildTaskPriority : uint8
{
    URGENT,     // may interrupt questing (a five called, the guild founding, a profession that beats questing)
    HELP,       // [coop] a co-op party's quest: below the urgent tasks, above the town ones
    CONVENIENT  // a town task: waits until the bot is in that town anyway (turn-ins, passing through)
};

enum class GuildTaskPlace : uint8
{
    FIELD,
    TOWN,
    MAILBOX,
    BANK,
    VENDOR,
    TRAINER
};

enum class GuildTaskSource : uint8
{
    GLOBAL,  // the global leader
    FIVE,    // the five-leader / the five's call
    SYSTEM   // the core / a mechanic (delivered mail, the default quest task)
};

// [tasks] PROFESSION: what the global leader shifts the bot to
enum class GuildProfessionKind : uint8
{
    NONE,
    TRAIN,   // learn the profession / the next rank at its trainer
    GATHER,  // gather the reagents of its own skill-ups itself
    BUY,     // buy vendor reagents of its skill-ups
    CRAFT    // craft its skill-ups from the reagents it holds
};

struct GuildOrder
{
    GuildOrderType type{GuildOrderType::QUEST};
    GuildTaskPriority priority{GuildTaskPriority::URGENT};
    GuildTaskPlace place{GuildTaskPlace::FIELD};
    GuildTaskSource source{GuildTaskSource::GLOBAL};
    GuildProfessionKind profession{GuildProfessionKind::NONE};
    uint32 skillLine{0};  // PROFESSION
    uint32 townArea{0};   // a town task: the area of its spot (0: no fixed spot)
    uint32 townZone{0};
    bool townCapital{false};  // a capital's zone: any spot of the city counts as being there
    std::string townName;
    bool waitsTown{false};  // a convenient town task the bot is not in the town of: questing goes on
    uint32 itemId{0};
    uint32 count{0};
    uint32 forLow{0};        // the member it is for (0 = the guild bank)
    std::string details;
    std::string reason;
    std::string status;
    bool executable{false};  // false = dormant (no guild / no tab / stage 2): New RPG goes on with quests
    uint32 since{0};         // game time seconds of its creation: display only, never a decision input
    // [guild50] BUY: the vendor spawn and price; CRAFT_FOR: the recipe
    uint32 npcEntry{0};
    WorldPosition pos{};
    uint32 cost{0};
    uint32 extendedCost{0};  // [guild50] BUY with tokens / emblems / honor
    uint32 spellId{0};
    uint32 transferId{0};  // [guild81] TRADE_TO / MAIL_TO / DONATE_GOLD: the transfer (0: take the delivered mail)
    uint32 placeId{0};     // [gather] GATHER: the GuildGatherPlan place
    bool repairUrgent{false};  // [regress10] REPAIR: a weapon broken or at one death's loss, or a broken armour piece
};

// What New RPG does for the order (map thread).
struct GuildRunStep
{
    enum Kind : uint8
    {
        NONE,    // no executable order: quests
        GATHER,  // DO_GATHER in this zone
        WALK,    // GO_CAMP to pos (the guild vault)
        BUSY,    // the order acted this tick (cast started)
        GRIND    // [gather] GO_GRIND to pos (a skinning place: its mobs are killed and skinned)
    };
    Kind kind{NONE};
    WorldPosition pos{};
    std::string reason;
    std::vector<uint32> entries;  // [econ1] GRIND: the creature entries of the skinning place
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
    bool poolGoesIn{false};  // [supply100] its pooled supplies carried it: no member's restock holds it
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

// [lock23] the global leader's lock_ with its holder known on this thread. A core call made under it (a cast, an item
// moved) can fire a player hook into the leader on the same thread (b366: DecideLoot -> ItemUsage -> IsHeld threw
// resource_deadlock_would_occur on the std::mutex): the hooks defer themselves while it is held (Held), and a
// method that takes it again on its own thread logs that and runs under the hold it already has (no second lock)
class CoordLock
{
public:
    CoordLock(std::mutex& m, char const* where);
    ~CoordLock();
    CoordLock(CoordLock const&) = delete;
    CoordLock& operator=(CoordLock const&) = delete;
    static bool Held();

private:
    std::mutex* m_{nullptr};  // null: the thread held it already
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
    void SendPlan(Player* player);       // [guild70] addon "BOTCTL\tplan": crafts, transfers, sources
    // [ui11] addon "BOTCTL\ttasks <num>": one member's whole task list with every field, its gear plan (now / band /
    // future) and why it stands still - asked only for the bot the BotControl detail pane shows
    void SendTasks(Player* player, uint32 num);
    bool IsPlanned(uint32 low) const { return planIndex_.count(low) != 0; }
    // [help] every member of the plan (read-only after Load: any thread)
    std::vector<uint32> PlannedLows() const
    {
        std::vector<uint32> lows;
        lows.reserve(plan_.size());
        for (PlanRow const& row : plan_)
            lows.push_back(row.low);
        return lows;
    }
    // [human] user rule 2026-11-08: the leader never acts with the items or inventory of a human's character - one
    // the human controls (HumanControls) or one with a real client session (a SelfBot the human plays with); it only
    // suggests in chat and lets the human act (Suggest)
    static bool HumanCharacter(Player* p);
    // [smelt] the smelting recipe the miner knows that still gives it Mining skill (not gray, below its rank's cap),
    // the cheapest first; 0: none. Its reagents and product (the bar)
    static uint32 NextSmelt(Player* p, std::vector<std::pair<uint32, uint32>>* reagents = nullptr,
                            uint32* product = nullptr);

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
    // [next1] the five the global leader would take into `contentKey` now (members in its band, tank / healer / dps as
    // PickFive picks them, the tank first) and its five-gate verdict - the harness's "guildlike" comp copies it
    bool PreviewFive(std::string const& contentKey, std::vector<std::pair<uint32, std::string>>& members,
                     std::string& verdict, std::string& error);
    bool AutoFives() const { return autoFives_; }
    // [wait19] both are members of one five the global leader formed that gathers at its dungeon's entrance or
    // heads into the dungeon (Gather / Run): its leader's group wait and meet walk hold for it
    bool InSameActiveFive(uint32 a, uint32 b) const;
    // [roles59] the role mix the global leader planned (PlanRoles): the premade spec (playerbots.conf
    // PremadeSpecName index) + 1 a planned member plays in talent spec `activeSpec` (0: its role spec; 1: its solo
    // spec once it bought dual spec), 0 = no plan (the factory's own pick). Any thread.
    uint32 RoleSpec(uint32 low, uint8 activeSpec) const;
    // [roles59] the member's role from the role plan ("tank" / "healer" / "dps"), "" = none planned. Any thread.
    std::string RoleOf(uint32 low) const;

    // the dungeon leader's end of the five's run (TestHarnessMgr hook, world thread)
    void OnFiveRunEnd(uint32 fiveId, GuildFiveReport const& report);
    // map thread: the five-leader's word on a group roll of a BoP item (false: not a five's roll, the bot decides)
    bool FiveRollVote(Player* bot, uint64 lootGuid, uint32 itemId, int32 randomProperty, uint8& vote);

    // map thread
    // [guild70] the item is on the wanted list with a shortfall (a skill-up craft whose product someone needs)
    bool IsWanted(uint32 itemId) const;
    // [guild70] a crafter's skill-up reagent lies in the guild bank: it takes it out on the next BANK_WITHDRAW run
    void RequestWithdraw(Player* bot, uint32 itemId, std::string const& why);
    void OnCrafted(Player* bot, uint32 spellId, uint32 before, uint32 after);  // [guild70] the «План» tab's done
    GuildRunStep NextStep(Player* bot, PlayerbotAI* botAI);
    uint8 OnCampReached(Player* bot);  // 0 = not a guild run, 1 = done (-> IDLE), 2 = still at it
    bool IsHeld(Player* bot, Item* item);
    void SellMarked(PlayerbotAI* botAI);
    // [vendor20] the global leader marked items of the bot for sale (its bag relief, unwanted gear): a vendor visit
    // has something to sell whatever its own sale keeps
    bool HasMarkedSale(Player* bot) const;
    // [supply27] map thread: the bot's five waits for its supplies (New RPG's vendor run restocks below the line)
    bool SupplyWanted(Player* bot) const;
    void SetOrderStatus(Player* bot, std::string const& status);
    void OnLoot(Player* player, Item* item, uint32 count);
    // [lock23] the loots a hook got while this thread held lock_: taken at the next Update, out of the lock
    void DrainDeferredLoot();
    void OnKilledByCreature(Player* killed, Creature* killer);
    void OnJustDied(Player* killed);
    void OnRpgStatus(Player* bot, uint8 status);
    void SetWait(Player* bot, std::string const& reason);  // a legitimate wait the bot sets itself ("" clears)
    void AddAiTime(Player* bot, uint64 microseconds);
    // [guild50] the choice reward the global leader picked for this quest (-1 = none: the bot picks itself)
    int32 RewardChoice(Player* bot, Quest const* quest);
    // [tasks] map thread: the PROFESSION task the global leader gave the bot (NONE: questing); the profession
    // executors (NewRpgProfession::StartTask, NewRpgCraft::StartCraft) should run only for a kind returned here
    // [econ2] ... and only once the call can run where the bot stands: TRAIN in the town of the trainer the leader
    // planned for it (its next visit there), CRAFT / BUY in a town (rest area); NONE while it only waits
    GuildProfessionKind ProfessionCall(Player* bot, uint32* skillLine = nullptr);
    // [econ2] map thread, for the profession executors' errands (NewRpgProfession train / tool / recipe trips): the
    // global leader called this errand for this skill line (TRAIN, a tool = TRAIN "tool"), the npc stands in the town
    // the bot is in now, and a bought tool has a free bag slot. The executors pick only errands this allows
    // [econ2] ... also a tool buy the leader called (TRAIN with the tool's item: Mining Pick, Skinning Knife, Fishing
    // Pole, a called crafting line's tool) with bag room, and a recipe trip on a line it is called to (TRAIN or
    // CRAFT); each only with the npc in the town the bot is in now
    bool ProfessionErrand(Player* bot, uint32 skillLine, bool tool, WorldPosition const& npc);
    // [econ2] map thread: the item is a reagent of the bot's own called skill-ups (UpdateWanted, the lines the leader
    // calls it to) or an objective item of a quest in its log: the vendor sale keeps it
    bool NeedsForOwnCalls(Player* bot, uint32 itemId) const;
    // [quest14] user rule 2026-11-08: a grey quest is not done unless it is needed. Grey = the core's: the quest
    // level (Player::GetQuestLevel) at or below the bot's grey level (Acore::XP::GetGrayLevel), the level the core
    // gives reduced or no quest XP at
    static bool IsGreyQuest(Player* bot, Quest const* quest);
    // [quest14] map thread, never with lock_ held: why the grey quest is needed ("" = not needed) -
    // (a) a step of a chain (PrevQuestId / NextQuestId / RewardNextQuest / ExclusiveGroup / breadcrumb) to a quest the
    //     bot would do that is not grey, or that is needed by (b) / (c);
    // (b) a reward that is an upgrade for the bot (GuildGear::Evaluate) or in a member's gear plan (the leader's);
    // (c) a reward recipe of the bot's own profession it does not know, a profession rank or recipe spell it learns,
    //     an attunement (dungeon_access_requirements quest or item) or a key (a key item, a Lock.dbc item key) it
    //     lacks, an item on the leader's wanted list (IsWanted) or one of its own calls (NeedsForOwnCalls)
    std::string GreyQuestNeed(Player* bot, Quest const* quest) const;
    // [quest14] "[quest14] <bot> skips|abandons|keeps grey q<id> <name>: <reason>", once per bot, quest and decision
    static void LogGrey(Player* bot, char const* decision, Quest const* quest, std::string const& reason);
    // [coop] map thread, read only: the bot runs or must start an URGENT task now (its first executable urgent order
    // not waiting for a town, its five, an urgent vault run); why: the task's name
    bool UrgentTask(Player* bot, std::string* why) const;
    // [questroute] map thread, read only: the bot's convenient town tasks that wait for their town (executable, a
    // spot on its map) - the spot and the task's name; the quest route visits them on its way
    void TownStops(Player* bot, std::vector<std::pair<WorldPosition, std::string>>& out) const;
    // [questroute] map thread, read only (the table is built at startup): a quest's work by the formula for `bot` at
    // `dps` - the kill seconds of its units left (all of them for a quest not taken: health of the cheapest source,
    // an item's over its drop chance), the yards of its objective-spawn tour still to walk, its area and ender;
    // false: no row, or kills it cannot price
    struct QuestWork
    {
        uint16 map{0};
        float killSeconds{0.0f}, tourYd{0.0f};
        float areaX{0.0f}, areaY{0.0f};
        bool ender{false};
        float enderX{0.0f}, enderY{0.0f};
    };
    bool QuestWorkOf(Player* bot, uint32 questId, float dps, QuestWork& out) const;
    // [equip] the best gain the leader's gear plan has reachable now for that gear group of the member (0: none);
    // GuildGear::ShouldBuy keeps a bot from buying a lesser vendor item meanwhile (takes lock_)
    float PlannedGain(uint32 low, uint8 group) const;
    // [regress10] map thread: the global leader's REPAIR call for the bot (its own small lock, never lock_): need =
    // the task exists; ready = a repairer on its map and the money for `cost`; inTown = in the repairer's town at the
    // leader's last look; weaponBroken = its main weapon (a hunter's ranged one too) is broken: no new pulls while the
    // repair is ready
    struct RepairCall
    {
        bool need{false};
        bool ready{false};
        bool urgent{false};
        bool inTown{false};
        bool weaponBroken{false};
        uint32 cost{0};
        WorldPosition at{};  // the nearest repairer spawn
    };
    RepairCall Repair(Player* bot) const;
    // [coop] world / map thread: the co-op task enters / leaves the bot's task list ([tasks] add / done, BotControl)
    void AddCoopTask(uint32 low, uint32 partnerLow, uint32 questId, bool leads, std::string const& why);
    void EndCoopTask(uint32 low, uint32 questId, std::string const& why);

    struct PlanRow
    {
        uint32 low{0};
        uint32 num{0};
        std::string name;
        char grp{'P'};
        std::string role;
        std::vector<uint32> profs;  // planned profession skill lines
        std::vector<uint32> secondary;  // [guild70] planned secondary skills (Cooking, First Aid)
    };

    struct WantedItem
    {
        uint32 itemId{0};
        uint32 need{0};
        uint32 have{0};
        uint32 gatherSkill{0};     // SKILL_MINING / SKILL_HERBALISM when nodes give it
        uint32 gatherReq{0};
        bool vendor{false};
        bool optional{false};      // [guild70] OPTIONAL: banked when held, never ordered; else REQUIRED
        std::string forWhom;       // [guild70] why: "реагент: Rough Copper Bomb ×N (инженеры 6×≈200) + квест q12 #3"
        // [gather] copper one unit is worth to the guild: the skill points, upgrade / bag crafts and quests it is
        // wanted for, shared over their materials (the gathering plan's comparison with questing)
        float unitValue{0.0f};
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
        uint32 product{0};  // [guild70] the created item
        float made{1.0f};
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
        float score{0.0f};    // [ui11] its weight and the worn one's (0 = empty slot): the gain text
        float current{0.0f};
        uint32 needLevel{0};  // [ui11] the level it is wearable / reachable from: the item's, else its source's
        uint8 kind{0};        // GuildGear::SourceKind
        uint32 srcId{0};      // quest / boss / recipe spell / vendor entry
        uint32 aux{0};        // quest choice index / skill line / price
        uint32 mapId{0};
        float chance{0.0f};
        uint32 crafterLow{0};
        WorldPosition pos{};  // vendor spawn
        uint32 extendedCost{0};  // vendor: ItemExtendedCost (tokens, emblems, honor), 0 = gold
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
        // [supply27] the five's supply check: what it lacks before the five goes in ("" = supplied), its vendor run
        // asked (the map thread yields its five step), seen started, and done (supplied or no run could be made)
        std::string supplyShort;
        bool supplyAsked = false;
        bool supplyYielded = false;
        bool supplyStarted = false;
        bool supplyDone = false;
        uint32 supplyMoneyAt = 0;  // [supply79] its money when its supply run ended (a gold arrival shows against it)
        // [guild80] map thread: no safe walk to the Guild Master / vault at this level (planning skips it until the
        // next level)
        std::string foundRefused;
        uint8 foundRefusedLevel{0};
        // health
        uint32 mapId{0};
        float ax{0}, ay{0}, az{0};
        uint32 anchorAt{0};
        bool stuckAlerted{false};
        std::string stuckKey;
        std::string waitReason;
        uint32 skillSum{0};         // [guild15] its profession skills at the last health sample (crafting = progress)
        uint32 casts{0};            // [econ2] its smelt casts (NextStep): a cast is progress, a skill-up or not
        uint32 castsSeen{0};        // [econ2] casts at the last health sample
        std::string profGate;       // [econ2] the last logged state of its profession call (ready / waits)
        std::deque<uint32> deaths;  // game time
        bool deathOpen{false};      // dead since the last DEATH alert
        uint64 xpTotal{0};
        uint32 questsDone{0};
        uint32 money{0};
        bool seen{false};
        // xp, progress, pos, map, [econ2] work (money, profession skills, smelt casts)
        std::map<uint8, std::tuple<uint64, uint32, float, float, float, uint32, uint64>> statusEntry;
        std::unordered_set<uint8> loopAlerted;
        // [guild50] gear through the global leader
        std::vector<GearPick> gearNow;   // per slot group: the best upgrade reachable now (bank, reward, craft, buy)
        std::vector<GearPick> gearPlan;  // per slot group: the best reachable upgrade of any source
        // [ui11] per slot group: the best upgrade beyond the plan's level band (needLevel above its level and the
        // levels of the quests it holds): BotControl's «будущее» section only, no decision reads it
        std::vector<GearPick> gearFuture;
        std::string stillWhy;  // [ui11] why it stands still (the wait reason, or the STUCK cause), "" = it moves
        std::map<uint32, RewardPick> rewards;  // quest -> the choice to take
        std::vector<Expect> expect;      // items about to appear in its bags that belong to another member
        uint32 gearAt{0};
        // [tasks] the task list's log and metrics
        std::set<std::string> ranTasks;  // keys of tasks the bot started (an ended one is "done", else "dropped")
        uint32 visitTown{0};             // the town of the town tasks it runs on this visit
        uint32 visitTasks{0};
        float copperPerXp{0.0f};  // the core's quest XP -> money at max level over its quest log (last seen)
        // [questrate] the quests of its map it may take now (Player::CanTakeQuest over the static quest table), kept
        // while its level, map, rewarded and logged quest counts stay the same (questAvailKey)
        uint64 questAvailKey{0};
        std::vector<uint32> questAvail;
        uint8 rpgStatus{0};       // the last New RPG status (gathering time)
        uint32 rpgStatusAt{0};
        std::map<uint32, std::string> profDecision;  // skill line -> the last [guild] decision (logged on change)
        // [gather] the last walk NextStep issued to its gathering place: the place and how far it was (yards on the
        // same map, else route seconds); a new walk from no nearer means the last one gave the place up
        uint32 gatherWalkPlace{0};
        float gatherWalkLeft{0.0f};
        // [regress10] REPAIR: the copper its money lacks for the repair (the leader's gold transfer), the items in need
        // as last logged ([gear] durability line on change)
        uint32 repairShort{0};
        std::string durabilityKey;
        // [gold41] its essential supplies it cannot pay (out of food, of drink using mana, a hunter low on ammo): the
        // copper its money lacks for a stack of each of its level, and what for
        uint32 supplyGoldShort{0};
        std::string supplyWhat;
        // [human] the suggestions sent to a human member: key -> text (sent again only when the text changes)
        std::map<std::string, std::string> suggested;
        // [tasks] the bag pressure last logged (free slots class: 0 full, 1 low, 2 ok; -1 not logged yet)
        int8 bagClass{-1};
        std::set<uint32> buyDropped;  // [tasks] vendor upgrades not planned (logged once per item)
        uint64 revalueKey{0};  // [questvalue] level | login time of the last re-valuation of its log
    };

    // [tasks] the global leader's profession call for one member (UpdateProfessions, lock_)
    struct ProfessionPick
    {
        GuildProfessionKind kind{GuildProfessionKind::NONE};
        uint32 skillLine{0};
        uint32 itemId{0};  // GATHER / BUY: the reagent
        uint32 count{0};
        WorldPosition pos{};  // [econ1] CRAFT smelt: the town forge it smelts at on its next visit there;
                              // [econ2] TRAIN: the trainer (the pole vendor) it learns at on its next visit there
        std::string details;
        std::string reason;
    };
    struct TrainerRank
    {
        uint32 reqLevel{0};
        uint32 reqSkill{0};
        uint32 maxSkill{0};
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
        // [five58] the instance's own entry level (dungeon_access_template): below the finder's band a five walks in
        uint32 walkMinLevel{0};
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
        bool upgrade{false};  // [need106] a needed upgrade in the dungeon's loot
        bool enchanter{false};
        uint32 repeats{0};  // [need106] its runs of the dungeon so far (a carrier with no need: the fewest first)
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
    // "" = the dungeon fits this character. [next1] walk: the five walks in (the Dungeon Finder does not take it),
    // so the entrance's continent and an enemy capital count; else only while the finder cannot port the five.
    std::string DungeonRefusal(DungeonOption const& d, Player* p, bool walk = false) const;
    std::string FiveWalkRefusal(GuildFive const& five) const;  // [next1] a member the walk in refuses, "" = none
    // [need106] the first boss item of the dungeon the member needs (its own spec's upgrade, not owned): its name,
    // "" = none
    std::string NeededUpgrade(DungeonOption const& d, Player* p) const;
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

    // [guild80] the guild founded by the bots (GuildFounding.cpp). World thread: LoadGuildMasters (startup),
    // UpdateFounding (the plan, then the game actions: charter purchase, offer + sign, turn-in, invites, bank tab -
    // the core's own opcode handlers, on the world thread where the core runs them). lock_ held: FoundingOrders (one
    // member's orders), FoundStep / FoundCampReached / FoundWait (map thread: the walk to the Guild Master / vault).
    struct GuildMasterSpot
    {
        uint32 entry{0};
        std::string name;
        WorldPosition pos{};
        uint32 zoneId{0};
        uint32 faction{0};  // creature_template faction (hostility per member)
    };
    void LoadGuildMasters();
    void UpdateFounding();
    void FoundingOrders(BotState const& st, uint32 low, Player* p, std::vector<GuildOrder>& orders) const;
    bool FoundStep(Player* bot, BotState& st, GuildOrder& order, GuildRunStep& step);
    uint8 FoundCampReached(Player* bot, BotState& st);
    std::string FoundWait(uint32 low, BotState const& st) const;
    bool FoundingEligible(uint32 low, Player* p) const;                 // lock_ held
    float FoundingCost(Player* p, GuildMasterSpot const& spot) const;  // FLT_MAX: other map / hostile
    static uint32 FoundingFreeMoney(Player* p);
    void FiveLoot(Player* player, Item* item, uint32 count);

    // [guild81] honest transfers between members (GuildTransfer.cpp): trade next to each other, mail at a mailbox
    // (core postage / delivery time), or the guild bank (the existing BANK_DEPOSIT / BANK_WITHDRAW flow), picked by
    // the walk seconds each channel costs both members; gold donations for the first bank tab. World thread:
    // LoadMailboxes (startup), UpdateTransfers (the plan, takes lock_), RunTransfers (the core's trade / mail
    // handlers). lock_ held: TransferOrders, TransferStep / TransferCampReached / TransferWait (map thread),
    // TransferHolds, TransferPlanRows.
    struct Transfer
    {
        enum Channel : uint8
        {
            TRADE,
            MAIL,
            BANK
        };
        uint32 id{0};
        std::string key;   // what it moves: "i<item guid>" held item, "r<to>:<item>" reagent, "c<to>:<kind>"
                           // consumable, "g<from>" donation
        bool gold{false};  // DONATE_GOLD (money), else an item stack
        Channel channel{TRADE};
        uint32 fromLow{0};
        uint32 toLow{0};
        uint32 itemGuidLow{0};
        uint32 itemId{0};
        uint32 count{0};      // what is handed over (a consumable share is split off the stack first)
        uint32 money{0};      // copper (gold)
        std::string purpose;  // upgrade / reagent / product / consumable / donation (Russian, for the tabs)
        std::string why;
        std::string costs;     // the channel costs it was chosen by
        uint32 walkerLow{0};   // trade: who walks to the other (0: nobody, traded when they meet)
        WorldPosition meet{};  // trade: a fixed meeting point (the other's own walk target), else the other's position
        WorldPosition mailbox{};  // mail: the sender's mailbox
        bool tradeRefused{false};  // no safe walk for that channel (map thread)
        bool mailRefused{false};
        uint32 refusedLevels{0};  // the members' levels (from << 8 | to) at the refusal: reset on a level-up
        bool stalled{false};    // no channel possible now: kept (its refusals too), traded if they meet
        // [human] the trade was refused while the receiver held this many free bag slots: no new try until that count
        // changes (a fact, no timer); -1: no such refusal
        int32 refusedFree{-1};
        bool mailed{false};     // sent: in the receiver's mailbox until it takes it
        uint32 deliverAt{0};    // the core's delivery time of the mail (game seconds)
        std::string failed;     // the last refusal of the core handler (logged once)
        uint32 since{0};
    };
    struct Mailbox
    {
        WorldPosition pos{};
        uint32 areaTeam{0};
        uint32 areaId{0};  // [guild15] its town (the area it stands in): mailed on a visit there
        uint32 zoneId{0};
        bool capital{false};  // [guild15] in a capital city's zone (AreaTable AREA_FLAG_CAPITAL)
    };
    void LoadMailboxes();
    void UpdateTransfers();
    void RunTransfers(uint32 diff);
    void TransferOrders(uint32 low, Player* p, std::vector<GuildOrder>& orders) const;
    bool TransferStep(Player* bot, BotState& st, GuildOrder& order, GuildRunStep& step);
    uint8 TransferCampReached(Player* bot, BotState& st);
    std::string TransferWait(uint32 low, BotState const& st) const;
    bool TransferHolds(uint32 itemGuidLow) const;
    void TransferPlanRows(std::vector<std::vector<std::string>>& rows) const;
    std::vector<Mailbox> mailboxes_;        // read-only after Load
    std::vector<Mailbox> forges_;
    // [econ2] the profession trainers by skill line and the Fishing Pole vendors (creature spawns on the continents,
    // their town as a mailbox's): the TRAIN call's town, read-only after Load
    struct NpcSpot
    {
        uint32 entry{0};
        Mailbox spot;
    };
    std::unordered_map<uint32, std::vector<NpcSpot>> trainerSpots_;
    std::unordered_map<uint32, std::vector<NpcSpot>> toolVendors_;  // tool item -> its vendors (Fishing Pole, ...)
    // the nearest friendly spot of the list on the bot's map whose walk its trip judge did not refuse (lock_ held)
    NpcSpot const* NearestSpot(Player* p, uint32 low, std::vector<NpcSpot> const& list) const;
    // [econ2] member -> raw item -> its own skill-up need on the lines the global leader calls it to (UpdateWanted):
    // what full bags keep of a material (lock_)
    std::map<uint32, std::map<uint32, uint32>> crafterNeed_;
    // [econ1] UpdateWanted: member -> raw item -> what its own skill-ups to its held rank cap still lack (lock_)
    std::map<uint32, std::map<uint32, uint32>> crafterLack_;
    std::string wantedTop_;  // [econ1] the last logged top of the wanted list (world thread)           // [econ1]
                             // forges in a town (an area with a mailbox), read-only after Load
    std::map<uint32, Transfer> transfers_;  // id -> transfer (lock_)
    // [recipes] (member, recipe item guid): recipes their holder is the best placed to learn, used on the next
    // RunTransfers (lock_)
    std::vector<std::pair<uint32, uint32>> recipeLearn_;
    // receiver -> its delivered mails from members (count, the cheapest mailbox), what is still in transit (lock_)
    std::map<uint32, std::pair<uint32, WorldPosition>> mailPickup_;
    std::map<uint32, std::vector<std::string>> mailTransit_;
    // [econ1] member -> (level, town task spots (x, y)) its own trip judge refused or its walk gave up on
    // (NewRpgBaseAction::TripRefused, judged in TownStops on the map thread): no mailbox / forge there is planned
    // for it until its next level (lock_; mutable: TownStops is const)
    mutable std::map<uint32, std::pair<uint8, std::set<std::pair<int32, int32>>>> townRefused_;
    bool TownRefused(uint32 low, WorldPosition const& pos) const;  // lock_ held
    std::string CampText(uint32 requesterLow, uint32 helperLow) const;  // [econ1] "camp <area>, <zone>"
    std::string donationState_;  // the pooling for the first bank tab (lock_)
    // member -> (food count last seen, (game time, food that left its bags)) over the last hour: the SUPPLY need
    std::map<uint32, std::pair<uint32, std::deque<std::pair<uint32, uint32>>>> foodUse_;
    uint32 nextTransferId_{1};
    uint32 transferTimer_{0};

    void UpdateLeaders();
    void UpdateWanted();
    void UpdateOrders();
    // [tasks] the task list: urgency / place / origin / town of each task (world thread, lock_ held), whether a
    // convenient town task waits for the town (both threads), the add / end log
    void ClassifyTask(GuildOrder& o);
    // [tasks] a member whose gear plan (now or later) holds this item, 0: none (lock_ held): no disenchant / sale
    uint32 PlannedFor(uint32 itemId) const;
    static bool TownReady(Player* bot, GuildOrder const& o);
    void LogTaskChanges(uint32 low, BotState& st, std::vector<GuildOrder> const& now);
    std::string TasksText(BotState const& st) const;
    // [tasks] profession vs questing, decided by the global leader only (world thread)
    void UpdateProfessions();
    // [questrate] the member's questing value by formula (no measurements): per quest the core's quest XP at its
    // copper per XP + the money + the reward items' price over the time it takes now (the walk to the objective area
    // and the ender, or to the giver for a quest it may take; the kills by the source's health at its EstimateDps,
    // the drop chance). false: no quest it can price (no static row on its map); `why`: the numbers
    // [econ1] questing by XP per minute (xpPerMinute); copperPerMinute = that XP at the bot's quest money per XP
    bool QuestPace(BotState& st, Player* p, float& copperPerMinute, std::string* why = nullptr,
                   float* xpPerMinute = nullptr) const;
    // [questvalue] its green log quests valued against the route's rate; the ones not worth it abandoned ([quest14]
    // greys: the route's grey rule, GreyQuestNeed)
    void RevalueQuests(BotState& st, Player* p);
    float GatherPace(uint32 skill) const;                              // units per minute of gathering, < 0: no data
    uint32 TrainerCap(uint32 skillLine, uint8 level) const;            // the highest rank's max skill by level
    void RecordPace(BotState& st, Player* p) const;                    // UpdateHealth: its copper per quest XP
    void LoadQuestValue();  // [questrate] playerbots_quest_value_static + the objectives' kill cost, startup
    void UpdateGear();  // [guild50] one member's upgrade plan per call
    void PlanGear(Player* m, std::vector<GearPick>& now, std::vector<GearPick>& plan, std::vector<GearPick>& future,
                  std::map<uint32, RewardPick>& rewards, std::vector<Player*> const& bots,
                  std::map<uint32, uint32> const& bank, std::vector<WantedItem> const& wanted);
    void UpdateCrafts(uint32 now);  // [guild50] craft queue (upgrade crafts + bags), its wanted mats; lock_ held
    static bool HumanControls(Player* p);
    // [human] lock_ held: a suggestion to the human member (a system line from the global leader), sent once per key
    // while its text stays the same
    void Suggest(uint32 low, std::string const& key, std::string const& text);
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
    // [guild70] the expected crafts per recipe index a crafter makes from `skill` over the whole range where recipes
    // still give skill-ups (orange / yellow); crafter may be null (offline: trainer recipes only)
    // the skill-up crafts from `skill` up to `upTo` (exclusive; [gather] the skill its level can reach now)
    void SkillUpCrafts(Player* crafter, uint32 skillLine, uint32 skill, uint32 upTo,
                       std::map<size_t, double>& crafts, std::map<size_t, uint32>* points = nullptr) const;
    // [guild70] an intermediate (bar, bolt, potion used as reagent…) as its raw materials; `via` gets
    // raw -> intermediate -> raw per intermediate
    void ExpandReagent(uint32 item, double count, std::map<uint32, double>& out,
                       std::map<uint32, std::map<uint32, float>>* via, float perUnit = 1.0f, uint32 depth = 0,
                       uint32 top = 0) const;
    void BuildSkillPlans();
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
    // [guild70] crafted intermediates: product -> (reagent, count per product), read-only after Load
    std::unordered_map<uint32, std::vector<std::pair<uint32, float>>> craftedFrom_;
    std::unordered_set<uint32> skinned_;  // skinning loot (leather is gathered, not only crafted)
    // [guild70] per skill line and skill value: the best trainer / auto-learned skill-up recipe (index, -1 none)
    std::unordered_map<uint32, std::vector<int32>> skillPlan_;
    std::unordered_map<uint32, std::vector<size_t>> extraRecipes_;  // per line: recipes learned elsewhere (drops)
    std::unordered_map<uint64, uint32> lastSkill_;  // (low << 16 | line) -> skill when last seen (offline members)
    std::unordered_map<uint64, uint32> lastCap_;    // [gather] (low << 16 | line) -> the skill it could reach then
    // [gather] (low << 16 | line) -> copper one skill point of that crafter is worth (UpdateProfessions, lock_)
    std::unordered_map<uint64, float> pointValue_;
    // [questrate] the static part of a quest's value (quest_audit.py -> playerbots_quest_value_static) and the
    // health per objective unit of its cheapest source creature (kill credit, or the drop over its chance; 0: no
    // creature, an object or an event); read-only after Load
    struct QuestStatic
    {
        uint16 map{0};
        bool takeable{false};  // doable yes / class / profession, no event, a giver and an ender
        bool giver{false}, ender{false};
        float areaX{0}, areaY{0}, giverX{0}, giverY{0}, enderX{0}, enderY{0};
        float tourYd{0};   // the tour over the objective spawns
        float idealYd{0};  // giver -> tour -> ender
        float npcHp[QUEST_OBJECTIVES_COUNT]{};
        float itemHp[QUEST_ITEM_OBJECTIVES_COUNT]{};
    };
    std::unordered_map<uint32, QuestStatic> questStatic_;
    std::unordered_map<uint32, std::vector<uint32>> questsByMap_;  // map -> takeable quests, by min level

    // [roles59] the global leader's role plan (world thread writes under roleLock_, a leaf; any thread reads)
    struct RoleAssign
    {
        std::string role;   // tank / healer
        uint8 spec{0};      // premade spec index of the role
        uint8 solo{0};      // premade spec index of its solo (questing) spec, the second spec once bought
    };
    mutable std::mutex roleLock_;
    std::unordered_map<uint32, RoleAssign> roleAssign_;
    std::map<std::string, std::string> rolesWhy_;  // band -> its last logged plan line
    std::string fiveLook_;                         // [five58] the last logged look line
    std::unordered_set<uint32> shutdownOut_;       // [cleft62] members sent out of their dungeon for the shutdown
    // [lfg69] dungeon -> member -> the level it was refused at (its walk to the entrance, the finder): left out of
    // that dungeon's fives until its level changes (lock_ held)
    std::unordered_map<std::string, std::unordered_map<uint32, uint8>> refusedFor_;
    void PlanRoles();                              // world thread, lock_ held
    std::string SpecRoleNow(Player* p) const;      // tank / healer / dps by its talents now
    mutable std::mutex lock_;
    std::mutex deferredLock_;  // [lock23] a leaf: nothing is called under it
    std::vector<std::tuple<ObjectGuid, ObjectGuid, uint32>> deferredLoot_;  // player, item, count
    // [regress10] the REPAIR calls the map threads read (Repair), written by UpdateOrders
    mutable std::mutex repairLock_;
    std::unordered_map<uint32, RepairCall> repairCalls_;
    std::unordered_map<uint32, BotState> bots_;
    std::map<uint32, GuildFive> fives_;
    uint32 nextFiveId_{1};
    std::vector<DungeonOption> dungeons_;  // [guild60] read-only after Load
    std::vector<GuildRunRecord> runs_;     // [guild60] the completion table (all runs, oldest first)
    std::unordered_map<std::string, std::unordered_set<uint32>> cleared_;  // content -> characters that cleared it
    // content -> members that outlevelled it uncleared (logged once; the in-band clear rule, user 2026-11-08)
    std::unordered_map<std::string, std::unordered_set<uint32>> missed_;
    // [done106] the dungeons no member at or below their band needs (cleared or a needed upgrade): no five forms
    std::unordered_set<std::string> guildDone_;
    std::unordered_map<std::string, std::string> needLogged_;  // [need106] the last needs line per dungeon
    bool autoFives_{true};
    uint32 fiveTimer_{0};
    uint32 autoFiveTimer_{0};
    uint32 raidLeader_[3]{0, 0, 0};  // A, B, C (indexed by grp - 'A')
    uint32 gatherStockCalls_{0};  // [gather] UpdateOrders calls: the stock line every GATHER_STOCK_LOG_EVERY
    static constexpr uint32 GATHER_STOCK_LOG_EVERY = 10;
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
    // [guild80] the founding of the project guild (lock_)
    std::vector<GuildMasterSpot> guildMasters_;  // read-only after Load
    struct Founding
    {
        uint32 guildId{0};       // the project guild, 0 = not founded yet
        uint32 founderLow{0};
        int32 spot{-1};          // guildMasters_ index: the meeting at the Guild Master
        uint64 charterGuid{0};   // the charter item (0 = not bought yet)
        std::string charterName;
        uint32 signs{0};
        uint32 needSigns{0};     // core MinPetitionSigns
        float planCost{0.0f};
        // [guild15] signers the plan still lacks: while > 0 nobody walks or waits for the charter (b322: 9 signers and
        // the founder walked / stood ~1200 s each at Jondor Steelbrow for a charter short of 4 signers, 0/9 signed)
        uint32 shortBy{0};
        std::vector<uint32> signers;              // the members called to sign, cheapest first
        std::map<uint32, std::string> signerWhy;  // member -> why it was called
        std::map<uint32, std::string> joinState;  // member -> why it is not in the guild yet
        uint32 tabGmLow{0};      // BANK_TAB: the guild master walks to this vault
        WorldPosition vault{};
        std::string state;       // the «Приказы» header line
        std::string lastLog;     // the last state logged (a [guild80] state line only on change)
    } found_;
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
    // [guild70] reagent -> why ("улучшение: <item>", "сумка <item>") -> (count, members it is for)
    struct GearNeed
    {
        uint32 count{0};
        std::set<uint32> forLows;
        double value{0};  // [gather] copper: the crafted item's price (BuyPrice), shared over its reagent units
    };
    std::map<uint32, std::map<std::string, GearNeed>> gearWanted_;
    std::string bagState_;
    struct CraftDone  // [guild70]
    {
        uint32 at{0};
        uint32 low{0};
        uint32 spellId{0};
        uint32 before{0};
        uint32 after{0};
    };
    std::deque<CraftDone> craftsDone_;
    size_t gearCursor_{0};

    // [tasks] professions (lock_ except the read-only trainer ranks)
    std::unordered_map<uint32, std::vector<TrainerRank>> trainerRanks_;    // skill line -> ranks; read-only after Load
    std::unordered_map<uint32, std::vector<ProfessionPick>> professions_;  // member -> the leader's calls
    std::map<uint32, std::deque<std::pair<uint32, uint32>>> gatherUnits_;  // skill -> (game time, units gathered)
    std::map<uint32, std::deque<std::pair<uint32, uint32>>> gatherTime_;   // skill -> (game time, seconds gathering)
    std::unordered_map<uint64, uint32> townCache_;  // (map, x, y) -> area of a task spot (world thread)
    // [tasks] metrics: town tasks started, their wait since creation, town visits with town tasks
    uint64 townWaitSum_{0};
    uint32 townStarted_{0};
    uint32 townVisits_{0};

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
