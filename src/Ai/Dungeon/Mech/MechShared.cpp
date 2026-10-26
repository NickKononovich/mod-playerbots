/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MechShared.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DungeonHoldFire.h"
#include "DungeonInteract.h"
#include "DynamicObject.h"
#include "GenericSpellActions.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "MechTriggers.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TestRunnerStrategy.h"
#include "Timer.h"
#include "Transport.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    struct V2
    {
        float x;
        float y;
    };
    constexpr V2 ROOM_POLY[] = {
        { MechanarFlames::ROOM_X_MIN, MechanarFlames::ROOM_Y_MIN },
        { MechanarFlames::ROOM_X_MAX, MechanarFlames::ROOM_Y_MIN },
        { MechanarFlames::ROOM_X_MAX, MechanarFlames::ROOM_Y_MAX },
        { MechanarFlames::ROOM_X_MIN, MechanarFlames::ROOM_Y_MAX } };
    constexpr int ROOM_POLY_N = 4;

    void ClosestOnSegment(float px, float py, float ax, float ay, float bx, float by, float& cx, float& cy)
    {
        float const dx = bx - ax, dy = by - ay;
        float const len2 = dx * dx + dy * dy;
        float t = (len2 < 1e-6f) ? 0.0f : ((px - ax) * dx + (py - ay) * dy) / len2;
        t = std::max(0.0f, std::min(1.0f, t));
        cx = ax + t * dx;
        cy = ay + t * dy;
    }
}

