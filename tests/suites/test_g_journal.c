/*
The journal's two pure halves: reading the loaded rule set as a list of lessons
with a per-client status, and packing a master's lessons into one server command
argument.

Both are worth a suite because both are silent when wrong. A lesson whose status
derivation is off draws the wrong colour and says nothing; a packer that
overruns its buffer produces a server command that is simply cut in half, and
the page below the cut is missing rather than broken.

The shipped content is loaded at the end, so the assertions about the real
ladder move with the content instead of describing a copy of it.
*/

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g_local.h"
#include "fake_fs.h"

#define TAGS_PATH    "rules/tags.def"
#define RULES_PATH   "rules/training.rules"
#define MASTERS_PATH "rules/masters.def"

static void setup(void) {
	fake_fs_reset();
	G_InitMemory();
	G_RulesReset();
	G_MastersReset();
	G_RulesSetMasterVocabulary(NULL, 0);
}

TestSuite(g_journal, .init = setup);

static void load(const char *tags, const char *rules) {
	fake_fs_add(TAGS_PATH, tags);
	fake_fs_add(RULES_PATH, rules);
	cr_assert(G_RulesLoad(TAGS_PATH, RULES_PATH), "%s", G_RulesError());
}

static void tagsOf(tagSet_t *set, const char *const *names, int count) {
	int i, bit;

	memset(set, 0, sizeof(*set));
	for (i = 0; i < count; ++i) {
		bit = G_TagFind(names[i]);
		cr_assert_geq(bit, 0, "unknown tag %s", names[i]);
		G_TagSet(set, bit);
	}
}

static const journalEntry_t *entryNamed(const journalEntry_t *entries, int count, const char *label) {
	int i;

	for (i = 0; i < count; ++i) {
		if (!strcmp(entries[i].label, label)) {
			return &entries[i];
		}
	}
	return NULL;
}

/* ------------------------------------------------------- what is a lesson */

static const char *const kTags =
	"tag training.begun\n"
	"tag trained.flight.hover\n"
	"tag world.roundOpen\n";

/* A rule that only talks is a nudge or a failure line, and a journal that
   listed those would be a transcript rather than a progression. */
Test(g_journal, dialogue_only_rules_are_not_lessons) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	tagSet_t tags;
	int count;

	load(kTags,
		"rule nudge {\n"
		"  say \"get off the ground\"\n"
		"}\n"
		"rule start {\n"
		"  when airborneTime atLeast 1s\n"
		"  grant training.begun\n"
		"}\n");
	memset(&tags, 0, sizeof(tags));
	count = G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES);
	cr_assert_eq(count, 1);
	cr_assert_str_eq(entries[0].label, "training.begun");
}

/* World tags belong to the round, so a rule that only sets one has granted the
   player nothing to read back. */
Test(g_journal, a_world_tag_grant_is_not_a_lesson) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	tagSet_t tags;

	load(kTags,
		"rule opened {\n"
		"  when airborneTime atLeast 1s\n"
		"  grant world.roundOpen\n"
		"}\n");
	memset(&tags, 0, sizeof(tags));
	cr_assert_eq(G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES), 0);
}

/* A lesson that opens a tier and grants nothing is still something earned, and
   the ceiling is what says whether it was. */
Test(g_journal, a_tier_unlock_is_a_lesson_labelled_by_its_tier) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	tagSet_t tags;

	load(kTags,
		"rule ascend {\n"
		"  when airborneTime atLeast 1s\n"
		"  unlock tier 3\n"
		"}\n");
	memset(&tags, 0, sizeof(tags));
	cr_assert_eq(G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES), 1);
	cr_assert_str_eq(entries[0].label, "tier 3");
	cr_assert_eq(entries[0].status, jsAvailable);

	cr_assert_eq(G_JournalBuild(&tags, 3, entries, MAX_JOURNAL_ENTRIES), 1);
	cr_assert_eq(entries[0].status, jsDone);
}

/* ------------------------------------------------------------ the status */

static const char *const kChainTags =
	"tag training.begun\n"
	"tag trained.flight.hover\n"
	"tag trained.flight.endurance\n";

