/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidLeaderStrategy.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string_view>

#include "Action.h"
#include "AttackAction.h"         // [lead15] the own-decision check
#include "GenericSpellActions.h"  // [lead15] CastBuffSpellAction / CastCrowdControlSpellAction
#include "Group.h"
#include "LeaderCommand.h"    // [lead410] BurnToolUse
#include "LeaderOrders.h"     // [lead15] the leader's member events
#include "MovementActions.h"  // [lead15]
#include "ObjectAccessor.h"   // [lead401]
#include "Playerbots.h"
#include "RaidLeaderState.h"
#include "ReachTargetActions.h"  // [lead392] the gap closers
#include "SpellInfo.h"  // [lead188] the dispel types
#include "SpellMgr.h"
#include "TestRunnerStrategy.h"  // [lead17] the harness's waypoint for a member

namespace
{
    // Actions that start a fight out of combat.
    bool IsPullStarter(std::string const& name)
    {
        static constexpr std::string_view starters[] = {"aggressive target", "attack anything",  "pull my target",
                                                        "pull rti target",   "pull start",       "pull action",
                                                        "reach pull",        "attack rti target"};

        for (std::string_view starter : starters)
            if (name == starter)
                return true;

        return false;
    }

    // Orders a leader gives in chat; when a person leads, they start the boss fight he held.
    bool IsLeaderOrder(std::string const& name)
    {
        static constexpr std::string_view orders[] = {"attack my target", "pull my target", "pull rti target",
                                                      "reach pull",       "pull start",     "pull action"};

        for (std::string_view order : orders)
            if (name == order)
                return true;

        return false;
    }

    // Out-of-combat actions that take the group somewhere new (SelfBot leader questing, grinding, travelling).
    bool IsLeadingMove(std::string const& name)
    {
        static constexpr std::string_view moves[] = {
            "choose travel target", "move to travel target", "move random",     "rpg",
            "choose rpg target",    "move to rpg target",    "attack anything", "grind chat shortcut"};

        if (name.compare(0, 8, "new rpg ") == 0 && name != "new rpg status update")
            return true;

        for (std::string_view move : moves)
            if (name == move)
                return true;

        return false;
    }
}

void RaidLeaderStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Rare bookkeeping for the coordinator; placed above the rotation so a call is not delayed by it.
    triggers.push_back(
        new TriggerNode("leader ready check needed", {NextAction("leader ready check", ACTION_HIGH + 1)}));
    triggers.push_back(
        new TriggerNode("leader ready check done", {NextAction("leader ready check result", ACTION_HIGH + 1)}));
    triggers.push_back(
        new TriggerNode("leader encounter event", {NextAction("leader encounter event", ACTION_MOVE + 9)}));
    triggers.push_back(new TriggerNode("leader tank flags", {NextAction("leader assign tanks", ACTION_MOVE + 8)}));

    // Members: water from the bags while the leader waits for mana.
    triggers.push_back(new TriggerNode("ready check low mana", {NextAction("drink", ACTION_NORMAL)}));

    // A lost fight: who can leave it does, and stands up again once the fight is over (any engine).
    triggers.push_back(new TriggerNode("wipe escape", {NextAction("wipe escape", ACTION_EMERGENCY + 5)}));
    triggers.push_back(new TriggerNode("wipe escape over", {NextAction("wipe escape end", ACTION_EMERGENCY + 4)}));

    // An ordered "burst" closes on time.
    triggers.push_back(new TriggerNode("burst over", {NextAction("burst end", ACTION_HIGH + 1)}));

    // [lead15] The dungeon leader's events (LeaderOrders.h Events): its MoveTo / StandAt above every own move of the
    // member (a dodge out of a cleave comes from the leader, ACTION_EMERGENCY + 4 > "dungeon avoid frontal cleave"),
    // its Attack at the pull's opening. Inert without a fresh event (any group the leader does not lead).
    triggers.push_back(
        new TriggerNode("leader move order", {NextAction("leader move order", ACTION_EMERGENCY + 4)}));
    triggers.push_back(
        new TriggerNode("leader attack order", {NextAction("leader attack order", ACTION_HIGH + 3)}));
    // [lead76] the leader's "top up X" (a readiness gap on health): above the healer's own heals
    triggers.push_back(
        new TriggerNode("leader heal target", {NextAction("leader heal target", ACTION_HIGH + 2)}));
}

void RaidLeaderStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new RaidLeaderHoldMultiplier(botAI));
}

void RaidConsumablesStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("pre pull potion", {NextAction("use pre pull potion", ACTION_HIGH + 2)}));
    triggers.push_back(new TriggerNode("soul portal available", {NextAction("click soul portal", ACTION_NORMAL + 4)}));
    triggers.push_back(new TriggerNode("soulwell available", {NextAction("use soulwell", ACTION_NORMAL + 3)}));
    triggers.push_back(new TriggerNode("need raid flask", {NextAction("use raid flask", ACTION_NORMAL + 2)}));
    triggers.push_back(new TriggerNode("need food buff", {NextAction("use food buff", ACTION_NORMAL + 1)}));
}

