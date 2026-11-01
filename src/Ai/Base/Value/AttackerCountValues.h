/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ATTACKERCOUNTVALUES_H
#define PLAYERBOTS_ATTACKERCOUNTVALUES_H

#include "NamedObjectContext.h"
#include "ObjectGuid.h"
#include "Value.h"
#include <unordered_map>
#include <vector>

class PlayerbotAI;
class Creature;
class Unit;

class AttackerCountValue : public Uint8CalculatedValue, public Qualified
{
public:
    AttackerCountValue(PlayerbotAI* botAI, std::string const name = "attackers count")
        : Uint8CalculatedValue(botAI, name)
    {
    }

    Unit* GetTarget();
    uint8 Calculate() override;
};

class MyAttackerCountValue : public Uint8CalculatedValue, public Qualified
{
public:
    MyAttackerCountValue(PlayerbotAI* botAI, std::string const name = "my attackers count")
        : Uint8CalculatedValue(botAI, name)
    {
    }

    Unit* GetTarget();
    uint8 Calculate() override;
};

class HasAggroValue : public BoolCalculatedValue, public Qualified
{
public:
    HasAggroValue(PlayerbotAI* botAI, std::string const name = "has aggro") : BoolCalculatedValue(botAI, name) {}

    Unit* GetTarget();
    bool Calculate() override;
};

// A mob hitting a non-tank group member in the tank's line of sight, for the tank to take back: healer victims
// first, then the most hurt victim, then the nearest mob. A mob the tank's peel already took within
// PEEL_SPREAD_MS ranks below every other victim's mob, so two taunts in a row cover two victims (wave 10
// Underbog: Hand of Reckoning and Righteous Defense both went to the healer's mob, the shaman's killed him).
// Raid bosses are left to their encounter strategies (tank swaps).
class GroupMemberAttackerValue : public UnitCalculatedValue
{
public:
    static constexpr uint32 CHECK_INTERVAL_MS = 300;  // line of sight per mob: not on every read
    static constexpr uint32 PEEL_SPREAD_MS = 8000;     // the taunts' cooldown

    GroupMemberAttackerValue(PlayerbotAI* botAI, std::string const name = "group member attacker")
        : UnitCalculatedValue(botAI, name, CHECK_INTERVAL_MS)
    {
    }

    Unit* Calculate() override;

    // A peel spell landed on these mobs: remembered for the spread, and both peel values pick afresh.
    static void RecordPeel(PlayerbotAI* botAI, GuidVector const& mobs);
    static bool IsRecentlyPeeled(PlayerbotAI* botAI, ObjectGuid mob);
    // The best mob on a non-tank member within `range` yards of combat reach and in sight, recently peeled or not:
    // the target of a short-reach peel spell (Hammer of the Righteous, Judgement) when the pick stands further away.
    static Unit* FindInReach(PlayerbotAI* botAI, float range);
    // [taunt_far_first] The mob for a taunt or a ranged threat spell of `range` yards: as the pick, but a mob out of
    // the tank's melee and area reach (TAUNT_AREA_REACH) first, and not one the peel took within PEEL_SPREAD_MS. The
    // mob beside the tank is held by the hands and the area threat; the one on a caster 15 yd off only by a taunt
    // (hor-h build 29: Hand of Reckoning went to the Ghostly Priest on the shaman at 0 yd, the Shadowy Mercenary
    // stayed on the mage 5 s).
    static constexpr float TAUNT_AREA_REACH = 8.0f;
    static constexpr float TAUNT_FAR_BONUS = 60.0f;
    static Unit* FindForTaunt(PlayerbotAI* botAI, float range);
    // How long the mob has been hitting its current non-tank victim, 0 when it is not.
    static uint32 LooseForMs(PlayerbotAI* botAI, ObjectGuid mob);
    // [taunt_reaction] A taunt / ranged threat / Righteous Defense / Challenging Shout of the tank landed on `mob`,
    // which was hitting `victim`: logged once per loose spell of the mob ("[taunt reaction]" line and the harness
    // event "taunt_reaction" with the delay since the mob turned to the victim). `via` names the action.
    static void NoteReaction(PlayerbotAI* botAI, Unit* mob, Unit* victim, std::string const& spell,
                             std::string const& via);

private:
    // Peel diagnostics (debug log only): a mob hitting a non-tank member of a party for LOOSE_LOG_MS is logged once
    // with why the peel did or did not take it.
    static constexpr uint32 LOOSE_LOG_MS = 2000;
    struct LooseMob
    {
        ObjectGuid victim;
        uint32 sinceMs = 0;
        bool logged = false;
        bool reacted = false;  // [taunt_reaction] already logged for this loose spell
    };

    void TrackLooseMobs(GuidVector const& attackers, Unit* best);

    std::unordered_map<ObjectGuid, uint32> _peeled;  // mob, time of the peel
    std::unordered_map<ObjectGuid, LooseMob> _loose;
};

// Mobs hitting non-tank group members within the qualifier's range in yards of the bot (default: taunt range).
class GroupMemberAttackerCountValue : public Uint8CalculatedValue, public Qualified
{
public:
    GroupMemberAttackerCountValue(PlayerbotAI* botAI, std::string const name = "group member attacker count")
        : Uint8CalculatedValue(botAI, name)
    {
    }

    uint8 Calculate() override;
};

