/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgProfession.h"

#include <cfloat>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Creature.h"
#include "DBCStores.h"
#include "TravelMgr.h"  // [guild15] trips to another landmass
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "NewRpgCraft.h"  // [guild70]
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsDatabase.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"

namespace
{
    enum : uint8
    {
        TASK_TRAIN = 1,
        TASK_BUY = 2
    };

    struct ProfessionPlan
    {
        std::vector<uint32> primary;
        std::vector<uint32> secondary;
    };

    // Read once on the world thread at startup (LoadPlan), read-only afterwards.
    std::unordered_map<uint32, ProfessionPlan> plans;

    // A trainer spell that raises a skill to rank `step` (max skill = step * 75).
    struct RankSpell
    {
        uint32 spellId;
        uint32 skill;
        uint32 step;
        uint32 cost;
    };

    struct NpcSpawn
    {
        uint32 entry;
        uint16 mapId;
        float x, y, z;
        uint32 zoneId;  // set for the secondary skills' trainers and the fishing pole vendors only
    };

    // [guild15] the trip's length: straight on the bot's landmass; on another one (Shattrath from Azuremyst, both map
    // 530; Stormwind from Ironforge) the travel planner's route (rides, portals, the Deeprun Tram) in walking yards,
    // none: out of reach (b323 Dekorhan L7 walked at Sessoh in Shattrath from Azuremyst, 19 WALK_FAIL)
    float TripDist(Player* bot, NpcSpawn const& spawn)
    {
        WorldPosition const to(spawn.mapId, spawn.x, spawn.y, spawn.z);
        if (TravelRouteMgr::Landmass(WorldPosition(bot)) == TravelRouteMgr::Landmass(to))
            return bot->GetExactDist(spawn.x, spawn.y, spawn.z);
        TravelRouteMgr::Leg leg;
        float seconds = 0.0f;
        if (!sTravelRouteMgr.FindRoute(bot, to, leg, seconds))
            return FLT_MAX;
        return seconds * TravelRouteMgr::WALK_SPEED;
    }

    struct Index
    {
        std::unordered_map<uint32, std::vector<RankSpell>> ranks;     // trainer entry -> rank spells
        std::unordered_map<uint32, std::vector<NpcSpawn>> trainers;   // skill -> trainer spawns teaching it
        std::unordered_map<uint32, std::vector<NpcSpawn>> toolVendors;  // tool item -> vendor spawns selling it
    };

    // The tool a profession's work needs, bought at a vendor (the game's own items).
    uint32 ToolFor(uint32 skill)
    {
        switch (skill)
        {
            case SKILL_MINING:         return 2901;   // Mining Pick
            case SKILL_SKINNING:       return 7005;   // Skinning Knife
            case SKILL_BLACKSMITHING:  return 5956;   // Blacksmith Hammer
            case SKILL_ENGINEERING:    return 6219;   // Arclight Spanner
            case SKILL_JEWELCRAFTING:  return 20815;  // Jeweler's Kit
            case SKILL_INSCRIPTION:    return 39505;  // Virtuoso Inking Set
            case SKILL_FISHING:        return 6256;   // Fishing Pole
            default:                   return 0;
        }
    }

    bool IsSecondary(uint32 skill)
    {
        return skill == SKILL_COOKING || skill == SKILL_FIRST_AID || skill == SKILL_FISHING;
    }

