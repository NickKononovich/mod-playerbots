/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CheckMountStateAction.h"
#include "PlayerbotsDatabase.h"
#include "AreaDefines.h"
#include "BattleGroundTactics.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "DBCStores.h"
#include "Event.h"
#include "GuildRiding.h"   // [mount7] the mount's pay distance, the dismount reason
#include "LeaderOrders.h"  // [mount_led] Leader::Orders::IsTracked
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Spell.h"
#include "SpellAuraEffects.h"

static constexpr uint32 SPELL_COLD_WEATHER_FLYING = 54197;
static constexpr float PARACHUTE_LAND_THRESHOLD = 15.0f;

// Preferred mounts per bot, loaded once at startup by LoadPreferredMounts()
std::unordered_map<uint32, PreferredMountCache> CheckMountStateAction::mountCache;

MountData CollectMountData(Player const* bot)
{
    MountData data;
    uint32 const ridingSkill = bot->GetPureSkillValue(SKILL_RIDING);
    for (auto& entry : bot->GetSpellMap())
    {
        uint32 spellId = entry.first;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
            continue;

        if (entry.second->State == PLAYERSPELL_REMOVED || !entry.second->Active || spellInfo->IsPassive())
            continue;

        // The core casts a known mount without checking Riding.
        if (PlayerbotSpellRepository::Instance().GetRequiredRidingSkill(spellInfo) > ridingSkill)
            continue;

        int32 effect1 = spellInfo->Effects[1].BasePoints;
        int32 effect2 = spellInfo->Effects[2].BasePoints;

        int32 speed = std::max(effect1, effect2);

        // Update max speed if appropriate.
        if (speed > data.maxSpeed)
            data.maxSpeed = speed;  // In BG, clamp max speed to 99 later; here we just store the maximum found.

        // Determine index: flight if either effect has flight aura or specific mount ID.
        uint32 index = (spellInfo->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                        spellInfo->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                        // Winged Steed of the Ebon Blade
                        // This mount is meant to autoscale from a 150% flyer
                        // up to a 280% as you train your flying skill up.
                        // This incorrectly gets categorised as a ground mount, force this to flyer only.
                        // TODO: Add other scaling mounts here if they have the same issue, or adjust above
                        // checks so that they are all correctly detected.
                        spellInfo->Id == 54729) ? 1 : 0;
        data.allSpells[index][speed].push_back(spellId);
    }
    return data;
}

