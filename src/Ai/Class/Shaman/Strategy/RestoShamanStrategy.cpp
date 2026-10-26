/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RestoShamanStrategy.h"
#include "HealthTriggers.h"
#include "Playerbots.h"

RestoShamanStrategy::RestoShamanStrategy(PlayerbotAI* botAI) : GenericShamanStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// [healer_audit_b30] Facing turns, damage and cures on other members wait while one member is below 35 % (and
// cures on others below low mana), as for the other healers since [heal_triage b29].
void RestoShamanStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    GenericShamanStrategy::InitMultipliers(multipliers);

    multipliers.push_back(new HealerEmergencyMultiplier(botAI));
}

// ===== Trigger Initialization ===
void RestoShamanStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericShamanStrategy::InitTriggers(triggers);

    // [healer_audit_b30] EMERGENCY (one member below 35 %): Nature's Swiftness (off the GCD) for an instant Healing
    // Wave, the instant Riptide, then the fast Lesser Healing Wave; all above Chain Heal (26-28.5) and the critical
    // band (23-25). The build 29 diff (PATCH_healer_triage.md) had not been applied.
    // [b31_sd] Tidal Force (+60 % heal crit for the next heals, off the GCD) in the emergency too, after Nature's
    // Swiftness; before, it only answered "low aoe heal" (0.14/min on bosses).
    triggers.push_back(new TriggerNode("party member emergency health",
                                       { NextAction("nature's swiftness", ACTION_CRITICAL_HEAL),
                                         NextAction("tidal force", ACTION_CRITICAL_HEAL - 1.0f),
                                         NextAction("riptide on party", 28.8f),
                                         NextAction("lesser healing wave on party", 28.6f) }));
    // [healer_audit_b30] GROUP DAMAGE (3+ members below 70 %): Riptide on the first target, then Chain Heal.
    triggers.push_back(new TriggerNode("group damage", { NextAction("riptide on party", 28.5f),
                                                         NextAction("chain heal on party", 28.0f) }));

    // Totem Triggers
    triggers.push_back(new TriggerNode("call of the elements", { NextAction("call of the elements", 60.0f) }));
    triggers.push_back(new TriggerNode("low health", { NextAction("stoneclaw totem", ACTION_LIGHT_HEAL + 2.5f) }));
    triggers.push_back(new TriggerNode("party healer low mana", { NextAction("mana tide totem", ACTION_HIGH + 5) }));

    // Healing Triggers
    // Healer priority: a dying member (critical band), then the tank picking mobs up, then the rest.
    // A member back from a battle res comes first until safe; Earth Shield goes back on him if he tanks.
    triggers.push_back(new TriggerNode("battle resurrected member low health",
                                       { NextAction("riptide on battle resurrected", 28.5f),
                                         NextAction("lesser healing wave on battle resurrected", 28.0f) }));
    triggers.push_back(new TriggerNode("battle resurrected member",
                                       { NextAction("earth shield on battle resurrected", ACTION_HIGH + 7.5f) }));
    // The peel: Riptide on the tank before the new mobs' swings, top of the losing-fast band; a wave only once he
    // is actually down, below a victim who is losing health fast.
    triggers.push_back(new TriggerNode("tank peel", { NextAction("riptide on peeling tank", 22.5f),
                                                      NextAction("lesser healing wave on peeling tank", 20.5f) }));

    triggers.push_back(new TriggerNode("chain heal targets", { NextAction("riptide on party", 27.0f),
                                                               NextAction("chain heal on party", 26.0f) }));
    // [healer_audit_b30] Two members below 65 % already make Chain Heal the efficient heal in a five-man (the tank and
    // the melee stand together): build 30 resto at 80 cast 5 Chain Heals in 1455 s of combat against 65 Lesser
    // Healing Waves; "chain heal targets" needs 3 members below 85 %. Below the critical band (23-25).
    triggers.push_back(new TriggerNode("medium aoe heal", { NextAction("chain heal on party", 22.8f) }));

    triggers.push_back(new TriggerNode("low aoe heal", { NextAction("tidal force", ACTION_CRITICAL_HEAL - 1.0f) }));
    triggers.push_back(new TriggerNode("nature's swiftness active",
                                       { NextAction("healing wave on party", ACTION_CRITICAL_HEAL + 1.0f) }));

    triggers.push_back(new TriggerNode("party member critical health",
                                       { NextAction("nature's swiftness", ACTION_CRITICAL_HEAL) }));

    triggers.push_back(new TriggerNode("party member critical health", { NextAction("riptide on party", 25.0f),
                                                                         NextAction("healing wave on party", 24.0f),
                                                                         NextAction("lesser healing wave on party", 23.0f) }));

    // Above the low band and the tank's Riptide upkeep, below critical health
    triggers.push_back(new TriggerNode("party member losing health fast",
                                       { NextAction("riptide on party", 22.0f),
                                         NextAction("lesser healing wave on party", 21.5f) }));
    // [b31_sd] Chain Heal in the single-target bands (GUIDE_AUDIT_HEALERS.md: CH 0.51/min on bosses, 0 at 60; the
    // guides make it the main heal). CastChainHealAction::isUseful() keeps it to a target with another injured
    // member (2 in a party, 3 in a raid) within its jump range, so with nobody near the band falls through to the
    // waves as before. Low band: below Healing Wave (the target is low); medium / almost full: above the waves.
    triggers.push_back(new TriggerNode("party member low health", { NextAction("riptide on party", 19.0f),
                                                                    NextAction("healing wave on party", 18.0f),
                                                                    NextAction("chain heal on party", 17.5f),
                                                                    NextAction("lesser healing wave on party", 17.0f) }));

    triggers.push_back(new TriggerNode("party member medium health", { NextAction("riptide on party", 16.0f),
                                                                       NextAction("chain heal on party", 15.5f),
                                                                       NextAction("healing wave on party", 15.0f),
                                                                       NextAction("lesser healing wave on party", 14.0f) }));

    triggers.push_back(new TriggerNode("party member almost full health", { NextAction("riptide on party", 12.0f),
                                                                            NextAction("chain heal on party", 11.5f),
                                                                            NextAction("lesser healing wave on party", 11.0f) }));

    // [b31_sd] Tidal Waves up and the member below 80 %: Healing Wave over the almost-full band's Lesser Healing Wave
    // (11.0), below its Riptide and Chain Heal. The losing-fast band keeps the fast LHW (21.5); medium and below
    // already rank HW over LHW.
    triggers.push_back(new TriggerNode("tidal waves healing wave", { NextAction("healing wave on party", 11.4f) }));

    triggers.push_back(new TriggerNode("earth shield on main tank", { NextAction("earth shield on main tank", ACTION_HIGH + 7) }));
    triggers.push_back(new TriggerNode("riptide on main tank", { NextAction("riptide on main tank", 16.5f) }));

    // Dispel Triggers
    float const dispelRelevance = SHAMAN_RESTO_DISPEL_RELEVANCE;
    triggers.push_back(new TriggerNode("party member cleanse spirit poison",
                                       { NextAction("cleanse spirit poison on party", dispelRelevance) }));
    triggers.push_back(new TriggerNode("party member cleanse spirit disease",
                                       { NextAction("cleanse spirit disease on party", dispelRelevance) }));
    triggers.push_back(new TriggerNode("party member cleanse spirit curse",
                                       { NextAction("cleanse spirit curse on party", dispelRelevance) }));

    // Range/Mana Triggers
    triggers.push_back(new TriggerNode("enemy too close for spell", { NextAction("flee", ACTION_MOVE + 9) }));
    triggers.push_back(new TriggerNode("party member to heal out of spell range", { NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 1) }));
    triggers.push_back(new TriggerNode("water shield", { NextAction("water shield", ACTION_LIGHT_HEAL + 3.0f) }));
    // [b31_deep] Earthliving that expired mid-fight (30 min) is restored in combat too, as the Elemental and
    // Enhancement imbues; below level 30 (no Earthliving Weapon) Flametongue takes the main hand, as out of combat.
    triggers.push_back(new TriggerNode("main hand weapon no imbue",
                                       { NextAction("earthliving weapon main hand", ACTION_LIGHT_HEAL + 2.9f),
                                         NextAction("flametongue weapon main hand", ACTION_LIGHT_HEAL + 2.8f) }));

    // Custom tier set bonuses ([setbonus-rot], SetBonusTriggers.h, C:/WoW/SET_BONUS_SPELLS.md).
    triggers.push_back(new TriggerNode("set bonus tidal surge", { NextAction("riptide on party", 27.5f) }));
    triggers.push_back(new TriggerNode("set bonus titanic tide", { NextAction("chain heal on party", 26.5f) }));
    triggers.push_back(new TriggerNode("set bonus ascendant tide", { NextAction("healing wave on party", 24.5f) }));
}

void ShamanHealerDpsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("healer should attack", { NextAction("flame shock", ACTION_DEFAULT + 0.2f),
                                                                 NextAction("lava burst", ACTION_DEFAULT + 0.1f),
                                                                 NextAction("lightning bolt", ACTION_DEFAULT) }));

    triggers.push_back( new TriggerNode("medium aoe and healer should attack", { NextAction("chain lightning", ACTION_DEFAULT + 0.3f) }));
}
