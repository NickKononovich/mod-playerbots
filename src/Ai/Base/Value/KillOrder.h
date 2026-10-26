/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_KILLORDER_H
#define PLAYERBOTS_KILLORDER_H

#include "ObjectGuid.h"

#include <vector>

class Player;
class PlayerbotAI;
class Unit;
class WorldObject;

// Trash kill order of a real party: the dps stay on what the tank holds, casters and healers in the tank's pack
// die first, and a mob still loose in the first seconds of a pull is the tank's to pick up, not the dps'.
namespace KillOrder
{
    enum class PackFocus : uint8
    {
        None,      // no tank to follow (solo, no tank, boss fight, scripted targets): the usual choice applies
        Narrowed,  // the tank holds mobs: `narrowed` excludes every attacker outside the best kill tier
        Wait,      // the pull just started and the tank holds nothing yet: pick no target
    };

    // 1 for a caster or healer creature (mage or paladin class with a mana pool) or a unit casting now, else 0.
    uint8 GetCasterTier(Unit* unit);

    // The unit's victim is a tank of `bot`'s group.
    bool IsHeldByTank(Unit* unit, Player* bot);

    // An alive tank of `bot`'s group on its map other than `bot`.
    bool HasHoldingTank(Player* bot);

    // Narrows the dps target choice to the tank's pack, casters first.
    PackFocus NarrowToTankPack(PlayerbotAI* botAI, GuidSet const& exclusions, GuidSet& narrowed);

    // Mobs that `tank` holds, best kill target first (caster tier, then the tank's own target, then lowest
    // health), skipping crowd-controlled, already-marked and `skip` units.
    std::vector<Unit*> RankTankPack(PlayerbotAI* tankAI, GuidSet const& skip);

    // The kill order RankTankPack uses, for any set of mobs: casters within reach of `anchor` (the tank, or the
    // mob it pulls) first, then `current` (the tank's target), then lowest health. Without an anchor `current` leads.
    // Crowd control goes only on the tail of this order (AutoCcMarkAction), never on what the group kills now.
    void SortKillOrder(std::vector<Unit*>& pack, WorldObject const* anchor, Unit const* current);

    // The main tank's Skull/Cross `icon` wants a new mob: it is free, its mob died, or it is the tank's own mark
    // (`ours`) on a mob that left the fight or was crowd-controlled. Anyone else's mark on a live mob stays.
    bool IsKillMarkStale(PlayerbotAI* tankAI, uint8 icon, ObjectGuid ours);

    // The pull of a trash pack is on: in combat, in a group, not in a battleground and no boss encounter running.
    bool IsTrashFight(Player* bot);

    // The fighting tank of `bot`'s group on its map: the main tank, or another alive tank; never `bot` itself.
    Player* FindFightTank(Player* bot);

    // `unit` is part of `tank`'s fight: a boss or a running encounter, out of combat, a mob running for help, within
    // Playerbots.DpsGroupFightRadius of the tank (the tank's own target too), or at its group victim (within reach of
    // the member it attacks). `keep`
    // widens both by a few yards for a target the bot already fights, so it does not flip at the edge.
    bool IsInTankFight(Player* tank, Unit* unit, bool keep = false);

    // 5-man dungeons only (true elsewhere, for the tank, and with no tank): the damage dealers and their crowd
    // control fight the tank's fight, never a mob deep in the room that is in combat (an assist chain, a caster
    // left at range by a line-of-sight pull) but not at the tank or on anyone of the group; it is a target once it
    // comes. The Skull, the Cross and an encounter script's prioritized targets always count.
    bool IsInGroupFight(Player* bot, Unit* unit, bool keep = false);

    // Adds every attacker outside the group's fight (IsInGroupFight) to `exclusions`, with a rate-limited
    // "[dps range]" log line per skipped mob.
    void ExcludeOutsideGroupFight(PlayerbotAI* botAI, GuidSet& exclusions);

    // [b33_hadronox] Boss fight with adds (5-man dungeon, an encounter in progress). User 2026-10-26: the tank holds
    // the adds at the boss, the damage dealers' single target is the boss and their area spells go on the stack (the
    // class "aoe" triggers count the mobs around the current target, so a boss target with the adds on it is the
    // stack). Adds a dungeon strategy names keep their own order: its exclusions (kill first, snake killer, hands
    // off) leave the boss out, and prioritized targets / explicit marks / healer mobs go first.

    // `guid` is one of the main tank's own automatic Skull / Cross marks (not one put out by a player or a script).
    bool IsTankOwnKillMark(Player* bot, ObjectGuid guid);

    // The engaged dungeon boss among `botAI`'s attackers that `exclusions` leave in (alive, attackable, fighting the
    // group), or nullptr. Of several (a council) the current target, else the lowest health.
    Unit* FindFightBoss(PlayerbotAI* botAI, GuidSet const& exclusions);

    // A damage dealer's target in a boss fight with adds: the boss, unless an add goes first (an encounter script's
    // prioritized target, an explicit mark, a healer mob in the fight, a mob on a group healer below
    // BOSS_FOCUS_HEALER_PEEL_PCT that no tank holds). nullptr: the usual choice applies.
    Unit* BossFocusTarget(PlayerbotAI* botAI, GuidSet const& exclusions);

    // The tank keeps the boss it holds (or that nobody holds) instead of walking off to each add: the adds come to it
    // by its taunts and area threat, so they stand on the boss. nullptr while a mob is on a group healer (that one
    // comes first, as before) or outside a 5-man boss fight.
    Unit* TankHoldBoss(PlayerbotAI* tankAI, GuidSet const& exclusions);
}

#endif
