/*
Link seams for Shared/files.c.

files.c is the least isolated unit in the codebase: it reaches the command
buffer, cvars, the zone and hunk allocators, the sound system and the platform
layer. That is a fact about files.c, not something to paper over, so the whole
surface is stubbed inertly here and the suite drives only the path arithmetic.

The zip reader is *not* stubbed - the suite links Shared/unzip.c and zlib for
real, because signature drift in fourteen hand-written unz* stubs would be a
worse trade than the extra translation units.

Nothing here allocates from the engine's zone: Z_Malloc/Z_Free map onto malloc so
that ASan sees ordinary heap traffic and can still catch an overflow inside the
function under test.
*/

#include "q_shared.h"
#include "qcommon.h"

/* ------------------------------------------------------------ command buffer */

void Cbuf_AddText( const char *text ) {}
void Cmd_AddCommand( const char *cmd_name, xcommand_t function ) {}
void Cmd_RemoveCommand( const char *cmd_name ) {}
void Cmd_TokenizeString( const char *text ) {}
int Cmd_Argc( void ) { return 0; }
char *Cmd_Argv( int arg ) { return ""; }

/* -------------------------------------------------------------------- common */

unsigned Com_BlockChecksum( const void *buffer, int length ) { return 0; }
int Com_FilterPath( char *filter, char *name, int casesensitive ) { return 0; }
void Com_GameRestart( int checksumFeed, qboolean disconnect ) {}
qboolean Com_SafeMode( void ) { return qfalse; }
void Com_StartupVariable( const char *match ) {}

qboolean com_fullyInitialized = qfalse;
fileHandle_t com_journalDataFile = 0;
int demo_protocols[] = { 0 };

/* ---------------------------------------------------------------------- cvars */

/*
A single shared cvar satisfies every Cvar_Get in files.c. The suite never reads
one back, and handing out the same object keeps the stub honest about the fact
that it is not modelling cvar semantics at all.
*/
static cvar_t stubCvar;
static char   stubCvarString[] = "";

cvar_t *com_basegame = &stubCvar;
cvar_t *com_journal = &stubCvar;
cvar_t *com_protocol = &stubCvar;

cvar_t *Cvar_Get( const char *var_name, const char *value, int flags ) {
	stubCvar.name = (char *)var_name;
	stubCvar.string = stubCvarString;
	stubCvar.resetString = stubCvarString;
	return &stubCvar;
}

void Cvar_Set( const char *var_name, const char *value ) {}
char *Cvar_VariableString( const char *var_name ) { return ""; }

/* ------------------------------------------------------------------ memory */

void *Z_MallocDebug( int size, char *label, char *file, int line ) {
	void *p = malloc( size );

	if ( p ) {
		memset( p, 0, size );	// Z_Malloc returns zero-filled memory
	}
	return p;
}

void Z_Free( void *ptr ) { free( ptr ); }
char *CopyString( const char *in ) { return strdup( in ); }

void *Hunk_AllocateTempMemory( int size ) { return malloc( size ); }
void Hunk_FreeTempMemory( void *buf ) { free( buf ); }
void Hunk_ClearTempMemory( void ) {}

/* ------------------------------------------------------------------- system */

char *Sys_DefaultInstallPath( void ) { return "."; }
char *Sys_DefaultHomePath( void ) { return "."; }
char *Sys_DefaultAppPath( void ) { return "."; }

char **Sys_ListFiles( const char *directory, const char *extension, char *filter,
		int *numfiles, qboolean wantsubs ) {
	if ( numfiles ) {
		*numfiles = 0;
	}
	return NULL;
}

void Sys_FreeFileList( char **list ) {}
FILE *Sys_Mkfifo( const char *ospath ) { return NULL; }

/* -------------------------------------------------------------------- sound */

void S_ClearSoundBuffer( void ) {}
