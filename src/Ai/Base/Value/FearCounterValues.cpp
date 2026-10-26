/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FearCounterValues.h"

#include "BotMoveEvents.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SmartScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <mutex>
#include <unordered_map>

namespace
{
// [fear_counter] C++-scripted bosses have no spell list the bot can read (their spells live in the script), so
// the ones with a fear / charm / sleep known from the AC scripts are listed here. Everything SmartAI-driven or
// with creature_template spells (Princess Theradras 12201 Repulsive Gaze 21869, ...) is found without a row, and
// any other boss is learnt at its first such cast or the first party member it fears.
struct ScriptedFearSource
{
    uint32 entry;
    uint8 threat;
};

constexpr uint8 FEAR = FearCounter::THREAT_TREMOR | FearCounter::THREAT_FEAR;
constexpr uint8 CC = FearCounter::THREAT_TREMOR;  // charm / sleep: Tremor removes it, Fear Ward does not

// Audit of src/server/scripts against Spell.dbc (aura 7 / mechanic 5 fear; aura 6 / 177 / mechanic 1 charm;
// mechanic 10 sleep), 2026-10-25. Horrors (Falric 72435, Death Coils) are left out: Tremor and Fear Ward do not
// touch them. Ingvar's Dreadful Roar 42729 is a damage + lockout, not a fear.
constexpr ScriptedFearSource SCRIPTED_FEAR_SOURCES[] = {
    // Classic
    {10184, FEAR},  // Onyxia: Bellowing Roar 18431
    {11583, FEAR},  // Nefarian: Fear 22678, Bellowing Roar 22686
    {10162, CC},    // Lord Victor Nefarius: Shadow Command 22667
    {14020, CC},    // Chromaggus: Chromatic Mutation 23174
    {11982, FEAR},  // Magmadar: Panic 19408
    {10430, FEAR},  // The Beast: Terrifying Roar 14100
    {10584, FEAR},  // Urok Doomhowl: Intimidating Roar 16508
    {16080, CC},    // Mor Grayhoof: Sleep 20663
    {9018, FEAR},   // High Interrogator Gerstahn: Psychic Scream 13704
    {10506, CC},    // Kirtonos the Herald: Dominate Mind 14515
    {10436, CC},    // Baroness Anastari: Possess 17244
    {16102, FEAR},  // Sothos: Fear 27641
    {14834, CC},    // Hakkar: Cause Insanity 24327
    {14517, FEAR},  // High Priestess Jeklik: Psychic Scream 22884
    {15083, CC},    // Hazza'rah: Sleep 24664
    {11382, FEAR},  // Bloodlord Mandokir: Frightening Shout 19134
    {7275, FEAR},   // Shadowpriest Sezz'ziz: Psychic Scream 13704
    {15543, FEAR},  // Princess Yauj: Fear 26580
    {15263, CC},    // Prophet Skeram: True Fulfillment 785
    {15311, FEAR},  // Anubisath Warder: Fear 26070
    {17808, CC},    // Anetheron: Sleep 31298
    {17968, FEAR},  // Archimonde: Fear 31970
    {26533, CC},    // Mal'Ganis: Sleep 52721
    {17848, FEAR},  // Lieutenant Drake: Frightening Shout 33789
    // The Burning Crusade
    {17225, FEAR},       // Nightbane: Bellowing Roar 36922
    {17521, FEAR},       // The Big Bad Wolf: Terrifying Howl 30752
    {17535, FEAR},       // Dorothee: Frightened Scream 31013
    {17546, FEAR},       // Roar: Frightened Scream 31013
    {16180, FEAR},       // Shadikith the Glider: Fear 29321
    {24239, FEAR | CC},  // Hex Lord Malacrass: Psychic Scream 43432, Mind Control 43550
    {24558, FEAR},       // Ellrys Duskhallow: Fear 38595
    {24559, FEAR},       // Warlord Salaris: Frightening Shout 19134
    {25038, CC},         // Felmyst: Fog of Corruption 45717 / 45726
    {18373, FEAR},       // Exarch Maladaar: Soul Scream 32421
    {18731, FEAR},       // Ambassador Hellmaw: Fear 33547
    {18667, CC},         // Blackheart the Inciter: Incite Chaos 33676 / 33684
    {18831, FEAR},       // High King Maulgar: Intimidating Roar 16508
    {17536, FEAR},       // Nazan: Bellowing Roar 39427
    {16809, FEAR},       // Warbringer O'mrogg: Fear 30584
    {17381, CC},         // The Maker: Domination 30923
    {19220, CC},         // Pathaleon the Calculator: Domination 35280
    {20912, FEAR | CC},  // Harbinger Skyriss: Fear 39415, Domination 37162
    {18805, FEAR},       // High Astromancer Solarian: Psychic Scream 34322
    {19622, CC},         // Kael'thas Sunstrider: Mind Control 36797
    {20060, FEAR},       // Lord Sanguinar: Bellowing Roar 44863
    {21215, CC},         // Leotheras the Blind: Consuming Madness 37749
    // Wrath of the Lich King
    {27483, FEAR},       // King Dred: Bellowing Roar 22686
    {26684, FEAR},       // Ravenous Furbolg (Palehoof): Terrifying Roar 48144
    {26798, FEAR},       // Commander Kolurg: Frightening Shout 19134
    {26796, FEAR},       // Commander Stoutbeard: Frightening Shout 19134
    {36497, FEAR},       // Bronjahm: Fear 68950
    {38175, FEAR},       // Ghostly Priest (HoR): Cower in Fear 72321
    {35307, CC},         // Argent Priestess: Mind Control 67229
    {34942, FEAR},       // Memory of Hogger (Paletress' memories): Waking Nightmare 66552
    {34447, FEAR},       // Faction Champions disc priest: Psychic Scream 65543
    {34466, FEAR},
    {34450, FEAR},       // Faction Champions warlock: Fear 65809
    {34474, FEAR},
    {34453, FEAR},       // Faction Champions warrior: Intimidating Shout 65930
    {34475, FEAR},
    {34448, CC},         // Faction Champions hunter: Wyvern Sting 65877
    {34467, CC},
    {36855, CC},         // Lady Deathwhisper: Dominate Mind 71289
    {38472, FEAR},       // Darnavan: Intimidating Shout 65930
    {38485, FEAR},
    {37122, CC},         // Captain Arnath: Dominate Mind 14515
    {37534, FEAR},       // Spinestalker: Bellowing Roar 36922
    {37955, FEAR | CC},  // Blood-Queen Lana'thel: Incite Terror 73070, Uncontrollable Frenzy 70923
    {15990, CC},         // Kel'Thuzad: Chains of Kel'Thuzad 28410
    {33515, FEAR},       // Auriaya: Terrifying Screech 64386
    {33288, FEAR | CC},  // Yogg-Saron: Malady of the Mind 63830, Insane 63120
    {33134, FEAR},       // Sara: Malady of the Mind 63830
    {39746, FEAR},       // General Zarithrian: Intimidating Roar 74384
};

// Seen-in-combat range for an out-of-combat pull target (pre-pull Fear Ward).
constexpr float PREPULL_TARGET_RANGE = 60.0f;
// In combat the value is read at least every 500 ms; a longer silence means the fight ended in between.
constexpr uint32 FIGHT_GAP_MS = 3 * IN_MILLISECONDS;

uint8 SpellThreatImpl(uint32 spellId, uint8 depth)
{
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!info || info->IsPositive())
        return FearCounter::THREAT_NONE;

