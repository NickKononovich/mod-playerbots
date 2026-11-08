/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WarlockTriggers.h"

#include <algorithm>  // [rot566]
#include <cmath>      // [rot566] std::floor

#include "AiFactory.h"  // [rot400] GetPlayerSpecTab
#include "Creature.h"   // [rot470] EncounterCreditDead
#include "DynamicObject.h"
#include "GameObject.h"
#include "GenericTriggers.h"
#include "DBCStores.h"       // [rot461] IsSharedDifficultyMap
#include "GameTime.h"        // [rot480] the respawn times' clock
#include "InstanceScript.h"  // [rot461]
#include "Item.h"
#include "LeaderOrders.h"  // [lead955] Leader::ExecutorMember
#include "Map.h"
#include "ObjectMgr.h"  // [rot461] GetDungeonEncounterList
#include "Pet.h"  // [rot600]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RaidConsumables.h"
#include "SpellAuraEffects.h"  // [rot350]
#include "SpellAuras.h"        // [rot350]
#include "SpellMgr.h"          // [rot350]
#include "SpellPayoff.h"       // [rot566] Read
#include "TargetValue.h"
#include "WarlockActions.h"  // [rot481] ImmolationAuraReaches

#include <unordered_map>  // [rot470]

static const uint32 SOUL_SHARD_ITEM_ID = 6265;

uint32 GetSoulShardCount(Player* bot)
{
    return bot->GetItemCount(SOUL_SHARD_ITEM_ID, false);  // false = only bags
}

// List of all Soulstone item IDs
static const std::vector<uint32> soulstoneItemIds = {
    5232,   // Minor Soulstone
    16892,  // Lesser Soulstone
    16893,  // Soulstone
    16895,  // Greater Soulstone
    16896,  // Major Soulstone
    22116,  // Master Soulstone
    36895   // Demonic Soulstone
};

uint32 GetSoulstoneCount(Player* bot)
{
    uint32 count = 0;
    for (uint32 id : soulstoneItemIds)
        count += bot->GetItemCount(id, false);  // false = only bags
    return count;
}

namespace
{
    // [b31_deep] Spellstone / Firestone are a temporary enchant of the main-hand weapon, not an aura on the bot: the
    // BuffTrigger aura check was always true, and the use action ran every tick ("spellstone 24 Execute returned
    // false", 179 times in the affliction logs). The weapon stone is due only while the main hand has no temporary
    // enchant.
    bool MainHandNeedsWeaponStone(Player* bot)
    {
        Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        return weapon && !weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
    }
}

bool SpellstoneTrigger::IsActive()
{
    return MainHandNeedsWeaponStone(bot) && AI_VALUE2(uint32, "item count", getName()) > 0;
}

bool FirestoneTrigger::IsActive()
{
    return MainHandNeedsWeaponStone(bot) && AI_VALUE2(uint32, "item count", getName()) > 0;
}

bool WarlockConjuredItemTrigger::IsActive()
{
    return ItemCountTrigger::IsActive() && AI_VALUE2(uint32, "item count", "soul shard") > 0;
}

bool OutOfSoulShardsTrigger::IsActive() { return GetSoulShardCount(botAI->GetBot()) == 0; }

bool TooManySoulShardsTrigger::IsActive() { return GetSoulShardCount(botAI->GetBot()) >= 26; }

bool OutOfSoulstoneTrigger::IsActive() { return GetSoulstoneCount(botAI->GetBot()) == 0; }

namespace
{
    constexpr uint32 SPELL_RITUAL_OF_SOULS_RANK_1 = 29893;
    constexpr uint32 SPELL_RITUAL_OF_SOULS_RANK_2 = 58887;
    constexpr float RITUAL_OF_SOULS_BOSS_DISTANCE = 45.0f;

    bool CanStartRitualOfSouls(Player* player)
    {
        for (uint32 spellId : {SPELL_RITUAL_OF_SOULS_RANK_2, SPELL_RITUAL_OF_SOULS_RANK_1})
            if (player->HasSpell(spellId))
                return !player->HasSpellCooldown(spellId);
        return false;
    }
}

