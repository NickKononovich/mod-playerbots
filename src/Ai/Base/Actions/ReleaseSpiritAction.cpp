/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReleaseSpiritAction.h"
#include "AggressiveTargetValue.h"
#include "Corpse.h"
#include "Event.h"
#include "GameGraveyard.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "NearestNpcsValue.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RaidLeaderState.h"
#include "ReachTargetActions.h"
#include "ServerFacade.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <algorithm>
#include <unordered_map>  // [b47_plan]

namespace
{
    // [b30_hor_rez] A living member is a resurrecter worth waiting for only when a resurrection can come. While the
    // instance's encounter is in progress the core refuses every out-of-combat resurrection (Spell::CheckCast,
    // SPELL_FAILED_TARGET_CANNOT_BE_RESURRECTED): only a ready battle resurrection counts then - unless the living
    // still outnumber the dead (they carry the encounter on and resurrect once it is over). hor-n
    // 20261025-185515-hor-n-5H-74: four dead in a wave at 731-764 s, the bear tank battle-rezzed alone; its
    // Revive failed with code 186 every tick (the waves are the encounter), the four lay "waiting" for it 73 s, the
    // next wave killed the tank eating alone at 851 s.
    bool ResurrectionCanCome(Player* member, uint32 alive, uint32 dead)
    {
        if (!RaidLeader::CanResurrect(member))
            return false;
        InstanceScript* script = member->GetInstanceScript();
        if (!script || alive >= dead || !script->IsEncounterInProgress())
            return true;
        return RaidLeader::HasBattleResurrection(member);
    }

    // [b32_sh_ooze] In an instance with a respawn zone (RespawnIgnored: trash that refills behind the group) a
    // resurrection is worth more than a corpse run - the run back from the entrance passes the refilled mobs again
    // (user 2026-10-25: «если кто-то умрет, то вся группа должна идти заново»). A resurrecter within this range counts
    // (PartyMemberToResurrect's walk range, the healer walks back to the corpse), not only one within `range`.
    constexpr float RESPAWN_ZONE_RESURRECTER_RANGE = 100.0f;
    float ResurrecterRange(Player* bot, float range)
    {
        return RespawnIgnored::Any(bot->GetInstanceId()) ? std::max(range, RESPAWN_ZONE_RESURRECTER_RANGE) : range;
    }

    void CountGroup(Player* bot, uint32& alive, uint32& dead)
    {
        alive = 0;
        dead = 0;
        for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource(); member && member->GetMap() == bot->GetMap())
                ++(member->IsAlive() ? alive : dead);
    }
}

// ReleaseSpiritAction implementation
bool ReleaseSpiritAction::Execute(Event event)
{
    if (bot->IsAlive())
    {
        if (!bot->InBattleground())
        {
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "release_spirit_not_dead_wait", "I am not dead, will wait here", {}));
            // -follow in bg is overwriten each tick with +follow
            // +stay in bg causes stuttering effect as bot is cycled between +stay and +follow each tick
            botAI->ChangeStrategy("-follow,+stay", BOT_STATE_NON_COMBAT);
        }

        return false;
    }

    if (bot->GetCorpse() && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "release_spirit_already_spirit", "I am already a spirit", {}));
        return false;
    }

    WorldPacket const& packet = event.getPacket();
    const std::string message = !packet.empty() && packet.GetOpcode() == CMSG_REPOP_REQUEST
        ? PlayerbotTextMgr::instance().GetBotTextOrDefault("release_spirit_releasing", "Releasing...", {})
        : PlayerbotTextMgr::instance().GetBotTextOrDefault("release_spirit_meet_graveyard", "Meet me at the graveyard", {});
    botAI->TellMasterNoFacing(message);

    IncrementDeathCount();
    LogRelease("released");

    WorldPacket releasePacket(CMSG_REPOP_REQUEST);
    releasePacket << uint8(0);
    bot->GetSession()->HandleRepopRequestOpcode(releasePacket);

    return true;
}

