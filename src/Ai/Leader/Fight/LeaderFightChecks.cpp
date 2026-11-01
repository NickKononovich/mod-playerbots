/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderFightChecks.h"

#include "CellImpl.h"
#include "ChooseTargetActions.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"
#include "DynamicObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "MovementActions.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TerrainHazard.h"
#include <algorithm>
#include <cmath>

namespace Leader::Fight
{
    // the first of `spells` the bot knows and has off cooldown (the highest rank listed last); 0: none
    uint32 LeaderReadySpell(Player* bot, std::initializer_list<uint32> spells)
    {
        uint32 ready = 0;
        for (uint32 spell : spells)
            if (bot->HasSpell(spell) && !bot->HasSpellCooldown(spell))
                ready = spell;
        return ready;
    }

    // the bot's interrupt with no cooldown running (Kick, Pummel, Shield Bash, Counterspell, Wind Shear, Mind
    // Freeze, Silence); 0: none
    uint32 LeaderReadyInterrupt(Player* bot)
    {
        static constexpr uint32 INTERRUPTS[] = {1766,  1767, 1768,  1769,  38768, 6552,  6554, 72,
                                                1671,  1672, 29704, 2139,  57994, 47528, 15487};
        uint32 ready = 0;
        for (uint32 spell : INTERRUPTS)
            if (bot->HasSpell(spell) && !bot->HasSpellCooldown(spell))
                ready = spell;
        return ready;
    }

    float g_spotAcquireMargin = 0.0f;

    float LeaderShootReach(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI ? botAI->GetRange("spell") : 0.0f;
    }

    bool LeaderSeesFrom(Player* bot, Unit* target, Position const& at)
    {
        // WorldObject::IsWithinLOSInMap as the member would stand at `at`: its eye (its collision height) to the
        // target's hit sphere point towards it
        float const eye = at.GetPositionZ() + bot->GetCollisionHeight();
        float ox = 0.0f;
        float oy = 0.0f;
        float oz = 0.0f;
        target->GetHitSpherePointFor(Position(at.GetPositionX(), at.GetPositionY(), eye), ox, oy, oz);
        return target->GetMap()->isInLineOfSight(at.GetPositionX(), at.GetPositionY(), eye, ox, oy, oz,
                                                 bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                 VMAP::ModelIgnoreFlags::Nothing);
    }

