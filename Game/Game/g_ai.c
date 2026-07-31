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
// Beyond this it stops chasing and re-places itself in front of its target
// after AI_LEASH_PATIENCE. A respawn puts a fighter most of a map away, and at
// flight speed that is minutes of travel to reach a fight that has already
// been decided.
#define	AI_LEASH_RANGE		4000
#define	AI_LEASH_PATIENCE	3000
#define	AI_LEASH_RETURN		400
// How far ahead it looks for something to fly around, and how hard it leans
// out of the way when it finds it. The short probe is what it uses in melee
// range, where leaning walks it out of the exchange and is only worth doing
// for something it is already up against.
#define	AI_AVOID_LOOKAHEAD	320
#define	AI_AVOID_CONTACT	48
#define	AI_AVOID_LEAN		127
// Pool worth breaking off to convert, as a fraction of the ceiling, and how
// long it commits to the conversion once it starts.
#define	AI_POWERUP_POOL		0.15f
#define	AI_POWERUP_TIME		2500
// Health below this fraction of the ceiling is worth spending a health pool on
#define	AI_POWERUP_HURT		0.85f
// Guard thin enough to break off on, and recovered enough to go back in. The
// gap between them is what stops a fighter flickering in and out of contact.
#define	AI_GUARD_LOW		0.55f
#define	AI_GUARD_READY		0.70f
#define	AI_GUARD_PATIENCE	12000
// Far enough out to be worth calling an escape: a skill is charged at anything
// past AI_SKILL_RANGE, so a retreat that stops short of this one only moves
// from being punched to being shot.
#define	AI_ESCAPE_RANGE		900
// Fatigue worth spending on leaving. PM_CheckZanzoken refuses outright under
// 15% of the ceiling, so a fighter that waits much past this cannot leave at
// all - which is the point of leaving early.
#define	AI_ESCAPE_FLOOR		0.20f

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
G_AIGuardBreaker

