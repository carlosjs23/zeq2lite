/*
Tier transformation requirements.

checkTierUpTransformation gates a transformation on a conjunction of about a
dozen thresholds read from tier.cfg. setupTiers zeroes the config struct before
parsing, so any key a config file omits is left at 0 - and for most of these
thresholds 0 is the permissive value ("needs at least 0 power": always true).

requirementHealthMaximum is the exception. It is an upper bound expressed as a
percentage of maximum:

    ps->powerLevel[plHealth] <= requirementHealthMaximum / 100.0f * ps->powerLevel[plMaximum]

so 0 does not mean "no cap", it means "only while on exactly zero health" - and
a living player never satisfies it. No shipped config sets the key, which made
every transformation in the game silently impossible regardless of power level.

These tests describe tiers the way the shipped configs do - stating only the
thresholds they care about - and assert that the omitted ones stay permissive.
*/

#include <criterion/criterion.h>

#include "g_local.h"
#include "../support/fake_fs.h"

void setupTiers(gclient_t *client);
qboolean checkTierUpTransformation(gclient_t *client, int nextTierIndex,
                                   int currentTierIndex, int tierChangeMode);

#define TIER_CHANGE_KEY_UP 2

static gclient_t client;

/* The two files setupTiers always reads, plus one ascension tier. `extra` is
   appended to tier2 so a test can add the threshold it wants to exercise. */
static void given_tiers(const char *extra) {
	char tier2[512];

	fake_fs_reset();
	memset(&client, 0, sizeof(client));
	client.modelName = "tester";   /* gclient_t holds a pointer, not a buffer */

	fake_fs_add("players/tierDefault.cfg", "tierName Normal\nspeed 1.0\n");
	fake_fs_add("players/tester/tier1/tier.cfg", "tierName Base\n");
	Com_sprintf(tier2, sizeof(tier2), "tierName Super\nrequirementCurrent 2500\n%s",
	            extra ? extra : "");
	fake_fs_add("players/tester/tier2/tier.cfg", tier2);

	setupTiers(&client);
}

/* A healthy fighter, comfortably past tier2's stated 2500 requirement. */
static void given_a_healthy_fighter(void) {
	client.ps.powerLevel[plMaximum] = 20000;
	client.ps.powerLevel[plCurrent] = 20000;
	client.ps.powerLevel[plHealth]  = 20000;
	client.ps.powerLevel[plFatigue] = 20000;
}

/*
The regression: tier2 says nothing about health, so health must not block it.
*/
Test(tiers, a_tier_that_states_no_health_cap_can_be_reached_at_full_health) {
	given_tiers(NULL);
	given_a_healthy_fighter();

	cr_assert(checkTierUpTransformation(&client, 1, 0, TIER_CHANGE_KEY_UP),
	          "a tier config that omits requirementHealthMaximum must not be unreachable");
	cr_assert_eq(client.ps.powerLevel[plTierCurrent], 1, "should have ascended to tier 2");
}

/*
The threshold the key actually exists for still has to bite: a tier that asks
to be entered only while badly hurt must refuse a fighter at full health.
*/
Test(tiers, a_tier_that_demands_low_health_refuses_a_healthy_fighter) {
	given_tiers("requirementHealthMaximum 40\n");
	given_a_healthy_fighter();

	cr_assert(!checkTierUpTransformation(&client, 1, 0, TIER_CHANGE_KEY_UP),
	          "full health should not satisfy a 40%% health cap");
	cr_assert_eq(client.ps.powerLevel[plTierCurrent], 0, "should still be on tier 1");
}

Test(tiers, a_tier_that_demands_low_health_admits_a_wounded_fighter) {
	given_tiers("requirementHealthMaximum 40\n");
	given_a_healthy_fighter();
	client.ps.powerLevel[plHealth] = 5000;   /* 25% of maximum */

	cr_assert(checkTierUpTransformation(&client, 1, 0, TIER_CHANGE_KEY_UP),
	          "a fighter under the health cap should be admitted");
}

/*
The power threshold the configs do state must keep working - this is what
stops you ascending the moment you spawn.
*/
Test(tiers, a_tier_refuses_a_fighter_below_its_power_requirement) {
	given_tiers(NULL);
	given_a_healthy_fighter();
	client.ps.powerLevel[plCurrent] = 1000;   /* tier2 asks for 2500 */

	cr_assert(!checkTierUpTransformation(&client, 1, 0, TIER_CHANGE_KEY_UP),
	          "1000 power should not satisfy a 2500 requirement");
}

/*
setupTiers builds each tier.cfg path and hands it straight to parseTier. It used
to do that with va() plus strcat:

    tierPath = va("players/%s/tier%i/", modelName, i+1);
    parseTier("players/tierDefault.cfg", tier);      <-- engine FS calls run va() in here
    parseTier(strcat(tierPath, "tier.cfg"), tier);   <-- appends onto a clobbered buffer

va() cycles two static buffers, so the intervening open overwrote the one
tierPath pointed at. The resulting path did not resolve, every tier past the
first was marked non-existent, and checkTierUpTransformation refused at its
first guard - transformations were impossible in game while every unit test
passed, because the fake filesystem did not use va() the way the engine does.
*/
Test(tiers, every_tier_config_is_found) {
	given_tiers(NULL);

	cr_assert(client.tiers[1].exists,
	          "tier 2's config must resolve - a clobbered path silently disables the tier");
}

/*
An empty tier.cfg is legitimate: several shipped characters ship a zero-byte
file for their base tier and inherit everything from tierDefault.cfg. Opening
one returns a length of 0, which must not be read as "missing".
*/
Test(tiers, an_empty_tier_config_still_counts_as_a_tier) {
	fake_fs_reset();
	memset(&client, 0, sizeof(client));
	client.modelName = "tester";
	fake_fs_add("players/tierDefault.cfg", "tierName Normal\n");
	fake_fs_add("players/tester/tier1/tier.cfg", "");          /* zero bytes, as goku ships */
	fake_fs_add("players/tester/tier2/tier.cfg", "tierName Super\nrequirementCurrent 2500\n");

	setupTiers(&client);

	cr_assert(client.tiers[0].exists,
	          "a zero-byte tier.cfg inherits tierDefault.cfg and is still a real tier");
}

/*
The "model" cvar still carries Quake III's default of "sarge" (cl_main.c), and
nothing in the shipped config overrides it, so a fresh install joins with a
model that has no players/<model>/tierN/tier.cfg at all. Every tier then reads
as non-existent and checkTierUpTransformation refuses at its first guard -
transformation is impossible and nothing says why.

g_client.c already falls back to the default model when a model ships no .phys
file; tiers now do the same, so an unknown model is playable rather than
silently crippled.
*/
Test(tiers, a_model_with_no_tiers_of_its_own_inherits_the_default_model) {
	fake_fs_reset();
	memset(&client, 0, sizeof(client));
	client.modelName = "sarge";   /* stock Quake III model, no ZEQ2 tier configs */

	fake_fs_add("players/tierDefault.cfg", "tierName Normal\n");
	fake_fs_add("players/goku/tier1/tier.cfg", "");
	fake_fs_add("players/goku/tier2/tier.cfg", "tierName Super\nrequirementCurrent 2500\n");

	setupTiers(&client);

	cr_assert(client.tiers[1].exists,
	          "a model with no tiers of its own must inherit the default model's");
	cr_assert_eq(client.tiers[1].requirementCurrent, 2500,
	             "and inherit its thresholds, not just its existence");
}
