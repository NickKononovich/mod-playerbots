/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidUtilityAssignment.h"

#include "AiFactory.h"
#include "AiObjectContext.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "ScriptedCreature.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "Timer.h"
#include "Trigger.h"
#include "Value.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <set>
#include <vector>

namespace ai::raidutil
{
namespace
{
    constexpr uint32 ASSIGNMENT_CACHE_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 BATTLE_RES_CACHE_MS = 1 * IN_MILLISECONDS;
    constexpr uint32 INNERVATE_CACHE_MS = 2 * IN_MILLISECONDS;
    constexpr uint32 THREAT_REDIRECT_CACHE_MS = 1 * IN_MILLISECONDS;
    constexpr uint32 UTILITY_BUFF_CACHE_MS = 1 * IN_MILLISECONDS;
    constexpr uint32 HAND_OF_SALVATION_CACHE_MS = 1 * IN_MILLISECONDS;
    constexpr uint32 PULL_IMMINENT_CACHE_MS = 1 * IN_MILLISECONDS;
    constexpr uint32 LOW_MANA_HEALER_CACHE_MS = 2 * IN_MILLISECONDS;

    constexpr uint32 BOSS_ENGAGE_MIN_SECONDS = 2;
    constexpr float BOSS_ENGAGED_HEALTH_PCT = 97.0f;
    constexpr float PULL_IMMINENT_RANGE = 40.0f;
    constexpr float PARTY_TOTEM_RANGE = 30.0f;
    constexpr float RAID_HEALER_RANGE = 40.0f;
    constexpr float HAND_RANGE = 30.0f;
    constexpr float SALVATION_THREAT_RATIO = 0.9f;
    constexpr uint8 MIN_PHYSICAL_FOR_WEAKNESS = 2;

    constexpr uint32 SPELL_EARTH_AND_MOON_TALENT[] = { 48506, 48510, 48511 };
    constexpr uint32 SPELL_EBON_PLAGUEBRINGER_TALENT[] = { 51099, 51160, 51161 };
    constexpr uint32 SPELL_IMPROVED_ICY_TALONS = 55610;
    constexpr uint32 SPELL_HORN_OF_WINTER[] = { 57330, 57623 };

    enum class UtilityRole : uint8
    {
        TANK,
        HEALER,
        CASTER,
        MELEE,
        HUNTER
    };

    struct MemberInfo
    {
        Player* player = nullptr;
        ObjectGuid guid;
        uint8 classId = 0;
        int tab = 0;
        bool isBot = false;
        UtilityRole role = UtilityRole::MELEE;
        uint8 subGroup = 0;
    };

    bool HasAnySpell(Player* player, uint32 const* spells, size_t count)
    {
        for (size_t index = 0; index < count; ++index)
        {
            if (player->HasSpell(spells[index]))
                return true;
        }

        return false;
    }

    // [b35_bal] Passive talent ranks (Earth and Moon, Ebon Plaguebringer, Improved Icy Talons) never enter the spell
    // map (Player::LearnTalent learns only non-passive spell-book talents), so HasSpell() is always false for them.
    bool HasAnyTalent(Player* player, uint32 const* ranks, size_t count)
    {
        uint8 const spec = player->GetActiveSpec();
        for (size_t index = 0; index < count; ++index)
        {
            if (player->HasTalent(ranks[index], spec))
                return true;
        }

        return false;
    }

    UtilityRole ResolveRole(Player* player, uint8 classId, int tab)
    {
        if (PlayerbotAI::IsTank(player))
            return UtilityRole::TANK;
        if (PlayerbotAI::IsHeal(player))
            return UtilityRole::HEALER;

        switch (classId)
        {
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                return UtilityRole::CASTER;
            case CLASS_PRIEST:
                return UtilityRole::CASTER;
            case CLASS_SHAMAN:
                return tab == SHAMAN_TAB_ENHANCEMENT ? UtilityRole::MELEE : UtilityRole::CASTER;
            case CLASS_DRUID:
                return tab == DRUID_TAB_BALANCE ? UtilityRole::CASTER : UtilityRole::MELEE;
            case CLASS_HUNTER:
                return UtilityRole::HUNTER;
            default:
                return UtilityRole::MELEE;
        }
    }

    uint32 GroupKey(Player* bot)
    {
        Group* group = bot->GetGroup();
        return group ? static_cast<uint32>(group->GetLeaderGUID().GetCounter()) : 0;
    }

    // Members on the bot's own map instance. Cross-bot reads are limited to them: the same map is updated by
    // the same thread. Sorted by GUID so every bot builds the same list.
    std::vector<MemberInfo> GatherMembers(Player* bot)
    {
        std::vector<MemberInfo> members;
        Group* group = bot->GetGroup();
        if (!group)
            return members;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld() || member->GetMap() != bot->GetMap() || member->IsGameMaster())
                continue;

            MemberInfo info;
            info.player = member;
            info.guid = member->GetGUID();
            info.classId = member->getClass();
            info.tab = AiFactory::GetPlayerSpecTab(member);
            info.isBot = GET_PLAYERBOT_AI(member) != nullptr;
            info.role = ResolveRole(member, info.classId, info.tab);
            info.subGroup = member->GetSubGroup();
            members.push_back(info);
        }

