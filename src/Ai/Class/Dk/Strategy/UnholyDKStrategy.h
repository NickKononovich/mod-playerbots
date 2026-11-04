/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_UNHOLYDKSTRATEGY_H
#define PLAYERBOTS_UNHOLYDKSTRATEGY_H

#include "GenericDKStrategy.h"

class PlayerbotAI;

// [rot_b31_pdd] While Summon Gargoyle waits for its fight (IsGargoyleWaiting), Death Coil (40) only spends what
// lies above the Gargoyle's 60 runic power.
class GargoylePoolingMultiplier : public Multiplier
{
public:
    GargoylePoolingMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "gargoyle pooling") {}

    float GetValue(Action* action) override;
};

// [b35_uh] Rune pooling for Death and Decay (wowsims unholy_dw_ss: Scourge Strike only while Death and Decay is more
// than 6 s from ready; Blood Strike and Blood Boil the same, or with Empower Rune Weapon ready). 0 for "scourge
// strike", "blood strike" and "blood boil" while Death and Decay is known, at most DK_DND_POOL_MS from ready and the
// current target lives long enough for it to tick DK_DND_MIN_TICK_S. Blood Strike / Blood Boil stay free while Empower
// Rune Weapon is ready in its boss fight ([rot451]; Scourge Strike while Summon Gargoyle is), Blood Strike also with
// no Desolation at all (the APL's "Blood Strike if no Desolation" sits above Death and Decay, unconditioned). Death
// and Decay ready but not cast (threat hold, range) holds only while its runes are missing, so a Death and Decay that
// cannot go out does not freeze the rotation. Icy Touch / Plague Strike are not held: the APL refreshes the diseases
// above everything (<= 3 s), and a non-refresh cast on a fresh disease is already 0 in DKDiseaseRefreshMultiplier.
// Logged: Death and Decay 2.2/min against the sim's 3.7, 904 NOT_READY (DEEP_AUDIT_DK_UNHOLY_NIGHT.md B5).
class DeathAndDecayPoolingMultiplier : public Multiplier
{
public:
    DeathAndDecayPoolingMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "death and decay pooling") {}

    float GetValue(Action* action) override;
};

class UnholyDKStrategy : public GenericDKStrategy
{
public:
    UnholyDKStrategy(PlayerbotAI* botAI);

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "unholy"; }
    std::vector<NextAction> getDefaultActions() override;
    uint32 GetType() const override { return STRATEGY_TYPE_DPS | STRATEGY_TYPE_MELEE; }
};

class UnholyDKAoeStrategy : public CombatStrategy
{
public:
    UnholyDKAoeStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "unholy aoe"; }
};

#endif
