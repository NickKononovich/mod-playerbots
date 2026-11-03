/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderSupport.h"

#include <algorithm>
#include <list>
#include <map>

#include "AiFactory.h"  // [lead774] the cat's spec
#include "ChooseTargetActions.h"  // [lead520] PullReadiness::MemberNeedsManaPotion
#include "Creature.h"
#include "DamageTaken.h"
#include "DungeonLeaderState.h"
#include "GameObject.h"
#include "GroupRouteState.h"
#include "HealthTriggers.h"  // RestState
#include "Item.h"
#include "ItemTemplate.h"  // [lead520]
#include "LeaderFightChecks.h"
#include "LeaderOrders.h"
#include "LeaderPlannerShared.h"  // [lead791] TankEngageOpeners
#include "LeaderPrepull.h"  // [lead773] StealthHolds
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"  // [lead520] the mana gem's item cooldown
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RaidConsumables.h"
#include "RaidLeaderState.h"  // CanResurrect, SetResurrectFirst
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"

namespace Leader::Support
{
namespace
{

// Cleanse, Purify, Dispel Magic, Cure Disease, Abolish Disease, Cleanse Spirit, Cure Toxins, Remove Curse (mage),
// Remove Curse (druid), Cure Poison, Abolish Poison: first ranks; the types each removes come from its own effects
constexpr uint32 DISPEL_SPELLS[] = {4987, 1152, 527, 528, 552, 51886, 526, 475, 2782, 8946, 2893};
// Resurrection, Redemption, Ancestral Spirit, Revive: first ranks
constexpr uint32 RESURRECT_SPELLS[] = {2006, 7328, 2008, 50769};
// Rebirth (every rank), Raise Ally
constexpr uint32 SPELL_RAISE_ALLY = 61999;
// Ritual of Souls ranks 1 / 2
constexpr uint32 RITUAL_OF_SOULS[] = {29893, 58887};
// Minor .. Demonic Soulstone, and the auras they leave
constexpr uint32 SOULSTONE_ITEMS[] = {5232, 16892, 16893, 16895, 16896, 22116, 36895};
constexpr uint32 SOULSTONE_AURAS[] = {20707, 20762, 20763, 20764, 20765, 27239, 47883};

// the highest rank of the chain starting at `first` the bot knows (0: none)
uint32 KnownRank(Player* bot, uint32 first)
{
    uint32 known = 0;
    for (uint32 spell = first; spell; spell = sSpellMgr->GetNextSpellInChain(spell))
        if (bot->HasSpell(spell))
            known = spell;
    return known;
}

// the dispel types (1 << DispelType) a spell removes: its SPELL_EFFECT_DISPEL effects
uint32 DispelMask(SpellInfo const* info)
{
    uint32 mask = 0;
    if (info)
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_DISPEL && effect.MiscValue >= 0 && effect.MiscValue < 32)
                mask |= 1u << effect.MiscValue;
    return mask;
}

// the bot has the power the spell costs now
bool Affords(Player* bot, SpellInfo const* info)
{
    if (!info || info->PowerType >= MAX_POWERS)
        return info != nullptr;
    return int32(bot->GetPower(Powers(info->PowerType))) >= info->CalcPowerCost(bot, info->GetSchoolMask());
}

// the spell reaches `target` from where the bot stands (its range, the game's line of sight)
bool Reaches(Player* bot, SpellInfo const* info, Unit* target)
{
    return info && target && bot->GetMap() == target->GetMap() &&
           (bot == target ||
            (bot->IsWithinDistInMap(target, info->GetMaxRange(true, bot)) && bot->IsWithinLOSInMap(target)));
}

// a dead group member whose body lies on the leader's map, no resurrection offered yet
std::vector<Player*> DeadMembers(LeaderGroup& group, Map* map)
{
    std::vector<Player*> dead;
    for (ObjectGuid const& guid : group.MemberGuids())
        if (Player* member = ObjectAccessor::FindPlayer(guid);
            member && member->IsInWorld() && member->GetMap() == map && !member->IsAlive() &&
            member->getDeathState() == DeathState::Corpse && !member->isResurrectRequested())
            dead.push_back(member);
    // [lead406] user: «Поднимают первыми тех, у кого есть воскрешение. Не важно, танк или дд.» Every member that
    // can resurrect comes first, whatever its role (it then raises the rest in parallel); among them the one whose
    // mana pays for the most further resurrections (its max mana over its own resurrection's cost); then the others
    std::vector<std::pair<Player*, float>> ranked;
    for (Player* member : dead)
    {
        float rezzes = -1.0f;  // no resurrection of its own
        for (uint32 first : RESURRECT_SPELLS)
            if (uint32 const rank = KnownRank(member, first))
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(rank);
                int32 const cost = info ? info->CalcPowerCost(member, info->GetSchoolMask()) : 0;
                float const pool = float(member->GetMaxPower(POWER_MANA));
                rezzes = cost > 0 ? pool / float(cost) : std::max(pool, 1.0f);
                break;
            }
        ranked.emplace_back(member, rezzes);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](auto const& a, auto const& b) { return a.second > b.second; });
    dead.clear();
    for (auto const& [member, rezzes] : ranked)
        dead.push_back(member);
    return dead;
}

std::vector<Player*> Everyone(std::vector<Player*> const& seats, Player* tank)
{
    std::vector<Player*> all;
    for (Player* bot : seats)
        if (bot->IsAlive())
            all.push_back(bot);
    if (tank && tank->IsAlive())
        all.push_back(tank);
    return all;
}

// the member's damage taken per second in this fight (DamageTaken since the fight's start); 0: none measured
float TakenPerSec(DungeonLeaderState::Support const& st, Player* bot, uint32 now)
{
    if (!st.fightStartMs)
        return 0.0f;
    float const sec = getMSTimeDiff(st.fightStartMs, now) / 1000.0f;
    auto const start = st.takenAtStart.find(bot->GetGUID());
    if (sec <= 0.0f || start == st.takenAtStart.end())
        return 0.0f;
    uint64 const taken = DamageTaken::Total(bot->GetGUID());
    return taken > start->second ? float(taken - start->second) / sec : 0.0f;
}