static const char *const kChain =
	"rule start {\n"
	"  when airborneTime atLeast 1s\n"
	"  forbids training.begun\n"
	"  grant training.begun\n"
	"}\n"
	"rule hover {\n"
	"  when airborneTime atLeast 10s\n"
	"  requires training.begun\n"
	"  forbids trained.flight.hover\n"
	"  grant trained.flight.hover\n"
	"}\n"
	"rule endurance {\n"
	"  when airborneTime atLeast 45s\n"
	"  requires training.begun trained.flight.hover\n"
	"  forbids trained.flight.endurance\n"
	"  grant trained.flight.endurance\n"
	"}\n";

Test(g_journal, an_untrained_client_sees_one_step_open_and_the_rest_locked) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	tagSet_t tags;

	load(kChainTags, kChain);
	memset(&tags, 0, sizeof(tags));
	cr_assert_eq(G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES), 3);
	cr_assert_eq(entries[0].status, jsAvailable);
	cr_assert_eq(entries[1].status, jsLocked);
	cr_assert_eq(entries[2].status, jsLocked);
}

/* The done check has to come first: a lesson forbids the tag it grants, so the
   moment it is earned its own forbid set makes it read as unavailable. Getting
   this backwards turns every finished lesson on the page back into "locked". */
Test(g_journal, an_earned_lesson_reads_done_not_locked) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	const char *const held[] = { "training.begun" };
	tagSet_t tags;

	load(kChainTags, kChain);
	tagsOf(&tags, held, 1);
	cr_assert_eq(G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES), 3);
	cr_assert_eq(entries[0].status, jsDone);
	cr_assert_eq(entries[1].status, jsAvailable);
	cr_assert_eq(entries[2].status, jsLocked);
}

Test(g_journal, the_ladder_opens_one_rung_at_a_time) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	const char *const held[] = { "training.begun", "trained.flight.hover" };
	tagSet_t tags;

	load(kChainTags, kChain);
	tagsOf(&tags, held, 2);
	cr_assert_eq(G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES), 3);
	cr_assert_eq(entries[0].status, jsDone);
	cr_assert_eq(entries[1].status, jsDone);
	cr_assert_eq(entries[2].status, jsAvailable);
}

/* ----------------------------------------------------------- the section */

Test(g_journal, a_lesson_with_no_master_criterion_is_a_solo_drill) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	tagSet_t tags;

	load(kChainTags, kChain);
	memset(&tags, 0, sizeof(tags));
	cr_assert_eq(G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES), 3);
	cr_assert_eq(entries[0].masterId, JOURNAL_SOLO_MASTER);
	cr_assert_eq(entries[2].masterId, JOURNAL_SOLO_MASTER);
}

Test(g_journal, a_masterNear_criterion_files_the_lesson_under_that_master) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	const char *const *vocabulary;
	tagSet_t tags;
	int count;

	fake_fs_add(MASTERS_PATH, "master 1 rhogan\nmaster 2 oberak\n");
	cr_assert(G_MastersLoadDef(MASTERS_PATH), "%s", G_MastersError());
	vocabulary = G_MastersVocabulary(&count);
	G_RulesSetMasterVocabulary(vocabulary, count);

	load("tag trained.rhogan.greeting\ntag trained.oberak.greeting\n",
		"rule rhogan_hello {\n"
		"  when masterNear is rhogan\n"
		"  forbids trained.rhogan.greeting\n"
		"  grant trained.rhogan.greeting\n"
		"}\n"
		"rule kingkai_hello {\n"
		"  when masterNear is oberak\n"
		"  forbids trained.oberak.greeting\n"
		"  grant trained.oberak.greeting\n"
		"}\n");
	memset(&tags, 0, sizeof(tags));
	cr_assert_eq(G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES), 2);
	cr_assert_eq(entries[0].masterId, 1);
	cr_assert_eq(entries[1].masterId, 2);
}

/* ----------------------------------------------------------- the packing */

