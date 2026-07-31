/*
Link seams for cg_players.c.

cg_players.c is a leaf of a very wide graph: it reaches effects, aura, weapon
and local-entity code across the whole cgame, plus a long tail of renderer and
sound syscalls. None of that is under test here, so all of it is inert.

The one exception lives in fake_lerptag.c. Everything in this file exists only
to satisfy the linker; if a test ever needs to observe one of these, promote it
to a fake there rather than growing an assertion against a stub.
*/

#include "cg_local.h"

/* --- cvars ---------------------------------------------------------------- */

vmCvar_t cg_shadows;
vmCvar_t cg_animSpeed;
vmCvar_t cg_debugAnim;
vmCvar_t cg_debugPosition;
vmCvar_t cg_noPlayerAnims;
vmCvar_t cg_advancedFlight;
vmCvar_t cg_buildScript;
vmCvar_t cg_drawFriend;
vmCvar_t cg_drawBBox;
vmCvar_t cg_thirdPersonCamera;

/* --- the entity array ----------------------------------------------------- */

centity_t cg_entities[MAX_GENTITIES];

/* --- cgame functions from other translation units ------------------------- */

void CG_Trace(trace_t *result, const vec3_t start, const vec3_t mins, const vec3_t maxs,
              const vec3_t end, int skipNumber, int mask) {
	(void)start; (void)mins; (void)maxs; (void)skipNumber; (void)mask;
	if (result) {
		memset(result, 0, sizeof(*result));
		result->fraction = 1.0f;
		if (end) {
			VectorCopy(end, result->endpos);
		}
	}
}

int CG_PointContents(const vec3_t point, int passEntityNum) {
	(void)point; (void)passEntityNum;
	return 0;
}

void CG_PositionRotatedEntityOnTag(refEntity_t *entity, const refEntity_t *parent,
                                   qhandle_t parentModel, char *tagName) {
	(void)parent; (void)parentModel; (void)tagName; (void)entity;
}

const char *CG_ConfigString(int index) {
	(void)index;
	return "";
}

void CG_AddEarthquake(const vec3_t origin, float radius, float duration,
                      float fadeIn, float fadeOut, float amplitude) {
	(void)origin; (void)radius; (void)duration; (void)fadeIn; (void)fadeOut; (void)amplitude;
}

void CG_Camera(centity_t *cent) { (void)cent; }

void CG_AddPlayerWeapon(refEntity_t *parent, playerState_t *ps, centity_t *cent, int team) {
	(void)parent; (void)ps; (void)cent; (void)team;
}

// Called from CG_CopyClientInfoModel, which no case here reaches. ld64 lets the
// reference go unresolved on that basis; GNU ld does not, so the suite only
// linked on macOS until this existed.
void CG_CopyUserWeaponGraphics(int from, int to) { (void)from; (void)to; }

qboolean CG_RegisterClientModelnameWithTiers(clientInfo_t *ci, const char *modelName,
                                             const char *skinName) {
	(void)ci; (void)modelName; (void)skinName;
	return qtrue;
}

void CG_RegisterClientAura(int clientNum, clientInfo_t *ci) { (void)clientNum; (void)ci; }
void CG_AddAuraToScene(centity_t *player) { (void)player; }
void CG_AuraStart(centity_t *player) { (void)player; }
void CG_AuraEnd(centity_t *player) { (void)player; }

localEntity_t *CG_AuraSpike(const vec3_t p, const vec3_t vel, float radius, float duration,
                            int startTime, int fadeInTime, int leFlags, centity_t *player) {
	(void)p; (void)vel; (void)radius; (void)duration;
	(void)startTime; (void)fadeInTime; (void)leFlags; (void)player;
	return NULL;
}

localEntity_t *CG_WaterBubble(const vec3_t p, const vec3_t vel, float radius,
                              float r, float g, float b, float a, float duration,
                              int startTime, int fadeInTime, int leFlags, qhandle_t hShader) {
	(void)p; (void)vel; (void)radius; (void)r; (void)g; (void)b; (void)a;
	(void)duration; (void)startTime; (void)fadeInTime; (void)leFlags; (void)hShader;
	return NULL;
}

