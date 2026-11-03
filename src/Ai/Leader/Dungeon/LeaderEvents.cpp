/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderEvents.h"

#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "AKTriggers.h"
#include "ANAddTactics.h"
#include "Bag.h"
#include "BotMoveEvents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "CoSTriggers.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DBCStores.h"
#include "Engine.h"
#include "EscortProtect.h"  // [b36_ohb] EscortGossipHeld
#include "EstimatedLifetimeValue.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "GDTriggers.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "HealthTriggers.h"
#include "RaidLeaderState.h"
#include "HoRTriggers.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MechShared.h"
#include "MgTShared.h"
#include "ClassicShared.h"  // [b32_smpull] Mograine's linked chapel trash
#include "MotionMaster.h"
#include "OCTriggers.h"
#include "MoveSpline.h"
#include "AttackerCountValues.h"
#include "MovementActions.h"
#include "LeaderCommand.h"  // [lead410] SPELL_MASTER_OF_GHOULS
#include "LeaderOrders.h"  // [lead1] the tank's pull orders
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PartyMemberToHeal.h"  // [lead945] [lead946] HealTriage
#include "PathGenerator.h"
#include "RouteSearch.h"  // [lead99] the leader's route search
#include "PullFacts.h"    // [lead104] the leader's pull / fight facts
#include "LeaderFightChecks.h"  // [lead107] the leader's checks
#include "LeaderPrepull.h"      // [lead270]
#include "LeaderTrapWeave.h"    // [lead271]
#include "LeaderHazardVerdict.h"  // [lead780]
#include "LeaderSupport.h"      // [lead400] .. [lead405]
#include "RaidConsumables.h"    // [lead610] FindManaGem
#include "Pet.h"  // [lead38] the pet redirect
#include <source_location>  // [lead76]
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "PullActions.h"  // [b39_strat] FightHold: the opening fight held on the use side of a key gate
#include "ChooseTargetActions.h"  // [b47_door] PullReadiness::GroupPreparing
#include "SmartAI.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"  // [lead852] the member's periodic damage on a mob
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StuckWatchdog.h"
#include "DamageTaken.h"
#include "DungeonHazard.h"   // [lead16] the leader's hazard view
#include "LeaderActions.h"   // [lead19] the member guard model (LeaderMoveOrderAction::WalkRefusal)
#include "RtiValue.h"        // [lead16] the leader's CC assignments
#include "DungeonTactics.h"  // [lead15] the leader's cleave dodge (InFrontalCleave)
#include "InstanceScript.h"
#include "InstanceSaveMgr.h"
#include "SelfTravelActions.h"
#include "TemporarySummon.h"
#include "TerrainHazard.h"
#include "LeaderKnowledge.h"  // [lead234] boss kills
#include "Transport.h"
#include "UKTriggers.h"
#include "UPTriggers.h"
#include "Timer.h"
#include <chrono>  // [lead15] the leader cadence cost
#include <functional>
#include <optional>  // [lead18]
#include "WaypointMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSession.h"
#include "G3D/Vector4.h"  // [b32_brs] HopelessDetourLeg discs
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Leader::Dungeon
{
    using namespace Leader::Fight;

    // [lead274] a member under half health is in danger (the leader's own hurt line of its saves: Divine Hymn's
    // "three members under half health")
    constexpr float LEADER_DANGER_HEALTH_PCT = 50.0f;

    // [lead425] a mana tool of `caster` whose return is still coming: its cooldown started within its duration (a
    // Shadowfiend that is out, a Mana Tide Totem that is down - the core's cooldown, spell data)
    bool ManaToolOut(Player* caster, uint32 spell)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info || !caster->HasSpell(spell) || info->GetMaxDuration() <= 0)
            return false;
        // [lead840] b223 rfc-h 5B: the shadow priest's Shadowfiend recharges in 180 s (Veiled Shadows, the
        // leader's own "recharge 180 s"), the raw spell data says 300 s - its cooldown was never "within its
        // duration" of 300 s, the fiend never counted as out: its mana was netted into the boss fight's measured use
        // (Oggleflint 70 -> 99 % with the potion and the fiend, the boss rate 0.36 %/s) and Taragaman's line asked
        // 19 % of a priest that spends ~1.3 %/s (dry 26 s in, wand 10 s). The recharge is the caster's, with its
        // cooldown modifiers (talents, glyphs: SPELLMOD_COOLDOWN, as Player::AddSpellAndCategoryCooldowns applies)
        int32 recoveryMs = int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
        caster->ApplySpellMod(spell, SPELLMOD_COOLDOWN, recoveryMs);
        uint32 const recovery = uint32(std::max(recoveryMs, 0));
        uint32 const delay = caster->GetSpellCooldownDelay(spell);
        return delay && recovery > uint32(info->GetMaxDuration()) && delay > recovery - uint32(info->GetMaxDuration());
    }

    // [lead541] The seconds until `spell` is ready again once `caster` casts it now, from the game: its cooldown with
    // the caster's cooldown modifiers (talents, glyphs: SPELLMOD_COOLDOWN, as Player::AddSpellAndCategoryCooldowns
    // applies them), and for a cooldown that starts only when the spell's effect ends (SPELL_ATTR0_COOLDOWN_ON_EVENT /
    // the category's flag: Player::SendCooldownEvent on the summon's or aura's end) that effect's duration on top.
    // b212 rfc-h 5C: Raise Dead (category cooldown 180 s, COOLDOWN_ON_EVENT) was allowed on trash 26 s in "recharge
    // 180 s" and was ready again only at 281 s - its cooldown starts when the ghoul it raises (46585, 60 s, named by
    // the spell's script effect) expires. Bloodlust / Heroism: the group's Sated / Exhaustion (its own duration, spell
    // data) keeps it from the next one longer than the spell's cooldown.
    float RechargeAfterCastSec(Player* caster, uint32 spell)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info)
            return 0.0f;
        int32 ms = int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
        caster->ApplySpellMod(spell, SPELLMOD_COOLDOWN, ms);
        ms = std::max(ms, 0);
        // [lead581] b215 rfc-h 5C: the blood DK's Raise Dead was judged "recharge 240 s" (180 s + the ghoul's 60 s,
        // the cooldown-on-event rule) and held on trash ("held for Taragaman (his pull in 211 s, recharge 240 s)" at
        // 578 s); it was ready again at Jergosh 228 s after its cast at 804 s ([rot51] raise dead cd 0). The bot's
        // CastRaiseDeadAction starts its cooldown at the cast (the temporary ghoul is a guardian, the core's event
        // never comes for 46584) unless Master of Ghouls keeps the core's own: the real recharge is the cooldown only
        constexpr uint32 SPELL_RAISE_DEAD = 46584;
        bool const setAtCast = spell == SPELL_RAISE_DEAD && !caster->HasAura(LeaderCommand::SPELL_MASTER_OF_GHOULS);
        if (info->IsCooldownStartedOnEvent() && !setAtCast)
        {
            int32 lasts = std::max(info->GetMaxDuration(), 0);
            for (SpellEffectInfo const& effect : info->Effects)
            {
                uint32 named = effect.TriggerSpell;
                // a script / dummy effect that names the spell it casts (Raise Dead's ghoul: Effects[1] = 46585)
                if (!named && (effect.Effect == SPELL_EFFECT_SCRIPT_EFFECT || effect.Effect == SPELL_EFFECT_DUMMY) &&
                    effect.CalcValue() > 0)
                    named = uint32(effect.CalcValue());
                SpellInfo const* cast = named ? sSpellMgr->GetSpellInfo(named) : nullptr;
                if (cast && cast->SpellFamilyName == info->SpellFamilyName && cast->IsCooldownStartedOnEvent())
                    lasts = std::max(lasts, cast->GetMaxDuration());
            }
            ms += lasts;
        }
        return ms / 1000.0f;
    }

    // [lead854] An effect that lasts until its proc charges are spent on crits while each hit of its school adds a
    // stack of crit chance (Combustion: 11129's 3 charges, 28682's +10 % a stack, the core's spell_mage_combustion; its
    // cooldown starts once it is spent). Rawr.Mage (ComputeCombustion, CombustionFireCritBonus): the casts it lasts, N,
    // from the crit chain c0, c0 + 10 %, ... until the 3rd crit; during it the crit rate is 3 / N and the crit damage
    // bonus has the effect's SPELLMOD_CRIT_DAMAGE_BONUS (+50 %) on top; a fire crit's Ignite (the talent's share of the
    // crit) rides on both. Its gain: N casts x the member's non-crit cast damage (its measured dps x the cast) x the
    // crits' extra. b223 rfc-h 5C: Combustion was judged "effect 0 s", one second of the group's dps (7551 damage).
    // `effectSec`: N x the cast; `valuePerSec`: the gain over it. False: not such an effect or no damage measured yet
    bool ChargeCritEffect(Player* who, SpellInfo const* info, uint32 fillerSpell, float& effectSec, float& valuePerSec)
    {
        if (!info || !info->IsCooldownStartedOnEvent() || !info->ProcCharges || info->GetMaxDuration() > 0)
            return false;
        SpellInfo const* filler = fillerSpell ? sSpellMgr->GetSpellInfo(fillerSpell) : nullptr;
        if (!filler)
            return false;
        float perStack = 0.0f;
        float critDamagePct = 0.0f;
        uint32 const school = filler->GetSchoolMask();
        auto const read = [&](SpellInfo const* spell)
        {
            for (SpellEffectInfo const& effect : spell->GetEffects())
            {
                if (effect.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER &&
                    effect.MiscValue == SPELLMOD_CRITICAL_CHANCE)
                    perStack = std::max(perStack, float(effect.CalcValue(who)) / 100.0f);
                else if ((effect.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER ||
                          effect.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER) &&
                         effect.MiscValue == SPELLMOD_CRIT_DAMAGE_BONUS)
                    critDamagePct = std::max(critDamagePct, float(effect.CalcValue(who)) / 100.0f);
            }
        };
        read(info);
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (SpellInfo const* proc = effect.TriggerSpell ? sSpellMgr->GetSpellInfo(effect.TriggerSpell) : nullptr)
                read(proc);
        constexpr uint32 SPELL_COMBUSTION_PROC = 28682;  // spell_mage_combustion casts it (no effect names it)
        if (SpellInfo const* proc = sSpellMgr->GetSpellInfo(SPELL_COMBUSTION_PROC);
            proc && info->SpellFamilyName == SPELLFAMILY_MAGE)
            read(proc);
        if (perStack <= 0.0f)
            return false;
        float const own = GroupDamage::MemberSustainedDps(who);
        if (own <= 0.0f)
            return false;
        // the crit chain (Rawr: up to 13 casts)
        float c0 = 0.0f;
        for (uint8 s = SPELL_SCHOOL_HOLY; s < MAX_SPELL_SCHOOL; ++s)
            if (school & (1 << s))
                c0 = std::max(c0, who->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + s) / 100.0f);
        uint32 const charges = info->ProcCharges;
        std::vector<float> state(charges + 1, 0.0f);  // state[k]: k crits so far, still running
        state[0] = 1.0f;
        float crit = std::min(c0, 1.0f);
        float casts = 0.0f;
        for (uint32 cast = 1; cast <= 13; ++cast)
        {
            std::vector<float> next(charges + 1, 0.0f);
            for (uint32 k = 0; k < charges; ++k)
            {
                next[k] += state[k] * (1.0f - crit);
                next[k + 1] += state[k] * crit;
            }
            casts += next[charges] * float(cast);
            next[charges] = 0.0f;
            state = next;
            crit = std::min(crit + perStack, 1.0f);
        }
        if (casts <= 0.0f)
            return false;
        // the crit damage factors (Unit::SpellCriticalDamageBonus: the core's own; the effect's bonus on its 50 %)
        float const bonus = float(Unit::SpellCriticalDamageBonus(who, filler, 10000, nullptr)) / 10000.0f - 1.0f;
        float const base = 1.5f * (1.0f + who->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS,
                                                                              school) / 100.0f) - 1.0f;
        float ignite = 0.0f;  // Ignite (11119 / 11120 / 12846 / 12847 / 12848): its share of a fire crit
        if (school & SPELL_SCHOOL_MASK_FIRE)
            for (uint32 rank : {12848u, 12847u, 12846u, 11120u, 11119u})
                if (who->HasAura(rank))
                    if (SpellInfo const* talent = sSpellMgr->GetSpellInfo(rank))
                    {
                        ignite = float(talent->Effects[EFFECT_0].CalcValue(who)) / 100.0f;
                        break;
                    }
        float const critExtraBase = (1.0f + bonus) * (1.0f + ignite) - 1.0f;
        float const critExtraOn = (1.0f + bonus + base * critDamagePct) * (1.0f + ignite) - 1.0f;
        float const castSec = std::max(filler->CalcCastTime(who) / 1000.0f, 1.0f);
        float const perCastBase = 1.0f + c0 * critExtraBase;
        float const nonCrit = own * castSec / perCastBase;
        float const perCastOn = 1.0f + std::min(float(charges) / casts, 1.0f) * critExtraOn;
        effectSec = casts * castSec;
        valuePerSec = nonCrit * (perCastOn - perCastBase) / castSec;
        return valuePerSec > 0.0f;
    }

    // [lead425] a tool's return still comes to `member`: under Innervate, Hymn of Hope's tick, Evocation, Dispersion's
    // mana (and Divine Plea when `plea`), its Shadowfiend out, in reach of a Mana Tide Totem that is down
    bool ManaToolReturning(Player* member, std::vector<Player*> const& everyone, bool plea)
    {
        if (member->HasAura(SPELL_INNERVATE) || member->HasAura(RestState::SPELL_HYMN_OF_HOPE_TICK) ||
            member->HasAura(RestState::SPELL_EVOCATION) || member->HasAura(RestState::SPELL_DISPERSION_MANA) ||
            (plea && member->HasAura(RestState::SPELL_DIVINE_PLEA)) ||
            ManaToolOut(member, RestState::SPELL_SHADOWFIEND))
            return true;
        for (Player* shaman : everyone)
            if (ManaToolOut(shaman, SPELL_MANA_TIDE_TOTEM) &&
                member->GetExactDist(shaman) <= RestState::ManaToolRadius(shaman, SPELL_MANA_TIDE_TOTEM))
                return true;
        return false;
    }

    // [lead772] a dungeon boss in combat that fights the group: his victim a member or one's pet, or engaged by
    // one of them or a pet. b220 rfc-h 5E 1192.5 s: the Searing Blade Cultist pull "brings Bazzalan" (leader_boss_pull
    // 1184.1 s); Bazzalan was in combat (the pull's fight on the tank and the hunter's pet) before any member was
    // his victim or engaged him - neither this fight's boss nor a boss left to pull (the next boss is one out of
    // combat): "vanish: trash, no boss left", "bestial wrath: trash, no boss left" (boss_engaged 1194 s). A pull
    // with the boss in it is the boss fight
    bool BossFightsGroup(Creature* boss, Player* tank, std::vector<Player*> const& seats)
    {
        if (!boss || !boss->IsAlive() || !boss->IsInCombat())
            return false;
        Unit* victim = boss->GetVictim();
        Unit* victimOwner = victim ? victim->GetCharmerOrOwnerOrSelf() : nullptr;
        auto const fights = [&](Player* member)
        {
            if (!member)
                return false;
            if (victimOwner == member || boss->IsEngagedBy(member))
                return true;
            Pet* pet = member->GetPet();
            return pet && boss->IsEngagedBy(pet);
        };
        if (fights(tank))
            return true;
        for (Player* bot : seats)
            if (fights(bot))
                return true;
        return false;
    }

    // [lead274] the burn verdict of one member (`kind`: its cooldowns, or the group's Heroism / Bloodlust), logged
    // when which cooldowns are allowed / held changes (the reasons' running numbers are no change)
    void LogBurnDecision(DecisionRun const& run, DungeonLeaderState& lead, Player* bot, char const* kind,
                         std::string const& allowed, std::string const& held, bool bossFight)
    {
        auto const names = [](std::string const& notes)
        {
            std::string out;
            size_t start = 0;
            while (start < notes.size())
            {
                size_t const end = std::min(notes.find("; ", start), notes.size());
                std::string const one = notes.substr(start, end - start);
                std::string const reason = one.substr(one.find(':') == std::string::npos ? one.size() : one.find(':'));
                out += one.substr(0, one.find(':')) + "|" + reason.substr(0, reason.find(" (")) + ",";
                start = end + 2;
            }
            return out;
        };
        std::string const sig = names(allowed) + "/" + names(held);
        std::string& logged = lead.burnLogged[bot->GetGUID().ToString() + kind];
        if (logged == sig || (allowed.empty() && held.empty()))
            return;
        logged = sig;
        Decisions().Write(run, "leader", "burn_decision", bot->GetGUID(), bot->GetName(), true,
                          JsonObject().Str("member", bot->GetName()).Str("kind", kind).Str("allowed", allowed)
                              .Str("held", held).Bool("bossFight", bossFight)
                              .Real("fightSec", lead.fightSecLeft).Real("fightOverload", lead.fightOverload)
                              .Real("bossPullSec", lead.bossPullSec).Str("bossPullParts", lead.bossPullParts)
                              .Str("nextBoss", lead.nextBossName));
    }

    // [lead22] slabs-h 20261026-201857: the leader told a full healer to Rest 52 times («[rest] Rtdruaa stands up:
    // full (hp 100 %, mana 100 %)») and its readiness waited for that sitter forever. A member at 99 % health and mana
    // never rests and a full sitter holds nothing (the bot's own stand-up rule: RestDone, full)
    bool MemberFull(Player* bot)
    {
        return bot && bot->GetHealthPct() >= 99.0f &&
               (!bot->GetMaxPower(POWER_MANA) || bot->GetPowerPct(POWER_MANA) >= 99.0f);
    }

    namespace
    {
        constexpr float HEADROOM_GCD_SEC = 1.5f;  // [lead163] the game's global cooldown: one cast's threat
        // [lead243] a mob on its victim moves to another at 110 % of the victim's threat in its melee reach, else at
        // 130 % (ThreatManager::ReselectVictim)
        constexpr float THREAT_OVERTAKE_MELEE = 1.1f;
        constexpr float THREAT_OVERTAKE_RANGED = 1.3f;

        // [lead137] a left combat spot keeps the member off the ground this near it
        constexpr float COMBAT_SPOT_LEFT_YD = 2.0f;

        // [lead254] b194 crash (FreezeDetector, world thread 60 s in combatSpotLeft[guid].push_back): every respot
        // and every blind / rejected look appended the spot again, never deduped, cleared only at a fight's end, and
        // each spot search scanned the list per candidate point. A member's left spots are those of its current
        // target, each place once (a spot within COMBAT_SPOT_LEFT_YD of a listed one is that one)
        void LeaveCombatSpot(std::vector<std::pair<ObjectGuid, Position>>& left,
                             std::pair<ObjectGuid, Position> const& spot)
        {
            left.erase(std::remove_if(left.begin(), left.end(), [&](std::pair<ObjectGuid, Position> const& was)
                                      { return was.first != spot.first; }),
                       left.end());
            if (std::none_of(left.begin(), left.end(), [&](std::pair<ObjectGuid, Position> const& was)
                             { return was.second.GetExactDist(spot.second) <= COMBAT_SPOT_LEFT_YD; }))
                left.push_back(spot);
        }

        // [lead243] the seconds a threat was built in: its first sight to its last, at least the global cooldown
        // its first cast landed in
        float ThreatSeenSec(Leader::DungeonLeaderState::ThreatSeen const& seen)
        {
            return std::max(getMSTimeDiff(seen.firstMs, seen.lastMs) / 1000.0f, HEADROOM_GCD_SEC);
        }

        // [lead561] its threat per second there: what it built itself since its first threat (a taunt's copy of the
        // top threat is no rate of the tank's)
        float ThreatSeenRate(Leader::DungeonLeaderState::ThreatSeen const& seen)
        {
            return std::max(0.0f, seen.threat - seen.base) / ThreatSeenSec(seen);
        }

        // [lead900] a spell's threat for the member by the game's rules: the spell threat data's multiplier
        // (spell_threat, ThreatManager::CalculateModifiedThreat), its talents' threat spell mods and its school's
        // threat auras (presence, Salvation)
        float SpellThreatFor(Player* who, SpellInfo const* info, float amount)
        {
            if (info->HasAttribute(SPELL_ATTR1_NO_THREAT))
                return 0.0f;
            if (SpellThreatEntry const* entry = sSpellMgr->GetSpellThreatEntry(info->Id))
                amount *= entry->pctMod;
            who->ApplySpellMod(info->Id, SPELLMOD_THREAT, amount);
            return amount * who->GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_THREAT, info->GetSchoolMask());
        }

        // [lead900] user 2026-11-03: the threat a member's first cast puts on a mob it has not hit yet. Of its ready
        // damage spells whose spell threat data multiplies their threat (Icy Touch x7), the largest: its direct
        // damage with its bonuses, the data's multiplier, flat and attack power threat, its threat mods. The spells
        // come from the game's threat data, not a list; the member's known ones are read again when its spells change
        float FirstCastThreat(Player* who, Unit* mob, uint32& spellOut)
        {
            struct Known
            {
                size_t spells = 0;
                uint8 spec = 0;
                std::vector<uint32> raised;
            };
            thread_local std::unordered_map<uint64, Known> known;
            Known& mine = known[who->GetGUID().GetRawValue()];
            if (mine.spells != who->GetSpellMap().size() || mine.spec != who->GetActiveSpec())
            {
                mine.spells = who->GetSpellMap().size();
                mine.spec = who->GetActiveSpec();
                mine.raised.clear();
                for (auto const& [id, spell] : who->GetSpellMap())
                {
                    if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active ||
                        !spell->IsInSpec(who->GetActiveSpec()))
                        continue;
                    SpellThreatEntry const* entry = sSpellMgr->GetSpellThreatEntry(id);
                    if (!entry || entry->pctMod <= 1.0f)
                        continue;
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                    if (info && !info->IsPassive() && !info->IsPositive() &&
                        info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE))
                        mine.raised.push_back(id);
                }
            }
            float best = 0.0f;
            spellOut = 0;
            for (uint32 id : mine.raised)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                if (!info || who->HasSpellCooldown(id))
                    continue;
                if (info->PowerType != POWER_RUNE && info->PowerType < MAX_POWERS &&
                    info->CalcPowerCost(who, info->GetSchoolMask()) > int32(who->GetPower(Powers(info->PowerType))))
                    continue;
                float damage = 0.0f;
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (info->Effects[i].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                        damage += float(who->SpellDamageBonusDone(
                            mob, info, uint32(std::max(0, who->CalculateSpellDamage(mob, info, i))),
                            SPELL_DIRECT_DAMAGE, i));
                float threat = SpellThreatFor(who, info, damage);
                if (SpellThreatEntry const* entry = sSpellMgr->GetSpellThreatEntry(id))
                    threat += SpellThreatFor(who, info,
                                             float(entry->flatMod) +
                                                 entry->apPctMod * who->GetTotalAttackPowerValue(BASE_ATTACK));
                if (threat > best)
                {
                    best = threat;
                    spellOut = id;
                }
            }
            return best;
        }

        // [lead901] the threat a member's running periodic effects still put on a mob, which no Stop takes back:
        // its damage over time on the mob (diseases, Death and Decay's aura, Corruption...) and, split as the game
        // splits a heal's threat (half the heal over the mobs that have the healed member on their threat list,
        // ThreatManager::ForwardThreatForAssistingMe), its heals over time on members the mob has on its threat list
        // (Lifebloom, Rejuvenation, Renew, Riptide...). Each tick left at its bonuses and threat mods; withinOut is
        // the part that lands within the GCD, secOut the longest of them left
        struct PeriodicLeft
        {
            float threat = 0.0f;
            float within = 0.0f;
            float sec = 0.0f;
        };
        PeriodicLeft PeriodicThreatLeft(Player* who, Unit* mob)
        {
            PeriodicLeft left;
            auto const add = [&](AuraEffect const* periodic, float perTick)
            {
                int32 const ticks = periodic->GetTotalTicks() - int32(periodic->GetTickNumber());
                if (ticks <= 0 || perTick <= 0.0f || periodic->GetAmplitude() <= 0)
                    return;
                float const sec = float(ticks * periodic->GetAmplitude()) / IN_MILLISECONDS;
                float const threat = perTick * float(ticks);
                left.threat += threat;
                left.within += threat * std::min(1.0f, HEADROOM_GCD_SEC / std::max(sec, 0.001f));
                left.sec = std::max(left.sec, sec);
            };
            for (AuraType type : {SPELL_AURA_PERIODIC_DAMAGE, SPELL_AURA_PERIODIC_LEECH})
                for (AuraEffect const* periodic : mob->GetAuraEffectsByType(type))
                    if (periodic->GetCasterGUID() == who->GetGUID())
                    {
                        SpellInfo const* info = periodic->GetSpellInfo();
                        float const tick = float(who->SpellDamageBonusDone(
                            mob, info, uint32(std::max(periodic->GetAmount(), 0)), DOT, periodic->GetEffIndex(),
                            0.0f, periodic->GetBase()->GetStackAmount()));
                        add(periodic, SpellThreatFor(who, info, tick));
                    }
            for (auto const& entry : mob->GetThreatMgr().GetUnsortedThreatList())
            {
                Unit* healed = entry ? entry->GetVictim() : nullptr;
                if (!healed || !healed->IsPlayer())
                    continue;
                size_t split = 0;
                for (auto const& [threatenedGuid, ref] : healed->GetThreatMgr().GetThreatenedByMeList())
                    if (ref && !ref->GetOwner()->HasUnitState(UNIT_STATE_CONTROLLED))
                        ++split;
                if (!split)
                    continue;
                for (AuraEffect const* periodic : healed->GetAuraEffectsByType(SPELL_AURA_PERIODIC_HEAL))
                    if (periodic->GetCasterGUID() == who->GetGUID())
                    {
                        SpellInfo const* info = periodic->GetSpellInfo();
                        float const tick = float(who->SpellHealingBonusDone(
                            healed, info, uint32(std::max(periodic->GetAmount(), 0)), DOT, periodic->GetEffIndex(),
                            0.0f, periodic->GetBase()->GetStackAmount()));
                        add(periodic, SpellThreatFor(who, info, tick * 0.5f / float(split)));
                    }
            }
            return left;
        }

        // [lead221] the planner's held answer named this mana line for the member (lead.plannerRestLine): its Rest
        // holds below it; without a named line (a health gap) the Rest holds as before, until the member is full
        bool BelowPlannerLine(Player* bot, float line)
        {
            return line < 0.0f || !bot->GetMaxPower(POWER_MANA) || bot->GetPowerPct(POWER_MANA) < line;
        }

        // [lead226] b185 rfc-h 5B 721-838 s: 50 "sits: to full" Rests to the tank eating in Ragefire's magma - each
        // tick of the lava broke the food and the next tick ordered the Rest again. What hurts a member out of a
        // fight (lava / slime under it, a damage over time on it) is no fact for a Rest: the Rest waits until it ends
        char const* HurtOutOfFight(Player* bot)
        {
            if (InHarmfulLiquid(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
                return "it stands in lava / slime";
            if (bot->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) || bot->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT))
                return "a damage over time is on it";
            return nullptr;
        }

        std::string PlannerWaitWhy(float line)
        {
            return line < 0.0f ? std::string("the planner waits for it")
                               : Acore::StringFormat("the planner waits for it (its line {:.0f} % mana)", line);
        }
    }

    namespace
    {
        // [lead217] a loose mob that stands off its victim (out of its melee reach, not moving: a caster at range, a
        // rooted mob) does not follow the victim anywhere
        bool LooseStandsOff(Unit* mob, Unit* victim)
        {
            return mob && victim && !mob->isMoving() && !mob->IsWithinMeleeRange(victim);
        }

        // [lead218] a ranged member casts on `target` from where it stands: its reach and minimum range
        // (LeaderCastsFrom: a hunter's dead zone) and the game's own line of sight from it (the one its spells use)
        bool MemberCastsHere(Player* bot, Unit* target)
        {
            return LeaderCastsFrom(bot, target, bot->GetPosition()) && bot->IsWithinLOSInMap(target);
        }

        // [lead380] why a ranged member does not cast on `target` from where it stands (the checks of LeaderCastsFrom,
        // the mobs where they stand now, then the game's sight), for the re-spot record
        std::string MemberCastsFails(Player* bot, Unit* target, std::vector<ObjectGuid> const& fightMobs)
        {
            Position const at = bot->GetPosition();
            float const gap = at.GetExactDist(target->GetPosition());
            if (gap > LeaderCastReach(bot, target))  // [lead660]
                return "out of spell range of the target";
            if (bot->getClass() == CLASS_HUNTER && gap < LeaderRangedMinGap(bot, target))
                return "inside its minimum range of the target";
            if (!LeaderSeesFrom(bot, target, at) || !bot->IsWithinLOSInMap(target))
                return "no line of sight to the target";
            if (!LeaderOutOfMobsMelee(bot, at, fightMobs, true))
                return "inside a fight mob's melee reach";
            return "";
        }
    }

    void TickLeaderEvents(LeaderGroup& group, Player* tank)
    {
        auto& lead = group.State();
        uint32 const now = getMSTime() ? getMSTime() : 1;
        if (!Leader::LEADER_MEMBER_EVENTS || !tank || !tank->IsInWorld())
            return;
        // [lead15] the cadence (user: «чем быстрее у него реакция, тем лучше»): the FAST layer runs on every update
        // while the group fights, a pull waits for its spots or a hazard is active; calm, every LEADER_CALM_TICK_MS.
        // The SLOW layer (exit / re-spot searches, the AoE rule's grid visit) runs only on a change (cached).
        // [lead93] b134 rfc-h 1206 s: the whole group was ported to the entrance (map 1) and the leader went on with
        // in-instance moves ("refused a straight move over open air on map 1 ... to (-330.8,243.0,-11.7)"). Outside
        // the instance the leader issues nothing (logged once); the harness's own re-entry walk brings them back
        // [lead227] b185 rfc-h 5B 838-920 s: the leader's last "Follow along with the tank" (838.6 s, 174 yd) stood
        // after the tank burned to death and its ghost left the instance - nobody re-asserted or ended it, the rogue
        // walked alone to the tank's corpse by the Enforcers, pulled them (913 s) and died (920 s). A dead leader or
        // one outside commands nothing: its members' commands end once (the group waits), it leads again when back
        bool const leaderDown = !tank->IsAlive() || tank->GetMapId() != group.MapId();
        if (leaderDown && !lead.leaderDownCleared)
        {
            lead.leaderDownCleared = true;
            for (ObjectGuid const& groupMember : group.MemberGuids())
                Leader::Events::Clear(groupMember);
            Decisions().Write(group.Run(), "leader", "leader_down", tank->GetGUID(), tank->GetName(), true,
                              JsonObject().Str("why", tank->IsAlive() ? "the leader is outside the instance"
                                                                      : "the leader is dead")
                                  .Str("then", "every member's command ended; the group waits"));
        }
        if (!leaderDown)
            lead.leaderDownCleared = false;
        if (!tank->IsAlive())
            return;
        if (tank->GetMapId() != group.MapId())
        {
            if (!lead.outsideLogged)
            {
                lead.outsideLogged = true;
                Decisions().Write(group.Run(), "leader",
                                              "leader_outside", tank->GetGUID(), tank->GetName(), true,
                                              JsonObject().Num("map", tank->GetMapId())
                                                  .Num("instanceMap", group.MapId()));
            }
            return;
        }
        lead.outsideLogged = false;
        bool anyCombat = group.FightOn(tank);  // [lead84] the one definition
        for (ObjectGuid const& groupMember : group.MemberGuids())
            if (!anyCombat)
                if (Player* bot = ObjectAccessor::FindPlayer(groupMember);
                    bot && bot->IsInCombat() && !bot->getAttackers().empty())
                    anyCombat = true;
        bool const prePull = lead.spotsWaiting || !lead.spots.empty();
        uint32 const state = lead.hazardActive ? 3 : anyCombat ? 2 : prePull ? 1 : 0;
        // [lead72] user: 0.3 s is fast enough - the fight / pre-pull / hazard pass runs at most every
        // LEADER_FIGHT_TICK_MS, not on every world update (b116: ~100 healer events in 0.2 s)
        uint32 const period = state ? LEADER_FIGHT_TICK_MS : LEADER_CALM_TICK_MS;
        if (lead.eventTickMs && getMSTimeDiff(lead.eventTickMs, now) < period)
            return;
        lead.eventTickMs = now;
        auto const tickStart = std::chrono::steady_clock::now();
        lead.slowUsTick = 0;
        ++lead.cadenceTicks[state];
        bool hazardSeen = false;
        DecisionRun const run = group.Run();
        Map* map = tank->GetMap();
        // [lead18] the group's walked ground (cleared): a sample per member and the tank every 3 yd
        for (ObjectGuid const& groupMember : group.MemberGuids())
            if (Player* walker = ObjectAccessor::FindPlayer(groupMember);
                walker && walker->IsAlive() && walker->GetMap() == map)
            {
                Position& last = lead.clearedLast[groupMember];
                if (last.GetExactDist(walker->GetPosition()) >= 3.0f)
                {
                    last = walker->GetPosition();
                    lead.clearedGround.push_back(last);
                    if (lead.clearedGround.size() > 800)
                        lead.clearedGround.erase(lead.clearedGround.begin(), lead.clearedGround.begin() + 100);
                }
            }
        // [lead18] the spot guard's idle mobs (refreshed every 500 ms: the slow layer)
        if (!lead.idleScanMs || getMSTimeDiff(lead.idleScanMs, now) >= 500)
        {
            lead.idleScanMs = now;
            std::list<Creature*> idle;
            IdleAggroCheck check{tank};
            Acore::CreatureListSearcher<IdleAggroCheck> searcher(tank, idle, check);
            Cell::VisitObjects(tank, searcher, 80.0f);
            lead.idleGuids.clear();
            for (Creature* mob : idle)
                lead.idleGuids.push_back(mob->GetGUID());
        }
        std::vector<Creature*> idleMobs;
        for (ObjectGuid const& guid : lead.idleGuids)
            if (Creature* mob = map->GetCreature(guid); mob && mob->IsAlive() && !mob->IsInCombat())
                idleMobs.push_back(mob);
        g_leaderIdle = &idleMobs;
        g_leaderCleared = &lead.clearedGround;
        std::vector<ObjectGuid> pullMobs = LeaderPullMobs(lead.commit, lead.commitMob);
        pullMobs.insert(pullMobs.end(), lead.joinedMobs.begin(), lead.joinedMobs.end());  // [lead76]
        g_leaderExempt = &pullMobs;
        g_leaderJoined = &lead.joinedMobs;  // [lead600]
        // [lead115] the pull walks: the engage order open (not fighting yet) and the tank without threat on its mob
        bool pullWalk = false;
        uint32 pullWalkOrder = 0;
        {
            Leader::PullOrder walkOrder;
            if (lead.commit && lead.commitIssued && Leader::Orders::GetOpen(group.LeaderGuid(), walkOrder) &&
                walkOrder.method == Leader::PullMethod::Engage && walkOrder.state != Leader::OrderState::Fighting)
                if (Unit* walkMob = ObjectAccessor::GetUnit(*tank, lead.commitMob); walkMob && walkMob->IsAlive())
                {
                    // [lead118] b152 rfc-h 870-931 s: the healer's pull-walk Hold outlived the pull (pull_start at
                    // 875 s, the healer 58 yd off the tank to the wipe) - the walk ends with the fight: the tank in
                    // combat, the group's pull on, or the tank's threat on the pull's mob
                    pullWalk = !tank->IsInCombat() && !group.InCombat() &&
                               (!walkMob->IsInCombat() || walkMob->GetThreatMgr().GetThreat(tank) <= 0.0f);
                    pullWalkOrder = walkOrder.id;
                }
        }
        if (!pullWalk && !lead.pullWalkSpots.empty())
        {
            // [lead117] the pull walk's decisions end with it; [lead118] and their commands: a member held by the
            // pull walk is let go at once (its fight spot / heal reach takes it this tick)
            for (auto const& [member, spot] : lead.pullWalkSpots)
            {
                Leader::MemberEvents events;
                if (Leader::Events::Get(member, events) && events.moveWhy == "the pull walks")
                    Leader::Events::SetMove(member, Leader::MoveEvent::None, spot.spot, 0.0f, "the pull walk ended");
            }
            lead.pullWalkSpots.clear();
        }
        struct GuardReset
        {
            ~GuardReset()
            {
                g_leaderIdle = nullptr;
                g_leaderCleared = nullptr;
                g_leaderExempt = nullptr;
                g_leaderJoined = nullptr;
            }
        } guardReset;
        // [lead18] every MoveTo / StandAt the leader issues: the spot and the walk out of every unpulled mob's reach;
        // a rejected one is not issued (the member stays) and logged as spot_rejected_aggro
        // [lead76] user: the leader computes, bigger pulls are better if survivable - a fight whose spots or walks an
        // idle pack's reach blocks: once per pack, the capacity model (JudgePack) on the fight plus that pack decides -
        // it fits: the pack joins the pull (its mobs count, no longer block); it does not: the fight moves away from
        // it (the tank to a spot out of its reach, on the group's side). [lead374] one judge for checkedMove's spots
        // and the chase's walk
        // the idle mob and its pack mates within its aggro range of it
        auto const packOf = [&](Creature* mob)
        {
            std::list<Creature*> pack;
            IdleAggroCheck packCheck{tank};
            Acore::CreatureListSearcher<IdleAggroCheck> packSearcher(mob, pack, packCheck);
            float const packReach = mob->GetAggroRange(tank);
            Cell::VisitObjects(mob, packSearcher, packReach);
            std::vector<ObjectGuid> packMobs{mob->GetGUID()};
            for (Creature* other : pack)
                if (other != mob && other->GetExactDist(mob) <= packReach)
                    packMobs.push_back(other->GetGUID());
            return packMobs;
        };
        // the pack is the fight's: its mobs block no spot or walk (lead600) and are the pull's for the members' own
        // guards (Leader::PullPack)
        auto const joinPack = [&](Creature* mob, std::vector<ObjectGuid> const& packMobs)
        {
            lead.joinedPack[mob->GetGUID()] = mob->GetGUID();
            lead.joinedMobs.insert(packMobs.begin(), packMobs.end());
            if (Group* group = tank->GetGroup())
            {
                std::vector<ObjectGuid> all = LeaderPullMobs(lead.commit, lead.commitMob);
                all.insert(all.end(), lead.joinedMobs.begin(), lead.joinedMobs.end());
                Leader::PullPack::Set(group->GetGUID(), all);
            }
        };
        auto const judgeBlockingPack = [&](Player* bot, Creature* mob)
        {
            if (!mob || !group.FightOn(tank) || lead.joinedPack.count(mob->GetGUID()))
                return;
            std::vector<ObjectGuid> combined;
            for (Unit* attacker : tank->getAttackers())
                if (attacker && attacker->IsAlive())
                    combined.push_back(attacker->GetGUID());
            std::vector<ObjectGuid> const packMobs = packOf(mob);
            float const packReach = mob->GetAggroRange(tank);
            combined.insert(combined.end(), packMobs.begin(), packMobs.end());
            float overload = 0.0f;
            std::string judged;
            bool const fits = PullPlanner::JudgePack(tank, combined, overload, judged);
            lead.joinedPack[mob->GetGUID()] = fits ? mob->GetGUID() : ObjectGuid::Empty;
            if (fits)
                joinPack(mob, packMobs);
            else
            {
                // away from the pack, out of its reach (its aggro range + the spot guard's margin)
                // [lead734] b219 rfc-h 5G pull 8 (91-149 s): the DK tank stood just out of an idle Trogg's reach and
                // the Trogg it held in it - every melee place round that Trogg was in the idle one's reach, three
                // melee stood 58 s. The fight is out of the pack's reach when the tank's target and the melee places
                // round it are: the tank stands that much farther (its gap to the target, the target's combat reach,
                // the member's)
                Unit* const held = tank->GetVictim();
                float const fightRing = held && held->IsAlive() ? tank->GetExactDist2d(held) + held->GetCombatReach() +
                                                                       bot->GetCombatReach()
                                                                 : 0.0f;
                float const dx = tank->GetPositionX() - mob->GetPositionX();
                float const dy = tank->GetPositionY() - mob->GetPositionY();
                float const length = std::max(0.1f, std::hypot(dx, dy));
                float const away = packReach + Leader::LEADER_SPOT_AGGRO_MARGIN + fightRing;
                float x = mob->GetPositionX() + dx / length * std::max(away, length);
                float y = mob->GetPositionY() + dy / length * std::max(away, length);
                float z = tank->GetPositionZ();
                if (tank->GetMap()->CheckCollisionAndGetValidCoords(tank, tank->GetPositionX(), tank->GetPositionY(),
                                                                    tank->GetPositionZ(), x, y, z))
                    lead.fightMoveTo = Position(x, y, z);
            }
            Decisions().Write(run, "leader", fits ? "pack_joins" : "fight_moves_away", mob->GetGUID(), mob->GetName(),
                              true,
                              JsonObject()
                                  .Num("packMobs", int64(packMobs.size()))
                                  .Real("overload", overload)
                                  .Str("judged", judged)
                                  .Str("blocked", bot->GetName()));
        };
        // [lead374] b197b rfc-h (8 runs): 50 "chase: behind the target" and 8 "along with the tank" Follows refused by
        // the members' own idle-path guard - the leader issued Follows it never judged. Every Follow is judged before
        // it goes out by the member's own walk to its place (LeaderMoveOrderAction::DryFollow: the executor's place,
        // its guards run dry); "" - it walks there, else what its walk would meet and the idle mob it would wake
        auto const followRefusal = [&](Player* bot, Unit* target, uint8 index, ObjectGuid& mob,
                                       bool chase = false) -> std::string
        {
            mob.Clear();
            PlayerbotAI* memberAI = bot && target ? GET_PLAYERBOT_AI(bot) : nullptr;
            auto* mover = memberAI ? dynamic_cast<LeaderMoveOrderAction*>(
                                         memberAI->GetAiObjectContext()->GetAction("leader move order"))
                                   : nullptr;
            return mover ? mover->DryFollow(target, index, mob, chase) : std::string();
        };
        // [lead730] a stay the leader's own guards issue in place of the move it judged ("<why>: stays (...)") is the
        // member's live command like any other: logged as its leader_event (b219 rfc-h: 167 "to the tank's stop
        // point" stays wrote none, fight_breakdown read the Follow before them as the live command)
        auto const logStay = [&](Player* bot, std::string const& stay)
        {
            ++lead.leaderEventCounts[bot->GetName()];
            Decisions().Write(run, "leader", "leader_event", bot->GetGUID(), bot->GetName(), true,
                              JsonObject()
                                  .Str("why", stay)
                                  .Real("x", bot->GetPositionX())
                                  .Real("y", bot->GetPositionY())
                                  .Real("z", bot->GetPositionZ())
                                  .Str("member", bot->GetName())
                                  .Str("event", MoveEventName(Leader::MoveEvent::StandAt)));
        };
        // [lead374] the walk along with the tank: a Follow the member's walk takes is issued; one it cannot take
        // safely is not - the member stays and the group holds for it (group_blocked: the tank walks on only where
        // the group follows, lead77), the idle mob in the way is the next pull (lead364). Returns the command changed
        auto const followTank = [&](Player* bot, uint8 index, std::string const& why) -> bool
        {
            ObjectGuid mob;
            std::string const refusal = followRefusal(bot, tank, index, mob);
            if (refusal.empty())
                return Leader::Events::SetFollow(bot->GetGUID(), tank->GetGUID(), index, why);
            Creature* woken = mob.IsEmpty() ? nullptr : map->GetCreature(mob);
            std::string const stay = "along with the tank: stays (" +
                                     (woken ? "its walk would wake " + woken->GetName() : std::string("no safe walk")) +
                                     ")";
            bool const changed =
                Leader::Events::SetMove(bot->GetGUID(), Leader::MoveEvent::StandAt, bot->GetPosition(), 2.0f, stay);
            if (changed)
                logStay(bot, stay);  // [lead730]
            if (!lead.groupBlockedBy.count(bot->GetGUID()))
                Decisions().Write(run, "leader", "group_blocked", bot->GetGUID(), bot->GetName(), true,
                                  JsonObject()
                                      .Str("member", bot->GetName())
                                      .Str("why", refusal)
                                      .Str("mob", woken ? woken->GetName() : "-")
                                      .Str("walk", why));
            lead.groupBlockedBy[bot->GetGUID()] = mob;
            return changed;
        };
        auto const checkedMove = [&](ObjectGuid member, Leader::MoveEvent move, Position const& spot, float tolerance,
                                     std::string const& why) -> bool
        {
            Player* bot = ObjectAccessor::FindPlayer(member);
            // [lead127] the arbiter's one owner: a move barred for the member is neither judged nor issued
            // [lead137] ... and no "along with the tank" walk while the group fights (b162: the shaman 83 s)
            if (Leader::Events::MoveBarred(member, why) ||
                (why.rfind("along with the tank", 0) == 0 && group.FightOn(tank)))
                return false;
            // [lead236] b188 rfc-h 5G 126.1 s: "combat spot: behind the target" issued at (1.06e24, 4.6e30, 4.0e24) -
            // a spot off every map grid is no spot (a broken source; logged once per member and reason)
            if (move != Leader::MoveEvent::Hold &&
                !Acore::IsValidMapCoord(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ()))
            {
                thread_local std::set<std::pair<ObjectGuid, std::string>> invalidLogged;
                if (invalidLogged.size() > 4096)
                    invalidLogged.clear();
                if (invalidLogged.insert({member, why}).second)
                    LOG_ERROR("playerbots", "[lead236] invalid_spot: {} for {} at ({},{},{}) not issued", why,
                              member.ToString(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
                return false;
            }
            if (bot && move != Leader::MoveEvent::Hold && Leader::LEADER_SPOT_AGGRO_GUARD)
            {
                auto& checked = lead.spotChecked[member];
                // [lead143] b166 20261101-134022 73.7-76.5 s: Rtshaab's "aggro loss: to the tank" refused by its own
                // guards ("no route"), the refusal not kept - the next tick's spot within 1 yd of the last accepted
                // one skipped the member check and issued the move again (command_repeat). A refusal stands until
                // a fact changes: the spot, the member or the blocking mob moved
                bool const sameSpot = checked.ms && checked.spot.GetExactDist(spot) <= 1.0f;
                bool again = !sameSpot || getMSTimeDiff(checked.ms, now) > 2 * IN_MILLISECONDS;
                if (sameSpot && !checked.reject.empty())
                {
                    Creature* blocker = checked.mob.IsEmpty() ? nullptr : bot->GetMap()->GetCreature(checked.mob);
                    again = bot->GetExactDist(checked.from) > 1.0f ||
                            (!checked.mob.IsEmpty() &&
                             (!blocker || !blocker->IsAlive() || blocker->GetExactDist(checked.mobAt) > 1.0f));
                }
                std::string reject;
                // [lead151] the tank's own spot in a fight is no idle mob's reach: the tank stands there unwoken
                // [lead152] ... and the point of the tank's walk the fallback chose for this member
                auto const ground = lead.tankGroundSpots.find(member);
                bool const tankSpot = group.FightOn(tank) && tank->IsAlive() &&
                                      (spot.GetExactDist(tank->GetPosition()) <= 1.0f ||
                                       (ground != lead.tankGroundSpots.end() &&
                                        spot.GetExactDist(ground->second) <= 0.5f));
                // [lead490] [lead492] a member's combat spot on its target, and its walk there, are judged against
                // the fight at that target (LeaderFightAggroAt): no deeper into an idle mob's reach than it stands
                Unit* fightTarget = nullptr;
                if (auto const held = lead.combatSpots.find(member);
                    held != lead.combatSpots.end() && held->second.second.GetExactDist(spot) <= 0.5f &&
                    group.FightOn(tank))
                    fightTarget = ObjectAccessor::GetUnit(*bot, held->second.first);
                Creature* mob = tankSpot      ? nullptr
                                : fightTarget ? LeaderFightAggroAt(bot, spot, fightTarget)
                                              : LeaderAggroAt(bot, spot);
                bool noPath = false;
                if (!mob && again)
                    mob = LeaderAggroOnWalk(bot, spot, noPath, fightTarget);
                bool const kept = !mob && !again && !checked.reject.empty();
                if (kept)
                {
                    reject = checked.reject;
                    noPath = checked.noPath;
                    mob = checked.mob.IsEmpty() ? nullptr : bot->GetMap()->GetCreature(checked.mob);
                }
                if (!kept)
                {
                    if (mob)
                        reject = "the spot or its walk is in the reach of " + mob->GetName();
                    else if (noPath)
                        reject = "no walk to the spot";
                    // [lead93] no spot in the instance's exit areatrigger (it ports the member out)
                    else if (uint32 const exit = InInstanceExit(bot->GetMapId(), spot.GetPositionX(),
                                                                spot.GetPositionY(), spot.GetPositionZ(),
                                                                bot->GetCombatReach()))
                        reject = Acore::StringFormat("the spot is in the instance's exit (areatrigger {})", exit);
                }
                // [lead19] the stricter model: the member's own guard (its idle-wake rule, a closed door) agrees too
                if (reject.empty() && again)
                    if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(bot))
                        if (auto* mover = dynamic_cast<LeaderMoveOrderAction*>(
                                memberAI->GetAiObjectContext()->GetAction("leader move order")))
                        {
                            reject = mover->WalkRefusal(spot);
                            // [lead71] b115 220.8 s: Rtshaab refused the leader's meeting-point move ("a straight
                            // move over open air") - one verdict: the member's own MoveTo guards, run dry from where
                            // it stands, before the move is issued
                            if (reject.empty() && !mover->DryMoveTo(spot, why))
                                reject = "the member's own move guards refuse it: " + LastMoveRefusal();  // [lead112]
                        }
                if (again)  // [lead143] the verdict and the facts it was judged on
                    checked = {spot, bot->GetPosition(), now, reject, noPath, mob ? mob->GetGUID() : ObjectGuid::Empty,
                               mob ? mob->GetPosition() : Position()};
                if (!reject.empty())
                {
                    // [lead81] b124: group_blocked / spot_rejected_aggro every tick (the reason's running numbers, a
                    // stay that flipped between two texts) - the reason without its numbers, logged on a change only
                    std::string const reason = reject.substr(0, reject.find(" ("));
                    lead.spotRejectLogMs[member] = now;
                    std::string& logged = lead.spotRejectLogged[member];
                    if (logged != why + "|" + reason)
                    {
                        logged = why + "|" + reason;
                        // [lead734] where the idle mob, the spot and the tank are (b219 5G pull 8: three melee
                        // 58 s at 14 yd, every spot "an idle mob's aggro reach", no position to tell why)
                        bool const tankInReach = mob && IdleMobReachesSpot(bot, mob, tank->GetPosition());
                        Decisions().Write(run, "leader", "spot_rejected_aggro", member, bot->GetName(), false,
                                          JsonObject()
                                              .Str("member", bot->GetName())
                                              .Str("why", why)
                                              .Str("reject", reject)
                                              .Str("mob", mob ? mob->GetName() : "-")
                                              .Real("distance", mob ? mob->GetExactDist(spot) : -1.0f)
                                              .Real("aggroRange", mob ? mob->GetAggroRange(bot) : -1.0f)
                                              .Real("margin", Leader::LEADER_SPOT_AGGRO_MARGIN)
                                              .Num("idleLow", mob ? int64(mob->GetGUID().GetCounter()) : 0)
                                              .Real("mobX", mob ? mob->GetPositionX() : 0.0f)
                                              .Real("mobY", mob ? mob->GetPositionY() : 0.0f)
                                              .Real("mobZ", mob ? mob->GetPositionZ() : 0.0f)
                                              .Real("x", spot.GetPositionX())
                                              .Real("y", spot.GetPositionY())
                                              .Real("z", spot.GetPositionZ())
                                              .Real("tankGap", mob ? tank->GetExactDist(mob) : -1.0f)
                                              .Bool("tankInReach", tankInReach));
                    }
                    judgeBlockingPack(bot, mob);  // [lead76]
                    // [lead99] b138 rfc-h 339-427 s: the engage's range / meeting spots on the ledge had no walk
                    // ("no walk to the spot"), the members stayed and blocked the group - the order stood open and
                    // nobody could execute it. A walk-along spot with no walk at all (the route search found none) is
                    // no spot: the member follows the tank on the tank's own way (a fact, not a wait)
                    if (noPath && member != tank->GetGUID() && why.rfind("along with the tank", 0) == 0)
                    {
                        if (followTank(bot, 0, "along with the tank"))
                            Decisions().Write(run, "leader", "spot_no_walk_follow", member, bot->GetName(),
                                                          true,
                                                          JsonObject().Str("member", bot->GetName()).Str("why", why));
                        return false;
                    }
                    // [lead77] the group moves as one: a member's walk with the tank refused (an idle mob's reach on
                    // its path, no walk) out of a fight blocks the group - the tank stays until it is clear, the mob
                    // in the way is the next pull
                    if (!group.FightOn(tank) && member != tank->GetGUID() &&
                        why.rfind("along with the tank", 0) == 0)
                    {
                        if (!lead.groupBlockedBy.count(member))
                            Decisions().Write(run, "leader", "group_blocked", member, bot->GetName(), true,
                                                          JsonObject().Str("member", bot->GetName()).Str("why", reject)
                                                              .Str("mob", mob ? mob->GetName() : "-")
                                                              .Str("walk", why));
                        lead.groupBlockedBy[member] = mob ? mob->GetGUID() : ObjectGuid::Empty;
                    }
                    // [lead71] the arbiter's next candidate: the member stays where it is (never a move it refuses)
                    // [lead730] logged as its command (it was the member's live command with no leader_event)
                    if (move != Leader::MoveEvent::StandAt || spot.GetExactDist(bot->GetPosition()) > 1.0f)
                        if (std::string const stay = why + ": stays (" + reason + ")";
                            Leader::Events::SetMove(member, Leader::MoveEvent::StandAt, bot->GetPosition(), 2.0f,
                                                    stay))
                            logStay(bot, stay);
                    return false;
                }
            }
            // [lead27] a new command overrides the old one: command_override (old -> new) when its purpose changes or
            // the point jumps (the member's executor reads only the current command, its walk is replaced)
            Leader::MemberEvents before;
            bool const hadMove = Leader::Events::Get(member, before) && before.move != Leader::MoveEvent::None;
            bool const changed = Leader::Events::SetMove(member, move, spot, tolerance, why);
            if (changed)
                Leader::Events::StampPhase(member, lead.movePhase);  // [lead74] the purpose's phase
            // [lead77] its walk is clear again - [lead81] a real walk, not its own stay where it stands
            if (bot && (move != Leader::MoveEvent::StandAt || spot.GetExactDist(bot->GetPosition()) > tolerance))
            {
                lead.groupBlockedBy.erase(member);
                lead.spotRejectLogged.erase(member);
            }
            // [lead55] command_repeat: the new command is the one before the current (a ping-pong) - logged once per
            // member and purpose, counted (should be 0)
            if (changed && hadMove && bot)
            {
                auto const prev = lead.prevMoves.find(member);
                // [lead375] the member's Attack target the command before the current one was left under
                thread_local std::map<ObjectGuid, ObjectGuid> prevMoveAttack;
                if (prevMoveAttack.size() > 4096)
                    prevMoveAttack.clear();
                ObjectGuid const attackNow =
                    before.attack == Leader::AttackEvent::Attack ? before.target : ObjectGuid::Empty;
                auto const ruleOf = [](std::string const& text) { return text.substr(0, text.find(':')); };
                // [lead64] A -> B -> A after a higher-precedence B ended (a run to the tank done) is no ping-pong
                // [lead65] ... and only within one phase (b110: engage -> follow -> a NEW pull's engage was flagged)
                // [lead375] b197b rfc-h: 3 "combat spot" was "trap weave" - the weave ended on its own fact (its target
                // died, trap_weave_end) and the hunter went back to its spot; 2 "combat spot" was "combat spot" - the
                // mage went back to a spot it had left, filed under the held spot's old target (fixed below). Since
                // lead320 a different rule's live command is never replaced at equal rank (leader_contradiction logs
                // the tries): a repeat is one rule reversing itself under the same facts - the same phase and the same
                // Attack target (a spot left for one target may fit the next)
                if (prev != lead.prevMoves.end() && prev->second.second == why &&
                    lead.prevMovePhase[member] == lead.movePhase &&
                    prev->second.first.GetExactDist(spot) <= std::max(tolerance, 1.0f) &&
                    Leader::Events::MovePrecedence(before.moveWhy) <= Leader::Events::MovePrecedence(why) &&
                    ruleOf(before.moveWhy) == ruleOf(why) && prevMoveAttack[member] == attackNow)
                {
                    uint32& repeats = lead.commandRepeats[bot->GetName() + "|" + why];
                    if (!repeats++)
                        Decisions().Write(run, "leader", "command_repeat", member, bot->GetName(), true,
                                                      JsonObject().Str("member", bot->GetName()).Str("why", why)
                                                          .Str("was", before.moveWhy));
                }
                lead.prevMoves[member] = {before.spot, before.moveWhy};
                lead.prevMovePhase[member] = lead.movePhase;
                prevMoveAttack[member] = attackNow;
            }
            if (changed && hadMove && bot &&
                (before.moveWhy != why || before.spot.GetExactDist(spot) > 8.0f))
                Decisions().Write(run, "leader", "command_override", member, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("old", before.moveWhy)
                                                  .Str("new", why).Real("jump", before.spot.GetExactDist(spot)));
            if (changed && bot && (!hadMove || before.moveWhy != why))
                Decisions().Write(run, "leader", "position_owner", member, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("owner", why)
                                                  .Num("precedence", Leader::Events::MovePrecedence(why))
                                                  .Str("was", hadMove ? before.moveWhy : std::string("-")));
            if (changed && bot && move != Leader::MoveEvent::Hold)
                Decisions().Write(run, "leader", "leader_spot", member, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("why", why)
                                                  .Str("ground", LeaderOnCleared(spot) ? "cleared" : "new"));
            return changed;
        };
        // [lead66] the meeting-point move's stay (moveOrStay) went with lead730's wait point: a refused wait is the
        // walk along with the tank
        // [lead83] b126 rfc-h 598.5-603.7 s: Rtshaab's "to the meeting point" was re-derived from where it stood every
        // tick (LeaderMeleeWait) and flipped between two points (command_repeat). Like the range spot (lead68): the
        // melee place is computed once and kept until a fact changes - [lead730] meleePullWait (per order) and
        // meleeStopAt (per target at the tank's opener) below
        // a slow-layer call, timed
        auto const slow = [&](auto&& call, std::source_location where = std::source_location::current())
        {
            auto const start = std::chrono::steady_clock::now();
            auto result = call();
            uint32 const us = uint32(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - start).count());
            lead.slowUsTick += us;
            // [lead76] the slowest slow-layer call this minute (its call site's line), logged once a minute
            if (us > lead.slowestPassUs)
            {
                lead.slowestPassUs = us;
                lead.slowestPass = "TestHarnessRunner.cpp:" + std::to_string(where.line());
            }
            return result;
        };
        // [lead730] user 2026-11-03 (top priority: the damage dealers hit within a couple of seconds of the tank): the
        // last point of the member's own walk to `goal` (the route the leader judges its walks by, lead99) before the
        // walk enters an unpulled mob's reach (LeaderAggroAt: the game's aggro radius by the level difference plus
        // the spot guard's margin; the pull's own mobs are unpulled for a member until the tank has them) - the
        // closest place to the goal it reaches without waking anything. None while it stands in such a reach itself
        // (no safe walk to judge from)
        auto const lastSafeOnWalk = [&](Player* bot, Position const& goal) -> std::optional<Position>
        {
            Position last = bot->GetPosition();
            if (LeaderAggroAt(bot, last))
                return std::nullopt;
            Leader::RouteResult const route = Leader::Route(bot, goal);
            std::vector<G3D::Vector3> points;
            if (route.found && !route.points.empty())
                points = route.points;
            else
                points.emplace_back(goal.GetPositionX(), goal.GetPositionY(), goal.GetPositionZ());
            G3D::Vector3 from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            constexpr float STEP_YD = 1.0f;
            for (G3D::Vector3 const& to : points)
            {
                float const length = (to - from).length();
                for (float at = std::min(STEP_YD, length); length > 0.0f; at = std::min(at + STEP_YD, length))
                {
                    G3D::Vector3 const point = from + (to - from) * (at / length);
                    Position const sample(point.x, point.y, point.z);
                    if (LeaderAggroAt(bot, sample))
                        return last;
                    last = sample;
                    if (at >= length)
                        break;
                }
                from = to;
            }
            return last;
        };
        // [lead730] before the pull (the order issued, the tank on its way) a melee member waits at the closest safe
        // point to the pull's stop point (the leader's meeting point: the engaged mob, the fight spot of a pull back
        // / LoS / range pull), not on a Follow ~21 yd behind the tank (b219 rfc-h: 116 of 376 melee first hits came
        // from that Follow, 614 s after the tanks' hits). Computed once per order (the stop point), kept until a new
        // one
        auto const meleePullWait = [&](Player* bot) -> std::optional<Position>
        {
            auto& kept = lead.meleeWait[bot->GetGUID()];
            // [lead740] an engage the tank walks back to a fight spot (FightSpot::Ahead: b220 5B 794 s, the tank met
            // the Enforcer 20 yd out and walked it back to where it had stood): the stop point is that spot
            Position goal = lead.meetPoint;
            Position back;
            if (lead.commit && lead.commit->kind == "engage" && FightSpot::Ahead(tank, back))
                goal = back;
            if (kept.target != lead.commitMob || kept.meet.GetExactDist(goal) > 1.0f)
            {
                std::optional<Position> const point = slow([&] { return lastSafeOnWalk(bot, goal); });
                if (!point)
                    return std::nullopt;  // judged again next tick, from where it walked
                kept = {lead.commitMob, goal, *point};
            }
            return kept.point;
        };
        // [lead730] at the tank's opener the pulled mob runs to its victim: where they meet is the fight's stop point
        // - the victim's place when it stands (a pull back / LoS / range pull: the tank waits at its fight spot), else
        // the point on their line where both arrive at once (an engage: the tank walks in at its run speed, the mob
        // runs at its own, both stop at their combat reaches). The melee member's place there is its meeting place
        // (LeaderMeleeWait), its walk the last safe point toward it. Decided once per member and target at the hit
        // (the fact it is known from), kept while that mob is its target
        // [lead740] user 2026-11-03 (top priority, third attempt): b220 rfc-h (8 runs) melee walked ~1440 s of the
        // fights, 987 s of it in the pull's first seconds, and its path in a pull was the tank's (5000 yd after the
        // tank's first hit) plus its own walk in: the tank moves the fight and the melee chased the mob it walks. A
        // mob on the tank stops where the tank stops: the tank's walk's end when it walks away from the mob (the
        // pull back / LoS pull's return, "move fight spot back", a fight drag or a hazard move - the spline's final
        // point, the game's own), not where the mob is now. A walk that ends at the mob (reach melee, a charge, a
        // chase) is no walk away: the fight stands at the mob (the game's melee range of the two)
        auto const tankWalksMobTo = [&](Unit* target, Position& stop) -> bool
        {
            if (!tank->isMoving() || tank->movespline->Finalized())
                return false;
            G3D::Vector3 const end = tank->movespline->FinalDestination();
            if (target->GetExactDist2d(end.x, end.y) <= tank->GetMeleeRange(target))
                return false;
            stop = Position(end.x, end.y, end.z);
            return true;
        };
        // [lead740] where the fight with `target` (a mob on the tank) will stand when it is not where the tank stands
        // now: the tank's walk away from the mob (tankWalksMobTo), the fight spot the tank walks the fight back to
        // (FightSpot::Ahead: the engage's planned walk back, b220: 44 pulls, melee 2401 yd walked for 1064 yd net),
        // or a pull back / LoS / range pull's fight spot (the leader's meeting point: the tank tags the mob from its
        // cast spot and returns there - b220 5G 720.5 s: the three melee were sent to the tank's cast spot 40 yd out,
        // "victimWalks 0.0", and walked back with the mob). "" when none
        auto const fightStopAhead = [&](Unit* target, Position& stop) -> char const*
        {
            if (target->GetVictim() && target->GetVictim() != tank)
                return "";
            if (tankWalksMobTo(target, stop))
                return "the tank's walk";
            if (FightSpot::Ahead(tank, stop))
                return "the fight spot back";
            // ... while the tank goes back there: not once it stands holding the mob elsewhere (b220 5F 580 s: the
            // pull back's tank charged the Cultist 18 yd out and fought there)
            bool const holdsElsewhere = !tank->isMoving() && tank->IsWithinMeleeRange(target) &&
                                        tank->GetExactDist(lead.meetPoint) > tank->GetMeleeRange(target);
            if (lead.commit && lead.commitIssued && !lead.commitMob.IsEmpty() && !holdsElsewhere &&
                (lead.commit->kind == "pull back" || lead.commit->kind == "los pull" || lead.commit->kind == "one"))
            {
                std::vector<ObjectGuid> const pulled = LeaderPullMobs(lead.commit, lead.commitMob);
                if (target->GetGUID() == lead.commitMob ||
                    std::find(pulled.begin(), pulled.end(), target->GetGUID()) != pulled.end())
                {
                    stop = lead.meetPoint;
                    return "the pull's fight spot";
                }
            }
            return "";
        };
        auto const meleeStopAt = [&](Player* bot, Unit* target) -> Position
        {
            auto& kept = lead.meleeStop[bot->GetGUID()];
            // [lead740] a new fact - the fight's stop ahead moved by more than the MoveTo's own tolerance - decides
            // the place again
            Position ahead;
            std::string const aheadBy = fightStopAhead(target, ahead);
            if (kept.target == target->GetGUID() && (aheadBy.empty() || kept.meet.GetExactDist(ahead) <= 3.0f))
                return kept.point;
            Unit* victim = target->GetVictim() ? target->GetVictim() : static_cast<Unit*>(tank);
            float const gap = victim->GetExactDist2d(target);
            float const reaches = victim->GetCombatReach() + target->GetCombatReach();
            float const mobSpeed = target->GetSpeed(MOVE_RUN);
            float const victimSpeed =
                victim == tank && lead.commit && lead.commit->kind == "engage" ? tank->GetSpeed(MOVE_RUN) : 0.0f;
            float const along =
                mobSpeed + victimSpeed > 0.0f ? std::max(0.0f, gap - reaches) * victimSpeed / (mobSpeed + victimSpeed)
                                              : 0.0f;
            float x = victim->GetPositionX();
            float y = victim->GetPositionY();
            float z = victim->GetPositionZ();
            if (gap > 0.1f && along > 0.0f)
            {
                x += (target->GetPositionX() - victim->GetPositionX()) / gap * along;
                y += (target->GetPositionY() - victim->GetPositionY()) / gap * along;
                if (!map->CheckCollisionAndGetValidCoords(victim, victim->GetPositionX(), victim->GetPositionY(),
                                                          victim->GetPositionZ(), x, y, z))
                {
                    x = victim->GetPositionX();
                    y = victim->GetPositionY();
                    z = victim->GetPositionZ();
                }
            }
            Position const stop = aheadBy.empty() ? Position(x, y, z) : ahead;
            Position const place = LeaderMeleeWait(bot, stop);
            kept = {target->GetGUID(), stop, slow([&] { return lastSafeOnWalk(bot, place); }).value_or(place)};
            Decisions().Write(run, "leader", "melee_stop_point", bot->GetGUID(), bot->GetName(), true,
                              JsonObject()
                                  .Str("member", bot->GetName())
                                  .Str("target", target->GetName())
                                  .Str("victim", victim->GetName())
                                  .Str("stopBy", aheadBy.empty() ? "the mob meets its victim" : aheadBy)
                                  .Real("gap", gap)
                                  .Real("victimWalks", aheadBy.empty() ? along : victim->GetExactDist(stop))
                                  .Real("x", kept.point.GetPositionX())
                                  .Real("y", kept.point.GetPositionY())
                                  .Real("z", kept.point.GetPositionZ())
                                  .Real("short", kept.point.GetExactDist(place)));
            return kept.point;
        };
        if (!lead.slowestPassLogMs)
            lead.slowestPassLogMs = now;
        else if (getMSTimeDiff(lead.slowestPassLogMs, now) >= MINUTE * IN_MILLISECONDS && lead.slowestPassUs)
        {
            Decisions().Write(run, "leader", "leader_cadence", tank->GetGUID(), tank->GetName(), false,
                                          JsonObject().Str("slowestPass", lead.slowestPass)
                                              .Num("slowestUs", lead.slowestPassUs));
            lead.slowestPassUs = 0;
            lead.slowestPassLogMs = now;
        }
        auto const wake = [&](Player* bot)
        {
            if (!Leader::LEADER_FAST_REACT)
                return;
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                botAI->SetNextCheckDelay(0);  // the event is executed on its next update, not 1-3 s later
                ++lead.wakeCount;
            }
        };
        auto const logEvent = [&](Player* bot, char const* kind, JsonObject fields)
        {
            ++lead.leaderEventCounts[bot->GetName()];  // [lead27] leader_event per member (run summary)
            fields.Str("member", bot->GetName()).Str("event", kind);
            Decisions().Write(run, "leader", "leader_event", bot->GetGUID(), bot->GetName(), true,
                                          fields);
            wake(bot);
        };
        // where a member will stand: its fresh MoveTo / StandAt spot, else where it is
        auto const plannedAt = [](Player* bot)
        {
            Leader::MemberEvents events;
            if (Leader::Events::Get(bot->GetGUID(), events) &&
                (events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt))
                return events.spot;
            return bot->GetPosition();
        };

        // the tank stands (the group is held): the members prepare
        if (!lead.tankStillSinceMs || tank->GetExactDist(lead.tankStillPos) > 1.0f)
        {
            lead.tankStillPos = tank->GetPosition();
            lead.tankStillSinceMs = now;
        }
        bool const tankStill = getMSTimeDiff(lead.tankStillSinceMs, now) >= LEADER_TANK_STILL_MS;

        std::vector<Player*> seats;
        Player* healer = nullptr;
        bool anySitting = false;
        bool anyDead = false;
        bool rebuff = false;
        bool fighting = group.FightOn(tank);  // [lead84] the one definition (Steer reads the same)
        for (ObjectGuid const& groupMember : group.MemberGuids())
        {
            Player* bot = ObjectAccessor::FindPlayer(groupMember);
            if (!bot || bot == tank || bot->GetMap() != map)
                continue;
            if (!bot->IsAlive())
            {
                anyDead = true;
                continue;
            }
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                continue;
            if (!healer && PlayerbotAI::IsHeal(bot))
                healer = bot;
            anySitting |= bot->IsSitState();
            rebuff |= botAI->forceRebuff.IsPending();
            // [lead37] the boss prep per member: its rebuff window, what the leader commands it, what it does
            {
                std::string prep;
                if (botAI->forceRebuff.IsPending())
                {
                    Leader::MemberEvents prepEvents;
                    Leader::Events::Get(bot->GetGUID(), prepEvents);
                    Spell const* casting = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    std::string const cast = casting && casting->GetSpellInfo()
                                                 ? "casting " + std::string(casting->GetSpellInfo()->SpellName[0])
                                                 : std::string("no cast");
                    prep = Acore::StringFormat("rebuff window open; ready {} move {}; {}{}", int32(prepEvents.ready),
                                               int32(prepEvents.move), cast,
                                               botAI->forceRebuff.IsOnGlobalCooldown() ? ", on the global cooldown"
                                                                                       : "");
                }
                std::string& logged = lead.bossPrepLogged[bot->GetGUID()];
                if (prep != logged)
                {
                    if (!prep.empty() || !logged.empty())
                        Decisions().Write(
                            run, "leader", "boss_prep", bot->GetGUID(), bot->GetName(), false,
                            JsonObject().Str("member", bot->GetName())
                                .Str("state", prep.empty() ? "rebuff window closed" : prep)
                                .Real("openSec", prep.empty() ? 0.0f
                                                              : getMSTimeDiff(botAI->forceRebuff.BeganMs(), now) /
                                                                    1000.0f));
                    logged = prep;
                }
            }
            fighting |= bot->IsInCombat();
            seats.push_back(bot);
        }
        // the healer first: the others' moves keep the reach to where the healer will stand
        std::stable_partition(seats.begin(), seats.end(), [healer](Player* bot) { return bot == healer; });
        // [lead27] this tick asserts every command it wants; what it does not assert ends with the tick (no TTL)
        for (Player* bot : seats)
            Leader::Events::BeginTick(bot->GetGUID());
        Leader::Events::BeginTick(tank->GetGUID());
        // [lead320] the fact the commands are judged in (the move phase; set again once this tick's is known)
        for (Player* bot : seats)
            Leader::Events::SetFact(bot->GetGUID(), lead.movePhase);
        Leader::Events::SetFact(tank->GetGUID(), lead.movePhase);
        // [lead372] user: the leader says where, the member walks there safely and reports what it cannot reach. A
        // reported MoveTo / StandAt is the spot verdict checkedMove keeps until a fact changes (the member, the spot
        // or the mob its walk would wake moved): the leader's own fallback (the member stays) or another spot. A
        // reported follow place: the follow rule below holds the member where it stands while the facts stand
        {
            std::vector<Player*> reporters = seats;
            reporters.push_back(tank);
            thread_local std::map<ObjectGuid, std::string> refusalLogged;
            if (refusalLogged.size() > 4096)
                refusalLogged.clear();
            for (Player* bot : reporters)
            {
                Leader::MoveRefusal refusal;
                if (!Leader::Events::TakeMoveRefused(bot->GetGUID(), refusal))
                    continue;
                if (refusal.move == Leader::MoveEvent::MoveTo || refusal.move == Leader::MoveEvent::StandAt)
                {
                    auto& verdict = lead.spotChecked[bot->GetGUID()];
                    verdict = {};
                    verdict.spot = refusal.spot;
                    verdict.from = refusal.from;
                    verdict.ms = now;
                    verdict.reject = "the member's walk refused: " + refusal.reason;
                    verdict.mob = refusal.mob;
                    verdict.mobAt = refusal.mobAt;
                }
                std::string& logged = refusalLogged[bot->GetGUID()];
                if (logged == refusal.why + "|" + refusal.reason)
                    continue;
                logged = refusal.why + "|" + refusal.reason;
                Creature* woken = refusal.mob.IsEmpty() ? nullptr : map->GetCreature(refusal.mob);
                Decisions().Write(run, "leader", "member_move_refused", bot->GetGUID(), bot->GetName(), true,
                                  JsonObject()
                                      .Str("member", bot->GetName())
                                      .Str("why", refusal.why)
                                      .Str("reason", refusal.reason)
                                      .Str("mob", woken ? woken->GetName() : "-")
                                      .Real("x", refusal.spot.GetPositionX())
                                      .Real("y", refusal.spot.GetPositionY())
                                      .Real("z", refusal.spot.GetPositionZ()));
            }
        }
        struct TickEnd
        {
            std::vector<Player*> const& seats;
            Player* tank;
            uint32 const& phase;
            ~TickEnd()
            {
                // [lead74] the stored position decisions stand until they arrive (generalised lead72)
                for (Player* bot : seats)
                {
                    Leader::Events::HoldMove(bot->GetGUID(), bot->GetPosition(), phase);
                    Leader::Events::EndTick(bot->GetGUID());
                }
                Leader::Events::HoldMove(tank->GetGUID(), tank->GetPosition(), phase);
                Leader::Events::EndTick(tank->GetGUID());
            }
        } tickEnd{seats, tank, lead.movePhase};
        for (ObjectGuid const& groupMember : group.MemberGuids())
            if (Player* dead = ObjectAccessor::FindPlayer(groupMember); dead && !dead->IsAlive())
                Leader::Events::Clear(groupMember);

        Leader::PullOrder order;
        bool const orderOpen = Leader::Orders::GetOpen(group.LeaderGuid(), order);
        constexpr uint32 LEADER_SPOTS_AFTER_ISSUE_MS = 6 * IN_MILLISECONDS;  // the tank's commit comes after
        bool const spotsHeld = Leader::LEADER_ASSIGNS_SPOTS && !lead.spots.empty() && !group.InCombat() &&
                               (lead.spotsWaiting || (orderOpen && order.target == lead.spotsFor) ||
                                (lead.spotsIssuedMs &&
                                 getMSTimeDiff(lead.spotsIssuedMs, now) < LEADER_SPOTS_AFTER_ISSUE_MS));
        if (!spotsHeld && !lead.spots.empty())
        {
            lead.spots.clear();
            lead.spotsFor.Clear();
        }
        // [lead17] an order for another mob runs (the patrol path issued it): the spots of the old pull go, its
        // gate too
        if (orderOpen && !lead.spotsFor.IsEmpty() && order.target != lead.spotsFor &&
            !(lead.commit && lead.commitIssued))
        {
            lead.spots.clear();
            lead.spotsFor.Clear();
            lead.spotsWaiting = false;
        }

        // the fight's target: the tank's, else whatever attacks a member (healer first) or the tank
        // [lead42] b91 rfc-h 203-245 s: every member's Attack on one Trogg was refused for 40 s and the leader kept it.
        // A target the members refused is replaced by another attacker nobody refused (if one fights the group)
        if (!fighting)
            lead.attackRefused.clear();
        auto const refusedTarget = [&](Unit* unit) { return unit && lead.attackRefused.count(unit->GetGUID()) > 0; };
        Unit* target = nullptr;
        if (fighting)
        {
            if (Unit* victim = tank->GetVictim();
                victim && victim->IsAlive() && victim->IsInCombat() && victim->GetMap() == map)
                target = victim;
            std::vector<Player*> attacked = seats;
            attacked.push_back(tank);
            if (refusedTarget(target))
                target = nullptr;
            Unit* refusedFallback = nullptr;
            std::string attackersSig;
            JsonArray attackerList;
            for (Player* bot : attacked)
            {
                for (Unit* attacker : bot->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker->IsInWorld() && attacker->GetMap() == map &&
                        bot->IsValidAttackTarget(attacker) && LeaderFightMob(tank, attacker))
                    {
                        Creature const* creature = attacker->ToCreature();
                        bool const evades = creature && creature->IsInEvadeMode();
                        attackersSig += std::to_string(attacker->GetGUID().GetCounter()) + (evades ? "e" : "") +
                                        (refusedTarget(attacker) ? "r" : "") + ",";
                        attackerList.Raw(JsonObject().Str("name", attacker->GetName())
                                             .Num("entry", attacker->GetEntry())
                                             .Num("low", attacker->GetGUID().GetCounter())
                                             .Str("on", bot->GetName())
                                             .Real("tankDistance", tank->GetExactDist(attacker))
                                             .Real("dz", attacker->GetPositionZ() - tank->GetPositionZ())
                                             .Bool("evades", evades).Bool("tankSees", tank->IsWithinLOSInMap(attacker))
                                             .Num("refusedBy", RefusedCount(lead.attackRefused, attacker))
                                             .Build());
                        if (target)
                            continue;
                        if (refusedTarget(attacker))
                        {
                            if (!refusedFallback)
                                refusedFallback = attacker;
                            continue;
                        }
                        target = attacker;
                    }
            }
            // every attacker refused: the refused one stays the target (the fight always has one, lead38)
            if (!target)
                target = refusedFallback;
            // [lead42] the attacker list on every change (the next stall is visible)
            if (attackersSig != lead.fightStateSig)
            {
                lead.fightStateSig = attackersSig;
                Decisions().Write(run, "leader", "fight_attackers",
                                              target ? target->GetGUID() : ObjectGuid(),
                                              target ? target->GetName() : std::string("-"), true,
                                              JsonObject().Raw("attackers", attackerList.Build())
                                                  .Bool("targetRefused", refusedTarget(target)));
            }
        }

        // the fight's rules around the target: the kill order (the target, then the rest of what fights the group,
        // nearest to the target first), the AoE rule, the intensity, the casting mobs
        std::vector<ObjectGuid> killOrder;
        std::vector<Unit*> casters;
        Leader::AoeRule aoe = Leader::AoeRule::Forbidden;
        std::string aoeWhy;
        Leader::Intensity intensity = Leader::Intensity::Normal;
        if (target)
        {
            std::vector<Unit*> fightingMobs;
            std::set<ObjectGuid> seen;
            std::vector<Player*> attacked = seats;
            attacked.push_back(tank);
            for (Player* bot : attacked)
                for (Unit* attacker : bot->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker->GetMap() == map &&
                        LeaderFightMob(tank, attacker) &&
                        seen.insert(attacker->GetGUID()).second)
                        fightingMobs.push_back(attacker);
            if (seen.insert(target->GetGUID()).second)
                fightingMobs.push_back(target);
            std::sort(fightingMobs.begin(), fightingMobs.end(),
                      [target](Unit* a, Unit* b)
                      {
                          if ((a == target) != (b == target))
                              return a == target;
                          return target->GetExactDist(a) < target->GetExactDist(b);
                      });
            for (Unit* mob : fightingMobs)
            {
                killOrder.push_back(mob->GetGUID());
                if (Spell* spell = mob->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    spell && spell->GetSpellInfo() &&
                    (spell->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT))
                    casters.push_back(mob);
            }
            if (lead.aoeTarget != target->GetGUID() || !lead.aoeMs ||
                getMSTimeDiff(lead.aoeMs, now) >= LEADER_AOE_RECHECK_MS)
            {
                // [lead415] every member's AoE footprint around the target: its centre at the target, or at the member
                // for a self-centred one (at most the farthest member's distance away) - plus [lead420] the largest
                // area radius among the members' spells (spell data: Starfall's 30 yd, not the rule's 10)
                float farthest = tank->GetExactDist(target);
                float areaRadius = LeaderMaxAreaRadius(tank);
                for (Player* bot : seats)
                {
                    farthest = std::max(farthest, bot->GetExactDist(target));
                    areaRadius = std::max(areaRadius, LeaderMaxAreaRadius(bot));
                }
                float const range = farthest + std::max(areaRadius, LEADER_AOE_RADIUS) + LEADER_AOE_GUARD_YD;
                lead.aoeRule = uint8(slow([&]
                                          { return LeaderAoeRule(tank, target, range, lead.aoeWhy, lead.aoeSpared); }));
                lead.aoeTarget = target->GetGUID();
                lead.aoeMs = now;
            }
            aoe = Leader::AoeRule(lead.aoeRule);
            aoeWhy = lead.aoeWhy;
            if (Creature* creature = target->ToCreature(); creature && creature->IsDungeonBoss())
                intensity = Leader::Intensity::Burn;
            // [lead80] the fight's expected seconds (its mobs' health over the estimated group dps) and the walk to
            // the nearest standing boss (its distance over the tank's run speed): what each member's burn is judged by
            float health = 0.0f;
            for (Unit* mob : fightingMobs)
            {
                health += float(mob->GetHealth());
                lead.fightMobHealth[mob->GetGUID()] = float(mob->GetMaxHealth());  // [lead513]
            }
            PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
            float const groupDps =
                tankAI ? tankAI->GetAiObjectContext()->GetValue<float>("estimated group dps")->Get() : 0.0f;
            lead.fightSecLeft = health / std::max(groupDps, 1.0f);
            lead.groupDps = groupDps;  // [lead700]
            float bossYd = -1.0f;
            Creature* nextBoss = nullptr;
            for (ObjectGuid const& bossGuid : group.BossCreatures())
                if (Creature* boss = map->GetCreature(bossGuid); boss && boss->IsAlive() && !boss->IsInCombat())
                    if (bossYd < 0.0f || tank->GetExactDist(boss) < bossYd)
                    {
                        bossYd = tank->GetExactDist(boss);
                        nextBoss = boss;
                    }
            // [lead421] the next boss is the one the route's plan walks to (route_plan), the nearest only without one
            GroupRouteState const& route = group.Route();
            if (Creature* planned = route.routePlanBoss.IsEmpty() ? nullptr : map->GetCreature(route.routePlanBoss);
                planned && planned->IsAlive() && !planned->IsInCombat())
            {
                nextBoss = planned;
                bossYd = tank->GetExactDist(planned);
            }
            lead.nextBossGuid = nextBoss ? nextBoss->GetGUID() : ObjectGuid::Empty;
            // [lead421] a boss fight ends the run's walk to him: its seconds (the last boss fight's end, or the
            // run's first tick, to this pull) are a measured stretch between bosses
            // [lead631] b218 rfc-h 5C Oggleflint (pull 15, 267.7 s): the fight's target on its first tick was a
            // Ragefire Shaman of his pack, the once-per-fight judgement saw no boss and the stretch was never noted
            // (no stretch measured, bossFightOn false: the boss fight counted as trash, the walk to Taragaman ran from
            // the run's start); Hysteria / Force of Nature held for Taragaman "his pull in >= 101 s" (pulled 391 s
            // later), Army of the Dead / Empower Rune Weapon for Bazzalan ">= 219 s" (806 s later) - all back long
            // before. A dungeon boss in this fight on any tick (the target, or engaged by / on a member: lead541's
            // test) is the boss fight
            if (!lead.bossFightOn)
            {
                Creature const* inFight = target->ToCreature();
                if (!inFight || !inFight->IsDungeonBoss())
                {
                    inFight = nullptr;
                    for (ObjectGuid const& bossGuid : group.BossCreatures())
                    {
                        Creature* boss = map->GetCreature(bossGuid);
                        if (!boss || !boss->IsAlive() || !boss->IsInCombat() || !boss->IsDungeonBoss())
                            continue;
                        if (BossFightsGroup(boss, tank, seats))  // [lead772]
                        {
                            inFight = boss;
                            break;
                        }
                    }
                }
                if (inFight)
                {
                    // [lead541] b212 rfc-h: "his pull in 4294931 s" / "2147153 s" - on a fight's first tick
                    // fightStartMs is still 0 here (set further down the tick), getMSTimeDiff(stretchFromMs, 0)
                    // wrapped to ~2^32 ms into the stretches' mean. The pull's start is this tick then; a start before
                    // the walk began (a fight that outlived the boss fight's end) is no stretch
                    uint32 const pulledMs = lead.fightStartMs ? lead.fightStartMs : now;
                    if (lead.stretchFromMs && int32(pulledMs - lead.stretchFromMs) >= 0)
                    {
                        lead.stretchSecSum += (pulledMs - lead.stretchFromMs) / 1000.0f;
                        ++lead.stretches;
                    }
                    lead.stretchFromMs = 0;
                    lead.bossFightOn = true;
                }
            }
            // [lead274] user: a normal raid leader saves all cooldowns for the boss; on trash only at critical
            // moments, or if the boss is far and everything recharges; in a short dungeon only on bosses, and not on
            // all - on the most needed. Once per fight every living boss is judged like a pull (the planner's
            // estimate: JudgePack's overload of tank health / healer mana, and his length over the group's dps) - the
            // cooldowns go to the boss that needs them most - and the fight itself (a critical pull: overload >= 1)
            if (lead.bossJudgedFor != lead.fightStartMs)
            {
                lead.bossJudgedFor = lead.fightStartMs;
                std::map<ObjectGuid, DungeonLeaderState::BossNeed> needs;
                for (ObjectGuid const& bossGuid : group.BossCreatures())
                    if (Creature* boss = map->GetCreature(bossGuid); boss && boss->IsAlive())
                    {
                        DungeonLeaderState::BossNeed need;
                        // a boss already in this fight keeps the judgement from before his pull (his full health)
                        if (auto const known = lead.bossNeeds.find(bossGuid);
                            boss->IsInCombat() && known != lead.bossNeeds.end())
                            need = known->second;
                        else
                        {
                            PullPlanner::JudgePack(tank, {bossGuid}, need.overload, need.judged);
                            need.fightSec = float(boss->GetMaxHealth()) / std::max(groupDps, 1.0f);
                            need.name = boss->GetName();
                        }
                        needs[bossGuid] = need;
                    }
                lead.bossNeeds = needs;
                std::vector<ObjectGuid> fightGuids;
                for (Unit* mob : fightingMobs)
                    fightGuids.push_back(mob->GetGUID());
                lead.fightOverload = 0.0f;
                lead.fightJudged.clear();
                PullPlanner::JudgePack(tank, fightGuids, lead.fightOverload, lead.fightJudged);
            }
            lead.nextBossName = nextBoss ? nextBoss->GetName() : std::string();
            lead.bossWalkSec = bossYd < 0.0f ? -1.0f : bossYd / std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
            // [lead178] the time to the next boss pull: what is left of this fight, the fights of the idle mobs the
            // leader holds on the path (their health over the group's dps) and the walk; no boss left: -1
            // [lead421] b201 rfc-h 5B: Shadowfiend was held on Oggleflint for Jergosh "his pull in >= 130 s" (the
            // fight left + the straight line) - he was pulled 667 s later. Along the route's plan now: its stops'
            // fights (their mobs' health over the group's dps), the rest they cost (each mana user's line for those
            // fight seconds over its best drink's rate; the group waits for the slowest drinker) and the walk from
            // stop to stop to the boss; without a plan to him the on-path mobs and the straight walk, as before
            lead.bossPullSec = -1.0f;
            lead.bossPullPartsSec = -1.0f;
            lead.bossPullParts.clear();
            if (nextBoss)
            {
                float const speed = std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
                float trashHealth = 0.0f;
                float walkYd = 0.0f;
                // [lead632] b218 rfc-h 5G 714 s: the route's plan to Jergosh had no stop left ("stops":0, 52.5 yd)
                // and read as no plan: the stretch floor (lead513) made his pull "110 s" at 722.7 s and "76 s" at
                // 756.8 s - he was pulled at 758 s; the blood DK tank's Dancing Rune Weapon (90 s) went on that trash
                // as "back before the pull". A plan the route made to him (routePlanBoss) with no stop left is the way
                // to him with no trash on it: its walk alone
                bool const planToHim = route.routePlanBoss == nextBoss->GetGUID();
                if (planToHim)
                {
                    Position at = tank->GetPosition();
                    for (GroupRouteState::PlannedStop const& stop : route.routePlan)
                    {
                        walkYd += at.GetExactDist(stop.spot);
                        at = stop.spot;
                        for (ObjectGuid const& mobGuid : stop.mobs)
                            if (Creature* mob = map->GetCreature(mobGuid); mob && mob->IsAlive() && !mob->IsInCombat())
                                trashHealth += float(mob->GetHealth());
                    }
                    walkYd += at.GetExactDist(nextBoss->GetPosition());
                }
                else
                {
                    for (auto const& [mobGuid, verdict] : group.PathVerdicts())
                        if (verdict.on)
                            if (Creature* mob = map->GetCreature(mobGuid); mob && mob->IsAlive() &&
                                                                          !mob->IsInCombat() && !mob->IsDungeonBoss() &&
                                                                          !mob->isWorldBoss())
                                trashHealth += float(mob->GetHealth());
                    walkYd = bossYd;
                }
                // [lead513] b210 rfc-h 5C 944 s: the plan to Bazzalan (5 stops, 232 yd) was estimated 86.6 s, his
                // pull came 148 s later - its 12 mobs fought 91 s, the estimate gave them about half (the expected
                // group dps: no run-in, no ramp, no switch). The trash seconds go by the run's measured trash fights
                // (the full health of the mobs they killed over their seconds) once there is one
                float const trashRate = lead.trashFightSec > 0.0f && lead.trashKilledHealth > 0.0f
                                            ? lead.trashKilledHealth / lead.trashFightSec
                                            : groupDps;
                float const trashSec = trashHealth / std::max(trashRate, 1.0f);
                float restSec = 0.0f;
                if (trashSec > 0.0f)
                {
                    std::vector<Player*> drinkers = seats;
                    drinkers.push_back(tank);
                    for (Player* member : drinkers)
                        if (member->IsAlive() && member->GetMaxPower(POWER_MANA))
                            if (float const drink = RestState::DrinkPctPerSecOf(member); drink > 0.0f)
                                restSec =
                                    std::max(restSec, RestState::FightManaLine(member, trashSec, 0.0f) / drink);
                }
                lead.bossWalkSec = walkYd / speed;
                lead.bossPullSec = lead.fightSecLeft + trashSec + restSec + lead.bossWalkSec;
                lead.bossPullPartsSec = lead.bossPullSec;  // [lead771]
                // [lead513] b210 rfc-h 5C 875.9 s: Starfall / Force of Nature held on Jergosh for Bazzalan "his pull
                // in >= 78 s" - he was pulled 217 s later. The route plans the way to the next boss only once the boss
                // fight under way ends (5C route_plan "first plan" to Bazzalan at 922 s, 7 stops 307 yd): until then
                // the estimate read the current path's on-path mobs and the straight line (no trash between the two
                // bosses). Without a plan to him the run's measured stretch between bosses (their mean, lead421) less
                // the part of it already walked is the floor of the estimate
                if (!planToHim && lead.stretches)
                {
                    float const walked =
                        lead.stretchFromMs ? getMSTimeDiff(lead.stretchFromMs, now) / 1000.0f : 0.0f;
                    float const stretchLeft =
                        std::max(0.0f, lead.stretchSecSum / float(lead.stretches) - walked);
                    lead.bossPullSec = std::max(lead.bossPullSec, lead.fightSecLeft + stretchLeft);
                    lead.bossPullParts = Acore::StringFormat("stretch floor {:.0f}; ", lead.fightSecLeft + stretchLeft);
                }
                // [lead724] b219 rfc-h (8 runs): 239 of 918 burn verdicts read the next boss's pull more than 10 s
                // later than it came (5A 15.9 s: 294 s, pulled 214 s later; 5F 461.5 s: 396 vs 263 s) - Bladestorm,
                // Gargoyle, Icy Veins "back before the pull" on trash that was not back. The estimate's parts are
                // logged with each verdict to find the part that overshoots
                lead.bossPullParts += Acore::StringFormat(
                    "fight {:.0f} + trash {:.0f} ({:.0f} hp at {:.0f}/s{}) + rest {:.0f} + walk {:.0f} ({:.0f} yd{})",
                    lead.fightSecLeft, trashSec, trashHealth, trashRate, lead.trashFightSec > 0.0f ? " measured" : "",
                    restSec, lead.bossWalkSec, walkYd, planToHim ? ", the route's plan" : ", straight");
            }
            // [lead990] the trash lines' own mana cooldowns read the same "back before the pull" as the verdict
            if (Group* tankGroup = tank->GetGroup())
                RestState::NoteBossPullSec(tankGroup->GetGUID(), lead.bossPullPartsSec >= 0.0f ? lead.bossPullPartsSec
                                                                                               : lead.bossPullSec);
        }

        // [lead20] the tank holds every fight mob near `to` and the walk from `from` (its victim is the tank, or it is
        // crowd-controlled)
        auto const tankHolds = [&](Position const& from, Position const& to)
        {
            Position const mid((from.GetPositionX() + to.GetPositionX()) / 2.0f,
                               (from.GetPositionY() + to.GetPositionY()) / 2.0f, to.GetPositionZ());
            for (ObjectGuid const guid : killOrder)
            {
                Unit* mob = ObjectAccessor::GetUnit(*tank, guid);
                if (!mob || !mob->IsAlive() || mob->GetVictim() == tank || mob->HasBreakableByDamageCrowdControlAura())
                    continue;
                if (mob->GetExactDist(to) <= 15.0f || mob->GetExactDist(mid) <= 15.0f)
                    return false;
            }
            return true;
        };
        Position healerAt = healer ? plannedAt(healer) : Position();
        // [lead950] the healer's eyes at `from`
        auto const reaches = [tank, healer](Position const& from, Position const& to)
        { return HealReaches(tank, from, to, healer); };
        // [lead120] where the tank fights: its fight anchor in a fight (moved on with it, LEADER_ANCHOR_MOVE_YD), else
        // where it stands - a heal-reach spot is judged against it, not against the tank's every step
        auto const tankFightAt = [&]()
        { return fighting && lead.fightAnchorSerial ? lead.fightAnchor : tank->GetPosition(); };
        // the healer at `spot` reaches every member (where it will stand) and the tank
        auto const healerKeeps = [&](Position const& spot)
        {
            // [lead18] the tank first: within LEADER_HEALER_TANK_YD of where it fights and in sight
            Position const tankAt = tankFightAt();
            if (!reaches(spot, tankAt) ||
                spot.GetExactDist(tankAt) > Leader::LEADER_HEALER_TANK_YD - g_spotAcquireMargin)
                return false;
            // [lead38] a held spot (no acquire margin) keeps a member it reaches where it stands or where it goes:
            // b87 rfc-h 23.6-24.0 s, 6 "heal reach" re-spots in 0.4 s while the counterparts' planned spots moved
            for (Player* other : seats)
                if (other != healer && !reaches(spot, plannedAt(other)) &&
                    !(g_spotAcquireMargin == 0.0f && reaches(spot, other->GetPosition())))
                    return false;
            return true;
        };
        // [lead490] b209 rfc-h: the healers' first combat spots were rejected "heal reach" on every candidate (near
        // x45-49, ring x5-13, walk x5-20; 5C 128.8 s Rtpriab) - healerKeeps asked the spot to reach where each member
        // stands while the members still followed the tank round the cave's corners: their fight places were not
        // given yet. A member's place is the leader's: one with its combat spot is judged at that spot (its
        // plannedAt), one still to be placed is not - its own spot search keeps the heal reach to the healer's place
        // (keepsHealer: the healer's planned spot, i.e. this one). The tank's fight place stays the first test
        auto const healerSpotKeeps = [&](Position const& spot)
        {
            Position const tankAt = tankFightAt();
            if (!reaches(spot, tankAt) ||
                spot.GetExactDist(tankAt) > Leader::LEADER_HEALER_TANK_YD - g_spotAcquireMargin)
                return false;
            for (Player* other : seats)
                if (other != healer && lead.combatSpots.count(other->GetGUID()) &&
                    !reaches(spot, plannedAt(other)) &&
                    !(g_spotAcquireMargin == 0.0f && reaches(spot, other->GetPosition())))
                    return false;
            return true;
        };
        // [lead463] b207 rfc-h: "heal reach" took 4-57 ring candidates of a combat spot search (the 39
        // combat_spot_none) - judged from where the healer stands now, while the healer is placed by the leader too
        // (its heal-reach spot). A spot keeps the heal reach when the healer's place reaches it, or a healer place
        // the leader can give exists: the line from the tank's fight place to the spot is in sight and short enough
        // that a point on it lies within LEADER_HEALER_TANK_YD of the tank and LEADER_HEAL_REACH_YD of the spot
        // [lead471] b208 rfc-h: "heal reach" still took 7-93 ring candidates of the combat_spot_none searches - the
        // healer place the leader can give was only a point on the straight line from the tank to the spot (its sight
        // needed). The healer places the leader can give are the healer's planned spot, where it stands, the tank's
        // fight place and the healer's band round it (LEADER_HEALER_YD, the 16 directions of the healer searches) on
        // the tank's floor in heal reach and sight of the tank: a spot keeps the heal reach when one of them reaches it
        std::optional<std::vector<Position>> healerPlaces;
        auto const healerCanReach = [&](Position const& spot)
        {
            Position const tankAt = fighting && lead.fightAnchorSerial ? lead.fightAnchor : tank->GetPosition();
            if (spot.GetExactDist(tankAt) >
                Leader::LEADER_HEALER_TANK_YD + Leader::LEADER_HEAL_REACH_YD - g_spotAcquireMargin)
                return false;
            if (!healerPlaces)
            {
                healerPlaces.emplace();
                healerPlaces->push_back(tankAt);
                constexpr uint32 DIRECTIONS = 16;
                for (uint32 i = 0; i < DIRECTIONS; ++i)
                {
                    float const angle = 2.0f * float(M_PI) * float(i) / float(DIRECTIONS);
                    float x = tankAt.GetPositionX() + std::cos(angle) * LEADER_HEALER_YD;
                    float y = tankAt.GetPositionY() + std::sin(angle) * LEADER_HEALER_YD;
                    float z = tankAt.GetPositionZ();
                    if (!map->CheckCollisionAndGetValidCoords(tank, tankAt.GetPositionX(), tankAt.GetPositionY(),
                                                              tankAt.GetPositionZ(), x, y, z) ||
                        std::fabs(z - tankAt.GetPositionZ()) > 4.0f)
                        continue;
                    Position const place(x, y, z);
                    if (place.GetExactDist(tankAt) <= Leader::LEADER_HEALER_TANK_YD && reaches(place, tankAt))
                        healerPlaces->push_back(place);
                }
            }
            for (Position const& place : *healerPlaces)
                if (reaches(place, spot))
                    return true;
            return false;
        };
        auto const keepsHealer = [&](Position const& spot)
        {
            return !Leader::LEADER_HEAL_REACH || !healer || reaches(healerAt, spot) ||
                   (g_spotAcquireMargin == 0.0f && reaches(healer->GetPosition(), spot)) || healerCanReach(spot);
        };

        // [lead15] the aggro-loss rule (user: «если кто-то кого-то сагрил - подбегай к танку, пусть танк АОЕ делает и
        // снимает агр с тебя»): a mob on a member (not the tank) - the victim runs next to the tank (in its taunt /
        // AoE reach, in heal reach) and stops its damage, the tank takes the mob back (a taunt when ready, else an
        // AoE threat spell, else it attacks it), the others leave the mob alone until the tank has it; the healer
        // keeps the victim up (Heal(group)). Logged: leader_aggro_loss -> the commands -> leader_aggro_recovered.
        // [lead200] b181 rfc-h 5G 144.4 / 197.8 / 712.5 s: a flee stop turned the fleer onto its stopper, and the fleer
        // became "loose" - DontAttack for everyone else on a mob the leader made the focus (Howling Blast held 55
        // times "the area holds the leader's DontAttack mob"). A fleer is the group's kill, not the tank's to take back
        std::map<ObjectGuid, Unit*> looseOn;  // victim -> the loose mob on it
        std::set<ObjectGuid> looseMobs;
        // [lead805] b221 rfc-h (8 runs): 9 harness aggro losses (39 s) had no leader_aggro_loss - 5H 808 s the cat with
        // a Searing Blade Warlock and a Voidwalker Minion on it (only the Voidwalker logged), 5E 625.9 s an Enforcer
        // that went from the priest to the warlock (logged "recovered" while it hit the warlock), 5F 822 s a Warlock
        // from the druid to the hunter, 5D 38 s a Molten Elemental from the mage to the DK. The scan kept one loose
        // mob per victim (a second on the same member was neither logged nor counted as loose: its episode "recovered"
        // the tick it lost its slot) and keyed an episode by the mob alone (a mob that turned to a second member was
        // the first episode still). Every loose mob is seen; a mob on a new victim is a new loss
        // the first-per-victim set (looseMobs) stays the commands' and the kill order's
        std::vector<std::pair<ObjectGuid, Unit*>> looseAll;  // [lead805] every (victim, loose mob)
        std::set<ObjectGuid> looseSeen;                       // [lead805] every loose mob, for the episodes
        if (fighting)
        {
            for (Player* bot : seats)
                for (Unit* attacker : bot->getAttackers())
                    // [lead217] a mob under crowd control (Polymorph) keeps its victim and hits nobody: no aggro loss,
                    // no walk to the tank (b184 rfc-h 5D 900-937 s: the DK sent to the tank for a sheeped Warlock)
                    if (attacker && attacker->IsAlive() && attacker->GetVictim() == bot && attacker->ToCreature() &&
                        LeaderFightMob(tank, attacker) && !Leader::Fight::IsFleer(attacker) &&
                        !IsCrowdControlled(attacker) && looseSeen.insert(attacker->GetGUID()).second)
                    {
                        if (looseOn.emplace(bot->GetGUID(), attacker).second)  // the first per victim: its commands
                            looseMobs.insert(attacker->GetGUID());
                        looseAll.emplace_back(bot->GetGUID(), attacker);
                    }
            for (auto const& [victimGuid, mob] : looseAll)
                if (auto const known = lead.looseMobs.find(mob->GetGUID());
                    known == lead.looseMobs.end() || known->second.first != victimGuid)
                {
                    bool const switched = known != lead.looseMobs.end();
                    bool const second = looseOn.at(victimGuid) != mob;
                    if (switched || second)
                    {
                        Player* const was = switched ? ObjectAccessor::FindPlayer(known->second.first) : nullptr;
                        Player* const onNow = ObjectAccessor::FindPlayer(victimGuid);
                        LOG_INFO("playerbots", "[lead805] aggro loss seen: {} on {}{}{}", mob->GetName(),
                                 onNow ? onNow->GetName() : "?",
                                 switched ? " (it left " + (was ? was->GetName() : std::string("?")) + ")" : "",
                                 second ? " (a second loose mob on it)" : "");
                        if (switched)  // [lead54] [lead56] the episode on the member before ended
                        {
                            lead.tauntLogged.erase(mob->GetGUID());
                            lead.threatDoneFor.erase(mob->GetGUID());
                        }
                    }
                    lead.looseMobs[mob->GetGUID()] = {victimGuid, now};
                    Player* victim = ObjectAccessor::FindPlayer(victimGuid);
                    // [lead472] the loss's cause from the facts of this moment: the victim's command and what it hits,
                    // its threat against the tank's on the mob, whether the tank has the mob on its threat list or
                    // attacks it, a heal (the victim a healer / casting a heal) or damage, the mob's kind (a summon,
                    // a caster) and whether the tank has a walk to the victim
                    JsonObject cause;
                    if (victim)
                    {
                        ThreatManager& threat = mob->GetThreatMgr();
                        float const victimThreat = threat.GetThreat(victim);
                        float const tankThreat = threat.GetThreat(tank);
                        bool const tankListed = threat.IsThreatenedBy(tank);
                        bool const tankHits = tank->GetVictim() == mob;
                        Leader::MemberEvents command;
                        bool const commanded = Leader::Events::Get(victimGuid, command);
                        Unit* const commandTarget = commanded && command.attack == Leader::AttackEvent::Attack
                                                        ? ObjectAccessor::GetUnit(*tank, command.target)
                                                        : nullptr;
                        bool const toldToHit = commandTarget == mob;
                        bool const hitsIt = victim->GetVictim() == mob;
                        // [lead571] b215: the "not told to" losses were mobs the member had been told seconds before;
                        // the seconds since its Attack left this mob (-1: it was not the last one it left)
                        float leftSec = -1.0f;
                        if (auto const left = lead.attackLeft.find(victimGuid);
                            !toldToHit && left != lead.attackLeft.end() && left->second.first == mob->GetGUID())
                            leftSec = float(getMSTimeDiff(left->second.second, now)) / 1000.0f;
                        Spell const* casting = victim->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                        if (!casting)
                            casting = victim->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                        SpellInfo const* castInfo = casting ? casting->GetSpellInfo() : nullptr;
                        bool const healing = castInfo && castInfo->IsPositive() &&
                                             (castInfo->HasEffect(SPELL_EFFECT_HEAL) ||
                                              castInfo->HasAura(SPELL_AURA_PERIODIC_HEAL));
                        bool const healer = PlayerbotAI::IsHeal(victim);
                        Creature* const creature = mob->ToCreature();
                        bool const summon = mob->IsSummon() || !mob->GetOwnerGUID().IsEmpty();
                        uint32 const unitClass = creature ? creature->GetCreatureTemplate()->unit_class : 0;
                        bool const caster = unitClass == CLASS_MAGE || unitClass == CLASS_PALADIN;
                        Leader::RouteResult const walk = Leader::Route(tank, victim->GetPosition());
                        bool const tankReaches = walk.found && walk.complete;
                        char const* why = "out-threatened the tank on its own target";
                        if (!tankListed)
                            why = summon ? "a summon the tank has no threat on"
                                         : "the tank never had it (not on its threat list)";
                        else if (tankThreat <= 0.0f)
                            why = "no tank threat on it";
                        else if (healer || healing)
                            why = "heal threat over the tank's";
                        else if (!toldToHit && !hitsIt)
                            why = "threat on a mob it was not told to hit (area / splash / dot)";
                        else if (!toldToHit && leftSec >= 0.0f)
                            why = "the leader moved its Attack off this mob, its switch still in flight";
                        else if (!toldToHit)
                            why = "it attacks a mob it was not told to";
                        cause.Real("attackLeftSec", leftSec);
                        cause.Str("cause", why)
                            .Str("victimCommand", commanded ? std::string(commandTarget ? "Attack " +
                                                                                              commandTarget->GetName()
                                                                                        : "no Attack")
                                                            : std::string("none"))
                            .Bool("victimHitsIt", hitsIt)
                            .Str("victimCasts", castInfo ? castInfo->SpellName[0] : "-")
                            .Real("victimThreat", victimThreat)
                            .Real("tankThreat", tankThreat)
                            .Bool("tankListed", tankListed)
                            .Bool("tankHits", tankHits)
                            .Bool("healer", healer)
                            .Bool("summon", summon)
                            .Bool("caster", caster)
                            .Bool("tankReaches", tankReaches);
                    }
                    Decisions().Write(run, "leader", "leader_aggro_loss", mob->GetGUID(), mob->GetName(),
                                                  true,
                                                  JsonObject().Str("mob", mob->GetName()).Num("entry", mob->GetEntry())
                                                      .Str("victim", victim ? victim->GetName() : "?")
                                                      .Real("tankDistance",
                                                            victim ? victim->GetExactDist(tank) : -1.0f)
                                                      .Merge(cause));
                }
        }
        for (auto it = lead.looseMobs.begin(); it != lead.looseMobs.end();)
        {
            if (looseSeen.count(it->first))  // [lead805]
            {
                ++it;
                continue;
            }
            Unit* mob = ObjectAccessor::GetUnit(*tank, it->first);
            char const* how = !mob || !mob->IsAlive()       ? "the mob died"
                              : mob->GetVictim() == tank    ? "the tank has it back"
                              : !mob->IsInCombat()          ? "the fight ended"
                                                            : "it left the member";
            Decisions().Write(run, "leader", "leader_aggro_recovered", it->first,
                                          mob ? mob->GetName() : std::string("?"), true,
                                          JsonObject().Str("how", how)
                                              .Real("seconds", float(getMSTimeDiff(it->second.second, now)) / 1000.0f));
            lead.tauntLogged.erase(it->first);  // [lead54] the episode ended
            lead.threatDoneFor.erase(it->first);  // [lead56]
            it = lead.looseMobs.erase(it);
        }
        if (looseOn.empty())
        {
            lead.tankCannotTake = false;
            lead.looseIsTarget = false;  // [lead245]
        }
        if (!looseOn.empty())
        {
            // the tank: the first loose mob (its taunt when ready, else its AoE threat, else an attack)
            Unit* mob = looseOn.begin()->second;
            // [lead112] b147 rfc-h 60-78 s: the tank owns its fight, but a mob on a member while the tank fights
            // nothing is the leader's command: the tank attacks it (issued once per change of the mob)
            // [lead252] b192 rfc-h 5D 1003-1037 s: a Searing Blade Warlock cast at the resto shaman 31 yd from the
            // bear for 34 s - the bear in combat with no victim ("tank assist", no target), the damage dealers on Stop
            // (no headroom: 0 tank threat on it). The tank that attacks nothing fights nothing, in combat or not
            Unit* const tankVictim = tank->GetVictim();
            if (Leader::LEADER_TANK_OWNS_FIGHT && (!tank->IsInCombat() || !tankVictim || !tankVictim->IsAlive()) &&
                Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack, mob->GetGUID(), {}))
                logEvent(tank, "Attack", JsonObject().Str("mob", mob->GetName())
                                             .Str("why", "aggro loss: the tank fights nothing"));
            // [lead54] b103 rfc-h pull 8 (wipe): 10 754 "taunt 31789" (Righteous Defense) commands - it is cast on
            // the friend, the executor casts the leader's taunt on the mob: it never went off and stayed "ready". The
            // taunts cast on the mob (Hand of Reckoning, Taunt, Growl, Dark Command); none ready: the AoE threat, else
            // the attack
            uint32 spell = LeaderReadySpell(tank, {TANK_TAUNTS[0], TANK_TAUNTS[1], TANK_TAUNTS[2], TANK_TAUNTS[3]});
            char const* how = "taunt";
            if (!spell)
            {
                for (uint32 aoeSpell : TANK_AOE_THREAT)
                    if (tank->HasSpell(aoeSpell) && !tank->HasSpellCooldown(aoeSpell))
                        spell = aoeSpell;
                how = spell ? "aoe threat" : "attack";
            }
            // [lead56] b105 rfc-h pull 7 wipe: the tank stood in "leader threat" with no target (its rotation never
            // ran, no threat, every damage dealer without headroom). A pick-up is one cast: issued only with a ready
            // spell and only until the tank reports it (its ack), never as a standing state; the tank's Attack on the
            // mob stays - it attacks the mob it took back and runs its rotation
            auto const done = lead.threatDoneFor.find(mob->GetGUID());
            bool const castDone = done != lead.threatDoneFor.end() && done->second == spell;
            bool const threatChanged = !Leader::LEADER_TANK_OWNS_FIGHT && spell && !castDone &&
                                       Leader::Events::SetThreat(tank->GetGUID(), spell, mob->GetGUID());
            bool const attackChanged =
                !Leader::LEADER_TANK_OWNS_FIGHT &&
                Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack, mob->GetGUID(), {});
            // [lead54] issued (logged, the tank woken) once per change of the pick-up: a new loose mob or a new spell
            // (the taunt came off cooldown / went on it) - not every tick the loose state flickers
            auto const logged = lead.tauntLogged.find(mob->GetGUID());
            if ((threatChanged || attackChanged) && (logged == lead.tauntLogged.end() || logged->second != spell))
            {
                lead.tauntLogged[mob->GetGUID()] = spell;
                logEvent(tank, "Taunt", JsonObject().Str("mob", mob->GetName()).Num("spell", spell).Str("how", how));
            }
            // [lead54] the tank can take it back now only with a ready taunt / AoE threat: else the damage dealers
            // with threat headroom kill it (no DontAttack while the healer dies)
            lead.tankCannotTake = spell == 0;
            // [lead36] b85 rfc-h: the tank stood 7.3 yd off the Trogg on the healer ("0 attackers ... going idle"):
            // the leader's Taunt / Attack includes the walk to melee - a MoveTo to the mob while out of its melee
            // reach (re-issued only when the mob moved off the issued point)
            // [lead39] b88 rfc-h pull 6 (112-183 s): the walk to the loose Trogg dropped the tank 6.9 yd off a
            // ledge ("[ledge drop] ... shortcut_drop leader move order") into the unpulled Troggs below (12 came for 2
            // planned). The walk only on the tank's own floor along a path with no drop; else the taunt / AoE alone
            if (!Leader::LEADER_TANK_OWNS_FIGHT && tank->IsAlive() && !tank->IsWithinMeleeRange(mob) &&
                LeaderWalkStaysOnFloor(tank, mob->GetPosition()))
            {
                Leader::MemberEvents tankEvents;
                bool const has = Leader::Events::Get(tank->GetGUID(), tankEvents) &&
                                 tankEvents.move == Leader::MoveEvent::MoveTo &&
                                 tankEvents.moveWhy == "aggro loss: to the mob";
                // [lead50] b99 rfc-h 245.3-246.1 s: ~15 "aggro loss: to the mob" moves in 1 s (a new point every tick).
                // The point changes only when the mob left it by the walk's own tolerance; logged once per loose mob
                float const reachTolerance = tank->GetCombatReach() + mob->GetCombatReach();
                bool const keepPoint = has && tankEvents.spot.GetExactDist(mob) <= reachTolerance;
                Position const point = keepPoint ? tankEvents.spot : mob->GetPosition();
                if (checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, point, reachTolerance,
                                "aggro loss: to the mob") &&
                    lead.dragFor != mob->GetGUID())
                {
                    lead.dragFor = mob->GetGUID();
                    logEvent(tank, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", "aggro loss: to the mob").Str("who", "tank")
                                 .Str("mob", mob->GetName()).Real("distance", tank->GetExactDist(mob)));
                    LogFightDrag(run, tank, point, lead.meetPoint, "aggro loss: to the mob");
                }
            }
            // the others' target is never a loose mob
            // [lead38] b87 rfc-h 507-600 s: the one Earthborer of the fight was loose on the healer (12.5 yd below
            // it, the tank never reached it): no target, nobody attacked for 90 s. A fight always has an Attack
            // target: with every fight mob loose, the loose mob is the target (the damage dealers kill it)
            Unit* const looseTarget = target && looseMobs.count(target->GetGUID()) ? target : nullptr;
            if (looseTarget && !lead.tankCannotTake)
            {
                target = nullptr;
                for (ObjectGuid const guid : killOrder)
                    if (!looseMobs.count(guid))
                        if (Unit* other = ObjectAccessor::GetUnit(*tank, guid))
                        {
                            target = other;
                            break;
                        }
            }
            // [lead245] the loose mob the tank cannot take back now (no taunt / AoE threat ready) hits a member while
            // the group kills the boss behind it: it is the target, first in the kill order
            if (!target || lead.tankCannotTake)
            {
                target = lead.tankCannotTake ? (looseTarget ? looseTarget : mob)
                         : target            ? target
                         : looseTarget       ? looseTarget
                                             : mob;
                lead.looseIsTarget = true;
                if (auto const at = std::find(killOrder.begin(), killOrder.end(), target->GetGUID());
                    at != killOrder.end())
                    std::rotate(killOrder.begin(), at, at + 1);
            }
            else
            {
                lead.looseIsTarget = false;
                killOrder.erase(std::remove_if(killOrder.begin(), killOrder.end(),
                                               [&looseMobs](ObjectGuid guid) { return looseMobs.count(guid) > 0; }),
                                killOrder.end());
            }
        }

        // [lead252] b192 rfc-h 5D 892.9-928.9 s: a Searing Blade Warlock cast at the bear from 25-37 yd for 36 s, the
        // bear in combat but its AI in the non-combat state ("tank assist" with no target, nothing attacked) and every
        // damage dealer on Stop (the bear's 19 threat on it). A tank in a fight that attacks nothing gets the fight's
        // target from the leader (a loose mob: the loose block above)
        if (Leader::LEADER_TANK_OWNS_FIGHT && fighting && target && looseOn.empty() && tank->IsAlive() &&
            tank->IsInCombat() && !(tank->GetVictim() && tank->GetVictim()->IsAlive()) &&
            Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack, target->GetGUID(), {}))
            logEvent(
                tank, "Attack",
                JsonObject().Str("mob", target->GetName()).Str("why", "the tank attacks nothing: the fight's target"));

        // [lead16] LEADER_CC_ASSIGN: the fight's crowd control - which mob, which member. A mob a member already
        // marked for the pack (the pre-pull plan's claim) stays its; more come from the tail of the kill order (never
        // the target, a boss, a backed-off or a group kill target) for a member that can hold it and reaches it (same
        // floor, sight: CrowdControlMarkReachable). The CC'd mobs leave the kill order.
        if (fighting && Leader::LEADER_CC_ASSIGN && target)
        {
            // [lead545] b213 rfc-h 5C 565.8-670 s: the balance druid's own pack mark (a Searing Blade Enforcer) became
            // its Cc, the Enforcer stood on the blood DK tank in melee, under its area damage; "leader cc" cast
            // Entangling Roots 40 times through Taragaman (the root broke at once and holds no mob that already hits
            // its victim), the druid ran dry (44 -> 0 %) and did 111 dps on the boss. The leader issues a Cc only
            // where the game lets it hold, on every tick: the member has a hold for the mob, nothing on the mob
            // breaks it (CrowdControlWouldBreak) unless it is held now, and a root alone is no hold on a mob in melee
            // with its victim
            auto const ccHolds = [&](Player* member, Unit* mob) -> bool
            {
                PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
                CrowdControlOption const* option = memberAI ? FindCrowdControl(memberAI, mob, false) : nullptr;
                if (!option)
                    return false;
                uint64 const mechanics = GetCrowdControlMechanics(GetCrowdControlHoldInfo(memberAI, *option));
                if (IsHeldByCrowdControl(mob, mechanics))
                    return true;
                if (CrowdControlWouldBreak(memberAI, mob))
                    return false;
                Unit* const victim = mob->GetVictim();
                bool const rootOnly = mechanics && !(mechanics & ~(uint64(1) << MECHANIC_ROOT));
                return !(rootOnly && victim && mob->IsWithinMeleeRange(victim));
            };
            for (auto it = lead.ccAssign.begin(); it != lead.ccAssign.end();)
            {
                Unit* mob = ObjectAccessor::GetUnit(*tank, it->first);
                Player* member = ObjectAccessor::FindPlayer(it->second);
                if (!mob || !mob->IsAlive() || !mob->IsInCombat() || !member || !member->IsAlive() ||
                    mob == target || looseOn.count(member->GetGUID()) || !ccHolds(member, mob))
                {
                    // [lead545] the command goes with the assignment (a command stands until replaced: the
                    // druid's Cc outlived its assignment and kept the roots on the tank's mob)
                    Leader::Events::SetCc(it->second, ObjectGuid::Empty);
                    it = lead.ccAssign.erase(it);
                }
                else
                    ++it;
            }
            std::set<ObjectGuid> controlling;
            for (auto const& [mob, member] : lead.ccAssign)
                controlling.insert(member);
            for (Player* bot : seats)
                if (bot != healer && !controlling.count(bot->GetGUID()))
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        if (Unit* marked = GetAutoCcTarget(botAI);
                            marked && marked->IsAlive() && marked != target && !lead.ccAssign.count(marked->GetGUID()) &&
                            ccHolds(bot, marked))  // [lead545]
                        {
                            lead.ccAssign[marked->GetGUID()] = bot->GetGUID();
                            controlling.insert(bot->GetGUID());
                        }
            PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
            bool const hardPack =
                tankAI && tankAI->GetAiObjectContext()->GetValue<bool>("auto cc hard pack")->Get();
            size_t const wanted = !hardPack ? 0 : killOrder.size() >= 5 ? 2 : 1;
            for (size_t i = killOrder.size(); i-- > 2 && lead.ccAssign.size() < wanted;)
            {
                Unit* mob = ObjectAccessor::GetUnit(*tank, killOrder[i]);
                Creature* creature = mob ? mob->ToCreature() : nullptr;
                // [lead28] by state only (no 15 s break backoff): not in the group's fight, nothing on it that breaks
                // the hold, and the hold spell's diminishing returns not at immune (the game's DR state)
                if (!creature || mob == target || lead.ccAssign.count(mob->GetGUID()) || creature->IsDungeonBoss() ||
                    IsGroupKillTarget(tank, mob))
                    continue;
                for (Player* bot : seats)
                {
                    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                    if (bot == healer || !botAI || controlling.count(bot->GetGUID()) || looseOn.count(bot->GetGUID()) ||
                        !CrowdControlMarkReachable(bot, mob))
                        continue;
                    CrowdControlOption const* option = FindCrowdControl(botAI, mob, false);
                    if (!option || CrowdControlWouldBreak(botAI, mob) || !ccHolds(bot, mob))  // [lead545]
                        continue;
                    if (SpellInfo const* hold = GetCrowdControlHoldInfo(botAI, *option))
                    {
                        DiminishingGroup const group = GetDiminishingReturnsGroupForSpell(hold, false);
                        if (group != DIMINISHING_NONE && mob->GetDiminishing(group) >= DIMINISHING_LEVEL_IMMUNE)
                            continue;
                    }
                    lead.ccAssign[mob->GetGUID()] = bot->GetGUID();
                    controlling.insert(bot->GetGUID());
                    break;
                }
            }
            for (auto const& [mobGuid, memberGuid] : lead.ccAssign)
            {
                Player* member = ObjectAccessor::FindPlayer(memberGuid);
                Unit* mob = ObjectAccessor::GetUnit(*tank, mobGuid);
                if (!member || !mob)
                    continue;
                if (Leader::Events::SetCc(memberGuid, mobGuid))
                    logEvent(member, "Cc", JsonObject().Str("mob", mob->GetName()).Num("entry", mob->GetEntry())
                                               .Bool("held", IsHeldByCrowdControl(mob, 0)));
                killOrder.erase(std::remove(killOrder.begin(), killOrder.end(), mobGuid), killOrder.end());
            }
        }
        else if (!fighting)
        {
            for (auto const& [mobGuid, memberGuid] : lead.ccAssign)
                Leader::Events::SetCc(memberGuid, ObjectGuid::Empty);  // [lead545]
            lead.ccAssign.clear();
        }

        // [lead16] LEADER_PRESAVE: a big hit foreseen - a heavy cast (its damage >= LEADER_PRESAVE_HIT_PCT of the
        // victim's health) of a fight mob on a member or the tank, an enraged boss on the tank, the tank low under a
        // boss: PreSave to that bot until the hit lands (a decision line leader_presave with the reason)
        if (fighting && Leader::LEADER_PRESAVE)
        {
            std::vector<Player*> everyone = seats;
            everyone.push_back(tank);
            auto const preSave = [&](Player* who, uint32 untilMs, std::string const& why)
            {
                if (!who || !who->IsAlive())
                    return;
                if (Leader::Events::SetPreSave(who->GetGUID(), untilMs, why))
                {
                    Decisions().Write(run, "leader", "leader_presave", who->GetGUID(), who->GetName(),
                                                  true, JsonObject().Str("member", who->GetName()).Str("why", why)
                                                            .Num("inMs", getMSTimeDiff(now, untilMs)));
                    wake(who);
                }
            };
            for (ObjectGuid const guid : killOrder)
            {
                Unit* mob = ObjectAccessor::GetUnit(*tank, guid);
                Spell* spell = mob ? mob->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
                SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
                if (!info || info->IsPositive())
                    continue;
                int32 damage = 0;
                bool area = false;
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (info->Effects[i].Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                        info->Effects[i].Effect == SPELL_EFFECT_WEAPON_DAMAGE ||
                        info->Effects[i].Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG ||
                        info->Effects[i].Effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE)
                    {
                        damage += info->Effects[i].CalcValue(mob);
                        area |= info->Effects[i].HasRadius();
                    }
                if (damage <= 0)
                    continue;
                uint32 const until = now + uint32(std::max(0, spell->GetCastTime() > 0 ? int32(spell->GetCastTime())
                                                                                        : 0)) +
                                     IN_MILLISECONDS;
                std::string const why = Acore::StringFormat("{} casts {} ({} damage)", mob->GetName(),
                                                            info->SpellName[LOCALE_enUS], damage);
                Unit* victim = spell->m_targets.GetUnitTarget();
                for (Player* bot : everyone)
                {
                    bool const hit = victim == bot ||
                                     (area && bot->GetExactDist(victim ? victim : mob) <=
                                                  info->Effects[EFFECT_0].CalcRadius(mob) + bot->GetCombatReach());
                    if (hit && float(damage) >= float(bot->GetMaxHealth()) * Leader::LEADER_PRESAVE_HIT_PCT / 100.0f)
                        preSave(bot, until, why);
                }
            }
            if (Unit* victim = tank->GetVictim();
                victim && victim->ToCreature() && victim->ToCreature()->IsDungeonBoss())
            {
                bool enraged = false;
                for (auto const& [id, application] : victim->GetAppliedAuras())
                    if (application && application->GetBase() && application->GetBase()->GetSpellInfo()->Dispel ==
                                                                      DISPEL_ENRAGE)
                        enraged = true;
                if (enraged)
                    preSave(tank, now + 2 * IN_MILLISECONDS, victim->GetName() + " is enraged");
                else if (tank->GetHealthPct() < Leader::LEADER_PRESAVE_TANK_PCT && victim->GetVictim() == tank)
                    preSave(tank, now + 2 * IN_MILLISECONDS,
                            Acore::StringFormat("the tank at {:.0f} % under {}", tank->GetHealthPct(),
                                                victim->GetName()));
            }
        }

        // [lead16] LEADER_HOLD_EVENTS: no target of the fight takes damage (immune: a boss phase) - the next one
        // that does, else StopDps for everyone
        bool stopDps = false;
        if (fighting && Leader::LEADER_HOLD_EVENTS && target && target->IsImmunedToDamage(SPELL_SCHOOL_MASK_ALL))
        {
            Unit* other = nullptr;
            for (ObjectGuid const guid : killOrder)
                if (Unit* mob = ObjectAccessor::GetUnit(*tank, guid);
                    mob && mob != target && mob->IsAlive() && !mob->IsImmunedToDamage(SPELL_SCHOOL_MASK_ALL))
                {
                    other = mob;
                    break;
                }
            if (other)
                target = other;
            else
                stopDps = true;
        }
        // [lead16] LEADER_TANK_EXECUTES: the tank's target (no aggro loss: its own victim, else the kill order's head)
        if (fighting && Leader::LEADER_TANK_EXECUTES && !Leader::LEADER_TANK_OWNS_FIGHT && looseOn.empty() &&
            Leader::LEADER_ATTACK_EVENTS)
        {
            Unit* tankTarget = tank->GetVictim();
            if (!tankTarget || !tankTarget->IsAlive() || !tankTarget->IsInCombat() ||
                lead.ccAssign.count(tankTarget->GetGUID()))
                tankTarget = target;
            if (tankTarget && Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack,
                                                        tankTarget->GetGUID(), killOrder))
                logEvent(tank, "Attack", JsonObject().Str("target", tankTarget->GetName())
                                             .Num("targetEntry", tankTarget->GetEntry()).Str("who", "tank"));
        }
        // [lead16] LEADER_LOOT_EVENTS: when a fight ends, one member (a damage dealer) loots for LEADER_LOOT_MS
        if (Leader::LEADER_LOOT_EVENTS)
        {
            if (lead.lastFighting && !fighting)
            {
                lead.looter.Clear();
                for (Player* bot : seats)
                    if (bot != healer)
                    {
                        lead.looter = bot->GetGUID();
                        break;
                    }
                lead.lootSinceMs = now;
            }
            if (fighting || (lead.lootSinceMs && getMSTimeDiff(lead.lootSinceMs, now) > Leader::LEADER_LOOT_MS))
                lead.looter.Clear();
            if (!lead.looter.IsEmpty())
                if (Player* looter = ObjectAccessor::FindPlayer(lead.looter))
                    if (Leader::Events::SetLoot(lead.looter, true))
                        logEvent(looter, "Loot", JsonObject().Str("why", "the fight is over"));
        }
        // [lead39] b88 rfc-h pull 6: extra_aggro read "fightSpotDistance 103.8" against the meeting point of an
        // earlier pull - an unplanned fight meets where the tank stands when it starts
        if (fighting && !lead.lastFighting && !(lead.commit && lead.commitIssued))
            lead.meetPoint = tank->GetPosition();
        // [lead88] the fight's estimated length at its start (the planner's published next fight) against its real
        // length at its end: the measured uncertainty the mana lines' reserve is computed from
        if (fighting && !lead.lastFighting)
        {
            RestState::NextFight next;
            lead.fightLenStartMs = now;
            lead.fightLenEstSec = RestState::PredictNextFight(tank, next)
                                      ? (next.fightSec > 0.0f ? next.fightSec : RestState::FightSecOf(tank, next))
                                      : -1.0f;
        }
        else if (!fighting && lead.lastFighting && lead.fightLenStartMs && lead.fightLenEstSec >= 0.0f)
        {
            float const realSec = getMSTimeDiff(lead.fightLenStartMs, now) / 1000.0f;
            if (Group* group = tank->GetGroup())
            {
                RestState::NoteFightLengthError(group->GetGUID(), lead.fightLenEstSec, realSec);  // [lead362]
                // [lead234] a boss kill: the group's damage on the boss over the fight's seconds
                float health = 0.0f;
                bool killed = !lead.fightBosses.empty();
                for (auto const& [guid, maxHealth] : lead.fightBosses)
                {
                    Creature const* boss = map->GetCreature(guid);
                    killed &= !boss || !boss->IsAlive();
                    health += maxHealth;
                }
                if (killed)
                    Leader::Knowledge::NoteBossKill(group->GetGUID(), health, realSec);
            }
            lead.fightBosses.clear();
            Decisions().Write(run, "leader", "fight_length", ObjectGuid(), tank->GetName(), false,
                                          JsonObject().Real("estimatedSec", lead.fightLenEstSec)
                                              .Real("realSec", realSec));
            lead.fightLenStartMs = 0;
        }
        if (!fighting && lead.lastFighting)
        {
            lead.fightBosses.clear();
            lead.peaceSinceMs = now;  // [lead940] a heal over time cast after this is an out-of-fight one
        }
        // [lead940] the heals over time running on the members as the fight starts, by whom, and whether one was cast
        // out of the last fight (a top-up's): their ticks forward threat to the caster from the start (triage
        // heal_gate "pre-pull HoT")
        if (fighting && !lead.lastFighting)
        {
            std::vector<Player*> all = seats;
            all.push_back(tank);
            for (Player* member : all)
                for (AuraEffect const* effect : member->GetAuraEffectsByType(SPELL_AURA_PERIODIC_HEAL))
                {
                    Aura const* aura = effect->GetBase();
                    if (aura->GetCasterGUID() == member->GetGUID() || !aura->GetCasterGUID().IsPlayer())
                        continue;
                    Player* caster = ObjectAccessor::FindPlayer(aura->GetCasterGUID());
                    int32 const ranMs = aura->GetMaxDuration() - aura->GetDuration();
                    bool const outOfFight = !lead.peaceSinceMs || ranMs < int32(getMSTimeDiff(lead.peaceSinceMs, now));
                    Decisions().Write(run, "leader", "pull_hot", member->GetGUID(), member->GetName(), false,
                                      JsonObject().Str("member", member->GetName())
                                          .Str("healer", caster ? caster->GetName() : std::string("-"))
                                          .Num("spell", aura->GetId())
                                          .Str("spellName", aura->GetSpellInfo()->SpellName[0])
                                          .Real("leftSec", aura->GetDuration() / 1000.0f)
                                          .Bool("tank", member == tank).Bool("outOfFight", outOfFight));
                }
        }
        lead.lastFighting = fighting;
        // [lead84] b127 rfc-h 206-306 s: a Ragefire Trogg 5.5 yd below on Rtdruaa (not evading, the tank out of its
        // sight, the attacks refused "no line of sight") kept the tank flagged in combat with nobody on it; the
        // harness counted the fight over at 221 s, the leader stayed in its fight mode and issued nothing for 85 s.
        // Out of a fight (Party::FightOn) with the group still flagged: the lingering mob set is logged on a change;
        // an attackable one (alive, not evading) the tank walks to (checked); refused, it is unreachable - the
        // leader's out-of-combat flow goes on and the harness's hunt for it ends (its stale combat ends as an
        // evading mob's would)
        bool lingeringWalk = false;
        if (!fighting && tank->IsInCombat())
        {
            std::vector<Player*> flagged = seats;
            flagged.push_back(tank);
            std::set<ObjectGuid> listed;
            std::string sig;
            JsonArray refs;
            Creature* lingering = nullptr;
            for (Player* bot : flagged)
                for (auto const& [refGuid, ref] : bot->GetCombatManager().GetPvECombatRefs())
                {
                    Unit* other = ref ? ref->GetOther(bot) : nullptr;
                    Creature* mob = other ? other->ToCreature() : nullptr;
                    if (!mob || !mob->IsAlive() || mob->GetMap() != map || !listed.insert(mob->GetGUID()).second)
                        continue;
                    bool const evades = mob->IsInEvadeMode();
                    sig += std::to_string(mob->GetGUID().GetCounter()) + (evades ? "e" : "") + ",";
                    Unit* victim = mob->GetVictim();
                    refs.Raw(JsonObject().Str("name", mob->GetName()).Num("low", mob->GetGUID().GetCounter())
                                 .Str("victim", victim ? victim->GetName() : "-").Bool("evades", evades)
                                 .Real("tankDistance", tank->GetExactDist(mob))
                                 .Real("dz", mob->GetPositionZ() - tank->GetPositionZ())
                                 .Bool("tankSees", tank->IsWithinLOSInMap(mob)).Bool("moving", mob->isMoving())
                                 .Build());
                    if (!evades && tank->IsValidAttackTarget(mob) &&
                        (!lingering || tank->GetExactDist(mob) < tank->GetExactDist(lingering)))
                        lingering = mob;
                }
            if (sig != lead.lingeringSig)
            {
                lead.lingeringSig = sig;
                Decisions().Write(run, "leader", "lingering_combat",
                                              lingering ? lingering->GetGUID() : ObjectGuid(),
                                              lingering ? lingering->GetName() : std::string("-"), true,
                                              JsonObject().Raw("mobs", refs.Build())
                                                  .Bool("harnessFight", group.InCombat())
                                                  .Num("tankAttackers", int64(tank->getAttackers().size())));
            }
            if (lingering && lead.lingeringUnreachable != lingering->GetGUID())
            {
                float const pickUp = LeaderTauntReach(tank);
                float const arrive = pickUp > 0.0f ? pickUp : NOMINAL_MELEE_RANGE;
                if (!(tank->GetExactDist(lingering) <= arrive && tank->IsWithinLOSInMap(lingering)))
                {
                    std::string const why = "lingering combat: the tank goes to the mob";
                    checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, lingering->GetPosition(), arrive, why);
                    Leader::MemberEvents tankNow;
                    lingeringWalk = Leader::Events::Get(tank->GetGUID(), tankNow) && tankNow.moveWhy == why;
                    if (!lingeringWalk)
                    {
                        lead.lingeringUnreachable = lingering->GetGUID();
                        if (group.PhantomHunt() == lingering->GetGUID())
                            group.ClearPhantomHunt();
                        Decisions().Write(run, "leader", "lingering_unreachable", lingering->GetGUID(),
                                                      lingering->GetName(), true,
                                                      JsonObject().Str("why", tankNow.moveWhy)
                                                          .Real("tankDistance", tank->GetExactDist(lingering)));
                    }
                }
            }
        }
        else
        {
            lead.lingeringSig.clear();
            lead.lingeringUnreachable.Clear();
        }
        // [lead34] the route is data: its waypoint for the tank becomes the leader's MoveTo ("route walk") while no
        // pull is committed and no fight is on (the special waypoints stay the tank's own walk: a jump, a straight
        // line, a gate pass)
        if (fighting)
            lead.groupBlockedBy.clear();
        // [lead82] b125 rfc-h 517.3-616 s: Rtmagaa's "to range of the engage" was blocked ("no walk to the spot"), the
        // engage ended at 520.2 and the block stayed - the Taragaman pull waited "a member cannot follow" 70 s. A block
        // holds only while the member's current command is still that walk's stay
        for (auto it = lead.groupBlockedBy.begin(); it != lead.groupBlockedBy.end();)
        {
            Leader::MemberEvents blocked;
            // [lead368] ... and while the mob in the way is there to wake: idle and alive (dead, gone or fighting,
            // the walk it blocked is clear of it)
            Creature const* blocker = it->second.IsEmpty() ? nullptr : map->GetCreature(it->second);
            bool const blockerStands =
                it->second.IsEmpty() || (blocker && blocker->IsAlive() && !blocker->IsInCombat());
            bool const holds = blockerStands && Leader::Events::Get(it->first, blocked) &&
                               blocked.move == Leader::MoveEvent::StandAt &&
                               blocked.moveWhy.rfind("along with the tank", 0) == 0 &&
                               blocked.moveWhy.find(": stays (") != std::string::npos;
            if (holds)
                ++it;
            else
            {
                lead.spotRejectLogged.erase(it->first);
                it = lead.groupBlockedBy.erase(it);
            }
        }
        // [lead94] travel per route leg (the route target): its real seconds, the tank's moving seconds and every stop
        // by its cause, logged when the leg ends (travel_leg)
        {
            float const dt = lead.travelTickMs ? getMSTimeDiff(lead.travelTickMs, now) / 1000.0f : 0.0f;
            lead.travelTickMs = now;
            if (group.RouteTarget() != lead.travelTarget)
            {
                if (!lead.travelTarget.IsEmpty() && lead.travelSec > 0.0f)
                {
                    JsonObject stops;
                    for (auto const& [cause, sec] : lead.travelStops)
                        stops.Real(cause.c_str(), sec);
                    Decisions().Write(run, "leader", "travel_leg", lead.travelTarget, tank->GetName(),
                                                  false,
                                                  JsonObject().Real("straight",
                                                                    lead.travelFrom.GetExactDist(tank->GetPosition()))
                                                      .Real("sec", lead.travelSec).Real("moving", lead.travelMoving)
                                                      .Real("fights", lead.travelFight).Raw("stops", stops.Build())
                                                      .Str("pathSource", lead.routePathSource)
                                                      .Bool("pathComplete", lead.routePathComplete));
                }
                lead.travelTarget = group.RouteTarget();
                lead.travelFrom = tank->GetPosition();
                lead.travelSec = lead.travelMoving = lead.travelFight = 0.0f;
                lead.travelStops.clear();
            }
            else if (dt > 0.0f && dt < 5.0f && !group.RouteTarget().IsEmpty())
            {
                lead.travelSec += dt;
                if (fighting)
                    lead.travelFight += dt;
                else if (tank->isMoving())
                    lead.travelMoving += dt;
                else
                {
                    LeaderGroup::WalkPoint routePoint;
                    std::string const cause =
                        lead.commit ? (lead.spotsWaiting ? "pull: the spots wait" : "pull committed")
                        : !lead.groupBlockedBy.empty() ? "a member's walk blocked"
                        : !lead.trailing.empty()       ? "a member trails"
                        : anySitting || tank->IsSitState() ? "rest"
                        : group.NeedBuff() || rebuff     ? "buffs"
                        : !lead.routeFirstFor.IsEmpty() ? "route first"
                        : !group.GetWaypoint(tank->GetGUID(), routePoint) ? "no route point"
                                                                                : "other";
                    lead.travelStops[cause] += dt;
                }
            }
        }
        // [lead91] a fight or a pull moves the tank: the kept route point is judged afresh after it
        if (fighting || lead.commit)
        {
            lead.routeWalkPoint.reset();
            lead.routePath.clear();  // [lead97] the leg's path is taken again from where the tank stands after it
        }
        // [lead77] the tank walks on only where the group follows: no route walk while a member is blocked
        if (Leader::LEADER_ROUTE_IS_DATA && !fighting && !lead.commit && !Leader::Orders::IsOpen(group.LeaderGuid()) &&
            lead.groupBlockedBy.empty() && !lingeringWalk)
        {
            LeaderGroup::WalkPoint waypoint;
            if (group.GetWaypoint(tank->GetGUID(), waypoint) && waypoint.mapId == tank->GetMapId() &&
                !waypoint.jump && !waypoint.straight && !waypoint.gatePass)
            {
                Position point(waypoint.x, waypoint.y, waypoint.z);
                // [lead91] b133 rfc-h 304.4-308.5 s: the route point was recomputed every second from where the
                // tank walked and stepped back (-165.5 -> -173.3: command_repeat). One command per route point:
                // the point the tank walks to is kept until it reaches it, a new one is taken when it lies further
                // along (nearer the route target) or the route itself changes (a new target) - never a step back
                // [lead94] b135: the straight-line "nearer the target" test held the kept point on a winding route
                // until the tank reached it - a stop every few yards. The test is the walk's own direction: a new
                // point on the kept point's side of the tank is ahead (taken at once, the walk goes on); one behind
                // the tank waits until the kept point is reached
                // [lead97] b137 rfc-h 297.9-300.9 s: the harness's per-second recompute went back again (-167.1 ->
                // -174.2, still "ahead" of the tank) - command_repeat at b133's place. The leg's path is taken ONCE
                // (the navmesh path to the route target) and walked by index: the next path point when the tank
                // reaches the current one; a new path only when the leg's target changes, the path is walked to its
                // end, or the tank stands off it by more than its arrival tolerance (a fight / a pull resets it too)
                constexpr float ROUTE_ARRIVED_YD = 1.5f;  // the route walk's MoveTo tolerance
                Unit* legTarget =
                    group.RouteTarget().IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*tank, group.RouteTarget());
                if (!lead.routePath.empty() && lead.routePathIndex < lead.routePath.size() &&
                    lead.routePathIndex > 0)
                {
                    // off the path: farther from the current segment than the arrival tolerance + the tank's reach
                    G3D::Vector3 const& a = lead.routePath[lead.routePathIndex - 1];
                    G3D::Vector3 const& b = lead.routePath[lead.routePathIndex];
                    G3D::Vector3 const p(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
                    G3D::Vector3 const ab = b - a;
                    float const len2 = ab.squaredLength();
                    float const k = len2 > 0.0f ? std::clamp((p - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
                    if ((a + ab * k - p).length() > ROUTE_ARRIVED_YD + tank->GetCombatReach())
                        lead.routePath.clear();
                }
                if (legTarget && (lead.routePathTarget != group.RouteTarget() || lead.routePath.empty() ||
                                  lead.routePathIndex >= lead.routePath.size()))
                {
                    // [lead98] b138 rfc-h 335-427 s: from the Oggleflint ledge the plain navmesh path to the route
                    // target was partial and the walk stood at its end - the route is data the leader reads: the
                    // harness's own route leg (SteerLeg: its long route search, door and detour legs, the route
                    // links), taken once per leg and walked by index
                    // [lead99] the leader's own route search (Ai/Leader/Travel, Leader::Route): the leg's corners
                    Leader::RouteResult const leg = Leader::Route(tank, legTarget->GetPosition());
                    lead.routePath.clear();
                    if (leg.found && leg.corners.size() >= 2)
                        lead.routePath = leg.corners;
                    // the leg path's source and completeness, for travel_leg
                    lead.routePathSource = leg.Describe();
                    lead.routePathComplete = leg.found && leg.complete;
                    lead.routePathIndex = 0;  // the arrived points are skipped below
                    lead.routePathTarget = group.RouteTarget();
                }
                while (!lead.routePath.empty() && lead.routePathIndex + 1 < lead.routePath.size() &&
                       tank->GetExactDist2d(lead.routePath[lead.routePathIndex].x,
                                            lead.routePath[lead.routePathIndex].y) <= ROUTE_ARRIVED_YD)
                    ++lead.routePathIndex;
                if (!lead.routePath.empty() && lead.routePathIndex < lead.routePath.size())
                {
                    G3D::Vector3 const& next = lead.routePath[lead.routePathIndex];
                    point.Relocate(next.x, next.y, next.z);
                }
                lead.routeWalkPoint = point;
                lead.routeWalkTarget = group.RouteTarget();
                // [lead73] b117 271.4 s: the route walk woke a Trogg ("would wake ..., walked anyway") - it goes
                // through the leader's checks and the tank's own guards run dry (checkedMove); refused, the tank
                // stays and the mob on the path is the next pull
                if (checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, point, 1.5f, "route walk") &&
                    lead.routeWalkSerial != waypoint.serial)
                {
                    lead.routeWalkSerial = waypoint.serial;
                    logEvent(tank, "RouteWalk", JsonObject().Str("who", "tank").Real("x", point.GetPositionX())
                                                    .Real("y", point.GetPositionY()).Real("z", point.GetPositionZ()));
                }
            }
        }
        // [lead34] b83 rfc-h 20261031-152252 (98-180 s): an engage on a Ragefire Trogg 64 yd out of the tank's sight,
        // the straight walk for sight refused, «walks its path in» walked nothing and the order ended unbegun. The
        // open engage order out of sight: the leader walks the tank's path in toward the mob until it sees it (then
        // the tank's executor takes its next step, the attack)
        if (Leader::LEADER_ROUTE_IS_DATA && !Leader::LEADER_TANK_WALKS_PULL && !fighting)
        {
            Leader::PullOrder order;
            if (Leader::Orders::GetOpen(group.LeaderGuid(), order) && order.method == Leader::PullMethod::Engage &&
                order.state == Leader::OrderState::Issued)
                if (Unit* mob = ObjectAccessor::GetUnit(*tank, order.target))
                    // [lead49] the walk is judged once before the order goes out (runCommit), not again here
                    if (mob->IsAlive() && !mob->IsInCombat() && !tank->IsWithinLOSInMap(mob))
                    {
                        Position const point = mob->GetPosition();
                        if (Leader::Events::SetMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, point, 3.0f,
                                                    "engage path in") &&
                            lead.engageWalkFor != mob->GetGUID())
                        {
                            lead.engageWalkFor = mob->GetGUID();
                            logEvent(tank, "EngagePathIn",
                                     JsonObject().Str("who", "tank").Str("mob", mob->GetName())
                                         .Real("distance", tank->GetExactDist(mob)));
                        }
                    }
        }
        // [lead38] b87 rfc-h pull 1 (16-22 s): the hunter's pet on a Crab ("kill command -> Crab"). A member's pet
        // fights the leader's target only: a pet on a critter or on a mob outside the group's fight is sent to the
        // member's Attack target (the client's pet Attack button), else called back (Follow)
        if (fighting)
            for (Player* bot : seats)
            {
                Pet* pet = bot->GetPet();
                Unit* petVictim = pet && pet->IsAlive() ? pet->GetVictim() : nullptr;
                Creature* victimMob = petVictim ? petVictim->ToCreature() : nullptr;
                if (!victimMob || !pet->GetCharmInfo())
                    continue;
                bool const wrong = victimMob->IsCritter() || !LeaderFightMob(tank, victimMob) ||
                                   (!victimMob->IsInCombat() && victimMob->GetVictim() != pet);
                if (!wrong)
                    continue;
                Leader::MemberEvents petEvents;
                Unit* petTarget = Leader::Events::Get(bot->GetGUID(), petEvents) &&
                                          petEvents.attack == Leader::AttackEvent::Attack
                                      ? ObjectAccessor::GetUnit(*bot, petEvents.target)
                                      : nullptr;
                CharmInfo* charmInfo = pet->GetCharmInfo();
                pet->AttackStop();
                pet->InterruptNonMeleeSpells(false);
                if (petTarget && petTarget->IsAlive() && petTarget->IsInCombat())
                {
                    pet->ClearUnitState(UNIT_STATE_FOLLOW);
                    charmInfo->SetIsCommandAttack(true);
                    charmInfo->SetIsAtStay(false);
                    charmInfo->SetIsFollowing(false);
                    charmInfo->SetIsCommandFollow(false);
                    charmInfo->SetIsReturning(false);
                    pet->AI()->AttackStart(petTarget);
                }
                else
                {
                    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
                    charmInfo->SetCommandState(COMMAND_FOLLOW);
                    charmInfo->SetIsCommandAttack(false);
                    charmInfo->SetIsReturning(true);
                    charmInfo->SetIsCommandFollow(true);
                }
                Decisions().Write(run, "leader", "pet_redirect", victimMob->GetGUID(), victimMob->GetName(),
                                              true,
                                              JsonObject().Str("member", bot->GetName()).Str("pet", pet->GetName())
                                                  .Str("was", victimMob->GetName())
                                                  .Bool("critter", victimMob->IsCritter())
                                                  .Str("to", petTarget ? petTarget->GetName() : "follow"));
            }
        // [lead61] b106 pull 1 started "unplanned: Rthunaa" 35 ms before the tank: only the tank opens a pull - out
        // of a fight a member's pet on a mob is called back
        if (!fighting)
            for (Player* bot : seats)
                if (Pet* pet = bot->GetPet(); pet && pet->IsAlive() && pet->GetVictim() && pet->GetCharmInfo())
                {
                    pet->AttackStop();
                    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
                    pet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
                    pet->GetCharmInfo()->SetIsCommandAttack(false);
                    Decisions().Write(run, "leader", "pet_redirect", bot->GetGUID(), bot->GetName(), true,
                                                  JsonObject().Str("member", bot->GetName())
                                                      .Str("pet", pet->GetName()).Str("to", "follow")
                                                      .Str("why", "only the tank opens a pull"));
                }
        // [lead69] command_dupe_blocked: identical commands the arbiter refused (should be 0), logged when they happen
        {
            std::vector<Player*> commanded = seats;
            commanded.push_back(tank);
            for (Player* member : commanded)
                if (uint32 const dupes = Leader::Events::TakeDupesBlocked(member->GetGUID()))
                {
                    uint32& total = lead.dupesBlocked[member->GetName()];
                    if (!total)
                        Decisions().Write(
                            run, "leader", "command_dupe_blocked", member->GetGUID(), member->GetName(), false,
                            JsonObject().Str("member", member->GetName()).Num("count", dupes));
                    total += dupes;
                }
            // [lead320] leader_contradiction: a rule that would reverse a command within one fact (should be 0)
            for (Player* member : commanded)
                for (Leader::Contradiction contradiction;
                     Leader::Events::TakeContradiction(member->GetGUID(), contradiction);)
                    Decisions().Write(run, "leader", "leader_contradiction", member->GetGUID(), member->GetName(), true,
                                      JsonObject()
                                          .Str("member", member->GetName())
                                          .Str("domain", contradiction.domain)
                                          .Str("was", contradiction.was)
                                          .Str("current", contradiction.current)
                                          .Str("again", contradiction.again)
                                          .Num("fact", contradiction.fact)
                                          .Bool("refused", contradiction.refused)
                                          .Num("count", contradiction.count));
        }
        // [lead76] the fight moves away from a pack it cannot take (the leader's fight spot): one tank MoveTo
        if (!fighting)
        {
            lead.fightMoveTo.reset();
            lead.joinedPack.clear();
            lead.joinedMobs.clear();
        }
        else if (lead.fightMoveTo)
        {
            // [lead909] the fight's move to a blocked healer keeps its own purpose
            std::string const moveWhy = lead.fightMoveWhy.empty()
                                            ? std::string("hazard: the fight moves away from an unpulled pack")
                                            : lead.fightMoveWhy;
            if (tank->GetExactDist(*lead.fightMoveTo) <= 2.0f)
                lead.fightMoveTo.reset();
            else if (checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, *lead.fightMoveTo, 2.0f, moveWhy))
                LogFightDrag(run, tank, *lead.fightMoveTo, lead.meetPoint,
                             lead.fightMoveWhy.empty() ? "away from an unpulled pack" : "to the blocked healer");
        }
        if (!lead.fightMoveTo)
            lead.fightMoveWhy.clear();
        // [lead81] b124 rfc-h 167.8-182 s: Rtshaab pulled a Trogg 150 yd from the tank (no sight), the tank stood and
        // he died alone - a member a fight mob is on, beyond the tank's pick-up reach (its taunt's range, in sight):
        // the fight spot moves to it - the tank walks to the member (its walk checked) while the member runs to the
        // tank (the aggro-loss run) and the others leave that mob alone (the aggro-loss rule)
        // [lead217] b184 rfc-h 5D: a caster that stands off its victim (not in its melee, not moving) does not follow
        // the victim's run to the tank - the tank goes to the mob itself (its taunt's range, in sight)
        if (fighting && !lead.fightMoveTo)
        {
            float const pickUp = LeaderTauntReach(tank);
            float const arrive = pickUp > 0.0f ? pickUp : NOMINAL_MELEE_RANGE;
            Player* alone = nullptr;
            Unit* goTo = nullptr;
            for (auto const& [victimGuid, mob] : looseOn)
                if (Player* victim = ObjectAccessor::FindPlayer(victimGuid);
                    victim && victim != tank && victim->IsAlive() && victim->GetMap() == map)
                {
                    Unit* const at = LooseStandsOff(mob, victim) ? mob : static_cast<Unit*>(victim);
                    if (!(at->GetExactDist(tank) <= arrive && tank->IsWithinLOSInMap(at)) &&
                        (!goTo || at->GetExactDist(tank) < goTo->GetExactDist(tank)))
                    {
                        alone = victim;
                        goTo = at;
                    }
                }
            bool const toMob = goTo && goTo != alone;
            // [lead572] b215 rfc-h 5B 201.7 / 241.8 s: a Ragefire Trogg 13.2 yd from the warrior, 3.5 yd below it and
            // out of its sight, took the priest / rogue; the tank's MoveTo arrived at the taunt's 30 yd (it stood
            // inside it already) and it never walked - 51 s of the run's 87 s standing (phantom combat, "the attack
            // refused: no line of sight"); 24 of the 58 such walks in b215 were issued in reach but out of sight. In
            // reach but out of sight the walk ends where the tank sees it: at the unit (melee reach)
            float const tolerance =
                goTo && goTo->GetExactDist(tank) <= arrive && !tank->IsWithinLOSInMap(goTo) ? NOMINAL_MELEE_RANGE
                                                                                            : arrive;
            std::string const pickUpWhy =
                toMob ? "aggro loss: the tank goes to the mob" : "aggro loss: the tank goes to the member";
            bool pickUpIssued = false;
            if (alone)
            {
                // [lead983] b228 rfc-h 5D-12 (20261103-224121) 805-865 s: a Searing Blade Warlock cast on the unholy
                // DK from 27.6 yd out of the bear's sight for 60 s; the bear's walk to the mob passed an idle
                // Enforcer pack judged too much for the fight (fight_moves_away, overload 1.14), the tank "stays"
                // and then "fights nothing" for 48 s. The walk's goal is a place the tank picks the mob up from
                // (its taunt's range, in sight - lead81 / lead217), not the mob's own spot: once the walk to the mob
                // is refused, the nearest such place (a ring round the unit inside the taunt's range, in its
                // sight, out of every idle mob's reach) that the leader's guards pass is the walk
                thread_local std::map<ObjectGuid, std::pair<ObjectGuid, Position>> pickUpSpots;
                if (pickUpSpots.size() > 4096)
                    pickUpSpots.clear();
                auto const seesFrom = [&](Position const& spot)
                {
                    return goTo->GetExactDist(spot) <= arrive - 1.0f &&
                           map->isInLineOfSight(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + 2.0f,
                                                goTo->GetPositionX(), goTo->GetPositionY(),
                                                goTo->GetPositionZ() + 2.0f, tank->GetPhaseMask(),
                                                LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
                };
                auto kept = pickUpSpots.find(tank->GetGUID());
                if (kept != pickUpSpots.end() &&
                    (kept->second.first != goTo->GetGUID() || !seesFrom(kept->second.second)))
                {
                    pickUpSpots.erase(kept);
                    kept = pickUpSpots.end();
                }
                if (kept != pickUpSpots.end())
                    pickUpIssued = checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, kept->second.second,
                                               1.5f, pickUpWhy + " (a spot in its taunt's reach and sight)");
                else
                {
                    pickUpIssued =
                        checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, goTo->GetPosition(), tolerance,
                                    pickUpWhy);
                    if (!pickUpIssued && pickUp > 0.0f)
                    {
                        std::vector<Position> ring;
                        for (float radius : {arrive - 4.0f, arrive * 0.66f, arrive * 0.33f})
                            for (uint32 i = 0; i < 12 && radius > NOMINAL_MELEE_RANGE; ++i)
                            {
                                float const angle = 2.0f * float(M_PI) * float(i) / 12.0f;
                                float x = goTo->GetPositionX() + std::cos(angle) * radius;
                                float y = goTo->GetPositionY() + std::sin(angle) * radius;
                                float z = goTo->GetPositionZ();
                                if (!map->CheckCollisionAndGetValidCoords(goTo, goTo->GetPositionX(),
                                                                          goTo->GetPositionY(), goTo->GetPositionZ(),
                                                                          x, y, z))
                                    continue;
                                Position const spot(x, y, z);
                                if (seesFrom(spot) && !LeaderAggroAt(tank, spot))
                                    ring.push_back(spot);
                            }
                        std::sort(ring.begin(), ring.end(), [&](Position const& a, Position const& b)
                                  { return tank->GetExactDist(a) < tank->GetExactDist(b); });
                        // the nearest few by the straight line (each judged by the guards' route search)
                        constexpr size_t RING_TRIES = 6;
                        if (ring.size() > RING_TRIES)
                            ring.resize(RING_TRIES);
                        for (Position const& spot : ring)
                        {
                            if (!checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, spot, 1.5f,
                                             pickUpWhy + " (a spot in its taunt's reach and sight)"))
                                continue;
                            pickUpIssued = true;
                            pickUpSpots[tank->GetGUID()] = {goTo->GetGUID(), spot};
                            LOG_INFO("playerbots",
                                     "[lead983] {}: its walk to {} ({:.1f} yd) is refused; it picks it up from "
                                     "({:.1f},{:.1f},{:.1f}), {:.1f} yd off it in its sight",
                                     tank->GetName(), goTo->GetName(), tank->GetExactDist(goTo), spot.GetPositionX(),
                                     spot.GetPositionY(), spot.GetPositionZ(), goTo->GetExactDist(spot));
                            break;
                        }
                    }
                }
            }
            if (pickUpIssued)
                Decisions().Write(run, "leader", "fight_spot_to_member", alone->GetGUID(),
                                              alone->GetName(), true,
                                              JsonObject().Str("member", alone->GetName())
                                                  .Str("to", toMob ? goTo->GetName() : alone->GetName())
                                                  .Real("distance", goTo->GetExactDist(tank))
                                                  .Real("pickUp", pickUp)
                                                  .Bool("sight", tank->IsWithinLOSInMap(goTo)));
            // [lead572] ... and the same Trogg while it was on the warrior (5B 196-201 / 231-241 s): it stood out of
            // the tank's sight, nobody could hit it ("the attack refused: no line of sight" from the tank and the
            // rogue), the tank stood ("leader in combat with attackers: no walk") until the game moved the mob onto
            // a member. A fight mob on the tank that stands (not walking in) out of the tank's sight is a mob the
            // tank goes to: at it (melee reach), where it sees it. A mob walking in (a LoS pull) is not touched
            else if (!alone)
            {
                Unit* stuck = nullptr;
                for (Unit* attacker : tank->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker->GetVictim() == tank && attacker->ToCreature() &&
                        LeaderFightMob(tank, attacker) && !attacker->isMoving() && !IsCrowdControlled(attacker) &&
                        !tank->IsWithinLOSInMap(attacker) &&
                        (!stuck || attacker->GetExactDist(tank) < stuck->GetExactDist(tank)))
                        stuck = attacker;
                if (stuck && checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, stuck->GetPosition(),
                                         NOMINAL_MELEE_RANGE, "the tank goes to its mob out of its sight"))
                    Decisions().Write(run, "leader", "fight_spot_to_mob", stuck->GetGUID(), stuck->GetName(), true,
                                      JsonObject().Str("mob", stuck->GetName())
                                          .Real("distance", stuck->GetExactDist(tank))
                                          .Real("dz", stuck->GetPositionZ() - tank->GetPositionZ()));
            }
        }
        // [lead29] member -> tank distance while the tank walks out of combat (run summary: max / median)
        if (!fighting && tank->isMoving())
            for (Player* bot : seats)
            {
                std::vector<float>& samples = lead.walkDistances[bot->GetName()];
                if (samples.size() < 4000)
                    samples.push_back(bot->GetExactDist(tank));
            }
        // [lead274] a critical moment of this fight: the pull over the leader's own estimate (overload >= 1), or a
        // member in danger (the tank below LEADER_PRESAVE_TANK_PCT, another member under half health: the lines the
        // leader's saves already use); empty: none
        std::string critical;
        if (fighting)
        {
            if (lead.fightOverload >= 1.0f)
                critical = Acore::StringFormat("a critical pull: overload {:.2f} ({})", lead.fightOverload,
                                               lead.fightJudged);
            else if (tank->IsAlive() && tank->GetHealthPct() < Leader::LEADER_PRESAVE_TANK_PCT)
                critical = Acore::StringFormat("the tank in danger at {:.0f} %", tank->GetHealthPct());
            else
                for (Player* bot : seats)
                    if (bot->IsAlive() && bot->GetHealthPct() < LEADER_DANGER_HEALTH_PCT)
                    {
                        critical = Acore::StringFormat("{} in danger at {:.0f} %", bot->GetName(), bot->GetHealthPct());
                        break;
                    }
            // [lead903] user 2026-11-03 (36): a healer out of mana (under the line the fight's seconds left need, its
            // own mana cooldowns and the potion credited) is a critical moment too: the cooldowns that save the fight
            // are not held for a boss
            if (critical.empty())
            {
                std::vector<Player*> everyone = seats;
                everyone.push_back(tank);
                critical = Leader::Support::HealerOutOfMana(everyone, lead.fightSecLeft);
                if (!critical.empty())
                {
                    thread_local std::set<uint64> dryLogged;
                    if (dryLogged.insert(uint64(lead.fightStartMs) << 8 ^ tank->GetGUID().GetCounter()).second)
                        LOG_INFO("playerbots", "[lead903] critical: {}", critical);
                }
            }
        }
        Creature* fightBoss = target && target->ToCreature() && target->ToCreature()->IsDungeonBoss()
                                  ? target->ToCreature()
                                  : nullptr;
        // [lead541] b212 rfc-h 5C 1008 s: the pull of a Searing Blade Cultist "brings Jergosh the Invoker (the plan
        // counts him)"; with the Cultist the tank's target the verdict judged trash ("back before the pull of
        // Bazzalan") - no Rapid Fire / Readiness on Jergosh, the cooldowns went by the trash rule. A dungeon boss that
        // fights the group (its victim a member, or engaged by one) makes it a boss fight
        if (!fightBoss && fighting)
            for (ObjectGuid const& bossGuid : group.BossCreatures())
            {
                Creature* boss = map->GetCreature(bossGuid);
                if (!boss || !boss->IsAlive() || !boss->IsInCombat() || !boss->IsDungeonBoss())
                    continue;
                if (BossFightsGroup(boss, tank, seats))  // [lead772]
                {
                    fightBoss = boss;
                    break;
                }
            }
        // [lead421] the seconds to a later boss's pull: the next boss, the route's estimate (bossPullSec); one after
        // him: that, his fight and a stretch between bosses as this run measured them (their mean; none yet: the
        // straight walk from him); without an estimate this fight's rest and the straight walk to him
        auto const secToBoss = [&](Creature* later) -> float
        {
            float toHim = lead.fightSecLeft + Leader::WalkYd(tank, later->GetPosition()) /  // [lead804]
                                                     std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
            if (later->GetGUID() == lead.nextBossGuid && lead.bossPullSec >= 0.0f)
                toHim = lead.bossPullSec;
            else if (Creature* next = lead.nextBossGuid.IsEmpty() ? nullptr : map->GetCreature(lead.nextBossGuid);
                     next && lead.bossPullSec >= 0.0f)
            {
                auto const nextNeed = lead.bossNeeds.find(lead.nextBossGuid);
                float const stretch = lead.stretches ? lead.stretchSecSum / float(lead.stretches)
                                                     : next->GetExactDist(later) /
                                                           std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
                toHim = lead.bossPullSec + (nextNeed != lead.bossNeeds.end() ? nextNeed->second.fightSec : 0.0f) +
                        stretch;
            }
            return toHim;
        };
        // [lead274] one cooldown's verdict by its recharge: true allowed, `why` the reason either way
        // [lead581] `effectSec`: how long its effect lasts once cast (the spell's duration)
        // [lead854] `spentEffect`: its recharge starts only once its effect is spent (Combustion: its 3 crits),
        // `effectSec` the fight seconds that takes; `valuePerSec`: the damage a second of its effect adds (0: the
        // group's dps)
        auto const cooldownVerdict = [&](float rechargeSec, float effectSec, std::string& why, bool spentEffect = false,
                                         float valuePerSec = 0.0f) -> bool
        {
            if (!critical.empty())
            {
                why = critical;
                return true;
            }
            if (!fightBoss)
            {
                if (lead.bossPullSec < 0.0f)
                {
                    why = "trash, no boss left";
                    return true;
                }
                // [lead771] b220 rfc-h 5F 935.2 s: Vanish "trash: back before the pull of Bazzalan (his pull in
                // 251 s, recharge 180 s)" in the fight's tail after Jergosh - the route had no plan to Bazzalan yet
                // (its first plan came the same second, 7 stops 297 yd) and the stretch floor (lead513: the run's
                // mean stretch between bosses, 278 / 383 / 330 s before) stood for the estimate; he was pulled 155 s
                // later (5B 1001.4 s: floor 287 s, pulled 158 s later). The floor is a guess of the boss fight's gain
                // plan; "back before" on trash needs the estimate the leader can account for - its parts (this
                // fight, the trash it knows of, the rest, the walk)
                float const pullSec = lead.bossPullPartsSec >= 0.0f ? lead.bossPullPartsSec : lead.bossPullSec;
                // [lead854] b223 rfc-h 5C: Combustion "trash: back before the pull of Oggleflint (his pull in 169 s,
                // recharge 120 s)" at 161.9 s - its effect ran until its 3rd fire crit at 233 s across the trash
                // pulls (the game starts its cooldown then), it was back at 353 s and Oggleflint (289 s) had none;
                // Taragaman the same (cast 513 s, spent 637 s, he was pulled at 669 s). Such a cooldown is back
                // before the boss only when this fight spends its effect and the recharge from then fits
                if (spentEffect && effectSec > lead.fightSecLeft)
                {
                    why = Acore::StringFormat("held for {}: its effect runs until spent ({:.0f} s of its casts > the "
                                              "fight's {:.0f} s left), its recharge {:.0f} s only from then (his pull "
                                              "in {:.0f} s)",
                                              lead.nextBossName, effectSec, lead.fightSecLeft, rechargeSec, pullSec);
                    return false;
                }
                float const backSec = rechargeSec + (spentEffect ? effectSec : 0.0f);
                bool const back = pullSec >= backSec;
                why = Acore::StringFormat("{} {} (his pull in {:.0f} s, recharge {:.0f} s)",
                                          back ? "trash: back before the pull of" : "held for", lead.nextBossName,
                                          pullSec, backSec);
                return back;
            }
            // [lead700] b218 rfc-h: Heroism (Sated 600 s) went to one boss a run in every shaman comp - 5B held it on
            // Oggleflint (408 s) "for Bazzalan (needs it more ...; his pull in >= 236 s, recharge 600 s)", Bazzalan
            // was pulled 794 s later and both would have had it; 5A held it on Oggleflint for Bazzalan (pulled 691 s
            // later). The pairwise rule held it for any boss "that needs it more" judged within its recharge, and a
            // boss beyond the next one was timed as the next one's pull, his fight and one stretch (the straight walk
            // before any was measured). Now the bosses left are put in their order (the next one, then each nearest
            // to the one before) and timed along it - the next one's pull estimate, then each one's fight and a
            // stretch (the run's measured mean, else the straight walk between them); every set of uses that fits
            // the recharge in that order (each use at his pull or when it is back, in his fight) is weighed by its
            // gain - the group's dps over the fight seconds the effect covers, the fights' lengths x the group's dps
            // between equal gains - and it is cast here when the best set has this fight in it
            float const dps = valuePerSec > 0.0f ? valuePerSec : std::max(lead.groupDps, 1.0f);  // [lead854]
            float const speed = std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
            struct Use
            {
                std::string name;
                float pullSec;  // seconds from now to his pull (0: this fight)
                float fightSec;
            };
            std::vector<Use> uses{{fightBoss->GetName(), 0.0f, std::max(lead.fightSecLeft, 0.0f)}};
            // [lead852] b223 rfc-h 5D Oggleflint 259.7-273.0 s: Army of the Dead "allowed" (here, Taragaman,
            // Bazzalan: 81352) then "held for Taragaman, Bazzalan, Jergosh (81352)" - the same gain, and the tie went
            // by the fights' lengths with this one counted by its seconds left: 66 + 97 + 70 > 97 + 70 + 64 at
            // 259.7 s, 63.97 + ... < ... at 272.4 s. The fight is as long as it is: its whole length (fought + left)
            float const foughtSec = lead.fightStartMs ? getMSTimeDiff(lead.fightStartMs, now) / 1000.0f : 0.0f;
            std::vector<Creature*> left;
            for (auto const& [bossGuid, need] : lead.bossNeeds)
                if (Creature* later = bossGuid == fightBoss->GetGUID() ? nullptr : map->GetCreature(bossGuid);
                    later && later->IsAlive() && !later->IsInCombat())
                    left.push_back(later);
            Creature* prev = nullptr;
            float at = 0.0f;
            while (!left.empty())
            {
                auto pick = left.begin();
                if (!prev)
                {
                    auto const next = std::find_if(left.begin(), left.end(), [&](Creature* boss)
                                                   { return boss->GetGUID() == lead.nextBossGuid; });
                    if (next != left.end())
                        pick = next;
                    else
                        pick = std::min_element(left.begin(), left.end(), [&](Creature* a, Creature* b)
                                                { return tank->GetExactDist(a) < tank->GetExactDist(b); });
                }
                else
                    pick = std::min_element(left.begin(), left.end(), [&](Creature* a, Creature* b)
                                            { return prev->GetExactDist(a) < prev->GetExactDist(b); });
                Creature* boss = *pick;
                left.erase(pick);
                if (!prev)
                    at = boss->GetGUID() == lead.nextBossGuid && lead.bossPullSec >= 0.0f ? lead.bossPullSec
                                                                                          : secToBoss(boss);
                else
                    at += uses.back().fightSec +
                          (lead.stretches ? lead.stretchSecSum / float(lead.stretches)
                                          : Leader::WalkYd(tank, prev->GetPosition(), boss->GetPosition()) / speed);
                auto const need = lead.bossNeeds.find(boss->GetGUID());
                uses.push_back({boss->GetName(), at, need != lead.bossNeeds.end() ? need->second.fightSec : 0.0f});
                prev = boss;
            }
            // every subset in the bosses' order (this fight and at most the next 9 bosses)
            size_t const n = std::min<size_t>(uses.size(), 10);
            uint32 best = 0;
            float bestGain = -1.0f;
            float bestLength = -1.0f;
            for (uint32 set = 1; set < (1u << n); ++set)
            {
                float ready = 0.0f;
                float gain = 0.0f;
                float length = 0.0f;
                bool fits = true;
                for (size_t i = 0; i < n && fits; ++i)
                    if (set & (1u << i))
                    {
                        float const cast = std::max(uses[i].pullSec, ready);
                        float const fightLeft = uses[i].pullSec + uses[i].fightSec - cast;
                        // an effect with no duration (an instant) gains the same anywhere in a fight
                        float const covered = effectSec > 0.0f ? std::min(effectSec, fightLeft) : 1.0f;
                        fits = fightLeft > 0.0f && covered > 0.0f;
                        gain += dps * covered;
                        length += dps * (uses[i].fightSec + (i ? 0.0f : foughtSec));  // [lead852]
                        ready = cast + rechargeSec + (spentEffect ? effectSec : 0.0f);  // [lead854]
                    }
                // equal gains: the longer fights, then the set that casts here (lower bit set first)
                if (fits && (gain > bestGain || (gain == bestGain && length > bestLength) ||
                             (gain == bestGain && length == bestLength && (set & 1u) && !(best & 1u))))
                {
                    best = set;
                    bestGain = gain;
                    bestLength = length;
                }
            }
            std::string plan;
            for (size_t i = 1; i < n; ++i)
                if (best & (1u << i))
                    plan += Acore::StringFormat("{}{} (pull in ~{:.0f} s, {:.0f} s fight)", plan.empty() ? "" : ", ",
                                                uses[i].name, uses[i].pullSec, uses[i].fightSec);
            if (!(best & 1u))
            {
                why = Acore::StringFormat("held for {} (the most gain over the bosses left without it here: {:.0f} "
                                          "damage; this fight {:.0f} s left, recharge {:.0f} s, effect {:.0f} s)",
                                          plan.empty() ? std::string("a later boss") : plan, bestGain,
                                          uses[0].fightSec, rechargeSec, effectSec);
                return false;
            }
            why = Acore::StringFormat("the most gain over the bosses left: here ({:.0f} s left){}{} - {:.0f} damage "
                                      "(recharge {:.0f} s, effect {:.0f} s)",
                                      uses[0].fightSec, plan.empty() ? "" : ", then ", plan, bestGain, rechargeSec,
                                      effectSec);
            return true;
        };
        // [lead178] [lead274] one member's burn cooldowns, each one ready, by its verdict
        // [lead853] b223 rfc-h 5E Oggleflint 355.8 s: Rapid Fire / Bestial Wrath / Avenging Wrath / Cold Blood
        // allowed while he ran in 27 yd off the tank (boss_engaged 357 s), 5C 288.2 s Mirror Image the same - their
        // seconds ran before anyone could hit him. A boss fight's burn cooldowns start at the engage, the tank's
        // first threat on the boss (the wowsims APL's own pre-pull casts - the DK's Army, the cat's Berserk at -1 s,
        // the rogue's Stealth, the pre-pull potion - are the leader's prepull steps, not these); a critical moment
        // is not held
        bool const bossEngaged = !fightBoss || !critical.empty() ||
                                 (tank->IsAlive() ? fightBoss->GetThreatMgr().GetThreat(tank) > 0.0f
                                                  : fightBoss->IsInCombat());
        auto const burnVerdict = [&](Player* who, PlayerbotAI* whoAI, std::vector<std::string>& allowedOut,
                                     std::string& allowedNote, std::string& heldNote)
        {
            // [lead852] one verdict per fact: a cooldown allowed in this boss fight stays allowed in it (b223 5D
            // Oggleflint: Army of the Dead allowed / held / allowed as the bosses' pull estimates moved a second)
            auto& kept = lead.burnKept[who->GetGUID()];
            for (auto entry = kept.begin(); entry != kept.end();)
                entry = !fightBoss || entry->second != fightBoss->GetGUID() ? kept.erase(entry) : std::next(entry);
            auto const judged = [&](std::string const& name, std::string const& why, bool allowed)
            {
                if (allowed)
                {
                    allowedOut.push_back(name);
                    if (fightBoss)
                        kept[name] = fightBoss->GetGUID();
                }
                std::string& note = allowed ? allowedNote : heldNote;
                note += Acore::StringFormat("{}{}: {}", note.empty() ? "" : "; ", name, why);
            };
            auto const keptOrEngage = [&](std::string const& name) -> bool
            {
                thread_local std::set<std::tuple<uint64, std::string, uint64>> logged;  // one log per fact
                if (kept.count(name))
                {
                    if (logged.insert({who->GetGUID().GetRawValue(), "kept " + name,
                                       fightBoss->GetGUID().GetRawValue()})
                            .second)
                        LOG_INFO("playerbots", "[lead852] {}: {} stays allowed in the fight with {}", who->GetName(),
                                 name, fightBoss->GetName());
                    judged(name, "kept: allowed in this boss fight already", true);
                    return true;
                }
                if (!bossEngaged)
                {
                    if (logged.insert({who->GetGUID().GetRawValue(), "engage " + name,
                                       fightBoss->GetGUID().GetRawValue()})
                            .second)
                        LOG_INFO("playerbots", "[lead853] {}: {} held until {} is engaged by the tank", who->GetName(),
                                 name, fightBoss->GetName());
                    judged(name,
                           Acore::StringFormat("held until {} is engaged (the tank's first threat on him)",
                                               fightBoss->GetName()),
                           false);
                    return true;
                }
                return false;
            };
            for (char const* burn : Leader::BURN_COOLDOWNS)
            {
                std::string const name(burn);
                // [lead273] wowsims casts Army of the Dead only before a boss pull (Blood, Frost) or on the boss
                // under the Gargoyle (Unholy): never a trash burn, it is kept for the boss prepull (Leader::Prepull)
                if (!fightBoss && name == "army of the dead")
                    continue;
                // Heroism / Bloodlust is the leader's own command (3c), judged there by the same verdict
                if (name == "heroism" || name == "bloodlust")
                    continue;
                // [lead410] Unholy's Raise Dead is its permanent pet's summon (Master of Ghouls), no fight cooldown
                if (name == "raise dead" && who->HasAura(LeaderCommand::SPELL_MASTER_OF_GHOULS))
                    continue;
                uint32 const spell = whoAI->GetAiObjectContext()->GetValue<uint32>("spell id", burn)->Get();
                SpellInfo const* info = spell && who->HasSpell(spell) && !who->HasSpellCooldown(spell)
                                            ? sSpellMgr->GetSpellInfo(spell)
                                            : nullptr;
                if (!info)
                    continue;
                if (keptOrEngage(name))  // [lead852] [lead853]
                    continue;
                float const rechargeSec = RechargeAfterCastSec(who, spell);  // [lead541] the game's, not the base
                // [lead812] b221 rfc-h 5G: the blood tank's Raise Dead was judged "effect 0 s" (46584 has no duration:
                // the core's spell_dk_raise_dead casts the guardian named by its effect 1, 52150, a 60 s ghoul) - an
                // instant's gain, the same anywhere, so the verdict spread it by the tie-break and it sat out
                // Oggleflint and Jergosh (wowsims raises it every fight, ~280 Claw dps). The effect of a spell with no
                // duration of its own is its summon's / triggered spell's duration (spell data)
                float effectSec = std::max(info->GetMaxDuration(), 0) / 1000.0f;
                if (effectSec <= 0.0f)
                {
                    for (SpellEffectInfo const& effect : info->GetEffects())
                        if (SpellInfo const* next = effect.TriggerSpell ? sSpellMgr->GetSpellInfo(effect.TriggerSpell)
                                                                        : nullptr)
                            effectSec = std::max(effectSec, std::max(next->GetMaxDuration(), 0) / 1000.0f);
                    if (name == "raise dead" && info->GetEffect(EFFECT_1).Effect)
                        if (SpellInfo const* ghoul =
                                sSpellMgr->GetSpellInfo(uint32(std::max(info->GetEffect(EFFECT_1).CalcValue(), 0))))
                            effectSec = std::max(effectSec, std::max(ghoul->GetMaxDuration(), 0) / 1000.0f);
                    if (effectSec > 0.0f)
                    {
                        thread_local std::set<uint32> effectLogged;
                        if (effectLogged.insert(spell).second)
                            LOG_INFO("playerbots", "[lead812] {}: {} judged with its summon's {:.0f} s effect",
                                     who->GetName(), name, effectSec);
                    }
                }
                // [lead854] an effect spent by its crits (Combustion): its seconds and its damage computed
                float valuePerSec = 0.0f;
                uint32 const filler = whoAI->GetAiObjectContext()->GetValue<uint32>("spell id", "fireball")->Get();
                bool const spentEffect = ChargeCritEffect(who, info, filler, effectSec, valuePerSec);
                if (spentEffect)
                {
                    thread_local std::set<std::pair<uint64, uint32>> effectLogged;
                    if (effectLogged.insert({who->GetGUID().GetRawValue(), spell}).second)
                        LOG_INFO("playerbots", "[lead854] {}: {} judged by its crit chain: {:.1f} s, {:.0f} damage",
                                 who->GetName(), name, effectSec, valuePerSec * effectSec);
                }
                std::string why;
                bool const allowed = cooldownVerdict(rechargeSec, effectSec, why, spentEffect, valuePerSec);
                judged(name, why, allowed);
            }
            // [lead512] b210 rfc-h 5C Rtdruab / 5G Rtpalae: their on-use trinkets (Tome of Arcane Phenomena, 2 min)
            // went on trash under the bot's own "generic boost" and were ready only at Oggleflint. An equipped
            // offensive on-use trinket that is ready is judged like the burn cooldowns, by its recharge (the item's
            // cooldown), and allowed by its name (UseTrinketAction reads Leader::BurnAllows)
            for (uint8 slot : {uint8(EQUIPMENT_SLOT_TRINKET1), uint8(EQUIPMENT_SLOT_TRINKET2)})
            {
                Item const* trinket = who->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                uint32 spell = 0;
                float rechargeSec = 0.0f;
                if (!Leader::BoostTrinket(trinket, spell, rechargeSec) || who->HasSpellCooldown(spell))
                    continue;
                std::string const name = trinket->GetTemplate()->Name1;
                if (keptOrEngage(name))  // [lead852] [lead853]
                    continue;
                std::string why;
                SpellInfo const* use = sSpellMgr->GetSpellInfo(spell);
                bool const allowed =
                    cooldownVerdict(rechargeSec, use ? std::max(use->GetMaxDuration(), 0) / 1000.0f : 0.0f, why);
                judged(name, why, allowed);
            }
        };
        // [lead187] b180: the blood DK tank cast no Raise Dead / Army of the Dead (sim 1.3 / min each) - its DK
        // cooldowns run only under the leader's Burn (LeaderBurnRule) and the leader gave the tank none. The tank's
        // Intensity by the same rule: a boss fight burns, trash the cooldowns the boss does not need
        if (fighting && Leader::LEADER_ATTACK_EVENTS && tank->IsAlive())
            if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank))
            {
                std::vector<std::string> tankAllowed;
                std::string tankAllowedNote;
                std::string tankHeldNote;
                burnVerdict(tank, tankAI, tankAllowed, tankAllowedNote, tankHeldNote);  // [lead274] bosses too
                Leader::Intensity const tankIntensity =
                    !tankAllowed.empty() ? Leader::Intensity::Burn : Leader::Intensity::Normal;
                LogBurnDecision(run, lead, tank, "cooldowns", tankAllowedNote, tankHeldNote, fightBoss != nullptr);
                if (Leader::Events::SetIntensity(tank->GetGUID(), tankIntensity, tankAllowed) &&
                    tankIntensity == Leader::Intensity::Burn)
                    logEvent(tank, "Intensity",
                             JsonObject().Str("intensity", "burn").Str("who", "tank").Str("why", tankAllowedNote));
            }
        // [lead21] slabs-h 20261026-195203: the tank executing a leader PullOrder out of combat had no fresh command -
        // rot3's stale mode ("keeps to its current target, no area spell") vetoed its pull: 17 orders "nothing came".
        // While an order runs, its target is the tank's fresh Attack, an area pull allowed on that pack
        if (!fighting && Leader::LEADER_TANK_EXECUTES)
        {
            Leader::PullOrder order;
            if (Leader::Orders::GetOpen(group.LeaderGuid(), order) && !order.target.IsEmpty())
                if (Unit* pullMob = ObjectAccessor::GetUnit(*tank, order.target); pullMob && pullMob->IsAlive())
                {
                    bool const changed = Leader::Events::SetAttack(tank->GetGUID(), Leader::AttackEvent::Attack,
                                                                   order.target, {order.target});
                    Leader::Events::SetAoe(tank->GetGUID(), Leader::AoeRule::AllowedOnPack, pullMob->GetPosition(),
                                           LEADER_AOE_RADIUS);
                    if (changed)
                        logEvent(tank, "Attack", JsonObject().Str("target", pullMob->GetName()).Str("who", "tank")
                                                     .Str("why", "the leader's pull order runs"));
                }
        }
        // [lead270] the prepull Army of the Dead, timed into the boss's preparation (replaces lead16's 4.5 s
        // LEADER_BOSS_PREPARE wait: an elapsed-time rule, and the tank never channelled under it)
        Leader::Prepull::TickArmy(group, tank, seats, fighting, logEvent);

        // [lead16] the hazard scan (slow layer, every LEADER_HAZARD_SCAN_MS); membership is checked every tick
        if (Leader::LEADER_HAZARD_SPOTS &&
            (!lead.hazardScanMs || getMSTimeDiff(lead.hazardScanMs, now) >= LEADER_HAZARD_SCAN_MS))
        {
            lead.hazardScanMs = now;
            lead.hazardZones.clear();
            // [lead820] the hazard casts seen are noted for their cadence (the healer's mana over every hit to come)
            for (Player* bot : seats)
                slow([&]
                     {
                         LeaderCollectHazards(bot, lead.hazardZones[bot->GetGUID()]);
                         if (fighting)
                             Leader::HazardVerdict::NoteCasts(lead.hazardCasts, bot, lead.hazardZones[bot->GetGUID()],
                                                              now);
                         return true;
                     });
            if (tank->IsAlive())  // [lead17] the tank's hazards too
                slow([&]
                     {
                         LeaderCollectHazards(tank, lead.hazardZones[tank->GetGUID()]);
                         if (fighting)
                             Leader::HazardVerdict::NoteCasts(lead.hazardCasts, tank, lead.hazardZones[tank->GetGUID()],
                                                              now);
                         return true;
                     });
        }

        // 3b. interrupts: one kicker per casting mob, the member with an interrupt ready (kept while it holds).
        // [lead460] decided before the members' moves: the hazard step below reads it
        std::set<ObjectGuid> kicked;  // the casters whose cast a kicker in reach of its interrupt stops this tick
        if (fighting && Leader::LEADER_ATTACK_EVENTS)
        {
            std::set<ObjectGuid> busy;
            for (Unit* caster : casters)
            {
                Player* kicker = nullptr;
                if (auto const held = lead.kickers.find(caster->GetGUID()); held != lead.kickers.end())
                    if (Player* bot = ObjectAccessor::FindPlayer(held->second);
                        bot && bot->IsAlive() && LeaderReadyInterrupt(bot) && !LeaderChannelsManaTool(bot))
                        kicker = bot;
                bool const kept = kicker != nullptr;  // the kicker kept while it holds (no switch mid-cast)
                // [lead776] b220 rfc-h 5E Taragaman: the rogue's Kick on every Fire Nova (714.5 - 763.5 s, 5 in the
                // 62 s fight, 25 energy each = 121 energy a minute). Fire Nova (11970: 2 s cast, 10 yd round him,
                // 100-112 fire x the map's spell factor 30 = ~3.2k on the tank and each melee) is worth a kick - not
                // kicked it lands on two or three members or walks the melee out 3-4 s ("hazard: area cast") - but
                // the kicker is the one it costs least: an interrupt off the global cooldown paid in mana (Wind
                // Shear, Counterspell, Silencing Shot) before one paid in energy / rage / runic power (Kick, Pummel,
                // Mind Freeze), one on the global cooldown last; the seats' order among equals
                uint8 kickerRank = 0;
                for (Player* bot : seats)
                {
                    if (kicker && kept)
                        break;
                    // [lead90] b131 946.1 s: Rtmagaa's Counterspell on a Searing Blade Warlock the tank had no threat
                    // on - a kicker only while it has threat headroom on the caster (its threat below the tank's)
                    // [lead840] b223 rfc-h 5C Taragaman 676.3 s: the fire mage was told the kick 4.5 s into its
                    // Evocation (the boss line's 60 %): the Counterspell breaks the channel. A member channelling a
                    // mana tool is no kicker (the channel is the mana its line counted)
                    if (bot != healer && !busy.count(bot->GetGUID()) && LeaderReadyInterrupt(bot) &&
                        !LeaderChannelsManaTool(bot) && bot->GetExactDist(caster) <= 30.0f &&
                        caster->GetThreatMgr().GetThreat(bot) < caster->GetThreatMgr().GetThreat(tank))
                        if (uint8 const rank = LeaderInterruptCostRank(bot); !kicker || rank < kickerRank)
                        {
                            kicker = bot;
                            kickerRank = rank;
                        }
                }
                if (!kicker || busy.count(kicker->GetGUID()))
                    continue;
                busy.insert(kicker->GetGUID());
                lead.kickers[caster->GetGUID()] = kicker->GetGUID();
                if (LeaderInterruptReaches(kicker, caster))
                    kicked.insert(caster->GetGUID());
                if (Leader::Events::SetInterrupt(kicker->GetGUID(), caster->GetGUID()))
                    logEvent(kicker, "Interrupt", JsonObject().Str("caster", caster->GetName())
                                                      .Num("casterEntry", caster->GetEntry())
                                                      .Num("spell", LeaderReadyInterrupt(kicker)));
            }
        }
        else
            lead.kickers.clear();
        // [lead460] b207 rfc-h, all 8 runs: every "hazard: area cast of Taragaman the Hungerer" escape (42, 107 member
        // seconds; 5E's rogue 12 of its 60 s on the boss) came with the leader's Interrupt on the same cast, and every
        // one was kicked: the members walked out of a cast that never landed and back (the leader against itself).
        // A cast the leader's kicker stops from where it stands is no hazard; one nobody stops this tick stays one
        if (!kicked.empty())
            for (auto& [member, zones] : lead.hazardZones)
                zones.erase(std::remove_if(zones.begin(), zones.end(),
                                           [&kicked](HazardZone const& zone)
                                           { return !zone.source.IsEmpty() && kicked.count(zone.source); }),
                            zones.end());
        // [lead780] user rule (hazard-stay-or-leave): every hazard a member stands in is a computed stay or leave -
        // the damage it would not deal out of it (the walk out and back, the wait for the cast / area, its rotation)
        // against the damage it takes, and it stays only when it survives and the healers cover it. A zone it stays
        // in is no hazard for it this tick (no escape, its fight spot may lie in it). b220 rfc-h 5H Taragaman: the cat
        // walked out of Fire Nova twice (681.7 / 721.7 s, 3-4 s out of melee each, Savage Roar dropped)
        // [lead820] with the casts seen (their cadence) and the other stays' heals: the healer's mana over them all
        Leader::HazardVerdict::Facts const verdictFacts{&group, tank, &seats, target, lead.fightSecLeft,
                                                        lead.bossFightOn, fighting, &lead.hazardCasts,
                                                        &lead.hazardStayHeals, now};
        auto const judged = [&](Player* bot, std::string const& key, Leader::HazardVerdict::Verdict const& verdict,
                                std::set<std::string>& seen)
        {
            seen.insert(key);
            lead.hazardVerdictWhy[bot->GetGUID()] = verdict.why;
            // [lead820] the heal its stay asks (every hit to come), counted against the healer's mana by the others
            if (verdict.stays)
                lead.hazardStayHeals[bot->GetGUID()][key] = verdict.heal;
            else if (auto heals = lead.hazardStayHeals.find(bot->GetGUID()); heals != lead.hazardStayHeals.end())
                heals->second.erase(key);
            auto& stays = lead.hazardStays[bot->GetGUID()];
            if (auto const was = stays.find(key); was != stays.end() && was->second == verdict.stays)
                return;
            stays[key] = verdict.stays;
            Decisions().Write(run, "leader", "hazard_verdict", bot->GetGUID(), bot->GetName(), true,
                              JsonObject().Str("hazard", key).Str("verdict", verdict.stays ? "stays" : "leaves")
                                  .Str("why", verdict.why));
        };
        if (Leader::LEADER_HAZARD_STAY_OR_LEAVE)
            for (auto& [member, zones] : lead.hazardZones)
            {
                Player* bot = ObjectAccessor::FindPlayer(member);
                std::set<std::string> seen;
                if (bot && bot->IsAlive())
                    zones.erase(std::remove_if(zones.begin(), zones.end(),
                                               [&](HazardZone const& zone)
                                               {
                                                   if (std::fabs(zone.centre.GetPositionZ() - bot->GetPositionZ()) >
                                                           LEADER_HAZARD_DZ ||
                                                       zone.centre.GetExactDist2d(bot->GetPositionX(),
                                                                                  bot->GetPositionY()) >=
                                                           zone.radius + 0.5f)
                                                       return false;
                                                   Leader::HazardVerdict::Verdict const verdict =
                                                       Leader::HazardVerdict::JudgeZone(verdictFacts, bot, zone);
                                                   judged(bot, zone.what, verdict, seen);
                                                   return verdict.stays;
                                               }),
                                zones.end());
                // on a rescan (every zone back in the list): the hazards it no longer stands in are judged anew when
                // it does again
                if (auto stays = lead.hazardStays.find(member);
                    lead.hazardScanMs == now && stays != lead.hazardStays.end())
                    for (auto it = stays->second.begin(); it != stays->second.end();)
                    {
                        bool const cleave = it->first.rfind("cleave: ", 0) == 0;  // judged in the member's moves
                        if (seen.count(it->first) || (fighting && cleave))
                        {
                            ++it;
                            continue;
                        }
                        lead.hazardStayHeals[member].erase(it->first);  // [lead820] no stay, no heal asked
                        it = stays->second.erase(it);
                    }
            }
        if (!fighting)
        {
            lead.hazardCasts.clear();      // [lead820] the cadence measured is this fight's
            lead.hazardStayHeals.clear();  // [lead820]
            lead.combatSpots.clear();
            lead.combatSpotLeft.clear();  // [lead239] the fight's left spots end with it
            lead.tankGroundSpots.clear();  // [lead152]
            lead.tankGroundFor.clear();
            lead.combatSpotAnchor.clear();
            lead.combatSpotStoppedAt.clear();  // [lead397]
            lead.meleeSide.clear();            // [lead720]
            lead.chaseVerdicts.clear();        // [lead801]
            lead.sideHeldLogged.clear();       // [lead800]
            lead.chaseHeldLogged.clear();      // [lead801]
            lead.walkEndHeldLogged.clear();    // [lead802]
            lead.fightAnchorSerial = 0;
            lead.fightAnchorAhead = false;  // [lead466]
            lead.healReachSpots.clear();  // [lead120] the next fight's heal reach is decided anew
            lead.healReachAnchor = 0;
            lead.healReachFrom.reset();
            lead.rangeSpots.clear();  // [lead122]
        }
        else
        {
            // [lead466] user 2026-11-02 (Q16): the fight is planned where the tank will stand. At the fight's start
            // the tank may still walk or charge to its stop point - the leader's meeting point of the tank and the
            // pulled mob (an engage: at the mob, on the tank's side at the two combat reaches, where its charge
            // lands; a pull back / range pull: the fight spot the mob comes to). The anchor is that point while the
            // tank goes there; it arrives (within LEADER_ANCHOR_MOVE_YD) or stops elsewhere: the anchor's own rule
            // [lead490] b209 rfc-h: 48 of the 77 combat_spot_none came within 15 s of a fight_anchor_ahead set on
            // another mob than the fight's first attacker, 25 of them 46-104 yd off the tank (5A 95.5 s: 63 yd) - the
            // walk-in woke another mob (walk_in_wakes "taken along"), it attacked the tank 15-20 yd off and became the
            // fight's target, while the anchor was set at the stop of the commit's mob ahead. Every healer / ranged
            // spot was judged against that far point ("heal reach" x45-49), the melee ones too. The tank walks to the
            // commit's stop only while that mob is the fight's target
            if (!lead.fightAnchorSerial && lead.commit && lead.commitIssued && !lead.commitMob.IsEmpty() && target &&
                target->GetGUID() == lead.commitMob)
            {
                Position stop = lead.meetPoint;
                if (lead.commit->kind == "engage")
                    if (Unit* mob = ObjectAccessor::GetUnit(*tank, lead.commitMob); mob && mob->IsAlive())
                    {
                        float const dx = tank->GetPositionX() - mob->GetPositionX();
                        float const dy = tank->GetPositionY() - mob->GetPositionY();
                        float const length = std::max(0.1f, std::hypot(dx, dy));
                        float const contact = std::min(length, mob->GetCombatReach() + tank->GetCombatReach());
                        stop = Position(mob->GetPositionX() + dx / length * contact,
                                        mob->GetPositionY() + dy / length * contact, mob->GetPositionZ());
                    }
                if (tank->isMoving() && tank->GetExactDist(stop) > Leader::LEADER_ANCHOR_MOVE_YD &&
                    Acore::IsValidMapCoord(stop.GetPositionX(), stop.GetPositionY(), stop.GetPositionZ()))
                {
                    lead.fightAnchor = stop;
                    lead.fightAnchorAhead = true;
                    ++lead.fightAnchorSerial;
                    Decisions().Write(run, "leader", "fight_anchor_ahead", tank->GetGUID(), tank->GetName(), true,
                                      JsonObject().Str("kind", lead.commit->kind)
                                          .Real("x", stop.GetPositionX()).Real("y", stop.GetPositionY())
                                          .Real("z", stop.GetPositionZ()).Real("tankGap", tank->GetExactDist(stop)));
                }
            }
            bool const arrived = tank->GetExactDist(lead.fightAnchor) <= Leader::LEADER_ANCHOR_MOVE_YD;
            if (lead.fightAnchorAhead &&
                (arrived || !tank->isMoving() || !target || target->GetGUID() != lead.commitMob))  // [lead490]
                lead.fightAnchorAhead = false;
            if (!lead.fightAnchorAhead &&
                (!lead.fightAnchorSerial || tank->GetExactDist(lead.fightAnchor) > Leader::LEADER_ANCHOR_MOVE_YD))
            {
                // [lead18] the fight spot: where the tank holds the mobs; a new anchor re-spots every member once
                lead.fightAnchor = tank->GetPosition();
                ++lead.fightAnchorSerial;
            }
        }

        // [lead17] LEADER_ONE_GATHER_OWNER: who the group waits for - a member without a leader spot farther than
        // LEADER_TRAIL_YD from the tank (back under LEADER_TRAIL_BACK_YD); the harness's far gate reads this set
        if (Leader::LEADER_ONE_GATHER_OWNER)
            for (Player* bot : seats)
            {
                ObjectGuid const guid = bot->GetGUID();
                // [lead24d] a member whose leader walk was just rejected (it would wake a mob) stays: not trailing
                auto const rejected = lead.spotRejectLogMs.find(guid);
                bool const walkRejected = rejected != lead.spotRejectLogMs.end() && rejected->second &&
                                          getMSTimeDiff(rejected->second, now) < 5 * IN_MILLISECONDS;
                // [lead119] b153 rfc-h 90.6-158 s: the four members the pull walk held (Hold "the tank takes the mob
                // first") trailed at 36 yd as the tank walked in, the far gate held the tank for them: a deadlock. A
                // member the pull walk holds stands where the leader put it - placed, not trailing
                // [lead710] b219 rfc-h 5B 567.8-807 s (stalled): Rtshaac's combat spot (lead660's spell reach) ended
                // the fight 35 yd from the tank, its Follow back was refused ("the walk would wake Searing Blade
                // Cultist") and the leader made it stay there (group_blocked, lead77: the Cultist the next pull) - and
                // marked it trailing, so the Cultist's pull held "Rtshaac trails the group" for the member only that
                // pull frees: a wait nobody ended (5E 630.8-750.9 s: Rthunab, the same Cultist). A member the leader
                // holds where it stands because its walk is blocked is placed by the leader: the block's pull frees it
                // [lead730] a member walking to / waiting at the pull's stop point (melee) or its range spot of the
                // engage (ranged) is placed there by the leader (it may stand ahead of the walking tank)
                Leader::MemberEvents waits;
                bool const atStopPoint = Leader::Events::Get(guid, waits) &&
                                         (waits.moveWhy.rfind("to the tank's stop point", 0) == 0 ||
                                          waits.moveWhy.rfind("along with the tank: to range of the engage", 0) == 0);
                bool const placed = lead.spots.count(guid) || lead.hazardSpots.count(guid) ||
                                    lead.combatSpots.count(guid) || lead.pullWalkSpots.count(guid) || walkRejected ||
                                    lead.groupBlockedBy.count(guid) || atStopPoint;
                float const gap = bot->GetExactDist(tank);
                bool const was = lead.trailing.count(guid) > 0;
                bool const trails = !fighting && !placed &&
                                    (was ? gap > Leader::LEADER_TRAIL_BACK_YD : gap > Leader::LEADER_TRAIL_YD);
                if (trails != was)
                {
                    if (trails)
                        lead.trailing.insert(guid);
                    else
                        lead.trailing.erase(guid);
                    Decisions().Write(run, "leader", "leader_trail", guid, bot->GetName(), true,
                                                  JsonObject().Str("member", bot->GetName())
                                                      .Str("state", trails ? "waits for it" : "with the group")
                                                      .Real("gap", gap));
                }
            }

        // [lead45] the tank rests too (it is no seat): the boss pull waits for its mana, so the leader tells it to
        // drink (its own rest line / the boss top-up line, RestState) while no order runs
        if (!fighting)
        {
            lead.healStarted = false;  // [lead61] the next fight's heals start on the leader's word again
            lead.burnLogged.clear();   // [lead80]
        }
        // [lead67] the tank's threat lead at 2 s and 5 s into each fight: per fight mob the tank's threat over the
        // top other's (threat_lead) - how well the tank picks the pack up
        // [lead67] user metric: threat per second per member over the pull (the highest threat it held on each mob,
        // sampled at 2 s, 5 s and every 5 s after - not every tick), the tank's per mob, and the margin (tank over the
        // top non-tank) min / median at 2 s, 5 s and the last sample: threat_tps at the fight's end
        auto const margins = [](std::vector<float> values)
        {
            std::sort(values.begin(), values.end());
            return values.empty() ? std::make_pair(-1.0f, -1.0f)
                                  : std::make_pair(values.front(), values[values.size() / 2]);
        };
        if (!fighting && lead.fightStartMs)
        {
            float const sec = std::max(getMSTimeDiff(lead.fightStartMs, now) / 1000.0f, 1.0f);
            std::map<std::string, float> perMember;
            JsonArray tankMobs;
            for (auto const& [mob, byMember] : lead.threatMax)
                for (auto const& [who, threat] : byMember)
                {
                    perMember[who] += threat;
                    if (who == tank->GetName())
                        tankMobs.Raw(JsonObject().Num("low", mob.GetCounter()).Real("tps", threat / sec).Build());
                }
            JsonObject members;
            for (auto const& [who, threat] : perMember)
                members.Real(who.c_str(), threat / sec);
            JsonObject marginsJson;
            for (auto const& [mark, values] : lead.threatMargins)
            {
                auto const [low, median] = margins(values);
                std::string const key = mark == 99000 ? std::string("end") : std::to_string(mark / 1000) + "s";
                marginsJson.Raw(key.c_str(), JsonObject().Real("min", low).Real("median", median).Build());
            }
            // [lead243] each member's threat per second on each fight mob from its first threat there (its rate
            // while it hits that mob): the run's opening rate on a mob it has not hit yet
            for (auto const& [mob, byWho] : lead.threatSeen)
                for (auto const& [who, seen] : byWho)
                    if (seen.threat > 0.0f)
                    {
                        auto& tps = lead.openTps[who];
                        tps.first += ThreatSeenRate(seen);
                        ++tps.second;
                    }
            lead.threatSeen.clear();
            JsonObject opening;
            for (Player* member : seats)
                if (auto const tps = lead.openTps.find(member->GetGUID()); tps != lead.openTps.end())
                    opening.Real(member->GetName().c_str(), tps->second.first / float(tps->second.second));
            Decisions().Write(run, "leader", "threat_tps", tank->GetGUID(), tank->GetName(), false,
                              JsonObject()
                                  .Real("fightSec", sec)
                                  .Raw("tps", members.Build())
                                  .Raw("tankPerMob", tankMobs.Build())
                                  .Raw("margin", marginsJson.Build())
                                  .Raw("openTps", opening.Build()));
            lead.threatMax.clear();
            lead.threatMargins.clear();
        }
        if (!fighting)
        {
            // [lead513] a finished trash fight: the full health of its mobs that died, and its seconds
            if (lead.fightStartMs && !lead.bossFightOn && !lead.fightMobHealth.empty())
            {
                float killed = 0.0f;
                for (auto const& [mobGuid, maxHealth] : lead.fightMobHealth)
                    if (Creature* mob = map->GetCreature(mobGuid); !mob || !mob->IsAlive())
                        killed += maxHealth;
                if (killed > 0.0f)
                {
                    lead.trashKilledHealth += killed;
                    lead.trashFightSec += getMSTimeDiff(lead.fightStartMs, now) / 1000.0f;
                }
            }
            lead.fightMobHealth.clear();
            lead.fightStartMs = 0;
            lead.threatLeadLogged = 0;
            // [lead421] the walk to the next boss starts at the run's first tick and at each boss fight's end
            if (lead.bossFightOn || (!lead.stretchFromMs && !lead.stretches))
                lead.stretchFromMs = now ? now : 1;
            lead.bossFightOn = false;
        }
        else
        {
            if (!lead.fightStartMs)
                lead.fightStartMs = now;
            uint32 const into = getMSTimeDiff(lead.fightStartMs, now);
            uint32 const nextMark = lead.threatLeadLogged < 2000   ? 2000
                                    : lead.threatLeadLogged < 5000 ? 5000
                                                                   : lead.threatLeadLogged + 5000;
            for (uint32 const mark : {nextMark})
                if (into >= mark && lead.threatLeadLogged < mark)
                {
                    lead.threatLeadLogged = mark;
                    std::vector<float>& marginValues = lead.threatMargins[mark <= 5000 ? mark : 99000];
                    if (mark > 5000)
                        marginValues.clear();  // the last sample stands for the end
                    JsonArray mobs;
                    std::set<ObjectGuid> seenMobs;
                    std::vector<Player*> sides = seats;
                    sides.push_back(tank);
                    for (Player* side : sides)
                        for (Unit* attacker : side->getAttackers())
                        {
                            if (!attacker || !attacker->IsAlive() || !seenMobs.insert(attacker->GetGUID()).second)
                                continue;
                            float const tankThreat = attacker->GetThreatMgr().GetThreat(tank);
                            float top = 0.0f;
                            std::string topName = "-";
                            auto& held = lead.threatMax[attacker->GetGUID()];
                            held[tank->GetName()] = std::max(held[tank->GetName()], tankThreat);
                            for (Player* other : seats)
                            {
                                float const threat = attacker->GetThreatMgr().GetThreat(other);
                                held[other->GetName()] = std::max(held[other->GetName()], threat);
                                if (threat > top)
                                {
                                    top = threat;
                                    topName = other->GetName();
                                }
                            }
                            if (top > 0.0f)
                                marginValues.push_back(tankThreat / top);
                            mobs.Raw(JsonObject().Str("mob", attacker->GetName())
                                         .Num("low", attacker->GetGUID().GetCounter())
                                         .Real("tank", tankThreat).Real("top", top).Str("topWho", topName)
                                         .Real("lead", top > 0.0f ? tankThreat / top : -1.0f)
                                         .Bool("onTank", attacker->GetVictim() == tank)
                                         .Build());
                        }
                    if (mark <= 5000)
                        Decisions().Write(run, "leader", "threat_lead", tank->GetGUID(), tank->GetName(),
                                                      false,
                                                      JsonObject().Num("sec", mark / 1000).Raw("mobs", mobs.Build()));
                }
        }
        // [lead65] the move phase: the fight state and the pull order - a change starts a new phase
        {
            Leader::PullOrder phaseOrder;
            uint32 const orderId = Leader::Orders::GetOpen(group.LeaderGuid(), phaseOrder) ? phaseOrder.id : 0;
            // [lead74] ... and the fight target (a held move's purpose ends when its target changes or dies)
            std::string const key = std::string(fighting ? "fight:" : "out:") + std::to_string(orderId) + ":" +
                                    std::to_string(lead.commitMob.GetCounter()) + ":" +
                                    std::to_string(target ? target->GetGUID().GetCounter() : 0);
            if (key != lead.movePhaseKey)
            {
                lead.movePhaseKey = key;
                ++lead.movePhase;
                for (Player* bot : seats)
                    Leader::Events::SetFact(bot->GetGUID(), lead.movePhase);
                Leader::Events::SetFact(tank->GetGUID(), lead.movePhase);
            }
        }

        if (fighting)
        {
            lead.bossWaitFor.Clear();  // [lead46] the wait ends with a fight
            lead.plannerRestFor.clear();
            lead.restEndMana.clear();  // [lead230] a fight's spend is the fight line's
            lead.restExtra.clear();
        }
        // [lead51] the members' measured mana spend in fights (% of max mana per second of fight): the model's rate
        {
            float const dt = fighting && lead.manaTickMs ? getMSTimeDiff(lead.manaTickMs, now) / 1000.0f : 0.0f;
            lead.manaTickMs = fighting ? now : 0;
            std::vector<Player*> spenders = seats;
            spenders.push_back(tank);
            bool bossFought = false;  // [lead232] a boss among the fight's attackers
            if (fighting)
                for (Player* side : spenders)
                    for (Unit* attacker : side->getAttackers())
                        if (Creature const* mob = attacker ? attacker->ToCreature() : nullptr;
                            mob && (mob->IsDungeonBoss() || mob->isWorldBoss()))
                            bossFought = true;
            // [lead234] the fight's bosses (their max health); a fight that ends with them dead is a boss kill
            if (fighting)
                for (Player* side : spenders)
                    for (Unit* attacker : side->getAttackers())
                        if (Creature const* mob = attacker ? attacker->ToCreature() : nullptr;
                            mob && (mob->IsDungeonBoss() || mob->isWorldBoss()))
                            lead.fightBosses[mob->GetGUID()] = float(mob->GetMaxHealth());
            for (Player* member : spenders)
            {
                if (!member->GetMaxPower(POWER_MANA))
                    continue;
                auto& spend = lead.manaSpend[member->GetGUID()];
                float const pct = member->GetPowerPct(POWER_MANA);
                // [lead69] the NET use (mana at the fight's start minus at its end, per fight second): b114 asked the
                // paladin tank for 100 % before Taragaman from its gross spend (it regains much of it in the fight)
                if (!fighting)
                {
                    // [lead417] b201 rfc-h 5C: the balance druid's boss rate read 0.64 %/s - Oggleflint 43 -> 0 % with
                    // its last seconds dry, the net use capped at what it had: the boss line (37 %) let it start
                    // Jergosh short, dry again 40 s into 55 s. A fight's use counts until the member ran dry (below
                    // its cheapest cast: no more spend); a fight it began dry tells nothing of its rate
                    bool const dry = spend.dryPct >= 0.0f;
                    float const endPct = dry ? spend.dryPct : pct;
                    // [lead921] without the seconds a tool returned in
                    // [lead990] ... and the seconds it wanded
                    float const usedSec =
                        std::max((dry ? spend.drySec : spend.fightSec) - spend.toolSec - spend.wandSec, 0.0f);
                    // [lead480] b208 rfc-h: the casters came to Taragaman short - 5D's arcane mage at ~1 % passed the
                    // boss gate (Oggleflint began at 3 %, its Evocation's 60 % in that fight made the boss net use
                    // negative: boss rate 0, line 0), 5B's elemental shaman stood up at 30 % ("0.35 %/s measured on
                    // bosses x 76 s + 3": Oggleflint's mana potion netted in) and was dry 57 s in. The rate is the
                    // spend without the mana its tools and potions returned: FightManaLine credits a tool only where
                    // it is ready for the fight (its cooldown, the potion the last combat or the pre-pull blocks)
                    float const used = spend.prev - endPct + spend.toolGain + spend.wandGain;
                    if (spend.wandSec > 0.0f && spend.prev >= 0.0f)
                        LOG_INFO("playerbots", "[lead990] {}: {:.1f} s it wanded ({:+.1f} % there) are out of its "
                                 "measured mana rate: used {:.1f} % in {:.1f} s",
                                 member->GetName(), spend.wandSec, spend.wandGain, used, usedSec);
                    if (spend.toolSec > 0.0f && spend.prev >= 0.0f)
                        LOG_INFO("playerbots", "[lead921] {}: {:.1f} s a tool returned in ({:+.1f} % there) are out of "
                                 "its measured mana rate: used {:.1f} % in {:.1f} s",
                                 member->GetName(), spend.toolSec, spend.toolGain, used, usedSec);
                    if (spend.prev >= 0.0f && !(dry && spend.dryPct >= spend.prev))  // its net use counts
                    {
                        // [lead222] signed: a fight that ends with more mana than it began (Divine Plea, Judgement
                        // of Wisdom, Spiritual Attunement) lowers the net use; the clamp at 0 per fight counted such
                        // fights as zero use and raised the rate
                        spend.spent += used;
                        spend.sec += usedSec;
                        ++spend.fights;
                        if (spend.sec > 0.0f)
                            RestState::NoteManaRate(member->GetGUID(), std::max(0.0f, spend.spent / spend.sec));
                        // [lead232] the boss fights' own rate, and a full fight's rate (one at least as long as the
                        // run's mean fight: its whole rotation, not a few casts on a short pull)
                        if (spend.boss && usedSec > 0.0f)
                        {
                            spend.bossSpent += used;
                            spend.bossSec += usedSec;
                            RestState::NoteBossManaRate(member->GetGUID(),
                                                        std::max(0.0f, spend.bossSpent / spend.bossSec));
                        }
                        // [lead234] b188: the highest single fight was an outlier (a hunter's 1.38 %/s); the net use
                        // over all the full-length fights
                        if (spend.fightSec > 0.0f && spend.fightSec >= spend.sec / float(spend.fights))
                        {
                            spend.fullSpent += used;
                            spend.fullSec += usedSec;
                            RestState::NotePeakManaRate(member->GetGUID(),
                                                        std::max(0.0f, spend.fullSpent / spend.fullSec));
                        }
                        // [lead520] the net use per second it acted (its whole rotation's rate)
                        // [lead810] b221 rfc-h 5H: the fire mage's boss line read "7.10 %/s (its acting seconds on
                        // bosses)" at Taragaman (its Fireball spends ~1.3 %/s), after Oggleflint - 100 % asked for 81
                        // s, the gem and the injector spent on it at 75 % / 52 %. A fight's acting rate above what the
                        // member's spells can spend per acting second (MaxSpendPctPerSec, spell data) is a measuring
                        // error: not noted, logged with the fight's inputs
                        float const spendCap = RestState::MaxSpendPctPerSec(member);
                        float const fightActing = spend.actingSec > 0.0f ? used / spend.actingSec : 0.0f;
                        bool const actingImpossible = spendCap > 0.0f && fightActing > spendCap;
                        if (actingImpossible)
                            LOG_INFO("playerbots", "[lead810] {}: its fight's acting mana rate {:.2f} %/s is above its "
                                     "spells' {:.2f} %/s - not noted (used {:.1f} % = start {:.1f} - end {:.1f} + "
                                     "returned {:.1f}; acting {:.1f} s of {:.1f} s{}{})",
                                     member->GetName(), fightActing, spendCap, used, spend.prev, endPct,
                                     spend.toolGain, spend.actingSec, usedSec, dry ? ", dry" : "",
                                     spend.boss ? ", a boss fight" : "");
                        if (spend.actingSec > 0.0f && !actingImpossible)
                        {
                            spend.actSpent += used;
                            spend.actSec += spend.actingSec;
                            RestState::NoteActingManaRate(member->GetGUID(),
                                                          std::max(0.0f, spend.actSpent / spend.actSec));
                            // [lead610] the boss fights' own acting rate (the boss line's rotation rate)
                            if (spend.boss)
                            {
                                spend.bossActSpent += used;
                                spend.bossActSec += spend.actingSec;
                                RestState::NoteBossActingManaRate(
                                    member->GetGUID(), std::max(0.0f, spend.bossActSpent / spend.bossActSec));
                            }
                        }
                        // [lead393] a healer's net use per tank health the plan costed its fights at
                        if (spend.fightCost > 0.0f)
                        {
                            spend.costSpent += used;
                            spend.costSum += spend.fightCost;
                            RestState::NoteHealerPctPerCost(member->GetGUID(),
                                                            std::max(0.0f, spend.costSpent / spend.costSum));
                        }
                    }
                    spend.prev = -1.0f;
                    spend.fightSec = 0.0f;
                    spend.boss = false;
                    spend.fightCost = 0.0f;
                    spend.dryPct = -1.0f;  // [lead417]
                    spend.drySec = 0.0f;
                    spend.lastPct = -1.0f;  // [lead480]
                    spend.toolGain = 0.0f;
                    spend.toolSec = 0.0f;  // [lead921]
                    spend.wandSec = 0.0f;  // [lead990]
                    spend.wandGain = 0.0f;
                    spend.potion = false;
                    spend.actingSec = 0.0f;  // [lead520]
                    spend.gemReady = false;  // [lead610]
                    continue;
                }
                if (spend.prev < 0.0f)
                    spend.prev = pct;  // the fight's start
                // [lead841] b223 rfc-h 5H Oggleflint 280-336 s: the fire mage began the boss dry (1 %), its Evocation
                // took it to 58 % in the first 8 s, and it fought the boss out with the potion and the gem - the whole
                // fight was dropped as "a fight it began dry" (lead417), no boss rate was noted, and Taragaman's line
                // read every fight's acting rate (1.04 %/s) where the boss rotation spent ~1.4 %/s (dry 6 s before the
                // kill). A fight begun dry is measured from the tick it can cast again (a tool's or a potion's
                // return): its start there, the rise before it is in that start
                if (spend.dryPct >= 0.0f && spend.dryPct >= spend.prev && pct >= RestState::CheapestCastPct(member))
                {
                    spend.prev = pct;
                    spend.lastPct = pct;
                    spend.fightSec = 0.0f;
                    spend.actingSec = 0.0f;
                    spend.toolGain = 0.0f;
                    spend.toolSec = 0.0f;  // [lead921]
                    spend.wandSec = 0.0f;  // [lead990]
                    spend.wandGain = 0.0f;
                    spend.dryPct = -1.0f;
                    spend.drySec = 0.0f;
                }
                // [lead480] the mana a tool or a potion returned this tick (its rise while the return comes: under
                // Evocation / Innervate / Hymn / Dispersion / Viper, a Shadowfiend or Mana Tide out, the tick the
                // game marks the combat's potion), counted until it runs dry. Divine Plea stays in the rate (lead224)
                bool const potionNow = member->GetLastPotionId() && !spend.potion;
                spend.potion |= member->GetLastPotionId() != 0;
                // [lead610] b217 rfc-h 5H: the fire mage's Mana Sapphire (24 % at each boss pull: 256.3, 654.7,
                // 855.5 s) is no potion (no Player::GetLastPotionId) and no tool aura, so its mana was netted into
                // the measured use - Taragaman's fight read ~45 % used where ~69 % was spent, the boss line 0.78 %/s.
                // The tick a ready gem goes (its cooldown started, or its last charge used) is the gem's return
                uint32 gemAmount = 0;
                bool const gemReady = RaidConsumables::FindManaGem(member, gemAmount) != 0;
                bool const gemNow = spend.gemReady && !gemReady;
                spend.gemReady = gemReady;
                // [lead921] b225 rfc-h 5C: the balance druid kept casting under its own Innervate (Taragaman
                // 40052, Jergosh 51715), each tick's net rise counted as the tool's return - the spend of its 10 s
                // vanished while they counted as acting seconds (0.95 %/s at Jergosh against ~1.3 %/s cast), and the
                // boss line credits the same Innervate again. A tick a tool returns in (not Viper: the hunter's
                // aspect is its rotation's own) tells nothing of the rotation's rate: its change and its seconds are
                // out of the measure
                bool const toolTick = spend.lastPct >= 0.0f && spend.dryPct < 0.0f &&
                                      (potionNow || gemNow || ManaToolReturning(member, spenders, false));
                // [lead990] b228 rfc-h: the casters wanded at 1-3 % mana - above their cheapest cast, so not "dry"
                // (lead417) - and those seconds spent nothing: 5H's fire mage measured 0.61 %/s on trash (its Fireball
                // spends ~1.3 %/s), its rest line let it start the next pull short, and that pull wanded again. A tick
                // on a wand (the autorepeat of a member that is no hunter: Shoot) is no second of its mana rotation
                Spell const* autoRepeat = member->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
                bool const wandTick = !toolTick && spend.lastPct >= 0.0f && spend.dryPct < 0.0f &&
                                      member->getClass() != CLASS_HUNTER && autoRepeat &&
                                      autoRepeat->getState() != SPELL_STATE_FINISHED;
                if (toolTick)
                {
                    spend.toolGain += pct - spend.lastPct;
                    spend.toolSec += dt;
                }
                else if (wandTick)
                {
                    spend.wandGain += pct - spend.lastPct;
                    spend.wandSec += dt;
                }
                else if (spend.lastPct >= 0.0f && pct > spend.lastPct && spend.dryPct < 0.0f &&
                         member->HasAura(RestState::SPELL_ASPECT_OF_THE_VIPER))
                    spend.toolGain += pct - spend.lastPct;
                spend.lastPct = pct;
                spend.fightSec += dt;
                // [lead520] it acted this tick: a cast or channel on, or its global cooldown running (game state)
                // [lead610] a channel of its own mana tool (Evocation, Dispersion) is no rotation second: its mana
                // is the tool's return (toolGain), its seconds spend nothing
                bool const ownToolChannel = member->HasAura(RestState::SPELL_EVOCATION) ||
                                            member->HasAura(RestState::SPELL_DISPERSION_MANA);
                // [lead842] b223 rfc-h: the casters wanded at 1-3 % (above their cheapest cast, so not "dry": 5H fire
                // 932-944 s, 5D arcane 835-837 s, 5B shadow) and the wand's autorepeat counted as acting seconds
                // (Unit::IsNonMeleeSpellCast without skipAutorepeat) - seconds that spend no mana, the acting rate
                // under the rotation's (5H fire 1.04 %/s at Taragaman, ~1.4 %/s spent there). The wand is no second
                // of the mana rotation
                if (spend.dryPct < 0.0f && !ownToolChannel && !toolTick)
                    if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
                        member->IsNonMeleeSpellCast(false, false, true) ||
                        (memberAI && memberAI->GetGlobalCooldownRemaining()))
                        spend.actingSec += dt;
                if (spend.dryPct < 0.0f && pct < RestState::CheapestCastPct(member))  // [lead417] it ran dry
                {
                    spend.dryPct = pct;
                    spend.drySec = spend.fightSec;
                }
                spend.boss |= bossFought;
                // [lead393] the fight's planned cost (the open order's): the unit FightManaLine's healer term reads
                if (PlayerbotAI::IsHeal(member))
                    if (Leader::PullOrder open; Leader::Orders::GetOpen(group.LeaderGuid(), open))
                        spend.fightCost = std::max(spend.fightCost, open.cost);
                // [lead367] the measured fights' Divine Plea uptime (FightManaLine credits it only where none was)
                if (member->HasAura(RestState::SPELL_DIVINE_PLEA))
                    RestState::NoteDivinePleaInFight(member->GetGUID());
            }
        }
        // [lead433] b202 rfc-h 5A 610.9-623.3 s: the boss pull waited for "health 94 % < 95 %" of the tank - the
        // leader told the tank Rest (it sat and drank at 93 % mana for 10 s) and the healer TopUp (on itself, the
        // lowest). One fact, one owner: a boss gap on health alone (its mana at its boss line) is the healer's
        // top-up while the healer stands and heals; the member does not sit for it
        auto const healerTopsUp = [&](Player* member, Creature* boss)
        {
            if (!boss || !healer || !healer->IsAlive() || healer->IsSitState() || RestState::WantsDrink(healer))
                return false;
            if (PullReadiness::MemberBossGap(member, boss).rfind("health", 0) != 0)
                return false;
            return member->getPowerType() != POWER_MANA ||
                   member->GetPowerPct(POWER_MANA) >= PullReadiness::MemberBossManaLine(member, boss);
        };
        if (!fighting && !tank->IsInCombat() && !Leader::Orders::IsOpen(group.LeaderGuid()))
        {
            std::string tankRestWhy;
            Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor);
            std::string const tankGap = waitBoss && waitBoss->IsAlive() && !healerTopsUp(tank, waitBoss)
                                            ? PullReadiness::MemberBossGap(tank, waitBoss)
                                            : std::string();
            if (!tankGap.empty())
                tankRestWhy = "the boss pull waits: " + tankGap;
            bool const plannerWaits = !lead.plannerRestFor.empty() && tank->GetName() == lead.plannerRestFor &&
                                      !MemberFull(tank) && BelowPlannerLine(tank, lead.plannerRestLine);  // [lead50]
            if (plannerWaits)
                tankRestWhy = PlannerWaitWhy(lead.plannerRestLine);
            // [lead221] b185 rfc-h 5E 770-816 s: "sits: to full" at 97 % and 62 % mana - a tank already drinking
            // (or under the leader's Rest, which WantsDrink reads as "drink to 100 %") was rested to full. The tank
            // rests while it is below its own computed lines (RestState::BelowOwnLine), as every member (lead59)
            bool const tankRests = plannerWaits ? (tank->IsSitState() || RestState::CanRest(tank))
                                                : (waitBoss && waitBoss->IsAlive()
                                        ? !tankGap.empty()
                                        : RestState::BelowOwnLine(tank, &tankRestWhy) && !MemberFull(tank)) &&
                                                      (tank->IsSitState() || RestState::CanRest(tank));
            char const* const tankHurt = tankRests ? HurtOutOfFight(tank) : nullptr;
            if (tankHurt && lead.restHurtLogged.insert(tank->GetGUID()).second)
                Decisions().Write(run, "leader", "rest_held", tank->GetGUID(), tank->GetName(), true,
                                  JsonObject().Str("member", tank->GetName()).Str("why", tankHurt));
            if (!tankHurt)
                lead.restHurtLogged.erase(tank->GetGUID());
            if (tankRests && !tankHurt && Leader::Events::SetReady(tank->GetGUID(), Leader::ReadyEvent::Rest))
                logEvent(tank, "Rest", JsonObject().Str("who", "tank")
                                           .Str("why", tankRestWhy.empty() ? "sits: to full" : tankRestWhy));
            // [lead433] b202 rfc-h 5A 578.0-582.7 s: "held for now: Rtpalaa sits" - the tank reached the planner's
            // 50 % line and drank on: its Rest, once out, was never replaced (a member's is, by Prepare), so the
            // drinking tank decided itself when to stand. The Rest ends where its line is reached
            if (!tankRests)
                if (Leader::MemberEvents tankEvents; Leader::Events::Get(tank->GetGUID(), tankEvents) &&
                                                     tankEvents.ready == Leader::ReadyEvent::Rest &&
                                                     Leader::Events::SetReady(tank->GetGUID(),
                                                                              Leader::ReadyEvent::Prepare))
                    logEvent(tank, "Prepare", JsonObject().Str("who", "tank").Str("why", "its rest line is reached"));
        }

        // [lead76] the leader's "top up X": the healer heals the member whose health holds the pull (out of a fight)
        if (!fighting && healer)
        {
            ObjectGuid topUp = lead.topUpFor;
            // [lead433] the member whose health holds the boss pull first (its gap is the healer's, above)
            if (topUp.IsEmpty())
                if (Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor);
                    waitBoss && waitBoss->IsAlive())
                {
                    std::vector<Player*> all = seats;
                    all.push_back(tank);
                    for (Player* member : all)
                        if (healerTopsUp(member, waitBoss))
                        {
                            topUp = member->GetGUID();
                            break;
                        }
                }
            // [lead116] a hurt member with the healer above its own mana line is topped up by the healer (one
            // command per change), not fed: the most hurt one
            if (topUp.IsEmpty() && !RestState::WantsDrink(healer) && !healer->IsSitState())
            {
                std::vector<Player*> all = seats;
                all.push_back(tank);
                float lowest = 100.0f;
                // [lead945] logged once per member and stretch above its line
                thread_local std::set<ObjectGuid> drainLogged;
                for (Player* member : all)
                    if (HealTriage::DrainsOwnHealth(member) && member->IsAlive())
                    {
                        float const line = RestState::HealthLine(member);
                        if (member->GetHealthPct() >= 100.0f || member->GetHealthPct() < line)
                            drainLogged.erase(member->GetGUID());
                        else if (drainLogged.insert(member->GetGUID()).second)
                            LOG_INFO("playerbots", "[lead945] {}: {} drains its own health ({:.0f} %, its line "
                                     "{:.0f} %): no top-up until its next fight needs it", run.runId,
                                     member->GetName(), member->GetHealthPct(), line);
                    }
                for (Player* member : all)
                    if (member->IsAlive() && member->GetHealthPct() < lowest &&
                        // [lead945] user 2026-11-03 (b226 5F-6: 24 of the tree's 36 TopUps were the demonology
                        // warlock, "a readiness gap on health" after each Life Tap): a member that drains its own
                        // health by design is topped up when its next fight needs it - below its health line
                        // (RestState::HealthLine) or named by a readiness hold (topUpFor, the boss gap above) - not
                        // after every tap; no healer fixes on it
                        (!HealTriage::DrainsOwnHealth(member) ||
                         member->GetHealthPct() < RestState::HealthLine(member)) &&
                        // [lead946] nor a member the heal cannot land on / that cannot lose health now
                        !HealTriage::IsHealImmune(healer, member) && !HealTriage::CannotLoseHealth(healer, member))
                    {
                        lowest = member->GetHealthPct();
                        topUp = member->GetGUID();
                    }
            }
            if (topUp.IsEmpty())
                if (Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor))
                {
                    std::vector<Player*> all = seats;
                    all.push_back(tank);
                    for (Player* member : all)
                        if (PullReadiness::MemberBossGap(member, waitBoss).rfind("health", 0) == 0)
                        {
                            topUp = member->GetGUID();
                            break;
                        }
                }
            Player* healed = topUp.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(topUp);
            // [lead179] not while the healer drinks: its Rest holds the top-up's mana, it heals once it stands
            if (healed && healed->IsAlive() && healed->GetHealthPct() < 100.0f &&
                !(healer->IsSitState() && RestState::IsDrinking(healer)) &&
                Leader::Events::SetHealTarget(healer->GetGUID(), topUp))
                logEvent(healer, "TopUp", JsonObject().Str("member", healed->GetName())
                                              .Real("health", healed->GetHealthPct())
                                              .Str("why", "a readiness gap on health"));
        }

        // [lead400] .. [lead405] dispels, the resurrections out of a fight, the potions, the buff pass's soulstone /
        // Ritual of Souls / Soulwell: the leader's (the members' own are off)
        Leader::Support::Tick(group, tank, seats, healer, fighting, logEvent);

        uint32 seat = 0;
        for (Player* bot : seats)
        {
            ObjectGuid const guid = bot->GetGUID();
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            bool const combat = fighting || bot->IsInCombat();
            uint32 const index = seat++;
            // [lead17] the harness's (the leader's) own walk for this member (a link, out of the water, an event
            // gather): it runs, the leader issues no other move meanwhile
            LeaderGroup::WalkPoint harnessWalk;
            bool const walking = group.GetWaypoint(guid, harnessWalk);

            bool restCommanded = false;  // [lead47] the Rest command this tick: no move replaces it
            // [lead732] b219 rfc-h 5C 840.8 s: the balance druid's Rest ("mana 20 % < need 34 %") went out 0.2 s before
            // pull 38 and its end was never logged - the Rest is asserted out of a fight only (the command ends at
            // the tick's end once the fight is on, the druid attacked at 841.1 s), but rest_line / the leader_event
            // timeline showed it live to 903 s. A Rest ends when the member's fight starts, logged as such
            if (combat)
                if (std::string& logged = lead.restLogged[guid]; !logged.empty())
                {
                    float const mana = bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f;
                    Decisions().Write(run, "leader", "rest_line", guid, bot->GetName(), false,
                                      JsonObject().Str("member", bot->GetName()).Str("state", "end")
                                          .Str("line", logged).Str("why", "the fight starts")
                                          .Real("mana", mana).Real("health", bot->GetHealthPct()));
                    logEvent(bot, "RestEnd", JsonObject().Str("why", "the fight starts").Real("mana", mana));
                    logged.clear();
                    lead.restEndMana.erase(guid);  // a fight's spend is no standing spend between two Rests
                    lead.restExtra.erase(guid);
                }
            // 1. readiness, out of combat: Rest to whoever needs it (to full), Ready? while the pull waits for the
            // spots, Prepare (buffs, forms, a resurrection) while the group stands
            if (!combat)
            {
                std::string restWhy;
                // [lead59] user: no "sits: to full" - the Rest holds while the member is below its own computed line
                // for the next fight (RestState::BelowOwnLine, FightManaLine) and stands it up at that line
                bool needsRest = RestState::BelowOwnLine(bot, &restWhy);
                bool const full = MemberFull(bot);
                needsRest &= !full;  // [lead22] never Rest a full member
                lead.restingToFull.erase(guid);
                // [lead46] b95 rfc-h 348-423 s: the boss pull waited for "Rtshaab at 66 % mana" (BossReadyGap, 90 %
                // for a real boss) while the Rest rule read Oggleflint as a mini-boss (70 %) and gave Rtshaab no Rest.
                // While the boss pull waits there is ONE readiness: the member rests exactly while it is below the
                // boss ready lines (PullReadiness::MemberBossGap) and stands when it reaches them - the pull goes as
                // the last one is ready
                if (Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor);
                    waitBoss && waitBoss->IsAlive())
                {
                    std::string const gap =
                        healerTopsUp(bot, waitBoss) ? std::string() : PullReadiness::MemberBossGap(bot, waitBoss);
                    needsRest = !gap.empty();  // [lead433] a health gap the healer tops up: no Rest
                    restWhy = gap.empty() ? restWhy : "the boss pull waits: " + gap;
                    lead.restingToFull.erase(guid);
                }
                // [lead50] the planner's held answer waits for this member: it rests
                if (!lead.plannerRestFor.empty() && bot->GetName() == lead.plannerRestFor && !MemberFull(bot) &&
                    BelowPlannerLine(bot, lead.plannerRestLine))  // [lead221] up to the line it named
                {
                    needsRest = true;
                    restWhy = PlannerWaitWhy(lead.plannerRestLine);
                }
                // [lead179] b179 rfc-h 5H 450-460 s: the disc priest sat at 10 %, stood at its line (15.2 %), was
                // told to top up the mage (88 %), fell below the line and sat again - 7 drinks in 10 s. A drinking
                // healer's line holds the top-up heals still to come: it drinks on while its mana is below its line
                // plus the mana of every member's missing health (HealerPctPerTankHealth per tank health), so
                // it stands once and heals without dropping under the line (one Rest per fact)
                if (!needsRest && PlayerbotAI::IsHeal(bot) && bot->IsSitState() && RestState::IsDrinking(bot) &&
                    bot->GetMaxPower(POWER_MANA) && tank->GetMaxHealth())
                {
                    float missing = 0.0f;
                    uint32 hurt = 0;
                    std::vector<Player*> all = seats;
                    all.push_back(tank);
                    for (Player* member : all)
                        if (member->IsAlive() && member->GetHealth() < member->GetMaxHealth())
                        {
                            missing += float(member->GetMaxHealth() - member->GetHealth());
                            ++hurt;
                        }
                    if (missing > 0.0f)
                    {
                        Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor);
                        float const line = waitBoss && waitBoss->IsAlive()
                                               ? PullReadiness::MemberBossManaLine(bot, waitBoss)
                                               : RestState::OwnManaLine(bot);
                        // [lead194] b181 rfc-h 5C 327-337 s: the disc stood 0.5 s above its 15 % line, its TopUp Renew
                        // on the druid (a few % of health) cost more than the health share and it sat again 7 times -
                        // each top-up is at least one cast of the healer's cheapest heal (its known top rank's cost)
                        float healPct = 0.0f;
                        // Flash Heal, Lesser Heal, Renew, Lesser / Healing Wave, Flash of Light, Holy Light,
                        // Regrowth, Rejuvenation, Healing Touch, Nourish (first ranks: the chains' top known rank)
                        static constexpr uint32 TOP_UP_HEALS[] = {2061, 2050, 139,  8004, 331,  19750,
                                                                  635,  8936, 774, 5185, 50464};
                        for (uint32 first : TOP_UP_HEALS)
                            for (uint32 rank = sSpellMgr->GetLastSpellInChain(first); rank;
                                 rank = sSpellMgr->GetPrevSpellInChain(rank))
                                if (bot->HasSpell(rank))
                                {
                                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(rank))
                                    {
                                        float const pct = 100.0f *
                                                          float(info->CalcPowerCost(bot, info->GetSchoolMask())) /
                                                          float(bot->GetMaxPower(POWER_MANA));
                                        healPct = healPct > 0.0f ? std::min(healPct, pct) : pct;
                                    }
                                    break;
                                }
                        float const topUp =
                            std::max(RestState::HealerPctPerTankHealth(bot) * missing / float(tank->GetMaxHealth()),
                                     float(hurt) * healPct);
                        if (bot->GetPowerPct(POWER_MANA) < line + topUp)
                        {
                            needsRest = true;
                            restWhy = Acore::StringFormat(
                                "drinks on for the top-up heals: mana {:.0f} % < {:.0f} % "
                                "line + {:.0f} % for {:.0f} missing health",
                                bot->GetPowerPct(POWER_MANA), line, topUp, missing);
                        }
                    }
                }
                // [lead230] b187 rfc-h 5C 970-1022 s: the disc priest rested 20 times in 52 s (12 -> 18 %, line 15 %):
                // each Rest ended at its line, the boss buff pass (Prepare) took 6 % and it sat again. Mana it spent
                // standing between two Rests out of a fight is mana it spends again before the pull: the Rest after
                // such a spend holds to its line plus that spend (one Rest for the fact, not one per buff)
                if (bot->GetMaxPower(POWER_MANA))
                {
                    float const mana = bot->GetPowerPct(POWER_MANA);
                    auto const ended = lead.restEndMana.find(guid);
                    if (needsRest && ended != lead.restEndMana.end() && ended->second > mana)
                    {
                        lead.restExtra[guid] = std::max(lead.restExtra[guid], ended->second - mana);
                        lead.restEndMana.erase(ended);
                    }
                    auto const extra = lead.restExtra.find(guid);
                    if (!needsRest && extra != lead.restExtra.end() && bot->IsSitState() &&
                        RestState::IsDrinking(bot))
                    {
                        float const line = RestState::OwnManaLine(bot);
                        if (mana < line + extra->second)
                        {
                            needsRest = true;
                            restWhy = Acore::StringFormat(
                                "drinks on for what it spends standing: mana {:.0f} % < {:.0f} % line + {:.0f} %", mana,
                                line, extra->second);
                        }
                    }
                    if (!needsRest && !lead.restLogged[guid].empty())
                    {
                        lead.restEndMana[guid] = mana;  // this Rest ends now
                        lead.restExtra.erase(guid);
                    }
                }
                // [lead955] user 2026-11-03 (warlock-detached): a warlock short of mana is not simply sat down - the
                // leader picks the fastest way to its mana line: Life Tap as its command (the taps' global cooldowns
                // plus the healer's time to heal back the health the taps take below the warlock's health line, and
                // the healer's own drink for that heal's mana beyond its spare), drinking like any member (the line
                // over its best drink's rate), or a druid's Innervate under the cooldown rule (cooldownVerdict: on
                // trash only when back before the next boss pull). Game data and the members' own rates, no clock
                bool manaWayLive = false;
                if (needsRest && HealTriage::DrainsOwnHealth(bot) && bot->GetMaxPower(POWER_MANA) &&
                    !RestState::IsDrinking(bot))
                {
                    float manaLine = RestState::OwnManaLine(bot);
                    if (Creature* waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor);
                        waitBoss && waitBoss->IsAlive())
                        manaLine = PullReadiness::MemberBossManaLine(bot, waitBoss);
                    if (!lead.plannerRestFor.empty() && bot->GetName() == lead.plannerRestFor)
                        manaLine = std::max(manaLine, lead.plannerRestLine >= 0.0f ? lead.plannerRestLine : 100.0f);
                    float const manaPct = bot->GetPowerPct(POWER_MANA);
                    float const needPct = std::min(manaLine, 100.0f) - manaPct;
                    if (needPct > 0.0f && ManaToolReturning(bot, seats, false))
                        needsRest = false;  // an Innervate (another tool) still gives it mana: no Rest, no tap over it
                    else if (needPct > 0.0f)
                    {
                        manaWayLive = true;
                        constexpr float NONE = -1.0f;
                        float const maxMana = float(bot->GetMaxPower(POWER_MANA));
                        float const drinkRate = RestState::DrinkPctPerSecOf(bot);
                        float const drinkSec = drinkRate > 0.0f ? needPct / drinkRate : NONE;
                        // Life Tap (spell_warl_life_tap: health = its effect, mana = that plus half the shadow spell
                        // power, Improved Life Tap's percent on it)
                        constexpr uint32 SPELL_LIFE_TAP_RANK_1 = 1454;
                        constexpr uint32 ICON_IMPROVED_LIFE_TAP = 208;
                        uint32 tapSpell = 0;
                        for (uint32 rank = sSpellMgr->GetLastSpellInChain(SPELL_LIFE_TAP_RANK_1); rank && !tapSpell;
                             rank = sSpellMgr->GetPrevSpellInChain(rank))
                            if (bot->HasSpell(rank))
                                tapSpell = rank;
                        float tapSec = NONE;
                        std::string tapWhy = "no Life Tap";
                        if (SpellInfo const* tap = tapSpell ? sSpellMgr->GetSpellInfo(tapSpell) : nullptr)
                        {
                            float const healthPerTap = float(std::max(tap->Effects[EFFECT_0].CalcValue(bot), 1));
                            float manaPerTap =
                                healthPerTap + float(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW)) * 0.5f;
                            if (AuraEffect const* improved =
                                    bot->GetDummyAuraEffect(SPELLFAMILY_WARLOCK, ICON_IMPROVED_LIFE_TAP, 0))
                                manaPerTap *= 1.0f + float(improved->GetAmount()) / 100.0f;
                            float const taps = std::ceil(needPct / 100.0f * maxMana / std::max(manaPerTap, 1.0f));
                            float const gcdSec =
                                std::max(1.0f, float(tap->StartRecoveryTime) / 1000.0f *
                                                   bot->GetFloatValue(UNIT_MOD_CAST_SPEED));
                            float const spent = taps * healthPerTap;
                            float const above = float(bot->GetHealth()) -
                                                RestState::HealthLine(bot) / 100.0f * float(bot->GetMaxHealth());
                            float const healBack = std::max(0.0f, spent - above);
                            float healSec = 0.0f;
                            float healerDrinkSec = 0.0f;
                            if (healBack > 0.0f)
                            {
                                RestState::HealerFacts const facts = healer && healer != bot
                                                                         ? RestState::HealerThroughput(healer, bot)
                                                                         : RestState::HealerFacts();
                                if (facts.hps > 0.0f && facts.healPerMana > 0.0f && facts.maxMana > 0.0f)
                                {
                                    healSec = healBack / facts.hps;
                                    float const healerCost = 100.0f * healBack / facts.healPerMana / facts.maxMana;
                                    float const spare = std::max(
                                        healer->GetPowerPct(POWER_MANA) - RestState::OwnManaLine(healer), 0.0f);
                                    float const healerDrink = RestState::DrinkPctPerSecOf(healer);
                                    if (healerCost > spare)
                                        healerDrinkSec = healerDrink > 0.0f ? (healerCost - spare) / healerDrink : NONE;
                                }
                                else
                                    healSec = NONE;  // no healer heals it back
                            }
                            if (healSec >= 0.0f && healerDrinkSec >= 0.0f)
                                tapSec = taps * gcdSec + healSec + healerDrinkSec;
                            tapWhy = Acore::StringFormat("{:.0f} taps x {:.1f} s + heal back {:.0f} health {:.1f} s + "
                                                         "the healer's drink {:.1f} s",
                                                         taps, gcdSec, healBack, healSec, healerDrinkSec);
                        }
                        // Innervate: a druid with it ready, in a form that casts it, not drinking, not short of its
                        // own line (lead751), its cooldown verdict allowing it
                        Player* innervator = nullptr;
                        uint32 innervate = 0;
                        float innervateSec = NONE;
                        std::string innervateWhy = "no Innervate ready";
                        std::vector<Player*> casters = seats;
                        casters.push_back(tank);
                        for (Player* druid : casters)
                        {
                            uint32 const spell = druid != bot && druid->IsAlive() && !druid->IsSitState()
                                                     ? LeaderReadySpell(druid, {SPELL_INNERVATE})
                                                     : 0;
                            SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
                            if (!info || info->CheckShapeshift(druid->GetShapeshiftForm()) != SPELL_CAST_OK ||
                                (druid->GetMaxPower(POWER_MANA) &&
                                 druid->GetPowerPct(POWER_MANA) < RestState::OwnManaLine(druid)))
                                continue;
                            float const gainPct = RestState::ManaToolPct(druid, bot, spell);
                            float const durationSec = std::max(info->GetMaxDuration(), 0) / 1000.0f;
                            if (gainPct <= 0.0f || durationSec <= 0.0f)
                                continue;
                            std::string verdict;
                            if (!cooldownVerdict(RechargeAfterCastSec(druid, spell), durationSec, verdict))
                            {
                                innervateWhy = "Innervate " + verdict;
                                continue;
                            }
                            float const castSec =
                                std::max<uint32>(info->StartRecoveryTime, info->CalcCastTime(druid)) / 1000.0f;
                            float const left = needPct - gainPct;
                            float const sec = left <= 0.0f      ? castSec + durationSec * needPct / gainPct
                                              : drinkRate > 0.0f ? castSec + durationSec + left / drinkRate
                                                                 : NONE;
                            if (sec >= 0.0f && (innervateSec < 0.0f || sec < innervateSec))
                            {
                                innervator = druid;
                                innervate = spell;
                                innervateSec = sec;
                                innervateWhy = Acore::StringFormat("{:.0f} % from {}, {}", gainPct, druid->GetName(),
                                                                   verdict);
                            }
                        }
                        auto const faster = [](float a, float b) { return a >= 0.0f && (b < 0.0f || a < b); };
                        char const* way = faster(tapSec, drinkSec) && faster(tapSec, innervateSec) ? "life tap"
                                          : faster(innervateSec, drinkSec)                        ? "innervate"
                                                                                                   : "drink";
                        // one decision per fact: the way taken stays while it is still a way (no A -> B -> A as the
                        // rates move with the mana)
                        if (std::string const& held = lead.manaWayLogged[guid];
                            (held == "life tap" && tapSec >= 0.0f) || (held == "innervate" && innervateSec >= 0.0f))
                            way = held == "life tap" ? "life tap" : "innervate";
                        std::string const note = Acore::StringFormat(
                            "{}: mana {:.0f} % < {:.0f} % line - life tap {:.1f} s ({}), drink {:.1f} s, innervate "
                            "{:.1f} s ({})",
                            way, manaPct, manaLine, tapSec, tapWhy, drinkSec, innervateSec, innervateWhy);
                        if (std::string& logged = lead.manaWayLogged[guid]; logged != way)
                        {
                            logged = way;
                            Decisions().Write(run, "leader", "warlock_mana_way", guid, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("way", way)
                                                  .Real("mana", manaPct).Real("line", manaLine)
                                                  .Real("lifeTapSec", tapSec).Real("drinkSec", drinkSec)
                                                  .Real("innervateSec", innervateSec).Str("why", note));
                            LOG_INFO("playerbots", "[lead955] {}: {}", bot->GetName(), note);
                        }
                        std::string const picked = way;
                        // the taps go while its health is at or above its health line; below it the healer's top-up
                        // (lead945: a Life Tap warlock under its line) brings it back first - no tap into a pull
                        if (picked == "life tap" && bot->GetHealthPct() >= RestState::HealthLine(bot) &&
                            Leader::Events::SetExternal(guid, tapSpell, ObjectGuid::Empty))
                            logEvent(bot, "External", JsonObject().Num("spell", tapSpell).Str("on", bot->GetName())
                                                          .Str("why", note));
                        if (picked == "innervate" &&
                            Leader::Events::SetExternal(innervator->GetGUID(), innervate, guid))
                            logEvent(innervator, "External", JsonObject().Num("spell", innervate)
                                                                 .Str("on", bot->GetName()).Str("why", note));
                        if (picked != "drink")
                            needsRest = false;  // its mana comes on the move: no Rest
                    }
                }
                if (!manaWayLive)
                    lead.manaWayLogged.erase(guid);
                {
                    // [lead76] the Rest's line vs the member's mana, logged at the Rest's start and end
                    // [lead79] logged at the Rest's start and end only (the line's text changes with the mana)
                    std::string const state = needsRest ? "rest: " + restWhy : std::string();
                    std::string& logged = lead.restLogged[guid];
                    if (needsRest != !logged.empty())
                    {
                        Decisions().Write(
                            run, "leader", "rest_line", guid, bot->GetName(), false,
                            JsonObject().Str("member", bot->GetName()).Str("state", needsRest ? "start" : "end")
                                .Str("line", needsRest ? restWhy : logged)
                                .Real("mana", bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f)
                                .Real("health", bot->GetHealthPct()));
                        logged = state;
                    }
                }
                char const* const hurt = needsRest ? HurtOutOfFight(bot) : nullptr;
                if (hurt && lead.restHurtLogged.insert(guid).second)
                    Decisions().Write(run, "leader", "rest_held", guid, bot->GetName(), true,
                                      JsonObject().Str("member", bot->GetName()).Str("why", hurt));
                if (!hurt)
                    lead.restHurtLogged.erase(guid);
                if (needsRest && !hurt && (bot->IsSitState() || RestState::CanRest(bot)))
                {
                    restCommanded = true;
                    if (Leader::Events::SetReady(guid, Leader::ReadyEvent::Rest))
                        logEvent(bot, "Rest", JsonObject().Str("why", restWhy.empty() ? "sits: to full" : restWhy));
                }
                else if (lead.spotsWaiting)
                {
                    if (Leader::Events::SetReady(guid, Leader::ReadyEvent::ReadyCheck))
                        logEvent(bot, "Ready?", JsonObject().Str("why", "the pull waits for the spots"));
                }
                else if (group.NeedBuff() || rebuff || anyDead || anySitting || tankStill)
                {
                    char const* why = group.NeedBuff() || rebuff ? "the buff pass"
                                      : anyDead                ? "a member is dead"
                                      : anySitting             ? "a member rests: the group waits"
                                                               : "the tank stands";
                    if (Leader::Events::SetReady(guid, Leader::ReadyEvent::Prepare))
                        logEvent(bot, "Prepare", JsonObject().Str("why", why));
                }
            }

            // 2. the moves: out of a cleave (keeping the heal reach), the pull's spot, along with the tank
            bool moved = walking;
            // [lead150] b170 356-428 s: the leader's Follow kept moving a member that had to drink - the leader's Rest
            // owns its position: no move command while it holds (a sitter stands still, the Follow resumes after)
            if (restCommanded && !moved)
            {
                Leader::MemberEvents held;
                if (Leader::Events::Get(guid, held) && held.move != Leader::MoveEvent::None &&
                    held.move != Leader::MoveEvent::Hold)
                    Leader::Events::SetMove(guid, Leader::MoveEvent::Hold, bot->GetPosition(), 0.0f, "rests");
                moved = true;
            }
            if (Leader::LEADER_OWNS_DODGE && combat &&
                DungeonTactics::InFrontalCleave(bot, botAI, 1.0f, 20.0f * float(M_PI) / 180.0f))
            {
                hazardSeen = true;
                Position exit;
                Leader::MemberEvents current;
                bool const kept = Leader::Events::Get(guid, current) && current.move == Leader::MoveEvent::MoveTo &&
                                  current.moveWhy == "out of a cleave cone" &&
                                  !DungeonTactics::SpotInFrontalCleave(bot, botAI, current.spot.GetPositionX(),
                                                                       current.spot.GetPositionY(), 1.0f,
                                                                       20.0f * float(M_PI) / 180.0f);
                if (kept)
                    exit = current.spot;
                if (kept || slow([&]
                                 {
                                     return LeaderCleaveExit(
                                         bot, botAI, exit,
                                         bot == healer ? std::function<bool(Position const&)>(healerKeeps)
                                                       : std::function<bool(Position const&)>(keepsHealer));
                                 }))
                {
                    // [lead780] the cleave is a hazard like any other: stay or leave by the computation (the side
                    // step's seconds x its dps vs the cone spell's hits over the fight, survival, the heals)
                    Leader::HazardVerdict::Verdict verdict;
                    if (Leader::LEADER_HAZARD_STAY_OR_LEAVE)
                        if (Unit* cleaver = DungeonTactics::FrontalCleaverAt(bot, botAI, 1.0f,
                                                                              20.0f * float(M_PI) / 180.0f))
                        {
                            verdict = Leader::HazardVerdict::JudgeCleave(verdictFacts, bot, cleaver,
                                                                         bot->GetExactDist2d(exit));
                            std::set<std::string> seen;
                            judged(bot, "cleave: " + cleaver->GetName(), verdict, seen);
                        }
                    // stays in the cone: no move, its fight spot holds
                    if (!verdict.stays)
                    {
                        if (checkedMove(guid, Leader::MoveEvent::MoveTo, exit, 1.5f, "out of a cleave cone"))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                     JsonObject().Str("why", "out of a cleave cone").Str("verdict", verdict.why)
                                         .Real("x", exit.GetPositionX()).Real("y", exit.GetPositionY())
                                         .Real("z", exit.GetPositionZ()));
                        if (bot == healer)
                            healerAt = exit;
                        moved = true;
                    }
                }
                else if (Leader::LEADER_HEAL_REACH && healer)
                    LogHealReach(group, bot, bot == healer ? tank : healer, "no cleave exit keeps the heal reach",
                                 "none");
            }
            // [lead16] LEADER_HAZARD_SPOTS: out of every ground hazard the leader sees for this member (to the nearest
            // safe spot that keeps the heal reach), and held there while the hazard stays next to it
            if (!moved && Leader::LEADER_HAZARD_SPOTS)
            {
                std::vector<HazardZone> const& zones = lead.hazardZones[guid];
                HazardZone const* in =
                    zones.empty() ? nullptr
                                  : LeaderZoneAt(zones, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                                 0.5f);
                auto const held = lead.hazardSpots.find(guid);
                // [lead155] b174 20261101-155639 669.2-670.4 s: Rtshaab's escape from Taragaman's area cast was
                // computed again every tick from where it walked (-204.5 -> -203.4 -> -204.4), a new MoveTo each time
                // (command_repeat). The escape spot is decided once per hazard and kept while it stays safe, also
                // while the member still walks out (it is in the circle)
                bool const heldSafe =
                    held != lead.hazardSpots.end() && !zones.empty() &&
                    LeaderSpotSafe(bot, botAI, zones, held->second.GetPositionX(), held->second.GetPositionY(),
                                   held->second.GetPositionZ());
                bool const keepHeld =
                    heldSafe && (in || LeaderZoneAt(zones, held->second.GetPositionX(), held->second.GetPositionY(),
                                                    held->second.GetPositionZ(), LEADER_HAZARD_EDGE + 6.0f));
                if (in || keepHeld)
                {
                    hazardSeen = true;
                    Position spot;
                    bool found =
                        keepHeld ? (spot = held->second, true)
                                 : slow([&]
                                        {
                                            return LeaderSafeSpot(
                                                bot, botAI, zones,
                                                bot == healer ? std::function<bool(Position const&)>(healerKeeps)
                                                              : std::function<bool(Position const&)>(keepsHealer),
                                                spot);
                                        });
                    // [lead155] out of the damage first: no spot keeps the heal reach - the nearest safe one (logged
                    // as the heal-reach gap below), never a stay in the circle
                    bool healReachLost = false;
                    if (!found && in)
                    {
                        std::function<bool(Position const&)> const anySpot = [](Position const&) { return true; };
                        found = slow([&] { return LeaderSafeSpot(bot, botAI, zones, anySpot, spot); });
                        healReachLost = found;
                    }
                    if (healReachLost && Leader::LEADER_HEAL_REACH && healer)
                        LogHealReach(group, bot, bot == healer ? tank : healer,
                                     "no safe spot out of " + in->what + " keeps the heal reach", "the nearest safe");
                    if (found)
                    {
                        lead.hazardSpots[guid] = spot;
                        std::string why = std::string("hazard: ") + (in ? in->what : "held out of it");
                        // [lead155] the held escape keeps its command's text (no new command for a new label)
                        if (Leader::MemberEvents current; keepHeld && Leader::Events::Get(guid, current) &&
                                                          current.moveWhy.rfind("hazard: ", 0) == 0 &&
                                                          current.spot.GetExactDist(spot) <= 0.5f)
                            why = current.moveWhy;
                        if (checkedMove(guid, Leader::MoveEvent::MoveTo, spot, 1.0f, why))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                     JsonObject().Str("why", why).Str("verdict", lead.hazardVerdictWhy[guid])
                                         .Real("x", spot.GetPositionX()).Real("y", spot.GetPositionY())
                                         .Real("z", spot.GetPositionZ()));
                        if (bot == healer)
                            healerAt = spot;
                        moved = true;
                    }
                    else if (in)
                    {
                        if (Leader::LEADER_HEAL_REACH && healer)
                            LogHealReach(group, bot, bot == healer ? tank : healer,
                                         "no safe spot out of " + in->what + " keeps the heal reach", "none");
                        moved = true;  // [lead155] the hazard owns the member: no fight-spot walk in the circle
                    }
                }
                else
                    lead.hazardSpots.erase(guid);
            }

            // [lead59] b106: Rtdruaa combat spot -> "aggro loss: to the tank" -> combat spot (command_repeat) - two
            // rules owned its position. The run, once issued, holds until the member arrived (the mob may switch
            // back meanwhile); the combat spot does not overwrite it
            if (!moved && combat && lead.runHold.count(guid))
            {
                Leader::MemberEvents held;
                if (Leader::Events::Get(guid, held) && held.move == Leader::MoveEvent::MoveTo &&
                    (held.moveWhy == "aggro loss: to the tank" ||
                     held.moveWhy == "aggro loss: into the mob's reach") &&  // [lead430]
                    bot->GetExactDist2d(held.spot.GetPositionX(), held.spot.GetPositionY()) > held.tolerance)
                {
                    Leader::Events::SetMove(guid, held.move, held.spot, held.tolerance, held.moveWhy);
                    moved = true;
                }
                else
                    lead.runHold.erase(guid);
            }
            if (!combat)
                lead.runHold.erase(guid);
            // the aggro-loss victim runs next to the tank (between it and the tank, in heal reach)
            // [lead67] only a victim beyond the tank's pick-up reach (its taunt's range, in sight) runs: one standing in
            // it is picked up where it stands (b112: the shaman behind the target flipped run <-> combat spot)
            auto const loose = looseOn.find(guid);
            float const pickUp = LeaderTauntReach(tank);
            bool const inPickUp = pickUp > 0.0f && bot->GetExactDist(tank) <= pickUp && tank->IsWithinLOSInMap(bot);
            // [lead217] ... nor does a mob that stands off it (a caster at range): the run would only take the member
            // out of its spot; the tank goes to that mob (above)
            // [lead430] b202 rfc-h 5A 749-837 s: a Voidwalker Minion stood by the tank (0.7 yd) for 90 s on Rtmagaa
            // / Rtdruaa 13 / 26 yd off; it had no path to them (Creature::CanNotReachTarget), so the core evaded every
            // hit and taunt on it (IsEvadingAttacks), the tank's Attack failed ("the attack itself failed"), and every
            // 10 s the core dropped the threat of the one it could not reach for the next (EvadeTimerExpired) - the
            // healer's heal threat. The victim was in the taunt's reach and the mob stood off it, so nobody ran. A mob
            // that cannot reach its victim stops evading only once the victim stands in its reach: the victim runs to
            // the tank when the tank stands in the mob's melee reach, else to the mob
            Creature* const looseCreature = loose != looseOn.end() ? loose->second->ToCreature() : nullptr;
            bool const unreachable = looseCreature && looseCreature->CanNotReachTarget() &&
                                     looseCreature->GetCannotReachTarget() == guid &&
                                     !looseCreature->IsWithinMeleeRange(bot);
            if (!moved && unreachable)
            {
                Position spot =
                    looseCreature->IsWithinMeleeRange(tank) ? tank->GetPosition() : looseCreature->GetPosition();
                Leader::MemberEvents runEvents;
                bool const running = Leader::Events::Get(guid, runEvents) &&
                                     runEvents.move == Leader::MoveEvent::MoveTo &&
                                     runEvents.moveWhy == "aggro loss: into the mob's reach" &&
                                     runEvents.spot.GetExactDist(spot) <= 1.5f;
                if (running)
                    spot = runEvents.spot;
                lead.runHold.insert(guid);
                if (checkedMove(guid, Leader::MoveEvent::MoveTo, spot, 1.5f, "aggro loss: into the mob's reach") &&
                    !running)
                    logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", "aggro loss: into the mob's reach")
                                 .Str("mob", loose->second->GetName()).Real("gap", bot->GetExactDist(loose->second)));
                if (bot == healer)
                    healerAt = spot;
                moved = true;
            }
            if (!moved && loose != looseOn.end() && !inPickUp && !LooseStandsOff(loose->second, bot))
            {
                float const dx = bot->GetPositionX() - tank->GetPositionX();
                float const dy = bot->GetPositionY() - tank->GetPositionY();
                float const length = std::max(0.1f, std::hypot(dx, dy));
                float x = tank->GetPositionX() + dx / length * LEADER_LOOSE_TANK_YD;
                float y = tank->GetPositionY() + dy / length * LEADER_LOOSE_TANK_YD;
                float z = tank->GetPositionZ();
                Position spot = tank->GetPosition();
                if (map->CheckCollisionAndGetValidCoords(tank, tank->GetPositionX(), tank->GetPositionY(),
                                                         tank->GetPositionZ(), x, y, z) &&
                    (bot == healer ? healerKeeps(Position(x, y, z)) : keepsHealer(Position(x, y, z))))
                    spot.Relocate(x, y, z);
                // [lead55] b100: Rtdruaa 2 304 "aggro loss: to the tank" moves - the run's point is kept while it
                // still lies by the tank (within LEADER_LOOSE_TANK_YD and the walk's tolerance)
                Leader::MemberEvents runEvents;
                if (Leader::Events::Get(guid, runEvents) && runEvents.move == Leader::MoveEvent::MoveTo &&
                    runEvents.moveWhy == "aggro loss: to the tank" &&
                    runEvents.spot.GetExactDist(tank->GetPosition()) <= LEADER_LOOSE_TANK_YD + 1.5f)
                    spot = runEvents.spot;
                lead.runHold.insert(guid);  // [lead59]
                bool runs = checkedMove(guid, Leader::MoveEvent::MoveTo, spot, 1.5f, "aggro loss: to the tank");
                // [lead112] b147: the spot by the tank refused by the healer's own move guards, it stayed and died -
                // the tank's own spot is the second choice (the same checks before it is issued)
                if (!runs && spot.GetExactDist(tank->GetPosition()) > 0.5f)
                {
                    spot = tank->GetPosition();
                    runs = checkedMove(guid, Leader::MoveEvent::MoveTo, spot, 1.5f, "aggro loss: to the tank");
                }
                if (runs)
                    logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", "aggro loss: to the tank").Str("mob", loose->second->GetName())
                                 .Real("gap", bot->GetExactDist(tank)));
                if (bot == healer)
                    healerAt = spot;
                moved = true;
            }
            // [lead16] LEADER_COMBAT_SPOTS: the member's combat spot (StandAt), kept while it fits, a new one when the
            // target moved off it, it lost sight, a cleave or a hazard covers it or it broke the heal reach
            // [lead136] a melee member's combat spot is judged against the mob it attacks (its Attack target), not
            // the tank's target: a switch of the tank's target is no reason to move; its own target moving out of
            // reach is
            // [lead137] b163 20261101-124143 373.9-376.3 s: the hunter's spot was judged against the tank's target
            // (Ragefire Shaman) here and against its Attack target (Ragefire Trogg) by the Attack's own rule - two
            // re-spots in turn, command_repeat. Every damage dealer's spot is judged against its Attack target
            Unit* spotTarget = target;
            if (target && !PlayerbotAI::IsHeal(bot) && bot != healer)
            {
                Leader::MemberEvents own;
                if (Leader::Events::Get(guid, own) && own.attack == Leader::AttackEvent::Attack)
                    if (Unit* ownTarget = ObjectAccessor::GetUnit(*tank, own.target);
                        ownTarget && ownTarget->IsAlive() && ownTarget->IsInCombat())
                        spotTarget = ownTarget;
            }
            // [lead271] the hunter's trap weave: the leader's computed choice owns its move while it runs
            if (!moved && combat && spotTarget && spotTarget->IsAlive() && bot->getClass() == CLASS_HUNTER)
                moved = Leader::TrapWeave::TickMove(
                    group, bot, tank, spotTarget, seats,
                    [&](Position const& spot, std::string const& why)
                    {
                        if (checkedMove(guid, Leader::MoveEvent::MoveTo, spot, 1.0f, why))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                     JsonObject().Str("why", why).Str("target", spotTarget->GetName())
                                         .Real("x", spot.GetPositionX()).Real("y", spot.GetPositionY())
                                         .Real("z", spot.GetPositionZ()));
                        Leader::MemberEvents current;
                        return Leader::Events::Get(guid, current) && current.move == Leader::MoveEvent::MoveTo &&
                               current.moveWhy == why;
                    });
            // [lead840] b223 rfc-h 5H Bazzalan 1157.9-1161.8 s: the boss gate passed the fire mage at 2 % on its
            // Evocation's 60 % ("Evocation in the fight"); the leader told it Evocation at the pull and 0.6 s later
            // its first combat spot ("no line of sight to the target", a 3 yd walk) - the walk broke the channel, the
            // 240 s recharge started, the mage was at 1 % at 1161.8 s and wanded 1191-1213 s. The mana tool's channel
            // is the member's fact while it runs: no combat spot moves it for its target's sight or range (its tool
            // needs neither); only its place's safety does (a fight mob's melee reach, a hazard, lava / a drop-off,
            // a cleave)
            {
                thread_local std::unordered_map<ObjectGuid, std::string> channelVerdict;  // logged once per change
                std::string verdict;
                if (!moved && combat && LeaderChannelsManaTool(bot))
                {
                    float const x = bot->GetPositionX();
                    float const y = bot->GetPositionY();
                    float const z = bot->GetPositionZ();
                    if (!LeaderOutOfMobsMelee(bot, bot->GetPosition(), killOrder, true))
                        verdict = "gives way: inside a fight mob's melee reach";
                    else if (LeaderZoneAt(lead.hazardZones[guid], x, y, z, LEADER_HAZARD_EDGE))
                        verdict = "gives way: a hazard covers it";
                    else if (NearHazard(bot->GetMap(), bot->GetPhaseMask(), x, y, z, LEADER_TERRAIN_EDGE))
                        verdict = "gives way: lava / a drop-off";
                    else if (botAI && DungeonTactics::SpotInFrontalCleave(bot, botAI, x, y, 1.0f,
                                                                          20.0f * float(M_PI) / 180.0f))
                        verdict = "gives way: a cleave covers it";
                    else
                    {
                        verdict = "holds its place (no combat spot until it ends)";
                        moved = true;
                    }
                }
                std::string& logged = channelVerdict[guid];
                if (!verdict.empty() && verdict != logged)
                    LOG_INFO("playerbots", "[lead840] {}: its mana tool's channel {}", bot->GetName(), verdict);
                logged = verdict;
            }
            if (!moved && combat && Leader::LEADER_COMBAT_SPOTS && target && target->IsAlive())
            {
                Unit* const target = spotTarget;  // the block below judges the spot against it
                bool const melee = !PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsHeal(bot);
                std::vector<HazardZone> const& zones = lead.hazardZones[guid];
                std::function<bool(Position const&)> const keepsBase =
                    bot == healer ? std::function<bool(Position const&)>(healerSpotKeeps)  // [lead490]
                                  : std::function<bool(Position const&)>(keepsHealer);
                // [lead218] b184 rfc-h 5H 892-944 s: the MM hunter stood on its spot 30 yd from Taragaman, every shot
                // "no line of sight" for 53 s, the spot judged fine. Standing on its spot, the game's own answer
                // (MemberCastsHere) is the fact: the spot breaks and is the one it leaves
                bool blindOnSpot = false;
                if (auto const held = lead.combatSpots.find(guid);
                    !melee && bot != healer && !PlayerbotAI::IsHeal(bot) && held != lead.combatSpots.end() &&
                    held->second.first == target->GetGUID() &&
                    getMSTimeDiff(lead.combatSpotLosMs[guid], now) >= LEADER_SPOT_LOS_MS &&
                    bot->GetExactDist2d(held->second.second.GetPositionX(), held->second.second.GetPositionY()) <=
                        2.0f &&
                    !MemberCastsHere(bot, target))
                {
                    blindOnSpot = true;
                    LeaveCombatSpot(lead.combatSpotLeft[guid], held->second);
                }
                // [lead137] never back to the spot it just left for this target
                auto const left = lead.combatSpotLeft.find(guid);
                auto const notLeft = [&](Position const& at)
                {
                    return !(left != lead.combatSpotLeft.end() &&
                             std::any_of(left->second.begin(), left->second.end(),
                                         [&](std::pair<ObjectGuid, Position> const& was)
                                         {
                                             return was.first == target->GetGUID() &&
                                                    was.second.GetExactDist(at) <= COMBAT_SPOT_LEFT_YD;
                                         }));
                };
                std::function<bool(Position const&)> const keeps = [&](Position const& at)
                { return keepsBase(at) && notLeft(at); };
                // [lead380] the healer's own place keeps it when it heals the tank from there where the tank stands
                // (its reach and sight, within LEADER_HEALER_TANK_YD); the other members' reach is the heal-reach
                // rule's, on its facts
                std::function<bool(Position const&)> const keepsHere =
                    bot != healer ? keeps
                                  : std::function<bool(Position const&)>(
                                        [&](Position const& at)
                                        {
                                            Position const tankNow = tank->GetPosition();
                                            return reaches(at, tankNow) &&
                                                   at.GetExactDist(tankNow) <=
                                                       Leader::LEADER_HEALER_TANK_YD - g_spotAcquireMargin &&
                                                   notLeft(at);
                                        });
                // [lead380] b200 rfc-h (8 runs): ranged members and healers walked 1543 s / 554 s of the fights to
                // combat spots, most of them from a place they cast from. Where a ranged member or a healer stands is
                // its spot when it fits there as a new spot would (SpotAcquire's margin): its Attack target in its
                // spells' range and its sight (the game's own), out of every fight mob's melee reach where the mobs
                // stand now, safe, in heal reach (a healer: the tank in its reach), not a spot it just left. It casts
                // there, no walk
                // [lead397] `whereTheyStand`: a mob on the move judged where it stands now (the place the member
                // stands on), else where it runs to (the facts a walk was decided on)
                auto const standsFit = [&](bool whereTheyStand) -> bool
                {
                    if (melee)
                        return false;
                    bool fits = false;
                    {
                        SpotAcquire const acquire;
                        fits = LeaderCombatSpotBreaks(bot, botAI, target, bot->GetPosition(), false, zones, keepsHere,
                                                      true, &killOrder, whereTheyStand)
                                   .empty();
                    }
                    return fits && (bot == healer || PlayerbotAI::IsHeal(bot) || MemberCastsHere(bot, target));
                };
                // [lead380] why it does not (logged with its walks): the same checks, "" when it fits
                auto const standsFails = [&]() -> std::string
                {
                    std::string why;
                    {
                        SpotAcquire const acquire;
                        why = LeaderCombatSpotBreaks(bot, botAI, target, bot->GetPosition(), melee, zones,
                                                     melee ? keeps : keepsHere, true, &killOrder, true);
                    }
                    if (why.empty() && !melee && bot != healer && !PlayerbotAI::IsHeal(bot) &&
                        !MemberCastsHere(bot, target))
                        why = "the game's sight / range to the target";
                    return why;
                };
                auto found = lead.combatSpots.find(guid);
                uint32& losMs = lead.combatSpotLosMs[guid];
                bool const sight = !losMs || getMSTimeDiff(losMs, now) >= LEADER_SPOT_LOS_MS;
                if (sight)
                    losMs = now;
                // [lead156] b175 20261101-161451 733.4-735.5 s: the shaman's "behind the target" StandAt moved with its
                // walking Cultist every tick ((-254,209.2) -> (-254,206.2) -> (-251,207.1), command_repeat). While a
                // melee member's target walks (it is on the tank: the walk is the fight's, not its run in), its one
                // command is the chase - a Follow behind that mob, issued once per target; the spot "behind the
                // target" once it stands
                // [lead162] wowsims audit (rfc-h wave b178): 1889 move orders vs 2888 casts, the enh shaman 310 vs
                // 225 - the chase while the mob walked, the StandAt behind it once it stood, again on its next step. One
                // decision per fact: a melee member on a mob the tank holds keeps the one Follow behind that mob,
                // walking or standing (the Follow walks only out of melee reach, lead160); a new target is the next
                // [lead192] b181 (8 runs): 318 overrides chase -> "combat spot: behind the target" and 2103 chase
                // Follows - the chase ended whenever the mob stepped out of the tank's melee reach for a moment and
                // a StandAt came, then the chase again. The chase of a mob on the tank holds while it stays on the
                // tank (the fact it began with); a new target or the mob off the tank ends it
                Leader::MemberEvents chaseNow;
                bool const chasing = melee && Leader::Events::Get(guid, chaseNow) &&
                                     chaseNow.move == Leader::MoveEvent::Follow &&
                                     chaseNow.followTarget == target->GetGUID();
                // [lead374] the chase only when the member's walk behind the mob takes no idle mob's reach (DryFollow);
                // a chase it cannot walk is no chase: its combat spot (checked spots) holds it, and the pack in the
                // way is judged for the fight (lead76)
                std::optional<bool> chaseBlocked;
                auto const chaseRefused = [&]() -> bool
                {
                    if (chaseBlocked)
                        return *chaseBlocked;
                    // [lead801] b221 rfc-h (8 runs): 99 flips Follow "chase: behind the target" <-> StandAt "combat
                    // spot: behind the target" within 3 s (5A 23.3 / 23.6 s Rtshaab, 5C 204.0 / 204.3 / 204.6 s
                    // Rtwarab): the chase's walk was judged again every tick from where the member had just walked -
                    // refused, then clear one step on, then refused again ("the chase walks deeper into Ragefire
                    // Trogg's reach than the fight stands"). The member's own walk is no fact: the verdict for this
                    // target holds until the target stands somewhere else (its combat reach away from where it was
                    // judged; while it walks the verdict holds), or the idle mob that refused it is no longer idle
                    // (dead, in combat, its pack joined the fight)
                    auto& held = lead.chaseVerdicts[guid];
                    bool fresh = held.target != target->GetGUID() ||
                                 (!target->isMoving() &&
                                  target->GetExactDist2d(held.targetAt.GetPositionX(), held.targetAt.GetPositionY()) >
                                      target->GetCombatReach());
                    if (!fresh && held.refused && !held.blocker.IsEmpty())
                    {
                        Creature* const blocker = map->GetCreature(held.blocker);
                        fresh = !blocker || !blocker->IsAlive() || blocker->IsInCombat() ||
                                lead.joinedMobs.count(held.blocker);
                    }
                    if (!fresh)
                    {
                        chaseBlocked = held.refused;
                        if (lead.chaseHeldLogged.insert({guid, target->GetGUID()}).second)
                            LOG_INFO("playerbots", "[lead801] {}: the chase verdict on {} holds ({}) until {} stands "
                                     "elsewhere or its blocker wakes",
                                     bot->GetName(), target->GetName(), held.refused ? "refused" : "clear",
                                     target->GetName());
                        return *chaseBlocked;
                    }
                    ObjectGuid mob;
                    std::string const refusal =
                        followRefusal(bot, target, uint8(index % 2), mob, true);  // [lead491] the chase
                    chaseBlocked = !refusal.empty();
                    held = {target->GetGUID(), *chaseBlocked, mob, target->GetPosition()};
                    if (!*chaseBlocked)
                        return false;
                    Creature* woken = mob.IsEmpty() ? nullptr : map->GetCreature(mob);
                    thread_local std::map<ObjectGuid, std::pair<ObjectGuid, ObjectGuid>> chaseLogged;
                    if (chaseLogged.size() > 4096)
                        chaseLogged.clear();
                    auto& logged = chaseLogged[guid];
                    if (logged != std::make_pair(target->GetGUID(), mob))
                    {
                        logged = {target->GetGUID(), mob};
                        Decisions().Write(run, "leader", "chase_refused", guid, bot->GetName(), true,
                                          JsonObject()
                                              .Str("member", bot->GetName())
                                              .Str("target", target->GetName())
                                              .Str("why", refusal)
                                              .Str("mob", woken ? woken->GetName() : "-"));
                    }
                    judgeBlockingPack(bot, woken);
                    return true;
                };
                // [lead454] in reach in front of a trash mob whose remaining life loses less to its parry / block
                // than the walk round it costs: the member stays where it stands (no chase, its spot is here)
                // [lead680] b218 rfc-h (8 runs, fight_breakdown): melee walked 1198 s under "chase: behind the
                // target", 483 s of it with the tank standing, most of it the walk in from out of reach (the target
                // switch to a mob at the tank's other side, the first walk in) straight past the near side to the
                // back. Out of reach too the leader weighs the walk round against the front's loss: the nearest place
                // in reach (frontAt) when the extra walk to the back costs more than the parry / block over the mob's
                // life (the core's roll with the member's expertise and the level difference)
                // [lead720] b219 rfc-h (8 runs): 590 flips StandAt "behind the target" <-> Follow "chase: behind the
                // target" within 1 s on the same target (5A 393.6 / 393.9 s, 412.6 / 413.5 s, 5G 608.3 / 608.6 s):
                // the front-or-back weighing ran again every tick from where the member had just walked - the near
                // place moved with it, the walk round shrank as it went, the mob's life (the front's loss) fell - so
                // the chase stopped where it stood ("stops its walk") and went on the next tick. One decision per
                // fact: the side weighed for this target holds - the front while its held place still hits the mob
                // from its front half, the chase while it walks - until the target changes or that place no longer
                // reaches it; it is weighed again only then (and only once the weighing is made: the mob standing on
                // its victim)
                std::string frontWhy;
                Position frontAt = bot->GetPosition();
                bool frontHere = false;
                if (melee && target->GetVictim() != bot)
                {
                    auto const side = lead.meleeSide.find(guid);
                    bool const decided = side != lead.meleeSide.end() && side->second.first == target->GetGUID();
                    bool const heldFront = decided && side->second.second && found != lead.combatSpots.end() &&
                                           found->second.first == target->GetGUID() &&
                                           target->HasInArc(float(M_PI), &found->second.second) &&
                                           LeaderMeleeReachesFrom(bot, target, found->second.second);
                    // [lead800] b221 rfc-h (8 runs): 72 of the 99 chase <-> spot flips were the front weighed on a mob
                    // that stood for a tick (melee_front, StandAt in front) and walked on the next (the weighing has
                    // no answer on a walking mob: the chase again), then stood again. The side weighed for this
                    // target holds while the mob walks (its front place is kept: no chase), the back holds for the
                    // target whether the member chases or stands behind it; the front is weighed again only once the
                    // mob stands where its held front place no longer reaches it
                    bool const frontWhileWalks = decided && side->second.second && target->isMoving() &&
                                                 found != lead.combatSpots.end() &&
                                                 found->second.first == target->GetGUID();
                    if (heldFront || frontWhileWalks)
                    {
                        frontHere = true;
                        frontAt = found->second.second;
                        if (frontWhileWalks && lead.sideHeldLogged.insert({guid, target->GetGUID()}).second)
                            LOG_INFO("playerbots", "[lead800] {}: its front place at {} holds while it walks",
                                     bot->GetName(), target->GetName());
                    }
                    else if (decided && !side->second.second)
                    {
                        if (lead.sideHeldLogged.insert({guid, target->GetGUID()}).second)
                            LOG_INFO("playerbots", "[lead800] {}: its side behind {} holds for this target",
                                     bot->GetName(), target->GetName());
                    }
                    else
                    {
                        frontHere = LeaderFrontCostsLess(bot, target, &frontWhy, &frontAt);
                        // [lead843] b223 rfc-h (8 runs): 72 of the 103 chase <-> "combat spot: behind the target"
                        // flips kept one target (5A 113.9 / 115.4 / 116.0 s Rtshaab on a Ragefire Shaman whose front
                        // place "holds while it walks"; 5G 621.8-625.4 s Rtwarad, Rtpalae, Rtdekad x7): the front was
                        // decided, the mob turned to the tank and stood, its held front place left its front half,
                        // and the weighing ran again - "the near place is in its back half already" (no answer: the
                        // chase), one step of the chase later the near place was in its front half again (the front
                        // again). The front decided for this target holds: in the mob's melee reach the member hits
                        // it from where it stands (the side it stands on costs no more than the front it accepted);
                        // out of it the weighing's place, else the chase. Weighed anew only for a new target
                        if (!frontHere && decided && side->second.second && bot->IsWithinMeleeRange(target) &&
                            bot->IsWithinLOSInMap(target))
                        {
                            frontHere = true;
                            frontAt = bot->GetPosition();
                            frontWhy.clear();
                            thread_local std::set<std::pair<ObjectGuid, ObjectGuid>> keptLogged;
                            if (keptLogged.size() > 4096)
                                keptLogged.clear();
                            if (keptLogged.insert({guid, target->GetGUID()}).second)
                                LOG_INFO("playerbots", "[lead843] {}: its front at {} holds where it stands in reach",
                                         bot->GetName(), target->GetName());
                        }
                        else if (!frontWhy.empty())
                            lead.meleeSide[guid] = {target->GetGUID(), frontHere};
                    }
                }
                if (frontHere && !frontWhy.empty())
                {
                    thread_local std::set<std::pair<ObjectGuid, ObjectGuid>> frontLogged;
                    if (frontLogged.size() > 4096)
                        frontLogged.clear();
                    if (frontLogged.insert({guid, target->GetGUID()}).second)
                        Decisions().Write(run, "leader", "melee_front", guid, bot->GetName(), true,
                                          JsonObject().Str("member", bot->GetName()).Str("target", target->GetName())
                                              .Str("why", frontWhy));
                }
                // [lead740] the tank walks its mob away (the walk back of a pull back / LoS pull, "move fight spot
                // back", a fight drag, a hazard move): the melee member goes to its place at the walk's end
                // (meleeStopAt), where the mob stops, not on a Follow behind the walking mob (b220 rfc-h: the chase
                // took the melee out to the mob and back with it; 5B 794-800 s, 5G 720-727 s, 5A 850-863 s)
                Position tankStop;
                bool toTankStop = melee && target->GetVictim() == tank && tankWalksMobTo(target, tankStop);
                // [lead802] b221 rfc-h (8 runs): 28 flips chase <-> MoveTo "to the tank's walk's end", 20 walk's end
                // <-> combat spot, 18 walk's end <-> "to the tank's stop point" (5C 202.2 / 202.5 / 203.1 s Rtwarab):
                // the walk was read from tank->isMoving() each tick, and the tank's walk pauses between its steps.
                // The walk the member was sent to the end of is over when the fight stands - the tank and the mob
                // both still - not when the tank's spline pauses: the walk's end holds until then
                if (!toTankStop && melee && target->GetVictim() == tank && chaseNow.move == Leader::MoveEvent::MoveTo &&
                    chaseNow.moveWhy == "to the tank's walk's end" && (tank->isMoving() || target->isMoving()))
                    if (auto const kept = lead.meleeStop.find(guid);
                        kept != lead.meleeStop.end() && kept->second.target == target->GetGUID())
                    {
                        toTankStop = true;
                        if (lead.walkEndHeldLogged.insert({guid, target->GetGUID()}).second)
                            LOG_INFO("playerbots", "[lead802] {}: the tank's walk's end with {} holds until the fight "
                                     "stands", bot->GetName(), target->GetName());
                    }
                bool const chases = !toTankStop && melee && !frontHere && target->GetVictim() == tank &&
                                    (chasing || tank->IsWithinMeleeRange(target)) && !chaseRefused();
                if (toTankStop)
                {
                    Position const place = meleeStopAt(bot, target);
                    if (checkedMove(guid, Leader::MoveEvent::MoveTo, place, 3.0f, "to the tank's walk's end"))
                        logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                 JsonObject().Str("why", "to the tank's walk's end")
                                     .Str("target", target->GetName())
                                     .Real("x", place.GetPositionX()).Real("y", place.GetPositionY())
                                     .Real("z", place.GetPositionZ()));
                    lead.combatSpots.erase(guid);
                    found = lead.combatSpots.end();
                    lead.meleeChase.erase(guid);
                    moved = true;
                }
                else if (chases)
                {
                    if (Leader::Events::SetFollow(guid, target->GetGUID(), uint8(index % 2),
                                                  "chase: behind the target"))
                        logEvent(bot, "Follow",
                                 JsonObject().Str("why", "chase: behind the target").Str("target", target->GetName()));
                    lead.combatSpots.erase(guid);
                    found = lead.combatSpots.end();  // [lead442] the erase freed the node found pointed at (ASan)
                    moved = true;
                }
                // [lead190] the chase of a walked-off target holds until the member is in its melee reach again
                else if (auto const chase = lead.meleeChase.find(guid);
                         melee && !frontHere && chase != lead.meleeChase.end() &&
                         chase->second == target->GetGUID() &&
                         !bot->IsWithinMeleeRange(target) && !chaseRefused())
                {
                    Leader::Events::SetFollow(guid, target->GetGUID(), uint8(index % 2), "chase: behind the target");
                    moved = true;
                }
                else if (Leader::LEADER_ANCHORED_SPOTS)
                {
                    lead.meleeChase.erase(guid);
                    // [lead18] anchored: kept for the whole fight while it fits; a target change alone never moves
                    // anyone; a new spot is the nearest that fits (from where the member stands), else a fresh one
                    std::string breaks;
                    if (found == lead.combatSpots.end())
                        breaks = "first spot";
                    // [lead38] b86: 135 "the fight spot moved" re-spots - a new anchor re-spots a member only when its
                    // held spot no longer fits; a ranged / healer spot is not re-judged while the target runs in and
                    // the tank does not hold it yet (b87 pull 1: spots chased the incoming Earthborer)
                    // [lead64] melee too: no spot chases a mob still running to the tank (b109 30.6-31.5 s)
                    else if (target->isMoving() &&
                             (target->GetVictim() != tank || !tank->IsWithinMeleeRange(target)))
                        breaks.clear();
                    else
                    {
                        // [lead137] b162: the combat-spot rule moved the mage 268 s, the hunter 247 s, the shaman 296 s
                        // a run. A held spot is left only when its Attack target is out of its reach or sight from it
                        // (melee: out of melee reach), or a hazard / cleave / drop-off covers it - not because the
                        // fight's anchor or the target moved a little, nor for the heal reach (its own rule)
                        // [lead380] the held spot under a ranged member's / healer's feet is judged by the facts
                        // (the mobs where they stand now), one it still walks to by where the mobs will be
                        bool const onHeld = !melee && bot->GetExactDist2d(found->second.second.GetPositionX(),
                                                                          found->second.second.GetPositionY()) <= 2.0f;
                        breaks = LeaderCombatSpotBreaks(bot, botAI, target, found->second.second, melee, zones,
                                                        keeps, sight, &killOrder, onHeld);  // [lead210]
                        if (breaks == "heal reach")
                            breaks.clear();
                        if (breaks.empty() && blindOnSpot)  // [lead218]
                            breaks = "it cannot cast on its target from its spot";
                        lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;
                        // [lead190] b180 rfc-h 5G 103.2-105.0 s: the ret paladin's spot "behind" a Ragefire Shaman
                        // that walked off broke "out of melee reach" every tick, each new StandAt stopped its walk to
                        // the last one (member_ack "stops its walk") - it never moved, out of its Consecration and
                        // its target. A melee member's held spot for this target that the target walked away from is
                        // the chase: one Follow behind that mob (lead156), the spot again once it stands
                        if (melee && found->second.first == target->GetGUID() &&
                            breaks.rfind("out of melee reach", 0) == 0 && !frontHere && !chaseRefused())
                        {
                            if (Leader::Events::SetFollow(guid, target->GetGUID(), uint8(index % 2),
                                                          "chase: behind the target"))
                                logEvent(bot, "Follow", JsonObject().Str("why", "chase: behind the target")
                                                            .Str("target", target->GetName()));
                            lead.combatSpots.erase(guid);
                            lead.meleeChase[guid] = target->GetGUID();
                            found = lead.combatSpots.end();
                            breaks.clear();
                            moved = true;
                        }
                    }
                    // [lead454] in front of the trash mob where the front costs less than the walk: where it stands
                    // is its spot when the spot's other checks hold there (no walk behind, a spot in front no break)
                    if (frontHere)
                    {
                        std::string const standing = LeaderCombatSpotBreaks(bot, botAI, target, frontAt, true, zones,
                                                                            keeps, true, &killOrder, true);
                        if (standing.empty() || standing == "in front of the target")
                        {
                            // [lead680] a held front spot for this target the member still hits it from is kept (one
                            // decision per fact: the near place moves with the member's own walk in)
                            bool const heldFits = found != lead.combatSpots.end() &&
                                                  found->second.first == target->GetGUID() &&
                                                  target->HasInArc(float(M_PI), &found->second.second) &&
                                                  LeaderMeleeReachesFrom(bot, target, found->second.second);
                            if (!heldFits && (found == lead.combatSpots.end() ||
                                              found->second.first != target->GetGUID() ||
                                              frontAt.GetExactDist2d(found->second.second.GetPositionX(),
                                                                     found->second.second.GetPositionY()) > 1.0f))
                            {
                                lead.combatSpots[guid] = {target->GetGUID(), frontAt};
                                lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;
                                found = lead.combatSpots.find(guid);
                            }
                            breaks.clear();
                        }
                    }
                    // [lead38] no first spot for a ranged / healer while the target runs in: it stays (and casts)
                    // [lead882] b224 rfc-h 5E 1087.7-1092.9 s: the destruction warlock Rtlocab stood where it saw
                    // neither Bazzalan running in nor the tank's stop point (fight_anchor_ahead): every cast "no line
                    // of sight" until the boss stood on the tank. A ranged damage dealer that casts on its target
                    // from where it stands, or sees where the fight lands, stays; one that does neither takes its
                    // first spot now
                    Position const landing = lead.fightAnchorSerial ? lead.fightAnchor : tank->GetPosition();
                    bool const blindToFight =
                        breaks == "first spot" && target->isMoving() && !melee && bot != healer &&
                        !PlayerbotAI::IsHeal(bot) && !MemberCastsHere(bot, target) &&
                        !bot->IsWithinLOS(landing.GetPositionX(), landing.GetPositionY(),
                                          landing.GetPositionZ() + tank->GetCollisionHeight());
                    if (blindToFight)
                    {
                        thread_local std::set<uint64> blindLogged;
                        if (blindLogged.insert(bot->GetGUID().GetRawValue() ^ target->GetGUID().GetRawValue()).second)
                            LOG_INFO("playerbots", "[lead882] {}: sees neither {} running in nor the fight's landing: "
                                     "takes its first spot now",
                                     bot->GetName(), target->GetName());
                    }
                    if (breaks == "first spot" && target->isMoving() && !blindToFight &&
                        (target->GetVictim() != tank || !tank->IsWithinMeleeRange(target)))
                    {
                        breaks.clear();
                        moved = true;
                        // [lead684] b218 rfc-h (8 runs, fight_breakdown): 197 s of first-hit delay and ~50 s of
                        // ranged / healer walking were "Follow: along with the tank [given before the pull]" - "it
                        // stays" here issued no command, so the pre-pull Follow stayed live and walked the member on
                        // behind the tank while the pulled mob ran in. A ranged member that casts on its target from
                        // where it stands (a healer: heals the tank from there, HealReaches) stays there: its live
                        // command is the stand, not the walk along
                        if (!melee)
                        {
                            Leader::MemberEvents live;
                            bool const walksAlong = Leader::Events::Get(guid, live) &&
                                                    live.move == Leader::MoveEvent::Follow &&
                                                    live.moveWhy.rfind("along with the tank", 0) == 0;
                            bool const healing = bot == healer || PlayerbotAI::IsHeal(bot);
                            bool const actsHere = healing ? HealReaches(tank, bot->GetPosition(),
                                                                        tank->GetPosition(), bot)
                                                          : MemberCastsHere(bot, target);
                            if (walksAlong && actsHere &&
                                Leader::Events::SetMove(guid, Leader::MoveEvent::StandAt, bot->GetPosition(), 2.0f,
                                                        "stays: acts from here while the target runs in"))
                                logEvent(bot, MoveEventName(Leader::MoveEvent::StandAt),
                                         JsonObject().Str("why", "stays: acts from here while the target runs in")
                                             .Str("target", target->GetName()));
                        }
                        // [lead137] no "along with the tank" walk in a fight (b162: the shaman 83 s): the member
                        // stays until the mob stands on the tank, then takes its spot
                        // [lead465] user 2026-11-02 (Q15): a melee member goes at once to where the tank will stop -
                        // the leader's meeting point of the tank and the pulled mob (an engage: at the mob, the
                        // tank's charge lands there too; a pull back / range pull: the fight spot the mob runs to) -
                        // instead of standing at its pull spot until the mob is on the tank (b207: ~4 s a pull). Its
                        // place there is the meeting place of the running mob and the tank (lead730: meleeStopAt);
                        // checkedMove judges the spot and the walk against every unpulled mob's reach
                        if (melee && lead.commit && lead.commitIssued && !lead.commitMob.IsEmpty())
                        {
                            std::vector<ObjectGuid> const pulled = LeaderPullMobs(lead.commit, lead.commitMob);
                            if (target->GetGUID() == lead.commitMob ||
                                std::find(pulled.begin(), pulled.end(), target->GetGUID()) != pulled.end())
                            {
                                // [lead730] where the running mob and its victim meet (meleeStopAt), not the pull's
                                // meeting point of the issue (b219: 167 stays, the engaged mob's spawn point inside
                                // its pack's reach); the pulled mobs' reach is no constraint once the tank has them
                                Position const stopAt = meleeStopAt(bot, target);
                                if (checkedMove(guid, Leader::MoveEvent::MoveTo, stopAt, 3.0f,
                                                "to the tank's stop point"))
                                    logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                             JsonObject().Str("why", "to the tank's stop point")
                                                 .Str("target", target->GetName())
                                                 .Real("x", stopAt.GetPositionX()).Real("y", stopAt.GetPositionY())
                                                 .Real("z", stopAt.GetPositionZ()));
                            }
                        }
                    }
                    if (!breaks.empty())
                    {
                        Position spot;
                        // [lead137] where the member stands is its spot when it fits there (reaches and sees its
                        // target, safe, in heal reach): no walk
                        // [lead380] for every break, not only the first spot, by the cast facts (standsFit), not by the
                        // band of LeaderCombatSpotValid (b200: 107 healer and 41 caster first spots walked 1-2 yd
                        // from a place that fit)
                        SpotRejects rejects;  // [lead455] each rejected candidate's reason, for combat_spot_none
                        bool const here = standsFit(true);
                        if (here)
                        {
                            spot = bot->GetPosition();
                            Decisions().Write(run, "leader", "combat_spot_here", guid, bot->GetName(), true,
                                              JsonObject().Str("member", bot->GetName()).Str("why", breaks)
                                                  .Str("target", target->GetName()));
                        }
                        bool const got =
                            here || slow([&]
                                         {
                                             SpotAcquire const acquire;  // [lead36]
                                             return LeaderNearestCombatSpot(bot, botAI, target, bot->GetPosition(),
                                                                            melee, zones, keeps, spot, &killOrder) ||
                                                    LeaderCombatSpot(bot, botAI, target, melee, zones, keeps, spot,
                                                                     &killOrder);  // [lead214]
                                         });
                        if (got)
                        {
                            // [lead137] [lead375] left for the target it was judged against (the held spot may
                            // still name the one before: b197b 5H the mage went back to the spot it had left)
                            // [lead380] what its own place fails, judged before the spot it leaves is marked left
                            std::string const hereFails = here || melee ? std::string("-") : standsFails();
                            if (found != lead.combatSpots.end())
                                LeaveCombatSpot(lead.combatSpotLeft[guid], {target->GetGUID(), found->second.second});
                            if (!here && breaks != "first spot")
                                Decisions().Write(
                                    run, "leader", "combat_respot", guid, bot->GetName(), true,
                                    JsonObject().Str("member", bot->GetName()).Str("why", breaks)
                                        .Str("target", target->GetName())
                                        .Str("hereFails", hereFails)
                                        .Real("walk", bot->GetExactDist(spot)));
                            // [lead250] b191: the ranged "combat spot" walks were 2439 s of the 8 runs' in-fight
                            // moving; a first spot away from where the member stands logs what its own place fails
                            else if (!here && !melee)
                                Decisions().Write(
                                    run, "leader", "combat_first_spot", guid, bot->GetName(), false,
                                    JsonObject()
                                        .Str("member", bot->GetName())
                                        .Str("target", target->GetName())
                                        .Str("hereFails", hereFails)
                                        .Real("walk", bot->GetExactDist(spot))
                                        .Real("targetGap", bot->GetExactDist(target)));
                            lead.combatSpots[guid] = {target->GetGUID(), spot};
                            lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;
                        }
                        else if (found != lead.combatSpots.end() && breaks.rfind("the fight spot moved", 0) == 0)
                            lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;  // keep it, nothing better
                        else
                        {
                            lead.combatSpots.erase(guid);
                            // [lead470] user 2026-11-02: no default behaviour - a member with no spot is a leader bug.
                            // The ranged "tank trail" fallback (lead151 / lead152 / lead153 / lead218) is gone: the
                            // ranged search covers its whole band (lead452 / lead463), a ranged member or healer that
                            // still finds none is combat_spot_none, an error to fix in the search (b207: 52 ranged
                            // "first spot" and 21 "out of spell range" walks onto the tank's trail).
                            // A melee member belongs at the target: when the behind-the-target ring has no spot, its
                            // spot is a melee spot at the target - the tank's own spot when that one is in the
                            // member's melee reach of the target on its floor and safe, else the nearest safe point in
                            // melee reach round the target on its floor that no unpulled mob reaches (b207: 230 melee
                            // fallbacks to the tank's spot, 43 of them "out of melee reach of the target" - the tank
                            // stood away from the mob)
                            bool const reaches = melee ? bot->IsWithinMeleeRange(target) : MemberCastsHere(bot, target);
                            float const meleeReach = target->GetCombatReach() + bot->GetCombatReach() + 1.0f;
                            Creature* idleBlocker = nullptr;  // [lead734] the first idle mob a melee place met
                            auto const meleeAtTarget = [&](Position const& at, char const* search)
                            {
                                if (target->GetExactDist2d(at.GetPositionX(), at.GetPositionY()) > meleeReach)
                                {
                                    NoteSpotReject(search, "out of melee reach");
                                    return false;
                                }
                                if (std::fabs(at.GetPositionZ() - target->GetPositionZ()) > 4.0f)
                                {
                                    NoteSpotReject(search, "off the target's floor");
                                    return false;
                                }
                                if (!LeaderSpotSafe(bot, botAI, zones, at.GetPositionX(), at.GetPositionY(),
                                                    at.GetPositionZ()))
                                {
                                    NoteSpotReject(search, "unsafe (hazard / drop-off / cleave)");
                                    return false;
                                }
                                // [lead490] no deeper into an idle mob's reach than the fight at the target stands
                                if (Creature* idle = LeaderFightAggroAt(bot, at, target))
                                {
                                    NoteSpotReject(search, "an idle mob's aggro reach");
                                    if (!idleBlocker)
                                        idleBlocker = idle;  // [lead734]
                                    return false;
                                }
                                return true;
                            };
                            bool placed = false;
                            Position chosen;
                            char const* from = "";
                            // [lead478] user 2026-11-02: a melee member in reach with no spot behind the target takes
                            // its own place in front of it - a real melee spot at the target - when it fits there
                            if (melee && bot != tank && reaches && meleeAtTarget(bot->GetPosition(), "where it stands"))
                            {
                                chosen = bot->GetPosition();
                                from = "where it stands at the target";
                                placed = true;
                            }
                            else if (melee && bot != tank && tank->IsAlive())
                            {
                                if (meleeAtTarget(tank->GetPosition(), "tank's spot"))
                                {
                                    chosen = tank->GetPosition();
                                    from = "the tank's spot at the target";
                                    placed = true;
                                }
                                else
                                {
                                    // round the target from its back, both ways, at the ring's melee distance
                                    constexpr uint32 DIRECTIONS = 16;
                                    float const distance =
                                        std::max(1.5f, target->GetCombatReach() + bot->GetCombatReach() * 0.5f);
                                    float const back = target->GetOrientation() + float(M_PI);
                                    float best = -1.0f;
                                    Map* map = target->GetMap();
                                    for (uint32 i = 0; i < DIRECTIONS; ++i)
                                    {
                                        float const turn = float((i + 1) / 2) * 2.0f * float(M_PI) /
                                                           float(DIRECTIONS) * (i % 2 ? 1.0f : -1.0f);
                                        float x = target->GetPositionX() + std::cos(back + turn) * distance;
                                        float y = target->GetPositionY() + std::sin(back + turn) * distance;
                                        float z = target->GetPositionZ();
                                        if (!map->CheckCollisionAndGetValidCoords(target, target->GetPositionX(),
                                                                                  target->GetPositionY(),
                                                                                  target->GetPositionZ(), x, y, z))
                                        {
                                            NoteSpotReject("at target", "no ground");
                                            continue;
                                        }
                                        Position const at(x, y, z);
                                        if (!meleeAtTarget(at, "at target"))
                                            continue;
                                        float const walk = bot->GetExactDist(at);
                                        if (best < 0.0f || walk < best)
                                        {
                                            best = walk;
                                            chosen = at;
                                            from = "a melee spot at the target";
                                            placed = true;
                                        }
                                    }
                                }
                            }
                            if (placed)
                            {
                                lead.combatSpots[guid] = {target->GetGUID(), chosen};
                                lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;
                                // [lead156] one decision per member, target and point (b175: 5 in a row)
                                auto const before = lead.tankGroundSpots.find(guid);
                                bool const same = before != lead.tankGroundSpots.end() &&
                                                  before->second.GetExactDist(chosen) <= 1.0f &&
                                                  lead.tankGroundFor[guid] == target->GetGUID();
                                lead.tankGroundSpots[guid] = chosen;
                                lead.tankGroundFor[guid] = target->GetGUID();
                                if (!same)
                                    Decisions().Write(run, "leader", "combat_spot_melee_at_target", guid,
                                                      bot->GetName(), true,
                                                      JsonObject().Str("member", bot->GetName())
                                                          .Str("why", breaks).Str("target", target->GetName())
                                                          .Str("from", from)
                                                          .Real("walk", bot->GetExactDist(chosen))
                                                          .Str("rejects", rejects.Summary()));
                            }
                            else if (bot != tank)
                            {
                                thread_local std::set<std::pair<ObjectGuid, ObjectGuid>> noneLogged;
                                if (noneLogged.size() > 4096)
                                    noneLogged.clear();
                                if (noneLogged.insert({guid, target->GetGUID()}).second)
                                {
                                    // [lead463] user 2026-11-02: a member with no spot is a leader bug - loud
                                    LOG_ERROR("playerbots", "[lead463] combat_spot_none: {} has no spot on {} ({}): {}",
                                              bot->GetName(), target->GetName(), breaks, rejects.Summary());
                                    // [lead734] the idle mob the melee places met, where it and the fight stand
                                    Decisions().Write(
                                        run, "leader", "combat_spot_none", guid, bot->GetName(), false,
                                        JsonObject()
                                            .Str("member", bot->GetName())
                                            .Str("why", breaks)
                                            .Str("target", target->GetName())
                                            .Bool("reaches", reaches)
                                            .Str("rejects", rejects.Summary())
                                            .Str("idle", idleBlocker ? idleBlocker->GetName() : "-")
                                            .Num("idleLow",
                                                 idleBlocker ? int64(idleBlocker->GetGUID().GetCounter()) : 0)
                                            .Real("idleX", idleBlocker ? idleBlocker->GetPositionX() : 0.0f)
                                            .Real("idleY", idleBlocker ? idleBlocker->GetPositionY() : 0.0f)
                                            .Real("idleZ", idleBlocker ? idleBlocker->GetPositionZ() : 0.0f)
                                            .Real("idleReach", idleBlocker ? idleBlocker->GetAggroRange(bot) : -1.0f)
                                            .Real("targetX", target->GetPositionX())
                                            .Real("targetY", target->GetPositionY())
                                            .Real("tankX", tank->GetPositionX())
                                            .Real("tankY", tank->GetPositionY())
                                            .Real("tankGap", idleBlocker ? tank->GetExactDist(idleBlocker) : -1.0f)
                                            .Real("targetGap",
                                                  idleBlocker ? target->GetExactDist(idleBlocker) : -1.0f));
                                }
                                // [lead734] the idle mob that keeps a melee member off its target is the fight's
                                // question like a refused walk's (lead76): its pack joins the fight when the group
                                // takes it, else the fight moves out of its reach (the tank's target and the melee
                                // places round it included)
                                if (melee && idleBlocker)
                                    judgeBlockingPack(bot, idleBlocker);
                            }
                        }
                        found = lead.combatSpots.find(guid);
                    }
                }
                else
                {
                    bool const valid = found != lead.combatSpots.end() && found->second.first == target->GetGUID() &&
                                       LeaderCombatSpotValid(bot, botAI, target, found->second.second, melee, zones,
                                                             keeps, sight);
                    if (!valid)
                    {
                        Position spot;
                        if (slow([&] { return LeaderCombatSpot(bot, botAI, target, melee, zones, keeps, spot); }))
                            lead.combatSpots[guid] = {target->GetGUID(), spot};
                        else
                            lead.combatSpots.erase(guid);
                        found = lead.combatSpots.find(guid);
                    }
                }
                // [lead380] a ranged member / healer still walking to its spot that casts from where it stands now (the
                // mob came within its reach, the tank stopped) stops there: the walk has no reason left
                // [lead397] b201 rfc-h: 13 command_repeat "combat spot" in 8 runs (5B 787.2-788.4 s: the shadow priest
                // re-spotted "inside a fight mob's melee reach" 6 yd off, stopped 0.3 s later "it casts from where it
                // walks" 3.9 yd short, re-spotted 0.9 s later "inside a fight mob's melee reach" back to the first
                // spot). The walk was decided with the running Searing Blade Cultist where it ran to, the stop with it
                // where it stood that moment - two facts for one mob. The stop reads the walk's facts: a running mob
                // where it runs to. 5E 441.1 / 441.7 s, 5A 1213.8 / 1214.1 s: the stop fired twice on one walk - the
                // hunter ran on 2-2.5 yd past the point it was stopped at, and the second stop put it back near the
                // spot the first one had left. A walk is stopped once; the stop's point is its spot
                // [lead505] b210 rfc-h: the command_repeat double decisions (5A 258.6 / 762.8, 5C 777.8, 5H 269.8 /
                // 666.1 s) were this flip the other way: the stop "it casts from where it walks" read a running mob
                // where it runs to, the held spot under its feet is judged by the mobs where they stand (onHeld), and
                // 0.3 s later it broke "inside a fight mob's melee reach" (the Shaman / Trogg passing it on its run to
                // the tank) - command_repeat "combat spot". The stop holds only where both facts let it cast: the mobs
                // where they run to and where they stand now
                // [lead533] b212 rfc-h: 10 command_repeat "combat spot" in 4 runs (5B 454.2-456.3 s Rtpriaa /
                // Rtshaac, 5H 650.1-652.2 s Rthunad, ...) - still this flip: the stop 2-3 yd short ("it casts from
                // where it walks") came while the target ran in (5B: Earthborer 15 yd from the tank at 453.9 s, 1 yd at
                // 454.8 s; 5H: the Enforcer still walking onto the casters, "inside a fight mob's melee reach" at 650.1
                // and 651.9 s), and 1 s later the stop point failed (no sight, inside the minimum range) and the walk
                // went back near the spot it had left. The spot was chosen for the fight where it will stand; where the
                // member walks is judged only once the fight stands there: the target and the tank still (5B: the
                // tank walked the fight to its anchor ahead, 12.8 yd), the target on the tank in its reach
                bool const fightStands = !target->isMoving() && !tank->isMoving() && target->GetVictim() == tank &&
                                         tank->IsWithinMeleeRange(target);
                auto const stopped = lead.combatSpotStoppedAt.find(guid);
                bool const stoppedHere = found != lead.combatSpots.end() && stopped != lead.combatSpotStoppedAt.end() &&
                                         stopped->second.GetExactDist(found->second.second) <= 0.1f;
                if (!melee && fightStands && !stoppedHere && found != lead.combatSpots.end() &&
                    found->second.first == target->GetGUID() &&
                    bot->GetExactDist2d(found->second.second.GetPositionX(), found->second.second.GetPositionY()) >
                        2.0f &&
                    standsFit(false) && standsFit(true))  // [lead505]
                {
                    Decisions().Write(run, "leader", "combat_spot_here", guid, bot->GetName(), true,
                                      JsonObject().Str("member", bot->GetName())
                                          .Str("why", "it casts from where it walks")
                                          .Str("target", target->GetName())
                                          .Real("walkLeft", bot->GetExactDist(found->second.second)));
                    found->second.second = bot->GetPosition();
                    lead.combatSpotStoppedAt[guid] = found->second.second;
                    lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;
                }
                // [lead20] user: «когда танк захватил уже всех с этого радиуса, хил может зайти» - a walk to the
                // combat spot only once the tank holds every fight mob near the spot and the walk (else it stays)
                if (found != lead.combatSpots.end() &&
                    bot->GetExactDist2d(found->second.second.GetPositionX(), found->second.second.GetPositionY()) >
                        (melee ? 1.0f : 2.0f))
                {
                    // [lead25] user: «танк пулит, моб бежит к нему… через секунду все ДД начинают дамажить»: melee
                    // walk to the meeting at the tag (the pulled mob already comes); ranged / healer wait for the hold
                    if (!melee && !tankHolds(bot->GetPosition(), found->second.second))
                    {
                        if (!lead.holdWaitMs.count(guid))
                            lead.holdWaitMs[guid] = now;
                        found = lead.combatSpots.end();
                        moved = true;  // it stays where it is meanwhile
                    }
                    else if (auto const waited = lead.holdWaitMs.find(guid); waited != lead.holdWaitMs.end())
                    {
                        Decisions().Write(run, "leader", "respot_after_tank_hold", guid, bot->GetName(),
                                                      true,
                                                      JsonObject().Str("member", bot->GetName())
                                                          .Real("waitSec", getMSTimeDiff(waited->second, now) /
                                                                               1000.0f));
                        lead.holdWaitMs.erase(waited);
                    }
                }
                if (found != lead.combatSpots.end())
                {
                    Position const spot = found->second.second;
                    if (checkedMove(guid, Leader::MoveEvent::StandAt, spot, melee ? 1.0f : 2.0f,
                                                melee ? "combat spot: behind the target" : "combat spot"))
                        logEvent(bot, MoveEventName(Leader::MoveEvent::StandAt),
                                 JsonObject().Str("why", melee ? "combat spot: behind the target" : "combat spot")
                                     .Str("target", target->GetName())
                                     .Real("distance", target->GetExactDist(spot))
                                     .Real("x", spot.GetPositionX()).Real("y", spot.GetPositionY())
                                     .Real("z", spot.GetPositionZ()));
                    // [lead241] b189 rfc-h 5A Taragaman 625-687 s: the frost mage's combat spot was rejected ("in the
                    // reach of Searing Blade Enforcer") and kept - no move, it stood 34-36 yd off and cast nothing for
                    // 62 s. A rejected spot from which it cannot attack where it stands is a spot left: the next tick
                    // searches another one (lead239 keeps it out)
                    else if (auto const verdict = lead.spotChecked.find(guid);
                             verdict != lead.spotChecked.end() && !verdict->second.reject.empty() &&
                             verdict->second.spot.GetExactDist(spot) <= 1.0f &&
                             !(melee ? bot->IsWithinMeleeRange(target) : MemberCastsHere(bot, target)))
                    {
                        LeaveCombatSpot(lead.combatSpotLeft[guid], {target->GetGUID(), found->second.second});
                        lead.combatSpots.erase(found);
                        found = lead.combatSpots.end();
                    }
                    if (bot == healer)
                        healerAt = spot;
                    moved = true;
                }
            }

            // [lead151] diagnostic: a damage dealer in the fight left without a move this tick (once per member and
            // fight target): why the combat-spot rule did not place it
            if (combat && !moved && bot != healer && !PlayerbotAI::IsHeal(bot) && target)
            {
                thread_local std::set<std::pair<ObjectGuid, ObjectGuid>> noSpotLogged;
                if (noSpotLogged.size() > 4096)
                    noSpotLogged.clear();
                if (noSpotLogged.insert({guid, target->GetGUID()}).second)
                    Decisions().Write(run, "leader", "fight_no_spot", guid, bot->GetName(), false,
                                      JsonObject().Str("member", bot->GetName()).Str("target", target->GetName())
                                          .Bool("combatSpots", Leader::LEADER_COMBAT_SPOTS)
                                          .Real("distance", bot->GetExactDist(target))
                                          .Bool("spotsHeld", spotsHeld));
            }
            if (!moved && spotsHeld)
                if (auto const spot = lead.spots.find(guid); spot != lead.spots.end())
                {
                    if (checkedMove(guid, Leader::MoveEvent::StandAt, spot->second,
                                                Leader::LEADER_SPOT_TOLERANCE, "its spot for the pull"))
                        logEvent(bot, MoveEventName(Leader::MoveEvent::StandAt),
                                 JsonObject().Str("why", "its spot for the pull")
                                     .Real("x", spot->second.GetPositionX()).Real("y", spot->second.GetPositionY())
                                     .Real("z", spot->second.GetPositionZ()));
                    // [lead930] b226 rfc-h 5H 259.2-491 s: the guard refused Rthunad's walk to its pull spot, the
                    // hunter stayed 53 yd off it and the spot gate waited for that spot to the stall (lead860: no
                    // refusal leaves the run without a next action). A pull spot the guard refuses is no spot: it is
                    // dropped and kept out, the next LeaderAssignSpots gives the member another one (or its place /
                    // none, lead20 - the gate never waits on a member without a spot)
                    else if (auto const verdict = lead.spotChecked.find(guid);
                             verdict != lead.spotChecked.end() && !verdict->second.reject.empty() &&
                             verdict->second.spot.GetExactDist(spot->second) <= 1.0f &&
                             bot->GetExactDist2d(spot->second.GetPositionX(), spot->second.GetPositionY()) >
                                 Leader::LEADER_SPOT_TOLERANCE)
                    {
                        LOG_INFO("playerbots", "[lead930] {}: its pull spot ({:.1f},{:.1f},{:.1f}) {:.0f} yd off is "
                                 "refused ({}): a new spot",
                                 bot->GetName(), spot->second.GetPositionX(), spot->second.GetPositionY(),
                                 spot->second.GetPositionZ(), bot->GetExactDist(spot->second),
                                 verdict->second.reject);
                        Decisions().Write(run, "leader", "spot_refused_respot", guid, bot->GetName(), true,
                                          JsonObject().Str("member", bot->GetName())
                                              .Str("reject", verdict->second.reject)
                                              .Real("x", spot->second.GetPositionX())
                                              .Real("y", spot->second.GetPositionY())
                                              .Real("z", spot->second.GetPositionZ())
                                              .Real("gap", bot->GetExactDist(spot->second)));
                        lead.spotRefused[guid].push_back(spot->second);
                        lead.spotBest.erase(guid);
                        lead.spots.erase(spot);
                    }
                    moved = true;
                }
            // the member's own follow is off (LEADER_NO_OWN_FOLLOW): the leader walks it along with the tank; a
            // member the harness holds ("stay", a regroup) or one that sits is not moved
            // [lead16] LEADER_HOLD_EVENTS: the group is held (a patrol wait, the harness's "stay"): Hold, no walk
            if (!moved && !combat && Leader::LEADER_HOLD_EVENTS && Leader::ExecutorMember(bot) &&
                (!lead.waitPatrol.IsEmpty() ||  // [lead33] no Hold <-> Follow flip on the step-back walk
                 botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT)))
            {
                if (checkedMove(guid, Leader::MoveEvent::Hold, bot->GetPosition(), 0.0f, "the group holds"))
                    logEvent(bot, "Hold", JsonObject().Str("why", lead.waitPatrol.IsEmpty() ? "stay (regroup)"
                                                                                          : "patrol wait"));
                moved = true;
            }
            // [lead148] user: no wait spots on a pull walk. b169 20261101-141316: "the pull walks" p90 82 yd, max
            // 105-135 yd from the tank (lead126's stop before any reach on the tank's trail, an idle mob passed 80-100
            // yd short of the pull); fights p90 47-70 yd. The members follow the tank at the follow spread behind it
            // while it walks to its opener's range (the follow below, no engage walk-along spot); the fight's start
            // ends the follow and the fight spots take them (lead118 / lead119)
            if (!moved && !combat && Leader::LEADER_NO_OWN_FOLLOW && Leader::ExecutorMember(bot) &&
                !bot->IsSitState() && !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
            {
                bool const melee = !PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsHeal(bot);
                bool const following = lead.following.count(guid) > 0;
                float const gap = bot->GetExactDist(tank);
                // [lead27] user: «он выдал - и меняет приказ только если что-то поменялось»: one Follow command (the
                // member keeps its place behind the tank by itself); an engage walk-along to range stays a point
                Leader::PullOrder engage;
                Unit* engaged = nullptr;
                // [lead730] a ranged damage dealer takes its range spot of the engage while the tank walks too (its
                // spot is outside every unpulled reach, judged below); the healer keeps lead148's walk with the tank
                bool const healing = PlayerbotAI::IsHeal(bot);
                if ((!pullWalk || !healing) && lead.commit && lead.commitIssued && lead.commit->kind == "engage" &&
                    !melee && Leader::Orders::GetOpen(group.LeaderGuid(), engage))
                    engaged = ObjectAccessor::GetUnit(*tank, lead.commitMob);
                // [lead81] b124 rfc-h 147.6-166 s: the group walked 120 yd to the engage's range / meeting point while
                // the tank never left (-90,-40) - the tank leads the walk: a walk-along point nearer the engaged mob
                // than the tank stands is ahead of the tank, the member follows the tank until the tank is past it
                Unit* const engagedMob = lead.commit && lead.commitIssued && lead.commit->kind == "engage"
                                             ? ObjectAccessor::GetUnit(*tank, lead.commitMob)
                                             : nullptr;
                auto const aheadOfTank = [&](Position const& at)
                { return engagedMob && engagedMob->GetExactDist(at) < engagedMob->GetExactDist(tank); };
                bool aheadHolds = false;
                // [lead64] melee at an engage: one command when the tank goes - to the meeting point
                // [lead730] user 2026-11-03: every pull (engage, pull back, LoS, range pull), also while the tank
                // walks to it (lead148's Follow on the pull walk is gone for melee): the melee member walks to the
                // closest safe point to the pull's stop point (meleePullWait: the last point of its walk there
                // outside every unpulled mob's reach, the pull's own mobs included until the tank has them) and
                // waits there for the tank's opener. Its walk judged as every walk (checkedMove); refused, it walks
                // along with the tank as before
                bool meleeToMeeting = false;
                if (melee && lead.commit && lead.commitIssued && !lead.commitMob.IsEmpty() &&
                    Leader::Orders::GetOpen(group.LeaderGuid(), engage))
                {
                    std::optional<Position> const waitFound = meleePullWait(bot);
                    Position const waitAt = waitFound.value_or(bot->GetPosition());
                    std::string const waitWhy = "to the tank's stop point: waits out of the unpulled mobs' reach";
                    auto const refusedHere = [&]
                    {
                        auto const verdict = lead.spotChecked.find(guid);
                        return verdict != lead.spotChecked.end() && !verdict->second.reject.empty() &&
                               verdict->second.spot.GetExactDist(waitAt) <= 1.0f;
                    };
                    if (waitFound && !refusedHere() &&
                        checkedMove(guid, Leader::MoveEvent::MoveTo, waitAt, 3.0f, waitWhy))
                        logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                 JsonObject().Str("why", waitWhy).Real("x", waitAt.GetPositionX())
                                     .Real("y", waitAt.GetPositionY()).Real("z", waitAt.GetPositionZ())
                                     .Real("stopGap", waitAt.GetExactDist(lead.meetPoint))
                                     .Str("kind", lead.commit->kind));
                    if (waitFound && !refusedHere())
                    {
                        meleeToMeeting = true;
                        moved = true;
                    }
                }
                // [lead29] the range spot is validated before it is issued (ground, lava / slime / a drop-off, an
                // unpulled mob's reach, the member's own walk guards): the nearest valid one between its range and
                // the tank; none (or the member refused it): the member follows the tank - never "stand" while it
                // walks off
                Position rangeSpot;
                bool rangeValid = false;
                // [lead38] the range spot is kept for the pull while the mob stays put (one command, no re-issue)
                if (engaged)
                {
                    auto const kept = lead.engageRangeSpots.find(guid);
                    // [lead68] b113: the spot moved with the tank (re-issued every ~0.6 s) - it is computed once per
                    // order (the leader's plan) and kept until a new order
                    if (kept != lead.engageRangeSpots.end() && kept->second.first == engaged->GetGUID() &&
                        lead.engageRangeOrder[guid] == engage.id)
                    {
                        rangeSpot = kept->second.second;
                        rangeValid = true;
                    }
                }
                if (engaged && !rangeValid && !lead.engageRefused.count(guid))
                {
                    float const range = PlayerbotAI::IsHeal(bot) ? Leader::LEADER_HEALER_TANK_YD - 4.0f
                                                                 : LeaderSpellReach(bot) - 4.0f;
                    // [lead68] from the leader's meeting point back toward the member (the group's side), not from
                    // the walking tank
                    Position const meet = lead.meetPoint;
                    float const dx = bot->GetPositionX() - meet.GetPositionX();
                    float const dy = bot->GetPositionY() - meet.GetPositionY();
                    float const length = std::max(0.1f, std::hypot(dx, dy));
                    LeaderMoveOrderAction* mover = dynamic_cast<LeaderMoveOrderAction*>(
                        botAI->GetAiObjectContext()->GetAction("leader move order"));
                    for (float at = std::min(range, length); at <= length && !rangeValid; at += 3.0f)
                    {
                        float x = meet.GetPositionX() + dx / length * at;
                        float y = meet.GetPositionY() + dy / length * at;
                        float z = meet.GetPositionZ();
                        if (!map->CheckCollisionAndGetValidCoords(engaged, meet.GetPositionX(), meet.GetPositionY(),
                                                                  meet.GetPositionZ(), x, y, z) ||
                            NearHazard(map, bot->GetPhaseMask(), x, y, z, LEADER_TERRAIN_EDGE))
                            continue;
                        Position const spot(x, y, z);
                        if (LeaderAggroAt(bot, spot) || (mover && !mover->WalkRefusal(spot).empty()))
                            continue;
                        rangeSpot = spot;
                        rangeValid = true;
                    }
                }
                if (meleeToMeeting)
                {
                }
                else if (rangeValid)
                {
                    lead.engageRangeSpots[guid] = {engaged->GetGUID(), rangeSpot};
                    lead.engageRangeOrder[guid] = engage.id;
                    // [lead81] the healer follows the tank until it is past its spot; [lead730] a ranged damage dealer
                    // waits at its spot (out of every unpulled reach) for the tank's opener
                    // On the tank's walk a spot whose walk the leader refused is no wait: the member walks along with
                    // the tank (the Follow below), the tank does not hold for it
                    auto const refusedOnWalk = [&]
                    {
                        auto const verdict = lead.spotChecked.find(guid);
                        return pullWalk && verdict != lead.spotChecked.end() && !verdict->second.reject.empty() &&
                               verdict->second.spot.GetExactDist(rangeSpot) <= 1.0f;
                    };
                    if (healing && aheadOfTank(rangeSpot))
                        aheadHolds = true;
                    else if (!refusedOnWalk())
                    {
                        if (checkedMove(guid, Leader::MoveEvent::MoveTo, rangeSpot, LEADER_FOLLOW_TOLERANCE,
                                        "along with the tank: to range of the engage"))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::MoveTo),
                                     JsonObject().Str("why", "along with the tank: to range of the engage"));
                        moved = !refusedOnWalk();
                    }
                }
                // [lead372] the member reported it cannot reach its follow place safely: while the facts of that
                // report stand (the member, the tank and the mob its walk would wake where they were) it stays where it
                // is and the group holds for it (group_blocked: the tank walks on only where the group follows)
                Leader::MoveRefusal followRefusal;
                if (!moved && !meleeToMeeting && Leader::Events::MoveRefused(guid, followRefusal) &&
                    followRefusal.move == Leader::MoveEvent::Follow)
                {
                    Creature* woken = followRefusal.mob.IsEmpty() ? nullptr : map->GetCreature(followRefusal.mob);
                    // [lead369] b200 5G 632-862 s: Rtdekad 1.5 yd from the tank shifted over 1 yd, the report was
                    // dropped, the Follow went out again and was refused again - 56 Follows in 28 s. The report's
                    // facts stand while nobody moved off them by the leader's own "moved" measure
                    bool const holds =
                        bot->GetExactDist(followRefusal.from) <= Leader::LEADER_COMMIT_MOVED_YD &&
                        tank->GetExactDist(followRefusal.targetAt) <= Leader::LEADER_COMMIT_MOVED_YD &&
                        (followRefusal.mob.IsEmpty() ||
                         (woken && woken->IsAlive() && !woken->IsInCombat() &&
                          woken->GetExactDist(followRefusal.mobAt) <= Leader::LEADER_COMMIT_MOVED_YD));
                    if (!holds)
                        Leader::Events::ClearMoveRefused(guid);
                    else
                    {
                        std::string const stay =
                            "along with the tank: stays (the member's walk refused: " + followRefusal.reason + ")";
                        if (Leader::Events::SetMove(guid, Leader::MoveEvent::StandAt, bot->GetPosition(), 2.0f, stay))
                            logEvent(bot, MoveEventName(Leader::MoveEvent::StandAt), JsonObject().Str("why", stay));
                        if (!lead.groupBlockedBy.count(guid))
                            Decisions().Write(run, "leader", "group_blocked", guid, bot->GetName(), true,
                                              JsonObject()
                                                  .Str("member", bot->GetName())
                                                  .Str("why", followRefusal.reason)
                                                  .Str("mob", woken ? woken->GetName() : "-")
                                                  .Str("walk", followRefusal.why));
                        lead.groupBlockedBy[guid] = followRefusal.mob;
                        moved = true;
                    }
                }
                // [lead44] the member buffs (its rebuff window): the buff is the command while the tank stands - no
                // Follow that would replace it, only a walk when the tank walks on or the member is far / out of sight
                // [lead47] b96: Rtshaab got fresh follow walks every few seconds under the leader's Rest and never
                // sat - a resting member is not walked along while the tank stands
                if (!moved && !meleeToMeeting &&
                    (aheadHolds ||
                     (!(restCommanded && !tank->isMoving()) &&
                      (engaged || pullWalk || (following && !botAI->forceRebuff.IsPending()) || tank->isMoving() ||
                       gap > LEADER_FOLLOW_START_YD || !bot->IsWithinLOSInMap(tank)))))
                {
                    lead.following.insert(guid);
                    if (followTank(bot, uint8(index % 4), "along with the tank"))
                        logEvent(bot, "Follow", JsonObject().Str("why", "along with the tank").Real("gap", gap));
                    moved = true;
                }
            }
            // [lead29] a led member always has a command: the group walks and this member got none - no_command (a
            // leader bug), the fallback is Follow
            if (!moved && !combat && tank->isMoving() && Leader::ExecutorMember(bot) && !bot->IsSitState() &&
                !botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
            {
                uint32& last = lead.noCommandLogMs[guid];
                if (!last || getMSTimeDiff(last, now) >= 5 * IN_MILLISECONDS)
                {
                    last = now;
                    Decisions().Write(run, "leader", "no_command", guid, bot->GetName(), true,
                                                  JsonObject().Str("member", bot->GetName())
                                                      .Str("why", "the group walks and no command fit: Follow")
                                                      .Real("gap", bot->GetExactDist(tank)));
                }
                if (followTank(bot, uint8(index % 4), "along with the tank"))
                    logEvent(bot, "Follow", JsonObject().Str("why", "fallback: no other command fit"));
                moved = true;
            }
            if (!moved || combat)
                lead.following.erase(guid);

            // 3. the fight: the healer heals the group, every damage dealer attacks the leader's target
            if (combat && Leader::LEADER_ATTACK_EVENTS)
            {
                // [lead53] [lead243] [lead246] the threat projection both the damage dealers' headroom and the
                // healer's read (lead390: the healer's threat is the game's too)
                auto const seenOn = [&](Unit* mob, Player* who) -> Leader::DungeonLeaderState::ThreatSeen&
                {
                    auto& seen = lead.threatSeen[mob->GetGUID()][who->GetGUID()];
                    if (float const threat = mob->GetThreatMgr().GetThreat(who); threat > 0.0f)
                    {
                        // [lead561] b214 rfc-h 5D 85-87.6 s: the bear's Growl copied the unholy DK's 9000 threat on
                        // a Ragefire Trogg and the bear's rate there (its threat over its seconds on the mob) carried
                        // it: tankNext 15598 against 12274 + its real 491 per 0.3 s, the DK got headroom again and
                        // took the Trogg back 2.5 s later. A rise while the tank's taunt aura comes onto the mob is
                        // the taunt's copy: the tank's rate there counts from it
                        bool const taunt =
                            who == tank && mob->HasAuraTypeWithCaster(SPELL_AURA_MOD_TAUNT, who->GetGUID());
                        if (!seen.firstMs)
                            seen.firstMs = now;
                        if (threat > seen.threat)
                        {
                            seen.riseMs = now;  // [lead432]
                            if (taunt && !seen.taunted)
                            {
                                seen.base = threat;
                                seen.firstMs = now;
                            }
                        }
                        seen.taunted = taunt;
                        seen.lastMs = now;
                        seen.threat = threat;
                    }
                    return seen;
                };
                float freshRate = 0.0f;
                if (auto const measured = lead.openTps.find(guid);
                    measured != lead.openTps.end() && measured->second.second)
                    freshRate = measured->second.first / float(measured->second.second);
                for (ObjectGuid const& other : killOrder)
                    if (Unit* fought = ObjectAccessor::GetUnit(*tank, other); fought && fought->IsAlive())
                        if (auto const& seen = seenOn(fought, bot); seen.threat > 0.0f)
                            freshRate = std::max(freshRate, ThreatSeenRate(seen));
                // [lead246] b191 rfc-h: the warrior tanks' threat rate 0-2 s into a pull was 740 / 987 per s
                // against 1691 / 1652 at 2-5 s (rage), so the tank's next GCD read from its first hit stopped
                // the damage dealers early (5B 129, 5F 118 stops, 31-38 of 46 per member in the first 2 s). Both
                // sides read a rate measured over less than a GCD the same way: at least the run's opening rate
                float tankOpen = 0.0f;
                if (auto const measured = lead.openTps.find(tank->GetGUID());
                    measured != lead.openTps.end() && measured->second.second)
                    tankOpen = measured->second.first / float(measured->second.second);
                // [lead683] b218 rfc-h (8 runs): 149 of the 590 "threat: no mob with headroom" Stops read the tank's
                // threat flat at its pull (5A 51 / 116 / 148.5 / 173.3 / 303.6 s: tank 41.2 -> tankNext 41.2 on the
                // mob it had just pulled, every damage dealer stopped) - the tank's opening rate projected only onto
                // its victim, and the tank opening on the mob it pulled (its selection, its opener landed, the mob
                // running in) has no victim yet. The mob the tank attacks or has selected is the one its opening
                // rate builds on
                auto const tankOpensOn = [&](Unit* mob)
                { return tank->GetVictim() == mob || (mob && tank->GetTarget() == mob->GetGUID()); };
                auto const rateOf = [](Leader::DungeonLeaderState::ThreatSeen const& seen, float opening)
                {
                    float const onMob = seen.threat > 0.0f ? ThreatSeenRate(seen) : 0.0f;
                    bool const measuredHere =
                        seen.threat > 0.0f &&
                        getMSTimeDiff(seen.firstMs, seen.lastMs) / 1000.0f >= HEADROOM_GCD_SEC;
                    return measuredHere ? onMob : std::max(onMob, opening);
                };
                struct Projection
                {
                    float mine = 0.0f;
                    float mineNext = 0.0f;
                    float tank = 0.0f;
                    float tankNext = 0.0f;
                    float share = 1.0f;
                    float redirected = 0.0f;  // [lead412] of the member's next threat, onto the tank
                };
                auto const project = [&](Unit* mob)
                {
                    Projection p;
                    auto const& mine = seenOn(mob, bot);
                    p.mine = mine.threat;
                    p.mineNext = mine.threat + rateOf(mine, freshRate) * HEADROOM_GCD_SEC;
                    // [lead900] user 2026-11-03: before its first hit on the mob, its first cast's threat by the
                    // spell threat data (Icy Touch x7) counts when it is more than its rate over the GCD (b187 lead229:
                    // the unholy DK's opening Icy Touch from range took the bear's mobs)
                    if (mine.threat <= 0.0f)
                    {
                        uint32 spell = 0;
                        if (float const first = FirstCastThreat(bot, mob, spell); first > p.mineNext - p.mine)
                        {
                            thread_local std::set<std::pair<uint64, uint32>> firstLogged;
                            if (firstLogged.insert({bot->GetGUID().GetRawValue(), spell}).second)
                                LOG_INFO("playerbots", "[lead900] {}: its first {} on {} counts {:.0f} threat before "
                                         "its hit (rate over the GCD {:.0f})",
                                         bot->GetName(), spell, mob->GetName(), first, p.mineNext - p.mine);
                            p.mineNext = p.mine + first;
                        }
                    }
                    // [lead901] user 2026-11-03: its running damage / heals over time keep adding threat after a Stop
                    // (no Stop cancels them): what they still put on the mob counts on top of its next GCD (the part
                    // within the GCD is already in its rate there), against the tank's threat over the same time
                    float horizon = HEADROOM_GCD_SEC;
                    if (PeriodicLeft const periodic = PeriodicThreatLeft(bot, mob); periodic.threat > 0.0f)
                    {
                        float const cast = p.mineNext - p.mine;
                        p.mineNext = p.mine + std::max(cast, periodic.within) + (periodic.threat - periodic.within);
                        horizon = std::max(horizon, periodic.sec);
                    }
                    // [lead412] user 2026-11-02: the threat a running redirect (Misdirection / Tricks of the Trade,
                    // registered on the member's ThreatManager until it is spent) moves onto the tank is the tank's
                    if (auto const redirect = lead.redirectTo.find(guid);
                        redirect != lead.redirectTo.end() && redirect->second.first == tank->GetGUID() &&
                        bot->GetThreatMgr().HasRedirects())
                    {
                        float const moved = (p.mineNext - p.mine) * redirect->second.second;
                        p.mineNext -= moved;
                        p.redirected = moved;
                    }
                    auto const& held = seenOn(mob, tank);
                    p.tank = held.threat;
                    // [lead391] b201 rfc-h 5D 579.3 s: the unholy DK got Attack with headroom on a Searing Blade
                    // Cultist the bear held at 140 threat (DK 1854 at 579.6, the mob on the DK): the bear's next GCD
                    // was read at its opening single-target rate on a mob it did not attack. The tank's opening rate
                    // projects onto the mob it attacks; on another its threat grows only by what it measured there
                    p.tankNext = held.threat > 0.0f
                                     ? held.threat + rateOf(held, tankOpensOn(mob) ? tankOpen : 0.0f) * horizon
                                     : 0.0f;
                    p.tankNext += p.redirected;
                    p.share = mob->GetVictim() != tank       ? 1.0f
                              : mob->IsWithinMeleeRange(bot) ? THREAT_OVERTAKE_MELEE
                                                             : THREAT_OVERTAKE_RANGED;
                    if (horizon > HEADROOM_GCD_SEC && p.mineNext >= p.share * p.tankNext)
                    {
                        thread_local std::set<std::pair<uint64, uint64>> periodicLogged;
                        if (periodicLogged.insert({bot->GetGUID().GetRawValue(), mob->GetGUID().GetRawValue()}).second)
                            LOG_INFO("playerbots", "[lead901] {}: its running periodic threat on {} over {:.1f} s "
                                     "closes the headroom ({:.0f} vs tank {:.0f})",
                                     bot->GetName(), mob->GetName(), horizon, p.mineNext, p.tankNext);
                    }
                    return p;
                };
                auto const headroomOn = [&](Unit* mob)
                {
                    Projection const p = project(mob);
                    return p.mineNext < p.share * p.tankNext;
                };
                // [lead985] user 2026-11-03 (aggro-loss-if-survives): headroom is no absolute stop - a damage dealer
                // keeps its damage on a mob it would take off the tank when it survives that mob's hits until the
                // tank has it back and the healer can heal it. The mob's damage on THIS member (AttackerDps: its
                // melee after the member's armor - plate takes less than cloth - its avoidance, plus the spell it
                // casts), with the mobs already on the member, over the take-back time: the tank's ready taunt in
                // its range and sight (its global cooldown + the leader's reaction tick; one taunt for one loose
                // mob: none while a fight mob is already off the tank), else its threat catch-up to the game's
                // overtake share at its rate there (the member stops once the mob turns). Against the member's
                // health + absorbs now, and a healer that reaches it (heal reach, sight) above its own mana line.
                // Never before the tank's first threat on the mob (lead875 stays). Any check failing: the headroom
                // Stop stands
                auto const survivesOn = [&](Unit* mob) -> bool
                {
                    if (!mob || !mob->IsAlive() || bot == tank || PlayerbotAI::IsHeal(bot) ||
                        PlayerbotAI::IsTank(bot))
                        return false;
                    Projection const p = project(mob);
                    if (p.tank <= 0.0f || mob->GetThreatMgr().GetThreat(tank) <= 0.0f)
                        return false;
                    bool looseNow = false;
                    for (ObjectGuid const& other : killOrder)
                        if (Unit* fought = ObjectAccessor::GetUnit(*tank, other);
                            fought && fought->IsAlive() && fought->GetVictim() && fought->GetVictim() != tank &&
                            fought->GetVictim()->IsPlayer())
                            looseNow = true;
                    float takeBack = -1.0f;
                    std::string how;
                    if (!looseNow)
                        for (uint32 taunt : TANK_TAUNTS)
                            if (tank->HasSpell(taunt) && !tank->HasSpellCooldown(taunt))
                                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(taunt);
                                    info && tank->IsWithinCombatRange(mob, info->GetMaxRange(false)) &&
                                    tank->IsWithinLOSInMap(mob))
                                {
                                    takeBack = HEADROOM_GCD_SEC + LEADER_FIGHT_TICK_MS / 1000.0f;
                                    how = Acore::StringFormat("{} ready", info->SpellName[LOCALE_enUS]);
                                    break;
                                }
                    if (takeBack < 0.0f)
                    {
                        float const tankRate =
                            rateOf(seenOn(mob, tank), tankOpensOn(mob) ? tankOpen : 0.0f);
                        if (tankRate <= 0.0f)
                            return false;
                        takeBack = std::max(HEADROOM_GCD_SEC,
                                            (THREAT_OVERTAKE_MELEE * p.mineNext - p.tankNext) / tankRate);
                        how = Acore::StringFormat("threat catch-up at {:.0f} tps", tankRate);
                    }
                    float spellPart = 0.0f;
                    float incoming = AttackerDps(mob, bot, spellPart);
                    for (Unit* attacker : bot->getAttackers())
                        if (attacker && attacker != mob && attacker->IsAlive() && attacker->GetVictim() == bot)
                        {
                            float part = 0.0f;
                            incoming += AttackerDps(attacker, bot, part);
                        }
                    float const damage = incoming * takeBack;
                    float const health =
                        float(bot->GetHealth()) + float(bot->GetTotalAuraModifier(SPELL_AURA_SCHOOL_ABSORB));
                    bool const healed = healer && healer != bot && healer->IsAlive() &&
                                        healer->IsWithinCombatRange(bot, Leader::LEADER_HEAL_REACH_YD) &&
                                        healer->IsWithinLOSInMap(bot) &&
                                        (!healer->GetMaxPower(POWER_MANA) ||
                                         healer->GetPowerPct(POWER_MANA) >= RestState::OwnManaLine(healer));
                    bool const survives = damage < health && healed;
                    // logged on a change of the verdict per member and mob
                    thread_local std::map<std::pair<uint64, uint64>, bool> verdicts;
                    if (verdicts.size() > 4096)
                        verdicts.clear();
                    auto const key = std::make_pair(bot->GetGUID().GetRawValue(), mob->GetGUID().GetRawValue());
                    if (auto const was = verdicts.find(key); was == verdicts.end() || was->second != survives)
                    {
                        verdicts[key] = survives;
                        LOG_INFO("playerbots",
                                 "[lead985] {}: no headroom on {} ({:.0f} -> {:.0f} vs tank {:.0f} -> {:.0f}); "
                                 "{} dps on it ({:.0f} cast) x {:.1f} s to take it back ({}) = {:.0f} vs {:.0f} "
                                 "health, healer {}: {}",
                                 bot->GetName(), mob->GetName(), p.mine, p.mineNext, p.tank, p.tankNext, incoming,
                                 spellPart, takeBack, how, damage, health, healed ? "reaches it" : "does not",
                                 survives ? "keeps its damage" : "the Stop stands");
                    }
                    return survives;
                };
                if (bot == healer || PlayerbotAI::IsHeal(bot))
                {
                    // [lead61] user: the leader starts the heals - once every fight mob is on the tank (no heal
                    // threat on a mob still coming), or at once when a member is about to die (the config's critical
                    // health)
                    bool allOnTank = true;
                    // [lead62] user: no fixed line - a member is about to die when its time to death (its health over
                    // the damage it took per second in this fight) is within the healer's time to land a heal (its
                    // fastest known heal's cast + the global cooldown + the walk into heal reach) plus the next cast
                    float castSec = 0.0f;
                    {
                        static constexpr uint32 HEALS[] = {2061, 8004, 19750, 8936, 50464, 331, 635, 5185, 2050};
                        for (uint32 spell : HEALS)
                            if (bot->HasSpell(spell))
                                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                                {
                                    float const sec = info->CalcCastTime(bot) / 1000.0f;
                                    castSec = castSec > 0.0f ? std::min(castSec, sec) : sec;
                                }
                    }
                    constexpr float GCD_SEC = 1.5f;  // the game's global cooldown
                    std::string emergencyWhy;
                    bool emergency = false;
                    std::vector<Player*> watched = seats;
                    watched.push_back(tank);
                    for (Player* member : watched)
                    {
                        if (member != tank)
                            for (Unit* attacker : member->getAttackers())
                                if (attacker && attacker->IsAlive() && attacker->GetVictim() == member)
                                    allOnTank = false;
                        // [lead63] predictive: the dps its attackers deal it now (AttackerDps), a new attacker counts
                        // before its first hit; time to death = health + absorbs over that sum
                        float incoming = 0.0f;
                        std::vector<std::pair<float, std::string>> byAttacker;
                        for (Unit* attacker : member->getAttackers())
                            if (attacker && attacker->IsAlive() && attacker->GetVictim() == member)
                            {
                                float spellPart = 0.0f;
                                float const dps = AttackerDps(attacker, member, spellPart);
                                incoming += dps;
                                byAttacker.emplace_back(dps, Acore::StringFormat("{} {:.0f}{}", attacker->GetName(),
                                                                                  dps, spellPart > 0.0f ? " (cast)" : ""));
                            }
                        // [lead980] b228 rfc-h 5D-4 336.7 s: the mage at 18 % stood among Troggs whose victim was the
                        // healer on the level above (no path to it); with no attacker "on" it the mage was no
                        // emergency and the heals waited ("a fight mob attacks a member") 0.3 s before a Trogg's
                        // swing killed it. A mob that has the member on its threat list and stands in its melee reach
                        // hits it on its next victim pick: its damage counts (not crowd-controlled, not held by the
                        // tank)
                        for (auto const& [mobGuid, ref] : member->GetThreatMgr().GetThreatenedByMeList())
                        {
                            Unit* const nearMob = ref ? ref->GetOwner() : nullptr;
                            // a mob the tank holds is no danger to the melee beside it (its victim stays the tank)
                            if (!nearMob || !nearMob->IsAlive() || nearMob->GetVictim() == member ||
                                nearMob->GetVictim() == tank || nearMob->HasUnitState(UNIT_STATE_CONTROLLED) || !nearMob->IsWithinMeleeRange(member))
                                continue;
                            float spellPart = 0.0f;
                            // its swings only: the spell it casts now goes to its victim
                            float const dps = AttackerDps(nearMob, member, spellPart) - spellPart;
                            incoming += dps;
                            byAttacker.emplace_back(dps, Acore::StringFormat("{} {:.0f} (in reach)", nearMob->GetName(),
                                                                              dps));
                        }
                        if (incoming <= 0.0f)
                            continue;
                        float const absorbs = float(member->GetTotalAuraModifier(SPELL_AURA_SCHOOL_ABSORB));
                        float const toDeath = (float(member->GetHealth()) + absorbs) / incoming;
                        float const walk = std::max(0.0f, bot->GetExactDist(member) - Leader::LEADER_HEAL_REACH_YD) /
                                           std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
                        float const response = castSec + GCD_SEC + walk + castSec;
                        if (toDeath <= response)
                        {
                            emergency = true;
                            std::sort(byAttacker.begin(), byAttacker.end(),
                                      [](auto const& a, auto const& b) { return a.first > b.first; });
                            std::string top;
                            for (size_t i = 0; i < byAttacker.size() && i < 3; ++i)
                                top += (i ? ", " : "") + byAttacker[i].second;
                            emergencyWhy = Acore::StringFormat(
                                "{} dies in {:.1f} s ({:.0f} hp + {:.0f} absorb / {:.0f} dps: {}), the heal lands in "
                                "{:.1f} s",
                                member->GetName(), toDeath, float(member->GetHealth()), absorbs, incoming, top,
                                response);
                            break;
                        }
                    }
                    for (Unit* attacker : tank->getAttackers())
                        if (attacker && attacker->IsAlive() && attacker->GetVictim() != tank)
                            allOnTank = false;
                    // [lead390] b201 rfc-h 5B: the holy paladin took 14 mobs off the warrior. 740.5 s: Heal "every
                    // fight mob is on the tank" 0.3 s before the pull started - the kill order held the target alone,
                    // its four pack mates were in no member's attackers yet; at 742.6 s the paladin had 32.2 threat on
                    // each of them and the warrior 0 (threat_lead), the Voidwalker went to the healer. 573.4 s: "the
                    // heals go on: headroom on every fight mob" while two Enforcers joining the pack were not in the
                    // kill order. And a Heal, once out, stayed for the fight. The game splits a heal's threat over
                    // every mob that has the healed member on its threat list (ThreatManager::
                    // ForwardThreatForAssistingMe): those mobs count, each on the tank with the tank's threat on it,
                    // and the healer's threat after its next GCD below the share the game moves a mob at over the
                    // tank's (the damage dealers' projection, lead243). Without that the Heal goes back to a hold;
                    // the emergency (a member about to die) heals regardless
                    std::vector<Unit*> healReach;
                    std::set<ObjectGuid> reachSeen;
                    auto const addReach = [&](Unit* mob)
                    {
                        // a crowd-controlled mob takes no assist threat (the game skips it): no fact for the heals
                        if (mob && mob->IsAlive() && mob->GetMap() == map &&
                            !mob->HasUnitState(UNIT_STATE_CONTROLLED) && reachSeen.insert(mob->GetGUID()).second)
                            healReach.push_back(mob);
                    };
                    for (Player* member : watched)
                        for (auto const& [mobGuid, ref] : member->GetThreatMgr().GetThreatenedByMeList())
                            if (ref)
                                addReach(ref->GetOwner());
                    for (ObjectGuid const& fightMob : killOrder)
                        addReach(ObjectAccessor::GetUnit(*tank, fightMob));
                    // [lead574] b215 rfc-h: the healer's Heal <-> DontAttack flipped 330 times within 1 s on all 8
                    // comps (5D 547.7 DontAttack "no headroom on Searing Blade Cultist" -> 548.0 Heal, the same one
                    // mob). 61 of them closed and opened on the healer's headroom with the same mobs: its rate there
                    // was its threat over the seconds since its first threat, and those seconds grew on every look
                    // while it waited - the wait itself lowered the projection until the gate opened, its next heal
                    // closed it again (the clock, no fact, opened it). The gate reads both sides' rates up to their
                    // last threat rise: the projection changes only when a heal lands or the tank's threat grows
                    // (the other flips - 181 "not on the tank", 69 "a fight mob attacks a member" - are the game's
                    // victim / threat facts; their reason now names the mob's victim and the tank's threat)
                    auto const riseRate = [](Leader::DungeonLeaderState::ThreatSeen const& seen)
                    {
                        if (seen.threat <= 0.0f || !seen.riseMs)
                            return 0.0f;
                        float const sec =
                            std::max(getMSTimeDiff(seen.firstMs, seen.riseMs) / 1000.0f, HEADROOM_GCD_SEC);
                        return std::max(0.0f, seen.threat - seen.base) / sec;
                    };
                    float healFresh = 0.0f;
                    if (auto const measured = lead.openTps.find(guid);
                        measured != lead.openTps.end() && measured->second.second)
                        healFresh = measured->second.first / float(measured->second.second);
                    for (ObjectGuid const& other : killOrder)
                        if (Unit* fought = ObjectAccessor::GetUnit(*tank, other); fought && fought->IsAlive())
                            healFresh = std::max(healFresh, riseRate(seenOn(fought, bot)));
                    auto const healHeadroomOn = [&](Unit* mob)
                    {
                        auto const& mine = seenOn(mob, bot);
                        auto const& held = seenOn(mob, tank);
                        auto const rateHere = [&](Leader::DungeonLeaderState::ThreatSeen const& seen, float opening)
                        {
                            float const onMob = riseRate(seen);
                            bool const measuredHere =
                                seen.threat > 0.0f &&
                                getMSTimeDiff(seen.firstMs, seen.riseMs) / 1000.0f >= HEADROOM_GCD_SEC;
                            return measuredHere ? onMob : std::max(onMob, opening);
                        };
                        float mineNext = mine.threat + rateHere(mine, healFresh) * HEADROOM_GCD_SEC;
                        // [lead901] its running heals / damage over time still add threat after the heals stop
                        float horizon = HEADROOM_GCD_SEC;
                        if (PeriodicLeft const periodic = PeriodicThreatLeft(bot, mob); periodic.threat > 0.0f)
                        {
                            mineNext = mine.threat +
                                       std::max(mineNext - mine.threat, periodic.within) +
                                       (periodic.threat - periodic.within);
                            horizon = std::max(horizon, periodic.sec);
                        }
                        float const tankNext =
                            held.threat > 0.0f
                                ? held.threat + rateHere(held, tankOpensOn(mob) ? tankOpen : 0.0f) * horizon
                                : 0.0f;
                        float const share = mob->GetVictim() != tank       ? 1.0f
                                            : mob->IsWithinMeleeRange(bot) ? THREAT_OVERTAKE_MELEE
                                                                           : THREAT_OVERTAKE_RANGED;
                        return mineNext < share * tankNext;
                    };
                    std::string closedOn = allOnTank ? "" : "a fight mob attacks a member";
                    for (Unit* mob : healReach)
                    {
                        if (!closedOn.empty())
                            break;
                        if (mob->GetVictim() != tank || mob->GetThreatMgr().GetThreat(tank) <= 0.0f)
                        {
                            // [lead574] its victim and the tank's threat named (a pet / totem it hits is not the tank)
                            Unit* const on = mob->GetVictim();
                            closedOn = Acore::StringFormat("{} is not on the tank with the tank's threat (on {}, "
                                                           "tank threat {:.0f})",
                                                           mob->GetName(), on ? on->GetName() : std::string("nobody"),
                                                           mob->GetThreatMgr().GetThreat(tank));
                        }
                        else if (!healHeadroomOn(mob))
                            closedOn = "no headroom on " + mob->GetName();
                    }
                    if (closedOn.empty() || emergency)
                    {
                        if (Leader::Events::SetAttack(guid, Leader::AttackEvent::Heal, ObjectGuid::Empty, {}))
                            logEvent(bot, "Heal",
                                     JsonObject()
                                         .Str("why", emergency ? emergencyWhy
                                                               : "every mob the heals reach is on the tank, headroom "
                                                                 "on each")
                                         .Num("mobs", int64(healReach.size())));
                        lead.healStarted = true;
                    }
                    else if (Leader::Events::SetAttack(guid, Leader::AttackEvent::DontAttack, ObjectGuid::Empty, {}))
                        logEvent(bot, "DontAttack", JsonObject().Str("why", "the heals wait: " + closedOn)
                                                        .Num("mobs", int64(healReach.size())));
                }
                else if (stopDps)
                {
                    if (Leader::Events::SetAttack(guid, Leader::AttackEvent::StopDps, ObjectGuid::Empty, {}))
                        logEvent(bot, "StopDps", JsonObject().Str("why", "no target of the fight takes damage"));
                }
                else if (!target)
                {
                    if (Leader::Events::SetAttack(guid, Leader::AttackEvent::DontAttack, ObjectGuid::Empty, {}))
                        logEvent(bot, "DontAttack", JsonObject().Str("why", "the fight has no target for it yet"));
                }
                else
                {
                    // [lead53] user: «0 aggro off the tank»: each damage dealer attacks a mob it has threat headroom
                    // on - its threat below the tank's on THAT mob (ThreatManager, no number of ours): the leader's
                    // focus mob first, else the next of the kill order; AoE only while every fight mob in its reach
                    // leaves it headroom
                    // [lead163] b178 rfc-h 5D (20261101-171156): the unholy DK pulled aggro 13 times, tps 2126 at
                    // 970 dps, and died - headroom read the threat it had, not the threat its next cast adds (Icy
                    // Touch, Death and Decay, Blood Boil). Headroom: its threat plus its own threat rate on this mob
                    // over one global cooldown (the game's 1.5 s) stays below the tank's
                    // [lead229] b187 rfc-h 5D: the unholy DK took 12 of the 18 mobs off the bear, most 12-29 yd from
                    // it (a mob the bear had touched once, Icy Touch / Death Coil from range): on a mob it had not
                    // hit yet its rate there was 0, so its first cast's threat was never counted
                    // [lead240] b189 rfc-h 5C: the fury warrior took 24 mobs, 30 of the run's 49 losses 8-35 yd from
                    // the tank - early in a fight its rate was still 0 (nothing hit yet)
                    // [lead243] b190 rfc-h: headroom stops doubled (5A 47 -> 79, 5D 64 -> 116), fight time 5A +81 s,
                    // and the losses barely fell (73 -> 60, 5C warrior 8, most with headroom): lead240's rate (its top
                    // mob's threat per fight second, 5C warrior ~1100) was below what a member adds while it hits
                    // (2092 at 2 -> 5 s), and the line was the tank's threat now. Both sides as the game moves a mob:
                    // the member's threat after one GCD at its rate on THAT mob (since its first threat there; on a
                    // mob it has not hit, its fastest rate of this fight or the run's opening rate) against the
                    // tank's after the same GCD at the tank's rate there, times the share the game moves a mob at
                    // [lead67] b112: Rtshaab out-threatened the tank on the OTHER Earthborer (procs, totems, cleave):
                    // a mob is attackable only while the member has headroom on it AND on every fight mob its damage
                    // can reach around it (the AoE radius)
                    // [lead89] b131 rfc-h: 35-44 headroom stops per damage dealer while the tank's lead on their target
                    // averaged 3.9 at the end - a mob near the target the tank had not hit yet (0 against 0) stopped
                    // a ranged single-target attacker too. The mobs around the target count for a member whose damage
                    // splashes onto them (a melee: cleave, procs, its totems); the AoE gate below holds every AoE
                    bool const splashes = !PlayerbotAI::IsRanged(bot);
                    auto const headroom = [&](Unit* mob)
                    {
                        if (!headroomOn(mob))
                            return false;
                        if (!splashes)
                            return true;
                        for (ObjectGuid const& other : killOrder)
                            if (Unit* close = ObjectAccessor::GetUnit(*tank, other);
                                close && close != mob && close->IsAlive() &&
                                mob->GetExactDist(close) <= LEADER_AOE_RADIUS && !headroomOn(close))
                                return false;
                        return true;
                    };
                    // [lead196] b181: the leader's Stop was the biggest damage loss (enh 5A ~300 held casts and its
                    // white swings 1275 vs the sim's 1865, the frost DK's whole rotation in 5G): a melee member with
                    // headroom on its target was stopped by a neighbour within LEADER_AOE_RADIUS the tank had not
                    // hit yet (0 against 0). Its single-target attack lands on its target only; what splashes is
                    // its area damage, which the AoE rule below forbids on such a neighbour. The pick and the Stop
                    // read the member's headroom on the mob it attacks
                    // [lead413] user 2026-11-02 (lead392 reverted, gap closers allowed): b201 5H 606.1 s the cat got
                    // Attack on a Cultist the bear had just growled in a LoS pull, still at its pack 9 yd from unpulled
                    // Enforcers - its Feral Charge landed in the pack. A melee member not yet in melee reach of the mob
                    // takes it only when the mob, where the member reaches it (its walk's or charge's end at the
                    // mob), is out of every unpulled mob's aggro reach (the spot guard's IdleMobReachesSpot): in a LoS
                    // pull it waits until the mob came to the tank
                    std::string unpulledNear;
                    // [lead731] b219 rfc-h: 9-13 A -> B -> A Attack switches within 3 s per melee member a run (5A
                    // 430-433 s Rtshaab Searing Blade Cultist <-> Molten Elemental every 0.3 s, 35 yd off; 5F Rtrogac
                    // 20 between two Troggs). Two deciders read the member's own walk: the arrival test below judged
                    // the held target from where the member stood that tick (its chase moved it, the arrival point
                    // swung in and out of an idle mob's reach), and lead136's out-of-reach pick took the nearest mob
                    // every tick (the walk to one made the other nearer). The held target keeps the arrival it was
                    // taken with while it stands where it stood (a fact: it moved, an idle mob woke or moved, is
                    // re-judged), and the out-of-reach pick leaves the held target only for a mob at the tank when the
                    // held one is not
                    Unit* heldTarget = nullptr;
                    if (Leader::MemberEvents current;
                        Leader::Events::Get(guid, current) && current.attack == Leader::AttackEvent::Attack)
                        heldTarget = ObjectAccessor::GetUnit(*tank, current.target);
                    std::map<ObjectGuid, Position> arrivals;
                    auto const pickable = [&](Unit* mob)
                    {
                        if (!headroomOn(mob) && !survivesOn(mob))  // [lead985]
                            return false;
                        if (!splashes || PlayerbotAI::IsHeal(bot) || bot->IsWithinMeleeRange(mob))
                            return true;
                        // [lead432] b202 rfc-h 5H / 5A: 19 / 13 cat / shaman stops "<boss> stands in Searing Blade
                        // Cultist's aggro reach" (Jergosh, Taragaman, Bazzalan) while the tank held the boss there -
                        // the test read the mob's centre, not where the member arrives (lead413's own words). Where
                        // its walk or charge ends: at the mob's combat reach plus its own, on its side of the mob
                        Position arrive = bot->GetPosition();
                        float const gap = bot->GetExactDist2d(mob);
                        float const reach = mob->GetCombatReach() + bot->GetCombatReach();
                        if (gap > reach)
                        {
                            float const step = reach / gap;
                            arrive.Relocate(mob->GetPositionX() + (bot->GetPositionX() - mob->GetPositionX()) * step,
                                            mob->GetPositionY() + (bot->GetPositionY() - mob->GetPositionY()) * step,
                                            mob->GetPositionZ());
                        }
                        // [lead731] the held target's arrival as it was taken, while the mob stands there (within
                        // the arrival's own scale: the combat reaches it was placed at)
                        if (auto const kept = lead.attackArrive.find(guid);
                            mob == heldTarget && kept != lead.attackArrive.end() &&
                            kept->second.target == mob->GetGUID() &&
                            kept->second.meet.GetExactDist(mob->GetPosition()) <= reach)
                            arrive = kept->second.point;
                        arrivals[mob->GetGUID()] = arrive;
                        // [lead721] b219 rfc-h (8 runs): 34 of the 204 "waits: <mob> stands in <idle>'s aggro reach"
                        // stops came with the mob already on the tank (<= 12 yd, 5C 151.1 s, 5F 545.7 s ...) - this
                        // test read the plain unpulled reach, while the spots and walks the leader issues read the
                        // fight at the target (lead490 / lead492: no deeper into an idle mob's reach than the target's
                        // victim stands unwoken). The arrival is judged by that same fact; a mob still at its pack (no
                        // victim standing in that reach) waits as before
                        if (Creature* idle = LeaderFightAggroAt(bot, arrive, mob))
                        {
                            if (unpulledNear.empty())
                                unpulledNear = Acore::StringFormat("{} stands in {}'s aggro reach", mob->GetName(),
                                                                   idle->GetName());
                            return false;
                        }
                        return true;
                    };
                    // [lead451] b205 rfc-h 5H / 5C: the fire mages held Fireball on their dying Attack target
                    // ([cast-short-life] "it lands in 3.5-4.6 s, the target lives 0.1-3.9 s") and wanded it - 139 / 81
                    // s of their fights on Shoot (wand 3 % of their damage) while the pack's other mobs lived on. A
                    // ranged member's cast it held on a mob (CastLand: the fact it reports) lands only on a mob that
                    // lives that long (ExpectedLifetime, the same estimate): such a mob is its pick first; the mobs it
                    // would not land on stay its pick only when no other mob is
                    CastLand::Held castHeld;
                    bool const castNoted = PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsHeal(bot) &&
                                           CastLand::Get(guid, castHeld) && castHeld.landsSec > 0.0f;
                    PlayerbotAI* const memberAI = castNoted ? GET_PLAYERBOT_AI(bot) : nullptr;
                    auto const castLands = [&](Unit* mob)
                    { return !memberAI || ExpectedLifetime(memberAI, mob) >= castHeld.landsSec; };
                    // [lead883] b224 rfc-h 5E Rtlocab (destruction): 14 s of "the attack refused: <mob> (no line of
                    // sight)" over the run (762-768 s: a Voidwalker Minion, then a Cultist, then a Warlock, each
                    // round the corner while other kill-order mobs stood in its sight). A ranged damage dealer's pick
                    // is a mob it sees from where it stands first; an unseen one only when it sees none (its spot
                    // walk brings it into sight then)
                    bool const sightFirst = PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsHeal(bot);
                    auto const seesMob = [&](Unit* mob) { return bot->IsWithinLOSInMap(mob); };
                    // [lead991] b228 rfc-h 5H-8: the fire mage's Attack was a fire-immune Molten Elemental while
                    // other kill-order mobs fought (40 s a run on the wand, "fireball ... target immune"; 5C 9 s). A
                    // ranged damage dealer's pick is a mob its rotation's school hurts first (the mob's own immunity,
                    // Unit::IsImmunedToDamageOrSchool); an immune one only when no other is (its spot then reads the
                    // reach of the spells that hurt it, LeaderCastReach)
                    bool const schoolFirst = PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsHeal(bot);
                    Unit* mine = nullptr;
                    for (uint8 pass = 0; pass < 8; ++pass)
                    {
                        bool const needSchool = pass < 4;
                        bool const needSight = pass % 4 < 2;
                        bool const needLanding = pass % 2 == 0;
                        if (mine)
                            break;
                        // without a held cast (no memberAI) every mob "lands": the landing passes are the only ones
                        if ((needSight && !sightFirst) || (!needLanding && !memberAI) || (needSchool && !schoolFirst))
                            continue;
                        auto const pickHere = [&](Unit* mob)
                        {
                            return pickable(mob) && (!needLanding || castLands(mob)) && (!needSight || seesMob(mob)) &&
                                   (!needSchool || !LeaderRotationImmune(bot, mob));
                        };
                        // [lead236] b188 rfc-h 5G 125-139 s: the ret paladin's Attack flipped Shaman <-> Trogg every
                        // leader tick (0.3 s, 30 changes) - lead229's rate moves with every hit, the leader's focus mob
                        // passed and failed the test in turn, and each switch opened a fresh mob: 16 aggro losses for
                        // Rtpalae. The mob it attacks now stays its target while it keeps headroom on it (one command
                        // per fact)
                        if (Leader::MemberEvents current;
                            Leader::Events::Get(guid, current) && current.attack == Leader::AttackEvent::Attack)
                            if (Unit* held = ObjectAccessor::GetUnit(*tank, current.target);
                                held && held->IsAlive() && held->IsInCombat() &&
                                std::find(killOrder.begin(), killOrder.end(), held->GetGUID()) != killOrder.end() &&
                                (!lead.looseIsTarget || held == target) && pickHere(held))  // [lead245] loose mob first
                                mine = held;
                        if (!mine && pickHere(target))
                            mine = target;
                        for (ObjectGuid const& next : killOrder)
                        {
                            if (mine)
                                break;
                            if (Unit* mob = ObjectAccessor::GetUnit(*tank, next);
                                mob && mob->IsAlive() && mob->IsInCombat() && pickHere(mob))
                                mine = mob;
                        }
                    }
                    if (sightFirst && mine && mine != target && !seesMob(target))
                    {
                        thread_local std::set<std::pair<uint64, uint64>> sightLogged;
                        if (sightLogged.insert({bot->GetGUID().GetRawValue(), mine->GetGUID().GetRawValue()}).second)
                            LOG_INFO("playerbots", "[lead883] {}: attacks {} it sees, not the unseen {}",
                                     bot->GetName(), mine->GetName(), target->GetName());
                    }
                    // [lead136] b162 20261101-121409: the shaman moved 377 s, 48 % of its fight time not acting - 121
                    // "combat spot: behind the target" moves, 63 re-spots "out of melee reach". A melee member out of
                    // melee reach of its pick takes the kill order's mob nearest to it that it has headroom on (its
                    // walk is the shortest); in reach, it keeps it
                    // [lead142] b164 20261101-125734 871-940 s: the shaman's Attack target was a Searing Blade
                    // Cultist / Warlock standing back 19-60 yd (casters the tank had not brought), it had no combat
                    // spot there and stood ("drop target") while the tank fought mobs in its melee. A caster standing
                    // back is the tank's to bring (LoS pull / taunt): a melee member takes a kill-order mob on the
                    // tank in the tank's melee first (the tank stands at it: a spot it can reach), the nearest of them
                    if (splashes && !PlayerbotAI::IsHeal(bot) && mine && !bot->IsWithinMeleeRange(mine))
                    {
                        auto const atTank = [&](Unit* mob)
                        { return mob->GetVictim() == tank && tank->IsWithinMeleeRange(mob); };
                        bool bestAtTank = atTank(mine);
                        float best = bot->GetExactDist(mine);
                        bool const keepsHeld = mine == heldTarget;  // [lead731]
                        for (ObjectGuid const& next : killOrder)
                            if (Unit* mob = ObjectAccessor::GetUnit(*tank, next);
                                mob && mob != mine && mob->IsAlive() && mob->IsInCombat() && pickable(mob))
                            {
                                bool const at = atTank(mob);
                                float const distance = bot->GetExactDist(mob);
                                if ((at && !bestAtTank) || (!keepsHeld && at == bestAtTank && distance < best))
                                {
                                    bestAtTank = at;
                                    best = distance;
                                    mine = mob;
                                }
                            }
                    }
                    // [lead414] user 2026-11-02: a melee member's passive splash (diseases spread by Wandering Plague
                    // / Pestilence, Blade Flurry) builds threat on fight mobs it does not attack - b201 rfc-h 5D the
                    // unholy DK lost 10 mobs. Its measured threat on such a mob the tank holds projects like any
                    // other (headroomOn); one that overtakes the tank holds its damage (hold the dps to avoid wipes)
                    // [lead432] b202 rfc-h 5A: Rtshaab held 14 times "its splash threat on Jergosh the Invoker /
                    // Ragefire Trogg overtakes the tank" - threat it had put on the mob while it attacked it, projected
                    // on at its average rate there after it had switched away (the projection's rate is its threat
                    // over the seconds since its first threat there). A mob it does not attack gains its threat only
                    // while its splash lands: the projection holds it only while its threat there still grows (within
                    // the game's global cooldown)
                    // [lead852] b223 rfc-h 5D Oggleflint 264.9 / 269.1 / 270.3 s: the unholy DK's Stop ("its splash
                    // threat on Ragefire Shaman / Oggleflint / Ragefire Trogg overtakes the tank") and its burn
                    // alternated every 0.3-1 s (leader_contradiction "1:army of the dead" -> "2" -> again) - its
                    // diseases there tick every 3 s, the window above was one global cooldown: between two ticks its
                    // threat "no longer grew" though nothing had changed. Its threat there grows while its own periodic
                    // damage is on the mob: the window is that aura's tick (spell data) as the leader samples it (its
                    // fight tick), a global cooldown with none
                    auto const growsWithin = [&](Unit* mob) -> uint32
                    {
                        uint32 tickMs = 0;
                        for (AuraType type : {SPELL_AURA_PERIODIC_DAMAGE, SPELL_AURA_PERIODIC_LEECH})
                            for (AuraEffect const* periodic : mob->GetAuraEffectsByType(type))
                                if (periodic->GetCasterGUID() == bot->GetGUID() && periodic->GetAmplitude() > 0)
                                    tickMs = std::max(tickMs, uint32(periodic->GetAmplitude()));
                        return tickMs ? tickMs + LEADER_FIGHT_TICK_MS : uint32(HEADROOM_GCD_SEC * IN_MILLISECONDS);
                    };
                    // [lead885] b224 rfc-h 5B 1007.2 s: the shadow priest Rtpriaa, told Jergosh, had 1776 threat on a
                    // Searing Blade Cultist it never hit (the tank 1043, the mob on the tank till then): Vampiric
                    // Embrace's heals and Vampiric Touch's Replenishment split their threat over every mob that has a
                    // healed / energized member on its threat list (ThreatManager::ForwardThreatForAssistingMe). A
                    // ranged member's threat rising on a mob it does not attack is that split; its own damage feeds it,
                    // so it holds like a melee member's splash
                    if (mine && !PlayerbotAI::IsHeal(bot))
                        for (ObjectGuid const& other : killOrder)
                            if (Unit* splashed = ObjectAccessor::GetUnit(*tank, other);
                                splashed && splashed != mine && splashed->IsAlive() &&
                                splashed->GetVictim() == tank && seenOn(splashed, bot).threat > 0.0f &&
                                getMSTimeDiff(seenOn(splashed, bot).riseMs, now) <= growsWithin(splashed) &&
                                !headroomOn(splashed))
                            {
                                unpulledNear = Acore::StringFormat("its splash threat on {} overtakes the tank",
                                                                   splashed->GetName());
                                thread_local std::set<std::pair<uint64, uint64>> splashLogged;
                                if (uint32 const window = growsWithin(splashed);
                                    window > uint32(HEADROOM_GCD_SEC * IN_MILLISECONDS) &&
                                    getMSTimeDiff(seenOn(splashed, bot).riseMs, now) >
                                        uint32(HEADROOM_GCD_SEC * IN_MILLISECONDS) &&
                                    splashLogged
                                        .insert({bot->GetGUID().GetRawValue(), splashed->GetGUID().GetRawValue()})
                                        .second)
                                    LOG_INFO("playerbots", "[lead852] {}: its splash on {} still grows within its "
                                             "periodic tick ({} ms): the stop holds",
                                             bot->GetName(), splashed->GetName(), window);
                                if (!splashes && splashLogged
                                                     .insert({bot->GetGUID().GetRawValue(),
                                                              splashed->GetGUID().GetRawValue() ^ 1u})
                                                     .second)
                                    LOG_INFO("playerbots", "[lead885] {}: its split threat on {} (not attacked) "
                                             "overtakes the tank: holds its damage",
                                             bot->GetName(), splashed->GetName());
                                mine = nullptr;
                                break;
                            }
                    // [lead875] user 2026-11-03: b224 rfc-h had 21 damage dealer losses "no tank threat on it" with no
                    // hit of theirs on the mob (5A 740.1 s the frost mage at 9.9 threat on a Searing Blade Cultist, a
                    // Warlock and its Voidwalker the warrior had 0 on; 5B the shadow priest at 0.1-15). The game
                    // splits the threat of a heal, an energize (Judgement of Wisdom, Replenishment, a mana potion) and
                    // a buff over every mob that has the member on its threat list (ThreatManager::
                    // ForwardThreatForAssistingMe), and a mob where the tank has 0 goes to the first member with any.
                    // A damage dealer does not attack while a fight mob that has it on its threat list has 0 tank
                    // threat; the Stop lifts as soon as the tank has threat on every such mob. The tank takes each
                    // one: a mob on a member is the leader's aggro loss (above) and the tank's take-back, a mob out of
                    // its melee its open-incoming, a mob in its melee its lowest-threat target (TankTargetValue). A
                    // mob under crowd control takes no split threat (the game skips it); a fleer is the group's kill
                    // (lead200), not the tank's
                    if (mine && !PlayerbotAI::IsHeal(bot))
                        for (auto const& [listingGuid, ref] : bot->GetThreatMgr().GetThreatenedByMeList())
                            if (Creature* listing = ref ? ref->GetOwner() : nullptr;
                                listing && listing->IsAlive() && listing->GetMap() == map && listing->IsInCombat() &&
                                !listing->HasUnitState(UNIT_STATE_CONTROLLED) && !Leader::Fight::IsFleer(listing) &&
                                LeaderFightMob(tank, listing) && listing->GetThreatMgr().GetThreat(tank) <= 0.0f)
                            {
                                unpulledNear = Acore::StringFormat(
                                    "a fight mob {} has it on its threat list at 0 tank threat", listing->GetName());
                                thread_local std::set<std::pair<uint64, uint64>> zeroTankLogged;
                                if (zeroTankLogged.insert({bot->GetGUID().GetRawValue(), listingGuid.GetRawValue()})
                                        .second)
                                    LOG_INFO("playerbots", "[lead875] {}: holds its damage, {} has it on its threat "
                                             "list at {:.1f} with the tank at 0 (attack on {})",
                                             bot->GetName(), listing->GetName(),
                                             listing->GetThreatMgr().GetThreat(bot), mine->GetName());
                                mine = nullptr;
                                break;
                            }
                    // [lead570] b215 rfc-h: 18 of the 25 "it attacks a mob it was not told to" losses (5D 557.3 /
                    // 859.0 s the unholy DK, 5B 859.1 / 975.9 s the elemental shaman) came one tick after the member
                    // lost its headroom: the Attack went from the mob it hit to the leader's focus with the Stop
                    // (Cultist -> Enforcer -> Cultist within 1 s), its victim was still the old mob, and the chase /
                    // combat spot walked it to the new one. Without headroom the member does no damage anywhere (the
                    // Stop): its live Attack target stays the one it has (one command per fact, lead236); only a
                    // loose mob the leader made the focus (lead245) or a target that left the fight moves it
                    Unit* attackOn = mine ? mine : target;
                    if (!mine && !lead.looseIsTarget)
                        if (Leader::MemberEvents current;
                            Leader::Events::Get(guid, current) && current.attack == Leader::AttackEvent::Attack)
                            if (Unit* held = ObjectAccessor::GetUnit(*tank, current.target);
                                held && held != target && held->IsAlive() && held->IsInCombat() &&
                                std::find(killOrder.begin(), killOrder.end(), held->GetGUID()) != killOrder.end())
                                attackOn = held;
                    // [lead571] the mob its Attack leaves, for the aggro-loss cause (the switch in flight)
                    if (Leader::MemberEvents before;
                        Leader::Events::Get(guid, before) && before.attack == Leader::AttackEvent::Attack &&
                        !before.target.IsEmpty() && before.target != attackOn->GetGUID())
                        lead.attackLeft[guid] = {before.target, now};
                    Leader::AoeRule memberAoe = aoe;
                    std::string memberAoeWhy = aoeWhy;
                    // [lead415] the fight's mobs this member has no headroom on are spared too: each AoE's own
                    // footprint (spell data) decides whether it reaches one
                    std::vector<ObjectGuid> memberSpared = lead.aoeSpared;
                    if (memberAoe != Leader::AoeRule::Forbidden)
                        for (ObjectGuid const& other : killOrder)
                            if (Unit* mob = ObjectAccessor::GetUnit(*tank, other);
                                mob && mob->IsAlive() && !headroom(mob))
                            {
                                if (memberSpared.size() == lead.aoeSpared.size())
                                    memberAoeWhy = "spares " + mob->GetName() + " (no threat headroom)";
                                memberSpared.push_back(mob->GetGUID());
                            }
                    // [lead731] the arrival the pick was taken with, kept for the held target (see heldTarget)
                    if (mine)
                        if (auto const arrived = arrivals.find(mine->GetGUID()); arrived != arrivals.end())
                        {
                            auto& kept = lead.attackArrive[guid];
                            if (kept.target != mine->GetGUID() ||
                                kept.meet.GetExactDist(mine->GetPosition()) >
                                    mine->GetCombatReach() + bot->GetCombatReach())
                                kept = {mine->GetGUID(), mine->GetPosition(), arrived->second};
                        }
                    lead.noHeadroom[guid] = !mine;  // no mob with headroom: it waits (Intensity Stop below)
                    lead.attackWaitWhy[guid] = !mine ? unpulledNear : std::string();  // [lead413]
                    if (!mine)                      // [lead246] the projection on its Attack target, for the Stop's log
                    {
                        Projection const p = project(attackOn);
                        lead.stopProjection[guid] = JsonObject()
                                                        .Str("mob", attackOn->GetName())
                                                        .Real("mine", p.mine)
                                                        .Real("mineNext", p.mineNext)
                                                        .Real("tank", p.tank)
                                                        .Real("tankNext", p.tankNext)
                                                        .Real("share", p.share)
                                                        .Build();
                    }
                    // [lead520] b211 rfc-h 5H: the fire mage's Living Bomb on the bosses' adds (Oggleflint's Shaman /
                    // Trogg, the Cultist with Jergosh / Bazzalan) was held by its own threat gate ("[threat-hold]
                    // holds living bomb on attacker ... 122 %") on mobs the leader's kill order named for it with and
                    // without headroom. The leader names the mobs it allows this member's multi-DoT / cleave on: its
                    // Attack target and every kill-order mob it has threat headroom on (the leader's one threat
                    // verdict, ThreatStrategy follows it); a mob without headroom is not named, so not hit
                    std::vector<ObjectGuid> named = {attackOn->GetGUID()};
                    for (ObjectGuid const& other : killOrder)
                        if (Unit* mob = ObjectAccessor::GetUnit(*tank, other);
                            mob && mob != attackOn && mob->IsAlive() && mob->IsInCombat() && headroom(mob))
                            named.push_back(other);
                    bool const changed = Leader::Events::SetAttack(guid, Leader::AttackEvent::Attack,
                                                                   attackOn->GetGUID(), named);
                    bool const aoeChanged = Leader::Events::SetAoe(guid, memberAoe, attackOn->GetPosition(),
                                                                   LEADER_AOE_RADIUS, memberSpared);
                    if (changed || aoeChanged)
                        logEvent(bot, "Attack", JsonObject().Str("target", attackOn->GetName())
                                                    .Num("targetEntry", attackOn->GetEntry())
                                                    .Num("targetLow", attackOn->GetGUID().GetCounter())  // [lead571]
                                                    .Num("killOrder", int64(killOrder.size()))
                                                    .Num("named", int64(named.size()))  // [lead520]
                                                    .Str("aoe", memberAoe == Leader::AoeRule::Forbidden
                                                                    ? "forbidden"
                                                                    : "allowed-on-pack")
                                                    .Str("aoeWhy", memberAoeWhy)
                                                    .Num("aoeSpared", int64(memberSpared.size()))  // [lead415]
                                                    .Bool("headroom", mine != nullptr));
                    // [lead122] b156 rfc-h 20261101-105614 91.7-117.5 s: the hunter got Attack 7 times 86 yd from
                    // its target ("the attack refused: no line of sight", nothing ran) and no spot all fight. A
                    // ranged member's Attack comes with a spot it can shoot from: where it stands when that reaches
                    // the target (its own attack reach, in sight), else its spot when that does, else the spot kept
                    // for this target, else one round the target in range and sight (one the tank holds the walk to
                    // first, then the nearest), kept for the fight while it reaches the target
                    // [lead124] the reach is the member's own (LeaderShootReach)
                    // [lead127] the melee walk to the target (lead124 / lead125) is gone: in a fight the member's
                    // position has one owner, its combat spot (the follow rule's stays that started it no longer run
                    // under an Attack, LeaderOrders.cpp MoveBarred)
                    bool const meleeMember = !PlayerbotAI::IsRanged(bot);
                    float const shootReach = LeaderShootReach(bot);
                    // [lead127] b159 20261101-113309 43.0-44.2 s / 79.6-81.1 s: the "in range of its target" MoveTo
                    // and the combat spot StandAt owned the hunter's position one after the other (it walked to one
                    // spot, was sent to another 1.5 s later, and never shot). One owner: the combat spot. A ranged
                    // member that cannot shoot its Attack target from where it stands (its own reach, the game's
                    // sight) gets its combat spot chosen now, for that target (the nearest spot that fits, else a
                    // fresh one) - the combat spot rule walks it there from the next tick on and keeps it
                    if (!meleeMember && shootReach > 0.0f)
                    {
                        // [lead157] b176 20261101-163239: the hunter at 48-59 % of wowsims, white melee 557 dps, Wing
                        // Clip / Disengage - "where it stands reaches and sees its target" took a spot in its dead
                        // zone (the mob came to the tank, the group 5-10 yd behind it). A ranged spot is also out of
                        // its minimum range (LeaderRangedMinGap)
                        // [lead158] b177 20261101-165826 22.4-26.4 s: Rthunaa re-spotted every 0.3-0.6 s against an
                        // Earthborer walking in to the tank, then "inside its minimum range", command_repeat. While
                        // the target runs in (not yet held in the tank's melee) the spot is judged against where the
                        // fight will be held - the tank - and against the mob only once the tank holds it
                        bool const runningIn = attackOn->isMoving() && tank->IsAlive() &&
                                               !(attackOn->GetVictim() == tank && tank->IsWithinMeleeRange(attackOn));
                        Unit* const judgedOn = runningIn ? static_cast<Unit*>(tank) : attackOn;
                        // [lead203] b181 (8 runs): 478 re-spots "the tank its Attack target runs to" and 293 "its
                        // Attack target", ~7 yd each, of members that cast from where they stood. One fact for every
                        // spot rule (LeaderCastsFrom): no re-spot while the member casts on its Attack target from
                        // where it stands; a new spot only when it cannot
                        // [lead210] and out of every fight mob's melee reach (the game's 110 % aggro rule there)
                        auto const shootsFrom = [&](Position const& at)
                        { return LeaderCastsFrom(bot, judgedOn, at, &killOrder); };
                        // [lead218] where it stands: the game's own sight too (MemberCastsHere); its kept spot under
                        // its feet shoots only if it does
                        // [lead380] b200 rfc-h (8 runs): 296 re-spots "the tank its Attack target runs to" and 144 "its
                        // Attack target", 104 and 29 of them within 8 yd of the tank - a mob running to the tank was
                        // judged in its melee reach already. Whether it casts where it stands is judged by the mobs
                        // where they stand now
                        bool const castsNow =
                            LeaderCastsFrom(bot, attackOn, bot->GetPosition(), &killOrder, true) &&
                            bot->IsWithinLOSInMap(attackOn);
                        auto const kept = lead.combatSpots.find(guid);
                        bool const onKept = kept != lead.combatSpots.end() &&
                                            bot->GetExactDist2d(kept->second.second.GetPositionX(),
                                                                kept->second.second.GetPositionY()) <= 2.0f;
                        bool const spotShoots = kept != lead.combatSpots.end() && shootsFrom(kept->second.second) &&
                                                (!onKept || castsNow || runningIn);
                        // [lead174] b179 rfc-h 5A: 122 re-spots "out of its reach / sight of the tank its Attack target
                        // runs to" - judged against the tank while the tank itself still walks (a pull back, the walk
                        // to its fight spot), every step of it was a new spot. Where the fight is held is a fact once
                        // the tank stands: the spot kept for this target holds while the tank walks
                        bool const tankSettling = runningIn && tank->isMoving() && kept != lead.combatSpots.end() &&
                                                  kept->second.first == attackOn->GetGUID();
                        bool const shootsHere = shootsFrom(bot->GetPosition()) && (runningIn || castsNow);
                        if (!tankSettling && !castsNow && !shootsHere && !spotShoots)
                        {
                            std::vector<HazardZone> const& zones = lead.hazardZones[guid];
                            // [lead137] never back to the spot it just left for this target
                            auto const left = lead.combatSpotLeft.find(guid);
                            // [lead218] nor where it stands when the game says it cannot cast from there
                            Position const here = bot->GetPosition();
                            bool const blindHere = !runningIn && !MemberCastsHere(bot, attackOn);
                            std::function<bool(Position const&)> const keeps = [&](Position const& at)
                            {
                                return keepsHealer(at) && !(blindHere && here.GetExactDist(at) <= 2.0f) &&
                                       !(left != lead.combatSpotLeft.end() &&
                                         std::any_of(left->second.begin(), left->second.end(),
                                                     [&](std::pair<ObjectGuid, Position> const& was)
                                                     {
                                                         return was.first == attackOn->GetGUID() &&
                                                                was.second.GetExactDist(at) <= COMBAT_SPOT_LEFT_YD;
                                                     }));
                            };
                            Position spot;
                            bool const got = slow(
                                [&]
                                {
                                    SpotAcquire const acquire;
                                    return LeaderNearestCombatSpot(bot, botAI, judgedOn, bot->GetPosition(), false,
                                                                   zones, keeps, spot, &killOrder) ||
                                           LeaderCombatSpot(bot, botAI, judgedOn, false, zones, keeps, spot,
                                                            &killOrder);  // [lead214]
                                });
                            if (got && shootsFrom(spot))
                            {
                                if (kept != lead.combatSpots.end())  // [lead137] [lead375] for this target
                                    LeaveCombatSpot(lead.combatSpotLeft[guid],
                                                    {attackOn->GetGUID(), kept->second.second});
                                lead.combatSpots[guid] = {attackOn->GetGUID(), spot};
                                lead.combatSpotAnchor[guid] = lead.fightAnchorSerial;
                                Decisions().Write(run, "leader", "combat_respot", guid, bot->GetName(), true,
                                                  JsonObject().Str("member", bot->GetName())
                                                      .Str("why", runningIn ? "out of its reach / sight of the "
                                                                              "tank its Attack target runs to"
                                                                            : "out of its reach / sight of its "
                                                                              "Attack target")
                                                      .Str("target", attackOn->GetName())
                                                      .Str("hereFails", MemberCastsFails(bot, attackOn, killOrder))
                                                      .Real("walk", bot->GetExactDist(spot)));
                            }
                        }
                    }
                }
                // the aggro-loss victim stops its damage until the tank has the mob back; the others leave the
                // loose mob alone
                // [lead18] a healer is never stopped: Stop is no offensive cast, its heals go on
                // [lead544] b212 rfc-h 5C 364.1-384.6 s: a Ragefire Shaman cast at the balance druid from 21 yd off
                // the tank ("the tank never had it"); the tank's walk to it was refused (an idle Trogg's reach), its
                // taunt did not reach - the druid stood stopped 20.5 s for a take-back that could not come. The stop
                // waits for the tank only while the tank can take the mob back: a pick-up ready (lead54's
                // tankCannotTake: no taunt / AoE threat off cooldown) with the mob in its taunt's reach and sight, or
                // the tank under the leader's walk to it
                bool tankTakesBack = false;
                if (loose != looseOn.end())
                {
                    float const takeReach = LeaderTauntReach(tank);
                    Leader::MemberEvents tankEvents;
                    tankTakesBack =
                        (!lead.tankCannotTake && takeReach > 0.0f && tank->GetExactDist(loose->second) <= takeReach &&
                         tank->IsWithinLOSInMap(loose->second)) ||
                        (Leader::Events::Get(tank->GetGUID(), tankEvents) &&
                         tankEvents.move == Leader::MoveEvent::MoveTo &&
                         tankEvents.moveWhy.rfind("aggro loss: the tank goes to", 0) == 0);
                }
                bool const looseVictim = loose != looseOn.end() && bot != healer && !PlayerbotAI::IsHeal(bot);
                bool const victim = looseVictim && tankTakesBack;
                // [lead52] the tank's threat lead before the damage: a damage dealer whose threat on its target
                // reaches the tank's stops until the tank leads again (the tank's lead is the game's own measure, no
                // number of ours); the start of a pull too - nobody out-threats a tank that has not hit yet
                // [lead53] only when no fight mob leaves it headroom (it was every member at the tank's threat)
                bool const overThreat = !looseVictim && bot != healer && !PlayerbotAI::IsHeal(bot) && target &&
                                        lead.noHeadroom[guid];
                // [lead80] a trash burn when it pays: the member's burn cooldown's effect fits in the fight's
                // seconds and the cooldown is back before the next boss (no boss left: it pays whenever it fits)
                // [lead178] user (approved): per cooldown - on trash only the ones back before the next boss pull
                // (their recharge within lead.bossPullSec: this fight, the on-path fights, the walk), the others held
                // for the boss; the member casts only the allowed ones (Leader::BurnAllows)
                // [lead274] bosses too: each cooldown by its verdict (held for the boss that needs it most)
                Leader::Intensity burnable = bot == healer ? intensity : Leader::Intensity::Normal;  // its tool uses
                std::vector<std::string> burnAllowed;
                std::string allowedNote;
                if (!victim && !overThreat && bot != healer && botAI)
                {
                    std::string heldNote;
                    burnVerdict(bot, botAI, burnAllowed, allowedNote, heldNote);
                    if (!burnAllowed.empty())
                        burnable = Leader::Intensity::Burn;
                    LogBurnDecision(run, lead, bot, "cooldowns", allowedNote, heldNote, fightBoss != nullptr);
                }
                Leader::Intensity const mine = victim || overThreat ? Leader::Intensity::Stop : burnable;
                // [lead722] b219 rfc-h: 791 Stop and 614 Burn events, no end of either - the back to Normal was not
                // logged, so fight_breakdown's "threat wait" (283 s of first-hit delay "waits: <mob> stands in ...
                // aggro reach") ran on to the member's next Intensity wherever the stop had ended before it. The
                // change back is logged too
                if (Leader::Events::SetIntensity(
                        guid, mine, mine == Leader::Intensity::Burn ? burnAllowed : std::vector<std::string>()))
                    logEvent(bot, "Intensity",
                             JsonObject()
                                 .Str("intensity", mine == Leader::Intensity::Stop   ? "stop"
                                                   : mine == Leader::Intensity::Burn ? "burn"
                                                                                     : "normal")
                                 .Str("why", mine == Leader::Intensity::Normal ? std::string("-")
                                             : victim ? "aggro loss: the tank takes the mob back"
                                             : overThreat && !lead.attackWaitWhy[guid].empty()  // [lead413]
                                                 ? "waits: " + lead.attackWaitWhy[guid]
                                             : overThreat ? std::string("threat: no mob with headroom")
                                                          : allowedNote)
                                 .Raw("threat", overThreat && lead.stopProjection.count(guid)
                                                    ? lead.stopProjection[guid]
                                                    : std::string("null")));  // [lead246]
                if (!looseVictim && !looseOn.empty() && bot != healer && !lead.looseIsTarget &&
                    Leader::Events::SetDontAttack(guid, looseOn.begin()->second->GetGUID()))
                    logEvent(bot, "DontAttack", JsonObject().Str("mob", looseOn.begin()->second->GetName())
                                                    .Str("why", "loose on a member until the tank has it"));
            }
        }

        // [lead104] the flee stop is ONE leader command per fleer (project rule: the leader decides, a member
        // executes): a fight mob at its flee-for-assist health or running gets one member with a stop ready (kept
        // while it can), else it is the damage dealers' focus (burst before it runs). The facts are the leader's
        // (Ai/Leader/Fight/PullFacts)
        if (fighting && Leader::LEADER_ATTACK_EVENTS)
        {
            std::vector<Player*> stoppers = seats;
            stoppers.push_back(tank);
            for (ObjectGuid const& fightMob : std::vector<ObjectGuid>(killOrder))
            {
                Unit* fleer = ObjectAccessor::GetUnit(*tank, fightMob);
                if (!Leader::Fight::NeedsFleeStop(fleer))
                {
                    lead.fleeStoppers.erase(fightMob);
                    continue;
                }
                Player* stopper = nullptr;
                if (auto const kept = lead.fleeStoppers.find(fightMob); kept != lead.fleeStoppers.end())
                    if (Player* keptBot = ObjectAccessor::FindPlayer(kept->second))
                        if (Leader::Fight::FleeStopper({keptBot}, fleer) == keptBot)
                            stopper = keptBot;
                if (!stopper)
                    stopper = Leader::Fight::FleeStopper(stoppers, fleer);
                if (stopper)
                {
                    lead.fleeStoppers[fightMob] = stopper->GetGUID();
                    if (Leader::Events::SetFleeStop(stopper->GetGUID(), fightMob))
                        logEvent(stopper, "FleeStop", JsonObject().Str("mob", fleer->GetName())
                                                          .Real("health", fleer->GetHealthPct()));
                }
                else
                {
                    lead.fleeStoppers.erase(fightMob);
                    killOrder.erase(std::remove(killOrder.begin(), killOrder.end(), fightMob), killOrder.end());
                    killOrder.insert(killOrder.begin(), fightMob);  // nobody can stop it: the focus bursts it
                    if (lead.fleeFocusLogged != fightMob)
                    {
                        lead.fleeFocusLogged = fightMob;
                        Decisions().Write(run, "leader", "flee_focus", fightMob, fleer->GetName(), true,
                                                      JsonObject().Real("health", fleer->GetHealthPct()));
                    }
                }
            }
        }

        // [lead426] user 2026-11-02 (rest / drink / food and Viper are the leader's, in a fight too): a led hunter's
        // aspect switch is the leader's External. In a fight ([lead440]): Viper below wowsims' 10 % line while its
        // need for the fight's seconds left (FightManaLine without the cooldowns and Viper it would cover) exceeds its
        // mana, its attack aspect (Dragonhawk, else Hawk) at wowsims' 30 % line; out of a fight ([lead450]): Viper below
        // full mana (no damage to cut there), its attack aspect at full. 0: no switch
        auto const viperSwitch = [&](Player* hunter, bool inFight, std::string& why) -> uint32
        {
            PlayerbotAI* hunterAI = GET_PLAYERBOT_AI(hunter);
            if (!hunterAI || hunter->getClass() != CLASS_HUNTER || !hunter->IsAlive() ||
                !hunter->HasSpell(RestState::SPELL_ASPECT_OF_THE_VIPER))
                return 0;
            AiObjectContext* hunterContext = hunterAI->GetAiObjectContext();
            uint32 attack = hunterContext->GetValue<uint32>("spell id", "aspect of the dragonhawk")->Get();
            if (!attack || !hunter->HasSpell(attack))
                attack = hunterContext->GetValue<uint32>("spell id", "aspect of the hawk")->Get();
            if (!attack || !hunter->HasSpell(attack))
                return 0;
            float const pct = hunter->GetPowerPct(POWER_MANA);
            bool const inViper = hunter->HasAura(RestState::SPELL_ASPECT_OF_THE_VIPER);
            bool wantViper = false;
            if (inFight)
            {
                // [lead440] b203 rfc-h 5H: the need line alone flipped Viper <-> Dragonhawk 288 times (Rthunad "Viper:
                // mana 50 % < 54 %" / "attack aspect: mana 48 % >= 47 %" a second apart): in Viper the mana rises
                // and the need for the seconds left falls, so they cross again at once - a GCD and half the damage
                // each time (MM 47 %, SV 34 % of wowsims). wowsims' lines in a fight (its hunter APL: Viper below
                // 10 %, the attack aspect again at 30 %) - the leader enters Viper only while the fight's need for
                // its seconds left also exceeds the mana (a hunter whose mana covers the rest keeps its damage) and
                // leaves it at the 30 % line alone, so no fact the switch itself changes flips it back
                if (inViper)
                {
                    wantViper = pct < RestState::VIPER_STOP_MANA_PCT;
                    why = Acore::StringFormat("{}: mana {:.0f} % {} wowsims' line {:.0f} %",
                                              wantViper ? "Viper" : "its attack aspect", pct, wantViper ? "<" : ">=",
                                              RestState::VIPER_STOP_MANA_PCT);
                }
                else
                {
                    if (lead.fightSecLeft <= 0.0f || pct >= RestState::VIPER_START_MANA_PCT)
                        return 0;
                    float const need =
                        RestState::FightManaLine(hunter, lead.fightSecLeft, 0.0f, nullptr, fightBoss != nullptr, false);
                    wantViper = pct < need;
                    why = Acore::StringFormat(
                        "{}: mana {:.0f} % < wowsims' line {:.0f} %, {} its need for the fight's {:.0f} s left "
                        "{:.0f} %",
                        wantViper ? "Viper" : "its attack aspect", pct, RestState::VIPER_START_MANA_PCT,
                        wantViper ? "<" : ">=", lead.fightSecLeft, need);
                }
            }
            else
            {
                // [lead450] b205/b206 rfc-h: the hunters left Viper on the walk at their rest line (OwnManaLine, which
                // counts Viper's mana in the next fight: 0-50 %) and so spent 176-337 s of their fights in Viper at
                // half damage (5H MM 337 of 969 s, 5F SV 181, 5E BM 213, 5A MM 176), while in Viper only 13-73 s of
                // the 300-430 s out of a fight. Out of a fight Viper cuts no damage: its regeneration is free until
                // the pool is full, the attack aspect goes on at full (or in the fight at wowsims' 30 % line above)
                wantViper = pct < 100.0f;
                why = Acore::StringFormat("{}: mana {:.0f} % out of a fight (Viper costs no damage there)",
                                          wantViper ? "Viper" : "its attack aspect: full", pct);
            }
            if (wantViper && !inViper)
                return RestState::SPELL_ASPECT_OF_THE_VIPER;
            // [lead691] a movement aspect the leader put it in (Cheetah / Pack, LeaderTickMoveSkills) ends like
            // Viper: the fight's or the walk's aspect by the same lines (the pull ordered, a fight, the walk over)
            if (!wantViper && (inViper || LeaderInMoveAspect(hunter)))
                return attack;
            return 0;
        };
        // [lead691] user 2026-11-03: every class's movement skills on the walks the leader orders (Blink, Sprint,
        // Shadowstep, Dash, Feral Charge - Cat, Ghost Wolf, Aspect of the Cheetah / Pack, Power Word: Shield with
        // Body and Soul, Disengage), told as Externals when they save walk seconds by game math and the member's
        // mana after it still covers its mana line for the fight now / next (LeaderTickMoveSkills); never kept for
        // a boss. The hunters it holds in a movement aspect on the walk are no Viper switch's out of a fight
        std::set<ObjectGuid> moveHeld;
        {
            LeaderMoveTick facts;
            facts.tank = tank;
            facts.members = seats;
            facts.fighting = fighting;
            facts.pullOrdered = lead.commit && lead.commitIssued;
            facts.fightSecLeft = lead.fightSecLeft;
            facts.bossFight = fightBoss != nullptr;
            facts.waitBoss = lead.bossWaitFor.IsEmpty() ? nullptr : map->GetCreature(lead.bossWaitFor);
            facts.hazardZones = &lead.hazardZones;
            facts.trailing = &lead.trailing;
            facts.routePath = &lead.routePath;
            facts.routePathIndex = lead.routePathIndex;
            // [lead905] the route plan's legs ahead (stop to stop to the boss, the navmesh walk): each begins after
            // the fight now, the walks before it at the tank's speed and each stop's fight (its mobs' health at the
            // run's measured trash rate)
            {
                GroupRouteState const& plan = group.Route();
                float const speed = std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
                float const trashRate = lead.trashFightSec > 0.0f && lead.trashKilledHealth > 0.0f
                                            ? lead.trashKilledHealth / lead.trashFightSec
                                            : 0.0f;
                float startSec = fighting ? lead.fightSecLeft : 0.0f;
                Position from = tank->GetPosition();
                auto const addLeg = [&](Position const& to)
                {
                    float const yd = Leader::WalkYd(tank, from, to);
                    facts.routeLegs.emplace_back(startSec, yd);
                    startSec += yd / speed;
                    from = to;
                };
                for (GroupRouteState::PlannedStop const& stop : plan.routePlan)
                {
                    addLeg(stop.spot);
                    if (trashRate > 0.0f)
                        for (ObjectGuid const& mobGuid : stop.mobs)
                            if (Creature* mob = map->GetCreature(mobGuid); mob && mob->IsAlive())
                                startSec += float(mob->GetHealth()) / trashRate;
                }
                if (Creature* boss = plan.routePlanBoss.IsEmpty() ? nullptr : map->GetCreature(plan.routePlanBoss);
                    boss && boss->IsAlive())
                    addLeg(boss->GetPosition());
            }
            std::vector<LeaderMoveTold> told;
            moveHeld = LeaderTickMoveSkills(facts, told);
            for (LeaderMoveTold const& one : told)
                logEvent(one.member, "External",
                         JsonObject().Num("spell", one.spell).Str("on", one.on).Str("why", one.why));
        }
        if (!fighting)
            for (Player* hunter : seats)
            {
                if (hunter->IsInCombat() || moveHeld.count(hunter->GetGUID()))
                    continue;
                std::string why;
                // [lead456] coordinator 2026-11-02 (Q5): once the leader issued the pull order the fight's rule
                // judges the aspect (out of Viper at wowsims' 30 % line), so the switch lands before the pull, not
                // as a GCD in the hunter's opener
                bool const pullOrdered = lead.commit && lead.commitIssued;
                if (uint32 const aspect = viperSwitch(hunter, pullOrdered, why))
                    if (Leader::Events::SetExternal(hunter->GetGUID(), aspect, ObjectGuid::Empty))
                        logEvent(hunter, "External", JsonObject().Num("spell", aspect).Str("on", hunter->GetName())
                                                         .Str("why", why));
            }

        // 3c. external saves and buffs on a target: the leader picks who and when, the member casts when told
        if (fighting && Leader::LEADER_ATTACK_EVENTS)
        {
            std::vector<Player*> everyone = seats;
            everyone.push_back(tank);
            std::set<ObjectGuid> told;
            auto const tell = [&](Player* caster, uint32 spell, Unit* on, char const* why)
            {
                if (!caster || !spell || told.count(caster->GetGUID()))
                    return false;
                told.insert(caster->GetGUID());
                // [lead412] a threat redirect: onto whom and the share its spell data moves
                if (SpellInfo const* info = on ? sSpellMgr->GetSpellInfo(spell) : nullptr)
                    for (SpellEffectInfo const& effect : info->GetEffects())
                        if (effect.Effect == SPELL_EFFECT_REDIRECT_THREAT)
                            lead.redirectTo[caster->GetGUID()] = {on->GetGUID(), effect.CalcValue(caster) / 100.0f};
                if (Leader::Events::SetExternal(caster->GetGUID(), spell, on ? on->GetGUID() : ObjectGuid::Empty,
                                                Leader::EXTERNAL_PRECEDENCE_FIGHT_SAVE))  // [lead320]
                    logEvent(caster, "External", JsonObject().Num("spell", spell)
                                                     .Str("on", on ? on->GetName() : "group").Str("why", why));
                return true;
            };
            auto const withSpell = [&](std::initializer_list<uint32> spells, Player* except)
            {
                for (Player* bot : seats)
                    if (bot != except && !told.count(bot->GetGUID()))
                        if (uint32 const spell = LeaderReadySpell(bot, spells))
                            return std::make_pair(bot, spell);
                return std::make_pair(static_cast<Player*>(nullptr), uint32(0));
            };
            uint32 hurt = 0;
            for (Player* bot : everyone)
                hurt += bot->GetHealthPct() < 50.0f ? 1 : 0;
            // [lead401] a battle resurrection: at once when the fight cannot go on without the dead member (the only
            // healer), else by the verdict the burn cooldowns get (its recharge against the next boss that needs it)
            if (Leader::Support::BattleRes res; Leader::Support::PickBattleRes(group, tank, seats, healer, told, res))
            {
                std::string why = res.why;
                bool allowed = res.needed;
                if (!allowed)
                    if (sSpellMgr->GetSpellInfo(res.spell))
                        allowed = cooldownVerdict(RechargeAfterCastSec(res.caster, res.spell), 0.0f,
                                                  why);  // [lead541]
                std::string const note = "battle resurrection of " + res.dead->GetName() + ": " + why;
                std::string& logged = lead.support.battleResLogged[res.dead->GetGUID()];
                if (logged != note)
                {
                    logged = note;
                    Decisions().Write(run, "leader", "battle_res", res.dead->GetGUID(), res.dead->GetName(), allowed,
                                      JsonObject().Str("dead", res.dead->GetName()).Str("caster", res.caster->GetName())
                                          .Num("spell", res.spell).Bool("allowed", allowed).Str("why", why));
                }
                if (allowed)
                    tell(res.caster, res.spell, res.dead, note.c_str());
            }
            // [lead197] b181 5E: Tricks of the Trade held 223 times "waiting for the leader's assignment" - the
            // leader told it only in the pull's opening window. Misdirection / Tricks put the caster's threat on the
            // tank (headroom for everyone): on the tank whenever one is ready in the fight
            // [lead233] b187 rfc-h 5H: Misdirection 2.6 times a minute on the boss (each a GCD and mana) while every
            // damage dealer had headroom. It is told when the threat needs it: a fight mob the tank has no threat on
            // yet (the pull's start), or a damage dealer without headroom (lead53 / lead229)
            bool threatNeeded = false;
            for (Player* bot : seats)
                if (auto const none = lead.noHeadroom.find(bot->GetGUID());
                    none != lead.noHeadroom.end() && none->second)
                    threatNeeded = true;
            for (ObjectGuid const& guid : killOrder)
                if (Unit* mob = ObjectAccessor::GetUnit(*tank, guid);
                    !threatNeeded && mob && mob->IsAlive() && mob->GetThreatMgr().GetThreat(tank) <= 0.0f)
                    threatNeeded = true;
            // [lead412] user 2026-11-02: b201 cast Misdirection / Tricks on 2 of 28 boss fights - on a boss whenever
            // one is ready (its threat is the tank's, the damage dealers keep their headroom); the pull's opening
            // (a mob without the tank's threat) and a member without headroom as before
            if (threatNeeded || fightBoss)
                if (auto const [bot, spell] = withSpell({SPELL_MISDIRECTION, SPELL_TRICKS_OF_THE_TRADE}, tank); bot)
                    tell(bot, spell, tank,
                         fightBoss ? "the threat onto the tank: a boss fight"
                                   : "the threat onto the tank: a mob without its threat or a member without "
                                     "headroom");
            if (tank->GetHealthPct() < LEADER_SAVE_HP_PCT)
            {
                // [lead300] Guardian Spirit / Lay on Hands are the leader's too (the members' own are off)
                if (auto const [bot, spell] = withSpell({SPELL_PAIN_SUPPRESSION, SPELL_HAND_OF_SACRIFICE}, tank); bot)
                    tell(bot, spell, tank, "the tank is low");
                else if (auto const [priest, spirit] = withSpell({SPELL_GUARDIAN_SPIRIT}, tank); priest)
                    tell(priest, spirit, tank, "the tank is low");
                else if (auto const [paladin, hands] =
                             withSpell({SPELL_LAY_ON_HANDS_1, SPELL_LAY_ON_HANDS_2, SPELL_LAY_ON_HANDS_3,
                                        SPELL_LAY_ON_HANDS_4, SPELL_LAY_ON_HANDS_5},
                                       tank);
                         paladin)
                    tell(paladin, hands, tank, "the tank is low");
            }
            // [lead912] user 2026-11-03 (46): a led bear tank's Survival Instincts and Frenzied Regeneration are the
            // leader's (its own triggers are off, rot371): told as its health drops - under the danger line
            // (LEADER_DANGER_HEALTH_PCT) and either under the tank's save line (LEADER_PRESAVE_TANK_PCT) or taking
            // more per second (DamageTaken) than the healers' throughput on it (HealerThroughput) - Survival
            // Instincts first, then Frenzied Regeneration, held by the cooldown verdict (for the boss that needs it
            // unless critical). The order ends once its aura is up or the bear reports it cannot cast it
            {
                constexpr uint32 SPELL_SURVIVAL_INSTINCTS = 61336;
                constexpr uint32 SPELL_FRENZIED_REGENERATION = 22842;
                bool const bearSave = tank->getClass() == CLASS_DRUID && tank->IsAlive();
                Leader::MemberEvents bearOrder;
                if (bearSave && Leader::Events::Get(tank->GetGUID(), bearOrder) &&
                    (bearOrder.externalSpell == SPELL_SURVIVAL_INSTINCTS ||
                     bearOrder.externalSpell == SPELL_FRENZIED_REGENERATION))
                {
                    std::string failed;
                    bool const reported =
                        Leader::Events::TakeExternalFailed(tank->GetGUID(), bearOrder.externalSpell, failed);
                    if (reported || tank->HasAura(bearOrder.externalSpell))
                    {
                        Leader::Events::SetExternal(tank->GetGUID(), 0, ObjectGuid::Empty);
                        LOG_INFO("playerbots", "[lead912] {}: its {} order ends ({})", tank->GetName(),
                                 bearOrder.externalSpell,
                                 reported ? "it reports: " + failed : std::string("its aura is up"));
                    }
                }
                if (bearSave && tank->GetHealthPct() < LEADER_DANGER_HEALTH_PCT &&
                    !tank->HasAura(SPELL_SURVIVAL_INSTINCTS) && !tank->HasAura(SPELL_FRENZIED_REGENERATION))
                {
                    float const taken = Leader::Support::MemberTakenPerSec(group, tank);
                    float hps = 0.0f;
                    for (Player* bot : everyone)
                        if (bot->IsAlive() && PlayerbotAI::IsHeal(bot))
                            hps += RestState::HealerThroughput(bot, tank).hps;
                    bool const low = tank->GetHealthPct() < Leader::LEADER_PRESAVE_TANK_PCT;
                    if (low || taken > hps)
                    {
                        uint32 spell = 0;
                        for (uint32 save : {SPELL_SURVIVAL_INSTINCTS, SPELL_FRENZIED_REGENERATION})
                            if (!spell && tank->HasSpell(save) && !tank->HasSpellCooldown(save))
                                spell = save;
                        std::string why;
                        if (spell && cooldownVerdict(RechargeAfterCastSec(tank, spell), 0.0f, why))
                        {
                            std::string const note = Acore::StringFormat(
                                "the bear's health drops ({:.0f} %, taken {:.0f}/s against heals {:.0f}/s): {}",
                                tank->GetHealthPct(), taken, hps, why);
                            if (tell(tank, spell, tank, note.c_str()))
                            {
                                thread_local std::set<std::pair<uint64, uint32>> bearLogged;
                                if (bearLogged.insert({tank->GetGUID().GetRawValue(), spell}).second)
                                    LOG_INFO("playerbots", "[lead912] {}: {} told, {}", tank->GetName(), spell, note);
                            }
                        }
                    }
                }
            }
            // [lead925] user 2026-11-03 (q50): a led tank's gap closer whose landing no heal reaches in time
            // (Fight::TankGapCloserRisk) waits for its defensive (Survival Instincts / Barkskin, Shield Wall / Last
            // Stand): the leader tells it while the risk still names one, held by the cooldown verdict (no save: no
            // charge). The order ends once its aura is up or the tank reports it cannot cast it
            if (tank->IsAlive())
            {
                Leader::MemberEvents saveOrder;
                if (Leader::Events::Get(tank->GetGUID(), saveOrder) &&
                    Leader::Fight::IsGapCloserSave(saveOrder.externalSpell))
                {
                    std::string failed;
                    bool const reported =
                        Leader::Events::TakeExternalFailed(tank->GetGUID(), saveOrder.externalSpell, failed);
                    if (reported || Leader::Fight::GapCloserSaveUp(tank, saveOrder.externalSpell))
                        Leader::Events::SetExternal(tank->GetGUID(), 0, ObjectGuid::Empty);
                }
                ObjectGuid asked;
                if (Leader::Fight::GapCloserSaveAsked(tank, asked))
                {
                    Unit* mob = ObjectAccessor::GetUnit(*tank, asked);
                    uint32 save = 0;
                    std::string const risk =
                        mob && mob->IsAlive() ? Leader::Fight::TankGapCloserRisk(tank, mob, &save) : std::string();
                    std::string why;
                    if (risk.empty() || !save)
                        Leader::Fight::ClearGapCloserSave(tank);
                    else if (cooldownVerdict(RechargeAfterCastSec(tank, save), 0.0f, why))
                    {
                        std::string const note = "its gap closer onto " + mob->GetName() + ": " + risk + "; " + why;
                        if (tell(tank, save, tank, note.c_str()))
                        {
                            thread_local std::set<std::pair<uint64, uint64>> gapLogged;
                            if (gapLogged.insert({tank->GetGUID().GetRawValue(), asked.GetRawValue()}).second)
                                LOG_INFO("playerbots", "[lead925] {}: {} told, {}", tank->GetName(), save, note);
                        }
                    }
                }
            }
            for (Player* member : seats)
                if (member != healer && !PlayerbotAI::IsTank(member) && !member->getAttackers().empty() &&
                    member->GetHealthPct() < LEADER_SAVE_HP_PCT)
                    if (auto const [bot, spell] = withSpell({SPELL_HAND_OF_PROTECTION_1, SPELL_HAND_OF_PROTECTION_2,
                                                             SPELL_HAND_OF_PROTECTION_3},
                                                            member);
                        bot)
                        tell(bot, spell, member, "a member is attacked and low");
            // [lead416] b201 rfc-h 5C / 5B: the balance druid and the elemental shaman ran to 0-5 % mana in boss
            // fights and got no Innervate (told only for a healer under LEADER_SAVE_MANA_PCT).
            // [lead423] every mana tool is the leader's, by one test (user 2026-11-02: rest / mana tools are the
            // leader's, every double a bug): a mana user whose need for the fight's seconds left (FightManaLine without
            // the cooldowns it would cover, its measured rate) exceeds its mana gains fight seconds from a tool - the
            // mana the tool returns to it (RestState::ManaToolPct, spell / game data), at most its shortfall and the
            // room in its pool, over its spend rate. Innervate (any member), Shadowfiend / Evocation / Dispersion /
            // a healer's Divine Plea (the caster itself), Hymn of Hope (the members lowest on mana, as many as it
            // reaches), Mana Tide Totem (every member): the tool and receiver with the most fight seconds gained is
            // told, held by the cooldown verdict like the battle resurrection
            struct ManaUser
            {
                Player* who = nullptr;
                float pct = 0.0f;
                float need = 0.0f;
                float ratePct = 0.0f;  // % of its pool per fight second
            };
            // [lead425] a member a tool's return is still coming to is no receiver (one need, one tool): under
            // Innervate, Hymn of Hope's tick, Evocation, Dispersion's mana, Divine Plea; a priest whose Shadowfiend is
            // out, a member in reach of a Mana Tide Totem that is down (the caster's cooldown started within its
            // summon's duration: the core's cooldown, spell data)
            auto const receiving = [&](Player* member) { return ManaToolReturning(member, everyone, true); };
            // [lead751] a member another told tool's return is still to come to: the leader's command not yet cast
            // (b220 rfc-h 5C Jergosh 998.1 s: Evocation told to the fire mage, 999.0 s Innervate on the mage too - its
            // channel had not begun, so it read as short again - while the balance druid itself stood at 13 % and
            // dealt 1200 dps). A tool on the caster itself (Evocation, Dispersion, Divine Plea, Shadowfiend) returns
            // to its caster, Innervate to the member it is told on. The member stays a mana user (its own told tool
            // is asserted on: the command lives while each tick asserts it); only `caster`'s Innervate skips it
            auto const otherToolComes = [&](Player* member, Player* innervator)
            {
                for (Player* caster : everyone)
                {
                    Leader::MemberEvents told;
                    if (!caster->IsAlive() || !Leader::Events::Get(caster->GetGUID(), told) || !told.externalSpell)
                        continue;
                    uint32 const spell = told.externalSpell;
                    if (spell == SPELL_INNERVATE && told.externalTarget == member->GetGUID() && caster != innervator)
                        return true;
                    if (caster == member &&
                        (spell == RestState::SPELL_EVOCATION || spell == RestState::SPELL_DISPERSION ||
                         spell == RestState::SPELL_DIVINE_PLEA || spell == RestState::SPELL_SHADOWFIEND))
                        return true;
                }
                return false;
            };
            std::vector<ManaUser> manaUsers;
            if (lead.fightSecLeft > 0.0f)
                for (Player* member : everyone)
                    if (member->IsAlive() && member->getPowerType() == POWER_MANA &&
                        member->GetMaxPower(POWER_MANA) && !receiving(member))
                    {
                        ManaUser user;
                        user.who = member;
                        user.pct = member->GetPowerPct(POWER_MANA);
                        user.need = RestState::FightManaLine(member, lead.fightSecLeft, 0.0f, nullptr,
                                                             fightBoss != nullptr, false);
                        user.ratePct = user.need / lead.fightSecLeft;
                        if (user.need > user.pct && user.ratePct > 0.0f)
                            manaUsers.push_back(user);
                    }
            auto const gainedSec = [](ManaUser const& user, float gainPct, float durationSec)
            {
                float const covered =
                    std::min({gainPct, user.need - user.pct, 100.0f - user.pct + user.ratePct * durationSec});
                return covered > 0.0f ? covered / user.ratePct : 0.0f;
            };
            struct ManaTool
            {
                Player* caster = nullptr;
                uint32 spell = 0;
                Unit* on = nullptr;  // nullptr: the caster itself / its group
                float sec = 0.0f;
                float damage = 0.0f;  // [lead761] the fight damage its seconds gained are worth
                std::string why;
            };
            // [lead761] user 2026-11-03: a mana tool goes where it gains the most damage - each receiver's fight
            // seconds gained times that member's own measured dps (GroupDamage::MemberSustainedDps), not the seconds
            // alone (a healer's seconds keep the whole group's damage going: the group's sustained dps). A member with
            // no measured damage yet counts the group's per-member average
            float const groupDamage = GroupDamage::SustainedDps(tank);
            float const memberAverage = groupDamage / float(std::max<size_t>(everyone.size(), 1));
            auto const damageWeight = [&](Player* member) -> float
            {
                if (PlayerbotAI::IsHeal(member))
                    return groupDamage > 0.0f ? groupDamage : 1.0f;
                float const own = GroupDamage::MemberSustainedDps(member);
                return own > 0.0f ? own : (memberAverage > 0.0f ? memberAverage : 1.0f);
            };
            // [lead440] b203 rfc-h: the burn verdict held every mana tool for a boss "that needs it more" (overload)
            // - 5H's fire mage came to Oggleflint at 3 % mana, Innervate / Hymn "held for Taragaman (his pull in
            // 114 s)", its Evocation "held for Jergosh" on trash 76 times and never cast (30.6 % of wowsims), the
            // members ran at 0-5 % mana most of the run: their rest lines count Evocation / Shadowfiend / Divine
            // Plea's mana in the fight (FightManaLine's credits) and the fight then held them. A mana tool's later
            // use is mana, not a boss's overload: before a later boss the members rest to their line for him, so a
            // tool is kept for him only when a member it reaches would still be short there from a full pool (his
            // fight's need without the tools, FightManaLine credits = false, over 100 %) and it would not be back for
            // his pull (secToBoss within its recharge); otherwise it gives its fight seconds now
            // [lead651] user 2026-11-02 (cooldowns-boss-judged-like-pulls): long cooldowns are kept for the bosses -
            // on trash only at a critical moment or when back before the next boss pull. b218 rfc-h 5C 1039.9 s:
            // Innervate on trash to Rtmagab ("10 % < 12 % for the fight's 12 s left - 2 fight s gained; no later boss
            // within its recharge needs it beyond a full pool"), Bazzalan pulled 36 s later with it on its 180 s
            // recharge. On trash a mana tool has the burn cooldowns' verdict (cooldownVerdict: the critical moment, or
            // back before the pull of the next boss); in a boss fight the later boss's need from a full pool as above
            auto const manaToolVerdict = [&](Player* caster, bool self, float rechargeSec, float effectSec,
                                             std::string& why) -> bool
            {
                if (!critical.empty())
                {
                    why = critical;
                    return true;
                }
                if (!fightBoss)
                    return cooldownVerdict(rechargeSec, effectSec, why);
                for (auto const& [bossGuid, need] : lead.bossNeeds)
                {
                    Creature* later =
                        fightBoss && bossGuid == fightBoss->GetGUID() ? nullptr : map->GetCreature(bossGuid);
                    if (!later || !later->IsAlive() || later->IsInCombat() || need.fightSec <= 0.0f)
                        continue;
                    float const toHim = secToBoss(later);
                    if (toHim >= rechargeSec)
                        continue;
                    for (Player* member : everyone)
                    {
                        if ((self && member != caster) || !member->IsAlive() ||
                            member->getPowerType() != POWER_MANA || !member->GetMaxPower(POWER_MANA))
                            continue;
                        float const there =
                            RestState::FightManaLine(member, need.fightSec, 0.0f, nullptr, true, false);
                        if (there > 100.0f)
                        {
                            why = Acore::StringFormat(
                                "held for {}: {} needs {:.0f} % there from a full pool ({:.0f} s; his pull in {:.0f} "
                                "s, recharge {:.0f} s)",
                                need.name, member->GetName(), there, need.fightSec, toHim, rechargeSec);
                            return false;
                        }
                    }
                }
                why = "no later boss within its recharge needs it beyond a full pool";
                return true;
            };
            ManaTool best;
            ManaTool heldTool;  // [lead440] the most fight seconds of a held tool (logged when none is allowed)
            std::string bestVerdict;
            std::string heldVerdict;
            auto const consider = [&](Player* caster, uint32 spell, Unit* on, std::vector<ManaUser const*> const& to,
                                      bool self)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
                if (!info || to.empty())
                    return;
                // [lead461] b207 rfc-h 5H 911-1073 s: the cat druid's Innervate was the pick (the most fight seconds)
                // in 134 verdicts and never cast - the game refuses it in Cat Form (its isPossible false 30 times) -
                // so the mage's ready Evocation was never told and Jergosh ran with the mage at 0-3 % mana (485 dps).
                // A tool its caster's form forbids (the spell's own shapeshift check) is no tool now
                if (info->CheckShapeshift(caster->GetShapeshiftForm()) != SPELL_CAST_OK)
                    return;
                float const durationSec = std::max(info->GetMaxDuration(), 0) / 1000.0f;
                float sec = 0.0f;
                float damage = 0.0f;
                std::string names;
                for (ManaUser const* user : to)
                {
                    // [lead733] b219 rfc-h 5C Taragaman 701.1 s: Innervate on the balance druid at 98 % mana ("2 fight
                    // s gained") - its return did not fit the pool. A return the pool's room cuts, in a fight that
                    // lasts until the receiver has spent room for all of it, gains more later (wowsims: Innervate as
                    // late as possible, at the bottom of the pool): no tool for this receiver now
                    float const gainPct = RestState::ManaToolPct(caster, user->who, spell);
                    float const room = 100.0f - user->pct + user->ratePct * durationSec;
                    if (gainPct > room && lead.fightSecLeft > (gainPct - room) / user->ratePct + durationSec)
                        continue;
                    if (float const gained = gainedSec(*user, gainPct, durationSec); gained > 0.0f)
                    {
                        sec += gained;
                        damage += gained * damageWeight(user->who);
                        names += Acore::StringFormat("{}{} {:.0f} % < {:.0f} %", names.empty() ? "" : ", ",
                                                     user->who->GetName(), user->pct, user->need);
                    }
                }
                // [lead733] b219 rfc-h 5C 93.5 / 110.0 / 502.9 s: Innervate on trash for "1 / 3 / 4 fight s gained" -
                // the cast itself takes the caster's global cooldown (a channel: its whole duration) out of the fight,
                // as FightManaLine's credit weighs it (lead177). A tool gains only the fight seconds beyond its own
                // cast; one that gains no more than that is no tool now (it stays for a fight it gains in)
                float const castSec =
                    info->IsChanneled()
                        ? std::max(info->GetDuration(), 0) / 1000.0f
                        : std::max<uint32>(info->StartRecoveryTime, info->CalcCastTime(caster)) / 1000.0f;
                // [lead761] ... in damage: the caster's own damage over its cast against the receivers' gained damage
                if (sec <= castSec || damage <= castSec * damageWeight(caster))
                    return;
                // [lead440] the most fight seconds among the tools the verdict allows: a held tool no longer keeps
                // another one out (5H: Innervate / Hymn held for Taragaman kept the mage's Evocation from the test)
                // [lead761] the most damage gained
                if (damage <= 0.0f || (damage <= best.damage && damage <= heldTool.damage))
                    return;
                std::string verdictWhy;
                bool allowed = manaToolVerdict(caster, self, RechargeAfterCastSec(caster, spell), durationSec,
                                               verdictWhy);  // [lead541] [lead651]
                // [lead931] b226 rfc-h: 9 of the 98 healer "cast in flight" aggro losses came 0.2-3.3 s after
                // the leader's Innervate (5A-9 132.0 s on Rthunaa, the restoration druid Rtdruaa at 186 threat on a
                // Ragefire Trogg against the bear's 83 at 134.7 s): each Innervate tick forwards half the mana it gives
                // to the druid over every mob that has the receiver on its threat list (AuraEffect::
                // HandlePeriodicEnergizeAuraTick -> ForwardThreatForAssistingMe). The potion's / gem's rule (lead884):
                // the Innervate waits while that share takes the druid past the tank's threat on such a mob
                if (Player* receiver = on ? on->ToPlayer() : nullptr; allowed && spell == SPELL_INNERVATE && receiver)
                {
                    uint32 const mana = uint32(RestState::ManaToolPct(caster, receiver, spell) / 100.0f *
                                               float(receiver->GetMaxPower(POWER_MANA)));
                    if (std::string const overtakes =
                            Leader::Support::EnergizeSplitOvertakes(receiver, caster, tank, mana);
                        !overtakes.empty())
                    {
                        allowed = false;
                        verdictWhy = "waits: " + overtakes + " [lead931]";
                        thread_local std::map<uint64, std::string> waitLogged;
                        std::string const key = overtakes.substr(0, overtakes.find(" ("));
                        if (std::string& logged = waitLogged[caster->GetGUID().GetRawValue()]; logged != key)
                        {
                            logged = key;
                            LOG_INFO("playerbots", "[lead931] {}: Innervate on {} waits: {}", caster->GetName(),
                                     receiver->GetName(), overtakes);
                        }
                    }
                }
                ManaTool& slot = allowed ? best : heldTool;
                if (damage > slot.damage)
                {
                    slot = {caster, spell, on, sec, damage,
                            Acore::StringFormat("{}: {} for the fight's {:.0f} s left - {:.0f} fight s gained, {:.0f} "
                                                "damage",
                                                info->SpellName[0], names, lead.fightSecLeft, sec, damage)};
                    (allowed ? bestVerdict : heldVerdict) = verdictWhy;
                }
            };
            if (!manaUsers.empty())
            {
                for (Player* caster : seats)
                {
                    // [lead610] b217 rfc-h 5H Taragaman 708-715 s: the fire mage's Evocation was ready (cast 468.1 s,
                    // recharge 240 s), the mage at 1-3 % short of the fight's need, and no mana_tool verdict came - a
                    // caster casting anything was skipped, and dry it cast Frostbolt / wanded (Shoot's autorepeat
                    // counts as a cast) on every tick; it got Evocation at 721.8 s on the next trash. A generic cast
                    // ends by itself (the member casts the tool after it, LeaderExternalAction) and the wand holds
                    // nothing: only a channel in progress is no moment to tell (the tool's cast would break it)
                    if (!caster->IsAlive() || told.count(caster->GetGUID()) ||
                        caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                        continue;
                    ManaUser const* self = nullptr;
                    for (ManaUser const& user : manaUsers)
                        if (user.who == caster)
                            self = &user;
                    // [lead751] a druid short of its own need keeps its Innervate: its boss line counted it on
                    // itself (lead733, FightManaLine's credit), so the group came to the boss with the druid that low
                    // (b220 5C Jergosh: the druid at 13 % gave it to the mage, then cast 7 spells in 66 s)
                    if (uint32 const spell = LeaderReadySpell(caster, {SPELL_INNERVATE}))
                        for (ManaUser const& user : manaUsers)
                            if ((!self || user.who == caster) && !otherToolComes(user.who, caster))
                                consider(caster, spell, user.who, {&user}, false);
                    if (self)
                    {
                        // [lead423] Shadowfiend attacks the fight's target; the others are on the caster itself
                        if (uint32 const spell = LeaderReadySpell(caster, {RestState::SPELL_SHADOWFIEND});
                            spell && target)
                            consider(caster, spell, target, {self}, true);
                        // [lead633] b218 rfc-h 5D Oggleflint 282.7 s: the arcane mage was told Evocation at 44 %
                        // ("44 % < 50 % for the fight's 30 s left - 3 fight s gained"), an 8 s channel for 3 s. The
                        // wowsims arcane / frost APL channels it at currentManaPercent <= 25 % (the mage's own trigger
                        // before the leader took the tool, b31_deep "mana at or below 25", every mage spec)
                        using RestState::WOWSIMS_EVOCATION_MANA_PCT;  // [lead850] one line, the boss line too
                        // [lead752] one decision per fact: a told Evocation stays told until it is cast (b220 rfc-h
                        // 5C Jergosh 997.2 s: told at 7 %, the mage's mana gem took it to 27 % in the same second, the
                        // order dropped above the 25 % line and the mage never channelled it in the fight)
                        Leader::MemberEvents pending;
                        bool const evocationTold = Leader::Events::Get(caster->GetGUID(), pending) &&
                                                   pending.externalSpell == RestState::SPELL_EVOCATION;
                        if (caster->GetPowerPct(POWER_MANA) <= WOWSIMS_EVOCATION_MANA_PCT || evocationTold)
                            if (uint32 const spell = LeaderReadySpell(caster, {RestState::SPELL_EVOCATION}))
                                consider(caster, spell, nullptr, {self}, true);
                        if (uint32 const spell = LeaderReadySpell(caster, {RestState::SPELL_DISPERSION}))
                            consider(caster, spell, nullptr, {self}, true);
                        if (PlayerbotAI::IsHeal(caster) && !caster->HasAura(RestState::SPELL_DIVINE_PLEA))
                            if (uint32 const spell = LeaderReadySpell(caster, {RestState::SPELL_DIVINE_PLEA}))
                                consider(caster, spell, nullptr, {self}, true);
                    }
                    if (uint32 const spell = LeaderReadySpell(caster, {SPELL_HYMN_OF_HOPE}))
                    {
                        // the members lowest on mana, as many as its energize reaches (spell data)
                        // [lead425] the core's pick (spell_pri_hymn_of_hope): the 3 group members lowest on mana -
                        // of them, the ones short of their need gain
                        std::vector<Player*> pool;
                        for (Player* member : everyone)
                            if (member->IsAlive() && member->getPowerType() == POWER_MANA &&
                                member->GetMaxPower(POWER_MANA))
                                pool.push_back(member);
                        std::sort(pool.begin(), pool.end(), [](Player* a, Player* b)
                                  { return a->GetPowerPct(POWER_MANA) < b->GetPowerPct(POWER_MANA); });
                        if (pool.size() > RestState::HYMN_OF_HOPE_TARGETS)
                            pool.resize(RestState::HYMN_OF_HOPE_TARGETS);
                        std::vector<ManaUser const*> lowest;
                        for (ManaUser const& user : manaUsers)
                            if (std::find(pool.begin(), pool.end(), user.who) != pool.end())
                                lowest.push_back(&user);
                        consider(caster, spell, nullptr, lowest, false);
                    }
                    if (uint32 const spell = LeaderReadySpell(caster, {SPELL_MANA_TIDE_TOTEM}))
                    {
                        // [lead425] the members within its tick's area round the shaman (the totem drops there)
                        float const radius = RestState::ManaToolRadius(caster, spell);
                        std::vector<ManaUser const*> inReach;
                        for (ManaUser const& user : manaUsers)
                            if (radius <= 0.0f || user.who->GetExactDist(caster) <= radius)
                                inReach.push_back(&user);
                        consider(caster, spell, nullptr, inReach, false);
                    }
                }
            }
            // [lead426] the hunters' aspect switch by their fight need (Viper / the attack aspect)
            for (Player* hunter : seats)
            {
                std::string why;
                if (uint32 const aspect = viperSwitch(hunter, true, why))
                    tell(hunter, aspect, nullptr, why.c_str());
            }
            if (ManaTool const& pick = best.caster ? best : heldTool; pick.caster)
            {
                bool const allowed = best.caster != nullptr;
                std::string const note = pick.why + "; " + (allowed ? bestVerdict : heldVerdict);
                if (note != lead.innervateLogged)
                {
                    lead.innervateLogged = note;
                    Decisions().Write(run, "leader", "mana_tool", pick.caster->GetGUID(), pick.caster->GetName(),
                                      allowed, JsonObject().Str("caster", pick.caster->GetName())
                                                   .Num("spell", pick.spell).Bool("allowed", allowed)
                                                   .Str("why", note));
                }
                if (allowed)
                    tell(best.caster, best.spell, best.on, best.why.c_str());
            }
            if (hurt >= 3)
                if (auto const [bot, spell] = withSpell({SPELL_DIVINE_HYMN}, nullptr); bot)
                    tell(bot, spell, nullptr, "three members under half health");
            // [lead105] Heroism / Bloodlust is the group's cooldown: the leader's command (one shaman, the group as
            // the target); the shaman's own trigger is off in a led group
            // [lead274] by the verdict of every burn cooldown: on the boss that needs it most, on trash only at a
            // critical moment or when it is back before the next boss pull; never under Sated / Exhaustion
            constexpr uint32 SPELL_SATED = 57724;
            constexpr uint32 SPELL_EXHAUSTION = 57723;
            if (target && !tank->HasAura(SPELL_SATED) && !tank->HasAura(SPELL_EXHAUSTION))
                if (auto const [bot, spell] = withSpell({SPELL_HEROISM, SPELL_BLOODLUST}, nullptr); bot)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                    {
                        std::string why;
                        // [lead541] the next one only after the group's Sated / Exhaustion ends (spell data)
                        float rechargeSec = RechargeAfterCastSec(bot, spell);
                        if (SpellInfo const* debuff =
                                sSpellMgr->GetSpellInfo(spell == SPELL_HEROISM ? SPELL_EXHAUSTION : SPELL_SATED))
                            rechargeSec = std::max(rechargeSec, debuff->GetMaxDuration() / 1000.0f);
                        bool const allowed =
                            cooldownVerdict(rechargeSec, std::max(info->GetMaxDuration(), 0) / 1000.0f, why);
                        std::string const note = "heroism / bloodlust: " + why;
                        LogBurnDecision(run, lead, bot, "heroism", allowed ? note : std::string(),
                                        allowed ? std::string() : note, fightBoss != nullptr);
                        if (allowed)
                            tell(bot, spell, nullptr, "the burn: the group's haste");
                    }
            if (intensity == Leader::Intensity::Burn)
                for (Player* member : seats)
                    if (member != healer && member->getPowerType() == POWER_MANA && !PlayerbotAI::IsTank(member))
                    {
                        if (auto const [bot, spell] = withSpell({SPELL_POWER_INFUSION}, member); bot)
                            tell(bot, spell, member, "the burn");
                        break;
                    }
        }

        // [lead17] the tank out of a hazard (its own "move fight off hazard" is off): a short step, in heal reach
        // [lead56] in a fight the tank's own (LEADER_TANK_OWNS_FIGHT)
        if (Leader::LEADER_HAZARD_SPOTS && tank->IsAlive() && !(Leader::LEADER_TANK_OWNS_FIGHT && tank->IsInCombat()))
        {
            std::vector<HazardZone> const& zones = lead.hazardZones[tank->GetGUID()];
            if (HazardZone const* in = zones.empty() ? nullptr
                                                     : LeaderZoneAt(zones, tank->GetPositionX(), tank->GetPositionY(),
                                                                    tank->GetPositionZ(), 0.5f))
            {
                hazardSeen = true;
                Position spot;
                if (slow([&] { return LeaderSafeSpot(tank, GET_PLAYERBOT_AI(tank), zones, keepsHealer, spot); }) &&
                    checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, spot, 1.0f,
                                            std::string("hazard: ") + in->what))
                {
                    logEvent(tank, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", std::string("hazard: ") + in->what).Str("who", "tank")
                                 .Str("verdict", lead.hazardVerdictWhy[tank->GetGUID()])
                                 .Real("x", spot.GetPositionX()).Real("y", spot.GetPositionY())
                                 .Real("z", spot.GetPositionZ()));
                    LogFightDrag(run, tank, spot, lead.meetPoint, "hazard");
                }
            }
        }

        // [lead271] a hunter on its trap spot drops the trap (after the externals above: its one external slot)
        Leader::TrapWeave::TickDrop(group, seats, fighting, logEvent);

        // 4. the heal reach in the fight: a member out of the healer's reach or sight moves the healer (a spot that
        // reaches everyone), else it is reported
        if (Leader::LEADER_HEAL_REACH && healer && fighting &&
            (!lead.healReachMs || getMSTimeDiff(lead.healReachMs, now) >= 0))
        {
            lead.healReachMs = now;
            // [lead950] b227 5C-11 95-115 s (the wipe): the blood DK stood under 7 attackers, the priest 30.5 yd off
            // on its "heal reach" spot without sight of it (losTank false, the bot's own cast test), cast nothing and
            // the tank died at 115 s - with no heal_reach record in those 20 s: the rule judged the tank's fight
            // anchor (up to LEADER_ANCHOR_MOVE_YD off the tank) by its own sight model and found it reached, so no
            // re-spot, no lead909 tank walk and no lead830 join ever ran. b227 (16 runs): 373 of the 666 s a healer
            // was out of reach / sight of the tank under attack had no heal_reach verdict (231 s both standing). The
            // tank under attack is judged by the game's own test where it stands: in the heal's reach
            // (Unit::IsWithinCombatRange, Spell::CheckRange) and the healer's sight (IsWithinLOSInMap); while it
            // stands there that is where it fights, and a healer standing still where that test fails is no spot
            // that keeps it (its own spot included)
            bool const tankUnderAttack = tank->IsAlive() && !tank->getAttackers().empty();
            bool const healsTankNow = healer->IsWithinCombatRange(tank, Leader::LEADER_HEAL_REACH_YD) &&
                                      healer->IsWithinLOSInMap(tank);
            bool const tankUnhealed = tankUnderAttack && !healsTankNow;
            bool const tankStands = tankUnhealed && !tank->isMoving();
            std::optional<Position> const failsHere =
                tankStands && !healer->isMoving() ? std::optional<Position>(healer->GetPosition()) : std::nullopt;
            auto const provenOut = [&failsHere](Position const& spot)
            { return failsHere && spot.GetExactDist(*failsHere) <= 2.0f; };
            // [lead980] b228 rfc-h 5D-4 313-337 s: the bear had no attacker (it never got the pack the mage woke on
            // the level below), the mage 80 yd off took the hits; every heal_reach fix walked the shaman "back into
            // reach of the tank" (the spots kept the tank and every assigned spot, impossible 80 yd apart) - away
            // from the mage, 82 yd off at its death. While nothing attacks the tank, the members who are attacked
            // are where the heals go: the reach anchors on the attacked member at the lowest health
            Player* rescue = nullptr;
            if (!tankUnderAttack)
                for (Player* other : seats)
                    if (other != healer && other != tank && other->IsAlive() && !other->getAttackers().empty() &&
                        (!rescue || other->GetHealthPct() < rescue->GetHealthPct()))
                        rescue = other;
            Position const tankAt = rescue      ? rescue->GetPosition()
                                    : tankStands ? tank->GetPosition()
                                                 : tankFightAt();  // [lead120]
            // [lead121] b155 rfc-h 20261101-103820 610.9-616.0 s: A -> B -> A heal-reach spots (command_repeat), each
            // time a member walking in drifted out of reach for a moment. The heal reach judges the members where the
            // leader put them (their MoveTo / StandAt spot), never a member on its way; one decision per fact: the
            // tank's fight anchor and the members' assigned spots
            auto const assignedAt = [](Player* bot) -> std::optional<Position>
            {
                Leader::MemberEvents events;
                if (Leader::Events::Get(bot->GetGUID(), events) &&
                    (events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt))
                    return events.spot;
                return std::nullopt;
            };
            std::map<ObjectGuid, std::optional<Position>> assigned;
            for (Player* other : seats)
                if (other != healer)
                    assigned[other->GetGUID()] = assignedAt(other);
            // [lead38] b87 rfc-h 511-596 s: 19 + 19 "out of the healer's reach" with the members 0.0 yd from the
            // healer - the healer's planned spot was judged, not where it stands: out only when neither reaches
            std::vector<Player*> out;
            auto const outOfReach = [&](Position const& at)
            { return !reaches(healerAt, at) && !reaches(healer->GetPosition(), at); };
            // [lead380] b200 rfc-h (8 runs): the healers walked 256 s of the fights on "heal reach". A healer that
            // heals the member where both stand now (its reach and sight) is not moved for it: the member's spot or
            // the tank's anchor out of its reach is where they will be, not the fact; it moves once they are there
            auto const healsNow = [&](Player* member) { return reaches(healer->GetPosition(), member->GetPosition()); };
            if (rescue)  // [lead980]
            {
                if (!healsNow(rescue))
                    out.push_back(rescue);
            }
            else if (tankUnderAttack ? tankUnhealed : (outOfReach(tankAt) && !healsNow(tank)))  // [lead950]
                out.push_back(tank);
            // [lead950] logged where the old judgement (the anchor by the model's sight) found it reached: once per
            // tank, healer and fight anchor
            if (tankUnhealed && !(outOfReach(tankFightAt()) && !healsNow(tank)))
            {
                thread_local std::set<std::string> missLogged;
                if (missLogged.size() > 4096)
                    missLogged.clear();
                if (missLogged.insert(tank->GetName() + "|" + healer->GetName() + "|" +
                                      std::to_string(lead.fightAnchorSerial)).second)
                    LOG_INFO("playerbots",
                             "[lead950] {}: {} under {} attackers is out of {}'s heal ({:.1f} yd, sight {}) where the "
                             "fight anchor read it reached: judged where it stands",
                             run.runId, tank->GetName(), tank->getAttackers().size(), healer->GetName(),
                             healer->GetExactDist(tank), healer->IsWithinLOSInMap(tank) ? "yes" : "no");
            }
            for (Player* other : seats)
                if (other != healer && other != rescue)
                    if (std::optional<Position> const& at = assigned[other->GetGUID()];
                        at && outOfReach(*at) && !healsNow(other))
                        out.push_back(other);
            // the healer at `spot` reaches the tank where it fights and every assigned spot ([lead980] the attacked
            // member it rescues: that member only)
            auto const keepsAssigned = [&](Position const& spot)
            {
                if (provenOut(spot) || !reaches(spot, tankAt) ||
                    spot.GetExactDist(tankAt) > Leader::LEADER_HEALER_TANK_YD - g_spotAcquireMargin)
                    return false;
                if (rescue)
                    return true;
                for (auto const& [member, at] : assigned)
                    if (at && !reaches(spot, *at))
                        return false;
                return true;
            };
            // [lead55] the heal-reach spot it walks to is kept while it still keeps the group (no new spot per tick);
            // [lead120] [lead121] and while it reaches where the tank fights and the facts it was decided on stand
            Leader::MemberEvents healerEvents;
            bool const ownSpot = Leader::Events::Get(healer->GetGUID(), healerEvents) &&
                                 healerEvents.move == Leader::MoveEvent::MoveTo &&
                                 healerEvents.moveWhy == "heal reach";
            bool sameFacts = ownSpot && lead.healReachAnchor == lead.fightAnchorSerial &&
                             reaches(healerEvents.spot, tankAt) && lead.healReachSpots.size() == assigned.size();
            for (auto const& [member, at] : assigned)
            {
                auto const was = lead.healReachSpots.find(member);
                sameFacts = sameFacts && was != lead.healReachSpots.end() &&
                            was->second.has_value() == at.has_value() &&
                            (!at || was->second->GetExactDist(*at) <= 1.0f);
            }
            bool const keepsOwn =
                ownSpot && !provenOut(healerEvents.spot) && (keepsAssigned(healerEvents.spot) || sameFacts);
            // [lead72] b116: the kept heal-reach spot was not re-asserted (the tick ended it), the combat spot took the
            // healer back the next tick ("was -") and the heal reach respotted again - the held spot is asserted
            // every tick while it still keeps the group (its end condition), so a lower rule cannot take it
            if (keepsOwn)
            {
                out.clear();
                Leader::Events::SetMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, healerEvents.spot,
                                        healerEvents.tolerance, "heal reach");
            }
            if (!out.empty())
            {
                // [lead121] never back to the spot the healer was just sent from while the spot it holds still
                // reaches the tank (A -> B -> A)
                bool const ownValid = ownSpot && reaches(healerEvents.spot, tankAt);
                auto const backAgain = [&](Position const& spot)
                { return ownValid && lead.healReachFrom && lead.healReachFrom->GetExactDist(spot) <= 1.5f; };
                Position fix;
                bool const fixed =
                    slow([&]
                         {
                             SpotAcquire const acquire;  // [lead36]
                             return LeaderHealerRespot(healer, GET_PLAYERBOT_AI(healer), keepsAssigned, fix);
                         });
                bool issued = fixed && !backAgain(fix) && tankHolds(healer->GetPosition(), fix) &&
                              checkedMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, fix, 1.5f, "heal reach");
                // [lead118] b152 rfc-h 895-931 s: "Rtpalaa out of the healer's reach, 57.9 yd, fix: none" until the
                // wipe (the re-spot searches 16 yd round the healer) - in a fight the healer always gets a spot;
                // [lead121] the spot round where the tank fights in heal reach and sight of it that reaches the most
                // assigned spots (the nearest to the healer of those), else the tank's own spot (the same checks
                // before it is issued)
                std::string how = !issued  ? std::string("none")
                                  : rescue ? "[lead980] the healer re-spots to the attacked " + rescue->GetName()
                                           : std::string("the healer re-spots");
                if (!issued)
                {
                    std::vector<std::pair<Position, uint32>> candidates;
                    slow([&]
                         {
                             for (float ring = 5.0f; ring < Leader::LEADER_HEALER_TANK_YD; ring += 5.0f)
                                 for (uint32 i = 0; i < 12; ++i)
                                 {
                                     float const angle = 2.0f * float(M_PI) * float(i) / 12.0f;
                                     float x = tankAt.GetPositionX() + std::cos(angle) * ring;
                                     float y = tankAt.GetPositionY() + std::sin(angle) * ring;
                                     float z = tankAt.GetPositionZ();
                                     if (!map->CheckCollisionAndGetValidCoords(tank, tankAt.GetPositionX(),
                                                                               tankAt.GetPositionY(),
                                                                               tankAt.GetPositionZ(), x, y, z))
                                         continue;
                                     Position const spot(x, y, z);
                                     if (provenOut(spot) || !reaches(spot, tankAt) || backAgain(spot) ||
                                         !tankHolds(healer->GetPosition(), spot))
                                         continue;
                                     uint32 kept = 0;
                                     for (auto const& [member, at] : assigned)
                                         if (at && reaches(spot, *at))
                                             ++kept;
                                     candidates.emplace_back(spot, kept);
                                 }
                             return true;
                         });
                    std::sort(candidates.begin(), candidates.end(),
                              [healer](auto const& a, auto const& b)
                              {
                                  if (a.second != b.second)
                                      return a.second > b.second;
                                  return healer->GetExactDist(a.first) < healer->GetExactDist(b.first);
                              });
                    for (auto const& candidate : candidates)
                    {
                        fix = candidate.first;
                        issued = checkedMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, fix, 1.5f, "heal reach");
                        if (issued)
                            break;
                    }
                    if (!issued)
                    {
                        fix = tankAt;
                        issued = checkedMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, fix, 1.5f, "heal reach");
                    }
                    if (issued)
                        how = rescue ? "[lead980] the healer walks into reach of the attacked " + rescue->GetName() +
                                           " (nothing attacks the tank)"
                                     : std::string("the healer walks back into reach of the tank");
                    // [lead830] b222 rfc-h 5E pull 7 (133-225 s, the wipe): every heal-reach walk of the holy priest
                    // (the ring round the tank and the tank's own spot) passed an idle Ragefire Trogg's reach; its
                    // pack was judged too much for the fight (fight_moves_away, overload 1.12) and the tank's walk
                    // away was refused by the same guard. 162 spot_rejected_aggro "heal reach", heal_reach "fix:
                    // none" 50 s: the priest stood 32.5 yd off without sight of the tank, cast nothing, and the
                    // paladin died under 5-6 mobs at 188 s, the group after it. A tank under attack that no heal
                    // reaches dies for certain; a pack woken on the healer's walk is a fight the healed tank picks
                    // up. No refusal leaves the tank unhealed: the pack in the way of the healer's walk to the tank
                    // joins the fight (it blocks no walk then) and the walk is issued
                    // [lead909] user 2026-11-03 (45): when the tank's walk to the healer is shorter than the healer's
                    // way to it, the tank walks to the healer (taking its mobs along) - to the first point of its
                    // route to the healer that the healer's heals reach; else the pack in the way joins (lead830)
                    bool tankComes = false;
                    if (!issued && tank->IsAlive() && !tank->getAttackers().empty() &&
                        std::find(out.begin(), out.end(), tank) != out.end() && !lead.fightMoveTo)
                    {
                        Position const healerAt = healer->GetPosition();
                        Leader::RouteResult const way = Leader::Route(tank, healerAt);
                        float tankWalk = -1.0f;
                        Position toward = healerAt;
                        if (way.found && way.complete && !way.points.empty())
                        {
                            float along = 0.0f;
                            constexpr float STEP_YD = 3.0f;
                            for (size_t i = 1; i < way.points.size() && tankWalk < 0.0f; ++i)
                            {
                                G3D::Vector3 const from = way.points[i - 1];
                                G3D::Vector3 const leg = way.points[i] - from;
                                float const length = leg.length();
                                // [lead932] the leg's end is sampled too (a leg not a multiple of 3 yd never reached
                                // its end: the route's last point, the healer's own place, was never judged)
                                for (float step = 0.0f; step < length + STEP_YD && tankWalk < 0.0f; step += STEP_YD)
                                {
                                    G3D::Vector3 const at =
                                        length > 0.0f ? from + leg * (std::min(step, length) / length) : from;
                                    Position const point(at.x, at.y, at.z);
                                    if (reaches(healerAt, point))
                                    {
                                        toward = point;
                                        tankWalk = along + std::min(step, length);
                                    }
                                }
                                along += length;
                            }
                        }
                        float const healerWalk =
                            Leader::WalkYd(healer, candidates.empty() ? tankAt : candidates.front().first);
                        if (tankWalk >= 0.0f && tankWalk < healerWalk)
                        {
                            // [lead950] a heal-reach move (MovePrecedence 3): the stay the tank's refused walk left
                            // this tick ("the tank goes to its mob out of its sight: stays", rank 0) held it at equal
                            // rank - b227's one "its walk is refused by the move guards"
                            std::string const why =
                                "heal reach: the fight moves to the blocked healer " + healer->GetName();
                            if (checkedMove(tank->GetGUID(), Leader::MoveEvent::MoveTo, toward, 2.0f, why))
                            {
                                tankComes = true;
                                lead.fightMoveTo = toward;
                                lead.fightMoveWhy = why;
                                how = "the tank walks to the healer";
                                LogFightDrag(run, tank, toward, lead.meetPoint, "to the blocked healer");
                                LOG_INFO("playerbots",
                                         "[lead909] {}: no heal reaches it under {} attackers; its walk to {} "
                                         "({:.0f} yd) is shorter than the healer's ({:.0f} yd): the tank walks",
                                         tank->GetName(), tank->getAttackers().size(), healer->GetName(), tankWalk,
                                         healerWalk);
                            }
                        }
                        // [lead932] b226: the check ran before every lead830 join (8 in 16 runs) and never walked the
                        // tank, with no line saying why: the verdict not taken is logged (once per tank, healer and
                        // reason)
                        if (!tankComes)
                        {
                            std::string const reason =
                                !way.found || !way.complete ? std::string("no complete route to the healer")
                                : tankWalk < 0.0f           ? std::string("no point of its route in the healer's reach")
                                : tankWalk >= healerWalk    ? std::string("the healer's walk is no longer")
                                                            : std::string("its walk is refused by the move guards");
                            thread_local std::set<std::string> notTakenLogged;
                            if (notTakenLogged.size() > 4096)
                                notTakenLogged.clear();
                            if (notTakenLogged.insert(tank->GetName() + "|" + healer->GetName() + "|" + reason).second)
                                LOG_INFO("playerbots",
                                         "[lead909] {}: no heal reaches it under {} attackers; the tank does not walk "
                                         "to {}: {} (its walk {:.0f} yd, the healer's {:.0f} yd)",
                                         tank->GetName(), tank->getAttackers().size(), healer->GetName(), reason,
                                         tankWalk, healerWalk);
                        }
                    }
                    if (!issued && !tankComes && tank->IsAlive() && !tank->getAttackers().empty() &&
                        std::find(out.begin(), out.end(), tank) != out.end())
                    {
                        // the best ring spot (out of the tank's melee pile), else the tank's own spot
                        std::vector<Position> goals;
                        if (!candidates.empty())
                            goals.push_back(candidates.front().first);
                        goals.push_back(tankAt);
                        std::set<ObjectGuid> joined;
                        for (Position const& goal : goals)
                        {
                            fix = goal;
                            lead.spotChecked.erase(healer->GetGUID());  // judged again on this tick's facts
                            issued =
                                checkedMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, fix, 1.5f, "heal reach");
                            while (!issued)
                            {
                                auto const verdict = lead.spotChecked.find(healer->GetGUID());
                                Creature* blocker =
                                    verdict != lead.spotChecked.end() && !verdict->second.mob.IsEmpty()
                                        ? map->GetCreature(verdict->second.mob)
                                        : nullptr;
                                if (!blocker || !blocker->IsAlive() || blocker->IsInCombat() ||
                                    !joined.insert(blocker->GetGUID()).second)
                                    break;
                                std::vector<ObjectGuid> const packMobs = packOf(blocker);
                                joinPack(blocker, packMobs);
                                lead.fightMoveTo.reset();  // the fight stays: no walk away from a pack it holds
                                Decisions().Write(run, "leader", "pack_joins", blocker->GetGUID(),
                                                  blocker->GetName(), true,
                                                  JsonObject()
                                                      .Num("packMobs", int64(packMobs.size()))
                                                      .Str("judged", "the healer's walk to the tank under attack")
                                                      .Str("blocked", healer->GetName()));
                                LOG_INFO("playerbots",
                                         "[lead830] {}: no heal reaches {} under {} attackers; {}'s pack ({} mobs) "
                                         "on the healer's walk joins the fight",
                                         healer->GetName(), tank->GetName(), tank->getAttackers().size(),
                                         blocker->GetName(), packMobs.size());
                                lead.spotChecked.erase(healer->GetGUID());  // the refusal's fact changed
                                issued = checkedMove(healer->GetGUID(), Leader::MoveEvent::MoveTo, fix, 1.5f,
                                                     "heal reach");
                            }
                            if (issued)
                                break;
                        }
                        if (issued)
                            how = "the healer walks back into reach of the tank (the pack on its walk joins)";
                    }
                }
                if (issued)
                {
                    // [lead120] [lead121] the facts this decision was made for, and the spot it leaves
                    lead.healReachAnchor = lead.fightAnchorSerial;
                    lead.healReachSpots = assigned;
                    if (ownSpot)
                        lead.healReachFrom = healerEvents.spot;
                    else
                        lead.healReachFrom.reset();
                    logEvent(healer, MoveEventName(Leader::MoveEvent::MoveTo),
                             JsonObject().Str("why", "heal reach").Real("x", fix.GetPositionX())
                                 .Real("y", fix.GetPositionY()).Real("z", fix.GetPositionZ()));
                }
                for (Player* other : out)
                    LogHealReach(group, other, healer, "out of the healer's reach in the fight", how);
            }
        }

        lead.hazardActive = hazardSeen;
        // [lead18] healer_lost_tank_s: the fight's time with the healer out of sight or LEADER_HEALER_LOST_YD of the
        // tank (the heal-reach check above re-spots it the same update)
        if (fighting && healer && tank->IsAlive())
        {
            uint32 const step = lead.lostCheckMs ? std::min<uint32>(getMSTimeDiff(lead.lostCheckMs, now), 500) : 0;
            if (healer->GetExactDist(tank) > Leader::LEADER_HEALER_LOST_YD || !healer->IsWithinLOSInMap(tank))
                lead.healerLostTankMs += step;
        }
        lead.lostCheckMs = now;
        uint64 const totalUs = uint64(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - tickStart).count());
        lead.cadenceSlowUs += lead.slowUsTick;
        lead.cadenceFastUs += totalUs > lead.slowUsTick ? totalUs - lead.slowUsTick : 0;
        lead.cadenceMaxUs = std::max(lead.cadenceMaxUs, totalUs);

        if (!lead.wakeLogMs)
            lead.wakeLogMs = now;
        else if (getMSTimeDiff(lead.wakeLogMs, now) >= LEADER_WAKE_LOG_MS)
        {
            uint32 const sinceMs = getMSTimeDiff(lead.wakeLogMs, now);
            float const minutes = float(sinceMs) / float(MINUTE * IN_MILLISECONDS);
            uint32 ticks = 0;
            for (uint32 count : lead.cadenceTicks)
                ticks += count;
            group.LogEvent("member_wake", JsonObject().Num("count", lead.wakeCount)
                                               .Num("sinceSec", sinceMs / IN_MILLISECONDS)
                                               .Num("total", lead.wakeTotal + lead.wakeCount));
            // the leader's cadence per state (ticks per minute) and the layers' cost per tick (µs)
            group.LogEvent("leader_cadence",
                     JsonObject().Num("sinceSec", sinceMs / IN_MILLISECONDS)
                         .Real("calmPerMin", float(lead.cadenceTicks[0]) / minutes)
                         .Real("prePullPerMin", float(lead.cadenceTicks[1]) / minutes)
                         .Real("combatPerMin", float(lead.cadenceTicks[2]) / minutes)
                         .Real("hazardPerMin", float(lead.cadenceTicks[3]) / minutes)
                         .Real("fastUsPerTick", ticks ? float(lead.cadenceFastUs) / float(ticks) : 0.0f)
                         .Real("slowUsPerTick", ticks ? float(lead.cadenceSlowUs) / float(ticks) : 0.0f)
                         .Num("maxUs", int64(lead.cadenceMaxUs)).Num("ticks", ticks)
                         .Real("usPerSec", float(lead.cadenceFastUs + lead.cadenceSlowUs) /
                                               (float(sinceMs) / 1000.0f)));
            lead.wakeTotal += lead.wakeCount;
            lead.wakeCount = 0;
            lead.wakeLogMs = now;
            for (uint32& count : lead.cadenceTicks)
                count = 0;
            lead.cadenceFastUs = lead.cadenceSlowUs = lead.cadenceMaxUs = 0;
        }
    }

    // [lead15] a heal_reach violation as a decision record (per member at most every LEADER_HEAL_REACH_LOG_MS)
    void LogHealReach(LeaderGroup& group, Player* member, Player* healer, std::string const& why,
                      std::string const& fix)
    {
        auto& lead = group.State();
        uint32 const now = getMSTime() ? getMSTime() : 1;
        uint32& last = lead.healReachLogMs[member->GetGUID()];
        if (last && getMSTimeDiff(last, now) < LEADER_HEAL_REACH_LOG_MS)
            return;
        last = now;
        DecisionRun const run = group.Run();
        Decisions().Write(run, "leader", "heal_reach", member->GetGUID(), member->GetName(), true,
                                      JsonObject().Str("member", member->GetName())
                                          .Str("healer", healer ? healer->GetName() : "?")
                                          .Real("distance", healer ? healer->GetExactDist(member) : -1.0f)
                                          .Bool("los", healer && healer->IsWithinLOSInMap(member))
                                          .Str("why", why).Str("fix", fix));
    }
}
