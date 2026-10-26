/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CcTargetValue.h"
#include "Action.h"
#include "AiObjectContext.h"
#include "Group.h"
#include "KillOrder.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RtiValue.h"
#include "ServerFacade.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

class FindTargetForCcStrategy : public FindTargetStrategy
{
public:
    FindTargetForCcStrategy(PlayerbotAI* botAI, std::string const spell, bool peacefulTargetsOnly, bool breakable)
        : FindTargetStrategy(botAI), spell(spell), maxDistance(0.f), peacefulTargetsOnly(peacefulTargetsOnly),
          breakable(breakable)
    {
    }

public:
    void CheckAttacker(Unit* creature, ThreatManager* /*threatMgr*/) override
    {
        Player* bot = botAI->GetBot();
        // Sap and the like only open a pull: never once the bot or the target is fighting.
        if (peacefulTargetsOnly && (bot->IsInCombat() || creature->IsInCombat()))
            return;

        // 5-man dungeons: never a mob deep in the room the group does not fight yet; it is held once it comes.
        if (!KillOrder::IsInGroupFight(bot, creature))
            return;

        if (!botAI->CanCastSpell(spell, creature))
            return;

        // [rot_b31_cc] never a mob whose crowd control broke early (back-off), nor one the group's own area damage or
        // a damage over time would break at once - the marked one either (build 30: 111 Polymorphs in a wave)
        if (bot->IsInCombat() && (IsCrowdControlBackedOff(creature) ||
                                  (breakable && CrowdControlWouldBreak(botAI, creature))))
            return;

        if (*botAI->GetAiObjectContext()->GetValue<Unit*>("rti cc target") == creature)
        {
            result = creature;
            return;
        }

        if (*botAI->GetAiObjectContext()->GetValue<Unit*>("current target") == creature)
            return;

        uint8 health = static_cast<uint8>(creature->GetHealthPct());
        if (health < sPlayerbotAIConfig.MediumHealth)
            return;

        float minDistance = botAI->GetRange("spell");
        Group* group = bot->GetGroup();
        if (!group)
            return;

        if (*botAI->GetAiObjectContext()->GetValue<uint8>("aoe count") > 2)
        {
            WorldLocation aoe = *botAI->GetAiObjectContext()->GetValue<WorldLocation>("aoe position");
            if (ServerFacade::instance().IsDistanceLessOrEqualThan(
                    ServerFacade::instance().GetDistance2d(creature, aoe.GetPositionX(), aoe.GetPositionY()),
                    sPlayerbotAIConfig.AoeRadius))
                return;
        }

        uint32 tankCount = 0;
        uint32 dpsCount = 0;
        GetPlayerCount(creature, &tankCount, &dpsCount);
        if (!tankCount || !dpsCount)
        {
            result = creature;
            return;
        }

        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            Player* member = ObjectAccessor::FindPlayer(itr->guid);
            if (!member || !member->IsAlive() || member == bot)
                continue;

            if (!PlayerbotAI::IsTank(member))
                continue;

            float distance = ServerFacade::instance().GetDistance2d(member, creature);
            if (distance < minDistance)
                minDistance = distance;
        }

        if (!result || minDistance > maxDistance)
        {
            result = creature;
            maxDistance = minDistance;
        }
    }

private:
    std::string const spell;
    float maxDistance;
    bool peacefulTargetsOnly;
    bool breakable;  // [rot_b31_cc] damage breaks the spell's hold (not Banish)
};

Unit* CcTargetValue::Calculate()
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", qualifier));
    bool const peacefulTargetsOnly = spellInfo && spellInfo->HasAttribute(SPELL_ATTR1_ONLY_PEACEFUL_TARGETS);
    bool const breakable = !spellInfo || !(spellInfo->GetAllEffectsMechanicMask() & (1ULL << MECHANIC_BANISH));
    FindTargetForCcStrategy strategy(botAI, qualifier, peacefulTargetsOnly, breakable);
    return FindTarget(&strategy);
}
