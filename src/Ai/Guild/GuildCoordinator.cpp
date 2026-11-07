/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildCoordinator.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <sstream>
#include <tuple>

#include "AiObjectContext.h"
#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Engine.h"
#include "Formulas.h"
#include "GameTime.h"
#include "Group.h"
#include "Guild.h"
#include "GuildBankRun.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotsDatabase.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "SelfBotControl.h"
#include "SellAction.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "UpdateTime.h"
#include "MotionMaster.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

namespace
{
    constexpr uint32 SPELL_DISENCHANT = 13262;
    constexpr size_t ADDON_LINE_MAX = 240;  // the client drops longer addon messages
    constexpr uint32 STUCK_SECONDS = 60;     // user rule: no movement beyond combat reach for 60 s
    uint32 const CRAFT_LINES[] = {164, 165, 171, 197, 202, 333, 755, 773, 185, 129};

    char const* OrderName(GuildOrderType type)
    {
        static char const* const names[] = {"QUEST", "PROFESSION", "BANK_DEPOSIT", "BANK_WITHDRAW", "CRAFT_FOR",
                                             "GATHER", "BUY", "DUNGEON", "DISENCHANT"};
        return uint8(type) < std::size(names) ? names[uint8(type)] : "?";
    }

    char const* RpgName(uint32 status)
    {
        static char const* const names[] = {"IDLE", "GO_GRIND", "GO_CAMP", "WANDER_RANDOM", "WANDER_NPC", "DO_QUEST",
                                             "TRAVEL_FLIGHT", "REST", "OUTDOOR_PVP", "DO_GATHER", "DO_PROFESSION"};
        return status < std::size(names) ? names[status] : "?";
    }

    char const* SkillShort(uint32 line)
    {
        switch (line)
        {
            case 164: return "кузн";
            case 165: return "кожев";
            case 171: return "алх";
            case 197: return "портн";
            case 202: return "инж";
            case 333: return "чар";
            case 755: return "ювел";
            case 773: return "начерт";
            case 185: return "кулин";
            case 129: return "перев";
            case 186: return "горн";
            case 182: return "трав";
            case 393: return "шкуры";
            default: return "?";
        }
    }

    std::string OrdersText(std::string text, size_t max)
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

    uint32 Now() { return uint32(GameTime::GetGameTime().count()); }

    // the engine's last-action trail, newest part only
    std::string LastAction(PlayerbotAI* ai)
    {
        if (!ai || !ai->GetCurrentEngine())
            return "-";
        std::string const all = ai->GetCurrentEngine()->GetLastAction();
        return all.size() > 140 ? "..." + all.substr(all.size() - 140) : all;
    }

    std::string ItemName(uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return proto ? proto->Name1 : std::to_string(itemId);
    }

    // Player::UpdateCraftSkill -> CraftSkillGainChance (per mille), the same formula and config values
    int32 CraftChance(uint32 skillValue, uint32 grayLevel, uint32 yellowLevel)
    {
        int32 const orange = int32(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE) * 10);
        int32 const gray = int32(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREY) * 10);
        if (grayLevel <= yellowLevel)
            return skillValue < grayLevel ? orange : gray;
        if (skillValue <= yellowLevel)
            return orange;
        if (skillValue >= grayLevel)
            return gray;
        return gray + int32(int64(grayLevel - skillValue) * (orange - gray) / (grayLevel - yellowLevel));
    }

    std::string ConText(Player* bot, Creature* killer)
    {
        int32 const diff = int32(killer->GetLevel()) - int32(bot->GetLevel());
        std::string con = diff >= 5 ? "red" : diff >= 3 ? "orange" : diff >= -2 ? "yellow" : "green";
        if (killer->GetLevel() <= Acore::XP::GetGrayLevel(bot->GetLevel()))
            con = "gray";
        std::string text = con + (diff >= 0 ? " +" : " ") + std::to_string(diff);
        if (killer->isElite())
            text += " elite";
        if (killer->IsDungeonBoss() || killer->isWorldBoss())
            text += " boss";
        uint32 const attackers = uint32(bot->getAttackers().size());
        if (attackers > 1)
            text += ", " + std::to_string(attackers) + " attackers";
        return text;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// startup
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::Load()
{
    LoadPlan();
    LoadRecipes();
    LoadGatherSources();
    GuildBankRun::LoadVaults();
    loaded_ = true;
}

void GuildCoordinator::LoadPlan()
{
    plan_.clear();
    planIndex_.clear();
    if (QueryResult result =
            PlayerbotsDatabase.Query("SELECT guid, num, name, grp, role, prof1, prof2 FROM guild_project_plan ORDER BY num"))
    {
        do
        {
            Field* f = result->Fetch();
            PlanRow row;
            row.low = f[0].Get<uint32>();
            row.num = f[1].Get<uint8>();
            row.name = f[2].Get<std::string>();
            std::string const grp = f[3].Get<std::string>();
            row.grp = grp.empty() ? 'P' : grp[0];
            row.role = f[4].Get<std::string>();
            for (uint32 i : {5u, 6u})
                if (uint32 const prof = f[i].Get<uint16>())
                    row.profs.push_back(prof);
            if (planIndex_.emplace(row.low, plan_.size()).second)
                plan_.push_back(row);
        } while (result->NextRow());
    }
    // the user's pack characters not in the plan (his own character among them)
    std::stringstream in(sConfigMgr->GetOption<std::string>("Playerbots.UserPack.Guids", "", false));
    std::string token;
    while (std::getline(in, token, ','))
    {
        uint32 const low = uint32(std::strtoul(token.c_str(), nullptr, 10));
        if (low && planIndex_.emplace(low, plan_.size()).second)
            plan_.push_back({low, 0, "", 'P', "pack", {}});
    }
    aiCount_ = plan_.size();
    aiTimeUs_ = std::make_unique<std::atomic<uint64>[]>(aiCount_ ? aiCount_ : 1);
    for (size_t i = 0; i < aiCount_; ++i)
        aiTimeUs_[i] = 0;
    LOG_INFO("playerbots", "[guild30] coordinator: {} planned characters", plan_.size());
}

void GuildCoordinator::LoadRecipes()
{
    recipesBySkill_.clear();
    smeltFrom_.clear();
    vendorItems_.clear();

    // trainer recipes: spell -> (skill rank, level); a trainer spell that teaches another spell stands for that one
    std::unordered_map<uint32, std::pair<uint32, uint32>> trainerReq;
    if (QueryResult result = WorldDatabase.Query("SELECT SpellId, ReqSkillLine, ReqSkillRank, ReqLevel FROM trainer_spell"))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 spellId = f[0].Get<uint32>();
            if (!f[1].Get<uint32>())
                continue;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                    if (info->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL && info->Effects[e].TriggerSpell)
                        spellId = info->Effects[e].TriggerSpell;
            auto& req = trainerReq[spellId];
            uint32 const rank = f[2].Get<uint32>(), level = f[3].Get<uint8>();
            if (!req.first || rank < req.first)
                req = {rank, level};
        } while (result->NextRow());
    }

    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE maxcount = 0"))
    {
        do
            vendorItems_.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }

    std::unordered_set<uint32> lines(std::begin(CRAFT_LINES), std::end(CRAFT_LINES));
    uint32 count = 0;
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
        if (!ability || (!lines.count(ability->SkillLine) && ability->SkillLine != SKILL_MINING))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell);
        if (!info)
            continue;
        Recipe recipe;
        recipe.spellId = ability->Spell;
        recipe.skillLine = ability->SkillLine;
        recipe.yellow = ability->TrivialSkillLineRankLow;
        recipe.gray = ability->TrivialSkillLineRankHigh;
        recipe.autoLearned = ability->AcquireMethod == 1;
        recipe.reqSkill = ability->MinSkillLineRank;
        auto const trainer = trainerReq.find(ability->Spell);
        if (trainer != trainerReq.end())
        {
            recipe.trainer = true;
            recipe.reqSkill = trainer->second.first;
            recipe.reqLevel = trainer->second.second;
        }
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (info->Reagent[r] > 0 && info->ReagentCount[r] > 0)
                recipe.reagents.emplace_back(uint32(info->Reagent[r]), info->ReagentCount[r]);
        if (recipe.reagents.empty())
            continue;

        if (ability->SkillLine == SKILL_MINING)
        {
            // smelting: the bar stands for its ores in the wanted list (gatherers mine ore, not bars)
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                if (info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[e].ItemType)
                {
                    float const made = float(std::max(1, info->Effects[e].CalcValue()));
                    auto& from = smeltFrom_[info->Effects[e].ItemType];
                    if (from.empty())
                        for (auto const& [item, n] : recipe.reagents)
                            from.emplace_back(item, float(n) / made);
                }
            continue;
        }
        if (!recipe.gray)
            continue;
        recipesBySkill_[ability->SkillLine].push_back(recipe);
        ++count;
    }
    LOG_INFO("playerbots", "[guild30] wanted list: {} craft recipes, {} smelts, {} vendor items", count,
             smeltFrom_.size(), vendorItems_.size());
}

