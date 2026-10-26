/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoRTriggers.h"
#include "AiObjectContext.h"
#include "DungeonInteract.h"
#include "DungeonTactics.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Pet.h"
#include "Playerbots.h"

#include <algorithm>
#include <list>
#include <cmath>

using namespace HoRIds;

namespace HoRHelpers
{
InstanceScript* GetInstance(Player* bot)
{
    if (bot->GetMapId() != HOR_MAP_ID)
        return nullptr;

    return bot->GetInstanceScript();
}

Creature* GetInstanceCreature(Player* bot, uint32 dataId)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return nullptr;

    ObjectGuid const guid = instance->GetGuidData(dataId);
    return guid ? bot->GetMap()->GetCreature(guid) : nullptr;
}

bool IsLichKingFightActive(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    return instance && instance->GetBossState(DATA_LICH_KING) == IN_PROGRESS;
}

bool IsWaveEventActive(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    return instance && instance->GetPersistentData(PERSISTENT_DATA_INTRO) &&
           instance->GetBossState(DATA_MARWYN) != DONE;
}

bool IsFrontDoorOpen(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return false;

    ObjectGuid const guid = instance->GetGuidData(GO_FRONT_DOOR);
    GameObject* door = guid ? bot->GetMap()->GetGameObject(guid) : nullptr;
    return door && door->GetGoState() == GO_STATE_ACTIVE;
}

bool IsShadowBossFight(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return false;

    for (uint32 const dataId : {DATA_FALRIC, DATA_MARWYN})
    {
        if (instance->GetBossState(dataId) == IN_PROGRESS)
            return true;

        // Waves 5 and 10 call the boss (DoAction 1): he talks, walks in and engages 8 s later.
        uint32 const callWave = dataId == DATA_FALRIC ? 5 : 10;
        if (instance->GetData(DATA_WAVE_NUMBER) != callWave)
            continue;

        Creature* boss = GetInstanceCreature(bot, dataId);
        if (boss && boss->IsAlive() && boss->IsVisible())
            return true;
    }
    return false;
}

bool IsShadowAuraPaladin(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->getClass() != CLASS_PALADIN || !bot->IsAlive())
        return false;

    uint32 const spellId =
        botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "shadow resistance aura")->Get();
    if (!spellId || !bot->HasSpell(spellId))
        return false;

    // One aura of a kind per group: the first living paladin in the instance (by GUID) keeps it.
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->getClass() == CLASS_PALADIN &&
                member->GetMap() == bot->GetMap() && member->GetGUID() < bot->GetGUID())
                return false;
        }
    }
    return true;
}

WaveHold GetWaveHold(Player* bot)
{
    WaveHold hold;
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return hold;

    bool const inCombat = bot->IsInCombat();

    // Wave number 0 after the intro: the event wiped and resumes only once everyone is alive within 40 yd.
    if (!instance->GetData(DATA_WAVE_NUMBER))
    {
        hold.spot = WAVE_GATHER_SPOT;
        hold.spread = 2.0f;
        hold.range = inCombat ? 0.0f : WAVE_STACK_RANGE;
        return hold;
    }

    bool const backRow = PlayerbotAI::IsHeal(bot) || (PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsTank(bot));
    hold.spot = backRow ? WAVE_CORNER_SPOT : WAVE_TANK_SPOT;

    if (!inCombat)
        hold.range = WAVE_SETTLE_RANGE;
    // Falric and Marwyn walk to the group themselves; their fights need room (Well of Corruption), only the leash.
    else if (instance->GetBossState(DATA_FALRIC) == IN_PROGRESS || instance->GetBossState(DATA_MARWYN) == IN_PROGRESS)
        hold.range = 0.0f;
    else
        hold.range = backRow ? WAVE_CORNER_HOLD_RANGE : WAVE_TANK_HOLD_RANGE;

    return hold;
}

bool IsOutsideWaveHold(Player* bot, Unit* unit)
{
    if (!unit || !IsWaveEventActive(bot))
        return false;

    WaveHold const hold = GetWaveHold(bot);
    return hold.range > 0.0f && unit->GetExactDist2d(&hold.spot) > hold.range;
}

