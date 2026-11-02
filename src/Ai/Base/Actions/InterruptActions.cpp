/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InterruptActions.h"
#include "Event.h"
#include "GameObject.h"  // [lead405]
#include "Item.h"        // [lead402]
#include "ItemTemplate.h"
#include "InterruptValues.h"
#include "LeaderCommandState.h"  // [rot2] [rot3]
#include "LeaderConsumables.h"   // [lead407] Leader::Dungeon::UseItem
#include "Log.h"
#include "ObjectAccessor.h"  // [lead401]
#include "RtiValue.h"        // [rot2] FindCrowdControl
#include "TargetValue.h"     // [rot2] IsCrowdControlled
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"   // [lead405]
#include "WorldSession.h"

Unit* InterruptAssignedCastAction::GetCaster()
{
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    Unit* caster = botAI->GetUnit(assignment.caster);
    if (!caster || !caster->IsAlive() || !caster->IsInWorld() || caster->GetMap() != bot->GetMap())
        return nullptr;

    return caster;
}

SpellInfo const* InterruptAssignedCastAction::GetAssignedSpell()
{
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    if (assignment.spell.empty())
        return nullptr;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", assignment.spell);
    return spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
}

bool InterruptAssignedCastAction::isUseful()
{
    Unit* caster = GetCaster();
    if (!caster || !caster->IsNonMeleeSpellCast(false, false, true))
        return false;

    // the cached assignment may lag a hand-off to the next member by a tick: the registry has the last word
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    return InterruptCoordination::Allows(botAI, caster, assignment.spell);
}

bool InterruptAssignedCastAction::isPossible()
{
    Unit* caster = GetCaster();
    SpellInfo const* spellInfo = GetAssignedSpell();
    if (!caster || !spellInfo || bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || bot->HasSpellCooldown(spellInfo->Id))
        return false;

    // An own channel is stopped in Execute; CanCastSpell would refuse while it runs.
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return true;

    Unit* target = spellInfo->NeedsExplicitUnitTarget() ? caster : bot;
    return botAI->CanCastSpell(spellInfo->Id, target, true);
}

bool InterruptAssignedCastAction::Execute(Event /*event*/)
{
    Unit* caster = GetCaster();
    SpellInfo const* spellInfo = GetAssignedSpell();
    if (!caster || !spellInfo)
        return false;

    // Stop-cast to kick, as a player would; a healer is never assigned while it casts.
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) && !PlayerbotAI::IsHeal(bot))
        bot->InterruptSpell(CURRENT_CHANNELED_SPELL);

    if (Spell* own = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        bot->InterruptSpell(own->GetCurrentContainer());

    Unit* target = spellInfo->NeedsExplicitUnitTarget() ? caster : bot;
    InterruptAssignment const assignment = AI_VALUE(InterruptAssignment, "interrupt assignment");
    if (!botAI->CastSpell(assignment.spell, target))
        return false;

    InterruptCoordination::MarkFired(botAI, caster);
    // the assignment is spent; recompute it at once instead of re-firing on a cached answer
    if (UntypedValue* value = context->GetUntypedValue("interrupt assignment"))
        value->Reset();
    return true;
}

namespace
{
    // [rot2] a unit of the bot's map, alive
    Unit* LeaderCommandUnit(PlayerbotAI* botAI, ObjectGuid guid)
    {
        if (guid.IsEmpty())
            return nullptr;

        Player* bot = botAI->GetBot();
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsInWorld() || unit->GetMap() != bot->GetMap())
            return nullptr;

