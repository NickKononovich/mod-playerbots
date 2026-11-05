/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WARLOCKACTIONS_H
#define PLAYERBOTS_WARLOCKACTIONS_H

#include "Action.h"
#include "GenericSpellActions.h"
#include "InventoryAction.h"
#include "UseItemAction.h"

class Player;
class PlayerbotAI;
class Unit;

// Buff and Out of Combat Spells

class CastDemonSkinAction : public CastBuffSpellAction
{
public:
    CastDemonSkinAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "demon skin") {}
};

class CastDemonArmorAction : public CastBuffSpellAction
{
public:
    CastDemonArmorAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "demon armor") {}
};

class CastFelArmorAction : public CastBuffSpellAction
{
public:
    CastFelArmorAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "fel armor") {}
};

class CastSoulLinkAction : public CastBuffSpellAction
{
public:
    CastSoulLinkAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "soul link", false, 5000) {}
    std::string const GetTargetName() override { return "pet target"; }
};

class CastUnendingBreathAction : public CastBuffSpellAction
{
public:
    CastUnendingBreathAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "unending breath") {}
};

class CastUnendingBreathOnPartyAction : public BuffOnPartyAction
{
public:
    CastUnendingBreathOnPartyAction(PlayerbotAI* botAI) : BuffOnPartyAction(botAI, "unending breath") {}
};

class CreateSoulShardAction : public Action
{
public:
    CreateSoulShardAction(PlayerbotAI* botAI) : Action(botAI, "create soul shard") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class DestroySoulShardAction : public InventoryAction
{
public:
    DestroySoulShardAction(PlayerbotAI* botAI) : InventoryAction(botAI, "destroy soul shard") {}

    bool Execute(Event event) override;
};

class CastCreateHealthstoneAction : public CastBuffSpellAction
{
public:
    CastCreateHealthstoneAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "create healthstone") {}
};

class CastCreateFirestoneAction : public CastBuffSpellAction
{
public:
    CastCreateFirestoneAction(PlayerbotAI* botAI);
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    static const std::vector<uint32> firestoneSpellIds;
};

class CastCreateSpellstoneAction : public CastBuffSpellAction
{
public:
    CastCreateSpellstoneAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "create spellstone") {}
};

class CastCreateSoulstoneAction : public CastBuffSpellAction
{
public:
    CastCreateSoulstoneAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "create soulstone") {}
    bool isUseful() override;
};

class UseSoulstoneSelfAction : public UseSpellItemAction
{
public:
    UseSoulstoneSelfAction(PlayerbotAI* botAI) : UseSpellItemAction(botAI, "soulstone") {}
    bool Execute(Event event) override;
};

class UseSoulstoneMasterAction : public UseSpellItemAction
{
public:
    UseSoulstoneMasterAction(PlayerbotAI* botAI) : UseSpellItemAction(botAI, "soulstone") {}
    bool Execute(Event event) override;
};

class UseSoulstoneTankAction : public UseSpellItemAction
{
public:
    UseSoulstoneTankAction(PlayerbotAI* botAI) : UseSpellItemAction(botAI, "soulstone") {}
    bool Execute(Event event) override;
};

class UseSoulstoneHealerAction : public UseSpellItemAction
{
public:
    UseSoulstoneHealerAction(PlayerbotAI* botAI) : UseSpellItemAction(botAI, "soulstone") {}
    bool Execute(Event event) override;
};

// "ss auto": in a group the stone goes to a healer who can resurrect out of combat (priest, paladin, shaman),
// else to the main tank; alone it goes on the warlock. Re-applied whenever the stone is used up (e.g. after a wipe).
class UseSoulstoneAutoAction : public UseSpellItemAction
{
public:
    UseSoulstoneAutoAction(PlayerbotAI* botAI) : UseSpellItemAction(botAI, "soulstone") {}
    bool Execute(Event event) override;
};