    // The skill step a spell teaches: SPELL_EFFECT_SKILL (the profession rank spells; SpellMgr's learn-skill node,
    // max = step * 75) or SPELL_EFFECT_SKILL_STEP (Spell::EffectLearnSkill).
    bool SkillStepOf(uint32 spellId, uint32& skill, uint32& step)
    {
        if (SpellLearnSkillNode const* node = sSpellMgr->GetSpellLearnSkill(spellId))
        {
            skill = node->skill;
            step = std::max<uint32>(1, node->step);
            return true;
        }
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect == SPELL_EFFECT_SKILL_STEP && info->Effects[i].MiscValue > 0)
            {
                skill = uint32(info->Effects[i].MiscValue);
                step = uint32(std::max(1, info->Effects[i].CalcValue()));
                return true;
            }
        return false;
    }

    Index const& GetIndex()
    {
        static Index index;
        static std::once_flag once;
        std::call_once(once, []()
        {
            std::unordered_map<uint32, bool> seen;  // trainer entry -> teaches a rank
            std::unordered_map<uint32, bool> tools;
            for (uint32 skill : {SKILL_MINING, SKILL_SKINNING, SKILL_BLACKSMITHING, SKILL_ENGINEERING,
                                 SKILL_JEWELCRAFTING, SKILL_INSCRIPTION, SKILL_FISHING})
                tools[ToolFor(skill)] = true;

            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
                if (!tmpl)
                    continue;
                NpcSpawn spawn{data.id, data.mapid, data.posX, data.posY, data.posZ, 0};

                if (tmpl->npcflag & UNIT_NPC_FLAG_TRAINER)
                {
                    auto itr = seen.find(data.id);
                    if (itr == seen.end())
                    {
                        bool any = false;
                        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(data.id);
                        if (trainer && trainer->GetTrainerType() == Trainer::Type::Tradeskill)
                            for (Trainer::Spell const& spell : trainer->GetSpells())
                            {
                                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId);
                                if (!info)
                                    continue;
                                std::vector<uint32> taught;
                                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                                    if (info->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL)
                                        taught.push_back(info->Effects[i].TriggerSpell);
                                if (taught.empty())
                                    taught.push_back(spell.SpellId);
                                for (uint32 t : taught)
                                {
                                    uint32 skill = 0, step = 0;
                                    if (SkillStepOf(t, skill, step))
                                    {
                                        index.ranks[data.id].push_back({spell.SpellId, skill, step, spell.MoneyCost});
                                        any = true;
                                    }
                                }
                            }
                        itr = seen.emplace(data.id, any).first;
                    }
                    if (itr->second)
                    {
                        std::vector<uint32> skills;
                        for (RankSpell const& rank : index.ranks[data.id])
                            if (std::find(skills.begin(), skills.end(), rank.skill) == skills.end())
                                skills.push_back(rank.skill);
                        for (uint32 skill : skills)
                        {
                            if (IsSecondary(skill) && !spawn.zoneId)
                                spawn.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, spawn.mapId, spawn.x, spawn.y,
                                                                  spawn.z);
                            index.trainers[skill].push_back(spawn);
                        }
                    }
                }

                if (tmpl->npcflag & UNIT_NPC_FLAG_VENDOR)
                    if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id))
                        for (VendorItem const* item : items->m_items)
                            if (item && !item->ExtendedCost && tools.count(item->item))
                            {
                                if (item->item == ToolFor(SKILL_FISHING) && !spawn.zoneId)
                                    spawn.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, spawn.mapId, spawn.x,
                                                                      spawn.y, spawn.z);
                                index.toolVendors[item->item].push_back(spawn);
                            }
            }
            std::ostringstream toolText;
            for (auto const& [item, spawns] : index.toolVendors)
                toolText << " " << item << "x" << spawns.size();
            std::ostringstream skills;
            for (auto const& [skill, spawns] : index.trainers)
                skills << " " << skill << "x" << spawns.size();
            LOG_INFO("playerbots", "[guild20] profession index: {} tradeskill trainers; trainer spawns by skill:{}; "
                     "tool vendor spawns:{}", index.ranks.size(), skills.str(), toolText.str());
        });
        return index;
    }

    bool Friendly(Player* bot, uint32 entry)
    {
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        FactionTemplateEntry const* faction = tmpl ? sFactionTemplateStore.LookupEntry(tmpl->faction) : nullptr;
        return own && faction && !faction->IsHostileTo(*own) && !own->IsHostileTo(*faction);
    }

    // Errands that failed at the npc (gone, refused): skipped while the bot's money and level stay the same.
    std::mutex failLock;
    std::map<std::pair<uint32, uint64>, std::pair<uint32, uint8>> failed;

    uint64 FailKey(uint32 npcEntry, uint32 id) { return (uint64(npcEntry) << 32) | id; }

    bool FailedBefore(Player* bot, uint32 npcEntry, uint32 id)
    {
        std::lock_guard<std::mutex> guard(failLock);
        auto itr = failed.find({bot->GetGUID().GetCounter(), FailKey(npcEntry, id)});
        return itr != failed.end() && itr->second.first == bot->GetMoney() && itr->second.second == bot->GetLevel();
    }

    void MarkFailed(Player* bot, uint32 npcEntry, uint32 id)
    {
        std::lock_guard<std::mutex> guard(failLock);
        failed[{bot->GetGUID().GetCounter(), FailKey(npcEntry, id)}] = {bot->GetMoney(), uint8(bot->GetLevel())};
    }

    bool HasTool(Player* bot, uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (proto && proto->TotemCategory && bot->HasItemTotemCategory(proto->TotemCategory))
            return true;
        return bot->HasItemCount(itemId, 1, false);
    }

    char const* SkillName(uint32 skill)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        return line ? line->name[0] : "?";
    }

    struct Candidate
    {
        NewRpgInfo::DoProfession task;
        float dist{FLT_MAX};
        NpcSpawn const* spawn = nullptr;  // [guild14] the spawn chosen
    };

    // The next rank of `skill` at the nearest friendly trainer on the bot's map that can teach it now
    // (Trainer::CanTeachSpell: level, skill rank, free primary profession slot). sameZone: only trainers in the
    // zone the bot stands in. Sets `waiting` when such a rank exists but the bot cannot pay for it.
    void TrainCandidate(Player* bot, uint32 skill, bool sameZone, Candidate& best, bool& waiting,
                        std::unordered_set<NpcSpawn const*> const& skip)
    {
        uint32 const curMax = bot->HasSkill(skill) ? bot->GetPureMaxSkillValue(skill) : 0;
        uint32 const step = curMax / 75 + 1;
        Index const& index = GetIndex();
        auto spawns = index.trainers.find(skill);
        if (spawns == index.trainers.end())
            return;
        uint32 const zone = bot->GetZoneId();
        for (NpcSpawn const& spawn : spawns->second)
        {
            if (spawn.mapId != bot->GetMapId() || !Friendly(bot, spawn.entry) || skip.count(&spawn))
                continue;
            float const d = TripDist(bot, spawn);
            if (d >= best.dist)
                continue;
            auto ranks = index.ranks.find(spawn.entry);
            if (ranks == index.ranks.end())
                continue;
            for (RankSpell const& rank : ranks->second)
            {
                if (rank.skill != skill || rank.step != step)
                    continue;
                Trainer::Trainer* trainer = sObjectMgr->GetTrainer(spawn.entry);
                Trainer::Spell const* spell = trainer ? trainer->GetSpell(rank.spellId) : nullptr;
                if (!spell || !trainer->IsTrainerValidForPlayer(bot) || !trainer->CanTeachSpell(bot, spell))
                    continue;
                if (sameZone && spawn.zoneId != zone)
                    continue;
                if (FailedBefore(bot, spawn.entry, rank.spellId))
                    continue;
                if (bot->GetMoney() < rank.cost)
                {
                    waiting = true;
                    continue;
                }
                best.dist = d;
                best.task = NewRpgInfo::DoProfession{};
                best.spawn = &spawn;
                best.task.kind = TASK_TRAIN;
                best.task.npcEntry = spawn.entry;
                best.task.pos = WorldPosition(spawn.mapId, spawn.x, spawn.y, spawn.z);
                best.task.spellId = rank.spellId;
                best.task.skill = skill;
                best.task.cost = rank.cost;
                break;
            }
        }
    }

    // The tool of a learned skill the bot lacks, at the nearest friendly vendor on its map selling it.
    void ToolCandidate(Player* bot, uint32 skill, bool sameZone, Candidate& best, bool& waiting,
                       std::unordered_set<NpcSpawn const*> const& skip)
    {
        uint32 const itemId = ToolFor(skill);
        if (!itemId || !bot->HasSkill(skill) || HasTool(bot, itemId))
            return;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            return;
        Index const& index = GetIndex();
        auto spawns = index.toolVendors.find(itemId);
        if (spawns == index.toolVendors.end())
            return;
        uint32 const zone = bot->GetZoneId();
        for (NpcSpawn const& spawn : spawns->second)
        {
            if (spawn.mapId != bot->GetMapId() || !Friendly(bot, spawn.entry) || skip.count(&spawn))
                continue;
            float const d = TripDist(bot, spawn);
            if (d >= best.dist)
                continue;
            if (sameZone && spawn.zoneId != zone)
                continue;
            if (FailedBefore(bot, spawn.entry, itemId))
                continue;
            if (bot->GetMoney() < proto->BuyPrice)
            {
                waiting = true;
                continue;
            }
            best.dist = d;
            best.task = NewRpgInfo::DoProfession{};
            best.spawn = &spawn;
            best.task.kind = TASK_BUY;
            best.task.npcEntry = spawn.entry;
            best.task.pos = WorldPosition(spawn.mapId, spawn.x, spawn.y, spawn.z);
            best.task.itemId = itemId;
            best.task.skill = skill;
            best.task.cost = proto->BuyPrice;
        }
    }
}

