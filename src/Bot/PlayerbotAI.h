/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTAI_H
#define PLAYERBOTS_PLAYERBOTAI_H

#include "Chat.h"
#include "ChatFilter.h"
#include "ChatHelper.h"
#include "CreatureData.h"
#include "Event.h"
#include "ForceRebuff.h"
#include "Item.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "ObjectGuid.h"
#include "PlayerbotAIBase.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include "SpellAuras.h"
#include "Util.h"
#include "WorldPacket.h"
#include <limits>  // [deaths145]
#include <mutex>
#include <stack>
#include <unordered_set>

class Action;
class AiObjectContext;
class Creature;
class Engine;
class ExternalEventHelper;
class Group;
class ReactionEngine;
class Gameobject;
class Item;
class Player;
class PlayerbotMgr;
class Spell;
class SpellInfo;
class Unit;
class WorldObject;
class WorldPosition;

struct CreatureData;
struct GameObjectData;

enum StrategyType : uint32;

enum HealingItemId
{
    HEALTHSTONE = 5512,
    MAJOR_HEALING_POTION = 13446,
    WHIPPER_ROOT_TUBER = 11951,
    NIGHT_DRAGON_BREATH = 11952,
    LIMITED_INVULNERABILITY_POTION = 3387,
    GREATER_DREAMLESS_SLEEP_POTION = 22886,
    SUPERIOR_HEALING_POTION = 3928,
    CRYSTAL_RESTORE = 11564,
    DREAMLESS_SLEEP_POTION = 12190,
    GREATER_HEALING_POTION = 1710,
    HEALING_POTION = 929,
    LESSER_HEALING_POTION = 858,
    DISCOLORED_HEALING_POTION = 3391,
    MINOR_HEALING_POTION = 118,
    VOLATILE_HEALING_POTION = 28100,
    SUPER_HEALING_POTION = 22829,
    CRYSTAL_HEALING_POTION = 13462,
    FEL_REGENERATION_POTION = 28101,
    MAJOR_DREAMLESS_SLEEP_POTION = 20002
};

enum BotState
{
    BOT_STATE_COMBAT = 0,
    BOT_STATE_NON_COMBAT = 1,
    BOT_STATE_DEAD = 2,
    BOT_STATE_REACTION = 3,

    BOT_STATE_MAX
};

bool IsRealPlayer(Player* player);
bool IsSelfBot(Player* player);
// [self10] A person's game client is attached to this character's session (an open socket). True for a real player
// and for a SelfBot alike: leaving the world is that person's decision, never the bot manager's or the AI's.
bool HasHumanClientSession(Player* player);
bool IsAlliance(uint8 race);

class PlayerbotChatHandler : protected ChatHandler
{
public:
    explicit PlayerbotChatHandler(Player* pMasterPlayer);
    void sysmessage(std::string const str) { SendSysMessage(str.c_str()); }
    uint32 extractQuestId(std::string const str);
    uint32 extractSpellId(std::string const str)
    {
        char* source = (char*)str.c_str();
        return extractSpellIdFromLink(source);
    }
};

class MinValueCalculator
{
public:
    MinValueCalculator(float def = 0.0f) : param(nullptr), minValue(def) {}

    void probe(float value, void* p)
    {
        if (!param || minValue >= value)
        {
            minValue = value;
            param = p;
        }
    }

    void* param;
    float minValue;
};
enum ChatChannelSource
{
    SRC_GUILD,
    SRC_WORLD,
    SRC_GENERAL,
    SRC_TRADE,
    SRC_LOOKING_FOR_GROUP,
    SRC_LOCAL_DEFENSE,
    SRC_WORLD_DEFENSE,
    SRC_GUILD_RECRUITMENT,

    SRC_SAY,
    SRC_WHISPER,
    SRC_EMOTE,
    SRC_TEXT_EMOTE,
    SRC_YELL,

    SRC_PARTY,
    SRC_RAID,

    SRC_UNDEFINED
};
static std::map<ChatChannelSource, std::string> ChatChannelSourceStr = {
    {SRC_GUILD, "SRC_GUILD"},
    {SRC_WORLD, "SRC_WORLD"},
    {SRC_GENERAL, "SRC_GENERAL"},
    {SRC_TRADE, "SRC_TRADE"},
    {SRC_LOOKING_FOR_GROUP, "SRC_LOOKING_FOR_GROUP"},
    {SRC_LOCAL_DEFENSE, "SRC_LOCAL_DEFENSE"},
    {SRC_WORLD_DEFENSE, "SRC_WORLD_DEFENSE"},
    {SRC_GUILD_RECRUITMENT, "SRC_GUILD_RECRUITMENT"},

    {SRC_SAY, "SRC_SAY"},
    {SRC_WHISPER, "SRC_WHISPER"},
    {SRC_EMOTE, "SRC_EMOTE"},
    {SRC_TEXT_EMOTE, "SRC_TEXT_EMOTE"},
    {SRC_YELL, "SRC_YELL"},

    {SRC_PARTY, "SRC_PARTY"},
    {SRC_RAID, "SRC_RAID"},

    {SRC_UNDEFINED, "SRC_UNDEFINED"}};

