/*
Link seam for g_tiers.c.

The tier parser and the transformation predicate reach exactly five cvars, all
of them scaling knobs applied on top of the parsed config. Real vmCvar_t storage
with the shipped defaults keeps syncTier's arithmetic meaningful without any
test having to care about them.

The FS traps this unit needs come from fake_fs.c, so the configs under test are
declared inline next to the assertions.
*/

#include "g_local.h"

/* g_breakLimitRate scales the parsed breakLimitRate; the quick-* pair only
   participates when a config leaves the corresponding field at -1. 1.0 and -1.0
   are the shipped defaults, so parsed values pass through unchanged. */
vmCvar_t g_breakLimitRate           = { 0, 0, 1.0f, 1, "1" };
vmCvar_t g_quickTransformCost       = { 0, 0, 1.0f, 1, "1" };
vmCvar_t g_quickTransformCostPerTier = { 0, 0, 1.0f, 1, "1" };
vmCvar_t g_quickZanzokenCost        = { 0, 0, -1.0f, -1, "-1" };
vmCvar_t g_quickZanzokenDistance    = { 0, 0, -1.0f, -1, "-1" };

/* g_tiers.c reaches the game module's printf wrapper for diagnostics. */
void QDECL G_Printf(const char *fmt, ...) { (void)fmt; }

/* Training gates the ability to ascend from outside this unit. Reporting every
   tier unlocked is what a server with g_training 0 does, so the tier vectors
   keep measuring the power requirements they were written for. */
qboolean G_TrainingTierUnlocked(gclient_t *client, int tier) {
	(void)client;
	(void)tier;
	return qtrue;
}
