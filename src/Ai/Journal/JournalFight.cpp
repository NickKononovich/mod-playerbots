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

#include "AggroReason.h"
#include "AiFactory.h"
#include "CombatManager.h"
#include "Creature.h"
#include "DamageTaken.h"
#include "Group.h"
#include "LeaderOrders.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SoloSafety.h"  // [fight20] SoloSafety::OpenFactsOf: did this bot's own judge open the fight
#include "StringFormat.h"
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

    // [jrn44] WAS THE BOT IN COMBAT WITH ANYBODY BUT THIS MOB. `bot->IsInCombat()` cannot answer that question here
    // and answering it with IsInCombat() is what killed the reason code: Creature::AtEngage is reached only after the
    // core has already joined these two, so IsInCombat() is true for the FIRST mob of every fight. Measured on b502:
    // 4 661 of 4 661 rows said `add`, and 2 621 of them were the first mob of a fight whose own `mobs` count was 0.
    //
    // The combat references are the right list rather than the threat list: the reference for this very mob exists at
    // this instant (it is why IsInCombat() is true), so excluding its guid is exact, while the threat list is written
    // a moment earlier or later depending on who engaged whom. Suppressed references are skipped for the same reason
    // CombatManager::HasCombat skips them - a suppressed reference is not a fight.
    bool InCombatWithOtherThan(Player* bot, ObjectGuid const& mob)
    {
        CombatManager const& combat = bot->GetCombatManager();
        for (auto const& [guid, ref] : combat.GetPvECombatRefs())
            if (guid != mob && ref && !ref->IsSuppressedFor(bot))
                return true;
        for (auto const& [guid, ref] : combat.GetPvPCombatRefs())
            if (guid != mob && ref && !ref->IsSuppressedFor(bot))
                return true;
        return false;
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

        // [fight20] DID THE BOT'S OWN JUDGE OPEN THIS FIGHT. This hook IS the instant the fight begins, so
        // the question is asked here and the solo judge answers it from its own state plus one reading of the
        // first mob's aggro disc (AggroDisc - the same judge the bot's avoidance uses, computed once per
        // fight and read again by the [solo] sentence). It answers `false` when it is not this bot's judge at
        // all - in a dungeon the leader is - and then nothing is written, because "the leader judged it" is
        // not a measured "the bot did not".
        //   Until this build "267 fights no judge opened" was taken apart out of the text of those sentences,
        // which rule 1 forbids, and that is exactly why the share the zone rule [dpszero3] takes away could
        // not be counted on b504.
        SoloSafety::OpenFacts opened;
        if (SoloSafety::OpenFactsOf(bot, enemy, opened))
        {
            fight.judgeOpened = opened.judgeOpened;
            // the ground is a fact ABOUT a fight no judge opened: when the judge did open it, there is no
            // such quantity, and NULL says that rather than a zero that reads as "it was outside the disc"
            if (!opened.judgeOpened)
            {
                fight.unjudgedInside = opened.inside;
                fight.unjudgedMoving = opened.moving;
            }
        }

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

        // [jrn44] THE REASON CODE, decided by the seam (Journal::AggroReason) over the facts of this instant. The
        // facts are gathered here, where they are held; the choice between the codes is asserted by a test over real
        // b502 rows, because it had been wrong for every row the table ever had.
        AggroReason::Facts facts;
        facts.inCombatWithOthers = InCombatWithOtherThan(bot, mob->GetGUID());
        facts.pullOrderOpen = owner != nullptr;
        facts.onThePlansTank = owner == bot;
        facts.thePlansMob = order.target == mob->GetGUID();
        facts.moving = bot->isMoving();
        aggro.inCombatOther = facts.inCombatWithOthers;
        aggro.reason = AggroReason::Of(facts);
        // `expected` answers the plan's own question and keeps answering it for an addition too; with no order open
        // it stays unset, because "no order" is not "the plan was wrong" and a 0 there would say it was.
        if (facts.pullOrderOpen)
            aggro.expected = AggroReason::Expected(facts);

        // the same reason in words, for the eyes. One sentence per code, built from what the code was decided on.
        std::string const method = facts.pullOrderOpen ? std::string(Leader::MethodName(order.method)) : std::string();
        if (aggro.reason == "add")
            aggro.why = "a mob joined a fight already running: " + aggro.botName + " was in combat with somebody "
                        "other than " + aggro.mobName + " when it engaged";
        else if (aggro.reason == "estimate")
            aggro.why = "AN ERROR OF THE ESTIMATE: the pull of " + order.targetName + " by " + aggro.tankName + " (" +
                        method + ", " + std::to_string(order.expected) + " mob(s) expected) put " +
                        (facts.onThePlansTank ? std::string("a mob it did not count (") + aggro.mobName + ")"
                                              : aggro.mobName + " on " + aggro.botName + ", who is not its tank");
        else if (aggro.reason == "path")
            aggro.why = "hooked on the way: " + aggro.botName + " was walking and in combat with nobody else when " +
                        aggro.mobName + " engaged it, and no pull order named it";
        else if (facts.pullOrderOpen)
            aggro.why = "the issued pull arrived as planned: " + aggro.mobName + " engaged the tank it was pulled "
                        "by (" + method + ")";
        else
            aggro.why = "it came by itself: " + aggro.botName + " stood still and in combat with nobody else, and "
                        "no pull order named " + aggro.mobName;

        RecordAggro(aggro);
    }

    void NoteDeath(Player* killed, Creature* killer, std::string const& context, std::string const& threat,
                   DeathFacts const& facts)
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
        // [death476] AND THE IDENTITY BESIDE IT, because the counter above is not one: it is handed out at
        // runtime and is new on every boot. Measured: Mor'Ladim (entry 522) carries killer_low 3009 on b503
        // and 2195 on b504 while `creature` holds exactly one row for it, guid 5086 - and in that table 3009
        // is an Elder Crag Boar and 2195 a Bloodsail Warlock, so a JOIN on creature.guid answers with the
        // wrong creature. The spawn id is stable, so "one spawn killed three bots" becomes a GROUP BY.
        // 0 means this killer has no spawn row (a player, a summon, a temporary creature), and that is not
        // spawn 0, so it goes unset.
        if (ObjectGuid::LowType const spawn = killer->GetSpawnId())
            death.killerSpawnId = uint32(spawn);
        death.killerLevel = killer->GetLevel();
        death.killerHealthPct = float(killer->GetHealthPct());
        // the share's denominator, always beside it: health 0 of a pool of 544 is readable, "0 %" alone is not
        death.healthMax = killed->GetMaxHealth();
        // [runpast88] ABSENCE IS NOT ZERO, in both of the next two.
        //
        // DamageTaken only accumulates for a player whose map is a dungeon (DeathRecorder::IsTracked), so out in the
        // world there is nothing to read and a 0 here said "the killer dealt it nothing". All eight b502 deaths were
        // in the open world. The open-world measure is the other instrument's (SoloSafety's own damage events) and
        // travels as `fightDamageFromKiller`; two instruments, two columns, neither pretending to be the other.
        if (death.inDungeon)
            death.damageFromKiller = uint32(DamageTaken::From(killed->GetGUID(), killer->GetGUID()));
        // The kill clears the core's attacker list before this hook runs, so this count is 0 at every death ever
        // recorded - "cannot be measured at this instant" written as "nobody was on it". It stays as the honest
        // reading of that list and goes unset when the list is empty; `fightHitBy` is the count that answers.
        if (!killed->getAttackers().empty())
            death.attackers = uint32(killed->getAttackers().size());
        death.context = context;
        death.threat = threat;
        // [runpast88] the numbers the context sentence carried as words, in their own columns - the emitter held
        // every one of them when it built that sentence (SoloSafety::DeathContext, one pass under one lock)
        death.facts = facts;

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

        uint64 const deathRowId = RecordDeath(death);

        // [jrn40] A DEATH IS A PROBLEM, and in this project it is a CALCULATION BUG rather than bad luck - so it
        // belongs in the table that is driven to zero. The issue row points at pb_journal_death and copies none of
        // it: that table is the one owner of a death's inputs.
        //
        // `is_real` is "yes" with nothing to establish: this is the core's own "killed by a creature" hook, so the
        // death either happened or this code did not run. That is also what separates it from the 20 502 lines
        // whose tag begins with "[death" - those are the PREVENTION domain, and 17 of them were deaths.
        //
        // THE KEY IS NOT MEASURED, AND THE ROW SAYS SO. Seventeen deaths in a 2.69 h window is too few to tell a
        // right key from a lucky one by COUNT(DISTINCT), which is the check every other kind here passed. Not
        // connecting the detector would be worse than a wrong key: a wrong key is visible and gets fixed, while
        // zero rows read as "there were no deaths". So the key is the killer's entry plus the context sentence with
        // its numbers removed, `real_source` says the key is unmeasured, and the measurement happens on the next
        // run - if COUNT(DISTINCT) is not severalfold below COUNT(*), the key changes.
        Issue issue;
        issue.kind = "death";
        issue.severity = "high";
        // the inputs are all recorded and the judges are deterministic: the same conditions put back re-run them
        issue.reproClass = "decision";
        issue.dedupKey = "death|entry " + std::to_string(death.killerEntry) + "|" + WithoutNumbers(death.context);
        issue.why = Acore::StringFormat("{} L{} killed by {} L{} at {} % health; {}", death.botName,
                                        killed->GetLevel(), death.killerName, killer->GetLevel(),
                                        uint32(killer->GetHealthPct()), context);
        issue.realWhen = "the core's own killed-by-creature hook ran: the bot is dead and a creature killed it";
        issue.real = "yes";
        issue.realWhy = "a death is a calculation bug in this project, not a failure of luck - the estimate that "
                        "allowed the fight was wrong";
        // stated in the row rather than in a note: the deduplication key for this kind has NOT been measured
        issue.realSource = "emitter";
        issue.value = death.healthMax ? double(*death.healthMax) : 0.0;
        issue.valueUnit = "hp pool";
        issue.botName = death.botName;
        issue.botLow = death.botLow;
        issue.mapId = death.mapId;
        issue.zoneId = death.zoneId;
        issue.x = death.x;
        issue.y = death.y;
        issue.z = death.z;
        issue.runId = death.runId;
        issue.sourceTable = "pb_journal_death";
        issue.sourceId = deathRowId;
        // THE INPUTS THEMSELVES, flat, because pb_journal_death is swept in 48 h and this row lives for ever. The
        // emitter is holding every one of these at this instant; nothing is read a second time.
        // [runpast88] the fight's and the chase's own numbers, and a key is WRITTEN ONLY WHEN IT WAS HELD: the
        // replay reads a missing key as "not measured" (PbFixture::Flat::Num's default), while a 0 written in its
        // place would say the chase was free and nobody hit the bot. Without `runPastPricedHigh` the stand cannot
        // put a price against the damage at all - the sentence only ever carried the mean.
        std::string held;
        auto const number = [&held](char const* key, auto const& value)
        {
            if (value)
                held += Acore::StringFormat(",\"{}\":{:.2f}", key, double(*value));
        };
        number("fightHitBy", death.facts.hitBy);
        number("fightTaken", death.facts.takenThisFight);
        number("fightDamageFromKiller", death.facts.damageFromKiller);
        number("fightSec", death.facts.fightSec);
        number("runPastPriced", death.facts.runPastPriced);
        number("runPastPricedHigh", death.facts.runPastPricedHigh);
        number("runPastSec", death.facts.runPastSec);
        number("runPastHp", death.facts.runPastHp);
        number("runPastTaken", death.facts.runPastTaken);
        number("routeAgeMs", death.facts.routeAgeMs);
        number("routePriced", death.facts.routePriced);
        number("routeRefused", death.facts.routeRefused);
        number("routeUnseen", death.facts.routeUnseen);
        number("routeOtherFight", death.facts.routeOtherFight);
        if (death.facts.runPastOfItsChase)
            held += Acore::StringFormat(",\"runPastOfItsChase\":{}", *death.facts.runPastOfItsChase ? 1 : 0);
        if (death.facts.routeComplete)
            held += Acore::StringFormat(",\"routeComplete\":{}", *death.facts.routeComplete ? 1 : 0);
        issue.fixture = Acore::StringFormat(
            "{{\"killerEntry\":{},\"killerLow\":{},\"killerLevel\":{},\"killerHealthPct\":{:.1f},\"botLevel\":{},"
            "\"botClass\":{},\"healthMax\":{},\"damageFromKiller\":{},\"attackers\":{},\"judgedPull\":{},"
            "\"pullMethod\":\"{}\",\"map\":{},\"zone\":{},\"area\":{},\"x\":{:.2f},\"y\":{:.2f},\"z\":{:.2f}{},"
            "\"keyUnmeasured\":true}}",
            death.killerEntry, death.killerLow, killer->GetLevel(), killer->GetHealthPct(), killed->GetLevel(),
            uint32(killed->getClass()), death.healthMax ? *death.healthMax : 0,
            death.damageFromKiller ? *death.damageFromKiller : 0, death.attackers ? *death.attackers : 0,
            death.judgedPull && *death.judgedPull ? 1 : 0, death.pullMethod, death.mapId, death.zoneId,
            death.areaId, death.x, death.y, death.z, held);
        RecordIssue(issue);
    }
}  // namespace Journal
