/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoSActions.h"
#include "Pet.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "DungeonInteract.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "TemporarySummon.h"
#include "Timer.h"
#include "WlkTacticLog.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <iterator>
#include <utility>

namespace
{
constexpr float BRANN_TALK_DISTANCE = INTERACTION_DISTANCE - 1.0f;
constexpr float BRANN_GATHER_RADIUS = 30.0f;
constexpr float BRANN_START_APPROACH_RANGE = 80.0f;  // escort/Tribunal starts happen where the group already is
constexpr float BRANN_STAY_DISTANCE = 4.0f;
constexpr float APPROACH_DISTANCE = 5.0f;
constexpr float HAZARD_CLEAR_DISTANCE = 10.0f;

// Tribunal: taunts that reach an add from the tank's hold spot (30 yd), Death Grip and Avenger's Shield pull or hit
// it from there as well.
// [b33_hos] Heroic Throw (30 yd, high threat) as well: hos-h 20261026-000434 had the warrior's Taunt on cooldown
// while a second Protector ran in on the healer.
constexpr std::array<char const*, 8> TRIBUNAL_RANGED_TAUNTS = {"hand of reckoning", "death grip", "dark command",
                                                               "taunt", "growl", "avenger's shield",
                                                               "heroic throw", "faerie fire (feral)"};  // [b34_hos]
// [b33_hos] Area taunts for a wave that reached the group: 3 Protectors were on the healer when she died and the
// warrior's single Taunt / Mocking Blow took one at a time (no Challenging Shout in the whole fight).
constexpr std::array<char const*, 2> TRIBUNAL_AREA_TAUNTS = {"challenging shout", "challenging roar"};
constexpr float TRIBUNAL_AREA_TAUNT_RANGE = 10.0f;
// [b35_hos] Area threat for a tank without an area taunt: hos-h 20261026-024524-hos-h-5G-28, the DK tank logged "2 adds
// on others within 10 yd, no area taunt castable" and put down Death and Decay once in 180 s (Horn of Winter 16
// samples). The ground spell lands on its target (PlayerbotAI::CastSpell SetDst(*target)).
constexpr std::array<char const*, 4> TRIBUNAL_AREA_THREAT = {"death and decay", "consecration",
                                                             "blood boil", "thunder clap"};  // [b36_hos]
// Control on a Dark Rune Protector hitting the healer: stuns and incapacitates of the damage dealers.
constexpr std::array<char const*, 9> TRIBUNAL_PROTECTOR_CC = {"hammer of justice", "kidney shot", "gouge",
                                                              "repentance", "scatter shot", "intimidation",
                                                              "concussion blow", "deep freeze", "war stomp"};

// Krystallus: short steps re-evaluated every tick, so bots that pick the same side correct each other. The longer
// step lets a bot still deep in the others' Shatter (heroic 61547: 25k at the centre, wave 10: 15.9k and 19.2k on
// the healer and the mage) get out within the ~5 s before it is Stoned, while Petrifying Grip slows it.
// [b32_pull] hos-h 20261025-221216-hos-h-5E-44: Ground Slam dropped the rogue 3 yd from the holy priest on the
// ledge east of Krystallus (z 207-208); both bots logged "shatter spread (Execute returned false)" for 7 s, the four
// best straight lines all ended at the ledge edge, and the rogue's Shatter killed the priest twice (16.9k, 1015 s and
// 1036 s). More directions are path-checked, a 15 yd step is tried, and a cut line falls back to a walked path.
constexpr std::array<float, 3> SPREAD_STEPS = {5.0f, 10.0f, 15.0f};
constexpr uint32 SPREAD_DIRECTIONS = 16;
constexpr uint32 SPREAD_MAX_PATH_CHECKS = 16;  // [b32_pull] was 4
constexpr uint32 SPREAD_MAX_WALKED_PATHS = 4;  // [b32_pull]
constexpr float SPREAD_WALK_STRETCH = 1.6f;  // [b32_pull] a walked path at most this much longer than the line
constexpr float SPREAD_MIN_GAIN = 1.0f;
// The boss is passive until Shatter. Ground Slam knocks everybody 40-50 yd out, and a 40 yd cap left those bots no
// step that did not close in on the boss, so they stood still in reach of the others (waves 3-6).
constexpr float SLAM_MAX_BOSS_DISTANCE = 55.0f;
constexpr float RANGED_SPREAD_DISTANCE = 18.0f;  // move when anything that will Shatter is closer than this
constexpr float RANGED_SPREAD_GOAL = 22.0f;
constexpr float RANGED_MAX_BOSS_DISTANCE = 30.0f;  // stay in casting range of the boss
constexpr uint32 RANGED_SPREAD_RETRY_MS = 1000;

// Pets are slowed by the same Petrifying Grip, so the park spot is searched within what they can still cover.
constexpr std::array<float, 3> PET_PARK_STEPS = {8.0f, 14.0f, 20.0f};
constexpr uint32 PET_PARK_DIRECTIONS = 16;
constexpr uint32 PET_PARK_MAX_PATH_CHECKS = 6;
// Wave 8: an Enhancement shaman died to his own two Spirit Wolves (2 x 4.9k Shatter, ~15 yd away at the boss) while
// they should have been parked. A parked pet still this close to a member, or no longer passive, is parked again.
constexpr float PET_REPARK_DISTANCE = 16.0f;
constexpr uint32 PET_REPARK_INTERVAL_MS = 1500;
// A slowed pet covers ~20 yd before it is Stoned, rarely enough to clear 30 yd from a group it stood in. When the
// best park spot still leaves a member this close (Shatter 33 % and more on it), a non-hunter pet is dismissed
// instead: Dismiss on the pet frame needs no cast and a dismissed pet cannot be Stoned (a warlock summons it again
// after Shatter). A hunter's pet is never abandoned here: that would delete it (it is dismissed before the fight).
constexpr float PET_DISMISS_MEMBER_DISTANCE = 20.0f;
constexpr uint32 STAR_RETRY_MS = 1000;

// Shatter damage falls off linearly to 0 at its radius, so the summed overlap is the damage to minimise.
float ShatterExposure(float x, float y, std::vector<Position> const& sources, float radius)
{
    float exposure = 0.0f;
    for (Position const& source : sources)
    {
        float const distance = source.GetExactDist2d(x, y);
        if (distance < radius)
            exposure += radius - distance;
    }
    return exposure;
}

bool AllMembersAlive(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return bot->IsAlive();

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (!member->IsAlive())
            return false;
    }
    return true;
}

