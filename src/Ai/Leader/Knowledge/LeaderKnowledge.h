/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LEADERKNOWLEDGE_H
#define PLAYERBOTS_LEADERKNOWLEDGE_H

#include "Define.h"

#include <string>
#include <vector>

// [lead35] Leader::Knowledge (working name): the dungeon knowledge the leader needs in live play - event
// starters, event spots, route links, area gates, door events, guarded doors, event items, class-gated
// bosses, absent script slots, encounter requirements / credits, key door passes. Moved out of the test
// harness (TestHarnessData); the leader QUERIES it, the harness reads it too.
#include "ObjectGuid.h"

namespace Leader::Knowledge
{
    enum class StarterKind : uint8
    {
        Gossip,        // talk to the creature, pick the first offered option of the preference list
        UseGameObject, // click the game object once it is selectable
        UseItem,       // stand at the game object (entries) and use the bag item itemId on it, as the client does
        Escort,        // gossip that starts an escort: the group then stays with the escorted creature
        // [brd_bar_buy] buy buyCount of itemId from the vendor creature (entries), as the client's CMSG_BUY_ITEM does
        VendorBuy,
        // [brd_bar_turnin] accept and hand in the quest questId at the quest giver (entries) with its items in the bags
        QuestTurnIn
    };

    struct EventStarterDef
    {
        uint32 mapId;
        StarterKind kind;
        std::vector<uint32> entries;     // any of these (faction variants)
        std::vector<uint32> optionPrefs; // gossip: GossipListId preference per menu page; empty = first item
        bool priority;                   // done before any trash (the instance does not start without it)
        uint8 maxUses;                   // ToC5 herald is talked to once per stage
        uint32 cooldownMs;
        char const* note;
        bool allMembers = false;         // summoning-ritual objects (ZA gong) need every member to click
        // UseItem: the item a real group brings (quest reward / drop a player of that level can get). The harness
        // puts one into the leader's bags at the start as test preparation (eventItems in TestHarnessData.cpp).
        uint32 itemId = 0;
        // Only once creatures of all these entries were killed by the party or a boss track naming them is done
        // (Sunken Temple: the Shade of Hakkar altar after Jammal'an, Dire Maul: Tortheldrin after the pylons).
        std::vector<uint32> afterDead = {};
        // Escort: the creature the group stays with until it dies or stays idle (0 = the gossip creature itself).
        uint32 escortEntry = 0;
        // Only once the starters of the same map naming these entries were used up (uses == maxUses): a fixed order
        // (Sunken Temple statues 1..6, Old Hillsbrad: the barrels before Thrall).
        std::vector<uint32> afterUsed = {};
        // Each use on another object of the entries (three Thunderbrew kegs); UseItem starters always are.
        bool distinct = false;
        // Not in this content (ContentDef::key) of a map with several wings (Blackrock Spire: an LBRS group does
        // not click the UBRS altar); nullptr: every content of the map.
        char const* skipContent = nullptr;
        // Gossip: only the optionPrefs items are ever chosen, never "the first offered one". A menu without them means
        // the step is past (Violet Hold: Sinclari offers only the late-join teleport once the bots' own VH strategy
        // started the event); the use is counted without a select.
        bool strictOptions = false;
        // The event this starter begins is past once a creature of these entries is alive or was killed: the starter
        // counts as used up (Old Hillsbrad: Lieutenant Drake 17848 comes after the fifth barrel; the bots' own
        // tbc-ohb step plants barrels too, the harness counted only its own and planted four more after Drake died,
        // 2026-10-25 ohb-h 22:39-24:42, while Thrall's escort waited on it).
        std::vector<uint32> doneWhen = {};
        // Only once the instance script's boss state of this slot is DONE (-1: none). Blackrock Spire: the Emberseer In
        // door 175244 opens with DATA_DRAGONSPIRE_ROOM 15 (instance_blackrock_spire.cpp), the bots' altar step walked
        // the tank into the closed door for minutes (2026-10-25 brs-h).
        int32 afterBossDone = -1;
        // Fields appended by HARNESS_PATCH_classic (parts A, B, C, 0 in that order). Positional order of a row:
        // mapId, kind, entries, optionPrefs, priority, maxUses, cooldownMs, note, allMembers, itemId, afterDead,
        // escortEntry, afterUsed, distinct, skipContent, strictOptions, doneWhen, afterBossDone, expectDoor,
        // sameWingOnly, priorityItem, doneWhenAuraAbsent, clearRadius, opensDoor, gatherRadius, passDoors, questId,
        // buyCount.
        // [expect_door] UseItem: the door (GO entry) this use opens. The use counts only once that door left
        // GO_STATE_READY EXPECT_DOOR_MS after it; otherwise the starter is armed again (EXPECT_DOOR_MAX_RETRIES). The
        // cast is made out of combat, within EXPECT_DOOR_CAST_RANGE and in line of sight of the object (Deadmines:
        // Fire Cannon 6250, 2 s cast, 5 yd, broken by moving; the Iron Clad Door 16397 goes to state 2 500 ms after).
        uint32 expectDoor = 0;
        // [sm-chapel-wing] Only while the group is in the object's own wing (a map with several entrances: the
        // entrance nearest to the object is Party::entranceIdx; a wing content: InThisWing). Scarlet Monastery heroic
        // 062726: the Chapel Door starter was chosen from the Graveyard at 41 s (starter_partial_walk, endLeft 657 yd).
        bool sameWingOnly = false;
        // [zf-gong-priority] Priority (as `priority`) while the leader carries this item (Zul'Farrak: the Mallet of
        // Zul'Farrak 9240 for the Gong 141832). 0: none.
        uint32 priorityItem = 0;
        // [ulda-altar-confirm] doneWhen counts a living creature only when it is awake: in combat or without this aura
        // (Uldaman: Stoned 10255 on the Stone Keepers 4857, spell_uldaman_stoned_aura). 0: any living one counts.
        uint32 doneWhenAuraAbsent = 0;
        // [ulda-altar-clear] Used only out of combat and with no attackable hostile (other than doneWhen entries)
        // within this many yards of the object; 0: no check. A summoning ritual's 5 s channel breaks when the user
        // moves.
        float clearRadius = 0.0f;
        // [dme-ironbark-door] Gossip / UseGameObject: the door this step opens. Used up, but the door still
        // GO_STATE_READY STARTER_DOOR_CHECK_MS after the use while the creature offers its gossip again: armed again
        // (at most STARTER_DOOR_REARM_MAX). 0: none.
        uint32 opensDoor = 0;
        // [starter_wait_group] Used only once every alive member stands within this many yards of the object (a gate
        // closes on the use: Scholomance's Kirtonos gate 175570 is 37 yd from the brazier); 0 = no wait. Gives up
        // after STARTER_GATHER_MAX_MS.
        float gatherRadius = 0.0f;
        // [door_hold_pass] Closed doors that are no barrier for this starter's walk or its escort (Gnomeregan: the
        // Cave Ins 146085 / 146086 are opened and sealed by Emi's own SAI); empty: none.
        std::vector<uint32> passDoors = {};
        // [brd_bar_turnin] QuestTurnIn: the quest handed in (Blackrock Depths: Rocknot's Ale 4295). 0: none.
        uint32 questId = 0;
        // [brd_bar_buy] VendorBuy: how many of itemId to buy from the vendor (entries). 0: none.
        uint32 buyCount = 0;
    };

