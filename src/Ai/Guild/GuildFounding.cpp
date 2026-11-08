/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [guild80] The guild bots found their guild themselves, by the game's own flow. The global leader (this coordinator)
// picks the founder and the meeting: the Guild Master (petitioner + tabard designer) and the founder whose walk plus
// the walks of the core's MinPetitionSigns cheapest signers there is the smallest (a member already walking into
// that capital - vendor, trainer - counts from its walk's target). The founder walks to the Guild Master and buys
// the charter with its own money (core Guild.CharterCost), the signers walk to it; every member that stands next to
// the founder (core TRADE_DISTANCE) gets the petition offered and signs it - the called ones and anyone passing by.
// With the signatures the founder turns the charter in there (the core makes the guild and adds the signers); then
// an online member with the invite right invites every other guild bot wherever it is, and it accepts. The guild
// master buys the first bank tab (core Guild.BankTabCost0) with its own free money at a guild vault and gives the
// member ranks deposit / withdraw rights on it. Until then the bank stays dormant: no bank orders, bots hold items.
// Every game action is the core's opcode handler (petition buy / offer / sign / turn-in, guild invite / accept, buy
// bank tab, rank rights), called on the world thread where the core itself runs these opcodes. No GM command, no
// handout, no teleport, no timer.

#include "GuildCoordinator.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

#include "AiObjectContext.h"
#include "BudgetValues.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildBankRun.h"
#include "GuildGear.h"
#include "GuildMgr.h"
#include "GuildPackets.h"
#include "Item.h"
#include "Language.h"
#include "Log.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgProfession.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PetitionMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    // NewRpgStatusUpdateAction's GO_CAMP arrival distance (the walk ends within it)
    constexpr float CAMP_REACH = 10.0f;
    // neutral, short; the first one free and valid is taken
    char const* const GUILD_NAMES[] = {"Wayfarers", "Hearthkeepers", "Kinsfolk", "Common Road"};

    std::string Gold(uint32 copper)
    {
        uint32 const g = copper / 10000, s = (copper / 100) % 100;
        return g ? std::to_string(g) + " g " + std::to_string(s) + " s" : std::to_string(s) + " s";
    }

    float Dist(WorldPosition const& a, WorldPosition const& b)
    {
        return std::sqrt((a.GetPositionX() - b.GetPositionX()) * (a.GetPositionX() - b.GetPositionX()) +
                         (a.GetPositionY() - b.GetPositionY()) * (a.GetPositionY() - b.GetPositionY()) +
                         (a.GetPositionZ() - b.GetPositionZ()) * (a.GetPositionZ() - b.GetPositionZ()));
    }

    bool FoundType(GuildOrderType t)
    {
        return t == GuildOrderType::FOUND_GUILD || t == GuildOrderType::SIGN_CHARTER || t == GuildOrderType::BANK_TAB;
    }

    Player* Online(uint32 low)
    {
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(low));
        return p && p->IsInWorld() && p->GetSession() ? p : nullptr;
    }

    uint32 SignCount(ObjectGuid charter)
    {
        Signatures const* sig = sPetitionMgr->GetSignature(charter);
        return sig ? uint32(sig->signatureMap.size()) : 0;
    }

    bool Signed(ObjectGuid charter, ObjectGuid player)
    {
        Signatures const* sig = sPetitionMgr->GetSignature(charter);
        return sig && sig->signatureMap.count(player);
    }
}

void GuildCoordinator::LoadGuildMasters()
{
    guildMasters_.clear();
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
        if (!tmpl || !(tmpl->npcflag & UNIT_NPC_FLAG_PETITIONER) || !(tmpl->npcflag & UNIT_NPC_FLAG_TABARDDESIGNER))
            continue;  // the core sells the guild charter only at a petitioner that is a tabard designer
        MapEntry const* map = sMapStore.LookupEntry(data.mapid);
        if (!map || map->Instanceable())
            continue;
        GuildMasterSpot spot;
        spot.entry = data.id;
        spot.name = tmpl->Name;
        spot.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
        spot.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        spot.faction = tmpl->faction;
        guildMasters_.push_back(spot);
    }
    LOG_INFO("playerbots", "[guild80] guild founding: {} Guild Masters on the continents, charter {} copper, {} "
             "signatures, first bank tab {} copper", guildMasters_.size(),
             sWorld->getIntConfig(CONFIG_CHARTER_COST_GUILD), sWorld->getIntConfig(CONFIG_MIN_PETITION_SIGNS),
             sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_0));
}

