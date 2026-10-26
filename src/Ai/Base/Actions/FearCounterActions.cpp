/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FearCounterActions.h"

#include "Event.h"
#include "FearCounterValues.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
// [fear_counter] PvP trinket on-use spells (Medallion / Insignia of the Alliance / Horde; SpellInfo.cpp grants
// both the loss-of-control immunity list).
constexpr uint32 SPELL_PVP_TRINKET = 42292;
constexpr uint32 SPELL_PVP_TRINKET_TOC = 65547;
// A pre-dropped Tremor Totem is not dropped again sooner than this when only the tank walked out of its pulse
// (a kiting tank would otherwise make the shaman pay 2 % base mana every global cooldown).
constexpr uint32 TREMOR_REDROP_MS = 6 * IN_MILLISECONDS;
}  // namespace

// Shaman
bool FearCounterTremorTotemAction::isUseful()
{
    if (!bot->IsInCombat() || !bot->HasSpell(FearCounter::SPELL_TREMOR_TOTEM))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    if (Unit* victim = FearCounter::TremorAnchor(botAI, state, true))
        return !FearCounter::TremorCovers(bot, victim);

    if (!(state.threat & FearCounter::THREAT_TREMOR))
        return false;

    if (FearCounter::TremorCovers(bot, FearCounter::TremorAnchor(botAI, state, false)))
        return false;

    // No totem of ours standing at all: drop it now; a standing one that lost the tank waits a little.
    return !FearCounter::TremorCovers(bot, nullptr) || !_lastDropMs ||
           getMSTimeDiff(_lastDropMs, getMSTime()) >= TREMOR_REDROP_MS;
}

bool FearCounterTremorTotemAction::isPossible() { return botAI->CanCastSpell("tremor totem", bot); }

bool FearCounterTremorTotemAction::Execute(Event /*event*/)
{
    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    Unit* victim = FearCounter::TremorAnchor(botAI, state, true);
    if (!botAI->CastSpell("tremor totem", bot))
        return false;

    _lastDropMs = getMSTime();
    if (victim)
        FearCounter::Log(bot, "tremor totem", victim, state.victimAura);
    else
        FearCounter::Log(bot, "tremor totem pre-drop", FearCounter::TremorAnchor(botAI, state, false),
                         state.sourceSpell);
    return true;
}

// Priest
Unit* FearCounterFearWardAction::GetTarget()
{
    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    return state.tank.IsEmpty() ? nullptr : botAI->GetUnit(state.tank);
}

bool FearCounterFearWardAction::isUseful()
{
    if (!bot->HasSpell(FearCounter::SPELL_FEAR_WARD) || bot->HasSpellCooldown(FearCounter::SPELL_FEAR_WARD))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    if (!(state.threat & FearCounter::THREAT_FEAR))
        return false;

    Unit* tank = GetTarget();
    return tank && tank->IsAlive() && !tank->HasAura(FearCounter::SPELL_FEAR_WARD) &&
           bot->IsWithinDistInMap(tank, FearCounter::FEAR_WARD_RANGE) && bot->IsWithinLOSInMap(tank);
}

bool FearCounterFearWardAction::isPossible()
{
    Unit* tank = GetTarget();
    return tank && botAI->CanCastSpell("fear ward", tank);
}

bool FearCounterFearWardAction::Execute(Event /*event*/)
{
    Unit* tank = GetTarget();
    if (!tank || !botAI->CastSpell("fear ward", tank))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    FearCounter::Log(bot, bot->IsInCombat() ? "fear ward" : "fear ward pre-pull", tank, state.sourceSpell);
    return true;
}

// Warrior
bool FearCounterBerserkerRageAction::isUseful()
{
    if (!bot->IsInCombat() || bot->HasAura(FearCounter::SPELL_BERSERKER_RAGE))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    return (state.threat & FearCounter::THREAT_INCOMING) && (state.threat & FearCounter::THREAT_FEAR);
}

bool FearCounterBerserkerRageAction::isPossible() { return botAI->CanCastSpell("berserker rage", bot); }

bool FearCounterBerserkerRageAction::Execute(Event /*event*/)
{
    if (!botAI->CastSpell("berserker rage", bot))
        return false;

    FearCounterState const state = AI_VALUE(FearCounterState, "fear counter state");
    FearCounter::Log(bot, "berserker rage pre-use", bot, state.sourceSpell);
    return true;
}

// PvP trinket
Item* FearCounterPvpTrinketAction::FindPvpTrinket(Player* bot, uint32& spellId)
{
    for (uint8 slot : {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2})
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            _Spell const& itemSpell = item->GetTemplate()->Spells[i];
            if (itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;

            if (itemSpell.SpellId == int32(SPELL_PVP_TRINKET) || itemSpell.SpellId == int32(SPELL_PVP_TRINKET_TOC))
            {
                spellId = uint32(itemSpell.SpellId);
                return item;
            }
        }
    }

    return nullptr;
}

bool FearCounterPvpTrinketAction::isUseful()
{
    uint32 spellId = 0;
    return FearCounter::TremorableAuraSpell(bot) && FindPvpTrinket(bot, spellId);
}

bool FearCounterPvpTrinketAction::isPossible()
{
    uint32 spellId = 0;
    Item* item = FindPvpTrinket(bot, spellId);
    return item && bot->CanUseItem(item) == EQUIP_ERR_OK && !bot->HasSpellCooldown(spellId) &&
           botAI->CanCastSpell(spellId, bot, false, nullptr, item);
}

bool FearCounterPvpTrinketAction::Execute(Event /*event*/)
{
    uint32 spellId = 0;
    Item* item = FindPvpTrinket(bot, spellId);
    if (!item)
        return false;

    uint32 const aura = FearCounter::TremorableAuraSpell(bot);

    // The same CMSG_USE_ITEM path as UseTrinketAction, so the item cooldown is set by the core.
    uint8 const bagIndex = item->GetBagSlot();
    uint8 const slot = item->GetSlot();
    uint8 const castCount = 1;
    uint32 const glyphIndex = 0;
    uint8 const castFlags = 0;
    uint32 const targetFlag = TARGET_FLAG_NONE;
    WorldPacket packet(CMSG_USE_ITEM);
    packet << bagIndex << slot << castCount << spellId << item->GetGUID() << glyphIndex << castFlags;
    packet << targetFlag << bot->GetPackGUID();
    bot->GetSession()->HandleUseItemOpcode(packet);
    if (!bot->HasSpellCooldown(spellId))
        return false;  // the core refused the use

    FearCounter::Log(bot, "pvp trinket", bot, aura);
    return true;
}
