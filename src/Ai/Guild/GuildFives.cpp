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
#include <cctype>
#include <cstring>
#include <sstream>

#include "AiFactory.h"  // [roles59] talent trees
#include "AiObjectContext.h"
#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DpsEstimate.h"  // [next1] the five gate
#include "HealthTriggers.h"  // [next1] RestState::HealerThroughput
#include "GameTime.h"
#include "Group.h"
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
#include "StatsWeightCalculator.h"
#include "TestHarnessData.h"
#include "LeaderDungeonRun.h"  // [guild70] the five-leader runs the dungeon (no test harness)
#include "LeaderOrders.h"  // [run80] Leader::LedBot
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

    // the item's gain for the member (the coordinator's BoE rule): EQUIP / REPLACE of its "item upgrade", the score
    // over its worn piece; < 0: no upgrade
    float UpgradeGain(Player* m, ItemTemplate const* proto, int32 randomProperty)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(m);
        if (!ai || !proto || m->CanUseItem(proto) != EQUIP_ERR_OK)
            return -1.0f;
        std::string const qualifier =
            std::to_string(proto->ItemId) + (randomProperty ? "," + std::to_string(randomProperty) : "");
        ItemUsage const usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", qualifier)->Get();
        if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
            return -1.0f;
        StatsWeightCalculator calc(m);
        float const score = calc.CalculateItem(proto->ItemId, randomProperty);
        float current = 0.0f;
        uint8 const slot = m->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot != NULL_SLOT)
            if (Item* worn = m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                calc.Reset();
                current = calc.CalculateItem(worn->GetEntry(), worn->GetItemRandomPropertyId());
            }
        return current > 0.0f ? (score - current) / current : 1.0f;
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
        float melee = 0.0f;  // damage per second before the tank's armor and avoidance
        bool boss = false;
    };

    // What one pull brings as the core brings it ([kara4] PackJoiners): the mob pulled, its formation
    // (creature_formations) and the mobs of the core's engage call - within CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS of
    // it (Creature::CallAssistance). The chain spread past that is the planner's fight estimate, not the gate's.
    struct Pack
    {
        std::vector<Mob> mobs;
        bool boss = false;
        float health = 0.0f;
    };

    std::map<uint32, std::vector<Pack>> packsByMap;  // world thread (AutoFive, PreviewFive)

    std::vector<Pack> const& Packs(uint32 mapId)
    {
        if (auto const found = packsByMap.find(mapId); found != packsByMap.end())
            return found->second;
        std::vector<Pack>& packs = packsByMap[mapId];

        std::set<uint32> credits;
        if (QueryResult result =
                WorldDatabase.Query("SELECT creditEntry FROM instance_encounters WHERE creditType = 0"))
            do
                credits.insert(result->Fetch()[0].Get<uint32>());
            while (result->NextRow());

        struct Spawn
        {
            uint32 guid = 0;
            uint32 leader = 0;
            G3D::Vector3 pos;
            Mob mob;
        };
        std::vector<Spawn> spawns;
        if (QueryResult result = WorldDatabase.Query(
                "SELECT c.guid, c.id, c.position_x, c.position_y, c.position_z, IFNULL(f.leaderGUID, 0) "
                "FROM creature c "
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
                s.mob.entry = tmpl->Entry;
                s.mob.name = tmpl->Name;
                s.mob.level = tmpl->maxlevel;
                s.mob.health = float(stats->GenerateHealth(tmpl));
                float const swing = std::max(float(tmpl->BaseAttackTime), 1.0f) / float(IN_MILLISECONDS);
                float const base = stats->GenerateBaseDamage(tmpl);
                s.mob.melee = ((base + base * 1.5f) / 2.0f + float(stats->AttackPower) / 14.0f * swing) *
                              tmpl->DamageModifier / swing;
                s.mob.boss = credits.count(tmpl->Entry) || tmpl->rank == CREATURE_ELITE_WORLDBOSS;
                spawns.push_back(s);
            } while (result->NextRow());
        }

        float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        std::set<std::vector<uint32>> seen;
        for (Spawn const& seed : spawns)
        {
            std::vector<size_t> members;
            for (size_t j = 0; j < spawns.size(); ++j)
            {
                Spawn const& other = spawns[j];
                bool const formation = (seed.leader && (other.leader == seed.leader || other.guid == seed.leader)) ||
                                       (other.leader && other.leader == seed.guid);
                if (&other == &seed || formation || (assist > 0.0f && (other.pos - seed.pos).length() <= assist))
                    members.push_back(j);
            }
            std::vector<uint32> key;
            for (size_t j : members)
                key.push_back(spawns[j].guid);
            std::sort(key.begin(), key.end());
            if (!seen.insert(key).second)
                continue;
            Pack pack;
            for (size_t j : members)
            {
                pack.mobs.push_back(spawns[j].mob);
                pack.boss |= spawns[j].mob.boss;
                pack.health += spawns[j].mob.health;
            }
            packs.push_back(std::move(pack));
        }
        LOG_INFO("playerbots", "[next1] five gate: map {} - {} hostile spawns, {} distinct pulls", mapId, spawns.size(),
                 packs.size());
        return packs;
    }

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

    struct Verdict
    {
        bool clears = true;
        std::string why;
    };

    // The five: the tank first, the healer second (PickFive's order). Each pull is judged as the planner judges a pull
    // "as it comes": the group's damage (DpsEstimate, each member against a mob of its level, the spell miss of the
    // mobs' levels) kills the mobs one by one, weakest first; every living one lands its melee on the tank (its armor,
    // dodge and parry); the healer's best rate (RestState::HealerThroughput) heals after its first gap. Clears when, in
    // every pull, the tank's deepest drop leaves it one more heal gap at the pull's full rate (the planner's maxDrop)
    // and the healer's mana covers the healing. Not counted: the mobs' spells, area damage on the group, splitting a
    // pull.
    Verdict Judge(uint32 mapId, std::vector<Player*> const& five)
    {
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
                d.level = p->GetLevel();
                d.burst = r.burstDps;
                d.white = r.whiteDps;
                d.oomSec = r.oomSec;
                dps.push_back(d);
                groupDps += d.burst;
            }
        RestState::HealerFacts const heal = RestState::HealerThroughput(healer, tank);
        float const tankHealth = float(tank->GetMaxHealth());
        float const avoid =
            std::clamp((tank->GetUnitDodgeChance() + tank->GetUnitParryChance()) / 100.0f, 0.0f, 0.75f);
        float const armor = float(tank->GetArmor());
        float const healMana = heal.maxMana;
        float const gap = std::max(heal.gapSec, 1.5f);

        float worst = 0.0f;
        std::string worstWhy;
        uint32 judged = 0, failed = 0;
        for (Pack const& pack : Packs(mapId))
        {
            ++judged;
            std::vector<Mob> mobs = pack.mobs;
            std::sort(mobs.begin(), mobs.end(), [](Mob const& a, Mob const& b) { return a.health < b.health; });
            auto landed = [&](Mob const& m)
            { return m.melee * (1.0f - ArmorReduction(armor, m.level)) * (1.0f - avoid); };
            float rate = 0.0f;
            for (Mob const& m : mobs)
                rate += landed(m);
            float const fightGuess = groupDps > 0.0f ? pack.health / groupDps : 0.0f;
            float const startRate = rate;
            float t = 0.0f, drop = 0.0f, deepest = 0.0f, intake = 0.0f;
            for (Mob const& m : mobs)
            {
                float killDps = 0.0f;
                for (Damage const& d : dps)
                    killDps += d.Over(fightGuess) * HitFactor(int32(m.level) - int32(d.level));
                float const seg = killDps > 0.0f ? m.health / killDps : 3600.0f;
                // before the healer's first heal lands, then after it
                float const before = std::clamp(gap - t, 0.0f, seg);
                drop += rate * before;
                deepest = std::max(deepest, drop);
                drop = std::max(0.0f, drop + (rate - heal.hps) * (seg - before));
                deepest = std::max(deepest, drop);
                intake += rate * seg;
                t += seg;
                rate -= landed(m);
            }
            float const healNeeded = std::max(0.0f, intake - drop);
            float const manaNeeded = heal.healPerMana > 0.0f ? healNeeded / heal.healPerMana : 0.0f;
            float const dropShare = tankHealth > 0.0f ? (deepest + startRate * gap) / tankHealth : 99.0f;
            float const manaShare = healMana > 0.0f ? manaNeeded / healMana : (healNeeded > 0.0f ? 99.0f : 0.0f);
            float const load = std::max(dropShare, manaShare);
            if (load > 1.0f)
                ++failed;
            if (load > worst)
            {
                worst = load;
                uint8 lo = 255, hi = 0;
                for (Mob const& m : mobs)
                {
                    lo = std::min(lo, m.level);
                    hi = std::max(hi, m.level);
                }
                worstWhy = Acore::StringFormat(
                    "{}{} x{} (L{}-{}, {:.0f} hp): fight {:.0f} s, tank drop {:.0f} + one gap {:.0f} of {:.0f} hp "
                    "({:.2f}), heal mana {:.0f} of {:.0f} ({:.2f})",
                    pack.boss ? "boss pull " : "", mobs.front().name, mobs.size(), lo, hi, pack.health, t, deepest,
                    startRate * gap, tankHealth, dropShare, manaNeeded, healMana, manaShare);
            }
        }
        out.clears = failed == 0 && judged > 0;
        out.why = Acore::StringFormat(
            "{} of {} pulls over capacity; worst {:.2f}: {}; group burst dps {:.0f}, healer {} {:.0f} hps "
            "(gap {:.1f} s, {:.1f} hp/mana), tank {} {:.0f} hp, armor {:.0f}, avoid {:.2f} [mob melee from templates; "
            "spells, area damage and splits not counted]",
            failed, judged, worst, worstWhy.empty() ? "-" : worstWhy, groupDps, healer->GetName(), heal.hps, gap,
            heal.healPerMana, tank->GetName(), tankHealth, armor, avoid);
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
        if (QueryResult result = WorldDatabase.Query(
                "SELECT DISTINCT c.map, t.entry, l.Item, l.Reference FROM creature c JOIN creature_template t ON "
                "t.entry = c.id JOIN creature_loot_template l ON l.Entry = t.lootid WHERE c.map IN ({}) AND "
                "(t.`rank` >= 2 OR t.entry IN (SELECT creditEntry FROM instance_encounters WHERE creditType = 0))",
                maps))
        {
            do
            {
                Field* f = result->Fetch();
                rows[f[0].Get<uint32>()].emplace_back(f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>());
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
                    WorldDatabase.Query("SELECT Entry, Item, Reference FROM reference_loot_template WHERE Entry IN ({})", ids))
            {
                do
                {
                    Field* f = result->Fetch();
                    uint32 const reference = f[2].Get<uint32>();
                    refs[f[0].Get<uint32>()].emplace_back(f[1].Get<uint32>(), reference);
                    if (reference && !refs.count(reference))
                        wanted.insert(reference);
                } while (result->NextRow());
            }
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
                for (auto const& [creature, item, reference] : rows[mapId])
                {
                    // a wing: its own bosses only
                    if (content && !content->bossEntries.empty() &&
                        std::find(content->bossEntries.begin(), content->bossEntries.end(), creature) ==
                            content->bossEntries.end())
                        continue;
                    if (!reference)
                    {
                        addItem(items, item);
                        continue;
                    }
                    for (auto const& [refItem, refRef] : refs[reference])
                    {
                        if (!refRef)
                            addItem(items, refItem);
                        else
                            for (auto const& [deepItem, deepRef] : refs[refRef])
                                if (!deepRef)
                                    addItem(items, deepItem);
                    }
                }
                d.bossItems.assign(items.begin(), items.end());
            }
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