// [lead400] the healer has a global cooldown to spare by its heals: every member outlives the dispel's global
// cooldown plus the healer's gap between two heals at its measured damage taken rate
bool HealerSpares(Player* healer, Player* tank, std::vector<Player*> const& members,
                  DungeonLeaderState::Support const& st, SpellInfo const* dispel, uint32 now, std::string& why)
{
    float const gcdSec = (dispel ? dispel->StartRecoveryTime : 0) / 1000.0f;
    RestState::HealerFacts const facts = RestState::HealerThroughput(healer, tank ? tank : healer);
    for (Player* member : members)
    {
        float const dtps = TakenPerSec(st, member, now);
        float const needs = dtps * (gcdSec + facts.gapSec);
        if (dtps > 0.0f && float(member->GetHealth()) <= needs)
        {
            why = Acore::StringFormat(
                "{} would not outlive the GCD and the heal gap ({:.0f} health < {:.0f} dtps x "
                "{:.1f} s)",
                member->GetName(), float(member->GetHealth()), dtps, gcdSec + facts.gapSec);
            return false;
        }
    }
    why = Acore::StringFormat("the healer has a GCD to spare (heal gap {:.1f} s)", facts.gapSec);
    return true;
}

char const* DispelTypeName(uint32 type)
{
    switch (type)
    {
        case DISPEL_MAGIC:
            return "magic";
        case DISPEL_CURSE:
            return "curse";
        case DISPEL_DISEASE:
            return "disease";
        case DISPEL_POISON:
            return "poison";
        default:
            return "other";
    }
}

// [lead400] Dispels: every harmful dispellable aura on a member, one dispeller per debuff (kept while it can),
// the healer first when it has a GCD to spare, else a member that knows a matching dispel; one order per member
void TickDispels(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, Player* healer, bool fighting,
                 LogFn const& logEvent, uint32 now)
{
    DungeonLeaderState::Support& st = group.State().support;
    std::vector<Player*> const members = Everyone(seats, tank);
    // the reports of a refused dispel: that dispeller is not picked for that debuff again
    for (auto const& [key, dispeller] : st.dispellers)
        if (Player* bot = ObjectAccessor::FindPlayer(dispeller))
        {
            Leader::MemberEvents events;
            std::string failed;
            if (Leader::Events::Get(dispeller, events) && events.dispelSpell &&
                Leader::Events::TakeDispelFailed(dispeller, events.dispelSpell, failed))
            {
                st.dispelFailed.insert({dispeller, key.second});
                Decisions().Write(group.Run(), "leader", "dispel_dropped", dispeller, bot->GetName(), true,
                                  JsonObject()
                                      .Str("member", bot->GetName())
                                      .Num("debuff", key.second)
                                      .Str("why", "the member reports: " + failed));
            }
        }
    std::set<ObjectGuid> busy;
    std::set<std::pair<ObjectGuid, uint32>> seen;
    std::string healerWhy;
    int8 healerSpares = -1;  // computed once per tick, when first needed
    for (Player* target : members)
    {
        Unit::VisibleAuraMap const* visible = target->GetVisibleAuras();
        if (!visible)
            continue;
        for (auto const& [slot, app] : *visible)
        {
            Aura* aura = app ? app->GetBase() : nullptr;
            SpellInfo const* auraInfo = aura ? aura->GetSpellInfo() : nullptr;
            if (!auraInfo || aura->IsPassive() || app->IsPositive() || auraInfo->Dispel < DISPEL_MAGIC ||
                auraInfo->Dispel > DISPEL_POISON)
                continue;
            std::pair<ObjectGuid, uint32> const key{target->GetGUID(), auraInfo->Id};
            if (!seen.insert(key).second)
                continue;
            // [lead630] b218 rfc-h 5C Bazzalan (1082.7-1110.6 s): Abolish Poison told on the tank's Deadly Poison 7
            // times in 28 s (casts 2.5-7.8 s apart, 13 % base mana each) while the last one still ran - its aura
            // (2893: a 12 s periodic trigger every 3 s of 3137, a poison dispel) removes the poison by itself, the
            // balance druid ran dry in that fight. An aura on the member whose periodic trigger dispels this type
            // (spell data: Abolish Poison / Abolish Disease) is the dispel already under way: no new order
            bool abolishing = false;
            for (auto const& [otherSlot, otherApp] : *visible)
            {
                Aura const* other = otherApp ? otherApp->GetBase() : nullptr;
                SpellInfo const* otherInfo = other ? other->GetSpellInfo() : nullptr;
                if (!otherInfo || other->IsRemoved())
                    continue;
                for (SpellEffectInfo const& effect : otherInfo->GetEffects())
                    if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && effect.TriggerSpell &&
                        (DispelMask(sSpellMgr->GetSpellInfo(effect.TriggerSpell)) & (1u << auraInfo->Dispel)))
                        abolishing = true;
            }
            if (abolishing)
                continue;
            // the spell `caster` removes this debuff with now (0: none): known, off cooldown, its power, its reach,
            // and the game's own dispel list has the aura for it
            auto const dispelWith = [&](Player* caster) -> uint32
            {
                if (!caster || !caster->IsAlive() || busy.count(caster->GetGUID()) ||
                    st.dispelFailed.count({caster->GetGUID(), auraInfo->Id}))
                    return 0;
                if (!fighting && (caster->IsSitState() || caster->IsInCombat()))
                    return 0;  // out of a fight a sitter rests on (the leader's Rest)
                for (uint32 first : DISPEL_SPELLS)
                {
                    uint32 const rank = KnownRank(caster, first);
                    SpellInfo const* info = rank ? sSpellMgr->GetSpellInfo(rank) : nullptr;
                    uint32 const mask = DispelMask(info);
                    if (!info || !(mask & (1u << auraInfo->Dispel)) || caster->HasSpellCooldown(rank) ||
                        !Affords(caster, info) || !Reaches(caster, info, target))
                        continue;
                    DispelChargesList dispellable;
                    target->GetDispellableAuraList(caster, mask, dispellable, info);
                    for (auto const& [listed, charges] : dispellable)
                        if (listed == aura)
                            return rank;
                }
                return 0;
            };
            // in a fight the tank's own debuffs are its own (it owns its fight, lead56) when it removes the type
            if (fighting && target == tank && dispelWith(tank))
                continue;
            Player* picked = nullptr;
            uint32 spell = 0;
            std::string why;
            if (auto const kept = st.dispellers.find(key); kept != st.dispellers.end())
                if (Player* bot = ObjectAccessor::FindPlayer(kept->second); bot && (bot != tank || !fighting))
                    if ((spell = dispelWith(bot)))
                    {
                        picked = bot;
                        why = "kept: it removes it";
                    }
            if (!picked && healer && (spell = dispelWith(healer)))
            {
                if (fighting && healerSpares < 0)
                    healerSpares =
                        HealerSpares(healer, tank, members, st, sSpellMgr->GetSpellInfo(spell), now, healerWhy) ? 1 : 0;
                if (!fighting || healerSpares == 1)
                {
                    picked = healer;
                    why = fighting ? healerWhy : "the healer, out of a fight";
                }
            }
            if (!picked)
                for (Player* bot : seats)
                    if (bot != healer && (spell = dispelWith(bot)))
                    {
                        picked = bot;
                        why = healer ? "a member that removes it: " + (healerWhy.empty() ? std::string("the healer "
                                                                                                       "cannot")
                                                                                         : healerWhy)
                                     : "a member that removes it: no healer";
                        break;
                    }
            if (!picked && !fighting && tank && (spell = dispelWith(tank)))
            {
                picked = tank;
                why = "the tank, out of a fight";
            }
            if (!picked)
            {
                if (st.dispelNoneLogged.insert(key).second)
                    Decisions().Write(group.Run(), "leader", "dispel_none", target->GetGUID(), target->GetName(), false,
                                      JsonObject()
                                          .Str("member", target->GetName())
                                          .Str("debuff", auraInfo->SpellName[0])
                                          .Num("debuffId", auraInfo->Id)
                                          .Str("type", DispelTypeName(auraInfo->Dispel))
                                          .Str("why",
                                               "no member removes it now (spell, cooldown, power, range, "
                                               "sight, a healer GCD to spare)"));
                continue;
            }
            st.dispelNoneLogged.erase(key);
            busy.insert(picked->GetGUID());
            st.dispellers[key] = picked->GetGUID();
            if (Leader::Events::SetDispel(picked->GetGUID(), spell, target->GetGUID()))
                logEvent(picked, "Dispel",
                         JsonObject()
                             .Num("spell", spell)
                             .Str("on", target->GetName())
                             .Str("debuff", auraInfo->SpellName[0])
                             .Num("debuffId", auraInfo->Id)
                             .Str("type", DispelTypeName(auraInfo->Dispel))
                             .Str("why", why));
        }
    }
    // a debuff gone ends its pick and its refusals
    for (auto it = st.dispellers.begin(); it != st.dispellers.end();)
        it = seen.count(it->first) ? std::next(it) : st.dispellers.erase(it);
    for (auto it = st.dispelNoneLogged.begin(); it != st.dispelNoneLogged.end();)
        it = seen.count(*it) ? std::next(it) : st.dispelNoneLogged.erase(it);
    for (auto it = st.dispelFailed.begin(); it != st.dispelFailed.end();)
    {
        bool still = false;
        for (auto const& key : seen)
            still |= key.second == it->second;
        it = still ? std::next(it) : st.dispelFailed.erase(it);
    }
}

