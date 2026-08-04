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

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>		// write, _exit, STDERR_FILENO - the async-signal-safe
						// half of Sys_SigHandler
#include <limits.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifndef DEDICATED
#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#	include "SDL_cpuinfo.h"
#else
#	include <SDL.h>
#	include <SDL_cpuinfo.h>
#endif
#endif

#include "sys_local.h"
#include "sys_loadlib.h"

#include "../../Shared/q_shared.h"
#include "../../Shared/qcommon.h"

static char binaryPath[ MAX_OSPATH ] = { 0 };
static char installPath[ MAX_OSPATH ] = { 0 };

/*
=================
Sys_SetBinaryPath
=================
*/
void Sys_SetBinaryPath(const char *path)
{
	Q_strncpyz(binaryPath, path, sizeof(binaryPath));
}

/*
=================
Sys_BinaryPath
=================
*/
char *Sys_BinaryPath(void)
{
	return binaryPath;
}

/*
=================
Sys_SetDefaultInstallPath
=================
*/
void Sys_SetDefaultInstallPath(const char *path)
{
	Q_strncpyz(installPath, path, sizeof(installPath));
}

/*
=================
Sys_DefaultInstallPath
=================
*/
char *Sys_DefaultInstallPath(void)
{
	if (*installPath)
		return installPath;
	else
		return Sys_Cwd();
}

/*
=================
Sys_DefaultAppPath
=================
*/
char *Sys_DefaultAppPath(void)
{
	return Sys_BinaryPath();
}

/*
=================
Sys_In_Restart_f

Restart the input subsystem
=================
*/
void Sys_In_Restart_f( void )
{
	IN_Restart( );
}

/*
=================
Sys_ConsoleInput

Handle new console input
=================
*/
char *Sys_ConsoleInput(void)
{
	return CON_Input( );
}

#ifdef DEDICATED
#	define PID_FILENAME PRODUCT_NAME "_server.pid"
#else
#	define PID_FILENAME PRODUCT_NAME ".pid"
#endif

/*
=================
Sys_PIDFileName
=================
*/
static char *Sys_PIDFileName( void )
{
	return va( "%s/%s", Sys_TempPath( ), PID_FILENAME );
}

/*
=================
Sys_WritePIDFile

Return qtrue if there is an existing stale PID file
=================
*/
qboolean Sys_WritePIDFile( void )
{
	char      *pidFile = Sys_PIDFileName( );
	FILE      *f;
	qboolean  stale = qfalse;

	// First, check if the pid file is already there
	if( ( f = fopen( pidFile, "r" ) ) != NULL )
	{
		char  pidBuffer[ 64 ] = { 0 };
		int   pid;

		pid = fread( pidBuffer, sizeof( char ), sizeof( pidBuffer ) - 1, f );
		fclose( f );

		if(pid > 0)
		{
			pid = atoi( pidBuffer );
			if( !Sys_PIDIsRunning( pid ) )
				stale = qtrue;
		}
		else
			stale = qtrue;
	}

	if( ( f = fopen( pidFile, "w" ) ) != NULL )
	{
		fprintf( f, "%d", Sys_PID( ) );
		fclose( f );
	}
	else
		Com_Printf( S_COLOR_YELLOW "Couldn't write %s.\n", pidFile );

	return stale;
}

/*
=================
Sys_Exit

Single exit point (regular exit or in case of error)
=================
*/
static __attribute__ ((noreturn)) void Sys_Exit( int exitCode )
{
	fprintf( stderr, "\n=== Sys_Exit( %d ) called ===\n", exitCode ); fflush( stderr );
	CON_Shutdown( );

#ifndef DEDICATED
	SDL_Quit( );
#endif

	if( exitCode < 2 )
	{
		// Normal exit
		remove( Sys_PIDFileName( ) );
	}

	Sys_PlatformExit( );

	exit( exitCode );
}

