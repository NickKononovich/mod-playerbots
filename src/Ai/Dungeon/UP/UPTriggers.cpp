/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UPTriggers.h"
#include "AiObjectContext.h"
#include "DungeonInteract.h"
#include "InstanceScript.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "Spell.h"  // [b36_ymiron] Ymiron's current Bane cast
#include "UPActions.h"  // [b35_up2] SkadiHarpoonRunAllowed

bool SkadiFreezingCloudTrigger::IsActive()
{
    Unit* bossMount = AI_VALUE2(Unit*, "find target", "grauf");
    if (!bossMount) { return false; }

    // Need to check two conditions here - the persistent ground effect doesn't
    // seem to be detectable until 3-5 secs in, despite it dealing damage.
    // The initial breath triggers straight away but once it's over, the bots will run back on
    // to the freezing cloud and take damage.
    // Therefore check both conditions and trigger on either.

    // Check this one first, if true then we don't need to iterate over any objects
    if (bossMount->HasAura(SPELL_FREEZING_CLOUD_BREATH))
    {
        return true;
    }

    // Otherwise, check for persistent ground objects emitting the freezing cloud
    GuidVector objects = AI_VALUE(GuidVector, "nearest hostile npcs");
    for (auto i = objects.begin(); i != objects.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (unit && unit->GetEntry() == NPC_BREATH_TRIGGER)
        {
            Unit::AuraApplicationMap const& Auras = unit->GetAppliedAuras();
            for (Unit::AuraApplicationMap::const_iterator itr = Auras.begin(); itr != Auras.end(); ++itr)
            {
                Aura* aura = itr->second->GetBase();
                if (aura && aura->GetId() == SPELL_FREEZING_CLOUD)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool SkadiWhirlwindTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "skadi the ruthless");
    return boss && boss->HasAura(SPELL_SKADI_WHIRLWIND);
}

// Only the aura reflects damage (48294 procs 48295); attacks during the 2 s cast are safe and interrupts need
// the target.
// [b36_ymiron] ...but reacting to the aura is too late: in run 33 the aura reflected 3-4 queued hits per melee
// (4.1-6.2k each) inside 25 ms of landing and killed Rtpalaq/Rtwaraj/Rtdekak before any bot tick saw it; the
// tank only reacted after they died. Damage now stops on the CAST; the target is kept so interrupts still go.
bool YmironBaneTrigger::IsActive() { return UPEvent::IsBaneThreat(botAI); }

namespace
{
// utgarde_pinnacle.h data values are uint32 instance data, not boss states (instance_utgarde_pinnacle.cpp GetData)
bool IsEncounterData(InstanceScript const* instance, uint32 data, EncounterState state)
{
    return instance && instance->GetData(data) == static_cast<uint32>(state);
}

bool IsNearAreaTrigger(Player* bot, uint32 triggerId)
{
    AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
    return at && at->map == bot->GetMapId() &&
           bot->GetExactDist(at->x, at->y, at->z) <= UPEvent::EVENT_APPROACH_YD;
}
}  // namespace

namespace UPEvent
{
InstanceScript* GetInstance(Player* bot)
{
    if (bot->GetMapId() != UPIds::MAP_ID)
        return nullptr;

    return bot->GetInstanceScript();
}

bool IsReadyToStart(Player* bot)
{
    if (bot->IsInCombat() || !DungeonInteract::IsGroupGathered(bot, bot, GATHER_YD))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        // nobody dead, everyone rested (bots eat and drink out of combat)
        if (!member->IsAlive() || member->GetHealthPct() < START_MIN_HEALTH_PCT ||
            (member->getPowerType() == POWER_MANA && member->GetPowerPct(POWER_MANA) < START_MIN_MANA_PCT))
            return false;
    }
    return true;
}

Creature* GetGrauf(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return nullptr;

    Creature* grauf = instance->GetCreature(UPIds::DATA_GRAUF);
    return grauf && grauf->IsAlive() ? grauf : nullptr;
}

bool IsGauntletRunning(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    return IsEncounterData(instance, UPIds::DATA_SKADI_THE_RUTHLESS, IN_PROGRESS) && GetGrauf(bot);
}

GameObject* GetStasisGenerator(Player* bot)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance)
        return nullptr;

    return bot->GetMap()->GetGameObject(instance->GetGuidData(UPIds::DATA_STATIS_GENERATOR));
}