bool RitualOfSoulsTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || !CanStartRitualOfSouls(bot) ||
        !AI_VALUE2(uint32, "item count", "soul shard"))
        return false;

    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    if (!group || !map || !(map->IsRaid() || (map->IsDungeon() && map->IsHeroic())))
        return false;

    // One caster per group: the bot warlock with the lowest GUID among those who can start the ritual now
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || member->getClass() != CLASS_WARLOCK || !member->IsAlive() ||
            member->IsInCombat() || member->GetMap() != map || !GET_PLAYERBOT_AI(member) ||
            !CanStartRitualOfSouls(member))
            continue;

        if (member->GetGUID() < bot->GetGUID())
            return false;
    }

    if (!AI_VALUE(bool, "ready check in progress"))
    {
        bool bossNear = false;
        for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets"))
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && unit->IsAlive() && !unit->IsInCombat() && ai::raidutil::IsBossUnit(unit) &&
                bot->GetDistance(unit) <= RITUAL_OF_SOULS_BOSS_DISTANCE)
            {
                bossNear = true;
                break;
            }
        }

        if (!bossNear)
            return false;
    }

    // Not while a Soulwell, or the Soul Portal of a ritual in progress, is already up nearby
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "nearest game objects"))
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (go && go->isSpawned() && (RaidConsumables::IsSoulwell(go) || RaidConsumables::IsSoulPortal(go)) &&
            bot->GetDistance(go) <= RaidConsumables::SOULWELL_SEARCH_DISTANCE)
            return false;
    }

    // [b43_class] not at "pull imminent" or next to a pack: only at a real rest or the run start
    std::string const blocked = RaidConsumables::RitualBlockedReason(botAI);
    if (!blocked.empty())
    {
        LOG_DEBUG("playerbots", "[b43_class] {}: ritual of souls waits: {}", bot->GetName(), blocked);
        return false;
    }

    return true;
}

bool DemonArmorTrigger::IsActive()
{
    Unit* target = GetTarget();
    return !botAI->HasAura("demon skin", target) && !botAI->HasAura("demon armor", target) &&
           !botAI->HasAura("fel armor", target);
}

bool SoulLinkTrigger::IsActive()
{
    // [b31_deep] Only with the talent learned and a pet out (157 "spell not known" failures for affliction).
    if (!bot->GetPet() || !AI_VALUE2(uint32, "spell id", "soul link"))
        return false;

    Unit* target = GetTarget();
    return !botAI->HasAura("soul link", target);
}

bool UnendingBreathTrigger::IsActive()
{
    return BuffTrigger::IsActive() && AI_VALUE2(bool, "swimming", "self target");
}

bool UnendingBreathOnPartyTrigger::IsActive()
{
    return BuffOnPartyTrigger::IsActive() && AI_VALUE2(bool, "swimming", "self target");
}

bool DemonicEmpowermentTrigger::IsActive()
{
    Pet* pet = bot->GetPet();
    if (!pet)
        return false;
    return !botAI->HasAura("demonic empowerment", pet);
}

bool DecimationTrigger::IsActive()
{
    Aura* aura = botAI->GetAura(getName(), GetTarget(), false, true);
    return aura && aura->GetDuration() > 3000;
}

// Checks if the bot's mana is below 85% and health is above a low health threshold
bool LifeTapTrigger::IsActive()
{
    // [lead955] out of a fight a led warlock's Life Tap for mana is the leader's command (its fastest way to the
    // mana line: Life Tap, a drink or an Innervate); its own trigger is off there. In a fight the rotation's filler
    if (!bot->IsInCombat() && Leader::ExecutorMember(bot))
        return false;
    if (AI_VALUE2(uint8, "health", "self target") <= sPlayerbotAIConfig.LowHealth)
        return false;

    if (!AI_VALUE2(bool, "has mana", "self target"))
        return false;
    if (AI_VALUE2(uint8, "mana", "self target") >= 85)
        return false;

    return true;
}

// Checks if the Life Tap Glyph buff is active
bool LifeTapGlyphBuffTrigger::IsActive()
{
    constexpr uint32 SPELL_LIFE_TAP_GLYPH = 63320;
    // Titanforged Doomcaller's Regalia (E1) 4p: Life Tap grants the glyph buff without the glyph ([setbonus-rot]).
    constexpr uint32 SPELL_SET_E1_WARLOCK_4P = 127151;
    return (bot->HasAura(SPELL_LIFE_TAP_GLYPH) || bot->HasAura(SPELL_SET_E1_WARLOCK_4P)) && BuffTrigger::IsActive();
}

namespace ai::warlock
{
using ai::raidutil::AssignedCurse;

AssignedCurse GetCoordinatedCurse(PlayerbotAI* botAI)
{
    // Only the spec default curse strategies are coordinated; any other curse strategy is a player choice.
    if (!botAI->HasStrategy("curse of agony", BOT_STATE_COMBAT) &&
        !botAI->HasStrategy("curse of elements", BOT_STATE_COMBAT))
    {
        return AssignedCurse::NONE;
    }

    ai::raidutil::RaidUtilityAssignment const& assignment = ai::raidutil::GetAssignment(botAI);
    if (!assignment.valid || !assignment.curseCoordinated)
        return AssignedCurse::NONE;

    return assignment.curse;
}

bool IsCurseAllowed(PlayerbotAI* botAI, AssignedCurse curse)
{
    AssignedCurse const assigned = GetCoordinatedCurse(botAI);
    return assigned == AssignedCurse::NONE || assigned == curse;
}
}