enum ChatChannelId
{
    GENERAL = 1,
    TRADE = 2,
    LOCAL_DEFENSE = 22,
    WORLD_DEFENSE = 23,
    LOOKING_FOR_GROUP = 26,
    GUILD_RECRUITMENT = 25,
};

enum RoguePoisonId
{
    INSTANT_POISON      = 6947,
    INSTANT_POISON_II   = 6949,
    INSTANT_POISON_III  = 6950,
    INSTANT_POISON_IV   = 8926,
    INSTANT_POISON_V    = 8927,
    INSTANT_POISON_VI   = 8928,
    INSTANT_POISON_VII  = 21927,
    INSTANT_POISON_VIII = 43230,
    INSTANT_POISON_IX   = 43231,
    DEADLY_POISON       = 2892,
    DEADLY_POISON_II    = 2893,
    DEADLY_POISON_III   = 8984,
    DEADLY_POISON_IV    = 8985,
    DEADLY_POISON_V     = 20844,
    DEADLY_POISON_VI    = 22053,
    DEADLY_POISON_VII   = 22054,
    DEADLY_POISON_VIII  = 43232,
    DEADLY_POISON_IX    = 43233
};

enum SharpeningStoneId
{
    ROUGH_SHARPENING_STONE      = 2862,
    COARSE_SHARPENING_STONE     = 2863,
    HEAVY_SHARPENING_STONE      = 2871,
    SOLID_SHARPENING_STONE      = 7964,
    DENSE_SHARPENING_STONE      = 12404,
    ELEMENTAL_SHARPENING_STONE  = 18262,
    FEL_SHARPENING_STONE        = 23528,
    ADAMANTITE_SHARPENING_STONE = 23529
};

enum WeightstoneId
{
    ROUGH_WEIGHTSTONE      = 3239,
    COARSE_WEIGHTSTONE     = 3240,
    HEAVY_WEIGHTSTONE      = 3241,
    SOLID_WEIGHTSTONE      = 7965,
    DENSE_WEIGHTSTONE      = 12643,
    FEL_WEIGHTSTONE        = 28420,
    ADAMANTITE_WEIGHTSTONE = 28421
};

enum WizardOilId
{
    MINOR_WIZARD_OIL      = 20744,
    LESSER_WIZARD_OIL     = 20746,
    WIZARD_OIL            = 20750,
    BRILLIANT_WIZARD_OIL  = 20749,
    SUPERIOR_WIZARD_OIL   = 22522
};

enum ManaOilId
{
    MINOR_MANA_OIL        = 20745,
    LESSER_MANA_OIL       = 20747,
    BRILLIANT_MANA_OIL    = 20748,
    SUPERIOR_MANA_OIL     = 22521
};

enum class BotTypeNumber : uint8
{
    ACTIVITY_TYPE_NUMBER = 1,
    GROUPER_TYPE_NUMBER = 2,
    GUILDER_TYPE_NUMBER = 3,
};

enum class GrouperType : uint8
{
    SOLO = 0,
    MEMBER = 1,
    LEADER_2 = 2,
    LEADER_3 = 3,
    LEADER_4 = 4,
    LEADER_5 = 5
};

enum class GuilderType : uint8
{
    SOLO = 0,
    TINY = 30,
    SMALL = 50,
    MEDIUM = 70,
    LARGE = 120,
    VERY_LARGE = 250
};

enum ActivityType
{
    GRIND_ACTIVITY = 1,
    RPG_ACTIVITY = 2,
    TRAVEL_ACTIVITY = 3,
    OUT_OF_PARTY_ACTIVITY = 4,
    PACKET_ACTIVITY = 5,
    DETAILED_MOVE_ACTIVITY = 6,
    PARTY_ACTIVITY = 7,
    REACT_ACTIVITY = 8,
    ALL_ACTIVITY = 9,

    MAX_ACTIVITY_TYPE
};

enum BotRoles : uint8
{
    BOT_ROLE_NONE = 0x00,
    BOT_ROLE_TANK = 0x01,
    BOT_ROLE_HEALER = 0x02,
    BOT_ROLE_DPS = 0x04
};

enum HUNTER_TABS
{
    HUNTER_TAB_BEAST_MASTERY,
    HUNTER_TAB_MARKSMANSHIP,
    HUNTER_TAB_SURVIVAL,
};

enum ROGUE_TABS
{
    ROGUE_TAB_ASSASSINATION,
    ROGUE_TAB_COMBAT,
    ROGUE_TAB_SUBTLETY,
};

enum PRIEST_TABS
{
    PRIEST_TAB_DISCIPLINE,
    PRIEST_TAB_HOLY,
    PRIEST_TAB_SHADOW,
};

