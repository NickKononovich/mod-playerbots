/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn30] Three events the journal had no row for at all, gathered here and written once each.
//
// 1. THE START OF A FIGHT. Measured on the live journal: one 2.69 h window held 9 467 finished fights in
//    playerbots_fight_history and NOT ONE beginning. The beginning was reachable only through a sampler that ran
//    about every 22 s and caught roughly half the bots, so "who pulled this, where, and against how many" was
//    reconstructed from neighbouring lines. Here it is a row, written at the combat-state transition the core
//    announces (CombatManager::UpdateOwnerCombatState -> OnPlayerEnterCombat fires only on not-in-combat ->
//    in-combat), so it is exactly one row per fight per bot, with no clock of its own and nothing to de-duplicate.
//
// 2. AGGRO GAINED, WITH A REASON CODE. The user asked for it ("it caught some aggro, that too") and named the case
//    that matters: "an error of the estimate". That case was not merely unrecorded - it was inexpressible, because
//    nothing in the journal said WHY a mob was on a bot. The code is a column with an index; see Journal::Aggro.
//
// 3. A REAL DEATH. 20 502 stream lines in one window carried a tag starting with "[death", and 17 of them were
//    deaths: the rest is the death-PREVENTION domain under its own numbered tags, writing about a death that did
//    NOT happen. So a real death is a row in a table of its own, and a count of deaths is a COUNT(*) over it.
//
// WHAT IS NOT GUESSED HERE. The run id: a dungeon run is the harness's, and a core hook does not know which run it
// is inside, so `run_id` stays empty rather than being matched by time - a window join over at_ms is the reader's
// job and is honest; a guess written into the column would not be. The `decision_id` is empty for the same reason:
// the leader's pull decision row is written about a second after its group of lines closes, which is after this
// event, so the two are joined on (entry, low) and the window, not by an id that does not exist yet.
//
// WHY THE ORDER REGISTRY IS THE AUTHORITY ON "WHO PULLED IT". Leader::Orders holds the pull order that was actually
// issued, with its tank, its target, its method, the mobs the plan expected and the price it was taken at - that is
// the thing that happened, not an inference from a name. It is read under its own mutex, so a map thread may ask it.

#include "Journal.h"

#include <string>

#include "AiFactory.h"
#include "Creature.h"
#include "DamageTaken.h"
#include "Group.h"
#include "LeaderOrders.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ThreatManager.h"

namespace
{
    // "tank" / "healer" / "dps" in this group. A bot with no AI is not ours and gets no row at all, so this is only
    // ever asked about a playerbot.
    std::string RoleOf(Player* bot)
    {
        if (PlayerbotAI::IsTank(bot))
            return "tank";
        if (PlayerbotAI::IsHeal(bot))
            return "healer";
        return "dps";
    }

    // The open pull order of this bot's group, whoever of its members holds it. The registry is keyed by the tank,
    // and a five has at most five members, so this is five locked lookups once per fight - not per tick.
    // Returns the member whose order is open (nullptr: none), and fills `order`.
    Player* OpenOrderOf(Player* bot, Leader::PullOrder& order)
    {
        if (Leader::Orders::GetOpen(bot->GetGUID(), order))
            return bot;
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource();
                member && member != bot && Leader::Orders::GetOpen(member->GetGUID(), order))
                return member;
        return nullptr;
    }

    // How many creatures have this bot on their threat list at this instant. It is the fight's size AS IT IS NOW,
    // not as it will end: mobs that join later are the "add" rows of pb_journal_aggro, and the two together are the
    // fight. Read here and nowhere later, because a count taken at another moment is a different quantity.
    uint32 MobsOn(Player* bot)
    {
        uint32 mobs = 0;
        for (auto const& [guid, ref] : bot->GetThreatMgr().GetThreatenedByMeList())
            if (Creature* mob = ref ? ref->GetOwner() : nullptr; mob && mob->IsAlive())
                ++mobs;
        return mobs;
    }

    // Only bots of this module get rows: a real player's fight is not the instrument's subject, and a unit with no
    // AI has no role, no leader and no plan to compare against.
    bool OurBot(Player* player)
    {
        return player && player->IsInWorld() && GET_PLAYERBOT_AI(player);
    }
}  // namespace

