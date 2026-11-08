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
        if (repair.weaponBroken && repair.ready)
            return nullptr;
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

        if (!bot->isHonorOrXPTarget(unit) && !neededForQuest())
            continue;

        if (questObjectNear && !neededForQuest())
            continue;

        if (abs(bot->GetPositionZ() - unit->GetPositionZ()) > INTERACTION_DISTANCE)
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
        if (botAI->rpgInfo.GetStatus() == RPG_DO_QUEST && unit->ToCreature() && !neededForQuest() &&
            !OnQuestWay(unit))
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
        if (!_reachability.IsReachable(bot, unit))
            continue;
        SoloSafety::Verdict const verdict = coop ? NewRpgCoop::JudgePull(bot, unit) : SoloSafety::Judge(bot, unit);
        if (verdict.kind == SoloSafety::Verdict::Kind::Win)
        {
            if (!coop)
                SoloSafety::NoteEngage(bot, unit, verdict);
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
    auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!quest)
        return false;
    WorldPosition const dest = quest->pos != WorldPosition() ? quest->pos : quest->giverPos;
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
    if (along <= 0.0f || along > std::min(length, sPlayerbotAIConfig.SightDistance))
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
            if (LootTemplates_Creature.HaveQuestLootForPlayer(lootId, bot))
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