// [lead401] Out of a fight a dead member is raised at once: one resurrecter per dead member (the healer first),
// kept while it can; its walk into range and sight is the External's prerequisite
void TickResurrect(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, Player* healer, bool fighting,
                   LogFn const& logEvent)
{
    DungeonLeaderState::Support& st = group.State().support;
    if (fighting || !tank)
    {
        st.resurrecters.clear();
        return;
    }
    std::vector<Player*> const dead = DeadMembers(group, tank->GetMap());
    std::set<ObjectGuid> deadNow;
    for (Player* body : dead)
        deadNow.insert(body->GetGUID());
    // a member up again: its picks, refusals and log end
    for (auto it = st.resFailed.begin(); it != st.resFailed.end();)
        it = deadNow.count(it->second) ? std::next(it) : st.resFailed.erase(it);
    for (auto it = st.resNoneLogged.begin(); it != st.resNoneLogged.end();)
        it = deadNow.count(*it) ? std::next(it) : st.resNoneLogged.erase(it);
    std::map<ObjectGuid, ObjectGuid> kept;
    for (auto const& [deadGuid, caster] : st.resurrecters)
        if (deadNow.count(deadGuid))
            kept[deadGuid] = caster;
    st.resurrecters.clear();

    std::vector<Player*> candidates;
    if (healer)
        candidates.push_back(healer);
    for (Player* bot : seats)
        if (bot != healer)
            candidates.push_back(bot);
    candidates.push_back(tank);
    std::set<ObjectGuid> busy;
    bool first = true;
    for (Player* body : dead)
    {
        auto const spellOf = [&](Player* caster) -> uint32
        {
            if (!caster || !caster->IsAlive() || caster->IsInCombat() || busy.count(caster->GetGUID()) ||
                st.resFailed.count({caster->GetGUID(), body->GetGUID()}))
                return 0;
            for (uint32 firstRank : RESURRECT_SPELLS)
                if (uint32 const rank = KnownRank(caster, firstRank))
                {
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(rank);
                    return info && !caster->HasSpellCooldown(rank) && Affords(caster, info) ? rank : 0;
                }
            return 0;
        };
        Player* picked = nullptr;
        uint32 spell = 0;
        if (auto const was = kept.find(body->GetGUID()); was != kept.end())
            if (Player* bot = ObjectAccessor::FindPlayer(was->second); bot && (spell = spellOf(bot)))
                picked = bot;
        for (Player* bot : candidates)
            if (!picked && (spell = spellOf(bot)))
                picked = bot;
        if (!picked)
        {
            if (st.resNoneLogged.insert(body->GetGUID()).second)
                Decisions().Write(group.Run(), "leader", "resurrect_none", body->GetGUID(), body->GetName(), false,
                                  JsonObject()
                                      .Str("dead", body->GetName())
                                      .Str("why", "no member out of combat knows a resurrection it has the mana for"));
            continue;
        }
        st.resNoneLogged.erase(body->GetGUID());
        busy.insert(picked->GetGUID());
        // [lead272] a resurrecter that reports it cannot (its cast check, no walk into range and sight): dropped
        if (std::string failed; Leader::Events::TakeExternalFailed(picked->GetGUID(), spell, failed))
        {
            st.resFailed.insert({picked->GetGUID(), body->GetGUID()});
            Leader::Events::SetExternal(picked->GetGUID(), 0, ObjectGuid::Empty);
            Decisions().Write(group.Run(), "leader", "resurrect_dropped", picked->GetGUID(), picked->GetName(), true,
                              JsonObject()
                                  .Str("caster", picked->GetName())
                                  .Str("dead", body->GetName())
                                  .Str("why", "the member reports: " + failed));
            continue;
        }
        st.resurrecters[body->GetGUID()] = picked->GetGUID();
        // the walk's corpse ("party member to resurrect") is the External's
        if (first)
            RaidLeader::SetResurrectFirst(picked, body->GetGUID());
        first = false;
        if (Leader::Events::SetExternal(picked->GetGUID(), spell, body->GetGUID(),
                                        Leader::EXTERNAL_PRECEDENCE_FIGHT_SAVE))
            logEvent(picked, "External",
                     JsonObject()
                         .Num("spell", spell)
                         .Str("on", body->GetName())
                         .Str("why", "resurrect: a dead member out of a fight, at once"));
    }
}

