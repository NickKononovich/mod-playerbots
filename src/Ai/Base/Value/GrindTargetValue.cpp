/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GrindTargetValue.h"
#include "NewRpgBaseAction.h"  // [guild12]
#include "Formulas.h"  // [guild12]
#include "SoloSafety.h"  // [guild13]
#include "NewRpgCoop.h"  // [coop]
#include "GuildCoordinator.h"  // [regress10] the leader's REPAIR call
#include "NewRpgInfo.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "ServerFacade.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>          // [deaths17]
#include <unordered_map>  // [deaths17]

Unit* GrindTargetValue::Calculate()
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    Unit* target = nullptr;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    return target;
}

Unit* GrindTargetValue::FindTargetForGrinding(uint32 assistCount)
{
    Group* group = bot->GetGroup();
    Player* master = GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() ||
                   !GET_PLAYERBOT_AI(master)))
        master = nullptr;

    GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        return unit;
    }

    // [regress10] its main weapon is broken and the global leader's repair can run: no new pull until the repairer
    // (b347 Ginde L5: eight pulls of boars at 0.2-1.7 dps on her 200 yd walk to the vendor, two deaths to Crag Boars)
    if (SoloSafety::Applies(bot))
    {
        GuildCoordinator::RepairCall const repair = sGuildCoordinator.Repair(bot);
        // [broken40] ... while a repair can still come: money to pay a part, or a repairer visit not tried at this
        // money. Broken and broke after a visit (nothing left to sell), it fights on unarmed - never an IDLE without
        // end (b388 Peny, Irrua: 0 durability all run, no pull, no run, 5 deaths each). The line says which
        NewRpgInfo const& info = botAI->rpgInfo;
        bool const brokeAfterVisit = bot->GetMoney() == 0 && info.repairVisited;
        if (repair.weaponBroken && repair.ready && !brokeAfterVisit)
        {
            static std::mutex brokenLock;  // one line per bot and money value: a log cadence, no decision reads it
            static std::unordered_map<uint32, uint32> brokenLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(brokenLock);
                uint32& last = brokenLogged[bot->GetGUID().GetCounter()];
                fresh = last != bot->GetMoney() + 1;
                last = bot->GetMoney() + 1;
            }
            if (fresh)
                LOG_INFO("playerbots", "[broken40] {} L{} no new pull: its weapon is broken and its repair can run "
                         "(money {}c of {}c; {})",
                         bot->GetName(), bot->GetLevel(), bot->GetMoney(), repair.cost,
                         info.vendorRunEntry ? "on its vendor run" : "the vendor run is due");
            return nullptr;
        }
        if (repair.weaponBroken && brokeAfterVisit)
        {
            static std::mutex unarmedLock;
            static std::unordered_set<uint32> unarmedLogged;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> guard(unarmedLock);
                fresh = unarmedLogged.insert(bot->GetGUID().GetCounter()).second;
            }
            if (fresh)
                LOG_INFO("playerbots", "[broken40] {} L{} broken and broke after its repairer visit: fights on with "
                         "what it has (its pulls judged at its broken gear) until money comes",
                         bot->GetName(), bot->GetLevel());
        }
        // [regress10] after a death it rests and walks to its hub before any pull, and a rest asked for runs to full
        // first (b345 Guinudrond L11: risen at its corpse, it pulled a Tunnel Rat Vermin there at once, then a Forest
        // Lurker at 4 % mana on the walk to its hub; three deaths in the Silver Stream Mine)
        if (SoloSafety::HoldsPulls(bot))
            return nullptr;
    }

    GuidVector targets = *context->GetValue<GuidVector>("possible targets");
    if (targets.empty())
        return nullptr;

    float distance = 0;
    Unit* result = nullptr;
    // [guild13] a solo bot judges its pulls; [coop] a co-op party's leader judges them for its party
    bool const coop = NewRpgCoop::JudgesPulls(bot);
    bool const solo = SoloSafety::Applies(bot) || coop;
    std::vector<std::pair<float, Unit*>> candidates;
    std::unordered_map<uint32, bool> needForQuestMap;
    // [skin12] a skinning grind of the global leader (GO_GRIND / WANDER_RANDOM flagged so): a skinnable mob counts
    bool const forSkin =
        bot->HasSkill(SKILL_SKINNING) &&
        ((std::holds_alternative<NewRpgInfo::WanderRandom>(botAI->rpgInfo.data) &&
          std::get<NewRpgInfo::WanderRandom>(botAI->rpgInfo.data).forSkin) ||
         (std::holds_alternative<NewRpgInfo::GoGrind>(botAI->rpgInfo.data) &&
          std::get<NewRpgInfo::GoGrind>(botAI->rpgInfo.data).forSkin));
    auto const skinTarget = [&](Unit* u)
    {
        Creature* c = forSkin ? u->ToCreature() : nullptr;
        return c && c->GetCreatureTemplate() && c->GetCreatureTemplate()->SkinLootId;
    };
    // [guild12] a needed quest object in reach comes before mobs the quest does not need (the loot of objects)
    bool const questObjectNear =
        botAI->rpgInfo.GetStatus() == RPG_DO_QUEST && NewRpgBaseAction::FindNeededQuestObject(bot);

    for (ObjectGuid const guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;

        if (!unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
            continue;

        if (unit->ToCreature() && !unit->ToCreature()->GetCreatureTemplate()->lootid &&
            bot->GetReactionTo(unit) >= REP_NEUTRAL)
            continue;

        if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
            continue;

        // While questing, a mob the quest needs is fair game even if it is gray or several levels higher.
        auto const neededForQuest = [&]()
        {
            if (botAI->rpgInfo.GetStatus() != RPG_DO_QUEST)
                return false;
            if (needForQuestMap.find(unit->GetEntry()) == needForQuestMap.end())
                needForQuestMap[unit->GetEntry()] = needForQuest(unit);
            return needForQuestMap[unit->GetEntry()];
        };

        // [skin12] the global leader's skinning grind: a gray mob that carries skins is the order's target too (the
        // skin, not the XP: b350 Keenonk L12 at the Grizzled Den - the L6 Wendigos gray, "no mob of its level")
        if (!bot->isHonorOrXPTarget(unit) && !neededForQuest() && !skinTarget(unit))
            continue;

        if (questObjectNear && !neededForQuest())
            continue;

        // [skin12] another floor: a solo bot's candidate is checked by its path (the reachability below, before the
        // judge); the height gap alone dropped the mobs of a sloped cave floor (the den's Wendigos 9-18 yd above the
        // spot)
        if (!solo && abs(bot->GetPositionZ() - unit->GetPositionZ()) > INTERACTION_DISTANCE)
            continue;

        if (!bot->InBattleground() && GetTargetingPlayerCount(unit) > assistCount)
            continue;

        // if (!bot->InBattleground() && master && master->GetDistance(unit) >= sPlayerbotAIConfig.GrindDistance &&
        // !sRandomPlayerbotMgr.IsRandomBot(bot)) continue;

        // Bots in bot-groups no have a more limited range to look for grind target
        if (!bot->InBattleground() && master && botAI->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) &&
            ServerFacade::instance().GetDistance2d(master, unit) > sPlayerbotAIConfig.LootDistance)
        {
            if (botAI->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                botAI->TellMaster(chat->FormatWorldobject(unit) + " ignored (far from master).");
            continue;
        }

        // [guild12] never a mob of orange or red con (the core's level rule), not even one a quest needs: the quest is
        // postponed instead (Selalleas L3 died 4 times to Webwood Silkspinner L8-9)
        if (!bot->InBattleground() && !unit->GetGUID().IsPlayer() &&
            Acore::XP::GetColorCode(bot->GetLevel(), unit->GetLevel()) <= XP_ORANGE)
            continue;

        if (Creature* creature = unit->ToCreature())
            if (CreatureTemplate const* CreatureTemplate = creature->GetCreatureTemplate())
                if (CreatureTemplate->rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite"))
                    continue;

        if (!bot->IsWithinLOSInMap(unit))
        {
            continue;
        }

        bool inactiveGrindStatus = botAI->rpgInfo.GetStatus() != RPG_WANDER_RANDOM && botAI->rpgInfo.GetStatus() != RPG_IDLE;

        float aggroRange = 30.0f;
        if (unit->ToCreature())
            aggroRange = std::min(30.0f, unit->ToCreature()->GetAggroRange(bot) + 10.0f);
        bool outOfAggro = unit->ToCreature() && bot->GetDistance(unit) > aggroRange;
        if (inactiveGrindStatus && outOfAggro)
        {
            if (needForQuestMap.find(unit->GetEntry()) == needForQuestMap.end())
                needForQuestMap[unit->GetEntry()] = needForQuest(unit);

            if (!needForQuestMap[unit->GetEntry()])
                continue;
        }

        // [guild90] questing, a mob no quest needs is fought only when it is in the way: hostile and within its own
        // aggro reach of the bot or of the walk ahead to the quest spot (it comes anyway). No detour to it (b317
        // Eraelor L6 on q487, Gnarlpine Ambushers, pulled a Strigid Screecher L8 off the road and died).
        // [onpath11] user 2026-11-08 ("why does it fight mobs that don't block its way?", project rule pull only on
        // path): a solo bot on any walk (to a grind spot, a camp, a vendor, a node), not only questing, pulls only
        // a mob on that walk; before, a mob within its aggro reach + 10 yd of the bot was pulled off the walk's side
        bool const walks = botAI->rpgInfo.GetStatus() != RPG_IDLE && botAI->rpgInfo.GetStatus() != RPG_WANDER_RANDOM;
        if ((botAI->rpgInfo.GetStatus() == RPG_DO_QUEST || (solo && walks)) && unit->ToCreature() &&
            !neededForQuest() && !OnQuestWay(unit))
            continue;

        float unitDistance = -1.0f;
        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = ObjectAccessor::FindPlayer(itr->guid);
                if (!member || !member->IsAlive())
                    continue;

                float d = member->GetDistance(unit);
                if (unitDistance < 0.0f || d < unitDistance)
                    unitDistance = d;
            }
        }
        else
            unitDistance = bot->GetDistance(unit);

        // [guild13] a solo bot judges its candidates nearest first (SoloSafety::Judge) after the loop
        if (solo)
        {
            if (unitDistance >= 0.0f)
                candidates.emplace_back(unitDistance, unit);
            continue;
        }

        if (unitDistance < 0.0f || (result && unitDistance >= distance))
            continue;

        // Only a would-be pick pays for a (cached) path check: a mob on another floor is not walkable to.
        if (!_reachability.IsReachable(bot, unit))
            continue;

        distance = unitDistance;
        result = unit;
    }

    if (!solo)
        return result;

    // [guild13] the nearest pull it wins: a mob in a remembered danger zone (a death, a refused pack) is skipped until
    // the bot outlevels it; a pull it wins only at full rests it first; a pack it loses is remembered as a danger
    // zone (its quest spots there are postponed) and the next candidate is judged
    std::sort(candidates.begin(), candidates.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
    for (auto const& [unitDistance, unit] : candidates)
    {
        // [coop] a danger mark is the bot's solo judgement; the party judges the pack itself
        if (!coop && NewRpgBaseAction::InDangerZone(bot, unit->GetPositionX(), unit->GetPositionY()))
            continue;
        // [stuck258] an idle mob its combat engine had as target with no action that reached it (let go): picked
        // again it stood the same way (b428 Aestymro's party: "party engages" it, the walk walled by [party75])
        if (unit->GetGUID() == botAI->ReleasedIdleTarget() && !unit->IsInCombat())
            continue;
        if (!_reachability.IsReachable(bot, unit))
            continue;
        SoloSafety::Verdict const verdict = coop ? NewRpgCoop::JudgePull(bot, unit) : SoloSafety::Judge(bot, unit);
        if (verdict.kind == SoloSafety::Verdict::Kind::Win)
        {
            if (!coop)
                SoloSafety::NoteEngage(bot, unit, verdict);
            else
            {
                SoloSafety::NotePartyPull(bot, unit);  // [deaths18] the death line's party fight
                // [deaths17] the party's engaged pull: what its judge counted, on whom (a Win was never logged: b357's

                // 13 party-fight deaths had no line of the pull they died in)
                static std::mutex partyLogLock;
                static std::unordered_map<uint32, ObjectGuid> partyLogged;
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> guard(partyLogLock);
                    ObjectGuid& last = partyLogged[bot->GetGUID().GetCounter()];
                    fresh = last != unit->GetGUID();
                    last = unit->GetGUID();
                }
                if (fresh)
                    LOG_INFO("playerbots",
                             "[deaths17] {} L{} party engages {} L{}: {} mobs ({} above), {:.0f} dmg over {:.0f} s, "
                             "ehp {:.0f} ({})",
                             bot->GetName(), bot->GetLevel(), unit->GetName(), unit->GetLevel(), verdict.mobs,
                             verdict.above, verdict.damage, verdict.fightSec, verdict.ehpNow,
                             verdict.why.empty() ? "won" : verdict.why);
            }
            return unit;
        }
        if (verdict.kind == SoloSafety::Verdict::Kind::RestFirst)
        {
            std::string const why = std::string("to pull ") + unit->GetName() + ": " + verdict.why;
            if (coop)
                NewRpgCoop::RestParty(bot, why);
            else
                SoloSafety::RequestRest(bot, why);
            return nullptr;
        }
        NewRpgBaseAction::RememberDanger(bot, verdict.at.GetPositionX(), verdict.at.GetPositionY(), verdict.radius,
                                         unit->GetEntry(), verdict.topLevel);
    }
    return nullptr;
}

