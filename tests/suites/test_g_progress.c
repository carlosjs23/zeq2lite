/*
Training progress: the save-file key, the serializer and the parser.

All three are pure - a guid and a name in, a filename-safe key out; a tag set in,
text out; text in, a tag set out - so none of it needs a running game. The tag
set is the part that has to survive content change, and the round-trip tests
below are what say it does: the file names tags, so a tags.def that renumbers
its bits still loads, and one that retires a name drops it and says so.
*/

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g_local.h"
#include "fake_fs.h"

#define TAGS_PATH   "rules/tags.def"
#define RULES_PATH  "rules/training.rules"

static const char *const kTags =
	"tag training.begun\n"
	"tag trained.roshi.greeting\n"
	"tag trained.roshi.flight\n"
	"tag trained.kingKai.gravity\n";

/* Nothing here needs a rule; the tag vocabulary is loaded through the same
   entry point the game uses, so the bits under test are the real ones. */
static const char *const kRules =
	"rule noop {\n"
	"    when  health  atLeast  1pl\n"
	"    say   \"hello\"\n"
	"}\n";

static void loadTags(const char *tags) {
	fake_fs_reset();
	G_InitMemory();
	G_RulesReset();
	fake_fs_add(TAGS_PATH, tags);
	fake_fs_add(RULES_PATH, kRules);
	cr_assert(G_RulesLoad(TAGS_PATH, RULES_PATH), "%s", G_RulesError());
}

static void setup(void) {
	loadTags(kTags);
}

TestSuite(g_progress, .init = setup);

static void grant(progress_t *p, const char *name) {
	int bit = G_TagFind(name);
	cr_assert(bit >= 0, "test fixture names an undeclared tag: %s", name);
	G_TagSet(&p->tags, bit);
}

/* ---------------------------------------------------------------- the key */

Test(g_progress, guid_becomes_the_key) {
	char key[MAX_PROGRESS_KEY];

	cr_assert(G_ProgressKey("8F14E45FCEEA167A5A36DEDD4BEA2543", "Goku", key, sizeof(key)));
	cr_assert_str_eq(key, "8f14e45fceea167a5a36dedd4bea2543");
	cr_assert_str_eq(G_ProgressPath(key), "training/8f14e45fceea167a5a36dedd4bea2543.progress");
}

/* The key becomes a filename, so anything that could steer one out of the
   training directory has to be gone rather than escaped. */
Test(g_progress, key_cannot_name_a_path) {
	char key[MAX_PROGRESS_KEY];

	cr_assert(G_ProgressKey("../../zeq2config", NULL, key, sizeof(key)));
	cr_assert_str_eq(key, "zeq2config");
	cr_assert(G_ProgressKey("a/b\\c.d*e", NULL, key, sizeof(key)));
	cr_assert_str_eq(key, "abcde");
}

/* A listen server's own client has no guid, and that is the single-player
   case, so it falls back to the cleaned name rather than not persisting. */
Test(g_progress, no_guid_falls_back_to_the_name) {
	char key[MAX_PROGRESS_KEY];

	cr_assert(G_ProgressKey("", "^1Go^7ku", key, sizeof(key)));
	cr_assert_str_eq(key, "name_goku");
}

Test(g_progress, nothing_to_key_on_does_not_persist) {
	char key[MAX_PROGRESS_KEY];

	cr_assert_eq(G_ProgressKey("", "^1^2^3", key, sizeof(key)), qfalse);
	cr_assert_str_eq(key, "");
	cr_assert_eq(G_ProgressKey(NULL, NULL, key, sizeof(key)), qfalse);
}

Test(g_progress, a_long_guid_is_truncated_not_overrun) {
	char key[MAX_PROGRESS_KEY];
	char guid[256];

	memset(guid, 'a', sizeof(guid) - 1);
	guid[sizeof(guid) - 1] = 0;
	cr_assert(G_ProgressKey(guid, NULL, key, sizeof(key)));
	cr_assert_eq((int)strlen(key), MAX_PROGRESS_KEY - 1);
}

