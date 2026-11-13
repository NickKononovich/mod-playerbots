/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [guild60] Dungeons through the global leader: the coordinator forms a five (tank, healer, three dps of a normal
// dungeon's LFGDungeons band, the dungeon holding upgrades or an open clear for them), each member walks to the
// entrance on its own safe walk, the five becomes a real group there and walks in through the portal; inside, the
// dungeon leader (the tested rfc-h / ac-h / strat leader) leads it and its word is final. Afterwards authority goes
// back to the global leader with a report (drops, deaths, needs) and the members go back to their quests. Every
// run is kept in the completion table (playerbots DB guild_project_runs).

#include "GuildCoordinator.h"

#include "BuyAction.h"  // [supply27] CountBagFood, RESTOCK_*
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cmath>
#include <limits>
#include <sstream>

#include "AiFactory.h"  // [roles59] talent trees
#include "AiObjectContext.h"
#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DpsEstimate.h"  // [next1] the five gate
#include "HealthTriggers.h"  // [next1] RestState::HealerThroughput
#include "Formulas.h"  // [roles108] Acore::XP::GetGrayLevel
#include "NewRpgProfession.h"  // [roles118] NeedsRespec
#include "GameTime.h"
#include "Group.h"
#include "GameEventMgr.h"  // [sfk246] a game event's spawns
#include "LFGMgr.h"  // [next1] whether the Dungeon Finder can port a five
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "MapMgr.h"
#include "NewRpgBaseAction.h"
#include "NewRpgCoop.h"  // [coop] a co-op party member is free for a five
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "SelfBotControl.h"
#include "SlowScope.h"  // [perf211]
#include "GuildGear.h"  // [loot12] the guild's one gear judge
#include "SmartScriptMgr.h"  // [loot16] a source's AI-start despawn
#include "StatsWeightCalculator.h"
#include "TestHarnessData.h"
#include "LeaderDungeonRun.h"  // [guild70] the five-leader runs the dungeon (no test harness)
#include "RouteTargets.h"  // [loot13] the route's goal hooks
#include "LeaderOrders.h"  // [run80] Leader::LedBot
#include "Creature.h"             // [pack1] CREATURE_Z_ATTACK_RANGE, the core's aggro height gate
#include "LeaderPackModel.h"      // [pack1] the one pull-pack model, shared with the leader's planner
#include "LeaderPlannerShared.h"  // [dun172] the bear the planner plans with
#include "PullActions.h"          // [pack1] FightHold: how far the melee stand from the fight spot
#include "LeaderTankSaves.h"      // [split254] the tank's defensives, as the planner prices them
#include "RtiValue.h"             // [split254] the members' crowd control
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    constexpr uint32 LFG_TYPE_DUNGEON = 1;     // LFGMgr.h lfg::LFG_TYPE_DUNGEON
    constexpr uint32 SPELL_DISENCHANT = 13262;
    // NewRpgStatusUpdateAction's GO_CAMP arrival distance: a member within it stands at the entrance
    constexpr float ENTRANCE_REACH = 10.0f;
    constexpr size_t ADDON_LINE_MAX = 240;  // the client drops longer addon messages
    constexpr size_t FIVE_DROPS_KEPT = 40;
    // [guild70] markers in the five-leader's report reason (FiveRunReport -> GuildFiveReport)
    constexpr char const* FIVE_LFG_REFUSED = "lfg refused: ";
    constexpr char const* REFUSAL_OTHER_CONTINENT = "entrance on another continent (map ";
    constexpr char const* FIVE_ITEM_NEED = "item needed: ";
    // [guild70] the last "no Dungeon Finder" reason logged per five (world thread)
    std::map<uint32, std::string> fiveLfgWhy;
    // [next1] the last five-gate reason logged per dungeon (world thread)
    std::map<std::string, std::string> gateWhy;

    uint32 Now() { return uint32(GameTime::GetGameTime().count()); }

    std::string ItemName(uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return proto ? proto->Name1 : std::to_string(itemId);
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // a text value for a quoted SQL string (backslash and quote)
    void SqlEscape(std::string& text)
    {
        std::string out;
        for (char c : text)
        {
            if (c == '\\' || c == '\'')
                out += '\\';
            out += c;
        }
        text = out;
    }

    std::string FiveCell(std::string text, size_t max)
    {
        std::replace(text.begin(), text.end(), ';', ',');
        std::replace(text.begin(), text.end(), '\t', ' ');
        return text.size() > max ? text.substr(0, max) : text;
    }

    void SendLine(Player* player, std::string const& line)
    {
        std::string text = std::string(SelfBotControl::ADDON_PREFIX) + "\t" + line.substr(0, ADDON_LINE_MAX);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    char const* RoleRu(std::string const& role)
    {
        return role == "tank" ? "tank" : role == "healer" ? "healer" : "dps";
    }

    // the user controls the character himself (no AI, or his SelfBot suspended): never commanded, waited for. A
    // character he handed to the bot (SelfBot active, UserPack bot) is a normal bot: no waiting (user 2026-11-07)
    bool IsHuman(Player* p) { return p && (IsRealPlayer(p) || (IsSelfBot(p) && SelfBotControl::IsSuspended(p))); }

    // [roles59] premade spec indexes (playerbots.conf PremadeSpecName.<class>.<n>, the pve ones) a role plays; -1: the
    // class has none. [roles62] a druid tank is feral in the bear template (Thick Hide from 20: AiFactory gives it
    // the "bear" strategy, the bear form a stance it takes in the fight).
    int8 RoleSpecIndex(uint8 cls, std::string const& role)
    {
        if (role == "healer")
            switch (cls)
            {
                case CLASS_PALADIN: return 0;  // holy pve
                case CLASS_PRIEST:  return 1;  // holy pve
                case CLASS_SHAMAN:  return 2;  // resto pve
                case CLASS_DRUID:   return 2;  // resto pve
                default:            return -1;
            }
        if (role == "tank")
            switch (cls)
            {
                case CLASS_WARRIOR:      return 2;  // prot pve
                case CLASS_PALADIN:      return 1;  // prot pve
                case CLASS_DEATH_KNIGHT: return 0;  // blood pve
                case CLASS_DRUID:        return 1;  // bear pve [roles62]
                default:                 return -1;
            }
        return -1;
    }

    // the talent tree a premade spec fills (the druid's cat spec 3 is the feral tree)
    uint8 SpecTree(uint8 cls, uint8 spec) { return cls == CLASS_DRUID && spec == 3 ? 1 : spec; }

    // its solo (questing) spec: the damage tree it levels in now, else the class's damage spec
    uint8 SoloSpecIndex(Player* p)
    {
        uint8 const cls = p->getClass();
        uint8 const tree = AiFactory::GetPlayerSpecTab(p);
        switch (cls)
        {
            case CLASS_WARRIOR:      return tree <= 1 ? tree : 1;           // arms / fury
            case CLASS_PALADIN:      return 2;                              // ret
            case CLASS_PRIEST:       return 2;                              // shadow
            case CLASS_SHAMAN:       return tree <= 1 ? tree : 1;           // ele / enh
            case CLASS_DRUID:        return tree == 0 ? 0 : 3;              // balance / cat
            case CLASS_DEATH_KNIGHT: return tree >= 1 ? tree : 2;           // frost / unholy
            default:                 return tree;
        }
    }

    std::string SpecName(uint8 cls, uint8 spec)
    {
        if (cls < MAX_CLASSES && spec < MAX_SPECNO && !sPlayerbotAIConfig.PremadeSpecName[cls][spec].empty())
            return sPlayerbotAIConfig.PremadeSpecName[cls][spec];
        return "spec " + std::to_string(spec);
    }

    bool Equipment(ItemTemplate const* proto)
    {
        return proto && (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON);
    }

    // the item's gain for the member: [loot12] the guild's one gear judge (GuildGear::EvaluateDrop, the verdict its
    // plan and its equip use). b456-b467: the roll asked "item upgrade" (REPLACE over any worn piece) while the equip
    // refused the armor type - Nurlian won Serpent Gloves twice, Aestymro Serpent's Shoulders 4 times, none worn;
    // a member that owns a copy is no need either. < 0: no upgrade
    float UpgradeGain(Player* m, ItemTemplate const* proto, int32 randomProperty)
    {
        if (!GET_PLAYERBOT_AI(m) || !proto)
            return -1.0f;
        GuildGear::Eval ev;
        if (!GuildGear::EvaluateDrop(m, proto, randomProperty, ev))
            return -1.0f;
        return ev.gain;
    }

    // [loot12] an owned item: whether the member's equip (the same judge) would wear it
    bool WouldWear(Player* m, Item* item)
    {
        uint8 slot = NULL_SLOT;
        float gain = 0.0f;
        std::string decision;
        return GET_PLAYERBOT_AI(m) && GuildGear::ShouldEquip(m, item, slot, gain, decision);
    }

    // [loot13] one loot row: item, reference, chance (percent), group
    using LootRow = std::tuple<uint32, uint32, float, uint8>;

    // the chance a kill drops this row (LootTemplate::Process): an explicit chance as it stands, a grouped row of
    // chance 0 shares what the group's explicit rows leave equally; 0: the row is not in the table
    float LootRowChance(std::set<LootRow> const& table, uint32 item, uint32 reference)
    {
        for (auto const& [rowItem, rowRef, chance, group] : table)
        {
            if (rowItem != item || rowRef != reference)
                continue;
            if (chance > 0.0f)
                return std::min(1.0f, chance / 100.0f);
            if (!group)
                return reference ? 1.0f : 0.0f;
            float explicitSum = 0.0f;
            uint32 equal = 0;
            for (auto const& [otherItem, otherRef, otherChance, otherGroup] : table)
                if (otherGroup == group)
                {
                    if (otherChance > 0.0f)
                        explicitSum += otherChance;
                    else
                        ++equal;
                }
            return equal ? std::max(0.0f, 100.0f - explicitSum) / 100.0f / float(equal) : 0.0f;
        }
        return 0.0f;
    }

    bool CanDisenchant(Player* p, ItemTemplate const* proto)
    {
        return p && proto && proto->DisenchantID && proto->Quality >= ITEM_QUALITY_UNCOMMON &&
               p->HasSkill(SKILL_ENCHANTING) && p->GetPureSkillValue(SKILL_ENCHANTING) >= proto->RequiredDisenchantSkill &&
               p->HasSpell(SPELL_DISENCHANT);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// [next1] the five's capacity against the dungeon (user rule: the leader computes from game math, no level floor)
// ---------------------------------------------------------------------------------------------------------------------
namespace FiveGate
{
    // One spawn as its template makes it (Creature::UpdateEntry / the stat system: creature_classlevelstats at the
    // template's top level, the template's health and damage modifiers), the melee as SoloSafety::RunPastTemplate
    // reads it: weapon damage base .. 1.5 x base plus attack power / 14 per second of swing.
    struct Mob
    {
        uint32 entry = 0;
        std::string name;
        uint8 level = 1;
        float health = 0.0f;
        float melee = 0.0f;     // damage per second before the tank's armor and its attack table
        float weaponHit = 0.0f;  // [gate20] its average swing, before armor: the shield blocks a flat amount of it
        bool boss = false;
        // [dun244] the planner's spell model of its template (PullPlanning::TemplateSpellDps): its spells per second,
        // the share of the fight it casts (no swing meanwhile), its weapon specials; a caster that keeps its range
        // lands its spells only
        float spellDps = 0.0f;
        float castShare = 0.0f;
        float weaponDps = 0.0f;
        bool keepsRange = false;
        // [dm3] what its script summons into the fight, the planner's own stream
        // (PullPlanning::TemplateSummonStream, [lead260]): `summonOnce` come once, `summonPerSec` is the repeating
        // in-combat timer's rate per second of fight, `summonEntry` what comes while it is one kind (0: unknown or
        // several, read as an average mob of the pack scaled by the shares)
        float summonOnce = 0.0f;
        float summonPerSec = 0.0f;
        uint32 summonEntry = 0;
        float summonHealthShare = 1.0f;
        float summonDamageShare = 1.0f;
    };

    // [dm3] one creature template read as the gate's Mob - the spawn loader's own reading, shared with the summons a
    // pull's scripts bring (judge below prices those from their own templates, as it prices every spawn). False: the
    // template or its creature_classlevelstats row is missing.
    bool MobOfTemplate(uint32 entry, Mob& out)
    {
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        CreatureBaseStats const* stats =
            tmpl ? sObjectMgr->GetCreatureBaseStats(tmpl->maxlevel, tmpl->unit_class) : nullptr;
        if (!stats)
            return false;
        out.entry = tmpl->Entry;
        out.name = tmpl->Name;
        out.level = tmpl->maxlevel;
        out.health = float(stats->GenerateHealth(tmpl));
        float const swing = std::max(float(tmpl->BaseAttackTime), 1.0f) / float(IN_MILLISECONDS);
        float const base = stats->GenerateBaseDamage(tmpl);
        // [gate20] its average swing before armor (the shield blocks a flat amount of it), and that over its swing
        out.weaponHit =
            ((base + base * 1.5f) / 2.0f + float(stats->AttackPower) / 14.0f * swing) * tmpl->DamageModifier;
        out.melee = out.weaponHit / swing;
        PullPlanning::TemplateSpellDps(out.entry, out.weaponHit, out.spellDps, out.castShare,
                                       out.weaponDps);  // [dun244]
        out.keepsRange = PullPlanning::KeepsRangeEntry(out.entry);  // [dun205]
        PullPlanning::TemplateSummonStream(out.entry, out.summonOnce, out.summonPerSec, out.summonEntry,
                                           out.summonHealthShare, out.summonDamageShare);
        return true;
    }

    // [dun170] a spawn where the core puts it and how it moves: a wanderer anywhere on its wander disc
    // (RANDOM_MOTION_TYPE, wander_distance), a waypoint patrol along its path (its formation leader's
    // creature_addon / creature_template_addon path_id, waypoint_data)
    struct Spawn
    {
        uint32 guid = 0;
        uint32 leader = 0;
        G3D::Vector3 pos;
        Mob mob;
        uint32 faction = 0;     // [gate94]
        bool noAssist = false;  // [gate94] CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE
        float wander = 0.0f;
        std::vector<G3D::Vector3> path;  // empty: it stands (or wanders) at its spawn point
        float callRadius = 0.0f;         // [dun191] its scripted call for help (the planner's ScriptedCallRadius)
        bool keepsRange = false;         // [dun205] it stands and casts where it is (the planner's KeepsRange)
        float detection = 0.0f;          // [dun205] its template's detection range (Creature::GetAggroRange's base)
        // [pack1] what the one pack model (LeaderPackModel.h) asks of a mob, read from its template instead of a
        // live creature: its model's sizes, its run speed, whether it is rooted or flies (the core's aggro Z gate),
        // whether it flees for assistance, whether it answers a call (Creature::CanAssistTo's own gates), and the
        // spawns its own script calls in
        float size = 0.0f;  // WorldObject::GetObjectSize: its model's combat reach, scaled
        float runSpeed = 0.0f;
        bool rooted = false;
        bool canFly = false;
        bool flees = false;
        bool respondsToCall = false;
        bool mayAssist = false;
        std::vector<uint32> adds;
    };

    // [dun170] what one pull of a spawn brings as the core brings it: its formation (CreatureGroup: every member
    // engages with the one engaged) and the mobs of its engage call (Creature::CallAssistance: within
    // CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS of it, of a faction friendly to it, not by a mob flagged
    // CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE; AssistDelayEvent switches the helpers' own call off: one hop)
    struct MapSpawns
    {
        std::vector<Spawn> spawns;
        std::vector<std::vector<uint32>> formation;  // per spawn: the other members of its formation
        std::vector<std::vector<std::vector<uint32>>> pathNear;  // per patrol, per path point: the standing spawns
                                                                 // its call reaches there
        std::vector<uint32> patrols;               // the spawns on a waypoint patrol
        std::map<uint32, uint32> indexOfGuid;      // [pack1] creature.guid -> its spawn (a script's called adds)
        std::vector<uint32> all;                   // [pack1] every spawn, the pack model's candidate list
        uint8 topLevel = 0;
        float meanHit = 0.0f;  // [gate20] the mean swing of its spawns, before armor (the shield's flat block)
    };

    std::map<uint32, MapSpawns> spawnsByMap;  // world thread (AutoFive, PreviewFive)

    bool Patrols(Spawn const& s) { return !s.path.empty(); }

    // the core's assistance rule (Acore::AnyAssistCreatureInRangeCheck / Creature::CanAssistTo): `helper` at
    // `helperAt` answers `caller`'s call at `callerAt` within the radius (a wanderer's wander radius on top: it may
    // stand anywhere in its disc) when it is friendly to the caller
    bool Answers(Spawn const& caller, G3D::Vector3 const& callerAt, Spawn const& helper, G3D::Vector3 const& helperAt,
                 float radius)
    {
        if (caller.noAssist || radius <= 0.0f ||
            (helperAt - callerAt).length() > radius + caller.wander + helper.wander)
            return false;
        if (caller.faction == helper.faction)
            return true;
        FactionTemplateEntry const* a = sFactionTemplateStore.LookupEntry(caller.faction);
        FactionTemplateEntry const* b = sFactionTemplateStore.LookupEntry(helper.faction);
        return a && b && b->IsFriendlyTo(*a);
    }

    MapSpawns const& Spawns(uint32 mapId)
    {
        if (auto const found = spawnsByMap.find(mapId); found != spawnsByMap.end())
            return found->second;
        MapSpawns& data = spawnsByMap[mapId];
        std::vector<Spawn>& spawns = data.spawns;

        std::set<uint32> credits;
        if (QueryResult result =
                WorldDatabase.Query("SELECT creditEntry FROM instance_encounters WHERE creditType = 0"))
            do
                credits.insert(result->Fetch()[0].Get<uint32>());
            while (result->NextRow());

        std::map<uint32, uint32> pathOf;  // spawn guid -> its waypoint path
        if (QueryResult result = WorldDatabase.Query(
                "SELECT c.guid, IFNULL(a.path_id, 0), IFNULL(t.path_id, 0) FROM creature c "
                "LEFT JOIN creature_addon a ON a.guid = c.guid "
                "LEFT JOIN creature_template_addon t ON t.entry = c.id WHERE c.map = {} AND c.MovementType = {}",
                mapId, uint32(WAYPOINT_MOTION_TYPE)))
            do
            {
                Field* f = result->Fetch();
                if (uint32 const path = f[1].Get<uint32>() ? f[1].Get<uint32>() : f[2].Get<uint32>())
                    pathOf[f[0].Get<uint32>()] = path;
            } while (result->NextRow());
        std::map<uint32, std::vector<G3D::Vector3>> paths;
        for (auto const& [guid, path] : pathOf)
        {
            if (paths.count(path))
                continue;
            std::vector<G3D::Vector3>& points = paths[path];
            if (QueryResult result = WorldDatabase.Query(
                    "SELECT position_x, position_y, position_z FROM waypoint_data WHERE id = {} ORDER BY point", path))
                do
                {
                    Field* f = result->Fetch();
                    points.emplace_back(f[0].Get<float>(), f[1].Get<float>(), f[2].Get<float>());
                } while (result->NextRow());
        }

        // [sfk246] no creature the five cannot fight, nor a holiday's spawn: b446 sfk-n's gate refused SFK on
        // "Slavering Worg x2 (L21-21, 6203 hp) ... heal mana 789 of 750 (1.05)" - the worg 16267 (1563 hp) and the
        // scripted Arugal 10000 2 yd from it (247103, 4640 hp, unit_flags IMMUNE_TO_PC | IMMUNE_TO_NPC: the Rethilgore
        // event's speaker, never a fight); Hallow's End's Sever 14682 (game_event_creature 120) stood in the list too
        uint32 skippedImmune = 0;
        uint32 skippedEvent = 0;
        if (QueryResult result = WorldDatabase.Query(
                "SELECT c.guid, c.id, c.position_x, c.position_y, c.position_z, IFNULL(f.leaderGUID, 0), "
                "c.MovementType, c.wander_distance, CAST(IFNULL((SELECT e.eventEntry FROM game_event_creature e "
                "WHERE e.guid = c.guid LIMIT 1), 0) AS SIGNED) FROM creature c "
                "LEFT JOIN creature_formations f ON f.memberGUID = c.guid WHERE c.map = {}",
                mapId))
        {
            do
            {
                Field* f = result->Fetch();
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(f[1].Get<uint32>());
                if (!tmpl || tmpl->type == CREATURE_TYPE_CRITTER ||
                    (tmpl->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE)))
                    continue;
                if (tmpl->unit_flags & UNIT_FLAG_IMMUNE_TO_PC)  // [sfk246]
                {
                    ++skippedImmune;
                    continue;
                }
                // [sfk246] a game event's spawn is in the world only while its event runs (a negative entry: only
                // while it does not)
                if (int64 const event = f[8].Get<int64>();
                    event && sGameEventMgr->IsActiveEvent(uint16(event > 0 ? event : -event)) != (event > 0))
                {
                    ++skippedEvent;
                    continue;
                }
                FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(tmpl->faction);
                if (!ft || !(ft->IsHostileToPlayers() || ft->IsHostileToAlliancePlayers()))
                    continue;
                CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(tmpl->maxlevel, tmpl->unit_class);
                if (!stats)
                    continue;
                Spawn s;
                s.guid = f[0].Get<uint32>();
                s.pos = G3D::Vector3(f[2].Get<float>(), f[3].Get<float>(), f[4].Get<float>());
                s.leader = f[5].Get<uint32>();
                s.faction = tmpl->faction;
                s.noAssist = (tmpl->flags_extra & CREATURE_FLAG_EXTRA_DONT_CALL_ASSISTANCE) != 0;
                if (f[6].Get<uint8>() == RANDOM_MOTION_TYPE)
                    s.wander = std::max(f[7].Get<float>(), 0.0f);
                // a formation member walks its leader's path
                if (auto const path = pathOf.find(s.leader ? s.leader : s.guid); path != pathOf.end())
                    s.path = paths[path->second];
                else if (auto const own = pathOf.find(s.guid); own != pathOf.end())
                    s.path = paths[own->second];
                if (!MobOfTemplate(tmpl->Entry, s.mob))  // [dm3]
                    continue;
                s.mob.boss = credits.count(tmpl->Entry) || tmpl->rank == CREATURE_ELITE_WORLDBOSS;
                data.meanHit += s.mob.weaponHit;  // [gate20]
                s.callRadius = PullPlanning::ScriptedCallRadius(tmpl->Entry);
                s.keepsRange = s.mob.keepsRange;
                s.detection = tmpl->detection_range;
                // [pack1] the template's reading of what the one pack model asks of a live creature
                CreatureModel const* model = tmpl->GetModelByIdx(0);
                CreatureModelInfo const* info =
                    model ? sObjectMgr->GetCreatureModelInfo(model->CreatureDisplayID) : nullptr;
                float const scale = model && model->DisplayScale > 0.0f ? model->DisplayScale : 1.0f;
                s.size = info ? info->combat_reach * scale : DEFAULT_WORLD_OBJECT_SIZE * scale;
                s.runSpeed = std::max(tmpl->speed_run * baseMoveSpeed[MOVE_RUN], 0.1f);
                s.rooted = tmpl->Movement.IsRooted();
                s.canFly = tmpl->Movement.IsFlightAllowed();
                s.flees = PullPlanning::FleesForHelpEntry(tmpl->Entry);
                s.respondsToCall = ft->FactionRespondsToCallForHelp();
                s.mayAssist = !(tmpl->flags_extra & (CREATURE_FLAG_EXTRA_CIVILIAN |
                                                     CREATURE_FLAG_EXTRA_IGNORE_ALL_ASSISTANCE_CALLS)) &&
                              !(tmpl->unit_flags & UNIT_FLAG_IMMUNE_TO_NPC);
                PullPlanning::ScriptedAddSpawns(tmpl->Entry, s.guid, s.adds);
                data.topLevel = std::max(data.topLevel, s.mob.level);
                spawns.push_back(std::move(s));
            } while (result->NextRow());
        }

        if (!spawns.empty())
            data.meanHit /= float(spawns.size());  // [gate20] the mean swing of the map
        // a formation whose leader walks no path stands: its members stand with it
        for (Spawn& s : spawns)
            if (s.leader && s.leader != s.guid && !pathOf.count(s.leader))
                s.path.clear();

        // [dun191] the planner's social radius (CollectPack: the core's assistance radius plus the fight's drift)
        // [pack1] what one pull brings is no longer precomputed here: the one pack model (LeaderPackModel.h) counts
        // it per pull, over these spawns. Only the formations (the core engages a group as one) and a patrol's
        // reach along its path stay - the latter is the gate's own route question (where a patrol can be taken on
        // its own), not a term of the pack.
        float const radius = PullPlanning::PackSocialRadius();
        size_t const n = spawns.size();
        data.formation.assign(n, {});
        data.pathNear.assign(n, {});
        data.all.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            data.all.push_back(uint32(i));
            data.indexOfGuid[spawns[i].guid] = uint32(i);
        }

        for (size_t i = 0; i < n; ++i)
        {
            Spawn const& s = spawns[i];
            for (size_t j = 0; j < n; ++j)
            {
                if (i == j)
                    continue;
                Spawn const& o = spawns[j];
                if ((s.leader && (o.leader == s.leader || o.guid == s.leader)) || (o.leader && o.leader == s.guid))
                    data.formation[i].push_back(uint32(j));
            }
            if (!Patrols(s))
                continue;
            data.patrols.push_back(uint32(i));
            for (G3D::Vector3 const& point : s.path)
            {
                std::vector<uint32>& inReach = data.pathNear[i].emplace_back();
                for (size_t j = 0; j < n; ++j)
                {
                    Spawn const& o = spawns[j];
                    if (j == i || Patrols(o) ||
                        std::find(data.formation[i].begin(), data.formation[i].end(), uint32(j)) !=
                            data.formation[i].end())
                        continue;
                    if (Answers(s, point, o, o.pos, radius))
                        inReach.push_back(uint32(j));
                }
            }
        }
        size_t callers = 0;
        for (Spawn const& s : spawns)
            if (s.callRadius > 0.0f)
                ++callers;
        LOG_INFO("playerbots",
                 "[dun170] five gate: map {} - {} hostile spawns, {} of them on a waypoint patrol, [dun191] {} with a "
                 "scripted call for help, social radius {:.1f}; [sfk246] left out: {} immune to players, {} of a game "
                 "event not running",
                 mapId, n, data.patrols.size(), callers, radius, skippedImmune, skippedEvent);
        return data;
    }

    uint8 TopLevel(uint32 mapId) { return Spawns(mapId).topLevel; }

    // [pack1] The five gate's reading of the one pull-pack model (LeaderPackModel.h): the map's spawn rows, before a
    // five is anywhere near the instance. Every term the model asks for - the formations, the engage call, what the
    // spot and the walk in wake, the scripted calls for help, the assistance chain with its hops, the fight a mob
    // that keeps its range draws, the flee-for-assistance race - is answered from the `creature` table and the
    // templates, so the gate and the leader count the same pull. Two terms need a live world and get the closest
    // spawn reading instead, named on the gate's own why line:
    //  - Sight: there is no instance to ray-cast against. The static vmap tiles load with an instance's grids, and
    //    loading them from the world thread would race every map thread reading the same shared tree, so sight is
    //    read as the core's own aggro height gate (Creature::CanStartAttack: CREATURE_Z_ATTACK_RANGE over the two
    //    object sizes). The floors of a room are separated; a wall inside one floor is not, so a link the geometry
    //    blocks is still counted - the gate stays on the safe side of the planner there.
    //  - The walk round a corner (the planner's navmesh legs, [b34_est]) and the Scarlet Monastery chapel's scripted
    //    call: no navmesh query and no live script run here, so neither adds to a gate count. The straight walk in
    //    is counted (OnPullPath).
    class SpawnPackWorld
    {
    public:
        using Mob = uint32;

        // the core's aggro radius terms (Creature::GetAggroRange) and its Z gate
        static constexpr int32 AGGRO_MIN_LEVEL_DIFF = -25;  // the 45 yd cap's level difference
        static constexpr float AGGRO_MIN_RANGE = 5.0f;      // a mob's aggro radius is never below combat range

        SpawnPackWorld(MapSpawns const& map, std::vector<char> const& left, std::vector<char> const& apart,
                       G3D::Vector3 const& from, bool const& walked, uint8 lowestLevel, float memberSize,
                       float groupDps)
            : _map(map), _left(left), _apart(apart), _from(from), _walked(walked), _lowestLevel(lowestLevel),
              _memberSize(memberSize), _groupDps(std::max(groupDps, 1.0f)),
              _aggroRate(sWorld->getRate(RATE_CREATURE_AGGRO)),
              _assistRadius(sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS)),
              _fleeRadius(sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS))
        {
        }

        // one pull: the spawns crowd control holds out of it, and a fresh count by cause for its why line
        void Take(std::vector<char> const* held)
        {
            _held = held;
            _counts.fill(0);
        }

        static Mob NoMob() { return std::numeric_limits<Mob>::max(); }

        std::vector<Mob> const& Possible() const { return _map.all; }
        bool IsIdle(Mob mob) const { return _left[mob] != 0; }
        bool IsHeld(Mob mob) const { return _held && (*_held)[mob] != 0; }
        // the spawn loader already left out every creature the five cannot fight ([sfk246])
        bool IsSealed(Mob /*mob*/) const { return false; }
        bool IsBoss(Mob mob) const { return _map.spawns[mob].mob.boss; }
        bool Chains(Mob /*mob*/) const { return true; }  // every row of `creature` is a creature

        Position At(Mob mob) const
        {
            G3D::Vector3 const& at = _map.spawns[mob].pos;
            return Position(at.x, at.y, at.z);
        }

        float Dist(Mob mob, Position const& at) const { return At(mob).GetExactDist(at); }
        float Dist(Mob mob, Mob other) const { return At(mob).GetExactDist(At(other)); }

        // WorldObject::IsWithinDistInMap: both object sizes count toward the radius
        bool WithinDist(Mob mob, Mob other, float dist) const
        {
            return Dist(mob, other) <= dist + _map.spawns[mob].size + _map.spawns[other].size;
        }

        float Size(Mob mob) const { return _map.spawns[mob].size; }

        // the sight stand-in: the core's aggro height gate against a member standing at `at` (the eye height a real
        // line of sight would add is no part of a height gate)
        bool Sees(Mob mob, Position const& at, float /*eye*/) const
        {
            return HeightGate(_map.spawns[mob], _map.spawns[mob].pos.z, at.GetPositionZ(), _memberSize);
        }

        bool SeesMob(Mob from, Mob to) const
        {
            return HeightGate(_map.spawns[to], _map.spawns[to].pos.z, _map.spawns[from].pos.z,
                              _map.spawns[from].size);
        }

        bool CallsOnEngage(Mob mob) const { return !_map.spawns[mob].noAssist; }

        // Creature::CanAssistTo with its faction check: the same faction exactly, a faction template that answers a
        // call for help, and none of the flags that keep a mob out of one (civilian, immune to NPCs, ignores every
        // assistance call). Its sight of the enemy is the height gate above, where the model asks for it.
        bool CanAssist(Mob helper, Mob caller) const
        {
            Spawn const& h = _map.spawns[helper];
            return h.mayAssist && h.respondsToCall && h.faction == _map.spawns[caller].faction;
        }

        // [lead1331] with a wanderer on either side, the gap between the wander discs; otherwise the gap between the
        // two mobs (Unit::GetDistance: both object sizes off the straight line)
        void SocialLink(Mob mob, Mob target, float /*radius*/, float& gap, bool& sees) const
        {
            Spawn const& m = _map.spawns[mob];
            Spawn const& t = _map.spawns[target];
            float const straight = Dist(mob, target);
            gap = m.wander > 0.0f || t.wander > 0.0f ? std::max(0.0f, straight - m.wander - t.wander)
                                                     : std::max(0.0f, straight - m.size - t.size);
            sees = HeightGate(m, m.pos.z, t.pos.z, t.size);
        }

        void Formation(Mob mob, std::vector<Mob>& out) const { out = _map.formation[mob]; }

        // the leader walks in from the pull before this one: a mob whose aggro reach comes within that straight walk
        // is pulled on the way and comes back with the pack (the planner's IsOnPullPath)
        bool OnPullPath(Position const& to, Mob mob) const
        {
            if (!_walked)
                return false;  // the run's first pull: the gate knows no walk in to it

            Spawn const& s = _map.spawns[mob];
            float const reach = AggroReach(mob);
            if (reach <= 0.0f)
                return false;

            float const dx = to.GetPositionX() - _from.x;
            float const dy = to.GetPositionY() - _from.y;
            float const lengthSq = dx * dx + dy * dy;
            float t = 0.0f;
            if (lengthSq > 0.0f)
                t = std::clamp(((s.pos.x - _from.x) * dx + (s.pos.y - _from.y) * dy) / lengthSq, 0.0f, 1.0f);

            float const px = _from.x + dx * t;
            float const py = _from.y + dy * t;
            float const pz = _from.z + (to.GetPositionZ() - _from.z) * t;
            return At(mob).GetExactDist(px, py, pz) <= reach + s.wander &&
                   HeightGate(s, s.pos.z, pz, _memberSize);
        }

        // An idle mob whose aggro range reaches the spot the five fights at, a wanderer whose disc brings it there,
        // or a patrol whose beat walks into it. A patrol the leader takes apart (it has a point on its beat out of
        // every standing mob's call) is pulled on its own and joins no other pull.
        bool ReachesSpot(Mob mob, Position const& spot, float drift) const
        {
            Spawn const& s = _map.spawns[mob];
            float const reach = AggroReach(mob);
            if (reach <= 0.0f)
                return false;

            if (Patrols(s))
            {
                if (_apart[mob])
                    return false;

                // The planner's forecast of where a patrol walks next needs a live walker. The gate has the whole
                // beat and the planner's own test for a walk it cannot forecast (PathCrossesSpot): a leg of the beat
                // within the mob's aggro reach of the spot and at the spot's height, with the spawn no further than
                // CAREFUL_PATROL_RANGE - the beat alone counted a patrol into every pull anywhere along it.
                if (Dist(mob, spot) > PullPlanning::CAREFUL_PATROL_RANGE ||
                    std::fabs(s.pos.z - spot.GetPositionZ()) > PullPlanning::PULL_PATH_HEIGHT)
                    return false;

                for (size_t p = 0; p < s.path.size(); ++p)
                {
                    G3D::Vector3 const& from = s.path[p];
                    G3D::Vector3 const& to = s.path[(p + 1) % s.path.size()];
                    if (std::fabs(from.z - spot.GetPositionZ()) > PullPlanning::PULL_PATH_HEIGHT &&
                        std::fabs(to.z - spot.GetPositionZ()) > PullPlanning::PULL_PATH_HEIGHT)
                        continue;

                    if (LegDistance2d(spot, from, to) <= reach)
                        return true;
                }

                return false;
            }

            float const extra = s.wander > 0.0f ? s.wander : drift;
            return Dist(mob, spot) <= reach + extra && HeightGate(s, s.pos.z, spot.GetPositionZ(), _memberSize);
        }

        PullPlanning::PackWhyKind SpotKind(Mob mob) const
        {
            Spawn const& s = _map.spawns[mob];
            if (Patrols(s))
                return PullPlanning::PackWhyKind::SpotPatrol;

            return s.wander > 0.0f ? PullPlanning::PackWhyKind::SpotWander : PullPlanning::PackWhyKind::SpotAggro;
        }

        bool KeepsRange(Mob mob) const { return _map.spawns[mob].keepsRange; }
        bool IsRooted(Mob mob) const { return _map.spawns[mob].rooted; }

        // a patrol whose beat has not brought it to the fight yet engages (and calls) where it arrives, which the
        // spot test above counts; not from the pack it passes on the way
        bool Arriving(Mob mob, Position const& kill) const
        {
            Spawn const& s = _map.spawns[mob];
            if (!Patrols(s) || s.mob.boss)
                return false;

            return Dist(mob, kill) > AggroReach(mob) + PullPlanning::CAREFUL_PATROL_MARGIN;
        }

        // the planner's split mode (a group crossing the pack now) is a live question: the gate judges the dungeon
        // as the leader pulls it, one pull at a time, with nothing walking past
        bool WalksBy(Mob /*mob*/, Position const& /*spot*/) const { return false; }
        bool Crossing(Mob /*bringer*/, Position const& /*anchor*/, Mob /*mob*/, float /*radius*/) const
        {
            return false;
        }
        bool WouldCross(Position const& /*from*/, Mob /*mob*/, float /*radius*/) const { return false; }
        void NoteCross(Position const& /*from*/, Mob /*mob*/, float /*radius*/) const {}

        float AssistRadius() const { return _assistRadius; }
        float FleeRadius() const { return _fleeRadius; }
        float CallRadius(Mob mob) const { return _map.spawns[mob].callRadius; }

        // Creature::CallForHelp of `caller` fighting at `from`: a friendly idle mob within the radius that sees the
        // fight. The call comes late in the fight, so a wanderer counts anywhere within its disc.
        bool CalledForHelp(Mob caller, Position const& from, float radius, Mob mob) const
        {
            if (radius <= 0.0f)
                return false;

            Spawn const& m = _map.spawns[mob];
            Spawn const& c = _map.spawns[caller];
            if (Dist(mob, from) > radius + m.wander || !m.mayAssist)
                return false;

            bool friendly = m.faction == c.faction;
            if (!friendly)
            {
                FactionTemplateEntry const* a = sFactionTemplateStore.LookupEntry(m.faction);
                FactionTemplateEntry const* b = sFactionTemplateStore.LookupEntry(c.faction);
                friendly = a && b && a->IsFriendlyTo(*b);
            }

            return friendly && HeightGate(m, m.pos.z, from.GetPositionZ(), _memberSize);
        }

        void ScriptedAdds(Mob mob, std::vector<Mob>& out) const
        {
            for (uint32 guid : _map.spawns[mob].adds)
            {
                auto const found = _map.indexOfGuid.find(guid);
                if (found != _map.indexOfGuid.end() && IsIdle(found->second))
                    out.push_back(found->second);
            }
        }

        bool WalkLegs(Mob /*mob*/, Position const& /*kill*/,
                      std::vector<std::pair<G3D::Vector3, G3D::Vector3>>& /*legs*/) const
        {
            return false;  // no navmesh query here: see the note above the class
        }

        bool ScriptedCall(Position const& /*spot*/, std::vector<Mob> const& /*pack*/, Mob& /*lead*/,
                          std::vector<Mob>& /*called*/) const
        {
            return false;  // no live script here: see the note above the class
        }

        bool FleesForHelp(Mob mob) const { return _map.spawns[mob].flees; }
        float MaxHealth(Mob mob) const { return _map.spawns[mob].mob.health; }
        float RunSpeed(Mob mob) const { return _map.spawns[mob].runSpeed; }
        float GroupDps() const { return _groupDps; }
        float MeleeEdge() const { return FightHold::RADIUS + FightHold::MELEE_SLACK; }
        bool RecordsWhy() const { return true; }

        void NoteWhy(Mob /*mob*/, Mob /*source*/, PullPlanning::PackWhyKind kind, float /*distance*/,
                     float /*radius*/, uint8 step, float /*reachSec*/, float /*killSec*/)
        {
            if (step)
                ++_counts[size_t(PullPlanning::PackWhyKind::FleeHelper)];
            else if (size_t(kind) < _counts.size())
                ++_counts[size_t(kind)];
        }

        void NoteSkipped(Mob /*helper*/, Mob /*fleer*/, PullPlanning::PackWhyKind /*kind*/, float /*distance*/,
                         float /*radius*/, uint8 /*step*/, float /*reachSec*/, float /*killSec*/)
        {
        }

        void Done(std::vector<Mob> const& /*pack*/, std::vector<uint8> const& /*hops*/) const {}

        // the pull's count by cause, in the planner's own words ("[pack why]"): the one line that says why the gate
        // priced this pack the way it did
        std::string WhyLine() const
        {
            static char const* const groups[] = {"start", "social", "pull path", "aggro reaches spot",
                                                 "wanders to spot", "patrol walks in", "kill spot path", "kill spot",
                                                 "formation", "assist", "call for help", "draws fight",
                                                 "scripted add", "scripted call", "flee chain", "flee miss",
                                                 "patrol forecast"};
            static_assert(std::size(groups) == size_t(PullPlanning::PackWhyKind::PatrolAdded) + 1);
            std::string line;
            for (size_t i = 0; i < std::size(groups); ++i)
            {
                if (_counts[i])
                    line += Acore::StringFormat("{}{} {}", line.empty() ? "" : " + ", _counts[i], groups[i]);
            }

            return line;
        }

    private:
        // the planner's SegmentDistance2d: how close one leg of a beat comes to the spot
        static float LegDistance2d(Position const& spot, G3D::Vector3 const& from, G3D::Vector3 const& to)
        {
            float const dx = to.x - from.x;
            float const dy = to.y - from.y;
            float const lengthSq = dx * dx + dy * dy;
            float t = 0.0f;
            if (lengthSq > 0.0f)
                t = std::clamp(((spot.GetPositionX() - from.x) * dx + (spot.GetPositionY() - from.y) * dy) / lengthSq,
                               0.0f, 1.0f);

            return spot.GetExactDist2d(from.x + dx * t, from.y + dy * t);
        }

        // MovementAction::PassesAggroZGate: a mob never reaches what stands more than CREATURE_Z_ATTACK_RANGE above
        // or below it, over the two object sizes (its combat distance is 0 for a spawn no script has changed)
        static bool HeightGate(Spawn const& s, float fromZ, float toZ, float otherSize)
        {
            if (s.canFly)
                return true;

            return std::max(0.0f, std::fabs(fromZ - toZ) - (s.size + otherSize)) <= float(CREATURE_Z_ATTACK_RANGE);
        }

        // Creature::GetAggroRange against the five's lowest member, plus the combat distance and the fight's drift
        // (MovementAction::IdleAggroReach)
        float AggroReach(Mob mob) const
        {
            Spawn const& s = _map.spawns[mob];
            if (_aggroRate <= 0.0f || s.detection < 1.0f)
                return 0.0f;

            int32 levelDiff = std::max(int32(_lowestLevel) - int32(s.mob.level), AGGRO_MIN_LEVEL_DIFF);
            float radius = std::min(s.detection - float(levelDiff), MAX_AGGRO_RADIUS);
            return std::max(radius, AGGRO_MIN_RANGE) * _aggroRate + PullPlanning::CAREFUL_AGGRO_MARGIN;
        }

        MapSpawns const& _map;
        std::vector<char> const& _left;
        std::vector<char> const& _apart;
        G3D::Vector3 const& _from;
        bool const& _walked;
        std::vector<char> const* _held = nullptr;
        uint8 _lowestLevel;
        float _memberSize;
        float _groupDps;
        float _aggroRate;
        float _assistRadius;
        float _fleeRadius;
        std::array<uint16, size_t(PullPlanning::PackWhyKind::PatrolAdded) + 1> _counts = {};
    };

    // The core's spell miss chance against a mob `diff` levels above the caster (3.3.5: 4 / 5 / 6 %, then +11 % per
    // level), as a factor on the damage of a mob of its own level (DpsEstimate's target)
    float HitFactor(int32 diff)
    {
        if (diff <= 0)
            return 1.0f;
        float const miss = diff <= 2 ? 4.0f + float(diff) : 6.0f + 11.0f * float(diff - 2);
        return std::max(0.0f, 1.0f - std::min(miss, 99.0f) / 100.0f) / 0.96f;
    }

    // Unit::CalcArmorReducedDamage for an attacker below level 60
    float ArmorReduction(float armor, uint8 level)
    {
        float const lvl = level > 59 ? float(level) + 4.5f * float(level - 59) : float(level);
        return std::clamp(armor / (armor + 85.0f * lvl + 400.0f), 0.0f, 0.75f);
    }

    // [order368] [gate20] the tank term of the drop on this map: the health a mob of the map's top level has to take
    // off the tank for the drop the gate prices - its armor and the whole attack table (miss, dodge, parry, the
    // shield's flat block against the map's mean swing, crushing blows, crits), a druid tank as the bear the planner
    // plans with. Higher is a lower drop: the carrier order of a first-clear five reads it. Not the whole verdict -
    // the pull's mobs and the healer set that - but the one term the tank changes
    float TankDropIndex(uint32 mapId, Player* tank, std::string* parts = nullptr)
    {
        if (!tank)
            return 0.0f;
        MapSpawns const& map = Spawns(mapId);
        float health = float(tank->GetMaxHealth());
        float armor = float(tank->GetArmor());
        float dodge = tank->GetUnitDodgeChance();
        float parry = tank->GetUnitParryChance();
        PullPlanning::BearStatsSeen(tank, health, armor, dodge, parry);
        uint8 const level = map.topLevel ? map.topLevel : uint8(tank->GetLevel());
        float const armorKeeps = 1.0f - ArmorReduction(armor, level);
        float const share = PullPlanning::TemplateTakenShare(0, level, tank, map.meanHit * armorKeeps, parts);
        return share > 0.0f ? health / (armorKeeps * share) : health * 100.0f;
    }

    struct Verdict
    {
        bool clears = true;
        std::string why;
        // [dun194] each mana user's net mana over the dungeon's pulls (the healer's healing mana less its regen in
        // the fights, a caster's rotation less its regen in combat), by its GUID
        std::map<ObjectGuid, float> manaNeed;
        // [dun230] the terms of the healer's: the healing's mana over the run, its regen in the fights, and each mana
        // user's regen on the walks between the pulls (taken off its need)
        float healingMana = 0.0f;
        float fightRegen = 0.0f;
        std::map<ObjectGuid, float> walkRegen;
        // [assist22] the margin: the worst pull's load and its terms (the tank's drop, the healer's mana), the pulls
        // over capacity of those judged
        float worstLoad = 0.0f;
        float worstDrop = 0.0f;
        float worstMana = 0.0f;
        uint32 over = 0;
        uint32 judged = 0;
    };

    // The five: the tank first, the healer second (PickFive's order). [dun170] The dungeon is pulled as its leader
    // pulls it: one pull at a time, the cheapest pull the mobs still standing allow first (the planner's side pull /
    // split: what one spawn's pull brings, MapSpawns), every pull from full health and mana (the leader's Rest
    // between pulls), a waypoint patrol on its own where its walk takes it out of every standing mob's call (the
    // planner waits a crossing patrol out or pulls it first), else with every pull its path reaches.
    // Each pull is judged as the planner judges a pull "as it comes": the group's damage (DpsEstimate, each member
    // against a mob of its level, the spell miss of the mobs' levels) kills the mobs one by one, weakest first; every
    // living one lands its melee on the tank (its armor, dodge and parry; a druid tank out of form as the bear the
    // planner plans with); the healer's best rate (RestState::HealerThroughput) heals after its first gap. Clears
    // when, in every pull, the tank's deepest drop leaves it one more heal gap at the pull's full rate (the planner's
    // maxDrop) and the healer's mana covers the healing. Not counted: the mobs' spells, area damage on the group.
    // [order367] healer2: a second healer of the five (one of its members past the first two), its heal added to the
    // first one's
    Verdict Judge(uint32 mapId, std::vector<Player*> const& five, Player* healer2 = nullptr)
    {
        std::string const gateName = "map " + std::to_string(mapId);
        SlowScope slow("five gate judge", &gateName);  // [perf211] a world-thread look's cost, named when slow
        Verdict out;
        if (five.size() < 2 || !five[0] || !five[1])
        {
            out.clears = false;
            out.why = "no tank / healer";
            return out;
        }
        Player* tank = five[0];
        Player* healer = five[1];
        // each member's burst, what lasts with no mana and the seconds its mana carries the burst (DpsEstimate on its
        // state now): a pull of T seconds gets burst x min(T, oom) + white x the rest, over T
        struct Damage
        {
            ObjectGuid guid;
            float manaPerSec = 0.0f;  // [dun194] its rotation's mana less its regen in combat
            float walkRegen = 0.0f;   // [dun230] its full regen per second on the walk between pulls
            float lastUsed = 0.0f;    // [dun230] what the pull before took of its pool
            uint8 level = 1;
            float burst = 0.0f;
            float white = 0.0f;
            float oomSec = 0.0f;
            float Over(float sec) const
            {
                if (sec <= 0.0f || sec <= oomSec)
                    return burst;
                return (burst * oomSec + white * (sec - oomSec)) / sec;
            }
        };
        std::vector<Damage> dps;
        float groupDps = 0.0f;
        for (Player* p : five)
            if (p)
            {
                DpsEstimate::Result const r = DpsEstimate::Estimate(p, nullptr, DpsEstimate::BuffContext::Group);
                Damage d;
                d.guid = p->GetGUID();
                d.manaPerSec = p->GetMaxPower(POWER_MANA) ? std::max(r.manaPerSec, 0.0f) : 0.0f;
                d.walkRegen = RestState::ManaRegenPerSec(p, false);
                d.level = p->GetLevel();
                d.burst = r.burstDps;
                d.white = r.whiteDps;
                d.oomSec = r.oomSec;
                // [dps242] the five's healer heals: its damage is no part of the kill times (b436 / b437 fives: the
                // healers dealt 0.0-0.7 dps in their pulls against 22-48 by the formula, 0.01 of it); its entry stays
                // for its mana ([dun194])
                if (p == healer || p == healer2)
                    d.burst = d.white = 0.0f;
                dps.push_back(d);
                groupDps += d.burst;
            }
        RestState::HealerFacts heal = RestState::HealerThroughput(healer, tank);
        // [order367] two healers: their rates and pools add up, the hp per mana is the pair's at both rates, the first
        // heal comes after the slower one's gap, and a control cast takes out the stronger one's rate for its seconds
        float controlledHps = 0.0f;
        float healerShare = 1.0f;  // the first healer's share of the healing mana
        if (healer2)
        {
            RestState::HealerFacts const second = RestState::HealerThroughput(healer2, tank);
            float const manaRate = (heal.healPerMana > 0.0f ? heal.hps / heal.healPerMana : 0.0f) +
                                   (second.healPerMana > 0.0f ? second.hps / second.healPerMana : 0.0f);
            healerShare = heal.hps + second.hps > 0.0f ? heal.hps / (heal.hps + second.hps) : 1.0f;
            controlledHps = std::max(heal.hps, second.hps);
            heal.hps += second.hps;
            heal.healPerMana = manaRate > 0.0f ? heal.hps / manaRate : 0.0f;
            heal.maxMana += second.maxMana;
            heal.gapSec = std::max(heal.gapSec, second.gapSec);
        }
        // [dun172] a druid tank out of (Dire) Bear Form is judged as the bear the planner plans with
        float tankHealth = float(tank->GetMaxHealth());
        float armor = float(tank->GetArmor());
        float dodge = tank->GetUnitDodgeChance();
        float parry = tank->GetUnitParryChance();
        bool const bear = PullPlanning::BearStatsSeen(tank, tankHealth, armor, dodge, parry);
        float const avoid = std::clamp((dodge + parry) / 100.0f, 0.0f, 0.75f);
        float const healMana = heal.maxMana;
        float const gap = std::max(heal.gapSec, 1.5f);
        // [dun230] without a food / drink aura's MP5 (both fields carry it while the healer sits): b443 / b444 wc-n
        // admitted on a drinking healer's 36 mana/s ("heal mana 3079 of 4514 (770 pool + regen)"); standing, the same
        // pulls read "3012 of 939" - 3.2 times its mana, over capacity
        float const regenCasting =
            RestState::ManaRegenPerSec(healer, true) + (healer2 ? RestState::ManaRegenPerSec(healer2, true) : 0.0f);
        float const regenFull =
            RestState::ManaRegenPerSec(healer, false) + (healer2 ? RestState::ManaRegenPerSec(healer2, false) : 0.0f);
        // [dun230] the walk between two pulls: the straight line from one pull's first mob to the next one's at the
        // tank's run speed (a path is never shorter, so the regen on it is never overcounted)
        float const runSpeed = std::max(tank->GetSpeed(MOVE_RUN), 1.0f);
        G3D::Vector3 lastPull;
        bool walked = false;

        // one pull's load (1.0: just fits) and its terms
        struct PullLoad
        {
            float load = 0.0f;
            float health = 0.0f;
            float sec = 0.0f;
            float deepest = 0.0f;
            float gapDrop = 0.0f;
            std::string parts;  // [gate20] the attack table of its heaviest mob, logged
            float dropShare = 0.0f;
            float manaNeeded = 0.0f;
            float fightMana = 0.0f;
            float manaShare = 0.0f;
            float controlSec = 0.0f;  // [dun261] the healer's seconds under the mobs' control casts
            uint8 lo = 255;
            uint8 hi = 0;
            size_t mobs = 0;
            std::string name;
            bool boss = false;
            std::string summons;  // [dm3] the stream its scripts bring, for the why line
            // [dm5] a boss pull's own terms (the planner's JudgeMobs): what the pack lands per second, and that
            // against the healers' rate. `peakShare` is 0 on a trash pull, which the drop term holds instead
            float landedPerSec = 0.0f;
            float peakShare = 0.0f;
        };
        // [gate20] the attack table of a template against this tank, once per entry (the pulls read it again and again)
        std::map<uint32, std::pair<float, std::string>> shareOf;
        auto const takenShare = [&](Mob const& m, float armorKeeps) -> float
        {
            auto const found = shareOf.find(m.entry);
            if (found != shareOf.end())
                return found->second.first;
            std::string parts;
            float const share =
                PullPlanning::TemplateTakenShare(m.entry, m.level, tank, m.weaponHit * armorKeeps, &parts);
            shareOf[m.entry] = {share, m.name + ": " + parts};
            return share;
        };
        // [dm3] the mob one summoner's stream brings, built once per summoner entry (the pulls read it again and
        // again): its own template when the script names one kind, else the summoner scaled by the planner's shares.
        // A summon carries no stream of its own, as the planner's lives do not.
        std::map<uint32, Mob> summonCache;
        auto const summonOf = [&summonCache](Mob const& summoner) -> Mob const&
        {
            auto const found = summonCache.find(summoner.entry);
            if (found != summonCache.end())
                return found->second;
            Mob summon;
            if (!summoner.summonEntry || !MobOfTemplate(summoner.summonEntry, summon))
            {
                summon = summoner;
                summon.name += " (summon)";
                summon.health *= summoner.summonHealthShare;
                summon.melee *= summoner.summonDamageShare;
                summon.weaponHit *= summoner.summonDamageShare;
                summon.weaponDps *= summoner.summonDamageShare;
                summon.spellDps *= summoner.summonDamageShare;
            }
            summon.summonOnce = 0.0f;
            summon.summonPerSec = 0.0f;
            summon.summonEntry = 0;
            summon.boss = false;
            return summonCache.emplace(summoner.entry, std::move(summon)).first->second;
        };
        auto const judge = [&](std::vector<Mob>& mobs, bool boss) -> PullLoad
        {
            std::sort(mobs.begin(), mobs.end(), [](Mob const& a, Mob const& b) { return a.health < b.health; });
            // [dun244] as the planner's LandedMeleeDps: a caster at range lands its spells, the others their swings
            // between casts and their weapon specials through the tank's armor and avoidance, plus their spells
            auto landed = [&](Mob const& m)
            {
                if (m.keepsRange && m.spellDps > 0.0f)
                    return m.spellDps;
                // [gate20] user rule: the game's own mitigation - its armor, then the attack table the core rolls
                // (a blocked swing loses the shield's flat value, a crushing blow lands 150 %, a crit 200 %)
                float const armorKeeps = 1.0f - ArmorReduction(armor, m.level);
                return (m.melee * (1.0f - m.castShare) + m.weaponDps) * armorKeeps * takenShare(m, armorKeeps) +
                       m.spellDps;
            };
            PullLoad result;
            float rate = 0.0f;
            // [dm3] b474 dm-n: the gate priced the Deadmines foundry as "Goblin Engineer x6 (9639 hp): fight 83-101 s"
            // while the planner counted 35 mobs there - each Goblin Engineer casts Summon Remote-Controlled Golem
            // (3605) every 35 s it fights. The summons a pull's scripts bring are the planner's own stream
            // (PullPlanning::TemplateSummonStream, [lead260]: what comes once plus the repeating timer's rate over the
            // pack's own kill time, as EstimatePack counts it), each priced from its own template like every spawn;
            // one whose kind the script does not name is an average mob of the pack scaled by the planner's shares
            // ([b36_scholo]). A summon brings none of its own: the planner's lives carry no stream either.
            {
                float packHealth = 0.0f;
                for (Mob const& m : mobs)
                    packHealth += m.health;
                if (!mobs.empty())
                    result.name = mobs.front().name;  // the pull is named by its own lightest mob, never a summon
                float const packSec = groupDps > 0.0f ? packHealth / groupDps : 0.0f;
                size_t const own = mobs.size();
                std::map<std::string, uint32> streams;  // what one summoner brings -> how many over this fight
                for (size_t i = 0; i < own; ++i)
                {
                    // `mobs` grows below, so the summoner is read by value: a reference into it would dangle
                    Mob const summoner = mobs[i];
                    if (summoner.summonOnce <= 0.0f && summoner.summonPerSec <= 0.0f)
                        continue;
                    uint32 const count = uint32(std::lround(summoner.summonOnce + summoner.summonPerSec * packSec));
                    if (!count)
                        continue;
                    Mob const& summon = summonOf(summoner);
                    std::string const every =
                        summoner.summonPerSec > 0.0f
                            ? Acore::StringFormat(" every {:.0f} s", 1.0f / summoner.summonPerSec)
                            : std::string();
                    streams[Acore::StringFormat("{} summons {}{}", summoner.name, summon.name, every)] += count;
                    mobs.insert(mobs.end(), count, summon);
                }
                for (auto const& [what, count] : streams)
                    result.summons +=
                        Acore::StringFormat("{}{} x{}", result.summons.empty() ? "" : ", ", what, count);
                if (mobs.size() != own)  // the summons joined: the kill order below reads the pack sorted
                    std::sort(mobs.begin(), mobs.end(),
                              [](Mob const& a, Mob const& b) { return a.health < b.health; });
            }
            for (Mob const& m : mobs)
            {
                rate += landed(m);
                result.health += m.health;
                result.lo = std::min(result.lo, m.level);
                result.hi = std::max(result.hi, m.level);
            }
            float const fightGuess = groupDps > 0.0f ? result.health / groupDps : 0.0f;
            // [dun244] the healer's rate as the planner's SustainedHealHps: its pool's healing over the fight caps it
            float healHps = fightGuess > 0.0f && heal.healPerMana > 0.0f
                                ? std::min(heal.hps, healMana * heal.healPerMana / fightGuess)
                                : heal.hps;
            float const startRate = rate;
            // [dun261] the planner's drop (EstimatePack): the kill order is not known - each mob lives its own kill
            // time plus half of the others' and lands its rate all that time (DropOver), and a lone healer's seconds
            // under the mobs' control casts (dun258 HealerControlSec, by their templates) are a gap with no heal.
            // b467 wc-n: the gate read "0 of 124 pulls over capacity" while the planner priced the Druid of the Fang
            // pack at drop 2.27 against its limit 0.82 (their Druid's Slumber on the lone healer)
            float t = 0.0f, intake = 0.0f;
            std::vector<std::pair<float, float>> profile;
            std::vector<uint32> entries;
            std::vector<float> lifeSec;
            for (Mob const& m : mobs)
            {
                float killDps = 0.0f;
                for (Damage const& d : dps)
                    killDps += d.Over(fightGuess) * HitFactor(int32(m.level) - int32(d.level));
                float const seg = killDps > 0.0f ? m.health / killDps : 3600.0f;
                float const life =
                    killDps > 0.0f ? (m.health + (result.health - m.health) / 2.0f) / killDps : 3600.0f;
                profile.emplace_back(landed(m), life);
                entries.push_back(m.entry);
                lifeSec.push_back(life);
                intake += landed(m) * life;
                t += seg;
            }
            result.controlSec =
                PullPlanning::TemplateHealerControlSec(entries, lifeSec, uint32(five.size()), t);
            float gapSec = std::max(gap, result.controlSec);  // the five's one healer
            if (healer2)
            {
                // [order367] the other healer heals through the control: the controlled one's rate lost for its seconds
                gapSec = gap;
                if (t > 0.0f)
                    healHps = std::max(0.0f, healHps - controlledHps * std::min(result.controlSec, t) / t);
            }
            float const deepest = PullPlanning::DropOver(profile, healHps, gapSec);
            float const drop = std::max(0.0f, intake - healHps * std::max(t - gapSec, 0.0f));
            float const healNeeded = std::max(0.0f, intake - drop);
            result.manaNeeded = heal.healPerMana > 0.0f ? healNeeded / heal.healPerMana : 0.0f;
            result.gapDrop = startRate * gap;
            result.dropShare = tankHealth > 0.0f ? (deepest + result.gapDrop) / tankHealth : 99.0f;
            // [gate94] the healer's mana over the fight: its pool (each pull from full: the five drinks before it,
            // the leader's Rest) plus its regeneration during the fight - the core's while-casting rate (MP5, the
            // talents' share of spirit: UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER) the whole fight, and its
            // full rate (UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER) for the seconds it casts nothing past the 5-second rule
            float const castSec = heal.hps > 0.0f ? healNeeded / heal.hps : t;
            float const idleSec = std::max(0.0f, t - castSec - 5.0f);
            result.fightMana = healMana + regenCasting * t + std::max(0.0f, regenFull - regenCasting) * idleSec;
            result.manaShare = result.fightMana > 0.0f ? result.manaNeeded / result.fightMana
                                                       : (healNeeded > 0.0f ? 99.0f : 0.0f);
            // [dm5] a boss is judged on his own facts, exactly as the planner's JudgeMobs judges him: the healers'
            // mana for the fight and their rate against what the pack lands per second, with the trash pulls' drop
            // model off ([lead207] b182 rfc-h 5G 507 s: Taragaman with two Cultists read "overload 1.18" by that
            // drop term, while the b181 facts of 32 boss fights have the tank's lowest health 73-100 % - the
            // healers heal a boss fight as it goes; and [lead1566] the mob count is no limit of its own once the
            // facts are known, which is the only mode this gate has). His melee stays in the fight's damage and in
            // the healers' mana all the same: only the drop term stands down. b474 dm-n: Gilnid is 2904 of the
            // foundry pull's 9639 hp and its heaviest hitter, so that term alone made the gate refuse a pull the
            // leader takes.
            // `dropShare` is still computed and logged: for a boss pull it is what the trash model would have said
            // [dm6] the rate term is every pack's, not only a boss's: CarefulOverload computes terms.peak for every
            // estimate of more than one mob (the healers' best rate together against the pack's average damage per
            // second), and JudgeMobs adds it for a lone boss on top. The gate had it on neither, so it was laxer
            // than the leader on trash - it admitted fives the leader then could not carry, which is how b474's
            // dm-n five4 came to stand in the Deadmines foundry at all.
            // [dm7] ... and the drop term is a pack's too: CarefulOverload gates hold, peak, drain and comfort
            // alike on `estimate.mobs > 1` ("a lone mob's drop too: it is no limit there, but its fight measures
            // the drop ratio"), so the planner holds a single trash mob to the healers' mana alone. The gate held
            // it to the drop as well, and refused fives on a pull the leader does not even weigh.
            bool const pack = mobs.size() > 1;  // CarefulOverload's own gate, the summons counted in as it counts them
            result.landedPerSec = t > 0.0f ? intake / t : 0.0f;
            result.peakShare = heal.hps > 0.0f && (pack || boss) ? result.landedPerSec / heal.hps : 0.0f;
            result.load = std::max({pack && !boss ? result.dropShare : 0.0f, result.manaShare, result.peakShare});
            result.sec = t;
            result.deepest = deepest;
            result.mobs = mobs.size();
            result.boss = boss;
            if (result.name.empty() && !mobs.empty())  // [dm3] named above, before its summons joined
                result.name = mobs.front().name;
            // [gate20] the drop's terms for the log: the attack table of the mob landing the most on the tank
            float heaviest = 0.0f;
            for (Mob const& m : mobs)
                if (float const rate = landed(m); rate >= heaviest)
                {
                    heaviest = rate;
                    if (auto const found = shareOf.find(m.entry); found != shareOf.end())
                        result.parts = found->second.second;
                }
            return result;
        };

        // [pack1] [dun170] the leader's pulls: what pulling spawn `i` brings of the spawns still standing (`left`),
        // counted by the one pack model (LeaderPackModel.h) the leader's planner counts with, over these spawn rows
        MapSpawns const& map = Spawns(mapId);
        std::vector<Spawn> const& spawns = map.spawns;
        size_t const n = spawns.size();
        std::vector<char> left(n, 1);
        // a patrol whose walk takes it out of every standing mob's call somewhere is pulled on its own there
        auto const patrolFree = [&](size_t p) -> bool
        {
            for (std::vector<uint32> const& inReach : map.pathNear[p])
                if (std::none_of(inReach.begin(), inReach.end(), [&](uint32 j) { return left[j]; }))
                    return true;
            return false;
        };
        std::vector<char> apartNow(n, 0);
        uint8 lowestLevel = 255;
        for (Damage const& d : dps)
            lowestLevel = std::min(lowestLevel, d.level);
        SpawnPackWorld world(map, left, apartNow, lastPull, walked, lowestLevel, tank->GetObjectSize(), groupDps);
        // [split263] `withDraws` false: the planner's pull round a corner (LosPull) - the casters walk out to the
        // tank, nobody stands where they cast, so nothing joins by their stand; `held`: spawns crowd control holds
        // before the pull - never in it, and no call, stand or formation of theirs comes either, as the planner's
        // count leaves a held mob out whole
        auto const pullOf = [&](size_t i, std::vector<uint32>& members, bool withDraws = true,
                                std::vector<char> const* held = nullptr)
        {
            // Where the pull is fought: every spawn where it stands, a patrol at the point of its beat that brings
            // the fewest of the standing spawns - the leader waits it out or pulls it first there ([dun170]).
            G3D::Vector3 at = spawns[i].pos;
            size_t fewest = std::numeric_limits<size_t>::max();
            for (size_t p = 0; p < map.pathNear[i].size() && p < spawns[i].path.size(); ++p)
            {
                std::vector<uint32> const& inReach = map.pathNear[i][p];
                size_t const count =
                    size_t(std::count_if(inReach.begin(), inReach.end(), [&](uint32 j) { return left[j]; }));
                if (count < fewest)
                {
                    fewest = count;
                    at = spawns[i].path[p];
                }
            }

            Position const spot(at.x, at.y, at.z);
            world.Take(held);
            PullPlanning::CollectPackOn(world, uint32(i), spot, members, PullPlanning::FightHere(spot), true,
                                        !withDraws);
        };
        PullLoad worst;
        std::string worstPack;  // [pack1] the worst pull's count by cause, as the pack model counted it
        uint32 judged = 0, failed = 0, apart = 0;
        std::vector<uint32> bestMembers;
        std::vector<Mob> mobs;
        // [perf211] each standing spawn's pull and its load are kept from round to round: a round takes one pull out,
        // and a kept pull changes only when one of its members is taken or one of its patrol members turns apart (a
        // member never comes back: `left` only falls, a patrol only becomes apart). Only those pulls are split and
        // judged again (a patrol's own pull always is: it reads its path's counts). The same pulls in the same order,
        // the same loads: the same choice. Before, every round split and judged every standing spawn - n^2 pulls
        // per gate (wc-n 132 rounds x 258 spawns), at each five look on the world thread
        std::vector<std::vector<uint32>> pulls(n);
        std::vector<PullLoad> loads(n);
        std::vector<std::string> whyOf(n);  // [pack1] each kept pull's count by cause, for the worst pull's line
        std::vector<char> kept(n, 0);
        std::vector<std::vector<uint32>> inPulls(n);  // spawn -> the kept pulls it is a member of (stale entries ok)
        std::vector<char> apartBefore(n, 0);
        auto const drop = [&](uint32 j)
        {
            for (uint32 i : inPulls[j])
                kept[i] = 0;
            inPulls[j].clear();
        };

        // [split254] a pull over capacity gets the leader's options in the planner's order (LeaderPlanner
        // PlanPackCrowdControl, PickTankSave), so a five is kept out only when they all fail for its members' spells.
        // The split itself is this gate's own: each spawn's pull is what the core brings with it ([dun170]).
        // (b) Crowd control: a member but the tank (a damage dealer, else the healer; no feral druid) holds a mob its
        // spell takes (the template's creature type and immunities, FindCrowdControlTemplate), greedy as the planner,
        // at most AutoCcMarkMaxPerPack: the hold that leaves the lightest pull of the rest, while the hold lasts the
        // rest's fight (CrowdControlHoldsFight) and a breakable hold stands beyond the group's area reach (its
        // members' spell data) from the rest's spawns - unless the tank pulls at range (the fight at the tank). The
        // held spawn stays standing: its own pull later. (c) The tank's defensive (TankSaves), ready on the gate's
        // clock (the pulls' fights and walks), the drop and the mana through its reduction and its health; one spent
        // on trash must be back before the next boss pull, else that pull fails as the planner would not spend it
        bool const ccOn = sPlayerbotAIConfig.AutoCc && sPlayerbotAIConfig.AutoCcPrePull;
        struct Holder
        {
            Player* player = nullptr;
            PlayerbotAI* ai = nullptr;
        };
        std::vector<Holder> dpsHolders;
        std::vector<Holder> healHolders;
        float areaReach = 0.0f;
        for (Player* p : five)
        {
            if (!p)
                continue;
            areaReach = std::max(areaReach, PullPlanning::MemberAreaReach(p));
            PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
            if (!ccOn || p == tank || !ai || !GetCrowdControlRole(p->getClass()) ||
                (p->getClass() == CLASS_DRUID && AiFactory::GetPlayerSpecTab(p) == DRUID_TAB_FERAL))
                continue;
            (p == healer || p == healer2 ? healHolders : dpsHolders).push_back({p, ai});
        }
        bool const tankAtRange = PullPlanning::TankPullsAtRange(tank);
        float const clearance = areaReach + PullPlanning::FightDriftYd();
        uint32 const maxHolds = std::max<uint32>(sPlayerbotAIConfig.AutoCcMarkMaxPerPack, 1);
        struct HoldKey
        {
            bool known = false;
            CrowdControlHold hold;
        };
        std::map<std::pair<uint64, uint32>, HoldKey> holdOf;  // (member, entry) -> its hold
        auto const holdFor = [&](Holder const& holder, Spawn const& spawn) -> HoldKey const&
        {
            HoldKey& key = holdOf[{holder.player->GetGUID().GetRawValue(), spawn.mob.entry}];
            if (!key.known)
            {
                key.known = true;
                CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(spawn.mob.entry);
                CrowdControlOption const* option =
                    proto && !spawn.mob.boss
                        ? FindCrowdControlTemplate(holder.ai, proto, spawn.mob.spellDps > 0.0f)
                        : nullptr;
                if (!option || !GetCrowdControlHold(holder.ai, *option, key.hold))
                    key.hold = CrowdControlHold();
            }
            return key;
        };
        auto const loadOf = [&](std::vector<uint32> const& members) -> PullLoad
        {
            std::vector<Mob> pullMobs;
            bool boss = false;
            for (uint32 j : members)
            {
                pullMobs.push_back(spawns[j].mob);
                boss |= spawns[j].mob.boss;
            }
            return judge(pullMobs, boss);
        };
        // the rest's pull and load with the held spawns out; `held` grows; false: no hold helps. [split263] The rest
        // is the pull of `start` again with the holds standing (pullOf `held`): what came only by a held mob's engage
        // call or its stand ([dun205]) stays out with it - a hold on the pull's own start takes only that mob out
        std::vector<char> heldMask(n, 0);
        auto const holdPart = [&](size_t start, bool withDraws, std::vector<uint32>& members, PullLoad& load,
                                  std::vector<uint32>& held, std::string& what) -> bool
        {
            for (std::vector<Holder> const* holders : {&dpsHolders, &healHolders})
            {
                std::vector<uint32> rest = members;
                std::vector<uint32> took;
                auto const restWithout = [&](uint32 j, std::vector<uint32>& trial)
                {
                    trial.clear();
                    if (j == start || std::find(took.begin(), took.end(), uint32(start)) != took.end() ||
                        Patrols(spawns[start]))
                    {
                        for (uint32 k : rest)
                            if (k != j)
                                trial.push_back(k);
                        return;
                    }
                    for (uint32 t : took)
                        heldMask[t] = 1;
                    heldMask[j] = 1;
                    pullOf(start, trial, withDraws, &heldMask);
                    for (uint32 t : took)
                        heldMask[t] = 0;
                    heldMask[j] = 0;
                    // never more than the pull it holds from (a patrol's reach may differ on a rebuild)
                    trial.erase(std::remove_if(trial.begin(), trial.end(),
                                               [&](uint32 k)
                                               { return std::find(rest.begin(), rest.end(), k) == rest.end(); }),
                                trial.end());
                };
                std::vector<Player*> usedBy;
                PullLoad restLoad = load;
                std::string line;
                while (took.size() < maxHolds)
                {
                    Holder const* bestHolder = nullptr;
                    uint32 bestSpawn = 0;
                    PullLoad bestLoad;
                    std::vector<uint32> bestRest;
                    for (Holder const& holder : *holders)
                    {
                        if (std::find(usedBy.begin(), usedBy.end(), holder.player) != usedBy.end())
                            continue;
                        for (uint32 j : rest)
                        {
                            if (spawns[j].mob.boss)  // [dm4] the leader holds the trash round a boss, never the boss
                                continue;
                            HoldKey const& key = holdFor(holder, spawns[j]);
                            if (!key.hold.spellId)
                                continue;
                            std::vector<uint32> trial;
                            restWithout(j, trial);
                            if (trial.empty())
                                continue;
                            PullLoad const trialLoad = loadOf(trial);
                            if (!CrowdControlHoldsFight(key.hold, trialLoad.sec))
                                continue;
                            bool clear = !key.hold.breakable || tankAtRange;
                            if (!clear)
                            {
                                clear = true;
                                for (uint32 k : trial)
                                {
                                    float const dx = spawns[k].pos.x - spawns[j].pos.x;
                                    float const dy = spawns[k].pos.y - spawns[j].pos.y;
                                    clear = clear && std::sqrt(dx * dx + dy * dy) > clearance;
                                }
                            }
                            if (clear && (!bestHolder || trialLoad.load < bestLoad.load))
                            {
                                bestHolder = &holder;
                                bestSpawn = j;
                                bestLoad = trialLoad;
                                bestRest = trial;
                            }
                        }
                    }
                    if (!bestHolder)
                        break;
                    took.push_back(bestSpawn);
                    usedBy.push_back(bestHolder->player);
                    rest = bestRest;
                    restLoad = bestLoad;
                    line += Acore::StringFormat("{}{} by {}'s {}", line.empty() ? "" : ", ",
                                                spawns[bestSpawn].mob.name, bestHolder->player->GetName(),
                                                holdFor(*bestHolder, spawns[bestSpawn]).hold.spell);
                    if (restLoad.load <= 1.0f)
                        break;
                }
                if (!took.empty() && restLoad.load < load.load)
                {
                    members = rest;
                    load = restLoad;
                    held.insert(held.end(), took.begin(), took.end());
                    what = line;
                    return true;
                }
            }
            return false;
        };
        std::vector<PullPlanning::TankSave> const saves = PullPlanning::TankSaves(tank);
        std::map<uint32, float> saveReadyAt;  // by spell: the gate's clock when it is back
        struct Spent
        {
            float readyAt = 0.0f;
            std::string what;
        };
        std::vector<Spent> spentOnTrash;
        float clock = 0.0f;
        uint32 heldPulls = 0;
        uint32 savedPulls = 0;
        uint32 savesBroken = 0;
        std::string optionsNote;  // the first pull each option made fit
        // [split263] the planner's pull round a corner (LosPull: the tank's pull back strategy, a ranged pull)
        PlayerbotAI* const tankAI = GET_PLAYERBOT_AI(tank);
        size_t const losMinCasters = sPlayerbotAIConfig.LosPullMinCasters;
        bool const losOn = losMinCasters && tankAI && tankAtRange && tankAI->HasStrategy("pull back", BOT_STATE_COMBAT);
        uint32 cornerPulls = 0;
        uint32 peeledPulls = 0;  // [dm4] pulls whose trash was peeled away from the boss they drag in
        constexpr uint32 OVER_LISTED = 8;  // log only
        uint32 overCount = 0;
        std::string overList;
        for (;;)
        {
            for (uint32 p : map.patrols)
            {
                apartNow[p] = left[p] && patrolFree(p);
                if (apartNow[p] != apartBefore[p])
                    drop(p);
                apartBefore[p] = apartNow[p];
            }
            PullLoad best;
            bool any = false;
            for (size_t i = 0; i < n; ++i)
            {
                if (!left[i])
                    continue;
                if (!kept[i] || Patrols(spawns[i]))
                {
                    std::vector<uint32>& members = pulls[i];
                    pullOf(i, members);
                    whyOf[i] = world.WhyLine();  // [pack1]
                    mobs.clear();
                    bool boss = false;
                    for (uint32 j : members)
                    {
                        mobs.push_back(spawns[j].mob);
                        boss |= spawns[j].mob.boss;
                        inPulls[j].push_back(uint32(i));
                    }
                    loads[i] = judge(mobs, boss);
                    kept[i] = 1;
                }
                if (!any || loads[i].load < best.load)
                {
                    any = true;
                    best = loads[i];
                    bestMembers = pulls[i];
                }
            }
            if (!any)
                break;
            ++judged;
            // [dm4] [dm2] a pull that only drags a boss in (its own start is no boss) is judged on the trash it
            // brings, as the leader judges it: the boss's own fight is his encounter's, the trash around him is a
            // trash pull. Over what the group takes, the leader does not pull it - its split search peels the trash
            // away from him first and drops every peel whose own pack brings a boss. The gate models that same plan
            // here: the cheapest boss-free pull of this pack's own members that the group takes, the boss left
            // standing for a later round with fewer mobs round him. No peel the group takes: the leader has none
            // either, and the pull goes on to the options below as before.
            if (best.boss && !spawns[bestMembers.front()].mob.boss)
            {
                std::vector<uint32> trash;
                for (uint32 j : bestMembers)
                    if (!spawns[j].mob.boss)
                        trash.push_back(j);
                PullLoad const trashLoad = trash.size() == bestMembers.size() ? best : loadOf(trash);
                if (trashLoad.load > 1.0f)
                {
                    std::vector<uint32> const* peel = nullptr;
                    PullLoad peelLoad;
                    for (uint32 j : bestMembers)
                    {
                        if (spawns[j].mob.boss || loads[j].load > 1.0f ||
                            std::any_of(pulls[j].begin(), pulls[j].end(),
                                        [&](uint32 k) { return spawns[k].mob.boss; }))
                            continue;
                        if (!peel || loads[j].load < peelLoad.load)
                        {
                            peel = &pulls[j];
                            peelLoad = loads[j];
                        }
                    }
                    if (peel)
                    {
                        ++peeledPulls;
                        if (optionsNote.find("[dm4] peeled off") == std::string::npos)
                            optionsNote += Acore::StringFormat(
                                "; [dm4] peeled off the boss: {} x{} (trash {:.2f} of it) -> {} x{} ({:.2f}), the "
                                "boss left standing", best.name, best.mobs, trashLoad.load, peelLoad.name,
                                peelLoad.mobs, peelLoad.load);
                        bestMembers = *peel;
                        best = peelLoad;
                    }
                }
            }
            if (Patrols(spawns[bestMembers.front()]) && apartNow[bestMembers.front()])
                ++apart;
            // [split254] the pull's start on the gate's clock (the walk to it after the fights before)
            float const startClock =
                clock + (walked ? (spawns[bestMembers.front()].pos - lastPull).length() / runSpeed : 0.0f);
            std::vector<uint32> heldNow;
            // [split263] a pull over capacity that its casters' stand makes ([dun205]: the members on a caster that
            // keeps its range wake what sees them there) is pulled round a corner as the planner's LosPull does (the
            // tank's ranged pull and its pull back, at least LosPullMinCasters casters): the casters walk out to the
            // tank and bring only their own engage calls; the mobs their stand would wake stay standing for their own
            // pulls. b468 wc-n: "Druid of the Fang x6 (7542 hp)" - four Druids and a Dreadfang and a Venomwing whose
            // engage calls (the core's 10 yd assistance) make two packs of three, joined only by the Druids' stand,
            // held 49 of 50 fives (worst 3.2-5.6); the planner pulled them 2-3 at a time
            size_t const start = bestMembers.front();
            // [dm4] the leader's options turn on the mob it pulls, not on what the pull drags in: its crowd control
            // stands down only for a boss pull target (HoldForPrePullCc's IsBossMob(pullTarget)) and it pulls a pack
            // that brings a boss round a corner like any other (b474 dm-n five5: "plan los pull ... where it stands:
            // 11 mobs, fight 70 s, a boss"). The gate exempted the whole pull instead, so a pull that only drags a
            // boss in was priced with none of the options the leader really has for it.
            bool const bossStart = spawns[start].mob.boss;
            bool cornered = false;
            if (best.load > 1.0f && !bossStart && losOn && !Patrols(spawns[start]))
            {
                size_t const casters = size_t(std::count_if(bestMembers.begin(), bestMembers.end(),
                                                            [&](uint32 j) { return spawns[j].keepsRange; }));
                std::vector<uint32> corner;
                if (casters >= losMinCasters)
                    pullOf(start, corner, false);
                if (!corner.empty() && corner.size() < bestMembers.size() &&
                    std::any_of(corner.begin(), corner.end(), [&](uint32 j) { return spawns[j].keepsRange; }))
                {
                    PullLoad const cornerLoad = loadOf(corner);
                    if (cornerLoad.load < best.load)
                    {
                        ++cornerPulls;
                        if (optionsNote.find("[split263] round a corner") == std::string::npos)
                            optionsNote += Acore::StringFormat(
                                "; [split263] round a corner: {} x{} ({:.2f}) -> {} x{} ({:.2f}), {} left standing",
                                best.name, best.mobs, best.load, cornerLoad.name, cornerLoad.mobs, cornerLoad.load,
                                bestMembers.size() - corner.size());
                        bestMembers = corner;
                        best = cornerLoad;
                        cornered = true;
                    }
                }
            }
            if (best.load > 1.0f && !bossStart)  // [dm4] the pull's own target, as HoldForPrePullCc reads it
            {
                // as the planner: the holds count only when they carry the rest (else the pull as it comes goes on
                // to the tank's defensive)
                std::string what;
                std::vector<uint32> rest = bestMembers;
                PullLoad restLoad = best;
                if (holdPart(start, !cornered, rest, restLoad, heldNow, what) && restLoad.load <= 1.0f)
                {
                    ++heldPulls;
                    if (optionsNote.find("[split254] held") == std::string::npos)
                        optionsNote += Acore::StringFormat("; [split254] held: {} x{} -> {} mobs ({})", best.name,
                                                           best.mobs, restLoad.mobs, what);
                    bestMembers = rest;
                    best = restLoad;
                }
            }
            if (best.load > 1.0f && !saves.empty())
            {
                PullPlanning::TankSave const* picked = nullptr;
                PullLoad pickedLoad;
                for (PullPlanning::TankSave const& save : saves)
                {
                    auto const ready = saveReadyAt.find(save.spell);
                    if (ready != saveReadyAt.end() && ready->second > startClock)
                        continue;
                    float const factor = PullPlanning::TankSaveFactor(save, best.sec);
                    float const gain = PullPlanning::TankSaveHealth(save, best.sec);
                    PullLoad saved = best;
                    saved.deepest *= factor;
                    saved.gapDrop *= factor;
                    saved.manaNeeded *= factor;
                    saved.dropShare = tankHealth > 0.0f
                                          ? (saved.deepest + saved.gapDrop) / (tankHealth * (1.0f + gain))
                                          : 99.0f;
                    saved.manaShare = saved.fightMana > 0.0f ? saved.manaNeeded / saved.fightMana : saved.manaShare;
                    saved.load = std::max(saved.dropShare, saved.manaShare);
                    if (saved.load <= 1.0f && (!picked || save.rechargeSec < picked->rechargeSec))
                    {
                        picked = &save;
                        pickedLoad = saved;
                    }
                }
                if (picked)
                {
                    best = pickedLoad;
                    saveReadyAt[picked->spell] = startClock + picked->rechargeSec;
                    ++savedPulls;
                    if (!best.boss)
                        spentOnTrash.push_back({startClock + picked->rechargeSec,
                                                Acore::StringFormat("{} on {} x{}", picked->name, best.name,
                                                                    best.mobs)});
                    if (optionsNote.find("[split254] save") == std::string::npos)
                        optionsNote += Acore::StringFormat("; [split254] save: {} on {} x{} ({:.2f})", picked->name,
                                                           best.name, best.mobs, best.load);
                }
            }
            // a defensive spent on trash not back for this boss pull: the planner keeps it, so that trash pull fails
            if (best.boss)
            {
                for (Spent const& spent : spentOnTrash)
                    if (spent.readyAt > startClock)
                    {
                        ++failed;
                        ++savesBroken;
                        optionsNote += Acore::StringFormat("; [split254] {} not back for {} (in {:.0f} s)", spent.what,
                                                           best.name, spent.readyAt - startClock);
                    }
                spentOnTrash.clear();
            }
            clock = startClock + best.sec;
            if (best.load > 1.0f)
            {
                ++failed;
                // [dun263] each pull the gate refuses, where it starts (the spawn's guid)
                if (++overCount <= OVER_LISTED)
                    overList += Acore::StringFormat("{}{}{} x{} {:.2f} at {}", overList.empty() ? "" : ", ",
                                                    best.boss ? "boss pull " : "", best.name, best.mobs, best.load,
                                                    spawns[start].guid);
            }
            if (best.load > worst.load)
            {
                worst = best;
                worstPack = whyOf[start];  // [pack1] why the pull as it came was counted the way it was
            }
            // [dun194] the pull's mana for each mana user; [dun230] less what the walk to it regenerates of the
            // previous pull's (its full rate past the five-second rule after that fight)
            G3D::Vector3 const pullPos = spawns[bestMembers.front()].pos;
            float const walkSec = walked ? (pullPos - lastPull).length() / runSpeed : 0.0f;
            lastPull = pullPos;
            walked = true;
            out.healingMana += best.manaNeeded;
            out.fightRegen += best.fightMana - healMana;
            for (Damage& d : dps)
            {
                float const back = std::min(d.lastUsed, d.walkRegen * std::max(0.0f, walkSec - 5.0f));
                if (back > 0.0f)
                {
                    out.manaNeed[d.guid] -= back;
                    out.walkRegen[d.guid] += back;
                }
                float const healUsed = std::max(0.0f, best.manaNeeded - (best.fightMana - healMana));
                float const used = d.guid == healer->GetGUID()               ? healUsed * healerShare
                                   : healer2 && d.guid == healer2->GetGUID() ? healUsed * (1.0f - healerShare)
                                                                             : d.manaPerSec * best.sec;
                d.lastUsed = used;
                if (used > 0.0f)
                    out.manaNeed[d.guid] += used;
            }
            for (uint32 j : bestMembers)
            {
                left[j] = 0;
                drop(j);  // [perf211] the kept pulls it was in are split again
            }
        }
        std::string const worstWhy =
            worst.mobs ? Acore::StringFormat(
                             "{}{} x{} (L{}-{}, {:.0f} hp): fight {:.0f} s, tank drop {:.0f} + one gap {:.0f} of "
                             "{:.0f} hp ({:.2f}{}), [dun261] healer controlled {:.1f} s, heal mana {:.0f} of {:.0f} "
                             "({:.0f} pool + regen) ({:.2f}){}{}{}",
                             worst.boss ? "boss pull " : "", worst.name, worst.mobs, worst.lo, worst.hi, worst.health,
                             worst.sec, worst.deepest, worst.gapDrop, tankHealth, worst.dropShare,
                             // [dm5] [dm7] the drop binds a pack of trash only: the number stays in the log either way
                             worst.boss          ? ", [dm5] stands down: a boss"
                             : worst.mobs <= 1   ? ", [dm7] stands down: a lone mob"
                                                 : "",
                             worst.controlSec,
                             worst.manaNeeded, worst.fightMana, healMana, worst.manaShare,
                             // [dm5] [dm6] the rate term, every pack's: what it lands per second against the
                             // healers' rate together (a lone trash mob has none, as the planner has none for it)
                             worst.peakShare > 0.0f
                                 ? Acore::StringFormat(", [dm5] lands {:.0f}/s against the healers' {:.0f}/s "
                                                       "({:.2f})", worst.landedPerSec, heal.hps, worst.peakShare)
                                 : std::string(),
                             // [dm3] the stream in the count: which summoner, at what rate, how many over the fight
                             worst.summons.empty() ? std::string()
                                                   : "; [dm3] summons in the x" + std::to_string(worst.mobs) + ": " +
                                                         worst.summons,
                             // [pack1] the pack as the one pack model counted it, in the planner's own words
                             // ("[pack why]"): the pull as it came, before the leader's options below. Sight is read
                             // as the core's aggro height gate and a corner pull brings no walk calls - no instance
                             // and no navmesh here (SpawnPackWorld)
                             worstPack.empty()
                                 ? std::string()
                                 : "; [pack1] pack why: " + worstPack +
                                       " [sight by the aggro height gate, no walk calls round a corner]")
                       : std::string("-");
        out.clears = failed == 0 && judged > 0;
        out.worstLoad = worst.load;  // [assist22]
        out.worstDrop = worst.dropShare;
        out.worstMana = worst.manaShare;
        out.over = failed;
        out.judged = judged;
        out.why = Acore::StringFormat(
            "{} of {} pulls over capacity ([dun170] as the leader splits them, {} patrols pulled apart, each from full "
            "after a rest; [dm4] {} peeled away from a boss they drag in; [split263] {} round a corner{}; [split254] "
            "{} with crowd control, {} with the tank's "
            "defensive, {} defensives not back for a boss); worst {:.2f}: {}; group burst dps {:.0f}, healer {} "
            "{:.0f} hps (gap {:.1f} s, {:.1f} hp/mana), tank {} {:.0f} hp, armor {:.0f}, avoid {:.2f}{}; [gate20] "
            "drop terms: armor keeps {:.2f}, {} [mob melee "
            "and [dun244] spells from templates, the healer's rate capped by its pool; [dun233] calls for help from "
            "the fight; [dm3] the scripts' summons over the fight; [dm6] every pack of more than one judged on the "
            "healers' rate too, [dm5] a boss on that and their mana alone and [dm7] a lone mob on their mana alone, "
            "neither on this drop; area damage not counted]{}{}",
            failed, judged, apart, peeledPulls, cornerPulls,
            losOn ? "" : " (no: the tank has no ranged pull and pull back)",
            heldPulls, savedPulls, savesBroken, worst.load, worstWhy, groupDps,
            healer->GetName() + (healer2 ? " + " + healer2->GetName() : std::string()), heal.hps, gap,
            heal.healPerMana, tank->GetName(), tankHealth, armor, avoid, bear ? " (the bear seen in form)" : "",
            1.0f - ArmorReduction(armor, worst.hi ? worst.hi : uint8(tank->GetLevel())),
            worst.parts.empty() ? std::string("-") : worst.parts, optionsNote,
            overList.empty()
                ? std::string()
                : Acore::StringFormat("; [dun263] over: {}{}", overList,
                                      overCount > OVER_LISTED
                                          ? Acore::StringFormat(" +{} more", overCount - OVER_LISTED)
                                          : std::string()));
        return out;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// startup (world thread)
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::LoadDungeons()
{
    dungeons_.clear();
    uint32 const expansion = sWorld->getIntConfig(CONFIG_EXPANSION);
    std::map<uint32, std::vector<size_t>> byMap;
    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        LFGDungeonEntry const* entry = sLFGDungeonStore.LookupEntry(i);
        if (!entry || entry->TypeID != LFG_TYPE_DUNGEON || entry->Difficulty != DUNGEON_DIFFICULTY_NORMAL ||
            entry->ExpansionLevel > expansion)
            continue;
        std::string const name = entry->Name[0] ? entry->Name[0] : "";
        // [sfk245] a holiday boss's entry (LFG_FLAG_SEASONAL: The Crown Chemical Co. in Shadowfang Keep, The Frost Lord
        // Ahune in the Slave Pens, Coren Direbrew in Blackrock Depths, L78-82) is an event, not the dungeon: kept, it
        // was an option of the dungeon's own key with the instance's walk-in floor and the event's ceiling (b445
        // "[need106] sfk-n: band 78-82"), so the five gate took any L14-82 member into SFK's five by it
        if (entry->Flags & lfg::LFG_FLAG_SEASONAL)
        {
            LOG_INFO("playerbots", "[sfk245] dungeon {} '{}' (map {}, L{}-{}): a holiday boss, no five's dungeon",
                     entry->ID, name, entry->MapID, entry->MinLevel, entry->MaxLevel);
            continue;
        }

        // the dungeon leader's content of that map; a wing (Scarlet Monastery, Dire Maul) by its name
        TestHarness::ContentDef const* content = nullptr;
        TestHarness::ContentDef const* single = nullptr;
        uint32 onMap = 0;
        for (TestHarness::ContentDef const& c : TestHarness::Contents())
        {
            if (c.mapId != entry->MapID || c.raid || c.difficulty != 0 || c.size != 5)
                continue;
            ++onMap;
            single = &c;
            if (Lower(c.name) == Lower(name))
                content = &c;
        }
        if (!content && onMap == 1)
            content = single;
        if (!content)
        {
            LOG_INFO("playerbots", "[guild60] dungeon {} '{}' (map {}): no 5-man content of the dungeon leader", entry->ID,
                     name, entry->MapID);
            continue;
        }

        DungeonOption d;
        d.lfgId = entry->ID;
        d.name = name;
        d.contentKey = content->key;
        d.mapId = entry->MapID;
        d.minLevel = entry->MinLevel;
        d.maxLevel = entry->MaxLevel;
        d.walkMinLevel = d.minLevel;
        if (DungeonProgressionRequirements const* access =
                sObjectMgr->GetAccessRequirement(d.mapId, DUNGEON_DIFFICULTY_NORMAL);
            access && access->levelMin && access->levelMin < d.minLevel)
            d.walkMinLevel = access->levelMin;
        d.targetLevel = entry->TargetLevel;
        d.expansion = entry->ExpansionLevel;

        // the outside portal: the wing's own triggers, else every areatrigger_teleport into the map but its exits
        std::vector<uint32> triggers(content->entrances.begin(), content->entrances.end());
        if (triggers.empty())
            if (QueryResult result = WorldDatabase.Query(
                    "SELECT ID, Name FROM areatrigger_teleport WHERE target_map = {} ORDER BY ID", entry->MapID))
            {
                do
                {
                    Field* f = result->Fetch();
                    std::string const trigger = f[1].Get<std::string>();
                    if (trigger.find("Exit") == std::string::npos && trigger.find("exit") == std::string::npos)
                        triggers.push_back(f[0].Get<uint32>());
                } while (result->NextRow());
            }
        bool found = false;
        for (uint32 id : triggers)
        {
            AreaTrigger const* at = sObjectMgr->GetAreaTrigger(id);
            MapEntry const* map = at ? sMapStore.LookupEntry(at->map) : nullptr;
            if (!map || map->Instanceable())
                continue;
            d.entrance = WorldPosition(at->map, at->x, at->y, at->z);
            d.reach = std::max({ENTRANCE_REACH, at->radius, at->length / 2.0f, at->width / 2.0f});
            d.entranceZone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, at->map, at->x, at->y, at->z);
            found = true;
            break;
        }
        if (!found)
        {
            LOG_INFO("playerbots", "[guild60] dungeon {} '{}': no outside portal", d.contentKey, name);
            continue;
        }
        byMap[d.mapId].push_back(dungeons_.size());
        dungeons_.push_back(d);
    }

    // the bosses' equipment drops per map (the upgrade check): encounter credits and rares / bosses by rank,
    // reference tables two levels deep
    if (!byMap.empty())
    {
        std::string maps;
        for (auto const& [mapId, list] : byMap)
            maps += (maps.empty() ? "" : ",") + std::to_string(mapId);
        std::map<uint32, std::vector<std::tuple<uint32, uint32, uint32>>> rows;  // map -> (creature, item, reference)
        // [loot13] the loot tables with their chances: creature entry / reference id -> (item, reference, chance, group)
        std::map<uint32, std::set<LootRow>> tables;
        std::map<uint32, std::set<LootRow>> refTables;
        if (QueryResult result = WorldDatabase.Query(
                "SELECT DISTINCT c.map, t.entry, l.Item, l.Reference, l.Chance, l.GroupId FROM creature c JOIN "
                "creature_template t ON "
                "t.entry = c.id JOIN creature_loot_template l ON l.Entry = t.lootid WHERE c.map IN ({}) AND "
                "(t.`rank` >= 2 OR t.entry IN (SELECT creditEntry FROM instance_encounters WHERE creditType = 0))",
                maps))
        {
            do
            {
                Field* f = result->Fetch();
                rows[f[0].Get<uint32>()].emplace_back(f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>());
                tables[f[1].Get<uint32>()].insert(
                    {f[2].Get<uint32>(), f[3].Get<uint32>(), f[4].Get<float>(), f[5].Get<uint8>()});
            } while (result->NextRow());
        }
        std::map<uint32, std::vector<std::pair<uint32, uint32>>> refs;  // reference -> (item, reference)
        std::set<uint32> wanted;
        for (auto const& [mapId, list] : rows)
            for (auto const& [creature, item, reference] : list)
                if (reference)
                    wanted.insert(reference);
        for (uint32 depth = 0; depth < 2 && !wanted.empty(); ++depth)
        {
            std::string ids;
            for (uint32 id : wanted)
                ids += (ids.empty() ? "" : ",") + std::to_string(id);
            wanted.clear();
            if (QueryResult result =
                    WorldDatabase.Query("SELECT Entry, Item, Reference, Chance, GroupId FROM reference_loot_template "
                                        "WHERE Entry IN ({})", ids))
            {
                do
                {
                    Field* f = result->Fetch();
                    uint32 const reference = f[2].Get<uint32>();
                    refs[f[0].Get<uint32>()].emplace_back(f[1].Get<uint32>(), reference);
                    refTables[f[0].Get<uint32>()].insert({f[1].Get<uint32>(), reference, f[3].Get<float>(),
                                                          f[4].Get<uint8>()});
                    if (reference && !refs.count(reference))
                        wanted.insert(reference);
                } while (result->NextRow());
            }
        }
        // [night300] a world-drop table is no loot of the dungeon: a reference table that creatures spawned in the open
        // world drop too is the generic loot of every mob of that level (24060-24078: one of 51-73 greens), or a rare
        // table shared with open-world rares. b432: RFC's Oggleflint and Bazzalan have no loot of their own, only those
        // tables at 1 % a kill (Lesser Staff of the Spire 0.014 %) - 26 of 50 members at L17-21 went back to RFC for
        // such an 'upgrade' at urgency 0 (L21 of max 21), and none of the 23 fives formed went to Deadmines or Wailing
        // Caverns. A boss's own items (direct rows) stay, also those an open-world mob by the entrance drops (BFD)
        std::set<uint32> worldRefs;
        {
            std::set<uint32> top;
            for (auto const& [mapId, list] : rows)
                for (auto const& [creature, item, reference] : list)
                    if (reference)
                        top.insert(reference);
            std::string ids;
            for (uint32 id : top)
                ids += (ids.empty() ? "" : ",") + std::to_string(id);
            if (!ids.empty())
                if (QueryResult result = WorldDatabase.Query(
                        "SELECT DISTINCT l.Reference, c.map FROM creature_loot_template l JOIN creature_template t ON "
                        "t.lootid = l.Entry JOIN creature c ON c.id = t.entry WHERE l.Reference IN ({})",
                        ids))
                {
                    do
                    {
                        Field* f = result->Fetch();
                        MapEntry const* map = sMapStore.LookupEntry(f[1].Get<uint32>());
                        if (map && !map->Instanceable())
                            worldRefs.insert(f[0].Get<uint32>());
                    } while (result->NextRow());
                }
            LOG_INFO("playerbots", "[night300] boss loot: {} of {} reference tables are world drops (open-world mobs "
                     "drop them too): no dungeon upgrade",
                     worldRefs.size(), top.size());
        }
        auto addItem = [](std::set<uint32>& items, uint32 itemId)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (Equipment(proto) && proto->Quality >= ITEM_QUALITY_UNCOMMON)
                items.insert(itemId);
        };
        for (auto const& [mapId, list] : byMap)
            for (size_t index : list)
            {
                DungeonOption& d = dungeons_[index];
                TestHarness::ContentDef const* content = TestHarness::FindContent(d.contentKey);
                std::set<uint32> items;
                // [loot13] each item's sources in the dungeon with the chance a kill drops it
                auto const addSource = [&](uint32 itemId, uint32 creature, float chance)
                {
                    if (!items.count(itemId) || chance <= 0.0f)
                        return;
                    auto& list = d.itemSources[itemId];
                    auto const same = std::find_if(list.begin(), list.end(),
                                                   [creature](auto const& s) { return s.first == creature; });
                    if (same == list.end())
                        list.emplace_back(creature, std::min(1.0f, chance));
                    else
                        same->second = 1.0f - (1.0f - same->second) * (1.0f - std::min(1.0f, chance));
                };
                for (auto const& [creature, item, reference] : rows[mapId])
                {
                    // a wing: its own bosses only
                    if (content && !content->bossEntries.empty() &&
                        std::find(content->bossEntries.begin(), content->bossEntries.end(), creature) ==
                            content->bossEntries.end())
                        continue;
                    float const rowChance = LootRowChance(tables[creature], item, reference);
                    if (!reference)
                    {
                        addItem(items, item);
                        addSource(item, creature, rowChance);
                        continue;
                    }
                    if (worldRefs.count(reference))
                        continue;
                    for (auto const& [refItem, refRef] : refs[reference])
                    {
                        float const refChance = rowChance * LootRowChance(refTables[reference], refItem, refRef);
                        if (!refRef)
                        {
                            addItem(items, refItem);
                            addSource(refItem, creature, refChance);
                        }
                        else
                            for (auto const& [deepItem, deepRef] : refs[refRef])
                                if (!deepRef)
                                {
                                    addItem(items, deepItem);
                                    addSource(deepItem, creature,
                                              refChance * LootRowChance(refTables[refRef], deepItem, deepRef));
                                }
                    }
                }
                d.bossItems.assign(items.begin(), items.end());
            }
        // [loot16] a source a script despawns when its AI starts (SMART_EVENT_AI_INIT / RESPAWN ->
        // SMART_ACTION_FORCE_DESPAWN, at its event chance) is in an instance only that share of the time
        std::set<uint32> sourceEntries;
        for (DungeonOption const& d : dungeons_)
            for (auto const& [itemId, list] : d.itemSources)
                for (auto const& [entry, chance] : list)
                    sourceEntries.insert(entry);
        std::string ids;
        for (uint32 entry : sourceEntries)
            ids += (ids.empty() ? "" : ",") + std::to_string(entry);
        if (!ids.empty())
            if (QueryResult result = WorldDatabase.Query(
                    "SELECT entryorguid, event_chance FROM smart_scripts WHERE source_type = 0 AND event_type IN "
                    "({}, {}) AND action_type = {} AND entryorguid IN ({})",
                    uint32(SMART_EVENT_AI_INIT), uint32(SMART_EVENT_RESPAWN), uint32(SMART_ACTION_FORCE_DESPAWN), ids))
            {
                do
                {
                    Field* f = result->Fetch();
                    float& presence = scriptPresence_.emplace(f[0].Get<int32>(), 1.0f).first->second;
                    presence *= 1.0f - std::min(100.0f, float(f[1].Get<uint8>())) / 100.0f;
                } while (result->NextRow());
            }
        for (auto const& [entry, presence] : scriptPresence_)
            LOG_INFO("playerbots", "[loot16] source {} is in an instance {:.0f} % of the time (its AI-start despawn)",
                     entry, presence * 100.0f);
    }

    std::sort(dungeons_.begin(), dungeons_.end(),
              [](DungeonOption const& a, DungeonOption const& b) { return a.targetLevel < b.targetLevel; });
    for (DungeonOption const& d : dungeons_)
        LOG_INFO("playerbots", "[guild60] dungeon {} '{}' L{}-{} (target {}), entrance map {} ({:.0f},{:.0f}) zone {}, "
                 "{} boss items",
                 d.contentKey, d.name, d.minLevel, d.maxLevel, d.targetLevel, d.entrance.GetMapId(),
                 d.entrance.GetPositionX(), d.entrance.GetPositionY(), d.entranceZone, d.bossItems.size());
}

