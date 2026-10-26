/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MechActions.h"
#include "AiFactory.h"
#include "Creature.h"
#include "Group.h"
#include "MechShared.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "Spell.h"
#include "Timer.h"
#include "Transport.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace
{
    bool Normalize(float& x, float& y)
    {
        float const len = std::sqrt(x * x + y * y);
        if (len < 0.01f)
            return false;
        x /= len;
        y /= len;
        return true;
    }

    float Dist2d(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx, dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    float MinSegDist(float ax, float ay, float bx, float by, float px, float py)
    {
        float const dx = bx - ax, dy = by - ay;
        float const len2 = dx * dx + dy * dy;
        float t = (len2 < 1e-6f) ? 0.0f : ((px - ax) * dx + (py - ay) * dy) / len2;
        t = std::max(0.0f, std::min(1.0f, t));
        return Dist2d(ax + t * dx, ay + t * dy, px, py);
    }

    bool PartyCentroid(Player* bot, float& cx, float& cy, bool rangedOnly)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        float sx = 0.0f, sy = 0.0f;
        int n = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId())
                continue;
            if (bot->GetExactDist2d(member) > 60.0f)
                continue;
            if (rangedOnly && (PlayerbotAI::IsTank(member) || !PlayerbotAI::IsRanged(member)))
                continue;
            sx += member->GetPositionX();
            sy += member->GetPositionY();
            ++n;
        }
        if (n == 0)
            return false;
        cx = sx / n;
        cy = sy / n;
        return true;
    }

    bool TankPosition(Player* bot, float& tx, float& ty)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        float best = 1e18f;
        bool found = false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId())
                continue;
            if (!PlayerbotAI::IsTank(member))
                continue;
            float const d = bot->GetExactDist2d(member);
            if (d < best)
            {
                best = d;
                tx = member->GetPositionX();
                ty = member->GetPositionY();
                found = true;
            }
        }
        return found;
    }

    constexpr float KITE_FAN_OFFSETS[] = {
        0.0f, 0.35f, -0.35f, 0.70f, -0.70f, 1.05f, -1.05f, 1.40f, -1.40f,
        1.75f, -1.75f, 2.10f, -2.10f, 2.45f, -2.45f, 2.80f, -2.80f, 3.14f };

    // [b33_mech] A fixated tank holding Sepethrea that is otherwise safe still steps on while her Arcane Blast would
    // throw it more than this many yards past MechanarFlames::THROW_SAFE_Y (a small slack, so it does not shuffle).
    constexpr float THROW_SETTLE_SLACK = 5.0f;

    bool SpotSafe(std::vector<std::pair<float, float>> const& flames,
                  std::vector<MechanarFlames::TrailPatch> const& patches, float x, float y,
                  float flameClear = MechanarFlames::INFERNO_SAFE_DIST)
    {
        for (auto const& f : flames)
        {
            float const dx = x - f.first, dy = y - f.second;
            if (dx * dx + dy * dy < flameClear * flameClear)
                return false;
        }
        for (MechanarFlames::TrailPatch const& p : patches)
        {
            float const dx = x - p.x, dy = y - p.y;
            float const r = p.radius + MechanarFlames::TRAIL_DANGER_MARGIN;
            if (dx * dx + dy * dy < r * r)
                return false;
        }
        return true;
    }

    int SegSamples(float ax, float ay, float bx, float by)
    {
        return std::max(5, static_cast<int>(Dist2d(ax, ay, bx, by) / 3.0f) + 1);
    }

    bool SegSafe(std::vector<std::pair<float, float>> const& flames,
                 std::vector<MechanarFlames::TrailPatch> const& patches,
                 float ax, float ay, float bx, float by)
    {
        int const n = SegSamples(ax, ay, bx, by);
        for (int k = 1; k <= n; ++k)
        {
            float const t = static_cast<float>(k) / n;
            if (!SpotSafe(flames, patches, ax + (bx - ax) * t, ay + (by - ay) * t))
                return false;
        }
        return true;
    }

    float HazardClearance(std::vector<std::pair<float, float>> const& flames,
                          std::vector<MechanarFlames::TrailPatch> const& patches, float x, float y,
                          float flameClear = MechanarFlames::INFERNO_SAFE_DIST)
    {
        float clear = 1e9f;
        for (auto const& f : flames)
            clear = std::min(clear, Dist2d(x, y, f.first, f.second) - flameClear);
        for (MechanarFlames::TrailPatch const& p : patches)
            clear = std::min(clear, Dist2d(x, y, p.x, p.y) - (p.radius + MechanarFlames::TRAIL_DANGER_MARGIN));
        return clear;
    }

    float SegClearance(std::vector<std::pair<float, float>> const& flames,
                       std::vector<MechanarFlames::TrailPatch> const& patches,
                       float ax, float ay, float bx, float by)
    {
        int const n = SegSamples(ax, ay, bx, by);
        float clear = 1e9f;
        for (int k = 0; k <= n; ++k)
        {
            float const t = static_cast<float>(k) / n;
            clear = std::min(clear, HazardClearance(flames, patches, ax + (bx - ax) * t, ay + (by - ay) * t));
        }
        return clear;
    }
}

