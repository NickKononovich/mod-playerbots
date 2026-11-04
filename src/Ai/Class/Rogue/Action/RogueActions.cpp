/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RogueActions.h"

#include <algorithm>
#include <string>

#include "AiFactory.h"
#include "DBCStores.h"
#include "Event.h"
#include "Group.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RoguePoisons.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

namespace
{
constexpr uint32 BG_WS_SPELL_WARSONG_FLAG = 23333;
constexpr uint32 BG_WS_SPELL_SILVERWING_FLAG = 23335;
constexpr uint32 BG_EY_NETHERSTORM_FLAG_SPELL = 34976;
}

bool CastStealthAction::isUseful()
{
    // User 2026-10-24, RFC live: «стелс ок перед пуллом (сап, оглушить, открыться), но зачем стелс, когда танк уже
    // сагрил моба?». In a dungeon group stealth is for before the fight only: once the target or anyone of the group
    // is fighting, re-stealthing on "enemy out of melee" just halves the rogue's speed and keeps it out of the fight.
    Unit* target = AI_VALUE(Unit*, "current target");
    if (bot->GetMap() && bot->GetMap()->IsDungeon() && bot->GetGroup())
    {
        if (bot->IsInCombat() || (target && target->IsInCombat()))
            return false;
        for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != bot && member->IsAlive() && member->IsInMap(bot) && member->IsInCombat())
                    return false;
    }

    if (target && bot->GetDistance(target) >= sPlayerbotAIConfig.SpellDistance)
        return false;
    return true;
}

bool CastStealthAction::isPossible()
{
    // do not use with WSG flag or EYE flag
    return !bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) && !bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) &&
           !bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL);
}

// [b31_wr] Was always possible and returned true without stealth: the out-of-combat "often" trigger spent 16-25 % of
// the ticks on it.
bool UnstealthAction::isPossible() { return bot->HasAuraType(SPELL_AURA_MOD_STEALTH); }

bool UnstealthAction::Execute(Event /*event*/)
{
    if (!botAI->HasAura("stealth", bot))
        return false;

    botAI->RemoveAura("stealth");
    // botAI->ChangeStrategy("+combat,-stealthed", BOT_STATE_COMBAT);

    return true;
}

bool CheckStealthAction::Execute(Event /*event*/)
{
    if (botAI->HasAura("stealth", bot))
    {
        botAI->ChangeStrategy("-combat,+stealthed", BOT_STATE_COMBAT);
    }
    else
    {
        botAI->ChangeStrategy("+combat,-stealthed", BOT_STATE_COMBAT);
    }

    return true;
}

bool CastVanishAction::isUseful()
{
    // do not use with WSG flag or EYE flag
    return !bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) && !bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) &&
           !bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL);
}

// [rot49] b184 5E-19 Taragaman: no Envenom in a 68 s boss fight (the sim casts 6.2 a minute), the points went to
// Slice and Dice. One line per change of the inputs of every finisher the 4-point state can take, on a boss only.
void CastEnvenomAction::TraceFinisher()
{
    constexpr uint8 TRACE_MIN_COMBO_POINTS = 4;
    Unit* target = GetTarget();
    Creature* boss = target ? target->ToCreature() : nullptr;
    if (!boss || !boss->IsDungeonBoss())
        return;

    uint8 const combo = AI_VALUE2(uint8, "combo", "current target");
    if (combo < TRACE_MIN_COMBO_POINTS)
    {
        lastTrace.clear();
        return;
    }

    Aura const* snd = botAI->GetAura("slice and dice", bot);
    Aura const* envenom = botAI->GetAura("envenom", bot);
    uint32 const vanish = AI_VALUE2(uint32, "spell id", "vanish");
    uint32 const coldBlood = AI_VALUE2(uint32, "spell id", "cold blood");
    std::string const trace = Acore::StringFormat(
        "cp {} energy {} melee {} deadly poison state {} envenom buff {} ms, spell useful {} possible {}, slice and "
        "dice {} ms, vanish ready {}, cold blood ready {}, expose armor on target {}, bleeding {}",
        combo, AI_VALUE2(uint8, "energy", "self target"), bot->IsWithinMeleeRange(target),
        target->HasAuraState(AURA_STATE_DEADLY_POISON, nullptr, bot), envenom ? envenom->GetDuration() : 0,
        CastSpellAction::isUseful(), CastMeleeSpellAction::isPossible(), snd ? snd->GetDuration() : 0,
        vanish && !bot->HasSpellCooldown(vanish), coldBlood && !bot->HasSpellCooldown(coldBlood),
        botAI->HasAura("expose armor", target), target->HasAuraState(AURA_STATE_BLEEDING));
    if (trace == lastTrace)
        return;

    lastTrace = trace;
    LOG_INFO("playerbots", "[rot49] {} envenom check on {}: {}", bot->GetName(), boss->GetName(), trace);
}

bool CastEnvenomAction::isUseful()
{
    TraceFinisher();
    // [rot27] wowsims rogue__mutilate: Envenom (at 4+ points) only while its buff is down or at 85+ energy
    constexpr uint8 ENVENOM_POOL_ENERGY = 85;
    uint8 const energy = AI_VALUE2(uint8, "energy", "self target");
    return CastMeleeSpellAction::isUseful() && energy >= 35 &&
           (energy >= ENVENOM_POOL_ENERGY || !botAI->HasAura("envenom", bot));
}

// [b31_wr] Was "has Master Poisoner rank 3" alone, in place of CastSpellAction::isPossible (no CanCast, cooldown or
// known-spell check), and Envenom without a Deadly Poison dose on the target does next to no damage.
bool CastEnvenomAction::isPossible()
{
    Unit* target = GetTarget();
    if (!target || !target->HasAuraState(AURA_STATE_DEADLY_POISON, nullptr, bot))
        return false;

    return CastMeleeSpellAction::isPossible();
}

