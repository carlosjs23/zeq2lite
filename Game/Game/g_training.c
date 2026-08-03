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
#define TRAINING_MASTERS_FILE	"rules/masters.def"

// Tags under this prefix belong to the shared world set rather than to the
// player who tripped the rule. The prefix is the only marker the language has,
// and it is the one every world tag in the plan already carries.
#define TRAINING_WORLD_PREFIX	"world."

// Untrained players keep the first transformation, so shipped content that
// unlocks tiers 2 and 3 is a ladder rather than a dead end.
#define TRAINING_BASE_TIER	1

static qboolean	trainingLive;
static char	trainingMapName[MAX_QPATH];

// ---------------------------------------------------------------- loading

// rules/masters_<mapname>.def. The name is derived rather than configured so
// that a map either has placements or does not, with nothing to keep in step.
static const char *placementsFile(void){
	return va("rules/masters_%s.def",trainingMapName);
}

static int countPlaced(void){
	const master_t *master;
	int i,placed;
	placed = 0;
	for(i=0;i<G_MastersCount();i++){
		master = G_MastersGet(i);
		if(master->placed){placed++;}
	}
	return placed;
}

// Masters load BEFORE the rules, because the master names are what
// `masterNear is roshi` is validated against - installing them afterwards would
// mean every master name in the content was an unknown value at parse time.
static void loadMasters(void){
	const char *const *vocabulary;
	int count;

	G_MastersReset();
	G_RulesSetMasterVocabulary(NULL,0);
	if(!G_MastersLoadDef(TRAINING_MASTERS_FILE)){
		G_Printf("Training mode: no master vocabulary, masterNear keeps its built-in values.\n");
		return;
	}
	if(!G_MastersLoadPlacements(placementsFile())){
		G_Printf("Training mode: %s did not load, no masters are placed.\n",placementsFile());
		G_MastersReset();
		return;
	}
	vocabulary = G_MastersVocabulary(&count);
	G_RulesSetMasterVocabulary(vocabulary,count);
	G_Printf("Training mode: %i master(s) declared, %i placed on %s.\n",
		G_MastersCount(),countPlaced(),trainingMapName);
}

