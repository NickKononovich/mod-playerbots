/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SurvivalKit.h"
#include "BotPath.h"  // [runpast11] WalkBotPoints

#include "CellImpl.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Bag.h"
#include "Item.h"
#include "LeaderPlannerShared.h"
#include "Log.h"
#include "MovementGenerator.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "RouteSearch.h"  // [deaths] the maze search of a run
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SoloSafety.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    enum class Kind : uint8
    {
        None,
        Reset,      // drops the fight: Vanish, Shadowmeld (sanctuary), Feign Death
        Heal,       // direct / periodic heal on itself, Lay on Hands
        Absorb,     // Power Word: Shield
        Reduce,     // damage taken / dodge / immunity on itself for its duration
        Stun,       // the attackers stopped through damage (Hammer of Justice, War Stomp)
        Silence,    // the casters' spells stopped (Arcane Torrent)
        Window,     // a control that breaks on damage (fears, incapacitates, roots): time for a heal / bandage
        Snare,      // the chaser slowed (Wing Clip, Hamstring, Frost Nova's root counts as speed 0)
        Speed,      // its own speed (Sprint, Dash, Aspect of the Cheetah)
        Leap,       // a jump away (Blink, Disengage)
        Flee
    };

    char const* KindName(Kind kind)
    {
        switch (kind)
        {
            case Kind::Reset: return "reset";
            case Kind::Heal: return "heal";
            case Kind::Absorb: return "absorb";
            case Kind::Reduce: return "damage taken down";
            case Kind::Stun: return "stun";
            case Kind::Silence: return "silence";
            case Kind::Window: return "control window";
            case Kind::Snare: return "snare";
            case Kind::Speed: return "speed";
            case Kind::Leap: return "leap";
            case Kind::Flee: return "flee";
            default: return "none";
        }
    }

    // one piece of the kit as its spell data gives it
    struct Piece
    {
        Kind kind = Kind::None;
        uint32 spell = 0;          // the spell cast (an item's on-use spell for items)
        uint32 item = 0;           // the item entry (potions, healthstones, bandages)
        float amount = 0.0f;       // heal / absorb points
        float pct = 0.0f;          // damage share taken off (Reduce: of the hits it covers), speed change (Snare/Speed)
        bool meleeOnly = false;    // Reduce by dodge, Window by a root: melee only
        bool area = false;         // Stun / Silence / Window around the caster (its radius)
        float radius = 0.0f;
        float durationSec = 0.0f;
        float castSec = 0.0f;      // its cast (channel for bandages) or the global cooldown, the longer
        bool instant = false;      // [deaths12] no cast time (castable on the move: a run past, a flee)
        float cooldownSec = 0.0f;  // its own recovery: shorter cooldowns go first
        float cost = 0.0f;         // mana
        bool needsWindow = false;  // a bandage: channelled, broken by damage
    };

    struct BotKit
    {
        size_t spells = size_t(-1);
        uint8 level = 0;
        std::vector<Piece> pieces;
    };

    struct KitState
    {
        bool active = false;   // a choice is pending for the action
        Piece choice;
        ObjectGuid target;
        std::string why;
        uint32 loggedSpell = 0;
        Kind loggedKind = Kind::None;
        // the run
        bool fleeing = false;
        float fleeAngle = 0.0f;
        ObjectGuid fleeFrom;
        // its pet set passive for the run, its stance before
        bool petPassive = false;
        ReactStates petReact = REACT_DEFENSIVE;
        // a control window opened: its heal follows while the attackers are held
        bool window = false;
        // a reset cast: nothing until the fight is over
        bool reset = false;
        // [guild15] a run found no way out clear of other mobs: no run again this fight (the ground is the same; b321
        // Lanbuzz L8 started and stopped 17 runs on a Frostmane Headhunter L9, each one dropped its attack and pet,
        // and died having fought none of it)
        bool noRun = false;
        // [guild15] the run goes back along the way it came: the next spot back to walk to
        bool fleeTrail = false;
        uint8 trailStep = 0;
        // [deaths] the run goes along a maze search's walk round the other mobs: its corners, the next one to walk to
        std::vector<G3D::Vector3> fleeRoute;
        uint32 routeStep = 0;
        // [deaths] where a maze search found no walk out this fight (searched again once the bot stands elsewhere)
        bool mazeFailed = false;
        G3D::Vector3 mazeFailedAt;
        // [deaths] the fight was logged lost with nothing to use (once a fight: b337 deaths "without rescue" left no
        // line at all - Feeli, Eraelor, Rumanatol, Ginde - and the triage could not tell why)
        bool loggedNone = false;
    };

    std::mutex lock;
    std::unordered_map<uint32, BotKit> kits;
    std::unordered_map<uint32, KitState> states;

    float Sec(int32 ms) { return float(std::max(ms, 0)) / float(IN_MILLISECONDS); }

    float CastSec(Player* bot, SpellInfo const* info)
    {
        float cast = Sec(info->CalcCastTime(bot));
        if (info->IsChanneled())
            cast = std::max(cast, Sec(info->GetDuration()));
        return std::max(cast, Sec(int32(info->StartRecoveryTime)));
    }

    bool Breaks(SpellInfo const* info) { return info->AuraInterruptFlags & AURA_INTERRUPT_FLAG_TAKE_DAMAGE; }

    bool OnEnemy(SpellEffectInfo const& effect)
    {
        Targets const target = effect.TargetA.GetTarget();
        return target == TARGET_UNIT_TARGET_ENEMY || target == TARGET_UNIT_SRC_AREA_ENEMY ||
               target == TARGET_UNIT_DEST_AREA_ENEMY || target == TARGET_UNIT_CONE_ENEMY_104 ||
               target == TARGET_UNIT_CONE_ENEMY_24 || target == TARGET_UNIT_CONE_ENEMY_54;
    }

    bool OnSelf(SpellEffectInfo const& effect)
    {
        Targets const target = effect.TargetA.GetTarget();
        return target == TARGET_UNIT_CASTER || target == TARGET_UNIT_TARGET_ALLY || target == TARGET_UNIT_TARGET_ANY ||
               target == TARGET_UNIT_TARGET_RAID || target == TARGET_UNIT_TARGET_PARTY;
    }

    // the pieces of one spell (the spell itself and, one level down, the spells it triggers on itself)
    void Classify(Player* bot, SpellInfo const* info, uint32 castId, std::vector<Piece>& out, bool nested = false)
    {
        Piece base;
        base.spell = castId;
        base.durationSec = Sec(info->GetDuration());
        base.castSec = CastSec(bot, info);
        base.instant = info->CalcCastTime(bot) <= 0 && !info->IsChanneled();  // [deaths12]
        base.cooldownSec = Sec(int32(std::max(info->RecoveryTime, info->CategoryRecoveryTime)));
        base.cost = info->PowerType == POWER_MANA ? float(info->CalcPowerCost(bot, info->GetSchoolMask())) : 0.0f;
        bool selfStun = false;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_MOD_STUN &&
                effect.TargetA.GetTarget() == TARGET_UNIT_CASTER)
                selfStun = true;

        Piece reduce = base;
        reduce.kind = Kind::Reduce;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_SANCTUARY ||
                (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_FEIGN_DEATH))
            {
                Piece piece = base;
                piece.kind = Kind::Reset;
                out.push_back(piece);
                return;
            }
            if (effect.Effect == SPELL_EFFECT_TRIGGER_SPELL && effect.TriggerSpell && !nested)
                if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                {
                    std::vector<Piece> inner;
                    Classify(bot, triggered, castId, inner, true);
                    for (Piece& piece : inner)
                        if (piece.kind == Kind::Reset)
                        {
                            piece.cooldownSec = base.cooldownSec;
                            piece.castSec = base.castSec;
                            out.push_back(piece);
                            return;
                        }
                }
            // heals on itself
            if (info->IsPositive() && OnSelf(effect))
            {
                Piece piece = base;
                if (effect.Effect == SPELL_EFFECT_HEAL)
                    piece.amount = float(bot->SpellHealingBonusDone(bot, info, uint32(std::max(effect.CalcValue(bot), 0)),
                                                                    HEAL, effect.EffectIndex));
                else if (effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH)
                    piece.amount = float(bot->GetMaxHealth());
                else if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL &&
                         effect.Amplitude > 0)
                    piece.amount = float(bot->SpellHealingBonusDone(bot, info, uint32(std::max(effect.CalcValue(bot), 0)),
                                                                    DOT, effect.EffectIndex)) *
                                   float(std::max(info->GetDuration(), 0) / int32(effect.Amplitude));
                if (piece.amount > 0.0f)
                {
                    piece.kind = Kind::Heal;
                    out.push_back(piece);
                    continue;
                }
            }
            if (effect.Effect != SPELL_EFFECT_APPLY_AURA && effect.Effect != SPELL_EFFECT_LEAP &&
                effect.Effect != SPELL_EFFECT_LEAP_BACK)
                continue;
            if (effect.Effect == SPELL_EFFECT_LEAP && effect.HasRadius())
            {
                Piece piece = base;
                piece.kind = Kind::Leap;
                piece.radius = effect.CalcRadius(bot);
                out.push_back(piece);
                continue;
            }
            if (effect.Effect == SPELL_EFFECT_LEAP_BACK)
            {
                // Spell::EffectLeapBack: horizontal speed MiscValue / 10, vertical damage / 10; its flight under the
                // core's gravity (Movement::gravity 19.29) gives the distance
                float const speedXY = float(effect.MiscValue) / 10.0f;
                float const speedZ = float(effect.CalcValue(bot)) / 10.0f;
                Piece piece = base;
                piece.kind = Kind::Leap;
                piece.radius = speedXY * 2.0f * speedZ / 19.29f;
                if (piece.radius > 0.0f)
                    out.push_back(piece);
                continue;
            }
            int32 const value = effect.CalcValue(bot);
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_SCHOOL_ABSORB:
                    if (OnSelf(effect) && (effect.MiscValue & SPELL_SCHOOL_MASK_NORMAL) && value > 0)
                    {
                        Piece piece = base;
                        piece.kind = Kind::Absorb;
                        piece.amount = float(value);
                        out.push_back(piece);
                    }
                    break;
                case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                    if (OnSelf(effect) && value < 0 && (effect.MiscValue & SPELL_SCHOOL_MASK_NORMAL))
                        reduce.pct = std::max(reduce.pct, std::min(1.0f, -float(value) / 100.0f));
                    break;
                case SPELL_AURA_SCHOOL_IMMUNITY:
                    if (OnSelf(effect) && (effect.MiscValue & SPELL_SCHOOL_MASK_NORMAL))
                        reduce.pct = 1.0f;
                    break;
                case SPELL_AURA_MOD_DODGE_PERCENT:
                    if (OnSelf(effect) && value > 0)
                    {
                        reduce.pct = std::max(reduce.pct, std::min(1.0f, float(value) / 100.0f));
                        reduce.meleeOnly = true;
                    }
                    break;
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_MOD_ROOT:
                case SPELL_AURA_MOD_SILENCE:
                    if (OnEnemy(effect) && base.durationSec > 0.0f)
                    {
                        Piece piece = base;
                        piece.area = effect.TargetA.GetTarget() != TARGET_UNIT_TARGET_ENEMY;
                        piece.radius = effect.HasRadius() ? effect.CalcRadius(bot) : 0.0f;
                        if (effect.ApplyAuraName == SPELL_AURA_MOD_SILENCE)
                            piece.kind = Kind::Silence;
                        else if (effect.ApplyAuraName == SPELL_AURA_MOD_STUN && !Breaks(info))
                            piece.kind = Kind::Stun;
                        else if (effect.ApplyAuraName != SPELL_AURA_MOD_ROOT)
                            piece.kind = Kind::Window;
                        // a root holds a melee chaser only while it stands out of reach: a run's first piece
                        if (piece.kind != Kind::None)
                            out.push_back(piece);
                        // a root also holds a chaser where it stands
                        if (effect.ApplyAuraName == SPELL_AURA_MOD_ROOT)
                        {
                            Piece snare = piece;
                            snare.kind = Kind::Snare;
                            snare.pct = -1.0f;
                            out.push_back(snare);
                        }
                    }
                    break;
                case SPELL_AURA_MOD_DECREASE_SPEED:
                    if (OnEnemy(effect) && value < 0 && base.durationSec > 0.0f)
                    {
                        Piece piece = base;
                        piece.kind = Kind::Snare;
                        piece.pct = std::max(-1.0f, float(value) / 100.0f);
                        piece.area = effect.TargetA.GetTarget() != TARGET_UNIT_TARGET_ENEMY;
                        piece.radius = effect.HasRadius() ? effect.CalcRadius(bot) : 0.0f;
                        out.push_back(piece);
                    }
                    break;
                case SPELL_AURA_MOD_INCREASE_SPEED:
                case SPELL_AURA_MOD_SPEED_ALWAYS:
                    if (OnSelf(effect) && value > 0)
                    {
                        Piece piece = base;
                        piece.kind = Kind::Speed;
                        piece.pct = float(value) / 100.0f;
                        // a speed aura with no duration lasts (an aspect)
                        if (piece.durationSec <= 0.0f)
                            piece.durationSec = std::numeric_limits<float>::max();
                        out.push_back(piece);
                    }
                    break;
                default:
                    break;
            }
        }
        // a buff that stuns its caster (Ice Block) only pauses the fight: the hits it holds back come after
        if (reduce.pct > 0.0f && !selfStun && reduce.durationSec > 0.0f)
            out.push_back(reduce);
    }

    BotKit const& KitOf(Player* bot)
    {
        BotKit& kit = kits[bot->GetGUID().GetCounter()];
        if (kit.spells == bot->GetSpellMap().size() && kit.level == bot->GetLevel())
            return kit;
        kit.spells = bot->GetSpellMap().size();
        kit.level = bot->GetLevel();
        kit.pieces.clear();
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || info->IsAutoRepeatRangedSpell())
                continue;
            // the highest rank it knows only
            if (SpellInfo const* next = info->GetNextRankSpell())
                if (bot->HasSpell(next->Id))
                    continue;
            Classify(bot, info, spellId, kit.pieces);
        }
        return kit;
    }

    // its heal items now (potions, healthstones, bandages), from item data
    void ItemPieces(Player* bot, std::vector<Piece>& out)
    {
        auto consider = [bot, &out](Item* item)
        {
            if (!item || item->IsInTrade())
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (proto->Class != ITEM_CLASS_CONSUMABLE || bot->CanUseItem(item) != EQUIP_ERR_OK)
                return;
            if (proto->RequiredSkill && bot->GetSkillValue(proto->RequiredSkill) < proto->RequiredSkillRank)
                return;
            for (auto const& slot : proto->Spells)
            {
                if (slot.SpellId <= 0 || slot.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(slot.SpellId));
                if (!info || bot->HasSpellCooldown(uint32(slot.SpellId)))
                    continue;
                std::vector<Piece> pieces;
                Classify(bot, info, uint32(slot.SpellId), pieces, true);
                for (Piece& piece : pieces)
                    if (piece.kind == Kind::Heal)
                    {
                        piece.item = proto->ItemId;
                        piece.cost = 0.0f;
                        piece.needsWindow = info->IsChanneled();
                        piece.cooldownSec = std::max(piece.cooldownSec,
                                                     Sec(std::max<int32>(slot.SpellCooldown, slot.SpellCategoryCooldown)));
                        // an item heal is no spell power heal: its points as the data gives them
                        for (SpellEffectInfo const& effect : info->GetEffects())
                            if (effect.Effect == SPELL_EFFECT_HEAL)
                                piece.amount = float(effect.CalcValue(bot));
                        if (std::none_of(out.begin(), out.end(),
                                         [&piece](Piece const& p) { return p.item == piece.item; }))
                            out.push_back(piece);
                    }
            }
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    consider(container->GetItemByPos(uint8(slot)));
    }

    Item* FindItem(Player* bot, uint32 entry)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetEntry() == entry)
                    return item;
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    if (Item* item = container->GetItemByPos(uint8(slot)))
                        if (item->GetEntry() == entry)
                            return item;
        return nullptr;
    }

    // the unit a piece goes on: itself, or the attacker it covers (its current target first)
    Unit* PieceTarget(Player* bot, Piece const& piece, SoloSafety::Projection const& p)
    {
        switch (piece.kind)
        {
            case Kind::Stun:
            case Kind::Silence:
            case Kind::Window:
            case Kind::Snare:
                if (piece.area)
                    return bot;
                return p.foes.empty() ? nullptr : p.foes.front().unit;
            default:
                return bot;
        }
    }

    bool Castable(PlayerbotAI* botAI, Player* bot, Piece const& piece, Unit* target)
    {
        if (!target)
            return false;
        if (piece.item)
        {
            Item* item = FindItem(bot, piece.item);
            return item && !bot->HasSpellCooldown(piece.spell) && botAI->CanCastSpell(piece.spell, bot, false);
        }
        if (piece.area && piece.radius > 0.0f)
        {
            // an area around it: the attacker it covers must stand inside
            return botAI->CanCastSpell(piece.spell, bot);
        }
        // a cast heal while it chases (PlayerbotAI::CanCastSpell refuses a cast on the move): it stops for it (b319
        // Rumanatol L11 chasing a Blue Flutterer: no Healing Wave, a Lasher Root at hp 43 and Gift of the Naaru at 21)
        SpellInfo const* info = sSpellMgr->GetSpellInfo(piece.spell);
        if (bot->isMoving() && info && info->CalcCastTime(bot) > 0 && !bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
            return bot->HasSpell(piece.spell) && !bot->HasSpellCooldown(piece.spell) &&
                   (piece.cost <= 0.0f || float(bot->GetPower(POWER_MANA)) >= piece.cost) &&
                   !bot->IsNonMeleeSpellCast(false) && bot->IsWithinLOSInMap(target) &&
                   (target == bot || bot->IsWithinDist(target, info->GetMaxRange(false, bot)));
        return botAI->CanCastSpell(piece.spell, target);
    }

    // the attackers a piece covers: their damage per second (melee, spell)
    void Covered(Player* bot, Piece const& piece, SoloSafety::Projection const& p, float& melee, float& spell)
    {
        melee = 0.0f;
        spell = 0.0f;
        for (size_t i = 0; i < p.foes.size(); ++i)
        {
            SoloSafety::Projection::Foe const& foe = p.foes[i];
            bool covered = piece.area ? bot->GetExactDist2d(foe.unit) <= piece.radius + foe.unit->GetCombatReach()
                                      : i == 0;
            if (!covered)
                continue;
            melee += std::max(foe.dps - foe.spellDps, 0.0f);
            spell += std::min(foe.spellDps, foe.dps);
        }
    }

    // what one piece saves over the rest of the fight (points of damage not taken or healed)
    float Gain(Player* bot, Piece const& piece, SoloSafety::Projection const& p, Piece const* heal, Piece const* bandage)
    {
        float const lost = p.incoming * piece.castSec;  // its cast keeps its damage off: the fight runs that much longer
        float const span = std::min(piece.durationSec, p.seconds);
        float melee = 0.0f;
        float spell = 0.0f;
        switch (piece.kind)
        {
            case Kind::Heal:
            {
                if (piece.needsWindow)
                    return 0.0f;
                // its mana for as many casts as the fight wants
                // ... within the fight (its casts, its cooldown: b319 Gift of the Naaru counted 10200 for a 3 min
                // cooldown)
                float casts = std::max(1.0f, std::ceil(p.seconds / std::max(piece.castSec, 0.1f)));
                if (piece.cost > 0.0f)
                    casts = std::min(casts, std::floor(float(bot->GetPower(POWER_MANA)) / piece.cost));
                if (piece.cooldownSec > 0.0f)
                    casts = std::min(casts, 1.0f + std::floor(p.seconds / piece.cooldownSec));
                if (piece.item)
                    casts = std::min(casts, 1.0f);
                float const each = piece.amount - lost;
                return each > 0.0f ? each * std::max(casts, 0.0f) : 0.0f;
            }
            case Kind::Absorb:
                return std::min(piece.amount, p.incoming * std::max(span, piece.castSec));
            case Kind::Reduce:
            {
                float covered = 0.0f;
                for (auto const& foe : p.foes)
                    covered += piece.meleeOnly ? std::max(foe.dps - foe.spellDps, 0.0f) : foe.dps;
                return covered * piece.pct * span;
            }
            case Kind::Stun:
                Covered(bot, piece, p, melee, spell);
                return (melee + spell) * span - lost;
            case Kind::Silence:
                Covered(bot, piece, p, melee, spell);
                return spell * span - lost;
            case Kind::Window:
            {
                // the time the control holds the attackers off its heals: the heal (or bandage) casts that fit
                Covered(bot, piece, p, melee, spell);
                if (melee + spell + 0.01f < p.incoming)
                    return 0.0f;  // another attacker still hits: no free window
                Piece const* use = bandage && bandage->castSec <= piece.durationSec ? bandage : heal;
                if (!use)
                    return 0.0f;
                float const fits = std::floor(piece.durationSec / std::max(use->castSec, 0.1f));
                float casts = fits;
                if (use->cost > 0.0f)
                    casts = std::min(casts, std::floor(float(bot->GetPower(POWER_MANA)) / use->cost));
                if (use->item)
                    casts = std::min(casts, 1.0f);
                return std::min(use->amount * casts, float(bot->GetMaxHealth()));
            }
            default:
                return 0.0f;
        }
    }

    // a heal goes out when it is all taken (the missing health covers it) or the hits before the next chance kill
    bool HealDue(Player* bot, Piece const& piece, SoloSafety::Projection const& p)
    {
        float const missing = float(bot->GetMaxHealth() - bot->GetHealth());
        if (missing >= piece.amount)
            return true;
        return p.health <= p.incoming * (piece.castSec + piece.castSec);
    }

    // the run: the hits until the attackers leash (CanCreatureAttack: more than CONFIG_CREATURE_LEASH_RADIUS from the
    // reset spot, the last leash extension older than GetLeashTimer; the threat list's next update, 1 s, drops it)
    struct FleePlan
    {
        bool ok = false;
        float damage = 0.0f;
        float leashSec = 0.0f;
        float dist = 0.0f;
        float angle = 0.0f;
        Piece const* first = nullptr;  // a snare / root / speed / leap cast before it runs
        bool trail = false;            // [guild15] back along the way it came
        std::string trailNote;         // [guild15] how far back, the spot cut and why
        std::string why;
        std::vector<G3D::Vector3> route;  // [deaths] a maze search's walk round the other mobs (its corners)
    };

    // [guild15] the spots of the way it came, newest first, still farther than its reach (the run walks them back)
    std::vector<G3D::Vector3> TrailBack(PlayerbotAI* botAI, Player* bot)
    {
        std::vector<G3D::Vector3> spots;
        NewRpgInfo const& info = botAI->rpgInfo;
        if (info.trailMap != bot->GetMapId())
            return spots;
        for (uint8 n = 0; n < info.trailCount; ++n)
        {
            G3D::Vector3 const& at = info.TrailBack(n);
            if (bot->GetExactDist2d(at.x, at.y) > bot->GetCombatReach())
                spots.push_back(at);
        }
        return spots;
    }

    // [guild15] an idle aggressive mob round a run's way (user 2026-11-08: the way it came is clear only of the mobs
    // still where they stood): where it stands; a wanderer at its spawn with its wander distance on its reach (the
    // model of SoloSafety::RouteCosts); a patrol where it walks while the bot passes (PullPlanning::PatrolForecast)
    struct Blocker
    {
        Creature* mob = nullptr;
        char const* kind = "idle";
        G3D::Vector3 at;
        float radius = 0.0f;
        std::vector<G3D::Vector3> track;  // a patrol: one spot every sampleSec from now
        float sampleSec = 0.0f;
        bool assists = false;  // [deaths] it assists a chaser: its radius is the chaser's call for assistance
    };

    std::vector<Blocker> Blockers(Player* bot, float range)
    {
        std::vector<Blocker> out;
        float const assist = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        std::vector<Unit*> chasers;
        for (Unit* attacker : bot->getAttackers())
            if (attacker && attacker->IsAlive())
                chasers.push_back(attacker);
        // [deaths12] a wanderer whose home lies beyond the run's reach walks its wander area onto it all the same
        // (its blocker is its home with that area, below): the search looks that much farther, at most as far as the
        // map shows the bot units. b350 Sycina L11: the flee was planned clear, a Thistle Bear L12 wanderer from
        // beyond the search cut its way back ("flee stops"), 6 s lost under the bear and dead
        float const search =
            std::min(range + SoloSafety::MapWanderReach(bot->GetMapId()), bot->GetMap()->GetVisibilityRange());
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, std::max(search, range));
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, std::max(search, range));
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID() ||
                mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE))
                continue;
            // beyond `range` only a wanderer whose area comes within it counts
            if (bot->GetExactDist2d(mob) > range &&
                (mob->GetDefaultMovementType() != RANDOM_MOTION_TYPE ||
                 mob->GetHomePosition().GetExactDist2d(bot) - mob->GetWanderDistance() > range))
                continue;
            Blocker blocker;
            blocker.mob = mob;
            blocker.at = G3D::Vector3(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
            blocker.radius = mob->GetAggroRange(bot) + bot->GetCombatReach();
            // [deaths] the chasers follow the bot along its way: an idle mob that assists one of them (the core's
            // CallAssistance from the chaser, CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS, Creature::CanAssistTo; called
            // again every CONFIG_CREATURE_FAMILY_ASSISTANCE_PERIOD of the fight) joins where the way passes within
            // that radius of it (b337 Dekorhan L7: a Bristlelimb Furbolg joined a run priced on two Windcallers)
            if (assist > 0.0f)
                for (Unit* chaser : chasers)
                    if (mob->CanAssistTo(chaser, bot))
                    {
                        blocker.assists = true;
                        blocker.radius = std::max(blocker.radius, assist);
                        break;
                    }
            if (PullPlanning::PatrolForecast(mob, blocker.track, blocker.sampleSec) && blocker.sampleSec > 0.0f &&
                !blocker.track.empty())
                blocker.kind = "patrol";
            else
            {
                blocker.track.clear();
                if (mob->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
                {
                    Position const& home = mob->GetHomePosition();
                    blocker.kind = "wanderer";
                    blocker.at = G3D::Vector3(home.GetPositionX(), home.GetPositionY(), home.GetPositionZ());
                    blocker.radius += mob->GetWanderDistance();
                }
            }
            out.push_back(std::move(blocker));
        }
        return out;
    }

    float SegmentGap2d(G3D::Vector3 const& p, G3D::Vector3 const& a, G3D::Vector3 const& b)
    {
        float const dx = b.x - a.x;
        float const dy = b.y - a.y;
        float const len2 = dx * dx + dy * dy;
        float const t = len2 > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
        return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
    }

    // the first mob whose aggro reach the walk from `a` to `b` enters, the walk starting `startSec` from now at
    // `speed` (a patrol: its forecast spots over that time)
    Blocker const* StretchBlocker(std::vector<Blocker> const& blockers, G3D::Vector3 const& a, G3D::Vector3 const& b,
                                  float startSec, float speed)
    {
        float const endSec = startSec + std::hypot(b.x - a.x, b.y - a.y) / std::max(speed, 0.1f);
        for (Blocker const& blocker : blockers)
        {
            if (blocker.track.empty())
            {
                if (SegmentGap2d(blocker.at, a, b) <= blocker.radius)
                    return &blocker;
                continue;
            }
            size_t const last = std::min(blocker.track.size() - 1, size_t(std::ceil(endSec / blocker.sampleSec)));
            for (size_t i = std::min(last, size_t(startSec / blocker.sampleSec)); i <= last; ++i)
                if (SegmentGap2d(blocker.track[i], a, b) <= blocker.radius)
                    return &blocker;
        }
        return nullptr;
    }

    G3D::Vector3 Ahead(G3D::Vector3 const& from, float angle, float length)
    {
        return G3D::Vector3(from.x + std::cos(angle) * length, from.y + std::sin(angle) * length, from.z);
    }

    std::string BlockerName(Blocker const& blocker)
    {
        return Acore::StringFormat("{} L{} ({}{})", blocker.mob->GetName(), blocker.mob->GetLevel(), blocker.kind,
                                   blocker.assists ? ", assists a chaser" : "");
    }

    // [regress10] every stretch of a run's walk keeps ground (or water) under it as the airborne guard samples it: a
    // stretch over a drop is a fall (b345 Januel L11 in Loch Modan: "no stretch clear, on round the other mobs (3
    // corners, 35 yd)", then "falls from (-5526.6,-2722.3,365.2)" and died under the Stonesplinter Scouts)
    bool WayKeepsGround(Player* bot, G3D::Vector3 from, std::vector<G3D::Vector3> const& corners)
    {
        for (G3D::Vector3 const& corner : corners)
        {
            if (!PlayerbotAI::StraightLineSupported(bot, from.x, from.y, from.z, corner.x, corner.y, corner.z))
                return false;
            from = corner;
        }
        return true;
    }

    // [deaths] user 2026-11-08: "no way out clear of other mobs" means a maze search - the navmesh walk round every
    // blocker's reach (Leader::Travel::LongRoute with the reaches refused: a patrol's at every spot of its forecast
    // walk, a reach the bot stands in lies behind it) to the spots `dist` away all round the bot (an eighth of the
    // circle apart, the turns of the direction search) and to `extra` (the way back past its cut); the complete
    // walks, their corners from the bot on
    std::vector<std::vector<G3D::Vector3>> MazeWays(Player* bot, std::vector<Blocker> const& blockers, float dist,
                                                    std::vector<G3D::Vector3> const& extra)
    {
        std::vector<std::vector<G3D::Vector3>> ways;
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        std::vector<G3D::Vector4> discs;
        std::vector<Leader::Travel::RoutePenalty> edges;  // the polygons' edges tested too (penalties at no cost)
        auto const add = [&](G3D::Vector3 const& at, float radius)
        {
            if ((at.x - here.x) * (at.x - here.x) + (at.y - here.y) * (at.y - here.y) <= radius * radius)
                return;
            discs.emplace_back(at.x, at.y, at.z, radius);
            Leader::Travel::RoutePenalty edge;
            edge.x = at.x;
            edge.y = at.y;
            edge.z = at.z;
            edge.r = radius;
            edges.push_back(edge);
        };
        for (Blocker const& blocker : blockers)
        {
            if (blocker.track.empty())
                add(blocker.at, blocker.radius);
            for (G3D::Vector3 const& at : blocker.track)
                add(at, blocker.radius);
        }
        std::vector<G3D::Vector3> goals = extra;
        constexpr uint32 MAZE_DIRECTIONS = 8;  // the direction search's quarter turns, all round
        for (uint32 i = 0; i < MAZE_DIRECTIONS; ++i)
        {
            float const angle = 2.0f * float(M_PI) * float(i) / float(MAZE_DIRECTIONS);
            float x = here.x + std::cos(angle) * dist;
            float y = here.y + std::sin(angle) * dist;
            float z = here.z;
            bot->UpdateAllowedPositionZ(x, y, z);
            goals.emplace_back(x, y, z);
        }
        for (G3D::Vector3 const& goal : goals)
        {
            Movement::PointsArray corners;
            Movement::PointsArray surface;  // [regress10] the walk on the navmesh's height, for the drop check
            bool complete = false;
            if (!Leader::Travel::LongRoute(bot, here, goal, false, corners, complete, nullptr, &discs,
                                           PlayerbotAI::GroundSampleStep(), &surface, &edges) ||
                !complete || corners.empty())
                continue;
            if (!WayKeepsGround(bot, here, std::vector<G3D::Vector3>(surface.begin(), surface.end())))
                continue;
            std::vector<G3D::Vector3> way;
            for (G3D::Vector3 const& corner : corners)
                if ((corner - here).length() > bot->GetCombatReach() || !way.empty())
                    way.push_back(corner);
            if (!way.empty())
                ways.push_back(std::move(way));
        }
        return ways;
    }

    // the yards along `route` until it leaves the circle of `radius` round `centre` (2D), past its last spot straight
    // on along `onAngle`
    float ExitYards(std::vector<G3D::Vector3> const& route, float onAngle, Position const& centre, float radius)
    {
        auto const leave = [&](G3D::Vector3 const& a, float ux, float uy, float length)
        {
            float const wx = a.x - centre.GetPositionX();
            float const wy = a.y - centre.GetPositionY();
            float const c = wx * wx + wy * wy - radius * radius;
            if (c >= 0.0f)
                return 0.0f;
            float const half = ux * wx + uy * wy;
            float const s = -half + std::sqrt(half * half - c);
            return s <= length ? s : -1.0f;
        };
        if (route.empty())
            return 0.0f;
        float yards = 0.0f;
        for (size_t i = 0; i + 1 < route.size(); ++i)
        {
            float const dx = route[i + 1].x - route[i].x;
            float const dy = route[i + 1].y - route[i].y;
            float const length = std::hypot(dx, dy);
            if (length <= 0.0f)
                continue;
            float const s = leave(route[i], dx / length, dy / length, length);
            if (s >= 0.0f)
                return yards + s;
            yards += length;
        }
        return yards + std::max(0.0f, leave(route.back(), std::cos(onAngle), std::sin(onAngle),
                                            std::numeric_limits<float>::max()));
    }

    // [guild15] where a chaser resets, as Creature::CanCreatureAttack measures its leash: its idle movement's reset
    // spot (a wanderer's current random point, a patrol's current node), else its home
    Position ResetSpot(Creature* mob)
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (MovementGenerator* idle = mob->GetMotionMaster()->GetMotionSlot(MOTION_SLOT_IDLE))
            if (idle->GetResetPosition(x, y, z))
                return Position(x, y, z);
        return mob->GetHomePosition();
    }

    FleePlan PlanFlee(PlayerbotAI* botAI, Player* bot, SoloSafety::Projection const& p, std::vector<Piece> const& kit,
                      KitState& state)
    {
        FleePlan plan;
        float const leashRadius = sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS);
        if (leashRadius <= 0.0f)
        {
            plan.why = "no leash";
            return plan;
        }
        for (auto const& foe : p.foes)
            if (!foe.unit->ToCreature())
            {
                plan.why = "a player attacker";
                return plan;
            }
        time_t const now = GameTime::GetGameTime().count();
        // the best speed piece / chaser slow it can cast now
        Piece const* speed = nullptr;
        Piece const* snare = nullptr;
        Piece const* leap = nullptr;
        for (Piece const& piece : kit)
        {
            if (piece.kind == Kind::Speed && (!speed || piece.pct > speed->pct) && Castable(botAI, bot, piece, bot))
                speed = &piece;
            else if (piece.kind == Kind::Snare && (!snare || piece.pct < snare->pct) &&
                     Castable(botAI, bot, piece, PieceTarget(bot, piece, p)))
                snare = &piece;
            else if (piece.kind == Kind::Leap && (!leap || piece.radius > leap->radius) &&
                     Castable(botAI, bot, piece, bot))
                leap = &piece;
        }
        // in water both swim (b319: Stone Cairn Lake murlocs killed Ravelandy and Orton on runs judged to cost 85 / 78)
        UnitMoveType const moveType = bot->IsInWater() ? MOVE_SWIM : MOVE_RUN;
        float const botSpeed = bot->GetSpeed(moveType);
        // a melee hit from behind may daze it (Unit::AttackerStateUpdate casts 1604): its speed by that spell's data
        float dazed = 0.0f;
        if (sWorld->getBoolConfig(CONFIG_ENABLE_DAZE))
            if (SpellInfo const* daze = sSpellMgr->GetSpellInfo(1604))
                for (SpellEffectInfo const& effect : daze->GetEffects())
                    if (effect.ApplyAuraName == SPELL_AURA_MOD_DECREASE_SPEED)
                        dazed = std::min(dazed, float(effect.CalcValue()) / 100.0f);
        // the direction: away from the attackers' homes, clear of idle mobs
        Position home;
        float homeX = 0.0f, homeY = 0.0f;
        for (auto const& foe : p.foes)
        {
            Position const at = foe.unit->ToCreature()->GetHomePosition();
            homeX += at.GetPositionX();
            homeY += at.GetPositionY();
        }
        home.Relocate(homeX / float(p.foes.size()), homeY / float(p.foes.size()), bot->GetPositionZ());
        float away = home.GetAbsoluteAngle(bot);
        if (home.GetExactDist2d(bot) < bot->GetCombatReach())
            away = Position::NormalizeOrientation(p.foes.front().unit->GetOrientation() + float(M_PI));

        // [guild15] a player hit from behind never dodges, parries or blocks (Unit::RollMeleeOutcomeAgainst): the
        // chasers' melee, measured on its front, lands that much more on the run
        float const avoided =
            (bot->GetFloatValue(PLAYER_DODGE_PERCENTAGE) + bot->GetFloatValue(PLAYER_PARRY_PERCENTAGE)) / 100.0f;
        float const behind = avoided > 0.0f && avoided < 1.0f ? 1.0f / (1.0f - avoided) : 1.0f;
        // the hits of a run on which chaser i walks needs[i] yards after the bot until it is past its leash radius
        struct Priced
        {
            float damage = 0.0f;
            float leashSec = 0.0f;
            Piece const* first = nullptr;
        };
        auto const price = [&](std::vector<float> const& needs, bool leaps)
        {
            Priced best;
            bool planned = false;
            for (Piece const* first : {static_cast<Piece const*>(nullptr), snare, speed, leaps ? leap : nullptr})
            {
                if (planned && !first)
                    continue;
                float damage = 0.0f;
                float longest = 0.0f;
                float vb = botSpeed;
                float head = 0.0f;  // the yards it gains before the chase starts
                if (first && first->kind == Kind::Speed)
                    vb *= 1.0f + first->pct;
                if (first && first->kind == Kind::Leap)
                    head = first->radius;
                for (size_t i = 0; i < p.foes.size(); ++i)
                {
                    auto const& foe = p.foes[i];
                    Creature* mob = foe.unit->ToCreature();
                    float vm = mob->GetSpeed(moveType);
                    float leashLeft = float(mob->GetLastLeashExtensionTime() + time_t(mob->GetLeashTimer()) - now);
                    // a spell on the chaser refreshes its leash (Unit::AtTargetAttacked): its full timer from the cast
                    if (first && first->kind == Kind::Snare && (first->area || i == 0))
                    {
                        leashLeft = float(mob->GetLeashTimer()) + first->castSec;
                        vm *= 1.0f + first->pct;
                    }
                    leashLeft = std::max(leashLeft, 0.0f);
                    // a slower chaser in reach can daze it down below its own speed: then it hits all the way too
                    // [deaths] ... and a faster one dazes it once it has caught up (Unit::AttackerStateUpdate from
                    // behind), however far back it starts
                    bool const canDaze = std::max(foe.dps - foe.spellDps, 0.0f) > 0.0f &&
                                         ((head <= 0.0f && bot->IsWithinMeleeRange(mob)) || vm > vb) &&
                                         vb * (1.0f + dazed) <= vm;
                    // [guild15] dazed it walks the leash yards at its dazed speed, the chaser on its heels
                    float const followSpeed = std::max(std::min(vm, canDaze ? vb * (1.0f + dazed) : vb), 0.1f);
                    float const until = std::max(leashLeft, needs[i] / followSpeed) + 1.0f;  // + ThreatManager's 1 s
                    // a faster chaser hits all the way (dazing from behind: Unit::AttackerStateUpdate); a slower one
                    // until the gap passes its melee reach
                    float exposure = until;
                    float const reach = mob->GetMeleeRange(bot);
                    if (vm < vb && !canDaze)
                        exposure = std::min(until, std::max(0.0f, reach - head) / (vb - vm));
                    else if (head > 0.0f)
                        exposure = std::max(0.0f, until - head / std::max(vm, 0.1f));
                    float const melee = std::max(foe.dps - foe.spellDps, 0.0f) * behind;
                    // [deaths] user 2026-11-08 (b337 Dekorhan L7: priced 106 at hp 106, died): its casts while it
                    // chases - every in-combat and at-range cast of its script, each repeat over the run, a cast that
                    // does not repeat once (SoloSafety::ChaseSpellDamage); the projection's share when it has none
                    float const casts = SoloSafety::ChaseSpellDamage(mob, until);
                    float const spell = casts > 0.0f ? casts : std::min(foe.spellDps, foe.dps) * until;
                    damage += melee * exposure + spell;
                    longest = std::max(longest, until);
                }
                if (first)
                    damage += p.incoming * first->castSec;
                if (!planned || damage < best.damage)
                {
                    best.damage = damage;
                    best.leashSec = longest;
                    best.first = first;
                }
                planned = true;
            }
            return best;
        };

        // 1. straight away from their homes, then to the sides: each chaser past its leash radius from its reset spot
        FleePlan straight;
        bool straightClear = false;
        {
            std::vector<float> needs;
            for (auto const& foe : p.foes)
            {
                Creature* mob = foe.unit->ToCreature();
                needs.push_back(std::max(0.0f, leashRadius - ResetSpot(mob).GetExactDist2d(mob)));
            }
            Priced const priced = price(needs, true);
            straight.damage = priced.damage;
            straight.leashSec = priced.leashSec;
            straight.first = priced.first;
            straight.dist = botSpeed * priced.leashSec;
            straight.angle = away;
        }
        std::vector<G3D::Vector3> const back = TrailBack(botAI, bot);
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

        // 2. [guild15] back the way it came (b333 Colian L12 at The Lost Fold: the five straight stretches all met an
        // idle mob, the run stopped and he died to a Bloodmyst Hatchling L10), stretch by stretch clear of the idle
        // mobs, wanderers and patrols as far as the run needs, cut at the last clear spot and on from there by the
        // same direction search; each chaser walks it after the bot from where it stands. No leap on it (a leap goes
        // where the bot faces, not along the way).
        FleePlan trail;
        bool trailClear = false;
        std::string trailNote;
        auto const trailNeeds = [&](size_t spots, float onAngle)
        {
            std::vector<float> needs;
            for (auto const& foe : p.foes)
            {
                Creature* mob = foe.unit->ToCreature();
                std::vector<G3D::Vector3> route;
                route.emplace_back(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
                route.push_back(here);
                route.insert(route.end(), back.begin(), back.begin() + spots);
                needs.push_back(ExitYards(route, onAngle, ResetSpot(mob), leashRadius));
            }
            return needs;
        };
        std::vector<Blocker> blockers;
        if (!back.empty())
        {
            // the whole way back priced first: how far the check must reach
            G3D::Vector3 const& oldest = back.back();
            Priced const whole = price(trailNeeds(back.size(), home.GetAbsoluteAngle(oldest.x, oldest.y)), false);
            float const need = botSpeed * whole.leashSec;
            blockers = Blockers(bot, std::max(need, straight.dist) + 40.0f);
            size_t spots = 0;
            float walked = 0.0f;
            G3D::Vector3 from = here;
            for (; spots < back.size() && walked < need; ++spots)
            {
                if (Blocker const* on = StretchBlocker(blockers, from, back[spots], walked / botSpeed, botSpeed))
                {
                    trailNote = Acore::StringFormat(", spot {} back skipped: {} on it", spots + 1, BlockerName(*on));
                    break;
                }
                walked += std::hypot(back[spots].x - from.x, back[spots].y - from.y);
                from = back[spots];
            }
            if (spots)
            {
                // on from the cut, away from their homes: straight, then to the sides
                float const onAway = home.GetAbsoluteAngle(from.x, from.y);
                for (float turn :
                     {0.0f, float(M_PI) / 4.0f, -float(M_PI) / 4.0f, float(M_PI) / 2.0f, -float(M_PI) / 2.0f})
                {
                    float const angle = Position::NormalizeOrientation(onAway + turn);
                    Priced const priced = price(trailNeeds(spots, angle), false);
                    float const rest = botSpeed * priced.leashSec - walked;
                    if (rest > 0.0f &&
                        StretchBlocker(blockers, from, Ahead(from, angle, rest), walked / botSpeed, botSpeed))
                        continue;
                    trail.damage = priced.damage;
                    trail.leashSec = priced.leashSec;
                    trail.first = priced.first;
                    trail.dist = botSpeed * priced.leashSec;
                    trail.angle = angle;
                    trail.trail = true;
                    trailClear = true;
                    if (rest > 0.0f)
                    {
                        trailNote += Acore::StringFormat(", {} spots back ({:.0f} yd) of {:.0f} yd needed, on straight",
                                                         spots, walked, trail.dist);
                        // the way kept is shorter than the run needs: logged when its capacity limits it (not the
                        // walk since the login or a cut)
                        if (spots == back.size() && botAI->rpgInfo.trailCount >= NewRpgInfo::TRAIL_SPOTS)
                            LOG_INFO("playerbots",
                                     "[solo] {} L{} survival flee: the way back kept ({} spots, {:.0f} yd) is shorter "
                                     "than the {:.0f} yd the run needs",
                                     bot->GetName(), bot->GetLevel(), spots, walked, trail.dist);
                    }
                    else
                        trailNote += Acore::StringFormat(", {} spots back", spots);
                    break;
                }
            }
        }
        else
            blockers = Blockers(bot, straight.dist + 40.0f);
        for (float turn : {0.0f, float(M_PI) / 4.0f, -float(M_PI) / 4.0f, float(M_PI) / 2.0f, -float(M_PI) / 2.0f})
        {
            float const angle = Position::NormalizeOrientation(away + turn);
            if (!StretchBlocker(blockers, here, Ahead(here, angle, straight.dist), 0.0f, botSpeed))
            {
                straight.angle = angle;
                straightClear = true;
                break;
            }
        }
        // 3. [deaths] no stretch clear (b338: 12 deaths "no way out clear of other mobs"): the maze search round them,
        // to as far as the straight run needs all round and to the oldest spot of the way back; each walk priced as
        // the chasers follow it (on straight past its end along its last stretch, that stretch clear too)
        FleePlan maze;
        bool mazeClear = false;
        G3D::Vector3 const at(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (!trailClear && !straightClear &&
            !(state.mazeFailed && (state.mazeFailedAt - at).length() <= bot->GetCombatReach()))
        {
            std::vector<G3D::Vector3> extra;
            if (!back.empty())
                extra.push_back(back.back());
            for (std::vector<G3D::Vector3> const& way :
                 MazeWays(bot, blockers, std::max(straight.dist, bot->GetCombatReach()), extra))
            {
                G3D::Vector3 const& last = way.back();
                G3D::Vector3 const& before = way.size() >= 2 ? way[way.size() - 2] : here;
                float const onAngle = std::atan2(last.y - before.y, last.x - before.x);
                std::vector<float> needs;
                for (auto const& foe : p.foes)
                {
                    Creature* mob = foe.unit->ToCreature();
                    std::vector<G3D::Vector3> route;
                    route.emplace_back(mob->GetPositionX(), mob->GetPositionY(), mob->GetPositionZ());
                    route.push_back(here);
                    route.insert(route.end(), way.begin(), way.end());
                    needs.push_back(ExitYards(route, onAngle, ResetSpot(mob), leashRadius));
                }
                Priced const priced = price(needs, false);
                // each stretch clear when the bot walks it (a patrol's forecast spot then)
                float walked = 0.0f;
                G3D::Vector3 from = here;
                bool clear = true;
                for (G3D::Vector3 const& corner : way)
                {
                    clear = clear && !StretchBlocker(blockers, from, corner, walked / botSpeed, botSpeed);
                    walked += std::hypot(corner.x - from.x, corner.y - from.y);
                    from = corner;
                }
                if (!clear)
                    continue;
                float const rest = botSpeed * priced.leashSec - walked;
                if (rest > 0.0f && StretchBlocker(blockers, last, Ahead(last, onAngle, rest), walked / botSpeed,
                                                  botSpeed))
                    continue;
                if (mazeClear && priced.damage >= maze.damage)
                    continue;
                maze.damage = priced.damage;
                maze.leashSec = priced.leashSec;
                maze.first = priced.first;
                maze.dist = botSpeed * priced.leashSec;
                maze.angle = onAngle;
                maze.route = way;
                mazeClear = true;
            }
            state.mazeFailed = !mazeClear;
            state.mazeFailedAt = at;
            if (mazeClear)
                trailNote += Acore::StringFormat(", round the other mobs ({} corners)", maze.route.size());
        }
        if (!trailClear && !straightClear && !mazeClear)
        {
            plan.why = "no way out clear of other mobs" + trailNote;
            return plan;
        }
        if (mazeClear)
        {
            plan = maze;
            plan.trailNote = trailNote;
            plan.ok = plan.damage < p.health;
            if (!plan.ok)
                plan.why = Acore::StringFormat("the run costs {:.0f} at hp {:.0f}{}", plan.damage, p.health, trailNote);
            return plan;
        }
        // the way that costs less
        plan = trailClear && (!straightClear || trail.damage <= straight.damage) ? trail : straight;
        plan.trailNote = trailNote;
        plan.ok = plan.damage < p.health;
        if (!plan.ok)
            plan.why = Acore::StringFormat("the run costs {:.0f} at hp {:.0f}", plan.damage, p.health);
        return plan;
    }

    void LogChoice(Player* bot, KitState& state, Piece const& piece, std::string const& why)
    {
        if (state.loggedSpell == piece.spell && state.loggedKind == piece.kind)
            return;
        state.loggedSpell = piece.spell;
        state.loggedKind = piece.kind;
        SpellInfo const* info = piece.spell ? sSpellMgr->GetSpellInfo(piece.spell) : nullptr;
        LOG_INFO("playerbots", "[solo] {} L{} survival {} ({}{}): {}", bot->GetName(), bot->GetLevel(),
                 info ? info->SpellName[0] : "run", KindName(piece.kind), piece.item ? ", item" : "", why);
    }

    void RestorePet(Player* bot, KitState& state)
    {
        if (!state.petPassive)
            return;
        state.petPassive = false;
        if (Pet* pet = bot->GetPet())
            if (pet->HasReactState(REACT_PASSIVE))
                pet->SetReactState(state.petReact);
    }
}

