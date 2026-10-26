/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TOCShared.h"

#include "AiObjectContext.h"
#include "DungeonHoldFire.h"
#include "DungeonInteract.h"
#include "Group.h"
#include "InstanceScript.h"
#include "InterruptValues.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <array>

namespace
{
// Memory of the Past entries summoned by Paletress (instance_trial_of_the_champion.cpp achievement criteria).
constexpr uint32 MEMORY_FIRST = 35028;
constexpr uint32 MEMORY_LAST = 35052;
constexpr uint32 MEMORY_HOGGER = 34942;
constexpr uint32 MEMORY_ENTRY_GAP = 35035;  // no such Memory in creature_template

constexpr float INTERRUPT_SCAN_RANGE = 30.0f;
constexpr float DISPEL_RANGE = 30.0f;  // Purge, Spellsteal, Dispel Magic; Tranquilizing Shot reaches 35

struct InterruptOption
{
    char const* spell;
    bool longCooldown;  // kept for Renew / Holy Fire / Waking Nightmare, never spent on a Holy Smite
};

constexpr std::array<InterruptOption, 9> INTERRUPTS = {{{"kick", false},
                                                        {"pummel", false},
                                                        {"shield bash", false},
                                                        {"wind shear", false},
                                                        {"mind freeze", false},
                                                        {"counterspell", true},
                                                        {"spell lock", true},
                                                        {"silencing shot", true},
                                                        {"strangulate", true}}};

constexpr std::array<uint32, 4> PALETRESS_PRIORITY_CASTS = {
    ToCIds::SPELL_PALETRESS_RENEW, ToCIds::SPELL_PALETRESS_RENEW_H, ToCIds::SPELL_PALETRESS_HOLY_FIRE,
    ToCIds::SPELL_PALETRESS_HOLY_FIRE_H};
// a 1.25 s cast every 3-4 s on a random member: worth a short-cooldown kick only
constexpr std::array<uint32, 2> PALETRESS_SMITES = {ToCIds::SPELL_PALETRESS_HOLY_SMITE,
                                                    ToCIds::SPELL_PALETRESS_HOLY_SMITE_H};
constexpr std::array<uint32, 2> PALETRESS_RENEWS = {ToCIds::SPELL_PALETRESS_RENEW, ToCIds::SPELL_PALETRESS_RENEW_H};

// The first ready interrupt of the bot that would stop caster's cast and is not another member's turn.
char const* PickInterrupt(PlayerbotAI* botAI, Unit* caster, bool shortCooldownOnly)
{
    for (InterruptOption const& option : INTERRUPTS)
    {
        if (shortCooldownOnly && option.longCooldown)
            continue;

        uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", option.spell)->Get();
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info || !InterruptCoordination::CanStopCast(caster, info) ||
            !InterruptCoordination::Allows(botAI, caster, option.spell) || !botAI->CanCastSpell(option.spell, caster))
            continue;

        return option.spell;
    }

    return nullptr;
}

char const* PickPaletressInterrupt(PlayerbotAI* botAI, Unit* paletress)
{
    if (DungeonHoldFire::IsCastingAny(paletress, PALETRESS_PRIORITY_CASTS))
        return PickInterrupt(botAI, paletress, false);

    if (DungeonHoldFire::IsCastingAny(paletress, PALETRESS_SMITES))
        return PickInterrupt(botAI, paletress, true);

    return nullptr;
}

bool HasRenew(Unit* unit)
{
    for (uint32 const spellId : PALETRESS_RENEWS)
        if (unit->HasAura(spellId))
            return true;

    return false;
}
}

namespace ToCShared
{
InstanceScript* GetInstance(Player* bot)
{
    if (!bot || bot->GetMapId() != ToCIds::MAP_TRIAL_OF_THE_CHAMPION)
        return nullptr;

    return bot->GetInstanceScript();
}

Creature* GetAnnouncer(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return nullptr;

    Creature* announcer = bot->GetMap()->GetCreature(instance->GetGuidData(ToCIds::DATA_ANNOUNCER));
    return announcer && announcer->IsAlive() ? announcer : nullptr;
}

Creature* GetArgentChampion(Player* bot, uint32 entry)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return nullptr;

    Creature* champion = bot->GetMap()->GetCreature(instance->GetGuidData(ToCIds::DATA_ARGENT_CHAMPION));
    if (!champion || !champion->IsAlive() || champion->GetEntry() != entry)
        return nullptr;

    return champion;
}

Creature* GetShieldedPaletress(Player* bot)
{
    Creature* paletress = GetArgentChampion(bot, ToCIds::NPC_PALETRESS_BOSS);
    return paletress && paletress->HasAura(ToCIds::SPELL_REFLECTIVE_SHIELD) ? paletress : nullptr;
}

