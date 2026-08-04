/*
===========================================================================
ZEQ2-Lite debug socket.

A DEVELOPMENT FACILITY, NOT A SHIPPING FEATURE. It hands anything that can
open a TCP connection the console, the cvar table and the server's view of
the world. It is off unless `net_debugPort` is set, it is latched so it
cannot be switched on mid-session, and it binds 127.0.0.1 only and hangs up
on any peer that is not loopback. Do not open it on a public build.

Why a line-oriented JSON socket rather than MCP directly: MCP is a versioned
protocol with a handshake and periodic spec revisions, and tracking that in
C99 with no JSON library is the wrong trade. The engine speaks something
small and permanent - one JSON object per line in, one per line out, with a
version number - and the MCP adapter in Tools/mcp/ absorbs the churn.

It is polled ONCE PER FRAME from Com_Frame and never from a thread. Nothing
in this engine is thread safe, so reading playerState or executing a console
command off-frame is a data race; a frame of latency is not a problem for a
debugger. Everything below therefore runs on the main thread, between
frames, exactly where a console command would run.

Protocol (see Tools/mcp/README.md for the adapter's side):

  -> {"v":1,"id":"7","op":"state"}
  <- {"v":1,"id":"7","ok":true,"op":"state","time":41234,"server":{...},...}

  ops: ping, version, state, eval, entities, cvar
===========================================================================
*/

#include "server.h"

#if defined(_WIN32)

// Winsock is a different enough animal that supporting it here would mean
// carrying a second copy of every call for a platform this port does not
// build for anyway (see CLAUDE.md - windows/mingw is unported). The cvar
// still exists so a config mentioning it is not an error.
void SV_DebugSocketInit( void ) {
	Cvar_Get( "net_debugPort", "0", CVAR_LATCH );
}
void SV_DebugSocketFrame( void ) {
}
void SV_DebugSocketShutdown( void ) {
}

#else

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define DEBUGSOCKET_PROTOCOL	1

#define MAX_DEBUG_CLIENTS		4
#define DEBUG_IN_SIZE			8192		// one request line, hard ceiling
#define DEBUG_OUT_SIZE			65536		// one response line, hard ceiling
#define DEBUG_EVAL_SIZE			32768		// console output captured per eval
#define DEBUG_EVAL_STAGE		4096		// Com_BeginRedirect's own buffer

#define MAX_JSON_FIELDS			12
#define MAX_JSON_KEY			32
#define MAX_JSON_VALUE			1024
#define MAX_JSON_DEPTH			8

// Mirrors of the game module's enums. The engine deliberately does not
// include Game/Game/bg_public.h - it is a module header and pulling it in
// here would tie the engine to the mod's build. The raw arrays go out
// alongside the named fields so an adapter still sees everything if these
// ever drift.
#define DBG_PL_CURRENT			0
#define DBG_PL_FATIGUE			1
#define DBG_PL_HEALTH			2
#define DBG_PL_MAXIMUM			4
#define DBG_PL_TIER_CURRENT		15
#define DBG_PL_TIER_TOTAL		16
#define DBG_ST_SKILLS			6
#define DBG_PERS_TRAINING_OBJECTIVE	6
#define DBG_PERS_TRAINING_PROGRESS	7
#define DBG_PERS_TRAINING_MASTER	8

typedef struct {
	int		socket;
	char	in[DEBUG_IN_SIZE];
	int		inLen;
	char	out[DEBUG_OUT_SIZE];
	int		outLen;
	int		outSent;
} debugClient_t;

static cvar_t			*net_debugPort;
static int				dbg_listen = -1;
static debugClient_t	dbg_clients[MAX_DEBUG_CLIENTS];

// Redirect bookkeeping. An ERR_DROP thrown by a command run through `eval`
// longjmps out of Com_Frame without unwinding, so the redirect can survive
// into the next frame; the buffers are static, so nothing dangles, but the
// console would stay captured. The next poll notices and lets go.
static qboolean	dbg_redirecting = qfalse;
static char		dbg_evalStage[DEBUG_EVAL_STAGE];
static char		dbg_eval[DEBUG_EVAL_SIZE];
static int		dbg_evalLen;
static qboolean	dbg_evalTruncated;

// An eval capturing across frames, waiting to be answered. At most one, for
// the same reason: Com_BeginRedirect installs a single global.
#define MAX_EVAL_FRAMES	240
static int		dbg_pendingClient = -1;
static int		dbg_pendingFrames;
static char		dbg_pendingId[MAX_JSON_VALUE];
static char		dbg_pendingCmd[MAX_JSON_VALUE];

/*
==============================================================================

	JSON OUTPUT

Hand-rolled because this codebase has no JSON and does not want a
dependency. Every write is bounds checked against one flag: once the writer
overflows it stops writing and the caller throws the whole response away
rather than emitting a truncated object that no parser can read.

==============================================================================
*/

typedef struct {
	char		*buf;
	int			size;
	int			len;
	qboolean	overflow;
	qboolean	comma[MAX_JSON_DEPTH];
	int			depth;
} jsonWriter_t;