void GuildCoordinator::LoadRuns()
{
    runs_.clear();
    cleared_.clear();
    ++orderGen_;  // [order360]
    PlayerbotsDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guild_project_runs ("
        "id INT UNSIGNED NOT NULL AUTO_INCREMENT, content VARCHAR(16) NOT NULL, kind VARCHAR(8) NOT NULL, "
        "at INT UNSIGNED NOT NULL, leader INT UNSIGNED NOT NULL, members VARCHAR(255) NOT NULL, "
        "result VARCHAR(16) NOT NULL, bosses_killed TINYINT UNSIGNED NOT NULL, bosses_total TINYINT UNSIGNED NOT NULL, "
        "deaths SMALLINT UNSIGNED NOT NULL, wipes SMALLINT UNSIGNED NOT NULL, duration INT UNSIGNED NOT NULL, "
        "drops TEXT, reason TEXT, PRIMARY KEY (id), KEY content (content)) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='[guild60] guild fives x dungeons, raids x raids'");
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT id, content, kind, at, leader, members, result, bosses_killed, bosses_total, deaths "
            "FROM guild_project_runs ORDER BY id"))
    {
        do
        {
            Field* f = result->Fetch();
            GuildRunRecord rec;
            rec.id = f[0].Get<uint32>();
            rec.contentKey = f[1].Get<std::string>();
            rec.kind = f[2].Get<std::string>();
            rec.at = f[3].Get<uint32>();
            rec.leaderLow = f[4].Get<uint32>();
            std::stringstream in(f[5].Get<std::string>());
            std::string token;
            while (std::getline(in, token, ','))
                if (uint32 const low = uint32(std::strtoul(token.c_str(), nullptr, 10)))
                    rec.members.push_back(low);
            rec.result = f[6].Get<std::string>();
            rec.bossesKilled = f[7].Get<uint8>();
            rec.bossesTotal = f[8].Get<uint8>();
            rec.deaths = f[9].Get<uint16>();
            if (rec.result == "done")
                for (uint32 low : rec.members)
                    cleared_[rec.contentKey].insert(low);
            runs_.push_back(rec);
        } while (result->NextRow());
    }
    LOG_INFO("playerbots", "[guild60] completion table: {} runs", runs_.size());

    // [loot13] the five's goal mobs for its route (the leader's route rules), and their states back
    Leader::Travel::SetRouteGoalHooks([](uint32 low) { return sGuildCoordinator.FiveGoalMobs(low); },
                                      [](uint32 low, uint32 entry, std::string const& state)
                                      { sGuildCoordinator.NoteGoalState(low, entry, state); });
    // [guild70] the five-leader's end of its run (Ai/Leader/Dungeon/LeaderDungeonRun.h) comes back here
    Leader::Dungeon::DungeonRuns::Instance().SetEndHook(
        [](uint32 fiveId, Leader::Dungeon::FiveRunReport const& r)
        {
            GuildFiveReport report;
            report.runId = r.runId;
            report.result = r.result;
            report.reason = r.reason;
            // the Dungeon Finder did not take the five: it walks to the entrance instead (OnFiveRunEnd)
            if (r.lfgRefused)
                report.reason = FIVE_LFG_REFUSED + r.reason;
            // the key / event items nobody carried: the five-leader's needs for the global leader
            for (std::string const& need : r.needs)
                report.reason += std::string(report.reason.empty() ? "" : "; ") + FIVE_ITEM_NEED + need;
            report.bossesKilled = r.bossesKilled;
            report.bossesTotal = r.bossesTotal;
            report.wipes = r.wipes;
            report.durationSec = r.durationSec;
            report.deaths = r.deaths;
            sGuildCoordinator.OnFiveRunEnd(fiveId, report);
        });
}

