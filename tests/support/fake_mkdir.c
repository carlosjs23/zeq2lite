/*
Recording fake for Sys_Mkdir. See fake_mkdir.h.
*/

#include "q_shared.h"
#include "qcommon.h"
#include "fake_mkdir.h"

static char	paths[FAKE_MKDIR_MAX][MAX_OSPATH];
static int	numPaths;

void fake_mkdir_reset( void ) {
	memset( paths, 0, sizeof( paths ) );
	numPaths = 0;
}

int fake_mkdir_count( void ) {
	return numPaths;
}

const char *fake_mkdir_path( int index ) {
	if ( index < 0 || index >= numPaths ) {
		return "";
	}
	return paths[index];
}

qboolean Sys_Mkdir( const char *path ) {
	if ( numPaths < FAKE_MKDIR_MAX ) {
		Q_strncpyz( paths[numPaths], path, sizeof( paths[numPaths] ) );
		numPaths++;
	}

	// FS_CreatePath treats a false return as fatal, so a fake that reported
	// failure would turn every test into a Com_Error rather than an assertion.
	return qtrue;
}