static void J_Init( jsonWriter_t *w, char *buf, int size ) {
	Com_Memset( w, 0, sizeof( *w ) );
	w->buf = buf;
	w->size = size;
	buf[0] = '\0';
}

static void J_Putc( jsonWriter_t *w, char c ) {
	if ( w->overflow ) {
		return;
	}
	if ( w->len + 1 >= w->size ) {
		w->overflow = qtrue;
		return;
	}
	w->buf[w->len++] = c;
	w->buf[w->len] = '\0';
}

static void J_Raw( jsonWriter_t *w, const char *s ) {
	while ( *s && !w->overflow ) {
		J_Putc( w, *s++ );
	}
}

// JSON strings carry no control characters and no raw quote or backslash.
// Bytes above 0x7f are passed through: the engine's strings are latin-1-ish
// and the adapter decodes them permissively.
static void J_Str( jsonWriter_t *w, const char *s ) {
	J_Putc( w, '"' );
	if ( s ) {
		for ( ; *s; s++ ) {
			unsigned char c = (unsigned char)*s;

			switch ( c ) {
			case '"':	J_Raw( w, "\\\"" ); break;
			case '\\':	J_Raw( w, "\\\\" ); break;
			case '\n':	J_Raw( w, "\\n" ); break;
			case '\r':	J_Raw( w, "\\r" ); break;
			case '\t':	J_Raw( w, "\\t" ); break;
			default:
				if ( c < 0x20 ) {
					char esc[7];
					Com_sprintf( esc, sizeof( esc ), "\\u%04x", c );
					J_Raw( w, esc );
				} else {
					J_Putc( w, (char)c );
				}
				break;
			}
		}
	}
	J_Putc( w, '"' );
}

static void J_Sep( jsonWriter_t *w ) {
	if ( w->depth > 0 && w->comma[w->depth - 1] ) {
		J_Putc( w, ',' );
	}
	if ( w->depth > 0 ) {
		w->comma[w->depth - 1] = qtrue;
	}
}

static void J_Push( jsonWriter_t *w, char open ) {
	J_Putc( w, open );
	if ( w->depth >= MAX_JSON_DEPTH ) {
		w->overflow = qtrue;
		return;
	}
	w->comma[w->depth++] = qfalse;
}

static void J_Pop( jsonWriter_t *w, char close ) {
	if ( w->depth > 0 ) {
		w->depth--;
	}
	J_Putc( w, close );
}

static void J_ObjOpen( jsonWriter_t *w )	{ J_Push( w, '{' ); }
static void J_ObjClose( jsonWriter_t *w )	{ J_Pop( w, '}' ); }
static void J_ArrClose( jsonWriter_t *w )	{ J_Pop( w, ']' ); }

static void J_Key( jsonWriter_t *w, const char *key ) {
	J_Sep( w );
	J_Str( w, key );
	J_Putc( w, ':' );
}

static void J_KeyStr( jsonWriter_t *w, const char *key, const char *val ) {
	J_Key( w, key );
	J_Str( w, val );
}

static void J_KeyInt( jsonWriter_t *w, const char *key, int val ) {
	char num[24];

	J_Key( w, key );
	Com_sprintf( num, sizeof( num ), "%i", val );
	J_Raw( w, num );
}

static void J_KeyBool( jsonWriter_t *w, const char *key, qboolean val ) {
	J_Key( w, key );
	J_Raw( w, val ? "true" : "false" );
}

static void J_KeyObj( jsonWriter_t *w, const char *key ) {
	J_Key( w, key );
	J_ObjOpen( w );
}

static void J_KeyArr( jsonWriter_t *w, const char *key ) {
	J_Key( w, key );
	J_Push( w, '[' );
}

static void J_ArrInt( jsonWriter_t *w, int val ) {
	char num[24];

	J_Sep( w );
	Com_sprintf( num, sizeof( num ), "%i", val );
	J_Raw( w, num );
}

static void J_ArrObjOpen( jsonWriter_t *w ) {
	J_Sep( w );
	J_ObjOpen( w );
}

// `inf` and `nan` are not JSON, and a non-finite coordinate means the game
// state is already broken - report 0 so the rest of the snapshot still
// parses rather than handing the reader a line it must reject whole.
static void J_KeyVec3( jsonWriter_t *w, const char *key, const vec3_t v ) {
	int i;

	J_KeyArr( w, key );
	for ( i = 0; i < 3; i++ ) {
		char	num[32];
		float	f = v[i];

		J_Sep( w );
		if ( f != f || f > 1e30f || f < -1e30f ) {
			f = 0;
		}
		Com_sprintf( num, sizeof( num ), "%.3f", f );
		J_Raw( w, num );
	}
	J_ArrClose( w );
}

/*
==============================================================================

	JSON INPUT

Deliberately minimal: a flat object of string, number, boolean and null
values, which is everything the verbs below need. Nested values are
rejected rather than skipped, so a malformed or hostile line fails loudly
instead of being half understood. Nothing here trusts a length from the
wire - every copy is bounded by the destination.

==============================================================================
*/

