#include "cg_local.h"

#define RADAR_RANGE		16000
#define RADAR_BLIPSIZE	  24
#define RADAR_MIDSIZE	  16

// The waypoint marks. Bigger than a blip and a different shape, because a
// master is a landmark rather than a fighter and the two must not be read as
// the same kind of thing at a glance.
#define RADAR_MARKSIZE		18
#define RADAR_QUESTSIZE		26
// A mark clamped to the edge sits this far inside it, so the whole icon stays
// on the radar rather than half of it hanging off the corner.
#define RADAR_EDGE_INSET	 3.0f
#define RADAR_QUEST_PERIOD	1100.0f

radar_t				cg_playerOrigins[MAX_CLIENTS];
static qboolean		cg_radarWarningAlready;


void CG_InitRadarBlips( void ) {

	cg_radarWarningAlready = qfalse;
	memset( cg_playerOrigins, 0, sizeof(cg_playerOrigins) );
}


static void CG_DrawRadarBlips( float x, float y, float w, float h ) {
	playerState_t	*ps;
	qboolean		warning;
	int				i;
	vec3_t			projection, temp;
	vec3_t			up = { 0.0f, 0.0f, 1.0f };
	vec4_t			draw_color;
	vec4_t			drawfull_color;
	vec4_t			drawteam_color;

	float			center_x;
	float			center_y;

	float			blip_x;
	float			blip_y;
	float			blip_w;
	float			blip_h;

	float			powerLevel;
	float			powerLevel2;
	float			powerLevelAverage;
	float			powerLevelMaximum;
	float			powerLevelMaximum2;
	float			powerLevelMaximumAverage;

	float			difference, differenceCurrent, differenceMaximum;

	ps = &cg.predictedPlayerState;
	warning = qfalse;

	center_x = x + 0.5f * w;
	center_y = y + 0.5f * h;

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		
		if ( !cg_playerOrigins[i].valid ) {
			continue;
		}
		if ( cg_playerOrigins[i].clientNum == cg.snap->ps.clientNum ) {
			continue;
		}

		// Calculate and check range
		VectorSubtract( cg_playerOrigins[i].pos, ps->origin, projection );
		if ( VectorLength( projection ) > RADAR_RANGE ) {
			continue;
		}

		// Rotate so that north of radar is direction we're facing (in world coordinates!)
		RotatePointAroundVector(temp, up, projection, 90 - ps->viewangles[YAW] );
		VectorCopy( temp, projection );

		blip_x = ( projection[0] / RADAR_RANGE) * 0.5f * w + center_x;
		blip_y = (-projection[1] / RADAR_RANGE) * 0.5f * h + center_y;

		blip_w = ( projection[2] / RADAR_RANGE) * 0.5f * RADAR_BLIPSIZE + RADAR_BLIPSIZE;
		blip_h = blip_w;

		powerLevel = cg.snap->ps.powerLevel[plCurrent];
		powerLevel2 = cg_playerOrigins[i].pl;
		powerLevelMaximum = cg.snap->ps.powerLevel[plMaximum];
		powerLevelMaximum2 = cg_playerOrigins[i].plMax;

		differenceCurrent = 1.0f - (powerLevel / powerLevel2);
		differenceMaximum = 1.0f - (powerLevelMaximum / powerLevelMaximum2);

		if(differenceCurrent > 1.0f){differenceCurrent = 1.0f;}
		if(differenceCurrent < 0.0f){differenceCurrent = 0.0f;}
		if(differenceMaximum > 1.0f){differenceMaximum = 1.0f;}
		if(differenceMaximum < 0.0f){differenceMaximum = 0.0f;}

		difference = differenceCurrent/* + differenceMaximum*/;

		if(difference > 1.0f){difference = 1.0f;}
		if(difference < 0.0f){difference = 0.0f;}

		//CG_Printf("Difference: %f\n",difference);

		// Set the blip color with respect to team.
		// The brighter the color, the higher the power level is compared to your own.
		// The blip fades down as the player's current power level gets lower then their maximum.
		// Should plHealth also effect the fade?
		if ( cg_playerOrigins[i].team == cg.snap->ps.persistant[PERS_TEAM] && cg_playerOrigins[i].team != TEAM_FREE ) {
			MAKERGBA( draw_color, 0.0f, difference, 0.0f, powerLevel2 / powerLevelMaximum2 );
			MAKERGBA( drawteam_color, 0.0f, 1.0f, 0.0f, powerLevel2 / powerLevelMaximum2 );
			MAKERGBA( drawfull_color, 0.0f, 1.0f, 0.0f, 1.0f );
		} else {
			MAKERGBA( draw_color, difference, 0.0f, 0.0f, powerLevel2 / powerLevelMaximum2 );
			MAKERGBA( drawfull_color, 1.0f, 0.0f, 0.0f, 1.0f );
		}

		// Draw the blip and possible warnings and bursts
		trap_R_SetColor( draw_color );
		CG_DrawPic(qfalse, blip_x - 0.5f * blip_w, blip_y - 0.5f * blip_h, blip_w, blip_h, cgs.media.RadarBlipShader );

		trap_R_SetColor( drawfull_color );		
		if ( cg_playerOrigins[i].properties & RADAR_BURST ) {
			CG_DrawPic(qfalse, blip_x - 0.5f * blip_w, blip_y - 0.5f * blip_h, blip_w, blip_h, cgs.media.RadarBurstShader );
		}

		if ( cg_playerOrigins[i].properties & RADAR_WARN ) {
			CG_DrawPic(qfalse, blip_x - 0.5f * blip_w, blip_y - 0.5f * blip_h, blip_w, blip_h, cgs.media.RadarWarningShader );
			// Atleast one warning was on the radar this screen.
			// NOTE: Used to check if a warning sound needs to be issued.
			warning = qtrue;
		}

		// Draw the team blip
		if ( cg_playerOrigins[i].team == cg.snap->ps.persistant[PERS_TEAM] && cg_playerOrigins[i].team != TEAM_FREE ) {
			trap_R_SetColor( drawteam_color );
			CG_DrawPic(qfalse, blip_x - 0.5f * blip_w, blip_y - 0.5f * blip_h, blip_w, blip_h, cgs.media.RadarBlipTeamShader );
		}

		trap_R_SetColor( NULL );
	}

	// Draw the middle point last (on top of everything else, for clarity).
	CG_DrawPic(qfalse, center_x - RADAR_MIDSIZE * 0.5f, center_y - RADAR_MIDSIZE * 0.5f, RADAR_MIDSIZE, RADAR_MIDSIZE, cgs.media.RadarMidpointShader );

	// Handle the warning sound.
	if ( warning && !cg_radarWarningAlready ) {
		cg_radarWarningAlready = qtrue;
		trap_S_StartLocalSound( cgs.media.radarwarningSound, CHAN_LOCAL_SOUND );
	}
	if ( !warning ) {
		cg_radarWarningAlready = qfalse;
	}

}

