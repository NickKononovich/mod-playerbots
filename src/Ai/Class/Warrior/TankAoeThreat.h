/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TANKAOETHREAT_H
#define PLAYERBOTS_TANKAOETHREAT_H

#include "AiObjectContext.h"
#include "Creature.h"
#include "GenericSpellActions.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TargetValue.h"
#include "ThreatManager.h"
#include "Timer.h"
#include "Trigger.h"

#include <algorithm>
#include <iterator>  // [lead526] std::begin / std::end
#include <string>
#include <unordered_map>

// [b32_peel] When a tank peels, the way a player tanks a big pull (dmn-n-5H-48: the bear's Feral Charge went to a
// Gordok Mage-Lord casting on the healer from range, 28 yd from the group, and the bear died there with the pack).
// Shared by the warrior, paladin, bear and death knight tanks: the peel spells, gap closers and peel walks in
// GenericSpellActions.cpp, ReachTargetActions.cpp, MovementActions.cpp, DruidBearActions.cpp, WarriorActions.cpp and
// "taunt reaction" (GenericTriggers.cpp). Every helper is O(1): pointer reads and cached values only.
//   1. a peel (taunt or single-target threat on a mob not on the tank) only on a mob in the tank's pack (within
//      ANCHOR_RANGE of the tank, the pack centre) or on a non-tank group member (MayPeel);
//   2. a gap closer (Charge, Intercept, Feral Charge) or a peel walk only onto a mob on a group member and next to
//      that member (MayCloseGap): the tank arrives at the group. A mob on the tank, on nobody, or a caster hitting
//      from range comes to the tank instead (taunt, Death Grip, Avenger's Shield, Faerie Fire);
//   3. more than 3 mobs in reach with one not held ("tank aoe crowd"): the area threat before any single peel
//      (CrowdFirst), except while a mob is on the healer (HealerPeelPending), which keeps the top.
namespace TankPeel
{
constexpr float ANCHOR_RANGE = 10.0f;          // a mob this close to the tank stands in its pack
constexpr float VICTIM_REACH = 8.0f;           // a mob this close to its victim is on the group, not casting from range
constexpr uint32 CROWD_MIN_MOBS = 4;           // "more than 3 attackers in reach"
constexpr char const* CROWD_TRIGGER = "tank aoe crowd";

// The non-tank group member `mob` is hitting, nullptr otherwise.
inline Player* GroupVictim(Player* bot, Unit* mob)
{
    Unit* victim = mob ? mob->GetVictim() : nullptr;
    Player* player = victim ? victim->ToPlayer() : nullptr;
    if (!player || player == bot || !player->IsAlive() || PlayerbotAI::IsTank(player) ||
        !bot->IsInSameRaidWith(player))
        return nullptr;

    return player;
}

inline bool IsBoss(Unit* mob)
{
    Creature* creature = mob ? mob->ToCreature() : nullptr;
    return creature && (creature->IsDungeonBoss() || creature->isWorldBoss());
}

// Rule 1.
inline bool MayPeel(Player* bot, Unit* mob)
{
    return mob && (bot->IsWithinDistInMap(mob, ANCHOR_RANGE) || GroupVictim(bot, mob));
}

// Rule 2: going to the mob brings the tank to the member it is hitting.
inline bool MayCloseGap(Player* bot, Unit* mob)
{
    Player* victim = GroupVictim(bot, mob);
    return victim && mob->IsWithinDistInMap(victim, VICTIM_REACH);
}

// Rule 2 for the current target ("enemy out of melee"): out of combat (the opener) or with nothing on the tank to
// leave behind, as before; with other mobs on the tank only onto a mob at a group member.
inline bool MayCloseGapOnTarget(Player* bot, Unit* target)
{
    if (!target || !bot->IsInCombat() || !PlayerbotAI::IsTank(bot) || MayCloseGap(bot, target))
        return true;

    Unit::AttackerSet const& onTank = bot->getAttackers();
    return onTank.empty() || (onTank.size() == 1 && onTank.count(target));
}

// Charge, Intercept, Feral Charge: the spell moves the caster to the target. Death Grip pulls the mob in instead.
inline bool IsGapCloser(SpellInfo const* spellInfo)
{
    return spellInfo && (spellInfo->HasEffect(SPELL_EFFECT_CHARGE) || spellInfo->HasEffect(SPELL_EFFECT_CHARGE_DEST) ||
                         spellInfo->HasEffect(SPELL_EFFECT_JUMP) || spellInfo->HasEffect(SPELL_EFFECT_JUMP_DEST));
}

// The peel's pick ("group member attacker", healer victims first) or the Righteous Defense friend is the healer.
inline bool HealerPeelPending(PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    Player* bot = botAI->GetBot();
    Player* victim = GroupVictim(bot, context->GetValue<Unit*>("group member attacker")->Get());
    if (victim && PlayerbotAI::IsHeal(victim))
        return true;

    Unit* attacked = context->GetValue<Unit*>("attacked group member")->Get();
    Player* friendPlayer = attacked ? attacked->ToPlayer() : nullptr;
    return friendPlayer && PlayerbotAI::IsHeal(friendPlayer);
}

// Rule 3: the crowd's area threat goes before a single peel. Not for a boss (the taunt-on-boss and swap logic) nor
// a mob on the healer; false for a class without the crowd trigger.
inline bool CrowdFirst(PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    if (IsBoss(context->GetValue<Unit*>("group member attacker")->Get()) || HealerPeelPending(botAI))
        return false;

    // [lead526] b211 rfc-h: the ready taunt went unused in ~14 of the 30 real aggro losses, most in 4+ mob trogg /
    // cultist pulls (5B 65 / 160 / 194 s, 5F 605 / 849 s: 7.2 s with no taunt at all) - "taunt reaction" waited for
    // the crowd's area threat while that spell was on its cooldown (Thunder Clap, Consecration, Death and Decay), so
    // neither ran. The area threat goes first only while one of the class's crowd spells is off cooldown
    Player* bot = botAI->GetBot();
    static char const* const WARRIOR[] = {"thunder clap"};
    static char const* const DRUID[] = {"swipe (bear)"};
    static char const* const PALADIN[] = {"consecration", "avenger's shield"};
    static char const* const DEATH_KNIGHT[] = {"pestilence", "death and decay", "blood boil"};
    char const* const* begin = nullptr;
    char const* const* end = nullptr;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            begin = std::begin(WARRIOR);
            end = std::end(WARRIOR);
            break;
        case CLASS_DRUID:
            begin = std::begin(DRUID);
            end = std::end(DRUID);
            break;
        case CLASS_PALADIN:
            begin = std::begin(PALADIN);
            end = std::end(PALADIN);
            break;
        case CLASS_DEATH_KNIGHT:
            begin = std::begin(DEATH_KNIGHT);
            end = std::end(DEATH_KNIGHT);
            break;
        default:
            return false;
    }
    bool ready = false;
    for (char const* const* spell = begin; spell != end && !ready; ++spell)
        ready = TauntReactionAction::SpellReady(botAI, *spell);
    if (!ready)
        return false;

