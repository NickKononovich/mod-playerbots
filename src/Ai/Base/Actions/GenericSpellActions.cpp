/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericSpellActions.h"
#include "AttackerCountValues.h"
#include "BotMoveEvents.h"
#include "Chat.h"
#include "Creature.h"
#include "Event.h"
#include "FearCounterValues.h"  // [fear_counter]
#include "GenericBuffUtils.h"
#include "Group.h"
#include "HealAssignmentValues.h"
#include "Item.h"  // [lead512]
#include "ItemTemplate.h"
#include "LeaderOrders.h"  // [lead512] the leader's burn verdict on a trinket
#include "MotionMaster.h"
#include "ObjectDefines.h"
#include "Opcodes.h"
#include "PartyMemberToHeal.h"
#include "PullFacts.h"  // [lead760] TankWalkMovesFight
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RtiValue.h"
#include "ServerFacade.h"
#include "TankAoeThreat.h"  // [b32_peel]
#include "TargetValue.h"
#include "TerrainHazard.h"  // [b40_strat] GapCloserLeavesGround
#include "Timer.h"
#include "WorldPacket.h"
#include <algorithm>
#include <ctime>
#include <iterator>
#include <unordered_set>

using ai::buff::BuffBelowRefreshTarget;
using ai::buff::MakeAuraQualifierForBuff;
using ai::spell::HasSpellOrCategoryCooldown;

namespace
{
    std::unordered_set<uint32> const& GetMixedTriggerTrinketSpellIds()
    {
        static std::unordered_set<uint32> const mixedTriggerSpellIds = []()
        {
            std::unordered_set<uint32> onUseSpellIds;
            std::unordered_set<uint32> onEquipSpellIds;
            std::unordered_set<uint32> mixedSpellIds;

            auto const* itemTemplates = sObjectMgr->GetItemTemplateStore();
            if (!itemTemplates)
                return mixedSpellIds;

            auto const markSpellId = [&](int32 spellId, uint8 spellTrigger)
            {
                if (spellId <= 0)
                    return;

                if (spellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                {
                    if (onEquipSpellIds.find(spellId) != onEquipSpellIds.end())
                        mixedSpellIds.insert(spellId);

                    onUseSpellIds.insert(spellId);
                }
                else if (spellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
                {
                    if (onUseSpellIds.find(spellId) != onUseSpellIds.end())
                        mixedSpellIds.insert(spellId);

                    onEquipSpellIds.insert(spellId);
                }
            };

            for (auto const& itr : *itemTemplates)
            {
                ItemTemplate const& proto = itr.second;
                if (proto.InventoryType != INVTYPE_TRINKET)
                    continue;

                for (uint8 spellIndex = 0; spellIndex < MAX_ITEM_PROTO_SPELLS; ++spellIndex)
                {
                    auto const& spellData = proto.Spells[spellIndex];
                    markSpellId(spellData.SpellId, spellData.SpellTrigger);
                }
            }

            return mixedSpellIds;
        }();

        return mixedTriggerSpellIds;
    }

    bool IsManaRestoreEffect(SpellEffectInfo const& effectInfo)
    {
        return (effectInfo.Effect == SPELL_EFFECT_ENERGIZE &&
                effectInfo.MiscValue == POWER_MANA) ||
               (effectInfo.Effect == SPELL_EFFECT_APPLY_AURA &&
                effectInfo.ApplyAuraName == SPELL_AURA_PERIODIC_ENERGIZE &&
                effectInfo.MiscValue == POWER_MANA);
    }

    bool IsManaEfficiencyEffect(SpellEffectInfo const& effectInfo)
    {
        return effectInfo.Effect == SPELL_EFFECT_APPLY_AURA &&
               (((effectInfo.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN ||
                  effectInfo.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN_PERCENT) &&
                 effectInfo.MiscValue == POWER_MANA) ||
                effectInfo.ApplyAuraName == SPELL_AURA_MOD_POWER_COST_SCHOOL ||
                effectInfo.ApplyAuraName == SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT ||
                effectInfo.ApplyAuraName == SPELL_AURA_MOD_MANA_REGEN_INTERRUPT);
    }

    bool IsDefensiveTankEffect(SpellEffectInfo const& effectInfo)
    {
        if (effectInfo.Effect != SPELL_EFFECT_APPLY_AURA)
            return false;

        uint32 const tankRatingsMask =
            (1u << CR_DEFENSE_SKILL) |
            (1u << CR_DODGE) |
            (1u << CR_PARRY) |
            (1u << CR_BLOCK) |
            (1u << CR_HIT_TAKEN_MELEE) |
            (1u << CR_HIT_TAKEN_RANGED) |
            (1u << CR_HIT_TAKEN_SPELL) |
            (1u << CR_CRIT_TAKEN_MELEE) |
            (1u << CR_CRIT_TAKEN_RANGED) |
            (1u << CR_CRIT_TAKEN_SPELL);

        switch (effectInfo.ApplyAuraName)
        {
            case SPELL_AURA_MOD_RESISTANCE:
                return (effectInfo.MiscValue & SPELL_SCHOOL_MASK_NORMAL) != 0;
            case SPELL_AURA_MOD_RATING:
                return (effectInfo.MiscValue & tankRatingsMask) != 0;
            case SPELL_AURA_MOD_INCREASE_HEALTH:
            case SPELL_AURA_MOD_INCREASE_HEALTH_PERCENT:
            case SPELL_AURA_MOD_PARRY_PERCENT:
            case SPELL_AURA_MOD_DODGE_PERCENT:
            case SPELL_AURA_MOD_BLOCK_PERCENT:
            case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                return true;
            default:
                return false;
        }
    }
}

// [lead512] b210 rfc-h 5C / 5G: the balance druid's and the ret paladin's on-use trinkets (Tome of Arcane Phenomena,
// 2 min) went on trash under "generic boost" (always true in an instance, rot9) and were on cooldown at the later
// bosses. The trinkets the leader's burn verdict judges: the first on-use spell of the item, an aura it puts on the
// wearer, not a mana return / mana efficiency / tank defence (UseTrinket's own lines for those)
bool Leader::BoostTrinket(Item const* item, uint32& spellId, float& rechargeSec)
{
    spellId = 0;
    rechargeSec = 0.0f;
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!proto)
        return false;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _Spell const& data = proto->Spells[i];
        if (data.SpellId <= 0 || data.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(data.SpellId);
        if (!info || !info->IsPositive() ||
            GetMixedTriggerTrinketSpellIds().count(uint32(data.SpellId)))
            return false;
        bool aura = false;
        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
        {
            SpellEffectInfo const& effect = info->Effects[e];
            if (IsManaRestoreEffect(effect) || IsManaEfficiencyEffect(effect) || IsDefensiveTankEffect(effect))
                return false;
            aura = aura || effect.Effect == SPELL_EFFECT_APPLY_AURA;
        }
        if (!aura)
            return false;
        spellId = uint32(data.SpellId);
        int32 const itemMs = std::max(data.SpellCooldown, data.SpellCategoryCooldown);
        uint32 const spellMs = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
        rechargeSec = float(std::max<int64>(itemMs, int64(spellMs))) / 1000.0f;
        return true;
    }
    return false;
}

CastSpellAction::CastSpellAction(PlayerbotAI* botAI, std::string const spell)
    : Action(botAI, spell), spell(spell), range(botAI->GetRange("spell")) {}

bool CastSpellAction::Execute(Event /*event*/)
{
    if (spell == "conjure food" || spell == "conjure water")
    {
        // uint32 id = AI_VALUE2(uint32, "spell id", spell);
        // if (!id)
        //     return false;

        uint32 castId = 0;

        for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
        {
            uint32 spellId = itr->first;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            std::string const namepart = spellInfo->SpellName[0];
            std::wstring wnamepart;
            if (!Utf8toWStr(namepart, wnamepart))
                return false;

            wstrToLower(wnamepart);

            if (!Utf8FitTo(spell, wnamepart) || spellInfo->Effects[0].Effect != SPELL_EFFECT_CREATE_ITEM)
                continue;

            uint32 itemId = spellInfo->Effects[0].ItemType;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;

            if (spellInfo->Id > castId)
                castId = spellInfo->Id;
        }

        return botAI->CastSpell(castId, bot);
    }

    return botAI->CastSpell(spell, GetTarget());
}

namespace
{
    // [drift_guardian_gate] The summons that put an aggressive guardian out (the core makes every guardian
    // REACT_AGGRESSIVE: it attacks what comes into its sight, the tank or not). `longCooldown`: saved for a boss or a
    // dangerous trash fight, as a player saves them.
    struct GuardianSummon
    {
        char const* spell;
        bool longCooldown;
    };

