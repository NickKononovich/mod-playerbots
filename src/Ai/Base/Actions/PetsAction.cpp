/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PetsAction.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "Group.h"
#include "CreatureAI.h"
#include "InstanceScript.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "InstanceGuid.h"  // [rot44]
#include "Map.h"
#include "MotionMaster.h"
#include "MovementActions.h"
#include "PathGenerator.h"
#include "ReachTargetActions.h"  // [b40_scholo] IdleMobReachingSpot, ReachMeleeAction::TANK_FAR_MOB
#include "Spell.h"
#include "Strategy.h"
#include "TargetValue.h"
#include "TargetedMovementGenerator.h"
#include "ThreatManager.h"
#include "ThreatValues.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

enum PetSpells
{
    PET_PROWL_1 = 24450,
    PET_PROWL_2 = 24452,
    PET_PROWL_3 = 24453,
    PET_COWER = 1742,
    PET_LEAP = 47482,
    PET_SPELL_LOCK_1 = 19244,
    PET_SPELL_LOCK_2 = 19647,
    PET_DEVOUR_MAGIC_1 = 19505,
    PET_DEVOUR_MAGIC_2 = 19731,
    PET_DEVOUR_MAGIC_3 = 19734,
    PET_DEVOUR_MAGIC_4 = 19736,
    PET_DEVOUR_MAGIC_5 = 27276,
    PET_DEVOUR_MAGIC_6 = 27277,
    PET_DEVOUR_MAGIC_7 = 48011,
    PET_SPIRIT_WOLF_LEAP = 58867
};

// Pet taunts, every castable rank (Spell.dbc 3.3.5a, SpellName + spellLevel): they pull mobs off the group's tank.
// Hunter pet Growl; Voidwalker Torment (single target) and Suffering (area); Felguard Anguish (area).
static std::vector<uint32> const petTauntSpells = {
    2649,  14916, 14917, 14918, 14919, 14920, 14921, 27047, 61676,  // Growl 1-9
    3716,  7809,  7810,  7811,  11774, 11775, 27270, 47984,         // Torment 1-8
    17735, 17750, 17751, 17752, 27271, 33701, 47989, 47990,         // Suffering 1-8
    33698, 33699, 33700, 47993                                      // Anguish 1-4
};

// Another member of the bot's group on its map is a tank: the pet's taunt stays off.
static bool GroupHasOtherTank(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsInWorld() && member->GetMap() == bot->GetMap() &&
            PlayerbotAI::IsTank(member))
            return true;
    }

    return false;
}

static std::vector<uint32> disabledPetSpells = {
    PET_PROWL_1, PET_PROWL_2, PET_PROWL_3,
    PET_COWER, PET_LEAP,
    PET_SPELL_LOCK_1, PET_SPELL_LOCK_2,
    PET_DEVOUR_MAGIC_1, PET_DEVOUR_MAGIC_2, PET_DEVOUR_MAGIC_3,
    PET_DEVOUR_MAGIC_4, PET_DEVOUR_MAGIC_5, PET_DEVOUR_MAGIC_6, PET_DEVOUR_MAGIC_7, PET_SPIRIT_WOLF_LEAP
};

bool PetsAction::Execute(Event event)
{
    // Extract the command parameter from the event (e.g., "aggressive", "defensive", "attack", etc.)
    std::string param = event.getParam();
    if (param.empty() && !defaultCmd.empty())
        param = defaultCmd;

    if (param.empty())
    {
        // If no parameter is provided, show usage instructions and return.
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pet_usage_error", "Usage: pet <aggressive|defensive|passive|stance|attack|follow|stay>", {});
        botAI->TellError(text);
        return false;
    }

    Player* bot = botAI->GetBot();

    // Collect all controlled pets and guardians, except totems, into the targets vector.
    std::vector<Creature*> targets;
    Pet* pet = bot->GetPet();
    if (pet)
        targets.push_back(pet);

    for (Unit::ControlSet::const_iterator itr = bot->m_Controlled.begin(); itr != bot->m_Controlled.end(); ++itr)
    {
        Creature* creature = dynamic_cast<Creature*>(*itr);
        if (!creature)
            continue;
        if (pet && creature == pet)
            continue;
        if (creature->IsTotem())
            continue;
        targets.push_back(creature);
    }

    // If no pets or guardians are found, notify and return.
    if (targets.empty())
    {
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pet_no_pet_error", "You have no pet or guardian pet.", {});
        botAI->TellError(text);
        return false;
    }

    ReactStates react;
    std::string stanceText;

    // Handle stance commands: aggressive, defensive, or passive.
    if (param == "aggressive")
    {
        react = REACT_AGGRESSIVE;
        stanceText = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pet_stance_aggressive", "aggressive", {});
    }
    else if (param == "defensive")
    {
        react = REACT_DEFENSIVE;
        stanceText = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pet_stance_defensive", "defensive", {});
    }
    else if (param == "passive")
    {
        react = REACT_PASSIVE;
        stanceText = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pet_stance_passive", "passive", {});
    }
    // The "stance" command simply reports the current stance of each pet/guardian.
    else if (param == "stance")
    {
        for (Creature* target : targets)
        {
            std::string type = target->IsPet() ?
                PlayerbotTextMgr::instance().GetBotTextOrDefault("pet_type_pet", "pet", {}) :
                PlayerbotTextMgr::instance().GetBotTextOrDefault("pet_type_guardian", "guardian", {});
            std::string name = target->GetName();
            std::string stance;
            switch (target->GetReactState())
            {
                case REACT_AGGRESSIVE:
                    stance = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                        "pet_stance_aggressive", "aggressive", {});
                    break;
                case REACT_DEFENSIVE:
                    stance = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                        "pet_stance_defensive", "defensive", {});
                    break;
                case REACT_PASSIVE:
                    stance = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                        "pet_stance_passive", "passive", {});
                    break;
                default:
                    stance = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                        "pet_stance_unknown", "unknown", {});
                    break;
            }
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_stance_report", "Current stance of %type \"%name\": %stance.",
                {{"type", type}, {"name", name}, {"stance", stance}});
            botAI->TellMaster(text);
        }
        return true;
    }
    // The "attack" command forces pets/guardians to attack the master's selected target.
    else if (param == "attack")
    {
        // Try to get the master's selected target.
        Player* master = botAI->GetMaster();
        Unit* targetUnit = nullptr;

        if (master)
        {
            ObjectGuid masterTargetGuid = master->GetTarget();
            if (!masterTargetGuid.IsEmpty())
                targetUnit = botAI->GetUnit(masterTargetGuid);
        }

        // If no valid target is selected, show an error and return.
        if (!targetUnit)
        {
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_no_target_error", "No valid target selected by master.", {});
            botAI->TellError(text);
            return false;
        }
        if (!targetUnit->IsAlive())
        {
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_target_dead_error", "Target is not alive.", {});
            botAI->TellError(text);
            return false;
        }
        if (!bot->IsValidAttackTarget(targetUnit))
        {
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_invalid_target_error", "Target is not a valid attack target for the bot.", {});
            botAI->TellError(text);
            return false;
        }
        if (sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId()) &&
            (targetUnit->IsPlayer() || targetUnit->IsPet()) &&
            (!bot->duel || bot->duel->Opponent != targetUnit))
        {
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_pvp_prohibited_error", "I cannot command my pet to attack players in PvP prohibited areas.", {});
            botAI->TellError(text);
            return false;
        }

        bool didAttack = false;
        // For each controlled pet/guardian, command them to attack the selected target.
        for (Creature* petCreature : targets)
        {
            CharmInfo* charmInfo = petCreature->GetCharmInfo();
            if (!charmInfo)
                continue;

            petCreature->ClearUnitState(UNIT_STATE_FOLLOW);
            // Only command attack if not already attacking the target, or if not currently under command attack.
            if (petCreature->GetVictim() != targetUnit ||
                (petCreature->GetVictim() == targetUnit && !charmInfo->IsCommandAttack()))
            {
                if (petCreature->GetVictim())
                    petCreature->AttackStop();

                if (!petCreature->IsPlayer() && petCreature->ToCreature()->IsAIEnabled)
                {
                    // For AI-enabled creatures (NPC pets/guardians): issue attack command and set flags.
                    charmInfo->SetIsCommandAttack(true);
                    charmInfo->SetIsAtStay(false);
                    charmInfo->SetIsFollowing(false);
                    charmInfo->SetIsCommandFollow(false);
                    charmInfo->SetIsReturning(false);

                    petCreature->ToCreature()->AI()->AttackStart(targetUnit);

                    didAttack = true;
                }
                else  // For charmed player pets/guardians
                {
                    if (petCreature->GetVictim() && petCreature->GetVictim() != targetUnit)
                        petCreature->AttackStop();

                    charmInfo->SetIsCommandAttack(true);
                    charmInfo->SetIsAtStay(false);
                    charmInfo->SetIsFollowing(false);
                    charmInfo->SetIsCommandFollow(false);
                    charmInfo->SetIsReturning(false);

                    petCreature->Attack(targetUnit, true);
                    didAttack = true;
                }
            }
        }
        // Inform the master if the command succeeded or failed.
        if (didAttack && sPlayerbotAIConfig.PetChatCommandDebug == 1)
        {
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_attack_success", "Pet commanded to attack your target.", {});
            botAI->TellMaster(text);
        }
        else if (!didAttack)
        {
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_attack_failed", "Pet did not attack. (Already attacking or unable to attack target)", {});
            botAI->TellError(text);
        }
        return didAttack;
    }
    // The "follow" command makes all pets/guardians follow the bot.
    else if (param == "follow")
    {
        botAI->PetFollow();
        if (sPlayerbotAIConfig.PetChatCommandDebug == 1)
        {
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_follow_success", "Pet commanded to follow.", {});
            botAI->TellMaster(text);
        }
        return true;
    }
    // The "stay" command causes all pets/guardians to stop and stay in place.
    else if (param == "stay")
    {
        for (Creature* target : targets)
        {
            // If not already in controlled motion, stop movement and set to idle.
            bool controlledMotion =
                target->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
            if (!controlledMotion)
            {
                target->StopMovingOnCurrentPos();
                target->GetMotionMaster()->Clear(false);
                target->GetMotionMaster()->MoveIdle();
            }

            CharmInfo* charmInfo = target->GetCharmInfo();
            if (charmInfo)
            {
                // Set charm/pet state flags for "stay".
                charmInfo->SetCommandState(COMMAND_STAY);
                charmInfo->SetIsCommandAttack(false);
                charmInfo->SetIsCommandFollow(false);
                charmInfo->SetIsFollowing(false);
                charmInfo->SetIsReturning(false);
                charmInfo->SetIsAtStay(!controlledMotion);
                charmInfo->SaveStayPosition(controlledMotion);
                if (target->ToPet())
                    target->ToPet()->ClearCastWhenWillAvailable();

                charmInfo->SetForcedSpell(0);
                charmInfo->SetForcedTargetGUID();
            }
        }
        if (sPlayerbotAIConfig.PetChatCommandDebug == 1)
        {
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pet_stay_success", "Pet commanded to stay.", {});
            botAI->TellMaster(text);
        }
        return true;
    }
    // Unknown command: show usage instructions and return.
    else
    {
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pet_unknown_command_error", "Unknown pet command: %param. Use: pet <aggressive|defensive|passive|stance|attack|follow|stay>",
            {{"param", param}});
        botAI->TellError(text);
        return false;
    }

    // For stance commands, apply the chosen stance to all targets.
    for (Creature* target : targets)
    {
        target->SetReactState(react);
        CharmInfo* charmInfo = target->GetCharmInfo();
        if (charmInfo)
            charmInfo->SetPlayerReactState(react);
    }

    // Inform the master of the new stance if debug is enabled.
    if (sPlayerbotAIConfig.PetChatCommandDebug == 1)
    {
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pet_stance_set_success", "Pet stance set to %stance.",
            {{"stance", stanceText}});
        botAI->TellMaster(text);
    }

    return true;
}

