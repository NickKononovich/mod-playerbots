/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaderKnowledge.h"

#include "SharedDefines.h"
#include "SmartScriptMgr.h"  // [lead102] flee for assist
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <cstring>
#include <map>

namespace Leader::Knowledge
{
    namespace
    {
        // From C:\WoW\raid_tactics\dungeons\*.md and the AC scripts / smart_scripts (research 2026-10-24, table in
        // TESTING.md "Event bosses"): what a human client clicks to start an encounter. Gossip optionPrefs are
        // GossipListIds: the DB OptionID for a DB menu, 0, 1, 2 ... in AddGossipItemFor order for a C++ menu.
        // Fields after note: allMembers, itemId, afterDead, escortEntry, afterUsed, distinct, skipContent,
        // strictOptions.
        std::vector<EventStarterDef> const starters = {
            // DB menu 9997: option 0 "Activate the crystals..." -> 9998 option 0 (SmartAI 30658: ACTION_START_INSTANCE)
            // only while DATA_ENCOUNTER_STATUS is NOT_STARTED; option 1 is the late-join teleport, shown only
            // IN_PROGRESS (conditions 15/9997). Sweep 2026-10-24 21:50: the VH strategy had started the event, the
            // harness fell back to option 1 and teleported the leader four times. Strict: never option 1.
            {608, StarterKind::Gossip, {30658}, {0, 0}, true, 1, 60000,
             "Violet Hold: Lieutenant Sinclari 9997 -> 9998", false, 0, {}, 0, {}, false, nullptr, true},
            // C++ menu: list id 0 "I am ready." each stage (the joust needs mounts: ToC dungeon strategy)
            {650, StarterKind::Gossip, {35004, 35005}, {0}, true, 4, 90000,
             "Trial of the Champion herald (progress 0 / 6 / 8)"},
            // C++ menu: 0 "Can you remove the sword?" (quest 24710 / 24712 complete), 1 the skip (quest 24500)
            {668, StarterKind::Gossip, {37221, 37223}, {0}, true, 1, 60000,
             "Halls of Reflection: Jaina / Sylvanas start"},
            // The escape (Jaina / Sylvanas part 2) only once the Frostsworn General 36723 is dead: before that its
            // invisible part-2 NPC anchored the event chain guard and filtered the General out at 196 yd (hor-n
            // build 10: the escape credit never came, the group stood next to the hidden NPC).
            {668, StarterKind::Gossip, {36955, 37554}, {0}, false, 2, 120000, "Halls of Reflection: escape start",
             false, 0, {36723}},
            // Priority: each object is selectable only once its own boss is dead (or, Ahn'kahet, right after Nadox) -
            // the group goes for it then instead of after a full clear (ok-n wave 6: 16 min between Nadox and
            // Taldaram).
            {576, StarterKind::UseGameObject, {188526}, {}, true, 1, 30000, "Nexus: Telestra's Containment Sphere"},
            {576, StarterKind::UseGameObject, {188527}, {}, true, 1, 30000, "Nexus: Anomalus' Containment Sphere"},
            {576, StarterKind::UseGameObject, {188528}, {}, true, 1, 30000, "Nexus: Ormorok's Containment Sphere"},
            {619, StarterKind::UseGameObject, {193094}, {}, true, 1, 30000,
             "Ahn'kahet: Ancient Nerubian Device (east) - Taldaram's sphere", false, 0, {29309}},
            {619, StarterKind::UseGameObject, {193093}, {}, true, 1, 30000,
             "Ahn'kahet: Ancient Nerubian Device (west) - Taldaram's sphere", false, 0, {29309}},
            {604, StarterKind::UseGameObject, {192518}, {}, true, 1, 30000, "Gundrak: altar of Slad'ran"},
            {604, StarterKind::UseGameObject, {192520}, {}, true, 1, 30000, "Gundrak: altar of the Colossus"},
            {604, StarterKind::UseGameObject, {192519}, {}, true, 1, 30000, "Gundrak: altar of Moorabi"},
            // [hleft-occ-portal-uses] no use limit (a group may come back through the portal more than twice)
            {578, StarterKind::UseGameObject, {189985}, {}, false, 255, 60000, "Oculus: Nexus Portal to Drakos"},
            // Four DB menus at four points of the run (OptionIDs 37476, 36142, 36412, 36236; brann_bronzebeard.cpp)
            {599, StarterKind::Gossip, {28070}, {37476, 36142, 36412, 36236}, false, 5, 60000,
             "Halls of Stone: Brann 9669 -> 9670 -> 10206 -> 10012 (Sjonnir's door)"},
            {575, StarterKind::UseGameObject, {188593}, {}, false, 1, 30000,
             "Utgarde Pinnacle: Gortok Stasis Generator"},
            // Once: one walk through 9586 -> 9594 -> 9595 shows the crates and casts the Arcane Disruptor
            // (npc_cos_chromie_start). A second, priority use pulled the leader back from the crates the CoS strategy
            // dispels (wave 9: stalled at 255 s, the leader between the crates and Chromie).
            {595, StarterKind::Gossip, {26527}, {0, 0, 0}, true, 1, 60000, "Culling of Stratholme: Chromie (start)"},
            // The five Suspicious Crates 190094 (npc_crate_helper 27827 sits in each; Arcane Disruption 49590, 8 yd,
            // TARGET_UNIT_NEARBY_ENTRY, conditions: helper alive). Wave 10 and every CoS run since wave 4: the CoS
            // strategy's "cos dispel crate" answered OK every tick at Perelli's crate (1570.9,669.9) while no crate
            // went (the leader stood in the crate, 1.1 yd from the helper; most likely still mounted from the walk -
            // the OK every tick keeps "check mount state" from running), "test runner move" never ran and the route
            // skipped Risen Zombies ~970 yd away. The harness uses the Disruptor itself as the step of the chain:
            // priority, dismounted, from beside the crate, a refused cast is no use (UseStarterItem). Cooldown 12 s:
            // the item's own is 10 s.
            {595, StarterKind::UseItem, {190094}, {}, true, 5, 12000,
             "Culling of Stratholme: Arcane Disruptor on the five Suspicious Crates", false, 37888, {}, 0, {26527}},
            // [hleft-cos-starters] one use, once she is there (GossipStarterHeld: visible to the leader)
            {595, StarterKind::Gossip, {27915}, {0, 0, 0}, true, 1, 60000,
             "Culling of Stratholme: Chromie 9610 -> 9611 -> 9612 after the 5 crates (Arthas intro)"},
            {595, StarterKind::Gossip, {26499}, {0}, false, 6, 60000,
             "Culling of Stratholme: Arthas stages (city, Town Hall, Epoch, last city, Mal'Ganis)"},

            // Classic. Priority for the starters a group does as soon as it can (wave 8: non-priority starters wait for
            // "nothing hostile reachable" - RFD's gong was first used at 1722 s, ZF's cage, Uldaman's keystone never
            // before the run ended): the gong (Tuten'kash, encounter 0, next to the entrance), and the item / altar
            // steps whose afterDead already orders them.
            {129, StarterKind::UseGameObject, {148917}, {}, true, 3, 20000,
             "Razorfen Downs: Gong x3 - Tomb Fiends, Tomb Reavers, Tuten'kash (unselectable while a wave lives)"},
            // [zf-gong-priority] priority while the Mallet of Zul'Farrak 9240 is in the bags (eventItems).
            {209, StarterKind::UseGameObject, {141832}, {}, false, 1, 60000,
             "Zul'Farrak: Gong of Zul'Farrak summons Gahz'rilla (lock 99 = Attacking, no mallet needed)", false, 0, {},
             0, {}, false, nullptr, false, {7273}, -1, 0, false, 9240},
            // zulfarrak.cpp go_troll_cage::GossipHello (339-342) needs only DATA_PYRAMID == PYRAMID_NOT_STARTED; the
            // key (lock 420) is the Executioner's drop. Zum'rah is no condition (zf-h 062726: cage at 3855 s, 20 min
            // after Zum'rah, the walk was full clear meanwhile).
            {209, StarterKind::UseItem, {141070, 141071, 141072, 141073, 141074}, {}, true, 1, 30000,
             "Zul'Farrak: Executioner's Key on a Troll Cage - Bly's crew, pyramid waves, Sezz'ziz + Nekrum", false,
             8444, {7274}},
            {209, StarterKind::Gossip, {7604}, {}, true, 2, 20000,
             "Zul'Farrak: Sergeant Bly 1517 'That's it!' within 156 s - the crew turns hostile, Weegli blows the door",
             false, 0, {7275, 7796}},
            {209, StarterKind::Gossip, {7607}, {}, true, 1, 30000,
             "Zul'Farrak: Weegli 1514 'Will you blow up that door now?' (End Door to Ukorz, if not blown yet)", false,
             0, {7275, 7796, 7604}},
            // Priority: Grubbis is encounter 0 of the dungeon, and the escort comes before the trash (wave 8: never
            // used - non-priority starters wait for "nothing hostile reachable", which a 355-hostile map never is).
            // [door_hold_pass] smart_scripts 7998: Emi opens the Cave In 146086 (id 100) before wave 1 and seals it (id
            // 29) at WP11, then opens / seals 146085 (ids 101 / 56); the leader's walk to her and with her hugs
            // 146086's model (door_hold "Cave In 146086", walked 23-30 yd, in every gnomer-h run): no barrier for this
            // escort.
            {90, StarterKind::Escort, {7998}, {0}, true, 2, 120000,
             "Gnomeregan: Emi Shortfuse 1080 'I am ready to begin.' - escort, cave-in waves, Grubbis + Chomper", false,
             0, {}, 0, {}, false, nullptr, false, {}, -1, 0, false, 0, 0, 0.0f, 0, 0.0f, {146085, 146086}},
            {43, StarterKind::Escort, {3678}, {0}, false, 2, 120000,
             "Wailing Caverns: Disciple of Naralex 201 'Let the event begin!' - escort, Naralex waves, Mutanus", false,
             0, {3669, 3670, 3671, 3673}},
            {70, StarterKind::UseItem, {124371}, {}, true, 1, 30000,
             "Uldaman: Staff of Prehistoria on the Keystone - Seal of Khaz'Mul opens, Ironaya", false, 7733,
             {6910}},
            // [ulda-altar-confirm] spell_uldaman_sub_boss_agro_keepers wakes the nearest Stone Keeper after the
            // ritual's 5 s channel (GameObject.cpp SUMMONING_RITUAL); a channel broken by a move wakes nobody, and the
            // use counted (ulda-h 062726: used at 1592 s, a trash target at 1593 s, no keeper awake, stalled). Done
            // once a keeper is awake (not Stoned 10255, or fighting) or dead; up to 3 tries 60 s apart, out of combat
            // with no attackable mob within 40 yd of the altar.
            {70, StarterKind::UseGameObject, {130511}, {}, true, 3, 60000,
             "Uldaman: Altar of the Keepers - the Stone Keepers wake one after another", false, 0, {7206}, 0, {}, false,
             nullptr, false, {4857}, -1, 0, false, 0, 10255, 40.0f},
            // Shadowfang Keep: Courtyard Door 18895. Rethilgore's death; the lever 18901 opens Sorcerer Ashcrombe's
            // cell 18936; his gossip 21213 'Please unlock the courtyard door.' walks him out, his script opens the door
            // (smart_scripts 3850 / 385000). wave 7 sfk-h: 1/8 "behind a closed door (Courtyard Door 18895)".
            {33, StarterKind::UseGameObject, {18901}, {}, true, 1, 30000,
             "Shadowfang Keep: Lever at Sorcerer Ashcrombe's cell - Cell Door 18936 opens", false, 0, {3914}},
            {33, StarterKind::Gossip, {3850}, {0}, true, 1, 60000,
             "Shadowfang Keep: Sorcerer Ashcrombe 21213 'Please unlock the courtyard door.' - Courtyard Door 18895",
             false, 0, {3914}, 0, {18901}},
            // Deadmines: Iron Clad Door 16397 (lock 202) is blown open by the Defias Cannon 16398 (lock 83 = Defias
            // Gunpowder 5397, spell 6250 Fire Cannon; smart_scripts 1639800 sets the door to state 2). The gunpowder
            // crate 17155 before it (loot 2882) plays its Taskmaster ambush; the item comes from eventItems.
            {36, StarterKind::UseGameObject, {17155}, {}, true, 1, 30000,
             "Deadmines: Defias Gunpowder crate (loot 2882: Defias Gunpowder) - a Defias Taskmaster ambush", false, 0,
             {1763}},
            {36, StarterKind::UseItem, {16398}, {}, true, 1, 30000,
             "Deadmines: Defias Gunpowder on the Defias Cannon - the Iron Clad Door 16397 blown open", false, 5397,
             {1763}, 0, {17155}, false, nullptr, false, {}, -1, 16397 /* expectDoor: Iron Clad Door */},
            // Dire Maul East: the Conservatory Door 176907 (lock 0, not selectable) is opened by Ironbark the Redeemed
            // 14241 after Zevrim's death: gossip 5602 'We are ready for you to open the door.' (smart_scripts 14241).
            // wave 7 dme-h: Alzzin "behind a closed door (Conservatory Door 176907)", Ironbark never talked to.
            // [dme-ironbark-door] strictOptions: only menu item 0; opensDoor 176907 re-arms the step while it stays
            // shut.
            {429, StarterKind::Gossip, {14241}, {0}, true, 1, 60000,
             "Dire Maul East: Ironbark the Redeemed 5602 'We are ready for you to open the door.' - Conservatory Door",
             false, 0, {11490}, 0, {}, false, nullptr, true, {}, -1, 0, false, 0, 0, 0.0f, 176907},
            // [b32_brs] Only after Galgann 7291 and Grimlok 4854 (DBC order 5, 6 before Archaedas 7) and the Stone
            // Keepers 7206: ulda-h 20261025-221216-ulda-h-5F-60 used it at 1796 s right after the Keepers, the route
            // walked off to Galgann 7 s later (boss order), Archaedas woke to nobody - DATA_ARCHAEDAS stayed
            // IN_PROGRESS (instance_uldaman.cpp: the Temple Door 141869 shut while IN_PROGRESS, the altar's spell
            // refused) and after Grimlok "no targets, nextBoss Archaedas beyond sight distance" until the stall at
            // 3242 s. The passing 163739-5A-24 used it at 2048 s, after Grimlok, and fought him at once.
            {70, StarterKind::UseGameObject, {133234}, {}, true, 1, 60000,
             "Uldaman: Altar of Archaedas (summoning ritual, 1 player) - Archaedas wakes", false, 0,
             {7228, 7206, 7291, 4854}},
            // Blackfathom Deeps: the four Fires of Aku'mai 21118-21121 (button, lock 0, SmartAI gossip hello: instance
            // data 1..4 DONE + a summoned wave in combat with the zone). The last wave's death opens the Portal of
            // Aku'Mai 21117 (instance_blackfathom_deeps OnUnitDeath; lock 85: no click opens it). Priority after Kelris
            // (2026-10-25 bfd-h: no starter, the group stood 10 yd from fire 1 and full-cleared back, 6/7).
            {48, StarterKind::UseGameObject, {21118, 21119, 21120, 21121}, {}, true, 4, 15000,
             "Blackfathom Deeps: Fire of Aku'mai x4 - four summoned waves, then the Portal of Aku'Mai opens", false, 0,
             {4832}, 0, {}, true},
            // Stratholme: the Service Entrance Gate 175368 (3624.9,-3642.2; lock 879 = Key to the City 12382, or
            // Lockpicking 300; SmartAI on use: data to Magistrate Barthilas). wave 7 strat-h: "partial: next boss
            // behind a closed door (Service Entrance Gate 175368)" with most bosses never engaged. Priority (a
            // non-priority key door lost to nearer trash every time, Dire Maul North).
            {329, StarterKind::UseItem, {175368}, {}, true, 1, 60000,
             "Stratholme: Key to the City on the Service Entrance Gate", false, 12382},
            // [strat_key_gates] the other Key to the City gates (acore_world gameobject_template: lock 879 for
            // 175352 / 175353 King's Square and 175357 Gauntlet; the instance script never opens them). Without a
            // starter everything behind them was "route through a closed door" (strat-h wave 23 112011: 723 skips at
            // the Gauntlet Gate, Timmy "next boss behind a closed door (King's Square Gate 175352)", stall at 1081
            // s). Not priority: the runner promotes a starter whose door the next boss is behind
            // (starter_door_priority); 175352 / 175353 pass with their KeyDoorPassDefs.
            {329, StarterKind::UseItem, {175352}, {}, false, 1, 60000,
             "Stratholme: Key to the City on the King's Square Gate 175352", false, 12382},
            {329, StarterKind::UseItem, {175353}, {}, false, 1, 60000,
             "Stratholme: Key to the City on the King's Square Gate 175353", false, 12382, {}, 0, {175352}},
            {329, StarterKind::UseItem, {175357}, {}, false, 1, 60000,
             "Stratholme: Key to the City on the Gauntlet Gate 175357", false, 12382},
            // [event_boss_classic] The second Gauntlet Gate 175356 (3782.6,-3590.5,144.0; GO_GAUNTLET_DOOR_2, lock
            // 879 = Key to the City / Lockpicking 300, data2 0: stays open; instance_stratholme only saves its state,
            // no SmartAI) is the way from the Gauntlet into the undead side: Baroness, Nerub'enkan, Maleki, the
            // slaughter square (Ramstein), Baron. strat-h 20261025-163739: 175357 keyed at 2806 s, then every UD boss
            // "next boss behind a closed door (Gauntlet Gate 175356)" to the 90-min timeout - no starter named it.
            {329, StarterKind::UseItem, {175356}, {}, false, 1, 60000,
             "Stratholme: Key to the City on the Gauntlet Gate 175356 (undead side) [event_boss_classic]", false,
             12382, {}, 0, {175357}},
            // Sunken Temple: the six statues in their fixed order (a wrong one only casts a trap on the user).
            // Priority: once statue 1 is used the chain guard (ROUTE_CHAIN_GUARD) keeps trash to the statue ring, and a
            // non-priority statue waits for "nothing hostile" - st-n / st-h sweep 2026-10-24 21:50: statue 1 at 383 s,
            // statue 2 chosen five times and always replaced by ring trash 4 s later, 1/6 in 25 minutes, 0/8 bosses.
            {109, StarterKind::UseGameObject, {148830}, {}, true, 1, 10000, "Sunken Temple: Atal'ai Statue 1/6"},
            {109, StarterKind::UseGameObject, {148831}, {}, true, 1, 10000, "Sunken Temple: Atal'ai Statue 2/6",
             false, 0, {}, 0, {148830}},
            {109, StarterKind::UseGameObject, {148832}, {}, true, 1, 10000, "Sunken Temple: Atal'ai Statue 3/6",
             false, 0, {}, 0, {148831}},
            {109, StarterKind::UseGameObject, {148833}, {}, true, 1, 10000, "Sunken Temple: Atal'ai Statue 4/6",
             false, 0, {}, 0, {148832}},
            {109, StarterKind::UseGameObject, {148834}, {}, true, 1, 10000, "Sunken Temple: Atal'ai Statue 5/6",
             false, 0, {}, 0, {148833}},
            {109, StarterKind::UseGameObject, {148835}, {}, true, 1, 10000,
             "Sunken Temple: Atal'ai Statue 6/6 - Atal'alarion appears", false, 0, {}, 0, {148834}},
            {109, StarterKind::UseItem, {148422}, {}, false, 1, 120000,
             "Sunken Temple: Egg of Hakkar at the Evil God Spell Focus - Shade of Hakkar event", false, 10465,
             {5710}},
            {109, StarterKind::UseItem, {148418}, {}, true, 1, 5000,
             "Sunken Temple: Hakkari Blood on Eternal Flame 1/4", false, 10460, {}, 0, {148422}},
            {109, StarterKind::UseItem, {148419}, {}, true, 1, 5000,
             "Sunken Temple: Hakkari Blood on Eternal Flame 2/4", false, 10460, {}, 0, {148422}},
            {109, StarterKind::UseItem, {148420}, {}, true, 1, 5000,
             "Sunken Temple: Hakkari Blood on Eternal Flame 3/4", false, 10460, {}, 0, {148422}},
            {109, StarterKind::UseItem, {148421}, {}, true, 1, 5000,
             "Sunken Temple: Hakkari Blood on Eternal Flame 4/4 - Avatar of Hakkar", false, 10460, {}, 0, {148422}},
            // [brd_seven_hold] boss_tomb_of_seven.cpp: the dwarves reset after 15 s with no player in combat. After
            // Ambassador Flamelash 9156, priority; the eventSpots hold keeps the group in the tomb until Doom'rel.
            // [brd_seven_fast] 3 uses: TombOfSevenReset (CheckTombReset, 15 s with no dwarf in combat) respawns the
            // dead dwarves, sets all seven FACTION_FRIENDLY + immune and Doom'rel's Reset() gives the gossip back
            // (NOT_STARTED) - each reset costs one more talk; the gossip only shows after a reset.
            {230, StarterKind::Gossip, {9039}, {0}, true, 3, 60000,
             "Blackrock Depths: Doom'rel 1947 'I challenge you!' -> 1950 - The Seven", false, 0, {9156}},
            // [brd_lyceum_hold] go_shadowforge_brazier: the first brazier sets TYPE_LYCEUM IN_PROGRESS, the second
            // (while the first burns) DONE - the gates to Magmus. One torch per Flame Keeper 9956 kill (perKill).
            {230, StarterKind::UseItem, {174744}, {}, true, 1, 10000,
             "Blackrock Depths: Shadowforge Torch on the west Shadowforge Brazier (Lyceum, Magmus)", false, 11885,
             {9956}},
            {230, StarterKind::UseItem, {174745}, {}, true, 1, 10000,
             "Blackrock Depths: Shadowforge Torch on the east Shadowforge Brazier (Lyceum, Magmus)", false, 11885,
             {9956}},
            {230, StarterKind::UseGameObject, {164911}, {}, false, 3, 5000,
             "Blackrock Depths: break the three Thunderbrew Lager Kegs - Hurley Blackbreath", false, 0, {}, 0, {},
             true},
            {230, StarterKind::Gossip, {9543}, {0}, false, 1, 60000,
             "Blackrock Depths: Ribbly Screwspigot 1970 'You're good for nothing, Ribbly' - Ribbly and his crony"},
            // [brd_bar_buy] [brd_bar_turnin] Grim Guzzler: Dark Iron Ale Mugs 11325 from Plugger Spazzring 9499
            // (npc_vendor maxcount 10, restock 60 s, 6 s each), Rocknot's Ale 4295 (2 mugs, repeatable) x3 to Private
            // Rocknot 9503: npc_rocknot::sQuestReward sets TYPE_BAR SPECIAL each hand-in, at the third he breaks the
            // keg - the Bar Door 170571 and Phalanx 9502. After Hurley and Ribbly (their bar fights first). No
            // doneWhen: the dormant Phalanx (faction 35) is alive from the start and would count as "event past".
            {230, StarterKind::VendorBuy, {9499}, {}, true, 1, 30000,
             "Blackrock Depths: buy 6 Dark Iron Ale Mugs from Plugger Spazzring [brd_bar_buy]", false, 11325,
             {9537, 9543}, 0, {}, false, nullptr, false, {}, -1, 0, false, 0, 0, 0.0f, 0, 0.0f, {}, 0, 6},
            {230, StarterKind::QuestTurnIn, {9503}, {}, true, 3, 5000,
             "Blackrock Depths: Rocknot's Ale 4295 x3 to Private Rocknot - the keg, the Bar Door [brd_bar_turnin]",
             false, 11325, {}, 0, {9499}, false, nullptr, false, {}, -1, 0, false, 0, 0, 0.0f, 0, 0.0f, {}, 4295, 0},
            // After Phalanx: the bar's Dark Iron Ale Mug 165738 / Grim Guzzler Boar 165739 (type 3 chests,
            // SmartGameObjectAI) are stolen to bring Plugger; a chest opens only by its lock's spell
            // (starter_chest_open).
            // [event_boss_classic] Priority once Phalanx is dead: the three thefts are the only way Plugger 9499
            // (npcflag 130 vendor, faction 674) turns hostile (smart_scripts 165738 / 165739 On State Changed -> Set
            // Data 1 1 on 9499; 9499 id 6-8 counter 3 -> actionlist 949900: Set Data on the patrons 9545 / 9547,
            // Start attack). As a non-priority starter it lost to far trash and the tank body-pulled the vendor
            // (brd-h 20261025-163739 18:11:00 / 18:13:42, tank_view "aggressive target: npc flags"); Retarget now
            // drops a vendor / quest-giver boss out of combat, [event_spot_hold] holds between the thefts.
            {230, StarterKind::UseGameObject, {165738, 165739}, {}, true, 3, 5000,
             "Blackrock Depths: steal the Grim Guzzler's ale and boar - Plugger Spazzring", false, 0, {9502}, 0, {},
             true},
            // The Shadowforge Lock 161460 (lock 680, flags 34): its SmartAI on state change opens the Giant Doors
            // 157923, their Fake Collision 161462 and BigDoorDummyCollision02 161516 - the way from Fineous / the
            // Ring to Bael'Gar, Angerforge and beyond. Reachable once the Ring of Law opened ArenaGate03; priority
            // as the Dire Maul North key doors (a non-priority key door lost to nearer trash every time).
            {230, StarterKind::UseItem, {161460}, {}, true, 1, 60000,
             "Blackrock Depths: Shadowforge Key on the Shadowforge Lock - Giant Doors to Shadowforge City", false,
             11000, {10096, 9016, 9017, 9056, 9041}},
            // [brd_key_door] The Lock 161460 SmartAI opens the Giant Doors but closes the Highway doors (Bael'Gar
            // stranded): so after Bael'Gar 9016, Incendius 9017, Fineous 9056 and Stilgiss 9041 (above). Lock 680
            // (Shadowforge Key 11000 / Lockpicking 250), spawned closed: Shadowforge Gates 170559 (496.4,16.0,-70.3) /
            // 170560 (569.6,-9.4,-70.3) and the East Garrison Door 170570 (560.5,-51.8,-60.1). One starter per door
            // (maxUses 1 over a list opens only one). Not priority: ROUTE_DOOR_STARTER_FIRST promotes them while the
            // next boss is behind a closed door. afterDead Roccor 9025 = "after the Detention Block" (to confirm).
            {230, StarterKind::UseItem, {170560}, {}, false, 1, 60000,
             "Blackrock Depths: Shadowforge Key on the Shadowforge Gate (east) [brd_key_door]", false, 11000, {9025}},
            {230, StarterKind::UseItem, {170559}, {}, false, 1, 60000,
             "Blackrock Depths: Shadowforge Key on the Shadowforge Gate (west) [brd_key_door]", false, 11000, {9025}},
            {230, StarterKind::UseItem, {170570}, {}, false, 1, 60000,
             "Blackrock Depths: Shadowforge Key on the East Garrison Door [brd_key_door]", false, 11000, {10096}},
            // Priority (2026-10-24 brs-h: never chosen in 90 minutes, LBRS always had trash); the cast needs the
            // Roughshod Pike 12533 as reagent (eventItems).
            // The pile (-14.4,-395.8,48.5) is reached from Omokk only over Voone's z -18.9 floor, Smolderweb's room and
            // the west passage (navmesh_probe: ~1100 yd for 95 yd straight) - DBC order Urok 4 after Omokk 0,
            // Vosh'gajin 1, Voone 2, Smolderweb 3. Spell 16447 needs focus 743 within 3 yd (STARTER_RANGE 4: the
            // UseItem path steps in, a refused cast is retried).
            {229, StarterKind::UseItem, {175621}, {}, true, 1, 120000,
             "Lower Blackrock Spire: Omokk's Head at Urok's Tribute Pile - Challenge to Urok, Urok Doomhowl", false,
             12534, {9196, 9236, 9237, 10596}, 0, {}, false, "ubrs-n", false, {}, -1, 0, false, 0, 0,
             // [lbrs_urok_event] the ogres (smart_scripts 10601 / 10602 id 1) fail the event after 30 s out of combat
             // and the head is consumed (charges -1): used only with nothing attackable within 40 yd of the pile, out
             // of combat, and the whole group within 20 yd of it (starter_wait_group), as a group gets ready first.
             40.0f, 0, 20.0f},
            {229, StarterKind::UseGameObject, {175706}, {}, false, 2, 120000,
             "Upper Blackrock Spire: Blackrock Altar (ritual, 1 player) - Pyroguard Emberseer", false, 0, {}, 0, {},
             false, "lbrs-n", false, {}, 15 /* DATA_DRAGONSPIRE_ROOM: the Emberseer In door 175244 open */},
            {229, StarterKind::UseGameObject, {175245}, {}, true, 1, 120000,
             "Upper Blackrock Spire: Father Flame - rookery waves, Solakar Flamewreath", false, 0, {9816}, 0, {},
             false, "lbrs-n", false, {10264}},
            // [starter_wait_group] Priority once Jandice is dead (the Blood of Innocents is in the bags: scholo-n
            // 2026-10-25 02:41, never used in the 1290 s after). The gate 175570 (288.4,93.0) closes on the use
            // (instance_scholomance SetData IN_PROGRESS): the whole group within 20 yd of the brazier first.
            {289, StarterKind::UseItem, {175564}, {}, true, 1, 120000,
             "Scholomance: Blood of Innocents on the Brazier of the Herald - Kirtonos the Herald", false, 13523,
             {10503}, 0, {}, false, nullptr, false, {}, -1, 0, false, 0, 0, 0.0f, 0, 20.0f},
            {289, StarterKind::UseItem, {175167}, {}, false, 1, 60000,
             "Scholomance: Viewing Room Key on the Viewing Room Door - Vectus, Marduk Blackpool", false, 13873,
             {11622}},
            // [b32_brs] Dawn's Gambit 12368: spell 17045 needs the spell focus 823 (GO 176111 "Scholomance Viewing
            // Room Spell Focus (DND)", 133.7,91.7,101.7, data1 35 yd), summons GO 176110 whose SmartAI turns the 32
            // Scholomance Students 10475 into skeletons over 16.5 s (data 2 3 / 2 4: spell 18115, Set Faction 21) and
            // has Vectus say "What is this?! How dare you!". Vectus / Marduk stay faction 15 (neutral) with npcflag 1:
            // the tank attacks them after it (ClassicDungeonScholomanceStrategy "scholo viewing room boss").
            // scholo-h 20261025-221216-5A-3: no Gambit, both "not pulled" at 2876 s (tank_view "npc flags"), skipped.
            // Priority once the Viewing Room Door was keyed; the whole group within 20 yd of the focus first.
            {289, StarterKind::UseItem, {176111}, {}, true, 1, 60000,
             "Scholomance: Dawn's Gambit at the Viewing Room spell focus - the students, Vectus, Marduk Blackpool "
             "[b32_brs]",
             false, 12368, {11622}, 0, {175167}, false, nullptr, false, {}, -1, 0, false, 0, 0, 0.0f, 0, 20.0f},
            // Priority once the key is in the bags (Dire Maul North 2026-10-25: as a non-priority starter the
            // Courtyard Door was chosen 9 times and always lost to nearer trash, the four bosses behind it stayed
            // "behind a closed door" 25 minutes with the key in the tank's bags; the door's lock 1563 takes only the
            // key 18266, a click without it does not open it in a client).
            {429, StarterKind::UseItem, {177219}, {}, true, 1, 60000,
             "Dire Maul North: Gordok Courtyard Key (Fengus's Chest) on the Courtyard Door", false, 18266, {14321}},
            {429, StarterKind::UseItem, {177217}, {}, true, 1, 60000,
             "Dire Maul North: Gordok Inner Door Key (Mol'dar) on the Inner Door - King Gordok's court", false, 18268,
             {14326}},
            // [dmw-crescent-key] dmw-n 062726: 464 "route through a closed door (Door 177221)" skips of the pylon
            // guards of 179504 / 179505 (y 737) and Prince Tortheldrin "behind a closed door (Door 177221)" - the only
            // walk there (navmesh poly 53.3,499.7,-23.2). Priority after Magister Kalendris (the boss before
            // Immol'thar).
            {429, StarterKind::UseItem, {177221}, {}, true, 1, 60000,
             "Dire Maul West: Crescent Key on the Door 177221 - the pylons by Immol'thar, Prince Tortheldrin", false,
             18249, {11487}},
            // [dmw_tortheldrin_door] Door 179550 (83.3,630.5,-24.6, spawned closed) in front of Prince Tortheldrin:
            // no instance script or SmartAI opens it, its lock 1562 (Lock.dbc) takes the Crescent Key 18249 (or
            // Lockpicking 300), as 177221's. dmw-n 20261025-123432: Tortheldrin "next boss behind a closed door (Door
            // 179550)" at 2205 / 2267 s, the run ended 3/5. Priority after Immol'thar 11496 (the boss before him).
            {429, StarterKind::UseItem, {179550}, {}, true, 1, 60000,
             "Dire Maul West: Crescent Key on the Door 179550 - Prince Tortheldrin [dmw_tortheldrin_door]", false,
             18249, {11496}},
            // 2026-10-25: the Chapel Door 104591 (lock 299: The Scarlet Key 7146 or Lockpicking 175, flags 34
            // LOCKED) is the cathedral's entrance; instance_scarlet_monastery only stores its GUID, no script opens
            // it. The key's spell 3366 (Opening) on the door, as the Viewing Room Key.
            // [sm-chapel-wing] priority, but only in the Cathedral wing (sm-h 062726: chosen from the Graveyard at
            // 41 s).
            {189, StarterKind::UseItem, {104591}, {}, true, 1, 60000,
             "Scarlet Monastery Cathedral: The Scarlet Key on the Chapel Door - Mograine, Whitemane", false, 7146, {},
             0, {}, false, nullptr, false, {}, -1, 0, true},
            // [b32_sm_door] High Inquisitor Fairbanks 4542 (1158.9,1354.3) sits behind the Secret Door 97700
            // (1167.8,1347.3, lock 85, no script opens it but this): the Torch 97701 beside it (1169.0,1345.6,33.0,
            // GAMEOBJECT_TYPE_BUTTON, lock 93) - smart_scripts 97701 id 0: On Gameobject State Changed (loot state 2,
            // GO_ACTIVATED: the button's use) -> Activate Gameobject 97700. The harness had no starter for it: sm-h
            // 20261025-221216-sm-h-5E-14 "High Inquisitor Fairbanks - skipped: next boss behind a closed door (Secret
            // Door 97700)", 0 of 30 sm-h / sm-cath runs ever killed him. After the Chapel Door (the cathedral wing);
            // expectDoor 97700: a click that did not open it is undone and tried again.
            {189, StarterKind::UseGameObject, {97701}, {}, true, 1, 30000,
             "Scarlet Monastery Cathedral: the Torch 97701 opens the Secret Door 97700 - Fairbanks [b32_sm_door]",
             false, 0, {}, 0, {104591}, false, nullptr, false, {}, -1, 97700, true},
            // [b30_wings] Herod's Door 101854 (1933.7,-431.7,18.7, spawned GO_STATE_READY; template lock 299:
            // The Scarlet Key 7146 or Lockpicking 175, as the Chapel Door) closes the Armory's last hall; no instance
            // script or SmartAI opens it. sm-h 20261025-184229-sm-h-5B-18 and build 29's -163739-sm-h-5A-19: Herod
            // "next boss behind a closed door (Herod's Door 101854)" for 18 minutes, then wing_change "no targets"
            // out to the Library and the stop at 4/7. Only in the Armory wing; counted once the door opened.
            {189, StarterKind::UseItem, {101854}, {}, true, 1, 60000,
             "Scarlet Monastery Armory: The Scarlet Key on Herod's Door - Herod [b30_wings]", false, 7146, {}, 0, {},
             false, nullptr, false, {}, -1, 101854, true},

            // The Burning Crusade
            // go_broggok_lever (boss_broggok.cpp): ACTION_PREPARE_BROGGOK, the four cells open one after another
            // (instance_blood_furnace.cpp ActivateCell), then the rear door and Broggok. Priority once The Maker is
            // dead: as a non-priority starter it was never chosen (wave 34 and every bf-n run of 2026-10-24: 0 lever
            // targets) - there was always trash, the group walked through the cell room past the lever (456.6,54.4,
            // 9.6, reachable) on to Keli'dan and stalled "Broggok - skipped: next boss unreachable".
            // [b30_stalls] clearRadius 60 / gatherRadius 30: bf-n 20261025-184229-bf-n-5G-44 pulled the lever at 838 s
            // with the 3-Technician pack 138217-138219 (480-484, 92-103; 46-55 yd from the lever, next to cell 3) idle
            // - the tank held it ("Rlpalbb rests below its line"), so no pull held the waypoint - and the healer 44 yd
            // back; the pack joined wave 2-3, two deaths, Broggok wiped the rest at 4.8 %. The Legionnaires at the
            // front door (62-66 yd) stay out; the room behind the rear door is no blocker (closed-door filter).
            {542, StarterKind::UseGameObject, {181982}, {}, true, 1, 60000,
             "Blood Furnace: Cell Door Lever - four prisoner cells, then Broggok", false, 0, {17381}, 0, {}, false,
             nullptr, false, {}, -1, 0, false, 0, 0, 60.0f, 0, 30.0f},
            {560, StarterKind::UseItem, {182589}, {}, true, 5, 3000,
             "Old Hillsbrad: Pack of Incendiary Bombs on five different barrels - Lieutenant Drake", false, 25853, {},
             0, {}, false, nullptr, false, {17848}},
            {560, StarterKind::Escort, {17876}, {0, 1}, true, 8, 30000,
             "Old Hillsbrad: Thrall 7499 / 7830 / 7840 / 7853 - escort, Skarloc, Tarren Mill, the inn, Epoch Hunter",
             false, 0, {}, 0, {182589}},
            {545, StarterKind::UseGameObject, {184125}, {}, false, 1, 30000,
             "Steamvault: Main Chambers Access Panel by Thespia", false, 0, {17797}},
            {545, StarterKind::UseGameObject, {184126}, {}, false, 1, 30000,
             "Steamvault: Main Chambers Access Panel by Steamrigger - door to Kalithresh", false, 0, {17796}},
            {557, StarterKind::UseItem, {185522}, {}, false, 1, 60000,
             "Mana-Tombs heroic: Shaffar's Stasis Chamber Key on the Stasis Chamber - Yor", false, 32079},

            // Raids (TBC raid agent notes)
            {534, StarterKind::Gossip, {17772}, {0}, true, 8, 120000, "Hyjal: Jaina starts the Alliance wave sets"},
            {534, StarterKind::Gossip, {17852}, {0}, false, 8, 120000, "Hyjal: Thrall starts the Horde wave sets"},
            {532, StarterKind::Gossip, {16812}, {0, 0, 0}, false, 1, 60000, "Karazhan: Barnes starts the Opera"},
            {564, StarterKind::Gossip, {23191}, {0}, false, 1, 60000, "Black Temple: Akama starts Shade of Akama"},
            {564, StarterKind::Gossip, {23089}, {0, 0}, false, 1, 60000,
             "Black Temple: Akama starts the Illidan intro"},
            {568, StarterKind::Gossip, {24358}, {0}, true, 1, 60000, "Zul'Aman: Harrison Jones"},
            {568, StarterKind::UseGameObject, {187359}, {}, true, 6, 15000,
             "Zul'Aman: Strange Gong (every member)", true},
        };

