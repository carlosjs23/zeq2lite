/*
An observable trap_R_LerpTag.

The tag lookups in cg_players.c hand the renderer a model handle and a pair of
animation frames taken from the playerEntity_t they decided to read. Which
playerEntity_t that was is the thing under test, and it is not otherwise
visible from outside the translation unit - the storage is static.

So this is a fake, not a stub: it records its arguments so a test can assert on
*whose* pose was passed down.

Like fake_fs.h, this header pulls in no engine headers. cg_local.h and
bg_public.h have no include guards, so a second inclusion is a hard error;
handles are plain ints there anyway.
*/

#ifndef FAKE_LERPTAG_H
#define FAKE_LERPTAG_H

#define FAKE_LERPTAG_MAXTAG	64	/* MAX_QPATH */

typedef struct {
	int	calls;
	int	model;		/* the clipHandle_t the caller read out of pe */
	int	startFrame;
	int	endFrame;
	char	tagName[FAKE_LERPTAG_MAXTAG];
} fakeLerpTagCall_t;

/* Forget every recorded call and go back to reporting "tag found". */
void fake_lerptag_reset(void);

/* The most recent call. calls == 0 means it was never invoked. */
const fakeLerpTagCall_t *fake_lerptag_last(void);

/* Whether trap_R_LerpTag should report the tag as found (default: yes). */
void fake_lerptag_set_found(int found);

#endif /* FAKE_LERPTAG_H */
