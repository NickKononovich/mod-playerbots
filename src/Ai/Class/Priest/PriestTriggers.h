/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PRIESTTRIGGERS_H
#define PLAYERBOTS_PRIESTTRIGGERS_H

#include "CureTriggers.h"
#include "PriestHelper.h"
#include "SharedDefines.h"
#include "Trigger.h"
#include <set>

class PlayerbotAI;

DEBUFF_CHECKISOWNER_TRIGGER(HolyFireTrigger, "holy fire");
// [b31_deep] never the current target: its Shadow Word: Pain waits for 5 Shadow Weaving stacks (ShadowWordPainTrigger)
DEBUFF_ENEMY_TRIGGER_A(ShadowWordPainOnAttackerTrigger, "shadow word: pain");
DEBUFF_ENEMY_TRIGGER(VampiricTouchOnAttackerTrigger, "vampiric touch");

class PriestDotTrigger : public DebuffTrigger
{
public:
    PriestDotTrigger(PlayerbotAI* botAI, std::string const spell, uint32 refreshMs,
                     float minLifetime = ai::priest::DOT_MIN_LIFETIME)
        : DebuffTrigger(botAI, spell, 1, true, minLifetime, refreshMs) {}
};

class ShadowWordPainTrigger : public PriestDotTrigger
{
public:
    ShadowWordPainTrigger(PlayerbotAI* botAI)
        : PriestDotTrigger(botAI, "shadow word: pain", ai::priest::DOT_REFRESH_MS) {}

    // Waits for 5 Shadow Weaving stacks (3 with < 75 s of the fight left, [rot230]); never re-cast while Mind Flay keeps it rolling (Pain and Suffering 3/3).
    bool IsActive() override;
};

class VampiricTouchTrigger : public PriestDotTrigger
{
public:
    VampiricTouchTrigger(PlayerbotAI* botAI)
        : PriestDotTrigger(botAI, "vampiric touch", ai::priest::VAMPIRIC_TOUCH_REFRESH_MS,
                           ai::priest::VAMPIRIC_TOUCH_MIN_LIFETIME) {}

    // Refresh window follows the hasted cast time.
    bool IsActive() override;
};

class DevouringPlagueTrigger : public PriestDotTrigger
{
public:
    DevouringPlagueTrigger(PlayerbotAI* botAI)
        : PriestDotTrigger(botAI, "devouring plague", ai::priest::DOT_REFRESH_MS,
                           ai::priest::DEVOURING_PLAGUE_MIN_LIFETIME) {}

    // [rot210] also while it is up when the fight ends within the APL's 1.75 s (DevouringPlagueFinalCast)
    bool IsActive() override;
};

BUFF_TRIGGER(VampiricEmbraceTrigger, "vampiric embrace");
CURE_TRIGGER(DispelMagicTrigger, "dispel magic", DISPEL_MAGIC);
CURE_PARTY_TRIGGER(DispelMagicPartyMemberTrigger, "dispel magic", DISPEL_MAGIC);
CURE_TRIGGER(CureDiseaseTrigger, "cure disease", DISPEL_DISEASE);
CURE_PARTY_TRIGGER(PartyMemberCureDiseaseTrigger, "cure disease", DISPEL_DISEASE);
BUFF_TRIGGER_A(InnerFireTrigger, "inner fire");
BUFF_TRIGGER_A(ShadowformTrigger, "shadowform");
// Raid utility coordinator: Power Infusion goes to the assigned best caster in range, else to the priest.
class PowerInfusionTrigger : public BoostTrigger
{
public:
    PowerInfusionTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "power infusion") {}

    std::string const GetTargetName() override { return "utility buff target::self"; }
    // [b31_deep] known and off cooldown first: the empty checks ran every tick (976 "on cooldown", 1526 "not known")
    bool IsActive() override;
};
BUFF_TRIGGER(InnerFocusTrigger, "inner focus");
// [rot18] Shadow: Inner Focus ready, at 5 Shadow Weaving stacks, in a fight
class InnerFocusShadowWeavingTrigger : public Trigger
{
public:
    InnerFocusShadowWeavingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "inner focus shadow weaving") {}
    bool IsActive() override;
};
CC_TRIGGER(ShackleUndeadTrigger, "shackle undead");
INTERRUPT_TRIGGER(SilenceTrigger, "silence");
INTERRUPT_HEALER_TRIGGER(SilenceEnemyHealerTrigger, "silence");
BUFF_TRIGGER(TouchOfWeaknessTrigger, "touch of weakness");
DEBUFF_TRIGGER(HexOfWeaknessTrigger, "hex of weakness");
BUFF_TRIGGER(ShadowguardTrigger, "shadowguard");
DEFLECT_TRIGGER(FeedbackTrigger, "feedback");
SNARE_TRIGGER(ChastiseTrigger, "chastise");
// [guide_b31_dead] Shadowfiend: off cooldown in combat with a live target, and mana below 50 %, or a boss target
// (healers on a boss: below 60 % mana). Replaces the BoostTrigger version (balance-gated, unused by any node).
class ShadowfiendTrigger : public Trigger
{
public:
    ShadowfiendTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shadowfiend") {}
    bool IsActive() override;
};

// [guide_b31_dead] Shadowform out of combat (after a rez / login), but not while a party member waits for a rez:
// the rez node removes the form, and re-casting it would loop.
class ShadowformNonCombatTrigger : public Trigger
{
public:
    ShadowformNonCombatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "shadowform out of combat") {}
    bool IsActive() override;
};

