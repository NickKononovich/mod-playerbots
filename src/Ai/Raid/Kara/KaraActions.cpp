/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "KaraActions.h"
#include "EncounterHelpers.h"
#include "KaraHelpers.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RaidHonesty.h"
#include "GossipDef.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "WorldSession.h"
#include <set>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <limits>
#include <list>
#include <map>
#include <string>

using namespace KaraHelpers;
using namespace EncounterHelpers;

// Shared

bool KarazhanResetEncounterStatesAction::Execute(Event /*event*/)
{
    uint32 const instanceId = bot->GetMap()->GetInstanceId();
    bool const isMechanicTracker = IsMechanicTrackerBot(bot, KARA_MAP_ID);
    bool reset = false;

    if (isMechanicTracker)
    {
        if (!AI_VALUE2(Unit*, "find target", "midnight") &&
            attumenDpsWaitTimer.erase(instanceId) > 0)
        {
            reset = true;
        }

        if (!AI_VALUE2(Unit*, "find target", "nightbane"))
        {
            if (nightbaneDpsWaitTimer.erase(instanceId) > 0)
                reset = true;

            if (nightbaneFlightPhaseStartTimer.erase(instanceId) > 0)
                reset = true;
        }
    }

    if (!AI_VALUE2(Unit*, "find target", "the big bad wolf"))
    {
        Action* wolfAction = context->GetAction("big bad wolf little red riding hood run away");
        if (wolfAction &&
            static_cast<BigBadWolfLittleRedRidingHoodRunAwayAction*>(wolfAction)->ResetRunIndex())
        {
            reset = true;
        }
    }

    if (!AI_VALUE2(Unit*, "find target", "netherspite"))
    {
        if (isMechanicTracker && netherspiteDpsWaitTimer.erase(instanceId) > 0)
            reset = true;

        Action* redAction = context->GetAction("netherspite block red beam");
        if (redAction &&
            static_cast<NetherspiteBlockRedBeamAction*>(redAction)->ResetRedBeamState())
        {
            reset = true;
        }

        Action* blueAction = context->GetAction("netherspite block blue beam");
        if (blueAction &&
            static_cast<NetherspiteBlockBlueBeamAction*>(blueAction)->ResetBlueBeamState())
        {
            reset = true;
        }

        Action* greenAction = context->GetAction("netherspite block green beam");
        if (greenAction &&
            static_cast<NetherspiteBlockGreenBeamAction*>(greenAction)->ResetGreenBeamState())
        {
            reset = true;
        }

        if (currentRedBlocker.erase(instanceId) > 0)
            reset = true;

        if (currentGreenBlocker.erase(instanceId) > 0)
            reset = true;

        if (currentBlueBlocker.erase(instanceId) > 0)
            reset = true;
    }

    return reset;
}

bool KarazhanSetTremorTotemAction::Execute(Event /*event*/)
{
    return botAI->CanCastSpell(Id(KaraSpells::SPELL_TREMOR_TOTEM), bot) &&
        botAI->CastSpell(Id(KaraSpells::SPELL_TREMOR_TOTEM), bot);
}

// Trash

bool ManaWarpStunCreatureBeforeWarpBreachAction::Execute(Event /*event*/)
{
    Unit* target = nullptr;
    constexpr float searchRadius = 40.0f;
    std::list<Creature*> manaWarps;
    bot->GetCreatureListWithEntryInGrid(manaWarps, Id(KaraNpcs::NPC_MANA_WARP), searchRadius);

    for (Creature* manaWarp : manaWarps)
    {
        if (!manaWarp || !manaWarp->IsAlive() || manaWarp->GetHealthPct() > 15.0f)
            continue;

        if (!target || manaWarp->GetGUID() < target->GetGUID())
            target = manaWarp;
    }

    if (!target)
        return false;

    static constexpr std::array spells = {
        "bash",
        "concussion blow",
        "hammer of justice",
        "kidney shot",
        "maim",
        "shadowfury",
        "shockwave",
    };

    for (char const* spell : spells)
    {
        if (botAI->CanCastSpell(spell, target) && botAI->CastSpell(spell, target))
            return true;
    }

    return false;
}

// Attumen the Huntsman

// Midnight's CombatReach is 1.6 yards
// Unmounted Attumen's CombatReach is 1.5 yards
bool AttumenTheHuntsmanHandlePhaseOneAction::Execute(Event /*event*/)
{
    Unit* midnight = AI_VALUE2(Unit*, "find target", "midnight");
    if (!midnight)
        return false;

    if (PlayerbotAI::IsAssistTank(bot))
    {
        Unit* attumen = AI_VALUE2(Unit*, "find target", "attumen the huntsman");
        return attumen && AssistTankMoveAttumenFromGroup(midnight, attumen);
    }

    // [kara3] a healer heals: no attack order on Midnight, whose fight brings Attumen's Shadow Cleave (a front cone,
    // 38k) to the melee spot. b356 kara-n 20261108-234616-10A-1 t=352: the holy paladin, sent onto Midnight by this
    // Attack, stood 1.4 yd from him in front of Attumen beside both tanks and died to one Shadow Cleave.
    if (PlayerbotAI::IsHeal(bot))
        return false;

    return AI_VALUE(Unit*, "current target") != midnight && Attack(midnight);
}

bool AttumenTheHuntsmanHandlePhaseOneAction::AssistTankMoveAttumenFromGroup(
    Unit* midnight, Unit* attumen)
{
    if (AI_VALUE(Unit*, "current target") != attumen)
        return Attack(attumen);

    if (attumen->GetVictim() != bot || midnight->GetVictim() == bot)
        return false;

    constexpr float safeDistance = 8.0f;
    Player* nearestPlayer = GetNearestPlayerInRadius(bot, safeDistance);
    if (!nearestPlayer || attumen->GetDistance2d(nearestPlayer) >= safeDistance)
        return false;

    return MoveFromGroup(safeDistance);
}

bool AttumenTheHuntsmanHandlePhaseTwoAction::Execute(Event /*event*/)
{
    Unit* attumen = AI_VALUE2(Unit*, "find target", "attumen the huntsman");
    if (!attumen)
        return false;

    if (AI_VALUE(Unit*, "current target") != attumen)
        return Attack(attumen);

    if (PlayerbotAI::IsTank(bot))
        return CurrentTankPositionAttumen(attumen);

    return StackBehindAttumen(attumen);
}

bool AttumenTheHuntsmanHandlePhaseTwoAction::CurrentTankPositionAttumen(Unit* attumen)
{
    if (attumen->GetVictim() != bot || !bot->IsWithinMeleeRange(attumen))
        return false;

    Position const& position = ATTUMEN_TANK_POSITION;
    float const distToPosition = bot->GetExactDist2d(position);
    if (distToPosition <= 2.0f || !bot->IsWithinLOS(
            position.GetPositionX(), position.GetPositionY(), position.GetPositionZ()))
    {
        return false;
    }

    float const posX = position.GetPositionX();
    float const posY = position.GetPositionY();
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    float const toPosX = posX - botX;
    float const toPosY = posY - botY;
    float const toBossX = attumen->GetPositionX() - botX;
    float const toBossY = attumen->GetPositionY() - botY;
    bool const backwards = (toPosX * toBossX + toPosY * toBossY) < 0.0f;

    float const maxMoveDist = backwards ? 2.25f : 3.5f;
    float const moveDist = std::min(maxMoveDist, distToPosition);
    float const moveX = botX + (toPosX / distToPosition) * moveDist;
    float const moveY = botY + (toPosY / distToPosition) * moveDist;

    return MoveTo(
        KARA_MAP_ID, moveX, moveY, position.GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_COMBAT, true, backwards);
}

// Mounted Attumen's CombatReach is 0 [sic] yards
bool AttumenTheHuntsmanHandlePhaseTwoAction::StackBehindAttumen(Unit* attumen)
{
    float const distanceBehind = bot->getClass() == CLASS_HUNTER? 8.0f : 2.0f;
    float const orientation = attumen->GetOrientation() + M_PI;
    float const rearX = attumen->GetPositionX() + std::cos(orientation) * distanceBehind;
    float const rearY = attumen->GetPositionY() + std::sin(orientation) * distanceBehind;

    if (bot->GetDistance2d(rearX, rearY) <= 0.2f)
        return false;

    return MoveTo(
        KARA_MAP_ID, rearX, rearY, attumen->GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_FORCED, true, false);
}

bool AttumenTheHuntsmanSetDpsTimerAction::Execute(Event /*event*/)
{
    uint32 const instanceId = bot->GetMap()->GetInstanceId();
    time_t const now = std::time(nullptr);
    return attumenDpsWaitTimer.try_emplace(instanceId, now).second;
}

// Moroes

bool MoroesMarkTargetAction::Execute(Event /*event*/)
{
    static constexpr std::array moroesGuests = {
        "baroness dorothea millstipe",
        "lady catriona von'indi",
        "lady keira berrybuck",
        "baron rafe dreuger",
        "lord robin daris",
        "lord crispin ference",
    };

    for (char const* name : moroesGuests)
    {
        if (Unit* guest = AI_VALUE2(Unit*, "find target", name))
            return MarkTargetWithSkull(bot, guest);
    }

    return false;
}

// [kara4] b359 kara-n 20261109-010057-10A-1 t=339-377: the Retainer's Oath of Fealty took the fury warrior, then the
// priest (dead at 377 s); the group fought the guests round it. The skull on it: the members' focus.
bool KarazhanMarkSpectralRetainerAction::Execute(Event /*event*/)
{
    Unit* retainer = AI_VALUE2(Unit*, "find target", "spectral retainer");
    if (!retainer || !retainer->IsAlive())
        return false;
    bool const marked = MarkTargetWithSkull(bot, retainer);
    if (marked)
        LOG_INFO("playerbots", "[kara4] charm: {} marks {} first in its pull (Oath of Fealty)", bot->GetName(),
                 retainer->GetName());
    return marked;
}

