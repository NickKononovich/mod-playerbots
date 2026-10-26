/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BMVALUECONTEXT_H
#define PLAYERBOTS_BMVALUECONTEXT_H

#include "BMShared.h"
#include "NamedObjectContext.h"
#include "ObjectGuid.h"
#include "Value.h"

// Infinite adds within BMShared::THREAT_RADIUS of Medivh, the ones draining his shield first. One grid scan around
// Medivh per second while the portals run (the adds walk ~7 yd/s, one comes every 15 s per rift).
class BlackMorassMedivhThreatsValue : public CalculatedValue<GuidVector>
{
public:
    BlackMorassMedivhThreatsValue(PlayerbotAI* botAI) : CalculatedValue<GuidVector>(botAI, "bm medivh threats", 1000)
    {
    }

protected:
    GuidVector Calculate() override { return BMShared::ScanThreats(bot); }
};

// The adds of "bm medivh threats" channeling Corrupt Medivh right now: Medivh's auras only, no grid scan. A channel
// breaks on the first hit, so this changes faster than the threat list.
class BlackMorassCorruptingAddsValue : public CalculatedValue<GuidVector>
{
public:
    BlackMorassCorruptingAddsValue(PlayerbotAI* botAI) : CalculatedValue<GuidVector>(botAI, "bm corrupting adds", 500)
    {
    }

protected:
    GuidVector Calculate() override { return BMShared::ScanCorrupting(botAI); }
};

// Living rift keepers and rift bosses within BMShared::GUARDIAN_SCAN_RADIUS of Medivh, nearest to him first.
class BlackMorassRiftGuardiansValue : public CalculatedValue<GuidVector>
{
public:
    BlackMorassRiftGuardiansValue(PlayerbotAI* botAI) : CalculatedValue<GuidVector>(botAI, "bm rift guardians", 2000)
    {
    }

protected:
    GuidVector Calculate() override { return BMShared::ScanGuardians(bot); }
};

class TbcDungeonBlackMorassValueContext : public NamedObjectContext<UntypedValue>
{
public:
    TbcDungeonBlackMorassValueContext()
    {
        creators["bm medivh threats"] = &TbcDungeonBlackMorassValueContext::bm_medivh_threats;
        creators["bm rift guardians"] = &TbcDungeonBlackMorassValueContext::bm_rift_guardians;
        creators["bm corrupting adds"] = &TbcDungeonBlackMorassValueContext::bm_corrupting_adds;
    }

private:
    static UntypedValue* bm_medivh_threats(PlayerbotAI* botAI) { return new BlackMorassMedivhThreatsValue(botAI); }
    static UntypedValue* bm_rift_guardians(PlayerbotAI* botAI) { return new BlackMorassRiftGuardiansValue(botAI); }
    static UntypedValue* bm_corrupting_adds(PlayerbotAI* botAI) { return new BlackMorassCorruptingAddsValue(botAI); }
};

#endif
