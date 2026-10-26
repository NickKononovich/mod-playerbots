/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ATTACKERSVALUE_H
#define PLAYERBOTS_ATTACKERSVALUE_H

#include "PlayerbotAIConfig.h"
#include "Value.h"

class Group;
class Player;
class PlayerbotAI;
class Unit;

class AttackersValue : public ObjectGuidListCalculatedValue
{
public:
    AttackersValue(PlayerbotAI* botAI) : ObjectGuidListCalculatedValue(botAI, "attackers", 1 * IN_MILLISECONDS) {}

    GuidVector Calculate();
    static bool IsPossibleTarget(Unit* attacker, Player* bot, float range = sPlayerbotAIConfig.SightDistance);
    static bool IsValidTarget(Unit* attacker, Player* bot);
    // A valid target out of the bot's sight that it keeps anyway: a dungeon boss its group fights, or a mob in
    // combat with a member of its group close by on its floor ([rjoin-oos]); the reach actions bring it into view.
    static bool IsFoughtOutOfSight(Unit* attacker, Player* bot);
    // Diagnostics: the first check of IsPossibleTarget the unit fails, nullptr when it passes (no side effects).
    static char const* PossibleTargetRejection(Unit* attacker, Player* bot);
    // A critter that flees instead of fighting (hit by an AoE, startled) and is not needed for a quest: not worth
    // chasing, and never a reason to leave the formation.
    static bool IsIgnoredCritter(Unit* unit, Player* bot);
    // [b44_st] A player of the bot's group or team that something has charmed (Hex of Jammal'an, Mind Control,
    // Incite Chaos): hostile while it lasts, but a friend - never a target, only controlled without damage.
    static bool IsCharmedAlly(Unit* unit, Player* bot);
    // [b44_st] The nearest charmed member of the bot's group within `range` that nothing holds yet (polymorphed,
    // stunned, feared, confused, pacified), or nullptr.
    static Player* FindCharmedAlly(Player* bot, float range);

private:
    void AddAttackersOf(Group* group, std::unordered_set<Unit*>& targets);
    void AddAttackersOf(Player* player, std::unordered_set<Unit*>& targets);
    void RemoveNonThreating(std::unordered_set<Unit*>& targets);
    void AddMeleeAttackersOfGroup(std::unordered_set<Unit*>& targets);
    void AddCombatOnlyRefs(std::unordered_set<Unit*>& targets);  // [b30_stalls]
    bool hasRealThreat(Unit* attacker);
};

class PossibleAddsValue : public BoolCalculatedValue
{
public:
    PossibleAddsValue(PlayerbotAI* botAI, std::string const name = "possible adds") : BoolCalculatedValue(botAI, name)
    {
    }

    bool Calculate() override;
};

class PrioritizedTargetsValue : public ManualSetValue<GuidVector>
{
public:
    PrioritizedTargetsValue(PlayerbotAI* botAI, std::string const name = "prioritized targets")
        : ManualSetValue(botAI, GuidVector(), name)
    {
    }
};

#endif
