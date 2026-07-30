/*
Engine/client/snd_dma.c - stereo spatialisation.

S_SpatializeOrigin turns a world position into a left/right volume pair:

    scale      = (1.0 - dist) * rscale;
    *right_vol = (master_vol * scale);      // float -> int, unchecked
    if (*right_vol < 0) *right_vol = 0;     // clamp AFTER the conversion

The clamp runs after the cast, so an out-of-range float has already been
converted by then. Converting a float that does not fit in an int is undefined
behaviour; on arm64 it saturates, so *left_vol comes back as INT_MAX. That value
then flows into ch->leftvol and reappears one subsystem away as

    snd_mix.c:424  samp[i].left += (data * leftvol) >> 8;

where UBSan reports "signed integer overflow: 2147483647 * 204". Two findings,
one root cause.

'dist' is only clamped below, never above, so any sufficiently distant sound - or
a NaN arriving through the attenuation - reaches the cast out of range. The
contract these tests assert is the one the code already tries to enforce, just
too late: a volume is between 0 and master_vol.
*/

#include <criterion/criterion.h>

#include "q_shared.h"
#include "qcommon.h"
#include "snd_local.h"

/*
listener_origin and listener_axis are file-static in snd_dma.c, so the tests
cannot drive them. That is fine and in fact useful: they start zeroed, which puts
the listener at the world origin with a zero rotation matrix, so VectorRotate
yields a zero vector, dot is 0 and both channels get an equal 0.5 pan. Fully
deterministic, and the distance term - the part that overflows - is still driven
entirely by the origin and attenuation arguments.

dma is not static, so the stereo/mono branch is reachable.
*/
extern dma_t dma;

void S_SpatializeOrigin(vec3_t origin, int master_vol, int *left_vol,
                        int *right_vol, float attenuation);

#define MASTER_VOL 255

static void snd_setup(void) {
	memset(&dma, 0, sizeof(dma));
	dma.channels = 2;          /* stereo, so the spatialisation path runs */
}

TestSuite(snd, .init = snd_setup);

static void spatialize(float x, float y, float z, float attenuation,
                       int *left, int *right) {
	vec3_t origin;

	VectorSet(origin, x, y, z);
	*left = *right = -12345;   /* poison, so "untouched" is visible */
	S_SpatializeOrigin(origin, MASTER_VOL, left, right, attenuation);
}

static void assert_sane(int left, int right, const char *what) {
	cr_assert_geq(left, 0, "%s: left volume is negative (%d)", what, left);
	cr_assert_geq(right, 0, "%s: right volume is negative (%d)", what, right);
	cr_assert_leq(left, MASTER_VOL,
	              "%s: left volume %d exceeds master_vol %d", what, left, MASTER_VOL);
	cr_assert_leq(right, MASTER_VOL,
	              "%s: right volume %d exceeds master_vol %d", what, right, MASTER_VOL);
}

/* ------------------------------------------------- ordinary cases (green) */

Test(snd, sound_at_the_listener_is_audible_in_both_ears) {
	int left, right;

	spatialize(0.0f, 0.0f, 0.0f, 0.0008f, &left, &right);

	assert_sane(left, right, "at listener");
	cr_assert_gt(left, 0, "a sound on top of the listener should be audible");
	cr_assert_gt(right, 0, "a sound on top of the listener should be audible");
}

Test(snd, nearby_sound_is_within_range) {
	int left, right;

	spatialize(100.0f, 0.0f, 0.0f, 0.0008f, &left, &right);
	assert_sane(left, right, "nearby");
}

Test(snd, mono_output_does_not_spatialize) {
	int left, right;

	dma.channels = 1;
	spatialize(100.0f, 50.0f, 0.0f, 0.0008f, &left, &right);

	assert_sane(left, right, "mono");
	cr_assert_eq(left, right, "mono output must not pan");
}

/* ------------------------------------------------------- the red tests */

/*
'dist' is clamped at zero from below but never bounded above, so (1.0 - dist)
grows without limit with distance. At roughly 1e12 units the product exceeds
INT_MAX and the cast is undefined. A sound entity with a corrupt or absurd origin
is not exotic - it is what a bad snapshot looks like.
*/
Test(snd, very_distant_sound_does_not_overflow_the_conversion) {
	int left, right;

	spatialize(1.0e12f, 0.0f, 0.0f, 0.0008f, &left, &right);
	assert_sane(left, right, "very distant");
}

Test(snd, extreme_attenuation_does_not_overflow_the_conversion) {
	int left, right;

	spatialize(1000.0f, 0.0f, 0.0f, 1.0e30f, &left, &right);
	assert_sane(left, right, "extreme attenuation");
}

/*
NaN survives "if (dist < 0)" because every comparison against NaN is false, so it
propagates through to the cast. Converting NaN to int is undefined, and the
post-cast "< 0" clamp cannot catch what saturation already produced.
*/
Test(snd, nan_attenuation_yields_silence_not_garbage) {
	int left, right;
	float nan_value = 0.0f / 0.0f;

	spatialize(1000.0f, 0.0f, 0.0f, nan_value, &left, &right);
	assert_sane(left, right, "NaN attenuation");
}
