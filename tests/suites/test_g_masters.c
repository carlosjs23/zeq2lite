/*
Masters: the global name/id vocabulary, per-map placement, and the search behind
the masterNear fact.

Everything here is pure - text in, table out, or a point in and an id out - so
none of it needs a running game. The last group loads the shipped content from
GameData/rules, which is what keeps masters.def and the map placement files
honest without launching anything.
*/

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g_local.h"
#include "fake_fs.h"

#define DEF_PATH    "rules/masters.def"
#define PLACE_PATH  "rules/masters_desert.def"

static const char *const kDef =
	"master 1 roshi\n"
	"master 2 kingKai\n";

static void setup(void) {
	fake_fs_reset();
	G_MastersReset();
}

TestSuite(g_masters, .init = setup);

/* The parsers take a mutable buffer because COM_Parse walks one; the suite hands
   them a copy so the string literals above stay usable. */
static qboolean parseDef(const char *text) {
	char buffer[MAX_MASTERS_FILE];
	Q_strncpyz(buffer, text, sizeof(buffer));
	return G_MastersParseDef(buffer, DEF_PATH);
}

static qboolean parsePlacements(const char *text) {
	char buffer[MAX_MASTERS_FILE];
	Q_strncpyz(buffer, text, sizeof(buffer));
	return G_MastersParsePlacements(buffer, PLACE_PATH);
}

/* --------------------------------------------------------------- vocabulary */

Test(g_masters, parses_ids_and_names) {
	const master_t *master;

	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert_eq(G_MastersCount(), 2);
	master = G_MastersFind("roshi");
	cr_assert_not_null(master);
	cr_assert_eq(master->id, 1);
	cr_assert_eq(master->placed, qfalse);
	cr_assert_str_eq(G_MastersName(2), "kingKai");
	cr_assert_str_eq(G_MastersName(0), "none");
}

/* The id is what a rule compiles to and what travels in PERS_TRAINING_MASTER,
   so the value table has to be indexable by it rather than by file order. */
Test(g_masters, the_vocabulary_is_indexed_by_id) {
	const char *const *vocabulary;
	int count;

	cr_assert(parseDef("master 2 kingKai\nmaster 1 roshi\n"), "%s", G_MastersError());
	vocabulary = G_MastersVocabulary(&count);
	cr_assert_eq(count, 3);
	cr_assert_str_eq(vocabulary[0], "none");
	cr_assert_str_eq(vocabulary[1], "roshi");
	cr_assert_str_eq(vocabulary[2], "kingKai");
}

Test(g_masters, a_reused_id_is_an_error) {
	cr_assert_not(parseDef("master 1 roshi\nmaster 1 kingKai\n"));
	cr_assert_not_null(strstr(G_MastersError(), "already 'roshi'"), "%s", G_MastersError());
}

Test(g_masters, a_reused_name_is_an_error) {
	cr_assert_not(parseDef("master 1 roshi\nmaster 2 roshi\n"));
	cr_assert_not_null(strstr(G_MastersError(), "declared twice"), "%s", G_MastersError());
}

/* Id 0 is the masterNear value 'none', so a master may not claim it. */
Test(g_masters, id_zero_is_rejected) {
	cr_assert_not(parseDef("master 0 roshi\n"));
	cr_assert_not_null(strstr(G_MastersError(), "out of range"), "%s", G_MastersError());
}

Test(g_masters, an_unknown_declaration_names_the_one_it_expected) {
	cr_assert_not(parseDef("teacher 1 roshi\n"));
	cr_assert_not_null(strstr(G_MastersError(), "did you mean 'master'"), "%s", G_MastersError());
	cr_assert_not_null(strstr(G_MastersError(), DEF_PATH), "%s", G_MastersError());
}

/* ---------------------------------------------------------------- placement */

Test(g_masters, places_a_declared_master) {
	const master_t *master;

	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert(parsePlacements("place roshi 100 200 300 512\n"), "%s", G_MastersError());
	master = G_MastersFind("roshi");
	cr_assert_eq(master->placed, qtrue);
	cr_assert_float_eq(master->origin[0], 100.0f, 0.01f);
	cr_assert_float_eq(master->origin[2], 300.0f, 0.01f);
	cr_assert_float_eq(master->radius, 512.0f, 0.01f);
	/* A master the map does not place is still declared, and still a legal
	   value for a rule - it is just never near anyone here. */
	cr_assert_eq(G_MastersFind("kingKai")->placed, qfalse);
}

Test(g_masters, negative_coordinates_survive_the_parse) {
	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert(parsePlacements("place roshi -40810 4681 -1486 768\n"), "%s", G_MastersError());
	cr_assert_float_eq(G_MastersFind("roshi")->origin[0], -40810.0f, 0.01f);
	cr_assert_float_eq(G_MastersFind("roshi")->origin[2], -1486.0f, 0.01f);
}

/* Placing a name masters.def never declared is the silent no-op this whole
   design exists to prevent: no rule could ever mention it. */
