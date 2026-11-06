/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyMemberToHeal.h"

#include <algorithm>
#include <limits>

#include "BattlegroundWS.h"
#include "SpellAuraEffects.h"
#include "Group.h"
#include "HealAssignmentValues.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    constexpr float TANK_HEAL_PRIORITY = 10.0f;      // health percent a tank counts as already missing
    constexpr float ASSIGNED_HEAL_PRIORITY = 15.0f;  // same for a member of this healer's raid assignment
    constexpr float PREDICTED_FULL_HEALTH = 95.0f;   // a member the heals already being cast will top off

    // [rot_b31_heal] Threat-of-death triage. dme-n-5H-29 18:48:25-18:48:40: the bear tank went 88 -> 46 -> 3 % while
    // the disc priest's Flash Heals went to the mage under 35 %; the plain health rank let a dps that had stopped
    // taking damage outrank a tank dropping ~10 % per 2 s. Both terms read the cached "group health loss" map
    // (HealthLossTracker, net loss over the last 2 s), nothing new is sampled.
    constexpr float TANK_LOSS_WEIGHT = 1.0f;      // extra rank per % a tank lost in the window, on top of lossWeight
    constexpr float STABLE_LOSS_PCT = 2.0f;       // a non-tank losing less than this in the window is not under attack
    constexpr float STABLE_WAIT_PRIORITY = 8.0f;  // ... and can wait one GCD for a member who is
    constexpr float STABLE_WAIT_FLOOR = 20.0f;    // below this health nobody waits, stable or not

    // [b31_shared] A tank carrying this paladin's own Beacon of Light gets every heal the paladin lands on someone
    // else. 36-39 % of Holy Flash of Light / Holy Light went to the beaconed tank: while that tank is at or above
    // BEACON_TANK_SAFE_PCT, a non-tank under BEACON_OTHER_HURT_PCT is healed first.
    constexpr uint32 SPELL_BEACON_OF_LIGHT = 53563;
    constexpr float BEACON_TANK_SAFE_PCT = 50.0f;
    constexpr float BEACON_OTHER_HURT_PCT = 90.0f;

    // Another living healer in the group, so a member it is already healing can be left to it.
    bool HasOtherHealer(Group* group, Player* bot)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() && member->GetMapId() == bot->GetMapId() &&
                PlayerbotAI::IsHeal(member))
                return true;
        }

        return false;
    }
}

inline bool compareByHealth(Unit const* u1, Unit const* u2) { return u1->GetHealthPct() < u2->GetHealthPct(); }

namespace HealTriage
{
    bool IsHealImmune(Player* healer, Unit* target)
    {
        if (!target)
            return false;

        // Rank 1 of a plain direct heal of the healer's school: Ice Block and Cyclone block every school, so one
        // probe answers for all the healer's spells.
        uint32 probeSpell = 2050;  // Lesser Heal (Holy)
        switch (healer ? healer->getClass() : 0)
        {
            case CLASS_DRUID:
                probeSpell = 5185;  // Healing Touch (Nature)
                break;
            case CLASS_SHAMAN:
                probeSpell = 331;  // Healing Wave (Nature)
                break;
            case CLASS_PALADIN:
                probeSpell = 635;  // Holy Light (Holy)
                break;
            default:
                break;
        }

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(probeSpell);
        // [lead946] the healer is the caster: the core's friendly-caster rule (IgnoresSchoolImmunityFromFriendlyCaster)
        // decides, as it does for the real cast - without a caster every school immunity read as "immune"
        return spellInfo && target->IsImmunedToSpell(spellInfo, healer);
    }

    bool CannotLoseHealth(Player* healer, Unit* target)
    {
        if (!target || !target->IsImmunedToDamage(SPELL_SCHOOL_MASK_ALL))
            return false;
        // the immunity's end: the longest of the auras that give it (a permanent one has no end)
        int32 left = 0;
        for (AuraType type : {SPELL_AURA_SCHOOL_IMMUNITY, SPELL_AURA_DAMAGE_IMMUNITY})
            for (AuraEffect const* effect : target->GetAuraEffectsByType(type))
            {
                int32 const duration = effect->GetBase()->GetDuration();
                left = duration < 0 ? std::numeric_limits<int32>::max() : std::max(left, duration);
            }
        // the healer's global cooldown: a heal's start recovery (1.5 s) under its haste, never under the game's 1 s
        constexpr float GCD_FLOOR_MS = 1000.0f;
        SpellInfo const* probe = sSpellMgr->GetSpellInfo(2050);  // Lesser Heal: the heals' global cooldown
        float const haste = healer ? healer->GetFloatValue(UNIT_MOD_CAST_SPEED) : 1.0f;
        float const gcd = probe ? std::max(GCD_FLOOR_MS, float(probe->StartRecoveryTime) * haste) : GCD_FLOOR_MS;
        return float(left) > gcd;
    }