/*
=================
Sys_Quit
=================
*/
void Sys_Quit( void )
{
	Sys_Exit( 0 );
}

/*
=================
Sys_GetProcessorFeatures
=================
*/
cpuFeatures_t Sys_GetProcessorFeatures( void )
{
	cpuFeatures_t features = 0;

#ifndef DEDICATED
	// SDL2 dropped SDL_HasMMXExt() and SDL_Has3DNowExt(); both were only ever
	// true on pre-2004 AMD parts, so CF_MMX_EXT / CF_3DNOW_EXT are never set.
	if( SDL_HasRDTSC( ) )    features |= CF_RDTSC;
	if( SDL_HasMMX( ) )      features |= CF_MMX;
	if( SDL_Has3DNow( ) )    features |= CF_3DNOW;
	if( SDL_HasSSE( ) )      features |= CF_SSE;
	if( SDL_HasSSE2( ) )     features |= CF_SSE2;
	if( SDL_HasAltiVec( ) )  features |= CF_ALTIVEC;
#endif

	return features;
}

/*
=================
Sys_LowPhysicalMemory

Gates the two prewarm passes that touch everything once so the driver and the
pager have it resident - Com_TouchMemory over the hunk and RB_ShowImages over
the texture set. On a machine this small both would thrash swap instead.
=================
*/
#define MEM_THRESHOLD_MB 96

qboolean Sys_LowPhysicalMemory( void )
{
#ifdef DEDICATED
	return qfalse;
#else
	// 0 means SDL could not tell, which must not read as "tiny machine" and
	// turn both passes off.
	int ram = SDL_GetSystemRAM( );

	return ( ram > 0 && ram <= MEM_THRESHOLD_MB ) ? qtrue : qfalse;
#endif
}

/*
=================
Sys_Init
=================
*/
void Sys_Init(void)
{
	Cmd_AddCommand( "in_restart", Sys_In_Restart_f );
	Cvar_Set( "arch", OS_STRING " " ARCH_STRING );
	Cvar_Set( "username", Sys_GetCurrentUser( ) );
}

/*
=================
Sys_AnsiColorPrint

Transform Q3 colour codes to ANSI escape sequences
=================
*/
void Sys_AnsiColorPrint( const char *msg )
{
	static char buffer[ MAXPRINTMSG ];
	int         length = 0;
	static int  q3ToAnsi[ 8 ] =
	{
		30, // COLOR_BLACK
		31, // COLOR_RED
		32, // COLOR_GREEN
		33, // COLOR_YELLOW
		34, // COLOR_BLUE
		36, // COLOR_CYAN
		35, // COLOR_MAGENTA
		0   // COLOR_WHITE
	};

	while( *msg )
	{
		if( Q_IsColorString( msg ) || *msg == '\n' )
		{
			// First empty the buffer
			if( length > 0 )
			{
				buffer[ length ] = '\0';
				fputs( buffer, stderr );
				length = 0;
			}

			if( *msg == '\n' )
			{
				// Issue a reset and then the newline
				fputs( "\033[0m\n", stderr );
				msg++;
			}
			else
			{
				// Print the color code
				Com_sprintf( buffer, sizeof( buffer ), "\033[%dm",
						q3ToAnsi[ ColorIndex( *( msg + 1 ) ) ] );
				fputs( buffer, stderr );
				msg += 2;
			}
		}
		else
		{
			if( length >= MAXPRINTMSG - 1 )
				break;

			buffer[ length ] = *msg;
			length++;
			msg++;
		}
	}

	// Empty anything still left in the buffer
	if( length > 0 )
	{
		buffer[ length ] = '\0';
		fputs( buffer, stderr );
	}
}

/*
=================
Sys_Print
=================
*/
void Sys_Print( const char *msg )
{
	CON_LogWrite( msg );
	CON_Print( msg );
}

