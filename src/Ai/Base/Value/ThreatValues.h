/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_THREATVALUES_H
#define PLAYERBOTS_THREATVALUES_H

#include "NamedObjectContext.h"
#include "Value.h"

class Player;
class PlayerbotAI;
class Unit;

class ThreatValue : public Uint8CalculatedValue, public Qualified
{
public:
    ThreatValue(PlayerbotAI* botAI, std::string const name = "threat") : Uint8CalculatedValue(botAI, name) {}

    uint8 Calculate() override;

protected:
    uint8 Calculate(Unit* target);
};

// The threat the bot's next cast adds to its target: what a damage dealer of its level and gear deals in
// NEXT_CAST_SECONDS (EstimatedGroupDpsValue's per-player estimate), the "one more cast" a player reading Omen
// keeps in hand before it hits. Cached for 10 s: level and gear do not change mid-fight.
class NextCastThreatValue : public FloatCalculatedValue
{
public:
    static constexpr float NEXT_CAST_SECONDS = 2.5f;

    NextCastThreatValue(PlayerbotAI* botAI) : FloatCalculatedValue(botAI, "next cast threat", 10 * IN_MILLISECONDS) {}

    float Calculate() override;
};

// The threat lead a damage dealer reads before and while it fights the tank's mob ([threat-open], [threat-hold]).
// The data is the mob's own ThreatManager (what Omen shows a player); the pull-over points are the core's: a unit
// takes the mob from its victim at 110 % of the victim's threat in melee range of it, 130 % out of it.
namespace ThreatLead
{
    constexpr float MELEE_PULL_OVER = 1.1f;
    constexpr float RANGED_PULL_OVER = 1.3f;
    // A dps opens on a mob once its threat after the next cast stays under this share of the holder's threat.
    constexpr float OPEN_SHARE = 0.9f;
    // ... a tank soft for the content or outgeared by its dps (PartyStrength::CarefulThreat) gets a wider lead.
    constexpr float CAREFUL_OPEN_SHARE = 0.7f;
    // A dps keeps hitting until its threat after the next cast reaches this share of its pull-over point.
    constexpr float HOLD_SHARE = 0.9f;
    constexpr float CAREFUL_HOLD_SHARE = 0.75f;

    // Threat of the player holding the mob (its victim, when that is the bot's group), else the highest of the
    // group's living tanks; 0 without one.
    // [b44_hos] A mob on a friendly NPC (NpcHolder): the NPC's own threat on it when it has some; HOLD_UNBOUNDED when
    // the NPC keeps it whatever threat the bot builds (NpcHoldsRegardless).
    float HolderThreat(Player* bot, Unit* target);

    // [b44_hos] HolderThreat of a mob no threat of the bot can take from its holder: every share against it is ~0.
    constexpr float HOLD_UNBOUNDED = 3.0e38f;

    // [b44_hos] The mob's victim when it is an NPC on the bot's side (an escort, Brann at the Tribunal of Ages, a
    // scripted ally): not a player, not a player's pet, guardian or totem, not hostile to the bot. nullptr otherwise.
    Unit* NpcHolder(Player* bot, Unit* target);

    // [b44_hos] The mob stays on its NPC holder whatever threat the bot builds: the holder's threat reference on it is
    // taunting (SPELL_AURA_MOD_TAUNT; ThreatManager::CompareReferencesLT ranks a taunting reference above every other
    // regardless of threat, and Brann's Tribunal taunt 51775 never expires) or the mob is fixated on it.
    bool NpcHoldsRegardless(Player* bot, Unit* target);

    // (the bot's threat + its next cast) / HolderThreat; -1 when there is no holder threat to compare with.
    float ProjectedShare(PlayerbotAI* botAI, Unit* target);

    // [rot1] The bot's threat on the mob in percent of the highest living tank's (0-255; 100 when the mob fights and no
    // tank has threat on it yet, 0 for a fleeing mob or one a friendly NPC keeps). ThreatValue's per-unit share.
    uint8 TankShare(Player* bot, Unit* target);

    // MELEE_PULL_OVER when the bot is in melee range of the mob, else RANGED_PULL_OVER.
    float PullOverPoint(Player* bot, Unit* target);

    // The holder has threat on the mob and the bot's next cast keeps it under the open share.
    bool SafeToOpen(PlayerbotAI* botAI, Unit* target, bool careful);
}

class NeglectThreatResetValue : public ManualSetValue<bool>
{
public:
    NeglectThreatResetValue(PlayerbotAI* botAI, bool defaultValue = false, std::string name = "neglect threat")
        : ManualSetValue<bool>(botAI, defaultValue, name)
    {
    }
    virtual bool Get()
    {
        bool ret = value;
        Reset();
        return ret;
    }
};

#endif
