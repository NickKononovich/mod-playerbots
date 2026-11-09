/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WaypointMgr.h"  // [rise33]
#include "ReviveFromCorpseAction.h"
#include "BotPath.h"  // [lead109]
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
#include "PathGenerator.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "SoloSafety.h"  // [deaths3] alone
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"  // [guild13] CONFIG_DEATH_SICKNESS_LEVEL

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
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

    bool GraveUnsafe(Player* bot, uint32 graveId)
    {
        std::lock_guard<std::mutex> guard(ghostLock);
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

    // [deaths3] it rises here: nothing reaches the spot (Full), or - no healer left for this death - the window is
    // open: no mob that stays reaches it and the wanderers / patrols are out of its reach now
    bool RisesHere(PlayerbotAI* botAI, Player* bot, GuidVector const& hostiles, bool& window)
    {
        float const x = bot->GetPositionX();
        float const y = bot->GetPositionY();
        float const z = bot->GetPositionZ();
        window = false;
        if (SpotMargin(botAI, bot, hostiles, x, y, z, Reach::Full) >= 0.0f)
            return true;
        window = (!HealerLeft(bot) || HealerSickens(bot)) &&  // [ghost14] the healer's sickness: the window first
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

    GuidVector const hostiles = AI_VALUE(GuidVector, "nearest hostile npcs");
    bool window = false;
    bool const safeHere = RisesHere(botAI, bot, hostiles, window);  // [deaths3] clear, or its window open

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
    if (bot->isMoving())
        return true;

    // [ghost13] the spirit healer chosen at this corpse: the ghost keeps to that walk (no walk back to the corpse)
    if (!inDungeon && HealerChosen(bot, corpse) && HealerLeft(bot))
    {
        LogGhost(bot, "[ghost13] keeps to the spirit healer it chose at its corpse");
        return botAI->DoSpecificAction("spirit healer", Event(), true);
    }

    // If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.ReactDistance)
    {
        G3D::Vector3 spot;
        if (moveToLeader && safeHere)  // [ghost12] as above
            moveToPos = leaderPos;
        else if (SafeSpot(corpse, hostiles, reclaimDist, spot))
            moveToPos = WorldPosition(corpsePos.GetMapId(), spot.x, spot.y, spot.z, 0.0f);
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
        }
        else if (!safeHere)
        {
            LogGhost(bot, "no healer left, every spot of its corpse's radius in reach of mobs that stay there");
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);

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
    bool moved = false;
    // [deaths3] the ghost walks to its corpse whenever it gets here (the healer choices went first): no time limit
    {
        moved = MoveTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(),
                       moveToPos.GetPositionZ(), false, false);
    }

    // No spirit healer inside a dungeon: the ghost waits at its spot for the mobs to leave.
    if (!moved && !inDungeon && HealerLeft(bot))
        moved = botAI->DoSpecificAction("spirit healer", Event(), true);

    return moved;
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
    if (hostiles.empty())
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
            float const margin = SpotMargin(botAI, bot, hostiles, x, y, z, window ? Reach::Fixed : Reach::Full);
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
    else if (startZone && AI_VALUE(uint8, "durability"))
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
            if (corpse && !bot->GetMap()->IsDungeon())
            {
                LogGhost(bot, Acore::StringFormat("graveyard {} unsafe, no other on this map: its corpse run",
                                                  unsafeId));
                return botAI->DoSpecificAction("find corpse", Event(), true);
            }
            ClosestGrave = sGraveyard->GetGraveyard(unsafeId);  // no corpse here: that healer's window
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
            float const margin = SpotMargin(botAI, bot, hostiles, bot->GetPositionX(), bot->GetPositionY(),
                                            bot->GetPositionZ(), Reach::Full);
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
                if (corpse && !bot->GetMap()->IsDungeon())
                {
                    LogGhost(bot, Acore::StringFormat("the spirit healer of graveyard {} stands {:.0f} yd inside a "
                                                      "hostile's reach, no other graveyard: its corpse run",
                                                      ClosestGrave->ID, -margin));
                    return botAI->DoSpecificAction("find corpse", Event(), true);
                }
                bool window = false;
                if (!RisesHere(botAI, bot, hostiles, window))
                {
                    LogGhost(bot, Acore::StringFormat("waits at the spirit healer of graveyard {}: {:.0f} yd inside a "
                                                      "hostile's reach, no corpse here, no other graveyard",
                                                      ClosestGrave->ID, -margin));
                    return true;
                }
                LogGhost(bot, "rises at the spirit healer in a window: the wanderers / patrols out of its reach now");
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
    LogGhost(bot, Acore::StringFormat("no spirit healer in sight, the walk to graveyard {} ({:.0f},{:.0f}) {:.0f} yd "
                                      "off failed (can move {}, rooted {})",
                                      ClosestGrave->ID, ClosestGrave->x, ClosestGrave->y,
                                      bot->GetDistance2d(ClosestGrave->x, ClosestGrave->y), botAI->CanMove(),
                                      bot->IsRooted()));

    // A ghost that cannot walk there stays put: no teleport to the graveyard. A failed walk is no death: counted as one
    // it pushed every ghost past the "death count" 5 of FindCorpse / ReviveFromCorpse within a few ticks.
    return false;
}

bool SpiritHealerAction::isUseful() { return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST); }