/*
=================
Sys_Error
=================
*/
void Sys_Error( const char *error, ... )
{
	va_list argptr;
	char    string[1024];

	va_start (argptr,error);
	Q_vsnprintf (string, sizeof(string), error, argptr);
	va_end (argptr);

	Sys_ErrorDialog( string );

	Sys_Exit( 3 );
}

#if 0
/*
=================
Sys_Warn
=================
*/
static __attribute__ ((format (printf, 1, 2))) void Sys_Warn( char *warning, ... )
{
	va_list argptr;
	char    string[1024];

	va_start (argptr,warning);
	Q_vsnprintf (string, sizeof(string), warning, argptr);
	va_end (argptr);

	CON_Print( va( "Warning: %s", string ) );
}
#endif

/*
============
Sys_FileTime

returns -1 if not present
============
*/
int Sys_FileTime( char *path )
{
	struct stat buf;

	if (stat (path,&buf) == -1)
		return -1;

	return buf.st_mtime;
}

/*
=================
Sys_UnloadDll
=================
*/
void Sys_UnloadDll( void *dllHandle )
{
	if( !dllHandle )
	{
		Com_Printf("Sys_UnloadDll(NULL)\n");
		return;
	}

	Sys_UnloadLibrary(dllHandle);
}

/*
=================
Sys_LoadDll

First try to load library name from system library path,
from executable path, then fs_basepath.
=================
*/

void *Sys_LoadDll(const char *name, qboolean useSystemLib)
{
	void *dllhandle;

	// a downloaded pk3 lands in the game directory the module search walks,
	// so refuse to hand one to the dynamic loader whatever it is named after
	if (COM_CompareExtension(name, ".pk3"))
	{
		Com_Printf("Rejecting DLL named \"%s\"\n", name);
		return NULL;
	}

	if(useSystemLib)
		Com_Printf("Trying to load \"%s\"...\n", name);
	
	if(!useSystemLib || !(dllhandle = Sys_LoadLibrary(name)))
	{
		const char *topDir;
		char libPath[MAX_OSPATH];

		topDir = Sys_BinaryPath();

		if(!*topDir)
			topDir = ".";

		Com_Printf("Trying to load \"%s\" from \"%s\"...\n", name, topDir);
		Com_sprintf(libPath, sizeof(libPath), "%s%c%s", topDir, PATH_SEP, name);

		if(!(dllhandle = Sys_LoadLibrary(libPath)))
		{
			const char *basePath = Cvar_VariableString("fs_basepath");
			
			if(!basePath || !*basePath)
				basePath = ".";
			
			if(FS_FilenameCompare(topDir, basePath))
			{
				Com_Printf("Trying to load \"%s\" from \"%s\"...\n", name, basePath);
				Com_sprintf(libPath, sizeof(libPath), "%s%c%s", basePath, PATH_SEP, name);
				dllhandle = Sys_LoadLibrary(libPath);
			}
			
			if(!dllhandle)
				Com_Printf("Loading \"%s\" failed\n", name);
		}
	}
	
	return dllhandle;
}

/*
=================
Sys_LoadGameDll

Used to load a development dll instead of a virtual machine
=================
*/
void *Sys_LoadGameDll(const char *name,
	intptr_t (QDECL **entryPoint)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t),
	intptr_t (*systemcalls)(intptr_t, ...))
{
	void *libHandle;
	void (*dllEntry)(intptr_t (*syscallptr)(intptr_t, ...));

	assert(name);

	Com_Printf( "Loading DLL file: %s\n", name);
	libHandle = Sys_LoadLibrary(name);
	if (!libHandle && name[0] != '/' && (name[0] != '.' || name[1] != '/')) {
		char altPath[MAX_OSPATH];
		Com_sprintf(altPath, sizeof(altPath), "./%s", name);
		libHandle = Sys_LoadLibrary(altPath);
	}

	if(!libHandle)
	{
		Com_Printf("Sys_LoadGameDll(%s) failed:\n\"%s\"\n", name, Sys_LibraryError());
		return NULL;
	}

	dllEntry = Sys_LoadFunction( libHandle, "dllEntry" );
	if (!dllEntry) {
		dllEntry = Sys_LoadFunction( libHandle, "_dllEntry" );
	}

	*entryPoint = Sys_LoadFunction( libHandle, "vmMain" );
	if (!*entryPoint) {
		*entryPoint = Sys_LoadFunction( libHandle, "_vmMain" );
	}

	if ( !*entryPoint || !dllEntry )
	{
		Com_Printf ( "Sys_LoadGameDll(%s) failed to find vmMain function:\n\"%s\" !\n", name, Sys_LibraryError( ) );
		Sys_UnloadLibrary(libHandle);

		return NULL;
	}

	Com_Printf ( "Sys_LoadGameDll(%s) found vmMain function at %p\n", name, *entryPoint );
	dllEntry( systemcalls );

	return libHandle;
}

