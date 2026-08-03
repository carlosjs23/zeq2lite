/*
The tournament ring: the per-map arena file, the inside/outside maths and the
ring-out decision.

All of it is pure - text in, one ring out, or a point and a ground flag in and a
verdict out - so none of it needs a running game. That is the whole reason the
ring-out rule lives in g_ring.c rather than in the frame that calls it: the
decision a round hangs on is the part that can be asserted.

The last group loads the shipped desert arena, which is what keeps the file the
smoke run uses honest without launching anything.
*/

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g_local.h"
#include "fake_fs.h"

#define ARENA_PATH  "rules/arena_desert.def"

static void setup(void) {
	fake_fs_reset();
	G_RingReset();
}

TestSuite(g_ring, .init = setup);

/* The parser takes a mutable buffer because COM_Parse walks one; the suite
   hands it a copy so the string literals stay usable. */
static qboolean parseRing(const char *text) {
	char buffer[MAX_RING_FILE];
	Q_strncpyz(buffer, text, sizeof(buffer));
	return G_RingParse(buffer, ARENA_PATH);
}

/* ------------------------------------------------------------------ parsing */

Test(g_ring, parses_a_center_a_radius_and_a_floor) {
	const ring_t *ring;

	cr_assert(parseRing("ring  100 200 300  1024  276\n"), "%s", G_RingError());
	cr_assert(G_RingDefined());
	ring = G_RingGet();
	cr_assert_float_eq(ring->center[0], 100.0f, 0.01f);
	cr_assert_float_eq(ring->center[1], 200.0f, 0.01f);
	cr_assert_float_eq(ring->center[2], 300.0f, 0.01f);
	cr_assert_float_eq(ring->radius, 1024.0f, 0.01f);
	cr_assert_float_eq(ring->floor, 276.0f, 0.01f);
}

Test(g_ring, comments_and_negative_coordinates_survive) {
	const ring_t *ring;

	cr_assert(parseRing("// the north plateau\nring  -40810 4681 1486  768  1462\n"),
		"%s", G_RingError());
	ring = G_RingGet();
	cr_assert_float_eq(ring->center[0], -40810.0f, 0.01f);
	cr_assert_float_eq(ring->floor, 1462.0f, 0.01f);
}

Test(g_ring, an_empty_file_leaves_no_ring) {
	cr_assert(parseRing("// nobody fights here\n"), "%s", G_RingError());
	cr_assert_not(G_RingDefined());
}

/* A ring with no radius is a mode whose central rule can never fire, which is
   the silent no-op the whole content language is written against. */
Test(g_ring, a_radius_of_zero_is_an_error) {
	cr_assert_not(parseRing("ring  0 0 0  0  0\n"));
	cr_assert(strstr(G_RingError(), "radius"), "%s", G_RingError());
	cr_assert_not(G_RingDefined());
}

Test(g_ring, a_second_ring_is_an_error_rather_than_the_last_one_winning) {
	cr_assert_not(parseRing("ring  0 0 0  512  0\nring  100 0 0  512  0\n"));
	cr_assert(strstr(G_RingError(), "already defined"), "%s", G_RingError());
}

Test(g_ring, an_unknown_declaration_names_itself) {
	cr_assert_not(parseRing("arena  0 0 0  512  0\n"));
	cr_assert(strstr(G_RingError(), "arena"), "%s", G_RingError());
	cr_assert(strstr(G_RingError(), "ring"), "%s", G_RingError());
}

Test(g_ring, a_missing_field_names_the_field) {
	cr_assert_not(parseRing("ring  0 0 0  512\n"));
	cr_assert(strstr(G_RingError(), "floor"), "%s", G_RingError());
}

Test(g_ring, a_word_where_a_number_belongs_is_an_error) {
	cr_assert_not(parseRing("ring  0 0 0  wide  0\n"));
	cr_assert(strstr(G_RingError(), "radius"), "%s", G_RingError());
}

/* -------------------------------------------------------------------- maths */

Test(g_ring, distance_is_signed_from_the_edge_not_the_center) {
	vec3_t at;

	cr_assert(parseRing("ring  0 0 0  1000  0\n"), "%s", G_RingError());
	VectorSet(at, 0, 0, 0);
	cr_assert_float_eq(G_RingDistance(at), -1000.0f, 0.5f);
	VectorSet(at, 600, 800, 0);      /* exactly 1000 out: on the edge */
	cr_assert_float_eq(G_RingDistance(at), 0.0f, 0.5f);
	VectorSet(at, 1200, 0, 0);
	cr_assert_float_eq(G_RingDistance(at), 200.0f, 0.5f);
}

/* The ring is a cylinder with no lid: a fighter a mile up over the middle is
   still in the match, so height must not enter the horizontal test. */
Test(g_ring, height_does_not_change_whether_a_point_is_inside) {
	vec3_t low, high;

	cr_assert(parseRing("ring  0 0 0  1000  0\n"), "%s", G_RingError());
	VectorSet(low, 500, 0, 0);
	VectorSet(high, 500, 0, 30000);
	cr_assert_float_eq(G_RingDistance(low), G_RingDistance(high), 0.01f);
}

