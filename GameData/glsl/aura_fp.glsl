#version 120
/*
 * aura_fp.glsl
 * screen-space aura fragment program
 *
 * The flame is computed here, not sampled. A texture strip was tried and could
 * not reach the reference art: a strip is a height field along the band, so a
 * tongue can never be wider than its base, its interiors arrive semi-opaque
 * from however the art was filtered, and every level of detail question turns
 * into a mip question. What the reference actually shows is three separable
 * things - an opaque white body, a rim broken into bold tongues, and hair-fine
 * strands leaving the tongue edges - and each of those is cheaper to state as
 * a function of ring position than to paint.
 *
 * The vertex stage still emits the coordinate twice, once scrolling each way,
 * because the flame has to flow toward the tip on both sides and the direction
 * must reverse somewhere. Crossfading the two evaluations hides that seam.
 */

uniform vec4 u_EntityColor;
uniform float u_Time;

varying float v_seamBlend;
varying float v_edge;
varying float v_base;
/* Wraps of the pattern around the ring, from the vertex stage. The noise
   lattice is hashed modulo its cell count per ring, and that count is
   frequency * wraps: without it the lattice does not close where the ring
   does, and the flame tears open along one radius. */
varying float v_wraps;

/* Tongue cells per wrap. One tongue per cell, so at the default two wraps
   the aura carries about twice this many tongues - which is what the
   reference carries around its full turn. */
#define TONGUE_FREQ  24.0

/* Strand cells per wrap. Hair is the highest frequency in the reference by
   an order of magnitude - it has to stay far above the tongue count or the
   strands read as more tongues rather than as texture on them. */
#define HAIR_FREQ    479.0

/* The two warp fields, in cells per wrap. CLUSTER_FREQ shifts tongues
   sideways so they bunch and spread instead of marching evenly; LEAN_FREQ
   varies how hard each region's tongues lean as they rise. Both are the
   domain-warping idea: displace the coordinate with one noise before
   evaluating another, which is what turns a lattice into something organic. */
#define CLUSTER_FREQ 3.0
#define LEAN_FREQ    5.0

/* Hair length clusters, in cells per wrap: a slow mask over the needle
   lengths so the hair comes in brushy patches rather than uniform fuzz. */
#define MED_FREQ     37.0

/* Where the solid body ends and the tongue zone begins, as a fraction of the
   band. Kept thin: an unbroken white expanse is the one thing the flame must
   not have, so almost the whole band belongs to the tongues and the clefts
   between them, and the solid ribbon only seals the ring at the body. */
#define TONGUE_ROOT  0.10

/* Deepest a gap between tongues can cut into the tongue zone, and the
   ceiling the tallest tongue body reaches; the wisps carry on above it. The
   floor keeps the ring visibly closed - a gap that reaches the body breaks
   the silhouette into petals. */
#define TONGUE_FLOOR 0.05
#define TONGUE_CEIL  0.78

/* Tint constants, unchanged from the sampled version: the tips deepen into
   the character's own colour, the core runs hotter than the body, and the
   base carries an extra glow where the ki meets the ground. */
#define TIP_START 0.35
#define TIP_SHIFT 0.85
#define CORE_END  0.20
#define CORE_SHIFT 0.25
#define CORE_GLOW 1.35
#define GLOW_END  0.45
#define BASE_GLOW 1.5

/* Not the folkloric fract(sin(x) * 43758.5453): that one needs sin to be
   accurate at arguments in the tens of thousands, and on this hardware it is
   not - the hash flattens and the whole flame comes out as gentle waves.
   Every intermediate here stays small enough to keep its precision. */
float vhash( float i, float period ){
	i = mod( i, period);
	i = fract( i * 0.1031);
	i *= i + 33.33;
	i *= i + i;
	return fract( i);
}

/* One-dimensional value noise on an integer lattice that wraps at `period`
   cells, so the pattern closes exactly where the ring does. */
float vnoise( float x, float period ){
	float i = floor(x);
	float f = x - i;
	f = f * f * (3.0 - 2.0 * f);
	return mix( vhash( i, period), vhash( i + 1.0, period), f);
}

/* Coverage of the flame at ring position u (in wraps) and band position t
   (0 at the inner ring, 1 at the spike tips). */