bool IsMemoryOfThePast(uint32 entry)
{
    return entry == MEMORY_HOGGER || (entry >= MEMORY_FIRST && entry <= MEMORY_LAST && entry != MEMORY_ENTRY_GAP);
}

bool IsBlackKnightGhoul(uint32 entry)
{
    return entry == ToCIds::NPC_RISEN_JAEREN_GHOUL || entry == ToCIds::NPC_RISEN_ARELAS_GHOUL ||
           entry == ToCIds::NPC_RISEN_CHAMPION_GHOUL;
}

bool IsJoustMount(uint32 entry)
{
    return entry == ToCIds::NPC_JOUST_WARHORSE || entry == ToCIds::NPC_JOUST_BATTLEWORG;
}

uint32 StartGossipAction(uint32 progress)
{
    switch (progress)
    {
        case ToCIds::PROGRESS_INITIAL:
            return ToCIds::GOSSIP_ACTION_START_JOUST_SKIP_RP;
        case ToCIds::PROGRESS_CHAMPIONS_DEAD:
            return ToCIds::GOSSIP_ACTION_START_ARGENT_CHALLENGE;
        case ToCIds::PROGRESS_ARGENT_CHALLENGE_DIED:
            return ToCIds::GOSSIP_ACTION_START_BLACK_KNIGHT;
        default:
            return 0;
    }
}

bool IsPuller(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (PlayerbotAI::IsTank(bot))
        return PlayerbotAI::IsMainTank(bot);

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (DungeonInteract::IsActiveMember(bot, member) && PlayerbotAI::IsTank(member))
                return false;
        }
    }

    return DungeonInteract::IsInteractor(botAI);
}

Creature* FindIdleSoldier(Player* bot)
{
    constexpr float SOLDIER_SEARCH_RANGE = 60.0f;
    constexpr std::array<uint32, 3> SOLDIERS = {ToCIds::NPC_ARGENT_LIGHTWIELDER_TRASH, ToCIds::NPC_ARGENT_MONK_TRASH,
                                                ToCIds::NPC_ARGENT_PRIESTESS_TRASH};
    Creature* nearest = nullptr;
    for (uint32 const entry : SOLDIERS)
    {
        Creature* soldier = bot->FindNearestCreature(entry, SOLDIER_SEARCH_RANGE);
        if (!soldier || soldier->IsInCombat() || soldier->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ||
            soldier->IsImmuneToPC())
            continue;

        if (!nearest || bot->GetExactDist(soldier) < bot->GetExactDist(nearest))
            nearest = soldier;
    }

    return nearest;
}

Unit* FindInterruptTarget(PlayerbotAI* botAI)
{
    constexpr std::array<uint32, 2> MEMORY_CASTS = {ToCIds::SPELL_MEMORY_WAKING_NIGHTMARE,
                                                    ToCIds::SPELL_MEMORY_WAKING_NIGHTMARE_H};
    constexpr std::array<uint32, 2> KNIGHT_CASTS = {ToCIds::SPELL_DEATHS_RESPITE, ToCIds::SPELL_DEATHS_RESPITE_H};

    Player* bot = botAI->GetBot();
    if (Creature* paletress = GetArgentChampion(bot, ToCIds::NPC_PALETRESS_BOSS);
        paletress && bot->IsWithinDistInMap(paletress, INTERRUPT_SCAN_RANGE) &&
        PickPaletressInterrupt(botAI, paletress))
        return paletress;

    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !bot->IsWithinDistInMap(unit, INTERRUPT_SCAN_RANGE))
            continue;

        uint32 const entry = unit->GetEntry();
        bool const worthKick = (IsMemoryOfThePast(entry) && DungeonHoldFire::IsCastingAny(unit, MEMORY_CASTS)) ||
                               (entry == ToCIds::NPC_BLACK_KNIGHT_BOSS &&
                                DungeonHoldFire::IsCastingAny(unit, KNIGHT_CASTS));
        if (worthKick && PickInterrupt(botAI, unit, false))
            return unit;
    }

    return nullptr;
}

bool CastInterrupt(PlayerbotAI* botAI, Unit* caster)
{
    if (!caster)
        return false;

    char const* spell = caster->GetEntry() == ToCIds::NPC_PALETRESS_BOSS ? PickPaletressInterrupt(botAI, caster)
                                                                          : PickInterrupt(botAI, caster, false);
    if (!spell || !botAI->CastSpell(spell, caster))
        return false;

    InterruptCoordination::MarkFired(botAI, caster);
    return true;
}

Unit* FindRenewTarget(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    auto const renewed = [bot](Unit* unit)
    {
        return unit && unit->IsAlive() && HasRenew(unit) && bot->IsWithinDistInMap(unit, DISPEL_RANGE) &&
               bot->IsWithinLOSInMap(unit);
    };

    // the Memory first: its Renew stretches the phase in which Old Wounds and Shadows of the Past land
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && IsMemoryOfThePast(unit->GetEntry()) && renewed(unit))
            return unit;
    }

    Creature* paletress = GetArgentChampion(bot, ToCIds::NPC_PALETRESS_BOSS);
    return renewed(paletress) ? paletress : nullptr;
}

