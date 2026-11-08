/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDPROFPATH_H
#define PLAYERBOTS_GUILDPROFPATH_H

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Define.h"

class Player;

// [prof] The cheapest skill-up path of every crafting profession, for the global leader (GuildCoordinator), which
// stays the single decider: it hands in a member and its cost model, and gets back the path, the mats, the recipes
// to learn and where they come from. Nothing here orders anything.
//   - chance per craft = the core's CraftSkillGainChance (orange up to TrivialSkillLineRankLow, then linear down to
//     gray at TrivialSkillLineRankHigh; SkillChance.Orange / Grey, SkillGain.Crafting);
//   - the expected cost of one skill point at value v with recipe r = (reagents + cast time - product value, floored
//     at 0) x 1000 / chance; the per-point costs add up, so the cheapest recipe per value is the exact optimum over
//     expected values; a recipe the member must acquire (trainer fee, vendor price, a recipe held in the guild) joins
//     the set only when what it saves over the set exceeds its price;
//   - reagents are priced recursively: vendor price, gathered (the gatherers' time at the measured pace, else the
//     sell price the guild gives up), held in the guild (reserved: the sell price), crafted intermediates (bars,
//     bolts, leather, inks), milled pigments, prospected gems, disenchanted mats (only from items no member needs);
//   - trainer ranks cap the skill by level (trainer_spell: 75 / 150 / ... at their ReqLevel).
// The static index is built once at startup (world thread); paths are cached per (member, skill line) and recomputed
// only when the member's skill, known recipes, level or the cost model change.
enum class ProfSourceKind : uint8
{
    KNOWN,          // the member knows it
    AUTO,           // learned with the profession / skill value (SkillLineAbility AcquireMethod)
    TRAINER,        // a trainer teaches it: entry = trainer id, price = fee
    VENDOR,         // a gold vendor sells the recipe item: entry = vendor creature, price = item price
    VENDOR_LIMITED, // as VENDOR with a limited supply (npc_vendor maxcount / incrtime)
    VENDOR_TOKENS,  // a vendor sells it for an extended cost (tokens, badges)
    REPUTATION,     // the recipe item needs a reputation rank (faction / rank)
    DROP,           // a creature drops the recipe item: entry = creature, chance per kill
    WORLD_DROP,     // a shared loot reference drops it
    QUEST,          // a quest rewards the recipe item or teaches the spell: entry = quest
    DISCOVERY,      // learned by chance while crafting (skill_discovery_template)
    GUILD_HELD,     // the recipe item lies in the guild (bank / a member's bags): price = its sell price
    NONE
};

struct ProfRecipeSource
{
    ProfSourceKind kind{ProfSourceKind::NONE};
    uint32 entry{0};       // trainer id / vendor or creature entry / quest id / recipe item holder
    uint32 item{0};        // the recipe item (0: the spell itself)
    uint32 price{0};       // copper (trainer fee, vendor price); 0 for drops / quests
    float chance{0.0f};    // DROP: percent per kill
    uint32 faction{0};     // REPUTATION
    uint32 rank{0};        // REPUTATION: ReputationRank
    uint32 maxCount{0};    // VENDOR_LIMITED
    uint32 incrTime{0};    // VENDOR_LIMITED: seconds to restock
    uint8 reqLevel{0};     // TRAINER
    uint8 friendly{0};     // TRAINER / vendors: 1 Alliance, 2 Horde (bits)
};

struct ProfRecipe
{
    uint32 spellId{0};
    uint32 skillLine{0};
    uint32 reqSkill{0};    // trainer ReqSkillRank, recipe item RequiredSkillRank, else MinSkillLineRank
    uint32 yellow{0};      // TrivialSkillLineRankLow
    uint32 gray{0};        // TrivialSkillLineRankHigh
    uint32 product{0};     // created item, 0 = an enchant on an item (Enchanting) or no item
    float made{1.0f};      // expected items per cast
    uint32 enchantId{0};   // SPELL_EFFECT_ENCHANT_ITEM: SpellItemEnchantment id
    float castSeconds{0.0f};
    uint32 focus{0};       // SpellFocusObject (anvil, forge, fire)
    uint32 raceMask{0};    // SkillLineAbility RaceMask / ClassMask (0 = all)
    uint32 classMask{0};
    bool discovery{false}; // skill_discovery_template: learned by chance, never planned
    std::vector<uint32> totems;          // tool items
    std::vector<uint32> totemCategories; // tool categories (rods, hammer, pick)
    std::vector<std::pair<uint32, uint32>> reagents;
    std::vector<ProfRecipeSource> sources;  // every way to learn it, cheapest first
};

