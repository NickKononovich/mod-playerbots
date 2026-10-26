/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ClassicStrategy.h"
#include "ClassicShared.h"
#include "ClassicMultipliers.h"
#include "DungeonHazard.h"
#include "DungeonTactics.h"
#include "TbcHazard.h"
#include "Playerbots.h"
#include "Timer.h"

void ClassicDungeonScarletMonasteryStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Arcanist Doan: below 50 % Arcane Bubble, then a 5 s Detonation over 30 yd; Arcane Explosion all fight
    triggers.push_back(new TriggerNode("doan detonation", {
        NextAction("doan escape detonation", ACTION_EMERGENCY + 6) }));
    triggers.push_back(new TriggerNode("doan arcane explosion", {
        NextAction("doan keep range", ACTION_RAID + 4) }));
    // Cathedral: Commander Mograine's engage sends his whole chapel; nobody steps into his reach while it stands
    triggers.push_back(new TriggerNode("mograine too close", {
        NextAction("mograine keep away", ACTION_EMERGENCY + 5) }));
}

void ClassicDungeonScarletMonasteryStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DoanMultiplier(botAI));
    multipliers.push_back(new MograineHoldMultiplier(botAI));
}

void ClassicDungeonGnomereganStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
    // Caverndeep Reaver Arcing Smash: out of the front cone and out of its body
    triggers.push_back(new TriggerNode("dungeon frontal cleave", {
        NextAction("dungeon avoid frontal cleave", ACTION_EMERGENCY + 3) }));
    // Blastmaster Emi Shortfuse's escort, which ends with Grubbis
    triggers.push_back(new TriggerNode("dungeon event step", {
        NextAction("dungeon event step", ACTION_MOVE + 3) }));
    // [b35_gnomer] the Mobile Alert System summons two Leprous adds every 10 s while it lives (DungeonTactics.cpp)
    triggers.push_back(new TriggerNode("dungeon kill first", {
        NextAction("dungeon kill first", ACTION_RAID + 2) }));
}

void ClassicDungeonGnomereganStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new DungeonFrontalCleaveMultiplier(botAI));
}

// [w24c-gnomer-mine-tank] A Dark Iron Land Mine (dropped by the Dark Iron Agents, in combat, standing still) is no
// tank or pet target: it detonates on whoever comes within 6 yd, and a mine "on" the healer ranked first in the
// tank's choice (gnomer-h 20261025-115805, 2380 s: "reach spell -> Dark Iron Land Mine 62.8 yd", the tank ran 60 yd
// into the Leprous packs and the group wiped at 2412 s). Ranged damage dealers still shoot the mines.
void ClassicDungeonGnomereganStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    // [b35_gnomer] the damage dealers' "dps target" is the kill-first mob while there is one, so "dps assist" keeps
    // the target that "dungeon kill first" chose instead of switching it back every tick
    if (type == TargetValueExclusionType::DpsTarget)
        DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);

    if (type != TargetValueExclusionType::TankTarget && type != TargetValueExclusionType::PetTarget)
        return;

    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit ||
            (unit->GetEntry() != NPC_DARK_IRON_LAND_MINE && unit->GetEntry() != NPC_DARK_IRON_LAND_MINE_HEROIC))
            continue;

        uint32 const now = getMSTime();
        if (exclusions.insert(guid).second && (!_mineLogMs || getMSTimeDiff(_mineLogMs, now) > MINE_LOG_MS))
        {
            _mineLogMs = now ? now : 1;
            LOG_INFO("playerbots", "[w24c-gnomer-mine-tank] {}: leaves {} ({:.1f} yd) out of the {} target",
                     botAI->GetBot()->GetName(), unit->GetName(), botAI->GetBot()->GetExactDist(unit),
                     type == TargetValueExclusionType::TankTarget ? "tank" : "pet");
        }
    }
}

void ClassicDungeonBlackrockSpireStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // The Rookery: stay off the eggs, kill the Hatchers first (the whelps are AoE'd and picked up by the generic
    // dps aoe and tank logic)
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
    triggers.push_back(new TriggerNode("dungeon kill first", {
        NextAction("dungeon kill first", ACTION_RAID + 2) }));
    // UBRS: the Father Flame starts the Solakar Flamewreath waves
    triggers.push_back(new TriggerNode("dungeon event step", {
        NextAction("dungeon event step", ACTION_MOVE + 3) }));
    // [b31_ubrs_door] UBRS stadium: out of the drake pen while the wave portcullis is up (it locks in for good)
    triggers.push_back(new TriggerNode("brs drake pen", {
        NextAction("brs leave drake pen", ACTION_EMERGENCY + 3) }));
}

void ClassicDungeonBlackrockSpireStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "brs hatcher focus", {NPC_ROOKERY_HATCHER,
                                                                                 NPC_ROOKERY_HATCHER_HEROIC}));
}