char const* GetRenewDispel(PlayerbotAI* botAI, Unit* target)
{
    static std::array<char const*, 4> const DISPELS = {"purge", "spellsteal", "dispel magic", "tranquilizing shot"};
    if (!target)
        return nullptr;

    for (char const* spell : DISPELS)
    {
        if (botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get() &&
            botAI->CanCastSpell(spell, target))
            return spell;
    }

    return nullptr;
}

bool HasMarkedForDeath(Unit* unit)
{
    return unit && (unit->HasAura(ToCIds::SPELL_MARKED_FOR_DEATH) || unit->HasAura(ToCIds::SPELL_MARKED_FOR_DEATH_H));
}

char const* GetMarkedDefensive(PlayerbotAI* botAI)
{
    struct DefensiveOption
    {
        uint8 classId;
        char const* spell;
        float belowHealthPct;
    };

    // Death's Bite hits the whole room every 2-4 s; the mark triples it on one member (~7 k on normal).
    static std::array<DefensiveOption, 11> const DEFENSIVES = {{{CLASS_MAGE, "ice block", 60.0f},
                                                               {CLASS_PALADIN, "divine protection", 90.0f},
                                                               {CLASS_PALADIN, "divine shield", 40.0f},
                                                               {CLASS_ROGUE, "cloak of shadows", 90.0f},
                                                               {CLASS_DEATH_KNIGHT, "anti-magic shell", 90.0f},
                                                               {CLASS_DEATH_KNIGHT, "icebound fortitude", 60.0f},
                                                               {CLASS_SHAMAN, "shamanistic rage", 90.0f},
                                                               {CLASS_DRUID, "barkskin", 90.0f},
                                                               {CLASS_PRIEST, "dispersion", 70.0f},
                                                               {CLASS_PRIEST, "pain suppression", 80.0f},
                                                               {CLASS_WARRIOR, "shield wall", 70.0f}}};

    Player* bot = botAI->GetBot();
    uint8 const classId = bot->getClass();
    for (DefensiveOption const& option : DEFENSIVES)
    {
        // one defensive at a time: a second one on top is wasted
        if (option.classId == classId && botAI->HasAura(option.spell, bot))
            return nullptr;
    }

    float const healthPct = bot->GetHealthPct();
    for (DefensiveOption const& option : DEFENSIVES)
    {
        if (option.classId != classId || healthPct >= option.belowHealthPct ||
            !botAI->GetAiObjectContext()->GetValue<uint32>("spell id", option.spell)->Get() ||
            !botAI->CanCastSpell(option.spell, bot))
            continue;

        return option.spell;
    }

    return nullptr;
}

Player* FindMarkedMemberToHeal(PlayerbotAI* botAI)
{
    constexpr float MARKED_HEAL_BELOW_PCT = 85.0f;
    constexpr float OTHER_MEMBER_LOW_PCT = 50.0f;  // the generic heals answer that member first
    constexpr float HEAL_RANGE = 40.0f;

    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Player* marked = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMap() != bot->GetMap())
            continue;

        if (!HasMarkedForDeath(member))
        {
            if (member->GetHealthPct() < OTHER_MEMBER_LOW_PCT)
                return nullptr;

            continue;
        }

        if (member->GetHealthPct() < MARKED_HEAL_BELOW_PCT && bot->IsWithinDistInMap(member, HEAL_RANGE) &&
            bot->IsWithinLOSInMap(member))
            marked = member;
    }

    return marked;
}

bool GetGhoulExplodeCenter(PlayerbotAI* botAI, Position& center)
{
    constexpr float EXPLODE_SAFETY_MARGIN = 3.0f;
    constexpr std::array<uint32, 2> EXPLODE_CASTS = {ToCIds::SPELL_GHOUL_EXPLODE, ToCIds::SPELL_GHOUL_EXPLODE_H};

    Player* bot = botAI->GetBot();
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 count = 0;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest hostile npcs")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !IsBlackKnightGhoul(unit->GetEntry()) ||
            !bot->IsWithinDist(unit, ToCIds::GHOUL_EXPLODE_RADIUS + EXPLODE_SAFETY_MARGIN) ||
            !DungeonHoldFire::IsCastingAny(unit, EXPLODE_CASTS))
            continue;

        x += unit->GetPositionX();
        y += unit->GetPositionY();
        z += unit->GetPositionZ();
        ++count;
    }

    if (!count)
        return false;

    center.Relocate(x / count, y / count, z / count);
    return true;
}
}  // namespace ToCShared
