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
   right. The strip is 256 texels wide, so `wavelength` wraps of it lay
   256 * wavelength texels around the ring; at the stock four wraps that is a
   thousand texels, which is about the ring's perimeter in pixels when the
   player's half-height covers a third of a 960-tall window. Anything smaller
   is undersampling the strip, which is where the crawling starts. */
#define SPIKE_REF 0.34

varying float v_seamBlend;
/* 0 on the inner ring, 1 at the spike tips. Carried separately from the
   texture coordinate because that one has amplitude folded into it, and the
   tip tint has to key off the ring itself rather than off how far the strip
   has been stretched. */
varying float v_edge;
/* Extra mip level for the fragment stage, in LOD units. */
varying float v_texBias;

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

	/* +1 at the tip, -1 at the flattened base. Drives both the directional
	   stretch and how far the outer ring is pushed out. */
	float along = dot(dir, flowDir);

	/* Split rather than used raw: the tip and the base want opposite
	   treatments, and a single signed term gives them the same one. */
	float tip  = max(along, 0.0);

	/* Squared so the base tucks in with zero slope where it meets the flanks.
	   A bare max() creases the silhouette at the ring's widest point. */
	float base = max(-along, 0.0);
	base *= base;

	/* --- build the position --------------------------------------------- */

	/* Inner ring hugs the bounding box, drawn in under the player toward the
	   base. Without the tuck the ring is an egg rather than a tear-drop, and
	   on a player who fills the screen its lowest point sits far enough below
	   the feet to fall off the bottom of the frame - so the base closes
	   somewhere nobody can see and the aura reads as two loose flanks. */
	vec2 pos = boxCentre + dir * boxHalf * originDist * (1.0 - 0.45 * base);

	/* Spikes need a length that does not depend on which way they point.
	   Scaling by boxHalf per axis - the obvious thing to write - gives sideways
	   spikes the player's narrow width and upward ones their full height, so
	   the ring reads as two bright vertical bands with nothing top or bottom.
	   One scalar length, aspect-corrected, keeps them even all the way round.

	   NDC spans -1..1 on both axes regardless of window shape, so an equal
	   offset in x and y is not equal on screen. The projection matrix carries
	   the ratio: [1][1]/[0][0] is width/height. */
	float spikeLen = max(boxHalf.x, boxHalf.y);
	float aspect   = u_ProjectionMatrix[1][1] / u_ProjectionMatrix[0][0];
	vec2  evenly   = vec2(1.0 / max(aspect, FORCE_EPSILON), 1.0);

	/* Draw the tip out along the flow. Scaling this by the signed `along` -
	   which looks like the same thing - stretches the base just as hard in the
	   opposite direction, so the aura grows downward out of frame exactly as
	   fast as it grows upward and never closes anywhere visible. */
	pos += flowDir * evenly * spikeLen * strength * 0.45 * tip;

	/* The outer ring carries the spikes: a real skirt everywhere so the ring
	   closes visibly, growing toward the tip. Only the outer vertices move. */
	float spike = strength * (0.35 + 0.45 * tip);
	pos += dir * evenly * spikeLen * spike * isOuter;

	gl_Position.xy = pos;
	gl_Position.w  = 1.0;

	/* --- texturing ------------------------------------------------------ */

	/* Which side of the force axis this vertex is on decides which way the
	   texture scrolls, so that it always flows toward the tip. */
	vec2  perp = vec2(-forceDir.y, forceDir.x);
	float side = dot(dir, perp);

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
	   the ring agrees on the answer. */
	float shrink = min(log2(max(spikeLen, FORCE_EPSILON) / SPIKE_REF), 0.0);
	float steps  = -floor(shrink);
	float wraps  = max(floor(wavelength * exp2(-steps)), 1.0);

	/* Once the count bottoms out at a single wrap there is nothing left to
	   halve, so the rest of the shrink is handed to the sampler as a mip
	   bias. This also covers the moment the count steps: dropping a wrap
	   doubles the texel footprint, so the bias falls by the same amount and
	   the apparent sharpness stays put instead of popping. */
	v_texBias = max(log2(wraps / max(wavelength, FORCE_EPSILON)) - shrink, 0.0);

	float u = gl_MultiTexCoord0.x * wraps + u_Time * scrollSpeed;
	gl_TexCoord[0] = vec4(u, gl_MultiTexCoord0.y * amplitude, 0.0, 1.0);
	/* The mirrored copy; the fragment stage crossfades between the two. */
	gl_TexCoord[1] = vec4(-u, gl_MultiTexCoord0.y * amplitude, 0.0, 1.0);

	v_edge = gl_MultiTexCoord0.y;

	/* Vertex colour is payload, not colour - do not pass it to the fragment
	   stage. The aura's tint comes from the entity colour instead. */
	gl_FrontColor = gl_BackColor = vec4(1.0);
}
