/*
cg_players.c keeps a shadow copy of every player's pose, because the real one in
cg_entities gets its lerp frames zeroed whenever an entity re-enters the PVS
(see CG_ResetPlayerEntity, called from CG_ResetEntity in cg_snapshot.c).

The copy is written by entity number:

    playerInfoDuplicate[cent->currentState.number] = cent->pe;

and read back by client number:

    pe = &playerInfoDuplicate[clientNum];

Those two indices are the same for a living player and different for a corpse -
which the file itself says twice, once in a comment at the read sites and once
as the definition of onBodyQue:

    onBodyQue = (cent->currentState.number != cent->currentState.clientNum);

So a corpse stores its pose in one slot and reads another client's out of a
different one. Anything positioned off a corpse's tags - beam attacks, particle
systems, the weapon model - follows the living player around instead.

The lever that makes this observable: CG_Player forces tier 0 for a corpse but
uses the entity's own tier otherwise, so one clientinfo yields two different
head models. The suite asserts on which model reaches trap_R_LerpTag, i.e. on
which slot was actually read, rather than on the index expression itself.
*/

#include <criterion/criterion.h>

#include "cg_local.h"
#include "fake_lerptag.h"

#define TEST_CLIENT		3	/* the living player, entity number == clientNum */
#define TEST_CORPSE_ENT	40	/* its corpse, a body-queue entity: number != clientNum */

#define TIER_LIVE		1	/* the living player's tier */
#define TIER_CORPSE		0	/* CG_Player forces this for anything on the body queue */

/* Distinguishable handles, so an assertion names which pose was read. */
#define HEAD_MODEL_LIVE		0x101
#define HEAD_MODEL_CORPSE	0x202
#define TORSO_MODEL			0x303
#define LEGS_MODEL			0x404

/* Full health puts CG_Player's damageState at 9; see the health ladder there. */
#define FULL_HEALTH_SLOT	9

static snapshot_t	testSnap;
static auraConfig_t	testAura[8];

static void seed_tier(clientInfo_t *ci, int tier, qhandle_t headModel) {
	/* [tier][0]=head, [tier][1]=torso, [tier][2]=legs, indexed by damage state.
	   CG_Player returns early on any of the three being absent. */
	ci->modelDamageState[tier][0][FULL_HEALTH_SLOT] = headModel;
	ci->modelDamageState[tier][1][FULL_HEALTH_SLOT] = TORSO_MODEL;
	ci->modelDamageState[tier][2][FULL_HEALTH_SLOT] = LEGS_MODEL;
	ci->auraConfig[tier] = &testAura[tier];
}

static void seed_entity(centity_t *cent, int entityNum, int tier) {
	memset(cent, 0, sizeof(*cent));
	cent->currentState.number = entityNum;
	cent->currentState.clientNum = TEST_CLIENT;
	cent->currentState.eType = ET_PLAYER;
	cent->currentState.tier = tier;
	/* health = current/total * 100; a zero total would divide by zero. */
	cent->currentState.attackPowerCurrent = 100;
	cent->currentState.attackPowerTotal = 100;
}

static void setup(void) {
	clientInfo_t *ci;

	memset(&cg, 0, sizeof(cg));
	memset(&cgs, 0, sizeof(cgs));
	memset(&testSnap, 0, sizeof(testSnap));
	memset(testAura, 0, sizeof(testAura));
	memset(cg_entities, 0, sizeof(cg_entities));

	/* CG_Player dereferences cg.snap->ps on entry. */
	cg.snap = &testSnap;
	testSnap.ps.clientNum = TEST_CLIENT;

	ci = &cgs.clientinfo[TEST_CLIENT];
	ci->infoValid = qtrue;
	seed_tier(ci, TIER_CORPSE, HEAD_MODEL_CORPSE);
	seed_tier(ci, TIER_LIVE, HEAD_MODEL_LIVE);

	fake_lerptag_reset();
}

/* Guards the premise: if a corpse ever stopped resolving to a different model
   than the living player, the assertions below would pass for the wrong reason. */
Test(cg_players, corpse_and_player_resolve_to_different_head_models) {
	setup();

	cr_assert_neq(cgs.clientinfo[TEST_CLIENT].modelDamageState[TIER_CORPSE][0][FULL_HEALTH_SLOT],
	              cgs.clientinfo[TEST_CLIENT].modelDamageState[TIER_LIVE][0][FULL_HEALTH_SLOT],
	              "the two tiers must differ for this suite to distinguish them");
}

Test(cg_players, player_tag_lookup_reads_its_own_pose) {
	centity_t		player;
	orientation_t	tagOrient;
	const fakeLerpTagCall_t *call;

	setup();
	seed_entity(&player, TEST_CLIENT, TIER_LIVE);

	CG_Player(&player);

	fake_lerptag_reset();
	cr_assert(CG_GetTagOrientationFromPlayerEntityHeadModel(&player, "tag_test", &tagOrient),
	          "the tag lookup should succeed for a rendered player");

	call = fake_lerptag_last();
	cr_assert_eq(call->calls, 1, "expected exactly one renderer tag lookup");
	cr_assert_eq(call->model, HEAD_MODEL_LIVE,
	             "a living player must read back its own head model");
}

Test(cg_players, corpse_tag_lookup_reads_its_own_pose_not_the_living_player) {
	centity_t		player, corpse;
	orientation_t	tagOrient;
	const fakeLerpTagCall_t *call;

	setup();
	seed_entity(&player, TEST_CLIENT, TIER_LIVE);
	seed_entity(&corpse, TEST_CORPSE_ENT, TIER_LIVE);

	/* Both get rendered, in the order the entity loop would. The living player
	   goes last so its pose is the most recent thing written. */
	CG_Player(&corpse);
	CG_Player(&player);

	fake_lerptag_reset();
	cr_assert(CG_GetTagOrientationFromPlayerEntityHeadModel(&corpse, "tag_test", &tagOrient),
	          "the tag lookup should succeed for a rendered corpse");

	call = fake_lerptag_last();
	cr_assert_eq(call->calls, 1, "expected exactly one renderer tag lookup");
	/* Naming the handle we got distinguishes the two ways this can fail:
	   HEAD_MODEL_LIVE means the read used clientNum and landed on the living
	   player's slot; 0 would mean the corpse's pose was never stored at all. */
	cr_assert_eq(call->model, HEAD_MODEL_CORPSE,
	             "a corpse must read back its own pose, not the living player's; "
	             "expected head model 0x%x, got 0x%x (0x%x is the living player's)",
	             HEAD_MODEL_CORPSE, call->model, HEAD_MODEL_LIVE);
}
