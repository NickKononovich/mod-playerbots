/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTHARNESSPREPARE_H
#define PLAYERBOTS_TESTHARNESSPREPARE_H

#include "TestHarnessData.h"

#include <string>
#include <utility>
#include <vector>

class Player;

namespace TestHarness
{
    struct PrepareResult
    {
        bool ok = false;
        std::string error;
        float avgIlvl = 0.0f;    // 17 slots, a two-hander counts twice
        float coreAvgIlvl = 0.0f;  // Player::GetAverageItemLevel, for comparison with the dungeon finder
        uint32 gearScore = 0;
        uint32 ilvlCap = 0;      // gear limit the factory was given for the final pass
        uint32 gearPasses = 0;
        int32 specNo = -1;
        uint8 level = 0;
        uint16 professionSkill = 0;
        std::pair<uint16, uint16> professions{0, 0};
        uint32 dragonsEyes = 0;
        std::vector<std::string> perks;
        // [b31_dk_talents] Talent points after the talent step: spent (Player::GetUsedTalentCount equivalent:
        // expected - free when the core grants them all), expected (the level's full points, as the core grants them
        // outside Ebon Hold), the core's own CalculateTalentsPoints() at that moment, the free-points field and the
        // map the bot stood on. talentsSpent < talentsExpected is a talent shortfall (event "talent_shortfall").
        uint32 talentsSpent = 0;
        uint32 talentsExpected = 0;
        uint32 talentsCore = 0;
        uint32 talentsFree = 0;
        uint32 talentsMap = 0;
        uint32 dkChainQuestsMarked = 0;
    };

    // Test characters only: level (raised, never lowered), talents (premade spec, as many points as the level
    // has), class and trainer spells of the level, two professions at the skill of the level (450 at 80) with
    // their gear perks, gear whose average item level lands near targetIlvl (blue at most below 80, epic at 80),
    // gems / enchants from Playerbots.MinEnchantingBotLevel, and the consumables of the level for the whole run.
    // World thread; the character must be online as a bot. content: the run's dungeon or raid, whose own boss loot
    // is kept out of the gear (none for a plain prepare).
    PrepareResult PrepareCharacter(Player* bot, SpecDef const& spec, uint8 level, uint32 targetIlvl,
                                   std::pair<uint16, uint16> professions, bool groupHasWarlock,
                                   ContentDef const* content = nullptr);

    // Average over the equipment slots the factory fills at the character's level (17 at 50+, a two-hander
    // counts twice): trinkets come at 50, neck and head at 30, rings at 20.
    float AverageItemLevel(Player* bot);
    int32 FindPremadeSpec(uint8 cls, std::string const& name);
}

#endif
