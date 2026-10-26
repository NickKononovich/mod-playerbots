/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestHarnessMgr.h"

#include "AggressiveTargetValue.h"
#include "AiObjectContext.h"
#include "Creature.h"
#include "GameObject.h"  // [lead1]
#include "Engine.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "HealthTriggers.h"
#include "Group.h"
#include "LastMovementValue.h"
#include "LeaderOrders.h"  // [lead1]
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PullStrategy.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "StuckWatchdog.h"
#include "TestRunnerStrategy.h"
#include "Timer.h"
#include "DBCStores.h"  // [b31_simpct]

#include <algorithm>
#include <tuple>  // [b31_simpct]
#include <unordered_set>  // [lead1]
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace TestHarness
{
    namespace
    {
        constexpr uint32 HEARTBEAT_MS = 30 * IN_MILLISECONDS;
        constexpr uint32 STALL_MS = 2 * MINUTE * IN_MILLISECONDS;
        // PULL_WAIT_MS 45 s + BODY_PULL_WAIT_MS 10 s of the runner, and the walk in.
        constexpr uint32 STALL_BOSS_GRACE_MS = 90 * IN_MILLISECONDS;
        // Waiting at an event's gather spot is no stall for this long (Halls of Reflection wave 11: the intro and the
        // first wave took ~3 minutes, the watchdog skipped the target at 172 s).
        constexpr uint32 EVENT_GATHER_WAIT_MS = 5 * MINUTE * IN_MILLISECONDS;
        constexpr float PROGRESS_DISTANCE = 5.0f;
        constexpr char const* HEARTBEAT_FILE = "heartbeat.jsonl";
        // Suspicious kill: boss this many levels below the group, or dead this many times faster than expected
        // (only for bosses expected to last at least SUSPICIOUS_MIN_EXPECTED_SEC).
        constexpr uint32 SUSPICIOUS_LEVEL_GAP = 5;
        constexpr float SUSPICIOUS_KILL_SPEED = 4.0f;
        constexpr float SUSPICIOUS_MIN_EXPECTED_SEC = 5.0f;

        // Raid buffs worth listing in a pull snapshot (substring of the enUS spell name).
        std::vector<char const*> const raidBuffNames = {
            "Fortitude", "Divine Spirit", "Prayer of Spirit", "Shadow Protection", "of the Wild",
            "Arcane Intellect", "Arcane Brilliance", "Dalaran Intellect", "Dalaran Brilliance", "Blessing of Kings",
            "Blessing of Might", "Blessing of Wisdom", "Blessing of Sanctuary", "Battle Shout", "Commanding Shout",
            "Horn of Winter", "Trueshot Aura", "Unleashed Rage", "Leader of the Pack", "Moonkin Aura",
            "Strength of Earth", "Windfury Totem", "Wrath of Air", "Mana Spring", "Totem of Wrath",
            "Flametongue Totem", "Abomination's Might", "Rampage", "Elemental Oath", "Fel Intelligence",
            "Blood Pact", "Devotion Aura", "Retribution Aura", "Concentration Aura", "Replenishment",
            "Heroism", "Bloodlust", "Well Fed"};

        bool HasNamed(std::vector<std::string> const& buffs, char const* text)
        {
            for (std::string const& buff : buffs)
                if (buff.find(text) != std::string::npos)
                    return true;
            return false;
        }

        constexpr uint32 WATCHDOG_STALL_SECONDS = 120;

        uint32 CountItem(Player* bot, uint32 itemId) { return itemId ? bot->GetItemCount(itemId) : 0; }

        // Seconds the follow watchdog (StuckWatchdog) has seen this bot make no progress; its GIVE_UP stage only
        // logs "needs path fix", so the harness treats a long count as a stall reason.
        uint32 WatchdogStuckSeconds(Player* bot)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || !bot->IsAlive())
                return 0;
            auto* watchdog =
                dynamic_cast<StuckWatchdogValue*>(botAI->GetAiObjectContext()->GetUntypedValue("stuck watchdog"));
            return watchdog ? watchdog->GetStuckSeconds() : 0;
        }

        Member const* FindMember(Party const& party, ObjectGuid guid)
        {
            for (Member const& member : party.members)
                if (member.guid == guid)
                    return &member;
            return nullptr;
        }

        // Intention diagnostics: every watched member's engine hands its last tick to RecordIntent on the bot's own
        // map thread (Engine::intentSink, at most every second); RecordIntent writes a finished JSON object and a
        // one-line summary here, the harness (world thread) reads them back into "positions" and the heartbeat.
        // Plain strings under one lock: nothing reads another bot's AI context from another thread.
        constexpr uint32 INTENT_WATCH_MS = 60 * IN_MILLISECONDS;  // refreshed every run tick while the party runs
        constexpr uint32 INTENT_LOG_MS = 5 * IN_MILLISECONDS;     // "[intent]" line in Playerbots.log, per bot
        constexpr size_t INTENT_WHY_TOP = 5;

        struct IntentRecord
        {
            uint32 ms = 0;
            uint32 logMs = 0;
            std::string json;
        };

        std::mutex intentLock;
        std::unordered_map<ObjectGuid, IntentRecord> intents;

        void WatchIntent(Player* bot)
        {
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                uint32 const until = getMSTime() + INTENT_WATCH_MS;
                botAI->GetAiObjectContext()->intentWatchUntilMs.store(until ? until : 1, std::memory_order_relaxed);
            }
        }

        // The member's last intention object with its age, "" when it never recorded one.
        std::string IntentJsonFor(ObjectGuid guid)
        {
            std::lock_guard<std::mutex> lock(intentLock);
            auto itr = intents.find(guid);
            if (itr == intents.end() || itr->second.json.size() < 2)
                return "";

            return "{\"ageMs\":" + std::to_string(getMSTimeDiff(itr->second.ms, getMSTime())) + "," +
                   itr->second.json.substr(1);
        }

        // The planner's pull actions ("pull start" / "pull action" / "reach pull": a [pull plan], [pull one] or
        // [pull guarantee] being carried out), and when each tank last executed one (world thread, Heartbeat). The
        // pull_start label is "planned" only with one of them shortly before the fight (occ-h wave 9: a drake-phase
        // pull labelled "planned").
        std::unordered_map<ObjectGuid, uint32> plannerPullMs;

        bool IsPlannerPullAction(std::string const& name)
        {
            return name == "pull start" || name == "pull action" || name == "reach pull";
        }

        std::string IntentOrNull(ObjectGuid guid)
        {
            std::string const intent = IntentJsonFor(guid);
            return intent.empty() ? "null" : intent;
        }

        char const* MotionName(uint32 type)
        {
            switch (type)
            {
                case IDLE_MOTION_TYPE:
                    return "idle";
                case RANDOM_MOTION_TYPE:
                    return "random";
                case WAYPOINT_MOTION_TYPE:
                    return "waypoint";
                case CONFUSED_MOTION_TYPE:
                    return "confused";
                case CHASE_MOTION_TYPE:
                    return "chase";
                case HOME_MOTION_TYPE:
                    return "home";
                case FLIGHT_MOTION_TYPE:
                    return "flight";
                case POINT_MOTION_TYPE:
                    return "point";
                case FLEEING_MOTION_TYPE:
                    return "fleeing";
                case DISTRACT_MOTION_TYPE:
                    return "distract";
                case TIMED_FLEEING_MOTION_TYPE:
                    return "timed fleeing";
                case FOLLOW_MOTION_TYPE:
                    return "follow";
                case ROTATE_MOTION_TYPE:
                    return "rotate";
                case EFFECT_MOTION_TYPE:
                    return "effect (jump/knockback)";
                default:
                    return "other";
            }
        }

        char const* PriorityName(MovementPriority priority)
        {
            switch (priority)
            {
                case MovementPriority::MOVEMENT_IDLE:
                    return "idle";
                case MovementPriority::MOVEMENT_WANDER:
                    return "wander";
                case MovementPriority::MOVEMENT_NORMAL:
                    return "normal";
                case MovementPriority::MOVEMENT_COMBAT:
                    return "combat";
                case MovementPriority::MOVEMENT_FORCED:
                    return "forced";
            }
            return "?";
        }

        std::string CastResultName(SpellCastResult result)
        {
            switch (result)
            {
                case SPELL_FAILED_OUT_OF_RANGE:
                    return "OUT_OF_RANGE";
                case SPELL_FAILED_LINE_OF_SIGHT:
                    return "LINE_OF_SIGHT";
                case SPELL_FAILED_NOT_READY:
                    return "NOT_READY";
                case SPELL_FAILED_NO_POWER:
                    return "NO_POWER";
                case SPELL_FAILED_MOVING:
                    return "MOVING";
                case SPELL_FAILED_BAD_TARGETS:
                    return "BAD_TARGETS";
                case SPELL_FAILED_BAD_IMPLICIT_TARGETS:
                    return "BAD_IMPLICIT_TARGETS";
                case SPELL_FAILED_TARGETS_DEAD:
                    return "TARGETS_DEAD";
                case SPELL_FAILED_NOT_INFRONT:
                    return "NOT_INFRONT";
                case SPELL_FAILED_UNIT_NOT_INFRONT:
                    return "UNIT_NOT_INFRONT";
                case SPELL_FAILED_NOT_BEHIND:
                    return "NOT_BEHIND";
                case SPELL_FAILED_TOO_CLOSE:
                    return "TOO_CLOSE";
                case SPELL_FAILED_SPELL_IN_PROGRESS:
                    return "SPELL_IN_PROGRESS";
                case SPELL_FAILED_SILENCED:
                    return "SILENCED";
                case SPELL_FAILED_PACIFIED:
                    return "PACIFIED";
                case SPELL_FAILED_STUNNED:
                    return "STUNNED";
                case SPELL_FAILED_CASTER_AURASTATE:
                    return "CASTER_AURASTATE";
                case SPELL_FAILED_TARGET_AURASTATE:
                    return "TARGET_AURASTATE";
                case SPELL_FAILED_AURA_BOUNCED:
                    return "AURA_BOUNCED";
                case SPELL_FAILED_REAGENTS:
                    return "REAGENTS";
                case SPELL_FAILED_ONLY_SHAPESHIFT:
                    return "ONLY_SHAPESHIFT";
                case SPELL_FAILED_NOT_SHAPESHIFT:
                    return "NOT_SHAPESHIFT";
                case SPELL_FAILED_TARGET_FRIENDLY:
                    return "TARGET_FRIENDLY";
                case SPELL_FAILED_TARGET_ENEMY:
                    return "TARGET_ENEMY";
                case SPELL_FAILED_IMMUNE:
                    return "IMMUNE";
                case SPELL_FAILED_TRY_AGAIN:
                    return "TRY_AGAIN";
                case SPELL_FAILED_NOT_STANDING:
                    return "NOT_STANDING";
                case SPELL_FAILED_NO_COMBO_POINTS:
                    return "NO_COMBO_POINTS";
                case SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW:
                    return "CANT_DO_THAT_RIGHT_NOW";
                case SPELL_FAILED_AFFECTING_COMBAT:
                    return "AFFECTING_COMBAT";
                case SPELL_FAILED_NOT_MOUNTED:
                    return "NOT_MOUNTED";
                case SPELL_FAILED_ONLY_OUTDOORS:
                    return "ONLY_OUTDOORS";
                case SPELL_FAILED_CASTER_DEAD:
                    return "CASTER_DEAD";
                default:
                    return "code " + std::to_string(int32(result));
            }
        }

        // Why a spell action did not run, checked in PlayerbotAI::CanCastSpell's order, then Spell::CheckCast.
        std::string CastDetail(PlayerbotAI* botAI, Action* action, bool useless)
        {
            CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
            if (!cast)
                return "";

            Player* bot = botAI->GetBot();
            AiObjectContext* context = botAI->GetAiObjectContext();
            std::string const spell = cast->getSpell();
            Value<uint32>* idValue = context->GetValue<uint32>("spell id", spell);
            uint32 const spellId = idValue ? idValue->Get() : 0;
            Unit* target = cast->GetTarget();
            if (!target)
                return ": no target (" + cast->GetTargetName() + " is empty)";
            if (!target->IsInWorld() || target->GetMap() != bot->GetMap())
                return ": target " + target->GetName() + " not on this map";

            std::string const on =
                Acore::StringFormat(": {} on {} {:.1f} yd", spell, target->GetName(), bot->GetDistance(target));
            if (!spellId)
                return on + " - spell not known (no spell id)";
            if (useless)
            {
                Value<bool>* usefulValue = context->GetValue<bool>("spell cast useful", spell);
                if (usefulValue && !usefulValue->Get())
                    return on + " - spell cast useful = false";
                return on + " - the action's own isUseful() check (aura present / health threshold / target)";
            }
            if (bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
                return on + " - lost control";
            Pet* pet = bot->GetPet();
            if (!(pet && pet->HasSpell(spellId)) && !bot->HasSpell(spellId))
                return on + " - bot lacks spell " + std::to_string(spellId);
            if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                return on + " - channeling";
            if (bot->HasSpellCooldown(spellId))
                return on + " - on cooldown";
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                return on + " - no spell info";
            uint32 const castTime = !spellInfo->IsChanneled() ? spellInfo->CalcCastTime(bot) : spellInfo->GetDuration();
            if ((castTime || spellInfo->IsAutoRepeatRangedSpell()) && bot->isMoving())
                return on + Acore::StringFormat(" - moving (cast time {} ms)", castTime);
            if (target->IsImmunedToSpell(spellInfo))
                return on + " - target immune";
            if (spellInfo->PowerType < MAX_POWERS)
            {
                int32 const cost = spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask());
                int32 const have = int32(bot->GetPower(Powers(spellInfo->PowerType)));
                if (cost > have)
                    return on + Acore::StringFormat(" - not enough power ({} of {})", have, cost);
            }
            if (target != bot)
            {
                float const maxRange = bot->GetSpellMaxRangeForTarget(target, spellInfo);
                if (maxRange > 0.0f && bot->GetDistance(target) > maxRange)
                    return on + Acore::StringFormat(" - out of range ({:.1f} > {:.1f})", bot->GetDistance(target),
                                                    maxRange);
                if (!bot->IsWithinLOSInMap(target))
                    return on + " - no line of sight";
            }

            Unit* oldSelection = bot->GetSelectedUnit();
            Spell* check = new Spell(bot, spellInfo, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
            check->m_targets.SetUnitTarget(target);
            SpellCastResult const result = check->CheckCast(true);
            delete check;
            if (oldSelection)
                bot->SetSelection(oldSelection->GetGUID());
            if (result != SPELL_CAST_OK)
                return on + " - CheckCast " + CastResultName(result);
            return on + " - CheckCast ok";
        }

        std::string IntentReason(PlayerbotAI* botAI, IntentCandidate const& candidate, IntentCandidate const* executed,
                                 bool minimal)
        {
            switch (candidate.outcome)
            {
                case INTENT_UNKNOWN:
                    return "unknown action";
                case INTENT_USELESS:
                    return "isUseful false" + CastDetail(botAI, candidate.action, true);
                case INTENT_MULTIPLIER:
                    return Acore::StringFormat("multiplier {} x{:.2f}", candidate.multiplier,
                                               candidate.multiplierValue);
                case INTENT_IMPOSSIBLE:
                    return "isPossible false" + CastDetail(botAI, candidate.action, false);
                case INTENT_PREREQ:
                    return "its prerequisite was pushed first";
                case INTENT_FAILED:
                    return "Execute returned false" + CastDetail(botAI, candidate.action, false);
                case INTENT_MINIMAL:
                    return "minimal tick (AllowActivity false): relevance below 100 is skipped";
                default:
                    break;
            }
            if (minimal)
                return "not evaluated (minimal tick)";
            if (executed)
                return Acore::StringFormat("not evaluated: {} ({:.2f}) ran first", executed->name,
                                           executed->relevance);
            return "not evaluated (tick iteration budget)";
        }

        // Runs on the bot's own map thread (Engine::DoNextAction of a watched bot).
        void RecordIntent(PlayerbotAI* botAI, Engine* engine)
        {
            Player* bot = botAI->GetBot();
            if (!bot || !bot->IsInWorld())
                return;

            uint32 const now = getMSTime();
            AiObjectContext* context = botAI->GetAiObjectContext();
            std::vector<IntentCandidate> const& candidates = engine->GetIntentCandidates();
            IntentExecuted const& done = engine->GetIntentExecuted();
            bool const minimal = engine->WasIntentTickMinimal();

            IntentCandidate const* executed = nullptr;
            for (IntentCandidate const& candidate : candidates)
                if (candidate.outcome == INTENT_EXECUTED)
                    executed = &candidate;

            // doing: the last executed action of this engine, who pushed it, its target.
            std::string doing = "nothing executed yet";
            if (done.ms)
            {
                doing = Acore::StringFormat("{} ({:.2f})", done.name, done.relevance);
                if (!done.target.empty())
                    doing += done.targetDistance >= 0.0f
                                 ? Acore::StringFormat(" -> {} {:.1f} yd", done.target, done.targetDistance)
                                 : " -> " + done.target;
                doing += " [" + (done.by.empty() ? std::string("?") : done.by) + "]";
                doing += Acore::StringFormat(" {:.1f}s ago", getMSTimeDiff(done.ms, now) / 1000.0f);
            }
            std::string const tick =
                minimal ? "minimal" : (engine->DidIntentTickExecute() ? "executed" : "nothing ran");

            // goingTo: the running spline's end, else the last MoveTo; the generator; the action that moved it.
            JsonObject going;
            std::string goingText;
            MotionMaster* motion = bot->GetMotionMaster();
            char const* generator = MotionName(motion->GetCurrentMovementGeneratorType());
            going.Str("gen", generator).Bool("moving", bot->isMoving());
            Value<LastMovement&>* lastValue = context->GetValue<LastMovement&>("last movement");
            LastMovement* last = lastValue ? &lastValue->Get() : nullptr;
            if (!bot->movespline->Finalized())
            {
                G3D::Vector3 const dest = bot->movespline->FinalDestination();
                float const distance = bot->GetExactDist(dest.x, dest.y, dest.z);
                going.Str("src", "spline").Real("x", dest.x).Real("y", dest.y).Real("z", dest.z).Real("d", distance);
                goingText = Acore::StringFormat("{} to ({:.1f},{:.1f},{:.1f}) {:.1f} yd", generator, dest.x, dest.y,
                                                dest.z, distance);
            }
            else if (last && last->msTime && getMSTimeDiff(last->msTime, now) < 30 * IN_MILLISECONDS)
            {
                float const distance = bot->GetExactDist(last->lastMoveToX, last->lastMoveToY, last->lastMoveToZ);
                going.Str("src", "last move to").Real("x", last->lastMoveToX).Real("y", last->lastMoveToY)
                    .Real("z", last->lastMoveToZ).Real("d", distance)
                    .Real("lastAgoSec", getMSTimeDiff(last->msTime, now) / 1000.0f);
                goingText = Acore::StringFormat("{}, stopped; last move to {:.1f} yd", generator, distance);
            }
            else
                goingText = std::string(generator) + ", standing";
            if (last)
                going.Str("prio", PriorityName(last->priority));
            if (last && last->reasonMs && last->reasonMs == last->msTime && !last->reason.empty())
            {
                going.Str("reason", last->reason);
                goingText += " [" + last->reason + "]";
            }
            if (!engine->GetIntentMover().empty())
            {
                float const ago = getMSTimeDiff(engine->GetIntentMoverMs(), now) / 1000.0f;
                going.Str("by", engine->GetIntentMover()).Real("byAgoSec", ago);
                goingText += Acore::StringFormat(" (by {} {:.1f}s ago)", engine->GetIntentMover(), ago);
            }

            // why: the highest-relevance actions of the last tick that did not run.
            std::vector<IntentCandidate const*> rejected;
            for (IntentCandidate const& candidate : candidates)
                if (candidate.outcome != INTENT_EXECUTED)
                    rejected.push_back(&candidate);
            std::sort(rejected.begin(), rejected.end(), [](IntentCandidate const* left, IntentCandidate const* right)
                      { return left->relevance > right->relevance; });
            if (rejected.size() > INTENT_WHY_TOP)
                rejected.resize(INTENT_WHY_TOP);
            JsonArray why;
            std::string whyText;
            for (IntentCandidate const* candidate : rejected)
            {
                std::string const reason = IntentReason(botAI, *candidate, executed, minimal);
                why.Raw(JsonObject().Str("a", candidate->name).Real("r", candidate->relevance).Str("by", candidate->by)
                            .Str("why", reason).Build());
                whyText += (whyText.empty() ? "" : "; ") + candidate->name + " (" + reason + ")";
            }

            // combat
            std::string state = "?";
            switch (botAI->GetState())
            {
                case BOT_STATE_COMBAT:
                    state = "combat";
                    break;
                case BOT_STATE_NON_COMBAT:
                    state = "non combat";
                    break;
                case BOT_STATE_DEAD:
                    state = "dead";
                    break;
                default:
                    break;
            }
            JsonObject combat;
            combat.Bool("in", bot->IsInCombat()).Str("state", state)
                .Num("attackers", int64(bot->getAttackers().size()));
            std::string combatText = Acore::StringFormat("{}, {} attackers", state, bot->getAttackers().size());
            Value<Unit*>* currentValue = context->GetValue<Unit*>("current target");
            if (Unit* current = currentValue ? currentValue->Get() : nullptr)
            {
                bool const here = current->IsInWorld() && current->GetMap() == bot->GetMap();
                Unit* itsVictim = here ? current->GetVictim() : nullptr;
                combat.Str("target", current->GetName()).Real("targetDist", here ? bot->GetDistance(current) : -1.0f)
                    .Str("targetVictim", itsVictim ? itsVictim->GetName() : "");
                combatText += ", target " + current->GetName() + (itsVictim ? " on " + itsVictim->GetName() : "");
            }
            if (Unit* victim = bot->GetVictim())
                combat.Str("meleeVictim", victim->GetName());

            // holds: gates the bot is under, as far as its own context knows them.
            JsonArray holds;
            std::string holdsText;
            auto hold = [&holds, &holdsText](std::string const& text)
            {
                holds.Str(text);
                holdsText += (holdsText.empty() ? "" : ", ") + text;
            };
            for (char const* name : {"pull hold", "pull countdown", "group paused"})
                if (Value<bool>* value = dynamic_cast<Value<bool>*>(context->GetUntypedValue(name)))
                    if (value->Get())
                        hold(name);
            if (LosPull::IsHolding(bot))
                hold("los pull hold");
            if (PrePullCc::IsHolding(bot))
                hold("pre-pull cc hold");
            if (RestState::IsEating(bot))  // a food aura, not a regen buff
                hold("eating");
            if (RestState::IsDrinking(bot))  // Blessing of Wisdom has SPELL_AURA_MOD_POWER_REGEN too
                hold("drinking");
            if (bot->HasAura(15007))
                hold("resurrection sickness");
            TestRunner::Waypoint waypoint;
            if (TestRunner::GetWaypoint(bot->GetGUID(), waypoint) && waypoint.mapId == bot->GetMapId())
                hold(Acore::StringFormat("harness waypoint {:.1f} yd",
                                         bot->GetExactDist(waypoint.x, waypoint.y, waypoint.z)));
            if (minimal)
                hold("minimal ticks (AllowActivity false)");

            JsonObject intent;
            intent.Str("tick", tick).Str("doing", doing).Raw("goingTo", going.Build()).Raw("why", why.Build())
                .Raw("combat", combat.Build()).Raw("holds", holds.Build());

            // The compact line, while the bot or its group fights.
            bool fighting = bot->IsInCombat();
            if (!fighting)
                if (Group* group = bot->GetGroup())
                    for (GroupReference* ref = group->GetFirstMember(); ref && !fighting; ref = ref->next())
                        if (Player* member = ref->GetSource())
                            fighting = member != bot && member->IsInWorld() && member->GetMap() == bot->GetMap() &&
                                       member->IsInCombat();

            bool logNow = false;
            {
                std::lock_guard<std::mutex> lock(intentLock);
                IntentRecord& record = intents[bot->GetGUID()];
                record.ms = now;
                record.json = intent.Build();
                if (fighting && (!record.logMs || getMSTimeDiff(record.logMs, now) >= INTENT_LOG_MS))
                {
                    record.logMs = now;
                    logNow = true;
                }
            }
            if (logNow)
                LOG_DEBUG("playerbots",
                          "[intent] {} hp {:.0f}% mp {:.0f}% | {} | tick {} | doing {} | going {} | why: {} "
                          "| holds: {}",
                          bot->GetName(), bot->GetHealthPct(),
                          bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 0.0f, combatText, tick, doing,
                          goingText, whyText.empty() ? "-" : whyText, holdsText.empty() ? "-" : holdsText);
        }

        struct IntentSinkInstaller
        {
            IntentSinkInstaller() { Engine::intentSink = &RecordIntent; }
        } const intentSinkInstaller;
    }

    std::string MemberIntentJson(ObjectGuid guid) { return IntentOrNull(guid); }  // [b30_reenter]

    // [b31_simpct] Inputs for tools/simpct.py (wowsims "% of sim"): equipped gear, talent link, glyphs, race.
    // gear rows: [slot, entry, permEnchant, gem1, gem2, gem3, prismatic, randomPropertyId]; written once per member.
    // Wowhead talent link from the active spec's talent map (HasTalent; HasSpell misses learned talent ranks):
    // tabs by TalentTab.tabpage, talents by (Row, Col), trailing zeros and dashes trimmed.
    static std::string SimTalentLink(Player* bot)
    {
        std::vector<std::pair<uint32, uint32>> tabs;
        for (uint32 i = 0; i < sTalentTabStore.GetNumRows(); ++i)
            if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(i))
                if ((tab->ClassMask & bot->getClassMask()) && !tab->petTalentMask)
                    tabs.emplace_back(tab->tabpage, tab->TalentTabID);
        std::sort(tabs.begin(), tabs.end());

        std::string link;
        for (size_t t = 0; t < tabs.size(); ++t)
        {
            std::vector<std::tuple<uint32, uint32, uint32>> cells;
            for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
            {
                TalentEntry const* info = sTalentStore.LookupEntry(i);
                if (!info || info->TalentTab != tabs[t].second)
                    continue;
                uint32 rank = 0;
                for (uint8 r = 0; r < MAX_TALENT_RANK; ++r)
                    if (info->RankID[r] && bot->HasTalent(info->RankID[r], bot->GetActiveSpec()))
                        rank = r + 1;
                cells.emplace_back(info->Row, info->Col, rank);
            }
            std::sort(cells.begin(), cells.end());
            std::string tree;
            for (auto const& cell : cells)
                tree += char('0' + std::get<2>(cell));
            while (!tree.empty() && tree.back() == '0')
                tree.pop_back();
            if (t)
                link += '-';
            link += tree;
        }
        while (!link.empty() && link.back() == '-')
            link.pop_back();
        return link;
    }

    static JsonObject SimSnapshotJson(ObjectGuid guid)
    {
        JsonObject out;
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot)
            return out.Bool("simSnapshot", false);

        JsonArray gear;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            JsonArray row;
            row.Num(slot).Num(item->GetEntry()).Num(item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))
                .Num(item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT))
                .Num(item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_2))
                .Num(item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_3))
                .Num(item->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT)).Num(item->GetItemRandomPropertyId());
            gear.Raw(row.Build());
        }

        JsonArray glyphs;
        for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
            glyphs.Num(bot->GetGlyph(i));

        return out.Bool("simSnapshot", true).Num("guid", bot->GetGUID().GetCounter()).Num("race", bot->getRace())
            .Raw("gear", gear.Build()).Str("talents", SimTalentLink(bot)).Raw("glyphs", glyphs.Build());
    }

    float ExpectedGroupDps(Party const& party)
    {
        float total = 0.0f;
        for (Member const& member : party.members)
            if (member.spec)
                total += ExpectedMemberDps(member.spec->role, member.prep.level ? member.prep.level : party.level,
                                           member.prep.avgIlvl, party.content ? party.content->size : 5);
        return total;
    }

    JsonObject CombatStatsJson(MemberCounters const& now, MemberCounters const& start, uint32 combatMs)
    {
        // Per second of the member's own combat time (alive and in combat), not of the wall clock: travel,
        // buffing and waiting between pulls do not dilute it.
        double const seconds = double(combatMs) / double(IN_MILLISECONDS);
        double const perSecond = seconds > 0.0 ? 1.0 / seconds : 0.0;
        uint64 const damage = now.damageDone - std::min(now.damageDone, start.damageDone);
        uint64 const healing = now.healingDone - std::min(now.healingDone, start.healingDone);
        uint64 const taken = now.damageTaken - std::min(now.damageTaken, start.damageTaken);

        // Rotation review: the member's own in-combat casts in the window, top 25 by count, and the share of its
        // combat time spent casting / on the GCD (busyPct, capped at 100).
        struct CastRow
        {
            uint32 id;
            uint32 casts;
            uint64 damage;
            uint64 healing;
        };
        std::vector<CastRow> rows;
        for (auto const& [spellId, counters] : now.spells)
        {
            auto itr = start.spells.find(spellId);
            SpellCounters const before = itr == start.spells.end() ? SpellCounters() : itr->second;
            CastRow const row{spellId, counters.casts - std::min(counters.casts, before.casts),
                              counters.damage - std::min(counters.damage, before.damage),
                              counters.healing - std::min(counters.healing, before.healing)};
            if (row.casts || row.damage || row.healing)
                rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(), [](CastRow const& left, CastRow const& right)
                  { return left.casts != right.casts ? left.casts > right.casts : left.damage > right.damage; });
        if (rows.size() > 25)
            rows.resize(25);

        JsonArray casts;
        for (CastRow const& row : rows)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(row.id);
            casts.Raw(JsonObject().Num("id", row.id)
                          .Str("name", info && info->SpellName[LOCALE_enUS] ? info->SpellName[LOCALE_enUS] : "")
                          .Num("n", row.casts).Num("dmg", int64(row.damage)).Num("heal", int64(row.healing)).Build());
        }

        uint64 const busyMs = now.busyMs - std::min(now.busyMs, start.busyMs);
        double const busyPct = combatMs ? std::min(100.0, 100.0 * double(busyMs) / double(combatMs)) : 0.0;
        return JsonObject().Real("activeSec", seconds).Real("activeDps", double(damage) * perSecond)
            .Real("activeHps", double(healing) * perSecond).Real("activeDtps", double(taken) * perSecond)
            .Real("busyPct", busyPct).Raw("casts", casts.Build());
    }

    std::string TestHarnessMgr::PullMembersJson(Party& party)
    {
        JsonArray members;
        for (Member const& member : party.members)
        {
            MemberCounters const current = StatsRegistry::Instance().Snapshot(member.guid);
            MemberCounters const& start = member.pullStart;
            uint32 const combatMs = member.combatMs - std::min(member.combatMs, member.pullStartCombatMs);
            uint64 const damage = current.damageDone - std::min(current.damageDone, start.damageDone);
            uint64 const healing = current.healingDone - std::min(current.healingDone, start.healingDone);
            uint64 const taken = current.damageTaken - std::min(current.damageTaken, start.damageTaken);
            members.Raw(JsonObject().Str("name", member.name).Str("spec", member.spec->label)
                            .Str("role", RoleName(member.spec->role)).Num("damage", int64(damage))
                            .Num("healing", int64(healing)).Num("damageTaken", int64(taken))
                            .Merge(CombatStatsJson(current, start, combatMs)).Build());
        }
        return members.Build();
    }

    // [lead1] The run events that are a leader decision (decides: it competes with the tank's own decision about the
    // mob - a pull, a walk, a hold, a skip) or a leader report (pull_hold: the leader yields the tank to its pull).
    static bool LeaderDecisionEvent(std::string const& type, bool& decides)
    {
        static std::unordered_set<std::string> const deciding = {
            "target", "target_commit", "skip_target", "skip_lifted", "route_blocker_body_pull", "guard_body_pull",
            "pull_after_hold", "body_pull_planner_request", "body_pull_planner_timeout", "boss_on_the_way_first",
            "behind_root_pull", "approach_sight", "approach_stop", "guard_approach", "patrol_step_back",
            "patrol_wait_spot", "patrol_hold", "patrol_handover", "harness_walk_halt", "stall_short_step",
            "steer_stuck", "pull_hold_stall", "harness_hold", "harness_hold_refused_pack", "guarantee_hold",
            "blocker_pull_gate", "held_for_now_cap", "rest_hold", "trash_stalemate", "stale_idle_victim_release",
            "aoe_pull_plan_accepted", "pull_alone_first", "pack_too_big", "route_blocker_walk_through",
            "route_blocker_required_only", "detour_fallback", "boss_order_hold", "meet_member", "assist_member",
            "regroup", "pull_option", "stall_recovery", "regen_stall_release",
            // [lead3] [lead4] [lead10] the leader's own decisions around its planner (C:/WoW/LEADER_REFACTOR_PLAN.md 9)
            "leader_whole_pack", "leader_boss_pull", "leader_patrol_wait", "leader_patrol_pull",
            "leader_skip_no_option", "plan_through_door", "door_far_side_priced",
            "plan_door_step",  // [lead11]
            "leader_unstuck"};  // [lead14]
        static std::unordered_set<std::string> const reporting = {"pull_hold"};
        if (deciding.count(type))
        {
            decides = true;
            return true;
        }
        decides = false;
        return reporting.count(type) != 0;
    }

    void TestHarnessMgr::LogEvent(Party& party, std::string const& type, JsonObject fields)
    {
        uint32 const now = getMSTime();
        JsonObject line;
        line.Str("type", type).Str("time", WallClock()).Str("runId", party.id)
            .Str("content", party.content ? party.content->key : "")
            .Str("comp", party.comp ? party.comp->id : "")
            .Num("elapsedSec", party.runStartMs ? getMSTimeDiff(party.runStartMs, now) / IN_MILLISECONDS : 0);
        line.Merge(fields);
        AppendLine(party.id + ".jsonl", line.Build());

        if (type == "stall" || type == "stall_recovery" || type == "run_end" || type == "run_start")
            AppendLine(HEARTBEAT_FILE, line.Build());

        // [lead1] The leader's own holds, body pulls, walks, skips and target choices are decision lines too
        // (C:/WoW/LEADER_REFACTOR_PLAN.md step 1: "the harness logs its own holds and body pulls as decisions"),
        // about the route target. One hook here instead of one per site.
        bool decides = false;
        if (Leader::LEADER_DECISION_LOG && LeaderDecisionEvent(type, decides))
        {
            std::string mobName;
            if (!party.target.IsEmpty())
                if (Player* leader = ObjectAccessor::FindPlayer(party.leader); leader && leader->IsInWorld())
                {
                    if (Creature* creature = leader->GetMap()->GetCreature(party.target))
                        mobName = creature->GetName();
                    else if (GameObject* object = leader->GetMap()->GetGameObject(party.target))
                        mobName = object->GetName();
                }
            DecisionLog::Instance().Write(DecisionRun{party.id, party.seq, party.runStartMs}, "leader", type,
                                          party.target, mobName, decides, fields, now);
        }
    }

    void TestHarnessMgr::LogRunStart(Party& party)
    {
        JsonArray members;
        for (Member const& member : party.members)
        {
            JsonArray professions;
            professions.Str(ProfessionName(member.professions.first)).Str(ProfessionName(member.professions.second));
            JsonArray perks;
            for (std::string const& perk : member.prep.perks)
                perks.Str(perk);

            members.Raw(JsonObject().Str("name", member.name).Str("class", ClassName(member.cls))
                            .Str("spec", member.spec->label).Str("role", RoleName(member.spec->role))
                            .Num("subgroup", member.slot.subgroup)
                            .Num("level", member.prep.level)
                            .Num("professionSkill", member.prep.professionSkill)
                            .Bool("mainTank", member.slot.flags & SLOT_MAIN_TANK)
                            .Bool("offTank", member.slot.flags & SLOT_OFF_TANK)
                            .Real("avgIlvl", member.prep.avgIlvl).Real("coreAvgIlvl", member.prep.coreAvgIlvl)
                            .Num("gearScore", member.prep.gearScore).Num("ilvlCap", member.prep.ilvlCap)
                            .Num("gearPasses", member.prep.gearPasses).Num("specNo", member.prep.specNo)
                            .Raw("professions", professions.Build()).Raw("perks", perks.Build())
                            .Num("dragonsEyes", member.prep.dragonsEyes)
                            // [b31_dk_talents]
                            .Num("talentsSpent", member.prep.talentsSpent)
                            .Num("talentsExpected", member.prep.talentsExpected)
                            .Num("talentsFree", member.prep.talentsFree).Num("talentsMap", member.prep.talentsMap)
                            .Merge(SimSnapshotJson(member.guid))  // [b31_simpct]
                            .Build());
        }

        std::vector<std::string> const gaps = CoverageGaps(*party.comp);
        JsonArray gapList;
        for (std::string const& gap : gaps)
            gapList.Str(gap);

        JsonArray starters;
        for (StarterTrack const& starter : party.starters)
            starters.Str(starter.def->note);
        JsonArray spots;
        for (EventSpotDef const* spot : EventSpots(party.content->mapId))
            spots.Str(spot->note);
        // User rule: excluded bosses are named with their reason, never dropped silently.
        JsonArray classGated;
        for (ClassGatedBossDef const* gate : ClassGatedBosses(party.content->mapId, party.content->difficulty))
            classGated.Str(Acore::StringFormat("{} ({})", gate->reason, gate->entry));
        JsonArray absentSlots;
        for (AbsentSlotDef const* slot : AbsentScriptSlots(party.content->mapId, party.content->difficulty))
            absentSlots.Str(Acore::StringFormat("slot {}: {}", slot->bossId, slot->reason));
        JsonArray eventItems;
        for (EventItemDef const* item : EventItems(party.content->mapId))
            eventItems.Raw(JsonObject().Num("item", item->itemId).Num("count", item->count)
                               .Str("source", item->source).Build());

        // Route order and identity of every tracked boss (bossId -> DungeonEncounter.dbc order, creature entry).
        JsonArray bossList;
        for (BossTrack const& boss : party.bosses)
            bossList.Raw(JsonObject().Num("bossId", boss.bossId).Str("name", boss.name).Num("entry", boss.entry)
                             .Num("order", boss.order).Bool("dbc", boss.dbc).Bool("killed", boss.killed)
                             .Str("needs", TrackRequirement(party.content->mapId, boss)
                                               ? TrackRequirement(party.content->mapId, boss) : "")
                             .Str("classGated", TrackClassGate(party, boss) ? TrackClassGate(party, boss) : "")
                             .Build());

        JsonArray skippedBosses;
        for (std::string const& line : party.bossCreaturesSkipped)
            skippedBosses.Str(line);

        JsonArray phantomSlots;
        for (BossTrack const& boss : party.bosses)
            if (IsPhantomSlot(party, boss))
                phantomSlots.Num(boss.bossId);

        float avg = 0.0f;
        for (Member const& member : party.members)
            avg += member.prep.avgIlvl;
        if (!party.members.empty())
            avg /= float(party.members.size());

        // User rule: prepared gear is below what the dungeon drops (a reason to run it). contentIlvl is the boss
        // loot's item level, the target at most contentIlvl - 10. Only token dungeons (TBC / WotLK heroics: badges /
        // emblems buy above the drops) may be at or above. Checked here, logged as a warning, the run goes on.
        bool const tokenDungeon = !strcmp(party.content->tier, "tbc-h") || !strcmp(party.content->tier, "wotlk-h");
        uint32 const gearCap = party.content->ilvl - std::min<uint32>(party.content->ilvl, std::clamp<uint32>((party.content->ilvl * 15 + 50) / 100, 3, 10));
        if (!tokenDungeon && !party.content->raid &&
            (party.targetIlvl > gearCap || avg > float(gearCap) + 0.5f))
            LogEvent(party, "gear_above_content",
                     JsonObject().Num("contentIlvl", party.content->ilvl).Num("cap", gearCap)
                         .Num("targetIlvl", party.targetIlvl).Real("groupAvgIlvl", avg)
                         .Str("rule", "prepared gear must stay at or below contentIlvl - clamp(15 %, 3, 10) (boss loot)"));

        LogEvent(party, "run_start",
                 JsonObject().Str("contentName", party.content->name).Str("tier", party.content->tier)
                     .Num("map", party.content->mapId).Num("difficulty", party.content->difficulty)
                     .Num("size", party.content->size).Num("contentIlvl", party.content->ilvl)
                     .Num("targetIlvl", party.targetIlvl).Real("groupAvgIlvl", avg).Num("level", party.level)
                     .Str("variant", party.comp->id).Str("variantNote", party.comp->note)
                     .Bool("harnessMovesBots", HARNESS_MOVES_BOTS)
                     .Num("instanceId", party.instanceId).Num("bossesTotal", RequiredBossCount(party))
                     // [hleft-daily-reset] seconds until the server's scheduled reset of this instance, -1: none
                     .Num("resetInSec", party.instanceResetAt
                                            ? int64(party.instanceResetAt) - int64(GameTime::GetGameTime().count())
                                            : int64(-1))
                     .Num("bossCreatures", int64(party.bossCreatures.size()))
                     .Raw("bossCreaturesSkipped", skippedBosses.Build()).Raw("phantomSlots", phantomSlots.Build())
                     .Num("encountersDbc", int64(party.encounters.size())).Raw("bosses", bossList.Build())
                     .Num("areatriggers", int64(party.triggers.size())).Raw("eventStarters", starters.Build())
                     .Raw("eventSpots", spots.Build()).Raw("eventItems", eventItems.Build())
                     .Raw("classGated", classGated.Build()).Raw("absentSlots", absentSlots.Build())
                     .Raw("coverageGaps", gapList.Build()).Raw("members", members.Build()));

        for (std::string const& gap : gaps)
            LogEvent(party, "comp_gap", JsonObject().Str("gap", gap));

        for (KnownGapDef const* gap : KnownGaps(party.content->mapId))
            LogEvent(party, "known_gap", JsonObject().Str("note", gap->note));
    }

    std::string TestHarnessMgr::MemberBuffJson(Party& party, Player* bot)
    {
        Member const* member = FindMember(party, bot->GetGUID());
        std::vector<std::string> buffs;
        uint32 elixirs = 0;
        for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
        {
            if (!aurApp || !aurApp->IsPositive())
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            char const* name = info ? info->SpellName[LOCALE_enUS] : nullptr;
            if (!name)
                continue;
            if (sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_BATTLE) ||
                sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_GUARDIAN))
                ++elixirs;
            for (char const* wanted : raidBuffNames)
                if (strstr(name, wanted))
                {
                    buffs.push_back(name);
                    break;
                }
        }

        uint32 priests = 0, druids = 0, mages = 0, paladins = 0, warriors = 0, knights = 0;
        bool disc = false;
        for (Member const& other : party.members)
        {
            switch (other.cls)
            {
                case CLASS_PRIEST:
                    ++priests;
                    disc = disc || std::string(other.spec->key) == "disc-priest";
                    break;
                case CLASS_DRUID:
                    ++druids;
                    break;
                case CLASS_MAGE:
                    ++mages;
                    break;
                case CLASS_PALADIN:
                    ++paladins;
                    break;
                case CLASS_WARRIOR:
                    ++warriors;
                    break;
                case CLASS_DEATH_KNIGHT:
                    ++knights;
                    break;
                default:
                    break;
            }
        }

        bool const manaUser = member && UsesMana(member->spec->role, member->cls);
        bool const physical = member && (member->spec->role == Role::Melee || member->spec->role == Role::Ranged ||
                                         member->spec->role == Role::Tank);
        JsonArray missing;
        auto expect = [&missing, &buffs](bool wanted, std::vector<char const*> const& any, char const* label)
        {
            if (!wanted)
                return;
            for (char const* name : any)
                if (HasNamed(buffs, name))
                    return;
            missing.Str(label);
        };

        expect(priests > 0, {"Fortitude"}, "Fortitude");
        expect(priests > 0 && disc, {"Spirit"}, "Divine Spirit");
        expect(priests > 0, {"Shadow Protection"}, "Shadow Protection");
        expect(druids > 0, {"of the Wild"}, "Mark of the Wild");
        expect(mages > 0 && manaUser, {"Intellect", "Brilliance"}, "Arcane Intellect");
        expect(paladins > 0, {"Kings", "Sanctuary"}, "Blessing of Kings");
        expect(paladins > 1 && physical, {"Might", "Battle Shout"}, "Blessing of Might");
        expect(paladins > 1 && manaUser, {"Wisdom", "Mana Spring"}, "Blessing of Wisdom");
        expect(warriors > 0, {"Shout"}, "Battle/Commanding Shout");
        expect(knights > 0 && physical, {"Horn of Winter", "Strength of Earth"}, "Horn of Winter");

        JsonArray buffList;
        for (std::string const& buff : buffs)
            buffList.Str(buff);

        // Consumables of the party's level; a kind that does not exist at the level (no flask below 50, no combat
        // potion for physical roles below 60) is left out of the bags and reported as not expected.
        RoleConsumables const role = member ? ConsumablesFor(*member->spec, party.level) : RoleConsumables{0, 0, 0};
        LevelSupplies const supplies = SuppliesFor(party.level);
        JsonObject bags;
        if (role.flask)
            bags.Num("flask", CountItem(bot, role.flask));
        bags.Num("food", CountItem(bot, role.food));
        if (role.prePot)
            bags.Num("prePot", CountItem(bot, role.prePot));
        bags.Num("healingPotion", CountItem(bot, supplies.healingPotion))
            .Num("healthstone", CountItem(bot, 36892) + CountItem(bot, 36893) + CountItem(bot, 36894));
        if (manaUser)
            bags.Num("manaPotion", CountItem(bot, supplies.manaPotion)).Num("drink", CountItem(bot, supplies.drink));

        return JsonObject().Str("name", bot->GetName()).Bool("alive", bot->IsAlive())
            .Bool("flask", HasFlaskAura(bot)).Bool("flaskExpected", role.flask != 0).Num("elixirs", elixirs)
            .Bool("food", HasWellFed(bot))
            .Raw("buffs", buffList.Build()).Raw("missing", missing.Build()).Raw("bags", bags.Build())
            .Build();
    }

    void TestHarnessMgr::LogPull(Party& party, std::vector<Player*> const& players, bool boss,
                                 std::string const& bossName)
    {
        JsonArray members;
        for (Player* bot : players)
            members.Raw(MemberBuffJson(party, bot));

        party.lastEvent = boss ? "boss pull " + bossName : "trash pull";
        LogEvent(party, "pull", JsonObject().Num("pull", party.pullSeq).Str("kind", boss ? "boss" : "trash")
                                    .Str("boss", bossName).Raw("members", members.Build()));
    }

    // "Who aggroed first" (user, Stockade 2026-10-24: «Рога полетел вперед, хотя не должен был»): the members in
    // the order they entered combat (ms after the previous one), the first mob that engaged and its first victim, the
    // first hit, and whether the tank started the fight. Hook entries older than OPENER_STALE_MS before the harness
    // saw the fight belong to something it did not count as a pull.
    std::string TestHarnessMgr::PullOpenerJson(Party& party, std::vector<Player*> const& players, bool atStart)
    {
        constexpr int32 OPENER_STALE_MS = 15 * IN_MILLISECONDS;
        OpenerNote const note = StatsRegistry::Instance().Opener(party.seq);
        auto fresh = [&party](uint32 ms) { return ms && int32(party.pullStartMs - ms) <= OPENER_STALE_MS; };

        JsonArray order;
        OpenerMember const* first = nullptr;
        uint32 previousMs = 0;
        for (OpenerMember const& entry : note.members)
        {
            if (!fresh(entry.ms))
                continue;

            Member const* member = FindMember(party, entry.guid);
            bool const tank = member && member->spec->role == Role::Tank;
            // Entered in the same update as the first one: the tank counts as the opener (its pull hit first).
            if (!first || (entry.ms == first->ms && tank))
                first = &entry;
            order.Raw(JsonObject().Str("name", entry.name)
                          .Str("role", member ? RoleName(member->spec->role) : "?")
                          .Num("afterPrevMs", previousMs ? int64(getMSTimeDiff(previousMs, entry.ms)) : 0)
                          .Str("enemy", entry.enemy).Num("enemyEntry", entry.enemyEntry).Build());
            previousMs = entry.ms;
        }

        Member const* firstMember = first ? FindMember(party, first->guid) : nullptr;
        bool const planned = !first || (firstMember && firstMember->spec->role == Role::Tank);
        std::string starter = "planned";
        // "planned" only for a planner pull: the tank opened it and carried out a pull action within OPENER_STALE_MS
        // before it (plannerPullMs). Otherwise "unplanned (<source>)": the harness body pull or walk, the drake
        // phase, or the tank's last mover (wave 7 scholo-h / mt-h mega-pulls and the occ-h drake pull were "planned").
        if (planned && first && firstMember)
        {
            auto const pulled = plannerPullMs.find(first->guid);
            bool const plannerPull = pulled != plannerPullMs.end() &&
                                     int32(party.pullStartMs - pulled->second) >= 0 &&
                                     getMSTimeDiff(pulled->second, party.pullStartMs) < OPENER_STALE_MS;
            if (!plannerPull)
            {
                std::string source = "no planner pull";
                if (Player* tank = ObjectAccessor::FindPlayer(first->guid))
                {
                    PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
                    Engine* engine = tankAI ? tankAI->GetCurrentEngine() : nullptr;
                    if (party.approach)
                        source = "harness body pull";
                    else if (tank->GetVehicle())
                        source = "drake phase";
                    else if (engine && engine->GetIntentMover() == "test runner move")
                        source = "harness walk";
                    // [b30_mech_pos] only a fresh mover names the opener: the combat engine keeps its last mover
                    // across fights (pos-n 20261025-184229: "tank's ick and krick" 90 s after Krick, on a Ymirjar
                    // pack the harness walk - the non-combat engine's test runner move - had pulled).
                    else if (engine && !engine->GetIntentMover().empty() && fresh(engine->GetIntentMoverMs()))
                        source = "tank's " + engine->GetIntentMover();
                }
                // [pull_label] plan29 follow-up 4 (user 2026-10-25): the tank's own walk-in (reach melee / tank face)
                // into the target its planner just carried out a plan for (PullCheck::Planned within
                // OPENER_STALE_MS: the plan, the spot and the group's readiness were judged) is a planned body pull;
                // one within PULL_LABEL_CHAIN_MS of the last fight's end is the chain into the next pack. Build 29:
                // 812 fresh walk-ins at 0.001 deaths/pull, 244 chains at 0.000, against 0.010 for the rest.
                constexpr uint32 PULL_LABEL_CHAIN_MS = 5 * IN_MILLISECONDS;
                bool const tankWalkIn = source == "tank's reach melee" || source == "tank's tank face";
                uint32 const plannedMs = PullEstimate::LastPlannedMs(first->guid);
                bool const planBacked = plannedMs && int32(party.pullStartMs - plannedMs) >= 0 &&
                                        getMSTimeDiff(plannedMs, party.pullStartMs) < uint32(OPENER_STALE_MS);
                bool const chained = party.lastPullEndMs && int32(party.pullStartMs - party.lastPullEndMs) >= 0 &&
                                     getMSTimeDiff(party.lastPullEndMs, party.pullStartMs) <= PULL_LABEL_CHAIN_MS;
                if (tankWalkIn && planBacked)
                    starter = "planned (body)";
                else if (tankWalkIn && chained)
                    starter = "chain";
                else
                    starter = "unplanned (" + source + ")";
            }
        }
        if (!planned)
            starter = "unplanned: " + first->name + " " +
                      (firstMember ? std::string(firstMember->spec->label) + "/" + RoleName(firstMember->spec->role)
                                   : std::string("?"));

        if (atStart)
        {
            party.pullStarter = starter;
            // [lead1] a start the dungeon's script made (no decision of the leader nor the tank): the chain from the
            // last fight into the next pack, a vehicle phase (ToC5 / Oculus drakes), a scripted summon (HoR waves,
            // the Black Morass rift guardians: no DB spawn), an escort's own fight
            party.pullScript.clear();
            if (starter == "chain")
                party.pullScript = "chain";
            else if (starter.find("drake phase") != std::string::npos)
                party.pullScript = "vehicle";
            else if (fresh(note.mobMs) && !note.mob.empty() && !note.mobSpawnId)
                party.pullScript = "summon";
            else if (!party.escort.IsEmpty())
                party.pullScript = "escort";
            if (!planned && firstMember)
                for (Member& member : party.members)
                    if (member.guid == firstMember->guid)
                        ++member.unplannedStarts;
        }

        // The tank's own target when the harness saw the fight: the mob it meant to pull.
        std::string tankTarget;
        for (Player* bot : players)
        {
            Member const* member = FindMember(party, bot->GetGUID());
            if (member && member->spec->role == Role::Tank && bot->IsAlive())
                if (Unit* victim = bot->GetVictim())
                {
                    tankTarget = victim->GetName();
                    break;
                }
        }

        // engageDelay per member (user, 2026-10-24: «ДПСы не ДПСят всегда, стоят, тупят»): seconds from the tank's
        // entering combat to the member's own combat entry and its first hit on a mob, "never" while it has none;
        // negative when the member was first.
        uint32 tankMs = 0;
        for (OpenerMember const& entry : note.members)
        {
            Member const* member = FindMember(party, entry.guid);
            if (fresh(entry.ms) && member && member->spec->role == Role::Tank)
            {
                tankMs = entry.ms;
                break;
            }
        }

        JsonArray engage;
        for (Member const& member : party.members)
        {
            uint32 combatMs = 0;
            for (OpenerMember const& entry : note.members)
                if (entry.guid == member.guid && fresh(entry.ms))
                    combatMs = entry.ms;
            uint32 hitMs = 0;
            for (auto const& [guid, ms] : note.memberFirstHits)
                if (guid == member.guid && fresh(ms))
                    hitMs = ms;
            uint32 zeroHitMs = 0;
            for (auto const& [guid, ms] : note.memberFirstZeroHits)
                if (guid == member.guid && fresh(ms))
                    zeroHitMs = ms;

            JsonObject line;
            line.Str("name", member.name).Str("role", RoleName(member.spec->role));
            auto delay = [&line, tankMs](char const* key, uint32 ms)
            {
                if (!ms)
                    line.Str(key, "never");
                else if (tankMs)
                    line.Real(key, double(int32(ms - tankMs)) / IN_MILLISECONDS);
                else
                    line.Str(key, "no tank combat");
            };
            delay("combatSec", combatMs);
            delay("firstHitSec", hitMs);
            // [opener_zero_hit] only when such a hit came before the first damage (or with none)
            if (zeroHitMs && (!hitMs || int32(hitMs - zeroHitMs) > 0))
                delay("firstZeroHitSec", zeroHitMs);
            engage.Raw(line.Build());
        }

        // [patrol_wait_spot] the fight began while the group waited for a patrol (the planner's "patrol wait"): its
        // cause is the patrol coming by, not a pull (dtk-h 064751: the Wretched Belcher opened pull 6).
        constexpr uint32 PATROL_CAUSE_MS = 45 * IN_MILLISECONDS;
        std::string cause;
        if (party.patrolWaitMs && int32(party.pullStartMs - party.patrolWaitMs) >= 0 &&
            getMSTimeDiff(party.patrolWaitMs, party.pullStartMs) <= PATROL_CAUSE_MS)
            cause = "patrol proximity aggro";
        if (atStart)
        {
            party.patrolWaitMs = 0;
            party.patrolWaitUntilMs = 0;
        }

        JsonObject json;
        json.Str("starter", starter)
            .Bool("planned", planned && (starter.rfind("planned", 0) == 0 || starter == "chain"))
            .Str("firstMember", first ? first->name : "")
            .Raw("order", order.Build()).Raw("engageDelay", engage.Build()).Str("tankTarget", tankTarget);
        if (!cause.empty())
            json.Str("cause", cause).Str("patrolWait", party.patrolWaitWhy);
        if (fresh(note.mobMs))
            json.Str("firstMob", note.mob).Num("firstMobEntry", note.mobEntry).Num("firstMobSpawn", note.mobSpawnId)
                .Str("firstMobVictim", note.mobVictim);
        if (fresh(note.hitMs))
            json.Str("firstHitBy", note.hitBy).Str("firstHitVictim", note.hitVictim)
                .Bool("firstHitByMember", note.hitByMember);
        return json.Build();
    }

    void TestHarnessMgr::LogPullStart(Party& party, std::vector<Player*> const& players)
    {
        std::string const opener = PullOpenerJson(party, players, true);
        LogEvent(party, "pull_start", JsonObject().Num("pull", party.pullSeq).Bool("boss", party.pullIsBoss)
                                          .Raw("opener", opener));
        if (party.pullStarter.rfind("planned", 0) != 0 && party.pullStarter != "chain")
            LOG_INFO("playerbots", "[pull start] pull {}: {}", party.pullSeq, party.pullStarter);
    }

    void TestHarnessMgr::LogAttemptEnd(Party& party, BossTrack& boss, std::string const& result)
    {
        uint32 const now = getMSTime();
        uint32 const durationMs = std::max<uint32>(getMSTimeDiff(boss.attemptStartMs, now), 1);
        float const seconds = float(durationMs) / float(IN_MILLISECONDS);

        JsonArray members;
        for (Member const& member : party.members)
        {
            MemberCounters const current = StatsRegistry::Instance().Snapshot(member.guid);
            MemberCounters const& start = boss.attemptStart[member.guid];
            uint64 const damage = current.damageDone - std::min(current.damageDone, start.damageDone);
            uint64 const healing = current.healingDone - std::min(current.healingDone, start.healingDone);
            uint64 const taken = current.damageTaken - std::min(current.damageTaken, start.damageTaken);

            JsonObject uses;
            for (auto const& [item, count] : current.itemUses)
            {
                auto itr = start.itemUses.find(item);
                uint32 const before = itr == start.itemUses.end() ? 0 : itr->second;
                if (count > before)
                    uses.Num(std::to_string(item).c_str(), count - before);
            }

            members.Raw(JsonObject().Str("name", member.name).Str("spec", member.spec->label)
                            .Str("role", RoleName(member.spec->role)).Num("damage", int64(damage))
                            .Num("healing", int64(healing)).Num("damageTaken", int64(taken))
                            .Real("dps", double(damage) / seconds).Real("hps", double(healing) / seconds)
                            .Num("interrupts", current.interrupts - std::min(current.interrupts, start.interrupts))
                            .Num("dispels", current.dispels - std::min(current.dispels, start.dispels))
                            .Num("deaths", current.deaths - std::min(current.deaths, start.deaths))
                            .Num("aggroPulled", member.aggroPulled - std::min(member.aggroPulled,
                                                                              boss.attemptStartAggro[member.guid]))
                            .Raw("itemUses", uses.Build())
                            .Merge(CombatStatsJson(current, start,
                                                   member.combatMs - std::min(member.combatMs,
                                                                              boss.attemptStartCombatMs[member.guid])))
                            .Build());
        }

        JsonArray deaths;
        for (std::string const& name : boss.attemptDeaths)
            deaths.Str(name);

        uint64 groupDamage = 0;
        for (Member const& member : party.members)
        {
            MemberCounters const current = StatsRegistry::Instance().Snapshot(member.guid);
            MemberCounters const& start = boss.attemptStart[member.guid];
            groupDamage += current.damageDone - std::min(current.damageDone, start.damageDone);
        }

        // A kill that cannot be real progress: the boss far below the group's level (classic heroics spawning the
        // normal templates), or dead much faster than its health and the group's expected DPS allow.
        float const expectedDps = ExpectedGroupDps(party);
        float const expectedSec = expectedDps > 0.0f ? float(boss.engageMaxHp) / expectedDps : 0.0f;
        std::string why;
        if (result == "kill")
        {
            if (boss.engageLevel && uint32(boss.engageLevel) + SUSPICIOUS_LEVEL_GAP < party.level)
                why = Acore::StringFormat("boss level {} vs group level {}", boss.engageLevel, party.level);
            if (expectedSec >= SUSPICIOUS_MIN_EXPECTED_SEC && seconds * SUSPICIOUS_KILL_SPEED < expectedSec)
                why += Acore::StringFormat("{}killed in {:.1f} s, ~{:.0f} s expected ({} hp, {:.0f} dps)",
                                           why.empty() ? "" : "; ", seconds, expectedSec, boss.engageMaxHp,
                                           expectedDps);
        }

        party.lastEvent = "boss " + boss.name + ": " + result;
        // [b32_sfk_door] no shortcut_reenter within 30 s of a boss kill: a post-kill script may still come (sfk-h
        // 223844-5B-83: out to the entrance 1 s after Fenrus, his Voidwalker wave despawned, Sorcerer's Gate shut).
        if (result == "kill")
        {
            uint32 const killMs = getMSTime();
            if (int32(killMs + 30 * IN_MILLISECONDS - party.shortcutCooldownUntilMs) > 0)
                party.shortcutCooldownUntilMs = killMs + 30 * IN_MILLISECONDS;
        }
        LogEvent(party, "boss_attempt", JsonObject().Num("bossId", boss.bossId).Str("boss", boss.name)
                                            .Num("attempt", boss.attempts).Str("result", result)
                                            .Real("durationSec", seconds).Num("bossLevel", boss.engageLevel)
                                            .Num("bossMaxHp", boss.engageMaxHp)
                                            .Num("groupDamage", int64(groupDamage))
                                            .Real("expectedGroupDps", expectedDps).Real("expectedSec", expectedSec)
                                            .Bool("suspicious", !why.empty()).Raw("deaths", deaths.Build())
                                            .Raw("members", members.Build()));

        if (!why.empty())
        {
            party.suspicious.push_back(boss.name + ": " + why);
            LogEvent(party, "suspicious", JsonObject().Str("boss", boss.name).Num("entry", boss.entry)
                                              .Str("reason", why).Num("bossLevel", boss.engageLevel)
                                              .Num("groupLevel", party.level).Num("bossMaxHp", boss.engageMaxHp)
                                              .Real("durationSec", seconds).Real("expectedSec", expectedSec));
        }
    }

    std::string TestHarnessMgr::MemberDiagnosisJson(Party& party, Player* bot, Player* leader)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        Member const* member = FindMember(party, bot->GetGUID());

        std::string lastAction;
        std::string strategies;
        std::string state = "?";
        std::string targetName;
        float targetDistance = 0.0f;
        if (botAI)
        {
            if (Engine* engine = botAI->GetCurrentEngine())
            {
                lastAction = engine->GetLastAction();
                strategies = engine->ListStrategies();
            }
            switch (botAI->GetState())
            {
                case BOT_STATE_COMBAT:
                    state = "combat";
                    break;
                case BOT_STATE_NON_COMBAT:
                    state = "non combat";
                    break;
                case BOT_STATE_DEAD:
                    state = "dead";
                    break;
                default:
                    break;
            }
            if (Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get())
            {
                targetName = target->GetName();
                targetDistance = bot->GetDistance(target);
            }
        }

        std::string waitingFor;
        if (bot->GetGUID() == party.leader)
            waitingFor = party.waitingFor;

        return JsonObject().Str("name", bot->GetName()).Str("role", member ? RoleName(member->spec->role) : "?")
            .Bool("alive", bot->IsAlive()).Bool("ghost", bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            .Real("hpPct", bot->GetHealthPct())
            .Real("manaPct", bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : 0.0f).Str("state", state)
            .Str("lastAction", lastAction).Str("strategies", strategies).Str("target", targetName)
            .Real("targetDist", targetDistance).Bool("moving", bot->isMoving())
            .Num("motion", int64(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()))
            .Num("map", bot->GetMapId()).Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
            .Real("z", bot->GetPositionZ())
            .Bool("eating", RestState::IsEating(bot))
            .Bool("drinking", RestState::IsDrinking(bot))
            .Real("distToLeader", bot->GetMap() == leader->GetMap() ? bot->GetDistance(leader) : -1.0f)
            .Str("waitingFor", waitingFor).Num("watchdogStuckSec", WatchdogStuckSeconds(bot))
            .Raw("intent", IntentOrNull(bot->GetGUID())).Build();
    }

    // Where every member stood and what it did, every POSITIONS_MS while the party fights (one compact line):
    // n name, x/y/z/o, a alive, hp %, dT to the tank, dTT to the tank's target, bh behind that target (outside its
    // front 180 deg), dH to the healer, c current cast or channel spell id, t current target entry.
    void TestHarnessMgr::LogPositions(Party& party, std::vector<Player*> const& players)
    {
        Player* tank = nullptr;
        Player* healer = nullptr;
        for (Member const& member : party.members)
        {
            Player* bot = ObjectAccessor::FindPlayer(member.guid);
            if (!bot || !bot->IsAlive() || !bot->IsInWorld())
                continue;
            if (member.spec->role == Role::Tank && (!tank || (member.slot.flags & SLOT_MAIN_TANK)))
                tank = bot;
            if (member.spec->role == Role::Healer && !healer)
                healer = bot;
        }

        Unit* tankTarget = tank ? tank->GetVictim() : nullptr;
        auto targetOf = [](Player* bot) -> Unit*
        {
            if (Unit* victim = bot->GetVictim())
                return victim;
            return bot->GetTarget() ? ObjectAccessor::GetUnit(*bot, bot->GetTarget()) : nullptr;
        };
        auto sameMap = [](WorldObject const* left, WorldObject const* right)
        { return left && right && left->GetMap() == right->GetMap(); };

        JsonArray members;
        for (Player* bot : players)
        {
            uint32 casting = 0;
            for (CurrentSpellTypes type : {CURRENT_CHANNELED_SPELL, CURRENT_GENERIC_SPELL})
                if (!casting)
                    if (Spell* spell = bot->GetCurrentSpell(type))
                        casting = spell->GetSpellInfo() ? spell->GetSpellInfo()->Id : 0;

            Unit* target = targetOf(bot);
            JsonObject line;
            line.Str("n", bot->GetName()).Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                .Real("z", bot->GetPositionZ()).Real("o", bot->GetOrientation()).Bool("a", bot->IsAlive())
                .Real("hp", bot->GetHealthPct())
                .Real("dT", sameMap(bot, tank) && bot != tank ? bot->GetExactDist(tank) : -1.0f)
                .Real("dTT", sameMap(bot, tankTarget) ? bot->GetExactDist(tankTarget) : -1.0f)
                .Bool("bh", sameMap(bot, tankTarget) && !tankTarget->HasInArc(static_cast<float>(M_PI), bot))
                .Real("dH", sameMap(bot, healer) && bot != healer ? bot->GetExactDist(healer) : -1.0f)
                .Num("c", casting).Num("t", target && target->IsCreature() ? target->GetEntry() : 0);
            // Line of sight healer <-> tank (user, Stockade 2026-10-24: «танк любит забегать за повороты и становится
            // вне видимости хила»): every healer to the tank, and the tank to the first healer. One LoS query per
            // healer per POSITIONS_MS, on the world thread while the maps stand still.
            Member const* member = FindMember(party, bot->GetGUID());
            if (member && member->spec->role == Role::Healer && bot->IsAlive() && sameMap(bot, tank))
                line.Bool("losTank", bot->IsWithinLOSInMap(tank)).Real("dTank", bot->GetExactDist(tank));
            else if (bot == tank && sameMap(bot, healer))
                line.Bool("losHealer", bot->IsWithinLOSInMap(healer)).Real("dHealer", bot->GetExactDist(healer));
            WatchIntent(bot);
            std::string const intent = IntentJsonFor(bot->GetGUID());
            if (!intent.empty())
                line.Raw("intent", intent);
            members.Raw(line.Build());
        }

        JsonObject event;
        event.Raw("m", members.Build());
        if (tankTarget)
            event.Raw("tt", JsonObject().Num("entry", tankTarget->IsCreature() ? tankTarget->GetEntry() : 0)
                                .Real("x", tankTarget->GetPositionX()).Real("y", tankTarget->GetPositionY())
                                .Real("z", tankTarget->GetPositionZ()).Real("o", tankTarget->GetOrientation())
                                .Build());
        LogEvent(party, "positions", event);
    }

    void TestHarnessMgr::Heartbeat(Party& party, std::vector<Player*> const& players, Player* leader)
    {
        uint32 const now = getMSTime();
        // The members' engines record their intention while the run ticks (Engine::intentSink, RecordIntent).
        for (Player* bot : players)
        {
            WatchIntent(bot);
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                if (Engine* engine = botAI->GetCurrentEngine())
                    if (IntentExecuted const& executed = engine->GetIntentExecuted();
                        executed.ms && IsPlannerPullAction(executed.name))
                        plannerPullMs[bot->GetGUID()] = executed.ms;
        }
        if (getMSTimeDiff(party.lastHeartbeatMs, now) < HEARTBEAT_MS)
            return;

        float const moved =
            leader->GetMapId() == party.content->mapId ? leader->GetExactDist(&party.heartbeatPos) : 0.0f;
        party.lastHeartbeatMs = now;
        party.heartbeatPos = leader->GetPosition();

        uint32 alive = 0;
        float hp = 0.0f;
        float mana = 0.0f;
        uint32 manaUsers = 0;
        for (Player* bot : players)
        {
            if (!bot->IsAlive())
                continue;
            ++alive;
            hp += bot->GetHealthPct();
            Member const* member = FindMember(party, bot->GetGUID());
            if (member && UsesMana(member->spec->role, member->cls) && bot->GetMaxPower(POWER_MANA))
            {
                mana += bot->GetPowerPct(POWER_MANA);
                ++manaUsers;
            }
        }

        uint32 killed = 0;
        uint32 failed = 0;
        for (BossTrack const& boss : party.bosses)
        {
            killed += boss.killed ? 1 : 0;
            failed += boss.failed ? 1 : 0;
        }

        std::string tankAction;
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(leader))
            if (Engine* engine = botAI->GetCurrentEngine())
                tankAction = engine->GetLastAction();

        JsonObject line;
        line.Str("type", "heartbeat").Str("time", WallClock()).Str("runId", party.id).Str("content", party.content->key)
            .Str("comp", party.comp->id).Num("elapsedSec", getMSTimeDiff(party.runStartMs, now) / IN_MILLISECONDS)
            .Str("phase", PhaseLabel(party)).Num("map", leader->GetMapId()).Real("x", leader->GetPositionX())
            .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ()).Real("moved30s", moved)
            .Num("bossesKilled", killed).Num("bossesFailed", failed).Num("bossesTotal", RequiredBossCount(party))
            .Num("trashKilledWindow", party.windowKills).Num("kills", party.kills).Num("wipes", party.wipes)
            .Str("tankLastAction", tankAction).Str("lastEvent", party.lastEvent).Num("alive", alive)
            .Num("members", int64(party.members.size())).Real("avgHpPct", alive ? hp / float(alive) : 0.0f)
            .Real("avgManaPct", manaUsers ? mana / float(manaUsers) : 0.0f).Str("waitingFor", party.waitingFor)
            .Num("stallLevel", party.stallLevel);

        // Every member's position, for the dungeon map.
        JsonArray positions;
        for (Player* bot : players)
            positions.Raw(JsonObject().Str("name", bot->GetName()).Num("alive", bot->IsAlive() ? 1 : 0)
                              .Real("x", bot->GetPositionX()).Real("y", bot->GetPositionY())
                              .Real("z", bot->GetPositionZ()).Raw("intent", IntentOrNull(bot->GetGUID())).Build());
        line.Raw("pos", positions.Build());
        AppendLine(HEARTBEAT_FILE, line.Build());
        party.windowKills = 0;
    }

    void TestHarnessMgr::CheckStall(Party& party, std::vector<Player*> const& players, Player* leader, bool anyInCombat)
    {
        uint32 const now = getMSTime();
        if (IsTerminalPhase(party.phase))
            return;

        std::string watchdogStuck;
        for (Player* bot : players)
            if (bot->GetGUID() != party.leader && WatchdogStuckSeconds(bot) >= WATCHDOG_STALL_SECONDS)
                watchdogStuck += (watchdogStuck.empty() ? "" : ",") + bot->GetName();

        // A member the follow watchdog gave up on blocks the group even while the leader still walks around.
        bool const leaderMoved = leader->GetMapId() != party.progressMapId ||
                                 leader->GetExactDist(&party.progressPos) >= PROGRESS_DISTANCE;
        if (watchdogStuck.empty() && leaderMoved)
        {
            party.progressPos = leader->GetPosition();
            party.progressMapId = leader->GetMapId();
            party.lastProgressMs = now;
            party.stallLevel = 0;
        }

        if (anyInCombat)
        {
            party.lastProgressMs = now;
            return;
        }

        // Legitimate waits are progress: a ghost running back (Stratholme wave 35: 592 yd from the graveyard, more
        // than the stall window) and a member waiting out Resurrection Sickness (the pull gate holds 10 minutes).
        for (Player* bot : players)
        {
            bool const ghostMoving = bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) && bot->isMoving();
            if (ghostMoving || (bot->IsAlive() && bot->HasAura(15007)))
            {
                party.lastProgressMs = now;
                return;
            }
        }

        if (party.eventGatherArrived && party.eventGatherArrivedMs &&
            getMSTimeDiff(party.eventGatherArrivedMs, now) < EVENT_GATHER_WAIT_MS && EventGatherApplies(party))
        {
            party.lastProgressMs = now;
            return;
        }

        if (getMSTimeDiff(party.lastProgressMs, now) < STALL_MS)
            return;

        // A boss just chosen gets its pull wait and body pull first (Magisters' Terrace wave 13: Selin was the
        // target 28 s when the stall that had run on refused trash packs skipped him for 10 minutes, the run ended
        // with Selin "unreachable" from his doorway).
        if (party.stallLevel == 0 && party.targetIsBoss && !party.target.IsEmpty() &&
            getMSTimeDiff(party.targetSinceMs, now) < STALL_BOSS_GRACE_MS)
            return;

        JsonArray diagnosis;
        for (Player* bot : players)
            diagnosis.Raw(MemberDiagnosisJson(party, bot, leader));

        ++party.stuckEvents;
        if (party.stallLevel == 0)
        {
            LogEvent(party, "stall", JsonObject().Num("level", 1).Str("phase", PhaseLabel(party))
                                         .Str("watchdogStuck", watchdogStuck)
                                         .Str("target", party.target.ToString()).Real("x", leader->GetPositionX())
                                         .Real("y", leader->GetPositionY()).Real("z", leader->GetPositionZ())
                                         .Raw("members", diagnosis.Build()));

            // [lead6] LEADER_ONE_READINESS: while the tank's order is open or the leader declared a wait (its group
            // not ready, a patrol wait, a whole-pack ask of its planner) the stall is a report, not a skip of the
            // leader's target: the wait has its own end, and a stuck one is the run's level-2 stall below.
            if (LeaderDecides(party) && Leader::Leads(Leader::LEADER_ONE_READINESS))
            {
                uint32 const askMs = party.lead.wholeSeenMs;
                std::string leaderWait;
                if (Leader::Orders::IsOpen(party.leader))
                    leaderWait = "the tank's pull order is open";
                else if (!party.lead.waitPatrol.IsEmpty())
                    leaderWait = "patrol wait: " + party.lead.waitWhy;
                else if (askMs && getMSTimeDiff(askMs, now) <= Leader::LEADER_REQUEST_TTL_MS)
                    leaderWait = "the whole pack asked of the planner: " + party.lead.wholeWhy;
                else if (!party.lead.ready && !party.lead.notReadyWhy.empty())
                    leaderWait = "not ready: " + party.lead.notReadyWhy;
                if (!leaderWait.empty())
                {
                    party.stallLevel = 1;
                    party.lastProgressMs = now;
                    LogEvent(party, "stall_recovery",
                             JsonObject().Str("action", "report only: the leader's declared wait")
                                 .Str("wait", leaderWait));
                    return;
                }
            }
            // One automatic recovery: drop the current target (the leader's intent). Bots walk by themselves: their
            // movement and AI are not reset from outside; HARNESS_MOVES_BOTS restarts every member's movement too.
            if (!party.target.IsEmpty())
                SkipTarget(party, party.target, 10 * MINUTE * IN_MILLISECONDS, "stall");
            TestRunner::ClearWaypoint(party.leader);
            if (HARNESS_MOVES_BOTS)
                for (Player* bot : players)
                {
                    if (!bot->IsAlive())
                        continue;
                    ++party.harnessMoves;
                    bot->GetMotionMaster()->Clear();
                    bot->StopMoving();
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        botAI->Reset();
                }
            party.waitSinceMs = 0;
            party.nextRetargetMs = now;
            party.stallLevel = 1;
            party.lastProgressMs = now;
            LogEvent(party, "stall_recovery",
                     JsonObject().Str("action", HARNESS_MOVES_BOTS ? "skip target, clear waypoint, reset movement"
                                                                   : "skip target, clear waypoint"));
            return;
        }

        LogEvent(party, "stall", JsonObject().Num("level", 2).Str("phase", PhaseLabel(party))
                                     .Str("watchdogStuck", watchdogStuck)
                                     .Real("x", leader->GetPositionX()).Real("y", leader->GetPositionY())
                                     .Real("z", leader->GetPositionZ()).Raw("members", diagnosis.Build()));
        Finish(party, "stalled", "no progress for 4 minutes outside a fight");
    }
}
