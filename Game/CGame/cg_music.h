#define MUSICTYPES 10
#define MUSICTRACKS 32
typedef struct{
	char *playlist[MUSICTYPES][MUSICTRACKS];
	int trackLength[MUSICTYPES][MUSICTRACKS];
	int hasPlayed[MUSICTYPES][MUSICTRACKS];
	qboolean random;
	qboolean started;
	qboolean fading;
	qboolean playToEnd;
	int typeSize[MUSICTYPES];
	int lastTrack[MUSICTYPES];
	int currentType;
	int currentIndex;
	int fadeAmount;
	int endTime;
	int volume;
}musicSystem;
typedef enum {
	battle,
	idle,
	idleWater,
	idleDanger,
	struggle,
	standoff,
	standoffDanger,
	victoryGood,
	victoryEvil,
	transform
}trackTypes;
void CG_CheckMusic(void);
int CG_GetMilliseconds(const char*);
void CG_ParsePlaylist(void);
void CG_StartMusic(void);
void CG_FadeNext(void);
void CG_NextTrack(void);
void CG_PlayTransformTrack(void);
