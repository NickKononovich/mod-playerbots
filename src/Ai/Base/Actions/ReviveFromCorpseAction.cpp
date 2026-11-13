/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WaypointMgr.h"  // [rise33]
#include "GuildCoordinator.h"  // [ghost40]
#include "GuildHonest.h"  // [grave136]
#include "ReviveFromCorpseAction.h"
#include "BotPath.h"  // [lead109]
#include "SelfTravelActions.h"  // [ghost58] the long route
#include "Corpse.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Creature.h"
#include "Event.h"
#include "FleeManager.h"
#include "GameGraveyard.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "NewRpgBaseAction.h"  // [death425] DangerDiscs
#include "PathGenerator.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "SoloSafety.h"  // [deaths3] alone
#include "SurvivalKit.h"  // [death460] the heals on the move after the healer
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"  // [guild13] CONFIG_DEATH_SICKNESS_LEVEL

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Top of the default start-zone brackets (Playerbots.ZoneBracket.*).
constexpr uint32 START_ZONE_GRAVEYARD_MAX_LEVEL = 12;

namespace
{
    // A spot is safe to rise on when no living hostile comes at once: none fighting or evading within
    // REVIVE_FIGHT_RADIUS, none at rest within its aggro range + REVIVE_AGGRO_MARGIN (the shaman's Reincarnation rule).
    // Stratholme wave 18: the shaman rose on its corpse among the Ravaged Cadavers that had wiped the group, 151 yd
    // from the group, and died again within 2 s.
    constexpr float REVIVE_FIGHT_RADIUS = 40.0f;
    constexpr float REVIVE_AGGRO_MARGIN = 5.0f;
    // The safe spot within the corpse's reclaim radius: rings of these radii, these directions, the floor within
    // SPOT_SNAP_RANGE below SPOT_SNAP_UP above the corpse, a complete walk for the best SPOT_PATH_TRIES of them.
    constexpr std::array<float, 3> SPOT_RINGS = {12.0f, 22.0f, 32.0f};
    constexpr uint32 SPOT_DIRECTIONS = 12;
    constexpr float SPOT_SNAP_UP = 5.0f;
    constexpr float SPOT_SNAP_RANGE = 15.0f;
    constexpr float SPOT_MAX_DZ = 8.0f;
    constexpr uint32 SPOT_PATH_TRIES = 4;
    constexpr uint32 SPOT_RECHECK_MS = 5 * IN_MILLISECONDS;

    // [deaths3] which reaches a rise spot's margin counts: Full - every hostile where it stands and an idle wanderer's
    // whole wander area (nothing comes there); Now - every hostile where it stands now; Fixed - only the mobs that
    // stay (fighting / evading, or idle without a wander area or a patrol): a spot clear of those is a window that
    // opens once the wanderers and patrols are out of its reach
    enum class Reach : uint8
    {
        Full,
        Now,
        Fixed,
        Paths  // [rise33] only the movers: a wanderer's area, a patrol's path, each with its aggro reach
    };

    // The smallest margin (yd) the point keeps from the reach of the living hostiles around; negative: one would come.
    float SpotMargin(PlayerbotAI* botAI, Player* bot, GuidVector const& hostiles, float x, float y, float z,
                     Reach mode = Reach::Full)
    {
        float margin = FLT_MAX;
        for (ObjectGuid const& guid : hostiles)
        {
            Unit* unit = botAI->GetUnit(guid);
            Creature* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->IsTrigger() ||
                creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ||
                creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2))
                continue;