/* -------------------------------------------------------------- the format */

Test(g_progress, serializes_named_tags_and_the_ceiling) {
	progress_t p;
	char text[MAX_PROGRESS_FILE];
	int length;

	memset(&p, 0, sizeof(p));
	p.unlockedTier = 3;
	grant(&p, "trained.roshi.greeting");
	grant(&p, "trained.roshi.flight");

	length = G_ProgressSerialize(&p, "abc123", text, sizeof(text));
	cr_assert(length > 0);
	cr_assert_eq(length, (int)strlen(text));
	cr_assert_not_null(strstr(text, "version 1\n"));
	cr_assert_not_null(strstr(text, "key abc123\n"));
	cr_assert_not_null(strstr(text, "tier 3\n"));
	cr_assert_not_null(strstr(text, "tag trained.roshi.greeting\n"));
	cr_assert_not_null(strstr(text, "tag trained.roshi.flight\n"));
	/* Ungranted tags are absent rather than written as a zero. */
	cr_assert_null(strstr(text, "trained.kingKai.gravity"));
	/* LF only: this is not one of the CRLF configs it sits beside. */
	cr_assert_null(strstr(text, "\r"));
	cr_assert_eq(G_ProgressTagCount(&p), 2);
}

Test(g_progress, round_trips_through_text) {
	progress_t written, read;
	progressLoad_t report;
	char text[MAX_PROGRESS_FILE];

	memset(&written, 0, sizeof(written));
	written.unlockedTier = 2;
	grant(&written, "training.begun");
	grant(&written, "trained.kingKai.gravity");
	cr_assert(G_ProgressSerialize(&written, "k", text, sizeof(text)) > 0);

	cr_assert(G_ProgressParse(text, "test.progress", &read, &report));
	cr_assert_eq(read.unlockedTier, 2);
	cr_assert_eq(report.restored, 2);
	cr_assert_eq(report.dropped, 0);
	cr_assert(G_TagTest(&read.tags, G_TagFind("training.begun")));
	cr_assert(G_TagTest(&read.tags, G_TagFind("trained.kingKai.gravity")));
	cr_assert_eq(G_TagTest(&read.tags, G_TagFind("trained.roshi.flight")), qfalse);
}

/* The reason names are stored rather than bits: a tag inserted in the middle of
   a prefix group moves every bit after it, and a file that meant bit 2 would
   restore whatever now lives there. */
Test(g_progress, names_survive_a_renumbered_tag_vocabulary) {
	progress_t written, read;
	progressLoad_t report;
	char text[MAX_PROGRESS_FILE];
	int before, after;

	memset(&written, 0, sizeof(written));
	grant(&written, "trained.roshi.flight");
	before = G_TagFind("trained.roshi.flight");
	cr_assert(G_ProgressSerialize(&written, "k", text, sizeof(text)) > 0);

	loadTags(
		"tag training.begun\n"
		"tag trained.roshi.greeting\n"
		"tag trained.roshi.powerup\n"		/* inserted ahead of it */
		"tag trained.roshi.flight\n");
	after = G_TagFind("trained.roshi.flight");
	cr_assert_neq(before, after, "fixture did not actually move the bit");

	cr_assert(G_ProgressParse(text, "test.progress", &read, &report));
	cr_assert_eq(report.restored, 1);
	cr_assert(G_TagTest(&read.tags, after));
	cr_assert_eq(G_TagTest(&read.tags, before), qfalse);
}

