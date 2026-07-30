/*
Link seams for the sound units.

snd_dma.c owns the spatialisation maths we want to test, but as a translation
unit it also reaches the audio backend, the codec layer, the memory pool and a
handful of cvars. None of that participates in S_SpatializeOrigin, so it is all
inert here. The globals snd_dma.c defines itself (listener_origin, listener_axis,
dma) are NOT stubbed - tests drive the real ones.
*/

/* Same include set as snd_dma.c, so cls and the sound types have the layout the
   unit under test was compiled against. */
#include "snd_local.h"
#include "snd_codec.h"
#include "client.h"

#include <stdio.h>

/* --- cvars the sound system reads at init ---------------------------------- */
cvar_t *s_volume;
cvar_t *s_musicVolume;
cvar_t *s_doppler;
cvar_t *s_muted;
cvar_t *cl_aviFrameRate;

/* --- client state --------------------------------------------------------- */
/* snd_dma.c only reads cls.realtime; a bare definition is enough. */
clientStatic_t cls;

/* --- audio backend: never started in a unit test -------------------------- */
qboolean SNDDMA_Init(void) { return qfalse; }
int  SNDDMA_GetDMAPos(void) { return 0; }
void SNDDMA_Shutdown(void) {}
void SNDDMA_BeginPainting(void) {}
void SNDDMA_Submit(void) {}

/* --- sound memory pool ---------------------------------------------------- */
void       SND_setup(void) {}
void       SND_shutdown(void) {}
void       SND_free(sndBuffer *v) { (void)v; }
sndBuffer *SND_malloc(void) { return NULL; }
void       S_DisplayFreeMemory(void) {}

/* --- codecs / loading ----------------------------------------------------- */
qboolean S_LoadSound(sfx_t *sfx) { (void)sfx; return qfalse; }
void     S_PaintChannels(int endtime) { (void)endtime; }

snd_stream_t *S_CodecOpenStream(const char *filename) { (void)filename; return NULL; }
void          S_CodecCloseStream(snd_stream_t *stream) { (void)stream; }
int           S_CodecReadStream(snd_stream_t *stream, int bytes, void *buffer) {
	(void)stream; (void)bytes; (void)buffer; return 0;
}

/* --- misc engine ---------------------------------------------------------- */
qboolean CL_VideoRecording(void) { return qfalse; }
void     Cmd_RemoveCommand(const char *cmd_name) { (void)cmd_name; }
int      Com_Milliseconds(void) { return 0; }

cvar_t *Cvar_Get(const char *name, const char *value, int flags) {
	/* Return a stable dummy so callers can dereference ->integer/->value. */
	static cvar_t pool[32];
	static int    used;
	cvar_t       *cv;

	(void)flags;
	if (used >= (int)ARRAY_LEN(pool)) {
		return &pool[0];
	}
	cv = &pool[used++];
	cv->name = (char *)name;
	cv->string = (char *)value;
	cv->value = (float)atof(value);
	cv->integer = atoi(value);
	return cv;
}
