/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldBossTactics.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "DynamicObject.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "LeaderFightChecks.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"

namespace WorldBoss
{
namespace
{
constexpr float FIGHT_RANGE = 60.0f;  // a world boss this close and in combat is the bot's fight
constexpr float SPELL_REACH = 40.0f;  // dispels, heals, the raid-damage count
constexpr float INTERRUPT_SCAN = 35.0f;
constexpr float TAUNT_RANGE = 30.0f;
constexpr float SPREAD_DISTANCE = 9.0f;    // Chain Lightning / Lightning Wave jump range is 8-10 yd
constexpr float CARRIER_DISTANCE = 15.0f;  // Living Bomb / Mark of Kazzak explosion radius 10-12 yd
constexpr float DREAM_FOG_RADIUS = 8.0f;
constexpr float MANASTORM_RADIUS = 12.0f;
constexpr float SIDE_CONE = float(M_PI) / 3.6f;   // 50 deg off the boss's front or back: breath and tail sweep
constexpr float FRONT_CONE = float(M_PI) / 4.0f;  // 45 deg: Meteor Slash cone
constexpr float RAID_HURT_PCT = 70.0f;
constexpr uint8 RAID_HURT_COUNT = 5;        // members under RAID_HURT_PCT before a raid cooldown
constexpr uint8 RAID_PHASE_WINDOW_PCT = 5;  // a raid phase "starts" over this many % below its threshold
constexpr uint32 SNAPSHOT_MS = 500;         // how often the per-bot fight picture is recomputed

// tank debuffs: Mortal Wound (-10 % healing), Crush Armor (-10 % armor), Noxious Breath (longer cooldowns),
// Meteor Slash (+75 % fire damage taken per stack)
constexpr uint32 SPELL_MORTAL_WOUND = 25646;
constexpr uint32 SPELL_MORTAL_WOUND_GLUTH = 28467;
constexpr uint32 SPELL_CRUSH_ARMOR = 33661;
constexpr uint32 SPELL_NOXIOUS_BREATH = 24818;
constexpr uint32 SPELL_METEOR_SLASH = 45150;

BossTactic const TACTICS[] = {
    {NPC_LORD_KAZZAK, SPELL_MORTAL_WOUND, 4, false, false, false, {66, 33}},
    {NPC_AZUREGOS, SPELL_MORTAL_WOUND_GLUTH, 4, true, false, false, {66, 33}},
    {NPC_DOOMWALKER, SPELL_CRUSH_ARMOR, 4, false, false, true, {66, 33}},
    {NPC_DOOM_LORD_KAZZAK, SPELL_MORTAL_WOUND, 4, false, false, false, {66, 33}},
    {NPC_TAERAR, SPELL_NOXIOUS_BREATH, 3, true, false, false, {75, 50, 25}},
    {NPC_EMERISS, SPELL_NOXIOUS_BREATH, 3, true, false, false, {75, 50, 25}},
    {NPC_LETHON, SPELL_NOXIOUS_BREATH, 3, true, false, false, {75, 50, 25}},
    {NPC_YSONDRE, SPELL_NOXIOUS_BREATH, 3, true, false, true, {75, 50, 25}},
    {NPC_KRUUL, SPELL_METEOR_SLASH, 3, false, true, false, {75, 50, 25}},
};

enum class DispelType : uint8
{
    Magic,
    Curse,
    Disease
};

struct Debuff
{
    uint32 spellId;
    DispelType type;
};

// Chill (Azuregos), Twisted Reflection (both Kazzaks, Kruul), Mark of Kazzak (curse), Volatile Infection (Emeriss)
Debuff const DISPELS[] = {
    {21098, DispelType::Magic},
    {21063, DispelType::Magic},
    {21056, DispelType::Curse},
    {SPELL_VOLATILE_INFECTION, DispelType::Disease},
};

char const* DispelSpell(Player* bot, DispelType type)
{
    switch (bot->getClass())
    {
        case CLASS_PRIEST:
            return type == DispelType::Magic ? "dispel magic" : type == DispelType::Disease ? "cure disease" : nullptr;
        case CLASS_PALADIN:
            return type == DispelType::Magic ? "cleanse" : type == DispelType::Disease ? "cleanse" : nullptr;
        case CLASS_SHAMAN:
            return type == DispelType::Curse ? "cleanse spirit" : type == DispelType::Disease ? "cure toxins" : nullptr;
        case CLASS_DRUID:
            return type == DispelType::Curse ? "remove curse" : nullptr;
        case CLASS_MAGE:
            return type == DispelType::Curse ? "remove curse" : nullptr;
        default:
            return nullptr;
    }
}

bool IsHealerAdd(Unit* unit)
{
    // the SmartAI healers of gen_world_bosses.js are the add kind "healer" (k = 1, Nightmare Phantasm k = 0):
    // identified by what they do, not by their entry - they cast Heal on the boss
    return unit && unit->IsCreature() && unit->GetEntry() >= NPC_ADD_FIRST && unit->GetEntry() <= NPC_ADD_LAST &&
           unit->ToCreature()->GetCreatureTemplate()->AIName == "SmartAI";
}

bool IsBossAdd(Unit* unit)
{
    if (!unit || !unit->IsCreature())
        return false;
    uint32 const entry = unit->GetEntry();
    return (entry >= NPC_ADD_FIRST && entry <= NPC_ADD_LAST) || entry == 15302 /*Shade of Taerar*/ ||
           entry == 15260 /*Demented Druid Spirit*/;
}

bool CastingAddHeal(Unit* unit)
{
    Spell* spell = unit ? unit->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    return spell && spell->m_spellInfo && spell->m_spellInfo->Id == SPELL_ADD_HEAL;
}

float AngleOffFront(Unit* boss, WorldObject* at)
{
    float diff = std::fabs(boss->GetAngle(at) - boss->GetOrientation());
    if (diff > float(M_PI))
        diff = 2.0f * float(M_PI) - diff;
    return diff;  // 0 = in front, PI = behind
}

// the fight picture of one bot, recomputed every SNAPSHOT_MS while it fights (the triggers read it: O(1))
struct Snapshot
{
    uint32 at = 0;
    ObjectGuid boss;
    ObjectGuid priorityAdd;
    ObjectGuid healCaster;
    ObjectGuid looseAdd;
    ObjectGuid dispelTarget;
    std::string dispelSpell;
    ObjectGuid dreamFog;
    bool crowded = false;    // a group member within SPREAD_DISTANCE
    uint8 hurt = 0;          // members in reach under RAID_HURT_PCT
    bool raidPhase = false;  // a raid-damage phase is on (threshold just crossed, Earthquake, Corruption of the Earth)
};

// a per-thread cache keyed by the bot (a map's bots tick on any MapUpdater worker: each worker recomputes its own copy
// at most every SNAPSHOT_MS; the picture is a cache, no decision state)
thread_local std::unordered_map<ObjectGuid, Snapshot> tSnapshots;

void NoteFightChanges(Player* bot, Unit* boss);  // [kzk1] below
void NoteTankMana(Player* bot, PlayerbotAI* botAI, Unit* boss);

Snapshot const& Picture(Player* bot, PlayerbotAI* botAI)
{
    static Snapshot const NONE;
    if (!bot->IsInCombat())
        return NONE;  // out of combat (the non-combat engine, questing): no lookup at all
    Snapshot& s = tSnapshots[bot->GetGUID()];
    uint32 const now = getMSTime();
    if (s.at && getMSTimeDiff(s.at, now) < SNAPSHOT_MS)
        return s;
    s = Snapshot();
    s.at = now ? now : 1;

    Unit* boss = nullptr;
    AiObjectContext* context = botAI->GetAiObjectContext();
    GuidVector const npcs = AI_VALUE(GuidVector, "nearest hostile npcs");
    std::vector<Unit*> adds;
    for (ObjectGuid const& guid : npcs)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsInCombat())
            continue;
        if (!boss && Tactic(unit->GetEntry()) && bot->GetDistance(unit) <= FIGHT_RANGE)
            boss = unit;
        else if (IsBossAdd(unit) || unit->GetEntry() == NPC_SPIRIT_SHADE)
            adds.push_back(unit);
        if (unit->GetEntry() == NPC_DREAM_FOG && bot->GetDistance(unit) < DREAM_FOG_RADIUS)
            s.dreamFog = unit->GetGUID();
    }
    if (!boss)
        return s;
    s.boss = boss->GetGUID();
    NoteFightChanges(bot, boss);
    NoteTankMana(bot, botAI, boss);