namespace MechanarFlames
{
    bool InRoom(float x, float y)
    {
        bool inside = false;
        int j = ROOM_POLY_N - 1;
        for (int i = 0; i < ROOM_POLY_N; ++i)
        {
            float const xi = ROOM_POLY[i].x, yi = ROOM_POLY[i].y;
            float const xj = ROOM_POLY[j].x, yj = ROOM_POLY[j].y;
            if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi + 1e-6f) + xi))
                inside = !inside;
            j = i;
        }
        return inside;
    }

    bool SegmentInRoom(float ax, float ay, float bx, float by)
    {
        return InRoom(ax, ay) && InRoom(bx, by) && InRoom((ax + bx) * 0.5f, (ay + by) * 0.5f);
    }

    void ClampIntoRoom(float& x, float& y)
    {
        if (InRoom(x, y))
            return;

        float best = 1e18f, resX = x, resY = y;
        int j = ROOM_POLY_N - 1;
        for (int i = 0; i < ROOM_POLY_N; ++i)
        {
            float cx, cy;
            ClosestOnSegment(x, y, ROOM_POLY[j].x, ROOM_POLY[j].y, ROOM_POLY[i].x, ROOM_POLY[i].y, cx, cy);
            float const d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
            if (d < best)
            {
                best = d;
                resX = cx;
                resY = cy;
            }
            j = i;
        }

        float tx = ROOM_ANCHOR_X - resX, ty = ROOM_ANCHOR_Y - resY;
        float const len = std::sqrt(tx * tx + ty * ty);
        if (len > 1e-3f)
        {
            resX += tx / len * 0.5f;
            resY += ty / len * 0.5f;
        }
        x = resX;
        y = resY;
    }

    bool HealerHoldsFire(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!PlayerbotAI::IsHeal(bot))
            return false;
        if (bot->GetHealthPct() <= HEALER_FIRE_BAIL_PCT)
            return false;
        // [enc-mech-healer] Never hold next to a flame: its Raging Flames aura (35312) hit the healer five times in one
        // tick, 3.1k of 6k in a second (mech-n 2026-10-25 115805 12:18:23-24, Rldruaf 100 % -> dead), so the 50 % bail
        // comes one tick too late. Holding is for a trail patch only.
        if (IsFlameNearCached(bot))
            return false;
        return botAI->GetAiObjectContext()->GetValue<Unit*>("party member to heal")->Get() != nullptr;
    }

    bool TankMustGrabBoss(Player* bot)
    {
        if (!PlayerbotAI::IsTank(bot))
            return false;

        Unit* boss = GetSepethrea(bot);
        if (!boss)
            return false;

        Unit* victim = boss->GetVictim();
        if (!victim)
            return true;
        if (victim == bot)
            return true;
        Player* victimPlayer = victim->ToPlayer();
        return !victimPlayer || !PlayerbotAI::IsTank(victimPlayer);
    }

    bool TankHoldsBoss(Player* bot)
    {
        if (!PlayerbotAI::IsTank(bot))
            return false;

        Unit* boss = GetSepethrea(bot);
        return boss && boss->GetVictim() == bot;
    }

    // [b33_mech] See ARCANE_BLAST_THROW: the throw goes straight away from her.
    float ThrowOvershoot(Unit* boss, float x, float y)
    {
        if (!boss)
            return 0.0f;

        float dx = x - boss->GetPositionX();
        float dy = y - boss->GetPositionY();
        float const len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.5f)
            return 0.0f;

        float const landY = y + dy / len * ARCANE_BLAST_THROW;
        return landY < THROW_SAFE_Y ? THROW_SAFE_Y - landY : 0.0f;
    }

    static void CollectFlames(Player* bot, float radius, std::list<Creature*>& out);

    bool BossNearFlame(Player* bot, float range)
    {
        Unit* boss = GetSepethrea(bot);
        if (!boss)
            return false;

        std::list<Creature*> flames;
        CollectFlames(bot, 100.0f, flames);
        for (Creature* flame : flames)
        {
            if (!flame || !flame->IsAlive() || flame->GetVictim() == bot)
                continue;
            if (boss->GetExactDist2d(flame) < range)
                return true;
        }
        return false;
    }

    bool IsFlame(Unit* unit)
    {
        if (!unit)
            return false;
        uint32 const entry = unit->GetEntry();
        return entry == static_cast<uint32>(MechanarIDs::NPC_RAGING_FLAMES) ||
               entry == static_cast<uint32>(MechanarIDs::NPC_RAGING_FLAMES_HEROIC);
    }

    static void CollectFlames(Player* bot, float radius, std::list<Creature*>& out)
    {
        bot->GetCreatureListWithEntryInGrid(
            out, static_cast<uint32>(MechanarIDs::NPC_RAGING_FLAMES), radius);
        bot->GetCreatureListWithEntryInGrid(
            out, static_cast<uint32>(MechanarIDs::NPC_RAGING_FLAMES_HEROIC), radius);
    }

    void CollectAvoidFlames(Player* bot, std::vector<std::pair<float, float>>& out)
    {
        std::list<Creature*> flames;
        CollectFlames(bot, 100.0f, flames);
        for (Creature* flame : flames)
        {
            if (!flame || !flame->IsAlive())
                continue;
            if (flame->GetVictim() == bot)
                continue;
            out.emplace_back(flame->GetPositionX(), flame->GetPositionY());
        }
    }

    void CollectChasingFlames(Player* bot, std::vector<Creature*>& out)
    {
        std::list<Creature*> flames;
        CollectFlames(bot, 100.0f, flames);
        for (Creature* flame : flames)
            if (flame && flame->IsAlive() && flame->GetVictim() == bot)
                out.push_back(flame);
    }

    Creature* GetNearestFlame(Player* bot, float radius)
    {
        std::list<Creature*> flames;
        CollectFlames(bot, radius, flames);

        Creature* best = nullptr;
        float bestDist = radius;
        for (Creature* flame : flames)
        {
            if (!flame || !flame->IsAlive())
                continue;
            float const d = bot->GetDistance2d(flame);
            if (d <= bestDist)
            {
                bestDist = d;
                best = flame;
            }
        }
        return best;
    }

    ObjectGuid FindFixatingFlameGuid(Player* bot)
    {
        std::list<Creature*> flames;
        CollectFlames(bot, 100.0f, flames);
        for (Creature* flame : flames)
            if (flame && flame->IsAlive() && flame->GetVictim() == bot)
                return flame->GetGUID();
        return ObjectGuid::Empty;
    }

    Unit* GetFixatingFlame(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        ObjectGuid guid =
            botAI->GetAiObjectContext()->GetValue<ObjectGuid>("sepethrea fixating flame")->Get();
        if (guid.IsEmpty())
            return nullptr;

        Unit* flame = ObjectAccessor::GetUnit(*bot, guid);
        if (!flame || !flame->IsAlive() || flame->GetVictim() != bot)
            return nullptr;
        return flame;
    }

    void CollectTrailPatches(Player* bot, float scanRadius, std::vector<TrailPatch>& out)
    {
        std::vector<WorldObject*> objs;
        Acore::AllWorldObjectsInRange check(bot, scanRadius);
        Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(
            bot, objs, check, GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
        Cell::VisitObjects(bot, searcher, scanRadius);

        for (WorldObject* o : objs)
        {
            DynamicObject* dyn = o->ToDynObject();
            if (!dyn || dyn->GetSpellId() != TRAIL_PATCH_SPELL)
                continue;
            float r = dyn->GetRadius();
            if (r <= 0.0f)
                r = TRAIL_PATCH_RADIUS;
            out.push_back({ dyn->GetPositionX(), dyn->GetPositionY(), r });
        }
    }

    bool InTrailDanger(Player* bot)
    {
        std::vector<TrailPatch> patches;
        CollectTrailPatches(bot, TRAIL_SCAN, patches);
        for (TrailPatch const& p : patches)
        {
            float const dx = bot->GetPositionX() - p.x;
            float const dy = bot->GetPositionY() - p.y;
            if (std::sqrt(dx * dx + dy * dy) < p.radius + TRAIL_DANGER_MARGIN)
                return true;
        }
        return false;
    }

    bool InTrailDangerCached(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI->GetAiObjectContext()->GetValue<bool>("sepethrea trail danger")->Get();
    }

    bool IsFlameNearCached(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI->GetAiObjectContext()->GetValue<bool>("sepethrea flame near")->Get();
    }

    bool BossFlameNearCached(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI->GetAiObjectContext()->GetValue<bool>("sepethrea boss flame near")->Get();
    }

    Unit* GetSepethrea(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI->GetAiObjectContext()->GetValue<Unit*>("find target", "nethermancer sepethrea")->Get();
    }
}

