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
// Guard under which converting is not worth wanting. The push refuses itself
// on a bar that cannot pay its sustain, and converting locks recovery out, so
// asking below this is standing in an aura converting nothing.
#define	AI_POWERUP_FLOOR	0.50f
// Guard thin enough to break off on, and recovered enough to go back in. The
// gap between them is what stops a fighter flickering in and out of contact.
#define	AI_GUARD_LOW		0.55f
#define	AI_GUARD_READY		0.70f
#define	AI_GUARD_PATIENCE	12000
// How long it fights on after giving up on recovering the guard. Hysteresis in
// time, because the one in level cannot work: the guard is still low the moment
// patience runs out, so a level test alone re-arms on the very next frame.
#define	AI_GUARD_COMMIT		6000
// Far enough out to be worth calling an escape: a skill is charged at anything
// past AI_SKILL_RANGE, so a retreat that stops short of this one only moves
// from being punched to being shot.
#define	AI_ESCAPE_RANGE		900
// Close enough that a zanzoken is worth its fatigue. Past this the fighter is
// already out of reach for the moment and flying away is free, so teleporting
// again only spends the guard it is retreating to rebuild.
#define	AI_ESCAPE_BREAK		200
// Speed the fighter wants before a zanzoken is worth spending. The teleport has
// no push of its own - it raises the step rate and lets the velocity already in
// hand carry the fighter - so one fired from a standstill costs the fatigue and
// moves nobody. Lean away first, spend once actually travelling.
#define	AI_ESCAPE_SPEED		200
// How long after a shot before another is worth planting for. Without it a
// planted fighter never closes, the gap it needs never shuts, and both ends of
// the fight stand off trading beams - nineteen charges and one melee exchange.
#define	AI_SHOT_COOLDOWN	4000
// Gap past which boosting to close repays itself. Boost buys roughly 0.9ms per
// unit of distance - base flight is about 725 a second and boosted about 2030 -
// against five percent of the ceiling up front plus drain.
//
// It has to sit inside AI_SKILL_RANGE, because the only branch that closes on a
// rooted target runs below that: at 800 the test was above every distance that
// could reach it and boost could never fire at all, which is what every boost 0
// in a duel summary was reporting. At 250 the saving is about a fifth of a
// second against a rooted window of AI_ROOTED_WINDOW - a tenth of the punish
// time, for five percent of the guard - and sustaining it past PM_Melee's
// tmBoost > 2500 upgrades the arrival to a knockback.
#define	AI_BOOST_WORTH		250
// How long a rooted opponent can be assumed to stay rooted - its own power-up
// commitment. It is the charge budget a beam gets against someone powering up,
// which is a different question from how long they take to reach us.
#define	AI_ROOTED_WINDOW	2500
// Fatigue worth spending on leaving. PM_CheckZanzoken refuses outright under
// 15% of the ceiling, so a fighter that waits much past this cannot leave at
// all - which is the point of leaving early.
#define	AI_ESCAPE_FLOOR		0.20f
// Guard worth spending on checking a stun. The check costs 5% of the ceiling;
// under this floor the guard left is worth more than what the knockout takes,
// so the fighter keeps it and eats the hit.
#define	AI_CHECK_FLOOR		0.35f
// How long an opponent has to hold a state before the fighter acts on it. A
// server frame is 50ms, so answering a guard on the frame it goes up is a
// reaction no hand can make; four frames is about one that can. This is the
// g_aiSkill 3 value; G_AIDifficulty is what the fighter actually reads.
#define	AI_REACTION_TIME	200
// What it watches for. One bitfield rather than a timer each, because these are
// read as a single glance: a fighter that has not yet noticed the guard has not
// noticed the charge behind it either.
#define	AI_SAW_CHARGING		1
#define	AI_SAW_ALTERING		2
#define	AI_SAW_BLOCKING		4
#define	AI_SAW_MELEE		8
#define	AI_SAW_STUNNED		16
// Fastest it turns, in degrees a second. Flight sweeps the bearing to a close
// opponent quickly, so this sits well above a hand on a mouse to hold a lock at
// all - but it is a rate, so coming about still costs time an opponent can use.
// The g_aiSkill 3 value, as AI_REACTION_TIME is.
#define	AI_TURN_RATE		380
// Ends of the g_aiSkill range. Skill is clamped rather than rejected: the cvar
// is archived, so a config written by an older build can carry anything.
#define	AI_SKILL_LOWEST		1
#define	AI_SKILL_HIGHEST	5
// How long a rolled decision stands before it is rolled again. G_AIThink runs
// twenty times a second, so rolling per frame is a coin flipped twenty times a
// second: the fighter would twitch between both answers and commit to neither.
// A roll has to outlive the frame that made it to be a decision at all, and
// this is about the length of one exchange.
#define	AI_ROLL_HOLD		1200
// How far the plant threshold moves per roll, as a fraction of AI_SKILL_RANGE.
// Hovering just outside the range and reading the plant off it is the seam a
// fixed threshold leaves open; jittering moves the edge every roll so the
// distance that worked last time says nothing about this one.
#define	AI_RANGE_JITTER		0.20f
// Longest a rolled escape sits on its zanzoken before spending it. The teleport
// is the readable half of a retreat, so firing it on the first frame the speed
// is there every time is a rhythm an opponent can meet without watching.
#define	AI_ESCAPE_STALL		700
// How long a reactive guard stays up. A guard no longer refills the fatigue it
// spends, so this is a window and not a stance: long enough for PM_Melee to
// break the attacker off against it, short enough that the fighter is swinging
// again inside the same exchange rather than standing behind a bar it is paying
// for by the frame.
#define	AI_GUARD_REACT		600
// How long a perceived charge has to hold before the shot counts as coming. The
// plant itself is the punish branch's business - that is an opening to take -
// and this is the moment later at which leaving the line stops being early and
// starts being late.
#define	AI_SHOT_IMMINENT	600
// How squarely an opponent has to be facing the fighter before its charge is
// worth leaving the line of. Cosine of about twenty-five degrees: a beam pointed
// wider than that misses without anybody moving, and dodging one is fatigue
// spent on nothing.
#define	AI_LINE_OF_FIRE		0.90f
// How long a tendency read keeps its weight. Reads have to fade or the first
// exchange of a round decides the rest of it, and halving on this period is the
// whole of the decay - one shift, in the integer msec the rest of the file
// thinks in, instead of a per-frame average.
#define	AI_TENDENCY_HALFLIFE	8000
// Reads at which a habit is believed, and the ceiling the count is held at.
// Below the mark the fighter is reading noise - anyone charges once - and the
// ceiling is what keeps a habit built over a round unlearnable in less than
// AI_TENDENCY_FORGET half-lives of it not happening.
#define	AI_TENDENCY_MARK	3
#define	AI_TENDENCY_CAP		6
// Half-lives past which a read is simply gone. Also the cap on the shift that
// applies them: a shift wider than the type is undefined, and a fighter that has
// not looked at this opponent in a minute has nothing left to forget.
#define	AI_TENDENCY_FORGET	4
// What a believed read is worth, as a factor on the odds of the roll it moves.
// A factor rather than a step so the skill ordering survives it: it halves the
// wrong answer for a sharp fighter and a dull one alike, and neither ends up
// braver than the other.
#define	AI_TENDENCY_EDGE	0.5f