// ---------------------------------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------------------------------
GuildCoordinator::DungeonOption const* GuildCoordinator::FindDungeon(std::string const& contentKey) const
{
    for (DungeonOption const& d : dungeons_)
        if (d.contentKey == contentKey || Lower(d.name) == Lower(contentKey))
            return &d;
    return nullptr;
}

// the entrance in the other faction's capital (its guards): the capital's name, else nullptr
static char const* EnemyCapital(uint32 entranceZone, TeamId team)
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(entranceZone);
    uint32 const own = team == TEAM_ALLIANCE ? AREATEAM_ALLY : AREATEAM_HORDE;
    if (zone && (zone->flags & AREA_FLAG_CAPITAL) && zone->team && zone->team != own && zone->team != AREATEAM_ANY)
        return zone->area_name[0];
    return nullptr;
}

std::string GuildCoordinator::DungeonRefusal(DungeonOption const& d, Player* p, bool walk, bool overMax) const
{
    // [five58] user 2026-11-09: the five gate proves a five's capacity, so the Dungeon Finder's minimum is no floor:
    // from the instance's own entry level (RFC 8, WC / DM 10, SFK 14) a five that walks in goes (b400: the healers
    // Guinudrond, Selalleas and Aevmae at L14 were outside every band of 15+)
    if (p->GetLevel() < d.walkMinLevel || (!overMax && p->GetLevel() > d.maxLevel))
        return "L" + std::to_string(p->GetLevel()) + " outside " + std::to_string(d.walkMinLevel) + "-" +
               std::to_string(d.maxLevel);
    // [order362] above the finder's band only the walk in takes it, up to the instance's own entry ceiling (if any)
    if (overMax && p->GetLevel() > d.maxLevel)
    {
        if (DungeonProgressionRequirements const* access =
                sObjectMgr->GetAccessRequirement(d.mapId, DUNGEON_DIFFICULTY_NORMAL);
            access && access->levelMax && p->GetLevel() > access->levelMax)
            return "L" + std::to_string(p->GetLevel()) + " above the entry's max " +
                   std::to_string(access->levelMax);
        walk = true;
    }
    bool const belowFinder = p->GetLevel() < d.minLevel;
    if (belowFinder)
        walk = true;
    // [next1] the Dungeon Finder ports the group in from anywhere (UpdateFives queues a five before it walks): the
    // entrance's continent and capital are only the walk's. 2026-11-09: 19 draenei on Azuremyst (map 530) and 19 night
    // elves on Kalimdor were no candidates for Deadmines / Wailing Caverns, and Ragefire Chasm (Orgrimmar) for none.
    if (!walk && d.lfgId && sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER))
        return "";
    // a walk across continents has no solo safety check yet (TripRefused judges the bot's own map)
    if (p->GetMapId() != d.entrance.GetMapId())
        return REFUSAL_OTHER_CONTINENT + std::to_string(d.entrance.GetMapId()) + ")";
    if (char const* capital = EnemyCapital(d.entranceZone, p->GetTeamId()))
        return std::string("entrance in an enemy capital ") + capital;
    return "";
}

