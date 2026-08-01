/* Generated variant for the zeq2clip.sh A/B loop - see
   Tools/dev/aura_variants/README.md. The defaults in GameData/glsl are
   authoritative; b-sway matches them by construction. */
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

uniform sampler2D u_Texture0;
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

/* The strand grain comes from the strip texture auragen.c bakes: one field
   evaluated in the same (u, t) domain this stage works in, with the soft
   radial-blur quality per-fragment noise cannot afford. The macro tongues
   live in the mesh - the outline baked from the reference - so the strip's
   longest strands just touch t = 1 and the geometry stays the silhouette. */

/* Tint constants, unchanged from the sampled version: the tips deepen into
   the character's own colour, the core runs hotter than the body, and the
   base carries an extra glow where the ki meets the ground. */
#define TIP_START 0.35
#define TIP_SHIFT 0.85
#define CORE_END  0.20
#define CORE_SHIFT 0.25
#define CORE_GLOW 1.0
#define GLOW_END  0.45
#define BASE_GLOW 0.0



/* The strip is a flipbook: STRIP_FRAMES bands stacked vertically, frame 0
   the reference exactly, the rest lick-jittered variants of it. Hard cuts
   between them are the anime's own animation - two or three drawings of the
   same flame alternating - and at time zero frame 0 shows, which is the
   frame the measurement harness compares against the art. Must match
   --frames in aura_band_from_reference.py.

   FLICKER_FPS 0 holds frame 0: the shipped animation is the vertex stage's
   sway alone, which the A/B clips read as the calmer of the two. The strip
   still carries all four frames so the flipbook variant
   (Tools/dev/aura_variants/c-flipbook.fp.glsl) is a pure shader swap. */
#define STRIP_FRAMES 4.0
#define FLICKER_FPS  0.0
/* Rows per frame, for the half-texel inset that keeps bilinear filtering
   from bleeding one frame's tips into the next frame's body. */
#define FRAME_ROWS   512.0

/* Coverage of the flame at ring position u (already in strip S units) and
   band position t (0 at the inner ring, 1 at the reference outline). */
float flame( float u, float t ){
	/* The strip is the reference's own band, unwrapped over exactly one
	   turn, so it tiles by construction and is sampled straight. */
	float frame = mod( floor( u_Time * FLICKER_FPS), STRIP_FRAMES);
	/* Identity in the interior - rescaling by (rows-1)/rows squeezed the
	   whole field measurably - clamped only within the half texel at each
	   frame edge that bilinear would blend into the neighbouring frame. */
	float tIn   = clamp( t, 0.5 / FRAME_ROWS, 1.0 - 0.5 / FRAME_ROWS);
	float strand = texture2D( u_Texture0, vec2( u, (frame + tIn) / STRIP_FRAMES)).a;

	/* No mist, no boost, and no inner fade on top: the strip already
	   carries the reference's interior glow and its hot rim - anything
	   added here is a departure from the art. The fade guarded the inner
	   ring back when it was a visible loop; at INNER_HUG 0 the ring is a
	   point and the fade only dug a dark dip the reference does not have. */
	return strand;
}



void main(void) {
	/* The vertex stage scales both coordinates by clip w so they interpolate
	   in screen space across the ground fold, and u additionally by the fan
	   radius so it fans projectively through each wedge instead of kinking
	   at quad boundaries. Dividing by each set's own q restores them. */
	float uu = gl_TexCoord[0].s / max( gl_TexCoord[0].q, 0.0001);
	float tt = gl_TexCoord[1].t / max( gl_TexCoord[1].q, 0.0001);

	float alpha = flame( uu, tt) * u_EntityColor.a;

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
