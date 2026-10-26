/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UPActions.h"
#include "DungeonInteract.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Pet.h"        // [b36_ymiron]
#include "Playerbots.h"
#include "Spell.h"      // [b36_ymiron]
#include "SpellInfo.h"  // [b36_ymiron]
#include "UPTriggers.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <list>
#include <vector>
#include <unordered_map>

bool AvoidFreezingCloudAction::Execute(Event /*event*/)
{
    Unit* closestTrigger = nullptr;
    GuidVector objects = AI_VALUE(GuidVector, "nearest hostile npcs");

    for (auto i = objects.begin(); i != objects.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_BREATH_TRIGGER)
        {
            if (!closestTrigger || bot->GetExactDist2d(unit) < bot->GetExactDist2d(closestTrigger))
            {
                closestTrigger = unit;
            }
        }
    }

    if (!closestTrigger) { return false; }

    float distance = bot->GetExactDist2d(closestTrigger->GetPosition());
    float radius = 3.0f;
    // Large buffer for this - the radius of the breath is a lot smaller than the graphic, but it looks dumb
    // if the bot stands just outside the hitbox but still visibly in the cloud patches.
    float distanceExtra = 3.0f;

    if (distance < radius + distanceExtra - 1.0f)
    {
        // bot->Yell("MOVING", LANG_UNIVERSAL);
        return MoveAway(closestTrigger, radius + distanceExtra - distance);
    }

    return false;
}

bool AvoidSkadiWhirlwindAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "skadi the ruthless");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    float radius = 5.0f;
    float distanceExtra = 2.0f;

    if (distance < radius + distanceExtra)
    {
        if (PlayerbotAI::IsTank(bot))
        {
            // [b33_skadi] up-h 20261026-000434-up-h-5G-38: Skadi chases his victim through Whirlwind, so a straight
            // MoveAway every tick kited him 90 yd north out of the launcher hall (989-1003 s), down the ramp into
            // the skipped Berserker / Necromancer / Hulk packs; the healer, the Ret and the Frost DK stayed at the
            // launchers 55 yd away and the tank died at 1010 s. The tank now steps out round the group (the healer):
            // the spot farthest from Skadi that stays within SKADI_WW_LEASH_YD of it and wakes no idle mob, or it
            // stays and takes the Whirlwind.
            constexpr float SKADI_WW_LEASH_YD = 15.0f;
            constexpr float SKADI_WW_IDLE_MARGIN = 3.0f;
            float const step = (2.0f * radius) + distanceExtra - distance;
            Unit* anchor = nullptr;
            if (Group* group = bot->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                        PlayerbotAI::IsHeal(member) && (!anchor || bot->GetExactDist(member) < bot->GetExactDist(anchor)))
                        anchor = member;
                }
            float const anchorX = anchor ? anchor->GetPositionX() : SkadiHarpoonStaging().GetPositionX();
            float const anchorY = anchor ? anchor->GetPositionY() : SkadiHarpoonStaging().GetPositionY();

            std::list<Creature*> nearby;
            Acore::AnyUnitInObjectRangeCheck check(bot, 45.0f);
            Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, nearby, check);
            Cell::VisitObjects(bot, searcher, 45.0f);
            float bestScore = -1.0f;
            float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;
            for (uint32 i = 0; i < 16; ++i)
            {
                float const angle = 2.0f * static_cast<float>(M_PI) * i / 16.0f;
                float x = bot->GetPositionX() + std::cos(angle) * step;
                float y = bot->GetPositionY() + std::sin(angle) * step;
                float z = bot->GetPositionZ();
                if (std::hypot(x - anchorX, y - anchorY) > SKADI_WW_LEASH_YD)
                    continue;
                if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                    bot->GetPositionZ(), x, y, z, false))
                    continue;
                bool wakes = false;
                for (Creature* mob : nearby)
                    if (mob && mob->IsAlive() && !mob->IsInCombat() &&
                        MovementAction::IdleAggroBlocker(mob, bot, x, y, z, SKADI_WW_IDLE_MARGIN, false))
                    {
                        wakes = true;
                        break;
                    }
                if (wakes)
                    continue;
                float const score = boss->GetExactDist2d(x, y);
                if (score > bestScore)
                {
                    bestScore = score;
                    bestX = x;
                    bestY = y;
                    bestZ = z;
                }
            }
            if (bestScore <= distance)
                return false;  // nothing better within the leash: take it where the group is
            return MoveTo(bot->GetMapId(), bestX, bestY, bestZ, false, false, false, false,
                          MovementPriority::MOVEMENT_COMBAT);
        }
        // else
        return MoveAway(boss, radius + distanceExtra - distance);
    }

    return false;
}