bool TogglePetSpellAutoCastAction::Execute(Event /*event*/)
{
    Pet* pet = bot->GetPet();
    if (!pet)
    {
        return false;
    }
    // hack on high level spell after low level initialization
    std::vector<unsigned int> shouldRemove;
    for (unsigned int& m_autospell : pet->m_autospells)
    {
        if (!pet->HasSpell(m_autospell))
        {
            shouldRemove.push_back(m_autospell);
        }
    }
    for (unsigned int spellId : shouldRemove)
    {
        auto autospellItr = std::find(pet->m_autospells.begin(), pet->m_autospells.end(), spellId);
        if (autospellItr != pet->m_autospells.end())
            pet->m_autospells.erase(autospellItr);
    }
    bool toggled = false;
    bool const groupTank = GroupHasOtherTank(bot);
    for (PetSpellMap::const_iterator itr = pet->m_spells.begin(); itr != pet->m_spells.end(); ++itr)
    {
        if (itr->second.state == PETSPELL_REMOVED)
            continue;

        uint32 spellId = itr->first;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !spellInfo->IsAutocastable())
            continue;

        bool shouldApply = true;
        for (uint32 disabledSpell : disabledPetSpells)
        {
            if (spellId == disabledSpell)
            {
                shouldApply = false;
                break;
            }
        }
        // A pet taunt in a group with a tank takes the pack off the tank (players turn it off in groups).
        if (shouldApply && groupTank &&
            std::find(petTauntSpells.begin(), petTauntSpells.end(), spellId) != petTauntSpells.end())
            shouldApply = false;

        bool isAutoCast = false;
        for (unsigned int& m_autospell : pet->m_autospells)
        {
            if (m_autospell == spellId)
            {
                isAutoCast = true;
                break;
            }
        }
        if (shouldApply != isAutoCast)
        {
            pet->ToggleAutocast(spellInfo, shouldApply);
            toggled = true;
        }
    }

    // Debug message if pet spells have been toggled and debug is enabled
    if (toggled && sPlayerbotAIConfig.PetChatCommandDebug == 1)
        botAI->TellMaster("Pet autocast spells have been toggled.");

    return toggled;
}

namespace
{
constexpr float PET_TANK_LEASH = 20.0f;       // the owner's target this close to the tank fights in the tank's pack
constexpr float PET_MELEE_FIGHT = 10.0f;      // ... or in melee with a member (not nuking from its spawn)
constexpr float PET_OWNER_LEASH = 40.0f;      // a pet never runs after a mob further than this from its owner
constexpr float PET_RETURN_DISTANCE = 15.0f;  // a pet with nothing allowed to do this far away is called back
constexpr float PET_HEEL_DISTANCE = 0.5f;     // out of combat a pet follows this close behind its owner
constexpr float PET_HEEL_ANGLE = static_cast<float>(M_PI);

// [pet-quick-engage] A pet sent to a mob before the tank's threat lead is safe: since when it waits, its run ahead.
struct PetApproach
{
    ObjectGuid target;
    uint32 sinceMs = 0;     // first sent to this mob
    uint32 lastSeenMs = 0;  // the wait last looked at
    uint32 movedMs = 0;     // the run ahead last ordered
    bool moving = false;    // the pet holds the approach's stay order
    bool logged = false;
};

// [pet_idle_reach] A pet kept out of an idle mob's reach: at its owner's side away from the mob (a follow at an angle
// of its own), or parked (a stay order) a few yards back where the owner itself stands inside that reach.
struct PetClearSpot
{
    bool parked = false;   // the pet holds the park's stay order
    float angle = 0.0f;    // the follow angle given (relative to the owner's facing), when not parked
    float x = 0.0f;        // the park spot
    float y = 0.0f;
    float z = 0.0f;
    uint32 orderedMs = 0;  // the last follow / park order
    ObjectGuid mob;        // the idle mob it keeps off (logged once per mob)
};

struct PetControlState
{
    std::mutex lock;
    // [rot44] by (instance, GUID): a guardian (Raise Dead ghoul, Army ghouls, Feral Spirits, Shadowfiend, Mirror
    // Image, treants) gets a per-map GUID, so the parallel instances' guardians collided in this one global table
    std::set<InstanceGuid> passivated;                   // pets this control set passive (restored outside)
    std::map<InstanceGuid, ObjectGuid> logged;           // pet -> last victim it was called back from (log once)
    std::map<InstanceGuid, PetApproach> approaches;      // pet -> its wait for the tank's lead
    std::map<InstanceGuid, PetClearSpot> clear;          // pet -> its place out of idle mobs' reach
};

PetControlState& ControlState()
{
    static PetControlState state;
    return state;
}

// A group in a dungeon or a raid. In a raid (or with a raid group) only outside a boss encounter: the raid strategies
// command pets in boss fights themselves (BT, ICC, SSC, Kara, Ulduar), so there the stance is restored. Gruul's Lair
// 25-man 2026-10-24: the raid had no control at all, the defensive Felguard "Flaadhun" of an idle warlock opened the
// fight on a Gronn-Priest 20+ yd from the tank while the tank still held its pull ("unplanned: Rtlocaa").
bool InDungeonGroup(Player* bot)
{
    Map* map = bot->GetMap();
    Group* group = bot->GetGroup();
    if (!map || !map->IsDungeon() || !group || bot->InBattleground())
        return false;

    if (!map->IsRaid() && !group->isRaidGroup())
        return true;

    InstanceScript* instance = bot->GetInstanceScript();
    return !instance || !instance->IsEncounterInProgress();
}

// The bot, a member of its group, or a pet / guardian of one of them.
bool IsGroupUnit(Unit const* unit, Player* bot)
{
    Player const* player = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    return player && (player == bot || player->IsInSameGroupWith(bot));
}

// The mob is in the group's fight: it attacks a player of the group, or a player of the group is on its threat list.
// A mob that fights only pets or guardians is not: the pet woke it, and a pet that joins it starts the fight before
// the tank. Scholomance heroic 2026-10-24, pull 2: the pack the planner had refused came on the shaman's Spirit Wolf
// ("firstMobVictim Spirit Wolf"), the shaman itself never in combat (a creature fighting a pet does not put the
// pet's owner on its threat list), and the old check counted the wolf as the group.
bool FightsGroup(Unit* target, Player* bot)
{
    if (!target->IsInCombat())
        return false;

    if (Player* victim = target->GetVictim() ? target->GetVictim()->ToPlayer() : nullptr;
        victim && (victim == bot || victim->IsInSameGroupWith(bot)))
        return true;

    Group* group = bot->GetGroup();
    for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->GetMap() == bot->GetMap() && target->IsEngagedBy(member))
            return true;
    }

    return false;
}

Player* GroupTank(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    ObjectGuid const mainTank = PlayerbotAI::GetMainTankGuid(group);
    Player* fallback = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap())
            continue;

        if (member->GetGUID() == mainTank)
            return member;

        if (!fallback && PlayerbotAI::IsTank(member))
            fallback = member;
    }

    return fallback;
}