std::string const ai::warlock::DotFiller(Player* bot)
{
    if (AiFactory::GetPlayerSpecTab(bot) != WARLOCK_TAB_DESTRUCTION)
        return "shadow bolt";
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    bool const incinerate =
        botAI && botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "incinerate")->Get();  // [rot601]
    return incinerate ? "incinerate" : "shadow bolt";
}

bool WarlockPetAssistTrigger::IsActive()
{
    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive() || pet->HasReactState(REACT_PASSIVE) || !pet->GetCharmInfo() || !pet->AI())
        return false;
    if (Unit* victim = pet->GetVictim())
        if (victim->IsAlive())
            return false;
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target))
        return false;
    return !bot->GetGroup() || target->IsInCombat();
}

// [b31_deep] Agony never replaces the bot's own Curse of Doom (one curse per warlock per target).
bool CurseOfAgonyTrigger::IsActive()
{
    return ai::warlock::IsCurseAllowed(botAI, ai::raidutil::AssignedCurse::AGONY) && DebuffTrigger::IsActive() &&
           !botAI->HasAura("curse of doom", GetTarget(), false, true);
}

bool CurseOfAgonyOnAttackerTrigger::IsActive()
{
    return ai::warlock::IsCurseAllowed(botAI, ai::raidutil::AssignedCurse::AGONY) && DebuffTrigger::IsActive() &&
           !botAI->HasAura("curse of doom", GetTarget(), false, true);
}

bool DamageCurseOfDoomTrigger::IsActive()
{
    ai::raidutil::AssignedCurse const assigned = ai::warlock::GetCoordinatedCurse(botAI);
    if (assigned != ai::raidutil::AssignedCurse::AGONY &&
        (assigned != ai::raidutil::AssignedCurse::NONE || !botAI->HasStrategy("curse of agony", BOT_STATE_COMBAT)))
        return false;

    uint32 const doomId = AI_VALUE2(uint32, "spell id", "curse of doom");
    if (!doomId || bot->HasSpellCooldown(doomId))
        return false;

    Unit* target = GetTarget();
    if (!target || target->IsPlayer() || !DebuffTrigger::IsActive())
        return false;

    return ExpectedLifetime(botAI, target) >= CURSE_OF_DOOM_MIN_TARGET_LIFETIME;
}

// [b35_lock]
bool InfernoBossTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    uint32 const infernoId = AI_VALUE2(uint32, "spell id", "inferno");
    if (!infernoId || bot->HasSpellCooldown(infernoId))
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || target->IsPlayer() || !ai::raidutil::IsBossUnit(target))
        return false;

    if (ExpectedLifetime(botAI, target) > INFERNO_MAX_TARGET_LIFETIME || !bot->HasItemCount(INFERNO_REAGENT_ITEM, 1))
        return false;

    // [rot461] wowsims' remainingTime is the rest of the simulation: the Infernal takes the pet slot (Pet.Disable) and
    // nothing comes after. In an instance another boss comes after, and the warlock has to resummon its demon (a 10 s
    // cast) once the Infernal is gone. So Inferno goes only where wowsims' "end of the fight" is real: on the last
    // boss of the instance, or with no demon out to lose
    if (!bot->GetPet())
        return true;

    bool const last = InfernoOnLastEncounter(bot, target);
    if (!last)
        LOG_DEBUG("playerbots", "[rot461] {} holds Inferno on {}: not the instance's last boss, its demon would need a "
                  "resummon", bot->GetName(), target->GetName());
    return last;
}

// [rot461] Every other DungeonEncounter.dbc encounter of the map is in the instance's completed mask (the core sets
// it per killed credit creature, Map::UpdateEncounterState). No list or no instance script: unknown, false.
// [rot470] b240 rfc-h: 142 "holds Inferno on Bazzalan: not the instance's last boss" with Bazzalan the last boss
// standing, so no Inferno at all. A classic map runs as a heroic here (difficulty 1) and DungeonEncounter.dbc lists
// only its normal mode: GetDungeonEncounterList(389, 1) is empty, and Map::UpdateEncounterState, which looks the list
// up under the same difficulty, never sets the completed mask. The list falls back to the normal mode (as the harness'
// LoadEncounters does), and an encounter is done by the mask or by its kill-credit creature lying dead in the map (an
// instance creature stays in the map's store once its grid is loaded; one never loaded counts as standing). Looked
// up once per boss target (the store walk is no per-tick work)
// [rot480] b241-b255: rot470 never fired once ("holds Inferno on Bazzalan: not the instance's last boss" 145 times in
// b255 with Bazzalan the last boss standing). The core runs the dynamic spawn mode (spawn group 0 has no
// SPAWNGROUP_FLAG_COMPATIBILITY_MODE, Respawn.ForceCompatibilityMode = 0): Creature::RemoveCorpse saves the respawn
// time and removes the dead creature from the map entirely (AddObjectToRemoveList), so a boss killed minutes ago is no
// longer in the spawn-id store. Its spawn is then dead by the map's own record: a pending respawn time
// (Map::GetCreatureRespawnTimes) for a spawn of the credit creature (CreatureData::id)
namespace
{
    bool EncounterCreditDead(Map* map, uint32 creditEntry)
    {
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (creature && creature->GetEntry() == creditEntry && !creature->IsAlive())
                return true;
        time_t const now = GameTime::GetGameTime().count();
        for (auto const& [spawnId, respawnTime] : map->GetCreatureRespawnTimes())
        {
            if (respawnTime <= now)
                continue;
            CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
            if (data && (data->id == creditEntry || data->id2 == creditEntry || data->id3 == creditEntry))
                return true;
        }
        return false;
    }

