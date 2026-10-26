/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CONSUMABLEACTIONS_H
#define PLAYERBOTS_CONSUMABLEACTIONS_H

#include "MovementActions.h"
#include "UseItemAction.h"

class PlayerbotAI;

// Uses the bag item a guid value points to; the value only ever names an item the bot carries.
class UseValueItemAction : public UseItemAction
{
public:
    UseValueItemAction(PlayerbotAI* botAI, std::string const name, std::string const itemValue)
        : UseItemAction(botAI, name, true), itemValue(itemValue)
    {
    }

    bool Execute(Event event) override;
    bool isPossible() override;

protected:
    Item* GetValueItem();

    std::string const itemValue;
};

class UseRaidFlaskAction : public UseValueItemAction
{
public:
    UseRaidFlaskAction(PlayerbotAI* botAI) : UseValueItemAction(botAI, "use raid flask", "raid flask") {}

    bool isUseful() override;
};

class UseFoodBuffAction : public UseValueItemAction
{
public:
    UseFoodBuffAction(PlayerbotAI* botAI) : UseValueItemAction(botAI, "use food buff", "raid food buff") {}

    bool isUseful() override;
};

class UsePrePullPotionAction : public UseValueItemAction
{
public:
    UsePrePullPotionAction(PlayerbotAI* botAI) : UseValueItemAction(botAI, "use pre pull potion", "pre pull potion") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    uint32 _usedForReadyMs = 0;
};

// Healthstones only: a name search for "healthstone" also matches the Glyph of Healthstone.
class UseHealthstoneAction : public UseItemAction
{
public:
    UseHealthstoneAction(PlayerbotAI* botAI) : UseItemAction(botAI, "healthstone", true) {}

    bool Execute(Event event) override;
    bool isPossible() override;
};

class UseGameObjectNearbyAction : public MovementAction
{
public:
    UseGameObjectNearbyAction(PlayerbotAI* botAI, std::string const name, std::string const objectValue)
        : MovementAction(botAI, name), objectValue(objectValue)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    std::string const objectValue;
};

class UseSoulwellAction : public UseGameObjectNearbyAction
{
public:
    UseSoulwellAction(PlayerbotAI* botAI) : UseGameObjectNearbyAction(botAI, "use soulwell", "nearby soulwell") {}
};

// The Warlock's Ritual of Souls needs two more group members to click the portal.
class ClickSoulPortalAction : public UseGameObjectNearbyAction
{
public:
    ClickSoulPortalAction(PlayerbotAI* botAI)
        : UseGameObjectNearbyAction(botAI, "click soul portal", "nearby soul portal")
    {
    }
};

#endif