    // A script boss slot that has no boss on that difficulty (Anzu and Yor exist in heroic only): never required,
    // listed in run_start.absentSlots with the reason.
    struct AbsentSlotDef
    {
        uint32 mapId;
        uint8 difficulty;
        uint32 bossId;
        char const* reason;
    };

    // [hleft-bm-quest] A quest a real group has in its log for an event (test preparation like the carried keys):
    // Black Morass - Sa'at 20201 offers the Chrono-beacon 24289 (gossip 8088, conditions: quest 10297 "The Opening of
    // the Dark Portal" taken or rewarded) only then. Given to every member at the start when none of them has it.
    struct PrepQuestDef
    {
        uint32 mapId;
        uint32 questId;
        char const* note;
    };

    // [hleft-boss-preclear] Trash a boss is never pulled next to: while an idle hostile non-boss creature (of
    // `entries`, empty: any) lives within `radius` of (x, y, z), the boss is no next boss and that trash is required
    // (scored first, however far). Drak'Tharon Keep 2026-10-25 (dtk-h 20261025-064751): the Wretched Belcher patrol and
    // the Ghoul Tormentor pack by Trollgore joined his pull - 6 deaths; King Dred heroic: his Scytheclaw / Gutripper.
    struct BossPreClearDef
    {
        uint32 mapId;
        int8 difficulty;  // -1: every difficulty
        uint32 bossEntry;
        float x;
        float y;
        float z;
        float radius;
        std::vector<uint32> entries;
        char const* note;
    };

