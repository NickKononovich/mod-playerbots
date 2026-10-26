/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericDKStrategy.h"
#include "DKAiObjectContext.h"
#include "DKTriggers.h"
#include "Playerbots.h"

class GenericDKStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericDKStrategyActionNodeFactory()
    {
        creators["anti-magic zone"] = &anti_magic_zone;
        creators["death grip"] = &death_grip;
    }

private:
    static ActionNode* death_grip([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("death grip",
                              /*P*/ {},
                              /*A*/ { NextAction("icy touch") },
                              /*C*/ {});
    }

    static ActionNode* anti_magic_zone([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("anti-magic zone",
                              /*P*/ {},
                              /*A*/ { NextAction("anti-magic shell") },
                              /*C*/ {});
    }
};

GenericDKStrategy::GenericDKStrategy(PlayerbotAI* botAI) : MeleeCombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericDKStrategyActionNodeFactory());
}

void GenericDKStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    MeleeCombatStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode("mind freeze", { NextAction("mind freeze", ACTION_INTERRUPT + 2) }));
    triggers.push_back(
        new TriggerNode("mind freeze on enemy healer",
                        { NextAction("mind freeze on enemy healer", ACTION_INTERRUPT + 2) }));
    // Below Mind Freeze: absorb only what could not be interrupted.
    triggers.push_back(
        new TriggerNode("anti-magic shell", { NextAction("anti-magic shell", ACTION_INTERRUPT + 1) }));
    triggers.push_back(new TriggerNode(
        "horn of winter", { NextAction("horn of winter", ACTION_NORMAL + 1) }));
    // [b31_dk] Icebound Fortitude, then Anti-Magic Shell before the Raise Dead + Death Pact heal: Death Pact eats the
    // ghoul, which for Unholy (Master of Ghouls) is the permanent pet (CastDeathPactAction::isUseful).
    triggers.push_back(new TriggerNode("critical health",
                                       { NextAction("icebound fortitude", ACTION_HIGH + 7),
                                         NextAction("anti-magic shell", ACTION_HIGH + 6.5f),
                                         NextAction("raise dead", ACTION_HIGH + 6),
                                         NextAction("death pact", ACTION_HIGH + 5) }));
    // Battle res queue of the raid utility coordinator (client-driven players only, see CastRaiseAllyAction).
    triggers.push_back(
        new TriggerNode("combat party member dead", { NextAction("raise ally", ACTION_HIGH + 5) }));

    triggers.push_back(
        new TriggerNode("medium aoe", { NextAction("death and decay", ACTION_HIGH + 9),
                                                        NextAction("pestilence", ACTION_NORMAL + 4),
                                                        NextAction("blood boil", ACTION_NORMAL + 3) }));
    triggers.push_back(
        new TriggerNode("pestilence glyph", { NextAction("pestilence", ACTION_HIGH + 9) }));
    // [b35_uh] Empower Rune Weapon per spec: "no rune" (any fight) for Blood (BloodDKStrategy), "no rune boss" for
    // Frost, "empower rune weapon under gargoyle" for Unholy. Here it burnt the 5 min cooldown on trash for Frost and
    // Unholy (DEEP_AUDIT_DK_UNHOLY_NIGHT.md B3).
}

float DKDiseaseRefreshMultiplier::GetValue(Action* action)
{
    // [b31_dk] the name first: every other action leaves at once
    if (!action)
        return 1.0f;

    std::string const name = action->getName();
    char const* disease;
    char const* otherDisease;
    if (name == "icy touch")
    {
        disease = "frost fever";
        otherDisease = "blood plague";
    }
    else if (name == "plague strike")
    {
        disease = "blood plague";
        otherDisease = "frost fever";
    }
    else
        return 1.0f;

    // [b31_deep] Unholy: only with Scourge Strike to keep the runes for (a cached spell id lookup)
    if (requireScourgeStrike && !AI_VALUE2(uint32, "spell id", "scourge strike"))
        return 1.0f;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return 1.0f;

    Aura* aura = botAI->GetAura(disease, target, true, true);
    if (!aura)
        return 1.0f;  // ours is missing: apply it

    int32 const remaining = aura->GetDuration();
    if (remaining > DK_DISEASE_REFRESH_MS)
        return 0.0f;  // a stale queued cast on a fresh disease

    // the Glyph of Disease refresh: Pestilence needs both of ours on the target (CastPestilenceAction::isUseful)
    if (!bot->HasAura(SPELL_DK_GLYPH_OF_DISEASE) || !botAI->GetAura(otherDisease, target, true, true) ||
        !bot->IsWithinMeleeRange(target))
        return 1.0f;

    uint32 const pestilence = AI_VALUE2(uint32, "spell id", "pestilence");
    if (!pestilence || bot->HasSpellCooldown(pestilence))
        return 1.0f;

    // a Blood or Death rune for it before the disease runs out (MAX_RUNES = 6)
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        RuneType const type = bot->GetCurrentRune(i);
        if ((type == RUNE_BLOOD || type == RUNE_DEATH) && int32(bot->GetRuneCooldown(i)) < remaining)
            return 0.0f;
    }

    return 1.0f;
}