// The damage dealers' "dps target" is the kill-first mob while there is one, so "dps assist" keeps the target that
// "dungeon kill first" chose instead of switching it back every tick.
void ClassicDungeonBlackrockSpireStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget)
        DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);

    // [b31_ubrs_door] A bot on the stadium side picks nothing that still stands inside the drake pen (east of the
    // doorway): the waves come out through the gate, a chase or a Charge in is what locked 10B-43 in (ClassicShared.h).
    using namespace BlackrockSpire;
    if (type != TargetValueExclusionType::TankTarget && type != TargetValueExclusionType::DpsTarget &&
        type != TargetValueExclusionType::PetTarget)
        return;
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != MAP_BLACKROCK_SPIRE || bot->GetPositionX() >= PEN_DOOR_X ||
        bot->GetPositionX() < PEN_DOOR_X - 80.0f || !InPenBand(bot->GetPositionY(), bot->GetPositionZ()))
        return;
    for (char const* list : {"attackers", "possible targets"})
        for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>(list)->Get())
            if (Unit* unit = botAI->GetUnit(guid))
                if (unit->GetPositionX() >= PEN_DEEP_X && InDrakePen(unit->GetPositionX(), unit->GetPositionY(),
                                                                      unit->GetPositionZ()))
                    exclusions.insert(guid);
}

void ClassicDungeonScholomanceStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Dark Mending, the Necromancers' Shadow Bolt Volley and Malicia's heals: whoever can kick does
    triggers.push_back(new TriggerNode("dungeon priority interrupt", {
        NextAction("dungeon priority interrupt", ACTION_INTERRUPT + 5) }));
    // Acolytes and Spectral Researchers (instant Mana Burn, Wail of Souls) first, when held or on a healer
    triggers.push_back(new TriggerNode("dungeon kill first", {
        NextAction("dungeon kill first", ACTION_RAID + 2) }));
    // below the critical heals, above the rest: a step out of the Acolytes' 20 yd keeps the healer's mana
    triggers.push_back(new TriggerNode("scholo mana burn", {
        NextAction("scholo avoid mana burn", ACTION_HIGH + 8) }));
    // after a fight on the hall floor, back up the ramp before the next pack's reach or a hall patrol finds the group
    triggers.push_back(new TriggerNode("scholo hall rally", {
        NextAction("scholo hall rally", ACTION_MOVE + 9) }));
    // [b32_brs] after Dawn's Gambit the tank opens on Vectus / Marduk (neutral with npc flags: no aggressive target)
    triggers.push_back(new TriggerNode("scholo viewing room boss", {
        NextAction("scholo attack viewing room boss", ACTION_HIGH + 1) }));
}

void ClassicDungeonScholomanceStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TbcKeepFocusMultiplier(botAI, "scholo caster focus", {NPC_SCHOLOMANCE_ACOLYTE,
                                                                                   NPC_SPECTRAL_RESEARCHER}));
}

// The damage dealers' "dps target" is the kill-first mob while there is one, so "dps assist" keeps the target that
// "dungeon kill first" chose instead of switching it back every tick.
void ClassicDungeonScholomanceStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::DpsTarget)
        DungeonTactics::AppendKillFirstExclusions(botAI, exclusions);
}

void ClassicDungeonMaraudonStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void ClassicDungeonMaraudonStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}

void ClassicDungeonSunkenTempleStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("dungeon event step", {
        NextAction("dungeon event step", ACTION_MOVE + 3) }));
    // [b30_st_spirit] out of the Atal'ai Deathwalker's Spirit's reach until it fades (15 s, 2 yd/s, immune)
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
    // [b32_st] a damage dealer kills each idle Nightmare Suppressor of the Shade of Hakkar event as it comes
    triggers.push_back(new TriggerNode("st nightmare suppressor", {
        NextAction("st attack suppressor", ACTION_HIGH + 1) }));
}

void ClassicDungeonSunkenTempleStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}

// [b30_st_spirit] The spirit is immune to all damage and no taunt holds it (st-n 20261025-184229-st-n-5F-11: the tank
// taunted it 16 times in one pull, it went back to the rogue, the druid and the warlock each time and killed them):
// no bot, tank or pet targets it; everybody walks away from it instead.
void ClassicDungeonSunkenTempleStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType /*type*/)
{
    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && (unit->GetEntry() == NPC_DEATHWALKER_SPIRIT || unit->GetEntry() == NPC_DEATHWALKER_SPIRIT_HEROIC))
            exclusions.insert(guid);
    }
}

void ClassicDungeonZulFarrakStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("dungeon event step", {
        NextAction("dungeon event step", ACTION_MOVE + 3) }));
}

void TbcDungeonOldHillsbradStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("dungeon event step", {
        NextAction("dungeon event step", ACTION_MOVE + 3) }));
    // [boss_puddles] Captain Skarloc's Consecration 38385 (10 yd, 20 s) at his feet
    // (DungeonHazard.cpp, BOSS_PUDDLES.md): out of it the short way, and the hold keeps everyone from walking
    // back in while the tank drags the boss out (BossKiteRooms).
    triggers.push_back(new TriggerNode("dungeon hazard", {
        NextAction("avoid dungeon hazard", ACTION_EMERGENCY + 4) }));
}

void TbcDungeonOldHillsbradStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonHazardHoldMultiplier(botAI));
}
