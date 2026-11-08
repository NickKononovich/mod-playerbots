/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoMaintenanceOnLevelupAction.h"
#include "BroadcastHelper.h"
#include "GuildHonest.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellMgr.h"

#include <cstring>

bool AutoMaintenanceOnLevelupAction::Execute(Event /*event*/)
{
    AutoPickTalents();
    AutoLearnSpell();
    AutoTeleportForLevel();
    AutoUpgradeEquip();

    return true;
}

void AutoMaintenanceOnLevelupAction::AutoTeleportForLevel()
{
    if (GuildHonest::Skip(bot, "level-up teleport"))  // [honest]
        return;
    if (!sPlayerbotAIConfig.AutoTeleportForLevel || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    if (botAI->HasGameClientMaster())
        return;

    sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
    return;
}

void AutoMaintenanceOnLevelupAction::AutoPickTalents()
{
    bool const isRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot);
    if (!(isRandomBot ? sPlayerbotAIConfig.AutoPickTalents
                      : sPlayerbotAIConfig.AutoPickTalentsForPlayerControlledBots))
        return;

    if (bot->GetFreeTalentPoints() <= 0)
        return;

    PlayerbotFactory factory(bot, bot->GetLevel());
    // [honest] earned points only: a guild-plan bot's spent talents are not reset for free (a respec costs gold)
    factory.InitTalentsTree(true, true, isRandomBot && !GuildHonest::IsHonestBot(bot));
    factory.InitPetTalents();
}

void AutoMaintenanceOnLevelupAction::AutoLearnSpell()
{
    std::ostringstream out;
    LearnSpells(&out);

    if (!out.str().empty())
    {
        std::string const temp = out.str();
        out.seekp(0);
        out << "Learned spells: ";
        out << temp;
        out.seekp(-2, out.cur);
        out << ".";
        botAI->TellMaster(out);
    }
    return;
}

void AutoMaintenanceOnLevelupAction::LearnSpells(std::ostringstream* out)
{
    BroadcastHelper::BroadcastLevelup(botAI, bot);
    // [honest] a guild-plan random bot (groups B / C) takes the guild path below: the random-bot path gives free
    // riding, weapon skills, professions, tools and a hunter pet (InitSkills / InitAvailableSpells / InitPet)
    if (sPlayerbotAIConfig.AutoLearnTrainerSpells && sRandomPlayerbotMgr.IsRandomBot(bot) &&
        !GuildHonest::IsHonestBot(bot))
        LearnTrainerSpells(out);
    // [guild15] the guild project bots (not random, no client master): their class trainers' spells, no professions
    // (learned at a trainer), no weapon skills, no free pet (it tames its own)
    else if (sPlayerbotAIConfig.AutoLearnTrainerSpells &&
             (!botAI->HasGameClientMaster() || GuildHonest::IsHonestBot(bot)))
        LearnGuildClassSpells(bot, "level up");

    // [guild13] the guild project bots (not random, no client master) skip the spell-unlock class quests: their
    // spells come here too (the hunter's Tame Beast / Call Pet at 10)
    if (sPlayerbotAIConfig.AutoLearnQuestSpells &&
        (sRandomPlayerbotMgr.IsRandomBot(bot) || !botAI->HasGameClientMaster()))
        LearnQuestSpells(out);
}

void AutoMaintenanceOnLevelupAction::LearnTrainerSpells(std::ostringstream* /*out*/)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitSkills();
    factory.InitClassSpells();
    factory.InitAvailableSpells();
    factory.InitPet();
}