// The leader's cost model for one plan (copper): the value of the crafter's minute, the gather paces it measured,
// what the guild holds and what its members need.
struct ProfCostModel
{
    float copperPerMinute{0.0f};        // the crafter's minute (GuildCoordinator::QuestPace), 0 = time is free
    std::map<uint32, float> gatherPace;  // gather skill -> units per minute (GuildCoordinator::GatherPace); <= 0: none
    float gatherCopperPerMinute{0.0f};  // a gatherer's minute; 0 = the crafter's
    std::map<uint32, uint32> stock;      // item -> held by the guild for this crafter (bank + bags + spare), reservable
    std::map<uint32, std::pair<uint32, uint32>> needed;  // product -> (members' count, copper value per unit)
    std::unordered_set<uint32> spareItems;  // items no member needs (disenchant sources), item entries
};

// Who plans: a bot or a human player in the leader's scope (a human gets mats and gold, returns the leftovers).
struct ProfMember
{
    uint32 low{0};
    uint8 level{0};
    uint32 factionTemplate{0};     // Player::GetFaction() (vendor / trainer friendliness)
    uint32 raceMask{0};
    uint32 classMask{0};
    bool human{false};
    std::map<uint32, uint32> skill;     // skill line -> pure value (0 / missing: not learned)
    std::map<uint32, uint32> maxSkill;  // skill line -> current cap
    std::unordered_set<uint32> known;   // recipe spells known (live HasSpell or character_spell)
    std::map<uint32, uint32> reputation;  // faction -> ReputationRank, for the factions recipe items need
    std::unordered_set<uint32> toolItems;       // tool items held (Totem)
    std::unordered_set<uint32> toolCategories;  // tool categories held (TotemCategory)
    uint64 Fingerprint(uint32 skillLine) const;

    // the player's own thread (the leader on the world thread); `human`: GuildCoordinator::HumanControls
    static ProfMember FromPlayer(Player* player, bool human);
};

struct ProfStep
{
    uint32 spellId{0};
    uint32 fromSkill{0}, toSkill{0};
    double crafts{0.0};
    double copper{0.0};       // the expected net cost of the step
    double castSeconds{0.0};
    std::map<uint32, double> reagents;  // as cast (intermediates included)
};

struct ProfRankStep
{
    uint32 atSkill{0};   // the cap reached
    uint32 newCap{0};
    uint8 reqLevel{0};
    uint32 price{0};
};

struct ProfMissingRecipe
{
    uint32 spellId{0};
    ProfRecipeSource source;
    double saving{0.0};   // copper it saves over the recipes taken without it; < 0: not worth it (listed only)
    bool taken{false};    // in the path: a TRAIN / BUY / FARM task for the leader
    bool fillsGap{false}; // no other recipe gives skill there
};

struct ProfMatSource
{
    double count{0.0};
    double fromStock{0.0};   // reserved from what the guild holds
    double toBuy{0.0};       // vendor
    double toGather{0.0};    // nodes, skinning, fishing (GuildGatherPlan)
    double toDisenchant{0.0};  // spare items' disenchant mats
    double fromDrops{0.0};   // mob drops (cloth, ...): banked when looted, never ordered
    double unitCopper{0.0};
    uint32 gatherSkill{0};
    std::string why;
};

struct ProfPath
{
    uint32 low{0};
    uint32 skillLine{0};
    uint32 fromSkill{0}, targetSkill{0}, reached{0};
    std::string blocked;     // why `reached` < target: "level 20 for rank 225", "no recipe 425-450", ...
    std::vector<ProfStep> steps;
    std::vector<ProfRankStep> ranks;
    std::vector<ProfMissingRecipe> recipes;  // to learn on the way (taken) and rare ones that would help (not taken)
    std::map<uint32, ProfMatSource> mats;    // raw mats (intermediates expanded) with their sources
    std::map<uint32, double> intermediates;  // bars, bolts, inks made on the way (crafter or a member who can)
    std::map<uint32, double> tools;          // tool items the member lacks (hammer, rod, pick)
    std::map<uint32, double> leftovers;      // products left after the path (needed by members first, else sold)
    double copper{0.0};        // expected net mats + time
    double fees{0.0};          // trainer fees, recipe prices, rank fees
    double castSeconds{0.0};
    double crafts{0.0};
    uint64 fingerprint{0};
    uint32 computedMs{0};
};

