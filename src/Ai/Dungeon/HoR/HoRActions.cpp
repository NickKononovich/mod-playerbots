/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HoRActions.h"
#include "AttackerCountValues.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "Pet.h"
#include "Playerbots.h"
#include "RaidBotDuty.h"
#include "Spell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

using namespace HoRIds;

namespace
{
constexpr float INTRO_READY_RANGE = 80.0f;
constexpr float ESCAPE_LEADER_FOLLOW_DISTANCE = 4.0f;
constexpr float ESCAPE_LEADER_SETTLED_RANGE = 7.0f;

// Every member of the group in this instance is alive, out of combat, within range of the anchor and (mana users)
// above the ready threshold. A dead member cannot rejoin later: entering 668 is refused while an encounter runs.
bool IsGroupReady(Player* bot, WorldObject const* anchor, float range)
{
    Group* group = bot->GetGroup();
    if (!group)
        return bot->IsAlive() && !bot->IsInCombat() && bot->IsWithinDistInMap(anchor, range);

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (!member->IsAlive() || member->IsInCombat() || !member->IsWithinDistInMap(anchor, range))
            return false;

        if (member->getPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA) > 0 &&
            member->GetPowerPct(POWER_MANA) < READY_MANA_PCT)
            return false;
    }
    return true;
}

bool IsLichKing(Unit* unit)
{
    return unit && (unit->GetEntry() == NPC_LICH_KING_BOSS || unit->GetEntry() == NPC_LICH_KING_EVENT);
}

// [b33_hor] Death Grip taunts too and drags the summon onto the tank's pile (a Witch Doctor stands and casts where it
// is): the Blood DK's second pick-up while Dark Command (8 s) cools down (hor-n 20261026-000434 5G: "no taunt ready,
// attacking it" on most loose summons, median 11.5 s to take one back against the warrior's 4.4 s in 20261025-222644).
constexpr std::array<char const*, 5> ESCAPE_TAUNTS = {"hand of reckoning", "taunt", "growl", "dark command",
                                                      "death grip"};
constexpr uint32 RULE_LOG_INTERVAL_MS = 10 * IN_MILLISECONDS;

// One "[tag]" line per bot every RULE_LOG_INTERVAL_MS (the tags are string literals).
bool ShouldLogRule(Player* bot, std::string_view tag)
{
    static std::mutex mutex;
    static std::map<std::pair<uint64, std::string_view>, uint32> lastLogMs;
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> lock(mutex);
    uint32& at = lastLogMs[{bot->GetGUID().GetRawValue(), tag}];
    if (at && getMSTimeDiff(at, now) < RULE_LOG_INTERVAL_MS)
        return false;

    at = now ? now : 1;
    return true;
}
}  // namespace

Player* HoRStartIntroAction::GetQuestStarter(Creature* leader)
{
    bool const horde = leader->GetEntry() == NPC_SYLVANAS_PART1;
    uint32 const pitQuest = horde ? QUEST_DELIVERANCE_FROM_THE_PIT_H : QUEST_DELIVERANCE_FROM_THE_PIT_A;
    uint32 const wrathQuest = horde ? QUEST_WRATH_OF_THE_LICH_KING_H : QUEST_WRATH_OF_THE_LICH_KING_A;

    auto const qualifies = [&](Player* member)
    {
        if (!DungeonInteract::IsActiveMember(bot, member))
            return false;

        for (uint32 const quest : {pitQuest, wrathQuest})
        {
            QuestStatus const status = member->GetQuestStatus(quest);
            if (status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_REWARDED)
                return true;
        }
        return false;
    };

    Group* group = bot->GetGroup();
    if (!group)
        return qualifies(bot) ? bot : nullptr;

    Player* first = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (IsRealPlayer(member))
            return nullptr;  // the person at the client starts the event

        if (qualifies(member) && (!first || member->GetGUID() < first->GetGUID()))
            first = member;
    }

    if (Player* leaderPlayer = ObjectAccessor::FindPlayer(group->GetLeaderGUID()); leaderPlayer &&
        leaderPlayer != first && qualifies(leaderPlayer) && GET_PLAYERBOT_AI(leaderPlayer))
        return leaderPlayer;

    return first;
}