bool IsWaveLost(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance || !bot->IsAlive() || !instance->GetData(DATA_WAVE_NUMBER) ||
        instance->GetBossState(DATA_MARWYN) == DONE || bot->GetExactDist2d(&WAVE_CENTER) > WAVE_RESET_LEAVE_RANGE)
        return false;

    // The leave only works through the front door, and that door is shut from the intro's end to the next wave wipe
    // (StartNextWave closes it, HandleWaveWipe opens it): the walk to 78 yd ends at the door, 60 yd from the centre,
    // and never resets anything (hor-h 20261025-153805: the lone tank stood there 59 s with "hor leave wave circle"
    // failing every tick, "[closed door] ... Doodad_IceCrown_Door_04 201976 ... stay", until wave 6 killed him).
    // Nobody can be brought back either: the core refuses an out-of-combat resurrection while the waves run
    // (Spell::CheckCast SPELL_FAILED_TARGET_CANNOT_BE_RESURRECTED under IsEncounterInProgress = wave number != 0),
    // only a combat resurrection (Rebirth) works. With the door shut the survivors fight on.
    if (!IsFrontDoorOpen(bot))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    // The core resets the waves only when every player is dead or one is out of range; a released member cannot
    // come back in while they run (TRANSFER_ABORT_ZONE_IN_COMBAT). With a member down and the waves no longer
    // winnable - two or fewer alive inside, or no living healer - every living member walks out so they reset
    // (hor-h 2026-10-25: two survivors held their spots for 5 minutes, the released shaman could not come back in).
    uint32 aliveInside = 0;
    uint32 down = 0;
    bool healerAlive = false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;

        bool const inside = member->IsAlive() && member->GetMap() == bot->GetMap() &&
                            member->GetExactDist2d(&WAVE_CENTER) <= WAVE_RESET_LEAVE_RANGE;
        if (!inside)
        {
            if (!member->IsAlive() || member->GetMap() != bot->GetMap())
                ++down;
            continue;
        }

        ++aliveInside;
        healerAlive |= PlayerbotAI::IsHeal(member);
    }

    return down > 0 && (aliveInside <= WAVE_LOST_MAX_ALIVE || !healerAlive);
}

float EscapeBehindOffset(Player* bot, Creature* lichKing)
{
    return (bot->GetPositionX() - lichKing->GetPositionX()) + (bot->GetPositionY() - lichKing->GetPositionY());
}

Creature* GetWaitingEscapeLeader(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance || !instance->GetPersistentData(PERSISTENT_DATA_LK_INTRO))
        return nullptr;

    EncounterState const state = instance->GetBossState(DATA_LICH_KING);
    if (state == IN_PROGRESS || state == DONE)
        return nullptr;

    Creature* leader = GetInstanceCreature(bot, NPC_SYLVANAS_PART2);
    if (!leader || !leader->IsAlive() || !leader->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return nullptr;

    return leader;
}

bool IsPriorityListActive(Player* bot, PriorityList list)
{
    if (!bot->IsInCombat() || PlayerbotAI::IsHeal(bot))
        return false;

    // The tank holds every summon (hor escape tank position / loose mob) instead of chasing the kill order.
    if (PlayerbotAI::IsTank(bot))
        return false;

    if (list == PriorityList::Escape)
        return IsLichKingFightActive(bot);

    InstanceScript* instance = GetInstance(bot);
    return instance && instance->GetData(DATA_WAVE_NUMBER) && instance->GetBossState(DATA_MARWYN) != DONE;
}