// The pet frame's Dismiss (WorldSession::HandlePetActionHelper, COMMAND_ABANDON) for a pet that is not a hunter's: a
// summoned pet is put down like a dismissed warlock demon, a guardian or minion is unsummoned.
bool DismissControlled(Player* owner, Unit* controlled)
{
    if (!controlled || !controlled->IsAlive() || !controlled->IsCreature() ||
        controlled->GetOwnerGUID() != owner->GetGUID())
        return false;

    if (Pet* pet = controlled->ToPet())
    {
        if (pet->getPetType() == HUNTER_PET)
            return false;

        pet->SendPetDismissSound();
        pet->setDeathState(DeathState::Corpse);
        return true;
    }

    if (controlled->HasUnitTypeMask(UNIT_MASK_MINION | UNIT_MASK_SUMMON | UNIT_MASK_GUARDIAN |
                                    UNIT_MASK_CONTROLLABLE_GUARDIAN))
    {
        if (TempSummon* summon = controlled->ToTempSummon())
        {
            summon->UnSummon();
            return true;
        }
    }
    return false;
}

float NearestMemberDistance(Player* bot, float x, float y)
{
    float nearest = FLT_MAX;
    Group* group = bot->GetGroup();
    if (!group)
        return bot->GetExactDist2d(x, y);

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->IsInWorld() && member->IsAlive() && member->GetMapId() == bot->GetMapId() &&
            member->GetInstanceId() == bot->GetInstanceId())
            nearest = std::min(nearest, member->GetExactDist2d(x, y));
    }
    return nearest;
}

// Everything a parked pet would Shatter on: every member (its owner too) and every other pet of the group.
void CollectPetParkSources(Player* bot, Unit* pet, std::vector<Position>& sources)
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        sources.push_back(bot->GetPosition());
        return;
    }

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() ||
            member->GetInstanceId() != bot->GetInstanceId() || !member->IsAlive())
            continue;

        sources.push_back(member->GetPosition());
        for (Unit* controlled : member->m_Controlled)
            if (controlled && controlled != pet && controlled->IsInWorld() && controlled->IsAlive() &&
                !controlled->IsTotem())
                sources.push_back(controlled->GetPosition());
    }
}
}  // namespace