namespace
{
    // [lead17b] a move / attack the executor whitelist blocked: a blocked_move / blocked_attack line (the step-6
    // work list: which tactics the leader takes over first), at most once per member and action per 10 s
    void NoteBlocked(Player* self, std::string const& name, char const* kind)
    {
        constexpr uint32 BLOCKED_LOG_MS = 10 * IN_MILLISECONDS;
        static std::mutex lock;
        static std::map<std::pair<ObjectGuid, std::string>, uint32> last;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(lock);
            uint32& at = last[{self->GetGUID(), name}];
            if (at && getMSTimeDiff(at, now) < BLOCKED_LOG_MS)
                return;
            at = now ? now : 1;
        }
        std::string const where = self->GetMap() ? self->GetMap()->GetMapName() : "?";
        Leader::Events::NoteOwn(self->GetGUID(), Leader::OwnDecision{name, kind, where, 0});
    }

    // [lead15] the member's own decisions the executor model turns off (LeaderOrders.h LEADER_NO_OWN_*)
    // [lead18] the action casts on an enemy, by its target value's name (no target computed)
    bool EnemyTargetName(Action* action)
    {
        std::string const target = action->GetTargetName();
        return target == "current target" || target == "dps target" || target == "enemy player target" ||
               target.find("attacker") != std::string::npos || target.find("cc target") != std::string::npos ||
               target.find("enemy") != std::string::npos || target.find("aoe") != std::string::npos;
    }

    // [lead170] [lead181] the healer's fight: its own combat flag, or the leader's Heal while a group member still
    // fights (b180 5B / 5G: the Heal outlived the fight and the holy paladin's blessings on others went out after it,
    // without Prepare - 16 / 13 own_decision)
    bool HealerFights(Player* self, Leader::MemberEvents const& events)
    {
        if (self->IsInCombat())
            return true;
        if (events.attack != Leader::AttackEvent::Heal)
            return false;
        if (Group* group = self->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource();
                    member && member != self && member->GetMap() == self->GetMap() && member->IsInCombat())
                    return true;
        return false;
    }

    // [lead400] a member's own dispel (on itself or another member): the leader's Dispel decides who removes what
    // (lead188's "a damage dealer waits while a healer removes the type" is that rule's healer-first pick now)
    bool IsOwnDispel(Action* action, std::string const& name)
    {
        if (dynamic_cast<CastCureSpellAction*>(action) || dynamic_cast<CurePartyMemberAction*>(action))
            return true;
        if (name == "dispel magic on target")  // an enemy's buff: the rotation's
            return false;
        static constexpr std::string_view dispels[] = {"abolish ", "cleanse", "cure", "purify ", "remove curse",
                                                       "dispel magic"};
        for (std::string_view dispel : dispels)
            if (name.rfind(dispel, 0) == 0)
                return true;
        return false;
    }

    // [lead300] a cast on another member that is the leader's External (its "leader external" executes it): the
    // member's own pick of these is off. The leader issues Misdirection / Tricks, Pain Suppression / Hand of
    // Sacrifice, Hand of Protection, Innervate, Hymn of Hope, Divine Hymn, Heroism / Bloodlust, Power Infusion; the
    // others here have no leader rule yet (LEADER_OWNERSHIP_AUDIT.md) and stay off.
    bool IsOwnExternal(std::string const& name)
    {
        static constexpr std::string_view externals[] = {"power infusion",
                                                         "power infusion on party",
                                                         "pain suppression",
                                                         "pain suppression on party",
                                                         "pain suppression on spiking tank",
                                                         "guardian spirit on party",
                                                         "guardian spirit on spiking tank",
                                                         "hand of sacrifice on main tank",
                                                         "hand of sacrifice on spiking tank",
                                                         "hand of protection on party",
                                                         "hand of salvation on party",
                                                         "hand of freedom on party",
                                                         "misdirection on main tank",
                                                         "tricks of the trade on main tank",
                                                         "hymn of hope",
                                                         "divine hymn",
                                                         "lay on hands on party",
                                                         "focus magic on party",
                                                         "divine sacrifice",
                                                         "aura mastery",
                                                         "mana tide totem",
                                                         "tranquility",
                                                         "innervate on healer",
                                                         "soulstone master",
                                                         "soulstone tank",
                                                         "soulstone healer",
                                                         "soulstone auto",
                                                         "ss master",
                                                         "ss tank",
                                                         "ss healer",
                                                         "ss auto"};
        for (std::string_view external : externals)
            if (name == external)
                return true;
        return false;
    }

    // [lead300] the tank's own protection tools on a member (LEADER_TANK_OWNS_FIGHT): no leader External
    bool IsTankTool(std::string const& name)
    {
        return name == "intervene" || name == "intervene on attacked group member" || name == "vigilance";
    }

    // [lead300] a battle resurrection is the leader's: off in a fight
    // [lead401] ... and every resurrection of another member, in and out of a fight: the leader's External names the
    // caster and the dead member
    bool IsBattleRes(std::string const& name)
    {
        return name == "rebirth" || name == "bear battle res" || name == "raise ally";
    }

    bool IsOwnResurrection(Action* action, std::string const& name)
    {
        return IsBattleRes(name) || dynamic_cast<ResurrectPartyMemberAction*>(action) != nullptr;
    }

    // [lead401] the member's External is a resurrection of a dead member (its walk there is the leader's)
    bool ResurrectionExternal(Leader::MemberEvents const& events)
    {
        SpellInfo const* info = events.externalSpell ? sSpellMgr->GetSpellInfo(events.externalSpell) : nullptr;
        Player* dead = info && !events.externalTarget.IsEmpty() ? ObjectAccessor::FindPlayer(events.externalTarget)
                                                                 : nullptr;
        return dead && !dead->IsAlive() &&
               (info->HasEffect(SPELL_EFFECT_RESURRECT) || info->HasEffect(SPELL_EFFECT_RESURRECT_NEW) ||
                info->HasAttribute(SPELL_ATTR2_ALLOW_DEAD_TARGET));
    }

    // [lead300] consumables, pulls and raid marks are the leader's decisions
    // [lead402] ... the mana / healing potion too (the leader's Consume by its estimate); [lead405] the Ritual of
    // Souls (the leader's External in the buff pass)
    bool IsLeaderConsumable(std::string const& name)
    {
        return name == "use raid flask" || name == "use food buff" || name == "use pre pull potion" ||
               name == "use soulwell" || name == "click soul portal" || name == "mana potion" ||
               name == "healing potion" || name == "ritual of souls";
    }

    bool IsDungeonPull(std::string const& name)
    {
        static constexpr std::string_view pulls[] = {
            "dungeon boss room pull out", "krik'thir watcher pull", "novos pull",
            "mechanar bridge pull",       "mgt selin room pull",    "oc pull boss",
            "toc pull argent soldiers"};
        for (std::string_view pull : pulls)
            if (name == pull)
                return true;
        return false;
    }

    bool IsRaidMark(std::string const& name)
    {
        static constexpr std::string_view marks[] = {"mark rti",
                                                     "clear auto cc mark",
                                                     "tank kill marks",
                                                     "gargolmar mark hellfire watchers",
                                                     "omor mark fiendish hound",
                                                     "vazruden mark boss",
                                                     "time-lost controller mark charming totem",
                                                     "darkweaver syth mark elementals",
                                                     "anzu alternate marks on boss",
                                                     "mgt delrissa cc mark"};
        for (std::string_view mark : marks)
            if (name == mark)
                return true;
        return false;
    }

    // [lead300] the bot itself or its own pet / guardian / totem
    bool OwnUnit(Player* self, Unit* unit)
    {
        return unit && (unit == self || unit->GetOwnerGUID() == self->GetGUID() ||
                        unit->GetCharmerOrOwnerGUID() == self->GetGUID());
    }

    // [lead410] the bot at its critical health (the "critical health" trigger's line)
    bool CriticalHealth(Player* bot)
    {
        return bot->GetHealthPct() < static_cast<float>(sPlayerbotAIConfig.CriticalHealth);
    }

    bool OwnTargetName(Action* action)
    {
        std::string const target = action->GetTargetName();
        return target == "self target" || target == "pet target";
    }

    // [lead300] a hostile unit the leader named for this member: its Attack target, the kill order after it, its
    // interrupt / CC / flee stop / threat assignment
    bool LeaderNamed(Leader::MemberEvents const& events, ObjectGuid guid)
    {
        if (guid == events.target || guid == events.interrupt || guid == events.cc || guid == events.fleeStop ||
            guid == events.threatTarget)
            return true;
        return std::find(events.killOrder.begin(), events.killOrder.end(), guid) != events.killOrder.end();
    }

    bool OwnDecisionOff(Player* self, Action* action, std::string const& name, Leader::MemberEvents const& events,
                        bool combat, bool gate = false)
    {
        if (name == "leader move order" || name == "leader attack order" || name == "leader heal target")
            return false;
        auto const in = [&name](std::initializer_list<std::string_view> names)
        {
            for (std::string_view n : names)
                if (name == n)
                    return true;
            return false;
        };
        if (in({"leader external", "leader threat", "leader cc", "leader dispel", "leader consume",
                "leader use object"}))  // [lead400] [lead402] [lead405]
            return false;
        // [lead37] b86 rfc-h 20261031-155828 (478-546 s): the hunter's rebuff window never closed - its "ready reply"
        // (the window's end, ForceRebuffStrategy) was vetoed by the walk-only rule below while it had only a Follow,
        // and the leader's readiness held the Oggleflint pull on "Rthunaa buffing". Closing the window is no decision
        if (in({"ready reply", "finish ready check"}))
            return false;
        // [lead300] LEADER_OWNERSHIP_AUDIT.md: the domains the leader owns that the member still decided itself -
        // externals, battle resurrections, the soulstone on others, raid marks, consumables, pulls, the wipe call
        if (IsOwnExternal(name) || IsTankTool(name) || IsRaidMark(name) || IsLeaderConsumable(name) ||
            IsDungeonPull(name) || IsPullStarter(name) || in({"wipe escape", "mount"}) ||
            IsOwnResurrection(action, name) || IsOwnDispel(action, name))  // [lead400] [lead401]
            return true;
        if (name == "innervate" && !gate)  // its own mana tool on itself stays (rot38)
            if (Unit* onWhom = action->GetTarget(); onWhom && onWhom != self)
                return true;
        // [lead300] the leader's live Attack target is not dropped
        if (name == "drop target" && events.attack == Leader::AttackEvent::Attack)
            if (PlayerbotAI* selfAI = GET_PLAYERBOT_AI(self))
                if (Unit* current = selfAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
                    current && current->IsAlive() && current->GetGUID() == events.target)
                    return true;
        // [lead392] b201 rfc-h 5H: the cat druid took 14 mobs, 9 of them in four leaps - 606.1 s the leader's
        // Attack on a Searing Blade Cultist the bear had just growled (a LoS pull, the mob still at its pack, "Searing
        // Blade Enforcer is not pulled, 9 yd from the target"), and the cat's Feral Charge put it 37 yd from the bear
        // in the pack: Cultist and three Enforcers on the cat in 2.3 s. A gap closer is a move; in a fight a damage
        // dealer's or healer's position is the leader's (its combat spot, which keeps off an unpulled pack's reach).
        // The tank's gap closers stay its own threat work
        if (dynamic_cast<CastReachTargetSpellAction*>(action) && !PlayerbotAI::IsTank(self))
        {
            if (!gate)
                NoteBlocked(self, name, "blocked_move");
            return true;
        }
        // [lead300] a spell on an enemy the leader did not name for this member (a multi-DoT, a group member's
        // attacker) or on its DontAttack mob: the target is the leader's
        if (!gate && events.attack == Leader::AttackEvent::Attack && dynamic_cast<CastSpellAction*>(action) &&
            !dynamic_cast<CastHealingSpellAction*>(action) && !dynamic_cast<CastBuffSpellAction*>(action))
            if (Unit* onWhom = action->GetTarget();
                onWhom && onWhom != self && !onWhom->GetGUID().IsPlayer() && self->IsHostileTo(onWhom) &&
                !OwnUnit(self, onWhom) &&
                (onWhom->GetGUID() == events.dontAttack || !LeaderNamed(events, onWhom->GetGUID())))
                return true;
        // [lead300] a damage dealer's buff on another member in a fight: the leader's (Prepare / its External)
        if (combat && !PlayerbotAI::IsHeal(self) && dynamic_cast<CastBuffSpellAction*>(action))
        {
            if (gate)
            {
                if (!OwnTargetName(action))
                    return true;
            }
            else if (Unit* buffTarget = action->GetTarget(); buffTarget && !OwnUnit(self, buffTarget))
                return true;
        }
        // [lead398] b201 rfc-h 5E 1233.2 s: the holy priest cast Divine Spirit on the party as the fight ended, with no
        // Prepare (own_decision + two two_owners). Out of a fight a buff on another member is the leader's buff pass
        // (Prepare / Ready? / Rest), the own-decision check's own rule
        if (!combat && dynamic_cast<CastBuffSpellAction*>(action) && events.ready != Leader::ReadyEvent::Prepare &&
            events.ready != Leader::ReadyEvent::ReadyCheck && events.ready != Leader::ReadyEvent::Rest)
        {
            if (gate)
            {
                if (!OwnTargetName(action))
                    return true;
            }
            else if (Unit* buffTarget = action->GetTarget(); buffTarget && !OwnUnit(self, buffTarget))
                return true;
        }
        // [lead20] Rest is eat / drink only: a buff stands the drinker up
        bool const prepareCmd = events.ready == Leader::ReadyEvent::Prepare ||
                                events.ready == Leader::ReadyEvent::ReadyCheck;
        // [lead17] LEADER_EXECUTOR_WHITELIST: no movement and no attack the leader did not order
        if (Leader::LEADER_EXECUTOR_WHITELIST)
        {
            TestRunner::Waypoint waypoint;
            if (name == "test runner move" && TestRunner::GetWaypoint(self->GetGUID(), waypoint))
                return false;  // the harness's (the leader's) walk for this member
            if (dynamic_cast<AttackAction*>(action))
            {
                // [lead18] gate: by name only - the basic attacks run on the leader's target (its Attack switched
                // the current target), every own target pick is off
                if (gate)
                    return !(events.attack == Leader::AttackEvent::Attack &&
                             in({"melee", "shoot", "auto shot", "attack"}));
                Unit* target = action->GetTarget();
                bool const blocked = !(events.attack == Leader::AttackEvent::Attack && target &&
                                       target->GetGUID() == events.target);
                if (blocked && name != "dps assist" && name != "attack anything")
                    NoteBlocked(self, name, "blocked_attack");
                return blocked;
            }
            if (dynamic_cast<MovementAction*>(action))
            {
                if (in({"stand up"}))
                    return false;
                if (events.loot && in({"move to loot", "loot", "open loot"}))
                    return false;
                // [lead401] ... and the walk of the leader's resurrection External
                if ((prepareCmd || ResurrectionExternal(events)) && in({"reach party member to resurrect"}))
                    return false;
                // [lead23] the rotation's reach toward the leader's Attack target (no leader spot holds it)
                // [lead66] b111 rfc-h 132.6 s: Rtshaab's own "reach melee" walked it 35 yd to a Trogg (own_decision,
                // two_owners). The leader's arbiter owns every move of a led member: no own reach / set behind
                if (!Leader::LEADER_ARBITER_OWNS_MOVES && events.attack == Leader::AttackEvent::Attack &&
                    in({"reach melee", "reach spell", "set behind"}) && events.move != Leader::MoveEvent::MoveTo &&
                    events.move != Leader::MoveEvent::StandAt && events.move != Leader::MoveEvent::Hold)
                    return false;
                // [lead23] blocked_move only for a move the member actually wanted (not the gate's presence check)
                if (!gate && !in({"follow"}))
                    NoteBlocked(self, name, "blocked_move");
                return true;
            }
        }
        if (Leader::LEADER_NO_OWN_FOLLOW && in({"follow"}))
            return true;
        // [lead61] user: «хил, начинай хилить» is the leader's command - a led healer heals in a fight only under the
        // leader's Heal (issued once every fight mob is on the tank, or when the tank is about to die); out of a fight
        // no heal on a full target (no pre-pull HoT)
        if (dynamic_cast<CastHealingSpellAction*>(action) && Leader::LEADER_HEAL_AFTER_THREAT)
        {
            if (combat && PlayerbotAI::IsHeal(self) && events.attack != Leader::AttackEvent::Heal)
                return true;
            // [lead404] out of a fight the healer heals whom the leader's TopUp names ("leader heal target": the most
            // hurt member, itself included, or the member the boss pull waits for): its own top-up pick is off
            if (!combat && PlayerbotAI::IsHeal(self))
                return true;
            if (!combat && !gate)
                if (Unit* healed = action->GetTarget(); healed && healed->GetHealthPct() >= 100.0f)
                    return true;
        }
        // [lead18] LEADER_HEALER_HEALS_ONLY: a led healer casts nothing on an enemy
        if (Leader::LEADER_HEALER_HEALS_ONLY && PlayerbotAI::IsHeal(self) &&
            !dynamic_cast<CastHealingSpellAction*>(action) &&
            (dynamic_cast<AttackAction*>(action) || dynamic_cast<CastShootAction*>(action) ||
             (dynamic_cast<CastSpellAction*>(action) && EnemyTargetName(action))))
            return true;
        // [lead36] b85 rfc-h: the enhancement shaman cast "lesser healing wave on party" for long stretches under
        // the leader's Attack - a damage dealer heals another member only on a leader Heal command (none exists for a
        // damage dealer yet): its own heal on itself stays (an emergency personal save)
        // [lead300] ... in and out of a fight, under any command (its own pet stays its own)
        if (!PlayerbotAI::IsHeal(self) && dynamic_cast<CastHealingSpellAction*>(action))
        {
            if (gate)
            {
                if (!OwnTargetName(action))
                    return true;
            }
            else if (Unit* healTarget = action->GetTarget(); healTarget && !OwnUnit(self, healTarget))
                return true;
        }
        // [lead36] ... and the mage handed out food in the fight
        if (combat && in({"give food", "give water"}))
            return true;
        // [lead42] b91 rfc-h 156.7 s: Rtmagaa's Mirror Image without the leader's burn (own_decision, two_owners):
        // the burn cooldowns run only under the leader's Intensity burn
        // [lead80] b123 141.6 s: Feral Spirit went out of combat (the gate held only in a fight) - the one list
        // (Leader::IsBurnCooldown), in and out of a fight
        // [lead105] Heroism / Bloodlust only as the leader's command (its external order), never the shaman's own
        if (name == "heroism" || name == "bloodlust")
            return true;
        if (Leader::IsBurnCooldown(name) && !Leader::BurnAllows(events, name) &&  // [lead178] per cooldown
            !LeaderCommand::BurnToolUse(self, name, CriticalHealth(self), PlayerbotAI::IsHeal(self)))  // [lead410]
            return true;
        // [lead64] b109 pull 1: Rtshaab's "call of the elements" (Searing / Magma totems) drew the mob before the tank
        // hit it. What makes threat on fight mobs without being an attack (damaging totems, Fire Nova) is gated like
        // damage: only under the leader's Attack and not under its Stop (no threat headroom)
        // [lead71] b115: Rtmagaa took 5 Troggs off the tank - its area spells (Frost Nova, Cone of Cold, Blizzard,
        // Arcane Explosion, Flamestrike, Dragon's Breath, Blast Wave) hit mobs it has no threat headroom on: only under
        // the leader's Attack with AoE allowed and not under its Stop
        if (combat && in({"frost nova", "cone of cold", "blizzard", "arcane explosion", "flamestrike",
                          "dragon's breath", "blast wave"}) &&
            (events.attack != Leader::AttackEvent::Attack || events.intensity == Leader::Intensity::Stop ||
             events.aoe == Leader::AoeRule::Forbidden))
            return true;
        // [lead64] user: non-damaging totems may go down early (they save GCDs); damaging ones only once the tank has
        // threat - out of a fight never (Call of the Elements drops Searing / Magma too)
        if (in({"call of the elements", "searing totem", "magma totem", "fire nova", "fire elemental totem"}) &&
            (!combat || events.attack != Leader::AttackEvent::Attack || events.intensity == Leader::Intensity::Stop))
            return true;
        // [lead16] loot only under the leader's Loot
        if (Leader::LEADER_LOOT_EVENTS && !events.loot &&
            in({"loot", "move to loot", "open loot", "add all loot", "add loot", "store loot", "release loot"}))
            return true;
        if (Leader::LEADER_NO_OWN_ASSIST && in({"dps assist", "dps aoe", "attack anything", "attack rti target"}))
            return true;
        if (Leader::LEADER_NO_OWN_REST && in({"drink", "food"}) && events.ready != Leader::ReadyEvent::Rest)
            return true;
        if (Leader::LEADER_NO_OWN_POSITIONS && in({"move to ranged position", "combat formation move", "rear flank",
                                                   "set behind", "move out of enemy contact", "flee"}))
            return true;
        if (Leader::LEADER_NO_OWN_CLEAVE_DODGE && in({"dungeon avoid frontal cleave"}))
            return true;
        if (Leader::LEADER_NO_OWN_LOS_GATHER && in({"move to los pull spot"}))
            return true;
        if (Leader::LEADER_NO_OWN_AOE_DODGE && in({"avoid aoe", "avoid dungeon hazard"}))
            return true;
        // [lead16] a leader spot (hazard, combat spot, aggro loss, walk) holds: no own range move off it
        if (Leader::LEADER_COMBAT_SPOTS && in({"reach melee", "reach spell"}) &&
            (events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt ||
             events.move == Leader::MoveEvent::Hold))
            return true;
        if (Leader::LEADER_NO_OWN_CC && in({"auto cc mark", "cast auto cc", "emergency cc"}))
            return true;
        if (Leader::LEADER_NO_OWN_CC && name != "pre pull cc" &&
            (dynamic_cast<CastCrowdControlSpellAction*>(action) ||
             (name.size() > 6 && name.compare(name.size() - 6, 6, " on cc") == 0)))
            return true;
        bool const prepare = events.ready == Leader::ReadyEvent::Prepare ||
                             events.ready == Leader::ReadyEvent::ReadyCheck;
        // [lead20] Rest is eat / drink only: no buff under it (a buff stands the drinker up)
        if (Leader::LEADER_NO_OWN_BUFF_TIMING && events.ready == Leader::ReadyEvent::Rest &&
            dynamic_cast<CastBuffSpellAction*>(action))
            return true;
        // [lead16] rot2: only a buff on ANOTHER member waits for Prepare; a self buff (a shapeshift form, Inner Fire,
        // an armor, a seal, a presence) is the bot's own upkeep
        if (Leader::LEADER_NO_OWN_BUFF_TIMING && !combat && !prepare && dynamic_cast<CastBuffSpellAction*>(action))
        {
            if (gate)
            {
                if (action->GetTargetName() != "self target")
                    return true;
            }
            else if (Unit* buffTarget = action->GetTarget(); buffTarget && buffTarget != self)
                return true;
        }
        // [lead300] out of combat with no command at all (the engine still runs in water, PlayerbotAI's idle skip):
        // nothing of its own, the leader's buff pass (forceRebuff) aside
        if (!combat && events.move == Leader::MoveEvent::None && events.ready == Leader::ReadyEvent::None &&
            events.attack == Leader::AttackEvent::None && !events.loot && !events.externalSpell && !in({"stand up"}))
            if (PlayerbotAI* selfAI = GET_PLAYERBOT_AI(self); selfAI && !selfAI->forceRebuff.IsPending())
                return true;
        // out of combat a member told only to walk / stand does just that
        // [lead300] a Hard move (lead371) has no side goal; a Soft one allows the leader's other rules
        if (!combat && !prepare && !events.loot && events.attack == Leader::AttackEvent::None &&
            events.moveMode == Leader::MoveMode::Hard &&
            (events.move == Leader::MoveEvent::MoveTo || events.move == Leader::MoveEvent::StandAt ||
             events.move == Leader::MoveEvent::Hold || events.move == Leader::MoveEvent::Follow) &&
            !in({"stand up", "reach party member to resurrect"}) &&
            // [lead46] b95: Rtshaab held a Follow and the leader's Rest - its drink was vetoed here, it never sat
            !(events.ready == Leader::ReadyEvent::Rest && in({"drink", "food"})))
            return true;
        return false;
    }
}

