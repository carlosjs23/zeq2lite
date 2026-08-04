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
//
// cg_weapons.c -- events and effects dealing with weapons
#include "cg_local.h"

/*
=============
CG_DrawLine
=============
*/
void CG_DrawLine (vec3_t start, vec3_t end, float width, qhandle_t shader, float RGBModulate) {
	vec3_t line, offset, viewLine;
	polyVert_t verts[4];
	float len;
	int i, j;
	
	VectorSubtract (end, start, line);
	VectorSubtract (start, cg.refdef.vieworg, viewLine);
	CrossProduct (viewLine, line, offset);
	len = VectorNormalize (offset);
	
	if (!len) {
		return;
	}
	
	VectorMA (end, -width, offset, verts[0].xyz);
	verts[0].st[0] = 1;
	verts[0].st[1] = 0;
	VectorMA (end, width, offset, verts[1].xyz);
	verts[1].st[0] = 0;
	verts[1].st[1] = 0;
	VectorMA (start, width, offset, verts[2].xyz);
	verts[2].st[0] = 0;
	verts[2].st[1] = 1;
	VectorMA (start, -width, offset, verts[3].xyz);
	verts[3].st[0] = 1;
	verts[3].st[1] = 1;
	
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			verts[i].modulate[j] = 255 * RGBModulate;
		}
	}

	trap_R_AddPolyToScene( shader, 4, verts);
}

void CG_DrawLineRGBA (vec3_t start, vec3_t end, float width, qhandle_t shader, vec4_t RGBA) {
	vec3_t line, offset, viewLine;
	polyVert_t verts[4];
	float len;
	int i, j;
	
	VectorSubtract (end, start, line);
	VectorSubtract (start, cg.refdef.vieworg, viewLine);
	CrossProduct (viewLine, line, offset);
	len = VectorNormalize (offset);
	
	if (!len) {
		return;
	}
	
	VectorMA (end, -width, offset, verts[0].xyz);
	verts[0].st[0] = 1;
	verts[0].st[1] = 0;
	VectorMA (end, width, offset, verts[1].xyz);
	verts[1].st[0] = 0;
	verts[1].st[1] = 0;
	VectorMA (start, width, offset, verts[2].xyz);
	verts[2].st[0] = 0;
	verts[2].st[1] = 1;
	VectorMA (start, -width, offset, verts[3].xyz);
	verts[3].st[0] = 1;
	verts[3].st[1] = 1;
	
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			verts[i].modulate[j] = RGBA[j];
		}
	}

	trap_R_AddPolyToScene( shader, 4, verts);
}

/*========================================================================================
VIEW WEAPON
========================================================================================*/

/*=================
CG_MapTorsoToWeaponFrame
=================*/
static int CG_MapTorsoToWeaponFrame( clientInfo_t *ci, int frame ) {
/*
	// change weapon
	if ( frame >= ci->animations[ANIM_DROP].firstFrame 
		&& frame < ci->animations[ANIM_DROP].firstFrame + 9 ) {
		return frame - ci->animations[ANIM_DROP].firstFrame + 6;
	}

	// stand attack
	if ( frame >= ci->animations[ANIM_ATTACK].firstFrame 
		&& frame < ci->animations[ANIM_ATTACK].firstFrame + 6 ) {
		return 1 + frame - ci->animations[ANIM_ATTACK].firstFrame;
	}

	// stand attack 2
	if ( frame >= ci->animations[ANIM_ATTACK2].firstFrame 
		&& frame < ci->animations[ANIM_ATTACK2].firstFrame + 6 ) {
		return 1 + frame - ci->animations[ANIM_ATTACK2].firstFrame;
	}
*/
	return 0;
}


/*
==============
CG_CalculateWeaponPosition
==============
*/
static void CG_CalculateWeaponPosition( vec3_t origin, vec3_t angles ) {
	float	scale;
	int		delta;
	float	fracsin;

	VectorCopy( cg.refdef.vieworg, origin );
	VectorCopy( cg.refdefViewAngles, angles );

	// on odd legs, invert some angles
	if ( cg.bobcycle & 1 ) {
		scale = -cg.xyspeed;
	} else {
		scale = cg.xyspeed;
	}

	// gun angles from bobbing
	angles[ROLL] += scale * cg.bobfracsin * 0.005;
	angles[YAW] += scale * cg.bobfracsin * 0.01;
	angles[PITCH] += cg.xyspeed * cg.bobfracsin * 0.005;

	// drop the weapon when landing
	delta = cg.time - cg.landTime;
	if ( delta < LAND_DEFLECT_TIME ) {
		origin[2] += cg.landChange*0.25 * delta / LAND_DEFLECT_TIME;
	} else if ( delta < LAND_DEFLECT_TIME + LAND_RETURN_TIME ) {
		origin[2] += cg.landChange*0.25 * 
			(LAND_DEFLECT_TIME + LAND_RETURN_TIME - delta) / LAND_RETURN_TIME;
	}

#if 0
	// drop the weapon when stair climbing
	delta = cg.time - cg.stepTime;
	if ( delta < STEP_TIME/2 ) {
		origin[2] -= cg.stepChange*0.25 * delta / (STEP_TIME/2);
	} else if ( delta < STEP_TIME ) {
		origin[2] -= cg.stepChange*0.25 * (STEP_TIME - delta) / (STEP_TIME/2);
	}
#endif

	// idle drift
	scale = cg.xyspeed + 40;
	fracsin = sin( cg.time * 0.001 );
	angles[ROLL] += scale * fracsin * 0.01;
	angles[YAW] += scale * fracsin * 0.01;
	angles[PITCH] += scale * fracsin * 0.01;
}


/*
===============
CG_LightningBolt

Origin will be the exact tag point, which is slightly
different than the muzzle point used for determining hits.
The cent should be the non-predicted cent if it is from the player,
so the endpoint will reflect the simulated strike (lagging the predicted
angle)
===============
*/
static void CG_LightningBolt( centity_t *cent, vec3_t origin ) {
	return;
}
/*
===============
JUHOX: CG_Draw3DLine
===============
*/
void CG_Draw3DLine(const vec3_t start, const vec3_t end, qhandle_t shader) {
	refEntity_t line;

	//if (DistanceSquared(start, end) < 10*10) return;	
	memset(&line, 0, sizeof(line));
	line.reType = RT_LIGHTNING;
	line.customShader = shader;
	VectorCopy(start, line.origin);
	VectorCopy(end, line.oldorigin);
	trap_R_AddRefEntityToScene(&line);
}