bool SepethreaKiteFlameAction::Execute(Event)
{
    std::vector<Creature*> chasers;
    MechanarFlames::CollectChasingFlames(bot, chasers);
    if (chasers.empty())
        return false;

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();

    Creature* flame = chasers.front();
    float dist = bot->GetDistance2d(flame);
    for (size_t i = 1; i < chasers.size(); ++i)
    {
        float const d = bot->GetDistance2d(chasers[i]);
        if (d < dist)
        {
            dist = d;
            flame = chasers[i];
        }
    }

    std::vector<std::pair<float, float>> chaserPos;
    chaserPos.reserve(chasers.size());
    for (Creature* chaser : chasers)
        chaserPos.emplace_back(chaser->GetPositionX(), chaser->GetPositionY());

    float fx = bx - flame->GetPositionX();
    float fy = by - flame->GetPositionY();
    if (!Normalize(fx, fy))
    {
        fx = bx - MechanarFlames::ROOM_ANCHOR_X;
        fy = by - MechanarFlames::ROOM_ANCHOR_Y;
        if (!Normalize(fx, fy))
        {
            fx = 1.0f;
            fy = 0.0f;
        }
    }

    std::vector<std::pair<float, float>> avoidFlames;
    MechanarFlames::CollectAvoidFlames(bot, avoidFlames);
    std::vector<MechanarFlames::TrailPatch> patches;
    MechanarFlames::CollectTrailPatches(bot, 60.0f, patches);

    float tankX = 0.0f, tankY = 0.0f;
    bool const haveTank = TankPosition(bot, tankX, tankY);
    bool const isTank = PlayerbotAI::IsTank(bot);

    // Leash: a healer kites inside healing range of the tank; a fixated tank whose boss is on someone
    // else kites close to her (taunt range, and she walks back onto the tank). A tank holding the boss
    // needs no leash: she follows it, so it drags her along the kite path.
    float leashX = tankX, leashY = tankY;
    float leashSoft = MechanarFlames::HEALER_KITE_LEASH;
    float leashHard = MechanarFlames::HEALER_KITE_LEASH_MAX;
    bool leashed = PlayerbotAI::IsHeal(bot) && haveTank;
    if (isTank)
    {
        Unit* boss = MechanarFlames::GetSepethrea(bot);
        leashed = boss && boss->GetVictim() != bot;
        if (leashed)
        {
            leashX = boss->GetPositionX();
            leashY = boss->GetPositionY();
            leashSoft = MechanarFlames::TANK_KITE_LEASH;
            leashHard = MechanarFlames::TANK_KITE_LEASH_MAX;
        }
    }
    // [b33_mech2] The leash point stays in the fight area: a tank her Arcane Blast threw out of the room (mech-n
    // 20261026-000434-mech-n-5D-17 00:25:04, Rldrual 303.5,-3.5 in the south trash) pulled the healer's leash after
    // it, out of the room and into the trash too. Capped to the nearest room point, the healer kites inside the room
    // as close to the tank as the room allows.
    if (leashed && !MechanarFlames::InRoom(leashX, leashY))
        MechanarFlames::ClampIntoRoom(leashX, leashY);
    float const leashDist = leashed ? bot->GetExactDist2d(leashX, leashY) : 0.0f;

    // [b33_mech] A tank dragging her keeps her Arcane Blast throw out of the south trash (MechShared.h
    // ARCANE_BLAST_THROW): it turns well before the south end and scores every spot by where the throw lands.
    Unit* const heldBoss = isTank && !leashed ? MechanarFlames::GetSepethrea(bot) : nullptr;
    float const southEndY = heldBoss ? MechanarFlames::TANK_TURN_SOUTH_Y : MechanarFlames::ROOM_Y_MIN;

    // The tank keeps its berth from the ranged/healer spot only: the melee stay on the boss, which
    // follows the tank, so fleeing from them would never settle.
    float px = tankX, py = tankY;
    bool const haveParty =
        isTank ? PartyCentroid(bot, px, py, true) : (haveTank || PartyCentroid(bot, px, py, false));
    float const groupDist = haveParty ? bot->GetExactDist2d(px, py) : 1000.0f;

    uint32 const nowMs = getMSTime();
    bool turning = _turnUntilMs != 0 && nowMs < _turnUntilMs;
    if (turning && (leashed || flame->GetGUID() != _turnFlame))
    {
        _turnUntilMs = 0;
        turning = false;
    }
    if (turning && !_turnLateral && dist > MechanarFlames::KITE_THRESHOLD &&
        (by - flame->GetPositionY()) * -_turnEnd > MechanarFlames::KITE_TURN_EXIT_PAST)
    {
        _turnUntilMs = 0;
        turning = false;
    }
    if (!turning && !leashed && std::fabs(fy) >= 0.5f && dist <= MechanarFlames::KITE_TURN_THRESHOLD)
    {
        float const endSign = fy > 0.0f ? 1.0f : -1.0f;
        float const endDist = fy > 0.0f ? MechanarFlames::ROOM_Y_MAX - by : by - southEndY;
        if (endDist < MechanarFlames::KITE_TURN_RUNWAY)
        {
            float bestKey = -1e18f, bestLaneClear = -1e18f;
            for (float side : { 1.0f, -1.0f })
            {
                float const sideX = side > 0.0f
                                        ? MechanarFlames::ROOM_X_MAX - MechanarFlames::KITE_TURN_WALL_INSET
                                        : MechanarFlames::ROOM_X_MIN + MechanarFlames::KITE_TURN_WALL_INSET;
                float laneEndY = flame->GetPositionY() - endSign * MechanarFlames::KITE_TURN_PASS_BEHIND;
                laneEndY = std::min(std::max(laneEndY, southEndY + 3.0f), MechanarFlames::ROOM_Y_MAX - 3.0f);
                float const laneClear =
                    std::min(SegClearance(avoidFlames, patches, bx, by, sideX, by),
                             SegClearance(avoidFlames, patches, sideX, by, sideX, laneEndY));
                float const key = laneClear * MechanarFlames::KITE_TURN_CLEAR_WEIGHT +
                                  std::fabs(sideX - flame->GetPositionX());
                if (key > bestKey)
                {
                    bestKey = key;
                    bestLaneClear = laneClear;
                    _turnSide = side;
                }
            }
            if (bestLaneClear > 0.0f)
            {
                _turnEnd = endSign;
                _turnFlame = flame->GetGUID();
                _turnLateral = true;
                _turnUntilMs = nowMs + MechanarFlames::KITE_TURN_FAILSAFE_MS;
                turning = true;
            }
        }
    }
    if (turning)
    {
        float const sideX = _turnSide > 0.0f
                                ? MechanarFlames::ROOM_X_MAX - MechanarFlames::KITE_TURN_WALL_INSET
                                : MechanarFlames::ROOM_X_MIN + MechanarFlames::KITE_TURN_WALL_INSET;
        if (_turnLateral && std::fabs(bx - sideX) <= 2.0f)
            _turnLateral = false;

        float laneEndY = flame->GetPositionY() - _turnEnd * MechanarFlames::KITE_TURN_PASS_BEHIND;
        laneEndY = std::min(std::max(laneEndY, southEndY + 3.0f), MechanarFlames::ROOM_Y_MAX - 3.0f);

        float laneClear;
        if (_turnLateral)
            laneClear = std::min(SegClearance(avoidFlames, patches, bx, by, sideX, by),
                                 SegClearance(avoidFlames, patches, sideX, by, sideX, laneEndY));
        else
            laneClear = SegClearance(avoidFlames, patches, bx, by, sideX, laneEndY);

        if (laneClear < MechanarFlames::KITE_TURN_ABORT_CLEAR)
        {
            _turnUntilMs = 0;
            turning = false;
        }
        else
            return MoveTo(bot->GetMapId(), sideX, _turnLateral ? by : laneEndY, bot->GetPositionZ(),
                          false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true, false);
    }

    if (dist >= MechanarFlames::KITE_THRESHOLD && groupDist >= MechanarFlames::KITE_GROUP_CLEARANCE &&
        (!leashed || leashDist <= leashSoft) &&
        (!heldBoss || MechanarFlames::ThrowOvershoot(heldBoss, bx, by) <= THROW_SETTLE_SLACK) &&
        SpotSafe(avoidFlames, patches, bx, by))
        return false;

    if (haveParty)
    {
        float ax = bot->GetPositionX() - px;
        float ay = bot->GetPositionY() - py;
        if (Normalize(ax, ay))
        {
            float const clr = MechanarFlames::KITE_GROUP_CLEARANCE;
            float bias = (clr - groupDist) / clr;
            if (bias < 0.0f)
                bias = 0.0f;
            bias *= 1.5f;
            fx += bias * ax;
            fy += bias * ay;
            Normalize(fx, fy);
        }
    }

    float const step = MechanarFlames::KITE_STEP;

    if (!MechanarFlames::InRoom(bx, by))
    {
        float inX = bx, inY = by;
        MechanarFlames::ClampIntoRoom(inX, inY);
        float dx = inX - bx, dy = inY - by;
        if (Normalize(dx, dy))
        {
            float rX = bx + dx * step, rY = by + dy * step;
            MechanarFlames::ClampIntoRoom(rX, rY);
            return MoveTo(bot->GetMapId(), rX, rY, bot->GetPositionZ(), false, false, false, false,
                          MovementPriority::MOVEMENT_COMBAT, true, false);
        }
    }

    float const stepLens[] = { step, 10.0f, 7.0f };
    float const baseAng = std::atan2(fy, fx);
    float bestX = 0.0f, bestY = 0.0f, bestScore = -1e18f;
    bool found = false;
    float safeX = 0.0f, safeY = 0.0f, bestClear = -1e18f;
    bool haveFallback = false;
    for (float off : KITE_FAN_OFFSETS)
    {
        float const ang = baseAng + off;
        for (float len : stepLens)
        {
            float const destX = bx + std::cos(ang) * len;
            float const destY = by + std::sin(ang) * len;
            if (!MechanarFlames::SegmentInRoom(bx, by, destX, destY))
                continue;

            float const clear = HazardClearance(avoidFlames, patches, destX, destY);
            if (clear > bestClear)
            {
                bestClear = clear;
                safeX = destX;
                safeY = destY;
                haveFallback = true;
            }

            if (!SegSafe(avoidFlames, patches, bx, by, destX, destY))
                continue;

            float const destLeashDist = leashed ? Dist2d(destX, destY, leashX, leashY) : 0.0f;
            if (leashed && destLeashDist > leashHard && destLeashDist >= leashDist)
                continue;

            float fd = 1e9f;
            for (auto const& chaser : chaserPos)
                fd = std::min(fd, Dist2d(destX, destY, chaser.first, chaser.second));
            float const longAxis = leashed ? 0.0f : MechanarFlames::KITE_LONG_AXIS_WEIGHT;
            float score = fd + longAxis * std::fabs(std::sin(ang)) + len * 0.1f;
            if (heldBoss)
                score -= MechanarFlames::THROW_WEIGHT * MechanarFlames::ThrowOvershoot(heldBoss, destX, destY);
            if (leashed)
            {
                if (destLeashDist > leashSoft)
                    score -= MechanarFlames::HEALER_KITE_LEASH_WEIGHT * (destLeashDist - leashSoft);
                float passDist = 1e9f;
                for (auto const& chaser : chaserPos)
                    passDist = std::min(passDist, MinSegDist(bx, by, destX, destY,
                                                             chaser.first, chaser.second));
                if (passDist < MechanarFlames::INFERNO_RADIUS)
                    score -= MechanarFlames::HEALER_KITE_FLAME_PATH_WEIGHT *
                             (MechanarFlames::INFERNO_RADIUS - passDist);
            }
            if (score > bestScore)
            {
                bestScore = score;
                bestX = destX;
                bestY = destY;
                found = true;
            }
        }
    }

    if (!found)
    {
        if (!haveFallback)
            return false;
        bestX = safeX;
        bestY = safeY;
    }

    return MoveTo(bot->GetMapId(), bestX, bestY, bot->GetPositionZ(), false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT, true, false);
}

