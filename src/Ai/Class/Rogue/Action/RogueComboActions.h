/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROGUECOMBOACTIONS_H
#define PLAYERBOTS_ROGUECOMBOACTIONS_H

#include "GenericSpellActions.h"

class PlayerbotAI;

class CastComboAction : public CastMeleeSpellAction
{
public:
    CastComboAction(PlayerbotAI* botAI, std::string const name) : CastMeleeSpellAction(botAI, name) {}

    bool isUseful() override;
};

// Combo builder that holds back when it would starve an imminent Slice and Dice refresh
class CastRogueBuilderAction : public CastSpellAction
{
public:
    CastRogueBuilderAction(PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

    bool isUseful() override;
};

class CastSinisterStrikeAction : public CastRogueBuilderAction
{
public:
    CastSinisterStrikeAction(PlayerbotAI* botAI) : CastRogueBuilderAction(botAI, "sinister strike") {}
};

class CastMutilateAction : public CastRogueBuilderAction
{
public:
    CastMutilateAction(PlayerbotAI* botAI) : CastRogueBuilderAction(botAI, "mutilate") {}

    // [melee_audit_b30] pools energy for Mutilate instead of falling through to Sinister Strike
    bool isUseful() override;
};

class CastHemorrhageAction : public CastRogueBuilderAction
{
public:
    CastHemorrhageAction(PlayerbotAI* botAI) : CastRogueBuilderAction(botAI, "hemorrhage") {}
};

class CastRiposteAction : public CastSpellAction
{
public:
    CastRiposteAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "riposte") {}
};

class CastGougeAction : public CastSpellAction
{
public:
    CastGougeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "gouge") {}
};

class CastBackstabAction : public CastSpellAction
{
public:
    CastBackstabAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "backstab") {}
};

#endif