/*
===============
CG_SpawnRailTrail

Origin will be the exact tag point, which is slightly
different than the muzzle point used for determining hits.
===============
*/
static void CG_SpawnRailTrail( centity_t *cent, vec3_t origin ) {return;}


/*
========================
CG_AddWeaponWithPowerups
========================
*/
static void CG_AddWeaponWithPowerups( refEntity_t *gun, int powerups ) {}


/*
=============
CG_AddPlayerWeaponCharge

Used for both the primary and the alternate fire of weapons.
Used case depends on input.
=============
*/
static void CG_AddPlayerWeaponCharge( refEntity_t *parent, cg_userWeapon_t *weaponGraphics,
									  refEntity_t *charge, float chargeLvl ) {
	float				chargeScale;
	float				chargeDlightScale;
	
	// Obtain the scale the charge must have.
	if (weaponGraphics->chargeGrowth) {
		// above the end, we use the highest form
		if (weaponGraphics->chargeEndPct <= chargeLvl) {
			chargeScale = weaponGraphics->chargeEndsize;
			chargeDlightScale = weaponGraphics->chargeDlightEndRadius;
		} else {
			// inbetween start and end, we work out the value
			float	PctRange;
			float	PctVal;
			float	SizeRange;
			float	SizeVal;
					
			PctRange = weaponGraphics->chargeEndPct - weaponGraphics->chargeStartPct;
			PctVal = chargeLvl - weaponGraphics->chargeStartPct;

			SizeRange = weaponGraphics->chargeEndsize - weaponGraphics->chargeStartsize;
			SizeVal = (PctVal / PctRange) * SizeRange;
			chargeScale = SizeVal + weaponGraphics->chargeStartsize;

			SizeRange = weaponGraphics->chargeDlightEndRadius - weaponGraphics->chargeDlightStartRadius;
			SizeVal = (PctVal / PctRange) * SizeRange;
			chargeDlightScale = SizeVal + weaponGraphics->chargeDlightStartRadius;
		}
	} else {
		chargeScale = weaponGraphics->chargeStartsize;
		chargeDlightScale = weaponGraphics->chargeDlightStartRadius;
	}

	// Add the charge model or sprite

	VectorCopy( parent->lightingOrigin, charge->lightingOrigin );
	charge->shadowPlane = parent->shadowPlane;
	charge->renderfx = parent->renderfx;
	if ( ! (weaponGraphics->chargeModel && weaponGraphics->chargeSkin) ) {
		charge->reType = RT_SPRITE;
		charge->radius = 4 * chargeScale;
		charge->rotation = 0;
		charge->customShader = weaponGraphics->chargeShader;
	} else {
		charge->reType = RT_MODEL;
		charge->hModel = weaponGraphics->chargeModel;
		charge->customSkin = weaponGraphics->chargeSkin;
	
		charge->nonNormalizedAxes = qtrue;
		VectorScale(charge->axis[0], chargeScale, charge->axis[0]);
		VectorScale(charge->axis[1], chargeScale, charge->axis[1]);
		VectorScale(charge->axis[2], chargeScale, charge->axis[2]);

		if(cg_drawBBox.value){
			vec3_t	mins,maxs;
			trap_R_ModelBounds( charge->hModel, mins, maxs, charge->frame );
			VectorScale(mins, chargeScale, mins);
			VectorScale(maxs, chargeScale, maxs);
			CG_DrawBoundingBox( charge->origin, mins, maxs );
		}

	}

	trap_R_AddRefEntityToScene( charge );


	// add dynamic light
	if ( chargeDlightScale ) {
		trap_R_AddLightToScene( charge->origin,
								100 * chargeDlightScale,
								weaponGraphics->chargeDlightColor[0],
								weaponGraphics->chargeDlightColor[1],
								weaponGraphics->chargeDlightColor[2] );
	}
}

/*
=============
CG_AddPlayerWeaponChargeVoices

Used for both the primary and the alternate fire of weapons.
Used case depends on input.
=============
*/
static void CG_AddPlayerWeaponChargeVoices( centity_t *player, cg_userWeapon_t *weaponGraphics, float curChargeLvl, float prevChargeLvl ) {
	int i;
	chargeVoice_t *voice;

	for ( i = MAX_CHARGE_VOICES - 1; i >= 0; i-- ) {
		voice = &weaponGraphics->chargeVoice[i];
		if ( voice->voice ) { // no sound; no complex boolean eval
			if ( (voice->startPct > prevChargeLvl) && (voice->startPct < curChargeLvl) && ( prevChargeLvl < curChargeLvl ) ) {
				trap_S_StartSound( NULL , player->currentState.number, CHAN_VOICE, voice->voice );
				break;
			}
		}
	}
}

/*
=============
CG_AddPlayerWeaponFlash

Used for both the primary and the alternate fire of weapons.
Used case depends on input.
=============
*/
static void CG_AddPlayerWeaponFlash( refEntity_t *parent, cg_userWeapon_t *weaponGraphics,
									  refEntity_t *flash, int flashPowerLevelTotal, int flashPowerLevelCurrent ) {
	float	flashScale;
	float	radiusScale;

	if(flashPowerLevelCurrent >= (flashPowerLevelTotal * 2)){
		flashPowerLevelCurrent = flashPowerLevelTotal * 2;
	}

	radiusScale = (float)flashPowerLevelCurrent / (float)flashPowerLevelTotal;
	flashScale = weaponGraphics->flashSize * radiusScale;

	if (radiusScale > 1.0f){
		radiusScale = 1.0f;
	}else if(radiusScale < 0.0f){
		radiusScale = 0.0f;
	}

	// Add the charge model or sprite

	VectorCopy( parent->lightingOrigin, flash->lightingOrigin );
	flash->shadowPlane = parent->shadowPlane;
	flash->renderfx = parent->renderfx;
	if ( ! (weaponGraphics->flashModel && weaponGraphics->flashSkin) ) {
		flash->reType = RT_SPRITE;
		flash->radius = 4 * flashScale;
		flash->rotation = 0;
		flash->customShader = weaponGraphics->flashShader;
	} else {
		flash->reType = RT_MODEL;
		flash->hModel = weaponGraphics->flashModel;
		flash->customSkin = weaponGraphics->flashSkin;
	
		flash->nonNormalizedAxes = qtrue;
		VectorScale(flash->axis[0], flashScale, flash->axis[0]);
		VectorScale(flash->axis[1], flashScale, flash->axis[1]);
		VectorScale(flash->axis[2], flashScale, flash->axis[2]);

		if(cg_drawBBox.value){
			vec3_t	mins,maxs;
			trap_R_ModelBounds( flash->hModel, mins, maxs, flash->frame );
			VectorScale(mins, flashScale, mins);
			VectorScale(maxs, flashScale, maxs);
			CG_DrawBoundingBox( flash->origin, mins, maxs );
		}
	}

	trap_R_AddRefEntityToScene( flash );


	// add dynamic light
	if ( weaponGraphics->flashDlightRadius ) {
		trap_R_AddLightToScene( flash->origin,
								100 * weaponGraphics->flashDlightRadius,
								weaponGraphics->flashDlightColor[0],
								weaponGraphics->flashDlightColor[1],
								weaponGraphics->flashDlightColor[2] );
	}
}

