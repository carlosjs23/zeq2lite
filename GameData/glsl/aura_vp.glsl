#version 120
/*
 * aura_vp.glsl
 * screen-space aura vertex program
 *
 * The mesh is a flat ring of quads whose authored positions are never used.
 * Each vertex instead carries its position relative to the ring centre in the
 * colour channel, and this program rebuilds the shape every frame against the
 * player's screen-space bounding box:
 *
 *   gl_Color.rg  unit-circle direction, remapped so the centre is 0.5
 *   gl_Color.b   0 for the inner ring, 1 for the outer ring
 *   gl_MultiTexCoord0  u runs around the ring, v runs inner -> outer
 *
 * u_ProgramParams, set per entity by cg_auras.c:
 *
 *   [0].xyz  force direction in world space   [0].w  strength
 *   [1].xyz  bounding box mins                [1].w  origin distance
 *   [2].xyz  bounding box maxs                [2].w  bounding box padding
 *   [3].x    amplitude  .y wavelength  .z scroll speed  .w unused
 */

uniform vec4 u_ProgramParams[4];
uniform mat4 u_ModelViewProjectionMatrix;
uniform mat4 u_ProjectionMatrix;
uniform float u_Time;

/* Below this the flattened force vector is too short to carry a direction.
   Normalising it anyway yields NaN, which does not crash a vertex shader - it
   silently produces degenerate geometry, which is far harder to diagnose. This
   happens routinely: it is exactly what a player flying straight at or away
   from the camera looks like once the force is projected onto the view plane. */
#define FORCE_EPSILON 0.0001

/* Half-width of the band, in dot-product units, over which the texture's
   scroll direction is blended rather than mirrored outright. The direction has
   to reverse somewhere - it flows toward the tip on both sides - and a hard
   flip leaves a visible seam at the tip *and* the base. */
#define SEAM_BLEND 0.25

/* Screen half-extent, in NDC, at which the authored wrap count is exactly
   right. The flame is computed in the fragment stage, so there is no texel
   budget - but the tongue count is still an apparent-size question: a distant
   aura carrying its full count crams the tongues into a few dozen pixels and
   the rim shimmers. Halving the wraps as the aura halves on screen keeps the
   tongues a steady width in pixels. */
#define SPIKE_REF 0.34

/* Where the inner ring sits, as a fraction of the outline. Nearly the
   whole disc - the reference's interior veil is part of its field - but
   not exactly zero: at the centre every strip column converges on one
   point, and the wedge interpolation streaks spokes through it. The
   character stands on the hole this leaves. */
#define INNER_HUG 0.05

/* Hard ceiling on the outline's crown reach in NDC; see the span comment. */
#define SPAN_CAP  0.85


varying float v_seamBlend;
/* 0 on the inner ring, 1 at the spike tips. Carried separately from the
   texture coordinate because that one has amplitude folded into it, and the
   tip tint has to key off the ring itself rather than off how far the strip
   has been stretched. */
varying float v_edge;
/* Wraps of the flame pattern around the ring. The fragment stage computes the
   flame procedurally on a lattice that has to close where the ring does, so
   it needs the count this stage settled on. */
varying float v_wraps;
/* 1 directly under the character, falling to 0 by the flanks. The fragment
   stage brightens against it: the reference puts a hot point where the ki
   meets the ground, and the geometry alone only gives that spot a shape. */
varying float v_base;