typedef struct {
	char		key[MAX_JSON_KEY];
	char		val[MAX_JSON_VALUE];
	qboolean	isString;
} jsonField_t;

static const char *J_SkipWhite( const char *p ) {
	while ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) {
		p++;
	}
	return p;
}

// Reads a quoted string into dest. Returns NULL on a malformed or oversized
// string; on success returns the character after the closing quote.
static const char *J_ParseString( const char *p, char *dest, int destSize ) {
	int len = 0;

	if ( *p != '"' ) {
		return NULL;
	}
	p++;
	while ( *p != '"' ) {
		int c = (unsigned char)*p;

		if ( !c ) {
			return NULL;			// unterminated
		}
		if ( c == '\\' ) {
			p++;
			switch ( *p ) {
			case '"':	c = '"';	break;
			case '\\':	c = '\\';	break;
			case '/':	c = '/';	break;
			case 'b':	c = '\b';	break;
			case 'f':	c = '\f';	break;
			case 'n':	c = '\n';	break;
			case 'r':	c = '\r';	break;
			case 't':	c = '\t';	break;
			case 'u': {
				int i, v = 0;

				for ( i = 0; i < 4; i++ ) {
					int h = (unsigned char)p[1 + i];

					if ( h >= '0' && h <= '9' )			{ v = v * 16 + h - '0'; }
					else if ( h >= 'a' && h <= 'f' )	{ v = v * 16 + h - 'a' + 10; }
					else if ( h >= 'A' && h <= 'F' )	{ v = v * 16 + h - 'A' + 10; }
					else								{ return NULL; }
				}
				p += 4;
				// Only the ASCII range is meaningful to the engine's
				// strings; anything else becomes a placeholder rather
				// than a half-encoded byte.
				c = ( v > 0 && v < 0x80 ) ? v : '?';
				break;
			}
			default:
				return NULL;
			}
		}
		if ( len + 1 >= destSize ) {
			return NULL;			// too long for the destination
		}
		dest[len++] = (char)c;
		p++;
	}
	dest[len] = '\0';
	return p + 1;
}

// Parses one flat JSON object. Returns the field count, or -1 with err set.
static int J_ParseObject( const char *line, jsonField_t *fields, int maxFields,
						  char *err, int errSize ) {
	const char	*p = J_SkipWhite( line );
	int			count = 0;

	if ( *p != '{' ) {
		Q_strncpyz( err, "request is not a JSON object", errSize );
		return -1;
	}
	p = J_SkipWhite( p + 1 );
	if ( *p == '}' ) {
		return 0;
	}
	for ( ;; ) {
		jsonField_t	*f;

		if ( count >= maxFields ) {
			Q_strncpyz( err, "too many fields in request", errSize );
			return -1;
		}
		f = &fields[count];
		Com_Memset( f, 0, sizeof( *f ) );

		p = J_SkipWhite( p );
		p = J_ParseString( p, f->key, sizeof( f->key ) );
		if ( !p ) {
			Q_strncpyz( err, "bad or oversized key", errSize );
			return -1;
		}
		p = J_SkipWhite( p );
		if ( *p != ':' ) {
			Q_strncpyz( err, "expected ':' after key", errSize );
			return -1;
		}
		p = J_SkipWhite( p + 1 );

		if ( *p == '"' ) {
			p = J_ParseString( p, f->val, sizeof( f->val ) );
			if ( !p ) {
				Q_strncpyz( err, "bad or oversized string value", errSize );
				return -1;
			}
			f->isString = qtrue;
		} else if ( *p == '{' || *p == '[' ) {
			Q_strncpyz( err, "nested values are not supported", errSize );
			return -1;
		} else {
			int len = 0;

			while ( *p && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' ) {
				if ( len + 1 >= (int)sizeof( f->val ) ) {
					Q_strncpyz( err, "oversized value", errSize );
					return -1;
				}
				f->val[len++] = *p++;
			}
			f->val[len] = '\0';
			if ( !len ) {
				Q_strncpyz( err, "empty value", errSize );
				return -1;
			}
		}
		count++;

		p = J_SkipWhite( p );
		if ( *p == ',' ) {
			p++;
			continue;
		}
		if ( *p == '}' ) {
			return count;
		}
		Q_strncpyz( err, "expected ',' or '}'", errSize );
		return -1;
	}
}

static const char *J_Field( const jsonField_t *fields, int count, const char *key ) {
	int i;

	for ( i = 0; i < count; i++ ) {
		if ( !Q_stricmp( fields[i].key, key ) ) {
			return fields[i].val;
		}
	}
	return NULL;
}

static qboolean J_FieldBool( const jsonField_t *fields, int count, const char *key ) {
	const char *v = J_Field( fields, count, key );

	return ( v && ( !Q_stricmp( v, "true" ) || !Q_stricmp( v, "1" ) ) ) ? qtrue : qfalse;
}

/*
==============================================================================

	SNAPSHOT

Everything reported here is read the way the engine already reads it: the
server's playerState through SV_GameClientNum, the entity array the game
module publishes, the configstrings and the cvar table. No trap_ syscall is
added and nothing in Game/ changes, so the QVM build is untouched.

==============================================================================
*/

