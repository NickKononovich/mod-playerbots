/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TOCSHARED_H
#define PLAYERBOTS_TOCSHARED_H

#include "Creature.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

class InstanceScript;
class Player;
class PlayerbotAI;
class Unit;

namespace ToCIds
{
constexpr uint32 MAP_TRIAL_OF_THE_CHAMPION = 650;

// trial_of_the_champion.h eData / eProgress
constexpr uint32 BOSS_ARGENT_CHALLENGE = 1;
constexpr uint32 BOSS_BLACK_KNIGHT = 2;
constexpr uint32 DATA_INSTANCE_PROGRESS = 4;
constexpr uint32 DATA_ANNOUNCER = 5;
constexpr uint32 DATA_ARGENT_CHAMPION = 15;  // DATA_PALETRESS: Eadric or Paletress, whichever was summoned

constexpr uint32 PROGRESS_INITIAL = 0;
constexpr uint32 PROGRESS_CHAMPIONS_DEAD = 6;
constexpr uint32 PROGRESS_ARGENT_CHALLENGE_DIED = 8;

// trial_of_the_champion.cpp: GOSSIP_ACTION_INFO_DEF (1000) + n of npc_announcer_toc5
constexpr uint32 GOSSIP_ACTION_START_JOUST_SKIP_RP = 1002;
constexpr uint32 GOSSIP_ACTION_START_ARGENT_CHALLENGE = 1003;
constexpr uint32 GOSSIP_ACTION_START_BLACK_KNIGHT = 1004;

// boss_argent_challenge.cpp
constexpr uint32 NPC_PALETRESS_BOSS = 34928;
constexpr uint32 NPC_EADRIC_BOSS = 35119;
constexpr uint32 NPC_ARGENT_LIGHTWIELDER_TRASH = 35309;
constexpr uint32 NPC_ARGENT_MONK_TRASH = 35305;
constexpr uint32 NPC_ARGENT_PRIESTESS_TRASH = 35307;
constexpr uint32 SPELL_REFLECTIVE_SHIELD = 66515;
constexpr uint32 SPELL_PALETRESS_RENEW = 66537;
constexpr uint32 SPELL_PALETRESS_RENEW_H = 67675;  // SpellDifficulty.dbc heroic versions
constexpr uint32 SPELL_PALETRESS_HOLY_FIRE = 66538;
constexpr uint32 SPELL_PALETRESS_HOLY_FIRE_H = 67676;
constexpr uint32 SPELL_PALETRESS_HOLY_SMITE = 66536;
constexpr uint32 SPELL_PALETRESS_HOLY_SMITE_H = 67674;
constexpr uint32 SPELL_MEMORY_WAKING_NIGHTMARE = 66552;
constexpr uint32 SPELL_MEMORY_WAKING_NIGHTMARE_H = 67677;
constexpr uint32 SPELL_EADRIC_RADIANCE = 66935;

// boss_black_knight.cpp
constexpr uint32 NPC_BLACK_KNIGHT_BOSS = 35451;
constexpr uint32 NPC_RISEN_JAEREN_GHOUL = 35545;
constexpr uint32 NPC_RISEN_ARELAS_GHOUL = 35564;
constexpr uint32 NPC_RISEN_CHAMPION_GHOUL = 35590;
constexpr uint32 SPELL_GHOUL_EXPLODE = 67729;
constexpr uint32 SPELL_GHOUL_EXPLODE_H = 67886;
constexpr uint32 SPELL_DEATHS_RESPITE = 67745;
constexpr uint32 SPELL_DEATHS_RESPITE_H = 68306;
constexpr uint32 SPELL_MARKED_FOR_DEATH = 67823;  // phase 3: +200% damage taken for 10 s, every 9 s
constexpr uint32 SPELL_MARKED_FOR_DEATH_H = 67882;
constexpr float GHOUL_EXPLODE_RADIUS = 8.0f;  // SpellRadius index 14

// player joust mounts (instance_trial_of_the_champion.cpp eVehicles)
constexpr uint32 NPC_JOUST_WARHORSE = 35644;
constexpr uint32 NPC_JOUST_BATTLEWORG = 36558;
}  // namespace ToCIds

namespace ToCShared
{
InstanceScript* GetInstance(Player* bot);
Creature* GetAnnouncer(Player* bot);
// The Argent Challenge boss of this run when it has the given entry (Eadric or Paletress), else nullptr.
Creature* GetArgentChampion(Player* bot, uint32 entry);
// Paletress while her Reflective Shield is up (the Memory of the Past is alive), else nullptr.
Creature* GetShieldedPaletress(Player* bot);
bool IsMemoryOfThePast(uint32 entry);
bool IsBlackKnightGhoul(uint32 entry);
bool IsJoustMount(uint32 entry);
// Gossip action the announcer offers for this progress, or 0 when no start belongs to it.
uint32 StartGossipAction(uint32 progress);
// Who pulls the passive Argent soldiers: the main tank, or the interactor when no tank is alive.
bool IsPuller(PlayerbotAI* botAI);
// Nearest attackable Argent soldier that is not fighting yet.
Creature* FindIdleSoldier(Player* bot);
// An enemy in combat with the group casting a spell worth a dedicated interrupt that the bot has ready and that
// would actually stop it (most bosses here are immune to MECHANIC_INTERRUPT in creature_immunities).
Unit* FindInterruptTarget(PlayerbotAI* botAI);
// Spends the interrupt FindInterruptTarget found for caster. Returns true on a cast.
bool CastInterrupt(PlayerbotAI* botAI, Unit* caster);
// Paletress or her Memory of the Past under Paletress's Renew (a magic buff) in dispel range; else nullptr.
Unit* FindRenewTarget(PlayerbotAI* botAI);
// The bot's offensive magic dispel (Purge, Spellsteal, Dispel Magic, Tranquilizing Shot) castable on target now.
char const* GetRenewDispel(PlayerbotAI* botAI, Unit* target);
bool HasMarkedForDeath(Unit* unit);
// The personal defensive the Black Knight's Marked For Death target uses now; nullptr when none is ready.
char const* GetMarkedDefensive(PlayerbotAI* botAI);
// The group member under Marked For Death a healer should top up now; nullptr when none needs it.
Player* FindMarkedMemberToHeal(PlayerbotAI* botAI);
// Centre of the Black Knight ghouls casting Explode close enough to hit the bot; false when none.
bool GetGhoulExplodeCenter(PlayerbotAI* botAI, Position& center);
}  // namespace ToCShared

#endif