float flame( float u, float t ){
	float cells = max( v_wraps, 1.0);
	float P  = TONGUE_FREQ * cells;
	float HP = HAIR_FREQ * cells;

	/* Domain warp, before any lattice is consulted: a slow field bunches
	   the tongues sideways so they cluster instead of marching evenly, and
	   a second slow field leans each region's tongues as they rise. The
	   lean term grows with x, which is what lets a tongue overhang its own
	   base - the one thing a height field can never do. */
	float xo   = max( (t - TONGUE_ROOT) / (1.0 - TONGUE_ROOT), 0.0);
	float dw   = (vnoise( u * CLUSTER_FREQ, CLUSTER_FREQ * cells) - 0.5) * 2.4;
	float lean = (vnoise( u * LEAN_FREQ + 3.1, LEAN_FREQ * cells) - 0.5) * 2.2;
	float warp = dw + lean * xo;

	/* One triangular tongue per cell, its peak height and position drawn
	   from the cell's hash. A triangle rather than smooth noise because
	   that is the shape the reference draws: straight-sided tongues meeting
	   in sharp clefts, not rolling waves. */
	float s    = u * TONGUE_FREQ + warp;
	float cell = floor(s);
	float f    = s - cell;
	float hgt  = 0.35 + 0.65 * vhash( cell, P);
	float ctr  = 0.30 + 0.40 * vhash( cell + 13.0, P);
	float tri  = f < ctr ? f / ctr : (1.0 - f) / (1.0 - ctr);
	float n    = hgt * pow( tri, 0.75);

	/* A second tongue layer at half the frequency and offset phase, taken
	   as a max. One tongue per cell alone reads as a picket fence - every
	   cleft reaching the same depth at the same spacing; the overlap breaks
	   the metre. */
	float s2   = s * 0.5 + 0.37;
	float c2   = floor(s2);
	float f2   = s2 - c2;
	float hgt2 = 0.5 + 0.5 * vhash( c2 + 101.0, P * 0.5);
	float tri2 = pow( 1.0 - abs( f2 - 0.5) * 2.0, 1.6);
	n = max( n, 0.85 * hgt2 * tri2);
	n = TONGUE_FLOOR + (TONGUE_CEIL - TONGUE_FLOOR) * clamp( n, 0.0, 1.0);

	/* Position across the tongue zone; negative is inside the solid body. */
	float x = (t - TONGUE_ROOT) / (1.0 - TONGUE_ROOT);

	/* Hair as discrete needles: one per strand cell. The length is the
	   cell's raw hash rather than smoothed noise - neighbouring needles
	   have to disagree, or the fringe blurs into a soft gradient - and the
	   thin triangular profile is what makes each one a distinct filament.
	   The needle coordinate carries the same warp as the tongues, scaled
	   to its own frequency, so every hair stays parallel to the tongue it
	   belongs to. */
	float sh   = u * HAIR_FREQ + warp * (HAIR_FREQ / TONGUE_FREQ);
	float hc   = floor(sh);
	float hf   = sh - hc;
	float hlen = vhash( hc + 29.0, HP);
	float med  = vnoise( u * MED_FREQ + 11.0, MED_FREQ * cells);
	hlen *= 0.35 + 0.65 * med;
	float prof = smoothstep( 0.20, 0.55, 1.0 - abs( hf - 0.5) * 2.0);

	/* The tongue body: opaque inside the boundary, its edge serrated by
	   the needles' roots. The solidity is the point - the reference's
	   flame covers what is behind it, and a translucent band reads as
	   fog, not ki. */
	float nh   = n + 0.09 * hlen * prof;
	float body = 1.0 - smoothstep( nh - 0.015, nh + 0.015, x);

	/* Rim striations: thin dark lines between strands near the edge,
	   where the hair runs down into the tongue it belongs to; without
	   them the needles look glued on. */
	float rim = smoothstep( nh - 0.25, nh, x);
	body *= 1.0 - 0.30 * rim * (1.0 - prof * smoothstep( 0.2, 0.8, hlen));

	/* Needles past the edge, fading over their own reach. */
	float reach = (0.05 + 0.55 * hlen * hlen) * (0.30 + 0.70 * n);
	float over  = (x - nh) / max( reach, 0.001);
	float wisp  = prof * smoothstep( 0.25, 0.75, hlen)
	            * pow( clamp( 1.0 - over, 0.0, 1.0), 1.2);
	wisp *= step( nh, x);

	/* The flame is a veil, not a wall: thin toward the body and swelling
	   to full strength approaching the rim, so the scene ghosts through
	   the interior and the tongues carry the brightness. The reference
	   in situ shows rock through the crown's heart. */
	float depth = mix( 0.55, 1.0, smoothstep( nh - 0.55, nh - 0.05, x));

	/* The inner rows fade in rather than starting solid: the inner ring is
	   a closed loop of geometry, and any coverage on it draws that loop as
	   a hard oval over the character. */
	return max( body, wisp) * depth * smoothstep( 0.0, 0.06, t);
}

void main(void) {
	float forward  = flame( gl_TexCoord[0].s, gl_TexCoord[0].t);
	float mirrored = flame( gl_TexCoord[1].s, gl_TexCoord[1].t);
	float alpha    = mix( mirrored, forward, v_seamBlend) * u_EntityColor.a;

	/* The tint pipeline is unchanged from the sampled version. The tips run
	   cool by deepening into the character's own colour; a fixed target is a
	   different hue for every character. */
	float level = max( max( u_EntityColor.r, u_EntityColor.g), u_EntityColor.b);
	vec3  deep  = u_EntityColor.rgb * u_EntityColor.rgb / max( level, 0.0001);
	vec3  cool  = mix( u_EntityColor.rgb, deep, TIP_SHIFT);
	vec3  tint  = mix( u_EntityColor.rgb, cool, smoothstep( TIP_START, 1.0, v_edge));

	vec3  hot   = mix( u_EntityColor.rgb, vec3(level), CORE_SHIFT);
	tint = mix( hot, tint, smoothstep( 0.0, CORE_END, v_edge));

	/* Premultiplied output against GL_ONE / GL_ONE_MINUS_SRC_ALPHA: at unity
	   this is alpha-over and the flame holds its colour against any sky;
	   pushed past unity the excess adds, so the core glows into the scene. */
	float boost = mix( CORE_GLOW, 1.0, smoothstep( 0.0, GLOW_END, v_edge));
	boost += BASE_GLOW * v_base * (1.0 - smoothstep( 0.0, 0.7, v_edge));

	gl_FragColor = vec4( tint * alpha * boost, alpha);
}