    // [hleft-kill-summon] A boss the instance summons when a group of trash dies (no click, no gossip): while his slot
    // is open, the slot afterBossDone is DONE and no creature of bossEntry lives, the trash of `entries` is required
    // (scored first, however far: guardKeys). Gundrak heroic: Eck the Ferocious 29932 comes when the Ruins Dweller
    // 29920 formation behind Moorabi's Eck door 192632 is dead (instance_gundrak.cpp OnUnitDeath, heroic only).
    struct KillSummonDef
    {
        uint32 mapId;
        uint8 difficulty;
        std::vector<uint32> entries;
        int32 afterBossDone;
        uint32 bossSlot;
        uint32 bossEntry;
        char const* note;
    };

    // [hleft-script-slot-names] A script boss slot the harness names from the instance enum (the slot keeps no
    // creature guid, so it stayed "boss N" next to its DungeonEncounter.dbc row: Culling of Stratholme heroic logged
    // "boss 3" and "Mal'ganis" as two kills, and slot 4 - the timed Infinite Corruptor - counted as a phantom slot,
    // its FAIL never seen and the run "done"). entry / order bind the slot (DropDbcTwins drops the dbc twin by
    // order); failNote: the slot's FAIL is the boss lost for good (a timer), reported as missing with this reason.
    // difficulty -1: every difficulty.
    struct ScriptSlotDef
    {
        uint32 mapId;
        int8 difficulty;
        uint32 bossId;
        char const* name;
        uint32 entry;
        int32 order;
        char const* failNote;
    };