bool CheckMountStateAction::Execute(Event /*event*/)
{
    // Forced flight dismount:
    // Bots get stale flight movement flags after a forced dismount (e.g: Dalaran) because the post landing dismount cleanup
    // needs MSG_MOVE_FALL_LAND (a client opcode) and client movement packets. The stale flags cause the bot to be stuck with
    // the parachute, or even keep the bot hovering indefinitely and block MMAP routing.
    // Note: PlayerbotAI::UpdateFall stands in for MSG_MOVE_FALL_LAND and applies the fall damage on landing, so the
    // parachute (feather fall) is kept while the fall spline runs and only dropped near the floor afterwards.
    if (bot->HasFeatherFallAura() && !bot->Unit::IsFalling())
    {
        float floorZ = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (floorZ != INVALID_HEIGHT && floorZ != VMAP_INVALID_HEIGHT_VALUE &&
            bot->GetPositionZ() - floorZ <= PARACHUTE_LAND_THRESHOLD)
            bot->RemoveAurasByType(SPELL_AURA_FEATHER_FALL);
    }
    ClearStaleFlightFlags();

    // Determine if there are no attackers
    bool noAttackers = !AI_VALUE2(bool, "combat", "self target") || !AI_VALUE(uint8, "attacker count");
    bool enemy = AI_VALUE(Unit*, "enemy player target");
    bool dps = AI_VALUE(Unit*, "dps target");
    bool shouldDismount = false;
    bool shouldMount = false;

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (currentTarget)
    {
        float dismountDistance = CalculateDismountDistance();
        float mountDistance = CalculateMountDistance();
        float combatReach = bot->GetCombatReach() + currentTarget->GetCombatReach();
        float distanceToTarget = bot->GetExactDist(currentTarget);

        shouldDismount = (distanceToTarget <= dismountDistance + combatReach);
        shouldMount = (distanceToTarget > mountDistance + combatReach);
    }
    else
    {
        // [mount9] A BOT STANDING STILL HAS NO RIDE TO TAKE, so having no target is not a reason to get on. This
        // is NOT a new rule and not a new threshold: it is the rule this very file already states and already
        // ships for a led run eighty lines down ("A led tank standing still has no ride to take: it stays off the
        // mount (and gets off), and mounts only for a walk (it is moving)"), and until now it was applied to a
        // tracked run only. Everywhere else "no target" meant "mount", and a standing bot was mounted on every
        // tick it had nothing to attack.
        //
        // THAT IS WHAT THE USER SAW IN THE GAME ("they just stand there and keep getting off the mount and back
        // on"), and it is two judges on one quantity - rule 5. Measured on b504: 915 standing windows over 23
        // bots whose engine trace shows this action EXECUTING, 823 of them with no group at all, and whatever the
        // bot was really doing in town (a craft, an enchant, a vendor) is a cast that takes the mount away again -
        // Emdyl held ONE point in Stormwind to four decimals for 95.4 minutes that way. Nothing here mounts for a
        // walk: that is MountForWalk's judgement (NewRpgBaseAction, [mount5]), and it stays the only one.
        //
        // No elapsed time is consulted: `isMoving()` is a fact of this instant, which is what rule 2 requires.
        shouldMount = bot->isMoving();
    }

    // If should dismount, or master (if any) is no longer in travel form, yet bot still is, remove the shapeshifts
    if (shouldDismount ||
        (masterInShapeshiftForm != FORM_TRAVEL && botInShapeshiftForm == FORM_TRAVEL) ||
        (masterInShapeshiftForm != FORM_FLIGHT && botInShapeshiftForm == FORM_FLIGHT && master && !master->IsMounted()) ||
        (masterInShapeshiftForm != FORM_FLIGHT_EPIC && botInShapeshiftForm == FORM_FLIGHT_EPIC && master && !master->IsMounted()))
        botAI->RemoveShapeshift();

    if (shouldDismount && bot->IsMounted())
    {
        return Dismount("a fight: its target in reach");
    }

    bool inBattleground = bot->InBattleground();
    bool const noRealMaster = (!master || master == bot);

    // If there is a master and bot not in BG, follow master's mount state regardless of group leader
    if (!noRealMaster && !inBattleground)
    {
        if (ShouldFollowMasterMountState(master, noAttackers, shouldMount))
            return Mount();

        else if (ShouldDismountForMaster(master) && bot->IsMounted())
        {
            return Dismount("its leader got off next to it");
        }

        return false;
    }

    // [mount_led] b342 wb-kazzak 25A-3 / bt-h 25A-2: the led run's tank (the harness or a guild leader leads it, Orders
    // tracked) had no target while the group buffed at the start, so "no target" mounted it outdoors (the open world,
    // Black Temple's courtyard); the members follow their mount leader's state, so all 25 sat mounted, every buff,
    // food and conjure failed NOT_MOUNTED and the buff step waited for 4 minutes. A led tank standing still has no
    // ride to take: it stays off the mount (and gets off), and mounts only for a walk (it is moving)
    if (noRealMaster && !inBattleground && Leader::Orders::IsTracked(bot->GetGUID()) && !bot->isMoving())
    {
        if (bot->IsMounted() && !bot->IsFlying())
            return Dismount("a led run standing");
        return false;
    }

    // No real master (random bot or SelfBot) OR bot in BG
    if ((noRealMaster || inBattleground) && !bot->IsMounted() &&
        noAttackers && shouldMount && !bot->IsInCombat())
        return Mount();

    if (!bot->IsFlying() && shouldDismount && bot->IsMounted() &&
        (enemy || dps || (!noAttackers && bot->IsInCombat())))
    {
        return Dismount("a fight");
    }

    return false;
}

