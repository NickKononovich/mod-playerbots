/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BMActions.h"
#include "BMShared.h"
#include "BMTriggers.h"
#include "Creature.h"
#include "DungeonInteract.h"
#include "Item.h"
#include "Playerbots.h"
#include "PullActions.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>  // [b32_bm]
#include <cmath>

namespace
{
constexpr float SAAT_TALK_DISTANCE = INTERACTION_DISTANCE - 1.0f;
constexpr float SAAT_SEARCH_RANGE = BMShared::SAAT_DETOUR_RANGE + 10.0f;
constexpr uint32 SAAT_RETRY_MS = 30 * IN_MILLISECONDS;
constexpr uint32 SAAT_NO_OPTION_RETRY_MS = 5 * MINUTE * IN_MILLISECONDS;
constexpr uint32 SAAT_VERIFY_MS = 5 * IN_MILLISECONDS;
constexpr uint32 BOSS_BUFF_RETRY_MS = 3 * IN_MILLISECONDS;
constexpr float FETCH_APPROACH_STEP = 5.0f;
constexpr float FETCH_MELEE_DISTANCE = 1.5f;

// Ranged openers of the tank classes (taunts first: they work on an idle keeper as on a fighting one).
constexpr char const* FETCH_OPENERS[] = {
    "hand of reckoning", "taunt",     "growl",    "dark command",        "avenger's shield",   "heroic throw",
    "faerie fire (feral)", "death grip", "icy touch", "judgement of wisdom", "judgement of light", "exorcism",
    "shoot",             "throw"};

// No spell from the saddle: a mounted tank stood 100 s at 20 yd from a keeper, "no ranged opener ready" (bm-h
// 2026-10-25 rift 4). The portals are on the ground: the mount is cancelled as a client does.
bool LeaveMount(Player* bot)
{
    if (!bot->IsMounted() || bot->IsFlying())
        return false;

    if (bot->isMoving())
        bot->StopMoving();
    WorldPacket emptyPacket;
    bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    return true;
}

std::string DescribeUnit(Unit* unit, Creature* medivh)
{
    if (!unit)
        return "?";
    return Acore::StringFormat("{} ({:.0f} yd from Medivh)", unit->GetName(),
                               medivh ? unit->GetExactDist2d(medivh) : -1.0f);
}
}  // namespace

bool BlackMorassDefendMedivhAction::Execute(Event /*event*/)
{
    Unit* threat = BMShared::ThreatFor(botAI);
    if (!threat)
        return false;

    Creature* medivh = BMShared::Medivh(bot);
    bool const corrupting = BMShared::IsCorruptingMedivh(medivh, threat);
    Unit* current = AI_VALUE(Unit*, "current target");

    // The tank tags a channeling add from range (a taunt breaks the channel and turns the add on it) and keeps the
    // target it holds; it switches to the add only when no opener is ready.
    if (PlayerbotAI::IsTank(bot) && corrupting)
    {
        if (LeaveMount(bot))
            return true;

        if (bot->GetExactDist(threat) <= BMShared::FETCH_PULL_RANGE && bot->IsWithinLOSInMap(threat))
            for (char const* opener : FETCH_OPENERS)
                if (botAI->CanCastSpell(opener, threat) && botAI->CastSpell(opener, threat))
                {
                    BMShared::Log(bot, "bm guard", threat->GetGUID().ToString() + ":tag",
                                  Acore::StringFormat("tags {} draining the shield with {}",
                                                      DescribeUnit(threat, medivh), opener));
                    return true;
                }
    }

    if (current == threat)
        return false;

    // Already on another add at Medivh: finish it, unless it is not draining him while this one is.
    if (current && BMShared::IsThreat(botAI, current) &&
        (BMShared::IsCorruptingMedivh(medivh, current) || !corrupting))
        return false;

    BMShared::Log(bot, "bm guard", threat->GetGUID().ToString(),
                  Acore::StringFormat("attacks {} {}, was on {}", DescribeUnit(threat, medivh),
                                      corrupting ? "draining the shield" : "near Medivh",
                                      current ? current->GetName() : std::string("nothing")));
    return Attack(threat);
}