// What the owner attacks: its melee / auto-shot victim, else the target of its harmful cast under way.
Unit* OwnerAttackTarget(Player* bot)
{
    if (Unit* victim = bot->GetVictim())
        return victim;

    for (CurrentSpellTypes const slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
    {
        Spell const* spell = bot->GetCurrentSpell(slot);
        if (!spell || !spell->GetSpellInfo() || spell->GetSpellInfo()->IsPositive())
            continue;

        // By GUID, never the cast's stored pointer: its target may have left the map since (worldserver crash
        // 2026-10-25 04:17, build 14: _IsValidAttackTarget on a freed unit from here, the owner attacked nothing).
        ObjectGuid const guid = spell->m_targets.GetUnitTargetGUID();
        if (guid.IsEmpty() || guid == bot->GetGUID())
            continue;

        Unit* target = ObjectAccessor::GetUnit(*bot, guid);
        if (target && target->IsInWorld() && target->GetMap() == bot->GetMap())
            return target;
    }

    return nullptr;
}

// [b35_sh] A mob on another level that the pet reaches only by a long way round. sh-n 20261026-024524-sh-n-5H-30:
// the hunter stood on the rim of the sewer jump (a 30 yd drop past the closed Grand Warlock Chamber Door 182539)
// when the tank below fought a Creeping Oozeling; "[pet-tank-target] Cat attacks the tank's target (20.1 yd from
// the owner)". The pet's own path ran through the door, Nethekurse's room and the whole sewer from its far end:
// all six Creeping Ooze packs (formations of 6, 36 mobs) woke on it ("[far engage] ... engaged Cat") and came to
// the tank, who died with 37 attackers. Only above PET_OTHER_LEVEL_DZ (a path is built then, cached per pet).
constexpr float PET_OTHER_LEVEL_DZ = 10.0f;
constexpr float PET_DETOUR_FACTOR = 2.5f;  // a path longer than this times the straight distance ...
constexpr float PET_DETOUR_SLACK = 15.0f;  // ... plus this is a way round
constexpr uint32 PET_DETOUR_MEMO_MS = 2 * IN_MILLISECONDS;

bool PetDetours(Creature* pet, Unit* target)
{
    float const straight = pet->GetExactDist(target);
    if (std::fabs(pet->GetPositionZ() - target->GetPositionZ()) <= PET_OTHER_LEVEL_DZ)
        return false;

    struct Memo
    {
        ObjectGuid target;
        uint32 ms = 0;
        bool detours = false;
    };
    thread_local std::map<InstanceGuid, Memo> memos;  // [rot44] by instance (a guardian's GUID is per map)
    uint32 const now = getMSTime();
    Memo& memo = memos[InstanceKey(pet)];
    if (memo.ms && memo.target == target->GetGUID() && getMSTimeDiff(memo.ms, now) < PET_DETOUR_MEMO_MS)
        return memo.detours;

    PathGenerator path(pet);
    path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    // no path / an incomplete one: not judged here (the chase goes as before)
    bool const detours = !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) &&
                         path.getPathLength() > straight * PET_DETOUR_FACTOR + PET_DETOUR_SLACK;
    memo.target = target->GetGUID();
    memo.ms = now ? now : 1;
    if (detours && !memo.detours)
        LOG_INFO("playerbots", "[b35_sh] pet_detour: {} skips {}: {:.1f} yd straight, dz {:.1f}, path {:.1f} yd",
                 pet->GetName(), target->GetName(), straight, pet->GetPositionZ() - target->GetPositionZ(),
                 path.getPathLength());
    memo.detours = detours;
    return detours;
}

// [b35_sh] A pet left behind (on the rim of a jump link, another level with no way down but the long way round) is
// brought back as a player would, through the normal cast with its cost and cooldown: the hunter casts Dismiss Pet
// (Call Pet follows from the "hunters pet dismissed" / "no pet" triggers), the warlock casts the summon of the same
// demon (the new one replaces the old at the owner), the death knight dismisses the ghoul from the pet bar only
// while Raise Dead is off cooldown ("no pet" -> raise dead). Out of combat only; otherwise the pet follows as before.
constexpr float PET_STRANDED_DISTANCE = 40.0f;
constexpr uint32 PET_STRANDED_CHECK_MS = 2 * IN_MILLISECONDS;
constexpr uint32 SPELL_DISMISS_PET = 2641;

uint32 WarlockSummonSpell(uint32 entry)
{
    switch (entry)
    {
        case 416:   return 688;    // Imp
        case 1860:  return 697;    // Voidwalker
        case 1863:  return 712;    // Succubus
        case 417:   return 691;    // Felhunter
        case 17252: return 30146;  // Felguard
        default:    return 0;
    }
}

bool RecallStrandedPet(PlayerbotAI* botAI, Creature* pet)
{
    Player* bot = botAI->GetBot();
    if (pet->GetGUID() != bot->GetPetGUID() || !pet->IsAlive() || bot->IsInCombat() || pet->IsInCombat() ||
        bot->isMoving() || bot->IsNonMeleeSpellCast(false) || bot->GetMap() != pet->GetMap())
        return false;

    thread_local std::unordered_map<ObjectGuid, uint32> lastCheck;
    uint32 const now = getMSTime();
    uint32& last = lastCheck[bot->GetGUID()];
    if (last && getMSTimeDiff(last, now) < PET_STRANDED_CHECK_MS)
        return false;
    last = now ? now : 1;

    float const distance = pet->GetExactDist(bot);
    bool stranded = distance > PET_STRANDED_DISTANCE;
    if (!stranded && std::fabs(pet->GetPositionZ() - bot->GetPositionZ()) > PET_OTHER_LEVEL_DZ)
    {
        PathGenerator path(pet);
        path.CalculatePath(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        stranded = (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) ||
                   path.getPathLength() > distance * PET_DETOUR_FACTOR + PET_DETOUR_SLACK;
    }
    if (!stranded)
        return false;

    std::string how;
    bool dismissGhoul = false;
    switch (bot->getClass())
    {
        case CLASS_HUNTER:
            if (botAI->CanCastSpell(SPELL_DISMISS_PET, bot) && botAI->CastSpell(SPELL_DISMISS_PET, bot))
                how = "Dismiss Pet";
            break;
        case CLASS_WARLOCK:
            if (uint32 const summon = WarlockSummonSpell(pet->GetEntry());
                summon && botAI->CanCastSpell(summon, bot) && botAI->CastSpell(summon, bot))
                how = "re-summon";
            break;
        case CLASS_DEATH_KNIGHT:
        {
            uint32 const raiseDead = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "raise dead")->Get();
            if (raiseDead && bot->HasSpell(raiseDead) && !bot->HasSpellCooldown(raiseDead))
            {
                dismissGhoul = true;
                how = "dismiss the ghoul, Raise Dead off cooldown";
            }
            break;
        }
        default:
            break;
    }
    if (how.empty())
        return false;

    LOG_INFO("playerbots", "[b35_sh] pet_stranded: {}: {} {:.1f} yd away (dz {:.1f}) -> {}", bot->GetName(),
             pet->GetName(), distance, pet->GetPositionZ() - bot->GetPositionZ(), how);
    if (dismissGhoul)
        bot->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT);  // the pet bar's Dismiss (the ghoul: no hunter pet slot)
    return true;
}

