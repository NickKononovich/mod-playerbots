/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// [jrn50] THE INSTRUMENT'S OWN TESTS: feed a recorded decision's `fields` and demand that each value reaches the
// column it is meant for. This is the class of test the gap asked for - pb_journal_actor had no bot row at all and
// every mob's level, health and mana was NULL, and the two DeathReplay tests skip for exactly that reason.
//
// Every number below is the shape the emitters now write (LeaderPlan's planner_answer / pull_order and GuildFives'
// five_enter), not an invented one. The redness was proved by putting today's reader back: with the pre-[jrn50]
// body, FiveMembersBecomeBotRows / TheLookTargetBecomesAMobRow / ASupplyFactReachesItsColumn /
// MobHealthAndLevelReachTheirColumns all failed, because the five, the look target and the supply counts were not
// read at all.

#include "ActorFacts.h"

#include <gtest/gtest.h>

using Journal::Facts::ActorFact;
using Journal::Facts::ActorsFromFields;
using Journal::Facts::Share;

namespace
{
    // one planner_answer's fields as the leader writes them: the five, the mob looked at, the pack, the wanderers
    char const* const FIELDS =
        "{\"answer\":\"held\",\"kind\":\"engage\",\"why\":\"Rtshaab at 11 % mana (needs 43 %)\","
        "\"judge\":\"Bierya\",\"judgeLow\":157,"
        "\"five\":["
        "{\"name\":\"Bierya\",\"low\":157,\"level\":21,\"role\":\"tank\",\"spec\":\"protection\","
        "\"hp\":1820,\"hpMax\":2040,\"mana\":112,\"manaMax\":2470,\"x\":-161.7,\"y\":133.1,\"z\":-73.5,"
        "\"tankYd\":0.0,\"targetYd\":34.2,\"combat\":false,\"openerReady\":true,\"drinks\":0,\"foods\":4,"
        "\"drinksNeed\":19,\"doing\":\"sits\"},"
        "{\"name\":\"Rtshaab\",\"low\":161,\"level\":20,\"role\":\"healer\",\"spec\":\"holy\","
        "\"hp\":980,\"hpMax\":980,\"mana\":0,\"manaMax\":2200,\"x\":-160.0,\"y\":130.0,\"z\":-73.5,"
        "\"tankYd\":3.4,\"combat\":false,\"drinks\":2,\"foods\":0,\"drinksNeed\":21}],"
        "\"lookAt\":{\"name\":\"Deviate Ravager\",\"low\":157,\"entry\":3637,\"level\":19,\"hp\":0,"
        "\"hpMax\":430,\"x\":-100.8,\"y\":180.8,\"z\":-73.1,\"tankYd\":77.5,\"combat\":false,"
        "\"wanders\":true},"
        "\"packAt\":[{\"low\":157,\"entry\":3637,\"x\":-100.8,\"y\":180.8,\"z\":-73.1,\"tankYd\":77.5,"
        "\"wanders\":true,\"level\":19,\"hp\":215,\"hpMax\":430,\"combat\":false}],"
        "\"wanderArea\":[{\"spawn\":18678,\"checks\":\"reaches spot,pull path,walk\"}]}";

    ActorFact const* Find(std::vector<ActorFact> const& actors, char const* note, uint32_t low)
    {
        for (ActorFact const& a : actors)
            if (a.note == note && a.low == low)
                return &a;
        return nullptr;
    }
}  // namespace

// the gap itself: the five's bots were not in the table at all, so "put the same conditions back" had nothing to
// put back
TEST(ActorFacts, FiveMembersBecomeBotRows)
{
    std::vector<ActorFact> const actors = ActorsFromFields(FIELDS);
    ActorFact const* tank = Find(actors, "five", 157);
    ASSERT_NE(tank, nullptr) << "the five's bots are missing from the participants";
    EXPECT_FALSE(tank->mob) << "a bot of the five is not a mob";
    EXPECT_EQ(tank->name, "Bierya");
    ASSERT_TRUE(tank->level.has_value());
    EXPECT_EQ(*tank->level, 21u);
    ASSERT_TRUE(tank->role.has_value());
    EXPECT_EQ(*tank->role, "tank");
    ASSERT_TRUE(tank->spec.has_value());
    EXPECT_EQ(*tank->spec, "protection");
    ASSERT_TRUE(tank->openerReady.has_value());
    EXPECT_TRUE(*tank->openerReady);
    ASSERT_TRUE(tank->doing.has_value());
    EXPECT_EQ(*tank->doing, "sits");
}