void GuildCoordinator::LoadGatherSources()
{
    gatherSource_.clear();
    // herb / ore nodes: chest gameobjects whose lock needs the gathering skill
    if (QueryResult result = WorldDatabase.Query(
            "SELECT glt.Item, gt.Data0 FROM gameobject_loot_template glt JOIN gameobject_template gt ON gt.type = 3 "
            "AND gt.Data1 = glt.Entry WHERE glt.Reference = 0 AND gt.Data0 <> 0 GROUP BY glt.Item, gt.Data0"))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 const item = f[0].Get<uint32>();
            LockEntry const* lock = sLockStore.LookupEntry(f[1].Get<uint32>());
            if (!lock)
                continue;
            for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            {
                if (lock->Type[i] != LOCK_KEY_SKILL)
                    continue;
                uint32 const skill = lock->Index[i] == LOCKTYPE_MINING      ? SKILL_MINING
                                     : lock->Index[i] == LOCKTYPE_HERBALISM ? SKILL_HERBALISM
                                                                            : 0;
                if (!skill)
                    continue;
                auto itr = gatherSource_.find(item);
                if (itr == gatherSource_.end() || lock->Skill[i] < itr->second.second)
                    gatherSource_[item] = {skill, lock->Skill[i]};
            }
        } while (result->NextRow());
    }
    LOG_INFO("playerbots", "[guild30] wanted list: {} items from herb/ore nodes", gatherSource_.size());
}

// ---------------------------------------------------------------------------------------------------------------------
// helpers (lock_ held where they touch the state)
// ---------------------------------------------------------------------------------------------------------------------
GuildCoordinator::PlanRow const* GuildCoordinator::Row(uint32 low) const
{
    auto const itr = planIndex_.find(low);
    return itr == planIndex_.end() ? nullptr : &plan_[itr->second];
}

std::string GuildCoordinator::Who(uint32 low) const
{
    PlanRow const* row = Row(low);
    Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
    std::string name = p ? p->GetName() : (row ? row->name : std::to_string(low));
    return "#" + std::to_string(row ? row->num : 0) + " " + name;
}

void GuildCoordinator::Decision(std::string text)
{
    LOG_INFO("playerbots", "[guild] decision: {}", text);
    decisions_.emplace_front(Now(), std::move(text));
    while (decisions_.size() > 10)
        decisions_.pop_back();
}

void GuildCoordinator::Alert(std::string text)
{
    LOG_INFO("playerbots", "[alert] {}", text);
    alerts_.emplace_front(Now(), std::move(text));
    while (alerts_.size() > 20)
        alerts_.pop_back();
}

std::string GuildCoordinator::RaidLeaderText(char grp) const
{
    uint32 const low = raidLeader_[grp == 'B' ? 1 : 0];
    return low ? Who(low) : "-";
}

std::string GuildCoordinator::Chain(uint32 low) const
{
    PlanRow const* row = Row(low);
    std::string chain = "глоб.";
    if (!row || (row->grp != 'A' && row->grp != 'B'))
        return chain;
    uint32 const leader = raidLeader_[row->grp == 'B' ? 1 : 0];
    if (leader == low)
        return chain + " (рейд-лидер " + row->grp + ")";
    if (leader)
        chain += std::string(" → рейд ") + row->grp + " " + Who(leader);
    auto const st = bots_.find(low);
    if (st != bots_.end() && st->second.fiveId)
    {
        auto const five = fives_.find(st->second.fiveId);
        if (five != fives_.end())
            chain += " → пятёрка " + std::to_string(five->first) +
                     (five->second.leaderLow == low ? " (лидер)" : " " + Who(five->second.leaderLow));
    }
    return chain;
}

uint64 GuildCoordinator::XpTotal(Player* bot)
{
    uint64 xp = bot->GetUInt32Value(PLAYER_XP);
    for (uint8 level = 1; level < bot->GetLevel(); ++level)
        xp += sObjectMgr->GetXPForLevel(level);
    return xp;
}

uint32 GuildCoordinator::QuestProgress(Player* bot)
{
    uint32 progress = uint32(bot->getRewardedQuests().size()) * 1000;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        progress += 1;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            progress += bot->GetQuestSlotCounter(slot, i);
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                progress += std::min<uint32>(bot->GetItemCount(quest->RequiredItemId[i], false),
                                             quest->RequiredItemCount[i]);
        progress += bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE ? 100 : 0;
    }
    return progress;
}

std::string GuildCoordinator::StatusName(Player* bot, PlayerbotAI* botAI) const
{
    if (!botAI)
        return "играет человек";
    if (!bot->IsAlive())
        return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? "призрак" : "мёртв";
    std::string text = RpgName(uint32(botAI->rpgInfo.GetStatus()));
    if (bot->IsInCombat())
        text = "бой/" + text;
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
        text += " q" + std::to_string(quest->questId) + (quest->quest ? " " + quest->quest->GetTitle() : "");
    if (botAI->rpgInfo.vendorRunEntry)
        text += " торговец: " + botAI->rpgInfo.vendorRunReason;
    return text;
}

// the legitimate waits a bot is in by its own state
std::string GuildCoordinator::WaitReason(Player* bot, PlayerbotAI* botAI, BotState const& state) const
{
    if (!state.waitReason.empty())
        return state.waitReason;
    if (!botAI || IsRealPlayer(bot))
        return "human player";
    if (SelfBotControl::IsSuspended(bot))
        return "player controls";
    if (bot->IsInFlight())
        return "flight";
    if (bot->GetTransport())
        return "transport";
    if (bot->IsNonMeleeSpellCast(false))
        return "casting / crafting";
    if (bot->getStandState() == UNIT_STAND_STATE_SIT)
        return "resting (sits)";
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus const status = info.GetStatus();
    if (status == RPG_REST)
        return "resting";
    if (status == RPG_WANDER_NPC && info.vendorRunEntry)
        return "at vendor";
    if (info.trip.phase == NewRpgInfo::TripPhase::Wait || info.trip.phase == NewRpgInfo::TripPhase::Ride ||
        info.trip.phase == NewRpgInfo::TripPhase::Board)
        return "waiting for boat / zeppelin";
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
        if (quest->lastReachPOI)
            return "waiting for quest mob respawn q" + std::to_string(quest->questId);
    if (auto const* gather = std::get_if<NewRpgInfo::DoGather>(&info.data))
        if (gather->lastReach)
            return "at gather node";
    if (Group* group = bot->GetGroup())
        if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()))
            if (leader != bot && IsRealPlayer(leader))
                return "with the human group leader";
    return "";
}

// ---------------------------------------------------------------------------------------------------------------------
// world thread
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::Update(uint32 diff)
{
    if (!loaded_)
        return;

    uint32 const ms = getMSTime();
    ticks_.emplace_back(ms, diff);
    aiTicks_.emplace_back(ms, uint32(std::min<uint64>(aiTimeTotalUs_.exchange(0), UINT32_MAX)));
    while (!ticks_.empty() && getMSTimeDiff(ticks_.front().first, ms) > MINUTE * IN_MILLISECONDS)
        ticks_.pop_front();
    while (!aiTicks_.empty() && getMSTimeDiff(aiTicks_.front().first, ms) > MINUTE * IN_MILLISECONDS)
        aiTicks_.pop_front();

    ProcessDeaths();
    ProcessLoot();

    wantedTimer_ += diff;
    if (wantedTimer_ >= 15 * IN_MILLISECONDS)
    {
        wantedTimer_ = 0;
        UpdateWanted();
    }
    updateTimer_ += diff;
    if (updateTimer_ >= 5 * IN_MILLISECONDS)
    {
        updateTimer_ = 0;
        UpdateLeaders();
        UpdateOrders();
        UpdateHealth();
    }
}