void ReleaseSpiritAction::IncrementDeathCount() const
{
    // BG deaths don't count, matching PlayerbotAI::DoNextAction.
    if (bot->InBattleground())
        return;

    // Death Count to prevent skeleton piles
    Player* master = botAI->GetMaster();
    if (!master || GET_PLAYERBOT_AI(master))
    {
        uint32 deathCount = AI_VALUE(uint32, "death count");
        context->GetValue<uint32>("death count")->Set(deathCount + 1);
    }
}

void ReleaseSpiritAction::LogRelease(std::string const& releaseMsg) const
{
    const std::string teamPrefix = bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H";

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> {}",
        bot->GetGUID().ToString().c_str(),
        teamPrefix,
        bot->GetLevel(),
        bot->GetName().c_str(),
        releaseMsg.c_str());
}

// AutoReleaseSpiritAction implementation
bool AutoReleaseSpiritAction::Execute(Event /*event*/)
{
    IncrementDeathCount();
    LogRelease("auto released");

    if (_noResSpot)
    {
        // In a dungeon the ghost is brought back where it walks in (WorldSession::HandleMoveWorldportAck: the corpse
        // lies in the instance), elsewhere "find corpse" picks a spot out of the pack's reach on the group's side.
        Map* map = bot->GetMap();
        AreaTriggerTeleport const* entrance =
            map->IsDungeon() ? sObjectMgr->GetMapEntranceTrigger(map->GetId()) : nullptr;
        if (entrance)
            LOG_INFO("playerbots", "[corpse res] no safe spot -> {} releases and revives at ({:.1f},{:.1f},{:.1f}), "
                     "the instance entrance", bot->GetName(), entrance->target_X, entrance->target_Y,
                     entrance->target_Z);
        else
            LOG_INFO("playerbots", "[corpse res] no safe spot -> {} releases and revives near its corpse "
                     "({:.1f},{:.1f},{:.1f}) out of the pack's reach", bot->GetName(), bot->GetPositionX(),
                     bot->GetPositionY(), bot->GetPositionZ());
        _noResSpot = false;
    }

    WorldPacket packet(CMSG_REPOP_REQUEST);
    packet << uint8(0);
    bot->GetSession()->HandleRepopRequestOpcode(packet);

    LogRelease("releases spirit");

    if (bot->InBattleground())
    {
        return HandleBattlegroundSpiritHealer();
    }

    botAI->SetNextCheckDelay(1000);
    return true;
}