        std::sort(members.begin(), members.end(),
                  [](MemberInfo const& left, MemberInfo const& right) { return left.guid < right.guid; });
        return members;
    }

    // ---- curses / armor ------------------------------------------------------------------------------------

    int WarlockElementsOrder(MemberInfo const& warlock)
    {
        switch (warlock.tab)
        {
            case WARLOCK_TAB_DESTRUCTION: return 0;
            case WARLOCK_TAB_DEMONOLOGY:  return 1;
            default:                      return 2;
        }
    }

    void AssignCursesAndArmor(Player* bot, std::vector<MemberInfo> const& members, RaidUtilityAssignment& out)
    {
        std::vector<MemberInfo const*> warlocks;
        bool elementsCovered = false;
        bool anyDruid = false;
        uint32 casterCount = 0;
        uint32 physicalCount = 0;

        for (MemberInfo const& member : members)
        {
            if (member.classId == CLASS_WARLOCK && member.isBot)
                warlocks.push_back(&member);

            if (member.classId == CLASS_DRUID)
            {
                anyDruid = true;
                // [b35_bal] talent map, not spell map (passive talent)
                if (HasAnyTalent(member.player, SPELL_EARTH_AND_MOON_TALENT, std::size(SPELL_EARTH_AND_MOON_TALENT)))
                    elementsCovered = true;
            }

            if (member.classId == CLASS_DEATH_KNIGHT &&
                HasAnyTalent(member.player, SPELL_EBON_PLAGUEBRINGER_TALENT,
                             std::size(SPELL_EBON_PLAGUEBRINGER_TALENT)))  // [b35_bal]
            {
                elementsCovered = true;
            }

            if (member.role == UtilityRole::CASTER)
                ++casterCount;
            if (member.role == UtilityRole::MELEE || member.role == UtilityRole::HUNTER)
                ++physicalCount;
        }

        ObjectGuid weaknessOwner;
        if (!warlocks.empty())
        {
            out.curseCoordinated = true;

            std::stable_sort(warlocks.begin(), warlocks.end(),
                             [](MemberInfo const* left, MemberInfo const* right)
                             { return WarlockElementsOrder(*left) < WarlockElementsOrder(*right); });

            bool const needElements = !elementsCovered && casterCount > 0;
            // Curse of Weakness carries the minor armor debuff; Faerie Fire already provides it.
            bool const needWeakness = !anyDruid && physicalCount >= MIN_PHYSICAL_FOR_WEAKNESS;

            size_t index = 0;
            for (MemberInfo const* warlock : warlocks)
            {
                AssignedCurse curse = AssignedCurse::AGONY;
                if (needElements && index == 0)
                    curse = AssignedCurse::ELEMENTS;
                else if (needWeakness && index == (needElements ? 1u : 0u))
                {
                    curse = AssignedCurse::WEAKNESS;
                    weaknessOwner = warlock->guid;
                }

                if (warlock->player == bot)
                    out.curse = curse;

                ++index;
            }
        }

        // Major armor debuff: a warrior tank sunders as part of its rotation, otherwise one bot warrior, otherwise
        // one bot rogue (Expose Armor). Sunder and Expose do not stack.
        ObjectGuid majorOwner;
        bool majorCovered = false;
        for (MemberInfo const& member : members)
        {
            if (member.classId == CLASS_WARRIOR && member.role == UtilityRole::TANK)
            {
                majorCovered = true;
                majorOwner = member.guid;
                break;
            }
        }
        if (!majorCovered)
        {
            for (MemberInfo const& member : members)
            {
                if (member.classId == CLASS_WARRIOR && member.isBot)
                {
                    majorCovered = true;
                    majorOwner = member.guid;
                    break;
                }
            }
        }
        if (!majorCovered)
        {
            for (MemberInfo const& member : members)
            {
                if (member.classId == CLASS_ROGUE && member.isBot)
                {
                    majorCovered = true;
                    majorOwner = member.guid;
                    break;
                }
            }
        }

        out.armorCoordinated = majorCovered;
        out.majorArmorOwner = majorCovered && majorOwner == bot->GetGUID();

        // Minor armor debuff: Faerie Fire from the first druid (balance first), otherwise Curse of Weakness.
        ObjectGuid minorOwner;
        for (MemberInfo const& member : members)
        {
            if (member.classId == CLASS_DRUID && member.tab == DRUID_TAB_BALANCE)
            {
                minorOwner = member.guid;
                break;
            }
        }
        if (minorOwner.IsEmpty())
        {
            for (MemberInfo const& member : members)
            {
                if (member.classId == CLASS_DRUID && member.role != UtilityRole::HEALER)
                {
                    minorOwner = member.guid;
                    break;
                }
            }
        }
        if (minorOwner.IsEmpty())
            minorOwner = weaknessOwner;

        out.minorArmorOwner = !minorOwner.IsEmpty() && minorOwner == bot->GetGUID();
    }

    // ---- bloodlust / heroism -------------------------------------------------------------------------------

    uint32 KnownLustSpell(Player* player)
    {
        if (player->HasSpell(SPELL_HEROISM))
            return SPELL_HEROISM;
        if (player->HasSpell(SPELL_BLOODLUST))
            return SPELL_BLOODLUST;
        return 0;
    }

    void AssignLust(Player* bot, std::vector<MemberInfo> const& members, RaidUtilityAssignment& out)
    {
        for (MemberInfo const& member : members)
        {
            if (member.classId != CLASS_SHAMAN || !member.isBot)
                continue;

            uint32 const spellId = KnownLustSpell(member.player);
            if (!spellId)
                continue;

            out.lustCoordinated = true;
            if (!member.player->IsAlive() || member.player->HasSpellCooldown(spellId))
                continue;

            // First alive shaman (by GUID) with the spell ready is the only one who casts it.
            out.lustAvailable = true;
            out.lustOwner = member.player == bot;
            out.lustOwnerGuid = member.player->GetGUID();
            return;
        }
    }

    // ---- power infusion / focus magic / hysteria -----------------------------------------------------------

    int PowerInfusionScore(MemberInfo const& member)
    {
        if (member.role == UtilityRole::HEALER)
            return 30;
        if (member.role != UtilityRole::CASTER)
            return 0;

        switch (member.classId)
        {
            case CLASS_MAGE:    return 100;
            case CLASS_WARLOCK: return 95;
            case CLASS_SHAMAN:
            case CLASS_DRUID:   return 90;
            default:            return 80;
        }
    }

    int FocusMagicScore(MemberInfo const& member)
    {
        if (member.role == UtilityRole::HEALER)
            return 40;
        if (member.role != UtilityRole::CASTER)
            return 0;

        switch (member.classId)
        {
            case CLASS_MAGE:    return 100;
            case CLASS_WARLOCK: return 90;
            default:            return 85;
        }
    }

    int HysteriaScore(MemberInfo const& member)
    {
        switch (member.role)
        {
            case UtilityRole::MELEE:
                switch (member.classId)
                {
                    case CLASS_WARRIOR:      return 100;
                    case CLASS_DEATH_KNIGHT:
                    case CLASS_ROGUE:        return 95;
                    case CLASS_DRUID:        return 90;
                    default:                 return 85;
                }
            case UtilityRole::HUNTER: return 60;
            case UtilityRole::TANK:   return 10;
            default:                  return 0;
        }
    }

    void AssignBuffPool(Player* bot, std::vector<MemberInfo> const& members, uint8 providerClass,
                        uint32 providerSpell, int (*score)(MemberInfo const&), RaidUtilityAssignment& out)
    {
        if (bot->getClass() != providerClass)
            return;

        std::set<ObjectGuid> used;
        for (MemberInfo const& provider : members)
        {
            if (provider.classId != providerClass || !provider.isBot || !provider.player->HasSpell(providerSpell))
                continue;

            MemberInfo const* best = nullptr;
            int bestScore = 0;
            for (MemberInfo const& candidate : members)
            {
                if (candidate.guid == provider.guid || used.count(candidate.guid) || !candidate.player->IsAlive())
                    continue;

                int const candidateScore = score(candidate);
                if (candidateScore > bestScore)
                {
                    bestScore = candidateScore;
                    best = &candidate;
                }
            }

            if (!best)
                continue;

            used.insert(best->guid);
            if (provider.player == bot)
            {
                out.buffTarget = best->guid;
                return;
            }
        }
    }

    // ---- shaman totems -------------------------------------------------------------------------------------

    int TotemPreference(MemberInfo const& shaman, std::string const& totem)
    {
        bool const enhancement = shaman.tab == SHAMAN_TAB_ENHANCEMENT;
        bool const restoration = shaman.tab == SHAMAN_TAB_RESTORATION;
        bool const elemental = shaman.tab == SHAMAN_TAB_ELEMENTAL;

        if (totem == "windfury")
            return enhancement ? 3 : 0;
        if (totem == "wrath of air")
            return elemental ? 3 : (restoration ? 2 : 0);
        if (totem == "strength of earth")
            return enhancement ? 3 : 0;
        if (totem == "stoneskin")
            return restoration ? 2 : (elemental ? 1 : 0);
        if (totem == "mana spring")
            return restoration ? 3 : (elemental ? 2 : 0);
        if (totem == "healing stream")
            return enhancement ? 2 : 0;
        if (totem == "wrath")
            return elemental ? 3 : 1;
        if (totem == "flametongue")
            return restoration ? 2 : (elemental ? 1 : 0);
        return 0;
    }

    // Gives each option (in need order) to the free shaman that prefers it most; ties go to the lower GUID.
    void AssignElement(std::vector<MemberInfo const*> const& shamans, std::vector<std::string> const& options,
                       bool (*canUse)(MemberInfo const&, std::string const&), std::vector<std::string>& result)
    {
        std::vector<bool> taken(shamans.size(), false);
        for (std::string const& option : options)
        {
            int bestIndex = -1;
            int bestPreference = -1;
            for (size_t index = 0; index < shamans.size(); ++index)
            {
                if (taken[index] || !canUse(*shamans[index], option))
                    continue;

                int const preference = TotemPreference(*shamans[index], option);
                if (preference > bestPreference)
                {
                    bestPreference = preference;
                    bestIndex = static_cast<int>(index);
                }
            }

            if (bestIndex < 0)
                continue;

            taken[bestIndex] = true;
            result[bestIndex] = option;
        }
    }

    bool CanUseAnyTotem(MemberInfo const& /*shaman*/, std::string const& /*totem*/) { return true; }

    bool CanUseFireTotem(MemberInfo const& shaman, std::string const& totem)
    {
        return totem != "wrath" || shaman.player->HasSpell(SPELL_TOTEM_OF_WRATH_RANK_1);
    }

    void AssignTotems(Player* bot, std::vector<MemberInfo> const& members, RaidUtilityAssignment& out)
    {
        if (bot->getClass() != CLASS_SHAMAN)
            return;

        uint8 const mySubGroup = bot->GetSubGroup();
        std::vector<MemberInfo const*> shamans;
        uint32 casterCount = 0;
        uint32 meleeCount = 0;
        uint32 hunterCount = 0;
        bool raidHasIcyTalons = false;
        bool raidHasHorn = false;
        bool anyTotemOfWrath = false;
        int myIndex = -1;

        for (MemberInfo const& member : members)
        {
            if (member.classId == CLASS_DEATH_KNIGHT)
            {
                // [b35_bal] talent map, not spell map (passive talent)
                if (member.player->HasTalent(SPELL_IMPROVED_ICY_TALONS, member.player->GetActiveSpec()))
                    raidHasIcyTalons = true;
                if (HasAnySpell(member.player, SPELL_HORN_OF_WINTER, std::size(SPELL_HORN_OF_WINTER)))
                    raidHasHorn = true;
            }

            if (member.subGroup != mySubGroup)
                continue;

            if (member.role == UtilityRole::CASTER || member.role == UtilityRole::HEALER)
                ++casterCount;
            else if (member.role == UtilityRole::HUNTER)
                ++hunterCount;
            else
                ++meleeCount;

            if (member.classId == CLASS_SHAMAN && member.isBot)
            {
                if (member.player == bot)
                    myIndex = static_cast<int>(shamans.size());
                if (member.player->HasSpell(SPELL_TOTEM_OF_WRATH_RANK_1))
                    anyTotemOfWrath = true;
                shamans.push_back(&member);
            }
        }

        // One shaman in a party keeps its own totems; the plan only removes duplicates.
        if (shamans.size() < 2 || myIndex < 0)
            return;

        // Air: Wrath of Air for casters/healers, Windfury for melee unless Improved Icy Talons covers melee haste.
        std::vector<std::string> airOptions;
        uint32 const windfuryUsers = raidHasIcyTalons ? 0 : meleeCount;
        if (windfuryUsers > casterCount)
        {
            airOptions.push_back("windfury");
            if (casterCount)
                airOptions.push_back("wrath of air");
        }
        else
        {
            if (casterCount)
                airOptions.push_back("wrath of air");
            if (windfuryUsers)
                airOptions.push_back("windfury");
        }

        // Earth: Strength of Earth (does not stack with Horn of Winter), then Stoneskin.
        std::vector<std::string> earthOptions;
        if (!raidHasHorn && (meleeCount + hunterCount) > 0)
            earthOptions.push_back("strength of earth");
        earthOptions.push_back("stoneskin");

        std::vector<std::string> waterOptions = { "mana spring", "healing stream" };

        // Fire: one spell power totem per party (Totem of Wrath, or Flametongue without it).
        std::vector<std::string> fireOptions;
        if (anyTotemOfWrath)
            fireOptions.push_back("wrath");
        else if (casterCount)
            fireOptions.push_back("flametongue");

        std::vector<std::string> earth(shamans.size());
        std::vector<std::string> fire(shamans.size());
        std::vector<std::string> water(shamans.size());
        std::vector<std::string> air(shamans.size());
        AssignElement(shamans, earthOptions, &CanUseAnyTotem, earth);
        AssignElement(shamans, fireOptions, &CanUseFireTotem, fire);
        AssignElement(shamans, waterOptions, &CanUseAnyTotem, water);
        AssignElement(shamans, airOptions, &CanUseAnyTotem, air);

        out.totems[TOTEM_ELEMENT_EARTH] = earth[myIndex];
        out.totems[TOTEM_ELEMENT_WATER] = water[myIndex];
        out.totems[TOTEM_ELEMENT_AIR] = air[myIndex];
        out.totems[TOTEM_ELEMENT_FIRE] =
            fire[myIndex].empty() && !fireOptions.empty() ? TOTEM_AVOID_FIRE_BUFF : fire[myIndex];
    }

    RaidUtilityAssignment ComputeAssignment(PlayerbotAI* botAI)
    {
        RaidUtilityAssignment out;
        Player* bot = botAI->GetBot();
        out.groupKey = GroupKey(bot);

        if (!bot->GetGroup())
            return out;

        std::vector<MemberInfo> const members = GatherMembers(bot);
        if (members.size() < 2)
            return out;

        AssignCursesAndArmor(bot, members, out);
        AssignLust(bot, members, out);
        AssignBuffPool(bot, members, CLASS_PRIEST, SPELL_POWER_INFUSION, &PowerInfusionScore, out);
        AssignBuffPool(bot, members, CLASS_MAGE, SPELL_FOCUS_MAGIC, &FocusMagicScore, out);
        AssignBuffPool(bot, members, CLASS_DEATH_KNIGHT, SPELL_HYSTERIA, &HysteriaScore, out);
        AssignTotems(bot, members, out);

        out.valid = true;
        return out;
    }

    class RaidUtilityAssignmentValue : public CalculatedValue<RaidUtilityAssignment>
    {
    public:
        RaidUtilityAssignmentValue(PlayerbotAI* botAI)
            : CalculatedValue<RaidUtilityAssignment>(botAI, "raid utility assignment", ASSIGNMENT_CACHE_MS)
        {
        }

    protected:
        RaidUtilityAssignment Calculate() override { return ComputeAssignment(botAI); }
    };

    // ---- battle res queue ----------------------------------------------------------------------------------

    uint32 HighestKnownRank(Player* player, uint32 firstRank)
    {
        uint32 known = 0;
        for (uint32 spellId = firstRank; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
        {
            if (player->HasSpell(spellId))
                known = spellId;
        }

        return known;
    }

    int DeadPriority(Player* player)
    {
        if (PlayerbotAI::IsTank(player))
            return 0;
        if (PlayerbotAI::IsHeal(player))
            return 1;
        return 2;
    }

    class BattleResTargetValue : public UnitCalculatedValue
    {
    public:
        BattleResTargetValue(PlayerbotAI* botAI)
            : UnitCalculatedValue(botAI, "battle res target", BATTLE_RES_CACHE_MS)
        {
        }

    protected:
        Unit* Calculate() override
        {
            uint8 const myClass = bot->getClass();
            if (myClass != CLASS_DRUID && myClass != CLASS_DEATH_KNIGHT)
                return nullptr;

            Group* group = bot->GetGroup();
            if (!group)
                return nullptr;

            struct Resser
            {
                Player* player;
                bool realPlayersOnly;
            };

            std::vector<Resser> ressers;
            std::vector<Player*> dead;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || !member->IsInWorld() || member->GetMap() != bot->GetMap())
                    continue;

                if (!member->IsAlive())
                {
                    // Soulstone / Reincarnation holders raise themselves; a pending request is already handled.
                    if (member->getDeathState() == DeathState::Corpse && !member->isResurrectRequested() &&
                        !member->GetUInt32Value(PLAYER_SELF_RES_SPELL))
                    {
                        dead.push_back(member);
                    }
                    continue;
                }

                if (!GET_PLAYERBOT_AI(member))
                    continue;

                if (member->getClass() == CLASS_DRUID)
                {
                    uint32 const rebirth = HighestKnownRank(member, SPELL_REBIRTH_RANK_1);
                    if (rebirth && !member->HasSpellCooldown(rebirth))
                        ressers.push_back({ member, false });
                }
                else if (member->getClass() == CLASS_DEATH_KNIGHT)
                {
                    // Raise Ally hands the dead player a ghoul to control: only a client-driven player can use it.
                    if (member->HasSpell(SPELL_RAISE_ALLY) && !member->HasSpellCooldown(SPELL_RAISE_ALLY))
                        ressers.push_back({ member, true });
                }
            }

            if (dead.empty() || ressers.empty())
                return nullptr;

            std::sort(dead.begin(), dead.end(),
                      [](Player* left, Player* right)
                      {
                          int const leftPriority = DeadPriority(left);
                          int const rightPriority = DeadPriority(right);
                          if (leftPriority != rightPriority)
                              return leftPriority < rightPriority;
                          return left->GetGUID() < right->GetGUID();
                      });
            // [b32_brez] A tank resser (bear) goes last: it leaves its pack only as a last resort (BearBattleRes), so
            // a lower GUID must not take the only corpse from a resto / cat / balance druid that never casts it.
            std::sort(ressers.begin(), ressers.end(),
                      [](Resser const& left, Resser const& right)
                      {
                          bool const leftTank = PlayerbotAI::IsTank(left.player);
                          bool const rightTank = PlayerbotAI::IsTank(right.player);
                          if (leftTank != rightTank)
                              return rightTank;
                          return left.player->GetGUID() < right.player->GetGUID();
                      });

            std::vector<bool> taken(ressers.size(), false);
            for (Player* target : dead)
            {
                bool const realPlayer = IsRealPlayer(target);
                for (size_t index = 0; index < ressers.size(); ++index)
                {
                    if (taken[index] || (ressers[index].realPlayersOnly && !realPlayer))
                        continue;

                    taken[index] = true;
                    if (ressers[index].player == bot)
                        return target;
                    break;
                }
            }

            return nullptr;
        }
    };

    // ---- innervate -----------------------------------------------------------------------------------------

    class InnervateTargetValue : public UnitCalculatedValue
    {
    public:
        InnervateTargetValue(PlayerbotAI* botAI) : UnitCalculatedValue(botAI, "innervate target", INNERVATE_CACHE_MS)
        {
        }

    protected:
        Unit* Calculate() override
        {
            Group* group = bot->GetGroup();
            if (!group || bot->getClass() != CLASS_DRUID)
                return nullptr;

            std::vector<Player*> healers;
            std::vector<Player*> druids;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMap() != bot->GetMap())
                    continue;

                if (PlayerbotAI::IsHeal(member) && member->getPowerType() == POWER_MANA &&
                    member->GetPowerPct(POWER_MANA) < sPlayerbotAIConfig.MediumMana &&
                    !member->HasAura(SPELL_INNERVATE))
                {
                    healers.push_back(member);
                }

                if (member->getClass() == CLASS_DRUID && GET_PLAYERBOT_AI(member) &&
                    member->HasSpell(SPELL_INNERVATE) && !member->HasSpellCooldown(SPELL_INNERVATE))
                {
                    druids.push_back(member);
                }
            }

            if (healers.empty() || druids.empty())
                return nullptr;

            std::sort(healers.begin(), healers.end(),
                      [](Player* left, Player* right)
                      {
                          float const leftMana = left->GetPowerPct(POWER_MANA);
                          float const rightMana = right->GetPowerPct(POWER_MANA);
                          if (leftMana != rightMana)
                              return leftMana < rightMana;
                          return left->GetGUID() < right->GetGUID();
                      });
            std::sort(druids.begin(), druids.end(),
                      [](Player* left, Player* right) { return left->GetGUID() < right->GetGUID(); });

            for (size_t index = 0; index < druids.size() && index < healers.size(); ++index)
            {
                if (druids[index] == bot)
                    return healers[index];
            }

            return nullptr;
        }
    };

    // ---- threat redirect (Misdirection / Tricks of the Trade) ----------------------------------------------

    class ThreatRedirectTargetValue : public UnitCalculatedValue
    {
    public:
        ThreatRedirectTargetValue(PlayerbotAI* botAI)
            : UnitCalculatedValue(botAI, "threat redirect target", THREAT_REDIRECT_CACHE_MS)
        {
        }

    protected:
        Unit* Calculate() override
        {
            Group* group = bot->GetGroup();
            if (!group)
                return nullptr;

            // The tank that holds the current pull, so a second tank's pack gets the threat of its own pull.
            Unit* target = AI_VALUE(Unit*, "current target");
            if (target && !target->ToPlayer())
            {
                Player* victim = target->GetVictim() ? target->GetVictim()->ToPlayer() : nullptr;
                if (victim && victim != bot && victim->IsAlive() && group->IsMember(victim->GetGUID()) &&
                    PlayerbotAI::IsTank(victim))
                {
                    return victim;
                }
            }

            Unit* mainTank = AI_VALUE(Unit*, "main tank");
            return mainTank != bot ? mainTank : nullptr;
        }
    };

    // ---- assigned buff target ------------------------------------------------------------------------------

    class UtilityBuffTargetValue : public UnitCalculatedValue, public Qualified
    {
    public:
        UtilityBuffTargetValue(PlayerbotAI* botAI)
            : UnitCalculatedValue(botAI, "utility buff target", UTILITY_BUFF_CACHE_MS)
        {
        }

    protected:
        Unit* Calculate() override
        {
            bool const selfFallback = qualifier == "self";
            RaidUtilityAssignment const& assignment = GetAssignment(botAI);
            if (assignment.valid && !assignment.buffTarget.IsEmpty())
            {
                Unit* target = botAI->GetUnit(assignment.buffTarget);
                if (target && target->IsInWorld() && target->IsAlive() && target->GetMap() == bot->GetMap() &&
                    bot->GetDistance(target) <= sPlayerbotAIConfig.SpellDistance)
                {
                    return target;
                }
            }

            return selfFallback ? bot : nullptr;
        }
    };

    // ---- hand of salvation ---------------------------------------------------------------------------------

    class HandOfSalvationTargetValue : public UnitCalculatedValue
    {
    public:
        HandOfSalvationTargetValue(PlayerbotAI* botAI)
            : UnitCalculatedValue(botAI, "hand of salvation target", HAND_OF_SALVATION_CACHE_MS)
        {
        }

    protected:
        Unit* Calculate() override
        {
            Group* group = bot->GetGroup();
            if (!group)
                return nullptr;

            Unit* target = AI_VALUE(Unit*, "current target");
            if (!target || target->ToPlayer() || !target->IsAlive() || !target->CanHaveThreatList())
                return nullptr;

            Unit* victim = target->GetVictim();
            Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
            Unit* tank = victimPlayer && PlayerbotAI::IsTank(victimPlayer) ? victim : AI_VALUE(Unit*, "main tank");
            if (!tank)
                return nullptr;

            ThreatManager& threat = target->GetThreatMgr();
            float const tankThreat = threat.GetThreat(tank);

            Player* best = nullptr;
            float bestRatio = 0.0f;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == tank || !member->IsInWorld() || !member->IsAlive() ||
                    member->GetMap() != bot->GetMap() || PlayerbotAI::IsTank(member) ||
                    bot->GetDistance(member) > HAND_RANGE)
                {
                    continue;
                }

                if (botAI->HasAura("hand of salvation", member))
                    continue;

                float ratio = 0.0f;
                if (victim == member)
                    ratio = 2.0f;  // a non-tank already pulled aggro
                else if (tankThreat > 0.0f)
                    ratio = threat.GetThreat(member) / tankThreat;

                if (ratio >= SALVATION_THREAT_RATIO && ratio > bestRatio)
                {
                    bestRatio = ratio;
                    best = member;
                }
            }

            return best;
        }
    };

    // ---- pull imminent -------------------------------------------------------------------------------------

    class PullImminentValue : public BoolCalculatedValue
    {
    public:
        PullImminentValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "pull imminent", PULL_IMMINENT_CACHE_MS)
        {
        }

    protected:
        bool Calculate() override
        {
            if (!bot->GetGroup() || !bot->IsAlive() || bot->IsInCombat())
                return false;

            Unit* tankUnit = AI_VALUE(Unit*, "main tank");
            Player* tank = tankUnit ? tankUnit->ToPlayer() : nullptr;
            if (!tank || tank == bot || !tank->IsAlive() || tank->GetMap() != bot->GetMap() ||
                bot->GetDistance(tank) > PULL_IMMINENT_RANGE)
            {
                return false;
            }

            Unit* target = tank->GetVictim();
            if (!target && !tank->GetTarget().IsEmpty())
                target = ObjectAccessor::GetUnit(*tank, tank->GetTarget());

            if (!target || target->ToPlayer() || !target->IsAlive() || !bot->IsValidAttackTarget(target) ||
                tank->GetDistance(target) > PULL_IMMINENT_RANGE)
            {
                return false;
            }

            // Outdoors every grind mob would count as a pull: only elites and bosses there.
            if (IsInstanceMap(bot))
                return true;

            Creature* creature = target->ToCreature();
            return creature && (creature->isElite() || IsBossUnit(creature));
        }
    };

    class PullImminentTrigger : public Trigger
    {
    public:
        PullImminentTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pull imminent") {}

        bool IsActive() override { return AI_VALUE(bool, "pull imminent"); }
    };

    // ---- low mana healers ----------------------------------------------------------------------------------

    class LowManaHealerCountValue : public Uint8CalculatedValue, public Qualified
    {
    public:
        LowManaHealerCountValue(PlayerbotAI* botAI)
            : Uint8CalculatedValue(botAI, "low mana healer count", LOW_MANA_HEALER_CACHE_MS)
        {
        }

    protected:
        uint8 Calculate() override
        {
            Group* group = bot->GetGroup();
            if (!group)
                return 0;

            size_t const separator = qualifier.find(':');
            if (separator == std::string::npos)
                return 0;

            bool const partyOnly = qualifier.substr(0, separator) == "party";
            float const manaPct = static_cast<float>(std::atoi(qualifier.substr(separator + 1).c_str()));
            float const range = partyOnly ? PARTY_TOTEM_RANGE : RAID_HEALER_RANGE;

            uint8 count = 0;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMap() != bot->GetMap())
                    continue;
                if (partyOnly && !group->SameSubGroup(bot, member))
                    continue;
                if (member != bot && bot->GetDistance(member) > range)
                    continue;
                if (!PlayerbotAI::IsHeal(member) || member->getPowerType() != POWER_MANA)
                    continue;

                if (member->GetPowerPct(POWER_MANA) < manaPct)
                    ++count;
            }

            return count;
        }
    };
}