bool KrystallusSpreadMovementAction::StepAwayFromShatter(Unit* boss, std::vector<Position> const& sources,
                                                         float radius, float maxBossDistance, bool keepBossLos)
{
    if (sources.empty() || !IsMovingAllowed() || IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
        return false;

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();
    float const exposure = ShatterExposure(botX, botY, sources, radius);
    if (exposure <= 0.0f)
        return false;

    float const bossDistance = boss ? boss->GetExactDist2d(botX, botY) : 0.0f;
    std::vector<std::pair<float, std::pair<float, float>>> candidates;  // exposure, (angle, step)
    candidates.reserve(SPREAD_DIRECTIONS * SPREAD_STEPS.size());
    for (float step : SPREAD_STEPS)
    {
        for (uint32 i = 0; i < SPREAD_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / SPREAD_DIRECTIONS;
            float const x = botX + std::cos(angle) * step;
            float const y = botY + std::sin(angle) * step;
            if (boss)
            {
                float const stepBossDistance = boss->GetExactDist2d(x, y);
                if (stepBossDistance > maxBossDistance && stepBossDistance > bossDistance)
                    continue;
            }

            float const stepExposure = ShatterExposure(x, y, sources, radius);
            if (stepExposure < exposure - SPREAD_MIN_GAIN)
                candidates.push_back({stepExposure, {angle, step}});
        }
    }
    std::sort(candidates.begin(), candidates.end());

    uint32 pathChecks = 0;
    std::vector<std::pair<float, float>> cutLines;  // [b32_pull] (angle, step) whose straight line a wall cut short
    for (auto const& candidate : candidates)
    {
        if (++pathChecks > SPREAD_MAX_PATH_CHECKS)
            break;

        float x = botX + std::cos(candidate.second.first) * candidate.second.second;
        float y = botY + std::sin(candidate.second.first) * candidate.second.second;
        float z = botZ;
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, botX, botY, botZ, x, y, z, false))
        {
            cutLines.push_back(candidate.second);
            continue;
        }

        // A wall may cut the step short.
        if (ShatterExposure(x, y, sources, radius) >= exposure - SPREAD_MIN_GAIN)
        {
            cutLines.push_back(candidate.second);
            continue;
        }

        if (keepBossLos && boss && !boss->IsWithinLOS(x, y, z))
            continue;

        if (MoveTo(bot->GetMapId(), x, y, z, false, false, false, true, MovementPriority::MOVEMENT_COMBAT))
            return true;
    }

    // [b32_pull] No straight line leads out: walk round the ledge to the best spot the navmesh reaches.
    uint32 walked = 0;
    for (auto const& line : cutLines)
    {
        if (++walked > SPREAD_MAX_WALKED_PATHS)
            break;

        float const x = botX + std::cos(line.first) * line.second;
        float const y = botY + std::sin(line.first) * line.second;
        float z = botZ;
        bot->UpdateAllowedPositionZ(x, y, z);
        PathGenerator path(bot);
        if (!path.CalculatePath(x, y, z, false) || (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) ||
            path.getPathLength() > line.second * SPREAD_WALK_STRETCH)
            continue;

        G3D::Vector3 const end = path.GetActualEndPosition();
        if (ShatterExposure(end.x, end.y, sources, radius) >= exposure - SPREAD_MIN_GAIN)
            continue;

        if (keepBossLos && boss && !boss->IsWithinLOS(end.x, end.y, end.z))
            continue;

        if (MoveTo(bot->GetMapId(), end.x, end.y, end.z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT))
            return true;
    }
    return false;
}

// Ground Slam -> 7 s of slowing Petrifying Grip -> Stoned: the step must be done before the bot is stoned.
bool ShatterSpreadAction::Execute(Event /*event*/)
{
    std::vector<Position> sources;
    HoSShared::CollectShatterSources(bot, true, sources);
    return StepAwayFromShatter(AI_VALUE2(Unit*, "find target", "krystallus"), sources, HoSIds::SHATTER_RADIUS,
                               SLAM_MAX_BOSS_DISTANCE, false);
}

bool KrystallusPetSpreadAction::Execute(Event /*event*/)
{
    bool const slammed = HoSShared::IsGroundSlamActive(bot);
    std::vector<Creature*> pets;
    for (Unit* controlled : bot->m_Controlled)
        if (controlled && controlled->IsAlive() && !controlled->IsTotem() && controlled->GetCharmInfo() &&
            controlled->ToCreature())
            pets.push_back(controlled->ToCreature());

    // Dismissed or dead pets leave no stale entries behind.
    for (auto itr = _parked.begin(); itr != _parked.end();)
    {
        bool const owned = std::any_of(pets.begin(), pets.end(),
                                       [&itr](Creature* pet) { return pet->GetGUID() == itr->first; });
        if (!owned)
            _reparkAt.erase(itr->first);

        itr = owned ? std::next(itr) : _parked.erase(itr);
    }

    bool acted = false;
    for (Creature* pet : pets)
    {
        auto const parked = _parked.find(pet->GetGUID());
        if (slammed || HoSShared::IsShatterPending(pet))
        {
            // Once Stoned it cannot move any more; a pet already sent away keeps its spot unless that spot failed.
            if (pet->HasAura(SPELL_STONED) || (parked != _parked.end() && !NeedsRepark(pet)))
                continue;

            _reparkAt[pet->GetGUID()] = getMSTime() + PET_REPARK_INTERVAL_MS;
            if (ParkPet(pet))
                acted = true;
        }
        else if (parked != _parked.end())
        {
            ReleasePet(pet, parked->second);
            _parked.erase(parked);
            _reparkAt.erase(pet->GetGUID());
            acted = true;
        }
    }
    return acted;
}