bool CheckMountStateAction::isUseful()
{
    // Not useful when:
    if (botAI->IsInVehicle() || bot->isDead() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) ||
        !bot->IsOutdoors() || bot->InArena())
        return false;

    master = GetMountLeader(botAI);

    // Get shapeshift states, only applicable when there's a master; without one nothing is copied.
    botInShapeshiftForm = bot->GetShapeshiftForm();
    masterInShapeshiftForm = master ? master->GetShapeshiftForm() : botInShapeshiftForm;

    // Not useful when in combat and not currently mounted / travel formed
    if ((bot->IsInCombat() || botAI->GetState() == BOT_STATE_COMBAT) &&
        !bot->IsMounted() && botInShapeshiftForm != FORM_TRAVEL && botInShapeshiftForm != FORM_FLIGHT && botInShapeshiftForm != FORM_FLIGHT_EPIC)
        return false;

    // In addition to checking IsOutdoors, also check whether bot is clipping below floor slightly because that will
    // cause bot to falsly indicate they are outdoors. This fixes bug where bot tries to mount indoors (which seems
    // to mostly be an issue in tunnels of WSG and AV)
    float posZ = bot->GetPositionZ();
    float groundLevel = bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), posZ);
    if (!bot->IsMounted() && !bot->HasWaterWalkAura() && posZ < groundLevel)
        return false;

    // Not useful when bot does not have mount strat and is not currently mounted
    if (!GET_PLAYERBOT_AI(bot)->HasStrategy("mount", BOT_STATE_NON_COMBAT) && !bot->IsMounted())
        return false;

    // [noattack] The level and form gates are for mounting. A mounted bot below the mount level rides a quest mount
    // (The Kessel Run's Kessel's Elekk, 31973, SPELL_AURA_MOUNTED) and must still get off to fight: mounted,
    // Unit::Attack refuses every attack. b338 Farret L11 retribution, Azuremyst: five fights with the mob 1.5-3.9 yd
    // away, "no victim, no cast", fled; b337 Rumanatol L11-12 and Laaral L13 on the same quest, 38-111 s of melee
    // dealing 0. The "mounted" trigger (54) fired, this gate made the action useless.
    if (!bot->IsMounted())
    {
        // Not useful when level lower than minimum required
        if (bot->GetLevel() < sPlayerbotAIConfig.UseGroundMountAtMinLevel)
            return false;

        // Allow mounting while transformed only if the form allows it
        if (bot->HasAuraType(SPELL_AURA_TRANSFORM) && bot->IsInDisallowedMountForm())
            return false;
    }

    // Mount attempts stop movement, including active jumps.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE && bot->movespline &&
        !bot->movespline->Finalized())
        return false;

    // BG Logic
    if (bot->InBattleground())
    {
        // Do not use when carrying BG Flags
        if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) || bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
            return false;

        // Only mount if BG starts in less than 30 sec
        if (Battleground* bg = bot->GetBattleground())
            if (bg->GetStatus() == STATUS_WAIT_JOIN && bg->GetStartDelayTime() > BG_START_DELAY_30S)
                return false;
    }

    return true;
}

bool CheckMountStateAction::Mount()
{
    // Remove current Shapeshift if need be
    if (botInShapeshiftForm != FORM_TRAVEL &&
        botInShapeshiftForm != FORM_FLIGHT &&
        botInShapeshiftForm != FORM_FLIGHT_EPIC)
    {
        botAI->RemoveShapeshift();
        botAI->RemoveAura("tree of life");
    }

    if (TryPreferredMount(master))
        return true;

    // Get bot mount data
    MountData mountData = CollectMountData(bot);
    int32 masterMountType = GetMountType(master);
    int32 masterSpeed = CalculateMasterMountSpeed(master);

    // Try shapeshift
    if (TryForms(master, masterMountType, masterSpeed))
        return true;

    // Try random mount
    auto spellsIt = mountData.allSpells.find(masterMountType);
    if (spellsIt != mountData.allSpells.end())
    {
        auto& spells = spellsIt->second;
        if (TryRandomMountFiltered(spells, masterSpeed))
            return true;
    }

    // The leader flies but we cannot (no flying riding, or no flying mount): ride along on the ground instead.
    if (masterMountType == 1)
    {
        auto groundIt = mountData.allSpells.find(0);
        if (groundIt != mountData.allSpells.end() && TryRandomMountFiltered(groundIt->second, 0))
            return true;
    }

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "mount");
    if (!items.empty())
        return UseItemAuto(*items.begin());

    return false;
}

