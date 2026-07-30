/*
Link seams for g_usermissile.c.

The missile code is a leaf of the server-side graph: it reaches spawning,
damage, muzzle geometry, the weapon-data tables and three engine syscalls.
None of that is under test, so all of it is inert.

The two globals are real storage rather than stubs because the unit under test
indexes them (`g_entities[...]`, `level.time`), and a test that wants to place
an entity needs somewhere to put it.
*/

#include "g_local.h"

/* --- the entity array and level state ------------------------------------- */

level_locals_t	level;
gentity_t		g_entities[MAX_GENTITIES];

/* --- spawning and lifetime ------------------------------------------------ */

/* G_FreeEntity is observable: a removal path that frees the missile is the
   normal outcome, and a test asserting the missile survived (or did not) needs
   the flag. Kept here rather than in a fake because it carries no other state. */
static int freed_count;

void G_FreeEntity(gentity_t *e) {
	if (e) { e->inuse = qfalse; }
	freed_count++;
}

int game_stubs_freed_count(void) { return freed_count; }
void game_stubs_reset(void) {
	freed_count = 0;
	memset(g_entities, 0, sizeof(g_entities));
	memset(&level, 0, sizeof(level));
}

gentity_t *G_Spawn(void) { return &g_entities[MAX_CLIENTS]; }
gentity_t *G_TempEntity(vec3_t origin, int event) { (void)origin; (void)event; return &g_entities[MAX_CLIENTS + 1]; }

/* --- inert ---------------------------------------------------------------- */

void G_AddEvent(gentity_t *ent, int event, int eventParm) { (void)ent; (void)event; (void)eventParm; }
void G_SetOrigin(gentity_t *ent, vec3_t origin) { (void)ent; (void)origin; }
void G_RunThink(gentity_t *ent) { (void)ent; }
qboolean CanDamage(gentity_t *targ, vec3_t origin) { (void)targ; (void)origin; return qfalse; }
qboolean OnSameTeam(gentity_t *e1, gentity_t *e2) { (void)e1; (void)e2; return qfalse; }
void CalcMuzzlePoint(gentity_t *ent, vec3_t f, vec3_t r, vec3_t u, vec3_t muzzle) {
	(void)ent; (void)f; (void)r; (void)u; (void)muzzle;
}
void G_GetMuzzleSettings(vec3_t m, vec3_t f, vec3_t r, vec3_t u) { (void)m; (void)f; (void)r; (void)u; }
g_userWeapon_t *G_FindUserWeaponData(int clientNum, int weaponNum) { (void)clientNum; (void)weaponNum; return NULL; }
g_userWeapon_t *G_FindUserAltWeaponData(int clientNum, int weaponNum) { (void)clientNum; (void)weaponNum; return NULL; }

void SnapVectorTowards(vec3_t v, vec3_t to) { (void)v; (void)to; }

void QDECL G_Printf(const char *fmt, ...) { (void)fmt; }

/* --- engine syscalls ------------------------------------------------------ */

void trap_Trace(trace_t *results, const vec3_t start, const vec3_t mins,
                const vec3_t maxs, const vec3_t end, int passEntityNum, int contentmask) {
	(void)start; (void)mins; (void)maxs; (void)end; (void)passEntityNum; (void)contentmask;
	if (results) { memset(results, 0, sizeof(*results)); results->fraction = 1.0f; }
}

void trap_LinkEntity(gentity_t *ent) { (void)ent; }
int trap_EntitiesInBox(const vec3_t mins, const vec3_t maxs, int *list, int maxcount) {
	(void)mins; (void)maxs; (void)list; (void)maxcount;
	return 0;
}
