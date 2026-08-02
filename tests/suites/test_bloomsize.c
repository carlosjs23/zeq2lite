/*
Engine/renderer/tr_bloomsize.c - the texture and work-region sizes the bloom
pass needs.

Bloom copies the screen into a power-of-two texture, renders a downscaled copy
of the screen into a corner of the framebuffer, blurs that, and stretches the
result back over the frame. Every step needs a size and a pair of texture
coordinates, and a wrong one shows up only as a glow that is subtly the wrong
shape - which is how it survived unnoticed.

Split out of tr_bloom.c so it can be linked on its own: the arithmetic needs no
GL, only the screen size, the sample size and the driver's texture limit.
*/

#include <criterion/criterion.h>

#include "q_shared.h"
#include "tr_bloomsize.h"

#define EPS 0.001f

/*
The headline contract. The work region is a downscaled copy of the screen, so it
has to carry the screen's shape; a square work region on a widescreen display
stretches the blur along one axis.
*/
Test(bloomsize, work_region_keeps_the_screen_shape) {
	const struct { int w, h; } screens[] = {
		{ 2940, 1912 }, { 1280, 960 }, { 640, 480 }, { 1920, 1080 }
	};
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( screens ); i++ ) {
		bloomSizes_t s;

		cr_assert( R_BloomSizes( &s, screens[i].w, screens[i].h, 512, 16384 ),
				"%dx%d should support bloom", screens[i].w, screens[i].h );

		cr_assert_float_eq(
				(float)s.workWidth / (float)s.workHeight,
				(float)screens[i].w / (float)screens[i].h, 0.01f,
				"work region %dx%d does not match screen %dx%d",
				s.workWidth, s.workHeight, screens[i].w, screens[i].h );
	}
}

/* One exact case, to pin the arithmetic rather than just its ratio. */
Test(bloomsize, work_region_is_exact_at_four_by_three) {
	bloomSizes_t s;

	cr_assert( R_BloomSizes( &s, 1280, 960, 512, 16384 ) );
	cr_assert_eq( s.workWidth, 512 );
	cr_assert_eq( s.workHeight, 384 );
}

/*
The work region is rendered into the real framebuffer before being copied off,
so it has to fit inside it - on any screen wide enough to host it at all.
*/
Test(bloomsize, work_region_never_exceeds_the_screen) {
	const struct { int w, h; } screens[] = {
		{ 2940, 1912 }, { 1280, 960 }, { 640, 480 }, { 1920, 1080 }
	};
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( screens ); i++ ) {
		bloomSizes_t s;

		cr_assert( R_BloomSizes( &s, screens[i].w, screens[i].h, 512, 16384 ) );
		cr_assert_leq( s.workWidth, screens[i].w );
		cr_assert_leq( s.workHeight, screens[i].h );
	}
}

/* Both textures round up to a power of two; GL 2.1 without NPOT support needs it. */
Test(bloomsize, textures_are_the_next_power_of_two) {
	bloomSizes_t s;

	cr_assert( R_BloomSizes( &s, 1280, 960, 512, 16384 ) );
	cr_assert_eq( s.screenWidth, 2048 );
	cr_assert_eq( s.screenHeight, 1024 );
	cr_assert_eq( s.effectWidth, 512 );
	cr_assert_eq( s.effectHeight, 512 );
}

/* The read fractions say how much of the padded texture the content occupies. */
Test(bloomsize, read_fractions_are_the_occupied_part) {
	bloomSizes_t s;

	cr_assert( R_BloomSizes( &s, 1280, 960, 512, 16384 ) );
	cr_assert_float_eq( s.screenReadW, 1280.0f / 2048.0f, EPS );
	cr_assert_float_eq( s.screenReadH,  960.0f / 1024.0f, EPS );
	cr_assert_float_eq( s.effectReadW,  512.0f /  512.0f, EPS );
	cr_assert_float_eq( s.effectReadH,  384.0f /  512.0f, EPS );
}

/*
The two effect fractions are genuinely different numbers on any screen that is
not square. Passing one where the other belongs samples the wrong part of the
texture, and that is exactly the mistake worth guarding.
*/
Test(bloomsize, effect_read_fractions_are_independent) {
	bloomSizes_t s;

	cr_assert( R_BloomSizes( &s, 1280, 960, 512, 16384 ) );
	cr_assert_neq( s.effectReadW, s.effectReadH );
}

/* A driver that cannot hold the screen texture cannot have bloom. */
Test(bloomsize, oversized_screen_texture_disables_bloom) {
	bloomSizes_t s;

	cr_assert_not( R_BloomSizes( &s, 4096, 4096, 512, 2048 ) );
}

/* Neither can a screen too small to host the work region. */
Test(bloomsize, work_region_larger_than_the_screen_disables_bloom) {
	bloomSizes_t s;

	cr_assert_not( R_BloomSizes( &s, 320, 240, 512, 16384 ) );
}