static const char *DebugConfigString( int index ) {
	if ( index < 0 || index >= MAX_CONFIGSTRINGS || !sv.configstrings[index] ) {
		return "";
	}
	return sv.configstrings[index];
}

// The one local client, i.e. the player the operator is driving. On a
// listen server that is client 0; a dedicated server has no local player
// and reports valid=false.
static int DebugLocalClient( void ) {
	int i;

	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		if ( svs.clients[i].state == CS_ACTIVE && svs.clients[i].netchan.remoteAddress.type == NA_LOOPBACK ) {
			return i;
		}
	}
	return -1;
}

static void DebugWritePlayer( jsonWriter_t *w, int clientNum ) {
	playerState_t	*ps;
	int				i;

	J_KeyObj( w, "player" );
	if ( clientNum < 0 || !svs.clients ) {
		J_KeyBool( w, "valid", qfalse );
		J_ObjClose( w );
		return;
	}
	ps = SV_GameClientNum( clientNum );

	J_KeyBool( w, "valid", qtrue );
	J_KeyInt( w, "clientNum", clientNum );
	J_KeyStr( w, "name", svs.clients[clientNum].name );
	J_KeyVec3( w, "origin", ps->origin );
	J_KeyVec3( w, "angles", ps->viewangles );
	J_KeyVec3( w, "velocity", ps->velocity );
	J_KeyInt( w, "groundEntityNum", ps->groundEntityNum );
	J_KeyInt( w, "pm_type", ps->pm_type );
	J_KeyInt( w, "weapon", ps->weapon );
	J_KeyInt( w, "weaponstate", ps->weaponstate );

	J_KeyInt( w, "powerLevel", ps->powerLevel[DBG_PL_CURRENT] );
	J_KeyInt( w, "powerLevelMax", ps->powerLevel[DBG_PL_MAXIMUM] );
	J_KeyInt( w, "health", ps->powerLevel[DBG_PL_HEALTH] );
	J_KeyInt( w, "fatigue", ps->powerLevel[DBG_PL_FATIGUE] );
	J_KeyInt( w, "tier", ps->powerLevel[DBG_PL_TIER_CURRENT] );
	J_KeyInt( w, "tierTotal", ps->powerLevel[DBG_PL_TIER_TOTAL] );

	// Which skills the player may select right now: one bit per slot in
	// stats[stSkills], which is what pmove tests before honouring a switch.
	J_KeyArr( w, "weapons" );
	for ( i = 0; i < MAX_WEAPONS; i++ ) {
		if ( ps->stats[DBG_ST_SKILLS] & ( 1 << i ) ) {
			J_ArrInt( w, i );
		}
	}
	J_ArrClose( w );

	J_KeyArr( w, "currentSkill" );
	for ( i = 0; i < MAX_WEAPONS; i++ ) {
		J_ArrInt( w, ps->currentSkill[i] );
	}
	J_ArrClose( w );

	J_KeyObj( w, "training" );
	J_KeyInt( w, "objective", ps->persistant[DBG_PERS_TRAINING_OBJECTIVE] );
	J_KeyInt( w, "progress", ps->persistant[DBG_PERS_TRAINING_PROGRESS] );
	J_KeyInt( w, "master", ps->persistant[DBG_PERS_TRAINING_MASTER] );
	J_ObjClose( w );

	// The raw arrays as well, because the named fields above are mirrors of
	// the game module's enums and could drift from it.
	J_KeyArr( w, "powerLevelRaw" );
	for ( i = 0; i < MAX_POWERSTATS; i++ ) {
		J_ArrInt( w, ps->powerLevel[i] );
	}
	J_ArrClose( w );
	J_KeyArr( w, "statsRaw" );
	for ( i = 0; i < MAX_STATS; i++ ) {
		J_ArrInt( w, ps->stats[i] );
	}
	J_ArrClose( w );
	J_KeyArr( w, "persistantRaw" );
	for ( i = 0; i < MAX_PERSISTANT; i++ ) {
		J_ArrInt( w, ps->persistant[i] );
	}
	J_ArrClose( w );

	J_ObjClose( w );
}

static void DebugWriteState( jsonWriter_t *w ) {
	int local = -1;

	J_KeyObj( w, "server" );
	J_KeyBool( w, "running", com_sv_running->integer ? qtrue : qfalse );
	if ( com_sv_running->integer ) {
		int i, active = 0;

		for ( i = 0; i < sv_maxclients->integer; i++ ) {
			if ( svs.clients[i].state >= CS_CONNECTED ) {
				active++;
			}
		}
		J_KeyInt( w, "state", sv.state );
		J_KeyStr( w, "map", Cvar_VariableString( "mapname" ) );
		J_KeyInt( w, "gametype", Cvar_VariableIntegerValue( "g_gametype" ) );
		J_KeyInt( w, "training", Cvar_VariableIntegerValue( "g_training" ) );
		J_KeyInt( w, "cheats", Cvar_VariableIntegerValue( "sv_cheats" ) );
		J_KeyInt( w, "time", sv.time );
		J_KeyInt( w, "clients", active );
		J_KeyInt( w, "maxclients", sv_maxclients->integer );
		J_KeyStr( w, "warmup", DebugConfigString( 5 ) );			// CS_WARMUP
		J_KeyStr( w, "intermission", DebugConfigString( 22 ) );		// CS_INTERMISSION
		J_KeyInt( w, "entities", sv.num_entities );
		local = DebugLocalClient();
	}
	J_ObjClose( w );

	J_KeyObj( w, "client" );
#ifdef DEDICATED
	J_KeyBool( w, "running", qfalse );
#else
	J_KeyBool( w, "running", ( com_cl_running && com_cl_running->integer ) ? qtrue : qfalse );
#endif
	J_ObjClose( w );

	DebugWritePlayer( w, local );
}