bool HoRStartIntroAction::isUseful()
{
    InstanceScript* instance = HoRHelpers::GetInstance(bot);
    if (!instance || instance->GetPersistentData(PERSISTENT_DATA_INTRO))
        return false;

    Creature* leader = HoRHelpers::GetInstanceCreature(bot, NPC_SYLVANAS_PART1);
    if (!leader || !leader->IsAlive() || !leader->IsVisible() || !leader->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return false;

    // npc_hor_leader refuses to start while the Battered Hilt (Quel'Delar) event is unfinished.
    if (uint32 const hilt = instance->GetData(DATA_BATTERED_HILT); hilt && !(hilt & BATTERED_HILT_FINISHED))
        return false;

    Player* starter = GetQuestStarter(leader);
    if (!starter)
    {
        if (!RaidBotDuty::GetHumanMaster(botAI))
            RaidBotDuty::LogWorkaroundOnce(bot, "hor-start-quest",
                                           "Halls of Reflection: no group member has quest 24710/24500 (Alliance) or "
                                           "24712/24802 (Horde) completed, the leader offers no start gossip");
        return false;
    }

    return starter == bot;
}

bool HoRStartIntroAction::Execute(Event /*event*/)
{
    Creature* leader = HoRHelpers::GetInstanceCreature(bot, NPC_SYLVANAS_PART1);
    if (!leader)
        return false;

    if (!IsGroupReady(bot, leader, INTRO_READY_RANGE))
        return false;

    if (!bot->IsWithinDistInMap(leader, INTERACTION_DISTANCE))
        return MoveNear(leader, INTERACTION_DISTANCE / 2.0f);

    bot->StopMoving();
    if (!DungeonInteract::OpenGossip(bot, leader))
        return false;

    // Prefer the short intro (offered once Wrath of the Lich King is done), else the full one.
    int32 option = DungeonInteract::FindGossipOption(
        bot, [](GossipMenuItem const& item) { return item.OptionType == GOSSIP_ACTION_SKIP_INTRO; });
    if (option < 0)
        option = DungeonInteract::FindGossipOption(
            bot, [](GossipMenuItem const& item) { return item.OptionType == GOSSIP_ACTION_START_INTRO; });

    bool const started = DungeonInteract::SelectGossipOption(bot, leader, option);
    bot->PlayerTalkClass->SendCloseGossip();
    if (started)
        LOG_INFO("playerbots", "{} starts the Halls of Reflection intro", bot->GetName());

    return started;
}

bool HoRMoveToWavePositionAction::Execute(Event /*event*/)
{
    // Healers + ranged to the corner, tank + melee to its edge; also the way back when the leash range is crossed.
    if (!HoRHelpers::IsWaveEventActive(bot))
        return false;

    HoRHelpers::WaveHold const hold = HoRHelpers::GetWaveHold(bot);
    MovementPriority const priority =
        bot->IsInCombat() ? MovementPriority::MOVEMENT_COMBAT : MovementPriority::MOVEMENT_NORMAL;
    return MoveNear(bot->GetMapId(), hold.spot.GetPositionX(), hold.spot.GetPositionY(), hold.spot.GetPositionZ(),
                    hold.spread, priority);
}

bool HoRGeneralLeashAction::Execute(Event /*event*/)
{
    Creature* general = HoRHelpers::GetFrostswornGeneral(bot);
    if (!general)
        return false;

    // hor-h 20261025-163739 pull 11: "move fight spot back" walked him 70 yd towards the healer's sight, past his
    // 30 yd leash: he evaded after 4 s and the copies hid. He is fought in his corridor, the group comes to him.
    Position const home = general->GetHomePosition();
    float const dx = bot->GetPositionX() - home.GetPositionX();
    float const dy = bot->GetPositionY() - home.GetPositionY();
    float const length = std::max(0.1f, std::hypot(dx, dy));
    float x = home.GetPositionX() + dx / length * GENERAL_TANK_RETURN;
    float y = home.GetPositionY() + dy / length * GENERAL_TANK_RETURN;
    float z = home.GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);
    if (ShouldLogRule(bot, "hor-general-leash"))
        LOG_INFO("playerbots", "[hor general leash] {}: Frostsworn General {:.1f} yd from his home (evades past {:.0f}), "
                 "walks him back", bot->GetName(), bot->GetExactDist2d(&home), GENERAL_LEASH);
    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}

