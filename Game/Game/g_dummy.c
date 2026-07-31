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
// g_dummy.c -- training dummies: player-shaped targets to hit
//
// There is no bot support in this tree, so a dummy is an ordinary client slot
// that the game module connects, spawns and feeds usercmds to itself. The
// engine is not involved: the slot's client_t stays CS_FREE, so the server
// never sends it a snapshot or expects one back, while the entity it owns is
// transmitted like any other player.
//
// A dummy does not move or fight. It stands where it was put, turns to face
// the nearest player, and takes damage.

#include "g_local.h"

#define	DUMMY_DISTANCE_DEFAULT	200
#define	DUMMY_DISTANCE_MIN		64
#define	DUMMY_DISTANCE_MAX		8000
#define	DUMMY_DROP_MAX			256

/*
=================
G_DummySlot

Top down, because the engine hands real connections out from the bottom up
(SV_DirectConnect scans from slot 0) and a dummy holds a slot the engine still
believes is free.
=================
*/
static int G_DummySlot( void ) {
	int		i;

	for ( i = level.maxclients - 1 ; i >= 0 ; i-- ) {
		if ( level.clients[i].pers.connected != CON_DISCONNECTED ) {
			continue;
		}
		if ( g_entities[i].inuse ) {
			continue;
		}
		return i;
	}

	return -1;
}

/*
=================
G_DummyModelOk

The model name ends up in an info string, so it must not carry any of the
characters that would split one.
=================
*/
static qboolean G_DummyModelOk( const char *model ) {
	if ( !model[0] || strlen( model ) >= MAX_QPATH ) {
		return qfalse;
	}
	if ( strchr( model, '\\' ) || strchr( model, '\"' ) || strchr( model, ';' ) ) {
		return qfalse;
	}
	return qtrue;
}

/*
=================
G_NearestClient

With humansOnly, skips the clients this file drives - a dummy faces whoever is
hitting it rather than the dummy beside it. Without, everything alive counts,
so two AI opponents will pick each other.
=================
*/
gentity_t *G_NearestClient( gentity_t *from, qboolean humansOnly ) {
	gentity_t	*ent;
	gentity_t	*closest;
	float		bestDistance;
	float		distance;
	vec3_t		delta;
	int			i;

	closest = NULL;
	bestDistance = 0;

	for ( i = 0 ; i < level.maxclients ; i++ ) {
		ent = &g_entities[i];

		if ( ent == from || !ent->inuse || !ent->client ) {
			continue;
		}
		if ( ent->client->pers.connected != CON_CONNECTED ) {
			continue;
		}
		if ( humansOnly && ent->client->pers.isDummy ) {
			continue;
		}
		if ( ent->client->ps.bitFlags & isDead ) {
			continue;
		}
		if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) {
			continue;
		}

		VectorSubtract( ent->client->ps.origin, from->client->ps.origin, delta );
		distance = VectorLength( delta );

		if ( !closest || distance < bestDistance ) {
			closest = ent;
			bestDistance = distance;
		}
	}

	return closest;
}

/*
=================
G_PlaceDummy

Drop the dummy in front of the player who asked for it, facing back at them.
=================
*/
static void G_PlaceDummy( gentity_t *dummy, gentity_t *owner, float distance ) {
	vec3_t		forward, start, origin, angles;
	trace_t		trace;

	VectorCopy( owner->client->ps.viewangles, angles );
	angles[PITCH] = 0;
	angles[ROLL] = 0;
	AngleVectors( angles, forward, NULL, NULL );

	// Search from chest height with a point hull: the player box sits flush on
	// the ground and starts solid on any real terrain.
	VectorCopy( owner->client->ps.origin, start );
	start[2] += owner->client->ps.viewheight;
	VectorMA( start, distance, forward, origin );

	trap_Trace( &trace, start, NULL, NULL, origin, owner->s.number, MASK_PLAYERSOLID );
	VectorCopy( trace.endpos, origin );
	if ( trace.fraction < 1.0f ) {
		// back off the wall far enough for the body to fit
		VectorMA( origin, -( dummy->r.maxs[0] + 8 ), forward, origin );
	}

	// Stand it at the player's own height, and only settle onto ground that is
	// close underneath - the point ahead is often over a canyon.
	origin[2] = owner->client->ps.origin[2];
	VectorCopy( origin, start );
	start[2] += 32;
	origin[2] -= DUMMY_DROP_MAX;
	trap_Trace( &trace, start, dummy->r.mins, dummy->r.maxs, origin, dummy->s.number, MASK_PLAYERSOLID );
	if ( trace.fraction < 1.0f ) {
		VectorCopy( trace.endpos, origin );
	} else {
		origin[2] = owner->client->ps.origin[2];
	}

	trap_UnlinkEntity( dummy );

	VectorCopy( origin, dummy->client->ps.origin );
	VectorClear( dummy->client->ps.velocity );
	dummy->client->ps.pm_time = 0;
	dummy->client->ps.pm_flags &= ~PMF_TIME_KNOCKBACK;

	// face the player who spawned it
	angles[YAW] = AngleNormalize360( angles[YAW] + 180 );
	SetClientViewAngle( dummy, angles );

	// don't interpolate in from the spawn point it was given
	dummy->client->ps.eFlags ^= EF_TELEPORT_BIT;

	VectorCopy( origin, dummy->r.currentOrigin );
	BG_PlayerStateToEntityState( &dummy->client->ps, &dummy->s, qtrue );
	trap_LinkEntity( dummy );
}