static void DebugWriteEntities( jsonWriter_t *w, int max ) {
	int i, written = 0;

	J_KeyArr( w, "entities" );
	if ( com_sv_running->integer && sv.gentities ) {
		for ( i = 0; i < sv.num_entities && written < max; i++ ) {
			sharedEntity_t	*ent = SV_GentityNum( i );
			const char		*model;

			if ( !ent->r.linked && i >= sv_maxclients->integer ) {
				continue;		// not in the world
			}
			model = "";
			if ( ent->s.modelindex > 0 && ent->s.modelindex < MAX_MODELS ) {
				model = DebugConfigString( 32 + ent->s.modelindex );		// CS_MODELS
			}
			J_ArrObjOpen( w );
			J_KeyInt( w, "number", i );
			J_KeyInt( w, "eType", ent->s.eType );
			J_KeyInt( w, "eFlags", ent->s.eFlags );
			J_KeyInt( w, "clientNum", ent->s.clientNum );
			J_KeyVec3( w, "origin", ent->r.currentOrigin );
			J_KeyStr( w, "model", model );
			if ( i < sv_maxclients->integer && svs.clients ) {
				J_KeyStr( w, "name", svs.clients[i].name );
			}
			J_ObjClose( w );
			written++;
		}
	}
	J_ArrClose( w );
	J_KeyInt( w, "count", written );
}

/*
==============================================================================

	EVAL

Console output reaches the player through Com_Printf, so that is what gets
captured: Com_BeginRedirect is the same seam rcon uses. The redirect buffer
is small and its flush appends into a larger accumulator, so a long report -
`ruledump` runs to hundreds of lines - comes back whole instead of only its
last page.

Not every console command answers in the frame it is issued, and this is the
difference that makes or breaks the verb. A SERVER console command
(`ruledump`) runs inside Cmd_ExecuteString and its output is there when the
call returns. A command the CLIENT forwards to the server (`masterlist`,
`where`) becomes a reliable message that the server reads on a later frame,
so ending the redirect immediately captures nothing at all and returns an
empty string that looks like a command with no output.

`frames` is the answer: hold the redirect open for that many further frames
and answer when it closes. The cost is that Com_Printf returns early while a
redirect is up, so those lines go to the caller INSTEAD of to the console and
the log - which is why the default is 0 and the adapter, not the engine,
decides.

==============================================================================
*/

static void DebugEvalFlush( char *text ) {
	int len = (int)strlen( text );

	if ( dbg_evalLen + len + 1 >= (int)sizeof( dbg_eval ) ) {
		len = (int)sizeof( dbg_eval ) - dbg_evalLen - 1;
		dbg_evalTruncated = qtrue;
	}
	if ( len > 0 ) {
		Com_Memcpy( dbg_eval + dbg_evalLen, text, len );
		dbg_evalLen += len;
	}
	dbg_eval[dbg_evalLen] = '\0';
}

static void DebugEval( jsonWriter_t *w, const char *cmd, qboolean buffered ) {
	dbg_evalLen = 0;
	dbg_eval[0] = '\0';
	dbg_evalTruncated = qfalse;

	if ( buffered ) {
		// `map`, `map_restart` and friends cannot run inside a frame; they
		// go through the command buffer and produce no capturable output.
		Cbuf_ExecuteText( EXEC_APPEND, va( "%s\n", cmd ) );
		J_KeyStr( w, "output", "" );
		J_KeyBool( w, "buffered", qtrue );
		return;
	}

	dbg_redirecting = qtrue;
	Com_BeginRedirect( dbg_evalStage, sizeof( dbg_evalStage ), DebugEvalFlush );
	Cmd_ExecuteString( cmd );
	Com_EndRedirect();
	dbg_redirecting = qfalse;

	J_KeyStr( w, "output", dbg_eval );
	J_KeyBool( w, "buffered", qfalse );
	J_KeyBool( w, "truncated", dbg_evalTruncated );
}

