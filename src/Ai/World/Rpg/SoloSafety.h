/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SOLOSAFETY_H
#define PLAYERBOTS_SOLOSAFETY_H

// [guild13] user 2026-11-07: the levelling guild bots stop dying (b294 night: 150 deaths in 2.5 h). A solo questing
// bot plays as a careful player:
// - it pulls only what it wins: the pack its pull wakes (the careful planner's CollectPack: the engage call, the
//   assistance chain, the mobs whose aggro reach covers the fight), their damage on it (the server's numbers,
//   Leader::Fight::AttackerDps, or what that mob was measured to deal, the higher) over the fight its measured damage
//   gives, against its health, its pet's and its heals' (spell data, its mana). The user's rule on con: one mob above
//   its level at a time, and only at full health and mana; two or more above its level at once never.
// - it rests to full before a pull the judge would win only at full;
// - its walks go round the idle mobs (a route search penalty: the reach of each costs the fight it starts; a mob
//   it must never fight is refused while a complete walk round exists);
// - after a death it rests to full and goes to its quest hub before it plans again.
// Applies to player bots under the New RPG that play alone (no group with others), outside instances.

#include "Define.h"
#include "G3D/Vector3.h"
#include "G3D/Vector4.h"
#include "Position.h"

#include <string>
#include <vector>

class Creature;
class Player;
class Unit;

namespace Leader::Travel
{
    struct RoutePenalty;
}

namespace SoloSafety
{
    bool Applies(Player* bot);

    // DeathRecorder's DealDamage hook (map thread): the bot's dealt and taken damage, per fight and per attacker
    void NoteDamage(Unit* attacker, Unit* victim, uint32 damage);
    // DeathRecorder's heal hook: the bot's own heals on itself (its measured self-heal per fight second)
    void NoteHeal(Unit* healer, Unit* target, uint32 heal);

    struct Verdict
    {
        enum class Kind : uint8
        {
            Win,
            RestFirst,
            Refuse
        };
        Kind kind = Kind::Win;
        std::string why;
        uint32 mobs = 0;
        uint32 above = 0;      // mobs above the bot's level
        float damage = 0.0f;   // expected damage on the bot
        float ehpNow = 0.0f;   // health + pet + heals now
        float ehpFull = 0.0f;  // the same at full health and mana
        float fightSec = 0.0f;
        Position at;           // the pack's centre and reach (a refused pack is remembered there)
        float radius = 0.0f;
        uint8 topLevel = 0;
    };
    // the bot's pull of `target` now
    Verdict Judge(Player* bot, Unit* target);
    // the bot engages `target` on the verdict (the fight record's prediction)
    void NoteEngage(Player* bot, Unit* target, Verdict const& verdict);

    // the bot wants to rest to full (a pull that needs it, a revive); `mana`: the drink side
    bool WantsRest(Player* bot, bool mana, std::string* why = nullptr);
    void RequestRest(Player* bot, std::string const& why);
    // it wants to rest and has nothing to eat / drink for it: it sits and regenerates
    bool WaitsToRegen(Player* bot);

    // [guild15] the walk's stretch in sight toward `dest` passes an idle mob whose fight the judge wins only at full:
    // the bot rests to full first (true: the rest is asked for)
    bool RestBeforeWalk(Player* bot, Position const& dest);
    // [guild15] its gear was repaired: its damage is measured again
    void OnRepaired(Player* bot);
    // the bot died: rest to full and the walk to its hub after the revive
    void OnDeath(Player* bot);
    // once after the revive: true and the hub to walk to
    bool TakeRecovery(Player* bot);
    // the death line's context: attackers now, the fight's start
    std::string DeathContext(Player* bot);

    // Leader::Route for a solo bot: the idle mobs round the walk, refused (`refuse`: x, y, z, radius) or priced
    void RouteCosts(Unit* walker, G3D::Vector3 const& from, G3D::Vector3 const& to, std::vector<G3D::Vector4>& refuse,
                    std::vector<Leader::Travel::RoutePenalty>& penalties);

    // the bot's damage per second: measured over its solo fights, else modelled
    float BotDps(Player* bot);

    // [guild80] the open solo fight projected: the attackers on the bot (their damage per second: this fight's
    // measurement, else the model; their SmartAI spell share), the time its damage needs for them, the damage still to
    // come against its health and its measured heals. lost: damage >= health + heals
    struct Projection
    {
        struct Foe
        {
            Unit* unit = nullptr;
            float hp = 0.0f;
            float dps = 0.0f;
            float spellDps = 0.0f;
        };
        bool open = false;
        bool lost = false;
        bool fleeing = false;
        std::vector<Foe> foes;
        float incoming = 0.0f;
        float seconds = 0.0f;
        float damage = 0.0f;
        float health = 0.0f;
        float heals = 0.0f;
        float botDps = 0.0f;
        float leashSec = 0.0f;
    };
    Projection Project(Player* bot);
    // [guild80] the survival kit chose to run until the mobs leash (the fight closes as DISENGAGED, rest after)
    void BeginFlee(Player* bot, std::string const& why);
    // [guild80] it rests to full once the fight is over (a reset by Vanish / Feign Death / Shadowmeld)
    void RequestRestAfter(Player* bot, std::string const& why);

    // [guild14] the open solo fight is projected lost (the attackers' damage still to come against its health, heals
    // over the time its damage needs to kill them) and running until they leash leaves it alive: it disengages (logged,
    // rests to full after) - only from mobs that cannot follow (rooted, stunned); true from then on until the fight
    // ends
    bool ShouldDisengage(Player* bot);
    // [guild14] it disengages now (cheap: the flag only)
    bool Disengaging(Player* bot);
}

#endif