bool IsBossUnit(Unit* unit)
{
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    if (!creature)
        return false;

    if (creature->isWorldBoss() || creature->IsDungeonBoss())
        return true;

    return dynamic_cast<BossAI*>(creature->AI()) != nullptr;
}

bool HasLustAura(Unit* unit) { return unit && (unit->HasAura(SPELL_BLOODLUST) || unit->HasAura(SPELL_HEROISM)); }

bool HasLustDebuff(Unit* unit) { return unit && (unit->HasAura(SPELL_SATED) || unit->HasAura(SPELL_EXHAUSTION)); }

bool IsInstanceMap(Player* bot)
{
    Map* map = bot ? bot->GetMap() : nullptr;
    return map && map->IsDungeon();
}

uint32 GetCombatSeconds(PlayerbotAI* botAI)
{
    // "combat start time" is kept only by the "wait for attack" strategy: without the fallback every fight read
    // 0 s, so no boss was ever "engaged" (Heroism never cast) and burst cooldowns were held for lust all fight.
    AiObjectContext* context = botAI->GetAiObjectContext();
    time_t start = context->GetValue<time_t>("combat start time")->Get();
    if (!start)
        start = context->GetValue<time_t>("pull grace start")->Get();
    if (!start)
        return 0;

    time_t const now = time(nullptr);
    return now > start ? static_cast<uint32>(now - start) : 0;
}