        std::vector<KnownGapDef> const knownGaps = {
            {532, "Karazhan chess event (Echo of Medivh 16816): pieces must be controlled - needs honest tactic"},
            {534, "Hyjal Archimonde: Tears of the Goddess item use - needs honest tactic"},
            // The Argent Lance 46106 does have a source: the Lance Rack (GO 196398, type 22 spellcaster, 4 spawns in
            // map 650) casts 64682, which creates it; Ai/Dungeon/TOC "toc lance" takes it. The jousting itself
            // (mounting the vehicles, the mounted fight) is the ToC / vehicle strategy's part, not the harness'.
            {650, "Trial of the Champion: the Grand Champions are jousted - Argent Lance 46106 from the Lance Rack "
                  "(GO 196398 casts 64682, 'toc lance'), mounted fight by the ToC / vehicle strategy"},
            {578, "Oculus: Varos / Urom / Eregos are fought from drakes (essence gossip, mount, flight, landing: "
                  "Ai/Dungeon/OC); after Drakos the harness pauses targeting while a member is on a drake"},
            {568, "Zul'Aman: hostage gossips after each boss are optional (timed chest)"},
        };

        // DungeonEncounter.dbc kill credits whose creature the instance script swaps for the other team's NPC: the
        // kill credits the live entry (KillRewarder: _victim->GetEntry()), the dbc credit never fires.
        struct EncounterCreditAliasDef
        {
            uint32 mapId;
            uint32 creditEntry;
            uint32 aliasEntry;
            char const* note;
        };

