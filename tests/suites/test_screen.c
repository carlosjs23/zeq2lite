/*
Shared/q_math.c - screen-space geometry for widescreen and HiDPI displays.

Every 2D drawing path in the engine, cgame and UI works in a 640x480 virtual
space and maps it onto the real framebuffer. Historically each of the three did
that arithmetic itself and all three simply stretched, so on any display that is
not 4:3 the HUD, text and crosshair came out horizontally fat. These tests pin
down the one shared mapping they now share.

Assertions here are deliberately written as *invariants* - "the aspect-correct
mapping never distorts", "the box stays centred and on screen" - rather than as
a second copy of the formula. A test that recomputes the implementation cannot
fail when the implementation is wrong.
*/

#include <criterion/criterion.h>
#include <math.h>

#include "q_shared.h"

#define EPS 0.01f

/* ------------------------------------------------------- Com_ScreenScale */

Test(screen, four_by_three_is_the_identity) {
	screenScale_t s;

	Com_ScreenScale(&s, SCREEN_WIDTH, SCREEN_HEIGHT);

	cr_assert_float_eq(s.xScale, 1.0f, EPS);
	cr_assert_float_eq(s.yScale, 1.0f, EPS);
	cr_assert_float_eq(s.scale, 1.0f, EPS);
	cr_assert_float_eq(s.xBias, 0.0f, EPS);
	cr_assert_float_eq(s.yBias, 0.0f, EPS);
}

/*
A larger 4:3 mode must still need no bias: the aspect-correct and the stretched
mapping are the same thing whenever the display is 4:3, which is what keeps this
change invisible on the resolutions the game was authored for.
*/
Test(screen, larger_four_by_three_needs_no_bias) {
	screenScale_t s;

	Com_ScreenScale(&s, 1024, 768);

	cr_assert_float_eq(s.xScale, s.scale, EPS);
	cr_assert_float_eq(s.yScale, s.scale, EPS);
	cr_assert_float_eq(s.xBias, 0.0f, EPS);
	cr_assert_float_eq(s.yBias, 0.0f, EPS);
}

Test(screen, widescreen_biases_horizontally_only) {
	screenScale_t s;

	Com_ScreenScale(&s, 1280, 720);

	/* Uniform scale is limited by the short axis, here the height. */
	cr_assert_float_eq(s.scale, 720.0f / SCREEN_HEIGHT, EPS);
	cr_assert(s.xBias > 0.0f, "a 16:9 display must inset the 4:3 box");
	cr_assert_float_eq(s.yBias, 0.0f, EPS);
}

/*
Displays narrower than 4:3 exist (1280x1024 is 5:4) and the historical Quake 3
widescreen patches ignore them, biasing only on x. Handling both axes costs one
comparison and means the mapping is correct for any aspect.
*/
Test(screen, tall_display_biases_vertically_only) {
	screenScale_t s;

	Com_ScreenScale(&s, 1280, 1024);

	cr_assert_float_eq(s.scale, 1280.0f / SCREEN_WIDTH, EPS);
	cr_assert_float_eq(s.xBias, 0.0f, EPS);
	cr_assert(s.yBias > 0.0f, "a 5:4 display must inset the 4:3 box");
}

Test(screen, degenerate_size_stays_finite) {
	screenScale_t s;

	Com_ScreenScale(&s, 0, 0);

	cr_assert(isfinite(s.xScale) && isfinite(s.yScale) && isfinite(s.scale),
	          "a zero-sized mode must not produce inf/nan scales");
	cr_assert(isfinite(s.xBias) && isfinite(s.yBias));
}

/* ------------------------------------------------ Com_ScreenAdjustFrom640 */

Test(screen, stretch_fills_the_whole_screen) {
	screenScale_t s;
	float x = 0, y = 0, w = SCREEN_WIDTH, h = SCREEN_HEIGHT;

	Com_ScreenScale(&s, 1280, 720);
	Com_ScreenAdjustFrom640(&s, qtrue, &x, &y, &w, &h);

	cr_assert_float_eq(x, 0.0f, EPS);
	cr_assert_float_eq(y, 0.0f, EPS);
	cr_assert_float_eq(w, 1280.0f, EPS);
	cr_assert_float_eq(h, 720.0f, EPS);
}

/*
The invariant that defines "aspect-correct": whatever the display, a square in
virtual space comes out square in pixels.
*/
Test(screen, aspect_mode_never_distorts) {
	const int modes[][2] = { {1280, 720}, {1440, 900}, {1920, 1080},
	                         {1280, 1024}, {640, 480}, {2560, 1440} };
	unsigned i;

	for (i = 0; i < ARRAY_LEN(modes); ++i) {
		screenScale_t s;
		float x = 100, y = 100, w = 64, h = 64;

		Com_ScreenScale(&s, modes[i][0], modes[i][1]);
		Com_ScreenAdjustFrom640(&s, qfalse, &x, &y, &w, &h);

		cr_assert_float_eq(w, h, EPS,
		                   "%dx%d turned a square into %gx%g",
		                   modes[i][0], modes[i][1], w, h);
	}
}

