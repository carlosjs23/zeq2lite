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
// cg_draw.c -- draw all of the graphical elements during
// active (after loading) gameplay

#include "cg_local.h"

#ifdef MISSIONPACK
#include "../UI/ui_shared.h"

// used for scoreboard
extern displayContextDef_t cgDC;
menuDef_t *menuScoreboard = NULL;
#else
int drawTeamOverlayModificationCount = -1;
#endif

int sortedTeamPlayers[TEAM_MAXOVERLAY];
int	numSortedTeamPlayers;

char systemChat[256];
char teamChat1[256];
char teamChat2[256];

#ifdef MISSIONPACK

int CG_Text_Width(const char *text, float scale, int limit) {
  int count,len;
	float out;
	glyphInfo_t *glyph;
	float useScale;
// FIXME: see ui_main.c, same problem
//	const unsigned char *s = text;
	const char *s = text;
	fontInfo_t *font = &cgDC.Assets.textFont;
	if (scale <= cg_smallFont.value) {
		font = &cgDC.Assets.smallFont;
	} else if (scale > cg_bigFont.value) {
		font = &cgDC.Assets.bigFont;
	}
	useScale = scale * font->glyphScale;
  out = 0;
  if (text) {
    len = strlen(text);
		if (limit > 0 && len > limit) {
			len = limit;
		}
		count = 0;
		while (s && *s && count < len) {
			if ( Q_IsColorString(s) ) {
				s += 2;
				continue;
			} else {
				glyph = &font->glyphs[(int)*s]; // TTimo: FIXME: getting nasty warnings without the cast, hopefully this doesn't break the VM build
				out += glyph->xSkip;
				s++;
				count++;
			}
    }
  }
  return out * useScale;
}

int CG_Text_Height(const char *text, float scale, int limit) {
  int len, count;
	float max;
	glyphInfo_t *glyph;
	float useScale;
// TTimo: FIXME
//	const unsigned char *s = text;
	const char *s = text;
	fontInfo_t *font = &cgDC.Assets.textFont;
	if (scale <= cg_smallFont.value) {
		font = &cgDC.Assets.smallFont;
	} else if (scale > cg_bigFont.value) {
		font = &cgDC.Assets.bigFont;
	}
	useScale = scale * font->glyphScale;
  max = 0;
  if (text) {
    len = strlen(text);
		if (limit > 0 && len > limit) {
			len = limit;
		}
		count = 0;
		while (s && *s && count < len) {
			if ( Q_IsColorString(s) ) {
				s += 2;
				continue;
			} else {
				glyph = &font->glyphs[(int)*s]; // TTimo: FIXME: getting nasty warnings without the cast, hopefully this doesn't break the VM build
	      if (max < glyph->height) {
		      max = glyph->height;
			  }
				s++;
				count++;
			}
    }
  }
  return max * useScale;
}

void CG_Text_PaintChar(float x, float y, float width, float height, float scale, float s, float t, float s2, float t2, qhandle_t hShader) {
  float w, h;
  w = width * scale;
  h = height * scale;
  // Glyphs come from a fixed-cell texture, so they scale uniformly like any
  // other HUD element; stretching them is what made text fat at 16:9.
  CG_AdjustFrom640( &x, &y, &w, &h,qfalse);
  trap_R_DrawStretchPic( x, y, w, h, s, t, s2, t2, hShader );
}

void CG_Text_Paint(float x, float y, float scale, vec4_t color, const char *text, float adjust, int limit, int style) {
  int len, count;
	vec4_t newColor;
	glyphInfo_t *glyph;
	float useScale;
	fontInfo_t *font = &cgDC.Assets.textFont;
	if (scale <= cg_smallFont.value) {
		font = &cgDC.Assets.smallFont;
	} else if (scale > cg_bigFont.value) {
		font = &cgDC.Assets.bigFont;
	}
	useScale = scale * font->glyphScale;
  if (text) {
// TTimo: FIXME
//		const unsigned char *s = text;
		const char *s = text;
		trap_R_SetColor( color );
		memcpy(&newColor[0], &color[0], sizeof(vec4_t));
    len = strlen(text);
		if (limit > 0 && len > limit) {
			len = limit;
		}
		count = 0;
		while (s && *s && count < len) {
			glyph = &font->glyphs[(int)*s]; // TTimo: FIXME: getting nasty warnings without the cast, hopefully this doesn't break the VM build
      //int yadj = Assets.textFont.glyphs[text[i]].bottom + Assets.textFont.glyphs[text[i]].top;
      //float yadj = scale * (Assets.textFont.glyphs[text[i]].imageHeight - Assets.textFont.glyphs[text[i]].height);
			if ( Q_IsColorString( s ) ) {
				memcpy( newColor, g_color_table[ColorIndex(*(s+1))], sizeof( newColor ) );
				newColor[3] = color[3];
				trap_R_SetColor( newColor );
				s += 2;
				continue;
			} else {
				float yadj = useScale * glyph->top;
				if (style == ITEM_TEXTSTYLE_SHADOWED || style == ITEM_TEXTSTYLE_SHADOWEDMORE) {
					int ofs = style == ITEM_TEXTSTYLE_SHADOWED ? 1 : 2;
					colorBlack[3] = newColor[3];
					trap_R_SetColor( colorBlack );
					CG_Text_PaintChar(x + ofs, y - yadj + ofs, 
														glyph->imageWidth,
														glyph->imageHeight,
														useScale, 
														glyph->s,
														glyph->t,
														glyph->s2,
														glyph->t2,
														glyph->glyph);
					colorBlack[3] = 1.0;
					trap_R_SetColor( newColor );
				}
				CG_Text_PaintChar(x, y - yadj, 
													glyph->imageWidth,
													glyph->imageHeight,
													useScale, 
													glyph->s,
													glyph->t,
													glyph->s2,
													glyph->t2,
													glyph->glyph);
				// CG_DrawPic(qfalse,x, y - yadj, scale * cgDC.Assets.textFont.glyphs[text[i]].imageWidth, scale * cgDC.Assets.textFont.glyphs[text[i]].imageHeight, cgDC.Assets.textFont.glyphs[text[i]].glyph);
				x += (glyph->xSkip * useScale) + adjust;
				s++;
				count++;
			}
    }
	  trap_R_SetColor( NULL );
  }
}


#endif

/*
==============
CG_DrawField

Draws large numbers for status bar and powerups
==============
*/
#ifndef MISSIONPACK
static void CG_DrawField (int x, int y, int width, int value) {
	char	num[16], *ptr;
	int		l;
	int		frame;

	if ( width < 1 ) {
		return;
	}

	// draw number string
	if ( width > 5 ) {
		width = 5;
	}

	switch ( width ) {
	case 1:
		value = value > 9 ? 9 : value;
		value = value < 0 ? 0 : value;
		break;
	case 2:
		value = value > 99 ? 99 : value;
		value = value < -9 ? -9 : value;
		break;
	case 3:
		value = value > 999 ? 999 : value;
		value = value < -99 ? -99 : value;
		break;
	case 4:
		value = value > 9999 ? 9999 : value;
		value = value < -999 ? -999 : value;
		break;
	}

	Com_sprintf (num, sizeof(num), "%i", value);
	l = strlen(num);
	if (l > width)
		l = width;
	x += 2 + CHAR_WIDTH*(width - l);

	ptr = num;
	while (*ptr && l)
	{
		if (*ptr == '-')
			frame = STAT_MINUS;
		else
			frame = *ptr -'0';

		CG_DrawPic(qfalse, x,y, CHAR_WIDTH, CHAR_HEIGHT, cgs.media.numberShaders[frame] );
		x += CHAR_WIDTH;
		ptr++;
		l--;
	}
}
#endif // !MISSIONPACK


/*
=================
CG_DrawHorGauge

=================
*/
void CG_DrawHorGauge( float x, float y, float w, float h, vec4_t color_bar, vec4_t color_empty, int value, int maxvalue, qboolean reversed) {
	float pct;
	float bar_w;

	pct = (float)value / (float)maxvalue;
	bar_w = w * pct;

	if (color_empty[3]) {
		trap_R_SetColor( color_empty );
		if (!reversed) {
			CG_DrawPic(qfalse, x + bar_w, y, w - bar_w, h, cgs.media.whiteShader );
		} else {
			CG_DrawPic(qfalse, x - bar_w, y, w + bar_w, h, cgs.media.whiteShader );
		}
	}
	trap_R_SetColor( color_bar );

	if (bar_w > w) {
		bar_w = w;
	}

	if (bar_w == 0) {
		trap_R_SetColor( NULL );
		return;
	}

	if (!reversed) {
		CG_DrawPic(qfalse, x, y, bar_w, h, cgs.media.whiteShader );
	} else {
		CG_DrawPic(qfalse, x + w - bar_w, y, bar_w, h, cgs.media.whiteShader );
	}

	trap_R_SetColor( NULL );
}
void CG_DrawDiffGauge(float x,float y,float width,float height,vec4_t color,vec4_t empty,int base,int value,int maxValue,int direction){
	float percent;
	int newWidth,newX,difference;
	difference = base - value;
	if(difference > 0 && direction <= 0){return;}
	if(difference < 0 && direction >= 0){return;}
	percent = ((float)value / (float)maxValue);
	newX = x + ((float)(width * percent));
	newWidth = ((float)difference / (float)maxValue) * width;
	CG_DrawHorGauge(newX,y,newWidth,height,color,empty,1,1,qfalse);
}
void CG_DrawRightGauge(float x,float y,float width,float height,vec4_t color,vec4_t empty,int value,int maxValue){
	float percent;
	int newWidth,newX,change;
	percent = ((float)value / (float)maxValue);
	change = width * percent;
	newX = x + change;
	newWidth = width - change;
	CG_DrawHorGauge(newX,y,newWidth,height,color,empty,1,1,qfalse);
}
void CG_DrawReverseGauge(float x,float y,float width,float height,vec4_t color,vec4_t empty,int value,int maxValue){
	float percent;
	int newWidth,newHeight,newX,newY;
	percent = (float)value / (float)maxValue;
	newWidth = width * percent;
	newX = (x + width) - newWidth;
	newHeight = height * percent;
	newY = (y + height) - newHeight;
	CG_DrawHorGauge(newX,newY,newWidth,newHeight,color,empty,1,1,qfalse);
}