        return unit;
    }

    void AckLeader(Player* bot, std::string const& what, std::string const& why)
    {
        Leader::MemberEvents command;
        if (LeaderCommand::CurrentCommand(bot, command))
            Leader::Events::Ack(bot->GetGUID(), command.seq, what, why);
    }

    // [lead401] a spell that brings a dead member back (its effects, or a spell the game lets target the dead)
    bool RaisesDead(uint32 spell)
    {
        SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
        return info && (info->HasEffect(SPELL_EFFECT_RESURRECT) || info->HasEffect(SPELL_EFFECT_RESURRECT_NEW) ||
                        info->HasAttribute(SPELL_ATTR2_ALLOW_DEAD_TARGET));
    }

    // [lead401] the leader's resurrection target: a dead group member of the bot's map whose body lies there
    // (not released) and that has no resurrection offered yet
    Player* LeaderDeadUnit(PlayerbotAI* botAI, ObjectGuid guid)
    {
        Player* bot = botAI->GetBot();
        Player* dead = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
        if (!dead || dead->IsAlive() || !dead->IsInWorld() || dead->GetMap() != bot->GetMap() ||
            dead->getDeathState() != DeathState::Corpse || dead->isResurrectRequested())
            return nullptr;
        return dead;
    }

    // [lead401] the External's unit: a resurrection's dead member, else a living unit (Empty: the bot itself)
    Unit* ExternalUnit(PlayerbotAI* botAI, uint32 spell, ObjectGuid target)
    {
        if (target.IsEmpty())
            return botAI->GetBot();
        if (RaisesDead(spell))
            return LeaderDeadUnit(botAI, target);
        return LeaderCommandUnit(botAI, target);
    }

    // [lead401] a resurrection the bot must still walk to: out of its range or sight
    bool ResurrectionNeedsWalk(Player* bot, uint32 spell, Unit* dead)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        return info && dead &&
               (!bot->IsWithinDistInMap(dead, info->GetMaxRange(true, bot)) || !bot->IsWithinLOSInMap(dead));
    }
}

bool LeaderExternalAction::isUseful()
{
    uint32 spell = 0;
    ObjectGuid target;
    return LeaderCommand::AssignedExternal(bot, spell, target) && spell && bot->HasSpell(spell) &&
           ExternalUnit(botAI, spell, target);
}

bool LeaderExternalAction::isPossible()
{
    uint32 spell = 0;
    ObjectGuid target;
    if (!LeaderCommand::AssignedExternal(bot, spell, target) || bot->HasSpellCooldown(spell))
        return false;

    Unit* unit = ExternalUnit(botAI, spell, target);
    if (!unit)
        return false;
    // [lead401] out of a fight a resurrection out of range or sight walks there first (its prerequisite)
    if (RaisesDead(spell) && !bot->IsInCombat() && ResurrectionNeedsWalk(bot, spell, unit))
        return true;
    if (botAI->CanCastSpell(spell, unit, true))
        return true;
    // [lead272] refused while nothing passing holds it (a walk, a cast, the global cooldown): reported to the
    // leader, which drops the order instead of waiting on it
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (info && !bot->isMoving() && !bot->IsNonMeleeSpellCast(false) &&
        !bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        Leader::Events::NoteExternalFailed(bot->GetGUID(), spell, "its cast check refuses it on " + unit->GetName());
    return false;
}

bool LeaderExternalAction::Execute(Event /*event*/)
{
    uint32 spell = 0;
    ObjectGuid target;
    if (!LeaderCommand::AssignedExternal(bot, spell, target))
        return false;

    Unit* unit = ExternalUnit(botAI, spell, target);
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!unit || !info)
        return false;
    // [lead401] still on its way to the dead member (the prerequisite walk): no cast yet; a walk that did not start
    // (no spot in range and sight the bot can reach) is reported, the leader picks again
    if (RaisesDead(spell) && ResurrectionNeedsWalk(bot, spell, unit))
    {
        if (!bot->isMoving())
            Leader::Events::NoteExternalFailed(bot->GetGUID(), spell, "no walk into range and sight of " +
                                                                          unit->GetName());
        return false;
    }
    if (!botAI->CastSpell(spell, unit))
    {
        // [lead272] reported: the leader drops the order instead of waiting on it
        Leader::Events::NoteExternalFailed(bot->GetGUID(), spell, "the cast failed on " + unit->GetName());
        return false;
    }

    LOG_INFO("playerbots", "[rot2] [leader external] {} casts {} ({}) on {}", bot->GetName(), info->SpellName[0], spell,
             unit->GetName());
    AckLeader(bot, "external", std::string(info->SpellName[0]) + " on " + unit->GetName());
    return true;
}

std::vector<NextAction> LeaderExternalAction::getPrerequisites()
{
    // [lead401] out of a fight the walk into the resurrection's range and sight (the leader named the dead member
    // first: RaidLeader::SetResurrectFirst, so the walk's corpse is the External's)
    uint32 spell = 0;
    ObjectGuid target;
    if (!bot->IsInCombat() && LeaderCommand::AssignedExternal(bot, spell, target) && RaisesDead(spell))
        return NextAction::merge({NextAction("reach party member to resurrect")}, Action::getPrerequisites());
    return Action::getPrerequisites();
}

