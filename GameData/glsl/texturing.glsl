#version 120
/*
 * texturing.glsl
 * shared texturing routines
 * Copyright (C) 2010  Jens Loehr <jens.loehr@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* multi-texturing modes */
#define REPLACE		0
#define MODULATE	1
#define DECAL		2
#define BLEND		3
#define ADD			4
#define COMBINE		5

/*
 * Takes the texture coordinate and environment colour by value rather than an
 * index into gl_TexCoord/gl_TextureEnvColor. Two reasons, both of which stopped
 * this compiling at all on strict GLSL 1.20 drivers (Apple's, for one):
 *
 *   - gl_TexCoord is declared implicitly sized, and indexing it with a value
 *     that is not a compile-time constant is illegal. Every call site passes a
 *     literal, but the constness is lost crossing the parameter, so the
 *     compiler can only see a dynamic index.
 *   - the local below was named "texture", which is reserved. The declaration
 *     was rejected and every later mention then reported as undeclared, which
 *     is why one bad line produced a dozen errors.
 */
vec4 applyTexture2D(sampler2D textureUnit, int type, vec4 texCoord, vec4 envColor, vec4 color) {
	vec4 texel = texture2D(textureUnit, texCoord.st);

	if (type == REPLACE) {
		color		= texel;
	} else if (type == MODULATE) {
		color		*= texel;
	} else if (type == DECAL) {
		color		= vec4(mix(color.rgb, texel.rgb, texel.a), color.a);
	} else if (type == BLEND) {
		color		= vec4(mix(color.rgb, envColor.rgb, texel.rgb), color.a * texel.a);
	} else if (type == ADD) {
		color.rgb	+= texel.rgb;
		color.a		*= texel.a;

		color		= clamp(color, 0.0, 1.0);
	} else {
		color		= clamp(texel * color, 0.0, 1.0);
	}

	return color;
}