std::vector<Unit*> GetPriorityRank(PlayerbotAI* botAI, PriorityList list)
{
    using Ranks = std::vector<std::vector<uint32>>;
    // The healer and the ranged spirits first, the melee ones are held by the tank (DUNGEON_TACTICS.md, HoR waves):
    // Ghostly Priest (Dark Mending heals the wave back, Shadow Word: Pain, Cower in Fear) > Phantom Mage (Fireball,
    // Frostbolt, Flamestrike, Chains of Ice; every 40 s a Phantom Hallucination copy) > Tortured Rifleman (Shoot,
    // Cursed Arrow, Ice Shot, Frost Trap from range) > Shadowy Mercenary (Shadow Step + Deadly Poison / Kidney Shot
    // on the back row), then the Spectral Footmen and the Hallucinations by normal targeting.
    static Ranks const waveRanks = {{NPC_WAVE_PRIEST, NPC_WAVE_PRIEST_H},
                                    {NPC_WAVE_MAGE, NPC_WAVE_MAGE_H},
                                    {NPC_WAVE_RIFLEMAN, NPC_WAVE_RIFLEMAN_H},
                                    {NPC_WAVE_MERCENARY, NPC_WAVE_MERCENARY_H}};
    // Risen Witch Doctors (Shadow Bolt Volley 70145 30 yd, Curse of Doom 70144) die first, then the Lumbering
    // Abominations (Cleave + Vomit Spray, the killer of both escape wipes), then the Raging Ghouls (melee + Leap)
    // (halls_of_reflection.cpp:1963-2093).
    static Ranks const escapeRanks = {{NPC_RISEN_WITCH_DOCTOR, NPC_RISEN_WITCH_DOCTOR_H},
                                      {NPC_LUMBERING_ABOMINATION, NPC_LUMBERING_ABOMINATION_H},
                                      {NPC_RAGING_GHOUL, NPC_RAGING_GHOUL_H}};

    Player* bot = botAI->GetBot();
    // Corner tactic: a spirit still out in the room is left alone until it comes round the corner; ranged may shoot
    // one already in sight and range from where they stand, nobody walks out to it.
    // [b33_hor] Escape: only a summon this bot can actually hit counts for the rank - one at the leader (where
    // HoREscapeMeetAtLeaderMultiplier lets it reach), one already in melee reach, or for ranged one in range and
    // sight. Otherwise a rank of unreachable Witch Doctors hid the reachable ones (AppendTargetExclusions) and pinned
    // the melee on a target they may not walk to (hor-n 20261026-000434 5G, no ranged: melee targeted a Witch Doctor
    // 6-100 yd away in ~100 of ~360 combat samples, 45 of them while a ghoul or Abomination was beating on them; wall
    // 4 wiped). Nothing reachable in a rank falls through to the next one.
    Creature* const escapeLeader =
        list == PriorityList::Escape ? GetInstanceCreature(bot, NPC_SYLVANAS_PART2) : nullptr;
    auto const canEngage = [bot, list, escapeLeader](Unit* unit)
    {
        if (list == PriorityList::Escape)
        {
            if (!escapeLeader || !escapeLeader->IsAlive() ||
                unit->GetExactDist2d(escapeLeader) <= ESCAPE_MEET_RANGE || bot->IsWithinMeleeRange(unit))
                return true;

            return PlayerbotAI::IsRanged(bot) && bot->GetExactDist2d(unit) <= sPlayerbotAIConfig.SpellDistance &&
                   bot->IsWithinLOSInMap(unit);
        }

        if (!IsOutsideWaveHold(bot, unit))
            return true;

        return PlayerbotAI::IsRanged(bot) && bot->GetExactDist2d(unit) <= sPlayerbotAIConfig.SpellDistance &&
               bot->IsWithinLOSInMap(unit);
    };

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    std::vector<Unit*> rankUnits;
    for (std::vector<uint32> const& rank : list == PriorityList::Wave ? waveRanks : escapeRanks)
    {
        for (ObjectGuid const& guid : attackers)
        {
            if (std::find(rank.begin(), rank.end(), guid.GetEntry()) == rank.end())
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || !unit->IsVisible() || unit->IsImmuneToPC() || !canEngage(unit))
                continue;

            rankUnits.push_back(unit);
        }

        if (!rankUnits.empty())
            break;
    }
    return rankUnits;
}

