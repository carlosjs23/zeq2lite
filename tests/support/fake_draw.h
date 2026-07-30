/*
Recording fake for the 2D drawing syscall.

A stub would be enough to satisfy the linker, but the thing these tests need to
assert on IS the rect that reaches the renderer - where a HUD element landed and
whether it kept its shape. So this records every call instead of discarding it.
*/

#ifndef FAKE_DRAW_H
#define FAKE_DRAW_H

typedef struct {
	float	x, y, w, h;
	float	s1, t1, s2, t2;
	int		shader;
} fakeDrawCall_t;

#define FAKE_DRAW_MAX 256

void fake_draw_reset( void );
int fake_draw_count( void );
/* Index 0 is the first call of the frame; negative indexes count from the end. */
const fakeDrawCall_t *fake_draw_call( int index );
const fakeDrawCall_t *fake_draw_last( void );

#endif