            float reach = 0.0f;
            bool const fighting = creature->IsInEvadeMode() || creature->IsInCombat();
            if (fighting)
                reach = REVIVE_FIGHT_RADIUS;
            else if (creature->GetReactState() == REACT_PASSIVE)
                continue;
            else
                reach = creature->GetAggroRange(bot) + REVIVE_AGGRO_MARGIN;
            MovementGeneratorType const moves = creature->GetDefaultMovementType();
            if (mode == Reach::Fixed && !fighting && (moves == RANDOM_MOTION_TYPE || moves == WAYPOINT_MOTION_TYPE))
                continue;
            // [rise33] the window's movers: a wanderer anywhere in its wander area, a patrol anywhere on its path,
            // reaches the spot while the bot rises and rests there (b385 Aestymro L13 rose "safe" at Longshore
            // between the murloc wanderers' rounds - Fixed and Now left them out - and died again within the rest)
            if (mode == Reach::Paths)
            {
                if (fighting)
                    continue;
                if (moves == RANDOM_MOTION_TYPE)
                    margin = std::min(margin, creature->GetHomePosition().GetExactDist(x, y, z) - reach -
                                                  creature->GetWanderDistance());
                else if (moves == WAYPOINT_MOTION_TYPE)
                {
                    if (WaypointPath const* path = sWaypointMgr->GetPath(creature->GetWaypointPath()))
                        for (WaypointNode const& node : path->Nodes)
                            margin = std::min(margin, std::hypot(node.X - x, node.Y - y) - reach);
                    else
                        margin = std::min(margin, creature->GetExactDist(x, y, z) - reach);
                }
                continue;
            }
            margin = std::min(margin, creature->GetExactDist(x, y, z) - reach);
            if (mode != Reach::Full)
                continue;
            // [deaths3] an idle wanderer reaches the spot from anywhere in its wander area (b338 Keenonk L11 rose twice
            // at the Wetlands South Road healer, 26 yd from a Black Slime L20's home, and died within seconds)
            if (!creature->IsInCombat() && !creature->IsInEvadeMode() &&
                creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                margin = std::min(margin, creature->GetHomePosition().GetExactDist(x, y, z) - reach -
                                              creature->GetWanderDistance());
            // [death448] ... and an idle patrol from anywhere on its path (b467 Rollothan L16 rose "safe" at its
            // corpse in Galardell Valley, 21 yd off a Rabid Shadowhide Gnoll's patrol, and died to it in its rest)
            if (!fighting && moves == WAYPOINT_MOTION_TYPE)
                if (WaypointPath const* path = sWaypointMgr->GetPath(creature->GetWaypointPath()))
                    for (WaypointNode const& node : path->Nodes)
                        margin = std::min(margin, std::hypot(node.X - x, node.Y - y) - reach);
        }
        return margin;
    }

    void LogGhost(Player* bot, std::string const& what);

    // [deaths3] the graveyards whose spirit healer stood in reach of a hostile, for this death (its corpse's ghost
    // time), and the healer chosen instead: the closest graveyard of the bot, else the nearest one on its map that
    // serves its team at its own spot (Graveyard::GetClosestGraveyard there gives it back), not found unsafe
    struct GhostDangers
    {
        int64 ghostTime = -1;
        std::set<uint32> unsafe;
        bool chosen = false;  // `grave` is computed for this `unsafe`
        uint32 grave = 0;     // 0: no healer left
    };
    std::mutex ghostLock;
    std::unordered_map<uint32, GhostDangers> ghostDangers;

    int64 GhostTimeOf(Player* bot)
    {
        Corpse* corpse = bot->GetCorpse();
        return corpse ? int64(corpse->GetGhostTime()) : 0;
    }

    // caller holds ghostLock
    GhostDangers& DangersOf(Player* bot)
    {
        GhostDangers& dangers = ghostDangers[bot->GetGUID().GetCounter()];
        int64 const ghostTime = GhostTimeOf(bot);
        if (dangers.ghostTime != ghostTime)
        {
            dangers = GhostDangers();
            dangers.ghostTime = ghostTime;
        }
        return dangers;
    }

    // [grave44] a graveyard the bot rose at and died again near (its next death before it leaves the healer's yard)
    // is unsafe for it at that level, not for one death only: bot -> graveyard -> the level it holds for
    std::unordered_map<uint32, std::unordered_map<uint32, uint8>> graveDeaths;
    // bot -> (graveyard it rose at, level, corpse position of the next death: its own reach)
    std::unordered_map<uint32, std::pair<uint32, uint8>> lastHealerRise;

    bool GraveUnsafe(Player* bot, uint32 graveId)
    {
        std::lock_guard<std::mutex> guard(ghostLock);
        if (auto const it = graveDeaths.find(bot->GetGUID().GetCounter()); it != graveDeaths.end())
            if (auto const g = it->second.find(graveId); g != it->second.end() && g->second == bot->GetLevel())
                return true;
        return DangersOf(bot).unsafe.count(graveId) != 0;
    }

    void MarkGraveUnsafe(Player* bot, uint32 graveId)
    {
        std::lock_guard<std::mutex> guard(ghostLock);
        GhostDangers& dangers = DangersOf(bot);
        dangers.unsafe.insert(graveId);
        dangers.chosen = false;
    }

    GraveyardStruct const* SafeGrave(Player* bot)
    {
        std::set<uint32> unsafe;
        {
            std::lock_guard<std::mutex> guard(ghostLock);
            GhostDangers& dangers = DangersOf(bot);
            if (dangers.chosen)
                return dangers.grave ? sGraveyard->GetGraveyard(dangers.grave) : nullptr;
            unsafe = dangers.unsafe;
        }
        GraveyardStruct const* grave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
        if (grave && unsafe.count(grave->ID))
        {
            grave = nullptr;
            float best = FLT_MAX;
            for (auto const& [id, candidate] : sGraveyard->GetGraveyardData())
            {
                if (candidate.Map != bot->GetMapId() || unsafe.count(id))
                    continue;
                float const dist = bot->GetExactDist(candidate.x, candidate.y, candidate.z);
                if (dist >= best)
                    continue;
                uint32 zoneId = 0;
                uint32 areaId = 0;
                sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, candidate.Map, candidate.x, candidate.y,
                                          candidate.z);
                GraveyardStruct const* serves =
                    sGraveyard->GetClosestGraveyard(candidate.Map, candidate.x, candidate.y, candidate.z,
                                                    bot->GetTeamId(), areaId, zoneId,
                                                    bot->getClass() == CLASS_DEATH_KNIGHT);
                if (!serves || serves->ID != id)
                    continue;
                best = dist;
                grave = &candidate;
            }
        }
        std::lock_guard<std::mutex> guard(ghostLock);
        GhostDangers& dangers = DangersOf(bot);
        dangers.chosen = true;
        dangers.grave = grave ? grave->ID : 0;
        return grave;
    }

    bool HealerLeft(Player* bot) { return SafeGrave(bot) != nullptr; }

    // [ghost14] the spirit healer's price: Resurrection Sickness from CONFIG_DEATH_SICKNESS_LEVEL on (Player::
    // ResurrectPlayer: a quarter of its stats for (level - start + 1) minutes, ten at most). User 2026-11-08: the bot
    // avoids it - it rises at its corpse whenever the corpse is reachable and a spot of its radius is safe (clear of
    // every reach now, or the window: no mob that stays reaches it and the wanderers / patrols are out of reach), the
    // healer only when neither holds. b354 Laaral L14 / Rumanatol L13 took the healer and died under the sickness
    bool HealerSickens(Player* bot)
    {
        return int32(bot->GetLevel()) >= sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL);
    }

    uint32 SicknessMinutes(Player* bot)
    {
        int32 const start = sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL);
        int32 const level = int32(bot->GetLevel());
        return level < start ? 0 : uint32(std::min(10, level - start + 1));
    }

    // [death460] Resurrection Sickness (spell 15007): every attribute -75 %, so the health / mana its stamina /
    // intellect give drop by that much; the spirit healer revives at half health and mana (Player::ResurrectPlayer
    // 0.5), held under the sick maxima
    constexpr float SICKNESS_STAT_SHARE = 0.75f;
    constexpr float HEALER_RESTORE = 0.5f;

    // [death460] coordinator 2026-11-13: at its last way back (the healer) with a mob that stays reaching the yard, the
    // bot rises only when the walk out of their reaches is survivable at the health, mana and run speed it rises with:
    // each such mob chases it to the leash (SoloSafety::RunPastChase at the base run speed, no speed piece: a ghost's
    // own speed is its ghost speed), the chases added, judged at their high end against its health after the healer
    // and its heals on the move from the mana it rises with. `why`: the price, for the log line
    bool HealerRiseSurvives(PlayerbotAI* botAI, Player* bot, GuidVector const& hostiles, float x, float y, float z,
                            std::string& why)
    {
        SoloSafety::RunPast total;
        uint32 chasers = 0;
        for (ObjectGuid const& guid : hostiles)
        {
            Unit* unit = botAI->GetUnit(guid);
            Creature* mob = unit ? unit->ToCreature() : nullptr;
            if (!mob)
                continue;
            GuidVector const one{guid};
            if (SpotMargin(botAI, bot, one, x, y, z, Reach::Fixed) >= 0.0f)
                continue;
            total.Add(SoloSafety::RunPastChase(bot, mob, false, playerBaseMoveSpeed[MOVE_RUN]));
            ++chasers;
        }
        float maxHealth = float(bot->GetMaxHealth());
        float maxMana = float(bot->GetMaxPower(POWER_MANA));
        if (HealerSickens(bot))
        {
            maxHealth -= SICKNESS_STAT_SHARE * bot->GetHealthBonusFromStamina();
            maxMana -= SICKNESS_STAT_SHARE * bot->GetManaBonusFromIntellect();
        }
        float const health = std::min(HEALER_RESTORE * float(bot->GetMaxHealth()), std::max(maxHealth, 1.0f));
        float const mana = std::min(HEALER_RESTORE * float(bot->GetMaxPower(POWER_MANA)), std::max(maxMana, 0.0f));
        float const damage = total.High();
        float const heals = damage == std::numeric_limits<float>::max()
                                ? 0.0f
                                : SurvivalKit::HealPoints(bot, total.seconds, mana, true, true);
        bool const survives = damage < health + heals;
        why = damage == std::numeric_limits<float>::max()
                  ? Acore::StringFormat("{} mob(s) that stay chase it without a leash", chasers)
                  : Acore::StringFormat("{} mob(s) that stay chase it {:.0f} s for {:.0f} dmg (high end) against hp "
                                        "{:.0f} + {:.0f} heals after the healer",
                                        chasers, total.seconds, damage, health, heals);
        return survives;
    }

    // [death425] the margin (yd) the point keeps from the zones where the bot died (NewRpgBaseAction::DangerDiscs): the
    // route search refuses them, and a walk that starts inside one judges the mobs there one by one ([pull9]
    // SameDangerZone). b460 Galiver L18 rose 3 times 8-12 yd from its last death spot at Longshore ("the spot is
    // safe" by the live murlocs' reaches), each walk out started "inside 2-3 refused reaches", went on through the
    // camp and met the murlocs again: 4 deaths. Counted while a spirit healer is left (else the ghost could wait for
    // good) and not in an instance (the corpse is the only way back there)
    float ZoneMargin(Player* bot, float x, float y)
    {
        if (bot->GetMap()->Instanceable() || !HealerLeft(bot))
            return FLT_MAX;
        std::vector<G3D::Vector4> zones;
        NewRpgBaseAction::DangerDiscs(bot, x, y, 0.0f, zones, false);
        float margin = FLT_MAX;
        for (G3D::Vector4 const& zone : zones)
            margin = std::min(margin, std::hypot(zone.x - x, zone.y - y) - zone.w);
        return margin;
    }

    // [deaths3] it rises here: nothing reaches the spot (Full), or - no healer left for this death - the window is
    // open: no mob that stays reaches it and the wanderers / patrols are out of its reach now
    bool RisesHere(PlayerbotAI* botAI, Player* bot, GuidVector const& hostiles, bool& window)
    {
        float const x = bot->GetPositionX();
        float const y = bot->GetPositionY();
        float const z = bot->GetPositionZ();
        window = false;
        if (float const zone = ZoneMargin(bot, x, y); zone < 0.0f)  // [death425]
        {
            LogGhost(bot, Acore::StringFormat("[death425] no rise here: the zone where it died covers the spot ({:.0f} "
                                              "yd inside)",
                                              -zone));
            return false;
        }
        if (SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Full) >= 0.0f)
            return true;
        // [ghost14] the healer's sickness: the window first. [death459] in a dungeon the corpse is the only way back
        window = (!HealerLeft(bot) || HealerSickens(bot) || bot->GetMap()->IsDungeon()) &&
                 SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Fixed) >= 0.0f &&
                 SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Now) >= 0.0f;
        if (window)
        {
            float const paths = SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Paths);  // [rise33]
            if (paths < 0.0f)
            {
                window = false;
                LogGhost(bot, Acore::StringFormat("[rise33] no window here: a wanderer's area or a patrol's path "
                                                  "reaches the spot ({:.0f} yd inside)",
                                                  -paths));
            }
        }
        return window;
    }

    // [guild13] user 2026-11-07: after a death a solo questing bot does not go straight back - it rises in town. Below
    // the level of resurrection sickness (CONFIG_DEATH_SICKNESS_LEVEL) the spirit healer costs it nothing but the
    // durability a corpse run costs too, so it takes the graveyard's healer (b300: Peny L8 rose 6 times at its corpse
    // in the Stone Cairn Lake murlocs and died again within seconds); above it the corpse run stays (a safe spot)
    bool RisesInTown(Player* bot)
    {
        // [deaths3] alone by SoloSafety's rule (no living member within the group reward distance); never to a healer
        // found in reach of a hostile for this death
        return SoloSafety::Applies(bot) &&
               int32(bot->GetLevel()) < sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL) && HealerLeft(bot);
    }
}

