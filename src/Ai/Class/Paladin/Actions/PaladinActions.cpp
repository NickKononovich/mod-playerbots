/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PaladinActions.h"
#include "AiFactory.h"
#include "Event.h"
#include "GenericBuffUtils.h"
#include "ObjectAccessor.h"
#include "PaladinGreaterBlessingAction.h"
#include "PaladinHelper.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include <algorithm>

static bool IsBlessingTargetCandidate(Player* bot, Player* player)
{
    if (!player || !player->IsAlive() || player->GetMapId() != bot->GetMapId())
        return false;

    if (player->IsGameMaster())
        return false;

    return bot->GetDistance(player) < sPlayerbotAIConfig.SpellDistance * 2 &&
           bot->IsWithinLOS(player->GetPositionX(), player->GetPositionY(),
                            player->GetPositionZ());
}

static bool HasBlessingAura(
    PlayerbotAI* botAI, Unit* target, std::initializer_list<char const*> auraNames)
{
    for (char const* auraName : auraNames)
    {
        if (botAI->HasAura(auraName, target))
            return true;
    }

    return false;
}

static bool IsGreaterBlessingMode(Player* bot)
{
    return ai::gbless::IsEligibleGroupForAutoBlessings(bot->GetGroup());
}

template <typename Predicate>
static Unit* FindBlessingTarget(
    Player* bot, PlayerbotAI* botAI, Predicate&& predicate)
{
    std::vector<Player*> masters;
    std::vector<Player*> healers;
    std::vector<Player*> tanks;
    std::vector<Player*> others;

    Player* master = botAI->GetMaster();
    auto addPlayer = [&](Player* player)
    {
        if (!IsBlessingTargetCandidate(bot, player))
            return;

        if (player == master)
            masters.push_back(player);
        else if (PlayerbotAI::IsHeal(player))
            healers.push_back(player);
        else if (PlayerbotAI::IsTank(player))
            tanks.push_back(player);
        else
            others.push_back(player);
    };

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            addPlayer(ref->GetSource());
    }
    else
    {
        addPlayer(bot);
    }

    std::vector<std::vector<Player*>*> orderedLists = {
        &masters, &healers, &tanks, &others };
    for (std::vector<Player*>* players : orderedLists)
    {
        for (Player* player : *players)
        {
            if (predicate(player))
                return player;
        }
    }

    return nullptr;
}

static inline bool IsTankRole(Player* player)
{
    if (!player)
        return false;

    if (player->HasTankSpec())
        return true;

    if (PlayerbotAI* otherAI = GET_PLAYERBOT_AI(player))
    {
        if (otherAI->HasStrategy("prot", BOT_STATE_NON_COMBAT) ||
            otherAI->HasStrategy("prot", BOT_STATE_COMBAT)     ||
            otherAI->HasStrategy("tank", BOT_STATE_NON_COMBAT) ||
            otherAI->HasStrategy("tank", BOT_STATE_COMBAT)     ||
            otherAI->HasStrategy("tank face", BOT_STATE_NON_COMBAT) ||
            otherAI->HasStrategy("tank face", BOT_STATE_COMBAT)     ||
            otherAI->HasStrategy("bear", BOT_STATE_NON_COMBAT) ||
            otherAI->HasStrategy("bear", BOT_STATE_COMBAT))
            return true;
    }

    return false;
}

static inline bool IsOnlyPaladinInGroup(Player* bot)
{
    if (!bot)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    uint32 paladins = 0u;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* player = ref->GetSource();
        if (!player || !player->IsInWorld()) continue;
        if (player->getClass() == CLASS_PALADIN) ++paladins;
    }

    return paladins == 1u;
}

inline std::string const GetActualBlessingOfMight(Unit* target)
{
    if (!target->ToPlayer())
        return "blessing of might";

    uint8 tab = AiFactory::GetPlayerSpecTab(target->ToPlayer());
    switch (target->getClass())
    {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
            return "blessing of wisdom";
            break;
        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_ELEMENTAL || tab == SHAMAN_TAB_RESTORATION)
                return "blessing of wisdom";
            break;
        case CLASS_DRUID:
            if (tab == DRUID_TAB_RESTORATION || tab == DRUID_TAB_BALANCE)
                return "blessing of wisdom";
            break;
        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_HOLY)
                return "blessing of wisdom";
            break;
    }

    return "blessing of might";
}

