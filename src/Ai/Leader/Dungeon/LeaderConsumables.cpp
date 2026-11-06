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
#include "LeaderOrders.h"  // [lead1084] Orders::IsOpen
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

    namespace
    {
        // [lead1214] a flask aura on the bot that lasts `needMs` more (or has no end)
        bool HasFlaskLasting(Player* bot, int32 needMs)
        {
            std::set<uint32> const& flasks = FlaskSpells();
            for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
            {
                if (!flasks.count(spellId))
                    continue;
                Aura const* aura = aurApp ? aurApp->GetBase() : nullptr;
                int32 const left = aura ? aura->GetDuration() : -1;
                if (left < 0 || left >= needMs)
                    return true;
            }
            return false;
        }
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

        // [lead1214] b240 rfc-h 5G-15: the run started on Well Fed / flasks left from the instance's last run ("Well
        // Fed on everyone, no food needed"), which ran out mid-run - three members ate before Oggleflint, the flask
        // and the food again before Taragaman. Present is not enough: they must last the next fight (the planner's
        // estimate, the same need as the buffs: NextFightNeedMs)
        int32 const needMs = NextFightNeedMs(ObjectAccessor::FindPlayer(group.LeaderGuid()));
        if (!food)
        {
            if (!flaskItem || HasFlaskLasting(bot, needMs) || !UseItem(bot, flaskItem))
                return false;
            group.LogEvent("consumable",
                           JsonObject().Str("name", bot->GetName()).Str("kind", "flask").Num("item", flaskItem));
            return true;
        }

        if (HasAuraLasting(bot, "Well Fed", needMs))
            return false;

        if (!UseItem(bot, foodItem))
            return false;
        botAI->SetNextCheckDelay(11 * IN_MILLISECONDS);  // sit through the ten seconds of eating
        group.LogEvent("consumable",
                       JsonObject().Str("name", bot->GetName()).Str("kind", "food").Num("item", foodItem));
        return true;
    }

    namespace
    {
        // [lead1084] user 2026-11-04 (Q55, "inadmissible"): no buff pass while a pull is open or a fight is on - a
        // buff on a member in combat puts the buffer in combat, a threat-making buff can pull the boss onto it. The
        // pass opens once the open pull is fought; the boss pull waits for it (BossBuffPassHolds)
        bool PassMayOpen(LeaderGroup& group, Player* leader)
        {
            return leader && !leader->IsInCombat() && !group.FightOn(leader) &&
                   !Leader::Orders::IsOpen(group.LeaderGuid());
        }

        // the pass before `boss` opens (`gone`: the buffs that went since the last pass before him)
        void OpenPass(LeaderGroup& group, Creature* boss, std::string const& gone)
        {
            GroupRouteState& route = group.Route();
            route.bossBuffed.insert(boss->GetGUID());
            // Black Morass: the shield drains during buffs + ready check (50 -> 30 % before Temporus, 33 -> 11 %
            // before Deja in bm-n 2026-10-25); a rift boss is pulled at once.
            if (route.watchedRift > 0)
                return;
            // [brd_seven_fast] a scripted wave that just became attackable: no buff pass / ready check, pulled at
            // once (the Tomb of the Seven resets after 15 s with no dwarf in combat).
            if (Travel::EventWaveFresh(group.InstanceId(), boss->GetGUID()))
            {
                group.LogEvent("event_wave_no_ready_check",
                               JsonObject().Str("name", boss->GetName()).Num("entry", boss->GetEntry()));
                return;
            }
            route.needBuff = true;
            route.buffStep = 0;
            route.buffReason =
                std::string("before ") + boss->GetName() + (gone.empty() ? "" : " (gone: " + gone + ")");
        }

        // [lead775] the buffs the group gives that went since the last pass (expired, dispelled, a death) and that
        // the last pass did not end without (route.buffEndMissing), over the living members out of combat
        std::string GoneSinceLastPass(LeaderGroup& group, Player* leader)
        {
            GroupRouteState const& route = group.Route();
            Map* map = leader->GetMap();
            std::vector<Player*> players;
            for (LeaderGroup::MemberInfo const& member : group.Members())
                if (Player* bot = ObjectAccessor::FindPlayer(member.guid);
                    bot && bot->IsAlive() && bot->GetMap() == map && !bot->IsInCombat())
                    players.push_back(bot);
            std::string const missing = GivableBuffsMissing(group, players, leader);
            std::string gone;
            size_t start = 0;
            while (start < missing.size())
            {
                size_t const end = std::min(missing.find(", ", start), missing.size());
                std::string const one = missing.substr(start, end - start);
                if (route.buffEndMissing.find(one) == std::string::npos)
                    gone += (gone.empty() ? "" : ", ") + one;
                start = end + 2;
            }
            return gone;
        }
    }

    Creature* BossBuffPassHolds(LeaderGroup& group, Player* leader, Unit* pull)
    {
        Creature* const pulled = pull ? pull->ToCreature() : nullptr;
        if (!leader || !pulled || leader->IsInCombat())
            return nullptr;
        GroupRouteState const& route = group.Route();
        if (route.watchedRift > 0)
            return nullptr;
        Map* map = leader->GetMap();
        for (ObjectGuid const& bossGuid : group.BossCreatures())
        {
            Creature* boss = map->GetCreature(bossGuid);
            if (!boss || !boss->IsAlive() || boss->IsInCombat() || !boss->IsDungeonBoss() ||
                leader->GetExactDist(boss) >= Travel::BOSS_BUFF_DISTANCE ||
                Travel::EventWaveFresh(group.InstanceId(), bossGuid))
                continue;
            // [lead1090] b234 rfc-h 5F-14 840-1096 s: lead1084 held every pull for Bazzalan's pass (an idle boss
            // within BOSS_BUFF_DISTANCE), the pull of Jergosh (the route's target) too - and BossBuffPass's pick was
            // Jergosh (his pass done at 839 s): Bazzalan's never opened, the group stood 4 minutes. Only the pull of
            // this boss or a pull that brings him (his formation, script or aggro reach: BossBroughtBy) waits
            if (boss != pulled && !Travel::BossBroughtBy(leader, boss, pulled))
                continue;
            if (!route.bossBuffed.count(bossGuid))
                return boss;
            // [lead1502] b271 rfc-h 5E-5 856.8-908 s: the planner committed the pull that brings Jergosh ("the group
            // ready") while the tank paladin's Blessing of Kings / Might had run out on the priest and the rogue; his
            // pass had run at the room's door, so this hold skipped him. BossBuffPass opened the "gone" pass 0.2 s
            // later, the commit waited 1 s on the rogue's Stealth only and the tank pulled at 857.8 s: Jergosh was
            // fought without the two blessings, the pass ended after his fight (51 s "held" in buff_hold). A boss
            // buffed for already holds his pull again while a pass is under way, or a buff the group gives went
            // since his last pass (the gone pass BossBuffPass opens)
            if (route.needBuff || !GoneSinceLastPass(group, leader).empty())
                return boss;
        }
        return nullptr;
    }

    bool OpenBossBuffPass(LeaderGroup& group, Creature* boss)
    {
        Player* leader = ObjectAccessor::FindPlayer(group.LeaderGuid());
        if (!boss || !PassMayOpen(group, leader))
            return false;
        if (group.Route().needBuff)
            return true;  // a pass is under way: the pull waits for it
        // [lead1502] a boss buffed for already: the pass names what went since
        std::string const gone =
            group.Route().bossBuffed.count(boss->GetGUID()) ? GoneSinceLastPass(group, leader) : std::string();
        OpenPass(group, boss, gone);
        return true;
    }

    void BossBuffPass(LeaderGroup& group)
    {
        GroupRouteState& route = group.Route();
        Player* leader = ObjectAccessor::FindPlayer(group.LeaderGuid());
        if (!PassMayOpen(group, leader))
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
                Creature* nearBoss = map->GetCreature(bossGuid);
                if (!nearBoss || !nearBoss->IsAlive() || nearBoss->IsInCombat() || !nearBoss->IsDungeonBoss())
                    continue;
                float const yd = leader->GetExactDist(nearBoss);
                if (yd < Travel::BOSS_BUFF_DISTANCE && (!boss || yd < leader->GetExactDist(boss)))
                    boss = nearBoss;
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
            gone = GoneSinceLastPass(group, leader);
            if (gone.empty())
                return;
        }
        OpenPass(group, boss, gone);
    }
}