bool HoRAttackReflectionAction::Execute(Event /*event*/)
{
    Unit* reflection = HoRHelpers::GetAttackableReflection(botAI);
    return reflection && Attack(reflection);
}

bool HoRShadowAuraAction::isUseful()
{
    return HoRHelpers::IsShadowAuraPaladin(botAI) && HoRHelpers::IsShadowBossFight(bot) &&
           !botAI->HasAura("shadow resistance aura", bot);
}

bool HoRShadowAuraAction::Execute(Event /*event*/)
{
    if (!botAI->CanCastSpell("shadow resistance aura", bot) || !botAI->CastSpell("shadow resistance aura", bot))
        return false;

    if (ShouldLogRule(bot, "hor-shadow-aura"))
    {
        InstanceScript* instance = HoRHelpers::GetInstance(bot);
        bool const marwyn = instance && instance->GetBossState(DATA_FALRIC) == DONE;
        LOG_INFO("playerbots", "[hor shadow aura] {}: Shadow Resistance Aura for {} (shadow damage on the whole group)",
                 bot->GetName(), marwyn ? "Marwyn" : "Falric");
    }
    return true;
}

bool HoRLeaveWaveCircleAction::Execute(Event /*event*/)
{
    if (!HoRHelpers::IsWaveLost(bot))
        return false;

    uint32 const now = getMSTime();
    if (!_loggedMs || getMSTimeDiff(_loggedMs, now) > WAVE_LOST_LOG_MS)
    {
        _loggedMs = now ? now : 1;
        LOG_INFO("playerbots", "[hor wave lost] {}: the waves cannot be won with the group down, leaving the circle "
                 "({:.1f} yd from the centre) so they reset", bot->GetName(), bot->GetExactDist2d(&WAVE_CENTER));
    }

    // Out past the corner (the way in), beyond the core's 70.5 yd: the waves wipe and the dead may walk back in.
    float const dx = WAVE_CORNER_SPOT.GetPositionX() - WAVE_CENTER.GetPositionX();
    float const dy = WAVE_CORNER_SPOT.GetPositionY() - WAVE_CENTER.GetPositionY();
    float const length = std::max(1.0f, std::hypot(dx, dy));
    float x = WAVE_CENTER.GetPositionX() + dx / length * WAVE_RESET_LEAVE_RANGE;
    float y = WAVE_CENTER.GetPositionY() + dy / length * WAVE_RESET_LEAVE_RANGE;
    float z = WAVE_CORNER_SPOT.GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);
    if (bot->isMoving() && IsWaitingForLastMove(MovementPriority::MOVEMENT_FORCED))
        return true;
    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED) ||
           bot->isMoving();
}

bool HoRAttackPriorityAction::AttackFirst(HoRHelpers::PriorityList list)
{
    std::vector<Unit*> const rank = HoRHelpers::GetPriorityRank(botAI, list);
    Unit* const current = AI_VALUE(Unit*, "current target");
    if (rank.empty() || std::find(rank.begin(), rank.end(), current) != rank.end())
        return false;  // nothing ranked, or already on a target of the best available rank

    Unit* best = nullptr;
    for (Unit* unit : rank)
        if (!best || bot->GetExactDist2d(unit) < bot->GetExactDist2d(best))
            best = unit;

    return Attack(best);
}

