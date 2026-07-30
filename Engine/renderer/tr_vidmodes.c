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
// tr_vidmodes.c -- the r_mode table and the arithmetic that resolves an r_mode
// value into a resolution.

#include "../../Shared/q_shared.h"
#include "tr_vidmodes.h"

/*
Modes 0-11 are the id table and their indices must not move: r_mode is archived
in every player's config, so renumbering them silently changes the resolution
people already chose. New shapes are appended instead.

Every entry from 12 up exists because the original table stopped at 4:3 (plus
one 856x480 curiosity), so anyone on a widescreen display had to find
r_customwidth/r_customheight for themselves. r_mode -2 covers the common case
of "just use whatever the display is set to".
*/
static const vidmode_t r_vidModes[] =
{
	{ "Mode  0: 320x240",			320,	240,	1 },
	{ "Mode  1: 400x300",			400,	300,	1 },
	{ "Mode  2: 512x384",			512,	384,	1 },
	{ "Mode  3: 640x480",			640,	480,	1 },
	{ "Mode  4: 800x600",			800,	600,	1 },
	{ "Mode  5: 960x720",			960,	720,	1 },
	{ "Mode  6: 1024x768",			1024,	768,	1 },
	{ "Mode  7: 1152x864",			1152,	864,	1 },
	{ "Mode  8: 1280x1024",			1280,	1024,	1 },
	{ "Mode  9: 1600x1200",			1600,	1200,	1 },
	{ "Mode 10: 2048x1536",			2048,	1536,	1 },
	{ "Mode 11: 856x480 (wide)",	856,	480,	1 },
	{ "Mode 12: 1280x720 (16:9)",	1280,	720,	1 },
	{ "Mode 13: 1366x768 (16:9)",	1366,	768,	1 },
	{ "Mode 14: 1600x900 (16:9)",	1600,	900,	1 },
	{ "Mode 15: 1920x1080 (16:9)",	1920,	1080,	1 },
	{ "Mode 16: 2560x1440 (16:9)",	2560,	1440,	1 },
	{ "Mode 17: 3840x2160 (16:9)",	3840,	2160,	1 },
	{ "Mode 18: 1280x800 (16:10)",	1280,	800,	1 },
	{ "Mode 19: 1440x900 (16:10)",	1440,	900,	1 },
	{ "Mode 20: 1680x1050 (16:10)",	1680,	1050,	1 },
	{ "Mode 21: 1920x1200 (16:10)",	1920,	1200,	1 },
	{ "Mode 22: 2560x1600 (16:10)",	2560,	1600,	1 },
	{ "Mode 23: 2880x1800 (16:10)",	2880,	1800,	1 },
	{ "Mode 24: 2560x1080 (21:9)",	2560,	1080,	1 },
	{ "Mode 25: 3440x1440 (21:9)",	3440,	1440,	1 }
};

int R_NumVidModes( void ) {
	return ARRAY_LEN( r_vidModes );
}

const vidmode_t *R_VidMode( int mode ) {
	if ( mode < 0 || mode >= R_NumVidModes() ) {
		return NULL;
	}
	return &r_vidModes[mode];
}

/*
================
R_ModeInfo
================
*/
qboolean R_ModeInfo( int *width, int *height, float *windowAspect, int mode,
		int customWidth, int customHeight, float customPixelAspect,
		int desktopWidth, int desktopHeight ) {
	const vidmode_t	*vm;
	float			pixelAspect;

	if ( !width || !height || !windowAspect ) {
		return qfalse;
	}

	switch ( mode ) {
		case VID_MODE_CUSTOM:
			*width = customWidth;
			*height = customHeight;
			pixelAspect = customPixelAspect;
			break;

		case VID_MODE_DESKTOP:
			// The platform layer passes 0 when it could not read a desktop
			// mode. Creating a 0x0 window from that fails somewhere deep in the
			// GL driver, so fall back on the authored resolution instead.
			if ( desktopWidth > 0 && desktopHeight > 0 ) {
				*width = desktopWidth;
				*height = desktopHeight;
			} else {
				*width = SCREEN_WIDTH;
				*height = SCREEN_HEIGHT;
			}
			pixelAspect = 1.0f;
			break;

		default:
			vm = R_VidMode( mode );
			if ( !vm ) {
				return qfalse;
			}
			*width = vm->width;
			*height = vm->height;
			pixelAspect = vm->pixelAspect;
			break;
	}

	if ( *width <= 0 || *height <= 0 || pixelAspect <= 0.0f ) {
		return qfalse;
	}

	*windowAspect = (float)*width / ( *height * pixelAspect );

	return qtrue;
}
