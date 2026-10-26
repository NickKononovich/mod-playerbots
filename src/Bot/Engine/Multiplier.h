/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MULTIPLIER_H
#define PLAYERBOTS_MULTIPLIER_H

#include "AiObject.h"

class Action;
class PlayerbotAI;
struct PerformanceData;

class Multiplier : public AiNamedObject
{
public:
    Multiplier(PlayerbotAI* botAI, std::string const name) : AiNamedObject(botAI, name) {}
    virtual ~Multiplier() {}

    virtual float GetValue([[maybe_unused]] Action* action) { return 1.0f; }
    // [b41_mgt] A veto (GetValue 0) the group's no-damage release (Engine, combat_no_damage_release) must not lift.
    virtual bool KeepsVeto([[maybe_unused]] Action* action) { return false; }

    PerformanceData* GetPerfData();
    void NoteBlock();

private:
    PerformanceData* perfData = nullptr;
};

#endif