    constexpr GuardianSummon GUARDIAN_SUMMONS[] = {
        {"fire elemental totem", true}, {"earth elemental totem", true}, {"army of the dead", true},
        {"summon infernal", true},      {"inferno", true},               {"shadowfiend", false},
        {"force of nature", false},     {"feral spirit", false},         {"summon gargoyle", false},
        {"mirror image", false}};
    constexpr uint8 GUARDIAN_DANGEROUS_TRASH = 4;  // attackers: a trash fight worth a long cooldown

    Player* FindDungeonTank(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        ObjectGuid const mainTank = PlayerbotAI::GetMainTankGuid(group);
        Player* fallback = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
                !PlayerbotAI::IsTank(member))
                continue;

            if (member->GetGUID() == mainTank)
                return member;

            if (!fallback)
                fallback = member;
        }

        return fallback;
    }

    bool IsBossUnit(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        return creature && (creature->IsDungeonBoss() || creature->isWorldBoss() ||
                            creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS);
    }

    // scholo-h 20261025-150737-4 pull 6: the Enhancement Shaman's Greater Fire Elemental opened a 12-mob pack the
    // planner had refused, 0.9 s before the tank. In a dungeon group with a tank, a guardian summon waits until the
    // tank holds the bot's target (in combat, on the tank or on its threat list), and a long cooldown also waits for
    // a boss or a dangerous trash fight. Empty when the summon may go out, else why not.
    std::string GuardianSummonRefusal(PlayerbotAI* botAI, std::string const& spell)
    {
        GuardianSummon const* summon = nullptr;
        for (GuardianSummon const& entry : GUARDIAN_SUMMONS)
        {
            if (spell == entry.spell)
            {
                summon = &entry;
                break;
            }
        }

        if (!summon)
            return std::string();

        Player* bot = botAI->GetBot();
        Map* map = bot->GetMap();
        if (!map || !map->IsDungeon() || bot->InBattleground() || PlayerbotAI::IsTank(bot))
            return std::string();

        Player* tank = FindDungeonTank(bot);
        if (!tank)
            return std::string();

        Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        if (!target || !target->IsAlive())
            return "no target";

        if (target->ToPlayer())
            return std::string();

        bool const held = tank->IsInCombat() && target->IsInCombat() &&
                          (target->GetVictim() == tank ||
                           (target->CanHaveThreatList() && target->GetThreatMgr().GetThreat(tank) > 0.0f));
        if (!held)
            return "the tank does not hold " + target->GetName() + " yet";

        if (summon->longCooldown && !IsBossUnit(target) && !IsBossUnit(tank->GetVictim()) &&
            botAI->GetAiObjectContext()->GetValue<uint8>("attacker count")->Get() < GUARDIAN_DANGEROUS_TRASH)
            return "saved for a boss or a dangerous trash fight";

        return std::string();
    }
}

bool CastSpellAction::isUseful()
{
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    if (spell == "mount" && !bot->IsMounted() && !bot->IsInCombat())
        return true;

    if (spell == "mount" && bot->IsInCombat())
    {
        bot->Dismount();
        return false;
    }

    Unit* spellTarget = GetTarget();
    if (!spellTarget || !spellTarget->IsInWorld() || spellTarget->GetMapId() != bot->GetMapId())
        return false;

    // [lead760] b220 rfc-h: Intervene moved a led tank 211 yd after its first hits, to a friend whose mob a ready
    // taunt reached from where the tank stood - the jump only where the leader's fight is planned to stand
    if (spell == "intervene" && spellTarget != bot && PlayerbotAI::IsTank(bot) && bot->IsInCombat())
        for (Unit* attacker : spellTarget->getAttackers())
            if (!Leader::Fight::TankWalkMovesFight(bot, attacker).empty())
                return false;

    // float combatReach = bot->GetCombatReach() + target->GetCombatReach();
    // if (!PlayerbotAI::IsRanged(bot))
    //     combatReach += 4.0f / 3.0f;

    return AI_VALUE2(bool, "spell cast useful", spell);
           // && ServerFacade::instance().GetDistance2d(bot, target) <= (range + combatReach);
}

bool CastSpellAction::isPossible()
{
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && botAI->HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. Vehicle. - bot name: {}", bot->GetName());

        return false;
    }

    if (spell == "mount" && !bot->IsMounted() && !bot->IsInCombat())
        return true;

    if (spell == "mount" && bot->IsInCombat())
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && botAI->HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. Mount. - bot name: {}", bot->GetName());

        bot->Dismount();
        return false;
    }

    // [drift_guardian_gate] no guardian before the tank holds the target
    if (!GuardianSummonRefusal(botAI, spell).empty())
        return false;

    // Spell* currentSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL); //not used, line marked for removal.
    return botAI->CanCastSpell(spell, GetTarget());
}

// Out of combat a corpse round a corner fails the cast check (no line of sight is not SPELL_FAILED_OUT_OF_RANGE), and
// the prerequisite walk into its sight never ran: RFC 2026-10-24 18:28, the paladin stood 12-19 yd from the tank's
// corpse ("redemption ... no line of sight") until the tank released 3.5 minutes later.
bool ResurrectPartyMemberAction::isPossible()
{
    if (CastSpellAction::isPossible())
        return true;

    Unit* target = GetTarget();
    if (!target || bot->IsInCombat() || botAI->IsInVehicle() || target->GetMap() != bot->GetMap() ||
        bot->IsWithinLOSInMap(target))
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && bot->HasSpell(spellId) && !bot->HasSpellCooldown(spellId);
}

CastMeleeSpellAction::CastMeleeSpellAction(
    PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell)
{
    range = ATTACK_DISTANCE;
}

bool CastMeleeSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !bot->IsWithinMeleeRange(target))
        return false;

    return CastSpellAction::isUseful();
}

CastMeleeDebuffSpellAction::CastMeleeDebuffSpellAction(
    PlayerbotAI* botAI, std::string const spell, bool isOwner, float needLifeTime) :
    CastDebuffSpellAction(botAI, spell, isOwner, needLifeTime)
{
    range = ATTACK_DISTANCE;
}

bool CastMeleeDebuffSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !bot->IsWithinMeleeRange(target))
        return false;

    return CastDebuffSpellAction::isUseful();
}

