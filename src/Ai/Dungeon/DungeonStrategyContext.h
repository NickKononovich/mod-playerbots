/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONSTRATEGYCONTEXT_H
#define PLAYERBOTS_DUNGEONSTRATEGYCONTEXT_H

#include "ACStrategy.h"
#include "AKStrategy.h"
#include "ANStrategy.h"
#include "ArcStrategy.h"
#include "BFStrategy.h"
#include "BMStrategy.h"
#include "ClassicStrategy.h"
#include "EscortProtect.h"
#include "BrdStrategy.h"
#include "StratStrategy.h"
#include "CoSStrategy.h"
#include "DTKStrategy.h"
#include "FoSStrategy.h"
#include "GDStrategy.h"
#include "HoLStrategy.h"
#include "HoRStrategy.h"
#include "HoSStrategy.h"
#include "MTStrategy.h"
#include "MechStrategy.h"
#include "MgTStrategy.h"
#include "NexStrategy.h"
#include "OCStrategy.h"
#include "PoSStrategy.h"
#include "RampStrategy.h"
#include "SHStrategy.h"
#include "SLStrategy.h"
#include "SPStrategy.h"
#include "SVStrategy.h"
#include "SethStrategy.h"
#include "Strategy.h"
#include "TOCStrategy.h"
#include "UBStrategy.h"
#include "UKStrategy.h"
#include "UPStrategy.h"
#include "VHStrategy.h"