bool CheckMountStateAction::Dismount(char const* why)
{
    // Nobody leaves the saddle in mid-air: a player would fall. Stay mounted until the mount is over solid ground.
    if (!PlayerbotAI::HasGroundSupport(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
        return false;
    GuildRiding::NoteDismount(bot, why);  // [mount7]

    if (bot->isMoving())
        bot->StopMoving();

    WorldPacket emptyPacket;
    bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);

    ClearStaleFlightFlags();
    return true;
}

void CheckMountStateAction::ClearStaleFlightFlags()
{
    if (bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura())
        return;

    if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY))
    {
        bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_CAN_FLY);
        if (!bot->IsRooted())
            bot->SendMovementFlagUpdate();
    }
}

void CheckMountStateAction::CompleteDismount(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return;

    // A dismount in the air makes a client fall; the bot falls on its next tick and takes the damage where it
    // lands (PlayerbotAI::UpdateFall). Applying the whole drop at once here killed bots in mid-air (Oculus wave 6:
    // corpses hanging at z 342, revived there, killed again by the next dismount). Deferred also because this runs
    // inside Unit::_EnterVehicle (Dismount before the seat is taken), where no fall may start.
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        botAI->RequestFallCheck();
}

bool CheckMountStateAction::TryForms(Player* master, int32 masterMountType, int32 masterSpeed) const
{
    if (!master)
        return false;

    // If both master and bot are in matching forms or master is mounted with corresponding speed, nothing to do
    else if
        ((masterInShapeshiftForm == FORM_TRAVEL && botInShapeshiftForm == FORM_TRAVEL) ||
        ((masterInShapeshiftForm == FORM_FLIGHT || (masterMountType == 1 && masterSpeed == 149)) && botInShapeshiftForm == FORM_FLIGHT) ||
        ((masterInShapeshiftForm == FORM_FLIGHT_EPIC || (masterMountType == 1 && masterSpeed >= 279)) && botInShapeshiftForm == FORM_FLIGHT_EPIC))
        return true;

    // Check if master is in Travel Form and bot can do the same
    if (botAI->CanCastSpell(SPELL_TRAVEL_FORM, bot, true) &&
        masterInShapeshiftForm == FORM_TRAVEL && botInShapeshiftForm != FORM_TRAVEL)
    {
        botAI->CastSpell(SPELL_TRAVEL_FORM, bot);
        return true;
    }

    // Check if master is in Flight Form or has a flying mount and bot can flight form
    if (botAI->CanCastSpell(SPELL_FLIGHT_FORM, bot, true) &&
        ((masterInShapeshiftForm == FORM_FLIGHT && botInShapeshiftForm != FORM_FLIGHT) ||
        (masterMountType == 1 && masterSpeed == 149)))
    {
        botAI->CastSpell(SPELL_FLIGHT_FORM, bot);  // the form's own aura sets the flight speed
        return true;
    }

    // Check if master is in Swift Flight Form or has an epic flying mount and bot can swift flight form
    if (botAI->CanCastSpell(SPELL_SWIFT_FLIGHT_FORM, bot, true) &&
        ((masterInShapeshiftForm == FORM_FLIGHT_EPIC && botInShapeshiftForm != FORM_FLIGHT_EPIC) ||
        (masterMountType == 1 && masterSpeed >= 279)))
    {
        botAI->CastSpell(SPELL_SWIFT_FLIGHT_FORM, bot);  // the form's own aura sets the flight speed
        return true;
    }

    return false;
}