    float bestPriority = 1e9f;
    for (Unit* add : adds)
    {
        float const dist = bot->GetDistance(add);
        bool const healer = IsHealerAdd(add);
        bool const shade = add->GetEntry() == NPC_SPIRIT_SHADE;
        if ((healer || shade) && dist < bestPriority && dist <= SPELL_REACH)
        {
            bestPriority = dist;
            s.priorityAdd = add->GetGUID();
        }
        if (healer && dist <= INTERRUPT_SCAN && CastingAddHeal(add) && s.healCaster.IsEmpty())
            s.healCaster = add->GetGUID();
        if (!healer && !shade && s.looseAdd.IsEmpty())
            if (Unit* victim = add->GetVictim())
                if (Player* player = victim->ToPlayer())
                    if (!PlayerbotAI::IsTank(player))
                        s.looseAdd = add->GetGUID();
    }

    // [kzk1] the area picture of a damage dealer (every 5 s): the boss and his adds around its target in the 8 yd the
    // "aoe" triggers count (MediumAoeTrigger); the triggers and the spells' payoff decide the cast
    if (!PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot))
        if (Unit* center = bot->GetVictim())
        {
            constexpr float AOE_COUNT_RADIUS = 8.0f;
            uint32 nearAdds = 0;
            for (Unit* add : adds)
                if (add->GetDistance(center) <= AOE_COUNT_RADIUS)
                    ++nearAdds;
            bool const bossNear = boss->GetDistance(center) <= AOE_COUNT_RADIUS;
            if (nearAdds + (bossNear ? 1 : 0) >= 2)
                Note(bot, "aoe_count",
                     Acore::StringFormat("aoe: around {}: {}{} adds within {:.0f} yd", center->GetName(),
                                         bossNear ? "the boss + " : "", nearAdds, AOE_COUNT_RADIUS));
        }

    // group: dispels, crowding, the raid's health
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
                continue;
            float const dist = bot->GetDistance(member);
            if (dist > SPELL_REACH)
                continue;
            if (member != bot && dist < SPREAD_DISTANCE)
                s.crowded = true;
            if (member->GetHealthPct() < RAID_HURT_PCT)
                ++s.hurt;
            if (member->HasAura(SPELL_CORRUPTION_OF_EARTH))
                s.raidPhase = true;
            if (s.dispelTarget.IsEmpty())
                for (Debuff const& d : DISPELS)
                    if (member->HasAura(d.spellId))
                        if (char const* name = DispelSpell(bot, d.type))
                        {
                            s.dispelTarget = member->GetGUID();
                            s.dispelSpell = name;
                            break;
                        }
        }

    if (boss->HasAura(SPELL_EARTHQUAKE))
        s.raidPhase = true;
    if (BossTactic const* t = Tactic(boss->GetEntry()))
        for (uint8 threshold : t->raidPhases)
        {
            float const pct = boss->GetHealthPct();
            if (pct <= threshold && pct > float(threshold - RAID_PHASE_WINDOW_PCT))
                s.raidPhase = true;
        }
    return s;
}

Unit* SnapshotUnit(PlayerbotAI* botAI, ObjectGuid guid) { return guid.IsEmpty() ? nullptr : botAI->GetUnit(guid); }
}  // namespace

BossTactic const* Tactic(uint32 entry)
{
    for (BossTactic const& t : TACTICS)
        if (t.entry == entry)
            return &t;
    return nullptr;
}

Unit* FightBoss(Player* bot, PlayerbotAI* botAI) { return SnapshotUnit(botAI, Picture(bot, botAI).boss); }

