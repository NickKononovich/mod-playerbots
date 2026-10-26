/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DRUIDTRIGGERS_H
#define PLAYERBOTS_DRUIDTRIGGERS_H

#include "CureTriggers.h"
#include "GenericTriggers.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RtiTriggers.h"
#include "SharedDefines.h"
#include "Trigger.h"
#include "Value.h"  // [b32_brez] UnitCalculatedValue
#include <set>
#include <unordered_map>  // [b32_brez]

inline constexpr uint32 AURA_OMEN_OF_CLARITY = 16864;
inline constexpr uint32 SPELL_TREE_OF_LIFE = 33891;
inline constexpr uint32 FERAL_DOT_REFRESH_WINDOW_MS = 1 * IN_MILLISECONDS;
inline constexpr uint32 SAVAGE_ROAR_REFRESH_WINDOW_MS = 2 * IN_MILLISECONDS;
inline constexpr int32 LIFEBLOOM_REFRESH_WINDOW_MS = 2 * IN_MILLISECONDS;
// Rip (12 s, bleed) only beats Ferocious Bite when the target lives through most of its ticks
inline constexpr float RIP_MIN_TARGET_LIFETIME = 12.0f;

class PlayerbotAI;

class MarkOfTheWildOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    MarkOfTheWildOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "mark of the wild", 8 * IN_MILLISECONDS) {}
};

class MarkOfTheWildTrigger : public BuffTrigger
{
public:
    MarkOfTheWildTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "mark of the wild", 8 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class ThornsOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    ThornsOnPartyTrigger(PlayerbotAI* botAI) : BuffOnPartyTrigger(botAI, "thorns", 4 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class ThornsOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    ThornsOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "thorns", false, 4 * IN_MILLISECONDS) {}
};

class ThornsTrigger : public BuffTrigger
{
public:
    ThornsTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "thorns", 4 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class ClearcastingTrigger : public HasAuraTrigger
{
public:
    ClearcastingTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "clearcasting") {}
};

class PredatorsSwiftnessTrigger : public HasAuraTrigger
{
public:
    PredatorsSwiftnessTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "predator's swiftness") {}
};

class NaturesSwiftnessActiveTrigger : public HasAuraTrigger
{
public:
    NaturesSwiftnessActiveTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "nature's swiftness") {}
    bool IsActive() override { return botAI->HasAura("nature's swiftness", bot); }
};

class RakeTrigger : public DebuffTrigger
{
public:
    RakeTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "rake", 1, true, 8.0f, FERAL_DOT_REFRESH_WINDOW_MS) {}
    bool IsActive() override { return !botAI->HasAura("prowl", bot) && DebuffTrigger::IsActive(); }
};

// [b31_sd] A second-target DoT only pays out on a mob that lives through most of its ticks (Insect Swarm and
// Moonfire are 12 s DoTs), and only with a second attacker to put it on.
inline constexpr float DRUID_MULTI_DOT_MIN_LIFETIME = 12.0f;
inline constexpr uint8 DRUID_MULTI_DOT_MIN_ATTACKERS = 2;

// [b31_sd] The DoT is not on the target at all (fell off or never applied). DebuffTrigger's lifetime gate (8 s) still
// applies. One aura lookup. [b31_deep] The "insect swarm" / "moonfire" refresh triggers are gone: with no refresh
// window they fired on exactly this condition and never won against these nodes.
class DruidDotMissingTrigger : public DebuffTrigger
{
public:
    DruidDotMissingTrigger(PlayerbotAI* botAI, std::string const spell) : DebuffTrigger(botAI, spell, 1, true) {}

    std::string const getName() override { return spell + " missing"; }
    bool IsActive() override
    {
        Unit* target = GetTarget();
        if (!target || botAI->GetAura(spell, target, true))
            return false;

        return DebuffTrigger::IsActive();
    }
};