A skill a raised guard cannot swat aside, or 0 if the character has none.
G_ImpactUserWeapon pushes a swattable attack away when it lands on a block and
drives a non-swattable one into a power struggle, so the heavy skills are the
only ones a guard has to answer - which is what the swat flag in the .phys
scripts is marking.
=================
*/
static int G_AIGuardBreaker( playerState_t *ps ) {
	g_userWeapon_t	*weapon;
	int				i;

	for ( i = 1 ; i <= MAX_PLAYERWEAPONS ; i++ ) {
		if ( !( ps->stats[stSkills] & ( 1 << i ) ) ) {
			continue;
		}

		weapon = G_FindUserWeaponData( ps->clientNum, i );
		if ( weapon && !weapon->physics_swat ) {
			return i;
		}
	}

	return 0;
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
G_AIPathClear

Whether the fighter can travel `distance` along `dir` without hitting anything.
=================
*/
static qboolean G_AIPathClear( gentity_t *ent, vec3_t dir, float distance ) {
	trace_t		trace;
	vec3_t		end;

	VectorMA( ent->client->ps.origin, distance, dir, end );
	// Geometry only. MASK_PLAYERSOLID includes CONTENTS_BODY, and the body
	// straight ahead of a fighter closing on someone is the someone: with it in
	// the mask the avoidance steers around its own target and the fight stops.
	trap_Trace( &trace, ent->client->ps.origin, ent->r.mins, ent->r.maxs, end,
		ent->s.number, CONTENTS_SOLID );

	return trace.fraction == 1.0f;
}

/*
=================
G_AIAvoid

Steers around whatever is in the way without taking its eyes off the target.
Movement is expressed in the view's own axes, so leaning on the strafe and lift
components slides the fighter past an obstacle while it keeps facing - and
therefore keeps aiming at - whoever it is fighting.

This is what a flying duellist needs instead of a navigation mesh: the target
is in sight by definition, there is nothing to search for, and the only
question is which way around the rock in front of it.
=================
*/
static void G_AIAvoid( gentity_t *ent, usercmd_t *cmd, float lookahead ) {
	vec3_t		forward, right, up, probe;

	AngleVectors( ent->client->ps.viewangles, forward, right, up );

	if ( G_AIPathClear( ent, forward, lookahead ) ) {
		return;
	}

	// Up first: these are fighters in open sky, and the way past a canyon wall
	// or a rock is almost always over it.
	if ( G_AIPathClear( ent, up, lookahead ) ) {
		cmd->upmove = AI_AVOID_LEAN;
		return;
	}

	VectorCopy( right, probe );
	if ( G_AIPathClear( ent, probe, lookahead ) ) {
		cmd->rightmove = AI_AVOID_LEAN;
		return;
	}

	VectorNegate( right, probe );
	if ( G_AIPathClear( ent, probe, lookahead ) ) {
		cmd->rightmove = -AI_AVOID_LEAN;
		return;
	}

	// Boxed in on every axis tried: back out rather than grind forwards.
	cmd->forwardmove = -AI_AVOID_LEAN;
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

	// A fight only ends when someone dies, and the winner is left staring at a
	// respawn on the far side of the map. Rather than fly there or stand
	// forever, the sparring partner comes back to the fight.
	if ( distance > AI_LEASH_RANGE ) {
		if ( !client->aiLeashedAt ) {
			client->aiLeashedAt = level.time;
		} else if ( level.time - client->aiLeashedAt > AI_LEASH_PATIENCE ) {
			client->aiLeashedAt = 0;
			G_PlaceDummy( ent, target, AI_LEASH_RETURN );
		}
		return;
	}
	client->aiLeashedAt = 0;

	// The guard decides fights, so it is what the fighter watches. It comes back
	// up at AI_GUARD_READY rather than at the threshold it dropped on, or it
	// would flicker in and out of contact, and it gives up on the attempt after
	// AI_GUARD_PATIENCE so an opponent who never lets go cannot keep it out of
	// its own fight.
	if ( client->aiRecovering ) {
		if ( ps->powerLevel[plFatigue] >= ps->powerLevel[plMaximum] * AI_GUARD_READY
			|| level.time - client->aiRecoverAt > AI_GUARD_PATIENCE ) {
			client->aiRecovering = qfalse;
		}
	} else if ( ps->powerLevel[plFatigue] < ps->powerLevel[plMaximum] * AI_GUARD_LOW ) {
		client->aiRecovering = qtrue;
		client->aiRecoverAt = level.time;
	}

	// Leaving, in the order the fight allows it.
	//
	// Base speed is (fatigue / 72.81) + speed * 450, so a spent fighter is
	// slower than the one chasing it and cannot simply back away. The two
	// verbs that change that both cost the fatigue being conserved, which is
	// the trade: boost lights for 5% of the ceiling and burns a quarter of
	// that a second, buying x4.2 for the first second and x2.8 after;
	// zanzoken is a flat 3% and breaks the lock and the melee outright, but
	// scales the velocity already in hand, so it is worth nothing standing
	// still.
	//
	// Zanzoken is refused while usingMelee, so boost is what breaks contact
	// and zanzoken is what opens the gap once out. A guard goes up when it can
	// afford neither.
	if ( client->aiRecovering ) {
		// The guard is the first half of leaving, not the consolation for
		// failing to. A fighter in a melee cannot get out of one on its own:
		// the attacker writes its own number into the victim's lockedTarget
		// every exchange, the lock pins movement at a flat 1000 through
		// PW_DRIFTING whatever the boost, and zanzoken is refused outright
		// while usingMelee. Raising the guard is what breaks the attacker off
		// - PM_Melee stops against a defending enemy - and only then is there
		// anything to run from.
		// usingMelee and not the lock, because the lock outlives the exchange
		// that set it. A guard no longer refills the fatigue it spends, so
		// blocking on a condition that never clears is a fighter holding a
		// guard it cannot pay for until the fight stops moving.
		if ( ps->bitFlags & usingMelee ) {
			cmd->buttons |= BUTTON_BLOCK;
			return;
		}

		// Free of it: open the gap before the guard has to do it again. Past
		// AI_ESCAPE_RANGE there is nothing left to spend fatigue on. Zanzoken
		// is also what drops the lock, so this is the step that turns being
		// disengaged into being genuinely out of the fight.
		if ( distance < AI_ESCAPE_RANGE
			&& ps->powerLevel[plFatigue] > ps->powerLevel[plMaximum] * AI_ESCAPE_FLOOR ) {
			cmd->forwardmove = -127;
			cmd->buttons |= BUTTON_TELEPORT;
			return;
		}

		// Out of reach: rest. The recovery bonus is for a fighter giving no
		// input at all, so an empty cmd is the whole move - and powering up is
		// not it, whatever it looks like, because that branch spends fatigue
		// on the ceiling rather than returning any.
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
	// Keep a selectable weapon in the command every frame. PM_Weapon returns on
	// its second line when cmd.weapon is not selectable, and weapon 0 never is,
	// so leaving it at zero freezes the weapon state wherever it stood - a
	// fighter that closed while charging stayed WEAPON_CHARGING for the rest of
	// the round, and PM_Melee refuses to start while charging. Ticking it lets
	// the charge lapse on the released button, which is what frees the melee.
	cmd->weapon = ps->weapon;

	cmd->forwardmove = 127;
	// In melee range the probe shrinks rather than switching off. Leaning at
	// that distance walks the fighter out of the exchange, so it is only worth
	// doing for something it is already against - but switching off entirely
	// left two fighters holding full forward into a canyon wall with nothing
	// in the loop able to notice, which is a stuck fight rather than a close
	// one.
	G_AIAvoid( ent, cmd, distance > AI_MELEE_RANGE ? AI_AVOID_LOOKAHEAD : AI_AVOID_CONTACT );

	// A raised guard stops melee outright - the engine breaks the attacker's
	// melee off against it - so answer it with something it cannot swat away.
	// A character with nothing heavy enough keeps swinging; there is nothing
	// better for it to do.
	if ( target->client->ps.bitFlags & usingBlock ) {
		int	breaker = G_AIGuardBreaker( ps );

		if ( breaker ) {
			cmd->weapon = breaker;
			G_AICharge( client );
			return;
		}
	}

	if ( distance > AI_SKILL_RANGE ) {
		// Too far to touch: charge a skill on the way in. Against someone
		// running this gets the shot off. Against someone standing their
		// ground it arrives still charging, which is why the weapon goes away
		// below rather than the charge being relied on to lapse.
		cmd->weapon = G_AISkill( ps );
		G_AICharge( client );
		return;
	}

	G_AIAttack( client );
}