class CastRitualOfSoulsAction : public CastSpellAction
{
public:
    CastRitualOfSoulsAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "ritual of souls") {}
    std::string const GetTargetName() override { return "self target"; }
};

// Summoning Spells

class CastSummonVoidwalkerAction : public CastBuffSpellAction
{
public:
    CastSummonVoidwalkerAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "summon voidwalker") {}
};

class CastSummonFelguardAction : public CastBuffSpellAction
{
public:
    CastSummonFelguardAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "summon felguard") {}
};

class CastSummonFelhunterAction : public CastBuffSpellAction
{
public:
    CastSummonFelhunterAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "summon felhunter") {}
};

class CastSummonImpAction : public CastBuffSpellAction
{
public:
    CastSummonImpAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "summon imp") {}
};

class CastSummonSuccubusAction : public CastBuffSpellAction
{
public:
    CastSummonSuccubusAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "summon succubus") {}
};
class CastFelDominationAction : public CastBuffSpellAction
{
public:
    CastFelDominationAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "fel domination") {}
};

// CC and Pet Spells

class CastBanishOnCcAction : public CastCrowdControlSpellAction
{
public:
    CastBanishOnCcAction(PlayerbotAI* botAI) : CastCrowdControlSpellAction(botAI, "banish") {}
    Value<Unit*>* GetTargetValue() override;
    bool isPossible() override;
};

class CastFearOnCcAction : public CastCrowdControlSpellAction
{
public:
    CastFearOnCcAction(PlayerbotAI* botAI) : CastCrowdControlSpellAction(botAI, "fear") {}
    Value<Unit*>* GetTargetValue() override;
    bool isPossible() override;
};

class CastSpellLockAction : public CastSpellAction
{
public:
    CastSpellLockAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "spell lock") {}
};

class CastDevourMagicPurgeAction : public CastSpellAction
{
public:
    CastDevourMagicPurgeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "devour magic") {}

    std::string const GetTargetName() override { return "current target"; }
};

class CastDevourMagicCleanseAction : public CastSpellAction
{
public:
    CastDevourMagicCleanseAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "devour magic cleanse") {}
    std::string const getName() override { return "cleanse magic on party"; }
};

// Utility Spells

class CastShadowWardAction : public CastBuffSpellAction
{
public:
    CastShadowWardAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "shadow ward") {}
};

class CastSoulshatterAction : public CastSpellAction
{
public:
    CastSoulshatterAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "soulshatter") {}
    bool isUseful() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

// [lead1310] user rule 2026-11-05: a warlock never casts Life Tap while any mob attacks it (a hostile unit has it as
// its victim: the core's attacker list, Unit::getAttackers()). True while refused; `refusedLogged` is the caller's
// last logged state, so the refusal is logged once per state change
bool LifeTapRefusedByAttackers(Player* bot, bool& refusedLogged);

class CastLifeTapAction : public CastSpellAction
{
public:
    CastLifeTapAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "life tap") {}

    std::string const GetTargetName() override { return "self target"; }
    bool isUseful() override;

private:
    bool refusedLogged = false;  // [lead1310]
};

class DemonChargeAction : public CastSpellAction
{
public:
    DemonChargeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "demon charge") {}
};

// Cooldown Spells

class CastMetamorphosisAction : public CastBuffSpellAction
{
public:
    CastMetamorphosisAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "metamorphosis") {}
};

class CastDemonicEmpowermentAction : public CastBuffSpellAction
{
public:
    CastDemonicEmpowermentAction(PlayerbotAI* botAI) : CastBuffSpellAction(botAI, "demonic empowerment") {}
    std::string const GetTargetName() override { return "pet target"; }
};

// DoT/Curse Spells

