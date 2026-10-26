/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericPaladinNonCombatStrategy.h"
#include "AiFactory.h"
#include "GenericPaladinStrategyActionNodeFactory.h"
#include "Group.h"
#include "Multiplier.h"
#include "Playerbots.h"
#include "RezzedTankHoldMultiplier.h"

namespace
{
    // A healer this low on mana no longer counts: the tank helps out again.
    constexpr float TANK_HEAL_HEALER_MANA = 10.0f;

    // A protection paladin with a living healer in the group leaves the party heals to it. Its non-combat engine
    // also runs while the group fights and an encounter or the raid leader holds its tank assist: in Forge of Souls
    // (Bronjahm) the tank healed the group with Flash of Light down to 1% mana and could neither taunt nor hold.
    class PaladinTankHealMultiplier : public Multiplier
    {
    public:
        PaladinTankHealMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "paladin tank heal") {}

        float GetValue(Action* action) override
        {
            if (!action)
                return 1.0f;

            std::string const name = action->getName();
            if (name != "flash of light on party" && name != "holy light on party")
                return 1.0f;

            if (!PlayerbotAI::IsTank(bot))
                return 1.0f;

            Group* group = bot->GetGroup();
            if (!group)
                return 1.0f;

            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || member->GetMap() != bot->GetMap() || !member->IsAlive() ||
                    !PlayerbotAI::IsHeal(member))
                    continue;

                if (member->getPowerType() != POWER_MANA || member->GetPowerPct(POWER_MANA) >= TANK_HEAL_HEALER_MANA)
                    return 0.0f;
            }

            return 1.0f;
        }
    };
}

GenericPaladinNonCombatStrategy::GenericPaladinNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericPaladinStrategyActionNodeFactory());
}

void GenericPaladinNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("party member dead", { NextAction("redemption", ACTION_CRITICAL_HEAL + 10) }));
    triggers.push_back(new TriggerNode("party member almost full health", { NextAction("flash of light on party", ACTION_MEDIUM_HEAL + 5.0f) }));
    triggers.push_back(new TriggerNode("party member medium health", { NextAction("flash of light on party", ACTION_MEDIUM_HEAL + 6.0f) }));
    triggers.push_back(new TriggerNode("party member low health", { NextAction("holy light on party", ACTION_MEDIUM_HEAL + 7.0f) }));
    triggers.push_back(new TriggerNode("party member critical health", { NextAction("holy light on party", ACTION_MEDIUM_HEAL + 8.0f) }));
    triggers.push_back(new TriggerNode("not sensing undead", { NextAction("sense undead", ACTION_IDLE + 1.0f) }));

    int specTab = AiFactory::GetPlayerSpecTab(botAI->GetBot());
    if (specTab == PALADIN_TAB_HOLY)
        triggers.push_back(new TriggerNode("often", { NextAction("apply oil", ACTION_IDLE + 1.0f) }));
    if (specTab == PALADIN_TAB_PROTECTION || specTab == PALADIN_TAB_RETRIBUTION)
        triggers.push_back(new TriggerNode("often", { NextAction("apply stone", ACTION_IDLE + 1.0f) }));

    triggers.push_back(new TriggerNode("greater blessing needed",
        { NextAction("cast greater blessing assignment", ACTION_NORMAL) }));

    // [pal-bless-role] Every member gets a blessing by role (the greater assignments above are for raids).
    triggers.push_back(new TriggerNode("blessing on party by role",
        { NextAction("blessing on party by role", ACTION_NORMAL + 1.5f) }));

    // K4: Divine Plea between pulls (it halves healing, so out of combat is the cheap window)
    triggers.push_back(new TriggerNode("medium mana", { NextAction("divine plea", ACTION_NORMAL + 1) }));

    // K6 pre-pull: Beacon of Light and Sacred Shield on the tank that is about to pull
    if (specTab == PALADIN_TAB_HOLY)
    {
        triggers.push_back(new TriggerNode("pull imminent",
            { NextAction("beacon of light on main tank", ACTION_HIGH + 3),
              NextAction("sacred shield on main tank", ACTION_HIGH + 2) }));
    }
}

void GenericPaladinNonCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    NonCombatStrategy::InitMultipliers(multipliers);
    multipliers.push_back(new PaladinTankHealMultiplier(botAI));
    // the non-combat engine runs right after a resurrection, also while the group still fights
    if (PlayerbotAI::IsTank(botAI->GetBot(), true))
        multipliers.push_back(new RezzedTankHoldMultiplier(botAI));
}
