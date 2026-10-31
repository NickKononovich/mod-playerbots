/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HEALTHTRIGGERS_H
#define PLAYERBOTS_HEALTHTRIGGERS_H

#include "PlayerbotAIConfig.h"
#include "Multiplier.h"
#include "Trigger.h"
#include <stdexcept>

class Creature;  // [b46_patrol]
class Player;
class PlayerbotAI;

class ValueInRangeTrigger : public Trigger
{
public:
    ValueInRangeTrigger(PlayerbotAI* botAI, std::string const name, float maxValue, float minValue)
        : Trigger(botAI, name), maxValue(maxValue), minValue(minValue)
    {
    }

    virtual float GetValue() = 0;
    bool IsActive() override
    {
        float value = GetValue();
        return value < maxValue && value >= minValue;
    }

protected:
    float maxValue, minValue;
};

class HealthInRangeTrigger : public ValueInRangeTrigger
{
public:
    HealthInRangeTrigger(PlayerbotAI* botAI, std::string const name, float maxValue, float minValue = 0)
        : ValueInRangeTrigger(botAI, name, maxValue, minValue)
    {
    }

    bool IsActive() override;
    float GetValue() override;
};

class LowHealthTrigger : public HealthInRangeTrigger
{
public:
    LowHealthTrigger(PlayerbotAI* botAI, std::string const name = "low health",
                     float value = sPlayerbotAIConfig.LowHealth, float minValue = 0)
        : HealthInRangeTrigger(botAI, name, value, minValue)
    {
    }

    std::string const GetTargetName() override { return "self target"; }
};

class CriticalHealthTrigger : public LowHealthTrigger
{
public:
    CriticalHealthTrigger(PlayerbotAI* botAI)
        : LowHealthTrigger(botAI, "critical health", sPlayerbotAIConfig.CriticalHealth, 0)
    {
    }
};

class MediumHealthTrigger : public LowHealthTrigger
{
public:
    MediumHealthTrigger(PlayerbotAI* botAI)
        : LowHealthTrigger(botAI, "medium health", sPlayerbotAIConfig.MediumHealth, 0)
    {
    }
};

class AlmostFullHealthTrigger : public LowHealthTrigger
{
public:
    AlmostFullHealthTrigger(PlayerbotAI* botAI)
        : LowHealthTrigger(botAI, "almost full health", sPlayerbotAIConfig.AlmostFullHealth,
                           sPlayerbotAIConfig.MediumHealth)
    {
    }
};

class PartyMemberLowHealthTrigger : public HealthInRangeTrigger
{
public:
    PartyMemberLowHealthTrigger(PlayerbotAI* botAI, std::string const name = "party member low health",
                                float value = sPlayerbotAIConfig.LowHealth,
                                float minValue = 0)
        : HealthInRangeTrigger(botAI, name, value, minValue)
    {
    }

    std::string const GetTargetName() override { return "party member to heal"; }
};

class PartyMemberCriticalHealthTrigger : public PartyMemberLowHealthTrigger
{
public:
    PartyMemberCriticalHealthTrigger(PlayerbotAI* botAI)
        : PartyMemberLowHealthTrigger(botAI, "party member critical health", sPlayerbotAIConfig.CriticalHealth, 0)
    {
    }
};

class PartyMemberMediumHealthTrigger : public PartyMemberLowHealthTrigger
{
public:
    PartyMemberMediumHealthTrigger(PlayerbotAI* botAI)
        : PartyMemberLowHealthTrigger(botAI, "party member medium health", sPlayerbotAIConfig.MediumHealth,
                                      0)
    {
    }
};

class PartyMemberAlmostFullHealthTrigger : public PartyMemberLowHealthTrigger
{
public:
    PartyMemberAlmostFullHealthTrigger(PlayerbotAI* botAI)
        : PartyMemberLowHealthTrigger(botAI, "party member almost full health", sPlayerbotAIConfig.AlmostFullHealth,
                                      0)
    {
    }
};

// The heal target lost HealerFastHealthLossPct of its max health within two seconds: the fast heals go out before
// it reaches the low-health band, which a non-tank under melee passes on its way to dying.
class PartyMemberLosingHealthFastTrigger : public Trigger
{
public:
    PartyMemberLosingHealthFastTrigger(PlayerbotAI* botAI) : Trigger(botAI, "party member losing health fast") {}

    std::string const GetTargetName() override { return "party member to heal"; }
    bool IsActive() override;
};