bool SepethreaAvoidFlameAction::Execute(Event)
{
    std::vector<std::pair<float, float>> avoidFlames;
    MechanarFlames::CollectAvoidFlames(bot, avoidFlames);
    std::vector<MechanarFlames::TrailPatch> patches;
    MechanarFlames::CollectTrailPatches(bot, MechanarFlames::TRAIL_SCAN + MechanarFlames::INFERNO_AVOID_CLEAR, patches);

    // A tank holding Sepethrea drags her clear: she stands in front of it with the melee around her,
    // so it keeps a wider berth and also moves when only she is in a flame's Inferno reach.
    Unit* boss = MechanarFlames::TankHoldsBoss(bot) ? MechanarFlames::GetSepethrea(bot) : nullptr;
    float const clearDist =
        MechanarFlames::MELEE_FLAME_CLEAR + (boss ? MechanarFlames::TANK_DRAG_EXTRA : 0.0f);
    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    std::vector<MechanarFlames::TrailPatch> const noPatches;
    bool const bossClear = !boss || SpotSafe(avoidFlames, noPatches, boss->GetPositionX(), boss->GetPositionY(),
                                             MechanarFlames::INFERNO_AVOID_RANGE);
    if (bossClear && SpotSafe(avoidFlames, patches, bx, by, clearDist))
        return false;

    constexpr int DIRS = 16;
    float bestX = bx, bestY = by, bestClear = HazardClearance(avoidFlames, patches, bx, by, clearDist);
    bool found = false;
    // [b33_mech] A tank holding her takes the nearest safe spot whose Arcane Blast throw stays out of the south trash;
    // a safe spot with a bad throw is only the fallback, the one with the shortest overshoot.
    bool haveThrown = false;
    float thrownX = 0.0f, thrownY = 0.0f, thrownOver = 1e9f;
    for (float radius = 4.0f; radius <= MechanarFlames::INFERNO_AVOID_CLEAR + 8.0f && !found; radius += 3.0f)
    {
        for (int i = 0; i < DIRS; ++i)
        {
            float const ang = (i * 2.0f * static_cast<float>(M_PI)) / DIRS;
            float const x = bx + std::cos(ang) * radius;
            float const y = by + std::sin(ang) * radius;
            if (!MechanarFlames::InRoom(x, y))
                continue;
            if (SpotSafe(avoidFlames, patches, x, y, clearDist))
            {
                float const over = MechanarFlames::ThrowOvershoot(boss, x, y);
                if (over > THROW_SETTLE_SLACK)
                {
                    if (over < thrownOver)
                    {
                        thrownOver = over;
                        thrownX = x;
                        thrownY = y;
                        haveThrown = true;
                    }
                    continue;
                }
                bestX = x;
                bestY = y;
                found = true;
                break;
            }
            float const clear = HazardClearance(avoidFlames, patches, x, y, clearDist);
            if (clear > bestClear)
            {
                bestClear = clear;
                bestX = x;
                bestY = y;
            }
        }
    }
    if (!found && haveThrown)
    {
        bestX = thrownX;
        bestY = thrownY;
    }

    if (bestX == bx && bestY == by)
        return false;

    MovementPriority const priority =
        PlayerbotAI::IsHeal(bot) ? MovementPriority::MOVEMENT_COMBAT : MovementPriority::MOVEMENT_FORCED;
    return MoveTo(bot->GetMapId(), bestX, bestY, bot->GetPositionZ(), false, false, false, false,
                  priority, true, false);
}