// What g_aiSkill scales, resolved once a frame. Three numbers and no more: how
// late the fighter reads its opponent, how fast it comes about, and how often it
// takes the worse of two live options.
typedef struct {
	int		reactionTime;
	int		turnRate;
	float	noise;
} aiDifficulty_t;

/*
=================
G_AIDifficulty

The whole g_aiSkill mapping, in one place so it can be read as one. Nothing else
in the file looks at the cvar.

Tabulated rather than interpolated because skill 3 has to land on the values the
fight was measured at, and no straight line between the ends passes through them.

Noise never reaches zero. It is what keeps a threshold from being a seam - a
fighter that always takes the optimal line is one whose next move is a lookup,
and a player who finds the lookup owns the fight - so even the sharpest skill
keeps a floor of it. Low skill spends the same mechanism on being wrong instead.
=================
*/
static void G_AIDifficulty( aiDifficulty_t *out ) {
	static const int	reaction[AI_SKILL_HIGHEST + 1] = { 0, 350, 275, AI_REACTION_TIME, 160, 120 };
	static const int	turn[AI_SKILL_HIGHEST + 1] = { 0, 240, 310, AI_TURN_RATE, 450, 520 };
	static const float	noise[AI_SKILL_HIGHEST + 1] = { 0, 0.50f, 0.35f, 0.25f, 0.16f, 0.10f };
	int					skill;

	// Clamped rather than rejected: the cvar is archived, so a config written by
	// another build can hand this anything at all.
	skill = g_aiSkill.integer;
	if ( skill < AI_SKILL_LOWEST ) {
		skill = AI_SKILL_LOWEST;
	} else if ( skill > AI_SKILL_HIGHEST ) {
		skill = AI_SKILL_HIGHEST;
	}

	out->reactionTime = reaction[skill];
	out->turnRate = turn[skill];
	out->noise = noise[skill];
}

/*
=================
G_AIRollPlant

Whether the fighter is currently willing to plant and shoot, and what gap counts
as one worth shooting into. Both are rolled together and held for AI_ROLL_HOLD.

A fighter that answers every gap with a beam is one an opponent holds at range on
purpose, and a fixed range is an edge that can be stood on. So the willingness is
a roll and the edge moves with it - but held, because a fighter re-deciding every
frame starts a charge it abandons on the next one and never fires anything.
=================
*/
static void G_AIRollPlant( gclient_t *client, float noise ) {
	if ( level.time < client->aiPlantRollAt ) {
		return;
	}

	client->aiPlantRollAt = level.time + AI_ROLL_HOLD;
	client->aiPlantRange = AI_SKILL_RANGE * ( 1.0f + crandom() * AI_RANGE_JITTER );
	client->aiWillPlant = ( random() >= noise ) ? qtrue : qfalse;
}

/*
=================
G_AIRollPunish

Whether to take the opening a rooted opponent leaves. Declining is not a loss by
itself - the pressure it falls back to still closes the gap - it is the
difference between rooting yourself being a trap that always springs and one that
has to be respected.

Cleared when nobody is rooted, so every opening is decided fresh, and re-rolled
while one lasts, so an opponent that roots itself all round is not answered by a
single verdict reached at the start of it.
=================
*/
static qboolean G_AIRollPunish( gclient_t *client, qboolean rooted, float noise ) {
	if ( !rooted ) {
		client->aiPunishRollAt = 0;
		return qfalse;
	}

	if ( level.time >= client->aiPunishRollAt ) {
		client->aiPunishRollAt = level.time + AI_ROLL_HOLD;
		client->aiWillPunish = ( random() >= noise ) ? qtrue : qfalse;
	}

	return client->aiWillPunish;
}

