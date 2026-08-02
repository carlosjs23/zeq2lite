/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// tr_bloom.c: 2D lighting post process effect

#include "tr_local.h"
#include "tr_bloomsize.h"


static cvar_t *r_bloom;
static cvar_t *r_bloom_sample_size;
//static cvar_t *r_bloom_fast_sample;
static cvar_t *r_bloom_alpha;
static cvar_t *r_bloom_darken;
static cvar_t *r_bloom_intensity;
static cvar_t *r_bloom_diamond_size;
static cvar_t *r_bloom_debug;

/* 
============================================================================== 
 
						LIGHT BLOOMS
 
============================================================================== 
*/ 

static float Diamond8x[8][8] =
{ 
	{ 0.0f, 0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f, },
	{ 0.0f, 0.0f, 0.2f, 0.3f, 0.3f, 0.2f, 0.0f, 0.0f, },
	{ 0.0f, 0.2f, 0.4f, 0.6f, 0.6f, 0.4f, 0.2f, 0.0f, },
	{ 0.1f, 0.3f, 0.6f, 0.9f, 0.9f, 0.6f, 0.3f, 0.1f, },
	{ 0.1f, 0.3f, 0.6f, 0.9f, 0.9f, 0.6f, 0.3f, 0.1f, },
	{ 0.0f, 0.2f, 0.4f, 0.6f, 0.6f, 0.4f, 0.2f, 0.0f, },
	{ 0.0f, 0.0f, 0.2f, 0.3f, 0.3f, 0.2f, 0.0f, 0.0f, },
	{ 0.0f, 0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f  }
};

static float Diamond6x[6][6] =
{ 
	{ 0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, },
	{ 0.0f, 0.3f, 0.5f, 0.5f, 0.3f, 0.0f, }, 
	{ 0.1f, 0.5f, 0.9f, 0.9f, 0.5f, 0.1f, },
	{ 0.1f, 0.5f, 0.9f, 0.9f, 0.5f, 0.1f, },
	{ 0.0f, 0.3f, 0.5f, 0.5f, 0.3f, 0.0f, },
	{ 0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f  }
};

static float Diamond4x[4][4] =
{  
	{ 0.3f, 0.4f, 0.4f, 0.3f, },
	{ 0.4f, 0.9f, 0.9f, 0.4f, },
	{ 0.4f, 0.9f, 0.9f, 0.4f, },
	{ 0.3f, 0.4f, 0.4f, 0.3f  }
};

static struct {
	struct {
		image_t	*texture;
		int		width, height;
		float	readW, readH;
	} effect;
	struct {
		image_t	*texture;
		int		width, height;
		float	readW, readH;
	} screen;
	struct {
		int		width, height;
		image_t	*texture;		// accumulator, framebuffer-object path only
	} work;
	image_t		*result;		// whichever texture ended up holding the effect
	GLuint		fbo;
	qboolean	useFBO;
	int			targetHeight;	// ortho height of whatever is being rendered into
	qboolean started;
} bloom;


static void ID_INLINE R_Bloom_Quad( int width, int height, float texX, float texY, float texWidth, float texHeight ) {
	int x = 0;
	int y = 0;
	x = 0;
	y += bloom.targetHeight - height;
	width += x;
	height += y;
	
	texWidth += texX;
	texHeight += texY;

	qglBegin( GL_QUADS );							
	qglTexCoord2f(	texX,						texHeight	);	
	qglVertex2f(	x,							y	);

	qglTexCoord2f(	texX,						texY	);				
	qglVertex2f(	x,							height	);	

	qglTexCoord2f(	texWidth,					texY	);				
	qglVertex2f(	width,						height	);	

	qglTexCoord2f(	texWidth,					texHeight	);	
	qglVertex2f(	width,						y	);				
	qglEnd ();
}