    bool LastEncounterLookup(Player* bot, Unit* target)
    {
        Map* map = bot->GetMap();
        InstanceMap* instanceMap = map ? map->ToInstanceMap() : nullptr;
        if (!instanceMap)
            return false;

        // the difficulty Map::UpdateEncounterState credits the kills under
        Difficulty const difficulty =
            IsSharedDifficultyMap(map->GetId()) ? Difficulty(map->GetDifficulty() % 2) : map->GetDifficulty();
        DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(map->GetId(), difficulty);
        if ((!encounters || encounters->empty()) && difficulty != DUNGEON_DIFFICULTY_NORMAL)
            encounters = sObjectMgr->GetDungeonEncounterList(map->GetId(), DUNGEON_DIFFICULTY_NORMAL);
        if (!encounters || encounters->empty())
            return false;

        InstanceScript* instance = instanceMap->GetInstanceScript();
        uint32 const done = instance ? instance->GetCompletedEncounterMask() : 0;
        bool targetIsOne = false;
        for (DungeonEncounter const* encounter : *encounters)
        {
            if (!encounter || !encounter->dbcEntry)
                continue;

            bool const killCredit = encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE;
            if (killCredit && encounter->creditEntry == target->GetEntry())
            {
                targetIsOne = true;
                continue;
            }

            if (done & (1u << encounter->dbcEntry->encounterIndex))
                continue;
            if (!killCredit || !EncounterCreditDead(map, encounter->creditEntry))
                return false;
        }
        return targetIsOne;
    }
}

bool InfernoOnLastEncounter(Player* bot, Unit* target)
{
    struct Looked
    {
        uint32 instanceId = 0;
        ObjectGuid target;
        bool last = false;
    };
    thread_local std::unordered_map<ObjectGuid, Looked> looked;
    if (looked.size() > 4096)
        looked.clear();

    Looked& entry = looked[bot->GetGUID()];
    uint32 const instanceId = bot->GetInstanceId();
    if (entry.target != target->GetGUID() || entry.instanceId != instanceId)
    {
        entry.instanceId = instanceId;
        entry.target = target->GetGUID();
        entry.last = LastEncounterLookup(bot, target);
        if (entry.last)
            LOG_DEBUG("playerbots",
                      "[rot470] [rot480] {}: {} is the instance's last boss standing (every other encounter done)",
                      bot->GetName(), target->GetName());
    }
    return entry.last;
}

// [rot481] see ImmolationAuraReadyTrigger (WarlockTriggers.h)
bool ImmolationAuraReadyTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "immolation aura");
    if (!spellId || bot->HasSpellCooldown(spellId) || botAI->HasAura("immolation aura", bot))
        return false;

    return ImmolationAuraReaches(bot, AI_VALUE(Unit*, "current target"));
}

bool CurseOfTonguesTrigger::IsActive()
{
    return ai::warlock::IsCurseAllowed(botAI, ai::raidutil::AssignedCurse::TONGUES) && BuffTrigger::IsActive();
}

bool AssignedCurseOfTheElementsTrigger::IsActive()
{
    return ai::warlock::GetCoordinatedCurse(botAI) == ai::raidutil::AssignedCurse::ELEMENTS &&
           CurseOfTheElementsTrigger::IsActive();
}

bool AssignedCurseOfWeaknessTrigger::IsActive()
{
    return ai::warlock::GetCoordinatedCurse(botAI) == ai::raidutil::AssignedCurse::WEAKNESS &&
           CurseOfWeaknessTrigger::IsActive();
}

bool AssignedCurseOfTonguesTrigger::IsActive()
{
    return ai::warlock::GetCoordinatedCurse(botAI) == ai::raidutil::AssignedCurse::TONGUES &&
           CurseOfTonguesTrigger::IsActive();
}

bool AssignedCurseOfAgonyTrigger::IsActive()
{
    return ai::warlock::GetCoordinatedCurse(botAI) == ai::raidutil::AssignedCurse::AGONY &&
           CurseOfAgonyTrigger::IsActive();
}