// [lead884] b224 rfc-h 5B 1000.2-1001.7 s: the shadow priest's Runic Mana Injector (in the fight at 0 % mana) put
// ~1050 threat on Jergosh the Invoker, who had it on his threat list and the tank at 156 (he had just run in): the game
// splits half the mana an energize gives, unmodified, over every mob that has the member on its threat list and is
// not crowd-controlled (Unit::EnergizeBySpell -> ThreatManager::ForwardThreatForAssistingMe). The mob where that share
// takes the member past the share the game moves it at over the tank's threat (110 % in its melee, 130 % out of it,
// a mob on another member at 100 %); empty: none
std::string ManaSplitOvertakes(Player* bot, Player* tank, uint32 mana)
{
    std::vector<Creature*> listing;
    for (auto const& [guid, ref] : bot->GetThreatMgr().GetThreatenedByMeList())
        if (Creature* mob = ref ? ref->GetOwner() : nullptr;
            mob && mob->IsAlive() && mob->IsInCombat() && !mob->HasUnitState(UNIT_STATE_CONTROLLED))
            listing.push_back(mob);
    if (listing.empty() || !tank)
        return std::string();
    float const perMob = mana / 2.0f / float(listing.size());
    for (Creature* mob : listing)
    {
        if (mob->GetVictim() == bot)
            continue;  // already on it: the aggro loss's, not this potion's
        float const mine = mob->GetThreatMgr().GetThreat(bot) + perMob;
        float const tankThreat = mob->GetThreatMgr().GetThreat(tank);
        float const share = mob->GetVictim() != tank      ? 1.0f
                            : mob->IsWithinMeleeRange(bot) ? 1.1f
                                                           : 1.3f;
        if (mine >= share * tankThreat)
            return Acore::StringFormat("its split threat on {} ({:.0f} + {:.0f}) overtakes the tank's {:.0f} x {:.1f}",
                                       mob->GetName(), mob->GetThreatMgr().GetThreat(bot), perMob, tankThreat, share);
    }
    return std::string();
}