/*
==================
CG_DrawVertGauge

==================
*/
void CG_DrawVertGauge( float x, float y, float w, float h, vec4_t color_bar, vec4_t color_empty, int value, int maxvalue, qboolean reversed) {
	float pct;
	float bar_h;

	pct = (float)value / (float)maxvalue;
	bar_h = h * pct;

	if (color_empty[3]) {
		trap_R_SetColor( color_empty );
		if (reversed) {
			CG_DrawPic(qfalse, x, y + bar_h, w, h - bar_h, cgs.media.whiteShader );
		} else {
			CG_DrawPic(qfalse, x, y, w, h - bar_h, cgs.media.whiteShader );
		}
	}
	trap_R_SetColor( color_bar );

	if (bar_h > h) {
		bar_h = h;
	}

	if (bar_h == 0) {
		trap_R_SetColor( NULL );
		return;
	}

	if (reversed) {
		CG_DrawPic(qfalse, x, y, w, bar_h, cgs.media.whiteShader );
	} else {
		CG_DrawPic(qfalse, x, y + h - bar_h, w, bar_h, cgs.media.whiteShader );
	}

	trap_R_SetColor( NULL );
}



/*
================
CG_Draw3DModel

================
*/
void CG_Draw3DModel( float x, float y, float w, float h, qhandle_t model, qhandle_t skin, vec3_t origin, vec3_t angles ) {
	refdef_t		refdef;
	refEntity_t		ent;
	// A model rendered into a stretched viewport comes out squashed, so the
	// 2D box it is drawn into keeps its aspect.
	CG_AdjustFrom640( &x, &y, &w, &h,qfalse);
	memset( &refdef, 0, sizeof( refdef ) );
	memset( &ent, 0, sizeof( ent ) );
	AnglesToAxis( angles, ent.axis );
	VectorCopy( origin, ent.origin );
	ent.hModel = model;
	ent.customSkin = skin;
	ent.renderfx = RF_NOSHADOW | RF_DEPTHHACK | RF_LIGHTING_ORIGIN;
	refdef.rdflags = RDF_NOWORLDMODEL;
	AxisClear(refdef.viewaxis);
	refdef.fov_x = 30;
	refdef.fov_y = 30;
	refdef.x = x;
	refdef.y = y;
	refdef.width = w;
	refdef.height = h;
	refdef.time = cg.time;
	trap_R_ClearScene();
	trap_R_AddRefEntityToScene(&ent);
	trap_R_RenderScene(&refdef);
}

/*
================
CG_DrawHead

Used for both the status bar and the scoreboard
================
*/
void CG_DrawHead( float x, float y, float w, float h, int clientNum, vec3_t headAngles ) {
	clipHandle_t	cm;
	clientInfo_t	*ci;
	float			len;
	vec3_t			origin;
	vec3_t			mins, maxs;
	tierConfig_cg	*tier;
	qhandle_t		icon;
	ci = &cgs.clientinfo[clientNum];
	tier = &ci->tierConfig[ci->tierCurrent];
	if(!cg_draw3dIcons.integer){
		icon = tier->icon2D[0];
		CG_DrawPic(qfalse, x, y, w, h,icon);
	}
	else{
		cm = ci->headModel[ci->tierCurrent];
		if(!cm){return;}
		trap_R_ModelBounds(cm,mins,maxs,0);
		len = 0.7 * ( maxs[2] - mins[2] );		
		origin[0] = len / (1.0 - tier->icon3DZoom);
		origin[1] = 0.5 * (mins[1] + maxs[1]);
		origin[2] = -0.5 * (mins[2] + maxs[2]);
		headAngles[0] = tier->icon3DRotation[0];
		headAngles[1] = tier->icon3DRotation[1];
		headAngles[2] = tier->icon3DRotation[2];
		CG_Draw3DModel(x+tier->icon3DOffset[0],y+tier->icon3DOffset[1],w+tier->icon3DSize[0],h+tier->icon3DSize[1],ci->headModel[ci->tierCurrent],ci->headSkin[ci->tierCurrent],origin,headAngles);
	}
}

/*
================
CG_DrawStatusBarHead

================
*/
#ifndef MISSIONPACK

static void CG_DrawStatusBarHead( float x ) {
	vec3_t		angles;
	float		size, stretch;
	float		frac;

	VectorClear( angles );

	if ( cg.damageTime && cg.time - cg.damageTime < DAMAGE_TIME ) {
		frac = (float)(cg.time - cg.damageTime ) / DAMAGE_TIME;
		size = ICON_SIZE * 1.25 * ( 1.5 - frac * 0.5 );

		stretch = size - ICON_SIZE * 1.25;
		// kick in the direction of damage
		x -= stretch * 0.5 + cg.damageX * stretch * 0.5;

		cg.headStartYaw = 180 + cg.damageX * 45;

		cg.headEndYaw = 180 + 20 * cos( crandom()*M_PI );
		cg.headEndPitch = 5 * cos( crandom()*M_PI );

		cg.headStartTime = cg.time;
		cg.headEndTime = cg.time + 100 + random() * 2000;
	} else {
		if ( cg.time >= cg.headEndTime ) {
			// select a new head angle
			cg.headStartYaw = cg.headEndYaw;
			cg.headStartPitch = cg.headEndPitch;
			cg.headStartTime = cg.headEndTime;
			cg.headEndTime = cg.time + 100 + random() * 2000;

			cg.headEndYaw = 180 + 20 * cos( crandom()*M_PI );
			cg.headEndPitch = 5 * cos( crandom()*M_PI );
		}

		size = ICON_SIZE * 1.25;
	}

	// if the server was frozen for a while we may have a bad head start time
	if ( cg.headStartTime > cg.time ) {
		cg.headStartTime = cg.time;
	}

	frac = ( cg.time - cg.headStartTime ) / (float)( cg.headEndTime - cg.headStartTime );
	frac = frac * frac * ( 3 - 2 * frac );
	angles[YAW] = cg.headStartYaw + ( cg.headEndYaw - cg.headStartYaw ) * frac;
	angles[PITCH] = cg.headStartPitch + ( cg.headEndPitch - cg.headStartPitch ) * frac;

	CG_DrawHead( x, 480 - size, size, size, 
				cg.snap->ps.clientNum, angles );
}
#endif // MISSIONPACK

/*
================
CG_DrawTeamBackground

================
*/
void CG_DrawTeamBackground( int x, int y, int w, int h, float alpha, int team )
{
	vec4_t		hcolor;

	hcolor[3] = alpha;
	if ( team == TEAM_RED ) {
		hcolor[0] = 1;
		hcolor[1] = 0;
		hcolor[2] = 0;
	} else if ( team == TEAM_BLUE ) {
		hcolor[0] = 0;
		hcolor[1] = 0;
		hcolor[2] = 1;
	} else {
		return;
	}
	trap_R_SetColor( hcolor );
	trap_R_SetColor( NULL );
}


