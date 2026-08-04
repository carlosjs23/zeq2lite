/*
RB_IQMSurfaceAnim - the vertex path for IQM models.

This suite exists because the screen-space aura design needs a mesh format that
carries a per-vertex colour channel: the technique stores each vertex's position
relative to the aura centre in R/G and an inner/outer flag in B, then rebuilds
the shape in a vertex shader. MD3 has no colour channel. IQM does, and
RB_IQMSurfaceAnim copies it straight into tess.vertexColors.

Three things about that path are load-bearing and none is obvious from reading
it, so they are pinned here before anything is built on top:

  1. The vertex transform is skinning-only. Every output position is
     blendWeights * jointMats, so a mesh authored with no joints produces zero
     weights and collapses every vertex onto the origin. A static ring mesh must
     therefore be authored with one identity joint and all weights at 255 - it
     is an authoring constraint, not an engine bug, and it is silent when you
     get it wrong.

  2. frame and oldframe are reduced with "% data->num_frames" unguarded, so a
     genuinely frameless mesh divides by zero.

  3. The blend walks exactly four influences and stops at the first
     non-positive weight, so a writer must pack them from slot 0 with no gap.
     This one matters to the skinning decomposition, whose whole output is
     multi-influence vertices.

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
	/* Two joints' worth: the four-influence blend test needs a second one, and
	   the single-joint tests only ever touch the first. */
	float			poseMats[2 * 12];
	int				jointParents[2];
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

Test(iqm, a_vertex_blends_across_four_bones) {
	/* What the skinning decomposition writes: a vertex weighted to several
	   bones at once. Pinned because the blend has two properties the writer has
	   to satisfy and neither is stated in the IQM specification - the loop runs
	   over exactly four slots, and it breaks at the first non-positive weight
	   rather than skipping it, so influences must be packed from slot 0 with no
	   gap. A writer that leaves a hole silently drops every influence after it.

	   One joint stands still, the other translates 100 on x, and the vertex is
	   split 191/64 between them: it has to land 64/255 of the way along. */
	seed_fixture(1, 2);

	fx.data.num_joints = 2;
	memcpy(fx.poseMats, identityJoint, sizeof(identityJoint));
	memcpy(fx.poseMats + 12, identityJoint, sizeof(identityJoint));
	fx.poseMats[12 + 3] = 100.0f;	/* second joint translates +100 on x */
	fx.jointParents[1] = -1;

	fx.blendIndexes[0] = 0;
	fx.blendIndexes[1] = 1;
	fx.blendWeights[0] = 191;	/* 191/255 on the joint that does not move */
	fx.blendWeights[1] = 64;

	RB_IQMSurfaceAnim((surfaceType_t *)&fx.surf);

	cr_assert_float_eq(tess.xyz[0][0], 1.0f + 100.0f * 64.0f / 255.0f, 0.01f,
	                   "a two-bone blend should land between the bones");

	/* A zero in slot 1 must stop the walk: slot 2's weight is ignored even
	   though it is set. This is the packing rule, stated as a measurement. */
	seed_fixture(1, 2);
	fx.data.num_joints = 2;
	memcpy(fx.poseMats, identityJoint, sizeof(identityJoint));
	memcpy(fx.poseMats + 12, identityJoint, sizeof(identityJoint));
	fx.poseMats[12 + 3] = 100.0f;
	fx.jointParents[1] = -1;

	fx.blendIndexes[0] = 0;
	fx.blendWeights[0] = 255;
	fx.blendIndexes[1] = 1;
	fx.blendWeights[1] = 0;
	fx.blendIndexes[2] = 1;
	fx.blendWeights[2] = 128;

	RB_IQMSurfaceAnim((surfaceType_t *)&fx.surf);

	cr_assert_float_eq(tess.xyz[0][0], 1.0f, 0.01f,
	                   "influences after a zero weight must not contribute - "
	                   "if they now do, the writer's packing rule is gone");
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

/* --------------------------------------------------------------------------
   R_IQMLerpTag - bones as attachment points.

   The skeletal characters hang gear off bones by name, through the same
   trap_R_LerpTag the md3 path uses for tags. Two properties of that make the
   whole scheme work, and both are invisible until an attachment lands in the
   wrong place:

     1. A tag's transform is the *accumulated* pose chain, so a bone parented
        under another inherits it. Gear on a head bone follows the head.

     2. What comes back is ComputeJointMats' output, which is
        pose_global * inverse(bind_global) - a skinning matrix, not the joint's
        transform. The two coincide only when the bind pose is the identity,
        which is why Tools/dev/iqm.py emits every joint bound at the identity
        and puts the whole rest pose in frame 0. A converter that bound joints
        at their rest transforms would produce gear offset by that transform,
        with nothing in the log to say so.
   -------------------------------------------------------------------------- */

typedef struct {
	iqmData_t	data;
	float		poseMats[2 * 12];
	int			jointParents[2];
	char		names[16];
} lerpTagFixture_t;

static lerpTagFixture_t tagFx;

static void seed_lerp_tag(void) {
	memset(&tagFx, 0, sizeof(tagFx));

	memcpy(tagFx.poseMats, identityJoint, sizeof(identityJoint));
	memcpy(tagFx.poseMats + 12, identityJoint, sizeof(identityJoint));
	tagFx.poseMats[3] = 10.0f;		/* root translated  +10 x */
	tagFx.poseMats[12 + 7] = 5.0f;	/* head translated  +5 y, in root space */

	tagFx.jointParents[0] = -1;
	tagFx.jointParents[1] = 0;

	memcpy(tagFx.names, "root\0head\0", 10);

	tagFx.data.num_joints   = 2;
	tagFx.data.num_frames   = 1;
	tagFx.data.poseMats     = tagFx.poseMats;
	tagFx.data.jointParents = tagFx.jointParents;
	tagFx.data.names        = tagFx.names;
}

Test(iqm, a_bone_resolves_as_a_tag_by_name) {
	orientation_t	tag;

	seed_lerp_tag();
	memset(&tag, 0, sizeof(tag));

	cr_assert(R_IQMLerpTag(&tag, &tagFx.data, 0, 0, 0.0f, "root"),
	          "a joint named in the names blob must resolve");
	cr_assert_float_eq(tag.origin[0], 10.0f, 0.001f,
	                   "the root tag should sit where its pose puts it");
}

Test(iqm, a_child_bone_tag_inherits_its_parent) {
	orientation_t	tag;

	seed_lerp_tag();
	memset(&tag, 0, sizeof(tag));

	cr_assert(R_IQMLerpTag(&tag, &tagFx.data, 0, 0, 0.0f, "head"));

	/* 10 from the root, 5 from the head's own pose. If this ever returns the
	   head's local pose alone, gear parented to a bone stops following the
	   chain and every attachment on a converted character moves to the model
	   origin's neighbourhood. */
	cr_assert_float_eq(tag.origin[0], 10.0f, 0.001f,
	                   "a child bone must inherit its parent's translation");
	cr_assert_float_eq(tag.origin[1], 5.0f, 0.001f,
	                   "a child bone must keep its own translation");
}

Test(iqm, an_unknown_bone_name_fails_rather_than_returning_junk) {
	orientation_t	tag;

	seed_lerp_tag();
	memset(&tag, 0xff, sizeof(tag));

	cr_assert_not(R_IQMLerpTag(&tag, &tagFx.data, 0, 0, 0.0f, "tag_weapon"),
	              "a name no joint carries must not resolve");
	cr_assert_float_eq(tag.origin[0], 0.0f, 0.001f,
	                   "a failed lookup must clear the tag, not leave it stale");
}