// [lead402] The pre-pull potion at the boss's pull (wowsims prepots at the pull): once the leader's pull order on
// a boss opens, each member out of combat uses its best buff potion for its role, so the buff runs from the pull
// on; in a fight the mana / health potion when the leader's estimate needs it
void TickPotions(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, Player* healer, bool fighting,
                 LogFn const& logEvent, uint32 now)
{
    DungeonLeaderState& lead = group.State();
    DungeonLeaderState::Support& st = lead.support;
    std::vector<Player*> const members = Everyone(seats, tank);
    Map* map = tank->GetMap();

    // the measured damage taken over this fight
    if (!fighting)
    {
        st.fightStartMs = 0;
        st.takenAtStart.clear();
    }
    else if (!st.fightStartMs)
    {
        st.fightStartMs = now;
        for (Player* bot : members)
            st.takenAtStart[bot->GetGUID()] = DamageTaken::Total(bot->GetGUID());
    }

    Leader::PullOrder order;
    Creature* boss = nullptr;
    if (!fighting && Leader::Orders::GetOpen(group.LeaderGuid(), order) &&
        order.state != Leader::OrderState::Fighting && !order.target.IsEmpty())
        if (Creature* mob = map->GetCreature(order.target);
            mob && mob->IsAlive() && !mob->IsInCombat() && (mob->IsDungeonBoss() || mob->isWorldBoss()))
            boss = mob;
    if (boss)
    {
        if (st.prepotBoss != boss->GetGUID())
        {
            st.prepotBoss = boss->GetGUID();
            st.prepotDone.clear();
        }
        for (Player* bot : members)
        {
            if (bot->IsInCombat() || st.prepotDone.count(bot->GetGUID()))
                continue;
            uint32 spell = 0;
            uint32 const item = RaidConsumables::FindBuffPotion(bot, spell);
            std::string failed;
            // [lead520] the fight needs its in-fight mana potion (one potion per combat, the pre-pot's cooldown)
            bool const keepsManaPotion = item && !bot->HasAura(spell) &&
                                         PullReadiness::MemberNeedsManaPotion(bot, boss);
            char const* done = !item                 ? "no buff potion for its role it can use now"
                               : bot->HasAura(spell) ? "used: its buff is on"
                               : keepsManaPotion
                                   ? "kept for the in-fight mana potion: its mana is under the boss line without it"
                               : Leader::Events::TakeConsumeFailed(bot->GetGUID(), item, failed) ? "refused"
                                                                                                 : nullptr;
            if (done)
            {
                st.prepotDone.insert(bot->GetGUID());
                Decisions().Write(
                    group.Run(), "leader", "prepot_end", bot->GetGUID(), bot->GetName(), false,
                    JsonObject()
                        .Str("member", bot->GetName())
                        .Str("boss", boss->GetName())
                        .Str("why", failed.empty() ? std::string(done) : "the member reports: " + failed));
                continue;
            }
            if (Leader::Events::SetConsume(bot->GetGUID(), item, ObjectGuid::Empty, ObjectGuid::Empty,
                                           "pre-pull potion before " + boss->GetName()))
                logEvent(bot, "Consume",
                         JsonObject()
                             .Num("item", item)
                             .Num("buff", spell)
                             .Str("boss", boss->GetName())
                             .Str("why", "pre-pull potion: the boss pull opens (" +
                                             std::string(Leader::MethodName(order.method)) + ")"));
        }
    }
    // [lead524] wowsims assassination: the rogue is in Stealth at the pull so its first ability out of it starts
    // Overkill (the talent's aura, 20 s of energy regen). The boss pull opening is its stealth's moment, like the
    // pre-pull potion; out of combat and Stealth ready (the game's checks are the cast's own)
    using Leader::Prepull::SPELL_STEALTH;
    // [lead773] the rogue told at the boss pull's gate (StealthHolds): its order stands while that boss is unpulled
    // and the rogue can stealth; a refusal ends it (the gate stops waiting)
    auto& stealthPlans = lead.stealthPrepull;
    auto const gateOrdered = [&](Player* bot) -> Creature*
    {
        auto const plan = stealthPlans.find(bot->GetGUID());
        if (plan == stealthPlans.end() || !plan->second.ordered || plan->second.done)
            return nullptr;
        Creature* planBoss = map->GetCreature(plan->second.boss);
        return planBoss && planBoss->IsAlive() && !planBoss->IsInCombat() ? planBoss : nullptr;
    };
    // the order is done once the rogue is in Stealth or fighting (its slot is free for the next External)
    for (Player* bot : members)
    {
        if (bot->getClass() != CLASS_ROGUE)
            continue;
        if (std::string failed; Leader::Events::TakeExternalFailed(bot->GetGUID(), SPELL_STEALTH, failed))
        {
            if (auto const plan = stealthPlans.find(bot->GetGUID()); plan != stealthPlans.end())
                plan->second.done = true;
            Decisions().Write(group.Run(), "leader", "stealth_prepull_dropped", bot->GetGUID(), bot->GetName(),
                              false,
                              JsonObject().Str("rogue", bot->GetName()).Str("why", "the member reports: " + failed));
        }
        Leader::MemberEvents events;
        if (Leader::Events::Get(bot->GetGUID(), events) && events.externalSpell == SPELL_STEALTH &&
            (bot->HasAura(SPELL_STEALTH) || bot->IsInCombat() || (!boss && !gateOrdered(bot))))
            Leader::Events::SetExternal(bot->GetGUID(), 0, ObjectGuid::Empty);
    }
    for (Player* bot : members)
    {
        Creature* stealthFor = boss ? boss : gateOrdered(bot);
        auto const plan = stealthPlans.find(bot->GetGUID());
        bool const doneFor = stealthFor && plan != stealthPlans.end() && plan->second.done &&
                             plan->second.boss == stealthFor->GetGUID();  // refused for this boss: no new order
        if (stealthFor && !doneFor && bot->IsAlive() && !bot->IsInCombat() && Leader::Prepull::OverkillStealther(bot) &&
            !bot->HasAura(SPELL_STEALTH) && !bot->HasSpellCooldown(SPELL_STEALTH) &&
            Leader::Events::SetExternal(bot->GetGUID(), SPELL_STEALTH, bot->GetGUID()))
            logEvent(bot, "External",
                     JsonObject()
                         .Num("spell", SPELL_STEALTH)
                         .Str("on", bot->GetName())
                         .Str("boss", stealthFor->GetName())
                         .Str("why", boss ? "pre-pull Stealth for Overkill: the boss pull opens"
                                          : "pre-pull Stealth for Overkill: the boss pull waits for it"));
    }
    // [lead774] wowsims feral (PrePopBerserk): the cat's Berserk goes out 1 s before the boss pull, its 15 s from the
    // first hit on (b220 rfc-h 5H Rtdruaf: Berserk in every boss fight, each one only after Rip was up - the in-fight
    // berserkNow). Once the boss's pull order is open, a feral cat out of combat in Cat Form with Berserk ready casts
    // it when the tank reaches the boss's aggro reach within that second (its walk over its run speed; a pull from
    // range: at once). Kept for a later boss when the run's measured stretch between bosses is shorter than its
    // recharge (the gain plan's own measure)
    constexpr uint32 SPELL_BERSERK_DRUID = 50334;
    constexpr uint32 SPELL_CAT_FORM = 768;
    constexpr float PREPOP_BERSERK_SEC = 1.0f;  // wowsims feral: Berserk at -1 s
    if (boss)
    {
        if (st.prepopBoss != boss->GetGUID())
        {
            st.prepopBoss = boss->GetGUID();
            st.prepopDone.clear();
        }
        float const speed = std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
        // [lead791] b221 rfc-h: 0 pre-pull Berserks in 8 runs - every boss was an engage from the tank's opener
        // range (5H Taragaman 689.7 s: the cat in Cat Form, Berserk ready, the tank's opener pulled him from range;
        // Berserk only at 690.4 s in the fight). The pull lands where the tank's first ready engage opener reaches him
        // (TankEngageOpeners, spell data range, in sight), or at his aggro reach, whichever comes first
        float pullReach = boss->GetAggroRange(tank);
        if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank); tankAI && tank->IsWithinLOSInMap(boss))
            for (char const* opener : PullPlanning::TankEngageOpeners(tank, true))
                if (uint32 const spell = tankAI->GetAiObjectContext()->GetValue<uint32>("spell id", opener)->Get();
                    spell && tank->HasSpell(spell) && !tank->HasSpellCooldown(spell))
                {
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                        pullReach = std::max(pullReach, info->GetMaxRange(false) + tank->GetCombatReach() +
                                                            boss->GetCombatReach());
                    break;
                }
        float const tankEta =
            std::max(0.0f, Leader::WalkYd(tank, boss->GetPosition()) - pullReach) / speed;  // [lead804]
        bool laterBoss = false;
        for (ObjectGuid const& bossGuid : group.BossCreatures())
            if (Creature* other = bossGuid == boss->GetGUID() ? nullptr : map->GetCreature(bossGuid);
                other && other->IsAlive())
                laterBoss = true;
        for (Player* bot : members)
        {
            if (bot->getClass() != CLASS_DRUID || st.prepopDone.count(bot->GetGUID()))
                continue;
            if (std::string failed; Leader::Events::TakeExternalFailed(bot->GetGUID(), SPELL_BERSERK_DRUID, failed))
            {
                st.prepopDone.insert(bot->GetGUID());
                continue;
            }
            if (PlayerbotAI::IsTank(bot) || AiFactory::GetPlayerSpecTab(bot) != DRUID_TAB_FERAL ||
                !bot->IsAlive() || bot->IsInCombat() || !bot->HasSpell(SPELL_BERSERK_DRUID) ||
                bot->HasSpellCooldown(SPELL_BERSERK_DRUID) || !bot->HasAura(SPELL_CAT_FORM) ||
                tankEta > PREPOP_BERSERK_SEC)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_BERSERK_DRUID);
            int32 rechargeMs = info ? int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime)) : 0;
            bot->ApplySpellMod(SPELL_BERSERK_DRUID, SPELLMOD_COOLDOWN, rechargeMs);
            if (laterBoss && lead.stretches &&
                lead.stretchSecSum / float(lead.stretches) < std::max(rechargeMs, 0) / 1000.0f)
                continue;
            if (Leader::Events::SetExternal(bot->GetGUID(), SPELL_BERSERK_DRUID, bot->GetGUID()))
                logEvent(bot, "External",
                         JsonObject()
                             .Num("spell", SPELL_BERSERK_DRUID)
                             .Str("on", bot->GetName())
                             .Str("boss", boss->GetName())
                             .Real("tankEtaSec", tankEta)
                             .Str("why", "pre-pull Berserk (wowsims feral -1 s): the boss pull opens"));
        }
    }
    // the order is done once Berserk is cast or the cat fights (its slot is free for the next External)
    for (Player* bot : members)
        if (Leader::MemberEvents events; bot->getClass() == CLASS_DRUID &&
                                         Leader::Events::Get(bot->GetGUID(), events) &&
                                         events.externalSpell == SPELL_BERSERK_DRUID &&
                                         (bot->HasSpellCooldown(SPELL_BERSERK_DRUID) || bot->IsInCombat() || !boss))
        {
            st.prepopDone.insert(bot->GetGUID());
            Leader::Events::SetExternal(bot->GetGUID(), 0, ObjectGuid::Empty);
        }
    if (!fighting)
        return;

    bool bossFight = false;
    for (Player* bot : members)
        for (Unit* attacker : bot->getAttackers())
            if (Creature const* mob = attacker ? attacker->ToCreature() : nullptr;
                mob && (mob->IsDungeonBoss() || mob->isWorldBoss()))
                bossFight = true;
    for (Player* bot : members)
    {
        if (!bot->IsInCombat())
            continue;
        std::string verdict;
        uint32 item = 0;
        uint32 amount = 0;
        // health first: it would die before the healer's next heal lands (or, with no healer, before the fight ends)
        if (uint32 const potion = RaidConsumables::FindRestorePotion(bot, false, amount);
            potion && bot->GetMaxHealth() - bot->GetHealth() >= amount)
        {
            float const dtps = TakenPerSec(st, bot, now);
            float const gapSec =
                healer && healer != bot ? RestState::HealerThroughput(healer, bot).gapSec : lead.fightSecLeft;
            if (dtps > 0.0f && float(bot->GetHealth()) <= dtps * gapSec)
            {
                item = potion;
                verdict = Acore::StringFormat(
                    "health potion: {:.0f} health < {:.0f} dtps x {:.1f} s ({})", float(bot->GetHealth()), dtps, gapSec,
                    healer && healer != bot ? "the healer's gap between heals" : "the fight left, no healer");
            }
        }
        // [lead840] b223 rfc-h 5C Taragaman 671.8-676.7 s: the fire mage's Evocation (the boss line's 60 %) ran 2 ->
        // 33 % and the leader's mana potion verdict ("31 % < the 97 % ... - 0 cooldowns": the channel's return still
        // to come is no cooldown any more) put the Runic Mana Injector on it - the item's cast broke the channel,
        // ~30 % of the counted 60 % never came. One owner per fact: while a mana tool's channel runs, its return is
        // the member's mana; the potion and the gem wait for its end (a health potion does not)
        // [lead850] b223 rfc-h 5D Oggleflint 275.1-276.6 s: Evocation was told at 24 %, the potion verdict ("24 % <
        // the 90 % ... - 0 cooldowns") put the Runic Mana Potion on the mage before its channel ran: 24 + 25 + 60 %
        // and ~9 % over the pool's top. A told Evocation is the member's next mana too (one order per fact, lead752)
        Leader::MemberEvents pending;
        bool const evocationTold = Leader::Events::Get(bot->GetGUID(), pending) &&
                                   pending.externalSpell == RestState::SPELL_EVOCATION;
        uint32 const maxMana =
            Leader::Fight::LeaderChannelsManaTool(bot) || evocationTold ? 0 : bot->GetMaxPower(POWER_MANA);
        if (!item && maxMana)
            if (uint32 const potion = RaidConsumables::FindRestorePotion(bot, true, amount);
                potion && maxMana - bot->GetPower(POWER_MANA) >= amount)
            {
                std::string lineWhy;
                // [lead482] the line without the potion this verdict decides on
                float const line =
                    RestState::FightManaLine(bot, lead.fightSecLeft, 0.0f, &lineWhy, bossFight, true, false);
                if (bot->GetPowerPct(POWER_MANA) < line)
                {
                    item = potion;
                    verdict = Acore::StringFormat(
                        "mana potion: {:.0f} % < the {:.0f} % the fight's {:.0f} s left "
                        "need ({})",
                        bot->GetPowerPct(POWER_MANA), line, lead.fightSecLeft, lineWhy);
                    // [lead884] its energize's split threat stays under the tank's on every mob listing it
                    if (std::string const overtakes = ManaSplitOvertakes(bot, tank, amount); !overtakes.empty())
                    {
                        item = 0;
                        verdict = "mana potion waits: " + overtakes;
                        thread_local std::map<uint64, std::string> waitLogged;
                        std::string const key = overtakes.substr(0, overtakes.find(" ("));
                        std::string& logged = waitLogged[bot->GetGUID().GetRawValue()];
                        if (logged != key)
                        {
                            logged = key;
                            LOG_INFO("playerbots", "[lead884] {}: {}", bot->GetName(), verdict);
                        }
                    }
                }
            }
        // [lead520] b211 rfc-h: the mages' own "high mana" trigger used the Mana Gem on trash under 65 % and it was on
        // its cooldown at the bosses. The gem is the leader's like the potion (the same need: the line without the
        // potion and the gem), held by the cooldown rule: in a boss fight, on trash only at a critical pull, with no
        // boss left, or when it is back before the next boss pull
        if (!item && maxMana)
            if (uint32 amount = 0; uint32 const gem = RaidConsumables::FindManaGem(bot, amount))
                if (maxMana - bot->GetPower(POWER_MANA) >= amount)
                {
                    std::string lineWhy;
                    float const line =
                        RestState::FightManaLine(bot, lead.fightSecLeft, 0.0f, &lineWhy, bossFight, true, false);
                    if (bot->GetPowerPct(POWER_MANA) < line)
                    {
                        float rechargeSec = 0.0f;
                        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(gem))
                            for (auto const& use : proto->Spells)
                                if (use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                                {
                                    rechargeSec = std::max({rechargeSec, float(std::max(use.SpellCooldown, 0)),
                                                            float(std::max(use.SpellCategoryCooldown, 0))}) /
                                                  1000.0f;
                                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(use.SpellId)))
                                        rechargeSec = std::max(
                                            rechargeSec,
                                            std::max(info->RecoveryTime, info->CategoryRecoveryTime) / 1000.0f);
                                }
                        std::string rule;
                        if (bossFight)
                            rule = "a boss fight";
                        else if (lead.fightOverload >= 1.0f)
                            rule = Acore::StringFormat("a critical pull: overload {:.2f}", lead.fightOverload);
                        else if (lead.bossPullSec < 0.0f)
                            rule = "trash, no boss left";
                        else if (lead.bossPullSec >= rechargeSec)
                            rule = Acore::StringFormat(
                                "trash: back before the pull of {} (in {:.0f} s, recharge {:.0f} s)", lead.nextBossName,
                                lead.bossPullSec, rechargeSec);
                        if (!rule.empty())
                            item = gem;
                        // [lead884] the gem's energize splits its threat like the potion's
                        if (std::string const overtakes = item ? ManaSplitOvertakes(bot, tank, amount) : "";
                            !overtakes.empty())
                        {
                            item = 0;
                            rule = "waits: " + overtakes;
                        }
                        verdict = Acore::StringFormat(
                            "mana gem {}: {:.0f} % < the {:.0f} % the fight's {:.0f} s left need ({}); {}",
                            item ? "used" : "held", bot->GetPowerPct(POWER_MANA), line, lead.fightSecLeft,
                            lineWhy,
                            rule.empty() ? Acore::StringFormat("held for {} (his pull in {:.0f} s, recharge {:.0f} s)",
                                                               lead.nextBossName, lead.bossPullSec, rechargeSec)
                                         : rule);
                    }
                }
        std::string& logged = st.potionLogged[bot->GetGUID()];
        if (item)
        {
            std::string failed;
            if (Leader::Events::TakeConsumeFailed(bot->GetGUID(), item, failed))
                verdict = "refused: " + failed;
            else if (Leader::Events::SetConsume(bot->GetGUID(), item, ObjectGuid::Empty, ObjectGuid::Empty, verdict))
                logEvent(bot, "Consume", JsonObject().Num("item", item).Str("why", verdict));
        }
        if (verdict != logged && !verdict.empty())
            Decisions().Write(group.Run(), "leader", "potion", bot->GetGUID(), bot->GetName(), item != 0,
                              JsonObject().Str("member", bot->GetName()).Str("verdict", verdict));
        logged = verdict;
    }
}