namespace MechanarLift
{
    namespace
    {
        Level LevelOf(Transport* lift, float z)
        {
            return z > lift->GetStationaryZ() + RISE * 0.5f ? Level::Top : Level::Bottom;
        }

        // The deck's next stop: the top while it rises or stands there, else the bottom.
        Level NextStop(Transport* lift)
        {
            uint32 const progress = lift->GetPathProgress();
            return progress >= BOTTOM_LEAVES_MS && progress < TOP_LEAVES_MS ? Level::Top : Level::Bottom;
        }

        // Where the bot goes: the harness waypoint near the shaft, else the group leader (or master) near it.
        bool GoalLevel(Player* bot, Transport* lift, float cx, float cy, Level& goal)
        {
            TestRunner::Waypoint waypoint;
            if (TestRunner::GetWaypoint(bot->GetGUID(), waypoint) && waypoint.mapId == bot->GetMapId() &&
                std::hypot(waypoint.x - cx, waypoint.y - cy) <= GOAL_RANGE)
            {
                goal = LevelOf(lift, waypoint.z);
                return true;
            }

            Player* leader = nullptr;
            if (Group* group = bot->GetGroup())
                leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
            if (!leader || leader == bot)
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                    leader = botAI->GetMaster();
            if (!leader || leader == bot || !leader->IsInWorld() || leader->GetMap() != bot->GetMap() ||
                !leader->IsAlive() ||
                std::hypot(leader->GetPositionX() - cx, leader->GetPositionY() - cy) > LEADER_RANGE)
                return false;

            goal = leader->GetTransport() == lift ? NextStop(lift) : LevelOf(lift, leader->GetPositionZ());
            return true;
        }
    }

    Transport* FindLift(Player* bot)
    {
        Map* map = bot->GetMap();
        if (!map || bot->GetMapId() != MAP_ID)
            return nullptr;

        for (Transport* transport : map->GetAllTransports())
            if (transport && transport->IsInWorld() && transport->GetEntry() == GO_LIFT)
                return transport;
        return nullptr;
    }