    uint8 threat = FearCounter::THREAT_NONE;
    uint64 const mechanics = info->GetAllEffectsMechanicMask();
    if (mechanics & (1ULL << MECHANIC_FEAR))
        threat |= FEAR;
    if (mechanics & ((1ULL << MECHANIC_CHARM) | (1ULL << MECHANIC_SLEEP)))
        threat |= CC;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = info->Effects[i];
        if (effect.IsAura(SPELL_AURA_MOD_FEAR))
            threat |= FEAR;
        else if (effect.IsAura(SPELL_AURA_MOD_CHARM) || effect.IsAura(SPELL_AURA_AOE_CHARM))
            threat |= CC;

        // Roars and gazes are often a dummy / trigger shell around the real fear.
        bool const triggers = effect.Effect == SPELL_EFFECT_TRIGGER_SPELL ||
                              effect.Effect == SPELL_EFFECT_TRIGGER_MISSILE ||
                              effect.IsAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        if (triggers && depth == 0 && effect.TriggerSpell && effect.TriggerSpell != spellId)
            threat |= SpellThreatImpl(effect.TriggerSpell, depth + 1);
    }

    return threat;
}

uint8 ComputeCreatureThreat(Creature* creature)
{
    uint8 threat = FearCounter::THREAT_NONE;
    CreatureTemplate const* tmpl = creature->GetCreatureTemplate();
    if (tmpl)
        for (uint8 i = 0; i < MAX_CREATURE_SPELLS; ++i)
            threat |= FearCounter::SpellThreat(tmpl->spells[i]);

    uint32 const entry = creature->GetEntry();
    if (tmpl && tmpl->AIName == "SmartAI")
    {
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE))
        {
            switch (holder.GetActionType())
            {
                case SMART_ACTION_CAST:
                case SMART_ACTION_SELF_CAST:
                case SMART_ACTION_INVOKER_CAST:
                    threat |= FearCounter::SpellThreat(holder.action.cast.spell);
                    break;
                case SMART_ACTION_CROSS_CAST:
                    threat |= FearCounter::SpellThreat(holder.action.crossCast.spell);
                    break;
                default:
                    break;
            }
        }
    }

    for (ScriptedFearSource const& source : SCRIPTED_FEAR_SOURCES)
        if (source.entry && source.entry == entry)
            threat |= source.threat;

    return threat;
}