/*
=================
Sys_ParseArgs
=================
*/
void Sys_ParseArgs( int argc, char **argv )
{
	if( argc == 2 )
	{
		if( !strcmp( argv[1], "--version" ) ||
				!strcmp( argv[1], "-v" ) )
		{
			const char* date = __DATE__;
#ifdef DEDICATED
			fprintf( stdout, Q3_VERSION " dedicated server (%s)\n", date );
#else
			fprintf( stdout, Q3_VERSION " client (%s)\n", date );
#endif
			Sys_Exit( 0 );
		}
	}
}

#ifndef DEFAULT_BASEDIR
#	ifdef MACOS_X
#		define DEFAULT_BASEDIR Sys_StripAppBundle(Sys_BinaryPath())
#	else
#		define DEFAULT_BASEDIR Sys_BinaryPath()
#	endif
#endif

// Set by the handler, read by the main loop. sig_atomic_t and volatile because
// this is the only thing a handler may safely hand to the rest of the program.
volatile sig_atomic_t	sys_quitRequested = 0;

/*
=================
Sys_SigHandler

Async-signal-safe, which the previous version was not: it ran the whole engine
teardown - VM_Forced_Unload_Start, CL_Shutdown, SV_Shutdown - from inside the
handler, along with fprintf and va. Those take locks and touch the allocator, so
a signal that arrived while the main thread was already inside malloc re-entered
it from the handler and faulted. That is why killing the engine mid-frame died
about one time in four while a clean "quit" never did, and why the second fault
landed on top of the first with no usable diagnostic.

A termination request - SIGTERM, SIGINT or SIGHUP - now only sets a flag; the
main loop performs the same shutdown it would for a typed "quit", on its own
stack, between frames.

A fault signal cannot be handled that way - there is no safe frame to return to
- so it writes a fixed string with write(), which is on the safe list, and
leaves. No teardown: a crashed process has nothing to gain from unloading its
dylibs, and attempting it is what turned a diagnosable fault into a double one.
=================
*/
void Sys_SigHandler( int signal )
{
	static volatile sig_atomic_t	signalcaught = 0;

	// SIGHUP is conditional because the C standard does not require it and the
	// mingw headers do not always define it.
	if( signal == SIGTERM || signal == SIGINT
#ifdef SIGHUP
		|| signal == SIGHUP
#endif
		)
	{
		sys_quitRequested = signal;
		return;
	}

	if( signalcaught )
		_exit( 2 );

	signalcaught = 1;

	// write() rather than fprintf(): the stdio lock may be held by the code
	// this signal interrupted.
	{
		static const char msg[] = "\n=== fatal signal, exiting ===\n";
		ssize_t written = write( STDERR_FILENO, msg, sizeof( msg ) - 1 );
		(void)written;
	}

	_exit( 2 );
}

