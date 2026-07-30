#include "cg_local.h"
#include "fake_lerptag.h"

static fakeLerpTagCall_t	lastCall;
static int				reportFound = 1;

void fake_lerptag_reset(void) {
	memset(&lastCall, 0, sizeof(lastCall));
	reportFound = 1;
}

const fakeLerpTagCall_t *fake_lerptag_last(void) {
	return &lastCall;
}

void fake_lerptag_set_found(int found) {
	reportFound = found;
}

int trap_R_LerpTag(orientation_t *tag, clipHandle_t mod, int startFrame, int endFrame,
                   float frac, const char *tagName) {
	(void)frac;

	lastCall.calls++;
	lastCall.model = (int)mod;
	lastCall.startFrame = startFrame;
	lastCall.endFrame = endFrame;
	Q_strncpyz(lastCall.tagName, tagName ? tagName : "", sizeof(lastCall.tagName));

	if (!reportFound) {
		return 0;
	}

	/* An identity orientation. The caller multiplies it into the refEntity's
	   axis, so anything non-degenerate would just obscure the assertion. */
	if (tag) {
		VectorClear(tag->origin);
		AxisClear(tag->axis);
	}
	return 1;
}