std::string GuildCoordinator::DungeonRefusal(DungeonOption const& d, Player* p, bool walk) const
{
    // [five58] user 2026-11-09: the five gate proves a five's capacity, so the Dungeon Finder's minimum is no floor:
    // from the instance's own entry level (RFC 8, WC / DM 10, SFK 14) a five that walks in goes (b400: the healers
    // Guinudrond, Selalleas and Aevmae at L14 were outside every band of 15+)
    if (p->GetLevel() < d.walkMinLevel || p->GetLevel() > d.maxLevel)
        return "L" + std::to_string(p->GetLevel()) + " outside " + std::to_string(d.walkMinLevel) + "-" +
               std::to_string(d.maxLevel);
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
        return "entrance on another continent (map " + std::to_string(d.entrance.GetMapId()) + ")";
    // the entrance in the other faction's capital: its guards
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(d.entranceZone))
    {
        uint32 const own = p->GetTeamId() == TEAM_ALLIANCE ? AREATEAM_ALLY : AREATEAM_HORDE;
        if ((zone->flags & AREA_FLAG_CAPITAL) && zone->team && zone->team != own && zone->team != AREATEAM_ANY)
            return std::string("entrance in an enemy capital ") + zone->area_name[0];
    }
    return "";
}

bool GuildCoordinator::HoldsUpgrade(DungeonOption const& d, Player* p) const
{
    for (uint32 itemId : d.bossItems)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (proto && proto->RequiredLevel <= d.maxLevel && UpgradeGain(p, proto, 0) > 0.0f)
            return true;
    }
    return false;
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
void GuildCoordinator::PlanRoles()
{
    std::set<std::pair<uint32, uint32>> bands;
    for (DungeonOption const& d : dungeons_)
        bands.emplace(d.walkMinLevel, d.maxLevel);
    for (auto const& [lo, hi] : bands)
    {
        std::vector<std::pair<PlanRow const*, Player*>> members;
        for (PlanRow const& row : plan_)
        {
            if (row.grp < 'A' || row.grp > 'C')
                continue;
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
            if (!p || !p->IsInWorld() || IsHuman(p) || p->GetLevel() < lo || p->GetLevel() > hi)
                continue;
            members.emplace_back(&row, p);
        }
        if (members.size() < 5)
            continue;
        uint32 const fives = uint32(members.size() / 5);
        auto roleNow = [this](uint32 low, Player* p)
        {
            std::string const planned = RoleOf(low);
            return planned.empty() ? SpecRoleNow(p) : planned;
        };
        std::string text;
        uint32 dps = 0;
        std::map<std::string, std::vector<std::string>> lists;
        std::map<std::string, uint32> counts;
        for (auto const& [row, p] : members)
            ++counts[roleNow(row->low, p)];
        for (std::string const role : {"healer", "tank"})
        {
            while (counts[role] < fives)
            {
                PlanRow const* best = nullptr;
                Player* bestP = nullptr;
                uint32 bestPoints = 0;
                bool bestPlanned = false;
                for (auto const& [row, p] : members)
                {
                    int8 const spec = RoleSpecIndex(p->getClass(), role);
                    if (spec < 0 || roleNow(row->low, p) != "dps" || !RoleOf(row->low).empty())
                        continue;
                    std::map<uint8, uint32> tabs = AiFactory::GetPlayerSpecTabs(p);
                    uint32 const points = tabs[SpecTree(p->getClass(), uint8(spec))];
                    bool const planned = row->role == role;
                    if (best && std::make_tuple(planned, points, p->GetLevel()) <=
                                    std::make_tuple(bestPlanned, bestPoints, bestP->GetLevel()))
                        continue;
                    best = row;
                    bestP = p;
                    bestPoints = points;
                    bestPlanned = planned;
                }
                if (!best)
                    break;
                RoleAssign assign;
                assign.role = role;
                assign.spec = uint8(RoleSpecIndex(bestP->getClass(), role));
                assign.solo = SoloSpecIndex(bestP);
                {
                    std::lock_guard<std::mutex> guard(roleLock_);
                    roleAssign_[best->low] = assign;
                }
                // [roles62] the role spec outranks questing as a five does (b401: 10 healers assigned, all "respec at
                // its trainer", and each kept its quest first - the fives had no healer meanwhile): the member leaves
                // its quest now, New RPG's IDLE takes the trainer walk first (StartRoleSpec)
                auto const stIt = bots_.find(best->low);
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bestP); ai && !bestP->IsInCombat() &&
                    SpecRoleNow(bestP) != role && ai->rpgInfo.GetStatus() != RPG_IDLE &&
                    (stIt == bots_.end() || !stIt->second.fiveId) && !NewRpgCoop::InParty(best->low))
                    ai->rpgInfo.ChangeToIdle();
                --counts["dps"];
                ++counts[role];
                LOG_INFO("playerbots", "[roles59] {} L{} → {} ({}): band {}-{} has {} fives and {} {}s; plan role {}, "
                         "{} points in its tree, talents now {}",
                         bestP->GetName(), bestP->GetLevel(), role, SpecName(bestP->getClass(), assign.spec), lo, hi,
                         fives, counts[role] - 1, role, best->role, bestPoints, SpecRoleNow(bestP));
            }
        }
        for (auto const& [row, p] : members)
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
        text = Acore::StringFormat("{} members, {} fives; healers {} ({}), tanks {} ({}), dps {}", members.size(), fives,
                                   lists["healer"].size(), join(lists["healer"]), lists["tank"].size(),
                                   join(lists["tank"]), dps);
        if (lists["healer"].size() < fives || lists["tank"].size() < fives)
            text += Acore::StringFormat("; short: {} healers, {} tanks (no damage hybrid of a class that can left)",
                                        lists["healer"].size() < fives ? fives - lists["healer"].size() : 0,
                                        lists["tank"].size() < fives ? fives - lists["tank"].size() : 0);
        std::string const key = std::to_string(lo) + "-" + std::to_string(hi);
        if (rolesWhy_[key] != text)
        {
            rolesWhy_[key] = text;
            LOG_INFO("playerbots", "[roles59] band {}: {}", key, text);
        }
    }
}