void GuildCoordinator::UpdateLeaders()
{
    std::lock_guard<std::mutex> guard(lock_);
    for (uint8 g = 0; g < 2; ++g)
    {
        char const grp = g ? 'B' : 'A';
        uint32& current = raidLeader_[g];
        auto const usable = [&](uint32 low) -> Player*
        {
            Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
            if (!p || !p->IsInWorld() || !GET_PLAYERBOT_AI(p) || IsRealPlayer(p) || IsSelfBot(p))
                return nullptr;
            return p;
        };
        if (current && usable(current))
            continue;
        uint32 best = 0;
        Player* bestPlayer = nullptr;
        PlanRow const* bestRow = nullptr;
        for (PlanRow const& row : plan_)
        {
            if (row.grp != grp)
                continue;
            Player* p = usable(row.low);
            if (!p)
                continue;
            bool const tank = row.role == "tank";
            bool const bestTank = bestRow && bestRow->role == "tank";
            if (!bestRow || (tank && !bestTank) ||
                (tank == bestTank && (p->GetLevel() > bestPlayer->GetLevel() ||
                                      (p->GetLevel() == bestPlayer->GetLevel() && row.num < bestRow->num))))
            {
                best = row.low;
                bestPlayer = p;
                bestRow = &row;
            }
        }
        if (best != current)
        {
            std::string const why = current ? "прежний не в сети" : "первый выбор";
            current = best;
            if (best)
                Decision(std::string("рейд ") + grp + ": лидер " + Who(best) + " (" + bestRow->role + ", L" +
                         std::to_string(bestPlayer->GetLevel()) + ") — " +
                         (bestRow->role == "tank" ? "танк с наибольшим уровнем" : "танков нет, наибольший уровень") +
                         "; " + why);
        }
    }
    for (PlanRow const& row : plan_)
        if (row.grp == 'A' || row.grp == 'B')
            bots_[row.low].raidLeaderLow = raidLeader_[row.grp == 'B' ? 1 : 0];
}

// how much of each reagent the crafter needs from its skill to its rank cap ("the next tier"), orange/yellow only
uint32 GuildCoordinator::SkillUpNeeds(Player* crafter, uint32 skillLine, std::map<uint32, uint32>& needs) const
{
    uint32 const skill = crafter->GetPureSkillValue(skillLine);
    uint32 const cap = crafter->GetPureMaxSkillValue(skillLine);
    auto const list = recipesBySkill_.find(skillLine);
    if (!skill || skill >= cap || list == recipesBySkill_.end())
        return 0;

    std::map<size_t, double> crafts;
    for (uint32 value = skill; value < cap; ++value)
    {
        int32 best = -1;
        double bestCost = 0, bestUnits = 0;
        int32 bestChance = 0;
        for (size_t i = 0; i < list->second.size(); ++i)
        {
            Recipe const& r = list->second[i];
            bool const usable = crafter->HasSpell(r.spellId) ||
                                ((r.trainer || r.autoLearned) && r.reqSkill <= value && r.reqLevel <= crafter->GetLevel());
            if (!usable || value >= (r.yellow + r.gray) / 2)  // green or gray: not a skill-up recipe
                continue;
            int32 const chance = CraftChance(value, r.gray, r.yellow);
            if (chance <= 0)
                continue;
            double gathered = 0, units = 0;
            for (auto const& [item, n] : r.reagents)
            {
                units += n;
                gathered += vendorItems_.count(item) ? 0 : n;
            }
            double const expected = 1000.0 / chance;
            double const cost = expected * gathered, allUnits = expected * units;
            if (best < 0 || cost < bestCost || (cost == bestCost && allUnits < bestUnits))
            {
                best = int32(i);
                bestCost = cost;
                bestUnits = allUnits;
                bestChance = chance;
            }
        }
        if (best < 0)
            break;  // no recipe at this skill: the next one is a trainer / recipe step, not mats
        crafts[size_t(best)] += 1000.0 / bestChance;
    }
    for (auto const& [index, expected] : crafts)
    {
        uint32 const n = uint32(std::ceil(expected - 0.001));
        for (auto const& [item, count] : list->second[index].reagents)
        {
            auto const smelt = smeltFrom_.find(item);
            if (smelt != smeltFrom_.end() && !vendorItems_.count(item))
                for (auto const& [ore, perBar] : smelt->second)
                    needs[ore] += uint32(std::ceil(n * count * perBar - 0.001));
            else
                needs[item] += n * count;
        }
    }
    return cap;
}

void GuildCoordinator::UpdateWanted()
{
    std::map<uint32, WantedItem> wanted;
    std::vector<Player*> members;
    Guild* guild = nullptr;
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || !p->IsInWorld())
            continue;
        members.push_back(p);
        if (!guild)
            guild = GuildBankRun::BotGuild(p);

        for (uint32 line : CRAFT_LINES)
        {
            if (!p->HasSkill(line))
                continue;
            std::map<uint32, uint32> needs;
            uint32 const cap = SkillUpNeeds(p, line, needs);
            for (auto const& [item, n] : needs)
            {
                WantedItem& w = wanted[item];
                w.itemId = item;
                w.need += n;
                if (w.forWhom.size() < 60)
                    w.forWhom += (w.forWhom.empty() ? "" : ", ") + std::string("#") + std::to_string(row.num) + " " +
                                 SkillShort(line) + " " + std::to_string(p->GetPureSkillValue(line)) + "→" +
                                 std::to_string(cap);
            }
        }
        // items of the members' active quests that can change hands (no quest-bound items)
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = p->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest || p->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                uint32 const item = quest->RequiredItemId[i];
                ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item) : nullptr;
                if (!proto || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_WHEN_PICKED_UP ||
                    proto->Class == ITEM_CLASS_QUEST)
                    continue;
                uint32 const have = p->GetItemCount(item, false);
                if (have >= quest->RequiredItemCount[i])
                    continue;
                WantedItem& w = wanted[item];
                w.itemId = item;
                w.need += quest->RequiredItemCount[i];
                if (w.forWhom.size() < 60)
                    w.forWhom += (w.forWhom.empty() ? "" : ", ") + std::string("#") + std::to_string(row.num) +
                                 " q" + std::to_string(questId);
            }
        }
    }

    std::map<uint32, uint32> bank;
    GuildBankRun::CountBank(guild, bank);
    std::vector<WantedItem> list;
    for (auto& [item, w] : wanted)
    {
        for (Player* p : members)
            w.have += p->GetItemCount(item, false);
        w.have += bank[item];
        w.vendor = vendorItems_.count(item) != 0;
        auto const src = gatherSource_.find(item);
        if (src != gatherSource_.end())
        {
            w.gatherSkill = src->second.first;
            w.gatherReq = src->second.second;
        }
        list.push_back(w);
    }
    std::sort(list.begin(), list.end(), [](WantedItem const& a, WantedItem const& b)
              { return a.Short() != b.Short() ? a.Short() > b.Short() : a.itemId < b.itemId; });

    std::lock_guard<std::mutex> guard(lock_);
    wanted_ = std::move(list);
    guildState_ = guild ? "гильдия «" + guild->GetName() + "», вкладок банка " +
                              std::to_string(guild->GetPurchasedTabsSize())
                        : "гильдии нет: банк спит";
}