        // DungeonEncounter.dbc encounters credited by no kill: Map::UpdateEncounterState finds no encounter list for
        // a classic map's difficulty 1 (heroic here) and never sets the completed mask; the script's data says it.
        struct EncounterDoneDataDef
        {
            uint32 mapId;
            uint32 creditEntry;
            uint32 dataId;  // InstanceScript::GetData(dataId) == DONE
        };

        std::vector<EncounterDoneDataDef> const encounterDoneData = {
            // blackrock_depths.cpp npc_grimstone waypoint 5: credit 10096, SetData(TYPE_RING_OF_LAW 1, DONE)
            {230, 10096, 1},
            // Dire Maul North: Cho'Rush the Observer 14324 gives up on King Gordok's death (11501 SmartAI 5-6:
            // SetData on him -> faction 35 + evade, instance data TYPE_NORTH_WING_PROGRESS 3 = DONE); never killed
            // after.
            {429, 14324, 3},
        };

        std::vector<EncounterCreditAliasDef> const encounterCreditAliases = {
            // instance_nexus.cpp OnCreatureCreate: Commander Stoutbeard 26796 (spawn 4764, heroic only) becomes
            // Commander Kolurg 26798 for an Alliance group; instance_encounters 519 "Frozen Commander" names 26796
            // (2026-10-25 nex-h: killed as "boss 4", the dbc track stayed "not spawned").
            {576, 26796, 26798, "The Nexus: Frozen Commander (Stoutbeard 26796 / Kolurg 26798)"},
            // Stratholme: Grand Crusader Dathrohan 10812 (spawn 52149) becomes Balnazzar 10813 at 40 % (smart_scripts
            // 10812 id 5 Balnazzar Transform 17288, id 7 Update Entry 10813; id 17 back to 10812 out of combat);
            // instance_encounters 478 credits 10813 (2026-10-25 strat-h: "Balnazzar - not spawned (event not
            // started)").
            {329, 10813, 10812, "Stratholme: Balnazzar (Grand Crusader Dathrohan 10812 / Balnazzar 10813)"},
        };