    Trigger* crowd = context->GetTrigger(CROWD_TRIGGER);
    return crowd && crowd->IsActive();
}
}  // namespace TankPeel

// Two or more mobs around a tank and at least one of them not held: on another member, or held by proximity aggro
// alone. The core splits heal threat over every mob in combat with the healed member, so a mob the tank never hit
// turns to the healer on the first heal; area threat right after the pull lands takes them all. Shared by the
// warrior, paladin, bear and death knight tank strategies.
// [fresh_add_aoe] Wave and event adds (HoR spirits, VH portals, BM rifts) and a pack at the pull start: two or more
// mobs new to the fight (FRESH_ADD_MS) count while they are still walking in, within `approach` yards beyond the
// radius, for the area spells that stay on the ground (Consecration, Death and Decay): the adds walk into it.
class TankAoeThreatTrigger : public Trigger
{
public:
    static constexpr uint32 MIN_MOBS = 2;
    static constexpr float SECURE_LEAD = 1.3f;              // as the tank assist: held with this margin over the rest
    static constexpr float THREAT_FLOOR_PER_LEVEL = 3.0f;   // below this the first heal on the group takes the mob
    static constexpr uint32 SCAN_INTERVAL_MS = 400;         // the threat lists are read at most this often
    static constexpr uint32 FRESH_ADD_MS = 4000;            // a mob in the fight this long or less is still arriving

    // approach: 0 for the instant pulses (Thunder Clap, Swipe, Blood Boil); a fresh add out there is not hit yet
    TankAoeThreatTrigger(PlayerbotAI* botAI, float radius = 8.0f, float approach = 0.0f)
        : Trigger(botAI, "tank aoe threat"), _radius(radius), _approach(approach)
    {
    }