namespace
{
    uint32 RecipeFailId(uint32 skill) { return 0x80000000u | skill; }

    // [guild70] a skill with no known recipe that still gives skill: the nearest friendly trainer on the map that
    // teaches one now, within the budget (secondary skills: in this zone only)
    bool RecipeCandidate(Player* bot, uint32 skill, bool sameZone, uint32 budget, Candidate& best,
                         std::unordered_set<NpcSpawn const*> const& skip)
    {
        if (!bot->HasSkill(skill) || bot->GetPureSkillValue(skill) >= bot->GetPureMaxSkillValue(skill) ||
            NewRpgCraft::HasSkillUpRecipe(bot, skill))
            return false;
        Index const& index = GetIndex();
        auto spawns = index.trainers.find(skill);
        if (spawns == index.trainers.end())
            return false;
        std::unordered_map<uint32, std::pair<bool, uint32>> teaches;  // trainer entry -> (teaches, cost)
        bool found = false;
        for (NpcSpawn const& spawn : spawns->second)
        {
            if (spawn.mapId != bot->GetMapId() || skip.count(&spawn) || (sameZone && spawn.zoneId != bot->GetZoneId()))
                continue;
            float const d = TripDist(bot, spawn);
            if (d >= best.dist || !Friendly(bot, spawn.entry) ||
                FailedBefore(bot, spawn.entry, RecipeFailId(skill)))
                continue;
            auto itr = teaches.find(spawn.entry);
            if (itr == teaches.end())
            {
                uint32 cost = 0;
                bool const ok = NewRpgCraft::TeachesSkillUpRecipe(bot, spawn.entry, skill, cost);
                itr = teaches.emplace(spawn.entry, std::make_pair(ok, cost)).first;
            }
            if (!itr->second.first || itr->second.second > budget)
                continue;
            best.dist = d;
            best.task = NewRpgInfo::DoProfession{};
            best.spawn = &spawn;
            best.task.kind = NewRpgCraft::TASK_RECIPES;
            best.task.npcEntry = spawn.entry;
            best.task.pos = WorldPosition(spawn.mapId, spawn.x, spawn.y, spawn.z);
            best.task.skill = skill;
            best.task.cost = itr->second.second;
            found = true;
        }
        return found;
    }