bool SepethreaAvoidTrailAction::Execute(Event)
{
    std::vector<MechanarFlames::TrailPatch> patches;
    MechanarFlames::CollectTrailPatches(bot, MechanarFlames::TRAIL_SCAN, patches);
    if (patches.empty())
        return false;

    float ax = 0.0f, ay = 0.0f;
    bool anyDanger = false;
    for (MechanarFlames::TrailPatch const& p : patches)
    {
        float const dx = bot->GetPositionX() - p.x;
        float const dy = bot->GetPositionY() - p.y;
        float const d = std::sqrt(dx * dx + dy * dy);
        float const danger = p.radius + MechanarFlames::TRAIL_DANGER_MARGIN;
        if (d >= danger)
            continue;
        anyDanger = true;
        if (d < 0.1f)
            continue;
        float const w = (danger - d) / danger;
        ax += dx / d * w;
        ay += dy / d * w;
    }

    if (!anyDanger)
        return false;

    if (!Normalize(ax, ay))
    {
        if (Unit* flame = MechanarFlames::GetNearestFlame(bot, 100.0f))
        {
            ax = bot->GetPositionX() - flame->GetPositionX();
            ay = bot->GetPositionY() - flame->GetPositionY();
        }
        if (!Normalize(ax, ay))
        {
            ax = bot->GetPositionX() - MechanarFlames::ROOM_ANCHOR_X;
            ay = bot->GetPositionY() - MechanarFlames::ROOM_ANCHOR_Y;
            if (!Normalize(ax, ay))
            {
                ax = 1.0f;
                ay = 0.0f;
            }
        }
    }

    // Step out along the push direction, but fan around it: straight out of a patch is often into a
    // wall or a corner (the old clamp put the bot right back where it stood, in the fire), or into the
    // flame that is laying the trail. Keep the flames' melee berth and prefer the push direction.
    std::vector<std::pair<float, float>> flames;
    MechanarFlames::CollectAvoidFlames(bot, flames);
    std::vector<Creature*> chasers;
    MechanarFlames::CollectChasingFlames(bot, chasers);
    for (Creature* chaser : chasers)
        flames.emplace_back(chaser->GetPositionX(), chaser->GetPositionY());

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    float const baseAng = std::atan2(ay, ax);
    // [b33_mech] A tank holding her also weighs where her Arcane Blast would throw it (MechShared.h
    // ARCANE_BLAST_THROW), capped below the clearance term: out of the fire first, the throw breaks the ties.
    Unit* const heldBoss = MechanarFlames::TankHoldsBoss(bot) ? MechanarFlames::GetSepethrea(bot) : nullptr;
    float const stepLens[] = { MechanarFlames::TRAIL_CLEAR_STEP, 7.0f, 13.0f };
    float destX = bx + ax * MechanarFlames::TRAIL_CLEAR_STEP;
    float destY = by + ay * MechanarFlames::TRAIL_CLEAR_STEP;
    float bestKey = -1e18f;
    bool found = false;
    for (float off : KITE_FAN_OFFSETS)
    {
        float const ang = baseAng + off;
        for (float len : stepLens)
        {
            float const x = bx + std::cos(ang) * len;
            float const y = by + std::sin(ang) * len;
            if (!MechanarFlames::SegmentInRoom(bx, by, x, y))
                continue;
            float const clear = HazardClearance(flames, patches, x, y, MechanarFlames::MELEE_FLAME_CLEAR);
            float const key = std::min(clear, 4.0f) * 4.0f + std::cos(off) * 3.0f - len * 0.05f -
                              MechanarFlames::THROW_WEIGHT *
                                  std::min(MechanarFlames::ThrowOvershoot(heldBoss, x, y), 4.0f);
            if (key > bestKey)
            {
                bestKey = key;
                destX = x;
                destY = y;
                found = true;
            }
        }
    }
    if (!found && !MechanarFlames::SegmentInRoom(bx, by, destX, destY))
        MechanarFlames::ClampIntoRoom(destX, destY);

    MovementPriority const priority =
        PlayerbotAI::IsHeal(bot) ? MovementPriority::MOVEMENT_COMBAT : MovementPriority::MOVEMENT_FORCED;

    return MoveTo(bot->GetMapId(), destX, destY, bot->GetPositionZ(), false, false, false, false,
                  priority, true, false);
}