// [b31_sd] Both on-attacker triggers went to BuffTrigger::IsActive(), skipping the lifetime gate, and fired on a
// single target too. Now: 2+ attackers and the attacker expected to live DRUID_MULTI_DOT_MIN_LIFETIME.
class InsectSwarmOnAttackerTrigger : public DebuffOnAttackerTrigger
{
public:
    InsectSwarmOnAttackerTrigger(PlayerbotAI* botAI)
        : DebuffOnAttackerTrigger(botAI, "insect swarm", true, DRUID_MULTI_DOT_MIN_LIFETIME) {}
    bool IsActive() override
    {
        return AI_VALUE(uint8, "attacker count") >= DRUID_MULTI_DOT_MIN_ATTACKERS && DebuffTrigger::IsActive();
    }
};

class MoonfireOnAttackerTrigger : public DebuffOnAttackerTrigger
{
public:
    MoonfireOnAttackerTrigger(PlayerbotAI* botAI)
        : DebuffOnAttackerTrigger(botAI, "moonfire", true, DRUID_MULTI_DOT_MIN_LIFETIME) {}
    bool IsActive() override
    {
        return AI_VALUE(uint8, "attacker count") >= DRUID_MULTI_DOT_MIN_ATTACKERS && DebuffTrigger::IsActive();
    }
};

class FaerieFireTrigger : public DebuffTrigger
{
public:
    FaerieFireTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "faerie fire", 1, false, 25.0f) {}
};

class FaerieFireFeralTrigger : public DebuffTrigger
{
public:
    FaerieFireFeralTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "faerie fire (feral)") {}

    bool IsActive() override
    {
        if (!bot->IsInCombat())
            return false;

        // Bear: every cast generates immediate threat/damage for free — spam it
        if (botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr))
        {
            Unit* target = GetTarget();
            return target && target->IsAlive() && target->IsInWorld();
        }

        if (!botAI->HasAura("cat form", bot))
            return false;

        // [b31_sd] Not under Berserk (the guides: every GCD of Berserk goes to the half-cost builders / finishers).
        if (botAI->HasAura("prowl", bot) || botAI->HasAura("berserk", bot))
            return false;

        // Cat with Omen of Clarity: spam to fish for Clearcasting procs
        if (bot->HasAura(AURA_OMEN_OF_CLARITY))
        {
            Unit* target = GetTarget();
            return target && target->IsAlive() && target->IsInWorld();
        }

        // Cat without Omen of Clarity: apply as a normal debuff, don't reapply
        return DebuffTrigger::IsActive();
    }
};

class BashInterruptSpellTrigger : public InterruptSpellTrigger
{
public:
    BashInterruptSpellTrigger(PlayerbotAI* botAI) : InterruptSpellTrigger(botAI, "bash") {}
};

// [b31_sd] Tiger's Fury / Berserk order (FERAL_CAT_AUDIT.md 2.2.6-7, wowsims waitForTf). Berserk removes an active
// Tiger's Fury (spell_dru_berserk) and the core refuses Tiger's Fury under Berserk, so in cat form Berserk waits for
// a Tiger's Fury that is neither up nor coming back within 15 s; a bear (no Tiger's Fury there) is not held.
inline constexpr uint32 BERSERK_TF_MIN_COOLDOWN_MS = 15 * IN_MILLISECONDS;
inline constexpr uint8 TIGERS_FURY_MAX_ENERGY = 40;  // wowsims: 40 - 10 * leeway; King of the Jungle adds 60

class BerserkTrigger : public BoostTrigger
{
public:
    BerserkTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "berserk") {}

    bool IsActive() override
    {
        if (botAI->HasAura("cat form", bot))
        {
            if (botAI->HasAura("tiger's fury", bot))
                return false;

            uint32 const tigersFury = AI_VALUE2(uint32, "spell id", "tiger's fury");
            if (tigersFury && bot->GetSpellCooldownDelay(tigersFury) < BERSERK_TF_MIN_COOLDOWN_MS)
                return false;
        }

        return BoostTrigger::IsActive();
    }
};

// [b31_sd] Tiger's Fury below TIGERS_FURY_MAX_ENERGY, never under Berserk (CASTER_AURASTATE: 31 refused casts in the
// logs) and not while it is still up. Replaces "low energy" (< 30) in the cat strategy.
class TigersFuryTrigger : public Trigger
{
public:
    TigersFuryTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tiger's fury") {}

