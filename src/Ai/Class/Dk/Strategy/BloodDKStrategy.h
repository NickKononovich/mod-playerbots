/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BLOODDKSTRATEGY_H
#define PLAYERBOTS_BLOODDKSTRATEGY_H

#include "GenericDKStrategy.h"

class PlayerbotAI;

// [b31_deep] The Blood tank's Death Coil only at >= 80 runic power once it knows Rune Strike (wowsims blood_aggro;
// Rune Strike, Icebound Fortitude and Anti-Magic Shell need 20 each). Build 30: Death Coil 4.0/min from the default
// list at 40. Below level 67 (no Rune Strike) Death Coil stays the runic power spender.
inline constexpr uint32 BLOOD_DEATH_COIL_RUNIC_POWER = 800;  // internal units (x10)

class BloodDeathCoilMultiplier : public Multiplier
{
public:
    BloodDeathCoilMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "blood death coil") {}

    float GetValue(Action* action) override;
};

// [b36_tankaoe] A Blood tank with 3+ mobs on it keeps one rune of each type for Death and Decay (Blood + Frost +
// Unholy, the 3.3.5 tank's main area threat) while it is about to come off cooldown: the single-target rune spenders
// (Icy Touch, Plague Strike, Death Strike, Heart Strike, Blood Strike) wait instead of spending the last rune of a
// type it still needs; a spare rune (the second of a pair) stays free. Build 35 L80 trash, 3+ mobs on the tank:
// "death and decay ... NOT_READY" in 1 of 4 tries, Death and Decay 1.2 a minute of its 2, Icy Touch 6.2 and Plague
// Strike 4.4 a minute. Off at low health (Death Strike heals) and once Death and Decay is ready with all its runes
// and still not cast (its trigger ranks above the spenders: it did not want it): the rotation cannot stall on it.
class BloodDnDRuneReserveMultiplier : public Multiplier
{
public:
    static constexpr uint32 MIN_MOBS = 3;
    static constexpr float PILE_RANGE = 10.0f;            // Death and Decay's radius round the tank
    static constexpr uint32 READY_WINDOW_MS = 2000;       // the cooldown left from which the runes are kept
    static constexpr uint8 DND_RUNE_TYPES = 3;            // Death and Decay costs a Blood, a Frost and an Unholy rune
    static constexpr float DEATH_STRIKE_FREE_HEALTH_PCT = 60.0f;

    BloodDnDRuneReserveMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "blood dnd rune reserve") {}

    float GetValue(Action* action) override;
};

class BloodDKStrategy : public GenericDKStrategy
{
public:
    BloodDKStrategy(PlayerbotAI* botAI);

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "blood"; }
    std::vector<NextAction> getDefaultActions() override;
    uint32 GetType() const override { return STRATEGY_TYPE_TANK | STRATEGY_TYPE_MELEE; }

protected:
    // [rot370] the wowsims blood_aggro APL never casts Blood Boil; Heart Strike already hits a second target
    bool CastsBloodBoil() const override { return false; }
    // [rot563] blood_aggro: Icy Touch / Plague Strike once a disease is gone, no Pestilence refresh. b276 rfc-h 5G
    // Rtdekac: Pestilence 3.2 a minute on the bosses vs the sim's 0, Icy Touch 2.3 / Plague Strike 1.8 vs 4.2 / 4.3
    bool RefreshesDiseasesWithPestilence() const override { return false; }
};

#endif