namespace
{
constexpr float USE_MARGIN_YD = 1.5f;
// gauntlet staging spot next to the three Harpoon Launchers at the east end of the hall
Position const SKADI_HARPOON_STAGING = { 484.0f, -512.0f, 104.8f, 0.0f };
constexpr float SKADI_STAGING_REACHED_YD = 20.0f;
// The tank runs the gauntlet with its healer: no farther ahead of it than this (up-h 2026-10-25 wipe: the tank
// reached the launchers alone, healer 65 yd and the hunter 126 yd behind, under 15 Ymirjar).
constexpr float SKADI_TANK_HEALER_LEASH_YD = 25.0f;
// [b32_pull] up-h 20261025-221216-up-h-5C-46: the walk to the launchers ran at ACTION_RAID over the tank's own
// rotation while Ymirjar Warriors (Hamstring) piled on: 1 -> 4 -> 6 -> 11 attackers in 27 s, the tank made 4 yd/s,
// cast Blood Boil twice in 39 s and died at x 443 of 484; then all five. With this many on it (or half its health
// gone) the tank stands and fights where it is, and walks on once the wave is down.
constexpr size_t SKADI_TANK_HOLD_ATTACKERS = 4;
constexpr float SKADI_TANK_HOLD_HEALTH_PCT = 50.0f;
constexpr float SKADI_STAGING_SPLINE_YD = 5.0f;
constexpr uint32 SKADI_LAUNCHERS[] = {
    UPIds::GO_HARPOON_LAUNCHER_A,
    UPIds::GO_HARPOON_LAUNCHER_B,
    UPIds::GO_HARPOON_LAUNCHER_C,
};
// navmesh spots ~2 yd in front of each launcher (gameobject 192175/192176/192177)
Position const SKADI_LAUNCHER_SPOTS[] = {
    { 489.7f, -509.2f, 105.2f, 0.0f },
    { 487.6f, -515.8f, 104.9f, 0.0f },
    { 478.3f, -523.6f, 104.9f, 0.0f },
};
constexpr size_t SKADI_LAUNCHER_COUNT = sizeof(SKADI_LAUNCHERS) / sizeof(SKADI_LAUNCHERS[0]);
constexpr float LAUNCHER_SPOT_REACHED_YD = 1.5f;
constexpr float LAUNCHER_SEARCH_YD = 8.0f;
// Launch Harpoon 48642 is a cone from the launcher's World Trigger, aimed at Grauf's breach point
constexpr float HARPOON_RANGE_YD = 58.0f;
constexpr float HARPOON_CONE_ARC = static_cast<float>(M_PI) / 3.0f;

}  // namespace

Position const& SkadiHarpoonStaging() { return SKADI_HARPOON_STAGING; }
float SkadiStagingReachedYd() { return SKADI_STAGING_REACHED_YD; }

namespace
{
char const* const BANE_PURGE_SPELLS[] = { "purge", "dispel magic", "spellsteal", "tranquilizing shot" };

char const* FindBanePurge(PlayerbotAI* botAI, Unit* boss)
{
    for (char const* spell : BANE_PURGE_SPELLS)
        if (botAI->CanCastSpell(spell, boss))
            return spell;
    return nullptr;
}
}  // namespace

bool UPAreaTriggerStartAction::Execute(Event /*event*/)
{
    AreaTrigger const* at = sObjectMgr->GetAreaTrigger(_triggerId);
    if (!at || at->map != bot->GetMapId())
        return false;

    if (!bot->IsInAreaTriggerRadius(at))
        return MoveTo(at->map, at->x, at->y, at->z, false, false, false, false, MovementPriority::MOVEMENT_NORMAL);

    // waits inside the box until the group stands with it
    if (!UPEvent::IsReadyToStart(bot) || !DungeonInteract::SendAreaTrigger(bot, _triggerId))
        return false;

    LOG_INFO("playerbots", "{} entered area trigger {} (Utgarde Pinnacle)", bot->GetName(), _triggerId);
    return true;
}

