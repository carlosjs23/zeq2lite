/*
Link seam for g_rules.c.

The rule engine reaches only COM_Parse, the FS traps and G_Alloc, which is what
makes it testable at all. g_mem.c is linked for real rather than stubbed so the
suite exercises the actual bump allocator and its POOLSIZE, and the FS traps
come from fake_fs.c so content sits next to the assertions.

G_Error is declared noreturn and g_mem.c is its only caller here, on pool
exhaustion. Aborting turns that into a Criterion crash for one test rather than
a silent short return.
*/

#include <stdio.h>
#include <stdlib.h>

#include "g_local.h"

vmCvar_t g_debugAlloc = { 0, 0, 0.0f, 0, "0" };

void QDECL G_Printf(const char *fmt, ...) { (void)fmt; }

void QDECL G_Error(const char *fmt, ...) {
	(void)fmt;
	fprintf(stderr, "G_Error from the game module\n");
	abort();
}
