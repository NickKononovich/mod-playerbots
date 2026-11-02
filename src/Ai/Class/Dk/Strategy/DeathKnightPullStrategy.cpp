/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DeathKnightPullStrategy.h"
#include "Creature.h"
#include "LeaderCommandState.h"  // [rot2] [rot3]
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

std::string DeathKnightPullStrategy::GetPullActionName() const
{
    Unit* target = GetTarget();
    if (!target ||
        (!botAI->HasStrategy("blood", BOT_STATE_COMBAT) && !botAI->HasStrategy("blood", BOT_STATE_NON_COMBAT)))
    {
        return PullStrategy::GetPullActionName();
    }

    if (botAI->CanCastSpell("death grip", target))
        return "death grip";

    if (!botAI->CanCastSpell("icy touch", target) &&
        botAI->CanCastSpell("dark command", target))
    {
        return "dark command";
    }

    return PullStrategy::GetPullActionName();
}

// [rot1] wowsims blood_aggro casts Army of the Dead before the pull (prepull at -6 s, a 4 s channel); the bots never
// cast it as Blood (sim 1.28/min, bots 0). PullAction runs this at the pull commit, once per pull, before the pull
// spell: a boss target only (10 min cooldown, 1 Blood + 1 Frost + 1 Unholy rune, all ready out of combat). The pull
// waits the channel (inside PullStrategy::GetMaxPullTime); the 8 ghouls join the fight with the tank.
std::string DeathKnightPullStrategy::GetPrePullBuffActionName() const
{
    if (!botAI->HasStrategy("blood", BOT_STATE_COMBAT) && !botAI->HasStrategy("blood", BOT_STATE_NON_COMBAT))
        return "";

    // [lead270] a led death knight channels it only by the leader's order, timed into the boss's preparation
    // (Leader::Prepull), never as a delay of its own at the pull commit
    Leader::MemberEvents command;
    if (LeaderCommandState::Read(botAI->GetBot(), command) != LeaderCommandState::State::NotLed)
        return "";

    Unit* target = GetTarget();
    Creature* creature = target ? target->ToCreature() : nullptr;
    if (!creature || !creature->IsAlive() || !(creature->IsDungeonBoss() || creature->isWorldBoss()))
        return "";

    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "army of the dead")->Get();
    if (!spellId || botAI->GetBot()->HasSpellCooldown(spellId))
        return "";

    LOG_INFO("playerbots", "[rot1] [army prepull] {} asks for Army of the Dead before the pull of {} {}",
             botAI->GetBot()->GetName(), creature->GetName(), creature->GetEntry());
    return "army of the dead";
}