inline std::string const GetActualBlessingOfWisdom(Unit* target)
{
    if (!target->ToPlayer())
        return "blessing of might";

    uint8 tab = AiFactory::GetPlayerSpecTab(target->ToPlayer());
    switch (target->getClass())
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
        case CLASS_DEATH_KNIGHT:
        case CLASS_HUNTER:
            return "blessing of might";
            break;
        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_ENHANCEMENT)
                return "blessing of might";
            break;
        case CLASS_DRUID:
            if (tab == DRUID_TAB_FERAL)
                return "blessing of might";
            break;
        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_PROTECTION || tab == PALADIN_TAB_RETRIBUTION)
                return "blessing of might";
            break;
    }

    return "blessing of wisdom";
}

inline std::string const GetActualBlessingOfSanctuary(Unit* target, Player* bot)
{
    if (!bot->HasSpell(ai::paladin::SPELL_BLESSING_OF_SANCTUARY))
        return "";

    Player* targetPlayer = target->ToPlayer();
    if (!targetPlayer)
        return "";

    if (auto* botAI = GET_PLAYERBOT_AI(bot))
    {
        if (Unit* mainTank =
            botAI->GetAiObjectContext()->GetValue<Unit*>("main tank")->Get())
        {
            if (mainTank == target)
                return "blessing of sanctuary";
        }
    }

    if (targetPlayer->HasTankSpec())
        return "blessing of sanctuary";

    return "";
}

Unit* CastBlessingOfMightOnPartyAction::GetTarget()
{
    if (IsGreaterBlessingMode(bot))
        return nullptr;

    return FindBlessingTarget(bot, botAI, [&](Player* player)
    {
        return !HasBlessingAura(botAI, player,
            { "blessing of might", "greater blessing of might",
              "blessing of wisdom", "greater blessing of wisdom",
              "blessing of sanctuary", "greater blessing of sanctuary" });
    });
}

bool CastBlessingOfMightAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    std::string castName = GetActualBlessingOfMight(target);
    return botAI->CastSpell(castName, target);
}

Value<Unit*>* CastBlessingOfMightOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>(
    "party member without aura",
    "blessing of might,greater blessing of might,blessing of wisdom,"
    "greater blessing of wisdom,blessing of sanctuary,"
    "greater blessing of sanctuary"
    );
}

bool CastBlessingOfMightOnPartyAction::Execute(Event /*event*/)
{
    if (IsGreaterBlessingMode(bot))
        return false;

    Unit* target = GetTarget();
    if (!target)
        return false;

    std::string castName = GetActualBlessingOfMight(target);
    return botAI->CastSpell(castName, target);
}

bool CastBlessingOfWisdomAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    std::string castName = GetActualBlessingOfWisdom(target);
    return botAI->CastSpell(castName, target);
}

Unit* CastBlessingOfWisdomOnPartyAction::GetTarget()
{
    if (IsGreaterBlessingMode(bot))
        return nullptr;

    return FindBlessingTarget(bot, botAI, [&](Player* player)
    {
        if (botAI->HasStrategy("bwisdom", BOT_STATE_NON_COMBAT) && IsTankRole(player))
            return false;

        return !HasBlessingAura(botAI, player,
            { "blessing of might", "greater blessing of might",
              "blessing of wisdom", "greater blessing of wisdom",
              "blessing of sanctuary", "greater blessing of sanctuary" });
    });
}

Value<Unit*>* CastBlessingOfWisdomOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>(
    "party member without aura",
    "blessing of wisdom,greater blessing of wisdom,blessing of might,greater blessing of might,"
    "blessing of sanctuary,greater blessing of sanctuary"
    );
}

bool CastBlessingOfWisdomOnPartyAction::Execute(Event /*event*/)
{
    if (IsGreaterBlessingMode(bot))
        return false;

    Unit* target = GetTarget();
    if (!target)
        return false;

    Player* targetPlayer = target->ToPlayer();

    if (Group* group = bot->GetGroup())
        if (targetPlayer && !group->IsMember(targetPlayer->GetGUID()))
            return false;

    if (botAI->HasStrategy("bwisdom", BOT_STATE_NON_COMBAT) &&
        targetPlayer && IsTankRole(targetPlayer))
        return false;

    std::string castName = GetActualBlessingOfWisdom(target);
    if (castName.empty())
        return false;

    return botAI->CastSpell(castName, target);
}

Value<Unit*>* CastBlessingOfSanctuaryOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>(
        "party member without aura",
        "blessing of sanctuary,greater blessing of sanctuary"
    );
}

