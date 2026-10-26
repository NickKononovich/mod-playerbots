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
class PlayerbotAI;
class Unit;

// [heal_triage] Shared healer triage helpers (all four healer classes).
namespace HealTriage
{
    // A member no heal can land on: Ice Block, Cyclone, Divine Intervention, ... (the same Unit::IsImmunedToSpell
    // test CheckCast runs, against a rank 1 heal of the healer's class).
    bool IsHealImmune(Player* healer, Unit* target);

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