/*
A full-screen virtual rect drawn aspect-correct must sit inside the framebuffer
and be centred: equal margins left and right. Asserting the margins against each
other rather than against a computed bias keeps the test independent of how the
bias is derived.
*/
Test(screen, aspect_mode_centres_and_fits) {
	const int modes[][2] = { {1280, 720}, {1920, 1080}, {1280, 1024}, {1440, 900} };
	unsigned i;

	for (i = 0; i < ARRAY_LEN(modes); ++i) {
		screenScale_t s;
		float x = 0, y = 0, w = SCREEN_WIDTH, h = SCREEN_HEIGHT;
		float marginL, marginR, marginT, marginB;

		Com_ScreenScale(&s, modes[i][0], modes[i][1]);
		Com_ScreenAdjustFrom640(&s, qfalse, &x, &y, &w, &h);

		marginL = x;
		marginR = modes[i][0] - (x + w);
		marginT = y;
		marginB = modes[i][1] - (y + h);

		cr_assert(marginL >= -EPS && marginR >= -EPS,
		          "%dx%d: 4:3 box overflows horizontally", modes[i][0], modes[i][1]);
		cr_assert(marginT >= -EPS && marginB >= -EPS,
		          "%dx%d: 4:3 box overflows vertically", modes[i][0], modes[i][1]);
		cr_assert_float_eq(marginL, marginR, EPS, "%dx%d: not centred in x",
		                   modes[i][0], modes[i][1]);
		cr_assert_float_eq(marginT, marginB, EPS, "%dx%d: not centred in y",
		                   modes[i][0], modes[i][1]);
	}
}

/*
Right-aligned HUD elements are placed at x near 640 in virtual space. On a wide
display they must land near the right edge of the *4:3 box*, not off the right
of the screen - i.e. the bias applies to the position, not just the size.
*/
Test(screen, aspect_mode_keeps_right_edge_inside) {
	screenScale_t s;
	float x = SCREEN_WIDTH - 16, y = 0, w = 16, h = 16;

	Com_ScreenScale(&s, 1920, 1080);
	Com_ScreenAdjustFrom640(&s, qfalse, &x, &y, &w, &h);

	cr_assert(x + w <= 1920.0f + EPS, "right-aligned element ran off screen");
	cr_assert(x > 1920.0f / 2.0f, "right-aligned element collapsed leftwards");
}

Test(screen, null_pointers_are_ignored) {
	screenScale_t s;
	float y = 240;

	Com_ScreenScale(&s, 1280, 720);
	/* Callers pass partial rects; cl_scrn.c does exactly this. */
	Com_ScreenAdjustFrom640(&s, qtrue, NULL, &y, NULL, NULL);

	cr_assert_float_eq(y, 360.0f, EPS);
}

/* ------------------------------------------------- Com_ScreenConsoleBounds */

/*
The defect these pin down: the console text column was positioned in virtual
units while it was drawn in framebuffer pixels, so on every display wider than
4:3 the first dozen characters of every line were off the left of the screen.

The invariant is stated in pixels, because that is the space the two mappings
have to agree in - the backdrop is stretched, the text is aspect-correct, and
the text has to land on the backdrop whichever of those is doing the scaling.
*/
#define CON_TEST_MARGIN 56.0f

Test(screen, console_bounds_are_the_identity_at_four_by_three) {
	screenScale_t s;
	float left, width;

	Com_ScreenScale(&s, 1024, 768);
	Com_ScreenConsoleBounds(&s, CON_TEST_MARGIN, &left, &width);

	cr_assert_float_eq(left, CON_TEST_MARGIN, EPS);
	cr_assert_float_eq(width, SCREEN_WIDTH - 2 * CON_TEST_MARGIN, EPS);
}

Test(screen, console_column_lands_on_the_stretched_backdrop) {
	const int modes[][2] = { {640, 480}, {1024, 768}, {1280, 720}, {1280, 800},
	                         {1920, 1080}, {2560, 1440}, {3440, 1440},
	                         {1280, 1024} };
	unsigned i;

	for (i = 0; i < ARRAY_LEN(modes); ++i) {
		screenScale_t s;
		float left, width;
		float x, w, wantLeft, wantRight;

		Com_ScreenScale(&s, modes[i][0], modes[i][1]);
		Com_ScreenConsoleBounds(&s, CON_TEST_MARGIN, &left, &width);

		/* Where the column actually lands once it is drawn. */
		x = left;
		w = width;
		Com_ScreenAdjustFrom640(&s, qfalse, &x, NULL, &w, NULL);

		/* Where the backdrop's margins are, in the same pixels. */
		wantLeft = CON_TEST_MARGIN;
		Com_ScreenAdjustFrom640(&s, qtrue, &wantLeft, NULL, NULL, NULL);
		wantRight = SCREEN_WIDTH - CON_TEST_MARGIN;
		Com_ScreenAdjustFrom640(&s, qtrue, &wantRight, NULL, NULL, NULL);

		cr_assert_float_eq(x, wantLeft, EPS,
		                   "%dx%d: text column starts at %g px, backdrop margin is %g px",
		                   modes[i][0], modes[i][1], x, wantLeft);
		cr_assert_float_eq(x + w, wantRight, EPS,
		                   "%dx%d: text column ends at %g px, backdrop margin is %g px",
		                   modes[i][0], modes[i][1], x + w, wantRight);
	}
}