uint32 GuildCoordinator::FoundingFreeMoney(Player* p)
{
    // what the module's budget leaves (repair, ammo, spells, travel kept back) minus the next profession ranks
    uint32 money = p->GetMoney();
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
        money = ai->GetAiObjectContext()
                    ->GetValue<uint32>("free money for", std::to_string(uint32(NeedMoneyFor::anything)))
                    ->Get();
    uint32 const ranks = NewRpgProfession::NextRankCost(p);
    return money > ranks ? money - ranks : 0;
}

bool GuildCoordinator::FoundingEligible(uint32 low, Player* p) const
{
    if (!p || !p->IsInWorld() || !p->GetSession() || !p->IsAlive() || !GET_PLAYERBOT_AI(p) || HumanCharacter(p) ||
        p->GetGuildId() || p->GetGuildIdInvited())
        return false;
    PlanRow const* row = Row(low);
    if (!row || row->grp < 'A' || row->grp > 'C')
        return false;
    auto const st = bots_.find(low);
    if (st == bots_.end())
        return true;
    if (st->second.fiveId)
        return false;  // a five's dungeon first
    return st->second.foundRefused.empty() || st->second.foundRefusedLevel != p->GetLevel();
}

float GuildCoordinator::FoundingCost(Player* p, GuildMasterSpot const& spot) const
{
    if (p->GetMapId() != spot.pos.GetMapId())
        return FLT_MAX;
    FactionTemplateEntry const* npc = sFactionTemplateStore.LookupEntry(spot.faction);
    FactionTemplateEntry const* own = p->GetFactionTemplateEntry();
    if (npc && own && npc->IsHostileTo(*own))
        return FLT_MAX;
    float cost = p->GetExactDist(spot.pos.GetPositionX(), spot.pos.GetPositionY(), spot.pos.GetPositionZ());
    // a natural trip: it already walks into this city (vendor, trainer): only the step from its target counts
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
    {
        WorldPosition target;
        if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&ai->rpgInfo.data))
            target = camp->pos;
        else if (auto const* prof = std::get_if<NewRpgInfo::DoProfession>(&ai->rpgInfo.data))
            target = prof->pos;
        if (target.GetMapId() == spot.pos.GetMapId() && target != WorldPosition() &&
            sMapMgr->GetZoneId(PHASEMASK_NORMAL, target.GetMapId(), target.GetPositionX(), target.GetPositionY(),
                               target.GetPositionZ()) == spot.zoneId)
            cost = std::min(cost, Dist(target, spot.pos));
    }
    return cost;
}