/*================
CG_DrawRadarMasters

The masters, on the same radar and in the same units as the ki-sense blips.

Two marks, and the difference between them is the whole feature. Every placed
master gets the quiet lozenge, so a map reads as a place with people on it. The
one the active objective names gets the bright pulsing mark, its distance under
the radar, and - this is the part a player notices - it is never culled. A
destination beyond RADAR_RANGE is clamped to the radar's edge on its true
bearing, because an objective whose marker disappears the moment you are far
enough from it to need it is worse than no marker at all.

Distances are in map units, which is what setviewpos, masterlist and every
other number a player sees in this game are in.
================*/
static void CG_DrawRadarMasters( float x, float y, float w, float h ) {
	playerState_t	*ps;
	vec3_t			projection, temp;
	vec3_t			up = { 0.0f, 0.0f, 1.0f };
	vec4_t			mark_color;
	float			center_x, center_y, limit, reach;
	float			mark_x, mark_y, size, pulse, distance;
	qboolean		destination;
	int				i;

	ps = &cg.predictedPlayerState;
	center_x = x + 0.5f * w;
	center_y = y + 0.5f * h;
	limit = 0.5f * ( w < h ? w : h ) - RADAR_EDGE_INSET;

	for ( i = 0; i < CG_MasterCount(); i++ ) {
		destination = ( cg.trainingDestination[0] &&
			!Q_stricmp( cg.trainingDestination, CG_MasterName( i ) ) ) ? qtrue : qfalse;

		VectorSubtract( CG_MasterOrigin( i ), ps->origin, projection );
		distance = VectorLength( projection );
		if ( distance > RADAR_RANGE && !destination ) {
			continue;
		}

		// Same rotation the blips use: radar north is the direction faced.
		RotatePointAroundVector( temp, up, projection, 90 - ps->viewangles[YAW] );
		VectorCopy( temp, projection );

		mark_x = ( projection[0] / RADAR_RANGE ) * 0.5f * w;
		mark_y = (-projection[1] / RADAR_RANGE ) * 0.5f * h;

		// Clamp to the edge along the bearing rather than per axis, which would
		// slide the mark around the rim and point at the wrong place.
		reach = sqrt( mark_x * mark_x + mark_y * mark_y );
		if ( reach > limit ) {
			if ( reach <= 0.0f ) {
				continue;
			}
			mark_x *= limit / reach;
			mark_y *= limit / reach;
		}
		mark_x += center_x;
		mark_y += center_y;

		if ( destination ) {
			pulse = 0.72f + 0.28f * sin( cg.time * 2.0f * M_PI / RADAR_QUEST_PERIOD );
			MAKERGBA( mark_color, 1.0f, 0.82f, 0.24f, pulse );
			size = RADAR_QUESTSIZE;
		} else {
			MAKERGBA( mark_color, 0.35f, 0.64f, 0.89f, 0.55f );
			size = RADAR_MARKSIZE;
		}

		trap_R_SetColor( mark_color );
		CG_DrawPic( qfalse, mark_x - 0.5f * size, mark_y - 0.5f * size, size, size,
			destination ? cgs.media.RadarQuestShader : cgs.media.RadarMasterShader );
		trap_R_SetColor( NULL );

		// Who and how far, under the radar, for the destination alone. Two of
		// these would be a list rather than a heading.
		if ( destination && CG_TextValid( TEXTFACE_BODY ) ) {
			vec4_t	label = { 1.0f, 0.82f, 0.24f, 0.92f };

			CG_TextDraw( TEXTFACE_BODY, center_x, y + h + 2, 12,
				label, va( "%s  %.0f", CG_MasterName( i ), distance ), 0,
				TEXTF_CENTER | TEXTF_SHADOW );
		}
	}
}

void CG_DrawRadar () {
	playerState_t	*ps;
	ps = &cg.predictedPlayerState;
	if(!(ps->lockedTarget>0)){
		CG_DrawRadarBlips( 512, 0, 128, 128 );
		if(cg_drawMasterMarks.integer){
			CG_DrawRadarMasters( 512, 0, 128, 128 );
		}
	}
}
