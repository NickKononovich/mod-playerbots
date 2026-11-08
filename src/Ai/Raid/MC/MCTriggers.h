/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MCTRIGGERS_H
#define PLAYERBOTS_MCTRIGGERS_H

#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Trigger.h"

class McLivingBombDebuffTrigger : public Trigger
{
public:
    McLivingBombDebuffTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc living bomb debuff") {}
    bool IsActive() override;
};

class McBaronGeddonInfernoTrigger : public Trigger
{
public:
    McBaronGeddonInfernoTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc baron geddon inferno") {}
    bool IsActive() override;
};

class McShazzrahRangedTrigger : public Trigger
{
public:
    McShazzrahRangedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc shazzrah ranged") {}
    bool IsActive() override;
};

class McGolemaggMarkBossTrigger : public Trigger
{
public:
    McGolemaggMarkBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg mark boss") {}
    bool IsActive() override;
};

class McGolemaggIsMainTankTrigger : public Trigger
{
public:
    McGolemaggIsMainTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg is main tank") {}
    bool IsActive() override;
};

class McGolemaggIsAssistTankTrigger : public Trigger
{
public:
    McGolemaggIsAssistTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg is assist tank") {}
    bool IsActive() override;
};

class McGolemaggIsHealerTrigger : public Trigger
{
public:
    McGolemaggIsHealerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg is healer") {}
    bool IsActive() override;
};

class McCoreHoundMarkTrigger : public Trigger
{
public:
    McCoreHoundMarkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc core hound mark") {}
    bool IsActive() override;
};

// Standing/swimming in magma (knocked into a lava pool by Ragnaros' Wrath or an Eruption)
class McInLavaTrigger : public Trigger
{
public:
    McInLavaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc in lava") {}
    bool IsActive() override;
};

class McGolemaggMagmaSplashTrigger : public Trigger
{
public:
    McGolemaggMagmaSplashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg magma splash") {}
    bool IsActive() override;
};

// A Lava Spawn (npc_lava_spawn, molten_core.cpp) splits in two 15 s after it enters combat, and again while fewer than
// 16 are near: the dps kill the Lava Spawns before their Firelord, the one longest in combat first. Lava Spawns enter
// combat as they are summoned (by the Firelord or a split), so the oldest summon - the lowest guid of the entry - is
// the one closest to its split, and every bot of the raid picks the same one. Null: none alive among its attackers.
Unit* McOldestLavaSpawn(PlayerbotAI* botAI);

class McLavaSpawnFocusTrigger : public Trigger
{
public:
    McLavaSpawnFocusTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc lava spawn focus") {}
    bool IsActive() override;
};

// Flame Imp packs (20 in formation 56569, 10 more linked within the assistance radius) cast Fire Nova 20602 in combat
// every 4-7 s (their SmartAI): an instant area round the imp, so nobody steps out once it is cast. b344 mc-h 25A
// 996-1029 s: the melee and the healers stood among 30 imps, 30-72 novas each (2.3-3.7k a hit) and died.
struct McFireNova
{
    uint32 imps = 0;            // the imps whose nova reaches the spot
    float hits = 0.0f;          // one nova of each of them on the bot (each imp's next cast before the next heal)
    Unit* deepest = nullptr;    // the imp the spot stands deepest in
    float depth = 0.0f;         // how far inside its reach
};

// The Fire Novas that reach `at` for the bot: the imps among its attackers within the spell's effect radius
// (SpellEffectInfo::CalcRadius at the imp) plus the bot's combat reach; an imp that attacks the bot itself is left
// out (it follows the bot wherever it goes: the tank takes it back).
McFireNova McFireNovaAt(PlayerbotAI* botAI, Position const& at);

// The bot keeps out of `nova`: a member with no melee to do there (ranged, healer) out of every nova; a melee damage
// dealer out of a spot where one nova of every imp reaching it (Leader::HazardVerdict::LandedHit) is its health or
// more (hazard-stay-or-leave: it stays only while it survives them). The tanks hold the imps.
bool McFireNovaKeepsOut(PlayerbotAI* botAI, McFireNova const& nova);

// The imp the damage dealers kill first: an imp on a tank (no threat taken from it) with the least health, so the
// novas fall one by one and the area spells of the aoe strategy take the clump round it; a melee only one whose spot
// it may stand at (McFireNovaKeepsOut). Null: none.
Unit* McFlameImpFocus(PlayerbotAI* botAI);

class McFlameImpFireNovaTrigger : public Trigger
{
public:
    McFlameImpFireNovaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc flame imp fire nova") {}
    bool IsActive() override;
};

class McFlameImpFocusTrigger : public Trigger
{
public:
    McFlameImpFocusTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc flame imp focus") {}
    bool IsActive() override;
};

#endif