// DoTs are refreshed this long before expiry: about the cast time plus a margin
constexpr uint32 CORRUPTION_REFRESH_MS = 1000;
constexpr uint32 UNSTABLE_AFFLICTION_REFRESH_MS = 1500;  // [rot352] the unhasted cast time (no spell id known)
// [rot352] wowsims warlock__affliction.apl.json row 18: Unstable Affliction when its dot has less than its (hasted)
// cast time left - the bot recast it at 2 s, so the new cast landed ~0.5 s before the last tick, which the core then
// loses (Aura::RefreshTimers keeps the periodic timer and resets the tick count). b223 rfc-h 5D Rtlocaa: 3.9 a minute
// vs the sim's 3.3, 4704 a cast vs 8971.
uint32 UnstableAfflictionRefreshMs(PlayerbotAI* botAI, Player* bot);
constexpr uint32 IMMOLATE_REFRESH_MS = 2500;
// [rot453] wowsims warlock__destro.apl.json row 4 / warlock__demo.apl.json: Immolate when its dot has less than its
// (hasted) cast time left - the bot recast it at 2.5 s, so the new cast landed ~1 s before the last tick, which the
// core then loses (Aura::RefreshTimers, as [rot352]). b238 rfc-h 5E Rtlocab: Immolate 4.1 a minute as the sim's,
// 6022 / 6516 a cast vs the sim's 7383 / 7748 (82-84 %: one of its five ticks). IMMOLATE_REFRESH_MS without a spell id.
uint32 ImmolateRefreshMs(PlayerbotAI* botAI, Player* bot);

// A DoT is only applied when the target lives long enough (seconds, "estimated group dps") for it to out-damage
// the filler cast it replaces; on a dying target the warlock keeps casting the filler / Drain Soul instead.
constexpr float CORRUPTION_MIN_TARGET_LIFETIME = 6.0f;           // instant, two 3 s ticks beat one filler GCD
constexpr float IMMOLATE_MIN_TARGET_LIFETIME = 6.0f;             // direct hit plus two ticks
// [rot471] wowsims warlock__affliction.apl.json row 18: Unstable Affliction only while remainingTime > 10 s (was 9 s,
// "1.5 s cast, needs about three ticks")
constexpr float UNSTABLE_AFFLICTION_MIN_TARGET_LIFETIME = 10.0f;
// [b31_deep] wowsims affliction / demo APL: Curse of Agony only when the fight lasts more than 22 s (its first
// ticks are the weak ones; was 12 s).
constexpr float CURSE_OF_AGONY_MIN_TARGET_LIFETIME = 22.0f;
constexpr float CURSE_OF_DOOM_MIN_TARGET_LIFETIME = 60.0f;       // the only hit lands when the curse expires
// [b35_lock] wowsims destro: Inferno when the fight has less than 61 s left (the Infernal serves 60 s).
constexpr float INFERNO_MAX_TARGET_LIFETIME = 61.0f;
constexpr uint32 INFERNO_REAGENT_ITEM = 5565;  // Infernal Stone
// +13 % spell damage for the whole group: worth a GCD only on a target the casters hit for a while
constexpr float CURSE_OF_THE_ELEMENTS_MIN_TARGET_LIFETIME = 10.0f;
// Drain Soul deals four times its damage at or below this target health percent (Unit::SpellDamageBonusDone).
constexpr int32 DRAIN_SOUL_EXECUTE_HEALTH_PCT = 25;
// [b31_deep] Demonology: Immolate only on a target that lives this long (wowsims demo APL, fight >= 12 s).
constexpr float DEMO_IMMOLATE_MIN_TARGET_LIFETIME = 12.0f;
// [b31_deep] Demonology multi-dot (Corruption / Immolate on other attackers): only on adds living this long.
constexpr float DEMO_MULTIDOT_MIN_TARGET_LIFETIME = 12.0f;
// [b31_deep] Glyph of Life Tap buff refreshed this long before it drops (wowsims: affliction < 2 s, demo and
// destruction < 3 s).
constexpr uint32 LIFE_TAP_GLYPH_REFRESH_AFFLICTION_MS = 2000;
constexpr uint32 LIFE_TAP_GLYPH_REFRESH_MS = 3000;