void GuildCoordinator::UpdateOrders()
{
    std::lock_guard<std::mutex> guard(lock_);
    std::unordered_map<uint32, uint32> gathering;  // item -> gatherers already sent
    uint32 const now = Now();
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        BotState& st = bots_[row.low];
        if (!p || !p->IsInWorld())
            continue;

        // items that left the bags (used, sold, traded, deposited) are not held any more
        auto const gone = [p](uint32 guidLow)
        { return !p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guidLow)); };
        st.held.erase(std::remove_if(st.held.begin(), st.held.end(),
                                     [&](HeldItem const& h) { return gone(h.itemGuidLow); }),
                      st.held.end());
        st.sell.erase(std::remove_if(st.sell.begin(), st.sell.end(), gone), st.sell.end());
        st.disenchant.erase(std::remove_if(st.disenchant.begin(), st.disenchant.end(), gone), st.disenchant.end());

        std::vector<GuildOrder> orders;
        if (!st.disenchant.empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::DISENCHANT;
            o.count = uint32(st.disenchant.size());
            o.details = std::to_string(o.count) + " предм.";
            o.reason = "никому не улучшение, зачарователь — сам";
            o.status = "распыляет";
            o.executable = true;
            orders.push_back(o);
        }
        std::vector<HeldItem> bankHeld;
        for (HeldItem const& h : st.held)
            if (h.purpose != "pending")
                bankHeld.push_back(h);
        if (!bankHeld.empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::BANK_DEPOSIT;
            o.itemId = bankHeld.front().itemId;
            o.count = uint32(bankHeld.size());
            o.forLow = bankHeld.front().forLow;
            o.details = std::to_string(o.count) + " предм.: " + ItemName(o.itemId) + " (" + bankHeld.front().purpose + ")";
            o.reason = "решение глоб. лидера по добыче";
            std::string const state = GuildBankRun::BankState(p, true);
            WorldPosition pos;
            float dist = 0;
            if (!state.empty())
                o.status = state + " — держит, квестует";
            else if (!st.vaultFail.empty())
                o.status = st.vaultFail;
            else if (!GuildBankRun::NearestVault(p, pos, dist))
                o.status = "нет хранилища на этом континенте";
            else
            {
                o.executable = true;
                o.status = "к хранилищу " + std::to_string(uint32(dist)) + " ярд";
            }
            orders.push_back(o);
        }
        auto const wd = withdraw_.find(row.low);
        if (wd != withdraw_.end() && !wd->second.empty())
        {
            GuildOrder o;
            o.type = GuildOrderType::BANK_WITHDRAW;
            o.itemId = wd->second.front().first;
            o.count = uint32(wd->second.size());
            o.details = ItemName(o.itemId) + " (" + wd->second.front().second + ")";
            o.reason = "в банке лежит улучшение для него";
            std::string const state = GuildBankRun::BankState(p, false);
            WorldPosition pos;
            float dist = 0;
            if (!state.empty())
                o.status = state;
            else if (!GuildBankRun::NearestVault(p, pos, dist))
                o.status = "нет хранилища на этом континенте";
            else
            {
                o.executable = true;
                o.status = "к хранилищу " + std::to_string(uint32(dist)) + " ярд";
            }
            orders.push_back(o);
        }
        // gatherers (group B): the biggest shortfall they can gather with their skill
        if (row.grp == 'B' && GET_PLAYERBOT_AI(p))
        {
            WantedItem const* pick = nullptr;
            for (WantedItem const& w : wanted_)
            {
                if (!w.Short() || !w.gatherSkill || !p->HasSkill(w.gatherSkill) ||
                    p->GetPureSkillValue(w.gatherSkill) < w.gatherReq)
                    continue;
                if (!pick || w.Short() > pick->Short() ||
                    (w.Short() == pick->Short() && gathering[w.itemId] < gathering[pick->itemId]))
                    pick = &w;
            }
            if (pick)
            {
                ++gathering[pick->itemId];
                GuildOrder o;
                o.type = GuildOrderType::GATHER;
                o.itemId = pick->itemId;
                o.count = pick->Short();
                o.details = ItemName(pick->itemId) + " ×" + std::to_string(pick->Short()) + " (" +
                            std::to_string(pick->have) + "/" + std::to_string(pick->need) + ") для " + pick->forWhom;
                o.reason = "наибольшая нехватка в wanted list, " + std::string(SkillShort(pick->gatherSkill)) + " " +
                           std::to_string(p->GetPureSkillValue(pick->gatherSkill)) + " ≥ " +
                           std::to_string(pick->gatherReq);
                o.status = "собирает (в сумках " + std::to_string(p->GetItemCount(pick->itemId, false)) + ")";
                o.executable = true;
                orders.push_back(o);
            }
        }
        for (GuildOrder const& ext : st.external)
            orders.push_back(ext);
        GuildOrder quest;
        quest.details = "сам себе лидер";
        quest.reason = "нет более полезного дела";
        orders.push_back(quest);

        // keep the since-time of an order that stays the same; log a decision when the first order changes
        for (GuildOrder& o : orders)
        {
            o.since = now;
            for (GuildOrder const& old : st.orders)
                if (old.type == o.type && old.itemId == o.itemId && old.forLow == o.forLow)
                {
                    o.since = old.since;
                    if (o.type == GuildOrderType::GATHER && old.status.rfind("нет ", 0) == 0)
                        o.status = old.status;  // set by New RPG (no nodes here)
                }
        }
        bool const changed = st.orders.empty() || st.orders.front().type != orders.front().type ||
                             st.orders.front().itemId != orders.front().itemId;
        if (changed && orders.front().type != GuildOrderType::QUEST)
            Decision(Who(row.low) + " → " + OrderName(orders.front().type) + ": " + orders.front().details + " — " +
                     orders.front().reason);
        st.orders = std::move(orders);
    }
}

void GuildCoordinator::SetOrder(uint32 low, GuildOrder order, std::string const& decision)
{
    std::lock_guard<std::mutex> guard(lock_);
    BotState& st = bots_[low];
    order.since = Now();
    if (order.status.empty())
        order.status = "этап 2: пока не выполняется";
    st.external.erase(std::remove_if(st.external.begin(), st.external.end(),
                                     [&](GuildOrder const& o) { return o.type == order.type; }),
                      st.external.end());
    if (order.type != GuildOrderType::QUEST)
        st.external.push_back(order);
    Decision(Who(low) + " → " + OrderName(order.type) + ": " + decision);
}

uint32 GuildCoordinator::FormFive(uint32 leaderLow, std::vector<uint32> const& members, std::string const& dungeon)
{
    std::lock_guard<std::mutex> guard(lock_);
    GuildFive five;
    five.id = nextFiveId_++;
    five.leaderLow = leaderLow;
    five.members = members;
    five.dungeon = dungeon;
    five.since = Now();
    for (uint32 low : members)
        bots_[low].fiveId = five.id;
    bots_[leaderLow].fiveId = five.id;
    fives_[five.id] = five;
    Decision("пятёрка " + std::to_string(five.id) + " → " + dungeon + ", лидер " + Who(leaderLow));
    return five.id;
}

void GuildCoordinator::DisbandFive(uint32 fiveId)
{
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = fives_.find(fiveId);
    if (itr == fives_.end())
        return;
    for (uint32 low : itr->second.members)
        bots_[low].fiveId = 0;
    bots_[itr->second.leaderLow].fiveId = 0;
    Decision("пятёрка " + std::to_string(fiveId) + " вернула полномочия глоб. лидеру");
    fives_.erase(itr);
}

// ---------------------------------------------------------------------------------------------------------------------
// loot flow
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::OnLoot(Player* player, Item* item, uint32 count)
{
    if (!player || !item || !IsPlanned(player->GetGUID().GetCounter()))
        return;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->Quality == ITEM_QUALITY_POOR || proto->Class == ITEM_CLASS_QUEST ||
        proto->Bonding == BIND_QUEST_ITEM || item->IsSoulBound())
        return;
    bool const equipment = (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
                           proto->Bonding == BIND_WHEN_EQUIPPED;
    bool const material = proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_REAGENT ||
                          proto->Class == ITEM_CLASS_GEM;
    if (!equipment && !material)
        return;
    uint32 const low = player->GetGUID().GetCounter();
    std::lock_guard<std::mutex> guard(lock_);
    BotState& st = bots_[low];
    // held until the global leader decides: no auto-equip, no vendor sale meanwhile
    bool known = false;
    for (HeldItem const& h : st.held)
        known |= h.itemGuidLow == item->GetGUID().GetCounter();
    if (!known)
        st.held.push_back({item->GetGUID().GetCounter(), item->GetEntry(), count, 0, "pending"});
    lootQueue_.push_back({low, item->GetGUID().GetCounter(), item->GetEntry(), count});
}

