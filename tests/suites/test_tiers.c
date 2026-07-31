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

/*
The parser and the shipped configs have to agree on spelling, and twice they did
not.

The five combat multipliers are written percent* in every config in the data
set, while the parser only knew meleeAttack, energyAttackDamage,
energyAttackCost, defenseMelee and defenseEnergy. Nothing wrote them, so they
kept the zero setupTiers memsets in - and a zero multiplier is not a weak
fighter, it is one whose attacks compute to no damage and whose defense scales
nothing. These are the numbers everything else in combat is measured against, so
they get asserted by value rather than by "non-zero".
*/
Test(tiers, the_combat_multipliers_are_read_under_the_names_the_configs_use) {
	fake_fs_reset();
	memset(&client, 0, sizeof(client));
	client.modelName = "tester";

	fake_fs_add("players/tierDefault.cfg", "tierName Normal\n");
	fake_fs_add("players/tester/tier1/tier.cfg",
	            "percentMeleeAttack 1.5\n"
	            "percentEnergyAttackDamage 2.0\n"
	            "percentEnergyAttackCost 0.5\n"
	            "percentMeleeDefense 1.25\n"
	            "percentEnergyDefense 0.75\n");

	setupTiers(&client);

	cr_assert_float_eq(client.ps.baseStats[stMeleeAttack], 1.5f, 0.001f,
	                   "percentMeleeAttack must reach baseStats or melee lands for nothing");
	cr_assert_float_eq(client.ps.baseStats[stEnergyAttack], 2.0f, 0.001f,
	                   "percentEnergyAttackDamage must reach baseStats or ki attacks charge to 0");
	cr_assert_float_eq(client.ps.baseStats[stEnergyAttackCost], 0.5f, 0.001f, "energy cost multiplier");
	cr_assert_float_eq(client.ps.baseStats[stDefenseMelee], 1.25f, 0.001f, "melee defense multiplier");
	cr_assert_float_eq(client.ps.baseStats[stDefenseEnergy], 0.75f, 0.001f, "energy defense multiplier");
}

/*
knockBackPower is stated by every config and was parsed all along, but syncTier
never handed it to the playerState, so the melee knockback that reads it saw
whatever else lived at that index.
*/
Test(tiers, the_knockback_multiplier_reaches_the_player_state) {
	fake_fs_reset();
	memset(&client, 0, sizeof(client));
	client.modelName = "tester";

	fake_fs_add("players/tierDefault.cfg", "tierName Normal\n");
	fake_fs_add("players/tester/tier1/tier.cfg", "knockBackPower 2.0\n");

	setupTiers(&client);

	cr_assert_float_eq(client.ps.baseStats[stKnockbackPower], 2.0f, 0.001f,
	                   "a tier's knockback power must reach baseStats to be readable");
}

/*
The guard knobs - defenseCapacity, defenseRecovery, defenseRecoveryDelay - and
knockbackIntensity were parsed into the tier config but syncTier never handed
them to the playerState, so a tier could state them and change nothing.
*/
Test(tiers, the_guard_knobs_reach_the_player_state) {
	fake_fs_reset();
	memset(&client, 0, sizeof(client));
	client.modelName = "tester";

	fake_fs_add("players/tierDefault.cfg", "tierName Normal\n");
	fake_fs_add("players/tester/tier1/tier.cfg",
	            "defenseCapacity 2.0\n"
	            "defenseRecovery 1.5\n"
	            "defenseRecoveryDelay 2000\n"
	            "knockbackIntensity 0.8\n");

	setupTiers(&client);

	cr_assert_float_eq(client.ps.baseStats[stDefenseCapacity], 2.0f, 0.001f,
	                   "a tier's guard capacity must reach baseStats to be readable");
	cr_assert_float_eq(client.ps.baseStats[stDefenseRecovery], 1.5f, 0.001f,
	                   "a tier's guard recovery must reach baseStats to be readable");
	cr_assert_float_eq(client.ps.baseStats[stDefenseRecoveryDelay], 2000.0f, 0.001f,
	                   "a tier's recovery delay must reach baseStats to be readable");
	cr_assert_float_eq(client.ps.baseStats[stKnockbackIntensity], 0.8f, 0.001f,
	                   "a tier's knockback intensity must reach baseStats to be readable");
}

