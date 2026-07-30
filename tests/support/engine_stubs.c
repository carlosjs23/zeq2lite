/*
Link seams for units under test.

C has no dependency injection, so the standard way to unit-test a translation
unit is to compile it as-is and satisfy its externals here. Everything in this
file is either a faithful minimal implementation (the Com_Mem* wrappers) or an
inert stub whose only job is to let the linker finish.

Anything a test needs to *observe* belongs in a purpose-built fake instead - see
fake_fs.c - not here.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "q_shared.h"

/* ---------------------------------------------------------------- Com_* core */

/* Real behaviour matters: several parsers rely on Com_Error not returning. */
void QDECL Com_Error(int level, const char *fmt, ...) {
	va_list ap;
	(void)level;
	fprintf(stderr, "\n*** Com_Error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, " ***\n");
	/* Criterion runs each test in its own process, so aborting here fails only
	   the test that provoked it and reports as a crash rather than hanging. */
	abort();
}

void QDECL Com_Printf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void QDECL Com_DPrintf(const char *fmt, ...) {
	(void)fmt;
}

/* Com_Memset/Com_Memcpy need no stub: q_shared.h #defines them to memset/memcpy. */

/* --------------------------------------------------------------------- cvars */

/* msg.c dereferences this for its shownet tracing. A real cvar_t keeps that
   path exercised (integer 0 = quiet) instead of crashing on NULL. */
static cvar_t stub_shownet;
cvar_t *cl_shownet = &stub_shownet;