    // Where a proximity-started event begins (Hellfire Ramparts: Vazruden comes when the group stands on the
    // platform) or where its next stage waits: with nothing hostile reachable, the leader walks there.
    struct EventSpotDef
    {
        uint32 mapId;
        float x;
        float y;
        float z;
        std::vector<uint32> afterDead;  // creature entries killed first (the bosses before the event)
        std::vector<uint32> untilDead;  // the spot is done once these are dead (the event's own bosses)
        char const* note;
        // false: a step of a walk (Pit of Saron tunnel triggers), done once reached - the next spot of the map
        // follows. true: the event's waiting place (Black Morass: Medivh), the group comes back to it whenever
        // nothing hostile is left, until untilDead are dead.
        bool hold = false;
        // A hold spot: only what stands this close to it is a target (bosses always); 0 = HOLD_TARGET_RADIUS.
        float holdRadius = 0.0f;
        // The spot applies only while the leader stands above this height (Azjol-Nerub: the shaft floor, until the
        // group dropped through the hole).
        float leaderMinZ = -100000.0f;
        // A hold spot where only what attacks the group is fought: idle non-boss mobs within holdRadius are no
        // target either, so the group gathers at the spot (Azjol-Nerub: the hole's rim, the drop needs the group
        // together out of combat).
        bool fightOnlyAttackers = false;
        // A hold spot: these entries are targets at any distance (Black Morass: the rift keepers stand at their rift,
        // 87-144 yd from Medivh; only their death closes it).
        std::vector<uint32> holdAlways;
        // The spot applies only while the leader stands below this height (Shattered Halls: in the sewer, after the
        // jump past the closed door).
        float leaderMaxZ = 100000.0f;
        // A hold spot: these entries standing within guardRadius of it attack the protected NPC (Black Morass: the
        // rift adds channel Corrupt on Medivh from a ring 14 yd round him) - out of combat they come before holdAlways.
        std::vector<uint32> guardEntries;
        float guardRadius = 0.0f;
        // Fields appended by HARNESS_PATCH_classic (parts A, C). Positional order of a row: mapId, x, y, z, afterDead,
        // untilDead, note, hold, holdRadius, leaderMinZ, fightOnlyAttackers, holdAlways, leaderMaxZ, guardEntries,
        // guardRadius, eventData, afterUsed, bossComesToSpot, bossOnlyNear.
        // [brd_ring_hold] A step spot (hold false) that holds while its event runs: InstanceScript::GetData(eventData)
        // == IN_PROGRESS, or a failed run of it waits for its retry (Party::eventSpotRetry). It is then the hold spot
        // (holdRadius, fightOnlyAttackers; idle bosses outside holdRadius are no target either, no starter). -1: none.
        // Blackrock Depths' Ring of Law: TYPE_RING_OF_LAW 1 (the waves need the group in the arena).
        int32 eventData = -1;
        // Only once the starters of the map naming these entries are used up (StartersUsed): a hold that begins with
        // the use (Urok's pile, Kirtonos' brazier), not with the boss before it.
        std::vector<uint32> afterUsed = {};
        // [event_boss_waits] Its untilDead boss stands out of reach until the event brings him (UBRS: Rend
        // NON_ATTACKABLE on his balcony behind the exit portcullis 175186 that only his death opens): while this
        // spot's afterDead are dead he is no closed-door / unreachable skip and holds no order back - the spot leads.
        bool bossComesToSpot = false;
        // A hold spot whose bosses are targets only within holdRadius too, out of combat (Maraudon: Rotgrip swims a
        // 230 yd patrol 7-11 yd under the surface; the group waits for him at one point of it).
        bool bossOnlyNear = false;
        // [b32_pos] A step spot (hold false) that, once reached, holds until InstanceScript::GetData(progressData) >=
        // progressMin: the trigger at the step advances the event only when its condition is met, and only on a
        // fire (Pit of Saron: areatrigger 5579 -> npc_pos_tyrannus_events SetData(1, 2) needs the 10 ambushers of
        // 5578 dead, 5580 -> SetData(1, 3) the 6 / 12 of 5579; a step "done once reached" walked off with the
        // ambush alive, the progress stopped at AFTER_WARN_1 and at_tyrannus_event_starter refused for good).
        // While it holds, progressTrigger re-fires every TRIGGER_STEP_REFIRE_MS with no TRIGGER_MAX_FIRES cap.
        // -1: none.
        int32 progressData = -1;
        uint32 progressMin = 0;
        uint32 progressTrigger = 0;
        // [kara3] The spot is done once InstanceScript::GetData(doneData) == DONE (an event no kill ends: Karazhan's
        // Chess Event, DATA_CHESS_EVENT 8). -1: none.
        int32 doneData = -1;
        // [kara6] A step spot (hold false) that holds while this instance boss slot is IN_PROGRESS (an event of a
        // boss state, not of instance data: Karazhan's Opera, SetBossState(DATA_OPERA_PERFORMANCE 3)), as
        // eventData does for GetData. -1: none.
        int32 eventBossSlot = -1;
    };