enum DEATH_KNIGHT_TABS
{
    DEATH_KNIGHT_TAB_BLOOD,
    DEATH_KNIGHT_TAB_FROST,
    DEATH_KNIGHT_TAB_UNHOLY,
};

enum DRUID_TABS
{
    DRUID_TAB_BALANCE,
    DRUID_TAB_FERAL,
    DRUID_TAB_RESTORATION,
};

enum MAGE_TABS
{
    MAGE_TAB_ARCANE,
    MAGE_TAB_FIRE,
    MAGE_TAB_FROST,
};

enum SHAMAN_TABS
{
    SHAMAN_TAB_ELEMENTAL,
    SHAMAN_TAB_ENHANCEMENT,
    SHAMAN_TAB_RESTORATION,
};

enum PALADIN_TABS
{
    PALADIN_TAB_HOLY,
    PALADIN_TAB_PROTECTION,
    PALADIN_TAB_RETRIBUTION,
};

enum WARLOCK_TABS
{
    WARLOCK_TAB_AFFLICTION,
    WARLOCK_TAB_DEMONOLOGY,
    WARLOCK_TAB_DESTRUCTION,
};

enum WARRIOR_TABS
{
    WARRIOR_TAB_ARMS,
    WARRIOR_TAB_FURY,
    WARRIOR_TAB_PROTECTION,
};

class PacketHandlingHelper
{
public:
    void AddHandler(uint16 opcode, std::string const handler);
    void Handle(ExternalEventHelper& helper);
    void AddPacket(WorldPacket const& packet);

private:
    std::map<uint16, std::string> handlers;
    std::stack<WorldPacket> queue;
};

class ChatCommandHolder
{
public:
    ChatCommandHolder(std::string const command, ObjectGuid owner, PlayerbotSecurityLevel requiredLevel,
                      uint32 type = CHAT_MSG_WHISPER, time_t time = 0)
        : command(command), owner(owner), requiredLevel(requiredLevel), type(type), time(time)
    {
    }
    ChatCommandHolder(ChatCommandHolder const& other)
        : command(other.command),
          owner(other.owner),
          requiredLevel(other.requiredLevel),
          type(other.type),
          time(other.time)
    {
    }

    std::string const& GetCommand() { return command; }
    ObjectGuid GetOwnerGuid() { return owner; }
    PlayerbotSecurityLevel GetRequiredLevel() { return requiredLevel; }
    uint32& GetType() { return type; }
    time_t& GetTime() { return time; }

private:
    std::string const command;
    ObjectGuid owner;
    PlayerbotSecurityLevel requiredLevel;  // what the sender needed when the command was accepted
    uint32 type;
    time_t time;
};

class PlayerbotAI : public PlayerbotAIBase
{
    // Consumes queued chat commands from FindReaction(); needs HandleCommands().
    friend class ReactionEngine;

public:
    PlayerbotAI();
    PlayerbotAI(Player* bot);
    virtual ~PlayerbotAI();

    void UpdateAI(uint32 elapsed, bool minimal = false) override;
    void UpdateAIInternal(uint32 elapsed, bool minimal = false) override;
    bool UpdateAIReaction(uint32 elapsed, bool minimal, bool canControlSelf);
    Engine* GetCurrentEngine() const { return currentEngine; }
    void SetActionDuration(Action const* action);
    using PlayerbotAIBase::SetActionDuration;