void GuildCoordinator::ProcessLoot()
{
    std::vector<LootReport> reports;
    {
        std::lock_guard<std::mutex> guard(lock_);
        reports.swap(lootQueue_);
    }
    for (LootReport const& report : reports)
    {
        Player* looter = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(report.playerLow));
        Item* item = looter ? looter->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(report.itemGuidLow)) : nullptr;
        if (!item)
            continue;
        DecideLoot(looter, item, report);
    }
}

void GuildCoordinator::DecideLoot(Player* looter, Item* item, LootReport const& report)
{
    ItemTemplate const* proto = item->GetTemplate();
    uint32 const looterLow = looter->GetGUID().GetCounter();
    std::string const what = ItemName(report.itemId) + (report.count > 1 ? " ×" + std::to_string(report.count) : "") +
                             " (у " + Who(looterLow) + ")";
    bool const equipment = proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON;

    // 1. BoE: whom of the members it upgrades most (ItemUsage of the bot; score gain vs its slot)
    uint32 bestLow = 0;
    float bestGain = 0.0f;
    std::string bestUsage;
    if (equipment)
    {
        std::string const qualifier =
            std::to_string(report.itemId) +
            (item->GetItemRandomPropertyId() ? "," + std::to_string(item->GetItemRandomPropertyId()) : "");
        for (PlanRow const& row : plan_)
        {
            Player* m = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
            if (!m || !m->IsInWorld() || m->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(m);
            if (!ai)
                continue;  // a human picks his own gear
            ItemUsage const usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", qualifier)->Get();
            if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
                continue;
            StatsWeightCalculator calc(m);
            float const score = calc.CalculateItem(report.itemId, item->GetItemRandomPropertyId());
            float current = 0.0f;
            uint8 const slot = m->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot != NULL_SLOT)
                if (Item* worn = m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    calc.Reset();
                    current = calc.CalculateItem(worn->GetEntry(), worn->GetItemRandomPropertyId());
                }
            float const gain = current > 0.0f ? (score - current) / current : 1.0f;
            if (gain > bestGain)
            {
                bestGain = gain;
                bestLow = row.low;
                bestUsage = usage == ITEM_USAGE_EQUIP ? "пустой слот" : "замена";
            }
        }
    }

    std::lock_guard<std::mutex> guard(lock_);
    BotState& st = bots_[looterLow];
    auto held = std::find_if(st.held.begin(), st.held.end(),
                             [&](HeldItem const& h) { return h.itemGuidLow == report.itemGuidLow; });
    auto const release = [&]()
    {
        if (held != st.held.end())
            st.held.erase(held);
    };
    std::string const gainText = "+" + std::to_string(int32(bestGain * 100.0f + 0.5f)) + "% к слоту";

    if (bestLow == looterLow)
    {
        release();
        Decision("BoE " + what + " → надевает сам: " + gainText + " (" + bestUsage + ")");
        return;
    }
    if (bestLow)
    {
        if (held != st.held.end())
        {
            held->forLow = bestLow;
            held->purpose = "улучшение " + Who(bestLow);
        }
        Decision("BoE " + what + " → " + Who(bestLow) + ": " + gainText + " (" + bestUsage +
                 "), через банк гильдии");
        return;
    }

    // 2. wanted-list material: to the guild bank (held until it exists)
    for (WantedItem const& w : wanted_)
        if (w.itemId == report.itemId && w.need)
        {
            if (held != st.held.end())
                held->purpose = "wanted";
            Decision(what + " → банк: в wanted list (" + std::to_string(w.have) + "/" + std::to_string(w.need) +
                     " для " + w.forWhom + ")");
            return;
        }

    // the looter uses it itself (its profession, a spell, a quest): no decision needed
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(looter))
    {
        ItemUsage const usage =
            ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", std::to_string(report.itemId))->Get();
        if (usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_USE || usage == ITEM_USAGE_QUEST ||
            usage == ITEM_USAGE_KEEP || usage == ITEM_USAGE_AMMO || usage == ITEM_USAGE_GUILD_TASK ||
            usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
        {
            release();
            return;
        }
    }
    else
    {
        release();
        return;  // a human decides himself
    }

    // 3. nobody's: an enchanter disenchants it, else it is sold
    if (equipment && proto->DisenchantID && proto->Quality >= ITEM_QUALITY_UNCOMMON)
    {
        uint32 const req = proto->RequiredDisenchantSkill;
        if (looter->HasSkill(SKILL_ENCHANTING) && looter->GetPureSkillValue(SKILL_ENCHANTING) >= req &&
            looter->HasSpell(SPELL_DISENCHANT))
        {
            release();
            st.disenchant.push_back(report.itemGuidLow);
            Decision(what + " → распылит сам (зачарователь, навык " +
                     std::to_string(looter->GetPureSkillValue(SKILL_ENCHANTING)) + ")");
            return;
        }
        uint32 enchanter = 0;
        for (PlanRow const& row : plan_)
            if (Player* m = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low)))
                if (m->HasSkill(SKILL_ENCHANTING) && m->GetPureSkillValue(SKILL_ENCHANTING) >= req)
                {
                    enchanter = row.low;
                    break;
                }
        if (enchanter && GuildBankRun::BankState(looter, true).empty())
        {
            if (held != st.held.end())
            {
                held->forLow = enchanter;
                held->purpose = "распылить " + Who(enchanter);
            }
            Decision(what + " → банк для зачарователя " + Who(enchanter));
            return;
        }
        release();
        st.sell.push_back(report.itemGuidLow);
        Decision(what + " → продать: никому не улучшение, " +
                 (enchanter ? "зачарователь " + Who(enchanter) + " есть, банка нет" : std::string("зачарователя нет")));
        return;
    }
    release();
    st.sell.push_back(report.itemGuidLow);
    Decision(what + " → продать: не улучшение и не в wanted list");
}

// ---------------------------------------------------------------------------------------------------------------------
// map thread: New RPG runs the order
// ---------------------------------------------------------------------------------------------------------------------
GuildRunStep GuildCoordinator::NextStep(Player* bot, PlayerbotAI* botAI)
{
    GuildRunStep step;
    if (!bot || !botAI || !IsPlanned(bot->GetGUID().GetCounter()))
        return step;
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return step;
    BotState& st = itr->second;
    for (GuildOrder& order : st.orders)
    {
        if (!order.executable)
            continue;
        switch (order.type)
        {
            case GuildOrderType::DISENCHANT:
            {
                while (!st.disenchant.empty())
                {
                    Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(st.disenchant.back()));
                    st.disenchant.pop_back();
                    if (item && !bot->IsNonMeleeSpellCast(false) && botAI->CastSpell(SPELL_DISENCHANT, bot, item))
                    {
                        step.kind = GuildRunStep::BUSY;
                        step.reason = "disenchant " + item->GetTemplate()->Name1;
                        return step;
                    }
                }
                continue;
            }
            case GuildOrderType::BANK_DEPOSIT:
            case GuildOrderType::BANK_WITHDRAW:
            {
                WorldPosition pos;
                float dist = 0;
                if (!GuildBankRun::NearestVault(bot, pos, dist))
                    continue;
                st.run = true;
                st.runType = order.type;
                st.runPos = pos;
                st.runApproached = false;
                step.kind = GuildRunStep::WALK;
                step.pos = pos;
                step.reason = std::string(OrderName(order.type)) + " " + order.details + ", vault " +
                              std::to_string(uint32(dist)) + " yd";
                return step;
            }
            case GuildOrderType::GATHER:
                step.kind = GuildRunStep::GATHER;
                step.reason = "GATHER " + order.details;
                return step;
            default:
                continue;
        }
    }
    return step;
}