    void DeckCenter(Transport* lift, float& x, float& y)
    {
        float const o = lift->GetOrientation();
        x = lift->GetPositionX() + std::cos(o) * DECK_LOCAL_X - std::sin(o) * DECK_LOCAL_Y;
        y = lift->GetPositionY() + std::sin(o) * DECK_LOCAL_X + std::cos(o) * DECK_LOCAL_Y;
    }

    float DeckZ(Transport* lift) { return lift->GetPositionZ() + DECK_TOP; }

    bool OnDeck(Player* bot, Transport* lift)
    {
        if (bot->GetTransport() == lift)
            return true;

        float cx = 0.0f, cy = 0.0f;
        DeckCenter(lift, cx, cy);
        return std::fabs(bot->GetPositionX() - cx) <= DECK_HALF && std::fabs(bot->GetPositionY() - cy) <= DECK_HALF &&
               std::fabs(bot->GetPositionZ() - DeckZ(lift)) <= ON_DECK_DZ;
    }

    uint32 StopLeftMs(Transport* lift, Level level)
    {
        uint32 const progress = lift->GetPathProgress();
        if (level == Level::Top)
            return progress >= TOP_ARRIVES_MS && progress < TOP_LEAVES_MS ? TOP_LEAVES_MS - progress : 0;

        if (progress < BOTTOM_LEAVES_MS)
            return BOTTOM_LEAVES_MS - progress;
        if (progress >= BOTTOM_ARRIVES_MS)
        {
            uint32 const period = std::max(progress + 1, static_cast<StaticTransport*>(lift)->GetPeriod());
            return period - progress + BOTTOM_LEAVES_MS;
        }
        return 0;
    }

    void Landing(Level level, float& x, float& y, float& z)
    {
        x = level == Level::Top ? TOP_LANDING_X : BOTTOM_LANDING_X;
        y = level == Level::Top ? TOP_LANDING_Y : BOTTOM_LANDING_Y;
        z = level == Level::Top ? TOP_LANDING_Z : BOTTOM_LANDING_Z;
    }

    bool ActiveLeg(Player* bot, Leg& leg)
    {
        if (bot->GetMapId() != MAP_ID || !bot->IsAlive() || bot->IsBeingTeleported())
            return false;

        Transport* lift = FindLift(bot);
        if (!lift)
            return false;

        float cx = 0.0f, cy = 0.0f;
        DeckCenter(lift, cx, cy);
        float const toShaft = std::hypot(bot->GetPositionX() - cx, bot->GetPositionY() - cy);
        if (toShaft > APPROACH_RANGE)
        {
            // [w24b-mech-landing-walk] Still on the leg while its own walk (the spline) ends at its level's landing.
            if (toShaft > LANDING_WALK_RANGE || bot->GetTransport() || !bot->isMoving() ||
                !bot->movespline->Initialized() || bot->movespline->Finalized())
                return false;
            float landingX = 0.0f, landingY = 0.0f, landingZ = 0.0f;
            Landing(LevelOf(lift, bot->GetPositionZ()), landingX, landingY, landingZ);
            G3D::Vector3 const end = bot->movespline->FinalDestination();
            if (std::hypot(end.x - landingX, end.y - landingY) > LANDING_WALK_END_RANGE)
                return false;
        }

        leg.lift = lift;
        leg.riding = bot->GetTransport() == lift;
        leg.onDeck = OnDeck(bot, lift);
        leg.here = leg.riding ? NextStop(lift) : LevelOf(lift, bot->GetPositionZ());
        bool const hasGoal = GoalLevel(bot, lift, cx, cy, leg.goal);
        if (leg.riding || leg.onDeck)
        {
            // On the deck without a goal: off at its next stop.
            if (!hasGoal)
                leg.goal = NextStop(lift);
            return true;
        }

        return hasGoal && leg.goal != leg.here && !bot->IsInCombat();
    }
}