SvalaStartEventAction::SvalaStartEventAction(PlayerbotAI* botAI)
    : UPAreaTriggerStartAction(botAI, "svala start event", UPIds::AT_SVALA_INTRO)
{
}

bool SvalaStartEventAction::isUseful() { return UPEvent::NeedsSvalaStart(botAI); }

SkadiStartEventAction::SkadiStartEventAction(PlayerbotAI* botAI)
    : UPAreaTriggerStartAction(botAI, "skadi start event", UPIds::AT_SKADI_GAUNTLET)
{
}

bool SkadiStartEventAction::isUseful() { return UPEvent::NeedsSkadiStart(botAI); }

bool GortokStartEventAction::Execute(Event /*event*/)
{
    if (!UPEvent::NeedsGortokStart(botAI))
        return false;

    GameObject* generator = UPEvent::GetStasisGenerator(bot);
    if (!generator)
        return false;

    if (!generator->IsWithinDistInMap(bot, generator->GetInteractionDistance() - USE_MARGIN_YD))
        return MoveTo(generator->GetMapId(), generator->GetPositionX(), generator->GetPositionY(),
                      generator->GetPositionZ(), false, false, false, false, MovementPriority::MOVEMENT_NORMAL);

    if (!UPEvent::IsReadyToStart(bot) || !DungeonInteract::UseGameObject(bot, generator))
        return false;

    LOG_INFO("playerbots", "{} used the Stasis Generator (Gortok Palehoof)", bot->GetName());
    return true;
}

bool SvalaAttackChannelerAction::Execute(Event /*event*/)
{
    Creature* channeler = UPEvent::FindRitualChanneler(botAI);
    if (!channeler)
        return false;

    Unit* current = AI_VALUE(Unit*, "current target");
    if (current && current->IsAlive() && current->GetEntry() == UPIds::NPC_RITUAL_CHANNELER)
        return false;

    return Attack(channeler);
}

bool SkadiCrossLaneAction::Execute(Event /*event*/)
{
    float const safeY = UPEvent::BreathSafeY(bot);
    if (safeY == 0.0f)
        return false;

    float x = bot->GetPositionX();
    float y = safeY;
    float z = bot->GetPositionZ();
    if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                        bot->GetPositionZ(), x, y, z))
        return false;

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED, true);
}

bool SkadiAttackHarpoonerAction::Execute(Event /*event*/)
{
    Unit* harpooner = UPEvent::FindStagingHarpooner(botAI);
    if (!harpooner || AI_VALUE(Unit*, "current target") == harpooner)
        return false;

    return Attack(harpooner);
}

namespace
{
// [skadi_behind_tank] The gauntlet hall runs east (+x) to the launchers. A non-tank never walks ahead of the tank on
// the way there: up-h 2026-10-25 (20261025-115805-up-h-5A-48) the tank waited for its healer at the hall's west end
// while the enhancement shaman walked on alone into the first Ymirjar Warrior ("unplanned: Rtshabd", 1 kill, wipe);
// waves 21 and 23 opened the gauntlet the same way (a hunter ahead), survived only by luck.
constexpr float SKADI_BEHIND_TANK_YD = 3.0f;

Player* SkadiGauntletTank(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
            PlayerbotAI::IsTank(member))
            return member;
    }
    return nullptr;
}

// Whether the tank still holds an attacker of the gauntlet that is on someone else: it fights there, not walks on.
bool SkadiTankMustHold(Player* tank)
{
    for (Unit* attacker : tank->getAttackers())
        if (attacker && attacker->IsAlive() && attacker->GetVictim() && attacker->GetVictim() != tank)
            return true;
    return false;
}
}  // namespace