// The pet may fight `target`: the group's tank has engaged it (it is on the mob's threat list or attacks it), and it
// fights by the tank or in melee with a member, near the owner. Without a tank in the group, any mob in the group's
// fight. DPS and their pets never open a fight before the tank: a mob that hits the hunter (a patrol walking into the
// group, Drak'Tharon 2026-10-24 pull 7) is the tank's first, the pet goes once the tank holds it.
// [pet-tank-target] Sunken Temple 2026-10-25 (st-n-5A-92): 15 times "[pet control] Spider called back from <the
// tank's target> ... owner attacks nothing, stance was passive". The owner was still in its non-combat engine the
// first 2-4 s of the pull (following, drinking, round a corner), so a pet on the tank's mob was called back and the
// pet's damage was lost with the hunter's. The tank's own fight does not wait for the owner to be in combat
// (`ownerInCombat` false: the tank's target).
bool IsAllowedPetTarget(Player* bot, Creature* pet, Unit* target, bool ownerInCombat = true)
{
    if (!target || !target->IsInWorld() || target->GetMap() != bot->GetMap() || (ownerInCombat && !bot->IsInCombat()) ||
        !target->IsAlive() || !bot->IsValidAttackTarget(target) ||
        target->HasBreakableByDamageCrowdControlAura() || bot->GetExactDist(target) > PET_OWNER_LEASH ||
        !FightsGroup(target, bot) || !pet->CanCreatureAttack(target) || PetDetours(pet, target))  // [b35_sh]
        return false;

    // [pet-target-exclusion] An encounter keeps the group's pets off some mobs (TargetValueExclusionType::PetTarget):
    // mgt-h 2026-10-25 build 25, a hunter's Crab ran into Selin Fireheart's room after a Wretched by her door while
    // her room was being cleared, became her victim and the group followed it in with 6 Wretched alive (wipe).
    if (GatherStrategyTargetExclusions(GET_PLAYERBOT_AI(bot), TargetValueExclusionType::PetTarget)
            .count(target->GetGUID()))
        return false;

    Player* tank = GroupTank(bot);
    if (!tank)
        return true;

    if (!target->IsEngagedBy(tank) && tank->GetVictim() != target)
        return false;

    // [b40_scholo] The tank's idle-pack guard for the pet too: a mob farther than TANK_FAR_MOB from the tank, standing
    // in the aggro reach of an idle mob, is left to the taunts. scholo-n 20261026-060218-scholo-n-5H-4 (1916-1942 s):
    // in pull 28 (a planned los pull of a Scholomance Adept, 4 expected) the tank refused to walk to a Scholomance
    // Necromancer at (180.9,109.4) ("[tank reach] no walk ... aggro reach of idle Diseased Ghoul (14.4 yd)"). The
    // Spider ran to it ("[pet-tank-target] ... Scholomance Necromancer (32.1 yd from the owner)"). The Ghoul /
    // Aberration / Corpse / Necrofiend pack at (185-191,84-85,104.5) woke on the Spider ("[far engage] ... engaged
    // Spider", 46 yd), came up the ramp onto the healer (12 attackers) and killed the priest, the tank and the hunter.
    if (Creature* creature = target->ToCreature(); creature && !creature->IsDungeonBoss() &&
                                                   GET_PLAYERBOT_AI(bot)->IsInNonRaidDungeon() &&
                                                   target->GetExactDist(tank) > ReachMeleeAction::TANK_FAR_MOB)
        if (Creature* idle = IdleMobReachingSpot(GET_PLAYERBOT_AI(bot), bot, target))
        {
            thread_local std::map<InstanceGuid, uint32> idleLog;  // [rot44]
            uint32 const now = getMSTime();
            uint32& last = idleLog[InstanceKey(pet)];
            if (!last || getMSTimeDiff(last, now) >= 10 * IN_MILLISECONDS)
            {
                last = now ? now : 1;
                LOG_INFO("playerbots", "[b40_scholo] pet_idle_pack_target: {}: {} stays off {} ({:.1f} yd from the "
                         "tank): its spot is in the aggro reach of idle {} ({:.1f} yd from it)", bot->GetName(),
                         pet->GetName(), target->GetName(), target->GetExactDist(tank), idle->GetName(),
                         idle->GetExactDist(target));
            }
            return false;
        }

    if (target->GetExactDist(tank) <= PET_TANK_LEASH)
        return true;

    Unit* victim = target->GetVictim();
    return victim && victim->IsPlayer() && IsGroupUnit(victim, bot) && target->GetExactDist(victim) <= PET_MELEE_FIGHT;
}

// The pet follows at its owner's heel (the move PET_HEEL_DISTANCE behind it), not PET_FOLLOW_DIST off its side.
bool AtHeel(Creature* pet)
{
    MotionMaster* motion = pet->GetMotionMaster();
    if (motion->empty())
        return false;

    MovementGenerator* top = motion->top();
    return top && top->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
           static_cast<FollowMovementGenerator<Creature>*>(top)->GetFollowRange() == PET_HEEL_DISTANCE;
}

// Out of combat a following pet walks at its owner's heel. The owner's own moves keep GUARD_AGGRO_MARGIN (2 yd) outside
// the aggro reach of idle mobs ("[follow path] Rtshaas: follow ... would wake Scholomance Neophyte (21.2 yd of reach
// 23.0) -> trail"); the pet stood PET_FOLLOW_DIST (2 yd) off the owner's side and spent the whole margin, so the
// refused pack woke on the Spirit Wolf. A pet in a scripted move (charge, leap, fear) is left to it.
void KeepAtHeel(Player* bot, Creature* pet)
{
    MotionMaster* motion = pet->GetMotionMaster();
    if (AtHeel(pet) || motion->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
        return;

    // PetAI::HandleReturnMovement re-issues its own follow only for a pet neither following nor returning
    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!charmInfo->IsFollowing())
        charmInfo->SetIsReturning(true);

    motion->MoveFollow(bot, PET_HEEL_DISTANCE, PET_HEEL_ANGLE);
}

// A guardian without a pet bar (Army of the Dead, Snake Trap, Mirror Image, Ebon Gargoyle, Dancing Rune Weapon) runs
// its own script and has no stance or command state: it is only stopped on a mob nobody of the group fights (the one
// it woke itself) and sent back to its owner.
void ControlGuardian(Player* bot, Creature* guardian)
{
    if (guardian->GetOwnerGUID() != bot->GetGUID() || guardian->IsCharmed() ||
        !guardian->HasUnitTypeMask(UNIT_MASK_GUARDIAN))
        return;

    Unit* victim = guardian->GetVictim();
    if (!victim || FightsGroup(victim, bot))
        return;

    bool log = false;
    {
        PetControlState& state = ControlState();
        std::lock_guard<std::mutex> guard(state.lock);
        ObjectGuid& last = state.logged[InstanceKey(guardian)];
        log = last != victim->GetGUID();
        last = victim->GetGUID();
    }

    if (log)
        LOG_INFO("playerbots", "[pet control] {}: guardian {} stopped on {} ({:.1f} yd from the owner, not in the "
                 "group's fight)", bot->GetName(), guardian->GetName(), victim->GetName(), victim->GetExactDist(bot));

    guardian->AttackStop();
    guardian->InterruptNonMeleeSpells(false);
    guardian->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, guardian->GetFollowAngle());
}

void CommandPetAttack(Creature* pet, Unit* target)
{
    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (pet->GetVictim() == target && charmInfo->IsCommandAttack())
        return;

    // the order the client's Attack button gives (WorldSession::HandlePetActionHelper, COMMAND_ATTACK)
    pet->ClearUnitState(UNIT_STATE_FOLLOW);
    pet->AttackStop();
    charmInfo->SetIsCommandAttack(true);
    charmInfo->SetIsAtStay(false);
    charmInfo->SetIsFollowing(false);
    charmInfo->SetIsCommandFollow(false);
    charmInfo->SetIsReturning(false);
    pet->AI()->AttackStart(target);
}

void CommandPetFollow(Player* bot, Creature* pet)
{
    // the client's Follow button (COMMAND_FOLLOW)
    CharmInfo* charmInfo = pet->GetCharmInfo();
    pet->AttackStop();
    pet->InterruptNonMeleeSpells(false);
    pet->ClearInPetCombat();
    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    charmInfo->SetCommandState(COMMAND_FOLLOW);
    charmInfo->SetIsCommandAttack(false);
    charmInfo->SetIsAtStay(false);
    charmInfo->SetIsReturning(true);
    charmInfo->SetIsCommandFollow(true);
    charmInfo->SetIsFollowing(false);
    charmInfo->RemoveStayPosition();
}

// [pet-quick-engage] User 2026-10-25: the pet engages fast but never takes the mob off the tank. A pet sent to the
// tank's mob waits for the tank's threat lead as a dps does ([threat-open], ThreatLead): its threat after the next
// PET_NEXT_CAST_SHARE of its owner's next cast under OPEN_SHARE (90 %) of the holder's, or PET_OPEN_MAX_WAIT_MS
// since it was first sent there (a stunned tank builds none). Meanwhile it runs ahead to the mob's far side from the
// tank (the hunter shoots from range, the pet has the way to walk) so it bites the moment the lead is there. The
// approach is a stay order at the spot; it is lifted (follow again) whenever the pet does anything else.
constexpr float PET_NEXT_CAST_SHARE = 0.4f;            // a pet's damage against its owner's
constexpr uint32 PET_OPEN_MAX_WAIT_MS = 3 * IN_MILLISECONDS;
constexpr uint32 PET_WAIT_FORGET_MS = 1500;            // a wait not looked at this long starts anew
constexpr float PET_APPROACH_GAP = 1.0f;               // the spot: this far past the mob's hitbox, behind it
constexpr uint32 PET_APPROACH_REISSUE_MS = IN_MILLISECONDS;
constexpr float PET_APPROACH_IDLE_MARGIN = 3.0f;       // the spot and the run keep this far off an idle mob's reach
constexpr int PET_APPROACH_RUN_SAMPLES = 4;            // the straight run there is sampled this often

PetApproach* FindApproach(PetControlState& state, Creature* pet)
{
    auto const found = state.approaches.find(InstanceKey(pet));
    return found == state.approaches.end() ? nullptr : &found->second;
}

// The pet does something else than running ahead (it attacks, follows, holds): the stay order of the approach is
// lifted. `forget`: the wait itself is over too (the pet attacks).
void EndApproach(PetControlState& state, Creature* pet, bool forget)
{
    bool restore = false;
    {
        std::lock_guard<std::mutex> guard(state.lock);
        PetApproach* approach = FindApproach(state, pet);
        if (!approach)
            return;

        restore = approach->moving;
        approach->moving = false;
        approach->movedMs = 0;
        if (forget)
            state.approaches.erase(InstanceKey(pet));
    }

    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (restore && charmInfo && charmInfo->HasCommandState(COMMAND_STAY))
    {
        charmInfo->SetCommandState(COMMAND_FOLLOW);
        charmInfo->RemoveStayPosition();
        charmInfo->SetIsAtStay(false);
    }
}