void G_TrainingInit(void){
	trainingLive = qfalse;
	G_RulesReset();
	trap_Cvar_VariableStringBuffer("mapname",trainingMapName,sizeof(trainingMapName));
	if(!g_training.integer){
		G_Printf("Training mode: off (g_training 0).\n");
		return;
	}
	loadMasters();
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

// ----------------------------------------------------------------- events
//
// Every training event that changes what a player owns goes to games.log as one
// line, in the shape the log already speaks - a label, a colon, then fields
// (`ClientConnect: 3`). Fields are positional and the arity is fixed per event,
// so a line parses by splitting on whitespace and the only quoted field is the
// last one:
//
//   Training: <clientNum> <key> progress-load <tags> <tier> <dropped>
//   Training: <clientNum> <key> progress-drop <tagName>
//   Training: <clientNum> <key> progress-save <tags> <tier>
//   Training: <clientNum> - progress-none
//
// The seam the plan asks for: the mod EMITS events and stores nothing beyond
// the flat file, so a later backend reading games.log is a sidecar rather than
// a change in here. `<key>` is `-` for a client that does not persist.
static void trainingEvent(int clientNum,const char *text){
	const char *key;

	key = level.clients[clientNum].pers.progressKey;
	G_LogPrintf("Training: %i %s %s\n",clientNum,key[0] ? key : "-",text);
}

// ------------------------------------------------------------ persistence

// Writes are event-driven rather than periodic: the only moments a save file
// can go stale are the edges where a rule granted something, and there are a
// handful of those per session. Nothing is written when nothing changed, so a
// player who trips no rule for an hour costs no writes at all.
static void saveProgress(int clientNum){
	gclient_t *client;
	progress_t p;

	client = level.clients + clientNum;
	if(!trainingLive){return;}
	if(!client->pers.progressKey[0]){return;}
	if(!client->pers.progressDirty){return;}
	memset(&p,0,sizeof(p));
	p.tags = client->pers.tags;
	p.unlockedTier = client->pers.unlockedTier;
	// A failed write leaves the client dirty on purpose, so disconnect and
	// shutdown try again rather than reporting a save that did not happen.
	if(!G_ProgressWrite(client->pers.progressKey,&p)){return;}
	client->pers.progressDirty = qfalse;
	trainingEvent(clientNum,va("progress-save %i %i",G_ProgressTagCount(&p),p.unlockedTier));
}

// The key is derived once, here, from the userinfo the client connected with.
// See g_progress.h for what keying on cl_guid is worth: it is trust-on-first-use
// and a modified client can name any file it likes.
void G_TrainingClientConnect(int clientNum,const char *userinfo){
	gclient_t *client;

	client = level.clients + clientNum;
	client->pers.progressKey[0] = 0;
	client->pers.progressDirty = qfalse;
	client->pers.progressLoaded = qfalse;
	if(!trainingLive){return;}
	if(G_ProgressKey(Info_ValueForKey(userinfo,"cl_guid"),client->pers.netname,
		client->pers.progressKey,sizeof(client->pers.progressKey))){
		return;
	}
	// No guid and a name that cleans to nothing. Said once, at connect, rather
	// than discovered as an absence of saves later.
	G_LogPrintf("Training: %i - progress-none\n",clientNum);
}

// ClientBegin also runs on a team change, where pers is preserved and the tags
// in it are newer than the file. Loading once per connection is what keeps a
// team change from rolling a session back to its last write.
void G_TrainingClientBegin(int clientNum){
	gclient_t *client;
	progress_t p;
	progressLoad_t report;
	int i;

	client = level.clients + clientNum;
	if(!trainingLive){return;}
	if(client->pers.progressLoaded){return;}
	client->pers.progressLoaded = qtrue;
	if(!client->pers.progressKey[0]){return;}
	if(!G_ProgressRead(client->pers.progressKey,&p,&report)){return;}
	client->pers.tags = p.tags;
	if(p.unlockedTier > client->pers.unlockedTier){
		client->pers.unlockedTier = p.unlockedTier;
	}
	for(i=0;i<report.named;i++){
		trainingEvent(clientNum,va("progress-drop %s",report.droppedNames[i]));
	}
	trainingEvent(clientNum,va("progress-load %i %i %i",report.restored,p.unlockedTier,report.dropped));
	client->pers.progressDirty = qfalse;
}

void G_TrainingClientDisconnect(int clientNum){
	saveProgress(clientNum);
}

// Called before G_ShutdownGame closes the log file, so the last writes of a
// map still have their lines in games.log.
void G_TrainingShutdown(void){
	int i;

	for(i=0;i<level.maxclients;i++){
		if(level.clients[i].pers.connected != CON_CONNECTED){continue;}
		saveProgress(i);
	}
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
	// A trigger volume without an entity: masters are a handful of spheres, and
	// testing them here costs one distance per master per client per frame
	// rather than a linked entity and a touch function.
	facts[fMasterNear] = G_MastersNearest(ps->origin);
}

// ---------------------------------------------------------------- actions

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
		// World tags belong to the round, not to a player, so they are neither
		// logged per client nor saved with one.
		if(!Q_strncmp(G_TagName(action->tag),TRAINING_WORLD_PREFIX,(int)strlen(TRAINING_WORLD_PREFIX))){
			G_TagSet(&g_worldTags,action->tag);
			break;
		}
		G_TagSet(&client->pers.tags,action->tag);
		client->pers.progressDirty = qtrue;
		break;
	case acRemove:
		if(!Q_strncmp(G_TagName(action->tag),TRAINING_WORLD_PREFIX,(int)strlen(TRAINING_WORLD_PREFIX))){
			G_TagClear(&g_worldTags,action->tag);
			break;
		}
		G_TagClear(&client->pers.tags,action->tag);
		client->pers.progressDirty = qtrue;
		break;
	case acSay:
		// One delivery. This also went out as a console print while the HUD had
		// nothing to draw it with; now that it does, the print was a second copy
		// of every line in the chat feed.
		trainingToast(clientNum,action->text);
		break;
	case acObjective:
		Q_strncpyz(client->pers.objectiveText,action->text,sizeof(client->pers.objectiveText));
		client->pers.objectiveTrack = action->track;
		client->pers.objectiveGoal = action->value;
		client->pers.objectiveId = G_RulesIndexOf(rule) + 1;
		client->pers.objectiveComplete = qfalse;
		trap_SendServerCommand(clientNum,va("trobj \"%s\" %i %i %i",action->text,
			client->pers.objectiveId,action->track,action->value));
		break;
	case acSetGravity:
		// gravity[0] is the per-client base pmove falls back to; gravity[2] is
		// the working value pmove rewrites every move.
		client->ps.gravity[0] = action->value;
		break;
	case acUnlockTier:
		if(action->value > client->pers.unlockedTier){
			client->pers.unlockedTier = action->value;
			client->pers.progressDirty = qtrue;
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
	return G_RulesProgress(value,goal);
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
		// The only frames a client can become dirty on are these, so the write
		// hangs off the latch rather than off the frame: a crash then loses at
		// most the lesson that was in flight.
		saveProgress(ent - g_entities);
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
	G_Printf("masters: %i declared, %i placed on %s (file %s)\n",
		G_MastersCount(),countPlaced(),trainingMapName,placementsFile());
	for(i=0;i<G_MastersCount();i++){
		const master_t *master = G_MastersGet(i);
		if(!master->placed){
			G_Printf("  %2i %-16s not placed on this map\n",master->id,master->name);
			continue;
		}
		G_Printf("  %2i %-16s at %.0f %.0f %.0f radius %.0f\n",master->id,master->name,
			master->origin[0],master->origin[1],master->origin[2],master->radius);
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
		G_Printf("  progress: %s%s\n",
			client->pers.progressKey[0] ? G_ProgressPath(client->pers.progressKey) : "not persisted",
			client->pers.progressDirty ? " (unwritten changes)" : "");
		G_Printf("  pers: objective=%i progress=%i master=%i (%s)\n",
			client->ps.persistant[PERS_TRAINING_OBJECTIVE],
			client->ps.persistant[PERS_TRAINING_PROGRESS],
			client->ps.persistant[PERS_TRAINING_MASTER],
			G_MastersName(client->ps.persistant[PERS_TRAINING_MASTER]));
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

// ------------------------------------------------------------ masterplace

// The in-game master editor, following the lens-flare editor's shape
// (CG_SaveLensFlareEntities_f in cg_consolecmds.c): stand where the thing
// belongs, drop it there, write the per-map file. Maps are not in this
// repository and re-BSP'ing to move a trigger is a bad authoring loop, so
// placement is authored from inside the running game.
//
// It lives in the game module rather than in cgame, which is the one deviation
// from that pattern and it is deliberate: the server is what reads the
// placements and what knows the authoritative player origin, so writing the
// file from the client would write it to the wrong side of a listen server and
// leave the server's own copy stale until a map restart.
//
// End to end, for an author:
//
//   1. Declare the master once in GameData/rules/masters.def:  master 1 roshi
//   2. Start the map with cheats on:  +set g_training 1 +set sv_cheats 1
//   3. Fly to where the master belongs and:  \masterplace roshi 320
//      (the radius is optional and defaults to MASTER_DEFAULT_RADIUS)
//   4. \masterlist to check what is placed and how far away you are
//   5. \mastersave writes rules/masters_<mapname>.def under fs_homepath
//   6. Copy that file into GameData/rules/ so zeq2build.sh stages it, and it
//      loads on the next map start.
//
// masterplace only moves a master that masters.def already declares, because
// the name is also the rule vocabulary: a master this command could invent
// would be a name no rule is allowed to mention.

static void masterPlace_f(gentity_t *ent){
	char name[MAX_MASTER_NAME],arg[MAX_TOKEN_CHARS];
	vec3_t origin;
	float radius;

	trap_Argv(1,name,sizeof(name));
	if(!name[0]){
		trap_SendServerCommand(ent-g_entities,"print \"usage: masterplace <name> [radius]\n\"");
		return;
	}
	trap_Argv(2,arg,sizeof(arg));
	radius = arg[0] ? atof(arg) : MASTER_DEFAULT_RADIUS;
	VectorCopy(ent->client->ps.origin,origin);
	if(!G_MastersPlace(name,origin,radius)){
		trap_SendServerCommand(ent-g_entities,va("print \"masterplace: %s\n\"",G_MastersError()));
		return;
	}
	trap_SendServerCommand(ent-g_entities,va("print \"masterplace: %s at %.0f %.0f %.0f radius %.0f - mastersave to keep it\n\"",
		name,origin[0],origin[1],origin[2],radius));
}

static void masterSave_f(gentity_t *ent){
	if(!G_MastersWrite(placementsFile(),trainingMapName)){
		trap_SendServerCommand(ent-g_entities,va("print \"mastersave: %s\n\"",G_MastersError()));
		return;
	}
	trap_SendServerCommand(ent-g_entities,va("print \"mastersave: wrote %s\n\"",placementsFile()));
}

static void masterList_f(gentity_t *ent){
	const master_t *master;
	vec3_t delta;
	int i;

	trap_SendServerCommand(ent-g_entities,va("print \"%i master(s) declared, %i placed on %s\n\"",
		G_MastersCount(),countPlaced(),trainingMapName));
	for(i=0;i<G_MastersCount();i++){
		master = G_MastersGet(i);
		if(!master->placed){
			trap_SendServerCommand(ent-g_entities,va("print \"  %i %s: not placed\n\"",master->id,master->name));
			continue;
		}
		VectorSubtract(ent->client->ps.origin,master->origin,delta);
		trap_SendServerCommand(ent-g_entities,va("print \"  %i %s: %.0f %.0f %.0f radius %.0f, %.0f away\n\"",
			master->id,master->name,master->origin[0],master->origin[1],master->origin[2],
			master->radius,VectorLength(delta)));
	}
}

// Authoring commands are cheat-gated for the same reason setviewpos is: they
// move the world, and a public server is not an editing session.
qboolean G_TrainingClientCommand(gentity_t *ent,const char *cmd){
	if(Q_stricmp(cmd,"masterplace") && Q_stricmp(cmd,"mastersave") && Q_stricmp(cmd,"masterlist")){
		return qfalse;
	}
	if(!g_cheats.integer){
		trap_SendServerCommand(ent-g_entities,"print \"Cheats are not enabled on this server.\n\"");
		return qtrue;
	}
	if(!Q_stricmp(cmd,"masterplace")){
		masterPlace_f(ent);
		return qtrue;
	}
	if(!Q_stricmp(cmd,"mastersave")){
		masterSave_f(ent);
		return qtrue;
	}
	masterList_f(ent);
	return qtrue;
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
