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

#ifdef SMP
#	ifdef USE_LOCAL_HEADERS
#		include "SDL_thread.h"
#	else
#		include <SDL_thread.h>
#	endif
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../renderer/tr_local.h"
#include "../client/client.h"
#include "../sys/sys_local.h"
#include "sdl_icon.h"

typedef enum
{
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
} rserr_t;

// SDL2 has an explicit window and GL context instead of SDL 1.2's single
// implicit "screen" surface.  sdl_gamma.c is linked into the same module and
// reads SDL_window, so it is deliberately not static.
SDL_Window *SDL_window = NULL;
static SDL_GLContext SDL_glContext = NULL;

// The SMP path used to hand the GL context between threads with CGL directly;
// SDL2 exposes that portably, so the CGL dependency is gone.
#define GLimp_SetCurrentContext(ctx) SDL_GL_MakeCurrent( SDL_window, (ctx) )

cvar_t *r_allowSoftwareGL; // Don't abort out if a hardware visual can't be obtained
cvar_t *r_allowResize; // make window resizable
cvar_t *r_allowHighDPI; // render at the display's real pixel density
cvar_t *r_centerWindow;
cvar_t *r_sdlDriver;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

// GL_ARB_framebuffer_object
GLvoid (APIENTRYP qglGenFramebuffers) (GLsizei n, GLuint *framebuffers);
GLvoid (APIENTRYP qglDeleteFramebuffers) (GLsizei n, const GLuint *framebuffers);
GLvoid (APIENTRYP qglBindFramebuffer) (GLenum target, GLuint framebuffer);
GLvoid (APIENTRYP qglFramebufferTexture2D) (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
GLenum (APIENTRYP qglCheckFramebufferStatus) (GLenum target);

// GL_ARB_shader_objects
GLvoid (APIENTRYP qglDeleteObjectARB) (GLhandleARB obj);
GLhandleARB (APIENTRYP qglGetHandleARB) (GLenum pname);
GLvoid (APIENTRYP qglDetachObjectARB) (GLhandleARB containerObj, GLhandleARB attachedObj);
GLhandleARB (APIENTRYP qglCreateShaderObjectARB) (GLenum shaderType);
GLvoid (APIENTRYP qglShaderSourceARB) (GLhandleARB shaderObj, GLsizei count, const GLcharARB **string,
				       const GLint *length);
GLvoid (APIENTRYP qglCompileShaderARB) (GLhandleARB shaderObj);
GLhandleARB (APIENTRYP qglCreateProgramObjectARB) (void);
GLvoid (APIENTRYP qglAttachObjectARB) (GLhandleARB containerObj, GLhandleARB obj);
GLvoid (APIENTRYP qglLinkProgramARB) (GLhandleARB programObj);
GLvoid (APIENTRYP qglUseProgramObjectARB) (GLhandleARB programObj);
GLvoid (APIENTRYP qglValidateProgramARB) (GLhandleARB programObj);
GLvoid (APIENTRYP qglUniform1fARB) (GLint location, GLfloat v0);
GLvoid (APIENTRYP qglUniform2fARB) (GLint location, GLfloat v0, GLfloat v1);
GLvoid (APIENTRYP qglUniform3fARB) (GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
GLvoid (APIENTRYP qglUniform4fARB) (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
GLvoid (APIENTRYP qglUniform1iARB) (GLint location, GLint v0);
GLvoid (APIENTRYP qglUniform2iARB) (GLint location, GLint v0, GLint v1);
GLvoid (APIENTRYP qglUniform3iARB) (GLint location, GLint v0, GLint v1, GLint v2);
GLvoid (APIENTRYP qglUniform4iARB) (GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
GLvoid (APIENTRYP qglUniform1fvARB) (GLint location, GLsizei count, const GLfloat *value);
GLvoid (APIENTRYP qglUniform2fvARB) (GLint location, GLsizei count, const GLfloat *value);
GLvoid (APIENTRYP qglUniform3fvARB) (GLint location, GLsizei count, const GLfloat *value);
GLvoid (APIENTRYP qglUniform4fvARB) (GLint location, GLsizei count, const GLfloat *value);
GLvoid (APIENTRYP qglUniform1ivARB) (GLint location, GLsizei count, const GLint *value);
GLvoid (APIENTRYP qglUniform2ivARB) (GLint location, GLsizei count, const GLint *value);
GLvoid (APIENTRYP qglUniform3ivARB) (GLint location, GLsizei count, const GLint *value);
GLvoid (APIENTRYP qglUniform4ivARB) (GLint location, GLsizei count, const GLint *value);
GLvoid (APIENTRYP qglUniformMatrix2fvARB) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLvoid (APIENTRYP qglUniformMatrix3fvARB) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLvoid (APIENTRYP qglUniformMatrix4fvARB) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLvoid (APIENTRYP qglGetObjectParameterfvARB) (GLhandleARB obj, GLenum pname, GLfloat *params);
GLvoid (APIENTRYP qglGetObjectParameterivARB) (GLhandleARB obj, GLenum pname, GLint *params);
GLvoid (APIENTRYP qglGetInfoLogARB) (GLhandleARB obj, GLsizei maxLength, GLsizei *length, GLcharARB *infoLog);
GLvoid (APIENTRYP qglGetAttachedObjectsARB) (GLhandleARB containerObj, GLsizei maxCount, GLsizei *count,
					     GLhandleARB *obj);
GLint(APIENTRYP qglGetUniformLocationARB) (GLhandleARB programObj, const GLcharARB * name);
GLvoid (APIENTRYP qglGetActiveUniformARB) (GLhandleARB programObj, GLuint index, GLsizei maxLength,
					   GLsizei *length, GLint *size, GLenum *type, GLcharARB *name);
GLvoid (APIENTRYP qglGetUniformfvARB) (GLhandleARB programObj, GLint location, GLfloat *params);
GLvoid (APIENTRYP qglGetUniformivARB) (GLhandleARB programObj, GLint location, GLint *params);
GLvoid (APIENTRYP qglGetShaderSourceARB) (GLhandleARB obj, GLsizei maxLength, GLsizei *length,
					  GLcharARB *source);
GLvoid (APIENTRYP qglVertexAttrib1fARB) (GLuint index, GLfloat v0);
GLvoid (APIENTRYP qglVertexAttrib1sARB) (GLuint index, GLshort v0);
GLvoid (APIENTRYP qglVertexAttrib1dARB) (GLuint index, GLdouble v0);
GLvoid (APIENTRYP qglVertexAttrib2fARB) (GLuint index, GLfloat v0, GLfloat v1);
GLvoid (APIENTRYP qglVertexAttrib2sARB) (GLuint index, GLshort v0, GLshort v1);
GLvoid (APIENTRYP qglVertexAttrib2dARB) (GLuint index, GLdouble v0, GLdouble v1);
GLvoid (APIENTRYP qglVertexAttrib3fARB) (GLuint index, GLfloat v0, GLfloat v1, GLfloat v2);
GLvoid (APIENTRYP qglVertexAttrib3sARB) (GLuint index, GLshort v0, GLshort v1, GLshort v2);
GLvoid (APIENTRYP qglVertexAttrib3dARB) (GLuint index, GLdouble v0, GLdouble v1, GLdouble v2);
GLvoid (APIENTRYP qglVertexAttrib4fARB) (GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
GLvoid (APIENTRYP qglVertexAttrib4sARB) (GLuint index, GLshort v0, GLshort v1, GLshort v2, GLshort v3);
GLvoid (APIENTRYP qglVertexAttrib4dARB) (GLuint index, GLdouble v0, GLdouble v1, GLdouble v2, GLdouble v3);
GLvoid (APIENTRYP qglVertexAttrib4NubARB) (GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w);
GLvoid (APIENTRYP qglVertexAttrib1fvARB) (GLuint index, GLfloat *v);
GLvoid (APIENTRYP qglVertexAttrib1svARB) (GLuint index, GLshort *v);
GLvoid (APIENTRYP qglVertexAttrib1dvARB) (GLuint index, GLdouble *v);
GLvoid (APIENTRYP qglVertexAttrib2fvARB) (GLuint index, GLfloat *v);
GLvoid (APIENTRYP qglVertexAttrib2svARB) (GLuint index, GLshort *v);
GLvoid (APIENTRYP qglVertexAttrib2dvARB) (GLuint index, GLdouble *v);
GLvoid (APIENTRYP qglVertexAttrib3fvARB) (GLuint index, GLfloat *v);
GLvoid (APIENTRYP qglVertexAttrib3svARB) (GLuint index, GLshort *v);
GLvoid (APIENTRYP qglVertexAttrib3dvARB) (GLuint index, GLdouble *v);
GLvoid (APIENTRYP qglVertexAttrib4fvARB) (GLuint index, GLfloat *v);
GLvoid (APIENTRYP qglVertexAttrib4svARB) (GLuint index, GLshort *v);
GLvoid (APIENTRYP qglVertexAttrib4dvARB) (GLuint index, GLdouble *v);
GLvoid (APIENTRYP qglVertexAttrib4ivARB) (GLuint index, GLint *v);
GLvoid (APIENTRYP qglVertexAttrib4bvARB) (GLuint index, GLbyte *v);
GLvoid (APIENTRYP qglVertexAttrib4ubvARB) (GLuint index, GLubyte *v);
GLvoid (APIENTRYP qglVertexAttrib4usvARB) (GLuint index, GLushort *v);
GLvoid (APIENTRYP qglVertexAttrib4uivARB) (GLuint index, GLuint *v);
GLvoid (APIENTRYP qglVertexAttrib4NbvARB) (GLuint index, const GLbyte *v);
GLvoid (APIENTRYP qglVertexAttrib4NsvARB) (GLuint index, const GLshort *v);
GLvoid (APIENTRYP qglVertexAttrib4NivARB) (GLuint index, const GLint *v);
GLvoid (APIENTRYP qglVertexAttrib4NubvARB) (GLuint index, const GLubyte *v);
GLvoid (APIENTRYP qglVertexAttrib4NusvARB) (GLuint index, const GLushort *v);
GLvoid (APIENTRYP qglVertexAttrib4NuivARB) (GLuint index, const GLuint *v);
GLvoid (APIENTRYP qglVertexAttribPointerARB) (GLuint index, GLint size, GLenum type, GLboolean normalized,
					      GLsizei stride, const GLvoid *pointer);
GLvoid (APIENTRYP qglEnableVertexAttribArrayARB) (GLuint index);
GLvoid (APIENTRYP qglDisableVertexAttribArrayARB) (GLuint index);
GLvoid (APIENTRYP qglBindAttribLocationARB) (GLhandleARB programObj, GLuint index, const GLcharARB *name);
GLvoid (APIENTRYP qglGetActiveAttribARB) (GLhandleARB programObj, GLuint index, GLsizei maxLength,
					  GLsizei *length, GLint *size, GLenum *type, GLcharARB *name);
GLint(APIENTRYP qglGetAttribLocationARB) (GLhandleARB programObj, const GLcharARB * name);
GLvoid (APIENTRYP qglGetVertexAttribdvARB) (GLuint index, GLenum pname, GLdouble *params);
GLvoid (APIENTRYP qglGetVertexAttribfvARB) (GLuint index, GLenum pname, GLfloat *params);
GLvoid (APIENTRYP qglGetVertexAttribivARB) (GLuint index, GLenum pname, GLint *params);
GLvoid (APIENTRYP qglGetVertexAttribPointervARB) (GLuint index, GLenum pname, GLvoid **pointer);

/*
===============
GLimp_Shutdown
===============
*/
void GLimp_Shutdown( void )
{
	ri.IN_Shutdown();

	if( SDL_glContext )
	{
		SDL_GL_DeleteContext( SDL_glContext );
		SDL_glContext = NULL;
	}

	if( SDL_window )
	{
		SDL_DestroyWindow( SDL_window );
		SDL_window = NULL;
	}

	SDL_QuitSubSystem( SDL_INIT_VIDEO );

	Com_Memset( &glConfig, 0, sizeof( glConfig ) );
	Com_Memset( &glState, 0, sizeof( glState ) );
}

/*
===============
GLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void GLimp_Minimize(void)
{
	SDL_MinimizeWindow( SDL_window );
}


/*
===============
GLimp_LogComment
===============
*/
void GLimp_LogComment( char *comment )
{
}

/*
===============
GLimp_CompareModes
===============
*/
static int GLimp_CompareModes( const void *a, const void *b )
{
	const float ASPECT_EPSILON = 0.001f;
	// SDL2 hands us an array of SDL_Rect by value, not an array of pointers
	SDL_Rect *modeA = (SDL_Rect *)a;
	SDL_Rect *modeB = (SDL_Rect *)b;
	float aspectA = (float)modeA->w / (float)modeA->h;
	float aspectB = (float)modeB->w / (float)modeB->h;
	int areaA = modeA->w * modeA->h;
	int areaB = modeB->w * modeB->h;
	float aspectDiffA = fabs( aspectA - displayAspect );
	float aspectDiffB = fabs( aspectB - displayAspect );
	float aspectDiffsDiff = aspectDiffA - aspectDiffB;

	if( aspectDiffsDiff > ASPECT_EPSILON )
		return 1;
	else if( aspectDiffsDiff < -ASPECT_EPSILON )
		return -1;
	else
		return areaA - areaB;
}


/*
===============
GLimp_DetectAvailableModes
===============
*/
static void GLimp_DetectAvailableModes(void)
{
	char buf[ MAX_STRING_CHARS ] = { 0 };
	SDL_Rect *modes;
	SDL_DisplayMode windowMode;
	int numSDLModes;
	int numModes = 0;
	int display;
	int i, j;

	display = SDL_GetWindowDisplayIndex( SDL_window );
	if( display < 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display index, "
				"no resolutions detected: %s\n", SDL_GetError( ) );
		return;
	}

	numSDLModes = SDL_GetNumDisplayModes( display );

	if( SDL_GetWindowDisplayMode( SDL_window, &windowMode ) < 0 || numSDLModes <= 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display mode, "
				"no resolutions detected: %s\n", SDL_GetError( ) );
		return;
	}

	modes = SDL_calloc( (size_t)numSDLModes, sizeof( SDL_Rect ) );
	if( !modes )
	{
		ri.Error( ERR_FATAL, "Out of memory" );
		return;
	}

	for( i = 0; i < numSDLModes; i++ )
	{
		SDL_DisplayMode mode;

		if( SDL_GetDisplayMode( display, i, &mode ) < 0 )
			continue;

		if( !mode.w || !mode.h )
		{
			ri.Printf( PRINT_ALL, "Display supports any resolution\n" );
			SDL_free( modes );
			return;
		}

		if( windowMode.format != mode.format )
			continue;

		// SDL can report the same resolution at several refresh rates;
		// only list each resolution once.
		for( j = 0; j < numModes; j++ )
		{
			if( mode.w == modes[ j ].w && mode.h == modes[ j ].h )
				break;
		}

		if( j != numModes )
			continue;

		modes[ numModes ].w = mode.w;
		modes[ numModes ].h = mode.h;
		numModes++;
	}

	if( numModes > 1 )
		qsort( modes, numModes, sizeof( SDL_Rect ), GLimp_CompareModes );

	for( i = 0; i < numModes; i++ )
	{
		const char *newModeString = va( "%ux%u ", modes[ i ].w, modes[ i ].h );

		if( strlen( newModeString ) < (int)sizeof( buf ) - strlen( buf ) )
			Q_strcat( buf, sizeof( buf ), newModeString );
		else
			ri.Printf( PRINT_WARNING, "Skipping mode %ux%u, buffer too small\n",
					modes[ i ].w, modes[ i ].h );
	}

	if( *buf )
	{
		buf[ strlen( buf ) - 1 ] = 0;
		ri.Printf( PRINT_ALL, "Available modes: '%s'\n", buf );
		ri.Cvar_Set( "r_availableModes", buf );
	}

	SDL_free( modes );
}

/*
===============
GLimp_SetMode
===============
*/
static int GLimp_SetMode(int mode, qboolean fullscreen, qboolean noborder)
{
	const char*   glstring;
	int sdlcolorbits;
	int colorbits, depthbits, stencilbits;
	int tcolorbits, tdepthbits, tstencilbits;
	int realcolorbits[3];
	int samples;
	int i = 0;
	SDL_Surface *icon = NULL;
	SDL_DisplayMode desktopMode;
	int display = 0;
	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;
	// Created hidden and shown once the context is up, so a rejected pixel
	// format never flashes an empty window at the user.
	Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL;

	ri.Printf( PRINT_ALL, "Initializing OpenGL display\n");

	if ( r_allowResize->integer )
		flags |= SDL_WINDOW_RESIZABLE;

	// On a Retina display the window is measured in points and the framebuffer
	// in pixels. Without this flag macOS hands us a point-sized framebuffer and
	// upscales it, which is the blurry rendering the port shipped with; with it
	// we render at the panel's real resolution. The requested r_mode stays the
	// window size in points either way, so a mode means the same physical size
	// on any display.
	if ( r_allowHighDPI->integer )
		flags |= SDL_WINDOW_ALLOW_HIGHDPI;

	// If a window already exists, stay on whatever display it is on
	if( SDL_window != NULL )
	{
		display = SDL_GetWindowDisplayIndex( SDL_window );
		if( display < 0 )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GetWindowDisplayIndex() failed: %s\n",
					SDL_GetError( ) );
			display = 0;
		}
	}

	if( SDL_GetDesktopDisplayMode( display, &desktopMode ) == 0 )
	{
		// Guess the display aspect ratio through the desktop resolution
		// by assuming (relatively safely) that it is set at or close to
		// the display's native aspect ratio
		displayAspect = (float)desktopMode.w / (float)desktopMode.h;

		// Published for r_mode -2 ("use the desktop resolution"), which is
		// resolved by R_GetModeInfo a few lines below.
		displayWidth = desktopMode.w;
		displayHeight = desktopMode.h;

		ri.Printf( PRINT_ALL, "Estimated display aspect: %.3f\n", displayAspect );
	}
	else
	{
		Com_Memset( &desktopMode, 0, sizeof( desktopMode ) );

		ri.Printf( PRINT_ALL,
				"Cannot estimate display aspect, assuming 1.333\n" );
	}

	ri.Printf (PRINT_ALL, "...setting mode %d:", mode );

	if ( !R_GetModeInfo( &glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode ) )
	{
		ri.Printf( PRINT_ALL, " invalid mode\n" );
		return RSERR_INVALID_MODE;
	}
	ri.Printf( PRINT_ALL, " %d %d\n", glConfig.vidWidth, glConfig.vidHeight);

	// Desktop fullscreen covers the display whatever size we ask for, and macOS
	// makes that transition asynchronously - a drawable measured right after
	// SDL_CreateWindow would still report the size requested here. Ask for the
	// desktop size to begin with and the two agree immediately.
	if( fullscreen && desktopMode.w > 0 && desktopMode.h > 0 )
	{
		glConfig.vidWidth = desktopMode.w;
		glConfig.vidHeight = desktopMode.h;
		glConfig.windowAspect = (float)desktopMode.w / (float)desktopMode.h;

		ri.Printf( PRINT_ALL, "...fullscreen uses the desktop resolution %d x %d\n",
				desktopMode.w, desktopMode.h );
	}

	// r_centerWindow used to be handled by the SDL_VIDEO_CENTERED environment
	// variable, which SDL2 ignores; position the window explicitly instead.
	if( r_centerWindow->integer && !fullscreen )
	{
		x = ( desktopMode.w / 2 ) - ( glConfig.vidWidth / 2 );
		y = ( desktopMode.h / 2 ) - ( glConfig.vidHeight / 2 );
	}

	// Destroy any existing state (vid_restart)
	if( SDL_glContext != NULL )
	{
		SDL_GL_DeleteContext( SDL_glContext );
		SDL_glContext = NULL;
	}

	if( SDL_window != NULL )
	{
		SDL_GetWindowPosition( SDL_window, &x, &y );
		ri.Printf( PRINT_DEVELOPER, "Existing window at %dx%d before being destroyed\n", x, y );
		SDL_DestroyWindow( SDL_window );
		SDL_window = NULL;
	}

	if (fullscreen)
	{
		// Exclusive fullscreen, switched to the mode the display is already in
		// (set below). Asking for r_mode's size instead needs a mode the
		// display enumerates, and a Retina panel driven at a scaled resolution
		// enumerates none - SDL then either declines outright ("Couldn't find
		// display mode match") or grants a fullscreen window still sized to
		// r_mode, covering part of the screen. SDL_WINDOW_FULLSCREEN_DESKTOP
		// avoids that but is fitted to the screen's *visible* frame, which
		// leaves the menu bar's 33 points as a dark band along the top.
		// r_mode therefore sizes the window only.
		flags |= SDL_WINDOW_FULLSCREEN;
		glConfig.isFullscreen = qtrue;
	}
	else
	{
		if (noborder)
			flags |= SDL_WINDOW_BORDERLESS;

		glConfig.isFullscreen = qfalse;
	}

	// ZEQ2's own window icon.  SDL2 takes the surface and copies it, so it can
	// be freed as soon as every SDL_CreateWindow attempt is done with it.
	icon = SDL_LoadBMP( "ZEQ2/zeq2.bmp" );

	colorbits = r_colorbits->value;
	if ((!colorbits) || (colorbits >= 32))
		colorbits = 24;

	if (!r_depthbits->value)
		depthbits = 24;
	else
		depthbits = r_depthbits->value;
	stencilbits = r_stencilbits->value;
	samples = r_ext_multisample->value;

	for (i = 0; i < 16; i++)
	{
		// 0 - default
		// 1 - minus colorbits
		// 2 - minus depthbits
		// 3 - minus stencil
		if ((i % 4) == 0 && i)
		{
			// one pass, reduce
			switch (i / 4)
			{
				case 2 :
					if (colorbits == 24)
						colorbits = 16;
					break;
				case 1 :
					if (depthbits == 24)
						depthbits = 16;
					else if (depthbits == 16)
						depthbits = 8;
				case 3 :
					if (stencilbits == 24)
						stencilbits = 16;
					else if (stencilbits == 16)
						stencilbits = 8;
			}
		}

		tcolorbits = colorbits;
		tdepthbits = depthbits;
		tstencilbits = stencilbits;

		if ((i % 4) == 3)
		{ // reduce colorbits
			if (tcolorbits == 24)
				tcolorbits = 16;
		}

		if ((i % 4) == 2)
		{ // reduce depthbits
			if (tdepthbits == 24)
				tdepthbits = 16;
			else if (tdepthbits == 16)
				tdepthbits = 8;
		}

		if ((i % 4) == 1)
		{ // reduce stencilbits
			if (tstencilbits == 24)
				tstencilbits = 16;
			else if (tstencilbits == 16)
				tstencilbits = 8;
			else
				tstencilbits = 0;
		}

		sdlcolorbits = 4;
		if (tcolorbits == 24)
			sdlcolorbits = 8;

#ifdef __sgi /* Fix for SGIs grabbing too many bits of color */
		if (sdlcolorbits == 4)
			sdlcolorbits = 0; /* Use minimum size for 16-bit color */

		/* Need alpha or else SGIs choose 36+ bit RGB mode */
		SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE, 1);
#endif

		SDL_GL_SetAttribute( SDL_GL_RED_SIZE, sdlcolorbits );
		SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, sdlcolorbits );
		SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, sdlcolorbits );
		SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, tdepthbits );
		SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, tstencilbits );

		SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, samples ? 1 : 0 );
		SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, samples );

		if(r_stereoEnabled->integer)
		{
			glConfig.stereoEnabled = qtrue;
			SDL_GL_SetAttribute(SDL_GL_STEREO, 1);
		}
		else
		{
			glConfig.stereoEnabled = qfalse;
			SDL_GL_SetAttribute(SDL_GL_STEREO, 0);
		}
		
		SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

