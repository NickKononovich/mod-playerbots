/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "VHStrategy.h"
#include "ChooseTargetActions.h"
#include "MovementActions.h"
#include "Playerbots.h"
#include "VHMultipliers.h"
#include "VHTriggers.h"

#include <algorithm>

void WotlkDungeonVHStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Event: Sinclari start, portals, Prison Seal
    triggers.push_back(new TriggerNode("vh sinclari start",
        { NextAction("vh start event", ACTION_MOVE + 3) }));
    triggers.push_back(new TriggerNode("vh seal under attack",
        { NextAction("vh attack seal attacker", ACTION_RAID + 3) }));
    triggers.push_back(new TriggerNode("vh portal keeper",
        { NextAction("vh attack portal keeper", ACTION_RAID + 2) }));
    triggers.push_back(new TriggerNode("vh released boss",
        { NextAction("vh engage released boss", ACTION_RAID + 1) }));
    triggers.push_back(new TriggerNode("vh rally",
        { NextAction("vh rally", ACTION_MOVE + 1) }));
    // Back from a release, outside the closed seal: Sinclari sends the bot in (above the attacks and the rally,
    // which are off outside the seal anyway, and above following the group through the door).
    triggers.push_back(new TriggerNode("vh late join",
        { NextAction("vh late join", ACTION_RAID + 5) }));

    // Erekem
    // This boss has many purgable buffs, purging/dispels could be merged into generic strats though
    triggers.push_back(new TriggerNode("erekem target",
        { NextAction("attack erekem", ACTION_RAID + 1) }));

    // Moragg
    // TODO: This guy has Optic Link which may require moving, add if needed

    // Ichoron
    triggers.push_back(new TriggerNode("ichoron target",
        { NextAction("attack ichor globule", ACTION_RAID + 1) }));

    // Xevozz: the tank walks him away from the Ethereal Spheres (Arcane Power within 3 yd)
    triggers.push_back(new TriggerNode("xevozz ethereal sphere",
        { NextAction("xevozz kite", ACTION_RAID + 4) }));

    // Lavanthor
    // Tank & spank

    // Zuramat the Obliterator
    triggers.push_back(new TriggerNode("shroud of darkness",
        { NextAction("drop target", ACTION_HIGH + 5) }));
    triggers.push_back(new TriggerNode("void shift",
        { NextAction("attack void sentry", ACTION_RAID + 1) }));

    // Cyanigosa
    triggers.push_back(new TriggerNode("cyanigosa positioning",
        { NextAction("rear flank", ACTION_MOVE + 5) }));
}

void WotlkDungeonVHStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new ErekemMultiplier(botAI));
    multipliers.push_back(new IchoronMultiplier(botAI));
    multipliers.push_back(new ZuramatMultiplier(botAI));
}

// A damage dealer's "dps target" is the portal keeper while it fights the group, so "dps assist" (50) keeps the
// target that "vh attack portal keeper" (62) chose instead of switching it back to the trash every tick ("[idle
// diag] ... cast 0 ... actions dps assist:19 vh attack portal keeper:18").
void WotlkDungeonVHStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    Player* bot = botAI->GetBot();
    if (type != TargetValueExclusionType::DpsTarget || !bot->IsInCombat() || PlayerbotAI::IsTank(bot) ||
        !VHShared::IsPortalKeeperPhase(bot))
        return;

    Unit* keeper = VHShared::PortalKeeper(VHShared::ActiveKeeperPortal(bot));
    if (!keeper)
        return;

    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    if (std::find(attackers.begin(), attackers.end(), keeper->GetGUID()) == attackers.end())
        return;

    for (ObjectGuid const& guid : attackers)
        if (guid != keeper->GetGUID())
            exclusions.insert(guid);
}
