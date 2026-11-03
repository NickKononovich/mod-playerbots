/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Common.h"
#include "LeaderConsumables.h"

#include "Creature.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LeaderBuffing.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "RouteSteer.h"    // BOSS_BUFF_DISTANCE
#include "RouteTargets.h"  // EventWaveFresh
#include "Player.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace Leader::Dungeon
{
    namespace
    {
        std::set<uint32> const& FlaskSpells()
        {
            static std::set<uint32> const spells = []
            {
                std::set<uint32> result;
                for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                    if (proto.Class == ITEM_CLASS_CONSUMABLE && proto.SubClass == ITEM_SUBCLASS_FLASK &&
                        proto.Spells[0].SpellId > 0)
                        result.insert(uint32(proto.Spells[0].SpellId));
                return result;
            }();
            return spells;
        }
    }

    bool HasFlaskAura(Player* bot)
    {
        std::set<uint32> const& flasks = FlaskSpells();
        for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
            if (flasks.count(spellId))
                return true;
        return false;
    }

    bool UseItem(Player* bot, uint32 itemId)
    {
        if (!bot || !itemId || !bot->IsAlive() || bot->IsNonMeleeSpellCast(false))
            return false;

        Item* item = bot->GetItemByEntry(itemId);
        if (!item || bot->CanUseItem(item) != EQUIP_ERR_OK)
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        uint32 spellId = 0;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS && !spellId; ++i)
            if (proto->Spells[i].SpellId > 0 && proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                spellId = uint32(proto->Spells[i].SpellId);
        if (!spellId)
            return false;

        if (bot->HasSpellCooldown(spellId))
            return false;

        WorldPacket packet(CMSG_USE_ITEM);
        packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0)
               << uint8(0);
        packet << uint32(TARGET_FLAG_NONE) << bot->GetPackGUID();
        bot->GetSession()->HandleUseItemOpcode(packet);
        return true;
    }

    bool UseConsumables(LeaderGroup& group, Player* bot, bool food)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        uint32 flaskItem = 0;
        uint32 foodItem = 0;
        if (!botAI || !bot->IsAlive() || bot->IsInCombat() || !group.MemberConsumables(bot, flaskItem, foodItem))
            return false;

        if (!food)
        {
            if (!flaskItem || HasFlaskAura(bot) || !UseItem(bot, flaskItem))
                return false;
            group.LogEvent("consumable",
                           JsonObject().Str("name", bot->GetName()).Str("kind", "flask").Num("item", flaskItem));
            return true;
        }

        if (HasWellFed(bot))
            return false;

        if (!UseItem(bot, foodItem))
            return false;
        botAI->SetNextCheckDelay(11 * IN_MILLISECONDS);  // sit through the ten seconds of eating
        group.LogEvent("consumable",
                       JsonObject().Str("name", bot->GetName()).Str("kind", "food").Num("item", foodItem));
        return true;
    }

    void BossBuffPass(LeaderGroup& group)
    {
        GroupRouteState& route = group.Route();
        Player* leader = ObjectAccessor::FindPlayer(group.LeaderGuid());
        if (!leader)
            return;
        Map* map = leader->GetMap();
        // the route's boss target; [lead775] b220 rfc-h 5E 703.0 s: the planner pulled Taragaman as its nearest pick
        // ("boss pulled where it stands") while the route's target was a patrolling Searing Blade Enforcer - no buff
        // pass, no ready check before him (buff passes at 264, 971, 1035, 1155, 1160 s): the rogue's Blessing of
        // Might and the tank's Blessing of Sanctuary (cast at 9 s, 10 min) had run out at ~610 s. Any idle boss within
        // BOSS_BUFF_DISTANCE of the leader is a boss the next pull may bring
        Creature* boss = nullptr;
        if (group.RouteTargetIsBoss() && !route.target.IsEmpty())
            boss = map->GetCreature(route.target);
        if (!boss || !boss->IsAlive() || boss->IsInCombat() ||
            leader->GetExactDist(boss) >= Travel::BOSS_BUFF_DISTANCE)
        {
            boss = nullptr;
            for (ObjectGuid const& bossGuid : group.BossCreatures())
            {
                Creature* near = map->GetCreature(bossGuid);
                if (!near || !near->IsAlive() || near->IsInCombat() || !near->IsDungeonBoss())
                    continue;
                float const yd = leader->GetExactDist(near);
                if (yd < Travel::BOSS_BUFF_DISTANCE && (!boss || yd < leader->GetExactDist(boss)))
                    boss = near;
            }
        }
        if (!boss)
            return;

        // [lead775] a boss buffed for already: a buff the group gives that went since (expired, dispelled, a death)
        // and that the last pass did not end without - the readiness never read buffs (the planner's own buff wait
        // is off under the leader, WaitForBuffs: "the buff pass is the leader's"), so the pass runs again
        std::string gone;
        if (route.bossBuffed.count(boss->GetGUID()))
        {
            if (route.needBuff || leader->IsInCombat())
                return;
            std::vector<Player*> players;
            for (LeaderGroup::MemberInfo const& member : group.Members())
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->IsAlive() && bot->GetMap() == map && !bot->IsInCombat())
                    players.push_back(bot);
            std::string const missing = GivableBuffsMissing(group, players, leader);
            size_t start = 0;
            while (start < missing.size())
            {
                size_t const end = std::min(missing.find(", ", start), missing.size());
                std::string const one = missing.substr(start, end - start);
                if (route.buffEndMissing.find(one) == std::string::npos)
                    gone += (gone.empty() ? "" : ", ") + one;
                start = end + 2;
            }
            if (gone.empty())
                return;
        }
        route.bossBuffed.insert(boss->GetGUID());
        // Black Morass: the shield drains during buffs + ready check (50 -> 30 % before Temporus, 33 -> 11 % before
        // Deja in bm-n 2026-10-25); a rift boss is pulled at once.
        if (route.watchedRift > 0)
            return;
        // [brd_seven_fast] a scripted wave that just became attackable: no buff pass / ready check, pulled at once
        // (the Tomb of the Seven resets after 15 s with no dwarf in combat).
        if (Travel::EventWaveFresh(group.InstanceId(), boss->GetGUID()))
        {
            group.LogEvent("event_wave_no_ready_check",
                           JsonObject().Str("name", boss->GetName()).Num("entry", boss->GetEntry()));
            return;
        }
        route.needBuff = true;
        route.buffStep = 0;
        route.buffReason = std::string("before ") + boss->GetName() + (gone.empty() ? "" : " (gone: " + gone + ")");
    }
}
