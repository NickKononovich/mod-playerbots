/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SurvivalKit.h"

#include "CellImpl.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Bag.h"
#include "Item.h"
#include "Log.h"
#include "PathGenerator.h"
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
        std::string why;
    };

    // the idle aggressive mobs round it (a way out must pass them out of their aggro reach)
    std::vector<Creature*> IdleMobs(Player* bot, float range)
    {
        std::vector<Creature*> idle;
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);
        for (Unit* unit : units)
        {
            Creature* mob = unit->ToCreature();
            if (!mob || !mob->IsAlive() || mob->IsInCombat() || !mob->IsHostileTo(bot) ||
                !mob->HasReactState(REACT_AGGRESSIVE) || mob->GetCharmerOrOwnerGUID() ||
                mob->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE))
                continue;
            idle.push_back(mob);
        }
        return idle;
    }

    bool WayClear(Player* bot, float angle, float length, std::vector<Creature*> const& idle)
    {
        float const x0 = bot->GetPositionX();
        float const y0 = bot->GetPositionY();
        float const dx = std::cos(angle);
        float const dy = std::sin(angle);
        for (Creature* mob : idle)
        {
            // the mob's nearest approach to the way out
            float const mx = mob->GetPositionX() - x0;
            float const my = mob->GetPositionY() - y0;
            float const along = std::clamp(mx * dx + my * dy, 0.0f, length);
            float const off = std::hypot(mx - along * dx, my - along * dy);
            if (off <= mob->GetAggroRange(bot) + bot->GetCombatReach())
                return false;
        }
        return true;
    }

    FleePlan PlanFlee(PlayerbotAI* botAI, Player* bot, SoloSafety::Projection const& p, std::vector<Piece> const& kit)
    {
        FleePlan plan;
        float const leashRadius = sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS);
        if (leashRadius <= 0.0f)
        {
            plan.why = "no leash";
            return plan;
        }
        time_t const now = GameTime::GetGameTime().count();
        // the best speed piece / chaser slow it can cast now
        Piece const* speed = nullptr;
        Piece const* snare = nullptr;
        Piece const* leap = nullptr;
        for (Piece const& piece : kit)
        {
            if (piece.kind == Kind::Speed && (!speed || piece.pct > speed->pct) &&
                Castable(botAI, bot, piece, bot))
                speed = &piece;
            else if (piece.kind == Kind::Snare && (!snare || piece.pct < snare->pct) &&
                     Castable(botAI, bot, piece, PieceTarget(bot, piece, p)))
                snare = &piece;
            else if (piece.kind == Kind::Leap && (!leap || piece.radius > leap->radius) && Castable(botAI, bot, piece, bot))
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
            Position const at = foe.unit->ToCreature() ? foe.unit->ToCreature()->GetHomePosition() : foe.unit->GetPosition();
            homeX += at.GetPositionX();
            homeY += at.GetPositionY();
        }
        home.Relocate(homeX / float(p.foes.size()), homeY / float(p.foes.size()), bot->GetPositionZ());
        float away = home.GetAbsoluteAngle(bot);
        if (home.GetExactDist2d(bot) < bot->GetCombatReach())
            away = Position::NormalizeOrientation(p.foes.front().unit->GetOrientation() + float(M_PI));

        bool planned = false;
        for (Piece const* first : {static_cast<Piece const*>(nullptr), snare, speed, leap})
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
                if (!mob)
                {
                    plan.why = "a player attacker";
                    return plan;
                }
                float vm = mob->GetSpeed(moveType);
                float leashLeft = float(mob->GetLastLeashExtensionTime() + time_t(mob->GetLeashTimer()) - now);
                // a spell on the chaser refreshes its leash (Unit::AtTargetAttacked): its full timer from the cast
                if (first && first->kind == Kind::Snare && (first->area || i == 0))
                {
                    leashLeft = float(mob->GetLeashTimer()) + first->castSec;
                    vm *= 1.0f + first->pct;
                }
                leashLeft = std::max(leashLeft, 0.0f);
                // the mob's distance from where it resets (its spawn; a wanderer's reset spot lies within its wander
                // distance of it)
                float const fromHome =
                    std::max(0.0f, mob->GetHomePosition().GetExactDist2d(mob) - mob->GetWanderDistance());
                float const need = std::max(0.0f, leashRadius - fromHome);
                float const followSpeed = std::max(std::min(vm, vb), 0.1f);
                float const until = std::max(leashLeft, need / followSpeed) + 1.0f;  // + ThreatManager's 1 s update
                // a faster chaser hits all the way (dazing from behind: Unit::AttackerStateUpdate); a slower one
                // until the gap passes its melee reach
                float exposure = until;
                float const reach = mob->GetMeleeRange(bot);
                // a slower chaser in reach can daze it down below its own speed: then it hits all the way too
                bool const canDaze = std::max(foe.dps - foe.spellDps, 0.0f) > 0.0f && head <= 0.0f && bot->IsWithinMeleeRange(mob) &&
                                     vb * (1.0f + dazed) <= vm;
                if (vm < vb && !canDaze)
                    exposure = std::min(until, std::max(0.0f, reach - head) / (vb - vm));
                else if (head > 0.0f)
                    exposure = std::max(0.0f, until - head / std::max(vm, 0.1f));
                float const melee = std::max(foe.dps - foe.spellDps, 0.0f);
                float const spell = std::min(foe.spellDps, foe.dps);
                damage += melee * exposure + spell * until;
                longest = std::max(longest, until);
            }
            if (first)
                damage += p.incoming * first->castSec;
            if (!planned || damage < plan.damage)
            {
                plan.damage = damage;
                plan.leashSec = longest;
                plan.first = first;
            }
            planned = true;
        }
        plan.dist = botSpeed * plan.leashSec;
        // the way out: straight away first, then to the sides
        std::vector<Creature*> const idle = IdleMobs(bot, plan.dist + 40.0f);
        bool clear = false;
        for (float turn : {0.0f, float(M_PI) / 4.0f, -float(M_PI) / 4.0f, float(M_PI) / 2.0f, -float(M_PI) / 2.0f})
        {
            float const angle = Position::NormalizeOrientation(away + turn);
            if (WayClear(bot, angle, plan.dist, idle))
            {
                plan.angle = angle;
                clear = true;
                break;
            }
        }
        if (!clear)
        {
            plan.why = "no way out clear of other mobs";
            return plan;
        }
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
            flee = PlanFlee(botAI, bot, p, pieces);
        if (flee.ok)
        {
            Piece run;
            run.kind = Kind::Flee;
            if (flee.first)
                run = *flee.first;
            state.choice = run;
            state.target = flee.first ? PieceTarget(bot, *flee.first, p)->GetGUID() : bot->GetGUID();
            state.fleeAngle = flee.angle;
            state.fleeFrom = p.foes.front().unit->GetGUID();
            state.why = Acore::StringFormat("leash={:.0f}s dist={:.0f} run costs {:.0f} at hp {:.0f}; {}", flee.leashSec,
                                            flee.dist, flee.damage, p.health, numbers);
            state.active = true;
            return true;
        }

        // 4. everything it has, the biggest piece first
        Scored const* best = nullptr;
        for (Scored const& s : scored)
            if (s.due && (!best || s.gain > best->gain))
                best = &s;
        if (!best)
            return false;
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
        float const leash = sWorld->getFloatConfig(CONFIG_CREATURE_LEASH_RADIUS);
        std::vector<Creature*> const idle = IdleMobs(bot, leash + 40.0f);
        for (float turn : {0.0f, float(M_PI) / 4.0f, -float(M_PI) / 4.0f, float(M_PI) / 2.0f, -float(M_PI) / 2.0f})
        {
            float const a = Position::NormalizeOrientation(angle + turn);
            if (!WayClear(bot, a, leash, idle))
                continue;
            float x = bot->GetPositionX() + std::cos(a) * leash;
            float y = bot->GetPositionY() + std::sin(a) * leash;
            float z = bot->GetPositionZ();
            bot->UpdateAllowedPositionZ(x, y, z);
            PathGenerator path(bot);
            path.CalculatePath(x, y, z, false);
            if (path.GetPathType() != PATHFIND_NORMAL)
                continue;
            bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, true);
            {
                std::lock_guard<std::mutex> guard(lock);
                states[bot->GetGUID().GetCounter()].fleeAngle = a;
            }
            return true;
        }
        // no stretch clear of other mobs: the run ends, the kit judges the fight again (it fights on with all it has)
        {
            std::lock_guard<std::mutex> guard(lock);
            KitState& state = states[bot->GetGUID().GetCounter()];
            state.fleeing = false;
            state.active = false;
            state.noRun = true;
            RestorePet(bot, state);  // [guild15] it fights on: the pet with it
        }
        SoloSafety::EndFlee(bot);
        LOG_INFO("playerbots", "[solo] {} L{} survival flee stops: no way on clear of other mobs", bot->GetName(),
                 bot->GetLevel());
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