void main(void) {
	vec3  force        = u_ProgramParams[0].xyz;
	float strength     = u_ProgramParams[0].w;
	vec3  boxMins      = u_ProgramParams[1].xyz;
	float originDist   = u_ProgramParams[1].w;
	vec3  boxMaxs      = u_ProgramParams[2].xyz;
	float padding      = u_ProgramParams[2].w;
	float amplitude    = u_ProgramParams[3].x;
	float wavelength   = u_ProgramParams[3].y;
	float scrollSpeed  = u_ProgramParams[3].z;

	/* --- the player's bounding box, in screen space -------------------- */

	/* Project all eight corners and take the extremes. A vertex shader has no
	   way to share this across vertices, so every vertex repeats it; at 128
	   vertices per aura that is cheaper than any means of avoiding it. */
	vec2 boxMin = vec2( 1e9);
	vec2 boxMax = vec2(-1e9);
	float depth = 1e9;

	for (int i = 0; i < 8; i++) {
		vec3 corner = vec3(
			(i     - 2 * (i / 2)) == 0 ? boxMins.x : boxMaxs.x,
			(i / 2 - 2 * (i / 4)) == 0 ? boxMins.y : boxMaxs.y,
			(i / 4)               == 0 ? boxMins.z : boxMaxs.z);

		vec4 clip = u_ModelViewProjectionMatrix * vec4(corner, 1.0);

		/* Behind-camera corners have w <= 0 and project to garbage. Clamping
		   keeps a partially-behind box from inverting the whole aura. */
		float w = max(clip.w, FORCE_EPSILON);
		vec2 ndc = clip.xy / w;

		boxMin = min(boxMin, ndc);
		boxMax = max(boxMax, ndc);

		/* One depth for the whole ring, so the aura sorts as a flat sheet
		   rather than slicing into geometry - but the *nearest* corner, not
		   the box centre and certainly not corner zero, which is whichever
		   corner happens to be at the world-space minimum. The skirt hangs
		   below the player's feet, and the floor it hangs over is nearer to
		   the camera than the player is; sorting the sheet any further back
		   lets that floor swallow the base of the ring, leaving the two
		   flanks standing unconnected. Which corner is furthest also depends
		   on the view azimuth, which is why the base used to survive from
		   some angles and not others. */
		depth = min(depth, clip.z / w);
	}

	/* A corner behind the camera lands at an absurd depth once w is clamped.
	   Pinning the result inside the frustum turns that into "draw in front of
	   everything", which is what a camera sitting inside the player should
	   look like anyway, instead of near-plane clipping the aura away. */
	gl_Position.z = clamp(depth, -0.99, 0.99);

	vec2 boxCentre = (boxMin + boxMax) * 0.5;
	vec2 boxHalf   = (boxMax - boxMin) * 0.5 + vec2(padding);

	/* --- where this vertex sits ---------------------------------------- */

	vec2  rel     = gl_Color.rg * 2.0 - 1.0;   /* unit-circle direction */
	float isOuter = gl_Color.b;
	vec2  dir     = normalize(rel + vec2(FORCE_EPSILON));

	/* --- force, flattened onto the view plane --------------------------- */

	vec4  forceClip = u_ModelViewProjectionMatrix * vec4(force, 0.0);
	vec2  forceDir  = forceClip.xy;
	float forceLen  = length(forceDir);

	/* Fall back to screen-down when the force carries no usable direction,
	   which is what gravity would give and keeps the tear-drop upright. */
	forceDir = forceLen > FORCE_EPSILON ? forceDir / forceLen : vec2(0.0, -1.0);

	/* The aura streams *against* the force: gravity pulling down makes it
	   flare upward, and running forward trails it behind. Everything below is
	   expressed in terms of that flow direction rather than the force itself. */
	vec2 flowDir = -forceDir;

	/* The mesh carries the reference outline: gl_Color.a is the boundary
	   radius at this vertex's authored angle, crown-normalised, read off the
	   reference art's alpha mask by make_aura_mesh.py. The silhouette is the
	   reference's own - every tongue of it - rather than any width function's
	   approximation. The tip is authored at +Y, so the directional terms below
	   work in the authored frame, and the finished shape is rotated rigidly
	   onto the flow at the end. */
	float rRef = gl_Color.a;

	float along = dir.y;

	/* Squared so the base terms collapse away from the bottom pole. */
	float base = max(-along, 0.0);
	base *= base;

	/* No wobble: it existed to break up a perfect ellipse, and the outline
	   is the reference's own art now. The life comes from the scroll
	   sliding the field along the boundary. */

	/* The inner ring hugs a scaled copy of the same outline, so the band's
	   thickness follows the tongues instead of cutting across them. */
	float rr  = mix(rRef * INNER_HUG, rRef, isOuter);
	vec2 pRef = dir * rr;

	/* Rotate the authored shape rigidly so its tip rides the flow. With the
	   flow straight up this is the identity. */
	/* Isotropic on purpose: the outline is the reference's own curve, and
	   any per-axis spread on top of it is a departure from the art. */
	vec2 rightDir = vec2(flowDir.y, -flowDir.x);
	vec2 shape    = rightDir * pRef.x + flowDir * pRef.y;

	/* NDC spans -1..1 on both axes regardless of window shape, so an equal
	   offset in x and y is not equal on screen. The projection matrix carries
	   the ratio: [1][1]/[0][0] is width/height. */
	float aspect   = u_ProjectionMatrix[1][1] / u_ProjectionMatrix[0][0];
	vec2  evenly   = vec2(1.0 / max(aspect, FORCE_EPSILON), 1.0);

	float halfWidth  = boxHalf.x * aspect;
	float halfHeight = boxHalf.y;
	float girth      = min(halfWidth, halfHeight);
	float reach      = max(halfWidth, halfHeight);

	/* One scalar span maps the outline onto the character: the crown reach -
	   the outline's unit radius - lands above the box top by however much
	   plume `strength` buys. Height alone, deliberately: the reference's
	   proportions are the aura's own, and sizing any part of the outline off
	   the body's width would squeeze the drop around a thin character and
	   fatten it around a broad one.

	   Clamped because screen-space sizing has one pathological regime: a
	   close camera puts the half-extent near a full screen, and a span
	   proportional to it towers a wall of flame over the character. The aura
	   may fill the view; its flame may not. */
	float span = halfHeight * (0.7 + 1.0 * strength);
	span = min(span, SPAN_CAP);

	vec2 pos = boxCentre + shape * evenly * span;

	gl_Position.xy = pos;
	gl_Position.w  = 1.0;

	/* --- texturing ------------------------------------------------------ */

	/* Which side of the tip axis this vertex is on decides which way the
	   texture scrolls, so that it always flows toward the tip. Authored
	   frame: the shape is rotated rigidly, and the pattern rides the shape. */
	float side = dir.x;

	/* Blended rather than mirrored: smoothstep gives 0 on one side, 1 on the
	   other, and a short ramp through the tip and base where the two meet. */
	v_seamBlend = smoothstep(-SEAM_BLEND, SEAM_BLEND, side);

	/* --- spike density against apparent size ---------------------------- */

	/* `wavelength` counts wraps of the strip around the ring, and a fixed
	   count means a player across the map crams every one of those spikes
	   into a few dozen pixels: the strip undersamples and the whole rim
	   crawls. boxHalf is the aura's apparent size, so halve the count each
	   time the aura halves on screen.

	   Halved rather than scaled smoothly because the strip repeats along S:
	   the ring only meets itself cleanly when the number of wraps is a whole
	   number, and a fractional count draws a hard vertical seam where the
	   ring closes. Everything here reads only uniforms, so every vertex of
	   the ring agrees on the answer.

	   `reach`, deliberately, and not anything per-vertex: a wrap count that
	   has to come out identical for every vertex can only be fed uniforms. */
	/* Unclamped in both directions: a distant aura halves its wraps so the
	   tongues stay legible, and a close one doubles them so a tongue never
	   becomes a monster filling half the screen. */
	float shrink = log2(max(reach, FORCE_EPSILON) / SPIKE_REF);
	float steps  = -floor(shrink);
	float wraps  = max(floor(wavelength * exp2(-steps)), 1.0);

	v_wraps = wraps;

	/* The authored coordinate advances with the outline's own arc length -
	   make_aura_mesh.py bakes the cumulative arc into it - and the band
	   strip is that same arc unwrapped, so one turn of the ring is one width
	   of the strip: no wrap count. Scroll slides the reference's own field
	   around the boundary. */
	float u = gl_MultiTexCoord0.x + u_Time * scrollSpeed;

	/* Sheared with distance out, so every lick leans. The same sign on both
	   copies: the mirrored one already reverses U, so an equal shear comes out
	   leaning the opposite way on the far side of the aura - which is what is
	   wanted, since both sides should sweep toward the tip rather than all
	   leaning the one way around the ring like a vortex. */
	/* No shear and no mirrored copy: the strip is the reference's own
	   field, its licks already lean in the art, and anything added on top
	   moves the material off the geometry that carries its outline. */
	gl_TexCoord[0] = vec4( u, gl_MultiTexCoord0.y * amplitude, 0.0, 1.0);
	gl_TexCoord[1] = gl_TexCoord[0];

	v_edge = gl_MultiTexCoord0.y;
	v_base = base;

	/* Vertex colour is payload, not colour - do not pass it to the fragment
	   stage. The aura's tint comes from the entity colour instead. */
	gl_FrontColor = gl_BackColor = vec4(1.0);
}