void CG_BigLightningEffect(vec3_t org) { (void)org; }
void CG_LightningEffect(vec3_t org, clientInfo_t *ci, int tier) { (void)org; (void)ci; (void)tier; }
void CG_DirtPush(vec3_t org, vec3_t dir, int size) { (void)org; (void)dir; (void)size; }
void CG_WaterRipple(vec3_t org, int size, qboolean single) { (void)org; (void)size; (void)single; }
void CG_WaterSplash(vec3_t org, int size) { (void)org; (void)size; }
void CG_DrawBoundingBox(vec3_t origin, vec3_t mins, vec3_t maxs) {
	(void)origin; (void)mins; (void)maxs;
}

void CG_ImpactMark(qhandle_t markShader, const vec3_t origin, const vec3_t dir,
                   float orientation, float r, float g, float b, float a,
                   qboolean alphaFade, float radius, qboolean temporary) {
	(void)markShader; (void)origin; (void)dir; (void)orientation;
	(void)r; (void)g; (void)b; (void)a; (void)alphaFade; (void)radius; (void)temporary;
}

qboolean CG_weapGfx_Parse(char *filename, int clientNum) {
	(void)filename; (void)clientNum;
	return qtrue;
}

void BG_EvaluateTrajectory(entityState_t *es, const trajectory_t *tr, int atTime, vec3_t result) {
	(void)es; (void)tr; (void)atTime;
	if (result) {
		VectorClear(result);
	}
}

/* --- syscalls ------------------------------------------------------------- */

void trap_R_AddRefEntityToScene(const refEntity_t *re) { (void)re; }
void trap_R_AddPolyToScene(qhandle_t hShader, int numVerts, const polyVert_t *verts) {
	(void)hShader; (void)numVerts; (void)verts;
}

int trap_R_LightForPoint(vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir) {
	(void)point;
	if (ambientLight)  { VectorClear(ambientLight); }
	if (directedLight) { VectorClear(directedLight); }
	if (lightDir)      { VectorSet(lightDir, 0, 0, 1); }
	return 0;
}

void trap_R_ModelBounds(clipHandle_t model, vec3_t mins, vec3_t maxs, int frame) {
	(void)model; (void)frame;
	if (mins) { VectorClear(mins); }
	if (maxs) { VectorClear(maxs); }
}

void trap_S_StartSound(vec3_t origin, int entityNum, int entchannel, sfxHandle_t sfx) {
	(void)origin; (void)entityNum; (void)entchannel; (void)sfx;
}

void trap_S_AddLoopingSound(int entityNum, const vec3_t origin, const vec3_t velocity, sfxHandle_t sfx) {
	(void)entityNum; (void)origin; (void)velocity; (void)sfx;
}

sfxHandle_t trap_S_RegisterSound(const char *sample, qboolean compressed) {
	(void)sample; (void)compressed;
	return 0;
}

void trap_CM_BoxTrace(trace_t *results, const vec3_t start, const vec3_t end,
                      const vec3_t mins, const vec3_t maxs, clipHandle_t model, int brushmask) {
	(void)start; (void)mins; (void)maxs; (void)model; (void)brushmask;
	if (results) {
		memset(results, 0, sizeof(*results));
		results->fraction = 1.0f;
		if (end) {
			VectorCopy(end, results->endpos);
		}
	}
}

int trap_CM_PointContents(const vec3_t p, clipHandle_t model) {
	(void)p; (void)model;
	return 0;
}

int trap_MemoryRemaining(void) {
	return 16 * 1024 * 1024;
}

int trap_FS_FOpenFile(const char *qpath, fileHandle_t *f, fsMode_t mode) {
	(void)qpath; (void)mode;
	if (f) { *f = 0; }
	return -1;
}

void trap_FS_Read(void *buffer, int len, fileHandle_t f) {
	(void)f;
	if (buffer && len > 0) {
		memset(buffer, 0, (size_t)len);
	}
}

void trap_FS_FCloseFile(fileHandle_t f) { (void)f; }
