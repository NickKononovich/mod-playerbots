/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_FORCEREBUFF_H
#define PLAYERBOTS_FORCEREBUFF_H

#include "Define.h"
#include "Strategy.h"
#include <string>
#include <vector>

class Player;
class PlayerbotAI;
class SpellInfo;

class ForceRebuffState
{
public:
    explicit ForceRebuffState(Player* owner = nullptr) : bot(owner) {}

    ForceRebuffState(ForceRebuffState const&) = delete;
    ForceRebuffState& operator=(ForceRebuffState const&) = delete;

    void Begin(bool reply);
    void End() { pending = false; }
    bool IsPending() const;
    bool ShouldReplyToReadyCheck() const { return replyToReadyCheck; }
    uint32 BeganMs() const { return beginMs; }  // [lead37] the boss-prep log

    // [lead1095] `spellId`: the buff's spell (0: unknown); a buff its owner keeps up (OwnerMaintained) is no work of
    // the window
    void NoteBuffWork(uint32 spellId = 0);
    void NoteBuffProposed(uint32 spellId = 0);
    // [lead1095] user 2026-11-04 (Q12 addendum): a short self-maintained buff, by spell data - one with charges that
    // its use spends (Water / Lightning Shield, Holy Shield, Bone Shield, Inner Fire ...), or one whose full duration
    // is shorter than the next fight (the planner's estimate) - is kept up by its owner on the go and in fights: it is
    // not part of the pre-pull pass and never holds it; the long buffs are
    bool OwnerMaintained(uint32 spellId) const;
    void RollBuffPendingCycle() { buffPendingThisCycle = false; buffProposedThisCycle = false; }
    bool IsBuffPendingThisCycle() const { return buffPendingThisCycle; }
    bool IsBuffProposedThisCycle() const { return buffProposedThisCycle; }

    void NoteCast(SpellInfo const* spellInfo) { lastGcdSpell = spellInfo; }
    bool IsOnGlobalCooldown() const;

    bool BuffBelowRefreshTarget(int32 remaining, int32 maxDuration, uint32 baseBeforeDuration) const;

private:
    Player* bot;
    bool pending = false;
    bool replyToReadyCheck = false;
    bool buffPendingThisCycle = false;
    bool buffProposedThisCycle = false;
    SpellInfo const* lastGcdSpell = nullptr;
    uint32 beginMs = 0;
};

class ForceRebuffStrategy : public Strategy
{
public:
    ForceRebuffStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "force rebuff"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

#endif