void CheckMountStateAction::LoadPreferredMounts()
{
    mountCache.clear();

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_PREFERRED_MOUNTS);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return;

    uint32 totalResults = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 guid = fields[0].Get<uint32>();
        uint32 spellId = fields[1].Get<uint32>();
        uint32 mountType = fields[2].Get<uint32>();

        if (mountType == 0)
            mountCache[guid].groundMounts.push_back(spellId);
        else if (mountType == 1)
            mountCache[guid].flightMounts.push_back(spellId);

        totalResults++;
    } while (result->NextRow());

    LOG_INFO("playerbots", "Preferred mounts initialized | Total records: {}", totalResults);
}

bool CheckMountStateAction::TryPreferredMount(Player* master) const
{
    uint32 botGUID = bot->GetGUID().GetRawValue();

    // mountCache is filled once on the world thread at startup and only read afterwards
    auto itr = mountCache.find(botGUID);
    if (itr == mountCache.end())
        return false;

    PreferredMountCache const& preferred = itr->second;

    // Pick a random preferred mount from the selection, if available
    uint32 chosenMountId = 0;

    if (GetMountType(master) == 0 && !preferred.groundMounts.empty())
    {
        uint32 index = urand(0, preferred.groundMounts.size() - 1);
        chosenMountId = preferred.groundMounts[index];
    }

    else if (GetMountType(master) == 1 && !preferred.flightMounts.empty())
    {
        uint32 index = urand(0, preferred.flightMounts.size() - 1);
        chosenMountId = preferred.flightMounts[index];
    }

    // No suitable preferred mount found
    if (chosenMountId == 0)
        return false;

    // Check if spell exists
    if (!sSpellMgr->GetSpellInfo(chosenMountId))
    {
        LOG_ERROR("playerbots", "Preferred mount failed: Invalid spell {} | Bot Guid: {}", chosenMountId, botGUID);
        return false;
    }

    // Required here as otherwise bots won't mount in BG's due to them constant moving
    if (bot->isMoving())
        bot->StopMoving();

    // Check if spell can be cast - for now allow all, even if the bot does not have the actual mount
    //if (botAI->CanCastSpell(mountId, botAI->GetBot()))
    //{
    botAI->CastSpell(chosenMountId, botAI->GetBot());
    return true;
    //}

    LOG_DEBUG("playerbots", "Preferred mount failed! | Bot Guid: {}", botGUID);
    return false;
}

bool CheckMountStateAction::TryRandomMountFiltered(std::map<int32, std::vector<uint32>> const& spells, int32 masterSpeed) const
{
    for (auto it = spells.rbegin(); it != spells.rend(); ++it)
    {
        int32 currentSpeed = it->first;

        if ((masterSpeed > 59 && currentSpeed < 99) || (masterSpeed > 149 && currentSpeed < 279))
            continue;

        // Pick a random mount from the candidate group.
        auto const& ids = it->second;
        if (!ids.empty())
        {
            // Required here as otherwise bots won't mount in BGs due to them constant moving
            if (bot->isMoving())
                bot->StopMoving();

            uint32 index = urand(0, ids.size() - 1);

            if (botAI->CanCastSpell(ids[index], bot))
            {
                botAI->CastSpell(ids[index], bot);
                return true;
            }
        }
    }
    return false;
}

float CheckMountStateAction::CalculateDismountDistance() const { return DismountBandYd(bot); }

// [mount9] the same expression, with the bot as its only input, so the journal's mount watch can READ this
// quantity instead of keeping a second copy of it
float CheckMountStateAction::DismountBandYd(Player* bot)
{
    // Warrior bots should dismount far enough to charge (because it's important for generating some initial rage),
    // a real player would be riding toward enemy mashing the charge key but the bots won't cast charge while mounted.
    bool isMelee = PlayerbotAI::IsMelee(bot);
    float dismountDistance = isMelee ? sPlayerbotAIConfig.MeleeDistance + 2.0f : sPlayerbotAIConfig.SpellDistance + 2.0f;
    return bot->getClass() == CLASS_WARRIOR ? std::max(18.0f, dismountDistance) : dismountDistance;
}

