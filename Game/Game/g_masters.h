// Masters: the named characters a training lesson is keyed on, and where they
// stand on a given map.
//
// Two files, deliberately. rules/masters.def is the GLOBAL vocabulary - the
// names and ids `masterNear is roshi` is validated against - and it is the same
// on every map, so a piece of content either loads everywhere or nowhere.
// rules/masters_<mapname>.def is per-map PLACEMENT: an origin and a radius for
// each master that appears on that map. A map that places nobody is not a
// content error, it is a map with no masters on it.
//
// Kept a leaf like g_rules.c - q_shared, the FS traps and G_Printf - so the
// parser and the nearest-master search are unit testable without the rest of
// the game module.
#ifndef G_MASTERS_H
#define G_MASTERS_H

#include "../../Shared/q_shared.h"

// Ids index the masterNear fact's value table, which the rule parser reads
// linearly, so this is a vocabulary size rather than a performance budget.
#define MAX_MASTERS		15
#define MAX_MASTER_NAME		32
#define MAX_MASTERS_FILE	8000
#define MAX_MASTERS_ERROR	256

// The radius an authoring drop uses when the author states none. Roughly the
// distance at which a player flying past would say they had arrived.
#define MASTER_DEFAULT_RADIUS	256.0f

typedef struct {
	char		name[MAX_MASTER_NAME];
	int		id;			// 1..MAX_MASTERS; 0 is the "none" value of masterNear
	vec3_t		origin;
	float		radius;
	qboolean	placed;		// declared everywhere, standing somewhere only on this map
} master_t;

void		G_MastersReset(void);
// Placement may be absent; the vocabulary may not, once any is expected.
qboolean	G_MastersLoadDef(const char *path);
qboolean	G_MastersLoadPlacements(const char *path);
const char	*G_MastersError(void);

// Both parse the text in place, and are what the suite exercises.
qboolean	G_MastersParseDef(char *text,const char *file);
qboolean	G_MastersParsePlacements(char *text,const char *file);

int		G_MastersCount(void);
const master_t	*G_MastersGet(int index);
const master_t	*G_MastersFind(const char *name);
const char	*G_MastersName(int id);

// Id of the master whose radius contains this point, nearest first when they
// overlap. 0 when the point is inside nobody's radius.
int		G_MastersNearest(const vec3_t origin);

// The masterNear value table: index 0 is "none" and index i is the master with
// id i, which is what makes `masterNear is roshi` compile to the id.
const char *const *G_MastersVocabulary(int *count);

// Authoring. Placing a master that is already placed moves it.
qboolean	G_MastersPlace(const char *name,const vec3_t origin,float radius);
qboolean	G_MastersWrite(const char *path,const char *mapname);

#endif // G_MASTERS_H