namespace
{
// [b348] the add a damage dealer committed to: kept while it is a live priority add of the fight in reach (its
// state, not a timer). b348 wb-kazzak: 155-190 "switches to Dreadlord" per bot - the nearest priority add changed
// with every move between the two Dreadlords, and the rotation went back to the boss in between.
// [kzk1] shared under a lock, not thread_local: AzerothCore's MapUpdater hands a map's update to whichever of its
// MapUpdate.Threads workers (4 here) pops it, so one bot ticks on different threads and a thread_local map kept up to
// four commitments per bot - two Dreadlords alternated ("switches to Dreadlord (69 %)" / "(84 %)"
// back and forth for the same bot, wb-kazzak 20261108-183632)
std::mutex gStateLock;
std::unordered_map<ObjectGuid, ObjectGuid> gCommitted;

ObjectGuid CommittedGuid(Player* bot)
{
    std::lock_guard<std::mutex> guard(gStateLock);
    auto const it = gCommitted.find(bot->GetGUID());
    return it == gCommitted.end() ? ObjectGuid::Empty : it->second;
}

void SetCommitted(Player* bot, ObjectGuid add)
{
    std::lock_guard<std::mutex> guard(gStateLock);
    if (add.IsEmpty())
        gCommitted.erase(bot->GetGUID());
    else
        gCommitted[bot->GetGUID()] = add;
}

bool StillPriority(Player* bot, Unit* add)
{
    return add && add->IsAlive() && add->IsInCombat() && add->IsInWorld() && add->GetMap() == bot->GetMap() &&
           !add->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && bot->GetDistance(add) <= SPELL_REACH + 10.0f &&
           (IsHealerAdd(add) || add->GetEntry() == NPC_SPIRIT_SHADE);
}
}  // namespace

Unit* PriorityAdd(Player* bot, PlayerbotAI* botAI, Unit* /*boss*/)
{
    if (ObjectGuid const committed = CommittedGuid(bot); !committed.IsEmpty())
    {
        if (Unit* add = botAI->GetUnit(committed); StillPriority(bot, add))
            return add;
        SetCommitted(bot, ObjectGuid::Empty);
    }
    Unit* add = SnapshotUnit(botAI, Picture(bot, botAI).priorityAdd);
    if (add)
        SetCommitted(bot, add->GetGUID());
    return add;
}

Unit* CommittedAdd(Player* bot, PlayerbotAI* botAI)
{
    ObjectGuid const committed = CommittedGuid(bot);
    if (committed.IsEmpty())
        return nullptr;
    Unit* add = botAI->GetUnit(committed);
    return StillPriority(bot, add) ? add : nullptr;
}

uint8 Stacks(Unit* unit, uint32 spellId)
{
    if (!unit || !spellId)
        return 0;
    Aura const* aura = unit->GetAura(spellId);
    return aura ? aura->GetStackAmount() : 0;
}

bool IsTauntAction(std::string const& name)
{
    return name == "taunt" || name == "growl" || name == "hand of reckoning" || name == "dark command" ||
           name == "righteous defense" || name == "mocking blow" || name == "challenging shout" ||
           name == "challenging roar" || name == "death grip" || name == "taunt spell";
}

namespace
{
// a class's taunts, first the single-target one, then the backups (Hand of Reckoning / Righteous Defense,
// Taunt / Mocking Blow / Challenging Shout, Growl / Challenging Roar, Dark Command / Death Grip); `onAlly`: cast on
// the member the mob attacks (Righteous Defense), not on the mob
struct TauntDef
{
    char const* name;
    bool onAlly;
};

std::vector<TauntDef> const& ClassTaunts(Player* bot)
{
    static std::vector<TauntDef> const warrior = {
        {"taunt", false}, {"mocking blow", false}, {"challenging shout", false}};
    static std::vector<TauntDef> const paladin = {{"hand of reckoning", false}, {"righteous defense", true}};
    static std::vector<TauntDef> const knight = {{"dark command", false}, {"death grip", false}};
    static std::vector<TauntDef> const druid = {{"growl", false}, {"challenging roar", false}};
    static std::vector<TauntDef> const none;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            return warrior;
        case CLASS_PALADIN:
            return paladin;
        case CLASS_DEATH_KNIGHT:
            return knight;
        case CLASS_DRUID:
            return druid;
        default:
            return none;
    }
}

uint32 SpellIdOf(PlayerbotAI* botAI, char const* name)
{
    return botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get();
}
}  // namespace

uint32 TauntReadyInMs(Player* bot, PlayerbotAI* botAI)
{
    uint32 best = UINT32_MAX;
    for (TauntDef const& def : ClassTaunts(bot))
        if (uint32 const spellId = SpellIdOf(botAI, def.name))
            best = std::min(best, bot->HasSpellCooldown(spellId) ? bot->GetSpellCooldownDelay(spellId) : 0u);
    return best;
}

bool CastClassTaunt(Player* bot, PlayerbotAI* botAI, Unit* target)
{
    std::string why;
    return CastClassTaunt(bot, botAI, target, why);
}

bool CastClassTaunt(Player* bot, PlayerbotAI* botAI, Unit* target, std::string& why)
{
    if (!target || !target->IsAlive())
    {
        why = "no living target";
        return false;
    }
    if (bot->GetDistance(target) > TAUNT_RANGE)
    {
        why = Acore::StringFormat("{:.1f} yd away", bot->GetDistance(target));
        return false;
    }
    std::vector<TauntDef> const& taunts = ClassTaunts(bot);
    if (taunts.empty())
    {
        why = "no class taunt";
        return false;
    }
    why.clear();
    for (TauntDef const& def : taunts)
    {
        Unit* on = def.onAlly ? target->GetVictim() : target;
        if (!on || (def.onAlly && (!on->IsPlayer() || on == bot)))
            continue;
        uint32 const spellId = SpellIdOf(botAI, def.name);
        if (!spellId)
            continue;
        if (bot->HasSpellCooldown(spellId))
        {
            why += (why.empty() ? "" : ", ") + Acore::StringFormat("{} on cooldown {:.1f} s", def.name,
                                                                   bot->GetSpellCooldownDelay(spellId) / 1000.0f);
            continue;
        }
        if (!botAI->CanCastSpell(def.name, on))
        {
            why += (why.empty() ? "" : ", ") + std::string(def.name) + " cannot be cast now";
            continue;
        }
        if (botAI->CastSpell(def.name, on))
        {
            why = def.name;
            return true;
        }
        why += (why.empty() ? "" : ", ") + std::string(def.name) + " cast failed";
    }
    if (why.empty())
        why = "no taunt known";
    return false;
}

