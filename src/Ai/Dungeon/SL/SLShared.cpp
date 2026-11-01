/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SLShared.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "InstanceGuid.h"  // [rot44]
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include <list>
#include <map>
#include <unordered_map>

namespace ShadowLabyrinth
{
bool IsCastingSonicBoom(Unit* murmur)
{
    if (!murmur)
        return false;

    Spell* spell = murmur->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell || !spell->GetSpellInfo())
        return false;

    uint32 const id = spell->GetSpellInfo()->Id;
    return id == SPELL_SONIC_BOOM_CAST || id == SPELL_SONIC_BOOM_CAST_HEROIC;
}

float SonicBoomDangerRadius(Player* bot)
{
    return SONIC_BOOM_RADIUS + bot->GetCombatReach();
}

float BlackheartSpread(Player* bot)
{
    return PlayerbotAI::IsHeal(bot) ? BLACKHEART_HEALER_SPREAD : BLACKHEART_SPREAD;
}

namespace
{
// A mob Blackheart's engage call (Creature::CallForHelp -> CallOfHelpCreatureInRangeDo, Creature::CanAssistTo
// without the faction check) brings: idle, aggressive, his friend, within the radius and in sight of his spot (where
// the group fights him). A stealthed one (the wandering Cabal Assassins) is left out: the tank cannot see or pull it.
bool AnswersBlackheartCall(Creature* boss, Creature* mob)
{
    return mob != boss && mob->IsAlive() && !mob->IsInCombat() && !mob->IsInEvadeMode() &&
           mob->HasReactState(REACT_AGGRESSIVE) && !mob->IsCivilian() && !mob->IsTrigger() && !mob->IsTotem() &&
           !mob->GetCharmerOrOwnerGUID() && !mob->HasStealthAura() &&
           !mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) && !mob->IsImmuneToNPC() &&
           !mob->GetCreatureTemplate()->HasFlagsExtra(CREATURE_FLAG_EXTRA_IGNORE_ALL_ASSISTANCE_CALLS) &&
           mob->IsFriendlyTo(boss) && mob->IsWithinDistInMap(boss, BLACKHEART_CALL_RADIUS) &&
           mob->IsWithinLOSInMap(boss);
}

struct BlackheartRoom
{
    uint32 checkedMs = 0;
    uint32 count = 0;        // mobs that would come, at the last check
    uint32 fewest = 0;       // the lowest count of this hold
    uint32 fewestSinceMs = 0;
    bool released = false;   // the count stalled: the pull goes ahead with them
    bool locked = false;
};
}  // namespace

bool IsBlackheartRoomLocked(Creature* boss)
{
    if (!boss || boss->GetEntry() != NPC_BLACKHEART || !boss->IsAlive() || boss->IsInCombat())
        return false;

    // per map thread; a map's bots run on its thread
    thread_local std::map<InstanceGuid, BlackheartRoom> rooms;  // [rot44] by instance: the boss's GUID is per map
    BlackheartRoom& room = rooms[InstanceKey(boss)];
    uint32 const now = getMSTime();
    if (room.checkedMs && getMSTimeDiff(room.checkedMs, now) < BLACKHEART_ROOM_RECHECK_MS)
        return room.locked;

    room.checkedMs = now;
    std::list<Creature*> creatures;
    Acore::AnyUnitInObjectRangeCheck check(boss, BLACKHEART_CALL_RADIUS);
    Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(boss, creatures, check);
    Cell::VisitObjects(boss, searcher, BLACKHEART_CALL_RADIUS);
    uint32 count = 0;
    Creature* nearest = nullptr;
    for (Creature* mob : creatures)
    {
        if (!AnswersBlackheartCall(boss, mob))
            continue;

        ++count;
        if (!nearest || boss->GetExactDist(mob) < boss->GetExactDist(nearest))
            nearest = mob;
    }

    if (!count)
    {
        if (room.locked)
            LOG_INFO("playerbots", "[pull order] {}: room cleared, his pull may go ahead", boss->GetName());

        room = BlackheartRoom();
        room.checkedMs = now;
        return false;
    }

    if (!room.fewest || count < room.fewest)
    {
        room.fewest = count;
        room.fewestSinceMs = now;
        room.released = false;
    }

    if (!room.released && getMSTimeDiff(room.fewestSinceMs, now) > BLACKHEART_ROOM_STALL_MS)
    {
        room.released = true;
        LOG_INFO("playerbots", "[pull order] {}: {} idle mobs within {:.0f} yd not cleared for {} s, the hold ends "
                 "(nearest {} at {:.1f} yd)", boss->GetName(), count, BLACKHEART_CALL_RADIUS,
                 BLACKHEART_ROOM_STALL_MS / 1000, nearest->GetName(), boss->GetExactDist(nearest));
    }

    if (count != room.count && !room.released)
        LOG_INFO("playerbots", "[pull order] {}: held, {} idle mobs within {:.0f} yd answer his engage call "
                 "(nearest {} at {:.1f} yd)", boss->GetName(), count, BLACKHEART_CALL_RADIUS, nearest->GetName(),
                 boss->GetExactDist(nearest));

    room.count = count;
    room.locked = !room.released;
    return room.locked;
}
}