bool IsBossEngaged(PlayerbotAI* botAI, Unit* target)
{
    if (!target || !target->IsAlive() || !target->IsInCombat() || !IsBossUnit(target))
        return false;

    if (GetCombatSeconds(botAI) < BOSS_ENGAGE_MIN_SECONDS)
        return false;

    Unit* victim = target->GetVictim();
    Player* victimPlayer = victim ? victim->ToPlayer() : nullptr;
    if (victimPlayer && PlayerbotAI::IsTank(victimPlayer))
        return true;

    return target->GetHealthPct() < BOSS_ENGAGED_HEALTH_PCT;
}

// The leader ordered a burst ("burst [seconds]", LeaderActions): a burn phase now, trash or boss.
static bool IsOrderedBurst(PlayerbotAI* botAI)
{
    uint32 const until = botAI->GetAiObjectContext()->GetValue<uint32>("burst until")->Get();
    return until && int32(until - getMSTime()) > 0 && botAI->GetBot()->IsInCombat();
}

bool ShouldCastLust(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (HasLustAura(bot) || HasLustDebuff(bot))
        return false;

    AiObjectContext* context = botAI->GetAiObjectContext();
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (!target || !target->IsAlive())
        return false;

    if (target->ToPlayer())
        return true;

    RaidUtilityAssignment const& assignment = GetAssignment(botAI);
    if (assignment.valid && assignment.lustCoordinated && !assignment.lustOwner)
        return false;

    if (IsOrderedBurst(botAI))
        return true;

    // Instances: once per boss pull (5 min cooldown, 10 min Sated/Exhaustion), never on trash.
    if (IsInstanceMap(bot))
        return IsBossEngaged(botAI, target);

    // Outdoors / solo: a world boss, or the old "losing this fight" heuristic.
    if (IsBossUnit(target))
        return IsBossEngaged(botAI, target);

    return context->GetValue<uint8>("balance")->Get() <= 50;
}