    // A walkable connection the navmesh does not have: a bridge the instance script raises as a game object (the
    // mmaps hold no game objects). The harness walks the group over it, every member from the same start point,
    // once the object is in goState and afterDead are dead, when the next boss is reachable only through it.
    // A lift (goEntry of GAMEOBJECT_TYPE_TRANSPORT, The Mechanar): goState is not checked and there is no ground on
    // the line; the members get the far end as their goal and the dungeon strategy rides the lift there.
    struct RouteLinkDef
    {
        uint32 mapId;
        float fromX;
        float fromY;
        float fromZ;
        float toX;
        float toY;
        float toZ;
        uint32 goEntry;  // the bridge / collision object, 0 for none
        uint32 goState;  // GOState it must be in
        std::vector<uint32> afterDead;
        char const* note;
        // A jump down (Shattered Halls: into the sewer past the closed Grand Warlock door): no ground on the line, each
        // member jumps from the start to the far end (TestRunner::Waypoint::jump). The drop is allowed only here.
        bool jump = false;
    };

    // A part of an instance closed until an event opened it (a bridge, a gate): nothing inside the box is a target
    // until the creatures of afterDead are dead. The partial-path rule would otherwise send the leader at what stands
    // there from the nearest walkable edge (Gundrak wave 7: the Rhinos of Gal'darah's wing from the entrance ledge).
    // Empty afterDead: closed for the whole run (a place a group never fights in: the Ring of Law stands).
    struct AreaGateDef
    {
        uint32 mapId;
        float minX;
        float maxX;
        float minY;
        float maxY;
        float minZ;
        float maxZ;
        std::vector<uint32> afterDead;
        char const* note;
        // Only in this content (ContentDef::key) of a map with several wings (Blackrock Spire: UBRS is closed to an
        // LBRS group, not to a UBRS one); nullptr: every content of the map.
        char const* onlyContent = nullptr;
        // A closed wing (onlyContent): where a leader that ended up inside (a fight that ran in, a chase) walks out
        // to, out of combat, before the route goes on. exitZ <= -100000: none.
        float exitX = 0.0f;
        float exitY = 0.0f;
        float exitZ = -100000.0f;
    };

    // A closed door (GAMEOBJECT_TYPE_DOOR in GO_STATE_READY) that an event the harness knows opens: engaging (or
    // killing) the key creature. A route through it is no way while it is closed, except for the key creature
    // itself: the leader stops before the door and the pull opens it. afterData: the key creature evades while the
    // instance's GetData(afterData) is 0 (an earlier step of the event has not started), so it is no target before
    // (Utgarde Keep: Forge Master n needs forge n-1 started).
    struct DoorEventDef
    {
        uint32 mapId;
        uint32 doorEntry;
        uint32 keyEntry;
        float keyX;  // the key creature's home (several creatures of keyEntry: the nearest within DOOR_KEY_MATCH)
        float keyY;
        uint32 afterData;  // 0: none
        char const* note;
    };
    constexpr float DOOR_KEY_MATCH = 5.0f;

    // A closed door the instance opens once guard groups are dead - no key creature, no use (Dire Maul West: the
    // force field round Immol'thar). Each guarded object turns itself off while no creature of guardEntries is alive
    // within guardRadius of it; all off, the script opens the door. While the door is closed, the guards of the
    // objects still in goState are objectives (scored first, never detours, required) and the boss behind the door
    // stays the next boss - no "behind a closed door" skip, no order hold.
    // [st-guard-approach] Where the walk to one guard goes first: a point on the guard's own floor (navmesh polygon
    // centre, tools/navmesh_probe.py), 20-30 yd from it.
    struct GuardApproachDef
    {
        uint32 entry;
        float x;
        float y;
        float z;
    };