// world thread
void GuildCoordinator::UpdateFounding()
{
    if (guildMasters_.empty() || plan_.empty())
        return;
    uint32 const need = sWorld->getIntConfig(CONFIG_MIN_PETITION_SIGNS);
    uint32 const charterCost = sWorld->getIntConfig(CONFIG_CHARTER_COST_GUILD);
    std::vector<std::string> logs;      // [guild80] action lines
    std::vector<std::string> decisions;  // the global leader's decisions

    // 1. the game state, read from the core: the project guild (its leader is a guild bot), else a charter in a bot's
    // bags (a restart keeps both)
    std::vector<std::pair<uint32, Player*>> online;
    for (PlanRow const& row : plan_)
        if (row.grp >= 'A' && row.grp <= 'C')
            if (Player* p = Online(row.low))
                online.emplace_back(row.low, p);
    Guild* guild = nullptr;
    {
        std::map<uint32, uint32> members;
        for (auto const& [low, p] : online)
            if (Guild* g = p->GetGuildId() ? sGuildMgr->GetGuildById(p->GetGuildId()) : nullptr)
                if (IsPlanned(g->GetLeaderGUID().GetCounter()))
                    ++members[g->GetId()];
        uint32 best = 0;
        for (auto const& [id, n] : members)
            if (!best || n > members[best])
                best = id;
        if (!best && found_.guildId && sGuildMgr->GetGuildById(found_.guildId))
            best = found_.guildId;
        guild = best ? sGuildMgr->GetGuildById(best) : nullptr;
    }
    Player* founder = nullptr;
    ObjectGuid charter;
    std::string charterName;
    if (!guild)
        for (auto const& [low, p] : online)
            if (Item* item = p->GetItemByEntry(GUILD_CHARTER))
                if (Petition const* pet = sPetitionMgr->GetPetition(item->GetGUID()))
                    if (pet->petitionType == GUILD_CHARTER_TYPE && pet->ownerGuid == p->GetGUID() &&
                        (!founder || SignCount(item->GetGUID()) > SignCount(charter)))
                    {
                        founder = p;
                        charter = item->GetGUID();
                        charterName = pet->petitionName;
                    }

    if (guild)
    {
        // 2a. the guild exists: invites, then the bank tab
        Player* inviter = nullptr;
        if (Player* leader = ObjectAccessor::FindPlayer(guild->GetLeaderGUID()))
            if (leader->IsInWorld() && leader->GetSession() && guild->HasRankRight(leader, GR_RIGHT_INVITE))
                inviter = leader;
        for (auto const& [low, p] : online)
            if (!inviter && p->GetGuildId() == guild->GetId() && guild->HasRankRight(p, GR_RIGHT_INVITE))
                inviter = p;
        std::map<uint32, std::string> joinState;
        for (PlanRow const& row : plan_)
        {
            if (row.grp < 'A' || row.grp > 'C')
                continue;
            Player* p = Online(row.low);
            if (p && p->GetGuildId() == guild->GetId())
                continue;
            if (!p)
            {
                joinState[row.low] = "offline: invited on login";
                continue;
            }
            if (!GET_PLAYERBOT_AI(p) || HumanCharacter(p))
                continue;  // the human's own play: never commanded
            if (p->GetGuildId())
            {
                Guild* other = sGuildMgr->GetGuildById(p->GetGuildId());
                joinState[row.low] = "in another guild «" + (other ? other->GetName() : std::string("?")) + "»";
                continue;
            }
            if (!inviter)
            {
                joinState[row.low] = "waits: nobody with invite rights online";
                continue;
            }
            if (p->GetGuildIdInvited() != guild->GetId())
                guild->HandleInviteMember(inviter->GetSession(), p->GetName());  // CMSG_GUILD_INVITE (by name)
            if (p->GetGuildIdInvited() == guild->GetId())
            {
                WorldPackets::Guild::AcceptGuildInvite accept{WorldPacket(CMSG_GUILD_ACCEPT)};
                p->GetSession()->HandleGuildAcceptOpcode(accept);
            }
            if (p->GetGuildId() == guild->GetId())
                logs.push_back(p->GetName() + " joins «" + guild->GetName() + "» (invited by " + inviter->GetName() +
                               ")");
            else
                joinState[row.low] = "the invite failed (" + inviter->GetName() + ")";
        }

        // the first bank tab: the guild master's own gold, at a vault
        uint32 const tabCost = sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_0);
        uint32 tabGm = 0;
        WorldPosition vault;
        std::string state;
        if (!guild->GetPurchasedTabsSize() && tabCost)
        {
            Player* gm = ObjectAccessor::FindPlayer(guild->GetLeaderGUID());
            uint32 pool = 0;  // the members' free money (what pooling could add)
            for (auto const& [low, p] : online)
                if (p->GetGuildId() == guild->GetId() && p != gm && GET_PLAYERBOT_AI(p))
                    pool += FoundingFreeMoney(p);
            if (!gm || !gm->IsInWorld() || !gm->GetSession())
                state = "«" + guild->GetName() + "»: the bank tab waits for the guild master (offline)";
            else
            {
                uint32 const own = GET_PLAYERBOT_AI(gm) ? FoundingFreeMoney(gm) : 0;
                float dist = 0;
                if (own >= tabCost && GET_PLAYERBOT_AI(gm) && !HumanControls(gm) &&
                    GuildBankRun::NearestVault(gm, vault, dist))
                {
                    tabGm = gm->GetGUID().GetCounter();
                    if (GuildBankRun::AtVault(gm, vault))
                    {
                        guild->HandleBuyBankTab(gm->GetSession(), 0);  // CMSG_GUILD_BANK_BUY_TAB
                        if (guild->GetPurchasedTabsSize())
                        {
                            // CMSG_GUILD_RANK per member rank: view + deposit, withdraw (core: unlimited slots)
                            static uint32 const rankNames[] = {LANG_GUILD_MASTER, LANG_GUILD_OFFICER,
                                                               LANG_GUILD_VETERAN, LANG_GUILD_MEMBER,
                                                               LANG_GUILD_INITIATE};
                            std::array<GuildBankRightsAndSlots, GUILD_BANK_MAX_TABS> rights;
                            rights[0] = GuildBankRightsAndSlots(0, uint8(GUILD_BANK_RIGHT_DEPOSIT_ITEM),
                                                                uint32(GUILD_WITHDRAW_SLOT_UNLIMITED));
                            for (uint8 rank = 1; rank < std::size(rankNames); ++rank)
                                guild->HandleSetRankInfo(gm->GetSession(), rank,
                                                         sObjectMgr->GetAcoreString(rankNames[rank], LOCALE_enUS),
                                                         guild->GetRankRights(rank), 0, rights);
                            logs.push_back(gm->GetName() + " buys bank tab 1 of «" + guild->GetName() + "» for " +
                                           Gold(tabCost) + " of its own gold; member ranks may deposit / withdraw");
                            decisions.push_back("«" + guild->GetName() + "»: guild master " + gm->GetName() +
                                                " bought the bank tab with its own " + Gold(tabCost) +
                                                " — the bank is open");
                            tabGm = 0;
                        }
                    }
                    else
                        state = "«" + guild->GetName() + "»: guild master " + gm->GetName() + " walks to the vault (" +
                                std::to_string(uint32(dist)) + " yd) to buy the tab for " + Gold(tabCost);
                }
                else
                    state = "«" + guild->GetName() + "»: bank tab " + Gold(tabCost) + " — the guild master has free " +
                            Gold(own) +
                            (own + pool >= tabCost
                                 ? ", members' donations would cover it (" + Gold(pool) +
                                       ") — the global leader decides donations"
                                 : ", all members have free " + Gold(pool) + " — the guild master saves himself") +
                            "; the bank sleeps, items are held by their holders";
            }
        }

        std::lock_guard<std::mutex> lockGuard(lock_);
        if (found_.guildId != guild->GetId())
        {
            if (found_.founderLow && found_.charterGuid)
                decisions.push_back("guild «" + guild->GetName() + "» founded: guild master " +
                                    Who(guild->GetLeaderGUID().GetCounter()) + ", " +
                                    std::to_string(guild->GetMemberCount()) + " members; invites the rest");
            found_ = Founding{};
            found_.guildId = guild->GetId();
        }
        found_.joinState = std::move(joinState);
        found_.tabGmLow = tabGm;
        found_.vault = vault;
        found_.state = state;
        for (std::string const& text : decisions)
            Decision(text);
        for (std::string const& text : logs)
            LOG_INFO("playerbots", "[guild80] {}", text);
        if (found_.state != found_.lastLog)
        {
            found_.lastLog = found_.state;
            if (!found_.state.empty())
                LOG_INFO("playerbots", "[guild80] state: {}", found_.state);
        }
        return;
    }

    // 2b. no guild yet: the plan (founder, Guild Master, signers), lock_ held
    uint32 founderLow = 0;
    int32 spot = -1;
    std::vector<uint32> eligible;  // may sign when standing next to the founder
    {
        std::lock_guard<std::mutex> lockGuard(lock_);
        found_.guildId = 0;
        found_.joinState.clear();
        found_.tabGmLow = 0;
        found_.needSigns = need;
        std::vector<std::pair<uint32, Player*>> pool;
        for (auto const& [low, p] : online)
            if (FoundingEligible(low, p))
                pool.emplace_back(low, p);
        for (auto const& [low, p] : pool)
            eligible.push_back(low);
        uint32 const signs = founder ? SignCount(charter) : 0;
        uint32 const missing = signs < need ? need - signs : 0;
        // the cheapest `missing` signers for this Guild Master (not the founder, not signed yet)
        auto const pick = [&](GuildMasterSpot const& s, uint32 without, std::vector<std::pair<float, uint32>>& out)
        {
            out.clear();
            for (auto const& [low, p] : pool)
            {
                if (low == without || (founder && Signed(charter, p->GetGUID())))
                    continue;
                float const c = FoundingCost(p, s);
                if (c < FLT_MAX)
                    out.emplace_back(c, low);
            }
            std::sort(out.begin(), out.end());
        };
        std::vector<std::pair<float, uint32>> best;
        float bestCost = FLT_MAX;
        std::vector<std::pair<float, uint32>> cand;
        int32 bestSpot = -1;
        std::string why;
        if (founder)
        {
            // the charter is bought: its owner founds; the spot stays unless it is not on its map any more
            founderLow = founder->GetGUID().GetCounter();
            if (found_.founderLow == founderLow && found_.spot >= 0 &&
                FoundingCost(founder, guildMasters_[found_.spot]) < FLT_MAX)
                spot = found_.spot;
            for (size_t i = 0; i < guildMasters_.size() && spot < 0; ++i)
            {
                float const cf = FoundingCost(founder, guildMasters_[i]);
                if (cf == FLT_MAX)
                    continue;
                pick(guildMasters_[i], founderLow, cand);
                float total = cf;
                for (size_t k = 0; k < cand.size() && k < missing; ++k)
                    total += cand[k].first;
                // a spot that can gather all missing signers first, then the cheapest
                bool const full = cand.size() >= missing;
                bool const bestFull = best.size() >= missing && bestCost < FLT_MAX;
                if (bestCost == FLT_MAX || (full && !bestFull) || (full == bestFull && total < bestCost))
                {
                    bestCost = total;
                    best = cand;
                    bestSpot = int32(i);
                }
            }
            if (spot < 0)
                spot = bestSpot;
            else
                pick(guildMasters_[spot], founderLow, best);
            why = "holds the charter «" + charterName + "»";
        }
        else
        {
            // keep the chosen founder while it can still found there; else the cheapest meeting of all
            Player* kept = found_.founderLow ? Online(found_.founderLow) : nullptr;
            if (kept && found_.spot >= 0 && FoundingEligible(found_.founderLow, kept) &&
                FoundingFreeMoney(kept) >= charterCost && FoundingCost(kept, guildMasters_[found_.spot]) < FLT_MAX)
            {
                pick(guildMasters_[found_.spot], found_.founderLow, cand);
                if (cand.size() >= missing)
                {
                    founderLow = found_.founderLow;
                    spot = found_.spot;
                    best = cand;
                    bestCost = found_.planCost;
                    why = found_.signerWhy.count(founderLow) ? found_.signerWhy[founderLow] : "previous choice";
                }
            }
            for (size_t i = 0; i < guildMasters_.size() && !founderLow; ++i)
                for (auto const& [low, p] : pool)
                {
                    // [guild15] the founder is a raid A member (PLAYER account): a GUILDBOT random-account founder
                    // makes the module treat the guild as a bot guild (its level-bracket rules for the members)
                    if (PlanRow const* row = Row(low); !row || row->grp != 'A')
                        continue;
                    if (FoundingFreeMoney(p) < charterCost)
                        continue;
                    float const cf = FoundingCost(p, guildMasters_[i]);
                    if (cf == FLT_MAX)
                        continue;
                    pick(guildMasters_[i], low, cand);
                    if (cand.size() < missing)
                        continue;
                    float total = cf;
                    for (size_t k = 0; k < missing; ++k)
                        total += cand[k].first;
                    // equal cost: the raid A leader founds
                    if (total < bestCost || (total == bestCost && low == raidLeader_[0]))
                    {
                        bestCost = total;
                        best = cand;
                        spot = int32(i);
                        founderLow = low;
                    }
                }
            if (founderLow && why.empty())
                why = "lowest meeting cost: " + std::to_string(uint32(bestCost)) + " yd of walking in total" +
                      (founderLow == raidLeader_[0] ? " (raid leader A)" : "");
        }

        std::string state;
        if (!founderLow || spot < 0)
        {
            uint32 rich = 0;
            for (auto const& [low, p] : pool)
                rich += FoundingFreeMoney(p) >= charterCost ? 1 : 0;
            state = !rich ? "no guild: nobody has a free " + Gold(charterCost) + " for the charter"
                          : "no guild: at no guild master can one gather " + std::to_string(need + 1) +
                                " free bots on one continent (free " + std::to_string(pool.size()) + ")";
            found_.founderLow = 0;
            found_.spot = -1;
            found_.signers.clear();
            found_.shortBy = 0;
        }
        else
        {
            if (found_.founderLow != founderLow || found_.spot != spot)
                decisions.push_back("guild founder " + Who(founderLow) + ", meeting at " + guildMasters_[spot].name +
                                    " (" + GuildGear::MapName(guildMasters_[spot].pos.GetMapId()) + ") — " + why);
            // keep called signers that still can come, fill up with the cheapest
            std::vector<uint32> signers;
            std::map<uint32, std::string> signerWhy;
            for (uint32 low : found_.signers)
                for (auto const& [c, l] : best)
                    if (l == low && signers.size() < missing)
                    {
                        signers.push_back(low);
                        signerWhy[low] = found_.signerWhy[low];
                    }
            for (auto const& [c, low] : best)
            {
                if (signers.size() >= missing)
                    break;
                if (std::find(signers.begin(), signers.end(), low) != signers.end())
                    continue;
                signers.push_back(low);
                Player* p = Online(low);
                float const direct = p ? p->GetExactDist(guildMasters_[spot].pos.GetPositionX(),
                                                         guildMasters_[spot].pos.GetPositionY(),
                                                         guildMasters_[spot].pos.GetPositionZ())
                                       : c;
                signerWhy[low] =
                    c + 1.0f < direct
                        ? "on the way: already walks to town, " + std::to_string(uint32(c)) + " yd"
                        : "of the nearest " + std::to_string(missing) + ", " + std::to_string(uint32(c)) + " yd";
            }
            signerWhy[founderLow] = why;
            found_.founderLow = founderLow;
            found_.spot = spot;
            found_.signers = signers;
            found_.signerWhy = signerWhy;
            found_.planCost = bestCost;
            uint32 const shortBy = missing > signers.size() ? missing - uint32(signers.size()) : 0;
            found_.shortBy = shortBy;
            state = "guild being founded: " + Who(founderLow) + " at " + guildMasters_[spot].name +
                    (founder ? ", charter «" + charterName + "» " + std::to_string(signs) + "/" + std::to_string(need)
                             : ", still buying the charter") +
                    (shortBy ? "; short of free signers: " + std::to_string(shortBy) : "");
        }
        found_.charterGuid = charter.GetRawValue();
        found_.charterName = charterName;
        found_.signs = signs;
        found_.state = state;
        for (std::string const& text : decisions)
            Decision(text);
        decisions.clear();
        if (found_.state != found_.lastLog)
        {
            found_.lastLog = found_.state;
            LOG_INFO("playerbots", "[guild80] state: {}", found_.state);
        }
    }
    if (!founderLow || spot < 0)
        return;

    // 3. the game actions of the plan, world thread, no lock (the core's handlers may call back into scripts)
    Player* f = Online(founderLow);
    if (!f)
        return;
    GuildMasterSpot const& gmSpot = guildMasters_[spot];
    Creature* npc = f->FindNearestCreature(gmSpot.entry, INTERACTION_DISTANCE);
    bool const atGm = npc && f->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_PETITIONER);
    if (!founder && atGm && FoundingFreeMoney(f) >= charterCost)
    {
        std::string name;
        for (char const* candidate : GUILD_NAMES)
            if (!sGuildMgr->GetGuildByName(candidate) && ObjectMgr::IsValidCharterName(candidate))
            {
                name = candidate;
                break;
            }
        if (!name.empty())
        {
            // CMSG_PETITION_BUY, laid out as the client sends it (BuyPetitionAction)
            WorldPacket data(CMSG_PETITION_BUY);
            data << npc->GetGUID() << uint32(0) << uint64(0) << name << std::string("");
            for (uint8 i = 0; i < 7; ++i)
                data << uint32(0);
            data << uint16(0) << uint32(0) << uint32(0) << uint32(0);
            for (uint8 i = 0; i < 10; ++i)
                data << std::string("");
            data << uint32(0) << uint32(0);
            uint32 const before = f->GetMoney();
            f->GetSession()->HandlePetitionBuyOpcode(data);
            if (Item* item = f->GetItemByEntry(GUILD_CHARTER))
            {
                founder = f;
                charter = item->GetGUID();
                charterName = name;
                logs.push_back(f->GetName() + " buys the guild charter «" + name + "» at " + gmSpot.name + " for " +
                               Gold(before - f->GetMoney()) + " of its own money");
            }
            else
                logs.push_back(f->GetName() + " could not buy the charter at " + gmSpot.name);
        }
        else
            logs.push_back("no free guild name of the list");
    }
    if (founder)
    {
        // every eligible member standing next to the founder: offer, it signs (CMSG_OFFER_PETITION / _SIGN)
        for (uint32 low : eligible)
        {
            if (SignCount(charter) >= need)
                break;
            Player* m = Online(low);
            if (!m || m == f || Signed(charter, m->GetGUID()) || !m->IsWithinDistInMap(f, TRADE_DISTANCE))
                continue;
            WorldPacket offer(CMSG_OFFER_PETITION);
            offer << uint32(0) << charter << m->GetGUID();
            f->GetSession()->HandleOfferPetitionOpcode(offer);
            WorldPacket sign(CMSG_PETITION_SIGN);
            sign << charter << uint8(0);
            m->GetSession()->HandlePetitionSignOpcode(sign);
            if (Signed(charter, m->GetGUID()))
                logs.push_back(m->GetName() + " signs the charter «" + charterName + "» of " + f->GetName() + " (" +
                               std::to_string(SignCount(charter)) + "/" + std::to_string(need) + ")");
        }
        // the signatures complete: turn it in at the Guild Master (CMSG_TURN_IN_PETITION)
        if (SignCount(charter) >= need && atGm)
        {
            WorldPacket turnIn(CMSG_TURN_IN_PETITION);
            turnIn << charter;
            f->GetSession()->HandleTurnInPetitionOpcode(turnIn);
            if (Guild* g = f->GetGuildId() ? sGuildMgr->GetGuildById(f->GetGuildId()) : nullptr)
                logs.push_back(f->GetName() + " turns in the charter at " + gmSpot.name + ": guild «" + g->GetName() +
                               "» founded with " + std::to_string(g->GetMemberCount()) + " members");
            else
                logs.push_back(f->GetName() + " could not turn in the charter at " + gmSpot.name);
        }
    }
    std::lock_guard<std::mutex> lockGuard(lock_);
    found_.charterGuid = charter.GetRawValue();
    found_.charterName = charterName;
    found_.signs = founder ? SignCount(charter) : 0;
    if (Guild* g = f->GetGuildId() ? sGuildMgr->GetGuildById(f->GetGuildId()) : nullptr)
    {
        found_.guildId = g->GetId();
        Decision("guild «" + g->GetName() + "» founded at " + gmSpot.name + ": guild master " + Who(founderLow) + ", " +
                 std::to_string(g->GetMemberCount()) + " members (the signers); invites the rest");
        found_.signers.clear();
    }
    for (std::string const& text : logs)
        LOG_INFO("playerbots", "[guild80] {}", text);
}

