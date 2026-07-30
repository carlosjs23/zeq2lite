#include "cg_local.h"
void CG_CheckMusic(void){
	playerState_t *ps;
	clientInfo_t *ci;
	tierConfig_cg *tier;
	ps = &cg.predictedPlayerState;
	ci = &cgs.clientinfo[ps->clientNum];
	tier = &ci->tierConfig[ci->tierCurrent];

	if(!cgs.music.started){
		CG_StartMusic();
	}
	if(ps->bitFlags & isTransforming){
		if(cgs.music.currentType != 9){
			char var[8];
			cgs.music.currentType = 9;
			if(tier->transformMusic[0]){
				CG_PlayTransformTrack();
			}
			else{
				CG_NextTrack();
			}
			trap_Cvar_VariableStringBuffer("cg_playTransformTrackToEnd", var, sizeof(var));
			if(atoi(var)){
				cgs.music.playToEnd = qtrue;
			}
		}
	}
	else if(ps->bitFlags & isStruggling){
		if(cgs.music.currentType != 4){
			cgs.music.currentType = 4;
			CG_NextTrack();
		}
	}
	else if(!(ps->bitFlags & isUnsafe)){
		if(ps->bitFlags & isTargeted || ps->lockedTarget > 0){
			if(ps->powerLevel[plHealth] < (ps->powerLevel[plMaximum] * 0.4)){
				if(cgs.music.currentType != 6){
					cgs.music.currentType = 6;
					CG_NextTrack();
				}
			}
			else if(cgs.music.currentType != 5){
				cgs.music.currentType = 5;
				CG_NextTrack();
			}
		}
		else{
			if(ps->powerLevel[plHealth] < (ps->powerLevel[plMaximum] * 0.4)){
				if(cgs.music.currentType != 3){
					cgs.music.currentType = 3;
					CG_FadeNext();
				}
			}
			else if(ps->bitFlags & underWater){
				if(cgs.music.currentType != 2){
					cgs.music.currentType = 2;
					CG_NextTrack();
				}
			}
			else if(cgs.music.currentType != 1){
				cgs.music.currentType = 1;
				CG_FadeNext();
			}
		}
	}
	else if(cgs.music.currentType != 0){
		cgs.music.currentType = 0;
		CG_NextTrack();		
	}
	if(cg.time > cgs.music.endTime){	
		int difference = (cg.time - cgs.music.endTime);
		float percent = 0.0;
		if(difference < cgs.music.fadeAmount){
			percent = 1.0 - ((float)difference / (float)cgs.music.fadeAmount);
			trap_Cvar_Set("s_musicvolume",va("%f",percent * cg_music.value));
		}
		else{
			cgs.music.fading = qfalse;
			cgs.music.playToEnd = qfalse;
			CG_NextTrack();
		}
	}
}
/*
Parses a colon separated duration into milliseconds.
Accepts "SS", "M:SS" and "H:MM:SS": every ':' shifts the value accumulated
so far up one sexagesimal place. Non-digits are ignored, so a stray carriage
return or trailing character cannot derail the scan. Scanning the string
directly means there is no fixed size buffer left to overflow.
*/
int CG_GetMilliseconds(const char *time){
	int amount,value;
	if(!time){return 0;}
	amount = 0;
	value = 0;
	while(*time){
		if(*time == ':'){
			amount = (amount + value) * 60;
			value = 0;
		}
		else if(*time >= '0' && *time <= '9'){
			value = (value * 10) + (*time - '0');
		}
		++time;
	}
	return (amount + value) * 1000;
}
void CG_ParsePlaylist(void){
	static char trackNames[MUSICTYPES][MUSICTRACKS][256];
	fileHandle_t playlist;
	char *token,*parse;
	int trackIndex,typeIndex;
	int fileLength;
	char fileContents[32000];
	trackIndex = 0;
	typeIndex = -1;
	cgs.music.currentIndex = -1;
	cgs.music.fadeAmount = 0;
	cgs.music.started = qtrue;
	cgs.music.random = qfalse;
	memset(cgs.music.playlist,0,sizeof(cgs.music.playlist));
	memset(cgs.music.trackLength,0,sizeof(cgs.music.trackLength));
	memset(cgs.music.typeSize,0,sizeof(cgs.music.typeSize));
	for(trackIndex = 0;trackIndex < MUSICTYPES;++trackIndex){
		cgs.music.lastTrack[trackIndex] = -1;
	}
	trackIndex = 0;
	fileLength = trap_FS_FOpenFile("music/playlist.cfg",&playlist,FS_READ);
	if(fileLength <= 0){
		if(playlist){trap_FS_FCloseFile(playlist);}
		return;
	}
	if(fileLength > (int)sizeof(fileContents)-1){
		CG_Printf("^3WARNING: music/playlist.cfg is %i bytes, truncating to %i\n",fileLength,(int)sizeof(fileContents)-1);
		fileLength = (int)sizeof(fileContents)-1;
	}
	trap_FS_Read(fileContents,fileLength,playlist);
	fileContents[fileLength] = 0;
	trap_FS_FCloseFile(playlist);
	parse = fileContents;
	while(1){
		token = COM_Parse(&parse);
		if(!token[0]){break;}
		if(!Q_stricmp(token,"type")){
			token = COM_Parse(&parse);
			if(!token[0]){break;}
			if(!Q_stricmp(token,"random")){
				cgs.music.random = qtrue;
			}
		}
		else if(!Q_stricmp(token,"fade")){
			token = COM_Parse(&parse);
			if(!token[0]){break;}
			cgs.music.fadeAmount = CG_GetMilliseconds(token);
		}
		else if(token[strlen(token)-1] == '{'){
			// "<typeName>{" opens a block; types are consumed in the order
			// they appear, matching the trackTypes enum.
			++typeIndex;
			trackIndex = 0;
			if(typeIndex >= MUSICTYPES){
				CG_Printf("^3WARNING: music/playlist.cfg declares more than %i track types, ignoring the rest\n",MUSICTYPES);
				break;
			}
		}
		else if(token[0] == '}'){
			if(typeIndex >= 0){
				cgs.music.typeSize[typeIndex] = trackIndex;
			}
			trackIndex = 0;
		}
		else{
			// "<trackName> <duration>"
			if(typeIndex < 0){
				CG_Printf("^3WARNING: music/playlist.cfg track '%s' is outside a type block, ignoring\n",token);
				COM_Parse(&parse);
				continue;
			}
			if(trackIndex >= MUSICTRACKS){
				CG_Printf("^3WARNING: music/playlist.cfg type %i has more than %i tracks, ignoring the rest\n",typeIndex,MUSICTRACKS);
				COM_Parse(&parse);
				continue;
			}
			Q_strncpyz(trackNames[typeIndex][trackIndex],token,sizeof(trackNames[typeIndex][trackIndex]));
			cgs.music.playlist[typeIndex][trackIndex] = trackNames[typeIndex][trackIndex];
			cgs.music.trackLength[typeIndex][trackIndex] = CG_GetMilliseconds(COM_Parse(&parse));
			++trackIndex;
		}
	}
}
void CG_StartMusic(void){
	CG_ParsePlaylist();
}
void CG_FadeNext(void){
	if(!cgs.music.playToEnd){
		cgs.music.fading = qtrue;
		cgs.music.endTime = cg.time;
	}
}
void CG_NextTrack(void){
	int nextIndex;
	int typeSize;
	char *path;
	int duration;
	if(cgs.music.fading || cgs.music.playToEnd){return;}
	if(cgs.music.currentType < 0 || cgs.music.currentType >= MUSICTYPES){return;}
	typeSize = cgs.music.typeSize[cgs.music.currentType];
	if(typeSize <= 0){return;}
	nextIndex = (cgs.music.currentIndex + 1 < typeSize) ? cgs.music.currentIndex + 1 : 0;
	if(cgs.music.random){
		nextIndex = fabs(crandom()) * typeSize;
	}
	if(nextIndex == cgs.music.lastTrack[cgs.music.currentType]){nextIndex += 1;}
	if(nextIndex < 0){nextIndex = typeSize-1;}
	if(nextIndex >= typeSize){nextIndex = 0;}
	if(!cgs.music.playlist[cgs.music.currentType][nextIndex]){return;}
	cgs.music.currentIndex = nextIndex;
	path = va("music/%s",cgs.music.playlist[cgs.music.currentType][nextIndex]);
	duration = cgs.music.trackLength[cgs.music.currentType][nextIndex];
	//CG_Printf("Playing type %i | track %i.  Next track in %i seconds.\n",cgs.music.currentType,nextIndex,duration/1000);
	if(duration > 300000){duration = 300000;}
	cgs.music.endTime = cg.time + duration - cgs.music.fadeAmount;
	cgs.music.lastTrack[cgs.music.currentType] = nextIndex;
	trap_S_StartBackgroundTrack(path,path);
	trap_Cvar_Set("s_musicvolume",va("%f",cg_music.value));
}
void CG_PlayTransformTrack(void){
	playerState_t	*ps;
	clientInfo_t	*ci;
	tierConfig_cg	*tier;
	char	*path;
	int		duration;

	ps = &cg.predictedPlayerState;
	ci = &cgs.clientinfo[ps->clientNum];
	tier = &ci->tierConfig[ci->tierCurrent];
	path = va("music/%s",tier->transformMusic);
	duration = tier->transformMusicLength;
	if(duration > 300000){duration = 300000;}
	cgs.music.endTime = cg.time + duration - cgs.music.fadeAmount;
	trap_S_StartBackgroundTrack(path,path);
	trap_Cvar_Set("s_musicvolume",va("%f",cg_music.value));
}
