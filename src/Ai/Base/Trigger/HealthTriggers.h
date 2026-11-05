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
#include <string>  // [lead1390]
#include <vector>  // [lead1390]

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
// [lead264] A mana user's spend per second while none is measured is its spell data's (SpellManaRate), not the
// role medians of 2026-10-24 (caster 1.3, healer 0.6, hunter 1.5, melee 1.5, tank 1.4 %/s).
// The other mana users (2026-10-24, *.bak_drink2): a hunter sat 18 s "to drink" at 47 % before a one-mob pull (Utgarde
// Keep heroic). Measured the same way, every kept log of 2026-10-24 scoped per harness run (names are reused across
// runs), full -> next drink: Marksmanship Hunter median 1.53 %/s (pooled 1.35, N 844), Enhancement Shaman 1.56 (1.20,
// N 778), Prot Paladin tank 1.39 (1.08, N 983). Retribution Paladin and feral druid have no window in these logs (no
// drink after a [pull check] from full): they take the melee rate. false restores the fixed lines for these roles.
constexpr bool PREDICT_DRINK_ALL_ROLES = true;
// [lead258] What a healer heals, from its spell data and its measured fights: the best rate of a heal it knows (the
// heal's amount with its spell power on the target, over the longest of its cast, the global cooldown, its own
// cooldown and a heal over time's duration), the most health per mana of one, and the time between two heals of
// that rate landing. `healPerMana`: the dungeon's measured effective healing per mana spent once a fight measured it.
struct HealerFacts
{
    float hps = 0.0f;
    float healPerMana = 0.0f;
    float gapSec = 0.0f;
    float maxMana = 0.0f;
    bool measured = false;  // healPerMana (and hps, if higher) from this dungeon's fights
    float manaPctPerSec = 0.0f;  // [lead264] the best rate's heal cast on and on: % of max mana per second
    std::string spell;      // the best rate's heal (logged)
    float castPct = 0.0f;   // [lead1400] the best rate's heal: one cast's cost, % of max mana
};
HealerFacts HealerThroughput(Player* healer, Unit* target);
// [lead258] the leader notes a healer's effective healing and its net mana spent over a fight in this dungeon
void NoteHealing(Player* healer, float healed, float manaSpent, float fightSec);
// [lead258] a healer's mana (% of its max) per tank health it heals: the group tank's health over its health per mana
float HealerPctPerTankHealth(Player* healer);
// [lead264] A member's mana spend from its spell data while nothing is measured (% of max mana per second): the mana
// per second of its best heal (a healer) or of its best damage spell that costs mana, each cast at the fastest the
// spell comes again; 0 for a bot whose spells cost no mana.
float SpellManaRate(Player* bot);
// [lead417] the cheapest mana cast of the bot's rotation (% of its max mana): an offensive spell, a healer's heal;
// below it the bot is dry (it cannot cast, its mana spend stops)
float CheapestCastPct(Player* bot);
// [lead810] the most mana (% of its max) the bot's spells can spend per second it acts: over its rotation's mana spells
// (an offensive spell, a healer's heal) the cost over the longest of its cast (a channel's duration), its global
// cooldown and its own cooldown - game data. A measured rate above it is a measuring error
float MaxSpendPctPerSec(Player* bot);
// [lead855] the mana (% of its max) a damage caster's rotation spends per second when it casts it nonstop (a boss
// fight, wowsims): its filler (the single-target spell with no cooldown of its own that deals the most per cast
// second, at its own cost stacks' average over their ramp) in the time its damage-over-time spells' casts leave, plus
// each of those (cost over its duration), net of the casts its free-cast procs (Clearcasting, Elemental Focus) save
// and of its in-combat regen, its ready boss burns (Arcane Power's cost, Icy Veins' haste) over the share of a fight
// of `fightSec` they last - spell data and the member's own stats. -1: not computed (a healer, no damage caster, a
// warlock: Life Tap is its rotation's own mana)
float RotationSpendPctPerSec(Player* bot, float fightSec, std::string* why = nullptr);
// [lead855] the rate a boss line reads from RotationSpendPctPerSec: while no boss fight of the member is measured
// (lead610), else -1
float BossRotationRate(Player* bot, float fightSec, std::string* why = nullptr);
// [lead990] the leader notes its group's estimate of the seconds to the next boss pull (bossPullSec's parts, the one
// its trash cooldown verdict reads; -1: no boss left): a trash line counts a mana tool of the member's own only when
// that verdict would give it on trash (back before that pull)
// [lead1083] `bossLeft` false: no boss left (its own state, not a -1 estimate)
void NoteBossPullSec(ObjectGuid group, float sec, bool bossLeft = true);
// [lead990] that estimate now (the seconds since it was noted taken off), -1: none known / no boss left
float BossPullSecOf(Player* bot);
// [lead1333] the leader notes the seconds its group waits before its next pull anyway besides the members' rests:
// the buff pass's casts still to give (each giver's buffs x its cast or global cooldown, the slowest giver); the
// ready check adds none (the bots answer it on receipt, LeaderBuffing READY_REPLY_ON_RECEIPT)
void NoteGroupWaitSec(ObjectGuid group, float sec);
// [lead1333] that wait now (the seconds since it was noted taken off), 0: none known
float GroupWaitSecOf(Player* bot);
// [lead1182] the member's last own boss line (FightManaLine out of a fight) was pulled short: its kill does not
// need the mana it lacks (the fight's longer seconds cost the group less than its drink)
bool BossLinePulledShort(Player* bot);
// [lead1083] the leader noted that no boss is left for the bot's group (false: a boss is left, or nothing noted)
bool NoBossLeftOf(Player* bot);
// [lead421] the mana per second (% of max mana) the best drink in the bot's bags gives; 0 without one
float DrinkPctPerSecOf(Player* bot);
// [lead264] the leader notes a finished fight's length and the mobs that came, for its group in this dungeon
void NoteFightLength(Player* tank, uint32 mobs, float sec);
// [lead1043] the fight seconds the group noted in this dungeon so far (a new fight changes it)
float FightSecNoted(Player* bot);
// [lead264] the next fight's length: the plan's estimate, else the group's measured seconds per mob in this dungeon
// times its mobs, else the game's: a creature of the bot's level (its base health) per mob over the group's
// estimated damage.
struct NextFight;
float FightSecOf(Player* bot, NextFight const& fight);
// [lead264] Left over after the predicted fight: one more mob's fight at the member's rate (FightManaLine's flat 20 /
// 25 % and the 75 % trash line cap are gone).
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
// [lead232] `boss`: a boss fight - the member's rate measured on boss fights, else its rate over its full-length fights
// (its whole rotation, lead234), else the rate of all fights
// [lead423] `credits` false: without the mana its own ready cooldowns return (the leader's mana tools judge the need
// they would cover)
// [lead482] `potion` false: without the mana potion the game lets it drink in the fight (the potion verdict itself)
// [lead1294] `damageCost` true: `cost` is the tank damage the fight deals in tank healths (BossFightCost: the game's
// data scaled by the measured intake), so a healer's heal term reads its measured mana per tank health it healed
// (HealerPctPerTankHealth), not its use per tank health of the plan's cost (a different unit)
float FightManaLine(Player* bot, float fightSec, float cost, std::string* why = nullptr, bool boss = false,
                    bool credits = true, bool potion = true, bool damageCost = false);