// lock_ held (UpdateOrders)
void GuildCoordinator::FoundingOrders(BotState const& st, uint32 low, Player* p,
                                      std::vector<GuildOrder>& orders) const
{
    if (found_.guildId)
    {
        if (low == found_.tabGmLow && p->GetGuildId() == found_.guildId)
        {
            GuildOrder o;
            o.type = GuildOrderType::BANK_TAB;
            o.pos = found_.vault;
            o.cost = sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_0);
            o.details = "bank tab 1 for " + Gold(o.cost);
            o.reason = "guild master, own money is enough";
            o.status = "to the vault " +
                       std::to_string(uint32(p->GetExactDist(found_.vault.GetPositionX(), found_.vault.GetPositionY(),
                                                             found_.vault.GetPositionZ()))) +
                       " yd";
            o.executable = true;
            orders.push_back(o);
        }
        auto const join = found_.joinState.find(low);
        if (join != found_.joinState.end())
        {
            GuildOrder o;
            o.type = GuildOrderType::JOIN;
            Guild* g = sGuildMgr->GetGuildById(found_.guildId);
            o.details = "into the guild «" + (g ? g->GetName() : std::string("?")) + "»";
            o.reason = "all guild bots — into one guild";
            o.status = join->second;
            orders.push_back(o);
        }
        return;
    }
    if (found_.spot < 0 || !found_.founderLow)
        return;
    // [guild15] a held charter the plan cannot fill yet: the founder keeps it and everyone quests on; a member that
    // passes the founder still signs (TRADE_DISTANCE, the [guild80] passer-by rule)
    if (found_.charterGuid && found_.shortBy)
        return;
    GuildMasterSpot const& spot = guildMasters_[found_.spot];
    std::string const charter = found_.charterGuid
                                    ? "charter «" + found_.charterName + "» " + std::to_string(found_.signs) + "/" +
                                          std::to_string(found_.needSigns)
                                    : "the charter is not bought yet";
    auto const why = found_.signerWhy.find(low);
    if (low == found_.founderLow)
    {
        GuildOrder o;
        o.type = GuildOrderType::FOUND_GUILD;
        o.pos = spot.pos;
        o.npcEntry = spot.entry;
        o.cost = sWorld->getIntConfig(CONFIG_CHARTER_COST_GUILD);
        o.details = charter + ", at " + spot.name;
        o.reason = "founder: " + (why != found_.signerWhy.end() ? why->second : std::string("plan"));
        o.status = st.run && st.runType == GuildOrderType::FOUND_GUILD && st.runApproached
                       ? (found_.charterGuid ? "waits for signatures at the guild master" : "buys the charter")
                       : "walks to the guild master";
        o.executable = true;
        orders.push_back(o);
    }
    else if (std::find(found_.signers.begin(), found_.signers.end(), low) != found_.signers.end())
    {
        GuildOrder o;
        o.type = GuildOrderType::SIGN_CHARTER;
        o.pos = spot.pos;
        o.npcEntry = spot.entry;
        o.forLow = found_.founderLow;
        o.details = charter + " at " + Who(found_.founderLow) + ", " + spot.name;
        o.reason = why != found_.signerWhy.end() ? why->second : "signer";
        o.status =
            st.run && st.runType == GuildOrderType::SIGN_CHARTER ? "at the founder: signs" : "walks to the founder";
        o.executable = true;
        orders.push_back(o);
    }
}