Test(g_ring, height_is_measured_from_the_floor_not_the_center) {
	vec3_t at;

	cr_assert(parseRing("ring  0 0 900  1000  276\n"), "%s", G_RingError());
	VectorSet(at, 0, 0, 276);
	cr_assert_float_eq(G_RingHeight(at), 0.0f, 0.01f);
	VectorSet(at, 0, 0, 1276);
	cr_assert_float_eq(G_RingHeight(at), 1000.0f, 0.01f);
	VectorSet(at, 0, 0, 176);
	cr_assert_float_eq(G_RingHeight(at), -100.0f, 0.01f);
}

/* With no ring the maths must read as "at the edge, on the floor" rather than
   as a huge distance, so content that forgets to check stays silent. */
Test(g_ring, with_no_ring_everything_reads_zero) {
	vec3_t at;

	VectorSet(at, 9000, 9000, 9000);
	cr_assert_float_eq(G_RingDistance(at), 0.0f, 0.01f);
	cr_assert_float_eq(G_RingHeight(at), 0.0f, 0.01f);
	cr_assert_not(G_RingIsOut(at, qtrue));
}

/* ------------------------------------------------------------- the decision */

Test(g_ring, touching_down_outside_the_ring_is_a_ring_out) {
	vec3_t out;

	cr_assert(parseRing("ring  0 0 0  1000  0\n"), "%s", G_RingError());
	VectorSet(out, 1200, 0, 0);
	cr_assert(G_RingIsOut(out, qtrue));
}

/* The rule the mode exists for: flying out over the edge is legal, and only
   landing out there loses the round. */
Test(g_ring, flying_over_the_edge_is_legal) {
	vec3_t out;

	cr_assert(parseRing("ring  0 0 0  1000  0\n"), "%s", G_RingError());
	VectorSet(out, 1200, 0, 4000);
	cr_assert_not(G_RingIsOut(out, qfalse));
	/* and the same point, once he lands on something out there */
	cr_assert(G_RingIsOut(out, qtrue));
}

Test(g_ring, standing_inside_the_ring_is_never_a_ring_out) {
	vec3_t in;

	cr_assert(parseRing("ring  0 0 0  1000  0\n"), "%s", G_RingError());
	VectorSet(in, 999, 0, 0);
	cr_assert_not(G_RingIsOut(in, qtrue));
}

/* --------------------------------------------------------------- authoring */

Test(g_ring, placing_a_ring_where_one_exists_moves_it) {
	vec3_t first, second;
	const ring_t *ring;

	VectorSet(first, 0, 0, 100);
	VectorSet(second, 500, 500, 200);
	cr_assert(G_RingPlace(first, 512, 76), "%s", G_RingError());
	cr_assert(G_RingPlace(second, 1024, 176), "%s", G_RingError());
	ring = G_RingGet();
	cr_assert_float_eq(ring->center[0], 500.0f, 0.01f);
	cr_assert_float_eq(ring->radius, 1024.0f, 0.01f);
	cr_assert_float_eq(ring->floor, 176.0f, 0.01f);
}

Test(g_ring, a_placed_ring_with_no_radius_is_refused) {
	vec3_t at;

	VectorSet(at, 0, 0, 0);
	cr_assert_not(G_RingPlace(at, 0, 0));
	cr_assert_not(G_RingDefined());
}

/* The spectator seat: outside the ring, above its floor, and aimed back at the
   middle of it. Nothing free-floating - it is one origin and one set of angles
   handed to the spawn selector. */
Test(g_ring, the_vantage_looks_back_at_the_center_from_outside) {
	vec3_t origin, angles, forward, toCenter;
	const ring_t *ring;

	cr_assert(parseRing("ring  0 0 300  1000  276\n"), "%s", G_RingError());
	G_RingVantage(origin, angles);
	ring = G_RingGet();
	cr_assert_gt(G_RingDistance(origin), 0, "the seat is inside the ring");
	cr_assert_gt(origin[2], ring->floor, "the seat is under the floor");
	AngleVectors(angles, forward, NULL, NULL);
	toCenter[0] = ring->center[0] - origin[0];
	toCenter[1] = ring->center[1] - origin[1];
	toCenter[2] = ring->floor - origin[2];
	VectorNormalize(toCenter);
	cr_assert_float_eq(DotProduct(forward, toCenter), 1.0f, 0.001f);
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

Test(g_ring, a_map_with_no_arena_file_is_not_an_error) {
	cr_assert(G_RingLoad("rules/arena_nowhere.def"), "%s", G_RingError());
	cr_assert_not(G_RingDefined());
}

Test(g_ring, the_shipped_desert_arena_loads_and_its_center_is_inside_it) {
	const ring_t *ring;
	vec3_t at;

	loadShipped(ARENA_PATH, RULES_CONTENT_DIR "/arena_desert.def");
	cr_assert(G_RingLoad(ARENA_PATH), "%s", G_RingError());
	cr_assert(G_RingDefined());
	ring = G_RingGet();
	/* The property the smoke run depends on: standing where the ring was placed
	   is inside it, and a step past the radius is not. */
	VectorCopy(ring->center, at);
	cr_assert_not(G_RingIsOut(at, qtrue));
	at[0] += ring->radius + 64;
	cr_assert(G_RingIsOut(at, qtrue));
	cr_assert_eq(fake_fs_leak_count(), 0);
}