#if 0 // if multisampling is enabled on X11, this causes create window to fail.
		// If not allowing software GL, demand accelerated
		if( !r_allowSoftwareGL->integer )
			SDL_GL_SetAttribute( SDL_GL_ACCELERATED_VISUAL, 1 );
#endif

		// The renderer is the fixed-function OpenGL 1.x path, so ask for a
		// plain compatibility context and nothing newer.
		SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, 0 );
		SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 1 );
		SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 1 );

		if( ( SDL_window = SDL_CreateWindow( CLIENT_WINDOW_TITLE, x, y,
				glConfig.vidWidth, glConfig.vidHeight, flags ) ) == NULL )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
			continue;
		}

		if( ( SDL_glContext = SDL_GL_CreateContext( SDL_window ) ) == NULL )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GL_CreateContext failed: %s\n", SDL_GetError( ) );
			SDL_DestroyWindow( SDL_window );
			SDL_window = NULL;
			continue;
		}

		// SDL_CreateWindow was given a size in points; the GL viewport needs
		// pixels. These are the same number except on a HiDPI display, and
		// asking SDL rather than multiplying by a scale factor keeps this
		// correct for windowed, fullscreen and mixed-DPI setups alike.
		SDL_GL_GetDrawableSize( SDL_window, &glConfig.vidWidth, &glConfig.vidHeight );

		if( fullscreen )
		{
			// Exclusive fullscreen, but switched to the mode the display is
			// already in - the one request guaranteed to match.
			if( SDL_SetWindowDisplayMode( SDL_window, &desktopMode ) < 0 )
				ri.Printf( PRINT_ALL, "SDL_SetWindowDisplayMode failed: %s\n",
						SDL_GetError( ) );
			glConfig.displayFrequency = desktopMode.refresh_rate;
		}

		if( icon )
			SDL_SetWindowIcon( SDL_window, icon );

		SDL_ShowCursor( 0 );

		qglClearColor( 0, 0, 0, 1 );
		qglClear( GL_COLOR_BUFFER_BIT );
		SDL_GL_SwapWindow( SDL_window );

		// SDL_GL_SWAP_CONTROL became a standalone call in SDL2
		if( SDL_GL_SetSwapInterval( r_swapInterval->integer ) == -1 )
			ri.Printf( PRINT_DEVELOPER, "SDL_GL_SetSwapInterval failed: %s\n", SDL_GetError( ) );

		SDL_GL_GetAttribute( SDL_GL_RED_SIZE,     &realcolorbits[0] );
		SDL_GL_GetAttribute( SDL_GL_GREEN_SIZE,   &realcolorbits[1] );
		SDL_GL_GetAttribute( SDL_GL_BLUE_SIZE,    &realcolorbits[2] );
		SDL_GL_GetAttribute( SDL_GL_DEPTH_SIZE,   &glConfig.depthBits );
		SDL_GL_GetAttribute( SDL_GL_STENCIL_SIZE, &glConfig.stencilBits );

		glConfig.colorBits = realcolorbits[0] + realcolorbits[1] + realcolorbits[2];

		ri.Printf( PRINT_ALL, "Using %d/%d/%d Color bits, %d depth, %d stencil display.\n",
				realcolorbits[0], realcolorbits[1], realcolorbits[2],
				glConfig.depthBits, glConfig.stencilBits );
		break;
	}

	if( icon )
		SDL_FreeSurface( icon );

	if (!SDL_window)
	{
		ri.Printf( PRINT_ALL, "Couldn't get a visual\n" );
		return RSERR_INVALID_MODE;
	}

	SDL_ShowWindow( SDL_window );

	// What we asked for is not always what we got, and a request SDL declined
	// leaves a plain window behind. Report the window we actually have.
	glConfig.isFullscreen = !!( SDL_GetWindowFlags( SDL_window ) &
			SDL_WINDOW_FULLSCREEN_DESKTOP );

	// macOS enters fullscreen asynchronously, so the drawable measured just
	// after SDL_CreateWindow is still the pre-transition size. Rendering into a
	// viewport that does not match the real buffer leaves black margins down
	// two sides and pushes the top of the picture off the screen, so wait for
	// the size to stop moving before believing it.
	if( glConfig.isFullscreen )
	{
		int w = glConfig.vidWidth, h = glConfig.vidHeight;
		int settled = 0, i;

		for( i = 0; i < 200 && settled < 5; i++ )
		{
			int nw, nh;

			SDL_PumpEvents( );
			SDL_Delay( 5 );
			SDL_GL_GetDrawableSize( SDL_window, &nw, &nh );

			if( nw == w && nh == h )
			{
				settled++;
			}
			else
			{
				w = nw;
				h = nh;
				settled = 0;
			}
		}

		{
			SDL_Rect bounds;
			int ww = 0, wh = 0;

			SDL_GetWindowSize( SDL_window, &ww, &wh );
			if( SDL_GetDisplayBounds( 0, &bounds ) == 0 )
				ri.Printf( PRINT_ALL, "...display %d x %d pts, window %d x %d pts, drawable %d x %d px\n",
						bounds.w, bounds.h, ww, wh, w, h );
		}

		if( w != glConfig.vidWidth || h != glConfig.vidHeight )
			ri.Printf( PRINT_ALL, "...fullscreen settled at %d x %d\n", w, h );

		glConfig.vidWidth = w;
		glConfig.vidHeight = h;
		glConfig.windowAspect = (float)w / (float)h;
	}

	GLimp_DetectAvailableModes();

	glstring = (char *) qglGetString (GL_RENDERER);
	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", glstring );

	return RSERR_OK;
}

