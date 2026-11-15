/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChaseModel.h"

namespace Chase
{
    Window Of(float botSpeed, float mobSpeed, float leashSec, float leashRadius, float dazedShare, float melee,
              float reach)
    {
        Window out;
        // [runpast89] THE DAZE IS A CONSEQUENCE OF THE SWING, not of the speeds.
        //
        // It used to be priced only where the mob was ALREADY faster (`mobSpeed > botSpeed`), and that is the one
        // case where it changes nothing: a faster mob catches up with or without it. Where it decides the chase - a
        // bot ahead by a hair, which one dazing swing drops to half speed and behind - no daze was priced at all.
        // b502: Tuihgrelfim L24 at 9.1 yd/s against a Barrens Guard L40 at 8.0 was priced 69 dmg over 4.545 s of
        // landed melee (under a third of the 14 s chase) and took 414 over 16.3 s; Irrua L25 the same guard entry.
        // The question is not who is faster now but whether a hit in the back can make the mob the faster one, so
        // the speed the chase is judged on is the bot's speed AFTER the daze, for every mob whose swings land.
        float const underSwings = melee > 0.0f ? botSpeed * (1.0f + dazedShare) : botSpeed;
        // [runpast89] AND THE CHASE ENDS WHEN THE MOB STOPS LANDING HITS, not on either term of a leash.
        //
        // In the core the leash TIMER is a window since the last leash extension (Creature::CanCreatureAttack,
        // Creature.cpp:2740), and Unit::DealDamage (Unit.cpp:1275) calls UpdateLeashExtensionTime on EVERY hit - so
        // a chaser that is hitting restarts its own timer. That branch is tested BEFORE the distance check and
        // answers "keep attacking", so the leash RADIUS never comes into it either: b502 recorded
        // "[lag355] leash=13s dist=91", a guard 91 yd from its home at a CreatureLeashRadius of 30, still swinging.
        // So a mob the bot cannot outrun - after the daze its own swings apply - never stops landing hits, and its
        // chase has no end. Lanbuzz L26 was priced at 14 s and chased for 29.4 s, taking 911 against a price of 577.
        // That infinity is the honest answer and not a number to soften: a run past a chaser that cannot be shaken
        // off is not a cheap run past, it is no run past at all, and the price says so through `ends`.
        if (underSwings <= mobSpeed && melee > 0.0f)
        {
            out.ends = false;
            out.until = std::numeric_limits<float>::infinity();
            out.exposure = out.until;
            return out;
        }
        if (melee > 0.0f)
        {
            // it pulls away even under the daze: the melee lands until it is out of reach, and from that last hit
            // the leash timer the core gives the mob runs its course (+ ThreatManager's 1 s, as the flee)
            out.exposure = reach / (underSwings - mobSpeed);
            out.until = out.exposure + leashSec + 1.0f;
            return out;
        }
        // its swings land nothing, so nothing extends its leash: the window is the one the core gives it, and the
        // bot's own speed carries it out of the radius
        float const follow = std::max(std::min(mobSpeed, botSpeed), 0.1f);
        out.until = std::max(leashSec, leashRadius / follow) + 1.0f;
        out.exposure = 0.0f;
        return out;
    }

    // [runpast89] THE HEAL A RUN PAST MAY COUNT, run forward in time instead of summed.
    //
    // What it replaces: `SurvivesRunPast` compared the damage with `GetHealth() + SurvivalKit::HealPoints(bot,
    // seconds, mana, true, true)`, and that term handed the WHOLE mana pool over as health to come - as many casts as
    // the mana paid for, each worth its full amount, every heal item of every shared cooldown on top, and
    // `incomingDps` 0 at every run-past call site. Measured on the b502 hour against the bots' own bodies: Irrua L25
    // judged against 1207 for a body of 797 (+51 %), Tuihgrelfim L24 956 for 616 (+55 %), Lanbuzz L26 924 for 760
    // (+22 %), and Colian L26 926 for 926 - exact, and the one of the four with no mana. Orenem L26 was priced
    // "931 hp vs hp 1683" against a body of 487.
    //
    // Two facts the sum could not carry, and they need no number of ours:
    //   * a heal puts back only the health the chase has already taken off. The bot cannot be healed above its
    //     maximum, so a cast landing on a body the chase has barely scratched is overheal and credit for it is
    //     invented health.
    //   * nothing lands after the bot is dead. A pool is a number for the END of the chase; the bot dies at the
    //     first moment its health reaches zero, and heals due later never happen.
    // So the chase is walked forward at its own damage per second, the heal lands at the pace its own data allows
    // (its cooldown, else the global cooldown its cast time already carries), the mana pays for as many as it pays
    // for, and the walk stops at zero. `health + healed` is then both the pool the line prints and the verdict.
    Survival Under(Heal const& heal, float chaseSec, float damage, float health, float maxHealth, float mana)
    {
        Survival out;
        out.lives = health > 0.0f;
        if (chaseSec <= 0.0f || !(chaseSec < std::numeric_limits<float>::infinity()))
            return out;
        float const dps = std::max(damage, 0.0f) / chaseSec;
        // one heal per its own recovery, or - with none - one per the cast its data gives it, the global cooldown
        // included (SurvivalKit's Piece::castSec is the cast or the global cooldown, whichever is longer)
        float const step = std::max(heal.cooldownSec, std::max(heal.castSec, 0.1f));
        float casts = heal.amount > 0.0f ? (heal.cost > 0.0f ? std::floor(std::max(mana, 0.0f) / heal.cost)
                                                             : std::numeric_limits<float>::max())
                                         : 0.0f;
        if (heal.uses >= 0)
            casts = std::min(casts, float(heal.uses));
        float hp = health;
        float at = 0.0f;
        for (float next = step; at < chaseSec; next += step)
        {
            float const to = std::min(next, chaseSec);
            hp -= dps * (to - at);
            at = to;
            if (hp <= 0.0f)
            {
                out.lives = false;
                out.lostAtSec = at;
                return out;
            }
            if (at >= chaseSec || casts < 1.0f)
                continue;
            // a heal over time gives only the share of itself the chase has left to run
            float const lands = heal.durationSec > 0.0f
                                    ? heal.amount * std::min(1.0f, (chaseSec - at) / heal.durationSec)
                                    : heal.amount;
            float const put = std::min(lands, std::max(0.0f, maxHealth - hp));
            hp += put;
            out.healed += put;
            casts -= 1.0f;
        }
        out.lives = hp > 0.0f;
        return out;
    }
}