std::mutex& CreatureThreatLock()
{
    static std::mutex lock;
    return lock;
}

std::unordered_map<uint32, uint8>& CreatureThreatCache()
{
    static std::unordered_map<uint32, uint8> cache;
    return cache;
}

// The aura's spell id of the first aura of this type on the unit (its caster into caster), 0 when none.
uint32 FirstAuraOfType(Unit* unit, AuraType type, ObjectGuid* caster)
{
    Unit::AuraEffectList const& effects = unit->GetAuraEffectsByType(type);
    if (effects.empty())
        return 0;

    if (caster)
        *caster = effects.front()->GetCasterGUID();
    return effects.front()->GetId();
}
}  // namespace

namespace FearCounter
{
uint8 SpellThreat(uint32 spellId) { return SpellThreatImpl(spellId, 0); }

uint8 CreatureThreat(Creature* creature)
{
    if (!creature)
        return THREAT_NONE;

    uint32 const entry = creature->GetEntry();
    {
        std::lock_guard<std::mutex> guard(CreatureThreatLock());
        auto const it = CreatureThreatCache().find(entry);
        if (it != CreatureThreatCache().end())
            return it->second;
    }

    // Computed outside the lock: the spell store and the SmartAI map are read-only while the world runs.
    uint8 const threat = ComputeCreatureThreat(creature) & (THREAT_TREMOR | THREAT_FEAR);
    std::lock_guard<std::mutex> guard(CreatureThreatLock());
    return CreatureThreatCache()[entry] |= threat;
}

void LearnCreatureThreat(uint32 entry, uint8 threat)
{
    threat &= THREAT_TREMOR | THREAT_FEAR;
    if (!entry || !threat)
        return;

    std::lock_guard<std::mutex> guard(CreatureThreatLock());
    auto const it = CreatureThreatCache().find(entry);
    // Not computed yet: leave it to CreatureThreat, which would otherwise skip its own list.
    if (it != CreatureThreatCache().end())
        it->second |= threat;
}

uint32 TremorableAuraSpell(Unit* unit, bool fearOnly, ObjectGuid* caster)
{
    if (!unit)
        return 0;

    if (uint32 const fear = FirstAuraOfType(unit, SPELL_AURA_MOD_FEAR, caster))
        return fear;

    if (fearOnly)
        return 0;

    if (uint32 const charm = FirstAuraOfType(unit, SPELL_AURA_MOD_CHARM, caster))
        return charm;

    if (uint32 const charm = FirstAuraOfType(unit, SPELL_AURA_AOE_CHARM, caster))
        return charm;

    if (!unit->HasAuraWithMechanic(1ULL << MECHANIC_SLEEP))
        return 0;

    for (auto const& [spellId, application] : unit->GetAppliedAuras())
    {
        Aura const* aura = application ? application->GetBase() : nullptr;
        if (aura && (aura->GetSpellInfo()->GetAllEffectsMechanicMask() & (1ULL << MECHANIC_SLEEP)))
        {
            if (caster)
                *caster = aura->GetCasterGUID();
            return spellId;
        }
    }

    return 0;
}

bool TremorCovers(Player* bot, Unit* anchor)
{
    ObjectGuid const guid = bot->m_SummonSlot[SUMMON_SLOT_TOTEM_EARTH];
    if (guid.IsEmpty())
        return false;

    Creature* totem = bot->GetMap()->GetCreature(guid);
    if (!totem || !totem->IsAlive() || totem->GetUInt32Value(UNIT_CREATED_BY_SPELL) != SPELL_TREMOR_TOTEM)
        return false;

    return !anchor || totem->IsWithinDistInMap(anchor, TREMOR_RADIUS);
}

Unit* TremorAnchor(PlayerbotAI* botAI, FearCounterState const& state, bool reactive)
{
    Player* bot = botAI->GetBot();
    if (reactive)
        return state.victim.IsEmpty() ? nullptr : botAI->GetUnit(state.victim);

    Unit* tank = state.tank.IsEmpty() ? nullptr : botAI->GetUnit(state.tank);
    if (tank && tank->IsAlive() && bot->IsWithinDistInMap(tank, TREMOR_DROP_REACH))
        return tank;

    return bot;
}

void Log(Player* bot, std::string const& action, Unit* victim, uint32 auraSpell)
{
    std::string const victimName = victim ? victim->GetName() : std::string("-");
    LOG_INFO("playerbots", "[fear_counter] {{bot: {}, action: {}, victim: {}, auraSpell: {}}}", bot->GetName(), action,
             victimName, auraSpell);
    BotMoveEvents::Push(bot->GetGUID(), BotMoveEvents::Event{"fear_counter", {}, {}}
                                            .Str("bot", bot->GetName())
                                            .Str("action", action)
                                            .Str("victim", victimName)
                                            .Num("auraSpell", auraSpell));
}
}  // namespace FearCounter