bool SepethreaFocusBossAction::Execute(Event)
{
    Unit* boss = MechanarFlames::GetSepethrea(bot);
    if (!boss)
        return false;

    if (AI_VALUE(Unit*, "current target") != boss)
        return Attack(boss);

    return false;
}

namespace
{
// The walk over the deck's rim (MechanarRideLiftAction): its middle point lies this far inside the deck's edge.
constexpr float DECK_RIM_INSET = 1.0f;
// The deck's floor at a point: searched from this high above the deck or the bot, this far down; only a floor this
// close to the deck's own height counts (else the bot's height on the landing: the deck stands flush with it).
constexpr float DECK_FLOOR_SEARCH_UP = 3.0f;
constexpr float DECK_FLOOR_SEARCH = 8.0f;
constexpr float DECK_FLOOR_MAX_DZ = 4.0f;
// Off the deck only while it stands long enough for the walk to the landing (up to 15 yd); else at the next stop.
constexpr uint32 STEP_OFF_MIN_LEFT_MS = 2300;

float DeckFloor(Player* bot, Transport* lift, float x, float y)
{
    float const deckZ = MechanarLift::DeckZ(lift);
    float const from = std::max(deckZ, bot->GetPositionZ()) + DECK_FLOOR_SEARCH_UP;
    float const floor = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, from, true, DECK_FLOOR_SEARCH);
    if (floor > INVALID_HEIGHT && std::fabs(floor - deckZ) <= DECK_FLOOR_MAX_DZ)
        return floor;
    return bot->GetPositionZ();
}