namespace Journal
{
    void NoteFightStart(Player* bot, Unit* enemy)
    {
        if (!OurBot(bot) || !enemy)
            return;

        Fight fight;
        fight.event = "start";
        fight.botName = bot->GetName();
        fight.botLow = bot->GetGUID().GetCounter();
        fight.botLevel = bot->GetLevel();
        fight.role = RoleOf(bot);
        fight.mapId = bot->GetMapId();
        fight.zoneId = bot->GetZoneId();
        fight.areaId = bot->GetAreaId();
        fight.x = bot->GetPositionX();
        fight.y = bot->GetPositionY();
        fight.z = bot->GetPositionZ();
        fight.inDungeon = bot->GetMap() && bot->GetMap()->IsDungeon();
        fight.runId = Leader::Runs::OfBot(bot);  // [jrn40] by identity, not by nearness in time
        fight.enemyName = enemy->GetName();
        fight.enemyLow = enemy->GetGUID().GetCounter();
        fight.enemyLevel = enemy->GetLevel();
        // the entry identifies a creature; a player has none, and 0 here means "this enemy has no entry", which is
        // why the GUID counter is beside it rather than instead of it
        if (Creature* creature = enemy->ToCreature())
            fight.enemyEntry = creature->GetEntry();
        fight.mobs = MobsOn(bot);

        // who pulled it and with what: the issued order is the authority
        Leader::PullOrder order;
        if (Player* owner = OpenOrderOf(bot, order))
        {
            fight.pullerName = owner->GetName();
            fight.pullerLow = owner->GetGUID().GetCounter();
            fight.pullKind = "order";
            fight.pullMethod = Leader::MethodName(order.method);
            fight.pullExpected = order.expected;
            fight.pullCost = order.cost;
            fight.pullOrderId = order.id;
        }
        else if (Unit* victim = enemy->GetVictim())
        {
            // no order names it. Its victim is a FACT about who it went for, and nothing more is claimed than that.
            fight.pullKind = victim == bot ? "self" : "other";
            fight.pullerName = victim->GetName();
            fight.pullerLow = victim->GetGUID().GetCounter();
        }
        else
            fight.pullKind = "none";

        RecordFight(fight);
    }