bool BlackMorassKillRiftGuardianAction::Execute(Event /*event*/)
{
    Unit* guardian = BMShared::GuardianAtMedivh(botAI);
    if (!guardian)
        return false;

    Unit* current = AI_VALUE(Unit*, "current target");
    BMShared::Log(bot, "bm guardian", guardian->GetGUID().ToString() + ":kill",
                  Acore::StringFormat("attacks {}: no add channels, its rift closes when it dies, was on {}",
                                      DescribeUnit(guardian, BMShared::Medivh(bot)),
                                      current ? current->GetName() : std::string("nothing")));
    return Attack(guardian);
}

bool BlackMorassHoldNearMedivhAction::Execute(Event /*event*/)
{
    Creature* medivh = BMShared::Medivh(bot);
    if (!medivh)
        return false;

    Unit* current = AI_VALUE(Unit*, "current target");
    if (current && BMShared::IsFarGuardian(bot, current) && BMShared::HasLivingTank(bot))
    {
        // The tank brings the keeper to Medivh; the reach actions would walk the bot after it again.
        BMShared::Log(bot, "bm guardian", current->GetGUID().ToString(),
                      Acore::StringFormat("drops {}: the tank brings it to Medivh", DescribeUnit(current, medivh)));
        context->GetValue<Unit*>("current target")->Set(nullptr);
        bot->AttackStop();
    }

    float const angle = GetFollowAngle();
    float const x = BMShared::HOLD_X + std::cos(angle) * BMShared::HOLD_SPREAD;
    float const y = BMShared::HOLD_Y + std::sin(angle) * BMShared::HOLD_SPREAD;
    float z = BMShared::HOLD_Z;
    bot->UpdateAllowedPositionZ(x, y, z);

    BMShared::Log(bot, "bm hold", "back",
                  Acore::StringFormat("back to Medivh from {:.0f} yd", bot->GetExactDist2d(medivh)));
    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}

bool BlackMorassFetchRiftGuardianAction::Execute(Event /*event*/)
{
    Unit* guardian = BMShared::GuardianToFetch(botAI);
    if (!guardian)
        return false;

    Creature* medivh = BMShared::Medivh(bot);
    if (bot->GetExactDist(guardian) > BMShared::FETCH_PULL_RANGE || !bot->IsWithinLOSInMap(guardian))
    {
        BMShared::Log(bot, "bm pull", guardian->GetGUID().ToString() + ":walk",
                      Acore::StringFormat("walks to {} to bring it to Medivh", DescribeUnit(guardian, medivh)));
        if (MoveTo(guardian, BMShared::FETCH_PULL_RANGE - FETCH_APPROACH_STEP, MovementPriority::MOVEMENT_COMBAT))
            return true;
        return MoveNear(guardian, FETCH_APPROACH_STEP, MovementPriority::MOVEMENT_COMBAT);
    }

    if (LeaveMount(bot))
    {
        BMShared::Log(bot, "bm pull", guardian->GetGUID().ToString() + ":mount",
                      Acore::StringFormat("dismounts to open on {}", DescribeUnit(guardian, medivh)));
        return true;
    }

    for (char const* opener : FETCH_OPENERS)
        if (botAI->CanCastSpell(opener, guardian) && botAI->CastSpell(opener, guardian))
        {
            BMShared::Log(bot, "bm pull", guardian->GetGUID().ToString() + ":open",
                          Acore::StringFormat("opens on {} with {}", DescribeUnit(guardian, medivh), opener));
            return true;
        }

    // No opener ready: into melee. Attack() alone from 20 yd only selected the keeper, every tick, while the fetch
    // outranked the reach actions (bm-h 2026-10-25: 100 s of "attacks Rift Keeper", the keeper never engaged).
    bool const attacked = Attack(guardian);
    if (!bot->IsWithinMeleeRange(guardian))
    {
        // [b32_bm] MoveTo(unit, d) stops at d yd past BOTH combat reaches (WorldObject::GetDistance), while melee
        // range is the reaches + 4/3 yd (Unit::GetMeleeRange, at least 5): a fixed 1.5 yd parked the tank 0.17 yd
        // outside the swing of any big model. bm-h 20261025-223301-bm-h-5H-72: Aeonus (combat reach 3, REACT_DEFENSIVE,
        // channelling Corrupt Medivh) + bear tank = 4.5 yd of reach, the tank stopped at 6.0 yd exact vs 5.83 yd of
        // melee range, every later MoveTo was a duplicate of its own spot, and the fetch (62) still returned true
        // ("attacked"), so reach melee / feral charge never ran: 140 s idle, the shield 100 -> 0, Aeonus never fought.
        // The walk now ends well inside the melee range; a walk that does not start leaves the tick to reach melee.
        float const reaches = bot->GetCombatReach() + guardian->GetCombatReach();
        float const closeTo =
            std::max(0.0f, std::min(FETCH_MELEE_DISTANCE, bot->GetMeleeRange(guardian) - reaches - 0.5f));
        BMShared::Log(bot, "bm pull", guardian->GetGUID().ToString() + ":close",
                      Acore::StringFormat("closes to melee on {} (no ranged opener ready, {:.1f} yd, to {:.1f} past "
                                          "the reaches)",
                                          DescribeUnit(guardian, medivh), bot->GetExactDist(guardian), closeTo));
        return MoveTo(guardian, closeTo, MovementPriority::MOVEMENT_COMBAT);
    }

    if (attacked)
        BMShared::Log(bot, "bm pull", guardian->GetGUID().ToString() + ":open",
                      Acore::StringFormat("attacks {} in melee", DescribeUnit(guardian, medivh)));
    return attacked;
}

