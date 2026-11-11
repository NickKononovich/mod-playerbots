/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "SlowScope.h"  // [perf26]
#include "BotPath.h"  // [lead109]
#include "BroadcastHelper.h"
#include "GuildCoordinator.h"  // [guild50] the quest reward pick
#include "GuildHonest.h"       // [honest] no teleport for a guild-plan bot
#include "ChatHelper.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "WaypointMgr.h"  // [patrol86] patrol beats in the spawn index
#include "CellImpl.h"  // [red85] the guards round a login spot
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GameObjectModel.h"
#include "GameEventMgr.h"  // [guild15] event spawns in the danger index
#include "DatabaseEnv.h"
#include "Formulas.h"
#include "DpsEstimate.h"  // [flit] the kill time of a grind
#include "SmartScriptMgr.h"  // [flit] kill credits of scripts
#include "Group.h"
#include "LFGMgr.h"
#include "LootObjectStack.h"
#include "GatherNodeMgr.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "QuestLevelRules.h"
#include "NewRpgStrategy.h"
#include "SoloSafety.h"  // [guild13]
#include "NewRpgCoop.h"  // [coop]
#include "NewRpgQuestUse.h"  // [questitems]
#include "PerfMonitor.h"  // [guild15]
#include "QuestStats.h"  // [guild90]
#include "QuestPackets.h"  // [quest14] the grey abandon
#include "World.h"  // [guild13]
#include "RouteSearch.h"  // [guild13] PointOnPath
#include "SpellMgr.h"     // [guild13]
#include "LeaderOrders.h"  // [guild14] Leader::Route
#include "DetourNavMeshQuery.h"  // [guild14] the nearest polygon
#include "MapDefines.h"         // [guild14] NAV_* flags
#include <map>
#include <memory>
#include <optional>  // [runpast15]
#include <tuple>
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "NewRpgFishing.h"  // [fish14]
#include "NewRpgGiverTrigger.h"  // [giver15]
#include "GuildGatherPlan.h"  // [fish14]
#include "OutdoorPvPMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotsDatabase.h"  // [guild15] death zones kept
#include "StringFormat.h"
#include <cstdio>
#include "PlayerbotMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "WorldPacket.h"
#include "G3D/Ray.h"
#include "G3D/Vector2.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>  // [deaths] the node stages
#include <mutex>
#include <shared_mutex>  // [drop74]
#include <queue>  // [deaths] the node stages
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr float DECK_STEP = 1.5f;              // yards between deck probes on the way from the pier
    constexpr float DECK_RAY_ABOVE = 8.0f;          // probes start this far over the pier...
    constexpr float DECK_RAY_LENGTH = 20.0f;        // ...and look this far down for the deck
    constexpr float DECK_MAX_STEP = 6.0f;           // a deck higher or lower than this is not reachable from the pier
    constexpr uint32 DECK_SAMPLES_INSIDE = 3;       // stop a few probes past the rail
    constexpr float ENVOY_REACHED = 4.0f;
    constexpr float CRYSTAL_REACHED = 3.0f;
    constexpr float ENVOY_SEARCH_RANGE = 15.0f;

    float Distance2d(WorldPosition const& a, WorldPosition const& b)
    {
        return std::hypot(a.GetPositionX() - b.GetPositionX(), a.GetPositionY() - b.GetPositionY());
    }
}

namespace
{
    // [deaths18] bot -> the hub its recovery after a death walks to; set when that walk is given up
    std::mutex recoveryLock;
    std::unordered_map<uint32, std::pair<WorldPosition, bool>> recoveryHub;
}

bool NewRpgBaseAction::SoloSafetyStep()
{
    if (bot->IsInCombat() || !bot->IsAlive() || bot->IsInFlight() || bot->GetTransport())
        return false;
    // the user's rule: after a death the bot does not go straight back - it goes to its hub, rests, plans again
    if (SoloSafety::TakeRecovery(bot))
    {
        // risen at the graveyard's spirit healer (below the resurrection-sickness level): it is in town already
        if (int32(bot->GetLevel()) < sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL))
        {
            LOG_INFO("playerbots", "[rpg] {} L{} RECOVER after a death: risen at the graveyard, rests to full, plans "
                     "again", bot->GetName(), bot->GetLevel());
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        WorldPosition const hub = NearestHub(bot);
        if (hub != WorldPosition())
        {
            LOG_INFO("playerbots", "[rpg] {} L{} RECOVER after a death: rests to full, walks to its hub ({:.0f} yd)",
                     bot->GetName(), bot->GetLevel(), bot->GetExactDist(hub));
            {
                std::lock_guard<std::mutex> guard(recoveryLock);
                recoveryHub[bot->GetGUID().GetCounter()] = {hub, false};  // [deaths18]
            }
            botAI->rpgInfo.ChangeToGoCamp(hub);
            return true;
        }
    }
    // [deaths18] coordinator 2026-11-09: its recovery walk out of the camp that killed it was given up (the maze
    // search: every way passes mobs it does not pull): it does not stay by the camp - its Hearthstone, else a help
    // request for the camp; meanwhile it stands out of every idle reach. b359 Guinudrond L12 rose 32 yd from its
    // corpse by the Riverpaw camp in Westfall (L13-18), gave the 744 yd hub walk up, quested on from there and died
    // twice more at the same spot
    WorldPosition blockedHub;
    {
        std::lock_guard<std::mutex> guard(recoveryLock);
        auto const found = recoveryHub.find(bot->GetGUID().GetCounter());
        if (found != recoveryHub.end() && found->second.second)
        {
            blockedHub = found->second.first;
            recoveryHub.erase(found);
        }
    }
    if (blockedHub != WorldPosition())
    {
        std::string const why = "after a death its walk out of the camp to its hub is given up";
        LOG_INFO("playerbots", "[deaths18] {} L{} {}: hearth, else help", bot->GetName(), bot->GetLevel(), why);
        if (HearthOut(why))
            return true;
        NewRpgCoop::OnCampRefused(botAI, bot, blockedHub, why);
    }

    if (SoloSafety::WaitsToRegen(bot))
    {
        // [supply26] nothing to eat or drink: to a vendor that sells it first (StartVendorRun's restock), as a player
        // buys water before sitting for minutes - b375: the regeneration sit came before the vendor look, so 1260
        // "nothing to eat or drink" sits stayed (Orton 233, Orenem 172), five restock runs in all
        if (!bot->IsInCombat() && StartVendorRun())
            return true;
        if (!bot->IsSitState())
        {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            LOG_INFO("playerbots", "[rest] {} sits to regenerate: nothing to eat or drink (hp {:.0f} %, mana {:.0f} %)",
                     bot->GetName(), bot->GetHealthPct(),
                     bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f);
        }
        return true;
    }
    return TameFirstPet();
}

namespace
{
    // [guild90] a spent mark: the bot stood here and saw nothing of the quest within SPENT_RADIUS (no mob it
    // pulls, no object). The spawns there come back at their respawn time (creature / gameobject spawntimesecs,
    // the world's own clock): the mark holds until then. A mark with no spawn of the quest's sources near it (a
    // POI blend) holds until the bot's next level.
    struct SpentMark
    {
        float x, y;
        time_t until;  // 0: until the next level
        uint8 level;
    };
    constexpr float SPENT_RADIUS = 30.0f;
    std::mutex spentLock;
    // bot guid counter -> quest -> the spent marks
    std::unordered_map<uint32, std::unordered_map<uint32, std::vector<SpentMark>>> spentSpots;

    // [guild90] the seconds until the first respawn of the quest's objective sources spawned within SPENT_RADIUS of
    // (x, y) on the bot's map, by the map's respawn clock [flit]; 0: none comes back there (defined below)
    uint32 ObjectiveRespawnNear(Player* bot, uint32 questId, float x, float y);

    void LogQuestObject(Player* bot, GameObject* go, std::string const& verdict);

    bool MarkHolds(SpentMark const& mark, Player* bot, time_t now)
    {
        return mark.until ? now < mark.until : mark.level == bot->GetLevel();
    }
}

void NewRpgBaseAction::NoteSpentSpot(Player* bot, uint32 questId, WorldPosition const& pos)
{
    uint32 const respawn = ObjectiveRespawnNear(bot, questId, pos.GetPositionX(), pos.GetPositionY());
    time_t const now = time(nullptr);
    if (respawn)
        LOG_INFO("playerbots", "[rpg] {} L{} spot of q{} spent at ({:.0f},{:.0f}): nothing to pull or loot within {:.0f} "
                 "yd, back at its respawn ({} s)", bot->GetName(), bot->GetLevel(), questId, pos.GetPositionX(),
                 pos.GetPositionY(), SPENT_RADIUS, respawn);
    else
        LOG_INFO("playerbots", "[rpg] {} L{} spot of q{} spent at ({:.0f},{:.0f}): nothing to pull or loot within {:.0f} "
                 "yd, no respawn of its sources due there (held until the next level)", bot->GetName(), bot->GetLevel(),
                 questId, pos.GetPositionX(), pos.GetPositionY(), SPENT_RADIUS);
    std::lock_guard<std::mutex> guard(spentLock);
    auto& marks = spentSpots[bot->GetGUID().GetCounter()][questId];
    marks.erase(std::remove_if(marks.begin(), marks.end(),
                               [bot, now](SpentMark const& m) { return !MarkHolds(m, bot, now); }),
                marks.end());
    marks.push_back({pos.GetPositionX(), pos.GetPositionY(), respawn ? now + time_t(respawn) : 0, bot->GetLevel()});
}

bool NewRpgBaseAction::IsSpentSpot(Player* bot, uint32 questId, float x, float y)
{
    std::lock_guard<std::mutex> guard(spentLock);
    auto const quests = spentSpots.find(bot->GetGUID().GetCounter());
    if (quests == spentSpots.end())
        return false;
    auto const spots = quests->second.find(questId);
    if (spots == quests->second.end())
        return false;
    time_t const now = time(nullptr);
    // [guild90] the whole scanned circle is spent, not the one point the bot walked to
    for (SpentMark const& mark : spots->second)
        if ((mark.x - x) * (mark.x - x) + (mark.y - y) * (mark.y - y) <= SPENT_RADIUS * SPENT_RADIUS &&
            MarkHolds(mark, bot, now))
            return true;
    return false;
}

void NewRpgBaseAction::ClearSpentSpots(Player* bot, uint32 questId)
{
    std::lock_guard<std::mutex> guard(spentLock);
    auto const quests = spentSpots.find(bot->GetGUID().GetCounter());
    if (quests != spentSpots.end())
        quests->second.erase(questId);
}

namespace
{
    // [guild15] DestinationRefused's memory: bot -> its last look (destination, level; refused), and every destination
    // it refused at its level
    std::mutex refusedDestLock;
    std::unordered_map<uint32, std::pair<std::tuple<int32, int32, uint8>, bool>> lookedDest;
    std::unordered_map<uint32, std::set<std::tuple<int32, int32, uint8>>> refusedDest;
}

bool NewRpgBaseAction::DestinationRefused(WorldPosition const& dest)
{
    if (dest.GetMapId() != bot->GetMapId() || !SoloSafety::Applies(bot) || bot->IsInCombat() ||
        bot->GetExactDist(dest) > sPlayerbotAIConfig.SightDistance)
        return false;
    if (CreatureTemplate const* danger = InDangerZone(bot, dest.GetPositionX(), dest.GetPositionY()))
    {
        LOG_INFO("playerbots", "[solo] {} L{} walk to ({:.0f},{:.0f}) refused: in its danger zone of {}",
                 bot->GetName(), bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY(), danger->Name);
        return true;
    }
    // the idle mob whose reach (aggro radius, its wander area) covers the destination nearest to it: its pull as the
    // judge sees it (the pack it brings); one look per destination
    // (a refusal leaves a danger zone there: the check above answers it from then on)
    // bot -> (destination, level) of its last look, and whether the walk there passes a refused reach
    // [guild15] ... and every refused destination, until the danger changes (its level, a repair: ForgetRefusals):
    // two targets looked at in turn were each judged again on every look (b333 gather routes: Mutil, Ganna, Peny)
    std::tuple<int32, int32, uint8> const key{int32(dest.GetPositionX()), int32(dest.GetPositionY()),
                                              bot->GetLevel()};
    {
        std::lock_guard<std::mutex> guard(refusedDestLock);
        auto& last = lookedDest[bot->GetGUID().GetCounter()];
        if (last.first == key)
            return last.second;
        auto& refused = refusedDest[bot->GetGUID().GetCounter()];
        if (refused.count(key))
            return true;
        last = {key, false};
    }
    auto const remember = [&]()
    {
        std::lock_guard<std::mutex> guard(refusedDestLock);
        lookedDest[bot->GetGUID().GetCounter()].second = true;
        auto& refused = refusedDest[bot->GetGUID().GetCounter()];
        // a refusal of a lower level is judged again at this one
        for (auto it = refused.begin(); it != refused.end();)
            it = std::get<2>(*it) != bot->GetLevel() ? refused.erase(it) : std::next(it);
        refused.insert(key);
    };
    // [guild15] the walk itself: a destination inside a camp it must not fight has no walk round the camp (b323:
    // Baridrol L8 to Oben Rageclaw in Ban'ethil Barrow Den, Orton / Cassand L8 to Rolf's corpse at Stone Cairn
    // Lake, Ravelandy to Jerod's Landing: attacked by 2-6, dead)
    {
        Leader::RouteResult const route = Leader::Route(bot, dest);
        if (route.found && route.throughRefused)
        {
            LOG_INFO("playerbots", "[solo] {} L{} walk to ({:.0f},{:.0f}) refused: its way passes a pack it does not "
                     "fight and no walk goes round it; walls: {}", bot->GetName(), bot->GetLevel(),
                     dest.GetPositionX(), dest.GetPositionY(), SoloSafety::RouteWalls(bot));
            remember();
            return true;
        }
    }
    Creature* nearest = nullptr;
    float nearestGap = std::numeric_limits<float>::max();
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Creature* mob = botAI->GetCreature(guid);
        if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
            !mob->HasReactState(REACT_AGGRESSIVE))
            continue;
        float reach = mob->GetAggroRange(bot);
        if (reach <= 0.0f)
            continue;
        Position centre = mob->GetPosition();
        if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
        {
            centre = mob->GetHomePosition();
            reach += mob->GetWanderDistance();
        }
        float const gap = centre.GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
        if (gap <= reach && gap < nearestGap)
        {
            nearestGap = gap;
            nearest = mob;
        }
    }
    if (!nearest)
        return false;
    SoloSafety::Verdict const verdict = SoloSafety::Judge(bot, nearest);
    if (verdict.kind != SoloSafety::Verdict::Kind::Refuse)
        return false;
    LOG_INFO("playerbots", "[solo] {} L{} walk to ({:.0f},{:.0f}) refused: among {} L{}: {}", bot->GetName(),
             bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY(), nearest->GetName(), nearest->GetLevel(),
             verdict.why);
    RememberDanger(bot, verdict.at.GetPositionX(), verdict.at.GetPositionY(), verdict.radius, nearest->GetEntry(),
                   verdict.topLevel);
    remember();
    return true;
}

namespace
{
    // [tame14] bot -> (its level, the beasts its walk never brought in tame range and sight)
    std::mutex tameSkipLock;
    std::unordered_map<uint32, std::pair<uint8, std::set<ObjectGuid>>> tameSkip;
    std::unordered_map<uint32, ObjectGuid> tameWalk;  // the beast its last tame walk went to
}

bool NewRpgBaseAction::TameFirstPet()
{
    constexpr uint32 SPELL_TAME_BEAST = 1515;
    if (bot->getClass() != CLASS_HUNTER || !bot->HasSpell(SPELL_TAME_BEAST) || bot->GetPet() ||
        bot->GetPetGUID() || bot->IsNonMeleeSpellCast(true) || !SoloSafety::Applies(bot))
        return false;
    if (PetStable* stable = bot->GetPetStable())
        if (stable->CurrentPet || stable->UnslottedPets.size())
            return false;
    SpellInfo const* tame = sSpellMgr->GetSpellInfo(SPELL_TAME_BEAST);
    if (!tame || bot->HasSpellCooldown(SPELL_TAME_BEAST))
        return false;
    // the nearest beast it may tame (CreatureTemplate::IsTameable), no higher than its level, idle and untapped
    Creature* beast = nullptr;
    float best = sPlayerbotAIConfig.SightDistance;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->GetLevel() > bot->GetLevel() ||
            creature->hasLootRecipient() || !creature->GetCreatureTemplate()->IsTameable(bot->CanTameExoticPets()))
            continue;
        if (NewRpgBaseAction::InDangerZone(bot, creature->GetPositionX(), creature->GetPositionY()))
            continue;
        {
            std::lock_guard<std::mutex> guard(tameSkipLock);
            auto const skip = tameSkip.find(bot->GetGUID().GetCounter());
            if (skip != tameSkip.end() && skip->second.first == bot->GetLevel() && skip->second.second.count(guid))
                continue;  // [tame14] its walk there never brought it in range and sight
        }
        float const d = bot->GetExactDist(creature);
        if (d < best)
        {
            best = d;
            beast = creature;
        }
    }
    if (!beast)
        return false;
    float const range = tame->GetMaxRange(false, bot);
    if (best > range || !bot->IsWithinLOSInMap(beast))
    {
        // [tame14] b354 Dandolar L10 (a hunter with no pet) in Darkshore: its walk to the q983 objective and the walk
        // to a Pygmy Tide Crawler in the surf took turns (every MoveFarTo begins with this step) - 10 min of DO_QUEST
        // without a line, 1180 yd walked in 20 min. A beast the bot has stopped short of (no walk on brings it in
        // range and sight) is skipped until its next level; a beast with no walk there is never walked to
        BotPath const path = FindBotPath(bot, beast->GetPositionX(), beast->GetPositionY(), beast->GetPositionZ());
        bool const reaches = !(path.type & PATHFIND_NOPATH) && path.points.size() >= 2 &&
                             beast->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z) <= range;
        ObjectGuid walkTo;
        {
            std::lock_guard<std::mutex> guard(tameSkipLock);
            walkTo = tameWalk[bot->GetGUID().GetCounter()];
        }
        bool const stoppedShort = !bot->isMoving() && walkTo == beast->GetGUID();
        if (!reaches || stoppedShort)
        {
            {
                std::lock_guard<std::mutex> guard(tameSkipLock);
                auto& skip = tameSkip[bot->GetGUID().GetCounter()];
                if (skip.first != bot->GetLevel())
                    skip = {bot->GetLevel(), {}};
                skip.second.insert(beast->GetGUID());
                tameWalk.erase(bot->GetGUID().GetCounter());
            }
            LOG_INFO("playerbots", "[tame14] {} L{} skips taming {} L{} {:.0f} yd off: {}", bot->GetName(),
                     bot->GetLevel(), beast->GetName(), beast->GetLevel(), best,
                     reaches ? "its walk stopped short of range and sight" : "no walk brings it in range");
            return false;
        }
        if (walkTo != beast->GetGUID())
            LOG_INFO("playerbots", "[tame14] {} L{} walks to tame {} L{} ({:.0f} yd)", bot->GetName(), bot->GetLevel(),
                     beast->GetName(), beast->GetLevel(), best);
        {
            std::lock_guard<std::mutex> guard(tameSkipLock);
            tameWalk[bot->GetGUID().GetCounter()] = beast->GetGUID();
        }
        return MoveTo(beast, range - 5.0f);
    }
    {
        std::lock_guard<std::mutex> guard(tameSkipLock);
        tameWalk.erase(bot->GetGUID().GetCounter());
    }
    bot->StopMoving();
    bot->SetFacingToObject(beast);
    LOG_INFO("playerbots", "[rpg] {} L{} tames {} L{} (entry {})", bot->GetName(), bot->GetLevel(), beast->GetName(),
             beast->GetLevel(), beast->GetEntry());
    return bot->CastSpell(beast, SPELL_TAME_BEAST, false) == SPELL_CAST_OK;
}

namespace
{
    // [trip52] (defined below) the trip by air; [fly97] walkRefused: no walk there, the flight at any length
    bool FlightTripSafe(Player* bot, WorldPosition const& dest, std::string& plan, bool walkRefused = false);
    // [fly164] one line per bot, flight master and level: a log cadence, no decision reads it
    void NoteFlightMasterGivenUp(Player* bot, WorldPosition const& masterPos)
    {
        static std::mutex lock;
        static std::map<std::tuple<uint32, int32, int32>, uint8> logged;
        std::tuple<uint32, int32, int32> const key{bot->GetGUID().GetCounter(), int32(masterPos.GetPositionX()),
                                                   int32(masterPos.GetPositionY())};
        {
            std::lock_guard<std::mutex> guard(lock);
            uint8& level = logged[key];
            if (level == bot->GetLevel())
                return;
            level = bot->GetLevel();
        }
        LOG_INFO("playerbots", "[fly164] {} L{} takes no flight from the flight master at ({:.0f},{:.0f}): its walk "
                 "there is given up at this level",
                 bot->GetName(), bot->GetLevel(), masterPos.GetPositionX(), masterPos.GetPositionY());
    }
    // [fly109] (defined below) a party member's own flight to the known node nearest the destination
    bool StartMemberFlight(Player* member, WorldPosition const& dest);
}

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    // [holds9] the refusal read after this call (LastMoveRefusal) is this call's: s_moveRefusal is thread-wide and
    // held the last MoveTo of any bot on the thread (b345: "IsWaitingForLastMove" / "IsDuplicateMove" HOLDs of walks
    // that never called MoveTo); every false below names its reason there
    s_moveRefusal.clear();
    if (dest == WorldPosition())
    {
        s_moveRefusal = "no destination";
        return false;
    }

    if (SoloSafetyStep())  // [guild13]
        return true;
    if (TramTrackStep())  // [tram25]
        return true;
    if (LakeBedStep())  // [nav257]
        return true;
    if (DestinationRefused(dest))  // [guild13]
        return RefusedWalk(dest, "its spot lies among mobs it does not pull");
    // [guild15] the next stretch of the walk passes an idle mob whose fight it wins only at full health and mana: it
    // rests first, as a player tops up before walking on through a camp (b307/b308: 26 of 47 deaths were bots
    // attacked on the way, half of them below full)
    if (SoloSafety::RestBeforeWalk(bot, dest))
        return true;

    // Don't start a ground move while the bot is on a taxi or in a load screen: the MovePoint issued below would
    // fight the active flight spline. Returning true consumes the tick instead of returning false, because the
    // callers of MoveFarTo fall through to MoveRandomNear, which is also a ground move.
    if (bot->IsInFlight() || bot->IsBeingTeleported())
        return true;

    // Aboard a boat or zeppelin: stay on until it docks where the trip gets off.
    if (Transport* transport = bot->GetTransport())
    {
        if (MotionTransport* motionTransport = transport->ToMotionTransport())
            return RideTransport(motionTransport);
        // [tram23] ... and a tram car of a travel line: it rides to the stop and gets off at its platform
        if (TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(transport->GetEntry()); line && line->isStatic)
            return RideTransport(transport);
    }

    // A mage in the group opens a portal to Dalaran from anywhere.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) && TravelRouteMgr::IsInDalaran(dest) &&
        !TravelRouteMgr::IsInDalaran(WorldPosition(bot)) && UseDalaranPortal())
        return true;

    // [taxi20] a flight, as a player takes one, when it is the faster way there and its fare is paid
    if (FlyIfFaster(dest))
        return true;


    if (TravelTrip(dest))
        return true;

    if (dest.GetMapId() != bot->GetMapId())
    {
        if (s_moveRefusal.empty())
            s_moveRefusal = "the destination lies on another map and no trip there starts";
        return false;
    }

    return WalkTo(dest);
}

void NewRpgBaseAction::GiveUpWalkTargets(WorldPosition const& dest, std::string const& why)
{
    // [grind25] the walk given up was a leg of a longer one - the walk to a trip's dock or portal, or a stretch of
    // the state's own target (its grind spot, its camp): that target is given up too, else the next choice takes it
    // again at once. b373 Enyssia L9 at (6064,557): "GRIND ... grind spot 3150 yd" -> its walk to the Auberdine
    // dock refused (MAZE / MOVE_FAR_LOOP) -> IDLE -> the same grind spot, 126 STUCK
    NewRpgInfo& info = botAI->rpgInfo;
    std::vector<WorldPosition> targets;
    if (info.trip.phase != NewRpgInfo::TripPhase::None || info.trip.dest != WorldPosition())
        targets.push_back(info.trip.dest);
    if (auto const* grind = std::get_if<NewRpgInfo::GoGrind>(&info.data))
        targets.push_back(grind->pos);
    if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&info.data))
        targets.push_back(camp->pos);
    for (WorldPosition const& target : targets)
        if (target != WorldPosition() && target != dest)
        {
            LOG_INFO("playerbots", "[grind25] {} L{} gives up ({:.0f},{:.0f}) map {} too: its walk's leg to ({:.0f},"
                     "{:.0f}) was given up ({})",
                     bot->GetName(), bot->GetLevel(), target.GetPositionX(), target.GetPositionY(), target.GetMapId(),
                     dest.GetPositionX(), dest.GetPositionY(), why);
            NoteWalkGivenUp(bot, target, why);
        }
    if (info.trip.phase != NewRpgInfo::TripPhase::Ride && info.trip.phase != NewRpgInfo::TripPhase::Leave)
        info.ClearTrip();
}

bool NewRpgBaseAction::LakeBedStep()
{
    // [nav257] a bot that stands under a lake's bed (the generator's polygons under the ground there, [nav256]) gets
    // out the way it came in: up into the water over it and on from the surface, whose polygons are the zone's main
    // region (b428 Peny L17, Lake Everstill (-9403,-2432,7): 36 yd under the bed, 5 h of "dead end of its route").
    // A straight swim up, no teleport: the guard takes the stretch through the ground as the way up ([swim28])
    if (bot->GetTransport() || bot->IsInFlight() || bot->IsBeingTeleported() || !bot->IsAlive())
        return false;
    float surface = 0.0f;
    if (!PlayerbotAI::UnderLakeBed(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), &surface))
        return false;
    if (bot->isMoving())
        return true;  // its swim up is under way
    LOG_INFO("playerbots", "[nav257] {} L{} under a lake's bed at ({:.1f},{:.1f},{:.1f}), terrain z {:.1f}: swims up "
             "to the water's surface z {:.1f}",
             bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
             bot->GetMap()->GetGridHeight(bot->GetPositionX(), bot->GetPositionY()), surface);
    // three points: a two-point move is pathed by the core (MovementAction::SwimStraight's rule)
    G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const to(from.x, from.y, surface);
    Movement::PointsArray path = {from, (from + to) * 0.5f, to};
    bot->GetMotionMaster()->Clear();
    bot->GetMotionMaster()->MoveSplinePath(&path);
    float const delay = std::clamp(float(IN_MILLISECONDS) * MoveDelay((to - from).length()), 0.0f,
                                   float(sPlayerbotAIConfig.MaxWaitForMove));
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), to.x, to.y, to.z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
    return true;
}

bool NewRpgBaseAction::TramTrackStep()
{
    SlowScope slow("tram track step", &bot->GetName());  // [perf26]
    // [tram25] coordinator 2026-11-09: a bot left standing on a tram line's track off the navmesh (an earlier
    // session walked it into the tunnel: b364-b373 Januel at (0,1551,-69) over the Deeprun pit) gets off it the way a
    // player does - the next car that comes under it carries it (the core's own test of the car's model under it,
    // Map::GetTransportForPos), it rides to the car's next stop and steps off at its platform (RideTransport).
    // Until a car comes it waits where it stands: no walk off the track, no Hearthstone
    if (bot->GetTransport() || bot->IsInFlight() || bot->IsBeingTeleported() || bot->IsInCombat())
        return false;
    TravelRouteMgr::Line const* track = nullptr;
    float trackGap = 0.0f;
    for (TravelRouteMgr::Line const& line : sTravelRouteMgr.GetLines())
    {
        if (!line.isStatic || line.docks.size() < 2 || line.docks.front().mapId != bot->GetMapId())
            continue;
        // the car's way between its stops, as the straight line their spots span (the tram's tunnel)
        TravelRouteMgr::Dock const& a = line.docks.front();
        TravelRouteMgr::Dock const& b = line.docks.back();
        float const abx = b.x - a.x, aby = b.y - a.y;
        float const t = std::clamp(((bot->GetPositionX() - a.x) * abx + (bot->GetPositionY() - a.y) * aby) /
                                       std::max(abx * abx + aby * aby, 1.0f),
                                   0.0f, 1.0f);
        float const gap = std::hypot(a.x + abx * t - bot->GetPositionX(), a.y + aby * t - bot->GetPositionY());
        if (!track || gap < trackGap)
        {
            track = &line;
            trackGap = gap;
        }
    }
    constexpr float TRACK_HALF_WIDTH = 10.0f;  // the car's half width and the track beside it
    if (!track || trackGap > TRACK_HALF_WIDTH)
        return false;
    // on the navmesh (a platform): the walk goes on as planned
    if (dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
    {
        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        float const point[3] = {bot->GetPositionY(), bot->GetPositionZ(), bot->GetPositionX()};
        float const extents[3] = {3.0f, 5.0f, 3.0f};
        float nearest[3] = {0.0f, 0.0f, 0.0f};
        dtPolyRef ref = 0;
        if (dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref)
            return false;
    }
    if (Transport* car = bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(),
                                                           bot->GetPositionY(), bot->GetPositionZ(), bot))
        if (TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(car->GetEntry()); line && line->isStatic)
        {
            car->AddPassenger(bot, true);
            bot->StopMovingOnCurrentPos();
            botAI->rpgInfo.ClearTrip();
            botAI->rpgInfo.SetTripPhase(NewRpgInfo::TripPhase::Ride);
            LOG_INFO("playerbots", "[tram25] {} L{} off the navmesh on the track at ({:.1f},{:.1f},{:.1f}): boards "
                     "car {} as it passes under it",
                     bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                     car->GetEntry());
            return true;
        }
    if (botAI->rpgInfo.holdNote != "tram25")
    {
        botAI->rpgInfo.holdNote = "tram25";
        LOG_INFO("playerbots", "[tram25] {} L{} off the navmesh on the track at ({:.1f},{:.1f},{:.1f}), {:.0f} yd off "
                 "the line of car {}: waits for a car to pass under it",
                 bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                 trackGap, track->entry);
    }
    return HoldPosition(IN_MILLISECONDS);
}

bool NewRpgBaseAction::RefusedWalk(WorldPosition const& dest, std::string const& why)
{
    NewRpgStatus const status = botAI->rpgInfo.GetStatus();
    GiveUpWalkTargets(dest, why);  // [grind25]
    // [holds9] b345 Ellosera: "MAZE no way to (-2050,-12000)" -> IDLE -> the same grind spot chosen again (its
    // TripRefused look had judged it safe at this level): the refused spot is out of the choices until the next level
    NoteWalkGivenUp(bot, dest, why);
    // a quest whose spot (its objective or its ender: b302 Kianey's q45 hand-in at Rolf's corpse among seven
    // Murlocs, re-chosen every look) lies there is postponed: the planner takes the next one
    if (auto* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data); quest && !botAI->rpgInfo.meetWalk)
    {
        if (botAI->lowPriorityQuest.insert(quest->questId).second)
        {
            LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} reason: {}", bot->GetName(), bot->GetLevel(),
                     quest->questId, why);
            NotePostpone(bot, quest->questId, why);
            // [wall17] walled by mobs it never fights ([guild12] orange / red, elites): the party's quest
            if (std::string const con = SoloSafety::ConWalls(bot); !con.empty())
                LOG_INFO("playerbots", "[wall17] {} L{} q{} walled by {}: asks for help", bot->GetName(),
                         bot->GetLevel(), quest->questId, con);
            // [coop] a camp it cannot solo: it looks for a party for it (NewRpgCoop.h); any spot of the quest
            // (its objective spot or a sweep spot of it; a hand-in spot ends in a [coop] no-seek line)
            NewRpgCoop::OnSpotRefused(botAI, bot, quest->questId, quest->objectiveIdx, dest);
        }
    }
    // [guild15] a gather node there: the refusal is a fact, the next node is taken at once in the same gather run
    // (b333 Peny: every refused node cost a whole IDLE -> GATHER round, LOOP alerts); a later run's look at it is
    // answered from DestinationRefused's memory until the danger changes
    if (auto* gather = std::get_if<NewRpgInfo::DoGather>(&botAI->rpgInfo.data); gather && gather->nodeSpawnId)
    {
        LOG_INFO("playerbots", "[guild20] {} L{} TARGET_SWITCH gather node {}: its walk is refused, the next node",
                 bot->GetName(), bot->GetLevel(), gather->nodeSpawnId);
        gather->visited.insert(gather->nodeSpawnId);
        gather->nodeSpawnId = 0;
        gather->lastReach = 0;
        return true;
    }
    // [help12] a walk that is no quest spot, refused for mobs it does not pull (the maze search, a spot among them):
    // the camp is its help request to the global leader (NewRpgCoop::OnCampRefused; a quest spot asks above)
    if (status != RPG_DO_QUEST && SoloSafety::Applies(bot) && !botAI->rpgInfo.meetWalk &&  // [wait19]

        (why.find("mobs it does not pull") != std::string::npos || why.find("among mobs") != std::string::npos))
        NewRpgCoop::OnCampRefused(botAI, bot, dest, why);
    // [guild14] a gather node / profession npc there: the state ends, its action marks the target failed and
    // takes the next one (b303 Gilduck, Gnetlibozz: DO_GATHER failed on a refused node every tick, STUCK)
    if (status == RPG_DO_QUEST || status == RPG_GO_CAMP || status == RPG_GO_GRIND || status == RPG_DO_GATHER ||
        status == RPG_DO_PROFESSION)
    {
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

namespace
{
    // [deaths] a solo far walk's maze search: per bot, the destination it is for, the via spot it walks to first and
    // the via spots tried for it (none is tried twice for the same destination)
    struct WalkAroundState
    {
        std::tuple<uint32, int32, int32> dest{0, 0, 0};
        bool hasVia = false;
        G3D::Vector3 via;
        std::vector<G3D::Vector3> tried;
    };
    std::mutex walkAroundLock;
    std::unordered_map<uint32, WalkAroundState> walkAround;

    std::tuple<uint32, int32, int32> DestKey(WorldPosition const& dest)
    {
        return {dest.GetMapId(), int32(dest.GetPositionX()), int32(dest.GetPositionY())};
    }

    // [perf214] coordinator 2026-11-11: WalkOffIsland's landing look (does a walk on to the destination start at
    // this landing: a route search each, up to PATH_TRIES) paced over ticks while the bot stands: the verdicts found
    // are kept for the spot and destination, a call adds at least one and stops at its search budget; the next call
    // takes up the look where it stopped - the same landings, the same verdicts, the same choice. b438 Ryarii L16:
    // ~20 searches of 50-70 ms from the landings round (1442,-532) to (4520,410) in one 1139 ms update
    struct IslandLook
    {
        std::tuple<uint32, int32, int32, int32> spot{0, 0, 0, 0};  // map, x, y, z (its health may change meanwhile)
        std::tuple<uint32, int32, int32> dest{0, 0, 0};
        std::vector<std::pair<G3D::Vector3, bool>> verdicts;  // landing -> leads on
        bool pending = false;                                 // a look stopped at its budget, taken up next call
    };
    std::mutex islandLookLock;
    std::unordered_map<uint32, IslandLook> islandLooks;
    constexpr uint64 ISLAND_LOOK_SEARCHES_PER_TICK = 2;  // compute pacing per call, not a behaviour rule

    // a landing look stopped for `dest` at the spot the bot stands on (its stand there is the look's, no stuck sign)
    bool IslandLookPending(Player* bot, WorldPosition const& dest)
    {
        std::lock_guard<std::mutex> guard(islandLookLock);
        auto const found = islandLooks.find(bot->GetGUID().GetCounter());
        if (found == islandLooks.end() || !found->second.pending || found->second.dest != DestKey(dest))
            return false;
        auto const& spot = found->second.spot;
        return std::get<0>(spot) == bot->GetMapId() && std::get<1>(spot) == int32(bot->GetPositionX()) &&
               std::get<2>(spot) == int32(bot->GetPositionY()) && std::get<3>(spot) == int32(bot->GetPositionZ());
    }

    // [deaths] user 2026-11-08: a long walk in stages - the travel node graph (TravelNodeMap, loaded at startup and
    // read only after) by its walk links on the bot's map, from the node nearest the bot to the node nearest the
    // destination, the shortest by link length; the nodes' spots in walk order (empty: no such chain)
    std::vector<G3D::Vector3> NodeStages(Player* bot, WorldPosition const& dest)
    {
        std::vector<G3D::Vector3> stages;
        std::vector<TravelNode*> const nodes = sTravelNodeMap.getNodes(WorldPosition(bot));
        if (nodes.size() < 2)
            return stages;
        TravelNode* const start = nodes.front();
        TravelNode* goal = nullptr;
        float goalGap = std::numeric_limits<float>::max();
        for (TravelNode* node : nodes)
            if (float const gap = node->fDist(dest); gap < goalGap)
            {
                goalGap = gap;
                goal = node;
            }
        if (!goal || goal == start)
            return stages;
        uint32 const mapId = bot->GetMapId();
        std::unordered_map<TravelNode*, float> cost{{start, 0.0f}};
        std::unordered_map<TravelNode*, TravelNode*> previous;
        using Entry = std::pair<float, TravelNode*>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        open.emplace(0.0f, start);
        while (!open.empty())
        {
            auto const [reached, node] = open.top();
            open.pop();
            if (node == goal)
                break;
            if (reached > cost[node])
                continue;
            for (auto const& [next, link] : *node->getLinks())
            {
                if (!next || !link || link->getPathType() != TravelNodePathType::walk || next->getMapId() != mapId)
                    continue;
                float const through = reached + std::max(link->getDistance(), 0.0f);
                auto const known = cost.find(next);
                if (known != cost.end() && known->second <= through)
                    continue;
                cost[next] = through;
                previous[next] = node;
                open.emplace(through, next);
            }
        }
        if (!previous.count(goal))
            return stages;
        for (TravelNode* node = goal; node && node != start; node = previous[node])
        {
            WorldPosition const* at = node->getPosition();
            stages.emplace_back(at->GetPositionX(), at->GetPositionY(), at->GetPositionZ());
        }
        std::reverse(stages.begin(), stages.end());
        return stages;
    }
}

// [runpast11] the leg walked: `points` from the bot on as far as `end` (the leg's end in sight), committed with the
// route search's run-past set for the executor (SoloSafety::RunPastStep)
static void CommitLeg(Player* bot, Movement::PointsArray const& points, G3D::Vector3 const& end)
{
    std::vector<G3D::Vector3> leg;
    for (G3D::Vector3 const& at : points)
    {
        leg.push_back(at);
        if ((at - end).length() < 0.5f)
            break;
        if (leg.size() >= 2 && (end - leg[leg.size() - 2]).length() <= (at - leg[leg.size() - 2]).length() &&
            (end - at).length() <= (at - leg[leg.size() - 2]).length())
        {
            leg.back() = end;  // `end` lies on this stretch
            break;
        }
    }
    SoloSafety::CommitRunPast(bot, leg, std::vector<G3D::Vector3>(points.begin(), points.end()));  // [runpast15]
}

bool NewRpgBaseAction::WalkAround(WorldPosition const& dest, BotPath const& path, float disToDest, bool deadEnd)
{
    float const sight = sPlayerbotAIConfig.SightDistance;
    float const searchReach = 2.0f * sight;  // a far walk's leg is searched two sights ahead (the farRoute legs)
    // [pull9] the walls of the walk's own search (the via searches below search again)
    std::string const walls = path.throughRefused ? SoloSafety::RouteWalls(bot) : std::string();
    G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const goal(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    uint32 const low = bot->GetGUID().GetCounter();
    auto const tried = [&](G3D::Vector3 const& via)
    {
        std::lock_guard<std::mutex> guard(walkAroundLock);
        WalkAroundState& state = walkAround[low];
        if (state.dest != DestKey(dest))
        {
            state = WalkAroundState();
            state.dest = DestKey(dest);
        }
        for (G3D::Vector3 const& spot : state.tried)
            if ((spot - via).length() <= MOVE_FAR_PROGRESS)
                return true;
        state.tried.push_back(via);
        return false;
    };
    // the walk to a via spot, as far as the bot sees; the via kept for the next legs
    auto const walkVia = [&](G3D::Vector3 const& via, BotPath const& leg, char const* how)
    {
        if (leg.points.size() < 2)
            return false;
        G3D::Vector3 const end = leg.length > sight ? Leader::Travel::PointOnPath(leg.points, sight) : leg.actualEnd;
        if (!MoveTo(bot->GetMapId(), end.x, end.y, end.z, false, false, false, true))
            return false;
        CommitLeg(bot, leg.points, end);  // [runpast11]
        {
            std::lock_guard<std::mutex> guard(walkAroundLock);
            WalkAroundState& state = walkAround[low];
            state.dest = DestKey(dest);
            state.hasVia = true;
            state.via = via;
        }
        LOG_INFO("playerbots", "[solo] {} L{} MAZE {} to ({:.0f},{:.0f}) via ({:.0f},{:.0f}), {:.0f} yd to it",
                 bot->GetName(), bot->GetLevel(), how, goal.x, goal.y, via.x, via.y, leg.length);
        return true;
    };
    bool const longWalk = !deadEnd && !path.throughRefused && path.type == PATHFIND_INCOMPLETE;
    // 1. a long walk in stages: the farthest node of the chain the search reaches, nearer the destination
    if (longWalk)
    {
        G3D::Vector3 const* stage = nullptr;
        std::vector<G3D::Vector3> const stages = NodeStages(bot, dest);
        for (G3D::Vector3 const& at : stages)
            if (std::hypot(at.x - here.x, at.y - here.y) <= searchReach &&
                (at - goal).length() + MOVE_FAR_PROGRESS < disToDest)
                stage = &at;
        if (stage && !tried(*stage))
        {
            BotPath const leg = FindBotPath(bot, stage->x, stage->y, stage->z);
            if (leg.type == PATHFIND_NORMAL && !leg.throughRefused && walkVia(*stage, leg, "stage"))
                return true;
        }
    }
    if (longWalk)
        return false;  // the walk goes on as the search gave it (partial, no refused reach on it)
    // 2. another way round: a travel node within the search's reach whose walk and whose walk on to the destination
    // pass no refused reach, the shortest way through it first
    {
        struct Via
        {
            G3D::Vector3 at;
            float yards;
        };
        std::vector<Via> vias;
        for (TravelNode* node : sTravelNodeMap.getNodes(WorldPosition(bot), searchReach))
        {
            WorldPosition const* at = node->getPosition();
            G3D::Vector3 const spot(at->GetPositionX(), at->GetPositionY(), at->GetPositionZ());
            if ((spot - here).length() <= MOVE_FAR_PROGRESS)
                continue;
            vias.push_back({spot, (spot - here).length() + (goal - spot).length()});
        }
        std::sort(vias.begin(), vias.end(), [](Via const& a, Via const& b) { return a.yards < b.yards; });
        constexpr size_t MAZE_VIA_TRIES = 4;  // search cost bound (route searches per look), not a behaviour rule
        size_t searched = 0;
        for (Via const& via : vias)
        {
            if (searched >= MAZE_VIA_TRIES)
                break;
            if (tried(via.at))
                continue;
            ++searched;
            BotPath const leg = FindBotPath(bot, via.at.x, via.at.y, via.at.z);
            if (leg.type != PATHFIND_NORMAL || leg.throughRefused)
                continue;
            BotPath const on = FindBotPath(bot, via.at.x, via.at.y, via.at.z, goal.x, goal.y, goal.z);
            bool const onward = on.type == PATHFIND_NORMAL ||
                                (on.type == PATHFIND_INCOMPLETE && (on.actualEnd - goal).length() + MOVE_FAR_PROGRESS <
                                                                       (via.at - goal).length());
            if (!onward || on.throughRefused)
                continue;
            if (walkVia(via.at, leg, "another way"))
                return true;
        }
    }
    // 3. a refused patrol on the way: its window (the forecast's ETA)
    if (path.throughRefused)
    {
        float waitSec = 0.0f;
        std::string who;
        if (SoloSafety::PatrolWait(bot, path.points, waitSec, who))
        {
            if (waitSec > 0.0f)
            {
                LOG_INFO("playerbots", "[solo] {} L{} MAZE waits {:.0f} s for {} to pass its walk to ({:.0f},{:.0f})",
                         bot->GetName(), bot->GetLevel(), waitSec, who, goal.x, goal.y);
                if (ForceToWait(uint32(waitSec * float(IN_MILLISECONDS)), MovementPriority::MOVEMENT_NORMAL))
                    return true;
                s_moveRefusal = Acore::StringFormat("its maze wait of {:.0f} s for {} did not start", waitSec, who);
                return false;  // [pull9] the reason read by MoveFarTo's caller
            }
            if (path.points.size() >= 2)
            {
                G3D::Vector3 const end =
                    path.length > sight ? Leader::Travel::PointOnPath(path.points, sight) : path.actualEnd;
                LOG_INFO("playerbots", "[solo] {} L{} MAZE walks now: {} clear of its walk to ({:.0f},{:.0f})",
                         bot->GetName(), bot->GetLevel(), who, goal.x, goal.y);
                if (MoveTo(bot->GetMapId(), end.x, end.y, end.z, false, false, false, true))
                    return true;
            }
        }
    }
    // 4. every way blocked: another task (the quest postponed with the co-op seek for help, the next gather node)
    std::string const why = path.throughRefused ? "every way there passes mobs it does not pull (maze search)"
                                                : "its walk there ends in a dead end (maze search)";
    LOG_INFO("playerbots", "[solo] {} L{} MAZE no way to ({:.0f},{:.0f}): {}{}{}", bot->GetName(), bot->GetLevel(),
             goal.x, goal.y, why, walls.empty() ? "" : "; walls: ", walls);
    // [nav165] a dead end (no mob walls it): the deep search out of where it stands, once per pocket and level
    if (!path.throughRefused)
        RequestPocketRescue(dest);
    // [fly97] a far destination with no walk there (a dead end, every way through mobs it must not fight): the flight
    // from its nearest flight master to the known node nearest it, as a player flies past the zone it cannot cross
    // (b414 Thokhen L18 / Feette L19 in Ashenvale: 36 "dead end (maze search)" postpones, every quest walk to
    // Astranaar refused, the bots stood). The walks to the flight master and off the landing are its own
    if (bot->GetExactDist(dest) > sPlayerbotAIConfig.SightDistance)
    {
        std::string plan;
        // [fly109] a co-op / help party's leader (party75 sends its walled walk here): the party flies together -
        // each member's own trip by air judged as the leader's (its flight master and landing walks refuse nothing,
        // the nodes known to it, its fare in its own purse), else nobody flies
        std::vector<Player*> members;
        std::string memberWhy;
        bool const partyLeader = !SoloSafety::Applies(bot) && NewRpgCoop::JudgesPulls(bot) && bot->GetGroup();
        if (partyLeader)
            for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref && memberWhy.empty(); ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot)
                    continue;
                std::string memberPlan;
                if (!GET_PLAYERBOT_AI(member) || !member->IsAlive() || member->GetMap() != bot->GetMap())
                    memberWhy = member->GetName() + " is no bot alive on its map";
                else if (!FlightTripSafe(member, dest, memberPlan, true))
                    memberWhy = member->GetName() + " has no safe trip by air there (its nodes, fare or walks)";
                else
                    members.push_back(member);
            }
        if (memberWhy.empty() && FlightTripSafe(bot, dest, plan, true) && FlyIfFaster(dest, true))
        {
            std::string flown;
            for (Player* member : members)
                if (StartMemberFlight(member, dest))
                    flown += (flown.empty() ? "" : ", ") + member->GetName();
            std::string const party =
                partyLeader ? "; [fly109] its party flies with it: " + (flown.empty() ? std::string("none") : flown)
                            : std::string();
            LOG_INFO("playerbots", "[fly97] {} L{} flies to ({:.0f},{:.0f}): {}; {}{}", bot->GetName(), bot->GetLevel(),
                     dest.GetPositionX(), dest.GetPositionY(), why, plan, party);
            return true;
        }
        if (!memberWhy.empty())
            LOG_INFO("playerbots", "[fly109] {} L{} party does not fly to ({:.0f},{:.0f}): {}", bot->GetName(),
                     bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY(), memberWhy);
    }
    RefusedWalk(dest, why);
    // [runpast11] the walls stand round the bot, not the target: its walk out (to its bind point, else its hub) is
    // walled too - its Hearthstone out (b349 Hobliir L12 among the Blacksilt murlocs, Lyleria L9 among the Ashenvale
    // bears, Enyssia L5 in the Gnarlpine camp: 921 s, 1136 s, 325 s STUCK, every quest postponed in turn)
    if (path.throughRefused)
    {
        std::string walledWhy;
        if (WalledIn(dest, walledWhy))
        {
            NoteWalledHold(dest, walledWhy);  // [hold58]
            // [help14] its help request is the way out while the global leader serves it or has not judged it yet
            // (b354 Sycina L11: help asked for the q1580 camp, then hearthed 3814 yd to Darnassus at once; its helper
            // came to an empty spot and it waited 1074 s for him there); a refused request leaves the Hearthstone
            if (NewRpgCoop::HelpAsked(bot))
                LOG_INFO("playerbots", "[help14] {} L{} stays for its help request ({}): no hearth", bot->GetName(),
                         bot->GetLevel(), walledWhy);
            else
                HearthOut(walledWhy, &dest);
        }
    }
    return true;
}

bool NewRpgBaseAction::WalledIn(WorldPosition const& refused, std::string& why)
{
    if (!SoloSafety::Applies(bot) || bot->IsInCombat())
        return false;
    float const sight = sPlayerbotAIConfig.SightDistance;
    uint32 const landmass = TravelRouteMgr::Landmass(WorldPosition(bot));
    // a spot beyond its sight on its own landmass, not the refused target itself
    auto const usable = [&](WorldPosition const& at)
    {
        return at != WorldPosition() && at.GetMapId() == bot->GetMapId() && bot->GetExactDist(at) > sight &&
               refused.GetExactDist(at) > sight && TravelRouteMgr::Landmass(at) == landmass;
    };
    WorldPosition const bind(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ);
    WorldPosition out;
    char const* what = "";
    if (usable(bind))
    {
        out = bind;
        what = "bind point";
    }
    else if (WorldPosition const hub = NearestHub(bot); usable(hub))
    {
        out = hub;
        what = "hub";
    }
    if (out == WorldPosition())
        return false;  // no walk out to judge: no fact that it is walled in
    BotPath const walk = FindBotPath(bot, out.GetPositionX(), out.GetPositionY(), out.GetPositionZ());
    G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    // a walk out: complete round every wall, or partial (the search's length) but leading on past the walls it sees
    // (RouteCosts prices the mobs within its sight and their reach)
    bool const walksOut = walk.points.size() >= 2 && !walk.throughRefused &&
                          (walk.type == PATHFIND_NORMAL ||
                           (walk.type == PATHFIND_INCOMPLETE && (walk.actualEnd - here).length() > 2.0f * sight));
    if (walksOut)
        return false;
    why = Acore::StringFormat("walled in: its walk to ({:.0f},{:.0f}) and the walk out to its {} ({:.0f},{:.0f}) {:.0f} "
                              "yd both pass mobs it does not pull{}",
                              refused.GetPositionX(), refused.GetPositionY(), what, out.GetPositionX(),
                              out.GetPositionY(), bot->GetExactDist(out),
                              walk.throughRefused ? "" : " (no walk out found)");
    return true;
}

bool NewRpgBaseAction::WalkTo(WorldPosition const& dest)
{
    // Random bots keep their old way of travel (walk, teleport when stuck); routing is for player-owned bots.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) &&
        TravelRouteMgr::IsInDalaran(dest) != TravelRouteMgr::IsInDalaran(WorldPosition(bot)))
        return DalaranStep(dest);

    return MoveFarToDirect(dest);
}

bool NewRpgBaseAction::HoldPosition(uint32 duration)
{
    if (bot->isMoving())
        bot->StopMoving();

    return ForceToWait(duration);
}

// Straight line, no path finding: decks and gangways are not on the navmesh. On a transport the spline is
// computed in its local frame, so walking off a docked boat works the same way.
void NewRpgBaseAction::MovePointDirect(float x, float y, float z)
{
    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm)
        return;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    mm->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
    float delay = IN_MILLISECONDS * MoveDelay(bot->GetExactDist(x, y, z));
    delay = std::clamp(delay, 0.0f, static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
}

// No living member of the group is left behind on this map.
bool NewRpgBaseAction::IsGroupGathered(float distance)
{
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMapId() != bot->GetMapId())
            continue;

        if (bot->GetDistance(member) > distance)
            return false;
    }

    return true;
}

std::string NewRpgBaseAction::ComingStraggler(float distance)
{
    Group* group = bot->GetGroup();
    if (!group)
        return "";

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMapId() != bot->GetMapId() || bot->GetDistance(member) <= distance)
            continue;

        BotPath const path = FindBotPath(member, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (!(path.type & PATHFIND_NOPATH) &&
            bot->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z) <= distance)
            return member->GetName();
    }

    return "";
}

uint32 NewRpgBaseAction::BoardWalkMs(Transport* transport)
{
    if (!transport)
        return 0;

    auto runMs = [transport](Player* player)
    {
        float const speed = player->GetSpeed(MOVE_RUN);
        return uint32(player->GetExactDist(transport) / (speed > 0.0f ? speed : 1.0f) * IN_MILLISECONDS);
    };
    uint32 slowest = runMs(bot);
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId() || bot->GetDistance(member) > GROUP_GATHER_DISTANCE)
                continue;
            slowest = std::max(slowest, runMs(member));
        }

    return slowest;
}

std::string NewRpgBaseAction::StragglerInTime(Transport* transport, uint32 windowMs)
{
    Group* group = bot->GetGroup();
    if (!group || !transport)
        return "";

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMapId() != bot->GetMapId() || bot->GetDistance(member) <= GROUP_GATHER_DISTANCE)
            continue;

        float const speed = member->GetSpeed(MOVE_RUN);
        uint32 const runMs =
            uint32(member->GetExactDist(transport) / (speed > 0.0f ? speed : 1.0f) * IN_MILLISECONDS);
        if (runMs < windowMs)
            return member->GetName();
    }

    return "";
}

namespace
{
    // [tram23] the bot rides a tram car of a travel line (no walk off it between the stops)
    bool RidesTramLine(Player* bot)
    {
        Transport* transport = bot ? bot->GetTransport() : nullptr;
        TravelRouteMgr::Line const* line = transport ? sTravelRouteMgr.GetLine(transport->GetEntry()) : nullptr;
        return line && line->isStatic;
    }
}

void NewRpgBaseAction::NavGap(Player* bot, std::string const& event)
{
    if (!bot || !bot->IsInWorld())
        return;
    float const x = bot->GetPositionX(), y = bot->GetPositionY(), z = bot->GetPositionZ();
    {
        // one alarm per bot and spot (a log line, no decision)
        static std::mutex navGapLock;
        static std::unordered_map<uint32, std::tuple<uint32, int32, int32, int32>> last;
        std::tuple<uint32, int32, int32, int32> const key{bot->GetMapId(), int32(x), int32(y), int32(z)};
        std::lock_guard<std::mutex> guard(navGapLock);
        auto& seen = last[bot->GetGUID().GetCounter()];
        if (seen == key)
            return;
        seen = key;
    }
    std::string cause;
    auto const add = [&cause](std::string const& part) { cause += (cause.empty() ? "" : "; ") + part; };
    if (Transport* transport = bot->GetTransport())
        add(Acore::StringFormat("on transport {} ({})", transport->GetEntry(),
                                transport->IsMotionTransport() ? "boat / zeppelin" : "tram car / elevator"));
    else if (GameObject* car = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_TRANSPORT, 30.0f))
        add(Acore::StringFormat("by moving transport {} {:.0f} yd off, not its passenger (walked onto it or left by it)",
                                car->GetEntry(), bot->GetExactDist(car)));
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot); ai && ai->rpgInfo.trip.phase != NewRpgInfo::TripPhase::None)
        add(Acore::StringFormat("trip phase {}", uint32(ai->rpgInfo.trip.phase)));
    float const ground = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, z + 2.0f, true, 100.0f);
    // [navgap29] under a liquid's surface the bot swims: no fall, the navmesh's water is its surface polygons only.
    // b378 Alnia x3, Hobliir, Shalaeth, Salleas in the sea off Auberdine at z -5..-15, every alarm "in the air 8 yd
    // over the ground: a fall not run, a knockback" ([swim28] takes the way up)
    LiquidData const liquid =
        bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight(), {});
    bool const underLiquid = liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT && liquid.Level >= z;
    if (underLiquid)
        add(Acore::StringFormat("[navgap29] under water {:.0f} yd below its surface (z {:.1f}), {:.0f} yd over the "
                                "floor: swims (the navmesh holds only the surface)",
                                liquid.Level - z, liquid.Level, ground > INVALID_HEIGHT ? z - ground : 0.0f));
    else if (ground > INVALID_HEIGHT && z - ground > 6.0f)
        add(Acore::StringFormat("in the air {:.0f} yd over the ground (z {:.1f}): a fall not run, a knockback, a "
                                "transport gone from under it",
                                z - ground, ground));
    if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
        add("falling");
    if (dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
    {
        dtQueryFilter steep;
        steep.setIncludeFlags(uint16(NAV_GROUND_STEEP));
        float const point[3] = {y, z, x};
        float const extents[3] = {3.0f, 5.0f, 3.0f};
        float nearest[3] = {0.0f, 0.0f, 0.0f};
        dtPolyRef ref = 0;
        if (dtStatusSucceed(query->findNearestPoly(point, extents, &steep, &ref, nearest)) && ref)
            add("on a steep polygon (the route search's steep fallback)");
    }
    else
        add("no navmesh loaded for the map");
    if (cause.empty())
        add("none of the above: a navmesh hole or an island in the tile (repair)");
    LOG_ERROR("playerbots", "[navgap] {} L{} map {} ({:.1f},{:.1f},{:.1f}) zone {} {}; cause: {}", bot->GetName(),
              bot->GetLevel(), bot->GetMapId(), x, y, z, bot->GetZoneId(), event, cause);
}

Transport* NewRpgBaseAction::FindTransport(uint32 entry)
{
    for (Transport* transport : bot->GetMap()->GetAllTransports())
    {
        if (!transport || transport->GetEntry() != entry || !transport->IsInWorld())
            continue;

        if (MotionTransport* motionTransport = transport->ToMotionTransport())
        {
            sTravelRouteMgr.Observe(motionTransport);
            return motionTransport;
        }
    }
    // [tram23] a grid-spawned car (the Deeprun Tram): the nearest of its entry the map shows the bot
    // [tram28] ... found by its spawn, not by a grid search: the car stays filed in its spawn's grid cell while it
    // runs (StaticTransport::UpdatePosition relocates it without a grid move), so a car spawned at the far platform
    // was never found at this one - never "docked", TRIP_ABANDON after every loop (b377: 8 bots, 2.7 of the 4.0
    // TRAVEL hours at the Deeprun Tram; 176081 / 176083 spawn at the other end of their bots' platform)
    if (TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(entry); line && line->isStatic && line->spawnId)
    {
        auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(line->spawnId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (StaticTransport* car = itr->second ? itr->second->ToStaticTransport() : nullptr;
                car && car->IsInWorld() && car->GetEntry() == entry)
            {
                sTravelRouteMgr.Observe(car);
                return car;
            }
    }
    if (TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(entry); line && line->isStatic)
        if (GameObject* go = bot->FindNearestGameObject(entry, bot->GetMap()->GetVisibilityRange()))
            if (StaticTransport* car = go->ToStaticTransport(); car && car->IsInWorld())
            {
                sTravelRouteMgr.Observe(car);
                return car;
            }

    return nullptr;
}

// Plans the boat, zeppelin or portal leg of a far move and runs it up to boarding. False hands the move back to
// plain walking (no leg needed, or the leg was given up).
bool NewRpgBaseAction::TravelTrip(WorldPosition const& dest)
{
    using Phase = NewRpgInfo::TripPhase;
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgInfo::Trip& trip = info.trip;
    // [guild15] random bots without the teleport shortcut (the guild's group B) ride and step through like players
    if (sRandomPlayerbotMgr.IsRandomBot(bot) && sPlayerbotAIConfig.RandomBotTeleport)
        return false;

    // Off the transport after the ride: the leg is done, the rest is planned from the pier next tick.
    // [guild15] ... once the step off to the pier is walked (the Leave phase keeps the boat from boarding it again)
    if (trip.phase == Phase::Leave && bot->isMoving())
        return true;
    if (trip.phase == Phase::Ride || trip.phase == Phase::Leave)
    {
        info.ClearTrip();
        return HoldPosition(IN_MILLISECONDS);
    }

    // A new destination drops a leg that has not boarded yet.
    if (trip.phase != Phase::None && trip.dest != dest)
        info.ClearTrip();

    if (trip.phase == Phase::None)
    {
        bool const otherMap = dest.GetMapId() != bot->GetMapId();
        // [guild15] ... unless the destination lies on another landmass (the tree top and Rut'theran below)
        if (!otherMap && bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY()) < TRIP_PLAN_DISTANCE &&
            TravelRouteMgr::Landmass(WorldPosition(bot)) == TravelRouteMgr::Landmass(dest))
            return false;

        if (trip.dest == dest && trip.plannedAt && GetMSTimeDiffToNow(trip.plannedAt) < TRIP_REPLAN_MS)
            return false;

        trip.dest = dest;
        trip.plannedAt = getMSTime();
        TravelRouteMgr::Leg leg;
        float seconds = 0.0f;
        if (!sTravelRouteMgr.FindRoute(bot, dest, leg, seconds) || leg.kind == TravelRouteMgr::LegKind::Walk)
            return false;

        trip.leg = leg;
        info.SetTripPhase(Phase::ToStart);
        LOG_DEBUG("playerbots", "[New RPG] {} travels by {} {} to map {} ({:.0f}s)", bot->GetName(),
                  leg.kind == TravelRouteMgr::LegKind::Portal ? "portal" : "transport",
                  leg.kind == TravelRouteMgr::LegKind::Portal ? leg.portalIndex : leg.lineEntry, dest.GetMapId(),
                  seconds);
    }

    if (trip.leg.kind == TravelRouteMgr::LegKind::Portal)
        return TravelByPortal(trip);

    return TravelByTransport(trip);
}

bool NewRpgBaseAction::TravelByTransport(NewRpgInfo::Trip& trip)
{
    using Phase = NewRpgInfo::TripPhase;
    NewRpgInfo& info = botAI->rpgInfo;
    TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(trip.leg.lineEntry);
    if (!line || trip.leg.fromDock >= line->docks.size() || trip.leg.toDock >= line->docks.size())
    {
        info.ClearTrip();
        return false;
    }

    TravelRouteMgr::Dock const& dock = line->docks[trip.leg.fromDock];
    if (bot->GetMapId() != dock.mapId)
    {
        info.ClearTrip();
        return false;
    }

    Transport* transport = FindTransport(line->entry);
    uint32 remaining = 0;
    bool const docked = TravelRouteMgr::IsDocked(transport, dock, remaining);

    switch (trip.phase)
    {
        case Phase::ToStart:
            if (bot->GetExactDist(dock.pier) > PIER_REACHED)
                return WalkTo(dock.pier);

            info.SetTripPhase(Phase::Wait);
            [[fallthrough]];
        case Phase::Wait:
        {
            // [timers] the stop is taken when the time left at the dock covers the run of the bot and its gathered
            // members to the deck (the transport's schedule, a game mechanic; no fixed boarding margin)
            if (docked && transport && remaining >= BoardWalkMs(transport))
            {
                info.SetTripPhase(Phase::Board);
                return HoldPosition(IN_MILLISECONDS);
            }

            // [timers] one full loop of its schedule (the transport's own period, a game mechanic; no spare time on
            // top) passed without it docking here: it does not come (a transport off this map is on the other map
            // of its route, no proof)
            if (GetMSTimeDiffToNow(trip.phaseStart) > line->periodMs)
            {
                LOG_INFO("playerbots", "[rpg] {} L{} TRIP_ABANDON transport {} reason: a full loop ({} ms) passed "
                         "without it docking here",
                         bot->GetName(), bot->GetLevel(), line->entry, line->periodMs);
                WorldPosition const dest = trip.dest;
                info.ClearTrip();
                info.trip.dest = dest;
                info.trip.plannedAt = getMSTime();
                return false;
            }

            return HoldPosition(3 * IN_MILLISECONDS);
        }
        case Phase::Board:
        {
            // [timers] no boarding timeout: the stop ended (the transport left) -> wait for the next one
            if (!docked)
            {
                info.SetTripPhase(Phase::Wait);
                return HoldPosition();
            }

            // The followers board after the leader; [timers] it waits for a straggler only while that straggler still
            // reaches the deck before the stop ends (its run time against the time left after the leader's own run)
            if (!transport)
            {
                info.SetTripPhase(Phase::Wait);
                return HoldPosition();
            }
            uint32 const ownMs = BoardWalkMs(transport);
            if (!IsGroupGathered(GROUP_GATHER_DISTANCE))
            {
                std::string const straggler =
                    StragglerInTime(transport, remaining > ownMs ? remaining - ownMs : 0);
                if (!straggler.empty())
                    return HoldPosition();
                LOG_INFO("playerbots", "[rpg] {} L{} BOARD {} reason: no straggler reaches the deck before the stop "
                         "ends ({} ms left, own run {} ms)",
                         bot->GetName(), bot->GetLevel(), line->entry, remaining, ownMs);
            }

            return BoardTransport(transport);
        }
        default:
            info.ClearTrip();
            return false;
    }
}

bool NewRpgBaseAction::BoardTransport(Transport* transport)
{
    Map* map = bot->GetMap();
    // [tram28] a tram car is on no map transport list and filed in its spawn's grid cell: GetTransportForPos (the
    // list, then the nearest grid car) never names it at the far platform. Its own deck under the bot, as the core
    // tests a static car (a ray from 2 yd over the bot down onto the car's model, the ground not above the deck)
    bool onDeck = false;
    if (transport->ToStaticTransport() && transport->m_model)
    {
        G3D::Vector3 const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() + 2.0f);
        G3D::Ray const ray(from, G3D::Vector3(0.0f, 0.0f, -1.0f));
        float distance = 10.0f;
        onDeck = transport->m_model->intersectRay(ray, distance, false, bot->GetPhaseMask(),
                                                  VMAP::ModelIgnoreFlags::Nothing) &&
                 map->GetHeight(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                true, 30.0f) < from.z - distance + 1.0f;
    }
    else
        onDeck = map->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                                         bot->GetPositionZ(), bot) == transport;
    if (onDeck)
    {
        transport->AddPassenger(bot, true);
        bot->StopMovingOnCurrentPos();
        botAI->rpgInfo.SetTripPhase(NewRpgInfo::TripPhase::Ride);
        QuestStats::TripBoard(bot, std::string(transport->ToStaticTransport() ? "tram car " : "transport ") +
                                       std::to_string(transport->GetEntry()));  // [log28]
        if (transport->ToStaticTransport())
            LOG_INFO("playerbots", "[tram28] {} L{} boards tram car {} at ({:.0f},{:.0f}) on map {}", bot->GetName(),
                     bot->GetLevel(), transport->GetEntry(), bot->GetPositionX(), bot->GetPositionY(),
                     bot->GetMapId());
        LOG_DEBUG("playerbots", "[New RPG] {} boarded transport {}", bot->GetName(), transport->GetEntry());
        return true;
    }

    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return true;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!FindDeckPoint(transport, x, y, z))
        return HoldPosition();

    MovePointDirect(x, y, z);
    return true;
}

// Probes from the pier towards the middle of the transport for a deck at about pier height, using the
// transport's own collision model.
bool NewRpgBaseAction::FindDeckPoint(Transport* transport, float& x, float& y, float& z)
{
    if (!transport->m_model)
        return false;

    float const startX = bot->GetPositionX();
    float const startY = bot->GetPositionY();
    float const startZ = bot->GetPositionZ();
    float const dx = transport->GetPositionX() - startX;
    float const dy = transport->GetPositionY() - startY;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length < DECK_STEP)
        return false;

    uint32 onDeck = 0;
    for (float travelled = DECK_STEP; travelled <= length; travelled += DECK_STEP)
    {
        float const px = startX + dx / length * travelled;
        float const py = startY + dy / length * travelled;
        G3D::Ray const ray(G3D::Vector3(px, py, startZ + DECK_RAY_ABOVE), G3D::Vector3(0.0f, 0.0f, -1.0f));
        float distance = DECK_RAY_LENGTH;
        if (!transport->m_model->intersectRay(ray, distance, false, bot->GetPhaseMask(),
                                              VMAP::ModelIgnoreFlags::Nothing))
        {
            if (onDeck)
                break;

            continue;
        }

        float const deckZ = startZ + DECK_RAY_ABOVE - distance;
        if (std::fabs(deckZ - startZ) > DECK_MAX_STEP)
            continue;

        x = px;
        y = py;
        z = deckZ;
        if (++onDeck >= DECK_SAMPLES_INSIDE)
            return true;
    }

    return onDeck > 0;
}

bool NewRpgBaseAction::RideTransport(Transport* transport)
{
    using Phase = NewRpgInfo::TripPhase;
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgInfo::Trip& trip = info.trip;
    sTravelRouteMgr.Observe(transport);

    TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(transport->GetEntry());
    if (!line)
        return HoldPosition();

    bool const plannedLeg = trip.leg.kind == TravelRouteMgr::LegKind::Transport &&
                            trip.leg.lineEntry == line->entry && trip.leg.toDock < line->docks.size() &&
                            trip.phase != Phase::None;
    uint32 remaining = 0;
    if (trip.phase != Phase::Ride && trip.phase != Phase::Leave)
    {
        if (!plannedLeg)
        {
            trip.leg = TravelRouteMgr::Leg();
            // [timers] an unplanned ride skips the dock it was boarded at until the transport has left it (no ride
            // timer): getting off where it got on is no trip
            trip.boardDock = NewRpgInfo::Trip::NO_DOCK;
            for (uint8 d = 0; d < line->docks.size(); ++d)
                if (TravelRouteMgr::IsDocked(transport, line->docks[d], remaining))
                {
                    trip.boardDock = d;
                    break;
                }
        }

        info.SetTripPhase(Phase::Ride);
    }
    if (!plannedLeg && trip.boardDock < line->docks.size() &&
        !TravelRouteMgr::IsDocked(transport, line->docks[trip.boardDock], remaining))
        trip.boardDock = NewRpgInfo::Trip::NO_DOCK;  // it left the stop it was boarded at

    TravelRouteMgr::Dock const* exitDock = nullptr;
    for (uint8 d = 0; d < line->docks.size() && !exitDock; ++d)
    {
        // [guild15] an unplanned ride that already chose its stop keeps it (the Leave phase), b327/b329 Mytaeleas,
        // Bierya 10+ round trips Rut'theran - Auberdine; [timers] the boarding dock is skipped while still docked there
        if (plannedLeg ? d != trip.leg.toDock : trip.phase != Phase::Leave && d == trip.boardDock)
            continue;

        if (TravelRouteMgr::IsDocked(transport, line->docks[d], remaining))
            exitDock = &line->docks[d];
    }

    // [guild15] the walk off takes the bot's own run time to the pier: a stop that ends before it would carry the bot
    // away on the deck's edge, where the transport check drops it into the sea (b324/b325 Thyllyn: off the
    // Rut'theran boat mid-sea, 3 drownings, 332 WALK_FAIL); it rides on to the next stop instead
    if (exitDock && trip.phase != Phase::Leave)
    {
        float const speed = bot->GetSpeed(bot->IsWalking() ? MOVE_WALK : MOVE_RUN);
        uint32 const walkMs =
            uint32(bot->GetExactDist(exitDock->pier) / (speed > 0.0f ? speed : 1.0f) * IN_MILLISECONDS);
        if (remaining < walkMs)
        {
            LOG_INFO("playerbots", "[rpg] {} L{} stays aboard {}: the stop ends in {} ms, the walk off takes {} ms",
                     bot->GetName(), bot->GetLevel(), line->entry, remaining, walkMs);
            exitDock = nullptr;
        }
    }

    if (!exitDock)
    {
        if (trip.phase == Phase::Leave)
        {
            // [guild15] the stop is over and the bot stands on the land (the static ground at or above the deck under
            // it, a client knows the surface it stands on): off the passenger list, the leg is done - never a walk to
            // a deck point of the departed transport (b332 Eraelor stood on the Auberdine pier as a passenger, walked
            // "back onto the deck" with the transport's stale offset and was dropped in the sea, 400+ WALK_FAIL)
            float const bx = bot->GetPositionX(), by = bot->GetPositionY(), bz = bot->GetPositionZ();
            float deckZ = INVALID_HEIGHT;
            if (transport->m_model)
            {
                G3D::Ray const ray(G3D::Vector3(bx, by, bz + DECK_RAY_ABOVE), G3D::Vector3(0.0f, 0.0f, -1.0f));
                float distance = DECK_RAY_LENGTH;
                if (transport->m_model->intersectRay(ray, distance, false, bot->GetPhaseMask(),
                                                     VMAP::ModelIgnoreFlags::Nothing))
                    deckZ = bz + DECK_RAY_ABOVE - distance;
            }
            bool const onDeck = deckZ != INVALID_HEIGHT && std::fabs(deckZ - bz) <= DECK_MAX_STEP;
            float const groundZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(), bx, by, bz + DECK_RAY_ABOVE, true);
            bool const onLand = groundZ > INVALID_HEIGHT && groundZ != VMAP_INVALID_HEIGHT_VALUE &&
                                std::fabs(groundZ - bz) <= DECK_MAX_STEP && (!onDeck || groundZ >= deckZ);
            if (onLand)
            {
                transport->RemovePassenger(bot, true);
                bot->StopMovingOnCurrentPos();
                QuestStats::TripLand(bot);  // [log28]
                info.ClearTrip();
                LOG_INFO("playerbots", "[rpg] {} L{} TRIP_LEG_DONE off {}: on the land at ({:.1f},{:.1f},{:.1f}) as "
                         "the stop ended, the walk goes on from here",
                         bot->GetName(), bot->GetLevel(), line->entry, bx, by, bz);
                return HoldPosition(IN_MILLISECONDS);
            }
            // [guild15] the transport left while the bot walked off: back to the middle of the deck, not the edge
            info.SetTripPhase(Phase::Ride);
            LOG_INFO("playerbots", "[rpg] {} L{} back onto the deck of {}: it left the stop before the bot was off "
                     "(moving {}, walking {}, at ({:.1f},{:.1f},{:.1f}), offset on the transport ({:.1f},{:.1f},{:.1f}))",
                     bot->GetName(), bot->GetLevel(), line->entry, bot->isMoving() ? "yes" : "no",
                     bot->IsWalking() ? "yes" : "no", bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                     bot->GetTransOffsetX(), bot->GetTransOffsetY(), bot->GetTransOffsetZ());
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (FindDeckPoint(transport, x, y, z))
            {
                MovePointDirect(x, y, z);
                return true;
            }
        }

        return HoldPosition();
    }

    if (trip.phase != Phase::Leave)
    {
        info.SetTripPhase(Phase::Leave);
        LOG_INFO("playerbots", "[rpg] {} L{} leaves transport {} at map {}: pier {:.0f} yd, the stop ends in {} ms "
                 "(walking {}, speed {:.1f})",
                 bot->GetName(), bot->GetLevel(), line->entry, exitDock->mapId, bot->GetExactDist(exitDock->pier),
                 remaining, bot->IsWalking() ? "yes" : "no", bot->GetSpeed(bot->IsWalking() ? MOVE_WALK : MOVE_RUN));
    }

    // [guild15] it steps off as a player does: off the boat at the stop, then a straight walk to the pier in the
    // world's frame (a passenger's walk stalled at the deck's edge: b331 Eraelor, "moving no", 11 yd from the pier,
    // until the boat left); the transport check does not board it again in this phase
    if (bot->GetTransport() == transport)
    {
        transport->RemovePassenger(bot, true);
        bot->StopMovingOnCurrentPos();
        QuestStats::TripLand(bot);  // [log28]
        LOG_INFO("playerbots", "[rpg] {} L{} steps off {} at the stop, {:.0f} yd to the pier", bot->GetName(),
                 bot->GetLevel(), line->entry, bot->GetExactDist(exitDock->pier));
    }
    MovePointDirect(exitDock->pier.GetPositionX(), exitDock->pier.GetPositionY(), exitDock->pier.GetPositionZ());
    return true;
}

bool NewRpgBaseAction::TravelByPortal(NewRpgInfo::Trip& trip)
{
    using Phase = NewRpgInfo::TripPhase;
    NewRpgInfo& info = botAI->rpgInfo;
    std::vector<TravelRouteMgr::Portal> const& portals = sTravelRouteMgr.GetPortals();
    if (trip.leg.portalIndex >= portals.size() || bot->GetMapId() != portals[trip.leg.portalIndex].from.GetMapId())
    {
        info.ClearTrip();
        return false;
    }

    TravelRouteMgr::Portal const& portal = portals[trip.leg.portalIndex];
    if (trip.phase == Phase::ToStart)
    {
        // [guild15] an area trigger is reached inside its radius: its centre can hang in the air (b327 the Deeprun
        // Tram entrances 2173 / 2175, radius 10, centre 4-5 yd above the floor: bots stood in the trigger walking to
        // its unreachable centre, gave up, STUCK; no bot ever rode the tram)
        AreaTrigger const* const at = portal.triggerId ? sObjectMgr->GetAreaTrigger(portal.triggerId) : nullptr;
        if (at ? !bot->IsInAreaTriggerRadius(at) : bot->GetExactDist(portal.from) > INTERACTION_DISTANCE - 1.0f)
            return WalkTo(portal.from);

        info.SetTripPhase(Phase::Wait);
    }

    // [guild15] an area trigger teleport: the bot walks into it (its centre) and the core's area trigger handler
    // checks the radius and teleports it, as a player's client reports the trigger
    if (portal.triggerId)
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(portal.triggerId);
        WorldPosition const dest = trip.dest;
        if (!trigger)
        {
            info.ClearTrip();
            return false;
        }
        if (!bot->IsInAreaTriggerRadius(trigger))
        {
            if (bot->GetExactDist(trigger->x, trigger->y, trigger->z) > 1.0f)
                return WalkTo(WorldPosition(trigger->map, trigger->x, trigger->y, trigger->z));
            info.ClearTrip();
            info.trip.dest = dest;
            info.trip.plannedAt = getMSTime();
            return false;
        }
        info.ClearTrip();
        LOG_INFO("playerbots", "[travel] {} L{} walks into area trigger {} -> ({:.0f},{:.0f},{:.0f})", bot->GetName(),
                 bot->GetLevel(), portal.triggerId, portal.to.GetPositionX(), portal.to.GetPositionY(),
                 portal.to.GetPositionZ());
        WorldPacket packet(CMSG_AREATRIGGER, 4);
        packet << portal.triggerId;
        packet.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(packet);
        return HoldPosition(2 * IN_MILLISECONDS);
    }

    // Everyone steps through together: a member left on another continent has no way to catch up. [timers] no gather
    // timer: the leader waits while a member beyond the portal's reach is coming (alive, on this map, a walk reaches
    // the leader from it); one that cannot come does not hold it
    if (!IsGroupGathered(GROUP_USE_DISTANCE))
    {
        std::string const coming = ComingStraggler(GROUP_USE_DISTANCE);
        if (!coming.empty())
            return HoldPosition();
        LOG_INFO("playerbots", "[rpg] {} L{} PORTAL reason: the members not at the portal have no walk to it",
                 bot->GetName(), bot->GetLevel());
    }

    GameObject* go = bot->FindNearestGameObject(portal.goEntry, 2.0f * INTERACTION_DISTANCE);
    WorldPosition const dest = trip.dest;
    info.ClearTrip();
    if (!go || !go->isSpawned())
    {
        info.trip.dest = dest;
        info.trip.plannedAt = getMSTime();
        return false;
    }

    QuestStats::TripBoard(bot, "portal " + std::to_string(go->GetEntry()));  // [log28]
    UseGameObjectWithGroup(go);
    return HoldPosition(5 * IN_MILLISECONDS);
}

// Group bots next to the object use it first (it may close behind the first user), the bot last.
bool NewRpgBaseAction::UseGameObjectWithGroup(GameObject* go)
{
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() || member->IsInCombat() ||
                member->GetMapId() != bot->GetMapId() || member->IsBeingTeleported() || !GET_PLAYERBOT_AI(member) ||
                member->GetDistance(go) > GROUP_USE_DISTANCE)
                continue;

            go->Use(member);
        }
    }

    if (bot->isMoving())
        bot->StopMoving();

    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
    use << go->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(use);
    return true;
}

bool NewRpgBaseAction::HasDalaranTeleportQuest(Player* player)
{
    using namespace DalaranTravel;
    return player->GetQuestStatus(QUEST_HORDE) == QUEST_STATUS_COMPLETE ||
           player->GetQuestStatus(QUEST_ALLIANCE) == QUEST_STATUS_COMPLETE ||
           player->GetQuestStatus(QUEST_DEATH_KNIGHT) == QUEST_STATUS_COMPLETE;
}

// Up to or down from Dalaran by its own ways: the Violet Stand crystals, a flight to Krasus' Landing, the
// Kirin Tor envoy teleport of the level 74 quest, or a group mage's portal (see MoveFarTo). Nothing walks or
// flies in over the city.
bool NewRpgBaseAction::DalaranStep(WorldPosition const& dest)
{
    using namespace DalaranTravel;
    WorldPosition const botPos(bot);

    if (TravelRouteMgr::IsInDalaran(botPos))
    {
        if (bot->GetQuestRewardStatus(QUEST_LEAVE_AND_RETURN) ||
            bot->GetQuestStatus(QUEST_LEAVE_AND_RETURN) == QUEST_STATUS_INCOMPLETE)
            return UseDalaranCrystal(GO_CRYSTAL_DOWN);

        // fly down to the discovered flight path nearest to the destination
        uint32 toNode = 0;
        float nearest = std::numeric_limits<float>::max();
        for (uint32 i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
            if (!node || node->ID == TAXI_NODE_KRASUS_LANDING || node->map_id != dest.GetMapId() ||
                !bot->m_taxi.IsTaximaskNodeKnown(node->ID))
                continue;

            float const distance = std::hypot(node->x - dest.GetPositionX(), node->y - dest.GetPositionY());
            if (distance < nearest)
            {
                nearest = distance;
                toNode = node->ID;
            }
        }

        return FlyTowards(toNode);
    }

    enum class Way : uint8
    {
        None,
        Taxi,
        Crystal,
        Envoy
    };
    Way way = Way::None;
    float best = std::numeric_limits<float>::max();
    WorldPosition const city(MAP_ID, CENTER_X, CENTER_Y, MIN_Z);

    if (bot->m_taxi.IsTaximaskNodeKnown(TAXI_NODE_KRASUS_LANDING))
    {
        if (TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot))
        {
            float const seconds = Distance2d(botPos, master->pos) / TravelRouteMgr::WALK_SPEED +
                                  Distance2d(master->pos, city) / FLIGHT_SPEED;
            if (seconds < best)
            {
                best = seconds;
                way = Way::Taxi;
            }
        }
    }

    if (bot->GetQuestRewardStatus(QUEST_CRYSTAL_FLAG))
    {
        for (WorldPosition const& crystal : sTravelRouteMgr.GetObjectSpawns(GO_CRYSTAL_UP))
        {
            float const seconds = Distance2d(botPos, crystal) / TravelRouteMgr::WALK_SPEED;
            if (crystal.GetMapId() == bot->GetMapId() && seconds < best)
            {
                best = seconds;
                way = Way::Crystal;
            }
        }
    }

    if (HasDalaranTeleportQuest(bot))
    {
        for (uint32 envoy : QUEST_GIVERS)
        {
            for (WorldPosition const& spawn : sTravelRouteMgr.GetSpawns(envoy))
            {
                float const seconds = Distance2d(botPos, spawn) / TravelRouteMgr::WALK_SPEED;
                if (spawn.GetMapId() == bot->GetMapId() && seconds < best)
                {
                    best = seconds;
                    way = Way::Envoy;
                }
            }
        }
    }

    switch (way)
    {
        case Way::Taxi:
            return FlyTowards(TAXI_NODE_KRASUS_LANDING);
        case Way::Crystal:
            return UseDalaranCrystal(GO_CRYSTAL_UP);
        case Way::Envoy:
            return TeleportByDalaranEnvoy();
        default:
            return false;
    }
}

// Hands over to the flight status: walk to the nearest flight master, fly a route over discovered nodes, pay.
bool NewRpgBaseAction::FlyTowards(uint32 toNode)
{
    TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!master || !toNode || !master->taxiNodeId)
        return false;

    std::vector<uint32> path = sTravelNodeMap.FindKnownTaxiPath(bot, master->taxiNodeId, toNode);
    if (path.empty())
        return false;

    botAI->rpgInfo.ChangeToTravelFlight(master->templateEntry, master->pos, path);
    return true;
}

// [taxi20] coordinator 2026-11-09: a guild bot takes a flight when it is the faster way to `dest` on this map - the
// walk to its nearest flight master, the flight over its known nodes (their straight legs at the realm's taxi speed,
// CONFIG_TAXI_FLIGHT_SPEED) and the walk from the known node nearest `dest`, against the straight walk there at its
// run speed (no walk is shorter) - and it holds the fare (each hop's TaxiPath price). Decided once per destination.
// b367: the q6391 / q6388 / q6392 chain (meant to be flown) was walked; Keenonk and Namwe stood STUCK at Gryth
// Thurden in Ironforge, Namwe walked 2100+ yd back to Thelsamar
bool NewRpgBaseAction::FlyIfFaster(WorldPosition const& dest, bool walkRefused)
{
    // [fly109] a co-op / help party's leader flies too (its party with it: MazeWalk's [fly97])
    bool const partyLeader = NewRpgCoop::JudgesPulls(bot) && bot->GetGroup() && !bot->GetMap()->Instanceable();
    if (!GuildHonest::IsHonestBot(bot) || (!SoloSafety::Applies(bot) && !partyLeader) ||
        dest.GetMapId() != bot->GetMapId() ||
        bot->GetMap()->Instanceable() || botAI->rpgInfo.GetStatus() == RPG_TRAVEL_FLIGHT)
        return false;
    static std::mutex flyLock;
    static std::unordered_map<uint32, std::tuple<uint32, int32, int32>> decided;
    std::tuple<uint32, int32, int32> const key{dest.GetMapId(), int32(dest.GetPositionX() / 10.0f),
                                               int32(dest.GetPositionY() / 10.0f)};
    {
        std::lock_guard<std::mutex> guard(flyLock);
        auto& last = decided[bot->GetGUID().GetCounter()];
        if (last == key && !walkRefused)
            return false;
        last = key;
    }
    TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!master || !master->taxiNodeId || master->pos.GetMapId() != bot->GetMapId() ||
        !bot->m_taxi.IsTaximaskNodeKnown(master->taxiNodeId))
        return false;
    // [lag332] the flight master on the bot's own landmass (TravelRouteMgr::Landmass: only a ride joins two): the
    // nearest one by the straight line may stand across the water. b442 Sylaenas L13 (the lowest member) on Teldrassil
    // walled from q483's spot: "[fly97] flies ... from Auberdine, Darkshore -> Rut'theran Village", her walk to the
    // Auberdine master took the portal down and the boat over, the flight brought her back - a Teldrassil-Darkshore
    // circle of portal, boat and flight, TRAVEL 73 % of her 37 min, 2.7 same-level kills an hour
    if (TravelRouteMgr::Landmass(master->pos) != TravelRouteMgr::Landmass(WorldPosition(bot)))
        return false;
    // [fly164] a flight master whose walk is given up at this level is no flight from here: b438 Sycina L19 on a
    // Stonetalon ridge (1764,-1019,246) gave the walk to the Astranaar flight master (2828,-284) up (MAZE dead end,
    // MOVE_FAR_LOOP), and every next quest's walk chose the flight from it again - WALK_FAIL x89 from that spot, 37 %
    // of the slow update time
    if (WalkGivenUp(bot, master->pos.GetPositionX(), master->pos.GetPositionY()))
    {
        NoteFlightMasterGivenUp(bot, master->pos);
        return false;
    }
    TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(master->taxiNodeId);
    TaxiNodesEntry const* to = nullptr;
    float toGap = std::numeric_limits<float>::max();
    for (uint32 i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
    {
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
        if (!node || node->map_id != dest.GetMapId() || !bot->m_taxi.IsTaximaskNodeKnown(node->ID))
            continue;
        // [lag332] ... and the landing on the destination's: the walk off it must reach the destination
        if (TravelRouteMgr::Landmass(WorldPosition(node->map_id, node->x, node->y, node->z)) !=
            TravelRouteMgr::Landmass(dest))
            continue;
        float const gap = std::hypot(node->x - dest.GetPositionX(), node->y - dest.GetPositionY());
        if (gap < toGap)
        {
            toGap = gap;
            to = node;
        }
    }
    if (!from || !to || to->ID == from->ID)
        return false;
    std::vector<uint32> path = sTravelNodeMap.FindKnownTaxiPath(bot, from->ID, to->ID);
    if (path.size() < 2)
        return false;
    float flightYd = 0.0f;
    float hopDelay = 0.0f;  // [taxi22] seconds the flight stands at its path's stops
    uint32 fare = 0;
    for (size_t i = 1; i < path.size(); ++i)
    {
        TaxiNodesEntry const* a = sTaxiNodesStore.LookupEntry(path[i - 1]);
        TaxiNodesEntry const* b = sTaxiNodesStore.LookupEntry(path[i]);
        if (!a || !b)
            return false;
        uint32 pathId = 0, cost = 0;
        sObjectMgr->GetTaxiPath(path[i - 1], path[i], pathId, cost);
        fare += cost;
        // [taxi22] the flight's own path (its TaxiPathNode points, with their stops' delays), not the straight hop:
        // b370 FLIGHT 8 % of the time, the straight hops underpriced every flight
        float hop = std::hypot(a->x - b->x, a->y - b->y);
        if (pathId < sTaxiPathNodesByPath.size() && sTaxiPathNodesByPath[pathId].size() >= 2)
        {
            TaxiPathNodeList const& nodes = sTaxiPathNodesByPath[pathId];
            hop = 0.0f;
            for (size_t k = 1; k < nodes.size(); ++k)
                hop += std::sqrt((nodes[k]->x - nodes[k - 1]->x) * (nodes[k]->x - nodes[k - 1]->x) +
                                 (nodes[k]->y - nodes[k - 1]->y) * (nodes[k]->y - nodes[k - 1]->y) +
                                 (nodes[k]->z - nodes[k - 1]->z) * (nodes[k]->z - nodes[k - 1]->z));
            for (TaxiPathNodeEntry const* node : nodes)
                hopDelay += float(node->delay);
        }
        flightYd += hop;
    }
    float const run = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    float const taxi = std::max(sWorld->getFloatConfig(CONFIG_TAXI_FLIGHT_SPEED), 1.0f);
    // [taxi22] the walks by their routes (Leader::WalkYd: the navmesh walk), the flight master's and the one off
    // the landing; the walk it replaces by its route too (a real walk, not the straight line)
    WorldPosition const landing(dest.GetMapId(), to->x, to->y, to->z);
    float const toMaster = Leader::WalkYd(bot, bot->GetPosition(), master->pos);
    float const offLanding = std::max(toGap, Leader::WalkYd(bot, landing, dest));
    float const fly = toMaster / run + flightYd / taxi + hopDelay + offLanding / run;
    float const walk = Leader::WalkYd(bot, bot->GetPosition(), dest) / run;
    if ((fly >= walk && !walkRefused) || bot->GetMoney() < fare)
        return false;
    LOG_INFO("playerbots", "[taxi20] {} L{} flies {} -> {} ({} nodes, {} c): {:.0f} s by air against {:.0f} s on "
             "foot to ({:.0f},{:.0f}) [taxi22: walks by their navmesh routes, the flight by its path points]",
             bot->GetName(), bot->GetLevel(), from->name[0], to->name[0], path.size(), fare, fly, walk,
             dest.GetPositionX(), dest.GetPositionY());
    botAI->rpgInfo.ChangeToTravelFlight(master->templateEntry, master->pos, path);
    return true;
}

bool NewRpgBaseAction::UseDalaranCrystal(uint32 entry)

{
    WorldPosition crystal;
    float nearest = std::numeric_limits<float>::max();
    for (WorldPosition const& spawn : sTravelRouteMgr.GetObjectSpawns(entry))
    {
        if (spawn.GetMapId() == bot->GetMapId() && bot->GetExactDist(spawn) < nearest)
        {
            nearest = bot->GetExactDist(spawn);
            crystal = spawn;
        }
    }

    if (crystal == WorldPosition())
        return false;

    if (nearest > CRYSTAL_REACHED)
        return MoveFarToDirect(crystal);

    GameObject* go = bot->FindNearestGameObject(entry, 2.0f * INTERACTION_DISTANCE);
    if (!go || !go->isSpawned())
        return false;

    // The crystal closes for a second after each use: send the members over one by one, the bot last.
    if (go->HasGameObjectFlag(GO_FLAG_IN_USE))
        return HoldPosition(IN_MILLISECONDS);

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() || member->IsInCombat() ||
                member->GetMapId() != bot->GetMapId() || member->IsBeingTeleported() || !GET_PLAYERBOT_AI(member) ||
                member->GetDistance(go) > GROUP_USE_DISTANCE)
                continue;

            bool const canUse = entry == DalaranTravel::GO_CRYSTAL_UP
                                    ? member->GetQuestRewardStatus(DalaranTravel::QUEST_CRYSTAL_FLAG)
                                    : member->GetQuestRewardStatus(DalaranTravel::QUEST_LEAVE_AND_RETURN) ||
                                          member->GetQuestStatus(DalaranTravel::QUEST_LEAVE_AND_RETURN) ==
                                              QUEST_STATUS_INCOMPLETE;
            if (!canUse)
                continue;

            go->Use(member);
            return HoldPosition(IN_MILLISECONDS + IN_MILLISECONDS / 2);
        }
    }

    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
    use << go->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(use);
    return HoldPosition(3 * IN_MILLISECONDS);
}

void NewRpgBaseAction::GossipTeleportToDalaran(Player* player, Creature* envoy)
{
    uint32 const menuId = envoy->GetCreatureTemplate()->GossipMenuId;
    WorldPacket hello(CMSG_GOSSIP_HELLO, 8);
    hello << envoy->GetGUID();
    player->GetSession()->HandleGossipHelloOpcode(hello);

    WorldPacket select(CMSG_GOSSIP_SELECT_OPTION, 8 + 4 + 4);
    select << envoy->GetGUID() << menuId << DalaranTravel::GOSSIP_OPTION_TELEPORT;
    player->GetSession()->HandleGossipSelectOptionOpcode(select);
}

// "I am ready to be teleported to Dalaran": the envoy only offers it to holders of the completed quest.
bool NewRpgBaseAction::TeleportByDalaranEnvoy()
{
    using namespace DalaranTravel;
    Creature* envoy = nullptr;
    for (uint32 entry : QUEST_GIVERS)
    {
        Creature* creature = bot->FindNearestCreature(entry, ENVOY_SEARCH_RANGE);
        if (creature && creature->IsAlive() && (!envoy || bot->GetDistance(creature) < bot->GetDistance(envoy)))
            envoy = creature;
    }

    if (!envoy)
    {
        WorldPosition target;
        float nearest = std::numeric_limits<float>::max();
        for (uint32 entry : QUEST_GIVERS)
        {
            for (WorldPosition const& spawn : sTravelRouteMgr.GetSpawns(entry))
            {
                if (spawn.GetMapId() == bot->GetMapId() && bot->GetExactDist(spawn) < nearest)
                {
                    nearest = bot->GetExactDist(spawn);
                    target = spawn;
                }
            }
        }

        if (target == WorldPosition())
            return false;

        return MoveFarToDirect(target);
    }

    if (!bot->IsWithinDistInMap(envoy, ENVOY_REACHED))
    {
        MoveWorldObjectTo(envoy->GetGUID(), 2.0f);
        return true;
    }

    // members next to the envoy who hold the quest too go up with the bot
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsInWorld() && member->IsAlive() && GET_PLAYERBOT_AI(member) &&
                member->GetMapId() == bot->GetMapId() && member->IsWithinDistInMap(envoy, INTERACTION_DISTANCE) &&
                HasDalaranTeleportQuest(member))
                GossipTeleportToDalaran(member, envoy);
        }
    }

    GossipTeleportToDalaran(bot, envoy);
    return HoldPosition(3 * IN_MILLISECONDS);
}

// Portal: Dalaran from a mage in the group (the bot itself or a group bot next to it).
bool NewRpgBaseAction::UseDalaranPortal()
{
    using namespace DalaranTravel;
    NewRpgInfo& info = botAI->rpgInfo;
    if (GameObject* portal = bot->FindNearestGameObject(GO_PORTAL_DALARAN, GROUP_GATHER_DISTANCE))
    {
        info.dalaranPortalMage.Clear();
        if (!bot->IsWithinDistInMap(portal, INTERACTION_DISTANCE - 1.0f))
        {
            MoveWorldObjectTo(portal->GetGUID(), 1.5f);
            return true;
        }

        return UseGameObjectWithGroup(portal);
    }

    // [timers] no cast wait timer: the asked mage is waited for while it casts the portal (the spell's own cast time);
    // its cast over and no portal here: that mage failed for this destination
    if (!info.dalaranPortalMage.IsEmpty())
    {
        Player* asked = ObjectAccessor::FindConnectedPlayer(info.dalaranPortalMage);
        if (asked && asked->IsInWorld() && asked->FindCurrentSpellBySpellId(SPELL_PORTAL_DALARAN))
            return HoldPosition();

        LOG_INFO("playerbots", "[rpg] {} L{} DALARAN_PORTAL reason: {}'s cast ended without a portal", bot->GetName(),
                 bot->GetLevel(), asked ? asked->GetName() : std::string("the mage"));
        info.dalaranPortalFailedMage = info.dalaranPortalMage;
        info.dalaranPortalMage.Clear();
        return false;
    }

    Player* mage = nullptr;
    auto canOpen = [this, &info](Player* player)
    {
        // [timers] no retry timer: a mage whose portal failed for this destination is not asked again for it
        return player && player->GetGUID() != info.dalaranPortalFailedMage && player->IsInWorld() &&
               player->IsAlive() && !player->IsInCombat() &&
               player->getClass() == CLASS_MAGE && player->GetMapId() == bot->GetMapId() &&
               bot->GetDistance(player) < GROUP_GATHER_DISTANCE && player->HasSpell(SPELL_PORTAL_DALARAN) &&
               !player->HasSpellCooldown(SPELL_PORTAL_DALARAN) && player->HasItemCount(ITEM_RUNE_OF_PORTALS, 1) &&
               !player->IsNonMeleeSpellCast(false);
    };

    if (canOpen(bot))
        mage = bot;
    else if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref && !mage; ref = ref->next())
            if (canOpen(ref->GetSource()) && GET_PLAYERBOT_AI(ref->GetSource()))
                mage = ref->GetSource();

    if (!mage)
        return false;

    PlayerbotAI* mageAI = mage == bot ? botAI : GET_PLAYERBOT_AI(mage);
    if (!mageAI || !mageAI->CastSpell(SPELL_PORTAL_DALARAN, mage))
    {
        LOG_INFO("playerbots", "[rpg] {} L{} DALARAN_PORTAL reason: {} could not cast it", bot->GetName(),
                 bot->GetLevel(), mage->GetName());
        info.dalaranPortalFailedMage = mage->GetGUID();
        return false;
    }

    LOG_DEBUG("playerbots", "[New RPG] {} asked {} for a portal to Dalaran", bot->GetName(), mage->GetName());
    info.dalaranPortalMage = mage->GetGUID();
    return HoldPosition();
}

bool NewRpgBaseAction::NpcOutOfWalk(WorldObject* npc)
{
    if (!npc || bot->isMoving() || npc->GetMapId() != bot->GetMapId())
        return false;
    BotPath const path = FindBotPath(bot, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
    float const endGap = npc->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z);
    if ((path.type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) && endGap <= INTERACTION_DISTANCE)
        return false;
    LOG_INFO("playerbots", "[rpg] {} L{} npc {} out of walk: the route ends {:.0f} yd from it (type {:#x}), "
             "{:.0f} yd away", bot->GetName(), bot->GetLevel(), npc->GetName(), endGap, uint32(path.type),
             bot->GetExactDist(npc));
    return true;
}

// [guild13] one line per bot and destination when its far walk cannot start: what the route and the walk said
void NewRpgBaseAction::NoteWalkFailure(WorldPosition const& dest, std::string const& why)
{
    static std::mutex lock;
    static std::unordered_map<uint32, std::pair<int32, int32>> logged;
    std::pair<int32, int32> const key{int32(dest.GetPositionX()), int32(dest.GetPositionY())};
    {
        std::lock_guard<std::mutex> guard(lock);
        auto& last = logged[bot->GetGUID().GetCounter()];
        if (last == key)
            return;
        last = key;
    }
    LOG_INFO("playerbots", "[rpg] {} L{} WALK_FAIL at ({:.0f},{:.0f},{:.0f}) to ({:.0f},{:.0f},{:.0f}) {:.0f} yd: {}",
             bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
             dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), bot->GetExactDist(dest), why);
}

// [guild14] Player::HandleFall's fall damage (Player.cpp: FALL_DMG_EQU_SLOPE, FALL_DMG_EQU_INTERCEPT): the share of
// the maximum health a fall of `height` yards takes, safe fall auras counted
static float FallDamageShare(Player* bot, float height)
{
    float const safeFall = float(bot->GetTotalAuraModifier(SPELL_AURA_SAFE_FALL));
    return std::max(0.0f, 0.018f * (height - safeFall) - 0.2426f) * sWorld->getRate(RATE_DAMAGE_FALL);
}

// [guild13] / [guild14] the bot's own search finds no way out from where it stands (a navmesh island: Aldrassil's
// upper floor, the Exodar's halls, a ledge, a building's upper floor in Darnassus): the core's path (its start polygon
// from the bot's small box, not the route's tall column, which can take the roof over a floor; never its straight
// shortcut, which glides through the air), the route found from the destination back to the bot walked the other way,
// and at last a step onto the nearest navmesh polygon in sight - walked when the ground carries it there, else a
// jump down when the fall (the core's fall damage) leaves the bot alive. Each one logs WALK_FAIL with the position.

namespace
{
    // [nav165] the pocket rescues: per bot (its GUID counter) its running deep search, and the pockets searched -
    // per bot, level and navmesh tile (533 yd) of the spot it stood at: one search per pocket and level
    struct PocketRescueState
    {
        Leader::Travel::DeepSearch search;
        WorldPosition dest;
        G3D::Vector3 from;
        uint32 mapId = 0;
    };
    std::mutex pocketLock;
    std::unordered_map<uint32, std::shared_ptr<PocketRescueState>> pocketRescues;
    std::set<std::tuple<uint32, uint8, uint32, int32, int32>> pocketsSearched;
    constexpr uint32 POCKET_SEARCH_NODES = 1u << 19;  // the node table's bound (~30 MB): a memory bound, no rule
    constexpr uint32 POCKET_SEARCH_SLICE = 4000;      // polygons expanded per bot update: a slice's cost, no rule
    constexpr float POCKET_TILE = 533.3333f;          // the navmesh tile (GRID_SIZE)
}

void NewRpgBaseAction::RequestPocketRescue(WorldPosition const& dest)
{
    // another landmass (the sea, a trigger's far side) is no walk at any depth: the travel planner's
    if (!SoloSafety::Applies(bot) || bot->GetMap()->Instanceable() || dest.GetMapId() != bot->GetMapId() ||
        !botAI->rpgInfo.rescueRoute.empty() ||
        TravelRouteMgr::Landmass(WorldPosition(bot)) != TravelRouteMgr::Landmass(dest))
        return;
    uint32 const counter = bot->GetGUID().GetCounter();
    std::tuple<uint32, uint8, uint32, int32, int32> const pocket{
        counter, bot->GetLevel(), bot->GetMapId(), int32(std::floor(bot->GetPositionX() / POCKET_TILE)),
        int32(std::floor(bot->GetPositionY() / POCKET_TILE))};
    {
        std::lock_guard<std::mutex> guard(pocketLock);
        if (pocketRescues.count(counter) || !pocketsSearched.insert(pocket).second)
            return;
    }
    auto state = std::make_shared<PocketRescueState>();
    state->dest = dest;
    state->from = G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    state->mapId = bot->GetMapId();
    std::string why;
    if (!state->search.Start(bot, state->from,
                             G3D::Vector3(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()),
                             POCKET_SEARCH_NODES, why))
    {
        LOG_INFO("playerbots", "[nav165] {} L{} no deep search out of ({:.0f},{:.0f},{:.0f}) to ({:.0f},{:.0f}): {}",
                 bot->GetName(), bot->GetLevel(), state->from.x, state->from.y, state->from.z, dest.GetPositionX(),
                 dest.GetPositionY(), why);
        return;
    }
    LOG_INFO("playerbots",
             "[nav165] {} L{} deep search out of ({:.0f},{:.0f},{:.0f}) to ({:.0f},{:.0f}) {:.0f} yd: its walk there "
             "ends in a dead end; it stands while the search runs ({} polygons an update)",
             bot->GetName(), bot->GetLevel(), state->from.x, state->from.y, state->from.z, dest.GetPositionX(),
             dest.GetPositionY(), bot->GetExactDist(dest), POCKET_SEARCH_SLICE);
    std::lock_guard<std::mutex> guard(pocketLock);
    pocketRescues[counter] = std::move(state);
}

bool NewRpgBaseAction::PocketRescue()
{
    NewRpgInfo& info = botAI->rpgInfo;
    uint32 const counter = bot->GetGUID().GetCounter();
    std::shared_ptr<PocketRescueState> state;
    {
        std::lock_guard<std::mutex> guard(pocketLock);
        auto const found = pocketRescues.find(counter);
        if (found != pocketRescues.end())
            state = found->second;
    }
    if (state)
    {
        auto const drop = [&]()
        {
            std::lock_guard<std::mutex> guard(pocketLock);
            pocketRescues.erase(counter);
        };
        if (state->mapId != bot->GetMapId() || bot->IsInCombat())
        {
            if (state->mapId != bot->GetMapId())
                drop();
            return false;
        }
        Leader::Travel::DeepSearch::State const result = state->search.Step(bot, POCKET_SEARCH_SLICE);
        if (result == Leader::Travel::DeepSearch::State::Running)
            return HoldPosition(IN_MILLISECONDS);
        drop();
        if (result != Leader::Travel::DeepSearch::State::Found)
        {
            LOG_INFO("playerbots", "[nav165] {} L{} deep search out of ({:.0f},{:.0f}) failed after {} updates: {}",
                     bot->GetName(), bot->GetLevel(), state->from.x, state->from.y, state->search.Steps(),
                     state->search.Why());
            return false;
        }
        info.rescueRoute.assign(state->search.Corners().begin(), state->search.Corners().end());
        info.rescueMap = bot->GetMapId();
        LOG_INFO("playerbots", "[nav165] {} L{} deep search out of ({:.0f},{:.0f}) found its way to ({:.0f},{:.0f}): "
                 "{:.0f} yd, {}, {} corners, {} updates; walks it",
                 bot->GetName(), bot->GetLevel(), state->from.x, state->from.y, state->dest.GetPositionX(),
                 state->dest.GetPositionY(), state->search.Length(), state->search.Why(), info.rescueRoute.size(),
                 state->search.Steps());
    }
    std::vector<G3D::Vector3>& route = info.rescueRoute;
    if (route.size() < 2)
        return false;
    float const sight = sPlayerbotAIConfig.SightDistance;
    auto const leave = [&](char const* why)
    {
        LOG_INFO("playerbots", "[nav165] {} L{} leaves its way out of the pocket at ({:.0f},{:.0f},{:.0f}): {}",
                 bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), why);
        route.clear();
        return false;
    };
    if (info.rescueMap != bot->GetMapId())
        return leave("another map");
    // where on the way it stands: the nearest point of its segments
    G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    size_t segment = 0;
    G3D::Vector3 onWay = route.front();
    float best = std::numeric_limits<float>::max();
    for (size_t i = 1; i < route.size(); ++i)
    {
        G3D::Vector3 const ab = route[i] - route[i - 1];
        float const lengthSq = ab.squaredLength();
        float const t = lengthSq > 0.0f ? std::clamp((here - route[i - 1]).dot(ab) / lengthSq, 0.0f, 1.0f) : 0.0f;
        G3D::Vector3 const point = route[i - 1] + ab * t;
        if (float const d = (point - here).length(); d < best)
        {
            best = d;
            segment = i;
            onWay = point;
        }
    }
    if (best > sight)
        return leave("it stands off the way");
    Movement::PointsArray rest;
    rest.push_back(onWay);
    rest.insert(rest.end(), route.begin() + segment, route.end());
    float restLength = 0.0f;
    for (size_t i = 1; i < rest.size(); ++i)
        restLength += (rest[i] - rest[i - 1]).length();
    if (restLength <= 2.0f * sight)
        return leave("out of the pocket: the walk on is searched as any walk");
    // the next leg as any leg on a known way ([guild15]'s far route): the local search to a sight ahead on the way
    // prices the mobs in sight; with none, the way's own corners to a sight ahead, past no mob it must not fight
    G3D::Vector3 const target = Leader::Travel::PointOnPath(rest, sight);
    BotPath const local = FindBotPath(bot, target.x, target.y, target.z);
    if (local.type == PATHFIND_NORMAL && !local.throughRefused && local.points.size() >= 2 &&
        (local.actualEnd - target).length() < 5.0f)
    {
        G3D::Vector3 const leg =
            local.length > sight ? Leader::Travel::PointOnPath(local.points, sight) : local.actualEnd;
        if (MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z, false, false, false, true))
        {
            CommitLeg(bot, local.points, leg);  // [runpast11]
            return true;
        }
    }
    if (local.throughRefused)
        return false;  // a mob it must not fight on the next leg: the walk's own handling (the maze search's wait)
    Movement::PointsArray slice{here};
    float walked = (onWay - here).length();
    for (size_t i = 1; i < rest.size() && walked < sight; ++i)
    {
        float const length = (rest[i] - rest[i - 1]).length();
        if (walked + length > sight && length > 0.0f)
        {
            slice.push_back(rest[i - 1] + (rest[i] - rest[i - 1]) * ((sight - walked) / length));
            break;
        }
        slice.push_back(rest[i]);
        walked += length;
    }
    if (slice.size() < 2)
        return false;
    for (size_t i = 1; i < slice.size(); ++i)
        if (DangerOnWay(bot, slice[i - 1].x, slice[i - 1].y, slice[i].x, slice[i].y, slice[i - 1].z, slice[i].z))
            return false;
    if (!WalkBotPoints(bot, slice))
        return false;
    float const delay =
        std::clamp(float(IN_MILLISECONDS) * MoveDelay(walked), 0.0f, float(sPlayerbotAIConfig.MaxWaitForMove));
    G3D::Vector3 const& end = slice.back();
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), end.x, end.y, end.z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
    return true;
}

bool NewRpgBaseAction::WalkOffIsland(WorldPosition const& dest, float disToDest)
{
    // [guild14] a solo bot walks such a path only as far as it sees, and only when that stretch passes no mob of
    // orange / red con (b307 Guinudrond L9: the core's 2887 yd path from Loch Modan to Elwynn crossed Searing Gorge)
    auto const SafeLeg = [&](Movement::PointsArray& points)
    {
        if (!SoloSafety::Applies(bot) || points.size() < 2)
            return true;
        float walked = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
        {
            if (DangerOnWay(bot, points[i - 1].x, points[i - 1].y, points[i].x, points[i].y, points[i - 1].z, points[i].z))
                return false;
            walked += (points[i] - points[i - 1]).length();
            if (walked >= sPlayerbotAIConfig.SightDistance)
            {
                points.resize(i + 1);
                break;
            }
        }
        return true;
    };
    // [guild15] the ground under a straight leg (PlayerbotAI::HasGroundSupport, a yard apart, as the airborne
    // spline guard samples it): b308 Dandolar L4 stood 2365 s at (10091,2243,1340) in Darnassus - the core's path
    // from there began with a leg over open air, the guard refused it on every tick and the jump down was never tried
    auto const carried = [&](G3D::Vector3 const& from, G3D::Vector3 const& to)
    {
        float const gap = (to - from).length();
        for (float walked = 1.0f; walked < gap; walked += 1.0f)
        {
            G3D::Vector3 const at = from + (to - from) * (walked / gap);
            if (!PlayerbotAI::HasGroundSupport(bot, at.x, at.y, at.z))
                return false;
        }
        return true;
    };
    G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    onIsland = false;  // [runpast11]
    // every leg the bot sees of it (the walk is planned again from there)
    auto const firstLegCarried = [&](Movement::PointsArray const& points)
    {
        if (points.size() < 2 || !carried(here, points[0]))
            return false;
        float walked = 0.0f;
        for (size_t i = 1; i < points.size() && walked < sPlayerbotAIConfig.SightDistance; ++i)
        {
            if (!carried(points[i - 1], points[i]))
                return false;
            walked += (points[i] - points[i - 1]).length();
        }
        return true;
    };
    auto const logged = [&](std::string const& how)
    {
        // one line per bot and way out (a failing one is tried again on every walk)
        static std::mutex lock;
        static std::unordered_map<uint32, std::string> last;
        {
            std::lock_guard<std::mutex> guard(lock);
            std::string& previous = last[bot->GetGUID().GetCounter()];
            if (previous == how)
                return true;
            previous = how;
        }
        LOG_INFO("playerbots", "[rpg] {} L{} WALK_FAIL at ({:.1f},{:.1f},{:.1f}) map {} to ({:.0f},{:.0f},{:.0f}): {}",
                 bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                 bot->GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), how);
        return true;
    };
    PathGenerator core(bot);
    core.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false);
    PathType const coreType = core.GetPathType();
    auto const walkCore = [&]()
    {
        Movement::PointsArray points = core.GetPath();
        G3D::Vector3 const coreEnd = core.GetActualEndPosition();
        if (!(coreType & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) ||
            (coreType & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT)) || points.size() < 2 ||
            dest.GetExactDist(coreEnd.x, coreEnd.y, coreEnd.z) + 5.0f >= disToDest || !firstLegCarried(points) ||
            !SafeLeg(points))
            return false;
        // [nav165] a partial path (it does not reach the destination) only to where the route search plans a complete
        // walk back to the walk's start: b435 Sycina L18's partial legs (21, 50, 114 yd) toward the Zoram Strand went
        // up a Stonetalon valley onto a ridge (1764,-1019,246) whose only way out is 4084 yd, past the search's pool:
        // every walk from there a dead end, 89 WALK_FAIL in b438
        if (!(coreType & PATHFIND_NORMAL) && SoloSafety::Applies(bot))
        {
            NewRpgInfo const& info = botAI->rpgInfo;
            G3D::Vector3 const start = info.moveFarFromSet ? info.moveFarFrom : here;
            G3D::Vector3 const legEnd = points.back();
            if ((legEnd - start).length() > 1.0f)
            {
                BotPath const back = FindBotPath(bot, legEnd.x, legEnd.y, legEnd.z, start.x, start.y, start.z);
                if (back.type != PATHFIND_NORMAL || !back.leaderRoute)
                {
                    logged(Acore::StringFormat("[nav165] no partial core path to ({:.0f},{:.0f},{:.0f}): no complete "
                                               "walk back from there to the walk's start ({:.0f},{:.0f},{:.0f})",
                                               legEnd.x, legEnd.y, legEnd.z, start.x, start.y, start.z));
                    return false;
                }
            }
        }
        if (!WalkBotPoints(bot, points))
            return false;
        float const delay = std::clamp(float(IN_MILLISECONDS) * MoveDelay(core.getPathLength()), 0.0f,
                                       float(sPlayerbotAIConfig.MaxWaitForMove));
        AI_VALUE(LastMovement&, "last movement")
            .Set(bot->GetMapId(), coreEnd.x, coreEnd.y, coreEnd.z, bot->GetOrientation(), delay,
                 MovementPriority::MOVEMENT_NORMAL);
        return logged(Acore::StringFormat("no route out from here: walks the core's path ({:.0f} yd)",
                                          core.getPathLength()));
    };
    auto const walkBack = [&]()
    {
        BotPath back = FindBotPath(bot, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                                   bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (back.type != PATHFIND_NORMAL || back.points.size() < 2)
            return false;
        std::reverse(back.points.begin(), back.points.end());
        if (!firstLegCarried(back.points) || !SafeLeg(back.points) || !WalkBotPoints(bot, back.points))
            return false;
        float const delay = std::clamp(float(IN_MILLISECONDS) * MoveDelay(back.length), 0.0f,
                                       float(sPlayerbotAIConfig.MaxWaitForMove));
        G3D::Vector3 const& end = back.points.back();
        AI_VALUE(LastMovement&, "last movement")
            .Set(bot->GetMapId(), end.x, end.y, end.z, bot->GetOrientation(), delay,
                 MovementPriority::MOVEMENT_NORMAL);
        return logged(Acore::StringFormat("no way out from here: walks the route found from the destination "
                                          "back ({:.0f} yd)",
                                          back.length));
    };
    // [nav163] the core's path first only when it reaches the destination (PATHFIND_NORMAL); a partial one (its
    // 74 / 148 polygon cap: it ends where the straight line is shortest, often a dead end) after the complete route
    // from the destination back. b428 Relara L15, Bierya L15, Sycina L16 on Darnassus' upper terrace (9590-9680,
    // 2480-2580, z 1355-1360), the only way down a stair at its west end: the route back walked them 80 yd west to
    // the stair, the next walk took the core's partial path 102 yd east to the terrace's dead end where they had
    // stood, MOVE_FAR_LOOP gave the Rut'theran trigger up - 151 WALK_FAIL, every quest off Teldrassil given up
    if (coreType & PATHFIND_NORMAL)
    {
        if (walkCore() || walkBack())
            return true;
    }
    else if (walkBack() || walkCore())
        return true;
    // the nearest polygon: the core's search box (PathGenerator::GetPolyByLocation, 3 yd round, 50 yd tall), widened
    // round the bot up to its sight
    dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
    if (!query || bot->IsFlying())
        return false;
    // [guild15] in open water (b333 Eraelor / Bierya, left in the sea off the Rut'theran boat: 400+ WALK_FAIL "no
    // polygon in sight"): the sea's surface has no navmesh; it swims on the surface towards the destination, a sight
    // at a time, until the shore's polygons are under it and the route search walks it on
    {
        Map* map = bot->GetMap();
        LiquidData const liquid =
            map->GetLiquidData(bot->GetPhaseMask(), here.x, here.y, here.z, bot->GetCollisionHeight(), {});
        float const floorZ = map->GetHeight(bot->GetPhaseMask(), here.x, here.y, here.z, true);
        bool const openWater = liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT &&
                               (bot->IsInWater() || PlayerbotAI::HasGroundSupport(bot, here.x, here.y, here.z)) &&
                               (floorZ <= INVALID_HEIGHT || floorZ + bot->GetCollisionHeight() < liquid.Level);
        if (openWater)
        {
            float const toDest = bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
            float const step = std::min(toDest, float(sPlayerbotAIConfig.SightDistance));
            float const heading = bot->GetAngle(dest.GetPositionX(), dest.GetPositionY());
            // the heading to the destination first, then turned off it both ways until the surface ahead is clear
            for (float turn = 0.0f; turn <= float(M_PI) && step > bot->GetCombatReach(); turn += float(M_PI) / 6.0f)
                for (float const side : {1.0f, -1.0f})
                {
                    float const angle = heading + side * turn;
                    float const tx = here.x + std::cos(angle) * step;
                    float const ty = here.y + std::sin(angle) * step;
                    float const tz = liquid.Level;
                    if (!map->isInLineOfSight(here.x, here.y, here.z + bot->GetCollisionHeight(), tx, ty,
                                              tz + bot->GetCollisionHeight(), bot->GetPhaseMask(),
                                              LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                    {
                        if (turn == 0.0f)
                            break;
                        continue;
                    }
                    // [regress10] a stretch the airborne guard refuses (a sample with no liquid or ground in the map
                    // data) is no swim: b347 Shalaeth L1 off Auberdine, every swim to the pier "refused a straight
                    // move over open air" at (6660,667), 141 MAZE / 148 give-ups, 1176 s STUCK. Off its heading then
                    if (!PlayerbotAI::StraightLineSupported(bot, here.x, here.y, here.z, tx, ty, tz))
                        continue;
                    bot->GetMotionMaster()->MovePoint(0, tx, ty, tz, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
                    float const delay = std::clamp(float(IN_MILLISECONDS) * MoveDelay(step), 0.0f,
                                                   float(sPlayerbotAIConfig.MaxWaitForMove));
                    AI_VALUE(LastMovement&, "last movement")
                        .Set(bot->GetMapId(), tx, ty, tz, bot->GetOrientation(), delay,
                             MovementPriority::MOVEMENT_NORMAL);
                    return logged(Acore::StringFormat("in open water (surface z {:.1f}): swims {:.0f} yd {}",
                                                      liquid.Level, step,
                                                      turn == 0.0f ? "towards the destination"
                                                                   : "along the clear water off its heading"));
                }
            logged("in open water: no clear stretch of surface in sight");
            return false;
        }
    }
    if (bot->IsInWater())
        return false;
    dtQueryFilter filter;
    // [stuck15] the steep polygons too: the route search walks them when it finds no complete way without them
    // (Leader::Route's steep pass), so a bot can stand on a ridge reached that way. b355 Sycina L11 walked 240 yd from
    // Shadowglen's inn to (10235,1052,1385) in 43 s - a ridge over Shadowglen, as b308 Bierya L7 at z 1410 - and its
    // island search, blind to the slope it came up, found "no step or survivable fall": 26 STUCK. A step onto the
    // slope is a step its ground carries (carried) like any other
    filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
    filter.setExcludeFlags(uint16(NAV_MAGMA | NAV_SLIME));
    // [guild15] the polygons round the bot: the core's search box (3 yd round, 50 yd tall) at the bot and at points
    // all round it out to its sight (b308 Bierya L7 on a ridge over Shadowglen, z 1410: the one nearest polygon lay
    // under a lethal fall while a lower ledge round it did not)
    struct Way
    {
        G3D::Vector3 to;
        float gap;
        float drop;
        float damage;
        bool carried;
    };
    std::vector<Way> ways;
    bool onPolygon = false;
    {
        // on a polygon already (the core's own box): a navmesh island - only a way whose landing walks on to the
        // destination counts (b310 Lyleria L6 in Darnassus at z 1343: an island of one polygon, the city floor 22 yd
        // off and 15 yd down)
        float const point[3] = {here.y, here.z, here.x};
        float const extents[3] = {3.0f, 50.0f, 3.0f};
        float nearest[3] = {0.0f, 0.0f, 0.0f};
        dtPolyRef ref = 0;
        onPolygon = dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref &&
                    (G3D::Vector3(nearest[2], nearest[0], nearest[1]) - here).length() < bot->GetCombatReach();
    }
    // one search per spot: a spot with no way off is not searched again until the bot has moved. [runpast11] its
    // verdict (an island or not) is kept with it: the caller reads it on every walk from the spot
    static std::mutex islandLock;
    // [stuck15] ... and with the health it had: a fall it survives is one its health now outlives (way.damage <
    // GetHealth()), so a spot searched hurt is searched again once its health changed. b355 Sycina L11 at (10235,1052,
    // 1385): "on a navmesh island" found once, the verdict kept through its rests - 26 STUCK, it waited for its
    // Hearthstone (1750 s) on the spot while every walk off it failed
    static std::unordered_map<uint32, std::pair<std::tuple<uint32, int32, int32, int32, uint32>, bool>> searched;
    std::tuple<uint32, int32, int32, int32, uint32> const spotKey{bot->GetMapId(), int32(here.x), int32(here.y),
                                                                  int32(here.z), bot->GetHealth()};
    // [perf214] the landing look taken up where the last call stopped it, else a new one for this spot
    bool resumed = false;
    {
        std::lock_guard<std::mutex> guard(islandLookLock);
        IslandLook& look = islandLooks[bot->GetGUID().GetCounter()];
        std::tuple<uint32, int32, int32, int32> const lookSpot{bot->GetMapId(), int32(here.x), int32(here.y),
                                                               int32(here.z)};
        if (look.spot != lookSpot || look.dest != DestKey(dest))
        {
            look = IslandLook();
            look.spot = lookSpot;
            look.dest = DestKey(dest);
        }
        resumed = look.pending;
        look.pending = false;
        if (!resumed)
            look.verdicts.clear();  // a new look judges its landings afresh
    }
    {
        std::lock_guard<std::mutex> guard(islandLock);
        auto& last = searched[bot->GetGUID().GetCounter()];
        if (last.first == spotKey && !resumed)
        {
            onIsland = last.second;
            return false;
        }
        last = {spotKey, false};
    }
    auto const noteIsland = [&]()
    {
        NavGap(bot, onPolygon ? "on a navmesh island (its polygons lead nowhere)" : "off the navmesh (no polygon "
                                                                                   "under it)");
        onIsland = true;
        std::lock_guard<std::mutex> guard(islandLock);
        searched[bot->GetGUID().GetCounter()] = {spotKey, true};
    };
    // [deaths] the reach of every idle mob the bot must never fight (SoloSafety::RouteCosts: orange / red, elite,
    // a danger zone, a pack it loses): no landing in it (b337 Salleas L1 and Ryarii L1 jumped 12 yd down off the
    // Dolanaar ledge onto a polygon between two Webwood Lurkers L5-L6 and died there)
    std::vector<G3D::Vector4> refusedReach;
    {
        std::vector<Leader::Travel::RoutePenalty> priced;
        SoloSafety::RouteCosts(bot, here, G3D::Vector3(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()),
                               refusedReach, priced);
        // [runpast11] a refused reach the bot stands in lies behind it, as the route search drops it (Leader::Route):
        // b349 Geron L10 / Namwe L7 on the 34-polygon island at Frostmane Hold (-5542,568), its rim 6.7 yd from the
        // main region - every landing round it lay in the reach of the trolls it stood among, "1 polygons in sight",
        // 1161 s and 117 s STUCK
        refusedReach.erase(std::remove_if(refusedReach.begin(), refusedReach.end(),
                                          [&here](G3D::Vector4 const& c)
                                          { return (c.x - here.x) * (c.x - here.x) + (c.y - here.y) * (c.y - here.y) <=
                                                   c.w * c.w; }),
                           refusedReach.end());
    }
    auto const probe = [&](float x, float y, float reach)
    {
        float const point[3] = {y, here.z, x};
        float const extents[3] = {reach, 50.0f, reach};
        float nearest[3] = {0.0f, 0.0f, 0.0f};
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) || !ref)
            return;
        G3D::Vector3 const to(nearest[2], nearest[0], nearest[1]);
        float const gap = (to - here).length();
        if (gap < bot->GetCombatReach() || gap > sPlayerbotAIConfig.SightDistance)
            return;
        for (Way const& way : ways)
            if ((way.to - to).length() < 1.0f)
                return;
        if (!bot->GetMap()->isInLineOfSight(here.x, here.y, here.z + bot->GetCollisionHeight(), to.x, to.y,
                                            to.z + bot->GetCollisionHeight(), bot->GetPhaseMask(),
                                            LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            return;
        for (G3D::Vector4 const& disc : refusedReach)
            if ((disc.x - to.x) * (disc.x - to.x) + (disc.y - to.y) * (disc.y - to.y) <= disc.w * disc.w)
                return;
        float const drop = here.z - to.z;
        ways.push_back({to, gap, drop, drop > 0.0f ? FallDamageShare(bot, drop) * float(bot->GetMaxHealth()) : 0.0f,
                        carried(here, to)});
    };
    for (float reach = 3.0f; reach <= sPlayerbotAIConfig.SightDistance; reach *= 2.0f)
    {
        probe(here.x, here.y, reach);
        if (reach == 3.0f)
            continue;
        // the ring at this reach: one probe per arc of the box's own width
        uint32 const arcs = std::max<uint32>(8, uint32(std::ceil(2.0f * float(M_PI) * reach / (2.0f * 3.0f))));
        for (uint32 i = 0; i < arcs; ++i)
        {
            float const angle = 2.0f * float(M_PI) * float(i) / float(arcs);
            probe(here.x + std::cos(angle) * reach, here.y + std::sin(angle) * reach, 3.0f);
        }
        if (!onPolygon && std::any_of(ways.begin(), ways.end(), [](Way const& way) { return way.carried; }))
            break;
    }
    // the landing's own walk to the destination: a complete one, or a partial one (a long walk is cut by the
    // search's length) that ends nearer the destination than the bot by more than the step there - an island's own
    // polygons have neither (b311 live: Dandolar, Bierya, Nelaeleas - the long walks were all partial)
    auto const leadsOn = [&](Way const& way)
    {
        BotPath const on = FindBotPath(bot, way.to.x, way.to.y, way.to.z, dest.GetPositionX(), dest.GetPositionY(),
                                       dest.GetPositionZ());
        if (on.type == PATHFIND_NORMAL)
            return true;
        return on.type == PATHFIND_INCOMPLETE && on.points.size() >= 2 &&
               dest.GetExactDist(on.actualEnd.x, on.actualEnd.y, on.actualEnd.z) + way.gap < disToDest;
    };
    // [perf214] a kept verdict, else a new one within the call's search budget (at least one per call); none: the
    // look stops here and the call returns (the walk waits for the look, next tick)
    uint64 const searchesAtStart = Leader::RouteSearchesOnThread();
    uint32 newVerdicts = 0;
    auto const pacedLeadsOn = [&](Way const& way) -> std::optional<bool>
    {
        {
            std::lock_guard<std::mutex> guard(islandLookLock);
            for (auto const& [to, leads] : islandLooks[bot->GetGUID().GetCounter()].verdicts)
                if (to.x == way.to.x && to.y == way.to.y && to.z == way.to.z)
                    return leads;
        }
        if (newVerdicts > 0 &&
            Leader::RouteSearchesOnThread() - searchesAtStart >= ISLAND_LOOK_SEARCHES_PER_TICK)
            return std::nullopt;
        bool const leads = leadsOn(way);
        ++newVerdicts;
        std::lock_guard<std::mutex> guard(islandLookLock);
        islandLooks[bot->GetGUID().GetCounter()].verdicts.emplace_back(way.to, leads);
        return leads;
    };
    auto const pauseLook = [&]()
    {
        std::lock_guard<std::mutex> guard(islandLookLock);
        islandLooks[bot->GetGUID().GetCounter()].pending = true;
        return true;  // the tick is the look's: nothing walked, nothing given up
    };
    if (onPolygon)
    {
        std::vector<Way> kept;
        std::vector<Way const*> order;
        for (Way const& way : ways)
            if (way.carried || (way.drop > 0.0f && way.damage < float(bot->GetHealth())))
                order.push_back(&way);
        std::sort(order.begin(), order.end(),
                  [](Way const* a, Way const* b)
                  {
                      // an island is left by a drop: the falls first (least damage), then the steps (nearest); the
                      // island's own polygons are steps
                      if (a->carried != b->carried)
                          return !a->carried;
                      return a->carried ? a->gap < b->gap : a->damage < b->damage;
                  });
        constexpr size_t PATH_TRIES = 16;  // search cost bound, not a behaviour rule
        // [island32] every landing that leads on (within the tries), not only the first: the order put the falls
        // first, the first leading one was a 94 yd "fall" past a jump's reach and the 2.4 yd step that also led on
        // was never looked at (b384 / b385: 9-33 alarms at the Dolanaar inn's 17-polygon island, 2.4 yd off the
        // main region at the same height)
        for (size_t i = 0; i < order.size() && i < PATH_TRIES; ++i)
        {
            std::optional<bool> const leads = pacedLeadsOn(*order[i]);  // [perf214]
            if (!leads)
                return pauseLook();
            if (*leads)
                kept.push_back(*order[i]);
        }
        if (kept.empty())
        {
            // [deaths] user 2026-11-08: the main region is no island - the bot's own region round it (Detour's
            // findPolysAroundCircle within its sight): when it reaches past its sight, or fills the search, the walk
            // is a dead end of the route (the maze search takes it: WalkAround), not an island
            bool reachesOut = false;
            {
                float const point[3] = {here.y, here.z, here.x};
                float const extents[3] = {3.0f, 50.0f, 3.0f};
                float nearest[3] = {0.0f, 0.0f, 0.0f};
                dtPolyRef start = 0;
                dtNavMesh const* navMesh = query->getAttachedNavMesh();
                if (navMesh && dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &start, nearest)) &&
                    start)
                {
                    constexpr int ISLAND_POLYS = 256;  // search bound: a region that fills it is no island
                    std::vector<dtPolyRef> region(ISLAND_POLYS);
                    int count = 0;
                    dtStatus const status =
                        query->findPolysAroundCircle(start, nearest, sPlayerbotAIConfig.SightDistance, &filter,
                                                     region.data(), nullptr, nullptr, &count, ISLAND_POLYS);
                    reachesOut = dtStatusSucceed(status) && (dtStatusDetail(status, DT_BUFFER_TOO_SMALL) ||
                                                             dtStatusDetail(status, DT_OUT_OF_NODES));
                    for (int i = 0; !reachesOut && i < count; ++i)
                    {
                        dtMeshTile const* tile = nullptr;
                        dtPoly const* poly = nullptr;
                        if (dtStatusFailed(navMesh->getTileAndPolyByRef(region[size_t(i)], &tile, &poly)) || !tile ||
                            !poly)
                            continue;
                        for (uint8 v = 0; v < poly->vertCount; ++v)
                        {
                            float const* vertex = &tile->verts[size_t(poly->verts[v]) * 3];
                            if (std::hypot(vertex[2] - here.x, vertex[0] - here.y) >= sPlayerbotAIConfig.SightDistance)
                                reachesOut = true;
                        }
                    }
                }
            }
            if (reachesOut)
            {
                logged(Acore::StringFormat("a dead end of its route, not an island: its region reaches past its "
                                           "sight ({} polygons in sight lead nowhere nearer)",
                                           ways.size()));
                return false;
            }
            // [stuck15] the falls its full health outlives but its health now does not: it rests, then searches again
            uint32 atFull = 0;
            for (Way const& way : ways)
                if (!way.carried && way.drop > 0.0f && way.damage >= float(bot->GetHealth()) &&
                    way.damage < float(bot->GetMaxHealth()))
                    ++atFull;
            logged(Acore::StringFormat("on a navmesh island: {} polygons in sight, no step or survivable fall leads on",
                                       ways.size()));
            if (atFull)
                LOG_INFO("playerbots", "[stuck15] {} L{} on an island: {} falls off it need more health than its {} "
                         "of {}: searched again once its health changed",
                         bot->GetName(), bot->GetLevel(), atFull, bot->GetHealth(), bot->GetMaxHealth());
            noteIsland();  // [runpast11]
            return false;
        }
        ways = kept;
    }
    if (ways.empty())
    {
        logged("off the navmesh, no polygon in sight");
        // [stuck12] no polygon round it within its sight that it can step or fall to: no walk from here at all -
        // the caller takes its Hearthstone out (b350 Blahadrin L13 at (-9463,-350,60), 2.5 yd under the Elwynn
        // navmesh with no line of sight to it: 148 STUCK alerts, every walk "no route (type 0x11)" for the whole run)
        noteIsland();
        return false;
    }
    // [runpast11] off the navmesh: a landing whose walk leads on to the destination first (b349 Thyllyn L12 jumped
    // off a Darnassus ledge onto the nearest polygon at (10091,2581) - a 16-polygon island, 971 s STUCK).
    // [nav24] user 2026-11-09: only such a landing - one with a walk on to the destination. Before, "none leads on:
    // the nearest landing still puts it on the navmesh": Thyllyn's island, Geron / Namwe in Frostmane Hold, Ginde on
    // the Elwynn river bank, the Fargodeep gallery. None: the [navgap] alarm and a help request, no move
    if (!onPolygon)
    {
        std::vector<Way> leading;
        std::vector<Way const*> order;
        for (Way const& way : ways)
            if (way.carried || (way.drop > 0.0f && way.damage < float(bot->GetHealth())))
                order.push_back(&way);
        std::sort(order.begin(), order.end(), [](Way const* a, Way const* b) { return a->gap < b->gap; });
        constexpr size_t PATH_TRIES = 16;  // search cost bound, not a behaviour rule
        for (size_t i = 0; i < order.size() && i < PATH_TRIES && leading.empty(); ++i)
        {
            std::optional<bool> const leads = pacedLeadsOn(*order[i]);  // [perf214]
            if (!leads)
                return pauseLook();
            if (*leads)
                leading.push_back(*order[i]);
        }
        if (leading.empty())
        {
            NavGap(bot, "off the navmesh: no landing in sight has a walk on to its destination");
            if (dest.GetMapId() == bot->GetMapId())
                NewRpgCoop::OnCampRefused(botAI, bot, dest, "off the navmesh: no landing leads on");
            logged(Acore::StringFormat("[nav24] off the navmesh: {} landings in sight, none with a walk on to "
                                       "({:.0f},{:.0f}): no step, no jump",
                                       ways.size(), dest.GetPositionX(), dest.GetPositionY()));
            return false;
        }
        ways = leading;
    }
    // [steep29] on a polygon: a landing its own region joins (Detour's corridor with the filter it walks - steep
    // slopes included, as the core's headless-bot filter) is walked over the navmesh, not stepped or jumped to.
    // b378 Thokhen L15 (x7), Brelir, Keenonk, Feette on the Barrens' slopes (1094,-1282,213): on a steep polygon of
    // Kalimdor's main region, "off the navmesh: the landing 96 yd off is past a jump's reach (6 yd)", WALK_FAIL
    if (onPolygon)
    {
        // [steep32] the corridor by the long route search (its own node pool, steep slopes included): the map's
        // query holds 1024 nodes, a 96 yd corridor over the small polygons of a slope ran out of them and came back
        // partial - steep29 never walked (b385: 15 Barrens alarms, Feette at (1102,-1302,216), "landing 96 yd off").
        // The landings nearest first; the walk is the corridor's corners
        std::vector<Way const*> byGap;
        for (Way const& way : ways)
            byGap.push_back(&way);
        std::sort(byGap.begin(), byGap.end(), [](Way const* a, Way const* b) { return a->gap < b->gap; });
        constexpr size_t CORRIDOR_TRIES = 8;  // search cost bound, not a behaviour rule
        for (size_t i = 0; i < byGap.size() && i < CORRIDOR_TRIES; ++i)
        {
            Way const& way = *byGap[i];
            Movement::PointsArray corners;
            bool complete = false;
            if (!Leader::Travel::LongRoute(bot, here, way.to, true, corners, complete) || !complete ||
                corners.size() < 2)
                continue;
            if (!WalkBotPoints(bot, corners))
                continue;
            LOG_INFO("playerbots", "[steep29] {} L{} at ({:.1f},{:.1f},{:.1f}): walks the navmesh {:.0f} yd to the "
                     "landing ({:.1f},{:.1f},{:.1f}) its region joins ({} corners)",
                     bot->GetName(), bot->GetLevel(), here.x, here.y, here.z, way.gap, way.to.x, way.to.y, way.to.z,
                     corners.size());
            return logged(Acore::StringFormat("[steep29] on the navmesh: walks {:.0f} yd to the landing its region "
                                              "joins",
                                              way.gap));
        }
    }
    // a step the ground carries, the nearest; else a jump down the bot survives, the least fall damage
    Way const* best = nullptr;
    for (Way const& way : ways)
        if (way.carried && (!best || way.gap < best->gap))
            best = &way;
    if (best)
    {
        bot->GetMotionMaster()->MovePoint(0, best->to.x, best->to.y, best->to.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          false, true);
        return logged(Acore::StringFormat("off the navmesh: steps onto the nearest polygon ({:.1f},{:.1f},{:.1f}) "
                                          "{:.0f} yd off",
                                          best->to.x, best->to.y, best->to.z, best->gap));
    }
    // [island32] a jump's reach (the run speed over the core jump's air time from its drop): a landing past it is
    // no jump - the least damage of those within it, else the nearest landing (refused below with the alarm)
    float const jumpSpeed = bot->GetSpeed(MOVE_RUN);
    auto const jumpReach = [&](Way const& way)
    {
        float const g = float(Movement::gravity);
        float const air = (jumpSpeed + std::sqrt(jumpSpeed * jumpSpeed + 2.0f * g * std::max(way.drop, 0.0f))) / g;
        return jumpSpeed * air;
    };
    for (Way const& way : ways)
        if (way.drop > 0.0f && way.damage < float(bot->GetHealth()) && way.gap <= jumpReach(way) &&
            (!best || way.damage < best->damage || (way.damage == best->damage && way.gap < best->gap)))
            best = &way;
    if (!best)
        for (Way const& way : ways)
            if (way.drop > 0.0f && way.damage < float(bot->GetHealth()) && (!best || way.gap < best->gap))
                best = &way;
    if (!best)
    {
        Way const& first = ways.front();
        logged(Acore::StringFormat("off the navmesh: {} polygons in sight, none a step or a fall it survives (the "
                                   "first {:.0f} yd {} over open air, {:.0f} damage at {} health)",
                                   ways.size(), std::fabs(first.drop), first.drop > 0.0f ? "down" : "up", first.damage,
                                   bot->GetHealth()));
        return false;
    }
    // [nav24] a real jump: its reach is the run speed over the air time of the core's jump from the drop's height
    // (MoveJump's speedZ, gravity Movement::gravity); b349-b372 logged 96-98 yd "jumps" (a long glide)
    float const speedXY = bot->GetSpeed(MOVE_RUN);
    {
        float const g = float(Movement::gravity);
        float const up = speedXY;  // the jump's vertical speed passed to MoveJump below
        float const air = (up + std::sqrt(up * up + 2.0f * g * std::max(best->drop, 0.0f))) / g;
        if (best->gap > speedXY * air)
        {
            // [steep29] the wording: a bot on a polygon stands on a navmesh island, not off the navmesh
            char const* const where = onPolygon ? "on a navmesh island" : "off the navmesh";
            NavGap(bot, Acore::StringFormat("{}: the landing {:.0f} yd off is past a jump's reach ({:.0f} yd)", where,
                                            best->gap, speedXY * air));
            // [island32] what it had to choose from: the nearest landings, each its gap, drop, step and lead-on
            {
                std::vector<Way const*> closeWays;
                for (Way const& way : ways)
                    closeWays.push_back(&way);
                std::sort(closeWays.begin(), closeWays.end(), [](Way const* a, Way const* b) { return a->gap < b->gap; });
                std::string list;
                for (size_t i = 0; i < closeWays.size() && i < 4; ++i)
                    list += Acore::StringFormat("{}({:.1f},{:.1f},{:.1f}) {:.1f} yd, drop {:.1f}, {}", list.empty() ? "" : "; ",
                                                closeWays[i]->to.x, closeWays[i]->to.y, closeWays[i]->to.z, closeWays[i]->gap,
                                                closeWays[i]->drop, closeWays[i]->carried ? "a step" : "no step");
                LOG_INFO("playerbots", "[island32] {} L{} at ({:.1f},{:.1f},{:.1f}): {} landings that lead on, nearest: "
                         "{}",
                         bot->GetName(), bot->GetLevel(), here.x, here.y, here.z, ways.size(), list);
            }
            logged(Acore::StringFormat("[nav24] {}: the landing ({:.1f},{:.1f},{:.1f}) {:.0f} yd off is past a jump's "
                                       "{:.0f} yd: no jump",
                                       where, best->to.x, best->to.y, best->to.z, best->gap, speedXY * air));
            return false;
        }
    }
    // a player jumps down: the core's jump arc to the polygon, the fall damage the core's formula gives
    bot->GetMotionMaster()->MoveJump(best->to.x, best->to.y, best->to.z, speedXY, speedXY);
    if (uint32 const fallDamage = uint32(best->damage))
        bot->EnvironmentalDamage(DAMAGE_FALL, fallDamage);
    return logged(Acore::StringFormat("off the navmesh: jumps down {:.0f} yd onto a polygon ({:.1f},{:.1f},{:.1f}) "
                                      "{:.0f} yd off, fall damage {}",
                                      best->drop, best->to.x, best->to.y, best->to.z, best->gap,
                                      uint32(best->damage)));
}

// [guild15] the walk's points up to `length` along them: a stretch that descends more than PlayerbotAI's
// GROUND_SUPPORT_DROP (6 yd) with no ground or liquid within that drop under a point of it (HasGroundSupport, every
// GROUND_SAMPLE_STEP = 2 yd, the airborne spline guard's own sampling); rim: the last supported point
static bool LegOverOpenAir(Player* bot, Movement::PointsArray const& points, float length, G3D::Vector3& rim)
{
    constexpr float SUPPORT_DROP = 6.0f;  // PlayerbotAI.cpp GROUND_SUPPORT_DROP
    constexpr float SAMPLE_STEP = 2.0f;   // PlayerbotAI.cpp GROUND_SAMPLE_STEP
    float walked = 0.0f;
    for (size_t i = 1; i < points.size() && walked < length; ++i)
    {
        G3D::Vector3 const& from = points[i - 1];
        G3D::Vector3 const& to = points[i];
        float const gap = (to - from).length();
        if (from.z - to.z > SUPPORT_DROP && gap > 0.0f)
        {
            rim = from;
            for (float at = SAMPLE_STEP; at < gap; at += SAMPLE_STEP)
            {
                G3D::Vector3 const point = from + (to - from) * (at / gap);
                if (!PlayerbotAI::HasGroundSupport(bot, point.x, point.y, point.z))
                    return true;
                rim = point;
            }
        }
        walked += gap;
    }
    return false;
}

bool NewRpgBaseAction::MoveFarToDirect(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // Don't start a ground move while the bot is on a taxi: the MovePoint issued
    // below would fight the active flight spline. Returning true consumes the tick
    // instead of returning false, because the callers of MoveFarTo fall through to
    // MoveRandomNear, which is also a ground move.
    if (bot->IsInFlight())
        return true;

    bool newDest = dest != botAI->rpgInfo.moveFarPos;
    if (newDest)
    {
        // clear stuck information if it's a new dest
        botAI->rpgInfo.SetMoveFarTo(dest);
        // [nav165] where this walk starts
        botAI->rpgInfo.moveFarFrom = G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        botAI->rpgInfo.moveFarFromSet = true;
        // [guild15] b317 Eraelor "route complete (335 yd, ends 0 yd short), the walk refused: IsWaitingForLastMove":
        // a new destination replaces the last walk; the old walk's clock no longer holds the new one (lead244)
        LastMovement& last = AI_VALUE(LastMovement&, "last movement");
        last.lastdelayTime = 0.0f;
        last.msTime = 0;
    }

    // performance optimization: the last walk's clock (its distance / speed estimate) runs
    // [holds9] b345: 61 HOLDs "IsWaitingForLastMove" with the bot standing, and this return gave no move and no
    // reason. The clock is an estimate of the walk, not the walk: a walk under way uses the tick (true); a deliberate
    // wait (ForceToWait: the group wait, a patrol's window, each logged) holds it (true); a clock of a walk whose
    // spline already ended (it stopped short of the estimate: a path end nearer than asked, a fight, a stop) is over -
    // the walk is searched again from where the bot stands
    if (!newDest && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        LastMovement& last = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() || last.msTime == botAI->rpgInfo.waitClockMs)
            return true;
        last.lastdelayTime = 0.0f;
        last.msTime = 0;
    }

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove
    // (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s
    // IsWaitingForLastMove returns false and MoveFarTo re-enters.
    // Without this gate, DoMovePoint would call mm->Clear() and
    // reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the
    // bot gets clobbered mid-walk and ends up oscillating (e.g.
    // cave entrance -> inside cave -> cave entrance -> mountain
    // base -> cave entrance...) around an unreachable destination.
    //
    // If the bot is still actively walking toward its last
    // committed point on the same map, just let the current spline
    // finish. The stuck counter below continues to track real
    // progress toward dest and triggers teleport recovery if the
    // committed paths genuinely aren't closing the gap.
    //
    // A changed destination (e.g. the gather state switching to a
    // live node it passes, or a quest POI change) skips this gate:
    // letting the old spline finish would keep walking the bot
    // toward a target it no longer has.
    if (!newDest)
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // [nav165] a pocket's deep search runs while the bot stands, then its way out is walked first (before the stuck
    // check: standing for the search proves nothing about the walk)
    if (PocketRescue())
        return true;

    // [runpast15] every search below is for a stage or leg of the walk to `dest`: a run past is measured to it
    // (another map's destination: no stop on this map - each search is measured to its own end)
    std::optional<SoloSafety::WalkStopScope> walkStop;
    if (dest.GetMapId() == bot->GetMapId())
        walkStop.emplace(G3D::Vector3(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()));

    // stuck check
    float disToDest = bot->GetDistance(dest);
    // Require a meaningful improvement (5yd) to reset the stuck counter.
    // The old 1yd threshold was small enough that bots oscillating back
    // and forth around an obstacle would keep "making progress" forever
    // and never trigger the teleport recovery below.
    // [timers] no stuck timer: a walk issued again from a spot within MOVE_FAR_PROGRESS of a spot an earlier walk
    // started from without getting closer has closed a loop (or the bot stands where it stood) - no walk from here
    // gets it closer; that is the proof the destination is out of walk
    bool loopClosed = false;
    if (disToDest + MOVE_FAR_PROGRESS < botAI->rpgInfo.nearestMoveFarDis)
    {
        botAI->rpgInfo.nearestMoveFarDis = disToDest;
        botAI->rpgInfo.stuckSpots.clear();
    }
    // [perf214] ... nor does one standing for its landing look, paced over ticks (IslandLookPending)
    else if (IsMovingAllowed() && !bot->IsNonMeleeSpellCast(false) && !IslandLookPending(bot, dest))
    {
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        for (G3D::Vector3 const& spot : botAI->rpgInfo.stuckSpots)
            if ((spot - here).squaredLength() < MOVE_FAR_PROGRESS * MOVE_FAR_PROGRESS)
                loopClosed = true;
        if (!loopClosed)
            botAI->rpgInfo.stuckSpots.push_back(here);
    }
    if (loopClosed)
    {
        LOG_INFO("playerbots", "[rpg] {} L{} MOVE_FAR_LOOP reason: back at a spot its walk to ({:.0f},{:.0f},{:.0f}) "
                 "left without getting closer (best {:.0f} yd, now {:.0f} yd, {} spots)",
                 bot->GetName(), bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                 botAI->rpgInfo.nearestMoveFarDis, disToDest, botAI->rpgInfo.stuckSpots.size());
        botAI->rpgInfo.stuckSpots.clear();

        // Player-owned bots play like players: no teleport shortcut. Give the goal up so the planner picks
        // another one (the quest drops to low priority, see the callers' fallbacks).
        // [honest] a guild-plan bot is never teleported: it gives the goal up as a player does
        if (!sRandomPlayerbotMgr.IsRandomBot(bot) || !sPlayerbotAIConfig.RandomBotTeleport ||
            GuildHonest::IsHonestBot(bot))  // [guild1]
        {
            LOG_DEBUG("playerbots", "[New RPG] {} gives up an unreachable destination ({},{},{},{})", bot->GetName(),
                      dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId());
            // [wait19] a meet walk given up is no quest's walk
            if (auto* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
                quest && !botAI->rpgInfo.meetWalk)
            {
                botAI->lowPriorityQuest.insert(quest->questId);

                // [wall17] coordinator 2026-11-09: a quest walk given up behind mobs it never fights ([guild12]
                // orange / red, elites) asks for a party as a refused spot does, not a silent postpone
                if (std::string const con = SoloSafety::ConWalls(bot); !con.empty())
                {
                    LOG_INFO("playerbots", "[wall17] {} L{} q{} walled by {}: asks for help", bot->GetName(),
                             bot->GetLevel(), quest->questId, con);
                    NewRpgCoop::OnSpotRefused(botAI, bot, quest->questId, quest->objectiveIdx, dest);
                }
            }


            // [holds9] b345 Ellosera, Feeli, Pherua: the give-up went back to IDLE, the choice took the same spot
            // again (LOOP alerts) and the caller logged a HOLD with no reason: the spot is remembered as out of walk
            // for the choices (TripRefused) until the next level, and the state change uses the tick
            NoteWalkGivenUp(bot, dest, "no walk from here gets it closer (MOVE_FAR_LOOP)");
            GiveUpWalkTargets(dest, "no walk from here gets it closer (MOVE_FAR_LOOP)");  // [grind25]
            botAI->rpgInfo.ChangeToIdle();
            // [travel23] it stands where the navmesh has no polygon: no walk from here goes anywhere, every
            // destination is given up in turn. b372 Januel L12 at (0,1551,-69) on map 369, 26 yd over the tunnel floor
            // (the tram walked, not ridden: [tram23]). [navgap] user 2026-11-09: the alarm first (the gap is repaired
            // at its root - the tile, the transport line); the Hearthstone only as the logged last resort after it
            if (dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
            {
                dtQueryFilter filter;
                filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
                float const point[3] = {bot->GetPositionY(), bot->GetPositionZ(), bot->GetPositionX()};
                float const extents[3] = {3.0f, 5.0f, 3.0f};  // the core's polygon search box round a unit, short
                float nearest[3] = {0.0f, 0.0f, 0.0f};
                dtPolyRef ref = 0;
                bool const onMesh = dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) &&
                                    ref;
                if (!onMesh && !bot->IsInWater() && !bot->GetTransport())
                {
                    NavGap(bot, "MOVE_FAR_LOOP: every walk given up, no polygon under it");
                    LOG_INFO("playerbots", "[travel23] {} L{} last resort after the [navgap] alarm: its Hearthstone",
                             bot->GetName(), bot->GetLevel());
                }
                if (!onMesh && !bot->IsInWater() && !bot->GetTransport())
                    HearthOut(Acore::StringFormat("off the navmesh at ({:.0f},{:.0f},{:.0f}) on map {}: no walk from "
                                                  "here gets anywhere",
                                                  bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                                  bot->GetMapId()));
            }
            return true;
        }

        AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        std::string zone_name = PlayerbotAI::GetLocalizedAreaName(entry);
        LOG_DEBUG(
            "playerbots",
            "[New RPG] Teleport {} from ({},{},{},{}) to ({},{},{},{}) as it stuck when moving far - Zone: {} ({})",
            bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId(), bot->GetZoneId(),
            zone_name);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        return bot->TeleportTo(dest);
    }

    // (after the stuck check above: a bot that cannot get on still gives the destination up)
    // [guild15] perf (b317 pmon: "new rpg do profession" 31 ms per call, max 1.4 s; "new rpg do quest" max 1.1 s):
    // the route search that found no walk from this spot is not asked again from the same spot - only after the bot
    // stands elsewhere (its random step, a fight, a fall) or the destination changes
    if (botAI->rpgInfo.moveFarFailed &&
        bot->GetExactDist(botAI->rpgInfo.failedFromX, botAI->rpgInfo.failedFromY, botAI->rpgInfo.failedFromZ) < 3.0f)
    {
        s_moveRefusal = "the route search found no walk from this spot";  // [holds9]
        return false;
    }
    botAI->rpgInfo.moveFarFailed = false;
    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        if (MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                   false, true))
            return true;
        if (!s_moveRefusal.empty() && s_moveRefusal.find("IsWaitingForLastMove") == std::string::npos &&
            s_moveRefusal.find("IsDuplicateMove") == std::string::npos &&
            s_moveRefusal.find("IsMovingAllowed") == std::string::npos)
        {
            NoteWalkFailure(dest, "near walk refused: " + s_moveRefusal);  // [guild13]
            // [guild15] a gather node the walk's own guards refuse (straight under or over the bot: under water, in
            // a cave below - b333 Irrua, Hobliir, Mutil, Thyllyn): one refusal is the fact, the next node at once
            if (auto* gather = std::get_if<NewRpgInfo::DoGather>(&botAI->rpgInfo.data);
                gather && gather->nodeSpawnId && gather->nodePos == dest)
            {
                LOG_INFO("playerbots", "[guild20] {} L{} TARGET_SWITCH gather node {}: the near walk is refused ({}), "
                         "the next node",
                         bot->GetName(), bot->GetLevel(), gather->nodeSpawnId, s_moveRefusal);
                gather->visited.insert(gather->nodeSpawnId);
                gather->nodeSpawnId = 0;
                gather->lastReach = 0;
                return true;
            }
        }
        return false;
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination.
    // If mmap can reach it directly (PATHFIND_NORMAL) or partially
    // (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap
    // of ~296 yards, or where local geometry blocks the final step),
    // walk to the furthest reachable waypoint mmap computed. This
    // lets bots follow the real route around obstacles (mountains,
    // cave walls, cliffs) instead of trying to cut straight through.
    // The spline system walks the whole returned path smoothly, so
    // subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    // [deaths] a maze search's via spot for this destination: walked to first, then the destination's walk again
    if (SoloSafety::Applies(bot))
    {
        bool hasVia = false;
        G3D::Vector3 via;
        {
            std::lock_guard<std::mutex> guard(walkAroundLock);
            auto const found = walkAround.find(bot->GetGUID().GetCounter());
            if (found != walkAround.end() && found->second.hasVia && found->second.dest == DestKey(dest))
            {
                hasVia = true;
                via = found->second.via;
            }
        }
        if (hasVia)
        {
            BotPath const leg = bot->GetExactDist(via.x, via.y, via.z) > MOVE_FAR_PROGRESS
                                    ? FindBotPath(bot, via.x, via.y, via.z)
                                    : BotPath();
            if (leg.type == PATHFIND_NORMAL && !leg.throughRefused && leg.points.size() >= 2)
            {
                G3D::Vector3 const end = leg.length > sPlayerbotAIConfig.SightDistance
                                             ? Leader::Travel::PointOnPath(leg.points, sPlayerbotAIConfig.SightDistance)
                                             : leg.actualEnd;
                if (MoveTo(bot->GetMapId(), end.x, end.y, end.z, false, false, false, true))
                {
                    CommitLeg(bot, leg.points, end);  // [runpast11]
                    return true;
                }
            }
            std::lock_guard<std::mutex> guard(walkAroundLock);
            walkAround[bot->GetGUID().GetCounter()].hasVia = false;  // reached, or no clean walk to it from here
        }
    }
    // [guild15] perf (b319 pmon: the leader route search was 78 % of all bot AI time, 4.9 ms a search, mostly the
    // far walks' re-plan of the whole way after every leg): on a complete route found before, the next leg is
    // searched only as far as two sights ahead along it - the mobs priced are the ones in sight either way
    if (SoloSafety::Applies(bot) && botAI->rpgInfo.farRoute.size() >= 2)
    {
        std::vector<G3D::Vector3> const& route = botAI->rpgInfo.farRoute;
        G3D::Vector3 const at(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        size_t nearest = route.size();
        float best = 8.0f * 8.0f;
        for (size_t i = 0; i < route.size(); ++i)
            if (float const d = (route[i] - at).squaredLength(); d < best)
            {
                best = d;
                nearest = i;
            }
        float const ahead = 2.0f * sPlayerbotAIConfig.SightDistance;
        if (nearest < route.size())
        {
            Movement::PointsArray rest(route.begin() + nearest, route.end());
            float restLength = 0.0f;
            for (size_t i = 1; i < rest.size(); ++i)
                restLength += (rest[i] - rest[i - 1]).length();
            if (restLength > ahead)
            {
                G3D::Vector3 const target = Leader::Travel::PointOnPath(rest, ahead);
                BotPath const local = FindBotPath(bot, target.x, target.y, target.z);
                if (local.type == PATHFIND_NORMAL && !local.throughRefused &&
                    (local.actualEnd - target).length() < 5.0f && local.points.size() >= 2)
                {
                    G3D::Vector3 leg = local.length > sPlayerbotAIConfig.SightDistance
                                           ? Leader::Travel::PointOnPath(local.points, sPlayerbotAIConfig.SightDistance)
                                           : local.actualEnd;
                    if (MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z, false, false, false, true))
                    {
                        CommitLeg(bot, local.points, leg);  // [runpast11]
                        return true;
                    }
                }
            }
        }
        botAI->rpgInfo.farRoute.clear();  // off the route, at its end, or the local leg failed: the whole way again
    }
    {
        BotPath const path =  // [lead109]
            FindBotPath(bot, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        if (path.type == PATHFIND_NORMAL && path.leaderRoute && !path.throughRefused)  // [guild15] the next legs
            botAI->rpgInfo.farRoute.assign(path.points.begin(), path.points.end());
        // [deaths] user 2026-11-08: a solo walk never goes through a pack it does not fight (b338 Sylaenas L1:
        // "complete, through a refused reach", killed by the Webwood Lurker there) and a long one goes in stages: the
        // maze search
        // [party75] ... a co-op / help party leader's walk too, when every way passes a pack the party loses: the same
        // maze search and refused-walk handling (another way, the patrol's window, else postponed with its help
        // asked) - never walked through it
        bool const partyWalled = path.throughRefused && NewRpgCoop::JudgesPulls(bot) && !bot->GetMap()->Instanceable();
        if ((SoloSafety::Applies(bot) || partyWalled) && path.leaderRoute &&
            (path.throughRefused ||
             (path.type == PATHFIND_INCOMPLETE && disToDest > 2.0f * sPlayerbotAIConfig.SightDistance)))
        {
            if (WalkAround(dest, path, disToDest, false))
                return true;
            if (path.throughRefused)
            {
                if (s_moveRefusal.empty())  // [pull9] the maze search's own reason when it set one
                    s_moveRefusal = "every way there passes mobs it does not pull and the maze search's wait for the "
                                    "patrol did not start";
                return false;
            }
        }
        PathType type = path.type;
        bool canReach = !(type & (~typeOk));
        if (canReach)
        {
            G3D::Vector3 const& endPos = path.actualEnd;
            // Only commit if the mmap endpoint actually makes progress
            // toward the destination. For pathological INCOMPLETE
            // results (e.g. disconnected polys that still report
            // INCOMPLETE) the endpoint can land right under the bot;
            // fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                // [guild13] a solo bot commits the walk only as far as it sees: the idle mobs beyond are priced when
                // it gets there (the route search penalties are of the mobs in sight) - b299: 13 of 14 deaths were
                // mobs met on a walk planned through a camp the bot had not seen yet
                G3D::Vector3 leg = endPos;
                // [b381] a path with a length but no points (the v21 core) keeps its end: b380 crash in PointOnPath
                if (SoloSafety::Applies(bot) && path.length > sPlayerbotAIConfig.SightDistance &&
                    path.points.size() >= 2)
                    leg = Leader::Travel::PointOnPath(path.points, sPlayerbotAIConfig.SightDistance);
                // [guild15] a stretch of the leg that drops through open air (no ground or liquid under it, sampled as
                // PlayerbotAI::GuardAirborneSpline samples a descending leg): the guard cuts that walk at the rim on
                // every try (b333 Hobliir, Ellosera at the Exodar: "refused a straight move over open air", STUCK);
                // the ways off from here are searched instead (the core's path, the route back, a survivable jump)
                if (G3D::Vector3 rim; LegOverOpenAir(bot, path.points,
                                                       SoloSafety::Applies(bot) ? sPlayerbotAIConfig.SightDistance
                                                                                : path.length,
                                                       rim))
                {
                    NoteWalkFailure(dest, Acore::StringFormat("route {} ({:.0f} yd): its leg drops through open air at "
                                                              "({:.1f},{:.1f},{:.1f})",
                                                              type == PATHFIND_NORMAL ? "complete" : "partial",
                                                              path.length, rim.x, rim.y, rim.z));
                    if (WalkOffIsland(dest, disToDest))
                        return true;
                    NoteFarSearchFailed();
                    return false;
                }
                if (MoveTo(bot->GetMapId(), leg.x, leg.y, leg.z, false, false, false, true))
                {
                    CommitLeg(bot, path.points, leg);  // [runpast11]
                    return true;
                }
                NoteWalkFailure(dest, Acore::StringFormat("route {} ({:.0f} yd, ends {:.0f} yd short), the walk "
                                                          "refused: {}",
                                                          type == PATHFIND_NORMAL ? "complete" : "partial", path.length,
                                                          endDistToDest, s_moveRefusal));
                return false;
            }
            // [guild13] the search from where the bot stands finds no way out (a navmesh island: Aldrassil's upper
            // floor, the Exodar's halls): the same search from the destination back to the bot; a complete one is
            // walked the other way
            // the core's own path search first (its start polygon from the bot's small box, not the route's tall
            // column, which can take the roof over a floor)
            if (WalkOffIsland(dest, disToDest))  // [guild14]
                return true;
            NoteWalkFailure(dest, Acore::StringFormat("route {} ends {:.0f} yd short, no nearer than the bot",
                                                      type == PATHFIND_NORMAL ? "complete" : "partial",
                                                      endDistToDest));
            // [runpast11] on a navmesh island no walk from here reaches anything: the target is not the dead end, the
            // spot is - its Hearthstone out (b349: all 31 "dead end" postpones were Geron, Namwe and Thyllyn on
            // islands; 135 + 22 + 21 maze give-ups, every quest of their logs postponed in turn)
            if (onIsland)
                NavGap(bot, "on a navmesh island: no walk off it");  // [navgap] the alarm before the last resort
            if (onIsland &&
                HearthOut(Acore::StringFormat("on a navmesh island at ({:.0f},{:.0f},{:.0f}): no walk off it",
                                              bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ())))
                return true;
            // [deaths] user 2026-11-08: no walking into a dead end - the maze search (another way, else another task)
            if (SoloSafety::Applies(bot) && path.leaderRoute && WalkAround(dest, path, disToDest, true))
                return true;
            NoteFarSearchFailed();  // [guild15]
        }
        else
        {
            // [loop70] no route because every way passes an area red for it, an enemy capital or static spawns it
            // must not fight: a refused walk (its quest postponed, its help asked), not a navmesh gap - the core's
            // path fallback walked it on toward the refused area a leg at a time and back (b406 Orenem L15: a 4211 yd
            // giver walk past Fallen Sky Lake, area level 26 - three "walks the core's path" legs, a MOVE_FAR_LOOP
            // after 499 s)
            if (SoloSafety::Applies(bot) && path.refusedArea)
            {
                LOG_INFO("playerbots", "[loop70] {} L{} walk to ({:.0f},{:.0f}) {:.0f} yd refused: every way passes "
                         "an area or static spawns it must not walk through",
                         bot->GetName(), bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY(), disToDest);
                RefusedWalk(dest, "every way there passes an area or spawns it must not walk through (route search)");
                return false;
            }
            NoteWalkFailure(dest, Acore::StringFormat("no route (type {:#x})", uint32(type)));
            NoteFarSearchFailed();  // [guild15]
            // [guild14] the bot stands off the navmesh (PathGenerator's shortcut, NOT_USING_PATH: b303 Manue L7 on a
            // ledge over Elwynn, 20 STUCK alerts): the same ways off the island
            if (WalkOffIsland(dest, disToDest))
                return true;
            // [stuck12] no way off the spot it stands on: its Hearthstone out, as on an island (above) - [navgap] only
            // as the last resort after the alarm
            if (onIsland)
                NavGap(bot, "off the navmesh: no polygon it can step or fall to in sight");
            if (onIsland &&
                HearthOut(Acore::StringFormat("off the navmesh at ({:.0f},{:.0f},{:.0f}): no polygon it can step or "
                                              "fall to in sight",
                                              bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ())))
                return true;
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the
    // forward cone for a reachable stepping stone so the bot keeps
    // moving and can try again from a new vantage point. Cap at 2
    // samples — we already spent one PathGenerator call above and at
    // 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        BotPath const path = FindBotPath(bot, dx, dy, dz);  // [lead109]
        PathType type = path.type;
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            G3D::Vector3 const& endPos = path.actualEnd;
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
    }
    if (s_moveRefusal.empty())
        s_moveRefusal = "no route there and no reachable step in its forward cone";  // [holds9]
    return false;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    // never walk off a boat or zeppelin at sea
    if (bot->GetTransport() && (bot->GetTransport()->IsMotionTransport() || RidesTramLine(bot)))  // [tram23]
        return false;

    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    float z = object->GetPositionZ();
    float mapId = object->GetMapId();
    float angle = 0.f;

    if (!object->ToUnit() || !object->ToUnit()->isMoving())
        angle = object->GetAngle(bot) + (M_PI * irand(-25, 25) / 100.0);  // Closest 45 degrees towards the target
    else
        angle = object->GetOrientation() +
                (M_PI * irand(-25, 25) / 100.0);  // 45 degrees infront of target (leading it's movement)

    float rnd = rand_norm();
    x += cos(angle) * distance * rnd;
    y += sin(angle) * distance * rnd;
    if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ(), x, y, z))
    {
        x = object->GetPositionX();
        y = object->GetPositionY();
        z = object->GetPositionZ();
    }
    return MoveTo(mapId, x, y, z, false, false, false, true);
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*)
{
    // never wander off a boat or zeppelin at sea
    if (bot->GetTransport() && (bot->GetTransport()->IsMotionTransport() || RidesTramLine(bot)))  // [tram23]
        return HoldPosition();

    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    // Previously: attempts = 1. A single random sample often landed in
    // water / blocked geometry / unreachable poly, the function returned
    // false, and the caller had no fallback — bot stood still. Retry a
    // handful of times with a fresh distance each loop so a bad roll
    // doesn't lock the bot in place.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        BotPath const path = FindBotPath(bot, dx, dy, dz);  // [lead109]
        PathType type = path.type;
        uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        bool canReach = !(type & (~typeOk));

        if (!canReach)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), dx, dy, dz, false, false, false, true, priority);
        if (moved)
            return true;
    }

    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    LastMovement& last = AI_VALUE(LastMovement&, "last movement");
    last.Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    botAI->rpgInfo.waitClockMs = last.msTime;  // [holds9]
    return true;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    RelayInteractPacketToGroupBots(guid);

    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    // Accepting and turning in rebuild the giver's menu: walk a copy.
    std::vector<uint32> questIds;
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
        questIds.push_back(menu.GetItem(idx).QuestId);

    for (uint32 questId : questIds)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        // [quest14] user rule 2026-11-08: a grey quest is taken only when it is needed (then its low level does not
        // hide it); one nobody needs is skipped
        bool worth = false;
        bool const takeable = status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) &&
                              bot->CanAddQuest(quest, false) && IsQuestCapableDoing(quest) &&
                              !QuestTooHighToTake(bot, quest);
        if (takeable && GuildCoordinator::IsGreyQuest(bot, quest) && !GuildCoordinator::HumanCharacter(bot))
        {
            std::string const need = sGuildCoordinator.GreyQuestNeed(bot, quest);
            worth = !need.empty() && !quest->IsRepeatable() && !quest->IsSeasonal();
            GuildCoordinator::LogGrey(bot, worth ? "keeps" : "skips", quest,
                                      worth ? need : std::string("not needed (no chain step, upgrade, recipe, skill, "
                                                                 "attunement, key or wanted item)"));
        }
        else if (takeable)
            worth = IsQuestWorthDoing(quest);
        if (takeable && worth)
        {
            AcceptQuest(quest, guid);
            // Quests that are complete on acceptance and end here (auto-complete "turn-in" quests, #2495) are handed
            // in right away below; the relayed accept comes first so the group bots hold the quest by then.
            status = bot->GetQuestStatus(questId);
            if (status == QUEST_STATUS_COMPLETE && !object->hasInvolvedQuest(questId))
                status = QUEST_STATUS_INCOMPLETE;
            RelayQuestPacketToGroupBots(CMSG_QUESTGIVER_ACCEPT_QUEST, guid, quest->GetQuestId());
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->RecordQuestAccepted(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && object->hasInvolvedQuest(questId) &&
            bot->CanRewardQuest(quest, 0, false))
        {
            RelayQuestPacketToGroupBots(CMSG_QUESTGIVER_COMPLETE_QUEST, guid, quest->GetQuestId());
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->RecordQuestRewarded(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

// A selfbot acts server-side, so its client never sends the packets its own bots react to (accept the same
// quest, sync the turn-in, use the same object, follow onto a taxi). Feed them the packet a client would send.
void NewRpgBaseAction::RelayPacketToGroupBots(WorldPacket const& packet)
{
    if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(bot))
        mgr->HandleMasterIncomingPacket(packet);
}

void NewRpgBaseAction::RelayQuestPacketToGroupBots(uint16 opcode, ObjectGuid guid, uint32 questId)
{
    WorldPacket packet(opcode, 8 + 4 + 4);
    packet << guid << questId;
    if (opcode == CMSG_QUESTGIVER_ACCEPT_QUEST)
        packet << uint32(0);

    RelayPacketToGroupBots(packet);
}

void NewRpgBaseAction::RelayInteractPacketToGroupBots(ObjectGuid guid)
{
    WorldPacket packet(guid.IsGameObject() ? CMSG_GAMEOBJ_USE : CMSG_GOSSIP_HELLO, 8);
    packet << guid;
    RelayPacketToGroupBots(packet);
}
bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

void NewRpgBaseAction::AbandonGreyQuest(uint8 slot, Quest const* quest, std::string const& reason)
{
    if (!quest || slot >= MAX_QUEST_LOG_SIZE || bot->GetQuestSlotQuestId(slot) != quest->GetQuestId())
        return;
    GuildCoordinator::LogGrey(bot, "abandons", quest, reason);
    if (auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
        doing && doing->questId == quest->GetQuestId())
        botAI->rpgInfo.ChangeToIdle();
    WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
    packet << uint8(slot);
    WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
    removeQuest.Read();
    bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    // [group23] coordinator 2026-11-09: a group quests as players do - the leader shares what it takes with its
    // party (the core's quest share, CMSG_PUSHQUESTTOPARTY: a shareable quest each member may take; the bots accept it
    // by their "accept quest share"), the members take a quest it cannot share at the giver themselves (FollowAction).
    // b370: every user-pack / group follower made 0 turn-ins - followers never took a quest
    if (Group* group = bot->GetGroup(); group && group->GetLeaderGUID() == bot->GetGUID() &&
                                         GuildHonest::IsHonestBot(bot) &&
                                         bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE &&
                                         quest->HasFlag(QUEST_FLAGS_SHARABLE))
    {
        WorldPacket share(CMSG_PUSHQUESTTOPARTY);
        share << quest->GetQuestId();
        WorldPackets::Quest::PushQuestToParty push(std::move(share));
        push.Read();
        bot->GetSession()->HandlePushQuestToParty(push);
        LOG_INFO("playerbots", "[group23] {} L{} shares q{} {} with its group", bot->GetName(), bot->GetLevel(),
                 quest->GetQuestId(), quest->GetTitle());
    }
    return true;
}


bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    // [guild50] the global leader's pick for the guild (itself, a member through the bank, the wanted list, sale)
    if (int32 const guild = sGuildCoordinator.RewardChoice(bot, quest); guild >= 0)
        return uint32(guild);
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                uint32 const itemId = quest->RewardChoiceItemId[i];
                ItemTemplate const* rewardProto = sObjectMgr->GetItemTemplate(itemId);
                uint8 rewardSlot = NULL_SLOT;
                if (rewardProto)
                    rewardSlot = botAI->FindEquipSlot(rewardProto, NULL_SLOT, true);
                float score = calc.CalculateItem(itemId, 0, rewardSlot);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::IsQuestWorthDoing(Quest const* quest)
{
    bool isLowLevelQuest =
        bot->GetLevel() > (bot->GetQuestLevel(quest) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));

    if (isLowLevelQuest)
        return false;

    if (quest->IsRepeatable())
        return false;

    if (quest->IsSeasonal())
        return false;

    return true;
}

bool NewRpgBaseAction::IsQuestCapableDoing(Quest const* quest)
{
    if (IsSpellUnlockClassQuest(quest))  // [guild13]
        return false;

    bool highLevelQuest = bot->GetLevel() + QUEST_MAX_LEVELS_ABOVE_BOT < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // Dungeon, raid, PvP and the other special quest types stay out.
    if (quest->GetType() != 0 && quest->GetType() != QUEST_TYPE_ELITE)
        return false;

    // [guild14] a quest ended only at a script-spawned object is not taken (the bot does not run the script)
    if (EnderNeedsScript(quest->GetQuestId()))
        return false;

    // [guild15] an objective only the quest's tool item gives credit for (b333 Kianey q6661, Farret / Feette q9531)
    if (NeedsItemUseCredit(quest) || NeedsSpellCredit(bot, quest, false))  // [flit] or a spell / script one
        return false;

    // [fish14] an item only fishing gives: only for a bot that fishes it (Fishing at the water's skill, its own pole)
    if (uint32 fishItem = 0; true)
        if (WorldPosition water; FishOnlyItem(bot, quest, fishItem, water) &&
                                 (!CanFishFor(bot, fishItem) || water == WorldPosition()))
            return false;

    // [stuck8] a quest of a zone the other faction holds (b343 Eraelor L7 night elf sent to Durotar for q834, whose
    // giver is of the neutral Ratchet faction): no zones or hubs of the other side
    if (quest->GetZoneOrSort() > 0 && TravelRouteMgr::IsEnemyZone(bot, uint32(quest->GetZoneOrSort())))
        return false;

    // [guild12] a quest it failed once (timed, escort) is not taken again
    if (HasFailedQuest(bot, quest->GetQuestId()))
        return false;

    // [guild12] a timed quest only when the walk to its ender at the bot's run speed fits its time
    if (quest->GetTimeAllowed() && !TimedQuestFits(quest))
        return false;

    // Elite and group quests are fine for a group of three or more around the bot's level (a five-man party).
    bool const groupQuest = quest->GetType() == QUEST_TYPE_ELITE || quest->GetSuggestedPlayers() >= 2;
    if (groupQuest && (quest->GetSuggestedPlayers() > MAX_GROUP_QUEST_PLAYERS || !HasCapableGroup()))
        return false;

    return true;
}

bool NewRpgBaseAction::HasCapableGroup()
{
    NewRpgInfo& info = botAI->rpgInfo;
    if (info.groupCheckedAt && GetMSTimeDiffToNow(info.groupCheckedAt) < GROUP_CHECK_MS)
        return info.capableGroup;

    info.groupCheckedAt = std::max(getMSTime(), 1u);
    uint32 nearLevel = 0;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
                std::abs(static_cast<int32>(member->GetLevel()) - static_cast<int32>(bot->GetLevel())) <=
                    GROUP_LEVEL_SPREAD)
                ++nearLevel;
        }
    }

    info.capableGroup = nearLevel >= MIN_GROUP_FOR_ELITE;
    return info.capableGroup;
}

// The quests of the level 74 Dalaran chain end up in the city, outside the zone filter of the quest POIs.
bool NewRpgBaseAction::GetDalaranQuestTarget(uint32 questId, WorldPosition& pos)
{
    using namespace DalaranTravel;
    QuestStatus const status = bot->GetQuestStatus(questId);
    uint32 creature = 0;
    uint32 object = 0;
    if (IsTeleportQuest(questId) && status == QUEST_STATUS_COMPLETE)
        creature = NPC_CELINDRA;
    else if (questId == QUEST_LEAVE_AND_RETURN)
    {
        if (status == QUEST_STATUS_INCOMPLETE)
            object = GO_CRYSTAL_DOWN;
        else if (status == QUEST_STATUS_COMPLETE)
            creature = NPC_CELINDRA;
    }

    std::vector<WorldPosition> const& spawns =
        creature ? sTravelRouteMgr.GetSpawns(creature) : sTravelRouteMgr.GetObjectSpawns(object);
    if ((!creature && !object) || spawns.empty())
        return false;

    pos = spawns.front();
    return pos.GetMapId() == bot->GetMapId();
}

// A quest objective that is a game object to use (a goober, the Dalaran crystal): walk up and use it. The core
// credits the whole group at reward distance; relaying the use lets group bots use their own copy too (#2725).
namespace
{
    // [objreach67] the quest objects a bot's walk cannot bring into its interact reach (the walk-up ended no nearer:
    // a pear in a tree, a relic on a ledge - static object, static navmesh): bot -> object guids; and its last walk-up
    std::mutex objReachLock;
    std::unordered_map<uint32, std::unordered_set<uint64>> objOutOfReach;
    std::unordered_map<uint32, std::pair<uint64, float>> objLastWalk;  // bot -> (object, its distance then)

    bool ObjectOutOfReach(Player* bot, GameObject* go)
    {
        std::lock_guard<std::mutex> guard(objReachLock);
        auto const found = objOutOfReach.find(bot->GetGUID().GetCounter());
        return found != objOutOfReach.end() && found->second.count(go->GetGUID().GetRawValue());
    }
}

bool NewRpgBaseAction::UseQuestGameObject(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    auto itr = bot->getQuestStatusMap().find(questId);
    if (!quest || itr == bot->getQuestStatusMap().end() || itr->second.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    GameObject* nearest = nullptr;
    GameObject* locked = nullptr;  // [quest130] the objective's object, not selectable yet
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const required = quest->RequiredNpcOrGo[i];
        if (required >= 0 || itr->second.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
            continue;

        // [guild90] spawned ones only: a used (despawned) nearest one hid the rest
        GameObject* go = bot->FindNearestGameObject(static_cast<uint32>(-required), QUEST_OBJECT_RANGE, true);
        if (!go || !go->isSpawned() || go->HasGameObjectFlag(GO_FLAG_IN_USE))
            continue;
        GameObject*& slot = go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) ? locked : nearest;
        if (!slot || bot->GetDistance(go) < bot->GetDistance(slot))
            slot = go;
    }
    // [quest130] the objective's object is locked until the quest's own switches are used: b424 q9740 The Sun Gate,
    // its gate not selectable while a channeler stands within 60 yd, each channeler sent off by one of the four
    // Sunhawk Portal Controllers (goobers whose quest field is 9740). Hobliir, Colian and Brelir clicked the locked
    // gate for 2 h, 0 turn-ins. A goober of the quest (its template's quest field: usable only on the quest) in reach
    // is used first; with none the locked object is used as before
    if (!nearest && locked)
    {
        static std::unordered_map<uint32, std::vector<uint32>> const switches = []
        {
            std::unordered_map<uint32, std::vector<uint32>> byQuest;
            for (auto const& [entry, goTemplate] : *sObjectMgr->GetGameObjectTemplates())
                if (goTemplate.type == GAMEOBJECT_TYPE_GOOBER && goTemplate.goober.questId > 0)
                    byQuest[uint32(goTemplate.goober.questId)].push_back(entry);
            return byQuest;
        }();
        if (auto const found = switches.find(questId); found != switches.end())
            for (uint32 const entry : found->second)
            {
                if (entry == locked->GetEntry())
                    continue;
                GameObject* go = bot->FindNearestGameObject(entry, QUEST_OBJECT_RANGE, true);
                if (go && go->isSpawned() && !go->HasGameObjectFlag(GO_FLAG_IN_USE) &&
                    !go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) && go->GetGoState() == GO_STATE_READY &&
                    (!nearest || bot->GetDistance(go) < bot->GetDistance(nearest)))
                    nearest = go;
            }
        if (nearest)
            LogQuestObject(bot, nearest,
                           Acore::StringFormat("[quest130] q{}: {} is locked, its switch first", questId,
                                               locked->GetName()));
        else
            nearest = locked;
    }

    if (!nearest)
    {
        // [guild12] an object that drops an item the quest still needs (herb, mushroom, crate): walk up, the loot
        // pipeline ("can loot" -> open loot -> store loot) opens and loots it
        GameObject* lootable = FindNeededQuestObject(bot);
        if (!lootable)
            return false;
        LootObject loot(bot, lootable->GetGUID());
        if (!loot.IsAtInteractDistance(bot))
        {
            // [guild15] the object inside the reach of a pack it does not fight (b326 Orenem L11: 4 Bloodmyst
            // Hatchlings at a Sand Pear; DangerOnWay looks at red / orange mobs on the line only)
            if (DestinationRefused(WorldPosition(lootable)))
            {
                LogQuestObject(bot, lootable, "skipped: it lies among mobs it does not pull");
                return false;
            }
            // [objreach67] b406: 38.9 min of "[quest] wait ... quest object in reach" - Sand Pears and Ancient Relics
            // 4-17 yd off, "used: walks up to it" / "skipped: the walk ... not started" in turn: each walk-up ended
            // out of the object's interact reach (up in a tree, on a ledge), the spot stayed unspent for it. A
            // walk-up that ended no nearer than the one before marks the object out of reach for the bot
            uint32 const low = bot->GetGUID().GetCounter();
            uint64 const goKey = lootable->GetGUID().GetRawValue();
            float const gap = bot->GetDistance(lootable);
            if (!bot->isMoving())
            {
                bool outOfReach = false;
                {
                    std::lock_guard<std::mutex> guard(objReachLock);
                    auto const last = objLastWalk.find(low);
                    if (last != objLastWalk.end() && last->second.first == goKey && gap >= last->second.second - 0.5f)
                    {
                        objOutOfReach[low].insert(goKey);
                        objLastWalk.erase(last);
                        outOfReach = true;
                    }
                }
                if (outOfReach)
                {
                    LogQuestObject(bot, lootable,
                                   Acore::StringFormat("[objreach67] skipped: out of its reach - its walk-up ends "
                                                       "{:.1f} yd off, no nearer than the last",
                                                       gap));
                    return false;
                }
            }
            LogQuestObject(bot, lootable, "used: walks up to it");
            if (MoveWorldObjectTo(lootable->GetGUID(), 1.5f))
            {
                std::lock_guard<std::mutex> guard(objReachLock);
                objLastWalk[low] = {goKey, gap};
                return true;
            }
            // its own last walk is still under way (or its window open): the tick is the walk's, no random hop
            LogQuestObject(bot, lootable, "skipped: the walk to it was not started (the last walk is not done)");
            return true;
        }
        if (bot->isMoving())
            bot->StopMoving();
        LogQuestObject(bot, lootable, "used: loots it");
        NoteQuestUse(bot, questId, lootable->GetEntry());  // [qeff31]
        return AI_VALUE(LootObjectStack*, "available loot")->Add(lootable->GetGUID());
    }

    // the Dalaran crystal teleports only its user: the group goes down with the bot
    if (nearest->GetEntry() == DalaranTravel::GO_CRYSTAL_DOWN)
        return UseDalaranCrystal(DalaranTravel::GO_CRYSTAL_DOWN);

    if (!nearest->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
    {
        if (DestinationRefused(WorldPosition(nearest)))  // [guild15] as above
        {
            LogQuestObject(bot, nearest, "skipped: it lies among mobs it does not pull");
            return false;
        }
        MoveWorldObjectTo(nearest->GetGUID(), 1.5f);
        return true;
    }

    if (bot->isMoving())
        bot->StopMoving();

    LogQuestObject(bot, nearest, "used: q" + std::to_string(questId) + " objective");
    NoteQuestUse(bot, questId, nearest->GetEntry());  // [qeff31]
    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
    use << nearest->GetGUID();
    RelayInteractPacketToGroupBots(nearest->GetGUID());
    bot->GetSession()->HandleGameObjectUseOpcode(use);
    return ForceToWait(3 * IN_MILLISECONDS);
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    // Quests of player-owned characters (selfbots, altbots) are never thrown away to make room.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        bool const worthDoing = IsQuestWorthDoing(quest);
        bool const capableDoing = IsQuestCapableDoing(quest);
        bool const failed = bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED;
        if (!worthDoing || !capableDoing || failed)
        {
            QuestDropReason const reason = !worthDoing    ? QuestDropReason::NotWorth
                                           : !capableDoing ? QuestDropReason::NotCapable
                                                           : QuestDropReason::Failed;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival/class quests and quests in different zone
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();

        int32 const zoneOrSort = quest->GetZoneOrSort();
        if (zoneOrSort < 0 || (zoneOrSort > 0 && zoneOrSort != botZoneId))
        {
            QuestDropReason const reason = zoneOrSort < 0 ? QuestDropReason::ClassOrHoliday
                                                          : QuestDropReason::OtherZone;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->RecordQuestDropped(questId, QuestDropReason::LogFull);
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    ObjectGuid npcOrGo = AI_VALUE(ObjectGuid, "new rpg quest giver");
    if (!npcOrGo)
        return false;

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
    if (!object)
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    info.questGiver = npcOrGo;

    // [timers] no give-up timer: the giver is skipped (until the next IDLE) by facts - an interaction that took nothing
    // (no quest from it the bot accepts, none it hands in: the quest log and the rewarded set unchanged), or no walk
    // reaches it (below)
    if (bot->CanInteractWithQuestGiver(object))
    {
        size_t const logBefore = bot->getQuestStatusMap().size();
        size_t const rewardedBefore = bot->getRewardedQuests().size();
        InteractWithNpcOrGameObjectForQuest(npcOrGo);
        if (bot->getQuestStatusMap().size() == logBefore && bot->getRewardedQuests().size() == rewardedBefore)
        {
            LOG_INFO("playerbots", "[rpg] {} L{} SKIP quest giver {} reason: the interaction took nothing (no quest "
                     "from it it accepts, none it hands in)",
                     bot->GetName(), bot->GetLevel(), object->GetName());
            info.ignoredQuestGivers.insert(npcOrGo);
            info.questGiver.Clear();
            return false;
        }
        ForceToWait(5000);
        return true;
    }
    // [guild90] still walking the last walk there: the tick is taken (b317 Eraelor at Tallonkai Swiftroot on
    // Dolanaar's upper floor, Emdyl / Thamesh at Marleth Barleybrew)
    if (bot->isMoving())
        return true;
    // [timers] in reach and still no talk (hostile, its quest flag off): no walk changes that
    if (bot->IsWithinDistInMap(object, INTERACTION_DISTANCE))
    {
        LOG_INFO("playerbots", "[rpg] {} L{} SKIP quest giver {} reason: in reach, it does not talk to it",
                 bot->GetName(), bot->GetLevel(), object->GetName());
        info.ignoredQuestGivers.insert(npcOrGo);
        info.questGiver.Clear();
        return false;
    }
    // [timers] a new walk starts only where the route reaches the giver: the route search is the proof, asked each
    // time the bot stands still short of it (a partial route walked to its end and re-issued never got there)
    BotPath const path = FindBotPath(bot, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
    float const shortBy = (path.type & PATHFIND_NOPATH)
                              ? object->GetExactDist(bot)
                              : object->GetExactDist(path.actualEnd.x, path.actualEnd.y, path.actualEnd.z);
    if ((path.type & PATHFIND_NOPATH) || shortBy > INTERACTION_DISTANCE)
    {
        LOG_INFO("playerbots", "[rpg] {} L{} SKIP quest giver {} reason: no walk reaches it (route ends {:.0f} yd short)",
                 bot->GetName(), bot->GetLevel(), object->GetName(), shortBy);
        info.ignoredQuestGivers.insert(npcOrGo);
        info.questGiver.Clear();
        // [guild15] its quests wait for the next level: the planner picks starters by spot, not by this guid (b321
        // Eraelor / Lyleria / Alnia took q932 and q2438 from Tallonkai Swiftroot on Dolanaar's upper floor again
        // after every skip: 10 STUCK at (9925,980))
        QuestRelationBounds const starts = object->IsCreature()
                                               ? sObjectMgr->GetCreatureQuestRelationBounds(object->GetEntry())
                                               : sObjectMgr->GetGOQuestRelationBounds(object->GetEntry());
        QuestRelationBounds const ends = object->IsCreature()
                                             ? sObjectMgr->GetCreatureQuestInvolvedRelationBounds(object->GetEntry())
                                             : sObjectMgr->GetGOQuestInvolvedRelationBounds(object->GetEntry());
        for (QuestRelationBounds const& bounds : {starts, ends})
            for (auto itr = bounds.first; itr != bounds.second; ++itr)
                if (bot->GetQuestStatus(itr->second) == QUEST_STATUS_NONE ||
                    bot->GetQuestStatus(itr->second) == QUEST_STATUS_COMPLETE)
                    botAI->lowPriorityQuest.insert(itr->second);
        return false;
    }
    return MoveWorldObjectTo(npcOrGo);
}

static std::vector<float> GenerateRandomWeights(int n)
{
    std::vector<float> weights(n);
    float sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        weights[i] = rand_norm();
        sum += weights[i];
    }
    for (int i = 0; i < n; ++i)
    {
        weights[i] /= sum;
    }
    return weights;
}

namespace
{
    // [guild10] Where quests end and where their objectives stand, straight from the world DB: every spawn of
    // each quest ender, of each objective creature / object, and the explore area triggers. Built once, shared
    // by all bots; used where a quest has no POI to go by (talk, delivery, explore, objectives without POI).
    struct QuestSpot
    {
        uint32 mapId;
        float x, y, z;
        uint32 respawn = 0;  // [guild90] the spawn's respawn time (spawntimesecs), 0 for enders and triggers
        ObjectGuid::LowType spawnId = 0;  // [flit] the spawn, for the map's own respawn clock (0: a trigger, a POI)
        bool object = false;              // [flit] a game object spawn
        uint32 entry = 0;                 // [drop71] the spawn's creature / object entry (0: a POI, an ender)
    };

    struct MobSpawn  // [guild12]
    {
        float x, y, z;  // [guild15] z: the core's aggro check is a 3D distance (Sewer Beast under Stormwind's bridges)
        CreatureTemplate const* tmpl;
        int16 event;
        float wander = 0.0f;  // [wander56] its spawn's wander distance (a wanderer reaches from anywhere in it)  // [guild15] game_event_creature: > 0 spawned only while that event runs, < 0 only while it does not
        bool beat = false;    // [patrol86] a node of a patrol's beat (its spawn counted once: not in camp counts)
    };

    // [guild15] spawn guid -> its game event (GameEventMgr's own lists: index = events + event - 1; > 0 spawned only
    // while that event runs, < 0 only while it does not), creatures and objects; built once at startup
    struct SpawnEventIndex
    {
        std::unordered_map<ObjectGuid::LowType, int16> creatures;
        std::unordered_map<ObjectGuid::LowType, int16> objects;
    };
    SpawnEventIndex const& SpawnEvents()
    {
        static SpawnEventIndex index;
        static std::once_flag built;
        std::call_once(built, []()
        {
            int32 const events = int32(sGameEventMgr->GetEventMap().size());
            auto const read = [events](auto const& lists, std::unordered_map<ObjectGuid::LowType, int16>& out)
            {
                for (size_t i = 0; i < lists.size(); ++i)
                    for (ObjectGuid::LowType const guid : lists[i])
                        out[guid] = int16(int32(i) - events + 1);
            };
            read(sGameEventMgr->GameEventCreatureGuids, index.creatures);
            read(sGameEventMgr->GameEventGameobjectGuids, index.objects);
        });
        return index;
    }
    int16 SpawnEvent(bool creature, ObjectGuid::LowType guid)
    {
        auto const& map = creature ? SpawnEvents().creatures : SpawnEvents().objects;
        auto const found = map.find(guid);
        return found == map.end() ? int16(0) : found->second;
    }
    // [guild15] a spawn of that event stands in the world now (map threads read the active set while the world
    // thread's event update does not run: events start and stop in World::Update, not during the map updates)
    bool EventSpawnPresent(int16 event)
    {
        if (!event)
            return true;
        bool const active = sGameEventMgr->IsActiveEvent(uint16(std::abs(int32(event))));
        return event > 0 ? active : !active;
    }
    // b321: the Scourge Invasion's Herald of the Lich King L80, event 17 not running, refused 21 trips and postponed
    // quests
    bool SpawnPresent(MobSpawn const& spawn) { return EventSpawnPresent(spawn.event); }

    // [guild12] the danger bins: map + cell of MAX_AGGRO_RADIUS (no aggro reaches past one cell)
    uint64 MobCellKey(uint32 mapId, float x, float y)
    {
        uint32 const cx = uint32(int32(std::floor(x / MAX_AGGRO_RADIUS)) + 0x8000) & 0xFFFF;
        uint32 const cy = uint32(int32(std::floor(y / MAX_AGGRO_RADIUS)) + 0x8000) & 0xFFFF;
        return (uint64(mapId) << 32) | (cx << 16) | cy;
    }

    struct QuestSpawnIndex
    {
        std::unordered_map<uint32, std::vector<QuestSpot>> enders;    // quest id -> ender spawns
        // [guild12] item objectives: quest item -> the object / creature entries that drop it
        std::unordered_map<uint32, std::vector<uint32>> itemObjects;
        std::unordered_map<uint32, std::vector<uint32>> itemCreatures;
        // [guild12] the spawns of creatures that can be attacked, binned per map (MobCellKey)
        std::unordered_map<uint64, std::vector<MobSpawn>> mobCells;
        // [patrol95] the patrols' beat nodes, apart: read only by the refused-reach passes (StaticDangerDiscs and the
        // trip's danger walk), thinned to one node per BEAT_NODE_STEP yd of the path - b414 had 138533 nodes in
        // mobCells beside 129052 spawns (+107 %), every static pass (camp pricing, static69's packs) read them twice:
        // route search 3748 slow calls (223 s) against b411's 2162 (150 s), route costs 2023 slow calls (67 s) against 59
        std::unordered_map<uint64, std::vector<MobSpawn>> beatCells;
        std::unordered_map<uint32, std::vector<QuestSpot>> creatures;  // creature entry -> spawns
        std::unordered_map<uint32, std::vector<QuestSpot>> objects;    // object entry -> spawns
        std::unordered_map<uint32, std::vector<QuestSpot>> triggers;   // quest id -> explore area triggers
        std::unordered_map<uint32, std::vector<uint32>> triggerIds;    // [quest13] quest id -> those triggers' ids
    };

    QuestSpawnIndex const& GetQuestSpawnIndex()
    {
        static QuestSpawnIndex index;
        static std::once_flag built;
        std::call_once(built, []()
        {
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                index.creatures[data.id].push_back(
                    {data.mapid, data.posX, data.posY, data.posZ, data.spawntimesecs, spawnId, false, data.id});
            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                index.objects[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ,
                                                  uint32(std::max<int32>(0, data.spawntimesecs)), spawnId, true,
                                                  data.id});

            for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
            {
                auto spawns = index.creatures.find(entry);
                if (spawns != index.creatures.end())
                    index.enders[questId].insert(index.enders[questId].end(), spawns->second.begin(),
                                                 spawns->second.end());
            }
            for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestInvolvedRelationMap())
            {
                auto spawns = index.objects.find(entry);
                if (spawns != index.objects.end())
                    index.enders[questId].insert(index.enders[questId].end(), spawns->second.begin(),
                                                 spawns->second.end());
            }

            if (QueryResult result = WorldDatabase.Query("SELECT id, quest FROM areatrigger_involvedrelation"))
            {
                do
                {
                    Field* fields = result->Fetch();
                    if (AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(fields[0].Get<uint32>()))
                    {
                        index.triggers[fields[1].Get<uint32>()].push_back(
                            {trigger->map, trigger->x, trigger->y, trigger->z});
                        index.triggerIds[fields[1].Get<uint32>()].push_back(trigger->entry);  // [quest13]
                    }
                } while (result->NextRow());
            }

            // [guild12] who drops each quest item: gameobject_loot_template -> gameobject_template (loot id, data1
            // of a chest) and gameobject_questitem; creature_loot_template -> creature_template.lootid
            std::unordered_set<uint32> questItems;
            for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i])
                        questItems.insert(quest->RequiredItemId[i]);

            std::unordered_map<uint32, std::vector<uint32>> objectsByLoot;
            for (auto const& [entry, goTemplate] : *sObjectMgr->GetGameObjectTemplates())
            {
                if (uint32 const lootId = goTemplate.GetLootId())
                    objectsByLoot[lootId].push_back(entry);
                if (GameObjectQuestItemList const* items = sObjectMgr->GetGameObjectQuestItemList(entry))
                    for (uint32 const item : *items)
                        if (questItems.count(item))
                            index.itemObjects[item].push_back(entry);
            }
            std::unordered_map<uint32, std::vector<uint32>> creaturesByLoot;
            for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
                if (creatureTemplate.lootid)
                    creaturesByLoot[creatureTemplate.lootid].push_back(entry);

            auto const readLoot = [&questItems](char const* query,
                                                std::unordered_map<uint32, std::vector<uint32>> const& byLoot,
                                                std::unordered_map<uint32, std::vector<uint32>>& out)
            {
                QueryResult result = WorldDatabase.Query(query);
                if (!result)
                    return;
                do
                {
                    Field* fields = result->Fetch();
                    uint32 const item = fields[1].Get<uint32>();
                    if (!questItems.count(item))
                        continue;
                    auto const sources = byLoot.find(fields[0].Get<uint32>());
                    if (sources != byLoot.end())
                        out[item].insert(out[item].end(), sources->second.begin(), sources->second.end());
                } while (result->NextRow());
            };
            readLoot("SELECT Entry, Item FROM gameobject_loot_template WHERE Reference = 0", objectsByLoot,
                     index.itemObjects);
            readLoot("SELECT Entry, Item FROM creature_loot_template WHERE Reference = 0", creaturesByLoot,
                     index.itemCreatures);
            for (auto* sources : {&index.itemObjects, &index.itemCreatures})
                for (auto& [item, entries] : *sources)
                {
                    std::sort(entries.begin(), entries.end());
                    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
                }

            uint32 const indexStart = getMSTime();  // [patrol86] its cost, logged once
            size_t spawnsIndexed = 0, beatNodes = 0, beatPatrols = 0;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
                if (!tmpl || tmpl->type == CREATURE_TYPE_CRITTER ||
                    (tmpl->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE)))
                    continue;
                index.mobCells[MobCellKey(data.mapid, data.posX, data.posY)].push_back(
                    {data.posX, data.posY, data.posZ, tmpl, SpawnEvent(true, spawnId), data.wander_distance});
                ++spawnsIndexed;
                // [patrol86] a patrol reaches from anywhere on its beat: each node of its waypoint path (creature_addon
                // path_id) stands in the index as a spawn point of its own, priced as the spawn is. b410 Laaral L18
                // (Alliance): static39 found "a way round" the Mor'shan guards' spawn points, the Barrens Guards and
                // Horde Elites came along their beats - 3 deaths; Orenem, Ellosera the same road
                if (data.movementType == WAYPOINT_MOTION_TYPE)
                    if (CreatureAddon const* addon = sObjectMgr->GetCreatureAddon(spawnId); addon && addon->path_id)
                        if (WaypointPath const* path = sWaypointMgr->GetPath(addon->path_id))
                        {
                            // a node every BEAT_NODE_STEP yd of the beat: the reach of the smallest aggro disc
                            // (5 yd, Creature::GetAggroRange's floor) over twice covers the path between them
                            constexpr float BEAT_NODE_STEP = 10.0f;
                            float lastX = data.posX, lastY = data.posY;
                            ++beatPatrols;
                            for (WaypointNode const& node : path->Nodes)
                            {
                                if (std::hypot(node.X - lastX, node.Y - lastY) < BEAT_NODE_STEP)
                                    continue;
                                lastX = node.X;
                                lastY = node.Y;
                                ++beatNodes;
                                MobSpawn beat{node.X, node.Y, node.Z, tmpl, SpawnEvent(true, spawnId)};
                                beat.beat = true;
                                index.beatCells[MobCellKey(data.mapid, node.X, node.Y)].push_back(beat);
                            }
                        }
            }
            LOG_INFO("playerbots", "[patrol86] static spawn index: {} spawns, {} patrol beat nodes of {} patrols ({:.1f} "
                     "% more points), built in {} ms",
                     spawnsIndexed, beatNodes, beatPatrols,
                     spawnsIndexed ? 100.0 * double(beatNodes) / double(spawnsIndexed) : 0.0,
                     getMSTimeDiff(indexStart, getMSTime()));
        });
        return index;
    }

    // [guild90] why the bot last left or skipped a quest, and the quest it works on until it is done or postponed
    std::mutex questNoteLock;
    std::unordered_map<uint32, uint32> zoneTarget;  // [guild15] bot -> the zone of its last [zone] line
    std::unordered_map<uint32, std::unordered_map<uint32, std::string>> postponeReasons;  // bot -> quest -> why
    std::unordered_map<uint32, uint32> committedQuest;                                    // bot -> quest
    std::unordered_map<uint32, uint32> resumeQuest;  // [flit] bot -> the quest it left while its camps respawn
    std::unordered_map<uint32, std::string> choiceNotes;  // bot -> the last ChooseBestQuest's why
    std::unordered_map<uint32, std::set<uint32>> deferredHandIns;  // bot -> hand-ins logged as grouped
    constexpr float GROUP_ENDER_RADIUS = 50.0f;     // enders this close are one hand-in visit (one camp of NPCs)
    constexpr float HAND_IN_PASS_DISTANCE = 60.0f;  // a done quest's ender this near is handed in now
    // [guild90] the spot each bot works for a quest (quest -> bot -> map, x, y): other bots' spots count as taken
    struct Claim
    {
        uint32 mapId;
        float x, y;
    };
    std::unordered_map<uint32, std::unordered_map<uint32, Claim>> claims;
    std::unordered_map<uint32, uint32> claimOf;  // bot -> the quest of its claim
    // [guild90] the last resolve of a bot's objective spots (map thread local: one bot resolves at a time per thread)
    struct ResolveStats
    {
        size_t spawns = 0, spent = 0, taken = 0, offered = 0;
    };
    thread_local ResolveStats resolveStats;

    bool TakenByOther(Player* bot, uint32 questId, float x, float y)
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        auto const found = claims.find(questId);
        if (found == claims.end())
            return false;
        for (auto const& [other, claim] : found->second)
            if (other != bot->GetGUID().GetCounter() && claim.mapId == bot->GetMapId() &&
                (claim.x - x) * (claim.x - x) + (claim.y - y) * (claim.y - y) <= SPENT_RADIUS * SPENT_RADIUS)
                return true;
        return false;
    }

    // inside the quest_poi polygon of the objective, or within SPENT_RADIUS of its border
    bool InPolygon(std::vector<QuestPOIPoint> const& poly, float x, float y)
    {
        bool inside = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        {
            float const xi = poly[i].x, yi = poly[i].y, xj = poly[j].x, yj = poly[j].y;
            if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
                inside = !inside;
            float const dx = xj - xi, dy = yj - yi;
            float const len = dx * dx + dy * dy;
            float t = len > 0.0f ? ((x - xi) * dx + (y - yi) * dy) / len : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            if (std::hypot(xi + dx * t - x, yi + dy * t - y) <= SPENT_RADIUS)
                return true;
        }
        return inside;
    }

    // [guild90] every spawn of the sources of one objective: the objective creature / object itself (kill, use) or
    // the creatures and objects that drop its item (loot tables, gameobject_questitem). Pointers into the index.
    void ObjectiveSources(Quest const* quest, uint32 objective, std::vector<QuestSpot const*>& out)
    {
        QuestSpawnIndex const& index = GetQuestSpawnIndex();
        auto const addEntry = [&out](std::unordered_map<uint32, std::vector<QuestSpot>> const& spawns, uint32 entry)
        {
            auto const found = spawns.find(entry);
            if (found != spawns.end())
                for (QuestSpot const& spot : found->second)
                    out.push_back(&spot);
        };
        // [questitems] an objective a tool's use credits: the spawns of what it is used on (rats, a school, a flag)
        std::vector<uint32> useCreatures, useObjects;
        NewRpgQuestUse::SourceEntries(quest, objective, useCreatures, useObjects);
        if (!useCreatures.empty() || !useObjects.empty())
        {
            for (uint32 const entry : useCreatures)
                addEntry(index.creatures, entry);
            for (uint32 const entry : useObjects)
                addEntry(index.objects, entry);
            return;
        }
        if (objective < QUEST_OBJECTIVES_COUNT)
        {
            int32 const npcOrGo = quest->RequiredNpcOrGo[objective];
            if (npcOrGo > 0)
                addEntry(index.creatures, uint32(npcOrGo));
            else if (npcOrGo < 0)
                addEntry(index.objects, uint32(-npcOrGo));
            return;
        }
        if (objective >= QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
            return;
        uint32 const item = quest->RequiredItemId[objective - QUEST_OBJECTIVES_COUNT];
        if (!item)
            return;
        auto const objects = index.itemObjects.find(item);
        if (objects != index.itemObjects.end())
            for (uint32 const entry : objects->second)
                addEntry(index.objects, entry);
        auto const creatures = index.itemCreatures.find(item);
        if (creatures != index.itemCreatures.end())
            for (uint32 const entry : creatures->second)
                addEntry(index.creatures, entry);
    }

    uint32 ObjectiveRespawnNear(Player* bot, uint32 questId, float x, float y)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        Map* map = bot->GetMap();
        if (!quest || !map)
            return 0;
        // [flit] the map's own clock, not the spawn's static spawntimesecs (b341: 60 waits for camps whose spawns
        // stood there alive or were pool members not up): a dead spawn's respawn time (the map's saved one, else its
        // corpse's), a living one in a fight (it dies there: its spawntimesecs); a living one out of combat is there
        // and not for the bot, a spawn not in the world (an idle pool member, an event's) comes back on no clock
        time_t const now = time(nullptr);
        time_t best = 0;
        auto const consider = [&best, now](time_t at)
        {
            if (at > now && (!best || at < best))
                best = at;
        };
        std::vector<QuestSpot const*> spots;
        for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT; ++objective)
            ObjectiveSources(quest, objective, spots);
        for (QuestSpot const* spot : spots)
        {
            if (spot->mapId != bot->GetMapId() || !spot->spawnId ||
                (spot->x - x) * (spot->x - x) + (spot->y - y) * (spot->y - y) > SPENT_RADIUS * SPENT_RADIUS)
                continue;
            if (spot->object)
            {
                consider(map->GetGORespawnTime(spot->spawnId));
                auto const range = map->GetGameObjectBySpawnIdStore().equal_range(spot->spawnId);
                for (auto it = range.first; it != range.second; ++it)
                    if (!it->second->isSpawned())
                        consider(it->second->GetRespawnTime());
                continue;
            }
            consider(map->GetCreatureRespawnTime(spot->spawnId));
            auto const range = map->GetCreatureBySpawnIdStore().equal_range(spot->spawnId);
            for (auto it = range.first; it != range.second; ++it)
            {
                Creature* creature = it->second;
                if (!creature->IsAlive())
                    consider(creature->GetRespawnTime());
                else if (creature->IsInCombat() && spot->respawn)
                    consider(now + time_t(spot->respawn));
            }
        }
        return best ? uint32(best - now) : 0;
    }

    // [drop71] the walk-equivalent yards a unit of `item` costs from `entry` beyond its first kill: the kills beyond
    // one its drop chance needs (100 / chance - 1) at its kill time (template health at `dps`) and the bot's run
    // speed; -1 when its chance is not named (a reference table: no cost known)
    // [drop74] the drop chance of `item` from creature `entry` (its loot table's own entries, references aside): one
    // read of the loot store per pair, kept in a shared read-mostly table (the loot store does not change)
    std::shared_mutex dropChanceLock;
    std::unordered_map<uint64, float> dropChances;
    float DropChance(uint32 entry, uint32 item)
    {
        uint64 const key = (uint64(entry) << 32) | item;
        {
            std::shared_lock<std::shared_mutex> guard(dropChanceLock);
            auto const found = dropChances.find(key);
            if (found != dropChances.end())
                return found->second;
        }
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        LootTemplate const* loot = tmpl && tmpl->lootid ? LootTemplates_Creature.GetLootFor(tmpl->lootid) : nullptr;
        float chance = 0.0f;
        if (loot)
            loot->VisitEntries([&](LootStoreItem const& e, float c)
                               {
                                   if (!e.reference && e.itemid == item)
                                       chance = std::max(chance, c);
                               });
        std::unique_lock<std::shared_mutex> guard(dropChanceLock);
        dropChances[key] = chance;
        return chance;
    }

    float DropCostYd(Player* bot, uint32 entry, uint32 item, float dps)
    {
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        float const chance = tmpl ? DropChance(entry, item) : 0.0f;
        if (chance <= 0.0f)
            return -1.0f;
        uint8 const level = uint8((uint32(tmpl->minlevel) + uint32(tmpl->maxlevel)) / 2);
        CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, tmpl->unit_class);
        float const killSec = stats ? float(stats->GenerateHealth(tmpl)) / std::max(dps, 1.0f) : 0.0f;
        return std::max(0.0f, 100.0f / chance - 1.0f) * killSec * bot->GetSpeed(MOVE_RUN);
    }

    // [qeff32] the creature entries a still-open objective of the quest is served by (its kill target and kill
    // credits, the creatures that drop its still-needed item, what its tool is used on)
    // [drop72] ... for an item: only the droppers worth staying for - one whose drop cost a unit is above another
    // source's cost plus the walk to that source's nearest spawn is no reason to stay at its spot (it counts spent
    // for the item; the spot search, ranked the same way by drop71, takes the better source)
    std::vector<uint32> OpenObjectiveCreatures(Player* bot, Quest const* quest)
    {
        std::vector<uint32> entries;
        auto const status = bot->getQuestStatusMap().find(quest->GetQuestId());
        if (status == bot->getQuestStatusMap().end())
            return entries;
        QuestSpawnIndex const& index = GetQuestSpawnIndex();
        for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT; ++objective)
        {
            std::vector<uint32> useCreatures, useObjects;
            NewRpgQuestUse::SourceEntries(quest, objective, useCreatures, useObjects);
            if (objective < QUEST_OBJECTIVES_COUNT)
            {
                if (!quest->RequiredNpcOrGoCount[objective] ||
                    status->second.CreatureOrGOCount[objective] >= quest->RequiredNpcOrGoCount[objective])
                    continue;
                if (!useCreatures.empty() || !useObjects.empty())
                    entries.insert(entries.end(), useCreatures.begin(), useCreatures.end());
                else if (quest->RequiredNpcOrGo[objective] > 0)
                    entries.push_back(uint32(quest->RequiredNpcOrGo[objective]));
                continue;
            }
            uint32 const slot = objective - QUEST_OBJECTIVES_COUNT;
            uint32 const item = quest->RequiredItemId[slot];
            if (!item || status->second.ItemCount[slot] >= quest->RequiredItemCount[slot])
                continue;
            entries.insert(entries.end(), useCreatures.begin(), useCreatures.end());
            auto const creatures = index.itemCreatures.find(item);
            if (creatures == index.itemCreatures.end())
                continue;
            float const dps = DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo);
            std::vector<std::pair<uint32, float>> costs;  // entry -> its drop cost (-1: unknown)
            float best = std::numeric_limits<float>::max();  // the cheapest source's cost plus its walk
            for (uint32 entry : creatures->second)
            {
                float const cost = DropCostYd(bot, entry, item, dps);
                costs.push_back({entry, cost});
                if (cost < 0.0f)
                    continue;
                auto const spawns = index.creatures.find(entry);
                if (spawns == index.creatures.end())
                    continue;
                float nearest = std::numeric_limits<float>::max();
                for (QuestSpot const& spot : spawns->second)
                    if (spot.mapId == bot->GetMapId())
                        nearest = std::min(nearest, bot->GetDistance2d(spot.x, spot.y));
                if (nearest != std::numeric_limits<float>::max())
                    best = std::min(best, cost + nearest);
            }
            for (auto const& [entry, cost] : costs)
                if (cost < 0.0f || cost <= best)
                    entries.push_back(entry);
        }
        return entries;
    }

    // [guild90] up to this many objective spots per objective go to the walk check, nearest first
    constexpr size_t MAX_SPOT_CANDIDATES = 4;
    // [guild12] the spots of `spots` on the bot's map whose way is free of orange / red mobs, as POIs, nearest first;
    // [guild90] the spent ones are skipped (IsSpentSpot: seen empty, until their respawn) and up to
    // MAX_SPOT_CANDIDATES camps are taken (b317 Eraelor q919: 33 Timberling Sprout spawns, one spot, spent, POSTPONE).
    // 0: none on this map, 1: found, 2: every unspent spot on this map has such a mob (`blocker`), 3: all spent
    // [drop71] the resolve's item objective: the extra walk-equivalent yards per objective unit of each source entry
    // (its drop chance in the loot tables: the kills beyond one a unit costs, at the kill's time, at the walk speed)
    thread_local std::unordered_map<uint32, float> const* sourceCostYd = nullptr;

    int NearestSafeSpot(Player* bot, uint32 questId, std::vector<QuestSpot const*> const& spots, int32 objectiveIdx,
                        std::vector<POIInfo>& poiInfo, CreatureTemplate const*& blocker)
    {
        std::vector<std::pair<float, QuestSpot const*>> onMap;
        std::unordered_map<QuestSpot const*, float> rank;  // [drop71] the walk plus the source's drop cost
        for (QuestSpot const* spot : spots)
            if (spot->mapId == bot->GetMapId())
            {
                float const distance = bot->GetDistance2d(spot->x, spot->y);
                onMap.push_back({distance, spot});
                float extra = 0.0f;
                if (sourceCostYd)
                    if (auto const found = sourceCostYd->find(spot->entry); found != sourceCostYd->end())
                        extra = found->second;
                rank[spot] = distance + extra;
            }
        if (onMap.empty())
            return 0;
        std::sort(onMap.begin(), onMap.end(),
                  [&rank](auto const& a, auto const& b) { return rank[a.second] < rank[b.second]; });
        size_t const first = poiInfo.size();
        size_t spent = 0;
        bool blocked = false;
        // [pull9] the camps other bots work, nearest first: offered when no other camp is left - a player shares the
        // camp (its mobs respawn, a quest object is looted by each). Before, they counted as "seen empty" and the
        // quest was left for as long as the other bot held its claim, stuck or not (b345: 38 of 43 "every camp seen
        // empty" with no camp seen; q9506 left by 4 bots while Almored looped in its camp)
        std::vector<std::pair<float, QuestSpot const*>> taken;
        auto const offer = [&](float distance, QuestSpot const* spot) -> bool
        {
            // a spawn next to a spot already taken adds nothing (one camp)
            for (size_t i = first; i < poiInfo.size(); ++i)
                if (std::hypot(poiInfo[i].pos.x - spot->x, poiInfo[i].pos.y - spot->y) < SPENT_RADIUS)
                    return false;
            // [guild15] the objective's own camp: the last aggro reach of the approach to the spot. The walk there is
            // the route search's (it goes round a mob it must not fight, RouteCosts); the straight line from the bot
            // met mobs nowhere near the objective (b333 Januel L11 in Stormwind: q62 / q2206 "objective among Sewer
            // Beast L50", the canals between him and Elwynn; q6 among a Greater Lava Spider L49)
            float const toBot = std::max(distance, 0.1f);
            float const back = std::min(MAX_AGGRO_RADIUS, toBot) / toBot;
            float const fromX = spot->x + (bot->GetPositionX() - spot->x) * back;
            float const fromY = spot->y + (bot->GetPositionY() - spot->y) * back;
            if (CreatureTemplate const* danger = NewRpgBaseAction::DangerOnWay(bot, fromX, fromY, spot->x, spot->y))
            {
                blocker = danger;
                blocked = true;
                return false;
            }
            poiInfo.push_back({{spot->x, spot->y}, objectiveIdx, spot->z != 0.0f ? spot->z : INVALID_HEIGHT});
            ++resolveStats.offered;
            return poiInfo.size() - first >= MAX_SPOT_CANDIDATES;
        };
        for (auto const& [distance, spot] : onMap)
        {
            ++resolveStats.spawns;
            if (NewRpgBaseAction::IsSpentSpot(bot, questId, spot->x, spot->y))
            {
                ++spent;
                ++resolveStats.spent;
                continue;
            }
            // [share64] the spawn itself down by the map's own respawn clock (dead, looted, despawned - by whoever: a
            // guildmate's kill is the world's fact, not that bot's memory): no spot until it is up again. The spent
            // marks were each bot's own, so every bot on a quest walked to the same empty spawn in turn (b400 q9537:
            // a single spawn, Feeli, Farret 2704 yd, Zharet 1581 yd, Aevmae - each "every camp seen empty")
            if (spot->spawnId && spot->mapId == bot->GetMapId())
            {
                Map* map = bot->GetMap();
                time_t const now = time(nullptr);
                time_t up = spot->object ? map->GetGORespawnTime(spot->spawnId)
                                         : map->GetCreatureRespawnTime(spot->spawnId);
                bool alive = false;
                if (spot->object)
                {
                    auto const range = map->GetGameObjectBySpawnIdStore().equal_range(spot->spawnId);
                    for (auto it = range.first; it != range.second; ++it)
                        if (it->second->isSpawned())
                            alive = true;
                        else
                            up = std::max(up, it->second->GetRespawnTime());
                }
                else
                {
                    auto const range = map->GetCreatureBySpawnIdStore().equal_range(spot->spawnId);
                    for (auto it = range.first; it != range.second; ++it)
                        if (it->second->IsAlive())
                            alive = true;
                        else
                            up = std::max(up, it->second->GetRespawnTime());
                }
                if (!alive && up > now)
                {
                    ++spent;
                    ++resolveStats.spent;
                    continue;
                }
            }
            // [guild90] another bot works that camp for the quest: taken, like a player sees it
            if (TakenByOther(bot, questId, spot->x, spot->y))
            {
                ++resolveStats.taken;
                taken.push_back({distance, spot});
                continue;
            }
            if (offer(distance, spot))
                break;
        }
        if (poiInfo.size() == first)
            for (auto const& [distance, spot] : taken)
                if (offer(distance, spot))
                    break;
        if (poiInfo.size() > first)
            return 1;
        return blocked || !spent ? 2 : 3;
    }

    int NearestSafeSpot(Player* bot, uint32 questId, std::vector<QuestSpot> const& spots, int32 objectiveIdx,
                        std::vector<POIInfo>& poiInfo, CreatureTemplate const*& blocker)
    {
        std::vector<QuestSpot const*> pointers;
        pointers.reserve(spots.size());
        for (QuestSpot const& spot : spots)
            pointers.push_back(&spot);
        return NearestSafeSpot(bot, questId, pointers, objectiveIdx, poiInfo, blocker);
    }

    // [guild12] one log line per bot, quest and level for a postponed quest
    void LogPostponed(Player* bot, Quest const* quest, std::string const& reason)
    {
        static std::mutex lock;
        static std::set<std::tuple<uint32, uint32, uint8>> logged;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (!logged.insert({bot->GetGUID().GetCounter(), quest->GetQuestId(), bot->GetLevel()}).second)
                return;
        }
        LOG_INFO("playerbots", "[rpg] {} L{} POSTPONE q{} {} reason: {}", bot->GetName(), bot->GetLevel(),
                 quest->GetQuestId(), quest->GetTitle(), reason);
    }

    // [guild90] a postponed quest holds until the next level (the planner's lowPriorityQuest, cleared on a level-up):
    // b317 Eraelor L6 postponed q919 for Gnarlpine Augur L9 and took it again at L6 from another spot
    void HoldPostponed(Player* bot, Quest const* quest, std::string const& reason)
    {
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->lowPriorityQuest.insert(quest->GetQuestId());
        NewRpgBaseAction::NotePostpone(bot, quest->GetQuestId(), reason);
        LogPostponed(bot, quest, reason);
    }

    // [guild12] where a bot died: the killer's spot and the death spot, the killer's level and aggro radius; the
    // bot keeps away until it outlevels the killer
    struct DeathZone
    {
        uint32 mapId;
        float killerX, killerY, deathX, deathY;
        uint32 entry;
        uint8 level;
        float radius;
        bool refused = false;  // [guild15] a refused pull's zone, not a death's
        bool restClears = false;  // [deaths143] a fight lost for mana to a mob not orange / red for it
    };
    std::mutex deathZonesLock;
    std::unordered_map<uint32, std::vector<DeathZone>> deathZones;

    // [deaths143] user 2026-11-11: at full health and mana, out of a fight, the zones of the fights it lost for mana
    // are gone (the loss was its mana, now full). Caller holds deathZonesLock
    void ClearRestedZones(Player* bot, std::vector<DeathZone>& list)
    {
        if (bot->IsInCombat() || bot->GetHealth() < bot->GetMaxHealth() ||
            bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA))
            return;
        size_t const before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [](DeathZone const& zone) { return zone.restClears; }),
                   list.end());
        if (before != list.size())
            LOG_INFO("playerbots", "[deaths143] {} L{} rested to full health and mana: {} zone(s) of fights lost "
                     "for mana cleared",
                     bot->GetName(), bot->GetLevel(), before - list.size());
    }

    float SegmentDistSq(float px, float py, float fromX, float fromY, float toX, float toY)
    {
        float const dx = toX - fromX;
        float const dy = toY - fromY;
        float const lengthSq = dx * dx + dy * dy;
        float t = lengthSq > 0.0f ? ((px - fromX) * dx + (py - fromY) * dy) / lengthSq : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        float const ex = fromX + dx * t - px;
        float const ey = fromY + dy * t - py;
        return ex * ex + ey * ey;
    }

    std::mutex failedQuestsLock;  // [guild12] bot guid -> quests it failed (timed, escort): not taken again
    std::unordered_map<uint32, std::unordered_set<uint32>> failedQuests;
}

std::vector<G3D::Vector4> NewRpgBaseAction::StaticDangerDiscs(Player* bot, std::vector<G3D::Vector3> const& points,
                                                             size_t fromIndex)
{
    std::vector<G3D::Vector4> discs;
    FactionTemplateEntry const* botFaction = bot ? bot->GetFactionTemplateEntry() : nullptr;
    if (!botFaction || points.size() < 2 || fromIndex >= points.size())
        return discs;
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    uint8 const level = bot->GetLevel();
    uint32 const mapId = bot->GetMapId();
    // the cells the walk touches, each with its neighbours (no aggro reaches past one cell)
    std::unordered_set<uint64> cells;
    constexpr size_t CELL_STRIDE = 4;  // the walk's 4 yd samples, every 16 yd: well inside a cell (a look's cost)
    for (size_t i = fromIndex; i < points.size(); i += CELL_STRIDE)
        for (int32 ox = -1; ox <= 1; ++ox)
            for (int32 oy = -1; oy <= 1; ++oy)
                cells.insert(MobCellKey(mapId, points[i].x + ox * MAX_AGGRO_RADIUS, points[i].y + oy * MAX_AGGRO_RADIUS));
    for (uint64 const cell : cells)
      for (auto const* cellMap : {&index.mobCells, &index.beatCells})  // [patrol95] the spawns, then the beats
    {
        auto const spawns = cellMap->find(cell);
        if (spawns == cellMap->end())
            continue;
        for (MobSpawn const& spawn : spawns->second)
        {
            CreatureTemplate const* tmpl = spawn.tmpl;
            bool const elite = tmpl->rank == CREATURE_ELITE_ELITE || tmpl->rank == CREATURE_ELITE_RAREELITE ||
                               tmpl->rank == CREATURE_ELITE_WORLDBOSS;
            XPColorChar const color = Acore::XP::GetColorCode(level, tmpl->maxlevel);
            bool const never = elite || color == XP_RED || color == XP_ORANGE;
            // [static69] b411: the static pricing of same-level packs on the way over-refused - the live search runs
            // past a pack it survives ([runpast11]) or pulls one mob of it ([wall17]), the static one refused the
            // pack's whole reach: [trip37] "only walk passes the reach of a mob it must not fight" 66 in 10 QUEST-h
            // against 9 in b406's 19, static39 "no way round" up 35 % per QUEST-hour. Only the mobs it never fights
            // are refused beyond sight again, until the static price has the run past and the single pull too
            if (!never)
                continue;
            if (!SpawnPresent(spawn))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(tmpl->faction);
            if (!faction || !faction->IsHostileTo(*botFaction))
                continue;
            // Creature::GetAggroRange: the detection range grows a yard per level the mob is above the player
            float const radius = std::clamp(tmpl->detection_range + float(int32(tmpl->maxlevel) - int32(level)), 5.0f,
                                            MAX_AGGRO_RADIUS) +
                                 bot->GetCombatReach() + spawn.wander;  // [wander56]
            bool covers = false;
            for (size_t i = fromIndex; i < points.size() && !covers; i += 2)  // every 8 yd: within a reach
                covers = std::hypot(points[i].x - spawn.x, points[i].y - spawn.y) <= radius;
            // [patrol101] a beat node whose disc an already kept disc holds for the most part (its centre within
            // half that disc's radius) adds nothing the route search does not refuse already: the refuse list stays
            // short (every polygon of the search is tested against each disc). b417: route searches of 200-430 ms,
            // 100 of them (14 s) in one health window, the 5.4 s tick at the start
            if (covers && spawn.beat)
                for (G3D::Vector4 const& kept : discs)
                    if (std::hypot(kept.x - spawn.x, kept.y - spawn.y) <= kept.w * 0.5f && kept.w >= radius)
                    {
                        covers = false;
                        break;
                    }
            if (covers)
                discs.emplace_back(spawn.x, spawn.y, spawn.z, radius);
        }
    }
    return discs;
}

CreatureTemplate const* NewRpgBaseAction::DangerOnWay(Player* bot, float fromX, float fromY, float toX, float toY,
                                                      float fromZ, float toZ)
{
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!botFaction)
        return nullptr;
    uint8 const level = bot->GetLevel();
    uint32 const mapId = bot->GetMapId();
    float const dx = toX - fromX;
    float const dy = toY - fromY;
    float const length = std::sqrt(dx * dx + dy * dy);
    bool const heights = fromZ != NO_WAY_HEIGHT && toZ != NO_WAY_HEIGHT;
    // [guild15] the walk's point nearest to (px, py): its share t along the walk; t == 0 = the start
    auto const nearestShare = [&](float px, float py)
    {
        float const t = length > 0.0f ? ((px - fromX) * dx + (py - fromY) * dy) / (length * length) : 0.0f;
        return std::clamp(t, 0.0f, 1.0f);
    };
    auto const gapSq = [&](float px, float py, float pz, float t)
    {
        float const ex = fromX + dx * t - px;
        float const ey = fromY + dy * t - py;
        float ez = 0.0f;
        if (heights && pz != NO_WAY_HEIGHT)
            ez = fromZ + (toZ - fromZ) * t - pz;
        return ex * ex + ey * ey + ez * ez;
    };

    // where it died, until it outlevels the killer
    {
        std::lock_guard<std::mutex> guard(deathZonesLock);
        auto const zones = deathZones.find(bot->GetGUID().GetCounter());
        if (zones != deathZones.end())
        {
            auto& list = zones->second;
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [level](DeathZone const& zone) { return level > zone.level; }),
                       list.end());
            ClearRestedZones(bot, list);  // [deaths143]
            for (DeathZone const& zone : list)
            {
                if (zone.mapId != mapId)
                    continue;
                float const radiusSq = zone.radius * zone.radius;
                for (auto const& [px, py] : {std::pair{zone.killerX, zone.killerY}, std::pair{zone.deathX, zone.deathY}})
                {
                    float const t = nearestShare(px, py);
                    if (t > 0.0f && gapSq(px, py, NO_WAY_HEIGHT, t) <= radiusSq)
                        if (CreatureTemplate const* killer = sObjectMgr->GetCreatureTemplate(zone.entry))
                            return killer;
                }
            }
        }
    }

    SoloSafety::RunPast runPast;  // [regress10] the chases of the orange / red spawns it walks past
    // the cells along the walk and their neighbours
    uint32 const steps = uint32(std::ceil(length / MAX_AGGRO_RADIUS));
    std::unordered_set<uint64> cells;
    for (uint32 step = 0; step <= steps; ++step)
    {
        float const t = steps ? float(step) / steps : 0.0f;
        for (int32 ox = -1; ox <= 1; ++ox)
            for (int32 oy = -1; oy <= 1; ++oy)
                cells.insert(MobCellKey(mapId, fromX + dx * t + ox * MAX_AGGRO_RADIUS,
                                        fromY + dy * t + oy * MAX_AGGRO_RADIUS));
    }

    for (uint64 const cell : cells)
      for (auto const* cellMap : {&index.mobCells, &index.beatCells})  // [patrol95] the spawns, then the beats
    {
        auto const spawns = cellMap->find(cell);
        if (spawns == cellMap->end())
            continue;
        for (MobSpawn const& spawn : spawns->second)
        {
            CreatureTemplate const* tmpl = spawn.tmpl;
            XPColorChar const color = Acore::XP::GetColorCode(level, tmpl->maxlevel);
            if (color != XP_RED && color != XP_ORANGE)
                continue;
            if (!SpawnPresent(spawn))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(tmpl->faction);
            if (!faction || !faction->IsHostileTo(*botFaction))
                continue;
            // Creature::GetAggroRange: the detection range grows a yard per level the mob is above the player
            // [wander56] a wanderer reaches from anywhere in its wander area: its spawn's wander distance on top. b397
            // Gilduck / Aestymro L14 walked "the core's path" past the Nightbane Dark Runners L28-29 (wanderers,
            // wander 5-10) in Brightwood Grove, every leg "safe" by their spawn points alone: 5 deaths
            float const aggro = std::clamp(tmpl->detection_range + float(int32(tmpl->maxlevel) - int32(level)),
                                           5.0f, MAX_AGGRO_RADIUS) +
                                spawn.wander;
            float const t = nearestShare(spawn.x, spawn.y);
            if (t <= 0.0f)
                continue;
            float const flatSq = gapSq(spawn.x, spawn.y, NO_WAY_HEIGHT, t);
            if (flatSq > aggro * aggro)
                continue;
            // [guild15] the core's aggro (Creature::CanStartAttack): a mob that cannot fly starts no attack on a
            // player more than CREATURE_Z_ATTACK_RANGE above or below it, and the range is a 3D distance. Looked at
            // over the stretch of the walk inside its aggro circle (its ends and the nearest point): b321-b322 the
            // Sewer Beasts L50 at the bottom of Stormwind's canals (z 86, streets 96-100) refused 32 vendor trips
            if (heights && !tmpl->Movement.IsFlightAllowed() && length > 0.0f)
            {
                float const half = std::sqrt(std::max(0.0f, aggro * aggro - flatSq)) / length;
                float const lo = std::max(0.0f, t - half);
                float const hi = std::min(1.0f, t + half);
                // the walk's height is linear on the leg: its nearest height to the mob is at an end of the stretch
                // or where it crosses the mob's height
                float const cross = toZ != fromZ ? std::clamp((spawn.z - fromZ) / (toZ - fromZ), lo, hi) : t;
                bool reaches = false;
                for (float const at : {lo, t, hi, cross})
                {
                    float const walkZ = fromZ + (toZ - fromZ) * at;
                    if (std::fabs(walkZ - spawn.z) <= CREATURE_Z_ATTACK_RANGE &&
                        gapSq(spawn.x, spawn.y, spawn.z, at) <= aggro * aggro)
                        reaches = true;
                }
                if (!reaches)
                    continue;
            }
            // [regress10] user 2026-11-08 "you can just run past": the walk takes the chase of every such spawn on it
            // (SoloSafety::RunPastTemplate, the core's leash) while the bot lives through their sum
            SoloSafety::RunPast const chase = SoloSafety::RunPastTemplate(bot, tmpl);
            runPast.damage = chase.damage == std::numeric_limits<float>::max() ? chase.damage
                                                                                 : runPast.damage + chase.damage;
            runPast.seconds = std::max(runPast.seconds, chase.seconds);
            if (!SoloSafety::SurvivesRunPast(bot, runPast.damage, runPast.seconds))
                return tmpl;
        }
    }
    return nullptr;  // none, or every one it passes is a chase it lives through
}

CreatureTemplate const* NewRpgBaseAction::TripDanger(Player* bot, float x, float y, float z)
{
    PMON_SCOPE(tripScope, PERF_MON_VALUE, "solo trip danger");  // [guild15]
    if (!bot || !SoloSafety::Applies(bot))
        return nullptr;
    if (CreatureTemplate const* zone = InDangerZone(bot, x, y))
        return zone;
    // [guild15] a destination on another landmass of this map (Stormwind from Ironforge: the Deeprun Tram): the walk
    // to the route's first ride / portal and the walk from its last one, not a straight line across the continent
    if (TravelRouteMgr::Landmass(WorldPosition(bot)) !=
        TravelRouteMgr::Landmass(WorldPosition(bot->GetMapId(), x, y, z)))
    {
        TravelRouteMgr::Leg leg;
        float seconds = 0.0f;
        WorldPosition exit;
        if (!sTravelRouteMgr.FindRoute(bot, WorldPosition(bot->GetMapId(), x, y, z), leg, seconds, &exit))
            return nullptr;  // no way there at all: the walk itself fails and gives it up, no danger to name
        WorldPosition start;
        if (leg.kind == TravelRouteMgr::LegKind::Portal && leg.portalIndex < sTravelRouteMgr.GetPortals().size())
            start = sTravelRouteMgr.GetPortals()[leg.portalIndex].from;
        else if (leg.kind == TravelRouteMgr::LegKind::Transport)
            if (TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(leg.lineEntry))
                if (leg.fromDock < line->docks.size())
                    start = line->docks[leg.fromDock].pier;
        auto const walkDanger = [bot](G3D::Vector3 const& from, G3D::Vector3 const& to) -> CreatureTemplate const*
        {
            Leader::RouteResult const route = Leader::Route(bot, from, to);
            float lastX = from.x, lastY = from.y, lastZ = from.z;
            if (route.found && route.points.size() >= 2)
            {
                for (size_t i = 1; i < route.points.size(); ++i)
                    if (CreatureTemplate const* danger =
                            DangerOnWay(bot, route.points[i - 1].x, route.points[i - 1].y, route.points[i].x,
                                        route.points[i].y, route.points[i - 1].z, route.points[i].z))
                        return danger;
                if (route.complete)
                    return nullptr;
                lastX = route.points.back().x;
                lastY = route.points.back().y;
                lastZ = route.points.back().z;
            }
            return DangerOnWay(bot, lastX, lastY, to.x, to.y, lastZ, to.z);
        };
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (start.GetMapId() == bot->GetMapId() && start != WorldPosition())
            if (CreatureTemplate const* danger =
                    walkDanger(here, G3D::Vector3(start.GetPositionX(), start.GetPositionY(), start.GetPositionZ())))
                return danger;
        if (exit.GetMapId() == bot->GetMapId() && exit != WorldPosition(bot))
            return walkDanger(G3D::Vector3(exit.GetPositionX(), exit.GetPositionY(), exit.GetPositionZ()),
                              G3D::Vector3(x, y, z));
        return nullptr;
    }
    // the walk it would take: b303 Thokhen L3 died to a Root Thresher L8 on its 919 yd vendor walk, Laaral L10 to
    // Mutated Constrictors L15 on gather and bank walks across Bloodmyst - mobs no quest check had looked at
    Leader::RouteResult const route = Leader::Route(bot, Position(x, y, z));
    float lastX = bot->GetPositionX();
    float lastY = bot->GetPositionY();
    if (route.found && route.points.size() >= 2)
    {
        for (size_t i = 1; i < route.points.size(); ++i)
            if (CreatureTemplate const* danger = DangerOnWay(bot, route.points[i - 1].x, route.points[i - 1].y,
                                                             route.points[i].x, route.points[i].y,
                                                             route.points[i - 1].z, route.points[i].z))
                return danger;
        if (route.complete)
            return nullptr;
        lastX = route.points.back().x;
        lastY = route.points.back().y;
    }
    // [deaths] user 2026-11-08: the rest of a walk longer than the search goes in stages over the travel nodes
    // (NodeStages): the danger on that chain, not on the straight line across whatever lies between (b338
    // TRIP_REFUSED walks of 1200-7000 yd named camps far off any road)
    std::vector<G3D::Vector3> const stages = NodeStages(bot, WorldPosition(bot->GetMapId(), x, y, z));
    if (!stages.empty())
    {
        float const restGap = std::hypot(x - lastX, y - lastY);
        for (G3D::Vector3 const& stage : stages)
        {
            if (std::hypot(x - stage.x, y - stage.y) >= restGap)
                continue;  // behind the walk's searched part
            if (CreatureTemplate const* danger = DangerOnWay(bot, lastX, lastY, stage.x, stage.y))
                return danger;
            lastX = stage.x;
            lastY = stage.y;
        }
    }
    return DangerOnWay(bot, lastX, lastY, x, y);
}

namespace
{
    // [holds9] bot -> spot (map, x, y) given up -> the level it was given up at
    std::mutex givenUpLock;
    std::unordered_map<uint32, std::map<std::tuple<uint32, int32, int32>, uint8>> givenUp;
}

void NewRpgBaseAction::NoteWalkGivenUp(Player* bot, WorldPosition const& dest, std::string const& why)
{
    if (!bot || dest == WorldPosition())
        return;
    {
        std::lock_guard<std::mutex> guard(recoveryLock);
        auto const found = recoveryHub.find(bot->GetGUID().GetCounter());
        if (found != recoveryHub.end() && found->second.first.GetMapId() == dest.GetMapId() &&
            found->second.first.GetExactDist2d(dest.GetPositionX(), dest.GetPositionY()) < 1.0f)
            found->second.second = true;  // [deaths18] its recovery walk is refused
    }

    std::tuple<uint32, int32, int32> const key{dest.GetMapId(), int32(dest.GetPositionX()), int32(dest.GetPositionY())};
    {
        std::lock_guard<std::mutex> guard(givenUpLock);
        auto& spots = givenUp[bot->GetGUID().GetCounter()];
        auto const found = spots.find(key);
        if (found != spots.end() && found->second == bot->GetLevel())
            return;
        spots[key] = bot->GetLevel();
    }
    LOG_INFO("playerbots", "[rpg] {} L{} GIVE_UP walk to ({:.0f},{:.0f},{:.0f}) {:.0f} yd reason: {}; out of its "
             "choices until L{}",
             bot->GetName(), bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
             bot->GetExactDist(dest), why, bot->GetLevel() + 1);
}

void NewRpgBaseAction::ForgetWalkGivenUp(Player* bot, WorldPosition const& dest)
{
    if (!bot || dest == WorldPosition())
        return;
    bool erased = false;
    {
        std::lock_guard<std::mutex> guard(givenUpLock);
        auto const spots = givenUp.find(bot->GetGUID().GetCounter());
        if (spots != givenUp.end())
            erased = spots->second.erase(
                         {dest.GetMapId(), int32(dest.GetPositionX()), int32(dest.GetPositionY())}) != 0;
    }
    if (erased)
        LOG_INFO("playerbots", "[rpg] {} L{} WALK_RETRY ({:.0f},{:.0f}): the camp that walled it is cleared (help)",
                 bot->GetName(), bot->GetLevel(), dest.GetPositionX(), dest.GetPositionY());
}

bool NewRpgBaseAction::WalkGivenUp(Player* bot, float x, float y)
{
    if (!bot)
        return false;
    std::lock_guard<std::mutex> guard(givenUpLock);
    auto const spots = givenUp.find(bot->GetGUID().GetCounter());
    if (spots == givenUp.end())
        return false;
    auto const found = spots->second.find({bot->GetMapId(), int32(x), int32(y)});
    return found != spots->second.end() && found->second == bot->GetLevel();
}

namespace
{
    // [trip52] the trip by air: the walk to its nearest known flight master and the walk off the known node nearest
    // the destination, each a walk it does not refuse, the flight faster than its walk there (as [taxi20] decides it
    // flies) and its fare in its purse: true and the plan's text
    bool FlightTripSafe(Player* bot, WorldPosition const& dest, std::string& plan, bool walkRefused)
    {
        TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot);
        if (!master || !master->taxiNodeId || master->pos.GetMapId() != bot->GetMapId() ||
            !bot->m_taxi.IsTaximaskNodeKnown(master->taxiNodeId))
            return false;
        // [fly164] ... nor a trip by air from a flight master whose walk is given up at this level
        if (NewRpgBaseAction::WalkGivenUp(bot, master->pos.GetPositionX(), master->pos.GetPositionY()))
        {
            NoteFlightMasterGivenUp(bot, master->pos);
            return false;
        }
        TaxiNodesEntry const* to = nullptr;
        float toGap = std::numeric_limits<float>::max();
        for (uint32 i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
            if (!node || node->map_id != dest.GetMapId() || !bot->m_taxi.IsTaximaskNodeKnown(node->ID))
                continue;
            float const gap = std::hypot(node->x - dest.GetPositionX(), node->y - dest.GetPositionY());
            if (gap < toGap)
            {
                toGap = gap;
                to = node;
            }
        }
        if (!to || to->ID == master->taxiNodeId)
            return false;
        std::vector<uint32> path = sTravelNodeMap.FindKnownTaxiPath(bot, master->taxiNodeId, to->ID);
        if (path.size() < 2)
            return false;
        uint32 fare = 0;
        float flightYd = 0.0f;
        for (size_t i = 1; i < path.size(); ++i)
        {
            uint32 pathId = 0, cost = 0;
            sObjectMgr->GetTaxiPath(path[i - 1], path[i], pathId, cost);
            fare += cost;
            TaxiNodesEntry const* a = sTaxiNodesStore.LookupEntry(path[i - 1]);
            TaxiNodesEntry const* b = sTaxiNodesStore.LookupEntry(path[i]);
            if (a && b)
                flightYd += std::hypot(a->x - b->x, a->y - b->y);
        }
        if (bot->GetMoney() < fare)
            return false;
        Leader::RouteResult const toMaster = Leader::Route(bot, master->pos);
        if (!toMaster.found || toMaster.throughRefused)
            return false;
        Leader::RouteResult const off = Leader::Route(bot, G3D::Vector3(to->x, to->y, to->z),
                                                      G3D::Vector3(dest.GetPositionX(), dest.GetPositionY(),
                                                                   dest.GetPositionZ()));
        if (!off.found || off.throughRefused)
            return false;
        float const run = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        float const taxi = std::max(sWorld->getFloatConfig(CONFIG_TAXI_FLIGHT_SPEED), 1.0f);
        float const fly = toMaster.length / run + flightYd / taxi + std::max(toGap, off.length) / run;
        float const walk = Leader::WalkYd(bot, bot->GetPosition(), dest) / run;
        if (fly >= walk && !walkRefused)  // [fly97] no walk there: the flight at any length
            return false;  // it would walk, and the walk is refused
        plan = Acore::StringFormat("by air from {} to {} ({} c, {:.0f} s against {:.0f} s on foot)", master->pos.ToString(),
                                   to->name[0], fare, fly, walk);
        return true;
    }

    // [fly109] the member's flight from its nearest known flight master to its known node nearest the destination
    // (FlightTripSafe judged the trip): its New RPG flight status, its own fare
    bool StartMemberFlight(Player* member, WorldPosition const& dest)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
        TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(member);
        if (!ai || !master || !master->taxiNodeId || member->IsInFlight() ||
            ai->rpgInfo.GetStatus() == RPG_TRAVEL_FLIGHT)
            return false;
        TaxiNodesEntry const* to = nullptr;
        float toGap = std::numeric_limits<float>::max();
        for (uint32 i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
            if (!node || node->map_id != dest.GetMapId() || !member->m_taxi.IsTaximaskNodeKnown(node->ID))
                continue;
            float const gap = std::hypot(node->x - dest.GetPositionX(), node->y - dest.GetPositionY());
            if (gap < toGap)
            {
                toGap = gap;
                to = node;
            }
        }
        if (!to || to->ID == master->taxiNodeId)
            return false;
        std::vector<uint32> path = sTravelNodeMap.FindKnownTaxiPath(member, master->taxiNodeId, to->ID);
        if (path.size() < 2)
            return false;
        ai->rpgInfo.ChangeToTravelFlight(master->templateEntry, master->pos, path);
        return true;
    }
}

bool NewRpgBaseAction::TripRefused(Player* bot, float x, float y, float z, std::string const& what)
{
    if (WalkGivenUp(bot, x, y))  // [holds9]
        return true;
    if (!bot || !SoloSafety::Applies(bot))
        return false;
    if (InDangerZone(bot, x, y))
        return true;
    // [red73] a destination in a zone red for it (the zone's or the area's own level, AreaTable area_level): no trip
    // there, from outside it. b393 Gilduck L14 walked to Darkshire for q116 Dry Times (its Bottle of Moonshine at
    // Barkeep Hann): bound there, dead 5x in b406 to Brightwood Grove's Nightbane Dark Runners L28 between the town and
    // its Westfall quests. zone38 judged only the areas the walk passes, never the destination's own
    {
        Map* map = bot->GetMap();
        uint32 const phase = bot->GetPhaseMask();
        uint32 const ownTeam = bot->GetTeamId() == TEAM_ALLIANCE ? AREATEAM_ALLY : AREATEAM_HORDE;
        auto const redAt = [&](float ax, float ay, float az, std::string* name) -> bool
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(map->GetAreaId(phase, ax, ay, az));
            AreaTableEntry const* zone = area && area->zone ? sAreaTableStore.LookupEntry(area->zone) : nullptr;
            // [red96] a town (AREA_FLAG_TOWN: an inn, its guards) of the bot's faction or of both is no red zone,
            // whatever its zone's level: b414 Selalleas L15's repair walks to Astranaar (area level 20, Ashenvale)
            // were refused 39 looks in turn with Windshear Crag / Stonetalon Peak / Silverwind Refuge / Nijel's Point,
            // while five 6 waited for it
            if (area && (area->flags & AREA_FLAG_TOWN) && (!area->team || area->team == ownTeam))
                return false;
            (void)ownTeam;
            for (AreaTableEntry const* a : {area, zone})
                if (a && a->area_level > 0 &&
                    Acore::XP::GetColorCode(bot->GetLevel(), uint8(std::min(a->area_level, 255))) == XP_RED)
                {
                    if (name)
                        *name = Acore::StringFormat("{} (level {})", a->area_name[0], a->area_level);
                    return true;
                }
            return false;
        };
        std::string where;
        if (map && redAt(x, y, z, &where) && !redAt(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                                    nullptr))
        {
            // [red96] one line per bot, destination area and level (the vendor searches look at the same far vendors
            // every look): a log cadence, no decision reads it
            thread_local std::set<std::tuple<uint64, std::string, uint8>> redLogged;
            if (redLogged.insert({bot->GetGUID().GetRawValue(), what + where, bot->GetLevel()}).second)
            {
                LOG_INFO("playerbots", "[red73] {} L{} TRIP_REFUSED {} at ({:.0f},{:.0f}): it lies in {}, red for it",
                         bot->GetName(), bot->GetLevel(), what, x, y, where);
            }
            return true;
        }
    }
    // [travel57] a destination on another landmass of its map with no route there at all (every dock / portal walk
    // of its landmass given up: [travel57] in TravelRouteMgr::Solve) is refused: TripDanger names no danger for it
    // and the walk itself only fails there (b397 Salleas L12: GO_GRIND to five spots by q982's Darkshore giver in
    // turn, each a MAZE / MOVE_FAR_LOOP at the Darnassus portal walk, STUCK)
    // [lag339] ... and one there is: the trip's own first leg is what it walks from here - the walk to the pier or
    // portal its route takes (judged as any walk, below); the straight line over the water to the far spot is no
    // walk (TripDanger read the mobs under it). b445 Dekorhan L22 (the guild's scribe) at Grove of the Ancients:
    // "TRIP_REFUSED town task PROFESSION Darnassus at (10138,2314) 6630 yd: its walk passes Wrathtail Sorceress
    // L18-19" - the Virtuoso Inking Set his Inscription crafts need is sold only in Darnassus for him
    float tx = x, ty = y, tz = z;
    if (TravelRouteMgr::Landmass(WorldPosition(bot)) !=
        TravelRouteMgr::Landmass(WorldPosition(bot->GetMapId(), x, y, z)))
    {
        TravelRouteMgr::Leg leg;
        float seconds = 0.0f;
        if (!sTravelRouteMgr.FindRoute(bot, WorldPosition(bot->GetMapId(), x, y, z), leg, seconds))
        {
            LOG_INFO("playerbots", "[travel57] {} L{} TRIP_REFUSED {} at ({:.0f},{:.0f}): on another landmass, no "
                     "route there (its walks to every dock and portal of its landmass given up)",
                     bot->GetName(), bot->GetLevel(), what, x, y);
            return true;
        }
        WorldPosition start;
        if (leg.kind == TravelRouteMgr::LegKind::Portal)
        {
            std::vector<TravelRouteMgr::Portal> const& portals = sTravelRouteMgr.GetPortals();
            if (leg.portalIndex < portals.size())
                start = portals[leg.portalIndex].from;
        }
        else if (leg.kind == TravelRouteMgr::LegKind::Transport)
            if (TravelRouteMgr::Line const* line = sTravelRouteMgr.GetLine(leg.lineEntry);
                line && leg.fromDock < line->docks.size())
                start = line->docks[leg.fromDock].pier;
        if (start == WorldPosition() || start.GetMapId() != bot->GetMapId())
            return false;  // no walk of its own to judge here: the ride's legs are judged as they come
        tx = start.GetPositionX();
        ty = start.GetPositionY();
        tz = start.GetPositionZ();
    }
    static std::mutex lock;
    // bot -> destination (map, x, y) -> (the level it was judged at, refused)
    static std::unordered_map<uint32, std::map<std::tuple<uint32, int32, int32>, std::pair<uint8, bool>>> judged;
    x = tx;  // [lag339] the walk judged from here on: to the far spot, else to its trip's first leg
    y = ty;
    z = tz;
    std::tuple<uint32, int32, int32> const key{bot->GetMapId(), int32(x), int32(y)};
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const& trips = judged[bot->GetGUID().GetCounter()];
        auto const found = trips.find(key);
        if (found != trips.end() && found->second.first == bot->GetLevel())
            return found->second.second;
    }
    CreatureTemplate const* danger = TripDanger(bot, x, y, z);
    if (danger)
        LOG_INFO("playerbots", "[solo] {} L{} TRIP_REFUSED {} at ({:.0f},{:.0f}) {:.0f} yd: its walk passes {} L{}-{}",
                 bot->GetName(), bot->GetLevel(), what, x, y, bot->GetExactDist(x, y, z), danger->Name,
                 danger->minlevel, danger->maxlevel);
    // [trip37] the trip is priced like any walk: a destination in an orange / red or elite roamer's round, or one
    // whose only walk passes a reach the bot must never fight (no complete walk round), is refused. b388: vendor runs
    // into camps - Kianey L14's repair walk to Gavin Gnarltree among the Nightbane Dark Runners (6 deaths),
    // Aestymro's "buy drink" walk through the Longshore murlocs, Guinudrond and Manue through the Riverpaw gnolls
    bool refused = danger != nullptr;
    if (!refused)
    {
        std::string roamer;
        if (SoloSafety::RoamerReaches(bot, G3D::Vector3(x, y, z), roamer))
        {
            refused = true;
            LOG_INFO("playerbots", "[trip37] {} L{} TRIP_REFUSED {} at ({:.0f},{:.0f}): it lies in the round of {}",
                     bot->GetName(), bot->GetLevel(), what, x, y, roamer);
        }
        else if (Leader::RouteResult const route = Leader::Route(bot, Position(x, y, z));
                 route.found && route.throughRefused)
        {
            // [trip52] ... unless it goes by air, the walks to and off the flight both safe
            std::string plan;
            if (FlightTripSafe(bot, WorldPosition(bot->GetMapId(), x, y, z), plan))
            {
                LOG_INFO("playerbots", "[trip52] {} L{} trip {} at ({:.0f},{:.0f}): its walk passes a refused reach, "
                         "taken {}",
                         bot->GetName(), bot->GetLevel(), what, x, y, plan);
                std::lock_guard<std::mutex> guard(lock);
                judged[bot->GetGUID().GetCounter()][key] = {bot->GetLevel(), false};
                return false;
            }
            refused = true;
            LOG_INFO("playerbots", "[trip37] {} L{} TRIP_REFUSED {} at ({:.0f},{:.0f}): its only walk passes the reach "
                     "of a mob it must not fight",
                     bot->GetName(), bot->GetLevel(), what, x, y);
        }
    }
    std::lock_guard<std::mutex> guard(lock);
    judged[bot->GetGUID().GetCounter()][key] = {bot->GetLevel(), refused};
    return refused;
}

void NewRpgBaseAction::RecordDeath(Player* bot, Creature* killer)
{
    if (!bot || !killer || !GET_PLAYERBOT_AI(bot))
        return;
    // the killer's aggro radius for this bot (Creature::GetAggroRange)
    float const radius = std::max(killer->GetAggroRange(bot), killer->GetCombatReach() + bot->GetCombatReach());
    std::string saved;
    {
        std::lock_guard<std::mutex> guard(deathZonesLock);
        auto& list = deathZones[bot->GetGUID().GetCounter()];
        // [danger29] the zone holds until the bot outlevels both the killer and itself at its death: a death to a
        // mob below its level (a pack, a camp) was in force "until L<killer+1>", a level the bot already had - void
        // at once. b378: 7 of 42 such marks void; Guinudrond L13 died 4 times in the Moonbrook Defias camp (L12 mobs),
        // each rise walked it back ("kept away until L13" at L13), Peny 3, Jurdogs 3 there too
        list.push_back({bot->GetMapId(), killer->GetPositionX(), killer->GetPositionY(), bot->GetPositionX(),
                        bot->GetPositionY(), killer->GetEntry(),
                        uint8(std::max<uint32>(killer->GetLevel(), bot->GetLevel())), radius});
        // [guild15] its deaths outlive a restart (b327-b331: Baridrol L9 died to the Gnarlpine at (9891,1553) in
        // every run, Cassand to the Stone Cairn Lake murlocs - the zones were forgotten at each restart); the
        // newest that fit playerbots_db_store's 255 characters, still in force (its level not above the killer's)
        for (auto it = list.rbegin(); it != list.rend(); ++it)
        {
            if (it->refused || bot->GetLevel() > it->level)
                continue;
            std::string const part = Acore::StringFormat("{},{:.0f},{:.0f},{:.0f},{:.0f},{},{},{:.0f};", it->mapId,
                                                         it->killerX, it->killerY, it->deathX, it->deathY,
                                                         it->entry, uint32(it->level), it->radius);
            if (saved.size() + part.size() > 250)
                break;
            saved += part;
        }
    }
    {
        uint32 const guid = bot->GetGUID().GetCounter();
        PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
        trans->Append(Acore::StringFormat("DELETE FROM playerbots_db_store WHERE guid = {} AND `key` = 'dz'", guid));
        trans->Append(Acore::StringFormat(
            "INSERT INTO playerbots_db_store (guid, `key`, value) VALUES ({}, 'dz', '{}')", guid, saved));
        PlayerbotsDatabase.CommitTransaction(trans);
    }
    LOG_INFO("playerbots", "[rpg] {} L{} DANGER remembered: {} L{} at ({:.0f},{:.0f}), {:.0f} yd, kept away until L{} [danger29]",
             bot->GetName(), bot->GetLevel(), killer->GetName(), killer->GetLevel(), killer->GetPositionX(),
             killer->GetPositionY(), radius, std::max<uint32>(killer->GetLevel(), bot->GetLevel()) + 1);
    // the quest it was on goes back to the planner: its spots there are now judged dangerous
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        if (botAI->rpgInfo.GetStatus() == RPG_DO_QUEST)
            botAI->rpgInfo.ChangeToIdle();
    SoloSafety::OnDeath(bot);  // [guild13] back to the hub and rest to full after the revive
}

void NewRpgBaseAction::LoadDeathZones(Player* bot, std::string const& value)
{
    if (!bot)
        return;
    std::vector<DeathZone> loaded;
    size_t start = 0;
    while (start < value.size())
    {
        size_t const end = value.find(';', start);
        std::string const part = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? value.size() : end + 1;
        unsigned mapId = 0, entry = 0, level = 0;
        float kx = 0, ky = 0, dx = 0, dy = 0, radius = 0;
        if (sscanf(part.c_str(), "%u,%f,%f,%f,%f,%u,%u,%f", &mapId, &kx, &ky, &dx, &dy, &entry, &level, &radius) != 8)
            continue;
        if (bot->GetLevel() > level)
            continue;  // outlevelled
        loaded.push_back({mapId, kx, ky, dx, dy, entry, uint8(level), radius});
    }
    if (loaded.empty())
        return;
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto& list = deathZones[bot->GetGUID().GetCounter()];
    list.insert(list.end(), loaded.begin(), loaded.end());
    LOG_INFO("playerbots", "[rpg] {} L{} DANGER remembered from its last session: {} death zone(s)", bot->GetName(),
             bot->GetLevel(), loaded.size());
}

void NewRpgBaseAction::ForgetRefusedZones(Player* bot)
{
    if (!bot)
        return;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> guard(deathZonesLock);
        auto const zones = deathZones.find(bot->GetGUID().GetCounter());
        if (zones == deathZones.end())
            return;
        auto& list = zones->second;
        size_t const before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [](DeathZone const& zone) { return zone.refused; }),
                   list.end());
        dropped = before - list.size();
    }
    if (dropped)
        LOG_INFO("playerbots", "[rpg] {} L{} {} refused-pull zone(s) judged again (its gear, spells or talents changed)",
                 bot->GetName(), bot->GetLevel(), dropped);
}

void NewRpgBaseAction::RememberDanger(Player* bot, float x, float y, float radius, uint32 entry, uint8 level,
                                      bool restClears)
{
    if (!bot)
        return;
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto& list = deathZones[bot->GetGUID().GetCounter()];
    for (DeathZone& zone : list)
        if (zone.mapId == bot->GetMapId() && zone.entry == entry &&
            SegmentDistSq(zone.killerX, zone.killerY, x, y, x, y) <= zone.radius * zone.radius)
        {
            zone.restClears = zone.restClears && restClears;  // [deaths143] a loss that is not mana's keeps it
            return;
        }
    // [danger29] in force until the bot outlevels the pack or itself at the refusal (a pack below its level was void)
    list.push_back({bot->GetMapId(), x, y, x, y, entry, uint8(std::max<uint32>(level, bot->GetLevel())), radius, true,
                    restClears});
}

void NewRpgBaseAction::ForgetRefusals(Player* bot)
{
    if (!bot)
        return;
    {
        std::lock_guard<std::mutex> guard(refusedDestLock);  // [guild15] the danger changed: judged again
        refusedDest.erase(bot->GetGUID().GetCounter());
        lookedDest.erase(bot->GetGUID().GetCounter());
    }
    {
        std::lock_guard<std::mutex> guard(givenUpLock);  // [regress10] the walks given up among them too
        givenUp.erase(bot->GetGUID().GetCounter());
    }
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto const found = deathZones.find(bot->GetGUID().GetCounter());
    if (found == deathZones.end())
        return;
    auto& list = found->second;
    size_t const before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [](DeathZone const& zone) { return zone.refused; }),
               list.end());
    if (before != list.size())
        LOG_INFO("playerbots", "[solo] {} L{} repaired: {} refused pulls judged again", bot->GetName(),
                 bot->GetLevel(), before - list.size());
}

CreatureTemplate const* NewRpgBaseAction::InDangerZone(Player* bot, float x, float y)
{
    if (!bot)
        return nullptr;
    uint8 const level = bot->GetLevel();
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto const zones = deathZones.find(bot->GetGUID().GetCounter());
    if (zones == deathZones.end())
        return nullptr;
    ClearRestedZones(bot, zones->second);  // [deaths143]
    for (DeathZone const& zone : zones->second)
    {
        if (zone.mapId != bot->GetMapId() || level > zone.level)
            continue;
        float const radiusSq = zone.radius * zone.radius;
        if (SegmentDistSq(zone.killerX, zone.killerY, x, y, x, y) <= radiusSq ||
            SegmentDistSq(zone.deathX, zone.deathY, x, y, x, y) <= radiusSq)
            return sObjectMgr->GetCreatureTemplate(zone.entry);
    }
    return nullptr;
}

bool NewRpgBaseAction::LoginEscape()
{
    // [nav24] a login where the navmesh has no polygon under the bot: the alarm names the saved spot (b350
    // Blahadrin logged in inside a 12x12 yd model footprint at (-9463,-350,60), its b349 end in Westfall - no log
    // showed the move that put it there; the relocations without a path now log [nav24] lines)
    if (dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery())
    {
        dtQueryFilter filter;
        filter.setIncludeFlags(uint16(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP));
        float const point[3] = {bot->GetPositionY(), bot->GetPositionZ(), bot->GetPositionX()};
        float const extents[3] = {3.0f, 5.0f, 3.0f};
        float nearest[3] = {0.0f, 0.0f, 0.0f};
        dtPolyRef ref = 0;
        // [nav28] in water at the saved spot (its swim flag is not set yet at the first look): the liquid itself
        // says so - b377 Dekorhan's "6 yd in the air" at (6297,624,-6.7) was a swim off the Darkshore coast
        LiquidData const liquid = bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), bot->GetPositionX(),
                                                               bot->GetPositionY(), bot->GetPositionZ(),
                                                               bot->GetCollisionHeight(), {});
        bool const inLiquid = liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > bot->GetPositionZ() - 1.0f;
        if (!bot->GetTransport() && !bot->IsInWater() && !inLiquid && !bot->IsInFlight() &&
            (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) || !ref))
        {
            NavGap(bot, "logged in where no polygon lies under it (the saved position of its last session)");
            // [nav28] saved in the air (a session that ended on a flight whose path the login did not resume: b377
            // Rumanatol 91 yd over Azuremyst): it falls, as a player whose flight broke does - no hanging there
            if (!PlayerbotAI::HasGroundSupport(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
            {
                // [nav45] no ground at all under it (the map's height lookup finds none: under the world's floor,
                // saved there by an earlier session): no fall lands anywhere - it leaves by its Hearthstone. b392
                // Ryarii L11 logged in at (6572,781,-53) under the Darkshore sea floor, "199947 yd in the air", and
                // stood there both windows (STUCK 2271 s)
                if (bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) <=
                    INVALID_HEIGHT)
                {
                    LOG_INFO("playerbots", "[nav45] {} L{} logged in with no ground under it at ({:.0f},{:.0f},{:.0f}): "
                             "its Hearthstone as the last resort (the cause, a dive under the bed, is [seabed46])",
                             bot->GetName(), bot->GetLevel(), bot->GetPositionX(), bot->GetPositionY(),
                             bot->GetPositionZ());
                    if (HearthOut("logged in with no ground under it (under the world's floor)"))
                        return true;
                }
                LOG_INFO("playerbots", "[nav28] {} L{} logged in {:.0f} yd in the air (not on a flight): falls",
                         bot->GetName(), bot->GetLevel(),
                         bot->GetPositionZ() - bot->GetMapWaterOrGroundLevel(bot->GetPositionX(),
                                                                             bot->GetPositionY(),
                                                                             bot->GetPositionZ()));
                botAI->RequestFallCheck();  // its fall runs at the next update (StartFall)
                return true;
            }
        }
    }
    // [group38] ... a group's leader too (its group follows it out): b388 Gnetlibozz's open-world group logged in
    // inside the Stonesplinter camp at (-4923,-3424), no escape for a leader that is not alone - Gilduck and
    // Blahadrin died there within the first minutes
    if (bot->IsInCombat() || !(SoloSafety::Applies(bot) || NewRpgCoop::JudgesPulls(bot)))
        return false;  // in a fight already: the survival kit judges it (fight on or flee)
    // [cleft62] logged in inside the other faction's capital (its guards L75 everywhere): the core put it at the
    // instance entrance (Player::LoadFromDB, a dungeon instance gone at the restart) - out by its Hearthstone, as a
    // player leaves. b393: the five in Ragefire Chasm was in it at the server stop; Alliance Orenem L15 / Brelir L16
    // logged in in the Cleft of Shadow, Orgrimmar, died to Orgrimmar Grunts, ghosts there every run since
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        uint32 const own = bot->GetTeamId() == TEAM_ALLIANCE ? AREATEAM_ALLY : AREATEAM_HORDE;
        if ((zone->flags & AREA_FLAG_CAPITAL) && zone->team && zone->team != own && zone->team != AREATEAM_ANY &&
            HearthOut(std::string("[cleft62] logged in in the enemy capital ") + zone->area_name[0]))
            return true;
        // [red85] ... or in a zone or subzone the other faction holds: b410 Alnia L16 / Dekorhan L16 (Alliance) logged
        // in at (1347,-4382) by Orgrimmar's gate in Durotar and died to an Orgrimmar Grunt L75 before their first walk
        // [red85] user 2026-11-10: by the fact, not the zone - a hostile guard (CREATURE_FLAG_EXTRA_GUARD: an Orgrimmar
        // Grunt, a Barrens Guard, a Horde Elite) whose aggro reach covers its login spot
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnitInObjectRangeCheck nearCheck(bot, MAX_AGGRO_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> nearSearcher(bot, nearbyUnits, nearCheck);
        Cell::VisitObjects(bot, nearSearcher, MAX_AGGRO_RADIUS);
        for (Unit* unit : nearbyUnits)
            if (Creature* guard = unit->ToCreature();
                guard && guard->IsAlive() && guard->IsGuard() && guard->IsHostileTo(bot) &&
                guard->IsWithinDistInMap(bot, guard->GetAggroRange(bot) + bot->GetCombatReach()) &&
                HearthOut(std::string("[red85] logged in in the reach of the guard ") + guard->GetName()))
                return true;
    }
    std::string who;
    std::vector<G3D::Vector3> spots;
    if (!SoloSafety::StandsInReach(bot, who, spots))
        return false;
    for (G3D::Vector3 const& at : spots)
    {
        Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
        if (!route.found || !route.complete || route.throughRefused)
            continue;
        if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
            continue;
        LOG_INFO("playerbots", "[rpg] {} L{} LOGIN_ESCAPE reason: logged in within the reach of {}: steps to "
                 "({:.0f},{:.0f}), {:.0f} yd walk",
                 bot->GetName(), bot->GetLevel(), who, at.x, at.y, route.length);
        return true;
    }
    // [login26] coordinator 2026-11-09: a login inside a camp leaves at once by the safest way out, not stays: no way
    // clear of every reach - the shortest complete one out (the reaches it crosses are the ones it stands in already;
    // staying keeps it in all of them). b375: 6 deaths "logged in inside the camp"
    G3D::Vector3 const* best = nullptr;
    float bestYd = 0.0f;
    for (G3D::Vector3 const& at : spots)
    {
        Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
        if (!route.found || !route.complete)
            continue;
        if (!best || route.length < bestYd)
        {
            best = &at;
            bestYd = route.length;
        }
    }
    if (best && MoveTo(bot->GetMapId(), best->x, best->y, best->z, false, false, false, true))
    {
        LOG_INFO("playerbots", "[login26] {} L{} logged in inside the reach of {}: no way out clear of every reach, "
                 "leaves by the shortest one to ({:.0f},{:.0f}), {:.0f} yd",
                 bot->GetName(), bot->GetLevel(), who, best->x, best->y, bestYd);
        return true;
    }
    LOG_INFO("playerbots", "[rpg] {} L{} LOGIN_ESCAPE reason: logged in within the reach of {}: no step out on a "
             "walk it takes ({} spots looked at) - stays, the survival kit judges what comes",
             bot->GetName(), bot->GetLevel(), who, spots.size());
    return false;
}

// [deaths13] a hurt bot out of a fight stands in an idle mob's reach (a wanderer's area, a patrol's beat): it rests out
// of every reach - its rest there lets the wanderer walk in at reduced health. b353 Guinudrond L11: after a won fight
// (took 100 of 232) a Stonesplinter Trogg wandering 20 yd off (wander 3, aggro 19) walked into it, the fight began at
// reduced health and mana, it died ("no judged pull (attacked)")
// [red63] b400: 9 of the 22 "no judged pull (attacked)" deaths in Brightwood Grove (Duskwood, area level 28):
// Manue L15 x4, Gilduck L14 x2, Guinudrond L14 x3 to Nightbane Dark Runners L28-29 (wanderers, aggro 32), Black
// Ravagers, Green Recluses. They had logged in there (an earlier session's walk) and rose at their corpses there; every
// walk out was refused by zone38 (Darkshire 25, Raven Hill 24, Addle's Stead 24 - all red for L14-15), the route search
// answered "no route" (not a wall: WalledIn and its Hearthstone never ran), so each stood until a wanderer walked in
bool NewRpgBaseAction::RedAreaEscape()
{
    // [red135] its flight out is planned (TRAVEL_FLIGHT): the flight's own action walks to the master and takes it.
    // b428 Guinudrond L14 stood in Darkshire 2.8 h: this escape re-planned the flight every tick (4628 "[red73] ...
    // flies out") and returned true before the TRAVEL_FLIGHT action ever ran, walked 0 yd
    if (botAI->rpgInfo.GetStatus() == RPG_TRAVEL_FLIGHT)
        return false;
    if (bot->IsInCombat() || !bot->IsAlive() || bot->isMoving() || bot->IsInFlight() || bot->GetTransport() ||
        !SoloSafety::Applies(bot) || bot->GetMap()->Instanceable())
        return false;
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
    if (!area || area->area_level <= 0 ||
        Acore::XP::GetColorCode(bot->GetLevel(), uint8(std::min(area->area_level, 255))) != XP_RED)
        return false;
    WorldPosition const bind(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ);
    WorldPosition const out = bind.GetMapId() == bot->GetMapId() ? bind : NearestHub(bot);
    if (out == WorldPosition() || out.GetMapId() != bot->GetMapId())
        return false;
    // one route search per area and level it stands in (the answer holds while it stands there: a refused walk out
    // does not move it); map thread, per bot
    thread_local std::unordered_map<uint64, std::tuple<uint32, uint8, bool>> judged;
    auto& [jArea, jLevel, refused] = judged[bot->GetGUID().GetRawValue()];
    if (jArea != area->ID || jLevel != bot->GetLevel())
    {
        BotPath const walk = FindBotPath(bot, out.GetPositionX(), out.GetPositionY(), out.GetPositionZ());
        jArea = area->ID;
        jLevel = bot->GetLevel();
        refused = !(walk.points.size() >= 2 && !walk.throughRefused && walk.type != PATHFIND_NOPATH);
    }
    // [red134] a walk out that is not refused still crosses the red area's wanderers, judged mob by mob: b426 Gilduck
    // L14 rose at its corpse by Darkshire (area level 25) and walked out through Brightwood Grove to his Westfall
    // quests - 5 deaths to Nightbane Dark Runners and Black Ravagers L24-28, all "attacked, no judged pull" (b406 the
    // same 5). Its Hearthstone or a flight comes first, as a player leaves a zone far above it; the walk only without
    std::string const why =
        refused ? Acore::StringFormat("[red63] stands in {} (area level {}, red for L{}) and its walk out to "
                                      "({:.0f},{:.0f}) is refused",
                                      area->area_name[0], area->area_level, bot->GetLevel(), out.GetPositionX(),
                                      out.GetPositionY())
                : Acore::StringFormat("[red134] stands in {} (area level {}, red for L{}): its walk out to ({:.0f},"
                                      "{:.0f}) crosses the area",
                                      area->area_name[0], area->area_level, bot->GetLevel(), out.GetPositionX(),
                                      out.GetPositionY());
    if (HearthOut(why, nullptr))
        return true;
    // [night305] the town's flight master first, when its node is unknown: the flight below starts there
    if (DiscoverTownFlightMaster(why))
        return true;
    // [red73] no Hearthstone way out (bound here, none, on its cooldown): a flight from the town's flight master to its
    // nearest known node outside every area red for it, as a player leaves (b406 Gilduck L14 in Darkshire, bound
    // there, every walk out through Brightwood Grove)
    TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!master || !master->taxiNodeId || master->pos.GetMapId() != bot->GetMapId())
        return false;
    uint32 bestNode = 0;
    float bestYd = 0.0f;
    for (uint32 node = 1; node < sTaxiNodesStore.GetNumRows(); ++node)
    {
        TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(node);
        if (!entry || node == master->taxiNodeId || entry->map_id != bot->GetMapId() ||
            !bot->m_taxi.IsTaximaskNodeKnown(node))
            continue;
        uint32 const nodeArea = bot->GetMap()->GetAreaId(bot->GetPhaseMask(), entry->x, entry->y, entry->z);
        AreaTableEntry const* at = sAreaTableStore.LookupEntry(nodeArea);
        AreaTableEntry const* atZone = at && at->zone ? sAreaTableStore.LookupEntry(at->zone) : nullptr;
        bool red = false;
        for (AreaTableEntry const* a : {at, atZone})
            if (a && a->area_level > 0 &&
                Acore::XP::GetColorCode(bot->GetLevel(), uint8(std::min(a->area_level, 255))) == XP_RED)
                red = true;
        if (red)
            continue;
        float const yd = std::hypot(entry->x - bot->GetPositionX(), entry->y - bot->GetPositionY());
        if (!bestNode || yd < bestYd)
        {
            bestNode = node;
            bestYd = yd;
        }
    }
    if (!bestNode)
        return false;
    if (!FlyTowards(bestNode))
        return false;
    LOG_INFO("playerbots", "[red73] {} L{} {}: flies out from the flight master of node {} to known node {} ({:.0f} yd)",
             bot->GetName(), bot->GetLevel(), why, master->taxiNodeId, bestNode, bestYd);
    return true;
}

// [night305] coordinator 2026-11-11: a player passing through a town talks to its flight master and so learns the
// node; the bots learned one only within DISCOVER_FLIGHT_MASTER_DISTANCE of a master their walk happened to pass.
// b432 Gilduck L14 stood 1.15 h in Darkshire (bound there, every walk out refused) 100 yd from Felicia Maline with
// the Darkshire node unknown: [red73]'s flight out needs the start node, so it never flew
bool NewRpgBaseAction::DiscoverTownFlightMaster(std::string const& why)
{
    if (bot->IsInCombat() || !bot->IsAlive() || bot->isMoving() || bot->IsInFlight() || bot->GetTransport() ||
        bot->GetMap()->Instanceable() || !GuildHonest::IsHonestBot(bot))
        return false;
    // the town it stands in: a town (an inn, its guards) or a capital, the master in the bot's own area
    uint32 const areaId = bot->GetAreaId();
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    AreaTableEntry const* zone = area && area->zone ? sAreaTableStore.LookupEntry(area->zone) : area;
    if (!area || !((area->flags & AREA_FLAG_TOWN) || (zone && (zone->flags & AREA_FLAG_CAPITAL))))
        return false;
    TravelMgr::FlightMasterInfo const* master = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!master || !master->taxiNodeId || master->pos.GetMapId() != bot->GetMapId() ||
        bot->m_taxi.IsTaximaskNodeKnown(master->taxiNodeId) ||
        bot->GetMap()->GetAreaId(bot->GetPhaseMask(), master->pos.GetPositionX(), master->pos.GetPositionY(),
                                 master->pos.GetPositionZ()) != areaId)
        return false;
    // once per node and level (a walk that ends without the node is not taken again until a fact changes); map thread
    thread_local std::unordered_map<uint64, std::pair<uint32, uint8>> tried;
    auto& last = tried[bot->GetGUID().GetRawValue()];
    if (last.first == master->taxiNodeId && last.second == bot->GetLevel())
        return false;
    last = {master->taxiNodeId, bot->GetLevel()};
    float const x = master->pos.GetPositionX(), y = master->pos.GetPositionY(), z = master->pos.GetPositionZ();
    if (WalkGivenUp(bot, x, y) || TripRefused(bot, x, y, z, "flight master"))
        return false;
    TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(master->taxiNodeId);
    LOG_INFO("playerbots", "[night305] {} L{} walks to the flight master of {} (node {}, {:.0f} yd) to learn its "
             "node: {}",
             bot->GetName(), bot->GetLevel(), node ? node->name[0] : "?", master->taxiNodeId,
             bot->GetExactDist(x, y, z), why);
    botAI->rpgInfo.ChangeToGoCamp(master->pos);
    return true;
}

bool NewRpgBaseAction::RestEscape()
{
    // [rest64] a co-op / help party's member too: b400 Namwe L13 and Galiver L13 (help party, q2038, Stonesplinter camp
    // in Loch Modan) rested and waited at (-5034,-3361), 18 yd from a Stonesplinter Seer's home (aggro 19) - Galiver
    // "UNJUDGED fight ... status REST", both dead, then Geron L15 twice at the same spot; the party's members were
    // never "solo" (SoloSafety::Applies), so no rest spot out of every reach was taken
    if (bot->IsInCombat() || bot->isMoving() ||
        !(SoloSafety::Applies(bot) || NewRpgCoop::InParty(bot->GetGUID().GetCounter())))
        return false;
    bool const hurt = bot->GetHealth() < bot->GetMaxHealth() ||
                      (bot->GetMaxPower(POWER_MANA) && bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA));
    if (!hurt || !(SoloSafety::WantsRest(bot, false) || SoloSafety::WantsRest(bot, true) || bot->IsSitState()))
        return false;
    std::string who;
    std::vector<G3D::Vector3> spots;
    if (!SoloSafety::StandsInReach(bot, who, spots))
        return false;
    for (G3D::Vector3 const& at : spots)
    {
        Leader::RouteResult const route = Leader::Route(bot, Position(at.x, at.y, at.z));
        if (!route.found || !route.complete || route.throughRefused)
            continue;
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        if (!MoveTo(bot->GetMapId(), at.x, at.y, at.z, false, false, false, true))
            continue;
        LOG_INFO("playerbots", "[deaths13] {} L{} REST_ESCAPE reason: hurt (hp {:.0f} %) within the reach of {}: "
                 "rests at ({:.0f},{:.0f}), {:.0f} yd walk",
                 bot->GetName(), bot->GetLevel(), bot->GetHealthPct(), who, at.x, at.y, route.length);
        return true;
    }
    return false;
}

bool NewRpgBaseAction::InDeathZone(Player* bot, float x, float y)
{
    if (!bot)
        return false;
    uint8 const level = bot->GetLevel();
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto const zones = deathZones.find(bot->GetGUID().GetCounter());
    if (zones == deathZones.end())
        return false;
    ClearRestedZones(bot, zones->second);  // [deaths143]
    for (DeathZone const& zone : zones->second)
    {
        if (zone.refused || zone.mapId != bot->GetMapId() || level > zone.level)
            continue;
        float const radiusSq = zone.radius * zone.radius;
        if (SegmentDistSq(zone.killerX, zone.killerY, x, y, x, y) <= radiusSq ||
            SegmentDistSq(zone.deathX, zone.deathY, x, y, x, y) <= radiusSq)
            return true;
    }
    return false;
}

bool NewRpgBaseAction::SameDangerZone(Player* bot, float ax, float ay, float bx, float by)
{
    if (!bot)
        return false;
    uint8 const level = bot->GetLevel();
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto const zones = deathZones.find(bot->GetGUID().GetCounter());
    if (zones == deathZones.end())
        return false;
    ClearRestedZones(bot, zones->second);  // [deaths143]
    for (DeathZone const& zone : zones->second)
    {
        if (zone.mapId != bot->GetMapId() || level > zone.level)
            continue;
        float const radiusSq = zone.radius * zone.radius;
        auto const covers = [&](float x, float y)
        {
            return SegmentDistSq(zone.killerX, zone.killerY, x, y, x, y) <= radiusSq ||
                   SegmentDistSq(zone.deathX, zone.deathY, x, y, x, y) <= radiusSq;
        };
        if (covers(ax, ay) && covers(bx, by))
            return true;
    }
    return false;
}

void NewRpgBaseAction::DangerDiscs(Player* bot, float x, float y, float range, std::vector<G3D::Vector4>& out)
{
    if (!bot)
        return;
    uint8 const level = bot->GetLevel();
    std::lock_guard<std::mutex> guard(deathZonesLock);
    auto const zones = deathZones.find(bot->GetGUID().GetCounter());
    if (zones == deathZones.end())
        return;
    ClearRestedZones(bot, zones->second);  // [deaths143]
    for (DeathZone const& zone : zones->second)
    {
        if (zone.mapId != bot->GetMapId() || level > zone.level)
            continue;
        for (auto const& [zx, zy] : {std::pair<float, float>{zone.killerX, zone.killerY}, {zone.deathX, zone.deathY}})
            if (std::hypot(zx - x, zy - y) <= range + zone.radius)
                out.emplace_back(zx, zy, bot->GetPositionZ(), zone.radius);
    }
}

bool NewRpgBaseAction::NeedsItemUseCredit(Quest const* quest, std::string* why)
{
    if (!quest || !quest->GetSrcItemId())
        return false;
    bool creditObjective = false;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        creditObjective = creditObjective || (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i]);
    // [flit] an item objective other than the tool itself comes from the tool's use too (b341 Orenem q9452 Red Snapper:
    // the Draenei Fishing Net on a spell-focus school; 1500 yd swept, 8 camps "seen empty")
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        creditObjective = creditObjective || (quest->RequiredItemId[i] && quest->RequiredItemCount[i] &&
                                              quest->RequiredItemId[i] != quest->GetSrcItemId());
    if (!creditObjective)
        return false;
    if (NewRpgQuestUse::CoversToolObjectives(nullptr, quest))  // [questitems] the use step does them
        return false;
    ItemTemplate const* tool = sObjectMgr->GetItemTemplate(quest->GetSrcItemId());
    if (!tool)
        return false;
    for (auto const& spell : tool->Spells)
        if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
        {
            if (why)
                *why = Acore::StringFormat("its objective credit comes from using {} (spell {}), the bot does not use "
                                           "quest items",
                                           tool->Name1, spell.SpellId);
            return true;
        }
    return false;
}

namespace
{
    // [flit] creature entries some way gives the kill credit of that the bot can take part in: the entry's own kill,
    // another creature's kill (creature_template KillCredit), a SmartAI CALL_KILLEDMONSTER on any event but a spell
    // hit (a gossip talk, a death, an escort's end); built once
    std::unordered_set<uint32> const& CreditableEntries()
    {
        static std::unordered_set<uint32> entries;
        static std::once_flag built;
        std::call_once(built, []()
        {
            for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
            {
                for (uint32 const credit : tmpl.KillCredit)
                    if (credit)
                        entries.insert(credit);
                for (SmartScriptHolder const& e : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
                    if (e.GetActionType() == SMART_ACTION_CALL_KILLEDMONSTER &&
                        e.GetEventType() != SMART_EVENT_SPELLHIT && e.GetEventType() != SMART_EVENT_SPELLHIT_TARGET)
                        entries.insert(e.action.killedMonster.creature ? e.action.killedMonster.creature : entry);
            }
            for (auto const& [entry, goTemplate] : *sObjectMgr->GetGameObjectTemplates())
                for (SmartScriptHolder const& e :
                     sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_GAMEOBJECT))
                    if (e.GetActionType() == SMART_ACTION_CALL_KILLEDMONSTER && e.action.killedMonster.creature)
                        entries.insert(e.action.killedMonster.creature);
        });
        return entries;
    }
}

bool NewRpgBaseAction::NeedsSpellCredit(Player* bot, Quest const* quest, bool openOnly, std::string* why)
{
    FactionTemplateEntry const* botFaction = bot ? bot->GetFactionTemplateEntry() : nullptr;
    if (!quest || !botFaction)
        return false;
    QuestStatusData const* status = nullptr;
    if (openOnly)
    {
        auto const found = bot->getQuestStatusMap().find(quest->GetQuestId());
        status = found == bot->getQuestStatusMap().end() ? nullptr : &found->second;
    }
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const entry = quest->RequiredNpcOrGo[i];
        if (entry <= 0 || !quest->RequiredNpcOrGoCount[i] ||
            (status && status->CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i]))
            continue;
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(uint32(entry));
        FactionTemplateEntry const* faction = tmpl ? sFactionTemplateStore.LookupEntry(tmpl->faction) : nullptr;
        // a creature friendly to the bot is never its kill; with no other credit path it is a spell or script one
        // (b341 q9283 Rescue the Survivors!: Gift of the Naaru on a Draenei Survivor, a C++ script - 60 respawn
        // waits of 30 s for survivors that stood there alive)
        if (!faction || !faction->IsFriendlyTo(*botFaction) || CreditableEntries().count(uint32(entry)) ||
            NewRpgQuestUse::PlanFor(bot, quest, i))  // [questitems] the use step casts it
            continue;
        if (why)
            *why = Acore::StringFormat("its objective credit on the friendly {} comes from a spell or script, not a "
                                       "kill or a talk",
                                       tmpl->Name);
        return true;
    }
    return false;
}

bool NewRpgBaseAction::EnderNeedsScript(uint32 questId)
{
    // quest id -> its enders are all objects the world keeps despawned (gameobject.spawntimesecs < 0: a script
    // spawns them - b303 q310 Bitter Rivals' Unguarded Thunder Ale Barrel appears only after Jarven Thunderbrew's
    // gossip; 8 bots stood 5 min each at its empty spot), no creature ends it
    static std::unordered_set<uint32> quests;
    static std::once_flag built;
    std::call_once(built, []()
    {
        std::unordered_map<uint32, bool> allHidden;  // quest -> every ender spawn script-spawned so far
        std::unordered_map<uint32, bool> hiddenEntry;  // object entry -> all its spawns despawned
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        {
            auto const found = hiddenEntry.find(data.id);
            bool const hidden = data.spawntimesecs < 0;
            if (found == hiddenEntry.end())
                hiddenEntry[data.id] = hidden;
            else
                found->second = found->second && hidden;
        }
        for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestInvolvedRelationMap())
        {
            auto const found = hiddenEntry.find(entry);
            bool const hidden = found != hiddenEntry.end() && found->second;
            auto const quest = allHidden.find(questId);
            if (quest == allHidden.end())
                allHidden[questId] = hidden;
            else
                quest->second = quest->second && hidden;
        }
        for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
            allHidden[questId] = false;
        for (auto const& [questId, hidden] : allHidden)
            if (hidden)
                quests.insert(questId);
        LOG_INFO("playerbots", "[guild14] {} quests end only at script-spawned objects: not taken", quests.size());
    });
    return quests.count(questId) > 0;
}

bool NewRpgBaseAction::FishOnlyItem(Player* bot, Quest const* quest, uint32& itemId, WorldPosition& water)
{
    // [fish14] b354 Nelaeleas L12 took q1141 The Family and the Fishing Pole (6 Darkshore Grouper): its sweeps found
    // nothing that drops them ("no creature or chest holds item 12238") - the grouper is fishing loot only
    itemId = 0;
    water = WorldPosition();
    if (!bot || !quest)
        return false;
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const item = quest->RequiredItemId[i];
        if (!item || bot->GetItemCount(item, true) >= quest->RequiredItemCount[i])
            continue;
        uint32 skill = 0;
        if (index.itemCreatures.count(item) || index.itemObjects.count(item) ||
            !sGuildGatherPlan.Gatherable(item, &skill) || skill != SKILL_FISHING)
            continue;
        itemId = item;
        float best = std::numeric_limits<float>::max();
        for (GatherPlace const* place : sGuildGatherPlan.PlacesFor(item))
            if (place && place->kind == GatherKind::FISH && !place->instance && place->spot.GetMapId() == bot->GetMapId())
                if (float const d = bot->GetExactDist(place->spot); d < best)
                {
                    best = d;
                    water = place->spot;
                }
        return true;
    }
    return false;
}

bool NewRpgBaseAction::CanFishFor(Player* bot, uint32 itemId)
{
    int32 req = 0;
    uint32 skill = 0;
    if (!bot || !sGuildGatherPlan.Gatherable(itemId, &skill, &req) || skill != SKILL_FISHING ||
        !bot->HasSkill(SKILL_FISHING) || !NewRpgFishing::HasPole(bot))
        return false;
    return int32(bot->GetSkillValue(SKILL_FISHING)) >= req;
}

bool NewRpgBaseAction::IsSpellUnlockClassQuest(Quest const* quest)
{
    if (!quest || !quest->GetRequiredClasses())
        return false;
    // the quest or the rest of its chain rewards a spell (RewardSpell cast / RewardDisplaySpell learned)
    std::unordered_set<uint32> seen;
    for (Quest const* step = quest; step && seen.insert(step->GetQuestId()).second;)
    {
        if (step->GetRewSpell() || step->GetRewSpellCast() > 0)
            return true;
        uint32 const next = step->GetNextQuestInChain() ? step->GetNextQuestInChain() : step->GetNextQuestId();
        step = next ? sObjectMgr->GetQuestTemplate(next) : nullptr;
    }
    return false;
}

bool NewRpgBaseAction::NearestEnderSpot(Player* bot, uint32 questId, WorldPosition& out)
{
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    auto const enders = index.enders.find(questId);
    if (enders == index.enders.end())
        return false;
    float best = std::numeric_limits<float>::max();
    for (QuestSpot const& spot : enders->second)
    {
        if (spot.mapId != bot->GetMapId())
            continue;
        float const d = bot->GetDistance2d(spot.x, spot.y);
        if (d < best)
        {
            best = d;
            out = WorldPosition(spot.mapId, spot.x, spot.y, spot.z);
        }
    }
    return best != std::numeric_limits<float>::max();
}

WorldPosition NewRpgBaseAction::NearestHub(Player* bot)
{
    WorldPosition best;
    float bestDist = std::numeric_limits<float>::max();
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        auto const enders = questId ? index.enders.find(questId) : index.enders.end();
        if (enders == index.enders.end())
            continue;
        for (QuestSpot const& spot : enders->second)
        {
            if (spot.mapId != bot->GetMapId())
                continue;
            float const d = bot->GetDistance2d(spot.x, spot.y);
            if (d < bestDist && !DangerOnWay(bot, bot->GetPositionX(), bot->GetPositionY(), spot.x, spot.y))
            {
                bestDist = d;
                best = WorldPosition(spot.mapId, spot.x, spot.y, spot.z);
            }
        }
    }
    if (best != WorldPosition())
        return best;
    for (WorldLocation const& hub : sTravelMgr.GetTravelHubs(bot))
    {
        if (hub.GetMapId() != bot->GetMapId())
            continue;
        float const d = bot->GetExactDist(hub);
        if (d < bestDist)
        {
            bestDist = d;
            best = WorldPosition(hub);
        }
    }
    return best;
}

bool NewRpgBaseAction::TimedQuestFits(Quest const* quest) { return TimedQuestFits(bot, quest); }

bool NewRpgBaseAction::TimedQuestFits(Player* bot, Quest const* quest)
{
    QuestSpawnIndex const& spawnIndex = GetQuestSpawnIndex();
    auto const enders = spawnIndex.enders.find(quest->GetQuestId());
    float nearest = std::numeric_limits<float>::max();
    if (enders != spawnIndex.enders.end())
        for (QuestSpot const& spot : enders->second)
            if (spot.mapId == bot->GetMapId())
                nearest = std::min(nearest, bot->GetDistance2d(spot.x, spot.y));
    return nearest != std::numeric_limits<float>::max() &&
           nearest / bot->GetSpeed(MOVE_RUN) <= float(quest->GetTimeAllowed());
}

bool NewRpgBaseAction::HasFailedQuest(Player* bot, uint32 questId)
{
    std::lock_guard<std::mutex> guard(failedQuestsLock);
    auto const itr = failedQuests.find(bot->GetGUID().GetCounter());
    return itr != failedQuests.end() && itr->second.count(questId);
}

bool NewRpgBaseAction::AbandonFailedQuest(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;
    {
        std::lock_guard<std::mutex> guard(failedQuestsLock);
        failedQuests[bot->GetGUID().GetCounter()].insert(questId);
    }
    std::string const kind =
        quest->GetTimeAllowed() ? "timed " + std::to_string(quest->GetTimeAllowed()) + " s" : "escort / event";
    LOG_INFO("playerbots", "[rpg] {} L{} ABANDON q{} {} reason: failed ({}), not taken again", bot->GetName(),
             bot->GetLevel(), questId, quest->GetTitle(), kind);
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        if (bot->GetQuestSlotQuestId(slot) != questId)
            continue;
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << slot;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        botAI->RecordQuestDropped(questId, QuestDropReason::Failed);
        return true;
    }
    return false;
}

namespace
{
    // [guild90] one [guild12] quest-object line per bot, object and verdict (the scan runs every tick)
    std::mutex questObjectLogLock;
    std::unordered_map<uint32, std::unordered_map<ObjectGuid::LowType, std::string>> questObjectLogged;

    void LogQuestObject(Player* bot, GameObject* go, std::string const& verdict)
    {
        {
            std::lock_guard<std::mutex> guard(questObjectLogLock);
            auto& logged = questObjectLogged[bot->GetGUID().GetCounter()];
            if (logged.size() > 256)
                logged.clear();
            auto& last = logged[go->GetGUID().GetCounter()];
            if (last == verdict)
                return;
            last = verdict;
        }
        LOG_INFO("playerbots", "[guild12] {} L{} quest object {} ({:.0f} yd) {}", bot->GetName(), bot->GetLevel(),
                 go->GetName(), bot->GetDistance(go), verdict);
    }
}

GameObject* NewRpgBaseAction::FindNeededQuestObject(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;
    QuestSpawnIndex const& index = GetQuestSpawnIndex();

    // the objects that drop an item a quest in the log still needs; none: no grid search at all
    std::vector<uint32> entries;
    for (auto const& [questId, status] : bot->getQuestStatusMap())
    {
        if (status.Status != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const item = quest->RequiredItemId[i];
            if (!item || status.ItemCount[i] >= quest->RequiredItemCount[i])
                continue;
            auto const sources = index.itemObjects.find(item);
            if (sources != index.itemObjects.end())
                entries.insert(entries.end(), sources->second.begin(), sources->second.end());
        }
    }
    if (entries.empty())
        return nullptr;
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

    // [guild90] every object of those entries in range, not FindNearestGameObject's one nearest: the one just looted
    // (despawned, still the nearest) hid all the others (b317 Eraelor stood among Timberling Sprouts, "spot spent")
    std::list<GameObject*> objects;
    bot->GetGameObjectListWithEntryInGrid(objects, entries, QUEST_OBJECT_RANGE);
    LootObjectStack* lootStack = botAI->GetAiObjectContext()->GetValue<LootObjectStack*>("available loot")->Get();
    GameObject* nearest = nullptr;
    float nearestDist = std::numeric_limits<float>::max();
    for (GameObject* go : objects)
    {
        if (!go->isSpawned() || go->GetGoState() != GO_STATE_READY || go->HasGameObjectFlag(GO_FLAG_IN_USE))
            continue;
        float const d = bot->GetDistance(go);
        if (d >= nearestDist)
            continue;
        if (!bot->IsWithinLOSInMap(go))
            continue;  // out of sight: the scan sees it from another step
        if (ObjectOutOfReach(bot, go))  // [objreach67]
            continue;
        LootObject loot(bot, go->GetGUID());
        if (loot.IsEmpty())
        {
            LogQuestObject(bot, go, "skipped: nothing in it the log needs");
            continue;
        }
        // [guild90] the height gap is judged by the walk, not here (IsLootPossible's 3.5 yd height rule is for the
        // object in reach: on Teldrassil's slopes it hid every sprout more than a step up or down)
        if (!loot.IsLootPossible(bot, false))
        {
            LogQuestObject(bot, go, "skipped: not lootable for it (lock, skill, flags)");
            continue;
        }
        if (lootStack && !lootStack->CanAttemptLoot(go->GetGUID()))
        {
            LogQuestObject(bot, go, "skipped: its last loot try failed, retried after the loot retry delay");
            continue;
        }
        if (CreatureTemplate const* danger =
                DangerOnWay(bot, bot->GetPositionX(), bot->GetPositionY(), go->GetPositionX(), go->GetPositionY()))
        {
            LogQuestObject(bot, go, "skipped: " + danger->Name + " on the way");
            continue;
        }
        nearest = go;
        nearestDist = d;
    }
    return nearest;
}

namespace
{
    // The spot of `spots` on the bot's map nearest to it, as a POI; false if none is on this map.
    bool NearestSpotOnMap(Player* bot, std::vector<QuestSpot> const& spots, int32 objectiveIdx,
                          std::vector<POIInfo>& poiInfo)
    {
        QuestSpot const* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();
        for (QuestSpot const& spot : spots)
        {
            if (spot.mapId != bot->GetMapId())
                continue;
            float const d = bot->GetDistance2d(spot.x, spot.y);
            if (d < bestDist)
            {
                bestDist = d;
                best = &spot;
            }
        }
        if (!best)
            return false;
        poiInfo.push_back({{best->x, best->y}, objectiveIdx});
        return true;
    }
}

// [quest13] an explore objective is the core's area trigger: a player's client reports every trigger it enters
// (CMSG_AREATRIGGER) and WorldSession::HandleAreaTriggerOpcode credits the quest (AreaExploredOrEventHappens) when the
// player stands in the trigger's radius / box. A bot has no client: it reports the trigger it stands in, as the client
// does. b353: 8 bots swept q62 The Fargodeep Mine 3-5 yd from its trigger for the whole run (2300 sweep lines, 0
// credit). `walkTo`: the nearest trigger of the quest on this map, its centre (the bot stands outside every one)
bool NewRpgBaseAction::ReportExploreTrigger(uint32 questId, WorldPosition* walkTo)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest || !quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) ||
        bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE || !bot->IsAlive())
        return false;
    auto const statusItr = bot->getQuestStatusMap().find(questId);
    if (statusItr == bot->getQuestStatusMap().end() || statusItr->second.Explored)
        return false;
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    auto const ids = index.triggerIds.find(questId);
    if (ids == index.triggerIds.end())
        return false;
    float best = std::numeric_limits<float>::max();
    for (uint32 const id : ids->second)
    {
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(id);
        if (!at || at->map != bot->GetMapId())
            continue;
        if (bot->IsInAreaTriggerRadius(at))
        {
            LOG_INFO("playerbots", "[quest13] {} L{} q{} explore: reports area trigger {} it stands in ({:.1f} yd "
                     "from its centre)",
                     bot->GetName(), bot->GetLevel(), questId, id, bot->GetExactDist(at->x, at->y, at->z));
            WorldPacket packet(CMSG_AREATRIGGER, 4);
            packet << uint32(id);
            packet.rpos(0);
            bot->GetSession()->HandleAreaTriggerOpcode(packet);
            return true;
        }
        float const d = bot->GetExactDist(at->x, at->y, at->z);
        if (walkTo && d < best)
        {
            best = d;
            *walkTo = WorldPosition(at->map, at->x, at->y, at->z);
        }
    }
    return false;
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    static QuestPOIVector const noPoi;
    QuestPOIVector const* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
        poiVector = &noPoi;

    auto const statusItr = bot->getQuestStatusMap().find(questId);
    if (statusItr == bot->getQuestStatusMap().end())
        return false;
    QuestStatusData const& q_status = statusItr->second;
    QuestSpawnIndex const& spawnIndex = GetQuestSpawnIndex();

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.size() == 0)
                continue;

            float dx = 0, dy = 0;
            std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
            for (size_t i = 0; i < qPoi.points.size(); i++)
            {
                QuestPOIPoint const& point = qPoi.points[i];
                dx += point.x * weights[i];
                dy += point.y * weights[i];
            }

            if (bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;

            if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
        }

        // [guild10] no hand-in POI: straight to the quest ender's spawn (creature / gameobject_questender)
        if (poiInfo.empty())
        {
            auto const enders = spawnIndex.enders.find(questId);
            if (enders != spawnIndex.enders.end())
                NearestSpotOnMap(bot, enders->second, -1, poiInfo);
        }

        return !poiInfo.empty();
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // [guild12] a quest of red or orange con for the bot (the core's level rule) waits until it is closer
    int32 const questLevel = std::max<int32>(1, bot->GetQuestLevel(quest));
    XPColorChar const questColor = Acore::XP::GetColorCode(bot->GetLevel(), uint8(std::min<int32>(questLevel, 255)));
    if (questColor == XP_RED || questColor == XP_ORANGE)
    {
        HoldPostponed(bot, quest,
                      "quest L" + std::to_string(questLevel) + (questColor == XP_RED ? " (red)" : " (orange)"));
        return false;
    }

    // [guild12] item objectives go where their droppers stand (world DB loot tables): the nearest spot whose way
    // is free of orange / red mobs; the POI blend only for an item nobody on this map drops.
    // [guild90] kill / use objectives likewise by the spawns of their creature / object, and every unspent camp
    // (up to MAX_SPOT_CANDIDATES per objective), not the single nearest spawn
    CreatureTemplate const* blocker = nullptr;
    std::set<int32> resolved;
    size_t spentObjectives = 0;
    resolveStats = ResolveStats();
    for (uint32 objective : incompleteObjectiveIdx)
    {
        std::vector<QuestSpot const*> spots;
        ObjectiveSources(quest, objective, spots);
        // [guild90] the objective's area: its spawns inside the quest_poi polygon when the quest has one (spawns of
        // the same mob far off are other quests' camps); none inside: all spawns
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId() || qPoi.ObjectiveIndex != int32(objective) || qPoi.points.size() < 3)
                continue;
            std::vector<QuestSpot const*> inside;
            for (QuestSpot const* spot : spots)
                if (spot->mapId != bot->GetMapId() || InPolygon(qPoi.points, spot->x, spot->y))
                    inside.push_back(spot);
            if (!inside.empty())
                spots.swap(inside);
            break;
        }
        // [drop71] an item objective's sources ranked by what a unit really costs there: b414 Gilduck L14 swept the
        // Murloc Lurkers (creature 732: Murloc Eye at 0.0226 %, ~4400 kills a unit) for q38 - 23 kills, nothing -
        // the nearest dropper, while Murloc Coastrunners drop it at 30 %. Each source's spots take the extra kills a
        // unit costs (100 / chance - 1, at its kill time and the walk speed) on top of their walk
        std::unordered_map<uint32, float> costYd;
        if (objective >= QUEST_OBJECTIVES_COUNT && objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            uint32 const item = quest->RequiredItemId[objective - QUEST_OBJECTIVES_COUNT];
            float const dps = std::max(DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo), 1.0f);
            std::unordered_set<uint32> entries;
            for (QuestSpot const* spot : spots)
                if (!spot->object && spot->entry)
                    entries.insert(spot->entry);
            for (uint32 entry : entries)
                if (float const cost = DropCostYd(bot, entry, item, dps); cost >= 0.0f)
                    costYd[entry] = cost;
        }
        sourceCostYd = costYd.empty() ? nullptr : &costYd;
        int const found = NearestSafeSpot(bot, questId, spots, int32(objective), poiInfo, blocker);
        sourceCostYd = nullptr;
        if (found)
            resolved.insert(int32(objective));
        if (found == 3)
            ++spentObjectives;
    }

    // Get POIs to go
    for (QuestPOI const& qPoi : *poiVector)
    {
        if (qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == static_cast<int32>(objective))
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete || resolved.count(qPoi.ObjectiveIndex))
            continue;
        if (qPoi.points.size() == 0)
            continue;
        // [guild90] no spawn of the objective's sources: the polygon's own points are the spots (swept one by one
        // like camps), no random blend of them
        std::vector<QuestSpot> corners;
        for (QuestPOIPoint const& point : qPoi.points)
            corners.push_back({qPoi.MapId, float(point.x), float(point.y), 0.0f});
        if (NearestSafeSpot(bot, questId, corners, qPoi.ObjectiveIndex, poiInfo, blocker) == 3)
            ++spentObjectives;
    }

    // [guild10] the explore trigger (the spawns of kill / use objectives are resolved above)
    if (poiInfo.empty() && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
    {
        auto const triggers = spawnIndex.triggers.find(questId);
        if (triggers != spawnIndex.triggers.end())
            NearestSafeSpot(bot, questId, triggers->second, QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT,
                            poiInfo, blocker);
    }

    // [guild12] every objective spot among or behind orange / red mobs: the next quest instead
    // [guild90] held until the next level; every camp seen empty is no postpone: the quest comes back with the
    // respawn (the spent marks run out), the planner meanwhile takes another
    if (poiInfo.empty() && blocker)
        HoldPostponed(bot, quest,
                      "objective among " + blocker->Name + " L" + std::to_string(blocker->maxlevel) +
                          [&]()
                          {
                              // [guild15] a mob of lower con blocks only by its remembered death / refused-pull zone
                              XPColorChar const con = Acore::XP::GetColorCode(bot->GetLevel(), blocker->maxlevel);
                              return std::string(con == XP_RED      ? " (red)"
                                                 : con == XP_ORANGE ? " (orange)"
                                                                    : " (refused pull / death zone)");
                          }());
    else if (poiInfo.empty() && spentObjectives)
        NotePostpone(bot, questId, "every camp of its objectives seen empty (back at their respawn)");

    return !poiInfo.empty();
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (WalkGivenUp(bot, loc.GetPositionX(), loc.GetPositionY()))  // [holds9]
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    WorldPosition dest{};
    if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
    {
        uint32 idx = urand(0, hi_prepared_locs.size() - 1);
        dest = hi_prepared_locs[idx];
    }
    else if (!lo_prepared_locs.empty())
    {
        uint32 idx = urand(0, lo_prepared_locs.size() - 1);
        dest = lo_prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({}+{} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size() - hi_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

namespace
{
    // Where every quest in the database can be picked up: the first spawn of each creature or object that
    // starts it. Built once, shared by all bots.
    struct QuestStartPoint
    {
        uint32 questId;
        uint16 mapId;
        float x, y, z;
        uint32 creatureFaction;  // 0 for game objects
        int16 event = 0;         // [guild15] the giver spawn's game event (b321: Day of the Dead givers, event 51 not
                                 // running, drew bots 1-17 km to an empty spot: q13952, q14167-14173)
    };

    std::vector<QuestStartPoint> const& GetQuestStartPoints()
    {
        static std::vector<QuestStartPoint> points;
        static std::once_flag built;
        std::call_once(built, []()
        {
            // [guild15] one spawn per entry, a permanent one before an event's
            std::unordered_map<uint32, std::pair<CreatureData const*, int16>> creatureSpawn;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                int16 const event = SpawnEvent(true, spawnId);
                auto const [it, added] = creatureSpawn.emplace(data.id, std::make_pair(&data, event));
                if (!added && it->second.second && !event)
                    it->second = {&data, event};
            }

            for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
            {
                auto spawn = creatureSpawn.find(entry);
                CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entry);
                if (spawn == creatureSpawn.end() || !creatureTemplate)
                    continue;

                CreatureData const* data = spawn->second.first;
                points.push_back({questId, data->mapid, data->posX, data->posY, data->posZ, creatureTemplate->faction,
                                  spawn->second.second});
            }

            std::unordered_map<uint32, std::pair<GameObjectData const*, int16>> objectSpawn;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
            {
                int16 const event = SpawnEvent(false, spawnId);
                auto const [it, added] = objectSpawn.emplace(data.id, std::make_pair(&data, event));
                if (!added && it->second.second && !event)
                    it->second = {&data, event};
            }

            for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestRelationMap())
            {
                auto spawn = objectSpawn.find(entry);
                if (spawn == objectSpawn.end())
                    continue;

                GameObjectData const* data = spawn->second.first;
                points.push_back({questId, data->mapid, data->posX, data->posY, data->posZ, 0, spawn->second.second});
            }
        });
        return points;
    }

    constexpr float QUEST_TRAVEL_SPEED = 7.0f;           // yards per second on foot
    constexpr float QUEST_OBJECTIVE_TIME = 180.0f;       // seconds to clear one quest objectives
    // [routeplan] the corridor searches one route build may run for legs no bot measured yet (the rest estimated by
    // the measured ones): a cost cap per tick, not a behaviour rule
    constexpr uint32 ROUTE_NEW_LEGS_PER_BUILD = 8;
    constexpr float QUEST_UNKNOWN_DISTANCE = 150.0f;     // assumed giver-to-objective distance without POI data
    constexpr float QUEST_MAX_GIVER_DISTANCE = 3000.0f;  // farther givers are left for a later trip
    constexpr float QUEST_IN_LOG_BONUS = 1.25f;          // accepted quests cost nothing to start
    constexpr float REST_KILL_QUEST_BONUS = 1.15f;       // rested experience doubles kill experience only
    constexpr float DALARAN_QUEST_BONUS = 4.0f;          // opens the city: portals, the crystal, the flight path
    constexpr float ZONE_IN_ORDER_BONUS = 1.3f;
    constexpr float ZONE_TOO_EARLY = 0.6f;
    constexpr float ZONE_OUTLEVELED = 0.8f;
    constexpr uint8 NORTHREND_ENTRY_LEVEL = 68;
    constexpr uint32 NORTHREND_MAP_ID = 571;
    // [guild15] user 2026-11-08: Outland proper (Hellfire Peninsula and on) from 58, as Blizzard's Dark Portal
    // quests open it; the draenei isles and the blood elf lands are map 530 too but belong to Kalimdor / the Eastern
    // Kingdoms (TravelRouteMgr::Landmass)
    constexpr uint8 OUTLAND_ENTRY_LEVEL = 58;
    enum class Continent : uint8
    {
        EasternKingdoms,
        Kalimdor,
        Outland,
        Northrend,
        Other
    };
    Continent ContinentOf(WorldPosition const& pos)
    {
        switch (pos.GetMapId())
        {
            case 0:
                return Continent::EasternKingdoms;
            case 1:
                return Continent::Kalimdor;
            case 530:
            {
                uint32 const landmass = TravelRouteMgr::Landmass(pos);
                if (landmass == 530 * 4 + 1)
                    return Continent::Kalimdor;  // Azuremyst, Bloodmyst, the Exodar
                if (landmass == 530 * 4 + 2)
                    return Continent::EasternKingdoms;  // Eversong, Ghostlands, Silvermoon, Quel'Danas
                return Continent::Outland;
            }
            case NORTHREND_MAP_ID:
                return Continent::Northrend;
            default:
            {
                // [flit] a transit map (the Deeprun Tram) belongs to the continent its exits lead onto (b341: Maronic
                // L1 and five L10-11 bots on map 369 saw every quest as "other continent", grinded or sat all run)
                for (TravelRouteMgr::Portal const& portal : sTravelRouteMgr.GetPortals())
                    if (portal.from.GetMapId() == pos.GetMapId() && portal.to.GetMapId() != pos.GetMapId())
                        switch (portal.to.GetMapId())
                        {
                            case 0:
                            case 1:
                            case 530:
                            case NORTHREND_MAP_ID:
                                return ContinentOf(portal.to);
                            default:
                                break;
                        }
                return Continent::Other;
            }
        }
    }
    // a starter on `giver`'s continent is priced for the bot (user 2026-11-08: no cross-continent zone change at low
    // level; it levels on its own continent - its starting zones and capitals included - until the level that needs
    // another: Outland at 58, Northrend at 68)
    bool ContinentOpen(Continent botContinent, Continent giver, uint8 level)
    {
        if (giver == botContinent)
            return true;
        if (giver == Continent::Northrend)
            return level >= NORTHREND_ENTRY_LEVEL;
        if (giver == Continent::Outland)
            return level >= OUTLAND_ENTRY_LEVEL;
        if (giver == Continent::Other)
            return false;
        return level >= OUTLAND_ENTRY_LEVEL;  // the old world's other continent: once it may leave for Outland
    }

    // Northrend levelling order (both factions, from their own docks): Borean Tundra or Howling Fjord,
    // Dragonblight, Grizzly Hills, Zul'Drak, Sholazar Basin, then the Storm Peaks and Icecrown.
    struct ZoneBand
    {
        uint32 zoneId;
        uint8 low;
        uint8 high;
    };
    constexpr ZoneBand NORTHREND_ZONE_ORDER[] = {
        {3537, 68, 72},  // Borean Tundra
        {495, 68, 72},   // Howling Fjord
        {65, 72, 74},    // Dragonblight
        {394, 74, 76},   // Grizzly Hills
        {66, 76, 77},    // Zul'Drak
        {3711, 77, 78},  // Sholazar Basin
        {67, 79, 80},    // The Storm Peaks
        {210, 79, 80},   // Icecrown
    };

    bool IsKillQuest(Quest const* quest)
    {
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] > 0)
                return true;

        return false;
    }
}

// Soft preferences on top of experience per second: kill quests while rested, the Northrend zone order, the
// Dalaran attunement at 74.
float NewRpgBaseAction::RemainingShare(Player* bot, Quest const* quest)
{
    auto const itr = bot->getQuestStatusMap().find(quest->GetQuestId());
    if (itr == bot->getQuestStatusMap().end())
        return 1.0f;
    uint32 required = 0, left = 0;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i])
        {
            required += quest->RequiredNpcOrGoCount[i];
            left += quest->RequiredNpcOrGoCount[i] - std::min<uint32>(itr->second.CreatureOrGOCount[i],
                                                                      quest->RequiredNpcOrGoCount[i]);
        }
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
        {
            required += quest->RequiredItemCount[i];
            left += quest->RequiredItemCount[i] -
                    std::min<uint32>(itr->second.ItemCount[i], quest->RequiredItemCount[i]);
        }
    return required ? float(left) / float(required) : 1.0f;
}

float NewRpgBaseAction::GetQuestWeight(Quest const* quest, bool rested)
{
    uint8 const level = bot->GetLevel();
    float weight = 1.0f;
    if (rested && IsKillQuest(quest))
        weight *= REST_KILL_QUEST_BONUS;

    if (level >= NORTHREND_ENTRY_LEVEL && quest->GetZoneOrSort() > 0)
    {
        for (ZoneBand const& band : NORTHREND_ZONE_ORDER)
        {
            if (band.zoneId != static_cast<uint32>(quest->GetZoneOrSort()))
                continue;

            if (level < band.low)
                weight *= ZONE_TOO_EARLY;
            else if (level > band.high)
                weight *= ZONE_OUTLEVELED;
            else
                weight *= ZONE_IN_ORDER_BONUS;
            break;
        }
    }

    if (DalaranTravel::IsTeleportQuest(quest->GetQuestId()) ||
        quest->GetQuestId() == DalaranTravel::QUEST_LEAVE_AND_RETURN)
        weight *= DALARAN_QUEST_BONUS;

    return weight;
}

// Kills, object uses and items collected so far towards the quest objectives.
uint32 NewRpgBaseAction::GetQuestProgress(uint32 questId)
{
    auto itr = bot->getQuestStatusMap().find(questId);
    if (itr == bot->getQuestStatusMap().end())
        return 0;

    uint32 progress = 0;
    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        progress += itr->second.CreatureOrGOCount[i];
    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        progress += itr->second.ItemCount[i];

    return progress;
}

void NewRpgBaseAction::NotePostpone(Player* bot, uint32 questId, std::string const& reason)
{
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        std::string& last = postponeReasons[bot->GetGUID().GetCounter()][questId];
        // [guild90] one line per reason (the planner looks at every quest of the log on every pass: b319 Peny q87
        // x340 the same note)
        if (last == reason)
            return;
        last = reason;
    }
    QuestStats::LogEvent(bot, "postpone", questId, "reason=" + reason);
}

void NewRpgBaseAction::ClaimSpot(Player* bot, uint32 questId, WorldPosition const& pos)
{
    std::lock_guard<std::mutex> guard(questNoteLock);
    uint32 const counter = bot->GetGUID().GetCounter();
    if (questId)
        postponeReasons[counter].erase(questId);
    auto const old = claimOf.find(counter);
    if (old != claimOf.end() && old->second != questId)
        claims[old->second].erase(counter);
    if (!questId)
    {
        claimOf.erase(counter);
        return;
    }
    claimOf[counter] = questId;
    claims[questId][counter] = {pos.GetMapId(), pos.GetPositionX(), pos.GetPositionY()};
}

std::string NewRpgBaseAction::LastResolveStats()
{
    return "spawns=" + std::to_string(resolveStats.spawns) + " spent=" + std::to_string(resolveStats.spent) +
           " taken=" + std::to_string(resolveStats.taken) + " offered=" + std::to_string(resolveStats.offered);
}

void NewRpgBaseAction::ForEachSpawnReaching(uint32 mapId, float x, float y, float assist,
                                            std::function<void(CreatureTemplate const*, float, float, float)> const& fn)
{
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    // [camp63] b405 froze 60 s in here: the scan ran over every cell within assist + the map's widest wander distance
    // (one spawn with a huge wander area made it thousands of cells, per spot, per quest, per route build). Now the
    // cells within assist + MAX_AGGRO_RADIUS hold every spawn whose own wander is up to MAX_AGGRO_RADIUS; the few
    // with a wider wander area are kept in one list per map (built once from the read-only index) and looked at alone
    static std::once_flag built;
    static std::unordered_map<uint32, std::vector<MobSpawn const*>> wide;
    std::call_once(built,
                   [&index]
                   {
                       for (auto const& [key, cell] : index.mobCells)
                           for (MobSpawn const& m : cell)
                               if (m.wander > MAX_AGGRO_RADIUS)
                                   wide[uint32(key >> 32)].push_back(&m);
                   });
    float const scanReach = assist + MAX_AGGRO_RADIUS;
    int32 const cells = int32(std::ceil(scanReach / MAX_AGGRO_RADIUS));
    for (int32 dx = -cells; dx <= cells; ++dx)
        for (int32 dy = -cells; dy <= cells; ++dy)
        {
            auto const cell = index.mobCells.find(
                MobCellKey(mapId, x + float(dx) * MAX_AGGRO_RADIUS, y + float(dy) * MAX_AGGRO_RADIUS));
            if (cell == index.mobCells.end())
                continue;
            for (MobSpawn const& m : cell->second)
                if (!m.beat && m.wander <= MAX_AGGRO_RADIUS && SpawnPresent(m) &&
                    std::hypot(m.x - x, m.y - y) <= assist + m.wander)
                    fn(m.tmpl, m.x, m.y, m.wander);
        }
    auto const wideHere = wide.find(mapId);
    if (wideHere != wide.end())
        for (MobSpawn const* m : wideHere->second)
            if (SpawnPresent(*m) && std::hypot(m->x - x, m->y - y) <= assist + m->wander)
                fn(m->tmpl, m->x, m->y, m->wander);
}

bool NewRpgBaseAction::WorthItemSource(Player* bot, uint32 entry)
{
    // one answer per bot, entry, level and 200 yd cell it stands in (its sources' walks barely change within one)
    uint64 const cell = (uint64(uint32(int32(bot->GetPositionX() / 200.0f) + 0x8000) & 0xFFFF) << 16) |
                        (uint32(int32(bot->GetPositionY() / 200.0f) + 0x8000) & 0xFFFF);
    uint64 const key = (uint64(entry) << 40) ^ (uint64(bot->GetLevel()) << 32) ^ cell ^
                       (uint64(bot->GetMapId()) << 56);
    static std::mutex worthLock;
    static std::unordered_map<uint32, std::unordered_map<uint64, bool>> worth;
    {
        std::lock_guard<std::mutex> guard(worthLock);
        auto const mine = worth.find(bot->GetGUID().GetCounter());
        if (mine != worth.end())
            if (auto const found = mine->second.find(key); found != mine->second.end())
                return found->second;
    }
    QuestSpawnIndex const& index = GetQuestSpawnIndex();
    float const dps = DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo);
    bool known = false, worthIt = false;
    for (auto const& [questId, status] : bot->getQuestStatusMap())
    {
        if (status.Status != QUEST_STATUS_INCOMPLETE || worthIt)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        for (uint8 i = 0; quest && i < QUEST_ITEM_OBJECTIVES_COUNT && !worthIt; ++i)
        {
            uint32 const item = quest->RequiredItemId[i];
            if (!item || status.ItemCount[i] >= quest->RequiredItemCount[i])
                continue;
            float const cost = DropCostYd(bot, entry, item, dps);
            if (cost < 0.0f)
                continue;
            known = true;
            // the cheapest dropper on its map: its cost plus the walk to its nearest spawn
            float best = std::numeric_limits<float>::max();
            if (auto const droppers = index.itemCreatures.find(item); droppers != index.itemCreatures.end())
                for (uint32 other : droppers->second)
                {
                    float const otherCost = DropCostYd(bot, other, item, dps);
                    auto const spawns = index.creatures.find(other);
                    if (otherCost < 0.0f || spawns == index.creatures.end())
                        continue;
                    for (QuestSpot const& spot : spawns->second)
                        if (spot.mapId == bot->GetMapId())
                            best = std::min(best, otherCost + bot->GetDistance2d(spot.x, spot.y));
                }
            worthIt = cost <= best;
        }
    }
    bool const answer = !known || worthIt;  // no known chance (a reference table): it stays a quest mob
    std::lock_guard<std::mutex> guard(worthLock);
    worth[bot->GetGUID().GetCounter()][key] = answer;
    return answer;
}

bool NewRpgBaseAction::ObjectiveTargetHere(Player* bot, uint32 questId, Unit* grindTarget)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return grindTarget != nullptr;
    std::vector<uint32> const entries = OpenObjectiveCreatures(bot, quest);
    auto const serves = [&entries](Creature const* c)
    {
        CreatureTemplate const* t = c->GetCreatureTemplate();
        for (uint32 entry : entries)
            if (c->GetEntry() == entry || (t && (t->KillCredit[0] == entry || t->KillCredit[1] == entry)))
                return true;
        return false;
    };
    if (grindTarget && grindTarget->ToCreature() && serves(grindTarget->ToCreature()))
        return true;
    // a living source in the spot's circle no other player has tapped: the grind takes it next
    for (uint32 entry : entries)
    {
        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, entry, SPENT_RADIUS);
        for (Creature* c : found)
            if (c->IsAlive() && (!c->hasLootRecipient() || c->isTappedBy(bot)))
                return true;
    }
    return false;
}

uint32 NewRpgBaseAction::EarliestRespawn(Player* bot, uint32 questId)
{
    std::lock_guard<std::mutex> guard(spentLock);
    auto const quests = spentSpots.find(bot->GetGUID().GetCounter());
    if (quests == spentSpots.end())
        return 0;
    auto const marks = quests->second.find(questId);
    if (marks == quests->second.end())
        return 0;
    time_t const now = time(nullptr);
    time_t best = 0;
    for (SpentMark const& mark : marks->second)
        if (mark.until > now && (!best || mark.until < best))
            best = mark.until;
    return best ? uint32(best - now) : 0;
}

float NewRpgBaseAction::OtherQuestSeconds(uint32 exceptQuest, uint32& otherQuest, float* walkSeconds)
{
    otherQuest = 0;
    if (walkSeconds)
        *walkSeconds = 0.0f;
    float best = -1.0f;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || questId == exceptQuest || botAI->lowPriorityQuest.count(questId))
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        std::vector<POIInfo> poiInfo;
        if (!quest ||
            (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE &&
             (NeedsItemUseCredit(quest) || NeedsSpellCredit(bot, quest, true))) ||  // [flit] no candidate either
            !GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
            continue;
        float distance = std::numeric_limits<float>::max();
        for (POIInfo const& poi : poiInfo)
            distance = std::min(distance, bot->GetDistance2d(poi.pos.x, poi.pos.y));
        float seconds = distance / QUEST_TRAVEL_SPEED;
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            seconds += QUEST_OBJECTIVE_TIME * RemainingShare(bot, quest);
        if (best < 0.0f || seconds < best)
        {
            best = seconds;
            otherQuest = questId;
            if (walkSeconds)
                *walkSeconds = distance / QUEST_TRAVEL_SPEED;
        }
    }
    return best;
}

void NewRpgBaseAction::NoteResume(Player* bot, uint32 questId)
{
    std::lock_guard<std::mutex> guard(questNoteLock);
    resumeQuest[bot->GetGUID().GetCounter()] = questId;
}

std::string NewRpgBaseAction::QuestChoiceNote(Player* bot)
{
    std::lock_guard<std::mutex> guard(questNoteLock);
    auto const found = choiceNotes.find(bot->GetGUID().GetCounter());
    return found == choiceNotes.end() ? std::string() : found->second;
}

void NewRpgBaseAction::NoteRouteChoice(Player* bot, uint32 chosen, std::string const& why)
{
    uint32 const counter = bot->GetGUID().GetCounter();
    uint32 committed = 0;
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        choiceNotes[counter] = why;
        auto const found = committedQuest.find(counter);
        if (found != committedQuest.end())
            committed = found->second;
        if (bot->GetQuestStatus(chosen) == QUEST_STATUS_INCOMPLETE)
            committedQuest[counter] = chosen;
        else
            committedQuest.erase(counter);
        if (committed && committed != chosen)
        {
            auto const old = claimOf.find(counter);
            if (old != claimOf.end() && old->second == committed)
            {
                claims[committed].erase(counter);
                claimOf.erase(old);
            }
        }
    }
    if (committed && committed != chosen && bot->GetQuestStatus(committed) == QUEST_STATUS_INCOMPLETE)
        QuestStats::LogEvent(bot, "target-switch", chosen, "from=q" + std::to_string(committed) + " reason=" + why);
}

// [questroute] the route's stops. The log: each open quest's objective area (the middle of its unspent camps, the
// work by the formula), each done or open quest's ender merged into hubs (enders within GROUP_ENDER_RADIUS are one
// visit); the givers of its landmass with quests it may take (each take's area and ender from the quest table); the
// town tasks waiting for their town. The same filters and log clean-ups as the per-quest choice.
void NewRpgBaseAction::CollectRouteStops(QuestRoute::Input& in,
                                         std::vector<std::pair<WorldPosition, std::string>> const& towns,
                                         std::set<uint64> const& visitedTown)
{
    using QuestRoute::Stop;
    using QuestRoute::StopKind;
    uint8 const level = bot->GetLevel();
    bool const rested = bot->GetRestBonus() > 0.0f;
    uint32 const counter = bot->GetGUID().GetCounter();
    in.x = bot->GetPositionX();
    in.y = bot->GetPositionY();
    in.z = bot->GetPositionZ();
    in.speed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    // [route23] coordinator 2026-11-09: the walk's pace is the one its legs showed, not the run speed. b372: the done
    // hand-in stops took 1.64x their plan (9583 s for 5845 s), the pickups 1.78x, the objectives 1.52x - the bots
    // covered 4.3-5.3 yd a second of their walking states against the 7 the plan assumed (the stops between the
    // sight-long legs, the waits for a walk's end, the walks round the walls). The run speed scaled by the planned
    // over the actual seconds of the hand-in and pickup stops it did (their work is a click)
    {
        QuestRoute::BotRoute const& done = QuestRoute::Of(counter);
        if (done.legStops && done.legActualSec > 0.0f && done.legPlannedSec > 0.0f)
        {
            float const pace = done.legPlannedSec / done.legActualSec;
            in.speed = std::max(in.speed * pace, 1.0f);
            LOG_INFO("playerbots", "[route23] {} L{} leg pace {:.2f}: planned {:.0f} s, took {:.0f} s over {} stops",
                      bot->GetName(), level, pace, done.legPlannedSec, done.legActualSec, done.legStops);
        }
    }
    in.hubRadius = GROUP_ENDER_RADIUS;
    float const dps = DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo);
    uint32 const landmass = TravelRouteMgr::Landmass(WorldPosition(bot));
    // [routeplan] the legs: the real walk and the fights on it, priced at this dps
    Player* const walker = bot;
    auto budget = std::make_shared<uint32>(ROUTE_NEW_LEGS_PER_BUILD);
    in.measure = [walker, dps, budget](QuestRoute::Point const& from, QuestRoute::Point const& to, float& walkYd,
                                       float& fightSec)
    { return MeasureLeg(walker, from, to, dps, walkYd, fightSec, budget.get()); };
    auto const onLandmass = [&](float x, float y, float z)
    {
        return TravelRouteMgr::Landmass(
                   WorldPosition(bot->GetMapId(), x, y, z == INVALID_HEIGHT ? bot->GetPositionZ() : z)) == landmass;
    };
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        auto const found = committedQuest.find(counter);
        if (found != committedQuest.end())
            in.pinnedQuest = found->second;
    }
    if (in.pinnedQuest && (bot->GetQuestStatus(in.pinnedQuest) != QUEST_STATUS_INCOMPLETE ||
                           botAI->lowPriorityQuest.count(in.pinnedQuest)))
        in.pinnedQuest = 0;
    // [quest15] the quests complete already hand in at a hub of their own (ready): one hub with an open quest waits
    // for that quest's objective (Feasible), which held a done quest's hand-in to the route's end (b355: 10 of the 50
    // bots without a turn-in had a complete quest - Manue q224 behind O267's hub "E224+267", Kylantino "E313+317").
    // Same spot, so the solver still visits them together where that is the cheaper order; HandInsEarlier weighs it
    auto const addToHub = [&](float x, float y, float z, uint32 questId, float questXp, bool complete)
    {
        for (Stop& s : in.stops)
            if (s.kind == StopKind::Ender && s.ready == complete && std::hypot(s.x - x, s.y - y) <= in.hubRadius)
            {
                s.quests.push_back(questId);
                s.xp += questXp;
                return;
            }
        Stop hub;
        hub.kind = StopKind::Ender;
        hub.x = x;
        hub.y = y;
        hub.z = z;
        hub.quests.push_back(questId);
        hub.xp = questXp;
        hub.ready = complete;
        in.stops.push_back(hub);
    };
    // the formula's work at an area: the kills left and the walk between its spawns; unpriced: the planner's old
    // objective time for the share left
    auto const workOf = [&](Quest const* quest, GuildCoordinator::QuestWork* out) -> float
    {
        GuildCoordinator::QuestWork work;
        if (sGuildCoordinator.QuestWorkOf(bot, quest->GetQuestId(), dps, work) && work.map == bot->GetMapId())
        {
            if (out)
                *out = work;
            return work.killSeconds + work.tourYd / in.speed;
        }
        if (out)
            *out = GuildCoordinator::QuestWork();
        return QUEST_OBJECTIVE_TIME * RemainingShare(bot, quest);
    };
    uint32 const now = uint32(time(nullptr));
    // [routeplan] a log quest the route does not hold, with its status: the route is built again only when it moves
    auto const leaveOut = [&](uint32 questId, bool count)
    {
        in.leftOutQuests[questId] = uint8(bot->GetQuestStatus(questId));
        if (count)
            ++in.leftOut;
    };

    uint32 inLog = 0;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        ++inLog;
        if (bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED)
        {
            AbandonFailedQuest(questId);
            --inLog;
            continue;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || botAI->lowPriorityQuest.count(questId))
        {
            if (!quest)
                leaveOut(questId, false);
            continue;
        }
        if (IsSpellUnlockClassQuest(quest))
        {
            LOG_INFO("playerbots", "[rpg] {} L{} ABANDON q{} {} reason: class quest for a spell (granted)",
                     bot->GetName(), bot->GetLevel(), questId, quest->GetTitle());
            bot->RemoveActiveQuest(questId, false);
            --inLog;
            continue;
        }
        QuestStatus const status = bot->GetQuestStatus(questId);
        if (std::string why; status == QUEST_STATUS_INCOMPLETE &&
                             (NeedsItemUseCredit(quest, &why) || NeedsSpellCredit(bot, quest, true, &why)))
        {
            NotePostpone(bot, questId, "unsupported: " + why);
            leaveOut(questId, false);
            continue;
        }
        if (EnderNeedsScript(questId))
        {
            LOG_INFO("playerbots", "[rpg] {} L{} ABANDON q{} {} reason: its ender is an object a script spawns",
                     bot->GetName(), bot->GetLevel(), questId, quest->GetTitle());
            bot->RemoveActiveQuest(questId, false);
            --inLog;
            continue;
        }
        if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
            continue;
        // [quest14] user rule 2026-11-08: a grey quest stays only when it is needed (a chain step, an upgrade, a
        // recipe / skill / attunement / key / wanted item - the global leader's call); one nobody needs leaves the
        // log, a complete one may still be handed in where the route passes its ender (PlaceGreyHandIns). A grey
        // quest's XP counts toward no rate. A human's character, or a bot a human leads (his shared quests), keeps
        // its log
        bool const grey = GuildCoordinator::IsGreyQuest(bot, quest);
        bool greyUnneeded = false;
        if (grey && !GuildCoordinator::HumanCharacter(bot) &&
            !(botAI->GetMaster() && GuildCoordinator::HumanCharacter(botAI->GetMaster())))
        {
            std::string const need = sGuildCoordinator.GreyQuestNeed(bot, quest);
            if (!need.empty())
                GuildCoordinator::LogGrey(bot, "keeps", quest, need);
            else if (status == QUEST_STATUS_COMPLETE)
                greyUnneeded = true;
            else
            {
                AbandonGreyQuest(slot, quest, "not needed (no chain step, upgrade, recipe, skill, attunement, key "
                                              "or wanted item)");
                --inLog;
                continue;
            }
        }
        float xp = grey ? 0.0f : GetQuestWeight(quest, rested) * quest->XPValue(level);
        // [group23] a group leader's route weighs the group's experience: each member in its sight that holds the
        // quest too hands it in with it (the leader's share, the members' own take at the giver)
        if (Group* group = bot->GetGroup(); !grey && group && group->GetLeaderGUID() == bot->GetGUID())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource(); m && m != bot && m->GetMapId() == bot->GetMapId() &&
                                                  m->GetExactDist2d(bot) <= sPlayerbotAIConfig.SightDistance &&
                                                  (m->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE ||
                                                   m->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE))
                    xp += float(quest->XPValue(m->GetLevel()));

        std::vector<POIInfo> poiInfo;
        bool const spots = GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true);
        WorldPosition ender;
        bool const hasEnder = NearestEnderSpot(bot, questId, ender);
        if (status == QUEST_STATUS_COMPLETE)
        {
            if (!spots)  // DoCompletedQuest needs the same spot
            {
                if (greyUnneeded)
                {
                    AbandonGreyQuest(slot, quest, "complete, not needed, its ender has no spot");
                    --inLog;
                    continue;
                }
                leaveOut(questId, true);
                continue;
            }
            float const ex = hasEnder ? ender.GetPositionX() : poiInfo[0].pos.x;
            float const ey = hasEnder ? ender.GetPositionY() : poiInfo[0].pos.y;
            float const ez = hasEnder ? ender.GetPositionZ() : poiInfo[0].z;
            if (!onLandmass(ex, ey, ez))
            {
                if (greyUnneeded)
                {
                    AbandonGreyQuest(slot, quest, "complete, not needed, its ender is off its landmass");
                    --inLog;
                    continue;
                }
                leaveOut(questId, true);
                continue;
            }
            if (greyUnneeded)
            {
                Stop hand;
                hand.kind = StopKind::Ender;
                hand.x = ex;
                hand.y = ey;
                hand.z = ez;
                hand.quests.push_back(questId);
                in.greyHandIns.push_back(hand);
                continue;
            }
            addToHub(ex, ey, ez, questId, xp, true);
            in.logXp += xp;
            continue;
        }
        WorldPosition dalaranTarget;
        if (!spots && GetDalaranQuestTarget(questId, dalaranTarget) && dalaranTarget.GetMapId() == bot->GetMapId())
        {
            poiInfo.push_back({{dalaranTarget.GetPositionX(), dalaranTarget.GetPositionY()}, 0,
                               dalaranTarget.GetPositionZ()});
        }
        else if (!spots)
        {
            // every camp spent (back at their respawn: the route is built again then), taken or behind a danger
            if (uint32 const wait = EarliestRespawn(bot, questId))
                in.respawnDue = in.respawnDue ? std::min(in.respawnDue, now + wait) : now + wait;
            leaveOut(questId, true);
            continue;
        }
        // [camp63] each objective camp priced now, from the static spawns, with the solo judge's formula against the
        // bot at full: a camp it loses even at full is no stop - its objective goes to the co-op seek / camp-help
        // request at once instead of a walk to it (b400: 125 objective stops left postponed, 20106 s walked). Priced
        // at every build: a level, its gear or a party that changes the price brings the quest back into the route
        if (SoloSafety::Applies(bot) && !NewRpgCoop::InParty(bot->GetGUID().GetCounter()))
        {
            std::string lostWhy;
            POIInfo lostAt = poiInfo[0];
            poiInfo.erase(std::remove_if(poiInfo.begin(), poiInfo.end(),
                                         [&](POIInfo const& poi)
                                         {
                                             int32 const i = poi.objectiveIdx;
                                             bool const priced =
                                                 (i >= 0 && i < QUEST_OBJECTIVES_COUNT &&
                                                  quest->RequiredNpcOrGo[i] > 0) ||
                                                 (i >= QUEST_OBJECTIVES_COUNT &&
                                                  i < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT);
                                             std::string w;
                                             if (!priced || !NewRpgCoop::StaticCampLost(
                                                                bot, questId, i,
                                                                WorldPosition(bot->GetMapId(), poi.pos.x, poi.pos.y,
                                                                              poi.z),
                                                                w))
                                                 return false;
                                             if (lostWhy.empty())
                                             {
                                                 lostWhy = w;
                                                 lostAt = poi;
                                             }
                                             return true;
                                         }),
                          poiInfo.end());
            if (poiInfo.empty())
            {
                static std::mutex campLogLock;  // one line per bot, quest and level: a log cadence
                static std::set<std::tuple<uint32, uint32, uint8>> campLogged;
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> guard(campLogLock);
                    fresh = campLogged.insert({bot->GetGUID().GetCounter(), questId, level}).second;
                }
                if (fresh)
                    LOG_INFO("playerbots", "[camp63] {} L{} q{} left out of its route: every objective camp is lost "
                             "even at full - {}; to the co-op seek / camp-help request",
                             bot->GetName(), level, questId, lostWhy);
                NewRpgCoop::OnStaticCampLost(
                    bot, questId, lostAt.objectiveIdx,
                    WorldPosition(bot->GetMapId(), lostAt.pos.x, lostAt.pos.y, lostAt.z), lostWhy);
                leaveOut(questId, true);
                continue;
            }
        }
        // the area's middle: the unspent camp nearest all the others
        POIInfo const* middle = &poiInfo[0];
        float bestSum = std::numeric_limits<float>::max();
        for (POIInfo const& a : poiInfo)
        {
            float sum = 0.0f;
            for (POIInfo const& b : poiInfo)
                sum += std::hypot(a.pos.x - b.pos.x, a.pos.y - b.pos.y);
            if (sum < bestSum)
            {
                bestSum = sum;
                middle = &a;
            }
        }
        if (!onLandmass(middle->pos.x, middle->pos.y, middle->z))
        {
            leaveOut(questId, true);
            continue;
        }
        Stop objective;
        objective.kind = StopKind::Objective;
        objective.x = middle->pos.x;
        objective.y = middle->pos.y;
        objective.z = middle->z;
        objective.quests.push_back(questId);
        objective.work = workOf(quest, nullptr);
        in.stops.push_back(objective);
        if (hasEnder && onLandmass(ender.GetPositionX(), ender.GetPositionY(), ender.GetPositionZ()))
            addToHub(ender.GetPositionX(), ender.GetPositionY(), ender.GetPositionZ(), questId, xp, false);

        in.logXp += xp;
    }
    in.freeSlots = inLog < MAX_QUEST_LOG_SIZE ? MAX_QUEST_LOG_SIZE - inLog : 0;

    for (auto const& [pos, what] : towns)
    {
        in.townsSeen.insert(QuestRoute::SpotKey(pos.GetPositionX(), pos.GetPositionY()));
        if (visitedTown.count(QuestRoute::SpotKey(pos.GetPositionX(), pos.GetPositionY())) ||
            !onLandmass(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()))
            continue;
        Stop town;
        town.kind = StopKind::Town;
        town.x = pos.GetPositionX();
        town.y = pos.GetPositionY();
        town.z = pos.GetPositionZ();
        town.what = what;
        town.xp = QuestRoute::MemberHandInXp(QuestRoute::SpotKey(town.x, town.y));  // [quest16]
        // [lag337] ... and its own town tasks' worth in its quest experience (their game prices): the stop moves
        // forward as a hand-in does (HandInsEarlier) when what it brings sooner beats the detour
        float const tasksXp = sGuildCoordinator.TownStopXp(bot, pos);
        town.xp += tasksXp;
        if (tasksXp > 0.0f)
        {
            thread_local std::unordered_map<uint64, uint32> townLogged;  // a log cadence, no decision reads it
            uint32& last = townLogged[bot->GetGUID().GetRawValue() ^ QuestRoute::SpotKey(town.x, town.y)];
            if (last != uint32(tasksXp))
            {
                last = uint32(tasksXp);
                LOG_INFO("playerbots", "[lag337] {} L{} town stop {} weighs {:.0f} quest xp (its tasks' game prices)",
                         bot->GetName(), bot->GetLevel(), what, tasksXp);
            }
        }
        in.stops.push_back(town);

    }

    if (!in.freeSlots || bot->GetMap()->Instanceable())
        return;
    // the givers of its landmass: the quests it may take now (the filters of the per-quest choice), one offer per
    // giver spawn
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    std::map<uint64, size_t> offerAt;
    std::set<uint32> offered;
    for (QuestStartPoint const& start : GetQuestStartPoints())
    {
        if (start.mapId != bot->GetMapId() || !EventSpawnPresent(start.event) || offered.count(start.questId))
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(start.questId);
        if (!quest || quest->GetMinLevel() > level)
            continue;
        int32 const questLevel = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : level;
        if (questLevel > level + 2 || questLevel <= Acore::XP::GetGrayLevel(level))
            continue;
        if (bot->GetQuestStatus(start.questId) != QUEST_STATUS_NONE || botAI->lowPriorityQuest.count(start.questId))
            continue;
        if (start.creatureFaction && botFaction)
        {
            FactionTemplateEntry const* giverFaction = sFactionTemplateStore.LookupEntry(start.creatureFaction);
            if (giverFaction && giverFaction->IsHostileTo(*botFaction))
                continue;
        }
        if (!onLandmass(start.x, start.y, start.z))
            continue;
        if (!IsQuestWorthDoing(quest) || !IsQuestCapableDoing(quest) || !bot->CanTakeQuest(quest, false))
            continue;
        // [giver15] a giver whose script gives the quest-giver flag: planned when the bot can trigger it, the
        // trigger's seconds in the quest's work (the cast and its cooldown, the kill or fight, the script's delay)
        if (!NewRpgGiverTrigger::Plannable(bot, start.questId))
            continue;
        float const triggerSec = NewRpgGiverTrigger::Seconds(bot, start.questId, dps);
        offered.insert(start.questId);

        QuestRoute::Offer::Take take;
        take.questId = start.questId;
        take.xp = GetQuestWeight(quest, rested) * quest->XPValue(level);
        GuildCoordinator::QuestWork work;
        take.objective.kind = StopKind::Objective;
        take.objective.quests.push_back(start.questId);
        take.objective.work = workOf(quest, &work) + triggerSec;
        take.objective.x = start.x;
        take.objective.y = start.y;
        take.objective.z = INVALID_HEIGHT;
        if (work.map == bot->GetMapId() && (work.areaX || work.areaY))
        {
            take.objective.x = work.areaX;
            take.objective.y = work.areaY;
        }
        if (work.map == bot->GetMapId() && work.ender)
        {
            take.hasEnder = true;
            take.ender.kind = StopKind::Ender;
            take.ender.x = work.enderX;
            take.ender.y = work.enderY;
            take.ender.z = INVALID_HEIGHT;
            take.ender.quests.push_back(start.questId);
        }
        uint64 const key = QuestRoute::SpotKey(start.x, start.y);
        auto const at = offerAt.find(key);
        if (at == offerAt.end())
        {
            QuestRoute::Offer offer;
            offer.giver.kind = StopKind::Giver;
            offer.giver.x = start.x;
            offer.giver.y = start.y;
            offer.giver.z = start.z;
            offer.takes.push_back(take);
            offerAt.emplace(key, in.offers.size());
            in.offers.push_back(std::move(offer));
        }
        else
            in.offers[at->second].takes.push_back(take);
    }
}

// [routeplan] the route's legs (QuestRoute::LegTable): the corridor of the long route search between two spots and
// the attackable spawns within the core's largest aggro reach of it, kept per spot pair for every bot (the corridor
// and the spawns do not depend on the bot; their price does)
namespace
{
    struct LegNear
    {
        MobSpawn const* spawn;
        float gap;  // its 3D distance from the corridor
    };
    struct LegGeometry
    {
        bool corridor{false};  // the navmesh gave one (else the straight line)
        float walkYd{0.0f};
        std::vector<LegNear> nearMobs;
    };
    constexpr float LEG_CACHE_GRAIN = 5.0f;    // spots this close share a corridor (the key's grid, yards)
    constexpr size_t LEG_CACHE_MAX = 200000;   // memory bound of the cache (entries), cleared past it
    std::mutex legCacheLock;
    std::unordered_map<uint64, std::shared_ptr<LegGeometry const>> legCache;

    uint64 LegKey(uint32 mapId, QuestRoute::Point const& a, QuestRoute::Point const& b)
    {
        auto const cell = [](float v) { return uint64(uint32(int32(std::floor(v / LEG_CACHE_GRAIN)))); };
        uint64 h = mapId;
        for (uint64 const v : {cell(a.x), cell(a.y), cell(b.x), cell(b.y)})
            h = h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        return h;
    }

    float SegmentGap(G3D::Vector3 const& a, G3D::Vector3 const& b, G3D::Vector3 const& p)
    {
        G3D::Vector3 const ab = b - a;
        float const lengthSq = ab.squaredLength();
        float const t = lengthSq > 0.0f ? std::clamp((p - a).dot(ab) / lengthSq, 0.0f, 1.0f) : 0.0f;
        return (a + ab * t - p).length();
    }

    std::shared_ptr<LegGeometry const> BuildLeg(Player* bot, QuestRoute::Point const& from,
                                                QuestRoute::Point const& to)
    {
        PMON_SCOPE(legScope, PERF_MON_VALUE, "quest route leg");
        auto const ground = [bot](QuestRoute::Point const& p)
        {
            float z = p.z;
            if (z <= INVALID_HEIGHT + 1.0f)
                z = bot->GetMap()->GetHeight(p.x, p.y, MAX_HEIGHT);
            if (z <= INVALID_HEIGHT + 1.0f)
                z = bot->GetPositionZ();
            return G3D::Vector3(p.x, p.y, z);
        };
        G3D::Vector3 const start = ground(from);
        G3D::Vector3 const dest = ground(to);
        auto leg = std::make_shared<LegGeometry>();
        Movement::PointsArray path;
        bool complete = false;
        if (Leader::Travel::LongRoute(bot, start, dest, false, path, complete) && !path.empty())
        {
            leg->corridor = true;
            if ((path.front() - start).length() > 1.0f)
                path.insert(path.begin(), start);
            if (!complete)  // the corridor's end, then the straight rest
                path.push_back(dest);
        }
        else
            path = {start, dest};
        for (size_t i = 1; i < path.size(); ++i)
            leg->walkYd += (path[i] - path[i - 1]).length();

        QuestSpawnIndex const& index = GetQuestSpawnIndex();
        uint32 const mapId = bot->GetMapId();
        std::unordered_map<MobSpawn const*, float> gaps;
        for (size_t i = 1; i < path.size(); ++i)
        {
            G3D::Vector3 const& a = path[i - 1];
            G3D::Vector3 const& b = path[i];
            float const length = (b - a).length();
            uint32 const steps = uint32(std::ceil(length / MAX_AGGRO_RADIUS));
            std::unordered_set<uint64> cells;
            for (uint32 step = 0; step <= steps; ++step)
            {
                float const t = steps ? float(step) / steps : 0.0f;
                for (int32 ox = -1; ox <= 1; ++ox)
                    for (int32 oy = -1; oy <= 1; ++oy)
                        cells.insert(MobCellKey(mapId, a.x + (b.x - a.x) * t + ox * MAX_AGGRO_RADIUS,
                                                a.y + (b.y - a.y) * t + oy * MAX_AGGRO_RADIUS));
            }
            for (uint64 const cell : cells)
            {
                auto const spawns = index.mobCells.find(cell);
                if (spawns == index.mobCells.end())
                    continue;
                for (MobSpawn const& spawn : spawns->second)
                {
                    float const gap = SegmentGap(a, b, G3D::Vector3(spawn.x, spawn.y, spawn.z));
                    if (gap > MAX_AGGRO_RADIUS)
                        continue;
                    auto const [at, fresh] = gaps.emplace(&spawn, gap);
                    if (!fresh)
                        at->second = std::min(at->second, gap);
                }
            }
        }
        leg->nearMobs.reserve(gaps.size());
        for (auto const& [spawn, gap] : gaps)
            leg->nearMobs.push_back({spawn, gap});
        return leg;
    }
}

bool NewRpgBaseAction::MeasureLeg(Player* bot, QuestRoute::Point const& from, QuestRoute::Point const& to,
                                  float dps, float& walkYd, float& fightSec, uint32* newLegs)
{
    walkYd = 0.0f;
    fightSec = 0.0f;
    if (!bot || !bot->IsInWorld())
        return false;
    uint64 const key = LegKey(bot->GetMapId(), from, to);
    std::shared_ptr<LegGeometry const> leg;
    {
        std::lock_guard<std::mutex> guard(legCacheLock);
        auto const found = legCache.find(key);
        if (found != legCache.end())
            leg = found->second;
    }
    if (!leg)
    {
        if (newLegs && !*newLegs)
            return false;  // the build's budget of new corridor searches is spent: the leg is estimated
        if (newLegs)
            --*newLegs;
        leg = BuildLeg(bot, from, to);  // outside the lock: the corridor search runs on the bot's map thread
        std::lock_guard<std::mutex> guard(legCacheLock);
        if (legCache.size() >= LEG_CACHE_MAX)
            legCache.clear();
        legCache.emplace(key, leg);
    }
    walkYd = leg->walkYd;

    // the fights on it: each hostile spawn whose aggro reach (Creature::GetAggroRange) the corridor crosses costs the
    // cheaper of its fight (its health at the bot's dps) and the walk round its reach (the arc over the chord the
    // corridor cuts) - the choice the solo route search makes with its penalty (SoloSafety::RouteCosts); one it never
    // fights (elite, orange / red, a remembered danger zone) costs the walk round
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!botFaction)
        return leg->corridor;
    uint8 const level = bot->GetLevel();
    float const speed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    for (LegNear const& close : leg->nearMobs)
    {
        CreatureTemplate const* tmpl = close.spawn->tmpl;
        if (!SpawnPresent(*close.spawn) || (tmpl->flags_extra & CREATURE_FLAG_EXTRA_CIVILIAN))
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(tmpl->faction);
        if (!faction || !faction->IsHostileTo(*botFaction))
            continue;
        uint8 const mobLevel = uint8((uint32(tmpl->minlevel) + uint32(tmpl->maxlevel)) / 2);
        float const aggro = std::clamp(tmpl->detection_range + float(int32(mobLevel) - int32(level)), 5.0f,
                                       MAX_AGGRO_RADIUS);
        if (close.gap >= aggro)
            continue;
        float const half = std::sqrt(aggro * aggro - close.gap * close.gap);
        float const roundSec = (aggro * 2.0f * std::asin(std::min(1.0f, half / aggro)) - 2.0f * half) / speed;
        bool const never = tmpl->rank != CREATURE_ELITE_NORMAL ||
                           Acore::XP::GetColorCode(level, mobLevel) <= XP_ORANGE ||
                           InDangerZone(bot, close.spawn->x, close.spawn->y);
        float seconds = roundSec;
        if (!never && dps > 0.0f)
            if (CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(mobLevel, tmpl->unit_class))
                seconds = std::min(roundSec, float(stats->GenerateHealth(tmpl)) / dps);
        fightSec += seconds;
    }
    return leg->corridor;
}

// The bot knows every quest in the world and picks the one that gives the most experience per second of
// travel and work: quests already in the log and quests it could still pick up, near or far.
// giverPos is set when the winner still has to be picked up.
// [questroute] the fallback of the route (nothing to route on its landmass): a zone change, a far log quest
namespace
{
    // [zone23] the world's attackable creature spawns by map (read once): where a grind finds its mobs
    struct GrindSpawn
    {
        float x, y;
        uint32 entry;
        uint8 minLevel, maxLevel;
        uint32 respawnSec;
    };

    // [pace25] by map and 100-yd cell (the look reads the cells round its spot, not the whole map: b373 AI max
    // 162 -> 268 ms, TimeRate walked every spawn of the map for each helper of each request)
    constexpr float GRIND_CELL = 100.0f;
    uint64 GrindCellKey(uint32 mapId, float x, float y)
    {
        int64 const cx = int64(std::floor(x / GRIND_CELL)) + (1 << 19);
        int64 const cy = int64(std::floor(y / GRIND_CELL)) + (1 << 19);
        return (uint64(mapId) << 40) | (uint64(cx & 0xFFFFF) << 20) | uint64(cy & 0xFFFFF);
    }

    std::unordered_map<uint64, std::vector<GrindSpawn>> const& GrindSpawns()
    {
        static std::unordered_map<uint64, std::vector<GrindSpawn>> byMap;
        static std::once_flag once;
        std::call_once(once, []()
        {
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(data.id);
                if (!t || t->npcflag || (t->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE)) ||
                    (t->flags_extra & CREATURE_FLAG_EXTRA_CIVILIAN) || t->rank != CREATURE_ELITE_NORMAL)
                    continue;
                byMap[GrindCellKey(data.mapid, data.posX, data.posY)].push_back({data.posX, data.posY, t->Entry, t->minlevel, t->maxlevel,
                                             data.spawntimesecs});
            }
        });
        return byMap;
    }

    // a creature template's melee damage per second at `level` (the core's base stats: NewRpgCoop's
    // TemplateMeleeDps), less the bot's armour (Unit::CalcArmorReducedDamage)
    float GrindMobDps(Player* bot, CreatureTemplate const* t, uint8 level)
    {
        CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, t->unit_class);
        if (!stats)
            return 0.0f;
        float const swing = std::max(float(t->BaseAttackTime), 1.0f) / float(IN_MILLISECONDS);
        float const base = stats->GenerateBaseDamage(t);
        float const dps =
            ((base + base * 1.5f) / 2.0f + float(stats->AttackPower) / 14.0f * swing) * t->DamageModifier / swing;
        float const armor = float(bot->GetArmor());
        float const divisor =
            level < 60 ? 400.0f + 85.0f * level : 400.0f + 85.0f * level + 4.5f * 85.0f * (level - 59);
        return dps * (1.0f - std::clamp(armor / (armor + divisor), 0.0f, 0.75f));
    }

    float FoodRegen(Player* bot, Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || proto->RequiredLevel > bot->GetLevel())
            return 0.0f;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
        if (!spell)
            return 0.0f;
        float best = 0.0f;
        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            if (spell->Effects[e].ApplyAuraName == SPELL_AURA_OBS_MOD_HEALTH && spell->Effects[e].Amplitude > 0)
                best = std::max(best, float(spell->Effects[e].CalcValue(bot)) /
                                          (float(spell->Effects[e].Amplitude) / float(IN_MILLISECONDS)));
        return best;
    }

    // its health back per second out of a fight: the best food in its bags (SPELL_AURA_OBS_MOD_HEALTH, eaten
    // sitting), else the core's spirit regeneration (Player::OCTRegenHPPerSpirit, per second)
    float GrindRegen(Player* bot)
    {
        float best = bot->OCTRegenHPPerSpirit() * float(bot->GetStat(STAT_SPIRIT));
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            best = std::max(best, FoodRegen(bot, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i)));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                for (uint32 i = 0; i < container->GetBagSize(); ++i)
                    best = std::max(best, FoodRegen(bot, container->GetItemByPos(uint8(i))));
        return best;
    }
}

// [zone23] coordinator 2026-11-09: a grind kill's whole cycle, not its fight alone: the fight, the walk to the next
// mob at the spot's spawn density, the rest for the damage it took (food in its bags, else its spirit regeneration),
// and no faster than the spot's spawns come back (all of them killed in turn: their mean respawn over their number).
// b372 [zone19]: Colian L17 "50699 xp in 14339 s (65 xp a kill of 18 s)" counted the fights only and kept the
// draenei grinding in Bloodmyst against Darkshore's quests
float NewRpgBaseAction::GrindCycleSeconds(Player* bot, WorldPosition const& spot, float killSeconds, std::string& parts)
{
    SlowScope slow("grind cycle", bot ? &bot->GetName() : nullptr);  // [perf26]
    uint8 const level = bot->GetLevel();
    uint8 const gray = Acore::XP::GetGrayLevel(level);
    float const radius = sPlayerbotAIConfig.SightDistance;
    FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
    auto const& all = GrindSpawns();
    uint32 count = 0;
    float respawn = 0.0f;
    float mobDps = 0.0f;
    int32 const reach = int32(std::ceil(radius / GRIND_CELL));
    for (int32 dx = -reach; dx <= reach; ++dx)
        for (int32 dy = -reach; dy <= reach; ++dy)
    {
        auto const cell = all.find(GrindCellKey(spot.GetMapId(), spot.GetPositionX() + float(dx) * GRIND_CELL,
                                                spot.GetPositionY() + float(dy) * GRIND_CELL));
        if (cell == all.end())
            continue;
        for (GrindSpawn const& g : cell->second)
        {
            if (g.maxLevel <= gray || g.minLevel > level + 1 ||
                std::hypot(g.x - spot.GetPositionX(), g.y - spot.GetPositionY()) > radius)
                continue;
            CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(g.entry);
            FactionTemplateEntry const* faction = t ? sFactionTemplateStore.LookupEntry(t->faction) : nullptr;
            if (!t || !faction || (own && own->IsFriendlyTo(*faction)))
                continue;
            ++count;
            respawn += float(g.respawnSec);
            mobDps += GrindMobDps(bot, t, std::clamp<uint8>(level, g.minLevel, g.maxLevel));
        }
    }
    if (!count)
    {
        parts = "no mob of its level within sight of the spot";
        return -1.0f;
    }
    respawn /= float(count);
    mobDps /= float(count);
    float const speed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    // the mean gap to the nearest other spawn at that density (Poisson: half the root of the area per spawn)
    float const walk = 0.5f * std::sqrt(float(M_PI) * radius * radius / float(count)) / speed;
    float const regen = GrindRegen(bot);
    float const rest = regen > 0.0f ? mobDps * killSeconds / regen : 0.0f;
    float const cycle = killSeconds + walk + rest;
    float const camped = respawn / float(count);
    parts = Acore::StringFormat("fight {:.0f} s + walk {:.0f} s ({} spawns in sight) + rest {:.0f} s ({:.1f} dps "
                                "taken, {:.1f} hp/s back); respawn floor {:.0f} s",
                                killSeconds, walk, count, rest, mobDps, regen, camped);
    return std::max(cycle, camped);
}

uint32 NewRpgBaseAction::ChooseQuestByRate(WorldPosition& giverPos)
{
    uint8 const level = bot->GetLevel();
    float bestRate = 0.0f;
    uint32 bestQuest = 0;
    float bestCompleteRate = 0.0f;  // [guild10] hand-ins first, then active objectives, then starters
    uint32 bestCompleteQuest = 0;
    giverPos = WorldPosition();
    bool const rested = bot->GetRestBonus() > 0.0f;
    // [guild90] the quest it works on stays its choice until it is done, postponed or has no spot left (finish the
    // active quest before switching: b317 Eraelor flitted q919 <-> q487 on every IDLE); a hand-in nearer than that
    // quest's spot is taken on the way
    uint32 const counter = bot->GetGUID().GetCounter();
    uint32 committed = 0;
    uint32 resume = 0;
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        auto const found = committedQuest.find(counter);
        if (found != committedQuest.end())
            committed = found->second;
        auto const back = resumeQuest.find(counter);
        if (back != resumeQuest.end())
            resume = back->second;
    }
    // [flit] a quest left for another one while its camps respawn is the started quest: it is taken up again once the
    // other is no longer being worked on or its own respawn is due (the world clock), never dropped (b341 Irorm q218)
    if (resume)
    {
        bool const open =
            bot->GetQuestStatus(resume) == QUEST_STATUS_INCOMPLETE && !botAI->lowPriorityQuest.count(resume);
        if (!open)
        {
            std::lock_guard<std::mutex> guard(questNoteLock);
            resumeQuest.erase(counter);
        }
        else if (resume != committed &&
                 (!committed || bot->GetQuestStatus(committed) != QUEST_STATUS_INCOMPLETE ||
                  !EarliestRespawn(bot, resume)))
        {
            QuestStats::LogEvent(bot, "resume", resume,
                                 "from=q" + std::to_string(committed) + " reason=" +
                                     (EarliestRespawn(bot, resume) ? "the other quest is no longer worked on"
                                                                   : "its camps' respawn is due"));
            committed = resume;
            std::lock_guard<std::mutex> guard(questNoteLock);
            committedQuest[counter] = resume;
            resumeQuest.erase(counter);
        }
    }
    float committedDistance = -1.0f;
    float committedSeconds = -1.0f;
    struct QuestCandidate
    {
        uint32 questId;
        float rate, distance, seconds;
        bool complete;
    };
    std::vector<QuestCandidate> candidates;
    float nearestHandIn = std::numeric_limits<float>::max();
    uint32 nearestHandInQuest = 0;
    std::string committedWhy;
    if (committed && bot->GetQuestStatus(committed) != QUEST_STATUS_INCOMPLETE)
        committedWhy = bot->GetQuestStatus(committed) == QUEST_STATUS_COMPLETE ? "done, hand it in"
                       : bot->GetQuestStatus(committed) == QUEST_STATUS_REWARDED ? "handed in"
                                                                                : "left the log";
    else if (committed && botAI->lowPriorityQuest.count(committed))
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        auto const& reasons = postponeReasons[counter];
        auto const why = reasons.find(committed);
        committedWhy = "postponed: " + (why != reasons.end() ? why->second : std::string("no progress or no walk"));
    }

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        // [guild12] a failed quest (timed, escort) never leaves the log by itself: it goes, and is not taken again
        if (questId && bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED)
        {
            AbandonFailedQuest(questId);
            continue;
        }
        if (!questId || botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        // [guild13] a spell-unlock class quest: its spell comes by AutoLearnQuestSpells, the quest leaves the log
        if (IsSpellUnlockClassQuest(quest))
        {
            LOG_INFO("playerbots", "[rpg] {} L{} ABANDON q{} {} reason: class quest for a spell (granted)",
                     bot->GetName(), bot->GetLevel(), questId, quest->GetTitle());
            bot->RemoveActiveQuest(questId, false);
            continue;
        }
        // [flit] a quest whose open objectives only a tool's use or a spell / script credits is no candidate: it is
        // noted once and skipped here, never entered and left on the next tick (b341: 30 of 121 switches were such
        // DO_QUEST -> POSTPONE hops, again after every level-up)
        if (std::string why; bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE &&
                             (NeedsItemUseCredit(quest, &why) || NeedsSpellCredit(bot, quest, true, &why)))
        {
            NotePostpone(bot, questId, "unsupported: " + why);
            if (questId == committed)
                committedWhy = "postponed: unsupported: " + why;
            continue;
        }
        // [guild14] a quest ended only at a script-spawned object (no bot runs the script) leaves the log
        if (EnderNeedsScript(questId))
        {
            LOG_INFO("playerbots", "[rpg] {} L{} ABANDON q{} {} reason: its ender is an object a script spawns",
                     bot->GetName(), bot->GetLevel(), questId, quest->GetTitle());
            bot->RemoveActiveQuest(questId, false);
            continue;
        }

        // [quest14] a grey quest nobody needs is no candidate (the route build abandons it)
        if (GuildCoordinator::IsGreyQuest(bot, quest) && !GuildCoordinator::HumanCharacter(bot) &&
            sGuildCoordinator.GreyQuestNeed(bot, quest).empty())
            continue;

        float distance = std::numeric_limits<float>::max();
        std::vector<POIInfo> poiInfo;
        WorldPosition dalaranTarget;
        if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            for (POIInfo const& poi : poiInfo)
                distance = std::min(distance, bot->GetDistance2d(poi.pos.x, poi.pos.y));
        }
        else if (GetDalaranQuestTarget(questId, dalaranTarget))
            distance = bot->GetDistance2d(dalaranTarget.GetPositionX(), dalaranTarget.GetPositionY());
        else
            continue;

        bool const complete = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
        float time = distance / QUEST_TRAVEL_SPEED;
        // [guild90] the work left, not a whole objective's time for every quest: the quest nearest completion first
        if (!complete)
            time += QUEST_OBJECTIVE_TIME * RemainingShare(bot, quest) + QUEST_UNKNOWN_DISTANCE / QUEST_TRAVEL_SPEED;
        if (questId == committed && !complete)
        {
            committedDistance = distance;
            committedSeconds = time;
        }

        float const rate = QUEST_IN_LOG_BONUS * GetQuestWeight(quest, rested) * quest->XPValue(level) /
                           std::max(time, 1.0f);
        candidates.push_back({questId, rate, distance, time, complete});
    }

    // [guild90] hand-ins are grouped like a player's: a done quest whose ender also ends (within GROUP_ENDER_RADIUS)
    // a quest still being worked on waits for it - unless the bot passes the ender now (the giver search hands it
    // in on the way then anyway)
    std::vector<std::pair<uint32, WorldPosition>> openEnders;
    for (QuestCandidate const& c : candidates)
    {
        WorldPosition ender;
        if (!c.complete && NearestEnderSpot(bot, c.questId, ender))
            openEnders.push_back({c.questId, ender});
    }
    for (QuestCandidate const& c : candidates)
    {
        if (c.complete)
        {
            WorldPosition ender;
            bool deferred = false;
            uint32 waitsFor = 0;
            if (c.distance > HAND_IN_PASS_DISTANCE && NearestEnderSpot(bot, c.questId, ender))
                for (auto const& [open, openEnder] : openEnders)
                    if (openEnder.GetMapId() == ender.GetMapId() &&
                        openEnder.GetExactDist2d(ender.GetPositionX(), ender.GetPositionY()) <= GROUP_ENDER_RADIUS)
                    {
                        deferred = true;
                        waitsFor = open;
                        break;
                    }
            if (deferred)
            {
                std::lock_guard<std::mutex> guard(questNoteLock);
                auto& logged = deferredHandIns[counter];
                if (logged.insert(c.questId).second)
                    QuestStats::LogEvent(bot, "handin-grouped", c.questId,
                                         "waits for q" + std::to_string(waitsFor) + " (same ender)");
                continue;
            }
            if (c.distance < nearestHandIn)
            {
                nearestHandIn = c.distance;
                nearestHandInQuest = c.questId;
            }
            if (c.rate > bestCompleteRate)
            {
                bestCompleteRate = c.rate;
                bestCompleteQuest = c.questId;
            }
        }
        else if (c.rate > bestRate)
        {
            bestRate = c.rate;
            bestQuest = c.questId;
        }
    }
    auto const secondsOf = [&](uint32 questId) -> float
    {
        for (QuestCandidate const& c : candidates)
            if (c.questId == questId)
                return c.seconds;
        return -1.0f;
    };
    // [guild90] every switch away from the active quest: one line with the computed costs
    auto const logSwitch = [&](uint32 chosen, std::string const& why)
    {
        if (!committed || chosen == committed || committed == resume)  // [flit] a respawn switch is logged once
            return;
        QuestStats::LogEvent(bot, "target-switch", chosen,
                             "from=q" + std::to_string(committed) + " finish_s=" +
                                 (committedSeconds >= 0.0f ? std::to_string(uint32(committedSeconds)) : "none") +
                                 " other_s=" + std::to_string(int32(secondsOf(chosen))) + " reason=" + why);
    };

    auto const note = [&](uint32 chosen, std::string const& why)
    {
        std::lock_guard<std::mutex> guard(questNoteLock);
        choiceNotes[counter] = why;
        if (chosen && bot->GetQuestStatus(chosen) == QUEST_STATUS_INCOMPLETE)
            committedQuest[counter] = chosen;
        else if (!chosen || (committed && committedDistance < 0.0f))
            committedQuest.erase(counter);
        if (committed && (chosen != committed || bot->GetQuestStatus(committed) != QUEST_STATUS_INCOMPLETE))
        {
            auto const old = claimOf.find(counter);
            if (old != claimOf.end() && old->second == committed)
            {
                claims[committed].erase(counter);
                claimOf.erase(old);
            }
        }
    };
    auto const leaving = [&]() -> std::string
    {
        if (!committed)
            return "";
        if (!committedWhy.empty())
            return "; left q" + std::to_string(committed) + ": " + committedWhy;
        if (committedDistance < 0.0f)
            return "; left q" + std::to_string(committed) + ": no unspent objective spot it can walk to";
        return "";
    };

    // [guild90] the active quest first; a hand-in only when its ender is nearer than the active quest's spot
    if (committedDistance >= 0.0f && !(nearestHandInQuest && nearestHandIn < committedDistance))
    {
        note(committed, "continues the active quest (" + std::to_string(uint32(committedDistance)) + " yd to its spot)");
        return committed;
    }
    if (bestCompleteQuest)
    {
        uint32 const handIn = committedDistance >= 0.0f ? nearestHandInQuest : bestCompleteQuest;
        if (committedDistance < 0.0f)
            logSwitch(handIn, "hand-in first" + leaving());
        note(committedDistance >= 0.0f ? committed : 0,
             committedDistance >= 0.0f
                 ? "hand-in on the way (" + std::to_string(uint32(nearestHandIn)) + " yd), then back to q" +
                       std::to_string(committed)
                 : "hand-in first" + leaving());
        if (committedDistance >= 0.0f)
        {
            std::lock_guard<std::mutex> guard(questNoteLock);
            committedQuest[counter] = committed;
        }
        return handIn;
    }

    // [guild10] a new quest only once nothing in the log is left to do; every bot levels by quests
    if (bestQuest)
    {
        note(bestQuest, "best xp per second of the log" + leaving());
        logSwitch(bestQuest, "best xp per second of the log" + leaving());
        return bestQuest;
    }
    note(0, "nothing left in the log" + leaving());

    // [guild15] zone change by value (user queue item 4): every starter the bot may take now on the continents it can
    // reach is priced by the travel planner's honest route (walk, Deeprun Tram, boats, portals: a giver on another map
    // or landmass by the arrivals there), its quests are summed per zone, and the zone whose quests give the most
    // experience per second of travel + work wins; inside it the best single quest from the bot. A zone is exhausted
    // when it holds no starter the bot may take (computed, no timer). From 68 Northrend joins by its boats.
    uint32 const botLandmass = TravelRouteMgr::Landmass(WorldPosition(bot));
    constexpr uint32 LANDMASS_ZONE_KEY = 0x100000;  // above every AreaTable id
    std::map<uint32, std::vector<std::pair<WorldPosition, float>>> arrivals;  // map -> docks / portal exits, seconds
    auto const arrivalsOf = [&](uint32 mapId) -> std::vector<std::pair<WorldPosition, float>> const&
    {
        auto found = arrivals.find(mapId);
        if (found == arrivals.end())
        {
            found = arrivals.emplace(mapId, std::vector<std::pair<WorldPosition, float>>()).first;
            if (!bot->GetMap()->Instanceable())
                sTravelRouteMgr.GetArrivalCosts(bot, mapId, found->second);
        }
        return found->second;
    };
    auto const continent = [&](uint32 mapId)
    {
        return mapId == 0 || mapId == 1 || mapId == 530 ||
               (mapId == NORTHREND_MAP_ID && level >= NORTHREND_ENTRY_LEVEL);
    };

    struct StartCandidate
    {
        uint32 questId;
        uint32 zone;
        float travel, local, xp, rate;
        WorldPosition pos;
        int32 questLevel;
        // [zone13] the quest's walk on the giver's map: the objective (its POI centre, else the giver) and the ender
        // (its nearest spawn to the objective, else the giver); the yards giver -> objective -> ender
        float ox = 0.0f, oy = 0.0f, ex = 0.0f, ey = 0.0f;
        float walkYd = 0.0f;
    };
    std::vector<StartCandidate> starts;
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    Continent const botContinent = ContinentOf(WorldPosition(bot));
    // [zone19] why the starters of its level were left out, for the line of a bot with none left
    uint32 outClosed = 0, outRoute = 0, outTaken = 0, outOther = 0, inBand = 0, outCamp = 0;
    uint32 outHostile = 0;  // [zone161] a giver of the other faction (the line left them uncounted)
    // [zone20] a bot whose own continent holds no starter it may take now goes over to the old world's other one (a
    // player's Darkshore -> Wetlands, Westfall -> Darkshore): the second pass opens it, its boats priced by the travel
    // planner as any route. b367: 9 of the 13 "[zone19] no zone" bots had 286-331 starters of their band on the
    // continent closed until Outland's level and none left on their own
    bool openedOther = false;  // [zone21]
    // [group22] a group's leader chooses its camps for its weakest member: the lowest level of the group on its map
    // (the group's pulls are the party judge's; the camp filter is the never-fight rule of its weakest)
    uint8 campLevel = level;
    if (Group* group = bot->GetGroup(); group && group->GetLeaderGUID() == bot->GetGUID())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            // [deaths23] ... of the members that come with it (an own-plan member walks its own camps)
            if (Player* m = ref->GetSource();
                m && m->GetMapId() == bot->GetMapId() && (m == bot || SoloSafety::FightsWith(bot, m)))
                campLevel = std::min<uint8>(campLevel, m->GetLevel());
    if (campLevel < level)
        LOG_INFO("playerbots", "[group22] {} L{} chooses its zone's camps for its group's weakest member: L{}",
                 bot->GetName(), level, campLevel);
    for (int pass = 0; pass < 2 && starts.empty(); ++pass)
    {
        bool const otherOldWorld = pass == 1;
        openedOther = otherOldWorld;
        outClosed = outRoute = outTaken = outOther = inBand = outCamp = outHostile = 0;
        for (QuestStartPoint const& start : GetQuestStartPoints())
        {
            if (!EventSpawnPresent(start.event))  // [guild15]
                continue;
            if (start.mapId != bot->GetMapId() && !continent(start.mapId))
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(start.questId);
            if (!quest || quest->GetMinLevel() > level)
                continue;

            int32 const questLevel = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : level;
            // [guild10] down to the last level that still gives experience (green), not only the current band
            if (questLevel > level + 2 || questLevel <= Acore::XP::GetGrayLevel(level))
                continue;
            ++inBand;
            // [guild15] its own continent until the level that needs another
            Continent const giverContinent = ContinentOf(WorldPosition(start.mapId, start.x, start.y, start.z));
            if (!ContinentOpen(botContinent, giverContinent, level) &&
                !(otherOldWorld &&
                  (giverContinent == Continent::EasternKingdoms || giverContinent == Continent::Kalimdor) &&
                  (botContinent == Continent::EasternKingdoms || botContinent == Continent::Kalimdor)))
            {
                ++outClosed;
                continue;
            }

            if (bot->GetQuestStatus(start.questId) != QUEST_STATUS_NONE || botAI->lowPriorityQuest.count(start.questId))
            {
                ++outTaken;
                continue;
            }


            if (start.creatureFaction && botFaction)
            {
                FactionTemplateEntry const* giverFaction = sFactionTemplateStore.LookupEntry(start.creatureFaction);
                if (giverFaction && giverFaction->IsHostileTo(*botFaction))
                {
                    ++outHostile;  // [zone161]
                    continue;
                }
            }

            WorldPosition const giver(start.mapId, start.x, start.y, start.z);
            float travelSeconds = std::numeric_limits<float>::max();
            if (start.mapId == bot->GetMapId() && TravelRouteMgr::Landmass(giver) == botLandmass)
                // [guild10] the whole landmass
                travelSeconds = bot->GetDistance2d(start.x, start.y) / QUEST_TRAVEL_SPEED;
            else
            {
                // another continent or landmass: the route's arrival there, then the walk to the giver
                uint32 const giverLandmass = TravelRouteMgr::Landmass(giver);
                for (auto const& [arrival, seconds] : arrivalsOf(start.mapId))
                {
                    if (TravelRouteMgr::Landmass(arrival) != giverLandmass)
                        continue;
                    float const fromArrival =
                        std::hypot(arrival.GetPositionX() - start.x, arrival.GetPositionY() - start.y);
                    if (fromArrival <= QUEST_MAX_GIVER_DISTANCE)
                        travelSeconds = std::min(travelSeconds, seconds + fromArrival / QUEST_TRAVEL_SPEED);
                }
            }
            if (travelSeconds == std::numeric_limits<float>::max())
            {
                ++outRoute;  // [zone19]
                continue;
            }

            if (!IsQuestWorthDoing(quest) || !IsQuestCapableDoing(quest) || !bot->CanTakeQuest(quest, false))
            {
                ++outOther;  // [zone19]
                continue;
            }

            float objectiveDistance = QUEST_UNKNOWN_DISTANCE;
            float objX = start.x, objY = start.y;  // [zone13]
            bool objKnown = false;
            if (QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(start.questId))
            {
                for (QuestPOI const& poi : *pois)
                {
                    if (poi.ObjectiveIndex < 0 || poi.MapId != start.mapId || poi.points.empty())
                        continue;

                    float cx = 0.0f;
                    float cy = 0.0f;
                    for (QuestPOIPoint const& point : poi.points)
                    {
                        cx += point.x;
                        cy += point.y;
                    }
                    cx /= poi.points.size();
                    cy /= poi.points.size();
                    objectiveDistance = std::sqrt((cx - start.x) * (cx - start.x) + (cy - start.y) * (cy - start.y));
                    objX = cx;
                    objY = cy;
                    objKnown = true;
                    break;
                }
            }

            // [zone21] coordinator 2026-11-09: the zone's value counts the mobs the bot fights there - a quest whose
            // objective camp holds a mob it never fights solo ([guild12]: orange / red con, an elite; its spawns
            // within the core's largest aggro reach of the objective's centre, hostile to the bot) is no quest of its
            // solo zone rate (its co-op / help request prices it there). b368: L12 bots sent to Westfall's L13-18
            // camps (Manue at Alexston Farmstead, Guinudrond, Peny), L14-15 to Bloodmyst's Sunhawk / Blacksilt L16-17
            if (objKnown)
            {
                QuestSpawnIndex const& spawnIndex = GetQuestSpawnIndex();
                bool walled = false;
                for (int32 dx = -1; dx <= 1 && !walled; ++dx)
                    for (int32 dy = -1; dy <= 1 && !walled; ++dy)
                    {
                        auto const cell = spawnIndex.mobCells.find(
                            MobCellKey(start.mapId, objX + float(dx) * MAX_AGGRO_RADIUS,
                                       objY + float(dy) * MAX_AGGRO_RADIUS));
                        if (cell == spawnIndex.mobCells.end())
                            continue;
                        for (MobSpawn const& m : cell->second)
                        {
                            if (!SpawnPresent(m) || std::hypot(m.x - objX, m.y - objY) > MAX_AGGRO_RADIUS)
                                continue;
                            FactionTemplateEntry const* mobFaction = sFactionTemplateStore.LookupEntry(m.tmpl->faction);
                            if (!mobFaction || !botFaction || !mobFaction->IsHostileTo(*botFaction))
                                continue;
                            bool const elite = m.tmpl->rank == CREATURE_ELITE_ELITE ||
                                               m.tmpl->rank == CREATURE_ELITE_RAREELITE ||
                                               m.tmpl->rank == CREATURE_ELITE_WORLDBOSS;
                            if (elite || Acore::XP::GetColorCode(campLevel, m.tmpl->maxlevel) <= XP_ORANGE)
                                walled = true;
                        }
                    }
                if (walled)
                {
                    ++outCamp;
                    continue;
                }
            }

            // [zone13] the quest's own walk: giver -> objective -> its ender (the nearest ender spawn on the map to the
            // objective; none known: back to the giver), plus the work itself. Before, giver -> objective and back only

            float enderX = start.x, enderY = start.y;
            {
                QuestSpawnIndex const& spawnIndex = GetQuestSpawnIndex();
                auto const enders = spawnIndex.enders.find(start.questId);
                float bestEnder = std::numeric_limits<float>::max();
                if (enders != spawnIndex.enders.end())
                    for (QuestSpot const& e : enders->second)
                        if (e.mapId == start.mapId)
                            if (float const d = std::hypot(e.x - objX, e.y - objY); d < bestEnder)
                            {
                                bestEnder = d;
                                enderX = e.x;
                                enderY = e.y;
                            }
            }
            float const toEnder =
                objKnown ? std::hypot(enderX - objX, enderY - objY) : objectiveDistance;  // no POI: as far back
            float const walkYd = objectiveDistance + toEnder;
            float local = walkYd / QUEST_TRAVEL_SPEED + QUEST_OBJECTIVE_TIME;
            // [giver15] a giver whose script gives the quest-giver flag: only with a trigger the bot can do, its
            // seconds in the quest's own time
            if (NewRpgGiverTrigger::IsScripted(start.questId))
            {
                if (!NewRpgGiverTrigger::Plannable(bot, start.questId))
                    continue;
                local += NewRpgGiverTrigger::Seconds(
                    bot, start.questId, DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo));
            }
            // the core's quest XP at the bot's level (Quest::XPValue: a quest below its level gives less, gray none)
            float const xp = GetQuestWeight(quest, rested) * quest->XPValue(level);
            // the zone the quest belongs to (QuestSortID > 0); a class / profession / holiday quest (sort <= 0) counts
            // to the bot's own zone on its landmass, else to its giver's landmass (no terrain lookup of far grids)
            uint32 zone = quest->GetZoneOrSort() > 0 ? uint32(quest->GetZoneOrSort()) : 0;
            if (!zone)
                zone = start.mapId == bot->GetMapId() && TravelRouteMgr::Landmass(giver) == botLandmass
                           ? bot->GetZoneId()
                           : LANDMASS_ZONE_KEY + TravelRouteMgr::Landmass(giver);
            float const rate = xp / (travelSeconds + local);
            // one entry per quest (a quest with several givers: the cheapest)
            auto const same = std::find_if(starts.begin(), starts.end(),
                                           [&](StartCandidate const& c) { return c.questId == start.questId; });
            StartCandidate const candidate{start.questId, zone,  travelSeconds, local, xp,   rate,
                                           giver,         questLevel, objX, objY, enderX, enderY, walkYd};
            if (same != starts.end())
            {
                if (rate > same->rate)
                    *same = candidate;
                continue;
            }
            starts.push_back(candidate);
        }
    }

    // [zone21] the second pass's own line: b368 logged none, the bots it opened the other continent for not named
    if (openedOther && !starts.empty())
        LOG_INFO("playerbots", "[zone21] {} L{} opens the old world's other continent: {} starters it may take there "
                 "(its own holds none)",
                 bot->GetName(), level, starts.size());
    struct ZoneValue
    {
        float xp = 0.0f;
        float local = 0.0f;
        float travel = std::numeric_limits<float>::max();
        uint32 quests = 0;
        int32 minLevel = 255;
        int32 maxLevel = 0;
        float Rate() const { return xp / (travel + local); }
    };
    std::map<uint32, ZoneValue> zones;
    std::map<uint32, std::vector<StartCandidate const*>> byZone;  // [zone13]
    for (StartCandidate const& c : starts)
    {
        ZoneValue& z = zones[c.zone];
        z.xp += c.xp;
        z.local += c.local;
        z.travel = std::min(z.travel, c.travel);
        ++z.quests;
        z.minLevel = std::min(z.minLevel, c.questLevel);
        z.maxLevel = std::max(z.maxLevel, c.questLevel);
        byZone[c.zone].push_back(&c);
    }
    // [zone13] the zone's local time is its real route: from the nearest giver, each quest's giver -> objective ->
    // ender, then on to the next quest's giver nearest to that ender (the nearest-neighbour tour over its quests, as
    // the route planner walks them). Before, the sum of each quest's giver -> objective -> giver alone: the walk
    // between the zone's quests was not counted (b353 Ellosera L13 on Azuremyst routes of ~9 km for green quests)
    for (auto& [zone, z] : zones)
    {
        std::vector<StartCandidate const*> left = byZone[zone];
        float tourYd = 0.0f;
        StartCandidate const* at = nullptr;
        for (StartCandidate const* c : left)
            if (!at || c->travel < at->travel)
                at = c;
        while (at)
        {
            left.erase(std::find(left.begin(), left.end(), at));
            tourYd += at->walkYd;
            StartCandidate const* next = nullptr;
            float bestGap = std::numeric_limits<float>::max();
            for (StartCandidate const* c : left)
                if (c->pos.GetMapId() == at->pos.GetMapId())
                    if (float const gap = std::hypot(c->pos.GetPositionX() - at->ex, c->pos.GetPositionY() - at->ey);
                        gap < bestGap)
                    {
                        bestGap = gap;
                        next = c;
                    }
            if (next)
                tourYd += bestGap;
            else if (!left.empty())
                next = left.front();  // another map of the zone key: its walk is in its travel already
            at = next;
        }
        z.local = tourYd / QUEST_TRAVEL_SPEED + float(z.quests) * QUEST_OBJECTIVE_TIME;
    }
    uint32 bestZone = 0;
    float bestZoneRate = 0.0f;
    for (auto const& [zone, value] : zones)
        if (!bestZone || value.Rate() > bestZoneRate)
        {
            bestZone = zone;
            bestZoneRate = value.Rate();
        }
    if (!bestZone)
    {
        // [zone19] no zone with a quest it may take now: why the starters of its band were left out (the grind for
        // the next quest's level follows); once per level
        static std::mutex noneLock;
        static std::unordered_map<uint32, uint8> noneLogged;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> guard(noneLock);
            uint8& last = noneLogged[counter];
            fresh = last != level;
            last = level;
        }
        if (fresh)
            LOG_INFO("playerbots", "[zone19] {} L{} has no zone with a quest it may take: {} starters of its band, "
                     "{} on a continent closed at its level, {} no route there, {} taken / done / postponed, {} it "
                     "cannot take, {} in a camp it never fights solo, {} of a hostile giver",
                     bot->GetName(), level, inBand, outClosed, outRoute, outTaken, outOther, outCamp, outHostile);
        return 0;
    }

    // [zone13] one line per change of the best zone: the top 3 zones by rate
    {
        bool fresh = false;
        {
            static std::mutex zone13Lock;
            static std::unordered_map<uint32, uint32> zone13Last;
            std::lock_guard<std::mutex> guard(zone13Lock);
            uint32& last = zone13Last[counter];
            fresh = last != bestZone;
            last = bestZone;
        }
        if (fresh)
        {
            std::vector<std::pair<float, uint32>> ranked;
            for (auto const& [zone, value] : zones)
                ranked.emplace_back(value.Rate(), zone);
            std::sort(ranked.begin(), ranked.end(), std::greater<>());
            std::string top;
            for (size_t i = 0; i < ranked.size() && i < 3; ++i)
            {
                ZoneValue const& v = zones[ranked[i].second];
                AreaTableEntry const* area =
                    ranked[i].second < LANDMASS_ZONE_KEY ? sAreaTableStore.LookupEntry(ranked[i].second) : nullptr;
                top += Acore::StringFormat("{}{} {} xp/h ({} quests L{}-{}, {:.0f} xp, travel {:.0f} s, route {:.0f} s)",
                                           top.empty() ? "" : "; ",
                                           area ? std::string(area->area_name[0])
                                                : "key " + std::to_string(ranked[i].second),
                                           uint32(v.Rate() * 3600.0f), v.quests, v.minLevel, v.maxLevel, v.xp,
                                           v.travel, v.local);
            }
            LOG_INFO("playerbots", "[zone13] {} L{} best zones: {}", bot->GetName(), level, top);
        }
    }

    StartCandidate const* best = nullptr;
    for (StartCandidate const& c : starts)
        if (c.zone == bestZone && (!best || c.rate > best->rate))
            best = &c;
    if (!best)
        return 0;
    giverPos = best->pos;

    // [zone] one line per change of the bot's questing zone: from -> to, why and the route
    uint32 const hereZone = bot->GetZoneId();
    // [flit] a zone change is a walk of best->travel seconds; when the level a quest of this landmass needs comes
    // sooner by killing mobs of its level here (the core's kill experience over the formula's kill time), the bot
    // grinds to it instead (user rule: grind only toward a target - here the nearest next-level quest). b341: five
    // L3 draenei, Ammen Vale done, left for Shadowglen 3000 s away (Azure Watch's quests open at 4)
    if (bestZone != hereZone)
    {
        WorldPosition nextGiver;
        WorldPosition nextGrind;
        // [stuck8] with its grind spot: the grind needs mobs of the bot's level where it stands (b343 Orton L9 on the
        // Deeprun Tram, map 369: "stays ... by grinding" toward q6661, no mob of its level on the map, IDLE / GO_GRIND
        // / WANDER_RANDOM 160 alerts in a loop). No grind spot on this map: no grind; else its walk counts too
        bool sweepPending = false;  // [perf214]
        uint32 const nextQuest =
            ChooseNextLevelQuest(nextGiver, nextGrind, true, choicePacing ? &sweepPending : nullptr);
        if (sweepPending)
        {
            choicePending = true;  // the IDLE look asks again next tick
            giverPos = WorldPosition();
            return 0;
        }
        Quest const* next = nextQuest ? sObjectMgr->GetQuestTemplate(nextQuest) : nullptr;
        ContentLevels const content = GetContentLevelsForMapAndZone(bot->GetMapId(), hereZone);
        CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, CLASS_WARRIOR);
        float const dps = next ? DpsEstimate::EstimateDps(bot, nullptr, DpsEstimate::BuffContext::Solo) : 0.0f;
        if (next && stats && dps > 0.0f && TravelRouteMgr::Landmass(nextGiver) == botLandmass &&
            next->GetMinLevel() > level)
        {
            uint32 missing = 0;
            for (uint8 l = level; l < next->GetMinLevel(); ++l)
                missing += sObjectMgr->GetXPForLevel(l);
            uint32 const have = bot->GetUInt32Value(PLAYER_XP);
            missing = missing > have ? missing - have : 0;
            float const killXp = float(Acore::XP::BaseGain(level, level, content)) * sWorld->getRate(RATE_XP_KILL);
            float const killSeconds = float(stats->BaseHealth[std::min<uint32>(content, MAX_EXPANSIONS - 1)]) / dps;
            // [zone23] the kill's whole cycle at the grind spot (GrindCycleSeconds), not its fight alone
            std::string cycleParts;
            float const cycle = nextGrind != WorldPosition() && nextGrind.GetMapId() == bot->GetMapId()
                                    ? GrindCycleSeconds(bot, nextGrind, killSeconds, cycleParts)
                                    : -1.0f;
            float const grindSeconds =
                killXp > 0.0f && cycle > 0.0f
                    ? float(missing) / killXp * cycle + bot->GetExactDist(nextGrind) / QUEST_TRAVEL_SPEED
                    : -1.0f;
            // [zone19] coordinator 2026-11-09: the grind wins only when it earns more experience than the best zone
            // over the same time: the grind's `missing` xp in `grindSeconds` against that zone's quest rate (its
            // experience per second of travel and work, ZoneValue::Rate) over the time the travel leaves of it.
            // Before, the grind's seconds were set against the travel alone, as if the zone's quests gave nothing:
            // b357 -> b363 the bots grinding for a quest's level went 2 -> 6 -> 12
            float const zoneXp =
                zones[bestZone].Rate() * std::max(0.0f, grindSeconds - zones[bestZone].travel);
            if (grindSeconds >= 0.0f && float(missing) >= zoneXp)
            {

                bool fresh = false;
                {
                    std::lock_guard<std::mutex> guard(questNoteLock);
                    uint32& last = zoneTarget[counter];
                    fresh = last != hereZone;
                    last = hereZone;
                }
                if (fresh)
                    LOG_INFO("playerbots",
                             "[zone19] {} L{} stays: L{} for q{} here by grinding {} xp in {} s ({} xp a kill of {} "
                             "s) beats the {} xp zone {} gives in that time after its {} s travel (first q{}) "
                             "[zone23 kill cycle: {}]",
                             bot->GetName(), level, next->GetMinLevel(), nextQuest, missing, uint32(grindSeconds),
                             uint32(killXp), uint32(cycle), uint32(zoneXp), bestZone,
                             uint32(zones[bestZone].travel), best->questId, cycleParts);
                giverPos = WorldPosition();
                return 0;
            }
        }
    }
    if (bestZone != hereZone)
    {
        bool fresh = false;
        {
            std::lock_guard<std::mutex> guard(questNoteLock);
            uint32& last = zoneTarget[counter];
            fresh = last != bestZone;
            last = bestZone;
        }
        if (fresh)
        {
            auto const zoneName = [&](uint32 id) -> std::string
            {
                if (id >= LANDMASS_ZONE_KEY)
                    return "landmass " + std::to_string(id - LANDMASS_ZONE_KEY);
                AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
                return area ? std::string(area->area_name[0]) : "zone " + std::to_string(id);
            };
            auto const here = zones.find(hereZone);
            std::string const why =
                here == zones.end()
                    ? std::string("exhausted: no quest here it may take now")
                    : "better value: here " + std::to_string(here->second.quests) + " quests " +
                          std::to_string(uint32(here->second.Rate() * 3600.0f)) + " xp/h";
            ZoneValue const& to = zones[bestZone];
            std::string route = "walk";
            TravelRouteMgr::Leg leg;
            float seconds = 0.0f;
            if (!sTravelRouteMgr.FindRoute(bot, giverPos, leg, seconds))
                route = "none";
            else if (leg.kind == TravelRouteMgr::LegKind::Transport)
                route = "transport " + std::to_string(leg.lineEntry);
            else if (leg.kind == TravelRouteMgr::LegKind::Portal)
                route = "portal " + std::to_string(leg.portalIndex);
            LOG_INFO("playerbots", "[zone19] {} moves to {}: {} quests {} xp/h after {} s travel", bot->GetName(),
                     zoneName(bestZone), to.quests, uint32(to.Rate() * 3600.0f), uint32(to.travel));
            LOG_INFO("playerbots",
                     "[zone] {} L{} {} -> {}: {}; there {} quests L{}-{} {} xp/h (travel {} s), route {} {} s, "
                     "first q{} giver at map {} ({},{})",

                     bot->GetName(), level, zoneName(hereZone), zoneName(bestZone), why, to.quests, to.minLevel,
                     to.maxLevel, uint32(to.Rate() * 3600.0f), uint32(to.travel), route, uint32(seconds), best->questId,
                     giverPos.GetMapId(), int32(giverPos.GetPositionX()), int32(giverPos.GetPositionY()));
        }
    }
    return best->questId;
}

uint32 NewRpgBaseAction::ChooseNextLevelQuest(WorldPosition& giverPos, WorldPosition& grindPos, bool withGrindSpot,
                                              bool* pending)
{
    if (pending)
        *pending = false;
    uint8 const level = bot->GetLevel();
    uint32 bestMinLevel = std::numeric_limits<uint32>::max();
    float bestDistance = std::numeric_limits<float>::max();
    uint32 bestQuest = 0;
    giverPos = WorldPosition();
    grindPos = WorldPosition();

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    uint32 const botLandmassHere = TravelRouteMgr::Landmass(WorldPosition(bot));  // [travel57]
    std::unordered_map<uint32, bool> routeTo;  // [travel57] landmass -> a route there (this look only)
    for (QuestStartPoint const& start : GetQuestStartPoints())
    {
        if (start.mapId != bot->GetMapId() || !EventSpawnPresent(start.event))  // [guild15]
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(start.questId);
        if (!quest || quest->GetMinLevel() <= level || quest->GetMinLevel() > bestMinLevel)
            continue;

        if (bot->GetQuestStatus(start.questId) != QUEST_STATUS_NONE || botAI->lowPriorityQuest.count(start.questId))
            continue;

        if (start.creatureFaction && botFaction)
        {
            FactionTemplateEntry const* giverFaction = sFactionTemplateStore.LookupEntry(start.creatureFaction);
            if (giverFaction && giverFaction->IsHostileTo(*botFaction))
                continue;
        }

        if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false) ||
            !bot->SatisfyQuestPreviousQuest(quest, false) || !bot->SatisfyQuestSkill(quest, false))
            continue;
        // [flit] no grind toward a quest it will not take at that level either (b341 Maronic L1 on the tram ground
        // toward q6661, whose rats only its flute credits)
        if (IsSpellUnlockClassQuest(quest) || EnderNeedsScript(start.questId) ||
            !NewRpgGiverTrigger::Plannable(bot, start.questId) ||
            NeedsItemUseCredit(quest) ||
            NeedsSpellCredit(bot, quest, false))
            continue;
        // [stuck8] nor toward one in the other faction's zone
        if (quest->GetZoneOrSort() > 0 && TravelRouteMgr::IsEnemyZone(bot, uint32(quest->GetZoneOrSort())))
            continue;

        float const distance = bot->GetDistance2d(start.x, start.y);
        if (quest->GetMinLevel() == bestMinLevel && distance >= bestDistance)
            continue;
        // [travel57] nor toward a giver on another landmass it has no route to (its dock / portal walks given up)
        {
            WorldPosition const giver(start.mapId, start.x, start.y, start.z);
            uint32 const giverLandmass = TravelRouteMgr::Landmass(giver);
            if (giverLandmass != botLandmassHere)
            {
                auto known = routeTo.find(giverLandmass);
                if (known == routeTo.end())
                {
                    TravelRouteMgr::Leg leg;
                    float seconds = 0.0f;
                    known = routeTo.emplace(giverLandmass, sTravelRouteMgr.FindRoute(bot, giver, leg, seconds)).first;
                }
                if (!known->second)
                    continue;
            }
        }

        bestMinLevel = quest->GetMinLevel();
        bestDistance = distance;
        bestQuest = start.questId;
        giverPos = WorldPosition(start.mapId, start.x, start.y, start.z);
    }

    if (!bestQuest || !withGrindSpot)
        return bestQuest;

    // the grind spot of its level nearest the giver, other than the one the bot already stands on; [flit] the
    // nearest one whose walk it does not refuse (b341 Sylaenas L1: the same refused spot 37 times, GRIND -> MAZE ->
    // IDLE), else the nearest (the caller then tries its hub)
    std::vector<std::pair<float, WorldPosition>> grinds;
    for (WorldLocation const& loc : sTravelMgr.GetLocsPerLevelCache(level))
        if (loc.GetMapId() == bot->GetMapId() && bot->GetExactDist(loc) >= 10.0f &&
            !WalkGivenUp(bot, loc.GetPositionX(), loc.GetPositionY()))  // [holds9] not even as the fallback
            grinds.emplace_back(giverPos.GetExactDist(loc), WorldPosition(loc));
    std::sort(grinds.begin(), grinds.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    if (!grinds.empty())
        grindPos = grinds.front().second;
    // [grind42] the spots toward this giver already found all refused at this level are not judged again (each a
    // route search and a log line), and a look judges a bounded number of them: b389 W2 407 "TRIP_REFUSED grind
    // spot ... its walk passes" lines, Thyllyn 137 distinct spots toward one giver 7000 yd off, every update
    static std::mutex grindLock;
    static std::unordered_map<uint32, std::pair<uint8, uint32>> allRefused;  // bot -> (level, giver quest)
    {
        std::lock_guard<std::mutex> guard(grindLock);
        auto const found = allRefused.find(bot->GetGUID().GetCounter());
        if (found != allRefused.end() && found->second == std::make_pair(level, bestQuest))
            return bestQuest;  // the nearest stays the fallback; the caller's refusal check sends it to its hub
    }
    constexpr uint32 GRIND_SPOT_TRIES = 8;  // a search cost bound per look, not a behaviour rule
    // [perf214] coordinator 2026-11-11: the sweep's route searches paced over ticks - at most this many in one call
    // when the caller paces it; a spot already judged answers from TripRefused's verdict at this level (no search),
    // so the next call goes on where this one stopped, the same spots in the same order, the same choice. b438
    // Uraela L18: 8 refused 3200-3600 yd grind spots, 125 ms each, in one 1270 ms update. Each call goes at least
    // one spot past where the last one stopped (a verdict TripRefused does not keep - a walk given up, a danger zone,
    // another landmass - is asked again), so the sweep always ends
    constexpr uint64 SWEEP_SEARCHES_PER_TICK = 2;  // compute pacing per call, not a behaviour rule
    static std::unordered_map<uint32, std::tuple<uint8, uint32, uint32>> sweepStop;  // bot -> (level, quest, spots)
    uint32 pastStop = 0;
    if (pending)
    {
        std::lock_guard<std::mutex> guard(grindLock);
        auto const stop = sweepStop.find(bot->GetGUID().GetCounter());
        if (stop != sweepStop.end() && std::get<0>(stop->second) == level && std::get<1>(stop->second) == bestQuest)
            pastStop = std::get<2>(stop->second);
    }
    uint64 const searchesBefore = Leader::RouteSearchesOnThread();
    uint32 refused = 0;
    uint32 judgedSpots = 0;
    bool found = false;
    for (auto const& [d, pos] : grinds)
    {
        if (d > QUEST_MAX_GIVER_DISTANCE)  // a spot that far is no grind toward this giver
            break;
        if (pending && judgedSpots > pastStop &&
            Leader::RouteSearchesOnThread() - searchesBefore >= SWEEP_SEARCHES_PER_TICK)
        {
            std::lock_guard<std::mutex> guard(grindLock);
            sweepStop[bot->GetGUID().GetCounter()] = {level, bestQuest, judgedSpots};
            *pending = true;
            return bestQuest;
        }
        ++judgedSpots;
        if (!TripRefused(bot, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), "grind spot"))
        {
            grindPos = pos;
            found = true;
            break;
        }
        if (++refused >= GRIND_SPOT_TRIES)
            break;
    }
    if (pending)  // [perf214] the sweep ended: the next one starts paced from its first spot
    {
        std::lock_guard<std::mutex> guard(grindLock);
        sweepStop.erase(bot->GetGUID().GetCounter());
    }
    if (!found && refused)
    {
        std::lock_guard<std::mutex> guard(grindLock);
        allRefused[bot->GetGUID().GetCounter()] = {level, bestQuest};
        LOG_INFO("playerbots", "[grind42] {} L{} every grind spot toward q{}'s giver it looked at is refused ({}): not "
                 "judged again until its level or that giver changes",
                 bot->GetName(), bot->GetLevel(), bestQuest, refused);
    }
    return bestQuest;
}

void NewRpgBaseAction::LogDungeonCandidates()
{
    uint8 const level = bot->GetLevel();
    uint8 const expansion = bot->GetSession()->Expansion();
    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i);
        if (!dungeon || dungeon->TypeID != lfg::LFG_TYPE_DUNGEON || dungeon->Difficulty != DUNGEON_DIFFICULTY_NORMAL ||
            dungeon->ExpansionLevel > expansion || level < dungeon->MinLevel || level > dungeon->MaxLevel)
            continue;

        LOG_INFO("playerbots", "[rpg] {} L{} dungeon candidate: {} (map {}, levels {}-{})", bot->GetName(), level,
                 dungeon->Name[0], dungeon->MapID, dungeon->MinLevel, dungeon->MaxLevel);
    }
}

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    std::vector<NewRpgStatus> availableStatus;
    uint32 probSum = 0;
    for (NewRpgStatus status : candidateStatus)
    {
        if (sPlayerbotAIConfig.RpgStatusProbWeight[status] == 0)
            continue;

        if (CheckRpgStatusAvailable(status))
        {
            availableStatus.push_back(status);
            probSum += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        }
    }
    // Safety check. Default to "rest" if all RPG weights = 0
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    for (NewRpgStatus status : availableStatus)
    {
        accumulate += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        if (accumulate >= rand)
        {
            chosenStatus = status;
            break;
        }
    }

    switch (chosenStatus)
    {
        case RPG_WANDER_RANDOM:
        {
            botAI->rpgInfo.ChangeToWanderRandom();
            return true;
        }
        case RPG_WANDER_NPC:
        {
            botAI->rpgInfo.ChangeToWanderNpc();
            return true;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                return true;
            }
            return false;
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoCamp(pos);
                return true;
            }
            return false;
        }
        case RPG_DO_QUEST:
        {
            WorldPosition giverPos;
            uint32 questId = ChooseBestQuest(giverPos);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest)
                return false;

            botAI->rpgInfo.ChangeToDoQuest(questId, quest, giverPos);
            return true;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
            {
                botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                return true;
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            botAI->rpgInfo.ChangeToDoGather();
            return true;
        }
        case RPG_IDLE:
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        case RPG_REST:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return true;
        }
        default:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            if (!sRandomPlayerbotMgr.IsRandomBot(bot))
            {
                WorldPosition giverPos;
                return ChooseBestQuest(giverPos) != 0;
            }

            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
                return false;

            // No point farming with (nearly) full bags - the loot pipeline
            // can't store the harvest (see StoreLootAction).
            if (AI_VALUE(uint8, "bag space") > 80)
                return false;

            return sGatherNodeMgr.HasUsableNodes(bot);
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}

// [guild12] the shared quest indexes, built at server startup (PlayerbotsWorldScript::OnStartup)
void NewRpgBaseAction::BuildQuestIndexes()
{
    GetQuestSpawnIndex();
    GetQuestStartPoints();
    EnderNeedsScript(0);  // [guild14]
    NewRpgGiverTrigger::Build();  // [giver15]
    NewRpgQuestUse::Build();  // [questitems]
}