// [b35_up2] up-h 20261026-024524-up-h-5D-35: the restarted gauntlet (areatrigger 4991 at 1402 s) began with harpoons
// still in the mage's, lock's and DK's bags from the reset run, so "skadi harpoon in hand" (62) sent all three
// straight to their launchers at 1405 s, level with and then ahead of the bear (x 394-401 vs 391 at 1419 s), into the
// first wave; the healer trailed 30 yd behind, took the Ymirjar Warriors and died at 1420 s. A carrier now walks to a
// launcher only while (1) Skadi rides a flying Grauf (an encounter reset re-summons Grauf grounded with Skadi off it,
// so a stale harpoon waits for the real harpoon phase), (2) no living gauntlet mob stands on its path or by the
// launcher (the designated harpooner - the group's first carrier - may fire past adds the tank holds at the launchers),
// and (3) it would not pass the tank toward live adds. Otherwise it walks/fights with the group ("skadi gauntlet")
// and fires on a later pass.
namespace
{
constexpr uint32 SKADI_GAUNTLET_MOB_ENTRIES[] = { 26690, 26691, UPIds::NPC_YMIRJAR_HARPOONER };  // Warrior/WD/Harpooner
constexpr float HARPOON_PATH_CORRIDOR_YD = 12.0f;
constexpr float HARPOON_LAUNCHER_CLEAR_YD = 20.0f;
constexpr float HARPOON_AT_SPOT_YD = 6.0f;  // already at its launcher with the group: firing is a click, not a walk
constexpr float HARPOON_PAST_TANK_YD = 3.0f;
constexpr uint32 HARPOON_RUN_CACHE_MS = 500;

Position const& SkadiLauncherSpot(PlayerbotAI* botAI)
{
    int32 const slot = botAI->GetGroupSlotIndex(botAI->GetBot());
    size_t const index = slot >= 0 ? static_cast<size_t>(slot) % SKADI_LAUNCHER_COUNT : 0;
    return SKADI_LAUNCHER_SPOTS[index];
}

bool SkadiInHarpoonPhase(Player* bot)
{
    if (!UPEvent::IsGauntletRunning(bot))
        return false;

    InstanceScript* instance = UPEvent::GetInstance(bot);
    Creature* grauf = UPEvent::GetGrauf(bot);
    Creature* skadi = instance ? instance->GetCreature(UPIds::DATA_SKADI_THE_RUTHLESS) : nullptr;
    return grauf && skadi && skadi->IsAlive() && skadi->GetVehicleBase() == grauf && grauf->IsFlying();
}

// The group's first living non-tank harpoon carrier (group order): it alone may fire past adds the tank holds.
Player* SkadiDesignatedHarpooner(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return bot;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->IsAlive() && member->GetMap() == bot->GetMap() && !PlayerbotAI::IsTank(member) &&
            member->HasItemCount(UPIds::ITEM_HARPOON))
            return member;
    }
    return nullptr;
}

float DistToSegment2d(float px, float py, float ax, float ay, float bx, float by)
{
    float const dx = bx - ax;
    float const dy = by - ay;
    float const len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = std::max(0.0f, std::min(1.0f, t));
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

// nullptr when the walk to the launcher is allowed, else why it is held.
char const* SkadiHarpoonRunBlocker(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!SkadiInHarpoonPhase(bot))
        return "not the harpoon phase (Skadi not on a flying Grauf)";

    Position const& spot = SkadiLauncherSpot(botAI);
    float const toSpot = bot->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
    if (toSpot <= HARPOON_AT_SPOT_YD)
        return nullptr;

    Player* tank = SkadiGauntletTank(bot);
    bool const tankAtStaging =
        !tank || tank->GetExactDist2d(SKADI_HARPOON_STAGING.GetPositionX(), SKADI_HARPOON_STAGING.GetPositionY()) <=
                     SKADI_STAGING_REACHED_YD;
    bool const passesTank =
        tank && !tankAtStaging && spot.GetPositionX() > tank->GetPositionX() + HARPOON_PAST_TANK_YD;
    bool const designated = SkadiDesignatedHarpooner(bot) == bot;

    std::vector<uint32> const entries(std::begin(SKADI_GAUNTLET_MOB_ENTRIES), std::end(SKADI_GAUNTLET_MOB_ENTRIES));
    std::list<Creature*> mobs;
    bot->GetCreatureListWithEntryInGrid(mobs, entries, toSpot + HARPOON_LAUNCHER_CLEAR_YD);
    for (Creature* mob : mobs)
    {
        if (!mob || !mob->IsAlive())
            continue;

        float const launcherDist = mob->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
        bool const heldAtLauncher = designated && tank && mob->GetVictim() == tank &&
                                    launcherDist <= HARPOON_LAUNCHER_CLEAR_YD;
        if (heldAtLauncher)
            continue;

        if (passesTank && mob->GetPositionX() > tank->GetPositionX() - HARPOON_PAST_TANK_YD)
            return "would pass the tank toward live adds";
        if (launcherDist <= HARPOON_LAUNCHER_CLEAR_YD)
            return "a gauntlet mob is alive by the launcher";
        if (DistToSegment2d(mob->GetPositionX(), mob->GetPositionY(), bot->GetPositionX(), bot->GetPositionY(),
                            spot.GetPositionX(), spot.GetPositionY()) <= HARPOON_PATH_CORRIDOR_YD)
            return "a gauntlet mob is alive on the path";
    }
    return nullptr;
}
}  // namespace