/*
===============
GLimp_StartDriverAndSetMode
===============
*/
static qboolean GLimp_StartDriverAndSetMode(int mode, qboolean fullscreen, qboolean noborder)
{
	rserr_t err;

	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		const char *driverName;

		if (SDL_Init(SDL_INIT_VIDEO) != 0)
		{
			ri.Printf( PRINT_ALL, "SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n",
					SDL_GetError());
			return qfalse;
		}

		driverName = SDL_GetCurrentVideoDriver( );
		ri.Printf( PRINT_ALL, "SDL using driver \"%s\"\n", driverName ? driverName : "(unknown)" );
		ri.Cvar_Set( "r_sdlDriver", driverName ? driverName : "" );
	}

	if (fullscreen && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
	{
		ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
		ri.Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = qfalse;
	}
	
	err = GLimp_SetMode(mode, fullscreen, noborder);

	switch ( err )
	{
		case RSERR_INVALID_FULLSCREEN:
			ri.Printf( PRINT_ALL, "...WARNING: fullscreen unavailable in this mode\n" );
			return qfalse;
		case RSERR_INVALID_MODE:
			ri.Printf( PRINT_ALL, "...WARNING: could not set the given mode (%d)\n", mode );
			return qfalse;
		default:
			break;
	}

	return qtrue;
}

