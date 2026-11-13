/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BudgetValues.h"
#include "GuildHonest.h"
#include "Playerbots.h"
#include "World.h"

uint32 MaxGearRepairCostValue::Calculate()
{
    uint32 totalCost = 0;

    for (int i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        uint16 pos = ((INVENTORY_SLOT_BAG_0 << 8) | i);
        Item* item = bot->GetItemByPos(pos);

        if (!item)
            continue;

        uint32 maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (!maxDurability)
            continue;

        uint32 curDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);

        if (uint32(i) >= EQUIPMENT_SLOT_END && curDurability >= maxDurability)  // Only count items equiped or already damanged.
            continue;

        ItemTemplate const* ditemProto = item->GetTemplate();

        DurabilityCostsEntry const* dcost = sDurabilityCostsStore.LookupEntry(ditemProto->ItemLevel);
        if (!dcost)
            continue;

        uint32 dQualitymodEntryId = (ditemProto->Quality + 1) * 2;
        DurabilityQualityEntry const* dQualitymodEntry = sDurabilityQualityStore.LookupEntry(dQualitymodEntryId);
        if (!dQualitymodEntry)
            continue;

        uint32 dmultiplier =
            dcost->multiplier[ItemSubClassToDurabilityMultiplierId(ditemProto->Class, ditemProto->SubClass)];

        uint32 costs = uint32(maxDurability * dmultiplier * double(dQualitymodEntry->quality_mod));

        totalCost += costs;
    }

    return totalCost;
}

uint32 RepairCostValue::Calculate()
{
    uint32 totalCost = 0;

    for (int i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        uint16 pos = ((INVENTORY_SLOT_BAG_0 << 8) | i);
        Item* item = bot->GetItemByPos(pos);

        if (!item)
            continue;

        uint32 maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (!maxDurability)
            continue;

        uint32 curDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);

        uint32 LostDurability = maxDurability - curDurability;

        if (LostDurability == 0)
            continue;

        ItemTemplate const* ditemProto = item->GetTemplate();

        DurabilityCostsEntry const* dcost = sDurabilityCostsStore.LookupEntry(ditemProto->ItemLevel);
        if (!dcost)
            continue;

        uint32 dQualitymodEntryId = (ditemProto->Quality + 1) * 2;
        DurabilityQualityEntry const* dQualitymodEntry = sDurabilityQualityStore.LookupEntry(dQualitymodEntryId);
        if (!dQualitymodEntry)
            continue;

        uint32 dmultiplier =
            dcost->multiplier[ItemSubClassToDurabilityMultiplierId(ditemProto->Class, ditemProto->SubClass)];
        uint32 costs = uint32(LostDurability * dmultiplier * double(dQualitymodEntry->quality_mod));

        totalCost += costs;
    }

    return totalCost;
}

uint32 TrainCostValue::Calculate()
{
    uint32 totalCost = 0;

    std::unordered_set<uint32> spells;

    CreatureTemplateContainer const* ctc = sObjectMgr->GetCreatureTemplates();
    for (CreatureTemplateContainer::const_iterator itr = ctc->begin(); itr != ctc->end(); ++itr)
    {
        if (!(itr->second.npcflag & UNIT_NPC_FLAG_TRAINER))
            continue;

        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(itr->first);
        if (!trainer)
            continue;

        if (trainer->GetTrainerType() != Trainer::Type::Class || !trainer->IsTrainerValidForPlayer(bot))
            continue;

        for (auto& spell : trainer->GetSpells())
        {
            Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
            if (!trainerSpell)
                continue;

            if (!trainer->CanTeachSpell(bot, trainerSpell))
                continue;

            if (spells.find(trainerSpell->SpellId) != spells.end())
                continue;

            totalCost += trainerSpell->MoneyCost;
            spells.insert(trainerSpell->SpellId);
        }
    }

    return totalCost;
}