namespace
{
    constexpr float CAPACITUS_SCAN = 60.0f;
    // The charge pulse (39090, Spell.dbc radius index 13) when the spell is missing from the store.
    constexpr float PULSE_RADIUS_FALLBACK = 10.0f;
    // A member of the other charge this much past the pulse radius is safe (movement and pulse timing slack).
    constexpr float PULSE_MARGIN = 1.0f;
    // The stack of the other charge stands this much beyond the pulse radius from the tank...
    constexpr float STACK_CLEARANCE = 4.0f;
    // ... and at least this far behind the boss (its melee spot is on its far side).
    constexpr float STACK_BEHIND_BOSS = 3.0f;
    // Ranged and healers of the tank's charge stand this far behind the tank, melee beside it.
    constexpr float TANK_STACK_BACK = 6.0f;
    constexpr float TANK_STACK_SIDE = 2.0f;

    float PulseRadius()
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(MechanarCapacitus::SPELL_POSITIVE_CHARGE_PULSE);
        float const radius = info ? info->Effects[EFFECT_0].CalcRadius() : 0.0f;
        return radius > 0.0f ? radius : PULSE_RADIUS_FALLBACK;
    }

    Player* GroupTank(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        Player* any = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!DungeonInteract::IsActiveMember(bot, member) || !PlayerbotAI::IsTank(member))
                continue;

            if (PlayerbotAI::IsMainTank(member))
                return member;
            if (!any)
                any = member;
        }
        return any;
    }

    // The spawns of the bridge pack (smart_scripts -138820 / -138869 / -138879 / -138893).
    constexpr std::array<uint32, 4> BRIDGE_SPAWNS = {138820, 138869, 138879, 138893};
    constexpr std::array<uint32, 4> BRIDGE_ENTRIES = {19168, 20059, 20988, 20990};
    // The upper-floor pack and the Destroyer before the bridge (formation 138819, spawn 138864): the planner clears
    // them first; the bridge pull waits for them.
    constexpr std::array<uint32, 4> BRIDGE_APPROACH_SPAWNS = {138819, 138878, 138892, 138864};
    constexpr std::array<uint32, 4> BRIDGE_APPROACH_ENTRIES = {19168, 20988, 20990, 19735};
    constexpr float BRIDGE_SCAN = 100.0f;
    constexpr float BRIDGE_PULL_RANGE = 70.0f;
    constexpr float BRIDGE_MAX_DZ = 6.0f;
    constexpr float BRIDGE_GATHER = 30.0f;
    constexpr float BRIDGE_READY_HEALTH_PCT = 80.0f;
    constexpr float BRIDGE_READY_HEALER_MANA_PCT = 70.0f;

    bool SpawnAlive(Player* bot, std::array<uint32, 4> const& entries, std::array<uint32, 4> const& spawns,
                    std::vector<Creature*>* out)
    {
        bool any = false;
        for (uint32 const entry : entries)
        {
            std::list<Creature*> found;
            bot->GetCreatureListWithEntryInGrid(found, entry, BRIDGE_SCAN);
            for (Creature* creature : found)
            {
                if (!creature->IsAlive() ||
                    std::find(spawns.begin(), spawns.end(), creature->GetSpawnId()) == spawns.end())
                    continue;

                any = true;
                if (out && std::find(out->begin(), out->end(), creature) == out->end())
                    out->push_back(creature);
            }
        }
        return any;
    }
}

namespace MechanarCapacitus
{
    Unit* GetCapacitus(Player* bot)
    {
        // Asked by the multiplier for every action: the grid search runs at most every CACHE_MS per bot.
        constexpr uint32 CACHE_MS = 500;

        struct Cached
        {
            uint32 ms = 0;
            ObjectGuid guid;
        };

        if (bot->GetMapId() != MechanarLift::MAP_ID)
            return nullptr;

        // Map threads update their bots in parallel: one cache per thread, keyed by the bot.
        thread_local std::unordered_map<uint64, Cached> cache;
        Cached& cached = cache[bot->GetGUID().GetRawValue()];
        uint32 const now = getMSTime();
        if (!cached.ms || getMSTimeDiff(cached.ms, now) >= CACHE_MS)
        {
            cached.ms = now;
            Creature* boss = bot->FindNearestCreature(NPC_CAPACITUS, CAPACITUS_SCAN, true);
            cached.guid = boss && boss->IsInCombat() ? boss->GetGUID() : ObjectGuid::Empty;
        }

        if (cached.guid.IsEmpty())
            return nullptr;

        Creature* boss = ObjectAccessor::GetCreature(*bot, cached.guid);
        return boss && boss->IsAlive() && boss->IsInCombat() ? boss : nullptr;
    }