/*
====================
CG_AddPlayerWeapon
====================
*/
void CG_AddPlayerWeapon( refEntity_t *parent, playerState_t *ps, centity_t *cent, int team ) {
	cg_userWeapon_t		*weaponGraphics;
	orientation_t		orient;
	refEntity_t			refEnt;
	entityState_t		*ent;
	float				lerp;
	float				backLerp;
	int					newLerp;
	weaponstate_t		weaponState;

	// Set some shorthands
	ent = &cent->currentState;
	weaponState = ent->weaponstate;
	
	// Any of the charging states
	if ( weaponState == WEAPON_CHARGING || weaponState == WEAPON_ALTCHARGING ) {

		// Set properties depending on primary or alternate fire
		if ( weaponState == WEAPON_CHARGING ) {
			weaponGraphics = CG_FindUserWeaponGraphics(ent->clientNum, ent->weapon);

			newLerp = ent->charge1.chBase;
			backLerp = cent->lerpPrim;
			if ( newLerp < backLerp ) {
				cent->lerpPrim = newLerp;
			} else {
				cent->lerpPrim = (cent->lerpPrim + newLerp ) / 2.0f;
			}

			lerp = cent->lerpPrim;
			cent->lerpSec = 0;
		} else {
			weaponGraphics = CG_FindUserWeaponGraphics(ent->clientNum, ent->weapon + ALTWEAPON_OFFSET );

			newLerp = ent->charge2.chBase;
			backLerp = cent->lerpSec;
			if ( newLerp < backLerp ) {
				cent->lerpSec = newLerp;
			} else {
				cent->lerpSec = (cent->lerpSec + newLerp ) / 2.0f;
			}

			lerp = cent->lerpSec;
			cent->lerpPrim = 0;
		}

		// Only bother with anything else if the charge in question is larger than the minimum used for display
		if ( lerp > weaponGraphics->chargeStartPct ) {			

			// Locate a tag wherever on the model's parts. Don't process further if the tag is not found
			if (CG_GetTagOrientationFromPlayerEntity( cent, weaponGraphics->chargeTag[0], &orient )) {
				memset( &refEnt, 0, sizeof(refEnt));

				if ( VectorLength(weaponGraphics->chargeSpin) != 0.0f ) {
					vec3_t	tempAngles;
					vec3_t	lerpAxis[3], tempAxis[3];
					
					VectorCopy( orient.origin, refEnt.origin );
					VectorSet( tempAngles, cg.time / 4.0f, cg.time / 4.0f, cg.time / 4.0f );
					VectorPieceWiseMultiply( tempAngles, weaponGraphics->chargeSpin, tempAngles );
					AnglesToAxis( tempAngles, lerpAxis );

					AxisClear( refEnt.axis );
					MatrixMultiply( refEnt.axis, lerpAxis, tempAxis );
					MatrixMultiply( tempAxis, orient.axis, refEnt.axis );					

				} else {

					VectorCopy( orient.origin, refEnt.origin );
					AxisCopy( orient.axis, refEnt.axis );
				}

				CG_AddPlayerWeaponCharge( parent, weaponGraphics, &refEnt, lerp );
			}
		}
		
		if ( weaponGraphics->chargeLoopSound ) {
			trap_S_AddLoopingSound( ent->number, cent->lerpOrigin, vec3_origin, weaponGraphics->chargeLoopSound );
		}

		CG_AddPlayerWeaponChargeVoices( cent, weaponGraphics, lerp, backLerp );

		// Set up any charging particle systems
		if ( weaponGraphics->chargeParticleSystem[0] ) {
			// If the entity wasn't previously in the PVS, if the weapon nr switched, or if the weaponstate switched
			// we need to start a new system
			if ( !CG_FrameHist_WasInPVS(ent->number) ||
				 CG_FrameHist_IsWeaponNr(ent->number) != CG_FrameHist_WasWeaponNr(ent->number) ||
				 CG_FrameHist_IsWeaponState(ent->number) != CG_FrameHist_WasWeaponState(ent->number) ) {
				PSys_SpawnCachedSystem( weaponGraphics->chargeParticleSystem, cent->lerpOrigin, NULL, cent, weaponGraphics->chargeTag[0], qfalse, qtrue );
			}
		}		

	// Any of the firing or guiding states
	} else if ( weaponState == WEAPON_GUIDING || weaponState == WEAPON_ALTGUIDING || weaponState == WEAPON_FIRING || weaponState == WEAPON_ALTFIRING ) { 

		// Set properties depending on primary or alternate fire
		if ( weaponState == WEAPON_GUIDING || weaponState == WEAPON_FIRING ) {
			weaponGraphics = CG_FindUserWeaponGraphics(ent->clientNum, ent->weapon );
		} else {
			weaponGraphics = CG_FindUserWeaponGraphics(ent->clientNum, ent->weapon + ALTWEAPON_OFFSET );
		}

		// Locate a tag wherever on the model's parts. Don't process further if the tag is not found
		if (CG_GetTagOrientationFromPlayerEntity( cent, weaponGraphics->chargeTag[0], &orient )) {
			memset( &refEnt, 0, sizeof(refEnt));

			VectorCopy( orient.origin, refEnt.origin );
			AxisCopy( orient.axis, refEnt.axis );
			
			CG_AddPlayerWeaponFlash( parent, weaponGraphics, &refEnt, ent->attackPowerTotal, ent->attackPowerCurrent );
		}

		if ( weaponGraphics->firingSound ) {
			trap_S_AddLoopingSound( ent->number, cent->lerpOrigin, vec3_origin, weaponGraphics->firingSound );
		}

		// Set up any firing particle systems
		if ( weaponGraphics->firingParticleSystem[0] ) {
			// If the entity wasn't previously in the PVS, if the weapon nr switched, or if the weaponstate switched
			// we need to start a new system
			if ( !CG_FrameHist_WasInPVS(ent->number) ||
				 CG_FrameHist_IsWeaponNr(ent->number) != CG_FrameHist_WasWeaponNr(ent->number) ||
				 CG_FrameHist_IsWeaponState(ent->number) != CG_FrameHist_WasWeaponState(ent->number) ) {
				PSys_SpawnCachedSystem( weaponGraphics->firingParticleSystem, cent->lerpOrigin, NULL, cent, weaponGraphics->chargeTag[0], qfalse, qtrue );
			}
		}
	}
}