// the wc-n failure: the tank's mana at the moment of the hold, beside its pool - "11 %" with no pool behind it is
// not checkable
TEST(ActorFacts, AShareNeverTravelsWithoutItsDenominator)
{
    std::vector<ActorFact> const actors = ActorsFromFields(FIELDS);
    ActorFact const* tank = Find(actors, "five", 157);
    ASSERT_NE(tank, nullptr);
    ASSERT_TRUE(tank->mana.has_value());
    ASSERT_TRUE(tank->manaMax.has_value());
    EXPECT_EQ(*tank->mana, 112u);
    EXPECT_EQ(*tank->manaMax, 2470u);
    ASSERT_TRUE(tank->manaPct.has_value());
    EXPECT_NEAR(*tank->manaPct, 4.53f, 0.01f);
    ASSERT_TRUE(tank->healthPct.has_value());
    EXPECT_NEAR(*tank->healthPct, 89.2f, 0.1f);

    // a pool of zero is not a share of zero, and a share with no pool is not a share
    EXPECT_FALSE(Share(std::optional<uint32_t>(10), std::optional<uint32_t>(0)).has_value());
    EXPECT_FALSE(Share(std::optional<uint32_t>(10), std::nullopt).has_value());
    EXPECT_FALSE(Share(std::nullopt, std::optional<uint32_t>(100)).has_value());
}

// the empty bar is a measured zero and must stay one: a healer at 0 mana is the finding, and NULL would hide it
TEST(ActorFacts, AMeasuredZeroIsNotAnAbsentValue)
{
    std::vector<ActorFact> const actors = ActorsFromFields(FIELDS);
    ActorFact const* healer = Find(actors, "five", 161);
    ASSERT_NE(healer, nullptr);
    ASSERT_TRUE(healer->mana.has_value()) << "0 mana is a measurement, not a gap";
    EXPECT_EQ(*healer->mana, 0u);
    ASSERT_TRUE(healer->manaPct.has_value());
    EXPECT_NEAR(*healer->manaPct, 0.0f, 0.001f);
    // what the emitter did not write stays absent: this member's spot has no targetYd and no openerReady
    EXPECT_FALSE(healer->targetYd.has_value()) << "an absent key must not become 0";
    EXPECT_FALSE(healer->openerReady.has_value());
    EXPECT_TRUE(healer->level.has_value()) << "level was written for this member";
}

// the failure that could only be found with a LIKE over the text: the tank carried no water
TEST(ActorFacts, ASupplyFactReachesItsColumn)
{
    std::vector<ActorFact> const actors = ActorsFromFields(FIELDS);
    ActorFact const* tank = Find(actors, "five", 157);
    ASSERT_NE(tank, nullptr);
    ASSERT_TRUE(tank->drinks.has_value()) << "drinks in the bags is a column now, not a word in a sentence";
    EXPECT_EQ(*tank->drinks, 0u);
    ASSERT_TRUE(tank->foods.has_value());
    EXPECT_EQ(*tank->foods, 4u);
    ASSERT_TRUE(tank->drinksNeed.has_value()) << "what the run was priced at, beside what it carries";
    EXPECT_EQ(*tank->drinksNeed, 19u);
}

// the mob the answer is about, with its own numbers: a corpse (0 of 430) is distinguishable from a mob never read
TEST(ActorFacts, TheLookTargetBecomesAMobRow)
{
    std::vector<ActorFact> const actors = ActorsFromFields(FIELDS);
    ActorFact const* look = Find(actors, "look target", 157);
    ASSERT_NE(look, nullptr) << "the mob the decision is about is missing from the participants";
    EXPECT_TRUE(look->mob);
    EXPECT_EQ(look->entry, 3637u) << "identity is entry plus guid, never the name";
    ASSERT_TRUE(look->health.has_value());
    EXPECT_EQ(*look->health, 0u);
    ASSERT_TRUE(look->healthPct.has_value());
    EXPECT_NEAR(*look->healthPct, 0.0f, 0.001f);
    ASSERT_TRUE(look->level.has_value());
    EXPECT_EQ(*look->level, 19u);
}

TEST(ActorFacts, MobHealthAndLevelReachTheirColumns)
{
    std::vector<ActorFact> const actors = ActorsFromFields(FIELDS);
    ActorFact const* mob = Find(actors, "pack member", 157);
    ASSERT_NE(mob, nullptr);
    ASSERT_TRUE(mob->level.has_value()) << "the mob's level was NULL in every row of the live table";
    EXPECT_EQ(*mob->level, 19u);
    ASSERT_TRUE(mob->health.has_value());
    EXPECT_EQ(*mob->health, 215u);
    ASSERT_TRUE(mob->healthMax.has_value());
    EXPECT_EQ(*mob->healthMax, 430u);
    ASSERT_TRUE(mob->healthPct.has_value());
    EXPECT_NEAR(*mob->healthPct, 50.0f, 0.01f);
    EXPECT_EQ(mob->entry, 3637u);
    ASSERT_TRUE(mob->z.has_value()) << "a pull is judged in three dimensions";
}