        // A creature whose death brings the encounter's credit creature (EncounterCarrierCredit).
        struct EncounterCarrierDef
        {
            uint32 mapId;
            uint32 carrierEntry;
            uint32 creditEntry;
            char const* note;
        };

        std::vector<EncounterCarrierDef> const encounterCarriers = {
            // smart_scripts 642 id 2: On Just Died - Cast Eject Sneed 5141 (Spell.dbc: Effect 28 SUMMON 643); Sneed 643
            // has no spawn. 2026-10-25 dm-h: "Sneed's Shredder (642): not in the encounter data", never the next boss.
            {36, 642, 643, "The Deadmines: Sneed's Shredder ejects Sneed"},
        };

        // Stratholme's auto-closing key gates (lock 879 = Key to the City 12382, Opening 3366: 5 s cast, 5 yd, broken
        // by moving; data2 3000: UseDoorOrButton shuts them 3 s after the use). Use point 3 yd before the gate on the
        // side the group comes from (the line of sight to the gate's centre fails farther out: strat-n/-h 2026-10-25
        // starter_item_failed los:false at 6.7-9.1 yd, the use from 3.0 yd worked), far point 6 yd past it. From the
        // spawn (gameobject 6851 / 47707 / 47985) and its facing f = (cos o, sin o).
        std::vector<KeyDoorPassDef> const keyDoorPasses = {
            {329, 175368, 3622.21f, -3643.48f, 138.5f, 3630.30f, -3639.53f, 138.5f,
             "Stratholme: Service Entrance Gate (from the instance entrance side)"},
            {329, 175352, 3573.60f, -3453.89f, 136.4f, 3567.52f, -3447.26f, 136.4f,
             "Stratholme: King's Square Gate 175352 (from the south-east)"},
            {329, 175353, 3557.86f, -3437.10f, 136.4f, 3551.78f, -3430.47f, 136.4f,
             "Stratholme: King's Square Gate 175353 (behind 175352, same street)"},
        };

        // Reference only (user rule: every boss must die, "done" needs all of them): DungeonEncounter.dbc bosses that
        // appear only through an event, a summoning item or as a rare spawn (kill-credit entries from
        // instance_encounters, no spawn in acore_world.creature unless noted). Used to name the reason in
        // bossesMissing and for the event starters / tactics still to write.
        struct EncounterRequirementDef
        {
            uint32 mapId;
            uint32 entry;
            char const* kind;  // "event not started", "summon item missing", "rare not spawned", PHASED_UNTIL_BOSS
            char const* note;
            // "summon item missing": the item; a member carrying it makes the reason "event not started (summon item
            // in bags)" (run_end said "summon item missing" with the Blood of Innocents / Omokk's Head in the bags).
            uint32 itemId = 0;
        };

        std::vector<EncounterRequirementDef> const encounterRequirements = {
            {43, 3654, "event not started", "Wailing Caverns: Mutanus - Disciple of Naralex escort"},
            {70, 7228, "event not started", "Uldaman: Ironaya - Staff of Prehistoria on the Keystone"},
            {70, 2748, "event not started", "Uldaman: Archaedas - Altar of Archaedas"},
            {109, 8580, "event not started", "Sunken Temple: Atal'alarion - six statues in order (phase 2 until)"},
            // [st-phased-until-boss] smart_scripts 5721 / 5720 id 2 (On Update - Set Ingame Phase Mask 1, conditions
            // 22/3 GetData(1) == DONE: Jammal'an's On Death id 11); instance_sunken_temple.cpp: Eranikus
            // NOT_SELECTABLE until OnUnitDeath(Jammal'an).
            {109, 5721, PHASED_UNTIL_BOSS, "Sunken Temple: Dreamscythe - phase 2 until Jammal'an the Prophet dies"},
            {109, 5720, PHASED_UNTIL_BOSS, "Sunken Temple: Weaver - phase 2 until Jammal'an the Prophet dies"},
            {109, 5709, PHASED_UNTIL_BOSS,
             "Sunken Temple: Shade of Eranikus - not selectable until Jammal'an the Prophet dies"},
            {209, 7271, "event not started", "Zul'Farrak: Witch Doctor Zum'rah - areatrigger 962 makes him hostile"},
            {230, 9938, "event not started", "Blackrock Depths: Magmus - two Shadowforge Braziers (Lyceum)"},
            {230, 9537, "event not started", "Blackrock Depths: Hurley Blackbreath - three Thunderbrew kegs"},
            {230, 9543, "event not started", "Blackrock Depths: Ribbly Screwspigot - gossip"},
            {230, 9035, "event not started", "Blackrock Depths: The Seven - Doom'rel gossip"},
            {229, 9816, "event not started", "Upper Blackrock Spire: Pyroguard Emberseer - Blackrock Altar"},
            {229, 10429, "event not started", "Upper Blackrock Spire: Rend - stadium areatrigger 2026"},
            {269, 17879, "event not started", "Black Morass: Chrono Lord Deja - rift 6 (stand at Medivh)"},
            {269, 17880, "event not started", "Black Morass: Temporus - rift 12"},
            {269, 17881, "event not started", "Black Morass: Aeonus - rift 18"},
            {329, 10439, "event not started", "Stratholme: Ramstein - 3 ziggurats, then the slaughter square"},
            {36, 643, "event not started",
             "Deadmines: Sneed - ejected when Sneed's Shredder 642 dies (smart_scripts 642 id 2, Eject Sneed 5141)"},
            {543, 17537, "event not started", "Hellfire Ramparts: Vazruden - the two Hellfire Sentries"},
            {560, 17848, "event not started", "Old Hillsbrad: Lieutenant Drake - five barrels"},
            {560, 17862, "event not started", "Old Hillsbrad: Captain Skarloc - Thrall escort"},
            {560, 18096, "event not started", "Old Hillsbrad: Epoch Hunter - Thrall escort at the inn"},
            {658, 36658, "event not started", "Pit of Saron: Tyrannus - the tunnel (areatriggers 5578-5633)"},
            {90, 7361, "event not started", "Gnomeregan: Grubbis - Emi Shortfuse escort"},
            {109, 8443, "event not started", "Sunken Temple: Avatar of Hakkar - Egg of Hakkar event"},
            {129, 7355, "event not started", "Razorfen Downs: Tuten'kash - gong"},
            {209, 7273, "event not started", "Zul'Farrak: Gahz'rilla - Gong of Zul'Farrak (no mallet needed)"},
            {209, 7796, "event not started", "Zul'Farrak: Nekrum Gutchewer - Executioner's Key cage, pyramid waves"},
            {209, 7275, "event not started", "Zul'Farrak: Shadowpriest Sezz'ziz - Executioner's Key cage, waves"},
            {229, 10584, "summon item missing",
             "Lower Blackrock Spire: Urok Doomhowl - Omokk's Head + Roughshod Pike at Urok's Tribute Pile", 12534},
            {229, 10268, "event not started",
             "Lower Blackrock Spire: Gizrul the Slavener - summoned at Halycon's death (boss_halycon.cpp:60), despawns "
             "5 min out of combat"},
            {229, 10264, "event not started", "Upper Blackrock Spire: Solakar Flamewreath - Father Flame"},
            {289, 10506, "summon item missing", "Scholomance: Kirtonos - Blood of Innocents (Jandice Barov)", 13523},
            {289, 1853, "event not started",
             "Scholomance: Darkmaster Gandling - invisible, faction 35 until the six study bosses die "
             "(DATA_MINI_BOSSES 6: Illucia 10502, Alexei 10504, Malicia 10505, Ravenian 10507, Polkelt 10901, "
             "Krastinov 11261)"},
            {329, 10558, "event not started", "Stratholme: Hearthsinger Forresten - neutral spawn (faction 7)"},
            {556, 23035, "class gated", "Sethekk Halls heroic: Anzu - druid-only quest item (classGatedBosses)"},
            {557, 22930, "summon item missing", "Mana-Tombs heroic: Yor - Shaffar's Stasis Chamber Key on the chamber",
             32079},
        };