class DungeonStrategyContext : public NamedObjectContext<Strategy>
{
public:
    DungeonStrategyContext() : NamedObjectContext<Strategy>(false, true)
    {
        // Vanilla
        creators["classic-sm"] = &DungeonStrategyContext::classic_sm;           // Scarlet Monastery
        creators["classic-gnomer"] = &DungeonStrategyContext::classic_gnomer;   // Gnomeregan
        creators["classic-mara"] = &DungeonStrategyContext::classic_mara;       // Maraudon
        creators["classic-brs"] = &DungeonStrategyContext::classic_brs;         // Blackrock Spire
        creators["classic-st"] = &DungeonStrategyContext::classic_st;           // Sunken Temple
        creators["classic-zf"] = &DungeonStrategyContext::classic_zf;           // Zul'Farrak
        creators["classic-scholo"] = &DungeonStrategyContext::classic_scholo;   // Scholomance
        creators["classic-strat"] = &DungeonStrategyContext::classic_strat;     // Stratholme
        // "dungeon-escort" lives in DungeonEscortStrategyContext (EscortProtect.h): no sibling of the map strategies
        creators["classic-brd"] = &DungeonStrategyContext::classic_brd;         // Blackrock Depths

        // Burning Crusade
        creators["tbc-ac"] = &DungeonStrategyContext::tbc_ac;           // Auchindoun: Auchenai Crypts
        creators["tbc-seth"] = &DungeonStrategyContext::tbc_seth;       // Auchindoun: Sethekk Halls
        creators["tbc-mech"] = &DungeonStrategyContext::tbc_mech;       // Tempest Keep: The Mechanar
        creators["tbc-ub"] = &DungeonStrategyContext::tbc_ub;           // Coilfang Reservoir: The Underbog
        creators["tbc-mgt"] = &DungeonStrategyContext::tbc_mgt;         // Magisters' Terrace
        creators["tbc-ramp"] = &DungeonStrategyContext::tbc_ramp;       // Hellfire Citadel: Hellfire Ramparts
        creators["tbc-sp"] = &DungeonStrategyContext::tbc_sp;           // Coilfang Reservoir: The Slave Pens
        creators["tbc-bf"] = &DungeonStrategyContext::tbc_bf;           // Hellfire Citadel: The Blood Furnace
        creators["tbc-mt"] = &DungeonStrategyContext::tbc_mt;           // Auchindoun: Mana-Tombs
        creators["tbc-sh"] = &DungeonStrategyContext::tbc_sh;           // Hellfire Citadel: The Shattered Halls
        creators["tbc-sl"] = &DungeonStrategyContext::tbc_sl;           // Auchindoun: Shadow Labyrinth
        creators["tbc-sv"] = &DungeonStrategyContext::tbc_sv;           // Coilfang Reservoir: The Steamvault
        creators["tbc-arc"] = &DungeonStrategyContext::tbc_arc;         // Tempest Keep: The Arcatraz
        creators["tbc-ohb"] = &DungeonStrategyContext::tbc_ohb;         // Caverns of Time: Old Hillsbrad Foothills
        creators["tbc-bm"] = &DungeonStrategyContext::tbc_bm;           // Caverns of Time: The Black Morass

        // Wrath of the Lich King
        creators["wotlk-uk"] = &DungeonStrategyContext::wotlk_uk;       // Utgarde Keep
        creators["wotlk-nex"] = &DungeonStrategyContext::wotlk_nex;     // The Nexus
        creators["wotlk-an"] = &DungeonStrategyContext::wotlk_an;       // Azjol-Nerub
        creators["wotlk-ok"] = &DungeonStrategyContext::wotlk_ok;       // Ahn'kahet: The Old Kingdom
        creators["wotlk-dtk"] = &DungeonStrategyContext::wotlk_dtk;     // Drak'Tharon Keep
        creators["wotlk-vh"] = &DungeonStrategyContext::wotlk_vh;       // The Violet Hold
        creators["wotlk-gd"] = &DungeonStrategyContext::wotlk_gd;       // Gundrak
        creators["wotlk-hos"] = &DungeonStrategyContext::wotlk_hos;     // Halls of Stone
        creators["wotlk-hol"] = &DungeonStrategyContext::wotlk_hol;     // Halls of Lightning
        creators["wotlk-occ"] = &DungeonStrategyContext::wotlk_occ;     // The Oculus
        creators["wotlk-up"] = &DungeonStrategyContext::wotlk_up;       // Utgarde Pinnacle
        creators["wotlk-cos"] = &DungeonStrategyContext::wotlk_cos;     // The Culling of Stratholme
        creators["wotlk-toc"] = &DungeonStrategyContext::wotlk_toc;     // Trial of the Champion
        creators["wotlk-pos"] = &DungeonStrategyContext::wotlk_pos;     // Pit of Saron
        creators["wotlk-fos"] = &DungeonStrategyContext::wotlk_fos;     // The Forge of Souls
        creators["wotlk-hor"] = &DungeonStrategyContext::wotlk_hor;     // Halls of Reflection
    }

private:
    static Strategy* classic_sm(PlayerbotAI* botAI) { return new ClassicDungeonScarletMonasteryStrategy(botAI); }
    static Strategy* classic_gnomer(PlayerbotAI* botAI) { return new ClassicDungeonGnomereganStrategy(botAI); }
    static Strategy* classic_mara(PlayerbotAI* botAI) { return new ClassicDungeonMaraudonStrategy(botAI); }
    static Strategy* classic_brs(PlayerbotAI* botAI) { return new ClassicDungeonBlackrockSpireStrategy(botAI); }
    static Strategy* classic_st(PlayerbotAI* botAI) { return new ClassicDungeonSunkenTempleStrategy(botAI); }
    static Strategy* classic_zf(PlayerbotAI* botAI) { return new ClassicDungeonZulFarrakStrategy(botAI); }
    static Strategy* classic_scholo(PlayerbotAI* botAI) { return new ClassicDungeonScholomanceStrategy(botAI); }
    static Strategy* classic_strat(PlayerbotAI* botAI) { return new ClassicDungeonStratholmeStrategy(botAI); }
    static Strategy* classic_brd(PlayerbotAI* botAI) { return new ClassicDungeonBlackrockDepthsStrategy(botAI); }
    static Strategy* tbc_ohb(PlayerbotAI* botAI) { return new TbcDungeonOldHillsbradStrategy(botAI); }
    static Strategy* tbc_bm(PlayerbotAI* botAI) { return new TbcDungeonBlackMorassStrategy(botAI); }
    static Strategy* tbc_ac(PlayerbotAI* botAI) { return new TbcDungeonAuchenaiCryptsStrategy(botAI); }
    static Strategy* tbc_seth(PlayerbotAI* botAI) { return new TbcDungeonSethekkHallsStrategy(botAI); }
    static Strategy* tbc_mech(PlayerbotAI* botAI) { return new TbcDungeonMechanarStrategy(botAI); }
    static Strategy* tbc_ub(PlayerbotAI* botAI) { return new TbcDungeonUnderbogStrategy(botAI); }
    static Strategy* tbc_mgt(PlayerbotAI* botAI) { return new TbcDungeonMagistersTerraceStrategy(botAI); }
    static Strategy* tbc_ramp(PlayerbotAI* botAI) { return new TbcDungeonHellfireRampartsStrategy(botAI); }
    static Strategy* tbc_sp(PlayerbotAI* botAI) { return new TbcDungeonSlavePensStrategy(botAI); }
    static Strategy* tbc_bf(PlayerbotAI* botAI) { return new TbcDungeonBloodFurnaceStrategy(botAI); }
    static Strategy* tbc_mt(PlayerbotAI* botAI) { return new TbcDungeonManaTombsStrategy(botAI); }
    static Strategy* tbc_sh(PlayerbotAI* botAI) { return new TbcDungeonShatteredHallsStrategy(botAI); }
    static Strategy* tbc_sl(PlayerbotAI* botAI) { return new TbcDungeonShadowLabyrinthStrategy(botAI); }
    static Strategy* tbc_sv(PlayerbotAI* botAI) { return new TbcDungeonSteamvaultStrategy(botAI); }
    static Strategy* tbc_arc(PlayerbotAI* botAI) { return new TbcDungeonArcatrazStrategy(botAI); }
    static Strategy* wotlk_uk(PlayerbotAI* botAI) { return new WotlkDungeonUKStrategy(botAI); }
    static Strategy* wotlk_nex(PlayerbotAI* botAI) { return new WotlkDungeonNexStrategy(botAI); }
    static Strategy* wotlk_an(PlayerbotAI* botAI) { return new WotlkDungeonANStrategy(botAI); }
    static Strategy* wotlk_ok(PlayerbotAI* botAI) { return new WotlkDungeonOKStrategy(botAI); }
    static Strategy* wotlk_dtk(PlayerbotAI* botAI) { return new WotlkDungeonDTKStrategy(botAI); }
    static Strategy* wotlk_vh(PlayerbotAI* botAI) { return new WotlkDungeonVHStrategy(botAI); }
    static Strategy* wotlk_gd(PlayerbotAI* botAI) { return new WotlkDungeonGDStrategy(botAI); }
    static Strategy* wotlk_hos(PlayerbotAI* botAI) { return new WotlkDungeonHoSStrategy(botAI); }
    static Strategy* wotlk_hol(PlayerbotAI* botAI) { return new WotlkDungeonHoLStrategy(botAI); }
    static Strategy* wotlk_occ(PlayerbotAI* botAI) { return new WotlkDungeonOccStrategy(botAI); }
    static Strategy* wotlk_up(PlayerbotAI* botAI) { return new WotlkDungeonUPStrategy(botAI); }
    static Strategy* wotlk_cos(PlayerbotAI* botAI) { return new WotlkDungeonCoSStrategy(botAI); }
    static Strategy* wotlk_fos(PlayerbotAI* botAI) { return new WotlkDungeonFoSStrategy(botAI); }
    static Strategy* wotlk_pos(PlayerbotAI* botAI) { return new WotlkDungeonPoSStrategy(botAI); }
    static Strategy* wotlk_toc(PlayerbotAI* botAI) { return new WotlkDungeonToCStrategy(botAI); }
    static Strategy* wotlk_hor(PlayerbotAI* botAI) { return new WotlkDungeonHoRStrategy(botAI); }
};

#endif