bool IsBurstWindow(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (HasLustAura(bot) || IsOrderedBurst(botAI))
        return true;

    if (!IsInstanceMap(bot))
        return false;

    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!IsBossEngaged(botAI, target))
        return false;

    // Boss pull without a lust to wait for.
    RaidUtilityAssignment const& assignment = GetAssignment(botAI);
    return !assignment.lustAvailable || HasLustDebuff(bot);
}

bool ShouldHoldBurstForLust(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!IsInstanceMap(bot) || HasLustAura(bot) || HasLustDebuff(bot) || IsOrderedBurst(botAI))
        return false;

    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!IsBossUnit(target))
        return false;

    RaidUtilityAssignment const& assignment = GetAssignment(botAI);
    if (!assignment.valid || !assignment.lustAvailable)
        return false;

    // [rot9] no clock: the hold lasts only while the shaman who casts the lust can cast it now (alive in this fight,
    // on a boss, neither stunned nor silenced, the mana for it)
    Player* owner = ObjectAccessor::FindPlayer(assignment.lustOwnerGuid);
    if (!owner || !owner->IsInWorld() || owner->GetMap() != bot->GetMap() || !owner->IsAlive() || !owner->IsInCombat())
        return false;
    if (owner->HasUnitState(UNIT_STATE_STUNNED) || owner->HasAuraType(SPELL_AURA_MOD_SILENCE) ||
        owner->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE))
        return false;
    PlayerbotAI* ownerAI = GET_PLAYERBOT_AI(owner);
    if (!ownerAI || !IsBossUnit(ownerAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get()))
        return false;

    SpellInfo const* lust = sSpellMgr->GetSpellInfo(KnownLustSpell(owner));
    return lust &&
           owner->GetPower(POWER_MANA) >= uint32(std::max(0, lust->CalcPowerCost(owner, lust->GetSchoolMask())));
}