bool AutoReleaseSpiritAction::isUseful()
{
    if (!bot->isDead())
    {
        _diedAt = 0;
        _releaseAt = 0;
        _deadSince = 0;
        _noResSpot = false;
        return false;
    }

    // [b32_bfd] A new death restarts every wait. The reset above never runs: this action belongs to the dead engine
    // only, and PlayerbotAI switches to the living engine before any of its actions is asked again, so _deadSince,
    // _diedAt and _releaseAt outlived the resurrection. bfd-h 20261025-221216-5D-35: the bear tank died at 2443 s
    // (rezzed at 2498 s) and again at 2683 s; the second death inherited the first one's clocks - "dead 2 min, nobody
    // near" was true at once and "waited 3 min for the shaman" ran out when the fight ended - and the tank released
    // 14 s after the fight while the shaman (21 yd away, drinking) was about to resurrect it; its ghost then stood
    // at the Zoram Strand graveyard for good. A new death: the death count PlayerbotAI raises on every death changed,
    // or a corpse that lies elsewhere than the one this wait was for (a death count kept for a real master).
    if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        uint32 const deathCount = AI_VALUE(uint32, "death count");
        bool const newDeath = _deadSince && (deathCount != _deathCountSeen ||
                                             bot->GetMapId() != _deathMapId ||
                                             bot->GetExactDist(&_deathPos) > NEW_DEATH_MOVED);
        if (newDeath)
        {
            _deadSince = 0;
            _diedAt = 0;
            _releaseAt = 0;
            _noResSpot = false;
        }
        if (!_deadSince)
        {
            _deathCountSeen = deathCount;
            _deathMapId = bot->GetMapId();
            _deathPos = bot->GetPosition();
        }
    }

    if (!_deadSince)
        _deadSince = getMSTime();

    if (bot->InArena())
        return false;

    if (bot->InBattleground())
        return ShouldDelayBattlegroundRelease();

    if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return false;

    // Every resurrecter's search found no spot in sight of the corpse out of the pack's reach (the corpse lies deep in
    // an idle pack): no resurrection comes, release once the fight is over (RFC 2026-10-24 18:28, the tank waited 3.5
    // minutes for a paladin that could not see it without waking the Searing Blades).
    if (bot->GetGroup() && !bot->isResurrectRequested() && CorpseRes::Blocked(bot, CORPSE_RES_BLOCKED_MS) &&
        FightIsOver())
    {
        _noResSpot = true;
        return ReleaseDelayElapsed();
    }

    // Not while the living still fight something that hits them, nor during a boss encounter (its own waits hold).
    auto const nobodyHit = [this]() -> bool
    {
        for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                !member->getAttackers().empty())
                return false;
        }
        return true;
    };
    InstanceScript* const instance = bot->GetInstanceScript();

    // [b47_plan] Blocked that long, the fight's end is not waited for: a member held in a fight that never ends (a
    // summoned minion standing at its idle master, the living "in combat" with it, nobody hit: bfd-n 20261026-093620-
    // bfd-n-5B-19, 3648-3855 s) kept FightIsOver false for good. Releasing draws nothing; the ghost walks back in.
    if (bot->GetGroup() && bot->GetMap()->IsDungeon() && !bot->isResurrectRequested() &&
        CorpseRes::Blocked(bot, CORPSE_RES_BLOCKED_LONG_MS) && !(instance && instance->IsEncounterInProgress()) &&
        nobodyHit())
    {
        _noResSpot = true;
        return ReleaseDelayElapsed();
    }

    // [lead1367] user 2026-11-05 (no elapsed-time rule decides): b259 rfc-h 5H-8 the mage's body lay 219 s on "dead
    // 160 s in a dungeon, no resurrection came" while the leader had dropped every resurrecter (each reported its
    // cast failed). The leader judges, out of a fight, whether a resurrection can come to the body (a living member
    // that knows one, has not reported it cannot cast it here and has a walk to it): while one can, the body waits
    // for it - no clock ends that wait; once none can, it releases at once (no encounter in progress, nobody of
    // the group hit) and runs back. A Soulstone / Reincarnation keeps the rules below (its own self resurrection)
    RaidLeader::ResurrectVerdict const verdict = RaidLeader::GetResurrectVerdict(bot);
    if (bot->GetGroup() && bot->GetMap()->IsDungeon() && verdict != RaidLeader::ResurrectVerdict::Unknown &&
        !bot->GetUInt32Value(PLAYER_SELF_RES_SPELL))
    {
        if (bot->isResurrectRequested() || verdict == RaidLeader::ResurrectVerdict::Comes ||
            (instance && instance->IsEncounterInProgress()) || !nobodyHit())
            return false;
        thread_local std::unordered_map<uint64, uint32> loggedFor;  // per dead bot, the death it was logged for
        uint32& logged = loggedFor[bot->GetGUID().GetRawValue()];
        if (logged != _deadSince)
        {
            logged = _deadSince;
            LOG_INFO("playerbots", "[lead1367] release_no_res: {} dead in a dungeon, the leader finds no member that "
                     "can resurrect it reach it: releases and runs back", bot->GetName());
        }
        return true;
    }

    if (bot->GetGroup() && bot->GetMap()->IsDungeon() && GetMSTimeDiffToNow(_deadSince) >= INSTANCE_DEAD_MAX_MS &&
        !bot->isResurrectRequested())
    {
        bool resurrecterNear = false;
        uint32 alive = 0;
        uint32 dead = 0;
        CountGroup(bot, alive, dead);
        for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->GetMap() == bot->GetMap() &&
                !member->IsInCombat() && ResurrectionCanCome(member, alive, dead) &&  // [b30_hor_rez]
                bot->GetDistance(member) <= ResurrecterRange(bot, NEAR_RESURRECTER))  // [b32_sh_ooze]
                resurrecterNear = true;
        }

        if (!resurrecterNear)
            return ReleaseDelayElapsed();
    }

    if (WaitForGroupResurrect())
        return false;

    // In an instance nobody is left to resurrect us: once the fight is over, release and run back.
    if (bot->GetGroup() && bot->GetMap()->IsDungeon())
        return FightIsOver() && ReleaseDelayElapsed();

    return ShouldAutoRelease();
}

