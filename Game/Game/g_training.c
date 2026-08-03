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
//
// Transport is split the way the plan splits it: STATE travels in persistant[]
// - three appended slots the client reads every snapshot for free - and EVENTS
// travel as server commands. MAX_RELIABLE_COMMANDS is 64 and overflowing it
// disconnects the client, so nothing here sends a command per frame; every
// command below is sent from the edge-triggered action pass or from a latch.

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

// The three event-shaped messages. Text is quoted so it survives tokenizing as
// one argument; everything else is a small integer.
//
//   trtoast "<text>"
//   trobj   "<text>" <objectiveId> <trackFactKey> <goal>
//   trdone  "<text>" <objectiveId>
//
// The client keeps the running numbers - which objective, how far through it,
// which master - out of persistant[], so none of these carry state a late
// joiner or a reconnecting client would miss.
static void trainingToast(int clientNum,const char *text){
	trap_SendServerCommand(clientNum,va("trtoast \"%s\"",text));
}

static void runAction(gentity_t *ent,const rule_t *rule,const action_t *action){
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
		// Two commands per line: the toast the HUD will draw, and the console
		// print that has carried this loop since Phase 1. The print stays until
		// the toast is on screen, because a lesson nobody can read is the
		// failure this whole mode exists to fix.
		trainingToast(clientNum,action->text);
		trainingSay(clientNum,action->text);
		break;
	case acObjective:
		Q_strncpyz(client->pers.objectiveText,action->text,sizeof(client->pers.objectiveText));
		client->pers.objectiveTrack = action->track;
		client->pers.objectiveGoal = action->value;
		client->pers.objectiveId = G_RulesIndexOf(rule) + 1;
		client->pers.objectiveComplete = qfalse;
		trap_SendServerCommand(clientNum,va("trobj \"%s\" %i %i %i",action->text,
			client->pers.objectiveId,action->track,action->value));
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

// Percent of the way to the objective's goal, quantized on the server. The
// plan's rule: milliseconds change every frame and would mark persistant[]
// dirty on every snapshot, while a percent changes about a hundred times per
// lesson and the bar interpolates locally.
static int objectiveProgress(const gclient_t *client){
	int key,value,goal;

	if(!client->pers.objectiveId || client->pers.objectiveGoal <= 0){return 0;}
	key = client->pers.objectiveTrack;
	goal = client->pers.objectiveGoal;
	if(key & RULE_WORLD_KEY){
		key &= ~RULE_WORLD_KEY;
		if(key < 0 || key >= fWorldFactCount){return 0;}
		value = g_worldFacts[key];
	}else{
		if(key < 0 || key >= fFactCount){return 0;}
		value = client->facts[key];
	}
	if(value <= 0){return 0;}
	if(value >= goal){return 100;}
	return (int)(100.0f * value / goal);
}

// The three state slots, written once per client per frame. Nothing here sends
// anything: persistant[] is delta-compressed by the engine, so a value that has
// not changed costs no bytes at all.
static void publishState(gentity_t *ent){
	gclient_t *client;
	int progress;

	client = ent->client;
	progress = objectiveProgress(client);
	client->ps.persistant[PERS_TRAINING_OBJECTIVE] = client->pers.objectiveId;
	client->ps.persistant[PERS_TRAINING_PROGRESS] = progress;
	client->ps.persistant[PERS_TRAINING_MASTER] = client->facts[fMasterNear];

	// Completion is an event, so it is latched rather than resent: the goal is
	// met once and the objective stays met until another one replaces it.
	if(progress >= 100 && !client->pers.objectiveComplete){
		client->pers.objectiveComplete = qtrue;
		trap_SendServerCommand(ent - g_entities,va("trdone \"%s\" %i",
			client->pers.objectiveText,client->pers.objectiveId));
	}
}

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
	if(G_RulesLatch(&ent->client->ruleLatched,index)){
		for(i=0;i<rule->numActions;i++){
			runAction(ent,rule,&rule->actions[i]);
		}
	}
	// After the actions, so an objective assigned this frame publishes its own
	// first progress sample rather than the previous lesson's.
	publishState(ent);
}

// ------------------------------------------------------------ tier ceiling

// Training unlocks the ability to ascend; breakLimit still earns the ascension
// in every fight. Called from where g_tiers.c already decides an ascension is
// allowed, so a locked tier fails exactly like an unmet power requirement.
static int tierCeiling(const gclient_t *client){
	return client->pers.unlockedTier > TRAINING_BASE_TIER ? client->pers.unlockedTier : TRAINING_BASE_TIER;
}

qboolean G_TrainingTierUnlocked(gclient_t *client,int tier){
	if(!trainingLive){return qtrue;}
	return tier <= tierCeiling(client) ? qtrue : qfalse;
}

// -------------------------------------------------------- console commands