bool LeaderDispelAction::isUseful()
{
    uint32 spell = 0;
    ObjectGuid target;
    if (!LeaderCommand::AssignedDispel(bot, spell, target) || !bot->HasSpell(spell))
        return false;
    Unit* unit = target.IsEmpty() ? bot : LeaderCommandUnit(botAI, target);
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!unit || !info)
        return false;
    // a debuff the spell removes (its SPELL_EFFECT_DISPEL types) is still on the member (the game's own dispel list)
    uint32 mask = 0;
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.Effect == SPELL_EFFECT_DISPEL && effect.MiscValue >= 0 && effect.MiscValue < 32)
            mask |= 1u << effect.MiscValue;
    DispelChargesList dispellable;
    if (mask)
        unit->GetDispellableAuraList(bot, mask, dispellable, info);
    return !dispellable.empty();
}

bool LeaderDispelAction::isPossible()
{
    uint32 spell = 0;
    ObjectGuid target;
    if (!LeaderCommand::AssignedDispel(bot, spell, target) || bot->HasSpellCooldown(spell))
        return false;
    Unit* unit = target.IsEmpty() ? bot : LeaderCommandUnit(botAI, target);
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!unit || !info)
        return false;
    if (botAI->CanCastSpell(spell, unit, true))
        return true;
    // refused while nothing passing holds it (a walk, a cast, the global cooldown): reported, the leader picks again
    if (!bot->isMoving() && !bot->IsNonMeleeSpellCast(false) && !bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        Leader::Events::NoteDispelFailed(bot->GetGUID(), spell, "its cast check refuses it on " + unit->GetName());
    return false;
}

bool LeaderDispelAction::Execute(Event /*event*/)
{
    uint32 spell = 0;
    ObjectGuid target;
    if (!LeaderCommand::AssignedDispel(bot, spell, target))
        return false;
    Unit* unit = target.IsEmpty() ? bot : LeaderCommandUnit(botAI, target);
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!unit || !info)
        return false;
    if (!botAI->CastSpell(spell, unit))
    {
        Leader::Events::NoteDispelFailed(bot->GetGUID(), spell, "the cast failed on " + unit->GetName());
        return false;
    }
    LOG_INFO("playerbots", "[lead400] [leader dispel] {} casts {} ({}) on {}", bot->GetName(), info->SpellName[0],
             spell, unit->GetName());
    AckLeader(bot, "dispel", std::string(info->SpellName[0]) + " on " + unit->GetName());
    return true;
}

namespace
{
    // [lead402] the bag item the leader named, and the unit it goes on (Empty target: the bot itself)
    Item* ConsumeItem(Player* bot, uint32& itemId, Unit*& on, PlayerbotAI* botAI, std::string* why = nullptr)
    {
        ObjectGuid target;
        ObjectGuid object;
        if (!LeaderCommand::AssignedConsume(bot, itemId, target, object, why) || !itemId)
            return nullptr;
        on = target.IsEmpty() ? bot : LeaderCommandUnit(botAI, target);
        return on ? bot->GetItemByEntry(itemId) : nullptr;
    }

    // [rot85] b208 rfc-h: a second potion in one combat. The game keeps a drunk potion's cooldown off until the combat
    // ends (Spell::SendSpellCooldown sets Player::m_lastPotionId instead) and refuses the next one with
    // SPELL_FAILED_NOT_READY (Spell::CheckCast); HasSpellCooldown saw no cooldown, the packet went out, the use counted
    // as done: 5C's balance druid "used" its Runic Mana Injector 100 times (Oggleflint: 25 of 54 s spent on the
    // repeated Consume, no Wrath / Starfire in between). The same check as the core's: the item is not usable now.
    bool PotionBlockedThisCombat(Player* bot, Item* item, uint32 spell)
    {
        SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
        return item && bot->GetLastPotionId() && (item->IsPotion() || (info && info->IsCooldownStartedOnEvent()));
    }