bool NeedsSvalaStart(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = GetInstance(bot);
    if (!instance || bot->IsInCombat() || !IsEncounterData(instance, UPIds::DATA_SVALA_SORROWGRAVE, NOT_STARTED))
        return false;

    if (!DungeonInteract::IsInteractor(botAI) || !IsNearAreaTrigger(bot, UPIds::AT_SVALA_INTRO))
        return false;

    // boss_svala.cpp SetData(1, 1): only Svala before the intro (29281) still waits for the trigger
    Creature* svala = bot->FindNearestCreature(UPIds::NPC_SVALA, SVALA_SEARCH_YD);
    return svala && !svala->IsInCombat();
}

bool NeedsGortokStart(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = GetInstance(bot);
    if (!instance || bot->IsInCombat() || !IsEncounterData(instance, UPIds::DATA_SVALA_SORROWGRAVE, DONE) ||
        !IsEncounterData(instance, UPIds::DATA_GORTOK_PALEHOOF, NOT_STARTED) || !DungeonInteract::IsInteractor(botAI))
        return false;

    // boss_palehoof.cpp go_palehoof_sphere: the click sets NOT_SELECTABLE + ACTIVE; Reset restores READY
    GameObject* generator = GetStasisGenerator(bot);
    if (!generator || !generator->isSpawned() || generator->GetGoState() != GO_STATE_READY ||
        generator->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) || !bot->IsWithinDistInMap(generator, EVENT_APPROACH_YD))
        return false;

    Creature* gortok = bot->GetMap()->GetCreature(instance->GetGuidData(UPIds::DATA_GORTOK_PALEHOOF));
    return gortok && gortok->IsAlive() && !gortok->IsInCombat();
}

bool NeedsSkadiStart(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    InstanceScript* instance = GetInstance(bot);
    if (!instance || bot->IsInCombat() || !IsEncounterData(instance, UPIds::DATA_GORTOK_PALEHOOF, DONE) ||
        !IsEncounterData(instance, UPIds::DATA_SKADI_THE_RUTHLESS, NOT_STARTED) ||
        !DungeonInteract::IsInteractor(botAI) || !IsNearAreaTrigger(bot, UPIds::AT_SKADI_GAUNTLET))
        return false;

    Creature* skadi = instance->GetCreature(UPIds::DATA_SKADI_THE_RUTHLESS);
    return skadi && skadi->IsAlive() && !skadi->IsInCombat() && GetGrauf(bot);
}

Creature* FindRitualChanneler(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (bot->GetMapId() != UPIds::MAP_ID)
        return nullptr;

    // Svala hovers unattackable over the altar during the ritual, so her threat link is no reliable gate; the
    // channelers only exist during the ritual and the group is in combat with her.
    if (!bot->IsInCombat())
        return nullptr;

    // boss_svala.cpp EVENT_SORROWGRAVE_RITUAL: three channelers paralyze the sacrifice; they despawn after it
    return bot->FindNearestCreature(UPIds::NPC_RITUAL_CHANNELER, CHANNELER_SEARCH_YD);
}

int GraufBreathSide(Player* bot)
{
    Creature* grauf = GetGrauf(bot);
    if (!grauf)
        return 0;

    if (grauf->HasAura(UPIds::SPELL_FREEZING_CLOUD_LEFT_PERIODIC))
        return 1;

    return grauf->HasAura(UPIds::SPELL_FREEZING_CLOUD_RIGHT_PERIODIC) ? -1 : 0;
}

bool InGauntletHall(Player* bot)
{
    return bot->GetMapId() == UPIds::MAP_ID && bot->GetPositionX() >= UPIds::HALL_X_MIN &&
           bot->GetPositionX() <= UPIds::HALL_X_MAX && bot->GetPositionY() >= UPIds::HALL_Y_MIN &&
           bot->GetPositionY() <= UPIds::HALL_Y_MAX && bot->GetPositionZ() >= UPIds::HALL_Z_MIN &&
           bot->GetPositionZ() <= UPIds::HALL_Z_MAX;
}

float BreathSafeY(Player* bot)
{
    int const side = GraufBreathSide(bot);
    if (!side || !InGauntletHall(bot))
        return 0.0f;

    float const y = bot->GetPositionY();
    if (side > 0)  // clouds on y >= -511
        return y > UPIds::BREATH_SPLIT_Y - UPIds::BREATH_SIDE_MARGIN ? UPIds::BREATH_SAFE_SOUTH_Y : 0.0f;

    return y < UPIds::BREATH_SPLIT_Y + UPIds::BREATH_SIDE_MARGIN ? UPIds::BREATH_SAFE_NORTH_Y : 0.0f;
}