// Begins a capture that spans frames. The redirect is one global in
// common.c, so only one may be open at a time across every connection.
static void DebugEvalBegin( int clientIndex, const char *id, const char *cmd,
							int frames ) {
	Q_strncpyz( dbg_pendingId, id ? id : "", sizeof( dbg_pendingId ) );
	Q_strncpyz( dbg_pendingCmd, cmd, sizeof( dbg_pendingCmd ) );
	dbg_pendingFrames = frames;
	dbg_pendingClient = clientIndex;

	dbg_evalLen = 0;
	dbg_eval[0] = '\0';
	dbg_evalTruncated = qfalse;

	dbg_redirecting = qtrue;
	Com_BeginRedirect( dbg_evalStage, sizeof( dbg_evalStage ), DebugEvalFlush );
	Cmd_ExecuteString( cmd );
}

/*
==============================================================================

	REQUEST DISPATCH

==============================================================================
*/

static void DebugError( char *out, int outSize, const char *id, const char *msg ) {
	jsonWriter_t w;

	J_Init( &w, out, outSize );
	J_ObjOpen( &w );
	J_KeyInt( &w, "v", DEBUGSOCKET_PROTOCOL );
	J_KeyStr( &w, "id", id ? id : "" );
	J_KeyBool( &w, "ok", qfalse );
	J_KeyStr( &w, "error", msg );
	J_ObjClose( &w );
	if ( w.overflow ) {
		Q_strncpyz( out, "{\"v\":1,\"ok\":false,\"error\":\"response overflow\"}", outSize );
	}
}

// Returns qfalse when the answer is deferred to a later frame, in which case
// nothing has been written to out.
static qboolean DebugHandleLine( const char *line, char *out, int outSize,
								 int clientIndex ) {
	jsonField_t		fields[MAX_JSON_FIELDS];
	jsonWriter_t	w;
	char			err[128];
	const char		*id, *op, *v;
	int				count;

	count = J_ParseObject( line, fields, MAX_JSON_FIELDS, err, sizeof( err ) );
	if ( count < 0 ) {
		DebugError( out, outSize, "", err );
		return qtrue;
	}
	id = J_Field( fields, count, "id" );
	op = J_Field( fields, count, "op" );
	if ( !op || !op[0] ) {
		DebugError( out, outSize, id, "missing op" );
		return qtrue;
	}

	// The one verb that may not answer in this frame, handled before the
	// reply is started because there may not be a reply yet.
	if ( !Q_stricmp( op, "eval" ) && !J_FieldBool( fields, count, "buffer" ) ) {
		const char	*cmd = J_Field( fields, count, "cmd" );
		const char	*fr = J_Field( fields, count, "frames" );
		int			frames = fr ? atoi( fr ) : 0;

		if ( frames > 0 ) {
			if ( !cmd || !cmd[0] ) {
				DebugError( out, outSize, id, "eval needs a cmd" );
				return qtrue;
			}
			if ( dbg_pendingClient >= 0 ) {
				DebugError( out, outSize, id, "another eval is still capturing" );
				return qtrue;
			}
			if ( frames > MAX_EVAL_FRAMES ) {
				frames = MAX_EVAL_FRAMES;
			}
			DebugEvalBegin( clientIndex, id, cmd, frames );
			return qfalse;
		}
	}

	J_Init( &w, out, outSize );
	J_ObjOpen( &w );
	J_KeyInt( &w, "v", DEBUGSOCKET_PROTOCOL );
	J_KeyStr( &w, "id", id ? id : "" );
	J_KeyBool( &w, "ok", qtrue );
	J_KeyStr( &w, "op", op );
	J_KeyInt( &w, "time", Sys_Milliseconds() );

	if ( !Q_stricmp( op, "ping" ) ) {
		J_KeyBool( &w, "pong", qtrue );
	} else if ( !Q_stricmp( op, "version" ) ) {
		J_KeyInt( &w, "protocol", DEBUGSOCKET_PROTOCOL );
		J_KeyInt( &w, "netProtocol", PROTOCOL_VERSION );
		J_KeyStr( &w, "engine", Q3_VERSION );
		J_KeyStr( &w, "fs_game", Cvar_VariableString( "fs_game" ) );
	} else if ( !Q_stricmp( op, "state" ) ) {
		DebugWriteState( &w );
	} else if ( !Q_stricmp( op, "entities" ) ) {
		const char	*maxStr = J_Field( fields, count, "max" );
		int			max = maxStr ? atoi( maxStr ) : 128;

		if ( max < 1 ) {
			max = 1;
		}
		if ( max > MAX_GENTITIES ) {
			max = MAX_GENTITIES;
		}
		DebugWriteEntities( &w, max );
	} else if ( !Q_stricmp( op, "eval" ) ) {
		const char *cmd = J_Field( fields, count, "cmd" );

		if ( !cmd || !cmd[0] ) {
			DebugError( out, outSize, id, "eval needs a cmd" );
			return qtrue;
		}
		J_KeyStr( &w, "cmd", cmd );
		DebugEval( &w, cmd, J_FieldBool( fields, count, "buffer" ) );
	} else if ( !Q_stricmp( op, "cvar" ) ) {
		const char	*mode = J_Field( fields, count, "mode" );
		const char	*name = J_Field( fields, count, "name" );

		if ( !name || !name[0] ) {
			DebugError( out, outSize, id, "cvar needs a name" );
			return qtrue;
		}
		if ( mode && !Q_stricmp( mode, "set" ) ) {
			v = J_Field( fields, count, "value" );
			if ( !v ) {
				DebugError( out, outSize, id, "cvar set needs a value" );
				return qtrue;
			}
			Cvar_Set( name, v );
		}
		J_KeyStr( &w, "name", name );
		J_KeyStr( &w, "value", Cvar_VariableString( name ) );
		J_KeyInt( &w, "integer", Cvar_VariableIntegerValue( name ) );
	} else {
		DebugError( out, outSize, id, va( "unknown op '%s'", op ) );
		return qtrue;
	}

	J_ObjClose( &w );
	if ( w.overflow ) {
		DebugError( out, outSize, id, "response too large for the debug socket" );
	}
	return qtrue;
}