// [kara3] b356 kara-n 20261108-234616-10A-1: the death knight tanked Moroes, the paladin tank the guests; Gouge
// sent Moroes to the second on his threat list - the fury warrior (4245 against the paladin's 2204), dead at 1108 s,
// then the warlock at 1145 s. The tank he does not hit holds the second place on his threat list: above every
// non-tank by OFF_TANK_MARGIN, in melee; while it does, it is free for the guests. Moroes on a non-tank (its tank
// gouged): it taunts him at once.
bool MoroesOffTankHoldsSecondThreatAction::Execute(Event /*event*/)
{
    constexpr float OFF_TANK_MARGIN = 1.2f;
    Unit* moroes = AI_VALUE2(Unit*, "find target", "moroes");
    if (!moroes || !moroes->IsAlive() || moroes->GetVictim() == bot)
        return false;
    Unit* victim = moroes->GetVictim();
    Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
    if (victimPlayer && !PlayerbotAI::IsTank(victimPlayer))
    {
        static constexpr std::array<char const*, 4> taunts = {"taunt", "dark command", "hand of reckoning", "growl"};
        for (char const* taunt : taunts)
            if (botAI->CanCastSpell(taunt, moroes) && botAI->CastSpell(taunt, moroes))
            {
                LOG_INFO("playerbots", "[kara3] moroes: {} taunts Moroes off {} ({})", bot->GetName(),
                         victimPlayer->GetName(), taunt);
                return true;
            }
    }
    float const mine = moroes->GetThreatMgr().GetThreat(bot);
    float highest = 0.0f;
    std::string over;
    for (ThreatReference const* ref : moroes->GetThreatMgr().GetSortedThreatList())
    {
        Player* other = ref->GetVictim() ? ref->GetVictim()->ToPlayer() : nullptr;
        if (!other || other == bot || PlayerbotAI::IsTank(other))
            continue;
        if (ref->GetThreat() > highest)
        {
            highest = ref->GetThreat();
            over = other->GetName();
        }
    }
    bool const second = mine >= highest * OFF_TANK_MARGIN && mine > 0.0f;
    if (second && bot->IsWithinMeleeRange(moroes))
        return false;  // its place is held: free for the guests
    if (AI_VALUE(Unit*, "current target") == moroes)
        return false;  // on him already: its rotation and its chase hold the place
    LOG_INFO("playerbots", "[kara3] moroes: {} holds second threat on Moroes ({:.0f} vs {} {:.0f})",
             bot->GetName(), mine, over.empty() ? "nobody" : over, highest);
    return Attack(moroes);
}

// Maiden of Virtue
// CombatReach is 4.8 yards

bool MaidenOfVirtueTankPositionBossAction::Execute(Event /*event*/)
{
    Unit* maiden = AI_VALUE2(Unit*, "find target", "maiden of virtue");
    if (!maiden)
        return false;

    if (AI_VALUE(Unit*, "current target") != maiden)
        return Attack(maiden);

    if (maiden->GetVictim() != bot)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    Player* healer = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !PlayerbotAI::IsHeal(member) ||
            !member->HasAura(Id(KaraSpells::SPELL_REPENTANCE)))
        {
            continue;
        }

        healer = member;
        break;
    }

    if (healer)
        return MoveBossToStunnedHealer(healer);

    if (!bot->IsWithinMeleeRange(maiden))
        return false;

    Position const& position = MAIDEN_OF_VIRTUE_TANK_POSITION;
    float const distToPosition = bot->GetExactDist2d(position);
    if (distToPosition <= 2.0f)
        return false;

    float const posX = position.GetPositionX();
    float const posY = position.GetPositionY();
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const toPosX = posX - botX;
    float const toPosY = posY - botY;

    float const toBossX = maiden->GetPositionX() - botX;
    float const toBossY = maiden->GetPositionY() - botY;
    bool const backwards = (toPosX * toBossX + toPosY * toBossY) < 0.0f;

    float const maxMoveDist = backwards ? 2.25f : 3.5f;
    float const moveDist = std::min(maxMoveDist, distToPosition);
    float const moveX = botX + (toPosX / distToPosition) * moveDist;
    float const moveY = botY + (toPosY / distToPosition) * moveDist;

    return MoveTo(
        KARA_MAP_ID, moveX, moveY, position.GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_COMBAT, true, backwards);
}

bool MaidenOfVirtueTankPositionBossAction::MoveBossToStunnedHealer(Player* healer)
{
    constexpr float endDistance = 6.0f;
    float const angle = healer->GetOrientation();
    float const targetX = healer->GetPositionX() + std::cos(angle) * endDistance;
    float const targetY = healer->GetPositionY() + std::sin(angle) * endDistance;

    return MoveTo(
        KARA_MAP_ID, targetX, targetY, healer->GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_COMBAT, true, false);
}

bool MaidenOfVirtuePositionRangedBetweenPillarsAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    constexpr uint8 maxIndex = 7;
    uint8 index = 0;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !PlayerbotAI::IsRanged(member))
            continue;

        if (member == bot)
            break;

        if (index >= maxIndex)
        {
            index = 0;
            continue;
        }
        index++;
    }

    Position const& position = MAIDEN_OF_VIRTUE_RANGED_POSITIONS[index];
    if (bot->GetExactDist2d(position) <= 2.0f)
        return false;

    return MoveTo(
        KARA_MAP_ID, position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
        false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true, false);
}

bool MaidenOfVirtueSetGroundingTotemAction::Execute(Event /*event*/)
{
    return botAI->CanCastSpell(Id(KaraSpells::SPELL_GROUNDING_TOTEM), bot) &&
        botAI->CastSpell(Id(KaraSpells::SPELL_GROUNDING_TOTEM), bot);
}

// The Big Bad Wolf
// CombatReach is 0 [sic] yards

bool BigBadWolfPositionBossAction::Execute(Event /*event*/)
{
    Unit* wolf = AI_VALUE2(Unit*, "find target", "the big bad wolf");
    if (!wolf)
        return false;

    if (AI_VALUE(Unit*, "current target") != wolf)
        return Attack(wolf);

    if (wolf->GetVictim() != bot || !bot->IsWithinMeleeRange(wolf))
        return false;

    Position const& position = BIG_BAD_WOLF_TANK_POSITION;
    float const distToPosition = bot->GetExactDist2d(position);
    if (distToPosition <= 2.0f)
        return false;

    float const posX = position.GetPositionX();
    float const posY = position.GetPositionY();
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const toPosX = posX - botX;
    float const toPosY = posY - botY;

    float const toBossX = wolf->GetPositionX() - botX;
    float const toBossY = wolf->GetPositionY() - botY;
    bool const backwards = (toPosX * toBossX + toPosY * toBossY) < 0.0f;

    float const maxMoveDist = backwards ? 2.25f : 3.5f;
    float const moveDist = std::min(maxMoveDist, distToPosition);
    float const moveX = botX + (toPosX / distToPosition) * moveDist;
    float const moveY = botY + (toPosY / distToPosition) * moveDist;

    return MoveTo(
        KARA_MAP_ID, moveX, moveY, position.GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_COMBAT, true, backwards);
}

// Run away, little girl, run away
bool BigBadWolfLittleRedRidingHoodRunAwayAction::Execute(Event /*event*/)
{
    Position const& target = BIG_BAD_WOLF_RUN_POSITIONS[_runIndex];

    if (bot->GetExactDist2d(target.GetPositionX(), target.GetPositionY()) < 1.0f)
        _runIndex = (_runIndex + 1) % 4;

    Position const& position = BIG_BAD_WOLF_RUN_POSITIONS[_runIndex];

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
        false, false, false, false, MovementPriority::MOVEMENT_FORCED, true, false);
}

// Romulo and Julianne

bool RomuloAndJulianneMarkTargetAction::Execute(Event /*event*/)
{
    Unit* romulo = AI_VALUE2(Unit*, "find target", "romulo");
    if (!romulo)
        return false;

    Unit* julianne = AI_VALUE2(Unit*, "find target", "julianne");
    if (!julianne)
        return false;

    constexpr float maxPctDifference = 10.0f;
    Unit* target = nullptr;

    if (julianne->GetHealthPct() + maxPctDifference < romulo->GetHealthPct() ||
        julianne->GetHealthPct() < 1.0f)
    {
        target = romulo;
    }
    else if (romulo->GetHealthPct() + maxPctDifference < julianne->GetHealthPct() ||
        romulo->GetHealthPct() < 1.0f)
    {
        target = julianne;
    }
    else
    {
        target = (romulo->GetHealthPct() >= julianne->GetHealthPct()) ? romulo : julianne;
    }

    if (target)
        return MarkTargetWithSkull(bot, target);

    return false;
}

// The Wizard of Oz

bool WizardOfOzMarkTargetAction::Execute(Event /*event*/)
{
    for (char const* name : OZ_TARGETS)
    {
        if (Unit* target = AI_VALUE2(Unit*, "find target", name))
            return MarkTargetWithSkull(bot, target);
    }

    return false;
}

bool WizardOfOzScorchStrawmanAction::Execute(Event /*event*/)
{
    Unit* strawman = AI_VALUE2(Unit*, "find target", "strawman");
    return strawman &&
        botAI->CanCastSpell("scorch", strawman) &&
        botAI->CastSpell("scorch", strawman);
}

// The Curator

bool TheCuratorMarkAstralFlareAction::Execute(Event /*event*/)
{
    Unit* flare = AI_VALUE2(Unit*, "find target", "astral flare");
    return flare && MarkTargetWithSkull(bot, flare);
}

