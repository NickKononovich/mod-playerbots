/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GDVALUECONTEXT_H
#define PLAYERBOTS_GDVALUECONTEXT_H

#include "GDTriggers.h"
#include "NamedObjectContext.h"
#include "ObjectGuid.h"
#include "Value.h"

class SladranAssignedSnakeWrapValue : public CalculatedValue<ObjectGuid>
{
public:
    SladranAssignedSnakeWrapValue(PlayerbotAI* botAI)
        : CalculatedValue<ObjectGuid>(botAI, "slad'ran snake wrap target", GundrakSladran::SNAKE_WRAP_SCAN_INTERVAL)
    {
    }

protected:
    ObjectGuid Calculate() override { return GundrakSladran::CalculateAssignedSnakeWrap(botAI); }
};

class GundrakAltarToUseValue : public CalculatedValue<ObjectGuid>
{
public:
    GundrakAltarToUseValue(PlayerbotAI* botAI)
        : CalculatedValue<ObjectGuid>(botAI, "gundrak altar to use", GundrakEvent::ALTAR_SCAN_INTERVAL)
    {
    }

protected:
    ObjectGuid Calculate() override { return GundrakEvent::FindAltarToUse(botAI); }
};

class GundrakBridgeOpenValue : public CalculatedValue<bool>
{
public:
    GundrakBridgeOpenValue(PlayerbotAI* botAI)
        : CalculatedValue<bool>(botAI, "gundrak bridge open", GundrakEvent::ALTAR_SCAN_INTERVAL)
    {
    }

protected:
    bool Calculate() override { return GundrakEvent::IsBridgeOpen(bot); }
};

class WotlkDungeonGDValueContext : public NamedObjectContext<UntypedValue>
{
public:
    WotlkDungeonGDValueContext()
    {
        creators["slad'ran snake wrap target"] = &WotlkDungeonGDValueContext::sladran_snake_wrap_target;
        creators["gundrak altar to use"] = &WotlkDungeonGDValueContext::gundrak_altar_to_use;
        creators["gundrak bridge open"] = &WotlkDungeonGDValueContext::gundrak_bridge_open;
    }

private:
    static UntypedValue* sladran_snake_wrap_target(PlayerbotAI* botAI)
    {
        return new SladranAssignedSnakeWrapValue(botAI);
    }
    static UntypedValue* gundrak_altar_to_use(PlayerbotAI* botAI) { return new GundrakAltarToUseValue(botAI); }
    static UntypedValue* gundrak_bridge_open(PlayerbotAI* botAI) { return new GundrakBridgeOpenValue(botAI); }
};

#endif