bool KrystallusPetSpreadAction::ParkPet(Creature* pet)
{
    std::vector<Position> sources;
    CollectPetParkSources(bot, pet, sources);

    Unit* boss = AI_VALUE2(Unit*, "find target", "krystallus");
    float const petX = pet->GetPositionX();
    float const petY = pet->GetPositionY();
    float const petZ = pet->GetPositionZ();
    float const exposure = ShatterExposure(petX, petY, sources, HoSIds::SHATTER_RADIUS);

    std::vector<std::pair<float, std::pair<float, float>>> candidates;  // exposure, (x, y)
    candidates.reserve(PET_PARK_STEPS.size() * PET_PARK_DIRECTIONS);
    for (float step : PET_PARK_STEPS)
    {
        for (uint32 i = 0; i < PET_PARK_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / PET_PARK_DIRECTIONS;
            float const x = petX + std::cos(angle) * step;
            float const y = petY + std::sin(angle) * step;
            if (boss && boss->GetExactDist2d(x, y) > SLAM_MAX_BOSS_DISTANCE)
                continue;

            float const stepExposure = ShatterExposure(x, y, sources, HoSIds::SHATTER_RADIUS);
            if (stepExposure < exposure - SPREAD_MIN_GAIN)
                candidates.push_back({stepExposure, {x, y}});
        }
    }
    std::sort(candidates.begin(), candidates.end());

    uint32 pathChecks = 0;
    for (auto const& candidate : candidates)
    {
        if (++pathChecks > PET_PARK_MAX_PATH_CHECKS)
            break;

        float x = candidate.second.first;
        float y = candidate.second.second;
        float z = petZ;
        if (!pet->GetMap()->CheckCollisionAndGetValidCoords(pet, petX, petY, petZ, x, y, z, false))
            continue;

        if (ShatterExposure(x, y, sources, HoSIds::SHATTER_RADIUS) >= exposure - SPREAD_MIN_GAIN)
            continue;

        // The best spot it can still reach is in the group's Shatter: it goes away instead (a hunter's pet stays).
        if (NearestMemberDistance(bot, x, y) < PET_DISMISS_MEMBER_DISTANCE && DismissControlled(bot, pet))
            return true;

        CharmInfo* charmInfo = pet->GetCharmInfo();
        // A re-park keeps the stance stored by the first one (the pet is passive by now).
        _parked.emplace(pet->GetGUID(), pet->GetReactState());

        // Passive + stay keeps PetAI from re-engaging on the owner's next cast and from walking back to the owner.
        pet->AttackStop();
        pet->InterruptNonMeleeSpells(false);
        pet->SetReactState(REACT_PASSIVE);
        charmInfo->SetCommandState(COMMAND_STAY);
        charmInfo->SetIsCommandAttack(false);
        charmInfo->SetIsCommandFollow(false);
        charmInfo->SetIsFollowing(false);
        charmInfo->SetIsAtStay(false);
        charmInfo->SetIsReturning(true);  // PetAI marks it "at stay" when this point id is reached
        charmInfo->SetForcedSpell(0);
        charmInfo->SetForcedTargetGUID();
        pet->GetMotionMaster()->MovePoint(pet->GetGUID().GetCounter(), x, y, z);
        return true;
    }

    // No spot that lowers its Shatter at all: next to a member it is dismissed as well.
    return NearestMemberDistance(bot, petX, petY) < PET_DISMISS_MEMBER_DISTANCE && DismissControlled(bot, pet);
}

bool KrystallusPetSpreadAction::NeedsRepark(Creature* pet) const
{
    auto const next = _reparkAt.find(pet->GetGUID());
    if (next != _reparkAt.end() && getMSTime() < next->second)
        return false;

    if (!pet->HasReactState(REACT_PASSIVE))
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return pet->GetExactDist2d(bot) < PET_REPARK_DISTANCE;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->IsAlive() && member->GetMapId() == bot->GetMapId() &&
            pet->GetExactDist2d(member) < PET_REPARK_DISTANCE)
            return true;
    }
    return false;
}

void KrystallusPetSpreadAction::ReleasePet(Creature* pet, ReactStates previous)
{
    CharmInfo* charmInfo = pet->GetCharmInfo();
    pet->SetReactState(previous);
    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    charmInfo->SetCommandState(COMMAND_FOLLOW);
    charmInfo->SetIsCommandAttack(false);
    charmInfo->SetIsAtStay(false);
    charmInfo->SetIsReturning(true);
    charmInfo->SetIsCommandFollow(true);
    charmInfo->SetIsFollowing(false);
    charmInfo->RemoveStayPosition();
    // The owner's next harmful cast (Spell::prepare -> PetAI::OwnerAttacked) sends it back to the boss.
}

