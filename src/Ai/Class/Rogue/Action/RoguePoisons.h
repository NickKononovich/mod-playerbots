/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROGUEPOISONS_H
#define PLAYERBOTS_ROGUEPOISONS_H

#include "Define.h"

#include <vector>

class Item;
class Player;
class PlayerbotAI;

// [b31_wr] The one place that decides which poison goes on which hand, used by the rogue's poison actions
// (RogueActions.cpp), "apply poison" (ImbueAction.cpp) and the poisons the factory stocks (PlayerbotFactory.cpp).
//  - Assassination (Mutilate, two daggers): Deadly main hand / Instant off hand (wowsims preset default).
//  - Combat / Subtlety: Instant main hand / Deadly off hand.
//  - Deadly from two rogues does not stack: the hand that would get Deadly takes Wound when another rogue of the group
//    already brings it (a non-bot rogue with Deadly on a weapon, or a bot rogue of 30+ with a lower GUID, so two bots
//    settle on the same answer without waiting for each other's weapons).
namespace RoguePoisons
{
    enum class Kind : uint8
    {
        INSTANT,
        DEADLY,
        WOUND
    };

    // Item ids of the kind, best rank first
    std::vector<uint32> const& ItemIds(Kind kind);

    // The kind this rogue puts on slot (EQUIPMENT_SLOT_MAINHAND / EQUIPMENT_SLOT_OFFHAND)
    Kind KindFor(PlayerbotAI* botAI, uint8 slot);

    // The best poison in the bags for slot; when the kind is missing: Instant -> Deadly, Deadly -> Instant,
    // Wound -> Instant (never Deadly, that is the point of Wound). nullptr when there is none.
    Item* ItemFor(PlayerbotAI* botAI, uint8 slot);

    // Whether a weapon of player carries a Deadly Poison temporary enchant
    bool HasDeadlyOnWeapon(Player* player);
}

#endif