/*
==============
CG_AddViewWeapon

Add the weapon, and flash for the player's view
==============
*/
void CG_AddViewWeapon( playerState_t *ps ) {

	return;
	
/*
	refEntity_t	hand;
	centity_t	*cent;
	clientInfo_t	*ci;
	float		fovOffset;
	vec3_t		angles;
	weaponInfo_t	*weapon;



	if ( ps->persistant[PERS_TEAM] == TEAM_SPECTATOR ) {
		return;
	}

	if ( ps->pm_type == PM_INTERMISSION ) {
		return;
	}

	// no gun if in third person view or a camera is active
	//if ( cg.renderingThirdPerson || cg.cameraMode) {
	if ( cg.renderingThirdPerson ) {
		return;
	}


	// allow the gun to be completely removed
	if ( !cg_drawGun.integer ) {
		vec3_t		origin;

		if ( cg.predictedPlayerState.eFlags & EF_FIRING ) {
			// special hack for lightning gun...
			VectorCopy( cg.refdef.vieworg, origin );
			VectorMA( origin, -8, cg.refdef.viewaxis[2], origin );
			CG_LightningBolt( &cg_entities[ps->clientNum], origin );
		}
		return;
	}

	// don't draw if testing a gun model
	if ( cg.testGun ) {
		return;
	}

	// drop gun lower at higher fov
	if ( cg_fov.integer > 90 ) {
		fovOffset = -0.2 * ( cg_fov.integer - 90 );
	} else {
		fovOffset = 0;
	}

	cent = &cg.predictedPlayerEntity;	// &cg_entities[cg.snap->ps.clientNum];
	CG_RegisterWeapon( ps->weapon );
	weapon = &cg_weapons[ ps->weapon ];

	memset (&hand, 0, sizeof(hand));

	// set up gun position
	CG_CalculateWeaponPosition( hand.origin, angles );

	VectorMA( hand.origin, cg_gun_x.value, cg.refdef.viewaxis[0], hand.origin );
	VectorMA( hand.origin, cg_gun_y.value, cg.refdef.viewaxis[1], hand.origin );
	VectorMA( hand.origin, (cg_gun_z.value+fovOffset), cg.refdef.viewaxis[2], hand.origin );

	AnglesToAxis( angles, hand.axis );

	// map torso animations to weapon animations
	if ( cg_gun_frame.integer ) {
		// development tool
		hand.frame = hand.oldframe = cg_gun_frame.integer;
		hand.backlerp = 0;
	} else {
		// get clientinfo for animation map
		ci = &cgs.clientinfo[ cent->currentState.clientNum ];
		hand.frame = CG_MapTorsoToWeaponFrame( ci, cent->pe.torso.frame );
		hand.oldframe = CG_MapTorsoToWeaponFrame( ci, cent->pe.torso.oldFrame );
		hand.backlerp = cent->pe.torso.backlerp;
	}

	hand.hModel = weapon->handsModel;
	hand.renderfx = RF_DEPTHHACK | RF_FIRST_PERSON | RF_MINLIGHT;

	// add everything onto the hand
	CG_AddPlayerWeapon( &hand, ps, &cg.predictedPlayerEntity, ps->persistant[PERS_TEAM] );
*/
}

/*
==============================================================================

WEAPON SELECTION

==============================================================================
*/

static void CG_DrawWeaponSelectQuarterFan( void ) {
	int				i;
	int				bits;
	int				count;
	int				x, y;
	int				textX, textY;
	int				iconsPerLine, iconW, iconH;
	int				rootX, rootY;
	int				startOffset, lineOffset;
	float			startAngle, endAngle;
	float			angleUnit;
	char			name[MAX_WEAPONNAME * 2 + 4]; // Need a little more leeway
	float			*color;
	cg_userWeapon_t	*weaponInfo;
	cg_userWeapon_t	*alt_weaponInfo;

	color = CG_FadeColor( cg.weaponSelectTime, WEAPON_SELECT_TIME, 200 );
	if ( !color ) {
		cg.drawWeaponBar = 0;
		return;
	}
	trap_R_SetColor( color );

	textX = 110; textY = 380;

	rootX = 64;	rootY = 424;
	iconsPerLine = 3; iconW = 32; iconH = 32;
	startAngle = DEG2RAD(-20); endAngle = DEG2RAD(40);
	startOffset = 80; lineOffset = 40;

	if (iconsPerLine != 1) {
		angleUnit = (endAngle - startAngle) / (iconsPerLine - 1);
	} else {
		angleUnit = 0;
	}
	
	bits = cg.snap->ps.stats[ stSkills ];
	count = 0;
	for ( i = 1 ; i < 16 ; i++ ) {
		if ( !( bits & ( 1 << i ) ) ) {
			continue;
		}

		weaponInfo = CG_FindUserWeaponGraphics( cg.snap->ps.clientNum, i );

		// set position
		x = rootX - (iconW / 2) +
			sin(startAngle + angleUnit * (count % 3)) * (startOffset + lineOffset * (count / 3));
		y = rootY - (iconH / 2) +
			-1 * cos(startAngle + angleUnit * (count % 3)) * (startOffset + lineOffset * (count / 3));
		// draw weapon icon
		if ( weaponInfo->weaponIcon ) {
			CG_DrawPic(qfalse, x, y, 32, 32, weaponInfo->weaponIcon );
		}

		if ( i == cg.weaponSelect ) {
			CG_DrawPic(qfalse, x-4, y-4, 40, 40, cgs.media.selectShader );
		}
		count++;
	}

	weaponInfo = CG_FindUserWeaponGraphics( cg.snap->ps.clientNum, cg.weaponSelect );
	alt_weaponInfo = CG_FindUserWeaponGraphics ( cg.snap->ps.clientNum, ALTWEAPON_OFFSET + cg.weaponSelect);

	// draw the selected name
	if ( weaponInfo->weaponName[0] != 0 ) {

		if ( alt_weaponInfo->weaponName[0] != 0 ) {
			Com_sprintf( name, sizeof(name), "%s / %s", weaponInfo->weaponName, alt_weaponInfo->weaponName );
		} else {
			Com_sprintf( name, sizeof(name), "%s", weaponInfo->weaponName );
		}

		CG_DrawMediumStringColor(textX, textY, name, color);
	}
	trap_R_SetColor( NULL );
	
}