/*================
CG_DRAWCHAT
================*/
void strrep(char *str, char old, char new)  {
    char *pos;
    while(1){
        pos = strchr(str, old);
        if (pos == NULL){
            break;
        }
        *pos = new;
    }
}
void CG_CheckChat(void){
	int index,offset;
	vec3_t angles;
	int yStart = cg.predictedPlayerState.lockedTarget ? 330 : 0;
	int yOffset = cg.predictedPlayerState.lockedTarget ? -40 : 40;
	if(cg.time < cgs.chatTimer){
		for(index=0;index<3;++index){
			if(cgs.messageClient[index] >= 0){
				CG_DrawPic(qfalse,-15,-15+yStart+(yOffset*index),369,75,cgs.media.chatBackgroundShader);
				CG_DrawHead(7,7+yStart+(yOffset*index),32,32,cgs.messageClient[index],angles);
				CG_DrawSmallStringCustom(45,16+yStart+(yOffset*index),8,8,cgs.messages[index],1.0,4);
			}
		}
	}
	else{
		for(index=0;index<3;++index){
			cgs.messageClient[index] = -1;
			strcpy(cgs.messages[index],"");
		}
	}
}
void CG_DrawChat(char *text){
	int clientNum,index,safeIndex;
	char cleaned[256];
	char name[14];
	char *safe;
	char find = ':';
	char find2[] = "^7";
	char replace = ' ';
	safe = text;
	strrep(safe, find, replace);
	strcpy(cleaned, text);
	strrep(safe, *find2, replace);
	cgs.chatTimer = cg.time + cg_chatTime.integer;
	strcpy(name,COM_Parse(&safe));
	for(safeIndex=0; safeIndex<3; ++safeIndex){if(!strcmp(cgs.messages[safeIndex],"")){break;}}
	if(safeIndex>=2 && cgs.messageClient[2]>= 0){
		safeIndex = 2;
		cgs.messageClient[0] = cgs.messageClient[1];
		cgs.messageClient[1] = cgs.messageClient[2];
		strcpy(cgs.messages[0],cgs.messages[1]);
		strcpy(cgs.messages[1],cgs.messages[2]);
	}
	for(clientNum=0;clientNum<MAX_CLIENTS;++clientNum){
		if(!strcmp(name,cgs.clientinfo[clientNum].name)){
			cgs.messageClient[safeIndex] = clientNum;
			break;
		}
	}
	strcpy(cgs.messages[safeIndex],cleaned);
}
void CG_DrawScreenEffects(){
	clientInfo_t	*ci;
	tierConfig_cg	*tier;
	playerState_t	*ps;
	qhandle_t		effect;
	int				state;
	ci = &cgs.clientinfo[cg.snap->ps.clientNum];
	tier = &ci->tierConfig[ci->tierCurrent];
	ps = &cg.snap->ps;
	// damageTextureState is 1 based: it holds "damage state + 1" and is reset
	// to 0 on spawn. Subtracting 1 unguarded indexed screenEffect[-1], which
	// reads the qhandle_t in front of the struct and drew that garbage shader
	// over the whole screen.
	state = ci->damageTextureState - 1;
	if(state < 0){state = 0;}
	if(state >= (int)ARRAY_LEN(tier->screenEffect)){state = ARRAY_LEN(tier->screenEffect)-1;}
	effect = tier->screenEffect[state];
	if(ps->bitFlags & isBreakingLimit && tier->screenEffectPowering){
		effect = tier->screenEffectPowering;
	}
	else if(ps->bitFlags & isTransforming && tier->screenEffectTransforming){
		effect = tier->screenEffectTransforming;
	}
	if(!effect){return;}
	CG_DrawPic(qfalse,0,0,640,480,effect);
}
/*================
CG_Scoreboard
================*/
void CG_DrawScoreboard(){
	int		i;
	int		clientNum;
	int		y;
	vec3_t	angles;
	// the server pushes scores on its own only at intermission; while the
	// board is up, keep asking so it tracks the fight
	if(cg.scoresRequestTime + 2000 < cg.time){
		cg.scoresRequestTime = cg.time;
		trap_SendClientCommand("score");
	}
	VectorClear(angles);
	CG_DrawSmallStringHalfHeight(240,160,"Name",1.0F);
	CG_DrawSmallStringHalfHeight(320,160,"Score",1.0F);
	CG_DrawSmallStringHalfHeight(380,160,"Ping",1.0F);
	CG_DrawSmallStringHalfHeight(420,160,"Time",1.0F);
	// cg.scores is packed in rank order, so the row index is the rank and the
	// client number has to come from the entry
	for(i=0;i<cg.numScores;++i){
		clientNum = cg.scores[i].client;
		if(!cgs.clientinfo[clientNum].infoValid){continue;}
		y = (36*i)+180;
		CG_DrawHead(180,y,50,50,clientNum,angles);
		CG_DrawSmallStringHalfHeight(240,y+20,cgs.clientinfo[clientNum].name,1.0F);
		CG_DrawSmallStringHalfHeight(320,y+20,va("%i",cg.scores[i].score),1.0F);
		CG_DrawSmallStringHalfHeight(380,y+20,va("%i",cg.scores[i].ping),1.0F);
		CG_DrawSmallStringHalfHeight(420,y+20,va("%i",cg.scores[i].time),1.0F);
	}
}
/*================
CG_PowerLevelString

The number beside a gauge, scaled by the tier's hudMultiplier and abbreviated
once it runs past what the column can hold.
================*/
static const char *CG_PowerLevelString(int clientNum,int powerLevel){
	float	multiplier;
	long	display;
	multiplier = cgs.clientinfo[clientNum].tierConfig[cgs.clientinfo[clientNum].tierCurrent].hudMultiplier;
	if(multiplier <= 0){multiplier = 1.0;}
	display = (float)powerLevel * multiplier;
	return display >= 1000000 ? va("%.1f ^3mil",(float)display / 1000000.0) : va("%i",display);
}
/*================
CG_DrawHudRow

One value, one gauge: a label column, a bar, a number column. The capsule's
window sits HUD_GAUGE_INSET inside its frame, and the fill goes there first with
the frame over it - the order every gauge on this HUD is drawn in, and where the
bars get their gloss. The label is a word rather than a code or a glyph: two of
these four values are this game's own inventions, so there is no icon a player
already reads as "power level" or "limit break reserve", and an abbreviation
short enough for a narrow column only moves the guessing somewhere else. It is
tinted to the bar's colour so the row reads as one object.
================*/
static void CG_DrawHudRow(int x,int y,int height,const char *label,int value,int maxValue,vec4_t color,qhandle_t frame,const char *number){
	vec4_t	trackColor = {0.110f,0.157f,0.204f,1.0f};
	vec4_t	clearColor = {0.0f,0.0f,0.0f,0.0f};
	int		textY;
	textY = y + HUD_GAUGE_INSET + height / 2 - SMALLCHAR_HEIGHT / 4;
	CG_DrawStringExt(-1,x+HUD_LABEL_X,textY,label,color,qfalse,qtrue,SMALLCHAR_WIDTH,SMALLCHAR_HEIGHT/2,0);
	CG_DrawHorGauge(x+HUD_BAR_X,y+HUD_GAUGE_INSET,HUD_BAR_WIDTH,height,trackColor,trackColor,1,1,qfalse);
	if(maxValue > 0){
		// CG_DrawHorGauge sizes the empty segment before it clamps the bar, so an
		// over-full value has to be caught here rather than there.
		if(value > maxValue){value = maxValue;}
		if(value > 0){
			CG_DrawHorGauge(x+HUD_BAR_X,y+HUD_GAUGE_INSET,HUD_BAR_WIDTH,height,color,clearColor,value,maxValue,qfalse);
		}
	}
	CG_DrawPic(qfalse,x+HUD_BAR_X-HUD_GAUGE_INSET,y,HUD_BAR_WIDTH+2*HUD_GAUGE_INSET,height+2*HUD_GAUGE_INSET,frame);
	if(number){
		CG_DrawSmallStringHalfHeight(x+HUD_NUMBER_RIGHT-Q_PrintStrlen(number)*SMALLCHAR_WIDTH,textY,number,1.0F);
	}
}
/*================
CG_DrawHUD

Four readouts in one plate. plHealth and plFatigue used to be painted into the
power bar as coloured regions, which is what made it seven colours deep; they
are quantities on the same scale as plCurrent, so each gets its own gauge and
the power bar goes back to being a single fill.

`remote` marks the locked-on target's copy of the panel, where only the three
values carried in lockonData are known - the rest would be drawn as zero, which
reads as a real reading rather than as an absent one.
================*/
void CG_DrawHUD(playerState_t *ps,int clientNum,int x,int y,qboolean remote){
	cg_userWeapon_t	*skill;
	int			chargePercent,chargeReady;
	int			reserve,reserveFull;
	float		flash;
	qboolean	charging = ps->weaponstate == WEAPON_CHARGING || ps->weaponstate == WEAPON_ALTCHARGING ? qtrue : qfalse;
	vec4_t	powerColor = {0.118f,0.588f,1.0f,1.0f};
	vec4_t	healthColor = {0.851f,0.251f,0.251f,1.0f};
	vec4_t	staminaColor = {0.922f,0.659f,0.200f,1.0f};
	vec4_t	reserveColor = {0.400f,0.722f,0.380f,1.0f};
	vec4_t	readyColor = {0.588f,1.0f,0.0f,1.0f};
	vec4_t	chargingColor = {0.9f,0.5f,0.0f,1.0f};
	vec4_t	ruleColor = {0.275f,0.431f,0.588f,0.55f};
	vec4_t	flashColor = {1.0f,1.0f,1.0f,1.0f};
	vec4_t	clearColor = {0.0f,0.0f,0.0f,0.0f};
	vec3_t	angles;
	CG_DrawPic(qfalse,x,y,HUD_PANEL_WIDTH,HUD_PANEL_HEIGHT,cgs.media.hudPlateShader);
	if(charging){
		skill = CG_FindUserWeaponGraphics(cg.snap->ps.clientNum,cg.weaponSelect);
		CG_DrawPic(qfalse,x+HUD_PAD,y+HUD_PAD,HUD_PORTRAIT,HUD_PORTRAIT,skill->weaponIcon);
	}
	else{
		CG_DrawHead(x+HUD_PAD,y+HUD_PAD,HUD_PORTRAIT,HUD_PORTRAIT,clientNum,angles);
	}
	// The primary row carries the charge while an attack is winding up, which is
	// what the single bar always did.
	if(charging){
		if(ps->weaponstate == WEAPON_CHARGING){
			chargePercent = ps->stats[stChargePercentPrimary];
			chargeReady = ps->currentSkill[WPSTAT_CHRGREADY];
		}
		else{
			chargePercent = ps->stats[stChargePercentSecondary];
			chargeReady = ps->currentSkill[WPSTAT_ALT_CHRGREADY];
		}
		CG_DrawHudRow(x,y+HUD_ROW_PL_Y,HUD_ROW_PRIMARY,"CHRG",chargePercent,100,
			chargePercent >= chargeReady ? readyColor : chargingColor,
			cgs.media.gaugePrimaryShader,CG_PowerLevelString(clientNum,ps->attackPower));
		if(chargeReady){
			CG_DrawPic(qfalse,x+HUD_BAR_X+(int)((HUD_BAR_WIDTH-HUD_PIN_WIDTH)*(chargeReady/100.0)),
				y+HUD_ROW_PL_Y-HUD_GAUGE_INSET-10,HUD_PIN_WIDTH,HUD_PIN_HEIGHT,cgs.media.markerAscendShader);
		}
		return;
	}
	CG_DrawHudRow(x,y+HUD_ROW_PL_Y,HUD_ROW_PRIMARY,"POWER",ps->powerLevel[plCurrent],ps->powerLevel[plMaximum],
		powerColor,cgs.media.gaugePrimaryShader,CG_PowerLevelString(clientNum,ps->powerLevel[plCurrent]));
	CG_DrawHudRow(x,y+HUD_ROW_HP_Y,HUD_ROW_SECONDARY,"LIFE",ps->powerLevel[plHealth],ps->powerLevel[plMaximum],
		healthColor,cgs.media.gaugeSecondaryShader,CG_PowerLevelString(clientNum,ps->powerLevel[plHealth]));
	if(remote){return;}
	CG_DrawHudRow(x,y+HUD_ROW_ST_Y,HUD_ROW_SECONDARY,"STAM",ps->powerLevel[plFatigue],ps->powerLevel[plMaximum],
		staminaColor,cgs.media.gaugeSecondaryShader,CG_PowerLevelString(clientNum,ps->powerLevel[plFatigue]));
	// The vitals are what the body is doing; the reserve is what a limit break
	// has left to spend. The rule is what says they are different questions.
	CG_DrawHorGauge(x+HUD_LABEL_X,y+HUD_RULE_Y,HUD_NUMBER_RIGHT-HUD_LABEL_X,1,ruleColor,ruleColor,1,1,qfalse);
	if(!(ps->options & canBreakLimit)){return;}
	reserveFull = BREAKLIMIT_RESERVE_FULL(ps->powerLevel[plMaximum]);
	if(reserveFull < 1){return;}
	reserve = ps->powerLevel[plMaximumPool];
	if(reserve > reserveFull){reserve = reserveFull;}
	// While the limit is actually breaking, the animated icon takes the label
	// slot: the row it labels is the one being spent.
	CG_DrawHudRow(x,y+HUD_ROW_BL_Y,HUD_ROW_MINOR,"BURST",reserve,reserveFull,reserveColor,
		cgs.media.gaugeMinorShader,va("%i%%",(int)(100.0f * reserve / reserveFull)));
	// The limit break's own animated mark, badged on the portrait: the label
	// column says what the row is, not what it is doing right now.
	if(ps->bitFlags & isBreakingLimit){
		CG_DrawPic(qfalse,x+HUD_PAD+HUD_PORTRAIT-20,y+HUD_PAD+HUD_PORTRAIT-20,18,18,cgs.media.breakLimitShader);
	}
	if(cg.breakLimitReadyTime){
		flash = 1.0f - (float)(cg.time - cg.breakLimitReadyTime) / BREAKLIMIT_FLASH_TIME;
		if(flash > 0 && flash <= 1.0f){
			flashColor[3] = flash;
			CG_DrawHorGauge(x+HUD_BAR_X,y+HUD_ROW_BL_Y+HUD_GAUGE_INSET,HUD_BAR_WIDTH,HUD_ROW_MINOR,flashColor,clearColor,1,1,qfalse);
			CG_DrawPic(qfalse,x+HUD_BAR_X-HUD_GAUGE_INSET,y+HUD_ROW_BL_Y,HUD_BAR_WIDTH+2*HUD_GAUGE_INSET,HUD_ROW_MINOR+2*HUD_GAUGE_INSET,cgs.media.gaugeMinorShader);
		}
	}
}
/*================
CG_MeleeReadoutState

What the locked target is doing, in one word, and the colour that says whether
it is a threat or an opening. Grouped rather than enumerated: the melee has
eighteen states and a player reacting inside a 550ms window needs to know which
of three answers applies, not which state index is up.

A knockback outranks the state because it is the one that lasts and the one an
attack is free against; a bare freeze is last, since every state above already
implies its own recovery.
================*/
static const char *CG_MeleeReadoutState(int state,int status,vec4_t color){
	vec4_t	threatColor = {1.0f,0.35f,0.2f,1.0f};
	vec4_t	guardColor = {0.4f,0.72f,1.0f,1.0f};
	vec4_t	openColor = {0.588f,1.0f,0.0f,1.0f};
	Vector4Copy(threatColor,color);
	if(status & LKSTATUS_KNOCKBACK){
		Vector4Copy(openColor,color);
		return "KNOCKED";
	}
	switch(state){
	case stMeleeUsingStun:
		Vector4Copy(openColor,color);
		return "STUNNED";
	case stMeleeUsingBlock:
		Vector4Copy(guardColor,color);
		return "BLOCKING";
	case stMeleeUsingEvade:
		Vector4Copy(guardColor,color);
		return "EVADING";
	case stMeleeChargingPower:
	case stMeleeChargingStun:
		return "CHARGING";
	case stMeleeUsingSpeedBreaker:
	case stMeleeUsingChargeBreaker:
		return "BREAKER";
	case stMeleeUsingSpeed:
		return "ATTACKING";
	case stMeleeStartPower:
	case stMeleeUsingPower:
		return "HEAVY";
	case stMeleeStartHit:
		Vector4Copy(openColor,color);
		return "HIT";
	}
	if(status & LKSTATUS_FROZEN){
		Vector4Copy(openColor,color);
		return "RECOVERING";
	}
	return NULL;
}
/*================
CG_DrawMeleeReadout

The melee charge and the target's state, drawn on the target. Both are why the
melee plays shallower than it is: the charge decides whether a windup becomes
the attack or the breaker, and the target's state decides which of them wins,
and neither was on screen anywhere.

Deliberately not a fifth row of the status panel. The panel is a plate of fixed
art in the corner and these windows are sub-second and happen at the target, so
a corner reading would be technically present and practically still unreadable.

Aspect-correct, like the panel it borrows its bar from: mixing the two mappings
inside one HUD makes the pieces drift apart as the display aspect changes.

The one place the treatment is asked to whisper, and the one place it gives up
its plate: the sheared word and the charge gauge sit straight on the scene so
they never box off the fighter they are describing. That is the deck's own
deviation, agreed with the layout.
================*/
static void CG_DrawMeleeReadout(void){
	const playerState_t	*ps;
	const centity_t		*foe;
	const char			*label;
	vec3_t				anchor;
	vec4_t				labelColor;
	vec4_t				litColor = {0.310f,0.639f,0.890f,1.0f};
	vec4_t				tipColor = {0.749f,0.902f,1.0f,1.0f};
	vec4_t				emptyColor = {1.0f,1.0f,1.0f,0.22f};
	float				x,y,segment,left;
	int					charge,cap,lit,i;

	if(!cg_drawMeleeState.integer){return;}
	ps = &cg.predictedPlayerState;
	if(ps->lockedTarget <= 0 || ps->lockedTarget > MAX_CLIENTS){return;}
	foe = &cg_entities[ps->lockedTarget-1];
	if(!foe->currentValid){return;}
	VectorCopy(foe->lerpOrigin,anchor);
	anchor[2] += MELEE_READOUT_LIFT;
	if(!CG_WorldCoordToScreenCoordFloat(anchor,&x,&y)){return;}
	// The charge is the local fighter's own and is predicted, so the meter
	// tracks the button rather than the last snapshot.
	charge = ps->timers[tmMeleeCharge];
	cap = 0;
	if(ps->stats[stMeleeState] == stMeleeChargingPower){cap = MELEE_POWER_CHARGE;}
	else if(ps->stats[stMeleeState] == stMeleeChargingStun){cap = MELEE_STUN_CHARGE;}
	if(cap){
		if(charge > cap){charge = cap;}
		// Ten ticks, not one per unit of charge. A continuous fill at this size
		// is four pixels of travel a player cannot read inside a 550ms window;
		// ten is the most that still leaves a gap between the segments, which
		// is why the deck's readout gauge caps there.
		segment = (MELEE_READOUT_WIDTH - (MELEE_READOUT_SEGMENTS-1) * MELEE_READOUT_SEGGAP)
			/ (float)MELEE_READOUT_SEGMENTS;
		lit = (charge * MELEE_READOUT_SEGMENTS + cap - 1) / cap;
		left = x - MELEE_READOUT_WIDTH / 2.0f;
		for(i=0;i<MELEE_READOUT_SEGMENTS;i++){
			// The leading segment takes the bar's white tip colour, which is
			// what makes the gauge read as filling rather than as ten lamps.
			CG_DrawTrainingPic(left + i * (segment + MELEE_READOUT_SEGGAP),y,segment,
				MELEE_READOUT_HEIGHT,i >= lit ? emptyColor : (i == lit-1 ? tipColor : litColor),
				cgs.media.whiteShader);
			// Then the additive pass on top. Additive alone is the deck's note
			// and it is right on the deck's dark stage; over sunlit desert it
			// saturates to white and the blue stops meaning anything, so the
			// colour is laid down blended first and the light added over it.
			if(i < lit){
				CG_DrawTrainingPic(left + i * (segment + MELEE_READOUT_SEGGAP),y,segment,
					MELEE_READOUT_HEIGHT,i == lit-1 ? tipColor : litColor,
					cgs.media.trainingSegmentShader);
			}
		}
	}
	label = CG_MeleeReadoutState(ps->lockonData[lkMeleeState],ps->lockonData[lkMeleeStatus],labelColor);
	if(!label){return;}
	// The white cut rather than the gold one, and this is the deviation the
	// readout forces: threat, guard and opening are the whole point of the
	// word, and a baked gold gradient multiplied by red leaves brown. It keeps
	// the shear, which is what makes it the same voice as the tracker.
	CG_TextDraw(TEXTFACE_DISPLAYW,
		x,y-CG_TextHeight(TEXTFACE_DISPLAYW,MELEE_READOUT_WORD)-MELEE_READOUT_GAP,
		MELEE_READOUT_WORD,labelColor,label,0,TEXTF_CENTER|TEXTF_SHADOW);
}
/*================
CG_TrainingMasterName

Master ids are global - rules/masters.def states them rather than taking them
from file order, so the same id means the same master on every map - but only
the id travels in PERS_TRAINING_MASTER. Reading the vocabulary here is a lookup
table for that id, not a second source of truth: the game module still owns
placement, radius and everything a rule matches on.

Loaded on first use, so a server with training off never touches the file.
================*/
#define	TRAINING_MASTERS		16
#define	TRAINING_MASTERS_FILE	"rules/masters.def"
#define	TRAINING_MASTERS_SIZE	8000