bool IsEscapeSummon(uint32 entry)
{
    switch (entry)
    {
        case NPC_RAGING_GHOUL:
        case NPC_RAGING_GHOUL_H:
        case NPC_RISEN_WITCH_DOCTOR:
        case NPC_RISEN_WITCH_DOCTOR_H:
        case NPC_LUMBERING_ABOMINATION:
        case NPC_LUMBERING_ABOMINATION_H:
            return true;
        default:
            return false;
    }
}

bool EscapeSummonsUp(PlayerbotAI* botAI)
{
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    return std::any_of(attackers.begin(), attackers.end(),
                       [](ObjectGuid const& guid) { return IsEscapeSummon(guid.GetEntry()); });
}

bool GetEscapeTankSpot(Player* bot, Position& spot)
{
    if (!IsLichKingFightActive(bot))
        return false;

    Creature* lichKing = GetInstanceCreature(bot, NPC_LICH_KING_BOSS);
    Creature* leader = GetInstanceCreature(bot, NPC_SYLVANAS_PART2);
    if (!lichKing || !leader || !leader->IsAlive())
        return false;

    // On the leader-Lich King line, so the tank is the player nearest to the summons he raises. While the group runs it
    // trails the leader; the spot is then behind the rearmost member, not a fixed distance from the leader (hor-h
    // 20261025-163739 wall 4: the tank ran at its spot 3 yd behind the leader, the group 11-15 yd behind him, and the
    // wall's ghouls, Witch Doctors and Abomination opened on the healer: 13 aggro losses in 30 s).
    float const dx = lichKing->GetPositionX() - leader->GetPositionX();
    float const dy = lichKing->GetPositionY() - leader->GetPositionY();
    float const length = std::hypot(dx, dy);
    float const limit = std::max(0.0f, length - ESCAPE_LICH_KING_MIN_RANGE - ESCAPE_TANK_LICH_KING_MARGIN);
    float rearmost = 0.0f;
    if (length > 0.0f)
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
                    PlayerbotAI::IsTank(member) || member->GetExactDist2d(leader) > ESCAPE_GROUP_SCAN)
                    continue;

                float const along = ((member->GetPositionX() - leader->GetPositionX()) * dx +
                                     (member->GetPositionY() - leader->GetPositionY()) * dy) / length;
                rearmost = std::max(rearmost, along);
            }
    float const forward =
        std::min(limit, std::clamp(rearmost + ESCAPE_TANK_BEHIND_GROUP, ESCAPE_TANK_FORWARD / 2.0f, ESCAPE_TANK_FORWARD));
    if (length <= 0.0f || forward <= 0.0f)
    {
        spot.Relocate(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
        return true;
    }

    spot.Relocate(leader->GetPositionX() + dx / length * forward, leader->GetPositionY() + dy / length * forward,
                  leader->GetPositionZ());
    return true;
}

bool IsBehindEscapeTank(Player* bot)
{
    if (PlayerbotAI::IsTank(bot) || !IsLichKingFightActive(bot))
        return false;

    // A melee damage dealer on a summon the tank holds stands at that summon, on the tank's side: it stays.
    if (!PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsHeal(bot) && bot->GetVictim())
        return false;

    Creature* lichKing = GetInstanceCreature(bot, NPC_LICH_KING_BOSS);
    Creature* leader = GetInstanceCreature(bot, NPC_SYLVANAS_PART2);
    Group* group = bot->GetGroup();
    if (!lichKing || !leader || !leader->IsAlive() || !group)
        return false;

    float const dx = lichKing->GetPositionX() - leader->GetPositionX();
    float const dy = lichKing->GetPositionY() - leader->GetPositionY();
    float const length = std::hypot(dx, dy);
    if (length <= 0.0f)
        return false;

    auto const along = [&](Unit* unit)
    {
        return ((unit->GetPositionX() - leader->GetPositionX()) * dx +
                (unit->GetPositionY() - leader->GetPositionY()) * dy) / length;
    };

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || member->GetMap() != bot->GetMap() || !PlayerbotAI::IsTank(member) ||
            member->GetExactDist2d(leader) > ESCAPE_GROUP_SCAN)
            continue;

        // at the leader herself there is nowhere further forward to go
        return along(bot) > ESCAPE_BEHIND_TANK_MARGIN && along(bot) > along(member) - ESCAPE_BEHIND_TANK_MARGIN;
    }
    return false;
}