bool CastBlessingOfSanctuaryOnPartyAction::Execute(Event /*event*/)
{
    if (IsGreaterBlessingMode(bot))
        return false;

    if (!bot->HasSpell(ai::paladin::SPELL_BLESSING_OF_SANCTUARY))
        return false;

    Unit* target = GetTarget();
    if (!target)
        target = bot;

    Player* targetPlayer = target ? target->ToPlayer() : nullptr;

    const auto HasSanctAura = [&](Unit* unit) -> bool {
        return botAI->HasAura("blessing of sanctuary", unit) ||
               botAI->HasAura("greater blessing of sanctuary", unit);
    };

    if (Group* group = bot->GetGroup())
    {
        if (targetPlayer && !group->IsMember(targetPlayer->GetGUID()))
        {
            target = bot;
            targetPlayer = bot->ToPlayer();
        }
    }

    if (Player* self = bot->ToPlayer())
    {
        bool selfHasSanct = HasSanctAura(self);
        bool needSelf = IsTankRole(self) && !selfHasSanct;

        if (needSelf)
        {
            target = self;
            targetPlayer = self;
        }
    }

    bool targetOk = false;
    if (targetPlayer)
    {
        bool hasSanct = HasSanctAura(targetPlayer);
        targetOk = IsTankRole(targetPlayer) && !hasSanct;
    }

    if (!targetOk)
    {
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* player = ref->GetSource();
                if (!player) continue;
                if (!player->IsInWorld() || !player->IsAlive()) continue;
                if (!IsTankRole(player)) continue;

                bool hasSanct = HasSanctAura(player);
                if (!hasSanct)
                {
                    target = player;
                    targetPlayer = player;
                    targetOk = true;
                    break;
                }
            }
        }
    }

    if (GetActualBlessingOfSanctuary(target, bot).empty())
    {
        if (targetPlayer)
        {
            if (IsTankRole(targetPlayer))
                return botAI->CastSpell("blessing of sanctuary", target);
            else
                return false;
        }
        else
            return false;
    }

    return botAI->CastSpell("blessing of sanctuary", target);
}

Unit* CastBlessingOfSanctuaryOnPartyAction::GetTarget()
{
    if (IsGreaterBlessingMode(bot))
        return nullptr;

    if (!bot->HasSpell(ai::paladin::SPELL_BLESSING_OF_SANCTUARY))
        return nullptr;

    return FindBlessingTarget(bot, botAI, [&](Player* player)
    {
        return IsTankRole(player) &&
               !HasBlessingAura(botAI, player,
                   { "blessing of sanctuary", "greater blessing of sanctuary" });
    });
}

Value<Unit*>* CastBlessingOfKingsOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>(
        "party member without aura",
        "blessing of kings,greater blessing of kings,"
        "blessing of sanctuary,greater blessing of sanctuary"
    );
}

Unit* CastBlessingOfKingsOnPartyAction::GetTarget()
{
    if (IsGreaterBlessingMode(bot))
        return nullptr;

    const bool hasBwisdom = botAI->HasStrategy("bwisdom", BOT_STATE_NON_COMBAT);
    const bool hasBkings = botAI->HasStrategy("bkings", BOT_STATE_NON_COMBAT);
    const bool onlyPaladinInGroup = IsOnlyPaladinInGroup(bot);

    return FindBlessingTarget(bot, botAI, [&](Player* player)
    {
        const bool isTank = IsTankRole(player);
        const bool hasKingsOrSanct = HasBlessingAura(botAI, player,
            { "blessing of kings", "greater blessing of kings",
              "blessing of sanctuary", "greater blessing of sanctuary" });

        if (hasKingsOrSanct)
            return false;

        if (hasBwisdom)
            return isTank;

        if (hasBkings)
        {
            if (isTank)
                return false;

            if (onlyPaladinInGroup && player == bot)
                return false;
        }

        return true;
    });
}