    bool IsActive() override
    {
        if (!bot->IsInCombat() || !PlayerbotAI::IsTank(bot))
            return false;

        uint32 const now = getMSTime();
        if (_scannedMs && getMSTimeDiff(_scannedMs, now) < SCAN_INTERVAL_MS)
            return _active;

        _scannedMs = now;
        _active = HasLooseMobNearby();
        return _active;
    }

private:
    // Also vetoes a crowd-controlled mob in reach and the strategies' AoE exclusions.
    bool HasLooseMobNearby()
    {
        GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
        uint32 const now = getMSTime();
        TrackFirstSeen(attackers, now);
        float const reach = _radius + _approach;
        bool const packAround = HasEnoughAoeTargets(botAI, bot->GetPosition(), _radius, MIN_MOBS);
        bool const packArriving =
            _approach > 0.0f && HasEnoughAoeTargets(botAI, bot->GetPosition(), reach, MIN_MOBS);
        if (!packAround && !packArriving)
            return false;

        float const floor = bot->GetLevel() * THREAT_FLOOR_PER_LEVEL;
        uint32 freshArriving = 0;
        bool looseInRadius = false;
        bool looseFresh = false;
        for (ObjectGuid const& guid : attackers)
        {
            Unit* mob = botAI->GetUnit(guid);
            if (!mob || !mob->IsAlive() || mob->IsPlayer() || !mob->IsInWorld() || mob->GetMap() != bot->GetMap())
                continue;

            float const distance = mob->GetDistance(bot);
            if (distance > reach || IsCrowdControlled(mob))
                continue;

            bool const loose = IsLoose(mob, floor);
            if (distance <= _radius)
                looseInRadius = looseInRadius || loose;

            auto const seen = _firstSeen.find(guid);
            if (_approach > 0.0f && seen != _firstSeen.end() && getMSTimeDiff(seen->second, now) <= FRESH_ADD_MS)
            {
                ++freshArriving;
                looseFresh = looseFresh || loose;
            }
        }

        // the pack around the tank, as before
        if (packAround && looseInRadius)
            return true;

        return packArriving && looseFresh && freshArriving >= MIN_MOBS;
    }

    // When each attacker joined the fight (first listed in "attackers").
    void TrackFirstSeen(GuidVector const& attackers, uint32 now)
    {
        std::unordered_map<ObjectGuid, uint32> seen;
        for (ObjectGuid const& guid : attackers)
        {
            auto const known = _firstSeen.find(guid);
            seen[guid] = known != _firstSeen.end() ? known->second : now;
        }

        _firstSeen = std::move(seen);
    }

    bool IsLoose(Unit* mob, float floor) const
    {
        Unit* victim = mob->GetVictim();
        if (victim && victim != bot)
            return true;

        if (!mob->CanHaveThreatList())
            return false;

        float mine = 0.0f;
        float others = 0.0f;
        for (ThreatReference const* ref : mob->GetThreatMgr().GetUnsortedThreatList())
        {
            if (ref->GetVictim() == bot)
                mine = ref->GetThreat();
            else
                others = std::max(others, ref->GetThreat());
        }

        return mine < floor || mine < others * SECURE_LEAD;
    }

    float _radius;
    float _approach;
    uint32 _scannedMs = 0;
    bool _active = false;
    std::unordered_map<ObjectGuid, uint32> _firstSeen;
};

// [b32_tank_aoe] Three or more mobs around a tank, one of them not held with a safe lead: the tank's area spell goes
// above its single-target builders (Mangle, Shield Slam, Heart Strike / Death Strike, the paladin 969 band) and stays
// there until every mob in reach has that lead, then the single-target order resumes. Build 31 put the bear's Swipe
// below Mangle and Faerie Fire on 3+ mobs (17.3 under 17.5 / 17.0): on 5-9 mob pulls Swipe fell from 21.9 to 14.9 a
// minute and Mangle rose from 2.6 to 9.6, aggro losses from 0.33 to 2.25 a pull. "tank aoe threat" (above) only
// covers the moment a mob is already loose with the 1.3 lead; this one keeps the area threat going on a pack until
// the lead is safe (hysteresis: enter below ENTER_LEAD, leave at SAFE_LEAD and SAFE_FLOOR_PER_LEVEL).
// Cost: IsActive() returns the cached answer between scans (SCAN_INTERVAL_MS); a scan reads the cached "attackers"
// value and the threat list of the mobs in reach only.
class TankAoePackTrigger : public Trigger
{
public:
    static constexpr uint32 MIN_MOBS = 3;
    static constexpr float ENTER_LEAD = 1.3f;               // as "tank aoe threat": below it a heal can take the mob
    static constexpr float SAFE_LEAD = 1.6f;                // above the 130 % ranged pull-off with room for a heal
    static constexpr float ENTER_FLOOR_PER_LEVEL = 3.0f;
    static constexpr float SAFE_FLOOR_PER_LEVEL = 10.0f;    // ~3 Swipes / Thunder Claps on a fresh mob at 58-60
    static constexpr uint32 SCAN_INTERVAL_MS = 400;