    bool IsPeriodicHeal(SpellInfo const* info)
    {
        return info && info->HasAura(SPELL_AURA_PERIODIC_HEAL);
    }

    bool IsThreatHeal(SpellInfo const* info)
    {
        if (!info || info->HasAttribute(SPELL_ATTR1_NO_THREAT))
            return false;
        for (SpellEffectInfo const& effect : info->Effects)
            if (effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_PCT ||
                effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL))
                return true;
        return false;
    }

    bool DrainsOwnHealth(Player* member)
    {
        constexpr uint32 SPELL_LIFE_TAP_RANK_1 = 1454;
        if (!member || member->getClass() != CLASS_WARLOCK)
            return false;
        for (uint32 rank = sSpellMgr->GetLastSpellInChain(SPELL_LIFE_TAP_RANK_1); rank;
             rank = sSpellMgr->GetPrevSpellInChain(rank))
            if (member->HasSpell(rank))
                return true;
        return member->HasSpell(SPELL_LIFE_TAP_RANK_1);
    }

    bool TankUpkeepYields(PlayerbotAI* botAI, Unit* tank)
    {
        Player* bot = botAI->GetBot();
        if (!tank || !tank->IsAlive() || !bot->IsInCombat() || !bot->GetGroup())
            return false;

        // A tank that is itself hurt keeps its HoTs.
        if (tank->GetHealthPct() < sPlayerbotAIConfig.AlmostFullHealth)
            return false;

        Unit* toHeal = botAI->GetAiObjectContext()->GetValue<Unit*>("party member to heal")->Get();
        return toHeal && toHeal != tank && toHeal->IsAlive() &&
               toHeal->GetHealthPct() < sPlayerbotAIConfig.MediumHealth;
    }

    bool HasImmuneMemberToHeal(PlayerbotAI* botAI)
    {
        Player* bot = botAI->GetBot();
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || !member->IsInMap(bot) ||
                member->GetHealthPct() >= sPlayerbotAIConfig.MediumHealth ||
                bot->GetDistance(member) > sPlayerbotAIConfig.HealDistance * 2)
                continue;

            if (IsHealImmune(bot, member) || CannotLoseHealth(bot, member))  // [lead946]
                return true;
        }

        return false;
    }
}