bool TheCuratorPositionBossAction::Execute(Event /*event*/)
{
    Unit* curator = AI_VALUE2(Unit*, "find target", "the curator");
    if (!curator)
        return false;

    if (AI_VALUE(Unit*, "current target") != curator)
        return Attack(curator);

    if (curator->GetVictim() != bot || !bot->IsWithinMeleeRange(curator))
        return false;

    Position const& position = THE_CURATOR_TANK_POSITION;
    float const distToPosition = bot->GetExactDist2d(position);
    if (distToPosition <= 2.0f)
        return false;

    float const posX = position.GetPositionX();
    float const posY = position.GetPositionY();
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const toPosX = posX - botX;
    float const toPosY = posY - botY;

    float const toBossX = curator->GetPositionX() - botX;
    float const toBossY = curator->GetPositionY() - botY;
    bool const backwards = (toPosX * toBossX + toPosY * toBossY) < 0.0f;

    float const maxMoveDist = backwards ? 2.25f : 3.5f;
    float const moveDist = std::min(maxMoveDist, distToPosition);
    float const moveX = botX + (toPosX / distToPosition) * moveDist;
    float const moveY = botY + (toPosY / distToPosition) * moveDist;

    return MoveTo(
        KARA_MAP_ID, moveX, moveY, position.GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_COMBAT, true, backwards);
}

bool TheCuratorSpreadRangedAction::Execute(Event /*event*/)
{
    constexpr float minDistance = 5.0f;
    Player* nearestPlayer = GetNearestPlayerInRadius(bot, minDistance);
    return nearestPlayer && FleePosition(nearestPlayer->GetPosition(), minDistance);
}

// Terestian Illhoof

bool TerestianIllhoofMarkTargetAction::Execute(Event /*event*/)
{
    static constexpr std::array illhoofTargets = {
        "demon chains", "kil'rek", "terestian illhoof", };

    for (char const* name : illhoofTargets)
    {
        if (Unit* target = AI_VALUE2(Unit*, "find target", name))
            return MarkTargetWithSkull(bot, target);
    }

    return false;
}

// Shade of Aran

bool ShadeOfAranRunAwayFromArcaneExplosionAction::Execute(Event /*event*/)
{
    Unit* aran = AI_VALUE2(Unit*, "find target", "shade of aran");
    if (!aran)
        return false;

    constexpr float safeDistance = 20.0f;
    float const currentDistance = bot->GetDistance2d(aran);
    if (currentDistance >= safeDistance)
        return false;

    bot->CastStop();
    return MoveAway(aran, safeDistance - currentDistance);
}

// I will not move when Flame Wreath is cast or the raid blows up
bool ShadeOfAranStopMovingDuringFlameWreathAction::Execute(Event /*event*/)
{
    AI_VALUE(LastMovement&, "last movement").Set(nullptr);
    if (!bot->isMoving())
        return false;

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    return true;
}

bool ShadeOfAranMarkConjuredElementalAction::Execute(Event /*event*/)
{
    std::list<Creature*> creatureList;
    constexpr float searchRadius = 75.0f;

    bot->GetCreatureListWithEntryInGrid(
        creatureList, Id(KaraNpcs::NPC_CONJURED_ELEMENTAL), searchRadius);

    for (Creature* elemental : creatureList)
    {
        if (!elemental || !elemental->IsAlive())
            continue;

        if (!botAI->HasAura("banish", elemental))
            return MarkTargetWithSkull(bot, elemental);
    }

    return false;
}

bool ShadeOfAranRangedMaintainDistanceAction::Execute(Event /*event*/)
{
    Unit* aran = AI_VALUE2(Unit*, "find target", "shade of aran");
    if (!aran)
        return false;

    constexpr float minDistance = 10.0f;
    constexpr float maxDistance = 15.0f;
    float const distanceToBoss = bot->GetDistance2d(aran);

    if (distanceToBoss > maxDistance)
        return MoveTo(aran, maxDistance, MovementPriority::MOVEMENT_COMBAT);

    if (distanceToBoss < minDistance)
        return MoveTo(aran, minDistance, MovementPriority::MOVEMENT_COMBAT);

    return false;
}

// Netherspite
// CombatReach is 18 yards

// The red beam dance (5 seconds in, 5 seconds out)
bool NetherspiteBlockRedBeamAction::Execute(Event /*event*/)
{
    constexpr float searchRadius = 150.0f;
    Unit* redPortal = bot->FindNearestCreature(Id(KaraNpcs::NPC_RED_PORTAL), searchRadius);
    if (!redPortal)
        return false;

    auto [redBlocker, greenBlocker, blueBlocker] = GetCurrentBeamBlockers(bot);

    if (bot != redBlocker)
    {
        _wasBlockingRedBeam = false;
        _redBeamTimerWasSet = false;
        return false;
    }

    if (bot->HasAura(Id(KaraSpells::SPELL_RED_BEAM_DEBUFF)))
    {
        if (!_redBeamTimerWasSet)
        {
            _redBeamMoveTimer = std::time(nullptr);
            _redBeamTimerWasSet = true;
        }
    }
    else
    {
        _redBeamTimerWasSet = false;
    }

    if (!_wasBlockingRedBeam)
    {
        std::map<std::string, std::string> placeholders{{"%player", bot->GetName()}};
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "netherspite_beam_blocking_red", "%player is moving to block the red beam!",
            placeholders);
        bot->Yell(text, LANG_UNIVERSAL);
    }
    _wasBlockingRedBeam = true;

    constexpr uint8 intervalSecs = 5;
    if (_redBeamTimerWasSet && std::time(nullptr) - _redBeamMoveTimer >= intervalSecs)
    {
        _lastBeamMoveSideways = !_lastBeamMoveSideways;
        _redBeamMoveTimer = std::time(nullptr);
    }

    Unit* netherspite = AI_VALUE2(Unit*, "find target", "netherspite");
    if (!netherspite)
        return false;

    constexpr float idealDistance = 18.0f;
    std::vector<Unit*> voidZones;
    Position beamPos;
    if (!FindBeamPosition(netherspite, redPortal, voidZones, idealDistance, beamPos))
        return false;

    if (!_lastBeamMoveSideways)
    {
        return MoveTo(
            KARA_MAP_ID, beamPos.GetPositionX(), beamPos.GetPositionY(), bot->GetPositionZ(),
            false, false, false, false, MovementPriority::MOVEMENT_FORCED, true, false);
    }

    float const length = netherspite->GetExactDist2d(redPortal);
    if (length == 0.0f)
        return false;

    float const dx = (redPortal->GetPositionX() - netherspite->GetPositionX()) / length;
    float const dy = (redPortal->GetPositionY() - netherspite->GetPositionY()) / length;
    float const perpDx = -dy;
    float const perpDy = dx;
    float const sideX = beamPos.GetPositionX() + perpDx * 3.0f;
    float const sideY = beamPos.GetPositionY() + perpDy * 3.0f;

    return MoveTo(
        KARA_MAP_ID, sideX, sideY, bot->GetPositionZ(), false, false, false, false,
        MovementPriority::MOVEMENT_FORCED, true, false);
}

// Two mana-user DPS bots will block the blue beam for each phase (swap at 25 debuff stacks)
bool NetherspiteBlockBlueBeamAction::Execute(Event /*event*/)
{
    constexpr float searchRadius = 150.0f;
    Unit* bluePortal = bot->FindNearestCreature(Id(KaraNpcs::NPC_BLUE_PORTAL), searchRadius);
    if (!bluePortal)
        return false;

    auto [redBlocker, greenBlocker, blueBlocker] = GetCurrentBeamBlockers(bot);

    if (bot != blueBlocker)
    {
        if (_wasBlockingBlueBeam)
        {
            std::map<std::string, std::string> placeholders{{"%player", bot->GetName()}};
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "netherspite_beam_leaving_blue",
                "%player is leaving the blue beam. Next blocker up!", placeholders);
            bot->Yell(text, LANG_UNIVERSAL);
        }
        _wasBlockingBlueBeam = false;
        return false;
    }

    if (!_wasBlockingBlueBeam)
    {
        std::map<std::string, std::string> placeholders{{"%player", bot->GetName()}};
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "netherspite_beam_blocking_blue",
            "%player is moving to block the blue beam!", placeholders);
        bot->Yell(text, LANG_UNIVERSAL);
    }
    _wasBlockingBlueBeam = true;

    Unit* netherspite = AI_VALUE2(Unit*, "find target", "netherspite");
    if (!netherspite)
        return false;

    float const idealDistance = bot->getClass() == CLASS_HUNTER ? 25.0f : 18.0f;
    std::vector<Unit*> voidZones = GetAllVoidZones(bot);
    Position beamPos;

    if (!FindBeamPosition(netherspite, bluePortal, voidZones, idealDistance, beamPos))
        return false;

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, beamPos.GetPositionX(), beamPos.GetPositionY(), bot->GetPositionZ(),
        false, false, false, false, MovementPriority::MOVEMENT_FORCED, true, false);
}

// Two healer bots will block the green beam for each phase (swap at 25 debuff stacks)
// OR one non-mana-user dps bot will block the green beam for an entire phase
bool NetherspiteBlockGreenBeamAction::Execute(Event /*event*/)
{
    constexpr float searchRadius = 150.0f;
    Unit* greenPortal = bot->FindNearestCreature(Id(KaraNpcs::NPC_GREEN_PORTAL), searchRadius);
    if (!greenPortal)
        return false;

    auto [redBlocker, greenBlocker, blueBlocker] = GetCurrentBeamBlockers(bot);

    if (bot != greenBlocker)
    {
        if (_wasBlockingGreenBeam)
        {
            std::map<std::string, std::string> placeholders{{"%player", bot->GetName()}};
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "netherspite_beam_leaving_green",
                "%player is leaving the green beam. Next blocker up!", placeholders);
            bot->Yell(text, LANG_UNIVERSAL);
        }
        _wasBlockingGreenBeam = false;
        return false;
    }

    if (!_wasBlockingGreenBeam)
    {
        std::map<std::string, std::string> placeholders{{"%player", bot->GetName()}};
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "netherspite_beam_blocking_green",
            "%player is moving to block the green beam!", placeholders);
        bot->Yell(text, LANG_UNIVERSAL);
    }
    _wasBlockingGreenBeam = true;

    Unit* netherspite = AI_VALUE2(Unit*, "find target", "netherspite");
    if (!netherspite)
        return false;

    constexpr float idealDistance = 18.0f;
    std::vector<Unit*> voidZones = GetAllVoidZones(bot);
    Position beamPos;

    if (!FindBeamPosition(netherspite, greenPortal, voidZones, idealDistance, beamPos))
        return false;

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, beamPos.GetPositionX(), beamPos.GetPositionY(), bot->GetPositionZ(),
        false, false, false, false, MovementPriority::MOVEMENT_FORCED, true, false);
}