Test(g_masters, placing_an_undeclared_master_is_an_error) {
	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert_not(parsePlacements("place yajirobe 0 0 0 512\n"));
	cr_assert_not_null(strstr(G_MastersError(), "not declared"), "%s", G_MastersError());
}

Test(g_masters, a_radius_of_zero_is_an_error) {
	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert_not(parsePlacements("place roshi 0 0 0 0\n"));
	cr_assert_not_null(strstr(G_MastersError(), "nobody can reach"), "%s", G_MastersError());
}

Test(g_masters, a_truncated_placement_line_says_what_was_missing) {
	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert_not(parsePlacements("place roshi 100 200 300\n"));
	cr_assert_not_null(strstr(G_MastersError(), "radius"), "%s", G_MastersError());
}

/* A map with no placement file has no masters on it. That is a fact about the
   map, not a content error, so the load succeeds with nothing placed. */
Test(g_masters, a_missing_placement_file_is_not_an_error) {
	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert(G_MastersLoadPlacements("rules/masters_nosuchmap.def"));
	cr_assert_eq(G_MastersFind("roshi")->placed, qfalse);
	cr_assert_eq(fake_fs_leak_count(), 0);
}

/* ------------------------------------------------------------------ nearest */

static void twoMasters(void) {
	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert(parsePlacements(
		"place roshi   0 0 0     100\n"
		"place kingKai 50 0 0    100\n"), "%s", G_MastersError());
}

Test(g_masters, inside_the_radius_reports_the_master) {
	vec3_t at = {0, 0, 90};
	twoMasters();
	cr_assert_eq(G_MastersNearest(at), 1);
}

Test(g_masters, outside_every_radius_reports_none) {
	vec3_t at = {0, 0, 400};
	twoMasters();
	cr_assert_eq(G_MastersNearest(at), 0);
}

/* Overlapping radii are normal once an author drops two masters in one room,
   and the readout should follow the one the player walked up to. */
Test(g_masters, the_nearest_master_wins_an_overlap) {
	vec3_t nearRoshi = {10, 0, 0};
	vec3_t nearKingKai = {40, 0, 0};
	twoMasters();
	cr_assert_eq(G_MastersNearest(nearRoshi), 1);
	cr_assert_eq(G_MastersNearest(nearKingKai), 2);
}

Test(g_masters, an_unplaced_master_is_never_near_anyone) {
	vec3_t origin = {0, 0, 0};
	cr_assert(parseDef(kDef), "%s", G_MastersError());
	cr_assert_eq(G_MastersNearest(origin), 0);
}

/* The radius is a sphere, so height counts. A master on the ground should not
   greet a player flying over the top of him. */
Test(g_masters, the_radius_is_spherical) {
	vec3_t above = {0, 0, 101};
	vec3_t diagonal = {80, 80, 80};
	twoMasters();
	cr_assert_eq(G_MastersNearest(above), 0);
	cr_assert_eq(G_MastersNearest(diagonal), 0);
}

/* ---------------------------------------------------------------- authoring */

Test(g_masters, masterplace_moves_a_declared_master) {
	vec3_t where = {1, 2, 3};
	twoMasters();
	cr_assert(G_MastersPlace("roshi", where, 640.0f));
	cr_assert_float_eq(G_MastersFind("roshi")->origin[1], 2.0f, 0.01f);
	cr_assert_float_eq(G_MastersFind("roshi")->radius, 640.0f, 0.01f);
}

/* masterplace cannot invent a master, because the name is also the rule
   vocabulary: a master it invented would be a name no rule may mention. */
Test(g_masters, masterplace_refuses_an_undeclared_name) {
	vec3_t where = {1, 2, 3};
	twoMasters();
	cr_assert_not(G_MastersPlace("yajirobe", where, 640.0f));
	cr_assert_not_null(strstr(G_MastersError(), "not declared"), "%s", G_MastersError());
}

/* ----------------------------------------------------------- shipped content */

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

Test(g_masters, the_shipped_desert_placement_loads_and_puts_roshi_somewhere) {
	const master_t *roshi;
	vec3_t at;

	loadShipped(DEF_PATH, RULES_CONTENT_DIR "/masters.def");
	loadShipped(PLACE_PATH, RULES_CONTENT_DIR "/masters_desert.def");

	cr_assert(G_MastersLoadDef(DEF_PATH), "%s", G_MastersError());
	cr_assert(G_MastersLoadPlacements(PLACE_PATH), "%s", G_MastersError());

	roshi = G_MastersFind("roshi");
	cr_assert_not_null(roshi);
	cr_assert_eq(roshi->placed, qtrue);
	/* Standing on his origin has to resolve to him, which is the property the
	   smoke run depends on when it spawns inside the radius. */
	VectorCopy(roshi->origin, at);
	cr_assert_eq(G_MastersNearest(at), roshi->id);
	cr_assert_eq(fake_fs_leak_count(), 0);
}
