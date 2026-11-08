/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldBossTactics.h"

#include <cmath>
#include <unordered_map>

#include "DynamicObject.h"
#include "Group.h"
#include "LeaderFightChecks.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
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

// bots of one map run on one map thread: thread-local, keyed by the bot
thread_local std::unordered_map<ObjectGuid, Snapshot> tSnapshots;

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
thread_local std::unordered_map<ObjectGuid, ObjectGuid> tCommitted;

bool StillPriority(Player* bot, Unit* add)
{
    return add && add->IsAlive() && add->IsInCombat() && add->IsInWorld() && add->GetMap() == bot->GetMap() &&
           !add->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && bot->GetDistance(add) <= SPELL_REACH + 10.0f &&
           (IsHealerAdd(add) || add->GetEntry() == NPC_SPIRIT_SHADE);
}
}  // namespace

Unit* PriorityAdd(Player* bot, PlayerbotAI* botAI, Unit* /*boss*/)
{
    auto const committed = tCommitted.find(bot->GetGUID());
    if (committed != tCommitted.end())
    {
        if (Unit* add = botAI->GetUnit(committed->second); StillPriority(bot, add))
            return add;
        tCommitted.erase(committed);
    }
    Unit* add = SnapshotUnit(botAI, Picture(bot, botAI).priorityAdd);
    if (add)
        tCommitted[bot->GetGUID()] = add->GetGUID();
    return add;
}

Unit* CommittedAdd(Player* bot, PlayerbotAI* botAI)
{
    auto const committed = tCommitted.find(bot->GetGUID());
    if (committed == tCommitted.end())
        return nullptr;
    Unit* add = botAI->GetUnit(committed->second);
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
    // without a line per tick (bots of one map tick on one map thread: thread-local)
    constexpr uint32 NOTE_MS = 5 * IN_MILLISECONDS;
    thread_local std::unordered_map<std::string, uint32> last;
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

// [b348] the holder's stack rate, measured from its own aura (ms between two stacks); thread-local per holder
struct StackRate
{
    uint8 stacks = 0;
    uint32 atMs = 0;
    uint32 msPerStack = 0;
};
thread_local std::unordered_map<ObjectGuid, StackRate> tStackRates;

uint32 StackRateMs(Unit* holder, uint8 stacks, uint32& sinceLast)
{
    StackRate& r = tStackRates[holder->GetGUID()];
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

// the tank carries the swap stacks and another living tank holds the boss
bool StackedTankOff(Player* bot, Unit* boss)
{
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!t || !t->swapAura || Stacks(bot, t->swapAura) < t->swapStacks)
        return false;
    Unit* holder = boss->GetVictim();
    Player* player = holder ? holder->ToPlayer() : nullptr;
    return player && player != bot && player->IsAlive() && PlayerbotAI::IsTank(player);
}

// the bot is a tank and may take the boss: it does not carry the swap stacks
bool TankMayTake(Player* bot, Unit* boss, BossTactic const* t)
{
    return PlayerbotAI::IsTank(bot) && (!t->swapAura || Stacks(bot, t->swapAura) < t->swapStacks) &&
           bot->GetDistance(boss) <= TAUNT_RANGE;
}
}  // namespace
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
        return false;
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
    return boss && bot->GetVictim() == boss && StackedTankOff(bot, boss);
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
    if (!boss || !t || bot->GetVictim() != boss)
        return false;
    bot->AttackStop();
    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
    Unit* holder = boss->GetVictim();
    Note(bot, "step_off",
         Acore::StringFormat("off {} with {} stacks, {} holds him: no threat until the stacks drop", boss->GetName(),
                             Stacks(bot, t->swapAura), holder ? holder->GetName() : std::string("nobody")));
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
float WorldBossStackedTankMultiplier::GetValue(Action* action)
{
    if (!action || !PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return 1.0f;
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!t || !t->swapAura || Stacks(bot, t->swapAura) < t->swapStacks)
        return 1.0f;
    std::string const name = action->getName();
    // a taunt on an add stays allowed; this tank only leaves the boss to the other tank
    if (IsTauntAction(name))
        return action->GetTarget() == boss ? 0.0f : 1.0f;
    // [b348] the other tank took the boss: no threat on him until the stacks drop - wb-kazzak b348, Rtpalaa kept
    // hitting Kazzak after each swap, took him back with its threat when the taunt ended and reached 10 stacks
    if (!StackedTankOff(bot, boss) || dynamic_cast<MovementAction*>(action) || name.rfind("world boss", 0) == 0)
        return 1.0f;
    return action->GetTarget() == boss ? 0.0f : 1.0f;
}

float WorldBossCommittedAddMultiplier::GetValue(Action* action)
{
    if (!action || !bot->IsInCombat() || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return 1.0f;
    Unit* add = CommittedAdd(bot, botAI);
    if (!add || dynamic_cast<MovementAction*>(action) || action->getName().rfind("world boss", 0) == 0)
        return 1.0f;
    // [b348] committed to a priority add: nothing on the boss until it is dead or out of reach (no flip-flop)
    Unit* boss = FightBoss(bot, botAI);
    return boss && action->GetTarget() == boss ? 0.0f : 1.0f;
}