bool RaidLeader::GateVetoes(PlayerbotAI* botAI, Leader::MemberEvents const& events,
                            std::vector<NextAction> const& handlers)
{
    if (handlers.empty())
        return false;
    Player* bot = botAI->GetBot();
    bool const combat = HealerFights(bot, events);  // [lead170] [lead181]
    for (NextAction handler : handlers)  // getName is not const
    {
        Action* action = botAI->GetAiObjectContext()->GetAction(handler.getName());
        if (!action || !OwnDecisionOff(bot, action, handler.getName(), events, combat, true))
            return false;
    }
    return true;
}

bool RaidLeaderHoldMultiplier::KeepsVeto(Action* /*action*/)
{
    return Leader::ExecutorMember(bot) || (Leader::LEADER_TANK_EXECUTES && Leader::LedBot(bot) &&
                                           Leader::Orders::IsTracked(bot->GetGUID()));
}

void RaidLeader::CheckOwnDecision(PlayerbotAI* botAI, Action* action, Unit* victimBefore)
{
    if (!Leader::LEADER_OWN_DECISION_CHECK || !botAI || !action)
        return;
    Player* bot = botAI->GetBot();
    if (!Leader::ExecutorMember(bot))
        return;
    std::string const& name = action->getName();
    if (name == "leader move order" || name == "leader attack order" || name == "leader dispel" ||
        name == "leader consume" || name == "leader use object")  // [lead400] [lead402] [lead405]
        return;
    Leader::MemberEvents events;
    Leader::Events::Get(bot->GetGUID(), events);
    bool const combat = HealerFights(bot, events);  // [lead181] the same fight as the member gate's
    bool const fights = events.attack == Leader::AttackEvent::Attack || events.attack == Leader::AttackEvent::Heal;
    bool const prepare = events.ready == Leader::ReadyEvent::Prepare ||
                         events.ready == Leader::ReadyEvent::ReadyCheck || events.ready == Leader::ReadyEvent::Rest;
    auto const in = [&name](std::initializer_list<std::string_view> names)
    {
        for (std::string_view n : names)
            if (name == n)
                return true;
        return false;
    };
    auto const note = [&](char const* kind, std::string const& why)
    { Leader::Events::NoteOwn(bot->GetGUID(), Leader::OwnDecision{name, kind, why, 0}); };

    // the target: a new victim that is not the leader's target
    Unit* victim = bot->GetVictim();
    if (victim && victim != victimBefore &&
        !(events.attack == Leader::AttackEvent::Attack && victim->GetGUID() == events.target) &&
        victim->GetGUID() != events.threatTarget)
    {
        note("target", "attacks " + victim->GetName() + " without a leader Attack on it");
        return;
    }
    if (dynamic_cast<AttackAction*>(action))
        return;
    if (in({"drink", "food"}))
    {
        if (events.ready != Leader::ReadyEvent::Rest)
            note("rest", "sits without the leader's Rest");
        return;
    }
    if (dynamic_cast<MovementAction*>(action))
    {
        // the rotation's own range on the commanded target is its "how"
        bool const rotationRange = combat && fights && in({"reach melee", "reach spell", "set behind"});
        bool const resurrect = (prepare || ResurrectionExternal(events)) && in({"reach party member to resurrect"});
        bool const plannedCc = !events.cc.IsEmpty() && in({"pre pull cc"});
        bool const looting = events.loot && in({"move to loot", "loot", "open loot"});
        if (!rotationRange && !resurrect && !plannedCc && !looting && !in({"stand up"}))
            note("move", "moves without a leader MoveTo / StandAt");
        return;
    }
    if (!dynamic_cast<CastSpellAction*>(action))
        return;
    if (dynamic_cast<CastCrowdControlSpellAction*>(action) ||
        (name.size() > 6 && name.compare(name.size() - 6, 6, " on cc") == 0))
    {
        if (events.cc.IsEmpty())
            note("cc", "crowd control without a leader CC assignment");
        return;
    }
    if (IsOwnExternal(name) || IsBattleRes(name) ||  // [lead300] the gate's one list
        in({"innervate", "hand of sacrifice", "hand of protection", "misdirection", "misdirection on party",
            "tricks of the trade", "hand of salvation", "hand of freedom"}))
    {
        // [lead169] b179 rfc-h 5F 650.5 s: the resto druid's Innervate on itself (its own mana tool, kept by rot38
        // like Evocation) counted as an external - an external is a cast on another member
        Unit* const onWhom = action->GetTarget();
        if (!events.externalSpell && !(onWhom == bot && in({"innervate"})))
            note("external", "an external without a leader assignment");
        return;
    }
    if (Leader::IsBurnCooldown(name) &&
        !LeaderCommand::BurnToolUse(bot, name, CriticalHealth(bot), PlayerbotAI::IsHeal(bot)))  // [lead80] [lead410]
    {
        if (!Leader::BurnAllows(events, name))  // [lead178]
            note("burn", "a burn cooldown without the leader's Intensity burn");
        return;
    }
    if (dynamic_cast<CastBuffSpellAction*>(action))
        if (Unit* target = action->GetTarget(); target && target != bot && !prepare && !combat)
            note("buff on others", "buffs " + target->GetName() + " without Prepare / Ready?");
}

