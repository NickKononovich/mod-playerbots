/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PARTYSTRENGTHVALUE_H
#define PLAYERBOTS_PARTYSTRENGTHVALUE_H

#include "Value.h"

#include <ctime>

class Player;
class PlayerbotAI;
class UntypedValue;

// How strong the party is for the content it stands in, per role, the way a real group sizes itself up: a tank
// in gear below the instance plays it safe, a group that outgears the place chain pulls, and dps that outgear
// their tank leave it time to build threat. 1.0 is a group geared for the content; every factor is clamped.
struct PartyStrength
{
    float tank = 1.0f;     // tanks' gear for the content, and their health against the rest of the group
    float healer = 1.0f;   // healers' gear for the content
    float dps = 1.0f;      // damage dealers' gear for the content
    float overall = 1.0f;  // role-weighted mean of the above
    bool dpsOutgearsTank = false;  // dps clearly better geared than the tank, or one of them nearly pulled aggro
    bool tankLacksAreaThreat = false;  // a levelling tank without Consecration, Thunder Clap, Swipe or Death and Decay
    bool measured = false;         // false: no group, not in a dungeon or nobody to compare: everything stays 1.0

    // Size of pack the group can take, relative to a group geared for the content: the weaker of the pair that
    // keeps everyone alive (tank, healer) caps it, overall strength raises it.
    float PullScale() const;

    // The dps should give the tank extra time and margin on threat.
    bool CarefulThreat() const;
};

// What a tank can hold at once with the spells it actually knows: a mob or two by hand, one more with a taunt, and
// area threat (Consecration, Thunder Clap, Swipe, Death and Decay ...) for the rest of a pack.
struct TankKit
{
    float holdMobs = 2.0f;
    bool areaThreat = false;
};

namespace PartyStrengthShared
{
    // The tank's kit from its known spells; a player without bot AI counts as fully equipped.
    TankKit GetTankKit(Player* tank);

    // The group's strength, computed once per group and map instance and shared by every member asking within
    // the refresh window. A bot outside a group gets the neutral default.
    PartyStrength Get(PlayerbotAI* botAI);

    // A dps of `bot`'s group came close to pulling aggro: the group counts as dps-outgeared for a while.
    void NoteThreatScare(Player* bot);
}

class PartyStrengthValue : public CalculatedValue<PartyStrength>
{
public:
    PartyStrengthValue(PlayerbotAI* botAI);

    PartyStrength Calculate() override;
    std::string const Format() override;
};

// Wall-clock second this bot first looked at the current fight, or 0 out of combat. Unlike "combat start time"
// it does not depend on the "wait for attack" strategy, so the trash pull grace works for every bot.
class PullGraceStartValue : public CalculatedValue<time_t>
{
public:
    PullGraceStartValue(PlayerbotAI* botAI);

    time_t Calculate() override;
    // [b32_engage] getMSTime() of the same start (0 out of combat), for waits shorter than a whole second.
    uint32 StartMs() const { return _start ? _startMs : 0; }

private:
    time_t _start = 0;
    time_t _lastSeen = 0;
    uint32 _startMs = 0;
};

UntypedValue* CreatePartyStrengthValue(PlayerbotAI* botAI);
UntypedValue* CreatePullGraceStartValue(PlayerbotAI* botAI);

#endif