bool KrystallusDismissPetAction::Execute(Event /*event*/)
{
    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive() || !botAI->CanCastSpell("dismiss pet", bot))
        return false;

    return botAI->CastSpell("dismiss pet", bot);
}

bool KrystallusDismissSummonAction::Execute(Event /*event*/)
{
    bool dismissed = false;
    // Dismissing erases the unit from m_Controlled: collect first.
    std::vector<Unit*> summons;
    for (Unit* controlled : bot->m_Controlled)
        if (controlled && controlled->IsAlive() && controlled->IsCreature() && !controlled->IsTotem() &&
            HoSShared::IsKrystallusShortSummon(controlled))
            summons.push_back(controlled);

    for (Unit* summon : summons)
        dismissed |= DismissControlled(bot, summon);

    return dismissed;
}

bool KrystallusRangedSpreadAction::isUseful()
{
    if (getMSTime() < _nextAttemptMs || HoSShared::IsGroundSlamActive(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "krystallus");
    float x = 0.0f;
    float y = 0.0f;
    if (boss && HoSShared::StarSpot(bot, boss, HoSIds::STAR_RANGED_RADIUS, x, y) &&
        bot->GetExactDist2d(x, y) > HoSIds::STAR_TOLERANCE)
        return true;

    std::vector<Position> sources;
    HoSShared::CollectShatterSources(bot, false, sources);
    return HoSShared::NearestShatterSourceDistance(bot, sources) < RANGED_SPREAD_DISTANCE;
}

bool KrystallusRangedSpreadAction::MoveToStarSpot(Unit* boss)
{
    float x = 0.0f;
    float y = 0.0f;
    if (!boss || !HoSShared::StarSpot(bot, boss, HoSIds::STAR_RANGED_RADIUS, x, y) ||
        bot->GetExactDist2d(x, y) <= HoSIds::STAR_TOLERANCE || !IsMovingAllowed())
        return false;

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();
    float const wantX = x;
    float const wantY = y;
    float z = botZ;
    if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, botX, botY, botZ, x, y, z, false))
        return false;

    // A wall short of the spoke or a pillar between the spot and the boss: the plain step away does the job.
    if (std::hypot(x - wantX, y - wantY) > HoSIds::STAR_TOLERANCE || !boss->IsWithinLOS(x, y, z))
        return false;

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, true, MovementPriority::MOVEMENT_COMBAT);
}

bool KrystallusRangedSpreadAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "krystallus");
    if (MoveToStarSpot(boss))
    {
        _nextAttemptMs = getMSTime() + STAR_RETRY_MS;
        return true;
    }

    // The bot's own pet is left out here: a caster pet keeps repositioning next to its owner.
    std::vector<Position> sources;
    HoSShared::CollectShatterSources(bot, false, sources);
    if (StepAwayFromShatter(boss, sources, RANGED_SPREAD_GOAL, RANGED_MAX_BOSS_DISTANCE, true))
        return true;

    _nextAttemptMs = getMSTime() + RANGED_SPREAD_RETRY_MS;
    return false;
}

bool AvoidLightningRingAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "sjonnir the ironshaper");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    float radius = 10.0f;
    float distanceExtra = 2.0f;

    if (distance < radius + distanceExtra)
    {
        return MoveAway(boss, radius + distanceExtra - distance);
    }

    return false;
}

bool HoSBrannGossipAction::Execute(Event /*event*/)
{
    Creature* brann = HoSShared::GetBrann(bot);
    uint32 const expected = HoSShared::NextBrannGossip(bot, brann);
    if (!expected)
        return false;

    bool const startsEvent = expected == HoSIds::GOSSIP_TRIBUNAL_BEFORE || expected == HoSIds::GOSSIP_TRIBUNAL_START;
    if (!bot->IsWithinDistInMap(brann, BRANN_TALK_DISTANCE))
    {
        // After the Tribunal Brann waits at Sjonnir's door, far from the group: the only long walk here.
        if (startsEvent && bot->GetExactDist2d(brann) > BRANN_START_APPROACH_RANGE)
        {
            RaidBotDuty::LogWorkaroundOnce(bot, "hos-brann-far",
                                           "Halls of Stone: Brann is far away, waiting for the group route "
                                           "to reach him");
            return false;
        }

        return MoveNear(brann, 2.0f);
    }

    if (startsEvent && (!AllMembersAlive(bot) || !DungeonInteract::IsGroupGathered(bot, brann, BRANN_GATHER_RADIUS)))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "hos-brann-wait",
                                       "Halls of Stone: waiting for the whole group alive and out of combat at Brann");
        return false;
    }

    if (!DungeonInteract::OpenGossip(bot, brann))
        return false;

    bool selected = false;
    if (bot->PlayerTalkClass->GetGossipMenu().GetMenuId() == expected)
    {
        int32 const option = DungeonInteract::FindGossipOption(bot, [](GossipMenuItem const&) { return true; });
        selected = DungeonInteract::SelectGossipOption(bot, brann, option);
    }
    bot->PlayerTalkClass->SendCloseGossip();

    if (selected)
        LOG_INFO("playerbots", "Halls of Stone: {} selected Brann gossip menu {}", bot->GetName(), expected);

    return selected;
}