uint32 MoneyNeededForValue::Calculate()
{
    NeedMoneyFor needMoneyFor = NeedMoneyFor(stoi(getQualifier()));

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    AiObjectContext* context = botAI->GetAiObjectContext();

    uint32 moneyWanted = 0;

    uint32 level = bot->GetLevel();

    switch (needMoneyFor)
    {
        case NeedMoneyFor::none:
            moneyWanted = 0;
            break;
        case NeedMoneyFor::repair:
            // [eco198] coordinator 2026-11-11: a guild-plan bot keeps back the repair it faces, not the bill of every
            // item at 0 durability: its repair now plus what its next death costs (the core's
            // Rate.DurabilityLoss.OnDeath of each equipped item, Player::DurabilityLossAll). The worst case left most
            // members no money for food or drink at all - b430: 47573 "buy drink" vendor visits bought nothing
            // (Maronic 59 s, Pherua, Zharet), the leader mailed them supply gold they then kept back for the same
            // bill; its REPAIR task and the leader's repair gold cover a bigger bill
            if (GuildHonest::IsHonestBot(bot))
                moneyWanted = AI_VALUE(uint32, "repair cost") +
                              uint32(float(AI_VALUE(uint32, "max repair cost")) *
                                     sWorld->getRate(RATE_DURABILITY_LOSS_ON_DEATH) / 100.0f);
            else
                moneyWanted = AI_VALUE(uint32, "max repair cost");
            break;
        case NeedMoneyFor::ammo:
            moneyWanted = (bot->getClass() == CLASS_HUNTER)
                              ? (level * level * level) / 10
                              : 0;  // Or level^3 (1s @ lvl10, 30s @ lvl30, 2g @ lvl60, 5g @ lvl80): Todo replace
                                    // (should be best ammo buyable x 8 stacks cost)
            break;
        case NeedMoneyFor::spells:
            // [eco202] coordinator 2026-11-11: a guild-plan bot pays no class trainer - its class trainers' spells
            // come at each level-up and login (AutoMaintenanceOnLevelupAction::LearnGuildClassSpells, the approved
            // AutoLearnTrainerSpells exception; b430's ledger: no class-trainer money at all). What "train cost"
            // still finds are spells that path does not teach (other trainers' lists): money kept back for a visit
            // that never comes, ahead of its food and drink - a bot that cannot regenerate does not level to them
            moneyWanted = GuildHonest::IsHonestBot(bot) ? 0 : AI_VALUE(uint32, "train cost");
            break;
        case NeedMoneyFor::travel:
            moneyWanted = 1500;  // 15s for traveling half a continent. Todo: Add better calculation (Should be ???)
            break;
        case NeedMoneyFor::gear:
            moneyWanted = level * level * level;  // Or level^3 (10s @ lvl10, 3g @ lvl30, 20g @ lvl60, 50g @ lvl80):
                                                  // Todo replace (Should be ~total cost of all >green gear equiped)
            break;
        case NeedMoneyFor::consumables:
            moneyWanted =
                (level * level * level) / 10;  // Or level^3 (1s @ lvl10, 30s @ lvl30, 2g @ lvl60, 5g @ lvl80): Todo
                                               // replace (Should be best food/drink x 2 stacks cost)
            break;
        case NeedMoneyFor::guild:
            if (botAI->HasStrategy("guild", BOT_STATE_NON_COMBAT))
            {
                if (bot->GetGuildId())
                    moneyWanted = AI_VALUE2(uint32, "item count", chat->FormatQItem(5976)) ? 0 : 10000;  // 1g (tabard)
                else
                    moneyWanted =
                        AI_VALUE2(uint32, "item count", chat->FormatQItem(5863)) ? 0 : 10000;  // 10s (guild charter)
            }
            break;
        case NeedMoneyFor::tradeskill:
            moneyWanted = (level * level *
                           level);  // Or level^3 (10s @ lvl10, 3g @ lvl30, 20g @ lvl60, 50g @ lvl80): Todo replace
                                    // (Should be buyable reagents that combined allow crafting of usefull items)
            break;
        default:
            break;
    }

    return moneyWanted;
};

uint32 TotalMoneyNeededForValue::Calculate()
{
    NeedMoneyFor needMoneyFor = NeedMoneyFor(stoi(getQualifier()));

    uint32 moneyWanted = AI_VALUE2(uint32, "money needed for", (uint32)needMoneyFor);

    auto needPtr = std::find(saveMoneyFor.begin(), saveMoneyFor.end(), needMoneyFor);

    while (needPtr != saveMoneyFor.begin())
    {
        needPtr--;

        NeedMoneyFor alsoNeed = *needPtr;

        moneyWanted = moneyWanted + AI_VALUE2(uint32, "money needed for", (uint32)alsoNeed);
    }

    return moneyWanted;
}

uint32 FreeMoneyForValue::Calculate()
{
    uint32 money = bot->GetMoney();

    if (IsRealPlayer(botAI->GetMaster()))
        return money;

    uint32 savedMoney = AI_VALUE2(uint32, "total money needed for", getQualifier()) -
                        AI_VALUE2(uint32, "money needed for", getQualifier());
    // [eco203] coordinator 2026-11-11: a guild-plan bot's food and drink for the rest it needs now come before its
    // travel reserve - kept only from the money left after that restock (a bot that cannot regenerate does not
    // travel safely anyway): the consumables budget does not hold it back
    // [eco41] coordinator 2026-11-13: ... and before its repair reserve, for the same reason - the repair is paid
    // from what is left and from the next loot ([broken40]: a bot short of the full cost repairs what it can), while
    // a bot that cannot drink does not fight at all. b475 Geron L20 held 13s 69c and "may spend 0c of its 1369c
    // (kept back: repair 1848c (now 1680c + one death))": it stood at a vendor selling Ice Cold Milk and bought
    // nothing ("[eco204] ... it may spend 13c"), then logged "[supply34] out of drink: no vendor run"
    if (NeedMoneyFor(stoi(getQualifier())) == NeedMoneyFor::consumables && GuildHonest::IsHonestBot(bot))
    {
        savedMoney -= std::min(savedMoney, AI_VALUE2(uint32, "money needed for", uint32(NeedMoneyFor::travel)));
        // only as far as its own consumables need: the rest of the repair reserve stays repair money
        uint32 const fromRepair = std::min(AI_VALUE2(uint32, "money needed for", uint32(NeedMoneyFor::repair)),
                                           AI_VALUE2(uint32, "money needed for", uint32(NeedMoneyFor::consumables)));
        savedMoney -= std::min(savedMoney, fromRepair);
    }

    if (savedMoney > money)
        return 0;

    return money - savedMoney;
};

bool ShouldGetMoneyValue::Calculate() { return !AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::anything); };