Unit* GetLooseEscapeMob(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Unit* best = nullptr;
    bool bestOnHealer = false;
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const& guid : attackers)
    {
        if (!IsEscapeSummon(guid.GetEntry()))
            continue;

        Unit* mob = botAI->GetUnit(guid);
        if (!mob || !mob->IsAlive())
            continue;

        Player* victim = mob->GetVictim() ? mob->GetVictim()->ToPlayer() : nullptr;
        if (!victim || PlayerbotAI::IsTank(victim))
            continue;

        bool const onHealer = PlayerbotAI::IsHeal(victim);
        if (!best || (onHealer && !bestOnHealer) ||
            (onHealer == bestOnHealer && bot->GetExactDist2d(mob) < bot->GetExactDist2d(best)))
        {
            best = mob;
            bestOnHealer = onHealer;
        }
    }
    return best;
}

Player* GetFalricDispelTarget(PlayerbotAI* botAI, std::string& spell)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = GetInstance(bot);
    if (!instance || instance->GetBossState(DATA_FALRIC) != IN_PROGRESS)
        return nullptr;

    // The friendly Magic dispels of 3.3.5: Dispel Magic and Cleanse (Cleanse Spirit takes no Magic).
    switch (bot->getClass())
    {
        case CLASS_PRIEST:
            spell = "dispel magic";
            break;
        case CLASS_PALADIN:
            spell = "cleanse";
            break;
        default:
            return nullptr;
    }

    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    // Impending Despair ends in a stun (on the healer the group goes down with it), Quivering Strike cuts the tank's
    // dodge.
    Player* best = nullptr;
    uint32 bestScore = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            bot->GetExactDist(member) > sPlayerbotAIConfig.SpellDistance)
            continue;

        uint32 score = 0;
        if (member->HasAura(SPELL_IMPENDING_DESPAIR))
            score = PlayerbotAI::IsHeal(member) ? FALRIC_DISPEL_DESPAIR_HEALER : FALRIC_DISPEL_DESPAIR;
        else if (member->HasAura(SPELL_QUIVERING_STRIKE) || member->HasAura(SPELL_QUIVERING_STRIKE_H))
            score = FALRIC_DISPEL_QUIVERING_STRIKE;

        if (score > bestScore)
        {
            best = member;
            bestScore = score;
        }
    }
    return best;
}
Creature* GetFrostswornGeneral(Player* bot)
{
    Creature* general = GetInstanceCreature(bot, NPC_FROSTSWORN_GENERAL);
    return general && general->IsAlive() && general->IsVisible() ? general : nullptr;
}

Unit* GetAttackableReflection(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!GetInstance(bot))
        return nullptr;

    // The copies come with UNIT_FLAG_IMMUNE_TO_PC from creature_template (37068/37721 unit_flags 33024/256) and the
    // script never clears it: then IsValidAttackTarget is false and nothing is done here (see PATCH_hor_gd.md 3.2).
    std::list<Creature*> reflections;
    bot->GetCreatureListWithEntryInGrid(reflections, NPC_SPIRITUAL_REFLECTION, REFLECTION_SEARCH);
    Unit* best = nullptr;
    for (Creature* reflection : reflections)
    {
        if (!reflection->IsAlive() || !reflection->IsVisible() || !reflection->IsInCombat() ||
            !bot->IsValidAttackTarget(reflection))
            continue;

        if (!best || bot->GetExactDist2d(reflection) < bot->GetExactDist2d(best))
            best = reflection;
    }
    return best;
}

