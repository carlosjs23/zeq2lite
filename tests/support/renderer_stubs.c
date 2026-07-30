/*
Link seams for renderer units under test.

The renderer keeps its world in four globals - tr, ri, backEnd, tess - and a
unit under test reaches all of them. They are defined (not just declared) here
so a suite can drive them directly, the same way cgame_stubs.c does for cg/cgs.
Criterion gives every test its own process, so dirtying them cannot leak.

Everything else here is inert: these are the functions the units under test
call on paths the tests do not exercise. Anything a test needs to *observe*
belongs in a fake, not here.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "tr_local.h"

/* --- the renderer world --------------------------------------------------- */

refimport_t			ri;
backEndState_t		backEnd;
trGlobals_t			tr;
shaderCommands_t	tess;

static cvar_t		stub_shadows;
cvar_t				*r_shadows = &stub_shadows;

/* --- ri callbacks --------------------------------------------------------- */

/* ri is a struct of function pointers, so a zeroed one crashes the moment a
   unit under test logs anything. Suites call renderer_stubs_install_ri() to get
   working Printf/Error before exercising code that reports errors. */

static void QDECL stub_ri_Printf(int printLevel, const char *fmt, ...) {
	va_list ap;
	(void)printLevel;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void QDECL __attribute__((noreturn)) stub_ri_Error(int errorLevel, const char *fmt, ...) {
	va_list ap;
	(void)errorLevel;
	fprintf(stderr, "\n*** ri.Error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, " ***\n");
	abort();
}

void renderer_stubs_install_ri(void) {
	ri.Printf = stub_ri_Printf;
	ri.Error  = stub_ri_Error;
}

/* --- inert ---------------------------------------------------------------- */

/* Only called when a batch would overflow. A suite that trips this is asking
   for more vertices than tess holds, which is a bug in the test, not the unit. */
void RB_CheckOverflow(int verts, int indexes) {
	(void)verts;
	(void)indexes;
	ri.Error(ERR_DROP, "RB_CheckOverflow: test exceeded tess capacity");
}

void R_AddDrawSurf(surfaceType_t *surface, shader_t *shader, int fogIndex, int dlightMap) {
	(void)surface; (void)shader; (void)fogIndex; (void)dlightMap;
}

int R_CullLocalBox(vec3_t bounds[2]) {
	(void)bounds;
	return CULL_IN;
}

shader_t *R_FindShader(const char *name, int lightmapIndex, qboolean mipRawImage) {
	(void)name; (void)lightmapIndex; (void)mipRawImage;
	return tr.defaultShader;
}

shader_t *R_GetShaderByHandle(qhandle_t hShader) {
	(void)hShader;
	return tr.defaultShader;
}

skin_t *R_GetSkinByHandle(qhandle_t hSkin) {
	(void)hSkin;
	return NULL;
}

void R_SetupEntityLighting(const trRefdef_t *refdef, trRefEntity_t *ent) {
	(void)refdef; (void)ent;
}