bool PullImminentLifeTapTrigger::IsActive()
{
    return AI_VALUE(bool, "pull imminent") &&
           AI_VALUE2(uint8, "health", "self target") > sPlayerbotAIConfig.MediumHealth &&
           LifeTapGlyphBuffTrigger::IsActive();
}

// Checks if the target has a conflicting debuff that is equal to Curse of the Elements
bool CurseOfTheElementsTrigger::IsActive()
{
    if (!ai::warlock::IsCurseAllowed(botAI, ai::raidutil::AssignedCurse::ELEMENTS))
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;

    // List of all spell IDs for Ebon Plague, Earth and Moon, and Curse of the Elements
    static const uint32 CurseOfTheElementsExclusiveDebuffs[] = {// Ebon Plague
                                                                51735, 51734, 51726,
                                                                // Earth and Moon (target debuff, not the talent)
                                                                60431, 60432, 60433,
                                                                // Curse of the Elements
                                                                1490, 11721, 11722, 27228, 47865};

    // Check if target has any of the exclusive debuffs
    for (uint32 spellId : CurseOfTheElementsExclusiveDebuffs)
    {
        if (target->HasAura(spellId))
            return false;
    }

    // Missing or expiring, and the target lives long enough for the group to use it
    return DebuffTrigger::IsActive();
}

// Checks if the target has a conflicting debuff that is equal to Curse of Weakness
bool CurseOfWeaknessTrigger::IsActive()
{
    if (!ai::warlock::IsCurseAllowed(botAI, ai::raidutil::AssignedCurse::WEAKNESS))
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;
    // List of all spell IDs for Curse of Weakness, Demoralizing Roar, Demoralizing Shout, and Vindication
    static const uint32 CurseOfWeaknessExclusiveDebuffs[] = {// Curse of Weakness
                                                             702, 1108, 6205, 7646, 11707, 11708, 27224, 30909, 50511,
                                                             // Demoralizing Roar
                                                             99, 1735, 9490, 9747, 9898, 26998, 48559, 48560,
                                                             // Demoralizing Shout
                                                             1160, 6190, 11554, 11555, 11556, 25202, 25203, 47437,
                                                             // Vindication
                                                             67, 26017};

    // Check if target has any of the exclusive debuffs
    for (uint32 spellId : CurseOfWeaknessExclusiveDebuffs)
    {
        if (target->HasAura(spellId))
            return false;
    }
    // [b31_deep] Missing or expiring, and the target lives long enough for the group to use it (as for Curse of the
    // Elements; was BuffTrigger::IsActive without a lifetime gate: a 29-relevance GCD on every dying trash mob).
    return DebuffTrigger::IsActive();
}

struct WarlockPetDef
{
    char const* strategy;  // The strategy string as recognized by the AI (e.g., "imp", "voidwalker", etc.)
    uint32 spellId;        // The spell ID required to summon this pet
    uint32 npcEntry;       // The NPC entry ID for the summoned pet creature
};

// Static array with all relevant Warlock pets and their data
static const WarlockPetDef pets[] = {{"imp", 688, 416},
                                     {"voidwalker", 697, 1860},
                                     {"succubus", 712, 1863},
                                     {"felhunter", 691, 417},
                                     {"felguard", 30146, 17252}};

bool WrongPetTrigger::IsActive()
{
    // Retrieve the bot player and its current pet (if any)
    Player* bot = botAI->GetBot();
    Pet* pet = bot->GetPet();

    // Step 1: Count how many pet strategies are currently enabled for this bot.
    //         While doing so, also remember which pet strategy is the only enabled one (if that's the case).
    int enabledCount = 0;
    WarlockPetDef const* enabledPet =
        nullptr;  // Pointer to the pet definition of the enabled strategy, if only one is enabled
    for (WarlockPetDef const& pd : pets)
    {
        if (botAI->HasStrategy(pd.strategy, BOT_STATE_NON_COMBAT))
        {
            enabledCount++;
            enabledPet = &pd;  // Save the pointer to last enabled pet
        }
    }

    // Step 2: If not exactly one pet strategy is enabled, we should not trigger.
    //         This prevents ambiguous or conflicting situations.
    if (enabledCount != 1)
        return false;

    // Step 3: If there is no pet, do not trigger.
    if (!pet)
        return false;

    // Step 4: At this point, we know only one pet strategy is enabled.
    //         We check if the currently summoned pet matches the enabled strategy.
    bool correctPet = false;
    if (pet)
    {
        CreatureTemplate const* ct = pet->GetCreatureTemplate();
        // Check if the pet's NPC entry matches the expected one for the enabled strategy
        if (ct && ct->Entry == enabledPet->npcEntry)
            correctPet = true;
    }

    // Step 5: If the correct pet is already summoned, the trigger should not activate.
    if (correctPet)
        return false;

    // Step 6: Finally, check if the bot actually knows the spell to summon the desired pet.
    //         If so, the trigger is active (bot should summon the correct pet).
    if (bot->HasSpell(enabledPet->spellId))
        return true;

    // Step 7: If we get here, the bot doesn't know the spell required to support the active pet strategy
    return false;
}