bool NetherspiteAvoidBeamAndVoidZoneAction::Execute(Event /*event*/)
{
    Unit* netherspite = AI_VALUE2(Unit*, "find target", "netherspite");
    if (!netherspite)
        return false;

    std::vector<Unit*> voidZones = GetAllVoidZones(bot);

    constexpr float hazardRadius = 4.0f;
    bool const nearVoidZone = !IsSafePosition(
        bot->GetPositionX(), bot->GetPositionY(), voidZones, hazardRadius);

    constexpr float searchRadius = 150.0f;
    float const bossX = netherspite->GetPositionX();
    float const bossY = netherspite->GetPositionY();
    std::vector<BeamAvoid> beams;

    if (Unit* redPortal = bot->FindNearestCreature(Id(KaraNpcs::NPC_RED_PORTAL), searchRadius))
    {
        float const len = netherspite->GetExactDist2d(redPortal);
        beams.push_back({0.0f, len,
            len > 0.0f ? (redPortal->GetPositionX() - bossX) / len : 0.0f,
            len > 0.0f ? (redPortal->GetPositionY() - bossY) / len : 0.0f});
    }

    if (Unit* bluePortal = bot->FindNearestCreature(Id(KaraNpcs::NPC_BLUE_PORTAL), searchRadius))
    {
        float const len = netherspite->GetExactDist2d(bluePortal);
        beams.push_back({0.0f, len,
            len > 0.0f ? (bluePortal->GetPositionX() - bossX) / len : 0.0f,
            len > 0.0f ? (bluePortal->GetPositionY() - bossY) / len : 0.0f});
    }

    if (Unit* greenPortal = bot->FindNearestCreature(Id(KaraNpcs::NPC_GREEN_PORTAL), searchRadius))
    {
        float const len = netherspite->GetExactDist2d(greenPortal);
        beams.push_back({0.0f, len,
            len > 0.0f ? (greenPortal->GetPositionX() - bossX) / len : 0.0f,
            len > 0.0f ? (greenPortal->GetPositionY() - bossY) / len : 0.0f});
    }

    bool const nearBeam = !IsAwayFromBeams(
        bot->GetPositionX(), bot->GetPositionY(), bossX, bossY, beams);

    if (!nearVoidZone && !nearBeam)
        return false;

    constexpr uint8 numAngles = 36;
    constexpr float stepDist = 0.5f;
    constexpr uint8 numSteps = 56;

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    Position bestCandidate;
    float bestDistSq = std::numeric_limits<float>::max();
    bool found = false;

    for (uint8 i = 0; i < numAngles; ++i)
    {
        float const angle = i * M_PI / 18.0f;
        for (uint8 j = 0; j <= numSteps; ++j)
        {
            float const dist = 2.0f + j * stepDist;
            float candidateX = botX + std::cos(angle) * dist;
            float candidateY = botY + std::sin(angle) * dist;

            if (!IsSafePosition(candidateX, candidateY, voidZones, hazardRadius) ||
                !IsAwayFromBeams(candidateX, candidateY, bossX, bossY, beams))
            {
                continue;
            }

            float dx = candidateX - botX;
            float dy = candidateY - botY;
            float moveDistSq = dx*dx + dy*dy;

            if (!found || moveDistSq < bestDistSq)
            {
                bestCandidate = Position(candidateX, candidateY, bot->GetPositionZ());
                bestDistSq = moveDistSq;
                found = true;
            }
        }
    }

    if (!found)
        return false;

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, bestCandidate.GetPositionX(), bestCandidate.GetPositionY(),
        bestCandidate.GetPositionZ(), false, false, false, false,
        MovementPriority::MOVEMENT_COMBAT, true, false);
}

bool NetherspiteAvoidBeamAndVoidZoneAction::IsAwayFromBeams(
     float x, float y, float botX, float botY, std::vector<BeamAvoid> const& beams)
{
    for (auto const& beam : beams)
    {
        if (beam.maxDist == 0.0f)
            continue;

        float const botdx = x - botX;
        float const botdy = y - botY;
        float const distanceAlongBeam = (botdx * beam.dirX + botdy * beam.dirY);
        float const beamX = botX + beam.dirX * distanceAlongBeam;
        float const beamY = botY + beam.dirY * distanceAlongBeam;
        float const distToBeamSq = (x - beamX) * (x - beamX) + (y - beamY) * (y - beamY);

        constexpr float minDistFromBeamSq = 25.0f;
        if (distToBeamSq < minDistFromBeamSq && distanceAlongBeam > beam.minDist &&
            distanceAlongBeam < beam.maxDist)
        {
            return false;
        }
    }

    return true;
}

bool NetherspiteBanishPhaseAvoidVoidZoneAction::Execute(Event /*event*/)
{
    std::vector<Unit*> voidZones = GetAllVoidZones(bot);
    constexpr float safeDistance = 4.0f;

    for (Unit* voidZone : voidZones)
    {
        if (voidZone->GetEntry() == Id(KaraNpcs::NPC_VOID_ZONE) &&
            bot->GetExactDist2d(voidZone) < safeDistance)
        {
            return FleePosition(voidZone->GetPosition(), safeDistance);
        }
    }

    return false;
}

bool NetherspiteManageTimersAndTrackersAction::Execute(Event /*event*/)
{
    Unit* netherspite = AI_VALUE2(Unit*, "find target", "netherspite");
    if (!netherspite)
        return false;

    uint32 const instanceId = netherspite->GetMap()->GetInstanceId();
    time_t const now = std::time(nullptr);
    bool const isMechanicTracker = IsMechanicTrackerBot(bot, KARA_MAP_ID);
    bool didSomething = false;

    if (IsBanishPhase(netherspite))
    {
        if (isMechanicTracker && netherspiteDpsWaitTimer.erase(instanceId) > 0)
            didSomething = true;

        Action* redAction = context->GetAction("netherspite block red beam");
        if (redAction &&
            static_cast<NetherspiteBlockRedBeamAction*>(redAction)->ResetRedBeamState())
        {
            didSomething = true;
        }

        Action* blueAction = context->GetAction("netherspite block blue beam");
        if (blueAction &&
            static_cast<NetherspiteBlockBlueBeamAction*>(blueAction)->ResetBlueBeamState())
        {
            didSomething = true;
        }

        Action* greenAction = context->GetAction("netherspite block green beam");
        if (greenAction &&
            static_cast<NetherspiteBlockGreenBeamAction*>(greenAction)->ResetGreenBeamState())
        {
            didSomething = true;
        }
    }
    else if (isMechanicTracker && netherspiteDpsWaitTimer.try_emplace(instanceId, now).second)
    {
        didSomething = true;
    }

    return didSomething;
}

// Prince Malchezaar
// CombatReach is 4 yards

bool PrinceMalchezaarEnfeebledBotAvoidHazardAction::Execute(Event /*event*/)
{
    Unit* malchezaar = AI_VALUE2(Unit*, "find target", "prince malchezaar");
    if (!malchezaar)
        return false;

    std::vector<Unit*> infernals = GetSpawnedInfernals(bot);

    constexpr uint8 numAngles = 64;
    constexpr float minSafeBossDistance = 32.0f;
    constexpr float distIncrement = 0.5f;
    constexpr uint8 numSteps = 56;

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const bossX = malchezaar->GetPositionX();
    float const bossY = malchezaar->GetPositionY();
    float bestMoveDistSq = std::numeric_limits<float>::max();
    float bestDestX = 0.0f;
    float bestDestY = 0.0f;
    bool found = false;

    for (int i = 0; i < numAngles; ++i)
    {
        float const angle = (2 * M_PI * i) / numAngles;
        float const dx = std::cos(angle);
        float const dy = std::sin(angle);

        for (uint8 j = 0; j <= numSteps; ++j)
        {
            float const dist = minSafeBossDistance + j * distIncrement;
            float destX = bossX + dx * dist;
            float destY = bossY + dy * dist;
            float destZ = bot->GetPositionZ();
            if (!bot->GetMap()->CheckCollisionAndGetValidCoords(
                    bot, botX, botY, destZ, destX, destY, destZ, true))
            {
                continue;
            }

            float ddx = destX - bossX;
            float ddy = destY - bossY;

            constexpr float minSafeBossDistanceSq = minSafeBossDistance * minSafeBossDistance;
            if (ddx*ddx + ddy*ddy < minSafeBossDistanceSq)
                continue;

            float mdx = destX - botX;
            float mdy = destY - botY;
            float moveDistSq = mdx*mdx + mdy*mdy;

            constexpr float safeInfernalDistance = 22.0f;
            if (IsStraightPathSafe(botX, botY, destX, destY, infernals, safeInfernalDistance) &&
                moveDistSq < bestMoveDistSq)
            {
                bestMoveDistSq = moveDistSq;
                bestDestX = destX;
                bestDestY = destY;
                found = true;
            }
        }
    }

    if (!found)
        return false;

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, bestDestX, bestDestY, bot->GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_FORCED, true, false);
}