bool HoRAttackWavePriorityAction::Execute(Event /*event*/) { return AttackFirst(HoRHelpers::PriorityList::Wave); }

bool HoREnterThroneRoomAction::Execute(Event /*event*/)
{
    AreaTrigger const* at = sObjectMgr->GetAreaTrigger(AREATRIGGER_SHADOW_THRONE);
    if (!at || at->map != bot->GetMapId())
        return false;

    if (DungeonInteract::SendAreaTrigger(bot, AREATRIGGER_SHADOW_THRONE))
    {
        LOG_INFO("playerbots", "{} enters the Shadow Throne area trigger (Halls of Reflection)", bot->GetName());
        return true;
    }

    if (!MoveTo(at->map, at->x, at->y, at->z))
    {
        RaidBotDuty::LogWorkaroundOnce(bot, "hor-throne-path",
                                       "Halls of Reflection: needs path fix: cannot path into area trigger 5605");
        return false;
    }
    return true;
}

bool HoRMoveToEscapeLeaderAction::Execute(Event /*event*/)
{
    Creature* leader = HoRHelpers::GetWaitingEscapeLeader(bot);
    if (!leader)
        return false;

    return MoveNear(leader, ESCAPE_LEADER_FOLLOW_DISTANCE);
}

bool HoRStartEscapeAction::Execute(Event /*event*/)
{
    Creature* leader = HoRHelpers::GetWaitingEscapeLeader(bot);
    if (!leader || !IsGroupReady(bot, leader, EVENT_GATHER_RANGE))
        return false;

    if (!bot->IsWithinDistInMap(leader, INTERACTION_DISTANCE))
        return MoveNear(leader, INTERACTION_DISTANCE / 2.0f);

    bot->StopMoving();
    if (!DungeonInteract::SelectFirstGossipOption(bot, leader))
        return false;

    LOG_INFO("playerbots", "{} starts the Halls of Reflection escape", bot->GetName());
    return true;
}

bool HoREscapeMoveToLeaderAction::Execute(Event /*event*/)
{
    Creature* leader = HoRHelpers::GetInstanceCreature(bot, NPC_SYLVANAS_PART2);
    Creature* lichKing = HoRHelpers::GetInstanceCreature(bot, NPC_LICH_KING_BOSS);
    if (!leader || !lichKing)
        return false;

    bool const behind = HoRHelpers::EscapeBehindOffset(bot, lichKing) > ESCAPE_BEHIND_LIMIT ||
                        HoRHelpers::IsBehindEscapeTank(bot);
    if (!behind && bot->GetExactDist2d(leader) < ESCAPE_LEADER_SETTLED_RANGE)
        return false;

    return MoveNear(leader, ESCAPE_LEADER_FOLLOW_DISTANCE, MovementPriority::MOVEMENT_FORCED);
}

bool HoRAttackEscapePriorityAction::Execute(Event /*event*/) { return AttackFirst(HoRHelpers::PriorityList::Escape); }

bool HoRStopAttackingLichKingAction::Execute(Event /*event*/)
{
    bool acted = false;
    if (IsLichKing(bot->GetVictim()) || IsLichKing(AI_VALUE(Unit*, "current target")))
    {
        if (Spell* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (Unit* target = ObjectAccessor::GetUnit(*bot, spell->m_targets.GetUnitTargetGUID());
                target && target->IsInWorld() && target->GetMap() == bot->GetMap() && IsLichKing(target))
                bot->InterruptNonMeleeSpells(false);

        bot->AttackStop();
        bot->SetTarget(ObjectGuid::Empty);
        context->GetValue<Unit*>("current target")->Set(nullptr);
        acted = true;
    }

    if (Pet* pet = bot->GetPet(); pet && IsLichKing(pet->GetVictim()))
    {
        pet->AttackStop();
        if (CharmInfo* charmInfo = pet->GetCharmInfo())
        {
            charmInfo->SetIsCommandAttack(false);
            charmInfo->SetIsCommandFollow(true);
        }
        pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
        acted = true;
    }

    for (Unit* controlled : bot->m_Controlled)
    {
        if (!controlled || controlled == bot->GetPet() || !controlled->IsAlive() || !IsLichKing(controlled->GetVictim()))
            continue;

        controlled->AttackStop();
        controlled->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, controlled->GetFollowAngle());
        acted = true;
    }

    return acted;
}

