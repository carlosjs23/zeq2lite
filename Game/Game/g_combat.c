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
// g_combat.c

#include "g_local.h"

// How long an attacker stays on the hook for a death. Long enough that the hit
// that started a fall still counts, short enough that a fighter who broke off
// a minute ago is not credited with someone else's kill.
#define	KILL_CREDIT_WINDOW	10000
// Share of the dead fighter's ceiling that goes into each of the killer's pools.
// The pools are what growth is bought with, so this is what a kill is worth:
// against a 20000 ceiling, 2500 into each - roughly two solid exchanges' worth
// of zenkai, arriving all at once for finishing the job.
#define	KILL_POOL_SHARE		8

/*
==============
G_RecordAttacker

Notes who last damaged a client, for the kill credit paid out on their death.
==============
*/
void G_RecordAttacker( gclient_t *victim, int attackerNum ) {
	if ( !victim ) {
		return;
	}
	if ( attackerNum < 0 || attackerNum >= level.maxclients ) {
		return;
	}
	if ( victim == &level.clients[attackerNum] ) {
		return;
	}
	victim->lastDamagedBy = attackerNum;
	victim->lastDamagedAt = level.time;
}

/*
==============
G_AwardKill

Pays the fighter who put this one down: a point of score, and a share of the
ceiling it took to do it into both pools. Growth costs pool and nothing else
handed pool out for winning, so a fight that ended was worth exactly as much as
a fight that never started.
==============
*/
void G_AwardKill( gentity_t *victim ) {
	gclient_t	*killer;
	int			award;
	int			limit;
	if ( !victim->client ) {
		return;
	}
	if ( victim->client->lastDamagedBy < 0 || victim->client->lastDamagedBy >= level.maxclients ) {
		return;
	}
	if ( level.time - victim->client->lastDamagedAt > KILL_CREDIT_WINDOW ) {
		return;
	}
	killer = &level.clients[victim->client->lastDamagedBy];
	victim->client->lastDamagedBy = -1;
	if ( killer == victim->client || killer->pers.connected != CON_CONNECTED ) {
		return;
	}
	if ( killer->sess.sessionTeam == TEAM_SPECTATOR ) {
		return;
	}
	killer->ps.persistant[PERS_SCORE] += 1;
	killer->lastkilled_client = victim->client->ps.clientNum;
	killer->lastKillTime = level.time;
	// plLimit is the network short the pools travel in; nothing may exceed it.
	limit = killer->ps.powerLevel[plLimit];
	award = victim->client->ps.powerLevel[plMaximum] / KILL_POOL_SHARE;
	killer->ps.powerLevel[plHealthPool] += award;
	killer->ps.powerLevel[plMaximumPool] += award;
	if ( killer->ps.powerLevel[plHealthPool] > limit ) { killer->ps.powerLevel[plHealthPool] = limit; }
	if ( killer->ps.powerLevel[plMaximumPool] > limit ) { killer->ps.powerLevel[plMaximumPool] = limit; }
}

int RaySphereIntersections( vec3_t origin, float radius, vec3_t point, vec3_t dir, vec3_t intersections[2] ) {
	float b, c, d, t;
	VectorNormalize(dir);
	b = 2 * (dir[0] * (point[0] - origin[0]) + dir[1] * (point[1] - origin[1]) + dir[2] * (point[2] - origin[2]));
	c = (point[0] - origin[0]) * (point[0] - origin[0]) +
		(point[1] - origin[1]) * (point[1] - origin[1]) +
		(point[2] - origin[2]) * (point[2] - origin[2]) -
		radius * radius;

	d = b * b - 4 * c;
	if (d > 0) {
		t = (- b + sqrt(d)) / 2;
		VectorMA(point, t, dir, intersections[0]);
		t = (- b - sqrt(d)) / 2;
		VectorMA(point, t, dir, intersections[1]);
		return 2;
	}
	else if (d == 0) {
		t = (- b ) / 2;
		VectorMA(point, t, dir, intersections[0]);
		return 1;
	}
	return 0;
}
qboolean CanDamage (gentity_t *targ, vec3_t origin) {
	vec3_t	dest;
	trace_t	tr;
	vec3_t	midpoint;
	VectorAdd (targ->r.absmin, targ->r.absmax, midpoint);
	VectorScale (midpoint, 0.5, midpoint);
	VectorCopy (midpoint, dest);
	trap_Trace ( &tr, origin, vec3_origin, vec3_origin, dest, ENTITYNUM_NONE, MASK_SOLID);
	if (tr.fraction == 1.0 || tr.entityNum == targ->s.number)
		return qtrue;
	VectorCopy (midpoint, dest);
	dest[0] += 15.0;
	dest[1] += 15.0;
	trap_Trace ( &tr, origin, vec3_origin, vec3_origin, dest, ENTITYNUM_NONE, MASK_SOLID);
	if (tr.fraction == 1.0)
		return qtrue;
	VectorCopy (midpoint, dest);
	dest[0] += 15.0;
	dest[1] -= 15.0;
	trap_Trace ( &tr, origin, vec3_origin, vec3_origin, dest, ENTITYNUM_NONE, MASK_SOLID);
	if (tr.fraction == 1.0)
		return qtrue;
	VectorCopy (midpoint, dest);
	dest[0] -= 15.0;
	dest[1] += 15.0;
	trap_Trace ( &tr, origin, vec3_origin, vec3_origin, dest, ENTITYNUM_NONE, MASK_SOLID);
	if (tr.fraction == 1.0)
		return qtrue;
	VectorCopy (midpoint, dest);
	dest[0] -= 15.0;
	dest[1] -= 15.0;
	trap_Trace ( &tr, origin, vec3_origin, vec3_origin, dest, ENTITYNUM_NONE, MASK_SOLID);
	if (tr.fraction == 1.0)
		return qtrue;
	return qfalse;
}
