/*
RB_IQMSurfaceAnim - the vertex path for IQM models.

This suite exists because the screen-space aura design needs a mesh format that
carries a per-vertex colour channel: the technique stores each vertex's position
relative to the aura centre in R/G and an inner/outer flag in B, then rebuilds
the shape in a vertex shader. MD3 has no colour channel. IQM does, and
RB_IQMSurfaceAnim copies it straight into tess.vertexColors.

Two things about that path are load-bearing and neither is obvious from reading
it, so they are pinned here before anything is built on top:

  1. The vertex transform is skinning-only. Every output position is
     blendWeights * jointMats, so a mesh authored with no joints produces zero
     weights and collapses every vertex onto the origin. A static ring mesh must
     therefore be authored with one identity joint and all weights at 255 - it
     is an authoring constraint, not an engine bug, and it is silent when you
     get it wrong.

  2. frame and oldframe are reduced with "% data->num_frames" unguarded, so a
     genuinely frameless mesh divides by zero.

Both are properties of the format's use here, not of any particular asset, which
is why they are worth a test rather than a comment.
*/

#include <criterion/criterion.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tr_local.h"
#include "iqm.h"

#define VERT_COUNT	3

/* An identity 3x4 joint matrix, in the row-major layout ComputeJointMats emits. */
static const float identityJoint[12] = {
	1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 1, 0
};

typedef struct {
	iqmData_t		data;
	srfIQModel_t	surf;
	float			positions[3 * VERT_COUNT];
	float			normals[3 * VERT_COUNT];
	float			texcoords[2 * VERT_COUNT];
	byte			colors[4 * VERT_COUNT];
	byte			blendIndexes[4 * VERT_COUNT];
	byte			blendWeights[4 * VERT_COUNT];
	int				triangles[3];
	float			poseMats[12];
	int				jointParents[1];
	trRefEntity_t	entity;
} iqmFixture_t;

static iqmFixture_t fx;

/* A single-joint, single-frame mesh with the identity pose - the shape a static
   aura ring has to be authored as. Colours are distinct per vertex so the test
   can tell which vertex a channel came from. */
static void seed_fixture(int numFrames, int numJoints) {
	int i;

	memset(&fx, 0, sizeof(fx));
	memset(&tess, 0, sizeof(tess));
	memset(&backEnd, 0, sizeof(backEnd));

	for (i = 0; i < VERT_COUNT; i++) {
		fx.positions[3 * i + 0] = (float)(i + 1);
		fx.positions[3 * i + 1] = (float)(i + 1) * 10.0f;
		fx.positions[3 * i + 2] = (float)(i + 1) * 100.0f;

		fx.normals[3 * i + 2] = 1.0f;

		fx.texcoords[2 * i + 0] = (float)i * 0.25f;
		fx.texcoords[2 * i + 1] = (float)i * 0.5f;

		/* R/G = position relative to centre, B = inner/outer, A = unused.
		   Distinct per vertex so a mix-up is visible. */
		fx.colors[4 * i + 0] = (byte)(10 + i);
		fx.colors[4 * i + 1] = (byte)(20 + i);
		fx.colors[4 * i + 2] = (byte)(i == 0 ? 0 : 255);
		fx.colors[4 * i + 3] = 255;

		/* All weight on joint 0. 255 is "full", because the transform divides
		   the accumulated matrix by 255. */
		fx.blendIndexes[4 * i + 0] = 0;
		fx.blendWeights[4 * i + 0] = 255;

		fx.triangles[i] = i;
	}

	memcpy(fx.poseMats, identityJoint, sizeof(identityJoint));

	/* -1 is "no parent": ComputeJointMats copies the pose straight through
	   rather than concatenating it with a parent's. A single root joint. */
	fx.jointParents[0] = -1;

	fx.data.num_vertexes  = VERT_COUNT;
	fx.data.num_triangles = 1;
	fx.data.num_frames    = numFrames;
	fx.data.num_surfaces  = 1;
	fx.data.num_joints    = numJoints;
	fx.data.positions     = fx.positions;
	fx.data.normals       = fx.normals;
	fx.data.texcoords     = fx.texcoords;
	fx.data.colors        = fx.colors;
	fx.data.blendIndexes  = fx.blendIndexes;
	fx.data.blendWeights  = fx.blendWeights;
	fx.data.triangles     = fx.triangles;
	fx.data.poseMats      = fx.poseMats;
	fx.data.jointParents  = fx.jointParents;
	fx.data.surfaces      = &fx.surf;

	fx.surf.surfaceType   = SF_IQM;
	fx.surf.data          = &fx.data;
	fx.surf.num_vertexes  = VERT_COUNT;
	fx.surf.num_triangles = 1;

	backEnd.currentEntity = &fx.entity;
}