bool HoREscapeTankPositionAction::Execute(Event /*event*/)
{
    Position spot;
    if (!HoRHelpers::GetEscapeTankSpot(bot, spot))
        return false;

    float x = spot.GetPositionX();
    float y = spot.GetPositionY();
    float z = spot.GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);
    if (ShouldLogRule(bot, "hor escape tank position"))
    {
        Creature* lichKing = HoRHelpers::GetInstanceCreature(bot, NPC_LICH_KING_BOSS);
        LOG_INFO("playerbots", "[hor escape tank position] {}: to the pick-up spot ({:.1f}, {:.1f}), {:.1f} yd away, "
                 "{:.1f} yd from the Lich King", bot->GetName(), x, y, bot->GetExactDist2d(x, y),
                 lichKing ? lichKing->GetExactDist2d(x, y) : 0.0f);
    }

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}

bool HoREscapeTauntLooseMobAction::Execute(Event /*event*/)
{
    Unit* mob = HoRHelpers::GetLooseEscapeMob(botAI);
    if (!mob)
        return false;

    Unit* victim = mob->GetVictim();
    char const* taunted = nullptr;
    for (char const* taunt : ESCAPE_TAUNTS)
        if (botAI->CanCastSpell(taunt, mob) && botAI->CastSpell(taunt, mob))
        {
            taunted = taunt;
            break;
        }

    if (taunted)
        GroupMemberAttackerValue::RecordPeel(botAI, {mob->GetGUID()});

    if (taunted || ShouldLogRule(bot, "hor escape loose mob"))
        LOG_INFO("playerbots", "[hor escape loose mob] {}: {} on {} ({:.1f} yd), {}", bot->GetName(), mob->GetName(),
                 victim ? victim->GetName() : std::string("nobody"), bot->GetExactDist2d(mob),
                 taunted ? taunted : "no taunt ready, attacking it");

    return Attack(mob) || taunted;
}

bool HoREscapeHealerHoldAction::Execute(Event /*event*/)
{
    Creature* leader = HoRHelpers::GetInstanceCreature(bot, NPC_SYLVANAS_PART2);
    if (!leader || !leader->IsAlive())
        return false;

    if (ShouldLogRule(bot, "hor escape healer hold"))
        LOG_INFO("playerbots", "[hor escape healer hold] {}: back to {} behind the tank ({:.1f} yd away)",
                 bot->GetName(), leader->GetName(), bot->GetExactDist2d(leader));

    return MoveNear(leader, ESCAPE_HEALER_LEADER_SPREAD, MovementPriority::MOVEMENT_COMBAT);
}

bool HoRFalricDispelAction::Execute(Event /*event*/)
{
    std::string spell;
    Player* target = HoRHelpers::GetFalricDispelTarget(botAI, spell);
    if (!target || !botAI->CanCastSpell(spell, target))
        return false;

    char const* debuff = target->HasAura(SPELL_IMPENDING_DESPAIR) ? "Impending Despair" : "Quivering Strike";
    if (!botAI->CastSpell(spell, target))
        return false;

    if (ShouldLogRule(bot, "hor falric dispel"))
        LOG_INFO("playerbots", "[hor falric dispel] {}: {} on {} ({})", bot->GetName(), spell, target->GetName(),
                 debuff);
    return true;
}
