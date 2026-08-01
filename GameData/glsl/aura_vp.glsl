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

/* How far the flank skirt is measured off the character's height rather than
   their width; 0 is pure width, 1 is the height-everywhere sizing this
   replaced.

   Deliberately well up the range rather than at the bottom of it. Pure width
   is the honest proportion and it does keep the aura fitted, but it also
   leaves the flanks thin enough that the whole effect reads as a sheath drawn
   round the character instead of as ki coming off them - the flame needs mass
   at the sides to look like it has any volume at all. This keeps the sizing
   tied to the body, so a broad character still gets a broader aura, while
   leaving enough girth for the licks to be legible. */
#define FLANK_GIRTH 0.85

/* How far the lower half's licks are reflected from pointing down to pointing
   up. Ki does not hang below a character like a hem - it gathers under them
   and is thrown back up, so the flame at the base sweeps up and out into a V
   rather than draping down. 1 mirrors it outright. */
#define BASE_SWEEP 0.25

/* How far every lick is biased toward the flow, on top of the base mirror. Ki
   rises: the reference frames show tongues climbing more or less vertically
   even out at the flanks, where a purely radial spike would be pointing
   sideways at the camera. 0 leaves each lick on the ring's own radius, 1 aims
   the whole aura straight up. */
#define RISE 0.35

/* How far the strip is sheared along U as it runs outward, in strip repeats.
   The licks in the reference do not stand perpendicular to the edge they leave
   - every one of them leans the same way, hard, so the flame reads as being
   swept rather than as bristling. The strip cannot express that in its own
   art: a lick that leans has to overhang its own base, and the silhouette is
   built as a height field, which by definition cannot. Shearing the texture
   coordinate does it instead, and costs nothing. */
#define SHEAR 0.11

/* Width against height: w = h^V_OPEN * (1 - h^V_CLOSE)^V_TAPER, scaled by
   V_NORM so the peak is 1. Fitted numerically to the reference art's
   measured silhouette, which is a low-bulged teardrop: widest around 35%%
   of the way up, tapering hard above 55%% and closing to a point. The old
   exponents made an egg - bulge at half height, shoulders still wide at
   70%% - and no amount of texture reads right on the wrong outline.
   Recompute V_NORM if any exponent changes. */
#define V_OPEN  0.54
#define V_CLOSE 1.2
#define V_TAPER 1.1
#define V_NORM  2.544

/* How far below the box the V's apex is dropped, as a fraction of the box
   half-height. The arms need somewhere to open from: with the apex exactly at
   the soles they are still nearly closed by the time they reach the calves,
   and the legs sit outside the flame. */
#define APEX_DROP 0.22

/* Peak radial deviation of the ring, as a fraction of the box half-extent.
   This is what stops the aura's inner boundary being a perfect ellipse - the
   ring is a unit circle mapped onto a box, so without it every character's
   flame has the same geometric outline and the texture is left doing all the
   work of not looking like one. Large enough to break the curve, small enough
   that the crown still clears the hair and the base still reaches the feet. */
