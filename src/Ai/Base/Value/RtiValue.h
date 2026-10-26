/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RTIVALUE_H
#define PLAYERBOTS_RTIVALUE_H

#include "ObjectGuid.h"
#include "Value.h"

#include <string>

class Player;
class PlayerbotAI;
class SpellInfo;
class Unit;

// How a crowd control spell is used, and what keeps it from pulling the room or breaking.
enum CrowdControlFlag : uint8
{
    CC_FLAG_NONE = 0x00,
    CC_FLAG_FEAR = 0x01,        // the mob runs off (Fear, Scare Beast, Turn Evil): only with no idle mob near it
    CC_FLAG_PRE_PULL = 0x02,    // an unaware mob only (Sap): placed before the tank pulls, never cast in a fight
    CC_FLAG_MELEE_ONLY = 0x04,  // holds the mob in place but lets it cast (roots): only on a melee mob
    CC_FLAG_TRAP = 0x08,        // a trap at the caster's feet; Distracting Shot brings the mob onto it
    CC_FLAG_PET = 0x10,         // the pet casts it (the Succubus' Seduction)
};

struct CrowdControlOption
{
    char const* spell = nullptr;  // as the "spell id" value knows it, at the highest rank the bot has
    uint32 holdSpell = 0;         // the aura that holds the mob when the cast only places it (a trap); 0: the spell
    uint8 flags = CC_FLAG_NONE;
};

constexpr uint8 MAX_CROWD_CONTROL_OPTIONS = 4;

// The trash crowd control a class brings, preferred first, and the raid icon it answers to (warcraft.wiki.gg
// convention). Level, talent and creature type come from the spells themselves: a bot without the spell skips it.
struct CrowdControlRole
{
    char const* icon;
    CrowdControlOption options[MAX_CROWD_CONTROL_OPTIONS];  // a null spell ends the list
};

CrowdControlRole const* GetCrowdControlRole(uint8 classId);

// The spell that holds the mob for this option (the effect of a trap), or nullptr when the bot does not know it.
SpellInfo const* GetCrowdControlHoldInfo(PlayerbotAI* botAI, CrowdControlOption const& option);

// Every mechanic the hold spell applies (a root of Frost Nova sits on an effect, not on the spell).
uint64 GetCrowdControlMechanics(SpellInfo const* holdInfo);

// The bot knows the option (its pet does, for a pet spell) and it can hold this mob: creature type, immunities, a
// melee mob for a root, an unaware mob for Sap (`prePull`: out of combat, before the pull), no idle mob where a
// feared mob runs. Cooldown, range and line of sight are the cast's business.
bool CanCrowdControl(PlayerbotAI* botAI, CrowdControlOption const& option, Unit* target, bool prePull);

// The first option of the bot's class that can hold the mob, or nullptr.
CrowdControlOption const* FindCrowdControl(PlayerbotAI* botAI, Unit* target, bool prePull);

// The bot has crowd control it can use in a fight, or (`prePull`) also one it can only place before the pull.
bool KnowsCrowdControl(PlayerbotAI* botAI, bool prePull);

// The mechanics of every hold spell the bot knows: a root or a fear holds a mob without counting as breakable crowd
// control (IsCrowdControlled), so the bot's own mark checks these too.
uint64 GetKnownCrowdControlMechanics(PlayerbotAI* botAI);

// Breakable crowd control holds the mob, or one of `mechanics` does.
bool IsHeldByCrowdControl(Unit* unit, uint64 mechanics);

// An attacker held by breakable crowd control stands within `range` of `center`: an area spell there breaks it.
bool IsCrowdControlledMobNear(PlayerbotAI* botAI, Unit const* center, float range);

// No idle hostile mob within reach of where a feared mob runs (FleeingMovementGenerator: 28-43 yd from the caster)
// around `center`, line of sight or not, so the fear does not pull the next pack.
bool IsFearSafe(PlayerbotAI* botAI, Unit* center);

// The group kills this mob now or next: the Skull or Cross on it, the main tank's target, or any member attacking
// it. Crowd control never goes on it (the kill order and the crowd control come from one plan: AutoCcMarkAction
// takes the tail of KillOrder::SortKillOrder). `reason` names which rule it is.
bool IsGroupKillTarget(Player* bot, Unit* unit, char const** reason = nullptr);

// IsGroupKillTarget as a guard before a crowd control cast: true (refuse) and a log line, at most one per bot and
// target every few seconds.
bool RefuseCrowdControlOnKillTarget(PlayerbotAI* botAI, Unit* unit, std::string const& spell);