/*
=================
R_Bloom_InitTextures
=================
*/
static void R_Bloom_InitTextures( void )
{
	byte	*data;

	// Sizes live in tr_bloomsize.c so they can be tested without a GL context.
	{
		bloomSizes_t sizes;

		if( !R_BloomSizes( &sizes, glConfig.vidWidth, glConfig.vidHeight,
				r_bloom_sample_size->integer, glConfig.maxTextureSize ) ) {
			ri.Cvar_Set( "r_bloom", "0" );
			Com_Printf( S_COLOR_YELLOW"WARNING: 'R_InitBloomTextures' too high resolution for light bloom, effect disabled\n" );
			return;
		}

		bloom.screen.width  = sizes.screenWidth;
		bloom.screen.height = sizes.screenHeight;
		bloom.screen.readW  = sizes.screenReadW;
		bloom.screen.readH  = sizes.screenReadH;
		bloom.effect.width  = sizes.effectWidth;
		bloom.effect.height = sizes.effectHeight;
		bloom.effect.readW  = sizes.effectReadW;
		bloom.effect.readH  = sizes.effectReadH;
		bloom.work.width    = sizes.workWidth;
		bloom.work.height   = sizes.workHeight;
	}

	data = ri.Hunk_AllocateTempMemory( bloom.screen.width * bloom.screen.height * 4 );
	Com_Memset( data, 0, bloom.screen.width * bloom.screen.height * 4 );
	bloom.screen.texture = R_CreateImage( "***bloom screen texture***", data, bloom.screen.width, bloom.screen.height, qfalse, qfalse, GL_CLAMP_TO_EDGE);
	ri.Hunk_FreeTempMemory( data );

	data = ri.Hunk_AllocateTempMemory( bloom.effect.width * bloom.effect.height * 4 );
	Com_Memset( data, 0, bloom.effect.width * bloom.effect.height * 4 );
	bloom.effect.texture = R_CreateImage( "***bloom effect texture***", data, bloom.effect.width, bloom.effect.height, qfalse, qfalse, GL_CLAMP_TO_EDGE);
	ri.Hunk_FreeTempMemory( data );

	// With render-to-texture the passes need somewhere to accumulate that is
	// not the texture they are sampling, so the effect gets a second buffer of
	// the same size and the two alternate. Without it the accumulator is the
	// backbuffer and one texture is enough.
	bloom.useFBO = qfalse;
	if( framebufferObjects ) {
		data = ri.Hunk_AllocateTempMemory( bloom.effect.width * bloom.effect.height * 4 );
		Com_Memset( data, 0, bloom.effect.width * bloom.effect.height * 4 );
		bloom.work.texture = R_CreateImage( "***bloom work texture***", data, bloom.effect.width, bloom.effect.height, qfalse, qfalse, GL_CLAMP_TO_EDGE);
		ri.Hunk_FreeTempMemory( data );

		qglGenFramebuffers( 1, &bloom.fbo );
		qglBindFramebuffer( GL_FRAMEBUFFER, bloom.fbo );
		qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloom.work.texture->texnum, 0 );
		if( qglCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE ) {
			bloom.useFBO = qtrue;
		} else {
			Com_Printf( S_COLOR_YELLOW"WARNING: 'R_Bloom_InitTextures' incomplete framebuffer, falling back to backbuffer copies\n" );
			qglDeleteFramebuffers( 1, &bloom.fbo );
			bloom.fbo = 0;
		}
		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	}

	bloom.result = bloom.effect.texture;
	bloom.targetHeight = glConfig.vidHeight;
	bloom.started = qtrue;
}

/*
=================
R_Bloom_RenderTo

Aim the passes that follow at a texture. With framebuffer objects that is a real
render target. Without them there is nowhere to draw but the backbuffer, so the
passes land in its bottom-left corner and R_Bloom_Snapshot copies them out - the
way every step of this effect used to work.
=================
*/
static void R_Bloom_RenderTo( image_t *tex )
{
	if( !bloom.useFBO ) {
		bloom.targetHeight = glConfig.vidHeight;
		return;
	}

	qglBindFramebuffer( GL_FRAMEBUFFER, bloom.fbo );
	qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->texnum, 0 );
	// Draw into the same bottom-left corner of the texture that the readW/readH
	// fractions address, so the sampling maths is the one the copy path used.
	qglViewport( 0, 0, bloom.work.width, bloom.work.height );
	qglScissor( 0, 0, bloom.work.width, bloom.work.height );
	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity();
	qglOrtho( 0, bloom.work.width, bloom.work.height, 0, 0, 1 );
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();
	bloom.targetHeight = bloom.work.height;
}

