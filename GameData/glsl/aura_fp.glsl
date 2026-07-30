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
varying float v_base;

/* Where along the ring the tip tint starts taking over. The dense body has to
   stay the entity's own colour or the aura stops reading as that character's;
   only the thinning outer spikes cool off. */
#define TIP_START 0.35

/* How far the tips deepen. The shift used to be toward a fixed blue, and that
   is wrong for the material: a Super Saiyan aura is one hue from the body to
   the ends of its spikes, and the only gradient in it is white core to
   saturated gold. A fixed target cannot do that, because it is a different
   hue for every character - plausible on gold, magenta on red, and it turned
   the whole crown cyan, which is the single thing that most gave the effect
   away against reference art.

   The tips now deepen into the character's *own* colour instead. That keeps a
   temperature gradient - the ends read cooler than the core because they are
   more saturated and less white - without inventing a hue that belongs to
   nobody. */
#define TIP_SHIFT 0.85

/* Where the white-hot core has faded back to the entity's own colour. Sits
   just outside the strip's alpha peak, so the whitest pixels are also the
   densest ones and the core reads as a single hot band rather than as a pale
   wash over the whole body. */
#define CORE_END 0.20

/* How far the core is pushed toward white. Kept low: the animated reference
   has no white sheath around the body at all - the flame is out at the edges
   and the space the character occupies is left open, because the character is
   lit on their own cel rather than by the aura. A strong core reads as the
   aura being wrapped around them instead of standing behind them. */
#define CORE_SHIFT 0.25

/* How far above alpha-over the core is driven, and where it settles back. Only
   the core glows: pushing the whole aura past unity is just the additive
   version again, hue and all. */
#define CORE_GLOW 1.35
#define GLOW_END  0.45

/* Extra glow where the aura gathers under the character. Rides on top of the
   core term rather than replacing it, so the point under the feet reads as the
   same white-hot material as the core and not as a second light source. */
#define BASE_GLOW 1.5

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

	/* The aura art keeps its silhouette in alpha and leaves RGB solid white, so
	   everything below shapes the tint and the alpha carries the silhouette. */

	/* The tips run cool while the body stays the entity's colour. The blue is
	   derived from u_EntityColor rather than delivered as its own uniform:
	   every programParams slot is spoken for, and scaling the target by the
	   entity colour's brightest channel keeps a dim or a saturated aura from
	   either blowing out or going black at the ends. */
	float level = max(max(u_EntityColor.r, u_EntityColor.g), u_EntityColor.b);

	/* Squaring and renormalising against the brightest channel pushes the
	   colour away from grey while pinning that channel where it was, so the
	   result is the same hue carrying more of it: gold deepens to amber,
	   white stays white because it has no hue to deepen, and a colour that is
	   already saturated is left alone. Scaling the channels down instead just
	   darkens, which reads as the aura running out rather than concentrating. */
	vec3  deep  = u_EntityColor.rgb * u_EntityColor.rgb / max(level, 0.0001);
	vec3  cool  = mix(u_EntityColor.rgb, deep, TIP_SHIFT);
	vec3  tint  = mix(u_EntityColor.rgb, cool, smoothstep(TIP_START, 1.0, v_edge));

	/* Inside that, the core runs hotter than the body: ki is drawn brightest
	   where it is densest, and without this the aura is one flat hue with a
	   cool fringe - it has a colour but no temperature. Desaturating toward
	   `level` rather than toward vec3(1.0) keeps a dim aura's core dim, so a
	   character whose colour is deliberately muted does not get a white core
	   as bright as everyone else's. */
	vec3  hot   = mix(u_EntityColor.rgb, vec3(level), CORE_SHIFT);
	tint = mix(hot, tint, smoothstep(0.0, CORE_END, v_edge));

	float alpha = spikes.a * u_EntityColor.a;

	/* Premultiplied output against GL_ONE / GL_ONE_MINUS_SRC_ALPHA, which is
	   the reason one stage can be both things at once.

	   That blend computes src.rgb + dst * (1 - src.a). Feed it rgb = tint *
	   alpha and it is exactly alpha-over: the aura holds its own colour no
	   matter how bright the scene behind it, which straight addition cannot do
	   - gold over a lit sky sums past 1 and comes back white with the sky's
	   own hue in it. Push rgb past tint * alpha and the excess is added rather
	   than blended, so it glows into the scene the way the additive version
	   did.

	   So the core is driven well above unity and burns out into the world,
	   while the body and spikes sit at unity and stay the character's colour.
	   Which is what the reference art shows: a white-hot centre that clearly
	   emits, surrounded by flame that is solidly, unmistakably gold. */
	float boost = mix(CORE_GLOW, 1.0, smoothstep(0.0, GLOW_END, v_edge));

	/* Weighted toward the inner rows as well as toward the base, so the hot
	   spot sits where the flame leaves the ground rather than smearing along
	   the whole length of the licks that spring from it. */
	boost += BASE_GLOW * v_base * (1.0 - smoothstep(0.0, 0.7, v_edge));

	gl_FragColor = vec4(spikes.rgb * tint * alpha * boost, alpha);
}
