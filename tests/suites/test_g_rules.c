/*
Rule engine: tag vocabulary, parser, load-time validation, matcher and the
executor for inline test vectors.

Most tests declare their .rules and tags.def inline through fake_fs, because the
error wordings are the feature under test and reading them next to the content
that provokes them is the point. The last group instead loads the shipped
content from GameData/rules and runs the vectors authored in it, which is the
gate that keeps content honest without launching the game.
*/

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g_local.h"
#include "fake_fs.h"

#define TAGS_PATH   "rules/tags.def"
#define RULES_PATH  "rules/training.rules"
#define MASTERS_PATH "rules/masters.def"
#define BUDOKAI_PATH "rules/budokai.rules"

/* The chain the sample content and most inline fixtures share. */
static const char *const kTags =
	"tag training.begun\n"
	"tag trained.flight.hover\n"
	"tag trained.flight.endurance\n"
	"tag trained.aura.sustain\n"
	"tag seen.firstAscension\n";

static void setup(void) {
	fake_fs_reset();
	G_InitMemory();
	G_RulesReset();
	/* The masterNear vocabulary survives G_RulesReset on purpose - it is loaded
	   once per map, outside the rules - so a suite that installs one has to put
	   the built-in table back itself. */
	G_RulesSetMasterVocabulary(NULL, 0);
}

TestSuite(g_rules, .init = setup);

static void load(const char *tags, const char *rules) {
	fake_fs_add(TAGS_PATH, tags);
	fake_fs_add(RULES_PATH, rules);
}

static qboolean loadOk(const char *tags, const char *rules) {
	load(tags, rules);
	return G_RulesLoad(TAGS_PATH, RULES_PATH);
}

/* ------------------------------------------------------------------ parsing */

Test(g_rules, parses_a_rule_into_ranges_and_actions) {
	const rule_t *rule;

	cr_assert(loadOk(kTags,
		"rule hover {\n"
		"    when  airborneTime  atLeast  45s\n"
		"    when  fatigue       above    0pl\n"
		"    requires  training.begun\n"
		"    forbids   trained.flight.hover\n"
		"    grant   trained.flight.hover\n"
		"    say     \"Not bad.\"\n"
		"    unlock  tier 2\n"
		"}\n"), "%s", G_RulesError());

	cr_assert_eq(G_RulesCount(), 1);
	rule = G_RulesFind("hover");
	cr_assert_not_null(rule);
	cr_assert_eq(rule->numCriteria, 2);
	/* Every operator compiles to the same two ints; that is the whole design. */
	cr_assert_eq(rule->criteria[0].key, fAirborneTime);
	cr_assert_eq(rule->criteria[0].min, 45000);
	cr_assert_eq(rule->criteria[0].max, MAX_QINT);
	cr_assert_eq(rule->criteria[1].key, fFatigue);
	cr_assert_eq(rule->criteria[1].min, 1);
	cr_assert_eq(rule->numActions, 3);
	cr_assert_eq(rule->actions[0].type, acGrant);
	cr_assert_eq(rule->actions[0].tag, G_TagFind("trained.flight.hover"));
	cr_assert_eq(rule->actions[1].type, acSay);
	cr_assert_str_eq(rule->actions[1].text, "Not bad.");
	cr_assert_eq(rule->actions[2].type, acUnlockTier);
	cr_assert_eq(rule->actions[2].value, 2);
}

/* One rule per load: two rules of equal specificity would be a load-time tie,
   which is the subject of its own test rather than an obstacle here. */
static const criterion_t *onlyCriterion(const char *body) {
	setup();
	cr_assert(loadOk(kTags, body), "%s", G_RulesError());
	return &G_RulesGet(0)->criteria[0];
}