const std::set<uint32> RainOfFireChannelCheckTrigger::RAIN_OF_FIRE_SPELL_IDS = {
    5740,   // Rain of Fire Rank 1
    6219,   // Rain of Fire Rank 2
    11677,  // Rain of Fire Rank 3
    11678,  // Rain of Fire Rank 4
    27212,  // Rain of Fire Rank 5
    47819,  // Rain of Fire Rank 6
    47820   // Rain of Fire Rank 7
};

// [rot472] wowsims warlock__affliction.apl.json channels Drain Soul in the execute phase (isExecutePhase E25) to the
// fight's end, and CastTimeMultiplier counts a channel as worth starting once its first tick lands; the trigger asked
// for 6 s of the target's life (two unhasted ticks). b240 rfc-h 5D: the last 6 s of every boss went to Shadow Bolt
// (4517 a cast in the sim's log) for Drain Soul ticks of 9939 at four times their damage. The first tick: the
// channel's periodic amplitude, hasted as AuraEffect::CalculatePeriodic hastes it (SPELL_AURA_PERIODIC_HASTE on the
// spell or SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC: UNIT_MOD_CAST_SPEED)
float DrainSoulFirstTickSec(PlayerbotAI* botAI, Player* bot)
{
    uint32 const id = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "drain soul")->Get();
    SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
    if (!info)
        return 0.0f;

    int32 amplitude = 0;
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.ApplyAuraName != SPELL_AURA_NONE && effect.Amplitude > 0)
            amplitude = amplitude ? std::min(amplitude, int32(effect.Amplitude)) : int32(effect.Amplitude);
    if (amplitude <= 0)
        return 0.0f;

    if (bot->HasAuraTypeWithAffectMask(SPELL_AURA_PERIODIC_HASTE, info) ||
        info->HasAttribute(SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC))
        amplitude = int32(amplitude * bot->GetFloatValue(UNIT_MOD_CAST_SPEED));
    return float(amplitude) / IN_MILLISECONDS;
}

bool DrainSoulExecuteTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || target->HealthAbovePct(DRAIN_SOUL_EXECUTE_HEALTH_PCT))
        return false;

    float const firstTick = DrainSoulFirstTickSec(botAI, bot);
    float const lifetime = ExpectedLifetime(botAI, target);
    if (firstTick <= 0.0f || lifetime < firstTick)
        return false;

    // one line per target where the 6 s rule would have held the channel
    if (lifetime < 2.0f * firstTick)
    {
        thread_local std::unordered_map<ObjectGuid, ObjectGuid> logged;
        if (logged.size() > 4096)
            logged.clear();
        ObjectGuid& last = logged[bot->GetGUID()];
        if (last != target->GetGUID())
        {
            last = target->GetGUID();
            LOG_DEBUG("playerbots", "[rot472] {} channels Drain Soul on {}: lives {:.1f} s, its first tick in {:.1f} s",
                      bot->GetName(), target->GetName(), lifetime, firstTick);
        }
    }
    return true;
}