bool ReviveFromCorpseAction::Execute(Event event)
{
    // BG ghosts are revived by spirit healer waves; teleporting to a world graveyard removes them from the BG.
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();

    // follow group Leader when group Leader revives
    WorldPacket& p = event.getPacket();
    if (!p.empty() && p.GetOpcode() == CMSG_RECLAIM_CORPSE && groupLeader && !corpse && bot->IsAlive())
    {
        if (ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.FarDistance))
        {
            if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            {
                botAI->TellMasterNoFacing("Welcome back!");
                botAI->ChangeStrategy("+follow,-stay", BOT_STATE_NON_COMBAT);
                return true;
            }
        }
    }

    if (!corpse)
        return false;

    if (RisesInTown(bot))  // [guild13] the spirit healer, not the corpse
        return false;

    // if (corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) >
    // time(nullptr))
    //     return false;

    if (groupLeader)
    {
        if (!GET_PLAYERBOT_AI(groupLeader) && groupLeader->isDead() && groupLeader->GetCorpse() &&
            ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.FarDistance))
            return false;
    }

    if (!botAI->HasGameClientMaster())
    {
        uint32 dCount = AI_VALUE(uint32, "death count");

        // In a dungeon the corpse is the only way back: its spirit healer stands at the graveyard outside.
        // [ghost34] ... and only where the healer costs no sickness, as "find corpse" rules ([ghost14]): the two
        // disagreed from the third death on - "find corpse" walked the ghost back to its corpse ("the spot is safe"),
        // this action sent it to the healer, round after round (b386 Aestymro L13 at Longshore: 8 rounds, ~25 min)
        if (dCount >= 5 && !bot->GetMap()->IsDungeon() && HealerLeft(bot) && !HealerSickens(bot))  // [deaths3]
            return botAI->DoSpecificAction("spirit healer");
    }

    // Rise only where nothing comes at once; "find corpse" walks to a safe spot of the corpse's radius meanwhile.
    // [deaths3] no time limit (no-forcing-timers): the spot is clear, or its window is open (RisesHere)
    bool window = false;
    if (!RisesHere(botAI, bot, AI_VALUE(GuidVector, "nearest hostile npcs"), window))
        return false;

    // [guild9] the reclaim delay still runs: HandleReclaimCorpseOpcode refuses it (MiscHandler), so nothing is sent
    // and nothing is logged; the log showed ~100 "revives at body" per hunter death while it waited.
    if (time_t(corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP)) >
        time_t(GameTime::GetGameTime().count()))
        return true;

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at body", bot->GetGUID().ToString().c_str(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    if (bot->GetGroup() || window)
        LOG_INFO("playerbots", "[corpse res] {} revives at ({:.1f},{:.1f},{:.1f}) {:.1f} yd from its corpse{}",
                 bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                 bot->GetExactDist(corpse),
                 window ? ": a window - no healer left, the wanderers / patrols out of its reach now" : "");

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();

    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetGUID();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);

    return true;
}

namespace
{
    // [guild15] b303-b308: six ghosts (Alnia, Arrengyl, Baridrol, Eraelor, Sycina, Thelengyl) stood at the Shadowglen
    // graveyard 17 yd from its Spirit Healer for five runs without a line in the log: one line per bot and outcome
    // [stuck8] MoveTo's false while the walk it asked for is still under way ("the same walk is on", "the last walk is
    // not done"): no failure (b343 Guinudrond: 15 "the walk to graveyard 1472 ... failed" lines while it walked
    // 354 -> 21 yd and revived there)
    bool WalkStillOn()
    {
        std::string const& refusal = LastMoveRefusal();
        return refusal.rfind("IsDuplicateMove", 0) == 0 || refusal.rfind("IsWaitingForLastMove", 0) == 0;
    }

