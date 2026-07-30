/*
Engine/renderer/tr_vidmodes.c - the r_mode table.

The table is data, and a typo'd entry is invisible until someone selects that
mode and wonders why the world looks squashed. The whole-table sanity checks
below are cheap insurance for exactly that.

Split out of tr_init.c so it can be linked on its own: the lookup needs no
renderer state, only the caller's custom/desktop dimensions, which is also what
makes r_mode -2 ("use the desktop resolution") testable without a display.
*/

#include <criterion/criterion.h>

#include "q_shared.h"
#include "tr_vidmodes.h"

#define EPS 0.01f

/* The mode the engine falls back on, and the resolution the game was authored at. */
Test(vidmodes, classic_mode_three_is_640x480) {
	int width = 0, height = 0;
	float aspect = 0;

	cr_assert( R_ModeInfo( &width, &height, &aspect, 3, 0, 0, 1.0f, 0, 0 ) );
	cr_assert_eq( width, 640 );
	cr_assert_eq( height, 480 );
	cr_assert_float_eq( aspect, 4.0f / 3.0f, EPS );
}

/*
Every entry has to be self-consistent: a positive size, a positive pixel aspect,
and a reported window aspect that matches its own dimensions. This is the check
that catches a fat-fingered digit in a new row.
*/
Test(vidmodes, every_entry_is_self_consistent) {
	int i;

	cr_assert( R_NumVidModes() > 0 );

	for ( i = 0; i < R_NumVidModes(); i++ ) {
		const vidmode_t *vm = R_VidMode( i );
		int width = 0, height = 0;
		float aspect = 0;

		cr_assert_not_null( vm, "mode %d missing", i );
		cr_assert_not_null( vm->description, "mode %d has no description", i );
		cr_assert( vm->width > 0 && vm->height > 0,
		           "mode %d is %dx%d", i, vm->width, vm->height );
		cr_assert( vm->pixelAspect > 0.0f, "mode %d has pixelAspect %g",
		           i, vm->pixelAspect );

		cr_assert( R_ModeInfo( &width, &height, &aspect, i, 0, 0, 1.0f, 0, 0 ),
		           "mode %d rejected by R_ModeInfo", i );
		cr_assert_eq( width, vm->width );
		cr_assert_eq( height, vm->height );
		cr_assert_float_eq( aspect,
		                    (float)vm->width / ( (float)vm->height * vm->pixelAspect ),
		                    EPS, "mode %d aspect mismatch", i );
	}
}

/*
The point of the exercise: the table has to offer the shapes real displays
actually are. Before this it stopped at 2048x1536 plus one 856x480 oddity, so
every 16:9 or 16:10 user had to discover r_customwidth.
*/
Test(vidmodes, table_offers_sixteen_by_nine) {
	int i, found = 0;

	for ( i = 0; i < R_NumVidModes(); i++ ) {
		const vidmode_t *vm = R_VidMode( i );
		if ( fabs( (float)vm->width / (float)vm->height - 16.0f / 9.0f ) < 0.01f ) {
			found++;
		}
	}
	cr_assert( found >= 3, "only %d of %d modes are 16:9", found, R_NumVidModes() );
}

Test(vidmodes, table_offers_sixteen_by_ten) {
	int i, found = 0;

	for ( i = 0; i < R_NumVidModes(); i++ ) {
		const vidmode_t *vm = R_VidMode( i );
		if ( fabs( (float)vm->width / (float)vm->height - 16.0f / 10.0f ) < 0.01f ) {
			found++;
		}
	}
	cr_assert( found >= 2, "only %d of %d modes are 16:10", found, R_NumVidModes() );
}

Test(vidmodes, table_keeps_a_1080p_entry) {
	int i;

	for ( i = 0; i < R_NumVidModes(); i++ ) {
		const vidmode_t *vm = R_VidMode( i );
		if ( vm->width == 1920 && vm->height == 1080 ) {
			return;
		}
	}
	cr_assert_fail( "no 1920x1080 mode in the table" );
}

/* ------------------------------------------------------------ custom mode */

Test(vidmodes, custom_mode_uses_the_caller_dimensions) {
	int width = 0, height = 0;
	float aspect = 0;

	cr_assert( R_ModeInfo( &width, &height, &aspect, VID_MODE_CUSTOM,
	                       1366, 768, 1.0f, 0, 0 ) );
	cr_assert_eq( width, 1366 );
	cr_assert_eq( height, 768 );
	cr_assert_float_eq( aspect, 1366.0f / 768.0f, EPS );
}

/* A non-square pixel aspect stretches the window aspect, not the pixel count. */
Test(vidmodes, custom_mode_honours_pixel_aspect) {
	int width = 0, height = 0;
	float aspect = 0;

	cr_assert( R_ModeInfo( &width, &height, &aspect, VID_MODE_CUSTOM,
	                       640, 480, 2.0f, 0, 0 ) );
	cr_assert_eq( width, 640 );
	cr_assert_eq( height, 480 );
	cr_assert_float_eq( aspect, ( 640.0f / 480.0f ) / 2.0f, EPS );
}

/* ----------------------------------------------------------- desktop mode */

Test(vidmodes, desktop_mode_uses_the_desktop_resolution) {
	int width = 0, height = 0;
	float aspect = 0;

	cr_assert( R_ModeInfo( &width, &height, &aspect, VID_MODE_DESKTOP,
	                       0, 0, 1.0f, 2560, 1440 ) );
	cr_assert_eq( width, 2560 );
	cr_assert_eq( height, 1440 );
	cr_assert_float_eq( aspect, 16.0f / 9.0f, EPS );
}

/*
SDL can fail to report a desktop mode (it does on a headless session), and the
caller passes 0 in that case. Falling back to the authored resolution is what
keeps the engine from creating a 0x0 window and dying inside the GL driver
rather than with a message.
*/
Test(vidmodes, desktop_mode_falls_back_when_unknown) {
	int width = 0, height = 0;
	float aspect = 0;

	cr_assert( R_ModeInfo( &width, &height, &aspect, VID_MODE_DESKTOP,
	                       0, 0, 1.0f, 0, 0 ) );
	cr_assert_eq( width, SCREEN_WIDTH );
	cr_assert_eq( height, SCREEN_HEIGHT );
	cr_assert( aspect > 0.0f );
}

/* --------------------------------------------------------------- rejection */

Test(vidmodes, out_of_range_modes_are_rejected) {
	int width = 0, height = 0;
	float aspect = 0;

	cr_assert_not( R_ModeInfo( &width, &height, &aspect, -3, 0, 0, 1.0f, 0, 0 ) );
	cr_assert_not( R_ModeInfo( &width, &height, &aspect, R_NumVidModes(),
	                           0, 0, 1.0f, 0, 0 ) );
	cr_assert_null( R_VidMode( -1 ) );
	cr_assert_null( R_VidMode( R_NumVidModes() ) );
}