bool SkadiHarpoonRunAllowed(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != UPIds::MAP_ID || !bot->HasItemCount(UPIds::ITEM_HARPOON))
        return false;

    struct Cached
    {
        uint32 atMs = 0;
        char const* blocker = nullptr;
        char const* logged = "";
    };
    static thread_local std::unordered_map<ObjectGuid, Cached> cache;
    uint32 const now = getMSTime();
    Cached& c = cache[bot->GetGUID()];
    if (c.atMs && getMSTimeDiff(c.atMs, now) < HARPOON_RUN_CACHE_MS)
        return !c.blocker;

    c.atMs = now ? now : 1;
    c.blocker = SkadiHarpoonRunBlocker(botAI);
    if (c.blocker != c.logged)
    {
        c.logged = c.blocker;
        if (c.blocker)
            LOG_INFO("playerbots", "[b35_up2] {}: harpoon run held ({}), fights with the group", bot->GetName(),
                     c.blocker);
        else
            LOG_INFO("playerbots", "[b35_up2] {}: harpoon run cleared (x {:.1f}), to the launcher", bot->GetName(),
                     bot->GetPositionX());
    }
    return !c.blocker;
}

bool SkadiMoveToHarpoonsAction::isUseful()
{
    if (bot->GetExactDist2d(SKADI_HARPOON_STAGING.GetPositionX(), SKADI_HARPOON_STAGING.GetPositionY()) <=
        SKADI_STAGING_REACHED_YD)
        return false;

    if (PlayerbotAI::IsTank(bot))
    {
        // [skadi_behind_tank] the tank picks up a mob that hits a member before it walks on
        return !SkadiTankMustHold(bot);
    }

    // [skadi_behind_tank] a non-tank at or past the tank on the hall's axis stays (follow / combat run instead)
    Player* tank = SkadiGauntletTank(bot);
    if (tank && tank->GetExactDist2d(SKADI_HARPOON_STAGING.GetPositionX(), SKADI_HARPOON_STAGING.GetPositionY()) >
                    SKADI_STAGING_REACHED_YD &&
        bot->GetPositionX() >= tank->GetPositionX() - SKADI_BEHIND_TANK_YD)
        return false;

    return true;
}

