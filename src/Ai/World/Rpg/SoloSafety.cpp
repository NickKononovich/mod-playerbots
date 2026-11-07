/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SoloSafety.h"

#include "BuyAction.h"
#include "CellImpl.h"
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "ObjectAccessor.h"
#include "World.h"
#include "EstimatedLifetimeValue.h"
#include "Formulas.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "LeaderFightChecks.h"
#include "NewRpgBaseAction.h"
#include "Log.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RouteSearch.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SmartScriptMgr.h"  // [guild14] creature casts
#include "StringFormat.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace
{
    // one attacker's part of an open fight
    struct AttackerTrack
    {
        uint32 entry = 0;
        uint32 firstMs = 0;
        uint32 lastMs = 0;
        uint64 damage = 0;
        float swingSec = 0.0f;
        std::string name;
        uint8 level = 0;
    };

    struct EngageNote
    {
        ObjectGuid target;
        std::string targetName;
        float predicted = 0.0f;
        float health = 0.0f;
        float healthPct = 0.0f;
        float manaPct = 0.0f;
        uint32 mobs = 0;
    };

    // the self heal the bot casts best (most healing per mana), from spell data
    struct HealSpell
    {
        size_t spells = size_t(-1);
        int32 power = -1;
        uint32 id = 0;
        float amount = 0.0f;
        float cost = 0.0f;
        float castSec = 0.0f;
    };

    struct SoloBotState
    {
        // the open fight
        bool open = false;
        uint32 firstMs = 0;
        uint32 lastMs = 0;
        uint64 dealt = 0;
        uint64 taken = 0;
        uint64 healed = 0;  // its own heals on itself this fight
        uint32 manaAtOpen = 0;
        std::unordered_map<uint64, AttackerTrack> attackers;
        bool engaged = false;
        EngageNote engage;
        // the bot's measured fights
        double dealtSum = 0.0;
        double fightSecSum = 0.0;
        double manaSum = 0.0;
        double healedSum = 0.0;
        uint32 fights = 0;
        // rest and recovery
        bool rest = false;
        std::string restWhy;
        bool recover = false;
        // one log line per target and verdict
        ObjectGuid loggedTarget;
        uint8 loggedKind = 255;
        HealSpell heal;
        // [guild14] its measured damage per level gap of the fight's top attacker (gap -> dealt, seconds)
        std::map<int32, std::pair<double, double>> gapDealt;
        // [guild14] (entry, level gap) of the mobs it lost a fight to
        std::set<std::pair<uint32, int32>> lostTo;
        // [guild14] the open fight turned lost: it disengages until the fight ends
        bool fleeing = false;
        // [guild15] per creature entry, against this bot: what the entry dealt it per second, and what it dealt that
        // entry in fights with that one attacker (damage, seconds)
        std::unordered_map<uint32, std::pair<double, double>> ownTaken;
        std::unordered_map<uint32, std::pair<double, double>> ownDealt;
    };

    struct EntryDamage
    {
        double damage = 0.0;
        double sec = 0.0;
    };

    float Rate(std::unordered_map<uint32, std::pair<double, double>> const& rates, uint32 entry)
    {
        auto const found = rates.find(entry);
        if (found == rates.end() || found->second.second <= 0.0)
            return -1.0f;
        return float(found->second.first / found->second.second);
    }

    std::mutex lock;
    std::unordered_map<uint32, SoloBotState> states;
    std::unordered_map<uint32, EntryDamage> entryDamage;  // creature entry -> what it dealt to solo bots, per second

    bool SoloPlayerBot(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
            return false;
        if (bot->GetMap() && bot->GetMap()->Instanceable())
            return false;
        Group* group = bot->GetGroup();
        return !group || group->GetMembersCount() <= 1;
    }

    float SwingSec(Unit* unit, WeaponAttackType type = BASE_ATTACK)
    {
        return std::max(float(unit->GetAttackTime(type)) / float(IN_MILLISECONDS), 0.1f);
    }

    // a closed fight goes into the bot's and the entries' measurements (caller holds the lock)
    void CloseFight(Player* bot, SoloBotState& state, bool died)
    {
        if (!state.open)
            return;
        state.open = false;
        float const seconds =
            float(getMSTimeDiff(state.firstMs, state.lastMs)) / float(IN_MILLISECONDS) + SwingSec(bot);
        state.dealtSum += double(state.dealt);
        state.healedSum += double(state.healed);
        state.fightSecSum += seconds;
        uint32 const mana = bot->GetPower(POWER_MANA);
        if (state.manaAtOpen > mana)
            state.manaSum += double(state.manaAtOpen - mana);
        ++state.fights;
        for (auto const& [guid, track] : state.attackers)
        {
            if (!track.entry || !track.damage)
                continue;
            EntryDamage& entry = entryDamage[track.entry];
            double const trackSec =
                double(getMSTimeDiff(track.firstMs, track.lastMs)) / IN_MILLISECONDS + track.swingSec;
            entry.damage += double(track.damage);
            entry.sec += trackSec;
            // [guild15] ... and on this bot (its armour, its level): b311 Ravelandy L7 died four times in 9 min to
            // Prowlers / bears L9 judged won on what they dealt every bot
            auto& own = state.ownTaken[track.entry];
            own.first += double(track.damage);
            own.second += trackSec;
        }
        if (state.attackers.size() == 1)
        {
            auto& own = state.ownDealt[state.attackers.begin()->second.entry];
            own.first += double(state.dealt);
            own.second += seconds;
        }
        std::string came;
        for (auto const& [guid, track] : state.attackers)
            came += (came.empty() ? "" : ", ") + track.name + " L" + std::to_string(track.level);
        if (state.engaged)
            LOG_INFO("playerbots",
                     "[solo] {} L{} fight {}: {} judged {} mobs, {:.0f} dmg expected at hp {:.0f}; came {} ({}), took "
                     "{} in {:.1f} s, dealt {} ({:.1f} dps)",
                     bot->GetName(), bot->GetLevel(), died ? "LOST" : state.fleeing ? "DISENGAGED" : "won",
                     state.engage.targetName,
                     state.engage.mobs, state.engage.predicted, state.engage.health, state.attackers.size(), came,
                     state.taken, seconds, state.dealt, float(state.dealt) / seconds);
        // [guild14] its damage at the fight's level gap (its top attacker's), and what it lost (a death, a disengage):
        // the mobs it lost to are not pulled again at that gap (b303 Rollothan L7 died three times to a Prowler L9
        // judged won)
        {
            int32 gap = std::numeric_limits<int32>::min();
            for (auto const& [guid, track] : state.attackers)
                gap = std::max(gap, int32(track.level) - int32(bot->GetLevel()));
            if (gap != std::numeric_limits<int32>::min())
            {
                auto& at = state.gapDealt[gap];
                at.first += double(state.dealt);
                at.second += seconds;
            }
            if (died || state.fleeing)
                for (auto const& [guid, track] : state.attackers)
                    if (track.entry && track.damage && int32(track.level) - int32(bot->GetLevel()) == gap)
                    {
                        state.lostTo.insert({track.entry, int32(track.level) - int32(bot->GetLevel())});
                        LOG_INFO("playerbots", "[solo] {} L{} lost to {} L{} ({}): not pulled again at a gap of {} or "
                                 "more", bot->GetName(), bot->GetLevel(), track.name, track.level,
                                 died ? "died" : "disengaged", int32(track.level) - int32(bot->GetLevel()));
                    }
        }
        state.fleeing = false;
        state.attackers.clear();
        state.engaged = false;
        state.dealt = 0;
        state.taken = 0;
        state.healed = 0;
    }

    void CloseIfOver(Player* bot, SoloBotState& state)
    {
        if (state.open && !bot->IsInCombat() && bot->IsAlive())
            CloseFight(bot, state, false);
    }

    void OpenIfNeeded(Player* bot, SoloBotState& state, uint32 now)
    {
        if (state.open)
            return;
        state.open = true;
        state.firstMs = now;
        state.lastMs = now;
        state.manaAtOpen = bot->GetPower(POWER_MANA);
    }

    // the bot's self heal from its known spells (direct heal effects; spell power counted)
    HealSpell const& BestHeal(Player* bot, SoloBotState& state)
    {
        int32 const power = bot->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_ALL);
        if (state.heal.spells == bot->GetSpellMap().size() && state.heal.power == power)
            return state.heal;
        HealSpell best;
        best.spells = bot->GetSpellMap().size();
        best.power = power;
        float bestPerMana = 0.0f;
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || !info->IsPositive() || info->PowerType != POWER_MANA)
                continue;
            float amount = 0.0f;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_HEAL)
                    amount += float(bot->SpellHealingBonusDone(bot, info, uint32(std::max(effect.CalcValue(bot), 0)),
                                                               HEAL, effect.EffectIndex));
            float const cost = float(info->CalcPowerCost(bot, info->GetSchoolMask()));
            if (amount <= 0.0f || cost <= 0.0f)
                continue;
            if (amount / cost > bestPerMana)
            {
                bestPerMana = amount / cost;
                best.id = spellId;
                best.amount = amount;
                best.cost = cost;
                best.castSec = std::max(float(info->CalcCastTime(bot)), float(info->StartRecoveryTime)) /
                               float(IN_MILLISECONDS);
            }
        }
        state.heal = best;
        return state.heal;
    }

    float MeasuredEntryDps(uint32 entry)
    {
        auto const found = entryDamage.find(entry);
        if (found == entryDamage.end() || found->second.sec <= 0.0)
            return 0.0f;
        return float(found->second.damage / found->second.sec);
    }

    // its weapon's white damage per second (melee, or the ranged weapon / wand: the better) and its pet's
    float WhiteDps(Player* bot)
    {
        float dps = 0.0f;
        for (WeaponAttackType type : {BASE_ATTACK, RANGED_ATTACK})
        {
            if (!bot->GetAttackTime(type))
                continue;
            float const hit = type == BASE_ATTACK ? (bot->GetFloatValue(UNIT_FIELD_MINDAMAGE) +
                                                     bot->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f
                                                  : (bot->GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE) +
                                                     bot->GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE)) / 2.0f;
            dps = std::max(dps, hit / SwingSec(bot, type));
        }
        if (Pet* pet = bot->GetPet())
            if (pet->IsAlive())
                dps += (pet->GetFloatValue(UNIT_FIELD_MINDAMAGE) + pet->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f /
                       SwingSec(pet);
        return dps;
    }

    float BotDpsLocked(Player* bot, SoloBotState const& state)
    {
        if (state.fightSecSum > 0.0 && state.dealtSum > 0.0)
            return float(state.dealtSum / state.fightSecSum);
        // before its first measured fight: its weapon's white damage only, the spells' share comes with the
        // measurement (b297: the model's every-cast spell damage put a level 5 mage at 19 damage taken for a fight it
        // lost in 103 s at 1.9 dps)
        return WhiteDps(bot);
    }

    // [guild14] its damage measured in fights at that level gap (b303: Rollothan L7 dealt 4.7 dps to Prowlers L9,
    // its all-fights figure was higher), else its all-fights figure
    float BotDpsAt(Player* bot, SoloBotState const& state, int32 gap)
    {
        auto const found = state.gapDealt.find(gap);
        if (found != state.gapDealt.end() && found->second.first > 0.0 && found->second.second > 0.0)
            return float(found->second.first / found->second.second);
        return BotDpsLocked(bot, state);
    }

    // [guild14] a creature's spell damage per second from its SmartAI: every in-combat repeating cast (SMART_EVENT_
    // UPDATE_IC + SMART_ACTION_CAST) of a damaging spell - its direct school damage and its periodic damage over the
    // aura's duration - once per repeat interval (the mean of repeatMin / repeatMax), at least its cast time and the
    // global cooldown. b303: casters were counted by their current cast only (Kobold Geomancers, Defias Rogue Wizards,
    // Gnarlpine and Bristlelimb Shamans killed bots judged to win)
    float SpellDpsOf(Creature* creature)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, float> cache;  // entry -> dps at the creature's level
        uint32 const entry = creature->GetEntry();
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
                return found->second;
        }
        float dps = 0.0f;
        if (creature->GetAIName() == "SmartAI")
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.event.type != SMART_EVENT_UPDATE_IC || holder.action.type != SMART_ACTION_CAST)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(holder.action.cast.spell);
                if (!info || info->IsPositive())
                    continue;
                float damage = 0.0f;
                for (SpellEffectInfo const& effect : info->GetEffects())
                {
                    if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                        damage += float(std::max(effect.CalcValue(creature), 0));
                    else if (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                             effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE && effect.Amplitude > 0)
                        damage += float(std::max(effect.CalcValue(creature), 0)) *
                                  float(std::max(info->GetMaxDuration(), 0) / effect.Amplitude);
                }
                if (damage <= 0.0f)
                    continue;
                float const repeatSec =
                    float(holder.event.minMaxRepeat.repeatMin + holder.event.minMaxRepeat.repeatMax) / 2.0f /
                    float(IN_MILLISECONDS);
                float const castSec = float(info->CalcCastTime(creature)) / float(IN_MILLISECONDS);
                float const gcdSec = float(std::max<uint32>(info->StartRecoveryTime, 1500)) / float(IN_MILLISECONDS);
                // a cast with no repeat goes once a fight: counted over its cast and the cooldown only when it repeats
                if (repeatSec <= 0.0f)
                    continue;
                dps += damage / std::max({repeatSec, castSec, gcdSec});
            }
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[entry] = dps;
        return dps;
    }

    // [guild14] what a mob deals the bot per second: its melee (Leader::Fight::AttackerDps, its crits counted: the
    // core's Unit::GetUnitCriticalChance, a crit doubling the hit), its spells (the cast it is on, its SmartAI casts:
    // the higher), or what its entry was measured to deal solo bots - the highest
    float MobDps(Unit* unit, Player* bot)
    {
        float spell = 0.0f;
        float const total = Leader::Fight::AttackerDps(unit, bot, spell);
        float const crit = std::clamp(unit->GetUnitCriticalChance(BASE_ATTACK, bot) / 100.0f, 0.0f, 1.0f);
        float dps = (total - spell) * (1.0f + crit);
        if (Creature* creature = unit->ToCreature())
        {
            dps += std::max(spell, SpellDpsOf(creature));
            dps = std::max(dps, MeasuredEntryDps(creature->GetEntry()));
        }
        else
            dps += spell;
        return dps;
    }

    // [guild15] the help a creature's SmartAI brings in the fight: SMART_ACTION_CALL_FOR_HELP (its radius: every idle
    // mob that assists it) and SMART_ACTION_FLEE_FOR_ASSIST (Creature::DoFleeToGetAssistance: the nearest one that
    // assists it within CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS). b314: murloc camps - Streamrunners flee for
    // assist at 1-5 % health; judged 2, came 3-4, two deaths in 10 min
    struct ScriptHelp
    {
        float callRadius = 0.0f;
        bool flees = false;
    };
    ScriptHelp ScriptHelpOf(Creature* creature)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, ScriptHelp> cache;
        uint32 const entry = creature->GetEntry();
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            auto const found = cache.find(entry);
            if (found != cache.end())
                return found->second;
        }
        ScriptHelp help;
        if (creature->GetAIName() == "SmartAI")
            for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
            {
                if (holder.action.type == SMART_ACTION_CALL_FOR_HELP)
                    help.callRadius = std::max(help.callRadius, float(holder.action.callHelp.range));
                else if (holder.action.type == SMART_ACTION_FLEE_FOR_ASSIST)
                    help.flees = true;
            }
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[entry] = help;
        return help;
    }

    char const* KindName(SoloSafety::Verdict::Kind kind)
    {
        switch (kind)
        {
            case SoloSafety::Verdict::Kind::Win:
                return "engage";
            case SoloSafety::Verdict::Kind::RestFirst:
                return "rest first";
            default:
                return "refuse";
        }
    }

    bool IsElite(Creature* creature)
    {
        uint32 const rank = creature->GetCreatureTemplate()->rank;
        return rank == CREATURE_ELITE_ELITE || rank == CREATURE_ELITE_RAREELITE || rank == CREATURE_ELITE_WORLDBOSS;
    }
}