bool GetChaseForwardAnchor(Player* bot, Position& anchor, Unit*& chaser)
{
    if (!IsLichKingFightActive(bot))
        return false;

    Creature* leader = GetInstanceCreature(bot, NPC_SYLVANAS_PART2);
    Creature* lichKing = GetInstanceCreature(bot, NPC_LICH_KING_BOSS);
    if (!leader || !leader->IsAlive() || !lichKing)
        return false;

    anchor.Relocate(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
    chaser = lichKing;
    return true;
}

bool IsGroupGathered(Player* bot, float range)
{
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap())
            continue;

        if (bot->GetExactDist(member) > range)
            return false;
    }
    return true;
}
}  // namespace HoRHelpers

bool HoRStartIntroTrigger::IsActive()
{
    if (bot->IsInCombat())
        return false;

    InstanceScript* instance = HoRHelpers::GetInstance(bot);
    return instance && !instance->GetPersistentData(PERSISTENT_DATA_INTRO) &&
           instance->GetBossState(DATA_FALRIC) != DONE;
}

bool HoRWavePositionTrigger::IsActive()
{
    if (!HoRHelpers::IsWaveEventActive(bot) || HoRHelpers::IsWaveLost(bot))
        return false;

    HoRHelpers::WaveHold const hold = HoRHelpers::GetWaveHold(bot);
    if (hold.range > 0.0f && bot->GetExactDist2d(&hold.spot) > hold.range)
        return true;

    return bot->IsInCombat() && bot->GetExactDist2d(&WAVE_CENTER) > WAVE_LEASH_RANGE;
}

bool HoRWaveLostTrigger::IsActive() { return HoRHelpers::IsWaveLost(bot); }

bool HoRWavePriorityTrigger::IsActive()
{
    return HoRHelpers::IsPriorityListActive(bot, HoRHelpers::PriorityList::Wave);
}

bool HoRLichKingIntroTrigger::IsActive()
{
    if (bot->IsInCombat())
        return false;

    InstanceScript* instance = HoRHelpers::GetInstance(bot);
    if (!instance || !instance->GetPersistentData(PERSISTENT_DATA_FROSTSWORN_GENERAL) ||
        instance->GetPersistentData(PERSISTENT_DATA_LK_INTRO))
        return false;

    // The general's persistent flag is stored on engage as well; the intro waits for his death.
    Creature* general = HoRHelpers::GetInstanceCreature(bot, NPC_FROSTSWORN_GENERAL);
    if (general && general->IsAlive())
        return false;

    return DungeonInteract::IsInteractor(botAI);
}

bool HoREscapeGatherTrigger::IsActive()
{
    if (bot->IsInCombat())
        return false;

    Creature* leader = HoRHelpers::GetWaitingEscapeLeader(bot);
    return leader && bot->GetExactDist2d(leader) > ESCAPE_GATHER_RANGE;
}

bool HoRStartEscapeTrigger::IsActive()
{
    if (bot->IsInCombat() || !HoRHelpers::GetWaitingEscapeLeader(bot))
        return false;

    return DungeonInteract::IsInteractor(botAI);
}

bool HoREscapePositionTrigger::IsActive()
{
    if (!HoRHelpers::IsLichKingFightActive(bot))
        return false;

    Creature* lichKing = HoRHelpers::GetInstanceCreature(bot, NPC_LICH_KING_BOSS);
    Creature* leader = HoRHelpers::GetInstanceCreature(bot, NPC_SYLVANAS_PART2);
    if (!lichKing || !leader || !leader->IsAlive())
        return false;

    if (HoRHelpers::EscapeBehindOffset(bot, lichKing) > ESCAPE_BEHIND_LIMIT ||
        bot->GetExactDist2d(leader) > ESCAPE_LEADER_MAX_RANGE || HoRHelpers::IsBehindEscapeTank(bot))
        return true;

    // Too close to him only matters while the leader's side is still clear of him; once he reaches the wall the
    // spot next to the leader is the only place to be.
    return bot->GetExactDist2d(lichKing) < ESCAPE_LICH_KING_MIN_RANGE &&
           leader->GetExactDist2d(lichKing) > ESCAPE_LICH_KING_MIN_RANGE + ESCAPE_LEADER_SETTLE_MARGIN;
}

bool HoREscapePriorityTrigger::IsActive()
{
    return HoRHelpers::IsPriorityListActive(bot, HoRHelpers::PriorityList::Escape);
}