// One planned craft of a gear item / consumable for a member (scope addition 1): who crafts it and what it takes
struct ProfCraftPlan
{
    uint32 product{0};
    uint32 spellId{0};
    uint32 crafterLow{0};
    double copper{0.0};        // mats + the crafter's skill-up path to the recipe + the recipe source
    std::vector<ProfRecipeSource> recipeSources;
    std::map<uint32, double> mats;
    uint32 skillNeeded{0};
};

// A reputation the plan needs (scope addition 2), with the reason
struct ProfReputationNeed
{
    uint32 faction{0};
    uint32 rank{0};  // ReputationRank
    uint32 forItem{0};
    uint32 forSpell{0};
    uint32 forLow{0};
    std::string reason;  // "Sons of Hodir Exalted -> shoulder inscription for #12"
};

class GuildProfPath
{
public:
    static GuildProfPath& instance()
    {
        static GuildProfPath instance;
        return instance;
    }

    // world thread, startup: after the item, spell, vendor, trainer and loot stores (and GuildGatherPlan::Load)
    void Load();
    bool Loaded() const { return loaded_; }

    // the path current -> target (0: the trainer cap reachable at the member's level, else 450). Cached: the same
    // member state and cost model return the cached path. World thread (the leader); thread-safe.
    ProfPath PlanPath(ProfMember const& member, uint32 skillLine, uint32 targetSkill, ProfCostModel const& model);
    // the cached path, if any (map threads: NewRpgCraft picks the step's recipe)
    bool CachedPath(uint32 low, uint32 skillLine, ProfPath& out) const;
    // the recipe the member should craft now for skill (the path's step at its current value), 0 = none
    uint32 NextRecipe(uint32 low, uint32 skillLine, uint32 skill) const;
    // drop a member's cached paths (skill-up, recipe learned, level-up, logout)
    void Invalidate(uint32 low, uint32 skillLine = 0);

    // raw mats every cached path still needs (fromStock excluded), summed over all crafters: GuildGatherPlan's need
    // input; `why` per item ("Blacksmithing #12 125-150 Bronze Bar x..., Engineering #7 ...")
    std::map<uint32, ProfMatSource> NeededMats() const;

    // the cheapest crafter of `product` for a member's gear or consumable (scope addition 1): members' paths to the
    // recipe's skill + the recipe source + mats; crafters = the members the leader allows
    bool PlanCraft(uint32 product, std::vector<ProfMember> const& crafters, ProfCostModel const& model,
                   ProfCraftPlan& out);
    // every recipe that makes the item / applies the enchant (scope additions 1, 2), with their sources
    std::vector<ProfRecipe const*> RecipesFor(uint32 product) const;
    std::vector<ProfRecipe const*> RecipesForEnchant(uint32 enchantId) const;
    // the reputations the chosen items / enchant recipes need (scope addition 2); `forLow` = the member it is for
    void ReputationNeeds(std::vector<std::pair<uint32, uint32>> const& itemsOrSpells, uint32 forLow,
                         std::vector<ProfReputationNeed>& out) const;

    ProfRecipe const* Recipe(uint32 spellId) const;
    std::vector<ProfRecipe> const* Recipes(uint32 skillLine) const;
    // the highest rank cap the member can train at its level, and the rank steps
    uint32 TrainerCap(uint32 skillLine, uint8 level) const;
    std::set<uint32> const& RepFactions() const { return repFactions_; }

    // the core's CraftSkillGainChance, per mille
    static int32 Chance(uint32 skillValue, uint32 gray, uint32 yellow);
    static std::string SourceText(ProfRecipeSource const& source);
    std::string PathText(ProfPath const& path, size_t maxSteps = 12) const;  // log / addon

private:
    GuildProfPath() = default;

