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

Unit* PriorityAdd(Player* bot, PlayerbotAI* botAI, Unit* /*boss*/)
{
    return SnapshotUnit(botAI, Picture(bot, botAI).priorityAdd);
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
           name == "challenging roar" || name == "taunt spell";
}

bool CastClassTaunt(Player* bot, PlayerbotAI* botAI, Unit* target)
{
    if (!target || !target->IsAlive() || bot->GetDistance(target) > TAUNT_RANGE)
        return false;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            return botAI->CastSpell("taunt", target);
        case CLASS_PALADIN:
            return botAI->CastSpell("hand of reckoning", target);
        case CLASS_DEATH_KNIGHT:
            return botAI->CastSpell("dark command", target);
        case CLASS_DRUID:
            return botAI->CastSpell("growl", target);
        default:
            return false;
    }
}

// --- helpers for the tank rules -----------------------------------------------------------------------------------
namespace
{
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
    return Stacks(holder, t->swapAura) >= t->swapStacks && TankMayTake(bot, boss, t);
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
    return TankMayTake(bot, boss, t);
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
    if (CastClassTaunt(bot, botAI, boss))
        return true;
    return Attack(boss);
}

bool WorldBossTauntAddAction::Execute(Event /*event*/)
{
    Unit* add = SnapshotUnit(botAI, Picture(bot, botAI).looseAdd);
    if (!add)
        return false;
    if (CastClassTaunt(bot, botAI, add))
        return true;
    return Attack(add);
}

bool WorldBossInterruptAddAction::Execute(Event /*event*/)
{
    Unit* caster = SnapshotUnit(botAI, Picture(bot, botAI).healCaster);
    if (!caster || !CastingAddHeal(caster) || !Leader::Fight::LeaderInterruptReaches(bot, caster))
        return false;
    uint32 const spell = Leader::Fight::LeaderReadyInterrupt(bot);
    return spell && botAI->CastSpell(spell, caster);
}

bool WorldBossAttackPriorityAddAction::Execute(Event /*event*/)
{
    Unit* add = PriorityAdd(bot, botAI, FightBoss(bot, botAI));
    return add && Attack(add);
}

bool WorldBossDispelAction::Execute(Event /*event*/)
{
    Snapshot const& s = Picture(bot, botAI);
    Unit* member = SnapshotUnit(botAI, s.dispelTarget);
    return member && !s.dispelSpell.empty() && botAI->CastSpell(s.dispelSpell, member);
}

bool WorldBossCarrierMoveAction::Execute(Event /*event*/) { return MoveFromGroup(CARRIER_DISTANCE); }

bool WorldBossSpreadAction::Execute(Event /*event*/) { return MoveFromGroup(SPREAD_DISTANCE); }

bool WorldBossMoveOutAction::Execute(Event /*event*/)
{
    if (Aura* storm = bot->GetAura(SPELL_MANASTORM))
        if (WorldObject* owner = storm->GetOwner())
            if (owner != bot)
                return FleePosition(owner->GetPosition(), MANASTORM_RADIUS);
    if (Unit* fog = SnapshotUnit(botAI, Picture(bot, botAI).dreamFog))
        return FleePosition(fog->GetPosition(), DREAM_FOG_RADIUS);
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
    return MoveTo(boss->GetMapId(), x, y, boss->GetPositionZ(), false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT);
}

bool WorldBossRaidCooldownAction::Execute(Event /*event*/)
{
    if (bot->getClass() == CLASS_PRIEST)
        return botAI->CastSpell("divine hymn", bot);
    if (bot->getClass() == CLASS_DRUID)
        return botAI->CastSpell("tranquility", bot);
    return false;
}

// --- multiplier ---------------------------------------------------------------------------------------------------
float WorldBossStackedTankMultiplier::GetValue(Action* action)
{
    if (!action || !PlayerbotAI::IsTank(bot) || !bot->IsInCombat() || !IsTauntAction(action->getName()))
        return 1.0f;
    Unit* boss = FightBoss(bot, botAI);
    BossTactic const* t = boss ? Tactic(boss->GetEntry()) : nullptr;
    if (!t || !t->swapAura || Stacks(bot, t->swapAura) < t->swapStacks)
        return 1.0f;
    // a taunt on an add stays allowed; this tank only leaves the boss to the other tank
    return action->GetTarget() == boss ? 0.0f : 1.0f;
}