static qboolean CG_WeaponSelectable( int i );

/*
===============================
CG_DrawSkillDeny

The two things an icon can say that are not "pick me", drawn as one mark in two
registers so a player learns a single symbol.

Corner-mounted and steady in the guard colour: this skill's own requirements
are not met - goku's solar flare wants flight, and no amount of waiting on the
ground reaches it. Over the whole icon and decaying in the threat colour: the
change was refused just now, which is a reaction to what the player did rather
than a standing property of the skill, so it fades the way a reaction should.

The refusal outranks the lock on the same icon: it is the newer answer and the
one that was asked for.
===============================
*/
static void CG_DrawSkillDeny( int skill, qboolean usable, float x, float y, float fade ) {
	vec4_t	threatColor = {1.0f,0.35f,0.2f,1.0f};
	vec4_t	guardColor = {0.4f,0.72f,1.0f,1.0f};
	vec4_t	mark;
	float	flash;

	flash = 0;
	if ( cg.skillRefuseTime && cg.skillRefuseIndex == skill ) {
		flash = 1.0f - (float)( cg.time - cg.skillRefuseTime ) / (float)SKILL_REFUSE_TIME;
	}
	if ( flash > 0 ) {
		Vector4Copy( threatColor, mark );
		mark[3] = flash * fade;
		trap_R_SetColor( mark );
		CG_DrawPic( qfalse, x - 2, y - 2, 28, 28, cgs.media.skillDenyShader );
		return;
	}
	if ( usable ) {
		return;
	}
	Vector4Copy( guardColor, mark );
	mark[3] = 0.85f * fade;
	trap_R_SetColor( mark );
	CG_DrawPic( qfalse, x + 13, y + 13, 11, 11, cgs.media.skillDenyShader );
}

/*
===============================
CG_DrawWeaponSelectHorCenterBar
===============================
*/
static void CG_DrawWeaponSelectHorCenterBar( void ) {
	int		i;
	int		bits;
	int		count;
	int		x, y, w;
	char	name[MAX_WEAPONNAME * 2 + 4]; // Need a little more leeway
	float	*color;
	vec4_t	dimColor;
	cg_userWeapon_t	*weaponInfo;
	cg_userWeapon_t	*alt_weaponInfo;

	color = CG_FadeColor( cg.weaponSelectTime, WEAPON_SELECT_TIME, 200 );
	if ( !color ) {
		cg.drawWeaponBar = 0;
		return;
	}

	trap_R_SetColor( color );

	// count the number of weapons owned
	bits = cg.snap->ps.stats[ stSkills ];
	count = 0;
	for ( i = 1 ; i < 16 ; i++ ) {
		if ( bits & ( 1 << i ) ) {
			count++;
		}
	}

	x = 164 - count * 15;
	// Clear of the HUD panel: the selection ring reaches 4 above each icon, and
	// the plate starts at HUD_PANEL_Y.
	y = HUD_PANEL_Y - 38;

	for ( i = 1 ; i < 16 ; i++ ) {
		qboolean usable;
		if ( !( bits & ( 1 << i ) ) ) {
			continue;
		}

		weaponInfo = CG_FindUserWeaponGraphics( cg.snap->ps.clientNum, i );
		usable = CG_WeaponSelectable( i );
		// An unavailable skill keeps its slot instead of leaving the bar. A
		// skill that vanishes teaches nothing - the player never learns it
		// exists, let alone what is holding it - so it is drawn dim with the
		// deny mark on it. It stays out of the wheel regardless: the cycle is
		// CG_StepWeaponSelect's, and that walks the same predicate it always
		// did. The slot count above already counted it, so the bar was
		// centring for it before it was drawn.
		if ( usable ) {
			trap_R_SetColor( color );
		} else {
			Vector4Copy( color, dimColor );
			dimColor[3] = color[3] * 0.35f;
			trap_R_SetColor( dimColor );
		}
		// A skill whose .grfx never named an icon would otherwise draw as
		// shader handle 0 - the default black and white block - and the block
		// ignores the tint, so it would arrive at full brightness with the deny
		// mark on it. The slot and its mark still stand; only the lie about the
		// art goes.
		if ( weaponInfo->weaponIcon ) {
			CG_DrawPic(qfalse, x, y, 24, 24, weaponInfo->weaponIcon );
		}
		if ( usable && i == cg.weaponSelect ) {
			trap_R_SetColor( color );
			CG_DrawPic(qfalse, x-4, y-4, 32, 32, cgs.media.selectShader );
		}
		if ( cg_drawSkillState.integer ) {
			CG_DrawSkillDeny( i, usable, x, y, color[3] );
		}
		x += 30;

	}
	trap_R_SetColor( color );

	weaponInfo = CG_FindUserWeaponGraphics( cg.snap->ps.clientNum, cg.weaponSelect );
	alt_weaponInfo = CG_FindUserWeaponGraphics ( cg.snap->ps.clientNum, ALTWEAPON_OFFSET + cg.weaponSelect);

	// draw the selected name
	if ( weaponInfo->weaponName ) {

		if ( alt_weaponInfo->weaponName ) {
			Com_sprintf( name, sizeof(name), "%s / %s", weaponInfo->weaponName, alt_weaponInfo->weaponName );
		} else {
			Com_sprintf( name, sizeof(name), "%s", weaponInfo->weaponName );
		}

		w = CG_DrawStrlen( name ) * MEDIUMCHAR_WIDTH; //BIGCHAR_WIDTH;
		x = ( SCREEN_WIDTH - w ) / 2;
		//CG_DrawSmallStringColor(6, y - 30, name, color);
	}

	trap_R_SetColor( NULL );

}

void CG_DrawWeaponSelect( void ) {
	cg.itemPickupTime = 0;
	if(cg.drawWeaponBar == 1)
	{
		CG_DrawWeaponSelectHorCenterBar();
		//CG_DrawWeaponSelectQuarterFan();
	}
}