    void LogGhost(Player* bot, std::string const& what)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, std::string> last;
        {
            std::lock_guard<std::mutex> guard(lock);
            std::string& previous = last[bot->GetGUID().GetCounter()];
            if (previous == what)
                return;
            previous = what;
        }
        LOG_INFO("playerbots", "[ghost] {} L{} at ({:.1f},{:.1f},{:.1f}): {}", bot->GetName(), bot->GetLevel(),
                 bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), what);
    }

    // [ghost13] the corpse whose ghost chose the spirit healer (bot -> corpse): the choice holds for that corpse. b353
    // Laaral, Rumanatol, Febineysh, Januel (logged in as ghosts at their corpses): "no spot clear: to the spirit
    // healer", the walk to the graveyard 470-600 yd off started, and once the ghost was a reaction distance from the
    // corpse "find corpse" walked it back to the corpse - 9-14 rounds each, the whole run, never risen
    std::mutex healerChoiceLock;
    std::unordered_map<uint32, ObjectGuid> healerChoice;

    void ChooseHealer(Player* bot, Corpse* corpse)
    {
        std::lock_guard<std::mutex> guard(healerChoiceLock);
        healerChoice[bot->GetGUID().GetCounter()] = corpse ? corpse->GetGUID() : ObjectGuid::Empty;
    }

    bool HealerChosen(Player* bot, Corpse* corpse)
    {
        std::lock_guard<std::mutex> guard(healerChoiceLock);
        auto const found = healerChoice.find(bot->GetGUID().GetCounter());
        return found != healerChoice.end() && corpse && found->second == corpse->GetGUID();
    }

    // [death458] the core sends a ghost in dark water (the fatigue bar) back to a graveyard (Player::HandleDrowning ->
    // RepopAtGraveyard): no rise spot lies there, and no walk to one goes through it
    constexpr float DARK_WATER_STEP = 10.0f;
    // a rise spot the walk ended this near is reached; a dropped spot drops the candidates this near it too
    constexpr float SPOT_REACHED = 3.0f;
    constexpr float SPOT_DROP_RANGE = 5.0f;

    bool InDarkWater(Player* bot, float x, float y, float z)
    {
        LiquidData const liquid =
            bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight(), {});
        return (liquid.Flags & MAP_LIQUID_TYPE_DARK_WATER) && liquid.Status != LIQUID_MAP_NO_WATER &&
               liquid.Status != LIQUID_MAP_ABOVE_WATER;
    }

    // [death458] a walk on the navmesh that ends within the corpse's reclaim radius and crosses no dark water. b468
    // Colian L23 had died at sea off Bloodmyst (-518,-10326): no walk reaches it ([nav163]), its ghost walked the
    // coast's partial path into the dark water, the core put it back at Blood Watch, and round again - 58 min, 69
    // "[rise33] no window here" on the way and never a healer
    bool CorpseReachable(Player* bot, Corpse* corpse, float reach, std::string& why)
    {
        G3D::Vector3 const dest(corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
        if (bot->GetExactDist(corpse) <= reach)
            return true;
        Movement::PointsArray corners;
        if (!SelfTravel::LongRoute(bot, dest, corners, reach))
        {
            why = "no walk on the navmesh reaches its corpse's radius";
            return false;
        }
        for (size_t i = 1; i < corners.size(); ++i)
        {
            G3D::Vector3 const leg = corners[i] - corners[i - 1];
            uint32 const steps = std::max<uint32>(1, uint32(leg.length() / DARK_WATER_STEP));
            for (uint32 s = 1; s <= steps; ++s)
            {
                G3D::Vector3 const p = corners[i - 1] + leg * (float(s) / steps);
                if (InDarkWater(bot, p.x, p.y, p.z))
                {
                    why = Acore::StringFormat("its walk crosses dark water at ({:.0f},{:.0f}): the core sends a ghost "
                                              "there back to a graveyard",
                                              p.x, p.y);
                    return false;
                }
            }
        }
        return true;
    }
}