    struct GuardedDoorDef
    {
        uint32 mapId;
        uint32 doorEntry;
        // the guarded objects; [st-guard-door-anywhere] empty: the door itself opens once no guard lives (Sunken
        // Temple's forcefield, Uldaman's Temple Door) - guards within guardRadius of the DOOR, 0 = anywhere on the map
        std::vector<uint32> goEntries;
        uint32 goState;                 // GOState while still guarded (1 = GO_STATE_READY)
        std::vector<uint32> guardEntries;
        float guardRadius;
        char const* note;
        std::vector<GuardApproachDef> approaches = {};
    };

    // A boss only one class can unlock (a class quest item summons it). User rule: the only bosses a run may leave
    // alive; each with its reason, listed in run_start / run_end as excluded, never silently.
    struct ClassGatedBossDef
    {
        uint32 mapId;
        uint8 difficulty;
        uint32 entry;
        char const* reason;
    };

    // An item a real group carries into the dungeon to start an event (EventStarterDef::itemId and items used
    // by the dungeon strategies), prepared in the leader's bags like a consumable.
    struct EventItemDef
    {
        uint32 mapId;
        uint32 itemId;
        uint32 count;
        char const* source;  // how a player of that level gets it (quest / drop), for the documentation
        // A drop of this dungeon (0: carried in from the start). The group loots it when these creatures are dead;
        // the harness bots do not loot (strategy -loot), so the 100 % drop is put into the leader's bags then.
        std::vector<uint32> dropFrom = {};
        bool perKill = false;  // one item per kill of dropFrom[0] (Hakkari Blood from each Bloodkeeper)
        int8 difficulty = -1;  // only for that difficulty (Mana-Tombs heroic key), -1 every difficulty
        // [b32_strat_gate] every member carries one (a key any player gets from the quest / the drop: Stratholme's
        // Key to the City). A member cut off behind its gate keys it from its own side (MemberKeyAtDoor).
        bool everyMember = false;
    };

    // [strat_key_pass] An auto-closing key gate (GAMEOBJECT_TYPE_DOOR, data2 autoclose ms; Stratholme: 3000) the whole
    // group passes at once: the key is used from (useX..) with every member within KEY_PASS_GATHER_RANGE of it, out of
    // combat; the moment it opens every member on that side gets (farX..) as its waypoint (TickKeyDoorPass).
    struct KeyDoorPassDef
    {
        uint32 mapId;
        uint32 doorEntry;
        float useX;
        float useY;
        float useZ;
        float farX;
        float farY;
        float farZ;
        char const* note;
    };

    // Known encounter starts the harness cannot do honestly yet; logged once per run as "known_gap".
    struct KnownGapDef
    {
        uint32 mapId;
        char const* note;
    };

    // [lead102] the health (percent) at which a creature's SmartAI flees for assistance (smart_scripts: a
    // SMART_EVENT_HEALTH_PCT event whose action is SMART_ACTION_FLEE_FOR_ASSIST, the event's upper bound); 0: it does
    // not. Read once per entry from the loaded SmartAI scripts
    float FleeForAssistHealthPct(uint32 creatureEntry);

    // [lead231] the creature has an attack that hits in front of it (a cone spell: its template spells or a SmartAI
    // cast); read once per entry. A tank turns such a mob away from the group; any other mob needs no turning
    bool HasFrontalAttack(uint32 creatureEntry);

    // [lead234] the group's measured damage per second on the bosses it killed in this run (their max health over
    // the fights' seconds, all its boss kills together); 0 before its first boss kill
    void NoteBossKill(ObjectGuid group, float bossHealth, float fightSec);
    float BossDps(ObjectGuid group);