uint8 GuildCoordinator::OnCampReached(Player* bot)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return 0;
    uint32 const low = bot->GetGUID().GetCounter();
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(low);
    if (itr == bots_.end() || !itr->second.run)
        return 0;
    BotState& st = itr->second;
    if (!GuildBankRun::AtVault(bot, st.runPos))
    {
        if (!st.runApproached)
        {
            st.runApproached = true;
            bot->GetMotionMaster()->MovePoint(0, st.runPos.GetPositionX(), st.runPos.GetPositionY(),
                                              st.runPos.GetPositionZ());
            return 2;
        }
        if (bot->isMoving())
            return 2;
        st.run = false;
        st.vaultFail = "не дотянуться до хранилища";
        Decision(Who(low) + ": " + st.vaultFail + " — квестует дальше");
        return 1;
    }
    st.run = false;
    if (st.runType == GuildOrderType::BANK_DEPOSIT)
    {
        std::vector<uint32> guids;
        std::vector<HeldItem> moving;
        for (HeldItem const& h : st.held)
            if (h.purpose != "pending")
            {
                guids.push_back(h.itemGuidLow);
                moving.push_back(h);
            }
        uint32 const moved = GuildBankRun::Deposit(bot, guids);
        for (HeldItem const& h : moving)
        {
            if (bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(h.itemGuidLow)))
                continue;  // still in the bags: not deposited
            if (h.forLow)
                withdraw_[h.forLow].emplace_back(h.itemId, "от " + Who(low) + ": " + h.purpose);
            st.held.erase(std::remove_if(st.held.begin(), st.held.end(),
                                         [&](HeldItem const& x) { return x.itemGuidLow == h.itemGuidLow; }),
                          st.held.end());
        }
        Decision(Who(low) + " положил в банк гильдии " + std::to_string(moved) + " из " +
                 std::to_string(guids.size()) + " предм.");
    }
    else if (st.runType == GuildOrderType::BANK_WITHDRAW)
    {
        auto& list = withdraw_[low];
        if (!list.empty())
        {
            uint32 const itemId = list.front().first;
            bool const ok = GuildBankRun::Withdraw(bot, itemId);
            list.erase(list.begin());
            Decision(Who(low) + (ok ? " взял из банка " : " не смог взять из банка ") + ItemName(itemId));
        }
    }
    return 1;
}

bool GuildCoordinator::IsHeld(Player* bot, Item* item)
{
    if (!bot || !item || !IsPlanned(bot->GetGUID().GetCounter()))
        return false;
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return false;
    for (HeldItem const& h : itr->second.held)
        if (h.itemGuidLow == item->GetGUID().GetCounter())
            return true;
    return false;
}

void GuildCoordinator::SellMarked(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    std::vector<uint32> sell;
    {
        std::lock_guard<std::mutex> guard(lock_);
        auto const itr = bots_.find(bot->GetGUID().GetCounter());
        if (itr == bots_.end())
            return;
        sell.swap(itr->second.sell);
    }
    if (sell.empty())
        return;
    SellAction action(botAI);
    uint32 sold = 0, gone = 0, kept = 0;
    uint32 const before = bot->GetMoney();
    for (uint32 low : sell)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low));
        if (!item)
        {
            ++gone;  // the vendor sale of the bot itself or a use came first
            continue;
        }
        action.Sell(item);
        if (bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low)))
            ++kept;  // SellAction keeps what a quest or a spell still needs
        else
            ++sold;
    }
    LOG_INFO("playerbots", "[guild30] {} coordinator sales: sold {}, already gone {}, kept by SellAction {} (+{}c)",
             bot->GetName(), sold, gone, kept, bot->GetMoney() - before);
}

void GuildCoordinator::SetOrderStatus(Player* bot, std::string const& status)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    std::lock_guard<std::mutex> guard(lock_);
    auto const itr = bots_.find(bot->GetGUID().GetCounter());
    if (itr == bots_.end())
        return;
    for (GuildOrder& order : itr->second.orders)
        if (order.executable)
        {
            order.status = status;
            return;
        }
}

void GuildCoordinator::SetWait(Player* bot, std::string const& reason)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    std::lock_guard<std::mutex> guard(lock_);
    bots_[bot->GetGUID().GetCounter()].waitReason = reason;
}

void GuildCoordinator::AddAiTime(Player* bot, uint64 microseconds)
{
    aiTimeTotalUs_ += microseconds;
    auto const itr = planIndex_.find(bot->GetGUID().GetCounter());
    if (itr != planIndex_.end() && itr->second < aiCount_)
        aiTimeUs_[itr->second] += microseconds;
}

// ---------------------------------------------------------------------------------------------------------------------
// health monitor
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::OnKilledByCreature(Player* killed, Creature* killer)
{
    if (!killed || !killer || !IsPlanned(killed->GetGUID().GetCounter()))
        return;
    DeathReport report;
    report.playerLow = killed->GetGUID().GetCounter();
    report.creature = true;
    report.killer = killer->GetName() + " L" + std::to_string(killer->GetLevel()) + " (entry " +
                    std::to_string(killer->GetEntry()) + ")";
    report.cause = ConText(killed, killer);
    std::lock_guard<std::mutex> guard(lock_);
    deathQueue_.push_back(report);
}

void GuildCoordinator::OnJustDied(Player* killed)
{
    if (!killed || !IsPlanned(killed->GetGUID().GetCounter()))
        return;
    DeathReport report;
    report.playerLow = killed->GetGUID().GetCounter();
    report.cause = killed->IsInWater() ? "water / environment" : "environment / not a creature";
    report.killer = "-";
    std::lock_guard<std::mutex> guard(lock_);
    deathQueue_.push_back(report);
}

void GuildCoordinator::ProcessDeaths()
{
    // One death = one alert. The creature kill and the death itself come from the same Unit::Kill, possibly in two
    // world updates: a death without a creature report waits one update for it; a bot's death stays open (no second
    // alert) until it is seen alive again.
    ++deathCall_;
    std::vector<DeathReport> reports;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (deathQueue_.empty() && envPending_.empty())
            return;
        reports.swap(deathQueue_);
    }
    std::map<uint32, DeathReport> byBot;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (DeathReport const& r : reports)
        {
            if (r.creature)
            {
                byBot[r.playerLow] = r;
                envPending_.erase(std::remove_if(envPending_.begin(), envPending_.end(),
                                                 [&](auto const& e) { return e.second.playerLow == r.playerLow; }),
                                  envPending_.end());
            }
            else if (!byBot.count(r.playerLow))
                envPending_.emplace_back(deathCall_, r);
        }
        for (auto itr = envPending_.begin(); itr != envPending_.end();)
        {
            if (itr->first < deathCall_)
            {
                if (!byBot.count(itr->second.playerLow))
                    byBot[itr->second.playerLow] = itr->second;
                itr = envPending_.erase(itr);
            }
            else
                ++itr;
        }
        for (auto itr = byBot.begin(); itr != byBot.end();)
        {
            BotState& st = bots_[itr->first];
            if (st.deathOpen)
                itr = byBot.erase(itr);
            else
            {
                st.deathOpen = true;
                ++itr;
            }
        }
    }
    if (byBot.empty())
        return;
    uint32 const now = Now();
    std::lock_guard<std::mutex> guard(lock_);
    for (auto const& [low, r] : byBot)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        BotState& st = bots_[low];
        st.deaths.push_back(now);
        while (!st.deaths.empty() && now - st.deaths.front() > HOUR)
            st.deaths.pop_front();
        ++deathsTotal_;
        PlanRow const* row = Row(low);
        AreaTableEntry const* zone = p ? sAreaTableStore.LookupEntry(p->GetZoneId()) : nullptr;
        std::ostringstream out;
        out << "DEATH #" << (row ? row->num : 0) << " " << (p ? p->GetName() : row ? row->name : "?") << " L"
            << (p ? p->GetLevel() : 0) << " killed by " << r.killer << " cause=" << r.cause;
        if (p)
            out << " at map " << p->GetMapId() << " (" << int32(p->GetPositionX()) << "," << int32(p->GetPositionY())
                << ") " << (zone ? zone->area_name[0] : "?") << " status=" << StatusName(p, GET_PLAYERBOT_AI(p))
                << " order=" << (st.orders.empty() ? "QUEST" : OrderName(st.orders.front().type));
        Alert(out.str());
        if (st.deaths.size() >= 2)
            Alert("DEATHS #" + std::to_string(row ? row->num : 0) + " " + (p ? p->GetName() : "?") + ": " +
                  std::to_string(st.deaths.size()) + " deaths in the last hour, last killer " + r.killer);
    }
}

