/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DropQuestAction.h"
#include "ChatHelper.h"
#include "Event.h"
#include "GuildCoordinator.h"  // [judge14] the one grey judge (IsGreyQuest + the user's GreyQuestNeed rule)
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"

bool DropQuestAction::Execute(Event event)
{
    std::string const link = event.getParam();

    Player* master = GetMaster();
    if (!master)
        return false;

    PlayerbotChatHandler handler(master);
    uint32 entry = handler.extractQuestId(link);

    // remove all quest entries for 'entry' from quest log
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 logQuest = bot->GetQuestSlotQuestId(slot);

        Quest const* quest = sObjectMgr->GetQuestTemplate(logQuest);
        if (!quest)
            continue;

        if (logQuest == entry || link.find(quest->GetTitle()) != std::string::npos)
        {
            bot->SetQuestSlot(slot, 0);

            // we ignore unequippable quest items in this case, its' still be equipped
            bot->TakeQuestSourceItem(logQuest, false);
            entry = logQuest;
            break;
        }
    }

    if (!entry)
        return false;

    bot->RemoveRewardedQuest(entry);
    bot->RemoveActiveQuest(entry, false);

    if (botAI->HasStrategy("debug quest", BotState::BOT_STATE_NON_COMBAT) || botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
    {
        Quest const* pQuest = sObjectMgr->GetQuestTemplate(entry);
        const std::string text_quest = ChatHelper::FormatQuest(pQuest);
        LOG_INFO("playerbots", "{} => Quest [ {} ] removed", bot->GetName(), pQuest->GetTitle());
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_removed_debug",
            "Quest [%quest] removed",
            {{"%quest", text_quest}});
        bot->Say(text, LANG_UNIVERSAL);
    }

    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "quest_remove", "Quest removed", {}));
    return true;
}

bool CleanQuestLogAction::Execute(Event event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    if (!sPlayerbotAIConfig.DropObsoleteQuests)
        return false;

    // Only output this message if "debug rpg" strategy is enabled
    if (botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "clean_quest_log_started",
            "Clean Quest Log command received, removing grey/trivial quests...",
            {}));

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        // [judge14] one grey judge: the core's own (Player::GetQuestLevel, which falls back to the bot's level for a
        // quest of level 0 or -1, and Acore::XP::GetGrayLevel) behind GuildCoordinator::IsGreyQuest, plus the user's
        // grey rule (GreyQuestNeed: a chain prerequisite, a key, a real upgrade is kept). This read
        // Quest::GetQuestLevel raw and remapped only -1, so a level-0 quest was "botLevel - 0" trivial and hard
        // deleted; and its hand table capped the difference at 8 where GetZeroDifference runs to 17, so quests the
        // rest of the module calls grey were not grey here. The same file already asks the core 80 lines below.
        if (sGuildCoordinator.IsGreyQuest(bot, quest) && sGuildCoordinator.GreyQuestNeed(bot, quest).empty())
        {
            // Output only if "debug rpg" strategy is enabled
            if (botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
                botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "quest_trivial_will_remove",
                    "Quest [%title] will be removed because it is trivial (grey).",
                    {{"%title", quest->GetTitle()}}));

            // Remove quest
            botAI->RecordQuestDropped(questId, QuestDropReason::Trivial);
            bot->SetQuestSlot(slot, 0);
            bot->TakeQuestSourceItem(questId, false);
            bot->SetQuestStatus(questId, QUEST_STATUS_NONE);
            bot->RemoveRewardedQuest(questId);

            if (botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
            {
                const std::string text_quest = ChatHelper::FormatQuest(quest);
                LOG_INFO("playerbots", "{} => Quest [ {} ] removed", bot->GetName(), quest->GetTitle());
                std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "quest_removed_debug",
                    "Quest [%quest] removed",
                    {{"%quest", text_quest}});
                bot->Say(text, LANG_UNIVERSAL);
            }

            if (botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
                botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "quest_has_been_removed",
                    "Quest [%title] has been removed.",
                    {{"%title", quest->GetTitle()}}));
        }
        else
        {
            // Only output if "debug rpg" strategy is enabled
            if (botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
                botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "quest_not_trivial_kept",
                    "Quest [%title] is not trivial and will be kept.",
                    {{"%title", quest->GetTitle()}}));
        }
    }

    return true;
}

void CleanQuestLogAction::DropQuestType(uint8& numQuest, uint8 wantNum, bool isGreen, bool hasProgress, bool isComplete)
{
    std::vector<uint8> slots;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        slots.push_back(slot);

    if (wantNum < 100)
    {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(slots.begin(), slots.end(), g);
    }

    for (uint8 slot : slots)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        // Do not drop class quest, may be not rewarding gold but important spells
        if (quest->GetRequiredClasses())
            continue;

        if (wantNum == 100)
            numQuest++;

        int32 lowLevelDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
        if (lowLevelDiff < 0 ||
            bot->GetLevel() <= bot->GetQuestLevel(quest) + uint32(lowLevelDiff))  // Quest is not gray
        {
            if (bot->GetLevel() + 5 > bot->GetQuestLevel(quest))  // Quest is not red
            {
                if (!isGreen)
                    continue;
            }
        }
        else  // Quest is gray
        {
            if (isGreen)
                continue;
        }

        if (HasProgress(bot, quest) && !hasProgress && bot->GetQuestStatus(questId) != QUEST_STATUS_FAILED)
            continue;

        if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !isComplete)
            continue;

        // Always drop failed quests
        if (numQuest <= wantNum && bot->GetQuestStatus(questId) != QUEST_STATUS_FAILED)
            continue;

        // Drop quest.
        bot->SetQuestSlot(slot, 0);

        // We ignore unequippable quest items in this case, its' still be equipped
        bot->TakeQuestSourceItem(questId, false);

        bot->SetQuestStatus(questId, QUEST_STATUS_NONE);
        bot->RemoveRewardedQuest(questId);

        numQuest--;

        if (botAI->HasStrategy("debug quest", BotState::BOT_STATE_NON_COMBAT) || botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
        {
            const std::string text_quest = ChatHelper::FormatQuest(quest);
            LOG_INFO("playerbots", "{} => Quest [ {} ] removed", bot->GetName(), quest->GetTitle());
            std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "quest_removed_debug",
                "Quest [%quest] removed",
                {{"%quest", text_quest}});
            bot->Say(text, LANG_UNIVERSAL);
        }
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_removed_with_name",
            "Quest removed %quest",
            {{"%quest", chat->FormatQuest(quest)}}));
    }
}

bool CleanQuestLogAction::HasProgress(Player* bot, Quest const* quest)
{
    uint32 questId = quest->GetQuestId();

    if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
        return true;

    QuestStatusData questStatus = bot->getQuestStatusMap()[questId];

    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        if (!quest->ObjectiveText[i].empty())
            return true;

        if (quest->RequiredItemId[i])
        {
            int required = quest->RequiredItemCount[i];
            int available = questStatus.ItemCount[i];
            if (available > 0 && required > 0)
                return true;
        }

        if (quest->RequiredNpcOrGo[i])
        {
            int required = quest->RequiredNpcOrGoCount[i];
            int available = questStatus.CreatureOrGOCount[i];

            if (available > 0 && required > 0)
                return true;
        }
    }

    return false;
}