bool CastBlessingOfKingsOnPartyAction::Execute(Event /*event*/)
{
    if (IsGreaterBlessingMode(bot))
        return false;

    Unit* target = GetTarget();
    if (!target)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    if (botAI->HasStrategy("bkings", BOT_STATE_NON_COMBAT) &&
        IsOnlyPaladinInGroup(bot))
    {
        if (target->GetGUID() == bot->GetGUID())
            return false;
    }

    Player* targetPlayer = target->ToPlayer();
    if (targetPlayer && !group->IsMember(targetPlayer->GetGUID()))
        return false;

    const bool hasBwisdom = botAI->HasStrategy("bwisdom", BOT_STATE_NON_COMBAT);
    const bool hasBkings = botAI->HasStrategy("bkings", BOT_STATE_NON_COMBAT);

    if (hasBwisdom && (!targetPlayer || !IsTankRole(targetPlayer)))
        return false;

    if (targetPlayer)
    {
        const bool isTank = IsTankRole(targetPlayer);
        const bool hasSanctFromMe =
            target->HasAura(ai::paladin::SPELL_BLESSING_OF_SANCTUARY, bot->GetGUID()) ||
            target->HasAura(ai::paladin::SPELL_GREATER_BLESSING_OF_SANCTUARY, bot->GetGUID());
        const bool hasSanctAny =
            botAI->HasAura("blessing of sanctuary", target) ||
            botAI->HasAura("greater blessing of sanctuary", target);

        if (isTank && hasSanctFromMe)
            return false;

        if (hasBkings && isTank && hasSanctAny)
            return false;
    }

    return botAI->CastSpell("blessing of kings", target);
}

bool CastSealSpellAction::isUseful()
{
    return AI_VALUE2(bool, "combat", "self target");
}

Value<Unit*>* CastTurnUndeadAction::GetTargetValue()
{
    return context->GetValue<Unit*>("cc target", getName());
}

Unit* CastHandOfFreedomOnPartyAction::GetTarget()
{
    bool const selfImpaired = ai::paladin::HasFreedomRemovableImpairment(bot);  // [b33_class] root/snare only
    bool const hasSelfHand =
        selfImpaired && ai::paladin::HasAnyPaladinHandFromCaster(bot, bot);

    if (!bot->GetGroup())
    {
        if (selfImpaired && !hasSelfHand)
            return bot;

        return nullptr;
    }

    if (selfImpaired && !hasSelfHand)
        return bot;

    return CastBuffSpellAction::GetTarget();
}

Value<Unit*>* CastHandOfFreedomOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>("party member snared target");
}

bool CastHandOfFreedomOnPartyAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    return CastBuffSpellAction::isUseful() &&
           !ai::paladin::HasAnyPaladinHandFromCaster(target, bot);
}

Unit* CastRighteousDefenseAction::GetTarget()
{
    Unit* current_target = AI_VALUE(Unit*, "current target");
    if (!current_target)
        return nullptr;

    return current_target->GetVictim();
}

bool CastDivineSacrificeAction::isUseful()
{
    return GetTarget() && (GetTarget() != nullptr) && CastSpellAction::isUseful() &&
           !botAI->HasAura("divine guardian", GetTarget(), false, false, -1, true);
}

bool CastCancelDivineSacrificeAction::Execute(Event /*event*/)
{
    botAI->RemoveAura("divine sacrifice");
    return true;
}

bool CastCancelDivineSacrificeAction::isUseful()
{
    return botAI->HasAura("divine sacrifice", GetTarget(), false, true, -1, true);
}

bool CastLayOnHandsOnPartyAction::isUseful()
{
    Unit* target = GetTarget();
    return target && target != bot && HealPartyMemberAction::isUseful();
}

bool CastDivineShieldAction::isUseful() { return !PlayerbotAI::IsTank(bot) && CastBuffSpellAction::isUseful(); }

bool CastHandOfSacrificeOnSpikingTankAction::isUseful()
{
    Unit* target = GetTarget();
    return target && target != bot && !PlayerbotAI::IsTank(bot) &&
           bot->GetHealthPct() > sPlayerbotAIConfig.MediumHealth && CastSpikeProtectSpellAction::isUseful();
}

bool DropStaleSealAction::Execute(Event /*event*/)
{
    // [rot_b31_pdd] PlayerbotAI::RemoveAura removes a seal only when the bot has it
    for (char const* seal : {"seal of righteousness", "seal of justice", "seal of light", "seal of wisdom",
                             "seal of command", "seal of vengeance", "seal of corruption"})
        botAI->RemoveAura(seal);

    LOG_INFO("playerbots", "[rot_b31_pdd] {} dropped a seal without AURA_STATE_JUDGEMENT", bot->GetName());
    return true;
}

// [b31_deep]
bool ai::paladin::HasHolyWrathTargetWithin(PlayerbotAI* botAI, Player* bot, float range)
{
    auto const isHolyWrathType = [](Unit* unit)
    {
        if (!unit || !unit->IsAlive())
            return false;

        uint32 const creatureType = unit->GetCreatureType();
        return creatureType == CREATURE_TYPE_UNDEAD || creatureType == CREATURE_TYPE_DEMON;
    };

    AiObjectContext* context = botAI->GetAiObjectContext();
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (isHolyWrathType(target) && bot->GetDistance(target) <= range)
        return true;

    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit != target && isHolyWrathType(unit) && bot->GetDistance(unit) <= range)
            return true;
    }

    return false;
}