bool Note(Player* bot, char const* key, std::string const& text)
{
    // [world_boss] one line per bot and kind every NOTE_MS: the fight's decisions are readable in Playerbots.log
    // without a line per tick; [kzk1] shared under a lock (a map's bots tick on any MapUpdater worker: a thread_local
    // throttle let up to four lines through per 5 s)
    constexpr uint32 NOTE_MS = 5 * IN_MILLISECONDS;
    static std::mutex lock;
    static std::unordered_map<std::string, uint32> last;
    std::lock_guard<std::mutex> guard(lock);
    uint32 const now = getMSTime();
    uint32& at = last[bot->GetName() + "|" + key];
    if (at && getMSTimeDiff(at, now) < NOTE_MS)
        return false;
    at = now ? now : 1;
    LOG_INFO("playerbots", "[world_boss] {}: {}", bot->GetName(), text);
    return true;
}

// --- helpers for the tank rules -----------------------------------------------------------------------------------
namespace
{
constexpr uint32 SWAP_LEAD_MS = 1500;  // a taunt's global cooldown: the swap lands before the holder's next stack
// the game's aggro switch (3.3.5): a unit takes a mob off its victim at 110 % of the victim's threat in the mob's
// melee range, 130 % beyond it
constexpr float OVERTAKE_MELEE = 1.1f;
constexpr float OVERTAKE_RANGED = 1.3f;

// [b348] the holder's stack rate, measured from its own aura (ms between two stacks); [kzk1] under gStateLock
struct StackRate
{
    uint8 stacks = 0;
    uint32 atMs = 0;
    uint32 msPerStack = 0;
};
std::unordered_map<ObjectGuid, StackRate> gStackRates;

uint32 StackRateMs(Unit* holder, uint8 stacks, uint32& sinceLast)
{
    std::lock_guard<std::mutex> guard(gStateLock);
    StackRate& r = gStackRates[holder->GetGUID()];
    uint32 const now = getMSTime();
    if (stacks != r.stacks)
    {
        if (stacks == r.stacks + 1 && r.atMs)
            r.msPerStack = getMSTimeDiff(r.atMs, now);
        r.stacks = stacks;
        r.atMs = now ? now : 1;
    }
    sinceLast = r.atMs ? getMSTimeDiff(r.atMs, now) : 0;
    return r.msPerStack;
}

int32 AuraMsLeft(Unit* unit, uint32 spellId)
{
    Aura const* aura = unit && spellId ? unit->GetAura(spellId) : nullptr;
    return aura ? aura->GetDuration() : 0;
}

char const* SpellNameOf(uint32 spellId)
{
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    return info ? info->SpellName[LOCALE_enUS] : "the swap debuff";
}

// [kzk1] the tank carries the swap debuff (any stack) and another living tank holds the boss: it stays off him until
// the debuff has expired. wb-kazzak 20261108-183632: off only from 4 stacks, it took him back at 1-3 and the stacks
// never dropped (Mortal Wound, recast on the victim every 6-8 s, refreshed them up to 10)
bool StackedTankOff(Player* bot, Unit* boss)
{
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!t || !t->swapAura || !Stacks(bot, t->swapAura))
        return false;
    Unit* holder = boss->GetVictim();
    Player* player = holder ? holder->ToPlayer() : nullptr;
    return player && player != bot && player->IsAlive() && PlayerbotAI::IsTank(player);
}

// the bot is a tank and may take the boss: [kzk1] its swap debuff has expired (0 stacks), not only below the swap count
bool TankMayTake(Player* bot, Unit* boss, BossTactic const* t)
{
    return PlayerbotAI::IsTank(bot) && (!t->swapAura || !Stacks(bot, t->swapAura)) &&
           bot->GetDistance(boss) <= TAUNT_RANGE;
}

// [kzk1] how far (centre to centre) the stacked tank stands from the boss: out of his melee reach (Cleave and
// Thunderclap hit the units in it; Unit::GetMeleeRange) plus the core's leeway for moving units
float OffDistance(Unit* boss, Unit* bot) { return boss->GetMeleeRange(bot) + LEEWAY_BONUS_RANGE; }

// [kzk1] an area or chain spell reaches the boss from where it lands: an area (its effect radius) around the caster or
// the target within reach of the boss's body, or a chain whose jump (Spell::SearchChainTargets: the spell's
// JumpDistance, else 7.5 yd ranged / 10 yd) reaches him; the spells it triggers too (one level)
bool AreaReaches(Player* bot, SpellInfo const* info, WorldObject* center, Unit* boss, uint8 depth = 0)
{
    if (!info || depth > 1)
        return false;
    float const toBoss = std::max(0.0f, center->GetExactDist2d(boss) - boss->GetCombatReach());
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.IsEffect())
            continue;
        if ((effect.IsTargetingArea() || effect.IsEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA) ||
             effect.IsAreaAuraEffect()) &&
            toBoss <= effect.CalcRadius(bot))
            return true;
        if (effect.ChainTarget > 1)
        {
            float const jump = info->JumpDistance > 0.0f                     ? info->JumpDistance
                               : info->DmgClass == SPELL_DAMAGE_CLASS_RANGED ? 7.5f
                                                                             : 10.0f;
            if (toBoss <= jump)
                return true;
        }
        if (effect.TriggerSpell &&
            AreaReaches(bot, sSpellMgr->GetSpellInfo(effect.TriggerSpell), center, boss, depth + 1))
            return true;
    }
    return false;
}

bool SpellHitsBoss(Player* bot, PlayerbotAI* botAI, Action* action, Unit* boss, std::string& spell)
{
    CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
    if (!cast)
        return false;
    spell = cast->getSpell();
    uint32 const id = SpellIdOf(botAI, spell.c_str());
    SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
    if (!info)
        return false;
    Unit* target = action->GetTarget();
    WorldObject* center = info->GetMaxRange(false) <= 0.0f || !target ? static_cast<WorldObject*>(bot) : target;
    return AreaReaches(bot, info, center, boss);
}

// [kzk1] each world boss's victim as last seen (one line per change, by the first bot that sees it) and each tank's
// swap stacks as last seen (one line when they drop to 0); under gStateLock
std::unordered_map<ObjectGuid, ObjectGuid> gBossVictims;
std::unordered_map<ObjectGuid, uint8> gOwnStacks;

