/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BloodDKStrategy.h"
#include "DKTriggers.h"  // [b31_deep] SPELL_DK_RUNE_STRIKE
#include "Playerbots.h"
#include "RezzedTankHoldMultiplier.h"
#include "TargetValue.h"  // [b36_tankaoe] HasEnoughAoeTargets
#include "Timer.h"        // [b36_tankaoe]

#include <algorithm>  // [b36_tankaoe]

class BloodDKStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    BloodDKStrategyActionNodeFactory()
    {
        creators["rune strike"] = &rune_strike;
        creators["heart strike"] = &heart_strike;
        creators["death strike"] = &death_strike;
        creators["icy touch"] = &icy_touch;
        creators["dark command"] = &dark_command;
        creators["taunt spell"] = &dark_command;
    }

private:
    static ActionNode* rune_strike([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "rune strike",
            {
                NextAction("frost presence")
            },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* icy_touch([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "icy touch",
            {
                NextAction("frost presence")
            },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* heart_strike([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "heart strike",
            {
                NextAction("frost presence")
            },
            /*A*/ {
                NextAction("blood strike")
            },
            /*C*/ {}
        );
    }

    static ActionNode* death_strike([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "death strike",
            {
                NextAction("frost presence")
            },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* dark_command([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "dark command",
            {
                NextAction("frost presence")
            },
            /*A*/ {
                NextAction("death grip")
            },
            /*C*/ {}
        );
    }
};

BloodDKStrategy::BloodDKStrategy(PlayerbotAI* botAI) : GenericDKStrategy(botAI)
{
    actionNodeFactories.Add(new BloodDKStrategyActionNodeFactory());
}

std::vector<NextAction> BloodDKStrategy::getDefaultActions()
{
    return {
        // Icy Touch only through the disease triggers: as a filler it spent the frost runes Death Strike needs
        NextAction("rune strike", ACTION_DEFAULT + 0.6f),
        NextAction("heart strike", ACTION_DEFAULT + 0.5f),
        NextAction("death strike", ACTION_DEFAULT + 0.4f),
        // [rot1] Dancing Rune Weapon moved to the "dancing rune weapon ready" trigger (above Rune Strike)
        NextAction("death coil", ACTION_DEFAULT + 0.2f),
        NextAction("horn of winter", ACTION_DEFAULT + 0.1f),
        NextAction("melee", ACTION_DEFAULT)
    };
}

void BloodDKStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericDKStrategy::InitMultipliers(multipliers);
    multipliers.push_back(new RezzedTankHoldMultiplier(botAI));
    multipliers.push_back(new DKDiseaseRefreshMultiplier(botAI));  // [b31_dk]
    multipliers.push_back(new BloodDeathCoilMultiplier(botAI));    // [b31_deep]
    multipliers.push_back(new BloodDnDRuneReserveMultiplier(botAI));  // [b36_tankaoe]
}

namespace
{
// [b36_tankaoe] Death and Decay's cost covered by the ready runes: one Blood, Frost and Unholy, Death runes for any.
uint8 DnDRunesCovered(uint8 const (&ready)[NUM_RUNE_TYPES])
{
    uint8 covered = 0;
    covered += ready[RUNE_BLOOD] ? 1 : 0;
    covered += ready[RUNE_FROST] ? 1 : 0;
    covered += ready[RUNE_UNHOLY] ? 1 : 0;
    uint8 const missing = 3 - covered;
    return covered + std::min<uint8>(missing, ready[RUNE_DEATH]);
}

// Pays `cost` from the matching runes, then Death runes; false when the runes do not cover it.
bool PayRunes(uint8 (&ready)[NUM_RUNE_TYPES], RuneType type, uint8 cost)
{
    uint8 const own = std::min<uint8>(cost, ready[type]);
    ready[type] -= own;
    uint8 const rest = cost - own;
    if (rest > ready[RUNE_DEATH])
        return false;

    ready[RUNE_DEATH] -= rest;
    return true;
}
}  // namespace

float BloodDnDRuneReserveMultiplier::GetValue(Action* action)
{
    // the name first (string compares), the fight state, then the cached spell id and cooldown, the mob count last
    if (!action)
        return 1.0f;

    std::string const& name = action->getName();
    uint8 blood = 0;
    uint8 frost = 0;
    uint8 unholy = 0;
    if (name == "icy touch")
        frost = 1;
    else if (name == "plague strike")
        unholy = 1;
    else if (name == "death strike")
        frost = unholy = 1;
    else if (name == "heart strike" || name == "blood strike")
        blood = 1;
    else
        return 1.0f;

    if (!bot->IsInCombat() || !PlayerbotAI::IsTank(bot))
        return 1.0f;

    if (frost && unholy && bot->GetHealthPct() < DEATH_STRIKE_FREE_HEALTH_PCT)
        return 1.0f;

    uint32 const dndId = AI_VALUE2(uint32, "spell id", "death and decay");
    if (!dndId || !bot->HasSpell(dndId))
        return 1.0f;

    uint32 const cooldown = bot->HasSpellCooldown(dndId) ? bot->GetSpellCooldownDelay(dndId) : 0;
    if (cooldown > READY_WINDOW_MS)
        return 1.0f;

    uint8 ready[NUM_RUNE_TYPES] = {};
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (!bot->GetRuneCooldown(i))
            ++ready[bot->GetCurrentRune(i)];
    }

    uint8 const before = DnDRunesCovered(ready);
    // [rot14] no clock: Death and Decay ready with all its runes and still not cast (its trigger ranks above every
    // spender, so it did not want it this tick: immune, out of sight) frees the runes
    if (!cooldown && before >= DND_RUNE_TYPES)
        return 1.0f;

    if (!PayRunes(ready, RUNE_BLOOD, blood) || !PayRunes(ready, RUNE_FROST, frost) ||
        !PayRunes(ready, RUNE_UNHOLY, unholy))
        return 1.0f;  // not payable: the cast check refuses it anyway

    if (DnDRunesCovered(ready) >= before)
        return 1.0f;  // a spare rune

    return HasEnoughAoeTargets(botAI, bot->GetPosition(), PILE_RANGE, MIN_MOBS) ? 0.0f : 1.0f;
}

float BloodDeathCoilMultiplier::GetValue(Action* action)
{
    // [b31_deep] name and runic power first, the Rune Strike lookup (a hash read) last
    if (!action || bot->GetPower(POWER_RUNIC_POWER) >= BLOOD_DEATH_COIL_RUNIC_POWER ||
        action->getName() != "death coil")
        return 1.0f;

    return bot->HasSpell(SPELL_DK_RUNE_STRIKE) ? 0.0f : 1.0f;
}

void BloodDKStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericDKStrategy::InitTriggers(triggers);
    // [lead96] a mob coming at the group with no tank threat: the ranged opener on it (TankOpenIncomingAction)
    // [lead106] above the tank's own target / facing / marks actions (tank assist 40, set facing 37): taking a
    // mob back from another member is the most urgent thing a tank does
    triggers.push_back(
        new TriggerNode("tank incoming mob", { NextAction("tank open incoming", ACTION_INTERRUPT + 1.0f) }));

    // [b31_dk] Frost Presence, the 3.3.5 tank presence (armor, threat, less damage taken) the rotation's
    // prerequisites already ask for; above movement and peel, below interrupts.
    triggers.push_back(new TriggerNode("no presence", { NextAction("frost presence", ACTION_MOVE + 9) }));

    // [b35_uh] moved here from GenericDKStrategy, unchanged: the tank spends Empower Rune Weapon in any fight once
    // every rune is down (wowsims blood_aggro: on cooldown; blood_dps: with no rune of any type left)
    // [rot1] in a boss fight (IsDKBossFight, NoRuneBossTrigger as Frost): spent on trash the 5 min cooldown met the
    // boss on cooldown (sim 1.28/min, bots 0.39; 246 "on cooldown" refusals in the 2026-10-26 logs). Elsewhere only as
    // the tank's last defence, with every rune down below low health.
    triggers.push_back(new TriggerNode("no rune boss", { NextAction("empower rune weapon", ACTION_HIGH + 1) }));
    triggers.push_back(new TriggerNode("no rune low health", { NextAction("empower rune weapon", ACTION_HIGH + 1) }));

    // [rot1] Dancing Rune Weapon on cooldown above Rune Strike (13), Death Strike and Heart Strike (wowsims
    // blood_aggro: Rune Strike only while it is not ready or at 80+ runic power). As a default action (+0.3) it ran
    // only when Rune Strike, Heart Strike and Death Strike were all impossible (0.89/min against the sim's 1.28).
    triggers.push_back(
        new TriggerNode("dancing rune weapon ready", { NextAction("dancing rune weapon", ACTION_NORMAL + 3.5f) }));

    // [b44_cast] Raise Dead on cooldown in a boss / long elite fight (wowsims blood_aggro: after Death Strike and Heart
    // Strike, before Empower Rune Weapon and Death Coil), the Blood template's Glyph of Raise Dead needing no corpse
    // (RaiseDeadBoostTrigger, as Frost). Below Dancing Rune Weapon / Death Strike / Heart Strike (+0.3..+0.5), above
    // Death Coil (+0.2). Sim A/B on the bots' 13 counted Blood boss fights: no Raise Dead 2812 DPS vs 2974 (-5.4 %).
    // The critical-health Raise Dead + Death Pact then uses this ghoul while it lives.
    // [rot1] a boss fight or the leader's burn window only (RaiseDeadBossTrigger): the 3 min cooldown is not spent on
    // a long trash pack before the boss
    triggers.push_back(new TriggerNode("raise dead boss", { NextAction("raise dead", ACTION_DEFAULT + 0.25f) }));

    // [rot1] Hysteria likewise (HysteriaCooldownFightTrigger, 3 min): bots 0.30/min against the sim's 1.28
    triggers.push_back(
        new TriggerNode(
            "hysteria boss",
            {
                NextAction("hysteria", ACTION_NORMAL + 4)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "rune strike",
            {
                NextAction("rune strike", ACTION_NORMAL + 3)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "blood tap",
            {
                NextAction("blood tap", ACTION_HIGH + 5)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "lose aggro",
            {
                NextAction("dark command", ACTION_HIGH + 3)
            }
        )
    );
    // Peel: take back a mob hitting the healer or a damage dealer, also when it is not the current target.
    // Above "set facing" (ACTION_MOVE + 7) and "tank face" (ACTION_MOVE), as the warrior's taunt.
    // [rot151] every step needs the mob in reach and sight: the trigger fires on the mob, not on the friend alone
    triggers.push_back(
        new TriggerNode(
            "mob in reach attacking group member",
            {
                NextAction("dark command on group member attacker", ACTION_INTERRUPT + 0.5f),
                // [b32_peel] Death Grip brings the mob (a caster at range too) to the tank: no gap-closer gate
                NextAction("death grip on group member attacker", ACTION_INTERRUPT + 0.4f),
                // both on cooldown: Icy Touch carries the presence's threat at range
                NextAction("icy touch on group member attacker", ACTION_INTERRUPT + 0.3f),
                // a loose mob in melee (any, not only the pick): Heart Strike's threat by hand
                NextAction("heart strike on group member attacker", ACTION_INTERRUPT + 0.25f)
            }
        )
    );
    // A pack around the tank with a mob held by proximity aggro alone (the first heal takes it) or on a member: area
    // threat as the pull lands and while one is loose. Below the peel steps, above "tank assist".
    triggers.push_back(
        new TriggerNode(
            "tank aoe threat",
            {
                NextAction("death and decay", ACTION_INTERRUPT + 0.2f),
                // [b32_tank_aoe] Pestilence before Blood Boil (both a Blood rune): at +0.15 under Blood Boil it never
                // ran on the scholo-n 13-mob pull (Blood Boil 8, Pestilence 0, Icy Touch 11). It spreads the diseases
                // (and their threat) from the target, and Blood Boil hits harder on diseased mobs; useless without
                // both diseases on the target (CastPestilenceAction::isUseful), then Blood Boil takes the rune.
                NextAction("pestilence", ACTION_INTERRUPT + 0.19f),
                NextAction("blood boil tank aoe", ACTION_INTERRUPT + 0.18f)
            }
        )
    );
    // [rot172] "untagged tank mob" (TankAoeThreat.h): a pack mob out of the area reach, on the tank by its first aggro
    // only, below the area-threat floor -> the ranged threat spell tags it before the first heal's split threat takes
    // it (b215 rfc-h 5D: Searing Blade Warlocks 23-25 yd off at 3 / 34 bear threat went to the shaman). Below the
    // area threat and the peel steps, above "tank assist".
    triggers.push_back(new TriggerNode(
        "untagged tank mob", { NextAction("icy touch on untagged mob", ACTION_INTERRUPT + 0.12f) }));
    // [b32_tank_aoe] "tank aoe pack" (TankAoeThreat.h): 3+ mobs in reach and one without a safe threat lead -> the area
    // spell above the single-target builders until the lead is safe; then the single-target order below resumes.
    // Above Death Strike (ACTION_HIGH + 3), Icy Touch / Plague Strike (+2) and Heart Strike (default); below Blood Tap.
    triggers.push_back(new TriggerNode("tank aoe pack",
        {
            NextAction("pestilence", ACTION_HIGH + 3.6f),
            NextAction("death and decay", ACTION_HIGH + 3.5f),
            NextAction("blood boil tank aoe", ACTION_HIGH + 3.4f)
        }));
    // [b32_peel] "tank aoe crowd" (TankAoeThreat.h): more than 3 mobs in reach, one not held -> the area threat above
    // every peel step (+0.25..+0.5) and "taunt reaction" (TankPeel::CrowdFirst); off while a mob is on the healer.
    triggers.push_back(new TriggerNode("tank aoe crowd",
        {
            NextAction("pestilence", ACTION_INTERRUPT + 0.63f),
            NextAction("death and decay", ACTION_INTERRUPT + 0.62f),
            NextAction("blood boil tank aoe", ACTION_INTERRUPT + 0.6f)
        }));
    // [b33_aoe_class] 3+ mobs on the tank's target pile ("medium aoe", 8 yd), threat lead safe or not: Death and Decay
    // above the Icy Touch peel (+0.3), which spent the frost rune it needs (hor-n-5G-27 escape, 299 s with 5-15 adds:
    // Death and Decay 4, Icy Touch 29; GenericDKStrategy's 29.0 lost every rune race). Pestilence and Blood Boil
    // above Death Strike (+3), Icy Touch / Plague Strike (+2) and Heart Strike, as "tank aoe pack".
    triggers.push_back(new TriggerNode("medium aoe",
        {
            NextAction("death and decay", ACTION_INTERRUPT + 0.35f),
            NextAction("pestilence", ACTION_HIGH + 3.6f),
            NextAction("blood boil tank aoe", ACTION_HIGH + 3.4f)
        }));
    // One mob within Blood Boil's reach is held by it while the taunts cool down (two or more: below).
    triggers.push_back(
        new TriggerNode(
            "mob attacking group member nearby",
            {
                NextAction("blood boil on group member attackers", ACTION_MOVE + 2.4f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "group member attacker near",
            {
                NextAction("reach group member attacker", ACTION_MOVE + 0.9f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "mobs attacking group members nearby",
            {
                NextAction("death and decay", ACTION_MOVE + 2.5f),
                NextAction("blood boil on group member attackers", ACTION_MOVE + 2.4f)
            }
        )
    );
    // Two or more adds out of reach of the area threat: step towards them (what the tank holds follows) and take
    // them with the AoE of "mobs attacking group members nearby"; reach melee brings the tank back afterwards.
    triggers.push_back(
        new TriggerNode(
            "group member attackers far",
            {
                NextAction("move to group member attackers", ACTION_MOVE + 1)
            }
        )
    );
    // A caster or a runner holding at range comes to the tank; above "reach melee", below the peel steps.
    triggers.push_back(
        new TriggerNode(
            "death grip far target",
            {
                NextAction("death grip", ACTION_MOVE + 0.8f)
            }
        )
    );
    // Above the rotation and "tank face" (ACTION_MOVE), below the peel steps and area threat.
    triggers.push_back(
        new TriggerNode(
            "enemy out of melee",
            {
                NextAction("reach melee", ACTION_MOVE + 0.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "low health",
            {
                NextAction("death strike", ACTION_HIGH + 3)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "rune tap health",
            {
                NextAction("rune tap", ACTION_EMERGENCY + 1),
                // [bear_aoe_threat] no Blood rune for it: Blood Tap activates one (off the global cooldown)
                NextAction("blood tap for defense", ACTION_EMERGENCY + 0.9f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "vampiric blood health",
            {
                NextAction("vampiric blood", ACTION_EMERGENCY + 2),
                NextAction("blood tap for defense", ACTION_EMERGENCY + 0.9f)  // [bear_aoe_threat]
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "icebound fortitude health",
            {
                NextAction("icebound fortitude", ACTION_EMERGENCY + 3)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "damage spike",
            {
                NextAction("icebound fortitude", ACTION_EMERGENCY + 3),
                NextAction("vampiric blood", ACTION_EMERGENCY + 2),
                // [rot_b31_pdd] Icebound Fortitude costs 20 runic power in 3.3.5 (48792: PowerType 6, cost 200), as
                // Anti-Magic Shell: on a pull with none (a body pull, a patrol) both fail. The rune-paid Rune Tap and
                // Vampiric Blood cover it, Blood Tap (off the global cooldown) frees a Blood rune for them.
                NextAction("rune tap", ACTION_EMERGENCY + 1),
                NextAction("blood tap for defense", ACTION_EMERGENCY + 0.9f)
            }
        )
    );
    // [b31_dk] "tank pressure" (below TankCooldownHealthPct and still falling) is the medium spike: Vampiric Blood and
    // Rune Tap. Icebound Fortitude stays for "icebound fortitude health" (35 %) and the big "damage spike" above, where
    // CastVampiricBloodAction does not add Vampiric Blood on top of it while health holds.
    triggers.push_back(
        new TriggerNode(
            "tank pressure",
            {
                NextAction("vampiric blood", ACTION_EMERGENCY + 2),
                // [b31_deep] on a boss (CastMarkOfBloodAction::isUseful): its hits heal the tank
                NextAction("mark of blood", ACTION_EMERGENCY + 1.5f),
                NextAction("rune tap", ACTION_EMERGENCY + 1),  // [rot_b31_pdd] as on "damage spike"
                NextAction("blood tap for defense", ACTION_EMERGENCY + 0.9f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "icy touch",
            {
                NextAction("icy touch", ACTION_HIGH + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "plague strike",
            {
                NextAction("plague strike", ACTION_HIGH + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "high unholy rune",
            {
                // [rot33] wowsims tank_deathknight__blood_aggro: Death Strike after Heart Strike (5.5); it sat at
                // ACTION_HIGH + 1, above Heart Strike, Rune Strike and Dancing Rune Weapon
                NextAction("death strike", ACTION_DEFAULT + 0.45f)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus vampiric blood tank",
        { NextAction("vampiric blood", ACTION_HIGH + 8) }));
    triggers.push_back(new TriggerNode("set bonus icebound fortitude tank",
        { NextAction("icebound fortitude", ACTION_HIGH + 7.9f) }));
    triggers.push_back(new TriggerNode("set bonus rune tap tank", { NextAction("rune tap", ACTION_HIGH + 7.5f) }));
    triggers.push_back(new TriggerNode("set bonus death strike ward",
        { NextAction("death strike", ACTION_HIGH + 1.5f) }));
}