    bool IsActive() override
    {
        return AI_VALUE2(uint8, "energy", "self target") < TIGERS_FURY_MAX_ENERGY &&
               !botAI->HasAura("berserk", bot) && !botAI->HasAura("tiger's fury", bot);
    }
};

class BerserkActiveTrigger : public HasAuraTrigger
{
public:
    BerserkActiveTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "berserk") {}
};

class SavageRoarTrigger : public BuffTrigger
{
public:
    SavageRoarTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "savage roar", 1, false, false, SAVAGE_ROAR_REFRESH_WINDOW_MS) {}

    // [b31_sd] Savage Roar needs a combo point: at 0 the trigger fired and queued an impossible cast. 1+ is kept on
    // purpose (a missing Roar at 1 point beats waiting for more); the Roar / Rip desync is "savage roar clip".
    bool IsActive() override
    {
        return AI_VALUE2(uint8, "combo", "current target") >= 1 && BuffTrigger::IsActive();
    }
};

// [b31_sd] Tank at or above this with a Lifebloom on him, on trash: the stack is left to bloom (the bloom heals and
// refunds half the mana) instead of rolling it (GUIDE_AUDIT_HEALERS.md: overheal >= 58 % on trash).
inline constexpr float LIFEBLOOM_BLOOM_TANK_HEALTH_PCT = 95.0f;

// [b31_sd] True when an existing Lifebloom on the tank is left to bloom: the tank is not hurt and the fight is not a
// boss (no encounter in progress, the tank's victim no boss). Aura lookup is done by the caller.
bool LifebloomLetBloom(Player* bot, Unit* tank, Aura* lifebloom);

// Keeps a rolling 3-stack: refreshes just before the stack blooms.
class LifebloomOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    LifebloomOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "lifebloom", true) {}

    std::string const getName() override { return "lifebloom on main tank"; }
    bool IsActive() override;
};

class RejuvenationOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    RejuvenationOnMainTankTrigger(PlayerbotAI* botAI) : BuffOnMainTankTrigger(botAI, "rejuvenation", true) {}

    std::string const getName() override { return "rejuvenation on main tank"; }
    bool IsActive() override;
};

class NaturesGraspTrigger : public BuffTrigger
{
public:
    NaturesGraspTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "nature's grasp") {}
};

class EntanglingRootsTrigger : public RtiCcTrigger
{
public:
    EntanglingRootsTrigger(PlayerbotAI* botAI) : RtiCcTrigger(botAI, "entangling roots") {}
};

class EntanglingRootsKiteTrigger : public DebuffTrigger
{
public:
    EntanglingRootsKiteTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "entangling roots") {}

    bool IsActive() override;
};

class HibernateTrigger : public RtiCcTrigger
{
public:
    HibernateTrigger(PlayerbotAI* botAI) : RtiCcTrigger(botAI, "hibernate") {}
};

class CycloneTrigger : public RtiCcTrigger
{
public:
    CycloneTrigger(PlayerbotAI* botAI) : RtiCcTrigger(botAI, "cyclone") {}
};

class CurePoisonTrigger : public NeedCureTrigger
{
public:
    CurePoisonTrigger(PlayerbotAI* botAI) : NeedCureTrigger(botAI, "cure poison", DISPEL_POISON) {}
};

class PartyMemberCurePoisonTrigger : public PartyMemberNeedCureTrigger
{
public:
    PartyMemberCurePoisonTrigger(PlayerbotAI* botAI) : PartyMemberNeedCureTrigger(botAI, "cure poison", DISPEL_POISON)
    {
    }
};

// [b32_tank_aoe] A bear tank out of combat and out of bear form, with nothing to rest for: (Dire) Bear Form on the
// non-combat engine. The combat "bear form" trigger (BearDruidStrategy) shifted only after the first hit: buffs
// (Mark of the Wild, Thorns: "caster form" prerequisite) and drinks left the tank in caster form for the walk to the
// next pack and for harness / body pulls. LEFT_FORM_GRACE_MS after it was last seen in form, so a buff sequence on
// the party (one "caster form" for several casts) is not cut by a shift per cast. O(1): flags and auras first, the
// rest lines (RestState, the next fight's estimate) last, every CHECK_INTERVAL_SEC.
class BearTankFormTrigger : public Trigger
{
public:
    static constexpr int CHECK_INTERVAL_SEC = 1;
    static constexpr uint32 LEFT_FORM_GRACE_MS = 3000;

    BearTankFormTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bear tank form", CHECK_INTERVAL_SEC) {}

    bool IsActive() override;

private:
    uint32 _inFormMs = 0;
};

class BearFormTrigger : public BuffTrigger
{
public:
    BearFormTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "bear form") {}

    bool IsActive() override;
};

class TreeFormTrigger : public BuffTrigger
{
public:
    TreeFormTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "tree of life") {}

    bool IsActive() override;
};

class CatFormTrigger : public BuffTrigger
{
public:
    CatFormTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "cat form") {}

    bool IsActive() override;
};

class AquaticFormTrigger : public Trigger
{
public:
    AquaticFormTrigger(PlayerbotAI* botAI) : Trigger(botAI, "aquatic form") {}

    bool IsActive() override;
};

class EclipseSolarTrigger : public HasAuraTrigger
{
public:
    EclipseSolarTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "eclipse (solar)") {}
};

class EclipseLunarTrigger : public HasAuraTrigger
{
public:
    EclipseLunarTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "eclipse (lunar)") {}
};

class BashInterruptEnemyHealerSpellTrigger : public InterruptEnemyHealerTrigger
{
public:
    BashInterruptEnemyHealerSpellTrigger(PlayerbotAI* botAI) : InterruptEnemyHealerTrigger(botAI, "bash") {}
};

class NaturesSwiftnessTrigger : public BuffTrigger
{
public:
    NaturesSwiftnessTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "nature's swiftness") {}
};

class DruidPartyMemberRemoveCurseTrigger : public PartyMemberNeedCureTrigger
{
public:
    DruidPartyMemberRemoveCurseTrigger(PlayerbotAI* botAI)
        : PartyMemberNeedCureTrigger(botAI, "druid remove curse", DISPEL_CURSE)
    {
    }
};

class StarfallTrigger : public SpellNoCooldownTrigger
{
public:
    StarfallTrigger(PlayerbotAI* botAI) : SpellNoCooldownTrigger(botAI, "starfall") {}
};

class ForceOfNatureTrigger : public BoostTrigger
{
public:
    ForceOfNatureTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "force of nature") {}
};

class MangleBearTrigger : public DebuffTrigger
{
public:
    MangleBearTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "mangle (bear)") {}

    bool IsActive() override
    {
        if (!bot->IsInCombat() || !botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr))
            return false;
        Unit* target = GetTarget();
        return target && target->IsAlive() && target->IsInWorld();
    }
};

class LacerateTrigger : public DebuffTrigger
{
public:
    LacerateTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "lacerate") {}

    bool IsActive() override
    {
        if (!bot->IsInCombat() || !botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr))
            return false;

        Unit* target = GetTarget();
        if (!target || !target->IsAlive() || !target->IsInWorld())
            return false;

        Aura* lacerate = botAI->GetAura("lacerate", target, false, false);
        if (!lacerate)
            return true;

        if (lacerate->GetStackAmount() < 5)
            return true;

        return lacerate->GetDuration() <= 6000;
    }
};

// [b31_deep] wowsims feral tank APL rage dump: Swipe (Bear) on a single target at SWIPE_MIN_RAGE+ rage once Lacerate
// holds 5 stacks. Cached rage value plus two aura lookups.
class SwipeBearRageDumpTrigger : public Trigger
{
public:
    static constexpr uint8 SWIPE_MIN_RAGE = 40;
    static constexpr uint8 LACERATE_MAX_STACKS = 5;

    SwipeBearRageDumpTrigger(PlayerbotAI* botAI) : Trigger(botAI, "swipe (bear) rage dump") {}

