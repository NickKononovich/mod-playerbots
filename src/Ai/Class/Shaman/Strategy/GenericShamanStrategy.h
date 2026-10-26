/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GENERICSHAMANSTRATEGY_H
#define PLAYERBOTS_GENERICSHAMANSTRATEGY_H

#include "CombatStrategy.h"

class PlayerbotAI;

// Resto dispels sit between its low (17-19) and medium (14-16) health heals.
inline constexpr float SHAMAN_RESTO_DISPEL_RELEVANCE = 16.6f;
inline constexpr float SHAMAN_RESTO_URGENT_DISPEL_RELEVANCE = 22.0f;  // between low (17-19) and critical heals

class GenericShamanStrategy : public CombatStrategy
{
public:
    GenericShamanStrategy(PlayerbotAI* botAI);

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class ShamanCureStrategy : public Strategy
{
public:
    ShamanCureStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "cure"; }
};

class ShamanBoostStrategy : public Strategy
{
public:
    ShamanBoostStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "boost"; }
};

class ShamanAoeStrategy : public CombatStrategy
{
public:
    ShamanAoeStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "aoe"; }
};

#endif