    uint32 OnUseSpell(Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            return 0;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (proto->Spells[i].SpellId > 0 && proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                return uint32(proto->Spells[i].SpellId);
        return 0;
    }
}

bool LeaderConsumeAction::isUseful()
{
    uint32 itemId = 0;
    Unit* on = nullptr;
    return bot->IsAlive() && ConsumeItem(bot, itemId, on, botAI) != nullptr;
}

bool LeaderConsumeAction::isPossible()
{
    uint32 itemId = 0;
    Unit* on = nullptr;
    Item* item = ConsumeItem(bot, itemId, on, botAI);
    uint32 const spell = OnUseSpell(item);
    if (!item || !spell || bot->HasSpellCooldown(spell))
        return false;
    if (PotionBlockedThisCombat(bot, item, spell))  // [rot85]
    {
        Leader::Events::NoteConsumeFailed(bot->GetGUID(), itemId, "one potion per combat: the last one blocks it");
        return false;
    }
    if (bot->CanUseItem(item) == EQUIP_ERR_OK)
        return true;
    if (!bot->isMoving() && !bot->IsNonMeleeSpellCast(false))
        Leader::Events::NoteConsumeFailed(bot->GetGUID(), itemId, "the game refuses the item");
    return false;
}

bool LeaderConsumeAction::Execute(Event /*event*/)
{
    uint32 itemId = 0;
    Unit* on = nullptr;
    std::string why;
    Item* item = ConsumeItem(bot, itemId, on, botAI, &why);
    if (!item)
        return false;
    std::string const name = item->GetTemplate()->Name1;
    // [lead407] b198 rfc-h 5G-7 603.0 s: the tank's pre-pull potion went through UseItemAction::UseItem, which stops a
    // walking bot and clears its chase - the tank's engage walk to Taragaman stopped, its pull lost its target and the
    // order stood Issued for 221 s ("use failed" reported after the stop). An instant item on the bot itself is used
    // as the client sends it, the walk and the chase go on; one with a cast time or another target waits for the bot
    // to stand (no report: the walk is the leader's, not a refusal)
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(OnUseSpell(item));
    if (on == bot && spellInfo && !spellInfo->CalcCastTime())
    {
        if (!Leader::Dungeon::UseItem(bot, itemId))
        {
            if (!bot->IsNonMeleeSpellCast(false))
                Leader::Events::NoteConsumeFailed(bot->GetGUID(), itemId, "the use failed");
            return false;
        }
    }
    else
    {
        if (bot->isMoving())
            return false;
        // the item's spell takes the selection as its unit target (a soulstone on another member)
        if (on != bot)
            bot->SetSelection(on->GetGUID());
        if (!UseItem(item, ObjectGuid::Empty, nullptr, on))
        {
            if (!bot->isMoving() && !bot->IsNonMeleeSpellCast(false))
                Leader::Events::NoteConsumeFailed(bot->GetGUID(), itemId, "the use failed");
            return false;
        }
    }
    LOG_INFO("playerbots", "[lead402] [leader consume] {} uses {} ({}) on {}: {}", bot->GetName(), name, itemId,
             on->GetName(), why);
    AckLeader(bot, "consume", name + " on " + on->GetName());
    return true;
}

bool LeaderUseObjectAction::isUseful()
{
    uint32 itemId = 0;
    ObjectGuid target;
    ObjectGuid object;
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsNonMeleeSpellCast(true) ||
        !LeaderCommand::AssignedConsume(bot, itemId, target, object, nullptr) || itemId || object.IsEmpty())
        return false;
    GameObject* go = botAI->GetGameObject(object);
    return go && go->isSpawned();
}

bool LeaderUseObjectAction::Execute(Event /*event*/)
{
    uint32 itemId = 0;
    ObjectGuid target;
    ObjectGuid object;
    std::string why;
    if (!LeaderCommand::AssignedConsume(bot, itemId, target, object, &why) || object.IsEmpty())
        return false;
    GameObject* go = botAI->GetGameObject(object);
    if (!go || !go->isSpawned())
        return false;
    // the leader says which object; the walk into its reach is the member's how
    if (bot->GetDistance(go) > INTERACTION_DISTANCE - 1.0f)
        return MoveNear(go, INTERACTION_DISTANCE - 2.0f);
    if (bot->isMoving())
        bot->StopMoving();
    WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
    packet << go->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(packet);
    LOG_INFO("playerbots", "[lead405] [leader use object] {} uses {} {}: {}", bot->GetName(), go->GetName(),
             go->GetEntry(), why);
    AckLeader(bot, "use object", go->GetName());
    return true;
}