// [b31_deep]
bool CastHolyWrathAction::isUseful()
{
    return CastMeleeSpellAction::isUseful() && ai::paladin::HasHolyWrathTargetWithin(botAI, bot, HOLY_WRATH_RADIUS);
}

// [b31_deep]
bool CastHolyWrathOnGroupMemberAttackersAction::isUseful()
{
    return CastAreaSpellOnGroupMemberAttackersAction::isUseful() &&
           ai::paladin::HasHolyWrathTargetWithin(botAI, bot, CastHolyWrathAction::HOLY_WRATH_RADIUS);
}

// [b34_pal]
static bool IsHolyWrathCreatureType(Unit* unit)
{
    if (!unit || !unit->IsAlive())
        return false;

    uint32 const creatureType = unit->GetCreatureType();
    return creatureType == CREATURE_TYPE_UNDEAD || creatureType == CREATURE_TYPE_DEMON;
}

// [b34_pal]
uint32 ai::paladin::CountHolyWrathTargetsWithin(PlayerbotAI* botAI, Player* bot, float range)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    uint32 count = IsHolyWrathCreatureType(target) && bot->GetDistance(target) <= range ? 1 : 0;

    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit != target && IsHolyWrathCreatureType(unit) && bot->GetDistance(unit) <= range)
            ++count;
    }

    return count;
}

// [b34_pal] Holy Wrath damages undead and demons only: only those can lose a crowd control to it or be pulled by it.
bool ai::paladin::HolyWrathWouldBreakCcOrPull(PlayerbotAI* botAI, Player* bot)
{
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!IsHolyWrathCreatureType(unit) || bot->GetDistance(unit) > CastHolyWrathAction::HOLY_WRATH_RADIUS)
            continue;

        if (unit->HasBreakableByDamageCrowdControlAura() || (unit->IsCreature() && !unit->IsInCombat()))
            return true;
    }

    return false;
}

// [b34_pal]
bool CastHolyWrathUndeadPackAction::isUseful()
{
    return CastSpellAction::isUseful() &&
           ai::paladin::CountHolyWrathTargetsWithin(botAI, bot, ai::paladin::HOLY_WRATH_PACK_RANGE) >=
               ai::paladin::HOLY_WRATH_PACK_MIN &&
           !ai::paladin::HolyWrathWouldBreakCcOrPull(botAI, bot);
}

// [b31_deep]
bool CastRetConsecrationAction::isUseful()
{
    if (!CastConsecrationAction::isUseful())
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsAlive())
        return false;

    float const groupDps = AI_VALUE(float, "estimated group dps");
    return groupDps <= 0.0f || target->GetHealth() / groupDps >= MIN_TARGET_LIFE_SECS;
}

// [b31_pal]
bool CastAvengingWrathAction::isUseful()
{
    constexpr uint32 FORBEARANCE = 25771;
    constexpr uint32 AVENGING_WRATH_MARKER = 61987;
    return !bot->HasAura(FORBEARANCE) && !bot->HasAura(AVENGING_WRATH_MARKER) && CastBuffSpellAction::isUseful();
}

bool CastDivinePleaAction::isUseful()
{
    // [b31_deep] trained at 71 (Spell.dbc spellLevel 71): the tank's "high mana" queued it at 60-70 and it failed on
    // "spell not known" (build 30 L60s: 2504 refusals); the cached spell id is 0 then
    if (!AI_VALUE2(uint32, "spell id", "divine plea"))
        return false;

    if (!CastBuffSpellAction::isUseful())
        return false;

    if (!PlayerbotAI::IsHeal(bot))
        return true;

    // Divine Plea halves healing done, so a healer holds it while someone needs big heals or the group takes
    // area damage; between pulls nothing holds it back.
    if (!bot->IsInCombat())
        return true;

    Unit* target = AI_VALUE(Unit*, "party member to heal");
    if (target && target->GetHealthPct() < sPlayerbotAIConfig.MediumHealth)
        return false;

    return AI_VALUE2(uint8, "aoe heal", "medium") < 3;
}