bool FindCorpseAction::Execute(Event /*event*/)
{
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        // [guild15] a ghost with no corpse on this map (logged in as a ghost, the corpse elsewhere): the spirit healer
        if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) && !bot->GetMap()->IsDungeon())
        {
            LogGhost(bot, "a ghost with no corpse on this map: to the spirit healer");
            return botAI->DoSpecificAction("spirit healer", Event(), true);
        }
        return false;
    }

    // if (groupLeader)
    // {
    //     if (!GET_PLAYERBOT_AI(groupLeader) &&
    //         ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
    //         sPlayerbotAIConfig.FarDistance)) return false;
    // }

    uint32 dCount = AI_VALUE(uint32, "death count");

    // A bot that keeps dying takes the spirit healer like a player would, not a free revive - in the world. In a
    // dungeon its healer stands at the graveyard outside and the corpse is the only way back (the count grows by two
    // per death, death and release, and never resets for a bot that gains no XP: from the third death every ghost
    // that had walked back in stood at its corpse calling for a healer it could not reach).
    bool const inDungeon = bot->GetMap()->IsDungeon();
    // [ghost40] user 2026-11-09: a ghost whose weapon is broken cannot fight what comes at its corpse: the spirit
    // healer, never the corpse among hostiles (b388 Peny / Irrua at 0 durability rose at their corpses in the camp,
    // 5 deaths each)
    if (!inDungeon && !bot->InBattleground() && sGuildCoordinator.Repair(bot).weaponBroken && HealerLeft(bot) &&
        !AI_VALUE(GuidVector, "nearest hostile npcs").empty())
    {
        LogGhost(bot, "[ghost40] its weapon is broken: to the spirit healer, not its corpse among hostiles");
        ChooseHealer(bot, corpse);
        return botAI->DoSpecificAction("spirit healer", Event(), true);
    }
    // [ghost14] ... only where the healer costs no sickness: from its level on the corpse comes first
    if (((!botAI->HasGameClientMaster() && dCount >= 5 && !inDungeon && !HealerSickens(bot)) || RisesInTown(bot)) &&
        HealerLeft(bot))  // [guild13] [deaths3]
        return botAI->DoSpecificAction("spirit healer", Event(), true);

    WorldPosition botPos(bot);
    WorldPosition corpsePos(corpse);
    WorldPosition moveToPos = corpsePos;
    WorldPosition leaderPos(groupLeader);

    float reclaimDist = CORPSE_RECLAIM_RADIUS - 5.0f;
    float corpseDist = botPos.distance(corpsePos);
    // A living leader at the corpse: rise next to him (a dead one lies among the mobs that killed him).
    bool moveToLeader = groupLeader && groupLeader != bot && groupLeader->IsAlive() &&
                        leaderPos.fDist(corpsePos) < reclaimDist;

    // [death458] a new corpse: its reach not searched yet, no spot dropped, no walk on for it
    if (_corpseGuid != corpse->GetGUID())
    {
        _corpseGuid = corpse->GetGUID();
        _reachSearched = false;
        _droppedSpots.clear();
        _walk = Walk::None;
        _spotMs = 0;
    }
    // [death458] a corpse no walk reaches: the spirit healer is the only way back, whatever its sickness costs
    if (!inDungeon && !_reachSearched && !HealerChosen(bot, corpse))
    {
        _reachSearched = true;
        std::string why;
        if (!CorpseReachable(bot, corpse, reclaimDist, why))
        {
            std::string const price =
                HealerSickens(bot)
                    ? Acore::StringFormat("{} min of Resurrection Sickness", SicknessMinutes(bot))
                    : std::string("no sickness at its level");
            LogGhost(bot, Acore::StringFormat("[death458] its corpse {:.0f} yd off at ({:.0f},{:.0f}) is out of "
                                              "reach ({}): to the spirit healer ({})",
                                              corpseDist, corpse->GetPositionX(), corpse->GetPositionY(), why, price));
            ChooseHealer(bot, corpse);
            _walk = Walk::None;
            return botAI->DoSpecificAction("spirit healer", Event(), true);
        }
    }

    GuidVector const hostiles = AI_VALUE(GuidVector, "nearest hostile npcs");
    bool window = false;
    // [deaths3] clear, or its window open. [death458] judged in the corpse's react range only: no spot of its radius
    // is chosen from farther (b468 Colian: 69 "[rise33] no window here" 300-1500 yd from its corpse)
    bool const safeHere = corpseDist < sPlayerbotAIConfig.ReactDistance && RisesHere(botAI, bot, hostiles, window);

    // Should we ressurect? If so, return false.
    if (corpseDist < reclaimDist)
    {
        // [ghost12] next to a living leader it rises only where "revive from corpse" rises (RisesHere): that action
        // refuses a spot in a hostile's reach, and this one stood still for it - b350 Febineysh L7 stood the whole
        // run as a ghost on its corpse in Loch Modan, its leader Orton alive 27 yd off (GROUP_WAIT "Febineysh dead"
        // for the run). Not safe: the spot / healer choices below, as for a ghost alone
        if (moveToLeader && safeHere)  // We are near group leader.
        {
            if (botPos.fDist(leaderPos) < sPlayerbotAIConfig.SpellDistance)
                return false;
        }
        else if (safeHere)  // Nothing comes at once here: the same rule as "revive from corpse".
        {
            if (HealerSickens(bot) && !bot->GetMap()->IsDungeon())
                LogGhost(bot, Acore::StringFormat("[ghost14] rises at its corpse: the spot is safe (the spirit healer "
                                                  "would cost {} min of Resurrection Sickness)",
                                                  SicknessMinutes(bot)));
            return false;
        }
    }

    // The ghost always runs back: no teleport to the corpse, also for bots with throttled activity.
    // [death459] a walk to the corpse started from beyond its react range is decided again inside it: it walked on
    // into the corpse's death zone and pack (b469 Kylanor L21 from the Raven Hill graveyard to its corpse among the
    // Skeletal Horrors, round after round)
    if (bot->isMoving() && !(_walk == Walk::CorpseFar && corpseDist < sPlayerbotAIConfig.ReactDistance))
        return true;

    // [ghost13] the spirit healer chosen at this corpse: the ghost keeps to that walk (no walk back to the corpse).
    // [death458] also with no healer clear of reaches left: that healer's own window is its way back then
    if (!inDungeon && HealerChosen(bot, corpse))
    {
        LogGhost(bot, "[ghost13] keeps to the spirit healer it chose at its corpse");
        return botAI->DoSpecificAction("spirit healer", Event(), true);
    }

    // [death459] the end of the walk to a rise spot: it rises there (above), waits there for a window that can open
    // (no mob that stays, no roamer's round and no death zone reaches it: only a mover where it is now), else the
    // spot is dropped for this corpse and the next one taken - none left: the spirit healer below. b469 Mytaeleas
    // L21 at Windshear Mine: its walks to a rise spot ended 5 / 11 yd inside the zone where it died, back and forth
    if (_walk == Walk::Spot)
    {
        float const x = bot->GetPositionX();
        float const y = bot->GetPositionY();
        float const z = bot->GetPositionZ();
        float const off = bot->GetExactDist2d(_walkTo.x, _walkTo.y);
        float const zone = ZoneMargin(bot, x, y);
        float const fixed = SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Fixed);
        float const paths = SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Paths);
        std::string const refusal = LastMoveRefusal().empty() ? std::string("no reason") : LastMoveRefusal();
        if (off > SPOT_REACHED)
            DropSpot(Acore::StringFormat("its walk there ended {:.0f} yd short at ({:.0f},{:.0f}) ({})", off, x, y,
                                         refusal));
        else if (corpseDist >= reclaimDist)
            DropSpot("it lies out of its corpse's reclaim radius");
        else if (zone < 0.0f)
            DropSpot(Acore::StringFormat("the zone where it died covers it ({:.0f} yd inside)", -zone));
        else if (fixed < 0.0f)
            DropSpot(Acore::StringFormat("a mob that stays reaches it ({:.0f} yd inside)", -fixed));
        else if (paths < 0.0f)
            DropSpot(Acore::StringFormat("a wanderer's area or a patrol's path reaches it ({:.0f} yd inside)", -paths));
        else if (!HealerLeft(bot) || HealerSickens(bot) || inDungeon)
        {
            LogGhost(bot, Acore::StringFormat("[death459] waits at its rise spot ({:.0f},{:.0f}): only a wanderer / "
                                              "patrol where it is now reaches it",
                                              x, y));
            return true;
        }
        else
            DropSpot("a mover reaches it now and the spirit healer costs no sickness");
    }

    Walk walk = corpseDist < sPlayerbotAIConfig.ReactDistance ? Walk::Corpse : Walk::CorpseFar;
    // If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.ReactDistance)
    {
        G3D::Vector3 spot;
        if (moveToLeader && safeHere)  // [ghost12] as above
        {
            moveToPos = leaderPos;
            walk = Walk::None;
        }
        else if (SafeSpot(corpse, hostiles, reclaimDist, spot))
        {
            moveToPos = WorldPosition(corpsePos.GetMapId(), spot.x, spot.y, spot.z, 0.0f);
            walk = Walk::Spot;
        }
        // [deaths3] no spot of the corpse's radius out of every reach: the spirit healer, unless its spot was found in
        // reach for this death too (then the ghost keeps to the corpse and rises once a spot clears)
        // [ghost14] the healer would sicken it: a spot where nothing that stays reaches it, risen once the wanderers
        // and patrols are out of its reach (the window), before the healer
        else if (!safeHere && !inDungeon && HealerLeft(bot) && HealerSickens(bot) &&
                 SafeSpot(corpse, hostiles, reclaimDist, spot, true))
        {
            LogGhost(bot, Acore::StringFormat("[ghost14] rises at its corpse, not the spirit healer (its Resurrection "
                                              "Sickness: {} min): waits at ({:.0f},{:.0f}) for the wanderers / patrols "
                                              "to leave its reach",
                                              SicknessMinutes(bot), spot.x, spot.y));
            moveToPos = WorldPosition(corpsePos.GetMapId(), spot.x, spot.y, spot.z, 0.0f);
            walk = Walk::Spot;
        }
        else if (!safeHere && !inDungeon && HealerLeft(bot))
        {
            LogGhost(bot, HealerSickens(bot)
                              ? Acore::StringFormat("[ghost14] no spot of its corpse's radius clear of the mobs that "
                                                    "stay there: to the spirit healer ({} min of Resurrection Sickness)",
                                                    SicknessMinutes(bot))
                              : std::string("no spot of its corpse's radius clear of the mobs there: to the spirit "
                                            "healer (no sickness at its level)"));
            ChooseHealer(bot, corpse);  // [ghost13]
            return botAI->DoSpecificAction("spirit healer", Event(), true);
        }
        // [deaths3] no healer left: a spot no mob that stays reaches - it rises there once the wanderers / patrols
        // are out of its reach
        else if (!safeHere && SafeSpot(corpse, hostiles, reclaimDist, spot, true))
        {
            LogGhost(bot, Acore::StringFormat("no healer left and no clear spot: waits at ({:.0f},{:.0f}) for the "
                                              "wanderers / patrols to leave its reach",
                                              spot.x, spot.y));
            moveToPos = WorldPosition(corpsePos.GetMapId(), spot.x, spot.y, spot.z, 0.0f);
            walk = Walk::Spot;
        }
        // [death458] no spot of its radius whose window can open and no healer clear of reaches: the nearest healer
        // and its own window - a wait at the corpse for a window that cannot open is no way back
        else if (!safeHere && !inDungeon)
        {
            LogGhost(bot, Acore::StringFormat("[death458] no spot of its corpse's radius whose window can open, no "
                                              "spirit healer clear of reaches: to the nearest healer and its window "
                                              "({} min of Resurrection Sickness)",
                                              SicknessMinutes(bot)));
            ChooseHealer(bot, corpse);
            return botAI->DoSpecificAction("spirit healer", Event(), true);
        }
        else if (!safeHere)
        {
            LogGhost(bot, "no healer left, every spot of its corpse's radius in reach of mobs that stay there");
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);
            walk = Walk::None;

            if (manager.isUseful())
            {
                float rx, ry, rz;
                if (manager.CalculateDestination(&rx, &ry, &rz))
                    moveToPos = WorldPosition(moveToPos.GetMapId(), rx, ry, rz, 0.0);
                else if (!moveToPos.GetReachableRandomPointOnGround(bot, reclaimDist, urand(0, 1)))
                    moveToPos = corpsePos;
            }
        }
    }

    // Actual mobing part.
    // [deaths3] the ghost walks to its corpse whenever it gets here (the healer choices went first): no time limit
    bool const moved = MoveTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(),
                              moveToPos.GetPositionZ(), false, false);
    _walkTo = G3D::Vector3(moveToPos.GetPositionX(), moveToPos.GetPositionY(), moveToPos.GetPositionZ());
    if (moved || WalkStillOn())
    {
        _walk = walk;
        return true;
    }
    // [death459] a rise spot it cannot walk to is dropped: the next one, else the healer
    if (walk == Walk::Spot)
    {
        DropSpot(Acore::StringFormat("its walk there is refused ({})",
                                     LastMoveRefusal().empty() ? std::string("no reason") : LastMoveRefusal()));
        return true;
    }
    // No spirit healer inside a dungeon: the ghost waits at its spot for the mobs to leave.
    if (inDungeon)
        return false;
    // [death458] the walk back failed: the healer, chosen for this corpse - an unchosen walk to it was cut by the walk
    // back to the corpse at its next stop (b469 Kylanor L21, three rounds corpse <-> graveyard 911)
    LogGhost(bot, Acore::StringFormat("[death458] the walk to its corpse failed ({}): to the spirit healer",
                                      LastMoveRefusal().empty() ? std::string("no reason") : LastMoveRefusal()));
    ChooseHealer(bot, corpse);
    _walk = Walk::None;
    return botAI->DoSpecificAction("spirit healer", Event(), true);
}