bool HoSStayWithBrannAction::Execute(Event /*event*/)
{
    Creature* brann = HoSShared::GetBrann(bot);
    if (!brann || !brann->IsAlive())
        return false;

    return MoveNear(brann, BRANN_STAY_DISTANCE);
}

// [b33_hos] The ranged taunts from the tank's hold spot, shared by the Brann protection and the loose-add pickup.
namespace
{
bool TribunalRangedTaunt(PlayerbotAI* botAI, Unit* add)
{
    for (char const* spell : TRIBUNAL_RANGED_TAUNTS)
        if (botAI->CanCastSpell(spell, add) && botAI->CastSpell(spell, add))
            return true;

    return false;
}

// Tribunal adds hitting a member other than the tank within the area taunt's reach of the tank.
// [b36_hos] othersOnly = false counts every Tribunal add in range, the tank's own included (for area threat).
uint32 LooseAddsNear(PlayerbotAI* botAI, Player* bot, float range, bool othersOnly = true)
{
    uint32 count = 0;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        uint32 const entry = guid.GetEntry();
        if (entry != HoSIds::NPC_DARK_RUNE_PROTECTOR && entry != HoSIds::NPC_DARK_RUNE_STORMCALLER &&
            entry != HoSIds::NPC_IRON_GOLEM_CUSTODIAN)
            continue;

        Unit* add = botAI->GetUnit(guid);
        if (!add || !add->IsAlive() || bot->GetExactDist2d(add) > range)
            continue;

        Player* victim = add->GetVictim() ? add->GetVictim()->ToPlayer() : nullptr;
        if (!othersOnly || (victim && victim != bot && !PlayerbotAI::IsTank(victim) && victim->IsInSameRaidWith(bot)))
            ++count;
    }
    return count;
}
}  // namespace

bool HoSProtectBrannAction::Execute(Event /*event*/)
{
    Creature* brann = HoSShared::GetBrann(bot);
    Unit* attacker = HoSShared::NearestBrannAttacker(bot, brann);
    if (!attacker)
        return false;

    if (brann->GetHealthPct() < 100.0f)
        RaidBotDuty::LogWorkaroundOnce(bot, "hos-brann-heal",
                                       "Halls of Stone: needs honest tactic: healer bots only heal group members, "
                                       "Brann (no regeneration) is protected by taunts only");

    // [b44_hos] Inside the Tribunal every add taunts Brann for good, so this action picks a damage dealer's target
    // whenever its own is not on him; it follows the "dps target" split (WotlkDungeonHoSStrategy::
    // AppendTargetExclusions): the ranged on the Stormcallers, the melee on what reaches the hold spot. It took the
    // Stormcaller nearest Brann whenever no Protector was up and walked the Enhancement shaman 40-46 yd out
    // (DPS_GAP_TRIBUNAL_COS.md 2.5), and the Protector nearest him for the ranged, off the casters.
    if (!PlayerbotAI::IsTank(bot) && HoSShared::IsTribunalActive(bot))
    {
        // finishing the Protector it peeled off the healer (now on the tank): no switch to one of Brann's
        if (HoSShared::PeeledProtector(botAI))
            return false;

        bool const melee = PlayerbotAI::IsMelee(bot);
        auto const fits = [this, melee](Unit* add)
        {
            if (melee)
                return !HoSShared::LeftToRanged(bot, add);
            return add->GetEntry() == HoSIds::NPC_DARK_RUNE_STORMCALLER;
        };

        Unit* pick = nullptr;
        for (Unit* other : brann->getAttackers())
            if (other && other->IsAlive() && other->GetVictim() == brann && fits(other) &&
                (!pick || brann->GetExactDist(other) < brann->GetExactDist(pick)))
                pick = other;

        Unit* current = AI_VALUE(Unit*, "current target");
        if (current && current->IsAlive() && current->GetVictim() == brann && (fits(current) || !pick))
            return false;

        // a ranged dealer with no Stormcaller up takes the nearest add; a melee one leaves the far casters alone
        if (!pick && melee)
            return false;
        if (pick)
            attacker = pick;
    }

    // Keep a target that is already on Brann; otherwise take the add closest to him (the tank taunts it off).
    Unit* current = AI_VALUE(Unit*, "current target");
    if (current && current->IsAlive() && current->GetVictim() == brann)
        return false;

    // [b33_hos] The tank takes a wave walking in on Brann from its hold spot: a ranged taunt once the add is in its
    // reach, so the tank has the threat before the healer's heals do (hos-h 20261026-000434: all 3 Protectors of wave
    // 1 went to the healer while the tank stood out of combat, first taunt 4.8 s late). No walk out to it: Brann
    // stands at the spot and the add comes in.
    if (PlayerbotAI::IsTank(bot) && HoSShared::IsTribunalActive(bot) &&
        attacker->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y) >
            HoSIds::TRIBUNAL_CHASE_LIMIT)  // [b34_hos] the tank's forward spot
    {
        if (!TribunalRangedTaunt(botAI, attacker))
            return false;

        if (WlkTacticLog::Allow(bot, "hos tribunal brann taunt"))
            LOG_INFO("playerbots", "[hos tribunal brann taunt] {} taunts {} off Brann from its hold spot ({:.1f} yd)",
                     bot->GetName(), attacker->GetName(), bot->GetExactDist2d(attacker));
        return true;
    }

    if (!bot->IsWithinLOSInMap(attacker))
        return MoveNear(attacker, APPROACH_DISTANCE, MovementPriority::MOVEMENT_COMBAT);

    return Attack(attacker);
}

