/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MTTRIGGERS_H
#define PLAYERBOTS_MTTRIGGERS_H

#include "Trigger.h"

// Nexus-Prince Shaffar: a damage dealer in the fight while a beacon or an apprentice is up.
class ShaffarBeaconUpTrigger : public Trigger
{
public:
    ShaffarBeaconUpTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shaffar beacon up") {}
    bool IsActive() override;
};

// Pandemonius: Dark Shell is up and the bot is still hitting or casting at him.
class PandemoniusDarkShellTrigger : public Trigger
{
public:
    PandemoniusDarkShellTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pandemonius dark shell") {}
    bool IsActive() override;
};

#endif
