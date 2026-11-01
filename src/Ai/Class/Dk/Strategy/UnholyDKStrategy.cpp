/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UnholyDKStrategy.h"
#include "DKTriggers.h"
#include "Playerbots.h"

class UnholyDKStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    // [b31_deep] The Blood Presence prerequisites of Death Strike / Scourge Strike / Ghoul Frenzy / Corpse Explosion /
    // Icy Touch are gone: they switched Unholy Presence back on the next strike, before the Gargoyle. Blood Presence
    // comes from "no presence" and "unholy presence after gargoyle" (InitTriggers).
    UnholyDKStrategyActionNodeFactory() { creators["summon gargoyle"] = &summon_gargoyle; }

private:
    // [b31_deep] the Gargoyle in Unholy Presence (wowsims uh_2h_ss: Unholy Presence for it, Blood Presence after)
    static ActionNode* summon_gargoyle([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "summon gargoyle",
            /*P*/ { NextAction("unholy presence") },
            /*A*/ {},
            /*C*/ {}
        );
    }
};

UnholyDKStrategy::UnholyDKStrategy(PlayerbotAI* botAI) : GenericDKStrategy(botAI)
{
    actionNodeFactories.Add(new UnholyDKStrategyActionNodeFactory());
}

float GargoylePoolingMultiplier::GetValue(Action* action)
{
    // [rot_b31_pdd] name and runic power first; the fight test only for a Death Coil that would dip below 60
    if (!action || action->getName() != "death coil" ||
        bot->GetPower(POWER_RUNIC_POWER) >= GARGOYLE_RUNIC_POWER + DEATH_COIL_RUNIC_POWER)
        return 1.0f;

    return IsGargoyleWaiting(botAI) ? 0.0f : 1.0f;
}

float DeathAndDecayPoolingMultiplier::GetValue(Action* action)
{
    // [b35_uh] the name first, then field and cooldown map reads, the lifetime value last
    if (!action)
        return 1.0f;

    std::string const name = action->getName();
    bool const scourgeStrike = name == "scourge strike";
    bool const bloodStrike = name == "blood strike";
    if (!scourgeStrike && !bloodStrike && name != "blood boil")
        return 1.0f;

    if (!bot->IsInCombat())
        return 1.0f;

    uint32 const dnd = AI_VALUE2(uint32, "spell id", "death and decay");
    if (!dnd)
        return 1.0f;

    uint32 const delay = bot->GetSpellCooldownDelay(dnd);
    if (delay > DK_DND_POOL_MS)
        return 1.0f;

    // ready but not cast (threat hold, range, no target for it): hold only while its runes are missing
    if (!delay && HasReadyRunes(bot, 1, 1, 1))
        return 1.0f;

    if (!scourgeStrike)
    {
        // Empower Rune Weapon about to refill the runes ("empower rune weapon under gargoyle")
        if (bot->HasSpell(SPELL_DK_EMPOWER_RUNE_WEAPON) && !bot->HasSpellCooldown(SPELL_DK_EMPOWER_RUNE_WEAPON) &&
            IsGargoyleOut(bot))
            return 1.0f;

        // wowsims "Blood Strike if no Desolation" (66817: Desolation 5/5, as DesolationTrigger)
        if (bloodStrike && bot->HasAura(66817) && !botAI->GetAura("desolation", bot, false, true))
            return 1.0f;
    }

    // a target dying before Death and Decay could tick DK_DND_MIN_TICK_S is no Death and Decay fight
    float const lifetime = AI_VALUE2(float, "estimated lifetime", "current target");
    if (lifetime < static_cast<float>(delay) / IN_MILLISECONDS + DK_DND_MIN_TICK_S)
        return 1.0f;

    return 0.0f;
}

void UnholyDKStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericDKStrategy::InitMultipliers(multipliers);
    multipliers.push_back(new GargoylePoolingMultiplier(botAI));
    multipliers.push_back(new DKDiseaseRefreshMultiplier(botAI, true));  // [b31_deep] with Scourge Strike only
    multipliers.push_back(new DeathAndDecayPoolingMultiplier(botAI));    // [b35_uh]
}

std::vector<NextAction> UnholyDKStrategy::getDefaultActions()
{
    // [b31_deep] The rune spenders are default actions: they sat on "death and decay cooldown" (Death and Decay on
    // cooldown >= 2 s), so with Death and Decay ready but not castable (threat hold, range, a missing rune) none ran.
    // Death and Decay above them is still cast first whenever it can be (wowsims uh_2h_ss: by priority, no pooling).
    return {
        NextAction("death and decay", ACTION_HIGH + 5),
        NextAction("scourge strike", ACTION_DEFAULT + 0.8f),
        NextAction("icy touch", ACTION_DEFAULT + 0.7f),
        // [rot22] wowsims unholy_dw_ss: Blood Boil is the Blood-rune spender (single target too); Blood Strike only
        // for Desolation ("no desolation", 5.75); Horn of Winter last, under Death Coil and the Ghoul Frenzy pair
        NextAction("blood boil", ACTION_DEFAULT + 0.6f),
        NextAction("plague strike", ACTION_DEFAULT + 0.5f),
        NextAction("death coil", ACTION_DEFAULT + 0.1f),
        NextAction("horn of winter", ACTION_DEFAULT + 0.04f),
        NextAction("melee", ACTION_DEFAULT)
    };
}

void UnholyDKStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericDKStrategy::InitTriggers(triggers);

    // [b31_dk] Blood Presence for Unholy DPS too (the 3.3.5 standard outside special builds); above movement and
    // peel, below interrupts.
    triggers.push_back(new TriggerNode("no presence", { NextAction("blood presence", ACTION_MOVE + 9) }));

    // [b31_dk] A missing disease goes on above Death and Decay (default HIGH + 5), with no Death and Decay coupling:
    // the refreshes below wait for "death and decay cooldown", so with Death and Decay off cooldown but not castable
    // (a rune type missing, no ground target) no disease was applied at all. [b31_deep] The 3 s refreshes below are
    // uncoupled now too.
    triggers.push_back(new TriggerNode("icy touch", { NextAction("icy touch", ACTION_HIGH + 5.3f) }));
    triggers.push_back(new TriggerNode("plague strike", { NextAction("plague strike", ACTION_HIGH + 5.2f) }));

    triggers.push_back(
        new TriggerNode("no pet", { NextAction("raise dead", ACTION_NORMAL + 5) }));
    triggers.push_back(
        new TriggerNode("has pet", { NextAction("toggle pet spell", 60.0f) }));
    triggers.push_back(
        new TriggerNode("new pet", { NextAction("set pet stance", 60.0f) }));

    // [b31_deep] Unholy Presence after the Gargoyle: back to Blood Presence, as "no presence" above
    triggers.push_back(
        new TriggerNode("unholy presence after gargoyle", { NextAction("blood presence", ACTION_MOVE + 9) }));

    // [b31_deep] Ghoul Frenzy on our ghoul, Blood Tap first for the Death rune. [rot22] wowsims unholy_dw_ss: the
    // Blood Tap -> Ghoul Frenzy sequence sits under Death Coil (5.1), not above Scourge Strike
    triggers.push_back(new TriggerNode("ghoul frenzy", {NextAction("blood tap", ACTION_DEFAULT + 0.07f),
                                                        NextAction("ghoul frenzy", ACTION_DEFAULT + 0.06f)}));

    // [b31_deep] no Death and Decay coupling (was "dd cd and no desolation")
    triggers.push_back(
        new TriggerNode(
            "no desolation",
            {
                NextAction("blood strike", ACTION_DEFAULT + 0.75f)
            }
        )
    );
    // [melee_audit_b30] Two ready Frost or Unholy runes go to Scourge Strike (one Frost + one Unholy), the Unholy
    // spender. With Icy Touch / Plague Strike alone on these triggers (above every Scourge Strike slot) build 30 cast
    // Scourge Strike 2 times in 28.7 min at level 80 against 141 Icy Touch and 103 Plague Strike. Icy Touch /
    // Plague Strike stay as the fallback when the other rune type is down (Scourge Strike then fails its cast).
    triggers.push_back(
        new TriggerNode(
            "high frost rune",
            {
                NextAction("scourge strike", ACTION_NORMAL + 3.5f),
                NextAction("icy touch", ACTION_NORMAL + 3)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "high unholy rune",
            {
                NextAction("scourge strike", ACTION_NORMAL + 3.5f),
                NextAction("plague strike", ACTION_NORMAL + 1)
            }
        )
    );
    // [b31_deep] the 3 s refreshes without the Death and Decay coupling (were "dd cd and ...")
    triggers.push_back(
        new TriggerNode("plague strike 3s",
            {
                NextAction("plague strike", ACTION_HIGH + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode("icy touch 3s",
            {
                NextAction("icy touch", ACTION_HIGH + 2)
            }
        )
    );
    // [b35_uh] Army of the Dead in combat under the Gargoyle (wowsims unholy_dw_ss: no prepull Army). Was "army of the
    // dead" at HIGH + 6, above the Gargoyle, the diseases and Death and Decay: the first action of every boss pull
    // (DEEP_AUDIT_DK_UNHOLY_NIGHT.md B4). Now the APL's two slots: with Empower Rune Weapon spent and one Frost rune
    // left above Death and Decay (HIGH + 5) but below the missing diseases (5.2 / 5.3) and the Gargoyle (5.5); else
    // below Death and Decay and the 3 s disease refreshes (HIGH + 1 / + 2), above every rune spender.
    triggers.push_back(
        new TriggerNode("army of the dead last frost rune", { NextAction("army of the dead", ACTION_HIGH + 5.1f) }));
    triggers.push_back(
        new TriggerNode("army of the dead under gargoyle", { NextAction("army of the dead", ACTION_HIGH + 0.9f) }));
    // [b35_uh] Empower Rune Weapon under the Gargoyle on a boss, once the runes are spent (wowsims unholy_dw_ss), below
    // the Gargoyle (5.5) and the missing diseases, above Death and Decay (5) so the refilled runes go to it first
    triggers.push_back(new TriggerNode("empower rune weapon under gargoyle",
                                       { NextAction("empower rune weapon", ACTION_HIGH + 5.15f) }));
    // [rot22] no in-combat Bone Shield (wowsims unholy_dw_ss never casts it in the fight); out of combat it stays
    // [b31_dk] Above Death and Decay (default HIGH + 5, cast on cooldown on a single target as wowsims unholy does)
    // and the missing diseases: the trigger already waits for the Gargoyle fight (boss, player, burst window).
    triggers.push_back(
        new TriggerNode(
            "summon gargoyle",
            {
                NextAction("summon gargoyle", ACTION_HIGH + 5.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "low health",
            {
                NextAction("icebound fortitude", ACTION_HIGH + 5)
            }
        )
    );

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus ascendant power", { NextAction("death coil", ACTION_HIGH + 1) }));
}

void UnholyDKAoeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // [b31_dk] no Corpse Explosion on "loot available": it fired on any corpse, a single target included; it stays
    // on "medium aoe" below
    triggers.push_back(
        new TriggerNode(
            "medium aoe",
            {
                NextAction("death and decay", ACTION_NORMAL + 3),
                NextAction("corpse explosion", ACTION_NORMAL + 3)
            }
        )
    );
}