    bool IsActive() override
    {
        if (!bot->IsInCombat() || AI_VALUE2(uint8, "rage", "self target") < SWIPE_MIN_RAGE ||
            !botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr))
            return false;

        Unit* target = AI_VALUE(Unit*, "current target");
        if (!target || !target->IsAlive() || !target->IsInWorld())
            return false;

        Aura* lacerate = botAI->GetAura("lacerate", target, false, false);
        return lacerate && lacerate->GetStackAmount() >= LACERATE_MAX_STACKS;
    }
};

class DemoralizeRoarTrigger : public DebuffTrigger
{
public:
    DemoralizeRoarTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "demoralizing roar") {}

    bool IsActive() override
    {
        return DebuffTrigger::IsActive()
            && !botAI->HasAura("curse of weakness",  GetTarget(), false, false)
            && !botAI->HasAura("demoralizing shout", GetTarget(), false, false)
            && !botAI->HasAura("vindication",        GetTarget(), false, false);
    }
};

class MangleCatTrigger : public DebuffTrigger
{
public:
    MangleCatTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "mangle (cat)", 1, false, 0.0f) {}
    bool IsActive() override
    {
        if (botAI->HasAura("prowl", bot))
            return false;
        return DebuffTrigger::IsActive() && !botAI->HasAura("mangle (bear)", GetTarget(), false, false, -1, true)
            && !botAI->HasAura("trauma", GetTarget(), false, false, -1, true);
    }
};

class FerociousBiteTimeTrigger : public Trigger
{
public:
    FerociousBiteTimeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ferocious bite time") {}
    bool IsActive() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (!target)
            return false;

        uint8 cp = AI_VALUE2(uint8, "combo", "current target");
        if (cp < 5)
            return false;

        // Dying target: a new Rip would not tick out, so the 5 points go into Ferocious Bite instead
        if (target->GetHealth() / AI_VALUE(float, "estimated group dps") < RIP_MIN_TARGET_LIFETIME)
            return true;

        // [b31_sd] wowsims biteTime: not at high energy (the extra energy would go into Bite's damage conversion
        // and leave nothing for the builders) and never under Clearcasting (the free Shred comes first).
        if (AI_VALUE2(uint8, "energy", "self target") >= FEROCIOUS_BITE_MAX_ENERGY ||
            botAI->HasAura("clearcasting", bot))
            return false;

        Aura* roar = botAI->GetAura("savage roar", bot);
        bool roarCheck = !roar || roar->GetDuration() > 10000;
        if (!roarCheck)
            return false;

        Aura* rip = botAI->GetAura("rip", target, true);
        bool ripCheck = !rip || rip->GetDuration() > 10000;
        if (!ripCheck)
            return false;

        return true;
    }

private:
    static constexpr uint8 FEROCIOUS_BITE_MAX_ENERGY = 67;
};

// [b31_sd] Execute Bite on a target about to die, by its time to live (health / "estimated group dps") instead of
// "below 25 % and below 20000 HP" with 1 point: that picked it 286 times against 67 for "ferocious bite time", 136
// of them without the energy, and ate the points Rip needed on the level 51-70 bosses (FERAL_CAT_AUDIT.md 2.2.5).
class FerociousBiteExecuteTrigger : public Trigger
{
public:
    static constexpr float EXECUTE_MAX_LIFETIME = 5.0f;
    static constexpr uint8 EXECUTE_MIN_COMBO = 3;
    static constexpr uint8 FEROCIOUS_BITE_ENERGY = 35;

    FerociousBiteExecuteTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ferocious bite execute") {}
    bool IsActive() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (!target || !target->IsAlive())
            return false;

        if (!botAI->HasSpell("ferocious bite"))
            return false;

        if (AI_VALUE2(uint8, "combo", "current target") < EXECUTE_MIN_COMBO)
            return false;

        if (AI_VALUE2(uint8, "energy", "self target") < FEROCIOUS_BITE_ENERGY && !botAI->HasAura("clearcasting", bot))
            return false;

        return target->GetHealth() / AI_VALUE(float, "estimated group dps") < EXECUTE_MAX_LIFETIME;
    }
};