Test(g_rules, named_operators_all_reach_the_same_two_ints) {
	const criterion_t *c;

	c = onlyCriterion("rule a {\n when tierCurrent is 2\n say \"a\"\n}\n");
	cr_assert_eq(c->min, 2);
	cr_assert_eq(c->max, 2);

	c = onlyCriterion("rule a {\n when tierTotal atMost 3\n say \"a\"\n}\n");
	cr_assert_eq(c->min, MIN_QINT);
	cr_assert_eq(c->max, 3);

	c = onlyCriterion("rule a {\n when gravity below 10g\n say \"a\"\n}\n");
	cr_assert_eq(c->min, MIN_QINT);
	cr_assert_eq(c->max, 8000 - 1);

	c = onlyCriterion("rule a {\n when auraTime between 5s and 9s\n say \"a\"\n}\n");
	cr_assert_eq(c->min, 5000);
	cr_assert_eq(c->max, 9000);
}

Test(g_rules, objective_action_carries_its_tracked_fact_and_goal) {
	const rule_t *rule;

	cr_assert(loadOk(kTags,
		"rule o {\n"
		"    when airborneTime atLeast 1s\n"
		"    objective \"Hover for ten seconds\" track airborneTime goal 10s\n"
		"}\n"), "%s", G_RulesError());

	rule = G_RulesFind("o");
	cr_assert_eq(rule->actions[0].type, acObjective);
	cr_assert_str_eq(rule->actions[0].text, "Hover for ten seconds");
	cr_assert_eq(rule->actions[0].track, fAirborneTime);
	cr_assert_eq(rule->actions[0].value, 10000);
}

Test(g_rules, world_facts_parse_into_their_own_key_space) {
	const rule_t *rule;

	cr_assert(loadOk(kTags,
		"rule w {\n"
		"    when world roundState is waiting\n"
		"    say \"w\"\n"
		"}\n"), "%s", G_RulesError());

	rule = G_RulesFind("w");
	cr_assert_eq(rule->criteria[0].key, wRoundState | RULE_WORLD_KEY);
	cr_assert_eq(rule->criteria[0].min, 0);
}

Test(g_rules, comments_and_trailing_comments_are_skipped) {
	cr_assert(loadOk(kTags,
		"// leading comment\n"
		"rule a {           // about this rule\n"
		"    when tierCurrent is 2   // and this criterion\n"
		"    /* block\n"
		"       comment */\n"
		"    say \"a\"\n"
		"}\n"), "%s", G_RulesError());
	cr_assert_eq(G_RulesCount(), 1);
	cr_assert_eq(G_RulesGet(0)->numCriteria, 1);
}

/* -------------------------------------------------------------- load errors */

Test(g_rules, unknown_fact_names_the_line_and_suggests) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when airbornTime atLeast 45s\n"
		"    say \"a\"\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:2: unknown fact 'airbornTime' - did you mean 'airborneTime'?");
}

Test(g_rules, undeclared_tag_names_tags_def) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when tierCurrent is 2\n"
		"    grant trained.flight.fligth\n"
		"}\n"));
	/* Nothing declared is within three edits, and a suggestion further away
	   than that is noise rather than help. */
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:3: tag 'trained.flight.fligth' not declared in tags.def");
}

Test(g_rules, a_near_miss_tag_gets_a_suggestion) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when tierCurrent is 2\n"
		"    requires trained.flight.hoverr\n"
		"    say \"a\"\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:3: tag 'trained.flight.hoverr' not declared in tags.def"
		" - did you mean 'trained.flight.hover'?");
}

Test(g_rules, a_bare_number_where_a_unit_exists_is_an_error) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when airborneTime atLeast 45\n"
		"    say \"a\"\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:2: bare number '45' for fact 'airborneTime'"
		" - did you mean '45s'?");
}

Test(g_rules, a_wrong_unit_is_an_error) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when airborneTime atLeast 45g\n"
		"    say \"a\"\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:2: unknown unit 'g' for fact 'airborneTime' - expected 's'");
}

Test(g_rules, a_unit_on_a_plain_fact_is_an_error) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when tierCurrent is 2s\n"
		"    say \"a\"\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:2: fact 'tierCurrent' takes a plain number, not '2s'");
}