bool AutoReleaseSpiritAction::FightIsOver()
{
    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    bool anyAlive = false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || member->GetMap() != map || !member->IsAlive())
            continue;

        anyAlive = true;
        if (member->IsInCombat())
            return false;
    }

    if (!anyAlive)
        RaidLeader::CallWipe(bot, nullptr, "all dead");

    return true;
}

// Dead bots release a few seconds apart instead of all on the same tick.
bool AutoReleaseSpiritAction::ReleaseDelayElapsed()
{
    if (!_releaseAt)
        _releaseAt = getMSTime() + urand(RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);

    return int32(getMSTime() - _releaseAt) >= 0;
}

// The dead stay in the field while a living group member can resurrect them: during the fight (battle res, or
// a resurrection once it is over) and for a while after it. Groups in an instance always wait; elsewhere only
// groups led from a game client (a real player or a SelfBot) do.
bool AutoReleaseSpiritAction::WaitForGroupResurrect()
{
    Group* group = bot->GetGroup();
    if (!group || bot->GetMap()->IsBattlegroundOrArena())
        return false;

    bool const inInstance = bot->GetMap()->IsDungeon();
    if (!inInstance && !botAI->HasGameClientMaster() && !IsSelfBot(bot))
        return false;

    if (!_diedAt)
        _diedAt = getMSTime();

    bool resurrecterFound = false;
    bool resurrecterInCombat = false;
    bool groupInCombat = false;
    uint32 alive = 0;
    uint32 dead = 0;
    CountGroup(bot, alive, dead);
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            bot->GetDistance(member) > ResurrecterRange(bot, sPlayerbotAIConfig.SightDistance))  // [b32_sh_ooze]
            continue;

        groupInCombat = groupInCombat || member->IsInCombat();
        if (!ResurrectionCanCome(member, alive, dead))  // [b30_hor_rez]
            continue;

        resurrecterFound = true;
        resurrecterInCombat = resurrecterInCombat || member->IsInCombat();
    }

    // Soulstone or Reincarnation: the "self resurrect" action brings the bot back once rising is safe (the shaman's
    // trigger holds the Ankh while mobs fight, evade or rest on the corpse). Stratholme wave 18: held only 90 s for
    // a pack resting on the corpse, the shaman rose among it, 151 yd from the group, and died again within 2 s. Not
    // safe within SELF_RESURRECT_WAIT_MS of the fight's end: released, the corpse run picks a safe spot to rise on.
    bool const selfResurrect = bot->GetUInt32Value(PLAYER_SELF_RES_SPELL) != 0;
    if (selfResurrect && groupInCombat)
    {
        _diedAt = getMSTime();
        return true;
    }
    if (selfResurrect && GetMSTimeDiffToNow(_diedAt) <= SELF_RESURRECT_WAIT_MS)
        return true;

    if (!resurrecterFound)
        return false;

    // The wait counts from the end of the fight: a whole raid takes a while to resurrect one by one.
    if (resurrecterInCombat)
    {
        _diedAt = getMSTime();
        return true;
    }

    return GetMSTimeDiffToNow(_diedAt) <= (inInstance ? INSTANCE_RESURRECT_WAIT_MS : GROUP_RESURRECT_WAIT_MS);
}