Unit* PartyMemberToHeal::Calculate()
{
    Group* group = bot->GetGroup();
    Battleground* bg = bot->GetBattleground();
    bool isWarsong = bg && bg->GetBgTypeID(true) == BATTLEGROUND_WS;
    ObjectGuid carrierGuid;
    if (isWarsong && bg->GetStatus() == STATUS_IN_PROGRESS && PlayerbotAI::IsHeal(bot))
        carrierGuid = static_cast<BattlegroundWS*>(bg)->GetFlagPickerGUID(bg->GetOtherTeamId(bot->GetTeamId()));
    if (!group)
        return bot;

    bool isRaid = group->isRaidGroup();
    MinValueCalculator calc(100);
    IncomingHealMap const& incoming = context->GetValue<IncomingHealMap>("incoming heals")->RefGet();
    // A member losing health fast is ranked by the health it is heading to: a 15k-health caster under two melee
    // mobs goes from 90 % to dead in about four seconds, faster than the health bands react.
    float const lossWeight = sPlayerbotAIConfig.HealerHealthLossWeight;
    HealthLossMap const& losses = context->GetValue<HealthLossMap>("group health loss")->RefGet();

    // If focus heal targets strategy is active, only heal those targets
    if (botAI->HasStrategy("focus heal targets", BOT_STATE_COMBAT))
    {
        std::list<ObjectGuid> const focusHealTargets =
            AI_VALUE(std::list<ObjectGuid>, "focus heal targets");

        for (ObjectGuid const& focusHealTarget : focusHealTargets)
        {
            Player* player = ObjectAccessor::FindPlayer(focusHealTarget);
            if (!player || !player->IsInWorld() || !player->IsAlive() || !player->IsInSameGroupWith(bot))
                continue;

            float health = player->GetHealthPct();
            bool const beingHealed = incoming.find(player->GetGUID()) != incoming.end();
            if (isRaid || health < sPlayerbotAIConfig.MediumHealth || !beingHealed)
            {
                float const predicted = PredictedHealthPct(player, incoming);
                if (beingHealed && predicted >= PREDICTED_FULL_HEALTH)
                    continue;

                float probeValue = 100.0f;
                if (player->GetDistance2d(bot) > sPlayerbotAIConfig.HealDistance)
                    probeValue = predicted + 30.0f;
                else
                    probeValue = predicted + player->GetDistance2d(bot) / 10.0f;

                probeValue -= RecentHealthLossPct(player, losses) * lossWeight;
                if (probeValue < calc.minValue && Check(player))
                    calc.probe(probeValue, player);
            }
        }

        return (Unit*)calc.param;
    }

    // Automatic raid assignment is a preference, not a restriction: anyone else low enough is still healed.
    std::list<ObjectGuid> const& assigned =
        context->GetValue<std::list<ObjectGuid>>("auto focus heal targets")->RefGet();

    // Leaving a member to the heal already on its way only makes sense when another healer is there to finish
    // the job. The only healer of a party would otherwise ignore a tank down to medium health each time a hybrid
    // (a paladin tank's Flash of Light, an enhancement shaman's Healing Wave) started a small heal on it.
    bool const spreadHeals = HasOtherHealer(group, bot);

    // [b31_shared] this paladin's beaconed tank, ranked after the loop (see BEACON_TANK_SAFE_PCT)
    bool const beaconHealer = bot->getClass() == CLASS_PALADIN;
    Player* beaconTank = nullptr;
    float beaconTankProbe = 0.0f;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player || player->IsGameMaster())
            continue;
        if (player->IsAlive())
        {
            float health = player->GetHealthPct();
            bool const beingHealed = incoming.find(player->GetGUID()) != incoming.end();
            // Someone already being healed is left to that healer until they drop to medium health, so in a
            // raid the healers spread out instead of all landing on the same target.
            if (health < sPlayerbotAIConfig.MediumHealth || !beingHealed || !spreadHeals)
            {
                // Heals already in flight decide the rank, so a second big heal does not land on a member
                // another healer is about to fill.
                float const predicted = PredictedHealthPct(player, incoming);
                if (beingHealed && predicted >= PREDICTED_FULL_HEALTH)
                    continue;

                float probeValue = 100.0f;
                if (player->GetDistance2d(bot) > sPlayerbotAIConfig.HealDistance)
                {
                    probeValue = predicted + 30.0f;
                }
                else
                {
                    probeValue = predicted + player->GetDistance2d(bot) / 10.0f;
                }

                float const recentLoss = RecentHealthLossPct(player, losses);
                probeValue -= recentLoss * lossWeight;
                // a tank losing health is the more urgent heal
                if (PlayerbotAI::IsTank(player))
                {
                    probeValue -= TANK_HEAL_PRIORITY;
                    // [rot_b31_heal] A tank dropping fast is heading to death sooner than its health says: a tank at
                    // 46 % losing 10 %/2 s ranks ~12, a dps at 30 % taking nothing ranks ~39.
                    probeValue -= recentLoss * TANK_LOSS_WEIGHT;
                }
                else if (recentLoss < STABLE_LOSS_PCT && health >= STABLE_WAIT_FLOOR && bot->IsInCombat())
                {
                    // [rot_b31_heal] Low but no longer hit (its mob was taunted off): one GCD of waiting costs
                    // nothing, so it yields to a member actually losing health. With nobody else hurt it is still
                    // the lowest probe and gets the heal.
                    probeValue += STABLE_WAIT_PRIORITY;
                }
                if (std::find(assigned.begin(), assigned.end(), player->GetGUID()) != assigned.end())
                    probeValue -= ASSIGNED_HEAL_PRIORITY;
                constexpr float carrierPriority = 15.0f;
                if (!carrierGuid.IsEmpty() && player->GetGUID() == carrierGuid &&
                    player->GetHealthPct() < sPlayerbotAIConfig.MediumHealth &&
                    player->GetDistance2d(bot) <= sPlayerbotAIConfig.HealDistance && player->IsInWorld() &&
                    player->GetMap() == bot->GetMap())
                    probeValue -= carrierPriority;
                // [b31_shared] a safe beaconed tank waits for the comparison after the loop
                bool const beaconed = beaconHealer && !beaconTank && health >= BEACON_TANK_SAFE_PCT &&
                                      PlayerbotAI::IsTank(player) &&
                                      player->HasAura(SPELL_BEACON_OF_LIGHT, bot->GetGUID());
                if (beaconed && probeValue < 100.0f && Check(player))
                {
                    beaconTank = player;
                    beaconTankProbe = probeValue;
                }
                // delay Check player to here for better performance
                else if (!beaconed && probeValue < calc.minValue && Check(player))
                {
                    calc.probe(probeValue, player);
                }
            }
        }

        Pet* pet = player->GetPet();
        if (pet && pet->IsAlive())
        {
            float health = ((Unit*)pet)->GetHealthPct();
            float probeValue = 100.0f;
            if (isRaid || health < sPlayerbotAIConfig.MediumHealth)
                probeValue = health + 30.0f;
            // delay Check pet to here for better performance
            if (probeValue < calc.minValue && Check(pet))
            {
                calc.probe(probeValue, pet);
            }
        }

        Unit* charm = player->GetCharm();
        if (charm && charm->IsAlive())
        {
            float health = charm->GetHealthPct();
            float probeValue = 100.0f;
            if (isRaid || health < sPlayerbotAIConfig.MediumHealth)
                probeValue = health + 30.0f;
            // delay Check charm to here for better performance
            if (probeValue < calc.minValue && Check(charm))
            {
                calc.probe(probeValue, charm);
            }
        }
    }

    // [b31_shared] The beaconed tank yields only to a hurt non-tank; with nobody else that low it keeps its rank.
    if (beaconTank)
    {
        Unit* chosen = static_cast<Unit*>(calc.param);
        // [lead1501] b271 rfc-h 5G-7 898-933 s: the beaconed blood tank fell 89 % -> 74 % under a Searing Blade
        // Warlock's Curse of Agony while the holy paladin cast nothing for 35 s: the paladin itself at 85.8 % and the
        // ret paladin at 89.7 % were "hurt" by the yield's 90 % and took the tank's rank, yet no heal trigger fires
        // above the bots' almost-full line (AlmostFullHealth, the lowest heal band of every healer's rotation). The
        // tank yields only to a member a heal of the rotation goes to
        bool const otherHurt = chosen && chosen->GetHealthPct() < BEACON_OTHER_HURT_PCT &&
                               chosen->GetHealthPct() < sPlayerbotAIConfig.AlmostFullHealth;
        if (!otherHurt && (!chosen || beaconTankProbe < calc.minValue))
            return beaconTank;
    }

    return static_cast<Unit*>(calc.param);
}

