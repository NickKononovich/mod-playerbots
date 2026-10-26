/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDUTILITYASSIGNMENT_H
#define PLAYERBOTS_RAIDUTILITYASSIGNMENT_H

#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <string>

class PlayerbotAI;
class Player;
class Trigger;
class Unit;
class UntypedValue;

// Raid utility coordinator.
//
// Every bot computes the same group-wide plan from the same inputs (members sorted by GUID, class, spec, talents)
// and keeps only its own part, so no messages are exchanged and two bots never pick the same job. The plan is
// cached ("raid utility assignment", 5 s) and read by class triggers/actions. Fast-changing jobs (battle res,
// innervate, threat redirect, hand of salvation) live in their own short-interval values.
namespace ai::raidutil
{
    enum class AssignedCurse : uint8
    {
        NONE     = 0,  // not coordinated: the warlock keeps its own curse strategy
        ELEMENTS = 1,
        TONGUES  = 2,
        WEAKNESS = 3,
        AGONY    = 4
    };

    enum TotemElement : uint8
    {
        TOTEM_ELEMENT_EARTH = 0,
        TOTEM_ELEMENT_FIRE  = 1,
        TOTEM_ELEMENT_WATER = 2,
        TOTEM_ELEMENT_AIR   = 3,
        TOTEM_ELEMENT_COUNT = 4
    };

    // Totem plan value meaning "keep your own totem, but do not duplicate the party's fire buff totem".
    inline constexpr char const* TOTEM_AVOID_FIRE_BUFF = "-";

    struct RaidUtilityAssignment
    {
        uint32 groupKey = 0;
        bool valid = false;

        bool curseCoordinated = false;
        AssignedCurse curse = AssignedCurse::NONE;

        bool armorCoordinated = false;
        bool majorArmorOwner = false;
        bool minorArmorOwner = false;

        bool lustCoordinated = false;  // at least one bot shaman with Bloodlust/Heroism is in the group
        bool lustOwner = false;        // this bot is the one shaman allowed to cast it
        bool lustAvailable = false;    // some bot shaman of the group has it off cooldown

        ObjectGuid buffTarget;         // Power Infusion / Focus Magic / Hysteria target for this bot

        // Totem strategy name per element ("" = keep, TOTEM_AVOID_FIRE_BUFF = see above).
        std::array<std::string, TOTEM_ELEMENT_COUNT> totems;
    };

    constexpr uint32 SPELL_BLOODLUST = 2825;
    constexpr uint32 SPELL_HEROISM = 32182;
    constexpr uint32 SPELL_SATED = 57724;
    constexpr uint32 SPELL_EXHAUSTION = 57723;
    constexpr uint32 SPELL_POWER_INFUSION = 10060;
    constexpr uint32 SPELL_FOCUS_MAGIC = 54646;
    constexpr uint32 SPELL_HYSTERIA = 49016;
    constexpr uint32 SPELL_INNERVATE = 29166;
    constexpr uint32 SPELL_REBIRTH_RANK_1 = 20484;
    constexpr uint32 SPELL_RAISE_ALLY = 61999;
    constexpr uint32 SPELL_TOTEM_OF_WRATH_RANK_1 = 30706;

    // Seconds into a boss fight during which offensive cooldowns wait for Bloodlust/Heroism.
    constexpr uint32 BURST_HOLD_FOR_LUST_SECONDS = 12;

    bool IsBossUnit(Unit* unit);
    bool HasLustAura(Unit* unit);
    bool HasLustDebuff(Unit* unit);
    bool IsInstanceMap(Player* bot);
    uint32 GetCombatSeconds(PlayerbotAI* botAI);

    // Boss is in combat with the group (tank has it, or it already lost health) for at least a short moment.
    bool IsBossEngaged(PlayerbotAI* botAI, Unit* target);
    // K2: Bloodlust/Heroism decision for this bot (boss pull in instances, never on instance trash).
    bool ShouldCastLust(PlayerbotAI* botAI);
    // K3: true while Bloodlust/Heroism is on the bot, or on a boss pull when no lust is coming.
    bool IsBurstWindow(PlayerbotAI* botAI);
    // K3: true in the first seconds of a boss fight while a bot shaman can still cast lust.
    bool ShouldHoldBurstForLust(PlayerbotAI* botAI);

    std::string CurseSpellName(AssignedCurse curse);
    // The assignment of this bot, refreshed when the group changes.
    RaidUtilityAssignment const& GetAssignment(PlayerbotAI* botAI);

    UntypedValue* CreateRaidUtilityAssignmentValue(PlayerbotAI* botAI);
    UntypedValue* CreateBattleResTargetValue(PlayerbotAI* botAI);
    UntypedValue* CreateInnervateTargetValue(PlayerbotAI* botAI);
    UntypedValue* CreateThreatRedirectTargetValue(PlayerbotAI* botAI);
    UntypedValue* CreateUtilityBuffTargetValue(PlayerbotAI* botAI);
    UntypedValue* CreateHandOfSalvationTargetValue(PlayerbotAI* botAI);
    UntypedValue* CreatePullImminentValue(PlayerbotAI* botAI);
    UntypedValue* CreateLowManaHealerCountValue(PlayerbotAI* botAI);

    Trigger* CreatePullImminentTrigger(PlayerbotAI* botAI);

    // Qualifier for "low mana healer count": "<party|raid>:<mana percent>".
    std::string LowManaHealerQualifier(bool partyOnly, uint32 manaPct);
}

#endif