// [b31_sd] Roar / Rip desync (wowsims clipRoar, FERAL_CAT_AUDIT.md 2.2.2): with Rip up, Savage Roar ending no later
// than Rip + ROAR_RIP_LEEWAY_MS, and a Roar cast now outlasting Rip by MIN_ROAR_OFFSET_MS, recast Roar early so the
// two never come due on the same points. Roar lasts 9 s + 5 s per point (14-34 s). Two aura lookups.
class SavageRoarClipTrigger : public Trigger
{
public:
    static constexpr int32 ROAR_RIP_LEEWAY_MS = 4 * IN_MILLISECONDS;
    static constexpr int32 MIN_ROAR_OFFSET_MS = 25 * IN_MILLISECONDS;

    SavageRoarClipTrigger(PlayerbotAI* botAI) : Trigger(botAI, "savage roar clip") {}
    bool IsActive() override
    {
        uint8 const combo = AI_VALUE2(uint8, "combo", "current target");
        Unit* target = AI_VALUE(Unit*, "current target");
        if (!combo || !target)
            return false;

        Aura* roar = botAI->GetAura("savage roar", bot);
        Aura* rip = roar ? botAI->GetAura("rip", target, true) : nullptr;
        if (!rip)
            return false;

        int32 const ripLeft = rip->GetDuration();
        if (roar->GetDuration() > ripLeft + ROAR_RIP_LEEWAY_MS)
            return false;

        int32 const newRoar = (9 + 5 * combo) * IN_MILLISECONDS;
        return newRoar >= ripLeft + MIN_ROAR_OFFSET_MS;
    }
};

class HurricaneChannelCheckTrigger : public Trigger
{
public:
    HurricaneChannelCheckTrigger(PlayerbotAI* botAI, uint32 minEnemies = 3)
        : Trigger(botAI, "hurricane channel check"), minEnemies(minEnemies)
    {
    }

    bool IsActive() override;

protected:
    uint32 minEnemies;
    static const std::set<uint32> HURRICANE_SPELL_IDS;
};

// [b31_deep] A caster druid (moonkin, restoration) in a group flees a mob in melee only when in danger: own health
// below the medium band or IN_DANGER_ATTACKERS+ mobs on it. One mob is the tank's to pick up, and stepping away
// breaks the cast (a healer's critical heal). Solo it is "enemy too close for spell". Cached values only.
class DruidEnemyTooCloseInDangerTrigger : public EnemyTooCloseForSpellTrigger
{
public:
    static constexpr uint8 IN_DANGER_ATTACKERS = 2;

    DruidEnemyTooCloseInDangerTrigger(PlayerbotAI* botAI) : EnemyTooCloseForSpellTrigger(botAI) {}

    std::string const getName() override { return "enemy too close for spell in danger"; }
    bool IsActive() override
    {
        if (bot->GetGroup() && bot->GetHealthPct() >= sPlayerbotAIConfig.MediumHealth &&
            AI_VALUE(uint8, "my attacker count") < IN_DANGER_ATTACKERS)
            return false;

        return EnemyTooCloseForSpellTrigger::IsActive();
    }
};

class NoHealerDpsStrategyTrigger : public Trigger
{
public:
    NoHealerDpsStrategyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no healer dps strategy") {}

    bool IsActive() override
    {
        return !botAI->HasStrategy("healer dps", BOT_STATE_COMBAT);
    }
};

// Blanketing and healer dps compete for a druid in tree form, so healer dps does not take effect
// for a grouped resto druid with tree form.
class HealerShouldAttackAndNotBlanketingTrigger : public HealerShouldAttackTrigger
{
public:
    HealerShouldAttackAndNotBlanketingTrigger(PlayerbotAI* botAI) : HealerShouldAttackTrigger(botAI) {}

    std::string const getName() override { return "healer should attack and not blanketing"; }
    bool IsActive() override;
};

class BlanketHotTrigger : public Trigger
{
public:
    BlanketHotTrigger(PlayerbotAI* botAI, std::string const& name, std::string const& spell)
        : Trigger(botAI, name), _spell(spell) {}