    // what the recipe look saw last time it found nothing (skills, money, level): not looked at again until it changes
    std::mutex recipeLookLock;
    std::unordered_map<uint32, uint64> recipeLook;

    uint64 RecipeLookKey(Player* bot)
    {
        uint64 key = (uint64(bot->GetMoney()) << 24) ^ (uint64(bot->GetLevel()) << 16) ^ bot->GetZoneId();
        for (uint32 skill : {SKILL_BLACKSMITHING, SKILL_LEATHERWORKING, SKILL_ALCHEMY, SKILL_TAILORING,
                             SKILL_ENGINEERING, SKILL_ENCHANTING, SKILL_JEWELCRAFTING, SKILL_INSCRIPTION,
                             SKILL_COOKING, SKILL_FIRST_AID, SKILL_MINING})
            if (bot->HasSkill(skill))
                key = key * 1000003 + bot->GetPureSkillValue(skill);
        return key;
    }

    // [guild70] the recipe trip, when no rank or tool errand is due
    bool StartRecipeTrip(PlayerbotAI* botAI, Player* bot, ProfessionPlan const& plan)
    {
        uint32 const low = bot->GetGUID().GetCounter();
        uint64 const key = RecipeLookKey(bot);
        {
            std::lock_guard<std::mutex> guard(recipeLookLock);
            auto const itr = recipeLook.find(low);
            if (itr != recipeLook.end() && itr->second == key)
                return false;
        }
        uint32 const budget = NewRpgCraft::Budget(botAI, bot);
        std::unordered_set<NpcSpawn const*> refused;
        while (true)
        {
            Candidate best;
            for (uint32 skill : plan.primary)
                RecipeCandidate(bot, skill, false, budget, best, refused);
            for (uint32 skill : plan.secondary)
                RecipeCandidate(bot, skill, true, budget, best, refused);
            if (bot->HasSkill(SKILL_MINING) &&
                std::find(plan.primary.begin(), plan.primary.end(), uint32(SKILL_MINING)) == plan.primary.end())
                RecipeCandidate(bot, SKILL_MINING, true, budget, best, refused);
            if (!best.spawn)
            {
                std::lock_guard<std::mutex> guard(recipeLookLock);
                recipeLook[low] = key;
                return false;
            }
            CreatureTemplate const* npc = sObjectMgr->GetCreatureTemplate(best.task.npcEntry);
            if (NewRpgBaseAction::TripRefused(bot, best.spawn->x, best.spawn->y, best.spawn->z,
                                              std::string("recipe trainer ") + (npc ? npc->Name : "?")))
            {
                refused.insert(best.spawn);
                continue;
            }
            LOG_INFO("playerbots",
                     "[rpg] {} L{} PROFESSION reason: no known {} recipe still gives skill at {}: learn recipes at {} "
                     "({:.0f} yd), cheapest {}c, budget {}c",
                     bot->GetName(), bot->GetLevel(), SkillName(best.task.skill),
                     bot->GetPureSkillValue(best.task.skill), npc ? npc->Name : "?", best.dist, best.task.cost, budget);
            botAI->rpgInfo.ChangeToDoProfession(best.task);
            return true;
        }
    }
}

