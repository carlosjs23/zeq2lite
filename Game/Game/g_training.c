#include "g_local.h"

// Training mode: what makes the rule engine live in a running game.
//
// g_rules.c is a leaf - it parses, matches and runs authored vectors and knows
// nothing about a player. Everything that reaches a playerState, a client slot
// or the console lives here, which is what keeps that unit testable and keeps
// this file small enough to read in one sitting.
//
// Three decisions the rest of the file rests on:
//
// Evaluation runs from the server frame, through ClientEndFrame, never from
// ClientThink. ClientThink runs once per usercmd and cl_maxpackets is archived
// and client-settable, so evaluating there would let a client set the server's
// per-client rule cost.
//
// A rule's actions run on the frame it BECOMES the best match for a client and
// not again while it stays one - an edge-trigger latch, cleared when the match
// changes. The latch is not the one-shot mechanism; content one-shots itself by
// granting a tag it also forbids, which stops the rule matching at all. Without
// the latch a still-matching rule would repeat at sv_fps, which for `say` means
// twenty server commands a second and a disconnected client.
//
// g_training 0 has to leave no trace. Nothing here is reached with the mode off
// and the tier ceiling reports every tier unlocked, so a server that does not
// want the mode plays exactly as it did before.

#define TRAINING_TAGS_FILE	"rules/tags.def"
#define TRAINING_RULES_FILE	"rules/training.rules"
#define TRAINING_FACTS_FILE	"rules/facts.def"

// Tags under this prefix belong to the shared world set rather than to the
// player who tripped the rule. The prefix is the only marker the language has,
// and it is the one every world tag in the plan already carries.
#define TRAINING_WORLD_PREFIX	"world."

// Untrained players keep the first transformation, so shipped content that
// unlocks tiers 2 and 3 is a ladder rather than a dead end.
#define TRAINING_BASE_TIER	1

static qboolean	trainingLive;

// ---------------------------------------------------------------- loading

void G_TrainingInit(void){
	trainingLive = qfalse;
	G_RulesReset();
	if(!g_training.integer){
		G_Printf("Training mode: off (g_training 0).\n");
		return;
	}
	if(!G_RulesLoad(TRAINING_TAGS_FILE,TRAINING_RULES_FILE)){
		// The error text is the product here, so it is printed whole and the
		// mode is switched off. A content typo must not take a server down;
		// ruletest and the Criterion suite are where it stays a hard failure.
		G_Printf("Training mode: disabled, content did not load.\n");
		G_Printf("%s\n",G_RulesError());
		G_RulesReset();
		return;
	}
	trainingLive = qtrue;
	G_Printf("Training mode: %i rule(s), %i test vector(s), %i tag(s) declared.\n",
		G_RulesCount(),G_RulesTestCount(),G_TagCount());
}

// ------------------------------------------------------------------ facts

// Every fact is either a direct playerState read or an accumulator advanced in
// the same pass. Facts live in the wiped half of gclient_t, so death resets the
// accumulators - which is what "stay airborne for 45 seconds" means.
static void refreshFacts(gentity_t *ent){
	gclient_t *client;
	playerState_t *ps;
	int *facts;
	int msec,previousPower,gravity;
	qboolean airborne,aura,raising;

	client = ent->client;
	ps = &client->ps;
	facts = client->facts;
	msec = level.time - level.previousTime;
	// Last frame's sample, still in the array until it is overwritten below.
	previousPower = facts[fPowerCurrent];

	airborne = ((ps->bitFlags & (usingJump | usingSoar)) || ps->groundEntityNum == ENTITYNUM_NONE) ? qtrue : qfalse;
	aura = (ps->eFlags & EF_AURA) ? qtrue : qfalse;
	// Powering up is an aura that is buying something: the aura alone is also a
	// transformation and a boost, neither of which is what a power-up lesson
	// means.
	raising = (aura && ps->powerLevel[plCurrent] > previousPower) ? qtrue : qfalse;

	facts[fAirborneTime] = G_RulesAdvance(facts[fAirborneTime],msec,airborne);
	facts[fAuraTime] = G_RulesAdvance(facts[fAuraTime],msec,aura);
	facts[fPowerRaiseTime] = G_RulesAdvance(facts[fPowerRaiseTime],msec,raising);

	facts[fPowerCurrent] = ps->powerLevel[plCurrent];
	facts[fFatigue] = ps->powerLevel[plFatigue];
	facts[fHealth] = ps->powerLevel[plHealth];
	facts[fPowerPercent] = ps->powerLevel[plMaximum] > 0 ?
		(int)(100.0f * ps->powerLevel[plCurrent] / ps->powerLevel[plMaximum]) : 0;
	facts[fTierCurrent] = ps->powerLevel[plTierCurrent];
	facts[fTierTotal] = ps->powerLevel[plTierTotal];
	// The base gravity, not gravity[2]: pmove drives that one to 12000 for the
	// length of a jump, and a gravity lesson that fired on every jump would be
	// measuring the jump.
	gravity = ps->gravity[0] ? ps->gravity[0] : PLAYER_BASE_GRAVITY;
	facts[fGravity] = gravity;
	facts[fStruggleEnergy] = ps->timers[tmStruggleEnergy];
	// Master triggers are Phase 2; until then nothing is ever near one.
	facts[fMasterNear] = 0;
}

