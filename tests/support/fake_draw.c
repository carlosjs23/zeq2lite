/*
Recording fake for trap_R_DrawStretchPic / trap_R_SetColor.

See fake_draw.h for why this is a fake rather than a stub.
*/

#include "cg_local.h"
#include "fake_draw.h"

static fakeDrawCall_t	calls[FAKE_DRAW_MAX];
static int				numCalls;

void fake_draw_reset( void ) {
	memset( calls, 0, sizeof( calls ) );
	numCalls = 0;
}

int fake_draw_count( void ) {
	return numCalls;
}

const fakeDrawCall_t *fake_draw_call( int index ) {
	if ( index < 0 ) {
		index += numCalls;
	}
	if ( index < 0 || index >= numCalls ) {
		return NULL;
	}
	return &calls[index];
}

const fakeDrawCall_t *fake_draw_last( void ) {
	return fake_draw_call( -1 );
}

void trap_R_DrawStretchPic( float x, float y, float w, float h,
		float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	fakeDrawCall_t *c;

	// Overflow is dropped rather than wrapped: a test that draws more than this
	// is asserting on the wrong thing, and wrapping would silently renumber the
	// calls it does look at.
	if ( numCalls >= FAKE_DRAW_MAX ) {
		return;
	}

	c = &calls[numCalls++];
	c->x = x;
	c->y = y;
	c->w = w;
	c->h = h;
	c->s1 = s1;
	c->t1 = t1;
	c->s2 = s2;
	c->t2 = t2;
	c->shader = (int)hShader;
}

void trap_R_SetColor( const float *rgba ) {
}