    // [b32_peel] minMobs / name: "tank aoe crowd" (TankAoeCrowdTrigger) is this trigger from CROWD_MIN_MOBS mobs
    TankAoePackTrigger(PlayerbotAI* botAI, float radius = 8.0f, uint32 minMobs = MIN_MOBS,
                       std::string const name = "tank aoe pack")
        : Trigger(botAI, name), _radius(radius), _minMobs(minMobs)
    {
    }

    bool IsActive() override
    {
        if (!bot->IsInCombat() || !PlayerbotAI::IsTank(bot))
        {
            _active = false;
            return false;
        }

        uint32 const now = getMSTime();
        if (_scannedMs && getMSTimeDiff(_scannedMs, now) < SCAN_INTERVAL_MS)
            return _active;

        _scannedMs = now;
        _active = Scan();
        return _active;
    }

private:
    bool Scan() const
    {
        // the count first (also vetoes a crowd-controlled mob in reach and the strategies' AoE exclusions)
        if (!HasEnoughAoeTargets(botAI, bot->GetPosition(), _radius, _minMobs))
            return false;

        // entering needs a mob below the "tank aoe threat" lead; staying needs one below the safe lead
        float const lead = _active ? SAFE_LEAD : ENTER_LEAD;
        float const floor = bot->GetLevel() * (_active ? SAFE_FLOOR_PER_LEVEL : ENTER_FLOOR_PER_LEVEL);
        GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
        for (ObjectGuid const& guid : attackers)
        {
            Unit* mob = botAI->GetUnit(guid);
            if (!mob || !mob->IsAlive() || mob->IsPlayer() || !mob->IsInWorld() || mob->GetMap() != bot->GetMap())
                continue;

            if (mob->GetDistance(bot) > _radius || IsCrowdControlled(mob))
                continue;

            Unit* victim = mob->GetVictim();
            if (victim && victim != bot)
                return true;

            if (!mob->CanHaveThreatList())
                continue;

            float mine = 0.0f;
            float others = 0.0f;
            for (ThreatReference const* ref : mob->GetThreatMgr().GetUnsortedThreatList())
            {
                if (ref->GetVictim() == bot)
                    mine = ref->GetThreat();
                else
                    others = std::max(others, ref->GetThreat());
            }

            if (mine < floor || mine < others * lead)
                return true;
        }

        return false;
    }

    float _radius;
    uint32 _minMobs;
    uint32 _scannedMs = 0;
    bool _active = false;
};

// [b32_peel] "tank aoe crowd": more than 3 mobs in reach and one not held with a safe lead (as "tank aoe pack", from
// TankPeel::CROWD_MIN_MOBS mobs). Its area spells sit above every single peel (ACTION_INTERRUPT + 0.6 and up; the
// peel steps are +0.1..+0.55) and "taunt reaction" waits for it (TankPeel::CrowdFirst). Off while the peel's mob is
// on the healer: that peel keeps the top. Cost: the cached "group member attacker" / "attacked group member" values,
// then the parent's cached scan.
class TankAoeCrowdTrigger : public TankAoePackTrigger
{
public:
    TankAoeCrowdTrigger(PlayerbotAI* botAI, float radius = 8.0f)
        : TankAoePackTrigger(botAI, radius, TankPeel::CROWD_MIN_MOBS, TankPeel::CROWD_TRIGGER)
    {
    }

    bool IsActive() override { return !TankPeel::HealerPeelPending(botAI) && TankAoePackTrigger::IsActive(); }
};

// A tank's area spell around itself (Thunder Clap, Consecration, Swipe, Blood Boil): cast on the tank, so a current
// target a step outside melee range does not hold it back.
class CastTankAreaThreatAction : public CastSpellAction
{
public:
    CastTankAreaThreatAction(PlayerbotAI* botAI, std::string const spell)
        : CastSpellAction(botAI, spell), _name(spell + " tank aoe")
    {
    }

    std::string const GetTargetName() override { return "self target"; }
    std::string const getName() override { return _name; }
    bool isUseful() override
    {
        if (!PlayerbotAI::IsTank(bot) || !CastSpellAction::isUseful())
            return false;

        // [fresh_add_aoe] the trigger also fires for adds still walking in: a spell that stays on the ground
        // (Consecration) is laid for them, an instant pulse (Thunder Clap, Swipe, Blood Boil) waits for one in reach
        uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
        SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!spellInfo)
            return true;

        if (spellInfo->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA))
            return true;

        float reach = 0.0f;
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            if (effect.HasRadius())
                reach = std::max(reach, effect.CalcRadius(bot));
        }

        return HasEnoughAoeTargets(botAI, bot->GetPosition(), reach > 0.0f ? reach : DEFAULT_PULSE_REACH, 1);
    }

private:
    static constexpr float DEFAULT_PULSE_REACH = 8.0f;
    std::string _name;
};

#endif
