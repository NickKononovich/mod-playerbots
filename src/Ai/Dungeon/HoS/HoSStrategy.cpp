/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoSStrategy.h"
#include "AiObjectContext.h"
#include "DungeonHazard.h"
#include "HoSMultipliers.h"
#include "HoSTriggers.h"
#include "Playerbots.h"

void WotlkDungeonHoSStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Maiden of Grief
    // Pillar of Woe 50761/59727 hits only its target (a Magic DoT, left to the class dispels); Storm of Grief
    // 50752/59772 is a 9 yd persistent area aura that the generic "avoid aoe" leaves.

    // Krystallus: every stoned unit, pets included, Shatters on the allies around it. Short-lived summons (Fire
    // Elemental, Water Elemental, Treants, ...) are dismissed near him, a hunter dismisses the pet, other pets are
    // parked (or dismissed) on Ground Slam; ranged and healers hold their spoke of the star the whole fight.
    triggers.push_back(new TriggerNode("krystallus dismiss summon",
        { NextAction("krystallus dismiss summon", ACTION_RAID + 8) }));
    triggers.push_back(new TriggerNode("krystallus dismiss pet",
        { NextAction("krystallus dismiss pet", ACTION_RAID + 7) }));
    triggers.push_back(new TriggerNode("krystallus pet spread",
        { NextAction("krystallus pet spread", ACTION_RAID + 6) }));
    triggers.push_back(new TriggerNode("ground slam",
        { NextAction("shatter spread", ACTION_RAID + 5) }));
    triggers.push_back(new TriggerNode("krystallus ranged spread",
        { NextAction("krystallus ranged spread", ACTION_RAID + 1) }));

    // Brann Bronzebeard: gossip chain 9669 -> 9670 -> 10206 -> 10012, escort and Tribunal of Ages
    triggers.push_back(new TriggerNode("hos brann gossip",
        { NextAction("hos brann gossip", ACTION_MOVE + 3) }));
    triggers.push_back(new TriggerNode("hos stay with brann",
        { NextAction("hos stay with brann", ACTION_MOVE + 2) }));
    triggers.push_back(new TriggerNode("hos brann attacked",
        { NextAction("hos protect brann", ACTION_RAID + 2) }));
    triggers.push_back(new TriggerNode("hos tribunal hazard",
        { NextAction("hos avoid tribunal hazard", ACTION_RAID + 4) }));
    // The tank takes every add that hits another member (above its own target choice, below Brann's protection).
    triggers.push_back(new TriggerNode("hos tribunal loose add",
        { NextAction("hos tank tribunal add", ACTION_RAID + 1) }));
    // The Tribunal is survived for 3 x 100 s, not killed: the tank holds a spot at Brann's console (above its add
    // pickup, below the hazards), the healer stays at the tank, the tank does not leave the spot for far adds
    // (TribunalHoldMultiplier) and the damage dealers control and kill the Dark Rune Protector on the healer first.
    triggers.push_back(new TriggerNode("hos tribunal hold point",
        { NextAction("hos tribunal hold point", ACTION_RAID + 3) }));
    triggers.push_back(new TriggerNode("hos tribunal healer hold",
        { NextAction("hos tribunal healer hold", ACTION_MOVE + 3) }));
    triggers.push_back(new TriggerNode("hos tribunal protector",
        { NextAction("hos tribunal protector", ACTION_RAID + 1) }));

    // Sjonnir The Ironshaper
    // Possibly tank in place in the middle of the room, assign a dps to adds?
    triggers.push_back(new TriggerNode("lightning ring",
        { NextAction("avoid lightning ring", ACTION_RAID + 5) }));

    // Trash: Unrelenting Construct Short Circuit below 25 % (DungeonHazard.cpp)
    triggers.push_back(new TriggerNode("dungeon hazard",
        { NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void WotlkDungeonHoSStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new KrystallusMultiplier(botAI));
    multipliers.push_back(new SjonnirMultiplier(botAI));
    multipliers.push_back(new TribunalMultiplier(botAI));
    multipliers.push_back(new TribunalHoldMultiplier(botAI));
    multipliers.push_back(new TribunalHazardKeepOutMultiplier(botAI));
    multipliers.push_back(new TribunalNoChaseMultiplier(botAI));  // [b33_hos]
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}

// hos-h 20261025-132016 (Tribunal attempt 1, 1289-1469 s): "hos tribunal protector" (61) set the Protector on the
// healer, "dps assist" (50) set the generic "dps target" back the next tick, and the two swapped the target all fight
// ("[idle diag] Rtmagax ... cast 2 ... actions hos tribunal protector:10 dps assist:9"; mage 20.8 % busy, 62k damage
// in 180 s). The adds piled up and killed the healer, then the mage. With the Protector the only candidate both pick
// the same unit.
namespace
{
// [b36_hos] A melee add this close to the hold spot keeps the melee damage dealers on the group (20 yd: the tank's
// forward spot 10.4 yd plus a Protector's walk-in and Charge distance to the healer).
constexpr float TRIBUNAL_MELEE_GUARD = HoSIds::TRIBUNAL_MELEE_GUARD;  // [b44_hos] moved to HoSIds
}  // namespace

void WotlkDungeonHoSStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    Player* bot = botAI->GetBot();
    // [b35_hos] The tank's own pick: hos-h 20261026-024524-hos-h-5G-28, "tank assist" kept the DK tank on a
    // Stormcaller 35 yd away (out of its reach, the no-chase hold) for ~40 s, filling with Horn of Winter, while the
    // Protectors hit the others. A Stormcaller beyond the tank's leash is the damage dealers' (and the tank's ranged
    // taunt / Death Grip, "hos tank tribunal add"); the tank's target is what it can reach.
    if (type == TargetValueExclusionType::TankTarget && PlayerbotAI::IsTank(bot) && bot->IsInCombat() &&
        HoSShared::IsTribunalActive(bot))
    {
        GuidVector const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
        GuidSet farCasters;
        bool reachable = false;
        for (ObjectGuid const& guid : attackers)
        {
            Unit* add = botAI->GetUnit(guid);
            if (!add || !add->IsAlive())
                continue;

            if (guid.GetEntry() == HoSIds::NPC_DARK_RUNE_STORMCALLER &&
                add->GetExactDist2d(HoSIds::TRIBUNAL_TANK_X, HoSIds::TRIBUNAL_TANK_Y) > HoSIds::TRIBUNAL_TANK_LEASH)
                farCasters.insert(guid);
            else
                reachable = true;
        }
        if (reachable)
            exclusions.insert(farCasters.begin(), farCasters.end());
        return;
    }

    if (type != TargetValueExclusionType::DpsTarget || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot) ||
        !bot->IsInCombat())
        return;

    GuidVector const& attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    Unit* protector = HoSShared::ProtectorOnHealer(botAI);
    if (protector)
    {
        for (ObjectGuid const& guid : attackers)
            if (guid != protector->GetGUID())
                exclusions.insert(guid);
        return;
    }

    // [b44_hos] The Protector it peeled, now on the tank: finished before anything else (HoSShared::PeeledProtector).
    if (Unit* peeled = HoSShared::PeeledProtector(botAI))
    {
        for (ObjectGuid const& guid : attackers)
            if (guid != peeled->GetGUID())
                exclusions.insert(guid);
        return;
    }

    // [b34_hos] Then the Stormcallers: hos-h 20261026-014521-hos-h-5D-34 lost the warlock (2379 s) and the bear
    // (2395 s) to Stormcaller Lightning Bolts while the damage dealers were on Protectors the tank already held. They
    // cast from range and die fast; the rest is left out while one of them lives. Only within the Tribunal.
    // [b35_hos] Every live Stormcaller, wherever it stands (was: within TRIBUNAL_DPS_REACH of the spot, and the
    // 5G-28 ones stood at 46 yd); the no-chase multiplier no longer holds a walk to one.
    if (!HoSShared::IsTribunalActive(bot))
        return;

    // [b36_hos] A melee damage dealer leaves the group for a Stormcaller only while no Protector or Golem stands near
    // the hold spot: hos-h 20261026-034959-hos-h-5G-25 (all-melee comp) had the Ret paladin, the warrior and the DK
    // 27 yd out on Stormcallers when two Protectors reached the healer 4 yd from the tank; she went 91 % -> 0 in 4 s
    // (1643-1647 s), before "hos tribunal protector" could walk them back. A Protector or Cleave hit on a healer is up
    // to 9k (p90), a Lightning Bolt 4.9k (p90), so the melee stay on the melee adds; ranged keep the Stormcallers.
    // [b44_hos] With a ranged damage dealer in the group the melee never walk out to a Stormcaller: they fight what
    // reaches the tank's spot (Protectors, the Golem, a Stormcaller that came within TRIBUNAL_MELEE_GUARD) and the
    // ranged take the casters, as a real group splits it. The b36 rule below still ran them 40-46 yd out to the
    // Stormcallers whenever the 20 yd zone emptied and back with the next wave (DPS_GAP_TRIBUNAL_COS.md 2.5: Enh
    // "reach melee" 19 % of the samples, Stormstrike 1.4 a minute, its totems left at the spot). The b36 rule stays
    // for an all-melee group: nobody else would kill the Stormcallers.
    if (PlayerbotAI::IsMelee(bot) && HoSShared::GroupHasRangedDps(bot))
    {
        for (ObjectGuid const& guid : attackers)
            if (guid.GetEntry() == HoSIds::NPC_DARK_RUNE_STORMCALLER &&
                HoSShared::LeftToRanged(bot, botAI->GetUnit(guid)))
                exclusions.insert(guid);
        return;
    }

    if (PlayerbotAI::IsMelee(bot))
        for (ObjectGuid const& guid : attackers)
        {
            uint32 const entry = guid.GetEntry();
            if (entry != HoSIds::NPC_DARK_RUNE_PROTECTOR && entry != HoSIds::NPC_IRON_GOLEM_CUSTODIAN)
                continue;

            Unit* add = botAI->GetUnit(guid);
            if (add && add->IsAlive() &&
                add->GetExactDist2d(HoSIds::TRIBUNAL_HOLD_X, HoSIds::TRIBUNAL_HOLD_Y) <= TRIBUNAL_MELEE_GUARD)
            {
                for (ObjectGuid const& other : attackers)
                    if (other.GetEntry() == HoSIds::NPC_DARK_RUNE_STORMCALLER)
                        exclusions.insert(other);
                return;
            }
        }

    bool stormcaller = false;
    for (ObjectGuid const& guid : attackers)
    {
        if (guid.GetEntry() != HoSIds::NPC_DARK_RUNE_STORMCALLER)
            continue;

        Unit* add = botAI->GetUnit(guid);
        if (add && add->IsAlive())
        {
            stormcaller = true;
            break;
        }
    }
    if (!stormcaller)
        return;

    for (ObjectGuid const& guid : attackers)
        if (guid.GetEntry() != HoSIds::NPC_DARK_RUNE_STORMCALLER)
            exclusions.insert(guid);
}