bool GrindTargetValue::OnQuestWay(Unit* unit)
{
    if (!unit->IsHostileTo(bot))
        return false;  // a neutral mob does not come by itself
    float const aggro = unit->ToCreature()->GetAggroRange(bot);
    if (bot->GetDistance(unit) <= aggro)
        return true;
    float const sight = sPlayerbotAIConfig.SightDistance;
    // [onpath11] the route the bot walks: its far route from the point nearest the bot on, a sight ahead - the mob's
    // aggro circle crosses one of its stretches (a route round a camp passes no camp mob)
    std::vector<G3D::Vector3> const& route = botAI->rpgInfo.farRoute;
    if (route.size() >= 2)
    {
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        size_t nearest = 0;
        float best = std::numeric_limits<float>::max();
        for (size_t i = 0; i < route.size(); ++i)
            if (float const d = (route[i] - here).squaredLength(); d < best)
            {
                best = d;
                nearest = i;
            }
        G3D::Vector3 const at(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
        float walked = 0.0f;
        G3D::Vector3 from = here;
        for (size_t i = nearest; i < route.size() && walked < sight; ++i)
        {
            G3D::Vector3 const to = route[i];
            G3D::Vector3 const ab = to - from;
            float const len2 = ab.x * ab.x + ab.y * ab.y;
            float const t =
                len2 > 0.01f ? std::clamp(((at.x - from.x) * ab.x + (at.y - from.y) * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
            if (std::hypot(from.x + ab.x * t - at.x, from.y + ab.y * t - at.y) <= aggro)
                return true;
            walked += std::sqrt(len2);
            from = to;
        }
        return false;
    }
    WorldPosition dest;
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
        dest = quest->pos != WorldPosition() ? quest->pos : quest->giverPos;
    else if (auto const* grind = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
        dest = grind->pos;
    else if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
        dest = camp->pos;
    if (dest == WorldPosition() || dest.GetMapId() != bot->GetMapId())
        return false;
    // the walk's next stretch (sight range): the mob's aggro circle touches it ahead of the bot
    float const fromX = bot->GetPositionX(), fromY = bot->GetPositionY();
    float dx = dest.GetPositionX() - fromX, dy = dest.GetPositionY() - fromY;
    float const length = std::hypot(dx, dy);
    if (length < 1.0f)
        return false;
    dx /= length;
    dy /= length;
    float const along = (unit->GetPositionX() - fromX) * dx + (unit->GetPositionY() - fromY) * dy;
    if (along <= 0.0f || along > std::min(length, sight))
        return false;
    float const across = std::fabs((unit->GetPositionX() - fromX) * dy - (unit->GetPositionY() - fromY) * dx);
    return across <= aggro;
}

bool GrindTargetValue::needForQuest(Unit* target)
{
    QuestStatusMap& questMap = bot->getQuestStatusMap();
    for (auto& quest : questMap)
    {
        Quest const* questTemplate = sObjectMgr->GetQuestTemplate(quest.first);
        if (!questTemplate)
            continue;

        uint32 questId = questTemplate->GetQuestId();
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);

        if (status == QUEST_STATUS_INCOMPLETE)
        {
            QuestStatusData const* questStatus = &bot->getQuestStatusMap()[questId];

            if (questTemplate->GetQuestLevel() > bot->GetLevel() + 5)
                continue;

            for (int j = 0; j < QUEST_OBJECTIVES_COUNT; j++)
            {
                int32 entry = questTemplate->RequiredNpcOrGo[j];

                if (entry && entry > 0)
                {
                    int required = questTemplate->RequiredNpcOrGoCount[j];
                    int available = questStatus->CreatureOrGOCount[j];

                    if (required && available < required && target->GetEntry() == uint32(entry))
                        return true;
                }
            }
        }
    }

    if (CreatureTemplate const* data = sObjectMgr->GetCreatureTemplate(target->GetEntry()))
    {
        if (uint32 lootId = data->lootid)
        {
            // [drop74] ... and the creature is a dropper worth killing for it: b415 Kylanor took Murloc Lurkers
            // (Murloc Eye at 0.0226 %) as quest mobs for q38 - 34 kills, no eye - while Coastrunners drop it at 30 %
            if (LootTemplates_Creature.HaveQuestLootForPlayer(lootId, bot) &&
                NewRpgBaseAction::WorthItemSource(bot, target->GetEntry()))
            {
                return true;
            }
        }
    }

    return false;
}

uint32 GrindTargetValue::GetTargetingPlayerCount(Unit* unit)
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    uint32 count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot)
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
        if ((botAI && *botAI->GetAiObjectContext()->GetValue<Unit*>("current target") == unit) ||
            (!botAI && member->GetTarget() == unit->GetGUID()))
            ++count;
    }

    return count;
}
