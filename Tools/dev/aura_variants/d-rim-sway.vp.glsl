/* Generated variant for the zeq2clip.sh A/B loop - see
   Tools/dev/aura_variants/README.md. The defaults in GameData/glsl are
   authoritative; b-sway matches them by construction. */
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

/* Where the inner ring sits, as a fraction of the outline. Zero - the band
   is the whole disc. The wedge spokes that once streaked through the centre
   came from per-angle sampling magnifying the centroid's pixels; the strip's
   inner rows are blended to their angular mean now, so every column agrees
   at the point where they converge. */
#define INNER_HUG 0.0

/* Hard ceiling on the outline's crown reach in NDC; see the span comment. */
#define SPAN_CAP  0.85

/* How far above the feet plane the grounded part of the sheet sits, in world
   units: enough that the flame lying across the floor wins the depth test
   against it instead of z-fighting, small enough to read as on the ground. */
#define GROUND_LIFT 2.0

/* How far the flame sways around its home position, in turns of the ring.
   The strip is the reference unwrapped, so every column belongs to one place
   on the boundary - the skirt's licks live at the base, the needles at the
   crown - and a continuous scroll parades each around the whole ring.
   Swaying keeps every lick in its own neighbourhood. Two incommensurate
   sines so the motion does not tick like a metronome; both vanish at
   u_Time 0, where the measurement harness compares against the art. */
#define OSC_SPAN 0.02
#define OSC_RATE 4.0


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

	}

	vec2 boxCentre = (boxMin + boxMax) * 0.5;
	vec2 boxHalf   = (boxMax - boxMin) * 0.5 + vec2(padding);

	/* --- where this vertex sits ---------------------------------------- */

	/* Direction rides the normal, not the colour bytes: a byte's 1/127 step
	   is coarser than the segment spacing at high segment counts, and the
	   aliased directions put one segment's radius on its neighbour's angle. */
	float isOuter = gl_Color.b;
	vec2  dir     = normalize(gl_Normal.xy + vec2(FORCE_EPSILON));

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

	/* The mesh carries the reference outline: gl_Normal.z is the boundary
	   radius at this vertex's authored angle, crown-normalised, read off the
	   reference art's alpha mask by make_aura_mesh.py - a float, because the
	   colour byte it once rode in stepped the needle tips visibly. The
	   silhouette is the reference's own - every tongue of it - rather than
	   any width function's approximation. The tip is authored at +Y, so the
	   directional terms below work in the authored frame, and the finished
	   shape is rotated rigidly onto the flow at the end. */
	float rRef = gl_Normal.z;

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

	/* --- back into the world -------------------------------------------- */

	/* The shape is authored in NDC, but a sheet needs honest depths: behind
	   the character it must lose their pixels, lying across the ground it
	   must win the floor's, and anything genuinely between camera and flame
	   must still occlude it. So each vertex goes back into model space along
	   its own view ray - onto the view-facing plane through the player, or
	   onto the ground plane at the feet where that is nearer - and then
	   reprojects. The ray is the same, so the screen position is unchanged
	   by construction; only the depth becomes real. */
	float pa  = u_ProjectionMatrix[0][0];
	float pb  = u_ProjectionMatrix[1][1];
	float p22 = u_ProjectionMatrix[2][2];
	float p32 = u_ProjectionMatrix[3][2];

	/* MV = P^-1 * MVP, with the perspective inverse written out. */
	mat4 invP  = mat4(0.0);
	invP[0][0] = 1.0 / pa;
	invP[1][1] = 1.0 / pb;
	invP[3][2] = -1.0;
	invP[2][3] = 1.0 / p32;
	invP[3][3] = p22 / p32;
	mat4 mv     = invP * u_ModelViewProjectionMatrix;
	mat3 rot    = mat3(mv);
	vec3 trans  = mv[3].xyz;
	vec3 camPos = -(trans * rot);   /* vec * mat multiplies by the transpose */

	/* Eye-space distance of the plane the sheet stands on. */
	float dRef = max(-(mv * vec4((boxMins + boxMaxs) * 0.5, 1.0)).z, FORCE_EPSILON);

	/* This pixel's point on that plane, back in model space. */
	vec3 eyePt   = vec3(pos.x * dRef / pa, pos.y * dRef / pb, -dRef);
	vec3 sheetPt = (eyePt - trans) * rot;

	/* Where the same ray crosses the feet plane, held GROUND_LIFT above it.
	   While that crossing is in front of the sheet the flame lies down onto
	   it; past the fold line it stands up the player's plane. The camera has
	   to be above the plane, or a low camera would fold the crown onto it. */
	vec3  rayDir  = sheetPt - camPos;
	float groundZ = boxMins.z + GROUND_LIFT;
	float rayFall = abs(rayDir.z) > FORCE_EPSILON ? rayDir.z : -FORCE_EPSILON;
	float hit     = (groundZ - camPos.z) / rayFall;
	if (camPos.z > groundZ && hit > 0.0 && hit < 1.0) {
		sheetPt = camPos + rayDir * hit;
	}

	gl_Position = u_ModelViewProjectionMatrix * vec4(sheetPt, 1.0);

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
	   of the strip: no wrap count. The field sways rather than rotates:
	   scrollSpeed drives the sway rate, OSC_SPAN bounds its reach, so the
	   configured cvar still means "how alive is this aura". */
	float phase = u_Time * scrollSpeed * OSC_RATE;
	float u = gl_MultiTexCoord0.x
	        + OSC_SPAN * (sin(phase) + 0.5 * sin(2.7 * phase))
	          * gl_MultiTexCoord0.y;

	/* The art is a flat drawing: its field is authored against the screen,
	   so the strip must interpolate in screen space even across the quads
	   the ground fold tilts into the world - perspective-correct
	   interpolation there slides the field off its authoring. Scaling the
	   coordinate by clip w and dividing it back out per fragment cancels
	   the hardware's 1/w exactly. */
	float wClip = gl_Position.w;

	/* The ring's quads are trapezoids - the inner edge is shorter than the
	   outer, and zero-length where the fan meets the centre - so an affine
	   u across each triangle kinks at every quad boundary and draws faint
	   radial hairlines through the interior. Riding u on a q proportional
	   to the vertex's own fan radius makes the interpolation projective:
	   the coordinate fans angularly inside each wedge, and neighbouring
	   wedges agree along their shared edges. t stays on plain w in the
	   second set - dividing it by the radius would collapse it at the
	   centre. */
	float fanQ = max(rr, FORCE_EPSILON);

	/* Sheared with distance out, so every lick leans. The same sign on both
	   copies: the mirrored one already reverses U, so an equal shear comes out
	   leaning the opposite way on the far side of the aura - which is what is
	   wanted, since both sides should sweep toward the tip rather than all
	   leaning the one way around the ring like a vortex. */
	/* No shear and no mirrored copy: the strip is the reference's own
	   field, its licks already lean in the art, and anything added on top
	   moves the material off the geometry that carries its outline. */
	gl_TexCoord[0] = vec4( u * wClip * fanQ, 0.0, 0.0, wClip * fanQ);
	gl_TexCoord[1] = vec4( 0.0, gl_MultiTexCoord0.y * amplitude * wClip, 0.0, wClip);

	v_edge = gl_MultiTexCoord0.y;
	v_base = base;

	/* Vertex colour is payload, not colour - do not pass it to the fragment
	   stage. The aura's tint comes from the entity colour instead. */
	gl_FrontColor = gl_BackColor = vec4(1.0);
}