void NoteFightChanges(Player* bot, Unit* boss)
{
    BossTactic const* t = Tactic(boss->GetEntry());
    if (!t || !t->swapAura)
        return;
    Unit* victim = boss->GetVictim();
    ObjectGuid const now = victim ? victim->GetGUID() : ObjectGuid::Empty;
    ObjectGuid was;
    bool changed = false;
    uint8 ownWas = 0;
    uint8 const own = Stacks(bot, t->swapAura);
    bool const tank = PlayerbotAI::IsTank(bot);
    {
        std::lock_guard<std::mutex> guard(gStateLock);
        ObjectGuid& seen = gBossVictims[boss->GetGUID()];
        if (seen != now)
        {
            was = seen;
            seen = now;
            changed = true;
        }
        if (tank)
        {
            uint8& ownSeen = gOwnStacks[bot->GetGUID()];
            ownWas = ownSeen;
            ownSeen = own;
        }
    }
    if (changed && !was.IsEmpty() && victim)
    {
        Unit* old = ObjectAccessor::GetUnit(*boss, was);
        LOG_INFO("playerbots", "[world_boss] {} victim: {} ({} stacks, threat {:.0f}) -> {} ({} stacks, threat {:.0f})",
                 boss->GetName(), old ? old->GetName() : std::string("?"), Stacks(old, t->swapAura),
                 old ? boss->GetThreatMgr().GetThreat(old) : 0.0f, victim->GetName(), Stacks(victim, t->swapAura),
                 boss->GetThreatMgr().GetThreat(victim));
    }
    if (tank && ownWas && !own)
        LOG_INFO("playerbots", "[world_boss] {}: {} expired (was {} stacks), may take {} back; holder {}",
                 bot->GetName(), SpellNameOf(t->swapAura), ownWas, boss->GetName(),
                 victim ? victim->GetName() : std::string("nobody"));
}

// [kzk1] a mana tank's mana picture every 5 s (wb-kazzak 20261108-183632: Rtpalaa under 10 % mana in 51 of 94
// samples while Divine Plea stayed up 7-638 s): mana, Divine Plea, Mark of Kazzak (mana drain), what it tanks
void NoteTankMana(Player* bot, PlayerbotAI* botAI, Unit* boss)
{
    constexpr uint32 SPELL_DIVINE_PLEA = 54428;
    constexpr uint32 SPELL_MARK_OF_KAZZAK = 21056;
    if (!PlayerbotAI::IsTank(bot) || !bot->GetMaxPower(POWER_MANA))
        return;
    uint32 const plea = SpellIdOf(botAI, "divine plea");
    std::string pleaText = "not known";
    if (int32 const left = AuraMsLeft(bot, SPELL_DIVINE_PLEA))
        pleaText = Acore::StringFormat("up {:.0f} s", left / 1000.0f);
    else if (plea)
        pleaText = bot->HasSpellCooldown(plea)
                       ? Acore::StringFormat("down, cooldown {:.0f} s", bot->GetSpellCooldownDelay(plea) / 1000.0f)
                       : std::string("down, ready");
    Unit* target = bot->GetVictim();
    Note(bot, "mana",
         Acore::StringFormat("mana {:.0f} %: Divine Plea {}, Mark of Kazzak {}, {} attackers, on {}{}",
                             bot->GetPowerPct(POWER_MANA), pleaText, bot->HasAura(SPELL_MARK_OF_KAZZAK) ? "on" : "off",
                             bot->getAttackers().size(), target ? target->GetName() : std::string("nothing"),
                             StackedTankOff(bot, boss) ? " (stacked: off the boss)" : ""));
}
}  // namespace

bool AreaMayTake(Player* bot, PlayerbotAI* botAI, std::vector<Creature*> const& mobs, std::string const& spell)
{
    if (mobs.empty() || !bot->GetMap() || bot->GetMap()->Instanceable() || PlayerbotAI::IsTank(bot) ||
        PlayerbotAI::IsHeal(bot))
        return false;
    Unit* boss = FightBoss(bot, botAI);
    if (!boss)
        return false;
    for (Creature* mob : mobs)
        if (!mob || !mob->IsAlive() || !IsBossAdd(mob))
            return false;
    Group* group = bot->GetGroup();
    if (!group)
        return false;
    Snapshot const& s = Picture(bot, botAI);

    auto const verdict = [&](bool ok, std::string const& why)
    {
        Note(bot, ok ? "aoe" : "aoe_held",
             Acore::StringFormat("aoe {} on {} adds near {} ({} first): {}", spell, mobs.size(), boss->GetName(),
                                 mobs.front()->GetName(), why));
        return ok;
    };
    // one taunt for one loose mob: none while an add already hits a non-tank
    if (!s.looseAdd.IsEmpty())
        return verdict(false, "an add is loose already, the tanks' taunts are for it");

    // the tank that takes them back: a living tank of the group, not the boss's holder, in taunt reach and sight of
    // the first mob; its ready taunts by how many mobs one cast takes
    Player* taker = nullptr;
    std::vector<uint8> capacity;  // per ready taunt: mobs it takes back in one global cooldown
    Player* healer = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap())
            continue;
        if (!healer && PlayerbotAI::IsHeal(member) && member->IsWithinCombatRange(bot, SPELL_REACH) &&
            member->IsWithinLOSInMap(bot))
            healer = member;
        if (taker || !PlayerbotAI::IsTank(member) || member == boss->GetVictim() ||
            !member->IsWithinCombatRange(mobs.front(), TAUNT_RANGE) || !member->IsWithinLOSInMap(mobs.front()))
            continue;
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;
        std::vector<uint8> ready;
        for (TauntDef const& def : ClassTaunts(member))
            if (uint32 const id = SpellIdOf(memberAI, def.name); id && !member->HasSpellCooldown(id))
            {
                std::string const name = def.name;
                ready.push_back(name == "challenging shout" || name == "challenging roar" ? uint8(255)
                                : name == "righteous defense"                             ? uint8(3)
                                                                                           : uint8(1));
            }
        if (ready.empty())
            continue;
        taker = member;
        capacity = ready;
    }
    if (!taker)
        return verdict(false, "no other tank with a ready taunt in reach");
    if (!healer)
        return verdict(false, "no healer reaches it");

    // the mobs back in global-cooldown steps, the biggest taunt first, the hardest hitter first
    std::sort(capacity.begin(), capacity.end(), std::greater<uint8>());
    std::vector<std::pair<float, Creature*>> hits;
    for (Creature* mob : mobs)
    {
        float part = 0.0f;
        hits.emplace_back(Leader::Fight::AttackerDps(mob, bot, part), mob);
    }
    std::sort(hits.begin(), hits.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    float const gcdSec = SWAP_LEAD_MS / 1000.0f;
    float damage = 0.0f;
    float longest = 0.0f;
    size_t next = 0;
    for (size_t step = 0; step < capacity.size() && next < hits.size(); ++step)
    {
        float const back = gcdSec * float(step + 1);
        for (uint8 n = 0; n < capacity[step] && next < hits.size(); ++n, ++next)
            damage += hits[next].first * back;
        longest = back;
    }
    if (next < hits.size())
        return verdict(false, Acore::StringFormat("{} ready taunts of {} take {} of {} back", capacity.size(),
                                                  taker->GetName(), next, hits.size()));
    for (Unit* attacker : bot->getAttackers())
        if (attacker && attacker->IsAlive() &&
            std::none_of(mobs.begin(), mobs.end(), [attacker](Creature* m) { return m == attacker; }))
        {
            float part = 0.0f;
            damage += Leader::Fight::AttackerDps(attacker, bot, part) * longest;
        }
    float const health = float(bot->GetHealth()) + float(bot->GetTotalAuraModifier(SPELL_AURA_SCHOOL_ABSORB));
    bool const survives = damage < health;
    return verdict(survives, Acore::StringFormat("{:.0f} damage until {} has them back ({:.1f} s) vs {:.0f} health, "
                                                 "healer {}: {}",
                                                 damage, taker->GetName(), longest, health, healer->GetName(),
                                                 survives ? "casts" : "held"));
}
}  // namespace WorldBoss

