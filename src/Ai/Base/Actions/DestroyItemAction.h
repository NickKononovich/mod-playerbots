/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DESTROYITEMACTION_H
#define PLAYERBOTS_DESTROYITEMACTION_H

#include "InventoryAction.h"

class FindItemVisitor;
class PlayerbotAI;

class DestroyItemAction : public InventoryAction
{
public:
    DestroyItemAction(PlayerbotAI* botAI, std::string const name = "destroy") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;

protected:
    // Destroys every match. An unsaved Item is deleted here, so callers must not hold Item* across this.
    void DestroyItem(FindItemVisitor* visitor);
};

class SmartDestroyItemAction : public DestroyItemAction
{
public:
    SmartDestroyItemAction(PlayerbotAI* botAI) : DestroyItemAction(botAI, "smart destroy") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool DestroyUntilBagSpace(std::vector<uint32> const& itemIds);
};

// [inv380] The quest-starter items and quest items the global leader's rule found the bot carries for nothing
// (GuildCoordinator::QuestDeadWeight). No vendor buys either kind, so the slot is freed by dropping them; unlike
// "smart destroy" this runs at any bag pressure, because the slot buys the bot nothing at all.
class DropDeadWeightAction : public DestroyItemAction
{
public:
    DropDeadWeightAction(PlayerbotAI* botAI) : DestroyItemAction(botAI, "drop dead weight") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