// [loot13] user rule 2026-11-13 «обязательно сопоставить цели в данже и маршрут»: the five's goals are its members'
// needed drops (GuildGear's judge, no copy owned) with their sources and chances. A boss is the route's anyway; another
// source (a rare, a wanderer) the route takes when its expected gain per run (chance x the slot gain, the best member's)
// over its kill effort (its template health) is at least the bosses' own: the detour earns what the run earns
GuildCoordinator::FiveGoals GuildCoordinator::GoalsFor(DungeonOption const& d,
                                                       std::vector<Player*> const& members) const
{
    FiveGoals goals;
    struct Need
    {
        float gain{0.0f};
        std::vector<std::string> who;
    };
    std::map<uint32, Need> needs;  // item -> the best gain and the members who need it
    for (Player* p : members)
    {
        if (!p || !GET_PLAYERBOT_AI(p))
            continue;
        for (uint32 itemId : d.bossItems)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto || proto->RequiredLevel > d.maxLevel || !d.itemSources.count(itemId))
                continue;
            float const gain = UpgradeGain(p, proto, 0);
            if (gain <= 0.0f)
                continue;
            Need& need = needs[itemId];
            need.gain = std::max(need.gain, gain);
            need.who.push_back(p->GetName());
        }
    }
    float bossValue = 0.0f;
    float bossHealth = 0.0f;
    std::set<uint32> bossSeen;
    for (auto const& [itemId, need] : needs)
        for (auto const& [entry, chance] : d.itemSources.at(itemId))
        {
            FiveGoalMob& mob = goals.mobs[entry];
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
            if (!info)
                continue;
            mob.name = info->Name;
            mob.boss = (info->flags_extra & CREATURE_FLAG_EXTRA_DUNGEON_BOSS) || info->rank == CREATURE_ELITE_WORLDBOSS;
            mob.presence = Presence(entry);
            if (CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(info->maxlevel, info->unit_class))
                mob.health = float(stats->GenerateHealth(info));
            mob.value += mob.presence * chance * need.gain;  // [loot16] a kill only where it is
            std::string who;
            for (std::string const& name : need.who)
                who += (who.empty() ? "" : ", ") + name;
            mob.items += Acore::StringFormat("{}{} {:.0f} % for {}", mob.items.empty() ? "" : "; ",
                                             ItemName(itemId), chance * 100.0f, who);
            if (mob.boss)
            {
                bossValue += mob.presence * chance * need.gain;
                if (bossSeen.insert(entry).second)
                    bossHealth += mob.health;
            }
        }
    for (auto& [entry, mob] : goals.mobs)
    {
        if (mob.boss)
        {
            mob.taken = mob.takenIfThere = true;
            mob.why = "a boss of the route";
            continue;
        }
        // nothing else to earn on the route: the source is the run's whole value. [loot16] The route kills it where
        // it stands (its kill's own gain, presence 1); the need to repeat the dungeon for it weighs its presence
        float const routePerHealth = bossHealth > 0.0f ? bossValue / bossHealth : 0.0f;
        float const health = std::max(1.0f, mob.health);
        float const killValue = mob.presence > 0.0f ? mob.value / mob.presence : 0.0f;
        mob.takenIfThere = killValue > 0.0f && killValue / health >= routePerHealth;
        mob.taken = mob.value > 0.0f && mob.value / health >= routePerHealth;
        mob.why = Acore::StringFormat("expected gain {:.3f} a kill, {:.3f} a run (in the instance {:.0f} %) over "
                                      "{:.0f} health ({:.2e} / {:.2e} a point) vs the bosses' {:.2e}: {}",
                                      killValue, mob.value, mob.presence * 100.0f, mob.health, killValue / health,
                                      mob.value / health, routePerHealth,
                                      mob.taken ? "a need and a route goal"
                                      : mob.takenIfThere ? "a route goal where it stands, no repeat for it"
                                                         : "neither");
    }
    return goals;
}

// [need106] user rule 2026-11-10: a member goes (again) while the dungeon's boss loot holds an upgrade it needs - its
// own spec's "item upgrade" (EQUIP / REPLACE) with a gain over its worn piece, one it does not own already (won or
// bought: in its bags or bank). [loot13] only from a source the route takes (GoalsFor), said with its chance
std::string GuildCoordinator::NeededUpgrade(DungeonOption const& d, Player* p) const
{
    FiveGoals const goals = GoalsFor(d, {p});
    std::string best;
    float bestValue = 0.0f;
    for (auto const& [entry, mob] : goals.mobs)
        if (mob.taken && mob.value > bestValue)
        {
            bestValue = mob.value;
            best = mob.items.substr(0, mob.items.find(" for ")) + " (" + mob.name + ")";
        }
    return best;
}

// [roles108] the dungeon's mobs still give the member experience: below the level cap, and its highest spawn (the
// gate's spawns: the bosses and trash as their templates make them) above the member's grey level (Acore::XP)
bool GuildCoordinator::GivesXp(DungeonOption const& d, Player* p) const
{
    if (p->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        return false;
    return FiveGate::TopLevel(d.mapId) > Acore::XP::GetGrayLevel(p->GetLevel());
}

bool GuildCoordinator::FreeForFive(uint32 low, Player* p, std::string* why) const
{
    auto refuse = [why](char const* text)
    {
        if (why)
            *why = text;
        return false;
    };
    if (!p || !p->IsInWorld() || p->IsBeingTeleported())
        return refuse("offline");
    if (!p->IsAlive())
        return refuse("dead");
    if (p->IsInCombat())
        return refuse("in combat");
    if (p->GetMap()->Instanceable())
        return refuse("in a dungeon");
    // [coop] a co-op party is no group for the five: the five's ChangeToIdle takes the member out of it
    if (p->GetGroup() && !NewRpgCoop::InParty(low))
        return refuse("in a group");
    auto const st = bots_.find(low);
    if (st != bots_.end())
    {
        if (st->second.fiveId)
            return refuse("already in a five");
        if (st->second.run)
            return refuse("walks to the vault");
        if (!st->second.orders.empty() && st->second.orders.front().type != GuildOrderType::QUEST &&
            st->second.orders.front().executable)
            return refuse("busy with an order");
    }
    return true;
}

// [roles62] a feral druid the role plan made a tank is one in or out of bear form (IsTank by spec asks for the form
// or Thick Hide: out of the form while questing it read as dps, and its five had no tank)
static bool PlannedBearTank(Player* p, std::string const& planned)
{
    return planned == "tank" && p->getClass() == CLASS_DRUID && AiFactory::GetPlayerSpecTab(p) == DRUID_TAB_FERAL;
}

std::string GuildCoordinator::PlanRole(uint32 low, Player* p) const
{
    // [five27] the plan's role holds only when the bot plays it now (its spec and strategies): b375 rfc-n five 2 had
    // Farret, planned a healer, as its healer while he played retribution - he cast Judgement and no heal in the
    // whole pull, the gate counted his 86 hps, the tank died at 321 s (pull 13, 5 mobs, 28 s)
    if (PlanRow const* row = Row(low))
        if ((row->role == "tank" && PlayerbotAI::IsTank(p, true)) ||
            (row->role == "healer" && PlayerbotAI::IsHeal(p, true)))
            return row->role;
    if (PlayerbotAI::IsTank(p, true) || PlannedBearTank(p, RoleOf(low)))
        return "tank";
    if (PlayerbotAI::IsHeal(p, true))
        return "healer";
    return "dps";
}

std::string GuildCoordinator::SpecRoleNow(Player* p) const
{
    if (PlayerbotAI::IsTank(p, true) || PlannedBearTank(p, RoleOf(p->GetGUID().GetCounter())))
        return "tank";
    if (PlayerbotAI::IsHeal(p, true))
        return "healer";
    return "dps";
}

uint32 GuildCoordinator::RoleSpec(uint32 low, uint8 activeSpec) const
{
    std::lock_guard<std::mutex> guard(roleLock_);
    auto const itr = roleAssign_.find(low);
    if (itr == roleAssign_.end())
        return 0;
    return uint32(activeSpec ? itr->second.solo : itr->second.spec) + 1;
}

std::string GuildCoordinator::RoleOf(uint32 low) const
{
    std::lock_guard<std::mutex> guard(roleLock_);
    auto const itr = roleAssign_.find(low);
    return itr == roleAssign_.end() ? "" : itr->second.role;
}

// [roles59] user direction 2026-11-09: the fives must never lack a healer or a tank. For each dungeon band the global
// leader counts the planned members in it (A, B, C; humans aside), the fives they make (members / 5: a tank, a healer
// and 3 dps each) and the tanks and healers they hold (a role planned earlier, else its talents now). A shortfall is
// filled from the band's hybrids playing damage: the plan's own role first (guild_project_plan), then the most
// points already in the role's tree (the least loss), then the highest level. Only as many as the fives need: a
// heal or tank spec quests slower solo. The assigned member puts its talent points into the role's spec from now on
// (PlayerbotFactory::InitTalentsTree reads RoleSpec), unlearns its talents at its class trainer for the core's price
// (NewRpgProfession::StartRoleSpec), and from the dual spec level buys the second spec there (1000 g, the user's
// rule): spec 1 then holds its solo spec, spec 0 the role, switched as it joins or leaves a five.
// [roles108] user design 2026-11-10: the raid roster per 25 members (2-3 tanks, 5-6 healers: the upper ends)
static constexpr uint32 RAID_TANKS_PER_25 = 3;
static constexpr uint32 RAID_HEALERS_PER_25 = 6;

void GuildCoordinator::PlanRoles()
{
    struct Band
    {
        uint32 lo = 0;
        uint32 hi = 0;
        std::vector<std::pair<PlanRow const*, Player*>> members;
        uint32 fives = 0;
        uint32 healTarget = 0;
        uint32 tankTarget = 0;  // [roles108]
    };
    std::vector<Band> bands;
    {
        std::set<std::pair<uint32, uint32>> keys;
        for (DungeonOption const& d : dungeons_)
            keys.emplace(d.walkMinLevel, d.maxLevel);
        for (auto const& [lo, hi] : keys)
        {
            Band band;
            band.lo = lo;
            band.hi = hi;
            for (PlanRow const& row : plan_)
            {
                if (row.grp < 'A' || row.grp > 'C')
                    continue;
                Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                if (!p || !p->IsInWorld() || IsHuman(p) || p->GetLevel() < lo || p->GetLevel() > hi)
                    continue;
                band.members.emplace_back(&row, p);
            }
            if (band.members.size() < 5)
                continue;
            uint32 const n = uint32(band.members.size());
            band.fives = n / 5;
            // [roles108] user design 2026-11-10: the standing roster is sized for raids - 2-3 tanks and 5-6 healers per
            // 25 members, kept at every level (each band its share: its members x 3 / 25 tanks, x 6 / 25 healers,
            // rounded up). Tanks: that share, at most the band's fives
            band.tankTarget = std::min(band.fives, (n * RAID_TANKS_PER_25 + 24) / 25);
            // [roles107] a spare healer per band, so one healer is not chained into every five - when the band has
            // members past its full fives; [roles108] at least the band's raid share, and never so many that its fives
            // lack their 3 dps
            uint32 const spare = n > band.fives * 5 ? 1 : 0;
            uint32 const dpsRoom = n - band.tankTarget - 3 * band.fives;
            band.healTarget = std::min(std::max(band.fives + spare, (n * RAID_HEALERS_PER_25 + 24) / 25),
                                       std::max(dpsRoom, band.fives));
            bands.push_back(std::move(band));
        }
    }
    auto roleNow = [this](uint32 low, Player* p)
    {
        std::string const planned = RoleOf(low);
        return planned.empty() ? SpecRoleNow(p) : planned;
    };
    // [lag334] the guild's median level (the planned members online): a member below it is moved to no other role
    uint8 medianLevel = 0;
    {
        std::vector<uint8> levels;
        for (PlanRow const& row : plan_)
            if (row.grp >= 'A' && row.grp <= 'C')
                if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                    p && p->IsInWorld() && !IsHuman(p))
                    levels.push_back(p->GetLevel());
        if (!levels.empty())
        {
            std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
            medianLevel = levels[levels.size() / 2];
        }
    }
    auto count = [&roleNow](Band const& band, std::string const& role)
    {
        uint32 n = 0;
        for (auto const& [row, p] : band.members)
            n += roleNow(row->low, p) == role ? 1 : 0;
        return n;
    };
    // the user's rule: no respec inside a five
    auto inFive = [this](uint32 low)
    {
        auto const st = bots_.find(low);
        return st != bots_.end() && st->second.fiveId;
    };
    auto assign = [&](Band const& band, PlanRow const* row, Player* p, std::string const& role, uint32 points,
                      std::string const& from)
    {
        uint32 const before = count(band, role);
        RoleAssign a;
        a.role = role;
        a.spec = uint8(RoleSpecIndex(p->getClass(), role));
        a.solo = SoloSpecIndex(p);
        {
            std::lock_guard<std::mutex> guard(roleLock_);
            roleAssign_[row->low] = a;
        }
        // [roles62] the role spec outranks questing as a five does (b401: 10 healers assigned, all "respec at its
        // trainer", and each kept its quest first - the fives had no healer meanwhile): the member leaves its quest
        // now, New RPG's IDLE takes the trainer walk first (StartRoleSpec)
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p); ai && !p->IsInCombat() && SpecRoleNow(p) != role &&
            ai->rpgInfo.GetStatus() != RPG_IDLE && !inFive(row->low) && !NewRpgCoop::InParty(row->low))
            ai->rpgInfo.ChangeToIdle("it leaves its quest for the role spec its guild leader planned");
        LOG_INFO("playerbots", "[roles59] {} L{} → {} ({}): band {}-{} has {} fives and {} {}s (target {}); {}; plan "
                 "role {}, {} points in its tree, talents now {}",
                 p->GetName(), p->GetLevel(), role, SpecName(p->getClass(), a.spec), band.lo, band.hi, band.fives,
                 before, role, role == "healer" ? band.healTarget : band.tankTarget, from, row->role, points,
                 SpecRoleNow(p));
    };
    // the best member to take a role: the plan's own role first (guild_project_plan), then the most points already in
    // the role's tree (the least loss), then the highest level
    auto pick = [&](Band const& band, std::string const& role, std::function<bool(PlanRow const*, Player*)> const& may,
                    uint32& bestPoints) -> std::pair<PlanRow const*, Player*>
    {
        std::pair<PlanRow const*, Player*> best{nullptr, nullptr};
        bool bestPlanned = false;
        uint32 bestPrice = 0;
        bestPoints = 0;
        // [roles118] coordinator 2026-11-10 (b421: Farret's unlearn had reached 5 g - the plan had flipped his role
        // several times): the real price first - the core's unlearn cost (Player::resetTalentsCost, it rises with
        // each reset) when its talents must go for the role, 0 when they need not - the cheapest first; then the
        // plan's role, the points already in the role's tree, the level. A member the plan moved already this
        // session is taken only when nobody else can fill the gap (two passes): no back and forth
        for (bool const movedToo : {false, true})
        {
            for (auto const& [row, p] : band.members)
            {
                int8 const spec = RoleSpecIndex(p->getClass(), role);
                if (spec < 0 || inFive(row->low) || !may(row, p) || (!movedToo && !RoleOf(row->low).empty()))
                    continue;
                // [lag334] coordinator decision 2026-11-11 (a member goes for progress of its own; levelling first
                // below the median, [lag324] / [lag325]): a member below the guild's median level is moved to no
                // other role - an unlearn at its trainer and a new spec while it levels behind the guild. b439-b442
                // Rollothan L13-14 (the second lowest member): "[roles107] a surplus tank moves to heal" (b439,
                // unlearn 5 g from Peny and Mutil, 1274 yd to Bromos Grummner), "[roles108] a surplus healer moves to
                // tank" (b440 and b442: unlearn 10 g, 2022 yd to the Paladin Trainer) as the band's counts swung; he
                // quested 5/0/0 Holy meanwhile
                if (p->GetLevel() < medianLevel && SpecRoleNow(p) != role)
                    continue;
                std::map<uint8, uint32> tabs = AiFactory::GetPlayerSpecTabs(p);
                uint32 const points = tabs[SpecTree(p->getClass(), uint8(spec))];
                uint32 const price =
                    NewRpgProfession::NeedsRespec(p, uint32(spec) + 1) ? p->resetTalentsCost() : 0;
                bool const planned = row->role == role;
                if (best.first &&
                    std::make_tuple(-int64(price), planned, points, p->GetLevel()) <=
                        std::make_tuple(-int64(bestPrice), bestPlanned, bestPoints, best.second->GetLevel()))
                    continue;
                best = {row, p};
                bestPoints = points;
                bestPlanned = planned;
                bestPrice = price;
            }
            if (best.first)
                break;
        }
        return best;
    };

    for (Band const& band : bands)
    {
        // [roles107] user 2026-11-10 (b420 band 17-32: 12 members, 2 fives, 5 tanks, 1 healer, "short: 1 healers"):
        // the plan only filled shortfalls and never took a surplus back. Tanks = the band's fives; a surplus tank of
        // a class that heals (paladin, druid) becomes a healer while the band is short of healers - only one whose
        // every other band keeps its own tanks, never one inside a five
        while (count(band, "healer") < band.healTarget && count(band, "tank") > band.tankTarget)
        {
            uint32 points = 0;
            auto const [row, p] = pick(
                band, "healer",
                [&](PlanRow const* r, Player* m)
                {
                    if (roleNow(r->low, m) != "tank")
                        return false;
                    for (Band const& other : bands)
                        if (&other != &band && m->GetLevel() >= other.lo && m->GetLevel() <= other.hi &&
                            count(other, "tank") <= other.tankTarget)
                            return false;
                    return true;
                },
                points);
            if (!row)
                break;
            assign(band, row, p, "healer", points,
                   Acore::StringFormat("[roles107] a surplus tank ({} tanks for a target of {}) moves to heal",
                                       count(band, "tank"), band.tankTarget));
        }
        // [roles108] and the other way: a surplus healer of a class that tanks (paladin, druid) tanks while the band
        // is short of tanks
        while (count(band, "tank") < band.tankTarget && count(band, "healer") > band.healTarget)
        {
            uint32 points = 0;
            auto const [row, p] = pick(
                band, "tank",
                [&](PlanRow const* r, Player* m)
                {
                    if (roleNow(r->low, m) != "healer")
                        return false;
                    for (Band const& other : bands)
                        if (&other != &band && m->GetLevel() >= other.lo && m->GetLevel() <= other.hi &&
                            count(other, "healer") <= other.healTarget)
                            return false;
                    return true;
                },
                points);
            if (!row)
                break;
            assign(band, row, p, "tank", points,
                   Acore::StringFormat("[roles108] a surplus healer ({} healers for a target of {}) moves to tank",
                                       count(band, "healer"), band.healTarget));
        }
        // the shortfalls from the band's hybrids playing damage: healers to their target, tanks to the fives
        for (std::string const role : {"healer", "tank"})
        {
            uint32 const target = role == "healer" ? band.healTarget : band.tankTarget;
            while (count(band, role) < target)
            {
                uint32 points = 0;
                auto const [row, p] = pick(band, role,
                                           [&](PlanRow const* r, Player* m)
                                           { return roleNow(r->low, m) == "dps" && RoleOf(r->low).empty(); },
                                           points);
                if (!row)
                    break;
                assign(band, row, p, role, points, "a damage hybrid takes the role");
            }
        }
    }

    for (Band const& band : bands)
    {
        uint32 dps = 0;
        std::map<std::string, std::vector<std::string>> lists;
        for (auto const& [row, p] : band.members)
        {
            std::string const role = roleNow(row->low, p);
            if (role == "dps")
            {
                ++dps;
                continue;
            }
            std::string entry = p->GetName() + " L" + std::to_string(p->GetLevel());
            int8 const spec = RoleSpecIndex(p->getClass(), role);
            if (!RoleOf(row->low).empty() && SpecRoleNow(p) != role)
                entry += " " + SpecName(p->getClass(), uint8(spec)) + " (respec at its trainer)";
            else if (!RoleOf(row->low).empty())
                entry += " " + SpecName(p->getClass(), uint8(spec));
            else
                entry += " (its talents)";
            lists[role].push_back(entry);
        }
        auto join = [](std::vector<std::string> const& v)
        {
            std::string out;
            for (std::string const& s : v)
                out += (out.empty() ? "" : ", ") + s;
            return out;
        };
        std::string text = Acore::StringFormat("{} members, {} fives; healers {} of target {} ({}), tanks {} of "
                                               "target {} ({}), dps {}",
                                               band.members.size(), band.fives, lists["healer"].size(),
                                               band.healTarget, join(lists["healer"]), lists["tank"].size(),
                                               band.tankTarget, join(lists["tank"]), dps);
        if (lists["healer"].size() < band.healTarget || lists["tank"].size() < band.tankTarget)
            text += Acore::StringFormat("; short: {} healers, {} tanks (no free tank or damage hybrid of a class that "
                                        "can left)",
                                        lists["healer"].size() < band.healTarget
                                            ? band.healTarget - lists["healer"].size()
                                            : 0,
                                        lists["tank"].size() < band.tankTarget ? band.tankTarget - lists["tank"].size()
                                                                               : 0);
        if (lists["tank"].size() > band.tankTarget)
            text += Acore::StringFormat("; {} tanks over the target (no healer class among them, or held by a five or "
                                        "another band)",
                                        lists["tank"].size() - band.tankTarget);
        std::string const key = std::to_string(band.lo) + "-" + std::to_string(band.hi);
        if (rolesWhy_[key] != text)
        {
            rolesWhy_[key] = text;
            LOG_INFO("playerbots", "[roles59] band {}: {}", key, text);
        }
    }

    // [roles108] user design 2026-11-10: the global leader's roster - per member what it can play (its class), what
    // it plays now (its talents), its planned role and spec, its dual spec - the facts the roles are planned from.
    // One line per member when its facts change; the guild's totals against the raid roster
    uint32 total = 0;
    std::map<std::string, uint32> guildRoles;
    // [roster1] the guild's roster against the raid targets: who holds each role, who still respecs for it (the
    // core's unlearn price at its class trainer now)
    std::map<std::string, std::vector<std::string>> holders;
    std::vector<std::string> respecPlan;
    auto gold = [](uint32 copper)
    { return Acore::StringFormat("{}g {}s {}c", copper / 10000, (copper / 100) % 100, copper % 100); };
    for (PlanRow const& row : plan_)
    {
        if (row.grp < 'A' || row.grp > 'C')
            continue;
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || !p->IsInWorld() || IsHuman(p))
            continue;
        ++total;
        std::string const role = roleNow(row.low, p);
        ++guildRoles[role];
        uint8 const cls = p->getClass();
        if (role != "dps")
        {
            holders[role].push_back(p->GetName() + " L" + std::to_string(p->GetLevel()));
            if (int8 const spec = RoleSpecIndex(cls, role); spec >= 0 && SpecRoleNow(p) != role)
            {
                uint32 const cost = NewRpgProfession::NeedsRespec(p, uint32(spec) + 1) ? p->resetTalentsCost() : 0;
                respecPlan.push_back(Acore::StringFormat("{} (class {} tree {} {} -> {}, unlearn {} at its class "
                                                         "trainer)",
                                                         p->GetName(), uint32(cls),
                                                         uint32(AiFactory::GetPlayerSpecTab(p)), SpecRoleNow(p),
                                                         SpecName(cls, uint8(spec)), gold(cost)));
            }
        }
        std::map<uint8, uint32> tabs = AiFactory::GetPlayerSpecTabs(p);
        std::string const planned = RoleOf(row.low);
        std::string line = Acore::StringFormat(
            "{} L{} class {}: can heal {}, can tank {}; talents now tree {} ({}/{}/{} points, plays {}); role {}{}",
            Who(row.low), p->GetLevel(), uint32(cls), RoleSpecIndex(cls, "healer") >= 0 ? "yes" : "no",
            RoleSpecIndex(cls, "tank") >= 0 ? "yes" : "no", uint32(AiFactory::GetPlayerSpecTab(p)), tabs[0], tabs[1],
            tabs[2], SpecRoleNow(p), role, planned.empty() ? " (its talents)" : " (planned)");
        {
            std::lock_guard<std::mutex> guard(roleLock_);
            if (auto const a = roleAssign_.find(row.low); a != roleAssign_.end())
                line += Acore::StringFormat(", role spec {}, solo spec {}", SpecName(cls, a->second.spec),
                                            SpecName(cls, a->second.solo));
        }
        line += p->GetSpecsCount() > 1
                    ? Acore::StringFormat("; dual spec yes (active spec {} of 2)", uint32(p->GetActiveSpec()) + 1)
                    : std::string("; dual spec no");
        if (std::string& logged = rosterLogged_[row.low]; logged != line)
        {
            logged = line;
            LOG_INFO("playerbots", "[roster] {}", line);
        }
    }
    std::string const guildText = Acore::StringFormat(
        "{} members: healers {} of the raid roster's {}, tanks {} of {}, dps {}", total, guildRoles["healer"],
        (total * RAID_HEALERS_PER_25 + 24) / 25, guildRoles["tank"], (total * RAID_TANKS_PER_25 + 24) / 25,
        guildRoles["dps"]);
    if (rolesWhy_["guild"] != guildText)
    {
        rolesWhy_["guild"] = guildText;
        LOG_INFO("playerbots", "[roles59] guild: {}", guildText);
    }
    auto joinNames = [](std::vector<std::string> const& v)
    {
        std::string out;
        for (std::string const& s : v)
            out += (out.empty() ? "" : ", ") + s;
        return out.empty() ? std::string("none") : out;
    };
    std::string const rosterText = Acore::StringFormat(
        "guild roles: healers {} -> target {} ({}), tanks {} -> target {} ({}), dps {}; respec plan: {}",
        guildRoles["healer"], (total * RAID_HEALERS_PER_25 + 24) / 25, joinNames(holders["healer"]),
        guildRoles["tank"], (total * RAID_TANKS_PER_25 + 24) / 25, joinNames(holders["tank"]), guildRoles["dps"],
        joinNames(respecPlan));
    if (rolesWhy_["roster1"] != rosterText)
    {
        rolesWhy_["roster1"] = rosterText;
        LOG_INFO("playerbots", "[roster1] {}", rosterText);
    }
}

bool GuildCoordinator::Cleared(std::string const& contentKey, uint32 low) const
{
    auto const itr = cleared_.find(contentKey);
    return itr != cleared_.end() && itr->second.count(low);
}