bool PrinceMalchezaarNonTankAvoidInfernalAction::Execute(Event /*event*/)
{
    Unit* malchezaar = AI_VALUE2(Unit*, "find target", "prince malchezaar");
    if (!malchezaar)
        return false;

    constexpr float safeInfernalDistance = 22.0f;
    if (!bot->FindNearestCreature(
            Id(KaraNpcs::NPC_NETHERSPITE_INFERNAL), safeInfernalDistance, true))
    {
        return false;
    }

    std::vector<Unit*> infernals = GetSpawnedInfernals(bot);

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    constexpr float maxDistanceFromBoss = 33.0f;

    float bestDestX = botX;
    float bestDestY = botY;

    bool found = TryFindSafePositionWithSafePath(
        bot, botX, botY, malchezaar->GetPositionX(), malchezaar->GetPositionY(),
        infernals, safeInfernalDistance, maxDistanceFromBoss, bestDestX, bestDestY);

    if (!found)
        return false;

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, bestDestX, bestDestY, bot->GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_COMBAT, true, false);
}

bool PrinceMalchezaarTanksPositionBossAction::Execute(Event /*event*/)
{
    Unit* malchezaar = AI_VALUE2(Unit*, "find target", "prince malchezaar");
    if (!malchezaar)
        return false;

    if (AI_VALUE(Unit*, "current target") != malchezaar)
        return Attack(malchezaar);

    if (malchezaar->GetVictim() != bot || !bot->IsWithinMeleeRange(malchezaar))
        return false;

    constexpr float safeInfernalDistance = 30.0f;
    if (!bot->FindNearestCreature(
            Id(KaraNpcs::NPC_NETHERSPITE_INFERNAL), safeInfernalDistance, true))
    {
        return false;
    }

    std::vector<Unit*> infernals = GetSpawnedInfernals(bot);

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    constexpr float maxSampleDist = 75.0f;
    float bestDestX = botX;
    float bestDestY = botY;

    bool found = TryFindSafePositionWithSafePath(
        bot, botX, botY, botX, botY, infernals, safeInfernalDistance,
        maxSampleDist, bestDestX, bestDestY);

    if (!found)
        return false;

    float const toPosX = bestDestX - botX;
    float const toPosY = bestDestY - botY;

    float const toBossX = malchezaar->GetPositionX() - botX;
    float const toBossY = malchezaar->GetPositionY() - botY;
    bool const backwards = (toPosX * toBossX + toPosY * toBossY) < 0.0f;

    return MoveTo(
        KARA_MAP_ID, bestDestX, bestDestY, bot->GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_COMBAT, true, backwards);
}

// Nightbane
// CombatReach is 10.5 yards

bool NightbaneGroundPhaseTanksPositionBossAction::Execute(Event /*event*/)
{
    Unit* nightbane = AI_VALUE2(Unit*, "find target", "nightbane");
    if (!nightbane)
        return false;

    if (AI_VALUE(Unit*, "current target") != nightbane)
        return Attack(nightbane);

    if (nightbane->GetVictim() != bot || !bot->IsWithinMeleeRange(nightbane))
        return false;

    Position const& domeCenter = TERRACE_DOME_CENTER;
    Position const& eastEnd = TERRACE_EAST_END;
    Position const& westEnd = TERRACE_WEST_END;

    float const radius = domeCenter.GetExactDist2d(eastEnd);
    float const thetaA = atan2(
        eastEnd.GetPositionY() - domeCenter.GetPositionY(),
        eastEnd.GetPositionX() - domeCenter.GetPositionX());
    float const thetaB = atan2(
        westEnd.GetPositionY() - domeCenter.GetPositionY(),
        westEnd.GetPositionX() - domeCenter.GetPositionX());

    float deltaAB = thetaB - thetaA;
    if (deltaAB < 0.0f)
        deltaAB += 2.0f * M_PI;

    float arcStart = 0.0f;
    float arcEnd = 0.0f;
    if (deltaAB < M_PI)
    {
        arcStart = thetaB;
        arcEnd = thetaA + 2.0f * M_PI;
    }
    else
    {
        arcStart = thetaA;
        arcEnd = thetaB;
    }

    float thetaN = atan2(
        nightbane->GetPositionY() - domeCenter.GetPositionY(),
        nightbane->GetPositionX() - domeCenter.GetPositionX());

    if (thetaN < arcStart)
        thetaN += 2.0f * M_PI;
    else if (thetaN >= arcStart + 2.0f * M_PI)
        thetaN -= 2.0f * M_PI;

    float const thetaClamped = std::max(arcStart, std::min(arcEnd, thetaN));
    float const destX = domeCenter.GetPositionX() + radius * cos(thetaClamped);
    float const destY = domeCenter.GetPositionY() + radius * sin(thetaClamped);
    float const distToPosition = bot->GetExactDist2d(destX, destY);

    if (distToPosition <= 0.5f)
        return false;

    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const toPosX = destX - botX;
    float const toPosY = destY - botY;

    float const toBossX = nightbane->GetPositionX() - botX;
    float const toBossY = nightbane->GetPositionY() - botY;
    bool const backwards = (toPosX * toBossX + toPosY * toBossY) < 0.0f;

    float const maxMoveDist = backwards ? 2.25f : 3.5f;
    float const moveDist = std::min(maxMoveDist, distToPosition);
    float const moveX = botX + (toPosX / distToPosition) * moveDist;
    float const moveY = botY + (toPosY / distToPosition) * moveDist;

    return MoveTo(
        KARA_MAP_ID, moveX, moveY, bot->GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_FORCED, true, backwards);
}

// Ranged bots will stack on one "ranged leader" that can be designated by the assistant flag
// The ranged leader will lead the ranged group out of Charred Earths
bool NightbaneGroundPhaseCoordinateRangedMovementAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    Player* rangedLeader = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != KARA_MAP_ID ||
            !member->IsAlive() || !PlayerbotAI::IsRanged(member))
        {
            continue;
        }

        if (group->IsAssistant(member->GetGUID()))
        {
            rangedLeader = member;
            break;
        }

        if (!rangedLeader || member->GetGUID() < rangedLeader->GetGUID())
            rangedLeader = member;
    }

    if (!rangedLeader)
        return false;

    if (bot == rangedLeader)
        return MoveRangedLeaderToSafeSpot();

    return StackOnRangedLeader(rangedLeader);
}

bool NightbaneGroundPhaseCoordinateRangedMovementAction::MoveRangedLeaderToSafeSpot()
{
    Unit* nightbane = AI_VALUE2(Unit*, "find target", "nightbane");
    if (!nightbane)
        return false;

    constexpr float searchRadius = 40.0f;
    constexpr float minBossDist = 15.0f;

    std::vector<Position> charredEarths = GetDynamicObjectPositions(
        bot, searchRadius, Id(KaraSpells::SPELL_CHARRED_EARTH));

    if (charredEarths.empty())
    {
        float const distToBoss = bot->GetExactDist2d(nightbane);
        if (distToBoss < minBossDist)
        {
            bot->CastStop();
            return MoveAway(nightbane, minBossDist - distToBoss, true);
        }

        return false;
    }

    constexpr float safeDistance = 12.0f;
    float const safeDistSq = safeDistance * safeDistance;
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    bool inDanger = false;
    for (auto const& charredEarth : charredEarths)
    {
        float dx = botX - charredEarth.GetPositionX();
        float dy = botY - charredEarth.GetPositionY();
        if (dx * dx + dy * dy < safeDistSq)
        {
            inDanger = true;
            break;
        }
    }

    if (!inDanger)
        return false;

    constexpr float maxBossDist = 35.0f;
    constexpr float distStep = 1.0f;
    constexpr uint8 numSteps = 20;
    constexpr uint8 numAngles = 36;

    float const bossX = nightbane->GetPositionX();
    float const bossY = nightbane->GetPositionY();
    float bestDistSq = std::numeric_limits<float>::max();
    float bestX = botX;
    float bestY = botY;
    bool found = false;

    for (uint8 i = 0; i <= numSteps; ++i)
    {
        float const dist = minBossDist + i * distStep;
        for (uint8 j = 0; j < numAngles; ++j)
        {
            float const angle = j * M_PI / 18.0f;
            float candidateX = bossX + cos(angle) * dist;
            float candidateY = bossY + sin(angle) * dist;

            bool safe = true;
            for (auto const& charredEarth : charredEarths)
            {
                float dx = candidateX - charredEarth.GetPositionX();
                float dy = candidateY - charredEarth.GetPositionY();
                if (dx * dx + dy * dy < safeDistSq)
                {
                    safe = false;
                    break;
                }
            }

            if (!safe || nightbane->GetExactDist2d(candidateX, candidateY) > maxBossDist)
                continue;

            float testX = candidateX;
            float testY = candidateY;
            float testZ = bot->GetPositionZ();
            if (!bot->GetMap()->CheckCollisionAndGetValidCoords(
                    bot, bot->GetPositionX(), bot->GetPositionY(),
                    bot->GetPositionZ(), testX, testY, testZ))
            {
                continue;
            }

            if (!nightbane->IsWithinLOS(candidateX, candidateY, bot->GetPositionZ()))
                continue;

            float dx = candidateX - botX;
            float dy = candidateY - botY;
            float moveDistSq = dx * dx + dy * dy;
            if (moveDistSq < bestDistSq)
            {
                bestDistSq = moveDistSq;
                bestX = candidateX;
                bestY = candidateY;
                found = true;
            }
        }
    }

    if (!found)
        return false;

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, bestX, bestY, bot->GetPositionZ(), false, false,
        false, false, MovementPriority::MOVEMENT_FORCED, true, false);
}

bool NightbaneGroundPhaseCoordinateRangedMovementAction::StackOnRangedLeader(Player* rangedLeader)
{
    if (bot->GetExactDist2d(rangedLeader) < 0.5f)
        return false;

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, rangedLeader->GetPositionX(), rangedLeader->GetPositionY(),
        rangedLeader->GetPositionZ(), false, false, false, false,
        MovementPriority::MOVEMENT_FORCED, true, false);
}

