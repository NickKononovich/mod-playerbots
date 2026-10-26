/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InterruptValues.h"

#include "DBCStructure.h"
#include "Group.h"
#include "InterruptStrategy.h"
#include "LeaderCommand.h"  // [rot2]
#include "Map.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TargetValue.h"
#include "Timer.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32 MIN_CAST_REMAINING_MS = 200;    // too late to react to a cast ending sooner
    constexpr uint32 HANDOFF_MS = 700;               // an interrupter that has not acted by then loses the cast
    constexpr uint32 FIRED_GRACE_MS = 300;           // a cast still running this long after the kick was missed
    constexpr uint32 SAME_CAST_TOLERANCE_MS = 500;   // two sightings of one cast agree on its end within this
    constexpr uint32 CLAIM_LINGER_MS = 1000;         // a claim outlives the end of its cast this long
    constexpr uint32 PASS_THROTTLE_MS = 150;         // one assignment pass per instance this often is enough
    constexpr uint32 STOP_CAST_MIN_REMAINING_MS = 400;  // finish an own cast that is about to land
    constexpr float SECONDARY_OPTION_PENALTY = 30.0f;   // a stun or a long cooldown only when no kick is free
    constexpr float BUSY_CASTER_PENALTY = 8.0f;         // a member mid-cast must stop-cast first
    constexpr float TANK_SILENCE_BONUS = 15.0f;         // a silence makes a caster at range walk into the tank
    constexpr float RANGE_MARGIN = 1.0f;
    constexpr float LOW_HEALTH_PCT = 50.0f;
    constexpr float CHAIN_BREAKS_CC_RANGE = 15.0f;      // Avenger's Shield jumps to mobs this close to its target
    constexpr int32 LONG_NUKE_CAST_MS = 2500;           // [ranged_audit_b30] a base cast this long is a big nuke

    constexpr uint32 SPELL_SHIELD_OF_THE_TEMPLAR_R1 = 53709;
    constexpr uint32 SPELL_SHIELD_OF_THE_TEMPLAR_R2 = 53710;
    constexpr uint32 SPELL_SHIELD_OF_THE_TEMPLAR_R3 = 53711;

    enum class OptionKind : uint8
    {
        Interrupt,  // kick, silence: locks the school or silences
        Secondary,  // stun or long cooldown: stops the cast, saved for when no kick is ready
    };

    struct InterruptOption
    {
        char const* spell;
        OptionKind kind;
        bool needsTemplar;  // Avenger's Shield silences only with the Shield of the Templar talent; it also
                            // bounces on to nearby mobs, so it is never thrown next to crowd control
    };

    std::vector<InterruptOption> const& GetClassOptions(uint8 classId)
    {
        static std::vector<InterruptOption> const none;
        static std::vector<InterruptOption> const mage = {{"counterspell", OptionKind::Interrupt, false}};
        static std::vector<InterruptOption> const shaman = {{"wind shear", OptionKind::Interrupt, false}};
        static std::vector<InterruptOption> const rogue = {{"kick", OptionKind::Interrupt, false}};
        static std::vector<InterruptOption> const warrior = {{"pummel", OptionKind::Interrupt, false},
                                                             {"shield bash", OptionKind::Interrupt, false}};
        static std::vector<InterruptOption> const deathKnight = {{"mind freeze", OptionKind::Interrupt, false},
                                                                 {"strangulate", OptionKind::Secondary, false}};
        static std::vector<InterruptOption> const hunter = {{"silencing shot", OptionKind::Interrupt, false}};
        static std::vector<InterruptOption> const paladin = {{"avenger's shield", OptionKind::Interrupt, true},
                                                             {"hammer of justice", OptionKind::Secondary, false}};
        static std::vector<InterruptOption> const priest = {{"silence", OptionKind::Interrupt, false}};
        static std::vector<InterruptOption> const druid = {{"bash", OptionKind::Secondary, false}};

        switch (classId)
        {
            case CLASS_MAGE:
                return mage;
            case CLASS_SHAMAN:
                return shaman;
            case CLASS_ROGUE:
                return rogue;
            case CLASS_WARRIOR:
                return warrior;
            case CLASS_DEATH_KNIGHT:
                return deathKnight;
            case CLASS_HUNTER:
                return hunter;
            case CLASS_PALADIN:
                return paladin;
            case CLASS_PRIEST:
                return priest;
            case CLASS_DRUID:
                return druid;
            default:
                return none;  // the warlock's Spell Lock is a pet spell and stays with the warlock strategy
        }
    }

    std::vector<InterruptOption> const& GetRacialOptions()
    {
        static std::vector<InterruptOption> const racials = {{"arcane torrent", OptionKind::Interrupt, false},
                                                             {"war stomp", OptionKind::Secondary, false}};
        return racials;
    }

    struct Claim
    {
        ObjectGuid interrupter;
        std::string spell;
        uint32 castSpellId = 0;
        uint32 castEndMs = 0;
        uint32 assignedMs = 0;
        uint32 firedMs = 0;
        GuidVector tried;  // members that already had this cast and did not stop it
    };

    // The claims of one instance; only bots of that map touch them, the lock guards against the rare second
    // map thread of a party split across maps.
    struct InstanceClaims
    {
        std::mutex lock;
        std::unordered_map<ObjectGuid, Claim> claims;
        uint32 lastPassMs = 0;
    };

    struct Registry
    {
        std::mutex lock;
        std::unordered_map<uint64, InstanceClaims> byInstance;  // node-based: entries keep their address
    };

    InstanceClaims* FindInstanceClaims(Player* bot, bool create)
    {
        static Registry registry;
        uint64 const key = (uint64(bot->GetMapId()) << 32) | bot->GetInstanceId();
        std::lock_guard<std::mutex> guard(registry.lock);
        if (create)
            return &registry.byInstance[key];

        auto const itr = registry.byInstance.find(key);
        return itr == registry.byInstance.end() ? nullptr : &itr->second;
    }

    struct CastInfo
    {
        ObjectGuid caster;
        uint32 spellId = 0;
        uint32 remainingMs = 0;
        uint8 priority = 0;
        bool crowdControlNearby = false;
    };

    // The cast `unit` is making right now if a kick, silence or stun can stop it (the core's EffectInterruptCast
    // rule: silence-prevented, flagged interruptible, still being cast or channelled).
    Spell* GetInterruptibleCast(Unit* unit)
    {
        if (Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        {
            SpellInfo const* info = spell->GetSpellInfo();
            if (info && info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE &&
                (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) &&
                spell->getState() == SPELL_STATE_PREPARING && spell->GetCastTime() > 0)
                return spell;
        }

        if (Spell* spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            SpellInfo const* info = spell->GetSpellInfo();
            if (info && info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE &&
                (info->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT) &&
                spell->getState() == SPELL_STATE_CASTING)
                return spell;
        }

        return nullptr;
    }

    // Heals first, then damage aimed at a healer or a member already low, then everything else.
    uint8 GetCastPriority(Spell* spell)
    {
        SpellInfo const* info = spell->GetSpellInfo();
        if (info->HasEffect(SPELL_EFFECT_HEAL) || info->HasEffect(SPELL_EFFECT_HEAL_PCT) ||
            info->HasAura(SPELL_AURA_PERIODIC_HEAL))
            return 3;

        // [ranged_audit_b30] brs-h build 30: a 5026 Mana Burn from a Smolderthorn caster killed the elemental
        // shaman uninterrupted (priority 1, the shaman is no healer). Mana Burn / Drain Mana rank with the heals;
        // a fear, charm, polymorph or stun on a member and a long nuke (2.5 s or more) rank with the area damage.
        if (info->HasEffect(SPELL_EFFECT_POWER_BURN) || info->HasEffect(SPELL_EFFECT_POWER_DRAIN))
            return 3;

        if (info->HasAura(SPELL_AURA_MOD_FEAR) || info->HasAura(SPELL_AURA_MOD_CHARM) ||
            info->HasAura(SPELL_AURA_MOD_POSSESS) || info->HasAura(SPELL_AURA_MOD_CONFUSE) ||
            info->HasAura(SPELL_AURA_MOD_STUN))
            return 2;

        if (info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) && info->CastTimeEntry &&
            info->CastTimeEntry->CastTime >= LONG_NUKE_CAST_MS)
            return 2;

        // Ground and area damage (Flamestrike, Blizzard, Rain of Fire) hits several members at once.
        if (info->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA))
            return 2;

        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE &&
                effect.TargetA.GetSelectionCategory() == TARGET_SELECT_CATEGORY_AREA)
                return 2;
        }

        // By GUID, never the cast's stored pointer: its target may have left the map (crash 2026-10-25 04:17).
        WorldObject* caster = spell->GetCaster();
        Unit* target = caster ? ObjectAccessor::GetUnit(*caster, spell->m_targets.GetUnitTargetGUID()) : nullptr;
        if (target && target->IsInWorld() && target->GetMap() == caster->GetMap())
        {
            if (Player* player = target->ToPlayer())
            {
                if (PlayerbotAI::IsHeal(player) || player->GetHealthPct() < LOW_HEALTH_PCT)
                    return 2;
            }
        }

        return 1;
    }

    bool HasShieldOfTheTemplar(Player* member)
    {
        return member->HasAura(SPELL_SHIELD_OF_THE_TEMPLAR_R3) || member->HasAura(SPELL_SHIELD_OF_THE_TEMPLAR_R2) ||
               member->HasAura(SPELL_SHIELD_OF_THE_TEMPLAR_R1);
    }

    bool IsInInterruptRange(Player* member, Unit* caster, SpellInfo const* info)
    {
        float const distance = member->GetDistance(caster);
        if (!info->NeedsExplicitUnitTarget())
        {
            // Arcane Torrent, War Stomp: an area around the member
            float const radius = info->Effects[EFFECT_0].CalcRadius(member);
            return radius > 0.0f && distance <= radius - RANGE_MARGIN;
        }

        if (info->RangeEntry && (info->RangeEntry->Flags & SPELL_RANGE_MELEE))
            return member->IsWithinMeleeRange(caster);

        float const maxRange = info->GetMaxRange(false, member);
        return distance <= maxRange - RANGE_MARGIN && distance >= info->GetMinRange(false);
    }

    struct Candidate
    {
        Player* member;
        std::string spell;
        float score;
    };

    // The best ready interrupt `member` has for `caster`, scored lower-is-better; false when it has none.
    bool EvaluateMember(Player* member, PlayerbotAI* memberAI, Unit* caster, bool crowdControlNearby,
                        Candidate& out)
    {
        if (member->HasUnitState(UNIT_STATE_LOST_CONTROL) || member->HasUnitFlag(UNIT_FLAG_SILENCED) ||
            member->HasUnitFlag(UNIT_FLAG_PACIFIED))
            return false;

        bool const busy =
            member->GetCurrentSpell(CURRENT_GENERIC_SPELL) || member->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        // A healer finishes its heal; a tank or dps may stop-cast.
        if (busy && PlayerbotAI::IsHeal(member))
            return false;

        bool found = false;
        auto const consider = [&](InterruptOption const& option)
        {
            uint32 const spellId = memberAI->GetAiObjectContext()->GetValue<uint32>("spell id", option.spell)->Get();
            if (!spellId || member->HasSpellCooldown(spellId))
                return;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->CheckShapeshift(member->GetShapeshiftForm()) != SPELL_CAST_OK)
                return;

            if (option.needsTemplar && (crowdControlNearby || !HasShieldOfTheTemplar(member)))
                return;

            if (info->PowerType < MAX_POWERS &&
                int32(member->GetPower(Powers(info->PowerType))) < info->CalcPowerCost(member, info->GetSchoolMask()))
                return;

            if (!InterruptCoordination::CanStopCast(caster, info) || !IsInInterruptRange(member, caster, info))
                return;

            float score = member->GetDistance(caster);
            if (option.kind == OptionKind::Secondary)
                score += SECONDARY_OPTION_PENALTY;

            if (busy)
                score += BUSY_CASTER_PENALTY;

            // The caster stands back nuking the tank: the tank's silence brings it into the pack.
            if (option.needsTemplar && caster->GetVictim() == member && !member->IsWithinMeleeRange(caster))
                score -= TANK_SILENCE_BONUS;

            if (!found || score < out.score)
            {
                out = {member, option.spell, score};
                found = true;
            }
        };

        for (InterruptOption const& option : GetClassOptions(member->getClass()))
            consider(option);

        for (InterruptOption const& option : GetRacialOptions())
            consider(option);

        return found;
    }

    bool IsSameCast(Claim const& claim, CastInfo const& cast, uint32 now)
    {
        if (claim.castSpellId != cast.spellId)
            return false;

        uint32 const castEnd = now + cast.remainingMs;
        uint32 const diff = claim.castEndMs > castEnd ? claim.castEndMs - castEnd : castEnd - claim.castEndMs;
        return diff <= SAME_CAST_TOLERANCE_MS;
    }

    std::vector<std::pair<Player*, PlayerbotAI*>> GetCoordinatingMembers(Player* bot, Group* group)
    {
        std::vector<std::pair<Player*, PlayerbotAI*>> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetMap() != bot->GetMap() || !member->IsInWorld() || !member->IsAlive())
                continue;

            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            if (!memberAI || !InterruptStrategy::IsCoordinating(memberAI))
                continue;

            members.emplace_back(member, memberAI);
        }

        return members;
    }
}

