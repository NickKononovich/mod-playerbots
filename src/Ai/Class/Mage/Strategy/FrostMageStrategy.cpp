/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FrostMageStrategy.h"
#include "Playerbots.h"

FrostMageStrategy::FrostMageStrategy(PlayerbotAI* botAI) : GenericMageStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> FrostMageStrategy::getDefaultActions()
{
    std::vector<NextAction> actions = {
        NextAction("frostbolt", 5.4f)
    };
    // [rot640] Ice Lance is learned at 66 and was 100 of this spec's 308 "spell not known" refusals in the b483
    // window: the engine made its node and refused it on the same cached "spell id" (SpellIdValue, 20 s) this
    // reads, every tick of every mage below that level. The entry stays - only the pushing waits for the spell.
    // The line below it, Fire Blast, is already the movement shot "if ice lance is not learned".
    if (botAI->GetAiObjectContext()->GetValue<uint32>("spell id", std::string("ice lance"))->Get())
        actions.push_back(NextAction("ice lance", 5.3f));  // cast during movement
    actions.push_back(NextAction("fire blast", 5.2f));  // cast during movement if ice lance is not learned
    // [rot640] "shoot" is never gated: CastShootAction resolves the wand or ranged weapon spell itself
    // (shootSpellId 3018 / 2764 / 5019), not through the "spell id" value, so that value says nothing about it
    actions.push_back(NextAction("shoot", 5.1f));
    actions.push_back(NextAction("fireball", 5.0f));
    return actions;
}

// ===== Trigger Initialization ===
void FrostMageStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericMageStrategy::InitTriggers(triggers);

    // Pet/Defensive triggers
    triggers.push_back(
        new TriggerNode(
            "no pet",
            {
                NextAction("summon water elemental", 30.0f)
            }
        )
    );
    // [b31_deep] every 30 s instead of every 5 s ("has pet"): the permanent pet rarely needs it
    triggers.push_back(
        new TriggerNode(
            "water elemental autocast check",
            {
                NextAction("toggle pet spell", 60.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "new pet",
            {
                NextAction("set pet stance", 60.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium health",
            {
                NextAction("ice barrier", 29.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "being attacked",
            {
                NextAction("ice barrier", 29.0f)
            }
        )
    );

    // Proc/Freeze triggers
    triggers.push_back(
        new TriggerNode(
            "brain freeze",
            {
                NextAction("frostfire bolt", 19.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "fingers of frost",
            {
                NextAction("deep freeze", 19.6f),  // [b31_deep] above Brain Freeze FFB (19.5): DF first under FoF
                NextAction("frostbolt", 18.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "frostbite on target",
            {
                NextAction("deep freeze", 19.6f),  // [b31_deep] above Brain Freeze FFB (19.5): DF first under FoF
                NextAction("frostbolt", 18.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "frost nova on target",
            {
                NextAction("deep freeze", 19.6f),  // [b31_deep] above Brain Freeze FFB (19.5): DF first under FoF
                NextAction("frostbolt", 18.0f)
            }
        )
    );

    // [rot603] Fire Blast where it pays against the filler; [rot604] Fireball over Frostbolt where it deals more (just
    // above the Frostbolt default, below every trigger)
    triggers.push_back(new TriggerNode("fire blast pays", { NextAction("fire blast", ACTION_NORMAL + 5) }));
    triggers.push_back(new TriggerNode("fireball over frostbolt", { NextAction("fireball", ACTION_DEFAULT + 0.45f) }));

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus ice lance", { NextAction("ice lance", 18.5f) }));
    triggers.push_back(new TriggerNode("set bonus arcane surge", { NextAction("frostbolt", 18.4f) }));
}
