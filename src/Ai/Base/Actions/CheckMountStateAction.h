/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHECKMOUNTSTATEACTION_H
#define PLAYERBOTS_CHECKMOUNTSTATEACTION_H

#include "Trigger.h"
#include "UseItemAction.h"
#include <unordered_map>
#include <vector>

const uint16 SPELL_TRAVEL_FORM = 783;
const uint16 SPELL_FLIGHT_FORM = 33943;
const uint16 SPELL_SWIFT_FLIGHT_FORM = 40120;

struct MountData
{
    bool swiftMount = false;
    // Outer map: index (0 for ground, 1 for flight), inner map: effect speed -> vector of spell IDs.
    std::map<uint32, std::map<int32, std::vector<uint32>>> allSpells;
    // Default mount speed.
    int32 maxSpeed = 59;
};

struct PreferredMountCache
{
    std::vector<uint32> groundMounts;
    std::vector<uint32> flightMounts;
};

class PlayerbotAI;
class SpellInfo;

class CheckMountStateAction : public UseItemAction
{
public:
    CheckMountStateAction(PlayerbotAI* botAI) : UseItemAction(botAI, "check mount state", true) {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override { return true; }
    bool Mount();

    static void CompleteDismount(Player* bot);
    static void LoadPreferredMounts();
    // The one whose mount state this bot copies: its master, else the leader of its group; never the bot itself.
    static Player* GetMountLeader(PlayerbotAI* botAI);
    static bool IsCastingMount(Player* player);
    static bool IsMountedOrMounting(Player* player);
    // [mount7] the bot is near enough its mount leader to copy its getting off: the way left is too short for a
    // mount to pay (GuildRiding::MountPaysFromYd); no leader on its map: true
    static bool RidesWithLeader(Player* bot, Player* leader);
    // [mount9] THE DISMOUNT BAND AND THE MOUNT ITSELF, PUBLISHED SO THAT NOBODY KEEPS A SECOND COPY. The band
    // is this action's own quantity ("a target in reach"); the journal's mount watch asks for it here instead
    // of repeating the expression, which is rule 5 - one quantity, one judge - applied to an instrument.
    static float DismountBandYd(Player* bot);
    static SpellInfo const* GetMountSpell(Player* player);

private:
    Player* master;
    ShapeshiftForm masterInShapeshiftForm;
    ShapeshiftForm botInShapeshiftForm;
    static std::unordered_map<uint32, PreferredMountCache> mountCache;
    float CalculateDismountDistance() const;
    float CalculateMountDistance() const;
    bool Dismount(char const* why);  // false: in the air, stays mounted; `why` for the walk ([mount7])
    void ClearStaleFlightFlags();
    bool ShouldFollowMasterMountState(Player* master, bool noAttackers, bool shouldMount) const;
    bool ShouldDismountForMaster(Player* master) const;
    int32 CalculateMasterMountSpeed(Player* master) const;
    bool CheckForSwiftMount() const;
    std::map<uint32, std::map<int32, std::vector<uint32>>> GetAllMountSpells() const;
    bool TryForms(Player* master, int32 masterMountType, int32 masterSpeed) const;
    bool TryPreferredMount(Player* master) const;
    uint32 GetMountType(Player* master) const;
    bool TryRandomMountFiltered(std::map<int32, std::vector<uint32>> const& spells, int32 masterSpeed) const;
};

// The leader's mount state differs from ours (it mounts, starts casting a mount, or dismounts): copy it now, not on
// the next idle tick, so the group rides off and stops together.
class LeaderMountStateTrigger : public Trigger
{
public:
    LeaderMountStateTrigger(PlayerbotAI* botAI) : Trigger(botAI, "leader mount state", 500) {}

    bool IsActive() override;
};

#endif