namespace InterruptCoordination
{
    InterruptAssignment Update(PlayerbotAI* botAI)
    {
        Player* bot = botAI->GetBot();
        if (!bot || !bot->IsInCombat() || !bot->IsInWorld())
            return {};

        // [rot2] A bot the dungeon leader leads interrupts only the caster the leader assigns it, with its best ready
        // interrupt for that cast; it picks no cast of its own (no assignment: none). A leader that stopped (no fresh
        // command) leaves the group rotation below in charge again.
        Leader::MemberEvents command;
        if (LeaderCommand::CurrentCommand(bot, command))
        {
            Unit* caster = command.interrupt.IsEmpty() ? nullptr : botAI->GetUnit(command.interrupt);
            if (!caster || !caster->IsAlive() || caster->GetMap() != bot->GetMap() || !GetInterruptibleCast(caster))
                return {};

            Candidate candidate{nullptr, "", 0.0f};
            if (!EvaluateMember(bot, botAI, caster, true, candidate))
                return {};

            return {caster->GetGUID(), candidate.spell};
        }

        GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
        std::vector<CastInfo> casts;
        std::vector<Unit*> controlled;
        for (ObjectGuid const& guid : attackers)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive())
                continue;

            if (IsCrowdControlled(unit))
            {
                controlled.push_back(unit);
                continue;
            }