uint32 AutoMaintenanceOnLevelupAction::LearnGuildClassSpells(Player* bot, char const* why)
{
    if (!bot)
        return 0;
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitClassSpells();
    std::vector<std::string> learned;
    uint32 const count = factory.InitClassTrainerSpells(&learned);
    if (count)
    {
        std::string names;
        for (std::string const& name : learned)
            names += (names.empty() ? "" : ", ") + name;
        LOG_INFO("playerbots", "[guild15] {} L{} learned {} class trainer spells ({}): {}", bot->GetName(),
                 bot->GetLevel(), count, why, names);
    }
    // [honest] user 2026-11-08: the class's weapon skills are part of the auto-learn, at their start value (1)
    std::vector<std::string> skills;
    uint32 const skillCount = factory.InitWeaponMasterSkills(&skills);
    std::string skillNames;
    for (std::string const& name : skills)
        skillNames += (skillNames.empty() ? "" : ", ") + name;
    // every login (the proof per bot), at level up when something was learned
    if (skillCount || count || std::strcmp(why, "login") == 0)
        LOG_INFO("playerbots", "[honest] {} L{} learned {} class spells, {} skills ({}){}{}", bot->GetName(),
                 bot->GetLevel(), count, skillCount, why, skillCount ? ": " : "", skillNames);
    return count;
}

void AutoMaintenanceOnLevelupAction::LearnQuestSpells(std::ostringstream* out)
{
    ObjectMgr::QuestMap const& questTemplates = sObjectMgr->GetQuestTemplates();
    for (ObjectMgr::QuestMap::const_iterator i = questTemplates.begin(); i != questTemplates.end(); ++i)
    {
        Quest const* quest = i->second;

        if (!quest->GetRequiredClasses() || quest->IsRepeatable() || quest->GetMinLevel() < 10 ||
            quest->GetMinLevel() > bot->GetLevel())
        {
            continue;
        }

        if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false) ||
            !bot->SatisfyQuestSkill(quest, false))
        {
            continue;
        }

        int32 spellId = quest->GetRewSpellCast();
        if (!spellId)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        bool found = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL && spellInfo->Effects[i].TriggerSpell &&
                !bot->HasSpell(spellInfo->Effects[i].TriggerSpell))
            {
                if (SpellInfo const* triggeredInfo = sSpellMgr->GetSpellInfo(spellInfo->Effects[i].TriggerSpell))
                    if (triggeredInfo->Effects[0].Effect == SPELL_EFFECT_TRADE_SKILL)
                        break;

                found = true;
                break;
            }
        }

        if (!found)
            continue;

        bot->CastSpell(bot, spellId, true);

        uint32 rewSpellId = quest->GetRewSpell();
        if (rewSpellId)
        {
            if (SpellInfo const* rewSpellInfo = sSpellMgr->GetSpellInfo(rewSpellId))
            {
                *out << FormatSpell(rewSpellInfo) << ", ";
                continue;
            }
        }

        *out << FormatSpell(spellInfo) << ", ";
    }
}

std::string const AutoMaintenanceOnLevelupAction::FormatSpell(SpellInfo const* sInfo)
{
    std::ostringstream out;
    std::string const rank = sInfo->Rank[0];

    if (rank.empty())
        out << "|cffffffff|Hspell:" << sInfo->Id << "|h[" << sInfo->SpellName[LOCALE_enUS] << "]|h|r";
    else
        out << "|cffffffff|Hspell:" << sInfo->Id << "|h[" << sInfo->SpellName[LOCALE_enUS] << " " << rank << "]|h|r";

    return out.str();
}

void AutoMaintenanceOnLevelupAction::AutoUpgradeEquip()
{
    if (GuildHonest::Skip(bot, "level-up supplies and gear (AutoUpgradeEquip)"))  // [honest]
        return;
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    // [guild1] no free ammo / reagents / food / consumables / potions on level up
    if (!sPlayerbotAIConfig.RandomBotLevelupSupplies)
        return;

    PlayerbotFactory factory(bot, bot->GetLevel());

    factory.CleanupConsumables();

    factory.InitAmmo();
    factory.InitReagents();
    factory.InitFood();
    factory.InitConsumables();
    factory.InitPotions();

    if (sPlayerbotAIConfig.AutoUpgradeEquip)
        factory.InitEquipment(true);
}
