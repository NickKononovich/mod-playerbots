/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptQuestAction.h"
#include "ObjectAccessor.h"
#include "Event.h"
#include "GuildCoordinator.h"  // [quest14] the grey rule
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"

bool AcceptAllQuestsAction::ProcessQuest(Quest const* quest, Object* questGiver)
{
    // [quest14] user rule 2026-11-08: no grey quest unless it is needed
    if (bot->GetQuestStatus(quest->GetQuestId()) == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) &&
        GuildCoordinator::IsGreyQuest(bot, quest) && !GuildCoordinator::HumanCharacter(bot))
    {
        std::string const need = sGuildCoordinator.GreyQuestNeed(bot, quest);
        GuildCoordinator::LogGrey(bot, need.empty() ? "skips" : "keeps", quest,
                                  need.empty() ? std::string("not needed (no chain step, upgrade, recipe, skill, "
                                                             "attunement, key or wanted item)")
                                               : need);
        if (need.empty())
            return false;
    }
    if (!AcceptQuest(quest, questGiver->GetGUID())) return false;

    auto text_quest = ChatHelper::FormatQuest(quest);
    bot->PlayDistanceSound(620);

    if (botAI->HasStrategy("debug quest", BotState::BOT_STATE_NON_COMBAT) || botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
    {
        LOG_INFO("playerbots", "{} => Quest [{}] accepted", bot->GetName(), quest->GetTitle());
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_accept_debug",
            "Quest [%quest] accepted",
            {{"%quest", text_quest}});
        bot->Say(text, LANG_UNIVERSAL);
    }

    return true;
}

bool AcceptQuestAction::Execute(Event event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    Player* bot = botAI->GetBot();
    uint64_t guid = 0;
    uint32 quest = 0;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(requester);
    quest = ch.extractQuestId(text);

    bool hasAccept = false;

    if (event.getPacket().empty())
    {
        GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
        for (auto i = npcs.begin(); i != npcs.end(); i++)
        {
            Unit* unit = botAI->GetUnit(*i);
            if (unit && quest && unit->hasQuest(quest))
            {
                guid = unit->GetGUID().GetRawValue();
                break;
            }
            if (unit && text == "*" && bot->GetDistance(unit) <= INTERACTION_DISTANCE)
                hasAccept |= QuestAction::ProcessQuests(unit);
        }
        GuidVector gos = AI_VALUE(GuidVector, "nearest game objects no los");
        for (auto i = gos.begin(); i != gos.end(); i++)
        {
            GameObject* go = botAI->GetGameObject(*i);
            if (go && quest && go->hasQuest(quest))
            {
                guid = go->GetGUID().GetRawValue();
                break;
            }
            if (go && text == "*" && bot->GetDistance(go) <= INTERACTION_DISTANCE)
                hasAccept |= QuestAction::ProcessQuests(go);
        }
    }
    else
    {
        WorldPacket& p = event.getPacket();
        p.rpos(0);
        p >> guid >> quest;
    }

    if (!quest || !guid)
        return false;

    Quest const* qInfo = sObjectMgr->GetQuestTemplate(quest);
    if (!qInfo)
        return false;

    hasAccept |= AcceptQuest(qInfo, ObjectGuid(guid));

    // Complete on acceptance and handed in to the same giver (#2495): hand it in now. The master's turn-in packet
    // may already have been handled, before this accept.
    if (hasAccept && bot->GetQuestStatus(quest) == QUEST_STATUS_COMPLETE)
    {
        Object* giver =
            ObjectAccessor::GetObjectByTypeMask(*bot, ObjectGuid(guid), TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT);
        if (giver && giver->hasInvolvedQuest(quest))
            botAI->DoSpecificAction("talk to quest giver", Event("accept quest", ObjectGuid(guid)), true);
    }

    if (hasAccept)
    {
        std::stringstream ss;
        ss << "AcceptQuestAction [" << qInfo->GetTitle() << "] - [" << std::to_string(qInfo->GetQuestId()) << "]";
        LOG_DEBUG("playerbots", "{}", ss.str().c_str());
        // botAI->TellMaster(ss.str());
    }

    return hasAccept;
}

bool AcceptQuestShareAction::Execute(Event event)
{
    Player* master = GetMaster();
    Player* bot = botAI->GetBot();

    WorldPacket& p = event.getPacket();
    p.rpos(0);
    uint32 quest;
    p >> quest;

    Quest const* qInfo = sObjectMgr->GetQuestTemplate(quest);
    if (!qInfo || !bot->GetDivider())
        return false;

    quest = qInfo->GetQuestId();

    if (bot->HasQuest(quest))
    {
        bot->SetDivider(ObjectGuid::Empty);
        botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_already_have_error", "I have this quest", {}));
        return false;
    }

    if (!bot->CanTakeQuest(qInfo, false))
    {
        // can't take quest
        bot->SetDivider(ObjectGuid::Empty);
        botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_cant_take_error", "I can't take this quest", {}));

        return false;
    }

    if (!bot->GetDivider().IsEmpty())
    {
        // send msg to quest giving player
        master->SendPushToPartyResponse(bot, QUEST_PARTY_MSG_ACCEPT_QUEST);
        bot->SetDivider(ObjectGuid::Empty);
    }

    if (bot->CanAddQuest(qInfo, false))
    {
        bot->AddQuest(qInfo, master);

        if (bot->CanCompleteQuest(quest))
            bot->CompleteQuest(quest);

        // Runsttren: did not add typeid switch from WorldSession::HandleQuestgiverAcceptQuestOpcode!
        // I think it's not needed, cause typeid should be TYPEID_PLAYER - and this one is not handled
        // there and there is no default case also.

        if (qInfo->GetSrcSpell() > 0)
        {
            bot->CastSpell(bot, qInfo->GetSrcSpell(), true);
        }

        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_accept", "Quest accepted", {}));
        return true;
    }

    return false;
}

bool ConfirmQuestAction::Execute(Event event)
{
    Player* bot = botAI->GetBot();
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    WorldPacket& p = event.getPacket();
    p.rpos(0);
    uint32 quest;
    p >> quest;
    Quest const* qInfo = sObjectMgr->GetQuestTemplate(quest);

    if (!qInfo)
        return false;

    quest = qInfo->GetQuestId();
    if (!bot->CanTakeQuest(qInfo, false))
    {
        // can't take quest
        // botAI->TellError("quest_cant_take");
        return false;
    }

    if (bot->CanAddQuest(qInfo, false))
    {
        bot->AddQuest(qInfo, requester);

        if (bot->CanCompleteQuest(quest))
            bot->CompleteQuest(quest);

        if (qInfo->GetSrcSpell() > 0)
        {
            bot->CastSpell(bot, qInfo->GetSrcSpell(), true);
        }

        // botAI->TellMaster("quest_accept");
        return true;
    }

    return false;
}