bool CastAuraSpellAction::isUseful()
{
    if (!GetTarget() || !CastSpellAction::isUseful())
        return false;

    // [lead395] b201 rfc-h 5H 1-21 s: the pull waited 20 s on "Rthunad buffing" - in its rebuff window the hunter
    // recast Mend Pet every global cooldown (the window's refresh rule: an aura that runs out before the next fight
    // ends is recast) and its Trueshot Aura, the one buff it lacked, failed NOT_READY on that cooldown each tick, so
    // the window never had a tick without a buff trigger. The window's refresh rule is the buff pass's: a buff
    // (CastBuffSpellAction) - a heal over time or a debuff is cast while missing, or below its own refresh time
    Aura* aura = botAI->GetAura(spell, GetTarget(), isOwner, checkDuration);
    if (!aura)
        return true;
    return beforeDuration && uint32(aura->GetDuration()) < beforeDuration;  // a permanent aura (-1) is no refresh
}

bool CastBuffSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !CastSpellAction::isUseful())
        return false;

    Aura* aura = botAI->GetAura(spell, target, isOwner, checkDuration);
    return BuffBelowRefreshTarget(botAI, aura, beforeDuration);
}

bool CastBuffSpellAction::Execute(Event /*event*/)
{
    // [lead86] only a buff really cast is buff work (a failed cast kept the rebuff window open)
    bool const cast = botAI->CastSpell(spell, GetTarget());
    if (cast && botAI->forceRebuff.IsPending())
        botAI->forceRebuff.NoteBuffWork(AI_VALUE2(uint32, "spell id", spell));  // [lead1095]
    return cast;
}

// [b31_shared] Lifebloom on the tank was vetoed 7076 times at L80, Bone Shield / Beacon of Light the same way
Action::ActionThreatType CastBuffSpellAction::getThreatType()
{
    Unit* target = GetTarget();
    if (!target || target == bot || bot->IsFriendlyTo(target))
        return ActionThreatType::None;

    return ActionThreatType::Single;
}

bool GroupBuffSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !CastSpellAction::isUseful())
        return false;

    if (ai::buff::IsGroupVariantEnabled(bot, spell))
    {
        std::string const groupVariant = ai::buff::GroupVariantFor(spell);
        if (!groupVariant.empty() && !BuffBelowRefreshTarget(
                botAI, botAI->GetAura(groupVariant, target, isOwner, checkDuration), beforeDuration))
            return false;
    }

    Aura* aura = botAI->GetAura(spell, target, isOwner, checkDuration);
    return BuffBelowRefreshTarget(botAI, aura, beforeDuration);
}

bool GroupBuffSpellAction::Execute(Event /*event*/)
{

    std::string missingReagentGroupName;
    std::string const castName = ai::buff::UpgradeToGroupIfAppropriate(
        bot, botAI, spell, &missingReagentGroupName);

    if (!botAI->CastSpell(castName, GetTarget()))
        return false;
    botAI->forceRebuff.NoteBuffWork();  // [lead86] only a buff really cast

    if (!missingReagentGroupName.empty())
        ai::buff::TryAnnounceMissingBuffReagents(botAI, spell, missingReagentGroupName);

    return true;
}

CastEnchantItemMainHandAction::CastEnchantItemMainHandAction(
    PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

bool CastEnchantItemMainHandAction::Execute(Event /*event*/)
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    return item && botAI->CastSpell(spell, bot, item);
}

bool CastEnchantItemMainHandAction::isPossible()
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!item || item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_MISC ||
        item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE ||
        item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
    {
        return false;
    }

    return botAI->CanCastSpell(spell, bot, item);
}

CastEnchantItemOffHandAction::CastEnchantItemOffHandAction(
    PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

bool CastEnchantItemOffHandAction::Execute(Event /*event*/)
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    return item && botAI->CastSpell(spell, bot, item);
}

bool CastEnchantItemOffHandAction::isPossible()
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!item || item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_MISC ||
        item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
    {
        return false;
    }

    return botAI->CanCastSpell(spell, bot, item);
}

CastHealingSpellAction::CastHealingSpellAction(PlayerbotAI* botAI, std::string const spell, uint8 estAmount,
                                               HealingManaEfficiency manaEfficiency, bool isOwner)
    : CastAuraSpellAction(botAI, spell, isOwner), manaEfficiency(manaEfficiency), estAmount(estAmount)
{
    range = botAI->GetRange("heal");
}

bool CastHealingSpellAction::isUseful() { return CastAuraSpellAction::isUseful(); }

// The threat strategy holds Aoe actions at 50 % (30 % careful) of the tank's threat on any mob near the bot or its
// target, and reads 100 % for a mob the tank has not touched yet. Every heal is Aoe, so on a pack the tank was still
// picking up the holy paladin held all its heals: Stockade 2026-10-24, Bazil Thredd, the tank went 87 -> 25 % in
// 13 s while Lay on Hands, Holy Light and Flash of Light dropped to relevance 0 (Deadmines the same day: "flash of
// light on party ... multiplier threat x0.00") and Judgement of Light (single target) ran instead. Only a top-off
// waits for the threat now; a hurt member is healed whatever it costs.
Action::ActionThreatType CastHealingSpellAction::getThreatType()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive())
        return ActionThreatType::Aoe;

    float const health = target->GetHealthPct();
    if (health < sPlayerbotAIConfig.MediumHealth)
        return ActionThreatType::None;

    // No early bypass for a tank above MediumHealth: AzerothCore splits heal threat (0.5 per point) evenly over every
    // mob in combat with the healed target (ThreatManager::ForwardThreatForAssistingMe), and a mob the tank has not
    // hit yet holds ~0 threat on him, so a top-off heal right after the pull turns that mob to the healer.
    float const fastLoss = sPlayerbotAIConfig.HealerFastHealthLossPct;
    if (fastLoss > 0.0f &&
        RecentHealthLossPct(target, context->GetValue<HealthLossMap>("group health loss")->RefGet()) >= fastLoss)
        return ActionThreatType::None;

    return ActionThreatType::Aoe;
}

bool CastAoeHealSpellAction::isUseful() { return CastSpellAction::isUseful(); }

CastCureSpellAction::CastCureSpellAction(
    PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell)
{
    range = botAI->GetRange("heal");
}

Value<Unit*>* CurePartyMemberAction::GetTargetValue()
{
    return context->GetValue<Unit*>("party member to dispel", dispelType);
}

Value<Unit*>* CastSpellOnGroupMemberAttackerAction::GetTargetValue()
{
    return context->GetValue<Unit*>("group member attacker");
}

// A short-reach peel spell (Hammer of the Righteous, Shield of Righteousness, Judgement, Hammer of Justice) goes to
// the best loose mob within its reach when the pick stands further away: wave 25 trails show "hammer of the
// righteous on group member attacker - USELESS" with other loose mobs 0-3 yd from the tank, and the tick ended with
// "no actions executed".
Unit* CastSpellOnGroupMemberAttackerAction::GetTarget()
{
    Unit* pick = GetTargetValue()->Get();
    if (!pick)
        return nullptr;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    float const reach = spellInfo ? spellInfo->GetMaxRange(false, bot) : 0.0f;
    if (reach <= 0.0f)
        return pick;

    uint32 const now = getMSTime();
    // [taunt_far_first] A taunt or a ranged threat spell goes to the loose mob out of the tank's reach first: the
    // hands and the area threat hold the one beside the tank.
    if (reach > SHORT_REACH)
    {
        if (!_reachCheckedMs || getMSTimeDiff(_reachCheckedMs, now) >= GroupMemberAttackerValue::CHECK_INTERVAL_MS)
        {
            _reachCheckedMs = now;
            Unit* forTaunt = GroupMemberAttackerValue::FindForTaunt(botAI, reach);
            _reachTarget = forTaunt ? forTaunt->GetGUID() : ObjectGuid::Empty;
        }

        Unit* forTaunt = _reachTarget ? botAI->GetUnit(_reachTarget) : nullptr;
        Unit* victim = forTaunt && forTaunt->IsAlive() ? forTaunt->GetVictim() : nullptr;
        Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
        bool const stillLoose = victimPlayer && victimPlayer != bot && !PlayerbotAI::IsTank(victimPlayer);
        return stillLoose ? forTaunt : pick;  // a mob back on a tank since the last look is not taunted
    }

    if (bot->IsWithinCombatRange(pick, reach))
        return pick;

    if (!_reachCheckedMs || getMSTimeDiff(_reachCheckedMs, now) >= GroupMemberAttackerValue::CHECK_INTERVAL_MS)
    {
        _reachCheckedMs = now;
        Unit* inReach = GroupMemberAttackerValue::FindInReach(botAI, reach);
        _reachTarget = inReach ? inReach->GetGUID() : ObjectGuid::Empty;
    }

    Unit* inReach = _reachTarget ? botAI->GetUnit(_reachTarget) : nullptr;
    return inReach && inReach->IsAlive() ? inReach : pick;
}

bool CastSpellOnGroupMemberAttackerAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    // An out-of-range cast passes CanCastSpell and fails every tick: Hammer of Justice reaches 10 yd, not 30.
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    Unit* target = GetTarget();
    if (!spellInfo || !target || !bot->IsWithinCombatRange(target, spellInfo->GetMaxRange(false, bot)))
        return false;

    // [b32_peel] a peel only on a mob in the pack or on a group member; a gap closer (Feral Charge) only onto a mob
    // next to the member it hits: a caster hitting from range is taunted or pulled in, not charged (dmn-n-5H-48)
    if (PlayerbotAI::IsTank(bot) &&
        (!TankPeel::MayPeel(bot, target) || (TankPeel::IsGapCloser(spellInfo) && !TankPeel::MayCloseGap(bot, target))))
        return false;

    // [b40_strat] the gap closer's own ground and door guard (GapCloserLeavesGround: a target in the air, down a ledge,
    // through an open auto-closing door). This generic peel path ("feral charge - bear on group member attacker") ran
    // past it: strat-h 20261026-060218-strat-h-5H-1, the bear charged through King's Square Gate 175353.
    if (TankPeel::IsGapCloser(spellInfo) && GapCloserLeavesGround(bot, target, spell.c_str()))
        return false;

    if (!healerOnly)
        return true;

    Unit* victim = target->GetVictim();
    Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
    return victimPlayer && PlayerbotAI::IsHeal(victimPlayer);
}

bool CastSpellOnGroupMemberAttackerAction::Execute(Event event)
{
    Unit* target = GetTarget();
    ObjectGuid const guid = target ? target->GetGUID() : ObjectGuid::Empty;
    Unit* victim = target ? target->GetVictim() : nullptr;  // before the cast turns it
    if (!CastSpellAction::Execute(event))
        return false;

    GroupMemberAttackerValue::NoteReaction(botAI, target, victim, spell, name);

    // The next peel spell goes to another victim's mob while this one is held. Only a taunt or a stun holds it:
    // after a Hammer of the Righteous or an Icy Touch the mob stays on the healer, and marking it peeled sent the
    // taunts and Righteous Defense to other victims for PEEL_SPREAD_MS.
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (guid && spellInfo &&
        (spellInfo->HasEffect(SPELL_EFFECT_ATTACK_ME) || spellInfo->HasAura(SPELL_AURA_MOD_TAUNT) ||
         spellInfo->HasAura(SPELL_AURA_MOD_STUN)))
        GroupMemberAttackerValue::RecordPeel(botAI, {guid});

    return true;
}

Unit* CastSpellOnAttackedGroupMemberAction::GetTarget() { return AI_VALUE(Unit*, "attacked group member"); }

bool CastSpellOnAttackedGroupMemberAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    if (minFreshMobs <= 1)
        return true;

    Unit* friendUnit = GetTarget();
    if (!friendUnit)
        return false;

    uint32 fresh = 0;
    for (Unit* mob : friendUnit->getAttackers())
    {
        if (mob && mob->GetVictim() == friendUnit && !GroupMemberAttackerValue::IsRecentlyPeeled(botAI, mob->GetGUID()))
            ++fresh;
    }

    return fresh >= minFreshMobs;
}

bool CastSpellOnAttackedGroupMemberAction::Execute(Event event)
{
    Unit* friendUnit = GetTarget();
    if (!friendUnit)
        return false;

    // Righteous Defense takes the friend's attackers: these are the mobs it covers (read from the friend: "attackers"
    // lags a second and misses some, see GatherPeelCandidates)
    GuidVector taken;
    for (Unit* mob : friendUnit->getAttackers())
    {
        if (mob && mob->GetVictim() == friendUnit)
            taken.push_back(mob->GetGUID());
    }

    if (!CastSpellAction::Execute(event))
        return false;

    for (ObjectGuid const& guid : taken)
        GroupMemberAttackerValue::NoteReaction(botAI, botAI->GetUnit(guid), friendUnit, spell, name);

    GroupMemberAttackerValue::RecordPeel(botAI, taken);
    return true;
}

TauntReactionAction::Spells const* TauntReactionAction::ClassSpells(Player* bot)
{
    static Spells const PALADIN = {"hand of reckoning", "avenger's shield", "righteous defense", true};
    static Spells const WARRIOR = {"taunt", "heroic throw", "challenging shout", false};
    static Spells const DRUID = {"growl", "faerie fire (feral)", "challenging roar", false};
    static Spells const DEATH_KNIGHT = {"dark command", "death grip", nullptr, false};
    switch (bot->getClass())
    {
        case CLASS_PALADIN:
            return &PALADIN;
        case CLASS_WARRIOR:
            return &WARRIOR;
        case CLASS_DRUID:
            return &DRUID;
        case CLASS_DEATH_KNIGHT:
            return &DEATH_KNIGHT;
        default:
            return nullptr;
    }
}

bool TauntReactionAction::SpellReady(PlayerbotAI* botAI, char const* spell)
{
    if (!spell)
        return false;

    Player* bot = botAI->GetBot();
    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    return spellId && bot->HasSpell(spellId) && !bot->HasSpellCooldown(spellId);
}

float TauntReactionAction::SpellRange(PlayerbotAI* botAI, char const* spell, float fallback)
{
    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    float const range = spellInfo ? spellInfo->GetMaxRange(spellInfo->IsPositive(), botAI->GetBot()) : 0.0f;
    return range > 0.0f ? range : fallback;
}

// Ready, the target in range and in sight, and the cast's own checks (form, stance, global cooldown) pass.
bool TauntReactionAction::Usable(PlayerbotAI* botAI, char const* spell, Unit* target)
{
    Player* bot = botAI->GetBot();
    if (!spell || !target || !SpellReady(botAI, spell))
        return false;

    if (target != bot)
    {
        float const range = SpellRange(botAI, spell, DEFAULT_TAUNT_RANGE);
        bool const inRange = target->ToPlayer() ? bot->IsWithinDistInMap(target, range)
                                                : bot->IsWithinCombatRange(target, range);
        if (!inRange || !bot->IsWithinLOSInMap(target))
            return false;
    }

    return botAI->CanCastSpell(spell, target);
}

std::string TauntReactionAction::ReadyFor(PlayerbotAI* botAI, Unit* mob)
{
    Player* bot = botAI->GetBot();
    Spells const* spells = ClassSpells(bot);
    if (!spells || !mob || !mob->IsAlive())
        return "";

    if (Usable(botAI, spells->taunt, mob))
        return spells->taunt;

    if (bot->IsWithinMeleeRange(mob))
        return "";

    // Death Grip on a caster only: a melee mob on its way is held by Dark Command and the hands
    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        Unit* victim = mob->GetVictim();
        bool const caster = mob->IsNonMeleeSpellCast(false) || (victim && !mob->IsWithinMeleeRange(victim));
        if (!caster)
            return "";
    }

    return Usable(botAI, spells->ranged, mob) ? spells->ranged : "";
}

