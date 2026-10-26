/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ConsumableActions.h"

#include "Event.h"
#include "GameObject.h"
#include "Playerbots.h"
#include "RaidConsumables.h"
#include "RaidLeaderState.h"

Item* UseValueItemAction::GetValueItem()
{
    ObjectGuid const guid = AI_VALUE(ObjectGuid, itemValue);
    return guid ? bot->GetItemByGuid(guid) : nullptr;
}

bool UseValueItemAction::Execute(Event /*event*/)
{
    Item* item = GetValueItem();
    if (!item)
        return false;

    bool const used = UseItemAuto(item);
    context->GetValue<ObjectGuid>(itemValue)->Reset();
    return used;
}

bool UseValueItemAction::isPossible() { return !bot->IsNonMeleeSpellCast(false) && GetValueItem(); }

bool UseRaidFlaskAction::isUseful()
{
    return bot->IsAlive() && !bot->IsInCombat() && !bot->IsMounted() && !AI_VALUE(bool, "encounter in progress");
}

bool UseFoodBuffAction::isUseful()
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || AI_VALUE(bool, "encounter in progress"))
        return false;

    bool drinking = false;
    return !RaidLeader::IsEatingOrDrinking(bot, drinking);
}

bool UsePrePullPotionAction::Execute(Event event)
{
    _usedForReadyMs = AI_VALUE(uint32, "pre pull window");
    return UseValueItemAction::Execute(event);
}

bool UsePrePullPotionAction::isUseful()
{
    uint32 const window = AI_VALUE(uint32, "pre pull window");
    return window && window != _usedForReadyMs;
}

bool UseHealthstoneAction::Execute(Event /*event*/)
{
    Item* healthstone = RaidConsumables::FindHealthstone(bot);
    return healthstone && UseItemAuto(healthstone);
}

bool UseHealthstoneAction::isPossible() { return RaidConsumables::FindHealthstone(bot); }

bool UseGameObjectNearbyAction::Execute(Event /*event*/)
{
    GameObject* go = botAI->GetGameObject(AI_VALUE(ObjectGuid, objectValue));
    if (!go || !go->isSpawned())
        return false;

    if (bot->GetDistance(go) > INTERACTION_DISTANCE - 1.0f)
        return MoveNear(go, INTERACTION_DISTANCE - 2.0f);

    if (bot->isMoving())
        bot->StopMoving();

    WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
    packet << go->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(packet);
    context->GetValue<ObjectGuid>(objectValue)->Reset();
    return true;
}

bool UseGameObjectNearbyAction::isUseful()
{
    return bot->IsAlive() && !bot->IsInCombat() && !bot->IsNonMeleeSpellCast(true) &&
           !AI_VALUE(ObjectGuid, objectValue).IsEmpty();
}