bool HoRLichKingTargetedTrigger::IsActive()
{
    if (bot->GetMapId() != HOR_MAP_ID)
        return false;

    auto const isLichKing = [](Unit* unit)
    { return unit && (unit->GetEntry() == NPC_LICH_KING_BOSS || unit->GetEntry() == NPC_LICH_KING_EVENT); };

    if (isLichKing(bot->GetVictim()) || isLichKing(AI_VALUE(Unit*, "current target")))
        return true;

    Pet* pet = bot->GetPet();
    if (pet && isLichKing(pet->GetVictim()))
        return true;

    // Guardians too (Feral Spirit wolves, Mirror Images, Treants): they take the owner's old target.
    for (Unit* controlled : bot->m_Controlled)
        if (controlled && controlled != pet && controlled->IsAlive() && isLichKing(controlled->GetVictim()))
            return true;
    return false;
}

bool HoREscapeTankPositionTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot))
        return false;

    Position spot;
    if (!HoRHelpers::GetEscapeTankSpot(bot, spot) || bot->GetExactDist2d(&spot) <= ESCAPE_TANK_SPOT_HOLD)
        return false;

    return !HoRHelpers::GetLooseEscapeMob(botAI);
}

bool HoREscapeLooseMobTrigger::IsActive()
{
    return PlayerbotAI::IsTank(bot) && HoRHelpers::IsLichKingFightActive(bot) && HoRHelpers::GetLooseEscapeMob(botAI);
}

bool HoREscapeHealerHoldTrigger::IsActive()
{
    if (!PlayerbotAI::IsHeal(bot) || !HoRHelpers::IsLichKingFightActive(bot))
        return false;

    Creature* leader = HoRHelpers::GetInstanceCreature(bot, NPC_SYLVANAS_PART2);
    if (!leader || !leader->IsAlive())
        return false;

    // Behind the tank the next wall's summons open on the healer: back in front of him at the leader at once, summons
    // up or not (they rise 7.5-11 s after a wall falls, the group is still walking then).
    bool const behindTank = HoRHelpers::IsBehindEscapeTank(bot);
    if (!behindTank && (bot->GetExactDist2d(leader) <= ESCAPE_HEALER_LEADER_RANGE || !HoRHelpers::EscapeSummonsUp(botAI)))
        return false;

    // An Abomination turned on the leader's side: "dungeon avoid frontal cleave" keeps the healer out of it instead.
    return !DungeonTactics::SpotInFrontalCleave(bot, botAI, leader->GetPositionX(), leader->GetPositionY(),
                                                ESCAPE_HEALER_CLEAVE_DIST_MARGIN, ESCAPE_HEALER_CLEAVE_ANGLE_MARGIN);
}

bool HoRShadowAuraTrigger::IsActive()
{
    if (!HoRHelpers::IsShadowAuraPaladin(botAI) || !HoRHelpers::IsShadowBossFight(bot) ||
        botAI->HasAura("shadow resistance aura", bot))
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", "shadow resistance aura");
    return spellId && bot->HasSpell(spellId);
}

bool HoRGeneralLeashTrigger::IsActive()
{
    if (!PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return false;

    Creature* general = HoRHelpers::GetFrostswornGeneral(bot);
    if (!general || !general->IsInCombat() || general->GetVictim() != bot)
        return false;

    Position const home = general->GetHomePosition();
    return bot->GetExactDist2d(&home) > GENERAL_TANK_HOLD;
}

bool HoRReflectionTrigger::IsActive()
{
    if (!bot->IsInCombat() || PlayerbotAI::IsHeal(bot) || PlayerbotAI::IsTank(bot))
        return false;

    Unit* reflection = HoRHelpers::GetAttackableReflection(botAI);
    return reflection && AI_VALUE(Unit*, "current target") != reflection;
}

bool HoRFalricDispelTrigger::IsActive()
{
    std::string spell;
    return HoRHelpers::GetFalricDispelTarget(botAI, spell) != nullptr;
}