FearCounterState FearCounterStateValue::Calculate()
{
    FearCounterState state;
    bool const inCombat = bot->IsInCombat();
    uint32 const now = getMSTime();
    if (!inCombat || (_lastInCombatMs && getMSTimeDiff(_lastInCombatMs, now) > FIGHT_GAP_MS))
        _seenThisFight = FearCounter::THREAT_NONE;
    _lastInCombatMs = inCombat ? now : 0;

    // The party: the tank, and who is feared / charmed / asleep now.
    Player* tank = nullptr;
    bool victimIsTank = false;
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;

        bool const isTank = PlayerbotAI::IsTank(member);
        if (isTank && (!tank || PlayerbotAI::IsMainTank(member)))
            tank = member;

        if (!inCombat || !member->IsInCombat())
            continue;

        ObjectGuid caster;
        uint32 const aura = FearCounter::TremorableAuraSpell(member, false, &caster);
        if (!aura)
            continue;

        uint8 const seen = FearCounter::THREAT_TREMOR |
                           (member->HasAuraType(SPELL_AURA_MOD_FEAR) ? FearCounter::THREAT_FEAR : 0);
        _seenThisFight |= seen;
        // A C++ boss is remembered by the aura's caster: its next pull starts with the counter up.
        if (caster.IsCreature())
            FearCounter::LearnCreatureThreat(caster.GetEntry(), seen);

        // The totem lands at the bot's feet: only a victim within its pulse is helped by it. The tank first.
        if (!bot->IsWithinDistInMap(member, FearCounter::TREMOR_RADIUS) ||
            (!state.victim.IsEmpty() && (victimIsTank || !isTank)))
            continue;

        state.victim = member->GetGUID();
        state.victimAura = aura;
        victimIsTank = isTank;
    }

    if (tank)
        state.tank = tank->GetGUID();

    state.threat = _seenThisFight;

    if (inCombat)
    {
        // The fight's mobs: a known fear source, or a fear-type cast in progress (learnt for the entry).
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
        {
            Unit* unit = botAI->GetUnit(guid);
            Creature* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive())
                continue;

            uint8 threat = FearCounter::CreatureThreat(creature);
            Spell const* cast = creature->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!cast)
                cast = creature->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

            uint8 const castThreat = cast ? FearCounter::SpellThreat(cast->GetSpellInfo()->Id) : 0;
            if (castThreat)
            {
                FearCounter::LearnCreatureThreat(creature->GetEntry(), castThreat);
                threat |= castThreat;
                if (bot->IsWithinDistInMap(creature, FearCounter::INCOMING_RADIUS))
                    threat |= FearCounter::THREAT_INCOMING;
            }

            if (threat && state.source.IsEmpty())
            {
                state.source = guid;
                state.sourceSpell = castThreat ? cast->GetSpellInfo()->Id : 0;
            }

            state.threat |= threat;
        }
    }
    else if (tank)
    {
        // Before the pull: the mob the tank has selected (pre-pull Fear Ward).
        Unit* target = ObjectAccessor::GetUnit(*bot, tank->GetTarget());
        Creature* creature = target ? target->ToCreature() : nullptr;
        if (creature && creature->IsAlive() && bot->IsValidAttackTarget(creature) &&
            bot->IsWithinDistInMap(creature, PREPULL_TARGET_RANGE))
        {
            state.threat |= FearCounter::CreatureThreat(creature);
            if (state.threat)
                state.source = creature->GetGUID();
        }
    }

    return state;
}