bool DrainSoulClipTrigger::IsActive()
{
    constexpr uint32 SPELLFAMILYFLAG0_DRAIN_SOUL = 0x00004000;

    Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (!channel || !channel->m_spellInfo || channel->m_spellInfo->SpellFamilyName != SPELLFAMILY_WARLOCK ||
        !(channel->m_spellInfo->SpellFamilyFlags[0] & SPELLFAMILYFLAG0_DRAIN_SOUL))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.LowMana)
        return false;

    // [rot350] ms until the channel's next tick (the Drain Soul aura on the target, AuraEffect::m_periodicTimer)
    int32 nextTickMs = 0;
    if (Aura* drain = target->GetAura(channel->m_spellInfo->Id, bot->GetGUID()))
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* effect = drain->GetEffect(i); effect && effect->GetAmplitude() > 0)
            {
                nextTickMs = effect->GetPeriodicTimer();
                break;
            }

    uint32 const hauntId = AI_VALUE2(uint32, "spell id", "haunt");
    if (hauntId && !bot->HasSpellCooldown(hauntId) && HauntDue(botAI, bot, target))
    {
        // the debuff left after the next tick still covers Haunt's cast and travel: the tick first
        int32 const refreshInMs = HauntRefreshInMs(botAI, bot, target);
        if (nextTickMs > refreshInMs + int32(HAUNT_REFRESH_SLACK_MS))
        {
            LOG_DEBUG("playerbots", "[rot350] {} clips Drain Soul on {} for Haunt: next tick in {} ms, the debuff's "
                      "slack {} ms", bot->GetName(), target->GetName(), nextTickMs, refreshInMs);
            return true;
        }
    }

    if (!AI_VALUE2(uint32, "spell id", "unstable affliction"))
        return false;

    // [rot352] due at its hasted cast time left (the APL's row 18): the recast then lands as the dot ends, so
    // waiting for the next tick would only let it lapse
    // [rot471] The clip is for the recast the "unstable affliction" trigger makes, so it asks that trigger: its own
    // refresh time, the lifetime and the rot400 payoff against the filler. b240 rfc-h 5D-12 Taragaman 50.3-54.3 s:
    // the clip read health / group dps (>= 9 s) and cancelled the execute Drain Soul for an Unstable Affliction the
    // trigger then held ("[rot400] holds unstable affliction on Taragaman the Hungerer: lives 6.1 s, 1 tick(s)"),
    // and the channel restarted from its first tick (wowsims warlockShouldRecastDrainSoul: the recast is for the
    // refresh the rotation then casts)
    Trigger* unstableAffliction = context->GetTrigger("unstable affliction");
    if (!unstableAffliction || !unstableAffliction->IsActive())
        return false;

    LOG_DEBUG("playerbots", "[rot471] {} clips Drain Soul on {} for Unstable Affliction", bot->GetName(),
              target->GetName());
    return true;
}

bool RainOfFireChannelCheckTrigger::IsActive()
{
    Player* bot = botAI->GetBot();

    // Check if the bot is channeling a spell
    if (Spell* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        // Only trigger if the spell being channeled is Rain of Fire
        if (!spell->m_spellInfo || !RAIN_OF_FIRE_SPELL_IDS.count(spell->m_spellInfo->Id))
            return false;

        // [b31_deep] Count the attackers inside this bot's own Rain of Fire, not within SightDistance of the bot
        // (that radius kept the channel going on one target: 20 cancels in all logs, Haunt "channeling" under RoF).
        DynamicObject* dynObj = bot->GetDynObject(spell->m_spellInfo->Id);
        if (!dynObj)
            return false;

        return !HasEnoughAoeTargets(botAI, dynObj->GetPosition(), dynObj->GetRadius(), minEnemies);
    }

    // Not channeling Rain of Fire
    return false;
}