/*
A widescreen console is wider in characters than a 4:3 one, because the backdrop
it sits on really is wider. Getting this backwards is what left the right third
of the console permanently empty.
*/
Test(screen, console_gets_wider_with_the_display) {
	screenScale_t narrow, wide;
	float a, b;

	Com_ScreenScale(&narrow, 1024, 768);
	Com_ScreenScale(&wide, 1920, 1080);
	Com_ScreenConsoleBounds(&narrow, CON_TEST_MARGIN, NULL, &a);
	Com_ScreenConsoleBounds(&wide, CON_TEST_MARGIN, NULL, &b);

	cr_assert(b > a, "16:9 console (%g) should be wider than 4:3 (%g)", b, a);
}

Test(screen, console_bounds_survive_a_degenerate_mode) {
	screenScale_t s;
	float left, width;

	Com_ScreenScale(&s, 0, 0);
	Com_ScreenConsoleBounds(&s, CON_TEST_MARGIN, &left, &width);

	cr_assert(isfinite(left) && isfinite(width));
	cr_assert(width > 0.0f, "a degenerate mode must still leave room to type");

	/* A NULL scale is the pre-video case, and has to be finite too. */
	Com_ScreenConsoleBounds(NULL, CON_TEST_MARGIN, &left, &width);
	cr_assert_float_eq(left, CON_TEST_MARGIN, EPS);
}

/* ---------------------------------------------------------- Com_ScreenFovX */

/*
Hor+ : the vertical field of view is what cg_fov would have produced at 4:3, and
a wider display shows *more* at the sides. The engine's own CG_CalcFov does the
opposite (it fixes fov_x and shrinks fov_y), so a widescreen player used to see
less of the world vertically than a 4:3 player - a competitive difference, not
just a cosmetic one.
*/
Test(screen, fov_unchanged_at_four_by_three) {
	cr_assert_float_eq(Com_ScreenFovX(90.0f, 640, 480), 90.0f, EPS);
	cr_assert_float_eq(Com_ScreenFovX(110.0f, 1024, 768), 110.0f, EPS);
}

Test(screen, fov_widens_on_widescreen) {
	float fov = Com_ScreenFovX(90.0f, 1920, 1080);

	cr_assert(fov > 90.0f, "16:9 should widen a 90 degree fov, got %g", fov);
	/* 2*atan(tan(45deg) * 16/9 / (4/3)) = 106.26 degrees. */
	cr_assert_float_eq(fov, 106.26f, 0.1f);
}

Test(screen, fov_narrows_on_tall_display) {
	cr_assert(Com_ScreenFovX(90.0f, 1280, 1024) < 90.0f,
	          "a 5:4 display shows less horizontally, not more");
}

Test(screen, fov_is_monotonic_in_aspect) {
	float prev = 0.0f;
	const int widths[] = { 960, 1024, 1280, 1600, 1920, 2560 };
	unsigned i;

	for (i = 0; i < ARRAY_LEN(widths); ++i) {
		float fov = Com_ScreenFovX(90.0f, widths[i], 720);
		cr_assert(fov > prev, "fov must grow with width, %d gave %g after %g",
		          widths[i], fov, prev);
		prev = fov;
	}
}

/*
Whatever the aspect, the result has to stay a usable angle: the projection
matrix divides by tan(fov/2), so 0 or 180 degrees would blow it up.
*/
Test(screen, fov_stays_in_range) {
	const int modes[][2] = { {1, 1000}, {1000, 1}, {0, 0}, {32000, 1080} };
	unsigned i;

	for (i = 0; i < ARRAY_LEN(modes); ++i) {
		float fov = Com_ScreenFovX(90.0f, modes[i][0], modes[i][1]);

		cr_assert(isfinite(fov), "%dx%d produced a non-finite fov",
		          modes[i][0], modes[i][1]);
		cr_assert(fov > 0.0f && fov < 180.0f,
		          "%dx%d produced an unusable fov of %g",
		          modes[i][0], modes[i][1], fov);
	}
}