// Whether the pet may bite `target` now: no tank, it already has threat there, the tank's lead is safe, or it waited
// PET_OPEN_MAX_WAIT_MS. `why` says which (or what it waits for).
bool PetMayEngage(PlayerbotAI* botAI, Creature* pet, Unit* target, PetControlState& state, std::string& why)
{
    Player* bot = botAI->GetBot();
    if (!GroupTank(bot))
    {
        why = "no tank in the group";
        return true;
    }

    float const petThreat = target->GetThreatMgr().GetThreat(pet);
    if (petThreat > 0.0f)
    {
        why = "already on it";
        return true;
    }

    float const holder = ThreatLead::HolderThreat(bot, target);
    float const next =
        botAI->GetAiObjectContext()->GetValue<float>("next cast threat")->Get() * PET_NEXT_CAST_SHARE;
    float const share = holder > 0.0f ? (petThreat + next) / holder : -1.0f;
    if (share >= 0.0f && share <= ThreatLead::OPEN_SHARE)
    {
        why = Acore::StringFormat("safe threat lead: next bite {:.0f} of the tank's {:.0f} ({:.0f} %)", next, holder,
                                  share * 100.0f);
        return true;
    }

    uint32 const now = getMSTime();
    uint32 waited = 0;
    {
        std::lock_guard<std::mutex> guard(state.lock);
        PetApproach& approach = state.approaches[InstanceKey(pet)];
        if (approach.target != target->GetGUID() || getMSTimeDiff(approach.lastSeenMs, now) > PET_WAIT_FORGET_MS)
        {
            approach.target = target->GetGUID();
            approach.sinceMs = now ? now : 1;
            approach.movedMs = 0;
            approach.logged = false;
        }

        approach.lastSeenMs = now ? now : 1;
        waited = getMSTimeDiff(approach.sinceMs, now);
    }

    if (waited >= PET_OPEN_MAX_WAIT_MS)
    {
        why = Acore::StringFormat("waited the longest ({} ms)", waited);
        return true;
    }

    why = holder > 0.0f ? Acore::StringFormat("the tank's lead: next bite {:.0f} of {:.0f} ({:.0f} %)", next, holder,
                                              share * 100.0f)
                        : std::string("the tank has no threat on it yet");
    return false;
}

// An idle aggressive mob whose aggro the pet standing at the point would reach.
bool PetSpotWakesIdle(PlayerbotAI* botAI, Creature* pet, float x, float y, float z)
{
    Player* bot = botAI->GetBot();
    GuidVector const possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    for (ObjectGuid const& guid : possible)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
            mob->GetMap() != bot->GetMap() || !bot->IsHostileTo(mob))
            continue;

        if (!MovementAction::IdleAggroBlocker(mob, pet, x, y, z, PET_APPROACH_IDLE_MARGIN, false))
            return true;
    }

    return false;
}

// [pet_idle_reach] scholo-h 2026-10-25 15:42:05 (build 28, pull 4 "unplanned: Rthunaa", firstMobVictim "Wind
// Serpent"): a Risen Guard behind the closed Iron Gate 175612 woke on the hunter's pet. The gate is a lattice: aggro
// and sight pass its bars, so the reach counted here takes no line of sight (doors and bars included). Out of combat
// in a dungeon a pet stands at its owner's side or behind it, and never inside the aggro reach of an idle mob: it
// keeps PET_IDLE_REACH_MARGIN outside it. Where the owner itself stands inside such a reach (the tank held 13.9 yd
// from that Risen Guard), the pet parks a few yards back, out of it.
constexpr float PET_IDLE_REACH_MARGIN = 1.0f;
constexpr float PET_SIDE_DISTANCE = 2.0f;       // the side spots: this far from the owner
constexpr float PET_PARK_MAX_DISTANCE = 12.0f;  // a park spot at most this far from the owner
constexpr float PET_PARK_RING_STEP = 2.0f;
constexpr int PET_PARK_ANGLES = 16;
constexpr float PET_PARK_MAX_DZ = 3.0f;  // a park spot on the owner's floor
constexpr uint32 PET_CLEAR_REISSUE_MS = IN_MILLISECONDS;
constexpr float PET_PARK_SAME_SPOT = 2.0f;
constexpr uint32 PET_CLEAR_TURN_MS = 500;

// [b34_petramp] scholo-h 20261026-014521-scholo-h-5H-3 (~133 s, pull 5, firstMobVictim "Cat"): the hunter ran ~35 yd
// from its rest down the Reliquary ramp after the tank; its Cat stood 5 yd AHEAD of it (218.3,52.1,123.9, the hunter
// at 213.4,50.8,127.2) and woke a Spectral Researcher 24.9 yd off ("[far engage]", dz 8.1) while the tank, 21.7 yd
// from it, stayed out of its 21 yd - a pet's aggro range is its own (Creature::GetAggroRange by the PET's level). No
// "[pet idle reach]" line: only the heel spot behind the owner was tested, and it was clear. The core's follow of a
// moving owner aims at the owner's place 0.5 s ahead (FollowMovementGenerator PredictPosition) and catches up at up
// to 2.6x its speed, so a "heel" pet runs in front of a walking owner. brd-n 20261026-000434-brd-n-5F-6 (3011 s, pull
// 59, firstMobVictim "Gorilla", the route past the closed Bar Door 170571) the same on the walk. While the owner moves,
// the spot the core really sends the pet to and the pet's own place are tested too (PET_MOVING_MARGIN for the tick's
// delay); when one is near a reach the pet trails PET_TRAIL_DISTANCE behind (past the lead), as a player's pet on
// follow behind a walking hunter, and keeps trailing while the owner moves within PET_TRAIL_KEEP of the margin.
constexpr bool PET_TRAIL_WHILE_MOVING = true;  // [b34_petramp] false: the build-33 heel test only
constexpr float PET_MOVING_MARGIN = 3.0f;
constexpr float PET_TRAIL_DISTANCE = 4.5f;      // the follow range behind a moving owner (lead 3.5 yd at run speed)
constexpr float PET_TRAIL_KEEP = 10.0f;         // a trailing pet keeps trailing while this close to the margin
constexpr float PET_TRAIL_ANGLE_MARK = 10.0f;   // PetClearSpot::angle of a trail follow (no real side angle)

// [b34_petramp] Where the core's follow sends the pet behind a moving owner: the owner's place 0.5 s ahead (as
// FollowMovementGenerator's PredictPosition), `back` yd behind that along its facing. False: the owner does not move.
bool MovingFollowSpot(Player* bot, Creature* pet, float back, float& lead, float& x, float& y, float& z)
{
    MovementInfo const& info = bot->m_movementInfo;
    if (!info.HasMovementFlag(MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD | MOVEMENTFLAG_STRAFE_LEFT |
                              MOVEMENTFLAG_STRAFE_RIGHT))
        return false;

    lead = bot->GetSpeed(info.GetSpeedType()) * 0.5f;
    float const o = bot->GetOrientation();
    x = bot->GetPositionX();
    y = bot->GetPositionY();
    if (info.HasMovementFlag(MOVEMENTFLAG_FORWARD))
    {
        x += std::cos(o) * lead;
        y += std::sin(o) * lead;
    }
    else if (info.HasMovementFlag(MOVEMENTFLAG_BACKWARD))
    {
        x -= std::cos(o) * lead;
        y -= std::sin(o) * lead;
    }

    if (info.HasMovementFlag(MOVEMENTFLAG_STRAFE_LEFT))
    {
        x += std::cos(o + static_cast<float>(M_PI) / 2.0f) * lead;
        y += std::sin(o + static_cast<float>(M_PI) / 2.0f) * lead;
    }
    else if (info.HasMovementFlag(MOVEMENTFLAG_STRAFE_RIGHT))
    {
        x += std::cos(o - static_cast<float>(M_PI) / 2.0f) * lead;
        y += std::sin(o - static_cast<float>(M_PI) / 2.0f) * lead;
    }

    x -= std::cos(o) * back;
    y -= std::sin(o) * back;
    z = bot->GetPositionZ();
    pet->UpdateAllowedPositionZ(x, y, z);
    return true;
}

// [b34_petramp] The pet trails its owner (KeepOutOfIdleReach's trail follow is its motion).
bool TrailFollowing(Creature* pet)
{
    MotionMaster* motion = pet->GetMotionMaster();
    if (motion->empty())
        return false;

    MovementGenerator* top = motion->top();
    return top && top->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
           static_cast<FollowMovementGenerator<Creature>*>(top)->GetFollowRange() >= PET_TRAIL_DISTANCE - 0.01f;
}

// The idle aggressive hostile mobs around the owner (no line of sight: bars and doors pass aggro).
std::vector<Creature*> IdleAggroMobs(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    std::vector<Creature*> mobs;
    GuidVector const possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    for (ObjectGuid const& guid : possible)
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->IsInEvadeMode() ||
            !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetMap() != bot->GetMap() || !bot->IsHostileTo(mob))
            continue;

        mobs.push_back(mob);
    }

    return mobs;
}

// How far outside the nearest idle reach the pet would stand at the point (negative: inside it, by that much).
float IdleReachClearance(std::vector<Creature*> const& mobs, Creature* pet, float x, float y, float z,
                         Creature** worst = nullptr)
{
    float best = std::numeric_limits<float>::max();
    for (Creature* mob : mobs)
    {
        if (!MovementAction::PassesAggroZGate(mob, pet, z))
            continue;

        float const clearance =
            mob->GetExactDist(x, y, z) - MovementAction::IdleAggroReach(mob, pet, PET_IDLE_REACH_MARGIN);
        if (clearance < best)
        {
            best = clearance;
            if (worst)
                *worst = mob;
        }
    }

    return best;
}

// The pet's place out of idle reach is given up (a fight, no control, a clear heel): a park's stay order is lifted.
void EndClearSpot(PetControlState& state, Creature* pet)
{
    bool restore = false;
    {
        std::lock_guard<std::mutex> guard(state.lock);
        auto const found = state.clear.find(InstanceKey(pet));
        if (found == state.clear.end())
            return;

        restore = found->second.parked;
        state.clear.erase(found);
    }

    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (restore && charmInfo && charmInfo->HasCommandState(COMMAND_STAY))
    {
        charmInfo->SetCommandState(COMMAND_FOLLOW);
        charmInfo->RemoveStayPosition();
        charmInfo->SetIsAtStay(false);
        charmInfo->SetIsReturning(true);
    }
}