/*
=================
G_AIRollEscape

Whether this retreat spends a zanzoken at all, and how far into it before it
does. Held like the others, so a retreat has one shape for its duration instead
of a teleport flickering on and off inside it.
=================
*/
static void G_AIRollEscape( gclient_t *client, float noise ) {
	if ( level.time < client->aiEscapeRollAt ) {
		return;
	}

	client->aiEscapeRollAt = level.time + AI_ROLL_HOLD;
	client->aiWillZanzoken = ( random() >= noise ) ? qtrue : qfalse;
	client->aiZanzokenAt = level.time + random() * AI_ESCAPE_STALL;
}

/*
=================
G_AIRollGuard

Whether an incoming melee is answered with a guard instead of traded with.
Cleared when nothing is coming in, so every rush is decided fresh, and re-rolled
while one lasts, the way the punish roll treats a rooted opponent.

Declining is the fighter taking the exchange, which is the point: a guard that
goes up against every swing is a fighter that never throws one, and an opponent
who cannot be traded with has no reason to stop pressing.
=================
*/
static qboolean G_AIRollGuard( gclient_t *client, qboolean threatened, float noise ) {
	if ( !threatened ) {
		client->aiGuardRollAt = 0;
		return qfalse;
	}

	if ( level.time >= client->aiGuardRollAt ) {
		client->aiGuardRollAt = level.time + AI_ROLL_HOLD;
		client->aiWillGuard = ( random() >= noise ) ? qtrue : qfalse;
	}

	return client->aiWillGuard;
}

/*
=================
G_AIRollStun

Whether a guard met inside a melee is answered with the stun charge. Cleared
when no guard is up, so every raised guard is decided fresh, and re-rolled
while one lasts, the way the guard roll treats a rush.

Declining matters more here than anywhere: the charge is a full second of
standing still inside the exchange, so a stun that always answers a guard is a
certainty an opponent baits by flashing one - the wind-up is longer than the
flash.
=================
*/
static qboolean G_AIRollStun( gclient_t *client, qboolean guarded, float noise ) {
	if ( !guarded ) {
		client->aiStunRollAt = 0;
		return qfalse;
	}

	if ( level.time >= client->aiStunRollAt ) {
		client->aiStunRollAt = level.time + AI_ROLL_HOLD;
		client->aiWillStun = ( random() >= noise ) ? qtrue : qfalse;
	}

	return client->aiWillStun;
}

/*
=================
G_AIRollDodge

Whether an incoming shot is stepped out of, and which way. The side is rolled
with the willingness and held with it, because a dodge that picks a fresh
direction every frame is a fighter oscillating on the spot - which is the one
place a beam aimed at where it stands will certainly find it.
=================
*/
static void G_AIRollDodge( gclient_t *client, qboolean threatened, float noise ) {
	if ( !threatened ) {
		client->aiDodgeRollAt = 0;
		client->aiWillDodge = qfalse;
		return;
	}

	if ( level.time >= client->aiDodgeRollAt ) {
		client->aiDodgeRollAt = level.time + AI_ROLL_HOLD;
		client->aiWillDodge = ( random() >= noise ) ? qtrue : qfalse;
		client->aiDodgeLean = ( crandom() < 0 ) ? -AI_AVOID_LEAN : AI_AVOID_LEAN;
	}
}

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
G_AISkillForWindow

