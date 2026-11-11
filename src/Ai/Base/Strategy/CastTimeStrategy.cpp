/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CastTimeStrategy.h"
#include "EstimatedLifetimeValue.h"
#include "GenericSpellActions.h"
#include "Group.h"  // [guild15]
#include "Pet.h"    // [deaths144]
#include "Playerbots.h"
#include "SpellInfo.h"
#include "Timer.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace
{
    // [cast-short-life] A cast lands this long after it ends, beyond its flight: the reaction of the next tick.
    constexpr float CAST_LAND_MARGIN = 0.4f;
    constexpr uint32 CAST_SHORT_LIFE_LOG_MS = 10 * IN_MILLISECONDS;  // one line per bot this often
    constexpr int32 CHANNEL_MAX_COUNTED_MS = 3000;  // a channel without a periodic effect counts this much at most
}

float CastTimeMultiplier::GetValue(Action* action)
{
    if (action == nullptr)
        return 1.0f;

    if (!action->GetTarget() || action->GetTarget() != AI_VALUE(Unit*, "current target"))
        return 1.0f;

    if (/*targetHealth < sPlayerbotAIConfig.CriticalHealth && */ dynamic_cast<CastSpellAction*>(action))
    {
        CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action);
        uint32 spellId = AI_VALUE2(uint32, "spell id", spellAction->getSpell());
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return 1.0f;

        if ((spellInfo->Targets & TARGET_FLAG_DEST_LOCATION) != 0 ||
            (spellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION) != 0)
            return 1.0f;

        uint32 castTime = spellInfo->CalcCastTime(bot);

        // [lifetime-calibrated] A channel deals its damage tick by tick (Mind Flay, Arcane Missiles, Drain Life): it
        // is worth starting when the target lives to its first tick, not to the end of the channel. Counting up to
        // 3 s of channel held the priest's and the mage's first damage on every mob the group kills in 1.5-3 s.
        if (spellInfo->IsChanneled())
        {
            int32 firstTick = 0;
            for (SpellEffectInfo const& effect : spellInfo->GetEffects())
                if (effect.ApplyAuraName != SPELL_AURA_NONE && effect.Amplitude > 0)
                {
                    int32 const amplitude = static_cast<int32>(effect.Amplitude);
                    firstTick = firstTick ? std::min(firstTick, amplitude) : amplitude;
                }

            int32 duration = spellInfo->GetDuration();
            duration = std::min(duration, CHANNEL_MAX_COUNTED_MS);
            if (firstTick > 0)
                duration = std::min(duration, firstTick);
            if (duration > 0)
                castTime += duration;
        }

        Unit* target = action->GetTarget();
        if (!target || !target->IsAlive() || !target->IsInWorld())
        {
            return 1.0f;
        }

        // [cast-short-life] Sunken Temple 2026-10-25 (st-n-5A-92): the mage opened every pull with a 2.5-3 s
        // Frostbolt on mobs that died 2-4 s into it, 32 pulls without a hit ("fire blast (not evaluated: frostbolt
        // (5.40) ran first)"). A cast is worth starting only when the target lives until it lands: its cast time and
        // its flight, against the lifetime the group's actual damage shows (ExpectedLifetime), not the model alone.
        // Else the instants of the rotation (Fire Blast, Ice Lance, Arcane Barrage, procs, the wand) go first, and
        // the caster's target choice moves on to a fresh mob (CasterFindTargetSmartStrategy).
        // [guild15] a bot that fights alone is the damage that would end the target: holding its cast for a lifetime
        // its own damage gives holds that damage too (b315 Bierya L7 held Wrath on a Gnarlpine Defender at 15 %
        // health, "the target lives 1.7 s", until two mobs killed it; Gilduck's Frostbolts the same)
        // [deaths144] ... alone on the target, not alone in a group: a member whose group fights elsewhere (a five
        // gathering for its port, a help party split up) is the only damage on its target all the same. b434 Jurdogs
        // L15 (five 7, waiting for the Dungeon Finder port) held Smite on a Redridge Thrasher at 5 % health ("the
        // target lives 2.5 s", group 0 dps by the gear model) until the Thrasher and a Poacher killed it. Alone = no
        // other member (or a member's pet) has the target as its victim
        Group* group = bot->GetGroup();
        bool alone = true;
        if (group)
            for (GroupReference* ref = group->GetFirstMember(); ref && alone; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsInWorld() || !member->IsAlive() ||
                    member->GetMapId() != bot->GetMapId())
                    continue;
                if (member->GetVictim() == target)
                    alone = false;
                else if (Pet* pet = member->GetPet())
                    alone = !(pet->IsAlive() && pet->GetVictim() == target);
            }
        if (castTime > 0 && !alone)
        {
            float flight = 0.0f;
            if (spellInfo->Speed > 0.0f)
                flight = bot->GetDistance(target) / spellInfo->Speed;

            float const lands = static_cast<float>(castTime) / IN_MILLISECONDS + flight + CAST_LAND_MARGIN;
            std::string estimate;  // [b35_mvaway] observed loss, or the group's dps by its share on the target
            float const lifetime = ExpectedLifetime(botAI, target, &estimate);
            if (lands > lifetime)
            {
                CastLand::Note(bot->GetGUID(), target->GetGUID(), lands);  // [lead451] the fact for the leader's pick
                thread_local std::unordered_map<ObjectGuid, uint32> logged;
                uint32 const now = getMSTime();
                uint32& last = logged[bot->GetGUID()];
                if (!last || getMSTimeDiff(last, now) >= CAST_SHORT_LIFE_LOG_MS)
                {
                    last = now ? now : 1;
                    // [lifetime-calibrated] Where the lifetime came from: before the target's health moves (the
                    // opener) only the group's dps, measured over its recent fights or else the gear model.
                    uint32 fights = 0;
                    float const calibrated = GroupDamage::CalibratedDps(bot, &fights);
                    std::string const source =
                        (calibrated > 0.0f
                             ? Acore::StringFormat("[lifetime-calibrated] group {:.0f} dps over {} fight(s)",
                                                   calibrated, fights)
                             : Acore::StringFormat("group {:.0f} dps by the gear model",
                                                   AI_VALUE(float, "estimated group dps"))) +
                        ", [b35_mvaway] by " + estimate;
                    LOG_INFO("playerbots",
                             "[cast-short-life] {} holds {} on {}: it lands in {:.1f} s, the target lives {:.1f} s "
                             "({:.0f} % health; {}{})",
                             bot->GetName(), spellAction->getSpell(), target->GetName(), lands, lifetime,
                             target->GetHealthPct(), source,
                             target->GetHealth() == target->GetMaxHealth() ? ", before any damage" : "");
                }
                return 0.1f;
            }
        }
    }
    // if (castTime >= 3000)
    //     return 0.0f;

    // if (castTime >= 1500)
    //     return 0.5f;

    // if (castTime >= 1000)
    //     return 0.25f;
    // // }

    return 1.0f;
}

void CastTimeStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new CastTimeMultiplier(botAI));
}
