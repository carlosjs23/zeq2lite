#version 120
/*
 * aura_glow_fp.glsl
 * the aura's interior glow
 *
 * The reference art fills the space the flame encloses with a soft veil,
 * darkest at the centre and brightening toward the band - the ki is thin
 * where the body is and gathers where the flame starts. The ring mesh never
 * covers that space, so this runs on a camera-facing sprite the size of the
 * player's box, added behind the ring by cg_auras.c.
 *
 * Additive on purpose: a backlight has no silhouette to protect, it only
 * lifts what is behind it, and additive output needs no draw-order argument
 * with the flame in front of it.
 */

uniform vec4 u_EntityColor;

/* Glow at the sprite's centre and at its rim, before the entity colour and
   its fade scale both. The rim value carries most of the light: the veil
   reads as the flame's own spill, so it has to be strongest where the flame
   begins. */
#define GLOW_CENTRE 0.08
#define GLOW_RIM    0.30

void main(void) {
	vec2  p = gl_TexCoord[0].st * 2.0 - 1.0;
	float r = length(p);

	/* Rise toward the rim, then fall away entirely before the sprite's
	   corners: a visible square edge is the one thing this must never
	   produce. */
	float veil = mix( GLOW_CENTRE, GLOW_RIM, r * r);
	veil *= 1.0 - smoothstep( 0.55, 0.92, r);

	gl_FragColor = vec4( u_EntityColor.rgb * u_EntityColor.a * veil, 1.0);
}
