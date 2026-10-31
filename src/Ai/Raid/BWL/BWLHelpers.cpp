/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BWLHelpers.h"
#include "AiObjectContext.h"
#include "GameObject.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include <limits>
#include <list>

namespace BlackwingLairHelpers
{
    bool IsActiveSuppressionDeviceInRange(GameObject const* go, Player const* bot)
    {
        constexpr float suppressionDeviceInteractionDistance = 15.0f;
        return go &&
               go->GetEntry() == static_cast<uint32>(BlackwingLairGameObjects::GO_SUPPRESSION_DEVICE) &&
               go->GetDistance(bot) < suppressionDeviceInteractionDistance &&
               go->GetGoState() == GO_STATE_READY;
    }

    bool AreRazorgoreEggsAlive(PlayerbotAI* botAI)
    {
        GuidVector gos = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest game objects")->Get();
        for (auto const& guid : gos)
        {
            GameObject const* go = botAI->GetGameObject(guid);
            if (go && go->GetEntry() == static_cast<uint32>(BlackwingLairGameObjects::GO_BLACK_DRAGON_EGG))
                return true;
        }
        return false;
    }

    bool IsRazorgoreOffTank(Player* bot)
    {
        return PlayerbotAI::IsAssistTankOfIndex(bot, 0, true);
    }

    Unit* FindRazorgore(Player* bot)
    {
        return bot->FindNearestCreature(
            static_cast<uint32>(BlackwingLairNPCs::NPC_RAZORGORE), RAZORGORE_SEARCH_RADIUS, true);
    }

    bool IsControllingRazorgore(Player* bot)
    {
        Unit* charm = bot->GetCharm();
        return charm && charm->GetEntry() == static_cast<uint32>(BlackwingLairNPCs::NPC_RAZORGORE);
    }

    // go_orb_of_domination refuses anyone with a pet or Mind Exhaustion. Hunters and Warlocks are
    // left out even with the pet down because the bots summon it straight back. Tanks and healers
    // keep their jobs, since any cast of the controller ends the possession channel.
    bool CanUseOrbOfDomination(Player* member)
    {
        if (!member || !member->IsAlive() || member->GetMapId() != BWL_MAP_ID || !GET_PLAYERBOT_AI(member))
            return false;

        if (PlayerbotAI::IsTank(member) || PlayerbotAI::IsHeal(member) ||
            member->getClass() == CLASS_HUNTER || member->getClass() == CLASS_WARLOCK)
        {
            return false;
        }

        return !member->GetPet() &&
            !member->HasAura(static_cast<uint32>(BlackwingLairSpells::SPELL_MIND_EXHAUSTION));
    }

    // The player holding the possession keeps it; otherwise the first eligible melee dps in group
    // order, then the first eligible ranged dps. Every bot evaluates the same order, so only one
    // walks to the orb, and the next one takes over when the previous gets Mind Exhaustion.
    Player* GetRazorgoreOrbController(Player* bot, Unit* razorgore)
    {
        if (!razorgore)
            return nullptr;

        if (razorgore->IsCharmed())
            return ObjectAccessor::GetPlayer(*razorgore, razorgore->GetCharmerGUID());

        Group const* group = bot->GetGroup();
        if (!group)
            return CanUseOrbOfDomination(bot) ? bot : nullptr;

        Player* rangedController = nullptr;
        for (GroupReference const* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (!CanUseOrbOfDomination(member))
                continue;

            if (PlayerbotAI::IsMelee(member))
                return member;

            if (!rangedController)
                rangedController = member;
        }

        return rangedController;
    }

    GameObject* FindNearestIntactEgg(Unit* razorgore)
    {
        std::list<GameObject*> eggs;
        constexpr float eggSearchRadius = 120.0f;
        razorgore->GetGameObjectListWithEntryInGrid(
            eggs, static_cast<uint32>(BlackwingLairGameObjects::GO_BLACK_DRAGON_EGG), eggSearchRadius);

        GameObject* nearest = nullptr;
        float nearestDistance = std::numeric_limits<float>::max();
        for (GameObject* egg : eggs)
        {
            // A broken egg stays spawned for 10s in GO_STATE_ACTIVE (spell_egg_event).
            if (!egg || !egg->isSpawned() || egg->GetGoState() != GO_STATE_READY)
                continue;

            float const distance = razorgore->GetExactDist(egg);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearest = egg;
            }
        }

        return nearest;
    }

    bool IsNonBABotNearPosition(Player const* bot, Position const& position, float distance)
    {
        Group const* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference const* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player const* p = gref->GetSource();
            if (!p || p == bot || !p->IsAlive() || p->GetMapId() != bot->GetMapId())
                continue;

            if (p->HasAura(static_cast<uint32>(BlackwingLairSpells::SPELL_BURNING_ADRENALINE)))
                continue;

            if (p->GetDistance2d(position.GetPositionX(), position.GetPositionY()) < distance)
                return true;
        }

        return false;
    }

    bool IsCorruptedWhelp(Unit const* unit)
    {
        if (!unit)
            return false;

        const uint32 entry = unit->GetEntry();
        return entry == static_cast<uint32>(BlackwingLairNPCs::NPC_CORRUPTED_RED_WHELP) ||
            entry == static_cast<uint32>(BlackwingLairNPCs::NPC_CORRUPTED_GREEN_WHELP) ||
            entry == static_cast<uint32>(BlackwingLairNPCs::NPC_CORRUPTED_BLUE_WHELP) ||
            entry == static_cast<uint32>(BlackwingLairNPCs::NPC_CORRUPTED_BRONZE_WHELP);
    }

    bool IsCorruptedTotemEntry(uint32 entry)
    {
        return entry == static_cast<uint32>(BlackwingLairNPCs::NPC_CORRUPTED_FIRE_NOVA_TOTEM) ||
            entry == static_cast<uint32>(BlackwingLairNPCs::NPC_CORRUPTED_STONESKIN_TOTEM) ||
            entry == static_cast<uint32>(BlackwingLairNPCs::NPC_CORRUPTED_HEALING_TOTEM) ||
            entry == static_cast<uint32>(BlackwingLairNPCs::NPC_CORRUPTED_WINDFURY_TOTEM);
    }

    bool IsCorruptedTotem(Unit const* unit)
    {
        return unit && IsCorruptedTotemEntry(unit->GetEntry());
    }

    Unit* FindCorruptedTotem(PlayerbotAI* botAI)
    {
        // Ranged take the nearest totem, melee only one they can already reach.
        Player* bot = botAI->GetBot();
        Unit* nearest = nullptr;
        float nearestDistance = 0.0f;
        for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->RefGet())
        {
            if (!IsCorruptedTotemEntry(guid.GetEntry()))
                continue;

            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || (!bot->IsWithinMeleeRange(unit) && !PlayerbotAI::IsRanged(bot)))
                continue;

            float const distance = bot->GetExactDist(unit);
            if (!nearest || distance < nearestDistance)
            {
                nearest = unit;
                nearestDistance = distance;
            }
        }
        return nearest;
    }
}