// [lead520] the class's backups while its taunt cools, the ones that taunt first: Death Grip (it taunts and pulls the
// mob in), Righteous Defense on the victim, Mocking Blow, the area taunts (Challenging Shout / Roar) with the mob in
// their reach of the tank; then the gap closers that bring the tank to a mob out of its melee (Intercept, Feral Charge
// - Bear), where its threat work takes the mob back. Each by the game's own cast checks (Usable)
namespace
{
    struct BackupSpell
    {
        char const* spell;
        bool onVictim;  // cast on the attacked member (Righteous Defense)
        bool onSelf;    // an area taunt round the tank
        bool holds;     // it taunts (the mob is marked peeled)
    };
    // [lead521] the class's backups, the ones that taunt first, then the gap closers
    void BackupSpells(Player* bot, BackupSpell const*& begin, BackupSpell const*& end)
    {
        static BackupSpell const PALADIN[] = {{"righteous defense", true, false, true}};
        static BackupSpell const WARRIOR[] = {{"mocking blow", false, false, true},
                                              {"challenging shout", false, true, true},
                                              {"intercept", false, false, false}};
        static BackupSpell const DRUID[] = {{"challenging roar", false, true, true},
                                            {"feral charge - bear", false, false, false}};
        static BackupSpell const DEATH_KNIGHT[] = {{"death grip", false, false, true}};
        begin = end = nullptr;
        switch (bot->getClass())
        {
            case CLASS_PALADIN:
                begin = std::begin(PALADIN);
                end = std::end(PALADIN);
                break;
            case CLASS_WARRIOR:
                begin = std::begin(WARRIOR);
                end = std::end(WARRIOR);
                break;
            case CLASS_DRUID:
                begin = std::begin(DRUID);
                end = std::end(DRUID);
                break;
            case CLASS_DEATH_KNIGHT:
                begin = std::begin(DEATH_KNIGHT);
                end = std::end(DEATH_KNIGHT);
                break;
            default:
                break;
        }
    }
}

bool TauntReactionAction::AnyBackupReady(PlayerbotAI* botAI)
{
    BackupSpell const* begin = nullptr;
    BackupSpell const* end = nullptr;
    BackupSpells(botAI->GetBot(), begin, end);
    for (BackupSpell const* backup = begin; backup && backup != end; ++backup)
        if (SpellReady(botAI, backup->spell))
            return true;
    return false;
}

bool TauntReactionAction::BackupTaunt(PlayerbotAI* botAI, Unit* mob, Unit* victim, Plan& plan)
{
    Player* bot = botAI->GetBot();
    BackupSpell const* begin = nullptr;
    BackupSpell const* end = nullptr;
    BackupSpells(bot, begin, end);
    if (!begin)
        return false;
    if (!mob || !victim)
        return false;
    bool const inMelee = bot->IsWithinMeleeRange(mob);
    for (BackupSpell const* backup = begin; backup != end; ++backup)
    {
        // a gap closer only to a mob out of the tank's melee
        if (!backup->holds && inMelee)
            continue;
        // [lead760] ... and only where the leader's fight is planned to stand (TankWalkMovesFight)
        if (!backup->holds && !Leader::Fight::TankWalkMovesFight(bot, mob, true).empty())
            continue;
        // [lead925] ... and where its landing passes the leader's check (a healer's heal reach, or it lives till then)
        if (!backup->holds && !Leader::Fight::TankGapCloserAllowed(bot, mob, backup->spell))
            continue;
        if (backup->onSelf && !bot->IsWithinDistInMap(mob, AOE_TAUNT_RADIUS))
            continue;
        Unit* castOn = backup->onVictim ? victim : backup->onSelf ? static_cast<Unit*>(bot) : mob;
        if (!Usable(botAI, backup->spell, castOn))
            continue;
        plan.spell = backup->spell;
        plan.target = castOn->GetGUID();
        plan.victim = victim->GetGUID();
        plan.mobs = {mob->GetGUID()};
        plan.holds = backup->holds;
        return true;
    }
    return false;
}