bool SkadiMoveToHarpoonsAction::Execute(Event /*event*/)
{
    if (!PlayerbotAI::IsTank(bot))
        if (Player* tank = SkadiGauntletTank(bot))
            if (tank->GetExactDist2d(SKADI_HARPOON_STAGING.GetPositionX(), SKADI_HARPOON_STAGING.GetPositionY()) >
                SKADI_STAGING_REACHED_YD)
            {
                // [skadi_behind_tank] follow the tank down the hall, a few yards behind it
                float const x = tank->GetPositionX() - SKADI_BEHIND_TANK_YD;
                float const y = tank->GetPositionY();
                float z = tank->GetPositionZ();
                bot->UpdateAllowedPositionZ(x, y, z);
                if (bot->GetExactDist2d(x, y) < 2.0f)
                    return false;

                static thread_local std::unordered_map<ObjectGuid, uint32> lastLogMs;
                uint32 const now = getMSTime();
                uint32& last = lastLogMs[bot->GetGUID()];
                if (!last || getMSTimeDiff(last, now) >= 10 * IN_MILLISECONDS)
                {
                    last = now ? now : 1;
                    LOG_INFO("playerbots", "[skadi_behind_tank] {}: walks the gauntlet behind {} (x {:.1f} of {:.1f})",
                             bot->GetName(), tank->GetName(), bot->GetPositionX(), tank->GetPositionX());
                }
                return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false,
                              MovementPriority::MOVEMENT_COMBAT);
            }

    // [b32_pull] fight the wave where it is: returning false lets the tank's rotation and AoE run
    if (PlayerbotAI::IsTank(bot) && bot->IsInCombat() &&
        bot->GetExactDist2d(SKADI_HARPOON_STAGING) > SKADI_STAGING_REACHED_YD &&
        (bot->getAttackers().size() >= SKADI_TANK_HOLD_ATTACKERS ||
         bot->GetHealthPct() < SKADI_TANK_HOLD_HEALTH_PCT))
    {
        if (bot->isMoving() && bot->movespline->Initialized() && !bot->movespline->Finalized())
        {
            auto const dest = bot->movespline->FinalDestination();
            if (SKADI_HARPOON_STAGING.GetExactDist2d(dest.x, dest.y) < SKADI_STAGING_SPLINE_YD)
                bot->StopMoving();
        }
        return false;
    }

    if (PlayerbotAI::IsTank(bot) && bot->GetExactDist2d(SKADI_HARPOON_STAGING) > SKADI_STAGING_REACHED_YD)
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
                    !PlayerbotAI::IsHeal(member) || bot->GetExactDist(member) <= SKADI_TANK_HEALER_LEASH_YD)
                    continue;

                // Ahead of the healer: wait for it, the mobs come to the tank where it stands.
                if (bot->isMoving())
                    bot->StopMoving();
                return true;
            }

    return MoveTo(bot->GetMapId(), SKADI_HARPOON_STAGING.GetPositionX(), SKADI_HARPOON_STAGING.GetPositionY(),
                  SKADI_HARPOON_STAGING.GetPositionZ(), false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT);
}

bool SkadiPickUpHarpoonAction::Execute(Event /*event*/)
{
    GameObject* harpoon = bot->FindNearestGameObject(UPIds::GO_HARPOON, UPEvent::HARPOON_SEARCH_YD, true);
    if (!harpoon)
        return false;

    if (!harpoon->IsWithinDistInMap(bot, harpoon->GetInteractionDistance() - USE_MARGIN_YD))
        return MoveTo(harpoon->GetMapId(), harpoon->GetPositionX(), harpoon->GetPositionY(), harpoon->GetPositionZ(),
                      false, false, false, false, MovementPriority::MOVEMENT_COMBAT);

    // a player stops casting to click; the Harpoon casts Create Harpoon (56790) on the user
    bot->InterruptNonMeleeSpells(false);
    return DungeonInteract::UseGameObject(bot, harpoon);
}

bool SkadiFireHarpoonAction::Execute(Event /*event*/)
{
    int32 const slot = botAI->GetGroupSlotIndex(bot);
    size_t const index = slot >= 0 ? static_cast<size_t>(slot) % SKADI_LAUNCHER_COUNT : 0;
    Position const& spot = SKADI_LAUNCHER_SPOTS[index];
    // [b35_up2] re-checked here: the trigger runs every 500 ms and a wave can walk onto the path in between
    if (!SkadiHarpoonRunAllowed(botAI))
    {
        if (bot->isMoving() && bot->movespline->Initialized() && !bot->movespline->Finalized())
        {
            auto const dest = bot->movespline->FinalDestination();
            if (spot.GetExactDist2d(dest.x, dest.y) < SKADI_STAGING_SPLINE_YD)
                bot->StopMoving();
        }
        return false;
    }
    if (bot->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) > LAUNCHER_SPOT_REACHED_YD)
        return MoveTo(bot->GetMapId(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), false, false,
                      false, false, MovementPriority::MOVEMENT_COMBAT);

    GameObject* launcher = bot->FindNearestGameObject(SKADI_LAUNCHERS[index], LAUNCHER_SEARCH_YD, true);
    if (!launcher || launcher->HasGameObjectFlag(GO_FLAG_IN_USE))
        return false;

    Creature* grauf = UPEvent::GetGrauf(bot);
    Creature* aim = launcher->FindNearestCreature(UPIds::NPC_HARPOON_LAUNCHER_TRIGGER, LAUNCHER_SEARCH_YD);
    if (!grauf || !aim)
        return false;

    // wait for Grauf at the breach, inside the launcher's cone (else the harpoon hits the breach trigger)
    if (aim->GetExactDist(grauf) > HARPOON_RANGE_YD || !aim->HasInArc(HARPOON_CONE_ARC, grauf))
        return false;

    bot->InterruptNonMeleeSpells(false);
    if (!DungeonInteract::UseGameObject(bot, launcher))
        return false;

    LOG_INFO("playerbots", "{} fired a Harpoon Launcher at Grauf ({}% health)", bot->GetName(),
             static_cast<uint32>(grauf->GetHealthPct()));
    return true;
}

