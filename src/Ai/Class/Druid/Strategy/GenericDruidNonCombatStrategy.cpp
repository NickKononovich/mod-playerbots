/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericDruidNonCombatStrategy.h"
#include "AiFactory.h"
#include "DruidTriggers.h"  // [rot1] DruidGroupNeedsHeal, BALANCE_HEAL_PCT
#include "FeralDruidStrategy.h"
#include "LeaderCommandState.h"  // [rot2] [rot3]
#include "Playerbots.h"
#include "RezzedTankHoldMultiplier.h"
#include "Log.h"    // [rot1]
#include "Timer.h"  // [rot1]

class GenericDruidNonCombatStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericDruidNonCombatStrategyActionNodeFactory()
    {
        creators["thorns"] = &thorns;
        creators["thorns on party"] = &thorns_on_party;
        creators["mark of the wild"] = &mark_of_the_wild;
        creators["mark of the wild on party"] = &mark_of_the_wild_on_party;
        creators["regrowth on party"] = &regrowth_on_party;  // [b31_deep] the key had underscores
        creators["rejuvenation on party"] = &rejuvenation_on_party;
        creators["remove curse on party"] = &remove_curse_on_party;
        creators["abolish poison on party"] = &abolish_poison_on_party;
        creators["revive"] = &revive;
        creators["aquatic form"] = &aquatic_form;
    }

