/*
Game/CGame/cg_drawtools.c - the cgame end of widescreen support.

The shared mapping is covered by test_screen.c. What this suite pins down is the
*wiring*: that cgame actually asks for the aspect-correct mapping when the caller
requested it. That mattered, because CG_AdjustFrom640 took a `stretch` argument
and then opened with

    stretch = qtrue;

so every caller got the stretched mapping no matter what it asked for, and the
half-finished widescreen work from 2012 was dead code. A unit test on the shared
maths would not have caught that; only one that drives the real draw path does.
*/

#include <criterion/criterion.h>

#include "cg_local.h"
#include "fake_draw.h"

#define EPS 0.01f

/* Puts cgame in a given video mode, the way CG_Init does from glconfig. */
static void set_mode( int width, int height ) {
	memset( &cgs.glconfig, 0, sizeof( cgs.glconfig ) );
	cgs.glconfig.vidWidth = width;
	cgs.glconfig.vidHeight = height;
	CG_SetupScreenScale();
	fake_draw_reset();
}

/* ------------------------------------------------------------ 4:3 is a no-op */

Test(cg_screen, native_mode_is_pixel_exact) {
	const fakeDrawCall_t *c;

	set_mode( SCREEN_WIDTH, SCREEN_HEIGHT );
	CG_DrawPic( qfalse, 100, 90, 64, 32, 0 );

	c = fake_draw_last();
	cr_assert_not_null( c );
	cr_assert_float_eq( c->x, 100.0f, EPS );
	cr_assert_float_eq( c->y, 90.0f, EPS );
	cr_assert_float_eq( c->w, 64.0f, EPS );
	cr_assert_float_eq( c->h, 32.0f, EPS );
}

/* ------------------------------------------------------- aspect-correct path */

Test(cg_screen, hud_element_keeps_its_shape_on_widescreen) {
	const fakeDrawCall_t *c;

	set_mode( 1280, 720 );
	CG_DrawPic( qfalse, 288, 208, 64, 64, 0 );

	c = fake_draw_last();
	cr_assert_not_null( c );
	cr_assert_float_eq( c->w, c->h, EPS,
	                    "a square icon came out %gx%g", c->w, c->h );
}

Test(cg_screen, hud_is_centred_on_widescreen) {
	const fakeDrawCall_t *c;
	float marginL, marginR;

	set_mode( 1920, 1080 );
	CG_DrawPic( qfalse, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0 );

	c = fake_draw_last();
	cr_assert_not_null( c );
	marginL = c->x;
	marginR = 1920.0f - ( c->x + c->w );
	cr_assert( marginL > 0.0f, "no left margin, so nothing was inset" );
	cr_assert_float_eq( marginL, marginR, EPS, "4:3 box is not centred" );
}

/*
Right-aligned readouts (the powerlevel counter sits at the right edge) are the
case that breaks if the bias is applied to the size but not the position.
*/
Test(cg_screen, right_aligned_element_stays_on_screen) {
	const fakeDrawCall_t *c;

	set_mode( 1920, 1080 );
	CG_DrawPic( qfalse, SCREEN_WIDTH - 64, 8, 64, 16, 0 );

	c = fake_draw_last();
	cr_assert_not_null( c );
	cr_assert( c->x + c->w <= 1920.0f + EPS, "ran off the right of the screen" );
	cr_assert( c->x > 960.0f, "collapsed into the left half of the screen" );
}

/* --------------------------------------------------------- stretched path */

/*
Backgrounds and full-screen fades must still cover every pixel. A damage flash
that letterboxes itself would be a very visible regression, so this asserts the
stretched path was left alone.
*/
Test(cg_screen, background_still_fills_the_screen) {
	const fakeDrawCall_t *c;

	set_mode( 1280, 720 );
	CG_DrawPic( qtrue, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0 );

	c = fake_draw_last();
	cr_assert_not_null( c );
	cr_assert_float_eq( c->x, 0.0f, EPS );
	cr_assert_float_eq( c->y, 0.0f, EPS );
	cr_assert_float_eq( c->w, 1280.0f, EPS );
	cr_assert_float_eq( c->h, 720.0f, EPS );
}

Test(cg_screen, fill_rect_covers_the_whole_screen) {
	const fakeDrawCall_t *c;
	vec4_t color = { 1, 0, 0, 0.5f };

	set_mode( 1280, 720 );
	CG_FillRect( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, color );

	c = fake_draw_last();
	cr_assert_not_null( c );
	cr_assert_float_eq( c->w, 1280.0f, EPS );
	cr_assert_float_eq( c->h, 720.0f, EPS );
}

/* ------------------------------------------------------------------- text */

/*
Stretched text is the most obvious widescreen artifact - every glyph 33% too
wide at 16:9 - and the charset is a fixed-cell texture, so the fix is to scale
it uniformly like any other HUD element.
*/
Test(cg_screen, characters_keep_their_shape_on_widescreen) {
	const fakeDrawCall_t *c;

	set_mode( 1280, 720 );
	CG_DrawChar( 32, 32, 16, 16, 'A' );

	c = fake_draw_last();
	cr_assert_not_null( c );
	cr_assert_float_eq( c->w, c->h, EPS,
	                    "a square glyph cell came out %gx%g", c->w, c->h );
}

/*
Text advances one cell at a time, so if the glyph is scaled uniformly but the
advance is not, a string either overlaps itself or gains gaps. Draw two
characters and require the step between them to match the cell width.
*/
Test(cg_screen, string_advance_matches_glyph_width) {
	const fakeDrawCall_t *first, *second;

	set_mode( 1280, 720 );
	/* spacing -1 is the "advance by one character cell" default. */
	CG_DrawStringExt( -1, 0, 0, "AB", NULL, qtrue, qfalse,
	                  SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );

	cr_assert( fake_draw_count() >= 2, "expected a draw per character" );
	first = fake_draw_call( 0 );
	second = fake_draw_call( 1 );
	cr_assert_float_eq( second->x - first->x, first->w, EPS,
	                    "glyph advance %g does not match glyph width %g",
	                    second->x - first->x, first->w );
}