static char		trainingMasterNames[TRAINING_MASTERS][32];
static qboolean	trainingMastersLoaded;
static char		trainingMasterBuffer[TRAINING_MASTERS_SIZE+1];
// The toast wrap works on a copy, because it has to cut the string to measure
// where the cut goes. Static rather than automatic: the QVM's stack is small.
static char		trainingToastLine[MAX_SAY_TEXT];

static const char *CG_TrainingMasterName(int id){
	fileHandle_t	f;
	char			*token,*parse;
	int				length,index;

	if(!trainingMastersLoaded){
		trainingMastersLoaded = qtrue;
		length = trap_FS_FOpenFile(TRAINING_MASTERS_FILE,&f,FS_READ);
		if(length > 0 && length <= TRAINING_MASTERS_SIZE){
			trap_FS_Read(trainingMasterBuffer,length,f);
			trainingMasterBuffer[length] = 0;
			parse = trainingMasterBuffer;
			while(1){
				token = COM_Parse(&parse);
				if(!token[0]){break;}
				if(Q_stricmp(token,"master")){continue;}
				token = COM_Parse(&parse);
				index = atoi(token);
				token = COM_Parse(&parse);
				if(index > 0 && index < TRAINING_MASTERS){
					Q_strncpyz(trainingMasterNames[index],token,sizeof(trainingMasterNames[index]));
				}
			}
		}
		if(length >= 0){trap_FS_FCloseFile(f);}
	}
	if(id <= 0 || id >= TRAINING_MASTERS){return "";}
	return trainingMasterNames[id];
}
/*================
CG_TrainingToast

Newest last, so the queue reads downward toward the tracker. Live toasts are
compacted before the insert: expiring in the push rather than in the draw keeps
a burst of lines from spending slots on messages that are already gone.
================*/
void CG_TrainingToast(const char *text,qboolean completion){
	int	i,live;

	live = 0;
	for(i=0;i<TRAINING_TOAST_SLOTS;i++){
		if(!cg.trainingToasts[i].time){continue;}
		if(cg.time - cg.trainingToasts[i].time >= TRAINING_TOAST_TIME){
			cg.trainingToasts[i].time = 0;
			continue;
		}
		if(live != i){memcpy(&cg.trainingToasts[live],&cg.trainingToasts[i],sizeof(trainingToast_t));}
		live++;
	}
	for(i=live;i<TRAINING_TOAST_SLOTS;i++){cg.trainingToasts[i].time = 0;}
	if(live == TRAINING_TOAST_SLOTS){
		for(i=0;i<TRAINING_TOAST_SLOTS-1;i++){
			memcpy(&cg.trainingToasts[i],&cg.trainingToasts[i+1],sizeof(trainingToast_t));
		}
		live = TRAINING_TOAST_SLOTS-1;
	}
	Q_strncpyz(cg.trainingToasts[live].text,text,sizeof(cg.trainingToasts[live].text));
	// A zero time is the free-slot marker, so the one millisecond a level can
	// start on has to be pushed off it.
	cg.trainingToasts[live].time = cg.time ? cg.time : 1;
	cg.trainingToasts[live].completion = completion;
}
/*================
CG_TrainingObjective

trobj carries what a snapshot cannot: the text, and the fact and goal it is
measured against. The percent it fills to arrives in persistant[] afterwards.
================*/
void CG_TrainingObjective(const char *text,int objectiveId,int trackFact,int goal){
	Q_strncpyz(cg.trainingObjective,text,sizeof(cg.trainingObjective));
	cg.trainingObjectiveId = objectiveId;
	cg.trainingTrackFact = trackFact;
	cg.trainingGoal = goal;
	cg.trainingProgress = 0;
	cg.trainingDoneTime = 0;
	CG_TrainingToast(va("objective: %s",text),qfalse);
}
/*================
CG_TrainingComplete

trdone and the snapshot that zeroes PERS_TRAINING_OBJECTIVE race, and either can
land first. Taking the text from the command rather than from what is already
stored makes both orders draw the same thing: the completed objective stays up
for its own moment whether or not the tracker was cleared a frame earlier.
================*/
void CG_TrainingComplete(const char *text,int objectiveId){
	Q_strncpyz(cg.trainingObjective,text,sizeof(cg.trainingObjective));
	cg.trainingObjectiveId = objectiveId;
	cg.trainingDoneTime = cg.time ? cg.time : 1;
	CG_TrainingToast(text,qtrue);
}
/*================
CG_DrawTrainingBar

The charge bar: a sheared trough, an additive fill and a white leading edge.

Additive because that is the language the auras and the ki gauges already speak
- light added to the scene rather than a swatch masking it - and the fill is a
sub-rectangle of its ramp rather than a squeezed copy, so the bright end of the
ramp stays at the bright end of the bar however short the fill is.
================*/
static void CG_DrawTrainingBar(float x,float y,float width,float height,float fraction,
	const vec4_t color){
	vec4_t	white = {1.0f,1.0f,1.0f,1.0f};
	float	tip;

	white[3] = color[3];
	CG_DrawTrainingPic(x,y,width,height,white,cgs.media.trainingBarTrackShader);
	if(fraction <= 0){return;}
	if(fraction > 1.0f){fraction = 1.0f;}
	CG_DrawTrainingPicST(x,y,width*fraction,height,fraction,color,cgs.media.trainingBarFillShader);
	// The tip stands proud of the trough top and bottom, which is what makes it
	// read as the edge of something moving rather than as the end of a fill.
	tip = height * 0.5f;
	CG_DrawTrainingPic(x+width*fraction-tip*0.5f,y-height*0.5f,tip,height*2.0f,white,
		cgs.media.trainingBarTipShader);
}
/*================
CG_DrawTrainingTracker

The plate, who is teaching, the objective in the display cut, and the bar it is
measured on - all sharing the deck's right edge.

The bar walks toward the percent instead of taking it. Progress is quantized to
whole percent precisely so it can be sent in persistant[] every snapshot, and a
20Hz integer percent stepped straight onto the screen visibly ratchets; the
walk is the other half of that decision, not decoration.
================*/
static void CG_DrawTrainingTracker(void){
	const char	*label,*name;
	vec4_t	plateColor = {1.0f,1.0f,1.0f,1.0f};
	vec4_t	masterColor = {0.310f,0.639f,0.890f,1.0f};
	vec4_t	labelColor = {1.0f,1.0f,1.0f,0.55f};
	vec4_t	barColor = {1.0f,1.0f,1.0f,1.0f};
	vec4_t	doneColor = {0.475f,0.839f,0.651f,1.0f};
	vec4_t	textColor = {1.0f,1.0f,1.0f,1.0f};
	qboolean	finishing;
	float		target,step,flash,size,room,width;
	int			active,face,master;

	active = cg.snap->ps.persistant[PERS_TRAINING_OBJECTIVE];
	finishing = cg.trainingDoneTime && cg.time - cg.trainingDoneTime < TRAINING_DONE_TIME ? qtrue : qfalse;
	if(!active && !finishing){
		// Nothing tracked: drop the assignment so the next objective cannot
		// inherit the last one's text between its trobj and its first snapshot.
		cg.trainingObjective[0] = 0;
		cg.trainingObjectiveId = 0;
		cg.trainingProgress = 0;
		cg.trainingDoneTime = 0;
		return;
	}
	target = finishing ? 100.0f : (float)cg.snap->ps.persistant[PERS_TRAINING_PROGRESS];
	if(target < 0){target = 0;}
	if(target > 100.0f){target = 100.0f;}
	step = cg.frametime * TRAINING_PROGRESS_RATE;
	if(cg.trainingProgress < target){
		cg.trainingProgress += step;
		if(cg.trainingProgress > target){cg.trainingProgress = target;}
	}
	else if(cg.trainingProgress > target){
		cg.trainingProgress -= step;
		if(cg.trainingProgress < target){cg.trainingProgress = target;}
	}
	// A tracker with no text is an objective assigned before this client was
	// listening - the bar is still true, so say that rather than nothing.
	label = cg.trainingObjective[0] && (finishing || cg.trainingObjectiveId == active) ? cg.trainingObjective : "training objective";
	CG_DrawTrainingPic(TRAINING_PLATE_X,TRAINING_PLATE_Y,TRAINING_PLATE_W,TRAINING_PLATE_H,
		plateColor,cgs.media.trainingPlateShader);
	// The master line lives inside the plate, because the rule engine sets
	// masterNear from a radius and this line appearing is the player's only
	// confirmation that they are standing close enough to be taught.
	master = cg.snap->ps.persistant[PERS_TRAINING_MASTER];
	if(master){
		name = CG_TrainingMasterName(master);
		CG_TextDraw(TEXTFACE_BODY,TRAINING_RIGHT,TRAINING_MASTER_Y,TRAINING_CAPS_SIZE,
			masterColor,CG_TextCaps(name[0] ? va("training with %s",name) : "master nearby"),
			TRAINING_CAPS_TRACK,TEXTF_RIGHT|TEXTF_SHADOW);
	}
	// The gold cut carries its gradient baked, so it is drawn at full white; a
	// completion swaps to the white cut and takes the jade, which is the only
	// way a baked gradient can change colour at all.
	face = finishing ? TEXTFACE_DISPLAYW : TEXTFACE_DISPLAY;
	if(finishing){Vector4Copy(doneColor,textColor);}
	// A long objective is set smaller rather than allowed off the plate. The
	// deck wraps its example to two lines; one line that shrinks keeps the bar
	// and the label on the rows they were placed on, which a second line would
	// push down into them.
	size = TRAINING_TEXT_SIZE;
	room = TRAINING_RIGHT - TRAINING_PLATE_X - 20;
	width = CG_TextWidth(face,label,size,0);
	if(width > room){
		size *= room / width;
		if(size < TRAINING_TEXT_MIN){size = TRAINING_TEXT_MIN;}
	}
	CG_TextDraw(face,TRAINING_RIGHT,TRAINING_TEXT_Y+(TRAINING_TEXT_SIZE-size)*0.5f,size,
		textColor,label,0,TEXTF_RIGHT|TEXTF_SHADOW);
	CG_TextDraw(TEXTFACE_BODY,TRAINING_RIGHT,TRAINING_LABEL_Y,TRAINING_CAPS_SIZE,labelColor,
		finishing ? "COMPLETE" : "OBJECTIVE",TRAINING_CAPS_TRACK,TEXTF_RIGHT|TEXTF_SHADOW);
	// The completion flash is the same gesture the limit break reserve makes
	// when it tops up: the bar itself says it filled.
	if(finishing){
		flash = 1.0f - (float)(cg.time - cg.trainingDoneTime) / TRAINING_DONE_TIME;
		if(flash > 0 && flash <= 1.0f){
			barColor[0] = 1.0f + flash;
			barColor[1] = 1.0f + flash;
			barColor[2] = 1.0f + flash;
		}
	}
	CG_DrawTrainingBar(TRAINING_BAR_X,TRAINING_BAR_Y,TRAINING_BAR_WIDTH,TRAINING_BAR_HEIGHT,
		cg.trainingProgress / 100.0f,barColor);
	// The number is the deck's own: display cut, hard right, level with the bar.
	label = finishing ? "DONE" : va("%i%%",(int)cg.trainingProgress);
	CG_TextDraw(face,TRAINING_RIGHT,
		TRAINING_BAR_Y+TRAINING_BAR_HEIGHT/2.0f-CG_TextHeight(face,TRAINING_PCT_SIZE)/2.0f,
		TRAINING_PCT_SIZE,textColor,label,0,TEXTF_RIGHT|TEXTF_SHADOW);
}
/*================
CG_TrainingToastBreak

Where a toast's one line has to become two, in characters. The sash is a fixed
piece of art, so the text has to fit it rather than the other way round; the
break goes at the last space that fits, and a run with no space in it is cut
where it lands because a word that long is a key, not a sentence.
================*/
static int CG_TrainingToastBreak(char *text,float width){
	int		i,last;

	if(CG_TextWidth(TEXTFACE_BODY,text,TRAINING_BODY_SIZE,0) <= width){return 0;}
	last = 0;
	for(i=0;text[i];i++){
		if(text[i] != ' '){continue;}
		text[i] = 0;
		if(CG_TextWidth(TEXTFACE_BODY,text,TRAINING_BODY_SIZE,0) <= width){last = i;}
		text[i] = ' ';
	}
	return last;
}
/*================
CG_DrawTrainingToasts

Oldest at the top, newest below it, each fading out on its own clock - the
queue's order and timings are untouched, only what it looks like changed.

The gold sash, top left, is where the approved layout puts these. A completion
takes the jade twin instead, so "you finished something" is a different object
from "someone said something" rather than the same object in another colour.
================*/
static void CG_DrawTrainingToasts(void){
	trainingToast_t	*toast;
	const char		*who,*name;
	float			*fade;
	vec4_t			whoColor = {0.165f,0.102f,0.024f,1.0f};
	vec4_t			doneWhoColor = {0.027f,0.153f,0.102f,1.0f};
	vec4_t			msgColor = {1.0f,1.0f,1.0f,1.0f};
	vec4_t			sashColor = {1.0f,1.0f,1.0f,1.0f};
	vec4_t			color;
	float			y,textWidth;
	int				i,live,row,brk;

	live = 0;
	for(i=0;i<TRAINING_TOAST_SLOTS;i++){
		if(cg.trainingToasts[i].time){live++;}
	}
	if(!live){return;}
	textWidth = TRAINING_TOAST_W - 30 - 24;
	row = 0;
	for(i=0;i<TRAINING_TOAST_SLOTS;i++){
		toast = &cg.trainingToasts[i];
		if(!toast->time){continue;}
		fade = CG_FadeColor(toast->time,TRAINING_TOAST_TIME,TRAINING_TOAST_FADE);
		if(!fade){
			toast->time = 0;
			row++;
			continue;
		}
		y = TRAINING_TOAST_TOP + row * TRAINING_TOAST_STEP;
		sashColor[3] = fade[3];
		CG_DrawTrainingPic(TRAINING_TOAST_X,y,TRAINING_TOAST_W,TRAINING_TOAST_H,sashColor,
			toast->completion ? cgs.media.trainingSashDoneShader : cgs.media.trainingSashShader);
		// Nothing on the wire says who spoke, so the sash names the master the
		// player is standing with. That is the same answer the tracker gives
		// and it is right whenever there is one to give.
		name = CG_TrainingMasterName(cg.snap->ps.persistant[PERS_TRAINING_MASTER]);
		who = toast->completion ? "COMPLETE" : (name[0] ? CG_TextCaps(name) : "TRAINING");
		Vector4Copy(toast->completion ? doneWhoColor : whoColor,color);
		color[3] = fade[3];
		CG_TextDraw(TEXTFACE_BODY,TRAINING_TOAST_X+14,y+5,TRAINING_CAPS_SIZE,color,who,
			TRAINING_CAPS_TRACK,0);
		Vector4Copy(msgColor,color);
		color[3] = fade[3];
		Q_strncpyz(trainingToastLine,toast->text,sizeof(trainingToastLine));
		brk = CG_TrainingToastBreak(trainingToastLine,textWidth);
		if(brk){
			trainingToastLine[brk] = 0;
			CG_TextDraw(TEXTFACE_BODY,TRAINING_TOAST_X+14,y+14,TRAINING_BODY_SIZE,color,
				trainingToastLine,0,TEXTF_SHADOW);
			CG_TextDraw(TEXTFACE_BODY,TRAINING_TOAST_X+14,y+14+TRAINING_BODY_SIZE,
				TRAINING_BODY_SIZE,color,trainingToastLine+brk+1,0,TEXTF_SHADOW);
		}
		else{
			CG_TextDraw(TEXTFACE_BODY,TRAINING_TOAST_X+14,y+18,TRAINING_BODY_SIZE,color,
				trainingToastLine,0,TEXTF_SHADOW);
		}
		row++;
	}
}
/*================
CG_DrawTraining

Deliberately outside the branch that draws the status panel. That branch hides
itself while soaring and while transforming, and the first lesson in the game is
measured on time spent airborne - a tracker that vanishes exactly while the
player is earning progress on it teaches nothing.
================*/
static void CG_DrawTraining(void){
	if(!cg_drawTraining.integer){return;}
	CG_DrawTrainingTracker();
	CG_DrawTrainingToasts();
}
static void CG_DrawStatusBar( void ) {
	centity_t		*cent;
	playerState_t	*ps;
	float			tierLast,tierNext,tier;
	int				base;
	clientInfo_t	*ci;
	cg_userWeapon_t	*weaponGraphics;
	tierConfig_cg	*activeTier;
	qboolean charging;
	ci = &cgs.clientinfo[cg.snap->ps.clientNum];
	ps = &cg.snap->ps;
	charging = ps->weaponstate == WEAPON_CHARGING || ps->weaponstate == WEAPON_ALTCHARGING ? qtrue : qfalse;
	if((ci->lockStartTimer > cg.time) /*(&& cg.time > ci->lockStartTimer - 500)*/){
		CG_DrawPic(qfalse,0,0,640,480,cgs.media.speedLineSpinShader);
	}
	if(ps->bitFlags & usingBoost){
		CG_DrawPic(qfalse,0,0,640,480,cgs.media.speedLineShader);
	}
	if(cg_drawStatus.integer == 0){return;}
	cent = &cg_entities[cg.snap->ps.clientNum];
	tier = (float)ps->powerLevel[plTierCurrent];
	CG_CheckChat();
	CG_DrawScreenEffects();
	if(ps->lockedTarget > 0 && cgs.clientinfo[ps->lockedTarget-1].infoValid){
		playerState_t lockedTargetPS;
		// Only the fields below arrive over the lockon data, and the HUD reads
		// bitFlags and options as well; leaving them as stack garbage decides
		// at random what the target's HUD draws.
		memset(&lockedTargetPS,0,sizeof(lockedTargetPS));
		lockedTargetPS.clientNum = ps->lockedTarget-1;
		lockedTargetPS.powerLevel[plCurrent] = ps->lockonData[lkPowerCurrent];
		lockedTargetPS.powerLevel[plHealth] = ps->lockonData[lkPowerHealth];
		lockedTargetPS.powerLevel[plMaximum] = ps->lockonData[lkPowerMaximum];
		lockedTargetPS.powerLevel[plFatigue] = lockedTargetPS.powerLevel[plMaximum];
		lockedTargetPS.powerLevel[plTierCurrent] = cgs.clientinfo[lockedTargetPS.clientNum].tierCurrent;
		CG_DrawHUD(ps,ps->clientNum,0,0,qfalse);
		CG_DrawHUD(&lockedTargetPS,lockedTargetPS.clientNum,320,0,qtrue);
	}
	else{
		CG_DrawHUD(ps,ps->clientNum,0,HUD_PANEL_Y,qfalse);
		if(charging){return;}
		if(tier){
			activeTier = &ci->tierConfig[ci->tierCurrent];
			tierLast = 32767;
			if(activeTier->sustainCurrent && activeTier->sustainCurrent < tierLast){tierLast = (float)activeTier->sustainCurrent;}
			if(activeTier->sustainFatigue && activeTier->sustainFatigue < tierLast){tierLast = (float)activeTier->sustainFatigue;}
			if(activeTier->sustainHealth && activeTier->sustainHealth < tierLast){tierLast = (float)activeTier->sustainHealth;}
			if(activeTier->sustainMaximum && activeTier->sustainMaximum < tierLast){tierLast = (float)activeTier->sustainMaximum;}
			if(tierLast < 32767){
				tierLast = tierLast / (float)ps->powerLevel[plMaximum];
				CG_DrawPic(qfalse,HUD_BAR_X+(HUD_BAR_WIDTH-HUD_PIN_WIDTH)*tierLast,HUD_PANEL_Y+HUD_ROW_PL_Y-HUD_GAUGE_INSET-10,HUD_PIN_WIDTH,HUD_PIN_HEIGHT,cgs.media.markerDescendShader);
			}
		}
		if(tier < ps->powerLevel[plTierTotal]){
			activeTier = &ci->tierConfig[ci->tierCurrent+1];
			tierNext = 0;
			if(activeTier->requirementCurrent && activeTier->requirementCurrent > tierNext){tierNext = (float)activeTier->requirementCurrent;}
			if(activeTier->requirementFatigue && activeTier->requirementFatigue > tierNext){tierNext = (float)activeTier->requirementFatigue;}
			if(activeTier->requirementMaximum && activeTier->requirementMaximum > tierNext){tierNext = (float)activeTier->requirementMaximum;}
			if(activeTier->requirementHealth && activeTier->requirementHealth > tierNext){tierNext = (float)activeTier->requirementHealth;}
			if(tierNext){
				tierNext = tierNext / (float)ps->powerLevel[plMaximum];
				if(tierNext < 1.0){
					CG_DrawPic(qfalse,HUD_BAR_X+(HUD_BAR_WIDTH-HUD_PIN_WIDTH)*tierNext,HUD_PANEL_Y+HUD_ROW_PL_Y-HUD_GAUGE_INSET-10,HUD_PIN_WIDTH,HUD_PIN_HEIGHT,cgs.media.markerAscendShader);
				}
			}
		}
	}
}
/*==================
CG_DrawSnapshot
==================*/
static float CG_DrawSnapshot( float y ) {
	char		*s;
	int			w;

	s = va( "time:%i snap:%i cmd:%i", cg.snap->serverTime, 
		cg.latestSnapshotNum, cgs.serverCommandSequence );
	w = CG_DrawStrlen( s ) * BIGCHAR_WIDTH;

	CG_DrawBigString( 635 - w, y + 2, s, 1.0F);

	return y + BIGCHAR_HEIGHT + 4;
}

