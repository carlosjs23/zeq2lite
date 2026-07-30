#version 120
/*
 * aura_fp.glsl
 * screen-space aura fragment program
 *
 * The vertex stage emits the spike texture twice - once scrolling each way -
 * because the texture has to flow toward the tip on both sides of the aura and
 * the direction therefore has to reverse somewhere. Crossfading the two copies
 * across that reversal hides the seam a hard mirror would leave, at the tip
 * where the spikes converge and at the base where nothing else would cover it.
 */

uniform sampler2D u_Texture0;
uniform vec4 u_EntityColor;

varying float v_seamBlend;
varying float v_edge;
varying float v_texBias;

/* Where along the ring the tip tint starts taking over. The dense body has to
   stay the entity's own colour or the aura stops reading as that character's;
   only the thinning outer spikes cool off. */
#define TIP_START 0.35

/* How far the tips get pushed toward blue. Short of 1 so a coloured aura keeps
   some of its hue at the very ends rather than resolving to a flat blue. */
#define TIP_SHIFT 0.8

void main(void) {
	/* No coordinate fixing up needed here: the stage binds this texture with
	   clampmapT, so S repeats around the ring while T clamps at the spike
	   tips. Sampling past the last row therefore returns the tips rather than
	   wrapping into the opaque body, which is what used to draw a bright
	   hairline around the aura's outer rim. */

	/* The bias is what stops a distant aura crawling: the vertex stage has
	   already dropped as many wraps of the strip as it can, and this takes the
	   sampler down the mip chain for whatever undersampling is left. */
	vec4 forward  = texture2D(u_Texture0, gl_TexCoord[0].st, v_texBias);
	vec4 mirrored = texture2D(u_Texture0, gl_TexCoord[1].st, v_texBias);

	vec4 spikes = mix(mirrored, forward, v_seamBlend);

	/* The aura art keeps its silhouette in alpha and leaves RGB solid white,
	   but this stage blends additively (GL_ONE, GL_ONE), which ignores alpha
	   outright. Premultiplying here is what turns the texture back into a
	   shaped glow - without it the ring paints as a flat white sheet, which is
	   exactly what a plain "spikes * tint" produced. */

	/* The tips run cool while the body stays the entity's colour. The blue is
	   derived from u_EntityColor rather than delivered as its own uniform:
	   every programParams slot is spoken for, and scaling the target by the
	   entity colour's brightest channel keeps a dim or a saturated aura from
	   either blowing out or going black at the ends. */
	float level = max(max(u_EntityColor.r, u_EntityColor.g), u_EntityColor.b);
	vec3  cool  = mix(u_EntityColor.rgb, vec3(0.15, 0.45, 1.0) * level, TIP_SHIFT);
	vec3  tint  = mix(u_EntityColor.rgb, cool, smoothstep(TIP_START, 1.0, v_edge));

	vec3 glow = spikes.rgb * spikes.a * tint * u_EntityColor.a;

	gl_FragColor = vec4(glow, 1.0);
}