// [b31_wr]
bool CastHungerForBloodAction::isUseful()
{
    if (!CastSpellAction::isUseful())
        return false;

    Aura* aura = botAI->GetAura(spell, bot);
    return !aura || aura->GetDuration() < static_cast<int32>(HUNGER_FOR_BLOOD_REFRESH_MS);
}

bool CastTricksOfTheTradeOnMainTankAction::isUseful()
{
    return CastSpellAction::isUseful() && AI_VALUE2(float, "distance", GetTargetName()) < 20.0f;
}

// [b31_wr]
bool UseRoguePoisonAction::Execute(Event /*event*/)
{
    Item* const weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    Item* const poison = weapon ? RoguePoisons::ItemFor(botAI, slot) : nullptr;
    if (!poison)
        return false;

    return UseItem(poison, ObjectGuid::Empty, weapon);
}

// [b31_wr] RoguePoisons (RoguePoisons.h)
namespace RoguePoisons
{
    namespace
    {
        bool IsDeadlyPoisonEnchant(uint32 enchantId)
        {
            SpellItemEnchantmentEntry const* entry =
                enchantId ? sSpellItemEnchantmentStore.LookupEntry(enchantId) : nullptr;
            if (!entry)
                return false;

            SpellInfo const* procSpell = sSpellMgr->GetSpellInfo(entry->spellid[0]);
            if (!procSpell || procSpell->SpellFamilyName != SPELLFAMILY_ROGUE)
                return false;

            std::string const name = procSpell->SpellName[0];
            return name.rfind("Deadly Poison", 0) == 0;
        }

        Item* FindBest(PlayerbotAI* botAI, Kind kind)
        {
            for (uint32 itemId : ItemIds(kind))
                if (Item* item = botAI->FindConsumable(itemId))
                    return item;

            return nullptr;
        }
    }

    std::vector<uint32> const& ItemIds(Kind kind)
    {
        static std::vector<uint32> const instant = {
            INSTANT_POISON_IX, INSTANT_POISON_VIII, INSTANT_POISON_VII, INSTANT_POISON_VI, INSTANT_POISON_V,
            INSTANT_POISON_IV, INSTANT_POISON_III, INSTANT_POISON_II, INSTANT_POISON };
        static std::vector<uint32> const deadly = {
            DEADLY_POISON_IX, DEADLY_POISON_VIII, DEADLY_POISON_VII, DEADLY_POISON_VI, DEADLY_POISON_V,
            DEADLY_POISON_IV, DEADLY_POISON_III, DEADLY_POISON_II, DEADLY_POISON };
        // Wound Poison VII .. I (item_template RequiredLevel 78, 72, 64, 56, 48, 40, 32)
        static std::vector<uint32> const wound = { 43235, 43234, 22055, 10922, 10921, 10920, 10918 };

        switch (kind)
        {
            case Kind::DEADLY:
                return deadly;
            case Kind::WOUND:
                return wound;
            default:
                return instant;
        }
    }

    bool HasDeadlyOnWeapon(Player* player)
    {
        for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND })
        {
            Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (weapon && IsDeadlyPoisonEnchant(weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)))
                return true;
        }

        return false;
    }

    bool HasDeadly(Item const* weapon)
    {
        return weapon && IsDeadlyPoisonEnchant(weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT));
    }

    bool IsDeadlyItem(Item const* item)
    {
        std::vector<uint32> const& deadly = ItemIds(Kind::DEADLY);
        return item && std::find(deadly.begin(), deadly.end(), item->GetEntry()) != deadly.end();
    }

    Kind KindFor(PlayerbotAI* botAI, uint8 slot)
    {
        Player* bot = botAI->GetBot();
        uint8 const deadlySlot = AiFactory::GetPlayerSpecTab(bot) == ROGUE_TAB_ASSASSINATION
                                     ? uint8(EQUIPMENT_SLOT_MAINHAND)
                                     : uint8(EQUIPMENT_SLOT_OFFHAND);
        if (slot != deadlySlot)
            return Kind::INSTANT;

        return Kind::DEADLY;
    }

    Item* ItemFor(PlayerbotAI* botAI, uint8 slot)
    {
        switch (KindFor(botAI, slot))
        {
            case Kind::DEADLY:
                if (Item* item = FindBest(botAI, Kind::DEADLY))
                    return item;
                return FindBest(botAI, Kind::INSTANT);
            case Kind::WOUND:
                if (Item* item = FindBest(botAI, Kind::WOUND))
                    return item;
                return FindBest(botAI, Kind::INSTANT);
            default:
                if (Item* item = FindBest(botAI, Kind::INSTANT))
                    return item;
                return FindBest(botAI, Kind::DEADLY);
        }
    }
}

// [rot432]
bool CastKidneyShotAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !CastMeleeSpellAction::isUseful())
        return false;

    Creature const* creature = target->ToCreature();
    if (creature && (creature->IsDungeonBoss() || creature->isWorldBoss()))
        return false;

    Player* victim = target->GetVictim() ? target->GetVictim()->ToPlayer() : nullptr;
    return victim && victim != bot && victim->IsInSameGroupWith(bot) && !PlayerbotAI::IsTank(victim);
}

bool CastKidneyShotAction::Execute(Event event)
{
    Unit* target = GetTarget();
    Unit* victim = target ? target->GetVictim() : nullptr;
    if (!CastMeleeSpellAction::Execute(event))
        return false;

    LOG_DEBUG("playerbots", "[rot432] {} Kidney Shot on {}: it hits the non-tank {}", bot->GetName(),
              target ? target->GetName() : "?", victim ? victim->GetName() : "?");
    return true;
}