static qboolean GLimp_HaveExtension(const char *ext)
{
	const char *ptr = Q_stristr( glConfig.extensions_string, ext );
	if (ptr == NULL)
		return qfalse;
	ptr += strlen(ext);
	return ((*ptr == ' ') || (*ptr == '\0'));  // verify it's complete string.
}


/*
===============
GLimp_InitExtensions
===============
*/
static void GLimp_InitExtensions( void )
{
	if ( !r_allowExtensions->integer )
	{
		ri.Printf( PRINT_ALL, "* IGNORING OPENGL EXTENSIONS *\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "Initializing OpenGL extensions\n" );

	glConfig.textureCompression = TC_NONE;

	// GL_EXT_texture_compression_s3tc
	if ( GLimp_HaveExtension( "GL_ARB_texture_compression" ) &&
	     GLimp_HaveExtension( "GL_EXT_texture_compression_s3tc" ) )
	{
		if ( r_ext_compressed_textures->value )
		{
			glConfig.textureCompression = TC_S3TC_ARB;
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_compression_s3tc\n" );
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_compression_s3tc\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_compression_s3tc not found\n" );
	}

	// GL_S3_s3tc ... legacy extension before GL_EXT_texture_compression_s3tc.
	if (glConfig.textureCompression == TC_NONE)
	{
		if ( GLimp_HaveExtension( "GL_S3_s3tc" ) )
		{
			if ( r_ext_compressed_textures->value )
			{
				glConfig.textureCompression = TC_S3TC;
				ri.Printf( PRINT_ALL, "...using GL_S3_s3tc\n" );
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_S3_s3tc\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_S3_s3tc not found\n" );
		}
	}


	// GL_EXT_texture_env_add
	glConfig.textureEnvAddAvailable = qfalse;
	if ( GLimp_HaveExtension( "EXT_texture_env_add" ) )
	{
		if ( r_ext_texture_env_add->integer )
		{
			glConfig.textureEnvAddAvailable = qtrue;
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_env_add\n" );
		}
		else
		{
			glConfig.textureEnvAddAvailable = qfalse;
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_env_add\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_env_add not found\n" );
	}

	// GL_ARB_multitexture
	qglMultiTexCoord2fARB = NULL;
	qglActiveTextureARB = NULL;
	qglClientActiveTextureARB = NULL;
	if ( GLimp_HaveExtension( "GL_ARB_multitexture" ) )
	{
		if ( r_ext_multitexture->value )
		{
			qglMultiTexCoord2fARB = SDL_GL_GetProcAddress( "glMultiTexCoord2fARB" );
			qglActiveTextureARB = SDL_GL_GetProcAddress( "glActiveTextureARB" );
			qglClientActiveTextureARB = SDL_GL_GetProcAddress( "glClientActiveTextureARB" );

			if ( qglActiveTextureARB )
			{
				GLint glint = 0;
				qglGetIntegerv( GL_MAX_TEXTURE_UNITS_ARB, &glint );
				glConfig.numTextureUnits = (int) glint;
				if ( glConfig.numTextureUnits > 1 )
				{
					ri.Printf( PRINT_ALL, "...using GL_ARB_multitexture\n" );
				}
				else
				{
					qglMultiTexCoord2fARB = NULL;
					qglActiveTextureARB = NULL;
					qglClientActiveTextureARB = NULL;
					ri.Printf( PRINT_ALL, "...not using GL_ARB_multitexture, < 2 texture units\n" );
				}
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_ARB_multitexture\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_ARB_multitexture not found\n" );
	}

	// GL_EXT_compiled_vertex_array
	if ( GLimp_HaveExtension( "GL_EXT_compiled_vertex_array" ) )
	{
		if ( r_ext_compiled_vertex_array->value )
		{
			ri.Printf( PRINT_ALL, "...using GL_EXT_compiled_vertex_array\n" );
			qglLockArraysEXT = ( void ( APIENTRY * )( GLint, GLint ) ) SDL_GL_GetProcAddress( "glLockArraysEXT" );
			qglUnlockArraysEXT = ( void ( APIENTRY * )( void ) ) SDL_GL_GetProcAddress( "glUnlockArraysEXT" );
			if (!qglLockArraysEXT || !qglUnlockArraysEXT)
			{
				ri.Error (ERR_FATAL, "bad getprocaddress");
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_compiled_vertex_array\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_compiled_vertex_array not found\n" );
	}

	textureFilterAnisotropic = qfalse;
	if ( GLimp_HaveExtension( "GL_EXT_texture_filter_anisotropic" ) )
	{
		if ( r_ext_texture_filter_anisotropic->integer ) {
			qglGetIntegerv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, (GLint *)&maxAnisotropy );
			if ( maxAnisotropy <= 0 ) {
				ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not properly supported!\n" );
				maxAnisotropy = 0;
			}
			else
			{
				ri.Printf( PRINT_ALL, "...using GL_EXT_texture_filter_anisotropic (max: %i)\n", maxAnisotropy );
				textureFilterAnisotropic = qtrue;
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_filter_anisotropic\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not found\n" );
	}

	vertexShaders = qfalse;
	if ( GLimp_HaveExtension( "GL_ARB_shader_objects" )
	     && GLimp_HaveExtension( "GL_ARB_fragment_shader" )
	     && GLimp_HaveExtension( "GL_ARB_vertex_shader" )
	     && GLimp_HaveExtension( "GL_ARB_shading_language_100" ) )
	{
		if ( r_ext_vertex_shader->integer ) {
		  ri.Printf( PRINT_ALL, "...using GL_ARB_vertex_shader\n" );

		  qglDeleteObjectARB = (GLvoid (APIENTRYP)(GLhandleARB)) SDL_GL_GetProcAddress("glDeleteObjectARB");
		  qglGetHandleARB = (GLhandleARB (APIENTRYP)(GLenum)) SDL_GL_GetProcAddress("glGetHandleARB");
		  qglDetachObjectARB = (GLvoid (APIENTRYP)(GLhandleARB, GLhandleARB)) SDL_GL_GetProcAddress("glDetachObjectARB");
		  qglCreateShaderObjectARB = (GLhandleARB (APIENTRYP)(GLenum)) SDL_GL_GetProcAddress("glCreateShaderObjectARB");
		  qglShaderSourceARB = (GLvoid (APIENTRYP)(GLhandleARB, GLsizei, const GLcharARB **,
							   const GLint *)) SDL_GL_GetProcAddress("glShaderSourceARB");
		  qglCompileShaderARB = (GLvoid (APIENTRYP)(GLhandleARB)) SDL_GL_GetProcAddress("glCompileShaderARB");
		  qglCreateProgramObjectARB = (GLhandleARB (APIENTRYP)(void)) SDL_GL_GetProcAddress("glCreateProgramObjectARB");
		  qglAttachObjectARB = (GLvoid (APIENTRYP)(GLhandleARB, GLhandleARB)) SDL_GL_GetProcAddress("glAttachObjectARB");
		  qglLinkProgramARB = (GLvoid (APIENTRYP)(GLhandleARB)) SDL_GL_GetProcAddress("glLinkProgramARB");
		  qglUseProgramObjectARB = (GLvoid (APIENTRYP)(GLhandleARB)) SDL_GL_GetProcAddress("glUseProgramObjectARB");
		  qglValidateProgramARB = (GLvoid (APIENTRYP)(GLhandleARB)) SDL_GL_GetProcAddress("glValidateProgramARB");
		  qglUniform1fARB = (GLvoid (APIENTRYP)(GLint, GLfloat)) SDL_GL_GetProcAddress("glUniform1fARB");
		  qglUniform2fARB = (GLvoid (APIENTRYP)(GLint, GLfloat, GLfloat)) SDL_GL_GetProcAddress("glUniform2fARB");
		  qglUniform3fARB = (GLvoid (APIENTRYP)(GLint, GLfloat, GLfloat, GLfloat)) SDL_GL_GetProcAddress("glUniform3fARB");
		  qglUniform4fARB = (GLvoid (APIENTRYP)(GLint, GLfloat, GLfloat, GLfloat, GLfloat)) SDL_GL_GetProcAddress("glUniform4fARB");
		  qglUniform1iARB = (GLvoid (APIENTRYP)(GLint, GLint)) SDL_GL_GetProcAddress("glUniform1iARB");
		  qglUniform2iARB = (GLvoid (APIENTRYP)(GLint, GLint, GLint)) SDL_GL_GetProcAddress("glUniform2iARB");
		  qglUniform3iARB = (GLvoid (APIENTRYP)(GLint, GLint, GLint, GLint)) SDL_GL_GetProcAddress("glUniform3iARB");
		  qglUniform4iARB = (GLvoid (APIENTRYP)(GLint, GLint, GLint, GLint, GLint)) SDL_GL_GetProcAddress("glUniform4iARB");
		  qglUniform1fvARB = (GLvoid (APIENTRYP)(GLint, GLsizei, const GLfloat *)) SDL_GL_GetProcAddress("glUniform1fvARB");
		  qglUniform2fvARB = (GLvoid (APIENTRYP)(GLint, GLsizei, const GLfloat *)) SDL_GL_GetProcAddress("glUniform2fvARB");
		  qglUniform3fvARB = (GLvoid (APIENTRYP)(GLint, GLsizei, const GLfloat *)) SDL_GL_GetProcAddress("glUniform3fvARB");
		  qglUniform4fvARB = (GLvoid (APIENTRYP)(GLint, GLsizei, const GLfloat *)) SDL_GL_GetProcAddress("glUniform4fvARB");
		  qglUniform1ivARB = (GLvoid (APIENTRYP)(GLint, GLsizei, const GLint *)) SDL_GL_GetProcAddress("glUniform1ivARB");
		  qglUniform2ivARB = (GLvoid (APIENTRYP)(GLint, GLsizei, const GLint *)) SDL_GL_GetProcAddress("glUniform2ivARB");
		  qglUniform3ivARB = (GLvoid (APIENTRYP)(GLint, GLsizei, const GLint *)) SDL_GL_GetProcAddress("glUniform3ivARB");
		  qglUniform4ivARB = (GLvoid (APIENTRYP)(GLint, GLsizei, const GLint *)) SDL_GL_GetProcAddress("glUniform4ivARB");
		  qglUniformMatrix2fvARB = (GLvoid (APIENTRYP)(GLint, GLsizei, GLboolean, const GLfloat *)) SDL_GL_GetProcAddress("glUniformMatrix2fvARB");
		  qglUniformMatrix3fvARB = (GLvoid (APIENTRYP)(GLint, GLsizei, GLboolean, const GLfloat *)) SDL_GL_GetProcAddress("glUniformMatrix3fvARB");
		  qglUniformMatrix4fvARB = (GLvoid (APIENTRYP)(GLint, GLsizei, GLboolean, const GLfloat *)) SDL_GL_GetProcAddress("glUniformMatrix4fvARB");
		  qglGetObjectParameterfvARB = (GLvoid (APIENTRYP)(GLhandleARB, GLenum, GLfloat *)) SDL_GL_GetProcAddress("glGetObjectParameterfvARB");
		  qglGetObjectParameterivARB = (GLvoid (APIENTRYP)(GLhandleARB, GLenum, GLint *)) SDL_GL_GetProcAddress("glGetObjectParameterivARB");
		  qglGetInfoLogARB = (GLvoid (APIENTRYP)(GLhandleARB, GLsizei, GLsizei *, GLcharARB *)) SDL_GL_GetProcAddress("glGetInfoLogARB");
		  qglGetAttachedObjectsARB = (GLvoid (APIENTRYP)(GLhandleARB, GLsizei, GLsizei *, GLhandleARB *)) SDL_GL_GetProcAddress("glGetAttachedObjectsARB");
		  qglGetUniformLocationARB = (GLint (APIENTRYP)(GLhandleARB, const GLcharARB *)) SDL_GL_GetProcAddress("glGetUniformLocationARB");
		  qglGetActiveUniformARB = (GLvoid (APIENTRYP)(GLhandleARB, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLcharARB *)) SDL_GL_GetProcAddress("glGetActiveUniformARB");
		  qglGetUniformfvARB = (GLvoid (APIENTRYP)(GLhandleARB, GLint, GLfloat *)) SDL_GL_GetProcAddress("glGetUniformfvARB");
		  qglGetUniformivARB = (GLvoid (APIENTRYP)(GLhandleARB, GLint, GLint *)) SDL_GL_GetProcAddress("glGetUniformivARB");
		  qglGetShaderSourceARB = (GLvoid (APIENTRYP)(GLhandleARB, GLsizei, GLsizei *, GLcharARB *)) SDL_GL_GetProcAddress("glGetShaderSourceARB");

		  qglVertexAttrib1fARB = (GLvoid (APIENTRYP)(GLuint, GLfloat)) SDL_GL_GetProcAddress("glVertexAttrib1fARB");
		  qglVertexAttrib1sARB = (GLvoid (APIENTRYP)(GLuint, GLshort)) SDL_GL_GetProcAddress("glVertexAttrib1sARB");
		  qglVertexAttrib1dARB = (GLvoid (APIENTRYP)(GLuint, GLdouble)) SDL_GL_GetProcAddress("glVertexAttrib1dARB");
		  qglVertexAttrib2fARB = (GLvoid (APIENTRYP)(GLuint, GLfloat, GLfloat)) SDL_GL_GetProcAddress("glVertexAttrib2fARB");
		  qglVertexAttrib2sARB = (GLvoid (APIENTRYP)(GLuint, GLshort, GLshort)) SDL_GL_GetProcAddress("glVertexAttrib2sARB");
		  qglVertexAttrib2dARB = (GLvoid (APIENTRYP)(GLuint, GLdouble, GLdouble)) SDL_GL_GetProcAddress("glVertexAttrib2dARB");
		  qglVertexAttrib3fARB = (GLvoid (APIENTRYP)(GLuint, GLfloat, GLfloat, GLfloat)) SDL_GL_GetProcAddress("glVertexAttrib3fARB");
		  qglVertexAttrib3sARB = (GLvoid (APIENTRYP)(GLuint, GLshort, GLshort, GLshort)) SDL_GL_GetProcAddress("glVertexAttrib3sARB");
		  qglVertexAttrib3dARB = (GLvoid (APIENTRYP)(GLuint, GLdouble, GLdouble, GLdouble)) SDL_GL_GetProcAddress("glVertexAttrib3dARB");
		  qglVertexAttrib4fARB = (GLvoid (APIENTRYP)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) SDL_GL_GetProcAddress("glVertexAttrib4fARB");
		  qglVertexAttrib4sARB = (GLvoid (APIENTRYP)(GLuint, GLshort, GLshort, GLshort, GLshort)) SDL_GL_GetProcAddress("glVertexAttrib4sARB");
		  qglVertexAttrib4dARB = (GLvoid (APIENTRYP)(GLuint, GLdouble, GLdouble, GLdouble, GLdouble)) SDL_GL_GetProcAddress("glVertexAttrib4dARB");
		  qglVertexAttrib4NubARB = (GLvoid (APIENTRYP)(GLuint, GLubyte, GLubyte, GLubyte, GLubyte)) SDL_GL_GetProcAddress("glVertexAttrib4NubARB");
		  qglVertexAttrib1fvARB = (GLvoid (APIENTRYP)(GLuint, GLfloat *)) SDL_GL_GetProcAddress("glVertexAttrib1fvARB");
		  qglVertexAttrib1svARB = (GLvoid (APIENTRYP)(GLuint, GLshort *)) SDL_GL_GetProcAddress("glVertexAttrib1svARB");
		  qglVertexAttrib1dvARB = (GLvoid (APIENTRYP)(GLuint, GLdouble *)) SDL_GL_GetProcAddress("glVertexAttrib1dvARB");
		  qglVertexAttrib2fvARB = (GLvoid (APIENTRYP)(GLuint, GLfloat *)) SDL_GL_GetProcAddress("glVertexAttrib2fvARB");
		  qglVertexAttrib2svARB = (GLvoid (APIENTRYP)(GLuint, GLshort *)) SDL_GL_GetProcAddress("glVertexAttrib2svARB");
		  qglVertexAttrib2dvARB = (GLvoid (APIENTRYP)(GLuint, GLdouble *)) SDL_GL_GetProcAddress("glVertexAttrib2dvARB");
		  qglVertexAttrib3fvARB = (GLvoid (APIENTRYP)(GLuint, GLfloat *)) SDL_GL_GetProcAddress("glVertexAttrib3fvARB");
		  qglVertexAttrib3svARB = (GLvoid (APIENTRYP)(GLuint, GLshort *)) SDL_GL_GetProcAddress("glVertexAttrib3svARB");
		  qglVertexAttrib3dvARB = (GLvoid (APIENTRYP)(GLuint, GLdouble *)) SDL_GL_GetProcAddress("glVertexAttrib3dvARB");
		  qglVertexAttrib4fvARB = (GLvoid (APIENTRYP)(GLuint, GLfloat *)) SDL_GL_GetProcAddress("glVertexAttrib4fvARB");
		  qglVertexAttrib4svARB = (GLvoid (APIENTRYP)(GLuint, GLshort *)) SDL_GL_GetProcAddress("glVertexAttrib4svARB");
		  qglVertexAttrib4dvARB = (GLvoid (APIENTRYP)(GLuint, GLdouble *)) SDL_GL_GetProcAddress("glVertexAttrib4dvARB");
		  qglVertexAttrib4ivARB = (GLvoid (APIENTRYP)(GLuint, GLint *)) SDL_GL_GetProcAddress("glVertexAttrib4ivARB");
		  qglVertexAttrib4bvARB = (GLvoid (APIENTRYP)(GLuint, GLbyte *)) SDL_GL_GetProcAddress("glVertexAttrib4bvARB");
		  qglVertexAttrib4ubvARB = (GLvoid (APIENTRYP)(GLuint, GLubyte *)) SDL_GL_GetProcAddress("glVertexAttrib4ubvARB");
		  qglVertexAttrib4usvARB = (GLvoid (APIENTRYP)(GLuint, GLushort *)) SDL_GL_GetProcAddress("glVertexAttrib4usvARB");
		  qglVertexAttrib4uivARB = (GLvoid (APIENTRYP)(GLuint, GLuint *)) SDL_GL_GetProcAddress("glVertexAttrib4uivARB");
		  qglVertexAttrib4NbvARB = (GLvoid (APIENTRYP)(GLuint, const GLbyte *)) SDL_GL_GetProcAddress("glVertexAttrib4NbvARB");
		  qglVertexAttrib4NsvARB = (GLvoid (APIENTRYP)(GLuint, const GLshort *)) SDL_GL_GetProcAddress("glVertexAttrib4NsvARB");
		  qglVertexAttrib4NivARB = (GLvoid (APIENTRYP)(GLuint, const GLint *)) SDL_GL_GetProcAddress("glVertexAttrib4NivARB");
		  qglVertexAttrib4NubvARB = (GLvoid (APIENTRYP)(GLuint, const GLubyte *)) SDL_GL_GetProcAddress("glVertexAttrib4NubvARB");
		  qglVertexAttrib4NusvARB = (GLvoid (APIENTRYP)(GLuint, const GLushort *)) SDL_GL_GetProcAddress("glVertexAttrib4NusvARB");
		  qglVertexAttrib4NuivARB = (GLvoid (APIENTRYP)(GLuint, const GLuint *)) SDL_GL_GetProcAddress("glVertexAttrib4NuivARB");
		  qglVertexAttribPointerARB = (GLvoid (APIENTRYP)(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid *)) SDL_GL_GetProcAddress("glVertexAttribPointerARB");
		  qglEnableVertexAttribArrayARB = (GLvoid (APIENTRYP)(GLuint)) SDL_GL_GetProcAddress("glEnableVertexAttribArrayARB");
		  qglDisableVertexAttribArrayARB = (GLvoid (APIENTRYP)(GLuint)) SDL_GL_GetProcAddress("glDisableVertexAttribArrayARB");
		  qglBindAttribLocationARB = (GLvoid (APIENTRYP)(GLhandleARB, GLuint, const GLcharARB *)) SDL_GL_GetProcAddress("glBindAttribLocationARB");
		  qglGetActiveAttribARB = (GLvoid (APIENTRYP)(GLhandleARB, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLcharARB *)) SDL_GL_GetProcAddress("glGetActiveAttribARB");
		  qglGetAttribLocationARB = (GLint (APIENTRYP)(GLhandleARB, const GLcharARB *)) SDL_GL_GetProcAddress("glGetAttribLocationARB");
		  qglGetVertexAttribdvARB = (GLvoid (APIENTRYP)(GLuint, GLenum, GLdouble *)) SDL_GL_GetProcAddress("glGetVertexAttribdvARB");
		  qglGetVertexAttribfvARB = (GLvoid (APIENTRYP)(GLuint, GLenum, GLfloat *)) SDL_GL_GetProcAddress("glGetVertexAttribfvARB");
		  qglGetVertexAttribivARB = (GLvoid (APIENTRYP)(GLuint, GLenum, GLint *)) SDL_GL_GetProcAddress("glGetVertexAAttribivARB");
		  qglGetVertexAttribPointervARB = (GLvoid (APIENTRYP)(GLuint, GLenum, GLvoid **)) SDL_GL_GetProcAddress("glGetVertexAttribPointervARB");
		  vertexShaders = qtrue;
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_ARB_vertex_shader\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_ARB_vertex_shader not found\n" );
	}

	framebufferObjects = qfalse;
	if ( GLimp_HaveExtension( "GL_ARB_framebuffer_object" ) )
	{
		qglGenFramebuffers = (GLvoid (APIENTRYP)(GLsizei, GLuint *)) SDL_GL_GetProcAddress("glGenFramebuffers");
		qglDeleteFramebuffers = (GLvoid (APIENTRYP)(GLsizei, const GLuint *)) SDL_GL_GetProcAddress("glDeleteFramebuffers");
		qglBindFramebuffer = (GLvoid (APIENTRYP)(GLenum, GLuint)) SDL_GL_GetProcAddress("glBindFramebuffer");
		qglFramebufferTexture2D = (GLvoid (APIENTRYP)(GLenum, GLenum, GLenum, GLuint, GLint)) SDL_GL_GetProcAddress("glFramebufferTexture2D");
		qglCheckFramebufferStatus = (GLenum (APIENTRYP)(GLenum)) SDL_GL_GetProcAddress("glCheckFramebufferStatus");

		if ( qglGenFramebuffers && qglDeleteFramebuffers && qglBindFramebuffer
			&& qglFramebufferTexture2D && qglCheckFramebufferStatus )
		{
			framebufferObjects = qtrue;
			ri.Printf( PRINT_ALL, "...using GL_ARB_framebuffer_object\n" );
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_ARB_framebuffer_object advertised but incomplete\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_ARB_framebuffer_object not found\n" );
	}
}

#define R_MODE_FALLBACK 3 // 640 * 480

/*
===============
GLimp_ProbeGamma

Whether a gamma ramp set on this window reaches the display.

Asking SDL to set one and taking a non-negative return for an answer is not
enough: on a compositing desktop the call is accepted and then ignored, and the
renderer reads that as permission to enable overbright - which halves every
lighting value and expects the ramp to double it back at scan-out. With no ramp
the halving is all that happens and the world renders dark and flat. Write a
ramp that is nothing like the identity, read it back, and believe the result.
===============
*/
static qboolean GLimp_ProbeGamma( void )
{
	Uint16	want[3][256], got[3][256];
	int		i;

	for( i = 0; i < 256; i++ )
	{
		// Half brightness: distinct from the identity ramp at every entry
		// except zero, so a stored-but-ignored ramp cannot pass by accident.
		want[0][i] = want[1][i] = want[2][i] = (Uint16)( ( i << 8 ) / 2 );
	}

	if( SDL_SetWindowGammaRamp( SDL_window, want[0], want[1], want[2] ) < 0 )
	{
		ri.Printf( PRINT_ALL, "...gamma ramp rejected: %s\n", SDL_GetError( ) );
		return qfalse;
	}

	if( SDL_GetWindowGammaRamp( SDL_window, got[0], got[1], got[2] ) < 0 ||
			memcmp( got, want, sizeof( got ) ) != 0 )
	{
		ri.Printf( PRINT_ALL, "...gamma ramp did not take, ignoring hardware gamma\n" );
		SDL_SetWindowBrightness( SDL_window, 1.0f );
		return qfalse;
	}

	SDL_SetWindowBrightness( SDL_window, 1.0f );

#ifdef MACOS_X
	// A ramp that survives the round trip proves only that SDL stored it. macOS
	// accepts one and does not apply it to the display - measured as fullscreen
	// coming out a quarter darker than windowed with overbright enabled. Report
	// no hardware gamma so the software path in R_LightScaleTexture is used,
	// which bakes both r_gamma and the overbright multiply into the textures.
	ri.Printf( PRINT_ALL, "...gamma ramp is stored but not applied here, using software gamma\n" );
	return qfalse;
#else
	return qtrue;
#endif
}

/*
===============
GLimp_Init

This routine is responsible for initializing the OS specific portions
of OpenGL
===============
*/
void GLimp_Init( void )
{
	r_allowSoftwareGL = ri.Cvar_Get( "r_allowSoftwareGL", "0", CVAR_LATCH );
	r_sdlDriver = ri.Cvar_Get( "r_sdlDriver", "", CVAR_ROM );
	r_allowResize = ri.Cvar_Get( "r_allowResize", "0", CVAR_ARCHIVE );
	r_allowHighDPI = ri.Cvar_Get( "r_allowHighDPI", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_centerWindow = ri.Cvar_Get( "r_centerWindow", "0", CVAR_ARCHIVE );

	if( ri.Cvar_VariableIntegerValue( "com_abnormalExit" ) )
	{
		ri.Cvar_Set( "r_mode", va( "%d", R_MODE_FALLBACK ) );
		ri.Cvar_Set( "r_fullscreen", "0" );
		ri.Cvar_Set( "r_centerWindow", "0" );
		ri.Cvar_Set( "com_abnormalExit", "0" );
	}

	// r_centerWindow is applied via the window position in GLimp_SetMode now;
	// SDL2 does not read SDL_VIDEO_CENTERED.

	ri.Sys_GLimpInit( );

	// Create the window and set up the context
	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, r_noborder->integer))
		goto success;

	// Try again, this time in a platform specific "safe mode"
	ri.Sys_GLimpSafeInit( );

	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, qfalse))
		goto success;

	// Finally, try the default screen resolution
	if( r_mode->integer != R_MODE_FALLBACK )
	{
		ri.Printf( PRINT_ALL, "Setting r_mode %d failed, falling back on r_mode %d\n",
				r_mode->integer, R_MODE_FALLBACK );
		ri.Cvar_Set("r_ext_multisample", "0");

		if(GLimp_StartDriverAndSetMode(R_MODE_FALLBACK, qfalse, qfalse))
			goto success;
	}

	// Nothing worked, give up
	ri.Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem" );

success:
	// This values force the UI to disable driver selection
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = GLimp_ProbeGamma( );

	// get our config strings
	Q_strncpyz( glConfig.vendor_string, (char *) qglGetString (GL_VENDOR), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (char *) qglGetString (GL_RENDERER), sizeof( glConfig.renderer_string ) );
	if (*glConfig.renderer_string && glConfig.renderer_string[strlen(glConfig.renderer_string) - 1] == '\n')
		glConfig.renderer_string[strlen(glConfig.renderer_string) - 1] = 0;
	Q_strncpyz( glConfig.version_string, (char *) qglGetString (GL_VERSION), sizeof( glConfig.version_string ) );
	Q_strncpyz( glConfig.extensions_string, (char *) qglGetString (GL_EXTENSIONS), sizeof( glConfig.extensions_string ) );

	// initialize extensions
	GLimp_InitExtensions( );

	ri.Cvar_Get( "r_availableModes", "", CVAR_ROM );

	// This depends on SDL_INIT_VIDEO, hence having it here
	ri.IN_Init( );
}


/*
===============
GLimp_Microseconds

Monotonic, and fine enough to time the parts of a frame against each other.
===============
*/
unsigned int GLimp_Microseconds( void )
{
	static Uint64	freq = 0;
	static Uint64	base = 0;
	Uint64			now;

	if ( !freq )
	{
		freq = SDL_GetPerformanceFrequency( );
		base = SDL_GetPerformanceCounter( );
	}

	now = SDL_GetPerformanceCounter( ) - base;

	// Seconds and remainder separately: the counter is nanosecond-scale here,
	// so scaling it whole would overflow after a few hours of run time. The
	// unsigned int wraps every ~71 minutes, which only ever spans a
	// subtraction of two samples taken in the same frame.
	return (unsigned int)( ( now / freq ) * 1000000ULL
		+ ( ( now % freq ) * 1000000ULL ) / freq );
}

/*
===============
GLimp_EndFrame

Responsible for doing a swapbuffers
===============
*/
void GLimp_EndFrame( void )
{
	// don't flip if drawing to front buffer
	if ( Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) != 0 )
	{
		SDL_GL_SwapWindow( SDL_window );
	}

	if( r_fullscreen->modified )
	{
		qboolean    fullscreen;
		qboolean    needToToggle;
		qboolean    sdlToggled = qfalse;

		// Find out the current state
		fullscreen = !!( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_FULLSCREEN );

		if( r_fullscreen->integer && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
		{
			ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
			ri.Cvar_Set( "r_fullscreen", "0" );
			r_fullscreen->modified = qfalse;
		}

		// Is the state we want different from the current state?
		needToToggle = !!r_fullscreen->integer != fullscreen;

		if( needToToggle )
		{
			// The window's display mode is already the desktop's, set in
			// GLimp_SetMode, so exclusive fullscreen matches by construction.
			sdlToggled = SDL_SetWindowFullscreen( SDL_window,
					r_fullscreen->integer ? SDL_WINDOW_FULLSCREEN : 0 ) >= 0;

			// SDL_SetWindowFullscreen didn't work, so do it the slow way
			if( !sdlToggled )
				ri.Cmd_ExecuteText(EXEC_APPEND, "vid_restart");

			ri.IN_Restart( );
		}

		r_fullscreen->modified = qfalse;
	}
}



#ifdef SMP
/*
===========================================================

SMP acceleration

===========================================================
*/

/*
 * I have no idea if this will even work...most platforms don't offer
 * thread-safe OpenGL libraries, and it looks like the original Linux
 * code counted on each thread claiming the GL context with glXMakeCurrent(),
 * which you can't currently do in SDL. We'll just have to hope for the best.
 */

static SDL_mutex *smpMutex = NULL;
static SDL_cond *renderCommandsEvent = NULL;
static SDL_cond *renderCompletedEvent = NULL;
static void (*glimpRenderThread)( void ) = NULL;
static SDL_Thread *renderThread = NULL;

/*
===============
GLimp_ShutdownRenderThread
===============
*/
static void GLimp_ShutdownRenderThread(void)
{
	if (smpMutex != NULL)
	{
		SDL_DestroyMutex(smpMutex);
		smpMutex = NULL;
	}

	if (renderCommandsEvent != NULL)
	{
		SDL_DestroyCond(renderCommandsEvent);
		renderCommandsEvent = NULL;
	}

	if (renderCompletedEvent != NULL)
	{
		SDL_DestroyCond(renderCompletedEvent);
		renderCompletedEvent = NULL;
	}

	glimpRenderThread = NULL;
}

/*
===============
GLimp_RenderThreadWrapper
===============
*/
static int GLimp_RenderThreadWrapper( void *arg )
{
	Com_Printf( "Render thread starting\n" );

	glimpRenderThread();

	GLimp_SetCurrentContext(NULL);

	Com_Printf( "Render thread terminating\n" );

	return 0;
}

/*
===============
GLimp_SpawnRenderThread
===============
*/
qboolean GLimp_SpawnRenderThread( void (*function)( void ) )
{
	static qboolean warned = qfalse;
	if (!warned)
	{
		Com_Printf("WARNING: You enable r_smp at your own risk!\n");
		warned = qtrue;
	}

#ifndef MACOS_X
	return qfalse;  /* better safe than sorry for now. */
#endif

	if (renderThread != NULL)  /* hopefully just a zombie at this point... */
	{
		Com_Printf("Already a render thread? Trying to clean it up...\n");
		SDL_WaitThread(renderThread, NULL);
		renderThread = NULL;
		GLimp_ShutdownRenderThread();
	}

	smpMutex = SDL_CreateMutex();
	if (smpMutex == NULL)
	{
		Com_Printf( "smpMutex creation failed: %s\n", SDL_GetError() );
		GLimp_ShutdownRenderThread();
		return qfalse;
	}

	renderCommandsEvent = SDL_CreateCond();
	if (renderCommandsEvent == NULL)
	{
		Com_Printf( "renderCommandsEvent creation failed: %s\n", SDL_GetError() );
		GLimp_ShutdownRenderThread();
		return qfalse;
	}

	renderCompletedEvent = SDL_CreateCond();
	if (renderCompletedEvent == NULL)
	{
		Com_Printf( "renderCompletedEvent creation failed: %s\n", SDL_GetError() );
		GLimp_ShutdownRenderThread();
		return qfalse;
	}

	glimpRenderThread = function;
	// SDL2 requires a thread name
	renderThread = SDL_CreateThread(GLimp_RenderThreadWrapper, "render thread", NULL);
	if ( renderThread == NULL )
	{
		ri.Printf( PRINT_ALL, "SDL_CreateThread() returned %s", SDL_GetError() );
		GLimp_ShutdownRenderThread();
		return qfalse;
	}

	// Deliberately left joinable rather than passed to SDL_DetachThread: the
	// re-entry path above reaps a leftover thread with SDL_WaitThread, which is
	// not allowed on a detached one.
	return qtrue;
}

static volatile void    *smpData = NULL;
static volatile qboolean smpDataReady;

/*
===============
GLimp_RendererSleep
===============
*/
void *GLimp_RendererSleep( void )
{
	void  *data = NULL;

	GLimp_SetCurrentContext(NULL);

	SDL_LockMutex(smpMutex);
	{
		smpData = NULL;
		smpDataReady = qfalse;

		// after this, the front end can exit GLimp_FrontEndSleep
		SDL_CondSignal(renderCompletedEvent);

		while ( !smpDataReady )
			SDL_CondWait(renderCommandsEvent, smpMutex);

		data = (void *)smpData;
	}
	SDL_UnlockMutex(smpMutex);

	GLimp_SetCurrentContext(SDL_glContext);

	return data;
}

/*
===============
GLimp_FrontEndSleep
===============
*/
void GLimp_FrontEndSleep( void )
{
	SDL_LockMutex(smpMutex);
	{
		while ( smpData )
			SDL_CondWait(renderCompletedEvent, smpMutex);
	}
	SDL_UnlockMutex(smpMutex);

	GLimp_SetCurrentContext(SDL_glContext);
}

/*
===============
GLimp_WakeRenderer
===============
*/
void GLimp_WakeRenderer( void *data )
{
	GLimp_SetCurrentContext(NULL);

	SDL_LockMutex(smpMutex);
	{
		assert( smpData == NULL );
		smpData = data;
		smpDataReady = qtrue;

		// after this, the renderer can continue through GLimp_RendererSleep
		SDL_CondSignal(renderCommandsEvent);
	}
	SDL_UnlockMutex(smpMutex);
}

#else

// No SMP - stubs
void GLimp_RenderThreadWrapper( void *arg )
{
}

qboolean GLimp_SpawnRenderThread( void (*function)( void ) )
{
	ri.Printf( PRINT_WARNING, "ERROR: SMP support was disabled at compile time\n");
	return qfalse;
}

void *GLimp_RendererSleep( void )
{
	return NULL;
}

void GLimp_FrontEndSleep( void )
{
}

void GLimp_WakeRenderer( void *data )
{
}

#endif