// ---------------------------------------------------------------- actions

static void trainingSay(int clientNum,const char *text){
	trap_SendServerCommand(clientNum,va("print \"%s\n\"",text));
}

static void runAction(gentity_t *ent,const action_t *action){
	gclient_t *client;
	int clientNum;

	client = ent->client;
	clientNum = ent - g_entities;
	switch(action->type){
	case acGrant:
		if(!Q_strncmp(G_TagName(action->tag),TRAINING_WORLD_PREFIX,(int)strlen(TRAINING_WORLD_PREFIX))){
			G_TagSet(&g_worldTags,action->tag);
			break;
		}
		G_TagSet(&client->pers.tags,action->tag);
		break;
	case acRemove:
		if(!Q_strncmp(G_TagName(action->tag),TRAINING_WORLD_PREFIX,(int)strlen(TRAINING_WORLD_PREFIX))){
			G_TagClear(&g_worldTags,action->tag);
			break;
		}
		G_TagClear(&client->pers.tags,action->tag);
		break;
	case acSay:
		trainingSay(clientNum,action->text);
		break;
	case acObjective:
		Q_strncpyz(client->pers.objectiveText,action->text,sizeof(client->pers.objectiveText));
		client->pers.objectiveTrack = action->track;
		client->pers.objectiveGoal = action->value;
		// The HUD tracker is Phase 2. Saying it as well keeps the loop playable
		// through chat in the meantime, and costs one reliable command per
		// objective rather than per frame.
		trainingSay(clientNum,va("Objective: %s",action->text));
		break;
	case acSetGravity:
		// gravity[0] is the per-client base pmove falls back to; gravity[2] is
		// the working value pmove rewrites every move.
		client->ps.gravity[0] = action->value;
		break;
	case acUnlockTier:
		if(action->value > client->pers.unlockedTier){
			client->pers.unlockedTier = action->value;
		}
		break;
	}
}

// ------------------------------------------------------------- evaluation

void G_TrainingEndFrame(gentity_t *ent){
	const rule_t *rule;
	int index,i;

	if(!trainingLive){return;}
	if(!ent->client){return;}
	// A dummy is a client slot with nobody behind it, so a lesson aimed at one
	// would talk to an empty seat.
	if(ent->client->pers.isDummy){return;}

	refreshFacts(ent);
	rule = G_RulesMatch(ent->client->facts,g_worldFacts,&ent->client->pers.tags,&g_worldTags);
	index = rule ? G_RulesIndexOf(rule) : -1;
	if(!G_RulesLatch(&ent->client->ruleLatched,index)){return;}
	for(i=0;i<rule->numActions;i++){
		runAction(ent,&rule->actions[i]);
	}
}

// ------------------------------------------------------------ tier ceiling

// Training unlocks the ability to ascend; breakLimit still earns the ascension
// in every fight. Called from where g_tiers.c already decides an ascension is
// allowed, so a locked tier fails exactly like an unmet power requirement.
qboolean G_TrainingTierUnlocked(gclient_t *client,int tier){
	int ceiling;
	if(!trainingLive){return qtrue;}
	ceiling = client->pers.unlockedTier > TRAINING_BASE_TIER ? client->pers.unlockedTier : TRAINING_BASE_TIER;
	return tier <= ceiling ? qtrue : qfalse;
}