        // Proximity / areatrigger events the group must walk to (EventSpotDef). Areatriggers on the way fire through
        // EmulateAreaTriggers (areatrigger_scripts of the map) once a member stands in them.
        std::vector<EventSpotDef> const eventSpots = {
            // Zul'Farrak: Witch Doctor Zum'rah 7271 stands faction 35 at his graveyard until areatrigger 962
            // (SmartTrigger 1909.27,1015.11,11.52, radius 10; smart_scripts 962: set faction 37 on guid 81524).
            // zf-h 2026-10-25:
            // nobody came within 82 yd of it, 25 min of full clear, "alive, not engaged". A step, done once reached
            // (EmulateAreaTriggers fires it); it leads the route while he is no valid target (SpotBossIsNext).
            // Zul'Farrak pyramid (instance_zulfarrak.cpp Update): after the cages Bly's crew stands at the stair top;
            // waves 1-2 walk up from the base, wave 3 Sezz'ziz + Nekrum stay at the base and the crew walks down to
            // them. Behind the crew only what comes up is fought; bosses are targets anywhere. After the Executioner
            // (the cage needs only his key); before Zum'rah's step - EventSpotIndex takes the first spot that applies.
            {209, 1887.0f, 1268.0f, 41.6f, {7274}, {7275, 7796},
             "Zul'Farrak: pyramid stair top - Bly's crew holds the waves", true, 25.0f, -100000.0f, true},
            {209, 1909.27f, 1015.11f, 11.52f, {}, {7271}, "Zul'Farrak: Zum'rah's graveyard (areatrigger 962)"},
            // boss_vazruden_the_herald.cpp:69-112: the two Hellfire Sentries (summoned) start it on aggro
            {543, -1378.0f, 1718.0f, 83.0f, {17306, 17308}, {17537, 17536},
             "Hellfire Ramparts: Vazruden's platform - the Hellfire Sentries, then Vazruden and Nazan"},
            // arcatraz.cpp npc_warden_mellichar::DamageTaken: the first hit by a player on Warden Mellichar 20904
            // (445.8,-169.0,43.6, behind his shield in the Containment Core) starts the five pod waves and Harbinger
            // Skyriss 20912 (summoned at 445.8,-191.6; instance_encounters 500 / 501 credit 20912). The security
            // fields 184318 / 184319 open with Soccothrates / Dalliah. Wave 21 arc-n: with Skyriss unspawned no boss
            // led the route, Mellichar was plain "full clear" trash - the group stood 50 yd from him, then took a
            // 942 yd detour for a Death Watcher and walked 357 yd away again, 131 s without a fight. A walk-in spot
            // (ROUTE_EVENT_SPOT_FIRST): the group walks into the core (floor z 43.1, ~24 yd short of him); the
            // tank's own pull there is Mellichar, the event a player starts.
            {552, 446.0f, -145.0f, 43.1f, {20885, 20886}, {20912},
             "The Arcatraz: the Containment Core - a hit on Warden Mellichar starts the waves, Harbinger Skyriss"},
            // the_black_morass.cpp:117-139: a player within 20 yd of Medivh starts the portals
            // Wave 10/11: no rift keeper was ever a target (outside the 90 yd hold radius), no rift closed, the shield
            // fell and Medivh died ~250 s after the start; the keepers (the_black_morass.h) are targets wherever.
            // Rifts 6 / 12 / 18 (npc_time_rift EVENT_SUMMON_BOSS) summon the boss itself as the rift's keeper, at the
            // rift (Chrono Lord Deja 17879 / Temporus 17880, heroic repeats 21697 / 21698, Aeonus 17881): wave 34
            // (12:22) Deja came at 12:32 (boss_state 5 -> 0) on a rift 90+ yd from Medivh, was never a target (a
            // summoned boss is no party boss creature before it is fought: the hold radius dropped it), its rift
            // stayed open and fed 62 Infinite Whelps, no further boss rift in 15 minutes.
            {269, -2021.0f, 7121.5f, 22.7f, {}, {17881},
             "Black Morass: Medivh - the rifts start, the group holds here between rifts", true, 0.0f, -100000.0f,
             false, {21104, 21148, 17839, 21140, 17879, 17880, 17881, 21697, 21698}, 100000.0f,
             {17835, 21137, 21818, 17892, 21136, 18994, 21138, 18995, 21139}, 25.0f},
            // instance_violet_hold.cpp: after the Sinclari start the portal waves channel on the Prison Door Seal
            // (30896 at 1823.7,803.9); at 0 seal health InstanceCleanup resets the event. Defenders hold inside the
            // door between the seal and the guards' line until Cyanigosa is dead (wave 8: the group wandered after
            // the prisoners, the event reset, run ended "no reachable targets").
            // Wave 10: the hold radius was 90 yd, but two of the six PortalLocations (violet_hold.h: 1936.1,803.2 and
            // 1927.6,758.4) are 94-96 yd from here and their Portal Keeper / Guardian channels 2 yd from the portal:
            // never a target, the portal never closed (npc_vh_teleportation_portal kills itself only when its keeper
            // is dead), its trash came every 20 s - 58 kills, no boss in 25 minutes. 110 yd covers all six.
            {608, 1843.0f, 804.0f, 44.0f, {}, {31134}, "Violet Hold: inside the prison door - hold between portals",
             true, 110.0f},
            // Pit of Saron tunnel: areatriggers 5578 / 5579 / 5580 (SmartTrigger) and 5633 (at_tyrannus_event_starter)
            // [b32_pos] smart_scripts 5578/5579/5580 -> SetData(1, 1/2/3) on npc_pos_tyrannus_events 36794
            // (pit_of_saron.cpp): 1 needs DATA_INSTANCE_PROGRESS (4) FINISHED_KRICK_SCENE (2) -> AFTER_WARN_1 (3) and
            // summons 10 Ymirjar; 2 needs AFTER_WARN_1 and those 10 dead (On death SetData(2, 1) to the summoner) ->
            // AFTER_WARN_2 (4), summons 6 / 12 Fallen Warrior / Wrathbone Coldwraith; 3 needs AFTER_WARN_2 and those
            // dead -> AFTER_TUNNEL_WARN (5), the icicles start; at_tyrannus_event_starter (5633) refuses below 5. A fire
            // with the condition unmet does nothing and nothing retries it: 2026-10-25 pos-n-5D-58 / pos-h-5D-47 (bear
            // tank leading) left the 5578 spot 8 s after the fire, 5579 fired with the Ymirjar alive, wave 2 never
            // came, 5633 refused for 4 minutes. Each step now holds until its progress value (progressData), and its
            // trigger re-fires while it holds; holdRadius covers the summons' end points (wave 1 from 5579's spot
            // <= 92 yd, wave 2 heroic 937.8,21.2 from 5580's spot 143 yd).
            {658, 859.3f, 44.4f, 515.0f, {36494, 36476}, {36658}, "Pit of Saron: tunnel ambush (areatrigger 5578)",
             false, 110.0f, -100000.0f, false, {}, 100000.0f, {}, 0.0f, -1, {}, false, false,
             4 /* DATA_INSTANCE_PROGRESS */, 3 /* AFTER_WARN_1 */, 5578},
            {658, 960.1f, 75.3f, 566.2f, {36494, 36476}, {36658},
             "Pit of Saron: second ambush (areatrigger 5579, after 10 ambushers)", false, 110.0f, -100000.0f, false, {},
             100000.0f, {}, 0.0f, -1, {}, false, false, 4 /* DATA_INSTANCE_PROGRESS */, 4 /* AFTER_WARN_2 */, 5579},
            {658, 969.9f, -117.4f, 597.9f, {36494, 36476}, {36658}, "Pit of Saron: icicle tunnel (areatrigger 5580)",
             false, 150.0f, -100000.0f, false, {}, 100000.0f, {}, 0.0f, -1, {}, false, false,
             4 /* DATA_INSTANCE_PROGRESS */, 5 /* AFTER_TUNNEL_WARN */, 5580},
            // [b33_harn] the hold point is the Tyrannus tank spot (PoSTriggers.h TYRANNUS_TANK_SPOT 1010,168,628.16,
            // [b33_pos]), not 5633's centre: its 51.5 yd radius fired with the group 26-29 yd short of the slave line,
            // and a walk back to this spot between fights must not pull the tank off the spot the fight uses.
            {658, 1010.0f, 168.0f, 628.2f, {36494, 36476}, {36658},
             "Pit of Saron: tunnel end (areatrigger 5633) - Tyrannus and Rimefang", true},
            // Azjol-Nerub after Hadronox: the group gathers at the edge of the hole in the shaft floor and the AN
            // strategy drops it to the lower kingdom ("drop to lower kingdom": the interactor jumps once the group
            // stands within 40 yd, out of combat). Wave 9: the endless Skittering Swarmers of the shaft kept the
            // harness leader chasing them for 20 minutes, the group was never together and nobody jumped. Only what
            // comes within 25 yd of the edge is fought here; below the shaft floor the spot is over. Wave 10: the
            // Skittering Swarmers 32593 (35 DB spawns at the hole's floor, 16-20 yd below the ledge the group stood on)
            // were still chosen one by one (steer_stuck "route end"), the leader never walked to the rim: at this spot
            // only what attacks is fought (fightOnlyAttackers), and the harness drive walks the leader down the steep
            // slope the bot's own move refuses.
            {601, 538.6f, 549.5f, 645.5f, {28921}, {29120},
             "Azjol-Nerub: the hole to the lower kingdom (after Hadronox) - the AN strategy drops the group", true,
             25.0f, 600.0f, true},
            // Utgarde Pinnacle: Svala's intro is areatrigger 5140 (SmartTrigger, box 37 x 14 yd at 312.6,-291.2) in her
            // hall; she spawns only then. Wave 6 the route crossed it by chance; wave 10 never came near (the leader
            // was held on a steep-joined ledge 200 yd east, run "Svala - not spawned"). With nothing else to do the
            // leader walks into it (EmulateAreaTriggers fires it, as for a client); a step, done once reached.
            // Since the event pass (TestHarnessRunner ROUTE_EVENT_SPOT_FIRST): Svala is the first boss and appears only
            // here, so the spot comes first - idle trash counts only on the way to it - and the group then waits out
            // her 72 s Arthas talk there (ROUTE_INTRO_WAIT; boss_svala.cpp, she picks a player within 100 yd).
            {575, 312.6f, -291.2f, 104.7f, {}, {26668},
             "Utgarde Pinnacle: Svala's hall (areatrigger 5140) - the intro, Svala Sorrowgrave"},
            // boss_nethekurse.cpp at_rp_nethekurse (4347, r 30 at the sewer ramp): his intro, he and the Fel Orc
            // Converts lose their immunity (until then nothing of his room is a target and the group would stand in
            // the sewer). Only below z -17 (the entrance side is z -15.7..-11.8): reached over the sewer jump
            // (RouteLinkDef), never through his closed door.
            {540, 181.4f, 220.7f, -20.1f, {}, {16807}, "The Shattered Halls: Nethekurse's intro (areatrigger 4347)",
             false, 0.0f, -100000.0f, false, {}, -17.0f},
            // blackrock_depths.cpp:142-163: at_ring_of_law, radius 8
            // untilDead 10096: High Justice Grimstone, the Ring of Law track's credit (instance_encounters 230) - the
            // spot leads the route once it applies (SpotBossIsNext), not only when nothing else is left (2026-10-25
            // brd-h: walked to 12 minutes late, every later boss "behind a closed door (ArenaGate03 161524)").
            // [brd_ring_hold] A step that holds while the event runs (eventData TYPE_RING_OF_LAW 1,
            // blackrock_depths.h): the waves (608.96,-235.32) and the boss (644.30,-175.99) attack a player within 100
            // yd; 30 s without a victim fails it (npc_grimstone updateReset). Only what attacks is fought, within 55 yd
            // (both spawn points are 48-49 yd from the trigger).
            {230, 596.4f, -188.5f, -50.0f, {}, {10096},
             "Blackrock Depths: Ring of Law (areatrigger 1526) - arena waves", false, 55.0f, -100000.0f, true, {},
             100000.0f, {}, 0.0f, 1 /* TYPE_RING_OF_LAW */},
            // [st_hakkar_hold] Sunken Temple, the Shade of Hakkar event (Egg of Hakkar 10465 at the Evil God Spell
            // Focus 148422, -467.1,276.6,-90.5): smart_scripts 8440 summon a Hakkari Bloodkeeper 8438 every 110 s at
            // (-434.5,288.3) and (-507.4,265.4); it attacks the nearest player within 40 yd (8438 id 1) and drops the
            // Hakkari Blood 10460 each Eternal Flame needs. A hold from the Focus' use until the Avatar of Hakkar 8443
            // dies: the Bloodkeepers are targets at any distance (holdAlways). st-n 2026-10-25 3819-3953 s: three
            // flames lit, the fourth waiting for blood, the route took an Atal'ai Slave 230 yd off on the z -54
            // balcony ("fallback (nothing on the route)") and walked away from the event until the stop.
            // [event_boss_classic] The Nightmare Suppressors 8497 (8440 id 21-22: two every 70 s) walk to the Shade
            // and, out of combat, add 1 to its counter 2 every 4 s while casting Suppression 12623 (8497 id 4-5); at
            // 25 the Shade despawns and the event resets (8440 id 9-10: instance data 2 NOT_STARTED, no more
            // Bloodkeepers). st-n 20261025-163739: egg at 3975 s, one blood, flame lit at 4063 s, then no Bloodkeeper
            // for 250 s - the event had failed - and the stall. The Suppressors are the hold's guards (they come before
            // the Bloodkeepers, out of combat within 45 yd) and targets at any distance.
            {109, -467.1f, 276.6f, -90.5f, {}, {8443},
             "Sunken Temple: the Shade of Hakkar event - hold for the Bloodkeepers and the four Eternal Flames "
             "[st_hakkar_hold]",
             true, 50.0f, -100000.0f, false, {8497, 8438}, 100000.0f, {8497}, 45.0f, -1, {148422}},
            // [brd_seven_hold] boss_tomb_of_seven.cpp: Doom'rel's gossip starts the dwarves one by one; they reset
            // after 15 s with no player in combat. A hold until Doom'rel dies: no member leaves before the 7th dwarf.
            {230, 1252.0f, -240.0f, -85.6f, {9156}, {9039},
             "Blackrock Depths: Tomb of the Seven - hold until Doom'rel [brd_seven_hold]", true, 35.0f, -100000.0f,
             true, {9034, 9035, 9036, 9037, 9038, 9039, 9040}},
            // [brd_lyceum_hold] go_shadowforge_brazier: braziers 174744 (1330.1,-508.9) / 174745 (1431.1,-508.9); hold
            // between them against the respawning Flame Keepers until Magmus (afterDead: the first keeper's death).
            {230, 1380.6f, -508.9f, -88.9f, {9956}, {9938},
             "Blackrock Depths: the Lyceum between the braziers - hold until Magmus [brd_lyceum_hold]", true, 40.0f,
             -100000.0f, true},
            // go_father_flame (instance_blackrock_spire.cpp): five Guardian + Hatcher waves at 78-84,-280,93, then
            // Solakar, who fails the event after 10 s without a victim (boss_solakar_flamewreath.cpp). Before the
            // stadium: EventSpotIndex takes the first spot that applies (ubrs-n 2026-10-25: the stadium walk started
            // 1 s after the Father Flame and the waves failed).
            {229, 78.0f, -312.0f, 91.5f, {9816}, {10264},
             "Upper Blackrock Spire: the Rookery - Father Flame waves, Solakar Flamewreath", true, 60.0f},
            // instance_blackrock_spire.cpp:683-712: at_blackrock_stadium, radius 20, after Emberseer (UBRS only).
            // [event_boss_waits] Rend 10429 stands NON_ATTACKABLE on his balcony behind the exit portcullis 175186
            // (doorData: opens with his DONE): the walk-in step leads (bossComesToSpot, no "behind a closed door"
            // skip).
            {229, 153.8f, -419.8f, 110.5f, {9816}, {10429},
             "Upper Blackrock Spire: stadium (areatrigger 2026) - six waves, Gyth, Rend", false, 0.0f, -100000.0f,
             false, {}, 100000.0f, {}, 0.0f, -1, {}, true},
            // boss_rend_blackhand.cpp SummonWave: six summon groups (creature_summon_groups 10429, x 201-214, behind
            // the portcullis 175185) DoZoneInCombat 100 and fail the event 30 s out of combat; then Gyth walks
            // GYTH_PATH_1 into the stadium (to 127.9,-420.5) and summons the fightable Rend at 25 %. The group holds
            // in the stadium until Rend is dead; the waves, Gyth and the summoned Rend are targets wherever they are.
            {229, 153.8f, -419.8f, 110.5f, {9816, 10264}, {10429},
             "Upper Blackrock Spire: stadium hold - waves, Gyth, Rend", true, 70.0f, -100000.0f, false,
             {10442, 10447, 10742, 10339, 10429}, 100000.0f, {}, 0.0f, -1, {}, true},
            // smart_scripts 175621 / 17558400: Omokk's Head on the pile summons 11 ogres at the pike over ~82 s (each
            // fails the event after 30 s out of combat), then Urok (boss_urok_doomhowl.cpp, DoZoneInCombat 100): the
            // group holds at the pile until he is dead.
            // Applies only after the pile was used (afterUsed 175621): from Omokk's death it held the group 40 yd round
            // the pile while the walk there had to go ~1100 yd round (brs-h / lbrs-n 2026-10-25).
            {229, -22.0f, -386.0f, 48.9f, {9196}, {10584},
             "Lower Blackrock Spire: Urok's Tribute Pile - ogre waves at the pike, Urok Doomhowl", true, 40.0f,
             -100000.0f, false, {10601, 10602, 10584}, 100000.0f, {}, 0.0f, -1, {175621}},
            // instance_scholomance.cpp SetData(DATA_KIRTONOS_THE_HERALD, IN_PROGRESS): Kirtonos 10506 flies in
            // (315.0,70.5) and lands at 314.9,90.3,101.6; the gate 175570 at the top of the stairs (288.4,93.0,110.0)
            // stays closed until he dies or evades. The group holds between the gate and the brazier.
            {289, 312.0f, 92.0f, 102.2f, {10503}, {10506},
             "Scholomance: Kirtonos' balcony (inside the gate 175570) - Kirtonos the Herald", true, 25.0f,
             -100000.0f, false, {10506}, 100000.0f, {}, 0.0f, -1, {175564}},
            // Maraudon: Rotgrip 13596 is swim-only (creature_template_movement Ground 0) and patrols path 890020 7-11
            // yd under the lake surface (z -199.6..-207.0), ~230 yd per leg; no dry ground within 37 yd of it. The
            // group waits on the surface (water poly 32.0,-160.0,-196.4, bot-reachable from Gizlock's shore, 7.6 yd
            // from the patrol, 19 yd from point 2) and pulls him when he passes (mara-h 2026-10-25 00:00: chased
            // 125-166 yd along the patrol for 8 minutes, never engaged; Theradras held back behind him). bossOnlyNear.
            {349, 32.0f, -160.0f, -196.4f, {13601}, {13596},
             "Maraudon: the lake surface on Rotgrip's patrol - wait, pull him as he passes", true, 40.0f, -100000.0f,
             false, {}, 100000.0f, {}, 0.0f, -1, {}, false, true},
        };

