/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_bloomsize.h -- the sizes the bloom pass works in, with no dependency on
// renderer state so that it can be linked (and tested) on its own.

#ifndef __TR_BLOOMSIZE_H
#define __TR_BLOOMSIZE_H

typedef struct {
	// Power-of-two texture the screen is copied into, and the fraction of it
	// the screen actually occupies.
	int		screenWidth, screenHeight;
	float	screenReadW, screenReadH;

	// Power-of-two texture the blurred effect lives in, and the fraction of it
	// the work region occupies.
	int		effectWidth, effectHeight;
	float	effectReadW, effectReadH;

	// The downscaled copy of the screen that is actually blurred. Rendered into
	// the corner of the real framebuffer, so it has to fit inside it.
	int		workWidth, workHeight;
} bloomSizes_t;

// Returns qfalse when this screen cannot support bloom at all - either texture
// would exceed maxTextureSize, or the work region would not fit on screen.
qboolean R_BloomSizes( bloomSizes_t *out, int vidWidth, int vidHeight,
		int sampleSize, int maxTextureSize );

#endif