bool YmironPurgeBaneAction::isUseful()
{
    if (!UPEvent::IsBaneUp(botAI))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "king ymiron");
    return boss && FindBanePurge(botAI, boss);
}

bool YmironPurgeBaneAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "king ymiron");
    if (!boss)
        return false;

    char const* spell = FindBanePurge(botAI, boss);
    return spell && botAI->CastSpell(spell, boss);
}

// [b36_ymiron]
bool YmironBaneAllowsSpell(SpellInfo const* info, Unit* target, Unit* boss)
{
    if (!info || info->IsPositive())
        return true;

    if (info->HasEffect(SPELL_EFFECT_INTERRUPT_CAST) || info->HasEffect(SPELL_EFFECT_ATTACK_ME) ||
        info->HasAura(SPELL_AURA_MOD_TAUNT) || info->HasEffect(SPELL_EFFECT_DISPEL) ||
        info->HasEffect(SPELL_EFFECT_STEAL_BENEFICIAL_BUFF))
        return true;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = info->Effects[i];
        if (effect.IsEffect() && (effect.IsTargetingArea() || effect.IsAreaAuraEffect()))
            return false;
    }

    return target && boss && target != boss;
}

namespace
{
// [b36_ymiron] the bot's own cast in a slot that would deal damage to Ymiron
bool CastHitsYmiron(Player* bot, CurrentSpellTypes slot, Unit* boss)
{
    Spell* spell = bot->GetCurrentSpell(slot);
    if (!spell || !spell->m_spellInfo)
        return false;

    Unit* target = ObjectAccessor::GetUnit(*bot, spell->m_targets.GetUnitTargetGUID());
    return !YmironBaneAllowsSpell(spell->m_spellInfo, target, boss);
}

bool PetHitsYmiron(Player* bot, Unit* boss)
{
    Pet* pet = bot->GetPet();
    return pet && pet->IsAlive() && pet->GetVictim() == boss;
}
}  // namespace

bool YmironHoldDamageAction::isUseful()
{
    if (!UPEvent::IsBaneThreat(botAI))
        return false;

    Unit* boss = UPEvent::GetYmiron(botAI);
    if (!boss)
        return false;

    return bot->GetVictim() == boss || bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) ||
           CastHitsYmiron(bot, CURRENT_GENERIC_SPELL, boss) || CastHitsYmiron(bot, CURRENT_CHANNELED_SPELL, boss) ||
           PetHitsYmiron(bot, boss);
}

bool YmironHoldDamageAction::Execute(Event /*event*/)
{
    Unit* boss = UPEvent::GetYmiron(botAI);
    if (!boss)
        return false;

    if (bot->GetVictim() == boss)
        bot->AttackStop();

    if (bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

    if (CastHitsYmiron(bot, CURRENT_GENERIC_SPELL, boss))
        bot->InterruptSpell(CURRENT_GENERIC_SPELL);

    if (CastHitsYmiron(bot, CURRENT_CHANNELED_SPELL, boss))
        bot->InterruptSpell(CURRENT_CHANNELED_SPELL);

    if (Pet* pet = bot->GetPet(); pet && PetHitsYmiron(bot, boss))
    {
        pet->AttackStop();
        if (CharmInfo* charmInfo = pet->GetCharmInfo())
        {
            charmInfo->SetIsCommandAttack(false);
            charmInfo->SetIsCommandFollow(true);
        }
        pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    }

    LOG_INFO("playerbots", "[b36_ymiron] {} holds damage on King Ymiron: Bane {}", bot->GetName(),
             UPEvent::IsBaneUp(botAI) ? "up" : "casting");
    return true;
}