/*
==================
CG_DrawFPS
==================
*/
#define	FPS_FRAMES	16
static float CG_DrawFPS( float y ) {
	char		*s;
	int			w;
	static int	previousTimes[FPS_FRAMES];
	static int	index;
	int			i, total;
	int			fps;
	static	int	previous, lastupdate;
	int			t, frameTime;
	const int	xOffset = 0;
	t = trap_Milliseconds();
	frameTime = t - previous;
	previous = t;
	if (t - lastupdate > 50){
		lastupdate = t;
		previousTimes[index % FPS_FRAMES] = frameTime;
		index++;
	}
	total = 0;
	for(i = 0 ; i < FPS_FRAMES ; i++){
		total += previousTimes[i];
	}
	if(!total){total = 1;}
	fps = 1000 * FPS_FRAMES / total;
	s = va( "%ifps", fps );
	w = CG_DrawStrlen( s ) * BIGCHAR_WIDTH;
	CG_DrawBigString( 635 - w + xOffset, y + 2, s, 1.0F);
	return y + BIGCHAR_HEIGHT + 4;
}

/*=================
CG_DrawTimer
=================*/
static float CG_DrawTimer( float y ) {
	char		*s;
	int			w;
	int			mins, seconds, tens;
	int			msec;

	msec = cg.time - cgs.levelStartTime;

	seconds = msec / 1000;
	mins = seconds / 60;
	seconds -= mins * 60;
	tens = seconds / 10;
	seconds -= tens * 10;

	s = va( "%i:%i%i", mins, tens, seconds );
	w = CG_DrawStrlen( s ) * BIGCHAR_WIDTH;

	CG_DrawBigString( 635 - w, y + 2, s, 1.0F);

	return y + BIGCHAR_HEIGHT + 4;
}