    struct Rank
    {
        uint8 reqLevel{0};
        uint32 reqSkill{0};
        uint32 maxSkill{0};
        uint32 price{0};
    };
    struct VendorOffer
    {
        uint32 vendor{0};
        uint32 price{0};          // per unit
        uint32 maxCount{0};
        uint32 incrTime{0};
        uint32 extendedCost{0};
        uint8 friendly{0};        // FRIEND_ALLIANCE / FRIEND_HORDE bits
    };
    // how a unit of an item is had at plan time
    enum class UnitWay : uint8
    {
        NONE,
        STOCK,
        BUY,
        GATHER,
        CRAFT,
        CONVERT,
        DISENCHANT,
        DROP
    };
    struct UnitPrice
    {
        double copper{0.0};
        UnitWay way{UnitWay::NONE};
        ProfRecipe const* recipe{nullptr};  // CRAFT
        uint32 raw{0};                      // CONVERT
        float rawPerUnit{0.0f};
        uint32 gatherSkill{0};
    };
    struct PriceCtx
    {
        ProfMember const* member{nullptr};
        ProfCostModel const* model{nullptr};
        uint8 friendMask{0};
        std::unordered_map<uint32, UnitPrice> memo;
        std::unordered_map<uint32, double> stockShare;    // item -> share of the path's need the stock covers
        std::unordered_map<uint32, double> neededShare;   // product -> share of the made ones a member needs
    };
    struct CacheEntry
    {
        ProfPath path;
        uint64 key{0};
    };

    void LoadTrainers();
    void LoadVendors();
    void LoadRecipeItems();
    void LoadRecipes();
    void LoadDrops();
    void LoadDisenchant();
    void SortSources();

    uint8 FriendMask(uint32 factionTemplate) const;
    UnitPrice Price(PriceCtx& ctx, uint32 item, uint32 depth = 0) const;
    double CraftCopper(PriceCtx& ctx, ProfRecipe const& recipe, double& rawUnits) const;
    void Expand(PriceCtx& ctx, uint32 item, double count, std::map<uint32, double>& raw,
                std::map<uint32, double>* intermediates, uint32 depth = 0) const;
    // the cheapest way the member gets the recipe; false: none it can use now (price = copper)
    bool Acquire(ProfMember const& member, uint8 friendMask, ProfRecipe const& recipe, ProfCostModel const& model,
                 ProfRecipeSource& out) const;
    void Compute(ProfMember const& member, uint32 skillLine, uint32 target, ProfCostModel const& model,
                 ProfPath& path) const;
    static uint64 ModelKey(ProfCostModel const& model);

    std::unordered_map<uint32, std::vector<ProfRecipe>> bySkill_;            // skill line -> recipes
    std::unordered_map<uint32, std::pair<uint32, size_t>> bySpell_;          // spell -> (line, index)
    std::unordered_map<uint32, std::vector<uint32>> byProduct_;              // item -> recipe spells
    std::unordered_map<uint32, std::vector<uint32>> byEnchant_;              // enchant id -> recipe spells
    std::unordered_map<uint32, std::vector<Rank>> ranks_;                    // skill line -> ranks by cap
    std::unordered_map<uint32, std::vector<VendorOffer>> vendors_;           // item -> offers
    std::unordered_map<uint32, std::vector<ProfRecipeSource>> trainerOf_;    // spell -> trainer sources
    std::unordered_map<uint32, std::vector<uint32>> recipeItems_;            // spell -> recipe items
    std::unordered_map<uint32, std::pair<uint32, float>> bestDrop_;          // item -> (creature, chance)
    std::unordered_set<uint32> worldDrop_;                                   // items of shared references
    std::unordered_map<uint32, uint32> questReward_;                         // item -> quest
    std::unordered_map<uint32, uint32> questSpell_;                          // taught spell -> quest
    std::unordered_map<uint32, uint32> discovery_;                           // spell -> required skill
    std::unordered_map<uint32, double> deMatCopper_;  // mat -> median share of a spare item's sell price
    std::unordered_map<uint32, std::vector<uint32>> totemItems_;  // totem category -> items (cheapest first)
    std::set<uint32> repFactions_;  // factions recipe items need (ProfMember::FromPlayer reads only these)

    mutable std::mutex lock_;
    std::unordered_map<uint64, CacheEntry> cache_;  // (low << 16 | line) -> path
    bool loaded_{false};
};

#define sGuildProfPath GuildProfPath::instance()

#endif
