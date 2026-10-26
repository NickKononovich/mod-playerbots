/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

// With StartHeroicPlayerLevel lowered, a death knight can start far below the level of the Acherus quest
// line. Such a death knight begins in its race's own starting area instead, like any other new character.
namespace
{
    constexpr uint8 ACHERUS_START_LEVEL = 55;
}

class PlayerbotsLowLevelDeathKnightStart : public PlayerScript
{
public:
    PlayerbotsLowLevelDeathKnightStart()
        : PlayerScript("PlayerbotsLowLevelDeathKnightStart", {PLAYERHOOK_ON_FIRST_LOGIN})
    {
    }

    void OnPlayerFirstLogin(Player* player) override
    {
        if (player->getClass() != CLASS_DEATH_KNIGHT || player->GetLevel() >= ACHERUS_START_LEVEL)
            return;

        PlayerInfo const* raceStart = sObjectMgr->GetPlayerInfo(player->getRace(), CLASS_WARRIOR);
        if (!raceStart)
            raceStart = sObjectMgr->GetPlayerInfo(player->getRace(), CLASS_ROGUE);
        if (!raceStart)
            return;

        WorldLocation const start(raceStart->mapId, raceStart->positionX, raceStart->positionY, raceStart->positionZ,
                                  raceStart->orientation);
        player->SetHomebind(start, raceStart->areaId);
        player->TeleportTo(start);
    }
};

void AddPlayerbotsLowLevelDeathKnightStart()
{
    new PlayerbotsLowLevelDeathKnightStart();
}