    int32 Charge(Unit* unit)
    {
        if (!unit)
            return 0;
        if (unit->HasAura(SPELL_POSITIVE_CHARGE))
            return 1;
        if (unit->HasAura(SPELL_NEGATIVE_CHARGE))
            return -1;
        return 0;
    }

    bool PolarityDanger(Player* bot)
    {
        int32 const mine = Charge(bot);
        if (!mine || PlayerbotAI::IsTank(bot) || !GetCapacitus(bot))
            return false;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        float const reach = PulseRadius() + PULSE_MARGIN;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member == bot || !DungeonInteract::IsActiveMember(bot, member))
                continue;

            int32 const theirs = Charge(member);
            if (theirs && theirs != mine && bot->GetExactDist2d(member) < reach)
                return true;
        }
        return false;
    }

    bool OppositeOfTank(Player* bot)
    {
        int32 const mine = Charge(bot);
        if (!mine || PlayerbotAI::IsTank(bot) || !GetCapacitus(bot))
            return false;

        int32 const tankCharge = Charge(GroupTank(bot));
        return tankCharge && tankCharge != mine;
    }

    bool PolaritySpot(Player* bot, float& x, float& y, float& z)
    {
        Unit* boss = GetCapacitus(bot);
        int32 const mine = Charge(bot);
        if (!boss || !mine || PlayerbotAI::IsTank(bot))
            return false;

        float const radius = PulseRadius();
        Player* tank = GroupTank(bot);
        int32 const tankCharge = Charge(tank);
        if (tank && tankCharge)
        {
            // From the boss towards the tank.
            float dx = tank->GetPositionX() - boss->GetPositionX();
            float dy = tank->GetPositionY() - boss->GetPositionY();
            float const len = std::max(0.1f, std::hypot(dx, dy));
            dx /= len;
            dy /= len;
            if (tankCharge == mine)
            {
                bool const melee = PlayerbotAI::IsMelee(bot);
                float const back = melee ? 0.0f : TANK_STACK_BACK;
                float const side = melee ? TANK_STACK_SIDE : 0.0f;
                x = tank->GetPositionX() + dx * back - dy * side;
                y = tank->GetPositionY() + dy * back + dx * side;
            }
            else
            {
                // Beyond the boss on the line from the tank, out of the tank stack's pulse.
                float const fromTank = std::max(radius + STACK_CLEARANCE, len + STACK_BEHIND_BOSS);
                x = tank->GetPositionX() - dx * fromTank;
                y = tank->GetPositionY() - dy * fromTank;
            }
        }
        else
        {
            // No charged tank to stand by: one stack each side of the boss, more than a pulse radius apart.
            float const angle = boss->GetOrientation() + static_cast<float>(M_PI) / 2.0f * static_cast<float>(mine);
            float const off = (radius + STACK_CLEARANCE) / 2.0f;
            x = boss->GetPositionX() + std::cos(angle) * off;
            y = boss->GetPositionY() + std::sin(angle) * off;
        }

        z = bot->GetPositionZ();
        return bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                              bot->GetPositionZ(), x, y, z);
    }

    bool ReflectedSpell(PlayerbotAI* botAI, Action* action, Unit* capacitus)
    {
        CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
        if (!cast || !capacitus || !capacitus->HasAura(SPELL_REFLECTIVE_MAGIC_SHIELD) ||
            DungeonHoldFire::IsSupportAction(action))
            return false;

        if (action->getThreatType() != Action::ActionThreatType::Aoe && action->GetTarget() != capacitus)
            return false;

        uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", cast->getSpell())->Get();
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        return info && info->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && !info->IsPositive();
    }

    bool CastingIntoMagicShield(PlayerbotAI* botAI, Unit* capacitus)
    {
        if (!capacitus || !capacitus->HasAura(SPELL_REFLECTIVE_MAGIC_SHIELD))
            return false;

        Player* bot = botAI->GetBot();
        for (CurrentSpellTypes const slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
        {
            Spell* spell = bot->GetCurrentSpell(slot);
            if (spell && spell->m_spellInfo && spell->m_spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MAGIC &&
                !spell->m_spellInfo->IsPositive() && spell->m_targets.GetUnitTarget() == capacitus)
                return true;
        }
        return false;
    }

    bool MeleeHoldsForDamageShield(Player* bot, Unit* capacitus)
    {
        return capacitus && capacitus->HasAura(SPELL_REFLECTIVE_DAMAGE_SHIELD) && PlayerbotAI::IsMelee(bot) &&
               !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot);
    }
}