/*
==============================================================================

	TRANSPORT

==============================================================================
*/

static void DebugCloseClient( debugClient_t *cl ) {
	if ( cl->socket >= 0 ) {
		close( cl->socket );
	}
	Com_Memset( cl, 0, sizeof( *cl ) );
	cl->socket = -1;
}

static void DebugAccept( void ) {
	for ( ;; ) {
		struct sockaddr_in	from;
		socklen_t			fromLen = sizeof( from );
		int					s, i, flags;

		s = accept( dbg_listen, (struct sockaddr *)&from, &fromLen );
		if ( s < 0 ) {
			return;					// EAGAIN, or nothing waiting
		}
		// Belt and braces: the listener is already bound to loopback, but a
		// socket that hands out the console has no business trusting that
		// alone.
		if ( from.sin_family != AF_INET ||
			 from.sin_addr.s_addr != htonl( INADDR_LOOPBACK ) ) {
			Com_Printf( "debug socket: refused a non-loopback connection\n" );
			close( s );
			continue;
		}
		for ( i = 0; i < MAX_DEBUG_CLIENTS; i++ ) {
			if ( dbg_clients[i].socket < 0 ) {
				break;
			}
		}
		if ( i == MAX_DEBUG_CLIENTS ) {
			close( s );
			continue;
		}
		flags = fcntl( s, F_GETFL, 0 );
		fcntl( s, F_SETFL, flags | O_NONBLOCK );
		i = 1;
		setsockopt( s, IPPROTO_TCP, TCP_NODELAY, (char *)&i, sizeof( i ) );

		for ( i = 0; i < MAX_DEBUG_CLIENTS; i++ ) {
			if ( dbg_clients[i].socket < 0 ) {
				Com_Memset( &dbg_clients[i], 0, sizeof( dbg_clients[i] ) );
				dbg_clients[i].socket = s;
				break;
			}
		}
	}
}

static void DebugFlush( debugClient_t *cl ) {
	while ( cl->outSent < cl->outLen ) {
		int sent = (int)send( cl->socket, cl->out + cl->outSent,
							  cl->outLen - cl->outSent, 0 );

		if ( sent > 0 ) {
			cl->outSent += sent;
			continue;
		}
		if ( sent < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) ) {
			return;					// finish next frame
		}
		DebugCloseClient( cl );
		return;
	}
	cl->outLen = 0;
	cl->outSent = 0;
}

static void DebugRead( debugClient_t *cl, int index ) {
	int		got;
	char	*nl;

	// One request in flight at a time - either a reply still going out, or
	// an eval capturing across frames.
	if ( cl->outLen || dbg_pendingClient == index ) {
		return;
	}
	got = (int)recv( cl->socket, cl->in + cl->inLen,
					 sizeof( cl->in ) - cl->inLen - 1, 0 );
	if ( got == 0 ) {
		DebugCloseClient( cl );
		return;
	}
	if ( got < 0 ) {
		if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
			return;
		}
		DebugCloseClient( cl );
		return;
	}
	cl->inLen += got;
	cl->in[cl->inLen] = '\0';

	nl = strchr( cl->in, '\n' );
	if ( !nl ) {
		// A line that fills the buffer without a newline is either a bug or
		// an attack; either way there is nothing sensible to answer with.
		if ( cl->inLen >= (int)sizeof( cl->in ) - 1 ) {
			Com_Printf( "debug socket: request line over %i bytes, dropping the connection\n",
						(int)sizeof( cl->in ) - 1 );
			DebugCloseClient( cl );
		}
		return;
	}
	*nl = '\0';

	if ( DebugHandleLine( cl->in, cl->out, sizeof( cl->out ) - 2, index ) ) {
		cl->outLen = (int)strlen( cl->out );
		cl->out[cl->outLen++] = '\n';
		cl->out[cl->outLen] = '\0';
		cl->outSent = 0;
	}

	// Keep whatever followed the newline; the adapter is lockstep, so this
	// is normally nothing.
	{
		int remain = cl->inLen - (int)( nl - cl->in ) - 1;

		if ( remain > 0 ) {
			memmove( cl->in, nl + 1, remain );
		} else {
			remain = 0;
		}
		cl->inLen = remain;
		cl->in[cl->inLen] = '\0';
	}
}