// [death459] a rise spot that did not hold for this corpse: not chosen again (nor the candidates near it)
void FindCorpseAction::DropSpot(std::string const& why)
{
    LogGhost(bot, Acore::StringFormat("[death459] drops its rise spot ({:.0f},{:.0f}): {}", _walkTo.x, _walkTo.y,
                                      why));
    _droppedSpots.push_back(_walkTo);
    _spotMs = 0;
    _walk = Walk::None;
}

// The spot of the corpse's reclaim radius farthest from the reach of the hostiles around, with a complete walk to it:
// a player rises out of sight of the pack that killed him. Kept for SPOT_RECHECK_MS.
bool FindCorpseAction::SafeSpot(Corpse* corpse, GuidVector const& hostiles, float radius, G3D::Vector3& spot,
                                bool window)
{
    uint32 const now = getMSTime();
    if (_spotMs && getMSTimeDiff(_spotMs, now) < SPOT_RECHECK_MS && _spotWindow == window)
    {
        spot = _spot;
        return _spotFound;
    }
    _spotMs = now;
    _spotWindow = window;
    _spotFound = false;
    if (hostiles.empty() && ZoneMargin(bot, corpse->GetPositionX(), corpse->GetPositionY()) >= 0.0f)  // [death425]
        return false;

    struct Candidate
    {
        float margin;
        G3D::Vector3 point;
    };
    std::vector<Candidate> candidates;
    for (float const ring : SPOT_RINGS)
    {
        if (ring > radius)
            continue;
        for (uint32 i = 0; i < SPOT_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / SPOT_DIRECTIONS;
            float const x = corpse->GetPositionX() + std::cos(angle) * ring;
            float const y = corpse->GetPositionY() + std::sin(angle) * ring;
            float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, corpse->GetPositionZ() + SPOT_SNAP_UP,
                                                     true, SPOT_SNAP_RANGE);
            if (z <= INVALID_HEIGHT || std::fabs(z - corpse->GetPositionZ()) > SPOT_MAX_DZ)
                continue;
            // [death459] a spot that did not hold for this corpse, and [death458] dark water (the core sends the
            // ghost back to a graveyard there)
            if (std::any_of(_droppedSpots.begin(), _droppedSpots.end(), [x, y](G3D::Vector3 const& dropped)
                            { return std::hypot(dropped.x - x, dropped.y - y) <= SPOT_DROP_RANGE; }) ||
                InDarkWater(bot, x, y, z))
                continue;
            // [ghost36] a window spot is one whose window can open: out of every roamer's round too ([rise33] refuses
            // the rise inside one) - else the ghost waited there for good (b387: 157 "[rise33] no window here",
            // Aestymro 45 at (-10244,1936), Irrua 43). With none in the corpse's ring, the spirit healer below
            float margin = window ? std::min(SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Fixed),
                                             SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Paths))
                                  : SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Full);
            margin = std::min(margin, ZoneMargin(bot, x, y));  // [death425] as RisesHere counts it
            if (margin >= 0.0f)
                candidates.push_back({margin, G3D::Vector3(x, y, z)});
        }
    }
    // On the group's side: the safe spot nearest to the living member nearest to the corpse (the ghost rises where
    // the group can take it in, not on the far side of the pack); without one, the farthest from the reaches.
    Player* nearestMember = nullptr;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->IsInWorld() &&
                member->GetMap() == bot->GetMap() &&
                (!nearestMember || corpse->GetExactDist(member) < corpse->GetExactDist(nearestMember)))
                nearestMember = member;
        }
    }

    if (nearestMember)
        std::sort(candidates.begin(), candidates.end(),
                  [nearestMember](Candidate const& a, Candidate const& b)
                  {
                      return nearestMember->GetExactDist(a.point.x, a.point.y, a.point.z) <
                             nearestMember->GetExactDist(b.point.x, b.point.y, b.point.z);
                  });
    else
        std::sort(candidates.begin(), candidates.end(),
                  [](Candidate const& a, Candidate const& b) { return a.margin > b.margin; });

    uint32 tries = 0;
    for (Candidate const& candidate : candidates)
    {
        if (++tries > SPOT_PATH_TRIES)
            break;
        BotPath const path = FindBotPath(bot, candidate.point.x, candidate.point.y, candidate.point.z);  // [lead109]
        if (path.type != PATHFIND_NORMAL)
            continue;
        _spot = candidate.point;
        _spotFound = true;
        spot = _spot;
        return true;
    }
    return false;
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    return bot->GetCorpse() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);  // [guild15]
}