The heaviest skill that can actually be finished in the time available, or the
cheapest one if none fits. Time to ready is chargeTime per one percent times
the percentage the weapon needs, so the spread between skills here is enormous
- the difference between a beam that arrives and one that never leaves.
=================
*/
static int G_AISkillForWindow( playerState_t *ps, int windowMsec ) {
	g_userWeapon_t	*weapon;
	int				best = 0;
	int				bestReady = -1;
	int				ready;
	int				i;

	for ( i = 1 ; i <= MAX_PLAYERWEAPONS ; i++ ) {
		if ( !( ps->stats[stSkills] & ( 1 << i ) ) ) {
			continue;
		}
		weapon = G_FindUserWeaponData( ps->clientNum, i );
		if ( !weapon ) {
			continue;
		}
		ready = weapon->costs_chargeTime * weapon->costs_chargeReady;
		if ( ready <= windowMsec && ready > bestReady ) {
			bestReady = ready;
			best = i;
		}
	}

	return best ? best : G_AISkill( ps );
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
G_AIPlantSkill

Which skill a plant charges. The cheap one by default - a sparring partner wants
pressure rather than a five second windup - and the guard breaker against an
opponent whose habit is to block, where the windup is what buys a shot that
lands at all. Falls back when the character has nothing heavy enough.
=================
*/
static int G_AIPlantSkill( playerState_t *ps, qboolean favourBreaker ) {
	int		breaker;

	if ( favourBreaker ) {
		breaker = G_AIGuardBreaker( ps );
		if ( breaker ) {
			return breaker;
		}
	}

	return G_AISkill( ps );
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

	// Let go, which is what fires it.
	if ( ps->currentSkill[WPSTAT_BITFLAGS] & WPF_READY ) {
		return;
	}

	// The shot has already left. Pressing again here would begin a second
	// windup inside the cooldown the caller is about to price, and on a guided
	// weapon the button means something else entirely.
	if ( ps->weaponstate == WEAPON_COOLING
		|| ps->weaponstate == WEAPON_GUIDING
		|| ps->weaponstate == WEAPON_ALTGUIDING ) {
		return;
	}

	// Held every frame, deferring to nothing. This used to wait on
	// aiNextAttack, which is the melee tap pacing: G_AIAttack sets it
	// AI_ATTACK_HOLD + AI_ATTACK_PAUSE ahead so that melee reads as taps rather
	// than a hold. A fighter that tapped melee and then disengaged to plant
	// carried that timer into the windup and stopped pressing part-way through
	// it, and a charge below its ready threshold is discarded outright - so the
	// shot was thrown away by the fighter's own pacing, with nothing on the
	// line to say so. Tapping and holding are opposite intentions; they no
	// longer share a clock.
	//
	// Nothing is needed in place of it. Entry is already gated by aiShotAt,
	// which the caller sets from AI_SHOT_COOLDOWN once the shot is away.
	client->pers.cmd.buttons |= BUTTON_ATTACK;
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

	// A conversion the bar cannot sustain never finishes: the push refuses
	// itself and the aura locks recovery out, which is a fighter parked.
	if ( ps->powerLevel[plFatigue] < ps->powerLevel[plMaximum] * AI_POWERUP_FLOOR ) {
		return qfalse;
	}

	worthConverting = ps->powerLevel[plMaximum] * AI_POWERUP_POOL;

	if ( ps->powerLevel[plHealth] < ps->powerLevel[plMaximum] * AI_POWERUP_HURT
		&& ps->powerLevel[plHealthPool] > worthConverting ) {
		return qtrue;
	}

	return ps->powerLevel[plMaximumPool] > worthConverting;
}

/*
=================
G_AIMeleeThreat

Whether the opponent is the one pressing the exchange. Its melee state is what
its animation is playing, so this is the wind-up a player watches for and not a
number only the server holds.

usingMelee cannot answer this on its own: PM_SyncMelee writes it into both
fighters, so it is set on the one being hit exactly as it is on the one hitting,
and a fighter reading it would raise its guard against its own attack.
=================
*/
static qboolean G_AIMeleeThreat( playerState_t *ps ) {
	switch ( ps->stats[stMeleeState] ) {
	case stMeleeAggressing:
	case stMeleeStartAttack:
	case stMeleeUsingSpeed:
	case stMeleeStartPower:
	case stMeleeChargingPower:
	case stMeleeChargingStun:
		return qtrue;
	default:
		return qfalse;
	}
}

/*
=================
G_AIPerceive

What the fighter believes about its opponent, which is what was true
`reactionTime` ago rather than what is true now. A changed observation
restarts the clock, so a state that flickers by faster than the delay is never
noticed at all - which is the difference between a fighter that reads a fight
and one that reads memory.

Called with no target as well, so a belief cannot outlive the opponent it was
formed about.
=================
*/
static int G_AIPerceive( gclient_t *client, gentity_t *target, int reactionTime ) {
	playerState_t	*ps;
	int				facts = 0;

	if ( target ) {
		ps = &target->client->ps;

		if ( ps->weaponstate == WEAPON_CHARGING || ps->weaponstate == WEAPON_ALTCHARGING ) {
			facts |= AI_SAW_CHARGING;
		}
		if ( ps->bitFlags & usingAlter ) {
			facts |= AI_SAW_ALTERING;
		}
		if ( ps->bitFlags & usingBlock ) {
			facts |= AI_SAW_BLOCKING;
		}
		if ( G_AIMeleeThreat( ps ) ) {
			facts |= AI_SAW_MELEE;
		}
		if ( ps->stats[stMeleeState] == stMeleeUsingStun && ps->timers[tmFreeze] > 0 ) {
			facts |= AI_SAW_STUNNED;
		}
	}

	if ( facts != client->aiSeenFacts ) {
		client->aiSeenFacts = facts;
		client->aiSeenAt = level.time;
	} else if ( level.time - client->aiSeenAt >= reactionTime ) {
		client->aiPerceivedFacts = facts;
	}

	return client->aiPerceivedFacts;
}

/*
=================
G_AITally

One read added to a counter, held at AI_TENDENCY_CAP. The cap is what bounds how
long a habit outlives itself: an uncapped counter climbing all round would still
be above the mark minutes after the opponent stopped.
=================
*/
static int G_AITally( int tally, int onsets, int fact ) {
	if ( !( onsets & fact ) ) {
		return tally;
	}

	return ( tally < AI_TENDENCY_CAP ) ? tally + 1 : AI_TENDENCY_CAP;
}

/*
=================
G_AITendency

What this opponent keeps doing. Counted on the frame a perceived fact is newly
adopted rather than every frame it holds, so a single four-second charge is one
read and not eighty - a per-frame count would only measure how long each state
lasts, which is a property of the state and not of the fighter in it.

Off the perceived bitfield, so the model cannot learn from anything the fighter
did not see: a habit built out of facts it never noticed would be the fighter
reading the server rather than the fight.

Reset on a change of opponent. Spending a read taken off one fighter against the
next is worse than having no memory at all.
=================
*/
static void G_AITendency( gclient_t *client, gentity_t *target, int perceived ) {
	int		onsets;
	int		steps;
	int		of;

	of = target ? target->s.number + 1 : 0;
	if ( client->aiTendencyOf != of ) {
		client->aiTendencyOf = of;
		client->aiTendencySeen = 0;
		client->aiChargeTally = 0;
		client->aiBlockTally = 0;
		client->aiRushTally = 0;
		client->aiTendencyAt = level.time;
	}

	// Halved rather than aged read by read: one shift a half-life is the whole
	// decay, and it keeps every number here an integer count of things seen.
	steps = ( level.time - client->aiTendencyAt ) / AI_TENDENCY_HALFLIFE;
	if ( steps > 0 ) {
		client->aiTendencyAt += steps * AI_TENDENCY_HALFLIFE;
		if ( steps > AI_TENDENCY_FORGET ) {
			steps = AI_TENDENCY_FORGET;
		}
		client->aiChargeTally >>= steps;
		client->aiBlockTally >>= steps;
		client->aiRushTally >>= steps;
	}

	onsets = perceived & ~client->aiTendencySeen;
	client->aiTendencySeen = perceived;

	client->aiChargeTally = G_AITally( client->aiChargeTally, onsets, AI_SAW_CHARGING );
	client->aiBlockTally = G_AITally( client->aiBlockTally, onsets, AI_SAW_BLOCKING );
	client->aiRushTally = G_AITally( client->aiRushTally, onsets, AI_SAW_MELEE );

	// When the charge was adopted, which is what says whether the shot behind it
	// is still being loaded or is about to leave.
	if ( onsets & AI_SAW_CHARGING ) {
		client->aiChargeSeenAt = level.time;
	}
}

// What the tendencies are worth at the decisions that read them. Each is the
// noise its roll uses, so a read only ever changes how often an answer the
// fighter already had is taken - never what the answers are.
typedef struct {
	float		punishNoise;
	float		guardNoise;
	float		dodgeNoise;
	float		plantNoise;
	qboolean	favourBreaker;
} aiWeights_t;

/*
=================
G_AIWeigh

The whole of the coupling between what an opponent keeps doing and what the
fighter is willing to try, in one place so it can be read as one - the rolls
downstream take a number and know nothing about where it came from.

Everything starts at the difficulty's noise and moves by a factor of it, which
is what keeps g_aiSkill meaning the same thing under a read as without one.

The charger's read raises the punish and lowers the plant deliberately without
touching the dodge: closing on a plant and stepping out of its line are the two
answers to the same observation, and a fighter more willing to do both is one
that starts a charge it abandons to run from.
=================
*/
static void G_AIWeigh( gclient_t *client, const aiDifficulty_t *skill, aiWeights_t *out ) {
	out->punishNoise = skill->noise;
	out->guardNoise = skill->noise;
	out->dodgeNoise = skill->noise;
	out->plantNoise = skill->noise;
	out->favourBreaker = qfalse;

	// Someone who keeps planting is someone worth denying the plant to, and a
	// gap it wants to charge across is a gap to close rather than to answer in
	// kind from - the fighter with the habit wins that trade by having started
	// first.
	if ( client->aiChargeTally >= AI_TENDENCY_MARK ) {
		out->punishNoise *= AI_TENDENCY_EDGE;
		out->plantNoise = 1.0f - ( 1.0f - out->plantNoise ) * AI_TENDENCY_EDGE;
	}

	// Someone who keeps blocking is a wall to charge a swattable skill into.
	// G_ImpactUserWeapon pushes those aside on a guard, so the answer is the
	// heavy skill when there is one and pressure when there is not - either way
	// fewer plants, since the plant is what the guard is waiting for.
	if ( client->aiBlockTally >= AI_TENDENCY_MARK ) {
		out->favourBreaker = qtrue;
		out->plantNoise = 1.0f - ( 1.0f - out->plantNoise ) * AI_TENDENCY_EDGE;
	}

	// Someone who keeps rushing gives the guard something to be up for. This is
	// the read that pays for itself directly: a block that meets a rush breaks
	// it off, and one that meets nothing is fatigue burned for the window.
	if ( client->aiRushTally >= AI_TENDENCY_MARK ) {
		out->guardNoise *= AI_TENDENCY_EDGE;
	}
}

/*
=================
G_AIInLineOfFire

Whether the opponent is pointed at us closely enough for its charge to be ours
to worry about. Its view angles are what its model is facing, so this is the
same read a player takes off a fighter squaring up.
=================
*/
static qboolean G_AIInLineOfFire( gentity_t *ent, gentity_t *target ) {
	vec3_t	forward, delta;

	VectorSubtract( ent->client->ps.origin, target->client->ps.origin, delta );
	if ( VectorNormalize( delta ) == 0 ) {
		return qtrue;
	}
	AngleVectors( target->client->ps.viewangles, forward, NULL, NULL );

	return ( DotProduct( forward, delta ) > AI_LINE_OF_FIRE ) ? qtrue : qfalse;
}

/*
=================
G_AIAim

Turns toward `angles` at `turnRate` rather than arriving on them. Movement is
expressed in the view's own axes, so the same limit bends the flight path: a
fighter coming about arcs into the fight instead of cutting to it.

Through SetClientViewAngle, which is what keeps delta_angles consistent with the
zeroed command angles the slot thinks with.
=================
*/
static void G_AIAim( gentity_t *ent, vec3_t angles, int turnRate ) {
	playerState_t	*ps;
	vec3_t			aim;
	float			step, diff;
	int				i;

	ps = &ent->client->ps;
	step = turnRate * ( level.time - level.previousTime ) * 0.001f;

	for ( i = 0 ; i < 2 ; i++ ) {
		diff = AngleSubtract( angles[i], ps->viewangles[i] );
		if ( diff > step ) {
			diff = step;
		} else if ( diff < -step ) {
			diff = -step;
		}
		aim[i] = AngleNormalize180( ps->viewangles[i] + diff );
	}
	aim[ROLL] = 0;

	SetClientViewAngle( ent, aim );
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
	int				perceived;
	qboolean		threatened;
	aiDifficulty_t	skill;
	aiWeights_t		weights;

	client = ent->client;
	ps = &client->ps;
	cmd = &client->pers.cmd;

	// Everything g_aiSkill changes, resolved before anything reads it.
	G_AIDifficulty( &skill );

	// Built from nothing every frame: a button left set from the last one is a
	// button the fighter can never release, and releases are what fire a
	// charged skill and what let melee start.
	memset( cmd, 0, sizeof( *cmd ) );

	if ( ps->bitFlags & isDead ) {
		return;
	}

	// Stunned. One decision is left and it is a trade: pay 5% of the ceiling in
	// guard to check the stun off inside its window, or keep the guard and eat
	// the knockout. Below the floor the guard is worth more than the hit. This
	// sits above the recovery branch because a recovering fighter is exactly the
	// one that gets stunned - the stun lands through the guard it is holding.
	// Noticing the daze takes the same reaction every other read takes, which is
	// what makes the check window a window instead of a formality.
	if ( ps->stats[stMeleeState] == stMeleeUsingStun && ps->timers[tmFreeze] > 0 ) {
		if ( !client->aiStunnedAt ) {
			client->aiStunnedAt = level.time;
		}
		if ( level.time - client->aiStunnedAt >= skill.reactionTime
			&& ps->powerLevel[plFatigue] > ps->powerLevel[plMaximum] * AI_CHECK_FLOOR ) {
			cmd->buttons |= BUTTON_ATTACK;
		}
		return;
	}
	client->aiStunnedAt = 0;

	target = G_NearestClient( ent, qfalse );
	perceived = G_AIPerceive( client, target, skill.reactionTime );
	// Counted with no target as well, so the reads die with the opponent they
	// were taken off. Everything the tendencies are worth is resolved here,
	// beside the difficulty, and read nowhere else.
	G_AITendency( client, target, perceived );
	G_AIWeigh( client, &skill, &weights );
	if ( !target ) {
		return;
	}

	VectorSubtract( target->client->ps.origin, ps->origin, delta );
	distance = VectorLength( delta );

	vectoangles( delta, angles );
	G_AIAim( ent, angles, skill.turnRate );

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

	// A stunned opponent is one payoff standing still, and the window is
	// short. Everything the branches below would spend it on instead -
	// converting the stun's own pool income, a reactive guard against a rush
	// that is no longer coming - is worth less than the knockout; measured, a
	// fighter that landed a stun stood converting beside the daze until it
	// wore off. The forward press is the offensive input PM_Melee cashes the
	// stun in on; the held attack costs nothing while the landing recovery
	// runs out.
	if ( ( perceived & AI_SAW_STUNNED ) && ( ps->bitFlags & usingMelee ) ) {
		cmd->forwardmove = 127;
		cmd->buttons |= BUTTON_ATTACK;
		return;
	}

	// The guard decides fights, so it is what the fighter watches. It comes back
	// up at AI_GUARD_READY rather than at the threshold it dropped on, or it
	// would flicker in and out of contact, and it gives up on the attempt after
	// AI_GUARD_PATIENCE so an opponent who never lets go cannot keep it out of
	// its own fight.
	if ( client->aiRecovering ) {
		if ( ps->powerLevel[plFatigue] >= ps->powerLevel[plMaximum] * AI_GUARD_READY
			|| level.time - client->aiRecoverAt > AI_GUARD_PATIENCE ) {
			client->aiRecovering = qfalse;
			// Giving up on the guard has to mean fighting without one for a
			// while, not for a single frame. A recovering fighter sets no
			// forwardmove, and PM_Melee's start sequence sits behind
			// forwardmove, so it cannot open an exchange at all - it can only
			// be struck, which drains the guard that keeps it here. Measured:
			// nought exchanges initiated in ninety seconds against sixteen.
			client->aiFightUntil = level.time + AI_GUARD_COMMIT;
		}
	} else if ( ps->powerLevel[plFatigue] < ps->powerLevel[plMaximum] * AI_GUARD_LOW
		&& level.time >= client->aiFightUntil ) {
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
	// Zanzoken can break a melee directly, but this branch runs when the bar
	// is thin: the 3% it costs is the fatigue being conserved and the 15%
	// refusal floor is close. The guard breaks the attacker off for only what
	// it absorbs, so it goes up first and the teleport waits for open air.
	if ( client->aiRecovering ) {
		// The guard is the first half of leaving, not the consolation for
		// failing to. The attacker writes its own number into the victim's
		// lockedTarget every exchange and the lock pins movement at a flat
		// 1000 through PW_DRIFTING whatever the boost, so outrunning a melee
		// is not on the table. Raising the guard is what breaks the attacker
		// off - PM_Melee stops against a defending enemy - and only then is
		// there anything to run from.
		// usingMelee and not the lock, because the lock outlives the exchange
		// that set it. A guard no longer refills the fatigue it spends, so
		// blocking on a condition that never clears is a fighter holding a
		// guard it cannot pay for until the fight stops moving.
		if ( ps->bitFlags & usingMelee ) {
			cmd->buttons |= BUTTON_BLOCK;
			return;
		}

		// Free of it: open the gap before the guard has to do it again, then fly
		// rather than teleport. Zanzoken costs about three percent of the ceiling
		// a time and only moves the fighter a few hundred units, while the
		// opponent closes a thousand in two seconds - so spending it at every
		// range inside AI_ESCAPE_RANGE buys a gap that cannot be held and pays
		// for it over and over. Measured: seven zanzokens in one duel, about a
		// quarter of everything that guard lost, while the distance came back
		// anyway. Spend it to break contact, and coast once contact is broken.
		//
		// Whether this retreat spends one and how long into it is rolled, so the
		// escape has no rhythm to meet: an opponent that knows the teleport comes
		// on the frame the fighter reaches speed is one that never loses it.
		if ( distance < AI_ESCAPE_BREAK
			&& ps->powerLevel[plFatigue] > ps->powerLevel[plMaximum] * AI_ESCAPE_FLOOR ) {
			G_AIRollEscape( client, skill.noise );

			cmd->forwardmove = -127;
			if ( client->aiWillZanzoken && level.time >= client->aiZanzokenAt
				&& VectorLength( ps->velocity ) > AI_ESCAPE_SPEED ) {
				cmd->buttons |= BUTTON_TELEPORT;
			}
			return;
		}

		// Out of reach: rest. The full recovery bonus is gated on standing
		// near-still, so an empty cmd - which lets the fighter drift to a stop
		// - is the whole move. Powering up is not it, whatever it looks like,
		// because that branch spends fatigue on the ceiling rather than
		// returning any.
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

	// Guard because something is coming in, which is a different thing from the
	// guard the recovery branch raises: that one is a fighter buying its way out
	// of a fight it is losing, this one is a fighter still in it choosing not to
	// trade this exchange. PM_Melee breaks an attacker off against a raised
	// guard, so the block is what makes the rush cost the rusher something.
	//
	// Only on a bar that can pay for it. AI_GUARD_READY is the level the recovery
	// branch calls fought-with, and blocking below it spends the fatigue that
	// branch would rather be recovering - a guard raised on a thin bar is the
	// recovery cycle starting a frame later, with the block wasted.
	//
	// The window is latched rather than held while the threat lasts, so the
	// answer outlives the frame that decided it and the fighter is not blocking
	// and releasing in step with the attacker's animation.
	// The roll is taken before the affordability test rather than behind it, so
	// that a rush the fighter cannot afford to answer still clears the roll on
	// its way past. Behind it, a bar that dipped mid-exchange would hold the last
	// verdict until the next rush re-rolled it.
	if ( G_AIRollGuard( client,
			( ( perceived & AI_SAW_MELEE ) && distance <= AI_MELEE_RANGE ) ? qtrue : qfalse,
			weights.guardNoise )
		&& level.time >= client->aiGuardUntil
		&& ps->powerLevel[plFatigue] >= ps->powerLevel[plMaximum] * AI_GUARD_READY ) {
		client->aiGuardUntil = level.time + AI_GUARD_REACT;
	}
	if ( level.time < client->aiGuardUntil ) {
		cmd->buttons |= BUTTON_BLOCK;
		return;
	}

	// Leave the line of a charge that is about to arrive. The punish branch
	// below answers the plant - an opponent that has just rooted itself is an
	// opening - but a charge held past AI_SHOT_IMMINENT is no longer an opening,
	// it is a shot, and closing the last of the gap into one is walking into it.
	//
	// Only in the band where moving helps: inside melee range the beam is on top
	// of the fighter before the lean carries it anywhere, and past AI_ESCAPE_RANGE
	// it has room to steer around a step. In between, a lean is free - it costs
	// the closing it interrupts and nothing else.
	//
	// The zanzoken on top of it is not free, and PM_CheckZanzoken sets the terms:
	// refused outright under 15% of the ceiling, and it scales the velocity
	// already in hand rather than adding any. So it goes on a lean already
	// travelling, on a bar with more than the retreat floor left, and not while
	// melee-bound - there the drift owns the velocity, so the teleport spends
	// fatigue to be put straight back. Spent on those terms it steps out of the
	// line; spent on any other it moves the fighter nowhere, which is a worse
	// trade than taking the hit.
	threatened = ( ( perceived & AI_SAW_CHARGING )
		&& level.time - client->aiChargeSeenAt >= AI_SHOT_IMMINENT
		&& distance > AI_MELEE_RANGE && distance < AI_ESCAPE_RANGE
		&& G_AIInLineOfFire( ent, target ) ) ? qtrue : qfalse;
	G_AIRollDodge( client, threatened, weights.dodgeNoise );
	if ( client->aiWillDodge ) {
		cmd->rightmove = client->aiDodgeLean;
		if ( ps->powerLevel[plFatigue] > ps->powerLevel[plMaximum] * AI_ESCAPE_FLOOR
			&& !( ps->bitFlags & usingMelee )
			&& VectorLength( ps->velocity ) > AI_ESCAPE_SPEED ) {
			cmd->buttons |= BUTTON_TELEPORT;
		}
		return;
	}

	// Punish a fighter that has rooted itself - charging a skill, or powering up,
	// both of which stop it moving. Closing on one denies the action and lands
	// free hits on a target that gave up its footing to earn it.
	//
	// After converting, not before. usingAlter covers the routine pool
	// conversion both fighters run constantly, so punishing ahead of our own
	// economy meant neither ever converted or charged: beams went from fourteen
	// starts to none and the fight went lopsided again. This is an opportunity
	// to take when there is nothing better to do, not a standing order.
	//
	// This is what boost is for. Every other use of it competes with the guard
	// it spends, which is why no duel had ever reached for it; closing on a
	// rooted target is the one case where the fatigue buys something the fight
	// cannot get another way.
	//
	// And it is declined some of the time, which is what stops it being a
	// standing order in practice as well as in principle: a punish that always
	// arrives is a punish an opponent can bait with a charge it never meant.
	if ( G_AIRollPunish( client,
		( perceived & ( AI_SAW_CHARGING | AI_SAW_ALTERING ) ) ? qtrue : qfalse,
		weights.punishNoise ) ) {
		if ( distance <= AI_MELEE_RANGE ) {
			cmd->forwardmove = 127;
			G_AIAttack( client );
		} else if ( distance > AI_SKILL_RANGE ) {
			// Too far to reach it before it finishes, so answer in kind. The
			// budget here is how long it stays rooted, not how long it would
			// take to close, which is a longer window and buys a heavier skill.
			cmd->forwardmove = 0;
			cmd->weapon = G_AISkillForWindow( ps, AI_ROOTED_WINDOW );
			G_AICharge( client );
		} else {
			// Near enough to reach it in time. Boost only past AI_BOOST_WORTH:
			// inside that the fighter arrives before the speed repays the guard
			// it costs.
			cmd->forwardmove = 127;
			if ( distance > AI_BOOST_WORTH ) {
				cmd->buttons |= BUTTON_BOOST;
			}
		}
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

	// A raised guard inside a melee is what the stun charge is for. Skills
	// cannot charge in contact - PM_Weapon releases them while usingMelee - so
	// the held attack is the only guard-cracking verb in reach. The guard is
	// read through perception like every other fact, and the answer is rolled:
	// a stun that always follows a guard is a wind-up an opponent baits by
	// flashing one. But a wind-up past a tap's length finishes whatever the
	// guard has done since - the second it costs is already spent, and the
	// commitment is what carries it to the full charge that fires the stun.
	// The knockout after it needs nothing taught, because the closing
	// forwardmove every fighter already holds is an offensive input.
	if ( ( ps->bitFlags & usingMelee )
		&& ( G_AIRollStun( client, ( perceived & AI_SAW_BLOCKING ) ? qtrue : qfalse, skill.noise )
			|| ( ps->stats[stMeleeState] == stMeleeChargingStun && ps->timers[tmMeleeCharge] > AI_ATTACK_HOLD ) ) ) {
		cmd->buttons |= BUTTON_ATTACK;
		return;
	}

	// A raised guard stops melee outright - the engine breaks the attacker's
	// melee off against it - so answer it with something it cannot swat away.
	// A character with nothing heavy enough keeps swinging; there is nothing
	// better for it to do.
	if ( perceived & AI_SAW_BLOCKING ) {
		int	breaker = G_AIGuardBreaker( ps );

		if ( breaker ) {
			cmd->weapon = breaker;
			G_AICharge( client );
			return;
		}
	}

	// A gap is a firing window, not something to close through. Charging while
	// closing walks the fighter into melee range, and a melee calls
	// PM_WeaponRelease, which throws a charge away without firing it - measured
	// as five charges begun and none fired, by both fighters, every duel. So
	// plant and finish the shot, and stay committed once started even if the
	// gap shuts, or the charge is abandoned exactly as before.
	//
	// Standing still to charge is punishable by closing on it. That is the
	// trade the shot is worth: an opponent that has just teleported clear is
	// far enough for a beam to travel and busy travelling to answer it.
	//
	// Which gap is worth it, and whether this one is taken at all, come from a
	// held roll rather than from AI_SKILL_RANGE directly. A fixed threshold is a
	// place to stand: hover just outside it and the plant is a certainty to play
	// around. A charge already begun still finishes - the commitment is what
	// stops the charge being thrown away, and noise belongs at the decision, not
	// inside it.
	if ( ps->weaponstate == WEAPON_COOLING ) {
		client->aiShotAt = level.time + AI_SHOT_COOLDOWN;
	}
	G_AIRollPlant( client, weights.plantNoise );
	if ( ( distance > client->aiPlantRange && client->aiWillPlant
			&& level.time >= client->aiShotAt )
		|| ps->weaponstate == WEAPON_CHARGING ) {
		cmd->forwardmove = 0;
		// Chosen when the charge starts and held for the length of it. Against an
		// opponent that keeps blocking the heavy skill is the one worth planting
		// for, since a swattable one is what the guard is waiting for - but a
		// weapon changed mid-charge is a charge thrown away, so the read is taken
		// once, at the plant. The zero test covers a charge some other branch
		// began: cmd.weapon 0 is never selectable and would freeze the weapon
		// state where it stands.
		if ( ps->weaponstate != WEAPON_CHARGING || !client->aiPlantWeapon ) {
			client->aiPlantWeapon = G_AIPlantSkill( ps, weights.favourBreaker );
		}
		cmd->weapon = client->aiPlantWeapon;
		G_AICharge( client );
		return;
	}

	G_AIAttack( client );
}