namespace NewRpgProfession
{
    void LoadPlan()
    {
        plans.clear();
        QueryResult result = PlayerbotsDatabase.Query("SELECT guid, prof1, prof2, secondary FROM guild_project_plan");
        if (!result)
            return;
        do
        {
            Field* fields = result->Fetch();
            ProfessionPlan plan;
            for (uint32 skill : {fields[1].Get<uint32>(), fields[2].Get<uint32>()})
                if (skill)
                    plan.primary.push_back(skill);
            std::stringstream in(fields[3].Get<std::string>());
            std::string token;
            while (std::getline(in, token, ','))
                if (uint32 const skill = uint32(std::strtoul(token.c_str(), nullptr, 10)))
                    plan.secondary.push_back(skill);
            plans[fields[0].Get<uint32>()] = plan;
        } while (result->NextRow());
        LOG_INFO("playerbots", "[guild20] profession plan: {} characters", plans.size());
        GetIndex();  // built here on the world thread, read-only on the map threads
        NewRpgCraft::Load();  // [guild70]
    }

    bool StartTask(PlayerbotAI* botAI, Player* bot)
    {
        auto plan = plans.find(bot->GetGUID().GetCounter());
        if (plan == plans.end() || !bot->IsAlive() || bot->IsInCombat())
            return false;

        // [guild14] a solo bot's walk to the npc must be safe (NewRpgBaseAction::TripRefused: b303 Thokhen L3 and
        // Laaral L10 walked their errands through mobs 5-6 levels above them): else the next nearest npc, none ->
        // the errand waits for a level
        Candidate best;
        bool waiting = false;
        std::string kindText = "primary";
        std::unordered_set<NpcSpawn const*> refused;
        while (true)
        {
            best = Candidate();
            waiting = false;
            kindText = "primary";
            for (uint32 skill : plan->second.primary)
            {
                TrainCandidate(bot, skill, false, best, waiting, refused);
                ToolCandidate(bot, skill, false, best, waiting, refused);
            }
            // Secondary skills are learned on the way: a trainer in this zone, and no primary rank or tool waiting
            // for the money.
            if (!best.task.kind && !waiting)
            {
                kindText = "secondary";
                bool unused = false;
                for (uint32 skill : plan->second.secondary)
                {
                    TrainCandidate(bot, skill, true, best, unused, refused);
                    ToolCandidate(bot, skill, true, best, unused, refused);
                }
            }
            // [guild70] no rank / tool errand: a recipe visit, else skill-up crafting in town
            if (!best.task.kind || !best.spawn)
                return StartRecipeTrip(botAI, bot, plan->second) || NewRpgCraft::StartCraft(botAI, bot);
            CreatureTemplate const* npc = sObjectMgr->GetCreatureTemplate(best.task.npcEntry);
            if (!NewRpgBaseAction::TripRefused(bot, best.spawn->x, best.spawn->y, best.spawn->z,
                                               std::string("profession npc ") + (npc ? npc->Name : "?")))
                break;
            refused.insert(best.spawn);
        }

        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(best.task.npcEntry);
        LOG_INFO("playerbots", "[rpg] {} L{} PROFESSION reason: {} {} {} {} at {} ({:.0f} yd), cost {}c, money {}c",
                 bot->GetName(), bot->GetLevel(), kindText, best.task.kind == TASK_TRAIN ? "learn" : "buy tool",
                 SkillName(best.task.skill),
                 best.task.kind == TASK_TRAIN ? "rank " + std::to_string(best.task.spellId)
                                              : "item " + std::to_string(best.task.itemId),
                 tmpl ? tmpl->Name : "?", best.dist, best.task.cost, bot->GetMoney());
        botAI->rpgInfo.ChangeToDoProfession(best.task);
        return true;
    }

