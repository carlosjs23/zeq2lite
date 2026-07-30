/*
Link seams specific to cgame units.

cg_local.h declares the two big cgame globals plus a wall of trap_* syscalls.
A unit test only needs the ones its unit actually references, but providing the
common set here keeps individual suites free of boilerplate.

cg and cgs are defined (not just declared) so a suite can drive them directly,
e.g. seeding cgs.music before calling into cg_music.c. Criterion gives every
test its own process, so a test that dirties them cannot leak into the next one.
*/

#include "cg_local.h"

/* The cgame world. Zero-initialised, which is what CG_Init would start from. */
cg_t  cg;
cgs_t cgs;

/* Cvars referenced by units under test. */
vmCvar_t cg_music;

void QDECL CG_Printf(const char *msg, ...) {
	va_list ap;
	va_start(ap, msg);
	vfprintf(stderr, msg, ap);
	va_end(ap);
}

void QDECL CG_Error(const char *msg, ...) {
	va_list ap;
	fprintf(stderr, "\n*** CG_Error: ");
	va_start(ap, msg);
	vfprintf(stderr, msg, ap);
	va_end(ap);
	fprintf(stderr, " ***\n");
	abort();
}

/* --- inert syscalls -------------------------------------------------------- */

void trap_Cvar_Set(const char *var_name, const char *value) {
	(void)var_name;
	(void)value;
}

void trap_Cvar_VariableStringBuffer(const char *var_name, char *buffer, int bufsize) {
	(void)var_name;
	if (bufsize > 0) {
		buffer[0] = '\0';
	}
}

void trap_S_StartBackgroundTrack(const char *intro, const char *loop) {
	(void)intro;
	(void)loop;
}

void trap_S_StopBackgroundTrack(void) {
}