std::vector<GuildCoordinator::FiveCandidate> GuildCoordinator::Candidates(DungeonOption const& d, bool band,
                                                                          uint32* heldByOrder)
{
    std::vector<FiveCandidate> out;
    for (PlanRow const& row : plan_)
    {
        if (row.grp < 'A' || row.grp > 'C')
            continue;
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || IsHuman(p) || !FreeForFive(row.low, p))
            continue;
        std::string refusal = DungeonRefusal(d, p);
        // [order362] user rule 2026-11-11 (all 75 members, no exceptions): guild-wide a member that outlevelled the
        // dungeon uncleared still clears it - it walks in (the finder's band ends at its max, the instance does not)
        bool overMax = false;
        if (!refusal.empty() && band && !sPlayerbotAIConfig.GuildDungeonOrderPerMember &&
            p->GetLevel() > d.maxLevel && !Cleared(d.contentKey, row.low))
        {
            refusal = DungeonRefusal(d, p, true, true);
            overMax = refusal.empty();
        }
        if (!refusal.empty() && (band || refusal.rfind("L", 0) != 0))
            continue;
        // [lfg69] refused for this dungeon at its level (its walk there, or the finder): left out until it levels
        if (auto const r = refusedFor_.find(d.contentKey); r != refusedFor_.end())
            if (auto const m = r->second.find(row.low); m != r->second.end() && m->second == p->GetLevel())
                continue;
        FiveCandidate c;
        c.low = row.low;
        c.player = p;
        c.role = PlanRole(row.low, p);
        c.overMax = overMax;
        // every planned member (A, B and C) clears every dungeon, in its band (user 2026-11-08)
        c.needsClear = !Cleared(d.contentKey, row.low);
        // [order360] the dungeon order: guild-wide only the guild's open dungeon (its uncleared members and helpers);
        // per member its first clear once the earlier dungeons of its band are cleared (a repeat is never held)
        if (band && (c.needsClear || !sPlayerbotAIConfig.GuildDungeonOrderPerMember) &&
            !OrderHold(d, row.low, p).empty())
        {
            if (heldByOrder)
                ++*heldByOrder;
            continue;
        }
        c.enchanter = p->HasSkill(SKILL_ENCHANTING) && p->HasSpell(SPELL_DISENCHANT);
        // [lag336] a crafter called for the members' orders with its profession town stop due (a BUY / TRAIN call
        // runnable at its vendor's / trainer's town, [lag326] / [prof272])
        if (auto const st = bots_.find(row.low); st != bots_.end() && st->second.ordersCall)
            for (GuildOrder const& o : st->second.orders)
                if (o.type == GuildOrderType::PROFESSION && o.executable && o.townArea &&
                    (o.profession == GuildProfessionKind::BUY || o.profession == GuildProfessionKind::TRAIN ||
                     o.profession == GuildProfessionKind::CRAFT))  // [lag342] its craft's town too
                    c.townStop = true;
        out.push_back(c);
    }
    return out;
}

// [order360] user rule 2026-11-11 («Я тебя просил идти всех по порядку», «и так со всеми остальными инстансами»): the
// guild takes the dungeons one at a time by difficulty - min level, then max level, equal bands in the list's order -
// and the next opens once every member has fully cleared the current one. b446 five 9 went to the Stockade with five
// members none of whom had cleared Wailing Caverns or Shadowfang Keep
void GuildCoordinator::UpdateDungeonOrder()
{
    if (orderIdx_.size() != dungeons_.size())
    {
        orderIdx_.clear();
        for (size_t index = 0; index < dungeons_.size(); ++index)
            orderIdx_.push_back(index);
        std::stable_sort(orderIdx_.begin(), orderIdx_.end(),
                         [this](size_t a, size_t b)
                         {
                             DungeonOption const& da = dungeons_[a];
                             DungeonOption const& db = dungeons_[b];
                             return da.minLevel != db.minLevel ? da.minLevel < db.minLevel : da.maxLevel < db.maxLevel;
                         });
        orderPos_.assign(dungeons_.size(), 0);
        for (size_t pos = 0; pos < orderIdx_.size(); ++pos)
            orderPos_[orderIdx_[pos]] = pos;
        orderOpen_.clear();
        orderHoldLogged_.clear();
    }
    if (sPlayerbotAIConfig.GuildDungeonOrderPerMember)
    {
        orderOpenKey_.clear();
        return;
    }
    bool const finder = sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER);
    TeamId team = TEAM_NEUTRAL;
    for (PlanRow const& row : plan_)
        if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low)))
        {
            team = p->GetTeamId();
            break;
        }
    std::set<std::string> passed;
    std::string notCounted;
    std::string held;
    uint32 heldCount = 0;
    DungeonOption const* open = nullptr;
    for (size_t index : orderIdx_)
    {
        DungeonOption const& d = dungeons_[index];
        if (passed.count(d.contentKey))
            continue;
        // the guild cannot run it: no Dungeon Finder port, and the entrance in the other faction's capital
        if (!(finder && d.lfgId) && team != TEAM_NEUTRAL)
            if (char const* capital = EnemyCapital(d.entranceZone, team))
            {
                notCounted += Acore::StringFormat("; {} skipped: no finder port, entrance in an enemy capital {}",
                                                  d.contentKey, capital);
                continue;
            }
        uint32 keyMax = 0;  // a dungeon of several options (Maraudon's wings): the highest band's top
        for (DungeonOption const& o : dungeons_)
            if (o.contentKey == d.contentKey)
                keyMax = std::max(keyMax, o.maxLevel);
        std::string names;
        std::string gone;
        uint32 count = 0;
        uint32 goneCount = 0;
        std::vector<uint32> below;  // [lag368] its uncleared members below its band
        for (PlanRow const& row : plan_)
        {
            if (row.grp < 'A' || row.grp > 'C' || Cleared(d.contentKey, row.low))
                continue;
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
            if (p && IsHuman(p))
                continue;
            // [order362] outlevelled it uncleared: it still clears it by the walk in ([order362] Candidates) - only one
            // that cannot enter at all (the instance's own entry ceiling, the entrance in an enemy capital) is not
            // counted; another continent is only a journey
            if (p && p->GetLevel() > keyMax)
                if (std::string const why = DungeonRefusal(d, p, true, true);
                    !why.empty() && why.rfind(REFUSAL_OTHER_CONTINENT, 0) != 0)
                {
                    ++goneCount;
                    gone += Acore::StringFormat(" {} L{} ({}),", Who(row.low), p->GetLevel(), why);
                    continue;
                }
            ++count;
            if (p && p->GetLevel() < d.minLevel)
                below.push_back(row.low);
            names += " " + Who(row.low) + (p ? " L" + std::to_string(p->GetLevel()) : std::string(" offline")) + ",";
        }
        if (goneCount)
        {
            gone.pop_back();
            notCounted += Acore::StringFormat("; {} not counted: {} outlevelled it uncleared and cannot enter ({})",
                                              d.contentKey, goneCount, gone.substr(1));
        }
        if (!count)
        {
            passed.insert(d.contentKey);
            continue;
        }
        names.pop_back();
        held = names.substr(1);
        heldCount = count;
        open = &d;
        {
            std::lock_guard<std::mutex> guard(orderHeldLock_);
            orderHeldBelow_ = std::unordered_set<uint32>(below.begin(), below.end());
        }
        break;
    }
    if (!open)
    {
        std::lock_guard<std::mutex> guard(orderHeldLock_);
        orderHeldBelow_.clear();
    }
    std::string const line =
        (open ? Acore::StringFormat("open up to {} ({} L{}-{}), held by {}: {} members not cleared ({})",
                                    open->contentKey, open->name, open->minLevel, open->maxLevel, open->contentKey,
                                    heldCount, held)
              : std::string("every dungeon cleared by every member, none held")) +
        notCounted;
    if (line != orderLogged_)
    {
        orderLogged_ = line;
        LOG_INFO("playerbots", "[order360] guild order: {}", line);
    }
    std::string const key = open ? open->contentKey : "";
    if (key != orderOpenKey_)
    {
        orderOpenKey_ = key;
        if (open)
            Decision("[order360] the guild's dungeon is now " + open->name + ": no five goes elsewhere until every "
                     "member has cleared it");
    }
}

bool GuildCoordinator::HoldsGuildDungeon(uint32 low) const
{
    {
        std::lock_guard<std::mutex> guard(orderHeldLock_);
        if (orderHeldBelow_.count(low))
            return true;
    }
    return UnlockWeight(low) > 1.0f;  // [assist22] the gate-held member whose levels are the unlock
}

std::string GuildCoordinator::OrderHold(DungeonOption const& d, uint32 low, Player* p)
{
    if (orderPos_.size() != dungeons_.size() || &d < dungeons_.data() || &d >= dungeons_.data() + dungeons_.size())
        return "";
    if (!sPlayerbotAIConfig.GuildDungeonOrderPerMember)
        return orderOpenKey_.empty() || d.contentKey == orderOpenKey_ ? "" : "the guild's dungeon is " + orderOpenKey_;
    size_t const index = size_t(&d - dungeons_.data());
    uint8 const level = uint8(p->GetLevel());
    OrderOpen& mine = orderOpen_[low];
    if (mine.level != level || mine.gen != orderGen_)
    {
        mine.level = level;
        mine.gen = orderGen_;
        mine.open.clear();
        bool const finder = sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER);
        for (size_t pos = 0; pos < orderIdx_.size(); ++pos)
        {
            DungeonOption const& e = dungeons_[orderIdx_[pos]];
            if (level >= e.minLevel && level <= e.maxLevel && !Cleared(e.contentKey, low) &&
                ((finder && e.lfgId) || !EnemyCapital(e.entranceZone, p->GetTeamId())))
                mine.open.push_back(pos);
        }
    }
    std::string why;
    for (size_t pos : mine.open)
    {
        if (pos >= orderPos_[index])
            break;
        DungeonOption const& e = dungeons_[orderIdx_[pos]];
        if (e.contentKey == d.contentKey || why.find(e.contentKey) != std::string::npos)
            continue;
        // [lfg69] refused for it at this level: it cannot run it now
        if (auto const r = refusedFor_.find(e.contentKey); r != refusedFor_.end())
            if (auto const m = r->second.find(low); m != r->second.end() && m->second == level)
                continue;
        why += (why.empty() ? "" : ", ") + e.contentKey;
    }
    if (!why.empty())
        why += " not cleared";
    if (std::string& logged = orderHoldLogged_[{low, index}]; logged != why)
    {
        if (!why.empty())
            LOG_INFO("playerbots", "[order360] {} L{} not for {}: {}", Who(low), level, d.contentKey, why);
        else
            LOG_INFO("playerbots", "[order360] {} L{} for {}: every earlier dungeon cleared", Who(low), level,
                     d.contentKey);
        logged = why;
    }
    return why;
}

bool GuildCoordinator::PickFive(std::vector<FiveCandidate> const& pool, std::vector<FiveCandidate>& out) const
{
    std::vector<FiveCandidate> sorted = pool;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](FiveCandidate const& a, FiveCandidate const& b)
                     {
                         // [need106] [roles108] an open clear, then a needed upgrade, then (a healer) the
                         // dungeon's experience, then no need at all
                         auto const rank = [](FiveCandidate const& c)
                         { return c.needsClear ? 0 : c.upgrade ? 1 : c.xp ? 2 : 3; };
                         if (rank(a) != rank(b))
                             return rank(a) < rank(b);
                         bool const na = rank(a) < 3;
                         // [need106] a carrier (no need of its own): the fewest repeats of the dungeon first
                         if (!na && a.repeats != b.repeats)
                             return a.repeats < b.repeats;
                         return a.player->GetLevel() > b.player->GetLevel();
                     });
    out.clear();
    FiveCandidate const* tank = nullptr;
    FiveCandidate const* healer = nullptr;
    std::vector<FiveCandidate const*> dps;
    for (FiveCandidate const& c : sorted)
    {
        if (c.role == "tank" && !tank)
            tank = &c;
        else if (c.role == "healer" && !healer)
            healer = &c;
        else if (c.role == "dps" && dps.size() < 3)
            dps.push_back(&c);
    }
    if (!tank || !healer || dps.size() < 3)
        return false;
    // an enchanter in the five: spare BoP is disenchanted, not sold (the user's loot rule)
    if (!tank->enchanter && !healer->enchanter &&
        std::none_of(dps.begin(), dps.end(), [](FiveCandidate const* c) { return c->enchanter; }))
        for (FiveCandidate const& c : sorted)
            if (c.role == "dps" && c.enchanter && std::find(dps.begin(), dps.end(), &c) == dps.end())
            {
                dps.back() = &c;
                break;
            }
    out.push_back(*tank);
    out.push_back(*healer);
    for (FiveCandidate const* c : dps)
        out.push_back(*c);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// forming and disbanding (world thread)
// ---------------------------------------------------------------------------------------------------------------------
uint32 GuildCoordinator::FormFiveLocked(uint32 leaderLow, std::vector<uint32> const& members, DungeonOption const& d,
                                        std::map<uint32, std::string> const& roles, std::string const& why,
                                        bool forced, std::string* error)
{
    auto fail = [error](std::string const& text) -> uint32
    {
        if (error)
            *error = text;
        return 0;
    };
    if (members.size() != 5 || std::find(members.begin(), members.end(), leaderLow) == members.end())
        return fail("needs 5 members with the leader");
    for (uint32 low : members)
    {
        auto const st = bots_.find(low);
        if (st != bots_.end() && st->second.fiveId)
            return fail(Who(low) + " already in five " + std::to_string(st->second.fiveId));
    }

    GuildFive five;
    five.id = nextFiveId_++;
    five.leaderLow = leaderLow;
    five.members = members;
    five.dungeon = d.name;
    five.since = Now();
    five.contentKey = d.contentKey;
    five.lfgId = d.lfgId;
    five.entrance = d.entrance;
    five.entranceReach = d.reach;
    five.roles = roles;
    five.quartermasterLow = leaderLow;
    five.why = why;
    five.forced = forced;
    for (uint32 low : members)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        if (IsHuman(p))
            five.humans.insert(low);
        if (!five.enchanterLow && p && p->HasSkill(SKILL_ENCHANTING) && p->HasSpell(SPELL_DISENCHANT))
            five.enchanterLow = low;
        BotState& st = bots_[low];
        st.fiveId = five.id;
        st.supplyShort.clear();  // [supply27] a new five, or none: its supply check starts again
        SetConjureGoal(ObjectGuid::Create<HighGuid::Player>(low), 0);  // [dun199]
        st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
        st.fiveRefused.clear();
        // the five's order comes first: the member leaves its quest now (New RPG takes the order at IDLE)
        if (PlayerbotAI* ai = p ? GET_PLAYERBOT_AI(p) : nullptr)
            if (!IsHuman(p) && !p->IsInCombat())
                ai->rpgInfo.ChangeToIdle("it joined a five, the five's order comes first");
    }

    // [loot13] the five's goals: what its members need here and the mobs its route must kill for them
    {
        std::vector<Player*> players;
        for (uint32 low : members)
            players.push_back(ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low)));
        FiveGoals goals = GoalsFor(d, players);
        std::string text;
        for (auto const& [entry, mob] : goals.mobs)
            text += Acore::StringFormat("{}{} ({}{}): {}", text.empty() ? "" : " | ", mob.name, mob.boss ? "boss" : "",
                                        mob.boss ? "" : (mob.takenIfThere ? "the route takes it: " : "not taken: ") + mob.why,
                                        mob.items);
        LOG_INFO("playerbots", "[loot13] five {} ({}) goals: {}", five.id, d.contentKey, text.empty() ? "none" : text);
        std::lock_guard<std::mutex> goalGuard(goalLock_);
        for (uint32 low : members)
            goalFiveOf_[low] = five.id;
        goals_[five.id] = std::move(goals);
    }

    // [dun194] the drinks each mana user carries for this run, priced by the five gate's pulls: its mana need over
    // the dungeon (FiveGate::Verdict::manaNeed) plus one full refill (a rest from empty: its max mana), over the mana
    // of one drink (the best one the five holds: they share inside, RestoredPoints with the core's drink amount)
    {
        std::vector<Player*> players(2, nullptr);
        Player* healer2 = nullptr;  // [order367]
        float drinkMana = 0.0f;
        std::map<ObjectGuid, float> ownDrink;  // [dun230] each one's best drink
        for (uint32 low : members)
        {
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
            if (!p)
                continue;
            float const own = BestBagRestore(p, RESTOCK_SPELL_CATEGORY_DRINK);
            ownDrink[p->GetGUID()] = own;
            drinkMana = std::max(drinkMana, own);
            auto const role = roles.find(low);
            std::string const r = role != roles.end() ? role->second : "dps";
            if (r == "tank" && !players[0])
                players[0] = p;
            else if (r == "healer" && !players[1])
                players[1] = p;
            else
            {
                if (r == "healer" && !healer2)
                    healer2 = p;
                players.push_back(p);
            }
        }
        if (players[0] && players[1] && drinkMana > 0.0f)
        {
            FiveGate::Verdict const priced = FiveGate::Judge(d.mapId, players, healer2);
            std::string line;
            for (Player* p : players)
            {
                if (!p || !p->GetMaxPower(POWER_MANA))
                    continue;
                auto const need = priced.manaNeed.find(p->GetGUID());
                float const mana = (need != priced.manaNeed.end() ? need->second : 0.0f) +
                                   float(p->GetMaxPower(POWER_MANA));
                // [dun230] never below one full rest: its bar from empty over its own best drink (the five's best
                // when it holds none) - b444 wc-n five 12 priced the healer Farret at 1 drink beside 19-38 each
                float const ownBest = ownDrink[p->GetGUID()] > 0.0f ? ownDrink[p->GetGUID()] : drinkMana;
                uint32 const oneRest = uint32(std::ceil(float(p->GetMaxPower(POWER_MANA)) / ownBest));
                uint32 const drinks = std::max(uint32(std::ceil(mana / drinkMana)), oneRest);
                five.drinkNeed[p->GetGUID().GetCounter()] = drinks;
                auto const walk = priced.walkRegen.find(p->GetGUID());
                line += Acore::StringFormat("{}{} {} ({:.0f} mana, [dun230] walks {:.0f}, rest {})",
                                            line.empty() ? "" : ", ", p->GetName(), drinks, mana,
                                            walk != priced.walkRegen.end() ? walk->second : 0.0f, oneRest);
            }
            LOG_INFO("playerbots",
                     "[dun194] five {} ({}): drinks the run needs at {:.0f} mana a drink: {}; [dun230] the healer {}'s "
                     "heals {:.0f} mana less {:.0f} regen in the fights",
                     five.id, d.contentKey, drinkMana, line, players[1]->GetName(), priced.healingMana,
                     priced.fightRegen);
        }
    }

    std::string list;
    for (uint32 low : members)
    {
        auto const role = roles.find(low);
        list += (list.empty() ? "" : ", ") + Who(low) + " " + RoleRu(role != roles.end() ? role->second : "dps") +
                (low == leaderLow ? " (five leader, quartermaster)" : "");
    }
    Decision("five " + std::to_string(five.id) + " → " + d.name + ": " + list + "; enchanter " +
             (five.enchanterLow ? Who(five.enchanterLow) : std::string("none")) + " — " + why);
    // consumables: each member withdraws its share from the guild bank before leaving, once the bank exists (it
    // sleeps until the guild and its tab exist: the members go with what they carry)
    for (uint32 low : five.humans)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        Alert("HUMAN " + Who(low) + " in five " + std::to_string(five.id) + " (" + d.name +
              "): the five waits for him at the entrance, map " + std::to_string(d.entrance.GetMapId()) + " (" +
              std::to_string(int32(d.entrance.GetPositionX())) + "," + std::to_string(int32(d.entrance.GetPositionY())) +
              ")");
        if (p && p->GetSession())
            ChatHandler(p->GetSession())
                .PSendSysMessage(
                    "[Guild] Five {} goes to {}: we wait for you at the entrance (map {}, {:.0f}, {:.0f}). "
                    "We do not enter without you.",
                    five.id, d.name, d.entrance.GetMapId(), d.entrance.GetPositionX(), d.entrance.GetPositionY());
    }
    fives_[five.id] = five;
    return five.id;
}

uint32 GuildCoordinator::FormFive(uint32 leaderLow, std::vector<uint32> const& members, std::string const& dungeon,
                                  std::string* error, bool forced)
{
    DungeonOption const* d = FindDungeon(dungeon);
    if (!d)
    {
        if (error)
            *error = "no dungeon " + dungeon;
        return 0;
    }
    std::map<uint32, std::string> roles;
    for (uint32 low : members)
        if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low)))
            roles[low] = PlanRole(low, p);
    CoordLock guard(lock_, __func__);
    return FormFiveLocked(leaderLow, members, *d, roles, forced ? "order by command" : "global leader's decision",
                          forced, error);
}

void GuildCoordinator::DisbandFive(uint32 fiveId, std::string const& reason)
{
    bool running = false;
    {
        CoordLock guard(lock_, __func__);
        auto const itr = fives_.find(fiveId);
        if (itr == fives_.end())
            return;
        running = itr->second.stage == GuildFive::Stage::Run;
    }
    // a run: the dungeon leader stops it, its end hook (OnFiveRunEnd) hands authority back
    Leader::Dungeon::DungeonRuns& runs = Leader::Dungeon::DungeonRuns::Instance();
    if (running && !runs.Phase(fiveId).empty())
    {
        runs.Stop(fiveId, reason.empty() ? "five disbanded" : reason);  // [guild70] the five-leader's run
        return;
    }
    runs.ForgetFive(fiveId);
    CoordLock guard(lock_, __func__);
    auto const itr = fives_.find(fiveId);
    if (itr == fives_.end())
        return;
    for (uint32 low : itr->second.members)
    {
        BotState& st = bots_[low];
        st.fiveId = 0;
        st.supplyShort.clear();  // [supply27] a new five, or none: its supply check starts again
        SetConjureGoal(ObjectGuid::Create<HighGuid::Player>(low), 0);  // [dun199]
        st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
        st.fiveRefused.clear();
        if (st.run && st.runType == GuildOrderType::DUNGEON)
        {
            st.run = false;
            // its walk to the entrance ends: back to its own choice (quests)
            if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low)))
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                    if (ai->rpgInfo.GetStatus() == RPG_GO_CAMP)
                        ai->rpgInfo.ChangeToIdle("its five is disbanded, back to its own plan");
        }
    }
    Decision("five " + std::to_string(fiveId) + " (" + itr->second.dungeon + ") disbanded" +
             (reason.empty() ? "" : ": " + reason) + " — authority back with the global leader, members quest");
    fives_.erase(itr);
}

// [lag321] a gathering five whose member's own walk to the entrance is refused ([lfg69], FiveStep) loses that member
// only: the free member of its role the global leader would pick (Candidates, the walk in, a dps with a need) takes
// its place when the five gate clears the five with it - the others' walks go on. Before, the whole five was called
// off and the next look formed a new one round the same tank: b437 fives 8, 9, 10 and 11 to Deadmines, each with
// Rollothan L13 as tank, disbanded in turn on Lanbuzz, Gilduck, Namwe and Geefeelli's refused walks - Rollothan's
// quests interrupted four times, a 198 s flight to Westfall and 7 min of refused walks; b434 5 such disbands
bool GuildCoordinator::ReplaceRefusedMember(GuildFive& five, uint32 refusedLow)
{
    if (refusedLow == five.leaderLow || five.forced || five.humans.count(refusedLow) ||
        five.stage != GuildFive::Stage::Gather)
        return false;
    DungeonOption const* d = FindDungeon(five.contentKey);
    if (!d)
        return false;
    auto const roleItr = five.roles.find(refusedLow);
    std::string const role = roleItr != five.roles.end() ? roleItr->second : "dps";
    std::vector<FiveCandidate> fits;
    for (FiveCandidate const& c : Candidates(*d, true))
        if (c.role == role && DungeonRefusal(*d, c.player, true, c.overMax).empty() &&
            (c.needsClear || !NeededUpgrade(*d, c.player).empty() || (role != "dps" && GivesXp(*d, c.player))))
            fits.push_back(c);
    // PickFive's order: an open clear first, then the highest level
    std::stable_sort(fits.begin(), fits.end(),
                     [](FiveCandidate const& a, FiveCandidate const& b)
                     {
                         if (a.townStop != b.townStop)  // [lag336] a crafter with its town stop due last
                             return b.townStop;
                         if (a.needsClear != b.needsClear)
                             return a.needsClear;
                         return a.player->GetLevel() > b.player->GetLevel();
                     });
    constexpr size_t TRIES = 4;  // AutoFive's gate cost bound (PAIRS_SIDE): the first four of the role
    if (fits.size() > TRIES)
        fits.resize(TRIES);
    for (FiveCandidate const& c : fits)
    {
        // the gate's order: the tank, the healer, then the dps
        std::vector<Player*> players(2, nullptr);
        Player* healer2 = nullptr;  // [order367]
        bool everyone = true;
        for (uint32 low : five.members)
        {
            uint32 const who = low == refusedLow ? c.low : low;
            Player* p = who == c.low ? c.player : ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(who));
            if (!p)
            {
                everyone = false;
                break;
            }
            auto const r = five.roles.find(low);
            std::string const memberRole = low == refusedLow ? role : r != five.roles.end() ? r->second : "dps";
            if (memberRole == "tank" && !players[0])
                players[0] = p;
            else if (memberRole == "healer" && !players[1])
                players[1] = p;
            else
            {
                if (memberRole == "healer" && !healer2)
                    healer2 = p;
                players.push_back(p);
            }
        }
        if (!everyone)
            return false;
        FiveGate::Verdict const verdict = FiveGate::Judge(d->mapId, players, healer2);
        if (!verdict.clears)
            continue;

        // the refused member goes back to its own plan (as DisbandFive does for each member)
        {
            BotState& st = bots_[refusedLow];
            st.fiveId = 0;
            st.supplyShort.clear();
            st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
            st.fiveRefused.clear();
            if (st.run && st.runType == GuildOrderType::DUNGEON)
            {
                st.run = false;
                if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(refusedLow)))
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p); ai && ai->rpgInfo.GetStatus() == RPG_GO_CAMP)
                        ai->rpgInfo.ChangeToIdle("it was left out of the five, back to its own plan");
            }
        }
        std::replace(five.members.begin(), five.members.end(), refusedLow, c.low);
        five.roles.erase(refusedLow);
        five.roles[c.low] = role;
        five.arrived.erase(refusedLow);
        five.drinkNeed.erase(refusedLow);
        if (five.enchanterLow == refusedLow)
            five.enchanterLow = 0;
        if (!five.enchanterLow && c.enchanter)
            five.enchanterLow = c.low;
        // the new member takes the five's order now (FormFiveLocked's per-member start)
        {
            BotState& st = bots_[c.low];
            st.fiveId = five.id;
            st.supplyShort.clear();
            st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
            st.fiveRefused.clear();
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(c.player); ai && !c.player->IsInCombat())
                ai->rpgInfo.ChangeToIdle("it joined a five, the five's order comes first");
        }
        // [dun194] its drinks for the run, priced as the five's were at its forming
        if (c.player->GetMaxPower(POWER_MANA))
        {
            float drinkMana = 0.0f;
            for (Player* p : players)
                if (p)
                    drinkMana = std::max(drinkMana, BestBagRestore(p, RESTOCK_SPELL_CATEGORY_DRINK));
            if (drinkMana > 0.0f)
            {
                auto const need = verdict.manaNeed.find(c.player->GetGUID());
                float const mana = (need != verdict.manaNeed.end() ? need->second : 0.0f) +
                                   float(c.player->GetMaxPower(POWER_MANA));
                five.drinkNeed[c.low] = uint32(std::ceil(mana / drinkMana));
            }
        }
        LOG_INFO("playerbots", "[lag321] five {} ({}): {} {} leaves it (no safe walk to the entrance), {} L{} takes "
                 "its place ({}): {}",
                 five.id, five.contentKey, role, Who(refusedLow), Who(c.low), c.player->GetLevel(),
                 c.needsClear ? "an open clear" : "a needed upgrade or the dungeon's experience", verdict.why);
        Decision("five " + std::to_string(five.id) + " (" + five.dungeon + "): " + Who(refusedLow) +
                 " has no safe way to the entrance — " + Who(c.low) + " " + RoleRu(role) +
                 " takes its place, the others walk on");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// the global leader's look at the fives (every coordinator update, world thread)
// ---------------------------------------------------------------------------------------------------------------------
std::string GuildCoordinator::FiveWalkRefusal(GuildFive const& five) const
{
    DungeonOption const* d = FindDungeon(five.contentKey);
    if (!d)
        return "";
    for (uint32 low : five.members)
        if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low)))
            if (std::string const why = DungeonRefusal(*d, p, true); !why.empty() && why.rfind("L", 0) != 0)
                return Who(low) + ": " + why;
    return "";
}