// The pet follows its owner at PET_SIDE_DISTANCE (the side follow of KeepOutOfIdleReach is still its motion).
bool SideFollowing(Creature* pet)
{
    MotionMaster* motion = pet->GetMotionMaster();
    if (motion->empty())
        return false;

    MovementGenerator* top = motion->top();
    return top && top->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
           static_cast<FollowMovementGenerator<Creature>*>(top)->GetFollowRange() == PET_SIDE_DISTANCE;
}

// Records the place given; true when it is a new one (or the old one's motion was lost).
bool NoteClearSpot(PetControlState& state, Creature* pet, Creature* worst, bool parked, float angle, float x, float y,
                   float z, bool force, bool& newMob)
{
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(state.lock);
    PetClearSpot& spot = state.clear[InstanceKey(pet)];
    // the order again only when its motion was lost (`force`), and not more often than PET_CLEAR_REISSUE_MS
    bool const due = !spot.orderedMs || (force && getMSTimeDiff(spot.orderedMs, now) >= PET_CLEAR_REISSUE_MS);
    bool const same = spot.orderedMs && spot.parked == parked &&
                      (parked ? std::hypot(spot.x - x, spot.y - y) < PET_PARK_SAME_SPOT
                              : std::fabs(spot.angle - angle) < 0.01f);
    newMob = worst && spot.mob != worst->GetGUID();
    if (same && !due)
        return false;

    // a side follow turns with its owner: a new angle at most every PET_CLEAR_TURN_MS (no jitter on a walk)
    if (!same && !parked && !spot.parked && spot.orderedMs && !force &&
        getMSTimeDiff(spot.orderedMs, now) < PET_CLEAR_TURN_MS)
        return false;

    spot.parked = parked;
    spot.angle = angle;
    spot.x = x;
    spot.y = y;
    spot.z = z;
    spot.orderedMs = now ? now : 1;
    if (worst)
        spot.mob = worst->GetGUID();
    return true;
}

