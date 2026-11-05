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
// [lead931] `gainer`: the one the threat goes to (the energize's caster: the druid of an Innervate on another member,
// AuraEffect::HandlePeriodicEnergizeAuraTick); nullptr: `bot` itself (a potion, a gem)
std::string ManaSplitOvertakes(Player* bot, Player* tank, uint32 mana, Player* gainer = nullptr)
{
    if (!gainer)
        gainer = bot;
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
        if (mob->GetVictim() == gainer)
            continue;  // already on it: the aggro loss's, not this potion's
        float const mine = mob->GetThreatMgr().GetThreat(gainer) + perMob;
        float const tankThreat = mob->GetThreatMgr().GetThreat(tank);
        float const share = mob->GetVictim() != tank         ? 1.0f
                            : mob->IsWithinMeleeRange(gainer) ? 1.1f
                                                              : 1.3f;
        if (mine >= share * tankThreat)
            return Acore::StringFormat("its split threat on {} ({:.0f} + {:.0f}) overtakes the tank's {:.0f} x {:.1f}",
                                       mob->GetName(), mob->GetThreatMgr().GetThreat(gainer), perMob, tankThreat,
                                       share);
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
        // [lead903] [lead904] the cooldown rule the mana gem and the mana potion are held by: in a boss fight, on
        // trash only at a critical moment (a critical pull, a healer out of mana), with no boss left, or when the item
        // is back (`backSec`) before the next boss pull; empty: held. A potion drunk in combat starts its cooldown
        // only once the fight ends (Player::UpdatePotionCooldown): its back time counts the fight's seconds left
        auto const itemRechargeSec = [](uint32 entry)
        {
            float rechargeSec = 0.0f;
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
                for (auto const& use : proto->Spells)
                    if (use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                    {
                        rechargeSec = std::max({rechargeSec, float(std::max(use.SpellCooldown, 0)),
                                                float(std::max(use.SpellCategoryCooldown, 0))}) /
                                      1000.0f;
                        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(use.SpellId)))
                            rechargeSec = std::max(rechargeSec,
                                                   std::max(info->RecoveryTime, info->CategoryRecoveryTime) / 1000.0f);
                    }
            return rechargeSec;
        };
        auto const cooldownRule = [&](float backSec) -> std::string
        {
            if (bossFight)
                return "a boss fight";
            if (lead.fightOverload >= 1.0f)
                return Acore::StringFormat("a critical pull: overload {:.2f}", lead.fightOverload);
            if (std::string const dry = HealerOutOfMana(members, lead.fightSecLeft); !dry.empty())  // [lead903]
                return "critical: " + dry;
            if (!lead.bossLeft)  // [lead1083]
                return "trash, no boss left";
            if (lead.bossPullSec >= backSec)
                return Acore::StringFormat("trash: back before the pull of {} (in {:.0f} s, back in {:.0f} s)",
                                           lead.nextBossName, lead.bossPullSec, backSec);
            return "";
        };
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
                    // [lead904] user 2026-11-03 (32 / 18): the mana potion on trash is kept for the next boss like the
                    // mana gem (the cooldown rule, except critical); its cooldown starts once the fight ends
                    float const backSec = lead.fightSecLeft + itemRechargeSec(potion);
                    std::string const rule = cooldownRule(backSec);
                    item = rule.empty() ? 0 : potion;
                    verdict = Acore::StringFormat(
                        "mana potion {}: {:.0f} % < the {:.0f} % the fight's {:.0f} s left "
                        "need ({}); {}",
                        item ? "used" : "held", bot->GetPowerPct(POWER_MANA), line, lead.fightSecLeft, lineWhy,
                        rule.empty() ? Acore::StringFormat("held for {} (his pull in {:.0f} s, back in {:.0f} s)",
                                                           lead.nextBossName, lead.bossPullSec, backSec)
                                     : rule);
                    if (!item)
                    {
                        thread_local std::map<uint64, std::string> heldLogged;
                        if (std::string& logged = heldLogged[bot->GetGUID().GetRawValue()];
                            logged != lead.nextBossName)
                        {
                            logged = lead.nextBossName;
                            LOG_INFO("playerbots", "[lead904] {}: {}", bot->GetName(), verdict);
                        }
                    }
                    // [lead884] its energize's split threat stays under the tank's on every mob listing it
                    if (std::string const overtakes = item ? ManaSplitOvertakes(bot, tank, amount) : "";
                        !overtakes.empty())
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
                        float const rechargeSec = itemRechargeSec(gem);
                        std::string rule = cooldownRule(rechargeSec);
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