void GuildCoordinator::UpdateFives()
{
    // [cleft62] the server announced its shutdown (the countdown every player sees): a five's bots inside their dungeon
    // leave it as players do (the Dungeon Finder's teleport out, else the way they came: Player::TeleportToEntryPoint).
    // An instance gone at the restart puts its players at the dungeon's entrance instead (Player::LoadFromDB) - b393's
    // RFC five was inside at the stop and Alliance Orenem / Brelir logged in in Orgrimmar's Cleft of Shadow, dead to
    // its guards, ghosts there in every later run
    if (sWorld->IsShuttingDown())
    {
        std::vector<ObjectGuid> out;
        {
            CoordLock guard(lock_, __func__);
            for (auto const& [id, five] : fives_)
                if (five.stage == GuildFive::Stage::Run)
                    for (uint32 low : five.members)
                        out.push_back(ObjectGuid::Create<HighGuid::Player>(low));
        }
        for (ObjectGuid const& guid : out)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(guid);
            if (!p || IsHuman(p) || !p->IsInWorld() || p->IsBeingTeleported() || !p->GetMap()->IsDungeon())
                continue;
            if (!shutdownOut_.insert(guid.GetCounter()).second)
                continue;
            bool const lfg = p->GetGroup() && p->GetGroup()->isLFGGroup();
            if (lfg)
                sLFGMgr->TeleportPlayer(p, true);
            else
                p->TeleportToEntryPoint();
            LOG_INFO("playerbots", "[cleft62] {} L{} leaves {} for the server's shutdown ({})", p->GetName(),
                     p->GetLevel(), p->GetMap()->GetMapName(), lfg ? "the Dungeon Finder's teleport out" :
                                                                      "back where it entered");
        }
    }
    // [run80] a planned bot inside a dungeon instance with no five of its own (its run ended while it fought, a
    // restored login) leaves it once out of combat: no unled member fights a dungeon's packs on its own (b406 rfc-n
    // five 4: 4 deaths in Taragaman's room after the run had ended "stalled")
    {
        std::vector<ObjectGuid> stray;
        {
            CoordLock guard(lock_, __func__);
            for (PlanRow const& row : plan_)
                if (auto const st = bots_.find(row.low); st == bots_.end() || !st->second.fiveId)
                    stray.push_back(ObjectGuid::Create<HighGuid::Player>(row.low));
        }
        for (ObjectGuid const& guid : stray)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(guid);
            if (!p || IsHuman(p) || !GET_PLAYERBOT_AI(p) || !p->IsInWorld() || p->IsBeingTeleported() ||
                !p->GetMap()->IsDungeon() || p->IsInCombat() || !p->IsAlive() || Leader::LedBot(p))
                continue;  // a led bot is some run's (a harness test, a five's): its leader holds it there
            bool const lfg = p->GetGroup() && p->GetGroup()->isLFGGroup();
            if (lfg)
                sLFGMgr->TeleportPlayer(p, true);
            else
                p->TeleportToEntryPoint();
            LOG_INFO("playerbots", "[run80] {} L{} leaves {}: no five holds it there ({})", p->GetName(), p->GetLevel(),
                     p->GetMap()->GetMapName(), lfg ? "the Dungeon Finder's teleport out" : "back where it entered");
        }
    }
    // [coop259] a member the global leader took into a five leaves its help / co-op party and its open help request
    // at once (the party's own exits are New RPG ticks, and a five member runs none): its helpers go free
    {
        std::vector<std::pair<uint32, uint32>> taken;  // (member, five)
        {
            CoordLock guard(lock_, __func__);
            for (auto const& [id, five] : fives_)
                if (five.stage == GuildFive::Stage::Gather || five.stage == GuildFive::Stage::Run)
                    for (uint32 low : five.members)
                        taken.emplace_back(low, id);
        }
        for (auto const& [low, id] : taken)
            NewRpgCoop::LeaveForFive(low, id);
    }
    // [group132] a planned bot with no five of its own left in a five's old group - a Dungeon Finder group, or one
    // whose leader is no longer its member - leaves it: a grouped bot that does not lead gets "follow" in place of New
    // RPG (AiFactory) and follows nobody. b424/b425: Keenonk, Ainne, Dandolar, Kylanor, Mytaeleas, Blahadrin logged
    // in into LFG groups whose leaders (Colian, Ravelandy, Farret) had left them, IDLE for the whole run (Keenonk 4682
    // s at (-592,-2127)). A co-op party (an ordinary group its leader is in) and a group with a human stay as they are
    {
        std::vector<ObjectGuid> loose;
        {
            CoordLock guard(lock_, __func__);
            for (PlanRow const& row : plan_)
                if (auto const st = bots_.find(row.low); st == bots_.end() || !st->second.fiveId)
                    loose.push_back(ObjectGuid::Create<HighGuid::Player>(row.low));
        }
        for (ObjectGuid const& guid : loose)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(guid);
            Group* group = p ? p->GetGroup() : nullptr;
            PlayerbotAI* botAI = p ? GET_PLAYERBOT_AI(p) : nullptr;
            if (!group || !botAI || IsHuman(p) || !p->IsInWorld() || p->IsBeingTeleported() || p->IsInCombat() ||
                p->GetMap()->IsDungeon() || Leader::LedBot(p) || group->isRaidGroup() || group->isBGGroup() ||
                group->isBFGroup())
                continue;
            bool const leaderGone = !group->IsMember(group->GetLeaderGUID());
            if (!group->isLFGGroup() && !leaderGone)
                continue;
            bool human = false;
            for (GroupReference* ref = group->GetFirstMember(); ref && !human; ref = ref->next())
                human = IsHuman(ref->GetSource());
            if (human)
                continue;
            std::string const why = group->isLFGGroup() ? "a Dungeon Finder group with no five" : "its leader left it";
            LOG_INFO("playerbots", "[group132] {} L{} leaves group {} ({}, {} members): {}", p->GetName(),
                     p->GetLevel(), group->GetGUID().GetCounter(), group->isLFGGroup() ? "LFG" : "party",
                     group->GetMembersCount(), why);
            group->RemoveMember(p->GetGUID());
            botAI->ResetStrategies(false);
        }
    }
    std::vector<std::pair<uint32, std::string>> cancel;
    // [guild70] fives the five-leader takes in now: (id, by the Dungeon Finder)
    std::vector<std::pair<uint32, bool>> ready;
    Leader::Dungeon::DungeonRuns& runs = Leader::Dungeon::DungeonRuns::Instance();
    // the five-leader's run request for a five (lock_ held)
    auto const request = [this](GuildFive const& five, bool viaLfg)
    {
        Leader::Dungeon::FiveRunRequest r;
        r.fiveId = five.id;
        r.contentKey = five.contentKey;
        r.name = five.dungeon;
        DungeonOption const* d = FindDungeon(five.contentKey);
        r.mapId = d ? d->mapId : 0;
        TestHarness::ContentDef const* content = TestHarness::FindContent(five.contentKey);
        r.difficulty = content ? content->difficulty : 0;
        r.lfgId = five.lfgId;
        r.entrance = five.entrance;
        r.entranceReach = five.entranceReach;
        r.leader = ObjectGuid::Create<HighGuid::Player>(five.leaderLow);
        r.viaLfg = viaLfg;
        auto const add = [&](uint32 low)
        {
            auto const role = five.roles.find(low);
            r.members.push_back({ObjectGuid::Create<HighGuid::Player>(low),
                                 role != five.roles.end() ? role->second : "dps", five.humans.count(low) != 0});
        };
        add(five.leaderLow);
        for (uint32 low : five.members)
            if (low != five.leaderLow)
                add(low);
        return r;
    };
    std::map<uint32, Leader::Dungeon::FiveRunRequest> requests;
    {
        CoordLock guard(lock_, __func__);
        for (auto& [id, five] : fives_)
        {
            if (five.stage != GuildFive::Stage::Gather)
                continue;
            std::string why;
            uint32 refusedLow = 0;  // [lag321]
            for (uint32 low : five.members)
            {
                Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                if (!p)
                {
                    why = Who(low) + " offline";
                    break;
                }
                BotState& st = bots_[low];
                if (!st.fiveRefused.empty())
                {
                    why = Who(low) + ": " + st.fiveRefused;
                    refusedLow = low;
                    break;
                }
                bool const at = p->IsInWorld() && p->IsAlive() && !p->IsBeingTeleported() &&
                                p->GetMapId() == five.entrance.GetMapId() &&
                                p->GetExactDist(five.entrance) <= five.entranceReach;
                if (at)
                    five.arrived.insert(low);
                else
                    five.arrived.erase(low);
            }
            if (!why.empty())
            {
                // [lag321] a member whose own walk to the entrance is refused leaves the five alone: a free member of
                // its role takes its place when the gate clears the five with it, else the five is called off
                if (refusedLow && ReplaceRefusedMember(five, refusedLow))
                    continue;
                cancel.emplace_back(id, why);
                continue;
            }
            // [supply27] user design: before the five queues or walks in each bot member has its supplies - drink
            // for a mana user, food for all (BuyAction's restock line) - else it restocks at a vendor with its own
            // money first (its New RPG vendor run; the quartermaster's trade tops it up inside, GuildTransfer c.).
            // A member whose run was made or could not be made goes in as it is (logged)
            // [supply100] coordinator 2026-11-10: the five's pooled supplies decide, not each member's. Inside the
            // dungeon share92 tops every member up from the one holding most (they stand together at the run's
            // start), the mages give water, the healer heals what food would. The five goes in when its mana users
            // hold, together, the restock line each (RESTOCK_FOOD_MIN x mana users); a short member's own vendor
            // run or supply gold never holds a five that the pool carries. b415: no five entered in 20 min - the
            // waits lasted up to ~1290 s (Ellosera, five 4, "the guild's supply gold is on its way"), ~1050 s
            // (Dekorhan, five 10), ~810 s (Selalleas, five 10), ~330 s (Mutil, five 1)
            // [dun194] a mana user's drink line: the drinks the run needs it to carry (priced at the five's forming)
            // [dun198] a mage that knows Conjure Water covers its own line (it conjures, in the run too), never the
            // five's: the others' drink is counted in their bags (b436 dm-n five 7: "a mage conjures water" let the
            // five in, the mage Feeli then stood at 6 % mana with no drink)
            auto const drinkLine = [&five](uint32 low) -> uint32
            {
                Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                if (p && p->getClass() == CLASS_MAGE && p->HasSpell(5504))  // Conjure Water
                    return 0;
                auto const priced = five.drinkNeed.find(low);
                return priced != five.drinkNeed.end() ? priced->second : RESTOCK_FOOD_MIN;
            };
            {
                uint32 manaUsers = 0, drinks = 0, food = 0, mages = 0, drinksNeeded = 0;
                std::string noRoom;  // [supply104] a short member the pool cannot reach: no free bag slot
                for (uint32 low : five.members)
                {
                    Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                    if (!p || !p->IsInWorld())
                        continue;
                    if (p->GetMaxPower(POWER_MANA))
                    {
                        ++manaUsers;
                        drinksNeeded += drinkLine(low);
                        uint32 const own = CountBagFood(p, RESTOCK_SPELL_CATEGORY_DRINK);
                        drinks += own;
                        // [supply104] the hand-over must be possible: a member below the line takes a stack only
                        // into a free slot (the giver's side: share103 hands the whole stack when it cannot split)
                        if (own < drinkLine(low) && !p->GetFreeInventorySpace())
                            noRoom += (noRoom.empty() ? "" : ", ") + p->GetName();
                    }
                    food += CountBagFood(p, RESTOCK_SPELL_CATEGORY_FOOD);
                    mages += p->getClass() == CLASS_MAGE && p->HasSpell(5504) ? 1 : 0;  // Conjure Water
                }
                // [dun199] a mage that knows Conjure Water conjures before the run up to the five's priced line: its
                // own count plus what the five's other mana users lack of theirs (their bags now); the members get it
                // by trade when they meet (the share of the five: c.), the pool counts it as it lands in the bags
                {
                    uint32 othersShort = 0;
                    std::vector<Player*> conjurers;
                    for (uint32 low : five.members)
                    {
                        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                        if (!p || !p->IsInWorld() || !p->GetMaxPower(POWER_MANA))
                            continue;
                        if (p->getClass() == CLASS_MAGE && p->HasSpell(5504))  // Conjure Water
                        {
                            conjurers.push_back(p);
                            continue;
                        }
                        uint32 const held = CountBagFood(p, RESTOCK_SPELL_CATEGORY_DRINK);
                        othersShort += drinkLine(low) > held ? drinkLine(low) - held : 0;
                    }
                    for (Player* mage : conjurers)
                    {
                        auto const own = five.drinkNeed.find(mage->GetGUID().GetCounter());
                        uint32 const goal = (own != five.drinkNeed.end() ? own->second : RESTOCK_FOOD_MIN) +
                                            othersShort / uint32(conjurers.size());
                        if (goal != ConjureGoal(mage->GetGUID()))
                        {
                            SetConjureGoal(mage->GetGUID(), goal);
                            uint32 const forOthers = othersShort / uint32(conjurers.size());
                            LOG_INFO("playerbots",
                                     "[dun199] five {} ({}): {} conjures water up to {} before the run (its own line + "
                                     "{} the others lack)",
                                     id, five.contentKey, mage->GetName(), goal, forOthers);
                        }
                    }
                }
                bool const pooled = noRoom.empty() && drinks >= drinksNeeded;
                if (!noRoom.empty() && !five.poolGoesIn)
                    if (std::string& logged = fiveLfgWhy[id + 1000000]; logged != noRoom)
                    {
                        logged = noRoom;
                        LOG_INFO("playerbots", "[supply104] five {} ({}): the pool cannot reach {}: no free bag slot for "
                                 "a stack - restocks or frees a slot first",
                                 id, five.contentKey, noRoom);
                    }
                std::string const poolText = Acore::StringFormat("pool {} drinks for {} mana users ([dun194] {} the "
                                                                 "run needs){}, {} food",
                                                                 drinks, manaUsers, drinksNeeded,
                                                                 mages ? ", a mage conjures its own" : "", food);
                if (pooled)
                {
                    if (!five.poolGoesIn)
                    {
                        LOG_INFO("playerbots", "[supply100] five {} ({}): its supplies pooled carry the run: {}; goes "
                                 "in, topped up inside",
                                 id, five.contentKey, poolText);
                        five.poolGoesIn = true;
                    }
                    for (uint32 low : five.members)
                        bots_[low].supplyDone = true;  // no member's own restock holds it
                }
                // [dun280] coordinator 2026-11-13: the pool is read again every pass until the five is in, and a
                // five whose pool has fallen back below the lines' total does not go: the verdict was latched, so a
                // five cleared minutes earlier walked in on a figure its members had since drunk. b475 five 20
                // (dm-n) was cleared at "pool 64 drinks ... 46 the run needs" and arrived at "[dun208] five 20 drink
                // (pool 4 of the lines' 46, short)"; five 16 at "pool 110 of 58" arrived at 28 of 58. Its members'
                // own restock is armed again by the same fact (their vendor run, a mage's conjuring, the five's
                // trades), so the five is supplied and goes - it is not parked
                else if (five.poolGoesIn)
                {
                    LOG_INFO("playerbots", "[dun280] five {} ({}): its pool no longer carries the run: {}; it is "
                             "supplied before it goes in",
                             id, five.contentKey, poolText);
                    five.poolGoesIn = false;
                    for (uint32 low : five.members)
                    {
                        BotState& st = bots_[low];
                        st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
                    }
                }
            }
            // [dun280] the pool verdict above is the only one: a second count of the same drinks ran here, without
            // the [supply104] bag-room term and without the [dun199] conjure order, and could latch poolGoesIn back
            // on after the first had cleared it. One judge, one verdict (the user's rule on duplicate judges)
            bool supplied = true;
            for (uint32 low : five.members)
            {
                Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                if (!p || IsHuman(p) || !GET_PLAYERBOT_AI(p))
                    continue;
                BotState& st = bots_[low];
                // [eco199] coordinator 2026-11-11: the five's leader takes the members' gold its planned need waits for
                // before the five goes in, when a mailbox is on its way (b430: Farret, five 8's leader, 27.3 g unread
                // in 19 letters across b426-b430); with none on its way the five goes on and the gold waits
                if (low == five.leaderLow && needGoldOnWay_.count(low))
                {
                    static std::set<std::pair<uint32, uint32>> goldWaitLogged;  // (five, leader) logged once
                    if (goldWaitLogged.insert({id, low}).second)
                        LOG_INFO("playerbots",
                                 "[eco199] five {} ({}): waits for its leader {}: takes the members' gold its planned "
                                 "need waits for at the mailbox on its way",
                                 id, five.contentKey, Who(low));
                    supplied = false;
                    continue;
                }
                std::string shortOf;
                uint32 const line = drinkLine(low);  // [dun194]
                uint32 const drink =
                    p->GetMaxPower(POWER_MANA) ? CountBagFood(p, RESTOCK_SPELL_CATEGORY_DRINK) : line;
                uint32 const food = CountBagFood(p, RESTOCK_SPELL_CATEGORY_FOOD);
                // [supply113] the guild cook's letters (sent only: a planned letter counts nothing) - the core's
                // delivery time (MailDeliveryDelay for items between accounts, HandleSendMail; 0 here) and the
                // member's own take at a mailbox (its collect-mail order) before the five goes: the finder ports it
                // from anywhere, a letter left in the box is no food inside
                uint32 mailDrink = 0, mailFood = 0;
                uint32 arrivesAt = 0;
                for (auto const& [tid, t] : transfers_)
                    if (t.mailed && !t.gold && t.toLow == low)
                        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(t.itemId);
                            proto && proto->Class == ITEM_CLASS_CONSUMABLE)
                        {
                            if (proto->Spells[0].SpellCategory == RESTOCK_SPELL_CATEGORY_DRINK &&
                                p->GetMaxPower(POWER_MANA))
                                mailDrink += t.count;
                            else if (proto->Spells[0].SpellCategory == RESTOCK_SPELL_CATEGORY_FOOD)
                                mailFood += t.count;
                            else
                                continue;
                            arrivesAt = std::max(arrivesAt, t.deliverAt);
                        }
                if ((drink < line || food < RESTOCK_FOOD_MIN) && (mailDrink || mailFood) &&
                    drink + mailDrink >= line && food + mailFood >= RESTOCK_FOOD_MIN)
                {
                    uint32 const nowSec = uint32(GameTime::GetGameTime().count());
                    std::string const wait =
                        arrivesAt > nowSec
                            ? Acore::StringFormat("its letter from the guild cook arrives in {} s", arrivesAt - nowSec)
                            : std::string("collects its letter from the guild cook at a mailbox");
                    if (st.supplyShort != "mail: " + wait)
                        LOG_INFO("playerbots", "[supply113] five {} ({}): waits for {}: {} (food {} + {} mailed, "
                                 "drink {} + {} mailed)",
                                 id, five.contentKey, Who(low), wait, food, mailFood, drink, mailDrink);
                    st.supplyShort = "mail: " + wait;
                    supplied = false;
                    continue;
                }
                if (drink < line)
                    shortOf = "drink " + std::to_string(drink) + " of " + std::to_string(line);
                if (food < RESTOCK_FOOD_MIN)
                    shortOf += (shortOf.empty() ? "" : ", ") + ("food " + std::to_string(food));
                // [supply27] ammo (a hunter) and the vendor-sold reagents of its spells, the same way
                if (std::string const gear = NewRpgBaseAction::FiveGearShort(p); !gear.empty())
                    shortOf += (shortOf.empty() ? "" : ", ") + gear;
                // [supply79] coordinator 2026-11-10: no five goes in short. After its supply run, a member still short
                // with the guild's supply gold planned for it ([gold41] / [supply76]: a member's spare money covers
                // it) waits for that gold; once its money rose since the run (the gold arrived) its restock runs
                // again. b406 rfc-n five 4: Feette went in "short of drink 11" and ran dry at pull 20
                // [supply100] the pool carried the five: nothing of a member's own holds it
                if (!shortOf.empty() && st.supplyDone && !five.poolGoesIn)
                {
                    // [supply79] waited for on a fact: a planned gold transfer of its supplies to it (the transfers
                    // plan, GuildTransfer: a member whose spare money covers it, its channel open) whose sender is
                    // online and still has the money; none, or refused, and it goes in as it is
                    bool donor = false;
                    if (st.supplyGoldShort)
                        for (auto const& [tid, t] : transfers_)
                        {
                            if (!t.gold || t.toLow != low || t.purpose != "supplies" || t.tradeRefused)
                                continue;
                            Player* d = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(t.fromLow));
                            // [supply91] ... and the sender can send it now: not inside a dungeon, not a member of a
                            // five that runs (its own run holds it), alive
                            auto const ds = d ? bots_.find(t.fromLow) : bots_.end();
                            bool const senderHeld =
                                d && (d->GetMap()->Instanceable() || !d->IsAlive() ||
                                      (ds != bots_.end() && ds->second.fiveId &&
                                       fives_.count(ds->second.fiveId) &&
                                       fives_.at(ds->second.fiveId).stage == GuildFive::Stage::Run));
                            if (d && d->GetMoney() >= t.money && !senderHeld)
                            {
                                donor = true;
                                break;
                            }
                        }
                    if (p->GetMoney() > st.supplyMoneyAt)
                    {
                        LOG_INFO("playerbots", "[supply79] five {} ({}): {} got money since its supply run ({} -> {} c): "
                                 "restocks again, short of {}",
                                 id, five.contentKey, Who(low), st.supplyMoneyAt, p->GetMoney(), shortOf);
                        st.supplyAsked = true;
                        st.supplyDone = st.supplyStarted = st.supplyYielded = false;
                        st.supplyShort = shortOf;
                        supplied = false;
                        continue;
                    }
                    // [supply100] the supply gold is no wait of the five's: its gold comes by mail, the member restocks
                    // with it after the run (b415: Ellosera waited ~1290 s for it)
                    donor = false;
                    if (donor)
                    {
                        if (st.supplyShort != "gold: " + shortOf)
                            LOG_INFO("playerbots", "[supply79] five {} ({}): waits for {}: short of {}, the guild's supply "
                                     "gold ({} c) is on its way",
                                     id, five.contentKey, Who(low), shortOf, st.supplyGoldShort);
                        st.supplyShort = "gold: " + shortOf;
                        supplied = false;
                        continue;
                    }
                }
                if (shortOf.empty() || st.supplyDone)
                {
                    if (!shortOf.empty() && st.supplyShort != "in: " + shortOf)
                        LOG_INFO("playerbots", "[supply27] five {} ({}): {} goes in short of {} (its restock was made "
                                 "or could not be made)",
                                 id, five.contentKey, Who(low), shortOf);
                    st.supplyShort = shortOf.empty() ? "" : "in: " + shortOf;
                    continue;
                }
                supplied = false;
                if (st.supplyShort != shortOf)
                    LOG_INFO("playerbots", "[supply27] five {} ({}): waits for {}: short of {} (below {}), restocks "
                             "at a vendor first",
                             id, five.contentKey, Who(low), shortOf, RESTOCK_FOOD_MIN);
                st.supplyShort = shortOf;
                st.supplyAsked = true;
            }
            if (!supplied)
                continue;
            if (five.arrived.size() == five.members.size())
            {
                ready.emplace_back(id, false);
                requests[id] = request(five, false);
                continue;
            }
            // [guild70] the five-leader's way in: the Dungeon Finder when it takes the five (an honest game
            // mechanic: the core ports the group in), else the walk to the entrance goes on
            Leader::Dungeon::FiveRunRequest lfg = request(five, true);
            std::string lfgWhy;
            if (runs.CanQueue(lfg, lfgWhy))
            {
                ready.emplace_back(id, true);
                requests[id] = lfg;
            }
            else if (std::string walkWhy = FiveWalkRefusal(five);
                     !walkWhy.empty() && (lfgWhy.find(" dead") != std::string::npos ||
                                          lfgWhy.find(" offline") != std::string::npos ||
                                          lfgWhy.find("already in the dungeon finder") != std::string::npos))
            {
                // [lfg68] a member the finder cannot take for now (a ghost, offline, still in the finder): the five
                // waits for it (its member rises, logs in, leaves the queue) - no walk, no disband
                if (std::string& logged = fiveLfgWhy[id]; logged != lfgWhy)
                {
                    logged = lfgWhy;
                    LOG_INFO("playerbots", "[lfg68] five {} ({}): waits for the Dungeon Finder ({}); no walk in: {}",
                             id, five.contentKey, lfgWhy, walkWhy);
                }
                continue;
            }
            else if (!walkWhy.empty())
            {
                // [lfg69] the member the finder named (level range, a lock) is left out of this dungeon's fives at its
                // level
                for (uint32 low : five.members)
                    if (Player* m = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                        m && lfgWhy.find(m->GetName()) != std::string::npos)
                        refusedFor_[five.contentKey][low] = m->GetLevel();
                // [next1] taken in by the finder only: a walk the five cannot make ends it
                LOG_INFO("playerbots", "[next1] five {} ({}): no Dungeon Finder ({}) and no walk in: {}", id,
                         five.contentKey, lfgWhy, walkWhy);
                cancel.emplace_back(id, "no dungeon finder (" + lfgWhy + "), no walk in: " + walkWhy);
                continue;
            }
            else if (std::string& logged = fiveLfgWhy[id]; logged != lfgWhy)
            {
                logged = lfgWhy;
                LOG_INFO("playerbots", "[guild70] five {} ({}): no Dungeon Finder ({}), the five walks to the entrance",
                         id, five.contentKey, lfgWhy);
            }
        }
    }
    for (auto const& [id, why] : cancel)
        DisbandFive(id, why);

    // [drain114] draining for a server switch: a formed five does not start (no queue, no walk in) - it waits; the
    // fives already in the finder / the dungeon run on
    if (drain_)
    {
        std::string held;
        for (auto const& [id, viaLfg] : ready)
            held += (held.empty() ? "" : ", ") + std::to_string(id);
        CoordLock guard(lock_, __func__);
        uint32 running = 0;
        for (auto const& [id, five] : fives_)
            running += five.stage == GuildFive::Stage::Run ? 1 : 0;
        std::string const line = Acore::StringFormat("draining: {} five(s) running to their result, start held for "
                                                     "{}",
                                                     running, held.empty() ? "none" : "fives " + held);
        if (drainLogged_ != line)
        {
            drainLogged_ = line;
            LOG_INFO("playerbots", "[drain114] {}", line);
        }
        ready.clear();
    }

    for (auto const& [id, viaLfg] : ready)
    {
        std::vector<ObjectGuid> members;
        {
            CoordLock guard(lock_, __func__);
            auto const itr = fives_.find(id);
            if (itr == fives_.end())
                continue;
            GuildFive& five = itr->second;
            five.stage = GuildFive::Stage::Run;
            for (uint32 low : five.members)
            {
                members.push_back(ObjectGuid::Create<HighGuid::Player>(low));
                BotState& st = bots_[low];
                st.run = false;
                // its walk to the entrance ends: the five-leader takes it in now
                if (Player* p = ObjectAccessor::FindPlayer(members.back()))
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p); ai && ai->rpgInfo.GetStatus() == RPG_GO_CAMP)
                        ai->rpgInfo.ChangeToIdle("its five's leader takes it into the dungeon");
            }
        }
        // a fresh instance as a player gets one: each bot resets its own instances ("Reset all instances", solo
        // and outside); a human resets his own
        for (ObjectGuid const& guid : members)
            if (Player* p = ObjectAccessor::FindConnectedPlayer(guid); p && !IsHuman(p) && !p->GetGroup())
                Player::ResetInstances(guid, INSTANCE_RESET_ALL, false);
        std::string error;
        std::string const runId = runs.Start(requests[id], error);
        if (runId.empty())
        {
            {
                CoordLock guard(lock_, __func__);
                auto const itr = fives_.find(id);
                if (itr != fives_.end())
                    itr->second.stage = GuildFive::Stage::Gather;  // nothing runs: disbanded as a gathering five
            }
            DisbandFive(id, "dungeon cannot start: " + error);
            continue;
        }
        CoordLock guard(lock_, __func__);
        auto const itr = fives_.find(id);
        if (itr == fives_.end())
            continue;
        itr->second.runId = runId;
        Decision("five " + std::to_string(id) + (viaLfg ? " queues for the dungeon finder: " : " at the entrance of ") +
                 itr->second.dungeon + ": group formed, the five leader leads " + Who(itr->second.leaderLow) +
                 " (its commands are final)");
    }

    // the global leader forms new fives every sixth look (30 s)
    if (++autoFiveTimer_ >= 6)
    {
        autoFiveTimer_ = 0;
        AutoFive();
    }
}

bool GuildCoordinator::PreviewFive(std::string const& contentKey, std::vector<std::pair<uint32, std::string>>& members,
                                   std::string& verdict, std::string& error)
{
    members.clear();
    std::vector<FiveCandidate> five;
    uint32 mapId = 0;
    {
        CoordLock guard(lock_, __func__);
        DungeonOption const* d = FindDungeon(contentKey);
        if (!d)
        {
            error = "no dungeon '" + contentKey + "' in the global leader's list";
            return false;
        }
        mapId = d->mapId;
        std::vector<FiveCandidate> const pool = Candidates(*d, true);
        if (!PickFive(pool, five))
        {
            error = Acore::StringFormat("no free tank, healer and 3 dps of level {}-{} for {} ({} candidates)",
                                        d->minLevel, d->maxLevel, d->name, pool.size());
            return false;
        }
    }
    std::vector<Player*> players;
    for (FiveCandidate const& c : five)
    {
        members.emplace_back(c.low, c.role);
        players.push_back(c.player);
    }
    FiveGate::Verdict const v = FiveGate::Judge(mapId, players);
    verdict = std::string(v.clears ? "clears: " : "does not clear: ") + v.why;
    return true;
}