    void StartBuy(PlayerbotAI* botAI, Player* bot, uint32 itemId, uint32 npcEntry, WorldPosition const& pos,
                  uint32 cost)
    {
        NewRpgInfo::DoProfession task;
        task.kind = TASK_BUY;
        task.npcEntry = npcEntry;
        task.pos = pos;
        task.itemId = itemId;
        task.cost = cost;
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(npcEntry);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        LOG_INFO("playerbots", "[rpg] {} L{} GUILD BUY reason: {} at {} ({:.0f} yd), cost {}c, money {}c",
                 bot->GetName(), bot->GetLevel(), proto ? proto->Name1 : "?", tmpl ? tmpl->Name : "?",
                 bot->GetExactDist(pos), cost, bot->GetMoney());
        botAI->rpgInfo.ChangeToDoProfession(task);
    }

    bool BuyFailed(Player* bot, uint32 npcEntry, uint32 itemId) { return FailedBefore(bot, npcEntry, itemId); }

    uint32 NextRankCost(Player* bot)
    {
        auto plan = plans.find(bot->GetGUID().GetCounter());
        if (plan == plans.end())
            return 0;
        Index const& index = GetIndex();
        uint32 total = 0;
        for (uint32 skill : plan->second.primary)
        {
            uint32 const curMax = bot->HasSkill(skill) ? bot->GetPureMaxSkillValue(skill) : 0;
            uint32 const step = curMax / 75 + 1;
            uint32 best = 0;
            for (auto const& [entry, ranks] : index.ranks)
                for (RankSpell const& rank : ranks)
                {
                    if (rank.skill != skill || rank.step != step || (best && rank.cost >= best))
                        continue;
                    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
                    Trainer::Spell const* spell = trainer ? trainer->GetSpell(rank.spellId) : nullptr;
                    if (spell && trainer->CanTeachSpell(bot, spell))
                        best = rank.cost;
                }
            total += best;
        }
        return total;
    }

    uint32 MissingTools(Player* bot, std::string& names)
    {
        auto plan = plans.find(bot->GetGUID().GetCounter());
        if (plan == plans.end())
            return 0;
        uint32 total = 0;
        for (auto const* list : {&plan->second.primary, &plan->second.secondary})
            for (uint32 skill : *list)
            {
                uint32 const itemId = ToolFor(skill);
                ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
                if (!proto || !bot->HasSkill(skill) || HasTool(bot, itemId))
                    continue;
                total += proto->BuyPrice;
                names += (names.empty() ? "" : ", ") + std::string(proto->Name1);
            }
        return total;
    }