bool BlackMorassDragRiftGuardianAction::Execute(Event /*event*/)
{
    Unit* guardian = BMShared::GuardianToDrag(botAI);
    if (!guardian)
        return false;

    float x = BMShared::HOLD_X;
    float y = BMShared::HOLD_Y;
    float z = BMShared::HOLD_Z;
    bot->UpdateAllowedPositionZ(x, y, z);

    BMShared::Log(bot, "bm drag", guardian->GetGUID().ToString(),
                  Acore::StringFormat("drags {} to Medivh, {:.0f} yd to go",
                                      DescribeUnit(guardian, BMShared::Medivh(bot)), bot->GetExactDist2d(x, y)));
    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}

bool BlackMorassChronoBeaconFromSaatAction::isUseful() { return getMSTime() >= _retryAtMs; }

bool BlackMorassChronoBeaconFromSaatAction::Execute(Event /*event*/)
{
    Creature* saat = bot->FindNearestCreature(BMShared::NPC_SAAT, SAAT_SEARCH_RANGE);
    if (!saat || !saat->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
    {
        _retryAtMs = getMSTime() + SAAT_RETRY_MS;
        return false;
    }

    if (!bot->IsWithinDistInMap(saat, SAAT_TALK_DISTANCE))
        return MoveNear(saat, 2.0f);

    if (bot->isMoving())
        bot->StopMoving();

    if (!DungeonInteract::OpenGossip(bot, saat))
    {
        _retryAtMs = getMSTime() + SAAT_RETRY_MS;
        BMShared::Log(bot, "bm beacon", "hello", "Sa'at opened no gossip menu", MINUTE * IN_MILLISECONDS);
        return false;
    }

    // Menu 8088 lists "I require a chrono-beacon" (quest 10297 rewarded) or "I have lost the chrono-beacon" (10297
    // in the log); the server shows neither without the quest or once the first rift opened (conditions 15/8088).
    GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
    uint32 const menuId = menu.GetMenuId();
    size_t const options = menu.GetMenuItemCount();
    int32 option = -1;
    if (menuId == BMShared::GOSSIP_MENU_SAAT)
        option = DungeonInteract::FindGossipOption(bot, [](GossipMenuItem const&) { return true; });
    bool const sent = DungeonInteract::SelectGossipOption(bot, saat, option);
    bot->PlayerTalkClass->SendCloseGossip();

    if (!sent)
    {
        _retryAtMs = getMSTime() + SAAT_NO_OPTION_RETRY_MS;
        BMShared::Log(bot, "bm beacon", "no option",
                      Acore::StringFormat("Sa'at offers no Chrono-beacon (menu {}, {} options): quest 10297 'The "
                                          "Opening of the Dark Portal' is neither in the log nor rewarded",
                                          menuId, options),
                      MINUTE * IN_MILLISECONDS);
        return false;
    }

    _retryAtMs = getMSTime() + SAAT_VERIFY_MS;
    BMShared::Log(bot, "bm beacon", "asked", Acore::StringFormat("asked Sa'at for a Chrono-beacon (option {})", option),
                  MINUTE * IN_MILLISECONDS);
    return true;
}

bool BlackMorassUseChronoBeaconAction::isUseful()
{
    // One Time Keeper at a time: it lives 30 s, the next beacon waits for it.
    Creature* medivh = BMShared::Medivh(bot);
    return medivh && !medivh->FindNearestCreature(BMShared::NPC_TIME_KEEPER, BMShared::TIME_KEEPER_SEARCH);
}

bool BlackMorassUseChronoBeaconAction::Execute(Event /*event*/)
{
    Item* beacon = bot->GetItemByEntry(BMShared::ITEM_CHRONO_BEACON);
    if (!beacon)
        return false;

    bool const used = UseItemAuto(beacon);
    BMShared::Log(bot, "bm beacon", "use",
                  Acore::StringFormat("{} the Chrono-beacon ({} adds at Medivh)", used ? "uses" : "failed to use",
                                      AI_VALUE(GuidVector, "bm medivh threats").size()));
    return used;
}

bool BlackMorassRemoveBossBuffAction::isUseful() { return getMSTime() >= _retryAtMs; }

bool BlackMorassRemoveBossBuffAction::Execute(Event /*event*/)
{
    Unit* boss = BMShared::BossBuffToRemove(botAI);
    if (!boss)
        return false;

    char const* spell = "tranquilizing shot";
    switch (bot->getClass())
    {
        case CLASS_SHAMAN:
            spell = "purge";
            break;
        case CLASS_MAGE:
            spell = "spellsteal";
            break;
        case CLASS_PRIEST:
            spell = "dispel magic";
            break;
        default:
            break;
    }

    char const* buff = boss->GetEntry() == BMShared::NPC_AEONUS ? "Frenzy (Enrage)" : "Hasten";
    if (botAI->CanCastSpell(spell, boss) && botAI->CastSpell(spell, boss))
    {
        BMShared::Log(bot, "bm dispel", boss->GetGUID().ToString(),
                      Acore::StringFormat("{} on {}: {}", spell, boss->GetName(), buff), 5 * IN_MILLISECONDS);
        return true;
    }

    _retryAtMs = getMSTime() + BOSS_BUFF_RETRY_MS;
    BMShared::Log(bot, "bm dispel", boss->GetGUID().ToString() + ":none",
                  Acore::StringFormat("{} has {}, no removal ready", boss->GetName(), buff));
    return false;
}

bool BlackMorassFightSpotAtMedivhAction::Execute(Event /*event*/)
{
    Position const hold(BMShared::HOLD_X, BMShared::HOLD_Y, BMShared::HOLD_Z);
    FightHold::Move(bot, hold);
    BMShared::Log(bot, "bm fight spot", "medivh",
                  Acore::StringFormat("moves its fight spot to Medivh's hold spot ({:.1f},{:.1f}): the portals are one "
                                      "fight, its chases end by Medivh", hold.GetPositionX(), hold.GetPositionY()),
                  30 * IN_MILLISECONDS);
    // bookkeeping only: the tick goes on to the next action
    return false;
}