static journalEntry_t *makeEntries(int count, int masterId, int status, const char *prefix) {
	static journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	int i;

	memset(entries, 0, sizeof(entries));
	for (i = 0; i < count; ++i) {
		snprintf(entries[i].label, sizeof(entries[i].label), "%s%i", prefix, i);
		entries[i].masterId = masterId;
		entries[i].status = status;
		entries[i].rule = i;
	}
	return entries;
}

Test(g_journal, packing_writes_a_status_char_per_lesson_and_separates_them) {
	journalEntry_t *entries = makeEntries(3, 4, jsDone, "tag.");
	char out[JOURNAL_PACK_SIZE];
	int cursor = 0;

	cr_assert_eq(G_JournalPack(entries, 3, 4, &cursor, out, sizeof(out)), 3);
	cr_assert_str_eq(out, "dtag.0|dtag.1|dtag.2");
	cr_assert_eq(cursor, 3);
	/* A second call at the same cursor has nothing left, which is how the
	   sender stops rather than by counting. */
	cr_assert_eq(G_JournalPack(entries, 3, 4, &cursor, out, sizeof(out)), 0);
}

Test(g_journal, packing_skips_every_other_masters_lessons) {
	journalEntry_t entries[4];
	char out[JOURNAL_PACK_SIZE];
	int cursor = 0;

	memset(entries, 0, sizeof(entries));
	strcpy(entries[0].label, "solo");     entries[0].masterId = 0; entries[0].status = jsDone;
	strcpy(entries[1].label, "rhogan.a");  entries[1].masterId = 1; entries[1].status = jsAvailable;
	strcpy(entries[2].label, "solo2");    entries[2].masterId = 0; entries[2].status = jsLocked;
	strcpy(entries[3].label, "rhogan.b");  entries[3].masterId = 1; entries[3].status = jsLocked;

	cr_assert_eq(G_JournalPack(entries, 4, 1, &cursor, out, sizeof(out)), 2);
	cr_assert_str_eq(out, "arhogan.a|lrhogan.b");

	cursor = 0;
	cr_assert_eq(G_JournalPack(entries, 4, 0, &cursor, out, sizeof(out)), 2);
	cr_assert_str_eq(out, "dsolo|lsolo2");
}

/* The command buffer is the reason the cursor exists at all: a master with more
   lessons than one command holds has to continue in the next one, not be cut. */
Test(g_journal, a_full_buffer_stops_on_a_whole_lesson_and_the_cursor_continues) {
	journalEntry_t *entries = makeEntries(6, 1, jsLocked, "aaaaaaaa.");
	char out[24];
	int cursor = 0, first, second, total;

	first = G_JournalPack(entries, 6, 1, &cursor, out, sizeof(out));
	cr_assert_gt(first, 0);
	cr_assert_lt(first, 6);
	cr_assert_lt((int)strlen(out), (int)sizeof(out));
	/* Whole lessons only: nothing may end mid-label. */
	cr_assert_eq(out[strlen(out) - 1], (char)('0' + first - 1));

	second = G_JournalPack(entries, 6, 1, &cursor, out, sizeof(out));
	total = first + second;
	while (second > 0 && total < 6) {
		second = G_JournalPack(entries, 6, 1, &cursor, out, sizeof(out));
		total += second;
	}
	cr_assert_eq(total, 6);
}

/* The separator is the only structure the argument has, so a label carrying one
   would arrive as two lessons with a garbage status char between them. */
Test(g_journal, a_label_containing_the_separator_is_neutered) {
	journalEntry_t entries[1];
	char out[JOURNAL_PACK_SIZE];
	int cursor = 0;

	memset(entries, 0, sizeof(entries));
	strcpy(entries[0].label, "bad|label");
	entries[0].masterId = 0;
	entries[0].status = jsAvailable;
	cr_assert_eq(G_JournalPack(entries, 1, 0, &cursor, out, sizeof(out)), 1);
	cr_assert_str_eq(out, "abad label");
}