// Answers an eval whose capture window has run out. Called before anything
// else in the frame so the reply goes out at the earliest opportunity.
static void DebugFinishPending( void ) {
	jsonWriter_t	w;
	debugClient_t	*cl;

	if ( dbg_pendingClient < 0 ) {
		return;
	}
	if ( --dbg_pendingFrames > 0 ) {
		return;
	}
	Com_EndRedirect();
	dbg_redirecting = qfalse;

	cl = &dbg_clients[dbg_pendingClient];
	dbg_pendingClient = -1;
	if ( cl->socket < 0 ) {
		return;					// hung up while we were capturing
	}

	J_Init( &w, cl->out, (int)sizeof( cl->out ) - 2 );
	J_ObjOpen( &w );
	J_KeyInt( &w, "v", DEBUGSOCKET_PROTOCOL );
	J_KeyStr( &w, "id", dbg_pendingId );
	J_KeyBool( &w, "ok", qtrue );
	J_KeyStr( &w, "op", "eval" );
	J_KeyInt( &w, "time", Sys_Milliseconds() );
	J_KeyStr( &w, "cmd", dbg_pendingCmd );
	J_KeyStr( &w, "output", dbg_eval );
	J_KeyBool( &w, "buffered", qfalse );
	J_KeyBool( &w, "truncated", dbg_evalTruncated );
	J_ObjClose( &w );
	if ( w.overflow ) {
		DebugError( cl->out, (int)sizeof( cl->out ) - 2, dbg_pendingId,
					"response too large for the debug socket" );
	}
	cl->outLen = (int)strlen( cl->out );
	cl->out[cl->outLen++] = '\n';
	cl->out[cl->outLen] = '\0';
	cl->outSent = 0;
}

/*
=================
SV_DebugSocketInit
=================
*/
void SV_DebugSocketInit( void ) {
	struct sockaddr_in	addr;
	int					i, port, flags, one = 1;

	for ( i = 0; i < MAX_DEBUG_CLIENTS; i++ ) {
		dbg_clients[i].socket = -1;
	}

	// Latched, so it cannot be turned on from the console mid-session, and
	// not archived, so a development run cannot leave it enabled in the
	// player's saved config.
	net_debugPort = Cvar_Get( "net_debugPort", "0", CVAR_LATCH );

	port = net_debugPort->integer;
	if ( !port ) {
		return;
	}
	if ( port < 1024 || port > 65535 ) {
		Com_Printf( "debug socket: net_debugPort %i is out of range, not opening\n", port );
		return;
	}

	dbg_listen = socket( PF_INET, SOCK_STREAM, IPPROTO_TCP );
	if ( dbg_listen < 0 ) {
		Com_Printf( "debug socket: socket() failed: %s\n", strerror( errno ) );
		return;
	}
	setsockopt( dbg_listen, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof( one ) );
	flags = fcntl( dbg_listen, F_GETFL, 0 );
	fcntl( dbg_listen, F_SETFL, flags | O_NONBLOCK );

	Com_Memset( &addr, 0, sizeof( addr ) );
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
	addr.sin_port = htons( (unsigned short)port );

	if ( bind( dbg_listen, (struct sockaddr *)&addr, sizeof( addr ) ) < 0 ||
		 listen( dbg_listen, MAX_DEBUG_CLIENTS ) < 0 ) {
		Com_Printf( "debug socket: could not listen on 127.0.0.1:%i: %s\n",
					port, strerror( errno ) );
		close( dbg_listen );
		dbg_listen = -1;
		return;
	}
	Com_Printf( "debug socket: listening on 127.0.0.1:%i (development facility)\n", port );
}

/*
=================
SV_DebugSocketFrame

Called once per frame from Com_Frame. Never from a thread.
=================
*/
void SV_DebugSocketFrame( void ) {
	int i;

	if ( dbg_listen < 0 ) {
		return;
	}
	// An ERR_DROP inside a command run by `eval` longjmps past
	// Com_EndRedirect. A capture that is still counting down frames owns the
	// redirect legitimately; anything else holding it is a leftover.
	if ( dbg_redirecting && dbg_pendingClient < 0 ) {
		Com_EndRedirect();
		dbg_redirecting = qfalse;
	}
	DebugFinishPending();

	DebugAccept();

	for ( i = 0; i < MAX_DEBUG_CLIENTS; i++ ) {
		debugClient_t *cl = &dbg_clients[i];

		if ( cl->socket < 0 ) {
			continue;
		}
		DebugFlush( cl );
		if ( cl->socket < 0 ) {
			continue;
		}
		DebugRead( cl, i );
		if ( cl->socket < 0 ) {
			continue;
		}
		DebugFlush( cl );
	}
}

/*
=================
SV_DebugSocketShutdown
=================
*/
void SV_DebugSocketShutdown( void ) {
	int i;

	for ( i = 0; i < MAX_DEBUG_CLIENTS; i++ ) {
		if ( dbg_clients[i].socket >= 0 ) {
			DebugCloseClient( &dbg_clients[i] );
		}
	}
	if ( dbg_listen >= 0 ) {
		close( dbg_listen );
		dbg_listen = -1;
	}
}

#endif	// !_WIN32