    bool HealReaches(Player* tank, Position const& from, Position const& to)
    {
        return from.GetExactDist(to) <= Leader::LEADER_HEAL_REACH_YD - g_spotAcquireMargin &&
               tank->GetMap()->isInLineOfSight(from.GetPositionX(), from.GetPositionY(),
                                               from.GetPositionZ() + 2.0f, to.GetPositionX(), to.GetPositionY(),
                                               to.GetPositionZ() + 2.0f, tank->GetPhaseMask(),
                                               LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
    }

    // the AoE rule around the target: forbidden when a crowd-controlled mob or a mob not in the fight (an
    // unpulled pack) stands within the AoE radius (plus a margin) of it
    Leader::AoeRule LeaderAoeRule(Player* tank, Unit* target, std::string& why)
    {
        std::list<Unit*> nearUnits;
        float const range = LEADER_AOE_RADIUS + LEADER_AOE_GUARD_YD;
        Acore::AnyUnitInObjectRangeCheck check(target, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(target, nearUnits, check);
        Cell::VisitObjects(target, searcher, range);
        for (Unit* unit : nearUnits)
        {
            if (unit == target || !unit->ToCreature() || !tank->IsValidAttackTarget(unit))
                continue;
            if (unit->HasBreakableByDamageCrowdControlAura())
            {
                why = Acore::StringFormat("{} is crowd-controlled {:.0f} yd from the target", unit->GetName(),
                                          target->GetExactDist(unit));
                return Leader::AoeRule::Forbidden;
            }
            if (!unit->IsInCombat())
            {
                why = Acore::StringFormat("{} is not pulled, {:.0f} yd from the target", unit->GetName(),
                                          target->GetExactDist(unit));
                return Leader::AoeRule::Forbidden;
            }
            // [lead90] b131 rfc-h 941.9-943.7 s: "allowed-on-pack" while a Searing Blade Warlock of the pulled
            // pack ran round the corner with no tank threat (not attacking anyone yet, so outside the kill
            // order's headroom check) - the mage's AoE took it (84 threat against the tank's 0)
            if (unit->GetThreatMgr().GetThreat(tank) <= 0.0f)
            {
                why = Acore::StringFormat("{} has no tank threat yet, {:.0f} yd from the target", unit->GetName(),
                                          target->GetExactDist(unit));
                return Leader::AoeRule::Forbidden;
            }
        }
        why = "the pack only";
        return Leader::AoeRule::AllowedOnPack;
    }

    // [lead53] the tank's taunt reach: its known taunt spell's range (0: none known)
    float LeaderTauntReach(Player* tank)
    {
        for (uint32 spell : TANK_TAUNTS)
            if (tank->HasSpell(spell))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                    return info->GetMaxRange(false);
        return 0.0f;
    }

    // the member's walk-along spot behind the tank (two rows of two), on reachable ground
    Position LeaderFollowSpot(Player* tank, uint32 index)
    {
        float const back = 3.0f + 2.5f * float((index / 2) % 2);
        float const side = (index % 2 ? 1.5f : -1.5f);
        float const o = tank->GetOrientation();
        float x = tank->GetPositionX() + std::cos(o + float(M_PI)) * back + std::cos(o + float(M_PI) / 2.0f) * side;
        float y = tank->GetPositionY() + std::sin(o + float(M_PI)) * back + std::sin(o + float(M_PI) / 2.0f) * side;
        float z = tank->GetPositionZ();
        if (!tank->GetMap()->CheckCollisionAndGetValidCoords(tank, tank->GetPositionX(), tank->GetPositionY(),
                                                              tank->GetPositionZ(), x, y, z))
            return tank->GetPosition();
        return Position(x, y, z);
    }

    std::vector<Creature*> const* g_leaderIdle = nullptr;
    std::vector<Position> const* g_leaderCleared = nullptr;
    std::vector<ObjectGuid> const* g_leaderExempt = nullptr;  // [lead24] the committed pull's mobs

    // [lead24] the committed pull's own mobs (target, pack, expected pack)
    std::vector<ObjectGuid> LeaderPullMobs(std::shared_ptr<PullPlanner::Decision> const& commit, ObjectGuid mob)
    {
        std::vector<ObjectGuid> out;
        if (!mob.IsEmpty())
            out.push_back(mob);
        if (commit)
        {
            for (ObjectGuid const guid : {commit->planTarget, commit->attack, commit->lookTarget,
                                          commit->verdictTarget})
                if (!guid.IsEmpty())
                    out.push_back(guid);
            out.insert(out.end(), commit->pack.begin(), commit->pack.end());
            out.insert(out.end(), commit->verdictPack.begin(), commit->verdictPack.end());
        }
        return out;
    }

    bool LeaderOnCleared(Position const& spot)
    {
        if (!g_leaderCleared)
            return false;
        for (Position const& at : *g_leaderCleared)
            if (at.GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) <= 4.0f &&
                std::fabs(at.GetPositionZ() - spot.GetPositionZ()) < 4.0f)
                return true;
        return false;
    }

    // the unpulled mob whose reach (the planner's IdleMobReachesSpot, plus LEADER_SPOT_AGGRO_MARGIN towards it)
    // covers the spot; nullptr: none
    Creature* LeaderAggroAt(Player* bot, Position const& spot)
    {
        if (!Leader::LEADER_SPOT_AGGRO_GUARD || !g_leaderIdle)
            return nullptr;
        for (Creature* mob : *g_leaderIdle)
        {
            if (!mob->IsInWorld() || !mob->IsAlive() || mob->IsInCombat())
                continue;
            // [lead24] the mob being pulled now and its pack: no reason to keep away from them
            if (g_leaderExempt && std::find(g_leaderExempt->begin(), g_leaderExempt->end(), mob->GetGUID()) !=
                                      g_leaderExempt->end())
                continue;
            if (IdleMobReachesSpot(bot, mob, spot))
                return mob;
            float const gap = mob->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
            if (gap > 0.1f && gap < 60.0f)
            {
                float const step = std::min(Leader::LEADER_SPOT_AGGRO_MARGIN, gap) / gap;
                Position const closer(spot.GetPositionX() + (mob->GetPositionX() - spot.GetPositionX()) * step,
                                      spot.GetPositionY() + (mob->GetPositionY() - spot.GetPositionY()) * step,
                                      spot.GetPositionZ());
                if (IdleMobReachesSpot(bot, mob, closer))
                    return mob;
            }
        }
        return nullptr;
    }

    // the walk to the spot passes no unpulled mob's reach (and there is a walk)
    Creature* LeaderAggroOnWalk(Player* bot, Position const& spot, bool& noPath)
    {
        noPath = false;
        if (!Leader::LEADER_SPOT_AGGRO_GUARD || !g_leaderIdle)
            return nullptr;
        Leader::RouteResult const route = Leader::Route(bot, spot);  // [lead99]
        if (!route.found)
        {
            noPath = true;
            return nullptr;
        }
        for (G3D::Vector3 const& point : route.points)
            if (Creature* mob = LeaderAggroAt(bot, Position(point.x, point.y, point.z)))
                return mob;
        return nullptr;
    }

    // a candidate of a spot search: 0 rejected (an unpulled mob reaches it), 1 valid on new ground (kept as the
    // fallback), 2 valid on cleared ground (taken at once)
    int LeaderCandidate(Player* bot, Position const& spot, std::optional<Position>& fallback)
    {
        if (LeaderAggroAt(bot, spot))
            return 0;
        if (!Leader::LEADER_PREFER_CLEARED || LeaderOnCleared(spot))
            return 2;
        if (!fallback)
            fallback = spot;
        return 1;
    }

    // a free spot near the healer, out of every cleave cone, from which it reaches everyone (`keeps`)
    bool LeaderHealerRespot(Player* healer, PlayerbotAI* healerAI,
                            std::function<bool(Position const&)> const& keeps, Position& out)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float STEPS[] = {4.0f, 8.0f, 12.0f, 16.0f};
        constexpr uint32 DIRECTIONS = 16;
        Map* map = healer->GetMap();
        for (float step : STEPS)
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = healer->GetPositionX() + std::cos(angle) * step;
                float y = healer->GetPositionY() + std::sin(angle) * step;
                float z = healer->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(healer, healer->GetPositionX(), healer->GetPositionY(),
                                                          healer->GetPositionZ(), x, y, z))
                    continue;
                if (healerAI && DungeonTactics::SpotInFrontalCleave(healer, healerAI, x, y, 1.0f,
                                                                    20.0f * float(M_PI) / 180.0f))
                    continue;
                Position const spot(x, y, z);
                if (!keeps(spot))
                    continue;
                if (LeaderCandidate(healer, spot, fallback) != 2)
                    continue;
                out = spot;
                return true;
            }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    char const* MoveEventName(Leader::MoveEvent move)
    {
        switch (move)
        {
            case Leader::MoveEvent::MoveTo:
                return "MoveTo";
            case Leader::MoveEvent::StandAt:
                return "StandAt";
            case Leader::MoveEvent::Hold:
                return "Hold";
            default:
                return "None";
        }
    }

    // A free spot out of every cleave cone near the member, nearest first (DungeonTactics's exit steps), that
    // `keeps` (the heal reach).
    bool LeaderCleaveExit(Player* bot, PlayerbotAI* botAI, Position& out,
                          std::function<bool(Position const&)> const& keeps)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float STEPS[] = {4.0f, 7.0f, 10.0f, 13.0f};
        constexpr uint32 DIRECTIONS = 16;
        constexpr float DIST_MARGIN = 1.0f;
        constexpr float ANGLE_MARGIN = 20.0f * float(M_PI) / 180.0f;
        Map* map = bot->GetMap();
        for (float step : STEPS)
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = bot->GetPositionX() + std::cos(angle) * step;
                float y = bot->GetPositionY() + std::sin(angle) * step;
                float z = bot->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                          bot->GetPositionZ(), x, y, z))
                    continue;
                if (DungeonTactics::SpotInFrontalCleave(bot, botAI, x, y, DIST_MARGIN, ANGLE_MARGIN))
                    continue;
                if (!keeps(Position(x, y, z)))
                    continue;
                if (LeaderCandidate(bot, Position(x, y, z), fallback) != 2)
                    continue;
                out.Relocate(x, y, z);
                return true;
            }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    void LeaderCollectHazards(Player* bot, std::vector<HazardZone>& out)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;
        AvoidAoeAction::CollectZones(botAI, out);
        std::list<WorldObject*> objects;
        Acore::AllWorldObjectsInRange check(bot, LEADER_HAZARD_SCAN_YD);
        Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, objects, check,
                                                                                GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
        Cell::VisitObjects(bot, searcher, LEADER_HAZARD_SCAN_YD);
        for (WorldObject* object : objects)
        {
            DynamicObject* dyn = object ? object->ToDynObject() : nullptr;
            if (!dyn || !dyn->IsInWorld() || dyn->GetByteValue(DYNAMICOBJECT_BYTES, 0) != DYNAMIC_OBJECT_AREA_SPELL)
                continue;
            Unit* caster = dyn->GetCaster();
            SpellInfo const* info = sSpellMgr->GetSpellInfo(dyn->GetSpellId());
            if (!caster || !info || info->IsPositive() || !caster->IsHostileTo(bot) ||
                sPlayerbotAIConfig.AoeAvoidSpellWhitelist.count(info->Id))
                continue;
            float const radius = dyn->GetRadius();
            if (radius > 0.0f && radius <= sPlayerbotAIConfig.MaxAoeAvoidRadius)
                out.push_back({dyn->GetPosition(), radius, info->SpellName[LOCALE_enUS]});
        }
        for (DungeonHazard::Circle const& circle : DungeonHazard::Collect(bot))
            if (!circle.chasing)
                out.push_back({Position(circle.x, circle.y, bot->GetPositionZ()), circle.radius,
                               "dungeon hazard"});
    }

    // the circle of `zones` that holds (x, y, z) (with `extra` around it), nullptr: none
    HazardZone const* LeaderZoneAt(std::vector<HazardZone> const& zones, float x, float y, float z, float extra)
    {
        for (HazardZone const& zone : zones)
            if (std::fabs(zone.centre.GetPositionZ() - z) <= LEADER_HAZARD_DZ &&
                zone.centre.GetExactDist2d(x, y) < zone.radius + extra)
                return &zone;
        return nullptr;
    }

    // the spot is fit to stand on in a fight: out of every hazard and cleave cone, off lava / drop-offs
    bool LeaderSpotSafe(Player* bot, PlayerbotAI* botAI, std::vector<HazardZone> const& zones, float x, float y,
                        float z)
    {
        if (LeaderZoneAt(zones, x, y, z, LEADER_HAZARD_EDGE))
            return false;
        if (NearHazard(bot->GetMap(), bot->GetPhaseMask(), x, y, z, LEADER_TERRAIN_EDGE))
            return false;
        return !botAI || !DungeonTactics::SpotInFrontalCleave(bot, botAI, x, y, 1.0f, 20.0f * float(M_PI) / 180.0f);
    }

    // the nearest safe spot around the member that `keeps` (the heal reach), on its floor
    bool LeaderSafeSpot(Player* bot, PlayerbotAI* botAI, std::vector<HazardZone> const& zones,
                        std::function<bool(Position const&)> const& keeps, Position& out)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float STEPS[] = {3.0f, 5.0f, 7.0f, 10.0f, 13.0f, 16.0f};
        constexpr uint32 DIRECTIONS = 16;
        Map* map = bot->GetMap();
        for (float step : STEPS)
            for (uint32 i = 0; i < DIRECTIONS; ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = bot->GetPositionX() + std::cos(angle) * step;
                float y = bot->GetPositionY() + std::sin(angle) * step;
                float z = bot->GetPositionZ();
                if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                          bot->GetPositionZ(), x, y, z) ||
                    std::fabs(z - bot->GetPositionZ()) > 4.0f || !LeaderSpotSafe(bot, botAI, zones, x, y, z))
                    continue;
                Position const spot(x, y, z);
                if (!keeps(spot))
                    continue;
                if (LeaderCandidate(bot, spot, fallback) != 2)
                    continue;
                out = spot;
                return true;
            }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    // a combat spot still fits: in its window from the target (melee: behind it), safe, in heal reach; `sight`:
    // also the line of sight to the target (ranged / healer)
    bool LeaderCombatSpotValid(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& spot, bool melee,
                               std::vector<HazardZone> const& zones,
                               std::function<bool(Position const&)> const& keeps, bool sight)
    {
        float const gap = target->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
        if (melee)
        {
            float const reach = target->GetCombatReach() + bot->GetCombatReach() + 1.0f;
            if (gap > reach || target->HasInArc(float(M_PI), &spot))
                return false;
        }
        else if (PlayerbotAI::IsHeal(bot))
        {
            if (gap < LEADER_HEALER_YD - 6.0f || gap > LEADER_HEALER_YD + 10.0f)
                return false;
        }
        else
        {
            // [lead127] the member's own reach (LeaderShootReach), not a fixed band: within it, and not nearer than
            // half of it (out of the target's melee and cleaves)
            float const reach = LeaderShootReach(bot);
            if (reach <= 0.0f || gap < reach / 2.0f || gap > reach)
                return false;
        }
        if (!LeaderSpotSafe(bot, botAI, zones, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ()) ||
            !keeps(spot))
            return false;
        // [lead127] the game's own sight from the spot (b159: the hunter's spot passed a +2 yd eye-to-eye ray, its
        // shots "no line of sight" all fight)
        return !sight || melee || LeaderSeesFrom(bot, target, spot);
    }

    // a new combat spot: melee behind the target, ranged at its own reach (LeaderShootReach) / the healer at LEADER_HEALER_YD
    // nearest the member's own side, each safe, in heal reach and (ranged) in sight of the target
    bool LeaderCombatSpot(Player* bot, PlayerbotAI* botAI, Unit* target, bool melee,
                          std::vector<HazardZone> const& zones, std::function<bool(Position const&)> const& keeps,
                          Position& out)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        static constexpr float TURNS[] = {0.0f,  0.35f, -0.35f, 0.7f,  -0.7f, 1.05f, -1.05f, 1.4f,
                                          -1.4f, 1.75f, -1.75f, 2.1f,  -2.1f, 2.45f, -2.45f, 3.14f};
        float const base = melee ? target->GetOrientation() + float(M_PI) : target->GetAngle(bot);
        float const distance = melee ? std::max(1.5f, target->GetCombatReach() + bot->GetCombatReach() * 0.5f)
                               : PlayerbotAI::IsHeal(bot) ? LEADER_HEALER_YD
                                                          : LeaderShootReach(bot) - 2.0f;  // [lead127] its reach
        Map* map = target->GetMap();
        for (float turn : TURNS)
        {
            if (melee && std::fabs(turn) > 1.2f)
                break;  // behind it only
            float const angle = base + turn;
            float x = target->GetPositionX() + std::cos(angle) * distance;
            float y = target->GetPositionY() + std::sin(angle) * distance;
            float z = target->GetPositionZ();
            if (!map->CheckCollisionAndGetValidCoords(target, target->GetPositionX(), target->GetPositionY(),
                                                      target->GetPositionZ(), x, y, z) ||
                std::fabs(z - target->GetPositionZ()) > 4.0f)
                continue;
            Position const spot(x, y, z);
            if (!LeaderCombatSpotValid(bot, botAI, target, spot, melee, zones, keeps, true))
                continue;
            if (LeaderCandidate(bot, spot, fallback) != 2)
                continue;
            out = spot;
            return true;
        }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    // [lead21] a mob of the fight: a hostile creature the tank may attack, never a player's summon or guardian
    // (slabs-h 20261026-195203: the tank was told to take back the mage's Mirror Image 1187 times)
    bool LeaderFightMob(Player* tank, Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        return creature && tank->IsValidAttackTarget(creature) && !creature->IsCharmedOwnedByPlayerOrPlayer() &&
               !creature->GetOwnerGUID().IsPlayer() && !creature->GetCharmerOrOwnerPlayerOrPlayerItself();
    }

    // [lead18] the nearest walking patrol (idle, a waypoint walker or on the move, not of the committed pull)
    // around the pull spot or the tank; `gap`: its distance to the nearer of the two, `reach`: its aggro reach
    Creature* LeaderPatrolNear(Player* tank, Position const& spot, PullPlanner::Decision const& d, float& gap,
                               float& reach)
    {
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(tank, 90.0f);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(tank, units, check);
        Cell::VisitObjects(tank, searcher, 90.0f);
        Creature* best = nullptr;
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || !creature->IsAlive() || creature->IsInCombat() ||
                !tank->IsValidAttackTarget(creature) ||
                creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ||
                creature->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE ||  // [lead24] a wanderer is a target
                creature->GetGUID() == d.planTarget || creature->GetGUID() == d.attack ||
                creature->GetGUID() == d.lookTarget || creature->GetGUID() == d.verdictTarget ||
                std::find(d.verdictPack.begin(), d.verdictPack.end(), creature->GetGUID()) != d.verdictPack.end() ||
                std::find(d.pack.begin(), d.pack.end(), creature->GetGUID()) != d.pack.end())
                continue;
            float const closest = std::min(creature->GetExactDist(spot), creature->GetExactDist(tank));
            if (!best || closest < gap)
            {
                best = creature;
                gap = closest;
                reach = creature->GetAggroRange(tank);
            }
        }
        return best;
    }

    // [lead18] LEADER_EDGE_FIRST: the room around `picked` (idle mobs within 40 yd of it, patrols out) as packs
    // (linked within 8 yd); the edge packs have the fewest other packs within 20 yd. The pick moves to the edge
    // pack nearest the tank when `picked` is not on the edge. `order`: the room, edge first (room_order).
    Unit* LeaderEdgePick(Player* tank, Unit* picked, std::string& why, JsonArray& order, std::string& sig)
    {
        std::list<Creature*> idle;
        IdleAggroCheck check{tank};
        Acore::CreatureListSearcher<IdleAggroCheck> searcher(picked, idle, check);
        Cell::VisitObjects(picked, searcher, 40.0f);
        std::vector<Creature*> mobs;
        for (Creature* mob : idle)
            if (mob->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE &&  // [lead24] wanderers are room mobs
                std::fabs(mob->GetPositionZ() - picked->GetPositionZ()) < 6.0f && tank->GetExactDist(mob) < 80.0f)
                mobs.push_back(mob);
        if (mobs.size() < 4)
            return nullptr;
        std::vector<size_t> root(mobs.size());
        for (size_t i = 0; i < root.size(); ++i)
            root[i] = i;
        std::function<size_t(size_t)> const find = [&](size_t i)
        { return root[i] == i ? i : root[i] = find(root[i]); };
        for (size_t i = 0; i < mobs.size(); ++i)
            for (size_t j = i + 1; j < mobs.size(); ++j)
                if (mobs[i]->GetExactDist2d(mobs[j]) <= 8.0f)
                    root[find(i)] = find(j);
        std::map<size_t, std::vector<Creature*>> packs;
        for (size_t i = 0; i < mobs.size(); ++i)
            packs[find(i)].push_back(mobs[i]);
        if (packs.size() < 3)
            return nullptr;
        struct Pack
        {
            std::vector<Creature*> mobs;
            uint32 neighbors = 0;
            float tankDist = 0.0f;
            Creature* nearest = nullptr;
            bool hasPicked = false;
        };
        std::vector<Pack> room;
        for (auto& [key, members] : packs)
        {
            Pack pack;
            pack.mobs = members;
            for (Creature* mob : members)
            {
                float const dist = tank->GetExactDist(mob);
                if (!pack.nearest || dist < pack.tankDist)
                {
                    pack.nearest = mob;
                    pack.tankDist = dist;
                }
                pack.hasPicked |= mob == picked;
            }
            room.push_back(pack);
        }
        for (Pack& a : room)
            for (Pack const& b : room)
            {
                if (&a == &b)
                    continue;
                bool close = false;
                for (Creature* x : a.mobs)
                    for (Creature* y : b.mobs)
                        close |= x->GetExactDist2d(y) <= 20.0f;
                a.neighbors += close ? 1 : 0;
            }
        std::sort(room.begin(), room.end(), [](Pack const& a, Pack const& b)
                  { return a.neighbors != b.neighbors ? a.neighbors < b.neighbors : a.tankDist < b.tankDist; });
        for (Pack const& pack : room)
        {
            order.Raw(JsonObject().Str("first", pack.nearest->GetName()).Num("size", int64(pack.mobs.size()))
                          .Num("neighbors", pack.neighbors).Real("dist", pack.tankDist)
                          .Real("x", pack.nearest->GetPositionX()).Real("y", pack.nearest->GetPositionY())
                          .Bool("picked", pack.hasPicked).Build());
            sig += std::to_string(pack.nearest->GetGUID().GetCounter()) + ",";
        }
        Pack const& edge = room.front();
        Pack const* mine = nullptr;
        for (Pack const& pack : room)
            if (pack.hasPicked)
                mine = &pack;
        if (!mine || mine->neighbors <= edge.neighbors)
            return nullptr;
        why = Acore::StringFormat("{} ({} mobs, {} packs beside it) is the room's edge; {} has {}",
                                  edge.nearest->GetName(), edge.mobs.size(), edge.neighbors, picked->GetName(),
                                  mine->neighbors);
        return edge.nearest;
    }

    // [lead18] a spot the member walks to: a path from where it stands, no idle mob (outside the pull) reaching it
    bool LeaderMemberWalks(Player* bot, Position const& spot, std::list<Creature*> const& idle,
                           std::vector<ObjectGuid> const& pack)
    {
        Leader::RouteResult const route = Leader::Route(bot, spot);  // [lead99]
        if (!route.found || !route.complete)
            return false;
        std::vector<G3D::Vector3> const& points = route.points;
        for (size_t i = 0; i < points.size(); ++i)
        {
            Position const at(points[i].x, points[i].y, points[i].z);
            for (Creature* mob : idle)
                if (std::find(pack.begin(), pack.end(), mob->GetGUID()) == pack.end() &&
                    IdleMobReachesSpot(bot, mob, at))
                    return false;
        }
        return true;
    }

    // [lead45] a boss is always walked to: never a refused pick (its hold is a readiness wait, not a refusal)
    bool IsBossLook(Player* tank, ObjectGuid guid)
    {
        Creature const* creature = tank->GetMap() ? tank->GetMap()->GetCreature(guid) : nullptr;
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
    }

    // [lead63] user: «берите физ. атаку / маг. атаку того, кто бьёт» - an attacker's expected damage per second on
    // `victim`, from the server's numbers: its melee damage range (the unit's final min / max damage) over its
    // attack time, after the victim's armor (Unit::CalcArmorReducedDamage at the attacker's level), times the
    // swings that land (1 - the victim's dodge, parry and miss chance), minus the blocked share (block chance x
    // shield block value); plus the spell it is casting now (its school damage effects over its cast time, at
    // least the global cooldown). `spell`: that spell part
    float AttackerDps(Unit* attacker, Unit* victim, float& spell)
    {
        spell = 0.0f;
        float const minDamage = attacker->GetFloatValue(UNIT_FIELD_MINDAMAGE);
        float const maxDamage = attacker->GetFloatValue(UNIT_FIELD_MAXDAMAGE);
        float const swingSec = std::max(attacker->GetAttackTime(BASE_ATTACK) / 1000.0f, 0.1f);
        uint32 const raw = uint32(std::max(0.0f, (minDamage + maxDamage) / 2.0f));
        float const afterArmor =
            float(Unit::CalcArmorReducedDamage(attacker, victim, raw, nullptr, attacker->GetLevel(), BASE_ATTACK));
        float const avoid = std::clamp((victim->GetUnitDodgeChance() + victim->GetUnitParryChance() +
                                        victim->GetUnitMissChance(BASE_ATTACK)) / 100.0f, 0.0f, 1.0f);
        float const blocked = victim->GetUnitBlockChance() / 100.0f * float(victim->GetShieldBlockValue());
        float const melee = std::max(0.0f, afterArmor * (1.0f - avoid) - blocked) / swingSec;
        if (Spell const* casting = attacker->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (SpellInfo const* info = casting->GetSpellInfo())
            {
                float damage = 0.0f;
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                        damage += float(effect.CalcValue(attacker));
                spell = damage / std::max(info->CalcCastTime(attacker) / 1000.0f, 1.5f);
            }
        return melee + spell;
    }

    // [lead64] where a melee member waits at a pull: short of the meeting point on its own side by
    // LEADER_LOOSE_TANK_YD (next to the tank), never into the mob before the tank has hit it
    Position LeaderMeleeWait(Player* bot, Position const& meet)
    {
        float const dx = bot->GetPositionX() - meet.GetPositionX();
        float const dy = bot->GetPositionY() - meet.GetPositionY();
        float const length = std::hypot(dx, dy);
        if (length <= LEADER_LOOSE_TANK_YD)
            return bot->GetPosition();
        return Position(meet.GetPositionX() + dx / length * LEADER_LOOSE_TANK_YD,
                        meet.GetPositionY() + dy / length * LEADER_LOOSE_TANK_YD, meet.GetPositionZ());
    }

    // [lead42] how many members refused their Attack on `unit` this fight
    int64 RefusedCount(std::map<ObjectGuid, std::set<ObjectGuid>> const& refused, Unit const* unit)
    {
        auto const found = refused.find(unit->GetGUID());
        return found == refused.end() ? 0 : int64(found->second.size());
    }

    // [lead40] the leader's reachability verdict of a mob for `walker`: the route walk to it (EvaluatePath) and the
    // skip_target detour metric (DETOUR_RATIO / DETOUR_EXTRA). Empty: reachable on a fair walk; else why not
    // [lead79] `onRoute`: the mob lies on the route's path (Retarget's route rules): the route IS that walk, so
    // only "no walk at all" refuses it - a winding route is no detour (b122: "detour (534 yd for 102 yd)" on the
    // way to Taragaman, which b100 walked fine)
    std::string LeaderWalkVerdict(Player* walker, Creature* mob, bool onRoute)
    {
        // [lead99] the one route search (Leader::Route); the verdict's text names the path's source and whether
        // it is complete (b138 could not tell a partial path)
        Leader::RouteResult const path = Leader::Route(walker, mob->GetPosition());
        float const straight = walker->GetExactDist(mob);
        // a partial walk still counts when it ends nearer the mob than the walker stands (RoutePath's rule)
        float const left = path.found && !path.points.empty()
                               ? (path.points.back() - G3D::Vector3(mob->GetPositionX(), mob->GetPositionY(),
                                                                    mob->GetPositionZ()))
                                     .length()
                               : straight;
        bool const reachable = path.found && (path.complete || left + 5.0f < straight);
        if (!reachable)
            return Acore::StringFormat("unreachable (no walk for {:.0f} yd; {})", straight, path.Describe());
        float const length = path.length + (path.complete ? 0.0f : left);
        if (!onRoute && length > std::max(DETOUR_RATIO * straight, straight + DETOUR_EXTRA))
            return Acore::StringFormat("detour ({:.0f} yd for {:.0f} yd; {})", length, straight, path.Describe());
        return std::string();
    }

    // [lead39] a walk on the walker's floor: a full path, no step down or up over 2.5 yd (no ledge drop)
    bool LeaderWalkStaysOnFloor(Player* bot, Position const& to)
    {
        if (std::fabs(to.GetPositionZ() - bot->GetPositionZ()) > 4.0f)
            return false;
        Leader::RouteResult const route = Leader::Route(bot, to);  // [lead99]
        if (!route.found || !route.complete)
            return false;
        std::vector<G3D::Vector3> const& points = route.points;
        for (size_t i = 1; i < points.size(); ++i)
            if (std::fabs(points[i].z - points[i - 1].z) > 2.5f)
                return false;
        return true;
    }

    // [lead39] fight_drag: the tank's fight moved by the leader (from, to, the distance, off the meeting point)
    void LogFightDrag(DecisionRun const& run, Player* tank, Position const& to, Position const& meet,
                      char const* why)
    {
        Decisions().Write(run, "leader", "fight_drag", tank->GetGUID(), tank->GetName(), true,
                                      JsonObject().Str("why", why).Real("fromX", tank->GetPositionX())
                                          .Real("fromY", tank->GetPositionY()).Real("toX", to.GetPositionX())
                                          .Real("toY", to.GetPositionY())
                                          .Real("distance", tank->GetExactDist(to))
                                          .Real("offMeetingPoint", meet.GetExactDist(to)));
    }

    // [lead18] the member's spell reach to its target (a hunter 35 yd, a caster 30 yd)
    float LeaderSpellReach(Player* bot) { return bot->getClass() == CLASS_HUNTER ? 35.0f : 30.0f; }

    // [lead18] why a held combat spot no longer fits ("" = it does): out of the member's reach of the target
    // (melee: of its melee reach), out of sight (ranged), a hazard, a cleave, lava / a drop-off, the heal reach
    std::string LeaderCombatSpotBreaks(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& spot,
                                       bool melee, std::vector<HazardZone> const& zones,
                                       std::function<bool(Position const&)> const& keeps, bool sight)
    {
        float const gap = target->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
        if (melee && gap > target->GetCombatReach() + bot->GetCombatReach() + 1.5f)
            return "out of melee reach of the target";
        if (!melee && !PlayerbotAI::IsHeal(bot) && gap > LeaderSpellReach(bot) - g_spotAcquireMargin)
            return "out of spell range of the target";
        if (LeaderZoneAt(zones, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), LEADER_HAZARD_EDGE))
            return "a hazard covers it";
        if (NearHazard(bot->GetMap(), bot->GetPhaseMask(), spot.GetPositionX(), spot.GetPositionY(),
                       spot.GetPositionZ(), LEADER_TERRAIN_EDGE))
            return "lava / a drop-off";
        if (botAI && DungeonTactics::SpotInFrontalCleave(bot, botAI, spot.GetPositionX(), spot.GetPositionY(), 1.0f,
                                                         20.0f * float(M_PI) / 180.0f))
            return "a cleave covers it";
        if (!keeps(spot))
            return "heal reach";
        if (sight && !melee && !PlayerbotAI::IsHeal(bot) &&
            !target->GetMap()->isInLineOfSight(spot.GetPositionX(), spot.GetPositionY(),
                                               spot.GetPositionZ() + 2.0f, target->GetPositionX(),
                                               target->GetPositionY(), target->GetPositionZ() + 2.0f,
                                               target->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                               VMAP::ModelIgnoreFlags::Nothing))
            return "no line of sight to the target";
        return std::string();
    }

    // [lead18] the nearest spot around `from` that fits (ranged: at least 8 yd off the target)
    bool LeaderNearestCombatSpot(Player* bot, PlayerbotAI* botAI, Unit* target, Position const& from, bool melee,
                                 std::vector<HazardZone> const& zones,
                                 std::function<bool(Position const&)> const& keeps, Position& out)
    {
        std::optional<Position> fallback;  // [lead18] new ground, when no cleared spot fits
        constexpr float RINGS[] = {0.0f, 3.0f, 6.0f, 9.0f, 12.0f};
        constexpr uint32 DIRECTIONS = 12;
        Map* map = bot->GetMap();
        for (float ring : RINGS)
            for (uint32 i = 0; i < (ring > 0.0f ? DIRECTIONS : 1u); ++i)
            {
                float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                float x = from.GetPositionX() + std::cos(angle) * ring;
                float y = from.GetPositionY() + std::sin(angle) * ring;
                float z = from.GetPositionZ();
                if (ring > 0.0f && (!map->CheckCollisionAndGetValidCoords(bot, from.GetPositionX(),
                                                                          from.GetPositionY(), from.GetPositionZ(),
                                                                          x, y, z) ||
                                    std::fabs(z - from.GetPositionZ()) > 4.0f))
                    continue;
                Position const spot(x, y, z);
                if (!melee && target->GetExactDist2d(x, y) < 8.0f)
                    continue;
                if (!LeaderCombatSpotBreaks(bot, botAI, target, spot, melee, zones, keeps, true).empty())
                    continue;
                if (LeaderCandidate(bot, spot, fallback) != 2)
                    continue;
                out = spot;
                return true;
            }
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        return false;
    }

    // [lead15] the assigned spot's checks: ground, sight to the fight spot, no idle mob outside the pulled pack
    // reaching it, a walk to it.
    bool LeaderSpotOk(Player* tank, Position const& fight, Position& spot, std::vector<ObjectGuid> const& pack)
    {
        Map* map = tank->GetMap();
        float const ground = map->GetHeight(tank->GetPhaseMask(), spot.GetPositionX(), spot.GetPositionY(),
                                            fight.GetPositionZ() + 5.0f, true, 15.0f);
        if (ground <= INVALID_HEIGHT || std::fabs(ground - fight.GetPositionZ()) > 6.0f)
            return false;
        spot.Relocate(spot.GetPositionX(), spot.GetPositionY(), ground);
        if (!map->isInLineOfSight(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + 2.0f,
                                  fight.GetPositionX(), fight.GetPositionY(), fight.GetPositionZ() + 2.0f,
                                  tank->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            return false;
        std::list<Creature*> idle;
        IdleAggroCheck check{tank};
        Acore::CreatureListSearcher<IdleAggroCheck> searcher(tank, idle, check);
        Cell::VisitObjects(tank, searcher, tank->GetExactDist(spot) + 30.0f);
        for (Creature* mob : idle)
            if (std::find(pack.begin(), pack.end(), mob->GetGUID()) == pack.end() &&
                IdleMobReachesSpot(tank, mob, spot))
                return false;
        return Leader::Route(tank, spot).found;  // [lead99]
    }
}