bool TauntReactionAction::MakePlan(Plan& plan)
{
    plan = Plan();
    Spells const* spells = ClassSpells(bot);
    if (!spells || !PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return false;

    // 1. two or more fresh mobs on one member: the area taunt takes them all
    if (spells->swarm && SpellReady(botAI, spells->swarm))
    {
        Unit* friendUnit = AI_VALUE(Unit*, "attacked group member");
        Player* friendPlayer = friendUnit ? friendUnit->ToPlayer() : nullptr;
        if (friendPlayer && friendPlayer->IsAlive())
        {
            GuidVector fresh;
            GuidVector beside;  // within the shouts' reach of the tank
            for (Unit* mob : friendPlayer->getAttackers())
            {
                if (!mob || !mob->IsAlive() || mob->GetVictim() != friendPlayer ||
                    GroupMemberAttackerValue::IsRecentlyPeeled(botAI, mob->GetGUID()))
                    continue;

                fresh.push_back(mob->GetGUID());
                if (bot->IsWithinDistInMap(mob, AOE_TAUNT_RADIUS))
                    beside.push_back(mob->GetGUID());
            }

            if (spells->swarmOnFriend && fresh.size() >= MIN_SWARM_MOBS &&
                Usable(botAI, spells->swarm, friendPlayer))
            {
                plan.spell = spells->swarm;
                plan.target = friendPlayer->GetGUID();
                plan.victim = friendPlayer->GetGUID();
                plan.mobs = fresh;
                plan.holds = true;
                return true;
            }

            // a three-minute shout: for the healer's mobs, or with the single taunt cooling
            if (!spells->swarmOnFriend && beside.size() >= MIN_SWARM_MOBS &&
                (PlayerbotAI::IsHeal(friendPlayer) || !SpellReady(botAI, spells->taunt)) &&
                Usable(botAI, spells->swarm, bot))
            {
                plan.spell = spells->swarm;
                plan.target = bot->GetGUID();
                plan.victim = friendPlayer->GetGUID();
                plan.mobs = beside;
                plan.holds = true;
                return true;
            }
        }

        // [bear_aoe_threat] The shouts (Challenging Shout, Challenging Roar) once SWARM_SPREAD_MOBS or more mobs
        // around the tank hit non-tank members, spread over several of them: the step above counts only the one
        // "attacked group member". gd-h build 30 (20261025-184229-gd-h-5B-38, Slad'ran): Constrictors and Vipers from
        // the pipes went to the stacked healer and priest, 74 aggro losses in 60 s, the Prot Warrior never shouted.
        if (!spells->swarmOnFriend && Usable(botAI, spells->swarm, bot))
        {
            GuidVector spread;
            ObjectGuid firstVictim;
            for (ObjectGuid const& guid : AI_VALUE(GuidVector, "attackers"))
            {
                Unit* mob = botAI->GetUnit(guid);
                Unit* victim = mob && mob->IsAlive() ? mob->GetVictim() : nullptr;
                Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
                if (!victimPlayer || victimPlayer == bot || !victimPlayer->IsInSameGroupWith(bot) ||
                    PlayerbotAI::IsTank(victimPlayer) || !bot->IsWithinDistInMap(mob, AOE_TAUNT_RADIUS) ||
                    GroupMemberAttackerValue::IsRecentlyPeeled(botAI, guid))
                    continue;

                if (spread.empty())
                    firstVictim = victimPlayer->GetGUID();
                spread.push_back(guid);
            }

            if (spread.size() >= SWARM_SPREAD_MOBS)
            {
                plan.spell = spells->swarm;
                plan.target = bot->GetGUID();
                plan.victim = firstVictim;
                plan.mobs = spread;
                plan.holds = true;
                return true;
            }
        }
    }

    // 2. / 3. the single taunt, else a ranged threat spell, on the mob out of the tank's reach first
    float const reach = std::max(SpellRange(botAI, spells->taunt, DEFAULT_TAUNT_RANGE),
                                 SpellRange(botAI, spells->ranged, DEFAULT_TAUNT_RANGE));
    Unit* mob = GroupMemberAttackerValue::FindForTaunt(botAI, reach);
    Unit* victim = mob ? mob->GetVictim() : nullptr;
    if (!victim)
        return false;

    std::string const spell = ReadyFor(botAI, mob);
    // [lead520] user 2026-11-02: with the main taunt on its cooldown the tank uses its backups. b211 rfc-h (8 runs):
    // 30 real aggro losses, the taunt on its cooldown at about 9 of them, and Death Grip / Challenging Shout /
    // Challenging Roar / Feral Charge / Intercept were cast 0 times (this plan returned nothing in melee reach)
    if ((spell.empty() || spell == spells->ranged) && !SpellReady(botAI, spells->taunt) &&
        BackupTaunt(botAI, mob, victim, plan))
        return true;
    if (spell.empty())
        return false;

    plan.spell = spell;
    plan.target = mob->GetGUID();
    plan.victim = victim->GetGUID();
    plan.mobs = {mob->GetGUID()};
    // Death Grip pulls the mob to the tank and taunts it; Avenger's Shield, Heroic Throw and Faerie Fire only add
    // threat
    plan.holds = spell == spells->taunt || spell == "death grip";
    return true;
}

bool TauntReactionAction::isUseful()
{
    uint32 const now = getMSTime();
    if (_plan.madeMs && getMSTimeDiff(_plan.madeMs, now) < PLAN_MS)
        return !_plan.spell.empty();

    MakePlan(_plan);
    _plan.madeMs = now ? now : 1;
    return !_plan.spell.empty();
}

bool TauntReactionAction::Execute(Event /*event*/)
{
    Plan const plan = _plan;
    _plan = Plan();  // chosen afresh next time
    Unit* target = plan.spell.empty() ? nullptr : botAI->GetUnit(plan.target);
    Unit* victim = botAI->GetUnit(plan.victim);
    if (!target || !target->IsAlive() || !victim)
        return false;

    // the mobs still on the victim, read before the cast turns them
    std::vector<Unit*> mobs;
    for (ObjectGuid const& guid : plan.mobs)
    {
        Unit* mob = botAI->GetUnit(guid);
        if (mob && mob->IsAlive() && mob->GetVictim() == victim)
            mobs.push_back(mob);
    }

    if (mobs.empty() || !botAI->CastSpell(plan.spell, target))
        return false;

    for (Unit* mob : mobs)
        GroupMemberAttackerValue::NoteReaction(botAI, mob, victim, plan.spell, getName());

    if (plan.holds)
        GroupMemberAttackerValue::RecordPeel(botAI, plan.mobs);

    return true;
}

uint8 FleeStopAction::HoldOf(uint32 spellId)
{
    // [lead1210] the hold by the spell's own auras: a stun stops the run, a root holds it in place, a snare slows it;
    // none (Death Grip's pull, a totem's summon) holds nothing by itself
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!info)
        return HOLD_NONE;
    uint8 hold = HOLD_NONE;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.IsAura())
            continue;
        if (effect.ApplyAuraName == SPELL_AURA_MOD_STUN)
            hold = std::max(hold, HOLD_STUN);
        else if (effect.ApplyAuraName == SPELL_AURA_MOD_ROOT)
            hold = std::max(hold, HOLD_ROOT);
        else if (effect.ApplyAuraName == SPELL_AURA_MOD_DECREASE_SPEED)
            hold = std::max(hold, HOLD_SNARE);
    }
    return hold;
}

uint8 FleeStopAction::HeldBy(Unit* mob)
{
    if (!mob)
        return HOLD_NONE;
    if (mob->HasAuraType(SPELL_AURA_MOD_STUN))
        return HOLD_STUN;
    if (mob->HasAuraType(SPELL_AURA_MOD_ROOT))
        return HOLD_ROOT;
    if (mob->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED))
        return HOLD_SNARE;
    return HOLD_NONE;
}

FleeStopAction::Stop const* FleeStopAction::Pick(Unit* mob, uint8* holdOut)
{
    // [lead1210] the class's stops; the one cast is the usable one that holds the runner most by spell data (HoldOf)
    static Stop const PALADIN[] = {{"hammer of justice", false}, {"avenger's shield", false}};
    static Stop const HUNTER[] = {{"concussive shot", false}, {"wing clip", false}};
    static Stop const MAGE[] = {{"frost nova", true}, {"frostbolt", false}};
    static Stop const WARRIOR[] = {
        {"concussion blow", false}, {"intercept", false}, {"hamstring", false}, {"piercing howl", true}};
    static Stop const SHAMAN[] = {{"frost shock", false}, {"earthbind totem", true}};
    static Stop const DRUID[] = {{"bash", false}, {"maim", false}, {"feral charge - bear", false},
                                 {"entangling roots", false}};
    static Stop const DEATH_KNIGHT[] = {{"chains of ice", false}, {"death grip", false}};
    static Stop const ROGUE[] = {{"kidney shot", false}, {"gouge", false}, {"deadly throw", false}};
    static Stop const WARLOCK[] = {{"curse of exhaustion", false}};
    static Stop const PRIEST[] = {{"mind flay", false}};

    Stop const* begin = nullptr;
    Stop const* end = nullptr;
    auto const use = [&begin, &end](auto const& list)
    {
        begin = std::begin(list);
        end = std::end(list);
    };
    switch (bot->getClass())
    {
        case CLASS_PALADIN:
            use(PALADIN);
            break;
        case CLASS_HUNTER:
            use(HUNTER);
            break;
        case CLASS_MAGE:
            use(MAGE);
            break;
        case CLASS_WARRIOR:
            use(WARRIOR);
            break;
        case CLASS_SHAMAN:
            use(SHAMAN);
            break;
        case CLASS_DRUID:
            use(DRUID);
            break;
        case CLASS_DEATH_KNIGHT:
            use(DEATH_KNIGHT);
            break;
        case CLASS_ROGUE:
            use(ROGUE);
            break;
        case CLASS_WARLOCK:
            use(WARLOCK);
            break;
        case CLASS_PRIEST:
            use(PRIEST);
            break;
        default:
            return nullptr;
    }

    // [lead1210] b240 rfc-h 5G-15 106 s: the frost DK's stop on a Ragefire Shaman at 0.5 yd was Death Grip (the
    // list's first) - a pull with no hold, at its feet it held nothing; the shaman ran 87 yd and brought Shaman 27.
    // A spell with no hold of its own (a pull) is a stop only on a runner out of the caster's melee (it brings it
    // back); the others rank by their hold, the list's order breaks a tie
    Stop const* best = nullptr;
    uint8 bestHold = 0;
    for (Stop const* stop = begin; stop != end; ++stop)
    {
        uint8 const hold = HoldOf(AI_VALUE2(uint32, "spell id", stop->spell));
        if (hold == HOLD_NONE && !stop->aroundSelf && bot->IsWithinMeleeRange(mob))
            continue;
        if (best && hold <= bestHold)
            continue;
        bool usable = false;
        if (stop->aroundSelf)
            usable = bot->IsWithinDistInMap(mob, SELF_REACH) && TauntReactionAction::Usable(botAI, stop->spell, bot);
        else
            usable = TauntReactionAction::Usable(botAI, stop->spell, mob);
        if (!usable)
            continue;
        best = stop;
        bestHold = hold;
    }
    if (holdOut)
        *holdOut = bestHold;
    return best;
}