bool AutoReleaseSpiritAction::HandleBattlegroundSpiritHealer()
{
    constexpr uint32_t RESURRECT_DELAY = 15;
    const time_t now = time(nullptr);

    if ((now - m_bgGossipTime < RESURRECT_DELAY) &&
        bot->HasAura(SPELL_WAITING_FOR_RESURRECT))
    {
        return false;
    }

    float bgRange = 2000.0f;
    GuidVector npcs = NearestNpcsValue(botAI, bgRange);
    Unit* spiritHealer = nullptr;

    for (auto const& guid : npcs)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsFriendlyTo(bot) && unit->IsSpiritService() &&
            (!spiritHealer || bot->GetDistance(unit) < bot->GetDistance(spiritHealer)))
            spiritHealer = unit;  // nearest: the list is not sorted
    }

    if (!spiritHealer)
        return false;

    // The bot must click the spirit healer to start its timer. Out of reach the ghost waits for the area
    // spirit healer instead of being teleported to it.
    if (bot->GetDistance(spiritHealer) >= INTERACTION_DISTANCE)
        return false;

    if (!IsSelfBot(bot))
    {
        m_bgGossipTime = now;
        WorldPacket packet(CMSG_GOSSIP_HELLO);
        packet << spiritHealer->GetGUID();
        bot->GetSession()->HandleGossipHelloOpcode(packet);
    }

    return true;
}

bool AutoReleaseSpiritAction::ShouldAutoRelease() const
{
    if (!bot->GetGroup())
        return true;

    Player* groupLeader = botAI->GetGroupLeader();
    if (!groupLeader || groupLeader == bot)
        return true;

    if (!botAI->HasGameClientMaster())
        return true;

    if (botAI->HasGameClientMaster() &&
        groupLeader->GetMapId() == bot->GetMapId() &&
        bot->GetMap() &&
        (bot->GetMap()->IsRaid() || bot->GetMap()->IsDungeon()))
    {
        return false;
    }

    return ServerFacade::instance().IsDistanceGreaterThan(
        AI_VALUE2(float, "distance", "group leader"),
        sPlayerbotAIConfig.SightDistance);
}

bool AutoReleaseSpiritAction::ShouldDelayBattlegroundRelease() const
{
    // The below delays release to spirit with 6 seconds.
    // This prevents currently casted (ranged) spells to be re-directed to the died bot's ghost.

    // If the bot already is a spirit, reset release time and return true
    if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        botAI->bgReleaseAttemptTime = 0;
        return true;
    }

    // Delay release to spirit.
    const time_t now = time(nullptr);
    constexpr time_t RELEASE_DELAY = 6;

    if (botAI->bgReleaseAttemptTime == 0)
        botAI->bgReleaseAttemptTime = now;

    if (now - botAI->bgReleaseAttemptTime < RELEASE_DELAY)
        return false;

    botAI->bgReleaseAttemptTime = 0;
    return true;
}

// A stuck or falling corpse releases like a player would (the server moves the ghost to the graveyard); a ghost
// is not teleported anywhere.
bool RepopAction::Execute(Event /*event*/)
{
    if (!bot->isDead() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return false;

    WorldPacket packet(CMSG_REPOP_REQUEST);
    packet << uint8(0);
    bot->GetSession()->HandleRepopRequestOpcode(packet);
    return true;
}

bool RepopAction::isUseful()
{
    return !bot->InBattleground() && bot->isDead() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
}

// SelfResurrectAction implementation for Warlock's Soulstone Resurrection/Shaman's Reincarnation
bool SelfResurrectAction::Execute(Event /*event*/)
{
    if (!bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL))
    {
        WorldPacket packet(CMSG_SELF_RES);
        bot->GetSession()->HandleSelfResOpcode(packet);
        return true;
    }
    return false;
}
bool SelfResurrectAction::isUseful()
{
    return !bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL);
}