    void NoteAggro(Creature* mob, Unit* victim)
    {
        Player* bot = victim ? victim->ToPlayer() : nullptr;
        if (!mob || !OurBot(bot))
            return;

        Aggro aggro;
        aggro.botName = bot->GetName();
        aggro.botLow = bot->GetGUID().GetCounter();
        aggro.role = RoleOf(bot);
        aggro.botHealthPct = float(bot->GetHealthPct());
        aggro.botInCombat = bot->IsInCombat();
        aggro.botMoving = bot->isMoving();
        aggro.mobName = mob->GetName();
        aggro.mobEntry = mob->GetEntry();
        aggro.mobLow = mob->GetGUID().GetCounter();
        aggro.mobLevel = mob->GetLevel();
        aggro.mapId = bot->GetMapId();
        aggro.zoneId = bot->GetZoneId();
        aggro.x = bot->GetPositionX();
        aggro.y = bot->GetPositionY();
        aggro.z = bot->GetPositionZ();
        aggro.runId = Leader::Runs::OfBot(bot);  // [jrn40]

        Leader::PullOrder order;
        Player* owner = OpenOrderOf(bot, order);
        if (owner)
        {
            aggro.tankName = owner->GetName();
            aggro.tankLow = owner->GetGUID().GetCounter();
            aggro.orderTargetEntry = order.targetEntry;
            aggro.orderTargetLow = order.target.GetCounter();
        }

        // THE REASON CODE. Every branch below is a fact at this instant, and the one case the user asked for -
        // "an error of the estimate" - is the branch where a judged pull was open and the mob went for somebody the
        // plan did not put in front of it, or for a mob the plan was not about. That is now a WHERE clause.
        if (bot->IsInCombat())
        {
            aggro.reason = "add";
            aggro.why = "a mob joined a fight already running: " + aggro.botName + " was in combat when " +
                        aggro.mobName + " engaged it";
            if (owner)
                aggro.expected = false;
        }
        else if (owner)
        {
            bool const onThePlansTank = owner == bot;
            bool const thePlansMob = order.target == mob->GetGUID();
            aggro.expected = onThePlansTank && thePlansMob;
            if (*aggro.expected)
            {
                aggro.reason = "came";
                aggro.why = "the issued pull arrived as planned: " + aggro.mobName + " engaged the tank it was " +
                            "pulled by (" + std::string(Leader::MethodName(order.method)) + ")";
            }
            else
            {
                aggro.reason = "estimate";
                aggro.why = "AN ERROR OF THE ESTIMATE: the pull of " + order.targetName + " by " +
                            aggro.tankName + " (" + std::string(Leader::MethodName(order.method)) + ", " +
                            std::to_string(order.expected) + " mob(s) expected) put " +
                            (onThePlansTank ? std::string("a mob it did not count (") + aggro.mobName + ")"
                                            : aggro.mobName + " on " + aggro.botName + ", who is not its tank");
            }
        }
        else if (bot->isMoving())
        {
            aggro.reason = "path";
            aggro.why = "hooked on the way: " + aggro.botName + " was walking and out of combat when " +
                        aggro.mobName + " engaged it, and no pull order named it";
        }
        else
        {
            aggro.reason = "came";
            aggro.why = "it came by itself: " + aggro.botName + " stood still and out of combat, and no pull "
                        "order named " + aggro.mobName;
        }

        RecordAggro(aggro);
    }

    void NoteDeath(Player* killed, Creature* killer, std::string const& context, std::string const& threat)
    {
        if (!OurBot(killed) || !killer)
            return;

        Death death;
        death.botName = killed->GetName();
        death.botLow = killed->GetGUID().GetCounter();
        death.botLevel = killed->GetLevel();
        death.botClass = killed->getClass();
        death.role = RoleOf(killed);
        death.spec = AiFactory::GetPlayerSpecName(killed);
        death.mapId = killed->GetMapId();
        death.zoneId = killed->GetZoneId();
        death.areaId = killed->GetAreaId();
        death.x = killed->GetPositionX();
        death.y = killed->GetPositionY();
        death.z = killed->GetPositionZ();
        death.inDungeon = killed->GetMap() && killed->GetMap()->IsDungeon();
        death.runId = Leader::Runs::OfBot(killed);  // [jrn40]
        death.killerName = killer->GetName();
        death.killerEntry = killer->GetEntry();
        death.killerLow = killer->GetGUID().GetCounter();
        death.killerLevel = killer->GetLevel();
        death.killerHealthPct = float(killer->GetHealthPct());
        // the share's denominator, always beside it: health 0 of a pool of 544 is readable, "0 %" alone is not
        death.healthMax = killed->GetMaxHealth();
        death.damageFromKiller = uint32(DamageTaken::From(killed->GetGUID(), killer->GetGUID()));
        death.attackers = uint32(killed->getAttackers().size());
        death.context = context;
        death.threat = threat;

        // WHAT THE JUDGE EXPECTED. A death is a calculation bug in this project, not bad luck, so the estimate that
        // allowed the fight goes into the row beside the outcome.
        Leader::PullOrder order;
        if (Player* owner = OpenOrderOf(killed, order))
        {
            death.judgedPull = order.leaderIssued;
            death.pullExpected = order.expected;
            death.pullMethod = Leader::MethodName(order.method);
            death.expected = "the open pull of " + order.targetName + " by " + owner->GetName() + ": " + order.why;
        }
        else
            death.judgedPull = false;  // asked and answered: no pull order was open. Not a gap.

        RecordDeath(death);
    }
}  // namespace Journal