class CastCorruptionAction : public CastDebuffSpellAction
{
public:
    CastCorruptionAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "corruption", true, CORRUPTION_MIN_TARGET_LIFETIME)
    {
        beforeDuration = CORRUPTION_REFRESH_MS;
    }
    bool isUseful() override
    {
        // Prevent casting if Seed of Corruption is present
        return CastDebuffSpellAction::isUseful() && !botAI->HasAura("seed of corruption", GetTarget(), false, true);
    }
};

class CastCorruptionOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastCorruptionOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "corruption", true, CORRUPTION_MIN_TARGET_LIFETIME)
    {
    }
    bool isUseful() override
    {
        // Prevent casting if Seed of Corruption is present
        return CastDebuffSpellOnAttackerAction::isUseful() &&
               !botAI->HasAura("seed of corruption", GetTarget(), false, true);
    }
};

class CastImmolateAction : public CastDebuffSpellAction
{
public:
    CastImmolateAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "immolate", true, IMMOLATE_MIN_TARGET_LIFETIME)
    {
        beforeDuration = IMMOLATE_REFRESH_MS;
    }

    bool isUseful() override;  // [rot453] the refresh at the hasted cast time
};

class CastImmolateOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastImmolateOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "immolate", true, IMMOLATE_MIN_TARGET_LIFETIME)
    {
    }
};

class CastUnstableAfflictionAction : public CastDebuffSpellAction
{
public:
    CastUnstableAfflictionAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "unstable affliction", true, UNSTABLE_AFFLICTION_MIN_TARGET_LIFETIME)
    {
        beforeDuration = UNSTABLE_AFFLICTION_REFRESH_MS;
    }

    bool isUseful() override;  // [rot352] the refresh at the hasted cast time
};

class CastUnstableAfflictionOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastUnstableAfflictionOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "unstable affliction", true, UNSTABLE_AFFLICTION_MIN_TARGET_LIFETIME)
    {
    }
};

class CastCurseOfAgonyAction : public CastDebuffSpellAction
{
public:
    CastCurseOfAgonyAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "curse of agony", true, CURSE_OF_AGONY_MIN_TARGET_LIFETIME)
    {
    }
    // [b31_deep] A warlock has one curse per target: Agony would overwrite the bot's own Curse of Doom.
    bool isUseful() override
    {
        return CastDebuffSpellAction::isUseful() && !botAI->HasAura("curse of doom", GetTarget(), false, true);
    }
};

class CastCurseOfAgonyOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastCurseOfAgonyOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "curse of agony", true, CURSE_OF_AGONY_MIN_TARGET_LIFETIME)
    {
    }
    // [b31_deep] Not over the bot's own Curse of Doom (one curse per warlock per target).
    bool isUseful() override
    {
        return CastDebuffSpellOnAttackerAction::isUseful() &&
               !botAI->HasAura("curse of doom", GetTarget(), false, true);
    }
};

class CastCurseOfTheElementsAction : public CastDebuffSpellAction
{
public:
    CastCurseOfTheElementsAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "curse of the elements", true, CURSE_OF_THE_ELEMENTS_MIN_TARGET_LIFETIME) {}
};

class CastCurseOfDoomAction : public CastDebuffSpellAction
{
public:
    CastCurseOfDoomAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "curse of doom", true, CURSE_OF_DOOM_MIN_TARGET_LIFETIME)
    {
    }
};

class CastCurseOfExhaustionAction : public CastDebuffSpellAction
{
public:
    CastCurseOfExhaustionAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "curse of exhaustion") {}
    bool isUseful() override
    {
        // Bypass TTL check
        return CastAuraSpellAction::isUseful();
    }
};

class CastCurseOfTonguesAction : public CastDebuffSpellAction
{
public:
    CastCurseOfTonguesAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "curse of tongues") {}
    bool isUseful() override
    {
        // Bypass TTL check
        return CastAuraSpellAction::isUseful();
    }
};

