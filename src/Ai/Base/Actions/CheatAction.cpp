/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CheatAction.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"

// Bots play under the same rules as players (user decision 2026-10-23): PlayerbotAI::HasCheat() is always false,
// so "cheat +gold" and friends could only ever print a mask that does nothing. The command now changes nothing
// and says so, instead of listing [conf:...] cheats from Playerbots.BotCheats as if they were active.
bool CheatAction::Execute(Event /*event*/)
{
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "cheats_disabled", "Cheats are disabled: I play by the same rules as you.", {}));
    return true;
}