bool GuildCoordinator::Cleared(std::string const& contentKey, uint32 low) const
{
    auto const itr = cleared_.find(contentKey);
    return itr != cleared_.end() && itr->second.count(low);
}

std::vector<GuildCoordinator::FiveCandidate> GuildCoordinator::Candidates(DungeonOption const& d, bool band)
{
    std::vector<FiveCandidate> out;
    for (PlanRow const& row : plan_)
    {
        if (row.grp < 'A' || row.grp > 'C')
            continue;
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || IsHuman(p) || !FreeForFive(row.low, p))
            continue;
        std::string const refusal = DungeonRefusal(d, p);
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
        // every planned member (A, B and C) clears every dungeon, in its band (user 2026-11-08)
        c.needsClear = !Cleared(d.contentKey, row.low);
        c.enchanter = p->HasSkill(SKILL_ENCHANTING) && p->HasSpell(SPELL_DISENCHANT);
        out.push_back(c);
    }
    return out;
}

bool GuildCoordinator::PickFive(std::vector<FiveCandidate> const& pool, std::vector<FiveCandidate>& out) const
{
    std::vector<FiveCandidate> sorted = pool;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](FiveCandidate const& a, FiveCandidate const& b)
                     {
                         bool const na = a.needsClear || a.upgrade, nb = b.needsClear || b.upgrade;
                         if (na != nb)
                             return na;
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
        st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
        st.fiveRefused.clear();
        // the five's order comes first: the member leaves its quest now (New RPG takes the order at IDLE)
        if (PlayerbotAI* ai = p ? GET_PLAYERBOT_AI(p) : nullptr)
            if (!IsHuman(p) && !p->IsInCombat())
                ai->rpgInfo.ChangeToIdle();
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
        st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
        st.fiveRefused.clear();
        if (st.run && st.runType == GuildOrderType::DUNGEON)
        {
            st.run = false;
            // its walk to the entrance ends: back to its own choice (quests)
            if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low)))
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                    if (ai->rpgInfo.GetStatus() == RPG_GO_CAMP)
                        ai->rpgInfo.ChangeToIdle();
        }
    }
    Decision("five " + std::to_string(fiveId) + " (" + itr->second.dungeon + ") disbanded" +
             (reason.empty() ? "" : ": " + reason) + " — authority back with the global leader, members quest");
    fives_.erase(itr);
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
                cancel.emplace_back(id, why);
                continue;
            }
            // [supply27] user design: before the five queues or walks in each bot member has its supplies - drink
            // for a mana user, food for all (BuyAction's restock line) - else it restocks at a vendor with its own
            // money first (its New RPG vendor run; the quartermaster's trade tops it up inside, GuildTransfer c.).
            // A member whose run was made or could not be made goes in as it is (logged)
            bool supplied = true;
            for (uint32 low : five.members)
            {
                Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low));
                if (!p || IsHuman(p) || !GET_PLAYERBOT_AI(p))
                    continue;
                BotState& st = bots_[low];
                std::string shortOf;
                uint32 const drink = p->GetMaxPower(POWER_MANA) ? CountBagFood(p, RESTOCK_SPELL_CATEGORY_DRINK)
                                                                : RESTOCK_FOOD_MIN;
                uint32 const food = CountBagFood(p, RESTOCK_SPELL_CATEGORY_FOOD);
                if (drink < RESTOCK_FOOD_MIN)
                    shortOf = "drink " + std::to_string(drink);
                if (food < RESTOCK_FOOD_MIN)
                    shortOf += (shortOf.empty() ? "" : ", ") + ("food " + std::to_string(food));
                // [supply27] ammo (a hunter) and the vendor-sold reagents of its spells, the same way
                if (std::string const gear = NewRpgBaseAction::FiveGearShort(p); !gear.empty())
                    shortOf += (shortOf.empty() ? "" : ", ") + gear;
                // [supply79] coordinator 2026-11-10: no five goes in short. After its supply run, a member still short
                // with the guild's supply gold planned for it ([gold41] / [supply76]: a member's spare money covers
                // it) waits for that gold; once its money rose since the run (the gold arrived) its restock runs
                // again. b406 rfc-n five 4: Feette went in "short of drink 11" and ran dry at pull 20
                if (!shortOf.empty() && st.supplyDone)
                {
                    bool donor = false;
                    if (st.supplyGoldShort)
                        for (PlanRow const& row : plan_)
                            if (Player* d = row.low == low ? nullptr
                                                           : ObjectAccessor::FindConnectedPlayer(
                                                                 ObjectGuid::Create<HighGuid::Player>(row.low));
                                d && GET_PLAYERBOT_AI(d) && !IsHuman(d) &&
                                FoundingFreeMoney(d) >= st.supplyGoldShort)
                            {
                                donor = true;
                                break;
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
                        ai->rpgInfo.ChangeToIdle();
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
    if (!autoFives_)
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
    }
    // [roles59] the role mix first: each band's fives get their tanks and healers
    {
        CoordLock guard(lock_, __func__);
        PlanRoles();
    }
    // [five58] user direction 2026-11-09: one line per look - each dungeon with members in its band, how many are
    // free, their roles and why no five goes (no tank / no healer / too few dps / nothing to gain / the gate)
    std::map<std::string, std::string> lookWhy;
    for (uint32 guardCount = 0; guardCount < 10; ++guardCount)  // one five per dungeon look, at most ten a look
    {
        // the free members of each dungeon's band (lock), their upgrades (no lock: the bots' own values)
        std::vector<std::pair<DungeonOption const*, std::vector<FiveCandidate>>> pools;
        {
            CoordLock guard(lock_, __func__);
            for (DungeonOption const& d : dungeons_)
            {
                std::vector<FiveCandidate> pool = Candidates(d, true);
                if (guardCount == 0)
                {
                    uint32 inBand = 0;
                    std::map<std::string, uint32> busy;
                    for (PlanRow const& row : plan_)
                    {
                        if (row.grp < 'A' || row.grp > 'C')
                            continue;
                        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
                        if (!p || IsHuman(p) || p->GetLevel() < d.walkMinLevel || p->GetLevel() > d.maxLevel)
                            continue;
                        ++inBand;
                        std::string why;
                        if (!FreeForFive(row.low, p, &why))
                            ++busy[why];
                        else if (std::string const refusal = DungeonRefusal(d, p); !refusal.empty())
                            ++busy[refusal];
                        else if (auto const r = refusedFor_.find(d.contentKey);
                                 r != refusedFor_.end() && r->second.count(row.low) &&
                                 r->second.at(row.low) == p->GetLevel())
                            ++busy["[lfg69] refused at its level"];
                    }
                    if (inBand)
                    {
                        uint32 tanks = 0, healers = 0;
                        for (FiveCandidate const& c : pool)
                            tanks += c.role == "tank" ? 1 : 0, healers += c.role == "healer" ? 1 : 0;
                        std::string text = Acore::StringFormat("{} in band, {} free (t{} h{} d{})", inBand, pool.size(),
                                                               tanks, healers, pool.size() - tanks - healers);
                        if (!busy.empty())
                        {
                            text += " busy:";
                            for (auto const& [why, n] : busy)
                                text += Acore::StringFormat(" {} {},", why, n);
                            text.pop_back();
                        }
                        if (pool.size() < 5)
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
                if (pool.size() >= 5)
                    pools.emplace_back(&d, std::move(pool));
            }
        }
        DungeonOption const* best = nullptr;
        std::vector<FiveCandidate> bestFive;
        uint32 bestScore = 0;
        uint32 bestLeft = UINT32_MAX;  // [five58] the fewest levels a member of the five has left in the band
        for (auto& [d, pool] : pools)
        {
            for (FiveCandidate& c : pool)
                if (!c.needsClear)
                    c.upgrade = HoldsUpgrade(*d, c.player);
            // the dungeon must hold an upgrade or an open clear for each of its dps; [five58] a tank or healer who
            // cleared it already goes again for the next five (user 2026-11-09: the healer runs again with the next
            // five as soon as it is out) - PickFive takes one with a clear or upgrade of its own first
            pool.erase(std::remove_if(pool.begin(), pool.end(),
                                      [](FiveCandidate const& c)
                                      { return !c.needsClear && !c.upgrade && c.role == "dps"; }),
                       pool.end());
            std::vector<FiveCandidate> five;
            // [five58] the finder's band first (the five may queue); else a five that walks in: every member of it
            // passes the walk (same continent, no enemy capital), members below the finder's minimum among them
            std::vector<FiveCandidate> finderPool, walkPool;
            for (FiveCandidate const& c : pool)
            {
                if (c.player->GetLevel() >= d->minLevel)
                    finderPool.push_back(c);
                if (DungeonRefusal(*d, c.player, true).empty())
                    walkPool.push_back(c);
            }
            if (!PickFive(finderPool, five) && !PickFive(walkPool, five))
            {
                if (guardCount == 0 && lookWhy[d->contentKey].find(" -> ") == std::string::npos)
                    lookWhy[d->contentKey] += Acore::StringFormat(
                        " -> {} with a clear or an upgrade to gain: no tank, healer and 3 dps among them", pool.size());
                continue;
            }
            uint32 const score = uint32(std::count_if(five.begin(), five.end(),
                                                      [](FiveCandidate const& c) { return c.needsClear; })) +
                                 uint32(std::count_if(five.begin(), five.end(),
                                                      [](FiveCandidate const& c) { return c.upgrade; }));
            // [five58] user rule: every member clears every dungeon at its own level - a five whose member is about to
            // outlevel the band (a clear open or an upgrade in it) goes first, then the most clears and upgrades
            uint32 left = UINT32_MAX;
            std::string urgent;
            for (FiveCandidate const& c : five)
                if ((c.needsClear || c.upgrade) && d->maxLevel - c.player->GetLevel() < left)
                {
                    left = d->maxLevel - c.player->GetLevel();
                    urgent = c.player->GetName() + " L" + std::to_string(c.player->GetLevel());
                }
            if (guardCount == 0)
                lookWhy[d->contentKey] += Acore::StringFormat(" -> five of score {}, urgency {} level(s) left ({} of "
                                                              "max {})",
                                                              score, left, urgent, d->maxLevel);
            if (best && (left > bestLeft || (left == bestLeft && score <= bestScore)))
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
            FiveGate::Verdict const verdict = FiveGate::Judge(d->mapId, players);
            if (!verdict.clears)
            {
                if (guardCount == 0)
                    lookWhy[d->contentKey] += " -> the gate says no: " + verdict.why;
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
            bestScore = score;
            bestLeft = left;
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
        "; loot: " + (drops.empty() ? "none" : drops) + "; needed: " + (needs.empty() ? "nothing" : needs);
    LOG_INFO("playerbots", "[guild60] report {} run {}: {} ({})", fiveId, report.runId, text, report.reason);
    Decision(text);

    for (uint32 low : five.members)
    {
        BotState& st = bots_[low];
        st.fiveId = 0;
        st.supplyShort.clear();  // [supply27] a new five, or none: its supply check starts again
        st.supplyAsked = st.supplyYielded = st.supplyStarted = st.supplyDone = false;
        st.run = false;
        st.fiveRefused.clear();
    }
    fives_.erase(itr);
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
        for (uint32 low : five.members)
            cleared_[rec.contentKey].insert(low);
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
            if (walkWhy.empty())
                walkWhy = "the finder takes the five from where it stands";
            if (st.fiveRefused.empty() && bot->IsInCombat())
                return false;  // its fight first (the survival kit)
            step.kind = GuildRunStep::BUSY;
            step.reason = "DUNGEON five " + std::to_string(five.id) + " -> " + five.dungeon +
                          ": waits for the Dungeon Finder (no walk in: " + walkWhy + ")";
            if (bot->isMoving())
                bot->StopMoving();
            return true;
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

std::string GuildCoordinator::FivesText() const
{
    CoordLock guard(lock_, __func__);
    std::ostringstream out;
    out << "fives: " << fives_.size() << ", auto " << (autoFives_ ? "on" : "off") << ", runs recorded " << runs_.size();
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
    }
    if (!decided.first)
        return false;
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
        bopSpare = UpgradeGain(player, proto, item->GetItemRandomPropertyId()) <= 0.0f;
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