/*
=================
R_Bloom_RenderToScreen
=================
*/
static void R_Bloom_RenderToScreen( void )
{
	if( bloom.useFBO )
		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	// Restores viewport, scissor, ortho and the matrices the GLSL path caches.
	RB_SetGL2D();
	bloom.targetHeight = glConfig.vidHeight;
}

/*
=================
R_Bloom_Snapshot

Leave the effect texture holding what the passes just produced, so the next pass
can sample it while still writing to the accumulator. A texture cannot be both
the render target and the source of one pass, which is the whole reason this
step exists in either path.
=================
*/
static void R_Bloom_Snapshot( void )
{
	if( !bloom.useFBO ) {
		GL_Bind( bloom.effect.texture );
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, bloom.work.width, bloom.work.height );
		return;
	}

	R_Bloom_RenderTo( bloom.effect.texture );
	GL_Bind( bloom.work.texture );
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	R_Bloom_Quad( bloom.work.width, bloom.work.height, 0, 0, bloom.effect.readW, bloom.effect.readH );
	R_Bloom_RenderTo( bloom.work.texture );
}

/*
=================
R_InitBloomTextures
=================
*/
void R_InitBloomTextures( void )
{
	if( !r_bloom->integer )
		return;
	memset( &bloom, 0, sizeof( bloom ));
	R_Bloom_InitTextures ();
}

/*
=================
R_Bloom_DrawEffect
=================
*/
static void R_Bloom_DrawEffect( void )
{
	GL_Bind( bloom.result );
	if( !r_bloom_debug->integer )
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	else
		GL_State( GLS_DEPTHTEST_DISABLE );
	qglColor4f( r_bloom_alpha->value, r_bloom_alpha->value, r_bloom_alpha->value, 1.0f );
	R_Bloom_Quad( glConfig.vidWidth, glConfig.vidHeight, 0, 0, bloom.effect.readW, bloom.effect.readH );
}