// The non-tank group member a mob is hitting, within Righteous Defense range (40 yd) and sight of the bot: that
// spell is cast on the friend, so the mob itself may stand out of taunt range. Healer first, then most hurt, then
// most mobs; only mobs the peel has not just taken count, so it covers a victim the last taunt left alone.
class AttackedGroupMemberValue : public UnitCalculatedValue
{
public:
    AttackedGroupMemberValue(PlayerbotAI* botAI, std::string const name = "attacked group member")
        : UnitCalculatedValue(botAI, name, GroupMemberAttackerValue::CHECK_INTERVAL_MS)
    {
    }

    Unit* Calculate() override;
};

// The mob hitting a non-tank member that an out-of-combat tank takes at once: healer first, then the most hurt, then
// the nearest; in sight, 40 yd (60 on the same floor for a healer). Read from the members' getAttackers(), not the
// cached threat-list "attackers" (Stratholme: 7 mobs killed the shaman 3 yd from an idle tank). No raid bosses.
class DefendMemberTargetValue : public UnitCalculatedValue
{
public:
    DefendMemberTargetValue(PlayerbotAI* botAI, std::string const name = "defend member target")
        : UnitCalculatedValue(botAI, name, GroupMemberAttackerValue::CHECK_INTERVAL_MS)
    {
    }

    Unit* Calculate() override;
};

// Non-raid dungeons: the nearest mob hitting the group's healer out of the tank's area threat, up to 60 yd away on
// the same floor. The tank walks back to it whatever it is fighting; a boss on the tank is not dragged along.
// Read from the healers' threat lists: "attackers" drops mobs out of the bot's line of sight (wave 9 Steamvault).
class FarHealerAttackerValue : public UnitCalculatedValue
{
public:
    FarHealerAttackerValue(PlayerbotAI* botAI, std::string const name = "far healer attacker")
        : UnitCalculatedValue(botAI, name)
    {
    }

    Unit* Calculate() override;

    // A mob on a healer of the bot's group within the walk-back range (no area-threat or boss condition).
    static bool IsHealerAttackerInReach(PlayerbotAI* botAI, Creature* attacker);
    // Every such mob, line of sight or not.
    static void GetHealerAttackers(PlayerbotAI* botAI, std::vector<Creature*>& attackers);
};

// Any role: the nearest mob hitting a healer of the bot's group within the qualifier's range in yards of the bot,
// for crowd control that helps while the tank's taunts are on cooldown (Frost Nova).
class HealerAttackerNearValue : public UnitCalculatedValue, public Qualified
{
public:
    HealerAttackerNearValue(PlayerbotAI* botAI, std::string const name = "healer attacker near")
        : UnitCalculatedValue(botAI, name)
    {
    }

    Unit* Calculate() override;
};

// [flee_stop] Mobs that ran from the group at low health: the core's flee for assistance (ASSISTANCE_MOTION_TYPE,
// "attempts to run away in fear") or its plain flee (UNIT_STATE_FLEEING without a fear aura: no help in reach). A
// runner brings the next pack; one that lost the fight on the way stands idle, damaged, where the group already
// walked (strat-h build 29: a Crimson Initiate 10420 fled from the Hall of Lights fight and was left behind).
// Shared by every bot on the map threads and read by the harness on the world thread (Retarget: "fled survivor").
class FledMobs
{
public:
    static constexpr uint32 MEMO_MS = 15 * 60 * 1000;  // a survivor is remembered this long
    static constexpr uint32 STOP_SPREAD_MS = 2000;     // after one bot's stun or snare the others burst, not stop
    static constexpr size_t MAX_MOBS = 256;
    static constexpr float HOME_SLACK = 5.0f;  // a mob this near its spawn at full health is back home

    // Running from the fight now (not under a player's fear: that is our crowd control).
    static bool IsRunning(Unit const* unit);
    // A runner seen by a bot of the group.
    static void Note(Unit const* unit);
    // A noted runner that left the fight and stands idle, alive, not evading, still damaged or away from home.
    static bool IsSurvivor(Creature const* creature);
    // Every noted mob of this map instance (the caller looks them up).
    static GuidVector Noted(uint32 mapId, uint32 instanceId);
    static void MarkStopped(Unit const* mob);
    static bool StoppedRecently(Unit const* mob);
};

// [flee_stop] The nearest runner (FledMobs::IsRunning) among the bot's attackers within RANGE yards and in sight.
class FleeingMobValue : public UnitCalculatedValue
{
public:
    static constexpr float RANGE = 35.0f;

    FleeingMobValue(PlayerbotAI* botAI, std::string const name = "fleeing mob")
        : UnitCalculatedValue(botAI, name, GroupMemberAttackerValue::CHECK_INTERVAL_MS)
    {
    }

    Unit* Calculate() override;
};

// [fled_survivor] The nearest fled survivor (FledMobs::IsSurvivor) within RANGE yards of the bot and in sight.
class FledSurvivorValue : public UnitCalculatedValue
{
public:
    static constexpr float RANGE = 40.0f;
    static constexpr uint32 CHECK_INTERVAL_MS = 1000;

    FledSurvivorValue(PlayerbotAI* botAI, std::string const name = "fled survivor")
        : UnitCalculatedValue(botAI, name, CHECK_INTERVAL_MS)
    {
    }

    Unit* Calculate() override;
};

class BalancePercentValue : public Uint8CalculatedValue, public Qualified
{
public:
    BalancePercentValue(PlayerbotAI* botAI, std::string const name = "balance percentage")
        : Uint8CalculatedValue(botAI, name)
    {
    }

    Unit* GetTarget();
    uint8 Calculate() override;
};

#endif