// [jrn50] the opener gate's own reading, and the three answers it can give. 121 pulls were refused on this fact
// with the column empty beside them; now it travels on the verdict that computed it (ChainPull::openerReady ->
// the chain_pull line's `five`), and the three states must stay three.
TEST(ActorFacts, TheOpenerFactKeepsItsThreeAnswers)
{
    // the gate ran and said yes
    ActorFact const* ready = Find(ActorsFromFields(FIELDS), "five", 157);
    ASSERT_NE(ready, nullptr);
    ASSERT_TRUE(ready->openerReady.has_value());
    EXPECT_TRUE(*ready->openerReady);

    // the gate ran and said no: every opener cooling down, or none known - one fact to the gate, and it is a
    // MEASURED false. A reader that turned absent into false would make this indistinguishable from the third case.
    char const* const refused = "{\"five\":[{\"name\":\"Bierya\",\"low\":157,\"openerReady\":false}]}";
    ActorFact const* cooling = Find(ActorsFromFields(refused), "five", 157);
    ASSERT_NE(cooling, nullptr);
    ASSERT_TRUE(cooling->openerReady.has_value()) << "a refusal on the opener is a measured false, not a gap";
    EXPECT_FALSE(*cooling->openerReady);

    // the gate never ran: out of combat no chain pull is judged at all, so the quantity did not exist. That is
    // NULL - "it did not exist then" - and never "its opener was not ready".
    char const* const never = "{\"five\":[{\"name\":\"Bierya\",\"low\":157,\"hp\":1820,\"hpMax\":2040}]}";
    ActorFact const* unasked = Find(ActorsFromFields(never), "five", 157);
    ASSERT_NE(unasked, nullptr);
    EXPECT_FALSE(unasked->openerReady.has_value())
        << "the opener gate was never reached: that is not a tank whose opener was not ready";
}

// [jrn50] TWO INSTANTS MUST NOT COLLAPSE INTO ONE ROW. One decision is written from several lines and they are not
// the same moment: the planner answers out of combat, the chain pull is judged in the fight. The journal keeps one
// row per distinct participant of a decision, so without a note of its own the fight's reading would be merged
// into - in practice, dropped in favour of - the row written first, and the opener fact would be lost exactly
// where it exists. The note is what keeps them two rows.
TEST(ActorFacts, AnEmittersOwnInstantGetsItsOwnRow)
{
    char const* const both =
        "{\"five\":[{\"name\":\"Bierya\",\"low\":157,\"hp\":1820,\"hpMax\":2040}],"
        "\"packAt\":[{\"note\":\"five at chain pull\",\"low\":157,\"hp\":900,\"hpMax\":2040,"
        "\"openerReady\":false}]}";
    std::vector<ActorFact> const actors = ActorsFromFields(both);
    ActorFact const* answer = Find(actors, "five", 157);
    ActorFact const* chain = Find(actors, "five at chain pull", 157);
    ASSERT_NE(answer, nullptr);
    ASSERT_NE(chain, nullptr) << "the emitter named its own instant and must get a row of its own";
    // the same bot, two moments, two different healths - and neither overwrote the other
    ASSERT_TRUE(answer->health.has_value());
    ASSERT_TRUE(chain->health.has_value());
    EXPECT_EQ(*answer->health, 1820u);
    EXPECT_EQ(*chain->health, 900u);
}

// what the judge did NOT write must stay NULL: the wanderers are named by their spawn, with no health at all
TEST(ActorFacts, AbsentIsNotZero)
{
    std::vector<ActorFact> const actors = ActorsFromFields(FIELDS);
    ActorFact const* spawn = nullptr;
    for (ActorFact const& a : actors)
        if (a.note == "wander area")
            spawn = &a;
    ASSERT_NE(spawn, nullptr);
    EXPECT_EQ(spawn->spawnId, 18678u);
    EXPECT_FALSE(spawn->health.has_value()) << "a spawn with no health written is a gap, not a corpse";
    EXPECT_FALSE(spawn->level.has_value());
    ASSERT_TRUE(spawn->chkReachesSpot.has_value());
    EXPECT_TRUE(*spawn->chkReachesSpot);
    ASSERT_TRUE(spawn->chkWalk.has_value());
    EXPECT_TRUE(*spawn->chkWalk);
    ASSERT_TRUE(spawn->chkWalksIntoFight.has_value());
    EXPECT_FALSE(*spawn->chkWalksIntoFight) << "a check the spawn did not pass is a rejected candidate";
}
