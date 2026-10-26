/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MTShared.h"
#include "Unit.h"

namespace ManaTombs
{
bool HasDarkShell(Unit* unit)
{
    return unit && (unit->HasAura(SPELL_DARK_SHELL) || unit->HasAura(SPELL_DARK_SHELL_HEROIC));
}
}