using namespace WorldBoss;

// --- triggers -----------------------------------------------------------------------------------------------------
bool WorldBossTankSwapTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot))
        return false;
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!t || !t->swapAura)
        return false;
    Unit* victim = boss->GetVictim();
    Player* holder = victim ? victim->ToPlayer() : nullptr;
    if (!holder || holder == bot || !PlayerbotAI::IsTank(holder))
        return false;
    if (!TankMayTake(bot, boss, t))
    {
        // [kzk1] the swap waits for the own debuff to expire (the holder takes the stacks meanwhile)
        if (uint8 const own = Stacks(bot, t->swapAura); own && Stacks(holder, t->swapAura) >= t->swapStacks)
            Note(bot, "swap_wait",
                 Acore::StringFormat("{} on {} has {} stacks, own {} stacks ({:.1f} s left): no taunt until they "
                                     "expire",
                                     boss->GetName(), holder->GetName(), Stacks(holder, t->swapAura), own,
                                     AuraMsLeft(bot, t->swapAura) / 1000.0f));
        return false;
    }
    // [b348] no retry while every taunt of the class is on cooldown (271 "on cooldown" lines in wb-kazzak b348)
    if (TauntReadyInMs(bot, botAI) > 0)
        return false;
    // the swap planned from the holder's measured stack rate: taunt when the holder would take the threshold stack
    // before the taunt lands (a global cooldown's lead), not after it has taken it
    uint8 const stacks = Stacks(holder, t->swapAura);
    uint32 sinceLast = 0;
    uint32 const msPerStack = StackRateMs(holder, stacks, sinceLast);
    uint8 predicted = stacks;
    if (msPerStack && sinceLast + SWAP_LEAD_MS >= msPerStack)
        ++predicted;
    return predicted >= t->swapStacks;
}

bool WorldBossStackedTankTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot))
        return false;
    Unit* boss = FightBoss(bot, botAI);
    if (!boss || !StackedTankOff(bot, boss))
        return false;
    // [kzk1] still hitting him, or still in his melee reach (Cleave / Thunderclap hit it: its Holy Shield, Retribution
    // Aura and Blessing of Sanctuary answer with threat on him)
    return bot->GetVictim() == boss || AI_VALUE(Unit*, "current target") == boss ||
           bot->GetExactDist2d(boss) < OffDistance(boss, bot);
}

bool WorldBossThreatResetTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot))
        return false;
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!t || boss->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;
    Unit* victim = boss->GetVictim();
    Player* holder = victim ? victim->ToPlayer() : nullptr;
    if (holder && PlayerbotAI::IsTank(holder))
        return false;
    // a non-tank hit is worse than a stacked tank: any tank in reach takes him; [b348] no retry on cooldown
    return bot->GetDistance(boss) <= TAUNT_RANGE && TauntReadyInMs(bot, botAI) == 0;
}

bool WorldBossLooseAddTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot))
        return false;
    Unit* boss = FightBoss(bot, botAI);
    if (!boss || boss->GetVictim() == bot)  // the boss's tank stays on the boss
        return false;
    return !Picture(bot, botAI).looseAdd.IsEmpty();
}

bool WorldBossHealerAddCastTrigger::IsActive()
{
    if (PlayerbotAI::IsHeal(bot) || !FightBoss(bot, botAI))
        return false;
    return !Picture(bot, botAI).healCaster.IsEmpty();
}

bool WorldBossPriorityAddTrigger::IsActive()
{
    if (!PlayerbotAI::IsDps(bot) || PlayerbotAI::IsTank(bot))
        return false;
    Unit* boss = FightBoss(bot, botAI);
    Unit* add = boss ? PriorityAdd(bot, botAI, boss) : nullptr;
    if (!add || bot->GetVictim() == add)
        return false;
    // Spirit Shades are the ranged damage dealers' (AoE, slows); the healer adds everybody's
    return add->GetEntry() != NPC_SPIRIT_SHADE || PlayerbotAI::IsRanged(bot);
}

bool WorldBossDispelTrigger::IsActive()
{
    if (!FightBoss(bot, botAI))
        return false;
    return !Picture(bot, botAI).dispelTarget.IsEmpty();
}

bool WorldBossCarrierTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;
    if (!bot->HasAura(SPELL_LIVING_BOMB) && !bot->HasAura(SPELL_MARK_OF_KAZZAK_TBC) &&
        !bot->HasAura(SPELL_VOLATILE_INFECTION))
        return false;
    return FightBoss(bot, botAI) && Picture(bot, botAI).crowded;
}

bool WorldBossSpreadTrigger::IsActive()
{
    if (PlayerbotAI::IsMelee(bot) || PlayerbotAI::IsTank(bot))
        return false;
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    return t && t->spreadRanged && Picture(bot, botAI).crowded;
}

bool WorldBossMoveOutTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;
    if (bot->HasAura(SPELL_MANASTORM))
        return true;
    return FightBoss(bot, botAI) && !Picture(bot, botAI).dreamFog.IsEmpty();
}

