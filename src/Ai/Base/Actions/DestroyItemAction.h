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

// [econ70] USER RULE 2026-11-13 («Я хочу, чтобы удаление было 0. Ты либо продаешь, либо передаешь, либо используешь,
// но всегда 0. Никто никогда не удаляет вещи.»): a bot has three disposals and deletion is none of them - it sells
// the item, it hands it to a member who wants it, or it uses it. This action therefore destroys nothing at all. At
// the bag pressure that used to start a destroy it names what the bags hold and by which of the three each leaves,
// and the slot comes free where those routes run: the SELL task at the next vendor ([inv380] marks it, [vendor15]
// runs it), the trade or mail to a member ([guild81]), the five's provisioning ([dun208] / [dun230]) and the
// disenchant queue ([craft409]). [econ61] was the half of this rule that kept only what a vendor buys or the guild
// wants; the rest of it is closed here.
class SmartDestroyItemAction : public DestroyItemAction
{
public:
    SmartDestroyItemAction(PlayerbotAI* botAI) : DestroyItemAction(botAI, "smart destroy") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // what the bags hold and how each item leaves without a deletion; one line per change, the maintenance
    // strategy's "random" trigger asks far more often than the answer changes
    void SayDisposals();
    std::string lastSaid_;
};

#endif
