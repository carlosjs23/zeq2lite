/*
G_RemoveUserWeapon tear-down.

isStruggling is a two-party flag: every place the missile code raises it, it
raises it on *both* the blocking player and the missile's owner, and every
place it drops it, it drops both (g_usermissile.c:407/408, 1244/1245,
1251/1252, 1255/1256). G_RemoveUserWeapon is the tear-down counterpart - if it
fails to clear the flag the player stays permanently "struggling", which
bg_pmove.c reads as a hard block on flight, boost, melee, attacking and ki
charging.

These tests pin that contract from the outside: they set the flag, remove the
weapon, and assert the flag is gone. They never name the pointer expression the
implementation uses to get there, so they stay honest if it is rewritten.
*/

#include <criterion/criterion.h>

#include "g_local.h"

int  game_stubs_freed_count(void);
void game_stubs_reset(void);

void G_LocationImpact(vec3_t point, gentity_t *targ, gentity_t *attacker);
void G_ImpactUserWeapon(gentity_t *self, trace_t *trace);
void G_UserWeaponDamage(gentity_t *target, gentity_t *inflictor, gentity_t *attacker,
                        vec3_t dir, vec3_t point, int damage, int dflags, int knockback);

#define OWNER_NUM   0
#define BLOCKER_NUM 1
#define MISSILE_NUM (MAX_CLIENTS + 4)

static gclient_t test_clients[MAX_CLIENTS];

/* A player entity: server entities only ever carry a client pointer for real
   players (g_client.c is the only place that assigns one), which is exactly the
   asymmetry these tests exist to cover. */
static gentity_t *make_player(int num) {
	gentity_t *ent = &g_entities[num];
	memset(ent, 0, sizeof(*ent));
	memset(&test_clients[num], 0, sizeof(test_clients[num]));
	ent->s.eType = ET_PLAYER;
	ent->s.number = num;
	ent->client = &test_clients[num];
	ent->inuse = qtrue;
	return ent;
}

/* A missile. Note what is *not* set: a missile has no client, and no enemy
   until somebody blocks it. */
static gentity_t *make_missile(gentity_t *owner) {
	gentity_t *ent = &g_entities[MISSILE_NUM];
	memset(ent, 0, sizeof(*ent));
	ent->s.eType = ET_MISSILE;
	ent->s.number = MISSILE_NUM;
	ent->s.clientNum = owner->s.number;
	ent->parent = owner;
	ent->inuse = qtrue;
	return ent;
}

static void setup(void) {
	game_stubs_reset();
	memset(test_clients, 0, sizeof(test_clients));
}

/*
The reported crash: a beam fired into empty space is never blocked, so it still
has no enemy when it drains to nothing and is removed.
*/
Test(usermissile, removing_an_unblocked_weapon_survives) {
	gentity_t *owner, *missile;

	setup();
	owner = make_player(OWNER_NUM);
	missile = make_missile(owner);
	cr_assert_null(missile->enemy, "an unblocked missile has no enemy");

	G_RemoveUserWeapon(missile);

	cr_assert_eq(game_stubs_freed_count(), 1, "the missile should have been freed");
}

/*
The owner is flagged as struggling by Think_NormalMissileStruggle; removing the
weapon has to unflag them or they are frozen for the rest of the round.
*/
Test(usermissile, removal_clears_struggle_on_the_owner) {
	gentity_t *owner, *blocker, *missile;

	setup();
	owner = make_player(OWNER_NUM);
	blocker = make_player(BLOCKER_NUM);
	missile = make_missile(owner);
	missile->enemy = blocker;

	owner->client->ps.bitFlags |= isStruggling;
	blocker->client->ps.bitFlags |= isStruggling;

	G_RemoveUserWeapon(missile);

	cr_assert_eq(owner->client->ps.bitFlags & isStruggling, 0,
	             "the player who fired must stop struggling when the weapon goes away");
}

Test(usermissile, removal_clears_struggle_on_the_blocker) {
	gentity_t *owner, *blocker, *missile;

	setup();
	owner = make_player(OWNER_NUM);
	blocker = make_player(BLOCKER_NUM);
	missile = make_missile(owner);
	missile->enemy = blocker;

	owner->client->ps.bitFlags |= isStruggling;
	blocker->client->ps.bitFlags |= isStruggling;

	G_RemoveUserWeapon(missile);

	cr_assert_eq(blocker->client->ps.bitFlags & isStruggling, 0,
	             "the player who blocked must stop struggling when the weapon goes away");
}

/*
While blocking, the blocker's lockedTarget is pointed at the missile entity
(g_usermissile.c:1353). That is an entity number above the client range, and it
has to be released when the missile dies or the blocker keeps a lock on a
freed entity.
*/
Test(usermissile, removal_releases_a_lock_held_on_the_missile) {
	gentity_t *owner, *blocker, *missile;

	setup();
	owner = make_player(OWNER_NUM);
	blocker = make_player(BLOCKER_NUM);
	missile = make_missile(owner);
	missile->enemy = blocker;
	blocker->client->ps.lockedTarget = MISSILE_NUM;

	G_RemoveUserWeapon(missile);

	cr_assert_eq(blocker->client->ps.lockedTarget, 0,
	             "a lock on the removed missile must be released");
}