// hos-h 2026-10-25 wipe: three Dark Rune Protectors stayed on the healer, then three on the mage, while the tank
// held the Stormcallers ("tank assist" picks by its own score); the healer fled from them and died.
bool HoSTankTribunalAddAction::TauntFromSpot(Unit* add)
{
    // Righteous Defense goes on the member being hit; the others on the add (30 yd taunts, Death Grip pulls it in).
    Player* victim = add->GetVictim() ? add->GetVictim()->ToPlayer() : nullptr;
    if (victim && botAI->CanCastSpell("righteous defense", victim) && botAI->CastSpell("righteous defense", victim))
        return true;

    return TribunalRangedTaunt(botAI, add);  // [b33_hos]
}

bool HoSTankTribunalAddAction::Execute(Event /*event*/)
{
    Unit* add = HoSShared::LooseTribunalAdd(botAI);
    if (!add)
        return false;

    // [b33_hos] Several adds on the others around the tank: one area taunt takes them all (before the "already my
    // target" exit, the second and third of a wave are usually on the healer while the first is the tank's target).
    if (uint32 const nearAdds = LooseAddsNear(botAI, bot, TRIBUNAL_AREA_TAUNT_RANGE);
        nearAdds >= HoSIds::TRIBUNAL_AOE_TAUNT_ADDS)
    {
        for (char const* spell : TRIBUNAL_AREA_TAUNTS)
        {
            if (!botAI->CanCastSpell(spell, bot) || !botAI->CastSpell(spell, bot))
                continue;

            if (WlkTacticLog::Allow(bot, "hos tribunal area taunt"))
                LOG_INFO("playerbots", "[hos tribunal area taunt] {} casts {} on the adds hitting the group",
                         bot->GetName(), spell);
            return true;
        }

        // [b34_hos] hos-h 20261026-014521: no area taunt in the whole fight with 3 Protectors on the healer 2-6 yd
        // from the bear; this tells "not castable" (unknown spell, cooldown, form) from "never counted".
        if (WlkTacticLog::Allow(bot, "hos tribunal area taunt unusable"))
            LOG_INFO("playerbots", "[hos tribunal area taunt unusable] {}: {} adds on others within {:.0f} yd, no area "
                     "taunt castable (challenging shout {}, challenging roar {})", bot->GetName(), nearAdds,
                     TRIBUNAL_AREA_TAUNT_RANGE, botAI->HasSpell("challenging shout") ? "known" : "unknown",
                     botAI->HasSpell("challenging roar") ? "known" : "unknown");
    }

    // [b35_hos] / [b36_hos] Area threat where the adds stand, also when only one of them is still on another member
    // and the rest is on the tank (b35 needed two on others; hos-h 20261026-034959-hos-h-5G-25 never logged it, the
    // DK tank showed Death and Decay in no and Blood Boil in one of its 65 samples while two Protectors killed the
    // healer next to it).
    // On the loose add when it stands in reach (a harmful spell's cast check wants an enemy), else at the tank.
    if (uint32 const aroundTank = LooseAddsNear(botAI, bot, TRIBUNAL_AREA_TAUNT_RANGE, false);
        aroundTank >= HoSIds::TRIBUNAL_AOE_TAUNT_ADDS)
    {
        Unit* const centre = bot->GetExactDist2d(add) <= TRIBUNAL_AREA_TAUNT_RANGE ? add : static_cast<Unit*>(bot);
        for (char const* spell : TRIBUNAL_AREA_THREAT)
        {
            if (!botAI->CanCastSpell(spell, centre) || !botAI->CastSpell(spell, centre))
                continue;

            if (WlkTacticLog::Allow(bot, "hos tribunal area threat"))
                LOG_INFO("playerbots", "[hos tribunal area threat] {} casts {} among {} adds", bot->GetName(), spell,
                         aroundTank);
            return true;
        }
    }

    // [b34_hos] Already the tank's target and still on another member: its Swipe / Thunder Clap did not take it
    // (hos-h 20261026-014521: 3 Protectors stayed on the healer 2 yd from the bear for 5 s, Growl on cooldown), so a
    // ranged taunt or threat spell goes on it (Faerie Fire (Feral), Heroic Throw, ...), else the rotation goes on.
    if (AI_VALUE(Unit*, "current target") == add)
        return TauntFromSpot(add);

    float const fromSpot = add->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y);  // [b34_hos]
    if (fromSpot <= HoSIds::TRIBUNAL_CHASE_LIMIT)
        return Attack(add);

    // Far from the spot: taunt it over from where the tank stands, the add walks in to the healer's side.
    if (TauntFromSpot(add))
    {
        if (WlkTacticLog::Allow(bot, "hos tribunal ranged taunt"))
            LOG_INFO("playerbots", "[hos tribunal ranged taunt] {} taunts {} from its hold spot ({:.1f} yd from the "
                     "spot, on {})", bot->GetName(), add->GetName(), fromSpot,
                     add->GetVictim() ? add->GetVictim()->GetName() : std::string("nobody"));
        return true;
    }

    if (WlkTacticLog::Allow(bot, "hos tribunal no chase"))
        LOG_INFO("playerbots", "[hos tribunal no chase] {} stays at its hold spot: {} is {:.1f} yd from it and no "
                 "taunt is ready", bot->GetName(), add->GetName(), fromSpot);
    return false;
}

