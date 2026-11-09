/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotRepository.h"
#include "PlayerbotsDatabase.h"
#include "AiObjectContext.h"
#include "StringFormat.h"
#include "SoloSafety.h"  // [guild15]
#include "NewRpgBaseAction.h"  // [guild15]
#include "QuestStats.h"  // [qeff31]

void PlayerbotRepository::Load(PlayerbotAI* botAI)
{
    ObjectGuid::LowType guid = botAI->GetBot()->GetGUID().GetCounter();

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_DB_STORE);
    stmt->SetData(0, guid);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        std::vector<std::string> values;
        do
        {
            Field* fields = result->Fetch();
            std::string const key = fields[0].Get<std::string>();
            std::string const value = fields[1].Get<std::string>();

            if (key == "value")
                values.push_back(value);
            else if (key == "co")
            {
                botAI->ClearStrategies(BOT_STATE_COMBAT);
                botAI->ChangeStrategy("+chat", BOT_STATE_COMBAT);
                botAI->ChangeStrategy(value, BOT_STATE_COMBAT);
            }
            else if (key == "nc")
            {
                botAI->ClearStrategies(BOT_STATE_NON_COMBAT);
                botAI->ChangeStrategy("+chat", BOT_STATE_NON_COMBAT);
                botAI->ChangeStrategy(value, BOT_STATE_NON_COMBAT);
            }
            else if (key == "dead")
                botAI->ChangeStrategy(value, BOT_STATE_DEAD);
            else if (key == "solo")  // [guild15] its measured solo damage
                SoloSafety::LoadMeasured(botAI->GetBot(), value);
            else if (key == "dz")  // [guild15] its death zones
                NewRpgBaseAction::LoadDeathZones(botAI->GetBot(), value);
            else if (key.compare(0, 2, "qa") == 0)  // [qeff31] a quest's accept time
                QuestStats::LoadAccepted(botAI->GetBot(), key, value);
        } while (result->NextRow());

        botAI->GetAiObjectContext()->GetUntypedValue("outfit list");

        botAI->GetAiObjectContext()->Load(values);
    }
}

void PlayerbotRepository::Save(PlayerbotAI* botAI)
{
    ObjectGuid::LowType guid = botAI->GetBot()->GetGUID().GetCounter();

    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();

    // [guild15] only the keys this save writes: the solo judge's measured damage ("solo") and death zones ("dz")
    // are kept by their own writers
    trans->Append(Acore::StringFormat(
        "DELETE FROM playerbots_db_store WHERE guid = {} AND `key` IN ('value', 'co', 'nc', 'dead')", guid));

    std::vector<std::string> data = botAI->GetAiObjectContext()->Save();
    for (std::vector<std::string>::iterator i = data.begin(); i != data.end(); ++i)
    {
        SaveValue(trans, guid, "value", *i);
    }

    SaveValue(trans, guid, "co", FormatStrategies("co", botAI->GetStrategies(BOT_STATE_COMBAT)));
    SaveValue(trans, guid, "nc", FormatStrategies("nc", botAI->GetStrategies(BOT_STATE_NON_COMBAT)));
    SaveValue(trans, guid, "dead", FormatStrategies("dead", botAI->GetStrategies(BOT_STATE_DEAD)));

    PlayerbotsDatabase.DirectCommitTransaction(trans);
}

std::string const PlayerbotRepository::FormatStrategies(std::string const /*type*/, std::vector<std::string> strategies)
{
    std::ostringstream out;
    for (std::vector<std::string>::iterator i = strategies.begin(); i != strategies.end(); ++i)
        out << "+" << (*i).c_str() << ",";

    std::string const res = out.str();
    return res.substr(0, res.size() - 1);
}

void PlayerbotRepository::Reset(PlayerbotAI* botAI)
{
    ObjectGuid::LowType guid = botAI->GetBot()->GetGUID().GetCounter();

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_DB_STORE);
    stmt->SetData(0, guid);
    PlayerbotsDatabase.DirectExecute(stmt);
}

void PlayerbotRepository::SaveValue(PlayerbotsDatabaseTransaction trans, uint32 guid, std::string const key,
                                    std::string const value)
{
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_DB_STORE);
    stmt->SetData(0, guid);
    stmt->SetData(1, key);
    stmt->SetData(2, value);
    trans->Append(stmt);
}
