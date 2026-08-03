// The tournament ring: where the Budokai arena stands on a given map, and the
// inside/outside test the ring-out rule is decided by.
//
// Per-map only, and for the same reason masters are: the maps are not in this
// repository and re-BSP'ing a map to move a trigger volume is a bad authoring
// loop, so the ring is a file written by an in-game editor rather than a brush.
// One ring per map - a map either hosts a tournament or does not.
//
// The name is g_ring rather than g_arena because g_arenas.c already exists and
// is the victory-podium code; the FILE it reads is rules/arena_<mapname>.def,
// which is the name the plan uses.
//
// Kept a leaf like g_masters.c - q_shared, the FS traps and G_Printf - so the
// parser and the ring maths are unit testable without a running game.
#ifndef G_RING_H
#define G_RING_H

#include "../../Shared/q_shared.h"

#define MAX_RING_FILE		2000
#define MAX_RING_ERROR		256

// The radius an authoring drop uses when the author states none: a ring a
// fighter can cross in a couple of seconds, which is what makes ring-out a
// threat rather than a curiosity.
#define RING_DEFAULT_RADIUS	1024.0f

// A placer stands on the ring floor, and ps.origin sits at the middle of the
// bounding box rather than at the feet, so arenaplace drops the floor by the
// player's mins[2].
#define RING_PLACE_FLOOR_DROP	24.0f

// Where a spectator watches from: back from the centre by this much of the
// radius, and up by this much of it above the floor.
#define RING_VANTAGE_BACK	1.6f
#define RING_VANTAGE_HEIGHT	0.6f

typedef struct {
	qboolean	defined;
	vec3_t		center;		// x and y are the ring axis; z is where the author stood
	float		radius;		// horizontal - the ring is a cylinder, not a sphere
	float		floor;		// world height of the ring surface
} ring_t;

void		G_RingReset(void);
// A map with no ring file hosts no tournament, which is a fact about the map
// rather than a fault in the content.
qboolean	G_RingLoad(const char *path);
qboolean	G_RingParse(char *text,const char *file);
const char	*G_RingError(void);

qboolean	G_RingDefined(void);
const ring_t	*G_RingGet(void);

// Signed horizontal distance past the ring edge: negative inside, positive out.
// Zero with no ring defined, so content that forgets to check reads "at the
// edge" rather than "far outside".
float		G_RingDistance(const vec3_t origin);
// Height above the ring floor. Negative below it.
float		G_RingHeight(const vec3_t origin);

// The decision itself. Touching down outside the ring loses the round; flying
// over the edge is legal, which is the DBZ reading of the rule and the reason
// this takes the grounded flag rather than working it out from the height.
qboolean	G_RingIsOut(const vec3_t origin,qboolean grounded);

// Where a spectator sees the whole fight from, aimed at the middle of the ring.
void		G_RingVantage(vec3_t origin,vec3_t angles);

// Authoring. Placing a ring when one exists moves it.
qboolean	G_RingPlace(const vec3_t origin,float radius,float floor);
qboolean	G_RingWrite(const char *path,const char *mapname);

#endif // G_RING_H