namespace SoloSafety
{
    bool Applies(Player* bot)
    {
        if (!SoloPlayerBot(bot))
            return false;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT);
    }

    void NoteDamage(Unit* attacker, Unit* victim, uint32 damage)
    {
        if (!attacker || !victim || !damage)
            return;
        uint32 const now = getMSTime();
        // the bot takes it
        if (Player* bot = victim->ToPlayer())
        {
            Unit* source = attacker->GetCharmerOrOwnerOrSelf();
            Creature* mob = source ? source->ToCreature() : nullptr;
            if (!mob || !SoloPlayerBot(bot))
                return;
            std::lock_guard<std::mutex> guard(lock);
            SoloBotState& state = states[bot->GetGUID().GetCounter()];
            OpenIfNeeded(bot, state, now);
            state.lastMs = now;
            uint32 const taken = std::min(damage, bot->GetHealth());
            state.taken += taken;
            AttackerTrack& track = state.attackers[mob->GetGUID().GetRawValue()];
            if (!track.entry)
            {
                track.entry = mob->GetEntry();
                track.firstMs = now;
                track.swingSec = SwingSec(mob);
                track.name = mob->GetName();
                track.level = mob->GetLevel();
            }
            track.lastMs = now;
            track.damage += taken;
            return;
        }
        // the bot (its pet) deals it
        Creature* mob = victim->ToCreature();
        Player* bot = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!mob || mob->GetCharmerOrOwnerPlayerOrPlayerItself() || !SoloPlayerBot(bot))
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        OpenIfNeeded(bot, state, now);
        state.lastMs = now;
        state.dealt += std::min(damage, mob->GetHealth());
    }

    void NoteHeal(Unit* healer, Unit* target, uint32 heal)
    {
        Player* bot = target ? target->ToPlayer() : nullptr;
        if (!bot || healer != target || !heal || !SoloPlayerBot(bot))
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        if (state.open)
            state.healed += std::min<uint32>(heal, bot->GetMaxHealth() - std::min(bot->GetHealth(),
                                                                                   bot->GetMaxHealth()));
    }

    float BotDps(Player* bot)
    {
        std::lock_guard<std::mutex> guard(lock);
        return BotDpsLocked(bot, states[bot->GetGUID().GetCounter()]);
    }

    Verdict Judge(Player* bot, Unit* target)
    {
        Verdict verdict;
        if (!target || !Applies(bot))
            return verdict;

        // where the fight stands: at the target for melee, at the bot's reach on its side for a ranged bot
        Position spot = target->GetPosition();
        float const reach = PlayerbotAI::IsRanged(bot) ? Leader::Fight::LeaderSpellReach(bot) : 0.0f;
        float const gap = bot->GetExactDist2d(target);
        if (reach > 0.0f)
        {
            if (gap > reach && gap > 0.0f)
            {
                float const share = reach / gap;
                spot.Relocate(target->GetPositionX() + (bot->GetPositionX() - target->GetPositionX()) * share,
                              target->GetPositionY() + (bot->GetPositionY() - target->GetPositionY()) * share,
                              target->GetPositionZ());
            }
            else
                spot = bot->GetPosition();
        }
        std::vector<Unit*> pack;
        PullPlanner::SoloPack(bot, target, spot, pack);
        if (pack.empty())
            pack.push_back(target);
        for (Unit* attacker : bot->getAttackers())
            if (attacker && attacker->IsAlive() && std::find(pack.begin(), pack.end(), attacker) == pack.end())
                pack.push_back(attacker);
        // [guild13] b300: "judged 1 mob, came 2-3" in most lost fights (Princess and her Porcine Entourage, Prowlers
        // wandering in): what the core brings on top, until nothing more joins - a pack mob's formation
        // (CreatureGroup), an idle mob whose aggro reach (with its wander area) covers the fight spot, and one its
        // faction's assistance call reaches (CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS, Creature::CanAssistTo)
        {
            float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
            std::vector<Creature*> idle;
            for (ObjectGuid const& guid : GET_PLAYERBOT_AI(bot)->GetAiObjectContext()
                                              ->GetValue<GuidVector>("possible targets no los")->Get())
                if (Creature* mob = ObjectAccessor::GetCreature(*bot, guid))
                    if (mob->IsAlive() && !mob->IsInCombat() && mob->IsHostileTo(bot) &&
                        mob->HasReactState(REACT_AGGRESSIVE))
                        idle.push_back(mob);
            auto const inPack = [&pack](Unit* unit) { return std::find(pack.begin(), pack.end(), unit) != pack.end(); };
            for (size_t i = 0; i < pack.size(); ++i)
            {
                Creature* member = pack[i]->ToCreature();
                if (!member)
                    continue;
                if (CreatureGroup* formation = member->GetFormation())
                    for (auto const& [mate, info] : formation->GetMembers())
                        if (mate && mate->IsAlive() && !inPack(mate))
                            pack.push_back(mate);
                for (Creature* mob : idle)
                {
                    if (inPack(mob))
                        continue;
                    bool const assists = assist > 0.0f && mob->GetFaction() == member->GetFaction() &&
                                         mob->IsWithinDistInMap(member, assist) && mob->CanAssistTo(member, bot);
                    if (assists)
                        pack.push_back(mob);
                }
                // [guild15] the help its script calls: all in the call's radius, the nearest one it flees to
                ScriptHelp const help = ScriptHelpOf(member);
                if (help.callRadius > 0.0f)
                    for (Creature* mob : idle)
                        if (!inPack(mob) && mob->IsWithinDistInMap(member, help.callRadius) &&
                            mob->CanAssistTo(member, bot))
                            pack.push_back(mob);
                if (help.flees)
                {
                    float const flee = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
                    Creature* nearest = nullptr;
                    for (Creature* mob : idle)
                        if (!inPack(mob) && mob->IsWithinDistInMap(member, flee) && mob->CanAssistTo(member, bot) &&
                            (!nearest || member->GetDistance(mob) < member->GetDistance(nearest)))
                            nearest = mob;
                    if (nearest)
                        pack.push_back(nearest);
                }
            }
            for (Creature* mob : idle)
            {
                if (inPack(mob))
                    continue;
                float reach = mob->GetAggroRange(bot) + bot->GetCombatReach();
                Position centre = mob->GetPosition();
                if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                {
                    centre = mob->GetHomePosition();
                    reach += mob->GetWanderDistance();
                }
                if (centre.GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) <= reach)
                    pack.push_back(mob);
            }
        }

        struct Mob
        {
            Unit* unit;
            float hp;
            float dps;
        };
        std::vector<Mob> mobs;
        bool elite = false;
        verdict.at = target->GetPosition();
        {
            std::lock_guard<std::mutex> guard(lock);
            for (Unit* unit : pack)
            {
                float dps = MobDps(unit, bot);  // [guild14] crits, SmartAI casts, the measurement
                if (Creature* creature = unit->ToCreature())
                {
                    // [guild15] what it measured dealing this bot, when higher
                    dps = std::max(dps, Rate(states[bot->GetGUID().GetCounter()].ownTaken, creature->GetEntry()));
                    elite = elite || IsElite(creature);
                    verdict.radius = std::max(verdict.radius,
                                              unit->GetExactDist2d(target) + creature->GetAggroRange(bot));
                }
                if (unit->GetLevel() > bot->GetLevel())
                    ++verdict.above;
                verdict.topLevel = std::max<uint8>(verdict.topLevel, unit->GetLevel());
                mobs.push_back({unit, float(unit->GetHealth()), dps});
            }
        }
        verdict.mobs = mobs.size();

        SoloBotState snapshot;
        float botDps = 0.0f;
        {
            std::lock_guard<std::mutex> guard(lock);
            SoloBotState& state = states[bot->GetGUID().GetCounter()];
            CloseIfOver(bot, state);
            botDps = BotDpsAt(bot, state, int32(verdict.topLevel) - int32(bot->GetLevel()));  // [guild14]
            // [guild15] its damage measured on the target's entry alone, when lower (b310 Ellosera L10 dealt nothing
            // to a Bloodmyst Hatchling it pulled twice and died)
            if (Creature* creature = target->ToCreature())
            {
                float const onEntry = Rate(state.ownDealt, creature->GetEntry());
                if (onEntry >= 0.0f)
                    botDps = std::min(botDps, onEntry);
            }
            // [guild15] user 2026-11-07: a mob it lost to is no veto by itself (a player kills yellow +1 / +2 mobs
            // solo all the time): the lost fight is in the numbers - the entry's measured damage, its damage at that
            // level gap - and only the projection refuses
            BestHeal(bot, state);
            snapshot.heal = state.heal;
            snapshot.manaSum = state.manaSum;
            snapshot.fightSecSum = state.fightSecSum;
            snapshot.healedSum = state.healedSum;
        }
        if (botDps <= 0.0f)
            botDps = 1.0f;

        // the kill order: the target first, then the most damage per health
        std::sort(mobs.begin() + 1, mobs.end(),
                  [](Mob const& a, Mob const& b)
                  { return a.dps / std::max(a.hp, 1.0f) > b.dps / std::max(b.hp, 1.0f); });

        float petHealth = 0.0f;
        if (Pet* pet = bot->GetPet())
            if (pet->IsAlive())
                petHealth = float(pet->GetHealth());
        // [guild15] its damage lasts as long as its mana: the mana it spends per fight second (measured over its
        // fights) runs its spells, its weapon's white damage (and its pet's) is what is left when the mana is gone
        // (b307/b308: priests pulled at 7-25 % mana judged on their full-mana damage and died - Orenem L10 three
        // times, Guinudrond L9)
        float const manaPerSec =
            snapshot.fightSecSum > 0.0 ? float(snapshot.manaSum / snapshot.fightSecSum) : 0.0f;
        float const whiteDps = std::clamp(WhiteDps(bot), 1.0f, botDps);
        // health + pet + what it heals itself per fight second, measured (its own heals on itself over its fights),
        // as far as its mana lasts: b298 Peny L8 took 4 Murlocs on a spell model of its heals and healed 24 in 50 s
        float const healRate =
            snapshot.fightSecSum > 0.0 ? float(snapshot.healedSum / snapshot.fightSecSum) : 0.0f;
        struct Projection
        {
            float seconds = 0.0f;
            float damage = 0.0f;
        };
        auto const project = [&](float mana) -> Projection
        {
            float const spellSec = bot->GetMaxPower(POWER_MANA) && manaPerSec > 0.0f
                                       ? mana / manaPerSec
                                       : std::numeric_limits<float>::max();
            Projection out;
            for (Mob const& mob : mobs)
            {
                float hp = mob.hp;
                if (out.seconds < spellSec)
                {
                    float const spellPart = (spellSec - out.seconds) * botDps;
                    if (hp <= spellPart)
                    {
                        out.seconds += hp / botDps;
                        hp = 0.0f;
                    }
                    else
                    {
                        out.seconds = spellSec;
                        hp -= spellPart;
                    }
                }
                out.seconds += hp / whiteDps;
                out.damage += mob.dps * out.seconds;
            }
            return out;
        };
        Projection const now = project(float(bot->GetPower(POWER_MANA)));
        Projection const atFull = project(float(bot->GetMaxPower(POWER_MANA)));
        float const seconds = now.seconds;
        float const damage = now.damage;
        verdict.damage = damage;
        verdict.fightSec = seconds;
        auto const effective = [&](Projection const& fight, float health, float mana, bool& wins) -> float
        {
            float heals = healRate * fight.seconds;
            if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
                heals *= std::min(1.0f, mana / float(maxMana));
            float const ehp = health + petHealth + heals;
            wins = fight.damage < ehp;
            return ehp;
        };
        bool winNow = false;
        bool winFull = false;
        verdict.ehpNow = effective(now, float(bot->GetHealth()), float(bot->GetPower(POWER_MANA)), winNow);
        verdict.ehpFull =
            effective(atFull, float(bot->GetMaxHealth()), float(bot->GetMaxPower(POWER_MANA)), winFull);
        bool const full = bot->GetHealth() >= bot->GetMaxHealth() &&
                          (!bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA));

        if (elite)
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = "an elite in the pack";
        }
        else if (verdict.above >= 2)
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = Acore::StringFormat("{} mobs above its level at once", verdict.above);
        }
        else if (winNow && (!verdict.above || full))
            verdict.kind = Verdict::Kind::Win;
        else if (winFull)
        {
            verdict.kind = Verdict::Kind::RestFirst;
            verdict.why = winNow ? "a mob above its level: at full health and mana only" : "wins only at full";
        }
        else
        {
            verdict.kind = Verdict::Kind::Refuse;
            verdict.why = "loses even at full";
        }
        std::string const numbers = Acore::StringFormat(
            "{} mobs ({} above), {:.0f} dmg expected over {:.0f} s at {:.1f} dps ({:.1f} without mana; full: {:.0f} "
            "dmg over {:.0f} s), ehp {:.0f} now / {:.0f} full",
            verdict.mobs, verdict.above, damage, seconds, botDps, whiteDps, atFull.damage, atFull.seconds,
            verdict.ehpNow, verdict.ehpFull);
        verdict.why = verdict.why.empty() ? numbers : verdict.why + ": " + numbers;

        bool log = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            SoloBotState& state = states[bot->GetGUID().GetCounter()];
            if (state.loggedTarget != target->GetGUID() || state.loggedKind != uint8(verdict.kind))
            {
                state.loggedTarget = target->GetGUID();
                state.loggedKind = uint8(verdict.kind);
                log = verdict.kind != Verdict::Kind::Win;
            }
        }
        if (log)
            LOG_INFO("playerbots", "[solo] {} L{} {} {} L{}: {}", bot->GetName(), bot->GetLevel(),
                     KindName(verdict.kind), target->GetName(), target->GetLevel(), verdict.why);
        return verdict;
    }

    void NoteEngage(Player* bot, Unit* target, Verdict const& verdict)
    {
        if (!bot || !target)
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        if (state.engaged && state.engage.target == target->GetGUID())
            return;
        state.engaged = true;
        state.engage.target = target->GetGUID();
        state.engage.targetName = target->GetName();
        state.engage.predicted = verdict.damage;
        state.engage.health = float(bot->GetHealth());
        state.engage.healthPct = bot->GetHealthPct();
        state.engage.manaPct = bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 100.0f;
        state.engage.mobs = verdict.mobs;
    }

    bool WantsRest(Player* bot, bool mana, std::string* why)
    {
        if (!bot)
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end())
            return false;
        // [guild14] a fight that ended (a disengage, a won fight with no next pull judged yet) is closed out of combat
        CloseIfOver(bot, found->second);
        if (!found->second.rest)
            return false;
        SoloBotState& state = found->second;
        if (bot->IsInCombat() || !bot->IsAlive())
        {
            if (bot->IsInCombat())
                state.rest = false;
            return false;
        }
        bool const healthFull = bot->GetHealth() >= bot->GetMaxHealth();
        bool const manaFull =
            !bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA);
        if (healthFull && manaFull)
        {
            state.rest = false;
            return false;
        }
        bool const wants = mana ? !manaFull : !healthFull;
        if (wants && why)
            *why = Acore::StringFormat("{} {:.0f} % < 100 %: {}", mana ? "mana" : "hp",
                                       mana ? bot->GetPowerPct(POWER_MANA) : bot->GetHealthPct(), state.restWhy);
        return wants;
    }

    void RequestRest(Player* bot, std::string const& why)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        if (!state.rest)
            LOG_INFO("playerbots", "[solo] {} L{} rests to full: {}", bot->GetName(), bot->GetLevel(), why);
        state.rest = true;
        state.restWhy = why;
    }

    bool WaitsToRegen(Player* bot)
    {
        if (!bot || bot->IsInCombat())
            return false;
        bool const health = WantsRest(bot, false);
        bool const mana = WantsRest(bot, true);
        // something to eat or drink for what it lacks: the rest actions sit it down with that
        bool const eats = health && CountBagFood(bot, RESTOCK_SPELL_CATEGORY_FOOD);
        bool const drinks = mana && CountBagFood(bot, RESTOCK_SPELL_CATEGORY_DRINK);
        return (health || mana) && !eats && !drinks;
    }

    bool RestBeforeWalk(Player* bot, Position const& dest)
    {
        if (!bot || bot->IsInCombat() || !bot->IsAlive() || !Applies(bot))
            return false;
        bool const full = bot->GetHealth() >= bot->GetMaxHealth() &&
                          (!bot->GetMaxPower(POWER_MANA) || bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA));
        if (full)
            return false;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto const found = states.find(bot->GetGUID().GetCounter());
            // a rest asked for (a pull, a death, this walk): no walk on until full - a step stands it up from its food
            if (found != states.end() && found->second.rest)
                return true;
        }
        // the stretch it sees of the walk
        float const fromX = bot->GetPositionX();
        float const fromY = bot->GetPositionY();
        float const total = bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
        if (total <= 0.0f)
            return false;
        float const share = std::min(1.0f, sPlayerbotAIConfig.SightDistance / total);
        float const toX = fromX + (dest.GetPositionX() - fromX) * share;
        float const toY = fromY + (dest.GetPositionY() - fromY) * share;
        float const dx = toX - fromX;
        float const dy = toY - fromY;
        float const lengthSq = dx * dx + dy * dy;
        // the first idle mob on it whose reach (aggro range, its wander area) the stretch enters
        Creature* first = nullptr;
        float firstT = 2.0f;
        for (ObjectGuid const& guid :
             GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get())
        {
            Creature* mob = ObjectAccessor::GetCreature(*bot, guid);
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID())
                continue;
            float reach = mob->GetAggroRange(bot) + bot->GetCombatReach();
            Position centre = mob->GetPosition();
            if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
            {
                centre = mob->GetHomePosition();
                reach += mob->GetWanderDistance();
            }
            float t = lengthSq > 0.0f
                          ? ((centre.GetPositionX() - fromX) * dx + (centre.GetPositionY() - fromY) * dy) / lengthSq
                          : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            float const ex = fromX + dx * t - centre.GetPositionX();
            float const ey = fromY + dy * t - centre.GetPositionY();
            if (ex * ex + ey * ey > reach * reach || t >= firstT)
                continue;
            first = mob;
            firstT = t;
        }
        if (!first)
            return false;
        Verdict const verdict = Judge(bot, first);
        if (verdict.kind != Verdict::Kind::RestFirst)
            return false;
        RequestRest(bot, std::string("before walking past ") + first->GetName() + ": " + verdict.why);
        return true;
    }

    void OnRepaired(Player* bot)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end())
            return;
        SoloBotState& state = found->second;
        // [guild15] its damage was measured with broken gear (b308: 12 of 50 bots wore broken weapons, hunters dealt
        // 4-5 dps): measured again from here
        state.dealtSum = 0.0;
        state.fightSecSum = 0.0;
        state.manaSum = 0.0;
        state.healedSum = 0.0;
        state.fights = 0;
        state.gapDealt.clear();
        state.lostTo.clear();
        state.ownTaken.clear();
        state.ownDealt.clear();
    }

    void OnDeath(Player* bot)
    {
        if (!SoloPlayerBot(bot))
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        CloseFight(bot, state, true);
        state.recover = true;
        state.rest = true;
        state.restWhy = "after a death";
    }

    // [guild80] the open solo fight projected (the attackers' damage still to come against its health and heals),
    // computed as the b304 disengage judged it; SurvivalKit picks the escape from it
    Projection Project(Player* bot)
    {
        Projection out;
        if (!bot || !bot->IsAlive() || !bot->IsInCombat() || !Applies(bot))
            return out;
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.open)
            return out;
        SoloBotState& state = found->second;
        out.open = true;
        out.fleeing = state.fleeing;

        // the attackers still on it (a mob on its pet hits the pet), their damage: measured this fight (each from its
        // first hit, a swing added), the model before their first hit - the higher
        int32 gap = std::numeric_limits<int32>::min();
        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != bot)
                continue;
            float dps = MobDps(attacker, bot);
            auto const track = state.attackers.find(attacker->GetGUID().GetRawValue());
            // its measurement this fight once it has swung twice (a single hit, a crit, is no rate), else the model
            if (track != state.attackers.end() && track->second.damage &&
                getMSTimeDiff(track->second.firstMs, track->second.lastMs) >=
                    uint32(track->second.swingSec * IN_MILLISECONDS))
                dps = float(track->second.damage) /
                      (float(getMSTimeDiff(track->second.firstMs, now)) / IN_MILLISECONDS + track->second.swingSec);
            Projection::Foe foe;
            foe.unit = attacker;
            foe.hp = float(attacker->GetHealth());
            foe.dps = dps;
            if (Creature* creature = attacker->ToCreature())
            {
                foe.spellDps = SpellDpsOf(creature);
                out.leashSec = std::max(out.leashSec, float(creature->GetLeashTimer()));
            }
            out.foes.push_back(foe);
            out.incoming += dps;
            gap = std::max(gap, int32(attacker->GetLevel()) - int32(bot->GetLevel()));
        }
        if (out.foes.empty() || out.incoming <= 0.0f)
            return out;

        // its damage: the higher of its measurement at this level gap and this fight's once it has swung (b304 run 1:
        // this fight's first seconds alone - a caster's first bolt still in flight - sent 9 bots running in 10 min)
        float const elapsed = float(getMSTimeDiff(state.firstMs, now)) / IN_MILLISECONDS;
        float botDps = BotDpsAt(bot, state, gap);
        if (state.dealt && elapsed >= SwingSec(bot))
            botDps = std::max(botDps, float(state.dealt) / (elapsed + SwingSec(bot)));
        out.botDps = std::max(botDps, 1.0f);
        // the rest of the fight as the judge counts it: its current target first, then the most damage per health
        Unit* const victim = bot->GetVictim();
        std::sort(out.foes.begin(), out.foes.end(),
                  [victim](Projection::Foe const& a, Projection::Foe const& b)
                  {
                      if ((a.unit == victim) != (b.unit == victim))
                          return a.unit == victim;
                      return a.dps / std::max(a.hp, 1.0f) > b.dps / std::max(b.hp, 1.0f);
                  });
        for (Projection::Foe const& foe : out.foes)
        {
            out.seconds += foe.hp / out.botDps;
            out.damage += foe.dps * out.seconds;
        }
        out.heals = state.fightSecSum > 0.0 ? float(state.healedSum / state.fightSecSum) * out.seconds : 0.0f;
        if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
            out.heals *= std::min(1.0f, float(bot->GetPower(POWER_MANA)) / float(maxMana));
        out.health = float(bot->GetHealth());
        out.lost = out.damage >= out.health + out.heals;
        return out;
    }

    void BeginFlee(Player* bot, std::string const& why)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.open)
            return;
        found->second.fleeing = true;
        found->second.rest = true;
        found->second.restWhy = why;
    }

    void RequestRestAfter(Player* bot, std::string const& why)
    {
        if (!bot)
            return;
        std::lock_guard<std::mutex> guard(lock);
        SoloBotState& state = states[bot->GetGUID().GetCounter()];
        state.rest = true;
        state.restWhy = why;
    }

    // [guild14] the b304 disengage (out of CombatStrategy since b309); [guild80] SurvivalKit decides the flee now
    bool ShouldDisengage(Player* bot) { return Disengaging(bot); }

    bool Disengaging(Player* bot)
    {
        if (!bot || !bot->IsInCombat())
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        return found != states.end() && found->second.open && found->second.fleeing;
    }

    bool TakeRecovery(Player* bot)
    {
        if (!bot || !bot->IsAlive())
            return false;
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.recover)
            return false;
        found->second.recover = false;
        return true;
    }

    std::string DeathContext(Player* bot)
    {
        std::string out = Acore::StringFormat("hp {:.0f}/{:.0f}", float(bot->GetHealth()), float(bot->GetMaxHealth()));
        std::string attackers;
        uint32 count = 0;
        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker)
                continue;
            ++count;
            attackers += (attackers.empty() ? "" : ", ") + attacker->GetName() + " L" +
                         std::to_string(attacker->GetLevel()) + " " +
                         std::to_string(uint32(attacker->GetHealthPct())) + "%";
        }
        out += Acore::StringFormat("; {} attackers: {}", count, attackers);
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found != states.end() && !found->second.attackers.empty())
        {
            std::string came;
            for (auto const& [guid, track] : found->second.attackers)
                came += (came.empty() ? "" : ", ") + track.name + " L" + std::to_string(track.level);
            out += Acore::StringFormat("; hit by {} this fight: {}", found->second.attackers.size(), came);
        }
        if (found != states.end() && found->second.engaged)
        {
            EngageNote const& engage = found->second.engage;
            out += Acore::StringFormat("; pulled {} judged {} mobs {:.0f} dmg at hp {:.0f} % mana {:.0f} %",
                                       engage.targetName, engage.mobs, engage.predicted, engage.healthPct,
                                       engage.manaPct);
        }
        else
            out += "; no judged pull (attacked)";
        return out;
    }

    void RouteCosts(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& to, std::vector<G3D::Vector4>& refuse,
                    std::vector<Leader::Travel::RoutePenalty>& penalties)
    {
        Player* bot = walker ? walker->ToPlayer() : nullptr;
        if (!bot || bot->IsInCombat() || !bot->IsAlive() || !Applies(bot))
            return;
        // the walk's first leg as far as the bot sees: the rest is planned again from there
        float const range = sPlayerbotAIConfig.SightDistance;
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);
        float const dps = std::max(BotDps(bot), 1.0f);
        float const speed = bot->GetSpeed(MOVE_RUN);
        ObjectGuid const target = bot->GetTarget();
        uint8 const level = bot->GetLevel();
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || mob->IsCivilian() || mob->IsTotem() ||
                mob->GetGUID() == target || !mob->IsHostileTo(bot) || !mob->HasReactState(REACT_AGGRESSIVE) ||
                mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) ||
                mob->GetCharmerOrOwnerGUID())
                continue;
            float const aggro = mob->GetAggroRange(bot);
            if (aggro <= 0.0f)
                continue;
            Position centre = mob->GetPosition();
            float radius = aggro + bot->GetCombatReach();
            if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
            {
                centre = mob->GetHomePosition();
                radius += mob->GetWanderDistance();
            }
            // never walked through: orange / red con, an elite, a mob in a remembered danger zone (where it died, a
            // pack its pull judge refused)
            bool never = IsElite(mob) || Acore::XP::GetColorCode(level, mob->GetLevel()) <= XP_ORANGE ||
                         NewRpgBaseAction::InDangerZone(bot, mob->GetPositionX(), mob->GetPositionY());
            // [guild15] ... and a mob whose fight alone the bot loses now: its damage (the model, every bot's and this
            // bot's measurement) over the time the bot's damage needs for its health, against the bot's health (b315:
            // 12 of 14 deaths were bots attacked on their way, most through Gnarlpine / Wendigo / murloc camps)
            if (!never)
            {
                std::lock_guard<std::mutex> guard(lock);
                float mobDps = MobDps(mob, bot);
                mobDps = std::max(mobDps, Rate(states[bot->GetGUID().GetCounter()].ownTaken, mob->GetEntry()));
                never = mobDps * float(mob->GetHealth()) / dps >= float(bot->GetHealth());
            }
            if (never)
                refuse.emplace_back(centre.GetPositionX(), centre.GetPositionY(), centre.GetPositionZ(), radius);
            // the fight it starts, in yards of walk: its health over the bot's damage, at the bot's run speed
            Leader::Travel::RoutePenalty penalty;
            penalty.x = centre.GetPositionX();
            penalty.y = centre.GetPositionY();
            penalty.z = centre.GetPositionZ();
            penalty.r = radius;
            penalty.cost = float(mob->GetHealth()) / dps * speed;
            // a mob it must never fight, when no walk round it is complete: any detour up to the whole trip again is
            // the better walk (b301: Thokhen L3 walked 4 times through the same L9 Nightstalkers on a 1943 yd walk)
            if (never)
                penalty.cost = std::max(penalty.cost, (to - from).length() + 2.0f * radius);
            penalties.push_back(penalty);
        }
    }
}