// The unit this bot marked with its own crowd control icon, and when.
struct AutoCcMark
{
    ObjectGuid target;
    uint32 markedAtMs = 0;
    uint32 lastControlledMs = 0;
    uint64 mechanics = 0;  // GetKnownCrowdControlMechanics when the mark went out
    uint32 fightMs = 0;    // first look at the mark in a fight: a mark put out before the pull ages from here
    // [rot_b31_cc] the current hold of the mob: when it was first seen held, and when it should run out (its aura's
    // remaining duration then; 0: unknown). Freed well before that, the crowd control broke (NoteCrowdControlBreak).
    uint32 heldSinceMs = 0;
    uint32 holdEndsMs = 0;
    bool broken = false;   // broke early: the mark goes (StaleAutoCcMarkTrigger), nothing casts on it meanwhile
};

// [rot_b31_cc] Polymorph loop (build 30: a Fire mage cast 111 Polymorphs in a level-80 wave, 22-25 in one pull of
// slabs-h, the group's own area damage breaking every sheep). A crowd control that broke early on a mob backs off:
// nobody of the group controls that mob again for CC_BREAK_BACKOFF_MS, and after CC_BREAKS_PER_PULL breaks not for
// the rest of its fight. Shared by every bot (keyed by the mob), read and written under a lock.
void NoteCrowdControlBreak(Unit* unit, uint32 heldMs);
bool IsCrowdControlBackedOff(Unit* unit);

// [rot_b31_cc] Breakable crowd control on `unit` would not last: a damage over time ticks on it, it stands inside a
// ground area spell of the group (Consecration, Death and Decay, Blizzard, Flamestrike, Rain of Fire, Hurricane,
// Volley: their persistent area aura is on it), or within reach of a tank's or a melee member's own area damage
// (Swipe, Thunder Clap, Blood Boil, Whirlwind, Consecration and Death and Decay at its feet). `why` names the rule.
// A loop over the group and the mob's auras: for actions and target values, never for a trigger.
bool CrowdControlWouldBreak(PlayerbotAI* botAI, Unit* unit, char const** why = nullptr);

// [rot_b31_cc] The hold spell of `option` is not broken by damage (Banish): the area rules above do not apply.
bool IsUnbreakableCrowdControl(PlayerbotAI* botAI, CrowdControlOption const& option);

// [rot_b31_cc] The unit carries a crowd control icon of the group (any but Skull and Cross).
bool HasCrowdControlIcon(Player* bot, Unit* unit);

// The mob this bot marked with its own crowd control icon, while the icon is still on it and nothing holds it.
Unit* GetAutoCcTarget(PlayerbotAI* botAI);

// The attackers are a pack worth crowd control: at least Playerbots.AutoCcMarkMinPackSize mobs, or three with a
// healer or two casters among them (MobRoles), and no boss. Cached for a second.
class AutoCcHardPackValue : public BoolCalculatedValue
{
public:
    AutoCcHardPackValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "auto cc hard pack", 1000) {}

    bool Calculate() override;
};

class RtiValue : public ManualSetValue<std::string>
{
public:
    RtiValue(PlayerbotAI* botAI);

    std::string const Save() override;
    bool Load(std::string const text) override;
};

class RtiCcValue : public ManualSetValue<std::string>
{
public:
    RtiCcValue(PlayerbotAI* botAI);

    std::string const Save() override;
    bool Load(std::string const text) override;
};

class AutoCcMarkValue : public ManualSetValue<AutoCcMark>
{
public:
    AutoCcMarkValue(PlayerbotAI* botAI) : ManualSetValue<AutoCcMark>(botAI, AutoCcMark(), "auto cc mark") {}
};

// The Skull and Cross the main tank put on its pack: it moves its own icons on and leaves anyone else's alone.
struct TankKillMarks
{
    ObjectGuid skull;
    ObjectGuid cross;
    // [b31_shared] The last run left a stale icon with no mob to put on it (a lone mob, the rest on other members).
    // The trigger stays quiet until the attackers count or the tank's victim changes, or idleMs is a few s old.
    uint32 idleMs = 0;
    size_t idleAttackers = 0;
    ObjectGuid idleVictim;
};

class TankKillMarksValue : public ManualSetValue<TankKillMarks>
{
public:
    TankKillMarksValue(PlayerbotAI* botAI) : ManualSetValue<TankKillMarks>(botAI, TankKillMarks(), "tank kill marks") {}
};

#endif