// The point of the deck's square, inset by DECK_RIM_INSET, nearest to (x, y).
G3D::Vector3 DeckRimPoint(Transport* lift, float x, float y, float z)
{
    float cx = 0.0f, cy = 0.0f;
    MechanarLift::DeckCenter(lift, cx, cy);
    float const half = MechanarLift::DECK_HALF - DECK_RIM_INSET;
    return G3D::Vector3(std::clamp(x, cx - half, cx + half), std::clamp(y, cy - half, cy + half), z);
}
}  // namespace

bool MechanarRideLiftAction::WalkOverRim(G3D::Vector3 const& rim, G3D::Vector3 const& end)
{
    UpdateMovementState();
    if (!IsMovingAllowed())
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // A path of three points is followed as given (SwimStraight): MovePoint pathfinds even without generatePath,
    // and that path ended on the landing's rim, short of the deck (mech-n/h 2026-10-25: all five at 247,50.8).
    G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    Movement::PointsArray path = {start, rim, end};
    MotionMaster* mm = bot->GetMotionMaster();
    mm->Clear();
    mm->MoveSplinePath(&path);

    float const distance = (rim - start).length() + (end - rim).length();
    float const delay = std::min(static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove),
                                 static_cast<float>(IN_MILLISECONDS) * MoveDelay(distance));
    AI_VALUE(LastMovement&, "last movement")
        .Set(MechanarLift::MAP_ID, end.x, end.y, end.z, bot->GetOrientation(), delay,
             MovementPriority::MOVEMENT_FORCED);
    return true;
}