namespace MechanarBridge
{
    void CollectBridgePack(Player* bot, std::vector<Creature*>& out)
    {
        if (bot->GetMapId() == MechanarLift::MAP_ID)
            SpawnAlive(bot, BRIDGE_ENTRIES, BRIDGE_SPAWNS, &out);
    }

    bool PathaleonDormant(Player* bot)
    {
        if (bot->GetMapId() != MechanarLift::MAP_ID)
            return false;

        if (SpawnAlive(bot, BRIDGE_ENTRIES, BRIDGE_SPAWNS, nullptr))
            return true;

        Creature* pathaleon = bot->FindNearestCreature(NPC_PATHALEON, BRIDGE_SCAN, true);
        return pathaleon && pathaleon->HasAura(SPELL_GREATER_INVISIBILITY);
    }

    bool IsPullingTank(Player* bot)
    {
        if (!PlayerbotAI::IsTank(bot))
            return false;

        if (PlayerbotAI::IsMainTank(bot))
            return true;

        Group* group = bot->GetGroup();
        if (!group)
            return true;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member != bot && DungeonInteract::IsActiveMember(bot, member) && PlayerbotAI::IsMainTank(member))
                return false;
        }
        return true;
    }

    Creature* BridgePullTarget(PlayerbotAI* botAI)
    {
        Player* bot = botAI->GetBot();
        if (bot->GetMapId() != MechanarLift::MAP_ID || !bot->IsAlive() || bot->IsInCombat() ||
            std::fabs(bot->GetPositionZ() - PACK_Z) > BRIDGE_MAX_DZ ||
            bot->GetExactDist2d(PACK_X, PACK_Y) > BRIDGE_PULL_RANGE || !IsPullingTank(bot))
            return nullptr;

        std::vector<Creature*> pack;
        CollectBridgePack(bot, pack);
        if (pack.empty())
            return nullptr;

        // A fight with part of it under way, or the way to it not cleared yet: the usual fight and planner.
        for (Creature* mob : pack)
            if (mob->IsInCombat())
                return nullptr;

        if (SpawnAlive(bot, BRIDGE_APPROACH_ENTRIES, BRIDGE_APPROACH_SPAWNS, nullptr))
            return nullptr;

        if (!DungeonInteract::IsGroupGathered(bot, bot, BRIDGE_GATHER))
            return nullptr;

        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!DungeonInteract::IsActiveMember(bot, member))
                    continue;

                if (member->GetHealthPct() < BRIDGE_READY_HEALTH_PCT ||
                    (PlayerbotAI::IsHeal(member) && member->getPowerType() == POWER_MANA &&
                     member->GetPowerPct(POWER_MANA) < BRIDGE_READY_HEALER_MANA_PCT))
                    return nullptr;
            }

        Creature* nearest = nullptr;
        for (Creature* mob : pack)
            if (!nearest || bot->GetExactDist(mob) < bot->GetExactDist(nearest))
                nearest = mob;
        return nearest;
    }
}