float CheckMountStateAction::CalculateMountDistance() const
{
    // Mount distance should be >= 21 regardless of class, because when travelling a distance < 21 it takes longer
    // to cast mount-spell than the time saved from the speed increase. At a distance of 21 both approaches take 3
    // seconds:
    // 21 / 7  =  21 / 14 + 1.5  =  3   (7 = dismounted speed  14 = epic-mount speed  1.5 = mount-spell cast time)
    bool isMelee = PlayerbotAI::IsMelee(bot);
    float baseDistance = isMelee ? sPlayerbotAIConfig.MeleeDistance + 10.0f : sPlayerbotAIConfig.SpellDistance + 10.0f;
    return std::max(21.0f, baseDistance);
}

bool CheckMountStateAction::ShouldFollowMasterMountState(Player* master, bool noAttackers, bool shouldMount) const
{
    // A leader who has started the mount cast counts as mounted, so we cast together instead of after it.
    bool isMasterMounted = master->IsMounted() || (masterInShapeshiftForm == FORM_FLIGHT ||
                                                    masterInShapeshiftForm == FORM_FLIGHT_EPIC ||
                                                    masterInShapeshiftForm == FORM_TRAVEL) ||
                           IsCastingMount(master);
    bool isBotMountedOrForm = bot->IsMounted() || botInShapeshiftForm == FORM_FLIGHT ||
                              botInShapeshiftForm == FORM_FLIGHT_EPIC || botInShapeshiftForm == FORM_TRAVEL ||
                              IsCastingMount(bot);
    return isMasterMounted && !isBotMountedOrForm && noAttackers &&
        shouldMount && !bot->IsInCombat() && botAI->GetState() != BOT_STATE_COMBAT;
}

bool CheckMountStateAction::ShouldDismountForMaster(Player* master) const
{
    // [mount7] a member walking to its leader from afar rides on: the leader's state is copied once the rest of the
    // way is too short for a mount to pay (b470 Maronic, PARTY to Ginde 1850 yd off: got off every 70-120 yd and
    // mounted again, 30 casts)
    if (bot->IsMounted() && !RidesWithLeader(bot, master))
        return false;
    bool isMasterMounted = master->IsMounted() || (masterInShapeshiftForm == FORM_FLIGHT ||
                                                   masterInShapeshiftForm == FORM_FLIGHT_EPIC ||
                                                   masterInShapeshiftForm == FORM_TRAVEL) ||
                           IsCastingMount(master);
    return !isMasterMounted && bot->IsMounted();
}

static bool BotCanUseFlyingMount(Player const* bot)
{
    if (bot->GetPureSkillValue(SKILL_RIDING) < 225)
        return false;

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
    if (!area || !area->IsFlyable())
        return false;
    if (area->flags & AREA_FLAG_NO_FLY_ZONE)
        return false;

    uint32 const vmap = GetVirtualMapForMapAndZone(bot->GetMapId(), bot->GetZoneId());
    if (vmap == MAP_NORTHREND && !bot->HasSpell(SPELL_COLD_WEATHER_FLYING))
        return false;

    return true;
}