namespace
{
// [rot566] talent first ranks (Spell.dbc; spell_proc -47201 / -17793 / -63156)
constexpr uint32 SPELL_EVERLASTING_AFFLICTION_RANK_1 = 47201;
constexpr uint32 SPELL_IMPROVED_SHADOW_BOLT_RANK_1 = 17793;
constexpr uint32 SPELL_DECIMATION_RANK_1 = 63156;
constexpr uint32 CRIT_BONUS_PROBE = 10000;  // the core's crit bonus is a function of the damage: read as a ratio

// a talent's proc chance (0..1): its spell_proc entry where it sets one, else Spell.dbc ProcChance; 0 untaken
float TalentChance(Aura const* talent)
{
    if (!talent)
        return 0.0f;
    SpellProcEntry const* entry = sSpellMgr->GetSpellProcEntry(talent->GetId());
    return (entry && entry->Chance > 0.0f ? entry->Chance : static_cast<float>(talent->GetSpellInfo()->ProcChance)) /
           100.0f;
}

// the spell a talent's proc casts (its effects' trigger spell)
SpellInfo const* TalentProc(Aura const* talent)
{
    if (!talent)
        return nullptr;
    for (SpellEffectInfo const& effect : talent->GetSpellInfo()->GetEffects())
        if (effect.TriggerSpell)
            if (SpellInfo const* proc = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                return proc;
    return nullptr;
}
}

// [rot566] Shadow Bolt's value beyond its hit, for Affliction / Demonology (wowsims warlock APLs: Shadow Bolt the
// filler), each where the talent is taken, from the talents and the spells:
// - Everlasting Affliction refreshes our Corruption (spell_warl_everlasting_affliction): a Corruption that runs out
//   before the area's cast ends loses the ticks a refresh would add (to its full duration, at most the target's life);
// - Improved Shadow Bolt applies its crit debuff (Shadow Mastery): one that is missing or runs out before the area
//   ends is down till the next Shadow Bolt, the debuff's crit x the crit bonus on this warlock's filler damage then
//   (its own spells; the other casters' share is not counted);
// - Decimation, the target at or below 35 % (spell_warl_decimation): the faster Soul Fire it gives (the proc's
//   SPELLMOD_CASTING_TIME) over the filler in its cast, per Shadow Bolt.
float WarlockAoePaysTrigger::FillerScale()
{
    std::string const filler = Filler();
    Unit* target = AI_VALUE(Unit*, "current target");
    uint32 const boltId = AI_VALUE2(uint32, "spell id", filler);
    uint32 const areaId = AI_VALUE2(uint32, "spell id", aoeSpell);
    SpellInfo const* boltInfo = boltId ? sSpellMgr->GetSpellInfo(boltId) : nullptr;
    SpellPayoff::Cast bolt;
    SpellPayoff::Cast area;
    if (filler != "shadow bolt" || !target || !boltInfo || !areaId || !SpellPayoff::Read(bot, target, boltId, bolt) ||
        bolt.direct <= 0.0f || !SpellPayoff::Read(bot, target, areaId, area) || area.executeMs <= 0.0f)
        return 1.0f;

    float const fillerDps = bolt.direct * static_cast<float>(IN_MILLISECONDS) / bolt.executeMs;
    float const windowMs = area.executeMs;
    float const lifeMs = ExpectedFightLifetime(botAI, target) * static_cast<float>(IN_MILLISECONDS);
    float perWindow = 0.0f;  // damage lost over the area's cast
    float perCast = 0.0f;    // damage each Shadow Bolt brings

    // Everlasting Affliction: the Corruption refresh
    Aura const* everlasting = bot->GetAuraOfRankedSpell(SPELL_EVERLASTING_AFFLICTION_RANK_1);
    Aura const* corruption = botAI->GetAura("corruption", target, true);
    uint32 const corruptionId = AI_VALUE2(uint32, "spell id", "corruption");
    SpellPayoff::Cast corruptionCast;
    if (everlasting && corruption && corruptionId && corruption->GetDuration() < windowMs &&
        SpellPayoff::Read(bot, target, corruptionId, corruptionCast) && corruptionCast.amplitudeMs)
    {
        float const lostMs = std::min(static_cast<float>(corruption->GetMaxDuration()), lifeMs) -
                             static_cast<float>(corruption->GetDuration());
        if (lostMs > 0.0f)
            perWindow += TalentChance(everlasting) * corruptionCast.tick *
                         std::floor(lostMs / static_cast<float>(corruptionCast.amplitudeMs));
    }

    // Improved Shadow Bolt: its crit debuff's upkeep
    Aura const* improved = bot->GetAuraOfRankedSpell(SPELL_IMPROVED_SHADOW_BOLT_RANK_1);
    if (SpellInfo const* debuff = TalentProc(improved))
    {
        int32 critPct = 0;
        for (SpellEffectInfo const& effect : debuff->GetEffects())
            if (effect.ApplyAuraName == SPELL_AURA_MOD_ATTACKER_SPELL_CRIT_CHANCE ||
                effect.ApplyAuraName == SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE)
                critPct += effect.CalcValue(bot);
        Aura const* up = target->GetAura(debuff->Id);
        float const remainingMs = up ? static_cast<float>(up->GetDuration()) : 0.0f;
        float const downMs = std::min(windowMs, lifeMs) - remainingMs;
        float const bonus =
            static_cast<float>(Unit::SpellCriticalDamageBonus(bot, boltInfo, CRIT_BONUS_PROBE, target)) /
                static_cast<float>(CRIT_BONUS_PROBE) -
            1.0f;
        if (critPct > 0 && downMs > 0.0f && bonus > 0.0f)
            perWindow += TalentChance(improved) * static_cast<float>(critPct) / 100.0f * bonus * fillerDps * downMs /
                         static_cast<float>(IN_MILLISECONDS);
    }

    // Decimation: the faster Soul Fire, the target at or below 35 %
    Aura const* decimation = bot->GetAuraOfRankedSpell(SPELL_DECIMATION_RANK_1);
    SpellInfo const* decimated = TalentProc(decimation);
    uint32 const soulFireId = AI_VALUE2(uint32, "spell id", "soul fire");
    SpellPayoff::Cast soulFire;
    if (decimated && soulFireId && !bot->HasAura(decimated->Id) &&
        target->HasAuraState(AURA_STATE_HEALTHLESS_35_PERCENT, boltInfo, bot) &&
        SpellPayoff::Read(bot, target, soulFireId, soulFire))
    {
        int32 castPct = 0;
        for (SpellEffectInfo const& effect : decimated->GetEffects())
            if (effect.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && effect.MiscValue == SPELLMOD_CASTING_TIME)
                castPct += effect.CalcValue(bot);
        float const castMs =
            std::max(soulFire.gcdMs, soulFire.castMs * static_cast<float>(100 + castPct) / 100.0f);
        float const gain = soulFire.Total() - fillerDps * castMs / static_cast<float>(IN_MILLISECONDS);
        if (gain > 0.0f)
            perCast += TalentChance(decimation) * gain;
    }

    return 1.0f + perCast / bolt.direct + perWindow / (fillerDps * windowMs / static_cast<float>(IN_MILLISECONDS));
}