// the group's ritual objects round `at`: a Soulwell, or a portal of a ritual (Souls, Refreshment) one of `owners` made
GameObject* RitualObject(Player* at, std::vector<Player*> const& owners, bool well)
{
    std::list<GameObject*> objects;
    std::vector<uint32> const entries =
        well ? std::vector<uint32>{RaidConsumables::GO_SOULWELL_RANK_1,
                                   RaidConsumables::GO_SOULWELL_RANK_1_TALENT_1,
                                   RaidConsumables::GO_SOULWELL_RANK_1_TALENT_2,
                                   RaidConsumables::GO_SOULWELL_RANK_2,
                                   RaidConsumables::GO_SOULWELL_RANK_2_TALENT_1,
                                   RaidConsumables::GO_SOULWELL_RANK_2_TALENT_2}
             : std::vector<uint32>{RaidConsumables::GO_SOUL_PORTAL_RANK_1, RaidConsumables::GO_SOUL_PORTAL_RANK_2,
                                   RaidConsumables::GO_REFRESHMENT_PORTAL_RANK_1,
                                   RaidConsumables::GO_REFRESHMENT_PORTAL_RANK_2};
    at->GetGameObjectListWithEntryInGrid(objects, entries, RaidConsumables::SOULWELL_SEARCH_DISTANCE);
    for (GameObject* go : objects)
    {
        if (!go || !go->isSpawned())
            continue;
        for (Player* owner : owners)
            if (go->GetOwnerGUID() == owner->GetGUID())
                return go;
    }
    return nullptr;
}