private:
    static ActionNode* thorns([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("thorns",
                              /*P*/ { NextAction("leave animal form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* thorns_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("thorns on party",
                              /*P*/ { NextAction("leave animal form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* mark_of_the_wild([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("mark of the wild",
                              /*P*/ { NextAction("leave animal form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* mark_of_the_wild_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("mark of the wild on party",
                              /*P*/ { NextAction("leave animal form") },
                              /*A*/ {},
                              /*C*/ {});
    }
    static ActionNode* regrowth_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("regrowth on party",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }
    static ActionNode* rejuvenation_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("rejuvenation on party",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }
    static ActionNode* remove_curse_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("remove curse on party",
                              /*P*/ { NextAction("leave animal form") },
                              /*A*/ {},
                              /*C*/ {});
    }
    static ActionNode* abolish_poison_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("abolish poison on party",
                              /*P*/ { NextAction("leave animal form") },
                              /*A*/ {},
                              /*C*/ {});
    }
    static ActionNode* revive([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("revive",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }

    static ActionNode* aquatic_form([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("aquatic form",
                              /*P*/ { NextAction("caster form") },
                              /*A*/ {},
                              /*C*/ {});
    }
};

GenericDruidNonCombatStrategy::GenericDruidNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericDruidNonCombatStrategyActionNodeFactory());
}

void GenericDruidNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("mark of the wild", { NextAction("mark of the wild", 14.0f) }));
    // [b31_deep] A bear tank's non-combat engine also runs between the mobs of a fight: each of these heals and the
    // poison cure shifted it to caster form (rage gone, 2 GCDs; 5.8 % of the level 70-79 combat ticks went to
    // Rejuvenation at "almost full"). It keeps the critical heals only. The second copy of these trigger nodes
    // (registered twice with the same actions) is merged into this block.
    int const specTab = AiFactory::GetPlayerSpecTab(botAI->GetBot());
    bool const bearTank = specTab == DRUID_TAB_FERAL && DruidHasThickHide(botAI->GetBot());

    // [rot1] the moonkin back in form out of combat (MoonkinFormUpkeepTrigger): below Mark of the Wild (13-14) and the
    // heals, above the harness waypoint walk (11.0) and Thorns (10-11)
    if (specTab == DRUID_TAB_BALANCE)
        triggers.push_back(new TriggerNode("no moonkin form", { NextAction("moonkin form", ACTION_NORMAL + 1.5f) }));

    if (!bearTank)
        triggers.push_back(new TriggerNode("party member cure poison",
                                           { NextAction("abolish poison on party", 20.0f) }));
    triggers.push_back(new TriggerNode("party member dead", { NextAction("revive", ACTION_CRITICAL_HEAL + 10) }));

    triggers.push_back(new TriggerNode("often", { NextAction("apply oil", 1.0f) }));

    triggers.push_back(new TriggerNode("party member critical health", {
                       NextAction("wild growth on party", ACTION_MEDIUM_HEAL + 7),
                       NextAction("regrowth on party", ACTION_MEDIUM_HEAL + 6),
                       NextAction("rejuvenation on party", ACTION_MEDIUM_HEAL + 5),
                       }));
    if (!bearTank)
    {
        triggers.push_back(new TriggerNode("party member low health", {
                           NextAction("wild growth on party", ACTION_MEDIUM_HEAL + 5),
                           NextAction("regrowth on party", ACTION_MEDIUM_HEAL + 4),
                           NextAction("rejuvenation on party", ACTION_MEDIUM_HEAL + 3),
                           }));
        triggers.push_back(new TriggerNode("party member medium health", {
                           NextAction("wild growth on party", ACTION_MEDIUM_HEAL + 3),
                           NextAction("regrowth on party", ACTION_MEDIUM_HEAL + 2),
                           NextAction("rejuvenation on party", ACTION_MEDIUM_HEAL + 1),
                           }));
        triggers.push_back(new TriggerNode("party member almost full health", {
                           NextAction("wild growth on party", ACTION_LIGHT_HEAL + 3),
                           NextAction("rejuvenation on party", ACTION_LIGHT_HEAL + 2),
                           }));
    }
    triggers.push_back(new TriggerNode("party member remove curse", {
                       NextAction("remove curse on party", ACTION_DISPEL + 7),
                       }));
    triggers.push_back(
        new TriggerNode("new pet", { NextAction("set pet stance", 60.0f) }));

    triggers.push_back(new TriggerNode("aquatic form", { NextAction("aquatic form", 10.0f) }));

    // [b32_tank_aoe] A bear tank back in (Dire) Bear Form out of combat once nothing is left to rest for: it walked to
    // the next pack and into harness / body pulls in caster form after Thorns / Mark of the Wild / a drink, and the
    // combat "bear form" trigger shifted only after the first hit. Above the harness waypoint walk (11.0) and Thorns
    // (10-11, which takes "caster form" once more and the trigger's 3 s grace), below Mark of the Wild (13-14), the
    // heals, the rest (drink / food) and the pull sequence. Bear Form below 40 (no Dire Bear Form: impossible).
    if (bearTank)
        triggers.push_back(new TriggerNode("bear tank form",
            { NextAction("dire bear form", 11.5f), NextAction("bear form", 11.4f) }));

    if (specTab == DRUID_TAB_BALANCE || specTab == DRUID_TAB_RESTORATION)
        triggers.push_back(new TriggerNode("often", { NextAction("apply oil", 1.0f) }));
    if (specTab == DRUID_TAB_FERAL)
        triggers.push_back(new TriggerNode("often", { NextAction("apply stone", 1.0f) }));

    // K6 pre-pull: Lifebloom on the tank that is about to pull
    if (specTab == DRUID_TAB_RESTORATION)
    {
        triggers.push_back(new TriggerNode("pull imminent", { NextAction("lifebloom on main tank", ACTION_HIGH + 2) }));

        // A resto druid whose tank engaged before the druid itself entered combat is still on this engine: keep
        // its direct heals here too, or a dying tank only gets Wild Growth, Regrowth and Rejuvenation.
        triggers.push_back(new TriggerNode("party member critical health",
                                           {
                                               NextAction("swiftmend on party", ACTION_MEDIUM_HEAL + 8),
                                               NextAction("nourish on party", ACTION_MEDIUM_HEAL + 5.5f),
                                           }));
        triggers.push_back(new TriggerNode("party member low health",
                                           { NextAction("nourish on party", ACTION_MEDIUM_HEAL + 3.5f) }));
        triggers.push_back(new TriggerNode("party member losing health fast",
                                           {
                                               NextAction("swiftmend on party", ACTION_MEDIUM_HEAL + 6.4f),
                                               NextAction("rejuvenation on party", ACTION_MEDIUM_HEAL + 6.3f),
                                               NextAction("nourish on party", ACTION_MEDIUM_HEAL + 6.2f),
                                           }));
    }

}

GenericDruidBuffStrategy::GenericDruidBuffStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericDruidNonCombatStrategyActionNodeFactory());
}

void GenericDruidBuffStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("mark of the wild on party", {
                       NextAction("mark of the wild on party", 13.0f),
                       }));
    triggers.push_back(new TriggerNode("thorns on main tank", {
                       NextAction("thorns on main tank", 11.0f),
                       }));
    triggers.push_back(new TriggerNode("thorns", {
                       NextAction("thorns", 10.0f),
                       }));
}

void GenericDruidNonCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    NonCombatStrategy::InitMultipliers(multipliers);
    // the non-combat engine runs right after a resurrection, also while the group still fights
    if (PlayerbotAI::IsTank(botAI->GetBot(), true))
        multipliers.push_back(new RezzedTankHoldMultiplier(botAI));
    // [rot1]
    if (AiFactory::GetPlayerSpecTab(botAI->GetBot()) == DRUID_TAB_BALANCE)
        multipliers.push_back(new BalanceGroupSupportMultiplier(botAI));
}

float BalanceGroupSupportMultiplier::GetValue(Action* action)
{
    // the name first (string compares), the group pass last
    if (!action)
        return 1.0f;

    std::string const& name = action->getName();
    bool const heal = name == "rejuvenation on party" || name == "regrowth on party" ||
                      name == "wild growth on party" || name == "healing touch on party";
    bool const thorns = name == "thorns on party" || name == "thorns on main tank";
    if (!heal && !thorns)
        return 1.0f;

    Player* bot = botAI->GetBot();

    // [rot2] a led moonkin heals or buffs another member only under the leader's Heal command (the leader's call for
    // heal help); its externals come as "leader external"
    // [rot3] a stale command: none (no fallback to its own rule)
    Leader::MemberEvents command;
    LeaderCommandState::State const state = LeaderCommandState::Read(bot, command);
    if (state != LeaderCommandState::State::NotLed)
    {
        if (state == LeaderCommandState::State::Fresh && command.attack == Leader::AttackEvent::Heal)
            return 1.0f;

        uint32 const now = getMSTime();
        if (!_loggedMs || getMSTimeDiff(_loggedMs, now) >= LOG_MS)
        {
            _loggedMs = now ? now : 1;
            LOG_INFO("playerbots", "[rot2] [moonkin support] {} skips {}: led, no Heal command from the leader",
                     bot->GetName(), name);
        }
        return 0.0f;
    }

    bool healerAlive = false;
    DruidGroupNeedsHeal(bot, &healerAlive);
    if (!healerAlive)
        return 1.0f;

    Unit* target = action->GetTarget();
    if (heal && (!target || target->GetHealthPct() < BALANCE_HEAL_PCT))
        return 1.0f;

    uint32 const now = getMSTime();
    if (!_loggedMs || getMSTimeDiff(_loggedMs, now) >= LOG_MS)
    {
        _loggedMs = now ? now : 1;
        LOG_INFO("playerbots", "[rot1] [moonkin support] {} skips {} on {} ({:.0f} % health): a healer lives, the "
                 "moonkin heals only below {:.0f} % and keeps Thorns to itself", bot->GetName(), name,
                 target ? target->GetName() : "nobody", target ? target->GetHealthPct() : 0.0f, BALANCE_HEAL_PCT);
    }

    return 0.0f;
}
