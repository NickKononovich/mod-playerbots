/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyMemberToResurrect.h"
#include "BotPath.h"  // [lead109]

#include "Battleground.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"

class IsTargetOfResurrectSpell : public SpellEntryPredicate
{
public:
    bool Check(SpellInfo const* spellInfo) override
    {
        for (uint8 i = 0; i < 3; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_RESURRECT ||
                spellInfo->Effects[i].Effect == SPELL_EFFECT_RESURRECT_NEW ||
                spellInfo->Effects[i].Effect == SPELL_EFFECT_SELF_RESURRECT)
                return true;
        }

        return false;
    }
};

class FindDeadPlayer : public FindPlayerPredicate
{
public:
    FindDeadPlayer(PartyMemberValue* value) : value(value) {}

    bool Check(Unit* unit) override
    {
        Player* player = unit->ToPlayer();
        return player && !player->isResurrectRequested() && player->getDeathState() == DeathState::Corpse &&
               !value->IsTargetOfSpellCast(player, predicate);
    }

private:
    PartyMemberValue* value;
    IsTargetOfResurrectSpell predicate;
};

namespace
{
    // Out of combat a resurrecter walks to a corpse this far, on a complete walk (Violet Hold wave 18: the mage lay
    // on the ledge 66-92 yd from the group, outside the 2 x SpellDistance window; nobody tried in three quiet spells
    // of 13-23 s, the mage released after 2 minutes into a Dalaran it could not walk).
    constexpr float RESURRECT_WALK_RANGE = 100.0f;

    enum ResurrectRank : uint8
    {
        RANK_HEALER_RESURRECTER = 0,  // brings back more resurrecters
        RANK_RESURRECTER = 1,
        RANK_TANK = 2,
        RANK_LEADER = 3,
        RANK_OTHER = 4
    };

    uint8 RankOf(Player* player, Player* leader)
    {
        bool const resurrecter = RaidLeader::CanResurrect(player);
        if (resurrecter && PlayerbotAI::IsHeal(player))
            return RANK_HEALER_RESURRECTER;
        if (resurrecter)
            return RANK_RESURRECTER;
        if (PlayerbotAI::IsTank(player))
            return RANK_TANK;
        if (player == leader)
            return RANK_LEADER;
        return RANK_OTHER;
    }
}

// In combat (battle resurrection) a corpse in reach and in sight; out of combat one the resurrecter can walk to
// ("reach party member to resurrect" walks it into spell range first).
bool PartyMemberToResurrect::Check(Unit* player)
{
    if (PartyMemberValue::Check(player))
        return true;
    if (!player || bot->IsInCombat() || player->GetMapId() != bot->GetMapId() ||
        (player->ToPlayer() && player->ToPlayer()->IsGameMaster()) ||
        bot->GetDistance(player) >= RESURRECT_WALK_RANGE)
        return false;

    BotPath const path =  // [lead109]
        FindBotPath(bot, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    return path.type == PATHFIND_NORMAL;
}

// After a wipe: healers who can resurrect first, then other resurrecters, tanks, the leader, and the rest.
Unit* PartyMemberToResurrect::Calculate()
{
    Battleground* bg = bot->GetBattleground();
    if (bg && bg->GetBgTypeID(true) == BATTLEGROUND_WS)
        return nullptr;

    FindDeadPlayer finder(this);
    Group* group = bot->GetGroup();
    if (!group)
        return FindPartyMember(finder);

    // The leader named who comes first ("res <name>"); the order holds until that member is up.
    if (ObjectGuid const first = RaidLeader::GetResurrectFirst(bot))
    {
        Player* member = ObjectAccessor::FindPlayer(first);
        if (!member || member->IsAlive() || !group->IsMember(first))
            RaidLeader::SetResurrectFirst(bot, ObjectGuid::Empty);
        else if (member != bot && member->IsInWorld() && finder.Check(member) && Check(member))
            return member;
    }

    Player* leader = botAI->GetGroupLeader();
    Player* best = nullptr;
    uint8 bestRank = RANK_OTHER + 1;
    float bestDistance = 0.0f;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !finder.Check(member) || !Check(member))
            continue;

        uint8 const rank = RankOf(member, leader);
        float const distance = bot->GetDistance(member);
        if (!best || rank < bestRank || (rank == bestRank && distance < bestDistance))
        {
            best = member;
            bestRank = rank;
            bestDistance = distance;
        }
    }

    return best;
}