bool PartyMemberToHeal::Check(Unit* player)
{
    // return player && player != bot && player->GetMapId() == bot->GetMapId() && player->IsInWorld() &&
    //     ServerFacade::instance().GetDistance2d(bot, player) < (player->IsPlayer() && PlayerbotAI::IsTank((Player*)player) ? 50.0f
    //     : 40.0f);
    // Line of sight is not required: a member behind a pillar stays the heal target, so the out-of-range
    // trigger moves the healer to them instead of the healer never seeing them.
    // [heal_triage] A member no heal can land on is not the heal target: HoR heroic Falric 2026-10-25 (waves 26 and
    // 27), the Ice Blocked mage at 18-29 % stayed "party member to heal" for 10 s, every "... on party" heal failed
    // with "target immune", and the resto druid rolled HoTs on a 96 % tank while the hunter and shaman sat at 39 %.
    return player->GetMapId() == bot->GetMapId() && !player->IsCharmed() &&
           bot->GetDistance2d(player) < sPlayerbotAIConfig.HealDistance * 2 && !HealTriage::IsHealImmune(bot, player) &&
           !HealTriage::CannotLoseHealth(bot, player);  // [lead946] its heal waits for the immunity's end
}

Unit* HealerLowMana::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    MinValueCalculator calc(100);

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player || player == bot)
            continue;
        if (player->IsGameMaster() || !player->IsAlive())
            continue;
        if (!PlayerbotAI::IsHeal(player))
            continue;

        float mana = player->GetPowerPct(POWER_MANA);
        if (mana < calc.minValue)
            calc.probe(mana, player);
    }

    return (Unit*)calc.param;
}

Unit* PartyMemberToProtect::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    std::vector<Unit*> needProtect;

    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (GuidVector::iterator i = attackers.begin(); i != attackers.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit)
            continue;

        Unit* pVictim = unit->GetVictim();
        if (!pVictim || !pVictim->IsPlayer())
            continue;

        if (pVictim == bot)
            continue;

        float attackDistance = 30.0f;
        if (ServerFacade::instance().GetDistance2d(pVictim, unit) > attackDistance)
            continue;

        if (PlayerbotAI::IsTank((Player*)pVictim) && pVictim->GetHealthPct() > 10)
            continue;
        else if (pVictim->GetHealthPct() > 30)
            continue;

        if (find(needProtect.begin(), needProtect.end(), pVictim) == needProtect.end())
            needProtect.push_back(pVictim);
    }

    if (needProtect.empty())
        return nullptr;

    sort(needProtect.begin(), needProtect.end(), compareByHealth);

    return needProtect[0];
}