// [lead970] the ms the item's on-use spell is still on the bot's cooldown (0: usable now; game data: the
// player's spell cooldowns, saved across logouts)
uint32 ItemUseCooldownMs(Player* bot, uint32 entry)
{
    uint32 left = 0;
    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
        for (auto const& use : proto->Spells)
            if (use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                left = std::max(left, bot->GetSpellCooldownDelay(uint32(use.SpellId)));
    return left;
}

// [lead975] b228 rfc-h 5E-13 (20261103-224151) 276.3 s: the buff step's three Blessing of Might orders for Rtpalac
// overwrote each other in its one External slot, and the last one, set once from the harness tick, ended with the
// next leader tick (EndTick drops an External the tick does not assert) - nothing was cast. The step's queue
// (TickBuffing, Support::buffQueue) is the leader's here, every leader tick: a caster holds one buff order at a time,
// asserted every tick until the buff is on its target (cast), the caster reports it cannot cast it (failed: the step
// gives it up with the reason) or the step no longer needs it; then the caster's next queued order is issued. An
// order whose mana the caster lacks now is not issued yet: its cast check would refuse it (the paladin at 1 %)
void TickBuffOrders(LeaderGroup& group, std::vector<Player*> const& members, LogFn const& logEvent)
{
    DungeonLeaderState::Support& st = group.State().support;
    GroupRouteState const& route = group.Route();
    if (route.buffStep != 1 || st.buffQueue.empty())
    {
        st.buffManaNeed.clear();  // [lead1133]
        st.buffTold.clear();
        st.buffOrderWork.clear();
        return;
    }
    auto const memberOf = [&members](ObjectGuid guid) -> Player*
    {
        for (Player* bot : members)
            if (bot->GetGUID() == guid)
                return bot;
        return nullptr;
    };
    using BuffKey = DungeonLeaderState::Support::BuffKey;
    auto const queued = [&st](BuffKey const& key)
    {
        for (DungeonLeaderState::Support::BuffOrder const& one : st.buffQueue)
            if (BuffKey{one.caster, one.spell, one.target} == key)
                return true;
        return false;
    };
    auto const ended = [&](DungeonLeaderState::Support::BuffOrder const& told, std::string const& how)
    {
        LOG_INFO("playerbots", "[lead975] buff_order_end: {}: {} ({}, {})", told.what, how, group.Run().runId,
                 route.buffReason);
    };
    // the orders the casters hold: cast, failed, no longer needed - or still standing
    for (auto it = st.buffTold.begin(); it != st.buffTold.end();)
    {
        DungeonLeaderState::Support::BuffOrder const& told = it->second;
        BuffKey const key{told.caster, told.spell, told.target};
        Player* caster = memberOf(told.caster);
        Unit* target = caster ? ObjectAccessor::GetUnit(*caster, told.target) : nullptr;
        std::string failed;
        if (!caster)
            failed = "the caster is no living member any more";
        else
            Leader::Events::TakeExternalFailed(told.caster, told.spell, failed);
        if (!failed.empty())
        {
            st.buffFailed[key] = failed;
            ended(told, "failed: " + failed);
            it = st.buffTold.erase(it);
            continue;
        }
        // [lead1251] b248 rfc-h (16 runs): 66 orders issued, 136 "cast" - a refresh order (the buff on but not
        // lasting the next fight, MissingGivableBuffs) ended "cast" on the old aura, was told again by the next
        // queue and ended again (5A-1 before Oggleflint: Blessing of Might on Rthunaa "cast" 4 times, step 1 12 s).
        // Cast = the caster's aura on the target with more time left than it had when the order was told
        Aura const* on = target ? target->GetAura(told.spell, told.caster) : nullptr;
        if (on && (told.leftMs <= 0 || on->GetDuration() < 0 || on->GetDuration() > told.leftMs))
        {
            ++st.buffCast;
            ended(told, "cast");
            it = st.buffTold.erase(it);
            continue;
        }
        if (!queued(key))
        {
            ended(told, "no longer needed (the buff is on from another source, or the member left the pass)");
            it = st.buffTold.erase(it);
            continue;
        }
        ++it;
    }
    // [lead1133] b238 rfc-h 5H-8 630-727 s (97 s): the disc priest at 3-5 % mana gave Shadow Protection to one member
    // every ~20 s - each order waited for "its mana for the cast" from the game's out-of-combat regeneration. The
    // mana a caster's standing orders cost (each order's own spell cost) is a fact the leader's Rest reads: below
    // it, it drinks to it (LeaderEvents' rest decision), then casts them one after another
    st.buffManaNeed.clear();
    for (DungeonLeaderState::Support::BuffOrder const& order : st.buffQueue)
    {
        Player* caster = memberOf(order.caster);
        SpellInfo const* info = sSpellMgr->GetSpellInfo(order.spell);
        if (caster && info && info->PowerType == POWER_MANA &&
            !st.buffFailed.count(BuffKey{order.caster, order.spell, order.target}))
            st.buffManaNeed[order.caster] += uint32(std::max(0, info->CalcPowerCost(caster, info->GetSchoolMask())));
    }
    for (auto it = st.buffManaNeed.begin(); it != st.buffManaNeed.end();)
    {
        Player* caster = memberOf(it->first);
        if (!caster || caster->GetPower(POWER_MANA) >= it->second)
            it = st.buffManaNeed.erase(it);
        else
            ++it;
    }
    // one order per caster: the one it holds (asserted again: a tick asserts every command it wants), else its next
    std::string work;
    std::set<ObjectGuid> busy;
    for (auto const& [casterGuid, told] : st.buffTold)
    {
        Leader::Events::SetExternal(casterGuid, told.spell, told.target);
        busy.insert(casterGuid);
        work += (work.empty() ? "" : ", ") + told.what;
    }
    for (DungeonLeaderState::Support::BuffOrder const& order : st.buffQueue)
    {
        BuffKey const key{order.caster, order.spell, order.target};
        if (busy.count(order.caster) || st.buffFailed.count(key))
            continue;
        Player* caster = memberOf(order.caster);
        if (!caster)
        {
            st.buffFailed[key] = "the caster is no living member any more";
            continue;
        }
        busy.insert(order.caster);
        SpellInfo const* info = sSpellMgr->GetSpellInfo(order.spell);
        if (!info)
        {
            st.buffFailed[key] = "no spell data";
            continue;
        }
        // mana comes back out of a fight by itself (the game's regeneration); a rage cost does not: that order is
        // issued and the caster's cast check reports it
        // [lead1133] a caster drinking for its orders' mana drinks on to it: no order stands it up for one cast
        bool const drinksForPass = st.buffManaNeed.count(order.caster) && caster->IsSitState() &&
                                   RestState::IsDrinking(caster);
        if (info->PowerType == POWER_MANA && (!Affords(caster, info) || drinksForPass))
        {
            work += (work.empty() ? "" : ", ") + order.what + " (its mana for the cast)";
            continue;
        }
        std::string stale;
        Leader::Events::TakeExternalFailed(order.caster, order.spell, stale);  // a report of an earlier order
        bool const changed = Leader::Events::SetExternal(order.caster, order.spell, order.target);
        Leader::MemberEvents held;
        if (!Leader::Events::Get(order.caster, held) || held.externalSpell != order.spell ||
            held.externalTarget != order.target)
        {
            work += (work.empty() ? "" : ", ") + order.what + " (its External holds another order)";
            continue;
        }
        DungeonLeaderState::Support::BuffOrder& told = st.buffTold[order.caster];
        told = order;
        Unit* buffed = ObjectAccessor::GetUnit(*caster, order.target);
        Aura const* before = buffed ? buffed->GetAura(order.spell, order.caster) : nullptr;
        told.leftMs = before ? std::max(before->GetDuration(), 1) : 0;  // [lead1251]
        st.buffIssuedKeys.insert(key);
        work += (work.empty() ? "" : ", ") + order.what;
        if (changed)
        {
            Unit* on = ObjectAccessor::GetUnit(*caster, order.target);
            logEvent(caster, "External",
                     JsonObject()
                         .Num("spell", order.spell)
                         .Str("on", on ? on->GetName() : "-")
                         .Str("why", "the buff pass: " + order.what));
            LOG_INFO("playerbots", "[lead975] buff_order: {} ({} queued) ({}, {})", order.what, st.buffQueue.size(),
                     group.Run().runId, route.buffReason);
        }
    }
    st.buffOrderWork = work;
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
        st.buffTold.clear();  // [lead975] a fight ends the standing buff orders (no tick asserts them)
        st.buffOrderWork.clear();
        return;
    }
    std::vector<Player*> const members = Everyone(seats, tank);
    std::vector<std::string> verdicts;
    TickBuffOrders(group, members, logEvent);  // [lead975]

    // the soulstone, once per pass before a boss
    uint32 const pass = route.buffSinceMs ? route.buffSinceMs : 1;
    if (route.buffReason.rfind("before ", 0) == 0 && st.soulstonePass != pass)
    {
        bool stoned = false;
        for (Player* bot : members)
            stoned |= HasSoulstoneAura(bot);
        // [lead970] b228 rfc-h 5E-13 (20261103-224151) 251.4-516 s: Rtlocab's Demonic Soulstone, used on Rtpriac in
        // the wave's earlier run (5E-5, 22:36), was still on its 15 min cooldown; the order stood, the warlock's
        // "leader consume" was IMPOSSIBLE each tick without a report, and the buff pass waited on "soulstone:
        // Rtlocab on Rtpriac" until the stall. The leader checks the stone is usable now before it orders it: a
        // stone on its cooldown is no stone for this pass
        Player* warlock = nullptr;
        uint32 stone = 0;
        std::string cooling;
        for (Player* bot : seats)
            for (uint32 item : SOULSTONE_ITEMS)
                if (!warlock && bot->IsAlive() && bot->HasItemCount(item, 1))
                {
                    if (uint32 const leftMs = ItemUseCooldownMs(bot, item))
                    {
                        if (cooling.empty())
                            cooling = Acore::StringFormat("{}'s soulstone is on its cooldown ({:.0f} s left)",
                                                          bot->GetName(), leftMs / 1000.0f);
                        continue;
                    }
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
        {
            st.soulstonePass = pass;  // nothing to do for this pass
            if (!stoned && !warlock && !cooling.empty())
                verdicts.push_back("soulstone dropped: " + cooling);
        }
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

std::string EnergizeSplitOvertakes(Player* receiver, Player* caster, Player* tank, uint32 mana)
{
    return receiver && caster ? ManaSplitOvertakes(receiver, tank, mana, caster) : std::string();
}

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

std::string HealerOutOfMana(std::vector<Player*> const& members, float fightSecLeft)
{
    if (fightSecLeft <= 0.0f)
        return "";
    for (Player* bot : members)
        if (bot && bot->IsAlive() && PlayerbotAI::IsHeal(bot) && bot->GetMaxPower(POWER_MANA))
            if (float const line = RestState::FightManaLine(bot, fightSecLeft, 0.0f);
                bot->GetPowerPct(POWER_MANA) < line)
                return Acore::StringFormat("the healer {} out of mana: {:.0f} % < the {:.0f} % the fight's {:.0f} s "
                                           "left need",
                                           bot->GetName(), bot->GetPowerPct(POWER_MANA), line, fightSecLeft);
    return "";
}

float MemberTakenPerSec(LeaderGroup& group, Player* member)
{
    return member ? TakenPerSec(group.State().support, member, getMSTime() ? getMSTime() : 1) : 0.0f;
}

RestManaWay BestRestManaTool(Player* bot, std::vector<Player*> const& members, float needPct, float drinkRate,
                             CooldownVerdictFn const& verdict, RechargeFn const& recharge)
{
    RestManaWay best;
    if (!bot || needPct <= 0.0f || !bot->GetMaxPower(POWER_MANA))
        return best;
    std::vector<Player*> everyone = members;
    if (std::find(everyone.begin(), everyone.end(), bot) == everyone.end())
        everyone.push_back(bot);
    std::string held;  // the last tool the cooldown verdict held (logged when none is allowed)
    // the group seconds a tool of `gainPct` over `durationSec` after a cast of `castSec` takes to `needPct`: the
    // lead955 Innervate rule - its duration's share when it covers the need, else its whole duration and the drink
    // for what it leaves (no drink in the bags: no way)
    auto const secOf = [&](float gainPct, float castSec, float durationSec) -> float
    {
        if (gainPct <= 0.0f)
            return -1.0f;
        float const left = needPct - gainPct;
        if (left <= 0.0f)
            return castSec + durationSec * needPct / gainPct;
        return drinkRate > 0.0f ? castSec + durationSec + left / drinkRate : -1.0f;
    };
    auto const consider = [&](Player* caster, uint32 spell, Player* on, char const* name)
    {
        SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
        if (!info || info->CheckShapeshift(caster->GetShapeshiftForm()) != SPELL_CAST_OK)
            return;
        // one order per fact: a caster told this tool on another member keeps it for that one
        if (Leader::MemberEvents told; Leader::Events::Get(caster->GetGUID(), told) && told.externalSpell == spell &&
                                       !told.externalTarget.IsEmpty() && on &&
                                       told.externalTarget != on->GetGUID())
            return;
        float const gainPct = RestState::ManaToolPct(caster, bot, spell);
        float const durationSec = std::max(info->GetMaxDuration(), 0) / 1000.0f;
        if (gainPct <= 0.0f || durationSec <= 0.0f)
            return;
        float const castSec = std::max<uint32>(info->StartRecoveryTime, info->CalcCastTime(caster)) / 1000.0f;
        float const sec = secOf(gainPct, info->IsChanneled() ? 0.0f : castSec, durationSec);
        if (sec < 0.0f || (best.sec >= 0.0f && sec >= best.sec))
            return;
        std::string why;
        if (!verdict(recharge(caster, spell), durationSec, why))
        {
            held = Acore::StringFormat("{} of {} {}", name, caster->GetName(), why);
            return;
        }
        best.caster = caster;
        best.spell = spell;
        best.item = 0;
        best.on = on;
        best.sec = sec;
        best.name = name;
        best.why = Acore::StringFormat("{} {:.0f} % from {} over {:.0f} s, {}", name, gainPct, caster->GetName(),
                                       durationSec, why);
    };
    // the 3 members lowest on mana (spell_pri_hymn_of_hope's pick)
    std::vector<Player*> lowest;
    for (Player* member : everyone)
        if (member->IsAlive() && member->getPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA))
            lowest.push_back(member);
    std::sort(lowest.begin(), lowest.end(),
              [](Player* a, Player* b) { return a->GetPowerPct(POWER_MANA) < b->GetPowerPct(POWER_MANA); });
    if (lowest.size() > RestState::HYMN_OF_HOPE_TARGETS)
        lowest.resize(RestState::HYMN_OF_HOPE_TARGETS);
    bool const hymnReaches = std::find(lowest.begin(), lowest.end(), bot) != lowest.end();
    for (Player* caster : everyone)
    {
        // a caster drinking keeps its drink; another member's druid is no tool while short of its own line
        // (lead751); the member itself is one while it does not drink yet
        if (!caster->IsAlive() || caster->IsInCombat() || (caster != bot && caster->IsSitState()))
            continue;
        if (uint32 const spell = Leader::Fight::LeaderReadySpell(caster, {RestState::SPELL_INNERVATE});
            spell && (caster == bot || !caster->GetMaxPower(POWER_MANA) ||
                      caster->GetPowerPct(POWER_MANA) >= RestState::OwnManaLine(caster)))
            consider(caster, spell, bot, "Innervate");
        if (hymnReaches)
            if (uint32 const spell = Leader::Fight::LeaderReadySpell(caster, {RestState::SPELL_HYMN_OF_HOPE}))
                consider(caster, spell, nullptr, "Hymn of Hope");
        if (uint32 const spell = Leader::Fight::LeaderReadySpell(caster, {RestState::SPELL_MANA_TIDE_TOTEM}))
            if (float const radius = RestState::ManaToolRadius(caster, spell);
                radius <= 0.0f || bot->GetExactDist(caster) <= radius)
                consider(caster, spell, nullptr, "Mana Tide Totem");
        if (caster == bot)
        {
            if (uint32 const spell = Leader::Fight::LeaderReadySpell(caster, {RestState::SPELL_EVOCATION}))
                consider(caster, spell, nullptr, "Evocation");
            if (uint32 const spell = Leader::Fight::LeaderReadySpell(caster, {RestState::SPELL_DISPERSION}))
                consider(caster, spell, nullptr, "Dispersion");
        }
    }
    // the member's mana gem (its own cooldown, no potion slot): instant, by the same verdict on its item's recharge
    if (uint32 amount = 0; uint32 const gem = RaidConsumables::FindManaGem(bot, amount))
    {
        float const gainPct = 100.0f * float(amount) / float(bot->GetMaxPower(POWER_MANA));
        float const sec = secOf(gainPct, 0.0f, 0.0f);
        float rechargeSec = 0.0f;
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(gem))
            for (auto const& use : proto->Spells)
                if (use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                {
                    rechargeSec = std::max({rechargeSec, float(std::max(use.SpellCooldown, 0)) / 1000.0f,
                                            float(std::max(use.SpellCategoryCooldown, 0)) / 1000.0f});
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(use.SpellId)))
                        rechargeSec = std::max(rechargeSec,
                                               std::max(info->RecoveryTime, info->CategoryRecoveryTime) / 1000.0f);
                }
        std::string why;
        if (sec >= 0.0f && (best.sec < 0.0f || sec < best.sec))
        {
            if (verdict(rechargeSec, 0.0f, why))
            {
                best = RestManaWay();
                best.caster = bot;
                best.item = gem;
                best.sec = sec;
                best.name = "mana gem";
                best.why = Acore::StringFormat("mana gem {:.0f} %, {}", gainPct, why);
            }
            else
                held = "mana gem " + why;
        }
    }
    if (!best.caster && !held.empty())
        best.why = "held: " + held;
    return best;
}
}  // namespace Leader::Support