bool HasSoulstoneAura(Player* bot)
{
    for (uint32 aura : SOULSTONE_AURAS)
        if (bot->HasAura(aura))
            return true;
    return false;
}

// [lead403] [lead405] The buff pass's support (while the pass runs; it ends only once this is done): the soulstone
// before a boss on the healer that raises the others (else the healer, else the tank); the Ritual of Souls when a
// member lacks a healthstone and no Soulwell stands; the two clicks a ritual's portal needs (the members nearest to
// it); the Soulwell for every member without a healthstone
void TickBuffPass(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, Player* healer, bool fighting,
                  LogFn const& logEvent)
{
    DungeonLeaderState::Support& st = group.State().support;
    GroupRouteState const& route = group.Route();
    std::string work;
    if (fighting || !route.needBuff)
    {
        st.buffWork.clear();
        st.wellClose.clear();
        return;
    }
    std::vector<Player*> const members = Everyone(seats, tank);
    std::vector<std::string> verdicts;

    // the soulstone, once per pass before a boss
    uint32 const pass = route.buffSinceMs ? route.buffSinceMs : 1;
    if (route.buffReason.rfind("before ", 0) == 0 && st.soulstonePass != pass)
    {
        bool stoned = false;
        for (Player* bot : members)
            stoned |= HasSoulstoneAura(bot);
        Player* warlock = nullptr;
        uint32 stone = 0;
        for (Player* bot : seats)
            for (uint32 item : SOULSTONE_ITEMS)
                if (!warlock && bot->IsAlive() && bot->HasItemCount(item, 1))
                {
                    warlock = bot;
                    stone = item;
                }
        Player* on = nullptr;
        for (Player* bot : members)
            if (!on && PlayerbotAI::IsHeal(bot) && RaidLeader::CanResurrect(bot))
                on = bot;
        if (!on)
            on = healer ? healer : tank;
        std::string failed;
        if (stoned || !warlock || !on)
            st.soulstonePass = pass;  // nothing to do for this pass
        else if (Leader::Events::TakeConsumeFailed(warlock->GetGUID(), stone, failed))
        {
            st.soulstonePass = pass;
            verdicts.push_back("soulstone dropped: " + failed);
        }
        else
        {
            if (Leader::Events::SetConsume(warlock->GetGUID(), stone, on->GetGUID(), ObjectGuid::Empty,
                                           "soulstone on " + on->GetName() + " " + route.buffReason))
                logEvent(warlock, "Consume",
                         JsonObject()
                             .Num("item", stone)
                             .Str("on", on->GetName())
                             .Str("why", "the soulstone before the boss on the member that raises the group"));
            work = "soulstone: " + warlock->GetName() + " on " + on->GetName();
        }
    }

    // the healthstones: Soulwell, the ritual's portal, the ritual
    std::vector<Player*> lacking;
    for (Player* bot : members)
        if (!bot->IsInCombat() && !bot->IsSitState() && !RaidConsumables::FindHealthstone(bot))
            lacking.push_back(bot);  // a sitter eats / drinks on (the pass's food step, the leader's Rest)
    GameObject* well = RitualObject(tank, members, true);
    GameObject* portal = well ? nullptr : RitualObject(tank, members, false);
    if (well)
    {
        std::set<ObjectGuid> close;
        std::string users;
        for (Player* bot : lacking)
        {
            std::pair<ObjectGuid, ObjectGuid> const tried{bot->GetGUID(), well->GetGUID()};
            if (st.wellTried.count(tried))
                continue;
            bool const inReach =
                bot->GetDistance(well) <= INTERACTION_DISTANCE && !bot->isMoving() && !bot->IsNonMeleeSpellCast(false);
            // it stood in reach last tick with the order and still has no stone: the well gives it nothing
            if (inReach && st.wellClose.count(bot->GetGUID()))
            {
                st.wellTried.insert(tried);
                verdicts.push_back(bot->GetName() + ": the Soulwell gives it no healthstone");
                continue;
            }
            if (inReach)
                close.insert(bot->GetGUID());
            if (Leader::Events::SetConsume(bot->GetGUID(), 0, ObjectGuid::Empty, well->GetGUID(),
                                           "a healthstone from the Soulwell"))
                logEvent(bot, "Consume",
                         JsonObject()
                             .Str("object", well->GetName())
                             .Num("entry", well->GetEntry())
                             .Str("why", "no healthstone: the Soulwell stands"));
            users += (users.empty() ? "" : ", ") + bot->GetName();
        }
        st.wellClose = close;
        if (!users.empty())
            work += (work.empty() ? "" : "; ") + std::string("Soulwell: ") + users;
    }
    else if (portal)
    {
        // the two members nearest to the portal that did not make it click it (each one's click is its channel)
        std::vector<Player*> clickers;
        for (Player* bot : members)
            if (bot->GetGUID() != portal->GetOwnerGUID() && !bot->IsInCombat() && !bot->IsSitState())
                clickers.push_back(bot);
        std::sort(clickers.begin(), clickers.end(),
                  [portal](Player* a, Player* b) { return a->GetDistance(portal) < b->GetDistance(portal); });
        if (clickers.size() > 2)
            clickers.resize(2);
        std::string names;
        for (Player* bot : clickers)
        {
            if (Leader::Events::SetConsume(bot->GetGUID(), 0, ObjectGuid::Empty, portal->GetGUID(),
                                           "the ritual's portal needs two clicks"))
                logEvent(bot, "Consume",
                         JsonObject()
                             .Str("object", portal->GetName())
                             .Num("entry", portal->GetEntry())
                             .Str("why", "the ritual's portal needs two more members"));
            names += (names.empty() ? "" : ", ") + bot->GetName();
        }
        if (!names.empty())
            work += (work.empty() ? "" : "; ") + portal->GetName() + ": " + names;
    }
    else if (!lacking.empty() && st.ritualPass != pass)
        for (Player* bot : seats)
        {
            if (!bot->IsAlive() || bot->IsInCombat() || bot->IsSitState())
                continue;
            uint32 ritual = 0;
            for (uint32 spell : RITUAL_OF_SOULS)
                if (bot->HasSpell(spell))
                    ritual = spell;
            SpellInfo const* info = ritual ? sSpellMgr->GetSpellInfo(ritual) : nullptr;
            if (!info || bot->HasSpellCooldown(ritual) || !Affords(bot, info))
                continue;
            bool reagents = bot->CanNoReagentCast(info);
            if (!reagents)
            {
                reagents = true;
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (info->Reagent[i] > 0 && !bot->HasItemCount(uint32(info->Reagent[i]), info->ReagentCount[i]))
                        reagents = false;
            }
            if (!reagents)
                continue;
            std::string failed;
            if (Leader::Events::TakeExternalFailed(bot->GetGUID(), ritual, failed))
            {
                verdicts.push_back("Ritual of Souls dropped: " + failed);
                st.ritualPass = pass;
                break;
            }
            if (Leader::Events::SetExternal(bot->GetGUID(), ritual, bot->GetGUID()))
                logEvent(bot, "External",
                         JsonObject()
                             .Num("spell", ritual)
                             .Str("on", bot->GetName())
                             .Str("why", Acore::StringFormat("Ritual of Souls: {} member(s) without a healthstone",
                                                             lacking.size())));
            work += (work.empty() ? "" : "; ") + bot->GetName() + " Ritual of Souls";
            break;
        }
    st.buffWork = work;
    std::string all = work + (verdicts.empty() ? "" : " | ");
    for (std::string const& verdict : verdicts)
        all += verdict + "; ";
    if (all != st.supportLogged)
    {
        st.supportLogged = all;
        if (!all.empty())
            Decisions().Write(group.Run(), "leader", "buff_support", tank->GetGUID(), tank->GetName(), false,
                              JsonObject().Str("reason", route.buffReason).Str("state", all));
    }
}
}  // namespace

