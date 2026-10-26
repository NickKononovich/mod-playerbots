/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RSScripts.h"
#include "Player.h"
#include "RSActions.h"
#include "RSTriggers.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "Vehicle.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace RubySanctumHelpers
{
    static std::mutex stateMutex;
    static std::unordered_map<uint32, RsInstanceState> instanceStates;

    RsInstanceState& RsState(uint32 instanceId)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return instanceStates[instanceId];
    }

    void ResetInstance(uint32 instanceId)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        instanceStates.erase(instanceId);
    }
}

static uint8 RsHalionReadCorporealityIndex(Creature* creature)
{
    using namespace RubySanctumHelpers;
    for (uint8 i = 0; i <= 10; ++i)
    {
        if (creature->HasAura(HALION_CORPOREALITY_AURAS[i]))
            return i;
    }
    return 5;
}

class RsHalionRootScript : public AllCreatureScript
{
public:
    RsHalionRootScript() : AllCreatureScript("RsHalionRootScript") { }

    void OnAllCreatureUpdate(Creature* creature, uint32 ) override
    {
        if (!creature || creature->GetMapId() != RS_MAP_RUBY_SANCTUM)
            return;

        if (creature->GetEntry() == NPC_METEOR_STRIKE_MARK)
        {
            HandleMeteorMark(creature);
            return;
        }

        if (creature->GetEntry() == NPC_HALION)
            HandleHalionRoot(creature);

        if (creature->GetEntry() == NPC_TWILIGHT_HALION)
            HandleTwilightHalionRoot(creature);

        if (creature->GetEntry() == NPC_ORB_CARRIER)
            HandleCutterTiming(creature);

        if ((creature->GetEntry() == NPC_LIVING_INFERNO || creature->GetEntry() == NPC_LIVING_EMBER) &&
            creature->IsAlive())
            RubySanctumHelpers::RsState(creature->GetMap()->GetInstanceId())
                .portalAddGate.lastAddAliveTime = getMSTime();
    }

private:
    void HandleCutterTiming(Creature* carrier)
    {
        uint32 const instanceId = carrier->GetMap()->GetInstanceId();
        RubySanctumHelpers::CutterTiming& state = RubySanctumHelpers::RsState(instanceId).cutterTiming;

        bool firing = false;
        Unit* tracked = nullptr;
        if (Vehicle* vehicle = carrier->GetVehicleKit())
        {
            tracked = vehicle->GetPassenger(0);
            for (int8 seat = 0; seat < 4; ++seat)
            {
                if (Unit* orb = vehicle->GetPassenger(seat); orb && orb->HasAura(SPELL_TWILIGHT_PULSE_PERIODIC))
                {
                    firing = true;
                    break;
                }
            }
        }

        if (firing && !state.active)
            state.lastShootTime = getMSTime();

        state.active = firing;

        if (tracked)
        {
            float const angle = std::atan2(tracked->GetPositionY() - carrier->GetPositionY(),
                                           tracked->GetPositionX() - carrier->GetPositionX());
            if (state.hasOrbAngle)
            {
                float const delta = RsAngleDiff(angle, state.lastOrbAngle);
                if (std::fabs(delta) > 0.0005f)
                    state.spinSign = delta > 0.0f ? 1.0f : -1.0f;
            }
            state.lastOrbAngle = angle;
            state.hasOrbAngle = true;
        }
        else
        {
            state.hasOrbAngle = false;
        }
    }

    void HandleTwilightHalionRoot(Creature* creature)
    {
        RubySanctumHelpers::RsInstanceState& instState =
            RubySanctumHelpers::RsState(creature->GetMap()->GetInstanceId());

        if (creature->IsAlive() && creature->IsInCombat())
        {
            RubySanctumHelpers::HalionCorporeality& corp = instState.halionCorporeality;
            corp.twilightIndex = RsHalionReadCorporealityIndex(creature);
            corp.twilightStamp = getMSTime();
            instState.bossHealth = { uint8(creature->GetHealthPct()), getMSTime() };
        }

        RubySanctumHelpers::CutterTiming& state = instState.cutterTiming;
        if (state.bossGuid != creature->GetGUID())
        {
            state = RubySanctumHelpers::CutterTiming{};
            state.bossGuid = creature->GetGUID();
        }
        if (state.encounterStart == 0 && creature->IsInCombat())
            state.encounterStart = getMSTime();

        // Observation only: the bots read the encounter, they never root, move, buff or dispel anyone here.
    }

    void HandleMeteorMark(Creature* mark)
    {
        if (!mark->IsAlive())
            return;

        RubySanctumHelpers::RsInstanceState& instState = RubySanctumHelpers::RsState(mark->GetMap()->GetInstanceId());

        if (!instState.seenMeteorMarks.insert(mark->GetGUID()).second)
            return;

        RubySanctumHelpers::MeteorPingPong& state = instState.meteorPingPong;
        state.lastCastTime = getMSTime();

        bool const tankMeteor = mark->GetExactDist2d(RS_HALION_TANK_POSITION.GetPositionX(), RS_HALION_TANK_POSITION.GetPositionY()) <= 10.0f;
        bool const tankEscapeMeteor = mark->GetExactDist2d(RS_HALION_TANK_METEOR_SPOT.GetPositionX(), RS_HALION_TANK_METEOR_SPOT.GetPositionY()) <= 10.0f;
        if (tankMeteor)
            state.tankMeteorTime = getMSTime();
        else if (tankEscapeMeteor)
            state.tankReturnTime = getMSTime();
        else
            ++state.count;
    }

    void HandleHalionRoot(Creature* creature)
    {
        uint32 const instanceId = creature->GetMap()->GetInstanceId();

        bool const fightActive = creature->IsAlive() && creature->IsInCombat();

        if (!fightActive)
        {
            RubySanctumHelpers::ResetInstance(instanceId);
            return;
        }

        RubySanctumHelpers::RsInstanceState& instState = RubySanctumHelpers::RsState(instanceId);

        RubySanctumHelpers::HalionCorporeality& corp = instState.halionCorporeality;
        corp.physicalIndex = RsHalionReadCorporealityIndex(creature);
        corp.physicalStamp = getMSTime();
        corp.physicalGuid = creature->GetGUID();
        instState.bossHealth = { uint8(creature->GetHealthPct()), getMSTime() };

        // Observation only (corporeality / health snapshot above); no boss root, no player auras.
    }
};

void AddSC_RubySanctumBotScripts()
{
    new RsHalionRootScript();
}