// [guide_b31_dead] Clip Mind Flay: it has landed at least one tick, one more tick is left, and the last tick landed
// under 400 ms ago (so no tick is thrown away). [b31_deep] For Vampiric Touch / Devouring Plague due on the channel
// target (wowsims "wait for VT"; [rot18] no longer for Mind Blast, which wowsims never casts), and never while the
// threat strategy holds the next cast: the bot would only re-channel Mind Flay or wand.
class MindFlayClipTrigger : public Trigger
{
public:
    MindFlayClipTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mind flay clip") {}
    bool IsActive() override;

private:
    bool DotDue(Unit* target, std::string const spell, uint32 refreshMs, float minLifetime);
    bool HeldForThreat(Unit* target);
};

// [b31_deep] Mind Sear over the current target's DoTs: 5+ enemies besides the target within the 10 yd it hits
// (HasEnoughAoeTargets counts the target too, so 6). Same cached attacker scan as "medium aoe".
class MindSearHeavyAoeTrigger : public AoeTrigger
{
public:
    MindSearHeavyAoeTrigger(PlayerbotAI* botAI) : AoeTrigger(botAI, 6, 10.0f) {}
    std::string const getName() override { return "mind sear heavy aoe"; }
};

class ShadowProtectionTrigger : public BuffTrigger
{
public:
    ShadowProtectionTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "shadow protection", 8 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class ShadowProtectionOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    ShadowProtectionOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "shadow protection", 8 * IN_MILLISECONDS) {}
};

class PowerWordFortitudeOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    PowerWordFortitudeOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "power word: fortitude", 8 * IN_MILLISECONDS) {}
};

class PowerWordFortitudeTrigger : public BuffTrigger
{
public:
    PowerWordFortitudeTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "power word: fortitude", 8 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class DivineSpiritOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    DivineSpiritOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "divine spirit", 8 * IN_MILLISECONDS) {}
};

class DivineSpiritTrigger : public BuffTrigger
{
public:
    DivineSpiritTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "divine spirit", 8 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class FearWardOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    FearWardOnMainTankTrigger(PlayerbotAI* botAI)
        : BuffOnMainTankTrigger(botAI, "fear ward", false, 2 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class PowerWordShieldOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    PowerWordShieldOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "power word: shield") {}

    bool IsActive() override;
};

class PrayerOfMendingOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    PrayerOfMendingOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "prayer of mending", true) {}

    bool IsActive() override;
};

class RenewOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    RenewOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "renew", true) {}

    // [b31_deep] only on a tank that takes damage (below 98 %), not as an unconditional upkeep
    bool IsActive() override;
};

class BindingHealTrigger : public PartyMemberLowHealthTrigger
{
public:
    BindingHealTrigger(PlayerbotAI* botAI);

    bool IsActive() override;
};

// Surge of Light: the next Flash Heal is instant and free.
class SurgeOfLightTrigger : public HasAuraTrigger
{
public:
    SurgeOfLightTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "surge of light") {}
};

class MindSearChannelCheckTrigger : public Trigger
{
public:
    MindSearChannelCheckTrigger(PlayerbotAI* botAI, uint32 minEnemies = 2)
        : Trigger(botAI, "mind sear channel check"), minEnemies(minEnemies) {}

    bool IsActive() override;

protected:
    uint32 minEnemies;
    static const std::set<uint32> MIND_SEAR_SPELL_IDS;
};

// [guide_b31_dead] Disc: Penance on its cooldown on the main tank once he is below 95 % (GUIDE_AUDIT_HEALERS 1).
class PenanceOnMainTankTrigger : public Trigger
{
public:
    PenanceOnMainTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "penance on main tank") {}
    bool IsActive() override;
};

// [guide_b31_dead] Holy: Circle of Healing on its cooldown when 2+ group members are at or below 90 %
// (the group heal nodes wanted 3 members below 70-85 %).
class CircleOfHealingTwoInjuredTrigger : public Trigger
{
public:
    CircleOfHealingTwoInjuredTrigger(PlayerbotAI* botAI) : Trigger(botAI, "circle of healing two injured") {}
    bool IsActive() override;
};

// [guide_b31_dead] Disc: Borrowed Time (buff 59887-59891, after Power Word: Shield) gives the next spell 25 % haste
// [b31_deep] (r5 per Spell.dbc; the 40 % is the shield's spell power bonus); spend it on the triaged
// "party member to heal" at or below maxHealthPct.
class BorrowedTimeTrigger : public Trigger
{
public:
    BorrowedTimeTrigger(PlayerbotAI* botAI, std::string const name, float maxHealthPct)
        : Trigger(botAI, name), maxHealthPct(maxHealthPct) {}
    bool IsActive() override;

protected:
    float maxHealthPct;
};

// [guide_b31_dead] Holy: 3 stacks of Serendipity (buff 63731 / 63734 / 63735; 63730/63733/63737 are the talent)
// cut Greater Heal's cast by 12 %: Greater Heal on the main tank below 70 % while standing still.
class SerendipityGreaterHealTrigger : public Trigger
{
public:
    SerendipityGreaterHealTrigger(PlayerbotAI* botAI) : Trigger(botAI, "serendipity greater heal") {}
    bool IsActive() override;
};

// K4: Hymn of Hope when two or more healers within 40 yd run low on mana, and nobody needs a big heal now.
class HymnOfHopeNeededTrigger : public Trigger
{
public:
    HymnOfHopeNeededTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hymn of hope needed") {}

    bool IsActive() override;
};

#endif
