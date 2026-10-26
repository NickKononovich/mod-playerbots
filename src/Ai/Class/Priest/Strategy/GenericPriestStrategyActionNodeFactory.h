/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GENERICPRIESTSTRATEGYACTIONNODEFACTORY_H
#define PLAYERBOTS_GENERICPRIESTSTRATEGYACTIONNODEFACTORY_H

#include "Action.h"
#include "NamedObjectContext.h"

class GenericPriestStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericPriestStrategyActionNodeFactory()
    {
        creators["inner fire"] = &inner_fire;
        creators["holy nova"] = &holy_nova;
        creators["power word: fortitude"] = &power_word_fortitude;
        creators["power word: fortitude on party"] = &power_word_fortitude_on_party;
        creators["divine spirit"] = &divine_spirit;
        creators["divine spirit on party"] = &divine_spirit_on_party;
        creators["power word: shield"] = &power_word_shield;
        creators["renew"] = &renew;
        creators["renew on party"] = &renew_on_party;
        creators["greater heal"] = &greater_heal;
        creators["greater heal on party"] = &greater_heal_on_party;
        creators["heal"] = &heal;
        creators["heal on party"] = &heal_on_party;
        creators["lesser heal"] = &lesser_heal;
        creators["lesser heal on party"] = &lesser_heal_on_party;
        creators["flash heal"] = &flash_heal;
        creators["flash heal on party"] = &flash_heal_on_party;
        creators["psychic scream"] = &psychic_scream;
        creators["hymn of hope"] = &hymn_of_hope;                          // [b31_deep]
        creators["divine hymn"] = &divine_hymn;                            // [b31_deep]
        creators["prayer of healing on party"] = &prayer_of_healing_on_party;  // [b31_deep]
        creators["greater heal on main tank"] = &greater_heal_on_main_tank;    // [b31_deep]
    }

private:
    static ActionNode* inner_fire([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "inner fire",
            /*P*/ {},  // [b31_deep] castable in Shadowform (Spell.dbc StancesNot has no bit 27)
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* holy_nova([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "holy nova",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* power_word_fortitude([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "power word: fortitude",
            /*P*/ {},  // [b31_deep] castable in Shadowform
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* power_word_fortitude_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "power word: fortitude on party",
            /*P*/ {},  // [b31_deep] castable in Shadowform
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* divine_spirit([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "divine spirit",
            /*P*/ {},  // [b31_deep] castable in Shadowform
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* divine_spirit_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "divine spirit on party",
            /*P*/ {},  // [b31_deep] castable in Shadowform
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* power_word_shield([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "power word: shield",
            // [b31_deep] castable in Shadowform: the prereq dropped the form each time a mob hit the shadow priest
            /*P*/ {},
            // /*A*/ { NextAction("renew", 50.0f) },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* power_word_shield_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "power word: shield on party",
            /*P*/ {},  // [b31_deep] castable in Shadowform
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* renew([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "renew",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* renew_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "renew on party",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* greater_heal([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "greater heal",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ { NextAction("heal") },
            /*C*/ {}
        );
    }
    static ActionNode* greater_heal_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "greater heal on party",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ { NextAction("heal on party") },
            /*C*/ {}
        );
    }
    static ActionNode* heal([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "heal",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ { NextAction("lesser heal") },
            /*C*/ {}
        );
    }
    static ActionNode* heal_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "heal on party",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ { NextAction("lesser heal on party") },
            /*C*/ {}
        );
    }
    static ActionNode* lesser_heal([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "lesser heal",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* lesser_heal_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "lesser heal on party",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* flash_heal([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "flash heal",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ { NextAction("greater heal") },
            /*C*/ {}
        );
    }
    static ActionNode* flash_heal_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "flash heal on party",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ { NextAction("greater heal on party") },
            /*C*/ {}
        );
    }
    static ActionNode* psychic_scream([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "psychic scream",
            /*P*/ {},
            /*A*/ { NextAction("fade") },
            /*C*/ {}
        );
    }
    // [b31_deep] Hymn of Hope is not castable in Shadowform (StancesNot bit 27).
    static ActionNode* hymn_of_hope([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "hymn of hope",
            /*P*/ { NextAction("remove shadowform") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    // [b31_deep] Inner Focus (off the GCD) goes into the expensive heals instead of the first spell after medium
    // mana; a missing talent or a cooldown makes the prereq impossible and the heal goes out alone.
    static ActionNode* divine_hymn([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "divine hymn",
            /*P*/ { NextAction("inner focus") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* prayer_of_healing_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "prayer of healing on party",
            /*P*/ { NextAction("inner focus") },
            /*A*/ {},
            /*C*/ {}
        );
    }
    static ActionNode* greater_heal_on_main_tank([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "greater heal on main tank",
            /*P*/ { NextAction("inner focus") },
            /*A*/ {},
            /*C*/ {}
        );
    }
};

class CurePriestStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    CurePriestStrategyActionNodeFactory()
    {
        creators["abolish disease"] = &abolish_disease;
        creators["abolish disease on party"] = &abolish_disease_on_party;
    }

private:

    static ActionNode* abolish_disease([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "abolish disease",
            /*P*/ {},
            /*A*/ { NextAction("cure disease") },
            /*C*/ {}
        );
    }

    static ActionNode* abolish_disease_on_party([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "abolish disease on party",
            /*P*/ {},
            /*A*/ { NextAction("cure disease on party") },
            /*C*/ {}
        );
    }
};

#endif