        // Walkable links the navmesh lacks (RouteLinkDef). Verified with tools/navmesh_probe.py on the server mmaps.
        std::vector<RouteLinkDef> const routeLinks = {
            // instance_gundrak.cpp: the three altars clicked -> 5 s later the bridge (193188) and its collision
            // (192633, 1775.2,743.5,119.1) go GO_STATE_ACTIVE_ALTERNATIVE. Gal'darah's wing (door 1848,743.8) is a
            // separate navmesh part (996 polygons, x 1797-1980): the main part ends at 1752,743 z 119.2, the statue
            // platform 1765-1785 z 118.4 is a third, the wing starts at 1797. Wave 7: without the bridge the route
            // fell into the wing from the entrance ledge and pulled Gal'darah first.
            {604, 1749.5f, 743.0f, 119.2f, 1802.0f, 743.5f, 119.6f, 192633, 2 /* GO_STATE_ACTIVE_ALTERNATIVE */,
             {29304, 29305, 29307}, "Gundrak: the bridge to Gal'darah (after the three altars)"},
            // [dmw_immolthar_link] Dire Maul West: Immol'thar's pit (z -33.0, 186 polygons, his spawn -38.1,812.4) is
            // no part of the main navmesh: the arena ledge ends at -24.5,767.4,-31.7, the pit floor starts 2.5 yd
            // away 1.3 yd lower (tools/navmesh_probe.py 429 -24.0 767.7 -31.8 -38.1 812.4 -29.5 --route --player:
            // NOT CONNECTED, gap 2.5 yd, dz +1.3), a step down for a player. dmw-n 20261025-123432 2254 s: the force
            // field open, every route to him "endLeft 47", "next boss unreachable" 300 s, the 25-min watchdog at 2272
            // s. Usable once the force field 179503 is open (GO_STATE_ACTIVE).
            {429, -24.5f, 767.4f, -31.7f, -31.4f, 768.4f, -33.0f, 179503, 0 /* GO_STATE_ACTIVE */, {},
             "Dire Maul West: the step down into Immol'thar's pit (once the force field is down) [dmw_immolthar_link]"},
            // The Mechanar's two levels are joined by a lift, not by stairs: GO 183788 Doodad_FactoryElevator01,
            // GAMEOBJECT_TYPE_TRANSPORT (a StaticTransport, data0 pauseAtTime 0: it cycles without stopping).
            // TransportAnimation.dbc 147460-147523: 0-3.7 s at the bottom, up 25.44 yd until 6.67 s, at the top until
            // 11.37 s, down until 15.23 s, period 15.33 s. The spawn (0.54,-1.39,0.21, o 3.147) is the model's origin
            // (Factoryelevator.m2 bounds -264.7..-249.7 / -59.8..-44.8 in GameObjectModels.dtree): turned by o the deck
            // is x 250.3-265.2, y 43.4-58.4 in the shaft east of the Mo'arg doors, deck top 0.4 yd above the spawn z.
            // mmaps hold no game objects: the lower part (1757 polygons) and the upper part (752, Sepethrea and
            // Pathaleon) are not joined (tools/navmesh_probe.py 554 246.2 48.4 1.3 300 10 26 --both). The Mo'arg
            // doors open with the two Gatewatchers' deaths; the Mech strategy (MechanarLift) boards, rides, steps off.
            {554, 245.0f, 50.8f, 1.8f, 269.3f, 52.3f, 26.8f, 183788, 0, {19218, 19710},
             "The Mechanar: the lift up to Sepethrea (GO 183788, after both Gatewatchers)"},
            // The Underbog: Swamplord Musel'ek's terrace (z 24-38, 2791 polygons, Musel'ek and Claw) is no part of the
            // lower navmesh (tools/navmesh_probe.py 546 222.4 -154.1 0.4 281.1 -120.5 29.8 --player: gap 19 yd, dz 14
            // from the steep root at 231.5,-195.3,12.0). The partial route ended under the ledge at 222.4,-154.1 for
            // 4 minutes (wave 2026-10-25 ub-n / ub-h); the only kill (20261024-122211) climbed from 232.3,-194.4,12.4.
            // Corrected 2026-10-25 (wave 7 ub-h: "no ground on the link" from the steep knob 232.3,-194.4,12.4): the
            // root ramp at x 224-228 (Coilfang_Marsh.wmo floor from z 4 at y -194 to z 24 at y -164; recast left only
            // steep polygons at its ends): ground poly 224.6,-194.0,3.8 (bot filter, 40 yd from the ledge) to terrace
            // ground 226.1,-163.7,24.3 (bot path to Musel'ek 73 yd). 36.6 yd at 34 deg, floor under every 2 yd.
            {546, 224.6f, -194.0f, 3.8f, 226.1f, -163.7f, 24.3f, 0, 0, {18105},
             "The Underbog: the root up to Swamplord Musel'ek's terrace (after Ghaz'an)"},
            // The Shattered Halls: the Grand Warlock Chamber Door 182539 (141.5,266.4) opens only by lockpicking or
            // with Nethekurse's death (instance_shattered_halls.cpp doorData, DOOR_TYPE_PASSAGE, with 182540 east of
            // his room). Players drop through the floor hole west of it into the sewer and come up into his room;
            // the ramp passes at_rp_nethekurse (4347, r 30), which ends his immunity. Navmesh with both doors cut:
            // entrance + hole rim (z -15.7..-11.8) | sewer + his room | beyond 182540 (tools/navmesh_probe.py, dtroute:
            // rim poly 1161, floor poly 1148, ground, no water): a 30 yd drop, ~30 % fall damage (LandLinkJump).
            // 2026-10-25 waves: 0 bosses, everything "behind a closed door". Usable while the door is closed.
            {540, 123.0f, 249.8f, -15.4f, 123.0f, 246.5f, -45.0f, 182539, 1 /* GO_STATE_READY: the door closed */, {},
             "The Shattered Halls: the jump into the sewer to Nethekurse (while his door 182539 is closed)", true},
            // Wailing Caverns: Serpentis, Verdan and the Lasher / Dreadfang ledges (z -64..-21) are one navmesh part of
            // 546 polygons, joined to the rest (11675) by no polygon (tools/navmesh_probe.py 43 -86.3 -34.4 -57.8
            // -120.2 -24.6 -28.6). The way up is the ramp south-west of the entrance, without polygons from 157,118
            // z -72 to 181,73 z -45 (20261023-225322 wc-h: the tank walked it at -169.6,93.9,-60.5). 2026-10-25 wc-h:
            // the partial route ended under Serpentis' ledge, boss_unreachable x3.
            // Replaced 2026-10-25 (wave 7 wc-h: 240 s, 0 crossed): the ramp line -157.1,117.6 -> -181.3,73.3 had no
            // floor under 21 of its 28 samples and was 57 yd long. The way on: the west path (z -58, bot reachable)
            // ends at -292.6,-2.6 over a 4-5 yd crack (water at z -105) to the ledge at z -64 (poly 6072; bot path to
            // Serpentis 237 yd, to Verdan 243 yd): a 6 yd jump down, 10.6 yd, line of sight clear, no fall damage.
            {43, -292.6f, -2.6f, -57.9f, -287.0f, 4.0f, -64.0f, 0, 0, {},
             "Wailing Caverns: the jump over the crack onto Lord Serpentis' / Verdan's ledges", true},
            // [event_boss_classic] The way back down: the ledges' navmesh part (546 polygons) has no polygon joined
            // to the rest, so after Serpentis / Verdan nothing below is walkable - the Disciple of Naralex 3678
            // (-135.0,125.4,-78.1), whose escort brings Mutanus, was "starter_partial_walk endLeft 76.9" at the ledge
            // edge (wc-h 20261025-163739 2920-3221 s). Players drop off the ledge above the ramp: edge poly 11067
            // (-187.7,76.4,-46.9, connected to Verdan's ledge -90.1,34.0) to ground poly 11078 below (-189.2,79.6,
            // -79.9; bot path to the Disciple, tools/navmesh_probe.py CONNECTED, 71 yd). A 33 yd drop: fall damage
            // 0.018 * 33 - 0.2426 = ~35 % of max health, no water below this edge (the water drops are 41+ yd).
            {43, -187.7f, 76.4f, -46.9f, -189.2f, 79.6f, -79.9f, 0, 0, {},
             "Wailing Caverns: the drop from Serpentis' / Verdan's ledges back down to the Disciple of Naralex "
             "[event_boss_classic]",
             true},
            // Ahn'kahet: the ramp from the cave east of Jedoga (z -31) up to Herald Volazj (z 27) is two navmesh layers
            // 2-4 yd apart Detour never joins (tile border x 533.3). wave 7 ok-n: the route ended at 420,-665 (endLeft
            // 248). Floor poly 110383 (bot path from 420,-665: 168 yd) to upper poly 110382 (bot path to Volazj
            // 305 yd): 13.6 yd, floor under every 2 yd (drop <= 1.6), line of sight clear at +0.3 and +2 yd.
            {619, 546.6f, -742.7f, -30.9f, 549.9f, -730.8f, -25.3f, 0, 0, {},
             "Ahn'kahet: the step up onto the ramp to Herald Volazj"},
        };

        // Parts of an instance closed until an event (AreaGateDef).
        std::vector<AreaGateDef> const areaGates = {
            // Gal'darah's wing (navmesh part x 1797-1980, y 646-841, z 119-137; the box starts east of x 1856 where
            // the main part ends, above the moat at z 110 and below the entrance ledge at z 177): only over the
            // bridge, after Slad'ran, Moorabi and the Drakkari Colossus. Inside: Gal'darah, 4 Drakkari Rhinos.
            {604, 1856.0f, 1985.0f, 640.0f, 845.0f, 118.0f, 150.0f, {29304, 29305, 29307},
             "Gundrak: Gal'darah's wing (closed until the three altars raise the bridge)"},
            // The Mechanar's upper level (navmesh part x 260-340, z 25-60: Sepethrea, Pathaleon, the Bloodwarders) is
            // reached only by the lift (RouteLinkDef GO 183788), open after both Gatewatchers. Wave 8 mech-h:
            // Gyro-Kill skipped 180 s (careful pull refused), the route chose Centurions 25 yd above the lower navmesh
            // end (244.3,21.3,0) and skipped them as "route end" one by one for 3 minutes.
            {554, 230.0f, 420.0f, -120.0f, 120.0f, 18.0f, 70.0f, {19218, 19710},
             "The Mechanar: the upper level (closed until both Gatewatchers die; reached by the lift)"},
            // Jammal'an's room behind the forcefield 149431 (-518.1,-85.5,-91.2; GO_STATE_READY until the six defenders
            // die: instance_sunken_temple.cpp DATA_DEFENDER_KILLED, smart_scripts 5712-5717 On Death Set Instance
            // Data 11). Inside (x -493..-425, y -121..-52, z -91..-88): Jammal'an, Ogom the Wretched, Atal'ai
            // Deathwalkers / High Priests, Mummified Atal'ai. The six trolls stand on the z -67 balcony ring,
            // 730-850 yd of walk from the z -91 ring for 40-55 yd straight, and were skipped as detours (2026-10-25
            // st-h, 1/8): named here they are required (routeRule "required", no detour skip).
            {109, -517.0f, -400.0f, -135.0f, -45.0f, -100.0f, -75.0f, {5712, 5713, 5714, 5715, 5716, 5717},
             "Sunken Temple: Jammal'an's room (forcefield until Zolo, Gasher, Loro, Hukku, Zul'Lor and Mijan die)"},
            // The Ring of Law stands (acore_world: 24 Arena Spectators 8916 and the Anvilrage / Shadowforge crowd at
            // z -37..-34, x 539-649, y -245..-132; the arena floor is z -53). Hostile until the Ring of Law is done
            // (instance_blackrock_depths.cpp: then FACTION_NEUTRAL); a group fights in the arena, never in the
            // stands. Wave 8 heroic: the route took an Anvilrage Medic and the spectators as the nearest trash and the
            // crowd wiped the group.
            // floor -40: Pyromancer Loregrain 9024 and his two 8913 stand at 530.2,-243.9,-43.0 (inside -44 he was
            // never a target, 2026-10-25 brd-h); the crowd is at z -36.9..-34.1.
            {230, 525.0f, 665.0f, -260.0f, -118.0f, -40.0f, -25.0f, {},
             "Blackrock Depths: the Ring of Law stands (spectators, never a target)"},
            // Pyromancer Loregrain 9024 and his two 8913 (530.2,-243.9,-43.0): the way there is the stands walkway
            // (z -38.2), safe only once the Ring of Law is done and the crowd turns neutral (brd-n / brd-h wave 7: the
            // body pull walked past the spectators, the crowd fight left the tank on the 198-polygon seat island).
            {230, 515.0f, 545.0f, -258.0f, -230.0f, -47.0f, -40.5f, {10096},
             "Blackrock Depths: Pyromancer Loregrain (after the Ring of Law)"},
            // Stratholme ziggurats: the first Thuzadin Acolyte 10399 that dies tells its Ash'ari Crystal (smart_scripts
            // 10399 id 3 -> -53955 / -53968 / -53963), the crystal sets TYPE_ZIGGURATn to 2 and dies. The instance
            // takes 2 only after 1 (instance_stratholme.cpp:350-385: the boss's death), so an acolyte killed before
            // the ziggurat's boss leaves it shut and Ramstein never comes. The acolytes wait for their boss (boxes
            // around the five at each crystal, the boss 20-30 yd away outside; acore_world.creature map 329).
            {329, 3825.0f, 3852.0f, -3510.0f, -3486.0f, 135.0f, 150.0f, {10437},
             "Stratholme: Nerub'enkan's ziggurat acolytes (after Nerub'enkan)"},
            {329, 3833.0f, 3862.0f, -3760.0f, -3740.0f, 140.0f, 155.0f, {10436},
             "Stratholme: Baroness Anastari's ziggurat acolytes (after the Baroness)"},
            {329, 4046.0f, 4072.0f, -3680.0f, -3655.0f, 128.0f, 142.0f, {10438},
             "Stratholme: Maleki's ziggurat acolytes (after Maleki)"},
            // The lone acolyte 53267 (3929.8,-3674.3) between the ziggurats: its death reaches the nearest crystal
            // in 200 yd, whichever that is.
            {329, 3922.0f, 3937.0f, -3682.0f, -3666.0f, 150.0f, 163.0f, {10436, 10437, 10438},
             "Stratholme: the acolyte between the ziggurats (after all three ziggurat bosses)"},
            // Blackrock Spire is one map for both wings; Upper Blackrock Spire is closed to an LBRS group (acore_world
            // map 229). LBRS bosses all stand at x < 0 (Omokk -23,-301,32 ... Wyrmthalak -23,-486,91), its trash east
            // of them at x 60-108 below z 62. UBRS: the Dragonspire Hall behind the Dragonspine Door (164725 at
            // 126.9,-319.1,70.9, open: no Seal of Ascension check on AC; at_dragonspire_hall 2046 at 102.4,-319.2,65.5)
            // with the Blackhand Summoner / Dreadweaver / Veteran niches and Rage Talon Dragonspawn (x 119-234,
            // z 71-122) and Pyroguard Emberseer (144,-258,96); above z 85 the Rookery, Drakkisath's hall, the stadium
            // and The Beast. LBRS waves 2026-10-24 (lbrs-n 5A-7, 10:27-11:22): the tank walked from the Legionnaires'
            // room (95,-295,61) up into the hall and pulled the niches - five wipes on UBRS trash. Run 13:04 (gate
            // live): no wipe, but the group still walked into the hall at 84-92 s (leader without a target), fought
            // there and walked 6 minutes as deep as x 225 towards LBRS targets; the exit point is the Legionnaires'
            // room by the hall's mouth (navmesh: 57 yd from inside the Dragonspine Door back to 89.6,-289.4).
            {229, 97.0f, 240.0f, -445.0f, -228.0f, 63.0f, 135.0f, {},
             "Blackrock Spire: UBRS Dragonspire Hall and Emberseer (closed to an LBRS group)", "lbrs-n", 93.5f,
             -296.0f, 60.7f},
            {229, 30.0f, 240.0f, -600.0f, -228.0f, 85.0f, 140.0f, {},
             "Blackrock Spire: UBRS Rookery, Drakkisath, stadium, The Beast (closed to an LBRS group)", "lbrs-n",
             93.5f, -296.0f, 60.7f},
            // [b31_ubrs_door] The stadium's drake pen east of the wave portcullis 175185 (87843 at 194.2,-420.2,
            // 110.8, facing west): Rend's waves spawn there (creature_summon_groups 10429, x 201-214) and come out
            // through the gate, which SummonWave / Gyth open for 10 s each; after Gyth nothing opens it, the exit
            // 175186 lies on the far side. ubrs-n 20261025-184229-10B-43 (and 115805-10A-8): the group ended up in
            // the pen as the gate closed, fought wave 6 and Gyth inside and stood 45 min locked in after Rend's DONE
            // (x 191.8-224.5, y -447..-391). Closed for good to a UBRS run: no target, route or tank pull in there;
            // a leader inside walks out to the stadium side (out of combat, through the gate while it is up).
            {229, 194.6f, 234.6f, -450.0f, -388.0f, 105.0f, 118.0f, {},
             "Upper Blackrock Spire: the stadium's drake pen behind the wave portcullis 175185 (locks in after Gyth)",
             "ubrs-n", 187.0f, -420.3f, 110.6f},
        };