GraveyardStruct const* SpiritHealerAction::GetGrave(bool startZone)
{
    GraveyardStruct const* ClosestGrave = nullptr;
    GraveyardStruct const* NewGrave = nullptr;

    ClosestGrave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    GraveyardStruct const* ownGrave = ClosestGrave;

    if (!startZone && ClosestGrave)
        return ClosestGrave;

    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) && botAI->GetGroupLeader() && botAI->GetGroupLeader() != bot)
    {
        Player* groupLeader = botAI->GetGroupLeader();
        if (groupLeader && groupLeader != bot)
        {
            ClosestGrave = sGraveyard->GetClosestGraveyard(groupLeader, bot->GetTeamId());

            if (ClosestGrave)
                return ClosestGrave;
        }
    }
    // [grave136] a guild bot's travel target is no plan of its own (New RPG plans it): the stale one - b428 Feeli L16
    // (Alliance), 10 deaths in an hour in Ashenvale, took the graveyard by its old target, Ragefire Chasm's Orgrimmar:
    // its ghost walked 2000 yd to Razor Hill's graveyard 850, rose there and died to Orgrimmar Grunts L75 three times.
    // It takes its own nearest graveyard
    else if (startZone && AI_VALUE(uint8, "durability") && !GuildHonest::IsHonestBot(bot))
    {
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

        if (travelTarget->getPosition())
        {
            WorldPosition travelPos = *travelTarget->getPosition();
            if (travelPos.GetMapId() != uint32(-1))
            {
                uint32 areaId = 0;
                uint32 zoneId = 0;
                sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, travelPos.GetMapId(), travelPos.GetPositionX(),
                                          travelPos.GetPositionY(), travelPos.GetPositionZ());
                ClosestGrave = sGraveyard->GetClosestGraveyard(travelPos.GetMapId(), travelPos.GetPositionX(), travelPos.GetPositionY(),
                                                               travelPos.GetPositionZ(), bot->GetTeamId(), areaId, zoneId,
                                                               bot->getClass() == CLASS_DEATH_KNIGHT);

                if (ClosestGrave)
                    return ClosestGrave;
            }
        }
    }

    if (ownGrave && bot->GetLevel() > START_ZONE_GRAVEYARD_MAX_LEVEL)
        return ownGrave;

    std::vector<uint32> races;

    if (bot->GetTeamId() == TEAM_ALLIANCE)
        races = {RACE_HUMAN, RACE_DWARF, RACE_GNOME, RACE_NIGHTELF, RACE_DRAENEI};
    else
        races = {RACE_ORC, RACE_TROLL, RACE_TAUREN, RACE_UNDEAD_PLAYER, RACE_BLOODELF};

    float graveDistance = -1;

    WorldPosition botPos(bot);

    for (auto race : races)
    {
        for (uint32 cls = 0; cls < MAX_CLASSES; cls++)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
            if (!info)
                continue;

            uint32 areaId = 0;
            uint32 zoneId = 0;
            sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, info->mapId, info->positionX,
                                      info->positionY, info->positionZ);

            NewGrave = sGraveyard->GetClosestGraveyard(info->mapId, info->positionX, info->positionY, info->positionZ,
                                                       bot->GetTeamId(), areaId, zoneId, cls == CLASS_DEATH_KNIGHT);
            if (!NewGrave)
                continue;

            WorldPosition gravePos(NewGrave->Map, NewGrave->x, NewGrave->y, NewGrave->z);

            float newDist = botPos.fDist(gravePos);

            if (graveDistance < 0 || newDist < graveDistance)
            {
                ClosestGrave = NewGrave;
                graveDistance = newDist;
            }
        }
    }

    return ClosestGrave;
}