// [b31_deep] Same lifetime gate as Curse of the Elements (a group debuff worth a GCD only on a target the group
// hits for a while); the TTL bypass put it on every dying trash mob at 29, above the whole rotation.
class CastCurseOfWeaknessAction : public CastDebuffSpellAction
{
public:
    CastCurseOfWeaknessAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "curse of weakness", false, CURSE_OF_THE_ELEMENTS_MIN_TARGET_LIFETIME) {}
};

// Damage Spells

class CastShadowBoltAction : public CastSpellAction
{
public:
    CastShadowBoltAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "shadow bolt") {}
};

class CastDrainSoulAction : public CastSpellAction
{
public:
    CastDrainSoulAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "drain soul") {}

    bool isUseful() override;
};

class CastDrainManaAction : public CastSpellAction
{
public:
    CastDrainManaAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "drain mana") {}
};

class CastDrainLifeAction : public CastSpellAction
{
public:
    CastDrainLifeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "drain life") {}
};

class CastConflagrateAction : public CastSpellAction
{
public:
    CastConflagrateAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "conflagrate") {}
};

class CastIncinerateAction : public CastSpellAction
{
public:
    CastIncinerateAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "incinerate") {}
};

// [rot350] wowsims warlock__affliction.apl.json row 15: Haunt is recast when the warlock's own Haunt debuff on the
// target has less than Haunt's cast time + travel time + 2 s left (and the fight more than 5 s), not on its cooldown:
// the debuff (12 s) outlasts the cooldown (8 s). b223 rfc-h 5D Rtlocaa: Haunt 6.2 a minute vs the sim's 5.3, and every
// execute-phase Drain Soul was clipped the moment Haunt came off cooldown ("drain soul clip"): 0-2 ticks per channel,
// 3537 damage a cast vs the sim's 22892.
inline constexpr uint32 HAUNT_REFRESH_SLACK_MS = 2000;  // the APL row's "+ 2s"
inline constexpr float HAUNT_MIN_FIGHT_LEFT = 5.0f;     // the APL row's remainingTime > 5s
// The ms the own Haunt debuff on the target may still lose before Haunt must be recast (<= 0: due now); INT32_MAX
// when the warlock has no Haunt.
int32 HauntRefreshInMs(PlayerbotAI* botAI, Player* bot, Unit* target);
bool HauntDue(PlayerbotAI* botAI, Player* bot, Unit* target);

class CastHauntAction : public CastSpellAction
{
public:
    CastHauntAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "haunt") {}

    bool isUseful() override;  // [rot350]
};

class CastSoulFireAction : public CastSpellAction
{
public:
    CastSoulFireAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "soul fire") {}
};

class CastShadowburnAction : public CastSpellAction
{
public:
    CastShadowburnAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "shadowburn") {}
};

// [b35_lock] Inferno (1122): the Infernal for 60 s. A TARGET_FLAG_DEST_LOCATION spell: PlayerbotAI::CastSpell drops
// it at the target's position. Its impact threat is small (the Infernal holds its own); its 2 s AoE stun is why
// "inferno boss" keeps it to bosses, and the guardian gate (GenericSpellActions.cpp) waits for the tank to hold it.
class CastInfernoAction : public CastSpellAction
{
public:
    CastInfernoAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "inferno") {}
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

class CastChaosBoltAction : public CastSpellAction
{
public:
    CastChaosBoltAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "chaos bolt") {}
};

class CastSearingPainAction : public CastSpellAction
{
public:
    CastSearingPainAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "searing pain") {}
};

// AoE Spells