/* Content evolves. A retired lesson is not a corrupt save file. */
Test(g_progress, unknown_tag_names_are_dropped_and_reported) {
	progress_t read;
	progressLoad_t report;
	char text[] =
		"version 1\n"
		"key k\n"
		"tier 2\n"
		"tag trained.roshi.flight\n"
		"tag trained.roshi.retired\n"
		"tag trained.gone.entirely\n";

	cr_assert(G_ProgressParse(text, "test.progress", &read, &report));
	cr_assert_eq(read.unlockedTier, 2);
	cr_assert_eq(report.restored, 1);
	cr_assert_eq(report.dropped, 2);
	cr_assert_eq(report.named, 2);
	cr_assert_str_eq(report.droppedNames[0], "trained.roshi.retired");
	cr_assert_str_eq(report.droppedNames[1], "trained.gone.entirely");
	cr_assert(G_TagTest(&read.tags, G_TagFind("trained.roshi.flight")));
}

Test(g_progress, the_dropped_name_list_is_bounded_but_the_count_is_not) {
	progress_t read;
	progressLoad_t report;
	char text[MAX_PROGRESS_FILE];
	int i, at;

	Q_strncpyz(text, "version 1\n", sizeof(text));
	at = (int)strlen(text);
	for (i = 0; i < MAX_PROGRESS_DROPPED + 5; ++i) {
		Com_sprintf(text + at, sizeof(text) - at, "tag retired.lesson%i\n", i);
		at += (int)strlen(text + at);
	}
	cr_assert(G_ProgressParse(text, "test.progress", &read, &report));
	cr_assert_eq(report.dropped, MAX_PROGRESS_DROPPED + 5);
	cr_assert_eq(report.named, MAX_PROGRESS_DROPPED);
}

/* A future format is refused whole rather than half-read. */
Test(g_progress, an_unknown_version_loads_nothing) {
	progress_t read;
	progressLoad_t report;
	char text[] =
		"version 99\n"
		"tier 4\n"
		"tag trained.roshi.flight\n";

	cr_assert_eq(G_ProgressParse(text, "test.progress", &read, &report), qfalse);
	cr_assert_eq(read.unlockedTier, 0);
	cr_assert(G_TagsEmpty(&read.tags));
}

Test(g_progress, a_file_without_a_version_loads_nothing) {
	progress_t read;
	progressLoad_t report;
	char text[] = "tier 4\ntag trained.roshi.flight\n";

	cr_assert_eq(G_ProgressParse(text, "test.progress", &read, &report), qfalse);
	cr_assert_eq(read.unlockedTier, 0);
}

/* Keys this build does not know carry one value, so a file from a later build
   with the same version still reads. */
Test(g_progress, unknown_keys_are_skipped_with_their_value) {
	progress_t read;
	progressLoad_t report;
	char text[] =
		"version 1\n"
		"key k\n"
		"lastSeen 12345\n"
		"tier 2\n"
		"tag trained.roshi.flight\n";

	cr_assert(G_ProgressParse(text, "test.progress", &read, &report));
	cr_assert_eq(read.unlockedTier, 2);
	cr_assert_eq(report.restored, 1);
}

/* ------------------------------------------------------------ through the FS */

Test(g_progress, reads_a_file_through_the_traps) {
	progress_t read;
	progressLoad_t report;

	fake_fs_add("training/abc.progress",
		"// ZEQ2 training progress. Safe to delete.\n"
		"version 1\nkey abc\ntier 3\ntag training.begun\n");
	cr_assert(G_ProgressRead("abc", &read, &report));
	cr_assert_eq(read.unlockedTier, 3);
	cr_assert_eq(report.restored, 1);
	cr_assert_eq(fake_fs_leak_count(), 0);
}

/* No file is what a player who has never trained here looks like. */
Test(g_progress, a_missing_file_is_not_an_error) {
	progress_t read;
	progressLoad_t report;

	cr_assert_eq(G_ProgressRead("nobody", &read, &report), qfalse);
	cr_assert_eq(read.unlockedTier, 0);
	cr_assert(G_TagsEmpty(&read.tags));
	cr_assert_eq(fake_fs_leak_count(), 0);
}