// [pal-bless-role] Sunken Temple 2026-10-25 (st-n-5A-92, section E #6): the group's only paladin, a protection tank
// with Kings / Might / Wisdom, never blessed the other four in the whole run (they kept "missing: Blessing of Kings"
// through two 15 s buffing waits). Its non-combat strategy was "bsanc", which blesses tanks only, and the greater
// blessing assignments are for raids (Playerbots.AutoGreaterBlessings 1). Out of combat every paladin gives each
// member that has no blessing of its own the best one by role that nobody gave it yet: Kings first, then Might for
// a physical damage dealer, Wisdom for a mana user, Sanctuary for a tank; the greater version (a 30 min buff for the
// whole class) when it knows it, has the reagent, and every member of that class wants the same blessing.
//
// [pal-bless-split] Two paladins in one group (a protection tank and a holy healer): a blessing of one type from
// another paladin replaces the first, different types stack, and one paladin keeps one blessing per member. Both
// picking "the best one nobody gave yet" on the same tick gave the member Kings twice (the second overwrote the
// first), and the legacy buff strategies (bwisdom / bsanc / bmight) blessed over the split. With two or more bot
// paladins every one of them computes the same split: a type already on the member stays with its caster; the free
// bot paladins, sorted by GUID, take the remaining types in the member's order of need, each type going to the
// paladin that knows it and knows the fewest of the types still to give (Sanctuary to a tank paladin first), ties
// to the lower GUID. A paladin casts only the type the split gives it, never one another paladin holds or was
// given; the legacy party blessings stand aside. With one paladin nothing changes.
namespace
{
    constexpr float ROLE_BLESSING_RANGE = 30.0f;

    struct BlessingKind
    {
        char const* lesser;
        char const* greater;
        char const* label;
    };

    constexpr BlessingKind BLESSING_KINGS = {"blessing of kings", "greater blessing of kings", "Kings"};
    constexpr BlessingKind BLESSING_MIGHT = {"blessing of might", "greater blessing of might", "Might"};
    constexpr BlessingKind BLESSING_WISDOM = {"blessing of wisdom", "greater blessing of wisdom", "Wisdom"};
    constexpr BlessingKind BLESSING_SANCTUARY = {"blessing of sanctuary", "greater blessing of sanctuary",
                                                 "Sanctuary"};

    bool SameKind(BlessingKind const& a, BlessingKind const& b)
    {
        return a.lesser && b.lesser && std::string(a.lesser) == std::string(b.lesser);
    }

    bool HasBlessingFrom(PlayerbotAI* botAI, Unit* target, bool mine)
    {
        for (BlessingKind const& kind : {BLESSING_KINGS, BLESSING_MIGHT, BLESSING_WISDOM, BLESSING_SANCTUARY})
            if (botAI->HasAura(kind.lesser, target, false, mine) || botAI->HasAura(kind.greater, target, false, mine))
                return true;
        return false;
    }

    // The blessings a member wants, best first.
    std::vector<BlessingKind> WantedBlessings(Player* member)
    {
        if (IsTankRole(member))
            return {BLESSING_KINGS, BLESSING_SANCTUARY, BLESSING_MIGHT};

        if (GetActualBlessingOfWisdom(member) == "blessing of wisdom")
            return {BLESSING_KINGS, BLESSING_WISDOM};

        return {BLESSING_KINGS, BLESSING_MIGHT, BLESSING_WISDOM};
    }

    // The first wanted blessing this paladin knows that nobody gave the member yet (lesser nullptr when none).
    BlessingKind PickBlessing(PlayerbotAI* botAI, Player* member)
    {
        for (BlessingKind const& kind : WantedBlessings(member))
        {
            if (!botAI->HasSpell(kind.lesser))
                continue;
            if (botAI->HasAura(kind.lesser, member) || botAI->HasAura(kind.greater, member))
                continue;
            return kind;
        }
        return {nullptr, nullptr, nullptr};
    }