    std::vector<EventStarterDef const*> EventStarters(uint32 mapId);
    std::vector<EventSpotDef const*> EventSpots(uint32 mapId);
    std::vector<RouteLinkDef const*> RouteLinks(uint32 mapId);
    std::vector<AreaGateDef const*> AreaGates(uint32 mapId);
    std::vector<DoorEventDef const*> DoorEvents(uint32 mapId);
    std::vector<GuardedDoorDef const*> GuardedDoors(uint32 mapId);
    std::vector<EventItemDef const*> EventItems(uint32 mapId);
    // Reason a boss of that map / difficulty is class-gated (ClassGatedBossDef), nullptr for every other boss.
    char const* ClassGatedBoss(uint32 mapId, uint8 difficulty, uint32 creatureEntry);
    std::vector<ClassGatedBossDef const*> ClassGatedBosses(uint32 mapId, uint8 difficulty);
    // Reason a script slot is absent on that difficulty (AbsentSlotDef), nullptr when it is a real boss.
    char const* AbsentScriptSlot(uint32 mapId, uint8 difficulty, uint32 bossId);
    std::vector<AbsentSlotDef const*> AbsentScriptSlots(uint32 mapId, uint8 difficulty);
    std::vector<ScriptSlotDef const*> ScriptSlots(uint32 mapId, uint8 difficulty);
    std::vector<KillSummonDef const*> KillSummons(uint32 mapId, uint8 difficulty);
    std::vector<BossPreClearDef const*> BossPreClears(uint32 mapId, uint8 difficulty);
    std::vector<PrepQuestDef const*> PrepQuests(uint32 mapId);
    // What a boss needs to appear (event, summoning item, rare spawn) - a reference for bossesMissing and future
    // tactics, never an exemption: "done" needs every boss dead. Kill-credit creature entry; nullptr if nothing.
    char const* EncounterRequirement(uint32 mapId, uint32 creatureEntry, char const** note = nullptr);
    // [st-phased-until-boss] encounterRequirements kind of a boss the instance hides (phase / NOT_SELECTABLE) until an
    // earlier boss dies: named in the report, still no next boss while hidden (Retarget's phase skip).
    constexpr char const* PHASED_UNTIL_BOSS = "phased until boss";
    // EncounterRequirement's "summon item missing" item (0: none).
    uint32 EncounterRequirementItem(uint32 mapId, uint32 creatureEntry);
    // Entries the instance script swaps a kill-credit creature for (the other team's NPC); empty if none.
    std::vector<uint32> EncounterCreditAliases(uint32 mapId, uint32 creditEntry);
    // [dm_shredder_required] A creature whose death brings an encounter's credit creature (Deadmines: Sneed's Shredder
    // 642 casts Eject Sneed 5141 on death, which summons Sneed 643): a required boss creature of the route, never the
    // encounter's credit. The credit entry, 0 for none.
    uint32 EncounterCarrierCredit(uint32 mapId, uint32 creatureEntry);
    // [strat_key_pass] An auto-closing key gate the group passes at once (KeyDoorPassDef), nullptr for none.
    KeyDoorPassDef const* KeyDoorPass(uint32 mapId, uint32 doorEntry);
    // Trash that cannot die while a boss lives (Nexus: a Crystalline Frayer turns into a Seed Pod and comes back
    // until Ormorok is dead, instance_nexus.cpp). Returns that boss's entry, 0 for ordinary trash.
    uint32 DeathlessUntilBoss(uint32 mapId, uint32 creatureEntry);
    // A dbc encounter credited by no kill: the instance data id that is DONE once it is done, 0 for none.
    uint32 EncounterDoneData(uint32 mapId, uint32 creditEntry);
    // [kara3] A script boss slot whose state the instance keeps as data (GetData), UINT32_MAX for none.
    uint32 ScriptSlotData(uint32 mapId, uint32 bossId);
    std::vector<KnownGapDef const*> KnownGaps(uint32 mapId);

    // [b32_strat_gate] [lead107 final] the members the leader found cut off behind a closed door (its TickDoorCut),
    // moved out of the test runner's registry (TestRunner::SetDoorCut / IsDoorCut): written on the world thread,
    // read by the tank's pull gate on the map threads (LeaderPlanner, "behind a closed door")
    void SetDoorCut(ObjectGuid bot, bool cut);
    bool IsDoorCut(ObjectGuid bot);
    void ClearDoorCuts();
}  // namespace Leader::Knowledge

#endif