/*
===============
CG_WeaponSelectable
===============
*/
static qboolean CG_WeaponSelectable( int i ) {
	qboolean usable;
	usable = qfalse;
	if ( ! (cg.snap->ps.stats[ stSkills ] & ( 1 << i ) ) ) {
		return qfalse;
	}
	if(i == 1 && cg.snap->ps.powerups[PW_SKILLS] & USABLE_SKILL1){usable = qtrue;}
	if(i == 2 && cg.snap->ps.powerups[PW_SKILLS] & USABLE_SKILL2){usable = qtrue;}
	if(i == 3 && cg.snap->ps.powerups[PW_SKILLS] & USABLE_SKILL3){usable = qtrue;}
	if(i == 4 && cg.snap->ps.powerups[PW_SKILLS] & USABLE_SKILL4){usable = qtrue;}
	if(i == 5 && cg.snap->ps.powerups[PW_SKILLS] & USABLE_SKILL5){usable = qtrue;}
	if(i == 6 && cg.snap->ps.powerups[PW_SKILLS] & USABLE_SKILL6){usable = qtrue;}
	if(!usable){return qfalse;}
	return qtrue;
}

/*
===============
CG_StepWeaponSelect

Walks the skill list one usable entry in 'step' direction, wrapping, and
returns where it lands. Same predicate and same order as PM_WeaponSelectable
walks it server side, which is what keeps the highlighted skill and the skill
that fires from naming different things.
===============
*/
static int CG_StepWeaponSelect( int from, int step ) {
	int	weapon;
	int	i;

	weapon = from;
	for ( i = 0 ; i < 15 ; i++ ) {
		weapon += step;
		if ( weapon > 15 ) { weapon = 1; }
		if ( weapon < 1 ) { weapon = 15; }
		if ( CG_WeaponSelectable( weapon ) ) {
			return weapon;
		}
	}
	return from;
}

/*
===============
CG_TierSelectHeld

True while the fighter holds the power level button, where PM_CheckContextOperations
rewires a wheel notch into a tier change. Stepping the skill list locally would
eat that notch, so a held power level button keeps the relative selection mode
the tier path is written against.
===============
*/
static qboolean CG_TierSelectHeld( void ) {
	usercmd_t	cmd;

	if ( !trap_GetUserCmd( trap_GetCurrentCmdNumber(), &cmd ) ) {
		return qfalse;
	}
	return ( cmd.buttons & BUTTON_POWERLEVEL ) ? qtrue : qfalse;
}

/*
===============
CG_SelectWeaponStep

One wheel notch. The selection is stepped here rather than on the far side of a
round trip because the selection mode byte carries a direction and not a count:
every notch that arrives before the server answers used to collapse into the
single step the byte could say, so a fast scroll landed short of the skill the
player picked. Stepping locally and sending the resulting index outright makes
a notch worth exactly one skill however fast they come.
===============
*/
static void CG_SelectWeaponStep( int step ) {
	if ( !cg.snap || cg.snap->ps.bitFlags & usingMelee ) {
		return;
	}
	if ( cg.snap->ps.pm_flags & PMF_FOLLOW ) {
		return;
	}
	cg.weaponSelectTime = cg.time;
	if ( CG_TierSelectHeld() ) {
		cg.weaponSelectionMode = ( step > 0 ) ? 2 : 1;
		return;
	}
	cg.weaponSelect = CG_StepWeaponSelect( cg.weaponSelect, step );
	cg.weaponDesired = cg.weaponSelect;
	cg.weaponSelectionMode = 3;
	cg.drawWeaponBar = 1;
}

/*
===============
CG_UpdateWeaponSelect

Keeps the skill the bar highlights and the skill a shot comes out of the same
thing. The predicted state is what fires, so the highlight follows it outright;
a notch the player has just taken holds the highlight on what they asked for
until the prediction answers. That is what lets a change pmove refuses - while
soaring, mid-charge, or with the skill's requirements lapsed - fall back on its
own instead of leaving the bar naming a skill that will not fire.
===============
*/
void CG_UpdateWeaponSelect( void ) {
	if ( !cg.snap || cg.predictedPlayerState.weapon <= WP_NONE ) {
		return;
	}
	if ( cg.weaponSelectionMode == 1 || cg.weaponSelectionMode == 2 ) {
		return;		// a relative notch is the tier path's, not ours
	}
	if ( cg.weaponDesired > 0 ) {
		if ( cg.predictedPlayerState.weapon != cg.weaponDesired
			&& cg.time - cg.weaponSelectTime <= WEAPON_REQUEST_TIME ) {
			return;
		}
		cg.weaponDesired = -1;
		cg.weaponSelectionMode = 0;
	}
	cg.weaponSelect = cg.predictedPlayerState.weapon;
}

/*
===============
CG_NextWeapon_f
===============
*/
void CG_NextWeapon_f( void ) {
	CG_SelectWeaponStep( 1 );
}

/*
===============
CG_PrevWeapon_f
===============
*/
void CG_PrevWeapon_f( void ) {
	CG_SelectWeaponStep( -1 );
}



/*
===============
CG_Weapon_f
===============
*/
void CG_Weapon_f( void ) {
	int		num;

	if ( !cg.snap ) {
		return;
	}
	if ( cg.snap->ps.pm_flags & PMF_FOLLOW ) {
		return;
	}
	num = atoi( CG_Argv( 1 ) );

	if ( num < 1 || num > 15 ) {
		return;
	}
	if ( !( cg.snap->ps.stats[ stSkills ] & ( 1 << num ) ) ) {
		return;		// not a skill this fighter has at all
	}
	cg.weaponSelectTime = cg.time;
	cg.weaponDesired = num;
	cg.weaponSelectionMode = 3;
	cg.drawWeaponBar = 1;
	// A skill whose requirements have lapsed is still asked for, because the
	// refusal and its reason are what the player is owed - dropping the bind
	// here is exactly the silence this pass exists to end. The highlight stays
	// on what fires; the deny mark lands on the icon that was asked for.
	if ( CG_WeaponSelectable( num ) ) {
		cg.weaponSelect = num;
	}
}

	/*
===================================================================================================

WEAPON EVENTS

===================================================================================================
*/