/*
=================
G_SpawnDummy

Returns the dummy, or NULL with the reason already printed to the caller.
=================
*/
static gentity_t *G_SpawnDummy( gentity_t *owner, const char *model, float distance, qboolean fights ) {
	gentity_t	*dummy;
	char		userinfo[MAX_INFO_STRING];
	char		*denied;
	int			clientNum;

	clientNum = G_DummySlot();
	if ( clientNum == -1 ) {
		trap_SendServerCommand( owner - g_entities, "print \"No free client slot left.\n\"" );
		return NULL;
	}

	Com_sprintf( userinfo, sizeof( userinfo ),
		"\\name\\%s %i\\model\\%s\\headmodel\\%s\\legsmodel\\%s"
		"\\ip\\localhost\\rate\\25000\\snaps\\20\\team\\free",
		fights ? "Fighter" : "Dummy", clientNum, model, model, model );

	trap_SetUserinfo( clientNum, userinfo );

	denied = ClientConnect( clientNum, qtrue );
	if ( denied ) {
		trap_SendServerCommand( owner - g_entities, va( "print \"Rejected: %s\n\"", denied ) );
		return NULL;
	}

	// ClientConnect clears the client, and ClientBegin runs a client frame that
	// G_RunDummy has to recognise, so the mark goes between the two.
	level.clients[clientNum].pers.isDummy = qtrue;
	level.clients[clientNum].pers.aiActive = fights;

	ClientBegin( clientNum );

	dummy = &g_entities[clientNum];
	G_PlaceDummy( dummy, owner, distance );

	return dummy;
}

/*
=================
G_RunDummy

Called from G_RunClient in place of the usual "wait for a usercmd" path.
Nothing on the network side feeds this slot, so the command it thinks with is
the one built here.
=================
*/
void G_RunDummy( gentity_t *ent ) {
	gentity_t	*player;
	vec3_t		angles, delta;

	// keep the client from being flagged as having lost its connection
	ent->client->lastCmdTime = level.time;

	if ( ent->client->pers.aiActive ) {
		G_AIThink( ent );
	} else {
		memset( &ent->client->pers.cmd, 0, sizeof( ent->client->pers.cmd ) );

		player = G_NearestClient( ent, qtrue );
		if ( player ) {
			VectorSubtract( player->client->ps.origin, ent->client->ps.origin, delta );
			vectoangles( delta, angles );
			angles[PITCH] = 0;
			angles[ROLL] = 0;
			SetClientViewAngle( ent, angles );
		}
	}

	ent->client->pers.cmd.serverTime = level.time;

	ClientThink_real( ent );
}

/*
=================
G_RemoveDummies

Returns how many were removed.
=================
*/
static int G_RemoveDummies( void ) {
	int		i;
	int		count;

	count = 0;
	for ( i = 0 ; i < level.maxclients ; i++ ) {
		if ( !level.clients[i].pers.isDummy ) {
			continue;
		}
		// game-side half of a disconnect only: trap_DropClient would be talking
		// about a client_t that was never allocated
		ClientDisconnect( i );
		level.clients[i].pers.isDummy = qfalse;
		level.clients[i].pers.aiActive = qfalse;
		count++;
	}

	return count;
}

/*
=================
G_DummyCommand

Shared by dummy and ai: [model] [distance]
=================
*/
static void G_DummyCommand( gentity_t *ent, qboolean fights ) {
	gentity_t	*dummy;
	char		model[MAX_QPATH];
	char		arg[MAX_TOKEN_CHARS];
	float		distance;

	if ( !CheatsOk( ent ) ) {
		return;
	}

	if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) {
		trap_SendServerCommand( ent - g_entities, "print \"Spectators can't spawn one.\n\"" );
		return;
	}

	if ( trap_Argc() > 1 ) {
		trap_Argv( 1, model, sizeof( model ) );
	} else {
		Q_strncpyz( model, "goku", sizeof( model ) );
	}

	if ( !G_DummyModelOk( model ) ) {
		trap_SendServerCommand( ent - g_entities, "print \"Bad model name.\n\"" );
		return;
	}

	distance = DUMMY_DISTANCE_DEFAULT;
	if ( trap_Argc() > 2 ) {
		trap_Argv( 2, arg, sizeof( arg ) );
		distance = atof( arg );
		if ( distance < DUMMY_DISTANCE_MIN ) {
			distance = DUMMY_DISTANCE_MIN;
		} else if ( distance > DUMMY_DISTANCE_MAX ) {
			distance = DUMMY_DISTANCE_MAX;
		}
	}

	dummy = G_SpawnDummy( ent, model, distance, fights );
	if ( !dummy ) {
		return;
	}

	trap_SendServerCommand( ent - g_entities,
		va( "print \"%s %i (%s) spawned at %i %i %i.\n\"",
			fights ? "Fighter" : "Dummy",
			(int)(dummy - g_entities), model,
			(int)dummy->client->ps.origin[0],
			(int)dummy->client->ps.origin[1],
			(int)dummy->client->ps.origin[2] ) );
}

/*
=================
Cmd_Dummy_f

dummy [model] [distance] - stands there and takes it
=================
*/
void Cmd_Dummy_f( gentity_t *ent ) {
	G_DummyCommand( ent, qfalse );
}

/*
=================
Cmd_AI_f

ai [model] [distance] - fights back
=================
*/
void Cmd_AI_f( gentity_t *ent ) {
	G_DummyCommand( ent, qtrue );
}

/*
=================
Cmd_DummyClear_f
=================
*/
void Cmd_DummyClear_f( gentity_t *ent ) {
	int		count;

	if ( !CheatsOk( ent ) ) {
		return;
	}

	count = G_RemoveDummies();
	trap_SendServerCommand( ent - g_entities, va( "print \"Removed %i dummies.\n\"", count ) );
}
