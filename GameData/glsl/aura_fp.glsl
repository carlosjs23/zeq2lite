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



/* Coverage of the flame at ring position u (already in strip S units) and
   band position t (0 at the inner ring, 1 at the reference outline). */
float flame( float u, float t ){
	/* The strip is the reference's own band, unwrapped over exactly one
	   turn, so it tiles by construction and is sampled straight. */
	float strand = texture2D( u_Texture0, vec2( u, t)).a;

	/* No mist and no boost on top: the strip already carries the
	   reference's interior glow and its hot rim - anything added here is a
	   departure from the art. The inner rows still fade in, because the
	   inner ring is a closed loop of geometry and any coverage on it draws
	   that loop as a hard oval over the character. */
	return strand * smoothstep( 0.0, 0.04, t);
}



void main(void) {
	float alpha = flame( gl_TexCoord[0].s, gl_TexCoord[0].t) * u_EntityColor.a;

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