/*
=================
R_Bloom_GeneratexDiamonds
=================
*/
static void R_Bloom_WarsowEffect( void )
{
	int		i, j, k;
	float	intensity, scale, *diamond;


	R_Bloom_RenderTo( bloom.work.texture );

	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	//Take the backup texture and downscale it
	GL_Bind( bloom.screen.texture );
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	R_Bloom_Quad( bloom.work.width, bloom.work.height, 0, 0, bloom.screen.readW, bloom.screen.readH );
	R_Bloom_Snapshot();

	// darkening passes with repeated filter
	if( r_bloom_darken->integer ) {
		int i;
		GL_Bind( bloom.effect.texture );
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO );

		for( i = 0; i < r_bloom_darken->integer; i++ ) {
			R_Bloom_Quad( bloom.work.width, bloom.work.height,
				0, 0,
				bloom.effect.readW, bloom.effect.readH );
		}
		R_Bloom_Snapshot();
	}

	// bluring passes, warsow uses a repeated semi blend on a selectable diamond grid
	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	GL_Bind( bloom.effect.texture );
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_COLOR );
	if( r_bloom_diamond_size->integer > 7 || r_bloom_diamond_size->integer <= 3 ) {
		if( r_bloom_diamond_size->integer != 8 )
			ri.Cvar_Set( "r_bloom_diamond_size", "8" );
	} else if( r_bloom_diamond_size->integer > 5 ) {
		if( r_bloom_diamond_size->integer != 6 )
			ri.Cvar_Set( "r_bloom_diamond_size", "6" );
	} else if( r_bloom_diamond_size->integer > 3 ) {
		if( r_bloom_diamond_size->integer != 4 )
			ri.Cvar_Set( "r_bloom_diamond_size", "4" );
	}

	switch( r_bloom_diamond_size->integer ) {
		case 4:
			k = 2;
			diamond = &Diamond4x[0][0];
			scale = r_bloom_intensity->value * 0.8f;
			break;
		case 6:
			k = 3;
			diamond = &Diamond6x[0][0];
			scale = r_bloom_intensity->value * 0.5f;
			break;
		default:
//		case 8:
			k = 4;
			diamond = &Diamond8x[0][0];
			scale = r_bloom_intensity->value * 0.3f;
			break;
	}

	for( i = 0; i < r_bloom_diamond_size->integer; i++ ) {
		for( j = 0; j < r_bloom_diamond_size->integer; j++, diamond++ ) {
			float x, y;
			intensity =  *diamond * scale;
			if( intensity < 0.01f )
				continue;
			qglColor4f( intensity, intensity, intensity, 1.0 );
			x = (i - k) * ( 2 / 640.0f ) * bloom.effect.readW;
			y = (j - k) * ( 2 / 480.0f ) * bloom.effect.readH;

			R_Bloom_Quad( bloom.work.width, bloom.work.height, x, y, bloom.effect.readW, bloom.effect.readH );
		}
	}

	// The framebuffer-object path has been accumulating straight into the work
	// texture, so the effect is already where it needs to be; the copy path has
	// to fetch it out of the backbuffer one last time.
	if( bloom.useFBO ) {
		bloom.result = bloom.work.texture;
	} else {
		R_Bloom_Snapshot();
		bloom.result = bloom.effect.texture;
	}
}

/*
=================
R_Bloom_BackupScreen
Backup the full original screen to a texture for downscaling and later restoration
=================
*/
static void R_Bloom_BackupScreen( void ) {
	GL_Bind( bloom.screen.texture );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, glConfig.vidWidth, glConfig.vidHeight );
}
/*
=================
R_Bloom_RestoreScreen
Restore the temporary framebuffer section we used with the backup texture
=================
*/
static void R_Bloom_RestoreScreen( void ) {
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	GL_Bind( bloom.screen.texture );
	qglColor4f( 1, 1, 1, 1 );
	R_Bloom_Quad( bloom.work.width, bloom.work.height, 0, 0,
		bloom.work.width / (float)bloom.screen.width,
		bloom.work.height / (float)bloom.screen.height );
}
 

/*
=================
R_Bloom_DownsampleView
Scale the copied screen back to the sample size used for subsequent passes
=================
*/
/*static void R_Bloom_DownsampleView( void )
{
	// TODO, Provide option to control the color strength here /
//	qglColor4f( r_bloom_darken->value, r_bloom_darken->value, r_bloom_darken->value, 1.0f );
	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	GL_Bind( bloom.screen.texture );
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	//Downscale it
	R_Bloom_Quad( bloom.work.width, bloom.work.height, 0, 0, bloom.screen.readW, bloom.screen.readH );
#if 1
	GL_Bind( bloom.effect.texture );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, bloom.work.width, bloom.work.height );
	// darkening passes
	if( r_bloom_darken->integer ) {
		int i;
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO );

		for( i = 0; i < r_bloom_darken->integer; i++ ) {
			R_Bloom_Quad( bloom.work.width, bloom.work.height, 
				0, 0, 
				bloom.effect.readW, bloom.effect.readH );
		}
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, bloom.work.width, bloom.work.height );
	}
#endif
	// Copy the result to the effect texture /
	GL_Bind( bloom.effect.texture );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, bloom.work.width, bloom.work.height );
}

static void R_Bloom_CreateEffect( void ) {
	int dir, x;
	int range;

	//First step will zero dst, rest will one add
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
//	GL_Bind( bloom.screen.texture );
	GL_Bind( bloom.effect.texture );
	range = 4;
	for (dir = 0;dir < 2;dir++)
	{
		// blend on at multiple vertical offsets to achieve a vertical blur
		// TODO: do offset blends using GLSL
		for (x = -range;x <= range;x++)
		{
			float xoffset, yoffset, r;
			if (!dir){
				xoffset = 0;
				yoffset = x*1.5;
			} else {
				xoffset = x*1.5;
				yoffset = 0;
			}
			xoffset /= bloom.work.width;
			yoffset /= bloom.work.height;
			// this r value looks like a 'dot' particle, fading sharply to
			// black at the edges
			// (probably not realistic but looks good enough)
			//r = ((range*range+1)/((float)(x*x+1)))/(range*2+1);
			//r = (dir ? 1.0f : brighten)/(range*2+1);
			r = 2.0f /(range*2+1)*(1 - x*x/(float)(range*range));
//			r *= r_bloom_darken->value;
			qglColor4f(r, r, r, 1);
			R_Bloom_Quad( bloom.work.width, bloom.work.height, 
				xoffset, yoffset, 
				bloom.effect.readW, bloom.effect.readH );
//				bloom.screen.readW, bloom.screen.readH );
			GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
		}
	}
	GL_Bind( bloom.effect.texture );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, bloom.work.width, bloom.work.height );
}*/