uint8 FleeStopAction::BestHold(Unit* mob)
{
    uint8 hold = HOLD_NONE;
    return mob && Pick(mob, &hold) ? hold : HOLD_NONE;
}

bool FleeStopAction::isUseful()
{
    Unit* mob = AI_VALUE(Unit*, "fleeing mob");
    if (!mob || PlayerbotAI::IsHeal(bot) || FledMobs::StoppedRecently(mob))
        return false;

    // already held: a stun or a root from somebody else stops it as well
    if (mob->HasAuraType(SPELL_AURA_MOD_STUN) || mob->HasAuraType(SPELL_AURA_MOD_ROOT) || IsCrowdControlled(mob))
        return false;

    // [lead1210] a snared runner takes only a stronger hold
    uint8 hold = HOLD_NONE;
    return Pick(mob, &hold) != nullptr && (HeldBy(mob) == HOLD_NONE || hold > HeldBy(mob));
}

bool FleeStopAction::Execute(Event /*event*/)
{
    Unit* mob = AI_VALUE(Unit*, "fleeing mob");
    uint8 hold = HOLD_NONE;
    Stop const* stop = mob ? Pick(mob, &hold) : nullptr;
    if (!stop || !botAI->CastSpell(stop->spell, stop->aroundSelf ? static_cast<Unit*>(bot) : mob))
        return false;

    // [lead1210] only a hold marks the runner stopped: after a pull the next member's stop goes at once
    if (hold != HOLD_NONE || stop->aroundSelf)
        FledMobs::MarkStopped(mob);
    MotionMaster* motion = mob->GetMotionMaster();
    char const* kind =
        motion && motion->GetCurrentMovementGeneratorType() == ASSISTANCE_MOTION_TYPE ? "for help" : "in fear";
    float const distance = bot->GetExactDist(mob);
    LOG_INFO("playerbots", "[flee stop] {}: {} on {} {} running {} at {:.0f} % health, {:.1f} yd", bot->GetName(),
             stop->spell, mob->GetName(), mob->GetEntry(), kind, mob->GetHealthPct(), distance);
    BotMoveEvents::Push(bot->GetGUID(), BotMoveEvents::Event{"flee_stop", {}, {}}
                                            .Str("name", bot->GetName())
                                            .Str("mob", mob->GetName())
                                            .Num("entry", mob->GetEntry())
                                            .Str("spell", stop->spell)
                                            .Num("hold", hold)
                                            .Str("kind", kind)
                                            .Real("hpPct", mob->GetHealthPct())
                                            .Real("distance", distance));
    return true;
}

bool CastAreaSpellOnGroupMemberAttackersAction::isUseful()
{
    if (!PlayerbotAI::IsTank(bot))
        return false;

    // the "group member attacker count" qualifier is the range in yards around the bot
    if (!AI_VALUE2(uint8, "group member attacker count", std::to_string(static_cast<int32>(radius))))
        return false;

    return CastSpellAction::isUseful();
}

Value<Unit*>* BuffOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>("party member without aura", spell);
}

Value<Unit*>* GroupBuffOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>("party member without aura", MakeAuraQualifierForBuff(spell));
}

CastShootAction::CastShootAction(
    PlayerbotAI* botAI) : CastSpellAction(botAI, "shoot"), shootSpellId(0)
{
    if (Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
    {
        spell = "shoot";

        switch (pItem->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_GUN:
                spell += " gun";
                shootSpellId = 3018;
                break;
            case ITEM_SUBCLASS_WEAPON_BOW:
                spell += " bow";
                shootSpellId = 3018;
                break;
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                spell += " crossbow";
                shootSpellId = 3018;
                break;
            case ITEM_SUBCLASS_WEAPON_THROWN:
                spell = "throw";
                shootSpellId = 2764;
                break;
        }
    }
}

bool CastShootAction::isPossible()
{
    if (shootSpellId)
        return botAI->CanCastSpell(shootSpellId, GetTarget(), false);

    return CastSpellAction::isPossible();
}

bool CastShootAction::Execute(Event /*event*/)
{
    if (shootSpellId)
        return botAI->CastSpell(shootSpellId, GetTarget());

    return botAI->CastSpell(spell, GetTarget());
}

Value<Unit*>* CastDebuffSpellOnAttackerAction::GetTargetValue()
{
    return context->GetValue<Unit*>("attacker without aura", spell);
}

Value<Unit*>* CastDebuffSpellOnMeleeAttackerAction::GetTargetValue()
{
    return context->GetValue<Unit*>("melee attacker without aura", spell);
}

CastBuffSpellAction::CastBuffSpellAction(
    PlayerbotAI* botAI, std::string const spell, bool checkIsOwner, uint32 beforeDuration)
    : CastAuraSpellAction(botAI, spell, checkIsOwner, false, beforeDuration)
{
    range = botAI->GetRange("spell");
}

Value<Unit*>* CastSpellOnEnemyHealerAction::GetTargetValue()
{
    return context->GetValue<Unit*>("enemy healer target", spell);
}

Value<Unit*>* CastSnareSpellAction::GetTargetValue() { return context->GetValue<Unit*>("snare target", spell); }

Value<Unit*>* CastCrowdControlSpellAction::GetTargetValue() { return context->GetValue<Unit*>("cc target", getName()); }

bool CastCrowdControlSpellAction::Execute(Event /*event*/) { return botAI->CastSpell(getName(), GetTarget()); }

bool CastCrowdControlSpellAction::isPossible()
{
    // Crowd control only on a mob the group does not fight now (IsGroupKillTarget), never its kill target.
    Unit* target = GetTarget();
    if (target && RefuseCrowdControlOnKillTarget(botAI, target, getName()))
        return false;

    return botAI->CanCastSpell(getName(), target);
}

bool CastCrowdControlSpellAction::isUseful() { return true; }

std::string const CastProtectSpellAction::GetTargetName() { return "party member to protect"; }

bool CastProtectSpellAction::isUseful() { return GetTarget() && !botAI->HasAura(spell, GetTarget()); }

bool CastVehicleSpellAction::isPossible()
{
    uint32 spellId = AI_VALUE2(uint32, "vehicle spell id", spell);
    return botAI->CanCastVehicleSpell(spellId, GetTarget());
}

bool CastVehicleSpellAction::isUseful() { return botAI->IsInVehicle(false, true); }

bool CastVehicleSpellAction::Execute(Event /*event*/)
{
    uint32 spellId = AI_VALUE2(uint32, "vehicle spell id", spell);
    return botAI->CastVehicleSpell(spellId, GetTarget());
}

bool CastEveryManForHimselfAction::isPossible()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && !HasSpellOrCategoryCooldown(bot, spellId);
}

bool CastWillOfTheForsakenAction::isPossible()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && !HasSpellOrCategoryCooldown(bot, spellId);
}

// [fear_counter] The racial breaks log a "[fear_counter]" line when they removed a fear / charm / sleep (Every Man
// for Himself also answers stuns and roots, which are not logged).
bool CastEveryManForHimselfAction::Execute(Event event)
{
    uint32 const aura = FearCounter::TremorableAuraSpell(bot);
    if (!CastSpellAction::Execute(event))
        return false;

    if (aura)
        FearCounter::Log(bot, spell, bot, aura);
    return true;
}