    std::string const HandleRemoteCommand(std::string const command);
    void HandleCommand(uint32 type, std::string const text, Player* fromPlayer);
    void QueueChatResponse(const ChatQueuedReply reply);
    void HandleBotOutgoingPacket(WorldPacket const& packet);
    void HandleMasterIncomingPacket(WorldPacket const& packet);
    void HandleMasterOutgoingPacket(WorldPacket const& packet);
    void HandleTeleportAck();
    void ChangeEngine(BotState type);
    void ChangeEngineOnCombat();
    void ChangeEngineOnNonCombat();
    void DoNextAction(bool minimal = false);
    // [stuck258] the combat engine out of combat with an idle, unreached target and no action that runs: let it go
    void ReleaseIdleTargetNobodyReaches(bool acted);
    // [stuck40] a target let go for a fact of this moment (no action of the combat engine ran on it, its pull spot
    // has no walk, its only walk passes a refused reach): the pickers of both engines leave it while that fact
    // holds. The memo lives in SoloSafety::NoteUnreachable / Unreachable, keyed per target and ended by a fact (the
    // bot or the target moved) - never a clock. It was one ObjectGuid here, which held one target at a time
    void ReleaseIdleTarget(Unit* target, char const* why);
    virtual bool DoSpecificAction(std::string const name, Event event = Event(), bool silent = false,
                                  std::string const qualifier = "");
    void ChangeStrategy(std::string const name, BotState type);
    void ClearStrategies(BotState type);
    void SelectiveResetStrategies(BotState type);
    std::vector<std::string> GetStrategies(BotState type);
    Strategy* GetStrategy(std::string const name, BotState type);
    void ApplyInstanceStrategies(uint32 mapId, bool tellMaster = false);
    bool IsInNonRaidDungeon() const;
    bool HasTargetExclusions() const;
    void EvaluateHealerDpsStrategy();
    bool ContainsStrategy(StrategyType type);
    bool HasStrategy(std::string const name, BotState type);
    BotState GetState() { return currentState; };
    void ResetStrategies(bool load = false);
    void ReInitCurrentEngine();
    void Reset(bool full = false);
    void LeaveOrDisbandGroup();
    static bool IsTank(Player* player, bool bySpec = false);
    static bool IsHeal(Player* player, bool bySpec = false);
    static bool IsDps(Player* player, bool bySpec = false);
    static bool IsRanged(Player* player, bool bySpec = false);
    static bool IsMelee(Player* player, bool bySpec = false);
    static bool IsCaster(Player* player, bool bySpec = false);
    static bool IsRangedDps(Player* player, bool bySpec = false);
    static bool IsCombo(Player* player);
    static ObjectGuid GetMainTankGuid(Group* group);
    static bool IsMainTank(Player* player);
    static bool IsExplicitMainTank(Player* player);
    static bool IsBotMainTank(Player* player);
    static uint32 GetGroupTankNum(Player* player);
    static bool IsAssistTank(Player* player);
    static bool IsAssistTankOfIndex(Player* player, uint8 index, bool indexLivingOnly = false);
    static bool IsAssistHealOfIndex(Player* player, uint8 index, bool indexLivingOnly = false);
    static bool IsAssistRangedDpsOfIndex(Player* player, uint8 index, bool indexLivingOnly = false);
    bool HasAggro(Unit* unit);
    bool IsMovementImpaired(Unit* unit);
    static int32 GetAssistTankIndex(Player* player);
    int32 GetGroupSlotIndex(Player* player);
    int32 GetRangedIndex(Player* player);
    int32 GetClassIndex(Player* player, uint8 cls);
    int32 GetRangedDpsIndex(Player* player);
    int32 GetMeleeIndex(Player* player);