// [lead520] a boss fight of `fightSec` needs the member's in-fight mana potion: its mana is under the boss line without
// the potion (then it keeps the potions' cooldown for it and drinks no pre-pull potion)
// [lead1362] `cost`: a healer's heal cost in tank healths (the boss line's, BossFightCost)
bool NeedsManaPotion(Player* bot, float fightSec, float cost = 0.0f);
// [lead423] the mana tools (Innervate, Shadowfiend, Hymn of Hope, Mana Tide Totem, Evocation, Divine Plea, Dispersion)
constexpr uint32 SPELL_INNERVATE = 29166;
constexpr uint32 SPELL_SHADOWFIEND = 34433;
// [lead1380] the enemy `caster`'s Shadowfiend reaches from where it stands (its range with the caster's spell mods,
// spell data, and its sight): `preferred` when it does, else the nearest mob attacking a member of its group;
// nullptr: none (the summon's cast needs a hostile unit there, Spell::CheckCast)
Unit* ShadowfiendReach(Player* caster, Unit* preferred = nullptr);
constexpr uint32 SPELL_HYMN_OF_HOPE = 64901;
constexpr uint32 SPELL_MANA_TIDE_TOTEM = 16190;
constexpr uint32 SPELL_EVOCATION = 12051;
// [lead633] [lead850] the wowsims arcane APL channels Evocation at currentManaPercent <= 25 % (the leader's in-fight
// Evocation verdict and the boss line's timeline read the same line)
constexpr float WOWSIMS_EVOCATION_MANA_PCT = 25.0f;
// [lead1327] the wowsims fire APL has no Evocation row: Evocation is its automatic major cooldown (wowsims
// mage/evocation.go): at mana below 10 %, more than 12 s of the fight left, no Innervate on the mage. Rawr.Mage
// (Sequence.cs / CalculationsMage.cs) places it by its LP solver at the end of the pool, no fixed line
constexpr float WOWSIMS_EVOCATION_AUTO_MANA_PCT = 10.0f;
constexpr float WOWSIMS_EVOCATION_AUTO_MIN_LEFT_SEC = 12.0f;
// [lead1327] the mana line a mage's Evocation is channelled at, by spec (fire: the automatic cooldown's line; arcane
// and frost: the APL's 25 %)
float EvocationUseAtPct(Player* bot);
// [lead920] Omen of Clarity procs Clearcasting on 6 % of the spell casts (Rawr 2.3.23 Rawr.Moonkin
// SpellRotation.cs:197, 356); its spell_proc row reads Chance 100 - the core's spell_dru_omen_of_clarity CheckProc
// rolls the 6 % itself
constexpr uint32 SPELL_OMEN_OF_CLARITY = 16864;
constexpr float OMEN_OF_CLARITY_SPELL_PROC_PCT = 6.0f;
constexpr uint32 SPELL_DISPERSION = 47585;
constexpr uint32 SPELL_DISPERSION_MANA = 60069;   // [lead425] Dispersion's linked mana aura (6 % a second, 6 s)
constexpr uint32 SPELL_MANA_TIDE_TICK = 39610;    // [lead425] the totem's tick (spell_sha_mana_tide_totem)
constexpr uint32 SPELL_HYMN_OF_HOPE_TICK = 64904;  // [lead425] Hymn's tick: its aura on the members it reaches
constexpr uint32 HYMN_OF_HOPE_TARGETS = 3;        // [lead425] spell_pri_hymn_of_hope: the 3 lowest on mana
constexpr uint32 SPELL_ASPECT_OF_THE_VIPER = 34074;
// [lead440] wowsims' hunter APL lines in a fight (b31_deep): Viper below 10 % mana, the attack aspect again at 30 %
constexpr float VIPER_START_MANA_PCT = 10.0f;
constexpr float VIPER_STOP_MANA_PCT = 30.0f;
// [lead425] the radius a group mana tool reaches its receivers in around its caster (Mana Tide: its tick's area,
// spell data); 0: not a group tool
float ManaToolRadius(Player* caster, uint32 spell);
// [lead423] the mana (% of the receiver's max) `caster`'s `spell` returns to `receiver`, from spell and game data: an
// energize (% or flat) over the aura's ticks, the spells it triggers, a totem's aura over the totem's life, Innervate's
// 225 % of the druid's base mana (spell_dru_innervate), the Shadowfiend's hits (its life over its attack time) each
// returning its Mana Leech (34650); 0: none known
float ManaToolPct(Player* caster, Player* receiver, uint32 spell);
// [lead1390] a mana tool or a mana potion / gem the member's last mana line with credits (FightManaLine, not nested
// in another member's) counted as ready now: its spell (a tool) or its item (a potion, a gem). The leader's
// invariant check (credit_unguaranteed) asks the game whether each one's use is possible now
struct ManaLineCredit
{
    uint32 spell = 0;
    uint32 item = 0;
    std::string name;
};
std::vector<ManaLineCredit> ManaLineCredits(ObjectGuid member);
// [lead1421] the mana potion (% of max mana, at most the line's shortfall) the member's last boss line out of combat
// counted on - the boss gate's credit for the fight it lets the member into; 0: none
float BossPotionCredit(ObjectGuid member);
// [lead51] the leader notes a member's measured mana spend in fights (% of max mana per second of fight)
void NoteManaRate(ObjectGuid member, float pctPerSec);
// [lead232] ... on boss fights, and over its fights at least as long as this run's mean fight (lead234)
void NoteBossManaRate(ObjectGuid member, float pctPerSec);
void NotePeakManaRate(ObjectGuid member, float pctPerSec);
// [lead520] ... per second it acted in its fights (casting, channelling, on its global cooldown): its whole rotation's
// rate, a boss fight's before one is measured
void NoteActingManaRate(ObjectGuid member, float pctPerSec);
// [lead610] ... per second it acted in its boss fights only: its boss rotation's rate (a boss line reads it first)
void NoteBossActingManaRate(ObjectGuid member, float pctPerSec);
// [lead393] the leader notes a healer's measured net mana use (% of max) per tank health of the plan's fight cost:
// FightManaLine's heal term reads it in place of the throughput model (HealerPctPerTankHealth) once measured
void NoteHealerPctPerCost(ObjectGuid member, float pctPerCost);
// [lead367] the leader notes a measured fight of `member` under Divine Plea: its measured rate has Plea's mana in it
constexpr uint32 SPELL_DIVINE_PLEA = 54428;
void NoteDivinePleaInFight(ObjectGuid member);
// [lead88] the leader notes how far a fight's real length was from its estimate (seconds, either way) for its group;
// FightManaLine's reserve is the spend over that measured uncertainty (the mean of this run's fights)
// [lead362] the estimate and the real length: FightSecOf scales the plan's estimate by the group's measured fights
// (lead363: the least-squares scale), and the uncertainty is the mean overrun past that scaled estimate
void NoteFightLengthError(ObjectGuid group, float estimatedSec, float realSec);
// [lead74] a tank's mana line for its next fight: FightManaLine for the fight plus the cost of the pick-ups it may need
// (the fight's mobs beyond the first, each one taunt: the cheapest known taunt's real mana cost); one line for the
// planner's tank hold and the tank's rest. `why`: the terms
float TankManaLine(Player* tank, NextFight const& fight, std::string* why = nullptr);

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
// [lead116] the health a bot rests to: a led group's tank needs its next fight's cost in tank healths (the planner's
// estimate, NextFight::cost), every other bot (or no known fight) the fixed REST_HEALTH_PCT
float HealthLine(Player* bot);
// [lead59] the member's own lines (the computed line for its next fight), without the leader's Rest command: what the
// leader's Rest is judged by (it stops at the line, not at full)
bool BelowOwnLine(Player* bot, std::string* why = nullptr);
// [lead85] the member's own computed mana line for its next fight (the led group's FightManaLine / TankManaLine), the
// fixed rest line when no next fight is known - the hunter's Viper reads it
float OwnManaLine(Player* bot, std::string* why = nullptr);
// [lead47] the leader's Rest command is on this led bot
bool LeaderRests(Player* bot);
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