    // The bot paladins of the group that bless by the split (alive, on the bot's map and instance), sorted by GUID.
    std::vector<Player*> SplitPaladins(Player* bot)
    {
        std::vector<Player*> paladins;
        Group* group = bot->GetGroup();
        if (!group)
            return paladins;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->IsAlive() && member->getClass() == CLASS_PALADIN &&
                member->GetMapId() == bot->GetMapId() && member->GetInstanceId() == bot->GetInstanceId() &&
                GET_PLAYERBOT_AI(member))
                paladins.push_back(member);
        }

        std::sort(paladins.begin(), paladins.end(), [](Player const* a, Player const* b)
                  { return a->GetGUID().GetRawValue() < b->GetGUID().GetRawValue(); });
        return paladins;
    }

    bool PaladinKnows(Player* paladin, BlessingKind const& kind)
    {
        PlayerbotAI* paladinAI = GET_PLAYERBOT_AI(paladin);
        return paladinAI && paladinAI->HasSpell(kind.lesser);
    }

    // One member's split ([pal-bless-split]): the type the split gives `self` (lesser nullptr: none), and each
    // type the member wants with who holds it (the caster of the aura on it) or whom the split gives it.
    struct BlessingSplit
    {
        BlessingKind mine = {nullptr, nullptr, nullptr};
        std::string describe;
    };

    BlessingSplit SplitFor(PlayerbotAI* botAI, Player* self, Player* member, std::vector<Player*> const& paladins)
    {
        BlessingSplit split;
        // The wanted types, and as a last one Sanctuary for a non-tank (3% less damage taken): a third paladin has
        // something to give too.
        std::vector<BlessingKind> wanted = WantedBlessings(member);
        if (std::none_of(wanted.begin(), wanted.end(),
                         [](BlessingKind const& kind) { return SameKind(kind, BLESSING_SANCTUARY); }))
            wanted.push_back(BLESSING_SANCTUARY);

        // Held: a blessing on the member stays with its caster (a bot or not); that paladin is done with the member.
        std::vector<ObjectGuid> holders(wanted.size());
        std::vector<ObjectGuid> done;
        for (BlessingKind const& kind : {BLESSING_KINGS, BLESSING_MIGHT, BLESSING_WISDOM, BLESSING_SANCTUARY})
        {
            Aura* aura = botAI->GetAura(kind.lesser, member);
            if (!aura)
                aura = botAI->GetAura(kind.greater, member);
            if (!aura)
                continue;

            done.push_back(aura->GetCasterGUID());
            for (size_t i = 0; i < wanted.size(); ++i)
                if (SameKind(wanted[i], kind))
                    holders[i] = aura->GetCasterGUID();
        }

        std::vector<Player*> available;
        for (Player* paladin : paladins)
            if (std::find(done.begin(), done.end(), paladin->GetGUID()) == done.end())
                available.push_back(paladin);

        for (size_t i = 0; i < wanted.size(); ++i)
        {
            if (!holders[i].IsEmpty())
                continue;

            Player* chosen = nullptr;
            int chosenOthers = 0;
            bool chosenTank = false;
            for (Player* paladin : available)
            {
                if (!PaladinKnows(paladin, wanted[i]))
                    continue;

                int others = 0;  // the types still to give after this one that the paladin knows
                for (size_t j = i + 1; j < wanted.size(); ++j)
                    if (holders[j].IsEmpty() && PaladinKnows(paladin, wanted[j]))
                        ++others;

                bool const tank = SameKind(wanted[i], BLESSING_SANCTUARY) && IsTankRole(paladin);
                if (!chosen || (tank && !chosenTank) || (tank == chosenTank && others < chosenOthers))
                {
                    chosen = paladin;
                    chosenOthers = others;
                    chosenTank = tank;
                }
            }

            if (!chosen)
                continue;

            holders[i] = chosen->GetGUID();
            available.erase(std::find(available.begin(), available.end(), chosen));
            if (chosen == self)
                split.mine = wanted[i];
        }

        for (size_t i = 0; i < wanted.size(); ++i)
        {
            if (holders[i].IsEmpty())
                continue;

            Player* holder = ObjectAccessor::FindConnectedPlayer(holders[i]);
            if (!split.describe.empty())
                split.describe += ", ";
            split.describe += std::string(wanted[i].label) + "=" + (holder ? holder->GetName() : std::string("?"));
        }

        return split;
    }
}

namespace ai::paladin
{
bool RoleBlessingSplit(Player* bot)
{
    return bot && bot->GetGroup() && !IsGreaterBlessingMode(bot) && SplitPaladins(bot).size() >= 2;
}

RoleBlessing FindRoleBlessing(PlayerbotAI* botAI)
{
    RoleBlessing found;
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsAlive() || bot->IsInCombat() || IsGreaterBlessingMode(bot))
        return found;