/*
=====================
CG_DrawUpperRight

=====================
*/
static void CG_DrawUpperRight( void ) {
	float y;
	y = 0;
	if(cg_drawSnapshot.integer){
		y = CG_DrawSnapshot( y );
	}
	if ( cg_drawFPS.integer ) {
		y = CG_DrawFPS( y );
	}
	if ( cg_drawTimer.integer ) {
		y = CG_DrawTimer( y );
	}
}

/*==============
CG_DrawDisconnect
Should we draw something differnet for long lag vs no packets?
==============*/
static void CG_DrawDisconnect( void ) {
	float		x, y;
	int			cmdNum;
	usercmd_t	cmd;
	const char		*s;
	int			w;

	// draw the phone jack if we are completely past our buffers
	cmdNum = trap_GetCurrentCmdNumber() - CMD_BACKUP + 1;
	trap_GetUserCmd( cmdNum, &cmd );
	if ( cmd.serverTime <= cg.snap->ps.commandTime
		|| cmd.serverTime > cg.time ) {	// special check for map_restart
		return;
	}

	// also add text in center of screen
	s = "Connection Interrupted";
	w = CG_DrawStrlen( s ) * BIGCHAR_WIDTH;
	CG_DrawBigString( 320 - w/2, 100, s, 1.0F);

	// blink the icon
	if ( ( cg.time >> 9 ) & 1 ) {
		return;
	}

	x = 640 - 48;
	y = 480 - 48;

	CG_DrawPic(qfalse, x, y, 48, 48, trap_R_RegisterShader("gfx/2d/net.tga" ) );
}
/*==============
CG_CenterPrint
Called for important messages that should stay in the center of the screen
for a few moments
==============
*/
void CG_CenterPrint( const char *str, int y, int charWidth ) {
	char	*s;

	Q_strncpyz( cg.centerPrint, str, sizeof(cg.centerPrint) );

	cg.centerPrintTime = cg.time;
	cg.centerPrintY = y;
	cg.centerPrintCharWidth = charWidth;

	// count the number of lines for centering
	cg.centerPrintLines = 1;
	s = cg.centerPrint;
	while( *s ) {
		if (*s == '\n')
			cg.centerPrintLines++;
		s++;
	}
}
/*===================
CG_DrawCenterString
===================*/
static void CG_DrawCenterString( void ) {
	char	*start;
	int		l;
	int		x, y, w;
	float	*color;

	if ( !cg.centerPrintTime ) {
		return;
	}

	color = CG_FadeColor( cg.centerPrintTime, 1000 * cg_centertime.value, 200 );
	if ( !color ) {
		return;
	}

	trap_R_SetColor( color );

	start = cg.centerPrint;

	y = cg.centerPrintY - cg.centerPrintLines * BIGCHAR_HEIGHT / 2;

	while ( 1 ) {
		char linebuffer[1024];

		for ( l = 0; l < 50; l++ ) {
			if ( !start[l] || start[l] == '\n' ) {
				break;
			}
			linebuffer[l] = start[l];
		}
		linebuffer[l] = 0;
		w = cg.centerPrintCharWidth * CG_DrawStrlen( linebuffer );
		x = ( SCREEN_WIDTH - w ) / 2;
		CG_DrawStringExt(-1, x, y, linebuffer, color, qfalse, qtrue,
			cg.centerPrintCharWidth, (int)(cg.centerPrintCharWidth * 1.5), 0 );
		y += cg.centerPrintCharWidth * 1.5;
		while ( *start && ( *start != '\n' ) ) {
			start++;
		}
		if ( !*start ) {
			break;
		}
		start++;
	}

	trap_R_SetColor( NULL );
}
/*=================
CG_DrawCrosshair
=================*/
static void CG_DrawCrosshair(void) {
	float			w, h;
	qhandle_t		hShader;
	float			f;
	float			x, y;
	int				ca;
	int				i;
	trace_t			trace;
	playerState_t	*ps;
	clientInfo_t	*ci;
	tierConfig_cg	*tier;
	vec3_t			muzzle, forward, up;
	vec3_t			start, end;
	vec4_t			lockOnEnemyColor	= {1.0f,0.0f,0.0f,1.0f};
	vec4_t			lockOnAllyColor		= {0.0f,1.0f,0.0f,1.0f};
	vec4_t			chargeColor			= {0.5f,0.5f,1.0f,1.0f};
	radar_t			cg_playerOrigins[MAX_CLIENTS];
	if(!cg_drawCrosshair.integer || cg.snap->ps.lockedTarget > 0){return;}
	if(cg.snap->ps.persistant[PERS_TEAM] == TEAM_SPECTATOR){return;}
	ci = &cgs.clientinfo[cg.snap->ps.clientNum];
	tier = &ci->tierConfig[ci->tierCurrent];
	ps = &cg.predictedPlayerState;
	if(ps->bitFlags & usingMelee){return;}
	AngleVectors( ps->viewangles, forward, NULL, up );
	VectorCopy( ps->origin, muzzle );
	VectorMA( muzzle, ps->viewheight, up, muzzle );
	VectorMA( muzzle, 14, forward, muzzle );
	VectorCopy( muzzle, start );
	VectorMA(start, 131072, forward, end);
	CG_Trace(&trace, start, NULL, NULL, end, cg.snap->ps.clientNum, CONTENTS_SOLID|CONTENTS_BODY);	
	if(!CG_WorldCoordToScreenCoordFloat( trace.endpos, &x, &y)){return;}
	w = h = (cg_crosshairSize.value * 8 + 8);
	f = cg.time - cg.itemPickupBlendTime;
	if(f > 0 && f < ITEM_BLOB_TIME){
		f /= ITEM_BLOB_TIME;
		w *= ( 1 + f );
		h *= ( 1 + f );
	}
	if(cg_crosshairHealth.integer){
		vec4_t		hcolor;
		CG_ColorForHealth(hcolor);
		trap_R_SetColor(hcolor);
	}
	else{
		trap_R_SetColor( NULL );
	}
	ca = cg_drawCrosshair.integer;
	if (ca < 0) {
		ca = 0;
	}
	hShader = cgs.media.crosshairShader[ca % NUM_CROSSHAIRS];
	if(tier->crosshair){
		hShader = tier->crosshair;
		if(ps->bitFlags & isBreakingLimit && tier->crosshairPowering){
			hShader = tier->crosshairPowering;
		}
	}
	if ( cg.snap->ps.currentSkill[WPSTAT_BITFLAGS] & WPF_READY || cg.snap->ps.currentSkill[WPSTAT_ALT_BITFLAGS] & WPF_READY) {
		trap_R_SetColor( chargeColor );
	}
	else if (cg.crosshairClientNum > 0 && cg.crosshairClientNum <= MAX_CLIENTS || ps->lockedTarget > 0) {
		if( cgs.clientinfo[cg.crosshairClientNum].team == cg.snap->ps.persistant[PERS_TEAM] && cgs.clientinfo[cg.crosshairClientNum].team != TEAM_FREE  ) {
			trap_R_SetColor( lockOnAllyColor );
		}
		else{
			trap_R_SetColor( lockOnEnemyColor );
		}
	}
	else{
		trap_R_SetColor( NULL );
	}
	CG_DrawPic(qfalse, x - 0.5f * w, y - 0.5f * h, w, h, hShader );
	trap_R_SetColor( NULL );
}
/*=================
CG_ScanForCrosshairEntity
=================*/
static void CG_ScanForCrosshairEntity(void){
	trace_t			trace,trace2;
	vec3_t			start,end,muzzle,forward,up,minSize,maxSize;
	playerState_t	*ps;
	ps = &cg.predictedPlayerState;
	AngleVectors(ps->viewangles,forward,NULL,up);
	VectorCopy(ps->origin, muzzle );
	VectorMA(muzzle,ps->viewheight,up,muzzle);
	VectorMA(muzzle,14,forward,muzzle);
	VectorCopy(muzzle,start);
	VectorMA(start,131072,forward,end);
	minSize[0] = -(float)cg_lockonDistance.value;
	minSize[1] = -(float)cg_lockonDistance.value;
	minSize[2] = -(float)cg_lockonDistance.value;
	maxSize[0] = -minSize[0];
	maxSize[1] = -minSize[1];
	maxSize[2] = -minSize[2];
	CG_Trace(&trace,start,minSize,maxSize,end,cg.snap->ps.clientNum,CONTENTS_BODY);
	if (trace.entityNum>=MAX_CLIENTS){cg.crosshairClientNum= -1;return;}
	cg.crosshairClientNum=trace.entityNum;
	cg.crosshairClientTime=cg.time;
}
/*=====================
CG_DrawCrosshairNames
=====================*/
static void CG_DrawCrosshairNames( void ) {
	float		*color;
	char		*name;
	float		w;
	if ( !cg_drawCrosshair.integer ) {
		return;
	}
	CG_ScanForCrosshairEntity();

	if ( !cg_drawCrosshairNames.integer ) {
		return;
	}
	color = CG_FadeColor( cg.crosshairClientTime, 1000, 200 );
	if ( !color ) {
		trap_R_SetColor( NULL );
		return;
	}
	name = cgs.clientinfo[ cg.crosshairClientNum ].name;
	w = CG_DrawStrlen( name ) * BIGCHAR_WIDTH;
	CG_DrawBigString( 320 - w / 2, 170, name, color[3] * 0.5f );
	trap_R_SetColor( NULL );
}
/*=================
CG_DrawSpectator
=================*/
static void CG_DrawSpectator(void) {
	CG_DrawBigString(320 - 9 * 8, 440, "SPECTATOR", 1.0F);
	if ( cgs.gametype == GT_TOURNAMENT ) {
		CG_DrawBigString(320 - 15 * 8, 460, "waiting to play", 1.0F);
	}
	else if ( cgs.gametype >= GT_TEAM ) {
		CG_DrawBigString(320 - 39 * 8, 460, "press ESC and use the JOIN menu to play", 1.0F);
	}
}

