/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include <array>
#include <utility>

namespace
{
// Telegraphed point-blank casts of dungeon bosses and trash. A bot in the middle of its own cast does not run its
// engine until that cast ends (PlayerbotAI::UpdateAIInternal), so a healer finishing a 3 s heal never starts the
// escape the encounter strategy queued; the bots in reach drop their cast when the telegraph starts instead.
constexpr std::array<std::pair<uint32, float>, 18> TELEGRAPHED_AOE = {{
    {68989, 15.0f},  // Pit of Saron, Ick: Poison Nova (5 s)
    {70434, 15.0f},  // Poison Nova, heroic
    {69582, 20.0f},  // Pit of Saron, Plagueborn Horror: Blight Bomb (5 s, on death)
    {52960, 20.0f},  // Halls of Lightning, Loken: Lightning Nova (5 s)
    {59835, 20.0f},  // Lightning Nova, heroic (4 s)
    {9435, 30.0f},   // Scarlet Monastery, Arcanist Doan: Detonation (5 s)
    {36837, 0.0f},   // Arcatraz, Gargantuan Abyssal: Meteor (3 s, split damage: everyone in 35 yd stacks)
    // Utgarde Keep, Ingvar: Smash / Dark Smash (3 s; 10 yd cone plus everything inside his boundary)
    {42669, 10.0f},
    {59706, 10.0f},
    {42723, 10.0f},
    {59709, 10.0f},
    {68899, 25.0f},  // Forge of Souls, Devourer of Souls: Wailing Souls (3 s, then a 104 degree beam sweeps 87 degrees)
    // Utgarde Keep, Ingvar: Staggering Roar / Dreadful Roar (2 s, 60 yd, not kickable) also lock the school of a
    // spell in progress for 6 / 8 s (SPELL_EFFECT_INTERRUPT_CAST): a healer mid-heal loses its heals for 8 s.
    {42708, 60.0f},
    {59708, 60.0f},
    {42729, 60.0f},
    {59734, 60.0f},
    // Halls of Stone, Unrelenting Construct: Short Circuit (3 s, 10 yd, the construct dies after it)
    {51832, 10.0f},
    {61513, 10.0f},
}};

// Thrown ground AoE with a cast bar: the bots near the impact point drop their cast so the generic "avoid aoe"
// (GetSpellAoe reads the spell's destination) can walk them out before it lands.
constexpr std::array<std::pair<uint32, float>, 1> TELEGRAPHED_DEST_AOE = {{
    {40062, 5.0f},  // Blood Furnace, Shadowmoon Technician: Throw Dynamite (1 s, 5 yd at the target point)
}};

constexpr float INTERRUPT_MARGIN = 4.0f;
constexpr float METEOR_JOIN_RANGE = 35.0f;
}  // namespace

class DungeonTelegraphedAoeSpellListenerScript : public AllSpellScript
{
public:
    DungeonTelegraphedAoeSpellListenerScript() : AllSpellScript("DungeonTelegraphedAoeSpellListenerScript") {}

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!caster || !caster->IsCreature() || !caster->GetMap() || !caster->GetMap()->IsDungeon())
            return;

        float radius = -1.0f;
        for (auto const& entry : TELEGRAPHED_AOE)
            if (entry.first == spellInfo->Id)
                radius = entry.second > 0.0f ? entry.second : METEOR_JOIN_RANGE;

        // Ground-targeted ones are centred on their destination, not on the caster.
        WorldObject const* centre = caster;
        Position dest;
        for (auto const& entry : TELEGRAPHED_DEST_AOE)
        {
            if (entry.first != spellInfo->Id || !spell || !spell->m_targets.HasDst() || !spell->m_targets.GetDstPos())
                continue;

            dest = spell->m_targets.GetDstPos()->GetPosition();
            radius = entry.second;
            centre = nullptr;
        }

        if (radius < 0.0f)
            return;

        Map::PlayerList const& players = caster->GetMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* player = itr->GetSource();
            if (!player || !player->IsAlive() || !player->IsNonMeleeSpellCast(false))
                continue;

            float const distance = centre ? centre->GetExactDist2d(player) : player->GetExactDist2d(dest);
            if (distance > radius + player->GetCombatReach() + INTERRUPT_MARGIN)
                continue;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
                botAI->RequestSpellInterrupt();
        }
    }
};

void AddSC_DungeonBotScripts() { new DungeonTelegraphedAoeSpellListenerScript(); }