void GuildCoordinator::AutoFive()
{
    if (!autoFives_ || drain_)  // [drain114] draining: no new five
        return;
    // a dungeon counts only when cleared in its own band: a member above the band with it uncleared has missed it
    {
        CoordLock guard(lock_, __func__);
        for (DungeonOption const& d : dungeons_)
            for (PlanRow const& row : plan_)
            {
                if (row.grp < 'A' || row.grp > 'C' || Cleared(d.contentKey, row.low) ||
                    missed_[d.contentKey].count(row.low))
                    continue;
                Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                if (!p || p->GetLevel() <= d.maxLevel)
                    continue;
                missed_[d.contentKey].insert(row.low);
                LOG_INFO("playerbots", "[alert] MISSED_DUNGEON {} L{} left {} ({}-{}) uncleared", Who(row.low),
                         p->GetLevel(), d.name, d.minLevel, d.maxLevel);
            }
        UpdateDungeonOrder();  // [order360]
    }
    // [roles59] the role mix first: each band's fives get their tanks and healers
    {
        CoordLock guard(lock_, __func__);
        PlanRoles();
    }
    // [need106] user rule 2026-11-10: every guild character clears each dungeon of its band once, and goes again only
    // while the dungeon's loot holds an upgrade it needs. need = not cleared, or a needed upgrade (NeededUpgrade). A
    // member below the band needs it while uncleared (it comes into the band); one above it has no need (out-levelled).
    // [done106] a dungeon no member needs is done for the guild: no five forms for it
    std::unordered_map<std::string, std::map<uint32, std::string>> needs;  // content -> member -> its need
    {
        struct Probe
        {
            DungeonOption const* d;
            uint32 low;
            Player* p;    // [order369] nullptr: the member is logged out - it still counts, its clear is on record
            bool cleared;
            bool inBand;
            bool healer;  // [roles108] (a tank too: [lag324])
            bool over;    // [order362] above the band: its clear is the walk in
        };
        std::vector<Probe> probes;
        {
            CoordLock guard(lock_, __func__);
            for (DungeonOption const& d : dungeons_)
                for (PlanRow const& row : plan_)
                {
                    if (row.grp < 'A' || row.grp > 'C')
                        continue;
                    Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                    if (p && IsHuman(p))
                        continue;
                    bool const cleared = Cleared(d.contentKey, row.low);
                    // [order362] guild-wide an outlevelled member keeps its open clear (all 75 members clear it)
                    // [order369] and it stays in the count with it: the guild's test is over every member, never over
                    // the shrinking set still inside the band
                    bool const over = p && p->GetLevel() > d.maxLevel;
                    if (over && sPlayerbotAIConfig.GuildDungeonOrderPerMember)
                        continue;
                    probes.push_back({&d, row.low, p, cleared, p && !over && p->GetLevel() >= d.walkMinLevel,
                                      p && PlanRole(row.low, p) != "dps", over});
                }
        }
        // [need115] per dungeon option (a content key may have several, one per level band: b421 'sfk-n: band 78-82:
        // 126 in band' summed the options of one key): its band's members, cleared, their needs, the uncleared below
        auto const optionKey = [](DungeonOption const& d)
        { return d.contentKey + " " + std::to_string(d.minLevel) + "-" + std::to_string(d.maxLevel); };
        // [order369] every member of the guild stands in one of the option's four places - in its band, above it
        // (its clear is the walk in), below it (it levels into the band) or logged out - and the line counts them all
        struct Bucket
        {
            uint32 total = 0;
            uint32 cleared = 0;
            uint32 Uncleared() const { return total - cleared; }
        };
        struct OptionCount
        {
            Bucket inBand;
            Bucket above;
            Bucket below;
            Bucket out;
            uint32 Total() const { return inBand.total + above.total + below.total + out.total; }
            uint32 Cleared() const { return inBand.cleared + above.cleared + below.cleared + out.cleared; }
        };
        std::unordered_map<std::string, OptionCount> counts;  // option -> its members by where they stand
        std::unordered_map<std::string, std::map<uint32, std::string>> namedNeeds;  // option -> needs named
        for (Probe const& probe : probes)
        {
            std::string need;
            if (!probe.cleared)
                need = "clear";
            else if (probe.inBand)
                if (std::string const item = NeededUpgrade(*probe.d, probe.p); !item.empty())
                    need = "upgrade " + item;
            // [roles108] user design 2026-11-10: a healer levels in dungeons (a heal spec kills slowly outside) - an
            // in-band dungeon whose mobs still give it experience is a need of its own
            // [lag324] ... a tank's too: with no carrier left (below) a tank or healer goes for a clear, an upgrade or
            // the dungeon's experience of its own
            if (need.empty() && probe.healer && probe.inBand && GivesXp(*probe.d, probe.p))
                need = "xp";
            std::string const option = optionKey(*probe.d);
            OptionCount& count = counts[option];
            Bucket* bucket = &count.below;
            if (!probe.p)
                bucket = &count.out;
            else if (probe.inBand)
                bucket = &count.inBand;
            else if (probe.over)
                bucket = &count.above;
            ++bucket->total;
            bucket->cleared += probe.cleared ? 1 : 0;
            // [need115] the in-band members' needs by name, and [order369] the few above the band with them: those
            // below it are only counted (b421: 1888 lines, every one naming all 75 members for every dungeon above)
            if (!need.empty() && (probe.inBand || probe.over))
                namedNeeds[option][probe.low] = probe.over ? need + " (above the band - it walks in)" : need;
            if (!need.empty())
                needs[probe.d->contentKey][probe.low] = need;
        }
        CoordLock guard(lock_, __func__);
        for (DungeonOption const& d : dungeons_)
        {
            auto const found = needs.find(d.contentKey);
            std::string const option = optionKey(d);
            OptionCount const& count = counts[option];
            // [order369] the counted set is named in full, so that "every one cleared it" is read off the line
            std::string where = Acore::StringFormat("{} in band", count.inBand.total);
            if (count.above.total)
                where += Acore::StringFormat(" + {} above it", count.above.total);
            if (count.below.total)
                where += Acore::StringFormat(" + {} below it", count.below.total);
            if (count.out.total)
                where += Acore::StringFormat(" + {} logged out", count.out.total);
            std::string line = Acore::StringFormat("band {}-{}: {}, {} cleared, {} uncleared", d.minLevel, d.maxLevel,
                                                   where, count.Cleared(), count.Total() - count.Cleared());
            if (auto const mine = namedNeeds.find(option); mine == namedNeeds.end() || mine->second.empty())
                line += "; needs: none";
            else
            {
                line += Acore::StringFormat("; needs ({}):", mine->second.size());
                for (auto const& [low, need] : mine->second)
                    line += " " + Who(low) + " " + need + ",";
                line.pop_back();
            }
            if (uint32 const over = count.above.Uncleared())
                line += Acore::StringFormat("; [order362] above the band uncleared (walk in): {}", over);
            line += Acore::StringFormat("; below the band uncleared: {}", count.below.Uncleared());
            if (std::string& logged = needLogged_[option]; logged != line)
            {
                logged = line;
                LOG_INFO("playerbots", "[need106] {}: {}", d.contentKey, line);
            }
            bool const done = found == needs.end() || found->second.empty();
            if (done && guildDone_.insert(d.contentKey).second)
            {
                uint32 clearedBy = 0;
                if (auto const c = cleared_.find(d.contentKey); c != cleared_.end())
                    for (uint32 low : c->second)
                        if (PlanRow const* row = Row(low); row && row->grp >= 'A' && row->grp <= 'C')
                            ++clearedBy;
                LOG_INFO("playerbots", "[done106] {}: cleared by all {} members of band {}-{} (no member needs it: "
                         "{}, every one cleared with no needed upgrade left)",
                         d.contentKey, clearedBy, d.minLevel, d.maxLevel, where);
                Decision("[done106] " + d.name + " done for the guild: no member needs it");
            }
            else if (!done && guildDone_.erase(d.contentKey))
                LOG_INFO("playerbots", "[done106] {}: needed again ({} members)", d.contentKey,
                         found->second.size());
        }
    }
    // [five58] user direction 2026-11-09: one line per look - each dungeon with members in its band, how many are
    // free, their roles and why no five goes (no tank / no healer / too few dps / nothing to gain / the gate)
    std::map<std::string, std::string> lookWhy;
    std::unordered_map<uint32, std::string> fiveWanted;  // [assist18] this look's members a five would take
    std::set<std::string> gateNo;                        // [assist18] the dungeons whose five the gate refused
    // [assist22] the open dungeon's uncleared members no five the gate passes carries, each with the best margin of the
    // fives tried with it (the lowest worst-pull load)
    std::unordered_map<uint32, GateHold> gateHeld;
    bool gateJudged = false;
    auto const noteHeld = [&gateHeld](std::string const& key, FiveCandidate const& c, FiveGate::Verdict const& v)
    {
        GateHold& h = gateHeld[c.low];
        if (h.judged && h.load <= v.worstLoad)
            return;
        h = {key, c.role, uint8(c.player->GetLevel()), v.worstLoad, v.worstDrop, v.worstMana, v.over, v.judged};
    };
    for (uint32 guardCount = 0; guardCount < 10; ++guardCount)  // one five per dungeon look, at most ten a look
    {
        // the free members of each dungeon's band (lock), their upgrades (no lock: the bots' own values)
        std::vector<std::pair<DungeonOption const*, std::vector<FiveCandidate>>> pools;
        // [order364] the guild's open dungeon: its uncleared members that could enter it now, free for a five or not
        std::map<DungeonOption const*, std::vector<FiveCandidate>> openUncleared;
        {
            CoordLock guard(lock_, __func__);
            for (DungeonOption const& d : dungeons_)
            {
                uint32 heldByOrder = 0;  // [order360]
                std::vector<FiveCandidate> pool = Candidates(d, true, &heldByOrder);
                if (guardCount == 0)
                {
                    uint32 inBand = 0;
                    uint32 overBand = 0;  // [order369] its uncleared members above the band: they walk in
                    std::map<std::string, uint32> busy;
                    for (PlanRow const& row : plan_)
                    {
                        if (row.grp < 'A' || row.grp > 'C')
                            continue;
                        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                        if (!p || IsHuman(p) || p->GetLevel() < d.walkMinLevel)
                            continue;
                        // [order362] above the band uncleared it is still one of the dungeon's own ([order362]
                        // Candidates takes it by the walk in), so the look counts it and says what holds it
                        bool const over = p->GetLevel() > d.maxLevel;
                        if (over && (Cleared(d.contentKey, row.low) ||
                                     sPlayerbotAIConfig.GuildDungeonOrderPerMember))
                            continue;
                        if (over)
                            ++overBand;
                        else
                            ++inBand;
                        std::string why;
                        if (!FreeForFive(row.low, p, &why))
                            ++busy[why];
                        else if (std::string const refusal = DungeonRefusal(d, p, over, over); !refusal.empty())
                            ++busy[refusal];
                        else if (auto const r = refusedFor_.find(d.contentKey);
                                 r != refusedFor_.end() && r->second.count(row.low) &&
                                 r->second.at(row.low) == p->GetLevel())
                            ++busy["[lfg69] refused at its level"];
                    }
                    if (inBand || overBand)
                    {
                        uint32 tanks = 0, healers = 0;
                        for (FiveCandidate const& c : pool)
                            tanks += c.role == "tank" ? 1 : 0, healers += c.role == "healer" ? 1 : 0;
                        std::string const aboveText =
                            overBand ? Acore::StringFormat(" + {} above it", overBand) : std::string();
                        std::string text = Acore::StringFormat("{} in band{}, {} free (t{} h{} d{})", inBand,
                                                               aboveText, pool.size(), tanks, healers,
                                                               pool.size() - tanks - healers);
                        if (heldByOrder)
                            text += Acore::StringFormat(", held by order: {}", heldByOrder);
                        if (!busy.empty())
                        {
                            text += " busy:";
                            for (auto const& [why, n] : busy)
                                text += Acore::StringFormat(" {} {},", why, n);
                            text.pop_back();
                        }
                        if (!sPlayerbotAIConfig.GuildDungeonOrderPerMember && !orderOpenKey_.empty() &&
                            d.contentKey != orderOpenKey_)
                            text += " -> held by order: the guild's dungeon is " + orderOpenKey_;
                        else if (pool.size() < 5)
                            text += " -> fewer than 5 free";
                        else if (!tanks)
                            text += " -> no tank";
                        else if (!healers)
                            text += " -> no healer";
                        else if (pool.size() - tanks - healers < 3)
                            text += " -> fewer than 3 dps";
                        lookWhy[d.contentKey] = text;
                    }
                }
                // [done106] no member needs the dungeon: no five for it
                if (guildDone_.count(d.contentKey))
                {
                    if (guardCount == 0 && lookWhy.count(d.contentKey))
                        lookWhy[d.contentKey] += " -> done for the guild: no member needs it";
                    continue;
                }
                if (guardCount == 0 && lookWhy.count(d.contentKey))
                    if (auto const n = needs.find(d.contentKey); n != needs.end())
                    {
                        // [need106] each member's need, in the look - [need115] the free ones (the five's pool) by
                        // name, the rest counted
                        std::string text = " needs:";
                        for (FiveCandidate const& c : pool)
                            if (auto const m = n->second.find(c.low); m != n->second.end())
                                text += " " + Who(c.low) + " " + m->second + ",";
                        text.pop_back();
                        text += Acore::StringFormat(" ({} members need it in all)", n->second.size());
                        lookWhy[d.contentKey].insert(0, text.substr(1) + "; ");
                    }
                // [assist18] the free members with a need of a dungeon the order lets a five into whose pool holds a
                // five's roles (a tank, a healer, five in all): the next five there would take them - the help
                // pricing leaves them out (b470: 3 of 8 helps ended by a five call)
                if (guardCount == 0)
                    if (auto const n = needs.find(d.contentKey);
                        n != needs.end() && pool.size() >= 5 &&
                        (sPlayerbotAIConfig.GuildDungeonOrderPerMember || orderOpenKey_.empty() ||
                         d.contentKey == orderOpenKey_))
                    {
                        bool const tank = std::any_of(pool.begin(), pool.end(),
                                                      [](FiveCandidate const& c) { return c.role == "tank"; });
                        bool const healer = std::any_of(pool.begin(), pool.end(),
                                                        [](FiveCandidate const& c) { return c.role == "healer"; });
                        if (tank && healer)
                            for (FiveCandidate const& c : pool)
                                if (auto const m = n->second.find(c.low); m != n->second.end())
                                    fiveWanted.emplace(c.low, d.contentKey + " (" + m->second + ")");
                    }
                for (FiveCandidate& c : pool)
                    for (GuildRunRecord const& rec : runs_)
                        if (rec.contentKey == d.contentKey &&
                            std::find(rec.members.begin(), rec.members.end(), c.low) != rec.members.end())
                            ++c.repeats;
                if (pool.size() >= 5 && !sPlayerbotAIConfig.GuildDungeonOrderPerMember &&
                    d.contentKey == orderOpenKey_)
                    for (PlanRow const& row : plan_)
                    {
                        if (row.grp < 'A' || row.grp > 'C' || Cleared(d.contentKey, row.low))
                            continue;
                        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                        if (!p || !p->IsInWorld() || IsHuman(p) || !p->IsAlive() || p->GetMap()->Instanceable())
                            continue;
                        if (auto const st = bots_.find(row.low); st != bots_.end() && st->second.fiveId)
                            continue;
                        if (!DungeonRefusal(d, p).empty() &&
                            (p->GetLevel() <= d.maxLevel || !DungeonRefusal(d, p, true, true).empty()))
                            continue;
                        if (auto const r = refusedFor_.find(d.contentKey); r != refusedFor_.end())
                            if (auto const m = r->second.find(row.low);
                                m != r->second.end() && m->second == p->GetLevel())
                                continue;
                        FiveCandidate u;
                        u.low = row.low;
                        u.player = p;
                        u.role = PlanRole(row.low, p);
                        u.needsClear = true;
                        openUncleared[&d].push_back(u);
                    }
                if (pool.size() >= 5)
                    pools.emplace_back(&d, std::move(pool));
            }
        }
        DungeonOption const* best = nullptr;
        std::vector<FiveCandidate> bestFive;
        uint32 bestScore = 0;
        uint32 bestLeft = UINT32_MAX;  // [five58] the fewest levels a member of the five has left in the band
        bool bestFirstClear = false;   // [night304] the best five holds a member's first clear
        for (auto& [d, pool] : pools)
        {
            // [need106] the need found above: not cleared, or a needed upgrade in the loot
            if (auto const n = needs.find(d->contentKey); n != needs.end())
                for (FiveCandidate& c : pool)
                    if (auto const m = n->second.find(c.low); m != n->second.end())
                    {
                        c.upgrade = !c.needsClear && m->second.rfind("upgrade ", 0) == 0;
                        c.xp = m->second == "xp";  // [roles108]
                    }
            // the dps slots go to members with a need (user rule 2026-11-10: no repeat for nothing to gain).
            // [lag324] coordinator decision 2026-11-11: every member goes for progress of its own at its level - a
            // clear or an upgrade it needs, or (a tank / healer) the dungeon's experience; no carrier with none (a
            // five's time is that member's questing time: the laggards' rule of the global leader)
            // [order360] user rule 2026-11-11: the guild's open dungeon takes helpers for its uncleared members - a
            // lone member's first clear forms with members who cleared it already, an upgrade of their own or not
            bool const helpers =
                !sPlayerbotAIConfig.GuildDungeonOrderPerMember && d->contentKey == orderOpenKey_ &&
                std::any_of(pool.begin(), pool.end(), [](FiveCandidate const& c) { return c.needsClear; });
            if (helpers && guardCount == 0)
                lookWhy[d->contentKey] += " -> [order360] helpers carry its uncleared members";
            pool.erase(std::remove_if(pool.begin(), pool.end(), [helpers](FiveCandidate const& c)
                                      { return !helpers && !c.needsClear && !c.upgrade && !c.xp; }),
                       pool.end());
            // [lag336] coordinator decision 2026-11-11: a crafter called for the members' orders whose town stop is due
            // goes to that town first - its own dungeon need waits until the stop is done; it is taken into a new
            // five only when the five cannot form without it (no other tank / healer free, fewer than 3 other dps).
            // b443 / b444: Dekorhan L22, the scribe holding the guild's 16 glyph orders, in RFC and SFK fives
            // (26 + 6.4 min) while his Light Parchment / Virtuoso Inking Set calls waited
            {
                // [lag338] per member and dungeon: the pools of the look alternate dungeons, so a line kept per
                // member alone changed every pool and logged every look (b445: 203 lines for Dekorhan in 19 min)
                static std::map<std::pair<uint32, std::string>, std::string> lag336Logged;  // world thread: a log
                                                                                             // cadence, no decision
                std::vector<uint32> held;
                for (FiveCandidate const& c : pool)
                {
                    // [order364] an uncleared member of the guild's open dungeon holds every member's next dungeon:
                    // its clear goes before its town stop. b468: Dekorhan L23 held in every look other dps were free
                    if (!c.townStop || (helpers && c.needsClear))
                        continue;
                    uint32 others = 0;
                    for (FiveCandidate const& o : pool)
                        others += !o.townStop && o.role == c.role ? 1 : 0;
                    uint32 const needed = c.role == "dps" ? 3 : 1;
                    std::string const line =
                        others >= needed ? "held for its profession town stop (" + d->contentKey + ")"
                                         : "taken though its town stop is due: no other " + c.role + " free (" +
                                               d->contentKey + ")";
                    if (std::string& logged = lag336Logged[{c.low, d->contentKey}]; logged != line)
                    {
                        logged = line;
                        LOG_INFO("playerbots", "[lag336] {} L{} {}", c.player->GetName(), c.player->GetLevel(), line);
                    }
                    if (others >= needed)
                        held.push_back(c.low);
                }
                if (!held.empty())
                    pool.erase(std::remove_if(pool.begin(), pool.end(),
                                              [&held](FiveCandidate const& c)
                                              { return std::find(held.begin(), held.end(), c.low) != held.end(); }),
                               pool.end());
            }
            // [order364] coordinator decision 2026-11-13: while the open dungeon has uncleared members, a tank / healer
            // with whom the gate passes a first-clear five for them is kept out of an upgrade-only repeat; it goes to
            // the repeats only when no such five passes. b467: the L24-25 tanks went on wc-n repeats while Rollothan
            // L16 (healer) was busy, and when he was free only weaker tanks were
            if (auto const u = openUncleared.find(d); u != openUncleared.end() && !u->second.empty() &&
                std::none_of(pool.begin(), pool.end(), [](FiveCandidate const& c) { return c.needsClear; }))
            {
                constexpr size_t CARRIERS_SIDE = 4;  // the look's cost bound: the 4 highest of each role (8 judges)
                std::vector<FiveCandidate const*> tanks, healers, others;
                for (FiveCandidate const& c : pool)
                    (c.role == "tank" ? tanks : c.role == "healer" ? healers : others).push_back(&c);
                auto const byLevel = [](FiveCandidate const* a, FiveCandidate const* b)
                { return a->player->GetLevel() > b->player->GetLevel(); };
                std::stable_sort(tanks.begin(), tanks.end(), byLevel);
                std::stable_sort(healers.begin(), healers.end(), [](FiveCandidate const* a, FiveCandidate const* b)
                                 { return a->player->GetMaxPower(POWER_MANA) > b->player->GetMaxPower(POWER_MANA); });
                std::stable_sort(others.begin(), others.end(), byLevel);
                std::vector<FiveCandidate const*> carriers(tanks.begin(),
                                                           tanks.begin() + std::min(tanks.size(), CARRIERS_SIDE));
                carriers.insert(carriers.end(), healers.begin(),
                                healers.begin() + std::min(healers.size(), CARRIERS_SIDE));
                std::vector<uint32> kept;
                std::string keptWhy;
                std::vector<std::pair<FiveCandidate const*, FiveGate::Verdict>> refused;  // [assist22]
                for (FiveCandidate const* carrier : carriers)
                {
                    Player* tank = carrier->role == "tank" ? carrier->player : nullptr;
                    Player* healer = carrier->role == "healer" ? carrier->player : nullptr;
                    std::vector<Player*> dpsSlots;
                    std::string clears;
                    std::vector<FiveCandidate const*> carried;  // [assist22]
                    for (FiveCandidate const& c : u->second)
                    {
                        Player** slot = c.role == "tank" ? &tank : c.role == "healer" ? &healer : nullptr;
                        if (slot ? *slot != nullptr : dpsSlots.size() >= 3)
                            continue;
                        if (slot)
                            *slot = c.player;
                        else
                            dpsSlots.push_back(c.player);
                        clears += " " + c.player->GetName() + ",";
                        carried.push_back(&c);
                    }
                    if (clears.empty())
                        continue;  // its slot is the uncleared members' only one: it carries none of them
                    if (!tank && !tanks.empty())
                        tank = tanks.front()->player;
                    if (!healer && !healers.empty())
                        healer = healers.front()->player;
                    for (FiveCandidate const* o : others)
                        if (dpsSlots.size() < 3)
                            dpsSlots.push_back(o->player);
                    if (!tank || !healer || dpsSlots.size() < 3)
                        continue;
                    std::vector<Player*> trial{tank, healer};
                    trial.insert(trial.end(), dpsSlots.begin(), dpsSlots.end());
                    FiveGate::Verdict trialVerdict = FiveGate::Judge(d->mapId, trial);
                    gateJudged |= guardCount == 0;  // [assist22]
                    if (!trialVerdict.clears)
                    {
                        for (FiveCandidate const* c : carried)
                            refused.emplace_back(c, trialVerdict);
                        continue;
                    }
                    kept.push_back(carrier->low);
                    clears.pop_back();
                    keptWhy += Acore::StringFormat(" {} {} (first clear of{} passes the gate),", carrier->role,
                                                   carrier->player->GetName(), clears);
                }
                if (!kept.empty())
                {
                    keptWhy.pop_back();
                    pool.erase(std::remove_if(pool.begin(), pool.end(),
                                              [&kept](FiveCandidate const& c)
                                              { return std::find(kept.begin(), kept.end(), c.low) != kept.end(); }),
                               pool.end());
                    if (guardCount == 0)
                        lookWhy[d->contentKey] += " -> [order364] kept from repeats:" + keptWhy;
                }
                if (guardCount == 0 && kept.empty())  // [assist22]
                    for (auto const& [c, v] : refused)
                        noteHeld(d->contentKey, *c, v);
                static std::map<std::string, std::string> order364Logged;  // world thread: a log cadence
                if (std::string& logged = order364Logged[d->contentKey]; logged != keptWhy)
                {
                    logged = keptWhy;
                    LOG_INFO("playerbots", "[order364] {}: {}", d->contentKey,
                             kept.empty() ? std::string("no free tank / healer carries an uncleared member's first "
                                                        "clear: the repeats go on")
                                          : "kept from upgrade-only repeats for the uncleared members:" + keptWhy);
                }
            }
            std::vector<FiveCandidate> five;
            // [five58] the finder's band first (the five may queue); else a five that walks in: every member of it
            // passes the walk (same continent, no enemy capital), members below the finder's minimum among them
            std::vector<FiveCandidate> finderPool, walkPool;
            for (FiveCandidate const& c : pool)
            {
                if (c.player->GetLevel() >= d->minLevel && !c.overMax)
                    finderPool.push_back(c);
                if (DungeonRefusal(*d, c.player, true, c.overMax).empty())
                    walkPool.push_back(c);
            }
            bool const fromFinder = PickFive(finderPool, five);
            if (!fromFinder && !PickFive(walkPool, five))
            {
                if (guardCount == 0 && lookWhy[d->contentKey].find(" -> ") == std::string::npos)
                    lookWhy[d->contentKey] += Acore::StringFormat(
                        " -> {} with a need (dps) or to carry (tank, healer): no tank, healer and 3 dps among them",
                        pool.size());
                continue;
            }
            uint32 const score = uint32(std::count_if(five.begin(), five.end(),
                                                      [](FiveCandidate const& c) { return c.needsClear; })) +
                                 uint32(std::count_if(five.begin(), five.end(),
                                                      [](FiveCandidate const& c) { return c.upgrade; }));
            // [five58] user rule: every member clears every dungeon at its own level - a five whose member is about to
            // outlevel the band (a clear open or an upgrade in it) goes first, then the most clears and upgrades
            // [night304] user rule (dungeon once per character, repeats only while a needed upgrade drops there): a
            // five with a member's first clear of the dungeon goes before an upgrade-only repeat of a cleared one, and
            // its urgency is its clearing members' levels left. b432: 23 of 24 fives were RFC upgrade repeats at
            // urgency 0 (L21 of max 21) while 29 members needed their first Deadmines clear
            bool const firstClear =
                std::any_of(five.begin(), five.end(), [](FiveCandidate const& c) { return c.needsClear; });
            uint32 left = UINT32_MAX;
            std::string urgent;
            for (FiveCandidate const& c : five)
            {
                // [order362] a member above the band (its walk-in clear) has no level left
                uint32 const levelsLeft = c.overMax ? 0 : d->maxLevel - c.player->GetLevel();
                if ((firstClear ? c.needsClear : c.upgrade) && levelsLeft < left)
                {
                    left = levelsLeft;
                    urgent = c.player->GetName() + " L" + std::to_string(c.player->GetLevel());
                }
            }
            if (guardCount == 0)
                lookWhy[d->contentKey] += Acore::StringFormat(" -> five of score {}, {}, urgency {} level(s) left ({} "
                                                              "of max {})",
                                                              score, firstClear ? "first clears" : "upgrades only",
                                                              left, urgent, d->maxLevel);
            if (best && ((bestFirstClear && !firstClear) ||
                         (bestFirstClear == firstClear &&
                          (left > bestLeft || (left == bestLeft && score <= bestScore)))))
            {
                if (guardCount == 0)
                    lookWhy[d->contentKey] += " -> another dungeon's five is more urgent";
                continue;
            }
            // [next1] the five clears the dungeon by the game's numbers (FiveGate::Judge), else it waits - no level
            // floor
            std::vector<Player*> players;
            for (FiveCandidate const& c : five)
                players.push_back(c.player);
            FiveGate::Verdict verdict = FiveGate::Judge(d->mapId, players);
            // [assist22] the open dungeon's fives refused here: each uncleared member's best margin among them
            bool const openFive = guardCount == 0 && d->contentKey == orderOpenKey_ &&
                                  !sPlayerbotAIConfig.GuildDungeonOrderPerMember;
            std::vector<std::pair<FiveCandidate const*, FiveGate::Verdict>> refusedFives;
            gateJudged |= openFive;
            if (openFive && !verdict.clears)
                for (FiveCandidate const& c : five)
                    if (c.needsClear)
                        refusedFives.emplace_back(&c, verdict);
            // [gate93] the gate judges the five PickFive took (its tank and healer by level); another tank / healer of
            // the same pool may carry the dungeon (a healer's mana pool, a tank's armor): each pair is judged, the
            // first that clears is the five. b411: dm-n "13 of 167 pulls over capacity; worst 2.35: ... heal mana 993
            // of 422" on Aestymro (paladin, 422 mana) with Ganna, sfk-n "14 of 103 ... heal mana 3824 of 422" -
            // the healer and tank PickFive happened to put first
            if (!verdict.clears)
            {
                std::vector<FiveCandidate> const& usedPool = fromFinder ? finderPool : walkPool;
                std::vector<FiveCandidate const*> tanks, healers;
                for (FiveCandidate const& c : usedPool)
                {
                    if (c.role == "tank")
                        tanks.push_back(&c);
                    else if (c.role == "healer")
                        healers.push_back(&c);
                }
                auto const byLevel = [](FiveCandidate const* a, FiveCandidate const* b)
                { return a->player->GetLevel() > b->player->GetLevel(); };
                std::stable_sort(tanks.begin(), tanks.end(), byLevel);
                // [gate94] healers by their mana pool (the gate's binding term), not by level
                std::stable_sort(healers.begin(), healers.end(), [](FiveCandidate const* a, FiveCandidate const* b)
                                 { return a->player->GetMaxPower(POWER_MANA) > b->player->GetMaxPower(POWER_MANA); });
                // [order364] a tank / healer with its first clear here is judged with every partner before its swap
                // out: b468 wc-n, Rollothan L16 (healer, the guild's last uncleared) was judged with one tank only, the
                // lowest mana of 8 healers, never in the top 4
                auto const clearFirst = [](FiveCandidate const* c) { return c->needsClear; };
                std::stable_partition(tanks.begin(), tanks.end(), clearFirst);
                std::stable_partition(healers.begin(), healers.end(), clearFirst);
                constexpr size_t PAIRS_SIDE = 4;  // the look's cost bound: the 4 highest of each role (16 judges)
                if (tanks.size() > PAIRS_SIDE)
                    tanks.resize(PAIRS_SIDE);
                if (healers.size() > PAIRS_SIDE)
                    healers.resize(PAIRS_SIDE);
                bool found = false;
                for (FiveCandidate const* t : tanks)
                {
                    for (FiveCandidate const* h : healers)
                    {
                        if (t->low == five[0].low && h->low == five[1].low)
                            continue;  // the pair judged above
                        std::vector<Player*> alt = players;
                        alt[0] = t->player;
                        alt[1] = h->player;
                        FiveGate::Verdict const altVerdict = FiveGate::Judge(d->mapId, alt);
                        if (!altVerdict.clears)
                        {
                            if (openFive)  // [assist22] its uncleared members: the pair's and the dps slots'
                            {
                                for (FiveCandidate const* c : {t, h})
                                    if (c->needsClear)
                                        refusedFives.emplace_back(c, altVerdict);
                                for (size_t i = 2; i < five.size(); ++i)
                                    if (five[i].needsClear)
                                        refusedFives.emplace_back(&five[i], altVerdict);
                            }
                            continue;
                        }
                        LOG_INFO("playerbots", "[gate93] five for {}: {} / {} did not clear ({}); {} tank, {} healer "
                                 "clear: {}",
                                 d->contentKey, five[0].player->GetName(), five[1].player->GetName(), verdict.why,
                                 t->player->GetName(), h->player->GetName(), altVerdict.why);
                        five[0] = *t;
                        five[1] = *h;
                        players = alt;
                        verdict = altVerdict;
                        found = true;
                        break;
                    }
                    if (found)
                        break;
                }
            }
            // [order368] user point 2026-11-13: a first-clear five takes its carriers by the terms the gate prices,
            // not by level - the tank by the gate's own drop term ([gate20] TankDropIndex: its armor and the whole
            // attack table of a mob of the map's top level), the dps by their formula dps (the fight length, and
            // a shorter fight is less damage taken and less healing). b472 wc-n, the same Deviate Guardian x6 pull:
            // Thelengyl tank 1.09, Irorm 1.23 - the tank alone moved the load 14 %
            if (!verdict.clears && helpers)
            {
                std::vector<FiveCandidate const*> wTanks, wHealers, wDps;
                for (FiveCandidate const& c : walkPool)
                    (c.role == "tank" ? wTanks : c.role == "healer" ? wHealers : wDps).push_back(&c);
                std::map<uint32, float> ehp, formulaDps;
                std::map<uint32, std::string> dropParts;
                for (FiveCandidate const* c : wTanks)
                    ehp[c->low] = FiveGate::TankDropIndex(d->mapId, c->player, &dropParts[c->low]);
                for (FiveCandidate const* c : wDps)
                    formulaDps[c->low] =
                        DpsEstimate::Estimate(c->player, nullptr, DpsEstimate::BuffContext::Group).burstDps;
                std::stable_sort(wTanks.begin(), wTanks.end(), [&ehp](FiveCandidate const* a, FiveCandidate const* b)
                                 { return ehp[a->low] > ehp[b->low]; });
                std::stable_sort(wHealers.begin(), wHealers.end(), [](FiveCandidate const* a, FiveCandidate const* b)
                                 { return a->player->GetMaxPower(POWER_MANA) > b->player->GetMaxPower(POWER_MANA); });
                // [order368] an uncleared member keeps its slot; among equals the highest formula dps
                std::stable_sort(wDps.begin(), wDps.end(),
                                 [&formulaDps](FiveCandidate const* a, FiveCandidate const* b)
                                 {
                                     if (a->needsClear != b->needsClear)
                                         return a->needsClear;
                                     return formulaDps[a->low] > formulaDps[b->low];
                                 });
                // the uncleared members of each role take their slots: the carriers fill the rest
                std::vector<FiveCandidate const*> tankTries, healerTries;
                for (FiveCandidate const* c : wTanks)
                    if (c->needsClear)
                        tankTries.assign(1, c);
                for (FiveCandidate const* c : wHealers)
                    if (c->needsClear)
                        healerTries.assign(1, c);
                constexpr size_t CARRIER_SIDE = 4;  // the look's cost bound, as [gate93]
                if (tankTries.empty())
                    tankTries.assign(wTanks.begin(), wTanks.begin() + std::min(wTanks.size(), CARRIER_SIDE));
                if (healerTries.empty())
                    healerTries.assign(wHealers.begin(), wHealers.begin() + std::min(wHealers.size(), CARRIER_SIDE));
                std::vector<FiveCandidate const*> dpsSlots(wDps.begin(),
                                                           wDps.begin() + std::min(wDps.size(), size_t(3)));
                std::string best368;
                float bestLoad368 = 99.0f;
                for (FiveCandidate const* t : tankTries)
                {
                    if (verdict.clears || dpsSlots.size() < 3)
                        break;
                    for (FiveCandidate const* h : healerTries)
                    {
                        std::vector<Player*> trial{t->player, h->player, dpsSlots[0]->player, dpsSlots[1]->player,
                                                   dpsSlots[2]->player};
                        FiveGate::Verdict const v = FiveGate::Judge(d->mapId, trial);
                        float groupDps = 0.0f;
                        std::string names;
                        for (FiveCandidate const* c : dpsSlots)
                        {
                            groupDps += formulaDps[c->low];
                            names += (names.empty() ? "" : "/") + c->player->GetName();
                        }
                        std::string const line = Acore::StringFormat(
                            "tank {} (drop {:.2f}, takes {:.0f} hp of a mob's hits before it falls [{}], the highest "
                            "of {}), healer {}, dps {} (group dps {:.0f}) -> load {:.2f}",
                            t->player->GetName(), v.worstDrop, ehp[t->low], dropParts[t->low], wTanks.size(),
                            h->player->GetName(), names, groupDps, v.worstLoad);
                        if (v.clears)
                        {
                            best368 = line + ": passes the gate";
                            five = {*t, *h, *dpsSlots[0], *dpsSlots[1], *dpsSlots[2]};
                            players = trial;
                            verdict = v;
                            break;
                        }
                        if (v.worstLoad < bestLoad368)
                        {
                            bestLoad368 = v.worstLoad;
                            best368 = line;
                        }
                    }
                }
                if (!best368.empty())
                {
                    std::string clears;
                    for (FiveCandidate const& c : five)
                        if (c.needsClear)
                            clears += (clears.empty() ? "" : ", ") + c.player->GetName();
                    std::string const line = Acore::StringFormat("first clear for {}: {}",
                                                                 clears.empty() ? "its uncleared members" : clears,
                                                                 best368);
                    static std::map<std::string, std::string> order368Logged;  // world thread: a log cadence
                    if (std::string& logged = order368Logged[d->contentKey]; logged != line)
                    {
                        logged = line;
                        LOG_INFO("playerbots", "[order368] {} {}", d->contentKey, line);
                    }
                    if (guardCount == 0)
                        lookWhy[d->contentKey] +=
                            verdict.clears ? " -> [order368] the carriers the gate prices pass it"
                                           : Acore::StringFormat(" -> [order368] the best carriers still load {:.2f}",
                                                                 bestLoad368);
                }
            }
            // [order367] coordinator decision 2026-11-13: an uncleared healer of the guild's open dungeon whose heal
            // carries no five alone goes with a second, stronger free healer (a tank, two healers, two dps) when the
            // gate passes that five by both healers' rates. b470 wc-n: Rollothan L17 at 103 hps failed the Deviate
            // Guardian x6 boss pull (worst 1.09-1.34) with every tank. A walk-in five: the finder takes one healer
            Player* secondHealer = nullptr;
            if (!verdict.clears && helpers)
            {
                std::vector<FiveCandidate const*> wTanks, wHealers, wDps;
                for (FiveCandidate const& c : walkPool)
                    (c.role == "tank" ? wTanks : c.role == "healer" ? wHealers : wDps).push_back(&c);
                std::stable_sort(wTanks.begin(), wTanks.end(), [](FiveCandidate const* a, FiveCandidate const* b)
                                 { return a->player->GetLevel() > b->player->GetLevel(); });
                std::stable_sort(wHealers.begin(), wHealers.end(), [](FiveCandidate const* a, FiveCandidate const* b)
                                 { return a->player->GetMaxPower(POWER_MANA) > b->player->GetMaxPower(POWER_MANA); });
                std::stable_sort(wDps.begin(), wDps.end(),
                                 [](FiveCandidate const* a, FiveCandidate const* b)
                                 {
                                     if (a->needsClear != b->needsClear)
                                         return a->needsClear;
                                     return a->player->GetLevel() > b->player->GetLevel();
                                 });
                FiveCandidate const* weak = nullptr;
                for (FiveCandidate const* h : wHealers)
                    if (h->needsClear)
                    {
                        weak = h;
                        break;
                    }
                if (weak)
                {
                    constexpr size_t TWO_HEALER_SIDE = 4;  // the look's cost bound, as [gate93]: 4 tanks x 4 healers
                    std::string twoWhy;
                    float bestLoad = 99.0f;
                    bool passed = false;
                    if (wDps.size() < 2)
                        twoWhy = "fewer than 2 dps who can walk in";
                    for (size_t ti = 0; ti < wTanks.size() && ti < TWO_HEALER_SIDE && !passed && wDps.size() >= 2;
                         ++ti)
                    {
                        size_t tried = 0;
                        for (FiveCandidate const* h : wHealers)
                        {
                            if (h == weak)
                                continue;
                            if (++tried > TWO_HEALER_SIDE)
                                break;
                            std::vector<Player*> trial{wTanks[ti]->player, weak->player, h->player, wDps[0]->player,
                                                       wDps[1]->player};
                            FiveGate::Verdict const v = FiveGate::Judge(d->mapId, trial, h->player);
                            if (!v.clears)
                            {
                                if (v.worstLoad < bestLoad || twoWhy.empty())
                                {
                                    bestLoad = v.worstLoad;
                                    twoWhy = Acore::StringFormat("tank {}, healers {} + {}: {}",
                                                                 wTanks[ti]->player->GetName(),
                                                                 weak->player->GetName(), h->player->GetName(), v.why);
                                }
                                continue;
                            }
                            twoWhy = Acore::StringFormat("tank {}, healers {} + {}, dps {}, {} -> passes the gate: {}",
                                                         wTanks[ti]->player->GetName(), weak->player->GetName(),
                                                         h->player->GetName(), wDps[0]->player->GetName(),
                                                         wDps[1]->player->GetName(), v.why);
                            five = {*wTanks[ti], *weak, *h, *wDps[0], *wDps[1]};
                            players = trial;
                            verdict = v;
                            secondHealer = h->player;
                            passed = true;
                            break;
                        }
                    }
                    if (wTanks.empty())
                        twoWhy = "no tank who can walk in";
                    else if (twoWhy.empty())
                        twoWhy = "no second healer who can walk in";
                    static std::map<std::string, std::string> order367Logged;  // world thread: a log cadence
                    std::string const line =
                        Acore::StringFormat("two-healer first-clear five for {}: {}", weak->player->GetName(),
                                            passed ? twoWhy : "no pair passes, the closest " + twoWhy);
                    if (std::string& logged = order367Logged[d->contentKey]; logged != line)
                    {
                        logged = line;
                        LOG_INFO("playerbots", "[order367] {}: {}", d->contentKey, line);
                    }
                    if (guardCount == 0)
                        lookWhy[d->contentKey] += passed ? " -> [order367] two healers (" + weak->player->GetName() +
                                                               " + " + secondHealer->GetName() + ") pass the gate"
                                                         : " -> [order367] two healers: no pair passes";
                }
            }
            if (!verdict.clears)
            {
                if (guardCount == 0)
                {
                    lookWhy[d->contentKey] += " -> the gate says no: " + verdict.why;
                    gateNo.insert(d->contentKey);  // [assist18]
                }
                if (openFive)  // [assist22] no pair of the pool carries its uncleared members
                {
                    gateJudged = true;
                    for (auto const& [c, v] : refusedFives)
                        noteHeld(d->contentKey, *c, v);
                }
                if (std::string& logged = gateWhy[d->contentKey]; logged != verdict.why)
                {
                    logged = verdict.why;
                    LOG_INFO("playerbots", "[next1] five for {} waits (leader {}): {}", d->contentKey,
                             five.front().player->GetName(), verdict.why);
                }
                continue;
            }
            LOG_INFO("playerbots", "[next1] five for {} passes the gate (leader {}): {}", d->contentKey,
                     five.front().player->GetName(), verdict.why);
            if (guardCount == 0)
                lookWhy[d->contentKey] += " -> passes the gate";
            // [need106] the tank and healer: each one's own need, or why a carrier with none goes
            {
                std::vector<FiveCandidate> const& usedPool = fromFinder ? finderPool : walkPool;
                std::string carry;
                for (FiveCandidate const& c : five)
                {
                    if (c.role != "tank" && c.role != "healer")
                        continue;
                    if (c.needsClear || c.upgrade || c.xp)
                    {
                        carry += Acore::StringFormat("; {} {}: needs it ({})", c.role, c.player->GetName(),
                                                     c.needsClear ? "clear" : c.upgrade ? "upgrade" : "xp");
                        continue;
                    }
                    bool const needyFree =
                        std::any_of(usedPool.begin(), usedPool.end(), [&c](FiveCandidate const& o)
                                    { return o.role == c.role && (o.needsClear || o.upgrade || o.xp); });
                    carry += Acore::StringFormat("; {} {}: no need of its own, carries ({} run(s) of it) - {}", c.role,
                                                 c.player->GetName(), c.repeats,
                                                 needyFree ? "the needy " + c.role + "'s pair did not pass the gate"
                                                           : "no needy " + c.role + " free");
                }
                if (!carry.empty())
                {
                    if (guardCount == 0)
                        lookWhy[d->contentKey] += " (" + carry.substr(2) + ")";
                    LOG_INFO("playerbots", "[need106] five for {}: {}", d->contentKey, carry.substr(2));
                }
            }
            bestScore = score;
            bestLeft = left;
            bestFirstClear = firstClear;
            best = d;
            bestFive = five;
        }
        if (guardCount == 0)
        {
            std::string line;
            for (auto const& [key, text] : lookWhy)
                line += (line.empty() ? "" : "; ") + key + ": " + text;
            LOG_INFO("playerbots", "[five58] look: {}{}", line.empty() ? "no planned member in any dungeon band" : line,
                     best ? " => forms " + best->contentKey : std::string(" => no five"));
            for (auto itr = fiveWanted.begin(); itr != fiveWanted.end();)  // [assist18] a five the gate passes
                itr = gateNo.count(itr->second.substr(0, itr->second.find(' '))) ? fiveWanted.erase(itr) : ++itr;
            SetFiveWanted(std::move(fiveWanted));
            SetGateHeld(std::move(gateHeld), gateJudged);  // [assist22]
        }
        if (!best)
            return;

        std::vector<uint32> members;
        std::map<uint32, std::string> roles;
        uint32 clears = 0, upgrades = 0;
        for (FiveCandidate const& c : bestFive)
        {
            members.push_back(c.low);
            roles[c.low] = c.role;
            clears += c.needsClear ? 1 : 0;
            upgrades += c.upgrade ? 1 : 0;
        }
        std::string const why = "free: a tank, a healer and 3 dps of level " + std::to_string(best->minLevel) + "-" +
                                std::to_string(best->maxLevel) + "; not cleared by " + std::to_string(clears) +
                                ", upgrades for " + std::to_string(upgrades);
        CoordLock guard(lock_, __func__);
        // still free (nothing changed since the look: world thread)
        for (uint32 low : members)
            if (!FreeForFive(low, ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low))))
                return;
        std::string error;
        if (!FormFiveLocked(bestFive.front().low, members, *best, roles, why, false, &error))
        {
            LOG_INFO("playerbots", "[guild60] five for {} not formed: {}", best->contentKey, error);
            return;
        }
    }
}