bool NightbaneControlPetAggressionAction::Execute(Event /*event*/)
{
    Unit* nightbane = AI_VALUE2(Unit*, "find target", "nightbane");
    if (!nightbane)
        return false;

    Pet* pet = bot->GetPet();
    if (!pet || !pet->IsAlive())
        return false;

    if (nightbane->GetPositionZ() <= NIGHTBANE_FLIGHT_Z && pet->GetReactState() == REACT_PASSIVE)
        pet->SetReactState(REACT_DEFENSIVE);

    if (nightbane->GetPositionZ() > NIGHTBANE_FLIGHT_Z && pet->GetReactState() != REACT_PASSIVE)
    {
        pet->AttackStop();
        pet->SetReactState(REACT_PASSIVE);
    }

    return false;
}

// 1. Stack at the "Flight Stack Position" near Nightbane so he doesn't use Fireball Barrage
// 2. Once Rain of Bones hits, the whole party moves to a new stack position
// This lasts for the first 35 seconds of the flight phase, after which Nightbane begins landing
bool NightbaneFlightPhaseStackAndMoveAction::Execute(Event /*event*/)
{
    Unit* nightbane = AI_VALUE2(Unit*, "find target", "nightbane");
    if (!nightbane)
        return false;

    if (AI_VALUE(Unit*, "current target") == nightbane)
    {
        bot->AttackStop();
        bot->CastStop();
    }

    if (bot->HasAura(Id(KaraSpells::SPELL_RAIN_OF_BONES)))
        _rainOfBonesHit = true;

    auto const& posArray = _rainOfBonesHit ?
        NIGHTBANE_RAIN_OF_BONES_POSITIONS : NIGHTBANE_FLIGHT_STACK_POSITIONS;
    Position destPos;
    bool foundSafe = false;

    constexpr float searchRadius = 40.0f;
    constexpr float charredEarthSafeDist = 12.0f;
    std::vector<Position> charredEarths = GetDynamicObjectPositions(
        bot, searchRadius, Id(KaraSpells::SPELL_CHARRED_EARTH));

    for (uint8 i = 0; i < 2; i++)
    {
        destPos = posArray[i];
        bool inCharredEarth = false;

        for (auto const& charredEarth : charredEarths)
        {
            if (charredEarth.GetExactDist2d(destPos) < charredEarthSafeDist)
            {
                inCharredEarth = true;
                break;
            }
        }

        if (!inCharredEarth)
        {
            foundSafe = true;
            break;
        }
    }

    if (!foundSafe)
        return false;

    if (bot->GetExactDist2d(destPos) < 0.5f)
        return false;

    bot->CastStop();
    return MoveTo(
        KARA_MAP_ID, destPos.GetPositionX(), destPos.GetPositionY(), destPos.GetPositionZ(),
        false, false, false, false, MovementPriority::MOVEMENT_FORCED, true, false);
}

// A bot that fell off the terrace walks back up; it is no longer teleported (bots play without
// cheats). Below the world there is no path, which is only reported.
bool NightbaneTeleportBackToTerraceAction::Execute(Event /*event*/)
{
    Position const& position = NIGHTBANE_TELEPORT_POSITION;
    if (MoveTo(KARA_MAP_ID, position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
               false, false, false, false, MovementPriority::MOVEMENT_FORCED, true, false))
    {
        return true;
    }

    RaidHonesty::LogNeedsHonestTactic(bot, "Nightbane: out of bounds without a path back");
    return false;
}

bool NightbaneManageTimersAndTrackersAction::Execute(Event /*event*/)
{
    Unit* nightbane = AI_VALUE2(Unit*, "find target", "nightbane");
    if (!nightbane)
        return false;

    uint32 const instanceId = nightbane->GetMap()->GetInstanceId();
    time_t const now = std::time(nullptr);
    bool const isMechanicTracker = IsMechanicTrackerBot(bot, KARA_MAP_ID);
    bool didSomething = false;

    if (nightbane->GetPositionZ() <= NIGHTBANE_FLIGHT_Z)
    {
        Action* action = context->GetAction("nightbane flight phase stack and move");
        if (action &&
            static_cast<NightbaneFlightPhaseStackAndMoveAction*>(action)->ResetRainOfBonesHit())
        {
            didSomething = true;
        }

        if (isMechanicTracker)
        {
            if (nightbaneFlightPhaseStartTimer.erase(instanceId) > 0)
                didSomething = true;

            if (nightbaneDpsWaitTimer.try_emplace(instanceId, now).second)
                didSomething = true;
        }
    }
    else if (isMechanicTracker)
    {
        if (nightbaneDpsWaitTimer.erase(instanceId) > 0)
            didSomething = true;

        if (nightbaneFlightPhaseStartTimer.try_emplace(instanceId, now).second)
            didSomething = true;
    }

    return didSomething;
}

// [kara3] Chess Event (boss_chess_event.cpp / instance_karazhan.cpp)
//
// How the game runs in the core: the Echo of Medivh's gossip "We want to play a game against you!" sets up the board
// (64 invisible cell stalkers 22519) and the team of the player who asked (CHESS_EVENT_TEAM). "Control <King>" on the
// own king starts the game; then every own piece offers "Control <piece>": Control Piece 30019 binds the player's sight
// to it and charms it, the piece's four spells are the charm's bar. Spell 0 moves (a destination: the cell stalker
// within 3 yd of it; the script allows one cell for king, rook, bishop and pawn, two for the knight, three for the
// queen, a free cell only), spell 1 turns it (Change Facing, the cell stalker within 4 yd), spells 2 and 3 are its
// abilities. Every piece hits the nearest enemy piece within 10 yd in a 60 degree front arc by itself (Melee Attack
// timer). A piece that dies drops its controller back to the side of the board ("Recently In Game"). Medivh moves his
// pieces himself and cheats every 45-100 s: his king healed, fire on three of the raid's cells (Fury of Medivh, the
// fire creature 22521), or his pieces buffed. The game is won when his king dies, lost when the raid's king dies (the
// game resets, Medivh is asked again).
namespace KaraChess
{
    constexpr uint32 NPC_MEDIVH = 16816;
    constexpr uint32 NPC_CELL = 22519;
    constexpr uint32 NPC_FIRE = 22521;
    constexpr uint32 SPELL_RECENTLY_INGAME = 30529;
    constexpr uint32 DATA_CHESS_EVENT = 8;
    constexpr uint32 DATA_CHESS_EVENT_TEAM = 33;
    constexpr uint32 DATA_CHESS_GAME_PHASE = 35;
    constexpr uint32 PHASE_NOT_STARTED = 0;
    constexpr uint32 PHASE_PVE_WARMUP = 1;
    constexpr uint32 PHASE_INPROGRESS_PVE = 2;
    constexpr float BOARD_X0 = -11108.099609f;  // cell (0, 0): npc_echo_of_medivh::SetupBoard
    constexpr float BOARD_Y0 = -1872.910034f;
    constexpr float GAME_RANGE = 80.0f;         // Medivh to the farthest cell is ~45 yd
    constexpr float CELL_TAKEN = 2.0f;          // a living piece this close to a cell's centre stands on it
    constexpr float FIRE_REACH = 3.5f;          // a fire creature this close to a cell's centre burns it
    constexpr float MELEE_REACH = 10.0f;        // SpellHitTarget SPELL_MELEE_ATTACK_TIMER_TRIGGER: GetEnemyPiece(10)
    constexpr float MELEE_ARC = float(M_PI) / 3.0f;  // GetPiece checkFront
    constexpr float HEAL_BELOW = 75.0f;              // a friendly piece under this health percent is healed
    constexpr float RANGED_HOLD = 18.0f;             // the queen / a bishop acts from here: Holy Lance 18, blasts 20
    constexpr float KING_GUARD = 15.0f;              // an enemy piece this close to the raid's king is met first

    enum class Kind : uint8
    {
        King,
        Queen,
        Bishop,
        Knight,
        Rook,
        Pawn
    };

    struct PieceDef
    {
        uint32 alliance;
        uint32 horde;
        Kind kind;
        int8 step;  // HandlePieceMove: the farthest row / column change of a move
        char const* name;
    };

    std::array<PieceDef, 6> const PIECES = {{
        {21684, 21752, Kind::King, 1, "king"},
        {21683, 21750, Kind::Queen, 3, "queen"},
        {21682, 21747, Kind::Bishop, 1, "bishop"},
        {21664, 21748, Kind::Knight, 2, "knight"},
        {21160, 21726, Kind::Rook, 1, "rook"},
        {17211, 17469, Kind::Pawn, 1, "pawn"},
    }};

    // The order the leader hands the pieces out: the king first (its control starts the game), then the pieces
    // that win the game - the queen's range, the healers, the knights' reach, the rooks; the centre pawns last.
    std::array<Kind, 16> const HAND_OUT = {Kind::King, Kind::Queen, Kind::Bishop, Kind::Bishop,
                                           Kind::Knight, Kind::Knight, Kind::Rook, Kind::Rook,
                                           Kind::Pawn, Kind::Pawn, Kind::Pawn, Kind::Pawn,
                                           Kind::Pawn, Kind::Pawn, Kind::Pawn, Kind::Pawn};

    PieceDef const* DefOf(uint32 entry)
    {
        for (PieceDef const& def : PIECES)
            if (def.alliance == entry || def.horde == entry)
                return &def;
        return nullptr;
    }

    Creature* ChessPiece(Unit* unit)
    {
        return unit && unit->IsCreature() && DefOf(unit->GetEntry()) ? unit->ToCreature() : nullptr;
    }

    // The board cell of a point (the inverse of SetupBoard's x = X0 + 3.49 col + 4.4 row, y = Y0 - 4.4 col + 3.45 row)
    void CellOf(float x, float y, int& row, int& col)
    {
        float const dx = x - BOARD_X0;
        float const dy = y - BOARD_Y0;
        float const det = 3.49f * 3.45f + 4.4f * 4.4f;
        col = int(std::lround((3.45f * dx - 4.4f * dy) / det));
        row = int(std::lround((4.4f * dx + 3.49f * dy) / det));
    }

    struct Board
    {
        InstanceScript* script = nullptr;
        Creature* medivh = nullptr;
        std::vector<Creature*> own;
        std::vector<Creature*> enemy;
        std::vector<Creature*> cells;
        std::vector<Creature*> fires;
        Creature* enemyKing = nullptr;
        Creature* ownKing = nullptr;
    };