bool SpiritHealerAction::Execute(Event /*event*/)
{
    // GetGrave() picks world graveyards; teleporting there from a BG removes the bot from it.
    if (bot->InBattleground())
        return false;

    // Player::GetCorpse looks on the current map only: a ghost whose corpse lies in an instance has none here, and
    // its spirit healer is the one outside (wave 11: every dungeon corpse run reported "no spirit healer").
    if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        botAI->TellError("I am not a spirit");
        return false;
    }

    Corpse* corpse = bot->GetCorpse();
    uint32 dCount = AI_VALUE(uint32, "death count");
    // [death458] its corpse is a way back unless the ghost chose the healer at it (no spot whose window can open there,
    // or no walk reaches it): "find corpse" sends that ghost here again, the corpse run would hand it straight back
    bool const corpseRun = corpse && !bot->GetMap()->IsDungeon() && !HealerChosen(bot, corpse);

    // [deaths3] no time rule (no-forcing-timers): the start-zone graveyard for a bot that keeps dying or whose gear is
    // near broken
    GraveyardStruct const* ClosestGrave = GetGrave(dCount > 10 || AI_VALUE(uint8, "durability") < 10);
    // [deaths3] a graveyard whose healer stood in reach of a hostile for this death: the next one on its map
    if (ClosestGrave && GraveUnsafe(bot, ClosestGrave->ID))
    {
        uint32 const unsafeId = ClosestGrave->ID;
        ClosestGrave = SafeGrave(bot);
        if (!ClosestGrave)
        {
            if (corpseRun)
            {
                LogGhost(bot, Acore::StringFormat("graveyard {} unsafe, no other on this map: its corpse run",
                                                  unsafeId));
                return botAI->DoSpecificAction("find corpse", Event(), true);
            }
            ClosestGrave = sGraveyard->GetGraveyard(unsafeId);  // no corpse run: that healer's window
        }
        else
            LogGhost(bot, Acore::StringFormat("graveyard {} unsafe: to graveyard {} ({:.0f},{:.0f})", unsafeId,
                                              ClosestGrave->ID, ClosestGrave->x, ClosestGrave->y));
    }

    if (!ClosestGrave)
    {
        LogGhost(bot, "no graveyard");
        return false;
    }

    if (bot->GetDistance2d(ClosestGrave->x, ClosestGrave->y) < sPlayerbotAIConfig.SightDistance)
    {
        // [guild15] every unit in sight, no line-of-sight filter ("nearest npcs" keeps only those in line of sight:
        // b310 the six Shadowglen ghosts 17 yd from the Spirit Healer saw "no spirit healer in sight" - the walk to it
        // is a navmesh path, line of sight is not needed for it)
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, sPlayerbotAIConfig.SightDistance);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, sPlayerbotAIConfig.SightDistance);
        units.sort([this](Unit* a, Unit* b) { return bot->GetDistance(a) < bot->GetDistance(b); });
        for (Unit* unit : units)
        {
            if (!unit || unit->IsPlayer() || !unit->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER))
                continue;

            if (bot->GetDistance(unit) > INTERACTION_DISTANCE)
            {
                bool const moving = MoveNear(unit, INTERACTION_DISTANCE - 1.0f) || WalkStillOn();
                LogGhost(bot, Acore::StringFormat("walks to the spirit healer {:.1f} yd off: {} (can move {}, rooted {})",
                                                  bot->GetDistance(unit), moving ? "moving" : "the move failed",
                                                  botAI->CanMove(), bot->IsRooted()));
                return moving;
            }

            // [deaths3] the healer revives it where it stands: in reach of a hostile this graveyard is unsafe for
            // this death - the next graveyard on its map, else its corpse run; with neither it rises in the window
            GuidVector const hostiles = AI_VALUE(GuidVector, "nearest hostile npcs");
            // [grave44] ... and a wanderer anywhere in its area, a patrol anywhere on its path, reaches the yard while
            // the bot stands there sick: Full read a patrol where it stood at the look. b391: Kianey x48, Bami x42,
            // Peny x29 revived at the Westfall healer by Demont's Place, the Defias Pathstalker patrols on them within
            // seconds, over and over
            float const margin =
                std::min(SpotMargin(botAI, bot, hostiles, bot->GetPositionX(), bot->GetPositionY(),
                                    bot->GetPositionZ(), Reach::Full),
                         SpotMargin(botAI, bot, hostiles, bot->GetPositionX(), bot->GetPositionY(),
                                    bot->GetPositionZ(), Reach::Paths));
            if (margin < 0.0f)
            {
                MarkGraveUnsafe(bot, ClosestGrave->ID);
                if (GraveyardStruct const* next = SafeGrave(bot))
                {
                    LogGhost(bot, Acore::StringFormat("the spirit healer of graveyard {} stands {:.0f} yd inside a "
                                                      "hostile's reach: to graveyard {} ({:.0f},{:.0f})",
                                                      ClosestGrave->ID, -margin, next->ID, next->x, next->y));
                    return true;
                }
                if (corpseRun)
                {
                    LogGhost(bot, Acore::StringFormat("the spirit healer of graveyard {} stands {:.0f} yd inside a "
                                                      "hostile's reach, no other graveyard: its corpse run",
                                                      ClosestGrave->ID, -margin));
                    return botAI->DoSpecificAction("find corpse", Event(), true);
                }
                // [death458] the last way back: this healer. It waits while a wanderer / patrol where it is now
                // reaches the yard and nothing that stays does (the window opens as they walk on: their positions,
                // no clock); a yard a mob that stays reaches has no window - [death460] it rises there when the walk
                // out of their reaches is survivable as it rises (HealerRiseSurvives), else it stays a ghost there,
                // logged each time the price changes. The window of RisesHere also wanted every roamer's whole round
                // out of reach ([rise33]): at a healer with a patrol round it never opened
                float const x = bot->GetPositionX();
                float const y = bot->GetPositionY();
                float const z = bot->GetPositionZ();
                float const fixed = SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Fixed);
                float const now = SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Now);
                if (fixed >= 0.0f && now < 0.0f)
                {
                    LogGhost(bot, Acore::StringFormat("[death458] waits at the spirit healer of graveyard {}, its last "
                                                      "way back: a wanderer / patrol where it is now reaches the yard "
                                                      "({:.0f} yd inside)",
                                                      ClosestGrave->ID, -now));
                    return true;
                }
                if (fixed >= 0.0f)
                    LogGhost(bot, Acore::StringFormat("[death458] rises at the spirit healer of graveyard {} in a "
                                                      "window: the wanderers / patrols out of reach where they are "
                                                      "now",
                                                      ClosestGrave->ID));
                else
                {
                    std::string price;
                    if (!HealerRiseSurvives(botAI, bot, hostiles, x, y, z, price))
                    {
                        LogGhost(bot, Acore::StringFormat("[death460] waits as a ghost at the spirit healer of "
                                                          "graveyard {}, its last way back: a mob that stays reaches "
                                                          "the yard ({:.0f} yd inside) and the walk out does not "
                                                          "survive - {}",
                                                          ClosestGrave->ID, -fixed, price));
                        return true;
                    }
                    LogGhost(bot, Acore::StringFormat("[death460] rises at the spirit healer of graveyard {}, its "
                                                      "last way back: a mob that stays reaches the yard ({:.0f} yd "
                                                      "inside), the walk out survives - {}",
                                                      ClosestGrave->ID, -fixed, price));
                }
            }

            {
                std::lock_guard<std::mutex> guard(ghostLock);
                lastHealerRise[bot->GetGUID().GetCounter()] = {ClosestGrave->ID, bot->GetLevel()};  // [grave44]
            }
            LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at spirit healer", bot->GetGUID().ToString().c_str(),
                      bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

            // The real spirit healer: resurrection sickness and durability loss, like for a player.
            WorldPacket packet(CMSG_SPIRIT_HEALER_ACTIVATE, 8);
            packet << unit->GetGUID();
            bot->GetSession()->HandleSpiritHealerActivateOpcode(packet);
            if (!bot->IsAlive())
            {
                LogGhost(bot, Acore::StringFormat("the spirit healer {:.1f} yd off did not revive it",
                                                  bot->GetDistance(unit)));
                return false;
            }

            context->GetValue<Unit*>("current target")->Set(nullptr);
            bot->SetTarget();
            botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

            if (dCount > 20)
                context->GetValue<uint32>("death count")->Set(0);

            return true;
        }
    }

    bool moved = false;

    if (bot->IsWithinLOS(ClosestGrave->x, ClosestGrave->y, ClosestGrave->z))
        moved = MoveNear(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.0);
    else
        moved = MoveTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, false, false);

    if (moved)  // [ghost12] one line per graveyard walk (b350: four ghosts went silent after "to the spirit healer")
        LogGhost(bot, Acore::StringFormat("walks to graveyard {} ({:.0f},{:.0f})", ClosestGrave->ID, ClosestGrave->x,
                                          ClosestGrave->y));
    if (moved || WalkStillOn())
        return true;
    // [ghost58] a far graveyard the single move cannot reach (b398-b400 Orenem L15 / Brelir L16: ghosts in the Cleft of
    // Shadow, Orgrimmar, 1626-1654 yd from graveyard 32, ~570 failed walks each, the whole run): the long navmesh route
    // (SelfTravel::LongRoute past PathGenerator's limits), one leg at a time, as a player's ghost runs it
    {
        static thread_local std::unordered_map<ObjectGuid, SelfTravel::RouteWalk> graveWalks;
        SelfTravel::RouteWalk& walk = graveWalks[bot->GetGUID()];
        G3D::Vector3 leg;
        if (SelfTravel::NextRouteLeg(bot, walk, G3D::Vector3(ClosestGrave->x, ClosestGrave->y, ClosestGrave->z),
                                     INTERACTION_DISTANCE, 60.0f, leg) &&
            bot->GetExactDist(leg.x, leg.y, leg.z) > 1.0f &&
            MoveTo(ClosestGrave->Map, leg.x, leg.y, leg.z, false, false))
        {
            LogGhost(bot, Acore::StringFormat("[ghost58] walks the long route to graveyard {} ({:.0f},{:.0f}), {} "
                                              "corners, next leg ({:.0f},{:.0f})",
                                              ClosestGrave->ID, ClosestGrave->x, ClosestGrave->y, walk.corners.size(),
                                              leg.x, leg.y));
            return true;
        }
    }
    LogGhost(bot, Acore::StringFormat("no spirit healer in sight, the walk to graveyard {} ({:.0f},{:.0f}) {:.0f} yd "
                                      "off failed (can move {}, rooted {}; [ghost58] the move: {})",
                                      ClosestGrave->ID, ClosestGrave->x, ClosestGrave->y,
                                      bot->GetDistance2d(ClosestGrave->x, ClosestGrave->y), botAI->CanMove(),
                                      bot->IsRooted(), LastMoveRefusal().empty() ? "no reason" : LastMoveRefusal()));

    // A ghost that cannot walk there stays put: no teleport to the graveyard. A failed walk is no death: counted as one
    // it pushed every ghost past the "death count" 5 of FindCorpse / ReviveFromCorpse within a few ticks.
    return false;
}

bool SpiritHealerAction::isUseful() { return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST); }

void NoteGhostDeath(Player* bot)
{
    if (!bot || !GET_PLAYERBOT_AI(bot))
        return;
    uint32 const low = bot->GetGUID().GetCounter();
    uint32 grave = 0;
    {
        std::lock_guard<std::mutex> guard(ghostLock);
        auto const rise = lastHealerRise.find(low);
        if (rise == lastHealerRise.end() || rise->second.second != bot->GetLevel())
            return;
        grave = rise->second.first;
    }
    GraveyardStruct const* g = sGraveyard->GetGraveyard(grave);
    if (!g || g->Map != bot->GetMapId() ||
        bot->GetExactDist2d(g->x, g->y) > sPlayerbotAIConfig.SightDistance + MAX_AGGRO_RADIUS)
        return;
    {
        std::lock_guard<std::mutex> guard(ghostLock);
        graveDeaths[low][grave] = bot->GetLevel();
        lastHealerRise.erase(low);
    }
    LOG_INFO("playerbots", "[grave44] {} L{} died {:.0f} yd from graveyard {} its healer raised it at: unsafe for it "
             "until its next level",
             bot->GetName(), bot->GetLevel(), bot->GetExactDist2d(g->x, g->y), grave);
}