class TargetLowHealthTrigger : public HealthInRangeTrigger
{
public:
    TargetLowHealthTrigger(PlayerbotAI* botAI, float value, float minValue = 0)
        : HealthInRangeTrigger(botAI, "target low health", value, minValue)
    {
    }

    std::string const GetTargetName() override { return "current target"; }
};

class TargetCriticalHealthTrigger : public TargetLowHealthTrigger
{
public:
    TargetCriticalHealthTrigger(PlayerbotAI* botAI) : TargetLowHealthTrigger(botAI, 20) {}
};

class HealerLowManaTrigger : public Trigger
{
public:
    HealerLowManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "healer low mana") {}

    std::string const GetTargetName() override { return "healer low mana"; }
    bool IsActive() override;
};

class PartyMemberDeadTrigger : public Trigger
{
public:
    PartyMemberDeadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "resurrect", 1 * IN_MILLISECONDS) {}

    std::string const GetTargetName() override { return "party member to resurrect"; }
    bool IsActive() override;
};

class CombatPartyMemberDeadTrigger : public Trigger
{
public:
    CombatPartyMemberDeadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "combat party member to resurrect", 1) {}
    std::string const GetTargetName() override { return "party member to resurrect"; }
    bool IsActive() override;
};

class DeadTrigger : public Trigger
{
public:
    DeadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dead") {}

    std::string const GetTargetName() override { return "self target"; }
    bool IsActive() override;
};

class AoeHealTrigger : public Trigger
{
public:
    AoeHealTrigger(PlayerbotAI* botAI, std::string const name, std::string const type, int32 count)
        : Trigger(botAI, name), count(count), type(type)
    {
    }  // reorder args - whipowill
    bool IsActive() override;

protected:
    int32 count;
    std::string const type;
};

// [heal_triage b29] A single member below HealTriage::EMERGENCY_HEALTH_PCT (35 %).
class PartyMemberEmergencyHealthTrigger : public PartyMemberLowHealthTrigger
{
public:
    PartyMemberEmergencyHealthTrigger(PlayerbotAI* botAI);
};

// [heal_triage b29] While the heal target is below HealTriage::EMERGENCY_HEALTH_PCT, a grouped healer's utility waits:
// a cure on another member, a facing turn, a damage spell. HoR escape 2026-10-25 16:58:31-33: "set facing" (37) and
// "remove curse on party" (29.6) ran in the seconds the mage went 69 -> 4 %. A cure on the dying member stays.
class HealerEmergencyMultiplier : public Multiplier
{
public:
    HealerEmergencyMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "healer emergency") {}

    float GetValue(Action* action) override;
};

class AoeInGroupTrigger : public Trigger
{
public:
    AoeInGroupTrigger(PlayerbotAI* botAI, std::string name, std::string type)
        : Trigger(botAI, name), type(type)
    {
    }
    bool IsActive() override;

protected:
    std::string type;
};