bool CastWillOfTheForsakenAction::Execute(Event event)
{
    uint32 const aura = FearCounter::TremorableAuraSpell(bot);
    if (!CastSpellAction::Execute(event))
        return false;

    if (aura)
        FearCounter::Log(bot, spell, bot, aura);
    return true;
}

bool UseTrinketAction::Execute(Event /*event*/)
{
    Item* trinket1 = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1);

    if (trinket1 && UseTrinket(trinket1))
        return true;

    Item* trinket2 = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2);
    if (trinket2 && UseTrinket(trinket2))
        return true;

    return false;
}

bool UseTrinketAction::UseTrinket(Item* item)
{
    if (bot->CanUseItem(item) != EQUIP_ERR_OK || bot->IsNonMeleeSpellCast(true))
        return false;

    uint8 bagIndex = item->GetBagSlot();
    uint8 slot = item->GetSlot();
    uint8 cast_count = 1;
    ObjectGuid item_guid = item->GetGUID();
    uint32 glyphIndex = 0;
    uint8 castFlags = 0;
    uint32 targetFlag = TARGET_FLAG_NONE;
    uint32 spellId = 0;
    int32 itemSpellCooldown = 0;
    uint32 itemSpellCategory = 0;
    int32 itemSpellCategoryCooldown = 0;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (item->GetTemplate()->Spells[i].SpellId > 0 &&
            item->GetTemplate()->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
        {
            spellId = item->GetTemplate()->Spells[i].SpellId;
            itemSpellCooldown = item->GetTemplate()->Spells[i].SpellCooldown;
            itemSpellCategory = item->GetTemplate()->Spells[i].SpellCategory;
            itemSpellCategoryCooldown = item->GetTemplate()->Spells[i].SpellCategoryCooldown;
            uint64 const itemCooldownKey = (static_cast<uint64>(item->GetEntry()) << 32) | spellId;
            uint32 const now = getMSTime();

            if (itemSpellCooldown > 0)
            {
                auto const itemCooldownItr = trinketItemCooldownExpiries.find(itemCooldownKey);
                if (itemCooldownItr != trinketItemCooldownExpiries.end())
                {
                    if (itemCooldownItr->second > now)
                        return false;

                    trinketItemCooldownExpiries.erase(itemCooldownItr);
                }
            }

            if (itemSpellCategory && itemSpellCategoryCooldown > 0)
            {
                auto const categoryCooldownItr = trinketCategoryCooldownExpiries.find(itemSpellCategory);
                if (categoryCooldownItr != trinketCategoryCooldownExpiries.end())
                {
                    if (categoryCooldownItr->second > now)
                        return false;

                    trinketCategoryCooldownExpiries.erase(categoryCooldownItr);
                }
            }

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || !spellInfo->IsPositive())
                return false;

            bool applyAura = false;
            bool restoresMana = false;
            bool improvesManaEfficiency = false;
            bool defensiveTankEffect = false;
            for (int i = 0; i < MAX_SPELL_EFFECTS; i++)
            {
                SpellEffectInfo const& effectInfo = spellInfo->Effects[i];
                if (effectInfo.Effect == SPELL_EFFECT_APPLY_AURA)
                    applyAura = true;

                restoresMana = restoresMana || IsManaRestoreEffect(effectInfo);
                improvesManaEfficiency = improvesManaEfficiency || IsManaEfficiencyEffect(effectInfo);
                defensiveTankEffect = defensiveTankEffect || IsDefensiveTankEffect(effectInfo);
            }

            if (!applyAura && !restoresMana)
                return false;

            if (restoresMana || improvesManaEfficiency)
            {
                if (!AI_VALUE2(bool, "has mana", "self target"))
                    return false;

                uint8 const manaPct = AI_VALUE2(uint8, "mana", "self target");
                if ((restoresMana && manaPct >= sPlayerbotAIConfig.MediumMana) ||
                    manaPct >= sPlayerbotAIConfig.HighMana)
                {
                    return false;
                }
            }

            if (defensiveTankEffect)
            {
                uint8 const healthPct = AI_VALUE2(uint8, "health", "self target");
                if (healthPct > sPlayerbotAIConfig.LowHealth)
                    return false;
            }

            auto const& mixedTriggerTrinketSpellIds = GetMixedTriggerTrinketSpellIds();
            // Exclude trinkets that expose the same spell as both ON_EQUIP and ON_USE across
            // item templates. Those are equip/proc effects leaking into the active-use path,
            // as seen with the error versions of Oracle Talisman of Ablution (44870) and
            // Frenzyheart Insignia of Fury (44869).
            if (mixedTriggerTrinketSpellIds.find(spellId) != mixedTriggerTrinketSpellIds.end())
                return false;

            // [lead512] a led damage dealer's / tank's offensive trinket by the leader's burn verdict on it (boss
            // first; on trash only when back before the next boss, or a critical pull), like its burn cooldowns; a
            // healer's trinkets stay its tool uses (BoostTrigger's rule)
            if (!restoresMana && !improvesManaEfficiency && !defensiveTankEffect && !PlayerbotAI::IsHeal(bot) &&
                Leader::LedBot(bot))
            {
                Leader::MemberEvents events;
                if (!Leader::Events::Get(bot->GetGUID(), events) ||
                    !Leader::BurnAllows(events, item->GetTemplate()->Name1))
                    return false;
            }

            if (!botAI->CanCastSpell(spellId, bot, false, nullptr, item))
                return false;

            break;
        }
    }

    if (!spellId)
        return false;

    WorldPacket packet(CMSG_USE_ITEM);
    packet << bagIndex << slot << cast_count << spellId << item_guid << glyphIndex << castFlags;

    targetFlag = TARGET_FLAG_NONE;
    packet << targetFlag << bot->GetPackGUID();

    // HandleUseItemOpcode can destroy the trinket on its last charge.
    uint32 const itemId = item->GetEntry();
    bot->GetSession()->HandleUseItemOpcode(packet);

    uint32 const now = getMSTime();
    uint32 const cooldownDelay = bot->GetSpellCooldownDelay(spellId);
    if (cooldownDelay > 0)
    {
        if (itemSpellCooldown > 0)
        {
            uint64 const itemCooldownKey = (static_cast<uint64>(itemId) << 32) | spellId;
            trinketItemCooldownExpiries[itemCooldownKey] = now + static_cast<uint32>(itemSpellCooldown);
        }

        if (itemSpellCategory && itemSpellCategoryCooldown > 0)
        {
            trinketCategoryCooldownExpiries[itemSpellCategory] = now + static_cast<uint32>(itemSpellCategoryCooldown);
        }
    }

    return true;
}

Value<Unit*>* BuffOnMainTankAction::GetTargetValue() { return context->GetValue<Unit*>("main tank", spell); }

// [heal_triage] Lifebloom / Rejuvenation / Renew / Riptide on the main tank (any spell with a periodic heal; Earth
// Shield, Beacon of Light, Thorns and the like keep their upkeep): HoR heroic Falric 2026-10-25, "lifebloom on main
// tank" (20.8) and "rejuvenation on main tank" (20.7) ran on a 96 % tank while the dps sat at 17-40 %.
bool BuffOnMainTankAction::HotUpkeepYields()
{
    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo || !spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL))
        return false;

    return HealTriage::TankUpkeepYields(botAI, GetTarget());
}

bool BuffOnMainTankAction::isUseful() { return !HotUpkeepYields() && CastBuffSpellAction::isUseful(); }

bool CastDebuffSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;

    return CastAuraSpellAction::isUseful() &&
           (target->GetHealth() / AI_VALUE(float, "estimated group dps")) >= needLifeTime;
}