Test(g_rules, unknown_enum_value_suggests_a_declared_one) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when world roundState is inProgres\n"
		"    say \"a\"\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:2: unknown value 'inProgres' for fact 'roundState'"
		" - did you mean 'inProgress'?");
}

Test(g_rules, unknown_action_suggests_a_declared_verb) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when tierCurrent is 2\n"
		"    gran training.begun\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:3: unknown action 'gran' - did you mean 'grant'?");
}

Test(g_rules, equal_specificity_ties_are_rejected_and_name_both_rules) {
	cr_assert_not(loadOk(kTags,
		"rule first  { when tierCurrent is 2 say \"a\" }\n"
		"rule second { when auraTime atLeast 1s say \"b\" }\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:2: rule 'second' can match the same state as rule 'first'"
		" with equal specificity");
}

Test(g_rules, disjoint_ranges_on_one_fact_are_not_a_tie) {
	cr_assert(loadOk(kTags,
		"rule low  { when tierCurrent is 1 say \"a\" }\n"
		"rule high { when tierCurrent is 2 say \"b\" }\n"), "%s", G_RulesError());
	cr_assert_eq(G_RulesCount(), 2);
}

Test(g_rules, a_forbidden_tag_the_other_rule_requires_breaks_the_tie) {
	cr_assert(loadOk(kTags,
		"rule before {\n"
		"    when tierCurrent is 2\n"
		"    forbids training.begun\n"
		"    grant   training.begun\n"
		"}\n"
		"rule after {\n"
		"    when auraTime atLeast 1s\n"
		"    requires training.begun\n"
		"    say \"b\"\n"
		"}\n"), "%s", G_RulesError());
	cr_assert_eq(G_RulesCount(), 2);
}

Test(g_rules, a_prefix_group_can_be_exhausted_while_bits_remain) {
	char tags[8192];
	int i, used;

	used = 0;
	tags[0] = 0;
	for (i = 0; i < TAG_GROUP_BITS + 1; ++i) {
		used += snprintf(tags + used, sizeof(tags) - used, "tag many.t%i\n", i);
	}
	load(tags, "");
	cr_assert_not(G_RulesLoad(TAGS_PATH, RULES_PATH));
	cr_assert_str_eq(G_RulesError(),
		"rules/tags.def:33: tag prefix group 'many' exhausted (32 of 32 used)");
}

Test(g_rules, the_global_tag_budget_is_exhausted_by_prefixes) {
	char tags[16384];
	int i, used;

	used = 0;
	tags[0] = 0;
	for (i = 0; i < MAX_TAG_GROUPS + 1; ++i) {
		used += snprintf(tags + used, sizeof(tags) - used, "tag group%i.only\n", i);
	}
	load(tags, "");
	cr_assert_not(G_RulesLoad(TAGS_PATH, RULES_PATH));
	cr_assert_str_eq(G_RulesError(),
		"rules/tags.def:17: tag budget exhausted (512 of 512 used)");
}

Test(g_rules, a_test_expecting_an_unknown_rule_is_a_load_error) {
	cr_assert_not(loadOk(kTags,
		"rule a { when tierCurrent is 2 say \"a\" }\n"
		"test \"typo\" {\n"
		"    given tierCurrent 2\n"
		"    expect b\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:2: test 'typo' expects unknown rule 'b'");
}

/* -------------------------------------------------------------------- tags */

Test(g_rules, bits_are_allocated_by_prefix_so_a_wildcard_is_one_mask) {
	tagSet_t mask;
	int hover, endurance;

	cr_assert(loadOk(kTags, ""), "%s", G_RulesError());
	hover = G_TagFind("trained.flight.hover");
	endurance = G_TagFind("trained.flight.endurance");
	cr_assert_neq(hover, -1);
	/* Declared next to each other in the file and adjacent in the bitfield. */
	cr_assert_eq(endurance, hover + 1);
	cr_assert(G_TagPrefixMask("trained.flight", &mask));
	cr_assert(G_TagTest(&mask, hover));
	cr_assert(G_TagTest(&mask, endurance));
	cr_assert_not(G_TagTest(&mask, G_TagFind("trained.aura.sustain")));
}

Test(g_rules, a_wildcard_clause_compiles_to_the_prefix_mask) {
	const rule_t *rule;

	cr_assert(loadOk(kTags,
		"rule a {\n"
		"    when tierCurrent is 2\n"
		"    requires trained.flight.*\n"
		"    say \"a\"\n"
		"}\n"), "%s", G_RulesError());

	rule = G_RulesFind("a");
	cr_assert(G_TagTest(&rule->requireTags, G_TagFind("trained.flight.hover")));
	cr_assert(G_TagTest(&rule->requireTags, G_TagFind("trained.flight.endurance")));
	cr_assert_not(G_TagTest(&rule->requireTags, G_TagFind("training.begun")));
}

Test(g_rules, an_undeclared_prefix_is_a_load_error) {
	cr_assert_not(loadOk(kTags,
		"rule a {\n"
		"    when tierCurrent is 2\n"
		"    requires trained.melee.*\n"
		"    say \"a\"\n"
		"}\n"));
	cr_assert_str_eq(G_RulesError(),
		"rules/training.rules:3: tag prefix 'trained.melee' not declared in tags.def");
}

/* ----------------------------------------------------------------- matching */

static const rule_t *match(const int *facts, const tagSet_t *tags) {
	static int worldFacts[fWorldFactCount];
	static tagSet_t worldTags;
	memset(worldFacts, 0, sizeof(worldFacts));
	memset(&worldTags, 0, sizeof(worldTags));
	return G_RulesMatch(facts, worldFacts, tags, &worldTags);
}

static const char *const kLadder =
	"rule general  { when auraTime atLeast 1s say \"general\" }\n"
	"rule specific { when auraTime atLeast 1s when tierCurrent atLeast 2 say \"specific\" }\n";

Test(g_rules, the_rule_with_the_most_criteria_wins) {
	int facts[fFactCount];
	tagSet_t tags;
	const rule_t *rule;

	cr_assert(loadOk(kTags, kLadder), "%s", G_RulesError());
	memset(facts, 0, sizeof(facts));
	memset(&tags, 0, sizeof(tags));

	facts[fAuraTime] = 2000;
	rule = match(facts, &tags);
	cr_assert_not_null(rule);
	cr_assert_str_eq(rule->name, "general");

	facts[fTierCurrent] = 3;
	rule = match(facts, &tags);
	cr_assert_not_null(rule);
	cr_assert_str_eq(rule->name, "specific");
}

Test(g_rules, nothing_matches_when_no_criterion_holds) {
	int facts[fFactCount];
	tagSet_t tags;

	cr_assert(loadOk(kTags, kLadder), "%s", G_RulesError());
	memset(facts, 0, sizeof(facts));
	memset(&tags, 0, sizeof(tags));
	cr_assert_null(match(facts, &tags));
}

Test(g_rules, require_and_forbid_tags_gate_a_match) {
	int facts[fFactCount];
	tagSet_t tags;

	cr_assert(loadOk(kTags,
		"rule gated {\n"
		"    when auraTime atLeast 1s\n"
		"    requires training.begun\n"
		"    forbids  trained.aura.sustain\n"
		"    say \"gated\"\n"
		"}\n"), "%s", G_RulesError());

	memset(facts, 0, sizeof(facts));
	memset(&tags, 0, sizeof(tags));
	facts[fAuraTime] = 5000;
	cr_assert_null(match(facts, &tags));

	G_TagSet(&tags, G_TagFind("training.begun"));
	cr_assert_not_null(match(facts, &tags));

	G_TagSet(&tags, G_TagFind("trained.aura.sustain"));
	cr_assert_null(match(facts, &tags));
}

Test(g_rules, a_rule_granting_a_tag_it_forbids_is_self_terminating) {
	int facts[fFactCount];
	tagSet_t tags;
	const rule_t *rule;

	cr_assert(loadOk(kTags,
		"rule once {\n"
		"    when auraTime atLeast 1s\n"
		"    forbids seen.firstAscension\n"
		"    grant   seen.firstAscension\n"
		"}\n"), "%s", G_RulesError());

	memset(facts, 0, sizeof(facts));
	memset(&tags, 0, sizeof(tags));
	facts[fAuraTime] = 5000;
	rule = match(facts, &tags);
	cr_assert_not_null(rule);

	/* Applying the rule's own grant is what stops it firing again. */
	G_TagSet(&tags, rule->actions[0].tag);
	cr_assert_null(match(facts, &tags));
}

Test(g_rules, world_facts_and_world_tags_take_part_in_matching) {
	int facts[fFactCount], worldFacts[fWorldFactCount];
	tagSet_t tags, worldTags;

	cr_assert(loadOk(kTags,
		"rule event {\n"
		"    when world roundState is inProgress\n"
		"    requires training.begun\n"
		"    say \"event\"\n"
		"}\n"), "%s", G_RulesError());

	memset(facts, 0, sizeof(facts));
	memset(worldFacts, 0, sizeof(worldFacts));
	memset(&tags, 0, sizeof(tags));
	memset(&worldTags, 0, sizeof(worldTags));

	cr_assert_null(G_RulesMatch(facts, worldFacts, &tags, &worldTags));
	worldFacts[wRoundState] = 1;
	cr_assert_null(G_RulesMatch(facts, worldFacts, &tags, &worldTags));

	/* The tag may be held on either side; the matcher reads the union. */
	G_TagSet(&worldTags, G_TagFind("training.begun"));
	cr_assert_not_null(G_RulesMatch(facts, worldFacts, &tags, &worldTags));
}

/* ------------------------------------------------------------ test vectors */

Test(g_rules, inline_vectors_run_against_the_loaded_rules) {
	int passed, failed;
	char err[MAX_RULE_ERROR];

	cr_assert(loadOk(kTags,
		"rule a { when tierCurrent atLeast 2 say \"a\" }\n"
		"test \"tier two fires it\" {\n"
		"    given  tierCurrent 2\n"
		"    expect a\n"
		"}\n"
		"test \"tier one does not\" {\n"
		"    given  tierCurrent 1\n"
		"    expect none\n"
		"}\n"), "%s", G_RulesError());

	cr_assert_eq(G_RulesTestCount(), 2);
	cr_assert(G_RulesRunTests(&passed, &failed, err, sizeof(err)), "%s", err);
	cr_assert_eq(passed, 2);
	cr_assert_eq(failed, 0);
}

Test(g_rules, a_wrong_vector_reports_what_it_got) {
	int passed, failed;
	char err[MAX_RULE_ERROR];

	cr_assert(loadOk(kTags,
		"rule a { when tierCurrent atLeast 2 say \"a\" }\n"
		"test \"wrong\" {\n"
		"    given  tierCurrent 1\n"
		"    expect a\n"
		"}\n"), "%s", G_RulesError());

	cr_assert_not(G_RulesRunTests(&passed, &failed, err, sizeof(err)));
	cr_assert_eq(failed, 1);
	cr_assert_str_eq(err, "test 'wrong' expected 'a', matched 'none'");
}

/* ------------------------------------------------ accumulators and latching */

/* Both seams the game-side evaluation loop is built on are pure functions of
   their arguments, so the behaviour that decides when a lesson counts and when
   a rule speaks is testable without a server frame. */

Test(g_rules, an_accumulator_counts_unbroken_time) {
	int held = 0;
	int i;

	for (i = 0; i < 10; ++i) {
		held = G_RulesAdvance(held, 50, qtrue);
	}
	cr_assert_eq(held, 500);
	/* One frame off the ground is the whole point: it resets, not decays. */
	held = G_RulesAdvance(held, 50, qfalse);
	cr_assert_eq(held, 0);
	held = G_RulesAdvance(held, 50, qtrue);
	cr_assert_eq(held, 50);
}

Test(g_rules, an_accumulator_saturates_rather_than_wrapping) {
	cr_assert_eq(G_RulesAdvance(MAX_QINT - 10, 50, qtrue), MAX_QINT);
	/* A frame that went backwards must not run the clock down. */
	cr_assert_eq(G_RulesAdvance(1000, -50, qtrue), 1000);
}

Test(g_rules, the_latch_fires_once_when_a_rule_becomes_the_best_match) {
	int latched = 0;

	/* A zeroed client has nothing latched, not rule 0 latched. */
	cr_assert(G_RulesLatch(&latched, 0));
	cr_assert_not(G_RulesLatch(&latched, 0));
	cr_assert_not(G_RulesLatch(&latched, 0));
	/* A different rule taking over is a new edge. */
	cr_assert(G_RulesLatch(&latched, 3));
	cr_assert_not(G_RulesLatch(&latched, 3));
	/* Matching nothing is never a firing, but it does clear the latch, so the
	   same rule fires again when it comes back. */
	cr_assert_not(G_RulesLatch(&latched, -1));
	cr_assert(G_RulesLatch(&latched, 3));
}

Test(g_rules, rules_are_addressable_by_index_for_the_latch) {
	const rule_t *rule;

	cr_assert(loadOk(kTags,
		"rule a { when tierCurrent atLeast 2 say \"a\" }\n"
		"rule b { when tierCurrent atMost 1 say \"b\" }\n"), "%s", G_RulesError());

	rule = G_RulesFind("b");
	cr_assert_eq(G_RulesIndexOf(rule), 1);
	cr_assert_eq(G_RulesGet(G_RulesIndexOf(rule)), rule);
	cr_assert_eq(G_RulesIndexOf(NULL), -1);
}

/* -------------------------------------------------------- shipped content */

/* The content under GameData/rules is a real deliverable, so the suite runs the
   vectors the author wrote in it rather than a copy that could drift. The path
   is baked in at compile time; fake_fs still serves the bytes, so nothing here
   depends on the working directory. */
static void loadShipped(const char *name, const char *path) {
	char *data;
	long length;
	FILE *f = fopen(path, "rb");

	cr_assert_not_null(f, "cannot open %s", path);
	fseek(f, 0, SEEK_END);
	length = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc((size_t)length + 1);
	cr_assert_eq(fread(data, 1, (size_t)length, f), (size_t)length);
	data[length] = '\0';
	fclose(f);
	fake_fs_add(name, data);
	free(data);
}

Test(g_rules, the_shipped_content_loads_and_its_vectors_pass) {
	int passed, failed;
	char err[MAX_RULE_ERROR];

	loadShipped(TAGS_PATH, RULES_CONTENT_DIR "/tags.def");
	loadShipped(RULES_PATH, RULES_CONTENT_DIR "/training.rules");

	cr_assert(G_RulesLoad(TAGS_PATH, RULES_PATH), "%s", G_RulesError());
	cr_assert_gt(G_RulesCount(), 0);
	cr_assert_gt(G_RulesTestCount(), 0);
	cr_assert(G_RulesRunTests(&passed, &failed, err, sizeof(err)), "%s", err);
	cr_assert_eq(failed, 0);
	cr_assert_eq(fake_fs_leak_count(), 0);
}

/* The Budokai ships its own content against the SAME tag vocabulary, which is
   what lets a tournament gate on a tag a training session granted. Its vectors
   run here for the same reason the training arc's do: the announcer is authored
   content, and content that only runs in a live tournament is content nobody
   checks. */
Test(g_rules, the_shipped_budokai_content_loads_and_its_vectors_pass) {
	int passed, failed;
	char err[MAX_RULE_ERROR];

	loadShipped(TAGS_PATH, RULES_CONTENT_DIR "/tags.def");
	loadShipped(BUDOKAI_PATH, RULES_CONTENT_DIR "/budokai.rules");

	cr_assert(G_RulesLoad(TAGS_PATH, BUDOKAI_PATH), "%s", G_RulesError());
	cr_assert_gt(G_RulesCount(), 0);
	cr_assert_gt(G_RulesTestCount(), 0);
	cr_assert(G_RulesRunTests(&passed, &failed, err, sizeof(err)), "%s", err);
	cr_assert_eq(failed, 0);
	cr_assert_eq(fake_fs_leak_count(), 0);
}

/* The gate itself, asserted across the two files: the tournament reads
   budokai.entry, and a training lesson has to be able to grant it. A rename on
   either side is otherwise a silent no-op - the tournament simply never lets
   anybody in and nothing says why. */
Test(g_rules, the_training_arc_grants_what_the_budokai_gate_requires) {
	const rule_t *rule;
	int i, j, entry, granted;

	loadShipped(TAGS_PATH, RULES_CONTENT_DIR "/tags.def");
	loadShipped(RULES_PATH, RULES_CONTENT_DIR "/training.rules");
	cr_assert(G_RulesLoad(TAGS_PATH, RULES_PATH), "%s", G_RulesError());

	entry = G_TagFind("budokai.entry");
	cr_assert_geq(entry, 0, "budokai.entry is no longer declared in tags.def");
	granted = 0;
	for (i = 0; i < G_RulesCount(); ++i) {
		rule = G_RulesGet(i);
		for (j = 0; j < rule->numActions; ++j) {
			if (rule->actions[j].type == acGrant && rule->actions[j].tag == entry) {
				granted++;
			}
		}
	}
	cr_assert_gt(granted, 0, "no training lesson grants budokai.entry");
}

/* The master arc is the reason the vocabulary is loaded before the rules parse:
   `masterNear is roshi` is only a legal value because rules/masters.def declared
   roshi, and this is the assertion that the two files agree. */
Test(g_rules, the_shipped_content_keys_on_the_shipped_masters) {
	const char *const *vocabulary;
	int count, i, j, keyed;
	const rule_t *rule;

	loadShipped(MASTERS_PATH, RULES_CONTENT_DIR "/masters.def");
	G_MastersReset();
	cr_assert(G_MastersLoadDef(MASTERS_PATH), "%s", G_MastersError());
	vocabulary = G_MastersVocabulary(&count);
	G_RulesSetMasterVocabulary(vocabulary, count);

	loadShipped(TAGS_PATH, RULES_CONTENT_DIR "/tags.def");
	loadShipped(RULES_PATH, RULES_CONTENT_DIR "/training.rules");
	cr_assert(G_RulesLoad(TAGS_PATH, RULES_PATH), "%s", G_RulesError());

	keyed = 0;
	for (i = 0; i < G_RulesCount(); ++i) {
		rule = G_RulesGet(i);
		for (j = 0; j < rule->numCriteria; ++j) {
			if (rule->criteria[j].key != fMasterNear) {
				continue;
			}
			keyed++;
			/* A criterion compiles to the master's id, so an id no master holds
			   would be a rule that can never match - the silent no-op again. */
			cr_assert_geq(rule->criteria[j].min, 1);
			cr_assert_lt(rule->criteria[j].min, count);
			cr_assert_str_neq(G_MastersName(rule->criteria[j].min), "");
		}
	}
	cr_assert_gt(keyed, 0, "the shipped content no longer keys on masterNear");
}

/* Progress is quantized on the server and travels as a percent, so the clamp is
   the whole contract the HUD gauge is drawn against. */
Test(g_rules, progress_is_a_clamped_percent) {
	cr_assert_eq(G_RulesProgress(0, 45000), 0);
	cr_assert_eq(G_RulesProgress(-1, 45000), 0);
	cr_assert_eq(G_RulesProgress(22500, 45000), 50);
	cr_assert_eq(G_RulesProgress(45000, 45000), 100);
	/* Overshoot is normal: a fact keeps accumulating after the goal is met. */
	cr_assert_eq(G_RulesProgress(90000, 45000), 100);
	/* A goal of zero is content that never states one; it must not divide. */
	cr_assert_eq(G_RulesProgress(1000, 0), 0);
	/* Truncation, not rounding: 99 must not read as done. */
	cr_assert_eq(G_RulesProgress(44999, 45000), 99);
}