/*
=================
CG_DrawVote
=================
*/
static void CG_DrawVote(void) {
	char	*s;
	int		sec;

	if ( !cgs.voteTime ) {
		return;
	}

	// play a talk beep whenever it is modified
	if ( cgs.voteModified ) {
		cgs.voteModified = qfalse;
		trap_S_StartLocalSound( cgs.media.talkSound, CHAN_LOCAL_SOUND );
	}

	sec = ( VOTE_TIME - ( cg.time - cgs.voteTime ) ) / 1000;
	if ( sec < 0 ) {
		sec = 0;
	}
	s = va("VOTE(%i):%s yes:%i no:%i", sec, cgs.voteString, cgs.voteYes, cgs.voteNo );
	CG_DrawSmallString( 0, 58, s, 1.0F );
}

/*=================
CG_DrawTeamVote
=================*/
static void CG_DrawTeamVote(void) {
	char	*s;
	int		sec, cs_offset;

	if ( cgs.clientinfo[cg.clientNum].team == TEAM_RED )
		cs_offset = 0;
	else if ( cgs.clientinfo[cg.clientNum].team == TEAM_BLUE )
		cs_offset = 1;
	else
		return;

	if ( !cgs.teamVoteTime[cs_offset] ) {
		return;
	}

	// play a talk beep whenever it is modified
	if ( cgs.teamVoteModified[cs_offset] ) {
		cgs.teamVoteModified[cs_offset] = qfalse;
		trap_S_StartLocalSound( cgs.media.talkSound, CHAN_LOCAL_SOUND );
	}

	sec = ( VOTE_TIME - ( cg.time - cgs.teamVoteTime[cs_offset] ) ) / 1000;
	if ( sec < 0 ) {
		sec = 0;
	}
	s = va("TEAMVOTE(%i):%s yes:%i no:%i", sec, cgs.teamVoteString[cs_offset],
							cgs.teamVoteYes[cs_offset], cgs.teamVoteNo[cs_offset] );
	CG_DrawSmallString( 0, 90, s, 1.0F );
}
/*=================
CG_DrawWarmup
=================*/
static void CG_DrawWarmup( void ) {
	int			w;
	int			sec;
	int			i;
	float scale;
	clientInfo_t	*ci1, *ci2;
	int			cw;
	const char	*s;

	sec = cg.warmup;
	if ( !sec ) {
		return;
	}

	if ( sec < 0 ) {
		s = "Waiting for players";		
		w = CG_DrawStrlen( s ) * BIGCHAR_WIDTH;
		CG_DrawBigString(320 - w / 2, 24, s, 1.0F);
		cg.warmupCount = 0;
		return;
	}

	if (cgs.gametype == GT_TOURNAMENT) {
		// find the two active players
		ci1 = NULL;
		ci2 = NULL;
		for ( i = 0 ; i < cgs.maxclients ; i++ ) {
			if ( cgs.clientinfo[i].infoValid && cgs.clientinfo[i].team == TEAM_FREE ) {
				if ( !ci1 ) {
					ci1 = &cgs.clientinfo[i];
				} else {
					ci2 = &cgs.clientinfo[i];
				}
			}
		}

		if ( ci1 && ci2 ) {
			s = va( "%s vs %s", ci1->name, ci2->name );
			w = CG_DrawStrlen( s );
			if ( w > 640 / GIANT_WIDTH ) {
				cw = 640 / w;
			} else {
				cw = GIANT_WIDTH;
			}
			CG_DrawStringExt(-1, 320 - w * cw/2, 20,s, colorWhite, 
					qfalse, qtrue, cw, (int)(cw * 1.5f), 0 );
		}
	} else {
		if ( cgs.gametype == GT_FFA ) {
			s = "Free For All";
		} else if ( cgs.gametype == GT_STRUGGLE ) {
			s = "Struggle";
		} else if ( cgs.gametype == GT_TEAM ) {
			s = "Team Deathmatch";
		} else if ( cgs.gametype == GT_CTF ) {
			s = "Capture the Flag";
		} else {
			s = "";
		}
		w = CG_DrawStrlen( s );
		if ( w > 640 / GIANT_WIDTH ) {
			cw = 640 / w;
		} else {
			cw = GIANT_WIDTH;
		}
		CG_DrawStringExt(-1, 320 - w * cw/2, 25,s, colorWhite, 
				qfalse, qtrue, cw, (int)(cw * 1.1f), 0 );
	}

	sec = ( sec - cg.time ) / 1000;
	if ( sec < 0 ) {
		cg.warmup = 0;
		sec = 0;
	}
	s = va( "Starts in: %i", sec + 1 );
	if ( sec != cg.warmupCount ) {
		cg.warmupCount = sec;
	}
	scale = 0.45f;
	switch ( cg.warmupCount ) {
	case 0:
		cw = 28;
		scale = 0.54f;
		break;
	case 1:
		cw = 24;
		scale = 0.51f;
		break;
	case 2:
		cw = 20;
		scale = 0.48f;
		break;
	default:
		cw = 16;
		scale = 0.45f;
		break;
	}
	w = CG_DrawStrlen( s );
	CG_DrawStringExt(-1, 320 - w * cw/2, 70, s, colorWhite, 
			qfalse, qtrue, cw, (int)(cw * 1.5), 0 );
}
/*=================
CG_Draw2D
=================*/
void CG_DrawScripted2D(void){
	int index;
	overlay2D *current;
	for(index=0;index<16;++index){
		current = &cg.scripted2D[index];
		if(current->active){
			if((cg.time <= current->endTime) || (current->endTime == -1)){
				CG_DrawPic(qfalse,current->x,current->y,current->width,current->height,current->shader);
				continue;
			}
			current->active = qfalse;
		}
	}
}