// Out of combat: keeps the pet out of every idle mob's reach. True when it placed the pet (a side follow or a park),
// false when the pet's heel spot is clear (the caller keeps it at heel).
bool KeepOutOfIdleReach(PlayerbotAI* botAI, Creature* pet, PetControlState& state)
{
    Player* bot = botAI->GetBot();
    CharmInfo* charmInfo = pet->GetCharmInfo();
    MotionMaster* motion = pet->GetMotionMaster();
    if (!charmInfo || motion->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
        return false;

    std::vector<Creature*> const mobs = IdleAggroMobs(botAI);
    float const facing = bot->GetOrientation();
    auto spotAt = [&](float relAngle, float distance, float& x, float& y, float& z)
    {
        x = bot->GetPositionX() + std::cos(facing + relAngle) * distance;
        y = bot->GetPositionY() + std::sin(facing + relAngle) * distance;
        z = bot->GetPositionZ();
        pet->UpdateAllowedPositionZ(x, y, z);
    };

    float hx, hy, hz;
    spotAt(PET_HEEL_ANGLE, PET_HEEL_DISTANCE, hx, hy, hz);
    // [b34_petramp] a moving owner: the spot the core's follow really sends the pet to (0.5 s ahead of the owner) and
    // the pet's own place, PET_MOVING_MARGIN out of every reach; a pet trailing already keeps it up to PET_TRAIL_KEEP
    bool trailNeeded = false;
    float lead = 0.0f;
    if (PET_TRAIL_WHILE_MOVING && !mobs.empty())
    {
        float fx, fy, fz;
        if (MovingFollowSpot(bot, pet, PET_HEEL_DISTANCE + pet->GetCombatReach(), lead, fx, fy, fz))
        {
            float const need = PET_MOVING_MARGIN + (TrailFollowing(pet) ? PET_TRAIL_KEEP : 0.0f);
            trailNeeded = IdleReachClearance(mobs, pet, fx, fy, fz) < need ||
                          IdleReachClearance(mobs, pet, pet->GetPositionX(), pet->GetPositionY(),
                                             pet->GetPositionZ()) < need;
        }
    }

    if (mobs.empty() || (!trailNeeded && IdleReachClearance(mobs, pet, hx, hy, hz) >= 0.0f))
    {
        bool placed = false;
        {
            std::lock_guard<std::mutex> guard(state.lock);
            placed = state.clear.count(InstanceKey(pet)) > 0;
        }

        if (placed)
        {
            EndClearSpot(state, pet);
            KeepAtHeel(bot, pet);
        }

        return false;
    }

    bool wasParked = false;
    {
        std::lock_guard<std::mutex> guard(state.lock);
        auto const found = state.clear.find(InstanceKey(pet));
        wasParked = found != state.clear.end() && found->second.parked;
    }

    Creature* worst = nullptr;
    IdleReachClearance(mobs, pet, hx, hy, hz, &worst);

    // [b34_petramp] behind a moving owner: a follow PET_TRAIL_DISTANCE back (more than the core's 0.5 s lead), on the
    // owner's own track, while that spot is out of every reach (else the side / park places below)
    if (trailNeeded)
    {
        float const range = std::max(PET_TRAIL_DISTANCE, lead + PET_HEEL_DISTANCE + PET_IDLE_REACH_MARGIN);
        float rx, ry, rz;
        spotAt(PET_HEEL_ANGLE, range, rx, ry, rz);
        if (IdleReachClearance(mobs, pet, rx, ry, rz) >= 0.0f)
        {
            Creature* nearMob = worst;
            float const petClearance = IdleReachClearance(mobs, pet, pet->GetPositionX(), pet->GetPositionY(),
                                                          pet->GetPositionZ(), &nearMob);
            bool newMob = false;
            if (!NoteClearSpot(state, pet, nearMob, false, PET_TRAIL_ANGLE_MARK, 0.0f, 0.0f, 0.0f, !TrailFollowing(pet),
                               newMob))
                return true;

            if (wasParked && charmInfo->HasCommandState(COMMAND_STAY))
            {
                charmInfo->SetCommandState(COMMAND_FOLLOW);
                charmInfo->RemoveStayPosition();
                charmInfo->SetIsAtStay(false);
            }

            if (!charmInfo->IsFollowing())
                charmInfo->SetIsReturning(true);
            motion->MoveFollow(bot, range, PET_HEEL_ANGLE);
            if (newMob && nearMob)
                LOG_INFO("playerbots", "[pet idle reach] {}: {} trails {:.1f} yd behind its moving owner, out of the "
                         "reach of idle {} (the pet {:.1f} yd from it, {:.1f} yd outside its reach) [b34_petramp]",
                         bot->GetName(), pet->GetName(), range, nearMob->GetName(), nearMob->GetExactDist(pet),
                         petClearance);
            return true;
        }
    }

    // the owner's side away from the mob: behind, the back diagonals, then the sides (a follow keeps it there)
    static constexpr float SIDE_ANGLES[] = {static_cast<float>(M_PI), static_cast<float>(M_PI * 0.75),
                                            static_cast<float>(-M_PI * 0.75), static_cast<float>(M_PI * 0.5),
                                            static_cast<float>(-M_PI * 0.5)};
    float bestAngle = PET_HEEL_ANGLE;
    float bestClearance = -std::numeric_limits<float>::max();
    for (float const relAngle : SIDE_ANGLES)
    {
        float x, y, z;
        spotAt(relAngle, PET_SIDE_DISTANCE, x, y, z);
        float const clearance = IdleReachClearance(mobs, pet, x, y, z);
        if (clearance > bestClearance && bot->IsWithinLOS(x, y, z + pet->GetCollisionHeight()))
        {
            bestClearance = clearance;
            bestAngle = relAngle;
        }
    }

    bool newMob = false;
    if (bestClearance >= 0.0f)
    {
        if (!NoteClearSpot(state, pet, worst, false, bestAngle, 0.0f, 0.0f, 0.0f, !SideFollowing(pet), newMob))
            return true;

        if (wasParked && charmInfo->HasCommandState(COMMAND_STAY))
        {
            charmInfo->SetCommandState(COMMAND_FOLLOW);
            charmInfo->RemoveStayPosition();
            charmInfo->SetIsAtStay(false);
        }

        if (!charmInfo->IsFollowing())
            charmInfo->SetIsReturning(true);
        motion->MoveFollow(bot, PET_SIDE_DISTANCE, bestAngle);
        if (newMob && worst)
            LOG_INFO("playerbots", "[pet idle reach] {}: {} keeps to the owner's side ({:.0f} deg off its back), out "
                     "of the reach of idle {} ({:.1f} yd from the owner)", bot->GetName(), pet->GetName(),
                     std::fabs(bestAngle - static_cast<float>(M_PI)) * 180.0f / static_cast<float>(M_PI),
                     worst->GetName(), worst->GetExactDist(bot));
        return true;
    }

    // the owner itself stands inside that reach: a park spot a few yards back, on its floor, in its sight, the way
    // there never deeper inside a reach than the owner stands
    float const ownerClearance =
        IdleReachClearance(mobs, pet, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    bool found = false;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    for (float radius = PET_SIDE_DISTANCE + PET_PARK_RING_STEP; radius <= PET_PARK_MAX_DISTANCE + 0.01f && !found;
         radius += PET_PARK_RING_STEP)
    {
        for (int i = 0; i < PET_PARK_ANGLES && !found; ++i)
        {
            // from straight back round both sides
            int const k = (i % 2) ? (i + 1) / 2 : -(i / 2);
            float const relAngle = static_cast<float>(M_PI) + k * 2.0f * static_cast<float>(M_PI) / PET_PARK_ANGLES;
            float x, y, z;
            spotAt(relAngle, radius, x, y, z);
            if (std::fabs(z - bot->GetPositionZ()) > PET_PARK_MAX_DZ || IdleReachClearance(mobs, pet, x, y, z) < 0.0f ||
                !bot->IsWithinLOS(x, y, z + pet->GetCollisionHeight()))
                continue;

            bool clearWay = true;
            for (int step = 1; step < 4 && clearWay; ++step)
            {
                float const along = static_cast<float>(step) / 4.0f;
                float const wx = bot->GetPositionX() + (x - bot->GetPositionX()) * along;
                float const wy = bot->GetPositionY() + (y - bot->GetPositionY()) * along;
                float const wz = bot->GetPositionZ() + (z - bot->GetPositionZ()) * along;
                clearWay = IdleReachClearance(mobs, pet, wx, wy, wz) >= ownerClearance - 0.5f;
            }

            if (clearWay)
            {
                found = true;
                px = x;
                py = y;
                pz = z;
            }
        }
    }

    if (!found)
    {
        // nowhere clear near: the owner's side farthest from the mob, the best there is
        if (!NoteClearSpot(state, pet, worst, false, bestAngle, 0.0f, 0.0f, 0.0f, !SideFollowing(pet), newMob))
            return true;

        if (wasParked && charmInfo->HasCommandState(COMMAND_STAY))
        {
            charmInfo->SetCommandState(COMMAND_FOLLOW);
            charmInfo->RemoveStayPosition();
            charmInfo->SetIsAtStay(false);
        }

        motion->MoveFollow(bot, PET_SIDE_DISTANCE, bestAngle);
        if (newMob && worst)
            LOG_INFO("playerbots", "[pet idle reach] {}: {} has no spot out of the reach of idle {} within {:.0f} yd "
                     "(the owner {:.1f} yd from it): the owner's far side", bot->GetName(), pet->GetName(),
                     worst->GetName(), PET_PARK_MAX_DISTANCE, worst->GetExactDist(bot));
        return true;
    }

    if (!NoteClearSpot(state, pet, worst, true, 0.0f, px, py, pz, false, newMob))
        return true;

    // the order the client's Stay button gives, at the spot (as the approach's run ahead)
    pet->AttackStop();
    pet->InterruptNonMeleeSpells(false);
    pet->ClearUnitState(UNIT_STATE_FOLLOW);
    charmInfo->SetCommandState(COMMAND_STAY);
    charmInfo->SetIsCommandAttack(false);
    charmInfo->SetIsCommandFollow(false);
    charmInfo->SetIsFollowing(false);
    charmInfo->SetIsAtStay(false);
    charmInfo->SetIsReturning(true);
    motion->Clear();
    motion->MovePoint(pet->GetGUID().GetCounter(), px, py, pz);
    charmInfo->SaveStayPosition(false);
    if (newMob && worst)
        LOG_INFO("playerbots", "[pet idle reach] {}: {} parks {:.1f} yd back at ({:.1f},{:.1f},{:.1f}), out of the "
                 "reach of idle {} (the owner stands {:.1f} yd from it, inside its reach)", bot->GetName(),
                 pet->GetName(), bot->GetExactDist(px, py, pz), px, py, pz, worst->GetName(),
                 worst->GetExactDist(bot));
    return true;
}

// The pet runs ahead to the mob's far side from the tank and waits there for the lead. False: no safe spot (an idle
// pack by it), or a stay the player ordered.
bool ApproachTarget(PlayerbotAI* botAI, Creature* pet, Unit* target, PetControlState& state, std::string const& why)
{
    Player* bot = botAI->GetBot();
    Player* tank = GroupTank(bot);
    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!tank || !charmInfo)
        return false;

    uint32 const now = getMSTime();
    bool logIt = false;
    {
        std::lock_guard<std::mutex> guard(state.lock);
        PetApproach* approach = FindApproach(state, pet);
        if (!approach || approach->target != target->GetGUID())
            return false;

        // a stay the player ordered stays theirs
        if (!approach->moving && charmInfo->HasCommandState(COMMAND_STAY))
            return false;

        if (approach->movedMs && getMSTimeDiff(approach->movedMs, now) < PET_APPROACH_REISSUE_MS)
            return true;
    }

    float const angle = tank->GetAngle(target);  // from the tank through the mob: its far side
    float const distance = target->GetCombatReach() + PET_APPROACH_GAP;
    float x = target->GetPositionX() + std::cos(angle) * distance;
    float y = target->GetPositionY() + std::sin(angle) * distance;
    float z = target->GetPositionZ();
    pet->UpdateAllowedPositionZ(x, y, z);
    if (PetSpotWakesIdle(botAI, pet, x, y, z))
        return false;

    for (int i = 1; i < PET_APPROACH_RUN_SAMPLES; ++i)
    {
        float const along = static_cast<float>(i) / PET_APPROACH_RUN_SAMPLES;
        float const runX = pet->GetPositionX() + (x - pet->GetPositionX()) * along;
        float const runY = pet->GetPositionY() + (y - pet->GetPositionY()) * along;
        float const runZ = pet->GetPositionZ() + (z - pet->GetPositionZ()) * along;
        if (PetSpotWakesIdle(botAI, pet, runX, runY, runZ))
            return false;
    }

    {
        std::lock_guard<std::mutex> guard(state.lock);
        PetApproach* approach = FindApproach(state, pet);
        if (!approach)
            return false;

        approach->moving = true;
        approach->movedMs = now ? now : 1;
        logIt = !approach->logged;
        approach->logged = true;
    }

    // the order the client's Stay button gives, at the spot (PetAI returns there and keeps still, MovementInform)
    pet->AttackStop();
    pet->InterruptNonMeleeSpells(false);
    pet->ClearUnitState(UNIT_STATE_FOLLOW);
    charmInfo->SetCommandState(COMMAND_STAY);
    charmInfo->SetIsCommandAttack(false);
    charmInfo->SetIsCommandFollow(false);
    charmInfo->SetIsFollowing(false);
    charmInfo->SetIsAtStay(false);
    charmInfo->SetIsReturning(true);
    pet->GetMotionMaster()->Clear();
    pet->GetMotionMaster()->MovePoint(pet->GetGUID().GetCounter(), x, y, z);
    charmInfo->SaveStayPosition(false);

    if (logIt)
        LOG_INFO("playerbots",
                 "[pet-quick-engage] {}: {} runs ahead to {} ({:.1f} yd from the pet, the tank {} on it) and waits for "
                 "{}",
                 bot->GetName(), pet->GetName(), target->GetName(), pet->GetExactDist(target), tank->GetName(), why);
    return true;
}

// [pet-quick-engage] A pet taunt on autocast in a group with a tank takes the mob off it. The toggle action keeps it
// off out of combat ("toggle pet spell"); a pet called or revived mid-fight comes back with its saved autocast.
void KeepPetTauntsOff(Player* bot, Creature* pet)
{
    Pet* realPet = pet->ToPet();
    if (!realPet || realPet->m_autospells.empty())
        return;

    std::vector<uint32> taunts;
    for (uint32 spellId : realPet->m_autospells)
        if (std::find(petTauntSpells.begin(), petTauntSpells.end(), spellId) != petTauntSpells.end())
            taunts.push_back(spellId);

    if (taunts.empty() || !GroupHasOtherTank(bot))
        return;

    for (uint32 spellId : taunts)
    {
        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId))
        {
            realPet->ToggleAutocast(spellInfo, false);
            LOG_INFO("playerbots", "[pet-quick-engage] {}: {} autocast of {} turned off (the group has a tank)",
                     bot->GetName(), realPet->GetName(), spellInfo->SpellName[0]);
        }
    }
}

ReactStates ConfigPetStance()
{
    switch (sPlayerbotAIConfig.DefaultPetStance)
    {
        case 0:
            return REACT_PASSIVE;
        case 2:
            return REACT_AGGRESSIVE;
        default:
            return REACT_DEFENSIVE;
    }
}
}  // namespace

namespace DungeonPetControl
{
void Update(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld() || bot->m_Controlled.empty())
        return;

    bool const control = bot->IsAlive() && InDungeonGroup(bot);
    PetControlState& state = ControlState();
    // a copy: a stop or a follow order must not touch the set walked (a guardian's script may unsummon on it)
    std::vector<Unit*> const controlledUnits(bot->m_Controlled.begin(), bot->m_Controlled.end());
    for (Unit* controlled : controlledUnits)
    {
        Creature* pet = controlled ? controlled->ToCreature() : nullptr;
        if (!pet || !pet->IsInWorld() || !pet->IsAlive() || pet->IsTotem() || !pet->IsAIEnabled || !pet->AI() ||
            pet->GetMap() != bot->GetMap())
            continue;

        if (!pet->IsControllableGuardian() || !pet->GetCharmInfo())
        {
            if (control)
                ControlGuardian(bot, pet);
            continue;
        }

        if (!control)
        {
            EndApproach(state, pet, true);
            EndClearSpot(state, pet);
            if (AtHeel(pet) && pet->GetCharmInfo()->HasCommandState(COMMAND_FOLLOW))
                pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());

            std::lock_guard<std::mutex> guard(state.lock);
            auto const itr = state.passivated.find(InstanceKey(pet));
            if (itr != state.passivated.end())
            {
                if (pet->HasReactState(REACT_PASSIVE))
                    pet->SetReactState(ConfigPetStance());
                state.passivated.erase(itr);
            }
            continue;
        }