void GuildCoordinator::OnRpgStatus(Player* bot, uint8 status)
{
    if (!bot || !IsPlanned(bot->GetGUID().GetCounter()))
        return;
    uint64 const xp = XpTotal(bot);
    uint32 const progress = QuestProgress(bot);
    std::lock_guard<std::mutex> guard(lock_);
    BotState& st = bots_[bot->GetGUID().GetCounter()];
    auto const itr = st.statusEntry.find(status);
    bool noProgress = false;
    if (itr != st.statusEntry.end())
    {
        auto const& [oldXp, oldProgress, x, y, z, map] = itr->second;
        noProgress = oldXp == xp && oldProgress == progress && map == bot->GetMapId() &&
                     bot->GetExactDist(x, y, z) <= std::max(bot->GetCombatReach(), 1.5f);
    }
    if (noProgress && !st.loopAlerted.count(status))
    {
        st.loopAlerted.insert(status);
        PlanRow const* row = Row(bot->GetGUID().GetCounter());
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        Alert("LOOP #" + std::to_string(row ? row->num : 0) + " " + bot->GetName() + " L" +
              std::to_string(bot->GetLevel()) + ": " + RpgName(status) +
              " re-entered without progress (no XP, no quest progress, same place) at map " +
              std::to_string(bot->GetMapId()) + " (" + std::to_string(int32(bot->GetPositionX())) + "," +
              std::to_string(int32(bot->GetPositionY())) + "), last action=" +
              LastAction(ai));
    }
    else if (!noProgress)
        st.loopAlerted.erase(status);
    st.statusEntry[status] = {xp, progress, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                              bot->GetMapId()};
}

void GuildCoordinator::UpdateHealth()
{
    uint32 const now = Now();
    // tick time over the last minute
    std::vector<uint32> diffs;
    diffs.reserve(ticks_.size());
    uint64 sum = 0;
    for (auto const& t : ticks_)
    {
        diffs.push_back(t.second);
        sum += t.second;
    }
    std::sort(diffs.begin(), diffs.end());
    uint32 const avg = diffs.empty() ? 0 : uint32(sum / diffs.size());
    uint32 const p95 = diffs.empty() ? 0 : diffs[std::min(diffs.size() - 1, diffs.size() * 95 / 100)];
    uint32 const maxDiff = diffs.empty() ? 0 : diffs.back();
    uint64 aiSum = 0, aiMax = 0;
    for (auto const& t : aiTicks_)
    {
        aiSum += t.second;
        aiMax = std::max<uint64>(aiMax, t.second);
    }
    float const aiAvgMs = aiTicks_.empty() ? 0.0f : float(aiSum) / aiTicks_.size() / 1000.0f;
    int32 const target = sConfigMgr->GetOption<int32>("MinRecordUpdateTimeDiff", 100);

    // per-bot AI time of the last interval
    std::vector<std::pair<uint64, uint32>> aiBots;
    for (size_t i = 0; i < aiCount_; ++i)
        if (uint64 const us = aiTimeUs_[i].exchange(0))
            aiBots.emplace_back(us, plan_[i].low);
    std::sort(aiBots.begin(), aiBots.end(), std::greater<>());

    std::lock_guard<std::mutex> guard(lock_);
    uint32 online = 0, dead = 0, stuck = 0;
    RateSample sample;
    sample.at = now;
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        BotState& st = bots_[row.low];
        if (!p || !p->IsInWorld())
        {
            st.seen = false;
            st.anchorAt = 0;
            continue;
        }
        ++online;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(p);

        // per-hour totals from the deltas of characters online at both samples
        uint64 const xp = XpTotal(p);
        uint32 const quests = uint32(p->getRewardedQuests().size());
        uint32 const money = p->GetMoney();
        if (st.seen)
        {
            sample.xp += xp > st.xpTotal ? xp - st.xpTotal : 0;
            sample.quests += quests > st.questsDone ? quests - st.questsDone : 0;
            sample.money += int64(money) - int64(st.money);
        }
        st.seen = true;
        st.xpTotal = xp;
        st.questsDone = quests;
        st.money = money;

        if (p->IsAlive())
            st.deathOpen = false;
        if (!p->IsAlive())
        {
            ++dead;
            st.anchorAt = 0;
            st.stuckAlerted = false;
            continue;
        }
        // stuck: out of combat, alive, not moved beyond its combat reach for 60 s, no legitimate wait
        float const reach = std::max(p->GetCombatReach(), 1.5f);
        if (p->IsInCombat() || !st.anchorAt || st.mapId != p->GetMapId() ||
            p->GetExactDist(st.ax, st.ay, st.az) > reach)
        {
            st.mapId = p->GetMapId();
            st.ax = p->GetPositionX();
            st.ay = p->GetPositionY();
            st.az = p->GetPositionZ();
            st.anchorAt = now;
            st.stuckAlerted = false;
            continue;
        }
        if (now - st.anchorAt < STUCK_SECONDS)
            continue;
        std::string const wait = WaitReason(p, ai, st);
        if (!wait.empty())
            continue;
        ++stuck;
        std::string const status = StatusName(p, ai);
        std::string const order = st.orders.empty() ? "QUEST" : OrderName(st.orders.front().type);
        std::string const key = status + "|" + order;
        if (st.stuckAlerted && st.stuckKey == key)
            continue;
        st.stuckAlerted = true;
        st.stuckKey = key;
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(p->GetZoneId());
        std::ostringstream out;
        out << "STUCK #" << row.num << " " << p->GetName() << " L" << uint32(p->GetLevel()) << " at map "
            << p->GetMapId() << " (" << int32(p->GetPositionX()) << "," << int32(p->GetPositionY()) << ") "
            << (zone ? zone->area_name[0] : "?") << ": " << (now - st.anchorAt) << " s, status=" << status
            << ", order=" << order << ", last action="
            << LastAction(ai)
            << ", reason=none";
        Alert(out.str());
    }
    sample.deaths = deathsTotal_;
    deathsTotal_ = 0;
    rates_.push_back(sample);
    while (!rates_.empty() && now - rates_.front().at > HOUR)
        rates_.pop_front();
    uint64 xpH = 0;
    uint32 questsH = 0, deathsH = 0;
    int64 moneyH = 0;
    for (RateSample const& r : rates_)
    {
        xpH += r.xp;
        questsH += r.quests;
        deathsH += r.deaths;
        moneyH += r.money;
    }
    uint32 const window = rates_.size() > 1 ? now - rates_.front().at : 0;
    auto const perHour = [window](double v) { return window ? v * HOUR / window : v; };

    char buf[400];
    snprintf(buf, sizeof(buf),
             "тик ср %u / p95 %u / макс %u мс (цель %d); AI ср %.2f / макс %.1f мс/тик; онлайн %u/%u, мёртвых %u, "
             "застряли %u; за час: квесты %.0f, опыт %.0f, смерти %.0f, золото %.1f (окно %u мин)",
             avg, p95, maxDiff, target, aiAvgMs, aiMax / 1000.0f, online, uint32(plan_.size()), dead, stuck,
             perHour(questsH), perHour(double(xpH)), perHour(deathsH), perHour(double(moneyH)) / GOLD, window / MINUTE);
    healthRow_ = buf;
    if (now - lastHealthLog_ >= MINUTE)
    {
        lastHealthLog_ = now;
        LOG_INFO("playerbots", "[health] {}", healthRow_);
    }

    // efficiency budget: the server's own slow-tick threshold (MinRecordUpdateTimeDiff)
    if (target > 0 && p95 > uint32(target))
    {
        if (!perfAlerted_)
        {
            perfAlerted_ = true;
            std::ostringstream out;
            out << "PERF p95 tick " << p95 << " ms > " << target << " ms (MinRecordUpdateTimeDiff); avg " << avg
                << " max " << maxDiff << "; bot AI avg " << int32(aiAvgMs * 10) / 10.0f << " ms/tick; top AI 5 s:";
            for (size_t i = 0; i < aiBots.size() && i < 5; ++i)
                out << " " << Who(aiBots[i].second) << " " << aiBots[i].first / 1000 << " ms";
            if (PerfMonitor::IsEnabled())
            {
                out << "; pmon:";
                for (auto const& [name, us] : sPerfMonitor.TopTotals(5))
                    out << " " << name.substr(0, 40) << " " << us / 1000 << " ms";
            }
            else
                out << "; pmon off";
            Alert(out.str());
        }
    }
    else
        perfAlerted_ = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// the BotControl «Приказы» tab
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::SendOrders(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;
    uint32 const now = Now();
    std::lock_guard<std::mutex> guard(lock_);
    SendLine(player, "OS;n=" + std::to_string(plan_.size()));
    SendLine(player, "OR;" + OrdersText(RaidLeaderText('A'), 40) + ";" + OrdersText(RaidLeaderText('B'), 40) + ";" +
                         OrdersText(guildState_.empty() ? "гильдии нет: банк спит" : guildState_, 80));
    for (auto const& [at, text] : decisions_)
        SendLine(player, "OD;" + std::to_string(now - at) + ";" + OrdersText(text, 200));
    size_t shown = 0;
    for (WantedItem const& w : wanted_)
    {
        if (!w.Short() || ++shown > 10)
            continue;
        std::string const source = w.gatherSkill ? std::string(SkillShort(w.gatherSkill)) + " " +
                                                       std::to_string(w.gatherReq)
                                   : w.vendor ? "торговец"
                                              : "добыча";
        SendLine(player, "OW;" + OrdersText(ItemName(w.itemId), 30) + ";" + std::to_string(w.Short()) + ";" +
                             std::to_string(w.have) + "/" + std::to_string(w.need) + ";" + source + ";" +
                             OrdersText(w.forWhom, 70));
    }
    SendLine(player, "OH;" + OrdersText(healthRow_, 220));
    for (auto const& [at, text] : alerts_)
        SendLine(player, "OA;" + std::to_string(now - at) + ";" + OrdersText(text, 200));
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        auto const st = bots_.find(row.low);
        GuildOrder order;
        if (st != bots_.end() && !st->second.orders.empty())
            order = st->second.orders.front();
        std::string status = order.status;
        std::string details = order.details;
        if (!p || !p->IsInWorld())
            status = "не в сети";
        else if (order.type == GuildOrderType::QUEST)
            status = StatusName(p, GET_PLAYERBOT_AI(p));
        if (st != bots_.end() && order.type != GuildOrderType::BANK_DEPOSIT)
        {
            uint32 hold = 0;
            for (HeldItem const& h : st->second.held)
                hold += h.purpose != "pending" ? 1 : 0;
            if (hold)
                details += "; держит для банка: " + std::to_string(hold);
        }
        uint32 const sinceMin = order.since ? (now - order.since) / MINUTE : 0;
        SendLine(player, "O;" + std::to_string(row.num) + ";" + OrdersText(p ? p->GetName() : row.name, 12) + ";" +
                             OrdersText(Chain(row.low), 60) + ";" + OrderName(order.type) + ";" + OrdersText(details, 70) + ";" +
                             std::to_string(sinceMin) + ";" + OrdersText(status, 60));
    }
    SendLine(player, "OE;n=" + std::to_string(plan_.size()));
}