/*
================
CG_FireWeapon

Caused by an EV_FIRE_WEAPON event
================
*/
void CG_FireWeapon( centity_t *cent, qboolean altFire ) {
	cg_userWeapon_t		*weaponGraphics;
	entityState_t		*ent;
	int					c, weapNr;

	ent = &cent->currentState;

	if ( ent->weapon == WP_NONE ) {
		return;
	}

	// Set the muzzle flash weapon Nr
	if ( altFire ) {
		weapNr = ent->weapon + ALTWEAPON_OFFSET;
	} else {
		weapNr = ent->weapon;
	}
	weaponGraphics = CG_FindUserWeaponGraphics(ent->clientNum, weapNr );
	
	// mark the entity as muzzle flashing, so when it is added it will
	// append the flash to the weapon model.
	cent->muzzleFlashTime = cg.time;

	if ( weaponGraphics->flashParticleSystem[0] ) {
		PSys_SpawnCachedSystem( weaponGraphics->flashParticleSystem, cent->lerpOrigin, NULL, cent, weaponGraphics->chargeTag[0], qfalse, qfalse );
	}

	// play a sound
	for ( c = 0 ; c < 4 ; c++ ) {
		if ( weaponGraphics->flashSound[c] == 0 ) {
			break;
		}
	}
	
	if ( c > 0 ) {
		c = rand() % c;
		if ( weaponGraphics->flashSound[c] )
		{
			trap_S_StartSound( NULL , ent->number, CHAN_WEAPON, weaponGraphics->flashSound[c] );
		}
	}

	for ( c = 0 ; c < 4 ; c++ ) {
		if ( weaponGraphics->voiceSound[c] == 0 ) {
			break;
		}
	}
	
	if ( c > 0 ) {
		c = rand() % c;
		if ( weaponGraphics->voiceSound[c] )
		{
			trap_S_StartSound( NULL , ent->number, CHAN_WEAPON, weaponGraphics->voiceSound[c] );
		}
	}



}

/*
==================
CG_UserRailTrail
==================
Caused by an EV_RAILTRAIL event
*/
void CG_UserRailTrail( int weapon, int clientNum, vec3_t start, vec3_t end) {
	cg_userWeapon_t		*weaponGraphics;
	localEntity_t		*le;
	refEntity_t			*re;
	
	weaponGraphics = CG_FindUserWeaponGraphics( clientNum, weapon );

	if ( !weaponGraphics->missileTrailShader ) {
		return;
	}

	le = CG_AllocLocalEntity();
	re = &le->refEntity;
 
	le->leType = LE_FADE_RGB;
	le->startTime = cg.time;
	le->endTime = cg.time + cg_railTrailTime.value;
	le->lifeRate = 1.0 / (le->endTime - le->startTime);
 
	re->shaderTime = cg.time / 1000.0f;
	re->reType = RT_RAIL_CORE;
	re->customShader = weaponGraphics->missileTrailShader;
 
	VectorCopy(start, re->origin);
	VectorCopy(end, re->oldorigin);
 
	re->shaderRGBA[0] = 255;
    re->shaderRGBA[1] = 255;
    re->shaderRGBA[2] = 255;
    re->shaderRGBA[3] = 255;

	le->color[0] = 0.75;
	le->color[1] = 0.75;
	le->color[2] = 0.75;
	le->color[3] = 1.0f;

	AxisClear( re->axis );
 
	/*
	// nudge the rail down just a tiny bit
	re->origin[2] -=8
	re->oldorigin[2] -= 8;
	*/
}