        // a pet summoned this tick (a guardian starts aggressive), or one "set pet stance" gave the config stance
        ReactStates const stance = pet->GetReactState();
        if (stance != REACT_PASSIVE)
        {
            pet->SetReactState(REACT_PASSIVE);
            std::lock_guard<std::mutex> guard(state.lock);
            state.passivated.insert(InstanceKey(pet));
        }

        // [pet-quick-engage] the pet's taunt stays off in a group with a tank, also for a pet called mid-fight
        KeepPetTauntsOff(bot, pet);

        // [pet-quick-engage] On a mob the tank holds the pet bites once the tank's threat lead is safe for it (or it
        // waited the longest), and runs ahead to the mob meanwhile.
        // [pet_idle_reach] a fight lifts the park's stay order (the engage orders below own the pet)
        if (bot->IsInCombat())
            EndClearSpot(state, pet);

        std::string engageWhy;
        Unit* ownerTarget = OwnerAttackTarget(bot);
        if (IsAllowedPetTarget(bot, pet, ownerTarget))
        {
            if (PetMayEngage(botAI, pet, ownerTarget, state, engageWhy))
            {
                EndApproach(state, pet, true);
                CommandPetAttack(pet, ownerTarget);
                continue;
            }

            if (ApproachTarget(botAI, pet, ownerTarget, state, engageWhy))
                continue;
        }

        Unit* victim = pet->GetVictim();
        // between two of the owner's targets (no target for a moment) the pet stays on an allowed mob
        if (!ownerTarget && IsAllowedPetTarget(bot, pet, victim))
        {
            EndApproach(state, pet, true);
            continue;
        }

        // [pet-tank-target] The owner attacks nothing yet (not shooting, still out of combat): the pet fights the mob
        // the tank fights, by the tank, and stays on it.
        if (!ownerTarget)
        {
            Player* tank = GroupTank(bot);
            Unit* tankTarget = tank && tank->IsInCombat() ? tank->GetVictim() : nullptr;
            if (victim && victim == tankTarget && IsAllowedPetTarget(bot, pet, victim, false))
            {
                EndApproach(state, pet, true);
                continue;
            }

            if (tankTarget && IsAllowedPetTarget(bot, pet, tankTarget, false))
            {
                if (!PetMayEngage(botAI, pet, tankTarget, state, engageWhy))
                {
                    if (ApproachTarget(botAI, pet, tankTarget, state, engageWhy))
                        continue;
                }
                else
                {
                    EndApproach(state, pet, true);
                    bool log = false;
                    {
                        std::lock_guard<std::mutex> guard(state.lock);
                        ObjectGuid& last = state.logged[InstanceKey(pet)];
                        log = last != tankTarget->GetGUID();
                        last = tankTarget->GetGUID();
                    }

                    if (log)
                        LOG_INFO("playerbots",
                                 "[pet-tank-target] {}: {} attacks the tank's target {} ({:.1f} yd from the owner, "
                                 "owner attacks nothing{}; [pet-quick-engage] {})",
                                 bot->GetName(), pet->GetName(), tankTarget->GetName(), tankTarget->GetExactDist(bot),
                                 bot->IsInCombat() ? "" : ", not in combat", engageWhy);

                    CommandPetAttack(pet, tankTarget);
                    continue;
                }
            }
        }

        // nothing to bite or run ahead to: the approach's stay order is lifted (the wait is kept, PET_WAIT_FORGET_MS)
        EndApproach(state, pet, false);

        // [b35_sh] a pet left behind on another level is dismissed / re-summoned by its owner (normal casts)
        if (!bot->IsInCombat() && !victim && RecallStrandedPet(botAI, pet))
            continue;

        // [pet_idle_reach] out of combat the pet keeps out of every idle mob's reach (bars and doors included)
        if (!bot->IsInCombat() && !victim)
        {
            if (KeepOutOfIdleReach(botAI, pet, state))
                continue;
        }
        else
            EndClearSpot(state, pet);

        CharmInfo* charmInfo = pet->GetCharmInfo();
        if (charmInfo->HasCommandState(COMMAND_STAY))
            continue;

        bool const strayed = !charmInfo->IsReturning() && !charmInfo->IsFollowing() &&
                             pet->GetExactDist(bot) > PET_RETURN_DISTANCE;
        if (!victim && !strayed)
        {
            if (!bot->IsInCombat())
                KeepAtHeel(bot, pet);
            continue;
        }

        if (victim)
        {
            bool log = false;
            {
                std::lock_guard<std::mutex> guard(state.lock);
                ObjectGuid& last = state.logged[InstanceKey(pet)];
                log = last != victim->GetGUID();
                last = victim->GetGUID();
            }

            if (log)
            {
                Player* tank = GroupTank(bot);
                bool const tankOn = tank && (victim->IsEngagedBy(tank) || tank->GetVictim() == victim);
                char const* stanceName = stance == REACT_PASSIVE ? "passive"
                                         : stance == REACT_DEFENSIVE ? "defensive"
                                                                     : "aggressive";
                LOG_INFO("playerbots",
                         "[pet control] {}: {} called back from {} ({:.1f} yd from the owner, {:.1f} from the tank, "
                         "tank engaged {}, owner attacks {}, stance was {})",
                         bot->GetName(), pet->GetName(), victim->GetName(), victim->GetExactDist(bot),
                         tank ? victim->GetExactDist(tank) : -1.0f, tankOn ? "yes" : "no",
                         ownerTarget ? ownerTarget->GetName() : "nothing", stanceName);
            }
        }

        CommandPetFollow(bot, pet);
    }
}
}  // namespace DungeonPetControl

bool PetAttackAction::Execute(Event /*event*/)
{
    Guardian* pet = bot->GetGuardianPet();
    if (!pet)
        return false;

    // Do not attack if the pet's stance is set to "passive".
    if (pet->GetReactState() == REACT_PASSIVE)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (!bot->IsValidAttackTarget(target))
        return false;

    // This section has been commented because it was overriding the
    // pet's stance to "passive" every time the attack action was executed.
    // pet->SetReactState(REACT_PASSIVE);

    pet->ClearUnitState(UNIT_STATE_FOLLOW);
    pet->AttackStop();
    pet->SetTarget(target->GetGUID());

    pet->GetCharmInfo()->SetIsCommandAttack(true);
    pet->GetCharmInfo()->SetIsAtStay(false);
    pet->GetCharmInfo()->SetIsFollowing(false);
    pet->GetCharmInfo()->SetIsCommandFollow(false);
    pet->GetCharmInfo()->SetIsReturning(false);

    pet->ToCreature()->AI()->AttackStart(target);
    return true;
}

bool SetPetStanceAction::Execute(Event /*event*/)
{
    // Prepare a list to hold all controlled pet and guardian creatures
    std::vector<Creature*> targets;

    // Add the bot's main pet (if it exists) to the target list
    Pet* pet = bot->GetPet();
    if (pet)
        targets.push_back(pet);

    // Loop through all units controlled by the bot (could be pets, guardians, etc.)
    for (Unit::ControlSet::const_iterator itr = bot->m_Controlled.begin(); itr != bot->m_Controlled.end(); ++itr)
    {
        // Only add creatures (skip players, vehicles, etc.)
        Creature* creature = dynamic_cast<Creature*>(*itr);
        if (!creature)
            continue;
        // Avoid adding the main pet twice
        if (pet && creature == pet)
            continue;
        targets.push_back(creature);
    }

    // If there are no controlled pets or guardians, notify the player and exit
    if (targets.empty())
    {
        botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "pet_no_pet_error", "You have no pet or guardian pet.", {}));
        return false;
    }

    // Get the default pet stance from the configuration
    int32 stance = sPlayerbotAIConfig.DefaultPetStance;
    ReactStates react = REACT_DEFENSIVE;
    std::string stanceText = "defensive (from config, fallback)";

    // Map the config stance integer to a ReactStates value and a message
    switch (stance)
    {
        case 0:
            react = REACT_PASSIVE;
            stanceText = "passive (from config)";
            break;
        case 1:
            react = REACT_DEFENSIVE;
            stanceText = "defensive (from config)";
            break;
        case 2:
            react = REACT_AGGRESSIVE;
            stanceText = "aggressive (from config)";
            break;
        default:
            react = REACT_DEFENSIVE;
            stanceText = "defensive (from config, fallback)";
            break;
    }

    // Apply the stance to all target creatures (pets/guardians)
    for (Creature* target : targets)
    {
        target->SetReactState(react);
        CharmInfo* charmInfo = target->GetCharmInfo();
        // If the creature has a CharmInfo, set the player-visible stance as well
        if (charmInfo)
            charmInfo->SetPlayerReactState(react);
    }

    // In a dungeon group the pets go straight back to passive, before an attack later in this tick reaches the config
    // stance (PetAI::OwnerAttacked): "new pet" fires on every summon and resummon.
    DungeonPetControl::Update(botAI);

    // If debug is enabled in config, inform the master of the new stance
    if (sPlayerbotAIConfig.PetChatCommandDebug == 1)
        botAI->TellMaster("Pet stance set to " + stanceText + " (applied to all pets/guardians).");

    return true;
}