namespace SurvivalKit
{
    float HealPoints(Player* bot, float fightSec, float spareMana, bool items, bool onTheMove)
    {
        if (!bot)
            return 0.0f;
        std::vector<Piece> pieces;
        {
            std::lock_guard<std::mutex> guard(lock);
            pieces = KitOf(bot).pieces;
            if (items)
                ItemPieces(bot, pieces);
        }
        float total = 0.0f;
        std::unordered_map<uint32, float> itemByCategory;  // one heal item per shared cooldown (potions, healthstones)
        Piece const* manaHeal = nullptr;                   // the most healing per mana
        for (Piece const& piece : pieces)
        {
            if (piece.kind != Kind::Heal || piece.needsWindow || piece.amount <= 0.0f)
                continue;
            // [deaths12] on the move (a run past, a chase) a spell with a cast time is no heal: the game cancels a
            // cast when its caster moves (Spell::update, SPELL_INTERRUPT_FLAG_MOVEMENT); an item's on-use is instant
            if (onTheMove && !piece.item && !piece.instant)
                continue;
            // [deaths13] a heal the aura on the bot forbids (SpellInfo::CheckCast: ExcludeCasterAuraSpell /
            // ExcludeTargetAuraSpell, the bot heals itself) is no heal now: Lay on Hands under Forbearance after a
            // Divine Protection (b353 Geron L10: run-past "hp 500" at 250 max health counted Lay on Hands; Divine
            // Protection went first in the fight, Lay on Hands never came, it died at hp 20)
            if (!piece.item)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(piece.spell);
                    info && ((info->ExcludeCasterAuraSpell && bot->HasAura(info->ExcludeCasterAuraSpell)) ||
                             (info->ExcludeTargetAuraSpell && bot->HasAura(info->ExcludeTargetAuraSpell))))
                    continue;
            if (piece.item)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(piece.spell);
                uint32 const category = info && info->GetCategory() ? info->GetCategory() : piece.spell;
                float& best = itemByCategory[category];
                best = std::max(best, piece.amount);
                continue;
            }
            if (bot->HasSpellCooldown(piece.spell))
                continue;
            if (piece.cost <= 0.0f)
            {
                // a heal without mana (Gift of the Naaru, Lay on Hands' cost aside): as often as its cooldown allows
                float const uses =
                    piece.cooldownSec > 0.0f ? 1.0f + std::floor(std::max(fightSec, 0.0f) / piece.cooldownSec) : 1.0f;
                total += piece.amount * uses;
                continue;
            }
            if (!manaHeal || piece.amount / piece.cost > manaHeal->amount / manaHeal->cost)
                manaHeal = &piece;
        }
        for (auto const& [category, amount] : itemByCategory)
            total += amount;
        if (manaHeal && spareMana > 0.0f)
        {
            float casts = std::floor(spareMana / manaHeal->cost);
            if (manaHeal->cooldownSec > 0.0f)
                casts = std::min(casts, 1.0f + std::floor(std::max(fightSec, 0.0f) / manaHeal->cooldownSec));
            // [deaths12] the casts the fight's time holds: one per cast time (the global cooldown at least, CastSec);
            // a heal over time once per its duration, its points as far as the fight lasts. Before, the mana alone
            // set the count: b350 Enyssia L6 (139 hp) had "hp 874" for a 12 s chase - 15 Healing Touches - and ran
            // past a Gnarlpine camp into her death; Peny L12 pulled 5 Rockjaws judged 438 dmg at hp 256
            float const span = std::max(fightSec, 0.0f);
            float amount = manaHeal->amount;
            if (manaHeal->durationSec > 0.0f)
            {
                casts = std::min(casts, std::max(1.0f, std::ceil(span / manaHeal->durationSec)));
                amount *= std::min(1.0f, span / manaHeal->durationSec);
            }
            else
                casts = std::min(casts, std::max(1.0f, std::floor(span / std::max(manaHeal->castSec, 0.1f))));
            total += amount * casts;
        }
        return total;
    }

    bool Evaluate(Player* bot)
    {
        if (!bot || !bot->IsInCombat() || !SoloSafety::Applies(bot))
        {
            if (bot)
            {
                std::lock_guard<std::mutex> guard(lock);
                auto const found = states.find(bot->GetGUID().GetCounter());
                if (found != states.end())
                {
                    RestorePet(bot, found->second);
                    states.erase(found);
                }
            }
            return false;
        }
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;
        SoloSafety::Projection const p = SoloSafety::Project(bot);
        std::lock_guard<std::mutex> guard(lock);
        KitState& state = states[bot->GetGUID().GetCounter()];
        state.active = false;
        if (!p.open)
            return false;
        if (state.fleeing && p.fleeing)
        {
            state.active = true;
            return true;
        }
        // a reset under way (Feign Death holds, Vanish / Shadowmeld): nothing breaks it
        if (state.reset)
        {
            if (bot->HasAura(state.choice.spell) || bot->HasUnitFlag2(UNIT_FLAG2_FEIGN_DEATH))
            {
                state.active = true;
                return true;
            }
            state.reset = false;
        }
        if (p.foes.empty())
            return false;

        BotKit const& kit = KitOf(bot);
        std::vector<Piece> pieces = kit.pieces;
        bool const lost = p.lost;
        if (!lost && !state.window)
            return false;
        ItemPieces(bot, pieces);

        // its best heal and bandage now (for the windows)
        Piece const* heal = nullptr;
        Piece const* bandage = nullptr;
        for (Piece const& piece : pieces)
        {
            if (piece.kind != Kind::Heal)
                continue;
            if (piece.needsWindow)
            {
                if (!bandage || piece.amount > bandage->amount)
                    bandage = &piece;
            }
            else if (!piece.item && (!heal || piece.amount - p.incoming * piece.castSec >
                                                   heal->amount - p.incoming * heal->castSec) &&
                     Castable(botAI, bot, piece, bot))
                heal = &piece;
        }

        // a window open: its heal (bandage) while the attackers are held
        if (state.window)
        {
            bool held = true;
            for (auto const& foe : p.foes)
                held = held && foe.unit->HasUnitState(UNIT_STATE_CONTROLLED);
            Piece const* use = nullptr;
            if (bandage && Castable(botAI, bot, *bandage, bot))
                use = bandage;
            else if (heal)
                use = heal;
            if (held && use && bot->GetHealth() < bot->GetMaxHealth())
            {
                state.choice = *use;
                state.target = bot->GetGUID();
                state.why = "in the control window";
                state.active = true;
                return true;
            }
            state.window = false;
            if (!lost)
                return false;
        }

        float const deficit = p.damage - (p.health + p.heals);
        std::string const numbers =
            Acore::StringFormat("{} attackers {:.1f} dps, {:.0f} s at its {:.1f} dps: {:.0f} dmg to come at hp {:.0f} "
                                "(+{:.0f} heals)",
                                p.foes.size(), p.incoming, p.seconds, p.botDps, p.damage, p.health, p.heals);

        // 1. a reset ends it
        for (Piece const& piece : pieces)
            if (piece.kind == Kind::Reset && Castable(botAI, bot, piece, bot))
            {
                state.choice = piece;
                state.target = bot->GetGUID();
                state.why = "the fight is lost: " + numbers;
                state.active = true;
                return true;
            }

        // 2. the pieces that turn it, shortest cooldown first
        struct Scored
        {
            Piece const* piece;
            Unit* target;
            float gain;
            bool due;
        };
        std::vector<Scored> scored;
        for (Piece const& piece : pieces)
        {
            if (piece.kind == Kind::Reset || piece.kind == Kind::Snare || piece.kind == Kind::Speed ||
                piece.kind == Kind::Leap || piece.needsWindow)
                continue;
            Unit* target = PieceTarget(bot, piece, p);
            float const gain = Gain(bot, piece, p, heal, bandage);
            if (gain <= 0.0f)
                continue;
            if (!Castable(botAI, bot, piece, target))
                continue;
            // the same buff / control already on
            if (piece.kind == Kind::Reduce || piece.kind == Kind::Absorb)
            {
                if (bot->HasAura(piece.spell))
                    continue;
            }
            else if (piece.kind != Kind::Heal && target != bot && target->HasAura(piece.spell))
                continue;
            bool const due = piece.kind == Kind::Heal ? HealDue(bot, piece, p) : true;
            scored.push_back({&piece, target, gain, due});
        }
        std::sort(scored.begin(), scored.end(),
                  [](Scored const& a, Scored const& b)
                  {
                      if (a.piece->cooldownSec != b.piece->cooldownSec)
                          return a.piece->cooldownSec < b.piece->cooldownSec;
                      return a.gain > b.gain;
                  });
        float planned = 0.0f;
        Scored const* now = nullptr;
        for (Scored const& s : scored)
        {
            planned += s.gain;
            if (!now && s.due)
                now = &s;
            if (planned >= deficit)
                break;
        }
        if (planned >= deficit)
        {
            if (!now)
                return false;  // its heals turn it once due: it fights on until then
            state.choice = *now->piece;
            state.target = now->target->GetGUID();
            state.why = Acore::StringFormat("turns it ({:.0f} of {:.0f} short): {}", now->gain, deficit, numbers);
            state.active = true;
            return true;
        }

        // 3. the run, when the leash leaves it alive
        FleePlan flee;
        if (state.noRun)
            flee.why = "no way out clear of other mobs";
        else
            flee = PlanFlee(botAI, bot, p, pieces, state);
        if (flee.ok)
        {
            Piece run;
            run.kind = Kind::Flee;
            if (flee.first)
                run = *flee.first;
            state.choice = run;
            state.target = flee.first ? PieceTarget(bot, *flee.first, p)->GetGUID() : bot->GetGUID();
            state.fleeAngle = flee.angle;
            state.fleeTrail = flee.trail;
            state.trailStep = 0;
            state.fleeRoute = flee.route;
            state.routeStep = 0;
            state.fleeFrom = p.foes.front().unit->GetGUID();
            state.why = Acore::StringFormat("leash={:.0f}s dist={:.0f} run costs {:.0f} at hp {:.0f}{}{}; {}",
                                            flee.leashSec, flee.dist, flee.damage, p.health,
                                            flee.trail ? ", back the way it came"
                                                       : (flee.route.empty() ? ", straight" : ", maze"),
                                            flee.trailNote,
                                            numbers);
            state.active = true;
            return true;
        }

        // 4. everything it has, the biggest piece first
        Scored const* best = nullptr;
        for (Scored const& s : scored)
            if (s.due && (!best || s.gain > best->gain))
                best = &s;
        if (!best)
        {
            if (!state.loggedNone)
            {
                state.loggedNone = true;
                LOG_INFO("playerbots",
                         "[solo] {} L{} survival none: no reset, no piece due ({} castable), no run: {}; {}",
                         bot->GetName(), bot->GetLevel(), scored.size(), flee.why, numbers);
            }
            return false;
        }
        state.choice = *best->piece;
        state.target = best->target->GetGUID();
        state.why = Acore::StringFormat("the biggest piece ({:.0f} of {:.0f} short; no run: {}): {}", best->gain, deficit,
                                        flee.why, numbers);
        state.active = true;
        return true;
    }
}