bool HoSTribunalHoldPointAction::Execute(Event /*event*/)
{
    if (WlkTacticLog::Allow(bot, "hos tribunal hold point"))
        LOG_INFO("playerbots", "[hos tribunal hold point] {} back to its spot at Brann's console ({:.1f} yd away)",
                 bot->GetName(), bot->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y));
    // [b34_hos] the tank's forward spot
    return MoveTo(bot->GetMapId(), HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y, HoSIds::TRIBUNAL_TANK_Z, false,
                  false, false, true, MovementPriority::MOVEMENT_COMBAT);
}

bool HoSTribunalHealerHoldAction::Execute(Event /*event*/)
{
    // [b34_hos] Back to the hold spot by Brann (no longer onto the tank, who meets the adds forward of it).
    if (WlkTacticLog::Allow(bot, "hos tribunal healer hold"))
        LOG_INFO("playerbots", "[hos tribunal healer hold] {} back to the hold spot by Brann ({:.1f} yd)",
                 bot->GetName(), bot->GetExactDist2d(HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y));
    return MoveTo(bot->GetMapId(), HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y, HoSIds::TRIBUNAL_HOLD_Z, false,
                  false, false, true, MovementPriority::MOVEMENT_COMBAT);
}

bool HoSTribunalProtectorAction::Execute(Event /*event*/)
{
    Unit* protector = HoSShared::ProtectorOnHealer(botAI);
    if (!protector)
        return false;

    // Control first where the class has one ready (stuns and incapacitates the Protectors are not immune to), then
    // the kill: they are what kills the healer.
    for (char const* spell : TRIBUNAL_PROTECTOR_CC)
    {
        if (!botAI->CanCastSpell(spell, protector) || !botAI->CastSpell(spell, protector))
            continue;

        if (WlkTacticLog::Allow(bot, "hos tribunal protector cc"))
            LOG_INFO("playerbots", "[hos tribunal protector cc] {} casts {} on {} (on the healer, {:.0f}%)",
                     bot->GetName(), spell, protector->GetName(), protector->GetHealthPct());
        return true;
    }

    if (WlkTacticLog::Allow(bot, "hos tribunal protector kill"))
        LOG_INFO("playerbots", "[hos tribunal protector kill] {} switches to {} on the healer ({:.0f}%)",
                 bot->GetName(), protector->GetName(), protector->GetHealthPct());
    return Attack(protector);
}

bool HoSAvoidTribunalHazardAction::Execute(Event /*event*/)
{
    Unit* hazard = HoSShared::NearestTribunalHazard(bot);
    if (!hazard)
        return false;

    return MoveAway(hazard, HAZARD_CLEAR_DISTANCE - bot->GetExactDist2d(hazard));
}