        // Closed doors an event the harness knows opens (DoorEventDef). Utgarde Keep's forge room: three walls of
        // flame (Doodad_VR_ForgeFire_*, GAMEOBJECT_TYPE_DOOR spawned GO_STATE_READY, acore_world gameobject
        // 65411-65413 at 359.9-362.4,-14.4..-18.2 - the models reach out to their forges). instance_utgarde_keep.cpp
        // SetData(DATA_FORGE_n): any state but NOT_STARTED opens wall n (with its bellows and anvil), NOT_STARTED (the
        // master's Reset: an evade, a wipe) closes it again. utgarde_keep.cpp npc_dragonflayer_forge_master: master n
        // (24079, told apart by his home like the script's constructor) sets IN_PROGRESS on engage, DONE on death, and
        // evades at once while GetData(DATA_FORGE_n-1) is 0. User 2026-10-24: «Как они через огонь прошли?» / «Даже с
        // правами ГМ не могу пройти через эту границу».
        std::vector<DoorEventDef> const doorEvents = {
            {574, 186692, 24079, 349.556f, -39.3328f, 0,
             "Utgarde Keep: forge 1's wall of flame opens with Forge Master 1's pull"},
            {574, 186693, 24079, 385.775f, -16.2345f, 100,
             "Utgarde Keep: forge 2's wall of flame opens with Forge Master 2's pull (after forge 1)"},
            {574, 186691, 24079, 347.623f, 4.63357f, 101,
             "Utgarde Keep: forge 3's wall of flame opens with Forge Master 3's pull (after forge 2)"},
            // [hleft-hos-brann-door] Halls of Stone: Sjonnir's door 191296 (instance_halls_of_stone.cpp
            // GO_SJONNIR_DOOR, open once BRANN_DOOR is DONE) is Brann's step (gossip 10012 at the door): the walk to
            // Brann 28070 is no "route through a closed door", his starter comes straight after the Maiden.
            {599, 191296, 28070, 1077.4f, 474.2f, 0, "Halls of Stone: Sjonnir's door opens with Brann's last step"},
        };

        // Dire Maul West: Doodad_DireMaulBossForceField01 179503 (-39.0,813.7,-27.3, GO_STATE_READY) round Immol'thar.
        // The five Crystal Generators poll every 5 s (smart_scripts source 1, event 60): with no Arcane Aberration
        // 11480 / Mana Remnant 11483 alive within 70 yd (conditions 22/1, type 29 negated) one sets state 0 and its
        // TYPE_PYLONS_STATE bit; ALL_PYLONS_OFF 0x1F makes Immol'thar attackable and his SetData opens the field
        // (instance_dire_maul.cpp, smart_scripts 11496 id 2). 2026-10-25 dmw-n: one generator went down on the route,
        // the ledge guards were detours (602 yd for 38 yd), Immol'thar "behind a closed door", 2/5.
        std::vector<GuardedDoorDef> const guardedDoors = {
            {429, 179503, {177259, 177257, 177258, 179504, 179505}, 1, {11480, 11483}, 70.0f,
             "Dire Maul West: Immol'thar's force field drops with the five Crystal Generators (kill their guards)"},
            // instance_sunken_temple.cpp SetData(DATA_DEFENDER_KILLED): the sixth defender's death opens the forcefield
            // 149431 (smart_scripts 5712-5717 On Death Set Instance Data 11). No guarded objects: the door's own
            // guards, anywhere on the map. Approach points: the balcony polygons 20-30 yd short of each defender on the
            // route from the entrance (tools/navmesh_probe.py 109 -319.3 99.0 -131.9 <defender> --route, --at), z
            // -65..-69; from the pit every stop was "no sight point" / "tank does not see it" (2026-10-25 st-h 062726).
            // Line of sight from the points is not verified offline (the probe has no vmaps): guard_approach logs the
            // walk.
            {109, 149431, {}, 1, {5712, 5713, 5714, 5715, 5716, 5717}, 0.0f,
             "Sunken Temple: Jammal'an's forcefield drops with Zolo, Gasher, Loro, Hukku, Zul'Lor and Mijan",
             {{5712, -542.1f, 157.1f, -69.2f},
              {5713, -546.4f, 67.6f, -68.1f},
              {5714, -485.9f, 15.3f, -64.9f},
              {5715, -402.7f, 39.2f, -68.1f},
              {5717, -379.7f, 127.1f, -69.5f},
              {5716, -448.4f, 173.1f, -64.8f}}},
            // Uldaman: the Stone Keepers' guid SmartAI (-27554/-27555/-27794/-28368, ids 7/8) sets the Temple Door
            // 124367 to state 0 when no Stone Keeper 4857 is alive within 100 yd - the last keeper's death.
            {70, 124367, {}, 1, {4857}, 100.0f,
             "Uldaman: the Temple Door to Archaedas opens with the last Stone Keeper's death (Altar of the Keepers)"},
            // instance_blackrock_spire.cpp Dragonspireroomstore / Dragonspireroomcheck: 1 s after areatrigger 2046 the
            // Blackhand Dreadweaver / Summoner / Veteran within 15 yd of each of the seven hall runes (state 0 while
            // guarded) are stored; a rune whose guards are dead turns off, all seven: DATA_DRAGONSPIRE_ROOM DONE and
            // the Emberseer In door 175244 opens (the Blackrock Altar starter waits on that, afterBossDone 15).
            {229, 175244, {175194, 175195, 175196, 175197, 175198, 175199, 175200}, 0 /* GO_STATE_ACTIVE */,
             {9817, 9818, 9819}, 15.0f,
             "Upper Blackrock Spire: Emberseer In opens with the seven hall runes (kill the Blackhand niche guards)"},
            // Razorfen Kraul: the ward 21099 in front of Agathelos opens itself (smart_scripts 21099, every 5 s) once
            // no Death's Head Ward Keeper 4625 is alive within 50 yd (conditions 22/1/21099); then Agathelos walks out.
            // rfk-h 2026-10-25: 5/6, "Agathelos - behind a closed door (ward 21099)".
            {47, 21099, {21099}, 1, {4625}, 50.0f,
             "Razorfen Kraul: the ward before Agathelos drops with its two Ward Keepers"},
        };

        // Items a real group brings or loots (StarterKind::UseItem). dropFrom: a 100 % drop of this dungeon - the
        // harness bots do not loot (-loot), so the drop is put into the leader's bags once the creature is dead.
        std::vector<EventItemDef> const eventItems = {
            {329, 12382, 1,
             "Key to the City: Magistrate Barthilas' drop (100 %), kept by a player who ran Stratholme once; lock 879 "
             "of the Service Entrance Gate 175368; every member carries one [b32_strat_gate]",
             {}, false, -1, true},
            {209, 8444, 1, "Executioner's Key: 100 % drop of the Sandfury Executioner on the pyramid", {7274}},
            // User 2026-10-24: give the prerequisite item for now (the mechanics are tested first); bots learn to get
            // it themselves later.
            {209, 9240, 1,
             "Mallet of Zul'Farrak: Sacred Mallet 9241 (Qiaga the Keeper, Hinterlands) used at the Altar of Zul, gong "
             "for Gahz'rilla"},
            {70, 7733, 1,
             "Staff of Prehistoria: The Shaft of Tsol (Revelosh, 100 %) + Gni'kiv Medallion (Baelog's Chest at the "
             "Lost Dwarves, 100 %; lock 93 opens for anyone - the Lost Dwarves are faction 122, friendly to an "
             "Alliance group), combined (item spells 9594 / 9595)",
             {6910}},
            {109, 10465, 1,
             "Egg of Hakkar: start item of quest 3528 'The God Hakkar' (Yeh'kinya, Tanaris; chain 3520 -> 3527 -> "
             "4787 -> 3528, levels 44-53)"},
            {109, 10460, 1, "Hakkari Blood: 100 % drop of every Hakkari Bloodkeeper of the Shade of Hakkar event",
             {8438}, true},
            {230, 11000, 1,
             "Shadowforge Key: quest 3802 'Dark Iron Legacy' reward (Ironfel from Fineous Darkvire on Franclorn's "
             "statue); lock 680 of the Shadowforge Lock / East Garrison Door / Shadowforge Gate, or Lockpicking 250"},
            {230, 11885, 1, "Shadowforge Torch: 100 % drop of each Shadowforge Flame Keeper in the Lyceum", {9956},
             true},
            {229, 12534, 1, "Omokk's Head: 100 % drop of Highlord Omokk (LBRS)", {9196}},
            // Omokk's Head's spell 16447 (Spawn Challenge to Urok) takes the Roughshod Pike 12533 as reagent and needs
            // the pile's spell focus 743 within 3 yd (Spell.dbc); without the pike the cast is refused.
            {229, 12533, 1,
             "Roughshod Pike: Roughshod Pike rack GO 175886 in LBRS (5 spawns at -34.5..-37.8,-589.6,31.0, lock 93 "
             "for anyone, loot 13340 100 %), the reagent of Omokk's Head (spell 16447)"},
            {289, 13523, 1, "Blood of Innocents: 100 % drop of Jandice Barov (also quest 5384)", {10503}},
            {289, 13873, 1, "Viewing Room Key: 100 % drop of Rattlegore", {11622}},
            // [b32_brs] quest 4771 "Dawn's Gambit" (Betina Bigglezink, Light's Hope Chapel: smart_scripts 11035 id 2
            // casts 18367 'Create Dawn's Gambit'), carried in
            {289, 12368, 1,
             "Dawn's Gambit: quest 4771 item from Betina Bigglezink (Light's Hope Chapel, spell 18367), carried in; "
             "used at the Viewing Room spell focus GO 176111 (spell 17045, focus 823)"},
            {189, 7146, 1,
             "The Scarlet Key: Doan's Strongbox (GO 103821, loot 4668, 100 %) behind Arcanist Doan in the Library "
             "wing, carried into the Cathedral (lock 299 also opens to Lockpicking 175)"},
            {429, 18266, 1, "Gordok Courtyard Key: Fengus's Chest next to Guard Fengus (lock 43, no key)", {14321}},
            // [dmw-crescent-key] lock 1562 of the West wing Door 177221 (50.6,501.9,-23.1): Crescent Key 18249 or
            // Lockpicking 300. A Dire Maul East drop: carried in, as the Scarlet Key into the Cathedral.
            {429, 18249, 1,
             "Crescent Key: 100 % drop of Pusillin 14354 (Dire Maul East, the chase), carried into Dire Maul West; "
             "lock 1562 of Door 177221 (also Lockpicking 300)"},
            {429, 18268, 1, "Gordok Inner Door Key: 100 % drop of Guard Mol'dar", {14326}},
            {36, 5397, 1,
             "Defias Gunpowder: the Defias Gunpowder crate GO 17155 (-106.4,-617.3,13.8; lock 57, loot 2882 100 %) "
             "before the Defias Cannon"},
            {560, 25853, 1,
             "Pack of Incendiary Bombs: start item of quest 10283 'Taretha's Diversion' (Erozion, level 66; his "
             "gossip 7769 hands out a new one)"},
            {557, 32079, 1,
             "Shaffar's Stasis Chamber Key: quest 10981 (Image of Commander Ameer, Netherstorm; Consortium Revered + "
             "Mark of the Nexus-King from Mana-Tombs ethereals), no class limit",
             {}, false, 1},
        };

        // User rule: the only bosses a run may leave alive.
        std::vector<ClassGatedBossDef> const classGatedBosses = {
            {556, 1, 23035,
             "Anzu (heroic): Essence-Infused Moonstone (32449) on The Raven's Claw, from the druid-only "
             "epic flight form chain (quest 11001 'Vanquish the Raven God', AllowableClasses 1024)"},
        };

        // Script boss slots without a boss on that difficulty (instance enums: sethekk_halls.h, mana_tombs.h).
        std::vector<AbsentSlotDef> const absentSlots = {
            {556, 0, 1, "Anzu (slot DATA_ANZU) exists in heroic only (The Raven's Claw spawnMask 2)"},
            {557, 0, 3, "Yor (slot DATA_YOR) exists in heroic only (Shaffar's Stasis Chamber spawnMask 2)"},
            {619, 0, 4, "Amanitar (slot DATA_AMANITAR) exists in heroic only (spawnMask 2)"},
            {604, 0, 4, "Eck the Ferocious (slot DATA_ECK_THE_FEROCIOUS) exists in heroic only"},
            {576, 0, 4, "Commander Kolurg / Stoutbeard (slot DATA_COMMANDER_EVENT) exists in heroic only"},
            // [event_boss_tbcwotlk] nexus.h DATA_TELESTRA_ORB 5 / DATA_ANOMALUS_ORB 6 / DATA_ORMOROK_ORB 7: the
            // Keristrasza prison orbs (SetBossState DONE on each boss's death), no boss on either difficulty.
            {576, 0, 5, "Telestra's orb (slot DATA_TELESTRA_ORB) is no boss"},
            {576, 1, 5, "Telestra's orb (slot DATA_TELESTRA_ORB) is no boss"},
            {576, 0, 6, "Anomalus' orb (slot DATA_ANOMALUS_ORB) is no boss"},
            {576, 1, 6, "Anomalus' orb (slot DATA_ANOMALUS_ORB) is no boss"},
            {576, 0, 7, "Ormorok's orb (slot DATA_ORMOROK_ORB) is no boss"},
            {576, 1, 7, "Ormorok's orb (slot DATA_ORMOROK_ORB) is no boss"},
            {595, 0, 4, "Infinite Corruptor (slot BOSS_INFINITE_CORRUPTOR) exists in heroic only (timed)"},
        };