    std::vector<Player*> members;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->IsAlive() && !member->IsGameMaster() &&
                member->GetMapId() == bot->GetMapId() && member->GetInstanceId() == bot->GetInstanceId())
                members.push_back(member);
        }
    }
    else
        members.push_back(bot);

    // [pal-bless-split] Two or more bot paladins: each member's type comes from the split; with one, the first
    // wanted type nobody gave (PickBlessing, as before).
    std::vector<Player*> const paladins = SplitPaladins(bot);
    bool const split = paladins.size() >= 2 && std::find(paladins.begin(), paladins.end(), bot) != paladins.end();
    auto pick = [&](Player* member, std::string* describe) -> BlessingKind
    {
        if (!split)
            return PickBlessing(botAI, member);

        BlessingSplit const memberSplit = SplitFor(botAI, bot, member, paladins);
        if (describe)
            *describe = memberSplit.describe;
        return memberSplit.mine;
    };

    for (Player* member : members)
    {
        if (member->IsInCombat() || bot->GetExactDist(member) > ROLE_BLESSING_RANGE ||
            !bot->IsWithinLOSInMap(member) || HasBlessingFrom(botAI, member, true))
            continue;

        BlessingKind const kind = pick(member, &found.split);
        if (!kind.lesser)
            continue;

        found.target = member;
        found.spell = kind.lesser;

        // The greater version blesses every member of the class: only when they all want this one and none of them
        // has it from another paladin.
        uint32 const greaterId =
            botAI->HasSpell(kind.greater)
                ? botAI->GetAiObjectContext()->GetValue<uint32>("spell id", kind.greater)->Get()
                : 0;
        if (greaterId && ai::buff::HasRequiredReagents(bot, greaterId))
        {
            bool sameForClass = true;
            for (Player* other : members)
            {
                if (other == member || other->getClass() != member->getClass())
                    continue;
                BlessingKind const wants = pick(other, nullptr);
                if (HasBlessingFrom(botAI, other, true) || !SameKind(wants, kind))
                {
                    sameForClass = false;
                    break;
                }
            }
            if (sameForClass)
                found.spell = kind.greater;
        }
        return found;
    }

    found.split.clear();
    return found;
}
}

bool CastBlessingOnPartyByRoleAction::isUseful() { return ai::paladin::FindRoleBlessing(botAI).target != nullptr; }

bool CastBlessingOnPartyByRoleAction::Execute(Event /*event*/)
{
    ai::paladin::RoleBlessing const blessing = ai::paladin::FindRoleBlessing(botAI);
    if (!blessing.target)
        return false;

    if (!botAI->CastSpell(blessing.spell, blessing.target))
        return false;

    char const* role = IsTankRole(blessing.target)                                           ? "tank"
                       : GetActualBlessingOfWisdom(blessing.target) == "blessing of wisdom" ? "mana user"
                                                                                             : "physical";
    if (!blessing.split.empty())
        LOG_INFO("playerbots", "[pal-bless-split] {} gives {} to {} ({}); split: {}", bot->GetName(), blessing.spell,
                 blessing.target->GetName(), role, blessing.split);
    else
        LOG_INFO("playerbots", "[pal-bless-role] {} gives {} to {} ({})", bot->GetName(), blessing.spell,
                 blessing.target->GetName(), role);
    return true;
}

// [pal-bless-split] The legacy party blessings (bwisdom / bmight / bsanc / bkings) pick by their own rules and would
// bless over the split of two paladins: they stand aside while it is on.
bool CastBlessingOfMightOnPartyAction::isUseful()
{
    return !ai::paladin::RoleBlessingSplit(bot) && BuffOnPartyAction::isUseful();
}

bool CastBlessingOfWisdomOnPartyAction::isUseful()
{
    return !ai::paladin::RoleBlessingSplit(bot) && BuffOnPartyAction::isUseful();
}

bool CastBlessingOfKingsOnPartyAction::isUseful()
{
    return !ai::paladin::RoleBlessingSplit(bot) && BuffOnPartyAction::isUseful();
}

bool CastBlessingOfSanctuaryOnPartyAction::isUseful()
{
    return !ai::paladin::RoleBlessingSplit(bot) && BuffOnPartyAction::isUseful();
}

// [b31_holyshield_pull] A paladin tank out of combat, at 30 % mana or more, that knows Holy Shield and has none up.
bool CastHolyShieldPrePullAction::isUseful()
{
    if (bot->IsInCombat() || !PlayerbotAI::IsTank(bot) || bot->GetPowerPct(POWER_MANA) < MIN_MANA_PCT)
        return false;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && bot->HasSpell(spellId) && CastBuffSpellAction::isUseful();
}

bool CastHolyShieldPrePullAction::Execute(Event event)
{
    if (!CastBuffSpellAction::Execute(event))
        return false;

    LOG_INFO("playerbots", "[holy shield pre-pull] {}: Holy Shield before the pull ({:.0f} % mana)", bot->GetName(),
             bot->GetPowerPct(POWER_MANA));
    return true;
}