/*
A missile whose owner is gone is the ordinary case for a disconnect mid-flight.
Tear-down must not depend on the owner still being resolvable.
*/
Test(usermissile, removing_a_weapon_with_no_owner_survives) {
	gentity_t *missile;

	setup();
	missile = &g_entities[MISSILE_NUM];
	memset(missile, 0, sizeof(*missile));
	missile->s.eType = ET_MISSILE;
	missile->s.number = MISSILE_NUM;
	missile->inuse = qtrue;

	G_RemoveUserWeapon(missile);

	cr_assert_eq(game_stubs_freed_count(), 1, "the missile should have been freed");
}

/*
G_LocationImpact works out which side of a player was hit, and the block check
in G_ImpactUserWeapon reads the answer (g_usermissile.c:1341) - a beam is only
blockable when it lands on LOCATION_FRONT. So the classification is behaviour
worth pinning, not just a crash to avoid.

Its caller passes &g_entities[trace->entityNum], which is whatever the missile
touched. Most shots hit map geometry, and world entities have no client.
*/
Test(usermissile, impact_on_world_geometry_survives) {
	gentity_t *world;
	vec3_t point = { -100.0f, 0.0f, 0.0f };

	setup();
	world = &g_entities[ENTITYNUM_WORLD];
	memset(world, 0, sizeof(*world));
	world->s.eType = ET_GENERAL;
	cr_assert_null(world->client, "map geometry is not a player");

	G_LocationImpact(point, world, make_player(OWNER_NUM));

	cr_assert_null(world->client, "nothing to record on a non-player target");
}

/* A player facing +X, hit from behind them. */
Test(usermissile, impact_from_behind_a_player_is_recorded_as_a_back_hit) {
	gentity_t *targ, *attacker;
	vec3_t point = { -100.0f, 0.0f, 0.0f };

	setup();
	targ = make_player(BLOCKER_NUM);
	attacker = make_player(OWNER_NUM);
	VectorClear(targ->r.currentOrigin);
	targ->client->ps.viewangles[YAW] = 0.0f;

	G_LocationImpact(point, targ, attacker);

	cr_assert_eq(targ->client->lasthurt_location, LOCATION_BACK,
	             "a hit from behind should register as a back hit");
}

/* The same player hit from in front - the case the block check depends on. */
Test(usermissile, impact_in_front_of_a_player_is_recorded_as_a_front_hit) {
	gentity_t *targ, *attacker;
	vec3_t point = { 100.0f, 0.0f, 0.0f };

	setup();
	targ = make_player(BLOCKER_NUM);
	attacker = make_player(OWNER_NUM);
	VectorClear(targ->r.currentOrigin);
	targ->client->ps.viewangles[YAW] = 0.0f;

	G_LocationImpact(point, targ, attacker);

	cr_assert_eq(targ->client->lasthurt_location, LOCATION_FRONT,
	             "a hit from the front should register as a front hit - blocking depends on it");
}

/*
G_UserWeaponDamage substitutes the world entity when it has no attacker
(g_usermissile.c:461), and the world has no client - so the damage bookkeeping
below it cannot assume one. This is the shape of falling damage, map hazards,
and a shot whose owner disconnected mid-flight.
*/
Test(usermissile, damage_from_the_world_survives) {
	gentity_t *targ, *world;
	vec3_t dir = { 1.0f, 0.0f, 0.0f };
	vec3_t point = { 0.0f, 0.0f, 0.0f };

	setup();
	targ = make_player(BLOCKER_NUM);
	targ->takedamage = qtrue;
	world = &g_entities[ENTITYNUM_WORLD];
	memset(world, 0, sizeof(*world));
	world->s.eType = ET_GENERAL;
	world->s.number = ENTITYNUM_WORLD;

	G_UserWeaponDamage(targ, world, world, dir, point, 100, 0, 0);

	cr_assert_gt(targ->client->ps.powerLevel[plDamageFromEnergy], 0,
	             "the target should still take the damage");
}

/*
Two beams meeting head-on. The struggle is between the players who fired them,
so the check belongs to the owner - the beam entity itself has no client.
*/
Test(usermissile, beam_meeting_beam_survives) {
	gentity_t *owner, *rival, *mine, *theirs;
	trace_t trace;

	setup();
	owner = make_player(OWNER_NUM);
	rival = make_player(BLOCKER_NUM);

	mine = make_missile(owner);
	mine->s.eType = ET_BEAMHEAD;

	theirs = &g_entities[MISSILE_NUM + 1];
	memset(theirs, 0, sizeof(*theirs));
	theirs->s.eType = ET_BEAMHEAD;
	theirs->s.number = MISSILE_NUM + 1;
	theirs->parent = rival;
	theirs->inuse = qtrue;

	memset(&trace, 0, sizeof(trace));
	trace.entityNum = theirs->s.number;

	G_ImpactUserWeapon(mine, &trace);

	cr_assert_eq(mine->enemy, theirs, "the two beams should be locked together");
}

/*
A guided attack releases its owner's weapon state on removal. The owner is
reached through s.clientNum, which is stale once that player has gone.
*/
Test(usermissile, removing_a_guided_weapon_whose_owner_left_survives) {
	gentity_t *missile;

	setup();
	missile = &g_entities[MISSILE_NUM];
	memset(missile, 0, sizeof(*missile));
	missile->s.eType = ET_MISSILE;
	missile->s.number = MISSILE_NUM;
	missile->s.clientNum = OWNER_NUM;   /* slot is empty - no client attached */
	missile->guided = qtrue;
	missile->inuse = qtrue;

	G_RemoveUserWeapon(missile);

	cr_assert_eq(game_stubs_freed_count(), 1, "the missile should have been freed");
}