// ---------------------------------------------------------------------------------------------------------------------
// the BotControl «Ресурсы» tab: the virtual guild bank (all planned characters' bags + bank slots + the guild bank),
// built on request from the players in memory
// ---------------------------------------------------------------------------------------------------------------------
void GuildCoordinator::SendResources(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;

    std::unordered_map<uint32, uint32> wantedNeed;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (WantedItem const& w : wanted_)
            if (w.need)
                wantedNeed[w.itemId] = w.need;
    }
    auto const useful = [&](ItemTemplate const* proto)
    {
        if (!proto || proto->Quality == ITEM_QUALITY_POOR || proto->Class == ITEM_CLASS_QUEST ||
            proto->Bonding == BIND_QUEST_ITEM)
            return false;
        if (wantedNeed.count(proto->ItemId))
            return true;
        return proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_REAGENT ||
               proto->Class == ITEM_CLASS_RECIPE || proto->Class == ITEM_CLASS_GEM;
    };

    struct Stock
    {
        uint32 total{0};
        std::vector<std::pair<uint32, uint32>> holders;  // (num, count), num 0 = the guild bank
    };
    std::map<uint32, Stock> stock;
    auto const add = [&](Item* item, std::map<uint32, uint32>& mine)
    {
        if (item && useful(item->GetTemplate()))
            mine[item->GetEntry()] += item->GetCount();
    };
    Guild* guild = nullptr;
    for (PlanRow const& row : plan_)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(row.low));
        if (!p || !p->IsInWorld())
            continue;
        if (!guild)
            guild = GuildBankRun::BotGuild(p);
        std::map<uint32, uint32> mine;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), mine);
        for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
            add(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), mine);
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    add(bag->GetItemByPos(uint8(j)), mine);
        for (uint8 b = BANK_SLOT_BAG_START; b < BANK_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    add(bag->GetItemByPos(uint8(j)), mine);
        for (auto const& [entry, count] : mine)
        {
            Stock& s = stock[entry];
            s.total += count;
            s.holders.emplace_back(row.num, count);
        }
    }
    std::map<uint32, uint32> bank;
    GuildBankRun::CountBank(guild, bank);
    for (auto const& [entry, count] : bank)
        if (useful(sObjectMgr->GetItemTemplate(entry)))
        {
            Stock& s = stock[entry];
            s.total += count;
            s.holders.emplace_back(0, count);
        }

    SendLine(player, "RS;n=" + std::to_string(stock.size()) + ";bank=" + (guild ? "1" : "0"));
    for (auto& [entry, s] : stock)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        std::sort(s.holders.begin(), s.holders.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        std::string holders;
        for (auto const& [num, count] : s.holders)
        {
            std::string const part =
                (num ? "#" + std::to_string(num) : std::string("банк")) + " ×" + std::to_string(count);
            if (holders.size() + part.size() > 90)
            {
                holders += ", …";
                break;
            }
            holders += (holders.empty() ? "" : ", ") + part;
        }
        auto const need = wantedNeed.find(entry);
        // R;itemId;quality;name;total;need (0 = not on the wanted list);holders
        SendLine(player, "R;" + std::to_string(entry) + ";" + std::to_string(proto ? proto->Quality : 1) + ";" +
                             OrdersText(proto ? proto->Name1 : std::to_string(entry), 40) + ";" +
                             std::to_string(s.total) + ";" +
                             std::to_string(need != wantedNeed.end() ? need->second : 0) + ";" +
                             OrdersText(holders, 100));
    }
    SendLine(player, "RE;n=" + std::to_string(stock.size()));
}

// ---------------------------------------------------------------------------------------------------------------------
// scripts
// ---------------------------------------------------------------------------------------------------------------------
class GuildCoordinatorPlayerScript : public PlayerScript
{
public:
    GuildCoordinatorPlayerScript()
        : PlayerScript("GuildCoordinatorPlayerScript", {PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_PLAYER_JUST_DIED,
                                                         PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE})
    {
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootguid*/) override
    {
        sGuildCoordinator.OnLoot(player, item, count);
    }

    void OnPlayerJustDied(Player* player) override { sGuildCoordinator.OnJustDied(player); }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        sGuildCoordinator.OnKilledByCreature(killed, killer);
    }
};

class GuildCoordinatorWorldScript : public WorldScript
{
public:
    GuildCoordinatorWorldScript()
        : WorldScript("GuildCoordinatorWorldScript", {WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_STARTUP})
    {
    }

    void OnStartup() override { sGuildCoordinator.Load(); }
    void OnUpdate(uint32 diff) override { sGuildCoordinator.Update(diff); }
};

void AddGuildCoordinatorScripts()
{
    new GuildCoordinatorPlayerScript();
    new GuildCoordinatorWorldScript();
}