Unit* FindStagingHarpooner(PlayerbotAI* botAI)
{
    constexpr float stagingRange = 35.0f;
    Player* bot = botAI->GetBot();
    Unit* best = nullptr;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        if (guid.GetEntry() != UPIds::NPC_YMIRJAR_HARPOONER)
            continue;

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || bot->GetExactDist2d(unit) > stagingRange)
            continue;

        if (!best || bot->GetExactDist2d(unit) < bot->GetExactDist2d(best))
            best = unit;
    }
    return best;
}

bool IsBaneUp(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    Unit* boss = botAI->GetAiObjectContext()->GetValue<Unit*>("find target", "king ymiron")->Get();
    return boss && boss->HasAura(SPELL_BANE);
}

// [b36_ymiron]
Unit* GetYmiron(PlayerbotAI* botAI)
{
    Unit* boss = botAI->GetAiObjectContext()->GetValue<Unit*>("find target", "king ymiron")->Get();
    return boss && boss->IsAlive() ? boss : nullptr;
}

// [b36_ymiron] the cast is non-triggered with a cast time; heroic runs it as the difficulty spell 59301
bool IsBaneCasting(PlayerbotAI* botAI)
{
    Unit* boss = GetYmiron(botAI);
    if (!boss)
        return false;

    Spell* spell = boss->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell || !spell->m_spellInfo)
        return false;

    uint32 const id = spell->m_spellInfo->Id;
    return id == SPELL_BANE_N || id == SPELL_BANE_H;
}

// [b36_ymiron]
bool IsBaneThreat(PlayerbotAI* botAI) { return IsBaneUp(botAI) || IsBaneCasting(botAI); }
}  // namespace UPEvent

bool YmironBanePurgeTrigger::IsActive() { return UPEvent::IsBaneUp(botAI); }

bool SvalaStartReadyTrigger::IsActive() { return UPEvent::NeedsSvalaStart(botAI); }

bool SvalaRitualTrigger::IsActive()
{
    if (PlayerbotAI::IsHeal(bot) || bot->HasAura(UPIds::SPELL_PARALYZE))
        return false;

    return UPEvent::FindRitualChanneler(botAI) != nullptr;
}

bool GortokStartReadyTrigger::IsActive() { return UPEvent::NeedsGortokStart(botAI); }

bool SkadiStartReadyTrigger::IsActive() { return UPEvent::NeedsSkadiStart(botAI); }

bool SkadiGauntletTrigger::IsActive()
{
    // [b35_up2] a harpoon carrier whose launcher run is held walks the gauntlet behind the tank like everyone else
    return bot->GetMapId() == UPIds::MAP_ID && UPEvent::IsGauntletRunning(bot) &&
           (!bot->HasItemCount(UPIds::ITEM_HARPOON) || !SkadiHarpoonRunAllowed(botAI));
}

bool SkadiHarpoonOnGroundTrigger::IsActive()
{
    if (bot->GetMapId() != UPIds::MAP_ID || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot) ||
        bot->HasItemCount(UPIds::ITEM_HARPOON) || !UPEvent::IsGauntletRunning(bot))
        return false;

    return bot->FindNearestGameObject(UPIds::GO_HARPOON, UPEvent::HARPOON_SEARCH_YD, true) != nullptr;
}

bool SkadiHarpoonInHandTrigger::IsActive()
{
    // [b35_up2] only in the harpoon phase with a clear path (5D-35: stale harpoons ran three dps into wave 1)
    return bot->GetMapId() == UPIds::MAP_ID && bot->HasItemCount(UPIds::ITEM_HARPOON) &&
           UPEvent::IsGauntletRunning(bot) && SkadiHarpoonRunAllowed(botAI);
}

bool SkadiBreathSideTrigger::IsActive() { return UPEvent::BreathSafeY(bot) != 0.0f; }

bool SkadiHarpoonerTrigger::IsActive()
{
    if (!PlayerbotAI::IsDps(bot) || bot->GetMapId() != UPIds::MAP_ID || bot->HasItemCount(UPIds::ITEM_HARPOON) ||
        !UPEvent::IsGauntletRunning(bot))
        return false;

    Unit* harpooner = UPEvent::FindStagingHarpooner(botAI);
    return harpooner && AI_VALUE(Unit*, "current target") != harpooner;
}
