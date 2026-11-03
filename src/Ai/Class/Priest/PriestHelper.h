/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PRIESTHELPER_H
#define PLAYERBOTS_PRIESTHELPER_H

#include "Define.h"

class Player;
class PlayerbotAI;
class Unit;

namespace ai::priest
{
static constexpr uint32 VAMPIRIC_TOUCH_REFRESH_MS = 1500; // [rot351] the unhasted cast time (no spell id known)
static constexpr uint32 DOT_REFRESH_MS = 1000;
// [rot351] wowsims shadow_priest__default.apl.json: Vampiric Touch when its dot has <= its hasted cast time left
// (row 6), Devouring Plague only once it is gone (row 7, "not dotIsActive"). A recast that lands before the dot's
// last tick costs that tick in the core (Aura::RefreshTimers keeps the periodic timer and resets the tick count: the
// pending tick becomes the new dot's first and the old one ends a tick short). b223 rfc-h 5B Rtpriaa: Vampiric Touch
// 5.7 a minute vs the sim's 5.0 at 6436 a cast vs 8687 (74 %), refreshed at cast time + 500 ms; Devouring Plague
// 4530 a cast vs 5149 (88 %), refreshed at 1 s left.
static constexpr uint32 DEVOURING_PLAGUE_REFRESH_MS = 0;
static constexpr float DOT_MIN_LIFETIME = 8.0f;
// [rot210] wowsims shadow_priest__default: Vampiric Touch while the fight has >= 3 s left; Devouring Plague whenever
// it is not up (no lifetime condition) and once more while the fight has <= 1.75 s left (the Improved Devouring
// Plague hit lands at once)
static constexpr float VAMPIRIC_TOUCH_MIN_LIFETIME = 3.0f;
static constexpr float DEVOURING_PLAGUE_MIN_LIFETIME = 0.0f;
static constexpr float DEVOURING_PLAGUE_FINAL_CAST_SEC = 1.75f;

// Shadow Weaving (3.3.5): a buff on the priest, +2 % shadow damage done per stack, up to 5 stacks.
// Shadow Word: Pain snapshots damage-done modifiers when it is applied.
static constexpr uint32 SPELL_SHADOW_WEAVING_RANK_1 = 15257;
static constexpr uint32 SPELL_SHADOW_WEAVING_RANK_2 = 15331;
static constexpr uint32 SPELL_SHADOW_WEAVING_RANK_3 = 15332;
static constexpr uint32 SPELL_SHADOW_WEAVING_BUFF = 15258;
static constexpr uint8 SHADOW_WEAVING_MAX_STACKS = 5;
// Pain and Suffering 3/3: every Mind Flay hit refreshes Shadow Word: Pain (100 % chance).
static constexpr uint32 SPELL_PAIN_AND_SUFFERING_RANK_3 = 47582;

// [rot230] wowsims shadow_priest__default: Shadow Word: Pain at 5 Shadow Weaving stacks while the fight has >= 75 s
// left, at >= 3 stacks once it has less. The core keeps the cast's damage-done multiplier (Shadow Weaving) and crit
// chance through every Pain and Suffering refresh (spell_pri_pain_and_suffering_proc: RefreshTimersWithMods +
// CalculateAmount with the stored pct mods, no CalculatePeriodicData) and re-takes only spell power, as wowsims'
// Dot.Rollover does - in a long fight the 5-stack cast pays back the wait, in a short one it does not
static constexpr uint8 SHADOW_WEAVING_EARLY_STACKS = 3;
static constexpr float SHADOW_WORD_PAIN_EARLY_FIGHT_SEC = 75.0f;

// True when Shadow Word: Pain should be applied now on `target`: the priest has no Shadow Weaving talent, has 5
// stacks, or has >= 3 stacks with the fight's remaining time under SHADOW_WORD_PAIN_EARLY_FIGHT_SEC.
bool ShadowWeavingReady(PlayerbotAI* botAI, Player* bot, Unit* target);
// [rot230] The fight's remaining seconds as the APL's remainingTime: the longest ExpectedLifetime of `target` and the
// group's other attackers (a pack's first kill is no end of the fight).
float FightRemainingSec(PlayerbotAI* botAI, Unit* target);
// Refresh window of Shadow Word: Pain: 0 (never re-cast while up) when Mind Flay keeps it rolling.
uint32 ShadowWordPainRefreshMs(Player* bot);
// Refresh window of Vampiric Touch: its hasted cast time ([rot351] the APL's row 6, no margin).
uint32 VampiricTouchRefreshMs(Player* bot, uint32 spellId);
// [rot210] The APL's "remainingTime <= 1.75 s" Devouring Plague: the fight ends within DEVOURING_PLAGUE_FINAL_CAST_SEC
// - `target` dies by then (ExpectedLifetime) and no other attacker of the group outlives it.
bool DevouringPlagueFinalCast(PlayerbotAI* botAI, Unit* target);
}

#endif