// Resting between pulls the way a group does it: a bot sits down only below its own rest line, or joins the group's
// rest below the company line while somebody close by already sits. Real food and water from the bags only.
namespace RestState
{
constexpr float REST_MANA_PCT = 50.0f;          // a mana user drinks after a pull below this
constexpr float HEALER_REST_MANA_PCT = 60.0f;   // ... a healer below this
constexpr float REST_HEALTH_PCT = 50.0f;        // anybody eats below this when no healer can heal it up
constexpr float HEALER_BUSY_MANA_PCT = 60.0f;   // a healer below this mana does not count as "can heal it up"
// [rest_company_45] joins the group's rest below this (mana or health); 70 % before DOWNTIME_AUDIT 2026-10-25: one
// member sitting at 85 % pulled everyone below 70 % down with it, 15-25 s per sit-down (regen waits 20,724 s a wave).
constexpr float COMPANY_PCT = 45.0f;
// [rest_no_company] STN92_EFFICIENCY 2026-10-25: false - nobody sits down only because somebody close by sits (the
// "joins the group's rest" cascade: hunter 6, paladin 5, mage 3 sit-downs in st-n-92 above their own lines); a bot
// drinks / eats only when its own line (the next fight's need) says so. true restores the company line above.
constexpr bool REST_JOIN_COMPANY = false;
// [rest_prot_paladin_line] A protection paladin regains mana in the fight it tanks (Spiritual Attunement, Blessing of
// Sanctuary, Divine Plea): its trash line is this, not the tank's predicted need (the paladin tank stood in 51 % of
// the wave's drinking waits, e.g. dme-h "avgMana 96, waitingFor Rtpalak").
constexpr float PROT_PALADIN_REST_MANA_PCT = 30.0f;
constexpr float GROUP_RANGE = 50.0f;            // group members this close count for "fighting" / "resting"

// Drinking for the next fight (2026-10-24, *.bak_drinkpredict), false restores the fixed lines above: with the
// tank's plan for its next pull known, a mana user drinks below what that fight costs it plus a reserve
// (a priest at 45 % stays up for one small mob, tops up at 65-70 % before a big pack), and before a boss always.
// Without a prediction (no plan published, no group) the fixed lines hold as before.
constexpr bool PREDICT_DRINK = true;
// Mana burn per second of fight, % of max mana, from the [rest] drink / stand-up lines against the [pull check]
// durations between them (Playerbots-20261024-204140, 5 groups, full -> next drink): Shadow Priest median 1.38
// (pooled 1.16, N 40), Elemental Shaman 0.98 (0.74, N 39), Holy Paladin 0.55 (0.36, N 17).
constexpr float CASTER_BURN_PCT_PER_SEC = 1.3f;
constexpr float HEALER_BURN_PCT_PER_SEC = 0.6f;
// The other mana users (2026-10-24, *.bak_drink2): a hunter sat 18 s "to drink" at 47 % before a one-mob pull (Utgarde
// Keep heroic). Measured the same way, every kept log of 2026-10-24 scoped per harness run (names are reused across
// runs), full -> next drink: Marksmanship Hunter median 1.53 %/s (pooled 1.35, N 844), Enhancement Shaman 1.56 (1.20,
// N 778), Prot Paladin tank 1.39 (1.08, N 983). Retribution Paladin and feral druid have no window in these logs (no
// drink after a [pull check] from full): they take the melee rate. false restores the fixed lines for these roles.
constexpr bool PREDICT_DRINK_ALL_ROLES = true;
constexpr float HUNTER_BURN_PCT_PER_SEC = 1.5f;
constexpr float MELEE_BURN_PCT_PER_SEC = 1.5f;  // a melee mana user (enhancement shaman, retribution paladin)
constexpr float TANK_BURN_PCT_PER_SEC = 1.4f;   // a tank on mana (protection paladin)
// A healer's mana per tank health the pack lands over the fight (the planner's cost): 11.0 % (same windows).
constexpr float HEALER_PCT_PER_TANK_HEALTH = 11.0f;
// Fight length when the plan gives only its mob count: median [pull check] duration by mobs that came, all kept
// logs of 2026-10-24 (N 4883): 1 -> 10 s, 2 -> 16, 3 -> 19, 4 -> 22, 5 -> 24, 6 -> 27, 7 -> 30.
constexpr float FIGHT_BASE_SEC = 6.0f;
constexpr float FIGHT_SEC_PER_MOB = 4.0f;
// Left over after the predicted fight: a pull that brings more than planned (a third of the measured windows burnt
// ~1.8x the median rate) and the healer's spike.
constexpr float PREDICT_RESERVE_PCT = 20.0f;
constexpr float HEALER_PREDICT_RESERVE_PCT = 25.0f;
// [rest_trash_line_75] above this only before a boss (90 before DOWNTIME_AUDIT 2026-10-25: in 68 % of the wave's
// drinking waits the group's average mana was >= 80 %; a trash pull needs 20-45 % of a caster's mana).
constexpr float PREDICT_MAX_LINE_PCT = 75.0f;
constexpr float BOSS_TOPUP_PCT = 95.0f;        // before a boss everybody tops up below this
// [rest_topup_real_boss] STN92_EFFICIENCY 2026-10-25: the 95 % top-up only before a real boss (an encounter of the
// map's DungeonEncounter.dbc list, NextFight::realBoss), never before a mini-boss / named trash flagged as a dungeon
// boss (st-n-92: "top-up 95 % before a boss (2 mobs)" for the Hukku troll pack, 44 + 6 s). Before such a mob the
// trash prediction holds, at least this line (the planner's mini-boss readiness, BossReadyGap, asks a little less).
constexpr float MINI_BOSS_TOPUP_PCT = 70.0f;

// The tank's next fight (planned, or refused as it stands) as its careful pull planner estimates it.
struct NextFight
{
    uint32 mobs = 0;
    float fightSec = 0.0f;  // 0: not estimated, read from `mobs`
    float cost = 0.0f;      // tank healths the pack lands on the tank over the fight (the healer heals them back)
    bool boss = false;
    bool realBoss = false;  // the boss is an encounter of the map's DungeonEncounter.dbc list (not a mini-boss)
    bool topUp = false;     // the tank refused it and waits for the group topped up (its healers to this line)
};
// The healer line for a refused pack: above the planner's topped-up mana (CAREFUL_FALLBACK_MANA, 80 %). wc-h build 19:
// the healer stood at 70-79 % (its line for a 1-mob next fight was lower) and the tank waited for 80 % for good.
constexpr float REFUSED_TOPUP_HEALER_PCT = 82.0f;
// The bot's group tank's next fight; false when nothing is known (no group, no tank, no fresh plan).
bool PredictNextFight(Player* bot, NextFight& fight);
// [lead51] user: «нет хард-константы 90 %» - the mana a member needs for a fight is computed: its spend rate (measured
// in this run's fights, NoteManaRate; else its role's burn rate above) x the fight's seconds (a healer: at least its
// share of the tank health the fight costs), plus the reserve, minus the mana its own ready cooldowns return
// (Evocation, Divine Plea). One line for the boss gate, the boss rest and the trash rest; why: the terms
float FightManaLine(Player* bot, float fightSec, float cost, std::string* why = nullptr);
// [lead51] the leader notes a member's measured mana spend in fights (% of max mana per second of fight)
void NoteManaRate(ObjectGuid member, float pctPerSec);

// The bot sits with a food / drink aura (spell specific, breaks on standing up), not a passive regen buff.
bool IsEating(Player* player);
bool IsDrinking(Player* player);
// The bot or a group member close by is in combat: no sitting down, the fight goes on.
bool GroupFighting(Player* bot);
// A group member close by sits eating or drinking: the others top off meanwhile.
bool GroupResting(Player* bot);
// Alive, out of combat, on dry ground, not mounted and nothing to fight close by.
bool CanRest(Player* bot);
// A healer with a hurt group member close by heals first and drinks after.
bool HealerHasWork(Player* bot);
// The rest lines alone (no aura / CanRest check, the harness asks them too); why: the one-line reason when true.
// skip: set when the next fight's prediction keeps the bot up although the fixed line would have it drink.
bool WantsDrink(Player* bot, std::string* why = nullptr, std::string* skip = nullptr);
bool WantsFood(Player* bot, std::string* why = nullptr);
// [lead59] the member's own lines (the computed line for its next fight), without the leader's Rest command: what the
// leader's Rest is judged by (it stops at the line, not at full)
bool BelowOwnLine(Player* bot, std::string* why = nullptr);
// Sitting with food / drink that has nothing left to do: full, or a fight started, or (REST_STAND_AT_LINE) above its
// own line while nobody close by still sits.
bool RestDone(Player* bot);
// [rest_stand_at_line] A sitter above its own line stands up once nobody close by sits any more: the group is ready
// and waits for no top-up to 100 % (the harness waited for every sitter until "rested", 15-25 s per sit-down).
constexpr bool REST_STAND_AT_LINE = true;
// [b32_dmw] "Nobody close by sits" counts only sitters still below their own line (the ones the harness waits for);
// false restores any sitter (two sitters above their lines kept each other seated while the group walked off).
constexpr bool REST_STAND_BELOW_LINE_COMPANY = true;
// [b46_patrol] strat-h 20261026-093620-strat-h-5E-2 (1520-1571 s): right after a 13-kill pull the warlock sat down
// to drink 26 yd from the Patchwork Horror patrol walking in; it aggroed on the rogue beside the next street packs and
// three died. A player does not sit down in a patrol's way: no drink / food while an idle patrol (waypoint walker or
// a member of a walking formation) stands within its aggro reach + PATROL_REST_MARGIN of the bot in a 5-man dungeon,
// and a sitter stands up (RestDone). The test harness steps the group back out of the patrol's walk meanwhile.
constexpr bool PATROL_REST_GATE = true;
constexpr float PATROL_REST_MARGIN = 5.0f;
// The idle patrol close enough to keep the bot standing (gap: its distance); nullptr when none. Bot thread only (it
// reads the bot's "possible targets no los").
Creature* PatrolComes(Player* bot, float* gap = nullptr);
}  // namespace RestState

class RestManaTrigger : public Trigger
{
public:
    RestManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "rest mana", 2) {}

    bool IsActive() override;

private:
    std::string _skipLogged;  // the last "[rest] skips" logged, again only when it changes or after a fight
};

class RestHealthTrigger : public Trigger
{
public:
    RestHealthTrigger(PlayerbotAI* botAI) : Trigger(botAI, "rest health", 2) {}

    bool IsActive() override;
};

// Eating / drinking while full (a food aura runs up to 30 s) or in a fight: stand up.
class RestedTrigger : public Trigger
{
public:
    RestedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "rested") {}

    bool IsActive() override;
};

#endif