        // [hleft-script-slot-names] culling_of_stratholme.h: BOSS_MEATHOOK 0 .. BOSS_INFINITE_CORRUPTOR 4. Mal'Ganis
        // never dies (he teleports away; the script sets the slot DONE), the Corruptor's slot goes FAIL when the
        // Guardian of Time timer runs out (boss_infinite.cpp SAY_FAIL).
        std::vector<ScriptSlotDef> const scriptSlots = {
            {595, -1, 0, "Meathook", 26529, 0, nullptr},
            {595, -1, 1, "Salramm the Fleshcrafter", 26530, 1, nullptr},
            {595, -1, 2, "Chrono-Lord Epoch", 26532, 2, nullptr},
            {595, -1, 3, "Mal'Ganis", 26533, 3, nullptr},
            {595, 1, 4, "Infinite Corruptor", 32273, -1, "Guardian of Time expired"},
            // [event_boss_tbcwotlk] the_black_morass.h DATA_CHRONO_LORD_DEJA 0 / DATA_TEMPORUS 1 / DATA_AEONUS 2
            // (BossAI of 17879 / 17880 / 17881). Unnamed, the slots ran next to their dbc rows: bm-h 20261025-163739 "done"
            // with bossesTotal 4 and "Aeonus" killed twice (slot 2 bound by DATA_AEONUS ObjectData, its row kept).
            {269, -1, 0, "Chrono Lord Deja", 17879, 0, nullptr},
            {269, -1, 1, "Temporus", 17880, 1, nullptr},
            {269, -1, 2, "Aeonus", 17881, 2, nullptr},
            // [event_boss_tbcwotlk] arcatraz.h DATA_ZEREKETH 0 / DATA_DALLIAH 1 / DATA_SOCCOTHRATES 2 /
            // DATA_WARDEN_MELLICHAR 3 (boss_harbinger_skyriss BossAI slot): every arc "done" run of 2026-10-24/25
            // ended with bossesTotal 5 and "Harbinger Skyriss" killed twice (slot 3 named only at the kill).
            {552, -1, 0, "Zereketh the Unbound", 20870, 0, nullptr},
            {552, -1, 1, "Dalliah the Doomsayer", 20885, 1, nullptr},
            {552, -1, 2, "Wrath-Scryer Soccothrates", 20886, 2, nullptr},
            {552, -1, 3, "Harbinger Skyriss", 20912, 3, nullptr},
            // [b31_dg] azjol_nerub.h DATA_KRIKTHIR 0 / DATA_HADRONOX 1 / DATA_ANUBARAK 2. Unnamed, the Hadronox slot
            // ("boss 1", entry 0) went IN_PROGRESS at the gauntlet's first Crusher and was never bound to her: every
            // gauntlet counted as an attempt (an-n 20261025-184229-5G-32, 14 resets). Named, the slot is bound when
            // Hadronox herself fights the party (TickRun), and GauntletSlot keeps the adds out of it.
            {601, -1, 0, "Krik'thir the Gatewatcher", 28684, 0, nullptr},
            {601, -1, 1, "Hadronox", 28921, 1, nullptr},
            {601, -1, 2, "Anub'arak", 29120, 2, nullptr},
        };

        // [hleft-bm-quest] gossip_menu_option 8088 / conditions: option 0 with 10297 in the log, 1 with it rewarded.
        std::vector<PrepQuestDef> const prepQuests = {
            {269, 10297, "Black Morass: The Opening of the Dark Portal (Sa'at's Chrono-beacon, gossip 8088)"},
        };

        // [hleft-boss-preclear] Drak'Tharon Keep 600: Trollgore 26630 at (-266.2,-660.1), King Dred 27483 at
        // (-544.9,-697.0); Drakkari Scytheclaw 26628, Drakkari Gutripper 26641.
        std::vector<BossPreClearDef> const bossPreClears = {
            {600, -1, 26630, -266.2f, -660.1f, 26.5f, 30.0f, {}, "Drak'Tharon Keep: the trash round Trollgore first"},
            {600, 1, 27483, -544.9f, -697.0f, 30.3f, 100.0f, {26628, 26641},
             "Drak'Tharon Keep heroic: King Dred's Scytheclaws / Gutrippers first"},
            // The Oculus 578: Varos Cloudstrider 27447 is NON_ATTACKABLE until the ring's 10 Centrifuge Constructs die
            // (boss_varos.cpp DATA_CC_COUNT); every Construct and its Ring-Lord pack (Sorceress 27639, Conjurer
            // 27640, Construct 27641; world DB spawns on both rings, 160-175 yd from this centre) is required.
            // [b30_oculus] the Constructs only: the drake route (OCRoute::GetLegTarget) flies to the packs with a
            // Construct alive and then to Varos; a Ring-Lord left on an island whose Construct is dead was a
            // required target no leg ever flies to (occ-h 20261025-184229-26: 1000 s on a Sorceress the route never
            // reached). The Ring-Lords of a pack come with its Construct's pull.
            {578, -1, 27447, 1080.0f, 1047.0f, 436.0f, 185.0f, {27641},
             "The Oculus: the Centrifuge Constructs (and the Ring-Lords pulled with them) before Varos"},
        };

        // [hleft-kill-summon] gundrak.h: DATA_MOORABI 1, DATA_ECK_THE_FEROCIOUS 4.
        std::vector<KillSummonDef> const killSummons = {
            {604, 1, {29920}, 1, 4, 29932, "Gundrak heroic: the Ruins Dwellers behind Moorabi's door summon Eck"},
        };

        struct DeathlessTrashDef
        {
            uint32 mapId;
            uint32 entry;
            uint32 bossEntry;
        };

        std::vector<DeathlessTrashDef> const deathlessTrash = {
            {576, 26793, 26794},  // The Nexus: Crystalline Frayer -> Seed Pod until Ormorok the Tree-Shaper dies
        };
    }

    std::vector<EventStarterDef const*> EventStarters(uint32 mapId)
    {
        std::vector<EventStarterDef const*> result;
        for (EventStarterDef const& starter : starters)
            if (starter.mapId == mapId)
                result.push_back(&starter);
        return result;
    }

    std::vector<EventSpotDef const*> EventSpots(uint32 mapId)
    {
        std::vector<EventSpotDef const*> result;
        for (EventSpotDef const& spot : eventSpots)
            if (spot.mapId == mapId)
                result.push_back(&spot);
        return result;
    }

    std::vector<RouteLinkDef const*> RouteLinks(uint32 mapId)
    {
        std::vector<RouteLinkDef const*> result;
        for (RouteLinkDef const& link : routeLinks)
            if (link.mapId == mapId)
                result.push_back(&link);
        return result;
    }

    std::vector<AreaGateDef const*> AreaGates(uint32 mapId)
    {
        std::vector<AreaGateDef const*> result;
        for (AreaGateDef const& gate : areaGates)
            if (gate.mapId == mapId)
                result.push_back(&gate);
        return result;
    }

    std::vector<DoorEventDef const*> DoorEvents(uint32 mapId)
    {
        std::vector<DoorEventDef const*> result;
        for (DoorEventDef const& door : doorEvents)
            if (door.mapId == mapId)
                result.push_back(&door);
        return result;
    }

    std::vector<GuardedDoorDef const*> GuardedDoors(uint32 mapId)
    {
        std::vector<GuardedDoorDef const*> result;
        for (GuardedDoorDef const& door : guardedDoors)
            if (door.mapId == mapId)
                result.push_back(&door);
        return result;
    }

    std::vector<EventItemDef const*> EventItems(uint32 mapId)
    {
        std::vector<EventItemDef const*> result;
        for (EventItemDef const& item : eventItems)
            if (item.mapId == mapId)
                result.push_back(&item);
        return result;
    }

    char const* ClassGatedBoss(uint32 mapId, uint8 difficulty, uint32 creatureEntry)
    {
        for (ClassGatedBossDef const& def : classGatedBosses)
            if (def.mapId == mapId && def.difficulty == difficulty && def.entry == creatureEntry)
                return def.reason;
        return nullptr;
    }

    std::vector<ClassGatedBossDef const*> ClassGatedBosses(uint32 mapId, uint8 difficulty)
    {
        std::vector<ClassGatedBossDef const*> result;
        for (ClassGatedBossDef const& def : classGatedBosses)
            if (def.mapId == mapId && def.difficulty == difficulty)
                result.push_back(&def);
        return result;
    }

    char const* AbsentScriptSlot(uint32 mapId, uint8 difficulty, uint32 bossId)
    {
        for (AbsentSlotDef const& def : absentSlots)
            if (def.mapId == mapId && def.difficulty == difficulty && def.bossId == bossId)
                return def.reason;
        return nullptr;
    }

    std::vector<AbsentSlotDef const*> AbsentScriptSlots(uint32 mapId, uint8 difficulty)
    {
        std::vector<AbsentSlotDef const*> result;
        for (AbsentSlotDef const& def : absentSlots)
            if (def.mapId == mapId && def.difficulty == difficulty)
                result.push_back(&def);
        return result;
    }

    std::vector<PrepQuestDef const*> PrepQuests(uint32 mapId)
    {
        std::vector<PrepQuestDef const*> result;
        for (PrepQuestDef const& def : prepQuests)
            if (def.mapId == mapId)
                result.push_back(&def);
        return result;
    }

    std::vector<BossPreClearDef const*> BossPreClears(uint32 mapId, uint8 difficulty)
    {
        std::vector<BossPreClearDef const*> result;
        for (BossPreClearDef const& def : bossPreClears)
            if (def.mapId == mapId && (def.difficulty < 0 || uint8(def.difficulty) == difficulty))
                result.push_back(&def);
        return result;
    }

    std::vector<KillSummonDef const*> KillSummons(uint32 mapId, uint8 difficulty)
    {
        std::vector<KillSummonDef const*> result;
        for (KillSummonDef const& def : killSummons)
            if (def.mapId == mapId && def.difficulty == difficulty)
                result.push_back(&def);
        return result;
    }

    std::vector<ScriptSlotDef const*> ScriptSlots(uint32 mapId, uint8 difficulty)
    {
        std::vector<ScriptSlotDef const*> result;
        for (ScriptSlotDef const& def : scriptSlots)
            if (def.mapId == mapId && (def.difficulty < 0 || uint8(def.difficulty) == difficulty))
                result.push_back(&def);
        return result;
    }

    uint32 DeathlessUntilBoss(uint32 mapId, uint32 creatureEntry)
    {
        for (DeathlessTrashDef const& def : deathlessTrash)
            if (def.mapId == mapId && def.entry == creatureEntry)
                return def.bossEntry;
        return 0;
    }

    char const* EncounterRequirement(uint32 mapId, uint32 creatureEntry, char const** note)
    {
        for (EncounterRequirementDef const& def : encounterRequirements)
            if (def.mapId == mapId && def.entry == creatureEntry)
            {
                if (note)
                    *note = def.note;
                return def.kind;
            }
        return nullptr;
    }

    uint32 EncounterDoneData(uint32 mapId, uint32 creditEntry)
    {
        for (EncounterDoneDataDef const& def : encounterDoneData)
            if (def.mapId == mapId && def.creditEntry == creditEntry)
                return def.dataId;
        return 0;
    }

    uint32 EncounterRequirementItem(uint32 mapId, uint32 creatureEntry)
    {
        for (EncounterRequirementDef const& def : encounterRequirements)
            if (def.mapId == mapId && def.entry == creatureEntry)
                return def.itemId;
        return 0;
    }

    uint32 EncounterCarrierCredit(uint32 mapId, uint32 creatureEntry)
    {
        for (EncounterCarrierDef const& def : encounterCarriers)
            if (def.mapId == mapId && def.carrierEntry == creatureEntry)
                return def.creditEntry;
        return 0;
    }

    KeyDoorPassDef const* KeyDoorPass(uint32 mapId, uint32 doorEntry)
    {
        for (KeyDoorPassDef const& def : keyDoorPasses)
            if (def.mapId == mapId && def.doorEntry == doorEntry)
                return &def;
        return nullptr;
    }

    std::vector<uint32> EncounterCreditAliases(uint32 mapId, uint32 creditEntry)
    {
        std::vector<uint32> result;
        for (EncounterCreditAliasDef const& def : encounterCreditAliases)
            if (def.mapId == mapId && def.creditEntry == creditEntry)
                result.push_back(def.aliasEntry);
        return result;
    }

    std::vector<KnownGapDef const*> KnownGaps(uint32 mapId)
    {
        std::vector<KnownGapDef const*> result;
        for (KnownGapDef const& gap : knownGaps)
            if (gap.mapId == mapId)
                result.push_back(&gap);
        return result;
    }
}  // namespace Leader::Knowledge

namespace Leader::Knowledge
{
    float FleeForAssistHealthPct(uint32 creatureEntry)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, float> known;
        std::lock_guard<std::mutex> guard(lock);
        auto [itr, fresh] = known.try_emplace(creatureEntry, 0.0f);
        if (fresh)
            for (SmartScriptHolder const& holder :
                 sSmartScriptMgr->GetScript(int32(creatureEntry), SMART_SCRIPT_TYPE_CREATURE))
                if (holder.GetEventType() == SMART_EVENT_HEALTH_PCT &&
                    holder.GetActionType() == SMART_ACTION_FLEE_FOR_ASSIST)
                    itr->second = std::max(itr->second, float(holder.event.minMaxRepeat.max));
        return itr->second;
    }
}

namespace Leader::Knowledge
{
    namespace
    {
        std::mutex doorCutLock;
        std::unordered_set<ObjectGuid> doorCut;
    }

    void SetDoorCut(ObjectGuid bot, bool cut)
    {
        std::lock_guard<std::mutex> lock(doorCutLock);
        if (cut)
            doorCut.insert(bot);
        else
            doorCut.erase(bot);
    }

    bool IsDoorCut(ObjectGuid bot)
    {
        std::lock_guard<std::mutex> lock(doorCutLock);
        return doorCut.count(bot) != 0;
    }

    void ClearDoorCuts()
    {
        std::lock_guard<std::mutex> lock(doorCutLock);
        doorCut.clear();
    }
}
