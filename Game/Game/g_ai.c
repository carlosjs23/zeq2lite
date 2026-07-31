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
// g_ai.c -- an opponent that applies pressure
//
// A sparring partner rather than a bot: it has no navigation, no awareness of
// the map and no tactics. It closes on its target, holds a lock, taps melee in
// range and charges a ki attack outside it. That is enough to move the
// resources a fight is made of - fatigue, health, the pools - which cannot be
// measured against a target that does nothing.
//
// It drives a dummy's client slot, so everything it does goes through the same
// usercmd a player's keyboard produces. There is no path here the player does
// not also take.

#include "g_local.h"

// PM_Melee engages inside 64 units; stop a little short of that so the drift
// the melee system applies does not immediately carry it back out
#define	AI_MELEE_RANGE		56
// beyond this it charges a skill instead of closing - far enough that the shot
// has somewhere to travel, near enough to stay a fight
#define	AI_SKILL_RANGE		512
#define	AI_ATTACK_HOLD		250
#define	AI_ATTACK_PAUSE		300
#define	AI_LOCK_RETRY		600
// Beyond this it holds position. With no navigation, chasing a target across a
// map is a minute of flying into scenery, which is what a respawn on the far
// side of one would otherwise start.
#define	AI_LEASH_RANGE		4000
// Pool worth breaking off to convert, as a fraction of the ceiling, and how
// long it commits to the conversion once it starts.
#define	AI_POWERUP_POOL		0.15f
#define	AI_POWERUP_TIME		2500
// Health below this fraction of the ceiling is worth spending a health pool on
#define	AI_POWERUP_HURT		0.85f

/*
=================
G_AISkill

Lowest skill the character has. The low slots are the cheap fast-charging ones,
which is what a sparring partner wants: pressure, not a five second windup.
=================
*/
static int G_AISkill( playerState_t *ps ) {
	int		i;

	for ( i = 1 ; i <= MAX_PLAYERWEAPONS ; i++ ) {
		if ( ps->stats[stSkills] & ( 1 << i ) ) {
			return i;
		}
	}

	return ps->weapon;
}

/*
=================
G_AIAttack

Presses attack in bursts. Melee only engages between taps - holding the button
puts the weapon into WEAPON_CHARGING, and PM_Melee returns while charging - so
a held button would be a fighter that never throws a punch.
=================
*/
static void G_AIAttack( gclient_t *client ) {
	if ( level.time < client->aiAttackUntil ) {
		client->pers.cmd.buttons |= BUTTON_ATTACK;
		return;
	}

	if ( level.time >= client->aiNextAttack ) {
		client->aiAttackUntil = level.time + AI_ATTACK_HOLD;
		client->aiNextAttack = client->aiAttackUntil + AI_ATTACK_PAUSE;
		client->pers.cmd.buttons |= BUTTON_ATTACK;
	}
}

/*
=================
G_AICharge

Holds attack until the skill reports itself ready, then lets go: a charged
weapon fires on release, and WPF_READY is the same flag the HUD colours the
charge gauge with.
=================
*/
static void G_AICharge( gclient_t *client ) {
	playerState_t	*ps = &client->ps;

	if ( ps->currentSkill[WPSTAT_BITFLAGS] & WPF_READY ) {
		client->aiNextAttack = level.time + AI_ATTACK_PAUSE;
		return;
	}

	if ( level.time >= client->aiNextAttack ) {
		client->pers.cmd.buttons |= BUTTON_ATTACK;
	}
}

/*
=================
G_AIShouldConvert

A pool is worth two different things: the maximum pool raises the ceiling, the
health pool heals. Reading only the first is how a fighter dies rich.
=================
*/
static qboolean G_AIShouldConvert( playerState_t *ps ) {
	int		worthConverting;

	worthConverting = ps->powerLevel[plMaximum] * AI_POWERUP_POOL;

	if ( ps->powerLevel[plHealth] < ps->powerLevel[plMaximum] * AI_POWERUP_HURT
		&& ps->powerLevel[plHealthPool] > worthConverting ) {
		return qtrue;
	}

	return ps->powerLevel[plMaximumPool] > worthConverting;
}

/*
=================
G_AIThink

Builds the usercmd the slot will think with this server frame.
=================
*/
void G_AIThink( gentity_t *ent ) {
	gclient_t		*client;
	playerState_t	*ps;
	usercmd_t		*cmd;
	gentity_t		*target;
	vec3_t			delta, angles;
	float			distance;

	client = ent->client;
	ps = &client->ps;
	cmd = &client->pers.cmd;

	// Built from nothing every frame: a button left set from the last one is a
	// button the fighter can never release, and releases are what fire a
	// charged skill and what let melee start.
	memset( cmd, 0, sizeof( *cmd ) );

	if ( ps->bitFlags & isDead ) {
		return;
	}

	target = G_NearestClient( ent, qfalse );
	if ( !target ) {
		return;
	}

	VectorSubtract( target->client->ps.origin, ps->origin, delta );
	distance = VectorLength( delta );

	vectoangles( delta, angles );
	angles[ROLL] = 0;
	SetClientViewAngle( ent, angles );

	if ( distance > AI_LEASH_RANGE ) {
		return;
	}

	// Break off to convert. Both fighters earn pool in an exchange - dealing
	// damage pays in as well as taking it - and converting it is the half of
	// the loop that turns a beating into a higher ceiling and a full bar.
	//
	// The direction key is what the power-up modifier reads: right is raise,
	// and forward would ask to transform instead.
	if ( level.time < client->aiPowerUpUntil || G_AIShouldConvert( ps ) ) {
		if ( level.time >= client->aiPowerUpUntil ) {
			client->aiPowerUpUntil = level.time + AI_POWERUP_TIME;
		}
		cmd->buttons |= BUTTON_POWERLEVEL;
		cmd->rightmove = 127;
		return;
	}

	// Melee needs a lock, and the lock is also what the camera and the HUD read
	// to present a fight. The button toggles, so it is only ever tapped while
	// there is nothing locked.
	if ( !ps->lockedTarget && level.time >= client->aiNextLock ) {
		cmd->buttons |= BUTTON_GESTURE;
		client->aiNextLock = level.time + AI_LOCK_RETRY;
	}

	// Always closing. PM_Melee reads stMeleeAggressing off forwardmove, so a
	// fighter that stops on arrival stands in front of its target and never
	// throws anything - and one that stops to charge is thrown clear by the
	// first melee exchange and spends the rest of the round charging at a
	// target it is drifting away from.
	cmd->forwardmove = 127;

	if ( distance > AI_SKILL_RANGE ) {
		// Too far to touch: charge a skill on the way in. Against someone
		// standing their ground it arrives before the shot is ready and throws
		// hands instead; against someone running it gets the shot off.
		cmd->weapon = G_AISkill( ps );
		G_AICharge( client );
		return;
	}

	G_AIAttack( client );
}