int32 CheckMountStateAction::CalculateMasterMountSpeed(Player* master) const
{
    // Check riding skill and level requirements
    int32 ridingSkill = bot->GetPureSkillValue(SKILL_RIDING);
    int32 botLevel = bot->GetLevel();

    if (ridingSkill <= 75 && botLevel < static_cast<int32>(sPlayerbotAIConfig.UseFastGroundMountAtMinLevel))
        return 59;

    // check if bot has master and if master is self
    bool const noRealMaster = (!master || master == bot);

    if (!noRealMaster && !bot->InBattleground())
    {
        if (SpellInfo const* masterSpell = GetMountSpell(master))
        {
            int32 effect1 = masterSpell->Effects[1].BasePoints;
            int32 effect2 = masterSpell->Effects[2].BasePoints;
            return std::max(effect1, effect2);
        }
        else if (masterInShapeshiftForm == FORM_FLIGHT_EPIC)
            return 279;
        else if (masterInShapeshiftForm == FORM_FLIGHT)
            return 149;
        return 59;  // walk pace
    }

    // No real master OR battleground: pick speed by skill tier.
    if (!bot->InBattleground() && BotCanUseFlyingMount(bot))
        return (ridingSkill >= 300) ? 279 : 149;

    int32 maxGround = (ridingSkill >= 150) ? 99 : 59;
    if (bot->InBattleground() && maxGround > 99)
        maxGround = 99;
    return maxGround;
}

uint32 CheckMountStateAction::GetMountType(Player* master) const
{
    bool const noRealMaster = (!master || master == bot);

    if (noRealMaster)
        return (!bot->InBattleground() && BotCanUseFlyingMount(bot)) ? 1 : 0;

    if (SpellInfo const* masterSpell = GetMountSpell(master))
    {
        return (masterSpell->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                masterSpell->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) ? 1 : 0;
    }
    else if (masterInShapeshiftForm == FORM_FLIGHT || masterInShapeshiftForm == FORM_FLIGHT_EPIC)
        return 1;

    return 0;
}

SpellInfo const* CheckMountStateAction::GetMountSpell(Player* player)
{
    if (!player)
        return nullptr;

    Unit::AuraEffectList const& auraEffects = player->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    if (!auraEffects.empty())
        return auraEffects.front()->GetSpellInfo();

    // Still casting it: the cast tells which kind of mount (and how fast) the leader is about to ride.
    if (IsCastingMount(player))
        return player->GetCurrentSpell(CURRENT_GENERIC_SPELL)->m_spellInfo;

    return nullptr;
}

Player* CheckMountStateAction::GetMountLeader(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->InBattleground())
        return botAI->GetMaster();

    Player* leader = botAI->GetMaster();
    if (leader && leader != bot)
        return leader;

    // No master (a party without a client): ride with the group leader, unless we are the leader.
    Player* groupLeader = botAI->GetGroupLeader();
    if (groupLeader && groupLeader != bot && groupLeader->IsInWorld() && groupLeader->GetMapId() == bot->GetMapId())
        return groupLeader;

    return leader;
}

bool CheckMountStateAction::IsCastingMount(Player* player)
{
    if (!player)
        return false;

    Spell const* spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    return spell && spell->m_spellInfo && spell->m_spellInfo->HasAura(SPELL_AURA_MOUNTED);
}

bool CheckMountStateAction::IsMountedOrMounting(Player* player)
{
    if (!player)
        return false;

    ShapeshiftForm const form = player->GetShapeshiftForm();
    return player->IsMounted() || form == FORM_TRAVEL || form == FORM_FLIGHT || form == FORM_FLIGHT_EPIC ||
           IsCastingMount(player);
}

bool LeaderMountStateTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->InBattleground() || botAI->IsInVehicle() ||
        bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;

    Player* leader = CheckMountStateAction::GetMountLeader(botAI);
    if (!leader || leader == bot || !leader->IsInWorld() || leader->GetMapId() != bot->GetMapId())
        return false;

    bool const leaderRides = CheckMountStateAction::IsMountedOrMounting(leader);
    bool const botRides = CheckMountStateAction::IsMountedOrMounting(bot);
    if (!leaderRides && botRides && !CheckMountStateAction::RidesWithLeader(bot, leader))  // [mount7]
        return false;
    return leaderRides != botRides;
}

bool CheckMountStateAction::RidesWithLeader(Player* bot, Player* leader)
{
    if (!leader || leader == bot || leader->GetMapId() != bot->GetMapId())
        return true;
    return bot->GetExactDist(leader) <= GuildRiding::MountPaysFromYd(bot, GuildRiding::BestGroundMount(bot));
}