bool LeaderThreatAction::isUseful()
{
    uint32 spell = 0;
    ObjectGuid mob;
    return LeaderCommand::AssignedThreat(bot, spell, mob) && LeaderCommandUnit(botAI, mob);
}

bool LeaderThreatAction::Execute(Event /*event*/)
{
    uint32 spell = 0;
    ObjectGuid mob;
    if (!LeaderCommand::AssignedThreat(bot, spell, mob))
        return false;

    Unit* unit = LeaderCommandUnit(botAI, mob);
    if (!unit)
        return false;

    // the rotation goes on against the mob taken back
    context->GetValue<Unit*>("current target")->Set(unit);

    SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
    bool cast = false;
    if (info && bot->HasSpell(spell) && !bot->HasSpellCooldown(spell))
    {
        Unit* spellTarget = info->NeedsExplicitUnitTarget() ? unit : bot;
        if (botAI->CanCastSpell(spell, spellTarget, true))
            cast = botAI->CastSpell(spell, spellTarget);
    }

    LOG_INFO("playerbots", "[rot2] [leader threat] {} takes back {} {}: {}", bot->GetName(), unit->GetName(),
             unit->GetEntry(), cast ? info->SpellName[0] : "attacks it");
    AckLeader(bot, "threat", std::string(cast ? info->SpellName[0] : "attack") + " on " + unit->GetName());
    return true;
}

bool LeaderCcAction::isUseful()
{
    Unit* unit = LeaderCommandUnit(botAI, LeaderCommand::AssignedCc(bot));
    return unit && !IsCrowdControlled(unit);
}

bool LeaderCcAction::isPossible()
{
    Unit* unit = LeaderCommandUnit(botAI, LeaderCommand::AssignedCc(bot));
    CrowdControlOption const* option = unit ? FindCrowdControl(botAI, unit, false) : nullptr;
    return option && option->spell && botAI->CanCastSpell(option->spell, unit);
}

bool LeaderCcAction::Execute(Event /*event*/)
{
    Unit* unit = LeaderCommandUnit(botAI, LeaderCommand::AssignedCc(bot));
    CrowdControlOption const* option = unit ? FindCrowdControl(botAI, unit, false) : nullptr;
    if (!option || !option->spell || !botAI->CastSpell(option->spell, unit))
        return false;

    LOG_INFO("playerbots", "[rot2] [leader cc] {} casts {} on {} {}", bot->GetName(), option->spell, unit->GetName(),
             unit->GetEntry());
    AckLeader(bot, "cc", std::string(option->spell) + " on " + unit->GetName());
    return true;
}

// [rot3] Best first: the full immunities and the big cuts, then the smaller ones. A tank skips the saves that stop it
// holding threat (Divine Shield halves its damage, Ice Block stops everything).
char const* LeaderPreSaveAction::PickSave()
{
    static char const* const saves[] = {"divine shield",       "ice block",         "shield wall",
                                        "survival instincts",  "icebound fortitude", "dispersion",
                                        "pain suppression",    "divine protection", "cloak of shadows",
                                        "evasion",             "deterrence",        "last stand",
                                        "vampiric blood",      "barkskin",          "anti-magic shell",
                                        "enraged regeneration", "frenzied regeneration"};

    bool const tank = PlayerbotAI::IsTank(bot);
    for (char const* save : saves)
    {
        std::string const name(save);
        if (tank && (name == "divine shield" || name == "ice block"))
            continue;

        uint32 const spellId = AI_VALUE2(uint32, "spell id", name);
        if (!spellId || !bot->HasSpell(spellId) || bot->HasSpellCooldown(spellId))
            continue;

        if (botAI->CanCastSpell(name, bot))
            return save;
    }

    return nullptr;
}

bool LeaderPreSaveAction::isUseful() { return LeaderCommandState::PreSaveAssigned(bot) && PickSave(); }

bool LeaderPreSaveAction::Execute(Event /*event*/)
{
    char const* save = PickSave();
    if (!save || !botAI->CastSpell(save, bot))
        return false;

    LOG_INFO("playerbots", "[rot3] [leader pre save] {} casts {} on the leader's PreSave", bot->GetName(), save);
    AckLeader(bot, "pre save", save);
    return true;
}