Test(iqm, vertex_colours_reach_the_tessellator) {
	int i;

	seed_fixture(1, 1);

	RB_IQMSurfaceAnim((surfaceType_t *)&fx.surf);

	cr_assert_eq(tess.numVertexes, VERT_COUNT, "every vertex should be emitted");

	/* The whole reason IQM is the candidate format for the aura mesh: the
	   per-vertex channel survives into tess unmodified. */
	for (i = 0; i < VERT_COUNT; i++) {
		cr_assert_eq(tess.vertexColors[i][0], (byte)(10 + i),
		             "vertex %d red channel changed in transit", i);
		cr_assert_eq(tess.vertexColors[i][1], (byte)(20 + i),
		             "vertex %d green channel changed in transit", i);
		cr_assert_eq(tess.vertexColors[i][2], (byte)(i == 0 ? 0 : 255),
		             "vertex %d blue channel (inner/outer flag) changed in transit", i);
	}
}

Test(iqm, one_identity_joint_passes_positions_through_unchanged) {
	int i;

	seed_fixture(1, 1);

	RB_IQMSurfaceAnim((surfaceType_t *)&fx.surf);

	/* If this drifts from the input, the "author with one identity joint"
	   recipe no longer produces an untransformed mesh and the aura's screen
	   space maths would be operating on the wrong positions. */
	for (i = 0; i < VERT_COUNT; i++) {
		cr_assert_float_eq(tess.xyz[i][0], (float)(i + 1), 0.001f,
		                   "vertex %d x moved", i);
		cr_assert_float_eq(tess.xyz[i][1], (float)(i + 1) * 10.0f, 0.001f,
		                   "vertex %d y moved", i);
		cr_assert_float_eq(tess.xyz[i][2], (float)(i + 1) * 100.0f, 0.001f,
		                   "vertex %d z moved", i);
	}
}

Test(iqm, a_boneless_mesh_collapses_to_the_origin) {
	int i;

	/* No joints means no blend weights, and the transform is weights-times-
	   joints all the way down. Documented as a test because the failure is
	   silent: the model loads, draws, and is simply invisible. */
	seed_fixture(1, 0);
	for (i = 0; i < 4 * VERT_COUNT; i++) {
		fx.blendWeights[i] = 0;
	}

	RB_IQMSurfaceAnim((surfaceType_t *)&fx.surf);

	for (i = 0; i < VERT_COUNT; i++) {
		cr_assert_float_eq(tess.xyz[i][0], 0.0f, 0.001f,
		                   "unweighted vertex %d should collapse - if this now "
		                   "holds its position, the skinning-only constraint is "
		                   "gone and the aura mesh need not carry a dummy joint", i);
	}
}

/* A recording ri.Printf. The loader reports every rejection the same way - by
   returning qfalse - so the return value alone cannot tell "rejected for having
   no frames" from "rejected for being a stub header". Capturing the warning is
   what makes the distinction assertable. */
static char lastWarning[1024];

static void QDECL recording_printf(int printLevel, const char *fmt, ...) {
	va_list ap;
	(void)printLevel;
	va_start(ap, fmt);
	vsnprintf(lastWarning, sizeof(lastWarning), fmt, ap);
	va_end(ap);
}

static void install_recorder(void) {
	lastWarning[0] = '\0';
	ri.Printf = recording_printf;
}

/* Enough of an IQM file to reach the header checks. The loader validates magic,
   version and filesize before anything else, so a header alone gets far enough
   to exercise the frame-count guard without building vertex arrays. */
static void seed_header(iqmHeader_t *h, unsigned int numFrames) {
	memset(h, 0, sizeof(*h));
	memcpy(h->magic, IQM_MAGIC, sizeof(h->magic));
	h->version    = IQM_VERSION;
	h->filesize   = sizeof(*h);
	h->num_joints = 1;
	h->num_frames = numFrames;
}

Test(iqm, a_frameless_model_is_rejected_at_load) {
	iqmHeader_t	header;
	model_t		mod;

	/* num_frames == 0 is legal in the IQM format for a static mesh, but this
	   renderer allocates poseMats as num_joints * num_frames matrices, so a
	   frameless model has no pose data at all - and RB_IQMSurfaceAnim reduces
	   the frame index modulo num_frames, dividing by zero on the way to
	   reading past that empty allocation. Rejecting at load is what keeps
	   num_frames == 0 from ever reaching the vertex path. */
	install_recorder();
	memset(&mod, 0, sizeof(mod));
	seed_header(&header, 0);

	cr_assert_not(R_LoadIQM(&mod, &header, (int)sizeof(header), "frameless.iqm"),
	              "a model with no frames must not load");
	cr_assert_neq(strstr(lastWarning, "no frames"), NULL,
	              "expected the frame-count guard to be what rejected it, got: %s",
	              lastWarning);
}

Test(iqm, a_single_frame_model_passes_the_frame_count_guard) {
	iqmHeader_t	header;
	model_t		mod;

	/* The guard must not reject the shape a static aura ring is authored as.
	   A bare header still fails to load for other reasons, so the assertion is
	   on *why* - the frame count must not be the complaint. Without this, a
	   guard that rejected everything would satisfy the test above. */
	install_recorder();
	memset(&mod, 0, sizeof(mod));
	seed_header(&header, 1);

	(void)R_LoadIQM(&mod, &header, (int)sizeof(header), "oneframe.iqm");

	cr_assert_eq(strstr(lastWarning, "no frames"), NULL,
	             "a single-frame model must clear the frame-count guard, got: %s",
	             lastWarning);
}