bool MechanarRideLiftAction::Execute(Event /*event*/)
{
    MechanarLift::Leg leg;
    if (!MechanarLift::ActiveLeg(bot, leg))
        return false;

    Transport* lift = leg.lift;
    float landingX = 0.0f, landingY = 0.0f, landingZ = 0.0f;
    bool const ownWalk =
        bot->isMoving() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE;

    if (leg.riding || leg.onDeck)
    {
        // The walk onto the deck or off it is under way: it goes on.
        if (ownWalk)
            return true;

        // The deck stands at the goal long enough for the walk: off it, onto the landing. Off the transport first,
        // so the walk is in world coordinates and ends on the landing, not carried on with the deck.
        if (MechanarLift::StopLeftMs(lift, leg.goal) >= STEP_OFF_MIN_LEFT_MS)
        {
            MechanarLift::Landing(leg.goal, landingX, landingY, landingZ);
            float const landingFloor = bot->GetMapHeight(landingX, landingY, landingZ + DECK_FLOOR_SEARCH_UP);
            if (landingFloor > INVALID_HEIGHT && std::fabs(landingFloor - landingZ) <= DECK_FLOOR_MAX_DZ)
                landingZ = landingFloor;
            if (leg.riding)
                lift->RemovePassenger(bot, true);
            G3D::Vector3 const rim = DeckRimPoint(lift, landingX, landingY, bot->GetPositionZ());
            if (WalkOverRim(rim, G3D::Vector3(landingX, landingY, landingZ)))
                return true;
            // No walk now: aboard again until the next stop.
            lift->AddPassenger(bot, true);
            return true;
        }

        // Aboard: a passenger, standing still until the goal. Also again after PlayerbotAI's own transport check
        // let go of the bot - it looks for a static transport within 75 yd of the object's position, which for
        // this lift is its model origin by the instance entrance, 260 yd from the shaft.
        if (!leg.riding)
        {
            lift->AddPassenger(bot, true);
            if (bot->isMoving())
                bot->StopMovingOnCurrentPos();
            return true;
        }
        if (bot->isMoving())
        {
            bot->StopMovingOnCurrentPos();
            return true;
        }
        // Riding: nothing to do here; what does not move the bot (a buff, a heal) may run meanwhile, every move is
        // held (MechanarLiftHoldMultiplier).
        return false;
    }

    // At the landing of its level: onto the deck while it stands there long enough, else wait for it.
    MechanarLift::Landing(leg.here, landingX, landingY, landingZ);
    float const toLanding = bot->GetExactDist2d(landingX, landingY);
    if (toLanding <= MechanarLift::BOARD_FROM_RANGE &&
        MechanarLift::StopLeftMs(lift, leg.here) >= MechanarLift::BOARD_MIN_LEFT_MS)
    {
        // Spread over the deck by the bot's place in the group, nobody in the middle of another.
        static float const SLOT_OFFSETS[][2] = {{0.0f, 0.0f}, {-2.5f, -2.5f}, {-2.5f, 2.5f}, {2.5f, -2.5f},
                                                {2.5f, 2.5f}};
        uint32 slot = 0;
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (ref->GetSource() == bot)
                    break;
                ++slot;
            }
        float deckX = 0.0f, deckY = 0.0f;
        MechanarLift::DeckCenter(lift, deckX, deckY);
        deckX += SLOT_OFFSETS[slot % 5][0];
        deckY += SLOT_OFFSETS[slot % 5][1];
        float const deckFloor = DeckFloor(bot, lift, deckX, deckY);
        G3D::Vector3 const rim = DeckRimPoint(lift, bot->GetPositionX(), bot->GetPositionY(), deckFloor);
        // A passenger before the step, as a client boards: the walk runs in the deck's coordinates, and no guard
        // of a walk over open air cuts it at the rim (PlayerbotAI::GuardAirborneSpline skips transport splines).
        if (bot->GetTransport() != lift)
            lift->AddPassenger(bot, true);
        if (WalkOverRim(rim, G3D::Vector3(deckX, deckY, deckFloor)))
            return true;
        lift->RemovePassenger(bot, true);
    }

    // On the way to the landing.
    if (bot->isMoving())
    {
        float shaftX = 0.0f, shaftY = 0.0f;
        MechanarLift::DeckCenter(lift, shaftX, shaftY);
        float const toShaft = bot->GetExactDist2d(shaftX, shaftY);
        uint32 const now = getMSTime();
        if (toShaft > MechanarLift::APPROACH_RANGE &&
            (!_landingWalkLogMs || getMSTimeDiff(_landingWalkLogMs, now) >= 10 * IN_MILLISECONDS))
        {
            _landingWalkLogMs = now ? now : 1;
            LOG_INFO("playerbots",
                     "[w24b-mech-landing-walk] {}: walks on to the {} landing past the approach range, {:.1f} yd from "
                     "the shaft, {:.1f} yd to go",
                     bot->GetName(), leg.here == MechanarLift::Level::Top ? "top" : "bottom", toShaft, toLanding);
        }
        return true;
    }
    if (toLanding > MechanarLift::LANDING_WAIT_RANGE)
        return MoveTo(MechanarLift::MAP_ID, landingX, landingY, landingZ, false, false, false, false,
                      MovementPriority::MOVEMENT_NORMAL);

    // Waiting for the deck: nothing else walks the bot away meanwhile (MechanarLiftHoldMultiplier).
    return false;
}