    Unit* GetTarget() override;
    bool IsActive() override;

private:
    std::string _spell;
};

// The ally this druid brought back with Rebirth (RebirthFollowUp) is still low: heal it first.
class RebirthTargetLowHealthTrigger : public Trigger
{
public:
    RebirthTargetLowHealthTrigger(PlayerbotAI* botAI) : Trigger(botAI, "rebirth target low health") {}

    bool IsActive() override;
};

// The ally back from Rebirth lost its buffs with its life.
class RebirthTargetNoMarkTrigger : public Trigger
{
public:
    RebirthTargetNoMarkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "rebirth target unbuffed") {}

    bool IsActive() override;
};

class ProwlTrigger : public Trigger
{
public:
    ProwlTrigger(PlayerbotAI* botAI) : Trigger(botAI, "prowl") {}

    bool IsActive() override;
};

// [b32_brez] A bear tank's battle res, a last resort: out of Dire Bear Form, Rebirth (2 s cast + GCD) on a dead
// member, straight back into form. Only while no other druid in the group can do it (a resto / cat / balance druid
// casts it from its own "rebirth" nodes, GenericDruidStrategy) and the bear can afford the cast:
//   - in combat, Rebirth known, off cooldown, its mana and reagent in the bags;
//   - its health above MIN_HEALTH_PCT and not falling fast ("recent health loss" under TankCooldownLossPct);
//   - at most MAX_ATTACKERS on it, or every mob within TankPeel::ANCHOR_RANGE on it with TankAoePackTrigger's safe
//     lead;
//   - no boss among the attackers casting (a big hit would land while the bear is out of form).
// The healer first; another member only while no healer is dead, and only a valuable one (a tank or a real
// player). In RANGE and line of sight; one attempt per dead member per RETRY_MS.
// Cost: "bear battle res target" scans the group and the cached "attackers" once per SCAN_MS; both triggers read the
// cached answer and the attempt time (O(1)).
namespace BearBattleRes
{
constexpr float MIN_HEALTH_PCT = 60.0f;
constexpr uint32 MAX_ATTACKERS = 2;
constexpr float RANGE = 30.0f;
constexpr uint32 RETRY_MS = 30 * IN_MILLISECONDS;           // one attempt per dead member this often
constexpr uint32 RESHIFT_WINDOW_MS = 15 * IN_MILLISECONDS;  // after an attempt: back to Dire Bear Form first
constexpr uint32 SCAN_MS = 1 * IN_MILLISECONDS;

// The action's attempt on `target`, before the cast: a failed or interrupted cast counts too.
void RecordAttempt(PlayerbotAI* botAI, Unit* target);
// getMSTime() of the last attempt, 0 for none.
uint32 LastAttemptMs(PlayerbotAI* botAI);
}  // namespace BearBattleRes

class BearBattleResTargetValue : public UnitCalculatedValue
{
public:
    BearBattleResTargetValue(PlayerbotAI* botAI)
        : UnitCalculatedValue(botAI, "bear battle res target", BearBattleRes::SCAN_MS)
    {
    }

    void RecordAttempt(ObjectGuid guid, uint32 now);
    uint32 LastAttemptMs() const { return _lastAttemptMs; }

protected:
    Unit* Calculate() override;

private:
    bool CanAffordCast(uint32 rebirthId) const;
    bool PackHeld() const;
    bool AttemptedRecently(ObjectGuid guid, uint32 now) const;

    std::unordered_map<ObjectGuid, uint32> _attempts;
    uint32 _lastAttemptMs = 0;
};

// [b32_brez] The bear may leave its form for Rebirth now ("bear battle res target").
class BearBattleResTrigger : public Trigger
{
public:
    BearBattleResTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bear battle res") {}

    std::string const GetTargetName() override { return "bear battle res target"; }
    bool IsActive() override;
};

// [b32_brez] Out of bear form after a battle res attempt (cast, failed or interrupted): Dire Bear Form above all.
class BearBattleResReshiftTrigger : public Trigger
{
public:
    BearBattleResReshiftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bear battle res reshift") {}

    bool IsActive() override;
};

#endif