Test(g_journal, the_status_char_round_trips) {
	cr_assert_eq(G_JournalStatusFromChar(G_JournalStatusChar(jsDone)), jsDone);
	cr_assert_eq(G_JournalStatusFromChar(G_JournalStatusChar(jsAvailable)), jsAvailable);
	cr_assert_eq(G_JournalStatusFromChar(G_JournalStatusChar(jsLocked)), jsLocked);
	/* Anything else is a truncated or corrupt command, and dim is the safe read. */
	cr_assert_eq(G_JournalStatusFromChar('?'), jsLocked);
}

/* ------------------------------------------------------ the shipped ladder */

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

static void loadShippedContent(void) {
	const char *const *vocabulary;
	int count;

	loadShipped(MASTERS_PATH, RULES_CONTENT_DIR "/masters.def");
	cr_assert(G_MastersLoadDef(MASTERS_PATH), "%s", G_MastersError());
	vocabulary = G_MastersVocabulary(&count);
	G_RulesSetMasterVocabulary(vocabulary, count);
	loadShipped(TAGS_PATH, RULES_CONTENT_DIR "/tags.def");
	loadShipped(RULES_PATH, RULES_CONTENT_DIR "/training.rules");
	cr_assert(G_RulesLoad(TAGS_PATH, RULES_PATH), "%s", G_RulesError());
}

/* The page a player sees on their first open: something to do alone, a master
   with a door in, and nothing already ticked. */
Test(g_journal, the_shipped_content_opens_with_work_available_and_nothing_done) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	tagSet_t tags;
	int count, i, solo, keyed, available, done;

	loadShippedContent();
	memset(&tags, 0, sizeof(tags));
	count = G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES);
	cr_assert_gt(count, 0, "the shipped content has no lessons in it");

	solo = keyed = available = done = 0;
	for (i = 0; i < count; ++i) {
		if (entries[i].masterId == JOURNAL_SOLO_MASTER) { solo++; } else { keyed++; }
		if (entries[i].status == jsAvailable) { available++; }
		if (entries[i].status == jsDone) { done++; }
	}
	cr_assert_gt(solo, 0, "no solo drill arc left in the shipped content");
	cr_assert_gt(keyed, 0, "no master keeps a lesson in the shipped content");
	cr_assert_gt(available, 0, "an untrained player has nothing open to them");
	cr_assert_eq(done, 0, "an untrained player already has a lesson ticked");
}

/* The smoke path, and the one the screenshot is taken on: earning Rhogan's
   greeting has to tick exactly that lesson and open the one behind it. */
Test(g_journal, the_rhogan_greeting_ticks_its_own_lesson_and_opens_the_next) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	const char *const held[] = { "trained.rhogan.greeting" };
	const journalEntry_t *greeting, *flight;
	tagSet_t tags;
	int count;

	loadShippedContent();
	tagsOf(&tags, held, 1);
	count = G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES);
	greeting = entryNamed(entries, count, "trained.rhogan.greeting");
	flight = entryNamed(entries, count, "trained.rhogan.flight");
	cr_assert_not_null(greeting);
	cr_assert_not_null(flight);
	cr_assert_eq(greeting->status, jsDone);
	cr_assert_eq(flight->status, jsAvailable);
	cr_assert_eq(greeting->masterId, flight->masterId);
	cr_assert_neq(greeting->masterId, JOURNAL_SOLO_MASTER);
}

/* Every section of the shipped ladder has to fit in a single command, or the
   page a player opens costs more of the reliable command budget than the batch
   was sized for. */
Test(g_journal, every_shipped_section_fits_one_command) {
	journalEntry_t entries[MAX_JOURNAL_ENTRIES];
	char out[JOURNAL_PACK_SIZE];
	tagSet_t tags;
	int count, section, cursor, written;

	loadShippedContent();
	memset(&tags, 0, sizeof(tags));
	count = G_JournalBuild(&tags, 0, entries, MAX_JOURNAL_ENTRIES);
	for (section = 0; section <= G_MastersCount(); ++section) {
		cursor = 0;
		written = G_JournalPack(entries, count, section, &cursor, out, sizeof(out));
		if (!written) {
			continue;
		}
		cr_assert_eq(G_JournalPack(entries, count, section, &cursor, out, sizeof(out)), 0,
			"section %i needs more than one command", section);
	}
}