/*
=================
R_BloomScreen
=================
*/
void R_BloomScreen( void )
{
	if( !r_bloom->integer )
		return;
	if ( backEnd.doneBloom )
		return;
	if ( !backEnd.doneSurfaces )
		return;
	backEnd.doneBloom = qtrue;
	if( !bloom.started ) {
		R_Bloom_InitTextures();
		if( !bloom.started )
			return;
	}

	if ( !backEnd.projection2D )
		RB_SetGL2D();
#if 0
	// set up full screen workspace
	GL_TexEnv( GL_MODULATE );
	qglScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	qglMatrixMode( GL_PROJECTION );
    qglLoadIdentity ();
	qglOrtho( 0, glConfig.vidWidth, glConfig.vidHeight, 0, 0, 1 );
	qglMatrixMode( GL_MODELVIEW );
    qglLoadIdentity ();

	GL_Cull( CT_TWO_SIDED );
#endif

	qglColor4f( 1, 1, 1, 1 );

	//Backup the old screen in a texture
	R_Bloom_BackupScreen();
	// create the bloom texture using one of a few methods
	R_Bloom_WarsowEffect ();
//	R_Bloom_CreateEffect();
	R_Bloom_RenderToScreen();
	// Only the copy path scribbled on the screen to get here, and only it has
	// something to put back.
	if( !bloom.useFBO )
		R_Bloom_RestoreScreen();
	// Do the final pass using the bloom texture for the final effect
	R_Bloom_DrawEffect ();
}


void R_BloomInit( void ) {
	memset( &bloom, 0, sizeof( bloom ));

	r_bloom = ri.Cvar_Get( "r_bloom", "1", CVAR_ARCHIVE );
	r_bloom_alpha = ri.Cvar_Get( "r_bloom_alpha", "0.5", CVAR_ARCHIVE );
	r_bloom_diamond_size = ri.Cvar_Get( "r_bloom_diamond_size", "8", CVAR_ARCHIVE );
	r_bloom_intensity = ri.Cvar_Get( "r_bloom_intensity", "0.5", CVAR_ARCHIVE );
	r_bloom_darken = ri.Cvar_Get( "r_bloom_darken", "32", CVAR_ARCHIVE );
	r_bloom_sample_size = ri.Cvar_Get( "r_bloom_sample_size", "256", CVAR_ARCHIVE|CVAR_LATCH );
//	r_bloom_fast_sample = ri.Cvar_Get( "r_bloom_fast_sample", "0", CVAR_ARCHIVE|CVAR_LATCH );
	r_bloom_debug = ri.Cvar_Get( "r_bloom_debug", "0", 0 );
}