/*
=====================
CG_DrawFightDebug

The fight line answers "what did this cost" after the fact; this answers "what
is it looking at" while it happens. Everything here gates a melee or a charge,
and none of it is otherwise on screen - the power bar shows one of five values
the HUD tracks and the decisions are all about the other four.

Drawn from cg.snap->ps, so following a fighter as a spectator shows that
fighter's state. That is what the duel harness leaves you in.
=====================
*/
static void CG_DrawFightDebug( void ) {
	const playerState_t	*ps;
	char				line[128];
	int					y;
	int					ready;
	int					dist;

	if ( !cg_debugFight.integer ) {
		return;
	}

	ps = &cg.snap->ps;
	y = 140;
	ready = ( ps->currentSkill[WPSTAT_BITFLAGS] & WPF_READY ) ? 1 : 0;

	// -1 rather than 0 for "no lock": zero is a real distance and this is the
	// number every melee gate is really asking about.
	dist = -1;
	if ( ps->lockedTarget > 0 && ps->lockedTarget <= MAX_CLIENTS ) {
		const centity_t *foe = &cg_entities[ps->lockedTarget - 1];
		if ( foe->currentValid ) {
			dist = (int)Distance( ps->origin, foe->lerpOrigin );
		}
	}

	Com_sprintf( line, sizeof( line ), "hp %i/%i  guard %i  pool %i/%i",
		ps->powerLevel[plHealth], ps->powerLevel[plMaximum], ps->powerLevel[plFatigue],
		ps->powerLevel[plHealthPool], ps->powerLevel[plMaximumPool] );
	CG_DrawSmallString( 8, y, line, 1.0f );
	y += SMALLCHAR_HEIGHT;

	// The charge funnel, on screen: a windup below its ready threshold is
	// thrown away by any interrupt, so "charging 40%" and "charging, ready"
	// are entirely different situations and the bar does not distinguish them.
	Com_sprintf( line, sizeof( line ), "wpn %i %s  charge %i%%%s",
		ps->weapon, BG_WeaponStateName( ps->weaponstate ),
		ps->stats[stChargePercentPrimary], ready ? " READY" : "" );
	CG_DrawSmallString( 8, y, line, 1.0f );
	y += SMALLCHAR_HEIGHT;

	Com_sprintf( line, sizeof( line ), "melee %s %s  lock %i dist %i",
		( ps->bitFlags & usingMelee ) ? "yes" : "no",
		BG_MeleeStateName( ps->stats[stMeleeState] ),
		ps->lockedTarget, dist );
	CG_DrawSmallString( 8, y, line, 1.0f );
	y += SMALLCHAR_HEIGHT;

	// Everything that makes a melee branch or a recovery refuse itself. A
	// fighter standing in range doing nothing is always one of these.
	Com_sprintf( line, sizeof( line ), "freeze %i  safe %i  mIdle %i  %s%s%s",
		ps->timers[tmFreeze], ps->timers[tmSafe], ps->timers[tmMeleeIdle],
		( ps->bitFlags & usingBlock ) ? "block " : "",
		( ps->bitFlags & usingAlter ) ? "alter " : "",
		( ps->bitFlags & usingZanzoken ) ? "zanzoken " : "" );
	CG_DrawSmallString( 8, y, line, 1.0f );
}

static void CG_Draw2D( void ) {
	// if we are taking a levelshot for the menu, don't draw anything
	if ( cg.levelShot ) {
		return;
	}
	if ( cg_draw2D.integer == 0 ) {
		return;
	}
	if ( cg.snap->ps.pm_type == PM_INTERMISSION ) {
		CG_DrawScoreboard();
		return;
	}
	if(cg_scripted2D.integer != 0){
		CG_DrawScripted2D();
	}
	if ( cg.snap->ps.persistant[PERS_TEAM] == TEAM_SPECTATOR ) {
		CG_DrawSpectator();
		CG_DrawCrosshair();
		CG_DrawCrosshairNames();
		CG_DrawRadar();
	}
	else if(!(cg.snap->ps.bitFlags & usingSoar)){
		if (!(cg.snap->ps.timers[tmTransform] > 1) && !(cg.snap->ps.powerups[PW_STATE] < 0)){
			playerState_t	*ps;
			clientInfo_t *ci;
			ci = &cgs.clientinfo[cg.snap->ps.clientNum];
			ps = &cg.snap->ps;
			CG_DrawStatusBar();
			CG_DrawCrosshair();
			CG_DrawCrosshairNames();
			CG_DrawMeleeReadout();
			CG_DrawRadar();
			if(!(cg.snap->ps.bitFlags & usingMelee)){
				CG_DrawWeaponSelect();
			}
		}
	}
	CG_DrawTraining();
	// Over the HUD and under nothing: the journal is a full-screen page, and
	// while it is up the tracker behind it is what its live line is quoting.
	CG_DrawJournal();
	CG_DrawVote();
	CG_DrawTeamVote();
	CG_DrawUpperRight();
	CG_DrawFightDebug();
	if ( cg.showScores ) {
		CG_DrawScoreboard();
	}
}
void CG_DrawScreenFlash ( void ) {
	float		*color;
	vec4_t		white = {1.0f,1.0f,1.0f,0.5f};
	vec4_t		black = {0.0f,0.0f,0.0f,0.5f};
	color = CG_FadeColor( cg.screenFlashTime, cg.screenFlastTimeTotal, cg.screenFlashFadeTime );
	if ( !color ) {
		return;
	}
	if ( cg.snap->ps.timers[tmBlind] > 0 ){
		trap_R_SetColor( color );
		white[3] = color[3] * cg.screenFlashFadeAmount * 0.5f;
		CG_DrawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH*SCREEN_HEIGHT, white);
		trap_R_SetColor( NULL );
	}
}

/*=====================
CG_DrawActive
Perform all drawing needed to completely fill the screen
=====================*/
void CG_DrawActive( stereoFrame_t stereoView ) {
	vec4_t		water = {0.25f,0.5f,1.0f,0.1f};
	int			contents;
	if(!cg.snap){
		CG_DrawInformation();
		return;
	}
	CG_TileClear();
	CG_MotionBlur();
	trap_R_RenderScene(&cg.refdef);
	contents = CG_PointContents(cg.refdef.vieworg,-1);
	if(contents & CONTENTS_WATER){
		float phase = cg.time / 1000.0 * 0.2f * M_PI * 2;
		water[3] = 0.1f + (0.02f*sin( phase ));
		CG_DrawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH*SCREEN_HEIGHT, water);
		//trap_R_AddFogToScene(0,5000,0,0,0,1,2,2);
	}
	else{
		//trap_R_AddFogToScene(0,0, 0,0,0,0,2,2);
	}
	CG_DrawScreenFlash();
 	CG_Draw2D();
	CG_LoadDeferredPlayers();
}