std::string GuildCoordinator::FormFiveCommand(Player* issuer, std::string const& contentKey,
                                              std::vector<std::string> const& names, bool withIssuer)
{
    if (drain_)  // [drain114]
        return "the fives drain (playerbots guild fives drain off first): no new five";
    DungeonOption const* d = FindDungeon(contentKey);
    if (!d)
        return "no dungeon '" + contentKey + "' (playerbots guild dungeons)";

    std::vector<uint32> chosen;
    std::map<uint32, std::string> roles;
    auto add = [&](Player* p) -> std::string
    {
        if (!p)
            return "not found";
        uint32 const low = p->GetGUID().GetCounter();
        if (std::find(chosen.begin(), chosen.end(), low) != chosen.end())
            return "";
        std::string why;
        {
            CoordLock guard(lock_, __func__);
            if (!FreeForFive(low, p, &why) && !(IsHuman(p) && why == "in a group"))
                return p->GetName() + ": " + why;
        }
        std::string const refusal = DungeonRefusal(*d, p);
        if (!refusal.empty() && refusal.rfind("L", 0) != 0)
            return p->GetName() + ": " + refusal;
        chosen.push_back(low);
        roles[low] = PlanRole(low, p);
        return "";
    };
    if (withIssuer)
        if (std::string const err = add(issuer); !err.empty())
            return err;
    for (std::string const& name : names)
        if (std::string const err = add(ObjectAccessor::FindPlayerByName(name)); !err.empty())
            return name + ": " + err;

    // the rest from the free members (the band and the need are not checked: a forced test)
    std::vector<FiveCandidate> pool;
    {
        CoordLock guard(lock_, __func__);
        pool = Candidates(*d, false);
    }
    uint32 tanks = 0, healers = 0, dps = 0;
    for (uint32 low : chosen)
        (roles[low] == "tank" ? tanks : roles[low] == "healer" ? healers : dps)++;
    std::stable_sort(pool.begin(), pool.end(), [](FiveCandidate const& a, FiveCandidate const& b)
                     { return a.player->GetLevel() > b.player->GetLevel(); });
    for (FiveCandidate const& c : pool)
    {
        if (chosen.size() >= 5)
            break;
        if (std::find(chosen.begin(), chosen.end(), c.low) != chosen.end())
            continue;
        if ((c.role == "tank" && !tanks) || (c.role == "healer" && !healers) || (c.role == "dps" && dps < 3))
        {
            chosen.push_back(c.low);
            roles[c.low] = c.role;
            (c.role == "tank" ? tanks : c.role == "healer" ? healers : dps)++;
        }
    }
    if (chosen.size() != 5 || !tanks || !healers)
        return "cannot form a five for " + d->name + ": tanks " + std::to_string(tanks) + ", healers " +
               std::to_string(healers) + ", dps " + std::to_string(dps) + " free on the entrance's continent";

    // the five-leader: its tank (the dungeon leader leads from the tank), never a human
    uint32 leaderLow = 0;
    for (uint32 low : chosen)
        if (roles[low] == "tank" &&
            !IsHuman(ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low))))
            leaderLow = low;
    if (!leaderLow)
        return "the five's tank is a human: the five leader is a bot only";

    std::string levels;
    for (uint32 low : chosen)
        if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low)))
            if (p->GetLevel() < d->minLevel || p->GetLevel() > d->maxLevel)
                levels += (levels.empty() ? "" : ", ") + p->GetName() + " L" + std::to_string(p->GetLevel());
    std::string error;
    uint32 id = 0;
    {
        CoordLock guard(lock_, __func__);
        id = FormFiveLocked(leaderLow, chosen, *d, roles,
                            "order by command" + (levels.empty() ? std::string() : " (out of level: " + levels + ")"),
                            true, &error);
    }
    if (!id)
        return "five not formed: " + error;
    return "five " + std::to_string(id) + " → " + d->name + " (" + d->contentKey + "), leader " + Who(leaderLow) +
           (levels.empty() ? "" : "; out of level: " + levels);
}

// ---------------------------------------------------------------------------------------------------------------------
// the run's end: authority back to the global leader with the report (world thread)
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::OnFiveRunEnd(uint32 fiveId, GuildFiveReport const& report)
{
    CoordLock guard(lock_, __func__);
    auto const itr = fives_.find(fiveId);
    if (itr == fives_.end())
        return;
    GuildFive& five = itr->second;
    // [guild70] the Dungeon Finder did not take the five: nothing was run, the five walks to the entrance instead
    if (report.result == "not_started" && report.reason.rfind(FIVE_LFG_REFUSED, 0) == 0)
    {
        five.stage = GuildFive::Stage::Gather;
        five.arrived.clear();
        five.runId.clear();
        Decision("five " + std::to_string(fiveId) + " (" + five.dungeon +
                 "): " + report.reason.substr(std::strlen(FIVE_LFG_REFUSED)) + " — walks to the entrance");
        return;
    }
    for (auto const& [name, count] : report.deaths)
        for (uint32 low : five.members)
            if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                p && p->GetName() == name)
                five.deaths[low] = count;
    RecordRun(five, report);

    std::string deaths;
    for (auto const& [low, count] : five.deaths)
        deaths += (deaths.empty() ? "" : ", ") + Who(low) + " ×" + std::to_string(count);
    std::string drops;
    for (std::string const& drop : five.drops)
        drops += (drops.empty() ? "" : ", ") + drop;
    // what is still needed: the bosses not killed (a clear is required), the members whose clear is still open
    std::string needs;
    // [guild70] the key / event items nobody carried (the five-leader's report) end the reason
    size_t const items = report.reason.find(FIVE_ITEM_NEED);
    std::string const reason = items != std::string::npos ? report.reason.substr(0, items) : report.reason;
    size_t const cut = reason.find("not killed: ");
    if (report.result != "done")
        needs = "repeat " + five.dungeon + (cut != std::string::npos ? " (" + reason.substr(cut) + ")" : "");
    for (size_t at = items; at != std::string::npos; at = report.reason.find(FIVE_ITEM_NEED, at + 1))
    {
        size_t const end = report.reason.find("; " + std::string(FIVE_ITEM_NEED), at + 1);
        needs += (needs.empty() ? "" : "; ") +
                 report.reason.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }
    std::string const lootLine = FiveLootOutcome(five);  // [loot14]
    {
        // [loot13] each goal mob the route had to kill: killed, or why not (its last state the route saw)
        std::lock_guard<std::mutex> goalGuard(goalLock_);
        if (auto const goals = goals_.find(fiveId); goals != goals_.end())
        {
            std::string text;
            uint32 missed = 0;
            for (auto const& [entry, mob] : goals->second.mobs)
            {
                if (mob.boss)
                    continue;
                std::string const state = mob.state.empty() ? "no report from the route (no retarget ran)" : mob.state;
                // [loot16] the runs' own count of its presence (the script's share is the prior)
                if (!mob.state.empty())
                {
                    auto& seen = presenceSeen_[entry];
                    seen.second += 1;
                    seen.first += mob.state.rfind("absent", 0) == 0 ? 0 : 1;
                }
                missed += mob.takenIfThere && state != "killed" && state.rfind("absent", 0) != 0;
                text += Acore::StringFormat("{}{} ({}): {}", text.empty() ? "" : " | ", mob.name,
                                            mob.takenIfThere ? "a goal" : "not a goal", state);
            }
            if (!text.empty())
                LOG_INFO("playerbots", "[loot13] five {} run {} goal mobs: {}; {} goal(s) not killed", fiveId,
                         report.runId, text, missed);
            goals_.erase(goals);
        }
        for (uint32 low : five.members)
            if (auto const of = goalFiveOf_.find(low); of != goalFiveOf_.end() && of->second == fiveId)
                goalFiveOf_.erase(of);
    }
    std::string open;
    for (uint32 low : five.members)
        if (PlanRow const* row = Row(low); row && !Cleared(five.contentKey, low))
            open += (open.empty() ? "" : ", ") + Who(low);
    if (!open.empty())
        needs += (needs.empty() ? "" : "; ") + std::string("not cleared by ") + open;

    std::string const text =
        "five " + std::to_string(fiveId) + " (" + five.dungeon + ") handed authority back to the global " +
        "leader: " + report.result + ", bosses " + std::to_string(report.bossesKilled.size()) + "/" +
        std::to_string(report.bossesTotal) + ", " + std::to_string(report.durationSec / 60) + " min, wipes " +
        std::to_string(report.wipes) + "; deaths: " + (deaths.empty() ? "none" : deaths) +
        "; loot: " + (drops.empty() ? "none" : drops) + " (" + lootLine + "); needed: " +
        (needs.empty() ? "nothing" : needs);
    LOG_INFO("playerbots", "[guild60] report {} run {}: {} ({})", fiveId, report.runId, text, report.reason);
    Decision(text);

    for (uint32 low : five.members)
    {
        BotState& st = bots_[low];
        st.fiveId = 0;
        st.supplyShort.clear();  // [supply27] a new five, or none: its supply check starts again
        SetConjureGoal(ObjectGuid::Create<HighGuid::Player>(low), 0);  // [dun199]
        st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
        st.run = false;
        st.fiveRefused.clear();
    }
    fives_.erase(itr);
}

// [loot16] the share of instances a source is in: its script's AI-start despawn chance as one observation beside the
// runs' own count (seen alive or killed / every run that looked)
float GuildCoordinator::Presence(uint32 entry) const
{
    auto const script = scriptPresence_.find(entry);
    float const prior = script != scriptPresence_.end() ? script->second : 1.0f;
    std::lock_guard<std::mutex> goalGuard(goalLock_);
    auto const seen = presenceSeen_.find(entry);
    if (seen == presenceSeen_.end())
        return prior;
    return (float(seen->second.first) + prior) / (float(seen->second.second) + 1.0f);
}

std::map<uint32, std::string> GuildCoordinator::FiveGoalMobs(uint32 memberLow) const
{
    std::map<uint32, std::string> mobs;
    std::lock_guard<std::mutex> goalGuard(goalLock_);
    auto const of = goalFiveOf_.find(memberLow);
    if (of == goalFiveOf_.end())
        return mobs;
    if (auto const goals = goals_.find(of->second); goals != goals_.end())
        for (auto const& [entry, mob] : goals->second.mobs)
            if (mob.takenIfThere && !mob.boss)  // [loot16]
                mobs[entry] = mob.items;
    return mobs;
}

void GuildCoordinator::NoteGoalState(uint32 memberLow, uint32 entry, std::string const& state)
{
    std::lock_guard<std::mutex> goalGuard(goalLock_);
    auto const of = goalFiveOf_.find(memberLow);
    if (of == goalFiveOf_.end())
        return;
    auto const goals = goals_.find(of->second);
    if (goals == goals_.end())
        return;
    auto const mob = goals->second.mobs.find(entry);
    if (mob == goals->second.mobs.end() || mob->second.state == state)
        return;
    if (mob->second.state != "killed")  // the instance's copy died: later states are its corpse's
        mob->second.state = state;
}

// [loot14] user 2026-11-13: every BoP the five gave, by its end - (a) a need win worn, (b) a need win held for a
// fact (an empty socket waits for its gem; a better piece the member wears now took the slot), (c) a need win neither
// worn nor held: a false need roll (must stay 0); greed when nobody needed it, the enchanter's disenchant. Under lock_
std::string GuildCoordinator::FiveLootOutcome(GuildFive const& five)
{
    uint32 worn = 0, held = 0, falseNeed = 0, greed = 0, disenchant = 0;
    std::string heldText, falseText;
    for (auto const& [key, decided] : five.rolls)
    {
        uint32 const itemId = key.second;
        if (!decided.first)
        {
            ++greed;
            continue;
        }
        if (decided.second)
        {
            ++disenchant;
            continue;
        }
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(decided.first));
        if (!proto || !p)
            continue;  // offline at the end: judged by its next five
        bool wearing = false;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END && !wearing; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                wearing = item->GetEntry() == itemId;
        if (wearing)
        {
            ++worn;
            continue;
        }
        std::string const what = ItemName(itemId) + " (" + p->GetName() + ", " +
                                 GuildGear::GroupName(GuildGear::EquipGroup(proto->InventoryType)) + ")";
        std::string reason;
        if (!p->GetItemCount(itemId, true))
            reason = "";  // sold / destroyed: never worn
        else if (proto->Socket[0].Color)
            reason = "an empty socket waits for its gem";
        else
            for (uint8 slot : GuildGear::SlotsFor(p, proto))
                if (Item* now = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); now && now->GetTemplate()->ItemLevel >
                                                                                    proto->ItemLevel)
                    reason = "a better piece took the slot: " + now->GetTemplate()->Name1;
        if (!reason.empty())
        {
            ++held;
            heldText += (heldText.empty() ? "" : ", ") + what + ": " + reason;
        }
        else
        {
            ++falseNeed;
            falseText += (falseText.empty() ? "" : ", ") + what;
        }
    }
    lootBuild_[0] += worn;
    lootBuild_[1] += held;
    lootBuild_[2] += falseNeed;
    lootBuild_[3] += greed;
    lootBuild_[4] += disenchant;
    std::string const line = std::to_string(five.rolls.size()) + " BoP rolled: " + std::to_string(worn) +
                             " equipped by a need, " + std::to_string(held) + " need held, " +
                             std::to_string(greed) + " greed (nobody needed), " + std::to_string(disenchant) +
                             " disenchant, " + std::to_string(falseNeed) + " need but not equipped [loot14]";
    LOG_INFO("playerbots", "[loot14] five {} run {} loot: {}{}{}; build so far: {} equipped by a need, {} held, {} "
             "need but not equipped, {} greed, {} disenchant",
             five.id, five.runId, line, falseText.empty() ? "" : "; not equipped: " + falseText,
             heldText.empty() ? "" : "; held: " + heldText, lootBuild_[0], lootBuild_[1], lootBuild_[2],
             lootBuild_[3], lootBuild_[4]);
    return line;
}

void GuildCoordinator::RecordRun(GuildFive const& five, GuildFiveReport const& report)
{
    GuildRunRecord rec;
    rec.contentKey = five.contentKey;
    rec.kind = "dungeon";
    rec.at = uint32(time(nullptr));
    rec.leaderLow = five.leaderLow;
    rec.members = five.members;
    rec.result = report.result;
    rec.bossesKilled = uint32(report.bossesKilled.size());
    rec.bossesTotal = report.bossesTotal;
    for (auto const& [name, count] : report.deaths)
        rec.deaths += count;
    rec.id = runs_.empty() ? 1 : runs_.back().id + 1;  // the DB's own id may differ; the tab shows the order
    if (rec.result == "done")
    {
        for (uint32 low : five.members)
            cleared_[rec.contentKey].insert(low);
        ++orderGen_;  // [order360]
    }
    runs_.push_back(rec);

    std::string members;
    for (uint32 low : five.members)
        members += (members.empty() ? "" : ",") + std::to_string(low);
    std::string drops;
    for (std::string const& drop : five.drops)
        drops += (drops.empty() ? "" : ", ") + drop;
    std::string reason = report.reason;
    std::string key = rec.contentKey;
    std::string result = rec.result;
    SqlEscape(drops);
    SqlEscape(reason);
    SqlEscape(key);
    SqlEscape(result);
    PlayerbotsDatabase.Execute(
        "INSERT INTO guild_project_runs (content, kind, at, leader, members, result, bosses_killed, bosses_total, "
        "deaths, wipes, duration, drops, reason) VALUES ('{}', 'dungeon', {}, {}, '{}', '{}', {}, {}, {}, {}, {}, '{}', "
        "'{}')",
        key, rec.at, rec.leaderLow, members, result, std::min<uint32>(rec.bossesKilled, 255),
        std::min<uint32>(rec.bossesTotal, 255), std::min<uint32>(rec.deaths, 65535), std::min<uint32>(report.wipes, 65535),
        report.durationSec, drops, reason);
}

// ---------------------------------------------------------------------------------------------------------------------
// the order flow (lock_ held)
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::FiveOrder(BotState const& st, uint32 low, Player* p, std::vector<GuildOrder>& orders) const
{
    if (!st.fiveId)
        return;
    auto const itr = fives_.find(st.fiveId);
    if (itr == fives_.end())
        return;
    GuildFive const& five = itr->second;
    auto const role = five.roles.find(low);
    GuildOrder o;
    o.type = GuildOrderType::DUNGEON;
    o.itemId = five.id;
    o.details =
        five.dungeon + " — five " + std::to_string(five.id) + ", " +
        RoleRu(role != five.roles.end() ? role->second : "dps") +
        (low == five.leaderLow ? std::string(", five leader, quartermaster") : ", leader " + Who(five.leaderLow));
    o.reason = five.why;
    if (five.stage == GuildFive::Stage::Gather)
    {
        std::string waiting;
        bool human = false;
        for (uint32 m : five.members)
            if (!five.arrived.count(m))
            {
                waiting += (waiting.empty() ? "" : " ") + Who(m);
                human |= five.humans.count(m) != 0;
            }
        if (five.arrived.count(low))
            o.status =
                (human ? "at the entrance, waits for the player (priority): " : "at the entrance, waits: ") + waiting;
        else if (p->GetMapId() == five.entrance.GetMapId())
            o.status = "walks to the entrance " + std::to_string(uint32(p->GetExactDist(five.entrance))) + " yd";
        else
            o.status = "walks to the entrance (map " + std::to_string(five.entrance.GetMapId()) + ")";
        if (!st.fiveRefused.empty())
            o.status = st.fiveRefused;
        o.executable = !IsHuman(p);
    }
    else
    {
        std::string const phase = Leader::Dungeon::DungeonRuns::Instance().Phase(five.id);
        o.status = phase.empty() ? "hands in the report" : "in the dungeon: " + phase;
    }
    orders.push_back(o);
}

bool GuildCoordinator::FiveStep(Player* bot, BotState& st, GuildRunStep& step)
{
    auto const itr = fives_.find(st.fiveId);
    if (itr == fives_.end() || itr->second.stage != GuildFive::Stage::Gather || !st.fiveRefused.empty())
        return false;
    // [supply27] its restock before the five goes in: the five step yields to New RPG's vendor run (it sees
    // SupplyWanted); a run seen started and ended, or none started after a yield, ends the wait
    if (st.supplyAsked && !st.supplyDone)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        bool const running = ai && ai->rpgInfo.vendorRunEntry;
        if (running)
            st.supplyStarted = true;
        else if (st.supplyStarted || st.supplyYielded)
        {
            st.supplyDone = true;
            st.supplyMoneyAt = bot->GetMoney();  // [supply79]
            LOG_INFO("playerbots", "[supply27] {} L{} supply run {}", bot->GetName(), bot->GetLevel(),
                     st.supplyStarted ? "made" : "could not be made (no vendor run started)");
        }
        else
            st.supplyYielded = true;
        if (!st.supplyDone)
            return false;
    }
    GuildFive const& five = itr->second;
    // [eco199] the five's leader with the members' gold its planned need waits for in its mailbox, a mailbox on its
    // way (the town it stands in or walks to): the five step yields to its pickup there (its MAIL_TO, the town task
    // after this one); the five's check waits for it (UpdateFives)
    if (bot->GetGUID().GetCounter() == five.leaderLow && needGoldOnWay_.count(five.leaderLow))
        return false;
    WorldPosition const& pos = five.entrance;
    // [lfg68] a five the walk in refuses (a member on another continent, the entrance in an enemy capital: Ragefire
    // Chasm for the Alliance) goes in by the Dungeon Finder only - no member walks to its entrance; it stands by out
    // of combat where it is until the finder ports it. b402 five 2 (rfc-n, Alliance): "Brelir L16 GUILD reason:
    // DUNGEON five 2 -> Ragefire Chasm entrance, 8416 yd" - its members' gather walk went to Orgrimmar while the five
    // waited for two members' drink
    // [lfg69] ... and every five the finder can take (each member in its level range) waits for it: the finder
    // ports it from where it stands, no member's walk can call it off. b402: RFC fives 11, 17-23 formed and were
    // disbanded in turn, "#10 Thelengyl: no safe way to the entrance of Ragefire Chasm" (his trip to the entrance
    // refused: Craven Drok L35's patrol round), each before it could queue
    bool finderTakes = false;
    if (LFGDungeonEntry const* entry = five.lfgId ? sLFGDungeonStore.LookupEntry(five.lfgId) : nullptr)
    {
        finderTakes = true;
        for (uint32 low : five.members)
        {
            Player* m = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low));
            if (!m || m->GetLevel() < entry->MinLevel || m->GetLevel() > entry->MaxLevel)
                finderTakes = false;
        }
    }
    if (five.lfgId && sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER))
        if (std::string walkWhy = FiveWalkRefusal(five); !walkWhy.empty() || finderTakes)
        {
            // [lfg89] coordinator 2026-11-10: the finder ports the five from anywhere - its members quest (and
            // restock) on until the proposal comes, then accept it (lfg74 holds the answer through a fight). b411: five
            // 1's members (Dandolar, Thelengyl, Kianey ...) stood 98 % of their time idle by lfg68's stand-by; IDLE /
            // WAIT 3.56 h against b406's 1.59 h. No step: New RPG's own plan goes on (never the walk to the entrance)
            (void)walkWhy;
            return false;
        }
    // the solo bot walks to the entrance only on a safe walk ([guild14]); none: the five is called off
    if (bot->GetMapId() == pos.GetMapId() && bot->GetExactDist(pos) > five.entranceReach &&
        NewRpgBaseAction::TripRefused(bot, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                                      "dungeon entrance " + five.contentKey))
    {
        st.fiveRefused = "no safe way to the entrance of " + five.dungeon;
        // [lfg69] a lasting refusal is remembered for this member and dungeon until a fact changes (its level): the
        // global leader forms the next five without it (b402: the same five re-formed right after each disband)
        refusedFor_[five.contentKey][bot->GetGUID().GetCounter()] = bot->GetLevel();
        return false;
    }
    st.run = true;
    st.runType = GuildOrderType::DUNGEON;
    st.runPos = pos;
    st.runApproached = false;
    step.kind = GuildRunStep::WALK;
    step.pos = pos;
    step.reason = "DUNGEON five " + std::to_string(five.id) + " -> " + five.dungeon + " entrance, " +
                  std::to_string(uint32(bot->GetExactDist(pos))) + " yd";
    return true;
}

bool GuildCoordinator::SupplyWanted(Player* bot) const
{
    if (!bot)
        return false;
    CoordLock guard(lock_, __func__);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    return itr != bots_.end() && itr->second.fiveId && itr->second.supplyAsked && !itr->second.supplyDone;
}

bool GuildCoordinator::InSameActiveFive(uint32 a, uint32 b) const
{
    CoordLock guard(lock_, __func__);
    for (auto const& [id, five] : fives_)
    {
        if (five.stage != GuildFive::Stage::Gather && five.stage != GuildFive::Stage::Run)
            continue;
        bool hasA = false, hasB = false;
        for (uint32 m : five.members)
        {
            hasA = hasA || m == a;
            hasB = hasB || m == b;
        }
        if (hasA && hasB)
            return true;
    }
    return false;
}

uint8 GuildCoordinator::FiveCampReached(Player* bot, BotState& st)

{
    auto const itr = fives_.find(st.fiveId);
    if (itr == fives_.end() || itr->second.stage != GuildFive::Stage::Gather)
    {
        st.run = false;
        return 1;
    }
    itr->second.arrived.insert(bot->GetGUID().GetCounter());
    return 2;  // it stays at the entrance until the five is complete
}

std::string GuildCoordinator::FiveWait(BotState const& st) const
{
    if (!st.fiveId)
        return "";
    auto const itr = fives_.find(st.fiveId);
    if (itr == fives_.end())
        return "";
    return itr->second.stage == GuildFive::Stage::Gather
               ? "five " + std::to_string(itr->first) + " gathers at the entrance of " + itr->second.dungeon
               : "five " + std::to_string(itr->first) + " runs " + itr->second.dungeon + " (the dungeon leader leads)";
}

// [stat1] lock_ held. A five member's status label: "DUNGEON <dungeon> five N (<role>): <what the five does>", the
// run's phase from its dungeon leader ("in the dungeon: boss fight Lord Cobrahn (bosses 1/8)"); "" outside a five
std::string GuildCoordinator::FiveLabel(BotState const& st, uint32 low) const
{
    if (!st.fiveId)
        return "";
    auto const itr = fives_.find(st.fiveId);
    if (itr == fives_.end())
        return "";
    GuildFive const& five = itr->second;
    auto const role = five.roles.find(low);
    std::string doing;
    if (five.stage == GuildFive::Stage::Gather)
        doing = five.arrived.count(low) ? "waits at the entrance " + std::to_string(five.arrived.size()) + "/" +
                                              std::to_string(five.members.size())
                                        : "walks to the entrance";
    else
    {
        std::string const phase = Leader::Dungeon::DungeonRuns::Instance().Phase(five.id);
        doing = phase.empty() ? "hands in the report" : phase;
    }
    return "DUNGEON " + five.dungeon + " five " + std::to_string(five.id) + " (" +
           RoleRu(role != five.roles.end() ? role->second : "dps") + (low == five.leaderLow ? " leader" : "") +
           "): " + doing;
}

void GuildCoordinator::SendFives(Player* player) const
{
    // OF;id;dungeon;status;leader;members;why
    for (auto const& [id, five] : fives_)
    {
        std::string members;
        for (uint32 low : five.members)
        {
            auto const role = five.roles.find(low);
            PlanRow const* row = Row(low);
            members += (members.empty() ? "" : " ") + std::string("#") + std::to_string(row ? row->num : 0) + " " +
                       RoleRu(role != five.roles.end() ? role->second : "dps") +
                       (five.arrived.count(low) && five.stage == GuildFive::Stage::Gather ? "+" : "");
        }
        std::string status;
        if (five.stage == GuildFive::Stage::Gather)
            status = "gathering at the entrance " + std::to_string(five.arrived.size()) + "/" +
                     std::to_string(five.members.size());
        else
        {
            std::string const phase = Leader::Dungeon::DungeonRuns::Instance().Phase(id);
            status = phase.empty() ? "report" : phase;
        }
        SendLine(player, "OF;" + std::to_string(id) + ";" + FiveCell(five.dungeon, 30) + ";" + FiveCell(status, 30) + ";" +
                             FiveCell(Who(five.leaderLow), 20) + ";" + FiveCell(members, 60) + ";" + FiveCell(five.why, 80));
    }
    // OC;dungeon;band;cleared/total planned members (A, B, C);runs;last run (result bosses #members)
    uint32 totalA = 0;
    for (PlanRow const& row : plan_)
        totalA += row.grp >= 'A' && row.grp <= 'C' ? 1 : 0;
    for (DungeonOption const& d : dungeons_)
    {
        auto const cleared = cleared_.find(d.contentKey);
        uint32 clearedA = 0;
        if (cleared != cleared_.end())
            for (uint32 low : cleared->second)
                if (PlanRow const* row = Row(low); row && row->grp >= 'A' && row->grp <= 'C')
                    ++clearedA;
        uint32 runs = 0;
        GuildRunRecord const* last = nullptr;
        for (GuildRunRecord const& rec : runs_)
            if (rec.contentKey == d.contentKey)
            {
                ++runs;
                last = &rec;
            }
        std::string lastText = "-";
        if (last)
        {
            lastText = last->result + " " + std::to_string(last->bossesKilled) + "/" + std::to_string(last->bossesTotal);
            for (uint32 low : last->members)
                if (PlanRow const* row = Row(low))
                    lastText += " #" + std::to_string(row->num);
        }
        SendLine(player, "OC;" + FiveCell(d.name, 30) + ";" + std::to_string(d.minLevel) + "-" + std::to_string(d.maxLevel) +
                             ";" + std::to_string(clearedA) + "/" + std::to_string(totalA) + ";" + std::to_string(runs) +
                             ";" + FiveCell(lastText, 60));
    }
}

void GuildCoordinator::SetDrain(bool on)
{
    CoordLock guard(lock_, __func__);
    if (drain_ == on)
        return;
    drain_ = on;
    drainLogged_.clear();
    uint32 running = 0, formed = 0;
    for (auto const& [id, five] : fives_)
        (five.stage == GuildFive::Stage::Run ? running : formed) += 1;
    LOG_INFO("playerbots", "[drain114] drain {}: {} five(s) running, {} formed and not started{}", on ? "on" : "off",
             running, formed, on ? " (held until the drain ends)" : " (they start now)");
}

std::string GuildCoordinator::FivesText() const
{
    CoordLock guard(lock_, __func__);
    std::ostringstream out;
    out << "fives: " << fives_.size() << ", auto " << (autoFives_ ? "on" : "off") << ", drain "
        << (drain_ ? "on" : "off") << ", runs recorded " << runs_.size();
    for (auto const& [id, five] : fives_)
    {
        out << "\n  " << id << " " << five.contentKey << " " << five.dungeon << " stage "
            << (five.stage == GuildFive::Stage::Gather ? "gather" : "run") << " arrived " << five.arrived.size() << "/"
            << five.members.size() << " leader " << Who(five.leaderLow) << " run " << (five.runId.empty() ? "-" : five.runId)
            << ":";
        for (uint32 low : five.members)
        {
            auto const role = five.roles.find(low);
            out << " " << Who(low) << "(" << (role != five.roles.end() ? role->second : "?") << ")";
        }
    }
    return out.str();
}

std::string GuildCoordinator::DungeonsText(uint8 level) const
{
    std::ostringstream out;
    out << "dungeons of the fives: " << dungeons_.size();
    for (DungeonOption const& d : dungeons_)
    {
        if (level && (level < d.minLevel || level > d.maxLevel))
            continue;
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(d.entranceZone);
        out << "\n  " << d.contentKey << " " << d.name << " L" << d.minLevel << "-" << d.maxLevel << " (target "
            << d.targetLevel << ") entrance map " << d.entrance.GetMapId() << " (" << int32(d.entrance.GetPositionX())
            << "," << int32(d.entrance.GetPositionY()) << ") " << (zone ? zone->area_name[0] : "?") << ", "
            << d.bossItems.size() << " boss items";
    }
    return out.str();
}

// ---------------------------------------------------------------------------------------------------------------------
// loot inside a five's run (map thread)
// ---------------------------------------------------------------------------------------------------------------------
bool GuildCoordinator::FiveRollVote(Player* bot, uint64 lootGuid, uint32 itemId, int32 randomProperty, uint8& vote)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return false;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    // BoP is the five-leader's to give; BoE goes on to the global leader (the looter holds it for his decision)
    if (!Equipment(proto) || proto->Bonding != BIND_WHEN_PICKED_UP)
        return false;
    uint32 const low = bot->GetGUID().GetCounter();
    auto const key = std::make_pair(lootGuid, itemId);
    std::vector<uint32> members;
    uint32 fiveId = 0, enchanter = 0;
    std::pair<uint32, bool> decided{0, false};
    bool known = false;
    {
        CoordLock guard(lock_, __func__);
        auto const st = bots_.find(low);
        if (st == bots_.end() || !st->second.fiveId)
            return false;
        auto const five = fives_.find(st->second.fiveId);
        if (five == fives_.end() || five->second.stage != GuildFive::Stage::Run)
            return false;
        fiveId = five->first;
        members = five->second.members;
        enchanter = five->second.enchanterLow;
        auto const roll = five->second.rolls.find(key);
        if (roll != five->second.rolls.end())
        {
            decided = roll->second;
            known = true;
        }
    }
    if (!known)
    {
        // the five-leader gives it by need: the member it upgrades most (members share the map thread)
        float bestGain = 0.0f;
        for (uint32 m : members)
        {
            Player* member = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m));
            if (!member || member->GetMap() != bot->GetMap() || IsHuman(member))
                continue;
            float const gain = UpgradeGain(member, proto, randomProperty);
            if (gain > bestGain)
            {
                bestGain = gain;
                decided = {m, false};
            }
        }
        // nobody's: the five's enchanter takes it to disenchant (else the normal rolls: the winner sells it)
        if (!decided.first && enchanter)
            if (Player* e = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(enchanter));
                e && e->GetMap() == bot->GetMap() && CanDisenchant(e, proto))
                decided = {enchanter, true};
        CoordLock guard(lock_, __func__);
        auto const five = fives_.find(fiveId);
        if (five == fives_.end())
            return false;
        auto const [roll, inserted] = five->second.rolls.emplace(key, decided);
        decided = roll->second;
        if (inserted && decided.first)
            Decision("five " + std::to_string(fiveId) + ": BoP " + ItemName(itemId) + " → " + Who(decided.first) +
                     (decided.second ? " (an upgrade for nobody, disenchants)"
                                     : " (+" + std::to_string(int32(bestGain * 100.0f + 0.5f)) + "% to the slot)"));
        else if (inserted)
            Decision("five " + std::to_string(fiveId) + ": BoP " + ItemName(itemId) +
                     " — an upgrade for nobody: every member greeds [loot12]");
    }
    // [loot12] user 2026-11-13: NEED only for the member the judge picked; nobody's item is a greed for all (the
    // bot's own roll rules asked "item usage" again and could need what the judge refused)
    if (!decided.first)
    {
        vote = uint8(GREED);
        return true;
    }
    vote = decided.first == low ? uint8(decided.second ? GREED : NEED) : uint8(PASS);
    return true;
}

void GuildCoordinator::FiveLoot(Player* player, Item* item, uint32 count)
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!proto || proto->Quality < ITEM_QUALITY_UNCOMMON || proto->Class == ITEM_CLASS_QUEST)
        return;
    uint32 const low = player->GetGUID().GetCounter();
    bool bopSpare = false;
    bool disenchant = false;
    if (Equipment(proto) && proto->Bonding == BIND_WHEN_PICKED_UP && item->IsSoulBound() && GET_PLAYERBOT_AI(player))
    {
        bopSpare = !WouldWear(player, item);  // [loot12] the copy it holds (EvaluateDrop is for an item not owned)
        disenchant = bopSpare && CanDisenchant(player, proto);
    }
    CoordLock guard(lock_, __func__);
    auto const st = bots_.find(low);
    if (st == bots_.end() || !st->second.fiveId)
        return;
    auto const five = fives_.find(st->second.fiveId);
    if (five == fives_.end() || five->second.stage != GuildFive::Stage::Run)
        return;
    if (five->second.drops.size() < FIVE_DROPS_KEPT)
        five->second.drops.push_back(ItemName(proto->ItemId) + (count > 1 ? " ×" + std::to_string(count) : "") + " (" +
                                     player->GetName() + ")");
    if (!bopSpare)
        return;
    // spare BoP: the enchanter disenchants it, anyone else sells it (after the run: New RPG's disenchant / vendor)
    if (disenchant)
        st->second.disenchant.push_back(item->GetGUID().GetCounter());
    else
        st->second.sell.push_back(item->GetGUID().GetCounter());
    Decision("five " + std::to_string(five->first) + ": BoP " + ItemName(proto->ItemId) + " at " + Who(low) +
             " — an upgrade for nobody, " + (disenchant ? "disenchants" : "sells"));
}
