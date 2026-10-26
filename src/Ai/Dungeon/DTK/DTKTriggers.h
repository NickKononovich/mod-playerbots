/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DTKTRIGGERS_H
#define PLAYERBOTS_DTKTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

enum DrakTharonIDs
{
    // Trollgore
    NPC_DRAKKARI_INVADER               = 27709,

    // Novos the Summoner
    NPC_NOVOS                          = 26631,
    SPELL_ARCANE_FIELD                 = 47346,
    NPC_CRYSTAL_HANDLER                = 26627,
    NPC_HULKING_CORPSE                 = 27597,
    NPC_RISEN_SHADOWCASTER             = 27600,
    NPC_FETID_TROLL_CORPSE             = 27598,

    // The Prophet Tharon'ja
    SPELL_GIFT_OF_THARONJA             = 52509,
};

namespace DTKIds
{
constexpr uint32 MAP_ID = 600;
// boss_trollgore.cpp: Summon Invader A/B/C (49456/49457/49458) summon one of each entry
constexpr uint32 NPC_DRAKKARI_INVADER_B = 27753;
constexpr uint32 NPC_DRAKKARI_INVADER_C = 27754;
// boss_novos.cpp: the Crystal Handlers are summoned by these; killing one softlocks the encounter
constexpr uint32 NPC_NOVOS_SUMMON_TARGET = 27583;
constexpr uint32 NPC_CRYSTAL_CHANNEL_TARGET = 26712;

inline bool IsTrollgoreInvader(uint32 entry)
{
    return entry == NPC_DRAKKARI_INVADER || entry == NPC_DRAKKARI_INVADER_B || entry == NPC_DRAKKARI_INVADER_C;
}

// boss_trollgore.cpp CheckInRoom: he evades (EVADE_REASON_BOUNDARY) once his Y leaves [-700, -628]; he follows his
// victim, so the group keeps its fight inside that band, held near his home (-266.2, -660.1, 26.5).
constexpr float TROLLGORE_ROOM_MIN_Y = -700.0f;
constexpr float TROLLGORE_ROOM_MAX_Y = -628.0f;
constexpr float TROLLGORE_ROOM_MARGIN = 8.0f;
constexpr float TROLLGORE_HOLD_X = -266.2f;
constexpr float TROLLGORE_HOLD_Y = -664.0f;
constexpr float TROLLGORE_HOLD_Z = 26.5f;

inline bool IsInsideTrollgoreRoom(float y, float margin)
{
    return y >= TROLLGORE_ROOM_MIN_Y + margin && y <= TROLLGORE_ROOM_MAX_Y - margin;
}

inline bool IsNovosNeverTarget(uint32 entry)
{
    return entry == NPC_NOVOS_SUMMON_TARGET || entry == NPC_CRYSTAL_CHANNEL_TARGET;
}

// drak_tharon_keep.h DATA_TROLLGORE; creature_template 26630.
constexpr uint32 DATA_TROLLGORE = 0;
constexpr uint32 NPC_TROLLGORE = 26630;

// boss_dred.cpp: Bellowing Roar at 67 / 34 % health (ScheduleHealthCheckEvent, delayed while he casts), Grievous
// Bite (a DoT until its target is at full health) closes every claw chain, heroic Raptor Call every 30 s makes a
// living, idle Scytheclaw or Gutripper within 100 yd attack his victim.
constexpr uint32 NPC_KING_DRED = 27483;
constexpr uint32 NPC_DRAKKARI_SCYTHECLAW = 26628;
constexpr uint32 NPC_DRAKKARI_GUTRIPPER = 26641;
constexpr uint32 SPELL_BELLOWING_ROAR = 22686;  // Spell.dbc: 1.5 s cast, fear (mechanic 5) 4 s, 35 yd around him
constexpr uint32 SPELL_GRIEVOUS_BITE = 48920;   // no heroic variant in spelldifficulty_dbc
constexpr float BELLOWING_ROAR_RADIUS = 35.0f;
constexpr float DRED_FIRST_ROAR_PCT = 67.0f;
constexpr float DRED_SECOND_ROAR_PCT = 34.0f;
constexpr float DRED_ROAR_LEAD_PCT = 5.0f;   // the counters go up this far above a threshold
constexpr float DRED_ROAR_LATE_PCT = 5.0f;   // and stay wanted this far below it (the roar waits for his casts)

inline bool IsDredRaptor(uint32 entry) { return entry == NPC_DRAKKARI_SCYTHECLAW || entry == NPC_DRAKKARI_GUTRIPPER; }
}  // namespace DTKIds