bool SoloSurvivalTrigger::IsActive() { return SurvivalKit::Evaluate(bot); }

bool SoloSurvivalAction::isUseful()
{
    std::lock_guard<std::mutex> guard(lock);
    auto const found = states.find(bot->GetGUID().GetCounter());
    return found != states.end() && found->second.active;
}

bool SoloSurvivalAction::Execute(Event /*event*/)
{
    Piece piece;
    ObjectGuid targetGuid;
    std::string why;
    bool fleeing = false;
    float angle = 0.0f;
    bool reset = false;
    bool trail = false;
    uint8 trailStep = 0;
    std::vector<G3D::Vector3> route;
    uint32 routeStep = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto const found = states.find(bot->GetGUID().GetCounter());
        if (found == states.end() || !found->second.active)
            return false;
        KitState& state = found->second;
        piece = state.choice;
        targetGuid = state.target;
        why = state.why;
        fleeing = state.fleeing;
        angle = state.fleeAngle;
        reset = state.reset;
        trail = state.fleeTrail;
        trailStep = state.trailStep;
        route = state.fleeRoute;
        routeStep = state.routeStep;
        bool const runKind = piece.kind == Kind::Flee || piece.kind == Kind::Snare || piece.kind == Kind::Speed ||
                             piece.kind == Kind::Leap;
        if (!fleeing && !reset && !runKind)
            LogChoice(bot, state, piece, why);
    }
    if (reset)
        return true;  // the reset holds: no swing, no cast breaks it

    // the run (or its first piece): every direct damage off, the pet passive at heel, auto shot off
    bool const run = fleeing || piece.kind == Kind::Flee || piece.kind == Kind::Snare || piece.kind == Kind::Speed ||
                     piece.kind == Kind::Leap;
    if (run && !fleeing)
    {
        {
            std::lock_guard<std::mutex> guard(lock);
            KitState& state = states[bot->GetGUID().GetCounter()];
            state.fleeing = true;
            if (Pet* pet = bot->GetPet())
                if (!state.petPassive)
                {
                    state.petReact = pet->GetReactState();
                    state.petPassive = true;
                    pet->SetReactState(REACT_PASSIVE);
                }
            LOG_INFO("playerbots", "[solo] {} L{} survival flee{}{}: {}", bot->GetName(), bot->GetLevel(),
                     piece.kind == Kind::Flee ? "" : " after ",
                     piece.kind == Kind::Flee ? "" : sSpellMgr->GetSpellInfo(piece.spell)->SpellName[0], why);
        }
        SoloSafety::BeginFlee(bot, "after a survival run");
        if (bot->GetPet())
            botAI->PetFollow();
        bot->AttackStop();
        bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        if (bot->IsNonMeleeSpellCast(false))
            bot->InterruptNonMeleeSpells(false);
        context->GetValue<Unit*>("current target")->Set(nullptr);
        bot->SetTarget(ObjectGuid::Empty);
        if (piece.kind != Kind::Flee)
        {
            Unit* target = ObjectAccessor::GetUnit(*bot, targetGuid);
            if (target && botAI->CastSpell(piece.spell, target))
                return true;
        }
    }
    if (run)
    {
        if (bot->GetVictim() || bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            bot->AttackStop();
        if (bot->GetPet())
        {
            Pet* pet = bot->GetPet();
            if (pet->GetVictim())
                botAI->PetFollow();
        }
        if (bot->isMoving() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            return true;
        if (bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONTROLLED))
            return true;
        // the next stretch of the way out on the navmesh (straight, else a quarter to either side), each clear of idle
        // mobs (b319 Lyleria L7: a run past its planned yards woke a Venomfang and a Nightsaber Stalker)
        // [guild15] back along the way it came, spot by spot (newest first); each stretch checked again before it is
        // walked (patrols and wanderers move): a blocked one ends the way back there, on by the direction search
        float const leash = sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS);
        float const runSpeed = bot->GetSpeed(bot->IsInWater() ? MOVE_SWIM : MOVE_RUN);
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        // [deaths] along the maze search's walk, corner by corner, each stretch checked again before it is walked
        if (!route.empty())
        {
            std::vector<Blocker> blockers;
            bool searched = false;
            for (uint32 n = routeStep; n < route.size(); ++n)
            {
                G3D::Vector3 const& at = route[n];
                if (bot->GetExactDist2d(at.x, at.y) <= bot->GetCombatReach())
                    continue;  // reached
                if (!searched)
                {
                    blockers = Blockers(bot, bot->GetExactDist2d(at.x, at.y) + 40.0f);
                    searched = true;
                }
                if (Blocker const* on = StretchBlocker(blockers, here, at, 0.0f, runSpeed))
                {
                    LOG_INFO("playerbots", "[solo] {} L{} survival flee: corner {} of the way round skipped: {} on it",
                             bot->GetName(), bot->GetLevel(), n + 1, BlockerName(*on));
                    break;
                }
                bot->GetMotionMaster()->MovePoint(0, at.x, at.y, at.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, true);
                std::lock_guard<std::mutex> guard(lock);
                states[bot->GetGUID().GetCounter()].routeStep = n;
                return true;
            }
            // walked (or blocked from here): on along its last stretch, the direction search from there
            std::lock_guard<std::mutex> guard(lock);
            KitState& state = states[bot->GetGUID().GetCounter()];
            G3D::Vector3 const& last = route.back();
            G3D::Vector3 const& before = route.size() >= 2 ? route[route.size() - 2] : here;
            state.fleeAngle = angle = std::atan2(last.y - before.y, last.x - before.x);
            state.fleeRoute.clear();
            state.routeStep = 0;
        }
        if (trail)
        {
            NewRpgInfo const& info = botAI->rpgInfo;
            std::vector<Blocker> blockers;
            bool searched = false;
            std::string ended = "walked";
            for (uint8 n = trailStep; info.trailMap == bot->GetMapId() && n < info.trailCount; ++n)
            {
                G3D::Vector3 const& at = info.TrailBack(n);
                if (bot->GetExactDist2d(at.x, at.y) <= bot->GetCombatReach())
                    continue;  // reached
                if (!searched)
                {
                    blockers = Blockers(bot, bot->GetExactDist2d(at.x, at.y) + 40.0f);
                    searched = true;
                }
                if (Blocker const* on = StretchBlocker(blockers, here, at, 0.0f, runSpeed))
                {
                    LOG_INFO("playerbots", "[solo] {} L{} survival flee: spot {} back skipped: {} on it",
                             bot->GetName(), bot->GetLevel(), n + 1, BlockerName(*on));
                    ended = "cut";
                    break;
                }
                PathGenerator path(bot);
                path.CalculatePath(at.x, at.y, at.z, false);
                if (path.GetPathType() != PATHFIND_NORMAL)
                {
                    LOG_INFO("playerbots", "[solo] {} L{} survival flee: spot {} back skipped: no navmesh path to it",
                             bot->GetName(), bot->GetLevel(), n + 1);
                    continue;
                }
                bot->GetMotionMaster()->MovePoint(0, at.x, at.y, at.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, true);
                std::lock_guard<std::mutex> guard(lock);
                states[bot->GetGUID().GetCounter()].trailStep = n;
                return true;
            }
            // the way back is walked (or blocked from here): on away from the attackers
            std::lock_guard<std::mutex> guard(lock);
            KitState& state = states[bot->GetGUID().GetCounter()];
            state.fleeTrail = false;
            if (Unit* from = ObjectAccessor::GetUnit(*bot, state.fleeFrom))
                state.fleeAngle = angle = from->GetAbsoluteAngle(bot);
            LOG_INFO("playerbots",
                     "[solo] {} L{} survival flee: the way back {} ({} of {} spots), on away from the "
                     "attackers",
                     bot->GetName(), bot->GetLevel(), ended, trailStep, info.trailCount);
        }
        std::vector<Blocker> const blockers = Blockers(bot, leash + 40.0f);
        std::string blocked;
        for (float turn : {0.0f, float(M_PI) / 4.0f, -float(M_PI) / 4.0f, float(M_PI) / 2.0f, -float(M_PI) / 2.0f})
        {
            float const a = Position::NormalizeOrientation(angle + turn);
            if (Blocker const* on = StretchBlocker(blockers, here, Ahead(here, a, leash), 0.0f, runSpeed))
            {
                if (blocked.empty())
                    blocked = BlockerName(*on);
                continue;
            }
            float x = bot->GetPositionX() + std::cos(a) * leash;
            float y = bot->GetPositionY() + std::sin(a) * leash;
            float z = bot->GetPositionZ();
            bot->UpdateAllowedPositionZ(x, y, z);
            PathGenerator path(bot);
            path.CalculatePath(x, y, z, false);
            if (path.GetPathType() != PATHFIND_NORMAL)
                continue;
            {
                Movement::PointsArray const& points = path.GetPath();
                if (!WayKeepsGround(bot, here, std::vector<G3D::Vector3>(points.begin(), points.end())))
                    continue;  // [regress10] a drop on it
            }
            bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, true);
            {
                std::lock_guard<std::mutex> guard(lock);
                states[bot->GetGUID().GetCounter()].fleeAngle = a;
            }
            return true;
        }
        // [deaths] no stretch clear: the maze search from here before the run ends (b338 Ravelandy L9: "flee stops: no
        // way on clear of other mobs (Murloc Lurker L9 (idle))", it fought five murlocs and died)
        {
            std::vector<std::vector<G3D::Vector3>> const ways = MazeWays(bot, blockers, leash, {});
            std::vector<G3D::Vector3> const* shortest = nullptr;
            float shortestYd = 0.0f;
            for (std::vector<G3D::Vector3> const& way : ways)
            {
                if (StretchBlocker(blockers, here, way.front(), 0.0f, runSpeed))
                    continue;  // its first stretch as the walk checks it
                float yd = 0.0f;
                G3D::Vector3 from = here;
                for (G3D::Vector3 const& corner : way)
                {
                    yd += (corner - from).length();
                    from = corner;
                }
                if (!shortest || yd < shortestYd)
                {
                    shortest = &way;
                    shortestYd = yd;
                }
            }
            if (shortest)
            {
                G3D::Vector3 const& at = shortest->front();
                bot->GetMotionMaster()->MovePoint(0, at.x, at.y, at.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, true);
                LOG_INFO("playerbots", "[solo] {} L{} survival flee: no stretch clear, on round the other mobs ({} "
                         "corners, {:.0f} yd)",
                         bot->GetName(), bot->GetLevel(), shortest->size(), shortestYd);
                std::lock_guard<std::mutex> guard(lock);
                KitState& state = states[bot->GetGUID().GetCounter()];
                state.fleeRoute = *shortest;
                state.routeStep = 0;
                return true;
            }
        }
        // no stretch clear of other mobs: the run ends, the kit judges the fight again (it fights on with all it has)
        ObjectGuid fleeFrom;
        {
            std::lock_guard<std::mutex> guard(lock);
            KitState& state = states[bot->GetGUID().GetCounter()];
            state.fleeing = false;
            state.active = false;
            state.noRun = true;
            fleeFrom = state.fleeFrom;
            RestorePet(bot, state);  // [guild15] it fights on: the pet with it
        }
        SoloSafety::EndFlee(bot);
        // [guild15] it fights its attacker again: the run cleared its target and nothing set it back (b333 Colian L12
        // stood 30 s under a Bloodmyst Hatchling L10 after "flee stops" and died to it at its full health)
        Unit* foe = ObjectAccessor::GetUnit(*bot, fleeFrom);
        if (!foe || !foe->IsAlive() || !foe->IsInCombatWith(bot))
        {
            foe = nullptr;
            for (Unit* attacker : bot->getAttackers())
                if (attacker->IsAlive() && (!foe || bot->GetExactDist(attacker) < bot->GetExactDist(foe)))
                    foe = attacker;
        }
        if (foe)
        {
            context->GetValue<Unit*>("current target")->Set(foe);
            bot->SetTarget(foe->GetGUID());
            if (!PlayerbotAI::IsRanged(bot) && bot->IsWithinMeleeRange(foe))
                bot->Attack(foe, true);
        }
        LOG_INFO("playerbots", "[solo] {} L{} survival flee stops: no way on clear of other mobs ({}), it fights {}",
                 bot->GetName(), bot->GetLevel(), blocked.empty() ? "no navmesh path" : blocked,
                 foe ? foe->GetName() : std::string("nobody"));
        return false;
    }

    Unit* target = ObjectAccessor::GetUnit(*bot, targetGuid);
    if (!target)
        return false;
    if (piece.item)
    {
        Item* item = FindItem(bot, piece.item);
        if (!item || bot->CanUseItem(item) != EQUIP_ERR_OK)
            return false;
        if (piece.needsWindow && bot->isMoving())
            bot->StopMoving();
        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        bot->CastItemUseSpell(item, targets, 1, 0);
        return true;
    }
    // a cast heal on the move: it stops first, the cast goes on the next check
    if (bot->isMoving())
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(piece.spell))
            if (info->CalcCastTime(bot) > 0)
            {
                bot->StopMoving();
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MoveIdle();
                return true;
            }
    bool const cast = botAI->CastSpell(piece.spell, target);
    if (cast)
    {
        {
            std::lock_guard<std::mutex> guard(lock);
            KitState& state = states[bot->GetGUID().GetCounter()];
            if (piece.kind == Kind::Reset)
                state.reset = true;
            else if (piece.kind == Kind::Window)
                state.window = true;
        }
        if (piece.kind == Kind::Reset)
        {
            // the fight ends as a loss (not pulled again at that gap) and it rests to full after
            SoloSafety::BeginFlee(bot, "after a survival reset");
            bot->AttackStop();
            if (bot->GetPet())
                botAI->PetFollow();
        }
    }
    return cast;
}

bool SoloRunPastTrigger::IsActive() { return bot->IsInCombat() && SoloSafety::HasRunPastLeg(bot); }

bool SoloRunPastAction::isUseful() { return SoloSafety::RunPastStep(bot, rest, line); }

bool SoloRunPastAction::Execute(Event /*event*/)
{
    if (rest.size() < 2)
        return false;
    if (!line.empty())
        LOG_INFO("playerbots", "[solo] RUN_PAST {}", line);
    // every damage off, as the kit's run: no swing, no auto shot, no cast, the pet at heel
    if (bot->GetVictim() || bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        bot->AttackStop();
    bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);
    context->GetValue<Unit*>("current target")->Set(nullptr);
    bot->SetTarget(ObjectGuid::Empty);
    if (Pet* pet = bot->GetPet())
        if (pet->GetVictim())
            botAI->PetFollow();
    if (bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONTROLLED))
        return true;
    // the leg's walk goes on: a walk already under way is kept (no new spline every tick)
    if (bot->isMoving() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        return true;
    Movement::PointsArray points(rest.begin(), rest.end());
    return WalkBotPoints(bot, points);
}