// map thread, lock_ held: the walk to the Guild Master / the vault
bool GuildCoordinator::FoundStep(Player* bot, BotState& st, GuildOrder& order, GuildRunStep& step)
{
    WorldPosition const& pos = order.pos;
    if (pos.GetMapId() != bot->GetMapId())
        return false;
    float const dist = bot->GetExactDist(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
    // a solo bot walks only a safe walk ([guild14]); none: the planner leaves it out until its next level
    if (dist > CAMP_REACH &&
        NewRpgBaseAction::TripRefused(bot, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                                      order.type == GuildOrderType::BANK_TAB ? "guild vault" : "guild master"))
    {
        st.foundRefused =
            order.type == GuildOrderType::BANK_TAB ? "no safe way to the vault" : "no safe way to the guild master";
        st.foundRefusedLevel = bot->GetLevel();
        order.executable = false;
        order.status = st.foundRefused + " — quests";
        LOG_INFO("playerbots", "[guild80] {} L{}: {} ({} yd), left out until its next level", bot->GetName(),
                 bot->GetLevel(), st.foundRefused, uint32(dist));
        return false;
    }
    st.run = true;
    st.runType = order.type;
    st.runPos = pos;
    st.runApproached = false;
    step.kind = GuildRunStep::WALK;
    step.pos = pos;
    char const* const what = order.type == GuildOrderType::FOUND_GUILD    ? "FOUND_GUILD"
                             : order.type == GuildOrderType::SIGN_CHARTER ? "SIGN_CHARTER"
                                                                          : "BANK_TAB";
    step.reason = std::string(what) + " " + order.details + ", " + std::to_string(uint32(dist)) + " yd";
    return true;
}

uint8 GuildCoordinator::FoundCampReached(Player* bot, BotState& st)
{
    bool still = false;
    for (GuildOrder const& o : st.orders)
        still |= o.type == st.runType && o.executable;
    if (!still)
    {
        st.run = false;
        return 1;  // done (signed / founded / tab bought) or called off: back to quests
    }
    if (!st.runApproached)
    {
        st.runApproached = true;  // step up to the Guild Master / the vault itself
        bot->GetMotionMaster()->MovePoint(0, st.runPos.GetPositionX(), st.runPos.GetPositionY(),
                                          st.runPos.GetPositionZ());
    }
    return 2;  // it stands there; the world thread does the game action
}

std::string GuildCoordinator::FoundWait(uint32 low, BotState const& st) const
{
    if (!st.run || !FoundType(st.runType))
        return "";
    if (st.runType == GuildOrderType::BANK_TAB)
        return "guild master buys the bank tab at the vault";
    if (low == found_.founderLow)
        return found_.charterGuid ? "founder waits for signatures " + std::to_string(found_.signs) + "/" +
                                        std::to_string(found_.needSigns) + " at the Guild Master"
                                  : "founder buys the guild charter";
    return "signer at the founder";
}