/*
======================
CG_UserMissileHitWall
======================
Caused by an EV_MISSILE_MISS event
*/
void CG_UserMissileHitWall( int weapon, int clientNum, int powerups, int number, vec3_t origin, vec3_t dir, qboolean inAir ) {
	cg_userWeapon_t		*weaponGraphics;
	vec3_t end;
	trace_t tr;
	//qhandle_t			mark;
	int					c;

	weaponGraphics = CG_FindUserWeaponGraphics( clientNum, weapon );
	//mark = cgs.media.burnMarkShader;

	
	// play an explosion sound
	for ( c = 0 ; c < 4 ; c++ ) {
		if ( !weaponGraphics->explosionSound[c] ) {
			break;
		}
	}
	if ( c > 0 ) {
		c = rand() % c;
		if ( weaponGraphics->explosionSound[c] )
		{
			trap_S_StartSound( origin, ENTITYNUM_WORLD, CHAN_AUTO, weaponGraphics->explosionSound[c] );
		}
	}

	// Create Explosion
	CG_MakeUserExplosion( origin, dir, weaponGraphics, powerups, number);

	if ( !inAir ) {
		vec3_t tempAxis[3];
		VectorNormalize2( dir, tempAxis[0] );

		MakeNormalVectors( tempAxis[0], tempAxis[1], tempAxis[2] );

		VectorCopy(origin, end);
		end[2] -= 64;
		CG_Trace( &tr, origin, NULL, NULL, end, -1, MASK_PLAYERSOLID );
		
		if (!weaponGraphics->noRockDebris){
			if (cg_particlesQuality.value == 2) {
				if (tr.surfaceFlags & SURF_METALSTEPS){
					if(weaponGraphics->explosionSize <= 10){
						PSys_SpawnCachedSystem( "SmallExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 25){
						PSys_SpawnCachedSystem( "NormalExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 50){
						PSys_SpawnCachedSystem( "LargeExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else{
						PSys_SpawnCachedSystem( "ExtraLargeExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}
				} else if (tr.surfaceFlags & SURF_FLESH){
					if(weaponGraphics->explosionSize <= 10){
						PSys_SpawnCachedSystem( "SmallExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 25){
						PSys_SpawnCachedSystem( "NormalExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 50){
						PSys_SpawnCachedSystem( "LargeExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else{
						PSys_SpawnCachedSystem( "ExtraLargeExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}
				} else if (tr.surfaceFlags & SURF_DUST){
					if(weaponGraphics->explosionSize <= 10){
						PSys_SpawnCachedSystem( "SmallExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 25){
						PSys_SpawnCachedSystem( "NormalExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 50){
						PSys_SpawnCachedSystem( "LargeExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else{
						PSys_SpawnCachedSystem( "ExtraLargeExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}
				} else {
					if(weaponGraphics->explosionSize <= 10){
						PSys_SpawnCachedSystem( "SmallExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 25){
						PSys_SpawnCachedSystem( "NormalExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 50){
						PSys_SpawnCachedSystem( "LargeExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else{
						PSys_SpawnCachedSystem( "ExtraLargeExplosionDebris", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}
				}
			}else if (cg_particlesQuality.value == 1){
				if (tr.surfaceFlags & SURF_METALSTEPS){
					if(weaponGraphics->explosionSize <= 10){
						PSys_SpawnCachedSystem( "SmallExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 25){
						PSys_SpawnCachedSystem( "NormalExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 50){
						PSys_SpawnCachedSystem( "LargeExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else{
						PSys_SpawnCachedSystem( "ExtraLargeExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}
				} else if (tr.surfaceFlags & SURF_FLESH){
					if(weaponGraphics->explosionSize <= 10){
						PSys_SpawnCachedSystem( "SmallExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 25){
						PSys_SpawnCachedSystem( "NormalExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 50){
						PSys_SpawnCachedSystem( "LargeExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else{
						PSys_SpawnCachedSystem( "ExtraLargeExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}
				} else if (tr.surfaceFlags & SURF_DUST){
					if(weaponGraphics->explosionSize <= 10){
						PSys_SpawnCachedSystem( "SmallExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 25){
						PSys_SpawnCachedSystem( "NormalExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 50){
						PSys_SpawnCachedSystem( "LargeExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else{
						PSys_SpawnCachedSystem( "ExtraLargeExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}
				} else {
					if(weaponGraphics->explosionSize <= 10){
						PSys_SpawnCachedSystem( "SmallExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 25){
						PSys_SpawnCachedSystem( "NormalExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else if(weaponGraphics->explosionSize <= 50){
						PSys_SpawnCachedSystem( "LargeExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}else{
						PSys_SpawnCachedSystem( "ExtraLargeExplosionDebrisLow", origin, tempAxis, NULL, NULL, qfalse, qfalse );
					}
				}
			}
		}

		if (weaponGraphics->markSize && weaponGraphics->markShader) {
			// Draw Impactmark
			CG_ImpactMark( weaponGraphics->markShader, origin, dir, random()*360, 1,1,1,1, qfalse,60, qfalse );
		}
	}

	
/* NOTE: Find another way of doing this in the new system...
	if ( weaponGraphics->missileTrailFunc == CG_TrailFunc_StraightBeam ||
		 weaponGraphics->missileTrailFunc == CG_TrailFunc_SpiralBeam ) {
		CG_CreateStraightBeamFade( cgs.clientinfo[ clientNum ].weaponTagPos0, origin, weaponGraphics);
	}
*/

}

void CG_UserMissileHitPlayer( int weapon, int clientNum, int powerups, int number, vec3_t origin, vec3_t dir, int entityNum ) {
	CG_Bleed( origin, entityNum );
	CG_UserMissileHitWall( weapon, clientNum, powerups, number, origin, dir, qtrue );
}
/*
===============
CG_Tracer
===============
*/
void CG_Tracer( vec3_t source, vec3_t dest ) {
	vec3_t		forward, right;
	polyVert_t	verts[4];
	vec3_t		line;
	float		len, begin, end;
	vec3_t		start, finish;
	vec3_t		midpoint;

	// tracer
	VectorSubtract( dest, source, forward );
	len = VectorNormalize( forward );

	// start at least a little ways from the muzzle
	if ( len < 100 ) {
		return;
	}
	begin = 50 + random() * (len - 60);
	end = begin + cg_tracerLength.value;
	if ( end > len ) {
		end = len;
	}
	VectorMA( source, begin, forward, start );
	VectorMA( source, end, forward, finish );

	line[0] = DotProduct( forward, cg.refdef.viewaxis[1] );
	line[1] = DotProduct( forward, cg.refdef.viewaxis[2] );

	VectorScale( cg.refdef.viewaxis[1], line[1], right );
	VectorMA( right, -line[0], cg.refdef.viewaxis[2], right );
	VectorNormalize( right );

	VectorMA( finish, cg_tracerWidth.value, right, verts[0].xyz );
	verts[0].st[0] = 0;
	verts[0].st[1] = 1;
	verts[0].modulate[0] = 255;
	verts[0].modulate[1] = 255;
	verts[0].modulate[2] = 255;
	verts[0].modulate[3] = 255;

	VectorMA( finish, -cg_tracerWidth.value, right, verts[1].xyz );
	verts[1].st[0] = 1;
	verts[1].st[1] = 0;
	verts[1].modulate[0] = 255;
	verts[1].modulate[1] = 255;
	verts[1].modulate[2] = 255;
	verts[1].modulate[3] = 255;

	VectorMA( start, -cg_tracerWidth.value, right, verts[2].xyz );
	verts[2].st[0] = 1;
	verts[2].st[1] = 1;
	verts[2].modulate[0] = 255;
	verts[2].modulate[1] = 255;
	verts[2].modulate[2] = 255;
	verts[2].modulate[3] = 255;

	VectorMA( start, cg_tracerWidth.value, right, verts[3].xyz );
	verts[3].st[0] = 0;
	verts[3].st[1] = 0;
	verts[3].modulate[0] = 255;
	verts[3].modulate[1] = 255;
	verts[3].modulate[2] = 255;
	verts[3].modulate[3] = 255;

	trap_R_AddPolyToScene( cgs.media.tracerShader, 4, verts );

	midpoint[0] = ( start[0] + finish[0] ) * 0.5;
	midpoint[1] = ( start[1] + finish[1] ) * 0.5;
	midpoint[2] = ( start[2] + finish[2] ) * 0.5;

}


/*
======================
CG_CalcMuzzlePoint
======================
*/
static qboolean	CG_CalcMuzzlePoint( int entityNum, vec3_t muzzle ) {
	vec3_t		forward;
	centity_t	*cent;
	int			anim;

	if ( entityNum == cg.snap->ps.clientNum ) {
		VectorCopy( cg.snap->ps.origin, muzzle );
		muzzle[2] += cg.snap->ps.viewheight;
		AngleVectors( cg.snap->ps.viewangles, forward, NULL, NULL );
		VectorMA( muzzle, 14, forward, muzzle );
		return qtrue;
	}

	cent = &cg_entities[entityNum];
	if ( !cent->currentValid ) {
		return qfalse;
	}

	VectorCopy( cent->currentState.pos.trBase, muzzle );

	AngleVectors( cent->currentState.apos.trBase, forward, NULL, NULL );
//	anim = cent->currentState.legsAnim & ~ANIM_TOGGLEBIT;
//	if ( anim == ANIM_WALKCR || anim == ANIM_IDLECR ) {
//		muzzle[2] += CROUCH_VIEWHEIGHT;
//	} else {
		muzzle[2] += DEFAULT_VIEWHEIGHT;
//	}

	VectorMA( muzzle, 14, forward, muzzle );

	return qtrue;

}