/*
No shipped config states the guard knobs, so the multipliers must come through
neutral rather than as the zero the memset left - a guard with a capacity of
zero would divide by nothing and one with a recovery of zero would never refill.
The delay is the exception: 0 means "unset", and pmove substitutes its stock
delay for it.
*/
Test(tiers, unstated_guard_knobs_are_neutral_not_zero) {
	given_tiers(NULL);   /* configs as shipped: no guard keys anywhere */

	cr_assert_float_eq(client.ps.baseStats[stDefenseCapacity], 1.0f, 0.001f,
	                   "an unstated guard capacity must be neutral");
	cr_assert_float_eq(client.ps.baseStats[stDefenseRecovery], 1.0f, 0.001f,
	                   "an unstated guard recovery must be neutral");
	cr_assert_float_eq(client.ps.baseStats[stDefenseRecoveryDelay], 0.0f, 0.001f,
	                   "an unstated recovery delay must stay 0 so pmove uses its stock delay");
	cr_assert_float_eq(client.ps.baseStats[stKnockbackIntensity], 1.0f, 0.001f,
	                   "an unstated knockback intensity must be neutral");
}

/*
goku's tier5 abbreviates requirementMaximum to requirementMax. Unread it stays
at zero, and zero is the permissive value for a lower bound, so the tier admits
a fighter who has never come near the maximum power it asks for.
*/
Test(tiers, a_tier_that_abbreviates_its_maximum_requirement_is_still_gated) {
	given_tiers("requirementMax 30000\n");
	given_a_healthy_fighter();   /* maximum 20000, short of the 30000 asked for */

	cr_assert(!checkTierUpTransformation(&client, 1, 0, TIER_CHANGE_KEY_UP),
	          "requirementMax must gate the tier the same as requirementMaximum");

	client.ps.powerLevel[plMaximum] = 30000;
	cr_assert(checkTierUpTransformation(&client, 1, 0, TIER_CHANGE_KEY_UP),
	          "and must admit a fighter who meets it");
}

/*
The quick zanzoken - the double tap - reads its distance and cost from
zanzokenQuickDistance and zanzokenQuickCost, which no shipped config writes. The
two cvars that would override them default to -1, meaning "use the tier's
value", so the distance resolved to zero and a double tap spent its event and
its animation to move the player nowhere.
*/
Test(tiers, a_double_tap_zanzoken_falls_back_to_the_ordinary_one) {
	fake_fs_reset();
	memset(&client, 0, sizeof(client));
	client.modelName = "tester";

	fake_fs_add("players/tierDefault.cfg", "tierName Normal\n");
	fake_fs_add("players/tester/tier1/tier.cfg",
	            "zanzokenDistance 1.0\nzanzokenCost 2.0\n");   /* no quick pair, as shipped */

	setupTiers(&client);

	cr_assert_float_eq(client.ps.baseStats[stZanzokenQuickDistance], 1.0f, 0.001f,
	                   "a quick zanzoken with no distance of its own teleports nowhere");
	cr_assert_float_eq(client.ps.baseStats[stZanzokenQuickCost], 2.0f, 0.001f,
	                   "and must not be free either");
}

/*
A config that does state the quick pair keeps it - the fallback above must not
overwrite a character who was tuned for a shorter, cheaper double tap.
*/
Test(tiers, a_stated_quick_zanzoken_is_left_alone) {
	fake_fs_reset();
	memset(&client, 0, sizeof(client));
	client.modelName = "tester";

	fake_fs_add("players/tierDefault.cfg", "tierName Normal\n");
	fake_fs_add("players/tester/tier1/tier.cfg",
	            "zanzokenDistance 1.0\nzanzokenCost 2.0\n"
	            "zanzokenQuickDistance 0.4\nzanzokenQuickCost 0.5\n");

	setupTiers(&client);

	cr_assert_float_eq(client.ps.baseStats[stZanzokenQuickDistance], 0.4f, 0.001f, "stated distance wins");
	cr_assert_float_eq(client.ps.baseStats[stZanzokenQuickCost], 0.5f, 0.001f, "stated cost wins");
}