            Spell* spell = GetInterruptibleCast(unit);
            if (!spell || spell->GetCastTimeRemaining() < int32(MIN_CAST_REMAINING_MS))
                continue;

            casts.push_back({guid, spell->GetSpellInfo()->Id, uint32(spell->GetCastTimeRemaining()),
                             GetCastPriority(spell), false});
        }

        if (casts.empty())
            return {};

        for (CastInfo& cast : casts)
        {
            Unit* caster = botAI->GetUnit(cast.caster);
            auto const nearCaster = [caster](Unit* unit)
            { return unit->GetDistance(caster) <= CHAIN_BREAKS_CC_RANGE; };
            cast.crowdControlNearby = caster && std::any_of(controlled.begin(), controlled.end(), nearCaster);
        }

        std::sort(casts.begin(), casts.end(), [](CastInfo const& a, CastInfo const& b)
        {
            if (a.priority != b.priority)
                return a.priority > b.priority;

            return a.remainingMs < b.remainingMs;
        });

        Group* group = bot->GetGroup();
        uint32 const now = getMSTime();
        InstanceClaims& instance = *FindInstanceClaims(bot, true);
        std::lock_guard<std::mutex> guard(instance.lock);

        if (getMSTimeDiff(instance.lastPassMs, now) >= PASS_THROTTLE_MS || !instance.lastPassMs)
        {
            instance.lastPassMs = now;
            for (auto itr = instance.claims.begin(); itr != instance.claims.end();)
            {
                if (itr->second.castEndMs + CLAIM_LINGER_MS < now)
                    itr = instance.claims.erase(itr);
                else
                    ++itr;
            }

            std::vector<std::pair<Player*, PlayerbotAI*>> members;
            if (group)
                members = GetCoordinatingMembers(bot, group);
            else if (InterruptStrategy::IsCoordinating(botAI))
                members.emplace_back(bot, botAI);

            for (CastInfo const& cast : casts)
            {
                Unit* caster = botAI->GetUnit(cast.caster);
                if (!caster)
                    continue;

                auto existing = instance.claims.find(cast.caster);
                GuidVector tried;
                if (existing != instance.claims.end() && IsSameCast(existing->second, cast, now))
                {
                    Claim const& claim = existing->second;
                    bool const claimantPresent =
                        std::any_of(members.begin(), members.end(),
                                    [&claim](auto const& member)
                                    {
                                        return member.first->GetGUID() == claim.interrupter &&
                                               !member.first->HasUnitState(UNIT_STATE_LOST_CONTROL);
                                    });
                    bool const handoff = getMSTimeDiff(claim.assignedMs, now) > HANDOFF_MS ||
                                         (claim.firedMs && getMSTimeDiff(claim.firedMs, now) > FIRED_GRACE_MS);
                    if (claimantPresent && !handoff)
                        continue;

                    tried = claim.tried;
                    tried.push_back(claim.interrupter);
                }

                Candidate best{nullptr, "", 0.0f};
                std::vector<Candidate> candidates;
                for (auto const& entry : members)
                {
                    Player* member = entry.first;
                    if (std::find(tried.begin(), tried.end(), member->GetGUID()) != tried.end())
                        continue;

                    // one cast at a time per member: it keeps its kick for the cast it already holds
                    bool const holdsOther = std::any_of(instance.claims.begin(), instance.claims.end(),
                                                        [&](auto const& held)
                                                        {
                                                            return held.first != cast.caster &&
                                                                   held.second.interrupter == member->GetGUID() &&
                                                                   !held.second.firedMs &&
                                                                   held.second.castEndMs > now;
                                                        });
                    if (holdsOther)
                        continue;

                    Candidate candidate{nullptr, "", 0.0f};
                    if (EvaluateMember(member, entry.second, caster, cast.crowdControlNearby, candidate))
                        candidates.push_back(candidate);
                }

                std::sort(candidates.begin(), candidates.end(),
                          [](Candidate const& a, Candidate const& b) { return a.score < b.score; });
                for (Candidate const& candidate : candidates)
                {
                    if (candidate.member->IsWithinLOSInMap(caster))
                    {
                        best = candidate;
                        break;
                    }
                }

                if (!best.member)
                {
                    // nobody can take it: drop the claim so the class triggers are not blocked by a stale one
                    if (existing != instance.claims.end())
                        instance.claims.erase(existing);
                    continue;
                }

                Claim claim;
                claim.interrupter = best.member->GetGUID();
                claim.spell = best.spell;
                claim.castSpellId = cast.spellId;
                claim.castEndMs = now + cast.remainingMs;
                claim.assignedMs = now;
                claim.tried = std::move(tried);
                instance.claims[cast.caster] = std::move(claim);
            }
        }

        for (CastInfo const& cast : casts)
        {
            auto const itr = instance.claims.find(cast.caster);
            if (itr != instance.claims.end() && itr->second.interrupter == bot->GetGUID() && !itr->second.firedMs &&
                IsSameCast(itr->second, cast, now))
                return {cast.caster, itr->second.spell};
        }

        return {};
    }

    bool Allows(PlayerbotAI* botAI, Unit* caster, std::string const& spell)
    {
        Player* bot = botAI->GetBot();
        if (!bot || !caster)
            return true;

        // [rot2] a led bot: only the caster the leader assigned it (its own interrupt picking is off)
        if (LeaderCommand::Led(bot))
        {
            Leader::MemberEvents command;
            if (LeaderCommand::CurrentCommand(bot, command))
                return command.interrupt == caster->GetGUID();
        }

        // keeps the registry current for this tick
        botAI->GetAiObjectContext()->GetValue<InterruptAssignment>("interrupt assignment")->Get();

        uint32 const now = getMSTime();
        InstanceClaims* instance = FindInstanceClaims(bot, false);
        if (!instance)
            return true;

        std::lock_guard<std::mutex> guard(instance->lock);
        auto const itr = instance->claims.find(caster->GetGUID());
        if (itr == instance->claims.end() || itr->second.castEndMs < now)
            return true;

        return itr->second.interrupter == bot->GetGUID() && itr->second.spell == spell;
    }

    void MarkFired(PlayerbotAI* botAI, Unit* caster)
    {
        Player* bot = botAI->GetBot();
        if (!bot || !caster)
            return;

        InstanceClaims* instance = FindInstanceClaims(bot, false);
        if (!instance)
            return;

        std::lock_guard<std::mutex> guard(instance->lock);
        auto const itr = instance->claims.find(caster->GetGUID());
        if (itr != instance->claims.end() && itr->second.interrupter == bot->GetGUID())
            itr->second.firedMs = getMSTime();
    }

    bool ShouldStopCastToInterrupt(PlayerbotAI* botAI)
    {
        Player* bot = botAI->GetBot();
        if (!bot || !bot->IsInCombat() || PlayerbotAI::IsHeal(bot))
            return false;

        Spell* own = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!own || own->GetCastTimeRemaining() < int32(STOP_CAST_MIN_REMAINING_MS))
            return false;

        InterruptAssignment const assignment =
            botAI->GetAiObjectContext()->GetValue<InterruptAssignment>("interrupt assignment")->Get();
        if (!assignment.caster)
            return false;

        Unit* caster = botAI->GetUnit(assignment.caster);
        return caster && caster->IsAlive() && GetInterruptibleCast(caster);
    }

    bool PickOrderedInterrupter(Player* bot, Unit* caster, ObjectGuid& interrupter, std::string& spell)
    {
        if (!bot || !caster || !caster->IsAlive() || !GetInterruptibleCast(caster))
            return false;

        std::vector<std::pair<Player*, PlayerbotAI*>> members;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member->GetMap() != bot->GetMap() || !member->IsInWorld() || !member->IsAlive())
                    continue;

                if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member))
                    members.emplace_back(member, memberAI);
            }
        }
        else if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            members.emplace_back(bot, botAI);

        std::vector<Candidate> candidates;
        for (auto const& [member, memberAI] : members)
        {
            Candidate candidate{nullptr, "", 0.0f};
            if (EvaluateMember(member, memberAI, caster, true, candidate))
                candidates.push_back(candidate);
        }

        // ties go to the earlier group slot, so every bot picks the same member
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](Candidate const& a, Candidate const& b) { return a.score < b.score; });
        for (Candidate const& candidate : candidates)
        {
            if (candidate.member->IsWithinLOSInMap(caster))
            {
                interrupter = candidate.member->GetGUID();
                spell = candidate.spell;
                return true;
            }
        }

        return false;
    }

    bool CanStopCast(Unit* caster, SpellInfo const* info)
    {
        if (!caster || !info)
            return false;

        // [b31_shared] Silence, Strangulate and Bash also cast the core's non-player interrupt (32747) on a hit
        // creature, an immune hit included (Spell::DoAllEffectOnTarget). A boss immune to the silence mechanic had
        // the priest's Silence read "cannot stop": 0 claims in 157 boss attempts.
        if (info->HasAttribute(SPELL_ATTR7_CAN_CAUSE_INTERRUPT) && !caster->IsPlayer() &&
            info->Id != SPELL_INTERRUPT_NONPLAYER)
        {
            if (SpellInfo const* interrupt = sSpellMgr->GetSpellInfo(SPELL_INTERRUPT_NONPLAYER))
                if (CanStopCast(caster, interrupt))
                    return true;
        }

        if (caster->IsImmunedToSpell(info))
            return false;

        bool hasStopEffect = false;
        for (uint8 i = EFFECT_0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = info->Effects[i];
            bool const stops = effect.Effect == SPELL_EFFECT_INTERRUPT_CAST ||
                               (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                                (effect.ApplyAuraName == SPELL_AURA_MOD_SILENCE ||
                                 effect.ApplyAuraName == SPELL_AURA_MOD_PACIFY_SILENCE ||
                                 effect.ApplyAuraName == SPELL_AURA_MOD_STUN));
            if (!stops)
                continue;

            hasStopEffect = true;
            if (!caster->IsImmunedToSpellEffect(info, i))
                return true;
        }

        return !hasStopEffect;
    }
}

InterruptAssignment InterruptAssignmentValue::Calculate() { return InterruptCoordination::Update(botAI); }