bool CapacitusPolarityAction::Execute(Event /*event*/)
{
    constexpr uint32 logMs = 5 * IN_MILLISECONDS;
    constexpr float arrived = 1.0f;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!MechanarCapacitus::PolaritySpot(bot, x, y, z) || bot->GetExactDist2d(x, y) < arrived)
        return false;

    uint32 const now = getMSTime();
    if (!_logMs || getMSTimeDiff(_logMs, now) >= logMs)
    {
        _logMs = now;
        LOG_INFO("playerbots",
                 "[capacitus polarity] {} ({} charge, {} the tank's) moves to ({:.1f}, {:.1f}), {:.1f} yd", bot->GetName(), MechanarCapacitus::Charge(bot) > 0 ? "positive" : "negative",
                 MechanarCapacitus::OppositeOfTank(bot) ? "not" : "with", x, y, bot->GetExactDist2d(x, y));
    }

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT, true,
                  false);
}

bool CapacitusHoldFireAction::Execute(Event /*event*/)
{
    Unit* capacitus = MechanarCapacitus::GetCapacitus(bot);
    if (!capacitus)
        return false;

    bool stopped = false;
    char const* shield = "";
    if (!PlayerbotAI::IsHeal(bot) && MechanarCapacitus::CastingIntoMagicShield(botAI, capacitus))
    {
        for (CurrentSpellTypes const slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
        {
            Spell* spell = bot->GetCurrentSpell(slot);
            if (spell && spell->m_targets.GetUnitTarget() == capacitus)
            {
                bot->InterruptSpell(slot);
                stopped = true;
            }
        }
        shield = "Reflective Magic Shield: cast stopped";
    }

    if (MechanarCapacitus::MeleeHoldsForDamageShield(bot, capacitus) && bot->GetVictim() == capacitus)
    {
        bot->AttackStop();
        stopped = true;
        shield = "Reflective Damage Shield: melee stopped";
    }

    if (stopped)
        LOG_INFO("playerbots", "[capacitus shield] {}: {}", bot->GetName(), shield);
    return stopped;
}

bool MechanarBridgePullAction::Execute(Event /*event*/)
{
    Creature* target = MechanarBridge::BridgePullTarget(botAI);
    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!target || !strategy)
        return false;

    std::vector<Creature*> pack;
    MechanarBridge::CollectBridgePack(bot, pack);
    LOG_INFO("playerbots",
             "[mech bridge] {} pulls {} at {:.1f} yd: the bridge pack ({} alive) must die before Pathaleon appears "
             "(instance DATA_BRIDGE_MOB_DEATH_COUNT 4, he is invisible and unattackable until then)",
             bot->GetName(), target->GetName(), bot->GetExactDist(target), pack.size());

    strategy->RequestPull(target, true, false);
    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>("current target")->Set(target);
    botAI->ChangeEngine(BOT_STATE_COMBAT);
    return true;
}
