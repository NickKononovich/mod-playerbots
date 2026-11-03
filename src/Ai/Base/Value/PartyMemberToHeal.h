/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PARTYMEMBERTOHEAL_H
#define PLAYERBOTS_PARTYMEMBERTOHEAL_H

#include "PartyMemberValue.h"

class Pet;
class Player;
class SpellInfo;
class PlayerbotAI;
class Unit;

// [heal_triage] Shared healer triage helpers (all four healer classes).
namespace HealTriage
{
    // A member no heal can land on: Ice Block, Cyclone, Divine Intervention, ... (the same Unit::IsImmunedToSpell
    // test CheckCast runs, against a rank 1 heal of the healer's class).
    // [lead946] the core's own test with the healer as the caster (Unit::IsImmunedToSpell): a friendly heal ignores
    // a school immunity unless its aura has SPELL_ATTR1_IMMUNITY_TO_HOSTILE_AND_FRIENDLY_EFFECTS (Cyclone, Banish);
    // Ice Block, Divine Shield and Hand of Protection let it land
    bool IsHealImmune(Player* healer, Unit* target);

    // [lead946] the member cannot lose health now: immune to the damage of every school (Ice Block, Divine Shield,
    // Cyclone; Unit::IsImmunedToDamage) for longer than the healer's global cooldown - a heal on it waits for the
    // immunity's end (a heal cast in that last global cooldown lands as it ends)
    bool CannotLoseHealth(Player* healer, Unit* target);

    // [lead940] a heal spell whose healing makes threat (ThreatManager::ForwardThreatForAssistingMe on every mob with
    // the healed member on its threat list): a heal effect or a periodic heal aura, without SPELL_ATTR1_NO_THREAT
    // (Earth Shield's 379 and Prayer of Mending's 33110 heals have it: no threat)
    bool IsThreatHeal(SpellInfo const* info);
    // [lead940] a heal over time (a periodic heal aura)
    bool IsPeriodicHeal(SpellInfo const* info);

    // [lead945] the member drains its own health by design: it knows Life Tap (any rank)
    bool DrainsOwnHealth(Player* member);

    // A healthy tank's HoT upkeep (Lifebloom / Rejuvenation / Renew / Riptide on the main tank) yields to a member
    // below medium health: HoR heroic Falric 2026-10-25, the resto druid rolled Lifebloom on a 96 % tank while the
    // dps sat at 17-40 %.
    bool TankUpkeepYields(PlayerbotAI* botAI, Unit* tank);

    // [heal_triage b29] Below this the heal target is an emergency: the single-target saves outrank the group heals
    // and no cure, facing turn or damage spell of the healer goes first.
    inline constexpr float EMERGENCY_HEALTH_PCT = 35.0f;

    // A group member below medium health who cannot take a heal right now (Ice Block): the healer holds its heals
    // ready instead of turning to damage, the block ends at any moment with the member still low.
    bool HasImmuneMemberToHeal(PlayerbotAI* botAI);
}

class PartyMemberToHeal : public PartyMemberValue
{
public:
    PartyMemberToHeal(PlayerbotAI* botAI, std::string const name = "party member to heal")
        : PartyMemberValue(botAI, name)
    {
    }

protected:
    Unit* Calculate() override;
    bool Check(Unit* player) override;
};

class PartyMemberToProtect : public PartyMemberValue
{
public:
    PartyMemberToProtect(PlayerbotAI* botAI, std::string const name = "party member to protect")
        : PartyMemberValue(botAI, name)
    {
    }

protected:
    Unit* Calculate() override;
};

class HealerLowMana : public PartyMemberValue
{
public:
    HealerLowMana(PlayerbotAI* botAI) : PartyMemberValue(botAI, "healer low mana") {}

protected:
    Unit* Calculate() override;
};

#endif