/*
=================
main
=================
*/
int main( int argc, char **argv )
{
	int   i;
	char  commandLine[ MAX_STRING_CHARS ] = { 0 };

#ifndef DEDICATED
	// SDL version check

	// Compile time
#	if !SDL_VERSION_ATLEAST(MINSDL_MAJOR,MINSDL_MINOR,MINSDL_PATCH)
#		error A more recent version of SDL is required
#	endif

	// Run time -- SDL_Linked_Version() was replaced by SDL_GetVersion()
	SDL_version sdlVersion;
	const SDL_version *ver = &sdlVersion;

	SDL_GetVersion( &sdlVersion );

#define MINSDL_VERSION \
	XSTRING(MINSDL_MAJOR) "." \
	XSTRING(MINSDL_MINOR) "." \
	XSTRING(MINSDL_PATCH)

	if( SDL_VERSIONNUM( ver->major, ver->minor, ver->patch ) <
			SDL_VERSIONNUM( MINSDL_MAJOR, MINSDL_MINOR, MINSDL_PATCH ) )
	{
		Sys_Dialog( DT_ERROR, va( "SDL version " MINSDL_VERSION " or greater is required, "
			"but only version %d.%d.%d was found. You may be able to obtain a more recent copy "
			"from http://www.libsdl.org/.", ver->major, ver->minor, ver->patch ), "SDL Library Too Old" );

		Sys_Exit( 1 );
	}
#endif

	Sys_PlatformInit( );

	// Set the initial time base
	Sys_Milliseconds( );

	Sys_ParseArgs( argc, argv );
	Sys_SetBinaryPath( Sys_Dirname( argv[ 0 ] ) );
	Sys_SetDefaultInstallPath( DEFAULT_BASEDIR );

	// Concatenate the command line for passing to Com_Init
	for( i = 1; i < argc; i++ )
	{
		const qboolean containsSpaces = strchr(argv[i], ' ') != NULL;
		if (containsSpaces)
			Q_strcat( commandLine, sizeof( commandLine ), "\"" );

		Q_strcat( commandLine, sizeof( commandLine ), argv[ i ] );

		if (containsSpaces)
			Q_strcat( commandLine, sizeof( commandLine ), "\"" );

		Q_strcat( commandLine, sizeof( commandLine ), " " );
	}

	Com_Init( commandLine );
	NET_Init( );

	CON_Init( );

	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = Sys_SigHandler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;

		// ZEQ2_NO_SIGHANDLER leaves the fault signals to whatever is watching.
		// Catching SIGSEGV means a sanitizer, a debugger or the OS crash
		// reporter never sees the fault: it surfaces as one line of ours with
		// no faulting address and no stack.
		if( !getenv( "ZEQ2_NO_SIGHANDLER" ) )
		{
			sigaction( SIGILL, &sa, NULL );
			sigaction( SIGFPE, &sa, NULL );
			sigaction( SIGSEGV, &sa, NULL );
			sigaction( SIGABRT, &sa, NULL );
			sigaction( SIGBUS, &sa, NULL );
		}

		// Termination is not a fault, and the flag it sets is what runs the
		// orderly shutdown. Always handled, so a debugging run can drop the
		// fault handlers and still exercise that path. SIGHUP is the third of
		// these and installs in Sys_PlatformInit, with the rest of the signals
		// that only exist on unix.
		sigaction( SIGTERM, &sa, NULL );
		sigaction( SIGINT, &sa, NULL );
	}

	while( 1 )
	{
		// Between frames, on the main stack, where the allocator and the stdio
		// locks are ours to take. Through the command buffer rather than
		// calling Com_Quit_f directly: it names the shutdown reason with
		// Cmd_Args(), which without a command to parse holds the arguments of
		// whatever ran last, and prints those as the reason. Does not return.
		if( sys_quitRequested )
		{
			Com_Printf( "Received signal %d, shutting down\n", (int)sys_quitRequested );
			Cbuf_ExecuteText( EXEC_NOW, "quit\n" );
		}

		IN_Frame( );
		Com_Frame( );
	}

	return 0;
}