    // What a player at the board sees: the pieces of both sides (the raid's side is the team of the player who
    // asked Medivh: CHESS_EVENT_TEAM; before the game, the bot's own), the cells and Medivh's fires.
    bool ReadBoard(Player* bot, Board& board)
    {
        if (bot->GetMapId() != KARA_MAP_ID)
            return false;
        board.script = bot->GetInstanceScript();
        board.medivh = bot->FindNearestCreature(NPC_MEDIVH, GAME_RANGE, true);
        if (!board.script || !board.medivh)
            return false;
        uint32 const team = board.script->GetData(DATA_CHESS_EVENT_TEAM);
        bool const alliance = team == TEAM_ALLIANCE || (team != TEAM_HORDE && bot->GetTeamId() == TEAM_ALLIANCE);
        std::vector<uint32> entries;
        for (PieceDef const& def : PIECES)
        {
            entries.push_back(def.alliance);
            entries.push_back(def.horde);
        }
        std::list<Creature*> pieces;
        board.medivh->GetCreatureListWithEntryInGrid(pieces, entries, GAME_RANGE);
        for (Creature* piece : pieces)
        {
            if (!piece->IsAlive())
                continue;
            PieceDef const* def = DefOf(piece->GetEntry());
            bool const ownSide = alliance ? def->alliance == piece->GetEntry() : def->horde == piece->GetEntry();
            (ownSide ? board.own : board.enemy).push_back(piece);
            if (def->kind == Kind::King)
                (ownSide ? board.ownKing : board.enemyKing) = piece;
        }
        std::list<Creature*> cells;
        board.medivh->GetCreatureListWithEntryInGrid(cells, NPC_CELL, GAME_RANGE);
        board.cells.assign(cells.begin(), cells.end());
        std::list<Creature*> fires;
        board.medivh->GetCreatureListWithEntryInGrid(fires, NPC_FIRE, GAME_RANGE);
        for (Creature* fire : fires)
            if (fire->IsAlive())
                board.fires.push_back(fire);
        return true;
    }

    bool Burning(Board const& board, float x, float y)
    {
        for (Creature* fire : board.fires)
            if (fire->GetExactDist2d(x, y) <= FIRE_REACH)
                return true;
        return false;
    }

    bool Taken(Board const& board, Creature const* cell, Creature const* self)
    {
        for (std::vector<Creature*> const* side : {&board.own, &board.enemy})
            for (Creature* piece : *side)
                if (piece != self && piece->GetExactDist2d(cell) <= CELL_TAKEN)
                    return true;
        return false;
    }

