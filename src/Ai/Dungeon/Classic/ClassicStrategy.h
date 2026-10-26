/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CLASSICSTRATEGY_H
#define PLAYERBOTS_CLASSICSTRATEGY_H

#include "Strategy.h"

// Scarlet Monastery: Arcanist Doan; the Cathedral chapel that Commander Mograine's engage sends (ClassicShared.h).
class ClassicDungeonScarletMonasteryStrategy : public Strategy
{
public:
    ClassicDungeonScarletMonasteryStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "classic-sm"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// Gnomeregan trash: Dark Iron Land Mines and Irradiated Slime Radiation Clouds (see DungeonHazard.cpp); the Mobile
// Alert System dies first (DungeonTactics.cpp, [b35_gnomer]).
class ClassicDungeonGnomereganStrategy : public Strategy
{
public:
    ClassicDungeonGnomereganStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "classic-gnomer"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;

    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;

private:
    static constexpr uint32 NPC_DARK_IRON_LAND_MINE = 8035;
    static constexpr uint32 NPC_DARK_IRON_LAND_MINE_HEROIC = 708035;
    static constexpr uint32 MINE_LOG_MS = 10000;
    uint32 _mineLogMs = 0;
};

// Blackrock Spire, the Rookery: Rookery Eggs (DungeonHazard.cpp) and the Rookery Hatchers first (DungeonTactics.cpp).
class ClassicDungeonBlackrockSpireStrategy : public Strategy
{
public:
    ClassicDungeonBlackrockSpireStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "classic-brs"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;

    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;

private:
    static constexpr uint32 NPC_ROOKERY_HATCHER = 10683;
    static constexpr uint32 NPC_ROOKERY_HATCHER_HEROIC = 710683;
};

// Scholomance caster packs: the Acolytes and Spectral Researchers die first, their heals and volleys are kicked,
// ranged and healers keep out of the Acolytes' Mana Burn, and the group goes back up the entrance ramp after a fight
// on the hall floor (ClassicShared.h, DungeonTactics.cpp).
class ClassicDungeonScholomanceStrategy : public Strategy
{
public:
    ClassicDungeonScholomanceStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "classic-scholo"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;

    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;

private:
    static constexpr uint32 NPC_SCHOLOMANCE_ACOLYTE = 10471;
    static constexpr uint32 NPC_SPECTRAL_RESEARCHER = 10499;
};

// Sunken Temple: the Atal'ai statues in order (event steps, ClassicShared.h).
class ClassicDungeonSunkenTempleStrategy : public Strategy
{
public:
    ClassicDungeonSunkenTempleStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "classic-st"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    // [b30_st_spirit] the Atal'ai Deathwalker's Spirit: walked away from (DungeonHazard.cpp), never a target
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;

    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;

private:
    static constexpr uint32 NPC_DEATHWALKER_SPIRIT = 8317;
    static constexpr uint32 NPC_DEATHWALKER_SPIRIT_HEROIC = 708317;
};

// Zul'Farrak: the troll cages (pyramid event), Weegli's door and the gong (event steps, ClassicShared.h).
class ClassicDungeonZulFarrakStrategy : public Strategy
{
public:
    ClassicDungeonZulFarrakStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "classic-zf"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// Caverns of Time: Old Hillsbrad Foothills: Erozion's bombs, the five barrels and Thrall's stops (event steps,
// ClassicShared.h; kept with the classic event starters).
class TbcDungeonOldHillsbradStrategy : public Strategy
{
public:
    TbcDungeonOldHillsbradStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "tbc-ohb"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// Maraudon trash: Noxious Slime Noxious Clouds (see DungeonHazard.cpp).
class ClassicDungeonMaraudonStrategy : public Strategy
{
public:
    ClassicDungeonMaraudonStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "classic-mara"; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

#endif
