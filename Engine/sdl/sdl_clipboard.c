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

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include "../../Shared/q_shared.h"
#include "../../Shared/qcommon.h"

/*
===============================================================================

SYSTEM CLIPBOARD

The clipboard is the one system service the engine wants that only the window
system can answer, so it lives beside the other SDL back ends and is linked
into the client alone.  sys_unix.c is shared with the dedicated server, and the
dedicated server must not pull in SDL - Engine/null/null_clipboard.c answers
these two symbols there instead.

===============================================================================
*/

/*
==================
Sys_GetClipboardData

Returns a Z_Malloc'd single line the callers own, or NULL when the clipboard
holds nothing usable.  Field_Paste and CL_GetClipboardData both Z_Free the
result, so SDL's own buffer is copied out and released here.

The field editor has one line, no tabs and an 8 bit charset, so anything the
line cannot hold is dropped rather than pasted as garbage: the text is cut at
the first line break, tabs become spaces, and bytes outside printable ASCII
(which is every byte of a multi byte UTF-8 sequence) are skipped.
==================
*/
char *Sys_GetClipboardData( void )
{
	char	*sdlText;
	char	*data;
	char	*out;
	int		i;

	if ( !SDL_WasInit( SDL_INIT_VIDEO ) ) {
		return NULL;
	}

	sdlText = SDL_GetClipboardText();
	if ( !sdlText ) {
		return NULL;
	}

	data = Z_Malloc( strlen( sdlText ) + 1 );
	out = data;

	for ( i = 0 ; sdlText[i] ; i++ ) {
		char c = sdlText[i];

		if ( c == '\n' || c == '\r' ) {
			break;
		}
		if ( c == '\t' ) {
			c = ' ';
		}
		if ( (unsigned char)c < ' ' || (unsigned char)c > '~' ) {
			continue;
		}
		*out++ = c;
	}
	*out = '\0';

	SDL_free( sdlText );

	if ( !data[0] ) {
		Z_Free( data );
		return NULL;
	}

	return data;
}

/*
==================
Sys_SetClipboardData

Text is copied by SDL, so the caller keeps ownership of what it passes.
==================
*/
void Sys_SetClipboardData( const char *text )
{
	if ( !text || !SDL_WasInit( SDL_INIT_VIDEO ) ) {
		return;
	}

	SDL_SetClipboardText( text );
}