// facts.def is generated from the C enum rather than maintained beside it,
// because a vocabulary file that can disagree with the code it describes is the
// silent no-op this language exists to prevent.
static void writeFactsDef(void){
	fileHandle_t f;
	const char *unit,*value;
	char line[256];
	int i,key,j;

	trap_FS_FOpenFile(TRAINING_FACTS_FILE,&f,FS_WRITE);
	if(!f){
		G_Printf("ruledump: could not open %s for writing\n",TRAINING_FACTS_FILE);
		return;
	}
	Com_sprintf(line,sizeof(line),"// Generated by `ruledump facts` from the C enum. Do not edit.\n\n");
	trap_FS_Write(line,(int)strlen(line),f);
	for(i=0;i<fFactCount + fWorldFactCount;i++){
		key = i < fFactCount ? i : ((i - fFactCount) | RULE_WORLD_KEY);
		unit = G_RulesFactUnit(key);
		Com_sprintf(line,sizeof(line),"fact  %s%s",key & RULE_WORLD_KEY ? "world " : "",G_RulesFactName(key));
		if(unit){
			Q_strcat(line,sizeof(line),va("  unit %s",unit));
		}
		for(j=0;;j++){
			value = G_RulesFactValue(key,j);
			if(!value){break;}
			Q_strcat(line,sizeof(line),va("  %s",value));
		}
		Q_strcat(line,sizeof(line),"\n");
		trap_FS_Write(line,(int)strlen(line),f);
	}
	trap_FS_FCloseFile(f);
	G_Printf("ruledump: wrote %i fact(s) to %s\n",fFactCount + fWorldFactCount,TRAINING_FACTS_FILE);
}

static void dumpRules(void){
	const rule_t *rule;
	gclient_t *client;
	int i,j;

	G_Printf("rules loaded: %i from %s\n",G_RulesCount(),TRAINING_RULES_FILE);
	for(i=0;i<G_RulesCount();i++){
		rule = G_RulesGet(i);
		G_Printf("  %2i %-28s %i criteria, %i actions\n",i,rule->name,rule->numCriteria,rule->numActions);
	}
	G_Printf("tags declared: %i of %i (%i bits per prefix group)\n",G_TagCount(),MAX_TAG_BITS,TAG_GROUP_BITS);
	for(i=0;i<G_TagCount();i++){
		if(!G_TagName(i)[0]){continue;}
		G_Printf("  %3i %s\n",i,G_TagName(i));
	}
	G_Printf("world facts:");
	for(i=0;i<fWorldFactCount;i++){
		G_Printf(" %s=%i",G_RulesFactName(i | RULE_WORLD_KEY),g_worldFacts[i]);
	}
	G_Printf("\n");
	for(i=0;i<level.maxclients;i++){
		client = &level.clients[i];
		if(client->pers.connected != CON_CONNECTED){continue;}
		G_Printf("client %i (%s) tier ceiling %i, objective '%s'\n",i,client->pers.netname,
			tierCeiling(client),client->pers.objectiveText);
		G_Printf("  facts:");
		for(j=0;j<fFactCount;j++){
			G_Printf(" %s=%i",G_RulesFactName(j),client->facts[j]);
		}
		G_Printf("\n  tags:");
		for(j=0;j<G_TagCount();j++){
			if(G_TagTest(&client->pers.tags,j)){G_Printf(" %s",G_TagName(j));}
		}
		G_Printf("\n");
	}
}

static void runRuleTests(void){
	const ruleTest_t *test;
	char err[MAX_RULE_ERROR];
	int i,passed,failed;

	if(!G_RulesTestCount()){
		G_Printf("ruletest: the loaded content carries no vectors\n");
		return;
	}
	passed = 0;
	failed = 0;
	for(i=0;i<G_RulesTestCount();i++){
		test = G_RulesGetTest(i);
		if(G_RulesRunTest(i,err,sizeof(err))){
			passed++;
			G_Printf("  pass  %s\n",test->name);
			continue;
		}
		failed++;
		G_Printf("  FAIL  %s\n        %s\n",test->name,err);
	}
	G_Printf("ruletest: %i passed, %i failed, %i total\n",passed,failed,G_RulesTestCount());
}

qboolean G_TrainingConsoleCommand(const char *cmd){
	char arg[MAX_TOKEN_CHARS];

	if(!Q_stricmp(cmd,"ruledump")){
		if(!trainingLive){
			G_Printf("ruledump: training mode is off\n");
			return qtrue;
		}
		trap_Argv(1,arg,sizeof(arg));
		if(!Q_stricmp(arg,"facts")){
			writeFactsDef();
			return qtrue;
		}
		dumpRules();
		return qtrue;
	}
	if(!Q_stricmp(cmd,"ruletest")){
		if(!trainingLive){
			G_Printf("ruletest: training mode is off\n");
			return qtrue;
		}
		runRuleTests();
		return qtrue;
	}
	return qfalse;
}