void Tick(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, Player* healer, bool fighting,
          LogFn const& logEvent)
{
    if (!tank || !tank->IsAlive())
        return;
    uint32 const now = getMSTime() ? getMSTime() : 1;
    TickPotions(group, tank, seats, healer, fighting, logEvent, now);
    TickDispels(group, tank, seats, healer, fighting, logEvent, now);
    TickResurrect(group, tank, seats, healer, fighting, logEvent);
    TickBuffPass(group, tank, seats, healer, fighting, logEvent);
}

bool PickBattleRes(LeaderGroup& group, Player* tank, std::vector<Player*> const& seats, Player* healer,
                   std::set<ObjectGuid> const& told, BattleRes& out)
{
    if (!tank)
        return false;
    for (Player* dead : DeadMembers(group, tank->GetMap()))
        for (Player* bot : seats)
        {
            if (!bot->IsAlive() || told.count(bot->GetGUID()))
                continue;
            // Rebirth (every rank: the ready one known), Raise Ally after it
            uint32 spell = Leader::Fight::LeaderReadySpell(bot, {20484, 20739, 20742, 20747, 20748, 26994, 48477});
            if (!spell)
                spell = Leader::Fight::LeaderReadySpell(bot, {SPELL_RAISE_ALLY});
            SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
            if (!info || !Affords(bot, info) || !Reaches(bot, info, dead))
                continue;
            out.caster = bot;
            out.spell = spell;
            out.dead = dead;
            out.needed = PlayerbotAI::IsHeal(dead) && !healer;
            out.why = out.needed ? "the only healer is dead" : std::string();
            return true;
        }
    return false;
}

std::string BuffPassWork(LeaderGroup& group) { return group.State().support.buffWork; }

float MemberTakenPerSec(LeaderGroup& group, Player* member)
{
    return member ? TakenPerSec(group.State().support, member, getMSTime() ? getMSTime() : 1) : 0.0f;
}
}  // namespace Leader::Support
