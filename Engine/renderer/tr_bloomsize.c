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
// tr_bloomsize.c -- see tr_bloomsize.h. Split out of tr_bloom.c: the arithmetic
// needs no GL, only the screen size, the sample size and the texture limit.

#include "../../Shared/q_shared.h"
#include "tr_bloomsize.h"

/*
================
R_NextPowerOfTwo
================
*/
static int R_NextPowerOfTwo( int value ) {
	int pot;

	for ( pot = 1; pot < value; pot *= 2 )
		;

	return pot;
}

/*
================
R_BloomSizes
================
*/
qboolean R_BloomSizes( bloomSizes_t *out, int vidWidth, int vidHeight,
		int sampleSize, int maxTextureSize ) {
	if ( !out || vidWidth <= 0 || vidHeight <= 0 || sampleSize <= 0 ) {
		return qfalse;
	}

	out->screenWidth = R_NextPowerOfTwo( vidWidth );
	out->screenHeight = R_NextPowerOfTwo( vidHeight );
	out->screenReadW = vidWidth / (float)out->screenWidth;
	out->screenReadH = vidHeight / (float)out->screenHeight;

	// A downscaled copy of the screen, so it carries the screen's shape: the
	// height follows the width by vidHeight/vidWidth, in that order and in
	// floating point.
	out->workWidth = sampleSize;
	out->workHeight = (int)( sampleSize * (double)vidHeight / (double)vidWidth + 0.5 );
	if ( out->workHeight < 1 ) {
		out->workHeight = 1;
	}

	out->effectWidth = R_NextPowerOfTwo( out->workWidth );
	out->effectHeight = R_NextPowerOfTwo( out->workHeight );
	out->effectReadW = out->workWidth / (float)out->effectWidth;
	out->effectReadH = out->workHeight / (float)out->effectHeight;

	if ( out->screenWidth > maxTextureSize ||
			out->screenHeight > maxTextureSize ||
			out->effectWidth > maxTextureSize ||
			out->effectHeight > maxTextureSize ||
			out->workWidth > vidWidth ||
			out->workHeight > vidHeight ) {
		return qfalse;
	}

	return qtrue;
}