    Creature* GetCreature(ObjectGuid guid);
    Unit* GetUnit(ObjectGuid guid);
    Player* GetPlayer(ObjectGuid guid);
    static Unit* GetUnit(CreatureData const* creatureData);
    GameObject* GetGameObject(ObjectGuid guid);
    // static GameObject* GetGameObject(GameObjectData const* gameObjectData);
    WorldObject* GetWorldObject(ObjectGuid guid);
    std::vector<Player*> GetAllPlayersInGroup();
    std::vector<Player*> GetRealPlayersInGroup();
    AreaTableEntry const* GetCurrentArea();
    AreaTableEntry const* GetCurrentZone();
    static std::string GetLocalizedAreaName(AreaTableEntry const* entry);
    static std::string GetLocalizedCreatureName(uint32 entry);
    static std::string GetLocalizedGameObjectName(uint32 entry);
    static constexpr size_t MAX_CHAT_MESSAGE_BYTES = 255;
    bool TellMaster(std::ostringstream& stream, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellMaster(std::string const text, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellMasterNoFacing(std::ostringstream& stream,
                            PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellMasterNoFacing(std::string const text,
                            PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellError(std::string const text, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool SayToGuild(std::string const& msg);
    bool SayToWorld(std::string const& msg);
    bool SayToChannel(std::string const& msg, ChatChannelId const& chanId);
    // [guild40] world thread: refreshes the channel copy map threads look up (SayToWorld / SayToChannel)
    static void RefreshChannelDirectory();
    bool SayToParty(std::string const& msg);
    bool SayToRaid(std::string const& msg);
    bool Yell(std::string const& msg);
    bool Say(std::string const& msg);
    bool Whisper(std::string const& msg, std::string const& receiverName);

    void SpellInterrupted(uint32 spellid);
    int32 CalculateGlobalCooldown(uint32 spellid);
    void RequestSpellInterrupt();
    void RemoveAura(std::string const name);
    void RemoveShapeshift();
    void WaitForSpellCast(Spell* spell);
    bool PlaySound(uint32 emote);
    bool PlayEmote(uint32 emote);
    void Ping(float x, float y);
    Item* FindPoison() const;
    Item* FindAmmo() const;
    Item* FindBandage() const;
    Item* FindOpenableItem() const;
    Item* FindLockedItem() const;
    Item* FindConsumable(uint32 itemId) const;
    Item* FindStoneFor(Item* weapon) const;
    Item* FindOilFor(Item* weapon) const;
    void ImbueItem(Item* item, uint32 targetFlag, ObjectGuid targetGUID);
    void ImbueItem(Item* item, uint8 targetInventorySlot);
    void ImbueItem(Item* item, Unit* target);
    void ImbueItem(Item* item);
    void EnchantItemT(uint32 spellid, uint8 slot);
    int32 GetNearGroupMemberCount(float dis = sPlayerbotAIConfig.SightDistance);

    virtual bool CanCastSpell(std::string const name, Unit* target, Item* itemTarget = nullptr);
    virtual bool CastSpell(std::string const name, Unit* target, Item* itemTarget = nullptr);
    virtual bool HasSpell(std::string const spellName) const;
    virtual bool HasAura(std::string const spellName, Unit* player, bool maxStack = false, bool checkIsOwner = false,
                         int maxAmount = -1, bool checkDuration = false);
    virtual bool HasAnyAuraOf(Unit* player, ...);

    virtual bool IsInterruptableSpellCasting(Unit* player, std::string const spell);
    virtual bool HasAuraToDispel(Unit* player, uint32 dispelType);
    bool CanCastSpell(uint32 spellid, Unit* target, bool checkHasSpell = true, Item* itemTarget = nullptr,
                      Item* castItem = nullptr);
    bool CanCastSpell(uint32 spellid, GameObject* goTarget, bool checkHasSpell = true, Item* castItem = nullptr);
    bool CanCastSpell(uint32 spellid, float x, float y, float z, bool checkHasSpell = true,
                      Item* itemTarget = nullptr);
    // [lead1390] What the game answers for this caster, spell and target now: Spell::CheckCast with the cast's own
    // flags (TRIGGERED_NONE: the bot's own cast, the spell known and off its cooldown), the power the cast check
    // cannot see before Spell::prepare (Spell::m_powerCost), and what an earlier answer hides (Spell::CheckCast stops
    // at its first failed check): the checks after the form (TRIGGERED_IGNORE_SHAPESHIFT), the caster's auras.
    // A passing state (a walk, sitting, the global cooldown, a cast or channel under way, facing, a lockpick roll)
    // ends by itself and a retry is right; every other answer is a refusal the leader must know. CanCastSpell stays
    // the rotation's check: it also passes the answers its prerequisites fix (the form left, the walk into range).
    struct CastCheck
    {
        SpellCastResult result = SPELL_CAST_OK;      // the game's first failed check now (OK: castable now)
        SpellCastResult refusal = SPELL_CAST_OK;     // the first answer no passing state explains (OK: none)
        SpellCastResult behindForm = SPELL_CAST_OK;  // `refusal` with the form left (the answer behind NOT_SHAPESHIFT)
        bool Castable() const { return result == SPELL_CAST_OK; }
        bool Refused() const { return refusal != SPELL_CAST_OK; }
    };
    // `castItem`: an item's use (its spell need not be known); `flags`: the game's own cast flags (a game object's
    // click casts its spell triggered, GameObject::Use)
    CastCheck CheckCastNow(uint32 spellId, Unit* target, Item* castItem = nullptr,
                           TriggerCastFlags flags = TRIGGERED_NONE);
    // the answer ends by itself (a retry is right): moving, sitting, the global cooldown only, a cast under way, facing
    bool IsPassingCastAnswer(SpellCastResult result, SpellInfo const* spellInfo, Item* castItem = nullptr) const;
    // "SPELL_FAILED_OUT_OF_RANGE" (the core's enum name)
    static std::string CastResultName(SpellCastResult result);

    Aura* GetAura(std::string const spellName, Unit* unit, bool checkIsOwner = false, bool checkDuration = false,
                  int checkStack = -1);
    bool CastSpell(uint32 spellId, Unit* target, Item* itemTarget = nullptr);
    bool CastSpell(uint32 spellId, float x, float y, float z, Item* itemTarget = nullptr);
    bool canDispel(SpellInfo const* spellInfo, uint32 dispelType);

    bool CanCastVehicleSpell(uint32 spellid, Unit* target);
    bool CastVehicleSpell(uint32 spellId, Unit* target);
    bool CastVehicleSpell(uint32 spellId, float x, float y, float z);
    bool IsInVehicle(bool canControl = false, bool canCast = false, bool canAttack = false, bool canTurn = false,
                     bool fixed = false);

    uint32 GetEquipGearScore(Player* player);
    //uint32 GetEquipGearScore(Player* player, bool withBags, bool withBank);
    static uint32 GetMixedGearScore(Player* player, bool withBags, bool withBank, uint32 topN = 0);
    bool HasSkill(SkillType skill);
    bool IsAllowedCommand(std::string const text);
    float GetRange(std::string const type);
    float GetHunterWeaponRange();  // [b32_engage] Auto Shot range less a margin, 0 without a ranged weapon

    Player* GetBot() { return bot; }
    Player* GetMaster() { return master; }
    Player* FindNewMaster();

    // Get the group leader or the master of the bot.
    // Checks if the bot is summoned an altbot of a player
    bool IsAltBot();
    bool HasGameClientMaster();
    Player* GetGroupLeader();
    uint32 GetFixedBotNumber(uint32 maxNum = 100);
    GrouperType GetGrouperType();
    GuilderType GetGuilderType();
    bool HasPlayerNearby(WorldPosition* pos, float range = sPlayerbotAIConfig.ReactDistance);
    bool HasPlayerNearby(float range = sPlayerbotAIConfig.ReactDistance);
    bool AllowActive(ActivityType activityType);
    bool AllowActivity(ActivityType activityType = ALL_ACTIVITY, bool checkNow = false);
    bool IsActivityAllowedCached() const { return allowActive[ALL_ACTIVITY]; }
    uint32 AutoScaleActivity(uint32 mod);

    // Check if player is safe to use.
    bool IsSafe(Player* player);
    bool IsSafe(WorldObject* obj);
    ChatChannelSource GetChatChannelSource(Player* bot, uint32 type, std::string channelName);

    bool StarterLevelDistanceCheck(Player* player, WorldLocation const& loc, bool fromStartUp = false);

    // Bots play under the same rules as players (user decision 2026-10-23): no cheat is ever active,
    // whatever Playerbots.BotCheats or the "cheat" chat command say. Kept as a kill switch for old call sites.
    bool HasCheat([[maybe_unused]] BotCheatMask mask) { return false; }
    BotCheatMask GetCheat() { return cheatMask; }
    void SetCheat(BotCheatMask mask) { cheatMask = mask; }

    void SetMaster(Player* newMaster) { master = newMaster; }
    AiObjectContext* GetAiObjectContext() { return aiObjectContext; }
    ChatHelper* GetChatHelper() { return &chatHelper; }
    bool IsOpposing(Player* player);
    static bool IsOpposing(uint8 race1, uint8 race2);
    PlayerbotSecurity* GetSecurity() { return &security; }

    Position GetJumpDestination() { return jumpDestination; }
    void SetJumpDestination(Position pos) { jumpDestination = pos; }
    void ResetJumpDestination() { jumpDestination = Position(); }

    bool CanMove();
    bool IsInRealGuild();
    static std::vector<std::string> dispel_whitelist;
    bool EqualLowercaseName(std::string s1, std::string s2);
    InventoryResult CanEquipItem(uint8 slot, uint16& dest, Item* pItem, bool swap, bool not_loading = true) const;
    uint8 FindEquipSlot(ItemTemplate const* proto, uint32 slot, bool swap) const;
    std::vector<Item*> GetInventoryAndEquippedItems();
    std::vector<Item*> GetInventoryItems();
    uint32 GetInventoryItemsCountWithId(uint32 itemId);

    /**
     * Consolidate all stackable items in the bot's bags: merge partial stacks of the same
     * entry into the fullest stacks until at most one partial stack remains per entry
     * (the client's "Consolidate" button; WotLK has no server-side equivalent). Uses the
     * core's SwapItem merge path, so no quest/progress hooks fire for moved stacks.
     */
    void ConsolidateItems();
    bool HasItemInInventory(uint32 itemId);
    std::vector<std::pair<Quest const*, uint32>> GetCurrentQuestsRequiringItemId(uint32 itemId);
    uint32 GetReactDelay();
    // Remaining global cooldown of the last spell this bot cast, in ms (0 when it is over).
    uint32 GetGlobalCooldownRemaining();
    // Temporary combat idle diagnostics (LOG_DEBUG "[idle diag]"): the engine executed `action` this tick, fired by
    // the trigger `source` (the event's source; empty for a default action).
    void NoteIdleDiagAction(Action* action, std::string const& source);
    // [chat20] the leader's command held one of this tick's actions (LeaderCommandMultiplier::Log)
    void NoteIdleDiagLeaderHold() { idleDiag.tickLeaderHold = true; }
    // [grp20] why the engine turned down this tick's action, in its own words (Engine::DoNextAction).
    enum IdleRefusal : uint8
    {
        IDLE_REFUSAL_NONE = 0,
        IDLE_REFUSAL_USELESS,       // isUseful() false
        IDLE_REFUSAL_IMPOSSIBLE,    // isPossible() false: the core's own answer, mana among its reasons
        IDLE_REFUSAL_PREREQ,        // a prerequisite was pushed in its place
        IDLE_REFUSAL_FAILED,        // it ran and reported failure
        IDLE_REFUSAL_BLOCKED,       // a multiplier scaled it to zero
        IDLE_REFUSAL_NO_CANDIDATE,  // the queue held nothing at all
        IDLE_REFUSAL_MAX
    };
    // the queue is relevance-ordered, so the first refusal of the tick is the action the bot would have taken
    void NoteIdleDiagRefusal(IdleRefusal cause)
    {
        if (idleDiag.tickRefusal == IDLE_REFUSAL_NONE)
            idleDiag.tickRefusal = cause;
    }

    std::vector<Quest const*> GetAllCurrentQuests();
    std::vector<Quest const*> GetCurrentIncompleteQuests();
    std::set<uint32> GetAllCurrentQuestIds();
    std::set<uint32> GetCurrentIncompleteQuestIds();
    void PetFollow();
    static float GetItemScoreMultiplier(ItemQualities quality);
    static bool IsHealingSpell(uint32 spellFamilyName, flag96 spellFamilyFlags);
    static SpellFamilyNames Class2SpellFamilyName(uint8 cls);
    NewRpgInfo rpgInfo;
    NewRpgStatistic rpgStatistic;
    std::unordered_set<uint32> countedQuestCompletions;
    void RecordQuestAccepted(uint32 questId);
    void RecordQuestCompleted(uint32 questId);
    void RecordQuestRewarded(uint32 questId);
    void RecordQuestDropped(uint32 questId, QuestDropReason reason);
    void RecordQuestAbandoned(uint32 questId, QuestAbandonReason reason);
    std::unordered_set<uint32> lowPriorityQuest;
    time_t bgReleaseAttemptTime = 0;
    ForceRebuffState forceRebuff;
    bool mimicStandLogged = false;  // [lead1552] one log line per sit under the leader's Rest

    // Schedules a callback to run once after <delayMs> milliseconds.
    void AddTimedEvent(std::function<void()> callback, uint32 delayMs);

    // Ground or liquid within GROUND_SUPPORT_DROP below the point: what a client's gravity would stand on.
    static bool HasGroundSupport(WorldObject const* object, float x, float y, float z);
    // [regress10] every sample GuardAirborneSpline takes on the straight line has that support (a move it lets run)
    static bool StraightLineSupported(WorldObject const* object, float fromX, float fromY, float fromZ, float toX,
                                      float toY, float toZ);
    // the spacing of those samples (yd)
    static float GroundSampleStep();
    // [nav256] the point lies more than a step under the terrain of a lake's bed (outdoors, no cave model): the
    // navmesh generator keeps only the water's surface over deep water and drops the bed, so a polygon there is a
    // model's underside under the ground. `surface`: that water's level (the way up is a swim)
    static bool UnderLakeBed(WorldObject const* object, float x, float y, float z, float* surface = nullptr);
    // [nav240] the point lies more than a step under the grid's terrain, whatever stands over it (water or dry
    // ground). Says nothing about why - a model's underside under a dropped sea bed, a cave, a WMO floor sunk into
    // its hillside - only that the grid's surface is overhead. UnderLakeBed is this plus water over that same quad;
    // a bot buried under a quad just over the waterline is in the same place with no water over its own feet
    static bool UnderTerrain(WorldObject const* object, float x, float y, float z);
    // Gravity applies to the bot: alive, outside any vehicle, transport or taxi, and without a way to fly.
    bool IsSubjectToGravity() const;
    // Next tick, a bot that lost its mount or seat in the air falls and takes the damage where it lands.
    void RequestFallCheck() { fallCheckRequested = true; }
    // [deaths145] the walk to this spot was refused by GuardAirborneSpline ([nav29] a leg under the surface): a mover
    // that asks for the same spot again gets the same refusal, so it takes another spot (within the bot's combat
    // reach of the refused walk's end, on its map)
    bool GuardRefusedWalkTo(float x, float y, float z) const;

private:
    uint32 guardRefusedMap = std::numeric_limits<uint32>::max();  // [deaths145]
    float guardRefusedX = 0.0f, guardRefusedY = 0.0f, guardRefusedZ = 0.0f;
    // Bots have no client to apply gravity: stop straight splines that leave the ground, fall when airborne.
    void GuardAirborneSpline();
    void UpdateFall();
    void StartFall();
    static void _fillGearScoreData(Player* player, Item* item, std::vector<uint32>* gearScore, uint32& twoHandScore,
                                   bool mixed = false);
    bool IsTellAllowed(PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    void UpdateAIGroupMaster();
    Item* FindItemInInventory(std::function<bool(ItemTemplate const*)> checkItem) const;
    void HandleCommands();
    void HandleCommand(uint32 type, std::string const& text, Player& fromPlayer, const uint32 lang = LANG_UNIVERSAL);
    // [lead1390] one Spell::CheckCast of the bot's cast with these flags, and the checks it stops before: the caster's
    // auras (Spell::CheckCasterAuras) behind an earlier answer, and the rune cost (Spell::CheckRuneCost)
    struct CastRun
    {
        SpellCastResult cast = SPELL_CAST_OK;
        SpellCastResult auras = SPELL_CAST_OK;
        SpellCastResult runes = SPELL_CAST_OK;
    };
    CastRun RunCheckCast(SpellInfo const* spellInfo, Unit* target, Item* itemTarget, Item* castItem,
                         TriggerCastFlags flags);
    inline bool IsValidUnit(Unit const* unit) const
    {
        return unit && unit->IsInWorld() && !unit->IsDuringRemoveFromWorld();
    }
    inline bool IsValidPlayer(Player const* player) const
    {
        return player && player->GetSession() && player->IsInWorld() && !player->IsDuringRemoveFromWorld() &&
               !player->IsBeingTeleported();
    }
protected:
    Player* bot;
    Player* master;
    uint32 accountId;
    AiObjectContext* aiObjectContext;
    Engine* currentEngine;
    Engine* engines[BOT_STATE_MAX];
    BotState currentState;
    ChatHelper chatHelper;
    std::list<ChatCommandHolder> chatCommands;
    // Filled from the speaker's thread (SMSG_MESSAGECHAT is handled where it is sent, often another map's worker) and
    // drained on this bot's map thread: guarded, a bare list corrupted the heap (2026-10-24 freezes in ChatReplyDo).
    std::list<ChatQueuedReply> chatReplies;
    std::mutex chatRepliesLock;
    PacketHandlingHelper botOutgoingPacketHandlers;
    PacketHandlingHelper masterIncomingPacketHandlers;
    PacketHandlingHelper masterOutgoingPacketHandlers;
    CompositeChatFilter chatFilter;
    PlayerbotSecurity security;
    std::map<std::string, time_t> whispers;
    std::pair<ChatMsg, time_t> currentChat;
    static std::set<std::string> unsecuredCommands;
    bool allowActive[MAX_ACTIVITY_TYPE];
    time_t allowActiveCheckTimer[MAX_ACTIVITY_TYPE];
    ReactionEngine* reactionEngine = nullptr;
    bool inCombat = false;
    BotCheatMask cheatMask = BotCheatMask::none;
    Position jumpDestination = Position();
    uint32 nextTransportCheck = 0;
    bool spellInterruptRequested = false;
    uint32 lastDodgeStopCastMs = 0;  // a bot that cannot get out is not kept from casting forever
    uint32 checkedSplineId = 0;       // GuardAirborneSpline checks each spline once
    uint64 paceSpot = 0;              // [pace25] the spot and New RPG state of the last standing look
    uint8 paceSameLooks = 0;          // [pace25] standing looks in a row there
    static constexpr uint8 PACE_SAME_LOOKS = 2;  // the fast looks a standing bot gets before the idle tick
    uint32 lastAirborneLogMs = 0;
    bool fallCheckRequested = false;  // dismounted or left a vehicle: fall if nothing is below
    bool fallPending = false;         // falling; HandleFall runs where the fall spline ends
    bool wasOnVehicle = false;
    bool wasSplineActive = false;  // [ledge54] a move ended this tick: the ground under its end is looked at
    uint32 lastCastSpellId = 0;  // the last spell CastSpell started, for the GCD-aligned wake-up in combat

    // Temporary heal diagnostics: a healer in an instance group casts nothing while a member in combat is below
    // half health ("[heal diag]", at most every HEAL_DIAG_LOG_MS): its engine, combat state and action trail.
    void UpdateHealDiag();
    uint32 healDiagCheckMs = 0;
    uint32 healDiagLogMs = 0;

    // Temporary combat idle diagnostics: why a dps bot in an instance group cast nothing on a tick.
    void UpdateIdleDiag(bool casting);
    // [rot171] LOG_DEBUG "[rogue gcd]": one line per rogue cast on a dungeon boss (energy and combo points after it,
    // the action and its trigger, ms since the last cast and the energy after that one)
    void TraceRogueGcd(Action* action, std::string const& source);
    uint32 rogueGcdLastMs = 0;
    uint32 rogueGcdLastEnergy = 0;
    // [rot181] LOG_DEBUG "[dk gcd]": one line per death knight cast on a dungeon boss (the six runes and runic power
    // after it, the action and its trigger, ms since the last cast, our diseases on the target, attackers)
    void TraceDkGcd(Action* action, std::string const& source);
    uint32 dkGcdLastMs = 0;
    struct IdleDiag
    {
        uint32 windowStartMs = 0;
        uint32 ticks = 0;
        uint32 casts = 0;
        uint32 castWait = 0;
        uint32 noTarget = 0;
        uint32 rangeLos = 0;
        uint32 moving = 0;
        uint32 gcd = 0;
        uint32 threat = 0;      // the bot's OWN threat on the target, not the leader's Stop
        uint32 leaderHold = 0;  // [chat20] the leader's command zeroed the bot's actions this tick
        uint32 other = 0;
        uint32 refusals[IDLE_REFUSAL_MAX] = {};  // [grp20] `other` split by the engine's own refusal
        uint32 nonCast = 0;
        uint32 gapCount = 0;
        uint64 gapSumMs = 0;
        uint32 gapMaxMs = 0;
        uint32 gapOver150 = 0;
        uint32 gapOver400 = 0;
        uint32 busyEndMs = 0;
        bool tickCast = false;
        bool tickLeaderHold = false;  // [chat20] set by LeaderCommandMultiplier::Log, cleared with tickAction
        IdleRefusal tickRefusal = IDLE_REFUSAL_NONE;  // [grp20] set by the engine, cleared with tickAction
        std::string tickAction;
        std::map<std::string, uint32> actions;
    } idleDiag;
};

#endif