#define WOBBLE 0.075

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

	float crown = tip * tip;

	/* Split the ring's own offset into the part along the flow and the part
	   across it, because everything that pulls the ring inward should act on
	   one and not the other.

	   originDist used to scale the whole offset, and the two axes want
	   opposite things from it. Across the flow it has to come in far enough
	   that the ring's inner edge passes *behind* the player - an ellipse is
	   never going to follow a human silhouette, so the only way that edge
	   stops reading as a bare oval hung around the character is for their own
	   body to cover it. Along the flow the ring wants the opposite: the ends
	   of the box are the top of the hair and the soles of the feet, and any
	   shrink at all lifts the base off the boots and drops the crown below the
	   hair. Shrinking both together is why no single value ever looked right -
	   tight enough to hide the oval left the feet outside, and low enough to
	   reach the feet put the oval back.

	   So the flow axis stays pinned to the box, and originDist, the base tuck
	   and the crown taper all act across it. originDist now means exactly one
	   thing: how tightly the flanks hug. */
	vec2 alongVec = flowDir * along;
	vec2 perpVec  = dir - alongVec;

	/* How far up the aura this vertex is: 0 at the base apex, 1 at the crown. */
	float height = 0.5 * (along + 1.0);

	/* The ring's own circle gives a perpendicular extent of sqrt(1 - along^2),
	   which is an ellipse - widest at its waist and curving back in above it,
	   symmetrically. That is the shape being replaced, so only the direction
	   is kept here and the magnitude comes from the profile below.

	   An ellipse is why the aura read as an oval sitting on top of a V rather
	   than as one shape: the V's arms rose out of the base, met the ellipse's
	   waist, and stopped there because that is where the ellipse stops
	   widening. Driving the width off height instead lets the arms keep
	   opening the whole way, so the silhouette is a single V that narrows only
	   where the crown has to close. */
	vec2 perpUnit = perpVec / max(length(perpVec), FORCE_EPSILON);

	float width = pow(height, V_OPEN)
	            * pow(1.0 - pow(height, V_CLOSE), V_TAPER)
	            * V_NORM;

	/* originDist scales the whole profile and nothing else. It used to be
	   opened out at the base by a separate term keyed on `base`, to clear the
	   legs - but `base` collapses a short way up, so that widened the very
	   bottom and then let the width fall back immediately above it. The result
	   was a bulge at the feet with a pinch over it: the aura got *narrower*
	   going up before widening again, which breaks the V outright. Anything
	   that widens one band and not its neighbours will do the same.

	   The leg clearance comes from V_OPEN instead, which is a property of the
	   whole profile, so the width can only grow on the way up. */
	float spread = originDist;

	/* Dropping the apex below the box is what gives the arms room to open
	   before they reach the legs. It only moves the bottom pole - `base`
	   collapses away from it - so the crown and flanks stay pinned. */
	vec2 shaped = flowDir * along * (1.0 + APEX_DROP * base)
	            + perpUnit * width * spread;

	/* Push the ring in and out around its circumference. An ellipse is the one
	   shape ki never has, and it is the shape this technique produces for
	   free: mapping a circle onto the bounding box gives a curve with no
	   feature anywhere on it, so the aura's inner boundary reads as geometry
	   however good the texture on top of it is.

	   Three harmonics rather than one, at rates that do not divide each other,
	   so the sum never settles into a recognisable lobed shape - one term
	   alone is just an ellipse with bumps, and the eye finds the period
	   immediately. Integer multiples of the angle, though, and that is not
	   optional: the ring closes where theta wraps, and a non-integer harmonic
	   leaves a step there that tears the aura open along one radius.

	   Driven by scrollSpeed so the boundary crawls at the same rate the flame
	   on it does. A static perturbation is worse than none - it reads as a
	   dented ring rather than as something burning. */
	float theta  = atan(dir.y, dir.x);
	float wobble = sin(theta * 3.0 + u_Time * scrollSpeed * 1.7)
	             + 0.6 * sin(theta * 5.0 - u_Time * scrollSpeed * 2.3)
	             + 0.4 * sin(theta * 8.0 + u_Time * scrollSpeed * 1.1);

	vec2 pos = boxCentre + shaped * boxHalf * (1.0 + WOBBLE * wobble);

	/* Spikes need a length that does not depend on which way they point.
	   Scaling by boxHalf per axis - the obvious thing to write - gives sideways
	   spikes the player's narrow width and upward ones their full height, so
	   the ring reads as two bright vertical bands with nothing top or bottom.
	   One scalar length, aspect-corrected, keeps them even all the way round.

	   NDC spans -1..1 on both axes regardless of window shape, so an equal
	   offset in x and y is not equal on screen. The projection matrix carries
	   the ratio: [1][1]/[0][0] is width/height. */
	float aspect   = u_ProjectionMatrix[1][1] / u_ProjectionMatrix[0][0];
	vec2  evenly   = vec2(1.0 / max(aspect, FORCE_EPSILON), 1.0);

	/* One scalar, but not the same one all the way round. Taking the larger
	   extent everywhere - which is what "even spikes" seemed to require - sizes
	   the *sideways* skirt off the character's height, and a standing character
	   is roughly three times as tall as they are wide. The flanks therefore
	   came out about as long as the player's half-height, which put the aura's
	   silhouette at over twice the width of the body it belongs to. It read as
	   a blob the character was standing inside rather than as their own ki.

	   So: the flanks measure against the body's width, the crown against its
	   height, blended along `crown` so the length still varies smoothly around
	   the ring and no direction gets a visible step. The two extents have to be
	   compared in the same units first - NDC spans -1..1 on both axes whatever
	   the window shape, so a half-width in NDC is not comparable to a
	   half-height until the aspect ratio is folded in. */
	float halfWidth  = boxHalf.x * aspect;
	float halfHeight = boxHalf.y;
	float girth      = min(halfWidth, halfHeight);
	float reach      = max(halfWidth, halfHeight);

	/* Not the bare width: a skirt that narrow leaves the flanks a hairline on
	   a thin character, and the ring stops reading as closed. */
	float spikeLen = mix(mix(girth, reach, FLANK_GIRTH), reach, crown);

	/* Draw the tip out along the flow. Scaling this by the signed `along` -
	   which looks like the same thing - stretches the base just as hard in the
	   opposite direction, so the aura grows downward out of frame exactly as
	   fast as it grows upward and never closes anywhere visible.

	   Both this and the crown's share of the skirt below are measured against
	   the character's half-height, so they compound: between them they were
	   putting most of a character's height of flame above the head. That reads
	   as a pillar the character happens to be standing at the bottom of rather
	   than as their own aura, and it is the crown alone that has to come down -
	   the flanks are already sized off the body's width and are fine. */
	pos += flowDir * evenly * spikeLen * strength * 0.34 * tip;

	/* The outer ring carries the spikes: a real skirt everywhere so the ring
	   closes visibly, growing toward the tip. Only the outer vertices move.

	   Weighted on `crown` rather than on `tip` so the growth is concentrated
	   at the top instead of spread evenly around the ring. Spread evenly, the
	   flanks carry nearly as much skirt as the crown and the aura reads as an
	   even fur; a flame puts almost all of its length in one direction. The
	   floor is what keeps the ring visibly closed at the sides and base.

	   The floor carries most of the weight now and the crown's share is small,
	   which looks backwards but is not. These two are the only ways to add
	   flame, and raising `strength` instead - the obvious lever, and the one
	   auraScale pulls - moves both at once: the crown term is twice the floor,
	   so scaling up grows the plume far faster than the flanks and the aura
	   gets taller when what it needed was to get thicker. Loading the floor
	   adds mass at the sides and leaves the plume where it was.

	   The base gets the same tuck the inner ring does. Without it the inner
	   ring draws in under the feet while the outer one does not, so the skirt
	   *widens* exactly where the aura should be gathering, and the effect
	   stands the character in a puddle rather than lifting off them. */
	float spike = strength * (0.55 + 0.17 * crown) * (1.0 - 0.15 * base);

	/* Spikes leave along the ring's own radius everywhere except the base,
	   where they are turned back up the flow. Left radial, the licks directly
	   under the character point at the floor, and the aura ends in a fringe
	   hanging off the soles - the one place the reference never puts flame.
	   What it shows instead is a bright point beneath the character with the
	   flame sweeping up and outward either side of it, like something struck
	   the ground there.

	   Two halves to that. This turns the direction; the far heavier base
	   falloff above closes the length, so the very bottom of the ring carries
	   almost no skirt at all and becomes the point of the V. Without the
	   falloff the turn alone would aim a full-length spike straight up through
	   the character's legs.
	   Mirrored rather than rotated toward the flow. Rotating only turns the
	   licks already pointing nearly straight down, because `base` is squared
	   and collapses either side of the bottom - the lower flanks kept pointing
	   sideways and the V never closed. Reflecting the downward component turns
	   the entire lower half at once, and because each lick keeps its sideways
	   component untouched the two wings splay outward instead of collapsing
	   into one upward jet. */
	vec2 across = dir - flowDir * along;

	/* Two stages. The mirror turns the lower half up; the rise then leans the
	   whole ring toward the flow on top of it, which is what stops the flanks
	   throwing their licks straight out sideways. Radially, a lick at the
	   waist points at the camera's left or right - correct for a ring, wrong
	   for fire, which climbs whatever part of the body it is leaving. */
	float lift = mix(along, abs(along), BASE_SWEEP);
	lift = mix(lift, 1.0, RISE);

	vec2 swept = across + flowDir * lift;

	vec2 spikeDir = normalize(swept + vec2(FORCE_EPSILON));

	pos += spikeDir * evenly * spikeLen * spike * isOuter;

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
	   the ring agrees on the answer.

	   `reach`, deliberately, and not `spikeLen`: spikeLen is blended along the
	   ring so the flanks can be sized off the body's width, which makes it a
	   per-vertex quantity. Feeding a per-vertex value into a wrap count that
	   has to come out identical for every vertex is exactly the fractional
	   mismatch this is written to avoid, and it would draw the seam it is
	   trying to prevent. */
	float shrink = min(log2(max(reach, FORCE_EPSILON) / SPIKE_REF), 0.0);
	float steps  = -floor(shrink);
	float wraps  = max(floor(wavelength * exp2(-steps)), 1.0);

	v_wraps = wraps;

	/* Not the authored coordinate, which advances uniformly with ring angle.
	   The aura is tall: at the flanks the ring covers screen distance at the
	   rate of the box's half-height, at the crown only its half-width, so an
	   angle-uniform pattern stretches its flank tongues by that ratio and
	   they smear into sheets. Advancing u with atan2 of the direction scaled
	   by the square roots of the two extents makes du/dtheta proportional to
	   the local arc rate at both poles, which is where it matters; between
	   them it is an approximation nobody can see. The result still spans one
	   full turn, so the periodic lattice closes exactly as before. */
	float arc = atan( sqrt(halfHeight) * dir.y, sqrt(halfWidth) * dir.x)
	          * 0.15915494 + 0.5;
	float u = arc * wraps + u_Time * scrollSpeed;

	/* Sheared with distance out, so every lick leans. The same sign on both
	   copies: the mirrored one already reverses U, so an equal shear comes out
	   leaning the opposite way on the far side of the aura - which is what is
	   wanted, since both sides should sweep toward the tip rather than all
	   leaning the one way around the ring like a vortex. */
	float shear = SHEAR * gl_MultiTexCoord0.y;

	gl_TexCoord[0] = vec4( u + shear, gl_MultiTexCoord0.y * amplitude, 0.0, 1.0);
	/* The mirrored copy; the fragment stage crossfades between the two. */
	gl_TexCoord[1] = vec4(-u + shear, gl_MultiTexCoord0.y * amplitude, 0.0, 1.0);

	v_edge = gl_MultiTexCoord0.y;
	v_base = base;

	/* Vertex colour is payload, not colour - do not pass it to the fragment
	   stage. The aura's tint comes from the entity colour instead. */
	gl_FrontColor = gl_BackColor = vec4(1.0);
}