    std::string PlannedSkills(Player* bot)
    {
        auto plan = plans.find(bot->GetGUID().GetCounter());
        if (plan == plans.end())
            return "";
        std::ostringstream out;
        bool first = true;
        auto add = [&](uint32 skill)
        {
            if (!first)
                out << ",";
            first = false;
            out << skill << ":" << (bot->HasSkill(skill) ? bot->GetPureSkillValue(skill) : 0) << "/"
                << (bot->HasSkill(skill) ? bot->GetPureMaxSkillValue(skill) : 0);
        };
        for (uint32 skill : plan->second.primary)
            add(skill);
        for (uint32 skill : plan->second.secondary)
            add(skill);
        return out.str();
    }
}

bool NewRpgDoProfessionAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* data = std::get_if<NewRpgInfo::DoProfession>(&info.data);
    if (!data || !bot->IsAlive())
        return false;
    if (bot->GetMapId() != data->pos.GetMapId())
    {
        info.ChangeToIdle();
        return true;
    }
    if (data->kind == NewRpgCraft::TASK_CRAFT)  // [guild70]
        return ExecuteCraft(*data);
    uint32 const id = data->kind == TASK_TRAIN              ? data->spellId
                      : data->kind == NewRpgCraft::TASK_RECIPES ? RecipeFailId(data->skill)
                                                                : data->itemId;

    if (data->npc.IsEmpty())
    {
        if (bot->GetExactDist(data->pos) > 10.0f)
        {
            uint32 const npcEntry = data->npcEntry;
            bool const moved = MoveFarTo(data->pos);
            // the far move gave the destination up as unreachable: next time the next trainer / vendor
            if (!std::holds_alternative<NewRpgInfo::DoProfession>(info.data))
            {
                LOG_INFO("playerbots", "[guild20] {} cannot reach npc {}: tries another one", bot->GetName(),
                         npcEntry);
                MarkFailed(bot, npcEntry, id);
                return true;
            }
            if (moved)
                return true;
            return MoveRandomNear(10.0f);
        }
        Creature* npc = bot->FindNearestCreature(data->npcEntry, 60.0f);
        if (!npc)
        {
            LOG_INFO("playerbots", "[guild20] {} finds no npc {} at its spawn", bot->GetName(), data->npcEntry);
            MarkFailed(bot, data->npcEntry, id);
            info.ChangeToIdle();
            return true;
        }
        data->npc = npc->GetGUID();
    }

    Creature* npc = ObjectAccessor::GetCreature(*bot, data->npc);
    if (!npc || !npc->IsAlive())
    {
        MarkFailed(bot, data->npcEntry, id);
        info.ChangeToIdle();
        return true;
    }
    if (!IsWithinInteractionDist(npc))
    {
        // the walk under way: it goes on
        if (bot->isMoving())
        {
            info.holdNote.clear();
            return true;
        }
        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        {
            // [stuck8] standing with its last walk's wait open: a HOLD line with the cause, once (b343 Thokhen L9 at
            // Mahri 0 yd from her spawn, DO_PROFESSION BUY, 60 s with no line, "reason=none")
            std::string const key = "DO_PROFESSION|last walk";
            if (info.holdNote != key)
            {
                info.holdNote = key;
                LOG_INFO("playerbots", "[rpg] {} L{} HOLD reason: DO_PROFESSION at {} {:.1f} yd (interaction {:.1f} "
                         "yd): stands, the wait of its last walk is open",
                         bot->GetName(), bot->GetLevel(), npc->GetName(), bot->GetExactDist(npc),
                         INTERACTION_DISTANCE);
            }
            return true;
        }
        // [guild15] the last walk to the npc ended and brought it no nearer: the step ends with a logged failure,
        // never a stand at the npc (b327-b333 Selalleas L6: "learn Alchemy at Ainethil 9 yd" in Darnassus, 225 s
        // STUCK in every run - the walk was issued and never moved it)
        float const gap = bot->GetExactDist(npc);
        if (data->walkGap >= 0.0f && gap >= data->walkGap)
        {
            LOG_INFO("playerbots", "[guild20] {} L{} PROFESSION_FAIL cannot reach {}: its walk there ended {:.1f} yd "
                     "off (z {:.1f}, the npc's z {:.1f}), no nearer than before; the next trainer / vendor",
                     bot->GetName(), bot->GetLevel(), npc->GetName(), gap, bot->GetPositionZ(), npc->GetPositionZ());
            MarkFailed(bot, data->npcEntry, id);
            info.ChangeToIdle();
            return true;
        }
        data->walkGap = gap;
        if (MoveWorldObjectTo(data->npc))
            return true;
        std::string const refusal = LastMoveRefusal();
        if (NpcOutOfWalk(npc))  // [guild15]
        {
            MarkFailed(bot, data->npcEntry, id);
            info.ChangeToIdle();
            return true;
        }
        LOG_INFO("playerbots", "[guild20] {} L{} PROFESSION_FAIL cannot walk to {} {:.1f} yd off: the walk refused "
                 "({})",
                 bot->GetName(), bot->GetLevel(), npc->GetName(), gap, refusal.empty() ? "no reason" : refusal);
        MarkFailed(bot, data->npcEntry, id);
        info.ChangeToIdle();
        return true;
    }

    bot->SetFacingToObject(npc);
    uint32 const moneyBefore = bot->GetMoney();
    if (data->kind == NewRpgCraft::TASK_RECIPES)  // [guild70] recipes only
    {
        uint32 const learned = NewRpgCraft::LearnRecipes(bot, npc, data->skill, NewRpgCraft::Budget(botAI, bot));
        LOG_INFO("playerbots", "[guild70] {} L{} learned {} {} recipes at {}", bot->GetName(), bot->GetLevel(), learned,
                 SkillName(data->skill), npc->GetName());
        if (!learned)
            MarkFailed(bot, data->npcEntry, id);
    }
    else if (data->kind == TASK_TRAIN)
    {
        uint16 const maxBefore = bot->HasSkill(data->skill) ? bot->GetPureMaxSkillValue(data->skill) : 0;
        uint16 const valueBefore = bot->HasSkill(data->skill) ? bot->GetPureSkillValue(data->skill) : 0;
        if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry()))
            trainer->TeachSpell(npc, bot, data->spellId);  // the trainer window's path: checks, pays, learns
        uint16 const maxAfter = bot->HasSkill(data->skill) ? bot->GetPureMaxSkillValue(data->skill) : 0;
        bool const learned = maxAfter > maxBefore;
        LOG_INFO("playerbots", "[guild20] {} L{} {} {} at {}: {} rank {}, skill {}/{} -> {}/{}, paid {}c ({}c left)",
                 bot->GetName(), bot->GetLevel(), learned ? "learned" : "could not learn", SkillName(data->skill),
                 npc->GetName(), data->spellId, maxAfter / 75, valueBefore, maxBefore,
                 bot->HasSkill(data->skill) ? bot->GetPureSkillValue(data->skill) : 0, maxAfter,
                 moneyBefore - bot->GetMoney(), bot->GetMoney());
        if (!learned)
            MarkFailed(bot, data->npcEntry, id);
        // [guild70] with the rank: the recipes it can learn now
        if (bot->HasSkill(data->skill))
            NewRpgCraft::LearnRecipes(bot, npc, data->skill, NewRpgCraft::Budget(botAI, bot));
    }
    else
    {
        bool bought = false;
        if (VendorItemData const* items = npc->GetVendorItems())
            for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
            {
                VendorItem const* item = items->GetItem(slot);
                // [guild50] a guild BUY (no skill) may pay an extended cost (tokens, emblems); a tool is bought with gold
                if (!item || item->item != data->itemId || (item->ExtendedCost && data->skill))
                    continue;
                uint32 const before = bot->GetItemCount(data->itemId, false);
                bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, data->itemId, 1, NULL_BAG, NULL_SLOT);
                bought = bot->GetItemCount(data->itemId, false) > before;
                break;
            }
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(data->itemId);
        LOG_INFO("playerbots", "[guild20] {} L{} {} {} for {} at {}: paid {}c ({}c left)", bot->GetName(),
                 bot->GetLevel(), bought ? "bought" : "could not buy", proto ? proto->Name1 : "?",
                 SkillName(data->skill), npc->GetName(), moneyBefore - bot->GetMoney(), bot->GetMoney());
        if (!bought)
            MarkFailed(bot, data->npcEntry, id);
    }
    info.ChangeToIdle();
    return true;
}