    // The members in the leader's order: the leader first, then by GUID (every member computes the same list).
    std::vector<Player*> Members(Player* bot)
    {
        std::vector<Player*> members;
        Group* group = bot->GetGroup();
        if (!group)
        {
            members.push_back(bot);
            return members;
        }
        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource();
                member && member != leader && member->IsInWorld() && member->GetMap() == bot->GetMap())
                members.push_back(member);
        std::sort(members.begin(), members.end(),
                  [](Player* a, Player* b) { return a->GetGUID().GetCounter() < b->GetGUID().GetCounter(); });
        if (leader && leader->IsInWorld() && leader->GetMap() == bot->GetMap())
            members.insert(members.begin(), leader);
        return members;
    }

    // [kara3] The leader's hand-out: member k takes the k-th piece of HAND_OUT that is alive and not controlled by
    // anybody else; a member that controls a piece keeps it. Pieces of one kind go by their column, the centre first.
    Creature* AssignedPiece(Player* bot, Board const& board)
    {
        std::vector<Player*> const members = Members(bot);
        std::set<ObjectGuid> taken;
        for (Player* member : members)
            if (Creature* charm = ChessPiece(member->GetCharm()))
                taken.insert(charm->GetGUID());
        std::vector<Creature*> order;
        std::array<uint8, 6> used{};
        for (Kind kind : HAND_OUT)
        {
            std::vector<Creature*> ofKind;
            for (Creature* piece : board.own)
                if (DefOf(piece->GetEntry())->kind == kind)
                    ofKind.push_back(piece);
            std::sort(ofKind.begin(), ofKind.end(),
                      [](Creature* a, Creature* b)
                      {
                          int ra = 0, ca = 0, rb = 0, cb = 0;
                          CellOf(a->GetHomePosition().GetPositionX(), a->GetHomePosition().GetPositionY(), ra, ca);
                          CellOf(b->GetHomePosition().GetPositionX(), b->GetHomePosition().GetPositionY(), rb, cb);
                          float const da = std::fabs(float(ca) - 3.5f);
                          float const db = std::fabs(float(cb) - 3.5f);
                          return da != db ? da < db : a->GetGUID().GetCounter() < b->GetGUID().GetCounter();
                      });
            uint8& index = used[uint8(kind)];
            if (index < ofKind.size())
                order.push_back(ofKind[index++]);
        }
        size_t next = 0;
        for (Player* member : members)
        {
            if (Creature* charm = ChessPiece(member->GetCharm()))
            {
                if (member == bot)
                    return charm;
                continue;
            }
            while (next < order.size() && taken.count(order[next]->GetGUID()))
                ++next;
            if (next >= order.size())
                return nullptr;
            if (member == bot)
                return order[next];
            ++next;
        }
        return nullptr;
    }

    bool GossipByText(Player* bot, Creature* npc, std::string const& prefix)
    {
        WorldPacket hello(CMSG_GOSSIP_HELLO);
        hello << npc->GetGUID();
        bot->GetSession()->HandleGossipHelloOpcode(hello);
        GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
        for (auto const& [index, item] : menu.GetMenuItems())
            if (item.Message.rfind(prefix, 0) == 0)
            {
                WorldPacket select(CMSG_GOSSIP_SELECT_OPTION);
                select << npc->GetGUID() << uint32(menu.GetMenuId()) << uint32(index);
                bot->GetSession()->HandleGossipSelectOptionOpcode(select);
                return true;
            }
        bot->PlayerTalkClass->SendCloseGossip();
        return false;
    }

    // As the client's pet bar sends a charm's spell (CMSG_PET_CAST_SPELL: WorldSession::HandlePetCastSpellOpcode
    // runs the charm's own cast checks and starts its cooldown when the cast goes through).
    bool CastPieceSpell(Player* bot, Creature* piece, uint32 spellId, Unit* target, Position const* dest)
    {
        if (!spellId || piece->HasSpellCooldown(spellId) || piece->IsNonMeleeSpellCast(false))
            return false;
        SpellCastTargets targets;
        if (target)
            targets.SetUnitTarget(target);
        if (dest)
            targets.SetDst(dest->GetPositionX(), dest->GetPositionY(), dest->GetPositionZ(), 0.0f);
        WorldPacket packet(CMSG_PET_CAST_SPELL);
        packet << piece->GetGUID() << uint8(0) << spellId << uint8(0);
        targets.Write(packet);
        bot->GetSession()->HandlePetCastSpellOpcode(packet);
        return piece->HasSpellCooldown(spellId) || piece->IsNonMeleeSpellCast(false);
    }

    enum class Use : uint8
    {
        None,
        Heal,       // a friendly target (Healing / Shadow Mend)
        SelfBuff,   // on itself (Shield Block, Water Shield, ...)
        GroupBuff,  // the own pieces round it (Heroism / Bloodlust)
        Single,     // an enemy target in range (Elemental Blast / Fireball)
        Ground,     // an area on an enemy (Rain of Fire / Poison Cloud)
        Around,     // the enemies round it (Geyser, Hellfire, Sweep, Cleave)
        Cone        // the enemies in front of it (Smash, Stomp, Holy Lance, Heroic Blow, ...)
    };

    // The use of an ability from its own spell data (the same table for both sides' pieces).
    Use UseOf(SpellInfo const* info, float& reach)
    {
        reach = 0.0f;
        if (!info)
            return Use::None;
        SpellEffectInfo const& effect = info->Effects[EFFECT_0];
        Targets const targetA = effect.TargetA.GetTarget();
        if (effect.Effect == SPELL_EFFECT_HEAL)
        {
            reach = info->GetMaxRange(true);
            return Use::Heal;
        }
        if (effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA)
        {
            reach = info->GetMaxRange(false);
            return Use::Ground;
        }
        if (targetA == TARGET_UNIT_CASTER)
            return Use::SelfBuff;
        if (targetA == TARGET_UNIT_CONE_ENTRY)
        {
            reach = effect.CalcRadius();
            return Use::Cone;
        }
        if (targetA == TARGET_SRC_CASTER)
        {
            reach = effect.CalcRadius();
            return effect.Effect == SPELL_EFFECT_APPLY_AURA ? Use::GroupBuff : Use::Around;
        }
        if (targetA == TARGET_UNIT_TARGET_ANY || targetA == TARGET_UNIT_TARGET_ENEMY)
        {
            reach = info->GetMaxRange(false);
            return Use::Single;
        }
        return Use::None;
    }

    // The enemy a piece goes for: Medivh's king first, else the weakest in reach.
    Creature* BestEnemy(Creature* piece, Board const& board, float reach, bool front)
    {
        Creature* best = nullptr;
        for (Creature* enemy : board.enemy)
        {
            if (piece->GetExactDist2d(enemy) > reach || (front && !piece->HasInArc(MELEE_ARC, enemy)))
                continue;
            bool const king = enemy == board.enemyKing;
            bool const bestKing = best == board.enemyKing;
            if (!best || (king && !bestKing) || (king == bestKing && enemy->GetHealthPct() < best->GetHealthPct()))
                best = enemy;
        }
        return best;
    }

    Creature* NearestEnemy(Creature* piece, Board const& board, float& distance)
    {
        Creature* nearest = nullptr;
        distance = std::numeric_limits<float>::max();
        for (Creature* enemy : board.enemy)
            if (float const d = piece->GetExactDist2d(enemy); d < distance)
            {
                distance = d;
                nearest = enemy;
            }
        return nearest;
    }

    // A cell the move spell takes the piece to (HandlePieceMove: free, within its step, in the spell's range, not
    // burning), the nearest to the goal; mustLeave: any such cell when none is nearer.
    Creature* MoveCell(Creature* piece, Board const& board, Position const& goal, bool mustLeave, float& gain)
    {
        gain = 0.0f;
        PieceDef const* def = DefOf(piece->GetEntry());
        SpellInfo const* move = sSpellMgr->GetSpellInfo(piece->m_spells[0]);
        if (!def || !move)
            return nullptr;
        float const range = move->GetMaxRange(false);
        int row = 0;
        int col = 0;
        CellOf(piece->GetPositionX(), piece->GetPositionY(), row, col);
        float const here = piece->GetExactDist2d(&goal);
        Creature* best = nullptr;
        float bestDistance = here;
        for (Creature* cell : board.cells)
        {
            int cellRow = 0;
            int cellCol = 0;
            CellOf(cell->GetPositionX(), cell->GetPositionY(), cellRow, cellCol);
            if ((cellRow == row && cellCol == col) || std::abs(cellRow - row) > def->step ||
                std::abs(cellCol - col) > def->step || piece->GetExactDist2d(cell) > range - 0.5f ||
                Taken(board, cell, piece) || Burning(board, cell->GetPositionX(), cell->GetPositionY()))
                continue;
            float const distance = cell->GetExactDist2d(&goal);
            if ((mustLeave && !best) || distance < bestDistance)
            {
                best = cell;
                bestDistance = distance;
            }
        }
        gain = here - bestDistance;
        return best;
    }

    // One line per piece each time its kind of play changes: the log shows the game without a line per tick.
    void Note(Player* bot, Creature* piece, std::string const& what)
    {
        static std::unordered_map<uint64, std::string> last;  // a log memo only
        std::string& seen = last[piece->GetGUID().GetRawValue()];
        if (seen == what)
            return;
        seen = what;
        LOG_INFO("playerbots", "[kara3] chess: {} plays {} ({:.0f} %): {}", bot->GetName(), piece->GetName(),
                 piece->GetHealthPct(), what);
    }

    // One decision for the piece the bot controls: off a burning cell, a heal, an ability, the turn to an enemy at
    // hand, the move toward Medivh's king. The raid's king holds its cell (its loss is the game's); the queen and the
    // bishops stay back while an enemy is in their range.
    bool PlayPiece(Player* bot, Creature* piece, Board const& board)
    {
        if (!piece->IsAlive() || piece->IsNonMeleeSpellCast(false) || piece->isMoving())
            return true;
        PieceDef const* def = DefOf(piece->GetEntry());
        if (!def)
            return false;

        float gain = 0.0f;
        if (Burning(board, piece->GetPositionX(), piece->GetPositionY()))
        {
            Position const away = board.enemyKing ? board.enemyKing->GetPosition() : piece->GetPosition();
            if (Creature* cell = MoveCell(piece, board, away, true, gain))
            {
                Position const dest = cell->GetPosition();
                if (CastPieceSpell(bot, piece, piece->m_spells[0], nullptr, &dest))
                {
                    Note(bot, piece, "leaves a burning cell");
                    return true;
                }
            }
        }

        float nearest = 0.0f;
        Creature* enemy = NearestEnemy(piece, board, nearest);
        for (uint8 slot = 2; slot <= 3; ++slot)
        {
            uint32 const spellId = piece->m_spells[slot];
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            float reach = 0.0f;
            Use const use = UseOf(info, reach);
            if (use == Use::None || piece->HasSpellCooldown(spellId))
                continue;
            std::string const name = info->SpellName[0];
            switch (use)
            {
                case Use::Heal:
                {
                    Creature* hurt = nullptr;
                    for (Creature* own : board.own)
                        if (piece->GetExactDist2d(own) <= reach && own->GetHealthPct() < HEAL_BELOW &&
                            (!hurt || (own == board.ownKing && hurt != board.ownKing) ||
                             (hurt != board.ownKing && own->GetHealthPct() < hurt->GetHealthPct())))
                            hurt = own;
                    if (hurt && CastPieceSpell(bot, piece, spellId, hurt, nullptr))
                    {
                        Note(bot, piece, name + " on " + hurt->GetName());
                        return true;
                    }
                    break;
                }
                case Use::SelfBuff:
                case Use::GroupBuff:
                    if (enemy && nearest <= MELEE_REACH &&
                        CastPieceSpell(bot, piece, spellId, use == Use::SelfBuff ? piece : nullptr, nullptr))
                    {
                        Note(bot, piece, name);
                        return true;
                    }
                    break;
                case Use::Single:
                case Use::Ground:
                    if (Creature* target = BestEnemy(piece, board, reach, false))
                    {
                        Position const dest = target->GetPosition();
                        if (CastPieceSpell(bot, piece, spellId, use == Use::Single ? target : nullptr,
                                           use == Use::Ground ? &dest : nullptr))
                        {
                            Note(bot, piece, name + " on " + target->GetName());
                            return true;
                        }
                    }
                    break;
                case Use::Around:
                case Use::Cone:
                    if (BestEnemy(piece, board, reach, use == Use::Cone) &&
                        CastPieceSpell(bot, piece, spellId, nullptr, nullptr))
                    {
                        Note(bot, piece, name);
                        return true;
                    }
                    break;
                default:
                    break;
            }
        }

        // An enemy at hand but none in front: the piece turns to it (its own melee and its cones hit in front only).
        if (enemy && nearest <= MELEE_REACH)
        {
            if (!BestEnemy(piece, board, MELEE_REACH, true))
            {
                Position const dest = enemy->GetPosition();
                if (CastPieceSpell(bot, piece, piece->m_spells[1], nullptr, &dest))
                    Note(bot, piece, std::string("turns to ") + enemy->GetName());
            }
            return true;  // fighting: it holds its cell
        }

        // No enemy at hand: an enemy piece near the raid's king first (its loss is the game's), else Medivh's king.
        // The raid's king stays; the queen and the bishops wait while an enemy is in their range (they act from
        // there), the others walk on until an enemy stands next to them.
        if (def->kind == Kind::King || !board.enemyKing)
            return true;
        if ((def->kind == Kind::Queen || def->kind == Kind::Bishop) && enemy && nearest <= RANGED_HOLD)
            return true;
        Creature* goalPiece = board.enemyKing;
        if (board.ownKing)
        {
            float threat = KING_GUARD;
            for (Creature* other : board.enemy)
                if (float const d = other->GetExactDist2d(board.ownKing); d < threat)
                {
                    threat = d;
                    goalPiece = other;
                }
        }
        Position const goal = goalPiece->GetPosition();
        if (Creature* cell = MoveCell(piece, board, goal, false, gain); cell && gain > 1.0f)
        {
            Position const dest = cell->GetPosition();
            if (CastPieceSpell(bot, piece, piece->m_spells[0], nullptr, &dest))
                Note(bot, piece,
                     std::string(goalPiece == board.enemyKing ? "moves toward " : "guards the king against ") +
                         goalPiece->GetName());
        }
        return true;
    }

    bool Active(Player* bot)
    {
        if (bot->GetMapId() != KARA_MAP_ID || !bot->IsAlive())
            return false;
        InstanceScript* script = bot->GetInstanceScript();
        if (!script || script->GetData(DATA_CHESS_EVENT) == DONE)
            return false;
        if (ChessPiece(bot->GetCharm()))
            return true;
        if (bot->IsInCombat() || !bot->FindNearestCreature(NPC_MEDIVH, GAME_RANGE, true))
            return false;
        uint32 const phase = script->GetData(DATA_CHESS_GAME_PHASE);
        if (phase == PHASE_NOT_STARTED)
        {
            Group* group = bot->GetGroup();
            return !group || group->IsLeader(bot->GetGUID());
        }
        return phase == PHASE_PVE_WARMUP || phase == PHASE_INPROGRESS_PVE;
    }
}

bool KarazhanChessPlayAction::Execute(Event /*event*/)
{
    using namespace KaraChess;
    Board board;
    if (!ReadBoard(bot, board))
        return false;

    if (Creature* charm = ChessPiece(bot->GetCharm()))
        return PlayPiece(bot, charm, board);

    uint32 const phase = board.script->GetData(DATA_CHESS_GAME_PHASE);
    if (phase == PHASE_NOT_STARTED)
    {
        // the leader asks Medivh for the game (again after a lost one: the board was reset)
        if (bot->GetExactDist(board.medivh) > INTERACTION_DISTANCE - 1.0f)
            return MoveNear(board.medivh, INTERACTION_DISTANCE - 2.0f, MovementPriority::MOVEMENT_NORMAL);
        bool const asked = GossipByText(bot, board.medivh, "We want to play");
        LOG_INFO("playerbots", "[kara3] chess: {} asks {} for the game{}", bot->GetName(), board.medivh->GetName(),
                 asked ? "" : " - no such option");
        return asked;
    }

    if (bot->HasAura(SPELL_RECENTLY_INGAME))
        return false;  // its piece just died: the core lets it take another one once the aura is gone
    Creature* piece = AssignedPiece(bot, board);
    if (!piece)
        return false;
    PieceDef const* def = DefOf(piece->GetEntry());
    // the king's control starts the game; every other piece offers its control only once the game runs
    if (phase == PHASE_PVE_WARMUP && def->kind != Kind::King)
        return false;
    if (bot->GetExactDist(piece) > INTERACTION_DISTANCE - 1.0f)
        return MoveNear(piece, INTERACTION_DISTANCE - 2.0f, MovementPriority::MOVEMENT_NORMAL);
    bool const taken = GossipByText(bot, piece, "Control ");
    LOG_INFO("playerbots", "[kara3] chess: {} takes {} ({}){}", bot->GetName(), piece->GetName(), def->name,
             taken ? "" : " - no Control option");
    return taken;
}