bool WorldBossPositionTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot))
        return false;
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!t || (!t->sides && !t->frontShare) || boss->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;
    float const dist = bot->GetDistance(boss);
    float const off = AngleOffFront(boss, bot);
    if (t->sides)
        return dist <= 30.0f && (off < SIDE_CONE || off > float(M_PI) - SIDE_CONE);
    // Kruul: melee into the front cone (Meteor Slash is split by everyone in it); ranged and healers out of it
    if (PlayerbotAI::IsMelee(bot))
        return dist <= 15.0f && off > FRONT_CONE;
    return dist <= 30.0f && off < FRONT_CONE;
}

bool WorldBossRaidCooldownTrigger::IsActive()
{
    if (!PlayerbotAI::IsHeal(bot) || (bot->getClass() != CLASS_PRIEST && bot->getClass() != CLASS_DRUID))
        return false;
    if (!FightBoss(bot, botAI))
        return false;
    Snapshot const& s = Picture(bot, botAI);
    return s.raidPhase && s.hurt >= RAID_HURT_COUNT;
}

// --- actions ------------------------------------------------------------------------------------------------------
bool WorldBossTauntAction::Execute(Event /*event*/)
{
    Unit* boss = FightBoss(bot, botAI);
    if (!boss)
        return false;
    BossTactic const* t = Tactic(boss->GetEntry());
    Unit* holder = boss->GetVictim();
    std::string const off =
        holder ? Acore::StringFormat("{} ({} stacks)", holder->GetName(), t ? Stacks(holder, t->swapAura) : 0)
               : std::string("nobody");
    std::string why;
    if (CastClassTaunt(bot, botAI, boss, why))
    {
        Note(bot, "taunt",
             Acore::StringFormat("tank swap / threat reset: {} on {} off {}, own stacks {}", why, boss->GetName(), off,
                                 t ? Stacks(bot, t->swapAura) : 0));
        return true;
    }
    Note(bot, "taunt_failed", Acore::StringFormat("taunt of {} off {} failed: {}", boss->GetName(), off, why));
    return bot->GetVictim() != boss && Attack(boss);
}

bool WorldBossTauntAddAction::Execute(Event /*event*/)
{
    Unit* add = SnapshotUnit(botAI, Picture(bot, botAI).looseAdd);
    if (!add)
        return false;
    Unit* victim = add->GetVictim();
    std::string why;
    if (CastClassTaunt(bot, botAI, add, why))
    {
        Note(bot, "taunt_add",
             Acore::StringFormat("loose add {} off {}: {}", add->GetName(),
                                 victim ? victim->GetName() : std::string("nobody"), why));
        return true;
    }
    return bot->GetVictim() != add && Attack(add);
}

bool WorldBossStepOffAction::Execute(Event /*event*/)
{
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!boss || !t || !StackedTankOff(bot, boss))
        return false;
    bool done = false;
    if (bot->GetVictim() == boss)
    {
        bot->AttackStop();
        done = true;
    }
    if (AI_VALUE(Unit*, "current target") == boss)
    {
        botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
        done = true;
    }
    // [kzk1] out of his melee reach: on the line from him through the tank, one own body clear of the reach
    float const off = OffDistance(boss, bot);
    float const dist = bot->GetExactDist2d(boss);
    bool moved = false;
    if (dist < off)
    {
        float const angle = dist > 0.1f ? boss->GetAngle(bot) : boss->GetOrientation() + float(M_PI);
        float const to = off + bot->GetCombatReach();
        float const x = boss->GetPositionX() + to * std::cos(angle);
        float const y = boss->GetPositionY() + to * std::sin(angle);
        float z = boss->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        moved = MoveTo(boss->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
        done = done || moved;
    }
    if (!done)
        return false;
    // the proof: the holder keeps him while the own threat stays under the game's switch share of the holder's
    Unit* holder = boss->GetVictim();
    float const own = boss->GetThreatMgr().GetThreat(bot);
    float const held = holder ? boss->GetThreatMgr().GetThreat(holder) : 0.0f;
    float const share = boss->IsWithinMeleeRange(bot) ? OVERTAKE_MELEE : OVERTAKE_RANGED;
    Note(bot, "step_off",
         Acore::StringFormat("off {} with {} stacks ({:.1f} s left), {} holds him: own threat {:.0f} vs holder {:.0f} "
                             "(x{:.2f}; he turns back at x{:.2f}), {:.1f} yd -> {:.1f} yd out of his reach{}",
                             boss->GetName(), Stacks(bot, t->swapAura), AuraMsLeft(bot, t->swapAura) / 1000.0f,
                             holder ? holder->GetName() : std::string("nobody"), own, held,
                             held > 0.0f ? own / held : 0.0f, share, dist, off, moved ? ", walks out" : ""));
    return true;
}

bool WorldBossInterruptAddAction::Execute(Event /*event*/)
{
    Unit* caster = SnapshotUnit(botAI, Picture(bot, botAI).healCaster);
    if (!caster || !CastingAddHeal(caster) || !Leader::Fight::LeaderInterruptReaches(bot, caster))
        return false;
    uint32 const spell = Leader::Fight::LeaderReadyInterrupt(bot);
    if (!spell || !botAI->CastSpell(spell, caster))
        return false;
    Note(bot, "interrupt", Acore::StringFormat("interrupts the Heal of {} (spell {})", caster->GetName(), spell));
    return true;
}

bool WorldBossAttackPriorityAddAction::Execute(Event /*event*/)
{
    Unit* add = PriorityAdd(bot, botAI, FightBoss(bot, botAI));
    if (!add || !Attack(add))
        return false;
    Note(bot, "priority", Acore::StringFormat("switches to {} ({:.0f} % hp)", add->GetName(), add->GetHealthPct()));
    return true;
}

bool WorldBossDispelAction::Execute(Event /*event*/)
{
    Snapshot const& s = Picture(bot, botAI);
    Unit* member = SnapshotUnit(botAI, s.dispelTarget);
    if (!member || s.dispelSpell.empty() || !botAI->CastSpell(s.dispelSpell, member))
        return false;
    Note(bot, "dispel", Acore::StringFormat("{} on {}", s.dispelSpell, member->GetName()));
    return true;
}

