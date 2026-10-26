/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONINTERACT_H
#define PLAYERBOTS_DUNGEONINTERACT_H

#include "GameObject.h"
#include "GossipDef.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <functional>
#include <string>  // [b32_pull] GroupLaggers

// Honest dungeon interactions for client-less groups: event gossips, GO clicks and area triggers go through the
// bot's own session handlers exactly as a client's packets would, so every server-side check still applies.
// Callers check the instance / encounter / GO state first, so a start already made by someone else is harmless.
namespace DungeonInteract
{
inline bool IsActiveMember(Player* bot, Player* member)
{
    return member && member->IsAlive() && member->IsInWorld() && !member->IsDuringRemoveFromWorld() &&
           member->GetMapId() == bot->GetMapId() && member->GetInstanceId() == bot->GetInstanceId();
}

// The one group member that performs one-shot interactions: the group leader when AI-driven (bot or SelfBot),
// else the lowest-GUID AI-driven member in the instance. nullptr while a person drives a member through the
// client (that person starts events) or when nobody qualifies.
inline Player* GetInteractor(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Group* group = bot->GetGroup();
    if (!group)
        return bot->IsAlive() ? bot : nullptr;

    Player* first = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (IsRealPlayer(member))
            return nullptr;

        if (IsActiveMember(bot, member) && (!first || member->GetGUID() < first->GetGUID()))
            first = member;
    }

    if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()); IsActiveMember(bot, leader))
        return leader;

    return first;
}

inline bool IsInteractor(PlayerbotAI* botAI) { return GetInteractor(botAI) == botAI->GetBot(); }

// Every alive member in this instance is within radius of the anchor and out of combat.
inline bool IsGroupGathered(Player* bot, WorldObject const* anchor, float radius)
{
    if (!anchor)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return !bot->IsInCombat() && bot->IsWithinDistInMap(anchor, radius);

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (!member->IsAlive())
            continue;

        if (member->IsInCombat() || !member->IsWithinDistInMap(anchor, radius))
            return false;
    }
    return true;
}

// [b32_pull] A walk by the group leader that leads the group somewhere (a dungeon event step, the return to a waiting
// escort) goes with the group, as players do: the laggers are the alive members behind the bot (not nearer the
// destination than it) that are farther than radius from it, or a healer behind out of its line of sight. Members
// ahead never hold it. Empty: the group is with the bot, or the bot leads nobody (no group / not the leader). The
// planner pulls the packs on the way meanwhile (its pull outranks these walks). ohb-h 20261025-221216 1162-1177 s:
// the tank walked ~50 yd alone from (2172,131) round a corner to Thrall's cell while the healer drank at (2176,122)
// without line of sight and pulled Durnholde Riflemen alone (pull 22 "unplanned (tank's dungeon event step)").
inline std::string GroupLaggers(Player* bot, float destX, float destY, float destZ, float radius)
{
    std::string laggers;
    Group* group = bot->GetGroup();
    if (!group || group->GetLeaderGUID() != bot->GetGUID())
        return laggers;

    constexpr float AHEAD_MARGIN = 5.0f;
    float const botToDest = bot->GetExactDist(destX, destY, destZ);
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member == bot || !IsActiveMember(bot, member) ||
            member->GetExactDist(destX, destY, destZ) + AHEAD_MARGIN < botToDest)
            continue;

        bool const tooFar = !bot->IsWithinDistInMap(member, radius);
        bool const blind = PlayerbotAI::IsHeal(member) && !bot->IsWithinLOSInMap(member);
        if (!tooFar && !blind)
            continue;

        if (!laggers.empty())
            laggers += ",";
        laggers += member->GetName() + (tooFar ? "(far)" : "(no los)");
    }
    return laggers;
}

// Sends CMSG_GOSSIP_HELLO; true when the server opened a menu from this source.
inline bool OpenGossip(Player* bot, WorldObject* source)
{
    if (!source)
        return false;

    bot->SetFacingToObject(source);
    WorldPacket hello(CMSG_GOSSIP_HELLO);
    hello << source->GetGUID();
    bot->GetSession()->HandleGossipHelloOpcode(hello);
    return bot->PlayerTalkClass->GetGossipMenu().GetSenderGUID() == source->GetGUID();
}

// List id of the first option of the open menu matching the predicate (sender, action), or -1.
inline int32 FindGossipOption(Player* bot, std::function<bool(GossipMenuItem const&)> const& match)
{
    for (auto const& [listId, item] : bot->PlayerTalkClass->GetGossipMenu().GetMenuItems())
        if (match(item))
            return static_cast<int32>(listId);
    return -1;
}

// Sends CMSG_GOSSIP_SELECT_OPTION for an option of the menu currently open from this source.
inline bool SelectGossipOption(Player* bot, WorldObject* source, int32 listId)
{
    if (!source || listId < 0)
        return false;

    GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
    if (menu.GetSenderGUID() != source->GetGUID() || !menu.GetItem(static_cast<uint32>(listId)))
        return false;

    WorldPacket select(CMSG_GOSSIP_SELECT_OPTION);
    select << source->GetGUID() << uint32(menu.GetMenuId()) << uint32(listId);
    bot->GetSession()->HandleGossipSelectOptionOpcode(select);
    return true;
}

// Opens the source's gossip and picks the first offered option; true when an option was selected.
inline bool SelectFirstGossipOption(Player* bot, WorldObject* source)
{
    if (!OpenGossip(bot, source))
        return false;

    int32 const option = FindGossipOption(bot, [](GossipMenuItem const&) { return true; });
    bool const selected = SelectGossipOption(bot, source, option);
    bot->PlayerTalkClass->SendCloseGossip();
    return selected;
}

// CMSG_GAMEOBJ_USE through the session: the handler checks spawn state and interaction distance.
inline bool UseGameObject(Player* bot, GameObject* go)
{
    if (!go || !go->isSpawned() || !go->IsWithinDistInMap(bot, go->GetInteractionDistance()))
        return false;

    bot->SetFacingToObject(go);
    WorldPacket use(CMSG_GAMEOBJ_USE);
    use << go->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(use);
    return true;
}

// CMSG_AREATRIGGER as the client sends it on entering the trigger volume; the handler rejects it unless the bot
// is really inside (IsInAreaTriggerRadius). Returns false when the bot is not inside yet.
inline bool SendAreaTrigger(Player* bot, uint32 triggerId)
{
    AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
    if (!at || at->map != bot->GetMapId() || !bot->IsInAreaTriggerRadius(at))
        return false;

    WorldPacket packet(CMSG_AREATRIGGER);
    packet << triggerId;
    bot->GetSession()->HandleAreaTriggerOpcode(packet);
    return true;
}
}  // namespace DungeonInteract

#endif