float RaidLeaderHoldMultiplier::GetValue(Action* action)
{
    if (!action || !RaidLeader::IsEnabled())
        return 1.0f;

    // [lead34] user: «лидер должен читать маршрут, а не наоборот» - the route's waypoint is data: the led tank walks
    // it only as the leader's MoveTo ("route walk"); its own "test runner move" runs only for a waypoint the leader's
    // walk cannot do (a jump, a straight line over no navmesh, a gate pass)
    if (Leader::LEADER_ROUTE_IS_DATA && action->getName() == "test runner move" &&
        Leader::Orders::IsTracked(bot->GetGUID()) && Leader::LedBot(bot))
    {
        TestRunner::Waypoint waypoint;
        if (TestRunner::GetWaypoint(bot->GetGUID(), waypoint) && !waypoint.jump && !waypoint.straight &&
            !waypoint.gatePass)
            return 0.0f;
    }

    // [lead50] b99 rfc-h 583 s: the tank cast "blessing on party by role" while the planner waited for its mana: under
    // the leader's Rest the led tank buffs nobody (its drink first)
    if (!bot->IsInCombat() && Leader::Orders::IsTracked(bot->GetGUID()) && Leader::LedBot(bot) &&
        (dynamic_cast<CastBuffSpellAction*>(action) || action->getName() == "blessing on party by role"))
    {
        Leader::MemberEvents restEvents;
        if (Leader::Events::Get(bot->GetGUID(), restEvents) && restEvents.ready == Leader::ReadyEvent::Rest)
            return 0.0f;
    }

    // [lead138] b162 20261101-121409: 8 pulls the leader did not order ("unplanned (tank's reach melee)",
    // "(tank's return to pull position)", ...). Only the leader starts a pull: out of a fight with no open pull order,
    // the led tank's own fight starters on a mob that is not fighting are off (the leader's planner orders the pull)
    if (!bot->IsInCombat() && Leader::Orders::IsTracked(bot->GetGUID()) && Leader::LedBot(bot) &&
        !Leader::Orders::IsOpen(bot->GetGUID()))
    {
        static constexpr std::string_view starters[] = {"reach melee", "return to pull position", "attack anything",
                                                        "dps assist", "tank assist", "melee", "pull action"};
        for (std::string_view starter : starters)
            if (action->getName() == starter)
            {
                Unit* startOn = action->GetTarget();
                if (!startOn || !startOn->IsInCombat())
                    return 0.0f;
            }
        // [lead300] ... and the dungeon tactics' own boss / room pulls and the chat pulls
        std::string const& pull = action->getName();
        if (IsDungeonPull(pull) || pull == "pull my target" || pull == "pull rti target" || pull == "pull start" ||
            pull == "reach pull")
            return 0.0f;
    }

    // [lead300] LEADER_OWNERSHIP_AUDIT.md: the led tank's externals (not its own protection tools), consumables and
    // its buffs on others out of a fight outside the leader's buff pass (forceRebuff) are the leader's
    if (Leader::Orders::IsTracked(bot->GetGUID()) && Leader::LedBot(bot))
    {
        std::string const& name = action->getName();
        // [lead400] [lead401] its resurrections and its dispels on another member are the leader's too
        if (IsOwnExternal(name) || IsLeaderConsumable(name) || IsOwnResurrection(action, name))
            return 0.0f;
        if (IsOwnDispel(action, name))
            if (Unit* dispelled = action->GetTarget(); dispelled && dispelled != bot)
                return 0.0f;
        if (!bot->IsInCombat() && !botAI->forceRebuff.IsPending() && dynamic_cast<CastBuffSpellAction*>(action))
            if (Unit* buffTarget = action->GetTarget(); buffTarget && !OwnUnit(bot, buffTarget))
                return 0.0f;
    }

    // [lead17] LEADER_TANK_OWN_PICKS_OFF: the led tank's own next pack and own hazard step are off in every state
    if (Leader::LEADER_TANK_OWN_PICKS_OFF && Leader::Orders::IsTracked(bot->GetGUID()) && Leader::LedBot(bot) &&
        (action->getName() == "chain pull" ||
         // [lead56] in a fight the tank keeps contact itself (LEADER_TANK_OWNS_FIGHT)
         (action->getName() == "move fight off hazard" && !(Leader::LEADER_TANK_OWNS_FIGHT && bot->IsInCombat()))))
        return 0.0f;

    // [lead16] LEADER_TANK_EXECUTES: in a fight the tank's target is the leader's Attack
    if (Leader::LEADER_TANK_EXECUTES && !Leader::LEADER_TANK_OWNS_FIGHT && bot->IsInCombat() &&
        Leader::Orders::IsTracked(bot->GetGUID()) &&
        Leader::LedBot(bot))
    {
        Leader::MemberEvents events;
        static constexpr std::string_view tankPicks[] = {"tank assist", "attack anything", "dps assist",
                                                         "tank defend on attacked group member",
                                                         "reach healer attacker", "chain pull",
                                                         "move fight off hazard"};
        if (Leader::Events::Get(bot->GetGUID(), events) && events.attack == Leader::AttackEvent::Attack)
            for (std::string_view pick : tankPicks)
                if (action->getName() == pick)
                    return 0.0f;
    }

    // [lead15] LEADER_MEMBER_EXECUTOR: the member executes the leader's command, its own decisions are off (alive:
    // a dead member's release / corpse run are its own)
    if (bot->IsAlive() && Leader::ExecutorMember(bot))
    {
        Leader::MemberEvents events;
        Leader::Events::Get(bot->GetGUID(), events);  // none: every field None
        // [lead170] b179 rfc-h 5C: the disc priest stood idle 207 s of fights, half of it with no combat flag of its
        // own (the tank held every mob, nobody needed a heal yet): "power word: shield / prayer of mending on main
        // tank" were vetoed as buffs on another member without Prepare. Under the leader's Heal the group's fight is
        // its fight
        bool const fight = HealerFights(bot, events);  // [lead181] the Heal only while the group fights
        if (OwnDecisionOff(bot, action, action->getName(), events, fight))
        {
            // [lead18] a vetoed action reached the multiplier: its trigger / chain was evaluated (vetoed_eval)
            if (Leader::LEADER_EXECUTOR_TRIGGER_GATE)
                Leader::Events::NoteVetoedEval(bot->GetGUID(), action->getName());
            return 0.0f;
        }
    }

    // [lead15] the leader's member events: while its MoveTo / StandAt holds, the member's own moves (the dodges it
    // picked itself, its ranged spot, follow, the los pull gather, formation) yield to it - no two movement rules; a
    // Hold stops every move; StopDps stops damage, DontAttack every new attack out of combat.
    if (Leader::LEADER_MEMBER_EVENTS)
    {
        Leader::MemberEvents events;
        if (Leader::Events::Get(bot->GetGUID(), events))
        {
            std::string const& name = action->getName();
            static constexpr std::string_view ownMoves[] = {
                "dungeon avoid frontal cleave", "move to ranged position", "follow", "move to los pull spot",
                "combat formation move", "rear flank", "set behind", "move out of enemy contact", "flee"};
            bool const leaderMove = events.move == Leader::MoveEvent::MoveTo ||
                                    events.move == Leader::MoveEvent::StandAt ||
                                    events.move == Leader::MoveEvent::Follow;
            if (leaderMove && Leader::LEADER_OWNS_DODGE && name != "leader move order")
                for (std::string_view own : ownMoves)
                    if (name == own)
                        return 0.0f;
            if (events.move == Leader::MoveEvent::Hold && name != "leader move order")
                for (std::string_view own : ownMoves)
                    if (name == own)
                        return 0.0f;
            if ((events.attack == Leader::AttackEvent::StopDps || events.intensity == Leader::Intensity::Stop) &&
                RaidLeader::IsOffensiveAction(action, bot))
                return 0.0f;
            if (events.attack == Leader::AttackEvent::DontAttack && !bot->IsInCombat() &&
                RaidLeader::IsOffensiveAction(action, bot))
                return 0.0f;
        }
    }

    // Out of a lost fight (stealth, Feign Death, Shadowmeld, Divine Intervention): any action or step gives the bot
    // away, so it does nothing until the fight is over ("wipe escape over" then ends the hold).
    if (AI_VALUE(bool, "wipe escape hold"))
        return 0.0f;

    if (bot->IsInCombat())
        return (AI_VALUE(bool, "pull countdown") && RaidLeader::IsOffensiveAction(action, bot)) ? 0.0f : 1.0f;

    if (!AI_VALUE(bool, "pull hold"))
        return 1.0f;

    std::string const& name = action->getName();
    if (AI_VALUE(bool, "group paused"))
    {
        // On a break the group stays where it is: no pulls, no leading on. Buffs, food and following go on.
        if (IsLeadingMove(name))
            return 0.0f;
    }
    else if (IsLeaderOrder(name) && RaidLeader::IsLedByPerson(bot))
        return 1.0f;

    // [lead17] slabs-h 20261026-184720: the tank sat 20 s on an Issued leader order while this hold (fed by the old
    // 40 yd readiness) blocked its pull: an open leader order was judged ready by the leader - one readiness owner
    if (Leader::LEADER_ONE_GATHER_OWNER && Leader::Orders::IsOpen(bot->GetGUID()))
        return 1.0f;

    if (IsPullStarter(name))
        return 0.0f;

    if (!RaidLeader::IsOffensiveAction(action, bot))
        return 1.0f;

    // The hold stops new pulls only: a tank whose group is already attacked still picks the attackers up.
    if (PlayerbotAI::IsTank(bot))
        return RaidLeader::IsTankDefendingGroup(botAI) ? 1.0f : 0.0f;

    // ... and the rest of the group joins a fight that is already on: mobs that came to the tank without a pull
    // (Deadmines 2026-10-24) killed it while the dps, out of combat themselves, had every attack held.
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->GetMap() == bot->GetMap() && member->IsAlive() &&
                member->IsInCombat())
                return 1.0f;
        }
    }

    return 0.0f;
}
