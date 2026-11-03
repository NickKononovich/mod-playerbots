/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GENERICDKSTRATEGY_H
#define PLAYERBOTS_GENERICDKSTRATEGY_H

#include "MeleeCombatStrategy.h"
#include "Multiplier.h"

class PlayerbotAI;

// [b31_dk] Icy Touch / Plague Strike only where they add a disease: build 30 Frost DKs cast Pestilence 3.4/min on
// single-target bosses plus Icy Touch 1.9 and Plague Strike 1.8 (GUIDE_AUDIT_DPS.md 2.9; Blood the same,
// GUIDE_AUDIT_TANKS.md section 3): the engine keeps a triggered action queued after the tick it was pushed on, and the
// spells' isUseful() does not look at the disease, so an Icy Touch queued next to the Pestilence refresh ran right
// after it on the fresh disease. 0 for the two while our disease on the target has more than DK_DISEASE_REFRESH_MS
// left, and while Pestilence with the Glyph of Disease can refresh both (both up, Pestilence ready, a Blood or Death
// rune ready before the disease runs out, in melee).
// [b31_deep] Unholy too, once it knows Scourge Strike (requireScourgeStrike): its Icy Touch / Plague Strike fallbacks
// on the rune triggers and the default fillers spent a lone Frost or Unholy rune on a fresh disease instead of
// keeping it for the Scourge Strike pair (wowsims uh_2h_ss: Icy Touch / Plague Strike only as the disease runs out).
// Without Scourge Strike (below the talent) the two stay Unholy's rune spenders.
class DKDiseaseRefreshMultiplier : public Multiplier
{
public:
    DKDiseaseRefreshMultiplier(PlayerbotAI* botAI, bool requireScourgeStrike = false)
        : Multiplier(botAI, "dk disease refresh"), requireScourgeStrike(requireScourgeStrike)
    {
    }

    float GetValue(Action* action) override;

private:
    bool requireScourgeStrike;
};

class GenericDKStrategy : public MeleeCombatStrategy
{
public:
    GenericDKStrategy(PlayerbotAI* botAI);

    std::string const getName() override { return "DK"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;

protected:
    // [rot370] false for the Blood tank: wowsims tank_deathknight__blood_aggro never casts Blood Boil
    virtual bool CastsBloodBoil() const { return true; }
};

#endif
