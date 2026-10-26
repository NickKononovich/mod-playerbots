/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StratActions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "Map.h"
#include "Playerbots.h"
#include "StratShared.h"
#include "Timer.h"

using namespace Stratholme;

namespace
{
constexpr std::array<float, 6> SPOT_RINGS = {2.5f, 4.0f, 6.0f, 8.0f, 11.0f, 14.0f};
constexpr uint32 SPOT_DIRECTIONS = 16;
constexpr uint32 SPOT_REACH_TRIES = 8;
constexpr uint32 SPOT_KEEP_MS = 4000;
constexpr float SPOT_ARRIVED = 1.0f;

// Righteous Defense taunts up to three mobs off the friendly target; the others taunt the mob itself.
constexpr std::array<char const*, 4> TAUNTS = {"hand of reckoning", "taunt", "growl", "dark command"};
}  // namespace

bool StratStepBehindTankAction::ChooseSpot(Player* tank, std::vector<IdleMob> const& mobs)
{
    struct Candidate
    {
        float x;
        float y;
        float dist;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(SPOT_RINGS.size() * SPOT_DIRECTIONS);
    float const z = tank->GetPositionZ();
    for (float const ring : SPOT_RINGS)
        for (uint32 i = 0; i < SPOT_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / SPOT_DIRECTIONS;
            float const x = tank->GetPositionX() + ring * std::cos(angle);
            float const y = tank->GetPositionY() + ring * std::sin(angle);
            if (SpotDrawsAggro(bot, tank, mobs, x, y, z))
                continue;

            candidates.push_back({x, y, bot->GetExactDist2d(x, y)});
        }

    std::sort(candidates.begin(), candidates.end(),
              [](Candidate const& left, Candidate const& right) { return left.dist < right.dist; });

    uint32 tries = 0;
    for (Candidate const& candidate : candidates)
    {
        if (++tries > SPOT_REACH_TRIES)
            break;

        float x = candidate.x;
        float y = candidate.y;
        float spotZ = z;
        if (!tank->IsWithinLOS(x, y, spotZ + bot->GetCollisionHeight()) ||
            !bot->GetMap()->CanReachPositionAndGetValidCoords(bot, x, y, spotZ))
            continue;

        // the walkable point the map gave back must still be clear
        if (SpotDrawsAggro(bot, tank, mobs, x, y, spotZ))
            continue;

        _spotX = x;
        _spotY = y;
        _spotZ = spotZ;
        _spotExpiresMs = getMSTime() + SPOT_KEEP_MS;
        return true;
    }
    return false;
}

bool StratStepBehindTankAction::Execute(Event /*event*/)
{
    Player* tank = GroupTank(bot);
    if (!tank)
        return false;

    std::vector<IdleMob> const mobs = CollectIdleMobs(botAI);
    if (mobs.empty())
        return false;

    // a kept spot is dropped once a wanderer steps over it
    bool const keep = _spotExpiresMs && getMSTime() < _spotExpiresMs &&
                      !SpotDrawsAggro(bot, tank, mobs, _spotX, _spotY, _spotZ);
    if (!keep && !ChooseSpot(tank, mobs))
        return false;

    if (bot->GetExactDist2d(_spotX, _spotY) <= SPOT_ARRIVED)
        return false;

    return MoveTo(bot->GetMapId(), _spotX, _spotY, _spotZ, false, false, false, false,
                  MovementPriority::MOVEMENT_NORMAL, true);
}

bool StratDefendMemberAction::isUseful()
{
    Player* victim = nullptr;
    return PlayerbotAI::IsTank(bot) && !bot->IsInCombat() && FindSwarmer(bot, victim);
}

bool StratDefendMemberAction::Execute(Event /*event*/)
{
    Player* victim = nullptr;
    Unit* mob = FindSwarmer(bot, victim);
    if (!mob || !victim)
        return false;

    bool taunted = false;
    if (botAI->CanCastSpell("righteous defense", victim))
        taunted = botAI->CastSpell("righteous defense", victim);

    if (!taunted)
        for (char const* taunt : TAUNTS)
            if (botAI->CanCastSpell(taunt, mob))
            {
                taunted = botAI->CastSpell(taunt, mob);
                if (taunted)
                    break;
            }

    LOG_INFO("playerbots", "[strat defend] {}: {} on {} out of the tank's fight ({:.1f} yd), {}", bot->GetName(),
             mob->GetName(), victim->GetName(), bot->GetExactDist(mob), taunted ? "taunted" : "no taunt ready");

    return Attack(mob) || taunted;
}