bool WorldBossCarrierMoveAction::Execute(Event /*event*/)
{
    if (!MoveFromGroup(CARRIER_DISTANCE))
        return false;
    Note(bot, "carrier", "carries a bomb / mark / infection: away from the group");
    return true;
}

bool WorldBossSpreadAction::Execute(Event /*event*/)
{
    if (!MoveFromGroup(SPREAD_DISTANCE))
        return false;
    Note(bot, "spread", "spreads out (chain spell)");
    return true;
}

bool WorldBossMoveOutAction::Execute(Event /*event*/)
{
    if (Aura* storm = bot->GetAura(SPELL_MANASTORM))
        if (WorldObject* owner = storm->GetOwner())
            if (owner != bot && FleePosition(owner->GetPosition(), MANASTORM_RADIUS))
            {
                Note(bot, "move_out", "out of Manastorm");
                return true;
            }
    if (Unit* fog = SnapshotUnit(botAI, Picture(bot, botAI).dreamFog))
        if (FleePosition(fog->GetPosition(), DREAM_FOG_RADIUS))
        {
            Note(bot, "move_out", "away from a Dream Fog");
            return true;
        }
    return false;
}

bool WorldBossPositionAction::Execute(Event /*event*/)
{
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!t)
        return false;
    float const facing = boss->GetOrientation();
    float const reach = boss->GetCombatReach();
    float angle;
    float dist;
    if (t->sides || !PlayerbotAI::IsMelee(bot))
    {
        // the nearer side of the boss; melee at their reach, ranged at their own distance (at most 25 yd)
        float const left = facing + float(M_PI) / 2.0f;
        float const right = facing - float(M_PI) / 2.0f;
        auto const sideDist = [&](float a)
        {
            float const x = boss->GetPositionX() + 10.0f * std::cos(a);
            float const y = boss->GetPositionY() + 10.0f * std::sin(a);
            return bot->GetDistance2d(x, y);
        };
        angle = sideDist(left) <= sideDist(right) ? left : right;
        dist = PlayerbotAI::IsMelee(bot) ? reach + 2.0f : std::min(25.0f, std::max(12.0f, bot->GetDistance(boss)));
    }
    else
    {
        // Kruul, melee: into the front cone, a little off the centre line
        angle = facing + (bot->GetGUID().GetCounter() % 2 ? 0.3f : -0.3f);
        dist = reach + 2.0f;
    }
    float const x = boss->GetPositionX() + dist * std::cos(angle);
    float const y = boss->GetPositionY() + dist * std::sin(angle);
    if (!MoveTo(boss->GetMapId(), x, y, boss->GetPositionZ(), false, false, false, false,
                MovementPriority::MOVEMENT_COMBAT))
        return false;
    Note(bot, "position", t->sides ? "to the boss's side" : "to the front cone / out of it");
    return true;
}

bool WorldBossRaidCooldownAction::Execute(Event /*event*/)
{
    char const* name = bot->getClass() == CLASS_PRIEST  ? "divine hymn"
                       : bot->getClass() == CLASS_DRUID ? "tranquility"
                                                        : nullptr;
    if (!name || !botAI->CastSpell(name, bot))
        return false;
    Note(bot, "raid_cooldown",
         Acore::StringFormat("{}: raid-damage phase, {} members hurt", name, Picture(bot, botAI).hurt));
    return true;
}

// --- multiplier ---------------------------------------------------------------------------------------------------
namespace
{
// [kzk1] a walk is no threat, an attack is: AttackAction derives from MovementAction, so the old "movement passes"
// test let "tank assist" / "dps assist" / "attack" through - wb-kazzak 20261108-183632: Rtpalaa back on Kazzak after
// every step off ("off Lord Kazzak with 10 stacks" dozens of times), the damage dealers' "dps assist" (50) back to
// Kazzak against the committed Dreadlord (534 intents) and "world boss attack priority add" (61) back again (795)
bool OnlyWalks(Action* action) { return dynamic_cast<MovementAction*>(action) && !dynamic_cast<AttackAction*>(action); }
}  // namespace

float WorldBossStackedTankMultiplier::GetValue(Action* action)
{
    if (!action || !PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return 1.0f;
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    uint8 const stacks = t && t->swapAura ? Stacks(bot, t->swapAura) : 0;
    if (!stacks)
        return 1.0f;
    bool const off = StackedTankOff(bot, boss);
    std::string const name = action->getName();
    // a taunt on an add stays allowed; this tank only leaves the boss to the other tank (Righteous Defense goes on
    // the boss's victim)
    if (IsTauntAction(name))
    {
        if (stacks < t->swapStacks && !off)
            return 1.0f;
        Unit* target = action->GetTarget();
        return target && (target == boss || target == boss->GetVictim()) ? 0.0f : 1.0f;
    }
    // [b348] the other tank took the boss: no threat on him until the stacks drop - wb-kazzak b348, Rtpalaa kept
    // hitting Kazzak after each swap, took him back with its threat when the taunt ended and reached 10 stacks
    // [kzk1] ... until they have expired (any stack), and no area / chain spell that reaches him (Consecration,
    // Hammer of the Righteous, Avenger's Shield, Thunder Clap on the adds next to him)
    if (!off || OnlyWalks(action) || name.rfind("world boss", 0) == 0)
        return 1.0f;
    if (action->GetTarget() == boss || name == "boss judgement duty")  // the duty judges the boss
        return 0.0f;
    std::string spell;
    if (SpellHitsBoss(bot, botAI, action, boss, spell))
    {
        Note(bot, "step_off_area",
             Acore::StringFormat("holds {}: it reaches {} ({} stacks on it, {} holds him)", spell, boss->GetName(),
                                 stacks, boss->GetVictim() ? boss->GetVictim()->GetName() : std::string("nobody")));
        return 0.0f;
    }
    return 1.0f;
}

float WorldBossCommittedAddMultiplier::GetValue(Action* action)
{
    if (!action || !bot->IsInCombat() || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return 1.0f;
    Unit* add = CommittedAdd(bot, botAI);
    if (!add || OnlyWalks(action) || action->getName().rfind("world boss", 0) == 0)
        return 1.0f;
    // [b348] committed to a priority add: nothing on the boss until it is dead or out of reach (no flip-flop);
    // [kzk1] the target choosers too ("dps assist" is an AttackAction); an area spell on the add still hits him
    Unit* boss = FightBoss(bot, botAI);
    return boss && action->GetTarget() == boss ? 0.0f : 1.0f;
}