std::string CurseSpellName(AssignedCurse curse)
{
    switch (curse)
    {
        case AssignedCurse::ELEMENTS: return "curse of the elements";
        case AssignedCurse::TONGUES:  return "curse of tongues";
        case AssignedCurse::WEAKNESS: return "curse of weakness";
        case AssignedCurse::AGONY:    return "curse of agony";
        default:                      return "";
    }
}

RaidUtilityAssignment const& GetAssignment(PlayerbotAI* botAI)
{
    static RaidUtilityAssignment const empty;

    AiObjectContext* context = botAI->GetAiObjectContext();
    Value<RaidUtilityAssignment>* value = context->GetValue<RaidUtilityAssignment>("raid utility assignment");
    if (!value)
        return empty;

    RaidUtilityAssignment& assignment = value->RefGet();
    if (assignment.groupKey != GroupKey(botAI->GetBot()))
    {
        value->Reset();
        return value->RefGet();
    }

    return assignment;
}

std::string LowManaHealerQualifier(bool partyOnly, uint32 manaPct)
{
    return std::string(partyOnly ? "party:" : "raid:") + std::to_string(manaPct);
}

UntypedValue* CreateRaidUtilityAssignmentValue(PlayerbotAI* botAI) { return new RaidUtilityAssignmentValue(botAI); }
UntypedValue* CreateBattleResTargetValue(PlayerbotAI* botAI) { return new BattleResTargetValue(botAI); }
UntypedValue* CreateInnervateTargetValue(PlayerbotAI* botAI) { return new InnervateTargetValue(botAI); }
UntypedValue* CreateThreatRedirectTargetValue(PlayerbotAI* botAI) { return new ThreatRedirectTargetValue(botAI); }
UntypedValue* CreateUtilityBuffTargetValue(PlayerbotAI* botAI) { return new UtilityBuffTargetValue(botAI); }
UntypedValue* CreateHandOfSalvationTargetValue(PlayerbotAI* botAI) { return new HandOfSalvationTargetValue(botAI); }
UntypedValue* CreatePullImminentValue(PlayerbotAI* botAI) { return new PullImminentValue(botAI); }
UntypedValue* CreateLowManaHealerCountValue(PlayerbotAI* botAI) { return new LowManaHealerCountValue(botAI); }
Trigger* CreatePullImminentTrigger(PlayerbotAI* botAI) { return new PullImminentTrigger(botAI); }
}