class CorpseExplodeTrigger : public Trigger
{
public:
    CorpseExplodeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "corpse explode") {}
    bool IsActive() override;
};

// Trollgore engaged by the group while this bot (and, for his victim, the boss behind it) drifts out of his room.
class TrollgoreOutOfRoomTrigger : public Trigger
{
public:
    TrollgoreOutOfRoomTrigger(PlayerbotAI* botAI) : Trigger(botAI, "trollgore out of room") {}
    bool IsActive() override;

private:
    // [b33_dtk] Trollgore engaged from afar (his wave or a far threat entry; dtk-n-5A-43 193 s: tank 84 yd off while
    // a Belcher patrol fought the group in the hall): no hold-room run while trash is on the group, then one
    // controlled walk in (the tank leads, nobody runs ahead of it, room trash only once the group is ready).
    bool FarEngageHold(Unit* boss);
    uint32 _waitSinceMs = 0;
};

class ArcaneFieldTrigger : public Trigger
{
public:
    ArcaneFieldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "arcane field") {}
    bool IsActive() override;
};

// The bot stands in Novos' Arcane Field: its own aura from the persistent area, an O(1) test.
// Novos the Summoner never aggroes on sight (boss_novos.cpp: empty MoveInLineOfSight, rooted on his platform): the
// tank opens on him once the group is with it (dtk-n-5A-17 build 15: the group stood 154 s 25 yd from him).
class NovosPullTrigger : public Trigger
{
public:
    NovosPullTrigger(PlayerbotAI* botAI) : Trigger(botAI, "novos pull") {}
    bool IsActive() override;
};

class InArcaneFieldTrigger : public Trigger
{
public:
    InArcaneFieldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "in arcane field") {}
    bool IsActive() override;
};

namespace DTKShared
{
// Arcane Field 47346: an 11 yd persistent area aura Novos drops on himself (triggered, so never a current spell).
constexpr float ARCANE_FIELD_RADIUS = 11.0f;
Unit* ArcaneFieldCenter(PlayerbotAI* botAI);

// One "[tag] bot: message" line per bot, tag and key at most every `intervalMs`.
void Log(Player* bot, char const* tag, std::string const& key, std::string const& message, uint32 intervalMs);

// A living attacker of the group with this entry ("attackers"), or nullptr.
Unit* AttackerWithEntry(PlayerbotAI* botAI, uint32 entry);

// King Dred in the group's fight, or nullptr.
Unit* KingDred(PlayerbotAI* botAI);
// Bellowing Roar is cast now, or his health is in the band around one of its thresholds.
bool DredFearWindow(Unit* dred);
// The member a priest Fear Wards before a roar (the tank, else the healer), or nullptr.
Player* DredFearWardTarget(PlayerbotAI* botAI, Unit* dred);
// A healer's member under Grievous Bite below full health, in heal range (nullptr while anybody is low).
Player* DredBittenMember(PlayerbotAI* botAI);
// A member of the group (or a pet of one) is in combat with the unit.
bool EngagedWithGroup(Player* bot, Unit* unit);
// The group's living tank on the bot's map other than the bot, or nullptr.
Player* GroupTank(Player* bot);
// Before Trollgore is down only the tank opens a fight in the halls before his room.
bool BeforeTrollgore(Player* bot);
}

// Shaman: Tremor Totem down; priest: Fear Ward on the tank (else the healer); warrior: Berserker Rage when Bellowing
// Roar starts in reach.
class KingDredFearGuardTrigger : public Trigger
{
public:
    KingDredFearGuardTrigger(PlayerbotAI* botAI) : Trigger(botAI, "king dred fear guard", 250) {}
    bool IsActive() override;
};

// A healer sees a member under Grievous Bite below full health (the bite only ends at full).
class KingDredGrievousBiteTrigger : public Trigger
{
public:
    KingDredGrievousBiteTrigger(PlayerbotAI* botAI) : Trigger(botAI, "king dred grievous bite", 500) {}
    bool IsActive() override;
};

// class CrystalHandlerTrigger : public Trigger
// {
// public:
//     CrystalHandlerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "crystal handler") {}
//     bool IsActive() override;
// };

class GiftOfTharonjaTrigger : public Trigger
{
public:
    GiftOfTharonjaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "gift of tharon'ja") {}
    bool IsActive() override;
};

class NovosSummonTargetTargetedTrigger : public Trigger
{
public:
    NovosSummonTargetTargetedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "novos summon target targeted") {}
    bool IsActive() override;
};

#endif