// [b44_hos] Affliction AoE. The count is the standard one: "medium aoe" (3 enemies within 8 yd of the target, the
// target included), Icy Veins WotLK Classic Affliction spell summary: Seed "on any packs with 3 targets or more". What
// it does not do is take the main target's DoTs: wowtbc.gg WotLK Affliction keeps Haunt on the top-priority target,
// and Seed of Corruption shares Corruption's SPELL_SPECIFIC_WARLOCK_CORRUPTION (SpellInfo::GetSpellSpecific), so a
// Seed on a target replaces the warlock's Corruption there. hos-h Tribunal (DPS_GAP_TRIBUNAL_COS.md 2.5): Seed at
// 23.4-23.5 above every DoT (20.2 and below), no Corruption, UA or CoA in 16 attempts, 11 % of sim.
// The math (base values from Spell.dbc, 3.3.5 coefficients, 2000 spell power, equal crit and haste on both sides):
//   Seed r9 explosion 1633-1897 + 0.2129 SP = ~2190 on each of N enemies, 2.0 s cast: ~1095 x N per cast second,
//     x1.05 Contagion: N = 3 -> ~3450 / s.
//   Corruption r10 6 x 180 + 1.2 SP = 3480 in a 1.5 s GCD: 2320 / s, x1.1 Improved Corruption x1.1 Shadow Embrace
//     x1.2 Haunt = ~3370 / s, and Everlasting Affliction keeps it rolling from Haunt / Shadow Bolt: one GCD buys the
//     whole fight, so it goes first at N = 3 and loses nothing at N >= 4.
//   Haunt: an 8 s cooldown nuke that carries the +20 % on every DoT above; it goes first too.
//   Unstable Affliction r5 5 x 230 + 1.0 SP = 3150 per 1.5 s cast: ~2770 / s with Shadow Embrace and Haunt, below
//     Seed from N = 3: it keeps its single-target rank (19.5) below the Seed (23.4-23.5) in a pack.
// So in a pack an Affliction warlock puts Haunt (when ready) and Corruption on its target first (on a target that
// lives CORRUPTION_MIN_TARGET_LIFETIME), and never seeds a unit that carries its Corruption.
bool AfflictionSeedWaits(PlayerbotAI* botAI, Unit* seedTarget);

class CastSeedOfCorruptionAction : public CastDebuffSpellAction
{
public:
    CastSeedOfCorruptionAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "seed of corruption", true, 0) {}
    bool isUseful() override
    {
        // Bypass TTL check
        return CastAuraSpellAction::isUseful() && !AfflictionSeedWaits(botAI, GetTarget());  // [b44_hos]
    }
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
};

class CastSeedOfCorruptionOnAttackerAction : public CastDebuffSpellOnAttackerAction
{
public:
    CastSeedOfCorruptionOnAttackerAction(PlayerbotAI* botAI)
        : CastDebuffSpellOnAttackerAction(botAI, "seed of corruption", true, 0)
    {
    }
    bool isUseful() override
    {
        // Bypass TTL check
        return CastAuraSpellAction::isUseful() && !AfflictionSeedWaits(botAI, GetTarget());  // [b44_hos]
    }
    // [b44_hos] Affliction: the attacker without Seed that also carries no Corruption of this warlock ("attacker
    // without aura" picks the healthiest one without Seed, often the main target with its Corruption).
    Unit* GetTarget() override;
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
};

class CastRainOfFireAction : public CastSpellAction
{
public:
    CastRainOfFireAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "rain of fire") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class CastHellfireAction : public CastSpellAction
{
public:
    CastHellfireAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "hellfire") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class CastShadowflameAction : public CastSpellAction
{
public:
    CastShadowflameAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "shadowflame") {}
    bool isUseful() override;
};

class CastShadowfuryAction : public CastSpellAction
{
public:
    CastShadowfuryAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "shadowfury") {}
    // [b33_aoe_class] Destruction talent: only a warlock that knows it
    bool isUseful() override { return botAI->HasSpell("shadowfury") && CastSpellAction::isUseful(); }
};

class CastImmolationAuraAction : public CastSpellAction
{
public:
    CastImmolationAuraAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "immolation aura") {}
    ActionThreatType getThreatType() override { return ActionThreatType::Aoe; }
    bool isUseful() override;
};

class ShadowCleaveAction : public CastMeleeSpellAction
{
public:
    ShadowCleaveAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "shadow cleave") {}
};
#endif
